// Vulkan domain over the RPC envelope.
//
// Beyond capability negotiation, this adds the instance/device
// lifecycle vkcube needs: create_instance -> enumerate_physical_devices ->
// create_device, plus destroy, with a per-session handle/object table and
// call-ordering enforcement (a device can only be created on the enumerated --
// i.e. selected -- physical device; an instance with live devices cannot be
// destroyed). The host-Vulkan side is behind VulkanBackend; the mock implements
// the state machine so the round-trip is testable headless on both platforms. A
// real loader-backed backend lives only in the worker.
#ifndef VKRELAY2_COMMON_VKRPC_VULKAN_SESSION_HPP
#define VKRELAY2_COMMON_VKRPC_VULKAN_SESSION_HPP

#include "common/display/display_layout.hpp"
#include "common/protocol/gpu.hpp"
#include "common/sidecar/input_queue.hpp"
#include "common/sidecar/sidecar_session.hpp"
#include "common/sidecar/window_registry.hpp"
#include "common/util/json.hpp"
#include "common/vkrpc/rpc.hpp"

#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace vkr::vkrpc {

// The worker's maximum supported Vulkan API (plan: "Vulkan 1.3"). Negotiation
// caps the requested level to this.
constexpr int kSupportedApiMajor = 1;
constexpr int kSupportedApiMinor = 3;

struct CapabilitiesRequest {
    int requested_api_major = 1;
    int requested_api_minor = 3;

    json::Value to_body() const;
    static CapabilitiesRequest from_body(const json::Value& body);
};

// Identity of the physical device the worker selected for this session.
struct DeviceCaps {
    std::string device_name;
    std::uint32_t vendor_id = 0;
    std::uint32_t device_id = 0;
    std::string device_type; // GpuDeviceType, stringized
    // Query pools (GL 3.3): the graphics queue family's real timestampValidBits. zink gates
    // ARB_timer_query (and thus GL 3.3) on this being > 0. Additive: 0 = unknown (an old worker /
    // the mock) -> the ICD keeps its synthesized 0, the prior behavior. Reported ONLY because the
    // relay now implements the query machinery (never advertise-then-hope).
    std::uint32_t timestamp_valid_bits = 0;
    // MRT: the host's real VkPhysicalDeviceLimits::maxColorAttachments. Additive: 0 = unknown (an
    // old worker) -> the ICD keeps its OLD single-color render-pass gate, so a version-skewed
    // ICD/worker pair degrades LOUDLY (render pass rejected at the ICD), never silently.
    std::uint32_t max_color_attachments = 0;
    // the SERVE LOOP can reply to read_memory_ranges with a raw binary body
    // ([u32 json_len][{ok,reason}][raw payload]) instead of hex-in-JSON. A protocol capability
    // (backend-agnostic; the serve loop stamps it on enumerate). Additive: false = old worker ->
    // the client keeps requesting the legacy JSON+hex response.
    bool raw_readback = false;
    // the serve loop accepts RpcOp::RecordCommandBufferRaw (binary-framed
    // command streams). Same additive protocol-capability pattern: false = old worker -> the
    // client keeps the JSON RecordCommandBuffer path.
    bool raw_record = false;
    // Compute: the selected queue family's REAL VkQueueFlags (the backend stamps it
    // at enumerate; the mock advertises GRAPHICS|COMPUTE|TRANSFER). Additive: 0 = unknown (an
    // old worker) -> the ICD keeps its synthesized GRAPHICS|TRANSFER and REFUSES compute
    // pipeline creation, so a version-skewed pair fails closed at create, never at dispatch.
    std::uint64_t queue_flags = 0;
    // Vulkan 1.3 support + required-feature audit: 1 iff the HOST can back a 1.3 device
    // AND the RELAY serves the complete cumulative required matrix. = host apiVersion >= 1.3 AND
    // every 1.3-REQUIRED feature reports TRUE on the host (the f13 family + the two required
    // Vulkan-1.3 memory-model bits + the promoted-and-required f10/f11/f12 members) AND the
    // relay-served gate (kRelayServes*: multiview + device-level hostQueryReset) is fully closed.
    // The ICD bumps the reported device apiVersion to 1.3 (native lane only) iff this is set.
    // Additive: 0 = unknown (an old worker / a host that cannot / a required feature not yet
    // served)
    // -> the device stays 1.2, the Vulkan-1.2 behavior.
    std::uint32_t vk13_ready = 0;
    // geometry-stream: 1 iff this worker VALIDATES + REPLAYS the
    // VkPipelineRasterizationStateStreamCreateInfoEXT pipeline pNext (the geometry-stream
    // admission path). Additive: 0 = unknown (an old worker) -> the ICD keeps its PRIOR
    // fail-closed rejection of the stream pNext (graphics_pipeline_ok's allow flag stays off), so
    // a version-skewed pair degrades to the exact prior contract (advertise-then-reject with a
    // named reason) instead of an old worker silently DROPPING the stream state. The
    // geometryStreams FEATURE stays advertised either way -- masking it breaks Mesa 23.2 zink
    // shaders (see the features2 pass-through note in icd.cpp).
    std::uint32_t rasterization_stream_state = 0;

    json::Value to_body() const;
    static DeviceCaps from_body(const json::Value& body);
};

// Result of negotiation: ok with the negotiated level + selected device, or
// !ok with a reason (e.g. the requested level is below the floor, or no usable
// device). The negotiated level is capped by the request and backend support
// (mock: worker ceiling; real: host instance/device support + worker ceiling).
struct CapabilitiesResponse {
    bool ok = false;
    std::string reason;
    int negotiated_api_major = 0;
    int negotiated_api_minor = 0;
    DeviceCaps device;

    json::Value to_body() const;
    static CapabilitiesResponse from_body(const json::Value& body);
};

// --- Instance / device lifecycle -------------------------
//
// Handles are opaque, worker-assigned u64 ids (0 = null). Semantic failures
// (unknown handle, selection violation, destroy ordering) come back as ok=false
// + reason at RpcStatus::Ok, the way capability negotiation does; RpcStatus is
// reserved for envelope/transport-level problems.

struct CreateInstanceRequest {
    std::string application_name; // informational (mock ignores it)

    json::Value to_body() const;
    static CreateInstanceRequest from_body(const json::Value& body);
};

struct CreateInstanceResponse {
    bool ok = false;
    std::string reason;
    std::uint64_t instance = 0;

    json::Value to_body() const;
    static CreateInstanceResponse from_body(const json::Value& body);
};

struct EnumeratePhysicalDevicesRequest {
    std::uint64_t instance = 0;

    json::Value to_body() const;
    static EnumeratePhysicalDevicesRequest from_body(const json::Value& body);
};

struct PhysicalDeviceEntry {
    std::uint64_t handle = 0;
    DeviceCaps caps;
    // The HOST's real device-extension names, so the ICD advertises its allowlist
    // INTERSECTED with what the host can actually enable (never advertise-then-hope). Additive:
    // absent/empty (an old worker, or the mock) -> the ICD falls back to the policy-only
    // allowlist, exactly the prior behavior.
    std::vector<std::string> device_extensions;

    json::Value to_body() const;
    static PhysicalDeviceEntry from_body(const json::Value& body);
};

struct EnumeratePhysicalDevicesResponse {
    bool ok = false;
    std::string reason;
    std::vector<PhysicalDeviceEntry> devices;

    json::Value to_body() const;
    static EnumeratePhysicalDevicesResponse from_body(const json::Value& body);
};

// One pNext capability struct, keyed by sType (used by create_device's enabled-feature chain AND
// the generic Features2/Properties2 query forwarder). In a REQUEST the sender sets s_type + size
// (the struct's sizeof, from the ICD's sType->size table) and, for the create_device
// enabled-feature chain, the blob (the app's requested struct bytes to enable); in a query RESPONSE
// the worker sets s_type + blob (the real host struct's bytes). blob length == size when filled.
struct CapabilityChainEntry {
    std::uint32_t s_type = 0;
    std::uint32_t size = 0;
    std::string blob; // raw struct bytes

    json::Value to_body() const;
    static CapabilityChainEntry from_body(const json::Value& body);
};

struct CreateDeviceRequest {
    std::uint64_t instance = 0;
    std::uint64_t physical_device = 0;
    // GL/zink: the app's enabled device extensions + requested features, so the worker
    // enables them on the REAL device instead of the hardcoded swapchain-only set. The ICD has
    // already filtered the extensions against its host-intersected allowlist (so an unwired/
    // unsupported extension never reaches here). enabled_feature_bits is
    // pack_physical_device_features of the app's requested VkPhysicalDeviceFeatures (from
    // pEnabledFeatures, when no Features2 pNext is used). Additive: a legacy request decodes to an
    // empty list + 0 bits -> the worker's old swapchain-only behavior, so the vkcube/canary path is
    // unchanged.
    std::vector<std::string> enabled_extensions;
    std::uint64_t enabled_feature_bits = 0;
    // Native lane: did the app enable the dynamicRendering
    // FEATURE (VkPhysicalDeviceDynamicRenderingFeatures.dynamicRendering or the Vulkan13Features
    // spelling), not merely the extension? The ICD parses the enabled-feature chain (it has the
    // Vulkan headers) and sends this bit; both backends gate begin_rendering + DR pipeline creation
    // on extension AND feature, so enabling the ext without the feature (which Vulkan forbids using
    // DR through) cannot slip a DR command/pipeline past the oracle. 0 for every non-DR device.
    int dynamic_rendering_feature_enabled = 0;
    // Native lane: the parallel bit for the synchronization2 FEATURE. Both
    // backends gate every sync2 command (barrier2 / timestamp2 / submit2 / the event2 trio) on
    // extension AND this feature. 0 for every non-sync2 device.
    int synchronization2_feature_enabled = 0;
    // Required-feature audit: did the app enable the
    // hostQueryReset FEATURE (core 1.2, no extension -- the f12 rollup or the standalone
    // VkPhysicalDeviceHostQueryResetFeatures)? The worker re-derives it from the forwarded chain
    // and rejects a scalar/chain mismatch; both backends fail-closed device-level vkResetQueryPool
    // unless this is set, so a skewed client cannot drive an invalid host reset. 0 = not enabled.
    int host_query_reset_feature_enabled = 0;
    // Required-feature audit (multiview, REQUIRED since 1.1): did the app enable the
    // multiview FEATURE (the standalone VkPhysicalDeviceMultiviewFeatures or the Vulkan11Features
    // ROLLUP spelling)? There is NO extension gate on either lane -- multiview is core 1.1,
    // reported host-TRUE. The worker re-derives it from the forwarded chain and rejects a
    // scalar/chain mismatch; both backends fail-closed a viewMask render pass (and a
    // viewMask dynamic-rendering scope) unless this is set, so a skewed client cannot drive a
    // multiview host render pass the device never enabled the feature for. 0 = not enabled.
    int multiview_feature_enabled = 0;
    // Vulkan 1.2 bufferDeviceAddress support: did the app enable the bufferDeviceAddress
    // FEATURE (the standalone VkPhysicalDeviceBufferDeviceAddressFeatures spelling or the
    // Vulkan12Features rollup)? There is NO extension for this gate -- it is core 1.2, unmasked
    // only on the native lane -- so the feature bit alone gates get_buffer_device_address and the
    // DEVICE_ADDRESS memory-allocate flag on both backends (mock == real). The real worker
    // additionally verifies the host actually supports it at create_device (fail closed).
    int buffer_device_address_feature_enabled = 0;
    // VK_EXT_vertex_attribute_divisor: did the app enable the two feature bits at vkCreateDevice?
    // The ext is advertised for zink's GL 3.x path; these gate the DIVISOR VALUES a pipeline may
    // carry. The worker re-derives both from the forwarded enabled-feature chain
    // (VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT) and rejects a scalar/chain mismatch (same
    // normalization as multiview/BDA). divisor != 1 needs ...RateDivisor; divisor == 0 additionally
    // needs ...ZeroDivisor. 0 = not enabled.
    int vertex_attr_divisor_feature_enabled = 0;
    int vertex_attr_zero_divisor_feature_enabled = 0;
    // VK_EXT_transform_feedback geometryStreams: did the app enable the feature at vkCreateDevice?
    // Gates the rasterization-stream pNext a graphics pipeline may carry (the shared
    // rasterization_stream_ok). The worker re-derives it from the forwarded enabled-feature chain
    // (VkPhysicalDeviceTransformFeedbackFeaturesEXT) and rejects a scalar/chain mismatch (the
    // divisor/multiview/BDA normalization pattern). THREE-STATE on the wire: a
    // prior ICD already forwards a geometryStreams=TRUE chain but has no scalar, so an ABSENT
    // key decodes kGeometryStreamsScalarOmitted (-1) and the worker derives the enabled state from
    // the chain alone instead of rejecting the old payload as a mismatch. A new ICD always sends
    // an explicit 0/1, which is agreement-checked. Omission is a PRESENCE fact, not a
    // value -- a transmitted non-0/1 (e.g. a forged -1 dodging the agreement check) decodes
    // kGeometryStreamsScalarInvalid (-2) and create_device rejects it by name (mock == real).
    int geometry_streams_feature_enabled = 0;
    // Descriptor indexing: the enabled kDIFeature* bits (the ICD sends only the
    // served kDIFeatureServedBits subset, native lane only). Gates the per-binding flag
    // admission, UPDATE_AFTER_BIND layouts/pools, and variable-count allocation on both backends;
    // the real worker normalizes these against the forwarded feature chain (the BDA pattern) and
    // verifies host support before the host device is created.
    std::uint64_t descriptor_indexing_feature_bits = 0;
    // Vulkan 1.3 support: the enabled kVk13Feature* bits (the ICD sends only the served
    // kVk13FeatureServedBits subset, native lane + vk13_ready only). Same discipline: normalized
    // against the forwarded chain (the 1.3/1.2 rollups + every standalone promoted-feature
    // struct), host-verified per bit, clamped to the served set at all three sites.
    std::uint64_t vk13_feature_bits = 0;
    // Vulkan 1.3 support: 1 iff the ICD reported this DEVICE as apiVersion 1.3 (native lane +
    // DeviceCaps.vk13_ready). Turns on the core-1.3 command surface -- the EDS core names,
    // copy_commands2, dynamic rendering + synchronization2 WITHOUT their KHR extensions -- and
    // the worker RE-VERIFIES the host can back it (host 1.3 + required features) before the host
    // device exists, so a lying client cannot uncap anything.
    int vk13_device_enabled = 0;
    // (GL/zink): the app's enabled-FEATURE pNext chain (VkPhysicalDeviceFeatures2 +
    // Vulkan11/12/13 + ext feature structs) captured verbatim as {sType, blob}, so the worker
    // rebuilds it into VkDeviceCreateInfo.pNext and the real device enables EXACTLY what the app
    // asked for (zink enables 1.2 bufferDeviceAddress etc.). When non-empty the worker uses the
    // chain (and not pEnabledFeatures). Empty for the vkcube/canary path.
    std::vector<CapabilityChainEntry> enabled_feature_chain;

    json::Value to_body() const;
    static CreateDeviceRequest from_body(const json::Value& body);
};

struct CreateDeviceResponse {
    bool ok = false;
    std::string reason;
    std::uint64_t device = 0;
    // The queue family the device created queues from, and how many, so the app
    // knows which (family, index) get_device_queue may be called with: only a
    // family/index requested at vkCreateDevice is valid -- vkGetDeviceQueue on any
    // other is undefined behavior. The spine creates one queue from one graphics
    // family, so the valid set is {(queue_family_index, 0 .. queue_count-1)}.
    int queue_family_index = 0;
    int queue_count = 0;

    json::Value to_body() const;
    static CreateDeviceResponse from_body(const json::Value& body);
};

// destroy_device / destroy_instance share this {handle} request shape.
struct HandleRequest {
    std::uint64_t handle = 0;

    json::Value to_body() const;
    static HandleRequest from_body(const json::Value& body);
};

struct StatusResponse {
    bool ok = false;
    std::string reason;

    json::Value to_body() const;
    static StatusResponse from_body(const json::Value& body);
};

// --- Device-child command objects ------------------------
//
// Queues are retrieved (and cached per family/index), not created; command pools
// are device children; command buffers are pool children. Vectors of handles
// (e.g. allocated command buffers) ride as JSON arrays of decimal strings.

struct GetDeviceQueueRequest {
    std::uint64_t device = 0;
    int queue_family_index = 0;
    int queue_index = 0;

    json::Value to_body() const;
    static GetDeviceQueueRequest from_body(const json::Value& body);
};

struct GetDeviceQueueResponse {
    bool ok = false;
    std::string reason;
    std::uint64_t queue = 0;

    json::Value to_body() const;
    static GetDeviceQueueResponse from_body(const json::Value& body);
};

struct CreateCommandPoolRequest {
    std::uint64_t device = 0;
    int queue_family_index = 0;

    json::Value to_body() const;
    static CreateCommandPoolRequest from_body(const json::Value& body);
};

struct CreateCommandPoolResponse {
    bool ok = false;
    std::string reason;
    std::uint64_t command_pool = 0;

    json::Value to_body() const;
    static CreateCommandPoolResponse from_body(const json::Value& body);
};

struct AllocateCommandBuffersRequest {
    std::uint64_t command_pool = 0;
    long long count = 0; // wide: range-checked against the cap before narrowing

    json::Value to_body() const;
    static AllocateCommandBuffersRequest from_body(const json::Value& body);
};

struct AllocateCommandBuffersResponse {
    bool ok = false;
    std::string reason;
    std::vector<std::uint64_t> command_buffers;

    json::Value to_body() const;
    static AllocateCommandBuffersResponse from_body(const json::Value& body);
};

struct FreeCommandBuffersRequest {
    std::uint64_t command_pool = 0;
    std::vector<std::uint64_t> command_buffers;

    json::Value to_body() const;
    static FreeCommandBuffersRequest from_body(const json::Value& body);
};

// --- Device-child sync + memory objects ------------------
//
// Fences, semaphores, and device memory are device-child leaf objects (no
// children of their own). Like command pools, they block their device's destroy
// until freed. create_fence / create_semaphore take just {device}; allocate_memory
// also carries the allocation size + memory type index. Each create returns a
// handle in a kind-named field; destroy_* / free_memory take {handle}.

struct CreateFenceRequest {
    std::uint64_t device = 0;
    // VK_FENCE_CREATE_SIGNALED_BIT: a fence created already-signaled, so the first wait_for_fences
    // succeeds without a prior submit. Real frame loops (e.g. vkcube) create their per-frame fences
    // signaled and wait on them before reusing the slot; dropping the flag deadlocks the first
    // wait.
    bool signaled = false;

    json::Value to_body() const;
    static CreateFenceRequest from_body(const json::Value& body);
};

struct CreateFenceResponse {
    bool ok = false;
    std::string reason;
    std::uint64_t fence = 0;

    json::Value to_body() const;
    static CreateFenceResponse from_body(const json::Value& body);
};

struct CreateSemaphoreRequest {
    std::uint64_t device = 0;
    // (GL/zink): semaphore_type 0 = BINARY (default; legacy decodes here), 1 = TIMELINE
    // (VkSemaphoreTypeCreateInfo TIMELINE in the app's pNext). initial_value seeds a timeline.
    int semaphore_type = 0;
    std::uint64_t initial_value = 0;

    json::Value to_body() const;
    static CreateSemaphoreRequest from_body(const json::Value& body);
};

struct CreateSemaphoreResponse {
    bool ok = false;
    std::string reason;
    std::uint64_t semaphore = 0;

    json::Value to_body() const;
    static CreateSemaphoreResponse from_body(const json::Value& body);
};

// VkEvent object model -- a device-child leaf beside fence/semaphore, the object
// the sync2 event commands stand on. create_event takes just {device} (vkEventCreateInfo
// has no app-controlled fields in core 1.0 -- VK_EVENT_CREATE_DEVICE_ONLY_BIT belongs to the
// unsupported Vulkan 1.3/sync2 shape); destroy_event and the host ops
// (get_event_status / set_event / reset_event)
// reuse HandleRequest {handle = event}, the op code carrying the operation.
struct CreateEventRequest {
    std::uint64_t device = 0;

    json::Value to_body() const;
    static CreateEventRequest from_body(const json::Value& body);
};

struct CreateEventResponse {
    bool ok = false;
    std::string reason;
    std::uint64_t event = 0;

    json::Value to_body() const;
    static CreateEventResponse from_body(const json::Value& body);
};

// vkGetEventStatus carries the honest VkResult: VK_EVENT_SET (3) / VK_EVENT_RESET (4) are the
// NORMAL non-VK_SUCCESS returns (the WaitForFences idiom -- ok=true, the app reads `result`).
// vkSetEvent / vkResetEvent return only VK_SUCCESS and ride StatusResponse (the ICD synthesizes the
// result).
struct GetEventStatusResponse {
    bool ok = false;
    std::string reason;
    int result = 0; // honest VkResult: VK_EVENT_SET (3) or VK_EVENT_RESET (4)

    json::Value to_body() const;
    static GetEventStatusResponse from_body(const json::Value& body);
};

struct AllocateMemoryRequest {
    std::uint64_t device = 0;
    std::uint64_t allocation_size = 0; // VkDeviceSize; must be > 0
    long long memory_type_index = 0;   // wide: range-checked to [0, UINT32_MAX]
    // (bufferDeviceAddress): VkMemoryAllocateFlagsInfo.flags, serialized rather than
    // dropped (the reference relay's "advertise the feature but don't serialize its pNext" trap).
    // Only kMemoryAllocateDeviceAddressBit is admitted, and only when the device enabled the
    // bufferDeviceAddress feature -- without the flag the host rejects binding a
    // SHADER_DEVICE_ADDRESS buffer (VUID-vkBindBufferMemory-bufferDeviceAddress-03339). 0 =
    // no flags-info chained (the legacy shape; additive decode).
    std::uint64_t allocate_flags = 0;

    json::Value to_body() const;
    static AllocateMemoryRequest from_body(const json::Value& body);
};

struct AllocateMemoryResponse {
    bool ok = false;
    std::string reason;
    std::uint64_t memory = 0;

    json::Value to_body() const;
    static AllocateMemoryResponse from_body(const json::Value& body);
};

// --- Presentation spine: surface + swapchain -------------
//
// A surface is an instance child (a window's drawable, created from the instance);
// a swapchain is a device child that targets a surface. This interface includes the
// create/destroy spine; image acquire/present and Win32 WSI execution are modeled separately. The
// mock models the handle lifecycle while the real backend owns the host objects. destroy_surface /
// destroy_swapchain use the shared HandleRequest.

struct CreateSurfaceRequest {
    std::uint64_t instance = 0;
    // Born-correlated guest-window topology. The ICD reads these from the WSI
    // create-info (e.g. VkXcbSurfaceCreateInfoKHR::window) so the surface is correlated to its
    // guest toplevel at birth -- no separate registration op to race. `platform` is "xcb" today
    // (wayland later), empty for a legacy/untopologied surface; `xid` is the guest window id (0 =
    // none); `role_hint` is ADVISORY only (the sidecar owns role classification), default
    // "UnknownPending".
    std::string platform;
    std::uint64_t xid = 0;
    std::string role_hint = "UnknownPending";

    json::Value to_body() const;
    static CreateSurfaceRequest from_body(const json::Value& body);
};

struct CreateSurfaceResponse {
    bool ok = false;
    std::string reason;
    std::uint64_t surface = 0;

    json::Value to_body() const;
    static CreateSurfaceResponse from_body(const json::Value& body);
};

struct CreateSwapchainRequest {
    std::uint64_t device = 0;
    std::uint64_t surface = 0;
    // Descriptive parameters (Vulkan enums/extents ride as ints). The mock records
    // the device/surface link; the real backend will validate these against the
    // surface's capabilities when WSI lands.
    int image_format = 0;
    int color_space = 0;
    int width = 0;
    int height = 0;
    int min_image_count = 0;
    int present_mode = 0;
    // App-declared VkImageUsageFlags for the swapchain images. The app, not
    // the worker, decides usage: the real backend validates every requested bit against
    // the surface's supportedUsageFlags (the mock validates only that it is non-zero,
    // having no caps). A clear_color_image command requires TRANSFER_DST here.
    int image_usage = 0;
    // (GL/zink): the app DEFERRED the extent -- it passed the 0xFFFFFFFF "currentExtent"
    // sentinel straight back instead of choosing a concrete extent (zink does this; native apps
    // like vkcube clamp it themselves). When set, the worker uses the host surface's real
    // currentExtent and skips the app-driven sizing + convergence gate (the app deferred to the
    // surface).
    bool use_current_extent = false;
    // (GL/zink): VkSwapchainCreateInfoKHR.oldSwapchain -- the app's previous swapchain on
    // this surface, retired by the new one. zink recreates the swapchain (resize / first present)
    // passing the old one; the worker MUST forward it as ci.oldSwapchain or the host rejects the
    // second swapchain on a still-bound surface with VK_ERROR_NATIVE_WINDOW_IN_USE_KHR.
    std::uint64_t old_swapchain = 0;

    json::Value to_body() const;
    static CreateSwapchainRequest from_body(const json::Value& body);
};

struct CreateSwapchainResponse {
    bool ok = false;
    std::string reason;
    std::uint64_t swapchain = 0;
    // VkResult (kVk* below); 0 = VK_SUCCESS unless the surface was out-of-date. A
    // ran-but-out-of-date create is ok=true + result=kVkErrorOutOfDateKhr (the app re-queries caps
    // + retries -- normal WSI); ok=false stays for true RPC/handle/validation faults. Mirrors
    // acquire/present's ok+result split. Back-compat: an old/silent worker decodes absent -> 0 =
    // VK_SUCCESS.
    int result = 0;

    json::Value to_body() const;
    static CreateSwapchainResponse from_body(const json::Value& body);
};

// get_swapchain_images: the swapchain's presentable images, as opaque handles. The
// app records per-image command buffers against these; acquire/present (which select
// among them) are handled separately. The image handles are owned by the swapchain and
// die with it (no explicit destroy), like queues under a device.
struct GetSwapchainImagesRequest {
    std::uint64_t swapchain = 0;

    json::Value to_body() const;
    static GetSwapchainImagesRequest from_body(const json::Value& body);
};

struct GetSwapchainImagesResponse {
    bool ok = false;
    std::string reason;
    std::vector<std::uint64_t> images;

    json::Value to_body() const;
    static GetSwapchainImagesResponse from_body(const json::Value& body);
};

// --- Acquire + present -----------------------------------
//
// `ok`/`reason` carry RPC/handle/validation faults (unknown handle, out-of-range
// image index, malformed body) -- the op did not run. `result` is the honest
// VkResult of an op that DID run, including the "successful failures" the app must
// act on: VK_SUCCESS (0), VK_SUBOPTIMAL_KHR (1000001003), and VK_ERROR_OUT_OF_DATE_KHR
// (1000001004). The geometry-dirty latch returns ok=true with
// result=VK_ERROR_OUT_OF_DATE_KHR and never touches the host driver.

// Vulkan result codes carried on the wire (subset; signed VkResult). Defined here so
// the mock (which never links Vulkan) and the app side agree with the real backend.
constexpr int kVkSuccess = 0;
constexpr int kVkNotReady = 1;   // VK_NOT_READY: get_fence_status honest unsignaled (non-fault)
constexpr int kVkTimeout = 2;    // VK_TIMEOUT: wait_for_fences honest, non-fault timeout
constexpr int kVkEventSet = 3;   // VK_EVENT_SET: get_event_status honest signaled (non-fault)
constexpr int kVkEventReset = 4; // VK_EVENT_RESET: get_event_status honest unsignaled (non-fault)
constexpr int kVkSuboptimalKhr = 1000001003;
constexpr int kVkErrorOutOfDateKhr = -1000001004;
// VK_ERROR_DEVICE_LOST: honest, non-fault -- the app must observe it (zink aborts cleanly). The
// worker's lost-device latch answers it locally for every post-loss op, never re-entering the host
// driver (a post-loss host call can block forever and hard-hang the session).
constexpr int kVkErrorDeviceLost = -4;

// VkImageUsageFlagBits the wire reasons about by value, so the mock (no
// Vulkan headers) and the app agree with the real backend's VK_IMAGE_USAGE_* enums.
constexpr int kImageUsageTransferSrc = 0x00000001;     // VK_IMAGE_USAGE_TRANSFER_SRC_BIT
constexpr int kImageUsageTransferDst = 0x00000002;     // VK_IMAGE_USAGE_TRANSFER_DST_BIT
constexpr int kImageUsageColorAttachment = 0x00000010; // VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT

struct AcquireNextImageRequest {
    std::uint64_t swapchain = 0;
    std::uint64_t timeout = 0;   // ns; UINT64_MAX = block. Carried as a decimal string.
    std::uint64_t semaphore = 0; // optional app sync object (0 = none)
    std::uint64_t fence = 0;     // optional app sync object (0 = none)

    json::Value to_body() const;
    static AcquireNextImageRequest from_body(const json::Value& body);
};

struct AcquireNextImageResponse {
    bool ok = false;
    std::string reason;
    int image_index = 0; // valid when result is VK_SUCCESS / VK_SUBOPTIMAL_KHR
    int result = 0;      // VkResult (see kVk* above)

    json::Value to_body() const;
    static AcquireNextImageResponse from_body(const json::Value& body);
};

// One {swapchain, image_index} target of a present. image_index decodes wide (a
// missing / out-of-range value becomes -1 and is rejected against image_count),
// never truncated into a plausible small index.
struct PresentEntry {
    std::uint64_t swapchain = 0;
    int image_index = 0;
};

struct QueuePresentRequest {
    std::uint64_t queue = 0;
    std::vector<std::uint64_t> wait_semaphores;
    std::vector<PresentEntry> presents;

    json::Value to_body() const;
    static QueuePresentRequest from_body(const json::Value& body);
};

struct QueuePresentResponse {
    bool ok = false;
    std::string reason;
    int result = 0;           // overall VkResult (worst across targets)
    std::vector<int> results; // per-swapchain VkResult, parallel to the request

    json::Value to_body() const;
    static QueuePresentResponse from_body(const json::Value& body);
};

// The exact `reason` create_swapchain returns when a requested image_usage bit is not in
// the surface's supportedUsageFlags (the one expected, capability-based failure). Tests
// skip the app-recorded clear proof on *this* reason only -- any other create_swapchain
// failure is a real regression they must surface. (COLOR_ATTACHMENT is guaranteed by
// spec; in practice the only realistic gap is TRANSFER_DST.) Shared so backend + tests
// agree on the exact string.
constexpr char kSwapchainUsageNotSupportedReason[] =
    "requested swapchain image usage not supported by the surface";

// --- Command recording + queue submit -------------------
//
// The app records a command stream into a command buffer and submits it. The whole
// begin->[cmds]->end recording ships in ONE record_command_buffer message (no per-vkCmd
// round-trip), then queue_submit references the recorded buffer(s). record + submit
// extend the window-thread serialization (they may touch swapchain-backed images);
// queue_submit returns the honest VkResult and never synthesizes OUT_OF_DATE.

// VkFlags are 32-bit. A stage/access mask carried wide (long long) must fit, or a narrowing
// cast to VkPipelineStageFlags/VkAccessFlags would silently truncate it -- the wide-decode
// bug class the wire layer guards against. A sync1 stage mask must additionally be non-zero;
// an access mask may be zero. Shared by the mock + the real backend's record/submit checks.
constexpr long long kVkFlagsMax = 0xFFFFFFFFLL;
inline bool valid_stage_mask(long long m) {
    return m > 0 && m <= kVkFlagsMax;
}
inline bool valid_access_mask(long long m) {
    return m >= 0 && m <= kVkFlagsMax;
}
// Compute: zink emits global/buffer barriers with a ZERO stage mask (sync2's NONE
// semantics through the sync1 entry point); every real driver it runs on tolerates it, so the
// relay carries it VERBATIM (the host driver is the authority) instead of rejecting -- the
// image-barrier path keeps the strict non-zero rule it has always had.
inline bool valid_stage_mask_or_none(long long m) {
    return m >= 0 && m <= kVkFlagsMax;
}

// sync2's VkPipelineStageFlags2 / VkAccessFlags2 are true 64-bit and are carried
// as std::uint64_t -- NEVER through the sync1 kVkFlagsMax cap (that 32-bit ceiling is exactly what
// sync2 sheds). Structurally any 64-bit value is legal (VK_PIPELINE_STAGE_2_NONE == 0 included);
// the host driver + validation layer are the authority on which specific bits are legal for a given
// op. Named + unconditional so the call sites read symmetrically with the sync1 validators and the
// "no cap, zero allowed" intent lives in one place.
inline bool valid_stage_mask2(std::uint64_t) {
    return true;
}
inline bool valid_access_mask2(std::uint64_t) {
    return true;
}
// vkCmdWriteTimestamp2's `stage` is NOT a dependency mask: it must name EXACTLY ONE pipeline stage
// (VUID-vkCmdWriteTimestamp2-stage-03859) -- nonzero, a single bit set. Do NOT validate it with the
// permit-anything valid_stage_mask2.
inline bool valid_timestamp_stage2(std::uint64_t m) {
    return m != 0 && (m & (m - 1)) == 0;
}

// the typed VK_KHR_synchronization2 dependency wire. A DependencyInfo2 mirrors
// VkDependencyInfo -- dependencyFlags + three barrier arrays -- with 64-bit stage/access masks
// (never the sync1 32-bit cap), guest handles kept as handles, and enums kept explicit numeric
// fields. Reused by pipeline_barrier2 (one dependency), set_event2 (one), and wait_events2 (one per
// waited event). Queue-family indices are carried faithfully (VK_QUEUE_FAMILY_IGNORED or the single
// exposed family) but normalized to IGNORED at replay (no ownership transfer is requested).
struct MemoryBarrier2 {
    std::uint64_t src_stage = 0;
    std::uint64_t src_access = 0;
    std::uint64_t dst_stage = 0;
    std::uint64_t dst_access = 0;
};
struct BufferMemoryBarrier2 {
    std::uint64_t src_stage = 0;
    std::uint64_t src_access = 0;
    std::uint64_t dst_stage = 0;
    std::uint64_t dst_access = 0;
    long long src_queue_family = 0; // wide: VK_QUEUE_FAMILY_IGNORED (0xFFFFFFFF) rides intact
    long long dst_queue_family = 0;
    std::uint64_t buffer = 0; // guest handle
    std::uint64_t offset = 0;
    std::uint64_t size = 0; // ~0 = VK_WHOLE_SIZE
};
struct ImageMemoryBarrier2 {
    std::uint64_t src_stage = 0;
    std::uint64_t src_access = 0;
    std::uint64_t dst_stage = 0;
    std::uint64_t dst_access = 0;
    int old_layout = 0;
    int new_layout = 0;
    long long src_queue_family = 0;
    long long dst_queue_family = 0;
    std::uint64_t image = 0; // guest handle
    long long aspect = 0;
    long long base_mip = 0;
    long long level_count = 0; // VK_REMAINING_* rides as 0xFFFFFFFF
    long long base_layer = 0;
    long long layer_count = 0;
};
struct DependencyInfo2 {
    std::uint64_t dependency_flags = 0;
    std::vector<MemoryBarrier2> memory;
    std::vector<BufferMemoryBarrier2> buffer;
    std::vector<ImageMemoryBarrier2> image;
};
// Structural (handle-independent) validation of a DependencyInfo2 shared by both backends so their
// reason strings match: the semantic caps, the queue-family policy (both IGNORED or both the single
// exposed family 0), and layout/aspect sanity. 64-bit masks are permit-all (valid_*_mask2). Each
// backend additionally checks buffer/image handle liveness on the CB's device. Returns false + sets
// `reason` on the first violation.
bool validate_dependency_info2(const DependencyInfo2& d, std::string& reason);
// Hard semantic caps: explicit per-array bounds so malformed input is
// rejected structurally, with overflow-safe length arithmetic (like the wait_events invariant).
constexpr std::uint64_t kMaxSync2Dependencies = 256;   // DependencyInfo2 per command
constexpr std::uint64_t kMaxSync2BarriersPerDep = 256; // memory/buffer/image per DependencyInfo2
constexpr std::uint64_t kMaxSync2SubmitInfos = 256;    // VkSubmitInfo2 per vkQueueSubmit2
constexpr std::uint64_t kMaxSync2SemaphoresPerSubmit = 64; // waits/signals per VkSubmitInfo2
constexpr std::uint64_t kMaxSync2CommandBuffersPerSubmit = 256;
// VK_QUEUE_FAMILY_IGNORED as a wide value (the mock has no Vulkan headers). A sync2 buffer/image
// barrier's queue-family pair must be both this OR both the single exposed guest family (0) -- any
// real ownership transfer is rejected. At replay both normalize to VK_QUEUE_FAMILY_IGNORED.
constexpr long long kVkQueueFamilyIgnored = 0xFFFFFFFFLL;
// VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME spelled out (the mock has no Vulkan headers). Both
// backends gate every sync2 command on this extension being enabled AND the synchronization2
// feature (mock == real -- feature-without-extension must reject, not
// accept).
constexpr const char* kSync2ExtensionName = "VK_KHR_synchronization2";
// VK_EXT_vertex_attribute_divisor, by string (both backends check their enabled-extension set for
// it to re-assert the divisor-pNext admission gate independently of the ICD).
// Matches VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME so mock == real.
constexpr const char* kVertexAttributeDivisorExtensionName = "VK_EXT_vertex_attribute_divisor";
// geometry-stream: the extension whose enablement (plus the geometryStreams feature) gates the
// rasterization-stream pipeline pNext (shared rasterization_stream_ok; mock == real).
constexpr const char* kTransformFeedbackExtensionName = "VK_EXT_transform_feedback";
// The mock's deterministic modeled stream properties (it has no host to query): generous enough
// that the accept path and every reject branch are exercised through the SAME shared validator the
// real worker parameterizes with its cached host values -- policy parity comes from the one
// helper, not from pretending a real GPU has these limits.
constexpr std::uint32_t kMockMaxTransformFeedbackStreams = 4;
constexpr bool kMockTransformFeedbackStreamSelect = true;
// The three-state geometry_streams_feature_enabled scalar.
// OMITTED is assigned ONLY when the key is ABSENT from an older ICD's wire body -- the
// worker then derives the enabled state from the forwarded feature chain. A PRESENT value must be
// exactly 0 or 1; anything else (including a forged -1 claiming legacy status to dodge the
// scalar/chain agreement check) decodes INVALID, which create_device rejects by name (mock ==
// real). Presence is decided at from_body; a hostile client cannot transmit its way into the
// omitted state.
constexpr int kGeometryStreamsScalarOmitted = -1;
constexpr int kGeometryStreamsScalarInvalid = -2;

// One recorded command. The subset is the minimum a clear frame needs:
// a single image-memory pipeline barrier and a full-subresource color clear. `kind`
// tags which fields are meaningful. Ints (layouts/stages/access/aspect) decode wide so
// a malformed wire value is rejected by validation, not truncated; `image` is a handle.
struct RecordedCommand {
    std::string kind; // "pipeline_barrier" | "clear_color_image"
    std::uint64_t image = 0;

    // pipeline_barrier: a single VkImageMemoryBarrier over the whole color subresource.
    int old_layout = 0;
    int new_layout = 0;
    long long src_stage = 0; // VkPipelineStageFlags (must be non-zero)
    long long dst_stage = 0; // VkPipelineStageFlags (must be non-zero)
    long long src_access = 0;
    long long dst_access = 0;
    int aspect = 0; // VkImageAspectFlags

    // clear_color_image: clears the full color subresource to {r,g,b,a} in `layout`.
    int layout = 0;
    double r = 0.0;
    double g = 0.0;
    double b = 0.0;
    double a = 0.0;

    // --- Draw subset ------------------------------------------------------
    // Additional `kind`s the bufferless triangle records: "begin_render_pass", "end_render_pass",
    // "bind_pipeline", "set_viewport", "set_scissor", "draw". Each kind reads only its own fields
    // (the others stay default); ints decode wide with a -1 sentinel so a malformed value is
    // rejected by validation, not truncated. begin_render_pass reuses {r,g,b,a} as the single
    // color clear value (loadOp=CLEAR).
    std::uint64_t render_pass = 0; // begin_render_pass
    std::uint64_t framebuffer = 0; // begin_render_pass
    // begin_render_pass depth clear: an EXPLICIT presence
    // flag
    // -- a missing depth_clear must NOT decode to 0.0 as if the app supplied a real clear (0.0 and
    // 1.0 are both valid depth clears, so a sentinel double will not do). has_depth_clear is true
    // iff the begin carried a color+depth clear pair; the worker rejects a depth render pass begun
    // without it (and a color-only pass begun with it). No stencil clear is representable.
    bool has_depth_clear = false;
    double depth_clear = 0.0;
    std::uint64_t pipeline = 0; // bind_pipeline (GRAPHICS)
    int render_area_x = 0;      // begin_render_pass: VkRect2D offset/extent
    int render_area_y = 0;
    int render_area_w = -1;
    int render_area_h = -1;
    double vp_x = 0.0; // set_viewport: VkViewport (floats); count 1, firstViewport 0
    double vp_y = 0.0;
    double vp_w = 0.0;
    double vp_h = 0.0;
    double vp_min_depth = 0.0;
    double vp_max_depth = 0.0;
    int sc_x = 0; // set_scissor: VkRect2D; count 1, firstScissor 0
    int sc_y = 0;
    int sc_w = -1;
    int sc_h = -1;
    long long vertex_count = -1; // draw: u32 args carried wide (-1 = missing/invalid)
    long long instance_count = -1;
    long long first_vertex = -1;
    long long first_instance = -1;

    // --- bind_vertex_buffers --------------------------------------------
    // kind "bind_vertex_buffers": firstBinding + parallel buffers[] / offsets[] arrays. subset
    // requires first_binding == 0 and a non-empty, equal-length pair. first_binding decodes wide
    // (-1 = missing); offsets are VkDeviceSize (decimal strings).
    int first_binding = -1;
    std::vector<std::uint64_t> vertex_buffers;
    std::vector<std::uint64_t> vertex_buffer_offsets;

    // --- bind_descriptor_sets -------------------------------------------
    // kind "bind_descriptor_sets": the GRAPHICS bind point only, firstSet == 0, no dynamic offsets.
    // `desc_layout` is the pipeline layout the sets are bound against (bind/draw
    // exactness keys on it); `descriptor_sets` is the ordered set list (size must equal the
    // layout's setLayoutCount). first_set decodes wide (-1 = missing).
    std::uint64_t desc_layout = 0;
    int first_set = -1;
    std::vector<std::uint64_t> descriptor_sets;

    // --- copy_buffer_to_image + generalized pipeline_barrier -----------------
    // kind "copy_buffer_to_image": `src_buffer` -> the dest `image`, ALL regions with ALL
    // VkBufferImageCopy fields carried in args_i64=[dstImageLayout, regionCount, then 13 values
    // per region: bufferOffset, bufferRowLength, bufferImageHeight, aspectMask, mipLevel,
    // baseArrayLayer, layerCount, imageOffset.{x,y,z}, imageExtent.{w,h,d}] -- the
    // copy_image_to_buffer convention, symmetric. The worker validates each region against the
    // image it tracks (mips/layers/mip-level extents); the named remainder (layouts, COLOR-only,
    // 2D scope, stride VUs) is asserted ICD-side AND worker-side. Destroying EITHER src_buffer OR
    // image invalidates the baked CB (both join referenced_draw_objects at record time).
    std::uint64_t src_buffer = 0;
    // copy_width/height/depth are GENERIC codec fields (kept wire-compatible; exercised by the
    // codec parity test) -- copy_buffer_to_image itself no longer uses them (its regions ride
    // args_i64, above).
    long long copy_width = -1;  // VkBufferImageCopy.imageExtent.width (u32 wide; -1 = missing)
    long long copy_height = -1; // .height
    long long copy_depth = -1;  // .depth
    // pipeline_barrier mip/layer range: the swapchain-color barrier left these
    // implied (the whole single subresource); the generalized texture barrier carries them
    // honestly. -1 = missing (the worker then treats it as the whole single subresource, the legacy
    // shape). The accepted transitions are an allowlist (UNDEFINED->TRANSFER_DST,
    // TRANSFER_DST->SHADER_READ_ONLY, PREINITIALIZED->SHADER_READ_ONLY for the texture upload, plus
    // the legacy swapchain-color cases); queue-family indices are NOT carried and are ignored.
    int barrier_base_mip = -1;
    int barrier_level_count = -1;
    int barrier_base_layer = -1;
    int barrier_layer_count = -1;

    // --- (GL/zink): generic positional payload for the broad faithful command set
    // --------- The vkcube draw subset above uses named fields per kind; zink's much wider vkCmd*
    // stream (copy_buffer, bind_index_buffer, draw_indexed, general barriers, copies/blits, push
    // constants,
    // ...) rides these generic arrays instead, so a new kind costs an ICD recorder + a worker
    // emitter and no struct churn. Each kind documents its own positional layout. u64 carries
    // handles + VkDeviceSize; i64 carries enums/flags/counts (wide); f64 carries floats; blob
    // carries inline bytes (push constants, inline buffer updates). Handles named here join
    // referenced_draw_objects.
    std::vector<std::uint64_t> args_u64;
    std::vector<long long> args_i64;
    std::vector<double> args_f64;
    std::string args_blob;

    // --- VK_KHR_synchronization2 typed dependency payload --------------------
    // kind "pipeline_barrier2": deps2.size()==1, args_u64 empty. kind "set_event2":
    // deps2.size()==1, args_u64=[event]. kind "wait_events2": deps2.size()==N paired with
    // args_u64=[event x N] (one dependency per waited event; deps2.size()==args_u64.size()). kind
    // "reset_event2": args_u64=[event, stageMask64], deps2 empty. kind "write_timestamp2":
    // args_u64=[queryPool, stageMask64], args_i64=[query], deps2 empty. The typed group keeps
    // 64-bit masks unsplit and each barrier kind independently fuzzable (vs. an overloaded args
    // bag).
    std::vector<DependencyInfo2> deps2;
};

// the recorded-command kind vocabulary as an enum. The `kind` STRING stays the wire +
// struct identity (every existing producer/pin unchanged); this enum exists so the two backends'
// per-command dispatch is ONE hash lookup (cmd_kind_from_string at the top of the loop) plus
// integer compares, instead of a ~40-arm string-compare chain per command -- at ETR-class volumes
// (~1.5M commands / 40 s) the chains were a measured slice of the worker's validate time. Unknown
// strings map to Unknown, which every dispatch chain rejects exactly as the old final `else` did.
enum class CmdKind : unsigned char {
    Unknown = 0,
    PipelineBarrier,
    ClearColorImage,
    BeginRenderPass,
    EndRenderPass,
    BindPipeline,
    SetViewport,
    SetScissor,
    Draw,
    DrawIndexed,
    DrawIndirectByteCount,
    BindVertexBuffers,
    BindDescriptorSets,
    BindIndexBuffer,
    PushConstants,
    Dispatch,
    DispatchIndirect,
    MemoryBarrierGlobal, // kind "memory_barrier"; windows.h #defines MemoryBarrier (a fence
                         // intrinsic), so the enumerator deviates from the string
    BufferMemoryBarrier,
    CopyBufferToImage,
    CopyBuffer,
    CopyImageToBuffer,
    ClearAttachments,
    BlitImage,
    CopyImage,
    BindTransformFeedbackBuffers,
    BeginTransformFeedback,
    EndTransformFeedback,
    BeginConditionalRendering,
    EndConditionalRendering,
    ResetQueryPool,
    BeginQuery,
    EndQuery,
    WriteTimestamp,
    CopyQueryPoolResults,
    SetLineWidth,
    SetDepthBias,
    SetBlendConstants,
    SetDepthBounds,
    SetStencilCompareMask,
    SetStencilWriteMask,
    SetStencilReference,
    // (native lane -- EDS1): VK_EXT_extended_dynamic_state, each a single u32 in
    // args_i64[0].
    SetCullMode,
    SetFrontFace,
    SetPrimitiveTopology,
    SetDepthTestEnable,
    SetDepthWriteEnable,
    SetDepthCompareOp,
    // (native lane): VK_KHR_dynamic_rendering. begin_rendering flattens
    // VkRenderingInfo into the generic arrays (args_i64 header + per-attachment enums, args_u64 the
    // guest image-view handles, args_blob the raw per-attachment VkClearValue unions);
    // end_rendering carries nothing.
    BeginRendering,
    EndRendering,
    // sync1 command events on the VkEvent object. set_event / reset_event carry
    // args_u64=[event(guest handle)] + src_stage (the 32-bit stageMask). wait_events is the atomic
    // one -- a single command carrying the event SET plus the three sync1 barrier arrays flattened
    // fixed-slot into args_u64 (header [eventCount, memCount, bufCount, imgCount], then events,
    // memory[2], buffer[5], image[10]); src_stage/dst_stage are the command's global stage masks.
    // Enumerators are Cmd*-prefixed: windows.h declares global SetEvent/ResetEvent functions.
    CmdSetEvent,
    CmdResetEvent,
    CmdWaitEvents,
    // VK_KHR_synchronization2 command-buffer commands (native lane). The typed
    // deps2 group carries their barriers; args_u64 carries event handles / query-pool + 64-bit
    // stage. Cmd*-prefixed for the event variants (global Win32 SetEvent/ResetEvent), and
    // PipelineBarrier2/WriteTimestamp2 for symmetry.
    PipelineBarrier2,
    WriteTimestamp2,
    CmdSetEvent2,
    CmdResetEvent2,
    CmdWaitEvents2,
    // Vulkan 1.3 support: the remaining EDS1 setters + the core-1.3 EDS2 subset +
    // vkCmdResolveImage. Positional layouts:
    //   set_viewport_with_count: args_i64=[count]; args_f64 = per viewport [x, y, width, height,
    //     minDepth, maxDepth] (6*count).
    //   set_scissor_with_count: args_i64=[count, then per scissor x, y, w, h] (1+4*count).
    //   bind_vertex_buffers2: args_i64=[firstBinding, count, has_sizes(0/1), has_strides(0/1)];
    //     args_u64=[buffers x count, offsets x count, sizes x count (present iff has_sizes),
    //     strides x count (present iff has_strides)].
    //   set_depth_bounds_test_enable / set_stencil_test_enable / set_rasterizer_discard_enable /
    //     set_depth_bias_enable / set_primitive_restart_enable: args_i64=[enable(0/1)].
    //   set_stencil_op: args_i64=[faceMask, failOp, passOp, depthFailOp, compareOp].
    //   resolve_image: args_u64=[srcImage, dstImage]; args_i64=[srcLayout, dstLayout, regionCount,
    //     then 17 per region: srcSubresource(aspectMask, mipLevel, baseArrayLayer, layerCount),
    //     srcOffset(x, y, z), dstSubresource(4), dstOffset(x, y, z), extent(w, h, d)] -- the
    //     blit_image 20-slot model minus filter, with one extent instead of second corner offsets.
    SetViewportWithCount,
    SetScissorWithCount,
    BindVertexBuffers2,
    SetDepthBoundsTestEnable,
    SetStencilTestEnable,
    SetStencilOp,
    SetRasterizerDiscardEnable,
    SetDepthBiasEnable,
    SetPrimitiveRestartEnable,
    ResolveImage,
    // faithful vkCmdFillBuffer (Mesa >= 25 zink emits it for GL buffer clears/initialization).
    // fill_buffer: args_u64=[dstBuffer, dstOffset, size, data]; size = VK_WHOLE_SIZE
    // (0xFFFFFFFFFFFFFFFF) means fill-to-end per spec, data is the 32-bit fill word. APPEND-ONLY
    // enum (the wire is string-keyed; integer positions are per-TU baked).
    FillBuffer,
};
CmdKind cmd_kind_from_string(const std::string& kind);

// the fixed-slot per-barrier widths for the wait_events args_u64 flatten (the
// sync1 barrier arrays). memory = [srcAccess, dstAccess]; buffer = [srcAccess, dstAccess, buffer,
// offset, size]; image = [srcAccess, dstAccess, oldLayout, newLayout, image, aspect, baseMip,
// levelCount, baseLayer, layerCount]. The 4-slot header [eventCount, memCount, bufCount, imgCount]
// re-derives the total: 4 + eventCount + memCount*2 + bufCount*5 + imgCount*10. Counts are bounded.
constexpr std::size_t kWaitEventsHeaderSlots = 4;
constexpr std::size_t kWaitEventsMemorySlots = 2;
constexpr std::size_t kWaitEventsBufferSlots = 5;
constexpr std::size_t kWaitEventsImageSlots = 10;
constexpr std::uint64_t kMaxWaitEventsBarriers = 256; // per-array + event-count cap (fail-closed)

// (native lane --, dynamic rendering): shared wire constants. The mock backend
// (this file) is Vulkan-header-free, so it cannot say sizeof(VkClearValue); the ICD recorder and
// the worker -- which DO include vulkan.h -- static_assert(sizeof(VkClearValue) ==
// kClearValueBytes) so the "16" is drift-guarded exactly once where the type is visible.
// begin_rendering carries one raw VkClearValue (16 bytes) per attachment in args_blob.
// The color-attachment cap bounds the wire shape; the worker additionally checks the host's real
// maxColorAttachments.
constexpr std::size_t kClearValueBytes = 16;
constexpr int kMaxDynamicRenderingColorAttachments = 8;

struct RecordCommandBufferRequest {
    std::uint64_t command_buffer = 0;
    bool one_time_submit = false;
    std::vector<RecordedCommand> commands;

    json::Value to_body() const;
    static RecordCommandBufferRequest from_body(const json::Value& body);
    // RpcOp::RecordCommandBufferRaw (SPARSE since, wire v2):
    // [u32 json_len][{v=2,command_buffer,one_time_submit,count} header][packed commands]. Each
    // command is `kind` (u64-length-prefixed) + a u64 PRESENCE MASK, then ONLY the field groups
    // whose bit is set -- a group is present iff any field in it differs from the struct default,
    // and each present group's fields ride in a FROZEN order (LE scalars; ints wide as i64; f64
    // bit-cast; length-prefixed strings/vectors). An absent group decodes to the struct default,
    // so the decoded request is field-identical to the JSON path (pinned in unit_vkrpc via
    // to_body() equality). from_wire fails CLOSED (err set) on a short/over-cap/overrunning
    // header, a non-v2 version, an unknown mask bit, any truncated field, an alloc-bomb count, or
    // trailing bytes.
    std::string to_wire() const;
    static RecordCommandBufferRequest from_wire(const std::string& body, std::string& err);
};

// One {semaphore, stage} wait of a submit. `stage` is the VkPipelineStageFlags the wait
// applies at (sync1 requires it be non-zero); carried wide and validated.
struct SubmitWait {
    std::uint64_t semaphore = 0;
    long long stage = 0;
    std::uint64_t value = 0; // timeline wait value (ignored for a binary semaphore)
};

struct QueueSubmitRequest {
    std::uint64_t queue = 0;
    std::vector<SubmitWait> waits;
    std::vector<std::uint64_t> command_buffers; // may be empty (fence/sema-only submit)
    std::vector<std::uint64_t> signal_semaphores;
    // timeline SIGNAL values, parallel to signal_semaphores (ignored for binary). Empty on
    // a legacy/binary-only submit -> the worker treats every signal value as 0.
    std::vector<std::uint64_t> signal_values;
    std::uint64_t fence = 0; // 0 = none

    json::Value to_body() const;
    static QueueSubmitRequest from_body(const json::Value& body);
};

// vkQueueSubmit2. A VkSemaphoreSubmitInfo shape -- {semaphore, value(timeline),
// stageMask(64-bit)}; the wait side widens SubmitWait's 32-bit stage to 64-bit, and the signal side
// gains a per-signal 64-bit stage (sync1's signal side was a bare {semaphore,value} pair).
struct SemaphoreSubmit2 {
    std::uint64_t semaphore = 0;
    std::uint64_t value = 0;      // timeline value (ignored for a binary semaphore)
    std::uint64_t stage_mask = 0; // VkPipelineStageFlags2 (64-bit)
};
// One VkSubmitInfo2: its own waits / command buffers / signals (per-submit ordering + wait/signal
// association stay exact). Reject nonzero deviceIndex/deviceMask/flags + unsupported pNext at the
// ICD (one device / one queue family) -- never carried here.
struct SubmitInfo2 {
    std::vector<SemaphoreSubmit2> waits;
    std::vector<std::uint64_t> command_buffers;
    std::vector<SemaphoreSubmit2> signals;
};
// The whole vkQueueSubmit2 batch in ONE RPC: a vector of VkSubmitInfo2 + one fence, replayed via a
// single native pfn_queue_submit2(queue, submitCount, submits, fence) call. Response = the shared
// QueueSubmitResponse (honest VkResult).
struct QueueSubmit2Request {
    std::uint64_t queue = 0;
    std::vector<SubmitInfo2> submits;
    std::uint64_t fence = 0; // 0 = none

    json::Value to_body() const;
    static QueueSubmit2Request from_body(const json::Value& body);
};

// (GL/zink): host-side timeline-semaphore ops.
struct WaitSemaphoresRequest {
    std::uint64_t device = 0;
    std::vector<std::uint64_t> semaphores;
    std::vector<std::uint64_t> values; // parallel to semaphores
    std::uint64_t timeout = 0;
    int wait_any = 0; // VK_SEMAPHORE_WAIT_ANY_BIT (else wait-all)

    json::Value to_body() const;
    static WaitSemaphoresRequest from_body(const json::Value& body);
};

struct WaitSemaphoresResponse {
    bool ok = false;
    std::string reason;
    int result = 0; // VkResult (VK_SUCCESS / VK_TIMEOUT honestly relayed)

    json::Value to_body() const;
    static WaitSemaphoresResponse from_body(const json::Value& body);
};

struct SignalSemaphoreRequest {
    std::uint64_t device = 0;
    std::uint64_t semaphore = 0;
    std::uint64_t value = 0;

    json::Value to_body() const;
    static SignalSemaphoreRequest from_body(const json::Value& body);
};

struct GetSemaphoreCounterValueRequest {
    std::uint64_t device = 0;
    std::uint64_t semaphore = 0;

    json::Value to_body() const;
    static GetSemaphoreCounterValueRequest from_body(const json::Value& body);
};

struct GetSemaphoreCounterValueResponse {
    bool ok = false;
    std::string reason;
    int result = 0; // honest VkResult; additive (legacy payloads decode as VK_SUCCESS)
    std::uint64_t value = 0;

    json::Value to_body() const;
    static GetSemaphoreCounterValueResponse from_body(const json::Value& body);
};

struct QueueSubmitResponse {
    bool ok = false;
    std::string reason;
    int result = 0; // honest VkResult from vkQueueSubmit

    json::Value to_body() const;
    static QueueSubmitResponse from_body(const json::Value& body);
};

struct ResetFencesRequest {
    std::vector<std::uint64_t> fences; // non-empty, all on one device

    json::Value to_body() const;
    static ResetFencesRequest from_body(const json::Value& body);
};

struct WaitForFencesRequest {
    std::vector<std::uint64_t> fences; // non-empty, all on one device
    bool wait_all = true;
    std::uint64_t timeout = 0; // ns; UINT64_MAX = block

    json::Value to_body() const;
    static WaitForFencesRequest from_body(const json::Value& body);
};

struct WaitForFencesResponse {
    bool ok = false;
    std::string reason;
    int result = 0; // honest VkResult: VK_SUCCESS (0) or VK_TIMEOUT (2)

    json::Value to_body() const;
    static WaitForFencesResponse from_body(const json::Value& body);
};

// vkGetFenceStatus: the fence-family completeness fix. Request = HandleRequest
// {handle = fence}. VK_SUCCESS (0, signaled) and VK_NOT_READY (1, unsignaled) are both NORMAL
// returns -- the WaitForFences idiom (ok=true, the app reads `result`); only a transport failure
// faults.
struct GetFenceStatusResponse {
    bool ok = false;
    std::string reason;
    int result = 0; // honest VkResult: VK_SUCCESS (0) or VK_NOT_READY (1)

    json::Value to_body() const;
    static GetFenceStatusResponse from_body(const json::Value& body);
};

// REAL idle waits (vkQueueWaitIdle / vkDeviceWaitIdle on the worker), so an
// idle wait is an honest readback-completion sync point. Request = HandleRequest (the queue or
// device handle).
struct WaitIdleResponse {
    bool ok = false;
    std::string reason;
    int result = 0; // honest VkResult

    json::Value to_body() const;
    static WaitIdleResponse from_body(const json::Value& body);
};

// --- App-facing WSI capability queries -------------------
//
// A native Vulkan app queries surface capabilities/formats/present-modes/support before
// creating a swapchain. The worker forwards these to the real host surface so the app's
// view is honest (supportedUsageFlags incl. TRANSFER_DST, real formats/present-modes); the
// mock returns canned values so the path is dual-platform-testable. All take the
// {physical_device, surface} pair (support also a queue_family_index); the worker validates
// the surface belongs to the physical device's instance.

// 0xFFFFFFFF dynamic-extent sentinel: surface caps report this as currentExtent so the
// app sizes the swapchain to its own framebuffer; the host surface's concrete extent never
// crosses the wire. Carried wide (decimal string) since it exceeds INT_MAX.
constexpr std::uint64_t kDynamicExtentSentinel = 0xFFFFFFFFull;

struct GetSurfaceCapabilitiesRequest {
    std::uint64_t physical_device = 0;
    std::uint64_t surface = 0;

    json::Value to_body() const;
    static GetSurfaceCapabilitiesRequest from_body(const json::Value& body);
};

// Mirrors VkSurfaceCapabilitiesKHR (the fields a swapchain setup reads). Every Vulkan u32
// field rides wide on the wire (carried as a JSON number / decoded with get_i64) so a driver
// value is never truncated -- the project's no-narrowing wire discipline; the ICD clamps back
// into the real VkSurfaceCapabilitiesKHR. currentExtent carries the 0xFFFFFFFF sentinel.
struct GetSurfaceCapabilitiesResponse {
    bool ok = false;
    std::string reason;
    std::uint64_t min_image_count = 0;
    std::uint64_t max_image_count = 0;       // 0 = no limit (Vulkan convention)
    std::uint64_t current_extent_width = 0;  // 0xFFFFFFFF sentinel
    std::uint64_t current_extent_height = 0; // 0xFFFFFFFF sentinel
    std::uint64_t min_image_extent_width = 0;
    std::uint64_t min_image_extent_height = 0;
    std::uint64_t max_image_extent_width = 0;
    std::uint64_t max_image_extent_height = 0;
    std::uint64_t max_image_array_layers = 0;
    std::uint64_t supported_transforms = 0;      // VkSurfaceTransformFlagsKHR
    std::uint64_t current_transform = 0;         // VkSurfaceTransformFlagBitsKHR
    std::uint64_t supported_composite_alpha = 0; // VkCompositeAlphaFlagsKHR
    std::uint64_t supported_usage_flags = 0;     // VkImageUsageFlags (honest; incl. TRANSFER_DST)

    json::Value to_body() const;
    static GetSurfaceCapabilitiesResponse from_body(const json::Value& body);
};

// One {format, color_space} pair the surface supports (VkSurfaceFormatKHR).
struct SurfaceFormat {
    int format = 0;
    int color_space = 0;
};

struct GetSurfaceFormatsRequest {
    std::uint64_t physical_device = 0;
    std::uint64_t surface = 0;

    json::Value to_body() const;
    static GetSurfaceFormatsRequest from_body(const json::Value& body);
};

struct GetSurfaceFormatsResponse {
    bool ok = false;
    std::string reason;
    std::vector<SurfaceFormat> formats;

    json::Value to_body() const;
    static GetSurfaceFormatsResponse from_body(const json::Value& body);
};

struct GetSurfacePresentModesRequest {
    std::uint64_t physical_device = 0;
    std::uint64_t surface = 0;

    json::Value to_body() const;
    static GetSurfacePresentModesRequest from_body(const json::Value& body);
};

struct GetSurfacePresentModesResponse {
    bool ok = false;
    std::string reason;
    std::vector<int> present_modes; // VkPresentModeKHR values

    json::Value to_body() const;
    static GetSurfacePresentModesResponse from_body(const json::Value& body);
};

struct GetSurfaceSupportRequest {
    std::uint64_t physical_device = 0;
    int queue_family_index = 0; // the app-facing family (the worker exposes one: index 0)
    std::uint64_t surface = 0;

    json::Value to_body() const;
    static GetSurfaceSupportRequest from_body(const json::Value& body);
};

struct GetSurfaceSupportResponse {
    bool ok = false;
    std::string reason;
    bool supported = false;

    json::Value to_body() const;
    static GetSurfaceSupportResponse from_body(const json::Value& body);
};

// --- Draw surface: image views / shaders / render pass / framebuffer / pipeline --
//
// The bufferless-triangle draw spine. Each create carries a BOUNDED faithful subset of its Vulkan
// create-info (strict-but-total: a field outside the subset is rejected with ok=false/reason, never
// silently defaulted); the worker builds the real object, the mock models the handle lifecycle. All
// destroys use the shared HandleRequest/StatusResponse. No buffers/memory/descriptors.

// ImageView over a swapchain image. subset: viewType 2D, format = the swapchain format, identity
// component swizzle, COLOR aspect, single mip/array layer. All ints decode wide (-1 = missing).
struct CreateImageViewRequest {
    std::uint64_t image = 0; // a swapchain image OR an app-created image (texture / depth)
    int view_type = -1;      // VkImageViewType (2D)
    int format = -1;         // VkFormat (must match the viewed image's format)
    int swizzle_r = -1;      // VkComponentSwizzle (IDENTITY)
    int swizzle_g = -1;
    int swizzle_b = -1;
    int swizzle_a = -1;
    int aspect = -1;           // VkImageAspectFlags (COLOR)
    int base_mip_level = -1;   // 0
    int level_count = -1;      // 1
    int base_array_layer = -1; // 0
    int layer_count = -1;      // 1
    // (GL/zink): VkImageViewUsageCreateInfo's usage when the app chained one (0 = absent).
    // zink narrows a view's usage below its image's; the worker re-chains it on the real create.
    std::uint64_t view_usage = 0;

    json::Value to_body() const;
    static CreateImageViewRequest from_body(const json::Value& body);
};

struct CreateImageViewResponse {
    bool ok = false;
    std::string reason;
    std::uint64_t image_view = 0;

    json::Value to_body() const;
    static CreateImageViewResponse from_body(const json::Value& body);
};

// (GL/zink): a texel buffer view (VK_KHR_buffer_view core). zink creates one over a buffer
// for samplerBuffer/imageBuffer GLSL, and one over its null-descriptor dummy at screen init.
// Faithfully marshalled: buffer handle + format + offset/range carried verbatim, the worker builds
// a real VkBufferViewCreateInfo. offset/range are VkDeviceSize (carried wide as decimal strings);
// VK_WHOLE_SIZE is a legal range and round-trips.
struct CreateBufferViewRequest {
    std::uint64_t buffer = 0;
    int format = -1;          // VkFormat
    std::uint64_t offset = 0; // VkDeviceSize
    std::uint64_t range = 0;  // VkDeviceSize (may be VK_WHOLE_SIZE)

    json::Value to_body() const;
    static CreateBufferViewRequest from_body(const json::Value& body);
};

struct CreateBufferViewResponse {
    bool ok = false;
    std::string reason;
    std::uint64_t buffer_view = 0;

    json::Value to_body() const;
    static CreateBufferViewResponse from_body(const json::Value& body);
};

// ShaderModule -- the first BINARY-bodied op. Wire body is [u32 json_len LE][json header][raw
// SPIR-V bytes]; the json header carries {device, code_size}, the tail is the SPIR-V words.
// caps: code_size > 0, multiple of 4, <= kMaxShaderCodeBytes, and the tail length must EQUAL
// code_size (trailing bytes are a decode error). to_wire/from_wire (not to_body/from_body) own that
// framing; the response is plain JSON.
constexpr std::size_t kMaxShaderCodeBytes = 1u * 1024u * 1024u; // small-payload cap (1 MiB)
constexpr std::size_t kMaxBinaryJsonHeaderBytes = 64u * 1024u;  // bounds the json header

struct CreateShaderModuleRequest {
    std::uint64_t device = 0;
    std::uint64_t code_size = 0; // SPIR-V byte count (== code.size())
    std::string code;            // raw SPIR-V bytes (the binary tail)

    // [u32 json_len LE][{device, code_size} json][code bytes]. Total decoder: on any framing fault
    // (json_len past the buffer, malformed header json, tail length != code_size, oversize) sets
    // `err` and returns a default-constructed request (caller replies BadRequest). Never OOB-reads.
    std::string to_wire() const;
    static CreateShaderModuleRequest from_wire(const std::string& body, std::string& err);
};

struct CreateShaderModuleResponse {
    bool ok = false;
    std::string reason;
    std::uint64_t shader_module = 0;

    json::Value to_body() const;
    static CreateShaderModuleResponse from_body(const json::Value& body);
};

// One color attachment description (exactly one). All fields VkenumS carried wide (1 =
// missing).
struct AttachmentDesc {
    int format = -1;           // the swapchain format
    int samples = -1;          // 1
    int load_op = -1;          // CLEAR
    int store_op = -1;         // STORE
    int stencil_load_op = -1;  // DONT_CARE
    int stencil_store_op = -1; // DONT_CARE
    int initial_layout = -1;   // UNDEFINED
    int final_layout = -1;     // PRESENT_SRC_KHR
};

// One subpass dependency (0..2; srcSubpass/dstSubpass in {VK_SUBPASS_EXTERNAL, 0}). subpass
// indices + masks ride wide (VK_SUBPASS_EXTERNAL = 0xFFFFFFFF exceeds INT_MAX); -1 = missing.
struct SubpassDependencyDesc {
    long long src_subpass = -1;
    long long dst_subpass = -1;
    long long src_stage = -1;
    long long dst_stage = -1;
    long long src_access = -1;
    long long dst_access = -1;
    long long dependency_flags = -1;
};

// RenderPass. subset: exactly 1 color attachment, exactly 1 GRAPHICS subpass with exactly 1
// color attachment ref {attachment=0, layout}, no depth/input/resolve/preserve, 0..2 dependencies.
// lifts the depth half: the subpass MAY also reference a single depth-stencil attachment
// (depth_attachment >= 0 selects an attachment in `attachments` as the depthStencil ref; -1 = none,
// the color-only pass). With depth, attachments is size 2 (index 0 color, the depth
// one at depth_attachment). The depth attachment's layout transition is render-pass-owned
// (initialLayout UNDEFINED + the dependency), so no explicit app barrier.
// MRT: one subpass color-attachment reference. `attachment` is signed WIDE: -1 encodes
// VK_ATTACHMENT_UNUSED (0xFFFFFFFF must never ride an int cast), matching the existing
// depth_attachment = -1 = none convention. Use the helper, never ad hoc casts.
struct ColorRefDesc {
    long long attachment = -1; // -1 = VK_ATTACHMENT_UNUSED; else the attachment index
    int layout = -1;           // the reference layout (meaningful only when used)
};
constexpr long long kColorRefUnused = -1;
inline bool color_ref_used(const ColorRefDesc& r) {
    return r.attachment != kColorRefUnused;
}

struct CreateRenderPassRequest {
    std::uint64_t device = 0;
    std::vector<AttachmentDesc> attachments; // size 1; with depth: size 2
    int color_attachment = -1;               // the single subpass's color ref index (0)
    int color_layout = -1;                   // its layout (COLOR_ATTACHMENT_OPTIMAL)
    int depth_attachment = -1;               // the subpass's depthStencil ref index (-1 = no depth)
    int depth_layout = -1;                   // its layout (DEPTH_STENCIL_ATTACHMENT_OPTIMAL)
    // MRT: the subpass's FULL ordered color-reference array (may contain UNUSED holes -- zink
    // emits them for gapped glDrawBuffers). Empty = a legacy single-color request (the scalars
    // above rule and the worker keeps its old single-ref build) -- which is also the
    // old-ICD-vs-new-worker compatibility path. A new ICD populates BOTH (scalars from ref [0])
    // and only ever sends >1 ref to a worker that advertised DeviceCaps.max_color_attachments
    // (the capability gate), so a version-skewed pair degrades loudly, never silently.
    std::vector<ColorRefDesc> color_refs;
    std::vector<SubpassDependencyDesc> dependencies;
    // Required-feature audit (multiview): the single subpass's VkSubpassDescription2
    // viewMask (0 = a plain non-multiview pass, the historical shape). A non-zero mask is only ever
    // sent by the *2 (create_renderpass2) path, only for a device whose multiview feature is
    // enabled (both backends reject view_mask != 0 otherwise), and the worker builds a REAL
    // multiview render pass from it (VkRenderPassMultiviewCreateInfo). The correlated view masks
    // and per-dependency viewOffset stay fail-closed in the ICD subset -- only the subpass mask is
    // carried.
    int view_mask = 0;

    json::Value to_body() const;
    static CreateRenderPassRequest from_body(const json::Value& body);
};

struct CreateRenderPassResponse {
    bool ok = false;
    std::string reason;
    std::uint64_t render_pass = 0;

    json::Value to_body() const;
    static CreateRenderPassResponse from_body(const json::Value& body);
};

struct FramebufferAttachmentInfoDesc {
    std::uint64_t flags = 0;
    std::uint64_t usage = 0;
    int width = 0;
    int height = 0;
    int layer_count = 0;
    std::vector<int> view_formats;

    json::Value to_body() const;
    static FramebufferAttachmentInfoDesc from_body(const json::Value& body);
};

struct FramebufferExtentDesc {
    int width = 0;
    int height = 0;
};

// Host-safety policy for malformed imageless requests. Valid requests are unchanged; an
// attachment envelope smaller than the requested framebuffer bounds the host object so invalid
// guest Vulkan cannot become a device-losing host call.
FramebufferExtentDesc
host_safe_framebuffer_extent(int requested_width, int requested_height,
                             const std::vector<FramebufferAttachmentInfoDesc>& attachment_infos);
int host_safe_render_extent(int offset, int requested_extent, int framebuffer_extent);

// Framebuffer. subset: exactly 1 attachment (the color image view), layers == 1. adds
// an optional depth view (depth_image_view != 0): when the render pass has a depth attachment the
// framebuffer carries the depth view as its second attachment (ordered to match the render pass).
struct CreateFramebufferRequest {
    std::uint64_t device = 0;
    std::uint64_t render_pass = 0;
    std::uint64_t image_view = 0;       // the color attachment (attachment 0); 0 if imageless
    std::uint64_t depth_image_view = 0; // the depth attachment (0 = none or imageless)
    int width = -1;
    int height = -1;
    int layers = -1; // 1
    // (GL/zink): an IMAGELESS framebuffer carries no concrete views at create -- the worker
    // records {render_pass, extent, layers} and builds a regular VkFramebuffer at
    // vkCmdBeginRenderPass time from the views the begin supplies
    // (VkRenderPassAttachmentBeginInfo). attachment_count is how many views the begin must provide.
    bool imageless = false;
    int attachment_count = 0;
    // Native imageless-framebuffer metadata. A new ICD forwards the complete
    // VkFramebufferAttachmentsCreateInfo so the worker can create a real imageless framebuffer;
    // empty retains compatibility with the old begin-time regular-framebuffer emulation.
    std::vector<FramebufferAttachmentInfoDesc> attachment_infos;
    // MRT: ALL concrete attachment views, positional (matching the render pass's attachment
    // array). Empty = a legacy request (the scalars above rule -- also the old-ICD compatibility
    // path). Non-empty: the worker validates + builds from THIS vector; the scalars stay populated
    // from views [0]/[1] for an old worker.
    std::vector<std::uint64_t> attachment_views;

    json::Value to_body() const;
    static CreateFramebufferRequest from_body(const json::Value& body);
};

struct CreateFramebufferResponse {
    bool ok = false;
    std::string reason;
    std::uint64_t framebuffer = 0;

    json::Value to_body() const;
    static CreateFramebufferResponse from_body(const json::Value& body);
};

// PipelineLayout. subset: empty (setLayoutCount == 0). lifts the set-layout
// half: the layout may reference up to kMaxPipelineLayoutSetLayouts descriptor set layouts (carried
// as a handle list so the worker validates each + records the ordered list for the bind/draw
// exactness check); push constants stay pushConstantRangeCount == 0. set_layout_count must equal
// set_layouts.size() (the worker cross-checks; a mismatch is rejected).
// (GL/zink): one VkPushConstantRange (zink uses push constants for its uniforms).
// stage_flags is a VkShaderStageFlags mask; offset/size are 4-byte-aligned byte ranges in the
// push-constant block.
struct PushConstantRange {
    std::uint32_t stage_flags = 0;
    std::uint32_t offset = 0;
    std::uint32_t size = 0;

    json::Value to_body() const;
    static PushConstantRange from_body(const json::Value& body);
};

struct CreatePipelineLayoutRequest {
    std::uint64_t device = 0;
    int set_layout_count = -1;              // == set_layouts.size()
    int push_constant_range_count = -1;     // == push_constant_ranges.size()
    std::vector<std::uint64_t> set_layouts; // ordered descriptor set layout handles
    // (GL/zink): the push-constant ranges (additive; empty for the vkcube/canary path).
    std::vector<PushConstantRange> push_constant_ranges;

    json::Value to_body() const;
    static CreatePipelineLayoutRequest from_body(const json::Value& body);
};

struct CreatePipelineLayoutResponse {
    bool ok = false;
    std::string reason;
    std::uint64_t pipeline_layout = 0;

    json::Value to_body() const;
    static CreatePipelineLayoutResponse from_body(const json::Value& body);
};

// One pipeline shader stage (exactly VERTEX + FRAGMENT, entry "main").
struct ShaderStageDesc {
    int stage = -1;           // VkShaderStageFlagBits (VERTEX / FRAGMENT)
    std::uint64_t module = 0; // a shader module handle
    std::string entry;        // entry-point name ("main")
    // Vulkan 1.3 support (subgroupSizeControl): VkPipelineShaderStageCreateFlags (the two
    // subgroup-size bits are the only admitted values) + the
    // VkPipelineShaderStageRequiredSubgroupSizeCreateInfo pNext's requiredSubgroupSize (0 =
    // absent). Both additive; the backends gate them on the device's enabled vk13 bits.
    long long stage_flags = 0;
    int required_subgroup_size = 0;
};

// One vertex-input binding / attribute. is bufferless (zero of each);
// carries the actual descriptions (not just counts) so the worker builds the
// real VkPipelineVertexInputStateCreateInfo and the draw state machine can require the matching
// bind_vertex_buffers. stride/offset are u32 carried wide (-1 = missing).
struct VertexBindingDesc {
    int binding = -1;      // 0
    long long stride = -1; // VkVertexInputBindingDescription.stride
    int input_rate = -1;   // VkVertexInputRate (VERTEX = 0)
};
struct VertexAttributeDesc {
    int location = -1;
    int binding = -1;      // 0 (must match the single binding)
    int format = -1;       // VkFormat (subset: R32G32_SFLOAT / R8G8B8A8_UNORM)
    long long offset = -1; // VkVertexInputAttributeDescription.offset
};
// One VkVertexInputBindingDivisorDescriptionEXT (VK_EXT_vertex_attribute_divisor). zink chains a
// VkPipelineVertexInputDivisorStateCreateInfoEXT on the vertex-input pNext for instanced attributes
// (Blender/zink). The array is INDEPENDENT of vertex_bindings (a binding may or may not have a
// divisor entry). divisor carried wide (u32). divisor != 1 needs
// vertexAttributeInstanceRateDivisor; divisor == 0 needs vertexAttributeInstanceRateZeroDivisor --
// both gated on the ENABLED feature.
struct VertexBindingDivisorDesc {
    int binding = -1;       // must name an existing vertex binding
    long long divisor = -1; // u32 wide
};

// (GL/zink): one color-blend attachment's full state (zink uses real blending). Factors /
// ops / write mask are VkBlend*/VkColorComponentFlags carried wide.
struct ColorBlendAttachmentDesc {
    int blend_enable = 0;
    int src_color_factor = 0;
    int dst_color_factor = 0;
    int color_blend_op = 0;
    int src_alpha_factor = 0;
    int dst_alpha_factor = 0;
    int alpha_blend_op = 0;
    int color_write_mask = 0xF; // RGBA
};

// GraphicsPipeline. The vkcube subset (VERTEX+FRAGMENT, TRIANGLE_LIST, FILL, samples 1, no blend,
// dynamic VIEWPORT+SCISSOR) is widened to FAITHFUL for (GL/zink): the full rasterization /
// multisample / colour-blend / depth-stencil state is carried verbatim and the host driver gates
// it. The depth-determining state keeps its hardened cross-check (a depth-test pipeline needs a
// depth render pass). pipelineCache is carried but the worker treats it as a no-op.
struct CreateGraphicsPipelinesRequest {
    std::uint64_t device = 0;
    std::uint64_t pipeline_cache = 0; // carried but a worker no-op
    std::vector<ShaderStageDesc> stages;
    int topology = -1;               // TRIANGLE_LIST
    int vertex_binding_count = -1;   // 0 (== vertex_bindings.size)
    int vertex_attribute_count = -1; // 0 (== vertex_attributes.size)
    // Vertex input: the actual bounded binding/attribute descriptions (empty for bufferless
    // pipelines). The
    // counts above stay as the header count the worker cross-checks against these arrays' sizes.
    std::vector<VertexBindingDesc> vertex_bindings;
    std::vector<VertexAttributeDesc> vertex_attributes;
    // VK_EXT_vertex_attribute_divisor: the VkPipelineVertexInputDivisorStateCreateInfoEXT chained
    // on the vertex-input state. present == 0 -> absent (byte-identical to today; the worker omits
    // the pNext). When 1, the worker rebuilds the pNext from vertex_binding_divisors. An omitting
    // (vkcube/canary) peer leaves present == 0 and round-trips unchanged.
    int vertex_divisor_present = 0;
    std::vector<VertexBindingDivisorDesc> vertex_binding_divisors;
    int cull_mode = -1;              // VkCullModeFlags (NONE/BACK/FRONT)
    int front_face = -1;             // VkFrontFace (CW/CCW)
    std::vector<int> dynamic_states; // {VIEWPORT, SCISSOR}
    std::uint64_t layout = 0;
    std::uint64_t render_pass = 0;
    int subpass = -1; // 0
    // (native lane): VK_KHR_dynamic_rendering. A dynamic-rendering pipeline is
    // created with render_pass == 0 (VK_NULL_HANDLE) and instead declares the attachment FORMATS it
    // renders to via a VkPipelineRenderingCreateInfo the backend rebuilds on the create-info pNext
    // (the NVIDIA driver segfaults on a NULL-renderpass pipeline with no rendering info --
    // forwarding is mandatory, not optional). has_dynamic_rendering == 0 is every non-DR pipeline
    // (default lane
    // + native render-pass pipelines) -- byte-identical to today. When 1: render_pass MUST be 0
    // (the ICD + backends reject render_pass!=0 with DR info -- no ambiguous mixed mode), and the
    // FORMATS become the pipeline's draw-time compatibility key against the active
    // dynamic-rendering scope (replacing the render-pass-derived key). dr_view_mask is carried for
    // faithfulness, but the implementation fail-closes multiview (0 only). Formats are VkFormat
    // wide (0 =
    // UNDEFINED = "no attachment at this index" for a color slot / "no depth|stencil").
    int has_dynamic_rendering = 0;
    int dr_view_mask = 0;
    std::vector<int> dr_color_formats;
    int dr_depth_format = 0;
    int dr_stencil_format = 0;
    // Depth-stencil state. has_depth_stencil == false is the
    // shape (no VkPipelineDepthStencilStateCreateInfo). When true: depth test/write are 0/1 and
    // depth_compare_op is the compare op; no stencil, no depth-bounds (the worker fixes those off).
    // A pipeline with depth-stencil state must target a render pass that has a depth attachment.
    bool has_depth_stencil = false;
    int depth_test_enable = -1;  // VkBool32 (0/1)
    int depth_write_enable = -1; // VkBool32 (0/1)
    int depth_compare_op = -1;   // VkCompareOp (LESS_OR_EQUAL)
    // (GL/zink): the rest of the faithful pipeline state. Defaults are the
    // vkcube-equivalent values so an omitting (vkcube/canary) peer round-trips unchanged.
    int polygon_mode = 0;              // VkPolygonMode (FILL = 0)
    int depth_clamp_enable = 0;        // VkBool32
    int rasterizer_discard_enable = 0; // VkBool32
    int depth_bias_enable = 0;         // VkBool32
    double depth_bias_constant = 0.0;
    double depth_bias_clamp = 0.0;
    double depth_bias_slope = 0.0;
    double line_width = 1.0;
    // (GL/zink) RENDER CORRECTNESS: the VkPipelineRasterizationDepthClipStateCreateInfoEXT
    // (VK_EXT_depth_clip_enable) chained on the rasterization pNext. present == 0 -> the struct was
    // absent (the worker omits the pNext -> Vulkan's default near/far clipping). When present, the
    // worker rebuilds the pNext with depth_clip_enable so the real pipeline clips as zink intends
    // (without it OpenSCAD's CSG preview renders black). An omitting (vkcube/canary) peer leaves
    // present == 0 and round-trips unchanged.
    int depth_clip_state_present = 0;
    int depth_clip_enable = 1; // VkBool32; the Vulkan/GL default is clipping ENABLED
    // (GL/zink) RENDER CORRECTNESS: the VkPipelineRasterizationLineStateCreateInfoEXT
    // (VK_EXT_line_rasterization) chained on the rasterization pNext. present == 0 -> absent (no
    // pNext rebuilt -> default line rasterization). The STIPPLED fields are carried but the relay
    // masks the stippled feature off (no line-stipple dynamic command), so zink keeps stipple
    // disabled. An omitting (vkcube/canary) peer leaves present == 0 and round-trips unchanged.
    int line_state_present = 0;
    int line_rasterization_mode = 0; // VkLineRasterizationModeEXT (0 = DEFAULT)
    int line_stipple_enable = 0;     // VkBool32 (masked off via the feature report)
    int line_stipple_factor = 1;
    int line_stipple_pattern = 0; // uint16 stipple bits
    // Geometry-stream state: the VkPipelineRasterizationStateStreamCreateInfoEXT
    // (VK_EXT_transform_feedback) chained on the rasterization pNext. present == 0 -> absent (no
    // pNext rebuilt; an omitting peer round-trips unchanged). rasterization_stream is a WIDE
    // scalar (u32 rides long long, the no-narrowing wire convention); the shared
    // rasterization_stream_ok validates the range + the feature/property VUID gates identically at
    // the ICD, the worker, and the mock BEFORE any host call. The struct's `flags` is reserved
    // (must be 0) and is NOT carried -- graphics_pipeline_ok rejects a nonzero flags fail-closed.
    int stream_state_present = 0;
    long long rasterization_stream = 0;
    int primitive_restart_enable = 0; // VkBool32
    int rasterization_samples = 1;    // VkSampleCountFlagBits
    int sample_shading_enable = 0;
    long long sample_mask = -1; // VkSampleMask[0] for <=32 samples (-1 = pSampleMask null)
    double min_sample_shading = 0.0;
    int alpha_to_coverage_enable = 0;
    int alpha_to_one_enable = 0;
    int viewport_count = 1;
    int scissor_count = 1;
    int patch_control_points = 0; // tessellation (0 = none)
    int depth_bounds_test_enable = 0;
    int stencil_test_enable = 0;
    // (GL/zink) RENDER CORRECTNESS: the pipeline's STATIC stencil op state + depth bounds.
    // OpenSCAD's CSG preview is rendered by OpenCSG, a STENCIL-BUFFER algorithm: zink bakes the
    // front/back VkStencilOpState ops (fail/pass/depthFail + compareOp) into the pipeline (the
    // compare-mask / write-mask / reference are CORE dynamic state zink drives via
    // vkCmdSetStencil*, already forwarded + replayed). The relay previously honored
    // stencil_test_enable while leaving front/back ZEROED (compareOp NEVER => the stencil test
    // never passes => CSG silently renders wrong: difference shows no holes, intersection draws
    // both solids). Carry them FAITHFULLY. The defaults are an all-KEEP / compareOp ALWAYS no-op so
    // an omitting (vkcube/canary) peer with stencil disabled round-trips unchanged.
    int stencil_front_fail_op = 0;       // VkStencilOp KEEP
    int stencil_front_pass_op = 0;       // KEEP
    int stencil_front_depth_fail_op = 0; // KEEP
    int stencil_front_compare_op = 7;    // VkCompareOp ALWAYS
    int stencil_front_compare_mask = 0;  // dynamic in zink; carried for faithfulness
    int stencil_front_write_mask = 0;    // dynamic in zink
    int stencil_front_reference = 0;     // dynamic in zink
    int stencil_back_fail_op = 0;        // KEEP
    int stencil_back_pass_op = 0;        // KEEP
    int stencil_back_depth_fail_op = 0;  // KEEP
    int stencil_back_compare_op = 7;     // ALWAYS
    int stencil_back_compare_mask = 0;
    int stencil_back_write_mask = 0;
    int stencil_back_reference = 0;
    double min_depth_bounds = 0.0; // VkPipelineDepthStencilStateCreateInfo.minDepthBounds
    double max_depth_bounds = 1.0; // .maxDepthBounds (the Vulkan/GL default range)
    int logic_op_enable = 0;
    int logic_op = 0; // VkLogicOp
    double blend_constants[4] = {0.0, 0.0, 0.0, 0.0};
    std::vector<ColorBlendAttachmentDesc> color_blend_attachments; // empty = the single default

    json::Value to_body() const;
    static CreateGraphicsPipelinesRequest from_body(const json::Value& body);
};

struct CreateGraphicsPipelinesResponse {
    bool ok = false;
    std::string reason;
    std::uint64_t pipeline = 0; // pipelineCount == 1, so a single handle

    json::Value to_body() const;
    static CreateGraphicsPipelinesResponse from_body(const json::Value& body);
};

// --- Compute pipelines + dispatch ---------------------------------
// One pipeline per RPC (the graphics shape). The bounded subset: one COMPUTE stage, entry
// point carried, NO specialization constants / pNext / flags / derivatives (named ICD
// rejects). `pipeline_cache` rides for shape-symmetry with graphics but is ALWAYS 0 from the
// ICD (the app's cache is a local no-op there); backends reject non-zero.
struct CreateComputePipelinesRequest {
    std::uint64_t device = 0;
    std::uint64_t pipeline_cache = 0; // always 0 from the ICD (documented ignored)
    std::uint64_t layout = 0;         // pipeline layout handle
    std::uint64_t shader_module = 0;  // the COMPUTE-stage module
    std::string entry_point;          // carried faithfully (any name the module exports)
    // Vulkan 1.3 support (subgroupSizeControl): the compute stage's admitted
    // VkPipelineShaderStageCreateFlags + requiredSubgroupSize (0 = absent), gated on the
    // device's enabled vk13 bits at both ends (the ShaderStageDesc fields, compute spelling).
    long long stage_flags = 0;
    int required_subgroup_size = 0;

    json::Value to_body() const;
    static CreateComputePipelinesRequest from_body(const json::Value& body);
};

struct CreateComputePipelinesResponse {
    bool ok = false;
    std::string reason;
    std::uint64_t pipeline = 0;

    json::Value to_body() const;
    static CreateComputePipelinesResponse from_body(const json::Value& body);
};

// --- Host-visible memory + buffers --------------------
//
// The memory-visibility spine, proven with a mapped vertex buffer. Honest device memory (the
// worker's real VkPhysicalDeviceMemoryProperties + VkMemoryRequirements -- no fabrication, no
// remap), buffers, bind, and write_memory_ranges (the ICD's dirty shadow chunks scattered into the
// worker's real VkDeviceMemory). allocate_memory/free_memory (16/17) are reused -- the worker backs
// a real host allocation; the ICD derives coherence from the cached memory-properties table by type
// index.

// One memory type / heap from VkPhysicalDeviceMemoryProperties. All VkFlags / VkDeviceSize fields
// ride wide (decimal strings) so a 32-bit flag set or a 64-bit heap size is never truncated.
struct MemoryType {
    std::uint64_t property_flags = 0; // VkMemoryPropertyFlags
    std::uint64_t heap_index = 0;
};
struct MemoryHeap {
    std::uint64_t size = 0;  // VkDeviceSize
    std::uint64_t flags = 0; // VkMemoryHeapFlags
};

// VkMemoryPropertyFlagBits the wire reasons about by value (mock has no Vulkan headers).
constexpr std::uint64_t kMemoryPropertyDeviceLocal = 0x00000001;  // DEVICE_LOCAL_BIT
constexpr std::uint64_t kMemoryPropertyHostVisible = 0x00000002;  // HOST_VISIBLE_BIT
constexpr std::uint64_t kMemoryPropertyHostCoherent = 0x00000004; // HOST_COHERENT_BIT
constexpr std::uint64_t kMemoryPropertyProtected = 0x00000020;    // PROTECTED_BIT (fail-closed)

// VkBufferUsageFlagBits the buffer subset reasons about by value.
constexpr std::uint64_t kBufferUsageTransferSrc = 0x00000001;  // VK_BUFFER_USAGE_TRANSFER_SRC_BIT
constexpr std::uint64_t kBufferUsageVertexBuffer = 0x00000080; // VK_BUFFER_USAGE_VERTEX_BUFFER_BIT
constexpr std::uint64_t kBufferUsageUniformBuffer =
    0x00000010; // VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT
// The buffer-usage allowlist. (GL/zink) widens it from the vkcube subset to the full set of
// core buffer usages a general GL->Vulkan driver creates (index/storage/transfer-dst/indirect/texel
// buffers), so zink's buffers are accepted. Specific behaviors still key on specific bits
// (descriptor-update tests UNIFORM_BUFFER; bind_vertex_buffers tests VERTEX_BUFFER;
// copy_buffer_to_image tests TRANSFER_SRC); broadening the allowlist does not change those gates.
// The worker's real vkCreateBuffer is the ultimate validator.
constexpr std::uint64_t kBufferUsageTransferDst = 0x00000002;    // VK_BUFFER_USAGE_TRANSFER_DST_BIT
constexpr std::uint64_t kBufferUsageUniformTexel = 0x00000004;   // UNIFORM_TEXEL_BUFFER_BIT
constexpr std::uint64_t kBufferUsageStorageTexel = 0x00000008;   // STORAGE_TEXEL_BUFFER_BIT
constexpr std::uint64_t kBufferUsageStorageBuffer = 0x00000020;  // STORAGE_BUFFER_BIT
constexpr std::uint64_t kBufferUsageIndexBuffer = 0x00000040;    // INDEX_BUFFER_BIT
constexpr std::uint64_t kBufferUsageIndirectBuffer = 0x00000100; // INDIRECT_BUFFER_BIT
// ext usages (gated by features create_device now enables): conditional rendering,
// transform feedback (+ its counter), and buffer-device-address.
constexpr std::uint64_t kBufferUsageConditionalRendering = 0x00000200; // EXT_conditional_rendering
constexpr std::uint64_t kBufferUsageTransformFeedback = 0x00000800;    // EXT_transform_feedback
constexpr std::uint64_t kBufferUsageTransformFeedbackCounter = 0x00001000;
constexpr std::uint64_t kBufferUsageShaderDeviceAddress = 0x00020000; // 1.2 bufferDeviceAddress
constexpr std::uint64_t kBufferUsageSubset =
    kBufferUsageTransferSrc | kBufferUsageTransferDst | kBufferUsageUniformTexel |
    kBufferUsageStorageTexel | kBufferUsageUniformBuffer | kBufferUsageStorageBuffer |
    kBufferUsageVertexBuffer | kBufferUsageIndexBuffer | kBufferUsageIndirectBuffer |
    kBufferUsageConditionalRendering | kBufferUsageTransformFeedback |
    kBufferUsageTransformFeedbackCounter | kBufferUsageShaderDeviceAddress;

// (bufferDeviceAddress): the one VkMemoryAllocateFlagBits the relay serves.
// VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT -- required on any allocation a SHADER_DEVICE_ADDRESS
// buffer will bind to (VUID 03339). DEVICE_MASK (0x1) and DEVICE_ADDRESS_CAPTURE_REPLAY (0x4)
// stay rejected by name (multiDevice / captureReplay are reported FALSE and unwired).
constexpr std::uint64_t kMemoryAllocateDeviceAddressBit = 0x00000002;

// vkGetBufferDeviceAddress (Vulkan 1.2 bufferDeviceAddress support). The response's
// device_address is the REAL host VkDeviceAddress verbatim -- a VkDeviceAddress is raw GPU-VA
// data (not a handle), consumed by the guest's shaders which execute on the host device that
// minted it, so any translation would break it. The worker fail-closes on an unknown/unbound
// buffer, a buffer created without SHADER_DEVICE_ADDRESS usage, or a device that did not enable
// the feature. The mock (no real GPU, never runs shaders) returns a deterministic per-buffer
// non-zero token instead, so the wire + gating logic is testable.
struct GetBufferDeviceAddressRequest {
    std::uint64_t device = 0;
    std::uint64_t buffer = 0;

    json::Value to_body() const;
    static GetBufferDeviceAddressRequest from_body(const json::Value& body);
};

struct GetBufferDeviceAddressResponse {
    bool ok = false;
    std::string reason;
    std::uint64_t device_address = 0; // the real host VkDeviceAddress (mock: per-buffer token)

    json::Value to_body() const;
    static GetBufferDeviceAddressResponse from_body(const json::Value& body);
};

// --- Descriptor surface + per-frame UBO -----------------
//
// The descriptor object graph vkcube's transform/uniform path needs, on the memory
// baseline. UNIFORM_BUFFER-only; the UBO bytes use the mapped-memory tracker and flush-at-submit
// path unchanged, with no new memory mechanism. The wire constants the mock reasons about by value
// (the mock never links Vulkan; they match <vulkan_core.h>, so the real backend stays in lockstep).
constexpr int kDescriptorTypeUniformBuffer = 6; // VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
// (GL/zink): the rest of the VkDescriptorType enum the mock classifies by value (buffer vs
// image vs texel) so its faithful update path mirrors the host (values match <vulkan_core.h>).
constexpr int kDescriptorTypeUniformTexelBuffer = 4;   // VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER
constexpr int kDescriptorTypeStorageTexelBuffer = 5;   // VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER
constexpr int kDescriptorTypeStorageBuffer = 7;        // VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
constexpr int kDescriptorTypeUniformBufferDynamic = 8; // VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC
constexpr int kDescriptorTypeStorageBufferDynamic = 9; // VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC
// descriptorIndexing: the remaining VkDescriptorType values the per-type
// UPDATE_AFTER_BIND map reasons about by value.
constexpr int kDescriptorTypeSampler = 0;          // VK_DESCRIPTOR_TYPE_SAMPLER
constexpr int kDescriptorTypeSampledImage = 2;     // VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE
constexpr int kDescriptorTypeStorageImage = 3;     // VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
constexpr int kDescriptorTypeInputAttachment = 10; // VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT
// Vulkan 1.3 support (inlineUniformBlock): the promoted descriptor type (core value). Its
// descriptorCount is a BYTE size everywhere (layout binding, pool size, write), not an element
// count -- the mock's slot model and the pool budget both account it in bytes.
constexpr int kDescriptorTypeInlineUniformBlock = 1000138000;

// --- Descriptor-indexing plumbing/model wire surface ----------------------------------
//
// landed the MODEL while every descriptorIndexing capability bit stayed masked FALSE;
// unmasks the PROVEN buffer-only subset (kDIFeatureServedBits) on the native lane. The device
// carries `descriptor_indexing_feature_bits` -- the sub-features relay plumbing keys on, plus
// the two BUFFER shader non-uniform-indexing bits, carried in the scalar purely so the
// scalar/chain normalization, the host verify, and the reporting mask share one rule (they gate
// nothing relay-side: the driver enforces them at pipeline creation from the SPIR-V
// capabilities).
constexpr std::uint64_t kDIFeatureUpdateAfterBindUniformBuffer = 1ull << 0;
constexpr std::uint64_t kDIFeatureUpdateAfterBindStorageBuffer = 1ull << 1;
constexpr std::uint64_t kDIFeatureUpdateAfterBindSampledImage = 1ull << 2; // + SAMPLER + CIS
constexpr std::uint64_t kDIFeatureUpdateAfterBindStorageImage = 1ull << 3;
constexpr std::uint64_t kDIFeatureUpdateAfterBindUniformTexelBuffer = 1ull << 4;
constexpr std::uint64_t kDIFeatureUpdateAfterBindStorageTexelBuffer = 1ull << 5;
constexpr std::uint64_t kDIFeatureUpdateUnusedWhilePending = 1ull << 6;
constexpr std::uint64_t kDIFeaturePartiallyBound = 1ull << 7;
constexpr std::uint64_t kDIFeatureVariableDescriptorCount = 1ull << 8;
constexpr std::uint64_t kDIFeatureRuntimeDescriptorArray = 1ull << 9;
constexpr std::uint64_t kDIFeatureShaderUniformBufferArrayNonUniformIndexing = 1ull << 10;
constexpr std::uint64_t kDIFeatureShaderStorageBufferArrayNonUniformIndexing = 1ull << 11;
constexpr std::uint64_t kDIFeatureAllBits =
    kDIFeatureUpdateAfterBindUniformBuffer | kDIFeatureUpdateAfterBindStorageBuffer |
    kDIFeatureUpdateAfterBindSampledImage | kDIFeatureUpdateAfterBindStorageImage |
    kDIFeatureUpdateAfterBindUniformTexelBuffer | kDIFeatureUpdateAfterBindStorageTexelBuffer |
    kDIFeatureUpdateUnusedWhilePending | kDIFeaturePartiallyBound |
    kDIFeatureVariableDescriptorCount | kDIFeatureRuntimeDescriptorArray |
    kDIFeatureShaderUniformBufferArrayNonUniformIndexing |
    kDIFeatureShaderStorageBufferArrayNonUniformIndexing;
// the SERVED subset -- the bits the ICD reports (host-intersected, native lane only) and the
// CreateDevice policy at ALL THREE sites (ICD enable gate + mock + real worker:
// a deferred-but-known bit rejects everywhere, so a skewed client can never enable an unproven
// class past the ICD). Buffer-only: exactly what the real-GPU crux + the DI canary prove.
// Everything else in kDIFeatureAllBits stays reported FALSE + reject-on-enable until its own
// proof (the image/texel UAB classes and their shader bits are named follow-ons; widening this
// constant IS the unmask act, taken with a new proof); the AGGREGATE `descriptorIndexing` bit
// stays FALSE until the extension's full minimum set is served (it asserts the deferred
// image/texel classes too).
constexpr std::uint64_t kDIFeatureServedBits =
    kDIFeatureUpdateAfterBindUniformBuffer | kDIFeatureUpdateAfterBindStorageBuffer |
    kDIFeatureUpdateUnusedWhilePending | kDIFeaturePartiallyBound |
    kDIFeatureVariableDescriptorCount | kDIFeatureRuntimeDescriptorArray |
    kDIFeatureShaderUniformBufferArrayNonUniformIndexing |
    kDIFeatureShaderStorageBufferArrayNonUniformIndexing;

// --- Vulkan 1.3 support: the vk13-family feature scalar ------------------------------
//
// One wire scalar for the VkPhysicalDeviceVulkan13Features members (minus dynamicRendering and
// synchronization2, which keep their dedicated request ints) plus the
// three Vulkan-1.2 memory-model members (required in 1.3). The DI-bits model verbatim: the ICD
// folds the app's enables (the 1.3/1.2 rollups or any standalone promoted-feature struct) into
// the scalar; the worker re-derives the fold from the forwarded chain and rejects a mismatch in
// either direction BEFORE the host device exists; host support is verified per bit; and the
// CreateDevice POLICY at all three sites (ICD gate + mock + real worker) clamps to the SERVED
// set -- widening kVk13FeatureServedBits with a new proof IS the unmask act.
constexpr std::uint64_t kVk13FeatureRobustImageAccess = 1ull << 0;
constexpr std::uint64_t kVk13FeatureInlineUniformBlock = 1ull << 1;
// UNSERVED until the DI image/texel + aggregate follow-on: UAB on an inline-uniform-block
// binding is a descriptorIndexing-family capability the relay has not proven.
constexpr std::uint64_t kVk13FeatureInlineUniformBlockUpdateAfterBind = 1ull << 2;
constexpr std::uint64_t kVk13FeaturePipelineCreationCacheControl = 1ull << 3;
constexpr std::uint64_t kVk13FeaturePrivateData = 1ull << 4;
constexpr std::uint64_t kVk13FeatureShaderDemoteToHelperInvocation = 1ull << 5;
constexpr std::uint64_t kVk13FeatureShaderTerminateInvocation = 1ull << 6;
constexpr std::uint64_t kVk13FeatureSubgroupSizeControl = 1ull << 7;
constexpr std::uint64_t kVk13FeatureComputeFullSubgroups = 1ull << 8;
// UNSERVED (optional in 1.3): ASTC-HDR gates FORMAT availability the worker's image admission
// does not carry; reported FALSE + reject-on-enable, fail-closed.
constexpr std::uint64_t kVk13FeatureTextureCompressionAstcHdr = 1ull << 9;
constexpr std::uint64_t kVk13FeatureShaderZeroInitializeWorkgroupMemory = 1ull << 10;
constexpr std::uint64_t kVk13FeatureShaderIntegerDotProduct = 1ull << 11;
constexpr std::uint64_t kVk13FeatureMaintenance4 = 1ull << 12;
constexpr std::uint64_t kVk13FeatureVulkanMemoryModel = 1ull << 13;
constexpr std::uint64_t kVk13FeatureVulkanMemoryModelDeviceScope = 1ull << 14;
constexpr std::uint64_t kVk13FeatureVulkanMemoryModelAvailabilityVisibilityChains = 1ull << 15;
constexpr std::uint64_t kVk13FeatureAllBits =
    kVk13FeatureRobustImageAccess | kVk13FeatureInlineUniformBlock |
    kVk13FeatureInlineUniformBlockUpdateAfterBind | kVk13FeaturePipelineCreationCacheControl |
    kVk13FeaturePrivateData | kVk13FeatureShaderDemoteToHelperInvocation |
    kVk13FeatureShaderTerminateInvocation | kVk13FeatureSubgroupSizeControl |
    kVk13FeatureComputeFullSubgroups | kVk13FeatureTextureCompressionAstcHdr |
    kVk13FeatureShaderZeroInitializeWorkgroupMemory | kVk13FeatureShaderIntegerDotProduct |
    kVk13FeatureMaintenance4 | kVk13FeatureVulkanMemoryModel |
    kVk13FeatureVulkanMemoryModelDeviceScope |
    kVk13FeatureVulkanMemoryModelAvailabilityVisibilityChains;
constexpr std::uint64_t kVk13FeatureServedBits =
    kVk13FeatureAllBits &
    ~(kVk13FeatureInlineUniformBlockUpdateAfterBind | kVk13FeatureTextureCompressionAstcHdr);
// The 1.3-REQUIRED subset of the vk13 SCALAR (spec feature requirements): the host must report
// every one of these TRUE for the vk13 scalar's part of the vouch. This is NECESSARY but not
// SUFFICIENT for the relay to claim apiVersion 1.3 -- the full cumulative required matrix (the
// f10/f11/f12 rows + the relay-served gate below) must also hold; see host_vk13_ready() +
// kRelayServes* + DeviceCaps.vk13_ready. dynamicRendering + synchronization2 are required too but
// ride their own request ints; the availability-visibility-chains memory-model bit is
// served-but-optional.
constexpr std::uint64_t kVk13FeatureRequiredBits =
    kVk13FeatureRobustImageAccess | kVk13FeatureInlineUniformBlock |
    kVk13FeaturePipelineCreationCacheControl | kVk13FeaturePrivateData |
    kVk13FeatureShaderDemoteToHelperInvocation | kVk13FeatureShaderTerminateInvocation |
    kVk13FeatureSubgroupSizeControl | kVk13FeatureComputeFullSubgroups |
    kVk13FeatureShaderZeroInitializeWorkgroupMemory | kVk13FeatureShaderIntegerDotProduct |
    kVk13FeatureMaintenance4 | kVk13FeatureVulkanMemoryModel |
    kVk13FeatureVulkanMemoryModelDeviceScope;

// The relay-served gate for the honest apiVersion-1.3 claim (required-feature audit). ONE
// source of truth shared by the real worker (host_vk13_ready) and the mock
// (device_caps + create_device): a cumulative-required feature that the relay does not yet serve
// end-to-end keeps its flag FALSE, and while ANY is false the vouch (DeviceCaps.vk13_ready) is 0
// and the native lane honestly falls back to 1.2 -- we never report >= 1.3 while a required feature
// is advertise-then-failed. Each flag flips true only when BOTH backends serve the feature
// (structural mock/real parity). multiview is required since 1.1; hostQueryReset (device-level
// vkResetQueryPool) since 1.2.
constexpr bool kRelayServesHostQueryReset = true; // device-level vkResetQueryPool wired
// FLIPPED true -- viewMask is served through render-pass2 + dynamic rendering,
// proven per-view end-to-end on the real GPU by the multiview serve-proof canary (layer 0 red,
// layer 1 green through BOTH paths). With multiview served the FULL cumulative required matrix is
// backed, so the native lane now honestly vouches conformant Vulkan 1.3 (host_vk13_ready /
// DeviceCaps.vk13_ready -> the device reports apiVersion 1.3 on a 1.3-capable host).
constexpr bool kRelayServesMultiview = true;
constexpr bool kRelayServesFullVk13RequiredMatrix =
    kRelayServesHostQueryReset && kRelayServesMultiview;

// Required-feature audit (multiview): the array-layer count a framebuffer's attachment
// images must cover to satisfy a subpass viewMask. It is the HIGHEST SET view bit + 1 (the largest
// gl_ViewIndex a shader can address), NOT popcount(viewMask) -- a mask of 0b101 (views 0 and 2)
// needs THREE layers (0,1,2), not two, even though only two views are active. A zero mask (no
// multiview) needs one layer. ONE definition shared by the ICD, both backends, and the unit test so
// the "not popcount" arithmetic is proven once. (VkFramebufferCreateInfo VUID-02531: each
// attachment used by a non-zero-viewMask subpass must have a layerCount greater than the most
// significant set bit of the mask; the host vkCreateFramebuffer is the authoritative enforcer, this
// is the shared mock-checkable pre-check.)
inline int multiview_required_layers(int view_mask) {
    if (view_mask == 0) {
        return 1;
    }
    int highest = 0;
    const std::uint32_t m = static_cast<std::uint32_t>(view_mask);
    for (int bit = 0; bit < 32; ++bit) {
        if ((m & (1u << bit)) != 0) {
            highest = bit;
        }
    }
    return highest + 1;
}

// Shared VK_EXT_vertex_attribute_divisor content validation. ONE
// definition used by the ICD (after it extracts the VkPipelineVertexInputDivisorStateCreateInfoEXT
// into the wire arrays), the real worker (before it rebuilds the pNext), and the mock -- so all
// three fail-closed IDENTICALLY (mock == real) with a named reason before the host ever sees a
// malformed or self-contradictory divisor array. The ICD's graphics_pipeline_ok owns the STRUCTURAL
// pNext admission (is it the divisor sType, is the ext enabled); this owns the CONTENT: bounded
// ranges, binding references, no duplicates, and the ENABLED-feature value gates
// (divisor != 1 needs vertexAttributeInstanceRateDivisor; divisor == 0 additionally needs
// vertexAttributeInstanceRateZeroDivisor; host SUPPORT is not enough, the app must ENABLE it).
inline bool vertex_binding_divisors_ok(int present, const std::vector<VertexBindingDesc>& bindings,
                                       const std::vector<VertexBindingDivisorDesc>& divisors,
                                       bool ext_enabled, bool divisor_feature_enabled,
                                       bool zero_divisor_feature_enabled, std::string& reason) {
    if (present == 0) {
        if (!divisors.empty()) {
            reason = "vertex divisor array carried without the divisor-pNext present flag";
            return false;
        }
        return true;
    }
    // The divisor pNext is an EXTENSION struct: it may ride a pipeline only on a device that
    // ENABLED VK_EXT_vertex_attribute_divisor. The ICD's graphics_pipeline_ok already gates
    // admission on this, but the backends re-assert it independently so a
    // stale/custom RPC cannot slip a divisor pNext (even divisor == 1, which needs no feature) onto
    // a device without the extension.
    if (!ext_enabled) {
        reason = "vertex divisor pNext present but VK_EXT_vertex_attribute_divisor was not enabled "
                 "on the device";
        return false;
    }
    if (divisors.empty()) {
        reason = "vertex divisor pNext present but the divisor array is empty";
        return false;
    }
    if (divisors.size() > bindings.size()) {
        reason = "more vertex divisor entries than vertex bindings";
        return false;
    }
    std::set<int> seen;
    for (const VertexBindingDivisorDesc& d : divisors) {
        if (d.binding < 0 || static_cast<unsigned long long>(d.binding) > 0xFFFFFFFFull) {
            reason = "vertex divisor binding out of u32 range";
            return false;
        }
        if (d.divisor < 0 || static_cast<unsigned long long>(d.divisor) > 0xFFFFFFFFull) {
            reason = "vertex divisor value out of u32 range";
            return false;
        }
        bool names_a_binding = false;
        for (const VertexBindingDesc& b : bindings) {
            if (b.binding == d.binding) {
                names_a_binding = true;
                break;
            }
        }
        if (!names_a_binding) {
            reason = "vertex divisor names a binding with no VkVertexInputBindingDescription";
            return false;
        }
        if (!seen.insert(d.binding).second) {
            reason = "duplicate vertex divisor binding";
            return false;
        }
        if (d.divisor != 1 && !divisor_feature_enabled) {
            reason = "vertex divisor != 1 requires the enabled vertexAttributeInstanceRateDivisor "
                     "feature";
            return false;
        }
        if (d.divisor == 0 && !zero_divisor_feature_enabled) {
            reason = "vertex divisor == 0 requires the enabled "
                     "vertexAttributeInstanceRateZeroDivisor feature";
            return false;
        }
    }
    return true;
}

// Shared VkPipelineRasterizationStateStreamCreateInfoEXT content validation. ONE definition used
// by the REAL WORKER (before it rebuilds the pNext) and the MOCK --
// NOT by the ICD, which performs structural admission only (graphics_pipeline_ok's allow flag) and
// has no host stream properties client-side -- so both backends fail-closed
// IDENTICALLY
// with a named reason before the host driver ever sees an invalid stream selection (the lesson:
// invalid guest Vulkan must never become invalid host Vulkan). Encodes the struct's VUID set
// (02324-02326): the geometryStreams FEATURE must be enabled (host support is not enough), the
// stream must be < maxTransformFeedbackStreams, and a NONZERO stream additionally needs the
// transformFeedbackRasterizationStreamSelect property. `stream` is the wide wire scalar; the u32
// range check happens here, before any narrowing cast. The properties are each backend's REAL
// capabilities (the worker caches the host's at create_device; the mock models a deterministic
// device) -- policy parity comes from this one helper, not from pretending both have the same
// limits.
inline bool rasterization_stream_ok(int present, long long stream, bool ext_enabled,
                                    bool geometry_streams_enabled, std::uint32_t max_streams,
                                    bool stream_select, std::string& reason) {
    if (present == 0) {
        if (stream != 0) {
            reason = "rasterization stream carried without the stream-pNext present flag";
            return false;
        }
        return true;
    }
    if (!ext_enabled) {
        reason = "rasterization-stream pNext present but VK_EXT_transform_feedback was not "
                 "enabled on the device";
        return false;
    }
    if (!geometry_streams_enabled) {
        reason = "rasterization-stream pNext present but the geometryStreams feature was not "
                 "enabled on the device";
        return false;
    }
    if (stream < 0 || static_cast<unsigned long long>(stream) > 0xFFFFFFFFull) {
        reason = "rasterization stream out of u32 range";
        return false;
    }
    if (static_cast<unsigned long long>(stream) >= max_streams) {
        reason = "rasterization stream >= maxTransformFeedbackStreams";
        return false;
    }
    if (stream != 0 && !stream_select) {
        reason = "nonzero rasterization stream requires the "
                 "transformFeedbackRasterizationStreamSelect property";
        return false;
    }
    return true;
}

// VkDescriptorBindingFlagBits, by value (match <vulkan_core.h>).
constexpr long long kDescriptorBindingUpdateAfterBind = 0x1;
constexpr long long kDescriptorBindingUpdateUnusedWhilePending = 0x2;
constexpr long long kDescriptorBindingPartiallyBound = 0x4;
constexpr long long kDescriptorBindingVariableDescriptorCount = 0x8;
constexpr long long kDescriptorBindingKnownFlags =
    kDescriptorBindingUpdateAfterBind | kDescriptorBindingUpdateUnusedWhilePending |
    kDescriptorBindingPartiallyBound | kDescriptorBindingVariableDescriptorCount;
// VkDescriptorSetLayoutCreateFlagBits / VkDescriptorPoolCreateFlagBits, by value.
constexpr long long kDescriptorSetLayoutCreateUpdateAfterBindPool = 0x2;
constexpr long long kDescriptorPoolCreateUpdateAfterBind = 0x2;

// (The explicit per-type UPDATE_AFTER_BIND feature map lives next to the shared layout validator
// below, after every kDescriptorType* constant is defined.)
constexpr std::uint64_t kVkWholeSize = ~0ull; // VK_WHOLE_SIZE (UINT64_MAX)
// Uniform-buffer range cap: the Vulkan-guaranteed *minimum* maxUniformBufferRange, so every
// descriptor range within it is valid on every conformant device without carrying the host limit.
// The cap applies to the *resolved* range, so VK_WHOLE_SIZE on a larger buffer is rejected rather
// than silently uncapped.
constexpr std::uint64_t kUniformBufferRangeCap = 16384;
// Bounds for the faithful subset (every count is checked against these before use).
// (GL/zink) widened these from the vkcube subset -- zink builds far larger pools / batches than
// vkcube's 2-binding layout. They stay sane upper bounds (a malformed huge value is still
// rejected), NOT host limits; the host driver is the authoritative gate. Kept in lockstep with the
// icd_subset.h kMax* constants by hand.
constexpr int kMaxDescriptorSetLayoutBindings = 4096; // bindings per set layout
constexpr int kMaxDescriptorCount = 65536;            // array elements per binding
constexpr int kMaxDescriptorPoolSets = 65536;         // a pool's maxSets ceiling
constexpr int kMaxPipelineLayoutSetLayouts = 32;      // set layouts per pipeline layout
// (GL/zink): push-constant bounds (mirror icd_subset.h kMaxPushConstant* in lockstep).
constexpr int kMaxPushConstantRanges = 8;
constexpr int kMaxPushConstantBytes = 256;
constexpr int kMaxBoundDescriptorSets = 32;      // sets bound by one bind_descriptor_sets
constexpr int kMaxAllocateDescriptorSets = 4096; // sets minted by one allocate
constexpr int kMaxDescriptorWrites = 4096;       // writes in one update batch
// Vulkan 1.3 support (inlineUniformBlock): an IUB binding's descriptorCount is a BYTE size and the
// mock models one slot per byte, so it gets its own (much lower) sane bound. The real ceiling is
// the host's maxInlineUniformBlockSize limit -- typically 256 on real hosts; this stays a bound,
// not a host limit.
constexpr int kMaxInlineUniformBlockBytes = 4096; // byte size per inline-uniform-block binding

// --- Textures + depth = literal vkcube ------------------
//
// Images (texture + depth), image views over app images, and a device-local (never-mapped) memory
// class complement coherent host-visible memory. The wire constants the mock reasons about by value
// (the mock never links Vulkan; they match <vulkan_core.h>, so the real backend stays in lockstep).
// The combined-image-sampler descriptor type, sampler, and texture-upload commands share this image
// and memory spine.
constexpr int kImageType2D = 1;               // VK_IMAGE_TYPE_2D
constexpr int kFormatR8G8B8A8Unorm = 37;      // VK_FORMAT_R8G8B8A8_UNORM (vkcube's texture format)
constexpr int kFormatD16Unorm = 124;          // VK_FORMAT_D16_UNORM (vkcube's depth format)
constexpr int kImageTilingOptimal = 0;        // VK_IMAGE_TILING_OPTIMAL
constexpr int kImageTilingLinear = 1;         // VK_IMAGE_TILING_LINEAR
constexpr int kImageAspectColor = 1;          // VK_IMAGE_ASPECT_COLOR_BIT
constexpr int kImageAspectDepth = 2;          // VK_IMAGE_ASPECT_DEPTH_BIT
constexpr int kImageLayoutUndefinedC3 = 0;    // VK_IMAGE_LAYOUT_UNDEFINED
constexpr int kImageLayoutPreinitialized = 8; // VK_IMAGE_LAYOUT_PREINITIALIZED
constexpr int kImageLayoutDepthStencilAttachmentOptimal = 3; // VK_IMAGE_LAYOUT_*_OPTIMAL
constexpr int kCompareOpLessOrEqual = 3; // VK_COMPARE_OP_LESS_OR_EQUAL (vkcube's depth test)
// VkImageUsageFlagBits the image subset reasons about by value (kImageUsageTransferDst is already
// defined above for the swapchain usage; reuse it here).
constexpr std::uint64_t kImageUsageSampled = 0x00000004;                // SAMPLED_BIT
constexpr std::uint64_t kImageUsageDepthStencilAttachment = 0x00000020; // DEPTH_STENCIL_ATTACHMENT
// An image's usage must be a nonzero subset of these (a strict allowlist: SAMPLED texture,
// TRANSFER_SRC/DST staging endpoints, DEPTH_STENCIL_ATTACHMENT depth buffer, COLOR_ATTACHMENT
// render target (MRT -- the minimal widening so mock unit tests can express render targets); no
// storage/transient/input). TRANSFER_SRC rides here so the mock can create the transfer-source
// shape its own format-properties advertise (the depth upload/readback round-trip). The full
// faithful-mock broadening remains separate housekeeping.
constexpr std::uint64_t kImageUsageSubset = static_cast<std::uint64_t>(kImageUsageTransferSrc) |
                                            static_cast<std::uint64_t>(kImageUsageTransferDst) |
                                            kImageUsageSampled | kImageUsageDepthStencilAttachment |
                                            static_cast<std::uint64_t>(kImageUsageColorAttachment);
// VkFormatFeatureFlagBits the format-properties path reasons about by value.
constexpr std::uint64_t kFormatFeatureSampledImage = 0x00000001;           // SAMPLED_IMAGE_BIT
constexpr std::uint64_t kFormatFeatureColorAttachment = 0x00000080;        // COLOR_ATTACHMENT_BIT
constexpr std::uint64_t kFormatFeatureDepthStencilAttachment = 0x00000200; // DEPTH_STENCIL_*
constexpr std::uint64_t kFormatFeatureTransferSrc = 0x00004000;            // TRANSFER_SRC_BIT
constexpr std::uint64_t kFormatFeatureTransferDst = 0x00008000;            // TRANSFER_DST_BIT

// VK_IMAGE_ASPECT_STENCIL_BIT (only COLOR=1 / DEPTH=2 existed before depth/stencil copy support).
constexpr int kImageAspectStencil = 4;
// VkFormat enum values for the depth/stencil formats the faithful copy + create paths reason about
// (D16 already has a constant above). These are the same 124-130 values the worker's create_image
// keys on -- named here so the shared aspect table below is self-documenting.
constexpr int kFormatX8D24UnormPack32 = 125; // VK_FORMAT_X8_D24_UNORM_PACK32 (depth-only)
constexpr int kFormatD32Sfloat = 126;        // VK_FORMAT_D32_SFLOAT (depth-only)
constexpr int kFormatS8Uint = 127;           // VK_FORMAT_S8_UINT (stencil-only)
constexpr int kFormatD16UnormS8Uint = 128;   // VK_FORMAT_D16_UNORM_S8_UINT (depth+stencil)
constexpr int kFormatD24UnormS8Uint = 129;   // VK_FORMAT_D24_UNORM_S8_UINT (depth+stencil)
constexpr int kFormatD32SfloatS8Uint = 130;  // VK_FORMAT_D32_SFLOAT_S8_UINT (depth+stencil)

// The ONE format->aspect table (single source of truth), keyed by VkFormat enum value: a depth
// format sets DEPTH, a stencil format sets STENCIL, a combined depth+stencil format sets both, and
// everything else is COLOR. The returned bits equal the VK_IMAGE_ASPECT_* values (COLOR=1, DEPTH=2,
// STENCIL=4), so the real worker (VkImageAspectFlags) and the mock (kImageAspect*) agree
// bit-for-bit. Callers must pick the PRECISE predicate (has_depth_aspect / has_stencil_aspect)
// instead of testing
// "!= COLOR" -- a depth-attachment path must not admit stencil-only S8_UINT as "depth".
inline int format_aspect_mask(int f) {
    int a = 0;
    if (f == kFormatD16Unorm || f == kFormatX8D24UnormPack32 || f == kFormatD32Sfloat ||
        f == kFormatD16UnormS8Uint || f == kFormatD24UnormS8Uint || f == kFormatD32SfloatS8Uint) {
        a |= kImageAspectDepth;
    }
    if (f == kFormatS8Uint || f == kFormatD16UnormS8Uint || f == kFormatD24UnormS8Uint ||
        f == kFormatD32SfloatS8Uint) {
        a |= kImageAspectStencil;
    }
    return a != 0 ? a : kImageAspectColor;
}
inline bool has_depth_aspect(int f) {
    return (format_aspect_mask(f) & kImageAspectDepth) != 0;
}
inline bool has_stencil_aspect(int f) {
    return (format_aspect_mask(f) & kImageAspectStencil) != 0;
}
inline bool is_depth_stencil_format(int f) {
    return (format_aspect_mask(f) & (kImageAspectDepth | kImageAspectStencil)) != 0;
}
// A conservative texel size (bytes) for the mock's memory-requirements estimate -- the mock does
// not need exact driver sizing, only to avoid UNDER-sizing an obvious format. Upper bounds: S8=1,
// D16=2, D32_S8=8, and 4 for D32 / X8_D24 / D24_S8 / D16_S8 (padded) and every color format.
inline std::uint64_t format_mock_texel_bytes(int f) {
    switch (f) {
    case kFormatS8Uint:
        return 1;
    case kFormatD16Unorm:
        return 2;
    case kFormatD32SfloatS8Uint:
        return 8;
    default:
        return 4;
    }
}

// --- Sampler + combined-image-sampler + texture upload
// ----------------
//
// The sampled texture needs a sampler plus a combined-image-sampler descriptor, along with the
// copy_buffer_to_image and generalized pipeline_barrier upload commands. The mock reasons about
// these wire constants by value (it never links Vulkan); they match <vulkan_core.h>, so the real
// backend stays in lockstep.
constexpr int kDescriptorTypeCombinedImageSampler = 1; // VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
// VkImageLayout values the texture path reasons about by value, covering undefined,
// preinitialized, depth/stencil attachment, transfer, and shader-read layouts.
constexpr int kImageLayoutTransferDstOptimal = 7;    // VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
constexpr int kImageLayoutShaderReadOnlyOptimal = 5; // VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
constexpr int kImageLayoutGeneral = 1;               // VK_IMAGE_LAYOUT_GENERAL
// (The staging buffer's TRANSFER_SRC usage bit -- kBufferUsageTransferSrc -- lives in the
// buffer-usage block above, where the buffer-usage allowlist is defined.) The bounded
// sampler subset (vkcube's exact sampler): VkFilter NEAREST, VkSamplerMipmapMode NEAREST,
// VkSamplerAddressMode CLAMP_TO_EDGE, anisotropy/compare disabled. The worker fixes the rest (LOD
// 0, border color, unnormalized FALSE) and validates these carried fields.
constexpr int kFilterNearest = 0;                 // VK_FILTER_NEAREST
constexpr int kSamplerMipmapModeNearest = 0;      // VK_SAMPLER_MIPMAP_MODE_NEAREST
constexpr int kSamplerAddressModeClampToEdge = 2; // VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE

// Validates an APP-image (texture) pipeline_barrier against the texture-upload allowlist,
// shared by the mock + real worker so they stay in lockstep. `usage` is the image's
// VkImageUsageFlags. The transition must be one of:
//   UNDEFINED            -> TRANSFER_DST_OPTIMAL       (needs TRANSFER_DST usage; staging upload)
//   TRANSFER_DST_OPTIMAL -> SHADER_READ_ONLY_OPTIMAL   (needs SAMPLED; staging path finalize)
//   PREINITIALIZED       -> SHADER_READ_ONLY_OPTIMAL   (needs SAMPLED; linear path finalize)
// and the subresource range must be EXPLICITLY the single subresource the image subset supports --
// exactly {mip 0, level 1, layer 0, count 1} (app-image barriers must carry the
// range honestly, no missing-range shorthand -- the ICD's CmdPipelineBarrier always carries it; the
// legacy whole-subresource shorthand survives only for swapchain-color barriers, which never reach
// this helper). Queue-family indices are not carried and are ignored. On false sets `reason`.
inline bool app_image_barrier_ok(const RecordedCommand& c, std::uint64_t usage,
                                 std::string& reason) {
    // (GL/zink): FAITHFUL barrier forwarding. zink is a trusted producer that emits the
    // full range of image transitions with honest subresource ranges -- e.g.
    // COLOR_ATTACHMENT_OPTIMAL <-> TRANSFER_SRC_OPTIMAL for an offscreen glReadPixels readback,
    // SHADER_READ_ONLY for sampling, PRESENT_SRC for the swapchain. Forward them verbatim (the
    // worker emits the carried layouts / access / stages / range) and let the host driver (+ the
    // validation layer) be the barrier-correctness authority. (The texture-upload
    // allowlist + fixed single-subresource shape are lifted -- they only ever rejected legal zink
    // barriers.)
    (void) c;
    (void) usage;
    (void) reason;
    return true;
}

// Honest format properties (the ICD memoizes per (physical_device, format); the void
// vkGetPhysicalDeviceFormatProperties is then a cache copy -- a zeroed table is the fail-closed
// answer when the worker cannot provide one, which makes a probing app fall to its no-support path
// cleanly rather than mis-render). Features ride wide (VkFormatFeatureFlags is 32-bit; carried as
// JSON integers).
struct GetPhysicalDeviceFormatPropertiesRequest {
    std::uint64_t physical_device = 0;
    int format = -1; // VkFormat

    json::Value to_body() const;
    static GetPhysicalDeviceFormatPropertiesRequest from_body(const json::Value& body);
};

struct GetPhysicalDeviceFormatPropertiesResponse {
    bool ok = false;
    std::string reason;
    std::uint64_t linear_tiling_features = 0;  // VkFormatProperties.linearTilingFeatures (32-bit)
    std::uint64_t optimal_tiling_features = 0; // .optimalTilingFeatures (32-bit)
    std::uint64_t buffer_features = 0;         // .bufferFeatures (32-bit)
    // (GL/zink): the 64-bit VkFormatProperties3 feature flags (VK_KHR_format_feature_flags2
    // extension or core 1.3), queried worker-side via vkGetPhysicalDeviceFormatProperties2. zink
    // probes
    // VkFormatProperties3 on a 1.3 device; without the 64-bit fill Mesa builds ZERO GLX configs.
    // Additive (legacy responses decode these to 0 -> the 32-bit path is unaffected).
    std::uint64_t linear_tiling_features2 = 0;
    std::uint64_t optimal_tiling_features2 = 0;
    std::uint64_t buffer_features2 = 0;

    json::Value to_body() const;
    static GetPhysicalDeviceFormatPropertiesResponse from_body(const json::Value& body);
};

// (GL/zink): the real host VkPhysicalDeviceFeatures, carried as a u64 bitmask (the 55 core
// VkBool32 fields; see common/vkrpc/feature_bits.h). The ICD's vkGetPhysicalDeviceFeatures returns
// it (zink reads it to set GL caps); the vkcube subset returned an all-zero set, which zink cannot
// build a device against. Fail-closed: a failed query yields ok=false -> the ICD returns a zeroed
// set.
struct GetPhysicalDeviceFeaturesRequest {
    std::uint64_t physical_device = 0;

    json::Value to_body() const;
    static GetPhysicalDeviceFeaturesRequest from_body(const json::Value& body);
};

struct GetPhysicalDeviceFeaturesResponse {
    bool ok = false;
    std::string reason;
    std::uint64_t feature_bits = 0; // pack_physical_device_features() of the host's real features

    json::Value to_body() const;
    static GetPhysicalDeviceFeaturesResponse from_body(const json::Value& body);
};

// (GL/zink): real vkGetPhysicalDeviceImageFormatProperties forwarding. zink queries it per
// (format, type, tiling, usage) while building GLX configs; the vkcube subset always returned
// FORMAT_NOT_SUPPORTED, so no configs. `result` is the VkResult (VK_SUCCESS or
// VK_ERROR_FORMAT_NOT_SUPPORTED); the maxima are meaningful only when result == VK_SUCCESS.
struct GetPhysicalDeviceImageFormatPropertiesRequest {
    std::uint64_t physical_device = 0;
    int format = -1;         // VkFormat
    int image_type = -1;     // VkImageType
    int tiling = -1;         // VkImageTiling
    std::uint64_t usage = 0; // VkImageUsageFlags
    std::uint64_t flags = 0; // VkImageCreateFlags

    json::Value to_body() const;
    static GetPhysicalDeviceImageFormatPropertiesRequest from_body(const json::Value& body);
};

struct GetPhysicalDeviceImageFormatPropertiesResponse {
    bool ok = false;
    std::string reason;
    int result = 0; // VkResult (0 == VK_SUCCESS; negative == error, e.g. FORMAT_NOT_SUPPORTED)
    std::uint32_t max_extent_width = 0;
    std::uint32_t max_extent_height = 0;
    std::uint32_t max_extent_depth = 0;
    std::uint32_t max_mip_levels = 0;
    std::uint32_t max_array_layers = 0;
    std::uint32_t sample_counts = 0;     // VkSampleCountFlags
    std::uint64_t max_resource_size = 0; // VkDeviceSize

    json::Value to_body() const;
    static GetPhysicalDeviceImageFormatPropertiesResponse from_body(const json::Value& body);
};

// (GL/zink): the FULL host VkPhysicalDeviceProperties forwarded verbatim as a byte blob
// (props_blob == raw struct bytes, identical layout on both platforms; carried hex-encoded in
// JSON). The ICD memcpy's it when the size matches, else keeps its synthesized minimal properties.
// The mock (no vulkan.h) returns ok=true + an EMPTY blob -> the ICD falls back gracefully.
struct GetPhysicalDevicePropertiesRequest {
    std::uint64_t physical_device = 0;

    json::Value to_body() const;
    static GetPhysicalDevicePropertiesRequest from_body(const json::Value& body);
};

struct GetPhysicalDevicePropertiesResponse {
    bool ok = false;
    std::string reason;
    std::string props_blob; // raw VkPhysicalDeviceProperties bytes (empty from the mock)

    json::Value to_body() const;
    static GetPhysicalDevicePropertiesResponse from_body(const json::Value& body);
};

// Generic VkPhysicalDeviceFeatures2 / Properties2 pNext-chain forwarder (which: 0 = features,
// 1 = properties). The worker builds the requested structs (zeroed, sType set), chains them, calls
// the host vkGetPhysicalDevice{Features,Properties}2, and returns each struct's real bytes.
struct GetPhysicalDeviceCapabilityChainRequest {
    std::uint64_t physical_device = 0;
    std::uint32_t which = 0; // 0 = features, 1 = properties
    std::vector<CapabilityChainEntry> entries;

    json::Value to_body() const;
    static GetPhysicalDeviceCapabilityChainRequest from_body(const json::Value& body);
};

struct GetPhysicalDeviceCapabilityChainResponse {
    bool ok = false;
    std::string reason;
    std::vector<CapabilityChainEntry> entries; // s_type + filled blob, in request order

    json::Value to_body() const;
    static GetPhysicalDeviceCapabilityChainResponse from_body(const json::Value& body);
};

// Image. subset: 2D, single mip/layer, samples 1, tiling OPTIMAL|LINEAR, usage a nonzero
// subset of kImageUsageSubset, sharingMode EXCLUSIVE, initialLayout UNDEFINED|PREINITIALIZED,
// positive extent, depth 1. Combined-image-sampler descriptors, samplers, and upload commands use
// this same image object.
struct CreateImageRequest {
    std::uint64_t device = 0;
    int image_type = -1; // VkImageType (2D)
    int format = -1;     // VkFormat
    int width = -1;
    int height = -1;
    int depth = -1;        // VkExtent3D.depth (1)
    int mip_levels = -1;   // 1
    int array_layers = -1; // 1
    int samples = -1;      // VkSampleCountFlagBits (1)
    int tiling = -1;       // VkImageTiling
    std::uint64_t usage = 0;
    int sharing_mode = -1;   // VkSharingMode (EXCLUSIVE = 0)
    int initial_layout = -1; // VkImageLayout (UNDEFINED | PREINITIALIZED)
    // (GL/zink): VkImageCreateFlags (e.g. MUTABLE_FORMAT) + the
    // VkImageFormatListCreateInfo view-format list (mutable-format aliasing). The worker rebuilds
    // the pNext when view_formats is non-empty. zink creates its render-target images
    // mutable-format.
    long long image_flags = 0;
    std::vector<int> view_formats; // VkFormat list

    json::Value to_body() const;
    static CreateImageRequest from_body(const json::Value& body);
};

// The response carries the real VkMemoryRequirements (so the void vkGetImageMemoryRequirements is a
// pure cache copy -- the create_buffer pattern) and, for LINEAR tiling, the real
// VkSubresourceLayout (so the void vkGetImageSubresourceLayout the linear texture path needs is
// honest). has_subresource_layout is false for OPTIMAL tiling (querying it is invalid there).
struct CreateImageResponse {
    bool ok = false;
    std::string reason;
    std::uint64_t image = 0;
    std::uint64_t mem_size = 0;      // VkMemoryRequirements.size
    std::uint64_t mem_alignment = 0; // .alignment
    std::uint64_t mem_type_bits = 0; // .memoryTypeBits
    bool has_subresource_layout = false;
    std::uint64_t sr_offset = 0;    // VkSubresourceLayout.offset (LINEAR only)
    std::uint64_t sr_size = 0;      // .size
    std::uint64_t sr_row_pitch = 0; // .rowPitch

    json::Value to_body() const;
    static CreateImageResponse from_body(const json::Value& body);
};

struct BindImageMemoryRequest {
    std::uint64_t image = 0;
    std::uint64_t memory = 0;
    std::uint64_t memory_offset = 0; // VkDeviceSize

    json::Value to_body() const;
    static BindImageMemoryRequest from_body(const json::Value& body);
};

// Sampler support. The bounded sampler subset vkcube's combined-image-sampler
// binds: NEAREST mag/min filter, NEAREST mipmap mode, CLAMP_TO_EDGE on all three axes, anisotropy +
// compare disabled. The worker fixes the rest of VkSamplerCreateInfo (LOD 0..0, border color,
// unnormalizedCoordinates FALSE) and validates these carried fields; the ICD rejects anything
// outside the subset before the wire. A sampler is a device child (blocks the device's destroy
// until freed).
struct CreateSamplerRequest {
    std::uint64_t device = 0;
    int mag_filter = -1;        // VkFilter
    int min_filter = -1;        // VkFilter
    int mipmap_mode = -1;       // VkSamplerMipmapMode
    int address_mode_u = -1;    // VkSamplerAddressMode
    int address_mode_v = -1;    // VkSamplerAddressMode
    int address_mode_w = -1;    // VkSamplerAddressMode
    int anisotropy_enable = -1; // VkBool32
    int compare_enable = -1;    // VkBool32
    // (GL/zink): the remaining VkSamplerCreateInfo fields, so a general GL sampler (LINEAR,
    // repeat, mipmapping, anisotropy, compare, LOD range, border color) is forwarded faithfully.
    double mip_lod_bias = 0.0;
    double max_anisotropy = 1.0;
    int compare_op = 0; // VkCompareOp (used only when compare_enable)
    double min_lod = 0.0;
    double max_lod = 0.0;
    int border_color = 0;             // VkBorderColor
    int unnormalized_coordinates = 0; // VkBool32

    json::Value to_body() const;
    static CreateSamplerRequest from_body(const json::Value& body);
};

struct CreateSamplerResponse {
    bool ok = false;
    std::string reason;
    std::uint64_t sampler = 0;

    json::Value to_body() const;
    static CreateSamplerResponse from_body(const json::Value& body);
};

// Query pools (GL 3.3 / occlusion / xfb queries). A faithful VkQueryPoolCreateInfo subset: the
// query type + count (+ pipelineStatistics flags for the stats type). No pNext / flags in the
// subset (VkQueryPoolCreateInfo.flags is reserved). The worker mints the real VkQueryPool.
struct CreateQueryPoolRequest {
    std::uint64_t device = 0;
    int query_type = -1;                   // VkQueryType
    std::uint32_t query_count = 0;         // number of queries in the pool
    std::uint64_t pipeline_statistics = 0; // VkQueryPipelineStatisticFlags (stats type only)

    json::Value to_body() const;
    static CreateQueryPoolRequest from_body(const json::Value& body);
};

struct CreateQueryPoolResponse {
    bool ok = false;
    std::string reason;
    std::uint64_t query_pool = 0;

    json::Value to_body() const;
    static CreateQueryPoolResponse from_body(const json::Value& body);
};

// Synchronous query-results readback (vkGetQueryPoolResults). The worker calls the real getter and
// ships the raw bytes + the real VkResult (so VK_NOT_READY without WAIT is faithful). data_size is
// the app's requested buffer size; the worker fills up to that. flags carries 64/WAIT/PARTIAL/
// WITH_AVAILABILITY exactly as the app passed them.
struct GetQueryPoolResultsRequest {
    std::uint64_t device = 0;
    std::uint64_t query_pool = 0;
    std::uint32_t first_query = 0;
    std::uint32_t query_count = 0;
    std::uint64_t data_size = 0; // bytes the app provided (the worker fills up to this)
    std::uint64_t stride = 0;    // VkDeviceSize between per-query results
    std::uint64_t flags = 0;     // VkQueryResultFlags (64 / WAIT / PARTIAL / WITH_AVAILABILITY)

    json::Value to_body() const;
    static GetQueryPoolResultsRequest from_body(const json::Value& body);
};

struct GetQueryPoolResultsResponse {
    bool ok = false; // false = a relay/handle error (bad pool); true = the getter ran
    std::string reason;
    int vk_result = 0; // the real VkResult (SUCCESS / NOT_READY) when ok
    std::string data;  // the raw result bytes (<= data_size)

    json::Value to_body() const;
    static GetQueryPoolResultsResponse from_body(const json::Value& body);
};

// Device-level vkResetQueryPool (hostQueryReset -- required since Vulkan 1.2). Resets a
// [first_query, first_query+query_count) range of a pool FROM THE HOST. Reuses StatusResponse (the
// API returns void). Range validation is mock == real (out-of-range fails closed).
struct ResetQueryPoolRequest {
    std::uint64_t device = 0;
    std::uint64_t query_pool = 0;
    std::uint32_t first_query = 0;
    std::uint32_t query_count = 0;

    json::Value to_body() const;
    static ResetQueryPoolRequest from_body(const json::Value& body);
};

struct GetPhysicalDeviceMemoryPropertiesRequest {
    std::uint64_t physical_device = 0;

    json::Value to_body() const;
    static GetPhysicalDeviceMemoryPropertiesRequest from_body(const json::Value& body);
};

// The honest host table (the ICD caches it at enumerate-time; the void vkGetPhysicalDeviceMemory
// Properties is a pure cache copy). An empty table is the fail-closed answer
// when the worker cannot provide one.
struct GetPhysicalDeviceMemoryPropertiesResponse {
    bool ok = false;
    std::string reason;
    std::vector<MemoryType> types;
    std::vector<MemoryHeap> heaps;

    json::Value to_body() const;
    static GetPhysicalDeviceMemoryPropertiesResponse from_body(const json::Value& body);
};

// Buffer. subset: usage == VERTEX_BUFFER, sharingMode EXCLUSIVE, size > 0, no flags/pNext.
struct CreateBufferRequest {
    std::uint64_t device = 0;
    std::uint64_t size = 0;  // VkDeviceSize, > 0
    std::uint64_t usage = 0; // VkBufferUsageFlags (VERTEX_BUFFER)
    int sharing_mode = -1;   // VkSharingMode (EXCLUSIVE = 0)

    json::Value to_body() const;
    static CreateBufferRequest from_body(const json::Value& body);
};

// The response carries the real VkMemoryRequirements the worker reads right after vkCreateBuffer,
// so the void vkGetBufferMemoryRequirements is a pure cache copy. size/alignment
// are VkDeviceSize; memory_type_bits is a VkFlags -- all wide.
struct CreateBufferResponse {
    bool ok = false;
    std::string reason;
    std::uint64_t buffer = 0;
    std::uint64_t mem_size = 0;      // VkMemoryRequirements.size
    std::uint64_t mem_alignment = 0; // .alignment
    std::uint64_t mem_type_bits = 0; // .memoryTypeBits

    json::Value to_body() const;
    static CreateBufferResponse from_body(const json::Value& body);
};

struct BindBufferMemoryRequest {
    std::uint64_t buffer = 0;
    std::uint64_t memory = 0;
    std::uint64_t memory_offset = 0; // VkDeviceSize

    json::Value to_body() const;
    static BindBufferMemoryRequest from_body(const json::Value& body);
};

// --- write_memory_ranges (the only BINARY-bodied op) ------------------
//
// Scatters the ICD's dirty shadow chunks into the worker's real VkDeviceMemory. Wire body is
// [u32 json_len LE][json header][raw bytes]; the header is {uploads:[{memory, ranges:[{offset,
// size}]}]}, the tail is the bytes in (upload, range) order. Request/response (a failed upload
// fails vkQueueSubmit before any child submit); response is StatusResponse.

// One {offset, size} byte run within one allocation.
struct MemoryUploadRange {
    std::uint64_t offset = 0;
    std::uint64_t size = 0;
};
// One allocation's contribution. Ranges are sorted ascending + disjoint (producer guarantee,
// decoder enforced).
struct MemoryUpload {
    std::uint64_t memory = 0;
    std::vector<MemoryUploadRange> ranges;
};

// Cap on a single write_memory_ranges raw payload, kept below the 16 MiB transport frame cap with
// headroom for the RPC header + the u32 json_len + the JSON header. The decoder
// also checks the FULL encoded frame against protocol::kMaxFrameBytes; a producer splits a larger
// sweep across frames.
constexpr std::size_t kMaxMemoryUploadBytes = 8u * 1024u * 1024u; // 8 MiB

struct WriteMemoryRangesRequest {
    std::vector<MemoryUpload> uploads;
    std::string payload; // raw bytes in (upload, range) order; length == sum of all range sizes

    // [u32 json_len LE][json header][raw bytes]. Total decoder: framing faults, json over cap, u64
    // offset+size overflow, ranges unsorted/overlapping per allocation, a duplicate memory handle,
    // payload length != sum(sizes), or an encoded frame over kMaxFrameBytes all set `err` and
    // return a default request (caller replies BadRequest). Never OOB-reads.
    std::string to_wire() const;
    static WriteMemoryRangesRequest from_wire(const std::string& body, std::string& err);
};

// Host-visible memory readback: the ranges to pull from the worker (mirror of WriteMemoryRanges,
// without payload -- the response carries the bytes). Plain JSON (the range set is small).
struct ReadMemoryRangesRequest {
    std::vector<MemoryUpload> reads; // reuse {memory, ranges}; no payload
    // ask for the raw binary response (below). Additive: absent/false -> the
    // worker replies JSON+hex, so an old peer on either side degrades to the legacy path.
    // The client sets this only when DeviceCaps.raw_readback was advertised.
    bool raw_response = false;

    json::Value to_body() const;
    static ReadMemoryRangesRequest from_body(const json::Value& body);
};

struct ReadMemoryRangesResponse {
    bool ok = false;
    std::string reason;
    std::string payload; // raw bytes in (read, range) order; length == sum of all range sizes

    json::Value to_body() const;
    static ReadMemoryRangesResponse from_body(const json::Value& body);
    // the raw binary response ([u32 json_len][{ok,reason} header][raw payload]
    // -- the WriteMemoryRangesRequest::to_wire framing in the response direction). A 4 MB read
    // was costing ~149 ms end to end, ~all of it hex-encode + 8 MB JSON dump/parse/decode;
    // raw framing removes every byte of that. from_wire fails closed (err set, ok=false) on a
    // short body, an over-cap or overrunning header, or a header parse error.
    std::string to_wire() const;
    static ReadMemoryRangesResponse from_wire(const std::string& body, std::string& err);
};

// --- Descriptor surface serialization -----------------
//
// All JSON. Every count is validated against the bounds before use; the descriptor type
// is validated == UNIFORM_BUFFER (the only type in the subset).

// One descriptor set layout binding. type/count/stageFlags decode wide (-1 = missing).
// descriptor_type == UNIFORM_BUFFER, descriptor_count >= 1, stage_flags a nonzero subset of
// {VERTEX, FRAGMENT}.
struct DescriptorSetLayoutBindingDesc {
    int binding = -1;
    int descriptor_type = -1; // VkDescriptorType (any host-supported type)
    int descriptor_count = -1;
    long long stage_flags = -1;  // VkShaderStageFlags (carried wide; any stages)
    long long binding_flags = 0; // VkDescriptorBindingFlags (GL/zink: per-binding, carried wide)
};

// The EXPLICIT per-type UPDATE_AFTER_BIND feature map (following the VUIDs):
// returns the kDIFeature* bit a binding of `descriptor_type` needs for UPDATE_AFTER_BIND, or 0
// when UPDATE_AFTER_BIND is NOT admissible for that type (INPUT_ATTACHMENT, the DYNAMIC buffer
// types, and every type without its own wired support -- acceleration structures, newer types,
// and (Vulkan 1.3 support) inline uniform blocks, whose UAB feature
// kVk13FeatureInlineUniformBlockUpdateAfterBind stays outside the served set -- stay fail-closed).
inline std::uint64_t di_update_after_bind_feature_for_type(int descriptor_type) {
    switch (descriptor_type) {
    case kDescriptorTypeUniformBuffer:
        return kDIFeatureUpdateAfterBindUniformBuffer;
    case kDescriptorTypeStorageBuffer:
        return kDIFeatureUpdateAfterBindStorageBuffer;
    case kDescriptorTypeSampler:
    case kDescriptorTypeCombinedImageSampler:
    case kDescriptorTypeSampledImage:
        return kDIFeatureUpdateAfterBindSampledImage;
    case kDescriptorTypeStorageImage:
        return kDIFeatureUpdateAfterBindStorageImage;
    case kDescriptorTypeUniformTexelBuffer:
        return kDIFeatureUpdateAfterBindUniformTexelBuffer;
    case kDescriptorTypeStorageTexelBuffer:
        return kDIFeatureUpdateAfterBindStorageTexelBuffer;
    default:
        return 0;
    }
}

// descriptorIndexing: the SHARED per-binding-flag admission -- ONE implementation of
// the VUID map called by the ICD entry AND both backends (mock == real by construction). Every
// BINDING flag is gated on the device's enabled kDIFeature* bit. While the capability remains
// masked, the bits are 0 on every conformant device, so any DI-flagged layout fails closed here.
// Rules: unknown flag bits reject by name; UPDATE_AFTER_BIND needs the per-type feature (map
// above), the layout's UPDATE_AFTER_BIND_POOL flag, and never a DYNAMIC/INPUT_ATTACHMENT/unmapped
// type; UPDATE_UNUSED_WHILE_PENDING / PARTIALLY_BOUND need their features;
// VARIABLE_DESCRIPTOR_COUNT needs its feature, only on the HIGHEST-numbered binding, never on a
// DYNAMIC type. The layout UPDATE_AFTER_BIND_POOL flag itself is a CONTAINER flag (limit-bucket
// selection) -- valid with no UAB binding and no feature. The DYNAMIC-type ban
// is conditioned on an ACTUAL UPDATE_AFTER_BIND binding, not the container flag
// (VUID-...-descriptorType-03001: once ANY binding carries UPDATE_AFTER_BIND, no binding in the
// layout -- flagged or not -- may be a dynamic buffer descriptor).
inline bool descriptor_indexing_layout_ok(long long layout_flags,
                                          const std::vector<DescriptorSetLayoutBindingDesc>& binds,
                                          std::uint64_t di_bits, const char** reason) {
    const bool layout_uab_pool =
        (layout_flags & kDescriptorSetLayoutCreateUpdateAfterBindPool) != 0;
    int highest_binding = -1;
    bool any_uab_binding = false;
    bool any_dynamic_binding = false;
    for (const DescriptorSetLayoutBindingDesc& b : binds) {
        highest_binding = b.binding > highest_binding ? b.binding : highest_binding;
        any_uab_binding =
            any_uab_binding || (b.binding_flags & kDescriptorBindingUpdateAfterBind) != 0;
        any_dynamic_binding = any_dynamic_binding ||
                              b.descriptor_type == kDescriptorTypeUniformBufferDynamic ||
                              b.descriptor_type == kDescriptorTypeStorageBufferDynamic;
    }
    if (any_uab_binding && any_dynamic_binding) {
        *reason = "a layout with an UPDATE_AFTER_BIND binding cannot contain DYNAMIC descriptor "
                  "types (VUID 03001)";
        return false;
    }
    for (const DescriptorSetLayoutBindingDesc& b : binds) {
        if (b.binding_flags == 0) {
            continue;
        }
        if ((b.binding_flags & ~kDescriptorBindingKnownFlags) != 0) {
            *reason = "descriptor binding flags outside the supported set";
            return false;
        }
        const bool is_dynamic = b.descriptor_type == kDescriptorTypeUniformBufferDynamic ||
                                b.descriptor_type == kDescriptorTypeStorageBufferDynamic;
        if ((b.binding_flags & kDescriptorBindingUpdateAfterBind) != 0) {
            const std::uint64_t need = di_update_after_bind_feature_for_type(b.descriptor_type);
            if (need == 0) {
                *reason = "UPDATE_AFTER_BIND not admissible for this descriptor type "
                          "(dynamic/input-attachment/unwired types fail closed)";
                return false;
            }
            if ((di_bits & need) == 0) {
                *reason = "UPDATE_AFTER_BIND requires the per-type UpdateAfterBind feature";
                return false;
            }
            if (!layout_uab_pool) {
                *reason = "UPDATE_AFTER_BIND binding requires the layout UPDATE_AFTER_BIND_POOL "
                          "flag";
                return false;
            }
        }
        if ((b.binding_flags & kDescriptorBindingUpdateUnusedWhilePending) != 0 &&
            (di_bits & kDIFeatureUpdateUnusedWhilePending) == 0) {
            *reason = "UPDATE_UNUSED_WHILE_PENDING requires its enabled feature";
            return false;
        }
        if ((b.binding_flags & kDescriptorBindingPartiallyBound) != 0 &&
            (di_bits & kDIFeaturePartiallyBound) == 0) {
            *reason = "PARTIALLY_BOUND requires its enabled feature";
            return false;
        }
        if ((b.binding_flags & kDescriptorBindingVariableDescriptorCount) != 0) {
            if ((di_bits & kDIFeatureVariableDescriptorCount) == 0) {
                *reason = "VARIABLE_DESCRIPTOR_COUNT requires its enabled feature";
                return false;
            }
            if (b.binding != highest_binding) {
                *reason = "VARIABLE_DESCRIPTOR_COUNT only on the highest-numbered binding";
                return false;
            }
            if (is_dynamic) {
                *reason = "VARIABLE_DESCRIPTOR_COUNT not admissible on a DYNAMIC descriptor type";
                return false;
            }
        }
    }
    return true;
}

// The binding a variable-count allocation resizes: the HIGHEST-numbered binding when it carries
// VARIABLE_DESCRIPTOR_COUNT (the validator above pinned it there); -1 when the layout has none.
inline int di_variable_binding(const std::vector<DescriptorSetLayoutBindingDesc>& binds) {
    int highest = -1;
    long long flags = 0;
    for (const DescriptorSetLayoutBindingDesc& b : binds) {
        if (b.binding > highest) {
            highest = b.binding;
            flags = b.binding_flags;
        }
    }
    return (highest >= 0 && (flags & kDescriptorBindingVariableDescriptorCount) != 0) ? highest
                                                                                      : -1;
}

// Vulkan 1.3 support (inlineUniformBlock): the SHARED per-binding admission for the
// INLINE_UNIFORM_BLOCK type, called by both backends' layout create AND layout support (mock ==
// real by the shared helper; a layout the relay would reject at create is never reported
// "supported"). An IUB binding's descriptorCount is a BYTE size, not an element count (the spec
// VUs): positive, a multiple of 4, and bounded by kMaxInlineUniformBlockBytes (the mock models
// one slot per byte). The type itself is gated on the device's enabled inlineUniformBlock bit.
inline bool inline_uniform_block_layout_ok(const std::vector<DescriptorSetLayoutBindingDesc>& binds,
                                           std::uint64_t vk13_bits, const char** reason) {
    for (const DescriptorSetLayoutBindingDesc& b : binds) {
        if (b.descriptor_type != kDescriptorTypeInlineUniformBlock) {
            continue;
        }
        if ((vk13_bits & kVk13FeatureInlineUniformBlock) == 0) {
            *reason = "inline uniform block requires the enabled inlineUniformBlock feature";
            return false;
        }
        if (b.descriptor_count <= 0 || (b.descriptor_count % 4) != 0) {
            *reason = "inline uniform block byte size must be a positive multiple of 4";
            return false;
        }
        if (b.descriptor_count > kMaxInlineUniformBlockBytes) {
            *reason = "inline uniform block byte size out of bounds";
            return false;
        }
    }
    return true;
}

struct CreateDescriptorSetLayoutRequest {
    std::uint64_t device = 0;
    // (GL/zink): VkDescriptorSetLayoutCreateFlags (carried wide). zink chains a
    // VkDescriptorSetLayoutBindingFlagsCreateInfo pNext whose per-binding flags ride
    // DescriptorSetLayoutBindingDesc::binding_flags; the worker rebuilds that pNext when any flag
    // is nonzero. UPDATE_AFTER_BIND is structurally absent (descriptor indexing is masked).
    long long layout_flags = 0;
    std::vector<DescriptorSetLayoutBindingDesc> bindings;

    json::Value to_body() const;
    static CreateDescriptorSetLayoutRequest from_body(const json::Value& body);
};

struct CreateDescriptorSetLayoutResponse {
    bool ok = false;
    std::string reason;
    std::uint64_t set_layout = 0;

    json::Value to_body() const;
    static CreateDescriptorSetLayoutResponse from_body(const json::Value& body);
};

// vkGetDescriptorSetLayoutSupport (RpcOp 88): the HOST's real answer replaces the
// ICD's local "supported=TRUE + generous max" fiction. The request reuses
// CreateDescriptorSetLayoutRequest (same shape, nothing created). max_variable_descriptor_count
// is meaningful only when the layout has a variable-count last binding (0 otherwise).
struct GetDescriptorSetLayoutSupportResponse {
    bool ok = false;
    std::string reason;
    int supported = 0; // honest VkBool32 from the host (mock: the relay's own admission)
    std::uint64_t max_variable_descriptor_count = 0;

    json::Value to_body() const;
    static GetDescriptorSetLayoutSupportResponse from_body(const json::Value& body);
};

// One descriptor pool size. The bounded shape uses poolSizeCount == 1 and type == UNIFORM_BUFFER
// (the simplest
// cut; widens to multiple sizes when the sampler type joins).
struct DescriptorPoolSizeDesc {
    int type = -1; // VkDescriptorType (UNIFORM_BUFFER)
    int descriptor_count = -1;
};

struct CreateDescriptorPoolRequest {
    std::uint64_t device = 0;
    int max_sets = -1;
    long long flags = 0; // (GL/zink): VkDescriptorPoolCreateFlags (e.g. FREE_DESCRIPTOR_SET)
    std::vector<DescriptorPoolSizeDesc> pool_sizes; // any host-supported types/counts
    // Vulkan 1.3 support (inlineUniformBlock): VkDescriptorPoolInlineUniformBlockCreateInfo's
    // maxInlineUniformBlockBindings (0 = the pNext was absent). The worker rebuilds the pNext
    // when non-zero; an INLINE_UNIFORM_BLOCK pool size (whose descriptorCount is a BYTE budget)
    // without this pNext is spec-invalid and both backends reject it.
    int max_inline_uniform_block_bindings = 0;

    json::Value to_body() const;
    static CreateDescriptorPoolRequest from_body(const json::Value& body);
};

struct CreateDescriptorPoolResponse {
    bool ok = false;
    std::string reason;
    std::uint64_t pool = 0;

    json::Value to_body() const;
    static CreateDescriptorPoolResponse from_body(const json::Value& body);
};

// Allocate descriptor sets from a pool (one per requested set layout). The whole batch is validated
// atomically against the pool's maxSets + UBO budget before any set is minted.
struct AllocateDescriptorSetsRequest {
    std::uint64_t device = 0;
    std::uint64_t pool = 0;
    std::vector<std::uint64_t> set_layouts; // one per set to allocate
    // descriptorIndexing: VkDescriptorSetVariableDescriptorCountAllocateInfo,
    // serialized rather than dropped. EMPTY = the pNext was absent (or its descriptorSetCount was
    // 0) -> every variable length is 0 per spec; non-empty MUST parallel set_layouts. Each entry
    // is the allocated count for that set's variable last binding -- IGNORED for layouts without
    // one, and <= the layout's declared max for layouts with one.
    std::vector<std::uint64_t> variable_counts;

    json::Value to_body() const;
    static AllocateDescriptorSetsRequest from_body(const json::Value& body);
};

struct AllocateDescriptorSetsResponse {
    bool ok = false;
    std::string reason;
    std::vector<std::uint64_t> descriptor_sets; // parallel to the requested set_layouts

    json::Value to_body() const;
    static AllocateDescriptorSetsResponse from_body(const json::Value& body);
};

// One VkDescriptorBufferInfo. offset/range are VkDeviceSize -> decimal strings (a
// JSON number cannot carry VK_WHOLE_SIZE = UINT64_MAX exactly). The bounded shape uses offset == 0
// and range != 0; the
// resolved range (range == VK_WHOLE_SIZE -> the buffer's remaining size) must be <= the buffer size
// AND <= kUniformBufferRangeCap.
struct DescriptorBufferInfoDesc {
    std::uint64_t buffer = 0;
    std::uint64_t offset = 0;
    std::uint64_t range = 0;
};

// One VkDescriptorImageInfo : a combined-image-sampler write
// references a live sampler + a live image view (over a SAMPLED image) at imageLayout
// SHADER_READ_ONLY_OPTIMAL. The worker validates each against the recorded objects; a violating
// write poisons its target set (device-scoped), exactly as the buffer-info path.
struct DescriptorImageInfoDesc {
    std::uint64_t sampler = 0;
    std::uint64_t image_view = 0;
    int image_layout = -1; // VkImageLayout (SHADER_READ_ONLY_OPTIMAL)
};

// One VkWriteDescriptorSet (writes only -- no VkCopyDescriptorSet is representable, structurally
// enforcing copyCount == 0). UNIFORM_BUFFER writes require buffer_infos.size ==
// descriptor_count. COMBINED_IMAGE_SAMPLER writes are carried via image_infos (exactly one
// of buffer_infos / image_infos is populated, keyed by descriptor_type). For either: info-count ==
// descriptor_count; dst_array_element + descriptor_count <= the binding's descriptorCount.
struct WriteDescriptorSetDesc {
    std::uint64_t dst_set = 0;
    int dst_binding = -1;
    int dst_array_element = -1;
    int descriptor_type = -1;
    int descriptor_count = -1;
    // Exactly one of these is populated, keyed by the descriptor type CLASS: buffer_infos for the
    // buffer types (UNIFORM/STORAGE[_DYNAMIC]); image_infos for the image/sampler types (SAMPLER,
    // SAMPLED_IMAGE, STORAGE_IMAGE, COMBINED_IMAGE_SAMPLER, INPUT_ATTACHMENT); texel_buffer_views
    // for the texel types (UNIFORM/STORAGE_TEXEL_BUFFER). (GL/zink) broadened this from the
    // vkcube UNIFORM_BUFFER + COMBINED_IMAGE_SAMPLER pair to faithful all-type forwarding.
    std::vector<DescriptorBufferInfoDesc> buffer_infos;
    std::vector<DescriptorImageInfoDesc> image_infos;
    std::vector<std::uint64_t> texel_buffer_views;
    // Vulkan 1.3 support (inlineUniformBlock): the RAW BYTES of a
    // VkWriteDescriptorSetInlineUniformBlock write (hex on the JSON wire). For the
    // INLINE_UNIFORM_BLOCK type: descriptor_count = the BYTE count = inline_data.size(),
    // dst_array_element = the BYTE offset, both multiples of 4 (spec VUs, validated at both
    // ends); the three structured vectors stay empty.
    std::string inline_data;
};

// Update descriptor sets. Validate-then-apply: if any write is invalid, no descriptor-set state
// changes and the real backend never calls vkUpdateDescriptorSets. Overlapping
// writes within one batch are rejected. Response is StatusResponse.
struct UpdateDescriptorSetsRequest {
    std::uint64_t device = 0;
    std::vector<WriteDescriptorSetDesc> writes;

    json::Value to_body() const;
    static UpdateDescriptorSetsRequest from_body(const json::Value& body);
};

// Worker-side abstraction over host Vulkan. The real implementation lives only in the worker; the
// supervisor never loads a Vulkan driver.
class VulkanBackend {
  public:
    virtual ~VulkanBackend() = default;
    virtual CapabilitiesResponse negotiate(const CapabilitiesRequest& req) = 0;
    virtual CreateInstanceResponse create_instance(const CreateInstanceRequest& req) = 0;
    virtual EnumeratePhysicalDevicesResponse
    enumerate_physical_devices(const EnumeratePhysicalDevicesRequest& req) = 0;
    virtual CreateDeviceResponse create_device(const CreateDeviceRequest& req) = 0;
    virtual StatusResponse destroy_device(const HandleRequest& req) = 0;
    virtual StatusResponse destroy_instance(const HandleRequest& req) = 0;
    virtual GetDeviceQueueResponse get_device_queue(const GetDeviceQueueRequest& req) = 0;
    virtual CreateCommandPoolResponse create_command_pool(const CreateCommandPoolRequest& req) = 0;
    virtual StatusResponse destroy_command_pool(const HandleRequest& req) = 0;
    virtual AllocateCommandBuffersResponse
    allocate_command_buffers(const AllocateCommandBuffersRequest& req) = 0;
    virtual StatusResponse free_command_buffers(const FreeCommandBuffersRequest& req) = 0;
    virtual CreateFenceResponse create_fence(const CreateFenceRequest& req) = 0;
    virtual StatusResponse destroy_fence(const HandleRequest& req) = 0;
    virtual CreateSemaphoreResponse create_semaphore(const CreateSemaphoreRequest& req) = 0;
    virtual StatusResponse destroy_semaphore(const HandleRequest& req) = 0;
    virtual AllocateMemoryResponse allocate_memory(const AllocateMemoryRequest& req) = 0;
    virtual StatusResponse free_memory(const HandleRequest& req) = 0;
    virtual CreateSurfaceResponse create_surface(const CreateSurfaceRequest& req) = 0;
    virtual StatusResponse destroy_surface(const HandleRequest& req) = 0;
    virtual CreateSwapchainResponse create_swapchain(const CreateSwapchainRequest& req) = 0;
    virtual StatusResponse destroy_swapchain(const HandleRequest& req) = 0;
    virtual GetSwapchainImagesResponse
    get_swapchain_images(const GetSwapchainImagesRequest& req) = 0;
    virtual AcquireNextImageResponse acquire_next_image(const AcquireNextImageRequest& req) = 0;
    virtual QueuePresentResponse queue_present(const QueuePresentRequest& req) = 0;
    virtual StatusResponse record_command_buffer(const RecordCommandBufferRequest& req) = 0;
    virtual QueueSubmitResponse queue_submit(const QueueSubmitRequest& req) = 0;
    // vkQueueSubmit2 -- the whole VkSubmitInfo2[] batch + one fence in one call.
    virtual QueueSubmitResponse queue_submit2(const QueueSubmit2Request& req) = 0;
    virtual StatusResponse reset_fences(const ResetFencesRequest& req) = 0;
    virtual WaitForFencesResponse wait_for_fences(const WaitForFencesRequest& req) = 0;
    // (core-1.0 sync honesty): fence status poll + the VkEvent object model.
    virtual GetFenceStatusResponse get_fence_status(const HandleRequest& req) = 0;
    virtual CreateEventResponse create_event(const CreateEventRequest& req) = 0;
    virtual StatusResponse destroy_event(const HandleRequest& req) = 0;
    virtual GetEventStatusResponse get_event_status(const HandleRequest& req) = 0;
    virtual StatusResponse set_event(const HandleRequest& req) = 0;
    virtual StatusResponse reset_event(const HandleRequest& req) = 0;
    // REAL idle waits (readback-completion sync points).
    virtual WaitIdleResponse queue_wait_idle(const HandleRequest& req) = 0;
    virtual WaitIdleResponse device_wait_idle(const HandleRequest& req) = 0;
    // (GL/zink): host-side timeline-semaphore ops.
    virtual WaitSemaphoresResponse wait_semaphores(const WaitSemaphoresRequest& req) = 0;
    virtual StatusResponse signal_semaphore(const SignalSemaphoreRequest& req) = 0;
    virtual GetSemaphoreCounterValueResponse
    get_semaphore_counter_value(const GetSemaphoreCounterValueRequest& req) = 0;
    virtual GetSurfaceCapabilitiesResponse
    get_surface_capabilities(const GetSurfaceCapabilitiesRequest& req) = 0;
    virtual GetSurfaceFormatsResponse get_surface_formats(const GetSurfaceFormatsRequest& req) = 0;
    virtual GetSurfacePresentModesResponse
    get_surface_present_modes(const GetSurfacePresentModesRequest& req) = 0;
    virtual GetSurfaceSupportResponse get_surface_support(const GetSurfaceSupportRequest& req) = 0;
    // Draw surface.
    virtual CreateImageViewResponse create_image_view(const CreateImageViewRequest& req) = 0;
    virtual StatusResponse destroy_image_view(const HandleRequest& req) = 0;
    // (GL/zink): texel buffer view (faithfully marshalled, no subset).
    virtual CreateBufferViewResponse create_buffer_view(const CreateBufferViewRequest& req) = 0;
    virtual StatusResponse destroy_buffer_view(const HandleRequest& req) = 0;
    virtual CreateShaderModuleResponse
    create_shader_module(const CreateShaderModuleRequest& req) = 0;
    virtual StatusResponse destroy_shader_module(const HandleRequest& req) = 0;
    virtual CreateRenderPassResponse create_render_pass(const CreateRenderPassRequest& req) = 0;
    virtual StatusResponse destroy_render_pass(const HandleRequest& req) = 0;
    virtual CreateFramebufferResponse create_framebuffer(const CreateFramebufferRequest& req) = 0;
    virtual StatusResponse destroy_framebuffer(const HandleRequest& req) = 0;
    virtual CreatePipelineLayoutResponse
    create_pipeline_layout(const CreatePipelineLayoutRequest& req) = 0;
    virtual StatusResponse destroy_pipeline_layout(const HandleRequest& req) = 0;
    virtual CreateGraphicsPipelinesResponse
    create_graphics_pipelines(const CreateGraphicsPipelinesRequest& req) = 0;
    virtual CreateComputePipelinesResponse
    create_compute_pipelines(const CreateComputePipelinesRequest& req) = 0;
    virtual StatusResponse destroy_pipeline(const HandleRequest& req) = 0;
    // Host-visible memory + buffers. Pure like the rest now that both the mock
    // and the worker's RealVulkanBackend override them (they were transiently non-pure across
    // chunks 3-4 so the real backend kept compiling before its overrides landed).
    virtual GetPhysicalDeviceMemoryPropertiesResponse
    get_physical_device_memory_properties(const GetPhysicalDeviceMemoryPropertiesRequest& req) = 0;
    virtual CreateBufferResponse create_buffer(const CreateBufferRequest& req) = 0;
    virtual StatusResponse destroy_buffer(const HandleRequest& req) = 0;
    virtual StatusResponse bind_buffer_memory(const BindBufferMemoryRequest& req) = 0;
    // (bufferDeviceAddress): the real host VkDeviceAddress verbatim (mock: per-buffer
    // token). Gated on the device's enabled bufferDeviceAddress feature, mock == real.
    virtual GetBufferDeviceAddressResponse
    get_buffer_device_address(const GetBufferDeviceAddressRequest& req) = 0;
    virtual StatusResponse write_memory_ranges(const WriteMemoryRangesRequest& req) = 0;
    virtual ReadMemoryRangesResponse read_memory_ranges(const ReadMemoryRangesRequest& req) = 0;
    // Descriptor surface + per-frame UBO. Both the mock and RealVulkanBackend implement these
    // interfaces.
    virtual CreateDescriptorSetLayoutResponse
    create_descriptor_set_layout(const CreateDescriptorSetLayoutRequest& req) = 0;
    // descriptorIndexing: the honest layout-support query (host answer on the real
    // backend; the relay's own admission on the mock).
    virtual GetDescriptorSetLayoutSupportResponse
    get_descriptor_set_layout_support(const CreateDescriptorSetLayoutRequest& req) = 0;
    // Vulkan 1.3 support (maintenance4): create-info-shaped memory-requirement queries (nothing
    // is created; the response's handle stays 0). Host answers on the real backend; the mock
    // answers with its modeled requirements.
    virtual CreateBufferResponse
    get_device_buffer_memory_requirements(const CreateBufferRequest& req) = 0;
    virtual CreateImageResponse
    get_device_image_memory_requirements(const CreateImageRequest& req) = 0;
    virtual StatusResponse destroy_descriptor_set_layout(const HandleRequest& req) = 0;
    virtual CreateDescriptorPoolResponse
    create_descriptor_pool(const CreateDescriptorPoolRequest& req) = 0;
    virtual StatusResponse destroy_descriptor_pool(const HandleRequest& req) = 0;
    virtual AllocateDescriptorSetsResponse
    allocate_descriptor_sets(const AllocateDescriptorSetsRequest& req) = 0;
    virtual StatusResponse update_descriptor_sets(const UpdateDescriptorSetsRequest& req) = 0;
    // Textures + depth for literal vkcube. Pure (both the mock and the
    // worker's RealVulkanBackend override them).
    virtual GetPhysicalDeviceFormatPropertiesResponse
    get_physical_device_format_properties(const GetPhysicalDeviceFormatPropertiesRequest& req) = 0;
    // (GL/zink): honest physical-device feature + image-format-support forwarding. Pure
    // (both the mock and the worker's RealVulkanBackend override them).
    virtual GetPhysicalDeviceFeaturesResponse
    get_physical_device_features(const GetPhysicalDeviceFeaturesRequest& req) = 0;
    virtual GetPhysicalDeviceImageFormatPropertiesResponse
    get_physical_device_image_format_properties(
        const GetPhysicalDeviceImageFormatPropertiesRequest& req) = 0;
    virtual GetPhysicalDevicePropertiesResponse
    get_physical_device_properties(const GetPhysicalDevicePropertiesRequest& req) = 0;
    virtual GetPhysicalDeviceCapabilityChainResponse
    get_physical_device_capability_chain(const GetPhysicalDeviceCapabilityChainRequest& req) = 0;
    virtual CreateImageResponse create_image(const CreateImageRequest& req) = 0;
    virtual StatusResponse destroy_image(const HandleRequest& req) = 0;
    virtual StatusResponse bind_image_memory(const BindImageMemoryRequest& req) = 0;
    // Sampler support. Pure (both the mock and the worker's RealVulkanBackend
    // override them). The combined-image-sampler descriptor type + the texture-upload commands need
    // no new virtual -- they extend update_descriptor_sets / record_command_buffer in place.
    virtual CreateSamplerResponse create_sampler(const CreateSamplerRequest& req) = 0;
    virtual StatusResponse destroy_sampler(const HandleRequest& req) = 0;
    // Query pools (GL 3.3 / occlusion / xfb queries).
    virtual CreateQueryPoolResponse create_query_pool(const CreateQueryPoolRequest& req) = 0;
    virtual StatusResponse destroy_query_pool(const HandleRequest& req) = 0;
    virtual GetQueryPoolResultsResponse
    get_query_pool_results(const GetQueryPoolResultsRequest& req) = 0;
    // hostQueryReset: device-level vkResetQueryPool.
    virtual StatusResponse reset_query_pool(const ResetQueryPoolRequest& req) = 0;

    // (windowing teardown robustness): session-abort hook. Called by the out-of-band
    // liveness observer -- a thread OTHER than the single RPC dispatcher -- the moment the app
    // connection peer-closes, so an in-flight blocking WSI call (the acquire poll) releases and the
    // session tears down even while the dispatcher is wedged in the Win32 pump. MUST be thread-safe
    // and NONBLOCKING (the real backend only stores an atomic). Default no-op: backends with no
    // blocking WSI path need nothing.
    virtual void abort_session() {}
};

// Mock backend: reports the device selected for this worker (looked up in the
// mocked probe by name), caps the API to the worker-supported level, and runs the
// instance/device lifecycle state machine in a per-session object table. Used by
// one app connection at a time (the worker serves RPCs single-threaded), so the
// table needs no locking.
class MockVulkanBackend : public VulkanBackend, public sidecar::SidecarBackend {
  public:
    explicit MockVulkanBackend(const std::string& gpu_name,
                               const display::DisplayLayout* display_layout = nullptr);
    CapabilitiesResponse negotiate(const CapabilitiesRequest& req) override;
    // Sidecar plane: the mock is one session object spanning both planes, so the
    // dual-platform tests exercise the sidecar plane against the same shape as the real worker.
    sidecar::SidecarNegotiateResponse
    negotiate(const sidecar::SidecarNegotiateRequest& req) override;
    sidecar::SidecarReadyResponse sidecar_ready(const sidecar::SidecarReadyRequest& req) override;
    // Worker-home toplevel registry lifecycle: drives the SAME pure WindowRegistry as
    // the real backend (so the state machine is identical mock == real); the mock "executes"
    // placeholder effects against a fake-id set (no HWNDs) so its executor is exercised + asserted.
    sidecar::SidecarToplevelResponse
    register_toplevel(const sidecar::SidecarRegisterToplevelRequest& req) override;
    sidecar::SidecarToplevelResponse
    update_toplevel(const sidecar::SidecarUpdateToplevelRequest& req) override;
    sidecar::SidecarToplevelResponse
    unregister_toplevel(const sidecar::SidecarUnregisterToplevelRequest& req) override;
    // (show/hide lifecycle): flip the registry's live visibility (generation-gated).
    // The mock has no HWND -- the authored visibility_state lives in the registry + is reported via
    // DebugEnumWindows; a hide preserves the entry + epoch (mock == real for the decision).
    sidecar::SidecarToplevelResponse
    set_visibility(const sidecar::SidecarSetVisibilityRequest& req) override;
    // Chrome pixels: the mock has no HWND/DIB, so it "paints" captured BGRA into a
    // synthetic per-xid pixel store (the stand-in for the real backend's window-thread DIB) so the
    // accept->paint->commit dance + the DebugChromeState pixel-sample proof are dual-platform.
    sidecar::SidecarPaintResponse
    paint_chrome(const sidecar::SidecarPaintChromeRequest& req) override;
    sidecar::SidecarDebugChromeStateResponse
    debug_chrome_state(const sidecar::SidecarDebugChromeStateRequest& req) override;
    // Input plane: the mock has no window thread, so debug_enqueue_input IS its input
    // producer (the WndProc stand-in) and poll_input drains the SAME pure InputQueue + applies the
    // SAME registry liveness gate as the real backend -- so the dual-platform tests pin the ring /
    // coalescing / staleness behavior mock == real.
    sidecar::SidecarPollInputResponse
    poll_input(const sidecar::SidecarPollInputRequest& req) override;
    sidecar::SidecarDebugEnqueueInputResponse
    debug_enqueue_input(const sidecar::SidecarDebugEnqueueInputRequest& req) override;
    // Cursor plane: the mock has no HWND, so set_cursor stores the BGRA cursor into a
    // synthetic per-xid buffer (the stand-in for the real backend's HCURSOR) so the SetCursor
    // decode
    // + the DebugCursorState pixel-sample proof are dual-platform. Gated on the xid having a live
    // representation, like the real backend (which needs a window to bind the HCURSOR to).
    sidecar::SidecarSetCursorResponse
    set_cursor(const sidecar::SidecarSetCursorRequest& req) override;
    sidecar::SidecarDebugCursorStateResponse
    debug_cursor_state(const sidecar::SidecarDebugCursorStateRequest& req) override;
    // a registry-derived snapshot (mock == real -- both drive the same
    // WindowRegistry).
    sidecar::SidecarDebugEnumWindowsResponse
    debug_enum_windows(const sidecar::SidecarDebugEnumWindowsRequest& req) override;
    // capture a layer's BGRA from the synthetic store (mock == real -- the real
    // backend copies the window-thread DIB/cursor; the absent/empty/mismatch/too_large contract
    // matches).
    sidecar::SidecarDebugCaptureWindowResponse
    debug_capture_window(const sidecar::SidecarDebugCaptureWindowRequest& req) override;
    CreateInstanceResponse create_instance(const CreateInstanceRequest& req) override;
    EnumeratePhysicalDevicesResponse
    enumerate_physical_devices(const EnumeratePhysicalDevicesRequest& req) override;
    CreateDeviceResponse create_device(const CreateDeviceRequest& req) override;
    StatusResponse destroy_device(const HandleRequest& req) override;
    StatusResponse destroy_instance(const HandleRequest& req) override;
    GetDeviceQueueResponse get_device_queue(const GetDeviceQueueRequest& req) override;
    CreateCommandPoolResponse create_command_pool(const CreateCommandPoolRequest& req) override;
    StatusResponse destroy_command_pool(const HandleRequest& req) override;
    AllocateCommandBuffersResponse
    allocate_command_buffers(const AllocateCommandBuffersRequest& req) override;
    StatusResponse free_command_buffers(const FreeCommandBuffersRequest& req) override;
    CreateFenceResponse create_fence(const CreateFenceRequest& req) override;
    StatusResponse destroy_fence(const HandleRequest& req) override;
    CreateSemaphoreResponse create_semaphore(const CreateSemaphoreRequest& req) override;
    StatusResponse destroy_semaphore(const HandleRequest& req) override;
    AllocateMemoryResponse allocate_memory(const AllocateMemoryRequest& req) override;
    StatusResponse free_memory(const HandleRequest& req) override;
    CreateSurfaceResponse create_surface(const CreateSurfaceRequest& req) override;
    StatusResponse destroy_surface(const HandleRequest& req) override;
    CreateSwapchainResponse create_swapchain(const CreateSwapchainRequest& req) override;
    StatusResponse destroy_swapchain(const HandleRequest& req) override;
    GetSwapchainImagesResponse get_swapchain_images(const GetSwapchainImagesRequest& req) override;
    AcquireNextImageResponse acquire_next_image(const AcquireNextImageRequest& req) override;
    QueuePresentResponse queue_present(const QueuePresentRequest& req) override;
    StatusResponse record_command_buffer(const RecordCommandBufferRequest& req) override;
    QueueSubmitResponse queue_submit(const QueueSubmitRequest& req) override;
    QueueSubmitResponse queue_submit2(const QueueSubmit2Request& req) override;
    StatusResponse reset_fences(const ResetFencesRequest& req) override;
    WaitForFencesResponse wait_for_fences(const WaitForFencesRequest& req) override;
    GetFenceStatusResponse get_fence_status(const HandleRequest& req) override;
    CreateEventResponse create_event(const CreateEventRequest& req) override;
    StatusResponse destroy_event(const HandleRequest& req) override;
    GetEventStatusResponse get_event_status(const HandleRequest& req) override;
    StatusResponse set_event(const HandleRequest& req) override;
    StatusResponse reset_event(const HandleRequest& req) override;
    WaitIdleResponse queue_wait_idle(const HandleRequest& req) override;
    WaitIdleResponse device_wait_idle(const HandleRequest& req) override;
    WaitSemaphoresResponse wait_semaphores(const WaitSemaphoresRequest& req) override;
    StatusResponse signal_semaphore(const SignalSemaphoreRequest& req) override;
    GetSemaphoreCounterValueResponse
    get_semaphore_counter_value(const GetSemaphoreCounterValueRequest& req) override;
    GetSurfaceCapabilitiesResponse
    get_surface_capabilities(const GetSurfaceCapabilitiesRequest& req) override;
    GetSurfaceFormatsResponse get_surface_formats(const GetSurfaceFormatsRequest& req) override;
    GetSurfacePresentModesResponse
    get_surface_present_modes(const GetSurfacePresentModesRequest& req) override;
    GetSurfaceSupportResponse get_surface_support(const GetSurfaceSupportRequest& req) override;
    // Draw surface.
    CreateImageViewResponse create_image_view(const CreateImageViewRequest& req) override;
    StatusResponse destroy_image_view(const HandleRequest& req) override;
    CreateBufferViewResponse create_buffer_view(const CreateBufferViewRequest& req) override;
    StatusResponse destroy_buffer_view(const HandleRequest& req) override;
    CreateShaderModuleResponse create_shader_module(const CreateShaderModuleRequest& req) override;
    StatusResponse destroy_shader_module(const HandleRequest& req) override;
    CreateRenderPassResponse create_render_pass(const CreateRenderPassRequest& req) override;
    StatusResponse destroy_render_pass(const HandleRequest& req) override;
    CreateFramebufferResponse create_framebuffer(const CreateFramebufferRequest& req) override;
    StatusResponse destroy_framebuffer(const HandleRequest& req) override;
    CreatePipelineLayoutResponse
    create_pipeline_layout(const CreatePipelineLayoutRequest& req) override;
    StatusResponse destroy_pipeline_layout(const HandleRequest& req) override;
    CreateGraphicsPipelinesResponse
    create_graphics_pipelines(const CreateGraphicsPipelinesRequest& req) override;
    CreateComputePipelinesResponse
    create_compute_pipelines(const CreateComputePipelinesRequest& req) override;
    StatusResponse destroy_pipeline(const HandleRequest& req) override;
    // Host-visible memory + buffers.
    GetPhysicalDeviceMemoryPropertiesResponse get_physical_device_memory_properties(
        const GetPhysicalDeviceMemoryPropertiesRequest& req) override;
    CreateBufferResponse create_buffer(const CreateBufferRequest& req) override;
    StatusResponse destroy_buffer(const HandleRequest& req) override;
    StatusResponse bind_buffer_memory(const BindBufferMemoryRequest& req) override;
    GetBufferDeviceAddressResponse
    get_buffer_device_address(const GetBufferDeviceAddressRequest& req) override;
    StatusResponse write_memory_ranges(const WriteMemoryRangesRequest& req) override;
    ReadMemoryRangesResponse read_memory_ranges(const ReadMemoryRangesRequest& req) override;
    // Descriptor surface + per-frame UBO.
    CreateDescriptorSetLayoutResponse
    create_descriptor_set_layout(const CreateDescriptorSetLayoutRequest& req) override;
    GetDescriptorSetLayoutSupportResponse
    get_descriptor_set_layout_support(const CreateDescriptorSetLayoutRequest& req) override;
    CreateBufferResponse
    get_device_buffer_memory_requirements(const CreateBufferRequest& req) override;
    CreateImageResponse
    get_device_image_memory_requirements(const CreateImageRequest& req) override;
    StatusResponse destroy_descriptor_set_layout(const HandleRequest& req) override;
    CreateDescriptorPoolResponse
    create_descriptor_pool(const CreateDescriptorPoolRequest& req) override;
    StatusResponse destroy_descriptor_pool(const HandleRequest& req) override;
    AllocateDescriptorSetsResponse
    allocate_descriptor_sets(const AllocateDescriptorSetsRequest& req) override;
    StatusResponse update_descriptor_sets(const UpdateDescriptorSetsRequest& req) override;
    // Textures + depth for literal vkcube.
    GetPhysicalDeviceFormatPropertiesResponse get_physical_device_format_properties(
        const GetPhysicalDeviceFormatPropertiesRequest& req) override;
    // (GL/zink).
    GetPhysicalDeviceFeaturesResponse
    get_physical_device_features(const GetPhysicalDeviceFeaturesRequest& req) override;
    GetPhysicalDeviceImageFormatPropertiesResponse get_physical_device_image_format_properties(
        const GetPhysicalDeviceImageFormatPropertiesRequest& req) override;
    GetPhysicalDevicePropertiesResponse
    get_physical_device_properties(const GetPhysicalDevicePropertiesRequest& req) override;
    GetPhysicalDeviceCapabilityChainResponse get_physical_device_capability_chain(
        const GetPhysicalDeviceCapabilityChainRequest& req) override;
    CreateImageResponse create_image(const CreateImageRequest& req) override;
    StatusResponse destroy_image(const HandleRequest& req) override;
    StatusResponse bind_image_memory(const BindImageMemoryRequest& req) override;
    // Sampler support.
    CreateSamplerResponse create_sampler(const CreateSamplerRequest& req) override;
    StatusResponse destroy_sampler(const HandleRequest& req) override;
    CreateQueryPoolResponse create_query_pool(const CreateQueryPoolRequest& req) override;
    StatusResponse destroy_query_pool(const HandleRequest& req) override;
    GetQueryPoolResultsResponse
    get_query_pool_results(const GetQueryPoolResultsRequest& req) override;
    StatusResponse reset_query_pool(const ResetQueryPoolRequest& req) override;

    // Test-only seam (not an RPC op): marks a surface geometry-dirty, so the latch
    // state machine -- acquire/present return OUT_OF_DATE without "calling the driver",
    // create_swapchain clears it -- is exercisable headless on both platforms, where the
    // real backend (which sets the latch from a Win32 WM_SIZE) cannot build.
    void debug_mark_surface_dirty(std::uint64_t surface);
    // the surface born-correlated with a guest XID, or 0 if none (mock == real -- the
    // worker-home registry is shared logic, so the dual-platform test pins the behavior here).
    std::uint64_t debug_registry_surface_for_xid(std::uint64_t xid) const {
        return registry_.surface_for_xid(xid);
    }
    // whether the sidecar has emitted its readiness barrier on this session.
    bool debug_sidecar_ready() const { return sidecar_ready_; }
    // Structural seams (mock == real): the registry's view + the executor's view.
    sidecar::Representation debug_representation_for_xid(std::uint64_t xid) const {
        return registry_.representation_for_xid(xid);
    }
    bool debug_toplevel_registered(std::uint64_t xid) const {
        return registry_.toplevel_registered(xid);
    }
    std::size_t debug_registry_entry_count() const { return registry_.size(); }
    std::size_t debug_registry_placeholder_count() const { return registry_.placeholder_count(); }
    // The mock executor's live placeholder ids; must equal debug_registry_placeholder_count().
    std::size_t debug_executor_placeholder_count() const { return mock_placeholders_.size(); }
    // in-process helper seams (the wire-level proof is the DebugChromeState op).
    bool debug_placeholder_shown(std::uint64_t xid) const {
        return registry_.placeholder_shown(xid);
    }
    std::uint64_t debug_last_paint_seq(std::uint64_t xid) const {
        return registry_.last_paint_seq(xid);
    }
    // the input ring depth (events queued but not yet polled).
    std::size_t debug_input_queue_size() const { return input_queue_.size(); }
    // the xid's current representation epoch (the worker's exact-epoch input gate key).
    std::uint64_t debug_registry_epoch_for_xid(std::uint64_t xid) const {
        return registry_.epoch_for_xid(xid);
    }

  private:
    DeviceCaps device_caps() const;
    // Apply a RegistryEffect against the mock's fake-id placeholder set (the mock's stand-in for
    // the real backend's HWND side table) -- caller holds backend_mutex_.
    void apply_mock_effect(const sidecar::RegistryEffect& eff);
    // composite a chrome paint's dirty rect into the synthetic per-xid pixel buffer (the
    // mock's stand-in for the real DIB) -- caller holds backend_mutex_.
    void mock_paint_chrome(const sidecar::SidecarPaintChromeRequest& req);

    // Device-child leaf objects (no children): fences, semaphores, device memory, events.
    enum class LeafKind { Fence, Semaphore, Memory, Event };
    // Creates a leaf on `device` (or sets `err` + returns 0 if the device is
    // unknown); destroys a leaf of the expected kind (or sets `err`).
    std::uint64_t create_device_leaf(std::uint64_t device, LeafKind kind, std::string& err);
    bool destroy_device_leaf(std::uint64_t handle, LeafKind kind, std::string& err);

    // Validates a fence array for reset_fences / wait_for_fences: non-empty, every handle
    // a fence on the SAME device (cross-device arrays are rejected before any driver call).
    bool validate_fence_array(const std::vector<std::uint64_t>& fences, std::string& err) const;

    struct Instance {
        std::uint64_t physical_device = 0; // the one selected GPU, per instance
        std::set<std::uint64_t> devices;   // live device handles
        std::set<std::uint64_t> surfaces;  // live surface handles (instance children)
    };
    struct Device {
        std::uint64_t instance = 0;                    // parent
        int queue_family = 0;                          // the family create_device made queues from
        int queue_count = 0;                           // queues created in that family
        std::set<std::uint64_t> pools;                 // live command pools
        std::set<std::uint64_t> leaves;                // live fences/semaphores/memory
        std::set<std::uint64_t> swapchains;            // live swapchains (device children)
        std::map<std::uint64_t, std::uint64_t> queues; // packed(family,index) -> queue handle
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
        // (GL/zink): the extensions enabled at create_device -- extension commands (transform
        // feedback / conditional rendering) validate against this set, mock == real.
        std::set<std::string> enabled_exts;
        // (native lane): did the app enable the
        // dynamicRendering FEATURE (not just the extension)? DR commands + pipelines require ext
        // AND feature.
        bool dynamic_rendering_feature_enabled = false;
        // (native lane): the parallel synchronization2 feature bit. Every sync2
        // command requires the extension (enabled_exts) AND this feature (mock == real).
        bool synchronization2_feature_enabled = false;
        // Required-feature audit (hardening): the enabled hostQueryReset
        // feature. Device-level reset_query_pool fails closed unless this is set (mock == real).
        bool host_query_reset_feature_enabled = false;
        // Required-feature audit: the enabled multiview feature. A viewMask render pass
        // (create_render_pass view_mask != 0) fails closed unless this is set (mock == real).
        bool multiview_feature_enabled = false;
        // (bufferDeviceAddress): the enabled 1.2 feature (no extension gate -- core 1.2).
        // Gates get_buffer_device_address + the DEVICE_ADDRESS allocate flag (mock == real).
        bool buffer_device_address_feature_enabled = false;
        // vertex-attr-divisor: the two enabled feature bits. Gate the divisor VALUES a pipeline may
        // carry via the shared vkrpc::vertex_binding_divisors_ok (mock == real).
        bool vertex_attr_divisor_feature_enabled = false;
        bool vertex_attr_zero_divisor_feature_enabled = false;
        // geometry-stream: the enabled geometryStreams feature. Gates the rasterization-stream
        // pNext via the shared vkrpc::rasterization_stream_ok, parameterized with the mock's
        // deterministic modeled properties (kMockMaxTransformFeedbackStreams/StreamSelect).
        // The mock is deliberately a SCALAR-ONLY oracle here: it does not
        // interpret the forwarded feature-chain blobs, so an omitted scalar (-1, old peer) is
        // treated as DISABLED and scalar/chain agreement is real-worker-only coverage
        // (integration_real_draw). It DOES mirror the structural extension invariant: a true
        // scalar without VK_EXT_transform_feedback rejects by name (mock == real).
        bool geometry_streams_feature_enabled = false;
        // Descriptor indexing: the enabled kDIFeature* bits. Gates the per-binding
        // flag admission, UAB layouts/pools, and variable-count allocation.
        std::uint64_t descriptor_indexing_feature_bits = 0;
        // Vulkan 1.3 support: the enabled kVk13Feature* bits + the honest-1.3-device flag. The
        // bits gate each feature's usage shapes; the flag admits the core-1.3 command surface
        // (mock == real).
        std::uint64_t vk13_feature_bits = 0;
        bool vk13_device = false;
    };
    struct Pool {
        std::uint64_t device = 0;        // parent
        std::set<std::uint64_t> buffers; // live command buffers
    };
    // Per-command-buffer state: which pool/device it belongs to, whether it
    // has been recorded (queue_submit rejects a never-recorded buffer), and the set of
    // swapchain surfaces its recorded commands referenced (queue_submit locks their
    // union -- mirrored structurally; the mock has no GPU).
    struct CmdBuffer {
        std::uint64_t pool = 0;
        std::uint64_t device = 0;
        bool recorded = false;
        std::set<std::uint64_t> referenced_surfaces;   // queue_submit locks their slots
        std::set<std::uint64_t> referenced_swapchains; // invalidated when one is destroyed
        // Draw objects whose handles a recorded draw stream baked in (render pass /
        // framebuffer / image view / pipeline): destroying any of them invalidates this buffer, so
        // a later submit is refused rather than referencing a freed object.
        std::set<std::uint64_t> referenced_draw_objects;
    };
    struct Leaf {
        std::uint64_t device = 0; // parent
        LeafKind kind = LeafKind::Fence;
        // (GL/zink): timeline-semaphore state (Semaphore leaves only). The mock models a
        // timeline as a monotonic counter advanced by signal_semaphore + a queue_submit's signal
        // values; wait/get read it. is_timeline distinguishes it from a binary semaphore.
        bool is_timeline = false;
        std::uint64_t timeline_value = 0;
        // Fence leaves carry signalled state so vkGetFenceStatus is honest -- set by
        // VK_FENCE_CREATE_SIGNALED_BIT at create, by a queue_submit that carries the fence, cleared
        // by vkResetFences. VK_SUCCESS when signaled, VK_NOT_READY when not.
        bool is_signaled = false;
        // Event leaves carry set state so vkGetEventStatus is honest -- set by vkSetEvent
        // or vkCmdSetEvent, cleared by vkResetEvent or vkCmdResetEvent. VK_EVENT_SET /
        // VK_EVENT_RESET.
        bool is_set = false;
    };
    struct Surface {
        std::uint64_t instance = 0;         // parent
        std::set<std::uint64_t> swapchains; // live swapchains targeting this surface
        // geometry-dirty latch (the real backend sets this from a Win32 WM_SIZE; the
        // mock sets it via debug_mark_surface_dirty). While dirty, acquire/present return
        // OUT_OF_DATE without "calling the driver"; create_swapchain clears it.
        bool geometry_dirty = false;
        bool shown = false; // flips on the first successful present (show-on-first-present)
        // Born-correlated guest-window topology (advisory role_hint).
        std::string platform;
        std::uint64_t xid = 0;
        std::string role_hint = "UnknownPending";
    };
    struct Swapchain {
        std::uint64_t device = 0;  // parent
        std::uint64_t surface = 0; // the surface it targets
        // Presentable images, minted on the first get_swapchain_images and returned
        // stably thereafter (the count derives from the requested min_image_count).
        std::vector<std::uint64_t> images;
        int image_count = 0;
        int next_image = 0;        // round-robin index handed out by acquire_next_image
        bool transfer_dst = false; // image_usage included TRANSFER_DST (clear support)
        int image_format = 0;      // the requested format (an image view must match it)
        int width = 0;             // requested extent (a framebuffer must match it)
        int height = 0;
        std::set<std::uint64_t>
            image_views; // live views over this swapchain's images (destroy order)
    };
    // A swapchain image resolvable by handle: record_command_buffer resolves
    // a referenced image to its device (for the same-device check), its surface (for the
    // referenced-surface set), and whether it supports TRANSFER_DST (clear validation).
    struct Image {
        std::uint64_t device = 0;
        std::uint64_t swapchain = 0;
        std::uint64_t surface = 0;
        bool transfer_dst = false;
        int format = 0; // the swapchain's format (create_image_view must match)
        // App-created images. app_created distinguishes these from swapchain images
        // (swapchain == 0 for them); aspect is the image's natural aspect (COLOR texture / DEPTH);
        // bind_image_memory ties memory like a buffer (alignment/type_bits/range enforced).
        bool app_created = false;
        int aspect = 0; // kImageAspectColor | kImageAspectDepth
        int tiling = 0;
        std::uint64_t usage = 0;
        int width = 0;
        int height = 0;
        // Mip/layer counts (create-carried) -- the copy_buffer_to_image widening validates each
        // region's subresource + mip-extent bounds against these (mock == real).
        int mip_levels = 1;
        int array_layers = 1;
        std::uint64_t alignment = 0; // the memreqs the response advertised (bind enforces it)
        std::uint64_t memory_type_bits = 0; // ditto (bind enforces type compatibility)
        std::uint64_t bound_memory = 0;     // 0 until bind_image_memory
        std::uint64_t bound_offset = 0;
        // Live image views over this app image: an app image's
        // destroy is blocked while any view references it -- the same parent/child rule as
        // swapchain -> view.
        std::set<std::uint64_t> image_views;
    };
    // Draw-surface objects. Each is a device child (blocks the device's destroy until
    // freed -- the one ordering Vulkan strictly enforces, alongside image-view-before-swapchain).
    // The framebuffer/pipeline reference fields are recorded for parallelism with the real backend
    // and for create-time validation, not for extra destroy blocking (real Vulkan does not block a
    // render-pass/layout destroy on a live framebuffer/pipeline -- only on in-flight use).
    struct ImageView {
        std::uint64_t device = 0;
        std::uint64_t image = 0;     // the swapchain image it views
        std::uint64_t swapchain = 0; // a swapchain blocks its destroy on live views over its images
        // (dynamic rendering): the view's VkFormat (create-carried). begin_rendering
        // derives the active DR scope's attachment formats from its attachment views, and draw
        // validation compares them to the bound pipeline's declared formats (the DR compatibility
        // key). 0 = UNDEFINED (an omitting/legacy peer round-trips unchanged).
        int format = 0;
    };
    // (GL/zink): a texel buffer view. The mock tracks only device + source buffer for
    // destroy bookkeeping (the buffer blocks its destroy on a live view).
    struct BufferView {
        std::uint64_t device = 0;
        std::uint64_t buffer = 0;
    };
    struct ShaderModule {
        std::uint64_t device = 0;
    };
    struct RenderPass {
        std::uint64_t device = 0;
        int color_format = 0; // the single color attachment's format (framebuffer view must match)
        int depth_format = 0; // the depth attachment's format (0 = no depth attachment)
        // MRT: the subpass's full color-ref vector (empty = a legacy single-color pass) + the
        // positional attachment formats, so framebuffer-view and pipeline-blend validation compare
        // against the REAL subpass shape (incl. UNUSED holes). Mirrors RealRenderPass.
        std::vector<ColorRefDesc> color_refs;
        std::vector<int> attachment_formats;
        // Required-feature audit: the subpass viewMask (0 = non-multiview). A
        // framebuffer built for a view_mask != 0 pass validates its attachment images cover
        // multiview_required_layers(view_mask). Mirrors RealRenderPass.view_mask.
        int view_mask = 0;
    };
    struct Framebuffer {
        std::uint64_t device = 0;
        std::uint64_t render_pass = 0; // validated same-device + format-compatible at create
        // Render-pass COMPATIBILITY key, SNAPSHOTTED from the creating pass at create -- Vulkan
        // lets the creating pass be destroyed and the framebuffer begun with any COMPATIBLE pass
        // (mpv/libplacebo does), so begin never resolves the creating pass by handle (mock ==
        // real).
        int compat_color_format = 0;
        int compat_depth_format = 0;
        std::uint64_t image_view = 0;       // validated same-device + format-match at create
        std::uint64_t depth_image_view = 0; // the depth attachment (0 = none)
        int width = 0; // == the swapchain extent; begin_render_pass clamps to it
        int height = 0;
        bool imageless = false;
        int attachment_count = 0;
        // MRT: the positional view vector (empty = legacy scalar shape). Mirrors RealFramebuffer.
        std::vector<std::uint64_t> attachment_views;
    };
    struct PipelineLayout {
        std::uint64_t device = 0;
        std::vector<std::uint64_t> set_layouts; // ordered descriptor set layouts (empty
                                                // = the empty layout)
    };
    struct Pipeline {
        std::uint64_t device = 0;
        std::uint64_t layout = 0;      // validated same-device at create
        std::uint64_t render_pass = 0; // validated same-device at create (0 for compute + DR)
        int vertex_binding_count = 0;  // bindings the draw must have bound (0 = bufferless)
        bool has_depth = false;        // declared depth-stencil state (render pass must match)
        bool compute = false;          // the pipeline's KIND -- bind point must match
        // (dynamic rendering): a DR pipeline (render_pass == 0 + these formats) whose
        // draw-time compatibility key is the FORMATS, not a render pass. dynamic_rendering
        // distinguishes it from a compute pipeline (also render_pass == 0). The color-format vector
        // + depth/stencil formats + view mask must equal the active dynamic-rendering scope at
        // draw.
        bool dynamic_rendering = false;
        int view_mask = 0;
        std::vector<int> color_formats;
        int depth_format = 0;
        int stencil_format = 0;
    };
    // Host-visible memory + buffers. MemoryObject backs the worker's "real" bytes that
    // write_memory_ranges scatters into (so a test can assert the upload landed); Buffer is a
    // device child that a bind ties to a memory + offset.
    struct MemoryObject {
        std::uint64_t device = 0;
        std::uint64_t size = 0;
        std::uint32_t type_index = 0;
        std::uint64_t property_flags = 0;
        // (bufferDeviceAddress): the admitted VkMemoryAllocateFlagsInfo.flags (0 = none).
        // bind_buffer_memory enforces VUID 03339 against it: a SHADER_DEVICE_ADDRESS buffer may
        // only bind to DEVICE_ADDRESS-allocated memory.
        std::uint64_t allocate_flags = 0;
        std::vector<std::byte> bytes; // sized lazily on first write_memory_ranges
    };
    struct Buffer {
        std::uint64_t device = 0;
        std::uint64_t size = 0;
        std::uint64_t usage = 0;
        std::uint64_t alignment = 0; // the memreqs the response advertised (bind enforces it)
        std::uint64_t memory_type_bits = 0; // ditto (bind enforces type compatibility)
        std::uint64_t bound_memory = 0;     // 0 until bind_buffer_memory
        std::uint64_t bound_offset = 0;
    };
    // Descriptor surface objects. All UNIFORM_BUFFER. A set layout + pool + pipeline
    // layout are device children (block the device's destroy until freed); a set is a pool child
    // (dies with the pool). A set layout's destroy is also blocked while a live pipeline layout
    // references it or a live set was allocated from it (handle-graph totality).
    struct DescriptorSetLayoutBinding {
        int binding = 0;
        int descriptor_type = 0;   // UNIFORM_BUFFER | COMBINED_IMAGE_SAMPLER (write/draw key)
        int descriptor_count = 0;  // array size of this binding
        long long stage_flags = 0; // recorded for parallelism with the real backend
        // descriptorIndexing: PERSISTED (the older mock dropped these -- the
        // mock != real gap). The readiness/dangle rules key on them.
        long long binding_flags = 0;
    };
    struct DescriptorSetLayout {
        std::uint64_t device = 0;
        long long layout_flags = 0; // descriptorIndexing: persisted (UPDATE_AFTER_BIND_POOL)
        std::vector<DescriptorSetLayoutBinding> bindings;
    };
    struct DescriptorPool {
        std::uint64_t device = 0;
        int max_sets = 0;       // VkDescriptorPoolCreateInfo.maxSets
        int sets_remaining = 0; // decremented per allocated set (per-type budget is the pool's job)
        // descriptorIndexing: created with UPDATE_AFTER_BIND (a UAB-pool layout's sets must
        // come from such a pool -- the allocate-time gate, mock == real).
        bool update_after_bind = false;
        std::set<std::uint64_t>
            sets; // live sets allocated from this pool (cascade-freed on destroy)
    };
    // One descriptor slot's state: initialized (a successful write landed) + which buffer it
    // currently references (the dynamic CB -> set -> buffer link consulted on destroy_buffer).
    // A repoint updates `buffer`; a destroy of the referenced buffer marks
    // the slot uninitialized (dangling) + invalidates any recorded CB that bound this set.
    // One descriptor slot's state. A UNIFORM_BUFFER slot tracks the buffer it currently references;
    // a COMBINED_IMAGE_SAMPLER slot tracks the {sampler, image_view} pair. The
    // unused fields stay 0. Destroying the CURRENT referent (buffer | sampler | image_view) marks
    // the slot uninitialized (dangling) + invalidates any recorded CB that bound this set -- the CB
    // -> set -> {buffer | sampler | image-view} destroy consult, extending the buffer-only check.
    struct DescriptorSlot {
        bool initialized = false;
        std::uint64_t buffer = 0;      // buffer-type referent
        std::uint64_t sampler = 0;     // sampler / combined-image-sampler referent
        std::uint64_t image_view = 0;  // image-type referent
        std::uint64_t buffer_view = 0; // (GL/zink): texel-buffer referent
    };
    struct DescriptorSet {
        std::uint64_t device = 0;
        std::uint64_t pool = 0;
        std::uint64_t layout =
            0; // the exact set layout it was allocated from (bind/draw exactness)
        std::map<int, std::vector<DescriptorSlot>> slots; // binding -> per-array-element slot state
        // descriptorIndexing: the ALLOCATED variable count of the last binding (1 = the
        // layout has no variable-count binding). The variable binding's slot vector is sized to
        // THIS, not the layout max -- so readiness + write bounds judge the allocated count.
        long long variable_count = -1;
    };
    // Sampler: a device-child leaf (blocks the device's destroy until freed).
    // The bounded subset's parameters are recorded for parallelism with the real backend; the only
    // stateful role here is the combined-image-sampler destroy-consult (destroy_sampler walks set
    // slots).
    struct Sampler {
        std::uint64_t device = 0;
    };
    // Query pool (GL 3.3 / occlusion / xfb queries): the type/count so recorded query commands can
    // range-check the query index (mock == real).
    struct QueryPool {
        std::uint64_t device = 0;
        int type = 0; // VkQueryType
        std::uint32_t count = 0;
    };

    protocol::GpuDevice device_;
    bool have_device_ = false;
    display::DisplayLayout display_layout_;
    bool has_display_layout_ = false;
    std::uint64_t next_handle_ = 1; // 0 is reserved for null
    std::map<std::uint64_t, Instance> instances_;
    std::map<std::uint64_t, Device> devices_;
    std::map<std::uint64_t, Pool> pools_;
    std::map<std::uint64_t, CmdBuffer> command_buffers_;     // command buffer handle -> state
    std::map<std::uint64_t, std::uint64_t> queue_to_device_; // queue handle -> owning device
    std::map<std::uint64_t, Leaf> leaves_;
    std::map<std::uint64_t, Surface> surfaces_;
    sidecar::WindowRegistry registry_; // worker-owned XID registry shared by both planes
    bool sidecar_ready_ = false;       // the sidecar emitted its readiness barrier
    // guards the cross-plane state (registry_ + mock_placeholders_), which the app data
    // plane (create/destroy_surface) and the sidecar plane (register/update/unregister_toplevel)
    // both touch. Kept short; the mock has no window thread so no blocking work runs under it.
    std::mutex backend_mutex_;
    std::set<std::uint64_t> mock_placeholders_; // the mock executor's live placeholder ids
    // the synthetic chrome buffer per xid (top-down BGRA8, stride == w*4) -- the mock's
    // stand-in for the real backend's window-thread DIB, so DebugChromeState can sample what was
    // painted. Re-sized when the source size changes; a dirty rect is composited at its offset.
    struct MockChrome {
        std::uint32_t w = 0;
        std::uint32_t h = 0;
        std::vector<unsigned char> bgra;
    };
    std::map<std::uint64_t, MockChrome> mock_chrome_;
    // the synthetic cursor per xid (the mock's stand-in for the real backend's
    // HCURSOR), so DebugCursorState can sample what set_cursor built. BGRA8, stride == w*4.
    struct MockCursor {
        std::uint32_t w = 0;
        std::uint32_t h = 0;
        std::int32_t xhot = 0;
        std::int32_t yhot = 0;
        std::vector<unsigned char> bgra;
    };
    std::map<std::uint64_t, MockCursor> mock_cursors_;
    // the mock's stand-in for the real backend's window-thread geometry cell (it has no
    // HWND). Records the last-APPLIED sidecar-authored move geometry + its seq per xid, so
    // debug_enum_windows(include_actual) reports the converged position -- mock == real for the
    // authored x/y + seq (the only thing a move changes; the mock does not model the host
    // client extent, which resize will). Erased when the placeholder/representation is torn
    // down.
    struct MockGeometry {
        std::int32_t x = 0;
        std::int32_t y = 0;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::uint64_t seq = 0;
    };
    std::map<std::uint64_t, MockGeometry> mock_geometry_;
    // the per-session input ring (the worker's WndProc stand-in fills it via debug_-
    // enqueue_input; poll_input drains it). Its own dedicated mutex (inside InputQueue), NOT
    // backend_mutex_, so the input plane never contends with the registry/data planes.
    sidecar::InputQueue input_queue_;
    std::map<std::uint64_t, Swapchain> swapchains_;
    std::map<std::uint64_t, Image> images_; // swapchain image handle -> resolvable metadata
    // Draw-surface object tables.
    std::map<std::uint64_t, ImageView> image_views_;
    std::map<std::uint64_t, BufferView> buffer_views_; // (GL/zink): texel buffer views
    std::map<std::uint64_t, ShaderModule> shader_modules_;
    std::map<std::uint64_t, RenderPass> render_passes_;
    std::map<std::uint64_t, Framebuffer> framebuffers_;
    std::map<std::uint64_t, PipelineLayout> pipeline_layouts_;
    std::map<std::uint64_t, Pipeline> pipelines_;
    // Host-visible memory + buffer tables. memory_objects_ is keyed by the same handle
    // as the Memory leaf (the leaf still drives device-destroy blocking); buffers_ are device
    // children.
    std::map<std::uint64_t, MemoryObject> memory_objects_;
    std::map<std::uint64_t, Buffer> buffers_;
    // Descriptor surface tables.
    std::map<std::uint64_t, DescriptorSetLayout> descriptor_set_layouts_;
    std::map<std::uint64_t, DescriptorPool> descriptor_pools_;
    std::map<std::uint64_t, DescriptorSet> descriptor_sets_;
    std::map<std::uint64_t, Sampler> samplers_;
    std::map<std::uint64_t, QueryPool> query_pools_; // query pools (GL 3.3 / queries)

    // The honest-but-canned memory-properties table the mock reports: type 0
    // DEVICE_LOCAL, type 1 HOST_VISIBLE|HOST_COHERENT (the mappable type a VBO/UBO binds), over two
    // heaps.
    std::vector<MemoryType> mock_memory_types() const;
    std::vector<MemoryHeap> mock_memory_heaps() const;

    // Mints (idempotently) the swapchain's image handles and registers each in images_.
    void ensure_swapchain_images(std::uint64_t swapchain_handle, Swapchain& sc);

    // Invalidates any recorded command buffer that baked in `handle` (a draw object being
    // destroyed), so a later queue_submit refuses it instead of referencing a freed object.
    void invalidate_cbs_referencing(std::uint64_t handle);

    // The CB -> set -> {buffer | sampler | image-view} destroy consult (for buffers,
    // extended to sampler/image-view referents). Marks any descriptor slot currently
    // referencing `resource` dangling (uninitialized), and invalidates any recorded command buffer
    // that baked the owning set -- so a later submit is refused rather than executing a set that
    // points at a freed resource. Handles are monotonic + unique across object kinds, so matching
    // by value cannot confuse a buffer for a sampler/view. Called by destroy_buffer /
    // destroy_image_view / destroy_sampler.
    void dangle_sets_referencing(std::uint64_t resource);

    // True if `set` (on `device`) is draw-ready: every slot its layout requires is
    // initialized and still references a live, same-device buffer with UNIFORM_BUFFER usage. On
    // false sets `reason`.
    bool descriptor_set_draw_ready(std::uint64_t set, std::uint64_t device,
                                   std::string& reason) const;
};

// Serves Vulkan RPCs on `channel` until the peer disconnects (clean EOF) or the
// connection errors. Each request is dispatched to `backend`; an unrecognized
// opcode gets an RpcStatus::UnknownOp reply (the stream stays usable). Returns
// normally on EOF.
void serve_vulkan_rpc(RpcChannel& channel, VulkanBackend& backend);

// the record handler's internal phase split (validate vs replay) is timed
// INSIDE the backends, which need the same per-process profile the RPC hooks use. Null unless
// VKRELAY2_PROFILE=1 -- callers must null-check and keep the off-path free of clock reads.
struct RpcProfile;
RpcProfile* profile_if_enabled();
std::uint64_t profile_clock_us(); // steady_clock microseconds (the profile's time base)

// Client helpers: each sends a request (correlated by request_id) and returns
// the decoded response. They throw transport::TransportError on EOF, a transport
// error, a mismatched correlation id, or an RPC-layer status that is not Ok; a
// body-level !ok (a semantic failure) is returned, not thrown.
CapabilitiesResponse negotiate_capabilities(RpcChannel& channel, std::uint32_t request_id,
                                            const CapabilitiesRequest& req);
CreateInstanceResponse create_instance(RpcChannel& channel, std::uint32_t request_id,
                                       const CreateInstanceRequest& req);
EnumeratePhysicalDevicesResponse
enumerate_physical_devices(RpcChannel& channel, std::uint32_t request_id,
                           const EnumeratePhysicalDevicesRequest& req);
CreateDeviceResponse create_device(RpcChannel& channel, std::uint32_t request_id,
                                   const CreateDeviceRequest& req);
StatusResponse destroy_device(RpcChannel& channel, std::uint32_t request_id,
                              const HandleRequest& req);
StatusResponse destroy_instance(RpcChannel& channel, std::uint32_t request_id,
                                const HandleRequest& req);
GetDeviceQueueResponse get_device_queue(RpcChannel& channel, std::uint32_t request_id,
                                        const GetDeviceQueueRequest& req);
CreateCommandPoolResponse create_command_pool(RpcChannel& channel, std::uint32_t request_id,
                                              const CreateCommandPoolRequest& req);
StatusResponse destroy_command_pool(RpcChannel& channel, std::uint32_t request_id,
                                    const HandleRequest& req);
AllocateCommandBuffersResponse allocate_command_buffers(RpcChannel& channel,
                                                        std::uint32_t request_id,
                                                        const AllocateCommandBuffersRequest& req);
StatusResponse free_command_buffers(RpcChannel& channel, std::uint32_t request_id,
                                    const FreeCommandBuffersRequest& req);
CreateFenceResponse create_fence(RpcChannel& channel, std::uint32_t request_id,
                                 const CreateFenceRequest& req);
StatusResponse destroy_fence(RpcChannel& channel, std::uint32_t request_id,
                             const HandleRequest& req);
CreateSemaphoreResponse create_semaphore(RpcChannel& channel, std::uint32_t request_id,
                                         const CreateSemaphoreRequest& req);
StatusResponse destroy_semaphore(RpcChannel& channel, std::uint32_t request_id,
                                 const HandleRequest& req);
AllocateMemoryResponse allocate_memory(RpcChannel& channel, std::uint32_t request_id,
                                       const AllocateMemoryRequest& req);
StatusResponse free_memory(RpcChannel& channel, std::uint32_t request_id, const HandleRequest& req);
// (bufferDeviceAddress): the real host VkDeviceAddress verbatim.
GetBufferDeviceAddressResponse get_buffer_device_address(RpcChannel& channel,
                                                         std::uint32_t request_id,
                                                         const GetBufferDeviceAddressRequest& req);
CreateSurfaceResponse create_surface(RpcChannel& channel, std::uint32_t request_id,
                                     const CreateSurfaceRequest& req);
StatusResponse destroy_surface(RpcChannel& channel, std::uint32_t request_id,
                               const HandleRequest& req);
CreateSwapchainResponse create_swapchain(RpcChannel& channel, std::uint32_t request_id,
                                         const CreateSwapchainRequest& req);
StatusResponse destroy_swapchain(RpcChannel& channel, std::uint32_t request_id,
                                 const HandleRequest& req);
GetSwapchainImagesResponse get_swapchain_images(RpcChannel& channel, std::uint32_t request_id,
                                                const GetSwapchainImagesRequest& req);
AcquireNextImageResponse acquire_next_image(RpcChannel& channel, std::uint32_t request_id,
                                            const AcquireNextImageRequest& req);
QueuePresentResponse queue_present(RpcChannel& channel, std::uint32_t request_id,
                                   const QueuePresentRequest& req);
StatusResponse record_command_buffer(RpcChannel& channel, std::uint32_t request_id,
                                     const RecordCommandBufferRequest& req);
// the same request over RpcOp::RecordCommandBufferRaw (binary body via
// to_wire; JSON response). Callers use it only when DeviceCaps.raw_record was advertised.
StatusResponse record_command_buffer_raw(RpcChannel& channel, std::uint32_t request_id,
                                         const RecordCommandBufferRequest& req);
QueueSubmitResponse queue_submit(RpcChannel& channel, std::uint32_t request_id,
                                 const QueueSubmitRequest& req);
QueueSubmitResponse queue_submit2(RpcChannel& channel, std::uint32_t request_id,
                                  const QueueSubmit2Request& req);
StatusResponse reset_fences(RpcChannel& channel, std::uint32_t request_id,
                            const ResetFencesRequest& req);
WaitForFencesResponse wait_for_fences(RpcChannel& channel, std::uint32_t request_id,
                                      const WaitForFencesRequest& req);
// (core-1.0 sync honesty): fence status poll + the VkEvent object model.
GetFenceStatusResponse get_fence_status(RpcChannel& channel, std::uint32_t request_id,
                                        const HandleRequest& req);
CreateEventResponse create_event(RpcChannel& channel, std::uint32_t request_id,
                                 const CreateEventRequest& req);
StatusResponse destroy_event(RpcChannel& channel, std::uint32_t request_id,
                             const HandleRequest& req);
GetEventStatusResponse get_event_status(RpcChannel& channel, std::uint32_t request_id,
                                        const HandleRequest& req);
StatusResponse set_event(RpcChannel& channel, std::uint32_t request_id, const HandleRequest& req);
StatusResponse reset_event(RpcChannel& channel, std::uint32_t request_id, const HandleRequest& req);
WaitSemaphoresResponse wait_semaphores(RpcChannel& channel, std::uint32_t request_id,
                                       const WaitSemaphoresRequest& req);
StatusResponse signal_semaphore(RpcChannel& channel, std::uint32_t request_id,
                                const SignalSemaphoreRequest& req);
GetSemaphoreCounterValueResponse
get_semaphore_counter_value(RpcChannel& channel, std::uint32_t request_id,
                            const GetSemaphoreCounterValueRequest& req);
// REAL idle waits.
WaitIdleResponse queue_wait_idle(RpcChannel& channel, std::uint32_t request_id,
                                 const HandleRequest& req);
WaitIdleResponse device_wait_idle(RpcChannel& channel, std::uint32_t request_id,
                                  const HandleRequest& req);
GetSurfaceCapabilitiesResponse get_surface_capabilities(RpcChannel& channel,
                                                        std::uint32_t request_id,
                                                        const GetSurfaceCapabilitiesRequest& req);
GetSurfaceFormatsResponse get_surface_formats(RpcChannel& channel, std::uint32_t request_id,
                                              const GetSurfaceFormatsRequest& req);
GetSurfacePresentModesResponse get_surface_present_modes(RpcChannel& channel,
                                                         std::uint32_t request_id,
                                                         const GetSurfacePresentModesRequest& req);
GetSurfaceSupportResponse get_surface_support(RpcChannel& channel, std::uint32_t request_id,
                                              const GetSurfaceSupportRequest& req);
// Draw surface. create_shader_module sends a BINARY body (req.to_wire); the
// rest are JSON like every other op.
CreateImageViewResponse create_image_view(RpcChannel& channel, std::uint32_t request_id,
                                          const CreateImageViewRequest& req);
StatusResponse destroy_image_view(RpcChannel& channel, std::uint32_t request_id,
                                  const HandleRequest& req);
CreateBufferViewResponse create_buffer_view(RpcChannel& channel, std::uint32_t request_id,
                                            const CreateBufferViewRequest& req);
StatusResponse destroy_buffer_view(RpcChannel& channel, std::uint32_t request_id,
                                   const HandleRequest& req);
CreateShaderModuleResponse create_shader_module(RpcChannel& channel, std::uint32_t request_id,
                                                const CreateShaderModuleRequest& req);
StatusResponse destroy_shader_module(RpcChannel& channel, std::uint32_t request_id,
                                     const HandleRequest& req);
CreateRenderPassResponse create_render_pass(RpcChannel& channel, std::uint32_t request_id,
                                            const CreateRenderPassRequest& req);
StatusResponse destroy_render_pass(RpcChannel& channel, std::uint32_t request_id,
                                   const HandleRequest& req);
CreateFramebufferResponse create_framebuffer(RpcChannel& channel, std::uint32_t request_id,
                                             const CreateFramebufferRequest& req);
StatusResponse destroy_framebuffer(RpcChannel& channel, std::uint32_t request_id,
                                   const HandleRequest& req);
CreatePipelineLayoutResponse create_pipeline_layout(RpcChannel& channel, std::uint32_t request_id,
                                                    const CreatePipelineLayoutRequest& req);
StatusResponse destroy_pipeline_layout(RpcChannel& channel, std::uint32_t request_id,
                                       const HandleRequest& req);
CreateGraphicsPipelinesResponse
create_graphics_pipelines(RpcChannel& channel, std::uint32_t request_id,
                          const CreateGraphicsPipelinesRequest& req);
CreateComputePipelinesResponse create_compute_pipelines(RpcChannel& channel,
                                                        std::uint32_t request_id,
                                                        const CreateComputePipelinesRequest& req);
StatusResponse destroy_pipeline(RpcChannel& channel, std::uint32_t request_id,
                                const HandleRequest& req);
// Host-visible memory + buffers. write_memory_ranges sends a BINARY body
// (req.to_wire()); the rest are JSON like every other op.
GetPhysicalDeviceMemoryPropertiesResponse
get_physical_device_memory_properties(RpcChannel& channel, std::uint32_t request_id,
                                      const GetPhysicalDeviceMemoryPropertiesRequest& req);
CreateBufferResponse create_buffer(RpcChannel& channel, std::uint32_t request_id,
                                   const CreateBufferRequest& req);
StatusResponse destroy_buffer(RpcChannel& channel, std::uint32_t request_id,
                              const HandleRequest& req);
StatusResponse bind_buffer_memory(RpcChannel& channel, std::uint32_t request_id,
                                  const BindBufferMemoryRequest& req);
StatusResponse write_memory_ranges(RpcChannel& channel, std::uint32_t request_id,
                                   const WriteMemoryRangesRequest& req);
ReadMemoryRangesResponse read_memory_ranges(RpcChannel& channel, std::uint32_t request_id,
                                            const ReadMemoryRangesRequest& req);
// Descriptor surface + per-frame UBO. All JSON like every other op.
CreateDescriptorSetLayoutResponse
create_descriptor_set_layout(RpcChannel& channel, std::uint32_t request_id,
                             const CreateDescriptorSetLayoutRequest& req);
// descriptorIndexing: the honest layout-support query (RpcOp 88).
GetDescriptorSetLayoutSupportResponse
get_descriptor_set_layout_support(RpcChannel& channel, std::uint32_t request_id,
                                  const CreateDescriptorSetLayoutRequest& req);
// Vulkan 1.3 support (maintenance4): create-info-shaped memory-requirement queries (ops 89/90).
CreateBufferResponse get_device_buffer_memory_requirements(RpcChannel& channel,
                                                           std::uint32_t request_id,
                                                           const CreateBufferRequest& req);
CreateImageResponse get_device_image_memory_requirements(RpcChannel& channel,
                                                         std::uint32_t request_id,
                                                         const CreateImageRequest& req);
StatusResponse destroy_descriptor_set_layout(RpcChannel& channel, std::uint32_t request_id,
                                             const HandleRequest& req);
CreateDescriptorPoolResponse create_descriptor_pool(RpcChannel& channel, std::uint32_t request_id,
                                                    const CreateDescriptorPoolRequest& req);
StatusResponse destroy_descriptor_pool(RpcChannel& channel, std::uint32_t request_id,
                                       const HandleRequest& req);
AllocateDescriptorSetsResponse allocate_descriptor_sets(RpcChannel& channel,
                                                        std::uint32_t request_id,
                                                        const AllocateDescriptorSetsRequest& req);
StatusResponse update_descriptor_sets(RpcChannel& channel, std::uint32_t request_id,
                                      const UpdateDescriptorSetsRequest& req);
// Textures + depth for literal vkcube. All JSON like every other op.
GetPhysicalDeviceFormatPropertiesResponse
get_physical_device_format_properties(RpcChannel& channel, std::uint32_t request_id,
                                      const GetPhysicalDeviceFormatPropertiesRequest& req);
// (GL/zink).
GetPhysicalDeviceFeaturesResponse
get_physical_device_features(RpcChannel& channel, std::uint32_t request_id,
                             const GetPhysicalDeviceFeaturesRequest& req);
GetPhysicalDeviceImageFormatPropertiesResponse get_physical_device_image_format_properties(
    RpcChannel& channel, std::uint32_t request_id,
    const GetPhysicalDeviceImageFormatPropertiesRequest& req);
GetPhysicalDevicePropertiesResponse
get_physical_device_properties(RpcChannel& channel, std::uint32_t request_id,
                               const GetPhysicalDevicePropertiesRequest& req);
GetPhysicalDeviceCapabilityChainResponse
get_physical_device_capability_chain(RpcChannel& channel, std::uint32_t request_id,
                                     const GetPhysicalDeviceCapabilityChainRequest& req);
CreateImageResponse create_image(RpcChannel& channel, std::uint32_t request_id,
                                 const CreateImageRequest& req);
StatusResponse destroy_image(RpcChannel& channel, std::uint32_t request_id,
                             const HandleRequest& req);
StatusResponse bind_image_memory(RpcChannel& channel, std::uint32_t request_id,
                                 const BindImageMemoryRequest& req);
// Sampler support. All JSON like every other op.
CreateSamplerResponse create_sampler(RpcChannel& channel, std::uint32_t request_id,
                                     const CreateSamplerRequest& req);
StatusResponse destroy_sampler(RpcChannel& channel, std::uint32_t request_id,
                               const HandleRequest& req);
// Query pools (GL 3.3 / occlusion / xfb queries).
CreateQueryPoolResponse create_query_pool(RpcChannel& channel, std::uint32_t request_id,
                                          const CreateQueryPoolRequest& req);
StatusResponse destroy_query_pool(RpcChannel& channel, std::uint32_t request_id,
                                  const HandleRequest& req);
GetQueryPoolResultsResponse get_query_pool_results(RpcChannel& channel, std::uint32_t request_id,
                                                   const GetQueryPoolResultsRequest& req);
StatusResponse reset_query_pool(RpcChannel& channel, std::uint32_t request_id,
                                const ResetQueryPoolRequest& req);

} // namespace vkr::vkrpc

#endif // VKRELAY2_COMMON_VKRPC_VULKAN_SESSION_HPP
