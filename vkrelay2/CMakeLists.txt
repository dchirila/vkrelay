# vkrelay2 bootstrap build.
#
# Targets:
#   vkrelay2_common      static library: portable + per-platform process glue
#   vkrelay2-supervisor  long-lived daemon; --self-test, --list-gpus
#   vkrelay2-worker      disposable worker; emits heartbeats
#   vkrelay2-launch      launch helper: builds/round-trips launch descriptors
#   vkrelay2-argv-echo   canary that echoes argv/env/cwd as JSON
#   plus CTest unit + integration + shell smoke tests
cmake_minimum_required(VERSION 3.20)
project(vkrelay2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()

option(VKRELAY2_WERROR "Treat warnings as errors for vkrelay2 targets" ON)

# Warnings policy applied only to vkrelay2 targets.
add_library(vkrelay2_warnings INTERFACE)
if(MSVC)
    target_compile_options(vkrelay2_warnings INTERFACE /W4 /permissive-)
    if(VKRELAY2_WERROR)
        target_compile_options(vkrelay2_warnings INTERFACE /WX)
    endif()
    target_compile_definitions(vkrelay2_warnings INTERFACE
        _CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
else()
    target_compile_options(vkrelay2_warnings INTERFACE -Wall -Wextra -Wpedantic)
    if(VKRELAY2_WERROR)
        target_compile_options(vkrelay2_warnings INTERFACE -Werror)
    endif()
endif()

# ---------------------------------------------------------------------------
# vkrelay2_common
# ---------------------------------------------------------------------------
set(VKRELAY2_COMMON_SOURCES
    common/util/json.cpp
    common/display/display_layout.cpp
    common/logging/logging.cpp
    common/argv/argv_split.cpp
    common/argv/command_line.cpp
    common/protocol/launch_descriptor.cpp
    common/protocol/gpu.cpp
    common/protocol/smoke_matrix.cpp
    common/protocol/worker_lifecycle.cpp
    common/protocol/wire.cpp
    common/protocol/messages.cpp
    common/protocol/ids.cpp
    common/transport/transport.cpp
    common/transport/tcp.cpp
    common/control/control_service.cpp
    common/control/daemon_endpoint.cpp
    common/vkrpc/rpc.cpp
    common/vkrpc/vulkan_session.cpp
    common/sidecar/sidecar_session.cpp
    common/process/process.cpp)

if(WIN32)
    list(APPEND VKRELAY2_COMMON_SOURCES common/process/process_win.cpp)
else()
    list(APPEND VKRELAY2_COMMON_SOURCES common/process/process_posix.cpp)
endif()

add_library(vkrelay2_common STATIC ${VKRELAY2_COMMON_SOURCES})
target_include_directories(vkrelay2_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(vkrelay2_common PUBLIC vkrelay2_warnings)
# Position-independent so the static lib can be linked into the Linux Vulkan ICD shared
# object (no effect on the Windows/MSVC build or on linking into executables).
set_target_properties(vkrelay2_common PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(vkrelay2_common PUBLIC Threads::Threads)
endif()

# ---------------------------------------------------------------------------
# Executables
# ---------------------------------------------------------------------------
# Supervisor logic as a library so the executable and the integration tests
# share one build of it.
add_library(vkrelay2_supervisor STATIC
    windows/supervisor/display_layout_probe.cpp
    windows/supervisor/display_snapshot_cache.cpp
    windows/supervisor/self_test.cpp
    windows/supervisor/gpu_probe.cpp
    windows/supervisor/worker_supervisor.cpp)
target_link_libraries(vkrelay2_supervisor PUBLIC vkrelay2_common vkrelay2_warnings)
if(WIN32)
    target_link_libraries(vkrelay2_supervisor PUBLIC Shcore)
endif()

add_executable(vkrelay2-supervisor windows/supervisor/main.cpp)
target_link_libraries(vkrelay2-supervisor PRIVATE vkrelay2_supervisor vkrelay2_warnings)

add_executable(vkrelay2-worker windows/worker/main.cpp)
target_link_libraries(vkrelay2-worker PRIVATE vkrelay2_common vkrelay2_warnings)

# Real (loader-backed) Vulkan backend for the worker.
# Worker-only -- the supervisor never loads a Vulkan driver. Gated on WIN32 + the
# Vulkan SDK so the dual-platform-green mock path is unaffected: WSL never links
# the loader; a Windows box without the SDK builds the mock-only worker. When
# present, the worker gains a real backend selectable via --vulkan-backend real.
find_package(Vulkan QUIET)
if(WIN32 AND Vulkan_FOUND)
    # Real (loader-backed) Vulkan as a worker-only static library, so the worker AND the
    # in-process Vulkan tests link one build of it. VKRELAY2_HAVE_VULKAN + the Vulkan link
    # are PUBLIC, so the worker (which compiles main.cpp under that define) and the tests
    # inherit them.
    add_library(vkrelay2_worker_vulkan STATIC
        windows/worker/real_vulkan_backend.cpp
        windows/worker/real_gpu_probe.cpp
        windows/worker/windowing/window_thread.cpp)
    target_link_libraries(vkrelay2_worker_vulkan
        PUBLIC vkrelay2_common vkrelay2_warnings Vulkan::Vulkan)
    target_compile_definitions(vkrelay2_worker_vulkan PUBLIC VKRELAY2_HAVE_VULKAN)
    target_link_libraries(vkrelay2-worker PRIVATE vkrelay2_worker_vulkan)
    message(STATUS "vkrelay2: Vulkan SDK found; worker gets the real backend (--vulkan-backend real)")

    # Geometry-shot harness (on-screen verification): drives a REAL visible
    # placeholder window through a sidecar-authored resize (no Vulkan device), stepping on stdin so
    # scripts/dev/run_resize_shot.ps1 can capture_window.ps1 the on-screen client before/after and
    # assert it grew. A dev/verification helper (not a ctest).
    add_executable(vkrelay2-geometry-shot windows/worker/tools/geometry_shot.cpp)
    target_link_libraries(vkrelay2-geometry-shot PRIVATE vkrelay2_worker_vulkan vkrelay2_warnings)
else()
    message(STATUS "vkrelay2: no Vulkan SDK (or non-Windows); worker is mock-backend only")
endif()

# ---------------------------------------------------------------------------
# Linux Vulkan ICD -- the app-facing driver the Khronos loader
# dlopens inside WSL; forwards the RPC envelope to the worker (worker-present). Built
# only on Linux with the Vulkan headers present (it needs vulkan.h + vk_icd.h but does
# NOT link the loader -- it IS a driver). A generated manifest names the .so for
# VK_ICD_FILENAMES. The clear canary links the loader + libxcb to exercise it.
if(NOT WIN32 AND Vulkan_FOUND)
    add_library(vkrelay2_icd SHARED linux/icd/icd.cpp)
    target_link_libraries(vkrelay2_icd PRIVATE vkrelay2_common vkrelay2_warnings)
    target_include_directories(vkrelay2_icd PRIVATE ${Vulkan_INCLUDE_DIRS})
    set_target_properties(vkrelay2_icd PROPERTIES OUTPUT_NAME vulkan_vkrelay2)
    # Manifest with the .so's absolute path (the loader reads it via VK_ICD_FILENAMES /
    # VK_DRIVER_FILES; the launcher pins it).
    file(GENERATE
        OUTPUT "${CMAKE_BINARY_DIR}/vkrelay2_icd.json"
        CONTENT "{\n    \"file_format_version\": \"1.0.0\",\n    \"ICD\": {\n        \"library_path\": \"$<TARGET_FILE:vkrelay2_icd>\",\n        \"api_version\": \"1.1.0\"\n    }\n}\n")
    message(STATUS "vkrelay2: Linux Vulkan ICD enabled (libvulkan_vkrelay2.so + manifest)")

    # Clear-only native Vulkan canary: links the real loader (Vulkan::Vulkan) + libxcb to
    # drive acquire->record(clear)->submit->present through whichever ICD the loader selects
    # (our manifest, pinned by the launcher). Optional on libxcb being present.
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(XCB QUIET xcb)
        # the sidecar's XComposite chrome capture needs libxcb-composite. Probed SEPARATELY
        # from base xcb so a missing package disables chrome capture with a clear reason -- it never
        # breaks the sidecar (lifecycle forwarding) or the protocol, which link only core xcb.
        pkg_check_modules(XCB_COMPOSITE QUIET xcb-composite)
        # XShape bounds non-rectangular chrome windows (xeyes/oclock). XComposite still exposes a
        # rectangular backing pixmap, so the capture path needs xcb-shape to mask storage outside
        # the current bounding region instead of forwarding stale/undefined pixels.
        pkg_check_modules(XCB_SHAPE QUIET xcb-shape)
        # the sidecar's input injection needs libxcb-xtest (xcb_test_fake_input). Probed
        # SEPARATELY, same as composite: absent -> pointer/key/wheel injection compiles out, but the
        # sidecar still does AND focus/close (xcb_set_input_focus / WM_DELETE_WINDOW are core
        # xcb, never gated on XTEST).
        pkg_check_modules(XCB_XTEST QUIET xcb-xtest)
        # the sidecar's cursor capture needs libxcb-xfixes (XFixes cursor image/notify).
        # Probed SEPARATELY: absent -> cursor compiles out, the sidecar still does -1.
        pkg_check_modules(XCB_XFIXES QUIET xcb-xfixes)
        # the GL readback canary needs GLX/Xlib (libgl + libx11 dev). Probed SEPARATELY, same
        # pattern: absent -> only this canary is skipped with a clear reason; nothing else changes.
        pkg_check_modules(GL_CANARY QUIET gl x11)
    endif()
    if(XCB_FOUND)
        add_executable(vkrelay2-canary linux/canary/canary.cpp)
        target_link_libraries(vkrelay2-canary PRIVATE vkrelay2_warnings Vulkan::Vulkan)
        target_include_directories(vkrelay2-canary PRIVATE ${Vulkan_INCLUDE_DIRS} ${XCB_INCLUDE_DIRS})
        target_link_libraries(vkrelay2-canary PRIVATE ${XCB_LIBRARIES})
        target_compile_definitions(vkrelay2-canary PRIVATE VK_USE_PLATFORM_XCB_KHR)
        message(STATUS "vkrelay2: clear canary enabled (vkrelay2-canary)")

        # Compute canary: surfaceless native-Vulkan compute through the loader against
        # the REAL daemon (run_compute_smoke.sh) -- pipeline + descriptors + push constants +
        # dispatch + buffer barrier + copy-out readback, asserted byte-exactly. No xcb needed.
        add_executable(vkrelay2-compute-canary linux/canary/compute_canary.cpp)
        target_link_libraries(vkrelay2-compute-canary PRIVATE vkrelay2_warnings Vulkan::Vulkan)
        target_include_directories(vkrelay2-compute-canary
                                   PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${Vulkan_INCLUDE_DIRS})
        message(STATUS "vkrelay2: compute canary enabled (vkrelay2-compute-canary)")

        # Vulkan-1.3 opener canary: a surfaceless native Vulkan program run on the
        # NATIVE lane (run_vk13_eds_smoke.sh via --frontend vulkan13) that proves VK_EXT_extended_
        # dynamic_state is exposed + a dynamic vkCmdSetCullModeEXT controls rasterization end to end
        # (offscreen draw + pixel readback). Version-adaptive: passes at the Vulkan 1.3 canary's 1.3 too.
        add_executable(vkrelay2-vk13-eds-canary linux/canary/vk13_eds_canary.cpp)
        target_link_libraries(vkrelay2-vk13-eds-canary PRIVATE vkrelay2_warnings Vulkan::Vulkan)
        target_include_directories(vkrelay2-vk13-eds-canary
                                   PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${Vulkan_INCLUDE_DIRS})
        message(STATUS "vkrelay2: vk13 EDS canary enabled (vkrelay2-vk13-eds-canary)")

        # Dynamic-rendering canary: a surfaceless native Vulkan program run on the
        # NATIVE lane (run_vk13_dynrender_smoke.sh via --frontend vulkan13) that proves
        # VK_KHR_dynamic_rendering is exposed + a NULL-renderpass pipeline with vkCmdBegin/EndRendering
        # KHR clears and draws end to end (offscreen draw + pixel readback: corner == clear color,
        # center drew). Version-adaptive: passes at the Vulkan 1.3 canary's 1.3 too (maintenance4 then served).
        add_executable(vkrelay2-vk13-dynrender-canary linux/canary/vk13_dynrender_canary.cpp)
        target_link_libraries(vkrelay2-vk13-dynrender-canary PRIVATE vkrelay2_warnings
                                                                     Vulkan::Vulkan)
        target_include_directories(vkrelay2-vk13-dynrender-canary
                                   PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${Vulkan_INCLUDE_DIRS})
        message(STATUS "vkrelay2: vk13 dynamic-rendering canary enabled "
                       "(vkrelay2-vk13-dynrender-canary)")

        # Core-1.0 synchronization canary: a surfaceless native Vulkan program
        # run on the DEFAULT lane (run_vk_event_smoke.sh) that proves vkGetFenceStatus + the VkEvent
        # object model (create/destroy, host status/set/reset) + the sync1 command events reach the
        # real GPU -- host + device event set, and a vkCmdWaitEvents-guarded clear/copy readback.
        # No extension/rollup probes (events are core 1.0; nothing is advertised).
        add_executable(vkrelay2-vk-event-canary linux/canary/vk_event_canary.cpp)
        target_link_libraries(vkrelay2-vk-event-canary PRIVATE vkrelay2_warnings Vulkan::Vulkan)
        target_include_directories(vkrelay2-vk-event-canary
                                   PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${Vulkan_INCLUDE_DIRS})
        message(STATUS "vkrelay2: vk event canary enabled (vkrelay2-vk-event-canary)")

        # synchronization2 canary: a surfaceless native Vulkan program on the
        # NATIVE lane (run_vk13_sync2_smoke.sh via --frontend vulkan13) that proves the whole
        # VK_KHR_synchronization2 command family serves with faithful 64-bit masks -- a compute
        # read-after-write ACROSS vkCmdPipelineBarrier2, vkCmdWriteTimestamp2, vkCmdSetEvent2, all
        # submitted via vkQueueSubmit2. Reuses the compute SPV (tests/ include).
        add_executable(vkrelay2-vk13-sync2-canary linux/canary/vk13_sync2_canary.cpp)
        target_link_libraries(vkrelay2-vk13-sync2-canary PRIVATE vkrelay2_warnings Vulkan::Vulkan)
        target_include_directories(vkrelay2-vk13-sync2-canary
                                   PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${Vulkan_INCLUDE_DIRS})
        message(STATUS "vkrelay2: vk13 synchronization2 canary enabled "
                       "(vkrelay2-vk13-sync2-canary)")

        # bufferDeviceAddress canary (Vulkan 1.2 BDA support): a surfaceless native Vulkan
        # program on the NATIVE lane (run_vk13_bda_smoke.sh via --frontend vulkan13) that proves
        # the raw-GPU-pointer contract -- a DEVICE_ADDRESS allocation binds a SHADER_DEVICE_ADDRESS
        # buffer, vkGetBufferDeviceAddress returns the real host VA, and a descriptorless
        # buffer-reference compute shader reads AND writes through it (tests/bda_spv.h).
        add_executable(vkrelay2-vk13-bda-canary linux/canary/vk13_bda_canary.cpp)
        target_link_libraries(vkrelay2-vk13-bda-canary PRIVATE vkrelay2_warnings Vulkan::Vulkan)
        target_include_directories(vkrelay2-vk13-bda-canary
                                   PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${Vulkan_INCLUDE_DIRS})
        message(STATUS "vkrelay2: vk13 bufferDeviceAddress canary enabled "
                       "(vkrelay2-vk13-bda-canary)")

        # descriptorIndexing canary (buffer-only bindless support): a surfaceless
        # native Vulkan program on the NATIVE lane (run_vk13_di_smoke.sh via --frontend vulkan13)
        # that proves the served subset -- served bits TRUE / aggregate + deferred FALSE + reject,
        # the host's layout-support answer, and the record -> update-after-bind
        # (destroy-old/repoint) -> submit crux byte-exact through the loader (tests/di_spv.h).
        add_executable(vkrelay2-vk13-di-canary linux/canary/vk13_di_canary.cpp)
        target_link_libraries(vkrelay2-vk13-di-canary PRIVATE vkrelay2_warnings Vulkan::Vulkan)
        target_include_directories(vkrelay2-vk13-di-canary
                                   PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${Vulkan_INCLUDE_DIRS})
        message(STATUS "vkrelay2: vk13 descriptorIndexing canary enabled "
                       "(vkrelay2-vk13-di-canary)")

        # Vulkan 1.3 support canary: the honest apiVersion-1.3 device end to end --
        # required features + unserved masks/rejects, tool properties, private data,
        # maintenance4 parity, cache control, subgroup size, an inline-uniform-block dispatch,
        # the core-name dynamic-state surface, featureless-core DR + sync2, copy_commands2, and
        # resolve2 (run_vk13_finale_smoke.sh via --frontend vulkan13; tests/iub_spv.h).
        add_executable(vkrelay2-vk13-finale-canary linux/canary/vk13_finale_canary.cpp)
        target_link_libraries(vkrelay2-vk13-finale-canary PRIVATE vkrelay2_warnings Vulkan::Vulkan)
        target_include_directories(vkrelay2-vk13-finale-canary
                                   PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${Vulkan_INCLUDE_DIRS})
        message(STATUS "vkrelay2: vk13 Vulkan 1.3 canary enabled (vkrelay2-vk13-finale-canary)")

        # hostQueryReset canary (required-feature audit): a surfaceless native Vulkan
        # program (run_vk13_hostquery_smoke.sh) proving the DEVICE-level vkResetQueryPool is served
        # -- the proc resolves and a HOST-side reset toggles a timestamp query's availability
        # available -> unavailable on the real GPU. Core 1.2, so it runs on either lane.
        add_executable(vkrelay2-vk13-hostquery-canary linux/canary/vk13_hostquery_canary.cpp)
        target_link_libraries(vkrelay2-vk13-hostquery-canary PRIVATE vkrelay2_warnings
                              Vulkan::Vulkan)
        target_include_directories(vkrelay2-vk13-hostquery-canary
                                   PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${Vulkan_INCLUDE_DIRS})
        message(STATUS "vkrelay2: vk13 hostQueryReset canary enabled "
                       "(vkrelay2-vk13-hostquery-canary)")

        # Required-feature AUDIT canary (required-feature audit): the machine-checked
        # conformance invariant -- the relay must never report apiVersion >= 1.3 while a required
        # feature (multiview) is advertise-then-failed. Reports the f10/f11/f12/f13 required matrix,
        # probes whether a viewMask render pass 2 is served, and asserts multiview-unserved => the
        # device stays below 1.3 (run_vk13_required_audit_smoke.sh). GREEN now (honest 1.2, multiview
        # pending) + after (1.3, multiview served); RED only on an advertise-then-fail regression.
        add_executable(vkrelay2-vk13-required-audit-canary
                       linux/canary/vk13_required_audit_canary.cpp)
        target_link_libraries(vkrelay2-vk13-required-audit-canary PRIVATE vkrelay2_warnings
                              Vulkan::Vulkan)
        target_include_directories(vkrelay2-vk13-required-audit-canary
                                   PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${Vulkan_INCLUDE_DIRS})
        message(STATUS "vkrelay2: vk13 required-feature audit canary enabled "
                       "(vkrelay2-vk13-required-audit-canary)")

        # Multiview SERVE-PROOF canary (required-feature audit): the release gate behind
        # flipping kRelayServesMultiview. Renders a gl_ViewIndex-colored full-screen triangle into a
        # 2-layer image (view 0 = red, view 1 = green) through BOTH a vkCreateRenderPass2 viewMask
        # pass AND a vkCmdBeginRenderingKHR viewMask scope, then reads back each layer -- proving
        # multiview actually runs per-view on the host, not a silent single-view fallback
        # (run_vk13_multiview_smoke.sh). Must PASS before flips the served flag.
        add_executable(vkrelay2-vk13-multiview-canary linux/canary/vk13_multiview_canary.cpp)
        target_link_libraries(vkrelay2-vk13-multiview-canary PRIVATE vkrelay2_warnings
                              Vulkan::Vulkan)
        target_include_directories(vkrelay2-vk13-multiview-canary
                                   PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${Vulkan_INCLUDE_DIRS})
        message(STATUS "vkrelay2: vk13 multiview serve-proof canary enabled "
                       "(vkrelay2-vk13-multiview-canary)")

        # Caps-convergence canary: drives the ICD's surface-caps cache end to
        # end against the MOCK daemon + real sidecar (run_caps_smoke.sh) -- guest resize ->
        # honest OUT_OF_DATE -> the next caps query must observe the new extent uncached.
        add_executable(vkrelay2-caps-canary linux/canary/caps_canary.cpp)
        target_link_libraries(vkrelay2-caps-canary PRIVATE vkrelay2_warnings Vulkan::Vulkan)
        target_include_directories(vkrelay2-caps-canary PRIVATE ${Vulkan_INCLUDE_DIRS}
                                                                ${XCB_INCLUDE_DIRS})
        target_link_libraries(vkrelay2-caps-canary PRIVATE ${XCB_LIBRARIES})
        target_compile_definitions(vkrelay2-caps-canary PRIVATE VK_USE_PLATFORM_XCB_KHR)
        message(STATUS "vkrelay2: caps canary enabled (vkrelay2-caps-canary)")

        # Draw canary: the deterministic on-screen artifact -- drives the whole
        # bufferless-triangle draw spine (image views / SPIR-V shaders / render pass / framebuffers /
        # pipeline / draw) through the loader + our ICD. Includes the repo root for the shared
        # pre-compiled SPIR-V header (tests/triangle_spv.h).
        add_executable(vkrelay2-triangle linux/canary/triangle_canary.cpp)
        target_link_libraries(vkrelay2-triangle PRIVATE vkrelay2_warnings Vulkan::Vulkan)
        target_include_directories(vkrelay2-triangle
                                   PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${Vulkan_INCLUDE_DIRS}
                                           ${XCB_INCLUDE_DIRS})
        target_link_libraries(vkrelay2-triangle PRIVATE ${XCB_LIBRARIES})
        target_compile_definitions(vkrelay2-triangle PRIVATE VK_USE_PLATFORM_XCB_KHR)
        message(STATUS "vkrelay2: triangle canary enabled (vkrelay2-triangle)")

        # VBO canary: the on-screen artifact for the memory-visibility spine --
        # a persistently-mapped vertex buffer (NO explicit flush) drives the triangle, so the picture
        # proves the coherent-flush-at-submit rule. Includes the repo root for tests/vbo_spv.h.
        add_executable(vkrelay2-vbo linux/canary/vbo_canary.cpp)
        target_link_libraries(vkrelay2-vbo PRIVATE vkrelay2_warnings Vulkan::Vulkan)
        target_include_directories(vkrelay2-vbo PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
                                                        ${Vulkan_INCLUDE_DIRS} ${XCB_INCLUDE_DIRS})
        target_link_libraries(vkrelay2-vbo PRIVATE ${XCB_LIBRARIES})
        target_compile_definitions(vkrelay2-vbo PRIVATE VK_USE_PLATFORM_XCB_KHR)
        message(STATUS "vkrelay2: VBO canary enabled (vkrelay2-vbo)")

        # vertex-attr-divisor canary: create-only, HEADLESS (no surface/swapchain). Proves a graphics
        # pipeline whose vertex-input state chains VkPipelineVertexInputDivisorStateCreateInfoEXT
        # (VK_EXT_vertex_attribute_divisor) creates on the real GPU -- the pNext the ICD used to reject
        # wholesale, which Blender/zink hit. Reuses tests/vbo_spv.h (repo root include).
        add_executable(vkrelay2-vertex-divisor linux/canary/vertex_divisor_canary.cpp)
        target_link_libraries(vkrelay2-vertex-divisor PRIVATE vkrelay2_warnings Vulkan::Vulkan)
        target_include_directories(vkrelay2-vertex-divisor
                                   PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${Vulkan_INCLUDE_DIRS})
        message(STATUS "vkrelay2: vertex-divisor canary enabled (vkrelay2-vertex-divisor)")

        # UBO canary: the on-screen artifact for the descriptor / per-frame UBO
        # surface -- a persistently-mapped uniform buffer rewritten every frame (NO explicit flush),
        # bound via a descriptor set, so the spinning triangle proves both the descriptor path and the
        # per-frame UBO write reaching the GPU. Includes the repo root for tests/ubo_spv.h.
        add_executable(vkrelay2-ubo linux/canary/ubo_canary.cpp)
        target_link_libraries(vkrelay2-ubo PRIVATE vkrelay2_warnings Vulkan::Vulkan)
        target_include_directories(vkrelay2-ubo PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
                                                        ${Vulkan_INCLUDE_DIRS} ${XCB_INCLUDE_DIRS})
        target_link_libraries(vkrelay2-ubo PRIVATE ${XCB_LIBRARIES})
        target_compile_definitions(vkrelay2-ubo PRIVATE VK_USE_PLATFORM_XCB_KHR)
        message(STATUS "vkrelay2: UBO canary enabled (vkrelay2-ubo)")

        # Cube canary: the literal-vkcube-shaped on-screen artifact -- a
        # bufferless spinning cube (geometry inside the UBO via gl_VertexIndex; draw of 36), a
        # 2-binding (UBO + combined-image-sampler) set, a D16_UNORM depth buffer, and a sampled
        # texture uploaded via vkcube's runtime linear-vs-staging path choice. The deterministic
        # in-repo counterpart to LunarG's vkcube (run_vkcube.sh drives the real binary). Includes the
        # repo root for tests/cube_spv.h + tests/cube_geometry.h.
        add_executable(vkrelay2-cube linux/canary/cube_canary.cpp)
        target_link_libraries(vkrelay2-cube PRIVATE vkrelay2_warnings Vulkan::Vulkan)
        target_include_directories(vkrelay2-cube PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
                                                         ${Vulkan_INCLUDE_DIRS} ${XCB_INCLUDE_DIRS})
        target_link_libraries(vkrelay2-cube PRIVATE ${XCB_LIBRARIES})
        target_compile_definitions(vkrelay2-cube PRIVATE VK_USE_PLATFORM_XCB_KHR)
        message(STATUS "vkrelay2: cube canary enabled (vkrelay2-cube)")

        # Sidecar: the X11 WM / geometry-authority process. Links the shared
        # transport/protocol (vkrelay2_common) + core libxcb only -- it does NOT link Vulkan (it is
        # not a GPU client). Colocated under the libxcb check; the extension-presence probe uses core
        # xcb_query_extension by name, so no per-extension lib is linked.
        add_executable(vkrelay2-sidecar linux/sidecar/sidecar_main.cpp)
        target_link_libraries(vkrelay2-sidecar PRIVATE vkrelay2_common vkrelay2_warnings)
        target_include_directories(vkrelay2-sidecar PRIVATE ${XCB_INCLUDE_DIRS})
        target_link_libraries(vkrelay2-sidecar PRIVATE ${XCB_LIBRARIES})
        # enable the XComposite chrome-capture path ONLY when libxcb-composite is present.
        # Absent -> the sidecar still builds + runs (lifecycle forwarding, the readiness barrier,
        # the WM pass-through), but logs "chrome capture disabled" and ships no PaintChrome.
        if(XCB_COMPOSITE_FOUND)
            target_compile_definitions(vkrelay2-sidecar PRIVATE VKRELAY2_HAVE_XCB_COMPOSITE)
            target_include_directories(vkrelay2-sidecar PRIVATE ${XCB_COMPOSITE_INCLUDE_DIRS})
            target_link_libraries(vkrelay2-sidecar PRIVATE ${XCB_COMPOSITE_LIBRARIES})
            message(STATUS "vkrelay2: sidecar enabled (vkrelay2-sidecar) + XComposite chrome capture")
        else()
            message(STATUS "vkrelay2: sidecar enabled (vkrelay2-sidecar); chrome capture DISABLED "
                           "(no libxcb-composite)")
        endif()
        if(XCB_SHAPE_FOUND)
            target_compile_definitions(vkrelay2-sidecar PRIVATE VKRELAY2_HAVE_XCB_SHAPE)
            target_include_directories(vkrelay2-sidecar PRIVATE ${XCB_SHAPE_INCLUDE_DIRS})
            target_link_libraries(vkrelay2-sidecar PRIVATE ${XCB_SHAPE_LIBRARIES})
            message(STATUS "vkrelay2: sidecar + XShape bounding-region mask")
        else()
            message(STATUS "vkrelay2: sidecar XShape mask DISABLED (no libxcb-shape)")
        endif()
        # enable input injection (xcb_test_fake_input) ONLY when libxcb-xtest is present.
        # Absent -> the sidecar still builds + runs (focus/close), but ships no pointer/key/
        # wheel injection and logs "input injection disabled".
        if(XCB_XTEST_FOUND)
            target_compile_definitions(vkrelay2-sidecar PRIVATE VKRELAY2_HAVE_XCB_XTEST)
            target_include_directories(vkrelay2-sidecar PRIVATE ${XCB_XTEST_INCLUDE_DIRS})
            target_link_libraries(vkrelay2-sidecar PRIVATE ${XCB_XTEST_LIBRARIES})
            message(STATUS "vkrelay2: sidecar + XTest input injection")
        else()
            message(STATUS "vkrelay2: sidecar input injection DISABLED (no libxcb-xtest); "
                           "focus/close still work")
        endif()
        # enable cursor capture (XFixes) ONLY when libxcb-xfixes is present. Absent -> the
        # sidecar still builds + runs, no cursor forwarding, logged.
        if(XCB_XFIXES_FOUND)
            target_compile_definitions(vkrelay2-sidecar PRIVATE VKRELAY2_HAVE_XCB_XFIXES)
            target_include_directories(vkrelay2-sidecar PRIVATE ${XCB_XFIXES_INCLUDE_DIRS})
            target_link_libraries(vkrelay2-sidecar PRIVATE ${XCB_XFIXES_LIBRARIES})
            message(STATUS "vkrelay2: sidecar + XFixes cursor capture")
        else()
            message(STATUS "vkrelay2: sidecar cursor capture DISABLED (no libxcb-xfixes)")
        endif()

        # Configure-passthrough probe (boundary-smoke helper): a pure-xcb client that
        # verifies the sidecar WM honors a client ConfigureRequest (move+resize). Run by
        # run_sidecar_smoke.sh after the sidecar is ready.
        add_executable(vkrelay2-configure-probe linux/sidecar/configure_probe.cpp)
        target_link_libraries(vkrelay2-configure-probe PRIVATE vkrelay2_warnings)
        target_include_directories(vkrelay2-configure-probe PRIVATE ${XCB_INCLUDE_DIRS})
        target_link_libraries(vkrelay2-configure-probe PRIVATE ${XCB_LIBRARIES})

        # Chrome canary (boundary-smoke helper): a pure-xcb client (NO Vulkan) that maps a
        # toplevel filled with a known solid color, so run_chrome_smoke.sh can prove the sidecar
        # captured it and the worker painted it (sampled via the DebugChromeState wire op).
        add_executable(vkrelay2-chrome-canary linux/sidecar/chrome_canary.cpp)
        target_link_libraries(vkrelay2-chrome-canary PRIVATE vkrelay2_warnings)
        target_include_directories(vkrelay2-chrome-canary PRIVATE ${XCB_INCLUDE_DIRS})
        target_link_libraries(vkrelay2-chrome-canary PRIVATE ${XCB_LIBRARIES})
        if(XCB_SHAPE_FOUND)
            target_compile_definitions(vkrelay2-chrome-canary PRIVATE VKRELAY2_HAVE_XCB_SHAPE)
            target_include_directories(vkrelay2-chrome-canary PRIVATE ${XCB_SHAPE_INCLUDE_DIRS})
            target_link_libraries(vkrelay2-chrome-canary PRIVATE ${XCB_SHAPE_LIBRARIES})
        endif()

        # Chrome state query (boundary-smoke helper): a sidecar-plane RPC client (NO xcb,
        # NO Vulkan) that proves a toplevel's chrome from OUTSIDE the worker -- SidecarHello(token) ->
        # DebugChromeState(xid, sample) -> exit 0 iff shown + the sampled pixel matches --expect-bgra.
        add_executable(vkrelay2-chrome-query linux/sidecar/chrome_query.cpp)
        target_link_libraries(vkrelay2-chrome-query PRIVATE vkrelay2_common vkrelay2_warnings)

        # Input canary (boundary-smoke helper): a pure-xcb client (NO Vulkan) that maps a
        # toplevel, selects pointer/button/key/focus input + advertises WM_DELETE_WINDOW, and is a
        # STRICT ORACLE -- it passes only after receiving exactly the canonical focus/click/wheel/key/
        # close sequence (in order), so run_input_smoke.sh proves sidecar->XTest->guest end to end.
        add_executable(vkrelay2-input-canary linux/sidecar/input_canary.cpp)
        target_link_libraries(vkrelay2-input-canary PRIVATE vkrelay2_warnings)
        target_include_directories(vkrelay2-input-canary PRIVATE ${XCB_INCLUDE_DIRS})
        target_link_libraries(vkrelay2-input-canary PRIVATE ${XCB_LIBRARIES})

        # Rotate canary (repro): a pure-xcb client (NO Vulkan, NO sidecar) that self-drives
        # Blender's view-rotate at the protocol level -- grab the pointer, WARP-recenter repeatedly,
        # ungrab -- and reports whether the guest Xwayland SURVIVED. On the seat-less headless stack it
        # reproduces the grab-teardown null-deref with no Blender and no human; run_rotate_smoke.sh
        # is the regression gate (green == Xwayland survives the gesture).
        add_executable(vkrelay2-rotate-canary linux/sidecar/rotate_canary.cpp)
        target_link_libraries(vkrelay2-rotate-canary PRIVATE vkrelay2_warnings)
        target_include_directories(vkrelay2-rotate-canary PRIVATE ${XCB_INCLUDE_DIRS})
        target_link_libraries(vkrelay2-rotate-canary PRIVATE ${XCB_LIBRARIES})

        # Cursor canary (boundary-smoke helper): a pure-xcb client (NO Vulkan) that maps a
        # toplevel and XDefineCursors a KNOWN solid-color cursor (core xcb_create_cursor). The sidecar
        # warps the pointer into it (XTest), captures the cursor (XFixes), and ships it to the worker;
        # run_cursor_smoke.sh proves the built HCURSOR via the DebugCursorState wire op.
        add_executable(vkrelay2-cursor-canary linux/sidecar/cursor_canary.cpp)
        target_link_libraries(vkrelay2-cursor-canary PRIVATE vkrelay2_warnings)
        target_include_directories(vkrelay2-cursor-canary PRIVATE ${XCB_INCLUDE_DIRS})
        target_link_libraries(vkrelay2-cursor-canary PRIVATE ${XCB_LIBRARIES})

        # Cursor state query (boundary-smoke helper): a sidecar-plane RPC client (NO xcb,
        # NO Vulkan) -- SidecarHello(token) -> DebugCursorState(xid, sample) -> exit 0 iff the worker
        # built a cursor and the sampled pixel matches --expect-bgra.
        add_executable(vkrelay2-cursor-query linux/sidecar/cursor_query.cpp)
        target_link_libraries(vkrelay2-cursor-query PRIVATE vkrelay2_common vkrelay2_warnings)

        # Source-layer capture tool: a sidecar-plane RPC client (NO xcb, NO Vulkan) --
        # SidecarHello(token) -> DebugCaptureWindow(xid, layer) -> writes a PNG (dependency-free
        # png_write.h) + a JSON metadata sidecar. --all captures every enumerated window (multi-window
        # apps). The obs smokes use it as a hard gate (--min-unique-colors / --expect-bgra).
        add_executable(vkrelay2-capture linux/observe/capture_main.cpp)
        target_link_libraries(vkrelay2-capture PRIVATE vkrelay2_common vkrelay2_warnings)

        # Popup canary (boundary-smoke helper): a pure-xcb client (NO Vulkan) that maps a
        # toplevel + an OVERRIDE-REDIRECT child popup (advertising _NET_WM_WINDOW_TYPE_MENU +
        # WM_TRANSIENT_FOR), so run_popup_smoke.sh can prove the sidecar detects the self-mapped popup,
        # resolves its owner, and the worker represents it owned by the toplevel.
        add_executable(vkrelay2-popup-canary linux/sidecar/popup_canary.cpp)
        target_link_libraries(vkrelay2-popup-canary PRIVATE vkrelay2_warnings)
        target_include_directories(vkrelay2-popup-canary PRIVATE ${XCB_INCLUDE_DIRS})
        target_link_libraries(vkrelay2-popup-canary PRIVATE ${XCB_LIBRARIES})

        # Popup state query (boundary-smoke helper): a sidecar-plane RPC client (NO xcb, NO
        # Vulkan) -- SidecarHello(token) -> DebugEnumWindows -> exit 0 iff the popup XID is represented
        # with is_popup + owner_xid == --expect-owner (the worker-visible owner/z-order linkage proof).
        add_executable(vkrelay2-popup-query linux/sidecar/popup_query.cpp)
        target_link_libraries(vkrelay2-popup-query PRIVATE vkrelay2_common vkrelay2_warnings)

        # Geometry canary (boundary-smoke helper): a pure-xcb client (NO Vulkan) that maps a
        # toplevel and MOVES ITSELF across a known sequence of X-root positions (xcb_configure_window),
        # so run_geometry_smoke.sh can prove the sidecar forwards each self-move as an UpdateToplevel
        # and the worker converges the host window to the final authored position (a live move).
        add_executable(vkrelay2-geometry-canary linux/sidecar/geometry_canary.cpp)
        target_link_libraries(vkrelay2-geometry-canary PRIVATE vkrelay2_warnings)
        target_include_directories(vkrelay2-geometry-canary PRIVATE ${XCB_INCLUDE_DIRS})
        target_link_libraries(vkrelay2-geometry-canary PRIVATE ${XCB_LIBRARIES})

        # Placement canary (boundary-smoke helper): a pure-xcb client (NO Vulkan) that maps ONE
        # toplevel with NO position hint, so run_placement_smoke.sh can prove the sidecar CENTERS it
        # (DebugEnumWindows include_actual shows the host landed at the centered origin, not 0,0).
        add_executable(vkrelay2-placement-canary linux/sidecar/placement_canary.cpp)
        target_link_libraries(vkrelay2-placement-canary PRIVATE vkrelay2_warnings)
        target_include_directories(vkrelay2-placement-canary PRIVATE ${XCB_INCLUDE_DIRS})
        target_link_libraries(vkrelay2-placement-canary PRIVATE ${XCB_LIBRARIES})

        # Bigroot canary (GD guest-display-geometry boundary-smoke helper): a pure-xcb client (NO
        # Vulkan) that maps a toplevel LARGER than the old default guest root and reports the root
        # size + the EXACT client coords of each ButtonPress -- run_bigroot_smoke.sh proves a click
        # injected BEYOND the old root lands exactly (the pointer warp is no longer root-clamped).
        add_executable(vkrelay2-bigroot-canary linux/sidecar/bigroot_canary.cpp)
        target_link_libraries(vkrelay2-bigroot-canary PRIVATE vkrelay2_warnings)
        target_include_directories(vkrelay2-bigroot-canary PRIVATE ${XCB_INCLUDE_DIRS})
        target_link_libraries(vkrelay2-bigroot-canary PRIVATE ${XCB_LIBRARIES})

        # Placement-clamp boundary canary: a pure-xcb client (NO Vulkan)
        # that maps an EXPLICITLY-positioned toplevel off the top-left, then self-ConfigureRequests
        # off the left, and reads back its OWN X-root geometry -- so run_clamp_smoke.sh proves the
        # sidecar clamps the client origin on-screen on BOTH the map (maybe_center_toplevel) and the
        # ConfigureRequest paths (pre-fix it lands off-screen; fixed it lands at the clamped target).
        add_executable(vkrelay2-clamp-canary linux/sidecar/clamp_canary.cpp)
        target_link_libraries(vkrelay2-clamp-canary PRIVATE vkrelay2_warnings)
        target_include_directories(vkrelay2-clamp-canary PRIVATE ${XCB_INCLUDE_DIRS})
        target_link_libraries(vkrelay2-clamp-canary PRIVATE ${XCB_LIBRARIES})

        # Fullscreen canary (the ExtremeTuxRacer class -- boundary-smoke helper): a pure-xcb client
        # (NO Vulkan) shaped like SFML 2.5's non-EWMH fullscreen path -- a ROOT-SIZED
        # OVERRIDE-REDIRECT toplevel that waits for the non-obscured VisibilityNotify SFML's
        # sf::Window::create busy-waits on. run_fullscreen_smoke.sh proves the sidecar registers
        # such a window as a fullscreen toplevel (pre-fix it was dropped as popup junk, nothing
        # composite-redirected it, and the app hung forever in that spin).
        add_executable(vkrelay2-fullscreen-canary linux/sidecar/fullscreen_canary.cpp)
        target_link_libraries(vkrelay2-fullscreen-canary PRIVATE vkrelay2_warnings)
        target_include_directories(vkrelay2-fullscreen-canary PRIVATE ${XCB_INCLUDE_DIRS})
        target_link_libraries(vkrelay2-fullscreen-canary PRIVATE ${XCB_LIBRARIES})

        # Resize canary (boundary-smoke helper): a pure-xcb client (NO Vulkan) that maps a
        # positioned toplevel at size A then issues its OWN ConfigureRequest RESIZE to size B, so
        # run_resize_smoke.sh can prove the sidecar forwards the guest resize to the worker
        # (DebugEnumWindows include_actual shows the host client converged to B). Covers the
        # ConfigureRequest->forward_update path the in-process integration test bypasses.
        add_executable(vkrelay2-resize-canary linux/sidecar/resize_canary.cpp)
        target_link_libraries(vkrelay2-resize-canary PRIVATE vkrelay2_warnings)
        target_include_directories(vkrelay2-resize-canary PRIVATE ${XCB_INCLUDE_DIRS})
        target_link_libraries(vkrelay2-resize-canary PRIVATE ${XCB_LIBRARIES})

        # Lifecycle canary (boundary-smoke helper): a pure-xcb client (NO Vulkan) that maps
        # a toplevel, UNMAPS it, then REMAPS it (synchronizing on the WM's StructureNotify), so
        # run_lifecycle_smoke.sh can prove the sidecar forwards unmap->set_visibility(Hidden) +
        # remap->set_visibility(Visible) and the worker keeps the SAME representation visible again at
        # the SAME epoch (the hidden-but-represented bug fix).
        add_executable(vkrelay2-lifecycle-canary linux/sidecar/lifecycle_canary.cpp)
        target_link_libraries(vkrelay2-lifecycle-canary PRIVATE vkrelay2_warnings)
        target_include_directories(vkrelay2-lifecycle-canary PRIVATE ${XCB_INCLUDE_DIRS})
        target_link_libraries(vkrelay2-lifecycle-canary PRIVATE ${XCB_LIBRARIES})

        # Feedback canary (boundary-smoke helper): a pure-xcb client (NO Vulkan) that maps
        # a toplevel and OBSERVES its own geometry, so run_feedback_smoke.sh can prove the sidecar
        # authors a GUEST move from a Win32-user-origin GeometryRequest (the reverse direction).
        add_executable(vkrelay2-feedback-canary linux/sidecar/feedback_canary.cpp)
        target_link_libraries(vkrelay2-feedback-canary PRIVATE vkrelay2_warnings)
        target_include_directories(vkrelay2-feedback-canary PRIVATE ${XCB_INCLUDE_DIRS})
        target_link_libraries(vkrelay2-feedback-canary PRIVATE ${XCB_LIBRARIES})

        # Geometry state query (boundary-smoke helper): a sidecar-plane RPC client (NO xcb,
        # NO Vulkan) -- SidecarHello(token) -> DebugEnumWindows(include_actual) -> exit 0 iff the xid's
        # ACTUAL host client origin converged to --expect-x/--expect-y with last_host_apply_seq != 0
        # (the worker-visible live-move convergence proof).
        add_executable(vkrelay2-geometry-query linux/sidecar/geometry_query.cpp)
        target_link_libraries(vkrelay2-geometry-query PRIVATE vkrelay2_common vkrelay2_warnings)
    else()
        message(STATUS "vkrelay2: libxcb not found; canaries + sidecar skipped (ICD still built)")
    endif()

    # Readback canary (offscreen-readback boundary-smoke helper): a GLX client (NO Vulkan
    # linkage -- its GL dispatches through GLX/zink into whatever ICD the session pinned) that
    # clears an app FBO and the winsys back buffer to distinct known colors and glReadPixels-checks
    # both, so run_readback_smoke.sh proves GPU-written pixels cross the relay BACK to the guest
    # (the OpenSCAD `-o png` export path). Gated on its own gl/x11 probe, independent of xcb.
    if(GL_CANARY_FOUND)
        add_executable(vkrelay2-readback-canary linux/canary/readback_canary.cpp)
        target_link_libraries(vkrelay2-readback-canary PRIVATE vkrelay2_warnings)
        target_include_directories(vkrelay2-readback-canary PRIVATE ${GL_CANARY_INCLUDE_DIRS})
        target_link_libraries(vkrelay2-readback-canary PRIVATE ${GL_CANARY_LIBRARIES})
        message(STATUS "vkrelay2: GL readback canary enabled (vkrelay2-readback-canary)")

        # AMD-iGPU viewport-corruption diagnostic: one parameterized GL/zink canary whose
        # self-judging multi-frame rungs isolate reused clears, the loadOp/scissored-clear
        # transition, imageless attachment rotation, and per-frame VBO replacement. Frame 0 and
        # every later frame are checked.
        add_executable(vkrelay2-frame-transition-canary
                       linux/canary/frame_transition_canary.cpp)
        target_link_libraries(vkrelay2-frame-transition-canary PRIVATE vkrelay2_warnings)
        target_include_directories(vkrelay2-frame-transition-canary
                                   PRIVATE ${GL_CANARY_INCLUDE_DIRS})
        target_link_libraries(vkrelay2-frame-transition-canary PRIVATE ${GL_CANARY_LIBRARIES})
        message(STATUS "vkrelay2: frame-transition canary enabled")

        # MRT canary (the productionized development probe that exposed the silent 2-color
        # half-render): a GLX client drawing distinct colors to multiple FBO attachments
        # (2-attachment, per-attachment write mask, UNUSED-gap draw buffers), each read back
        # exactly -- run_mrt_smoke.sh proves faithful-N color attachments through the relay.
        add_executable(vkrelay2-mrt-canary linux/canary/mrt_canary.cpp)
        target_link_libraries(vkrelay2-mrt-canary PRIVATE vkrelay2_warnings)
        target_include_directories(vkrelay2-mrt-canary PRIVATE ${GL_CANARY_INCLUDE_DIRS})
        target_link_libraries(vkrelay2-mrt-canary PRIVATE ${GL_CANARY_LIBRARIES})

        # Tessellation + geometry breadth: GL 4.0 core TCS/TES/GS sub-gates whose
        # PIXELS prove the stages ran (TES shrink / GS expansion), read back exactly.
        add_executable(vkrelay2-tessgeom-canary linux/canary/tessgeom_canary.cpp)
        target_link_libraries(vkrelay2-tessgeom-canary PRIVATE vkrelay2_warnings)
        target_include_directories(vkrelay2-tessgeom-canary PRIVATE ${GL_CANARY_INCLUDE_DIRS})
        target_link_libraries(vkrelay2-tessgeom-canary PRIVATE ${GL_CANARY_LIBRARIES})
    else()
        message(STATUS "vkrelay2: gl/x11 dev not found; GL readback + MRT canaries skipped")
    endif()

    # Queue-order readback canary (regression): a native Vulkan client (real
    # loader + the session-pinned ICD, NO WSI, NO GL) that submits a clear+copy readback with NO
    # completion proof of its own, then waits a LATER same-queue fence-only submit (phase A) / an
    # empty timeline-signal submit (phase B) -- queue order makes both proofs cover the earlier
    # copy, so the mapped bytes must be the clear colors, not stale zeros.
    add_executable(vkrelay2-readback-order-canary linux/canary/readback_order_canary.cpp)
    target_link_libraries(vkrelay2-readback-order-canary PRIVATE vkrelay2_warnings Vulkan::Vulkan)
    target_include_directories(vkrelay2-readback-order-canary PRIVATE ${Vulkan_INCLUDE_DIRS})
endif()

# Launch CLI as a library so the executable AND the integration tests share one build of it
# (e.g. the --open-session backend-gate test drives run_cli in-process against a loopback daemon).
add_library(vkrelay2_launch STATIC common/launch/launch_cli.cpp)
target_link_libraries(vkrelay2_launch PUBLIC vkrelay2_common vkrelay2_warnings)

add_executable(vkrelay2-launch common/launch/main.cpp)
target_link_libraries(vkrelay2-launch PRIVATE vkrelay2_launch vkrelay2_warnings)

add_executable(vkrelay2-argv-echo tests/canaries/argv_echo_main.cpp)
target_link_libraries(vkrelay2-argv-echo PRIVATE vkrelay2_common vkrelay2_warnings)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
enable_testing()
add_subdirectory(tests)
