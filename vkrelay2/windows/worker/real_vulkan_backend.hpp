// Loader-backed Vulkan backend for the worker.
//
// Lives only in the worker -- the supervisor never loads a Vulkan driver. Built
// only on Windows when the Vulkan SDK is present (see CMakeLists), selected at
// runtime via `--vulkan-backend real`. Implemented against the real host loader:
// capability negotiation, the instance/device creation spine,
// device queues, command pool/buffers, and the device-child
// sync + memory objects -- fences, semaphores, device memory. The
// presentation spine (surface/swapchain) is stubbed "not implemented" on
// the real path pending the real Win32-window + WSI work; the mock models its
// handle lifecycle. The mock remains the headless/dual-platform path throughout.
#ifndef VKRELAY2_WINDOWS_WORKER_REAL_VULKAN_BACKEND_HPP
#define VKRELAY2_WINDOWS_WORKER_REAL_VULKAN_BACKEND_HPP

#include "common/vkrpc/vulkan_session.hpp"
#include "windows/worker/windowing/window_thread.hpp"

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

namespace vkr::worker {

class RealVulkanBackend : public vkrpc::VulkanBackend, public sidecar::SidecarBackend {
  public:
    // gpu_required marks a faithful launch (the daemon resolved a concrete real device): the worker
    // selects exactly that device -- by gpu_luid (the stable key) when present, else by exact
    // gpu_name -- and FAILS CLOSED if it is absent, never substituting another. When gpu_required
    // is false (auto/mock-world) selection stays lenient (name substring, then discrete/first), and
    // gpu_name is display text. gpu_luid is preferred over gpu_name whenever both are present.
    RealVulkanBackend(std::string gpu_name, std::string gpu_luid, bool gpu_required,
                      const display::DisplayLayout* display_layout = nullptr);
    ~RealVulkanBackend() override; // destroys any instances/devices still live

    // Implemented against the real loader: capability negotiation, the
    // instance/device creation spine, and real device queues.
    vkrpc::CapabilitiesResponse negotiate(const vkrpc::CapabilitiesRequest& req) override;
    // Sidecar plane: RealVulkanBackend is the single session-owned object spanning the
    // Vulkan data plane AND the sidecar plane, so the window registry it will hold can
    // join guest toplevels to surfaces/HWNDs and enforce.
    sidecar::SidecarNegotiateResponse
    negotiate(const sidecar::SidecarNegotiateRequest& req) override;
    sidecar::SidecarReadyResponse sidecar_ready(const sidecar::SidecarReadyRequest& req) override;
    // Worker-home toplevel registry lifecycle: drives the SAME pure WindowRegistry as
    // the mock (mock == real state machine), then executes its effects against real placeholder
    // HWNDs on the window thread. The registry transition runs under backend_mutex_; the HWND
    // create/destroy is marshaled onto the window thread OFF-lock (never hold the mutex across an
    // invoke), with a post-create re-check that discards a placeholder a racing surface superseded.
    sidecar::SidecarToplevelResponse
    register_toplevel(const sidecar::SidecarRegisterToplevelRequest& req) override;
    sidecar::SidecarToplevelResponse
    update_toplevel(const sidecar::SidecarUpdateToplevelRequest& req) override;
    sidecar::SidecarToplevelResponse
    unregister_toplevel(const sidecar::SidecarUnregisterToplevelRequest& req) override;
    // (show/hide lifecycle): apply a sidecar-authored live visibility change to the
    // host window (SW_HIDE / SW_SHOWNA), with the reveal predicate (intent == Visible &&
    // paint-eligible) and the owned-popup cascade. A hide PRESERVES the representation (no
    // teardown).
    sidecar::SidecarToplevelResponse
    set_visibility(const sidecar::SidecarSetVisibilityRequest& req) override;
    // Chrome pixels: paint captured BGRA into the placeholder's window-thread DIB via
    // the accept -> paint -> commit dance (shown/last_seq commit only after a realized paint);
    // debug_chrome_state answers the worker-visible test query by sampling that DIB.
    sidecar::SidecarPaintResponse
    paint_chrome(const sidecar::SidecarPaintChromeRequest& req) override;
    sidecar::SidecarDebugChromeStateResponse
    debug_chrome_state(const sidecar::SidecarDebugChromeStateRequest& req) override;
    // Input plane: the WndProc fills input_queue_ on the window thread; poll_input
    // drains it (under the ring's own mutex) and drops events whose xid no longer has a live
    // registry representation (the staleness gate, under backend_mutex_ -- never nested with the
    // ring mutex). debug_enqueue_input is the token-gated WndProc stand-in the WSL boundary smoke
    // drives. Both run the SAME pure InputQueue + registry-liveness logic as the mock (mock ==
    // real).
    sidecar::SidecarPollInputResponse
    poll_input(const sidecar::SidecarPollInputRequest& req) override;
    sidecar::SidecarDebugEnqueueInputResponse
    debug_enqueue_input(const sidecar::SidecarDebugEnqueueInputRequest& req) override;
    // Cursor plane: set_cursor finds the xid's window (surface OR placeholder) and
    // builds + binds an HCURSOR from the guest's BGRA image on the window thread (WM_SETCURSOR
    // applies it); debug_cursor_state samples that built cursor for the worker-visible proof. mock
    // == real (the mock stores a synthetic per-xid cursor buffer).
    sidecar::SidecarSetCursorResponse
    set_cursor(const sidecar::SidecarSetCursorRequest& req) override;
    sidecar::SidecarDebugCursorStateResponse
    debug_cursor_state(const sidecar::SidecarDebugCursorStateRequest& req) override;
    // a registry-derived snapshot under the backend mutex (mock == real -- same
    // WindowRegistry, same mapping; no HWND/Win32 in the response).
    sidecar::SidecarDebugEnumWindowsResponse
    debug_enum_windows(const sidecar::SidecarDebugEnumWindowsRequest& req) override;
    // capture a window's full source-layer BGRA (chrome DIB / cursor) off the window
    // thread, with the lifecycle selectors + frame cap. mock == real.
    sidecar::SidecarDebugCaptureWindowResponse
    debug_capture_window(const sidecar::SidecarDebugCaptureWindowRequest& req) override;
    vkrpc::CreateInstanceResponse create_instance(const vkrpc::CreateInstanceRequest& req) override;
    vkrpc::EnumeratePhysicalDevicesResponse
    enumerate_physical_devices(const vkrpc::EnumeratePhysicalDevicesRequest& req) override;
    vkrpc::CreateDeviceResponse create_device(const vkrpc::CreateDeviceRequest& req) override;
    vkrpc::StatusResponse destroy_device(const vkrpc::HandleRequest& req) override;
    vkrpc::StatusResponse destroy_instance(const vkrpc::HandleRequest& req) override;
    vkrpc::GetDeviceQueueResponse
    get_device_queue(const vkrpc::GetDeviceQueueRequest& req) override;
    vkrpc::CreateCommandPoolResponse
    create_command_pool(const vkrpc::CreateCommandPoolRequest& req) override;
    vkrpc::StatusResponse destroy_command_pool(const vkrpc::HandleRequest& req) override;
    vkrpc::AllocateCommandBuffersResponse
    allocate_command_buffers(const vkrpc::AllocateCommandBuffersRequest& req) override;
    vkrpc::StatusResponse
    free_command_buffers(const vkrpc::FreeCommandBuffersRequest& req) override;
    vkrpc::CreateFenceResponse create_fence(const vkrpc::CreateFenceRequest& req) override;
    vkrpc::StatusResponse destroy_fence(const vkrpc::HandleRequest& req) override;
    vkrpc::CreateSemaphoreResponse
    create_semaphore(const vkrpc::CreateSemaphoreRequest& req) override;
    vkrpc::StatusResponse destroy_semaphore(const vkrpc::HandleRequest& req) override;
    vkrpc::AllocateMemoryResponse allocate_memory(const vkrpc::AllocateMemoryRequest& req) override;
    vkrpc::StatusResponse free_memory(const vkrpc::HandleRequest& req) override;

    // Presentation spine: real WSI plumbing. A surface is a hidden
    // Win32 window (owned by the window thread) + VkSurfaceKHR; a swapchain is a real
    // VkSwapchainKHR on it; get_swapchain_images enumerates its images. No present yet
    // (acquire/present and showing the window are handled separately).
    vkrpc::CreateSurfaceResponse create_surface(const vkrpc::CreateSurfaceRequest& req) override;
    vkrpc::StatusResponse destroy_surface(const vkrpc::HandleRequest& req) override;
    vkrpc::CreateSwapchainResponse
    create_swapchain(const vkrpc::CreateSwapchainRequest& req) override;
    vkrpc::StatusResponse destroy_swapchain(const vkrpc::HandleRequest& req) override;
    vkrpc::GetSwapchainImagesResponse
    get_swapchain_images(const vkrpc::GetSwapchainImagesRequest& req) override;

    // Acquire + worker-present. HWND-touching WSI calls run on the
    // window thread; the session thread holds the per-surface slot lock across the
    // routed call; while the surface is geometry-dirty acquire/present return
    // VK_ERROR_OUT_OF_DATE_KHR without calling the host driver. The window shows on the
    // first successful present.
    vkrpc::AcquireNextImageResponse
    acquire_next_image(const vkrpc::AcquireNextImageRequest& req) override;
    vkrpc::QueuePresentResponse queue_present(const vkrpc::QueuePresentRequest& req) override;

    // session-abort hook (called by the liveness observer thread on app peer-close).
    // Thread-safe + nonblocking -- only stores the atomic the acquire poll checks between quanta,
    // so an in-flight infinite acquire releases within one quantum and the session can tear down
    // even when the app died mid-acquire.
    void abort_session() override { session_aborting_.store(true, std::memory_order_relaxed); }

    // Command recording + queue submit. record_command_buffer replays
    // the app's whole command stream into the real VkCommandBuffer; queue_submit submits
    // recorded buffers with real binary semaphores/fences; reset_fences / wait_for_fences
    // drive fence sync. record + submit extend the serialization: a command stream that
    // touches swapchain images holds the referenced surfaces' slot locks and runs the host
    // calls on the window thread. queue_submit returns the honest VkResult (no OUT_OF_DATE
    // synthesis); wait_for_fences reports VK_TIMEOUT honestly.
    vkrpc::StatusResponse
    record_command_buffer(const vkrpc::RecordCommandBufferRequest& req) override;
    vkrpc::QueueSubmitResponse queue_submit(const vkrpc::QueueSubmitRequest& req) override;
    vkrpc::QueueSubmitResponse queue_submit2(const vkrpc::QueueSubmit2Request& req) override;
    vkrpc::StatusResponse reset_fences(const vkrpc::ResetFencesRequest& req) override;
    vkrpc::WaitForFencesResponse wait_for_fences(const vkrpc::WaitForFencesRequest& req) override;
    // (core-1.0 sync honesty): fence status poll (VK_SUCCESS/VK_NOT_READY forwarded
    // verbatim, the WaitForFences idiom) + the VkEvent object model (create/destroy + host
    // status/set/reset). The sync1 command events ride record_command_buffer.
    vkrpc::GetFenceStatusResponse get_fence_status(const vkrpc::HandleRequest& req) override;
    vkrpc::CreateEventResponse create_event(const vkrpc::CreateEventRequest& req) override;
    vkrpc::StatusResponse destroy_event(const vkrpc::HandleRequest& req) override;
    vkrpc::GetEventStatusResponse get_event_status(const vkrpc::HandleRequest& req) override;
    vkrpc::StatusResponse set_event(const vkrpc::HandleRequest& req) override;
    vkrpc::StatusResponse reset_event(const vkrpc::HandleRequest& req) override;
    // REAL idle waits (vkQueueWaitIdle / vkDeviceWaitIdle), so an ICD-side
    // idle wait is an honest readback-completion sync point (was a local success stub).
    vkrpc::WaitIdleResponse queue_wait_idle(const vkrpc::HandleRequest& req) override;
    vkrpc::WaitIdleResponse device_wait_idle(const vkrpc::HandleRequest& req) override;
    // (GL/zink): host-side timeline-semaphore ops.
    vkrpc::WaitSemaphoresResponse wait_semaphores(const vkrpc::WaitSemaphoresRequest& req) override;
    vkrpc::StatusResponse signal_semaphore(const vkrpc::SignalSemaphoreRequest& req) override;
    vkrpc::GetSemaphoreCounterValueResponse
    get_semaphore_counter_value(const vkrpc::GetSemaphoreCounterValueRequest& req) override;

    // App-facing WSI capability queries: forward the host surface queries
    // (run on the window thread, since they touch the Win32 surface/HWND). Surface
    // capabilities report currentExtent = 0xFFFFFFFF -- the host's concrete extent never
    // crosses the wire -- with honest supportedUsageFlags/formats/present-modes.
    vkrpc::GetSurfaceCapabilitiesResponse
    get_surface_capabilities(const vkrpc::GetSurfaceCapabilitiesRequest& req) override;
    vkrpc::GetSurfaceFormatsResponse
    get_surface_formats(const vkrpc::GetSurfaceFormatsRequest& req) override;
    vkrpc::GetSurfacePresentModesResponse
    get_surface_present_modes(const vkrpc::GetSurfacePresentModesRequest& req) override;
    vkrpc::GetSurfaceSupportResponse
    get_surface_support(const vkrpc::GetSurfaceSupportRequest& req) override;

    // Draw surface: image views / shader modules / render pass / framebuffer /
    // pipeline layout / graphics pipeline, implemented against the host driver with the
    // bufferless-triangle bounded subset and destroy ordering mirrored by the mock.
    vkrpc::CreateImageViewResponse
    create_image_view(const vkrpc::CreateImageViewRequest& req) override;
    vkrpc::StatusResponse destroy_image_view(const vkrpc::HandleRequest& req) override;
    vkrpc::CreateBufferViewResponse
    create_buffer_view(const vkrpc::CreateBufferViewRequest& req) override;
    vkrpc::StatusResponse destroy_buffer_view(const vkrpc::HandleRequest& req) override;
    vkrpc::CreateShaderModuleResponse
    create_shader_module(const vkrpc::CreateShaderModuleRequest& req) override;
    vkrpc::StatusResponse destroy_shader_module(const vkrpc::HandleRequest& req) override;
    vkrpc::CreateRenderPassResponse
    create_render_pass(const vkrpc::CreateRenderPassRequest& req) override;
    vkrpc::StatusResponse destroy_render_pass(const vkrpc::HandleRequest& req) override;
    vkrpc::CreateFramebufferResponse
    create_framebuffer(const vkrpc::CreateFramebufferRequest& req) override;
    vkrpc::StatusResponse destroy_framebuffer(const vkrpc::HandleRequest& req) override;
    vkrpc::CreatePipelineLayoutResponse
    create_pipeline_layout(const vkrpc::CreatePipelineLayoutRequest& req) override;
    vkrpc::StatusResponse destroy_pipeline_layout(const vkrpc::HandleRequest& req) override;
    vkrpc::CreateGraphicsPipelinesResponse
    create_graphics_pipelines(const vkrpc::CreateGraphicsPipelinesRequest& req) override;
    vkrpc::CreateComputePipelinesResponse
    create_compute_pipelines(const vkrpc::CreateComputePipelinesRequest& req) override;
    vkrpc::StatusResponse destroy_pipeline(const vkrpc::HandleRequest& req) override;

    // Host-visible memory + buffers: honest host memory properties + buffer
    // requirements, real-backed buffers/bind, and write_memory_ranges (map/scatter/flush/unmap the
    // real VkDeviceMemory, host-visible/coherent only). allocate_memory/free_memory (above) already
    // back real allocations; records their size + property flags for the upload path.
    vkrpc::GetPhysicalDeviceMemoryPropertiesResponse get_physical_device_memory_properties(
        const vkrpc::GetPhysicalDeviceMemoryPropertiesRequest& req) override;
    vkrpc::CreateBufferResponse create_buffer(const vkrpc::CreateBufferRequest& req) override;
    vkrpc::StatusResponse destroy_buffer(const vkrpc::HandleRequest& req) override;
    vkrpc::StatusResponse bind_buffer_memory(const vkrpc::BindBufferMemoryRequest& req) override;
    // (bufferDeviceAddress): the real host VkDeviceAddress verbatim, via the core PFN
    // resolved at create_device. Fail-closed on a device without the feature, an unknown/unbound
    // buffer, or one created without SHADER_DEVICE_ADDRESS usage.
    vkrpc::GetBufferDeviceAddressResponse
    get_buffer_device_address(const vkrpc::GetBufferDeviceAddressRequest& req) override;
    vkrpc::StatusResponse write_memory_ranges(const vkrpc::WriteMemoryRangesRequest& req) override;

    // Descriptor surface + per-frame UBO: the descriptor object graph against
    // the host driver, with the same device-scoped validate-then-poison semantics as the mock
    // oracle (a rejected update poisons its target sets so a previously-valid set cannot stay
    // draw-ready); layout-exact bind/draw + the set->buffer destroy consult. UNIFORM_BUFFER-only.
    vkrpc::CreateDescriptorSetLayoutResponse
    create_descriptor_set_layout(const vkrpc::CreateDescriptorSetLayoutRequest& req) override;
    // descriptorIndexing: the honest layout-support query (the host's real answer).
    vkrpc::GetDescriptorSetLayoutSupportResponse
    get_descriptor_set_layout_support(const vkrpc::CreateDescriptorSetLayoutRequest& req) override;
    // Vulkan 1.3 support (maintenance4): create-info-shaped memory-requirement queries.
    vkrpc::CreateBufferResponse
    get_device_buffer_memory_requirements(const vkrpc::CreateBufferRequest& req) override;
    vkrpc::CreateImageResponse
    get_device_image_memory_requirements(const vkrpc::CreateImageRequest& req) override;
    vkrpc::StatusResponse destroy_descriptor_set_layout(const vkrpc::HandleRequest& req) override;
    vkrpc::CreateDescriptorPoolResponse
    create_descriptor_pool(const vkrpc::CreateDescriptorPoolRequest& req) override;
    vkrpc::StatusResponse destroy_descriptor_pool(const vkrpc::HandleRequest& req) override;
    vkrpc::AllocateDescriptorSetsResponse
    allocate_descriptor_sets(const vkrpc::AllocateDescriptorSetsRequest& req) override;
    vkrpc::StatusResponse
    update_descriptor_sets(const vkrpc::UpdateDescriptorSetsRequest& req) override;

    // Textures + depth = literal vkcube : honest format properties,
    // app-created images (texture + depth) with real memory requirements (+ LINEAR subresource
    // layout), bound to a device-local OR host-visible memory class, against the host driver -- the
    // same bounded subset + destroy ordering as the mock oracle.
    vkrpc::GetPhysicalDeviceFormatPropertiesResponse get_physical_device_format_properties(
        const vkrpc::GetPhysicalDeviceFormatPropertiesRequest& req) override;
    // (GL/zink): honest feature + image-format-support forwarding from the real host
    // device.
    vkrpc::GetPhysicalDeviceFeaturesResponse
    get_physical_device_features(const vkrpc::GetPhysicalDeviceFeaturesRequest& req) override;
    vkrpc::GetPhysicalDeviceImageFormatPropertiesResponse
    get_physical_device_image_format_properties(
        const vkrpc::GetPhysicalDeviceImageFormatPropertiesRequest& req) override;
    vkrpc::GetPhysicalDevicePropertiesResponse
    get_physical_device_properties(const vkrpc::GetPhysicalDevicePropertiesRequest& req) override;
    vkrpc::GetPhysicalDeviceCapabilityChainResponse get_physical_device_capability_chain(
        const vkrpc::GetPhysicalDeviceCapabilityChainRequest& req) override;
    vkrpc::CreateImageResponse create_image(const vkrpc::CreateImageRequest& req) override;
    vkrpc::StatusResponse destroy_image(const vkrpc::HandleRequest& req) override;
    vkrpc::StatusResponse bind_image_memory(const vkrpc::BindImageMemoryRequest& req) override;

    // Sampler + combined-image-sampler + texture upload : the bounded
    // sampler against the host driver; the combined-image-sampler descriptor type + the texture
    // upload commands (copy_buffer_to_image + the generalized pipeline_barrier) extend
    // update_descriptor_sets / record_command_buffer in place (no new override) -- the same bounded
    // subset, validate-then-poison, and destroy-consult as the mock oracle.
    vkrpc::CreateSamplerResponse create_sampler(const vkrpc::CreateSamplerRequest& req) override;
    vkrpc::StatusResponse destroy_sampler(const vkrpc::HandleRequest& req) override;
    vkrpc::CreateQueryPoolResponse
    create_query_pool(const vkrpc::CreateQueryPoolRequest& req) override;
    vkrpc::StatusResponse destroy_query_pool(const vkrpc::HandleRequest& req) override;
    vkrpc::GetQueryPoolResultsResponse
    get_query_pool_results(const vkrpc::GetQueryPoolResultsRequest& req) override;
    vkrpc::StatusResponse reset_query_pool(const vkrpc::ResetQueryPoolRequest& req) override;
    vkrpc::ReadMemoryRangesResponse
    read_memory_ranges(const vkrpc::ReadMemoryRangesRequest& req) override;

    // Test-only seam (Windows in-process latch test): the HWND backing a surface, so a
    // test can drive a real WM_SIZE (SetWindowPos) and exercise the latch. Returns
    // nullptr for an unknown surface. Not part of the RPC surface.
    HWND debug_surface_hwnd(std::uint64_t surface) const;
    // Lost-device containment regression seams. Feed the shared observer a synthetic result, then
    // inspect the sticky latch; the real device remains healthy, so post-latch DEVICE_LOST answers
    // prove those operations stayed local. Not part of the RPC surface.
    int debug_observe_device_result(std::uint64_t device, int result, const char* where);
    bool debug_device_lost_latched(std::uint64_t device) const {
        return device_lost_latched(device);
    }
    bool debug_instance_surface_maintenance1(std::uint64_t instance) const {
        const auto it = instances_.find(instance);
        return it != instances_.end() && it->second.surface_maintenance1;
    }
    bool debug_device_present_fence_retire(std::uint64_t device) const {
        const auto it = devices_.find(device);
        return it != devices_.end() && it->second.present_fence_retire;
    }
    // the surface born-correlated with a guest XID, or 0 if none (mock == real).
    std::uint64_t debug_registry_surface_for_xid(std::uint64_t xid) const {
        return registry_.surface_for_xid(xid);
    }
    // the xid's current representation epoch (the worker's exact-epoch input gate key).
    std::uint64_t debug_registry_epoch_for_xid(std::uint64_t xid) const {
        return registry_.epoch_for_xid(xid);
    }
    // whether the sidecar has emitted its readiness barrier on this session.
    bool debug_sidecar_ready() const { return sidecar_ready_; }
    // Structural seams (mock == real): the registry's view + the executor's view. The
    // HWND count must track the registry's placeholder count (the executor obeys the state
    // machine).
    sidecar::Representation debug_representation_for_xid(std::uint64_t xid) const {
        return registry_.representation_for_xid(xid);
    }
    bool debug_toplevel_registered(std::uint64_t xid) const {
        return registry_.toplevel_registered(xid);
    }
    std::size_t debug_registry_entry_count() const { return registry_.size(); }
    std::size_t debug_registry_placeholder_count() const { return registry_.placeholder_count(); }
    std::size_t debug_placeholder_hwnd_count() const { return placeholder_hwnds_.size(); }
    // The actual placeholder HWND backing `xid` (via the registry's placeholder id), or nullptr.
    // Lets the structural test prove a sibling toplevel's HWND survives when another is
    // unregistered.
    HWND debug_placeholder_hwnd_for_xid(std::uint64_t xid) const {
        const auto it = placeholder_hwnds_.find(registry_.placeholder_id_for_xid(xid));
        return it == placeholder_hwnds_.end() ? nullptr : it->second;
    }

  private:
    // Per-session object table mapping our opaque u64 handles to real Vk handles.
    // One app connection per worker, served single-threaded, so no locking. Like
    // the mock, the instance owns a single selected physical device (minted on
    // first enumerate) and tracks its live devices for destroy ordering.
    struct RealInstance {
        VkInstance vk = VK_NULL_HANDLE;
        std::uint64_t physical = 0; // device handle (0 = not enumerated)
        VkPhysicalDevice physical_vk = VK_NULL_HANDLE;
        std::set<std::uint64_t> devices;  // live device handles
        std::set<std::uint64_t> surfaces; // live surface handles (instance children)
        // cross-adapter retirement: the instance enabled VK_EXT_surface_maintenance1 (its
        // caps2 dependency) -- the prerequisite for enabling VK_EXT_swapchain_maintenance1 on a
        // device of this instance.
        bool surface_maintenance1 = false;
        // Lost-device containment: a device of this instance was latched
        // lost and is ABANDONED (never vkDestroyDevice'd). Instance-level host teardown
        // (vkDestroySurfaceKHR / vkDestroyInstance) must then be skipped too -- tearing the
        // instance down under a live abandoned device child is UB (observed AV). STICKY: survives
        // the lost device's table erase; the whole instance is abandoned to process termination.
        bool lost_device_taint = false;
    };
    struct RealDevice {
        VkDevice vk = VK_NULL_HANDLE;
        std::uint64_t instance = 0;     // parent
        std::uint32_t queue_family = 0; // the family create_device made queues from
        std::uint32_t queue_count = 0;  // queues created in that family
        // Lost-device latch (the original vkrelay `lost_devices` lesson): set
        // when ANY host command on this device returns VK_ERROR_DEVICE_LOST. Once set, no later
        // acquire/submit/present/wait/recreate re-enters the host driver -- a post-loss
        // vkAcquireNextImageKHR can block forever on the window thread (holding slot locks), which
        // turned the recoverable loss into the observed hard hang. Post-latch calls answer an
        // honest VK_ERROR_DEVICE_LOST locally; teardown skips idle-waits + abandons the device's
        // host objects to process termination (per-session worker) rather than touch a wedged
        // driver.
        bool lost = false;
        // cross-adapter retirement experiment: VK_EXT_swapchain_maintenance1 present fences
        // are active on this device (host supports + feature enabled privately; never advertised
        // to the guest). Presents then carry a per-swapchain fence, and a recreate waits the
        // retiring swapchain's outstanding present fences before handing it to oldSwapchain.
        bool present_fence_retire = false;
        // packed(family,index) -> our stable queue handle. VkQueue is owned by the
        // device (never destroyed) and re-derivable via vkGetDeviceQueue, so only
        // the stable handle is cached; it dies with the device's table entry.
        std::map<std::uint64_t, std::uint64_t> queues;
        std::set<std::uint64_t> pools;      // live command pool handles (destroy ordering)
        std::set<std::uint64_t> leaves;     // live fence/semaphore/memory handles
        std::set<std::uint64_t> swapchains; // live swapchain handles (device children)
        // Draw-surface device children: each blocks the device's destroy until freed.
        std::set<std::uint64_t> image_views;
        std::set<std::uint64_t> shader_modules;
        std::set<std::uint64_t> render_passes;
        std::set<std::uint64_t> framebuffers;
        std::set<std::uint64_t> pipeline_layouts;
        std::set<std::uint64_t> pipelines;
        std::set<std::uint64_t> buffers; // device children (block destroy until freed)
        // Descriptor surface device children: each blocks the device's destroy.
        std::set<std::uint64_t> descriptor_set_layouts;
        std::set<std::uint64_t> descriptor_pools;
        std::set<std::uint64_t> images;      // app-created images (block destroy until freed)
        std::set<std::uint64_t> samplers;    // Device children; block destroy until freed.
        std::set<std::uint64_t> query_pools; // query pools (block destroy until freed)
        // (GL/zink): the extensions ENABLED on this real device (create_device's
        // host-intersected list) -- extension commands validate against it (mock == real), and
        // the matching PFNs are resolved once at create_device (null when not enabled, and the
        // validation guarantees a null PFN is never reached at emit).
        std::set<std::string> enabled_extensions;
        PFN_vkCmdBindTransformFeedbackBuffersEXT pfn_bind_tf_buffers = nullptr;
        PFN_vkCmdBeginTransformFeedbackEXT pfn_begin_tf = nullptr;
        PFN_vkCmdEndTransformFeedbackEXT pfn_end_tf = nullptr;
        PFN_vkCmdDrawIndirectByteCountEXT pfn_draw_indirect_byte_count = nullptr;
        PFN_vkCmdBeginConditionalRenderingEXT pfn_begin_cond_render = nullptr;
        PFN_vkCmdEndConditionalRenderingEXT pfn_end_cond_render = nullptr;
        // (native lane -- EDS1): VK_EXT_extended_dynamic_state's setters replay through the
        // *EXT PFNs, so we honestly serve the ADVERTISED extension (not core-1.3) on the interim
        // 1.2+extension lane. Resolved once at create_device iff the extension is
        // enabled; the EDS validate gate guarantees a null PFN is never reached at emit.
        // Vulkan 1.3 support: the full twelve EDS1 setters, resolved *EXT-name-first with a
        // core-name fallback (an honest vk13 device may be served by a promoted-core host with no
        // *EXT aliases); the gate widens to extension-or-vk13_device.
        PFN_vkCmdSetCullModeEXT pfn_set_cull_mode = nullptr;
        PFN_vkCmdSetFrontFaceEXT pfn_set_front_face = nullptr;
        PFN_vkCmdSetPrimitiveTopologyEXT pfn_set_primitive_topology = nullptr;
        PFN_vkCmdSetDepthTestEnableEXT pfn_set_depth_test_enable = nullptr;
        PFN_vkCmdSetDepthWriteEnableEXT pfn_set_depth_write_enable = nullptr;
        PFN_vkCmdSetDepthCompareOpEXT pfn_set_depth_compare_op = nullptr;
        PFN_vkCmdSetViewportWithCountEXT pfn_set_viewport_with_count = nullptr;
        PFN_vkCmdSetScissorWithCountEXT pfn_set_scissor_with_count = nullptr;
        PFN_vkCmdBindVertexBuffers2EXT pfn_bind_vertex_buffers2 = nullptr;
        PFN_vkCmdSetDepthBoundsTestEnableEXT pfn_set_depth_bounds_test_enable = nullptr;
        PFN_vkCmdSetStencilTestEnableEXT pfn_set_stencil_test_enable = nullptr;
        PFN_vkCmdSetStencilOpEXT pfn_set_stencil_op = nullptr;
        // Vulkan 1.3 support (EDS2 subset): the three enable-toggles core 1.3 absorbed from
        // VK_EXT_extended_dynamic_state2. Resolved by CORE name only, iff vk13_device (the
        // extension itself is not advertised); the vk13 validate gate guarantees non-null at emit.
        PFN_vkCmdSetRasterizerDiscardEnableEXT pfn_set_rasterizer_discard_enable = nullptr;
        PFN_vkCmdSetDepthBiasEnableEXT pfn_set_depth_bias_enable = nullptr;
        PFN_vkCmdSetPrimitiveRestartEnableEXT pfn_set_primitive_restart_enable = nullptr;
        // (native lane): VK_KHR_dynamic_rendering's commands replay through the
        // *KHR PFNs, honestly serving the ADVERTISED extension (not core-1.3) on the interim
        // 1.2+extension lane. Resolved once at create_device iff the extension is enabled, with a
        // fail-closed null guard (a null PFN is never reached at emit).
        PFN_vkCmdBeginRenderingKHR pfn_begin_rendering = nullptr;
        PFN_vkCmdEndRenderingKHR pfn_end_rendering = nullptr;
        // The app must enable the dynamicRendering FEATURE, not just the
        // extension; DR commands + pipelines gate on ext AND this bit (mock == real).
        bool dynamic_rendering_feature_enabled = false;
        // (native lane): VK_KHR_synchronization2's commands replay through the
        // *KHR PFNs. Resolved once at create_device iff the extension is enabled, with ONE
        // fail-closed null guard over all six (a null PFN is never reached at emit). Every sync2
        // command additionally gates on ext AND the synchronization2 feature bit (mock == real).
        PFN_vkCmdPipelineBarrier2KHR pfn_pipeline_barrier2 = nullptr;
        PFN_vkCmdWriteTimestamp2KHR pfn_write_timestamp2 = nullptr;
        PFN_vkQueueSubmit2KHR pfn_queue_submit2 = nullptr;
        PFN_vkCmdSetEvent2KHR pfn_set_event2 = nullptr;
        PFN_vkCmdResetEvent2KHR pfn_reset_event2 = nullptr;
        PFN_vkCmdWaitEvents2KHR pfn_wait_events2 = nullptr;
        bool synchronization2_feature_enabled = false;
        // Required-feature audit (hardening): the enabled hostQueryReset
        // feature (core 1.2, no extension). reset_query_pool fails closed unless set (mock ==
        // real).
        bool host_query_reset_feature_enabled = false;
        // Required-feature audit: the enabled multiview feature (core 1.1, no
        // extension). create_render_pass fails closed a viewMask pass unless set; the worker
        // re-derives it from the forwarded chain and rejects a scalar/chain mismatch at
        // create_device (mock == real).
        bool multiview_feature_enabled = false;
        // (bufferDeviceAddress, core 1.2 -- no extension): the enabled feature bit gates
        // get_buffer_device_address + the DEVICE_ADDRESS allocate flag (mock == real). The CORE
        // PFN is resolved at create_device iff the feature is enabled, after verifying the host
        // actually supports it (fail-closed null guard, the recipe).
        PFN_vkGetBufferDeviceAddress pfn_get_buffer_device_address = nullptr;
        bool buffer_device_address_feature_enabled = false;
        // vertex-attr-divisor: the two enabled feature bits, NORMALIZED against the forwarded
        // feature chain at create_device (the multiview/BDA pattern) and re-asserted in
        // create_graphics_pipeline via vkrpc::vertex_binding_divisors_ok. Gate the divisor VALUES a
        // pipeline may carry (divisor != 1 needs the first, divisor == 0 additionally needs the
        // second). mock == real.
        bool vertex_attr_divisor_feature_enabled = false;
        bool vertex_attr_zero_divisor_feature_enabled = false;
        // geometry-stream: the enabled geometryStreams feature, NORMALIZED against the forwarded
        // VkPhysicalDeviceTransformFeedbackFeaturesEXT chain entry at create_device (the divisor
        // pattern), plus the host's two stream PROPERTIES cached once at create (immutable per
        // physical device; queried only when VK_EXT_transform_feedback is enabled). Together they
        // parameterize the shared vkrpc::rasterization_stream_ok gate on the stream pNext a
        // pipeline may carry (mock == real via the same helper on the mock's modeled values).
        bool geometry_streams_feature_enabled = false;
        // Core indirect draws: single-draw indirect needs no feature; drawCount > 1 requires the
        // enabled VkPhysicalDeviceFeatures::multiDrawIndirect bit.
        bool multi_draw_indirect_feature_enabled = false;
        std::uint32_t max_transform_feedback_streams = 0;
        bool transform_feedback_rasterization_stream_select = false;
        // Descriptor indexing: the enabled kDIFeature* bits, NORMALIZED against the
        // forwarded feature chain at create_device (the BDA pattern) and host-verified. Gates the
        // per-binding flag admission, UAB pools, and variable-count allocation (mock == real).
        std::uint64_t descriptor_indexing_feature_bits = 0;
        // Vulkan 1.3 support: the enabled kVk13Feature* bits (normalized + host-verified) and
        // whether this device is an honest apiVersion-1.3 device (host re-verified). vk13_device
        // admits the core-1.3 command surface (EDS core names, featureless DR/sync2, copy2);
        // the bits gate each feature's usage shapes. The maintenance4 query PFNs resolve iff the
        // feature is enabled (fail-closed null guard, the recipe).
        std::uint64_t vk13_feature_bits = 0;
        bool vk13_device = false;
        PFN_vkGetDeviceBufferMemoryRequirements pfn_get_device_buffer_memory_requirements = nullptr;
        PFN_vkGetDeviceImageMemoryRequirements pfn_get_device_image_memory_requirements = nullptr;
    };
    // Surface = a hidden Win32 window (owned by the window thread) + its VkSurfaceKHR.
    // Instance child; blocks the instance's destroy until destroyed, and blocks on its
    // own live swapchains.
    struct RealSurface {
        VkSurfaceKHR vk = VK_NULL_HANDLE;
        std::uint64_t instance = 0;            // parent
        HWND hwnd = nullptr;                   // window backing the surface (hidden -> shown)
        windowing::WindowSlot* slot = nullptr; // present/geometry state (lock + latch)
        bool shown = false;                    // flips on first successful present
        std::set<std::uint64_t> swapchains;
        // Born-correlated guest-window topology (advisory role_hint -- sidecar owns
        // role).
        std::string platform;
        std::uint64_t xid = 0;
        std::string role_hint = "UnknownPending";
        // (GL/zink): set once the app creates a swapchain that DEFERS the extent
        // (use_current_extent == the 0xFFFFFFFF sentinel; zink/kopper does this, native apps do
        // not). A deferring surface then advertises a CONCRETE, pinned currentExtent so kopper
        // converges instead of looping; a never-deferring (native) surface keeps the app-picks
        // range.
        bool defers_extent = false;
        // resize-diag (cross-monitor maximize hang isolation): count consecutive
        // get_surface_capabilities re-queries with NO successful present in between -- the kopper
        // recreate-loop signal when the pinned currentExtent exceeds the guest-realizable root (a
        // maximize on a monitor larger than the primary/guest root). Reset on a successful present.
        // Rate-limits the log (on extent change or every N cycles) so the diagnostic itself never
        // becomes the stall.
        std::uint32_t rz_cycles = 0;        // caps re-queries since the last successful present
        std::uint32_t rz_last_extent_w = 0; // last pinned extent we logged (log on change)
        std::uint32_t rz_last_extent_h = 0;
        // device-loss probe: the GDI device name of the monitor this surface's window was last
        // presented to (e.g. "\\.\DISPLAY2"). Log the present-target adapter context only when it
        // CHANGES (a drag onto another monitor) so the diagnostic is quiet in steady state but
        // captures every monitor transition -- the moment a cross-GPU present could lose the
        // device.
        std::string dl_last_monitor;
    };
    // Swapchain = a real VkSwapchainKHR on a device, targeting a surface. Its images
    // are swapchain-owned (no destroy) and minted on first get_swapchain_images: our
    // stable handle per image, parallel to the VkImage (used by acquire/present + the
    // app's recorded commands).
    struct RealSwapchain {
        VkSwapchainKHR vk = VK_NULL_HANDLE;
        std::uint64_t device = 0;                    // parent
        std::uint64_t surface = 0;                   // the surface it targets
        VkExtent2D extent{};                         // the swapchain's image extent
        VkFormat image_format = VK_FORMAT_UNDEFINED; // an image view must match this
        // The EFFECTIVE usage the worker created these images with (the app's request, plus the
        // debug-only TRANSFER_SRC when present-capture is armed). Recorded so the present-capture
        // artifact is self-describing.
        VkImageUsageFlags image_usage = 0;
        // Whether the images were created with VK_IMAGE_USAGE_TRANSFER_DST_BIT (derived
        // from the app's requested image_usage): a clear_color_image command needs it.
        bool transfer_dst = false;
        // present-capture (debug only): true iff the worker ADDED
        // VK_IMAGE_USAGE_TRANSFER_SRC_BIT to this swapchain -- done only when
        // VKRELAY2_DEBUG_DUMP_PRESENT is set AND the surface advertises TRANSFER_SRC. The
        // pre-present capture copies FROM the swapchain image, which requires it; false => the
        // capture is skipped (never an invalid copy from a non-TRANSFER_SRC image).
        bool transfer_src = false;
        std::vector<std::uint64_t> images;
        std::vector<VkImage> vk_images;
        std::set<std::uint64_t> image_views; // live views over its images (destroy ordering)
        // cross-adapter retirement experiment (device.present_fence_retire): the OUTSTANDING
        // worker-private present fences for this swapchain, submission order. Each queue_present
        // chains VkSwapchainPresentFenceInfoEXT with one fresh fence per target swapchain (reaping
        // already-signaled ones first, so the list stays ~swapchain-depth); a recreate/destroy
        // waits + destroys them all before the old swapchain is retired -- the spec-blessed "safe
        // to destroy" signal for a present's resources.
        std::vector<VkFence> present_fences;
    };
    // A swapchain image resolvable by handle: record_command_buffer resolves a
    // referenced image to its VkImage, device (same-device check), surface (the referenced-
    // surface set whose slot lock submit takes), and TRANSFER_DST support (clear check).
    struct RealImage {
        VkImage vk = VK_NULL_HANDLE;
        std::uint64_t device = 0;
        std::uint64_t swapchain = 0;
        std::uint64_t surface = 0;
        bool transfer_dst = false;
        VkFormat format = VK_FORMAT_UNDEFINED; // the swapchain format (image-view match)
        // App-created images. app_created distinguishes these from swapchain images
        // (swapchain == 0); the worker owns the VkImage (destroyed in destroy_image) and binds
        // memory to it (bind_image_memory enforces memoryTypeBits / alignment). aspect is the
        // image's natural aspect (COLOR texture / DEPTH).
        bool app_created = false;
        VkImageAspectFlags aspect = 0;
        VkImageTiling tiling = VK_IMAGE_TILING_OPTIMAL;
        std::uint64_t usage = 0;
        VkExtent2D extent{};
        // Mip/layer counts (create-carried) -- the copy_buffer_to_image widening validates each
        // region's subresource + mip-extent bounds against these (mock == real).
        std::uint32_t mip_levels = 1;
        std::uint32_t array_layers = 1;
        VkDeviceSize alignment = 0;
        std::uint32_t memory_type_bits = 0;
        std::uint64_t bound_memory = 0;
        // Live views over this app image: destroy is blocked while
        // any view references it -- the same parent/child rule as swapchain -> view.
        std::set<std::uint64_t> image_views;
    };
    // Draw-surface objects. Each is a device child (blocks the device's destroy until
    // freed). Reference fields are validated at create + drive record-time compatibility checks;
    // they are not extra destroy blockers (real Vulkan does not block a render-pass/layout destroy
    // on a live framebuffer/pipeline -- only on in-flight use), matching the mock.
    struct RealImageView {
        VkImageView vk = VK_NULL_HANDLE;
        std::uint64_t device = 0;
        std::uint64_t image = 0;     // the swapchain image it views
        std::uint64_t swapchain = 0; // a swapchain blocks its destroy on live views
        // (dynamic rendering): the view's VkFormat (create-carried). begin_rendering
        // derives the active DR scope's attachment formats from its attachment views; draw
        // validation compares them to the bound pipeline's declared formats (the DR compatibility
        // key).
        VkFormat format = VK_FORMAT_UNDEFINED;
    };
    // (GL/zink): a real texel buffer view over an app buffer.
    struct RealBufferView {
        VkBufferView vk = VK_NULL_HANDLE;
        std::uint64_t device = 0;
        std::uint64_t buffer = 0; // the source buffer (blocks its destroy on a live view)
    };
    struct RealShaderModule {
        VkShaderModule vk = VK_NULL_HANDLE;
        std::uint64_t device = 0;
    };
    struct RealRenderPass {
        VkRenderPass vk = VK_NULL_HANDLE;
        std::uint64_t device = 0;
        VkFormat color_format = VK_FORMAT_UNDEFINED; // compat key (framebuffer/pipeline must match)
        VkFormat depth_format = VK_FORMAT_UNDEFINED; // (UNDEFINED = no depth attachment)
        // MRT: the subpass's FULL color-ref vector (empty = a legacy
        // single-color pass) + positional attachment formats -- framebuffer-view and
        // pipeline-blend validation compare against the REAL subpass shape incl. UNUSED holes.
        std::vector<vkrpc::ColorRefDesc> color_refs;
        std::vector<VkFormat> attachment_formats;
        // Required-feature audit: the subpass viewMask (0 = non-multiview). A multiview
        // pass is built with a real VkRenderPassMultiviewCreateInfo; the mask also drives the
        // begin-time layer-sufficiency check for an imageless framebuffer. Mirrors
        // RenderPass.view_mask.
        int view_mask = 0;
    };
    struct RealFramebuffer {
        VkFramebuffer vk = VK_NULL_HANDLE;
        std::uint64_t device = 0;
        std::uint64_t render_pass = 0; // validated compatible at create
        // Render-pass COMPATIBILITY key, SNAPSHOTTED from the creating pass at create. Vulkan
        // lets an app destroy the creating pass and begin the framebuffer with any COMPATIBLE
        // pass (mpv/libplacebo does exactly this: a temporary creation pass, destroyed at once),
        // so begin-time compatibility must not resolve the creating pass by handle.
        VkFormat compat_color_format = VK_FORMAT_UNDEFINED;
        VkFormat compat_depth_format = VK_FORMAT_UNDEFINED;
        std::uint64_t image_view = 0;       // validated same-device + format-match at create
        std::uint64_t depth_image_view = 0; // the depth attachment (0 = none)
        VkExtent2D extent{}; // == the swapchain extent; begin clamps the render area to it
        // Host-safe extent. Normally identical to `extent`; for malformed imageless requests it
        // is bounded to the attachment metadata so no invalid Vulkan reaches the host driver.
        VkExtent2D host_extent{};
        // (GL/zink): an imageless framebuffer has no views at create. At begin we build a
        // regular VkFramebuffer from the views the begin supplies and CACHE it keyed by the view
        // set
        // -- never destroy-then-rebuild (a prior framebuffer may still be in flight on the GPU).
        // zink reuses the same FBO views across frames, so the cache is normally a single entry;
        // all cached framebuffers are destroyed together at destroy_framebuffer.
        bool imageless = false;
        // New clients forward VkFramebufferAttachmentsCreateInfo and get a real host imageless
        // framebuffer. False preserves compatibility with the former begin-time emulation.
        bool native_imageless = false;
        std::uint32_t layers = 1;
        // Required-feature audit: the compat render pass's subpass viewMask (0 =
        // non-multiview), SNAPSHOTTED at create like the compat formats. An imageless begin
        // validates each supplied view's image array-layer coverage against
        // multiview_required_layers(view_mask).
        int view_mask = 0;
        std::map<std::vector<std::uint64_t>, VkFramebuffer> imageless_cache;
        // MRT: the positional concrete view vector (empty = the legacy scalar shape), and the
        // declared attachment count an imageless begin must supply (0 = legacy/unknown).
        std::vector<std::uint64_t> attachment_views;
        int attachment_count = 0;
    };
    struct RealPipelineLayout {
        VkPipelineLayout vk = VK_NULL_HANDLE;
        std::uint64_t device = 0;
        std::vector<std::uint64_t> set_layouts; // ordered set layouts (empty = empty layout)
    };
    struct RealPipeline {
        VkPipeline vk = VK_NULL_HANDLE;
        std::uint64_t device = 0;
        std::uint64_t layout = 0;      // validated same-device at create
        std::uint64_t render_pass = 0; // validated same-device at create (draw-time compat key;
                                       // 0 for compute + dynamic rendering)
        int vertex_binding_count = 0;  // bindings a draw must have bound (0 = bufferless)
        bool has_depth = false;        // declared depth-stencil state (render pass must match)
        bool compute = false;          // the pipeline's KIND -- bind point must match
        // (dynamic rendering): a DR pipeline (render_pass == 0 + these formats) whose
        // draw-time compatibility key is the FORMATS, not a render pass. dynamic_rendering
        // distinguishes it from a compute pipeline (also render_pass == 0). The color-format vector
        // + depth/stencil formats + view mask must equal the active dynamic-rendering scope at
        // draw.
        bool dynamic_rendering = false;
        std::uint32_t view_mask = 0;
        std::vector<VkFormat> color_formats;
        VkFormat depth_format = VK_FORMAT_UNDEFINED;
        VkFormat stencil_format = VK_FORMAT_UNDEFINED;
    };
    // Buffer: a real VkBuffer, a device child, with the requirements the worker
    // reported at create (cross-checked at bind). bound_* are recorded for parallelism with the
    // mock.
    struct RealBuffer {
        VkBuffer vk = VK_NULL_HANDLE;
        std::uint64_t device = 0;
        VkDeviceSize size = 0;
        VkDeviceSize alignment = 0;
        std::uint32_t memory_type_bits = 0;
        std::uint64_t usage = 0; // descriptor writes test the UNIFORM_BUFFER bit
        std::uint64_t bound_memory = 0;
        VkDeviceSize bound_offset = 0;
    };
    // Descriptor surface objects. All UNIFORM_BUFFER. Set layout / pool / pipeline
    // layout are device children; a set is a pool child (freed with the pool).
    struct RealDescriptorSetLayoutBinding {
        int binding = 0;
        int descriptor_type = 0; // UNIFORM_BUFFER | COMBINED_IMAGE_SAMPLER (write/draw)
        int descriptor_count = 0;
        // descriptorIndexing: persisted -- the readiness/dangle rules key on these.
        long long binding_flags = 0;
    };
    struct RealDescriptorSetLayout {
        VkDescriptorSetLayout vk = VK_NULL_HANDLE;
        std::uint64_t device = 0;
        long long layout_flags = 0; // descriptorIndexing: UPDATE_AFTER_BIND_POOL persists
        std::vector<RealDescriptorSetLayoutBinding> bindings;
    };
    struct RealDescriptorPool {
        VkDescriptorPool vk = VK_NULL_HANDLE;
        std::uint64_t device = 0;
        int sets_remaining = 0; // maxSets accounting (per-type budget is the real pool's job now)
        // descriptorIndexing: a UAB-pool layout's sets must come from a UAB pool.
        bool update_after_bind = false;
        std::set<std::uint64_t> sets;
    };
    // One slot: initialized (a successful write landed) + the resource it currently references (the
    // dynamic CB -> set -> {buffer | sampler | image-view} link consulted on destroy). A UNIFORM
    // slot tracks `buffer`; a COMBINED_IMAGE_SAMPLER slot tracks {sampler, image_view}. The unused
    // fields stay 0.
    struct RealDescriptorSlot {
        bool initialized = false;
        std::uint64_t buffer = 0;
        std::uint64_t sampler = 0;
        std::uint64_t image_view = 0;
        std::uint64_t buffer_view = 0; // (GL/zink): texel-buffer descriptor referent
    };
    struct RealDescriptorSet {
        VkDescriptorSet vk = VK_NULL_HANDLE;
        std::uint64_t device = 0;
        std::uint64_t pool = 0;
        std::uint64_t layout =
            0; // the exact set layout it was allocated from (bind/draw exactness)
        std::map<int, std::vector<RealDescriptorSlot>> slots;
        // descriptorIndexing: the ALLOCATED variable count of the last binding (1 = none);
        // its slot vector is sized to this, so readiness/write bounds judge the allocated count.
        long long variable_count = -1;
    };
    // Sampler : a real VkSampler, a device child (blocks the device's destroy
    // until freed). The bounded subset's parameters are validated at create; the stateful role is
    // the combined-image-sampler destroy-consult (destroy_sampler walks set slots).
    struct RealSampler {
        VkSampler vk = VK_NULL_HANDLE;
        std::uint64_t device = 0;
    };
    // Query pool (GL 3.3 / occlusion / xfb queries): the real VkQueryPool + the type/count carried
    // so recorded reset/begin/end/write_timestamp can range-check the query index worker-side.
    struct RealQueryPool {
        VkQueryPool vk = VK_NULL_HANDLE;
        std::uint64_t device = 0;
        VkQueryType type = VK_QUERY_TYPE_OCCLUSION;
        std::uint32_t count = 0;
    };
    // Per-command-buffer metadata: its VkCommandBuffer, owning pool/device,
    // whether it has been recorded (queue_submit rejects a never-recorded buffer), and the
    // swapchain surfaces its recorded commands referenced (submit locks their union).
    struct RealCmdBuffer {
        VkCommandBuffer vk = VK_NULL_HANDLE;
        std::uint64_t device = 0;
        std::uint64_t pool = 0;
        bool recorded = false;
        std::set<std::uint64_t> referenced_surfaces;   // queue_submit locks their slots
        std::set<std::uint64_t> referenced_swapchains; // invalidated when one is destroyed
        // Draw objects whose host handles a recorded draw stream baked in (render pass /
        // framebuffer / image view / pipeline): destroying any invalidates this buffer so a later
        // submit is refused instead of replaying a stale baked handle.
        std::set<std::uint64_t> referenced_draw_objects;
    };
    struct RealPool {
        VkCommandPool vk = VK_NULL_HANDLE;
        std::uint64_t device = 0; // parent
        // our buffer handle -> VkCommandBuffer (needed to free the right buffers).
        std::map<std::uint64_t, VkCommandBuffer> buffers;
    };
    // Device-child leaf objects (no children of their own). One typed entry per
    // handle; only the field matching `kind` is set. A typed destroy rejects a
    // wrong-kind handle (e.g. free_memory on a fence), like the mock.
    enum class LeafKind { Fence, Semaphore, Memory, Event };
    struct RealLeaf {
        LeafKind kind = LeafKind::Fence;
        std::uint64_t device = 0; // parent
        VkFence fence = VK_NULL_HANDLE;
        VkSemaphore semaphore = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkEvent event = VK_NULL_HANDLE; // Event leaves only
        // Memory-only: the allocation size, the chosen type's property flags, and its
        // type index -- so write_memory_ranges bounds-checks + rejects non-coherent targets and
        // bind_buffer_memory enforces the buffer's memoryTypeBits / alignment / range.
        VkDeviceSize memory_size = 0;
        VkMemoryPropertyFlags memory_flags = 0;
        std::uint32_t memory_type_index = 0;
        // (bufferDeviceAddress): the admitted VkMemoryAllocateFlagsInfo.flags this
        // allocation was created with (0 = none). bind_buffer_memory enforces VUID 03339 against
        // it (a SHADER_DEVICE_ADDRESS buffer may only bind to DEVICE_ADDRESS memory), mock == real.
        std::uint64_t memory_allocate_flags = 0;
        // (GL/zink): a TIMELINE semaphore (VkSemaphoreTypeCreateInfo TIMELINE).
        // queue_submit chains a VkTimelineSemaphoreSubmitInfo only when a submit touches one (the
        // test devices that never enable the timeline feature must not get the struct).
        bool is_timeline = false;
    };

    // Shared by destroy_fence / destroy_semaphore / free_memory: looks up `handle`,
    // rejects an unknown or wrong-kind one, destroys the underlying Vk object, and
    // unlinks it from its device. Returns false + sets `err` on rejection.
    vkrpc::StatusResponse destroy_leaf(std::uint64_t handle, LeafKind kind);

    // Resolves a queue handle to its owning device handle + the VkQueue (re-derived via
    // vkGetDeviceQueue; the VkQueue is device-owned). Returns false if it is not a known
    // queue handle.
    bool resolve_queue(std::uint64_t queue, std::uint64_t& out_device, VkQueue& out_vk);
    // Mints a swapchain's image handles + VkImages once (idempotent) and registers each in
    // images_ (resolvable by record_command_buffer). Shared by get_swapchain_images and
    // present. Returns false + sets `err` on failure.
    bool ensure_swapchain_images(std::uint64_t swapchain_handle, RealSwapchain& sc,
                                 std::string& err);
    // Invalidates any recorded command buffer that baked in `handle` (a draw object being
    // destroyed), so a later queue_submit refuses it rather than replaying a freed host handle.
    void invalidate_cbs_referencing(std::uint64_t handle);
    // The CB -> set -> {buffer | sampler | image-view} destroy consult (for buffers,
    // extended to sampler/image-view referents). Marks any descriptor slot currently
    // referencing `resource` dangling (uninitialized) and invalidates any recorded command buffer
    // that baked the owning set. Called by destroy_buffer / destroy_image_view / destroy_sampler.
    void dangle_sets_referencing(std::uint64_t resource);
    // True if `set` (on `device`) is draw-ready: every slot
    // its layout requires is initialized and still references a live, same-device resource of the
    // binding's type (UNIFORM_BUFFER buffer, or COMBINED_IMAGE_SAMPLER sampler + view over a
    // SAMPLED image). On false sets `reason`.
    bool descriptor_set_draw_ready(std::uint64_t set, std::uint64_t device,
                                   std::string& reason) const;
    // Takes the per-surface slot locks for a set of surfaces, in ascending handle
    // order (std::set iterates sorted) to avoid a lock-ordering deadlock. Used by
    // record_command_buffer / queue_submit to serialize command work that touches
    // swapchain-backed images against window mutation.
    std::vector<std::unique_lock<std::mutex>>
    lock_surface_slots(const std::set<std::uint64_t>& surfaces);
    // Resolves a fence-handle array for reset_fences / wait_for_fences: non-empty, every
    // handle a fence on ONE live device. Fills the VkDevice + VkFences. Returns false +
    // sets `err` on an empty/cross-device/wrong-kind array (rejected before the driver).
    bool resolve_fence_array(const std::vector<std::uint64_t>& handles, VkDevice& out_device,
                             std::vector<VkFence>& out_fences, std::string& err);
    // Resolves a {physical_device, surface} WSI-query pair: the surface must exist and the
    // physical-device handle must be the one its instance enumerated. Fills the host
    // VkPhysicalDevice + VkSurfaceKHR + the surface's WindowSlot (so the caller can hold the
    // slot lock across the routed host query). Returns false + sets `err` on rejection.
    bool resolve_surface_query(std::uint64_t physical_device, std::uint64_t surface,
                               VkPhysicalDevice& out_phys, VkSurfaceKHR& out_surface,
                               windowing::WindowSlot*& out_slot, std::string& err);
    // device-loss probe: log a VK_ERROR_DEVICE_LOST detected on a submit (submit does not
    // carry a surface/HWND, but device loss is sticky -- the prior present is the likely trigger),
    // naming the render adapter and each referenced surface's monitor + driving adapter (the
    // cross-GPU signal). `which` is "vkQueueSubmit" / "vkQueueSubmit2".
    void log_device_lost_on_submit(const std::set<std::uint64_t>& referenced_surfaces,
                                   const char* which);
    // Lost-device containment: latch `device` lost after a host command
    // returned VK_ERROR_DEVICE_LOST (`where` names the command; logged on the transition only).
    void latch_device_lost(std::uint64_t device, const char* where);
    // Observe exactly one host VkResult. The result is returned unchanged; DEVICE_LOST latches the
    // owning relay device once. Call sites retain their existing non-loss status semantics.
    VkResult observe_device_result(std::uint64_t device, VkResult result, const char* where);
    // True iff the device is latched lost -- callers answer VK_ERROR_DEVICE_LOST locally instead
    // of re-entering the host driver (a post-loss host call can block forever).
    bool device_lost_latched(std::uint64_t device) const {
        const auto d = devices_.find(device);
        return d != devices_.end() && d->second.lost;
    }
    // Lazily start the single window thread, shared by the app data plane (surface HWNDs) and
    // the sidecar plane (placeholder HWNDs). Cross-plane safe: double-checked under backend_mutex_,
    // with the WindowThread ctor (which blocks on pump-ready) run OFF-lock. Returns the live
    // thread, or nullptr if it could not be started.
    windowing::WindowThread* ensure_window_thread();
    // Execute a CreatePlaceholder effect: create a hidden placeholder window on the window thread
    // (off-lock), then re-check under backend_mutex_ that the registry still wants exactly this
    // placeholder (a racing create_surface may have promoted/removed it) -- commit the id->HWND
    // mapping or discard the just-created window. (the mutex dance).
    void execute_create_placeholder(const sidecar::RegistryEffect& eff);
    // place the host HWND currently representing `xid` at `geometry` (X-root client
    // coords), generation/seq-sequenced. Resolves the HWND under backend_mutex_ then marshals the
    // move onto the window thread OFF-lock (the rule). Shared by update_toplevel (live move) and
    // the representation-establishing transitions (initial placement / promote / re-register). A
    // no-op when the xid has no host HWND (a None representation).
    void apply_geometry_move(std::uint64_t xid, const sidecar::ToplevelGeometry& geometry,
                             std::uint64_t host_apply_seq, bool apply_size,
                             sidecar::ZOrder z_order);
    // reveal predicate: is `xid`'s representation paint-eligible (safe to SW_SHOWNA)?
    // True for a placeholder that has committed its first chrome paint (registry `shown`) or a
    // surface that has had its first successful present (RealSurface::shown -- which lives OUTSIDE
    // the registry, so this consults surfaces_). False for a None representation. MUST be called
    // with backend_mutex_ HELD (reads the registry + surfaces_).
    bool paint_eligible_locked(std::uint64_t xid) const;
    // after a register_toplevel that applied, re-stamp the surface
    // HWND's input slot with the xid's CURRENT representation epoch, so real Win32 input captured
    // after a surface-backed remap is stamped the new epoch (and old-epoch input is dropped). Reads
    // surface_input_hwnds_ + epoch under backend_mutex_; marshals the slot update onto the window
    // thread OFF-lock. A no-op when the xid has no surface HWND (placeholder-only / pending).
    void reconcile_surface_input_epoch(std::uint64_t xid);
    // the HWND currently representing `xid` -- its surface window
    // (surface_input_hwnds_) if any, else its placeholder window (placeholder_hwnds_ via the
    // registry's placeholder id), else nullptr. Caller holds backend_mutex_.
    HWND hwnd_for_xid(std::uint64_t xid) const;

    std::string gpu_name_;      // selected device name (display; exact key when required+no LUID)
    std::string gpu_luid_;      // preferred stable match key (may be empty)
    bool gpu_required_ = false; // faithful selection: fail closed on a miss (no silent fallback)
    display::DisplayLayout display_layout_;
    bool has_display_layout_ = false;
    bool present_fence_retire_requested_ = false;
    std::uint64_t next_handle_ = 1; // 0 is reserved for null
    std::map<std::uint64_t, RealInstance> instances_;
    std::map<std::uint64_t, RealDevice> devices_;
    std::map<std::uint64_t, RealPool> pools_;  // a buffer's owning pool is the one holding it
    std::map<std::uint64_t, RealLeaf> leaves_; // fences/semaphores/memory by handle
    std::map<std::uint64_t, RealSurface> surfaces_;
    // developer proof (VKRELAY2_DEBUG_DUMP_READBACK=<path>): copy_image_to_buffer dest
    // buffers to dump as raw pixels after the next fence wait completes -- captures EXACTLY what
    // the GPU rendered through the relay. Populated at record time, drained + cleared in
    // wait_for_fences. Data-plane
    // only (single-threaded), like surfaces_; a no-op unless the env var is set.
    struct PendingReadbackDump {
        std::uint64_t device = 0;
        std::uint64_t buffer = 0;
        int width = 0;
        int height = 0;
    };
    std::vector<PendingReadbackDump> pending_readback_dumps_;
    void debug_dump_readbacks();
    // developer proof (VKRELAY2_DEBUG_DUMP_PRESENT=<path>, gated, no-op otherwise): capture
    // the swapchain image that is ABOUT TO BE PRESENTED to raw BGRA8 (+dims), so a windowed GL app
    // (OpenSCAD) can be screenshotted -- the present is flip-model, so a desktop CopyFromScreen
    // returns black; the only trustworthy windowed capture is the worker's own source layer
    // rather than a desktop copy.
    //
    // Per WSI rules a PRESENTED image is no longer app-owned until it is re-acquired, so the copy
    // is done PRE-present, not post-present. begin_present_capture (on the window
    // thread, inside the present invoke) records + submits "transition PRESENT_SRC->TRANSFER_SRC,
    // copy image->staging, transition back", WAITING the app's present-wait semaphores and
    // SIGNALING its own binary semaphore (capture.done) -- so the present then chains on
    // capture.done instead of re-waiting the app's already-consumed binary semaphores. The
    // swapchain must carry transfer_src (added at create only when this env is set + the surface
    // supports it); otherwise capture is skipped. end_present_capture (after the present, same
    // invoke) waits the queue idle, maps + writes the artifact, and frees the capture resources.
    // Capture runs entirely BEFORE present, so it can never read a presented (non-owned) image; a
    // begin failure leaves the present on the app's own semaphores, so a capture fault can never
    // affect rendering.
    struct PresentCapture {
        bool active = false;
        std::uint64_t device = 0;
        VkDevice vk = VK_NULL_HANDLE;
        VkBuffer buf = VK_NULL_HANDLE;
        VkDeviceMemory mem = VK_NULL_HANDLE;
        VkCommandPool pool = VK_NULL_HANDLE;
        VkSemaphore done = VK_NULL_HANDLE; // capture-finished -> present waits on this
        VkDeviceSize bytes = 0;
        VkExtent2D extent{};
        VkFormat format = VK_FORMAT_UNDEFINED; // recorded into the .meta sidecar
        VkImageUsageFlags usage = 0; // the swapchain's effective usage, for the .meta sidecar
    };
    bool begin_present_capture(std::uint64_t device, VkQueue queue, VkImage image,
                               VkExtent2D extent, VkFormat format, VkImageUsageFlags usage,
                               const std::vector<VkSemaphore>& app_wait_sems, PresentCapture& out);
    VkResult end_present_capture(VkQueue queue, PresentCapture& cap);
    int present_capture_seq_ = 0; // bounds the captured-frame count (a continuous app would flood)
    sidecar::WindowRegistry registry_; // worker-owned XID registry shared by both planes
    bool sidecar_ready_ = false;       // the sidecar emitted its readiness barrier
    // device-loss probe: the RENDER adapter identity (Vulkan deviceName + LUID of the selected
    // physical device), cached at create_device. The present/submit device-loss diagnostics compare
    // it against the adapter DRIVING the window's current monitor (EnumDisplayDevices) so a monitor
    // transition + the loss are logged with both adapters. A mismatch is only a FACT (cross-adapter
    // present), NOT a verdict: on this laptop the internal panel is AMD-iGPU-owned while rendering
    // is on the NVIDIA dGPU, and that cross-adapter present WORKS -- so the diagnostic reports the
    // adapters for a human to compare across monitors, it does not assert the cause. Empty until
    // the first device is created.
    std::string render_adapter_name_;
    std::string render_adapter_luid_;
    // guards the cross-plane state -- registry_ + placeholder_hwnds_ + the lazy
    // window_thread_ pointer -- which the app data plane (create/destroy_surface) and the sidecar
    // plane (register/update/unregister_toplevel) both touch. NEVER held across a WindowThread
    // invoke/create/destroy (those run off-lock); the data plane's own handle tables stay
    // single-threaded and unguarded (the sidecar never reads them).
    std::mutex backend_mutex_;
    // set true by abort_session (the liveness observer thread) on app peer-close. The
    // acquire poll (and its internal-fence wait) check it between host quanta on the pump thread,
    // so an in-flight infinite acquire releases within one quantum when the app died -- letting the
    // dispatcher return, the control channel close, and teardown proceed (supervisor kill
    // backstops).
    std::atomic<bool> session_aborting_{false};
    // Placeholder aux HWNDs the sidecar plane created for non-surface toplevels (opaque
    // placeholder_id from the registry -> the hidden Win32 window). shows + paints them.
    std::map<std::uint64_t, HWND> placeholder_hwnds_;
    // the surface HWND backing each guest XID, MIRRORED here under
    // backend_mutex_ so the sidecar plane (register_toplevel) can rebind the surface slot's input
    // epoch after a remap without reaching into the data-plane-only surfaces_ table (a cross-thread
    // read). Maintained by create_surface/destroy_surface (data plane) under the same mutex.
    std::map<std::uint64_t, HWND> surface_input_hwnds_; // guest xid -> its current surface HWND
    // xid -> the SURFACE HANDLE that deferred its swapchain
    // extent (use_current_extent -- zink/kopper). SURFACE-SPECIFIC (not just an xid bit): same-xid
    // surface replacement is real (Tk/Togl, Qt/OpenSCAD, glmark2 churn surfaces), so a deferred bit
    // from an OLD surface must not leak onto a newer current surface for the same xid. Mirrored
    // here under backend_mutex_ (recorded by create_swapchain, erased by destroy_surface, both data
    // plane) so the SIDECAR plane (register_toplevel) can adopt the registered geometry SIZE for a
    // surface-first/register-late deferred surface -- but ONLY when the recorded surface is still
    // the CURRENT bound surface (registry_.surface_for_xid). The narrow "register-late adoption"
    // rule that converges a deferred zink window off its default WITHOUT making the registry's
    // plain geometry cell a second live-resize authority.
    std::map<std::uint64_t, std::uint64_t> deferred_extent_surface_by_xid_;
    // the per-session input ring. The WndProc (window thread) enqueues into it via the
    // slot's input target; poll_input drains it. Its mutex is INSIDE InputQueue (a small dedicated
    // lock, never backend_mutex_). Declared BEFORE window_thread_ so it OUTLIVES the window thread
    // (members destroy in reverse order -- the WndProc must never enqueue into a destroyed ring).
    sidecar::InputQueue input_queue_;
    std::map<std::uint64_t, RealSwapchain> swapchains_;
    std::map<std::uint64_t, RealImage> images_;              // swapchain image handle -> metadata
    std::map<std::uint64_t, RealCmdBuffer> command_buffers_; // command buffer handle -> metadata
    // Draw-surface object tables.
    std::map<std::uint64_t, RealImageView> image_views_;
    std::map<std::uint64_t, RealBufferView> buffer_views_; // (GL/zink): texel buffer views
    std::map<std::uint64_t, RealShaderModule> shader_modules_;
    std::map<std::uint64_t, RealRenderPass> render_passes_;
    std::map<std::uint64_t, RealFramebuffer> framebuffers_;
    std::map<std::uint64_t, RealPipelineLayout> pipeline_layouts_;
    std::map<std::uint64_t, RealPipeline> pipelines_;
    std::map<std::uint64_t, RealBuffer> buffers_; // buffer table
    // Descriptor surface tables.
    std::map<std::uint64_t, RealDescriptorSetLayout> descriptor_set_layouts_;
    std::map<std::uint64_t, RealDescriptorPool> descriptor_pools_;
    std::map<std::uint64_t, RealDescriptorSet> descriptor_sets_;
    std::map<std::uint64_t, RealSampler> samplers_;
    std::map<std::uint64_t, RealQueryPool> query_pools_; // query pools (GL 3.3 / queries)
    // The single window thread that owns every per-app HWND. Created lazily on
    // the first surface; tears down (after the destructor body destroys windows).
    std::unique_ptr<windowing::WindowThread> window_thread_;
};

} // namespace vkr::worker

#endif // VKRELAY2_WINDOWS_WORKER_REAL_VULKAN_BACKEND_HPP
