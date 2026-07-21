# vkrelay2 test registration.

# --- Unit tests -----------------------------------------------------------
set(VKRELAY2_UNIT_TESTS json display_layout launch_descriptor worker_lifecycle command_line gpu
                        smoke_matrix wire messages vkrpc daemon_endpoint sidecar png)
foreach(name ${VKRELAY2_UNIT_TESTS})
    add_executable(unit_${name} unit/unit_${name}.cpp)
    target_link_libraries(unit_${name} PRIVATE vkrelay2_common vkrelay2_warnings)
    add_test(NAME unit_${name} COMMAND unit_${name})
endforeach()

# --- Unit: launch-time worker argv (faithful GPU selection + backend pass-through) ---
# Links vkrelay2_supervisor (build_worker_argv lives there), so it is registered outside the
# common-only loop above.
add_executable(unit_launch_argv unit/unit_launch_argv.cpp)
target_link_libraries(unit_launch_argv PRIVATE vkrelay2_supervisor vkrelay2_warnings)
add_test(NAME unit_launch_argv COMMAND unit_launch_argv)

# --- Unit: raw-monitor/QDC reconciliation (pure, dual-platform) ---
add_executable(unit_display_probe unit/unit_display_probe.cpp)
target_link_libraries(unit_display_probe PRIVATE vkrelay2_supervisor vkrelay2_warnings)
add_test(NAME unit_display_probe COMMAND unit_display_probe)

# --- Unit: ICD fail-closed subset predicates (pure; needs vulkan.h) ---
# The ICD's create-info/command subset validators (linux/icd/icd_subset.h) are header-only, so this
# links nothing extra -- it just needs the Vulkan headers. Gated on Vulkan_FOUND (true wherever the
# ICD / SDK is, i.e. both platforms here), so the reject-out-of-subset behavior is dual-platform.
if(Vulkan_FOUND)
    add_executable(unit_icd_subset unit/unit_icd_subset.cpp)
    target_link_libraries(unit_icd_subset PRIVATE vkrelay2_common vkrelay2_warnings)
    target_include_directories(unit_icd_subset PRIVATE ${Vulkan_INCLUDE_DIRS})
    add_test(NAME unit_icd_subset COMMAND unit_icd_subset)
endif()

# --- Unit: ICD host-visible memory tracker (pure; dual-platform) ---
# linux/icd/icd_memory.h is pure C++ (no Vulkan, no backend), so the hardest logic -- the
# transactional dirty-diff + the coherent-submit sweep -- is tested directly on BOTH platforms.
add_executable(unit_icd_memory unit/unit_icd_memory.cpp)
target_link_libraries(unit_icd_memory PRIVATE vkrelay2_common vkrelay2_warnings)
add_test(NAME unit_icd_memory COMMAND unit_icd_memory)

# --- Unit: abort-aware acquire poll policy (pure header, dual-platform) ---
# windows/worker/acquire_poll.hpp is dependency-free (no Vulkan/Win32/clock), so the guest-timeout
# contract + abort-release logic is tested directly on BOTH platforms.
add_executable(unit_acquire_poll unit/unit_acquire_poll.cpp)
target_link_libraries(unit_acquire_poll PRIVATE vkrelay2_common vkrelay2_warnings)
add_test(NAME unit_acquire_poll COMMAND unit_acquire_poll)

# --- Fuzz smoke: control-plane decoders cannot crash ----------------------
add_executable(fuzz_wire fuzz/fuzz_wire.cpp)
target_link_libraries(fuzz_wire PRIVATE vkrelay2_common vkrelay2_warnings)
add_test(NAME fuzz_wire COMMAND fuzz_wire)

# --- Fuzz smoke: Vulkan RPC envelope decoders cannot crash ----------------
add_executable(fuzz_vkrpc fuzz/fuzz_vkrpc.cpp)
target_link_libraries(fuzz_vkrpc PRIVATE vkrelay2_common vkrelay2_warnings)
add_test(NAME fuzz_vkrpc COMMAND fuzz_vkrpc)

# --- Integration: loopback handshake + GPU list/select over TCP -----------
add_executable(integration_handshake integration/integration_handshake.cpp)
target_link_libraries(integration_handshake PRIVATE vkrelay2_common vkrelay2_warnings)
add_test(NAME integration_handshake COMMAND integration_handshake)

# --- Integration: app-run backend gate fails closed before LaunchSession --
add_executable(integration_session_backend_gate integration/integration_session_backend_gate.cpp)
target_link_libraries(integration_session_backend_gate PRIVATE vkrelay2_launch vkrelay2_supervisor
                                                                  vkrelay2_warnings)
add_test(NAME integration_session_backend_gate COMMAND integration_session_backend_gate)

# --- Integration: serve-loop read deadline -- a stalled client cannot wedge the one-at-a-time
# control loop (mirrors do_serve()'s per-connection set_read_timeout). Loopback only, no Vulkan.
add_executable(integration_serve_deadline integration/integration_serve_deadline.cpp)
target_link_libraries(integration_serve_deadline PRIVATE vkrelay2_common vkrelay2_warnings)
add_test(NAME integration_serve_deadline COMMAND integration_serve_deadline)

# --- Integration: transport non-consuming peer-close detection (prerequisite) -- the
# primitive an out-of-band observer uses to notice app death while the RPC reader is blocked in the
# WSI pump. Loopback only, no Vulkan.
add_executable(integration_transport_peerclose integration/integration_transport_peerclose.cpp)
target_link_libraries(integration_transport_peerclose PRIVATE vkrelay2_common vkrelay2_warnings)
add_test(NAME integration_transport_peerclose COMMAND integration_transport_peerclose)

# --- Integration: deterministic worker teardown -- app peer-close -> liveness
# observer fires abort -> the wedged acquire poll releases. Loopback + the pure worker headers, no
# Vulkan. Hard TIMEOUT so a regression that fails to release cannot hang the gate.
add_executable(integration_worker_teardown integration/integration_worker_teardown.cpp)
target_link_libraries(integration_worker_teardown PRIVATE vkrelay2_common vkrelay2_warnings)
add_test(NAME integration_worker_teardown COMMAND integration_worker_teardown)
set_tests_properties(integration_worker_teardown PROPERTIES TIMEOUT 30)

# --- Integration: argv-echo through the real spawn wrapper ----------------
add_executable(integration_argv_echo integration/integration_argv_echo.cpp)
target_link_libraries(integration_argv_echo PRIVATE vkrelay2_common vkrelay2_warnings)
add_test(NAME integration_argv_echo
         COMMAND integration_argv_echo $<TARGET_FILE:vkrelay2-argv-echo>)

# --- Integration: app-lifetime teardown (Process::terminate_graceful + reset_child_signals) -------
# Self-spawning (re-execs itself as --child); no external canary needed.
add_executable(integration_process_teardown integration/integration_process_teardown.cpp)
target_link_libraries(integration_process_teardown PRIVATE vkrelay2_common vkrelay2_warnings)
add_test(NAME integration_process_teardown COMMAND integration_process_teardown)

# --- Integration: supervisor self-test (spawn + heartbeat + kill) ---------
add_test(NAME supervisor_self_test
         COMMAND vkrelay2-supervisor --self-test --worker $<TARGET_FILE:vkrelay2-worker>)

# --- Integration: worker lifecycle (concurrency + crash containment) -
add_executable(integration_supervisor_worker integration/integration_supervisor_worker.cpp)
target_link_libraries(integration_supervisor_worker
                      PRIVATE vkrelay2_supervisor vkrelay2_warnings)
add_test(NAME integration_supervisor_worker
         COMMAND integration_supervisor_worker $<TARGET_FILE:vkrelay2-worker>)

# --- Integration: app trigger + reconnect handoff -----------
add_executable(integration_app_session integration/integration_app_session.cpp)
target_link_libraries(integration_app_session PRIVATE vkrelay2_supervisor vkrelay2_warnings)
add_test(NAME integration_app_session
         COMMAND integration_app_session $<TARGET_FILE:vkrelay2-worker>)

# --- Integration: hard-path -- an uncooperative worker (data plane wedged, ignoring
# abort) is STILL reaped by the supervisor after the app disconnects (proves the leak is bounded
# independently of graceful cancellation). Real supervisor + worker child; hard TIMEOUT backstops a
# regression that never reaps.
add_executable(integration_worker_kill integration/integration_worker_kill.cpp)
target_link_libraries(integration_worker_kill PRIVATE vkrelay2_supervisor vkrelay2_warnings)
add_test(NAME integration_worker_kill
         COMMAND integration_worker_kill $<TARGET_FILE:vkrelay2-worker>)
set_tests_properties(integration_worker_kill PROPERTIES TIMEOUT 60)

# --- Integration: sidecar control plane (handshake + negotiate + gating) --
add_executable(integration_sidecar_plane integration/integration_sidecar_plane.cpp)
target_link_libraries(integration_sidecar_plane PRIVATE vkrelay2_supervisor vkrelay2_warnings)
add_test(NAME integration_sidecar_plane
         COMMAND integration_sidecar_plane $<TARGET_FILE:vkrelay2-worker>)

# --- Integration: Vulkan RPC over the data plane -------------
add_executable(integration_vulkan_rpc integration/integration_vulkan_rpc.cpp)
target_link_libraries(integration_vulkan_rpc PRIVATE vkrelay2_supervisor vkrelay2_warnings)
add_test(NAME integration_vulkan_rpc
         COMMAND integration_vulkan_rpc $<TARGET_FILE:vkrelay2-worker>)

# --- Integration: default daemon endpoint + bind override -------
# Both platforms: the launcher reaches the daemon via the env-defaulted well-known
# endpoint (no typed host:port) and falls back to the mock when none is running.
add_executable(integration_daemon_endpoint integration/integration_daemon_endpoint.cpp)
target_link_libraries(integration_daemon_endpoint PRIVATE vkrelay2_common vkrelay2_warnings)
add_test(NAME integration_daemon_endpoint
         COMMAND integration_daemon_endpoint
                 $<TARGET_FILE:vkrelay2-supervisor> $<TARGET_FILE:vkrelay2-worker>
                 $<TARGET_FILE:vkrelay2-launch>)

# --- Integration: daemon-side GPU enumeration (worker probe) ---
# Runs on both platforms: exercises the worker spawn + JSON parse + mock fallback.
# On a Vulkan-capable worker with a device it asserts the list is real.
add_executable(integration_gpu_probe integration/integration_gpu_probe.cpp)
target_link_libraries(integration_gpu_probe PRIVATE vkrelay2_supervisor vkrelay2_warnings)
add_test(NAME integration_gpu_probe
         COMMAND integration_gpu_probe $<TARGET_FILE:vkrelay2-worker>)

# two-connection query -> LaunchSession snapshot pinning, cache resolution, and
# fail-closed unknown identity. Pure loopback; runs on both platforms.
add_executable(integration_display_snapshot integration/integration_display_snapshot.cpp)
target_link_libraries(integration_display_snapshot PRIVATE vkrelay2_supervisor vkrelay2_warnings)
add_test(NAME integration_display_snapshot COMMAND integration_display_snapshot)

if(WIN32)
    # Isolated live topology capture (the production cache now consumes the same
    # probe through its validated injected-ID seam).
    add_executable(integration_display_probe integration/integration_display_probe.cpp)
    target_link_libraries(integration_display_probe PRIVATE vkrelay2_supervisor vkrelay2_warnings)
    add_test(NAME integration_display_probe COMMAND integration_display_probe)
    set_tests_properties(integration_display_probe PROPERTIES SKIP_RETURN_CODE 77)
endif()

# --- Integration: real (loader-backed) capability negotiation -
# Only when the worker was built with the real backend (Windows + Vulkan SDK).
# The test itself skips gracefully if no usable device is present at run time.
if(WIN32 AND Vulkan_FOUND)
    # Standalone native-loader oracle for the AMD exact-tail vertex-fetch investigation. This
    # intentionally bypasses every vkrelay2 component and is NOT a CTest gate: a failing affected
    # driver is diagnostic evidence, not a project regression.
    add_executable(vkrelay2-native-vertex-tail-probe
                   integration/native_vertex_tail_probe.cpp)
    target_link_libraries(vkrelay2-native-vertex-tail-probe PRIVATE Vulkan::Vulkan
                                                                     vkrelay2_warnings)
    target_include_directories(vkrelay2-native-vertex-tail-probe
                               PRIVATE ${CMAKE_SOURCE_DIR} ${Vulkan_INCLUDE_DIRS})

    add_executable(integration_vulkan_real integration/integration_vulkan_real.cpp)
    target_link_libraries(integration_vulkan_real PRIVATE vkrelay2_supervisor vkrelay2_warnings)
    add_test(NAME integration_vulkan_real
             COMMAND integration_vulkan_real $<TARGET_FILE:vkrelay2-worker>)

    # In-process real-backend test: drives a real WM_SIZE against the
    # surface's HWND and asserts the geometry-dirty latch (acquire/present return
    # OUT_OF_DATE, create_swapchain clears it) + the slot lock -- the only way to
    # exercise the real WndProc latch deterministically before the sidecar's resize verb
    # Links the real backend directly; skips if no usable device.
    add_executable(integration_real_backend integration/integration_real_backend.cpp)
    target_link_libraries(integration_real_backend PRIVATE vkrelay2_worker_vulkan vkrelay2_warnings
                                                           Shcore)
    add_test(NAME integration_real_backend COMMAND integration_real_backend)

    # In-process real-backend DRAW test: builds a real bufferless-triangle
    # pipeline (image views, SPIR-V shader modules, render pass, framebuffers, empty layout, dynamic
    # VIEWPORT/SCISSOR pipeline) and draws + presents a frame on the real GPU. The validation-clean
    # gate is a manual run with VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation; skips if no device.
    add_executable(integration_real_draw integration/integration_real_draw.cpp)
    target_link_libraries(integration_real_draw PRIVATE vkrelay2_worker_vulkan vkrelay2_warnings)
    add_test(NAME integration_real_draw COMMAND integration_real_draw)

    # In-process real-backend VBO DRAW test: allocate host-visible-coherent
    # memory, create+bind a vertex buffer, upload 3 vertices via write_memory_ranges, build a pipeline
    # whose vertex input matches the buffer, and draw+present. Validation-clean gate is a manual run
    # with VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation; skips if no device.
    add_executable(integration_real_vbo integration/integration_real_vbo.cpp)
    target_link_libraries(integration_real_vbo PRIVATE vkrelay2_worker_vulkan vkrelay2_warnings)
    add_test(NAME integration_real_vbo COMMAND integration_real_vbo)

    add_executable(integration_real_indirect integration/integration_real_indirect.cpp)
    target_link_libraries(integration_real_indirect PRIVATE vkrelay2_worker_vulkan vkrelay2_warnings)
    add_test(NAME integration_real_indirect COMMAND integration_real_indirect)

    # the descriptor / per-frame UBO path drawn + presented on the real GPU.
    add_executable(integration_real_ubo integration/integration_real_ubo.cpp)
    target_link_libraries(integration_real_ubo PRIVATE vkrelay2_worker_vulkan vkrelay2_warnings)
    add_test(NAME integration_real_ubo COMMAND integration_real_ubo)

    # Compute pipelines + dispatch: the whole compute chain on the real GPU --
    # queue-flags honesty, create (+rejects), COMPUTE-point bind/sets/push constants, dispatch
    # (direct AND indirect), the shader-write -> transfer-read buffer barrier, copy-out
    # readback, and 4096 byte-exact results via read_memory_ranges.
    add_executable(integration_real_compute integration/integration_real_compute.cpp)
    target_link_libraries(integration_real_compute PRIVATE vkrelay2_worker_vulkan
                                                           vkrelay2_warnings)
    add_test(NAME integration_real_compute COMMAND integration_real_compute)

    # core-1.0 synchronization objects on the real GPU -- vkGetFenceStatus (honest
    # VkResult), the VkEvent object model (create/destroy + host status/set/reset), and a submitted
    # vkCmdSetEvent reaching the host. No complex GPU workload is required.
    add_executable(integration_real_events integration/integration_real_events.cpp)
    target_link_libraries(integration_real_events PRIVATE vkrelay2_worker_vulkan vkrelay2_warnings)
    add_test(NAME integration_real_events COMMAND integration_real_events)

    # VK_KHR_synchronization2 on the real GPU -- a vkCmdPipelineBarrier2 (64-bit
    # masks) + vkCmdSetEvent2 recorded + replayed + submitted via vkQueueSubmit2, event reads SET.
    add_executable(integration_real_sync2 integration/integration_real_sync2.cpp)
    target_link_libraries(integration_real_sync2 PRIVATE vkrelay2_worker_vulkan vkrelay2_warnings)
    add_test(NAME integration_real_sync2 COMMAND integration_real_sync2)

    # Vulkan 1.2 bufferDeviceAddress support: the raw-GPU-pointer chain on the real GPU --
    # the enabled feature (host-verified at create_device), the DEVICE_ADDRESS allocate flag, the
    # VUID-03339 bind gate, get_buffer_device_address (real VA, non-zero + stable, fail-closed
    # negatives), and a descriptorless buffer-reference compute shader through the raw address
    # with a byte-exact readback.
    add_executable(integration_real_bda integration/integration_real_bda.cpp)
    target_link_libraries(integration_real_bda PRIVATE vkrelay2_worker_vulkan vkrelay2_warnings)
    add_test(NAME integration_real_bda COMMAND integration_real_bda)

    #   (descriptorIndexing plumbing/model, capability still masked): the scalar/chain
    # normalization negatives, UAB layout/pool admission, variable-count allocation at the
    # ALLOCATED count, the honest host GetDescriptorSetLayoutSupport, and THE ordering crux --
    # record an UNWRITTEN update-after-bind set, write it, destroy-old/update-to-new before
    # submit, and the byte-exact readback shows the NEW referent (replay reads the LIVE set).
    add_executable(integration_real_di integration/integration_real_di.cpp)
    target_link_libraries(integration_real_di PRIVATE vkrelay2_worker_vulkan vkrelay2_warnings)
    add_test(NAME integration_real_di COMMAND integration_real_di)

    # Depth spine: a real D16_UNORM depth image bound to
    # device-local memory, a color+depth render pass / framebuffer / depth-stencil pipeline, and a
    # depth-tested triangle drawn + presented on the real GPU.
    add_executable(integration_real_depth integration/integration_real_depth.cpp)
    target_link_libraries(integration_real_depth PRIVATE vkrelay2_worker_vulkan vkrelay2_warnings)
    add_test(NAME integration_real_depth COMMAND integration_real_depth)

    # Texture/sampler path: an R8G8B8A8_UNORM SAMPLED texture uploaded via the staging path
    # (TRANSFER_SRC buffer -> copy_buffer_to_image -> SHADER_READ_ONLY barriers), a NEAREST/CLAMP
    # sampler, and a 2-binding (UBO + combined-image-sampler) descriptor set -- a textured triangle
    # drawn + presented on the real GPU, plus the real-path negatives.
    add_executable(integration_real_texture integration/integration_real_texture.cpp)
    target_link_libraries(integration_real_texture PRIVATE vkrelay2_worker_vulkan vkrelay2_warnings)
    add_test(NAME integration_real_texture COMMAND integration_real_texture)

    # Depth/stencil copy_buffer_to_image (zink's depth-texture staging path): a D32_SFLOAT depth
    # round-trip -- upload a buffer of known depth values into a depth image (DEPTH aspect), copy it
    # back, and byte-compare -- proving the widened copy carries depth data faithfully, not just that
    # the submit succeeds.
    add_executable(integration_real_depth_upload integration/integration_real_depth_upload.cpp)
    target_link_libraries(integration_real_depth_upload PRIVATE vkrelay2_worker_vulkan
                          vkrelay2_warnings)
    add_test(NAME integration_real_depth_upload COMMAND integration_real_depth_upload)

    # real-GPU proof that a wedged vkAcquireNextImageKHR (exhausted swapchain, infinite
    # guest timeout) is INTERRUPTIBLE -- abort_session() releases it within a bounded deadline instead
    # of parking the pump thread forever. Hard TIMEOUT bounds a regression that fails to release.
    add_executable(integration_real_acquire_abort integration/integration_real_acquire_abort.cpp)
    target_link_libraries(integration_real_acquire_abort PRIVATE vkrelay2_worker_vulkan
                          vkrelay2_warnings)
    add_test(NAME integration_real_acquire_abort COMMAND integration_real_acquire_abort)
    set_tests_properties(integration_real_acquire_abort PROPERTIES TIMEOUT 60)

    # Literal-vkcube shape: a bufferless spinning cube (geometry inside the
    # UBO, indexed by gl_VertexIndex; draw of 36), a 2-binding (UBO + combined-image-sampler) set, a
    # D16_UNORM depth attachment, and a sampled texture uploaded via vkcube's runtime path choice
    # (linear-mapped if the driver advertises it, else staging). The deterministic regression half of
    # the "vkcube on screen" gate (run_vkcube.sh is the on-screen half).
    add_executable(integration_real_cube integration/integration_real_cube.cpp)
    target_link_libraries(integration_real_cube PRIVATE vkrelay2_worker_vulkan vkrelay2_warnings)
    add_test(NAME integration_real_cube COMMAND integration_real_cube)

    # Worker-present extent negotiation: the app chooses its own swapchain extent and the worker
    # converges the host Win32 window to it -- a non-square 503x377 swapchain is honored (HWND client
    # rect == requested, framebuffer accepted), the advertised range is the work-area (not the 256
    # window), and an unrealizable request returns VK_ERROR_OUT_OF_DATE_KHR rather than a stale extent.
    add_executable(integration_real_extent integration/integration_real_extent.cpp)
    target_link_libraries(integration_real_extent PRIVATE vkrelay2_worker_vulkan vkrelay2_warnings)
    add_test(NAME integration_real_extent COMMAND integration_real_extent)
endif()

# Shell-agnostic verifier used by both smoke wrappers.
add_executable(vkrelay2-argv-verify canaries/argv_verify_main.cpp)
target_link_libraries(vkrelay2-argv-verify PRIVATE vkrelay2_common vkrelay2_warnings)

# Raw-byte emitter for the byte-exact streaming regression.
add_executable(vkrelay2-byte-emitter canaries/byte_emitter_main.cpp)
target_link_libraries(vkrelay2-byte-emitter PRIVATE vkrelay2_warnings)

# --- Smoke: Bash argv/env/cwd preservation through the launcher -----------
# On Windows prefer Git Bash (MSYS); the System32 "bash.exe" is the WSL stub,
# which cannot see a Windows-path script. On Linux use the default bash.
if(WIN32)
    find_program(VKRELAY2_BASH NAMES bash
                 PATHS "$ENV{ProgramFiles}/Git/bin" "$ENV{ProgramFiles}/Git/usr/bin"
                       "$ENV{ProgramW6432}/Git/bin" "C:/Program Files/Git/bin"
                 NO_DEFAULT_PATH)
else()
    find_program(VKRELAY2_BASH NAMES bash)
endif()
if(VKRELAY2_BASH)
    add_test(NAME smoke_argv_echo_bash
             COMMAND ${VKRELAY2_BASH} ${CMAKE_CURRENT_SOURCE_DIR}/smoke/argv_echo_bash.sh
                     $<TARGET_FILE:vkrelay2-launch> $<TARGET_FILE:vkrelay2-argv-echo>
                     $<TARGET_FILE:vkrelay2-argv-verify>)
    add_test(NAME smoke_run_byte_exact_bash
             COMMAND ${VKRELAY2_BASH} ${CMAKE_CURRENT_SOURCE_DIR}/smoke/run_byte_exact_bash.sh
                     $<TARGET_FILE:vkrelay2-launch> $<TARGET_FILE:vkrelay2-byte-emitter>)
    # GL-on-zink is the launcher default (no --frontend flag), vulkan13 is the only opt-out.
    # Pure-function assertion of vkr_gl_frontend_env -- no session/display, so it runs anywhere.
    add_test(NAME smoke_gl_frontend_env_bash
             COMMAND ${VKRELAY2_BASH} ${CMAKE_CURRENT_SOURCE_DIR}/smoke/gl_frontend_env_bash.sh)
    # A pure-X11 target never produces Vulkan App-socket EOF; the wrapper's cleanup edge must send
    # the authenticated explicit worker close using the exact SessionStarted id/token.
    add_test(NAME smoke_session_close_cleanup_bash
             COMMAND ${VKRELAY2_BASH}
                     ${CMAKE_CURRENT_SOURCE_DIR}/smoke/session_close_cleanup_bash.sh)
    # a normal launch auto-selects a checkout-local guarded Xwayland only when its build
    # provenance exactly matches the host; mismatched distro/arch/package/build-id stays rejected.
    add_test(NAME smoke_xwayland_selection_bash
             COMMAND ${VKRELAY2_BASH} ${CMAKE_CURRENT_SOURCE_DIR}/smoke/xwayland_selection_bash.sh)
    # app-lifetime-teardown: signal the launcher, assert the signal-ignoring app GROUP dies (pins the
    # run_managed_child wiring). POSIX-only (process groups / pgrep / kill -TERM + the launcher's
    # signal management are POSIX); daemon-free.
    if(NOT WIN32)
        add_test(NAME smoke_launch_teardown_bash
                 COMMAND ${VKRELAY2_BASH} ${CMAKE_CURRENT_SOURCE_DIR}/smoke/launch_teardown_bash.sh
                         $<TARGET_FILE:vkrelay2-launch>)
        # WSLg's read-only X11 socket directory requires a private mount namespace. Exercise the
        # actual double re-exec and prove that the application-facing process retains caller uid/gid.
        add_test(NAME smoke_x11_namespace_identity_bash
                 COMMAND ${VKRELAY2_BASH}
                         ${CMAKE_CURRENT_SOURCE_DIR}/smoke/x11_namespace_identity_bash.sh)
    endif()
endif()

# --- Smoke: PowerShell argv/env/cwd preservation through the launcher ------
if(WIN32)
    find_program(POWERSHELL_EXECUTABLE NAMES pwsh powershell)
    if(POWERSHELL_EXECUTABLE)
        add_test(NAME smoke_argv_echo_powershell
                 COMMAND ${POWERSHELL_EXECUTABLE} -NoProfile -ExecutionPolicy Bypass -File
                         ${CMAKE_CURRENT_SOURCE_DIR}/smoke/argv_echo_powershell.ps1
                         -LaunchBin $<TARGET_FILE:vkrelay2-launch>
                         -EchoBin $<TARGET_FILE:vkrelay2-argv-echo>
                         -VerifyBin $<TARGET_FILE:vkrelay2-argv-verify>)
    endif()
endif()
