// Vulkan RPC envelope for the app<->worker data plane.
//
// After the data-plane handshake (app_hello / app_ack), the app and worker stop
// speaking JSON control messages and switch to this binary RPC envelope. Each
// RPC rides one length-prefixed frame (wire.hpp); the frame payload is a fixed
// 12-byte binary header [u32 op][u32 request_id][u32 status] followed by an
// opaque body. The body is JSON for now (small, structured capability
// data); bulk Vulkan object/buffer transfers get binary bodies later,
// which is exactly why the opcode/correlation header is binary rather than a
// JSON control message.
#ifndef VKRELAY2_COMMON_VKRPC_RPC_HPP
#define VKRELAY2_COMMON_VKRPC_RPC_HPP

#include "common/transport/transport.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

namespace vkr::vkrpc {

// Opcode space is forward-compatible: an unrecognized op is NOT a decode error
// -- the server replies RpcStatus::UnknownOp so new ops can be added without
// breaking old peers.
enum class RpcOp : std::uint32_t {
    Invalid = 0,
    NegotiateCapabilities = 1,
    // Instance/device lifecycle.
    CreateInstance = 2,
    EnumeratePhysicalDevices = 3,
    CreateDevice = 4,
    DestroyDevice = 5,
    DestroyInstance = 6,
    // Device-child command objects.
    GetDeviceQueue = 7,
    CreateCommandPool = 8,
    DestroyCommandPool = 9,
    AllocateCommandBuffers = 10,
    FreeCommandBuffers = 11,
    // Device-child sync + memory objects.
    CreateFence = 12,
    DestroyFence = 13,
    // `CreateSemaphoreOp` (not `CreateSemaphore`) because windows.h #defines CreateSemaphore /
    // CreateEvent as A/W macros: an enumerator named `CreateSemaphore` would be macro-mangled to
    // `CreateSemaphoreW` in any TU that saw windows.h first, and a matching
    // `RpcOp::CreateSemaphore` use in a NON-mangled TU would then fail to resolve. The `Op` suffix
    // removes that class of include-order fragility entirely -- the wire
    // VALUE (14/81) is unchanged, so this is a pure source rename. Same reason for CreateEventOp
    // below.
    CreateSemaphoreOp = 14,
    DestroySemaphore = 15,
    AllocateMemory = 16,
    FreeMemory = 17,
    // Presentation spine: surface (instance child) + swapchain (device child,
    // referencing a surface). Create/destroy only for now; image acquire/present
    // is a follow-up.
    CreateSurface = 18,
    DestroySurface = 19,
    CreateSwapchain = 20,
    DestroySwapchain = 21,
    // Presentation data plane: the swapchain's images. Acquire /
    // presentation is handled separately; this only enumerates the images so the app can
    // record per-image command buffers. Real WSI lands behind this op in the worker.
    GetSwapchainImages = 22,
    // Presentation data plane: acquire an image, present it. The
    // window shows on first successful present. AcquireNextImage / QueuePresent obey
    // the geometry-dirty latch (return VK_ERROR_OUT_OF_DATE_KHR without calling
    // the host driver while the surface is dirty).
    AcquireNextImage = 23,
    QueuePresent = 24,
    // RpcOp 25 (DebugRenderTestFrame) is RETIRED -- it was scaffolding (the
    // worker synthesizing a clear-color frame) that was replaced by real
    // app-recorded commands. The number is left a reserved gap, not reused, for
    // forward-compat hygiene.
    //
    // Command recording + queue submit: the app records a command
    // stream into a command buffer (the whole begin->[cmds]->end recording ships in one
    // message -- no per-vkCmd round-trip, the shape the original converged on), submits
    // it, and drives fence reset/wait. record_command_buffer + queue_submit extend the
    // window-thread serialization (they may touch swapchain-backed images): the
    // referenced surfaces' slot locks are held and the host calls run on the window
    // thread. queue_submit returns the honest VkResult and never synthesizes
    // OUT_OF_DATE -- the dirty latch stays an acquire/present contract.
    RecordCommandBuffer = 26,
    QueueSubmit = 27,
    ResetFences = 28,
    WaitForFences = 29,
    // App-facing WSI capability queries: a native Vulkan app queries
    // these before choosing a swapchain's format/present-mode/extent/usage. The worker runs
    // them against the host surface (the worker already did so internally inside
    // create_swapchain); exposing them keeps the app's view honest -- supportedUsageFlags
    // (incl. TRANSFER_DST), formats, and present modes are the real host values. Surface
    // capabilities report currentExtent = 0xFFFFFFFF (the dynamic-extent sentinel): the
    // app sizes the swapchain to its framebuffer and the host Win32 surface's concrete
    // extent stays behind the worker boundary.
    GetSurfaceCapabilities = 30,
    GetSurfaceFormats = 31,
    GetSurfacePresentModes = 32,
    GetSurfaceSupport = 33,
    // Draw surface: the bufferless-triangle draw spine. Image views (over
    // swapchain images), shader modules, render pass, framebuffer, pipeline layout, and the
    // graphics pipeline -- create/destroy. create_shader_module is the first op with a BINARY body
    // ([u32 json_len LE][json header][raw SPIR-V bytes]); all others stay JSON. The draw vkCmd
    // subset (begin/end render pass, bind pipeline, set viewport/scissor, draw) rides the existing
    // record_command_buffer envelope as new RecordedCommand kinds, NOT new opcodes. No buffers /
    // device memory / mapping / descriptors here -- those are (literal vkcube).
    CreateImageView = 34,
    DestroyImageView = 35,
    CreateShaderModule = 36,
    DestroyShaderModule = 37,
    CreateRenderPass = 38,
    DestroyRenderPass = 39,
    CreateFramebuffer = 40,
    DestroyFramebuffer = 41,
    CreatePipelineLayout = 42,
    DestroyPipelineLayout = 43,
    CreateGraphicsPipelines = 44,
    DestroyPipeline = 45,
    // Host-visible memory + buffers: the memory-visibility spine, proven with
    // a mapped vertex buffer. get_physical_device_memory_properties returns the honest host table
    // (the ICD caches it at enumerate-time so the void vkGetPhysicalDeviceMemoryProperties never
    // RPCs). create_buffer's response carries the real VkMemoryRequirements (same reason -- the
    // void getter is a cache copy). write_memory_ranges is a BINARY-bodied op ([u32 json_len
    // LE][json header][raw bytes]) that scatters the ICD's dirty shadow chunks into the worker's
    // real VkDeviceMemory; it is request/response (a failed upload fails vkQueueSubmit before any
    // child submit). allocate_memory / free_memory (16/17) are extended in place to back real host
    // allocations. The vkCmdBindVertexBuffers draw command rides the existing record_command_buffer
    // envelope as a new RecordedCommand kind.
    GetPhysicalDeviceMemoryProperties = 46,
    CreateBuffer = 47,
    DestroyBuffer = 48,
    BindBufferMemory = 49,
    WriteMemoryRanges = 50,
    // Descriptor surface + per-frame UBO: the descriptor object graph vkcube's
    // transform/uniform path needs. UNIFORM_BUFFER-only; combined-image-sampler and depth support
    // are described separately.
    // All JSON bodies (descriptor writes are structured handles + small ints, no bulk payload). The
    // update_descriptor_sets write carries no copy representation at all (structurally enforcing
    // copyCount == 0). vkCmdBindDescriptorSets rides the existing record_command_buffer envelope as
    // a new RecordedCommand kind (NOT an opcode), like bind_vertex_buffers. create_pipeline_layout
    // (42) is extended in place to carry a set-layout handle list; create_buffer (47) usage is
    // extended to admit UNIFORM_BUFFER. FreeDescriptorSets / ResetDescriptorPool are out of subset
    // (the pool owns set lifetime; vkcube destroys the pool). The new client-side stateful logic is
    // the update_descriptor_sets validation (validate-then-apply against the recorded layout) --
    // the UBO bytes themselves still use the existing write_memory_ranges path unchanged.
    CreateDescriptorSetLayout = 51,
    DestroyDescriptorSetLayout = 52,
    CreateDescriptorPool = 53,
    DestroyDescriptorPool = 54,
    AllocateDescriptorSets = 55,
    UpdateDescriptorSets = 56,
    // Textures + depth = literal vkcube. The two pieces vkcube needs that
    // (host-visible memory/buffer) and (descriptors/UBO) did not: a sampled texture
    // (combined-image-sampler) and a depth buffer. get_physical_device_format_properties is honest
    // (the ICD memoizes per format; the void vkGetPhysicalDeviceFormatProperties is a cache copy --
    // currently a zeroed stub that would make vkcube ERR_EXIT when it probes its texture-upload
    // path). create_image's response carries the real VkMemoryRequirements (the void
    // vkGetImageMemoryRequirements is a cache copy, the create_buffer pattern) and, for LINEAR
    // tiling, the real VkSubresourceLayout (so vkGetImageSubresourceLayout is honest). A
    // device-local-only (never-mapped) memory class complements coherent host-visible memory for
    // depth images. Samplers, combined-image-sampler descriptors, and texture-upload commands
    // (vkCmdCopyBufferToImage plus generalized vkCmdPipelineBarrier records) use the same existing
    // descriptor and command-buffer opcodes. Ops 57-60 provide format properties and the
    // image/memory/depth spine.
    GetPhysicalDeviceFormatProperties = 57,
    CreateImage = 58,
    DestroyImage = 59,
    BindImageMemory = 60,
    // Sampler support. create_sampler / destroy_sampler are the bounded
    // sampler subset (NEAREST, CLAMP_TO_EDGE, no anisotropy/compare/LOD) the combined-image-sampler
    // descriptor binds. The combined-image-sampler descriptor TYPE itself + the texture-upload
    // commands (vkCmdCopyBufferToImage + the generalized vkCmdPipelineBarrier) need NO new opcode:
    // the descriptor surface ops (51/53/56) are extended in place, and the upload commands ride
    // record_command_buffer (26) as new RecordedCommand kinds, exactly like bind_vertex_buffers /
    // bind_descriptor_sets. create_buffer (47) usage is extended in place to admit TRANSFER_SRC
    // (the staging buffer).
    CreateSampler = 61,
    DestroySampler = 62,
    // (GL/zink bring-up): honest physical-device capability forwarding so a full
    // GL->Vulkan driver (zink) can build a device + GLX configs against the relay. The vkcube
    // subset exposed only Features (zeroed) + 1.0 FormatProperties; zink needs the real host
    // features and the 64-bit VkFormatProperties3 (carried alongside the 1.0 flags in op 57's
    // response) + real image-format support. create_device (op 4) is extended in place to forward
    // the app's enabled device extensions + requested features so the worker enables them on the
    // real device.
    GetPhysicalDeviceFeatures = 63,
    GetPhysicalDeviceImageFormatProperties = 64,
    // The FULL host VkPhysicalDeviceProperties (incl. the real VkPhysicalDeviceLimits), forwarded
    // verbatim as a byte blob -- zink reads dozens of limits (maxFramebuffer*, maxColorAttachments,
    // maxBoundDescriptorSets, subgroupSize, ...) and rejects a device whose limits are zero. The
    // vkcube subset reported only deviceName + maxImageDimension2D.
    GetPhysicalDeviceProperties = 65,
    // Generic VkPhysicalDeviceFeatures2 / VkPhysicalDeviceProperties2 pNext-chain forwarder: the
    // app (zink) chains extended structs (Vulkan11/12/13 + subgroup/driver/... features+properties)
    // and reads them back. Carried as {sType, byte-blob} entries keyed by sType; the worker fills
    // the real host values, the ICD copies them into the app's matching pNext nodes (fields only,
    // preserving the app's sType+pNext header). Output-only capability data -- honest forwarding,
    // not blind command marshalling.
    GetPhysicalDeviceCapabilityChain = 66,
    // (GL/zink bring-up): the device-object + command surface a full GL->Vulkan
    // translator emits that the vkcube subset never did. zink is a TRUSTED, correct producer, so
    // these ops are FAITHFULLY MARSHALLED -- the create-info / command args are carried verbatim
    // and the worker forwards them to the real host driver, with NO vkcube-shaped subset allowlist.
    // The two deliberately-hardened defect classes keep their fail-closed guards (host-visible
    // memory coherence; depth-determining render-pass state). A texel buffer view is the first such
    // op (zink mints one at screen init for its null-descriptor fallback).
    CreateBufferView = 67,
    DestroyBufferView = 68,
    // (GL/zink): timeline semaphores. zink HARD-REQUIRES VK_KHR_timeline_semaphore and
    // syncs its batches with them (a created-timeline semaphore the GPU signals to a value on
    // submit + the host waits/queries). CreateSemaphore (existing) carries the timeline type +
    // initial value; these add the host-side ops; queue_submit carries the per-semaphore
    // wait/signal values.
    WaitSemaphores = 69,
    SignalSemaphore = 70,
    GetSemaphoreCounterValue = 71,
    // Query pools (GL 3.3 / occlusion / xfb queries). zink drives OCCLUSION (glBeginQuery),
    // TIMESTAMP (ARB_timer_query -> GL 3.3), PIPELINE_STATISTICS, and TRANSFORM_FEEDBACK_STREAM
    // queries. create/destroy are host-object ops; GetQueryPoolResults is a synchronous readback
    // (the worker calls the real vkGetQueryPoolResults and ships the bytes + the real VkResult).
    // The RECORDED commands (reset/begin/end/write_timestamp + the EXT indexed pair) ride
    // record_command_buffer (26) as new RecordedCommand kinds, NOT new opcodes -- like the other
    // vkCmd* recordings.
    CreateQueryPool = 72,
    DestroyQueryPool = 73,
    GetQueryPoolResults = 74,
    // Host-visible memory READBACK (worker -> ICD): the mirror of WriteMemoryRanges. The worker
    // reads the CURRENT bytes of a host-visible allocation (which now include GPU writes, e.g. a
    // vkCmdCopyQueryPoolResults destination, transform-feedback output, an SSBO) and ships them so
    // the ICD refreshes its mapped-memory shadow. The ICD triggers it after a fence signals for the
    // CLEAN mapped allocations (no pending local writes to clobber), so an app that MAPS + READS a
    // GPU-written buffer sees the real results. Foundational for query-result reads AND the
    // eventual native readback paths (glReadPixels / offscreen export).
    ReadMemoryRanges = 75,
    // readback completion: REAL idle waits. The ICD's QueueWaitIdle /
    // DeviceWaitIdle were local success stubs (safe while nothing depended on them); once idle
    // waits count as readback-completion sync points they must actually wait on the worker
    // (vkQueueWaitIdle / vkDeviceWaitIdle) -- a local VK_SUCCESS would let an idle-synced client
    // download before the GPU copy completed.
    QueueWaitIdle = 76,
    DeviceWaitIdle = 77,
    // record_command_buffer with a BINARY body ([u32 json_len][header][packed
    // commands] -- see RecordCommandBufferRequest::to_wire). Profiling measured the JSON record
    // handler at 94-97% parse+hex; this op removes that marshal while feeding the SAME request
    // struct into the unchanged validate-then-record backend boundary. A separate opcode (not a
    // body sniff): the client sends it only when DeviceCaps.raw_record was advertised, an old
    // worker replies UnknownOp, and the JSON RecordCommandBuffer stays fully alive as the
    // fallback.
    RecordCommandBufferRaw = 78,
    // Compute: one compute pipeline per RPC (the CreateGraphicsPipelines shape).
    // The ICD sends it ONLY when DeviceCaps.queue_flags advertised COMPUTE, so an old worker
    // never sees it (creation-time skew gate; UnknownOp would fail closed anyway).
    CreateComputePipelines = 79,
    // Core-1.0 synchronization-object honesty. These close
    // implied-but-unserved core Vulkan 1.0 gaps -- an app polling a fence or using
    // events hits the named abort-stub today. GetFenceStatus is the fence-family
    // completeness fix (fences already expose create/wait/reset/destroy); the Event
    // ops are the VkEvent object model the sync2 event commands stand on.
    // GetFenceStatus / GetEventStatus carry the honest VkResult (WaitForFences idiom):
    // VK_SUCCESS/VK_NOT_READY and VK_EVENT_SET/VK_EVENT_RESET are normal returns, not
    // faults. Set/Reset/Destroy reuse HandleRequest -> StatusResponse (the op code is
    // the semantic name). The three sync1 command events (vkCmdSetEvent / ResetEvent /
    // WaitEvents) ride record_command_buffer (26) as new RecordedCommand kinds, NOT
    // new opcodes -- like every other vkCmd* recording.
    GetFenceStatus = 80,
    CreateEventOp = 81, // `Op` suffix: windows.h #defines CreateEvent (see CreateSemaphoreOp
                        // above). DestroyEvent / SetEvent / ResetEvent / GetEventStatus are NOT
                        // macros (no A/W variants), so they keep the plain name.
    DestroyEvent = 82,
    GetEventStatus = 83,
    SetEvent = 84,
    ResetEvent = 85,
    // VK_KHR_synchronization2 (native lane). Only vkQueueSubmit2 needs a new
    // opcode -- it carries a vector of VkSubmitInfo2 in one RPC (one native pfn_queue_submit2 call
    // + one fence for the whole batch). The other five sync2 commands (vkCmdPipelineBarrier2 /
    // WriteTimestamp2 / SetEvent2 / ResetEvent2 / WaitEvents2) ride record_command_buffer (26) as
    // new RecordedCommand kinds, NOT new opcodes -- like every other vkCmd* recording.
    QueueSubmit2 = 86,
    // Native lane, Vulkan 1.2 bufferDeviceAddress support: vkGetBufferDeviceAddress. A
    // VkDeviceAddress is NOT a handle -- it is raw 64-bit GPU-VA *data* the app writes into
    // shader-visible memory and the GPU dereferences -- so it cannot ride the opaque-handle table:
    // the worker calls the real vkGetBufferDeviceAddress on the translated buffer and ships the
    // REAL host address verbatim (there is one real device behind the relay, and the guest's
    // shaders execute on it). Gated on the enabled bufferDeviceAddress FEATURE (no extension --
    // it is core 1.2, unmasked only on the native lane).
    GetBufferDeviceAddress = 87,
    // descriptorIndexing: vkGetDescriptorSetLayoutSupport, forwarded so the HOST's
    // real answer (supported + maxVariableDescriptorCount) replaces the ICD's local advisory
    // fiction. The request reuses the CreateDescriptorSetLayoutRequest shape (nothing is
    // created). On UnknownOp (an old worker) the ICD falls back to the advisory local answer for
    // classic layouts and fails closed for DI-shaped ones.
    GetDescriptorSetLayoutSupport = 88,
    // Vulkan 1.3 support (maintenance4): vkGetDeviceBufferMemoryRequirements /
    // vkGetDeviceImageMemoryRequirements -- memory requirements for a CREATE-INFO (no object
    // exists), so the ICD's created-object requirement caches cannot answer and the HOST must.
    // The requests REUSE the CreateBuffer/CreateImage shapes (nothing is created); the responses
    // reuse the create responses (the handle stays 0). Gated on the enabled maintenance4
    // feature. The SPARSE image variant is served ICD-locally (count 0 -- the relay serves no
    // sparse resources; sparseBinding is never enabled, so no legal query can expect entries).
    GetDeviceBufferMemoryRequirements = 89,
    GetDeviceImageMemoryRequirements = 90,
    // Vulkan 1.3 required-feature audit: the DEVICE-level vkResetQueryPool -- the
    // hostQueryReset feature (reset a query pool FROM THE HOST, no command buffer), required since
    // Vulkan 1.2. Distinct from the command-buffer CmdResetQueryPool (which rides
    // record_command_buffer, 26): this one is a synchronous device op like GetQueryPoolResults. The
    // worker calls the real vkResetQueryPool; the response is a plain StatusResponse (the API
    // returns void, so a range/handle error is validation-layer territory, not an app-visible
    // result).
    ResetQueryPool = 91,
    // Pipeline specialization constants need a binary tail for the raw pData bytes. Keep the
    // legacy JSON pipeline ops byte-stable and use separate vocabulary (the op-78 precedent), so
    // old ICD -> new worker remains unambiguous and no body sniffing is required.
    CreateGraphicsPipelinesRaw = 92,
    CreateComputePipelinesRaw = 93,
};

enum class RpcStatus : std::uint32_t {
    Ok = 0,
    UnknownOp = 1,
    BadRequest = 2,
    Internal = 3,
};

const char* to_string(RpcStatus status);

// One RPC frame. request_id correlates a response with its request; status is an
// RpcStatus on a response and 0 on a request. body is uninterpreted here.
struct RpcMessage {
    std::uint32_t op = 0;
    std::uint32_t request_id = 0;
    std::uint32_t status = 0;
    std::string body;
};

// Size of the binary header (op + request_id + status), each u32 little-endian.
constexpr std::size_t kRpcHeaderBytes = 12;

// Serializes msg to [u32 op][u32 request_id][u32 status][body] (header LE).
std::string encode_rpc(const RpcMessage& msg);

// Total decoder: returns false (setting error) on a payload shorter than the
// header. Never throws, never reads out of bounds. The body is whatever follows
// the header, uninterpreted (so the op space stays forward-compatible).
bool decode_rpc(const std::string& payload, RpcMessage& msg, std::string& error);

// Frames + binary RPC messages over a Connection (mirrors transport's
// MessageChannel, but for the binary RPC envelope rather than JSON control
// messages). Reuses the length-prefixed frame envelope (wire.hpp).
class RpcChannel {
  public:
    explicit RpcChannel(transport::Connection& conn) : conn_(conn) {}

    void send(const RpcMessage& msg);
    // Reads one RPC message. Returns false on a clean EOF with no partial frame
    // buffered; throws transport::TransportError on a truncated/oversized frame
    // or a malformed RPC header.
    bool recv(RpcMessage& msg);

  private:
    transport::Connection& conn_;
    std::string buffer_;
};

} // namespace vkr::vkrpc

#endif // VKRELAY2_COMMON_VKRPC_RPC_HPP
