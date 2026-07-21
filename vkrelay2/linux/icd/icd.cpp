// vkrelay2 Linux Vulkan ICD.
//
// The Khronos Vulkan loader dlopens this library inside WSL and dispatches the app's Vulkan
// calls here; the ICD forwards them to the Windows worker over the existing RPC envelope
// and the worker presents
// to a real Win32 window (worker-present). The bufferless-triangle DRAW spine covers: image
// views, shader modules (SPIR-V), render pass, framebuffer, pipeline layout, graphics pipeline, and
// the draw vkCmd subset (begin/end render pass, bind pipeline, set viewport/scissor, draw) --
// enough for `vkrelay2-triangle` on screen. The host-visible memory spine adds: honest
// memory properties + buffer requirements, vkCreateBuffer/vkBindBufferMemory, vkAllocateMemory, a
// Linux-side shadow buffer for vkMapMemory, vkFlushMappedMemoryRanges + the
// coherent-flush-at-submit sweep, and vkCmdBindVertexBuffers -- enough for `vkrelay2-vbo` (a mapped
// vertex buffer on screen). Images / descriptors / textures come later (their proc-addr
// stays null).
//
// Design decisions:
//  - = A: the LAUNCHER establishes the worker session and exports the data-plane endpoint +
//    token via env (VKRELAY2_DATA_PLANE_HOST/PORT, VKRELAY2_APP_TOKEN); the ICD only connects
//    the data plane at vkCreateInstance and FAILS LOUD if the env is absent (no system-driver
//    fallback, no half-initialized instance).
//  -: Xcb surface only; in worker-present mode the app's xcb window is a placeholder -- the
//    ICD ignores connection/window and forwards a plain create_surface. No libxcb dependency
//    (the VkXcbSurfaceCreateInfoKHR arg is taken as an opaque pointer).
//  -: hand-written minimal entry set; VK_LOADER_DATA magic on every dispatchable; worker
//    u64s pass through for non-dispatchables; loader interface capped at v3.
//  - Conservative advertised surface: VK_KHR_surface + VK_KHR_xcb_surface (instance) and
//    VK_KHR_swapchain (device). Only backed entry points are returned from the proc-addr.
//
// Built only on Linux (see CMakeLists); links vkrelay2_common for the transport + RPC client.

#include <vulkan/vk_icd.h>
#include <vulkan/vulkan.h>

#include "common/protocol/messages.hpp"
#include "common/transport/tcp.hpp"
#include "common/transport/transport.hpp"
#include "common/util/json.hpp"
#include "common/vkrpc/feature_bits.h" // pack/unpack VkPhysicalDeviceFeatures <-> u64
#include "common/vkrpc/rpc.hpp"
#include "common/vkrpc/rpc_profile.h" // Upload-sweep profile line.
#include "common/vkrpc/vulkan_session.hpp"
#include "linux/icd/icd_caps_cache.h"
#include "linux/icd/icd_memory.h"
#include "linux/icd/icd_softdirty.h"
#include "linux/icd/icd_subset.h"
#include "linux/icd/icd_version_policy.h" // the native-lane predicate (pure, unit-tested)

#include <algorithm>
#include <array>
#include <atomic>
#include <climits>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

// Non-dispatchable handles round-trip through a worker u64 via uintptr_t (x86_64 WSI: 64-bit
// pointer-defined handles).
static_assert(sizeof(void*) == 8, "vkrelay2 ICD assumes a 64-bit target");

namespace {

namespace vkrpc = vkr::vkrpc;
namespace transport = vkr::transport;
namespace protocol = vkr::protocol;
namespace json = vkr::json;

constexpr bool multi_draw_indirect_feature_bit_matches_wire_order() {
    VkPhysicalDeviceFeatures features{};
    features.multiDrawIndirect = VK_TRUE;
    return vkrpc::pack_physical_device_features(features) == vkrpc::kFeatureMultiDrawIndirect;
}
static_assert(multi_draw_indirect_feature_bit_matches_wire_order(),
              "kFeatureMultiDrawIndirect drifted from VkPhysicalDeviceFeatures wire order");

// --- Dispatchable handle impls (VK_LOADER_DATA MUST be first) ---------------
struct PhysicalDeviceImpl; // defined below; the instance caches STABLE handles to these
struct InstanceImpl {
    VK_LOADER_DATA loader_data;
    std::uint64_t worker = 0;
    // Physical-device handles MUST be persistent for the instance's lifetime: the Vulkan loader
    // reconciles them across vkEnumeratePhysicalDevices calls BY VALUE, so returning a fresh
    // allocation each call breaks its handle tracking (it frees the tramp wrapper an app still
    // holds, then abort()s on the next physical-device call). Enumerate populates this cache ONCE
    // and every call returns these same pointers. (The single-enumerate canaries never tripped it;
    // the Vulkan CTS -- which enumerates repeatedly -- did.) Owned via unique_ptr so
    // DestroyInstance's `delete impl` frees them -- no per-device cleanup to forget.
    std::vector<std::unique_ptr<PhysicalDeviceImpl>> physical_devices;
};
struct PhysicalDeviceImpl {
    VK_LOADER_DATA loader_data;
    std::uint64_t worker = 0;
    std::uint64_t instance_worker = 0; // owning instance (create_device needs {instance, phys})
    vkrpc::DeviceCaps caps;
    // the HOST's real device-extension names (from the enumerate response), so
    // EnumerateDeviceExtensionProperties + CreateDevice work on the allowlist INTERSECTED with
    // what the host can actually enable. Empty (old worker / mock) -> policy-only fallback (the
    // prior behavior; the mock backend enables anything).
    std::vector<std::string> host_device_extensions;
    // The honest host memory-properties table, fetched + cached at enumerate-time so the void
    // vkGetPhysicalDeviceMemoryProperties is a pure cache copy. A zeroed table
    // (0 types/heaps) is the fail-closed value if the worker could not provide one.
    VkPhysicalDeviceMemoryProperties mem_props{};
    // GL/zink: the host's real VkPhysicalDeviceFeatures, fetched once on first query
    // (the void vkGetPhysicalDeviceFeatures cannot report an RPC failure -> memoize an honest
    // answer, zeroed fail-closed otherwise). Guarded by g_mu.
    bool features_cached = false;
    VkPhysicalDeviceFeatures features{};
    // GL/zink: the host's real FULL VkPhysicalDeviceProperties (incl. limits), fetched
    // once (op 65). props_cached=false -> the synthesized minimal properties (the vkcube-subset
    // fallback).
    bool props_cached = false;
    VkPhysicalDeviceProperties full_props{};
};
struct DeviceImpl {
    VK_LOADER_DATA loader_data;
    std::uint64_t worker = 0;
    int queue_family = 0; // the worker's reported graphics family (app sees it as index 0)
    // Copied from the physical device at create: vkAllocateMemory reads the chosen type's property
    // flags here (to tell the memory tracker host-visible/coherent) and range-checks the type
    // index.
    VkPhysicalDeviceMemoryProperties mem_props{};
    // MRT capability gate: the worker-advertised DeviceCaps.max_color_attachments, copied at
    // create. 0 = unknown (an old worker) -> the render-pass checkers cap colors at 1 (the pre-MRT
    // gate), so a version-skewed pair rejects loudly instead of the worker silently narrowing.
    std::uint32_t max_color_attachments = 0;
    // Compute: the worker-advertised DeviceCaps.queue_flags, copied at create.
    // vkCreateComputePipelines fails closed unless the COMPUTE bit is here -- an old worker
    // (0) never receives compute work.
    std::uint64_t queue_flags = 0;
    // Set at create iff on the native lane AND the app enabled
    // VK_KHR_dynamic_rendering. Gates the VkPipelineRenderingCreateInfo pNext admission at
    // CreateGraphicsPipelines, so the default/zink lane is never widened by
    // accident and a device that did not enable the extension cannot slip a DR pipeline through.
    bool dynamic_rendering_enabled = false;
    // Set at create iff on the native lane AND the app enabled
    // VK_KHR_synchronization2 AND its feature. Purely informational on the ICD side (the sync2
    // command admission gate lives on the backends, mock == real); kept for symmetry + diagnostics.
    bool synchronization2_enabled = false;
    // Required-feature audit: set at create iff the app
    // enabled the hostQueryReset FEATURE (core 1.2, served on BOTH lanes -- no lane gate). The
    // device-level ResetQueryPool entrypoint no-ops before forwarding unless this is set, so a
    // client that never enabled the feature cannot drive an invalid host vkResetQueryPool.
    bool host_query_reset_enabled = false;
    // Required-feature audit (multiview, REQUIRED since 1.1, served on BOTH lanes -- no
    // lane gate): set at create iff the app enabled the multiview FEATURE (the Vulkan11Features
    // rollup spelling, which the relay reports host-TRUE and forwards). CreateRenderPass2 carries a
    // subpass viewMask ONLY when this is set (else it rejects the pass fail-closed), so a device
    // that never enabled multiview cannot drive a multiview host render pass.
    bool multiview_enabled = false;
    // bufferDeviceAddress: set at create iff on the native lane AND the app enabled the
    // bufferDeviceAddress FEATURE (no extension -- core 1.2). Gates the DEVICE_ADDRESS
    // memory-allocate flag admission and vkGetBufferDeviceAddress, so a device that did not enable
    // the feature (or the default/zink lane, where it is masked FALSE) fails closed.
    bool buffer_device_address_enabled = false;
    // VK_EXT_vertex_attribute_divisor: set at create iff the app
    // enabled the EXTENSION (advertised for zink's GL 3.x path). Gates the
    // VkPipelineVertexInputDivisorStateCreateInfoEXT pNext admission at CreateGraphicsPipelines --
    // a device that did not enable the ext cannot slip the divisor pNext through. The two feature
    // flags gate the divisor VALUES (via the shared vkrpc::vertex_binding_divisors_ok): divisor !=
    // 1 needs vertexAttributeInstanceRateDivisor, divisor == 0 additionally needs the zero-divisor
    // bit.
    bool vertex_attr_divisor_enabled = false;
    bool vertex_attr_divisor_feature_enabled = false;
    bool vertex_attr_zero_divisor_feature_enabled = false;
    // geometry-stream: the ENABLED VK_EXT_transform_feedback extension + geometryStreams feature
    // + the worker-advertised DeviceCaps.rasterization_stream_state (false = old
    // worker whose pipeline decoder would silently DROP the stream pNext). All three gate the
    // VkPipelineRasterizationStateStreamCreateInfoEXT pNext admission at CreateGraphicsPipelines
    // (structural via graphics_pipeline_ok's allow_rasterization_stream; values via the worker's
    // shared vkrpc::rasterization_stream_ok) -- a skewed pair degrades to the prior
    // advertise-then-reject contract, never to silent wrongness.
    bool transform_feedback_enabled = false;
    bool geometry_streams_feature_enabled = false;
    bool worker_rasterization_stream = false;
    // Core indirect-draw command vocabulary. The worker bit is additive protocol negotiation;
    // multi_draw_indirect_enabled is the base feature state requested at device creation.
    bool worker_core_indirect_draw = false;
    bool worker_core_indirect_draw_scalar_payload = false;
    bool multi_draw_indirect_enabled = false;
    bool worker_core_indirect_draw_count = false;
    bool draw_indirect_count_enabled = false;
    // descriptorIndexing: the enabled kDIFeature* bits. CreateDevice folds the app's
    // enabled-feature chain into these (served subset only -- an unserved or off-lane enable is
    // FEATURE_NOT_PRESENT), and the per-binding flag admission, UAB pools, and variable-count
    // allocation all key on them, failing closed on a device that did not enable the feature.
    std::uint64_t descriptor_indexing_feature_bits = 0;
    // The enabled kVk13Feature* bits + whether this device was
    // reported (and enabled) as an honest apiVersion-1.3 device. vk13_device turns on the
    // core-1.3 command surface; the bits gate each feature's usage shapes.
    std::uint64_t vk13_feature_bits = 0;
    bool vk13_device = false;
    // The extensions THIS device enabled -- the device-aware
    // GetDeviceProcAddr consults it so promoted-extension alias names resolve only where their
    // extension was actually enabled (symbol availability is an observable capability surface).
    // Immutable after CreateDevice, so GetDeviceProcAddr reads it lock-free.
    std::set<std::string> enabled_extensions;
    // privateData (served ICD-LOCALLY): private data has no driver-observable
    // semantics -- it is an app-side {slot, objectType, objectHandle} -> u64 store -- so the
    // conformant implementation lives here, with no wire traffic. Slot handles are ICD-minted.
    // The key INCLUDES the object type: the relay's handle values come from several independent
    // counters (worker ids, ICD-local handles) and are only unique per type. Guarded by g_mu
    // like all device state.
    std::uint64_t next_private_data_slot = 1;
    std::map<std::tuple<std::uint64_t, long long, std::uint64_t>, std::uint64_t> private_data;
    std::set<std::uint64_t> private_data_slots; // live ICD-minted slot handles
};
struct QueueImpl {
    VK_LOADER_DATA loader_data;
    std::uint64_t worker = 0;
};
struct CommandBufferImpl {
    VK_LOADER_DATA loader_data;
    std::uint64_t worker = 0;
    // Client-side command stream: vkCmd* accumulate here; vkEndCommandBuffer ships the whole
    // begin->[cmds]->end recording in one record_command_buffer RPC (no per-vkCmd round-trip).
    bool one_time_submit = false;
    std::vector<vkrpc::RecordedCommand> recording;
    // The void vkCmd* cannot return an error, so an out-of-subset command (a non-INLINE render
    // pass, a non-graphics bind, firstViewport != 0, ...) sets this; vkEndCommandBuffer then fails
    // cleanly instead of forwarding a silently-canonicalized stream.
    bool local_invalid = false;
    const char* invalid_reason = "";
    // Readback destinations this recording writes: the dst memory of each
    // recorded vkCmdCopyQueryPoolResults / vkCmdCopyImageToBuffer. Carried on the CB (not armed
    // globally at record time) so a successful vkQueueSubmit can arm exactly the submitted work.
    std::vector<std::uint64_t> readback_dsts;
    // Vulkan 1.3 support: whether the OWNING device is an honest
    // apiVersion-1.3 device, stamped at vkAllocateCommandBuffers. The copy_commands2 wrappers
    // down-convert to the classic recorded kinds (spec-identical with every pNext empty), which
    // ERASES the 1.3-ness before the worker's per-device gates could see it -- so the version
    // gate for those entries lives HERE: on a 1.2 device (the default/zink lane, or a Vulkan-1.2
    // native device) a copy2 call marks the recording locally invalid instead of silently
    // serving a command surface the reported apiVersion does not include.
    bool vk13_device = false;
    // Additive worker-vocabulary gate + the owning device's enabled base feature, stamped at
    // allocation like vk13_device so void vkCmd* recorders can fail locally without an RPC.
    bool worker_core_indirect_draw = false;
    bool worker_core_indirect_draw_scalar_payload = false;
    bool multi_draw_indirect_enabled = false;
    bool worker_core_indirect_draw_count = false;
    bool draw_indirect_count_enabled = false;
};

template <class H> H to_handle(std::uint64_t v) {
    return reinterpret_cast<H>(static_cast<std::uintptr_t>(v));
}
template <class H> std::uint64_t from_handle(H h) {
    return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(h));
}

// --- The single data-plane connection (one per ICD/process) -----------------
std::mutex g_mu; // serializes RPC (the worker serves one in-flight request at a time)
std::unique_ptr<transport::Connection> g_conn;
std::unique_ptr<vkrpc::RpcChannel> g_rpc;
std::atomic<std::uint32_t> g_request_id{1};
// the worker advertises the raw readback reply per SESSION (a serve-loop
// protocol capability riding DeviceCaps at enumerate; every device from one worker agrees).
// false = old worker -> readbacks keep the legacy JSON+hex response.
bool g_raw_readback = false;
// same pattern for the binary record stream. VKRELAY2_NO_RAW_RECORD=1 forces
// the JSON path even against a new worker -- the explicit compatibility fallback while the GL
// correctness support is still incomplete (also the debugging escape hatch).
bool g_raw_record = false;
// the surface-caps cache (zink's per-frame resize poll = one round trip per
// frame). All state + rules live in the pure header (icd_caps_cache.h, unit-tested); the
// entrypoints only call its hooks. Guarded by g_mu like everything else here. The env gate reads
// once at first use (VKRELAY2_NO_CAPS_CACHE=1 = correctness-first fallback, all polls uncached).
vkr::icd_caps::CapsCache<vkrpc::GetSurfaceCapabilitiesResponse>& caps_cache() {
    static vkr::icd_caps::CapsCache<vkrpc::GetSurfaceCapabilitiesResponse> cache{
        std::getenv("VKRELAY2_NO_CAPS_CACHE") == nullptr};
    return cache;
}

std::uint32_t next_id() {
    return g_request_id.fetch_add(1);
}

// GL/zink: an env-gated entry-point trace (VKRELAY2_ICD_TRACE=1) -- the "ICD log" the
// roadmap's localize-don't-guess discipline calls for. zink fails screen creation silently, so this
// shows which physical-device queries / pNext sTypes / create calls zink makes and where it stops.
bool icd_trace_enabled() {
    static const bool on = std::getenv("VKRELAY2_ICD_TRACE") != nullptr;
    return on;
}

// (the Vulkan-1.3 opener): is this process on the steering-safe NATIVE lane? Read once +
// cached (the VKRELAY2_ICD_TRACE precedent). The launcher's native frontend sets VKRELAY2_NATIVE_
// LANE=1 and actively neutralizes it for the zink/GL modes, so the DEFAULT (zink) lane can never
// observe a 1.3-family surface -- the uncap is impossible on it BY CONSTRUCTION. Decision lives in
// the pure icd_policy::native_lane_enabled (unit-tested); this only supplies the cached env read.
bool native_lane() {
    static const bool on =
        vkr::icd_policy::native_lane_enabled(std::getenv("VKRELAY2_NATIVE_LANE"));
    return on;
}

// Mesa/Zink 23.2 can select the EXT attachment-feedback-loop layout for a render-pass feedback
// path even when VK_EXT_attachment_feedback_loop_layout was not advertised. Win32 WSI swapchain
// images cannot request the extension's mandatory usage bit, so advertising the extension would
// merely move the inconsistency to swapchain creation. GENERAL is Zink's own fallback when the
// extension is unavailable and is valid for both attachment and sampled access.
int normalize_unadvertised_feedback_layout(VkImageLayout layout) {
    constexpr int kAttachmentFeedbackLoopOptimalExt = 1000339000;
    return static_cast<int>(layout) == kAttachmentFeedbackLoopOptimalExt
               ? static_cast<int>(VK_IMAGE_LAYOUT_GENERAL)
               : static_cast<int>(layout);
}
// (native lane): would advertised_device_extensions actually
// advertise `name` for THIS physical device? = we are on the native lane AND the host supports it
// (an empty host list = old worker / mock -> "has everything", matching
// advertised_device_extensions exactly). The per-family feature masks key on THIS predicate, not
// merely native_lane(), so a promoted-core host that reports a 1.3 feature but does NOT list its
// KHR/EXT device extension neither advertises the extension nor forwards the feature -- the 1.2 +
// honest-extension story stays exact. (name is one of the native-lane allowlist entries; membership
// is implied by the caller.)
bool advertises_native_device_ext(const PhysicalDeviceImpl* pd, const char* name) {
    const bool host_has =
        pd->host_device_extensions.empty() ||
        std::find(pd->host_device_extensions.begin(), pd->host_device_extensions.end(), name) !=
            pd->host_device_extensions.end();
    return native_lane() && host_has;
}

bool host_supports_device_ext(const PhysicalDeviceImpl* pd, const char* name) {
    return pd->host_device_extensions.empty() ||
           std::find(pd->host_device_extensions.begin(), pd->host_device_extensions.end(), name) !=
               pd->host_device_extensions.end();
}
// Vulkan 1.3 support + required-feature audit: is THIS physical device reported as an
// honest 1.3 device? = the native lane AND the worker vouched (DeviceCaps.vk13_ready: host
// apiVersion >= 1.3 AND the full relay-served cumulative required matrix -- the f13 family +
// dynamicRendering/synchronization2 + the two required 1.3 memory-model bits + the promoted f10/
// f11/f12 required members -- AND the relay-served gate for multiview + device-level hostQueryReset
// is closed). Everything Vulkan 1.3 support unmasks -- the reported apiVersion, the f13 rollup +
// standalone served features, the core-1.3 command surface -- keys on THIS predicate, so an old
// worker (vk13_ready absent -> 0), a host below 1.3, a required feature not yet served, or the
// default/zink lane all stay on the exact Vulkan 1.2 surface.
bool vk13_device_ok(const PhysicalDeviceImpl* pd) {
    return native_lane() && pd->caps.vk13_ready != 0;
}
void icd_trace(const char* fmt, ...) {
    if (!icd_trace_enabled()) {
        return;
    }
    std::fprintf(stderr, "vkrelay2-icd-trace: ");
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
    std::fprintf(stderr, "\n");
}

std::uint64_t trace_hash_bytes(const void* data, std::size_t size) {
    auto* bytes = static_cast<const unsigned char*>(data);
    std::uint64_t hash = 1469598103934665603ull;
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

// Monotonic source for LOCAL non-dispatchable handles -- objects the ICD owns entirely and never
// sends to the worker (e.g. the no-op pipeline cache below). Guarded by g_mu. Kept distinct from
// the worker's handle space only conceptually; these never cross the wire so they cannot collide
// there.
std::uint64_t g_next_local_handle = 1;

// Host-visible memory tracker: the app's vkMapMemory pointer is a Linux-side shadow
// buffer this owns; the dirty chunks ship to the worker's real VkDeviceMemory at flush/submit. One
// per process (worker handles are globally unique), guarded by g_mu like every other RPC.
vkr::icd_mem::MappedMemoryTracker g_tracker;
// The soft-dirty page pre-filter for the submit sweep (a zink app keeps big
// allocations persistently mapped, so the full-diff sweep memcmps ~everything mapped at every
// submit -- 188 MB x 3/frame on the OpenSCAD GUI). Lazily self-tested at the first sweep;
// unavailable/failed/VKRELAY2_NO_SOFT_DIRTY=1 -> the sweep stays the full diff (fail closed).
// Guarded by g_mu like the tracker it filters.
vkr::icd_softdirty::SoftDirtyTracker g_softdirty;
bool g_softdirty_tried = false;
bool g_softdirty_on = false;
std::uint64_t g_sweep_counter = 0;
// Every Nth sweep re-baselines (soft-dirty reset + full diff): pages written ONCE since the
// last reset stay flagged (the kernel bit is sticky until a reset), so a periodic re-baseline
// sheds no-longer-written pages from the flagged set. The kernel offers no ATOMIC
// read-and-clear, so a per-sweep reset would lose any write racing the (read, clear) window --
// this epoch protocol (reset ONLY immediately before a full diff) is the correct-by-
// construction alternative, and the cadence trades the amortized full-diff cost against
// flagged-set accumulation under zink's rotating streaming writes. 8 measured optimal on the
// OpenSCAD GUI (k=2..64 swept: 7.4/5.3/4.8/5.0/8.0 ms); tunable for measurement via
// VKRELAY2_SWEEP_REBASELINE_EVERY.
std::uint64_t sweep_rebaseline_every() {
    static const std::uint64_t n = [] {
        const char* v = std::getenv("VKRELAY2_SWEEP_REBASELINE_EVERY");
        if (v != nullptr) {
            const long long parsed = std::atoll(v);
            if (parsed >= 1) {
                return static_cast<std::uint64_t>(parsed);
            }
        }
        return std::uint64_t{8};
    }();
    return n;
}
// One authoritative entry per live worker buffer, guarded by g_mu. Requirements make the void
// getters pure cache copies; logical size/usage drive indirect-draw admission (requirements.size
// may include host tail padding); memory is populated by either BindBufferMemory spelling and lets
// GPU-write recorders enroll exactly their destination allocation in fence-time readback.
struct BufferInfo {
    VkMemoryRequirements requirements{};
    VkDeviceSize size = 0;
    VkBufferUsageFlags usage = 0;
    std::uint64_t memory = 0;
};
std::map<std::uint64_t, BufferInfo> g_buffers;
// Readback lifecycle: recorded -> SUBMITTED -> READY -> delivered. A
// recorder (vkCmdCopyQueryPoolResults / vkCmdCopyImageToBuffer) notes its dst memory ON THE
// COMMAND BUFFER; a successful vkQueueSubmit arms one record per submitted batch (the batch's dst
// memories + the completion proofs that cover it: the submit call's fence and the batch's timeline
// signal values); a PROVEN sync point (a wait that covers the fence / a timeline value, or a real
// idle wait) promotes the record's dsts to READY. Only READY allocations ever download -- so an
// unrelated earlier wait (or a plain vkMapMemory) can no longer consume the flag and deliver
// pre-copy bytes, the exact silent-stale class the record-time flagging had. Guarded by g_mu.
// Queue order: a fence/timeline signal operation's first synchronization scope
// includes ALL commands earlier in submission order on that queue (spec 7.3.1/7.4.1) -- so every
// LATER same-queue submit's proofs also prove an earlier readback batch, and are appended to its
// record (fence-only and empty-batch signal submits included). The record names its queue so a
// multi-queue future cannot silently cross-promote.
struct ReadbackSubmission {
    std::uint64_t queue = 0;                                      // the worker queue handle
    std::vector<std::uint64_t> fences;                            // any covering fence proof
    std::vector<std::pair<std::uint64_t, std::uint64_t>> signals; // covering timeline signals
    std::vector<std::uint64_t> dsts;                              // dst memory handles
};
std::vector<ReadbackSubmission> g_readback_submitted;
std::set<std::uint64_t> g_readback_ready;
// Image equivalents: vkGetImageMemoryRequirements + vkGetImageSubresourceLayout are
// void, so the worker returns the real values on CreateImageResponse and they are cached here
// (keyed by worker image handle); the getters are pure cache copies. The subresource layout is
// present only for LINEAR tiling (querying it is invalid for OPTIMAL). Format properties are
// memoized per (worker physical device, format) -- the void getter forwards once then serves the
// cache; a zeroed table is the fail-closed answer on RPC failure.
std::map<std::uint64_t, VkMemoryRequirements> g_image_reqs;
std::map<std::uint64_t, VkSubresourceLayout> g_image_subresource; // LINEAR images only
// (GL/zink): cache the FULL format-properties response (32-bit VkFormatProperties +
// 64-bit VkFormatProperties3) so both the 1.0 and the *2 getters serve one honest forward.
std::map<std::pair<std::uint64_t, int>, vkrpc::GetPhysicalDeviceFormatPropertiesResponse>
    g_format_props;

// Per-frame range cap for the upload splitter: keeps the WriteMemoryRanges json header well under
// kMaxBinaryJsonHeaderBytes (64 KiB) even at the worst-case ~60 bytes/range, while the payload cap
// (kMaxMemoryUploadBytes) bounds the tail. Together they keep every split frame under
// kMaxFrameBytes.
constexpr std::size_t kFrameMaxRanges = 512;

// Converts a tracker dirty snapshot into the wire request (same shape, different types).
vkrpc::WriteMemoryRangesRequest to_write_request(const vkr::icd_mem::DirtySnapshot& snap) {
    vkrpc::WriteMemoryRangesRequest req;
    for (const vkr::icd_mem::Upload& u : snap.uploads) {
        vkrpc::MemoryUpload mu;
        mu.memory = u.memory;
        for (const vkr::icd_mem::Range& r : u.ranges) {
            mu.ranges.push_back(vkrpc::MemoryUploadRange{r.offset, r.size});
        }
        req.uploads.push_back(std::move(mu));
    }
    req.payload = snap.payload;
    return req;
}

// Ships a tracker snapshot to the worker, split into cap-fitting frames, committing each only after
// its own ack (split-per-frame design). Returns false (leaving
// the uncommitted frames pending for the next sweep) on the first failed upload.
bool ship_snapshot(const vkr::icd_mem::DirtySnapshot& snap) {
    for (const vkr::icd_mem::DirtySnapshot& frame :
         vkr::icd_mem::split_for_upload(snap, vkrpc::kMaxMemoryUploadBytes, kFrameMaxRanges)) {
        const std::uint32_t request_id = next_id();
        if (icd_trace_enabled()) {
            std::size_t range_count = 0;
            for (const auto& upload : frame.uploads) {
                range_count += upload.ranges.size();
            }
            icd_trace("UploadSnapshot request=%u allocations=%zu ranges=%zu bytes=%zu hash=0x%llx",
                      request_id, frame.uploads.size(), range_count, frame.payload.size(),
                      static_cast<unsigned long long>(
                          trace_hash_bytes(frame.payload.data(), frame.payload.size())));
        }
        const vkrpc::StatusResponse wr =
            vkrpc::write_memory_ranges(*g_rpc, request_id, to_write_request(frame));
        if (!wr.ok) {
            return false; // this + later frames stay uncommitted -> re-sent next sweep
        }
        g_tracker.commit(frame);
    }
    return true;
}

bool ensure_connected() {
    if (g_rpc) {
        return true;
    }
    const char* host = std::getenv("VKRELAY2_DATA_PLANE_HOST");
    const char* port_s = std::getenv("VKRELAY2_DATA_PLANE_PORT");
    const char* token = std::getenv("VKRELAY2_APP_TOKEN");
    if (host == nullptr || port_s == nullptr || token == nullptr) {
        std::fprintf(stderr, "vkrelay2-icd: missing VKRELAY2_DATA_PLANE_HOST/PORT/APP_TOKEN "
                             "(launch through vkrelay2)\n");
        return false;
    }
    const int port = std::atoi(port_s);
    if (port <= 0) {
        std::fprintf(stderr, "vkrelay2-icd: invalid VKRELAY2_DATA_PLANE_PORT '%s'\n", port_s);
        return false;
    }
    try {
        auto conn = transport::tcp_connect(host, port);
        transport::MessageChannel channel(*conn);
        protocol::AppHello hello;
        hello.app_token = token;
        channel.send(protocol::MessageType::AppHello, hello.to_body());
        protocol::MessageType type = protocol::MessageType::Unknown;
        json::Value body;
        if (!channel.recv(type, body) || type != protocol::MessageType::AppAck) {
            std::fprintf(stderr, "vkrelay2-icd: no app_ack from worker data plane\n");
            return false;
        }
        if (!protocol::AppAck::from_body(body).ok) {
            std::fprintf(stderr, "vkrelay2-icd: worker rejected app_hello (bad token?)\n");
            return false;
        }
        g_conn = std::move(conn);
        g_rpc = std::make_unique<vkrpc::RpcChannel>(*g_conn);
        std::fprintf(stderr, "vkrelay2-icd: data plane connected (%s:%d), app_ack ok\n", host,
                     port);
        return true;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "vkrelay2-icd: data-plane connect failed: %s\n", e.what());
        return false;
    }
}

VkResult fault() {
    return VK_ERROR_INITIALIZATION_FAILED;
}

// =====================================================================================
// Instance / physical-device level
// =====================================================================================
VKAPI_ATTR VkResult VKAPI_CALL EnumerateInstanceVersion(std::uint32_t* pApiVersion) {
    if (pApiVersion != nullptr) {
        // Vulkan 1.3 support: the NATIVE lane reports instance-level 1.3 (its only
        // instance-level addition beyond 1.1 is vkGetPhysicalDeviceToolProperties, served). The
        // default/zink lane keeps 1.1 -- the DEVICE apiVersion (capped per-lane in
        // GetPhysicalDeviceProperties) is what gates feature surfaces; this stays conservative.
        *pApiVersion = native_lane() ? VK_API_VERSION_1_3 : VK_API_VERSION_1_1;
    }
    return VK_SUCCESS;
}

VkResult enumerate_extensions(const char* const* names, std::uint32_t count, std::uint32_t* pCount,
                              VkExtensionProperties* pProps) {
    if (pProps == nullptr) {
        *pCount = count;
        return VK_SUCCESS;
    }
    const std::uint32_t n = *pCount < count ? *pCount : count;
    for (std::uint32_t i = 0; i < n; ++i) {
        VkExtensionProperties p{};
        std::snprintf(p.extensionName, sizeof(p.extensionName), "%s", names[i]);
        p.specVersion = 1;
        pProps[i] = p;
    }
    *pCount = n;
    return n < count ? VK_INCOMPLETE : VK_SUCCESS;
}

// Returns the first requested extension not in `allowed` (so the caller can reject it with the
// Vulkan error apps expect), or nullptr if every requested extension is supported.
const char* first_unsupported_ext(std::uint32_t count, const char* const* names,
                                  const char* const* allowed, std::uint32_t n_allowed) {
    for (std::uint32_t i = 0; i < count; ++i) {
        bool ok = false;
        for (std::uint32_t j = 0; j < n_allowed; ++j) {
            if (std::strcmp(names[i], allowed[j]) == 0) {
                ok = true;
                break;
            }
        }
        if (!ok) {
            return names[i];
        }
    }
    return nullptr;
}

VKAPI_ATTR VkResult VKAPI_CALL EnumerateInstanceExtensionProperties(const char* /*layer*/,
                                                                    std::uint32_t* pCount,
                                                                    VkExtensionProperties* pProps) {
    // "VK_KHR_xcb_surface" written literally so the ICD needs no vulkan_xcb.h / libxcb.
    // VK_KHR_get_physical_device_properties2 (Vulkan 1.3 support, found by vulkaninfo): pure
    // REPORTING aliases of core-1.1 functionality the ICD already serves -- 1.0-style consumers
    // (vulkaninfo gates its whole extended dump on it) reach the *2 query surface through the KHR
    // names. VK_KHR_xlib_surface (Vulkan 1.3 support breadth, found by mpv): the Xlib spelling of
    // the same XID-keyed surface path -- see CreateXlibSurfaceKHR.
    const char* kExts[] = {VK_KHR_SURFACE_EXTENSION_NAME, "VK_KHR_xcb_surface",
                           "VK_KHR_xlib_surface",
                           VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME};
    return enumerate_extensions(kExts, 4, pCount, pProps);
}

VKAPI_ATTR VkResult VKAPI_CALL EnumerateInstanceLayerProperties(std::uint32_t* pCount,
                                                                VkLayerProperties*) {
    *pCount = 0;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL CreateInstance(const VkInstanceCreateInfo* pCreateInfo,
                                              const VkAllocationCallbacks*, VkInstance* pInstance) {
    // Validate the requested instance extensions against what we actually back (we advertise
    // only these). Reject an unsupported one with the error apps expect rather than succeeding
    // into a partially-fictional instance. (Layers are the loader's domain -- a modern loader
    // wraps the ICD and strips them; we deliberately do not reject ppEnabledLayerNames, or a
    // loader-inserted layer like validation would fail.)
    const char* kInstExts[] = {VK_KHR_SURFACE_EXTENSION_NAME, "VK_KHR_xcb_surface",
                               "VK_KHR_xlib_surface",
                               VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME};
    if (pCreateInfo != nullptr) {
        const char* bad = first_unsupported_ext(pCreateInfo->enabledExtensionCount,
                                                pCreateInfo->ppEnabledExtensionNames, kInstExts, 4);
        if (bad != nullptr) {
            std::fprintf(stderr, "vkrelay2-icd: unsupported instance extension '%s'\n", bad);
            return VK_ERROR_EXTENSION_NOT_PRESENT;
        }
    }
    std::lock_guard<std::mutex> lk(g_mu);
    if (!ensure_connected()) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    try {
        const vkrpc::CreateInstanceResponse r =
            vkrpc::create_instance(*g_rpc, next_id(), vkrpc::CreateInstanceRequest{});
        if (!r.ok) {
            std::fprintf(stderr, "vkrelay2-icd: create_instance RPC ok=false reason='%s'\n",
                         r.reason.c_str());
            return fault();
        }
        auto* impl = new InstanceImpl{};
        set_loader_magic_value(impl);
        impl->worker = r.instance;
        *pInstance = reinterpret_cast<VkInstance>(impl);
        return VK_SUCCESS;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "vkrelay2-icd: create_instance RPC threw: %s\n", e.what());
        return VK_ERROR_INITIALIZATION_FAILED;
    }
}

VKAPI_ATTR void VKAPI_CALL DestroyInstance(VkInstance instance, const VkAllocationCallbacks*) {
    if (instance == VK_NULL_HANDLE) {
        return;
    }
    std::lock_guard<std::mutex> lk(g_mu);
    auto* impl = reinterpret_cast<InstanceImpl*>(instance);
    // observability: the caps-cache counters, so a convergence-smoke
    // failure explains itself (did the cache serve at all? did anything invalidate?).
    icd_trace("caps-cache: hits=%llu misses=%llu invalidations=%llu",
              static_cast<unsigned long long>(caps_cache().hits()),
              static_cast<unsigned long long>(caps_cache().misses()),
              static_cast<unsigned long long>(caps_cache().invalidations()));
    try {
        vkrpc::HandleRequest req;
        req.handle = impl->worker;
        (void) vkrpc::destroy_instance(*g_rpc, next_id(), req);
    } catch (const std::exception&) {
    }
    delete impl;
}

VKAPI_ATTR VkResult VKAPI_CALL EnumeratePhysicalDevices(VkInstance instance, std::uint32_t* pCount,
                                                        VkPhysicalDevice* pDevices) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto* inst = reinterpret_cast<InstanceImpl*>(instance);
    try {
        // Populate the per-instance physical-device cache exactly ONCE (the handles must be stable
        // for the instance's lifetime -- see InstanceImpl::physical_devices). Every enumerate call
        // then serves these same pointers, so the loader's cross-call handle reconciliation holds.
        if (inst->physical_devices.empty()) {
            vkrpc::EnumeratePhysicalDevicesRequest req;
            req.instance = inst->worker;
            const vkrpc::EnumeratePhysicalDevicesResponse r =
                vkrpc::enumerate_physical_devices(*g_rpc, next_id(), req);
            if (!r.ok) {
                return fault();
            }
            for (std::size_t i = 0; i < r.devices.size(); ++i) {
                auto owned = std::make_unique<PhysicalDeviceImpl>();
                PhysicalDeviceImpl* pd = owned.get();
                set_loader_magic_value(pd);
                pd->worker = r.devices[i].handle;
                pd->instance_worker = inst->worker;
                pd->caps = r.devices[i].caps;
                g_raw_readback = r.devices[i].caps.raw_readback; // per-worker
                g_raw_record = r.devices[i].caps.raw_record &&
                               std::getenv("VKRELAY2_NO_RAW_RECORD") == nullptr; // opt-out fallback
                pd->host_device_extensions = r.devices[i].device_extensions;     //   (may be empty)
                // Fetch + cache the honest host memory-properties table now, so the void getter
                // never RPCs. Fail closed to a zeroed table (0 types/heaps) +
                // a loud log.
                vkrpc::GetPhysicalDeviceMemoryPropertiesRequest mreq;
                mreq.physical_device = pd->worker;
                const vkrpc::GetPhysicalDeviceMemoryPropertiesResponse mr =
                    vkrpc::get_physical_device_memory_properties(*g_rpc, next_id(), mreq);
                if (mr.ok) {
                    pd->mem_props.memoryTypeCount = static_cast<std::uint32_t>(
                        mr.types.size() < VK_MAX_MEMORY_TYPES ? mr.types.size()
                                                              : VK_MAX_MEMORY_TYPES);
                    for (std::uint32_t t = 0; t < pd->mem_props.memoryTypeCount; ++t) {
                        pd->mem_props.memoryTypes[t].propertyFlags =
                            static_cast<VkMemoryPropertyFlags>(mr.types[t].property_flags);
                        pd->mem_props.memoryTypes[t].heapIndex =
                            static_cast<std::uint32_t>(mr.types[t].heap_index);
                    }
                    pd->mem_props.memoryHeapCount = static_cast<std::uint32_t>(
                        mr.heaps.size() < VK_MAX_MEMORY_HEAPS ? mr.heaps.size()
                                                              : VK_MAX_MEMORY_HEAPS);
                    for (std::uint32_t hh = 0; hh < pd->mem_props.memoryHeapCount; ++hh) {
                        pd->mem_props.memoryHeaps[hh].size = mr.heaps[hh].size;
                        pd->mem_props.memoryHeaps[hh].flags =
                            static_cast<VkMemoryHeapFlags>(mr.heaps[hh].flags);
                    }
                } else {
                    std::fprintf(stderr,
                                 "vkrelay2-icd: could not fetch host memory properties (%s); "
                                 "reporting an empty table (fail-closed)\n",
                                 mr.reason.c_str());
                }
                inst->physical_devices.push_back(std::move(owned));
            }
        }
        const auto count = static_cast<std::uint32_t>(inst->physical_devices.size());
        if (pDevices == nullptr) {
            *pCount = count;
            return VK_SUCCESS;
        }
        const std::uint32_t n = *pCount < count ? *pCount : count;
        for (std::uint32_t i = 0; i < n; ++i) {
            pDevices[i] = reinterpret_cast<VkPhysicalDevice>(inst->physical_devices[i].get());
        }
        *pCount = n;
        return n < count ? VK_INCOMPLETE : VK_SUCCESS;
    } catch (const std::exception&) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
}

// (GL/zink): the wire size of a VkPhysicalDeviceFeatures2 / Properties2 pNext capability
// struct, by sType (0 = not forwarded -> the ICD leaves the app's struct zeroed). zink chains these
// extended structs and reads the host's real values; grow this table as the proof ladder needs
// more.
std::uint32_t capability_struct_size(std::uint32_t sType) {
    switch (sType) {
    // Features (1.1/1.2/1.3 core rollups -- cover the vast majority of what zink checks).
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES:
        return sizeof(VkPhysicalDeviceVulkan11Features);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES:
        return sizeof(VkPhysicalDeviceVulkan12Features);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES:
        return sizeof(VkPhysicalDeviceVulkan13Features);
    // Properties (1.1/1.2/1.3 core rollups + the standalone ones zink probes).
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES:
        return sizeof(VkPhysicalDeviceVulkan11Properties);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES:
        return sizeof(VkPhysicalDeviceVulkan12Properties);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES:
        return sizeof(VkPhysicalDeviceVulkan13Properties);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES:
        return sizeof(VkPhysicalDeviceSubgroupProperties);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES:
        return sizeof(VkPhysicalDeviceDriverProperties);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_3_PROPERTIES:
        return sizeof(VkPhysicalDeviceMaintenance3Properties);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_PROPERTIES:
        return sizeof(VkPhysicalDeviceMaintenance4Properties);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES:
        return sizeof(VkPhysicalDeviceDescriptorIndexingProperties);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_STENCIL_RESOLVE_PROPERTIES:
        return sizeof(VkPhysicalDeviceDepthStencilResolveProperties);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_PROPERTIES:
        return sizeof(VkPhysicalDeviceTimelineSemaphoreProperties);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES:
        return sizeof(VkPhysicalDeviceScalarBlockLayoutFeatures);
    // Required-feature audit (hardening): the standalone hostQueryReset feature struct
    // (the core-1.2 sType; the EXT alias shares the value). Forwarded so an app enabling
    // hostQueryReset via the standalone spelling (not the f12 rollup) still enables it on the host
    // device -- the reset gate then agrees with what the host actually supports (alias
    // consistency).
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES:
        return sizeof(VkPhysicalDeviceHostQueryResetFeatures);
    // Required-feature audit (multiview): the standalone feature struct (core-1.1
    // sType; the KHR alias shares the value). Forwarded so an app probing/enabling multiview via
    // the standalone spelling sees the same served answer as the Vulkan11Features rollup; masked
    // below (multiview passes through host-TRUE,
    // multiviewGeometryShader/multiviewTessellationShader FALSE -- served-vs-unserved).
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES:
        return sizeof(VkPhysicalDeviceMultiviewFeatures);
    // Promoted-feature CONSISTENCY (surfaced by the Vulkan CTS
    // dEQP-VK.api.info.vulkan1p2.feature_extensions_consistency): the standalone spelling of every
    // 1.1/1.2 promoted feature struct must agree with its Vulkan11/12Features ROLLUP member. We
    // forwarded the rollups but not these standalones, so a standalone read ZERO while the rollup
    // read the host value -> mismatch. Forward them all; they pass through the host value UNMASKED
    // (matching the unmasked rollup member) EXCEPT protectedMemory + samplerYcbcrConversion, which
    // are masked FALSE below exactly as the Vulkan11Features rollup masks them (unserved). The ones
    // already listed above (multiview / hostQueryReset / bufferDeviceAddress / descriptorIndexing /
    // scalarBlockLayout) keep their existing masks.
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES:
        return sizeof(VkPhysicalDevice16BitStorageFeatures);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES:
        return sizeof(VkPhysicalDevice8BitStorageFeatures);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VARIABLE_POINTERS_FEATURES:
        return sizeof(VkPhysicalDeviceVariablePointersFeatures);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES:
        return sizeof(VkPhysicalDeviceShaderDrawParametersFeatures);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_FEATURES: // masked FALSE below
        return sizeof(VkPhysicalDeviceProtectedMemoryFeatures);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES: // masked FALSE below
        return sizeof(VkPhysicalDeviceSamplerYcbcrConversionFeatures);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_INT64_FEATURES:
        return sizeof(VkPhysicalDeviceShaderAtomicInt64Features);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES:
        return sizeof(VkPhysicalDeviceShaderFloat16Int8Features);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGELESS_FRAMEBUFFER_FEATURES:
        return sizeof(VkPhysicalDeviceImagelessFramebufferFeatures);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_UNIFORM_BUFFER_STANDARD_LAYOUT_FEATURES:
        return sizeof(VkPhysicalDeviceUniformBufferStandardLayoutFeatures);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_SUBGROUP_EXTENDED_TYPES_FEATURES:
        return sizeof(VkPhysicalDeviceShaderSubgroupExtendedTypesFeatures);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SEPARATE_DEPTH_STENCIL_LAYOUTS_FEATURES:
        return sizeof(VkPhysicalDeviceSeparateDepthStencilLayoutsFeatures);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES:
        return sizeof(VkPhysicalDeviceTimelineSemaphoreFeatures);
    // (VkPhysicalDeviceVulkanMemoryModelFeatures is already forwarded in the Vulkan-1.3 block
    // below.) Promoted-PROPERTY consistency
    // (dEQP-VK.api.info.vulkan1p2.property_extensions_consistency): the standalone spelling of
    // every 1.1/1.2 promoted property struct must agree with its Vulkan11/12Properties rollup.
    // Properties are characteristics, not gated features, so these pass the host value through
    // UNMASKED (matching the rollup). (Subgroup/Driver/Maintenance3/
    // DescriptorIndexing/DepthStencilResolve/TimelineSemaphore properties are already listed
    // above.)
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES:
        return sizeof(VkPhysicalDeviceIDProperties);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_POINT_CLIPPING_PROPERTIES:
        return sizeof(VkPhysicalDevicePointClippingProperties);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_PROPERTIES:
        return sizeof(VkPhysicalDeviceMultiviewProperties);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_PROPERTIES:
        return sizeof(VkPhysicalDeviceProtectedMemoryProperties);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FLOAT_CONTROLS_PROPERTIES:
        return sizeof(VkPhysicalDeviceFloatControlsProperties);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_FILTER_MINMAX_PROPERTIES:
        return sizeof(VkPhysicalDeviceSamplerFilterMinmaxProperties);
    // VkPhysicalDeviceFeatures2 itself (the head of the create_device enabled-feature chain).
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2:
        return sizeof(VkPhysicalDeviceFeatures2);
    // Ext feature/property structs for the advertised device extensions (zink chains these in
    // Features2/Properties2 + the device create-info; see the trace's "unknown sType skipped").
    // VK_EXT_depth_clip_enable: zink chains this in Features2 + the device create-info to learn /
    // enable depthClipEnable (GL's per-pipeline near/far clip toggle). Without it reported honest,
    // zink renders incorrectly (OpenSCAD CSG preview goes black).
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_ENABLE_FEATURES_EXT:
        return sizeof(VkPhysicalDeviceDepthClipEnableFeaturesEXT);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LINE_RASTERIZATION_FEATURES_EXT:
        return sizeof(VkPhysicalDeviceLineRasterizationFeaturesEXT);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LINE_RASTERIZATION_PROPERTIES_EXT:
        return sizeof(VkPhysicalDeviceLineRasterizationPropertiesEXT);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT:
        return sizeof(VkPhysicalDeviceCustomBorderColorFeaturesEXT);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_PROPERTIES_EXT:
        return sizeof(VkPhysicalDeviceCustomBorderColorPropertiesEXT);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT:
        return sizeof(VkPhysicalDeviceTransformFeedbackFeaturesEXT);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_PROPERTIES_EXT:
        return sizeof(VkPhysicalDeviceTransformFeedbackPropertiesEXT);
    // (GL/zink): VK_EXT_conditional_rendering feature struct (zink chains it in Features2 +
    // the device create-info; the extension is one of zink's two OpenGL 3.0 gates).
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CONDITIONAL_RENDERING_FEATURES_EXT:
        return sizeof(VkPhysicalDeviceConditionalRenderingFeaturesEXT);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT:
        return sizeof(VkPhysicalDeviceRobustness2FeaturesEXT);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_PROPERTIES_EXT:
        return sizeof(VkPhysicalDeviceRobustness2PropertiesEXT);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT:
        return sizeof(VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_PROPERTIES_EXT:
        return sizeof(VkPhysicalDeviceVertexAttributeDivisorPropertiesEXT);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT:
        return sizeof(VkPhysicalDeviceExtendedDynamicStateFeaturesEXT);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT:
        return sizeof(VkPhysicalDeviceExtendedDynamicState2FeaturesEXT);
    // (native lane): VK_KHR_dynamic_rendering's feature struct (the app chains
    // it in Features2 + the device create-info to learn/enable dynamicRendering). The KHR and
    // core-1.3 sTypes alias to the same value + struct. Masked FALSE off the native DR lane below.
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES:
        return sizeof(VkPhysicalDeviceDynamicRenderingFeatures);
    // (native lane): VK_KHR_synchronization2's feature struct. The KHR and
    // core-1.3 sTypes alias to the same value + struct. Masked FALSE off the native sync2 lane
    // below.
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES:
        return sizeof(VkPhysicalDeviceSynchronization2Features);
    // Vulkan 1.2 bufferDeviceAddress support: the standalone feature struct (the core and KHR
    // sTypes alias). An app chains it to probe/enable the feature without the 1.2 rollup. Masked
    // below: bufferDeviceAddress native-lane-only, captureReplay/multiDevice always FALSE
    // (unwired). The DEPRECATED VK_EXT_buffer_device_address struct (a DIFFERENT sType with
    // different semantics) is deliberately NOT forwarded.
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES:
        return sizeof(VkPhysicalDeviceBufferDeviceAddressFeatures);
    //   (descriptorIndexing): the standalone feature struct (core-1.2 sType; the EXT
    // alias shares the value). An app chains it to probe/enable the served buffer-only subset
    // without the 1.2 rollup. Masked below to kDIFeatureServedBits on the native lane, all-FALSE
    // off it.
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES:
        return sizeof(VkPhysicalDeviceDescriptorIndexingFeatures);
    // Vulkan 1.3 support: the standalone spellings of the promoted 1.3-family feature
    // structs (core sTypes; the KHR/EXT aliases share the values). Forwarded so an app probing a
    // single feature sees the same answer as the f13 rollup; masked below to the served set on
    // an honest 1.3 device (vk13_device_ok), all-FALSE otherwise. The memory-model struct is a
    // 1.2 rollup member set and passes through UNMASKED on both lanes, exactly like its rollup
    // fields always have.
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_ROBUSTNESS_FEATURES:
        return sizeof(VkPhysicalDeviceImageRobustnessFeatures);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INLINE_UNIFORM_BLOCK_FEATURES:
        return sizeof(VkPhysicalDeviceInlineUniformBlockFeatures);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_CREATION_CACHE_CONTROL_FEATURES:
        return sizeof(VkPhysicalDevicePipelineCreationCacheControlFeatures);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIVATE_DATA_FEATURES:
        return sizeof(VkPhysicalDevicePrivateDataFeatures);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DEMOTE_TO_HELPER_INVOCATION_FEATURES:
        return sizeof(VkPhysicalDeviceShaderDemoteToHelperInvocationFeatures);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_TERMINATE_INVOCATION_FEATURES:
        return sizeof(VkPhysicalDeviceShaderTerminateInvocationFeatures);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES:
        return sizeof(VkPhysicalDeviceSubgroupSizeControlFeatures);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ZERO_INITIALIZE_WORKGROUP_MEMORY_FEATURES:
        return sizeof(VkPhysicalDeviceZeroInitializeWorkgroupMemoryFeatures);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_INTEGER_DOT_PRODUCT_FEATURES:
        return sizeof(VkPhysicalDeviceShaderIntegerDotProductFeatures);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_FEATURES:
        return sizeof(VkPhysicalDeviceMaintenance4Features);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES:
        return sizeof(VkPhysicalDeviceVulkanMemoryModelFeatures);
    // ... and the promoted 1.3-family PROPERTIES structs (honest host limits, no mask).
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_PROPERTIES:
        return sizeof(VkPhysicalDeviceSubgroupSizeControlProperties);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INLINE_UNIFORM_BLOCK_PROPERTIES:
        return sizeof(VkPhysicalDeviceInlineUniformBlockProperties);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_INTEGER_DOT_PRODUCT_PROPERTIES:
        return sizeof(VkPhysicalDeviceShaderIntegerDotProductProperties);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TEXEL_BUFFER_ALIGNMENT_PROPERTIES:
        return sizeof(VkPhysicalDeviceTexelBufferAlignmentProperties);
    default:
        return 0;
    }
}

// (GL/zink): forward an app VkPhysicalDeviceFeatures2/Properties2 pNext chain to the
// worker, which fills each KNOWN struct from the real host, and copy the FILLED FIELDS back into
// the app's nodes (preserving each node's sType+pNext header). Unknown sTypes are left zeroed
// (honest: we do not fabricate). Caller holds g_mu. which: 0 = features, 1 = properties.
void forward_capability_chain(PhysicalDeviceImpl* pd, std::uint32_t which, void* pNextHead) {
    std::vector<VkBaseOutStructure*> nodes;
    vkrpc::GetPhysicalDeviceCapabilityChainRequest req;
    req.physical_device = pd->worker;
    req.which = which;
    for (auto* node = static_cast<VkBaseOutStructure*>(pNextHead); node != nullptr;
         node = node->pNext) {
        const std::uint32_t sz = capability_struct_size(static_cast<std::uint32_t>(node->sType));
        if (sz == 0) {
            icd_trace("capability chain (which=%u): unknown sType=%d skipped", which,
                      static_cast<int>(node->sType));
            continue;
        }
        vkrpc::CapabilityChainEntry e;
        e.s_type = static_cast<std::uint32_t>(node->sType);
        e.size = sz;
        req.entries.push_back(e);
        nodes.push_back(node);
    }
    if (req.entries.empty()) {
        return;
    }
    try {
        const vkrpc::GetPhysicalDeviceCapabilityChainResponse r =
            vkrpc::get_physical_device_capability_chain(*g_rpc, next_id(), req);
        if (!r.ok) {
            return;
        }
        const std::size_t hdr = sizeof(VkBaseOutStructure); // {sType, pNext} -- preserve it
        for (std::size_t i = 0; i < r.entries.size() && i < nodes.size(); ++i) {
            const std::uint32_t sz = capability_struct_size(r.entries[i].s_type);
            if (sz <= hdr || r.entries[i].blob.size() < sz) {
                continue;
            }
            std::memcpy(reinterpret_cast<char*>(nodes[i]) + hdr, r.entries[i].blob.data() + hdr,
                        sz - hdr);
            // Required-feature audit: the Vulkan11Features rollup was forwarded UNMASKED,
            // so every OPTIONAL f11 member the relay does not serve leaked through as host-TRUE --
            // an advertise-then-fail. Mask the unserved optional members FALSE on BOTH lanes:
            // protectedMemory + samplerYcbcrConversion (no protected-submit / no Ycbcr-conversion
            // object path -- zero code references either), and the multiviewGeometryShader /
            // multiviewTessellationShader variants (the viewMask machinery is not relayed). NOTE:
            // multiview itself is REQUIRED (since 1.1) and is NOT masked here -- WIRES it
            // (path A: serve, don't mask); it stays host-TRUE, gated by kRelayServesMultiview,
            // until then. Just-mask (matching the existing optional-feature masks): the mask is on
            // the REPORTED features, so a well-behaved app never enables them.
            if (which == 0 &&
                r.entries[i].s_type == static_cast<std::uint32_t>(
                                           VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES)) {
                auto* f11 = reinterpret_cast<VkPhysicalDeviceVulkan11Features*>(nodes[i]);
                f11->protectedMemory = VK_FALSE;
                f11->samplerYcbcrConversion = VK_FALSE;
                f11->multiviewGeometryShader = VK_FALSE;
                f11->multiviewTessellationShader = VK_FALSE;
            }
            // Required-feature audit (multiview): the STANDALONE
            // VkPhysicalDeviceMultiviewFeatures gets the SAME served-vs-unserved mask as the rollup
            // above -- multiview passes through host-TRUE (the render-pass2 and dynamic-rendering
            // paths serve it, real-GPU proven by the serve-proof canary), while the
            // geometry/tessellation variants stay FALSE (unrelayed
            // viewMask machinery). So the standalone struct and the rollup agree, on
            // BOTH lanes.
            if (which == 0 &&
                r.entries[i].s_type == static_cast<std::uint32_t>(
                                           VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES)) {
                auto* fmv = reinterpret_cast<VkPhysicalDeviceMultiviewFeatures*>(nodes[i]);
                fmv->multiviewGeometryShader = VK_FALSE;
                fmv->multiviewTessellationShader = VK_FALSE;
            }
            // Promoted-feature consistency (CTS vulkan1p2.feature_extensions_consistency): the two
            // standalone 1.1 feature structs whose rollup member the Vulkan11Features mask forces
            // FALSE must be masked identically, else the standalone reads host-TRUE while the
            // rollup reads FALSE. protectedMemory (no protected submit path) +
            // samplerYcbcrConversion (no Ycbcr-conversion object path) are unserved -- exactly the
            // two the f11 mask above clears.
            if (which == 0 &&
                r.entries[i].s_type ==
                    static_cast<std::uint32_t>(
                        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_FEATURES)) {
                reinterpret_cast<VkPhysicalDeviceProtectedMemoryFeatures*>(nodes[i])
                    ->protectedMemory = VK_FALSE;
            }
            if (which == 0 &&
                r.entries[i].s_type ==
                    static_cast<std::uint32_t>(
                        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES)) {
                reinterpret_cast<VkPhysicalDeviceSamplerYcbcrConversionFeatures*>(nodes[i])
                    ->samplerYcbcrConversion = VK_FALSE;
            }
            // (GL/zink): MASK features the relay cannot honor across the WSL->Windows
            // boundary, so the app never enables (then uses) them. Done on the REPORTED features
            // so create_device's enabled-feature chain never carries them either.
            //
            // bufferDeviceAddress: served on the NATIVE lane only -- the
            // address a shader dereferences is the REAL host VA (vkGetBufferDeviceAddress ships it
            // verbatim; it is meaningless to the guest CPU but exactly what the guest's GPU work
            // consumes, and that work runs on the host device that minted it). Off the native lane
            // it stays FALSE: the mask is the zink steering (bindless/BDA paths stay off the
            // default lane by construction). On the native lane the host's REAL value passes
            // through (auto-intersecting host support). captureReplay + multiDevice stay FALSE
            // everywhere (unwired -- opaque-capture-address structs and device masks are rejected
            // by name).
            if (which == 0 &&
                r.entries[i].s_type == static_cast<std::uint32_t>(
                                           VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES)) {
                auto* f12 = reinterpret_cast<VkPhysicalDeviceVulkan12Features*>(nodes[i]);
                if (!native_lane()) {
                    f12->bufferDeviceAddress = VK_FALSE;
                }
                f12->bufferDeviceAddressCaptureReplay = VK_FALSE;
                f12->bufferDeviceAddressMultiDevice = VK_FALSE;
                // (GL/zink) + (descriptorIndexing): on the DEFAULT lane the
                // WHOLE family is masked FALSE -- it steers zink onto its BINDLESS path
                // (VkDescriptorSetLayoutBindingFlagsCreateInfo pNext + update-after-bind +
                // variable/partially-bound counts); with it off, zink uses CLASSIC descriptor
                // sets (update-before-bind), which the relay forwards faithfully. On the NATIVE
                // lane, lets the host's REAL value pass through for the SERVED buffer-only
                // subset (kDIFeatureServedBits -- exactly what the real-GPU crux + the DI
                // canary prove); every UNSERVED member stays FALSE everywhere until its own
                // proof. The AGGREGATE bit stays FALSE even on the native lane: per the 1.2
                // feature requirements it asserts the extension's full minimum set, which
                // includes the deferred image/texel UAB + non-uniform-indexing classes --
                // reporting it TRUE while masking those would be an honesty bug.
                // Sub-feature bits TRUE with the aggregate FALSE is spec-legal.
                f12->descriptorIndexing = VK_FALSE;
                f12->shaderInputAttachmentArrayDynamicIndexing = VK_FALSE;
                f12->shaderUniformTexelBufferArrayDynamicIndexing = VK_FALSE;
                f12->shaderStorageTexelBufferArrayDynamicIndexing = VK_FALSE;
                f12->shaderSampledImageArrayNonUniformIndexing = VK_FALSE;
                f12->shaderStorageImageArrayNonUniformIndexing = VK_FALSE;
                f12->shaderInputAttachmentArrayNonUniformIndexing = VK_FALSE;
                f12->shaderUniformTexelBufferArrayNonUniformIndexing = VK_FALSE;
                f12->shaderStorageTexelBufferArrayNonUniformIndexing = VK_FALSE;
                f12->descriptorBindingSampledImageUpdateAfterBind = VK_FALSE;
                f12->descriptorBindingStorageImageUpdateAfterBind = VK_FALSE;
                f12->descriptorBindingUniformTexelBufferUpdateAfterBind = VK_FALSE;
                f12->descriptorBindingStorageTexelBufferUpdateAfterBind = VK_FALSE;
                if (!native_lane()) {
                    f12->shaderUniformBufferArrayNonUniformIndexing = VK_FALSE;
                    f12->shaderStorageBufferArrayNonUniformIndexing = VK_FALSE;
                    f12->descriptorBindingUniformBufferUpdateAfterBind = VK_FALSE;
                    f12->descriptorBindingStorageBufferUpdateAfterBind = VK_FALSE;
                    f12->descriptorBindingUpdateUnusedWhilePending = VK_FALSE;
                    f12->descriptorBindingPartiallyBound = VK_FALSE;
                    f12->descriptorBindingVariableDescriptorCount = VK_FALSE;
                    f12->runtimeDescriptorArray = VK_FALSE;
                }
                // NOTE: timelineSemaphore is NOT masked -- zink HARD-REQUIRES it (fails to load
                // otherwise, like imageless_framebuffer), so the relay implements it
                // (CreateSemaphore timeline type +
                // WaitSemaphores/SignalSemaphore/GetSemaphoreCounterValue + the
                // VkTimelineSemaphoreSubmitInfo wait/signal values on queue_submit).
                //
                // Required-feature audit: OPTIONAL f12 members the relay does NOT serve
                // ride this same rollup pass-through as host-TRUE -- an advertise-then-fail. Mask
                // them FALSE on BOTH lanes (they are not core-version blockers -- none is required
                // for us, since we do not advertise their gating extensions):
                // shaderOutputViewportIndex + shaderOutputLayer (VK_EXT_shader_viewport_index_layer
                // not advertised; the viewport-index/layer output path is not relayed). Zero code
                // references any of these -> genuinely unserved; masking is honest + safe (an app
                // falls back instead of enabling-then-hitting an unwired path). Just-mask, matching
                // the existing optional-feature masks below (stippled lines /
                // inheritedConditionalRendering; geometryStreams is NO LONGER masked -- it passes
                // through and its pipeline pNext is carried): the mask is on the REPORTED features,
                // so a well-behaved app never enables them and create_device's chain never carries
                // them.
                // drawIndirectCount is now served only when the worker advertises the additive
                // vocabulary bit. Preserve the host's copied value as the authority; an old
                // worker or host-false feature remains FALSE.
                f12->drawIndirectCount =
                    vkr::icd_policy::indirect_count_feature_reported(
                        pd->caps.core_indirect_draw_count != 0, f12->drawIndirectCount != VK_FALSE)
                        ? VK_TRUE
                        : VK_FALSE;
                f12->shaderOutputViewportIndex = VK_FALSE;
                f12->shaderOutputLayer = VK_FALSE;
            }
            // (GL/zink): MASK the STIPPLED line sub-features. The non-stippled line mode
            // (rectangular/bresenham/smooth) rides the pipeline via the carried
            // VkPipelineRasterizationLineStateCreateInfoEXT, but a STIPPLED line additionally needs
            // VK_DYNAMIC_STATE_LINE_STIPPLE_EXT + vkCmdSetLineStippleEXT, which the relay does NOT
            // carry. Report the base feature honestly but stippled* off, so zink never enables
            // (then tries to drive) line stipple. (Same faithful-or-fail-closed discipline as the
            // f12 masks above.)
            if (which == 0 &&
                r.entries[i].s_type ==
                    static_cast<std::uint32_t>(
                        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LINE_RASTERIZATION_FEATURES_EXT)) {
                auto* lr =
                    reinterpret_cast<VkPhysicalDeviceLineRasterizationFeaturesEXT*>(nodes[i]);
                lr->stippledRectangularLines = VK_FALSE;
                lr->stippledBresenhamLines = VK_FALSE;
                lr->stippledSmoothLines = VK_FALSE;
            }
            // geometryStreams passes through the host value. Mesa 23.2 emits the SPIR-V
            // GeometryStreams capability for geometry shaders that use stream zero; masking this
            // feature produced an invalid host shader/pipeline even though the selected adapters
            // support it.
            // (GL/zink): MASK inheritedConditionalRendering -- it only concerns SECONDARY
            // command buffers, which do not exist across the relay boundary. Primary-buffer
            // conditional rendering (zink's other OpenGL 3.0 gate) is reported honestly.
            if (which == 0 &&
                r.entries[i].s_type ==
                    static_cast<std::uint32_t>(
                        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CONDITIONAL_RENDERING_FEATURES_EXT)) {
                auto* cr =
                    reinterpret_cast<VkPhysicalDeviceConditionalRenderingFeaturesEXT*>(nodes[i]);
                cr->inheritedConditionalRendering = VK_FALSE;
            }
            // (native-lane gate + the per-family 1.3 mask): the extendedDynamicState
            // feature is exposed ONLY on the native lane (where EDS1's commands are wired +
            // advertised). On the DEFAULT (zink) lane it is forced FALSE -- even though the host
            // supports it -- so zink never enables (then emits) the dynamic-state command surface
            // the 1.2 steering exists to avoid. This is the fail-closed mask that steering
            // requires: a 1.3-family feature bit is TRUE only where its API surface is actually
            // relayed. EDS2/3 features stay masked until their entry points are implemented.
            if (which == 0 &&
                r.entries[i].s_type ==
                    static_cast<std::uint32_t>(
                        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT) &&
                !advertises_native_device_ext(pd, VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME)) {
                auto* eds =
                    reinterpret_cast<VkPhysicalDeviceExtendedDynamicStateFeaturesEXT*>(nodes[i]);
                eds->extendedDynamicState = VK_FALSE;
            }
            // (native lane): the dynamicRendering feature is
            // TRUE only where its command surface is actually relayed AND advertised -- i.e. iff we
            // WILL advertise VK_KHR_dynamic_rendering for this physical device (native lane +
            // host-intersected). Tying the mask to actual advertising (not merely native_lane())
            // keeps the 1.2 + honest-extension story exact on a promoted-core host that reports the
            // feature but does not list the KHR extension: we neither advertise it nor forward it.
            if (which == 0 &&
                r.entries[i].s_type ==
                    static_cast<std::uint32_t>(
                        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES) &&
                !advertises_native_device_ext(pd, VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME)) {
                auto* dr = reinterpret_cast<VkPhysicalDeviceDynamicRenderingFeatures*>(nodes[i]);
                dr->dynamicRendering = VK_FALSE;
            }
            // synchronization2 is served on both lanes. Keep the feature false only when an older
            // host reports the promoted feature but cannot enable the KHR extension on our
            // apiVersion-1.2 default device.
            if (which == 0 &&
                r.entries[i].s_type ==
                    static_cast<std::uint32_t>(
                        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES) &&
                !host_supports_device_ext(pd, VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME)) {
                auto* s2 = reinterpret_cast<VkPhysicalDeviceSynchronization2Features*>(nodes[i]);
                s2->synchronization2 = VK_FALSE;
            }
            // (bufferDeviceAddress): the standalone feature struct gets the SAME policy as
            // its 1.2-rollup bits above -- bufferDeviceAddress passes through (the host's real
            // value) on the native lane only, captureReplay/multiDevice FALSE everywhere. No
            // extension predicate: this is core 1.2, gated on the lane alone.
            if (which == 0 &&
                r.entries[i].s_type ==
                    static_cast<std::uint32_t>(
                        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES)) {
                auto* bda =
                    reinterpret_cast<VkPhysicalDeviceBufferDeviceAddressFeatures*>(nodes[i]);
                if (!native_lane()) {
                    bda->bufferDeviceAddress = VK_FALSE;
                }
                bda->bufferDeviceAddressCaptureReplay = VK_FALSE;
                bda->bufferDeviceAddressMultiDevice = VK_FALSE;
            }
            //   (descriptorIndexing): the standalone feature struct gets the SAME policy
            // as its 1.2-rollup members above -- the SERVED buffer-only subset passes through
            // (the host's real values) on the native lane only; every unserved member FALSE
            // everywhere. No extension predicate: core 1.2, gated on the lane alone.
            if (which == 0 &&
                r.entries[i].s_type ==
                    static_cast<std::uint32_t>(
                        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES)) {
                auto* di = reinterpret_cast<VkPhysicalDeviceDescriptorIndexingFeatures*>(nodes[i]);
                di->shaderInputAttachmentArrayDynamicIndexing = VK_FALSE;
                di->shaderUniformTexelBufferArrayDynamicIndexing = VK_FALSE;
                di->shaderStorageTexelBufferArrayDynamicIndexing = VK_FALSE;
                di->shaderSampledImageArrayNonUniformIndexing = VK_FALSE;
                di->shaderStorageImageArrayNonUniformIndexing = VK_FALSE;
                di->shaderInputAttachmentArrayNonUniformIndexing = VK_FALSE;
                di->shaderUniformTexelBufferArrayNonUniformIndexing = VK_FALSE;
                di->shaderStorageTexelBufferArrayNonUniformIndexing = VK_FALSE;
                di->descriptorBindingSampledImageUpdateAfterBind = VK_FALSE;
                di->descriptorBindingStorageImageUpdateAfterBind = VK_FALSE;
                di->descriptorBindingUniformTexelBufferUpdateAfterBind = VK_FALSE;
                di->descriptorBindingStorageTexelBufferUpdateAfterBind = VK_FALSE;
                if (!native_lane()) {
                    di->shaderUniformBufferArrayNonUniformIndexing = VK_FALSE;
                    di->shaderStorageBufferArrayNonUniformIndexing = VK_FALSE;
                    di->descriptorBindingUniformBufferUpdateAfterBind = VK_FALSE;
                    di->descriptorBindingStorageBufferUpdateAfterBind = VK_FALSE;
                    di->descriptorBindingUpdateUnusedWhilePending = VK_FALSE;
                    di->descriptorBindingPartiallyBound = VK_FALSE;
                    di->descriptorBindingVariableDescriptorCount = VK_FALSE;
                    di->runtimeDescriptorArray = VK_FALSE;
                }
            }
            // The Vulkan 1.3 ROLLUP feature struct would otherwise leak EVERY 1.3 feature the host
            // supports even where
            // the device is capped at 1.2. Mask it to the honest served surface. Vulkan-1.2
            // (default lane / host below 1.3 / old worker): dynamicRendering + synchronization2
            // TRUE only where we actually advertise the extension; EVERY other field FALSE -- the
            // exact Vulkan-1.2 behavior. On an honest 1.3 DEVICE (vk13_device_ok): the SERVED
            // members pass the host's real values through (all required ones are host-TRUE by
            // the vk13_ready definition); the two UNSERVED members --
            // descriptorBindingInlineUniformBlockUpdateAfterBind (a descriptorIndexing-family
            // capability, deferred with the DI aggregate) and textureCompressionASTC_HDR
            // (optional; gates FORMATS the image admission does not carry) -- stay FALSE. Only
            // the feature fields are touched; the node's {sType, pNext} header is preserved.
            if (which == 0 &&
                r.entries[i].s_type == static_cast<std::uint32_t>(
                                           VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES)) {
                auto* f13 = reinterpret_cast<VkPhysicalDeviceVulkan13Features*>(nodes[i]);
                f13->descriptorBindingInlineUniformBlockUpdateAfterBind = VK_FALSE;
                f13->textureCompressionASTC_HDR = VK_FALSE;
                if (!vk13_device_ok(pd)) {
                    if (!advertises_native_device_ext(pd,
                                                      VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME)) {
                        f13->dynamicRendering = VK_FALSE;
                    }
                    f13->robustImageAccess = VK_FALSE;
                    f13->inlineUniformBlock = VK_FALSE;
                    f13->pipelineCreationCacheControl = VK_FALSE;
                    f13->privateData = VK_FALSE;
                    f13->shaderDemoteToHelperInvocation = VK_FALSE;
                    f13->shaderTerminateInvocation = VK_FALSE;
                    f13->subgroupSizeControl = VK_FALSE;
                    f13->computeFullSubgroups = VK_FALSE;
                    if (!host_supports_device_ext(pd, VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME)) {
                        f13->synchronization2 = VK_FALSE;
                    }
                    f13->shaderZeroInitializeWorkgroupMemory = VK_FALSE;
                    f13->shaderIntegerDotProduct = VK_FALSE;
                    f13->maintenance4 = VK_FALSE; // queries unwired Vulkan-1.2
                }
            }
            // Vulkan 1.3 support: the standalone spellings of the served 1.3-family
            // feature structs get the SAME policy as their f13-rollup members -- host values
            // through on an honest 1.3 device, all-FALSE otherwise (and the unserved
            // descriptorBindingInlineUniformBlockUpdateAfterBind member FALSE everywhere). The
            // memory-model struct passes through unmasked on both lanes, matching its 1.2-rollup
            // members' long-standing pass-through.
            if (which == 0 && !vk13_device_ok(pd)) {
                const std::size_t body = sizeof(VkBaseOutStructure); // zero AFTER {sType, pNext}
                switch (r.entries[i].s_type) {
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_ROBUSTNESS_FEATURES:
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INLINE_UNIFORM_BLOCK_FEATURES:
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_CREATION_CACHE_CONTROL_FEATURES:
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIVATE_DATA_FEATURES:
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DEMOTE_TO_HELPER_INVOCATION_FEATURES:
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_TERMINATE_INVOCATION_FEATURES:
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES:
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ZERO_INITIALIZE_WORKGROUP_MEMORY_FEATURES:
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_INTEGER_DOT_PRODUCT_FEATURES:
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_FEATURES: {
                    // Every member after the header is a VkBool32: zero them all (the exact
                    // Vulkan-1.2 "left zeroed / not forwarded" answer, now explicit).
                    const std::uint32_t sz = capability_struct_size(r.entries[i].s_type);
                    std::memset(reinterpret_cast<char*>(nodes[i]) + body, 0, sz - body);
                    break;
                }
                default:
                    break;
                }
            }
            if (which == 0 &&
                r.entries[i].s_type ==
                    static_cast<std::uint32_t>(
                        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INLINE_UNIFORM_BLOCK_FEATURES)) {
                // The UAB member is a descriptorIndexing-family capability, deferred with the DI
                // aggregate -- FALSE even on an honest 1.3 device.
                reinterpret_cast<VkPhysicalDeviceInlineUniformBlockFeatures*>(nodes[i])
                    ->descriptorBindingInlineUniformBlockUpdateAfterBind = VK_FALSE;
            }
            // Query pools: MASK transformFeedbackQueries in the PROPERTIES struct.
            // The relay accepts OCCLUSION/TIMESTAMP/PIPELINE_STATISTICS query pools but REJECTS
            // TRANSFORM_FEEDBACK_STREAM (its indexed begin/end are unwired). If the host reports
            // transformFeedbackQueries=TRUE and we forwarded it, zink would believe XFB queries
            // work and pick a path we then reject at pool creation -- advertise-then-hope at the
            // capability layer. Report it FALSE until the indexed pair is wired + GL-verified, so
            // zink never tries. (which == 1 is the Properties2 chain.)
            if (which == 1 &&
                r.entries[i].s_type ==
                    static_cast<std::uint32_t>(
                        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_PROPERTIES_EXT)) {
                auto* tp =
                    reinterpret_cast<VkPhysicalDeviceTransformFeedbackPropertiesEXT*>(nodes[i]);
                tp->transformFeedbackQueries = VK_FALSE;
            }
        }
    } catch (const std::exception&) {
        // fail-closed: leave the app's structs as they were (zeroed)
    }
}

// (GL/zink): the FULL host VkPhysicalDeviceProperties (incl. the real limits), forwarded
// verbatim (op 65) and memoized. zink rejects a device whose limits are zero. Falls back to a
// synthesized minimal struct (the vkcube subset) if the worker cannot provide one.
VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceProperties(VkPhysicalDevice physicalDevice,
                                                       VkPhysicalDeviceProperties* pProperties) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto* pd = reinterpret_cast<PhysicalDeviceImpl*>(physicalDevice);
    if (!pd->props_cached) {
        // Synthesized minimal fallback (used if the RPC yields no full struct, e.g. the mock).
        VkPhysicalDeviceProperties p{};
        p.apiVersion = VK_API_VERSION_1_1;
        p.driverVersion = 1;
        p.vendorID = pd->caps.vendor_id;
        p.deviceID = pd->caps.device_id;
        p.deviceType = VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
        std::snprintf(p.deviceName, sizeof(p.deviceName), "%s",
                      pd->caps.device_name.empty() ? "vkrelay2" : pd->caps.device_name.c_str());
        p.limits.maxImageDimension2D = 16384;
        pd->full_props = p;
        try {
            vkrpc::GetPhysicalDevicePropertiesRequest req;
            req.physical_device = pd->worker;
            const vkrpc::GetPhysicalDevicePropertiesResponse r =
                vkrpc::get_physical_device_properties(*g_rpc, next_id(), req);
            if (r.ok && r.props_blob.size() == sizeof(VkPhysicalDeviceProperties)) {
                std::memcpy(&pd->full_props, r.props_blob.data(),
                            sizeof(VkPhysicalDeviceProperties));
                // (GL/zink): CAP the reported device API version at 1.2 on the DEFAULT
                // lane. A 1.3 device pulls zink onto its hardest-to-relay paths -- core
                // extended-dynamic-state (vkCmdSetCullMode / SetPatchControlPoints / ... emitted
                // unconditionally), synchronization2, and dynamic rendering. Reported as 1.2,
                // zink uses the render-pass path and BAKES rasterization/depth/topology state
                // into the pipeline (far fewer, simpler commands). The worker still creates the
                // real device at its true version; this only bounds what zink believes it may
                // use. (The limits + everything else stay the honest host values.)
                //
                // Vulkan 1.3 support: on the NATIVE lane, when the worker vouched that
                // the host can honestly back a 1.3 device (vk13_device_ok), the cap moves to 1.3
                // -- the required 1.3 feature set is served (the vk13 scalar), the core-1.3
                // command surface is wired (EDS core names, copy_commands2, private data,
                // maintenance4 queries, tool properties), and everything above 1.3 still caps.
                const std::uint32_t cap =
                    vk13_device_ok(pd) ? VK_API_VERSION_1_3 : VK_API_VERSION_1_2;
                if (pd->full_props.apiVersion > cap) {
                    pd->full_props.apiVersion = cap;
                }
                pd->props_cached = true; // memoize the honest full host struct
            } else if (r.ok) {
                pd->props_cached = true; // empty blob (mock) -> keep the synthesized fallback
            }
        } catch (const std::exception&) {
            // leave props_cached false -> the synthesized fallback below; retried next call
        }
    }
    *pProperties = pd->full_props;
}

// Vulkan 1.3 support: vkGetPhysicalDeviceToolProperties (core 1.3's one instance-level addition).
// The relay injects no tooling into the app's stream, so the honest answer is ZERO tools --
// spec-conformant for an implementation with no active tools.
VKAPI_ATTR VkResult VKAPI_CALL GetPhysicalDeviceToolProperties(VkPhysicalDevice,
                                                               std::uint32_t* pToolCount,
                                                               VkPhysicalDeviceToolProperties*) {
    if (pToolCount != nullptr) {
        *pToolCount = 0;
    }
    return VK_SUCCESS;
}

// Core 1.1. The MESA device-select implicit layer (auto-loaded on many Linux boxes) calls this on
// every physical device during vkEnumeratePhysicalDevices to pick one; a null entry here makes the
// layer call through a null pointer and crash. Fill the core properties and leave the caller's
// pNext chain (PCI-bus / driver / ID structs) untouched -- the worker is the real authority and the
// clear canary needs none; the layer falls back to the core properties to choose.
VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceProperties2(VkPhysicalDevice physicalDevice,
                                                        VkPhysicalDeviceProperties2* pProperties) {
    if (pProperties == nullptr) {
        return;
    }
    GetPhysicalDeviceProperties(physicalDevice, &pProperties->properties); // fills .properties
    icd_trace("Properties2 done (apiVersion=0x%x deviceType=%d maxImageDim2D=%u)",
              pProperties->properties.apiVersion,
              static_cast<int>(pProperties->properties.deviceType),
              pProperties->properties.limits.maxImageDimension2D);
    std::lock_guard<std::mutex> lk(g_mu);
    forward_capability_chain(reinterpret_cast<PhysicalDeviceImpl*>(physicalDevice), /*which=*/1,
                             pProperties->pNext);
}

// (GL/zink): the host's REAL VkPhysicalDeviceFeatures. The void getter forwards once per
// physical device and memoizes; on RPC failure it serves a zeroed (fail-closed) set. The vkcube
// subset returned an all-zero set unconditionally, which a full GL->Vulkan driver (zink) cannot
// build a device against.
VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceFeatures(VkPhysicalDevice physicalDevice,
                                                     VkPhysicalDeviceFeatures* f) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto* pd = reinterpret_cast<PhysicalDeviceImpl*>(physicalDevice);
    if (!pd->features_cached) {
        try {
            vkrpc::GetPhysicalDeviceFeaturesRequest req;
            req.physical_device = pd->worker;
            const vkrpc::GetPhysicalDeviceFeaturesResponse r =
                vkrpc::get_physical_device_features(*g_rpc, next_id(), req);
            if (r.ok) {
                pd->features = vkrpc::unpack_physical_device_features(r.feature_bits);
                // Honesty: the relay serves NO sparse binding -- no sparse-capable queue is exposed
                // (the queue flags mask VK_QUEUE_SPARSE_BINDING_BIT off) and vkQueueBindSparse is
                // unwired. So every sparse feature must read FALSE, else an app that enables
                // sparseBinding then looks for a sparse queue that does not exist (the Vulkan CTS
                // DefaultDevice throws exactly this). Faithful-or-fail-closed.
                pd->features.sparseBinding = VK_FALSE;
                pd->features.sparseResidencyBuffer = VK_FALSE;
                pd->features.sparseResidencyImage2D = VK_FALSE;
                pd->features.sparseResidencyImage3D = VK_FALSE;
                pd->features.sparseResidency2Samples = VK_FALSE;
                pd->features.sparseResidency4Samples = VK_FALSE;
                pd->features.sparseResidency8Samples = VK_FALSE;
                pd->features.sparseResidency16Samples = VK_FALSE;
                pd->features.sparseResidencyAliased = VK_FALSE;
                pd->features_cached =
                    true; // memoize only an honest answer; retry on transient fail
            }
        } catch (const std::exception&) {
            // leave features_cached false -> the zeroed default below, retried on the next call
        }
    }
    *f = pd->features; // the honest host set, or the zeroed fail-closed default
}

// Features2 fills .features from the real host set AND forwards the pNext feature chain (Vulkan
// 1.1/1.2/1.3 + the known ext structs) from the real host -- zink reads these to gate GL caps and
// rejects the device if a required core feature reads false.
VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceFeatures2(VkPhysicalDevice physicalDevice,
                                                      VkPhysicalDeviceFeatures2* pFeatures) {
    if (pFeatures == nullptr) {
        return;
    }
    GetPhysicalDeviceFeatures(physicalDevice, &pFeatures->features); // fills .features
    icd_trace("Features2 done (samplerAniso=%d geometryShader=%d tessShader=%d)",
              pFeatures->features.samplerAnisotropy, pFeatures->features.geometryShader,
              pFeatures->features.tessellationShader);
    std::lock_guard<std::mutex> lk(g_mu);
    forward_capability_chain(reinterpret_cast<PhysicalDeviceImpl*>(physicalDevice), /*which=*/0,
                             pFeatures->pNext);
}

VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceQueueFamilyProperties(VkPhysicalDevice physicalDevice,
                                                                  std::uint32_t* pCount,
                                                                  VkQueueFamilyProperties* pProps) {
    if (pProps == nullptr) {
        *pCount = 1;
        return;
    }
    if (*pCount >= 1) {
        std::lock_guard<std::mutex> lk(g_mu);
        const auto* pd = reinterpret_cast<const PhysicalDeviceImpl*>(physicalDevice);
        VkQueueFamilyProperties q{};
        // Compute: the selected family's REAL flags when the worker advertised them
        // (masked to what the relay actually forwards); 0 = old worker -> the pre-compute
        // synthesis, and vkCreateComputePipelines fails closed on the missing COMPUTE bit.
        q.queueFlags =
            pd->caps.queue_flags != 0
                ? static_cast<VkQueueFlags>(
                      pd->caps.queue_flags &
                      (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT))
                : (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_TRANSFER_BIT);
        q.queueCount = 1;
        // Query pools (GL 3.3): report the host graphics family's real timestampValidBits (0 until
        // the worker carries it -> the prior synthesized behavior), so zink enables ARB_timer_query
        // only when the relay can honor timestamp queries.
        q.timestampValidBits = pd->caps.timestamp_valid_bits;
        q.minImageTransferGranularity = {1, 1, 1};
        pProps[0] = q;
        *pCount = 1;
    }
}

VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceMemoryProperties(VkPhysicalDevice physicalDevice,
                                                             VkPhysicalDeviceMemoryProperties* pM) {
    // A pure copy of the table cached at enumerate-time: the honest host properties,
    // or the fail-closed zeroed table if the worker could not provide them. No RPC (this entrypoint
    // is void and cannot report a failure).
    auto* pd = reinterpret_cast<PhysicalDeviceImpl*>(physicalDevice);
    *pM = pd->mem_props;
}

// Fetch + memoize the host format properties (32-bit VkFormatProperties + 64-bit
// VkFormatProperties3) for (physical device, format). Caller holds g_mu. Returns a fail-closed
// all-zero response (ok=false, not memoized -> retried) on RPC failure. Shared by the 1.0 and the
// *2 getters (honest format props; adds the 64-bit VkFormatProperties3 that zink
// probes on a 1.3 device).
vkrpc::GetPhysicalDeviceFormatPropertiesResponse fetch_format_props(PhysicalDeviceImpl* pd,
                                                                    VkFormat format) {
    const std::pair<std::uint64_t, int> key{pd->worker, static_cast<int>(format)};
    const auto cached = g_format_props.find(key);
    if (cached != g_format_props.end()) {
        return cached->second;
    }
    vkrpc::GetPhysicalDeviceFormatPropertiesResponse r;
    try {
        vkrpc::GetPhysicalDeviceFormatPropertiesRequest req;
        req.physical_device = pd->worker;
        req.format = static_cast<int>(format);
        r = vkrpc::get_physical_device_format_properties(*g_rpc, next_id(), req);
    } catch (const std::exception&) {
        r = vkrpc::GetPhysicalDeviceFormatPropertiesResponse{}; // fail-closed (ok=false)
    }
    if (r.ok) {
        g_format_props[key] = r; // memoize only an honest answer
    }
    return r;
}

VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceFormatProperties(VkPhysicalDevice physicalDevice,
                                                             VkFormat format,
                                                             VkFormatProperties* p) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto* pd = reinterpret_cast<PhysicalDeviceImpl*>(physicalDevice);
    const vkrpc::GetPhysicalDeviceFormatPropertiesResponse r = fetch_format_props(pd, format);
    p->linearTilingFeatures = static_cast<VkFormatFeatureFlags>(r.linear_tiling_features);
    p->optimalTilingFeatures = static_cast<VkFormatFeatureFlags>(r.optimal_tiling_features);
    p->bufferFeatures = static_cast<VkFormatFeatureFlags>(r.buffer_features);
}

// (GL/zink): FormatProperties2 fills the 1.0 VkFormatProperties AND any VkFormatProperties3
// in the pNext chain with the 64-bit VkFormatFeatureFlags2. zink probes VkFormatProperties3 on
// a 1.3 device; without the 64-bit fill Mesa's dri_fill_in_modes() returns NULL -> zero GLX
// configs.
VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceFormatProperties2(VkPhysicalDevice physicalDevice,
                                                              VkFormat format,
                                                              VkFormatProperties2* p) {
    if (p == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lk(g_mu);
    auto* pd = reinterpret_cast<PhysicalDeviceImpl*>(physicalDevice);
    const vkrpc::GetPhysicalDeviceFormatPropertiesResponse r = fetch_format_props(pd, format);
    p->formatProperties.linearTilingFeatures =
        static_cast<VkFormatFeatureFlags>(r.linear_tiling_features);
    p->formatProperties.optimalTilingFeatures =
        static_cast<VkFormatFeatureFlags>(r.optimal_tiling_features);
    p->formatProperties.bufferFeatures = static_cast<VkFormatFeatureFlags>(r.buffer_features);
    for (auto* node = static_cast<VkBaseOutStructure*>(p->pNext); node != nullptr;
         node = node->pNext) {
        if (node->sType == VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3) {
            auto* fp3 = reinterpret_cast<VkFormatProperties3*>(node);
            fp3->linearTilingFeatures = r.linear_tiling_features2;
            fp3->optimalTilingFeatures = r.optimal_tiling_features2;
            fp3->bufferFeatures = r.buffer_features2;
        }
    }
}

// GetPhysicalDeviceImageFormatProperties is loader-REQUIRED (loader_icd_init_entries looks it up
// right after CreateInstance; a null pointer makes the loader drop the ICD). forwards the
// host's real answer (the vkcube subset always returned FORMAT_NOT_SUPPORTED, so zink built no
// configs); on RPC failure it fails closed as FORMAT_NOT_SUPPORTED.
VKAPI_ATTR VkResult VKAPI_CALL GetPhysicalDeviceImageFormatProperties(
    VkPhysicalDevice physicalDevice, VkFormat format, VkImageType type, VkImageTiling tiling,
    VkImageUsageFlags usage, VkImageCreateFlags flags, VkImageFormatProperties* pProps) {
    if (pProps != nullptr) {
        *pProps = VkImageFormatProperties{};
    }
    std::lock_guard<std::mutex> lk(g_mu);
    auto* pd = reinterpret_cast<PhysicalDeviceImpl*>(physicalDevice);
    try {
        vkrpc::GetPhysicalDeviceImageFormatPropertiesRequest req;
        req.physical_device = pd->worker;
        req.format = static_cast<int>(format);
        req.image_type = static_cast<int>(type);
        req.tiling = static_cast<int>(tiling);
        req.usage = static_cast<std::uint64_t>(usage);
        req.flags = static_cast<std::uint64_t>(flags);
        const vkrpc::GetPhysicalDeviceImageFormatPropertiesResponse r =
            vkrpc::get_physical_device_image_format_properties(*g_rpc, next_id(), req);
        if (!r.ok || r.result != 0) {
            return r.ok ? static_cast<VkResult>(r.result) : VK_ERROR_FORMAT_NOT_SUPPORTED;
        }
        if (pProps != nullptr) {
            pProps->maxExtent = {r.max_extent_width, r.max_extent_height, r.max_extent_depth};
            pProps->maxMipLevels = r.max_mip_levels;
            pProps->maxArrayLayers = r.max_array_layers;
            pProps->sampleCounts = static_cast<VkSampleCountFlags>(r.sample_counts);
            pProps->maxResourceSize = static_cast<VkDeviceSize>(r.max_resource_size);
        }
        return VK_SUCCESS;
    } catch (const std::exception&) {
        return VK_ERROR_FORMAT_NOT_SUPPORTED; // fail-closed
    }
}

// (GL/zink): the *2 form unwraps the VkPhysicalDeviceImageFormatInfo2 and forwards to
// the 1.0 path (the base properties; no pNext sidecars in the subset).
VKAPI_ATTR VkResult VKAPI_CALL GetPhysicalDeviceImageFormatProperties2(
    VkPhysicalDevice physicalDevice, const VkPhysicalDeviceImageFormatInfo2* pInfo,
    VkImageFormatProperties2* pProps) {
    if (pInfo == nullptr || pProps == nullptr) {
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    }
    // External-image-format honesty (output-path tightened): NO
    // external handle types cross the relay boundary, consistent with the external-capabilities
    // trio. Zero any chained VkExternalImageFormatProperties output FIRST -- so the "chained
    // output reads all-zero" contract holds literally on BOTH the error and success paths --
    // then, if an external handle type was requested, answer the spec's
    // "combination not supported".
    for (auto* n = static_cast<VkBaseOutStructure*>(pProps->pNext); n != nullptr; n = n->pNext) {
        if (n->sType == VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES) {
            reinterpret_cast<VkExternalImageFormatProperties*>(n)->externalMemoryProperties =
                VkExternalMemoryProperties{};
        }
    }
    for (auto* n = static_cast<const VkBaseInStructure*>(pInfo->pNext); n != nullptr;
         n = n->pNext) {
        if (n->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO &&
            reinterpret_cast<const VkPhysicalDeviceExternalImageFormatInfo*>(n)->handleType != 0) {
            return VK_ERROR_FORMAT_NOT_SUPPORTED;
        }
    }
    return GetPhysicalDeviceImageFormatProperties(physicalDevice, pInfo->format, pInfo->type,
                                                  pInfo->tiling, pInfo->usage, pInfo->flags,
                                                  &pProps->imageFormatProperties);
}

VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceSparseImageFormatProperties(
    VkPhysicalDevice, VkFormat, VkImageType, VkSampleCountFlagBits, VkImageUsageFlags,
    VkImageTiling, std::uint32_t* pPropertyCount, VkSparseImageFormatProperties*) {
    if (pPropertyCount != nullptr) {
        *pPropertyCount = 0; // no sparse image formats reported
    }
}

// Core-1.1 *2 wrappers (the VK_KHR_get_physical_device_properties2 seven). Found by a
// third-party consumer (vulkaninfo 1.3.204 gates its ENTIRE extended-feature dump on the
// instance extension + these entry points): the ICD claimed instance 1.1 but served only four of
// the mandated seven. Each fills the core member from its v1 sibling and leaves the caller's
// OUTPUT pNext chains untouched (zeroed by the caller = the honest "not filled" answer for
// structs the relay does not serve).
VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceQueueFamilyProperties2(
    VkPhysicalDevice physicalDevice, std::uint32_t* pCount, VkQueueFamilyProperties2* pProps) {
    if (pProps == nullptr) {
        GetPhysicalDeviceQueueFamilyProperties(physicalDevice, pCount, nullptr);
        return;
    }
    std::vector<VkQueueFamilyProperties> base(pCount != nullptr ? *pCount : 0);
    GetPhysicalDeviceQueueFamilyProperties(physicalDevice, pCount, base.data());
    for (std::uint32_t i = 0; pCount != nullptr && i < *pCount; ++i) {
        pProps[i].queueFamilyProperties = base[i];
    }
}

VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceMemoryProperties2(
    VkPhysicalDevice physicalDevice, VkPhysicalDeviceMemoryProperties2* pProps) {
    if (pProps == nullptr) {
        return;
    }
    GetPhysicalDeviceMemoryProperties(physicalDevice, &pProps->memoryProperties);
}

VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceSparseImageFormatProperties2(
    VkPhysicalDevice, const VkPhysicalDeviceSparseImageFormatInfo2*, std::uint32_t* pCount,
    VkSparseImageFormatProperties2*) {
    if (pCount != nullptr) {
        *pCount = 0; // no sparse image formats reported (the v1 answer)
    }
}

// The remaining core-1.1 INSTANCE-level entries (found by mpv/libplacebo, which loads them
// unconditionally on a 1.1+ instance and SEGVs on a NULL -- unknown INSTANCE names return null,
// unlike the device-level named abort-stubs). The external-capabilities trio answers "no
// external handle types" (all-zero properties -- the honest answer: nothing crosses the relay
// boundary as an OS handle); device groups reports ONE single-device group.
VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceExternalBufferProperties(
    VkPhysicalDevice, const VkPhysicalDeviceExternalBufferInfo*,
    VkExternalBufferProperties* pProps) {
    if (pProps != nullptr) {
        pProps->externalMemoryProperties = VkExternalMemoryProperties{};
    }
}

VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceExternalSemaphoreProperties(
    VkPhysicalDevice, const VkPhysicalDeviceExternalSemaphoreInfo*,
    VkExternalSemaphoreProperties* pProps) {
    if (pProps != nullptr) {
        pProps->exportFromImportedHandleTypes = 0;
        pProps->compatibleHandleTypes = 0;
        pProps->externalSemaphoreFeatures = 0;
    }
}

VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceExternalFenceProperties(
    VkPhysicalDevice, const VkPhysicalDeviceExternalFenceInfo*, VkExternalFenceProperties* pProps) {
    if (pProps != nullptr) {
        pProps->exportFromImportedHandleTypes = 0;
        pProps->compatibleHandleTypes = 0;
        pProps->externalFenceFeatures = 0;
    }
}

VKAPI_ATTR VkResult VKAPI_CALL EnumeratePhysicalDeviceGroups(
    VkInstance instance, std::uint32_t* pCount, VkPhysicalDeviceGroupProperties* pProps) {
    if (pProps == nullptr) {
        if (pCount != nullptr) {
            *pCount = 1;
        }
        return VK_SUCCESS;
    }
    if (pCount == nullptr || *pCount == 0) {
        return VK_INCOMPLETE;
    }
    std::uint32_t one = 1;
    VkPhysicalDevice pd = VK_NULL_HANDLE;
    const VkResult r = EnumeratePhysicalDevices(instance, &one, &pd);
    if (r != VK_SUCCESS || one == 0) {
        *pCount = 0;
        return r == VK_SUCCESS ? VK_SUCCESS : r;
    }
    pProps[0].physicalDeviceCount = 1;
    pProps[0].physicalDevices[0] = pd;
    pProps[0].subsetAllocation = VK_FALSE;
    *pCount = 1;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL GetPhysicalDeviceSurfaceSupportKHR(VkPhysicalDevice physicalDevice,
                                                                  std::uint32_t queueFamilyIndex,
                                                                  VkSurfaceKHR surface,
                                                                  VkBool32* pSupported) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto* pd = reinterpret_cast<PhysicalDeviceImpl*>(physicalDevice);
    try {
        vkrpc::GetSurfaceSupportRequest req;
        req.physical_device = pd->worker;
        req.queue_family_index = static_cast<int>(queueFamilyIndex);
        req.surface = from_handle(surface);
        const vkrpc::GetSurfaceSupportResponse r =
            vkrpc::get_surface_support(*g_rpc, next_id(), req);
        if (!r.ok) {
            return fault();
        }
        *pSupported = r.supported ? VK_TRUE : VK_FALSE;
        return VK_SUCCESS;
    } catch (const std::exception&) {
        return VK_ERROR_SURFACE_LOST_KHR;
    }
}

// Response -> VkSurfaceCapabilitiesKHR (shared by the cached and over-the-wire paths).
void fill_surface_caps(const vkrpc::GetSurfaceCapabilitiesResponse& r,
                       VkSurfaceCapabilitiesKHR* pCaps) {
    VkSurfaceCapabilitiesKHR c{};
    c.minImageCount = static_cast<std::uint32_t>(r.min_image_count);
    c.maxImageCount = static_cast<std::uint32_t>(r.max_image_count);
    c.currentExtent.width = static_cast<std::uint32_t>(r.current_extent_width);   // sentinel
    c.currentExtent.height = static_cast<std::uint32_t>(r.current_extent_height); // sentinel
    c.minImageExtent.width = static_cast<std::uint32_t>(r.min_image_extent_width);
    c.minImageExtent.height = static_cast<std::uint32_t>(r.min_image_extent_height);
    c.maxImageExtent.width = static_cast<std::uint32_t>(r.max_image_extent_width);
    c.maxImageExtent.height = static_cast<std::uint32_t>(r.max_image_extent_height);
    c.maxImageArrayLayers = static_cast<std::uint32_t>(r.max_image_array_layers);
    c.supportedTransforms = static_cast<VkSurfaceTransformFlagsKHR>(r.supported_transforms);
    c.currentTransform = static_cast<VkSurfaceTransformFlagBitsKHR>(r.current_transform);
    c.supportedCompositeAlpha = static_cast<VkCompositeAlphaFlagsKHR>(r.supported_composite_alpha);
    c.supportedUsageFlags = static_cast<VkImageUsageFlags>(r.supported_usage_flags);
    *pCaps = c;
}

VKAPI_ATTR VkResult VKAPI_CALL GetPhysicalDeviceSurfaceCapabilitiesKHR(
    VkPhysicalDevice physicalDevice, VkSurfaceKHR surface, VkSurfaceCapabilitiesKHR* pCaps) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto* pd = reinterpret_cast<PhysicalDeviceImpl*>(physicalDevice);
    const std::uint64_t surf = from_handle(surface);
    // serve the steady-state poll from the cache (live-swapchain-gated; bring-up
    // always misses). A result-signal invalidation (acquire/present/create) empties it first.
    if (const vkrpc::GetSurfaceCapabilitiesResponse* hit = caps_cache().lookup(pd->worker, surf)) {
        fill_surface_caps(*hit, pCaps);
        return VK_SUCCESS;
    }
    try {
        vkrpc::GetSurfaceCapabilitiesRequest req;
        req.physical_device = pd->worker;
        req.surface = surf;
        const vkrpc::GetSurfaceCapabilitiesResponse r =
            vkrpc::get_surface_capabilities(*g_rpc, next_id(), req);
        if (!r.ok) {
            caps_cache().invalidate_surface(surf); // rule: a surface-scoped failure
            return fault();
        }
        caps_cache().store(pd->worker, surf, r);
        fill_surface_caps(r, pCaps);
        return VK_SUCCESS;
    } catch (const std::exception&) {
        caps_cache().invalidate_surface(surf); // rule: fail closed, never serve pre-fault caps
        return VK_ERROR_SURFACE_LOST_KHR;
    }
}

VKAPI_ATTR VkResult VKAPI_CALL GetPhysicalDeviceSurfaceFormatsKHR(VkPhysicalDevice physicalDevice,
                                                                  VkSurfaceKHR surface,
                                                                  std::uint32_t* pCount,
                                                                  VkSurfaceFormatKHR* pFormats) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto* pd = reinterpret_cast<PhysicalDeviceImpl*>(physicalDevice);
    try {
        vkrpc::GetSurfaceFormatsRequest req;
        req.physical_device = pd->worker;
        req.surface = from_handle(surface);
        const vkrpc::GetSurfaceFormatsResponse r =
            vkrpc::get_surface_formats(*g_rpc, next_id(), req);
        if (!r.ok) {
            return fault();
        }
        const auto count = static_cast<std::uint32_t>(r.formats.size());
        if (pFormats == nullptr) {
            *pCount = count;
            return VK_SUCCESS;
        }
        const std::uint32_t n = *pCount < count ? *pCount : count;
        for (std::uint32_t i = 0; i < n; ++i) {
            pFormats[i].format = static_cast<VkFormat>(r.formats[i].format);
            pFormats[i].colorSpace = static_cast<VkColorSpaceKHR>(r.formats[i].color_space);
        }
        *pCount = n;
        return n < count ? VK_INCOMPLETE : VK_SUCCESS;
    } catch (const std::exception&) {
        return VK_ERROR_SURFACE_LOST_KHR;
    }
}

VKAPI_ATTR VkResult VKAPI_CALL
GetPhysicalDeviceSurfacePresentModesKHR(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface,
                                        std::uint32_t* pCount, VkPresentModeKHR* pModes) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto* pd = reinterpret_cast<PhysicalDeviceImpl*>(physicalDevice);
    try {
        vkrpc::GetSurfacePresentModesRequest req;
        req.physical_device = pd->worker;
        req.surface = from_handle(surface);
        const vkrpc::GetSurfacePresentModesResponse r =
            vkrpc::get_surface_present_modes(*g_rpc, next_id(), req);
        if (!r.ok) {
            return fault();
        }
        const auto count = static_cast<std::uint32_t>(r.present_modes.size());
        if (pModes == nullptr) {
            *pCount = count;
            return VK_SUCCESS;
        }
        const std::uint32_t n = *pCount < count ? *pCount : count;
        for (std::uint32_t i = 0; i < n; ++i) {
            pModes[i] = static_cast<VkPresentModeKHR>(r.present_modes[i]);
        }
        *pCount = n;
        return n < count ? VK_INCOMPLETE : VK_SUCCESS;
    } catch (const std::exception&) {
        return VK_ERROR_SURFACE_LOST_KHR;
    }
}

// Worker-present ignores the app's xcb connection (the GPU presents to a host Win32 window), but
// reads the guest window XID so the surface is born correlated to its toplevel for the
// sidecar registry. The create-info is taken as an opaque pointer (the ABI matches
// PFN_vkCreateXcbSurfaceKHR, so no libxcb is linked); the XID is read via a local ABI mirror of
// VkXcbSurfaceCreateInfoKHR -- only the 32-bit `window` field, the connection pointer stays opaque.
// (VkXcbSurfaceCreateInfoKHR itself needs VK_USE_PLATFORM_XCB_KHR + xcb headers we don't pull in;
// the structure-type enum value is in core vulkan.h, so the sType guard is safe.)
struct VkrXcbSurfaceCreateInfoMirror {
    VkStructureType sType;
    const void* pNext;
    std::uint32_t flags;  // VkXcbSurfaceCreateFlagsKHR
    void* connection;     // xcb_connection_t* -- opaque, never dereferenced
    std::uint32_t window; // xcb_window_t (the guest XID)
};

VKAPI_ATTR VkResult VKAPI_CALL CreateXcbSurfaceKHR(VkInstance instance, const void* pCreateInfo,
                                                   const VkAllocationCallbacks*,
                                                   VkSurfaceKHR* pSurface) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto* inst = reinterpret_cast<InstanceImpl*>(instance);
    try {
        vkrpc::CreateSurfaceRequest req;
        req.instance = inst->worker;
        if (pCreateInfo != nullptr) {
            const auto* ci = reinterpret_cast<const VkrXcbSurfaceCreateInfoMirror*>(pCreateInfo);
            if (ci->sType == VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR) {
                req.platform = "xcb";
                req.xid = ci->window;
                // role_hint stays the default "UnknownPending" -- the sidecar owns role.
            }
        }
        const vkrpc::CreateSurfaceResponse r = vkrpc::create_surface(*g_rpc, next_id(), req);
        if (!r.ok) {
            return fault();
        }
        *pSurface = to_handle<VkSurfaceKHR>(r.surface);
        return VK_SUCCESS;
    } catch (const std::exception&) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
}

VKAPI_ATTR void VKAPI_CALL DestroySurfaceKHR(VkInstance, VkSurfaceKHR surface,
                                             const VkAllocationCallbacks*) {
    if (surface == VK_NULL_HANDLE) {
        return;
    }
    std::lock_guard<std::mutex> lk(g_mu);
    caps_cache().on_surface_destroyed(from_handle(surface)); // erase all surface state
    try {
        vkrpc::HandleRequest req;
        req.handle = from_handle(surface);
        (void) vkrpc::destroy_surface(*g_rpc, next_id(), req);
    } catch (const std::exception&) {
    }
}

VKAPI_ATTR VkBool32 VKAPI_CALL GetPhysicalDeviceXcbPresentationSupportKHR(VkPhysicalDevice,
                                                                          std::uint32_t, void*,
                                                                          std::uint32_t) {
    return VK_TRUE; // worker owns presentation
}

// VK_KHR_xlib_surface (Vulkan 1.3 support breadth, found by mpv/libplacebo: its X11 Vulkan context
// creates XLIB surfaces, not xcb ones, so the xcb-only surface story turned mpv away). An Xlib
// Window IS an xcb window id -- the same 32-bit XID on the same private display -- and the xcb
// path never dereferences its connection either, so the xlib spelling forwards through the
// IDENTICAL wire request. Mirrored struct so the ICD needs no Xlib headers (Window = unsigned
// long on Linux).
struct VkrXlibSurfaceCreateInfoMirror {
    VkStructureType sType;
    const void* pNext;
    std::uint32_t flags;  // VkXlibSurfaceCreateFlagsKHR
    void* dpy;            // Display* -- opaque, never dereferenced
    unsigned long window; // Window (the guest XID; values are 32-bit)
};

VKAPI_ATTR VkResult VKAPI_CALL CreateXlibSurfaceKHR(VkInstance instance, const void* pCreateInfo,
                                                    const VkAllocationCallbacks*,
                                                    VkSurfaceKHR* pSurface) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto* inst = reinterpret_cast<InstanceImpl*>(instance);
    try {
        vkrpc::CreateSurfaceRequest req;
        req.instance = inst->worker;
        if (pCreateInfo != nullptr) {
            const auto* ci = reinterpret_cast<const VkrXlibSurfaceCreateInfoMirror*>(pCreateInfo);
            if (ci->sType == VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR) {
                req.platform = "xcb"; // the same XID space; sidecar/worker key on the XID alone
                req.xid = static_cast<std::uint32_t>(ci->window);
            }
        }
        const vkrpc::CreateSurfaceResponse r = vkrpc::create_surface(*g_rpc, next_id(), req);
        if (!r.ok) {
            return fault();
        }
        *pSurface = to_handle<VkSurfaceKHR>(r.surface);
        return VK_SUCCESS;
    } catch (const std::exception&) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
}

VKAPI_ATTR VkBool32 VKAPI_CALL GetPhysicalDeviceXlibPresentationSupportKHR(VkPhysicalDevice,
                                                                           std::uint32_t, void*,
                                                                           unsigned long) {
    return VK_TRUE; // worker owns presentation (the xcb answer)
}

// =====================================================================================
// Device level
// =====================================================================================
// (GL/zink): the device-extension ALLOWLIST -- only extensions we have wired end-to-end (so
// the app never resolves a NULL PFN for an advertised-but-unbacked extension and crashes). The
// worker further intersects this with the real host support at create_device. Grow this list one
// wired extension at a time as the GL proof ladder demands. (maintenance1 is zink's flipped-Y
// baseline.)
const char* const kDeviceExtAllowlist[] = {
    VK_KHR_SWAPCHAIN_EXTENSION_NAME,
    VK_KHR_MAINTENANCE1_EXTENSION_NAME,
    VK_KHR_MAINTENANCE2_EXTENSION_NAME,
    VK_KHR_MAINTENANCE3_EXTENSION_NAME,
    // (audit): VK_KHR_maintenance4 is REMOVED from the advertised set. Its
    // device-query commands (vkGetDeviceBufferMemoryRequirementsKHR /
    // vkGetDeviceImageMemoryRequirementsKHR / vkGetDeviceImageSparseMemoryRequirementsKHR) are NOT
    // wired -- get_proc hands them a named abort-stub -- so advertising it violated this
    // allowlist's own "only extensions we have wired end-to-end" contract
    // (advertise-before-backend). maintenance4 is not a zink hard-requirement (unlike
    // timeline/imageless below), and OpenSCAD/glmark2 still render without it (verified). Re-add it
    // separately once the three queries are relayed (the capability_struct_size case for its
    // PROPERTIES struct is kept, harmless, for that day).
    VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME,
    VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME,
    VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME,
    VK_KHR_IMAGELESS_FRAMEBUFFER_EXTENSION_NAME, // zink HARD-REQUIRES it (fails to load without)
    VK_KHR_DESCRIPTOR_UPDATE_TEMPLATE_EXTENSION_NAME,
    VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME, // zink HARD-REQUIRES it; the relay implements it
    VK_KHR_SHADER_DRAW_PARAMETERS_EXTENSION_NAME,
    VK_KHR_SAMPLER_MIRROR_CLAMP_TO_EDGE_EXTENSION_NAME,
    VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME,
    VK_KHR_DRIVER_PROPERTIES_EXTENSION_NAME,
    VK_EXT_SCALAR_BLOCK_LAYOUT_EXTENSION_NAME,
    VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME,
    // (GL/zink) RENDER CORRECTNESS: zink names VK_EXT_depth_clip_enable a hard requirement
    // ("Incorrect rendering will happen because the Vulkan device doesn't support the
    // 'VK_EXT_depth_clip_enable' feature" -- observed with OpenSCAD's CSG preview rendering BLACK
    // through the relay). GL clips against the near/far planes by default but, unlike Vulkan core,
    // lets the app toggle that per-pipeline (glEnable(GL_DEPTH_CLAMP)); zink expresses it via the
    // depthClipEnable feature + a VkPipelineRasterizationDepthClipStateCreateInfoEXT pNext on the
    // rasterization state. The relay now reports the feature honestly (capability_struct_size
    // below) and CARRIES that rasterization pNext faithfully (icd_subset +
    // create_graphics_pipelines), so the real RTX 4080 clips exactly as zink intends.
    VK_EXT_DEPTH_CLIP_ENABLE_EXTENSION_NAME,
    // (GL/zink) RENDER CORRECTNESS: zink names VK_EXT_line_rasterization a "base Zink
    // requirement" ("Some incorrect rendering might occur ... doesn't support base Zink
    // requirements: have_EXT_line_rasterization"). It chains a
    // VkPipelineRasterizationLineStateCreateInfoEXT pNext on EVERY pipeline's rasterization state
    // to pick the GL line-rasterization mode. The relay now reports the feature honestly (with the
    // STIPPLED sub-features MASKED off in forward_capability_chain -- the line-stipple dynamic
    // command is not carried, so zink stays on non-stippled lines) and CARRIES that rasterization
    // pNext faithfully (icd_subset + create_graphics_pipelines).
    VK_EXT_LINE_RASTERIZATION_EXTENSION_NAME,
    VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME,
    VK_EXT_ROBUSTNESS_2_EXTENSION_NAME,
    // (GL/zink): the two extensions gating zink's OpenGL 3.0 (and with the already-advertised
    // depth_clip_enable / vertex_attribute_divisor + honest features, its 3.1/3.2/3.3). Without
    // them zink reports GL 2.1, and a GL-3.0-shaped app (a glad/GLEW loader that populates
    // glGenerateMipmap & co. only for a >=3.0 context) calls a NULL pointer and dies -- observed
    // as glmark2's SIGSEGV entering its texture-filter=mipmap scene. Both are advertised with
    // their command surfaces wired end-to-end (record -> validate -> replay):
    //   transform feedback: bind_transform_feedback_buffers / begin|end_transform_feedback /
    //     draw_indirect_byte_count (geometryStreams passes through the host value -- masking it
    //     produced invalid stream-zero SPIR-V from Mesa 23.2 -- and the pipeline admission CARRIES
    //     VkPipelineRasterizationStateStreamCreateInfoEXT on an enabled device, validated by the
    //     shared vkrpc::rasterization_stream_ok against the host's real stream properties;
    //     the vkCmdBegin/EndQueryIndexedEXT pair is NOT wired (query pools exist for
    //     OCCLUSION/TIMESTAMP/PIPELINE_STATISTICS, but XFB-stream queries need the indexed pair),
    //     so transformFeedbackQueries is reported FALSE + XFB-stream pools fail closed at creation
    //     -- an app driving XFB *queries* never starts one, same fail-closed posture).
    //   conditional rendering: begin|end_conditional_rendering (inheritedConditionalRendering
    //     MASKED off -- secondary command buffers do not cross the relay).
    VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME,
    VK_EXT_CONDITIONAL_RENDERING_EXTENSION_NAME,
    // Mesa 23.2 emits zero-stage classic barriers whose validity is gated by synchronization2.
    // The relay serves the full sync2 command/submit surface, so expose it on the Zink lane too.
    VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME,
    // Featureless promoted extension. Unlike legacy allowlist entries, advertisement also needs
    // the additive worker vocabulary bit; an empty old-worker host list must not uncap it.
    VK_KHR_DRAW_INDIRECT_COUNT_EXTENSION_NAME,
    // (GL/zink): VK_EXT_extended_dynamic_state(2) stays DELIBERATELY NOT advertised -- it
    // pulls zink onto the dynamic-state command surface (vkCmdSetCullMode / SetPatchControlPoints
    // ...) for EVERY pipeline. Without it (and on a reported 1.2 device, where it is not core),
    // zink bakes that state into the pipeline and emits a much smaller command stream.
};
constexpr std::uint32_t kDeviceExtAllowlistCount =
    static_cast<std::uint32_t>(sizeof(kDeviceExtAllowlist) / sizeof(kDeviceExtAllowlist[0]));

// the ADVERTISED device extensions = the wired allowlist INTERSECTED with the
// host's real support (an empty host list = old worker / mock -> the full allowlist, the prior
// policy-only behavior; the mock enables anything). Enumeration AND CreateDevice validation use
// this one list, so the guest can never observe -- or successfully request -- an extension the
// real device cannot enable ("do not advertise a capability and then hope"). Returned pointers
// alias the static allowlist storage. Caller holds g_mu.
// (native lane): the 1.3-family device extensions exposed ONLY on the native lane -- the
// same "wired end-to-end + intersected with real host support" contract as the default allowlist,
// but gated so zink/default NEVER sees them (the 1.2 steering stays untouched by construction). The
// opener wires VK_EXT_extended_dynamic_state (EDS1); dynamic_rendering / synchronization2 join this
// list when implemented. The reported apiVersion stays 1.2 until all required support is present --
// a 1.2 device honestly advertising an extension it truly serves.
const char* const kNativeLaneDeviceExtAllowlist[] = {
    VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME,
    // VK_KHR_dynamic_rendering (vkCmdBeginRendering/EndRendering + the
    // VkPipelineRenderingCreateInfo NULL-renderpass pipeline path). Its dependencies
    // (depth_stencil_resolve / create_renderpass2 / multiview) are all CORE in our 1.2-reported
    // device, so advertising it on 1.2 is legal; the dynamicRendering feature is masked FALSE off
    // this lane. synchronization2 is handled separately.
    VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME,
};
constexpr std::uint32_t kNativeLaneDeviceExtAllowlistCount = static_cast<std::uint32_t>(
    sizeof(kNativeLaneDeviceExtAllowlist) / sizeof(kNativeLaneDeviceExtAllowlist[0]));

std::vector<const char*> advertised_device_extensions(const PhysicalDeviceImpl* pd) {
    std::vector<const char*> out;
    out.reserve(kDeviceExtAllowlistCount + kNativeLaneDeviceExtAllowlistCount);
    const auto host_has = [&](const char* name) {
        return pd->host_device_extensions.empty() ||
               std::find(pd->host_device_extensions.begin(), pd->host_device_extensions.end(),
                         name) != pd->host_device_extensions.end();
    };
    for (std::uint32_t i = 0; i < kDeviceExtAllowlistCount; ++i) {
        if (std::strcmp(kDeviceExtAllowlist[i], VK_KHR_DRAW_INDIRECT_COUNT_EXTENSION_NAME) == 0 &&
            !vkr::icd_policy::indirect_count_extension_advertised(
                pd->caps.core_indirect_draw_count != 0, pd->host_device_extensions.empty(),
                host_has(VK_KHR_DRAW_INDIRECT_COUNT_EXTENSION_NAME))) {
            continue;
        }
        if (host_has(kDeviceExtAllowlist[i])) {
            out.push_back(kDeviceExtAllowlist[i]);
        }
    }
    if (native_lane()) {
        for (std::uint32_t i = 0; i < kNativeLaneDeviceExtAllowlistCount; ++i) {
            if (host_has(kNativeLaneDeviceExtAllowlist[i])) {
                out.push_back(kNativeLaneDeviceExtAllowlist[i]);
            }
        }
    }
    return out;
}

VKAPI_ATTR VkResult VKAPI_CALL EnumerateDeviceExtensionProperties(VkPhysicalDevice physicalDevice,
                                                                  const char*,
                                                                  std::uint32_t* pCount,
                                                                  VkExtensionProperties* pProps) {
    std::lock_guard<std::mutex> lk(g_mu);
    const auto* pd = reinterpret_cast<const PhysicalDeviceImpl*>(physicalDevice);
    const std::vector<const char*> exts = advertised_device_extensions(pd);
    icd_trace("EnumerateDeviceExtensionProperties (query=%d, advertising %zu of %u wired)",
              pProps == nullptr ? 0 : 1, exts.size(), kDeviceExtAllowlistCount);
    return enumerate_extensions(exts.data(), static_cast<std::uint32_t>(exts.size()), pCount,
                                pProps);
}

VKAPI_ATTR VkResult VKAPI_CALL CreateDevice(VkPhysicalDevice physicalDevice,
                                            const VkDeviceCreateInfo* pCreateInfo,
                                            const VkAllocationCallbacks*, VkDevice* pDevice) {
    // Validate device extensions against the wired allowlist + the queue request (we expose exactly
    // one family, app-index 0, count 1). Reject the unsupported rather than succeeding into a
    // device the app thinks has more than it does. The app's enabled extensions +
    // requested features are FORWARDED to the worker (which enables them on the real device), not
    // silently dropped.
    std::vector<std::string> enabled_exts;
    std::uint64_t enabled_feature_bits = 0;
    std::vector<vkrpc::CapabilityChainEntry> enabled_feature_chain;
    bool wants_dynamic_rendering = false;           // the ENABLED extension
    bool wants_dynamic_rendering_feature = false;   // ... and the ENABLED dynamicRendering feature
    bool wants_synchronization2 = false;            // the ENABLED extension
    bool wants_synchronization2_feature = false;    // ... and the ENABLED synchronization2 feature
    bool wants_host_query_reset_feature = false;    // hardening: ENABLED hostQueryReset (no ext)
    bool wants_multiview_feature = false;           // ENABLED multiview (core 1.1, no ext)
    bool wants_vertex_attr_divisor = false;         // vertex-attr-divisor: the ENABLED EXTENSION
    bool wants_vertex_attr_divisor_feature = false; // ... vertexAttributeInstanceRateDivisor
    bool wants_vertex_attr_zero_divisor_feature =
        false;                                      // ... vertexAttributeInstanceRateZeroDivisor
    bool wants_transform_feedback = false;          // geometry-stream: the ENABLED EXTENSION
    bool wants_geometry_streams_feature = false;    // ... and the ENABLED geometryStreams feature
    bool wants_draw_indirect_count = false;         // the ENABLED featureless KHR extension
    bool wants_draw_indirect_count_feature = false; // ... or promoted core-1.2 feature
    bool wants_bda_feature = false;    // the ENABLED bufferDeviceAddress feature (no ext)
    std::uint64_t wants_di_bits = 0;   // the ENABLED served descriptorIndexing bits
    bool wants_unserved_di = false;    // ... and whether any UNSERVED DI member was requested
    std::uint64_t wants_vk13_bits = 0; // Vulkan 1.3 support: the ENABLED served kVk13Feature* bits
    bool wants_unserved_vk13 = false;  // ... and whether any UNSERVED vk13 member was requested
    std::lock_guard<std::mutex> lk(g_mu);
    auto* pd = reinterpret_cast<PhysicalDeviceImpl*>(physicalDevice);
    if (pCreateInfo != nullptr) {
        // validate against the ADVERTISED (host-intersected) list, not the raw
        // allowlist -- requesting an extension this host lacks is EXTENSION_NOT_PRESENT here, the
        // spec-correct failure, never a silently-degraded device.
        const std::vector<const char*> advertised = advertised_device_extensions(pd);
        const char* bad = first_unsupported_ext(
            pCreateInfo->enabledExtensionCount, pCreateInfo->ppEnabledExtensionNames,
            advertised.data(), static_cast<std::uint32_t>(advertised.size()));
        if (bad != nullptr) {
            std::fprintf(stderr, "vkrelay2-icd: unsupported device extension '%s'\n", bad);
            return VK_ERROR_EXTENSION_NOT_PRESENT;
        }
        for (std::uint32_t i = 0; i < pCreateInfo->queueCreateInfoCount; ++i) {
            const VkDeviceQueueCreateInfo& q = pCreateInfo->pQueueCreateInfos[i];
            if (q.queueFamilyIndex != 0 || q.queueCount > 1) {
                std::fprintf(stderr,
                             "vkrelay2-icd: unsupported queue request (family %u, count %u)\n",
                             q.queueFamilyIndex, q.queueCount);
                return VK_ERROR_INITIALIZATION_FAILED;
            }
        }
        for (std::uint32_t i = 0; i < pCreateInfo->enabledExtensionCount; ++i) {
            enabled_exts.emplace_back(pCreateInfo->ppEnabledExtensionNames[i]);
            icd_trace("CreateDevice enabled extension: %s",
                      pCreateInfo->ppEnabledExtensionNames[i]);
            if (std::strcmp(pCreateInfo->ppEnabledExtensionNames[i],
                            VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME) == 0) {
                wants_dynamic_rendering = true;
            }
            if (std::strcmp(pCreateInfo->ppEnabledExtensionNames[i],
                            VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME) == 0) {
                wants_synchronization2 = true;
            }
            if (std::strcmp(pCreateInfo->ppEnabledExtensionNames[i],
                            VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME) == 0) {
                wants_vertex_attr_divisor = true;
            }
            if (std::strcmp(pCreateInfo->ppEnabledExtensionNames[i],
                            VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME) == 0) {
                wants_transform_feedback = true;
            }
            if (std::strcmp(pCreateInfo->ppEnabledExtensionNames[i],
                            VK_KHR_DRAW_INDIRECT_COUNT_EXTENSION_NAME) == 0) {
                wants_draw_indirect_count = true;
            }
        }
        // Features ride EITHER pEnabledFeatures (the vkcube path -> base bits) OR a
        // VkPhysicalDeviceFeatures2 + Vulkan11/12/13 + ext feature pNext chain (zink). Capture the
        // FULL enabled-feature pNext chain verbatim (known sTypes -> {sType, blob}) so the worker
        // rebuilds it into VkDeviceCreateInfo.pNext and the real device enables exactly what the
        // app asked for. If the app used pEnabledFeatures instead, capture the base bits.
        if (pCreateInfo->pEnabledFeatures != nullptr) {
            enabled_feature_bits =
                vkrpc::pack_physical_device_features(*pCreateInfo->pEnabledFeatures);
        }
        for (auto* node = static_cast<const VkBaseInStructure*>(pCreateInfo->pNext);
             node != nullptr; node = node->pNext) {
            // The Features2 spelling owns the core-1.0 feature members when pEnabledFeatures is
            // null. Preserve the same scalar mirror used by the worker's mock/device-state gates;
            // the real host still receives the original Features2 chain verbatim.
            if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2) {
                enabled_feature_bits = vkrpc::pack_physical_device_features(
                    reinterpret_cast<const VkPhysicalDeviceFeatures2*>(node)->features);
            }
            const std::uint32_t sz =
                capability_struct_size(static_cast<std::uint32_t>(node->sType));
            if (sz == 0) {
                icd_trace("CreateDevice: unknown enabled-feature sType=%d skipped",
                          static_cast<int>(node->sType));
                continue;
            }
            vkrpc::CapabilityChainEntry e;
            e.s_type = static_cast<std::uint32_t>(node->sType);
            e.size = sz;
            e.blob.assign(reinterpret_cast<const char*>(node), sz); // the app's requested struct
            enabled_feature_chain.push_back(std::move(e));
        }
        // drawIndirectCount has only the Vulkan12Features rollup spelling. The KHR extension is
        // featureless and is collected independently above; either path enables the commands.
        const auto collect_draw_indirect_count = [&](const VkBaseInStructure* n) {
            if (n->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES) {
                wants_draw_indirect_count_feature =
                    wants_draw_indirect_count_feature ||
                    reinterpret_cast<const VkPhysicalDeviceVulkan12Features*>(n)
                            ->drawIndirectCount != VK_FALSE;
            }
        };
        for (auto* node = static_cast<const VkBaseInStructure*>(pCreateInfo->pNext);
             node != nullptr; node = node->pNext) {
            collect_draw_indirect_count(node);
            if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2) {
                for (auto* n = static_cast<const VkBaseInStructure*>(
                         reinterpret_cast<const VkPhysicalDeviceFeatures2*>(node)->pNext);
                     n != nullptr; n = n->pNext) {
                    collect_draw_indirect_count(n);
                }
            }
        }
        if ((wants_draw_indirect_count || wants_draw_indirect_count_feature) &&
            pd->caps.core_indirect_draw_count == 0) {
            std::fprintf(stderr, "vkrelay2-icd: rejecting device -- indirect-count drawing "
                                 "requested but the worker lacks its command vocabulary\n");
            return VK_ERROR_FEATURE_NOT_PRESENT;
        }
        // did the app enable the dynamicRendering FEATURE (not
        // merely the extension)? Detect it from the standalone
        // VkPhysicalDeviceDynamicRenderingFeatures or the VkPhysicalDeviceVulkan13Features rollup,
        // chained directly on VkDeviceCreateInfo.pNext OR inside a VkPhysicalDeviceFeatures2
        // wrapper (zink-style). The DR gates require ext AND feature, so enabling the ext without
        // the feature -- which Vulkan forbids using DR through -- cannot admit a DR pipeline or
        // command.
        const auto node_enables_dr = [](const VkBaseInStructure* n) -> bool {
            if (n->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES) {
                return reinterpret_cast<const VkPhysicalDeviceDynamicRenderingFeatures*>(n)
                           ->dynamicRendering != VK_FALSE;
            }
            if (n->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES) {
                return reinterpret_cast<const VkPhysicalDeviceVulkan13Features*>(n)
                           ->dynamicRendering != VK_FALSE;
            }
            return false;
        };
        for (auto* node = static_cast<const VkBaseInStructure*>(pCreateInfo->pNext);
             node != nullptr && !wants_dynamic_rendering_feature; node = node->pNext) {
            if (node_enables_dr(node)) {
                wants_dynamic_rendering_feature = true;
            } else if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2) {
                for (auto* n = static_cast<const VkBaseInStructure*>(
                         reinterpret_cast<const VkPhysicalDeviceFeatures2*>(node)->pNext);
                     n != nullptr; n = n->pNext) {
                    if (node_enables_dr(n)) {
                        wants_dynamic_rendering_feature = true;
                        break;
                    }
                }
            }
        }
        // the same detection for the synchronization2 FEATURE (standalone struct
        // or the 1.3 rollup, direct or inside a Features2 wrapper). A SEPARATE walk (not folded
        // into the DR loop above) -- that loop short-circuits on !wants_dynamic_rendering_feature,
        // so neither feature's presence may hide the other's.
        const auto node_enables_sync2 = [](const VkBaseInStructure* n) -> bool {
            if (n->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES) {
                return reinterpret_cast<const VkPhysicalDeviceSynchronization2Features*>(n)
                           ->synchronization2 != VK_FALSE;
            }
            if (n->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES) {
                return reinterpret_cast<const VkPhysicalDeviceVulkan13Features*>(n)
                           ->synchronization2 != VK_FALSE;
            }
            return false;
        };
        for (auto* node = static_cast<const VkBaseInStructure*>(pCreateInfo->pNext);
             node != nullptr && !wants_synchronization2_feature; node = node->pNext) {
            if (node_enables_sync2(node)) {
                wants_synchronization2_feature = true;
            } else if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2) {
                for (auto* n = static_cast<const VkBaseInStructure*>(
                         reinterpret_cast<const VkPhysicalDeviceFeatures2*>(node)->pNext);
                     n != nullptr; n = n->pNext) {
                    if (node_enables_sync2(n)) {
                        wants_synchronization2_feature = true;
                        break;
                    }
                }
            }
        }
        // Required-feature audit (hardening): the same detection for the
        // hostQueryReset FEATURE (the standalone VkPhysicalDeviceHostQueryResetFeatures or the 1.2
        // ROLLUP spelling, direct or inside a Features2 wrapper). A separate walk, like sync2's.
        // The enable is forwarded to the worker (both spellings ride the capability chain), and the
        // scalar below lets both backends fail-closed device-level vkResetQueryPool unless enabled.
        const auto node_enables_host_query_reset = [](const VkBaseInStructure* n) -> bool {
            if (n->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES) {
                return reinterpret_cast<const VkPhysicalDeviceHostQueryResetFeatures*>(n)
                           ->hostQueryReset != VK_FALSE;
            }
            if (n->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES) {
                return reinterpret_cast<const VkPhysicalDeviceVulkan12Features*>(n)
                           ->hostQueryReset != VK_FALSE;
            }
            return false;
        };
        for (auto* node = static_cast<const VkBaseInStructure*>(pCreateInfo->pNext);
             node != nullptr && !wants_host_query_reset_feature; node = node->pNext) {
            if (node_enables_host_query_reset(node)) {
                wants_host_query_reset_feature = true;
            } else if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2) {
                for (auto* n = static_cast<const VkBaseInStructure*>(
                         reinterpret_cast<const VkPhysicalDeviceFeatures2*>(node)->pNext);
                     n != nullptr; n = n->pNext) {
                    if (node_enables_host_query_reset(n)) {
                        wants_host_query_reset_feature = true;
                        break;
                    }
                }
            }
        }
        // Required-feature audit: the same detection for the multiview FEATURE. A
        // separate walk, like the ones above -- no walk may hide another's feature. BOTH forwarded
        // spellings are recognized: the Vulkan11Features ROLLUP and the standalone
        // VkPhysicalDeviceMultiviewFeatures (added it to capability_struct_size + the
        // served-vs-unserved mask). Recognizing exactly the forwarded spellings keeps the scalar
        // and the worker's chain re-derivation in exact agreement.
        const auto node_enables_multiview = [](const VkBaseInStructure* n) -> bool {
            if (n->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES) {
                return reinterpret_cast<const VkPhysicalDeviceVulkan11Features*>(n)->multiview !=
                       VK_FALSE;
            }
            if (n->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES) {
                return reinterpret_cast<const VkPhysicalDeviceMultiviewFeatures*>(n)->multiview !=
                       VK_FALSE;
            }
            return false;
        };
        for (auto* node = static_cast<const VkBaseInStructure*>(pCreateInfo->pNext);
             node != nullptr && !wants_multiview_feature; node = node->pNext) {
            if (node_enables_multiview(node)) {
                wants_multiview_feature = true;
            } else if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2) {
                for (auto* n = static_cast<const VkBaseInStructure*>(
                         reinterpret_cast<const VkPhysicalDeviceFeatures2*>(node)->pNext);
                     n != nullptr; n = n->pNext) {
                    if (node_enables_multiview(n)) {
                        wants_multiview_feature = true;
                        break;
                    }
                }
            }
        }
        // vertex-attr-divisor: detect the two ENABLED feature bits from
        // VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT (direct or inside a Features2 wrapper).
        // Ext-only (no rollup spelling). A separate walk (no walk hides another's feature);
        // COLLECTS both bits (no short-circuit) since they independently gate the divisor VALUES a
        // pipeline may carry. The worker re-derives both from the forwarded chain + rejects a
        // scalar mismatch.
        const auto collect_divisor = [&](const VkBaseInStructure* n) {
            if (n->sType ==
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT) {
                const auto* f =
                    reinterpret_cast<const VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT*>(n);
                if (f->vertexAttributeInstanceRateDivisor != VK_FALSE) {
                    wants_vertex_attr_divisor_feature = true;
                }
                if (f->vertexAttributeInstanceRateZeroDivisor != VK_FALSE) {
                    wants_vertex_attr_zero_divisor_feature = true;
                }
            }
            // geometry-stream: detect the ENABLED geometryStreams feature from the standalone
            // VkPhysicalDeviceTransformFeedbackFeaturesEXT (ext-only, no rollup spelling). Rides
            // the same walk (direct or inside a Features2 wrapper); gates the rasterization-stream
            // pNext a pipeline may carry. The worker re-derives it from the forwarded chain +
            // rejects a scalar mismatch (the divisor normalization pattern).
            if (n->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT) {
                const auto* f =
                    reinterpret_cast<const VkPhysicalDeviceTransformFeedbackFeaturesEXT*>(n);
                if (f->geometryStreams != VK_FALSE) {
                    wants_geometry_streams_feature = true;
                }
            }
        };
        for (auto* node = static_cast<const VkBaseInStructure*>(pCreateInfo->pNext);
             node != nullptr; node = node->pNext) {
            collect_divisor(node);
            if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2) {
                for (auto* n = static_cast<const VkBaseInStructure*>(
                         reinterpret_cast<const VkPhysicalDeviceFeatures2*>(node)->pNext);
                     n != nullptr; n = n->pNext) {
                    collect_divisor(n);
                }
            }
        }
        // (bufferDeviceAddress): the same detection for the bufferDeviceAddress FEATURE
        // (the standalone struct or the 1.2 ROLLUP spelling, direct or inside a Features2
        // wrapper). A separate walk, like sync2's -- no walk may hide another's feature. This one
        // COLLECTS all three BDA bits (no short-circuit): captureReplay/multiDevice are reported
        // FALSE everywhere, so requesting them is rejected below, not forwarded.
        bool wants_bda_capture_replay = false;
        bool wants_bda_multi_device = false;
        const auto collect_bda = [&](const VkBaseInStructure* n) {
            if (n->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES) {
                const auto* f =
                    reinterpret_cast<const VkPhysicalDeviceBufferDeviceAddressFeatures*>(n);
                wants_bda_feature = wants_bda_feature || f->bufferDeviceAddress != VK_FALSE;
                wants_bda_capture_replay =
                    wants_bda_capture_replay || f->bufferDeviceAddressCaptureReplay != VK_FALSE;
                wants_bda_multi_device =
                    wants_bda_multi_device || f->bufferDeviceAddressMultiDevice != VK_FALSE;
            } else if (n->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES) {
                const auto* f = reinterpret_cast<const VkPhysicalDeviceVulkan12Features*>(n);
                wants_bda_feature = wants_bda_feature || f->bufferDeviceAddress != VK_FALSE;
                wants_bda_capture_replay =
                    wants_bda_capture_replay || f->bufferDeviceAddressCaptureReplay != VK_FALSE;
                wants_bda_multi_device =
                    wants_bda_multi_device || f->bufferDeviceAddressMultiDevice != VK_FALSE;
            }
        };
        for (auto* node = static_cast<const VkBaseInStructure*>(pCreateInfo->pNext);
             node != nullptr; node = node->pNext) {
            collect_bda(node);
            if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2) {
                for (auto* n = static_cast<const VkBaseInStructure*>(
                         reinterpret_cast<const VkPhysicalDeviceFeatures2*>(node)->pNext);
                     n != nullptr; n = n->pNext) {
                    collect_bda(n);
                }
            }
        }
        // Fail closed at the ICD, spec-correct: enabling a feature we REPORT
        // FALSE is VK_ERROR_FEATURE_NOT_PRESENT -- captureReplay/multiDevice are FALSE everywhere
        // (unwired), and bufferDeviceAddress is FALSE off the native lane. Rejecting here (instead
        // of forwarding the invalid enable) keeps the scalar feature bit and the forwarded chain
        // consistent in everything a conformant ICD ever sends, which the worker now VERIFIES.
        if (wants_bda_capture_replay || wants_bda_multi_device) {
            std::fprintf(stderr, "vkrelay2-icd: rejecting device -- bufferDeviceAddress"
                                 "CaptureReplay/MultiDevice requested but reported FALSE "
                                 "(unwired)\n");
            return VK_ERROR_FEATURE_NOT_PRESENT;
        }
        if (wants_bda_feature && !native_lane()) {
            std::fprintf(stderr, "vkrelay2-icd: rejecting device -- bufferDeviceAddress requested "
                                 "but reported FALSE off the native lane\n");
            return VK_ERROR_FEATURE_NOT_PRESENT;
        }
        // descriptorIndexing: collect EVERY DI-family enable (the standalone
        // VkPhysicalDeviceDescriptorIndexingFeatures spelling or the 1.2 rollup, direct or
        // Features2-wrapped -- including the rollup's AGGREGATE `descriptorIndexing` bit).
        // SERVED members (the buffer-only kDIFeatureServedBits subset) fold into the
        // wire scalar; ANY unserved member -- the aggregate, the image/texel UAB classes, their
        // shader bits, the dynamic-indexing bits -- is reported FALSE, so enabling it is
        // FEATURE_NOT_PRESENT (both spellings). Off the native lane every DI
        // member is reported FALSE, so any enable rejects there too.
        const auto fold_di_wants = [&](const VkBool32* f20) {
            // The 20 members of VkPhysicalDeviceDescriptorIndexingFeatures in declaration order
            // (the 1.2 rollup repeats them verbatim after its aggregate bit). Served indices:
            // 3 = shaderUniformBufferArrayNonUniformIndexing, 5 = shaderStorageBufferArrayNon-
            // UniformIndexing, 10 = UB UpdateAfterBind, 13 = SB UpdateAfterBind, 16 = Update-
            // UnusedWhilePending, 17 = PartiallyBound, 18 = VariableDescriptorCount, 19 =
            // runtimeDescriptorArray. Everything else trips the unserved reject.
            static const struct {
                std::size_t index;
                std::uint64_t bit;
            } served[] = {
                {3, vkrpc::kDIFeatureShaderUniformBufferArrayNonUniformIndexing},
                {5, vkrpc::kDIFeatureShaderStorageBufferArrayNonUniformIndexing},
                {10, vkrpc::kDIFeatureUpdateAfterBindUniformBuffer},
                {13, vkrpc::kDIFeatureUpdateAfterBindStorageBuffer},
                {16, vkrpc::kDIFeatureUpdateUnusedWhilePending},
                {17, vkrpc::kDIFeaturePartiallyBound},
                {18, vkrpc::kDIFeatureVariableDescriptorCount},
                {19, vkrpc::kDIFeatureRuntimeDescriptorArray},
            };
            for (std::size_t i = 0; i < 20; ++i) {
                if (f20[i] == VK_FALSE) {
                    continue;
                }
                std::uint64_t bit = 0;
                for (const auto& s : served) {
                    if (s.index == i) {
                        bit = s.bit;
                    }
                }
                if (bit != 0) {
                    wants_di_bits |= bit;
                } else {
                    wants_unserved_di = true;
                }
            }
        };
        const auto collect_di = [&](const VkBaseInStructure* n) {
            if (n->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES) {
                const auto* f =
                    reinterpret_cast<const VkPhysicalDeviceDescriptorIndexingFeatures*>(n);
                fold_di_wants(&f->shaderInputAttachmentArrayDynamicIndexing);
            } else if (n->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES) {
                const auto* f = reinterpret_cast<const VkPhysicalDeviceVulkan12Features*>(n);
                wants_unserved_di = wants_unserved_di || f->descriptorIndexing != VK_FALSE;
                fold_di_wants(&f->shaderInputAttachmentArrayDynamicIndexing);
            }
        };
        for (auto* node = static_cast<const VkBaseInStructure*>(pCreateInfo->pNext);
             node != nullptr; node = node->pNext) {
            collect_di(node);
            if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2) {
                for (auto* n = static_cast<const VkBaseInStructure*>(
                         reinterpret_cast<const VkPhysicalDeviceFeatures2*>(node)->pNext);
                     n != nullptr; n = n->pNext) {
                    collect_di(n);
                }
            }
        }
        if (wants_unserved_di) {
            std::fprintf(stderr,
                         "vkrelay2-icd: rejecting device -- an UNSERVED descriptorIndexing member "
                         "was requested (the aggregate bit, an image/texel class, or a "
                         "dynamic-indexing bit -- all reported FALSE until their own proof)\n");
            return VK_ERROR_FEATURE_NOT_PRESENT;
        }
        if (wants_di_bits != 0 && !native_lane()) {
            std::fprintf(stderr, "vkrelay2-icd: rejecting device -- descriptorIndexing "
                                 "sub-features requested but reported FALSE off the native lane\n");
            return VK_ERROR_FEATURE_NOT_PRESENT;
        }
        // Vulkan 1.3 support: collect every vk13-family enable -- the f13 rollup (minus
        // its dynamicRendering/synchronization2 members, which the dedicated walks above already
        // detect), the 1.2 rollup's three memory-model members, and every standalone promoted
        // struct spelling. SERVED members fold into the wire scalar; the two UNSERVED members
        // (descriptorBindingInlineUniformBlockUpdateAfterBind, textureCompressionASTC_HDR) are
        // reported FALSE everywhere, so enabling them is FEATURE_NOT_PRESENT. The memory-model
        // bits are reported as the host's value on BOTH lanes (their long-standing pass-through),
        // so they are accepted anywhere; every other served bit needs the honest 1.3 device.
        const auto collect_vk13 = [&](const VkBaseInStructure* n) {
            const auto fold = [&](VkBool32 v, std::uint64_t bit) {
                if (v != VK_FALSE) {
                    wants_vk13_bits |= bit;
                }
            };
            switch (static_cast<int>(n->sType)) {
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES: {
                const auto* f = reinterpret_cast<const VkPhysicalDeviceVulkan13Features*>(n);
                fold(f->robustImageAccess, vkrpc::kVk13FeatureRobustImageAccess);
                fold(f->inlineUniformBlock, vkrpc::kVk13FeatureInlineUniformBlock);
                wants_unserved_vk13 =
                    wants_unserved_vk13 ||
                    f->descriptorBindingInlineUniformBlockUpdateAfterBind != VK_FALSE ||
                    f->textureCompressionASTC_HDR != VK_FALSE;
                fold(f->pipelineCreationCacheControl,
                     vkrpc::kVk13FeaturePipelineCreationCacheControl);
                fold(f->privateData, vkrpc::kVk13FeaturePrivateData);
                fold(f->shaderDemoteToHelperInvocation,
                     vkrpc::kVk13FeatureShaderDemoteToHelperInvocation);
                fold(f->shaderTerminateInvocation, vkrpc::kVk13FeatureShaderTerminateInvocation);
                fold(f->subgroupSizeControl, vkrpc::kVk13FeatureSubgroupSizeControl);
                fold(f->computeFullSubgroups, vkrpc::kVk13FeatureComputeFullSubgroups);
                fold(f->shaderZeroInitializeWorkgroupMemory,
                     vkrpc::kVk13FeatureShaderZeroInitializeWorkgroupMemory);
                fold(f->shaderIntegerDotProduct, vkrpc::kVk13FeatureShaderIntegerDotProduct);
                fold(f->maintenance4, vkrpc::kVk13FeatureMaintenance4);
                break;
            }
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES: {
                const auto* f = reinterpret_cast<const VkPhysicalDeviceVulkan12Features*>(n);
                fold(f->vulkanMemoryModel, vkrpc::kVk13FeatureVulkanMemoryModel);
                fold(f->vulkanMemoryModelDeviceScope,
                     vkrpc::kVk13FeatureVulkanMemoryModelDeviceScope);
                fold(f->vulkanMemoryModelAvailabilityVisibilityChains,
                     vkrpc::kVk13FeatureVulkanMemoryModelAvailabilityVisibilityChains);
                break;
            }
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES: {
                const auto* f =
                    reinterpret_cast<const VkPhysicalDeviceVulkanMemoryModelFeatures*>(n);
                fold(f->vulkanMemoryModel, vkrpc::kVk13FeatureVulkanMemoryModel);
                fold(f->vulkanMemoryModelDeviceScope,
                     vkrpc::kVk13FeatureVulkanMemoryModelDeviceScope);
                fold(f->vulkanMemoryModelAvailabilityVisibilityChains,
                     vkrpc::kVk13FeatureVulkanMemoryModelAvailabilityVisibilityChains);
                break;
            }
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_ROBUSTNESS_FEATURES:
                fold(reinterpret_cast<const VkPhysicalDeviceImageRobustnessFeatures*>(n)
                         ->robustImageAccess,
                     vkrpc::kVk13FeatureRobustImageAccess);
                break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INLINE_UNIFORM_BLOCK_FEATURES: {
                const auto* f =
                    reinterpret_cast<const VkPhysicalDeviceInlineUniformBlockFeatures*>(n);
                fold(f->inlineUniformBlock, vkrpc::kVk13FeatureInlineUniformBlock);
                wants_unserved_vk13 =
                    wants_unserved_vk13 ||
                    f->descriptorBindingInlineUniformBlockUpdateAfterBind != VK_FALSE;
                break;
            }
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_CREATION_CACHE_CONTROL_FEATURES:
                fold(
                    reinterpret_cast<const VkPhysicalDevicePipelineCreationCacheControlFeatures*>(n)
                        ->pipelineCreationCacheControl,
                    vkrpc::kVk13FeaturePipelineCreationCacheControl);
                break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIVATE_DATA_FEATURES:
                fold(reinterpret_cast<const VkPhysicalDevicePrivateDataFeatures*>(n)->privateData,
                     vkrpc::kVk13FeaturePrivateData);
                break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DEMOTE_TO_HELPER_INVOCATION_FEATURES:
                fold(
                    reinterpret_cast<const VkPhysicalDeviceShaderDemoteToHelperInvocationFeatures*>(
                        n)
                        ->shaderDemoteToHelperInvocation,
                    vkrpc::kVk13FeatureShaderDemoteToHelperInvocation);
                break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_TERMINATE_INVOCATION_FEATURES:
                fold(reinterpret_cast<const VkPhysicalDeviceShaderTerminateInvocationFeatures*>(n)
                         ->shaderTerminateInvocation,
                     vkrpc::kVk13FeatureShaderTerminateInvocation);
                break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES: {
                const auto* f =
                    reinterpret_cast<const VkPhysicalDeviceSubgroupSizeControlFeatures*>(n);
                fold(f->subgroupSizeControl, vkrpc::kVk13FeatureSubgroupSizeControl);
                fold(f->computeFullSubgroups, vkrpc::kVk13FeatureComputeFullSubgroups);
                break;
            }
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ZERO_INITIALIZE_WORKGROUP_MEMORY_FEATURES:
                fold(reinterpret_cast<const VkPhysicalDeviceZeroInitializeWorkgroupMemoryFeatures*>(
                         n)
                         ->shaderZeroInitializeWorkgroupMemory,
                     vkrpc::kVk13FeatureShaderZeroInitializeWorkgroupMemory);
                break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_INTEGER_DOT_PRODUCT_FEATURES:
                fold(reinterpret_cast<const VkPhysicalDeviceShaderIntegerDotProductFeatures*>(n)
                         ->shaderIntegerDotProduct,
                     vkrpc::kVk13FeatureShaderIntegerDotProduct);
                break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_FEATURES:
                fold(reinterpret_cast<const VkPhysicalDeviceMaintenance4Features*>(n)->maintenance4,
                     vkrpc::kVk13FeatureMaintenance4);
                break;
            default:
                break;
            }
        };
        for (auto* node = static_cast<const VkBaseInStructure*>(pCreateInfo->pNext);
             node != nullptr; node = node->pNext) {
            collect_vk13(node);
            if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2) {
                for (auto* n = static_cast<const VkBaseInStructure*>(
                         reinterpret_cast<const VkPhysicalDeviceFeatures2*>(node)->pNext);
                     n != nullptr; n = n->pNext) {
                    collect_vk13(n);
                }
            }
        }
        if (wants_unserved_vk13) {
            std::fprintf(stderr,
                         "vkrelay2-icd: rejecting device -- an UNSERVED 1.3-family member was "
                         "requested (descriptorBindingInlineUniformBlockUpdateAfterBind / "
                         "textureCompressionASTC_HDR -- reported FALSE until their own proof)\n");
            return VK_ERROR_FEATURE_NOT_PRESENT;
        }
        constexpr std::uint64_t kMemoryModelBits =
            vkrpc::kVk13FeatureVulkanMemoryModel | vkrpc::kVk13FeatureVulkanMemoryModelDeviceScope |
            vkrpc::kVk13FeatureVulkanMemoryModelAvailabilityVisibilityChains;
        if ((wants_vk13_bits & ~kMemoryModelBits) != 0 && !vk13_device_ok(pd)) {
            std::fprintf(stderr, "vkrelay2-icd: rejecting device -- 1.3-family features requested "
                                 "but this device is reported 1.2 (they are FALSE off the honest "
                                 "1.3 native-lane device)\n");
            return VK_ERROR_FEATURE_NOT_PRESENT;
        }
    }
    try {
        vkrpc::CreateDeviceRequest req;
        req.instance = pd->instance_worker;
        req.physical_device = pd->worker;
        req.enabled_extensions = std::move(enabled_exts);
        req.enabled_feature_bits = enabled_feature_bits;
        req.enabled_feature_bits_authoritative = true;
        req.dynamic_rendering_feature_enabled = wants_dynamic_rendering_feature ? 1 : 0;
        req.synchronization2_feature_enabled = wants_synchronization2_feature ? 1 : 0;
        req.host_query_reset_feature_enabled = wants_host_query_reset_feature ? 1 : 0;
        req.multiview_feature_enabled = wants_multiview_feature ? 1 : 0;
        req.buffer_device_address_feature_enabled = wants_bda_feature ? 1 : 0;
        // vertex-attr-divisor: the two ENABLED feature bits (the worker re-derives + checks
        // agreement against the forwarded chain).
        req.vertex_attr_divisor_feature_enabled = wants_vertex_attr_divisor_feature ? 1 : 0;
        req.vertex_attr_zero_divisor_feature_enabled =
            wants_vertex_attr_zero_divisor_feature ? 1 : 0;
        // geometry-stream: the ENABLED geometryStreams feature rides the scalar; the worker
        // re-derives it from the forwarded chain and rejects a mismatch (divisor pattern).
        req.geometry_streams_feature_enabled = wants_geometry_streams_feature ? 1 : 0;
        req.draw_indirect_count_enabled =
            vkr::icd_policy::indirect_count_device_enabled(wants_draw_indirect_count,
                                                           wants_draw_indirect_count_feature)
                ? 1
                : 0;
        // the served DI bits ride the scalar; the worker re-derives them from the
        // forwarded chain and rejects a mismatch (normalization, same as BDA's). Off-lane /
        // unserved enables were already rejected above with FEATURE_NOT_PRESENT.
        req.descriptor_indexing_feature_bits = wants_di_bits;
        // Vulkan 1.3 support: the served vk13 bits + whether this device was REPORTED as 1.3 (the
        // worker re-verifies the host before creating; a lying scalar/flag dies there).
        req.vk13_feature_bits = wants_vk13_bits;
        req.vk13_device_enabled = vk13_device_ok(pd) ? 1 : 0;
        req.enabled_feature_chain = std::move(enabled_feature_chain);
        icd_trace("CreateDevice enabled_ext_count=%zu feature_bits=0x%llx feature_chain=%zu",
                  req.enabled_extensions.size(),
                  static_cast<unsigned long long>(req.enabled_feature_bits),
                  req.enabled_feature_chain.size());
        const vkrpc::CreateDeviceResponse r = vkrpc::create_device(*g_rpc, next_id(), req);
        icd_trace("CreateDevice result ok=%d reason='%s'", r.ok ? 1 : 0, r.reason.c_str());
        if (!r.ok) {
            return fault();
        }
        auto* dev = new DeviceImpl{};
        set_loader_magic_value(dev);
        dev->worker = r.device;
        dev->queue_family = r.queue_family_index; // the worker's real graphics family
        dev->mem_props = pd->mem_props;           // cached host table (for allocate's type lookup)
        dev->max_color_attachments = pd->caps.max_color_attachments; // MRT gate (0 = old worker)
        dev->queue_flags = pd->caps.queue_flags; // compute gate (0 = old worker: no compute)
        // the DR pipeline-pNext admission is allowed ONLY on
        // the native lane AND when the app enabled BOTH VK_KHR_dynamic_rendering (validated against
        // the advertised list above) AND the dynamicRendering feature -- Vulkan forbids using
        // dynamic rendering with the ext but not the feature, so the ICD refuses to admit it
        // either.
        // Vulkan 1.3 support: on an honest 1.3 device, dynamic rendering + synchronization2 are
        // CORE -- the FEATURE alone admits them (no KHR extension to require). Vulkan-1.2 devices
        // keep the ext-AND-feature rule unchanged.
        dev->dynamic_rendering_enabled =
            (native_lane() && wants_dynamic_rendering && wants_dynamic_rendering_feature) ||
            (vk13_device_ok(pd) && wants_dynamic_rendering_feature);
        dev->synchronization2_enabled =
            (wants_synchronization2 && wants_synchronization2_feature) ||
            (vk13_device_ok(pd) && wants_synchronization2_feature);
        // Required-feature audit (hardening): hostQueryReset is core 1.2
        // and served on BOTH lanes, so the gate is the enabled FEATURE alone (no lane/ext
        // predicate). The device-level ResetQueryPool entrypoint no-ops before forwarding unless
        // this is set.
        dev->host_query_reset_enabled = wants_host_query_reset_feature;
        // Required-feature audit: multiview is core 1.1, reported host-TRUE and served
        // on BOTH lanes, so the gate is the enabled FEATURE alone (no lane/ext predicate).
        // CreateRenderPass2 carries a subpass viewMask only when this is set (else it rejects the
        // pass fail-closed).
        dev->multiview_enabled = wants_multiview_feature;
        // vertex-attr-divisor: the ENABLED extension gates the divisor-pNext admission at
        // CreateGraphicsPipelines; the two feature bits gate the divisor VALUES (shared validator).
        dev->vertex_attr_divisor_enabled = wants_vertex_attr_divisor;
        dev->vertex_attr_divisor_feature_enabled = wants_vertex_attr_divisor_feature;
        dev->vertex_attr_zero_divisor_feature_enabled = wants_vertex_attr_zero_divisor_feature;
        // geometry-stream: the ENABLED extension + geometryStreams feature gate the
        // rasterization-stream pNext admission at CreateGraphicsPipelines (graphics_pipeline_ok's
        // allow_rasterization_stream) and the shared value validator. The WORKER
        // capability is the third leg -- an old worker (caps bit absent) would silently DROP the
        // stream state its decoder does not know, so against one the ICD keeps its legacy
        // fail-closed rejection of the stream pNext (the same skew discipline as the MRT/compute
        // gates above). geometryStreams itself stays advertised + forwarded either way (masking it
        // breaks Mesa 23.2 zink shaders -- see the features2 pass-through note).
        dev->transform_feedback_enabled = wants_transform_feedback;
        dev->geometry_streams_feature_enabled = wants_geometry_streams_feature;
        dev->worker_rasterization_stream = pd->caps.rasterization_stream_state != 0;
        dev->worker_core_indirect_draw = pd->caps.core_indirect_draw != 0;
        dev->worker_core_indirect_draw_scalar_payload =
            pd->caps.core_indirect_draw_scalar_payload != 0;
        dev->multi_draw_indirect_enabled =
            (enabled_feature_bits & vkrpc::kFeatureMultiDrawIndirect) != 0;
        dev->worker_core_indirect_draw_count = pd->caps.core_indirect_draw_count != 0;
        dev->draw_indirect_count_enabled = vkr::icd_policy::indirect_count_device_enabled(
            wants_draw_indirect_count, wants_draw_indirect_count_feature);
        // (bufferDeviceAddress): native lane AND the enabled FEATURE (there is no
        // extension to require -- core 1.2). An off-lane enable was already rejected above with
        // FEATURE_NOT_PRESENT, so this equals wants_bda_feature; the && stays as defense in depth.
        dev->buffer_device_address_enabled = native_lane() && wants_bda_feature;
        //   (descriptorIndexing): the served bits this device ENABLED -- the ICD's own
        // layout/pool/allocate admission keys on them (the shared validator). An off-lane or
        // unserved enable was rejected above, so on any surviving device these are served +
        // native-lane by construction.
        dev->descriptor_indexing_feature_bits = wants_di_bits;
        // Vulkan 1.3 support: the vk13 admission state -- the core-1.3 command surface (EDS core
        // names, copy_commands2, featureless DR/sync2) keys on vk13_device; the per-feature usage
        // surfaces (cache-control pipeline flags, subgroup-size stage shapes, inline-uniform
        // writes, private data, maintenance4 queries) key on their enabled bits.
        dev->vk13_feature_bits = wants_vk13_bits;
        dev->vk13_device = vk13_device_ok(pd);
        dev->enabled_extensions.insert(req.enabled_extensions.begin(),
                                       req.enabled_extensions.end());
        *pDevice = reinterpret_cast<VkDevice>(dev);
        return VK_SUCCESS;
    } catch (const std::exception&) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
}

VKAPI_ATTR void VKAPI_CALL DestroyDevice(VkDevice device, const VkAllocationCallbacks*) {
    if (device == VK_NULL_HANDLE) {
        return;
    }
    std::lock_guard<std::mutex> lk(g_mu);
    auto* dev = reinterpret_cast<DeviceImpl*>(device);
    try {
        vkrpc::HandleRequest req;
        req.handle = dev->worker;
        (void) vkrpc::destroy_device(*g_rpc, next_id(), req);
    } catch (const std::exception&) {
    }
    delete dev;
}

VKAPI_ATTR void VKAPI_CALL GetDeviceQueue(VkDevice device, std::uint32_t /*queueFamilyIndex*/,
                                          std::uint32_t queueIndex, VkQueue* pQueue) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto* dev = reinterpret_cast<DeviceImpl*>(device);
    *pQueue = VK_NULL_HANDLE;
    try {
        // The app uses the single synthesized family (0); map it to the worker's real family.
        vkrpc::GetDeviceQueueRequest req;
        req.device = dev->worker;
        req.queue_family_index = dev->queue_family;
        req.queue_index = static_cast<int>(queueIndex);
        const vkrpc::GetDeviceQueueResponse r = vkrpc::get_device_queue(*g_rpc, next_id(), req);
        if (!r.ok) {
            return;
        }
        auto* q = new QueueImpl{};
        set_loader_magic_value(q);
        q->worker = r.queue;
        *pQueue = reinterpret_cast<VkQueue>(q);
    } catch (const std::exception&) {
    }
}

VKAPI_ATTR VkResult VKAPI_CALL CreateSwapchainKHR(VkDevice device,
                                                  const VkSwapchainCreateInfoKHR* pCreateInfo,
                                                  const VkAllocationCallbacks*,
                                                  VkSwapchainKHR* pSwapchain) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto* dev = reinterpret_cast<DeviceImpl*>(device);
    try {
        vkrpc::CreateSwapchainRequest req;
        req.device = dev->worker;
        req.surface = from_handle(pCreateInfo->surface);
        req.image_format = static_cast<int>(pCreateInfo->imageFormat);
        req.color_space = static_cast<int>(pCreateInfo->imageColorSpace);
        req.width = static_cast<int>(pCreateInfo->imageExtent.width);
        req.height = static_cast<int>(pCreateInfo->imageExtent.height);
        req.min_image_count = static_cast<int>(pCreateInfo->minImageCount);
        req.present_mode = static_cast<int>(pCreateInfo->presentMode);
        req.image_usage = static_cast<int>(pCreateInfo->imageUsage);
        // (GL/zink): zink echoes the 0xFFFFFFFF "currentExtent" sentinel back as the image
        // extent instead of choosing one. Flag it so the worker uses the host surface's real
        // extent.
        req.use_current_extent = pCreateInfo->imageExtent.width == 0xFFFFFFFFu ||
                                 pCreateInfo->imageExtent.height == 0xFFFFFFFFu;
        req.old_swapchain = from_handle(pCreateInfo->oldSwapchain);
        const vkrpc::CreateSwapchainResponse r = vkrpc::create_swapchain(*g_rpc, next_id(), req);
        if (!r.ok) {
            icd_trace("CreateSwapchainKHR FAILED: %s", r.reason.c_str());
            caps_cache().invalidate_surface(req.surface); // create failed for this surface
            return fault();
        }
        // A ran-but-out-of-date surface (the worker could not converge the HWND to the requested
        // extent) is ok=true + a non-success result -- return it verbatim so the app re-queries the
        // surface caps + retries (normal WSI), rather than masking it as a relay fault. No
        // swapchain handle in that case.
        if (r.result != vkrpc::kVkSuccess) {
            caps_cache().invalidate_surface(req.surface); // the retry poll must re-query
            return static_cast<VkResult>(r.result);
        }
        // the surface is now live (cacheable) -- and the create itself invalidates, so
        // the recreate flow's first poll re-queries.
        caps_cache().on_swapchain_created(r.swapchain, req.surface);
        *pSwapchain = to_handle<VkSwapchainKHR>(r.swapchain);
        return VK_SUCCESS;
    } catch (const std::exception&) {
        caps_cache().invalidate_surface(from_handle(pCreateInfo->surface)); // fail closed
        return VK_ERROR_INITIALIZATION_FAILED;
    }
}

VKAPI_ATTR void VKAPI_CALL DestroySwapchainKHR(VkDevice, VkSwapchainKHR swapchain,
                                               const VkAllocationCallbacks*) {
    if (swapchain == VK_NULL_HANDLE) {
        return;
    }
    std::lock_guard<std::mutex> lk(g_mu);
    // void API -- drop local state + invalidate unconditionally, before the RPC, so even
    // a destroy that faults can never leave a cache serving a destroyed swapchain's surface.
    caps_cache().on_swapchain_destroyed(from_handle(swapchain));
    try {
        vkrpc::HandleRequest req;
        req.handle = from_handle(swapchain);
        (void) vkrpc::destroy_swapchain(*g_rpc, next_id(), req);
    } catch (const std::exception&) {
    }
}

VKAPI_ATTR VkResult VKAPI_CALL GetSwapchainImagesKHR(VkDevice, VkSwapchainKHR swapchain,
                                                     std::uint32_t* pCount, VkImage* pImages) {
    std::lock_guard<std::mutex> lk(g_mu);
    try {
        vkrpc::GetSwapchainImagesRequest req;
        req.swapchain = from_handle(swapchain);
        const vkrpc::GetSwapchainImagesResponse r =
            vkrpc::get_swapchain_images(*g_rpc, next_id(), req);
        if (!r.ok) {
            return fault();
        }
        const auto count = static_cast<std::uint32_t>(r.images.size());
        if (pImages == nullptr) {
            *pCount = count;
            return VK_SUCCESS;
        }
        const std::uint32_t n = *pCount < count ? *pCount : count;
        for (std::uint32_t i = 0; i < n; ++i) {
            pImages[i] = to_handle<VkImage>(r.images[i]);
        }
        *pCount = n;
        return n < count ? VK_INCOMPLETE : VK_SUCCESS;
    } catch (const std::exception&) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
}

// --- Draw surface ----------------------------------------
// Each create translates the app's VkCreateInfo into the bounded RPC request (the fields the
// subset carries) and forwards it; the worker enforces the strict-but-total allowlist and mints the
// handle, which rides back as an opaque u64 -> non-dispatchable Vk handle. Destroys forward the
// handle. The draw vkCmd* accumulate into the command buffer's recording (flushed at
// vkEndCommandBuffer), like the barrier/clear commands.

VKAPI_ATTR VkResult VKAPI_CALL CreateImageView(VkDevice /*device*/,
                                               const VkImageViewCreateInfo* pCreateInfo,
                                               const VkAllocationCallbacks*, VkImageView* pView) {
    std::lock_guard<std::mutex> lk(g_mu);
    try {
        const char* why = "";
        std::uint64_t view_usage = 0;
        if (!vkr::icd_subset::image_view_ok(pCreateInfo, &view_usage, &why)) {
            std::fprintf(stderr, "vkrelay2-icd: rejecting image view -- %s\n", why);
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        vkrpc::CreateImageViewRequest req;
        req.view_usage = view_usage; // the admitted usage-narrowing pNext (0 = absent)
        req.image = from_handle(pCreateInfo->image);
        req.view_type = static_cast<int>(pCreateInfo->viewType);
        req.format = static_cast<int>(pCreateInfo->format);
        req.swizzle_r = static_cast<int>(pCreateInfo->components.r);
        req.swizzle_g = static_cast<int>(pCreateInfo->components.g);
        req.swizzle_b = static_cast<int>(pCreateInfo->components.b);
        req.swizzle_a = static_cast<int>(pCreateInfo->components.a);
        req.aspect = static_cast<int>(pCreateInfo->subresourceRange.aspectMask);
        req.base_mip_level = static_cast<int>(pCreateInfo->subresourceRange.baseMipLevel);
        req.level_count = static_cast<int>(pCreateInfo->subresourceRange.levelCount);
        req.base_array_layer = static_cast<int>(pCreateInfo->subresourceRange.baseArrayLayer);
        req.layer_count = static_cast<int>(pCreateInfo->subresourceRange.layerCount);
        const vkrpc::CreateImageViewResponse r = vkrpc::create_image_view(*g_rpc, next_id(), req);
        if (!r.ok) {
            return fault();
        }
        *pView = to_handle<VkImageView>(r.image_view);
        return VK_SUCCESS;
    } catch (const std::exception&) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
}

VKAPI_ATTR void VKAPI_CALL DestroyImageView(VkDevice, VkImageView imageView,
                                            const VkAllocationCallbacks*) {
    if (imageView == VK_NULL_HANDLE) {
        return;
    }
    std::lock_guard<std::mutex> lk(g_mu);
    try {
        vkrpc::HandleRequest req;
        req.handle = from_handle(imageView);
        (void) vkrpc::destroy_image_view(*g_rpc, next_id(), req);
    } catch (const std::exception&) {
    }
}

// (GL/zink): a texel buffer view. Faithfully marshalled (buffer/format/offset/range) to the
// worker -- no vkcube-shaped subset check (zink is a trusted producer).
VKAPI_ATTR VkResult VKAPI_CALL CreateBufferView(VkDevice /*device*/,
                                                const VkBufferViewCreateInfo* pCreateInfo,
                                                const VkAllocationCallbacks*, VkBufferView* pView) {
    std::lock_guard<std::mutex> lk(g_mu);
    try {
        if (pCreateInfo == nullptr || pView == nullptr) {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        vkrpc::CreateBufferViewRequest req;
        req.buffer = from_handle(pCreateInfo->buffer);
        req.format = static_cast<int>(pCreateInfo->format);
        req.offset = static_cast<std::uint64_t>(pCreateInfo->offset);
        req.range = static_cast<std::uint64_t>(pCreateInfo->range);
        const vkrpc::CreateBufferViewResponse r = vkrpc::create_buffer_view(*g_rpc, next_id(), req);
        if (!r.ok) {
            return fault();
        }
        *pView = to_handle<VkBufferView>(r.buffer_view);
        return VK_SUCCESS;
    } catch (const std::exception&) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
}

VKAPI_ATTR void VKAPI_CALL DestroyBufferView(VkDevice, VkBufferView bufferView,
                                             const VkAllocationCallbacks*) {
    if (bufferView == VK_NULL_HANDLE) {
        return;
    }
    std::lock_guard<std::mutex> lk(g_mu);
    try {
        vkrpc::HandleRequest req;
        req.handle = from_handle(bufferView);
        (void) vkrpc::destroy_buffer_view(*g_rpc, next_id(), req);
    } catch (const std::exception&) {
    }
}

VKAPI_ATTR VkResult VKAPI_CALL CreateShaderModule(VkDevice device,
                                                  const VkShaderModuleCreateInfo* pCreateInfo,
                                                  const VkAllocationCallbacks*,
                                                  VkShaderModule* pShaderModule) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto* dev = reinterpret_cast<DeviceImpl*>(device);
    try {
        const char* why = "";
        if (!vkr::icd_subset::shader_module_ok(pCreateInfo, &why)) {
            std::fprintf(stderr, "vkrelay2-icd: rejecting shader module -- %s\n", why);
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        vkrpc::CreateShaderModuleRequest req;
        req.device = dev->worker;
        req.code_size = static_cast<std::uint64_t>(pCreateInfo->codeSize);
        req.code.assign(reinterpret_cast<const char*>(pCreateInfo->pCode), pCreateInfo->codeSize);
        const vkrpc::CreateShaderModuleResponse r =
            vkrpc::create_shader_module(*g_rpc, next_id(), req);
        if (!r.ok) {
            return fault();
        }
        *pShaderModule = to_handle<VkShaderModule>(r.shader_module);
        return VK_SUCCESS;
    } catch (const std::exception&) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
}

VKAPI_ATTR void VKAPI_CALL DestroyShaderModule(VkDevice, VkShaderModule shaderModule,
                                               const VkAllocationCallbacks*) {
    if (shaderModule == VK_NULL_HANDLE) {
        return;
    }
    std::lock_guard<std::mutex> lk(g_mu);
    try {
        vkrpc::HandleRequest req;
        req.handle = from_handle(shaderModule);
        (void) vkrpc::destroy_shader_module(*g_rpc, next_id(), req);
    } catch (const std::exception&) {
    }
}

VKAPI_ATTR VkResult VKAPI_CALL CreateRenderPass(VkDevice device,
                                                const VkRenderPassCreateInfo* pCreateInfo,
                                                const VkAllocationCallbacks*,
                                                VkRenderPass* pRenderPass) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto* dev = reinterpret_cast<DeviceImpl*>(device);
    try {
        const char* why = "";
        if (!vkr::icd_subset::render_pass_ok(pCreateInfo, dev->max_color_attachments, &why)) {
            std::fprintf(stderr, "vkrelay2-icd: rejecting render pass -- %s\n", why);
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        vkrpc::CreateRenderPassRequest req;
        req.device = dev->worker;
        for (std::uint32_t i = 0; i < pCreateInfo->attachmentCount; ++i) {
            const VkAttachmentDescription& a = pCreateInfo->pAttachments[i];
            vkrpc::AttachmentDesc d;
            d.format = static_cast<int>(a.format);
            d.samples = static_cast<int>(a.samples);
            d.load_op = static_cast<int>(a.loadOp);
            d.store_op = static_cast<int>(a.storeOp);
            d.stencil_load_op = static_cast<int>(a.stencilLoadOp);
            d.stencil_store_op = static_cast<int>(a.stencilStoreOp);
            d.initial_layout = normalize_unadvertised_feedback_layout(a.initialLayout);
            d.final_layout = normalize_unadvertised_feedback_layout(a.finalLayout);
            req.attachments.push_back(d);
        }
        // MRT: carry the subpass's FULL ordered color-ref array (UNUSED holes as -1, wide -- never
        // an int cast of 0xFFFFFFFF). The legacy scalars stay populated from ref [0] so an old
        // worker (which ignores color_refs) still builds a correct SINGLE-color pass; the checker
        // above already refused >1 ref when the worker never advertised MRT.
        const VkSubpassDescription& sp = pCreateInfo->pSubpasses[0];
        for (std::uint32_t i = 0; i < sp.colorAttachmentCount; ++i) {
            vkrpc::ColorRefDesc cr;
            cr.attachment = sp.pColorAttachments[i].attachment == VK_ATTACHMENT_UNUSED
                                ? vkrpc::kColorRefUnused
                                : static_cast<long long>(sp.pColorAttachments[i].attachment);
            cr.layout = normalize_unadvertised_feedback_layout(sp.pColorAttachments[i].layout);
            req.color_refs.push_back(cr);
        }
        if (!req.color_refs.empty() && vkrpc::color_ref_used(req.color_refs[0])) {
            req.color_attachment = static_cast<int>(req.color_refs[0].attachment);
            req.color_layout = req.color_refs[0].layout;
        }
        // Depth-stencil attachment reference (-1 = none).
        if (sp.pDepthStencilAttachment != nullptr) {
            req.depth_attachment = static_cast<int>(sp.pDepthStencilAttachment->attachment);
            req.depth_layout =
                normalize_unadvertised_feedback_layout(sp.pDepthStencilAttachment->layout);
        }
        for (std::uint32_t i = 0; i < pCreateInfo->dependencyCount; ++i) {
            const VkSubpassDependency& dep = pCreateInfo->pDependencies[i];
            vkrpc::SubpassDependencyDesc dd;
            dd.src_subpass =
                static_cast<long long>(dep.srcSubpass); // VK_SUBPASS_EXTERNAL stays wide
            dd.dst_subpass = static_cast<long long>(dep.dstSubpass);
            dd.src_stage = static_cast<long long>(dep.srcStageMask);
            dd.dst_stage = static_cast<long long>(dep.dstStageMask);
            dd.src_access = static_cast<long long>(dep.srcAccessMask);
            dd.dst_access = static_cast<long long>(dep.dstAccessMask);
            dd.dependency_flags = static_cast<long long>(dep.dependencyFlags);
            req.dependencies.push_back(dd);
        }
        const vkrpc::CreateRenderPassResponse r = vkrpc::create_render_pass(*g_rpc, next_id(), req);
        if (!r.ok) {
            std::fprintf(stderr, "vkrelay2-icd: create_render_pass rejected by worker -- %s\n",
                         r.reason.c_str());
            return fault();
        }
        *pRenderPass = to_handle<VkRenderPass>(r.render_pass);
        return VK_SUCCESS;
    } catch (const std::exception&) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
}

// (GL/zink): vkCreateRenderPass2 (core 1.2 / VK_KHR_create_renderpass2). zink uses the *2
// path on a 1.2 device. Convert the VkRenderPassCreateInfo2 to the same create_render_pass request
// (single subpass: first colour ref + optional depth ref; attachments + dependencies verbatim).
// Required-feature audit: the subpass viewMask IS now carried (> a real host multiview
// render pass) on a device that enabled the multiview feature; the ref aspectMask + sync2-in-deps
// stay uncarried (loud in render_pass2_ok).
VKAPI_ATTR VkResult VKAPI_CALL CreateRenderPass2(VkDevice device,
                                                 const VkRenderPassCreateInfo2* pCreateInfo,
                                                 const VkAllocationCallbacks*,
                                                 VkRenderPass* pRenderPass) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto* dev = reinterpret_cast<DeviceImpl*>(device);
    try {
        if (pCreateInfo == nullptr || pCreateInfo->subpassCount < 1 ||
            pCreateInfo->pSubpasses == nullptr) {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        // MRT (the empirically proven silent 2-color half-render): this path used to have NO
        // subset gate -- it forwarded pColorAttachments[0] and silently dropped every other color
        // ref (and would have silently dropped extra subpasses, viewMask, attachment flags, ...).
        // The v2 checker enforces the same rule as v1: widened fields carried, uncarried LOUD.
        const char* why = "";
        if (!vkr::icd_subset::render_pass2_ok(pCreateInfo, dev->max_color_attachments,
                                              dev->multiview_enabled, &why)) {
            std::fprintf(stderr, "vkrelay2-icd: rejecting render pass2 -- %s\n", why);
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        vkrpc::CreateRenderPassRequest req;
        req.device = dev->worker;
        // Required-feature audit: carry the single subpass's viewMask (0 =
        // non-multiview). render_pass2_ok already gated a non-zero mask on dev->multiview_enabled.
        req.view_mask = static_cast<int>(pCreateInfo->pSubpasses[0].viewMask);
        for (std::uint32_t i = 0; i < pCreateInfo->attachmentCount; ++i) {
            const VkAttachmentDescription2& a = pCreateInfo->pAttachments[i];
            vkrpc::AttachmentDesc d;
            d.format = static_cast<int>(a.format);
            d.samples = static_cast<int>(a.samples);
            d.load_op = static_cast<int>(a.loadOp);
            d.store_op = static_cast<int>(a.storeOp);
            d.stencil_load_op = static_cast<int>(a.stencilLoadOp);
            d.stencil_store_op = static_cast<int>(a.stencilStoreOp);
            d.initial_layout = normalize_unadvertised_feedback_layout(a.initialLayout);
            d.final_layout = normalize_unadvertised_feedback_layout(a.finalLayout);
            req.attachments.push_back(d);
        }
        const VkSubpassDescription2& sp = pCreateInfo->pSubpasses[0];
        // MRT: the full ordered ref array (UNUSED as -1 wide); scalars from ref [0] for an old
        // worker -- same contract as the v1 path above.
        for (std::uint32_t i = 0; i < sp.colorAttachmentCount; ++i) {
            vkrpc::ColorRefDesc cr;
            cr.attachment = sp.pColorAttachments[i].attachment == VK_ATTACHMENT_UNUSED
                                ? vkrpc::kColorRefUnused
                                : static_cast<long long>(sp.pColorAttachments[i].attachment);
            cr.layout = normalize_unadvertised_feedback_layout(sp.pColorAttachments[i].layout);
            req.color_refs.push_back(cr);
        }
        if (!req.color_refs.empty() && vkrpc::color_ref_used(req.color_refs[0])) {
            req.color_attachment = static_cast<int>(req.color_refs[0].attachment);
            req.color_layout = req.color_refs[0].layout;
        }
        if (sp.pDepthStencilAttachment != nullptr) {
            req.depth_attachment = static_cast<int>(sp.pDepthStencilAttachment->attachment);
            req.depth_layout =
                normalize_unadvertised_feedback_layout(sp.pDepthStencilAttachment->layout);
        }
        for (std::uint32_t i = 0; i < pCreateInfo->dependencyCount; ++i) {
            const VkSubpassDependency2& dep = pCreateInfo->pDependencies[i];
            vkrpc::SubpassDependencyDesc dd;
            dd.src_subpass = static_cast<long long>(dep.srcSubpass);
            dd.dst_subpass = static_cast<long long>(dep.dstSubpass);
            dd.src_stage = static_cast<long long>(dep.srcStageMask);
            dd.dst_stage = static_cast<long long>(dep.dstStageMask);
            dd.src_access = static_cast<long long>(dep.srcAccessMask);
            dd.dst_access = static_cast<long long>(dep.dstAccessMask);
            dd.dependency_flags = static_cast<long long>(dep.dependencyFlags);
            req.dependencies.push_back(dd);
        }
        const vkrpc::CreateRenderPassResponse r = vkrpc::create_render_pass(*g_rpc, next_id(), req);
        if (!r.ok) {
            icd_trace("CreateRenderPass2 rejected by worker -- %s", r.reason.c_str());
            return fault();
        }
        *pRenderPass = to_handle<VkRenderPass>(r.render_pass);
        return VK_SUCCESS;
    } catch (const std::exception&) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
}

VKAPI_ATTR void VKAPI_CALL DestroyRenderPass(VkDevice, VkRenderPass renderPass,
                                             const VkAllocationCallbacks*) {
    if (renderPass == VK_NULL_HANDLE) {
        return;
    }
    std::lock_guard<std::mutex> lk(g_mu);
    try {
        vkrpc::HandleRequest req;
        req.handle = from_handle(renderPass);
        (void) vkrpc::destroy_render_pass(*g_rpc, next_id(), req);
    } catch (const std::exception&) {
    }
}

VKAPI_ATTR VkResult VKAPI_CALL CreateFramebuffer(VkDevice device,
                                                 const VkFramebufferCreateInfo* pCreateInfo,
                                                 const VkAllocationCallbacks*,
                                                 VkFramebuffer* pFramebuffer) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto* dev = reinterpret_cast<DeviceImpl*>(device);
    try {
        const char* why = "";
        if (!vkr::icd_subset::framebuffer_ok(pCreateInfo, &why)) {
            std::fprintf(stderr, "vkrelay2-icd: rejecting framebuffer -- %s\n", why);
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        vkrpc::CreateFramebufferRequest req;
        req.device = dev->worker;
        req.render_pass = from_handle(pCreateInfo->renderPass);
        // (GL/zink): an IMAGELESS framebuffer has no concrete pAttachments -- the views
        // come at vkCmdBeginRenderPass. Carry imageless + the attachment count; the worker builds a
        // regular framebuffer at begin from the supplied views.
        req.imageless = (pCreateInfo->flags & VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT) != 0;
        req.attachment_count = static_cast<int>(pCreateInfo->attachmentCount);
        if (req.imageless) {
            for (auto* n = static_cast<const VkBaseInStructure*>(pCreateInfo->pNext); n != nullptr;
                 n = n->pNext) {
                if (n->sType != VK_STRUCTURE_TYPE_FRAMEBUFFER_ATTACHMENTS_CREATE_INFO) {
                    continue;
                }
                const auto* attachments =
                    reinterpret_cast<const VkFramebufferAttachmentsCreateInfo*>(n);
                req.attachment_infos.reserve(attachments->attachmentImageInfoCount);
                for (std::uint32_t i = 0; i < attachments->attachmentImageInfoCount; ++i) {
                    const VkFramebufferAttachmentImageInfo& src =
                        attachments->pAttachmentImageInfos[i];
                    vkrpc::FramebufferAttachmentInfoDesc dst;
                    dst.flags = static_cast<std::uint64_t>(src.flags);
                    dst.usage = static_cast<std::uint64_t>(src.usage);
                    dst.width = static_cast<int>(src.width);
                    dst.height = static_cast<int>(src.height);
                    dst.layer_count = static_cast<int>(src.layerCount);
                    if (src.pViewFormats != nullptr) {
                        for (std::uint32_t j = 0; j < src.viewFormatCount; ++j) {
                            dst.view_formats.push_back(static_cast<int>(src.pViewFormats[j]));
                        }
                    }
                    req.attachment_infos.push_back(std::move(dst));
                }
                break;
            }
        }
        if (!req.imageless) {
            req.image_view =
                (pCreateInfo->attachmentCount >= 1 && pCreateInfo->pAttachments != nullptr)
                    ? from_handle(pCreateInfo->pAttachments[0])
                    : 0;
            // Depth view: the second attachment, when present (0 = none).
            req.depth_image_view =
                (pCreateInfo->attachmentCount >= 2 && pCreateInfo->pAttachments != nullptr)
                    ? from_handle(pCreateInfo->pAttachments[1])
                    : 0;
            // MRT: ALL views, positional (a 2-view framebuffer may be color+color, which the
            // scalar color/depth fields cannot express). The scalars above stay populated for an
            // old worker; a new worker prefers this vector.
            if (pCreateInfo->pAttachments != nullptr) {
                for (std::uint32_t i = 0; i < pCreateInfo->attachmentCount; ++i) {
                    req.attachment_views.push_back(from_handle(pCreateInfo->pAttachments[i]));
                }
            }
        }
        req.width = static_cast<int>(pCreateInfo->width);
        req.height = static_cast<int>(pCreateInfo->height);
        req.layers = static_cast<int>(pCreateInfo->layers);
        icd_trace("CreateFramebuffer request render_pass=%llu imageless=%d extent=%ux%u layers=%u "
                  "attachments=%u",
                  static_cast<unsigned long long>(req.render_pass), req.imageless ? 1 : 0,
                  pCreateInfo->width, pCreateInfo->height, pCreateInfo->layers,
                  pCreateInfo->attachmentCount);
        const vkrpc::CreateFramebufferResponse r =
            vkrpc::create_framebuffer(*g_rpc, next_id(), req);
        if (!r.ok) {
            std::fprintf(stderr,
                         "vkrelay2-icd: create_framebuffer rejected by worker -- %s "
                         "(app requested %ux%u, attachments=%u)\n",
                         r.reason.c_str(), pCreateInfo->width, pCreateInfo->height,
                         pCreateInfo->attachmentCount);
            return fault();
        }
        icd_trace("CreateFramebuffer response framebuffer=%llu",
                  static_cast<unsigned long long>(r.framebuffer));
        *pFramebuffer = to_handle<VkFramebuffer>(r.framebuffer);
        return VK_SUCCESS;
    } catch (const std::exception&) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
}

VKAPI_ATTR void VKAPI_CALL DestroyFramebuffer(VkDevice, VkFramebuffer framebuffer,
                                              const VkAllocationCallbacks*) {
    if (framebuffer == VK_NULL_HANDLE) {
        return;
    }
    std::lock_guard<std::mutex> lk(g_mu);
    try {
        vkrpc::HandleRequest req;
        req.handle = from_handle(framebuffer);
        (void) vkrpc::destroy_framebuffer(*g_rpc, next_id(), req);
    } catch (const std::exception&) {
    }
}

VKAPI_ATTR VkResult VKAPI_CALL CreatePipelineLayout(VkDevice device,
                                                    const VkPipelineLayoutCreateInfo* pCreateInfo,
                                                    const VkAllocationCallbacks*,
                                                    VkPipelineLayout* pPipelineLayout) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto* dev = reinterpret_cast<DeviceImpl*>(device);
    try {
        const char* why = "";
        if (!vkr::icd_subset::pipeline_layout_ok(pCreateInfo, &why)) {
            std::fprintf(stderr, "vkrelay2-icd: rejecting pipeline layout -- %s\n", why);
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        vkrpc::CreatePipelineLayoutRequest req;
        req.device = dev->worker;
        req.set_layout_count = static_cast<int>(pCreateInfo->setLayoutCount);
        req.push_constant_range_count = static_cast<int>(pCreateInfo->pushConstantRangeCount);
        // forward the set-layout handle list (the predicate bounded the count + guarded
        // the array). empty layouts carry an empty list.
        for (std::uint32_t i = 0; i < pCreateInfo->setLayoutCount; ++i) {
            req.set_layouts.push_back(from_handle(pCreateInfo->pSetLayouts[i]));
        }
        // (GL/zink): forward the validated push-constant ranges.
        for (std::uint32_t i = 0; i < pCreateInfo->pushConstantRangeCount; ++i) {
            const VkPushConstantRange& pc = pCreateInfo->pPushConstantRanges[i];
            vkrpc::PushConstantRange r2;
            r2.stage_flags = static_cast<std::uint32_t>(pc.stageFlags);
            r2.offset = pc.offset;
            r2.size = pc.size;
            req.push_constant_ranges.push_back(r2);
        }
        const vkrpc::CreatePipelineLayoutResponse r =
            vkrpc::create_pipeline_layout(*g_rpc, next_id(), req);
        if (!r.ok) {
            return fault();
        }
        *pPipelineLayout = to_handle<VkPipelineLayout>(r.pipeline_layout);
        return VK_SUCCESS;
    } catch (const std::exception&) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
}

VKAPI_ATTR void VKAPI_CALL DestroyPipelineLayout(VkDevice, VkPipelineLayout pipelineLayout,
                                                 const VkAllocationCallbacks*) {
    if (pipelineLayout == VK_NULL_HANDLE) {
        return;
    }
    std::lock_guard<std::mutex> lk(g_mu);
    try {
        vkrpc::HandleRequest req;
        req.handle = from_handle(pipelineLayout);
        (void) vkrpc::destroy_pipeline_layout(*g_rpc, next_id(), req);
    } catch (const std::exception&) {
    }
}

// --- Descriptor surface + per-frame UBO -----------------
//
// The descriptor object graph is validated fail-closed by the icd_subset predicates here, then
// forwarded; the worker is the authority for the stateful checks (update-vs-layout, layout-exact
// bind, draw-readiness), which surface through record_command_buffer at vkEndCommandBuffer. The
// void vkUpdateDescriptorSets cannot return an error, so a failed/out-of-subset update is logged
// and not applied -- the worker's set stays uninitialized and the next recording's draw is refused.

// descriptorIndexing: vkGetDescriptorSetLayoutSupport now forwards to the worker
// (RpcOp 88) so the HOST's real answer -- supported + maxVariableDescriptorCount -- replaces the
// old local "supported=TRUE + generous max" fiction. Version-skew fallback: on
// a failed RPC (an old worker replies UnknownOp), a CLASSIC layout (no DI shape) keeps the old
// advisory local answer (no regression on a mismatched pair), but a DI-SHAPED layout (any binding
// flag / UAB layout flag) reports supported=FALSE -- a new ICD must never run the DI path against
// a worker that would ignore its variable-count/layout-support semantics.
VKAPI_ATTR void VKAPI_CALL
GetDescriptorSetLayoutSupport(VkDevice device, const VkDescriptorSetLayoutCreateInfo* pCreateInfo,
                              VkDescriptorSetLayoutSupport* pSupport) {
    if (pSupport == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lk(g_mu);
    auto* dev = reinterpret_cast<DeviceImpl*>(device);
    const auto answer = [&](VkBool32 supported, std::uint32_t max_variable) {
        pSupport->supported = supported;
        for (auto* node = static_cast<VkBaseOutStructure*>(pSupport->pNext); node != nullptr;
             node = node->pNext) {
            if (node->sType ==
                VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_LAYOUT_SUPPORT) {
                reinterpret_cast<VkDescriptorSetVariableDescriptorCountLayoutSupport*>(node)
                    ->maxVariableDescriptorCount = max_variable;
            }
        }
    };
    // Build the wire shape (the create-request struct; nothing is created). A create-info the
    // relay cannot even parse (malformed binding-flags shape) is honestly unsupported.
    const char* why = "";
    if (pCreateInfo == nullptr || !vkr::icd_subset::descriptor_set_layout_ok(pCreateInfo, &why)) {
        answer(VK_FALSE, 0);
        return;
    }
    const VkDescriptorBindingFlags* pBindingFlags = nullptr;
    std::uint32_t bindingFlagsCount = 0;
    for (auto* n = static_cast<const VkBaseInStructure*>(pCreateInfo->pNext); n != nullptr;
         n = n->pNext) {
        if (n->sType == VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO) {
            const auto* bf =
                reinterpret_cast<const VkDescriptorSetLayoutBindingFlagsCreateInfo*>(n);
            pBindingFlags = bf->pBindingFlags;
            bindingFlagsCount = bf->bindingCount;
        }
    }
    vkrpc::CreateDescriptorSetLayoutRequest req;
    req.device = dev->worker;
    req.layout_flags = static_cast<long long>(pCreateInfo->flags);
    bool di_shaped = (req.layout_flags & vkrpc::kDescriptorSetLayoutCreateUpdateAfterBindPool) != 0;
    for (std::uint32_t i = 0; i < pCreateInfo->bindingCount; ++i) {
        const VkDescriptorSetLayoutBinding& b = pCreateInfo->pBindings[i];
        vkrpc::DescriptorSetLayoutBindingDesc d;
        d.binding = static_cast<int>(b.binding);
        d.descriptor_type = static_cast<int>(b.descriptorType);
        d.descriptor_count = static_cast<int>(b.descriptorCount);
        d.stage_flags = static_cast<long long>(b.stageFlags);
        if (pBindingFlags != nullptr && i < bindingFlagsCount) {
            d.binding_flags = static_cast<long long>(pBindingFlags[i]);
        }
        di_shaped = di_shaped || d.binding_flags != 0;
        req.bindings.push_back(d);
    }
    // The relay's own admission first: a layout create would reject -> honestly unsupported.
    if (!vkrpc::descriptor_indexing_layout_ok(req.layout_flags, req.bindings,
                                              dev->descriptor_indexing_feature_bits, &why)) {
        answer(VK_FALSE, 0);
        return;
    }
    try {
        const vkrpc::GetDescriptorSetLayoutSupportResponse r =
            vkrpc::get_descriptor_set_layout_support(*g_rpc, next_id(), req);
        if (r.ok) {
            answer(r.supported != 0 ? VK_TRUE : VK_FALSE,
                   static_cast<std::uint32_t>(r.max_variable_descriptor_count));
            return;
        }
        answer(VK_FALSE, 0); // the worker answered but refused: honestly unsupported
    } catch (const std::exception&) {
        // Old worker (UnknownOp) / transport skew: advisory for classic layouts, fail-closed for
        // DI-shaped ones.
        answer(di_shaped ? VK_FALSE : VK_TRUE, di_shaped ? 0 : (1u << 20));
    }
}

VKAPI_ATTR VkResult VKAPI_CALL
CreateDescriptorSetLayout(VkDevice device, const VkDescriptorSetLayoutCreateInfo* pCreateInfo,
                          const VkAllocationCallbacks*, VkDescriptorSetLayout* pSetLayout) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto* dev = reinterpret_cast<DeviceImpl*>(device);
    try {
        const char* why = "";
        if (!vkr::icd_subset::descriptor_set_layout_ok(pCreateInfo, &why)) {
            std::fprintf(stderr, "vkrelay2-icd: rejecting descriptor set layout -- %s\n", why);
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        // (GL/zink): the per-binding VkDescriptorBindingFlags ride a
        // VkDescriptorSetLayoutBindingFlagsCreateInfo pNext; index it by binding position (its
        // bindingCount must match pBindings, or the flags do not apply).
        const VkDescriptorBindingFlags* pBindingFlags = nullptr;
        std::uint32_t bindingFlagsCount = 0;
        for (auto* n = static_cast<const VkBaseInStructure*>(pCreateInfo->pNext); n != nullptr;
             n = n->pNext) {
            if (n->sType == VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO) {
                const auto* bf =
                    reinterpret_cast<const VkDescriptorSetLayoutBindingFlagsCreateInfo*>(n);
                pBindingFlags = bf->pBindingFlags;
                bindingFlagsCount = bf->bindingCount;
            }
        }
        vkrpc::CreateDescriptorSetLayoutRequest req;
        req.device = dev->worker;
        req.layout_flags = static_cast<long long>(pCreateInfo->flags);
        for (std::uint32_t i = 0; i < pCreateInfo->bindingCount; ++i) {
            const VkDescriptorSetLayoutBinding& b = pCreateInfo->pBindings[i];
            vkrpc::DescriptorSetLayoutBindingDesc d;
            d.binding = static_cast<int>(b.binding);
            d.descriptor_type = static_cast<int>(b.descriptorType);
            d.descriptor_count = static_cast<int>(b.descriptorCount);
            d.stage_flags = static_cast<long long>(b.stageFlags);
            if (pBindingFlags != nullptr && i < bindingFlagsCount) {
                d.binding_flags = static_cast<long long>(pBindingFlags[i]);
            }
            req.bindings.push_back(d);
        }
        // descriptorIndexing: the SHARED per-binding-flag admission -- every DI flag is
        // gated on the device's enabled kDIFeature* bits (0 while masked -> any DI-flagged layout
        // fails closed here, loudly, before the wire).
        if (!vkrpc::descriptor_indexing_layout_ok(req.layout_flags, req.bindings,
                                                  dev->descriptor_indexing_feature_bits, &why)) {
            std::fprintf(stderr, "vkrelay2-icd: rejecting descriptor set layout -- %s\n", why);
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        const vkrpc::CreateDescriptorSetLayoutResponse r =
            vkrpc::create_descriptor_set_layout(*g_rpc, next_id(), req);
        if (!r.ok) {
            return fault();
        }
        *pSetLayout = to_handle<VkDescriptorSetLayout>(r.set_layout);
        return VK_SUCCESS;
    } catch (const std::exception&) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
}

VKAPI_ATTR void VKAPI_CALL DestroyDescriptorSetLayout(VkDevice, VkDescriptorSetLayout setLayout,
                                                      const VkAllocationCallbacks*) {
    if (setLayout == VK_NULL_HANDLE) {
        return;
    }
    std::lock_guard<std::mutex> lk(g_mu);
    try {
        vkrpc::HandleRequest req;
        req.handle = from_handle(setLayout);
        (void) vkrpc::destroy_descriptor_set_layout(*g_rpc, next_id(), req);
    } catch (const std::exception&) {
    }
}

VKAPI_ATTR VkResult VKAPI_CALL CreateDescriptorPool(VkDevice device,
                                                    const VkDescriptorPoolCreateInfo* pCreateInfo,
                                                    const VkAllocationCallbacks*,
                                                    VkDescriptorPool* pDescriptorPool) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto* dev = reinterpret_cast<DeviceImpl*>(device);
    try {
        const char* why = "";
        if (!vkr::icd_subset::descriptor_pool_ok(pCreateInfo, &why)) {
            std::fprintf(stderr, "vkrelay2-icd: rejecting descriptor pool -- %s\n", why);
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        // descriptorIndexing: the pool UPDATE_AFTER_BIND flag is a
        // CONTAINER flag (limit-bucket selection) -- valid without any UAB feature, so it rides
        // the wire unconditionally; the per-binding UAB flags carry the feature gates.
        vkrpc::CreateDescriptorPoolRequest req;
        req.device = dev->worker;
        req.max_sets = static_cast<int>(pCreateInfo->maxSets);
        req.flags = static_cast<long long>(pCreateInfo->flags);
        // Vulkan 1.3 support (inlineUniformBlock): VkDescriptorPoolInlineUniformBlockCreateInfo is
        // the ONE admitted pool pNext (the predicate above rejected everything else, fail-closed);
        // its maxInlineUniformBlockBindings rides the wire and the worker rebuilds the struct.
        // Both backends gate it: an IUB pool size without it, or either without the enabled
        // inlineUniformBlock feature, rejects there.
        for (auto* n = static_cast<const VkBaseInStructure*>(pCreateInfo->pNext); n != nullptr;
             n = n->pNext) {
            if (n->sType != VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_INLINE_UNIFORM_BLOCK_CREATE_INFO) {
                continue; // unreachable post-predicate; the copy walk stays total
            }
            const auto* ib =
                reinterpret_cast<const VkDescriptorPoolInlineUniformBlockCreateInfo*>(n);
            req.max_inline_uniform_block_bindings =
                static_cast<int>(ib->maxInlineUniformBlockBindings);
        }
        for (std::uint32_t i = 0; i < pCreateInfo->poolSizeCount; ++i) {
            vkrpc::DescriptorPoolSizeDesc s;
            s.type = static_cast<int>(pCreateInfo->pPoolSizes[i].type);
            s.descriptor_count = static_cast<int>(pCreateInfo->pPoolSizes[i].descriptorCount);
            req.pool_sizes.push_back(s);
        }
        const vkrpc::CreateDescriptorPoolResponse r =
            vkrpc::create_descriptor_pool(*g_rpc, next_id(), req);
        if (!r.ok) {
            icd_trace("CreateDescriptorPool FAILED: %s", r.reason.c_str());
            return fault();
        }
        *pDescriptorPool = to_handle<VkDescriptorPool>(r.pool);
        return VK_SUCCESS;
    } catch (const std::exception&) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
}

VKAPI_ATTR void VKAPI_CALL DestroyDescriptorPool(VkDevice, VkDescriptorPool descriptorPool,
                                                 const VkAllocationCallbacks*) {
    if (descriptorPool == VK_NULL_HANDLE) {
        return;
    }
    std::lock_guard<std::mutex> lk(g_mu);
    try {
        vkrpc::HandleRequest req;
        req.handle = from_handle(descriptorPool);
        (void) vkrpc::destroy_descriptor_pool(*g_rpc, next_id(), req);
    } catch (const std::exception&) {
    }
}

VKAPI_ATTR VkResult VKAPI_CALL
AllocateDescriptorSets(VkDevice device, const VkDescriptorSetAllocateInfo* pAllocateInfo,
                       VkDescriptorSet* pDescriptorSets) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto* dev = reinterpret_cast<DeviceImpl*>(device);
    try {
        if (pAllocateInfo == nullptr || pDescriptorSets == nullptr ||
            (pAllocateInfo->descriptorSetCount > 0 && pAllocateInfo->pSetLayouts == nullptr)) {
            std::fprintf(stderr, "vkrelay2-icd: rejecting descriptor set alloc -- null pointer\n");
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        // descriptorIndexing: the ONE admitted pNext is
        // VkDescriptorSetVariableDescriptorCountAllocateInfo, validated by the PURE
        // icd_subset predicate (the spec's descriptorSetCount == 0 means "as if
        // absent" and is accepted BEFORE the feature gate -- only a NONZERO count engages the
        // feature and the must-parallel rule).
        const char* why = "";
        if (!vkr::icd_subset::descriptor_set_alloc_ok(pAllocateInfo,
                                                      (dev->descriptor_indexing_feature_bits &
                                                       vkrpc::kDIFeatureVariableDescriptorCount) !=
                                                          0,
                                                      &why)) {
            std::fprintf(stderr, "vkrelay2-icd: rejecting descriptor set alloc -- %s\n", why);
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        vkrpc::AllocateDescriptorSetsRequest req;
        for (auto* n = static_cast<const VkBaseInStructure*>(pAllocateInfo->pNext); n != nullptr;
             n = n->pNext) {
            if (n->sType !=
                VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO) {
                continue; // unreachable post-predicate; the copy walk stays total
            }
            const auto* vc =
                reinterpret_cast<const VkDescriptorSetVariableDescriptorCountAllocateInfo*>(n);
            for (std::uint32_t i = 0; i < vc->descriptorSetCount; ++i) {
                req.variable_counts.push_back(static_cast<std::uint64_t>(vc->pDescriptorCounts[i]));
            }
        }
        req.device = dev->worker;
        req.pool = from_handle(pAllocateInfo->descriptorPool);
        for (std::uint32_t i = 0; i < pAllocateInfo->descriptorSetCount; ++i) {
            req.set_layouts.push_back(from_handle(pAllocateInfo->pSetLayouts[i]));
        }
        const vkrpc::AllocateDescriptorSetsResponse r =
            vkrpc::allocate_descriptor_sets(*g_rpc, next_id(), req);
        if (!r.ok || r.descriptor_sets.size() != pAllocateInfo->descriptorSetCount) {
            return fault();
        }
        for (std::uint32_t i = 0; i < pAllocateInfo->descriptorSetCount; ++i) {
            pDescriptorSets[i] = to_handle<VkDescriptorSet>(r.descriptor_sets[i]);
        }
        return VK_SUCCESS;
    } catch (const std::exception&) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
}

// (GL/zink): classify a VkDescriptorType + fill the matching info list (buffer / image /
// texel) of a WriteDescriptorSetDesc faithfully -- shared by vkUpdateDescriptorSets AND the
// descriptor-update-template expansion. The worker builds the host VkWriteDescriptorSet by the same
// class.
void fill_write_infos(vkrpc::WriteDescriptorSetDesc& d, VkDescriptorType type, std::uint32_t count,
                      const VkDescriptorBufferInfo* pBufferInfo,
                      const VkDescriptorImageInfo* pImageInfo,
                      const VkBufferView* pTexelBufferView) {
    switch (type) {
    case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
    case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
    case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
    case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
        if (pBufferInfo != nullptr) {
            for (std::uint32_t j = 0; j < count; ++j) {
                vkrpc::DescriptorBufferInfoDesc bi;
                bi.buffer = from_handle(pBufferInfo[j].buffer);
                bi.offset = static_cast<std::uint64_t>(pBufferInfo[j].offset);
                bi.range = static_cast<std::uint64_t>(pBufferInfo[j].range);
                d.buffer_infos.push_back(bi);
            }
        }
        break;
    case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
    case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
        if (pTexelBufferView != nullptr) {
            for (std::uint32_t j = 0; j < count; ++j) {
                d.texel_buffer_views.push_back(from_handle(pTexelBufferView[j]));
            }
        }
        break;
    default: // SAMPLER / SAMPLED_IMAGE / STORAGE_IMAGE / COMBINED_IMAGE_SAMPLER / INPUT_ATTACHMENT
        if (pImageInfo != nullptr) {
            for (std::uint32_t j = 0; j < count; ++j) {
                vkrpc::DescriptorImageInfoDesc ii;
                ii.sampler = from_handle(pImageInfo[j].sampler);
                ii.image_view = from_handle(pImageInfo[j].imageView);
                ii.image_layout = static_cast<int>(pImageInfo[j].imageLayout);
                d.image_infos.push_back(ii);
            }
        }
        break;
    }
}

VKAPI_ATTR void VKAPI_CALL UpdateDescriptorSets(VkDevice device, std::uint32_t descriptorWriteCount,
                                                const VkWriteDescriptorSet* pDescriptorWrites,
                                                std::uint32_t descriptorCopyCount,
                                                const VkCopyDescriptorSet* pDescriptorCopies) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto* dev = reinterpret_cast<DeviceImpl*>(device);
    // vkUpdateDescriptorSets is void, so the worker is the single validate-or-poison authority:
    // a rejected update must POISON its target sets, not silently leave a
    // previously- valid set draw-ready. So forward EVERY write rather than dropping out-of-subset
    // ones -- a faithfully-representable write (UNIFORM_BUFFER + pBufferInfo) carries its buffer
    // infos; any other write is forwarded with EMPTY buffer_infos so the worker rejects the batch
    // (count/type mismatch) and poisons the named sets. Copies are out of subset: each copy's
    // dstSet is named as a poison write so a rejected copy poisons it too.
    if (descriptorWriteCount > 0 && pDescriptorWrites == nullptr) {
        std::fprintf(stderr, "vkrelay2-icd: vkUpdateDescriptorSets null writes -- dropped\n");
        return;
    }
    if (descriptorCopyCount > 0 && pDescriptorCopies == nullptr) {
        std::fprintf(stderr, "vkrelay2-icd: vkUpdateDescriptorSets null copies -- dropped\n");
        return;
    }
    vkrpc::UpdateDescriptorSetsRequest req;
    req.device = dev->worker;
    for (std::uint32_t i = 0; i < descriptorWriteCount; ++i) {
        const VkWriteDescriptorSet& w = pDescriptorWrites[i];
        vkrpc::WriteDescriptorSetDesc d;
        d.dst_set = from_handle(w.dstSet);
        d.dst_binding = static_cast<int>(w.dstBinding);
        d.dst_array_element = static_cast<int>(w.dstArrayElement);
        d.descriptor_type = static_cast<int>(w.descriptorType);
        d.descriptor_count = static_cast<int>(w.descriptorCount);
        // (GL/zink): carry the matching info list for ANY descriptor type (the worker
        // builds the host write by class + is the authority). A malformed write leaves the lists
        // empty so the worker rejects the batch + poisons the named set (the void-return
        // substitute).
        fill_write_infos(d, w.descriptorType, w.descriptorCount, w.pBufferInfo, w.pImageInfo,
                         w.pTexelBufferView);
        // Vulkan 1.3 support (inlineUniformBlock): an IUB write's payload rides a
        // VkWriteDescriptorSetInlineUniformBlock pNext -- copy its raw bytes verbatim. A missing
        // struct, a null pData, or a dataSize that disagrees with descriptorCount forwards an
        // EMPTY inline_data, so the worker rejects the batch + poisons the named set (the same
        // malformed-write posture as the empty info lists above).
        if (w.descriptorType == VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK) {
            for (auto* n = static_cast<const VkBaseInStructure*>(w.pNext); n != nullptr;
                 n = n->pNext) {
                if (n->sType != VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_INLINE_UNIFORM_BLOCK) {
                    continue;
                }
                const auto* ib = reinterpret_cast<const VkWriteDescriptorSetInlineUniformBlock*>(n);
                if (ib->pData != nullptr && ib->dataSize == w.descriptorCount) {
                    d.inline_data.assign(static_cast<const char*>(ib->pData), ib->dataSize);
                }
                break;
            }
        }
        req.writes.push_back(std::move(d));
    }
    for (std::uint32_t i = 0; i < descriptorCopyCount; ++i) {
        vkrpc::WriteDescriptorSetDesc d;
        d.dst_set = from_handle(pDescriptorCopies[i].dstSet);
        d.dst_binding = static_cast<int>(pDescriptorCopies[i].dstBinding);
        d.dst_array_element = static_cast<int>(pDescriptorCopies[i].dstArrayElement);
        d.descriptor_type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        d.descriptor_count =
            1; // empty buffer_infos -> guaranteed worker rejection -> poison dstSet
        req.writes.push_back(std::move(d));
    }
    if (req.writes.empty()) {
        return; // no writes and no copies: nothing to do
    }
    try {
        const vkrpc::StatusResponse r = vkrpc::update_descriptor_sets(*g_rpc, next_id(), req);
        if (!r.ok) {
            std::fprintf(stderr, "vkrelay2-icd: vkUpdateDescriptorSets rejected -- %s\n",
                         r.reason.c_str());
        }
    } catch (const std::exception&) {
        std::fprintf(stderr, "vkrelay2-icd: vkUpdateDescriptorSets transport error\n");
    }
}

// (GL/zink): descriptor update templates (core 1.1; zink uses them for its descriptor
// management). Shimmed ENTIRELY ICD-side -- no worker op: the template's entries are stored locally
// at create, and vkUpdateDescriptorSetWithTemplate expands {entries, pData} into ordinary
// descriptor writes (reading each element at pData + offset + i*stride) forwarded via the existing
// update_descriptor_sets RPC. The template handle is a local id (never sent to the worker).
struct UpdateTemplateImpl {
    std::vector<VkDescriptorUpdateTemplateEntry> entries;
};
std::map<std::uint64_t, UpdateTemplateImpl> g_update_templates;
std::uint64_t g_next_update_template = 1;

VKAPI_ATTR VkResult VKAPI_CALL CreateDescriptorUpdateTemplate(
    VkDevice, const VkDescriptorUpdateTemplateCreateInfo* pCreateInfo, const VkAllocationCallbacks*,
    VkDescriptorUpdateTemplate* pTemplate) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (pCreateInfo == nullptr || pTemplate == nullptr) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    UpdateTemplateImpl t;
    for (std::uint32_t i = 0; i < pCreateInfo->descriptorUpdateEntryCount; ++i) {
        t.entries.push_back(pCreateInfo->pDescriptorUpdateEntries[i]);
    }
    const std::uint64_t h = g_next_update_template++;
    g_update_templates.emplace(h, std::move(t));
    *pTemplate = to_handle<VkDescriptorUpdateTemplate>(h);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
UpdateDescriptorSetWithTemplate(VkDevice device, VkDescriptorSet set,
                                VkDescriptorUpdateTemplate updateTemplate, const void* pData) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto* dev = reinterpret_cast<DeviceImpl*>(device);
    const auto it = g_update_templates.find(from_handle(updateTemplate));
    if (it == g_update_templates.end() || pData == nullptr) {
        return;
    }
    const char* base = static_cast<const char*>(pData);
    vkrpc::UpdateDescriptorSetsRequest req;
    req.device = dev->worker;
    for (const VkDescriptorUpdateTemplateEntry& e : it->second.entries) {
        vkrpc::WriteDescriptorSetDesc d;
        d.dst_set = from_handle(set);
        d.dst_binding = static_cast<int>(e.dstBinding);
        d.dst_array_element = static_cast<int>(e.dstArrayElement);
        d.descriptor_type = static_cast<int>(e.descriptorType);
        d.descriptor_count = static_cast<int>(e.descriptorCount);
        // Read the descriptorCount elements strided in pData into contiguous temporaries, then fill
        // by class. The element type follows the descriptor type (buffer / texel-view / image).
        // Vulkan 1.3 support (inlineUniformBlock): an IUB template entry's payload is RAW BYTES at
        // pData + offset (stride is ignored per spec; descriptorCount is the byte count) -- it
        // must NOT be read as an info-struct array, so it takes its own branch.
        const VkDescriptorType type = e.descriptorType;
        if (type == VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK) {
            d.inline_data.assign(base + e.offset, e.descriptorCount);
        } else if (type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ||
                   type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ||
                   type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC ||
                   type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC) {
            std::vector<VkDescriptorBufferInfo> tmp(e.descriptorCount);
            for (std::uint32_t j = 0; j < e.descriptorCount; ++j) {
                tmp[j] = *reinterpret_cast<const VkDescriptorBufferInfo*>(base + e.offset +
                                                                          j * e.stride);
            }
            fill_write_infos(d, type, e.descriptorCount, tmp.data(), nullptr, nullptr);
        } else if (type == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER ||
                   type == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER) {
            std::vector<VkBufferView> tmp(e.descriptorCount);
            for (std::uint32_t j = 0; j < e.descriptorCount; ++j) {
                tmp[j] = *reinterpret_cast<const VkBufferView*>(base + e.offset + j * e.stride);
            }
            fill_write_infos(d, type, e.descriptorCount, nullptr, nullptr, tmp.data());
        } else {
            std::vector<VkDescriptorImageInfo> tmp(e.descriptorCount);
            for (std::uint32_t j = 0; j < e.descriptorCount; ++j) {
                tmp[j] =
                    *reinterpret_cast<const VkDescriptorImageInfo*>(base + e.offset + j * e.stride);
            }
            fill_write_infos(d, type, e.descriptorCount, nullptr, tmp.data(), nullptr);
        }
        req.writes.push_back(std::move(d));
    }
    if (req.writes.empty()) {
        return;
    }
    try {
        const vkrpc::StatusResponse r = vkrpc::update_descriptor_sets(*g_rpc, next_id(), req);
        if (!r.ok) {
            std::fprintf(stderr, "vkrelay2-icd: vkUpdateDescriptorSetWithTemplate rejected -- %s\n",
                         r.reason.c_str());
        }
    } catch (const std::exception&) {
        std::fprintf(stderr, "vkrelay2-icd: vkUpdateDescriptorSetWithTemplate transport error\n");
    }
}

VKAPI_ATTR void VKAPI_CALL DestroyDescriptorUpdateTemplate(
    VkDevice, VkDescriptorUpdateTemplate updateTemplate, const VkAllocationCallbacks*) {
    if (updateTemplate == VK_NULL_HANDLE) {
        return;
    }
    std::lock_guard<std::mutex> lk(g_mu);
    g_update_templates.erase(from_handle(updateTemplate));
}

// Vulkan 1.3 support: report NO pipeline-creation feedback (clear the VALID bits) on an admitted
// VkPipelineCreationFeedbackCreateInfo -- the spec-allowed answer for an implementation that
// provides none (the honest one here: creation happens across the relay boundary).
void clear_pipeline_feedback(const void* pnext) {
    for (auto* n = static_cast<const VkBaseInStructure*>(pnext); n != nullptr; n = n->pNext) {
        if (n->sType != VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO) {
            continue;
        }
        const auto* f = reinterpret_cast<const VkPipelineCreationFeedbackCreateInfo*>(n);
        if (f->pPipelineCreationFeedback != nullptr) {
            f->pPipelineCreationFeedback->flags = 0;
            f->pPipelineCreationFeedback->duration = 0;
        }
        for (std::uint32_t i = 0; f->pPipelineStageCreationFeedbacks != nullptr &&
                                  i < f->pipelineStageCreationFeedbackCount;
             ++i) {
            f->pPipelineStageCreationFeedbacks[i].flags = 0;
            f->pPipelineStageCreationFeedbacks[i].duration = 0;
        }
    }
}

VKAPI_ATTR VkResult VKAPI_CALL CreateGraphicsPipelines(
    VkDevice device, VkPipelineCache pipelineCache, std::uint32_t createInfoCount,
    const VkGraphicsPipelineCreateInfo* pCreateInfos, const VkAllocationCallbacks*,
    VkPipeline* pPipelines) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto* dev = reinterpret_cast<DeviceImpl*>(device);
    try {
        // supports exactly one pipeline per call (the contract's pipelineCount == 1).
        if (createInfoCount != 1) {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        const VkGraphicsPipelineCreateInfo& ci = pCreateInfos[0];
        const char* why = "";
        // admit a VkPipelineRenderingCreateInfo top-level pNext ONLY on a device
        // that enabled VK_KHR_dynamic_rendering on the native lane (dynamic_rendering_enabled).
        // Every other device keeps the byte-identical "no top-level pNext" guard. Vulkan 1.3
        // support: the cache-control flags / subgroup-size stage shapes / creation-feedback pNext
        // are admitted per the device's enabled vk13 bits (documented at the predicate).
        if (!vkr::icd_subset::graphics_pipeline_ok(
                &ci, dev->dynamic_rendering_enabled,
                (dev->vk13_feature_bits & vkrpc::kVk13FeaturePipelineCreationCacheControl) != 0,
                (dev->vk13_feature_bits & vkrpc::kVk13FeatureSubgroupSizeControl) != 0,
                dev->vk13_device, dev->vertex_attr_divisor_enabled,
                dev->transform_feedback_enabled && dev->geometry_streams_feature_enabled &&
                    dev->worker_rasterization_stream,
                &why)) {
            std::fprintf(stderr, "vkrelay2-icd: rejecting graphics pipeline -- %s\n", why);
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        clear_pipeline_feedback(ci.pNext);
        // Vulkan 1.3 support (pipelineCreationCacheControl, served ICD-LOCALLY): the relay's
        // pipeline cache is an honest empty no-op, so compilation is ALWAYS required -- a create
        // that asks to fail-rather-than-compile answers VK_PIPELINE_COMPILE_REQUIRED without
        // forwarding.
        if ((ci.flags & VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT) != 0) {
            pPipelines[0] = VK_NULL_HANDLE;
            return VK_PIPELINE_COMPILE_REQUIRED;
        }
        vkrpc::CreateGraphicsPipelinesRequest req;
        req.device = dev->worker;
        // The relay does not use a pipeline cache (the worker builds pipelines fresh), so the app's
        // cache -- if any -- is ignored: always forward null. A pipeline cache is purely an
        // optimization hint, so an ineffective cache is spec-legal, and this keeps the worker's
        // "pipeline_cache must be null" contract intact even when a real app (e.g. the public cube
        // demo) supplies one.
        (void) pipelineCache;
        req.pipeline_cache = 0;
        for (std::uint32_t i = 0; i < ci.stageCount; ++i) {
            vkrpc::ShaderStageDesc s;
            s.stage = static_cast<int>(ci.pStages[i].stage);
            s.module = from_handle(ci.pStages[i].module);
            s.entry = ci.pStages[i].pName != nullptr ? ci.pStages[i].pName : "";
            // Vulkan 1.3 support (subgroupSizeControl): the admitted stage flags + the
            // required-size pNext ride the wire verbatim (the predicate already gated them on the
            // enabled features; the worker re-gates + rebuilds the pNext).
            s.stage_flags = static_cast<long long>(ci.pStages[i].flags);
            for (auto* n = static_cast<const VkBaseInStructure*>(ci.pStages[i].pNext); n != nullptr;
                 n = n->pNext) {
                if (n->sType ==
                    VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO) {
                    s.required_subgroup_size = static_cast<int>(
                        reinterpret_cast<
                            const VkPipelineShaderStageRequiredSubgroupSizeCreateInfo*>(n)
                            ->requiredSubgroupSize);
                }
            }
            req.stages.push_back(std::move(s));
        }
        if (ci.pInputAssemblyState != nullptr) {
            req.topology = static_cast<int>(ci.pInputAssemblyState->topology);
            req.primitive_restart_enable =
                static_cast<int>(ci.pInputAssemblyState->primitiveRestartEnable);
        }
        if (ci.pTessellationState != nullptr) {
            req.patch_control_points = static_cast<int>(ci.pTessellationState->patchControlPoints);
        }
        if (ci.pViewportState != nullptr) {
            req.viewport_count = static_cast<int>(ci.pViewportState->viewportCount);
            req.scissor_count = static_cast<int>(ci.pViewportState->scissorCount);
        }
        if (ci.pVertexInputState != nullptr) {
            const VkPipelineVertexInputStateCreateInfo& vi = *ci.pVertexInputState;
            req.vertex_binding_count = static_cast<int>(vi.vertexBindingDescriptionCount);
            req.vertex_attribute_count = static_cast<int>(vi.vertexAttributeDescriptionCount);
            // Carry the full binding/attribute descriptions: every field of both
            // Vulkan structs rides the wire, so the worker validates them all (nothing silently
            // dropped).
            for (std::uint32_t i = 0; i < vi.vertexBindingDescriptionCount; ++i) {
                const VkVertexInputBindingDescription& b = vi.pVertexBindingDescriptions[i];
                vkrpc::VertexBindingDesc vb;
                vb.binding = static_cast<int>(b.binding);
                vb.stride = static_cast<long long>(b.stride);
                vb.input_rate = static_cast<int>(b.inputRate);
                req.vertex_bindings.push_back(vb);
            }
            for (std::uint32_t i = 0; i < vi.vertexAttributeDescriptionCount; ++i) {
                const VkVertexInputAttributeDescription& a = vi.pVertexAttributeDescriptions[i];
                vkrpc::VertexAttributeDesc va;
                va.location = static_cast<int>(a.location);
                va.binding = static_cast<int>(a.binding);
                va.format = static_cast<int>(a.format);
                va.offset = static_cast<long long>(a.offset);
                req.vertex_attributes.push_back(va);
            }
            // vertex-attr-divisor: extract the VkPipelineVertexInputDivisorStateCreateInfoEXT the
            // predicate already admitted (only present when the ext was enabled) into the wire
            // array.
            for (auto* n = static_cast<const VkBaseInStructure*>(vi.pNext); n != nullptr;
                 n = n->pNext) {
                if (n->sType !=
                    VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_EXT) {
                    continue;
                }
                const auto* div =
                    reinterpret_cast<const VkPipelineVertexInputDivisorStateCreateInfoEXT*>(n);
                req.vertex_divisor_present = 1;
                for (std::uint32_t i = 0; i < div->vertexBindingDivisorCount; ++i) {
                    vkrpc::VertexBindingDivisorDesc vd;
                    vd.binding = static_cast<int>(div->pVertexBindingDivisors[i].binding);
                    vd.divisor = static_cast<long long>(div->pVertexBindingDivisors[i].divisor);
                    req.vertex_binding_divisors.push_back(vd);
                }
            }
            // Validate the extracted divisor content against the bindings + the ENABLED feature
            // bits (shared with the worker + mock -- mock == real). Fail closed locally with a
            // named reason before the RPC, rather than letting the host driver be the first to
            // reject it.
            std::string div_why;
            if (!vkrpc::vertex_binding_divisors_ok(
                    req.vertex_divisor_present, req.vertex_bindings, req.vertex_binding_divisors,
                    dev->vertex_attr_divisor_enabled, dev->vertex_attr_divisor_feature_enabled,
                    dev->vertex_attr_zero_divisor_feature_enabled, div_why)) {
                std::fprintf(stderr, "vkrelay2-icd: rejecting graphics pipeline -- %s\n",
                             div_why.c_str());
                return VK_ERROR_INITIALIZATION_FAILED;
            }
        }
        if (ci.pRasterizationState != nullptr) {
            const VkPipelineRasterizationStateCreateInfo& rs = *ci.pRasterizationState;
            req.cull_mode = static_cast<int>(rs.cullMode);
            req.front_face = static_cast<int>(rs.frontFace);
            req.polygon_mode = static_cast<int>(rs.polygonMode);
            req.depth_clamp_enable = static_cast<int>(rs.depthClampEnable);
            req.rasterizer_discard_enable = static_cast<int>(rs.rasterizerDiscardEnable);
            req.depth_bias_enable = static_cast<int>(rs.depthBiasEnable);
            req.depth_bias_constant = rs.depthBiasConstantFactor;
            req.depth_bias_clamp = rs.depthBiasClamp;
            req.depth_bias_slope = rs.depthBiasSlopeFactor;
            req.line_width = rs.lineWidth;
            // (GL/zink) RENDER CORRECTNESS: when VK_EXT_depth_clip_enable is enabled, zink
            // chains a VkPipelineRasterizationDepthClipStateCreateInfoEXT on the rasterization
            // pNext to control near/far clipping (GL_DEPTH_CLAMP). Carry it FAITHFULLY so the
            // worker rebuilds the same pNext on the real pipeline; absent => no pNext (default
            // clip). icd_subset::graphics_pipeline_ok admits exactly this rasterization pNext.
            // line state (VK_EXT_line_rasterization): zink picks the GL line-rasterization mode
            // here.
            for (const auto* n = static_cast<const VkBaseInStructure*>(rs.pNext); n != nullptr;
                 n = n->pNext) {
                if (n->sType ==
                    VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_DEPTH_CLIP_STATE_CREATE_INFO_EXT) {
                    const auto* dc =
                        reinterpret_cast<const VkPipelineRasterizationDepthClipStateCreateInfoEXT*>(
                            n);
                    req.depth_clip_state_present = 1;
                    req.depth_clip_enable = static_cast<int>(dc->depthClipEnable);
                } else if (n->sType ==
                           VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_LINE_STATE_CREATE_INFO_EXT) {
                    const auto* ls =
                        reinterpret_cast<const VkPipelineRasterizationLineStateCreateInfoEXT*>(n);
                    req.line_state_present = 1;
                    req.line_rasterization_mode = static_cast<int>(ls->lineRasterizationMode);
                    req.line_stipple_enable = static_cast<int>(ls->stippledLineEnable);
                    req.line_stipple_factor = static_cast<int>(ls->lineStippleFactor);
                    req.line_stipple_pattern = static_cast<int>(ls->lineStipplePattern);
                } else if (n->sType ==
                           VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_STREAM_CREATE_INFO_EXT) {
                    // geometry-stream: admission (graphics_pipeline_ok) already gated ext+feature
                    // enablement, uniqueness, and reserved flags == 0; carry the one field wide
                    // (u32 -> long long, no narrowing). The worker/mock validate the VALUE against
                    // their real capabilities via the shared rasterization_stream_ok.
                    const auto* ss =
                        reinterpret_cast<const VkPipelineRasterizationStateStreamCreateInfoEXT*>(n);
                    req.stream_state_present = 1;
                    req.rasterization_stream = static_cast<long long>(ss->rasterizationStream);
                }
            }
        }
        if (ci.pMultisampleState != nullptr) {
            const VkPipelineMultisampleStateCreateInfo& ms = *ci.pMultisampleState;
            req.rasterization_samples = static_cast<int>(ms.rasterizationSamples);
            req.sample_shading_enable = static_cast<int>(ms.sampleShadingEnable);
            // Sample mask: one VkSampleMask word (valid for <=32 samples; graphics_pipeline_ok
            // caps).
            req.sample_mask =
                ms.pSampleMask != nullptr ? static_cast<long long>(ms.pSampleMask[0]) : -1;
            req.min_sample_shading = ms.minSampleShading;
            req.alpha_to_coverage_enable = static_cast<int>(ms.alphaToCoverageEnable);
            req.alpha_to_one_enable = static_cast<int>(ms.alphaToOneEnable);
        }
        if (ci.pColorBlendState != nullptr) {
            const VkPipelineColorBlendStateCreateInfo& cb = *ci.pColorBlendState;
            req.logic_op_enable = static_cast<int>(cb.logicOpEnable);
            req.logic_op = static_cast<int>(cb.logicOp);
            for (int i = 0; i < 4; ++i) {
                req.blend_constants[i] = cb.blendConstants[i];
            }
            for (std::uint32_t i = 0; i < cb.attachmentCount; ++i) {
                const VkPipelineColorBlendAttachmentState& a = cb.pAttachments[i];
                vkrpc::ColorBlendAttachmentDesc d;
                d.blend_enable = static_cast<int>(a.blendEnable);
                d.src_color_factor = static_cast<int>(a.srcColorBlendFactor);
                d.dst_color_factor = static_cast<int>(a.dstColorBlendFactor);
                d.color_blend_op = static_cast<int>(a.colorBlendOp);
                d.src_alpha_factor = static_cast<int>(a.srcAlphaBlendFactor);
                d.dst_alpha_factor = static_cast<int>(a.dstAlphaBlendFactor);
                d.alpha_blend_op = static_cast<int>(a.alphaBlendOp);
                d.color_write_mask = static_cast<int>(a.colorWriteMask);
                req.color_blend_attachments.push_back(d);
            }
        }
        if (ci.pDynamicState != nullptr) {
            for (std::uint32_t i = 0; i < ci.pDynamicState->dynamicStateCount; ++i) {
                req.dynamic_states.push_back(static_cast<int>(ci.pDynamicState->pDynamicStates[i]));
            }
        }
        req.layout = from_handle(ci.layout);
        req.render_pass = from_handle(ci.renderPass);
        req.subpass = static_cast<int>(ci.subpass);
        // a dynamic-rendering pipeline (renderPass == VK_NULL_HANDLE) declares its
        // attachment FORMATS via a VkPipelineRenderingCreateInfo on the top-level pNext (admitted
        // above only on the native DR lane). Carry viewMask + color/depth/stencil formats so the
        // worker rebuilds the struct -- forwarding it is MANDATORY (the NVIDIA driver segfaults on
        // a NULL-renderpass pipeline create with no rendering info). renderPass forwarding is
        // unchanged (VK_NULL_HANDLE -> 0); the worker rejects render_pass != 0 WITH DR info (no
        // mixed mode).
        for (const auto* n = static_cast<const VkBaseInStructure*>(ci.pNext); n != nullptr;
             n = n->pNext) {
            if (n->sType == VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO) {
                const auto* pri = reinterpret_cast<const VkPipelineRenderingCreateInfo*>(n);
                req.has_dynamic_rendering = 1;
                req.dr_view_mask = static_cast<int>(pri->viewMask);
                for (std::uint32_t f = 0; f < pri->colorAttachmentCount; ++f) {
                    req.dr_color_formats.push_back(
                        pri->pColorAttachmentFormats != nullptr
                            ? static_cast<int>(pri->pColorAttachmentFormats[f])
                            : 0);
                }
                req.dr_depth_format = static_cast<int>(pri->depthAttachmentFormat);
                req.dr_stencil_format = static_cast<int>(pri->stencilAttachmentFormat);
            }
        }
        // Depth-stencil state: carry test/write/compare when present (the worker
        // validates them + cross-checks against the render pass's depth attachment).
        if (ci.pDepthStencilState != nullptr) {
            req.has_depth_stencil = true;
            req.depth_test_enable = static_cast<int>(ci.pDepthStencilState->depthTestEnable);
            req.depth_write_enable = static_cast<int>(ci.pDepthStencilState->depthWriteEnable);
            req.depth_compare_op = static_cast<int>(ci.pDepthStencilState->depthCompareOp);
            req.depth_bounds_test_enable =
                static_cast<int>(ci.pDepthStencilState->depthBoundsTestEnable);
            req.stencil_test_enable = static_cast<int>(ci.pDepthStencilState->stencilTestEnable);
            // (GL/zink) RENDER CORRECTNESS: carry the STATIC front/back stencil op state +
            // depth bounds FAITHFULLY. zink bakes the stencil ops + compareOp into the pipeline
            // (compare-mask/write-mask/reference stay CORE dynamic state); dropping them silently
            // broke OpenSCAD's OpenCSG stencil compositing. The worker rebuilds the same
            // VkStencilOpState.
            const VkStencilOpState& sf = ci.pDepthStencilState->front;
            const VkStencilOpState& sb = ci.pDepthStencilState->back;
            req.stencil_front_fail_op = static_cast<int>(sf.failOp);
            req.stencil_front_pass_op = static_cast<int>(sf.passOp);
            req.stencil_front_depth_fail_op = static_cast<int>(sf.depthFailOp);
            req.stencil_front_compare_op = static_cast<int>(sf.compareOp);
            req.stencil_front_compare_mask = static_cast<int>(sf.compareMask);
            req.stencil_front_write_mask = static_cast<int>(sf.writeMask);
            req.stencil_front_reference = static_cast<int>(sf.reference);
            req.stencil_back_fail_op = static_cast<int>(sb.failOp);
            req.stencil_back_pass_op = static_cast<int>(sb.passOp);
            req.stencil_back_depth_fail_op = static_cast<int>(sb.depthFailOp);
            req.stencil_back_compare_op = static_cast<int>(sb.compareOp);
            req.stencil_back_compare_mask = static_cast<int>(sb.compareMask);
            req.stencil_back_write_mask = static_cast<int>(sb.writeMask);
            req.stencil_back_reference = static_cast<int>(sb.reference);
            req.min_depth_bounds = ci.pDepthStencilState->minDepthBounds;
            req.max_depth_bounds = ci.pDepthStencilState->maxDepthBounds;
        }
        // AMD-iGPU viewport-corruption diagnostic: one bounded, handle-light pipeline summary at
        // the guest decision boundary. Sorting these lines across adapter runs answers whether
        // zink selected a different graphics path before we instrument the wire/worker. Hash only
        // plain pipeline state (never shader bytes or application payloads); disabled tracing pays
        // no hashing cost.
        if (icd_trace_enabled()) {
            std::uint64_t stage_mask = 0;
            std::uint64_t vertex_hash = 1469598103934665603ull;
            std::uint64_t dynamic_hash = 1469598103934665603ull;
            std::uint64_t blend_hash = 1469598103934665603ull;
            const auto mix = [](std::uint64_t& hash, std::uint64_t value) {
                hash ^= value;
                hash *= 1099511628211ull;
            };
            for (const auto& stage : req.stages) {
                stage_mask |= static_cast<std::uint64_t>(stage.stage);
            }
            for (const auto& binding : req.vertex_bindings) {
                mix(vertex_hash, static_cast<std::uint64_t>(binding.binding));
                mix(vertex_hash, static_cast<std::uint64_t>(binding.stride));
                mix(vertex_hash, static_cast<std::uint64_t>(binding.input_rate));
            }
            for (const auto& attribute : req.vertex_attributes) {
                mix(vertex_hash, static_cast<std::uint64_t>(attribute.location));
                mix(vertex_hash, static_cast<std::uint64_t>(attribute.binding));
                mix(vertex_hash, static_cast<std::uint64_t>(attribute.format));
                mix(vertex_hash, static_cast<std::uint64_t>(attribute.offset));
            }
            for (const int state : req.dynamic_states) {
                mix(dynamic_hash, static_cast<std::uint64_t>(state));
            }
            for (const auto& blend : req.color_blend_attachments) {
                mix(blend_hash, static_cast<std::uint64_t>(blend.blend_enable));
                mix(blend_hash, static_cast<std::uint64_t>(blend.src_color_factor));
                mix(blend_hash, static_cast<std::uint64_t>(blend.dst_color_factor));
                mix(blend_hash, static_cast<std::uint64_t>(blend.color_blend_op));
                mix(blend_hash, static_cast<std::uint64_t>(blend.src_alpha_factor));
                mix(blend_hash, static_cast<std::uint64_t>(blend.dst_alpha_factor));
                mix(blend_hash, static_cast<std::uint64_t>(blend.alpha_blend_op));
                mix(blend_hash, static_cast<std::uint64_t>(blend.color_write_mask));
            }
            icd_trace(
                "GraphicsPipeline stages=0x%llx topology=%d vb=%d va=%d vhash=0x%llx "
                "cull=%d front=%d poly=%d line_present=%d line_mode=%d samples=%d depth=%d/%d/%d "
                "stencil=%d blends=%zu bhash=0x%llx dynamic=%zu dhash=0x%llx renderpass=%llu "
                "subpass=%d",
                static_cast<unsigned long long>(stage_mask), req.topology, req.vertex_binding_count,
                req.vertex_attribute_count, static_cast<unsigned long long>(vertex_hash),
                req.cull_mode, req.front_face, req.polygon_mode, req.line_state_present,
                req.line_rasterization_mode, req.rasterization_samples,
                req.has_depth_stencil ? 1 : 0, req.depth_test_enable, req.depth_write_enable,
                req.stencil_test_enable, req.color_blend_attachments.size(),
                static_cast<unsigned long long>(blend_hash), req.dynamic_states.size(),
                static_cast<unsigned long long>(dynamic_hash),
                static_cast<unsigned long long>(req.render_pass), req.subpass);
        }
        const vkrpc::CreateGraphicsPipelinesResponse r =
            vkrpc::create_graphics_pipelines(*g_rpc, next_id(), req);
        if (!r.ok) {
            // The worker's carried refusal reason is the ONLY way a user (or a triage run) can
            // see WHICH subset boundary an app's pipeline hit -- swallowing it made every
            // refusal an anonymous INITIALIZATION_FAILED.
            std::fprintf(stderr, "vkrelay2-icd: worker rejected graphics pipeline -- %s\n",
                         r.reason.c_str());
            return fault();
        }
        pPipelines[0] = to_handle<VkPipeline>(r.pipeline);
        return VK_SUCCESS;
    } catch (const std::exception&) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
}

// Compute. One pipeline per call (the graphics contract); the app's pipeline cache is
// ignored like the graphics path (a cache is an optimization hint; the worker builds fresh).
// GATED on the worker-advertised COMPUTE queue bit -- an old worker (queue_flags == 0) fails
// closed HERE, at creation, so no dispatch can ever exist against it.
VKAPI_ATTR VkResult VKAPI_CALL CreateComputePipelines(
    VkDevice device, VkPipelineCache pipelineCache, std::uint32_t createInfoCount,
    const VkComputePipelineCreateInfo* pCreateInfos, const VkAllocationCallbacks*,
    VkPipeline* pPipelines) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto* dev = reinterpret_cast<DeviceImpl*>(device);
    try {
        if ((dev->queue_flags & VK_QUEUE_COMPUTE_BIT) == 0) {
            std::fprintf(stderr, "vkrelay2-icd: rejecting compute pipeline -- the worker did not "
                                 "advertise a COMPUTE queue (old worker?)\n");
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        if (createInfoCount != 1) {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        const VkComputePipelineCreateInfo& ci = pCreateInfos[0];
        const char* why = "";
        // Vulkan 1.3 support: the cache-control flags / subgroup-size stage shapes /
        // creation-feedback pNext are admitted per the device's enabled vk13 bits (documented at
        // the predicate).
        if (!vkr::icd_subset::compute_pipeline_ok(
                &ci,
                (dev->vk13_feature_bits & vkrpc::kVk13FeaturePipelineCreationCacheControl) != 0,
                (dev->vk13_feature_bits & vkrpc::kVk13FeatureSubgroupSizeControl) != 0,
                (dev->vk13_feature_bits & vkrpc::kVk13FeatureComputeFullSubgroups) != 0,
                dev->vk13_device, &why)) {
            std::fprintf(stderr, "vkrelay2-icd: rejecting compute pipeline -- %s\n", why);
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        clear_pipeline_feedback(ci.pNext);
        // Vulkan 1.3 support (pipelineCreationCacheControl, ICD-LOCAL): compilation is ALWAYS
        // required through the relay (the local pipeline cache is an honest empty no-op).
        if ((ci.flags & VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT) != 0) {
            pPipelines[0] = VK_NULL_HANDLE;
            return VK_PIPELINE_COMPILE_REQUIRED;
        }
        (void) pipelineCache;
        vkrpc::CreateComputePipelinesRequest req;
        req.device = dev->worker;
        req.pipeline_cache = 0; // always null-forwarded (shape symmetry; documented ignored)
        req.layout = from_handle(ci.layout);
        req.shader_module = from_handle(ci.stage.module);
        req.entry_point = ci.stage.pName;
        // Vulkan 1.3 support (subgroupSizeControl): the admitted stage flags + required-size pNext.
        req.stage_flags = static_cast<long long>(ci.stage.flags);
        for (auto* n = static_cast<const VkBaseInStructure*>(ci.stage.pNext); n != nullptr;
             n = n->pNext) {
            if (n->sType ==
                VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO) {
                req.required_subgroup_size = static_cast<int>(
                    reinterpret_cast<const VkPipelineShaderStageRequiredSubgroupSizeCreateInfo*>(n)
                        ->requiredSubgroupSize);
            }
        }
        const vkrpc::CreateComputePipelinesResponse r =
            vkrpc::create_compute_pipelines(*g_rpc, next_id(), req);
        if (!r.ok) {
            return fault();
        }
        pPipelines[0] = to_handle<VkPipeline>(r.pipeline);
        return VK_SUCCESS;
    } catch (const std::exception&) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
}

VKAPI_ATTR void VKAPI_CALL DestroyPipeline(VkDevice, VkPipeline pipeline,
                                           const VkAllocationCallbacks*) {
    if (pipeline == VK_NULL_HANDLE) {
        return;
    }
    std::lock_guard<std::mutex> lk(g_mu);
    try {
        vkrpc::HandleRequest req;
        req.handle = from_handle(pipeline);
        (void) vkrpc::destroy_pipeline(*g_rpc, next_id(), req);
    } catch (const std::exception&) {
    }
}

// Pipeline cache (real-app surface). A real app (e.g. the public cube demo) creates a
// pipeline cache, passes it to vkCreateGraphicsPipelines as an optimization hint, then destroys it.
// The relay caches nothing (the worker builds pipelines fresh), so this is a LOCAL no-op: mint a
// unique non-dispatchable handle, ignore it at pipeline creation (CreateGraphicsPipelines forces a
// null cache), and accept it back at destroy. No RPC, no per-cache state. An empty/ineffective
// cache is spec-legal, so this is faithful rather than a shortcut -- and it is what stops vkcube
// from calling a NULL dispatch slot (the canaries never exercised this: they pass VK_NULL_HANDLE).
VKAPI_ATTR VkResult VKAPI_CALL CreatePipelineCache(VkDevice, const VkPipelineCacheCreateInfo*,
                                                   const VkAllocationCallbacks*,
                                                   VkPipelineCache* pCache) {
    std::lock_guard<std::mutex> lk(g_mu);
    *pCache = to_handle<VkPipelineCache>(g_next_local_handle++);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL DestroyPipelineCache(VkDevice, VkPipelineCache,
                                                const VkAllocationCallbacks*) {
    // Local no-op: there is no per-cache state to free (the relay never cached anything).
}

// (GL/zink): zink reads back the pipeline cache to persist it. The relay caches nothing, so
// report an EMPTY cache (size 0) -- spec-legal; the app stores nothing.
VKAPI_ATTR VkResult VKAPI_CALL GetPipelineCacheData(VkDevice, VkPipelineCache,
                                                    std::size_t* pDataSize, void*) {
    if (pDataSize != nullptr) {
        *pDataSize = 0;
    }
    return VK_SUCCESS;
}

// --- Vulkan 1.3 support: private data (core 1.3), served ICD-LOCALLY -------------------------
// Private data has NO driver-observable semantics -- it is an app-side {slot, objectType,
// objectHandle} -> u64 association -- so a local store is a fully conformant implementation
// with zero wire traffic. Gated on the ENABLED privateData feature (fail closed, the relay
// rule: commands whose feature was not enabled do not work). VkDevicePrivateDataCreateInfo on
// device create (reserved-slot count) is a pure optimization HINT; the chain capture skips it
// as an unknown sType, which is the spec-allowed "no reservation" behavior.
VKAPI_ATTR VkResult VKAPI_CALL CreatePrivateDataSlot(VkDevice device,
                                                     const VkPrivateDataSlotCreateInfo* pCreateInfo,
                                                     const VkAllocationCallbacks*,
                                                     VkPrivateDataSlot* pPrivateDataSlot) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto* dev = reinterpret_cast<DeviceImpl*>(device);
    if (pCreateInfo == nullptr || pPrivateDataSlot == nullptr) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if ((dev->vk13_feature_bits & vkrpc::kVk13FeaturePrivateData) == 0) {
        std::fprintf(stderr, "vkrelay2-icd: rejecting private-data slot -- the privateData "
                             "feature was not enabled on this device\n");
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (pCreateInfo->pNext != nullptr || pCreateInfo->flags != 0) {
        std::fprintf(stderr, "vkrelay2-icd: rejecting private-data slot -- unknown pNext/flags "
                             "(fail closed)\n");
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    const std::uint64_t slot = dev->next_private_data_slot++;
    dev->private_data_slots.insert(slot);
    *pPrivateDataSlot = reinterpret_cast<VkPrivateDataSlot>(slot);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL DestroyPrivateDataSlot(VkDevice device, VkPrivateDataSlot slot,
                                                  const VkAllocationCallbacks*) {
    if (slot == VK_NULL_HANDLE) {
        return;
    }
    std::lock_guard<std::mutex> lk(g_mu);
    auto* dev = reinterpret_cast<DeviceImpl*>(device);
    const std::uint64_t s = reinterpret_cast<std::uint64_t>(slot);
    dev->private_data_slots.erase(s);
    // Drop every association stored under this slot (keys are ordered slot-first).
    auto it = dev->private_data.lower_bound(std::make_tuple(s, LLONG_MIN, 0ull));
    while (it != dev->private_data.end() && std::get<0>(it->first) == s) {
        it = dev->private_data.erase(it);
    }
}

VKAPI_ATTR VkResult VKAPI_CALL SetPrivateData(VkDevice device, VkObjectType objectType,
                                              std::uint64_t objectHandle, VkPrivateDataSlot slot,
                                              std::uint64_t data) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto* dev = reinterpret_cast<DeviceImpl*>(device);
    const std::uint64_t s = reinterpret_cast<std::uint64_t>(slot);
    if (dev->private_data_slots.count(s) == 0) {
        return VK_ERROR_INITIALIZATION_FAILED; // unknown/destroyed slot: fail closed
    }
    dev->private_data[std::make_tuple(s, static_cast<long long>(objectType), objectHandle)] = data;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL GetPrivateData(VkDevice device, VkObjectType objectType,
                                          std::uint64_t objectHandle, VkPrivateDataSlot slot,
                                          std::uint64_t* pData) {
    if (pData == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lk(g_mu);
    auto* dev = reinterpret_cast<DeviceImpl*>(device);
    const std::uint64_t s = reinterpret_cast<std::uint64_t>(slot);
    const auto it = dev->private_data.find(
        std::make_tuple(s, static_cast<long long>(objectType), objectHandle));
    *pData = it == dev->private_data.end() ? 0 : it->second; // never-set reads 0, per spec
}

// --- Host-visible memory + buffers ---------------------
// The memory-visibility spine: honest memory (cached props + buffer requirements), real host-backed
// allocations, a Linux-side shadow buffer the app maps, and the dirty chunks shipped at
// flush/submit.

VKAPI_ATTR VkResult VKAPI_CALL CreateBuffer(VkDevice device, const VkBufferCreateInfo* pCreateInfo,
                                            const VkAllocationCallbacks*, VkBuffer* pBuffer) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto* dev = reinterpret_cast<DeviceImpl*>(device);
    try {
        const char* why = "";
        if (!vkr::icd_subset::buffer_ok(pCreateInfo, &why)) {
            std::fprintf(stderr, "vkrelay2-icd: rejecting buffer -- %s\n", why);
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        vkrpc::CreateBufferRequest req;
        req.device = dev->worker;
        req.size = static_cast<std::uint64_t>(pCreateInfo->size);
        req.usage = static_cast<std::uint64_t>(pCreateInfo->usage);
        req.sharing_mode = static_cast<int>(pCreateInfo->sharingMode);
        const vkrpc::CreateBufferResponse r = vkrpc::create_buffer(*g_rpc, next_id(), req);
        if (!r.ok) {
            return fault();
        }
        *pBuffer = to_handle<VkBuffer>(r.buffer);
        // Cache the real requirements so the void getter is a pure copy.
        VkMemoryRequirements mr{};
        mr.size = r.mem_size;
        mr.alignment = r.mem_alignment;
        mr.memoryTypeBits = static_cast<std::uint32_t>(r.mem_type_bits);
        BufferInfo info;
        info.requirements = mr;
        info.size = pCreateInfo->size;
        info.usage = pCreateInfo->usage;
        g_buffers[r.buffer] = info;
        return VK_SUCCESS;
    } catch (const std::exception&) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
}

VKAPI_ATTR void VKAPI_CALL DestroyBuffer(VkDevice, VkBuffer buffer, const VkAllocationCallbacks*) {
    if (buffer == VK_NULL_HANDLE) {
        return;
    }
    std::lock_guard<std::mutex> lk(g_mu);
    try {
        vkrpc::HandleRequest req;
        req.handle = from_handle(buffer);
        (void) vkrpc::destroy_buffer(*g_rpc, next_id(), req);
        g_buffers.erase(from_handle(buffer));
    } catch (const std::exception&) {
    }
}

VKAPI_ATTR void VKAPI_CALL GetBufferMemoryRequirements(VkDevice, VkBuffer buffer,
                                                       VkMemoryRequirements* pRequirements) {
    // Pure copy of the requirements cached at vkCreateBuffer (this entrypoint is void).
    std::lock_guard<std::mutex> lk(g_mu);
    const auto it = g_buffers.find(from_handle(buffer));
    *pRequirements = it != g_buffers.end() ? it->second.requirements : VkMemoryRequirements{};
}

VKAPI_ATTR VkResult VKAPI_CALL BindBufferMemory(VkDevice, VkBuffer buffer, VkDeviceMemory memory,
                                                VkDeviceSize memoryOffset) {
    std::lock_guard<std::mutex> lk(g_mu);
    try {
        vkrpc::BindBufferMemoryRequest req;
        req.buffer = from_handle(buffer);
        req.memory = from_handle(memory);
        req.memory_offset = static_cast<std::uint64_t>(memoryOffset);
        const vkrpc::StatusResponse r = vkrpc::bind_buffer_memory(*g_rpc, next_id(), req);
        if (r.ok) {
            const auto it = g_buffers.find(req.buffer);
            if (it != g_buffers.end()) {
                it->second.memory = req.memory;
            }
        }
        return r.ok ? VK_SUCCESS : fault();
    } catch (const std::exception&) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
}

// Vulkan 1.2 bufferDeviceAddress support: the REAL host VkDeviceAddress, verbatim. A
// VkDeviceAddress is raw GPU-VA data (not a handle) -- the app writes it into shader-visible
// memory and the GPU dereferences it, so it cannot ride the opaque-handle table; the worker
// queries the real vkGetBufferDeviceAddress on the translated buffer and ships the host VA
// unmodified (the guest's shaders execute on the host device that minted it). Fail-closed to 0
// (+ a loud log) on every invalid use -- the entry returns VkDeviceAddress, not VkResult, and 0
// is never a valid address. Core-only serving: the KHR/EXT aliases are deliberately NOT
// registered (the extensions are not advertised; a 1.2 app uses the core name).
VKAPI_ATTR VkDeviceAddress VKAPI_CALL
GetBufferDeviceAddress(VkDevice device, const VkBufferDeviceAddressInfo* pInfo) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto* dev = reinterpret_cast<DeviceImpl*>(device);
    if (!dev->buffer_device_address_enabled) {
        std::fprintf(stderr, "vkrelay2-icd: vkGetBufferDeviceAddress rejected -- the device did "
                             "not enable the bufferDeviceAddress feature (native lane only)\n");
        return 0;
    }
    if (pInfo == nullptr || pInfo->pNext != nullptr || pInfo->buffer == VK_NULL_HANDLE) {
        std::fprintf(stderr, "vkrelay2-icd: vkGetBufferDeviceAddress rejected -- null/chained "
                             "VkBufferDeviceAddressInfo\n");
        return 0;
    }
    try {
        vkrpc::GetBufferDeviceAddressRequest req;
        req.device = dev->worker;
        req.buffer = from_handle(pInfo->buffer);
        const vkrpc::GetBufferDeviceAddressResponse r =
            vkrpc::get_buffer_device_address(*g_rpc, next_id(), req);
        if (!r.ok || r.device_address == 0) {
            std::fprintf(stderr, "vkrelay2-icd: vkGetBufferDeviceAddress failed -- %s\n",
                         r.reason.c_str());
            return 0;
        }
        icd_trace("GetBufferDeviceAddress buffer=%llu -> 0x%llx",
                  static_cast<unsigned long long>(req.buffer),
                  static_cast<unsigned long long>(r.device_address));
        return static_cast<VkDeviceAddress>(r.device_address);
    } catch (const std::exception&) {
        return 0;
    }
}

// --- Images + depth ------------------------------------

VKAPI_ATTR VkResult VKAPI_CALL CreateImage(VkDevice device, const VkImageCreateInfo* pCreateInfo,
                                           const VkAllocationCallbacks*, VkImage* pImage) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto* dev = reinterpret_cast<DeviceImpl*>(device);
    try {
        const char* why = "";
        if (!vkr::icd_subset::image_ok(pCreateInfo, &why)) {
            std::fprintf(stderr, "vkrelay2-icd: rejecting image -- %s\n", why);
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        vkrpc::CreateImageRequest req;
        req.device = dev->worker;
        req.image_type = static_cast<int>(pCreateInfo->imageType);
        req.format = static_cast<int>(pCreateInfo->format);
        req.width = static_cast<int>(pCreateInfo->extent.width);
        req.height = static_cast<int>(pCreateInfo->extent.height);
        req.depth = static_cast<int>(pCreateInfo->extent.depth);
        req.mip_levels = static_cast<int>(pCreateInfo->mipLevels);
        req.array_layers = static_cast<int>(pCreateInfo->arrayLayers);
        req.samples = static_cast<int>(pCreateInfo->samples);
        req.tiling = static_cast<int>(pCreateInfo->tiling);
        req.usage = static_cast<std::uint64_t>(pCreateInfo->usage);
        icd_trace("CreateImage format=%d extent=%ux%u usage=0x%llx flags=0x%x",
                  static_cast<int>(pCreateInfo->format), pCreateInfo->extent.width,
                  pCreateInfo->extent.height, static_cast<unsigned long long>(req.usage),
                  static_cast<unsigned int>(pCreateInfo->flags));
        req.sharing_mode = static_cast<int>(pCreateInfo->sharingMode);
        req.initial_layout = static_cast<int>(pCreateInfo->initialLayout);
        req.image_flags = static_cast<long long>(pCreateInfo->flags);
        // (GL/zink): carry the VkImageFormatListCreateInfo view formats (mutable-format
        // aliasing); the worker rebuilds the pNext from them.
        for (auto* n = static_cast<const VkBaseInStructure*>(pCreateInfo->pNext); n != nullptr;
             n = n->pNext) {
            if (n->sType == VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO) {
                const auto* fl = reinterpret_cast<const VkImageFormatListCreateInfo*>(n);
                for (std::uint32_t i = 0; i < fl->viewFormatCount; ++i) {
                    req.view_formats.push_back(static_cast<int>(fl->pViewFormats[i]));
                }
            }
        }
        const vkrpc::CreateImageResponse r = vkrpc::create_image(*g_rpc, next_id(), req);
        if (!r.ok) {
            return fault();
        }
        *pImage = to_handle<VkImage>(r.image);
        // Cache the real requirements + (LINEAR) subresource layout so the void getters are pure
        // copies (the create_buffer pattern).
        VkMemoryRequirements mr{};
        mr.size = r.mem_size;
        mr.alignment = r.mem_alignment;
        mr.memoryTypeBits = static_cast<std::uint32_t>(r.mem_type_bits);
        g_image_reqs[r.image] = mr;
        if (r.has_subresource_layout) {
            VkSubresourceLayout sl{};
            sl.offset = r.sr_offset;
            sl.size = r.sr_size;
            sl.rowPitch = r.sr_row_pitch;
            g_image_subresource[r.image] = sl;
        }
        return VK_SUCCESS;
    } catch (const std::exception&) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
}

VKAPI_ATTR void VKAPI_CALL DestroyImage(VkDevice, VkImage image, const VkAllocationCallbacks*) {
    if (image == VK_NULL_HANDLE) {
        return;
    }
    std::lock_guard<std::mutex> lk(g_mu);
    try {
        vkrpc::HandleRequest req;
        req.handle = from_handle(image);
        (void) vkrpc::destroy_image(*g_rpc, next_id(), req);
        g_image_reqs.erase(from_handle(image));
        g_image_subresource.erase(from_handle(image));
    } catch (const std::exception&) {
    }
}

VKAPI_ATTR void VKAPI_CALL GetImageMemoryRequirements(VkDevice, VkImage image,
                                                      VkMemoryRequirements* pRequirements) {
    // Pure copy of the requirements cached at vkCreateImage (this entrypoint is void).
    std::lock_guard<std::mutex> lk(g_mu);
    const auto it = g_image_reqs.find(from_handle(image));
    *pRequirements = it != g_image_reqs.end() ? it->second : VkMemoryRequirements{};
}

VKAPI_ATTR void VKAPI_CALL GetImageSubresourceLayout(VkDevice, VkImage image,
                                                     const VkImageSubresource*,
                                                     VkSubresourceLayout* pLayout) {
    // Pure copy of the LINEAR-tiling subresource layout cached at vkCreateImage (void getter). For
    // a single-mip single-layer image the subresource selector does not change the answer.
    std::lock_guard<std::mutex> lk(g_mu);
    const auto it = g_image_subresource.find(from_handle(image));
    *pLayout = it != g_image_subresource.end() ? it->second : VkSubresourceLayout{};
}

VKAPI_ATTR VkResult VKAPI_CALL BindImageMemory(VkDevice, VkImage image, VkDeviceMemory memory,
                                               VkDeviceSize memoryOffset) {
    std::lock_guard<std::mutex> lk(g_mu);
    try {
        vkrpc::BindImageMemoryRequest req;
        req.image = from_handle(image);
        req.memory = from_handle(memory);
        req.memory_offset = static_cast<std::uint64_t>(memoryOffset);
        const vkrpc::StatusResponse r = vkrpc::bind_image_memory(*g_rpc, next_id(), req);
        return r.ok ? VK_SUCCESS : fault();
    } catch (const std::exception&) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
}

// (GL/zink): the 1.1-core get-memory-requirements2 + bind-memory2 family (also
// VK_KHR_get_memory_requirements2). The *2 getters are pure copies of the requirements cached at
// create (like the 1.0 getters); if the app chained VkMemoryDedicatedRequirements, report "no
// dedicated allocation needed" (the relay backs every allocation with a plain VkDeviceMemory). The
// bind2 family loops the 1.0 bind RPC per VkBindBufferMemoryInfo / VkBindImageMemoryInfo.
void fill_dedicated_reqs(void* pNext) {
    for (auto* n = static_cast<VkBaseOutStructure*>(pNext); n != nullptr; n = n->pNext) {
        if (n->sType == VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS) {
            auto* d = reinterpret_cast<VkMemoryDedicatedRequirements*>(n);
            d->prefersDedicatedAllocation = VK_FALSE;
            d->requiresDedicatedAllocation = VK_FALSE;
        }
    }
}
VKAPI_ATTR void VKAPI_CALL GetBufferMemoryRequirements2(
    VkDevice, const VkBufferMemoryRequirementsInfo2* pInfo, VkMemoryRequirements2* pOut) {
    std::lock_guard<std::mutex> lk(g_mu);
    const auto it = g_buffers.find(from_handle(pInfo->buffer));
    pOut->memoryRequirements =
        it != g_buffers.end() ? it->second.requirements : VkMemoryRequirements{};
    fill_dedicated_reqs(pOut->pNext);
}
VKAPI_ATTR void VKAPI_CALL GetImageMemoryRequirements2(VkDevice,
                                                       const VkImageMemoryRequirementsInfo2* pInfo,
                                                       VkMemoryRequirements2* pOut) {
    std::lock_guard<std::mutex> lk(g_mu);
    const auto it = g_image_reqs.find(from_handle(pInfo->image));
    pOut->memoryRequirements = it != g_image_reqs.end() ? it->second : VkMemoryRequirements{};
    fill_dedicated_reqs(pOut->pNext);
}
VKAPI_ATTR VkResult VKAPI_CALL BindBufferMemory2(VkDevice, std::uint32_t bindInfoCount,
                                                 const VkBindBufferMemoryInfo* pBindInfos) {
    std::lock_guard<std::mutex> lk(g_mu);
    try {
        for (std::uint32_t i = 0; i < bindInfoCount; ++i) {
            vkrpc::BindBufferMemoryRequest req;
            req.buffer = from_handle(pBindInfos[i].buffer);
            req.memory = from_handle(pBindInfos[i].memory);
            req.memory_offset = static_cast<std::uint64_t>(pBindInfos[i].memoryOffset);
            if (!vkrpc::bind_buffer_memory(*g_rpc, next_id(), req).ok) {
                return fault();
            }
            const auto it = g_buffers.find(req.buffer);
            if (it != g_buffers.end()) {
                it->second.memory = req.memory;
            }
        }
        return VK_SUCCESS;
    } catch (const std::exception&) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
}
VKAPI_ATTR VkResult VKAPI_CALL BindImageMemory2(VkDevice, std::uint32_t bindInfoCount,
                                                const VkBindImageMemoryInfo* pBindInfos) {
    std::lock_guard<std::mutex> lk(g_mu);
    try {
        for (std::uint32_t i = 0; i < bindInfoCount; ++i) {
            vkrpc::BindImageMemoryRequest req;
            req.image = from_handle(pBindInfos[i].image);
            req.memory = from_handle(pBindInfos[i].memory);
            req.memory_offset = static_cast<std::uint64_t>(pBindInfos[i].memoryOffset);
            if (!vkrpc::bind_image_memory(*g_rpc, next_id(), req).ok) {
                return fault();
            }
        }
        return VK_SUCCESS;
    } catch (const std::exception&) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
}

// Vulkan 1.3 support (maintenance4): the create-info-shaped memory-requirement queries. Gated on
// the ENABLED maintenance4 feature (fail closed: zeroed requirements + a loud log -- the entry is
// void, so 0 is the honest "no answer"); the admitted create-info subset mirrors what
// create_buffer / create_image serve (EXCLUSIVE sharing, no unknown pNext -- the only admitted
// image pNext is VkImageFormatListCreateInfo), so a query the relay could not also create is
// fail-closed here rather than silently narrowed. The worker answers with the real
// VkMemoryRequirements WITHOUT minting an object (the requests reuse the create-request shapes;
// the response handle stays 0). Chained VkMemoryDedicatedRequirements reads FALSE/FALSE (the
// relay backs every allocation with a plain VkDeviceMemory).
VKAPI_ATTR void VKAPI_CALL
GetDeviceBufferMemoryRequirements(VkDevice device, const VkDeviceBufferMemoryRequirements* pInfo,
                                  VkMemoryRequirements2* pMemoryRequirements) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto* dev = reinterpret_cast<DeviceImpl*>(device);
    pMemoryRequirements->memoryRequirements = VkMemoryRequirements{};
    fill_dedicated_reqs(pMemoryRequirements->pNext);
    if ((dev->vk13_feature_bits & vkrpc::kVk13FeatureMaintenance4) == 0) {
        std::fprintf(stderr, "vkrelay2-icd: vkGetDeviceBufferMemoryRequirements rejected -- the "
                             "maintenance4 feature was not enabled on this device\n");
        return;
    }
    const VkBufferCreateInfo* ci = pInfo != nullptr ? pInfo->pCreateInfo : nullptr;
    if (ci == nullptr || ci->pNext != nullptr || ci->flags != 0 ||
        ci->sharingMode != VK_SHARING_MODE_EXCLUSIVE) {
        std::fprintf(stderr, "vkrelay2-icd: vkGetDeviceBufferMemoryRequirements rejected -- buffer "
                             "create-info pNext/flags/non-EXCLUSIVE sharing (fail closed)\n");
        return;
    }
    try {
        vkrpc::CreateBufferRequest req;
        req.device = dev->worker;
        req.size = static_cast<std::uint64_t>(ci->size);
        req.usage = static_cast<std::uint64_t>(ci->usage);
        req.sharing_mode = static_cast<int>(ci->sharingMode);
        const vkrpc::CreateBufferResponse r =
            vkrpc::get_device_buffer_memory_requirements(*g_rpc, next_id(), req);
        if (!r.ok) {
            icd_trace("GetDeviceBufferMemoryRequirements FAILED: %s", r.reason.c_str());
            return;
        }
        pMemoryRequirements->memoryRequirements.size = r.mem_size;
        pMemoryRequirements->memoryRequirements.alignment = r.mem_alignment;
        pMemoryRequirements->memoryRequirements.memoryTypeBits =
            static_cast<std::uint32_t>(r.mem_type_bits);
    } catch (const std::exception&) {
    }
}
VKAPI_ATTR void VKAPI_CALL
GetDeviceImageMemoryRequirements(VkDevice device, const VkDeviceImageMemoryRequirements* pInfo,
                                 VkMemoryRequirements2* pMemoryRequirements) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto* dev = reinterpret_cast<DeviceImpl*>(device);
    pMemoryRequirements->memoryRequirements = VkMemoryRequirements{};
    fill_dedicated_reqs(pMemoryRequirements->pNext);
    if ((dev->vk13_feature_bits & vkrpc::kVk13FeatureMaintenance4) == 0) {
        std::fprintf(stderr, "vkrelay2-icd: vkGetDeviceImageMemoryRequirements rejected -- the "
                             "maintenance4 feature was not enabled on this device\n");
        return;
    }
    const VkImageCreateInfo* ci = pInfo != nullptr ? pInfo->pCreateInfo : nullptr;
    if (ci == nullptr || ci->sharingMode != VK_SHARING_MODE_EXCLUSIVE ||
        pInfo->planeAspect != 0) { // multi-planar per-plane queries are unwired
        std::fprintf(stderr, "vkrelay2-icd: vkGetDeviceImageMemoryRequirements rejected -- image "
                             "create-info non-EXCLUSIVE sharing / plane aspect (fail closed)\n");
        return;
    }
    vkrpc::CreateImageRequest req;
    // The only admitted create-info pNext is VkImageFormatListCreateInfo (the create_image
    // subset); anything else is fail-closed, never silently dropped.
    for (auto* n = static_cast<const VkBaseInStructure*>(ci->pNext); n != nullptr; n = n->pNext) {
        if (n->sType != VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO) {
            std::fprintf(stderr,
                         "vkrelay2-icd: vkGetDeviceImageMemoryRequirements rejected -- unknown "
                         "image create-info pNext sType=%d (fail closed)\n",
                         static_cast<int>(n->sType));
            return;
        }
        const auto* fl = reinterpret_cast<const VkImageFormatListCreateInfo*>(n);
        for (std::uint32_t i = 0; i < fl->viewFormatCount; ++i) {
            req.view_formats.push_back(static_cast<int>(fl->pViewFormats[i]));
        }
    }
    try {
        req.device = dev->worker;
        req.image_type = static_cast<int>(ci->imageType);
        req.format = static_cast<int>(ci->format);
        req.width = static_cast<int>(ci->extent.width);
        req.height = static_cast<int>(ci->extent.height);
        req.depth = static_cast<int>(ci->extent.depth);
        req.mip_levels = static_cast<int>(ci->mipLevels);
        req.array_layers = static_cast<int>(ci->arrayLayers);
        req.samples = static_cast<int>(ci->samples);
        req.tiling = static_cast<int>(ci->tiling);
        req.usage = static_cast<std::uint64_t>(ci->usage);
        req.sharing_mode = static_cast<int>(ci->sharingMode);
        req.initial_layout = static_cast<int>(ci->initialLayout);
        req.image_flags = static_cast<long long>(ci->flags);
        const vkrpc::CreateImageResponse r =
            vkrpc::get_device_image_memory_requirements(*g_rpc, next_id(), req);
        if (!r.ok) {
            icd_trace("GetDeviceImageMemoryRequirements FAILED: %s", r.reason.c_str());
            return;
        }
        pMemoryRequirements->memoryRequirements.size = r.mem_size;
        pMemoryRequirements->memoryRequirements.alignment = r.mem_alignment;
        pMemoryRequirements->memoryRequirements.memoryTypeBits =
            static_cast<std::uint32_t>(r.mem_type_bits);
    } catch (const std::exception&) {
    }
}
VKAPI_ATTR void VKAPI_CALL GetDeviceImageSparseMemoryRequirements(
    VkDevice, const VkDeviceImageMemoryRequirements*, std::uint32_t* pSparseMemoryRequirementCount,
    VkSparseImageMemoryRequirements2*) {
    // Local: the relay serves no sparse resources (every sparse* feature is reported FALSE), so
    // every queryable image has zero sparse memory requirements -- the spec's answer for a
    // non-sparse image.
    if (pSparseMemoryRequirementCount != nullptr) {
        *pSparseMemoryRequirementCount = 0;
    }
}

// Sampler: the bounded subset is asserted here (sampler_ok); the
// carried fields ride the wire and the worker re-validates + builds the real VkSampler.
VKAPI_ATTR VkResult VKAPI_CALL CreateSampler(VkDevice device,
                                             const VkSamplerCreateInfo* pCreateInfo,
                                             const VkAllocationCallbacks*, VkSampler* pSampler) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto* dev = reinterpret_cast<DeviceImpl*>(device);
    try {
        const char* why = "";
        if (!vkr::icd_subset::sampler_ok(pCreateInfo, &why)) {
            std::fprintf(stderr, "vkrelay2-icd: rejecting sampler -- %s\n", why);
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        vkrpc::CreateSamplerRequest req;
        req.device = dev->worker;
        req.mag_filter = static_cast<int>(pCreateInfo->magFilter);
        req.min_filter = static_cast<int>(pCreateInfo->minFilter);
        req.mipmap_mode = static_cast<int>(pCreateInfo->mipmapMode);
        req.address_mode_u = static_cast<int>(pCreateInfo->addressModeU);
        req.address_mode_v = static_cast<int>(pCreateInfo->addressModeV);
        req.address_mode_w = static_cast<int>(pCreateInfo->addressModeW);
        req.anisotropy_enable = static_cast<int>(pCreateInfo->anisotropyEnable);
        req.compare_enable = static_cast<int>(pCreateInfo->compareEnable);
        // (GL/zink): the rest of VkSamplerCreateInfo, forwarded faithfully.
        req.mip_lod_bias = pCreateInfo->mipLodBias;
        req.max_anisotropy = pCreateInfo->maxAnisotropy;
        req.compare_op = static_cast<int>(pCreateInfo->compareOp);
        req.min_lod = pCreateInfo->minLod;
        req.max_lod = pCreateInfo->maxLod;
        req.border_color = static_cast<int>(pCreateInfo->borderColor);
        req.unnormalized_coordinates = static_cast<int>(pCreateInfo->unnormalizedCoordinates);
        const vkrpc::CreateSamplerResponse r = vkrpc::create_sampler(*g_rpc, next_id(), req);
        if (!r.ok) {
            return fault();
        }
        *pSampler = to_handle<VkSampler>(r.sampler);
        return VK_SUCCESS;
    } catch (const std::exception&) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
}

VKAPI_ATTR void VKAPI_CALL DestroySampler(VkDevice, VkSampler sampler,
                                          const VkAllocationCallbacks*) {
    if (sampler == VK_NULL_HANDLE) {
        return;
    }
    std::lock_guard<std::mutex> lk(g_mu);
    try {
        vkrpc::HandleRequest req;
        req.handle = from_handle(sampler);
        (void) vkrpc::destroy_sampler(*g_rpc, next_id(), req);
    } catch (const std::exception&) {
    }
}

// --- Query pools (GL 3.3 / occlusion / xfb queries) -------------------------
VKAPI_ATTR VkResult VKAPI_CALL CreateQueryPool(VkDevice device,
                                               const VkQueryPoolCreateInfo* pCreateInfo,
                                               const VkAllocationCallbacks*,
                                               VkQueryPool* pQueryPool) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto* dev = reinterpret_cast<DeviceImpl*>(device);
    try {
        if (pCreateInfo == nullptr || pCreateInfo->pNext != nullptr) {
            return VK_ERROR_INITIALIZATION_FAILED; // no query-pool pNext in the subset
        }
        vkrpc::CreateQueryPoolRequest req;
        req.device = dev->worker;
        req.query_type = static_cast<int>(pCreateInfo->queryType);
        req.query_count = pCreateInfo->queryCount;
        req.pipeline_statistics = static_cast<std::uint64_t>(pCreateInfo->pipelineStatistics);
        const vkrpc::CreateQueryPoolResponse r = vkrpc::create_query_pool(*g_rpc, next_id(), req);
        if (!r.ok) {
            return fault();
        }
        *pQueryPool = to_handle<VkQueryPool>(r.query_pool);
        return VK_SUCCESS;
    } catch (const std::exception&) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
}

VKAPI_ATTR void VKAPI_CALL DestroyQueryPool(VkDevice, VkQueryPool queryPool,
                                            const VkAllocationCallbacks*) {
    if (queryPool == VK_NULL_HANDLE) {
        return;
    }
    std::lock_guard<std::mutex> lk(g_mu);
    try {
        vkrpc::HandleRequest req;
        req.handle = from_handle(queryPool);
        (void) vkrpc::destroy_query_pool(*g_rpc, next_id(), req);
    } catch (const std::exception&) {
    }
}

VKAPI_ATTR VkResult VKAPI_CALL GetQueryPoolResults(VkDevice device, VkQueryPool queryPool,
                                                   std::uint32_t firstQuery,
                                                   std::uint32_t queryCount, std::size_t dataSize,
                                                   void* pData, VkDeviceSize stride,
                                                   VkQueryResultFlags flags) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto* dev = reinterpret_cast<DeviceImpl*>(device);
    try {
        vkrpc::GetQueryPoolResultsRequest req;
        req.device = dev->worker;
        req.query_pool = from_handle(queryPool);
        req.first_query = firstQuery;
        req.query_count = queryCount;
        req.data_size = static_cast<std::uint64_t>(dataSize);
        req.stride = static_cast<std::uint64_t>(stride);
        req.flags = static_cast<std::uint64_t>(flags);
        const vkrpc::GetQueryPoolResultsResponse r =
            vkrpc::get_query_pool_results(*g_rpc, next_id(), req);
        if (!r.ok) {
            return fault();
        }
        if (pData != nullptr && dataSize > 0) {
            const std::size_t n = r.data.size() < dataSize ? r.data.size() : dataSize;
            std::memcpy(pData, r.data.data(), n);
        }
        return static_cast<VkResult>(r.vk_result); // faithful SUCCESS / NOT_READY
    } catch (const std::exception&) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
}

// hostQueryReset (required-feature audit): the DEVICE-level vkResetQueryPool -- reset a
// query range FROM THE HOST, no command buffer. Core since Vulkan 1.2; distinct from the
// command-buffer CmdResetQueryPool below. Returns void, so a relay/range error is swallowed (as a
// real driver would -- validation-layer territory); the worker still fails closed on a bad range.
VKAPI_ATTR void VKAPI_CALL ResetQueryPool(VkDevice device, VkQueryPool queryPool,
                                          std::uint32_t firstQuery, std::uint32_t queryCount) {
    if (queryPool == VK_NULL_HANDLE) {
        return;
    }
    std::lock_guard<std::mutex> lk(g_mu);
    auto* dev = reinterpret_cast<DeviceImpl*>(device);
    // Fail closed: a device that never enabled the hostQueryReset feature must
    // not drive a host vkResetQueryPool (Vulkan valid-usage violation). The API returns void, so a
    // no-op is the faithful "did nothing" -- and it never crosses the wire. The worker + mock
    // enforce the same gate as defense in depth (a stale/hostile ICD cannot reach the host call).
    if (!dev->host_query_reset_enabled) {
        icd_trace("ResetQueryPool ignored: hostQueryReset feature not enabled on this device");
        return;
    }
    try {
        vkrpc::ResetQueryPoolRequest req;
        req.device = dev->worker;
        req.query_pool = from_handle(queryPool);
        req.first_query = firstQuery;
        req.query_count = queryCount;
        (void) vkrpc::reset_query_pool(*g_rpc, next_id(), req);
    } catch (const std::exception&) {
    }
}

// Recorded query commands (ride record_command_buffer, like the other vkCmd* recordings).
// args_u64=[query_pool]; args_i64 per kind (see the worker/mock validators).
VKAPI_ATTR void VKAPI_CALL CmdResetQueryPool(VkCommandBuffer commandBuffer, VkQueryPool queryPool,
                                             std::uint32_t firstQuery, std::uint32_t queryCount) {
    auto* cb = reinterpret_cast<CommandBufferImpl*>(commandBuffer);
    vkrpc::RecordedCommand c;
    c.kind = "reset_query_pool";
    c.args_u64.push_back(from_handle(queryPool));
    c.args_i64.push_back(static_cast<long long>(firstQuery));
    c.args_i64.push_back(static_cast<long long>(queryCount));
    cb->recording.push_back(std::move(c));
}

VKAPI_ATTR void VKAPI_CALL CmdBeginQuery(VkCommandBuffer commandBuffer, VkQueryPool queryPool,
                                         std::uint32_t query, VkQueryControlFlags flags) {
    auto* cb = reinterpret_cast<CommandBufferImpl*>(commandBuffer);
    vkrpc::RecordedCommand c;
    c.kind = "begin_query";
    c.args_u64.push_back(from_handle(queryPool));
    c.args_i64.push_back(static_cast<long long>(query));
    c.args_i64.push_back(static_cast<long long>(flags));
    cb->recording.push_back(std::move(c));
}

VKAPI_ATTR void VKAPI_CALL CmdEndQuery(VkCommandBuffer commandBuffer, VkQueryPool queryPool,
                                       std::uint32_t query) {
    auto* cb = reinterpret_cast<CommandBufferImpl*>(commandBuffer);
    vkrpc::RecordedCommand c;
    c.kind = "end_query";
    c.args_u64.push_back(from_handle(queryPool));
    c.args_i64.push_back(static_cast<long long>(query));
    cb->recording.push_back(std::move(c));
}

VKAPI_ATTR void VKAPI_CALL CmdWriteTimestamp(VkCommandBuffer commandBuffer,
                                             VkPipelineStageFlagBits pipelineStage,
                                             VkQueryPool queryPool, std::uint32_t query) {
    auto* cb = reinterpret_cast<CommandBufferImpl*>(commandBuffer);
    vkrpc::RecordedCommand c;
    c.kind = "write_timestamp";
    c.args_u64.push_back(from_handle(queryPool));
    c.args_i64.push_back(static_cast<long long>(pipelineStage));
    c.args_i64.push_back(static_cast<long long>(query));
    cb->recording.push_back(std::move(c));
}

// zink's query-RESULT read path: copy query results into a host-visible VkBuffer (with WAIT), which
// the app then MAPS and reads (the relay refreshes the shadow on the following fence -- see the
// readback path). args_u64=[query_pool, dst_buffer]; args_i64=[firstQuery, queryCount, dstOffset,
// stride, flags].
VKAPI_ATTR void VKAPI_CALL CmdCopyQueryPoolResults(VkCommandBuffer commandBuffer,
                                                   VkQueryPool queryPool, std::uint32_t firstQuery,
                                                   std::uint32_t queryCount, VkBuffer dstBuffer,
                                                   VkDeviceSize dstOffset, VkDeviceSize stride,
                                                   VkQueryResultFlags flags) {
    auto* cb = reinterpret_cast<CommandBufferImpl*>(commandBuffer);
    vkrpc::RecordedCommand c;
    c.kind = "copy_query_pool_results";
    c.args_u64.push_back(from_handle(queryPool));
    c.args_u64.push_back(from_handle(dstBuffer));
    c.args_i64.push_back(static_cast<long long>(firstQuery));
    c.args_i64.push_back(static_cast<long long>(queryCount));
    c.args_i64.push_back(static_cast<long long>(dstOffset));
    c.args_i64.push_back(static_cast<long long>(stride));
    c.args_i64.push_back(static_cast<long long>(flags));
    cb->recording.push_back(std::move(c));
    // Query-result readback (surgical): the dst buffer's memory will hold GPU-written results
    // after this command executes; note JUST that memory on the CB (nothing else ever pays a
    // readback cost). Armed at vkQueueSubmit, promoted to downloadable at a PROVEN sync point
    // (record-time arming could deliver pre-copy bytes).
    std::lock_guard<std::mutex> lk(g_mu);
    const auto it = g_buffers.find(from_handle(dstBuffer));
    if (it != g_buffers.end() && it->second.memory != 0) {
        cb->readback_dsts.push_back(it->second.memory);
    }
}

VKAPI_ATTR VkResult VKAPI_CALL AllocateMemory(VkDevice device,
                                              const VkMemoryAllocateInfo* pAllocateInfo,
                                              const VkAllocationCallbacks*,
                                              VkDeviceMemory* pMemory) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto* dev = reinterpret_cast<DeviceImpl*>(device);
    try {
        const char* why = "";
        if (!vkr::icd_subset::memory_allocate_ok(pAllocateInfo, dev->buffer_device_address_enabled,
                                                 &why)) {
            std::fprintf(stderr, "vkrelay2-icd: rejecting memory allocation -- %s\n", why);
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        if (pAllocateInfo->memoryTypeIndex >= dev->mem_props.memoryTypeCount) {
            std::fprintf(stderr, "vkrelay2-icd: memory type index %u >= memoryTypeCount %u\n",
                         pAllocateInfo->memoryTypeIndex, dev->mem_props.memoryTypeCount);
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        // Memory-class gate (Option 1 Tier 1, vk13 audit): admit allocation of any type the
        // host can allocate EXCEPT PROTECTED and HOST_VISIBLE-without-HOST_COHERENT (see
        // icd_subset::memory_class_ok). Mappability is enforced later at vkMapMemory, not here: a
        // non-host-visible type (e.g. NVIDIA's propertyFlags==0 device-accessible type, or a
        // DEVICE_LOCAL-only type) is admitted and simply never mapped -- the tracker's on_map still
        // fails on it, so no shadow is ever created for it.
        {
            const VkMemoryPropertyFlags class_flags =
                dev->mem_props.memoryTypes[pAllocateInfo->memoryTypeIndex].propertyFlags;
            const char* class_why = "";
            if (!vkr::icd_subset::memory_class_ok(class_flags, &class_why)) {
                std::fprintf(stderr, "vkrelay2-icd: rejecting memory allocation -- %s\n",
                             class_why);
                return VK_ERROR_INITIALIZATION_FAILED;
            }
        }
        vkrpc::AllocateMemoryRequest req;
        req.device = dev->worker;
        req.allocation_size = static_cast<std::uint64_t>(pAllocateInfo->allocationSize);
        req.memory_type_index = static_cast<long long>(pAllocateInfo->memoryTypeIndex);
        // (bufferDeviceAddress): carry the admitted VkMemoryAllocateFlagsInfo.flags
        // (memory_allocate_ok already fail-closed everything but DEVICE_ADDRESS on an enabled
        // device) so the worker chains the same flags-info -- dropping it would break the bind
        // (VUID 03339), the reference relay's documented trap.
        for (auto* n = static_cast<const VkBaseInStructure*>(pAllocateInfo->pNext); n != nullptr;
             n = n->pNext) {
            if (n->sType == VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO) {
                req.allocate_flags |= static_cast<std::uint64_t>(
                    reinterpret_cast<const VkMemoryAllocateFlagsInfo*>(n)->flags);
            }
        }
        const vkrpc::AllocateMemoryResponse r = vkrpc::allocate_memory(*g_rpc, next_id(), req);
        if (!r.ok) {
            return fault();
        }
        *pMemory = to_handle<VkDeviceMemory>(r.memory);
        const VkMemoryPropertyFlags flags =
            dev->mem_props.memoryTypes[pAllocateInfo->memoryTypeIndex].propertyFlags;
        g_tracker.on_allocate(r.memory, static_cast<std::uint64_t>(pAllocateInfo->allocationSize),
                              pAllocateInfo->memoryTypeIndex,
                              (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0,
                              (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0);
        return VK_SUCCESS;
    } catch (const std::exception&) {
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
}

VKAPI_ATTR void VKAPI_CALL FreeMemory(VkDevice, VkDeviceMemory memory,
                                      const VkAllocationCallbacks*) {
    if (memory == VK_NULL_HANDLE) {
        return;
    }
    std::lock_guard<std::mutex> lk(g_mu);
    try {
        vkrpc::HandleRequest req;
        req.handle = from_handle(memory);
        (void) vkrpc::free_memory(*g_rpc, next_id(), req);
        std::string err;
        (void) g_tracker.on_free(from_handle(memory), err);
        // Freed memory can never be downloaded: drop it from READY and from every armed
        // submission record, then drop records left with no destinations (an
        // emptied record would only ever promote as a no-op -- erase it instead).
        g_readback_ready.erase(from_handle(memory));
        for (auto& s : g_readback_submitted) {
            s.dsts.erase(std::remove(s.dsts.begin(), s.dsts.end(), from_handle(memory)),
                         s.dsts.end());
        }
        g_readback_submitted.erase(
            std::remove_if(g_readback_submitted.begin(), g_readback_submitted.end(),
                           [](const ReadbackSubmission& s) { return s.dsts.empty(); }),
            g_readback_submitted.end());
    } catch (const std::exception&) {
    }
}

// Defined below (near WaitForFences): pull clean mapped host-visible allocations' current bytes
// from the worker into the ICD shadow. Declared here for MapMemory. Caller holds g_mu.
void download_readback_locked();

VKAPI_ATTR VkResult VKAPI_CALL MapMemory(VkDevice, VkDeviceMemory memory, VkDeviceSize offset,
                                         VkDeviceSize size, VkMemoryMapFlags flags, void** ppData) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (flags != 0) {
        return VK_ERROR_MEMORY_MAP_FAILED; // no map flags in the subset
    }
    std::string err;
    std::byte* p = g_tracker.on_map(from_handle(memory), static_cast<std::uint64_t>(offset),
                                    static_cast<std::uint64_t>(size), err);
    if (p == nullptr) {
        std::fprintf(stderr, "vkrelay2-icd: vkMapMemory failed -- %s\n", err.c_str());
        return VK_ERROR_MEMORY_MAP_FAILED;
    }
    // Query-result / GPU-write readback: an app that MAPS a host-visible buffer AFTER the GPU wrote
    // it (zink maps the query-results copy destination once the fence signals) must see the real
    // bytes, not the stale shadow. Refresh the now-mapped clean allocations from the worker. Safe
    // (clean = no pending local writes to clobber; a write-map's zero-fill is re-sent on flush).
    download_readback_locked();
    if (ppData != nullptr) {
        *ppData = p;
    }
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL UnmapMemory(VkDevice, VkDeviceMemory memory) {
    // Local only -- no RPC. The dirty bytes stay a flush-candidate and ship at the next
    // vkFlushMappedMemoryRanges / vkQueueSubmit (removes the void-call failure).
    std::lock_guard<std::mutex> lk(g_mu);
    std::string err;
    (void) g_tracker.on_unmap(from_handle(memory), err);
}

VKAPI_ATTR VkResult VKAPI_CALL FlushMappedMemoryRanges(VkDevice, std::uint32_t memoryRangeCount,
                                                       const VkMappedMemoryRange* pMemoryRanges) {
    std::lock_guard<std::mutex> lk(g_mu);
    try {
        std::vector<vkr::icd_mem::MemRange> ranges;
        for (std::uint32_t i = 0; i < memoryRangeCount; ++i) {
            ranges.push_back(
                vkr::icd_mem::MemRange{from_handle(pMemoryRanges[i].memory),
                                       static_cast<std::uint64_t>(pMemoryRanges[i].offset),
                                       static_cast<std::uint64_t>(pMemoryRanges[i].size)});
        }
        std::string err;
        const vkr::icd_mem::DirtySnapshot snap = g_tracker.snapshot_explicit(ranges, err);
        if (!err.empty()) {
            std::fprintf(stderr, "vkrelay2-icd: vkFlushMappedMemoryRanges rejected -- %s\n",
                         err.c_str());
            return VK_ERROR_MEMORY_MAP_FAILED;
        }
        if (snap.empty()) {
            return VK_SUCCESS; // nothing dirty in the named ranges
        }
        // Split into cap-fitting frames; commit each only after its ack.
        if (!ship_snapshot(snap)) {
            return fault();
        }
        return VK_SUCCESS;
    } catch (const std::exception&) {
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
}

VKAPI_ATTR VkResult VKAPI_CALL CreateCommandPool(VkDevice device,
                                                 const VkCommandPoolCreateInfo* /*pCreateInfo*/,
                                                 const VkAllocationCallbacks*,
                                                 VkCommandPool* pCommandPool) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto* dev = reinterpret_cast<DeviceImpl*>(device);
    try {
        vkrpc::CreateCommandPoolRequest req;
        req.device = dev->worker;
        req.queue_family_index = dev->queue_family; // map the app's family 0 -> worker family
        const vkrpc::CreateCommandPoolResponse r =
            vkrpc::create_command_pool(*g_rpc, next_id(), req);
        if (!r.ok) {
            return fault();
        }
        *pCommandPool = to_handle<VkCommandPool>(r.command_pool);
        return VK_SUCCESS;
    } catch (const std::exception&) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
}

VKAPI_ATTR void VKAPI_CALL DestroyCommandPool(VkDevice, VkCommandPool commandPool,
                                              const VkAllocationCallbacks*) {
    if (commandPool == VK_NULL_HANDLE) {
        return;
    }
    std::lock_guard<std::mutex> lk(g_mu);
    try {
        vkrpc::HandleRequest req;
        req.handle = from_handle(commandPool);
        (void) vkrpc::destroy_command_pool(*g_rpc, next_id(), req);
    } catch (const std::exception&) {
    }
}

VKAPI_ATTR VkResult VKAPI_CALL AllocateCommandBuffers(VkDevice device,
                                                      const VkCommandBufferAllocateInfo* pAllocInfo,
                                                      VkCommandBuffer* pCommandBuffers) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto* dev = reinterpret_cast<DeviceImpl*>(device);
    try {
        vkrpc::AllocateCommandBuffersRequest req;
        req.command_pool = from_handle(pAllocInfo->commandPool);
        req.count = pAllocInfo->commandBufferCount;
        const vkrpc::AllocateCommandBuffersResponse r =
            vkrpc::allocate_command_buffers(*g_rpc, next_id(), req);
        if (!r.ok || r.command_buffers.size() != pAllocInfo->commandBufferCount) {
            return fault();
        }
        for (std::uint32_t i = 0; i < pAllocInfo->commandBufferCount; ++i) {
            auto* cb = new CommandBufferImpl{};
            set_loader_magic_value(cb);
            cb->worker = r.command_buffers[i];
            cb->vk13_device = dev->vk13_device; // Vulkan 1.3 support: gates the copy2 wrappers
            cb->worker_core_indirect_draw = dev->worker_core_indirect_draw;
            cb->worker_core_indirect_draw_scalar_payload =
                dev->worker_core_indirect_draw_scalar_payload;
            cb->multi_draw_indirect_enabled = dev->multi_draw_indirect_enabled;
            cb->worker_core_indirect_draw_count = dev->worker_core_indirect_draw_count;
            cb->draw_indirect_count_enabled = dev->draw_indirect_count_enabled;
            pCommandBuffers[i] = reinterpret_cast<VkCommandBuffer>(cb);
        }
        return VK_SUCCESS;
    } catch (const std::exception&) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
}

VKAPI_ATTR void VKAPI_CALL FreeCommandBuffers(VkDevice, VkCommandPool commandPool,
                                              std::uint32_t count,
                                              const VkCommandBuffer* pCommandBuffers) {
    std::lock_guard<std::mutex> lk(g_mu);
    try {
        vkrpc::FreeCommandBuffersRequest req;
        req.command_pool = from_handle(commandPool);
        for (std::uint32_t i = 0; i < count; ++i) {
            auto* cb = reinterpret_cast<CommandBufferImpl*>(pCommandBuffers[i]);
            req.command_buffers.push_back(cb->worker);
        }
        (void) vkrpc::free_command_buffers(*g_rpc, next_id(), req);
    } catch (const std::exception&) {
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        delete reinterpret_cast<CommandBufferImpl*>(pCommandBuffers[i]);
    }
}

// (GL/zink): command-pool / command-buffer reset. The worker re-begins (implicitly resets)
// each command buffer at record_command_buffer time (the pool carries RESET_COMMAND_BUFFER_BIT), so
// a local success is sufficient -- vkBeginCommandBuffer already clears the ICD-side recording. A
// real-frame app that resets a pool mid-flight would want this forwarded; revisit if needed.
VKAPI_ATTR VkResult VKAPI_CALL ResetCommandPool(VkDevice, VkCommandPool, VkCommandPoolResetFlags) {
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL ResetCommandBuffer(VkCommandBuffer commandBuffer,
                                                  VkCommandBufferResetFlags) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto* cb = reinterpret_cast<CommandBufferImpl*>(commandBuffer);
    cb->recording.clear();
    cb->local_invalid = false;
    cb->invalid_reason = "";
    cb->readback_dsts.clear();
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL CreateFence(VkDevice device, const VkFenceCreateInfo* pCreateInfo,
                                           const VkAllocationCallbacks*, VkFence* pFence) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto* dev = reinterpret_cast<DeviceImpl*>(device);
    try {
        vkrpc::CreateFenceRequest req;
        req.device = dev->worker;
        // Carry VK_FENCE_CREATE_SIGNALED_BIT so a created-signaled fence is honestly signaled on
        // the worker (real frame loops wait on their per-frame fences before first use; dropping it
        // deadlocks the first wait).
        req.signaled =
            pCreateInfo != nullptr && (pCreateInfo->flags & VK_FENCE_CREATE_SIGNALED_BIT) != 0;
        const vkrpc::CreateFenceResponse r = vkrpc::create_fence(*g_rpc, next_id(), req);
        if (!r.ok) {
            return fault();
        }
        *pFence = to_handle<VkFence>(r.fence);
        return VK_SUCCESS;
    } catch (const std::exception&) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
}

VKAPI_ATTR void VKAPI_CALL DestroyFence(VkDevice, VkFence fence, const VkAllocationCallbacks*) {
    if (fence == VK_NULL_HANDLE) {
        return;
    }
    std::lock_guard<std::mutex> lk(g_mu);
    try {
        vkrpc::HandleRequest req;
        req.handle = from_handle(fence);
        (void) vkrpc::destroy_fence(*g_rpc, next_id(), req);
    } catch (const std::exception&) {
    }
}

VKAPI_ATTR VkResult VKAPI_CALL ResetFences(VkDevice, std::uint32_t fenceCount,
                                           const VkFence* pFences) {
    std::lock_guard<std::mutex> lk(g_mu);
    try {
        vkrpc::ResetFencesRequest req;
        for (std::uint32_t i = 0; i < fenceCount; ++i) {
            req.fences.push_back(from_handle(pFences[i]));
        }
        const vkrpc::StatusResponse r = vkrpc::reset_fences(*g_rpc, next_id(), req);
        return r.ok ? VK_SUCCESS : fault();
    } catch (const std::exception&) {
        return VK_ERROR_DEVICE_LOST;
    }
}

// Query-result / GPU-write readback: after a fence/idle wait, GPU writes to host-visible buffers
// (a vkCmdCopyQueryPoolResults destination, transform-feedback output, an SSBO) are complete. Pull
// the CLEAN mapped host-visible allocations' current bytes from the worker into the ICD shadow so
// the app's next map/read sees them. "Clean" (no pending local writes) means the download cannot
// clobber un-flushed app data. Size-capped (query/readback buffers are small; a streaming buffer
// the app rewrites is dirty -> excluded). Caller holds g_mu. Best-effort: a failed RPC leaves the
// shadow as-is (the app reads stale, not corrupt -- same as before this path existed).
// Promotion: a submission record's dsts become READY (downloadable) only on
// a PROVEN completion of that submission -- its fence waited to success, one of its timeline
// signal values covered by a successful wait, or a real idle wait. Callers hold g_mu.
void promote_readbacks_all_locked() {
    for (const ReadbackSubmission& s : g_readback_submitted) {
        g_readback_ready.insert(s.dsts.begin(), s.dsts.end());
    }
    g_readback_submitted.clear();
}
void promote_readbacks_by_fence_locked(std::uint64_t fence) {
    for (auto it = g_readback_submitted.begin(); it != g_readback_submitted.end();) {
        if (std::find(it->fences.begin(), it->fences.end(), fence) != it->fences.end()) {
            g_readback_ready.insert(it->dsts.begin(), it->dsts.end());
            it = g_readback_submitted.erase(it);
        } else {
            ++it;
        }
    }
}
// A successful wait for (semaphore >= value) proves every batch whose signal on that semaphore is
// <= value (timeline counters are monotonic).
void promote_readbacks_by_semaphore_locked(std::uint64_t semaphore, std::uint64_t value) {
    for (auto it = g_readback_submitted.begin(); it != g_readback_submitted.end();) {
        bool proven = false;
        for (const auto& sig : it->signals) {
            if (sig.first == semaphore && sig.second <= value) {
                proven = true;
                break;
            }
        }
        if (proven) {
            g_readback_ready.insert(it->dsts.begin(), it->dsts.end());
            it = g_readback_submitted.erase(it);
        } else {
            ++it;
        }
    }
}

void download_readback_locked() {
    if (g_readback_ready.empty()) {
        return;
    }
    // Only READY (proven-complete) allocations that are NOW mapped + clean (no pending local
    // writes to clobber) are downloaded; a still-unmapped one stays ready until the app maps it.
    // Everything else -- streaming upload buffers, textures -- is never flagged, so it pays
    // nothing.
    std::vector<std::pair<std::uint64_t, std::uint64_t>> cands; // (memory, size)
    for (const std::uint64_t mem : g_readback_ready) {
        if (g_tracker.mapped(mem) && g_tracker.readback_clean(mem)) {
            cands.emplace_back(mem, g_tracker.allocation_size(mem));
        }
    }
    if (cands.empty()) {
        return;
    }
    try {
        vkrpc::ReadMemoryRangesRequest req;
        req.raw_response = g_raw_readback; // raw reply when advertised
        for (const auto& c : cands) {
            vkrpc::MemoryUpload u;
            u.memory = c.first;
            u.ranges.push_back({0, c.second});
            req.reads.push_back(std::move(u));
        }
        const vkrpc::ReadMemoryRangesResponse r = vkrpc::read_memory_ranges(*g_rpc, next_id(), req);
        if (!r.ok) {
            return;
        }
        std::size_t cursor = 0;
        for (const auto& c : cands) {
            const auto size = static_cast<std::size_t>(c.second);
            if (cursor + size > r.payload.size()) {
                break;
            }
            g_tracker.apply_readback(c.first, r.payload.substr(cursor, size));
            cursor += size;
            g_readback_ready.erase(c.first); // delivered to the app's shadow -> done
        }
    } catch (const std::exception&) {
        // best-effort
    }
}

VKAPI_ATTR VkResult VKAPI_CALL WaitForFences(VkDevice, std::uint32_t fenceCount,
                                             const VkFence* pFences, VkBool32 waitAll,
                                             std::uint64_t timeout) {
    std::lock_guard<std::mutex> lk(g_mu);
    try {
        vkrpc::WaitForFencesRequest req;
        for (std::uint32_t i = 0; i < fenceCount; ++i) {
            req.fences.push_back(from_handle(pFences[i]));
        }
        req.wait_all = (waitAll == VK_TRUE);
        req.timeout = timeout;
        const vkrpc::WaitForFencesResponse r = vkrpc::wait_for_fences(*g_rpc, next_id(), req);
        if (!r.ok) {
            return fault();
        }
        if (r.result == 0) { // VK_SUCCESS
            // A proven completion promotes exactly the submissions this wait covers:
            // with wait-all (or a single fence) every waited fence is proven signaled;
            // a satisfied wait-ANY over several fences proves none in particular, so it promotes
            // nothing (the readback stays armed for a later proven sync).
            if (req.wait_all || req.fences.size() == 1) {
                for (const std::uint64_t f : req.fences) {
                    promote_readbacks_by_fence_locked(f);
                }
            }
            download_readback_locked();
        }
        return static_cast<VkResult>(r.result); // VK_SUCCESS or VK_TIMEOUT
    } catch (const std::exception&) {
        return VK_ERROR_DEVICE_LOST;
    }
}

// the fence-family completeness fix. VK_SUCCESS (signaled) / VK_NOT_READY (unsignaled)
// are both honest returns forwarded verbatim (the WaitForFences idiom); only a transport failure
// faults. The device param is ignored -- the worker resolves the fence's device from the leaf (as
// ResetFences / WaitForFences do).
VKAPI_ATTR VkResult VKAPI_CALL GetFenceStatus(VkDevice, VkFence fence) {
    std::lock_guard<std::mutex> lk(g_mu);
    try {
        vkrpc::HandleRequest req;
        req.handle = from_handle(fence);
        const vkrpc::GetFenceStatusResponse r = vkrpc::get_fence_status(*g_rpc, next_id(), req);
        if (!r.ok) {
            return fault();
        }
        return static_cast<VkResult>(r.result); // VK_SUCCESS or VK_NOT_READY
    } catch (const std::exception&) {
        return VK_ERROR_DEVICE_LOST;
    }
}

// the VkEvent object model (core Vulkan 1.0). create/destroy mirror the fence leaf; the
// host ops get/set/reset carry just the event handle (the worker resolves its device).
VKAPI_ATTR VkResult VKAPI_CALL CreateEvent(VkDevice device, const VkEventCreateInfo*,
                                           const VkAllocationCallbacks*, VkEvent* pEvent) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto* dev = reinterpret_cast<DeviceImpl*>(device);
    try {
        vkrpc::CreateEventRequest req;
        req.device = dev->worker;
        const vkrpc::CreateEventResponse r = vkrpc::create_event(*g_rpc, next_id(), req);
        if (!r.ok) {
            return fault();
        }
        *pEvent = to_handle<VkEvent>(r.event);
        return VK_SUCCESS;
    } catch (const std::exception&) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
}

VKAPI_ATTR void VKAPI_CALL DestroyEvent(VkDevice, VkEvent event, const VkAllocationCallbacks*) {
    if (event == VK_NULL_HANDLE) {
        return;
    }
    std::lock_guard<std::mutex> lk(g_mu);
    try {
        vkrpc::HandleRequest req;
        req.handle = from_handle(event);
        (void) vkrpc::destroy_event(*g_rpc, next_id(), req);
    } catch (const std::exception&) {
    }
}

VKAPI_ATTR VkResult VKAPI_CALL GetEventStatus(VkDevice, VkEvent event) {
    std::lock_guard<std::mutex> lk(g_mu);
    try {
        vkrpc::HandleRequest req;
        req.handle = from_handle(event);
        const vkrpc::GetEventStatusResponse r = vkrpc::get_event_status(*g_rpc, next_id(), req);
        if (!r.ok) {
            return fault();
        }
        return static_cast<VkResult>(r.result); // VK_EVENT_SET or VK_EVENT_RESET
    } catch (const std::exception&) {
        return VK_ERROR_DEVICE_LOST;
    }
}

VKAPI_ATTR VkResult VKAPI_CALL SetEvent(VkDevice, VkEvent event) {
    std::lock_guard<std::mutex> lk(g_mu);
    try {
        vkrpc::HandleRequest req;
        req.handle = from_handle(event);
        const vkrpc::StatusResponse r = vkrpc::set_event(*g_rpc, next_id(), req);
        return r.ok ? VK_SUCCESS : fault();
    } catch (const std::exception&) {
        return VK_ERROR_DEVICE_LOST;
    }
}

VKAPI_ATTR VkResult VKAPI_CALL ResetEvent(VkDevice, VkEvent event) {
    std::lock_guard<std::mutex> lk(g_mu);
    try {
        vkrpc::HandleRequest req;
        req.handle = from_handle(event);
        const vkrpc::StatusResponse r = vkrpc::reset_event(*g_rpc, next_id(), req);
        return r.ok ? VK_SUCCESS : fault();
    } catch (const std::exception&) {
        return VK_ERROR_DEVICE_LOST;
    }
}

VKAPI_ATTR VkResult VKAPI_CALL CreateSemaphore(VkDevice device,
                                               const VkSemaphoreCreateInfo* pCreateInfo,
                                               const VkAllocationCallbacks*,
                                               VkSemaphore* pSemaphore) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto* dev = reinterpret_cast<DeviceImpl*>(device);
    try {
        vkrpc::CreateSemaphoreRequest req;
        req.device = dev->worker;
        // (GL/zink): a VkSemaphoreTypeCreateInfo TIMELINE in the pNext chain makes this a
        // timeline semaphore (zink uses them for batch sync). Carry the type + initial value.
        if (pCreateInfo != nullptr) {
            for (auto* n = static_cast<const VkBaseInStructure*>(pCreateInfo->pNext); n != nullptr;
                 n = n->pNext) {
                if (n->sType == VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO) {
                    const auto* sti = reinterpret_cast<const VkSemaphoreTypeCreateInfo*>(n);
                    if (sti->semaphoreType == VK_SEMAPHORE_TYPE_TIMELINE) {
                        req.semaphore_type = 1;
                        req.initial_value = sti->initialValue;
                    }
                }
            }
        }
        const vkrpc::CreateSemaphoreResponse r = vkrpc::create_semaphore(*g_rpc, next_id(), req);
        if (!r.ok) {
            return fault();
        }
        *pSemaphore = to_handle<VkSemaphore>(r.semaphore);
        return VK_SUCCESS;
    } catch (const std::exception&) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
}

VKAPI_ATTR void VKAPI_CALL DestroySemaphore(VkDevice, VkSemaphore semaphore,
                                            const VkAllocationCallbacks*) {
    if (semaphore == VK_NULL_HANDLE) {
        return;
    }
    std::lock_guard<std::mutex> lk(g_mu);
    try {
        vkrpc::HandleRequest req;
        req.handle = from_handle(semaphore);
        (void) vkrpc::destroy_semaphore(*g_rpc, next_id(), req);
    } catch (const std::exception&) {
    }
}

// (GL/zink): host-side timeline-semaphore ops (zink syncs its batches with these).
VKAPI_ATTR VkResult VKAPI_CALL WaitSemaphores(VkDevice device, const VkSemaphoreWaitInfo* pWaitInfo,
                                              std::uint64_t timeout) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto* dev = reinterpret_cast<DeviceImpl*>(device);
    if (pWaitInfo == nullptr) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    try {
        vkrpc::WaitSemaphoresRequest req;
        req.device = dev->worker;
        req.timeout = timeout;
        req.wait_any = (pWaitInfo->flags & VK_SEMAPHORE_WAIT_ANY_BIT) != 0 ? 1 : 0;
        for (std::uint32_t i = 0; i < pWaitInfo->semaphoreCount; ++i) {
            req.semaphores.push_back(from_handle(pWaitInfo->pSemaphores[i]));
            req.values.push_back(pWaitInfo->pValues != nullptr ? pWaitInfo->pValues[i] : 0);
        }
        const vkrpc::WaitSemaphoresResponse r = vkrpc::wait_semaphores(*g_rpc, next_id(), req);
        if (!r.ok) {
            return fault();
        }
        // (offscreen readback): zink syncs its glReadPixels staging with a TIMELINE wait, not a
        // fence (the fence hook is in WaitForFences). A successful wait PROVES every waited
        // (semaphore, value) pair reached its value (wait-all, or a single-pair wait-any) -- which
        // proves complete every submitted batch whose timeline signal on that semaphore is <= the
        // waited value (promote exactly what the sync covers, then download). A
        // satisfied multi-pair wait-ANY proves no pair in particular and promotes nothing.
        if (r.result == 0) { // VK_SUCCESS
            if (req.wait_any == 0 || req.semaphores.size() == 1) {
                for (std::size_t i = 0; i < req.semaphores.size(); ++i) {
                    promote_readbacks_by_semaphore_locked(req.semaphores[i], req.values[i]);
                }
            }
            download_readback_locked();
        }
        return static_cast<VkResult>(r.result); // honest VK_SUCCESS / VK_TIMEOUT
    } catch (const std::exception&) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
}

VKAPI_ATTR VkResult VKAPI_CALL SignalSemaphore(VkDevice device,
                                               const VkSemaphoreSignalInfo* pSignalInfo) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto* dev = reinterpret_cast<DeviceImpl*>(device);
    if (pSignalInfo == nullptr) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    try {
        vkrpc::SignalSemaphoreRequest req;
        req.device = dev->worker;
        req.semaphore = from_handle(pSignalInfo->semaphore);
        req.value = pSignalInfo->value;
        const vkrpc::StatusResponse r = vkrpc::signal_semaphore(*g_rpc, next_id(), req);
        return r.ok ? VK_SUCCESS : fault();
    } catch (const std::exception&) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
}

VKAPI_ATTR VkResult VKAPI_CALL GetSemaphoreCounterValue(VkDevice device, VkSemaphore semaphore,
                                                        std::uint64_t* pValue) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto* dev = reinterpret_cast<DeviceImpl*>(device);
    if (pValue == nullptr) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    try {
        vkrpc::GetSemaphoreCounterValueRequest req;
        req.device = dev->worker;
        req.semaphore = from_handle(semaphore);
        const vkrpc::GetSemaphoreCounterValueResponse r =
            vkrpc::get_semaphore_counter_value(*g_rpc, next_id(), req);
        if (!r.ok) {
            return fault();
        }
        if (r.result != VK_SUCCESS) {
            return static_cast<VkResult>(r.result);
        }
        *pValue = r.value;
        return VK_SUCCESS;
    } catch (const std::exception&) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
}

// --- Command recording (client-side stream; flushed at vkEndCommandBuffer) ---
VKAPI_ATTR VkResult VKAPI_CALL BeginCommandBuffer(VkCommandBuffer commandBuffer,
                                                  const VkCommandBufferBeginInfo* pBeginInfo) {
    auto* cb = reinterpret_cast<CommandBufferImpl*>(commandBuffer);
    cb->recording.clear();
    cb->local_invalid = false;
    cb->invalid_reason = "";
    cb->readback_dsts.clear();
    cb->one_time_submit = pBeginInfo != nullptr &&
                          (pBeginInfo->flags & VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT) != 0;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL CmdPipelineBarrier(
    VkCommandBuffer commandBuffer, VkPipelineStageFlags srcStageMask,
    VkPipelineStageFlags dstStageMask, VkDependencyFlags /*dependencyFlags*/,
    std::uint32_t memoryBarrierCount, const VkMemoryBarrier* pMemoryBarriers,
    std::uint32_t bufferMemoryBarrierCount, const VkBufferMemoryBarrier* pBufferBarriers,
    std::uint32_t imageMemoryBarrierCount, const VkImageMemoryBarrier* pImageBarriers) {
    auto* cb = reinterpret_cast<CommandBufferImpl*>(commandBuffer);
    // Compute: global and buffer barriers were silently DROPPED
    // here (image-only) -- a faithfulness hole. One recorded command per barrier of every
    // class now, in call order.
    for (std::uint32_t i = 0; i < memoryBarrierCount; ++i) {
        const VkMemoryBarrier& b = pMemoryBarriers[i];
        if (b.pNext != nullptr) {
            cb->local_invalid = true;
            cb->invalid_reason = "memory barrier pNext not supported";
            return;
        }
        vkrpc::RecordedCommand c;
        c.kind = "memory_barrier";
        c.src_stage = static_cast<long long>(srcStageMask);
        c.dst_stage = static_cast<long long>(dstStageMask);
        c.src_access = static_cast<long long>(b.srcAccessMask);
        c.dst_access = static_cast<long long>(b.dstAccessMask);
        cb->recording.push_back(std::move(c));
    }
    for (std::uint32_t i = 0; i < bufferMemoryBarrierCount; ++i) {
        const VkBufferMemoryBarrier& b = pBufferBarriers[i];
        // Single queue family: an ownership transfer (either index not IGNORED) is a named
        // fail-closed reject, never a silent drop.
        if (b.pNext != nullptr || b.srcQueueFamilyIndex != VK_QUEUE_FAMILY_IGNORED ||
            b.dstQueueFamilyIndex != VK_QUEUE_FAMILY_IGNORED) {
            cb->local_invalid = true;
            cb->invalid_reason = "buffer barrier pNext/queue-family transfer not supported";
            return;
        }
        vkrpc::RecordedCommand c;
        c.kind = "buffer_memory_barrier";
        c.src_buffer = from_handle(b.buffer);
        c.src_stage = static_cast<long long>(srcStageMask);
        c.dst_stage = static_cast<long long>(dstStageMask);
        c.src_access = static_cast<long long>(b.srcAccessMask);
        c.dst_access = static_cast<long long>(b.dstAccessMask);
        c.args_u64.push_back(static_cast<std::uint64_t>(b.offset));
        c.args_u64.push_back(static_cast<std::uint64_t>(b.size)); // WHOLE_SIZE rides as ~0
        cb->recording.push_back(std::move(c));
    }
    for (std::uint32_t i = 0; i < imageMemoryBarrierCount; ++i) {
        const VkImageMemoryBarrier& b = pImageBarriers[i];
        vkrpc::RecordedCommand c;
        c.kind = "pipeline_barrier";
        c.image = from_handle(b.image);
        c.old_layout = normalize_unadvertised_feedback_layout(b.oldLayout);
        c.new_layout = normalize_unadvertised_feedback_layout(b.newLayout);
        c.src_stage = static_cast<long long>(srcStageMask);
        c.dst_stage = static_cast<long long>(dstStageMask);
        c.src_access = static_cast<long long>(b.srcAccessMask);
        c.dst_access = static_cast<long long>(b.dstAccessMask);
        c.aspect = static_cast<int>(b.subresourceRange.aspectMask);
        // Carry the mip/layer range honestly: the worker validates an app-image
        // (texture) barrier against the single-subresource subset. Queue-family indices are IGNORED
        // (not carried). VK_REMAINING_*_LEVELS/LAYERS exceed INT_MAX -> the wide -1 sentinel marks
        // "not the single-subresource shape", which the worker rejects for an app image.
        auto clamp_range = [](std::uint32_t v) {
            return v <= static_cast<std::uint32_t>(INT_MAX) ? static_cast<int>(v) : -1;
        };
        c.barrier_base_mip = clamp_range(b.subresourceRange.baseMipLevel);
        c.barrier_level_count = clamp_range(b.subresourceRange.levelCount);
        c.barrier_base_layer = clamp_range(b.subresourceRange.baseArrayLayer);
        c.barrier_layer_count = clamp_range(b.subresourceRange.layerCount);
        cb->recording.push_back(std::move(c));
    }
}

// sync1 command events on the VkEvent object. set_event / reset_event carry the event
// handle (args_u64[0]) + the 32-bit stageMask (src_stage). The worker translates the event handle
// and replays vkCmdSetEvent / vkCmdResetEvent.
VKAPI_ATTR void VKAPI_CALL CmdSetEvent(VkCommandBuffer commandBuffer, VkEvent event,
                                       VkPipelineStageFlags stageMask) {
    auto* cb = reinterpret_cast<CommandBufferImpl*>(commandBuffer);
    vkrpc::RecordedCommand c;
    c.kind = "set_event";
    c.src_stage = static_cast<long long>(stageMask);
    c.args_u64.push_back(from_handle(event));
    cb->recording.push_back(std::move(c));
}

VKAPI_ATTR void VKAPI_CALL CmdResetEvent(VkCommandBuffer commandBuffer, VkEvent event,
                                         VkPipelineStageFlags stageMask) {
    auto* cb = reinterpret_cast<CommandBufferImpl*>(commandBuffer);
    vkrpc::RecordedCommand c;
    c.kind = "reset_event";
    c.src_stage = static_cast<long long>(stageMask);
    c.args_u64.push_back(from_handle(event));
    cb->recording.push_back(std::move(c));
}

// vkCmdWaitEvents is atomic (it waits on a SET of events and carries the sync1 barrier
// arrays), so unlike vkCmdPipelineBarrier it is ONE RecordedCommand -- a single-array fixed-slot
// flatten in args_u64: header [eventCount, memCount, bufCount, imgCount], then events, then per
// memory barrier [srcAccess, dstAccess], per buffer [srcAccess, dstAccess, buffer, offset, size],
// per image [srcAccess, dstAccess, oldLayout, newLayout, image, aspect, baseMip, levelCount,
// baseLayer, layerCount]. src_stage/dst_stage carry the global stage masks. VK_REMAINING_* ride as
// 0xFFFFFFFF and cast back at replay. pNext / queue-family ownership transfers are fail-closed
// (named reject, never silently dropped), matching the sync1 pipeline-barrier policy.
VKAPI_ATTR void VKAPI_CALL
CmdWaitEvents(VkCommandBuffer commandBuffer, std::uint32_t eventCount, const VkEvent* pEvents,
              VkPipelineStageFlags srcStageMask, VkPipelineStageFlags dstStageMask,
              std::uint32_t memoryBarrierCount, const VkMemoryBarrier* pMemoryBarriers,
              std::uint32_t bufferMemoryBarrierCount, const VkBufferMemoryBarrier* pBufferBarriers,
              std::uint32_t imageMemoryBarrierCount, const VkImageMemoryBarrier* pImageBarriers) {
    auto* cb = reinterpret_cast<CommandBufferImpl*>(commandBuffer);
    if (eventCount > vkrpc::kMaxWaitEventsBarriers ||
        memoryBarrierCount > vkrpc::kMaxWaitEventsBarriers ||
        bufferMemoryBarrierCount > vkrpc::kMaxWaitEventsBarriers ||
        imageMemoryBarrierCount > vkrpc::kMaxWaitEventsBarriers) {
        cb->local_invalid = true;
        cb->invalid_reason = "wait_events count exceeds the supported cap";
        return;
    }
    vkrpc::RecordedCommand c;
    c.kind = "wait_events";
    c.src_stage = static_cast<long long>(srcStageMask);
    c.dst_stage = static_cast<long long>(dstStageMask);
    c.args_u64.push_back(eventCount);
    c.args_u64.push_back(memoryBarrierCount);
    c.args_u64.push_back(bufferMemoryBarrierCount);
    c.args_u64.push_back(imageMemoryBarrierCount);
    for (std::uint32_t i = 0; i < eventCount; ++i) {
        c.args_u64.push_back(from_handle(pEvents[i]));
    }
    for (std::uint32_t i = 0; i < memoryBarrierCount; ++i) {
        const VkMemoryBarrier& b = pMemoryBarriers[i];
        if (b.pNext != nullptr) {
            cb->local_invalid = true;
            cb->invalid_reason = "wait_events memory barrier pNext not supported";
            return;
        }
        c.args_u64.push_back(b.srcAccessMask);
        c.args_u64.push_back(b.dstAccessMask);
    }
    for (std::uint32_t i = 0; i < bufferMemoryBarrierCount; ++i) {
        const VkBufferMemoryBarrier& b = pBufferBarriers[i];
        if (b.pNext != nullptr || b.srcQueueFamilyIndex != VK_QUEUE_FAMILY_IGNORED ||
            b.dstQueueFamilyIndex != VK_QUEUE_FAMILY_IGNORED) {
            cb->local_invalid = true;
            cb->invalid_reason =
                "wait_events buffer barrier pNext/queue-family transfer not supported";
            return;
        }
        c.args_u64.push_back(b.srcAccessMask);
        c.args_u64.push_back(b.dstAccessMask);
        c.args_u64.push_back(from_handle(b.buffer));
        c.args_u64.push_back(static_cast<std::uint64_t>(b.offset));
        c.args_u64.push_back(static_cast<std::uint64_t>(b.size)); // WHOLE_SIZE rides as ~0
    }
    for (std::uint32_t i = 0; i < imageMemoryBarrierCount; ++i) {
        const VkImageMemoryBarrier& b = pImageBarriers[i];
        if (b.pNext != nullptr || b.srcQueueFamilyIndex != VK_QUEUE_FAMILY_IGNORED ||
            b.dstQueueFamilyIndex != VK_QUEUE_FAMILY_IGNORED) {
            cb->local_invalid = true;
            cb->invalid_reason =
                "wait_events image barrier pNext/queue-family transfer not supported";
            return;
        }
        c.args_u64.push_back(b.srcAccessMask);
        c.args_u64.push_back(b.dstAccessMask);
        c.args_u64.push_back(static_cast<std::uint64_t>(b.oldLayout));
        c.args_u64.push_back(static_cast<std::uint64_t>(b.newLayout));
        c.args_u64.push_back(from_handle(b.image));
        c.args_u64.push_back(b.subresourceRange.aspectMask);
        c.args_u64.push_back(b.subresourceRange.baseMipLevel);
        c.args_u64.push_back(b.subresourceRange.levelCount); // VK_REMAINING_* rides as 0xFFFFFFFF
        c.args_u64.push_back(b.subresourceRange.baseArrayLayer);
        c.args_u64.push_back(b.subresourceRange.layerCount);
    }
    cb->recording.push_back(std::move(c));
}

// flatten a VkDependencyInfo into the typed DependencyInfo2, FAIL-CLOSED on every
// unsupported field (pNext, unsupported dependencyFlags, queue-family ownership transfer) and every
// null-with-nonzero-count / over-cap array (null/count + cap guards). Guest handles
// carried as-is; 64-bit masks carried full-width. Reused by the three sync2 dependency-carrying
// commands. Returns false + sets `reason` (never dereferences past a rejected pointer).
bool flatten_dependency_info2(const VkDependencyInfo* dep, vkrpc::DependencyInfo2& out,
                              const char*& reason) {
    if (dep == nullptr) {
        reason = "null VkDependencyInfo";
        return false;
    }
    if (dep->pNext != nullptr) {
        reason = "VkDependencyInfo pNext not supported";
        return false;
    }
    if ((dep->dependencyFlags & ~static_cast<VkDependencyFlags>(VK_DEPENDENCY_BY_REGION_BIT)) !=
        0) {
        reason = "unsupported dependencyFlags (only BY_REGION)";
        return false;
    }
    if (dep->memoryBarrierCount > vkrpc::kMaxSync2BarriersPerDep ||
        dep->bufferMemoryBarrierCount > vkrpc::kMaxSync2BarriersPerDep ||
        dep->imageMemoryBarrierCount > vkrpc::kMaxSync2BarriersPerDep) {
        reason = "sync2 barrier count exceeds the supported cap";
        return false;
    }
    if ((dep->memoryBarrierCount != 0 && dep->pMemoryBarriers == nullptr) ||
        (dep->bufferMemoryBarrierCount != 0 && dep->pBufferMemoryBarriers == nullptr) ||
        (dep->imageMemoryBarrierCount != 0 && dep->pImageMemoryBarriers == nullptr)) {
        reason = "null sync2 barrier array with a nonzero count";
        return false;
    }
    // Both VK_QUEUE_FAMILY_IGNORED or both the single exposed guest family (0); no ownership
    // transfer (normalized to IGNORED at replay).
    const auto qfi_ok = [](std::uint32_t s, std::uint32_t d) {
        return (s == VK_QUEUE_FAMILY_IGNORED && d == VK_QUEUE_FAMILY_IGNORED) || (s == 0 && d == 0);
    };
    out.dependency_flags = dep->dependencyFlags;
    for (std::uint32_t i = 0; i < dep->memoryBarrierCount; ++i) {
        const VkMemoryBarrier2& b = dep->pMemoryBarriers[i];
        if (b.pNext != nullptr) {
            reason = "VkMemoryBarrier2 pNext not supported";
            return false;
        }
        vkrpc::MemoryBarrier2 m;
        m.src_stage = b.srcStageMask;
        m.src_access = b.srcAccessMask;
        m.dst_stage = b.dstStageMask;
        m.dst_access = b.dstAccessMask;
        out.memory.push_back(m);
    }
    for (std::uint32_t i = 0; i < dep->bufferMemoryBarrierCount; ++i) {
        const VkBufferMemoryBarrier2& b = dep->pBufferMemoryBarriers[i];
        if (b.pNext != nullptr) {
            reason = "VkBufferMemoryBarrier2 pNext not supported";
            return false;
        }
        if (!qfi_ok(b.srcQueueFamilyIndex, b.dstQueueFamilyIndex)) {
            reason = "buffer barrier queue-family ownership transfer not supported";
            return false;
        }
        vkrpc::BufferMemoryBarrier2 bb;
        bb.src_stage = b.srcStageMask;
        bb.src_access = b.srcAccessMask;
        bb.dst_stage = b.dstStageMask;
        bb.dst_access = b.dstAccessMask;
        bb.src_queue_family = static_cast<long long>(b.srcQueueFamilyIndex);
        bb.dst_queue_family = static_cast<long long>(b.dstQueueFamilyIndex);
        bb.buffer = from_handle(b.buffer);
        bb.offset = static_cast<std::uint64_t>(b.offset);
        bb.size = static_cast<std::uint64_t>(b.size);
        out.buffer.push_back(bb);
    }
    for (std::uint32_t i = 0; i < dep->imageMemoryBarrierCount; ++i) {
        const VkImageMemoryBarrier2& b = dep->pImageMemoryBarriers[i];
        if (b.pNext != nullptr) {
            reason = "VkImageMemoryBarrier2 pNext not supported";
            return false;
        }
        if (!qfi_ok(b.srcQueueFamilyIndex, b.dstQueueFamilyIndex)) {
            reason = "image barrier queue-family ownership transfer not supported";
            return false;
        }
        vkrpc::ImageMemoryBarrier2 im;
        im.src_stage = b.srcStageMask;
        im.src_access = b.srcAccessMask;
        im.dst_stage = b.dstStageMask;
        im.dst_access = b.dstAccessMask;
        im.old_layout = normalize_unadvertised_feedback_layout(b.oldLayout);
        im.new_layout = normalize_unadvertised_feedback_layout(b.newLayout);
        im.src_queue_family = static_cast<long long>(b.srcQueueFamilyIndex);
        im.dst_queue_family = static_cast<long long>(b.dstQueueFamilyIndex);
        im.image = from_handle(b.image);
        im.aspect = static_cast<long long>(b.subresourceRange.aspectMask);
        im.base_mip = static_cast<long long>(b.subresourceRange.baseMipLevel);
        im.level_count = static_cast<long long>(b.subresourceRange.levelCount);
        im.base_layer = static_cast<long long>(b.subresourceRange.baseArrayLayer);
        im.layer_count = static_cast<long long>(b.subresourceRange.layerCount);
        out.image.push_back(im);
    }
    return true;
}

// vkCmdPipelineBarrier2 -- one DependencyInfo2 (deps2.size==1), no events.
VKAPI_ATTR void VKAPI_CALL CmdPipelineBarrier2(VkCommandBuffer commandBuffer,
                                               const VkDependencyInfo* pDependencyInfo) {
    auto* cb = reinterpret_cast<CommandBufferImpl*>(commandBuffer);
    vkrpc::RecordedCommand c;
    c.kind = "pipeline_barrier2";
    vkrpc::DependencyInfo2 dep;
    const char* reason = "";
    if (!flatten_dependency_info2(pDependencyInfo, dep, reason)) {
        cb->local_invalid = true;
        cb->invalid_reason = reason;
        return;
    }
    c.deps2.push_back(std::move(dep));
    cb->recording.push_back(std::move(c));
}

// vkCmdWriteTimestamp2 -- args_u64=[queryPool, stageMask64], args_i64=[query].
// The 64-bit stage rides u64 (never args_i64's 32-bit sync1 slot); validated single-bit at replay.
VKAPI_ATTR void VKAPI_CALL CmdWriteTimestamp2(VkCommandBuffer commandBuffer,
                                              VkPipelineStageFlags2 stage, VkQueryPool queryPool,
                                              std::uint32_t query) {
    auto* cb = reinterpret_cast<CommandBufferImpl*>(commandBuffer);
    vkrpc::RecordedCommand c;
    c.kind = "write_timestamp2";
    c.args_u64.push_back(from_handle(queryPool));
    c.args_u64.push_back(static_cast<std::uint64_t>(stage));
    c.args_i64.push_back(static_cast<long long>(query));
    cb->recording.push_back(std::move(c));
}

// vkCmdSetEvent2 -- event + one DependencyInfo2. args_u64=[event],
// deps2.size()==1.
VKAPI_ATTR void VKAPI_CALL CmdSetEvent2(VkCommandBuffer commandBuffer, VkEvent event,
                                        const VkDependencyInfo* pDependencyInfo) {
    auto* cb = reinterpret_cast<CommandBufferImpl*>(commandBuffer);
    vkrpc::RecordedCommand c;
    c.kind = "set_event2";
    vkrpc::DependencyInfo2 dep;
    const char* reason = "";
    if (!flatten_dependency_info2(pDependencyInfo, dep, reason)) {
        cb->local_invalid = true;
        cb->invalid_reason = reason;
        return;
    }
    c.args_u64.push_back(from_handle(event));
    c.deps2.push_back(std::move(dep));
    cb->recording.push_back(std::move(c));
}

// vkCmdResetEvent2 -- event + a 64-bit stageMask (no dependency).
// args_u64=[event, stageMask64].
VKAPI_ATTR void VKAPI_CALL CmdResetEvent2(VkCommandBuffer commandBuffer, VkEvent event,
                                          VkPipelineStageFlags2 stageMask) {
    auto* cb = reinterpret_cast<CommandBufferImpl*>(commandBuffer);
    vkrpc::RecordedCommand c;
    c.kind = "reset_event2";
    c.args_u64.push_back(from_handle(event));
    c.args_u64.push_back(static_cast<std::uint64_t>(stageMask));
    cb->recording.push_back(std::move(c));
}

// vkCmdWaitEvents2 -- N events paired with N VkDependencyInfo. args_u64=[event x
// N] and deps2.size()==N (deps2[i] is the dependency for events[i]). Null/count guards before any
// deref.
VKAPI_ATTR void VKAPI_CALL CmdWaitEvents2(VkCommandBuffer commandBuffer, std::uint32_t eventCount,
                                          const VkEvent* pEvents,
                                          const VkDependencyInfo* pDependencyInfos) {
    auto* cb = reinterpret_cast<CommandBufferImpl*>(commandBuffer);
    if (eventCount > vkrpc::kMaxSync2Dependencies) {
        cb->local_invalid = true;
        cb->invalid_reason = "wait_events2 event count exceeds the supported cap";
        return;
    }
    if (eventCount != 0 && (pEvents == nullptr || pDependencyInfos == nullptr)) {
        cb->local_invalid = true;
        cb->invalid_reason = "wait_events2 null events/dependencies with a nonzero count";
        return;
    }
    vkrpc::RecordedCommand c;
    c.kind = "wait_events2";
    for (std::uint32_t i = 0; i < eventCount; ++i) {
        vkrpc::DependencyInfo2 dep;
        const char* reason = "";
        if (!flatten_dependency_info2(&pDependencyInfos[i], dep, reason)) {
            cb->local_invalid = true;
            cb->invalid_reason = reason;
            return;
        }
        c.args_u64.push_back(from_handle(pEvents[i]));
        c.deps2.push_back(std::move(dep));
    }
    cb->recording.push_back(std::move(c));
}

VKAPI_ATTR void VKAPI_CALL CmdClearColorImage(VkCommandBuffer commandBuffer, VkImage image,
                                              VkImageLayout imageLayout,
                                              const VkClearColorValue* pColor,
                                              std::uint32_t /*rangeCount*/,
                                              const VkImageSubresourceRange* /*pRanges*/) {
    auto* cb = reinterpret_cast<CommandBufferImpl*>(commandBuffer);
    vkrpc::RecordedCommand c;
    c.kind = "clear_color_image";
    c.image = from_handle(image);
    c.layout = static_cast<int>(imageLayout);
    if (pColor != nullptr) {
        c.r = pColor->float32[0];
        c.g = pColor->float32[1];
        c.b = pColor->float32[2];
        c.a = pColor->float32[3];
    }
    cb->recording.push_back(std::move(c));
}

// (GL/zink): faithful IN-RENDER-PASS scissored clear. zink emits vkCmdClearAttachments for a
// scissored/partial glClear -- notably the FIRST frame after a window resize (steady-state clears
// ride the render-pass loadOp instead), so the missing implementation aborted a GL app exactly on
// maximize. Plain data, fully faithful: args_i64 = [attachmentCount, rectCount, then per attachment
// (aspectMask, colorAttachment), then per rect (x, y, w, h, baseArrayLayer, layerCount)]; args_u64
// = 4 RAW 32-bit words of each attachment's VkClearValue -- the raw union words are bit-faithful
// for BOTH interpretations (color float/int32/uint32 AND depth[word0]+stencil[word1]).
VKAPI_ATTR void VKAPI_CALL CmdClearAttachments(VkCommandBuffer commandBuffer,
                                               std::uint32_t attachmentCount,
                                               const VkClearAttachment* pAttachments,
                                               std::uint32_t rectCount, const VkClearRect* pRects) {
    auto* cb = reinterpret_cast<CommandBufferImpl*>(commandBuffer);
    vkrpc::RecordedCommand c;
    c.kind = "clear_attachments";
    c.args_i64.push_back(static_cast<long long>(attachmentCount));
    c.args_i64.push_back(static_cast<long long>(rectCount));
    for (std::uint32_t i = 0; i < attachmentCount; ++i) {
        const VkClearAttachment& a = pAttachments[i];
        c.args_i64.push_back(static_cast<long long>(a.aspectMask));
        c.args_i64.push_back(static_cast<long long>(a.colorAttachment)); // VK_ATTACHMENT_UNUSED ok
        for (int w = 0; w < 4; ++w) {
            c.args_u64.push_back(a.clearValue.color.uint32[w]); // raw union words (bit-faithful)
        }
    }
    for (std::uint32_t i = 0; i < rectCount; ++i) {
        const VkClearRect& r = pRects[i];
        c.args_i64.push_back(static_cast<long long>(r.rect.offset.x));
        c.args_i64.push_back(static_cast<long long>(r.rect.offset.y));
        c.args_i64.push_back(static_cast<long long>(r.rect.extent.width));
        c.args_i64.push_back(static_cast<long long>(r.rect.extent.height));
        c.args_i64.push_back(static_cast<long long>(r.baseArrayLayer));
        c.args_i64.push_back(static_cast<long long>(r.layerCount));
    }
    cb->recording.push_back(std::move(c));
}

// Faithful texture upload: staging buffer -> TRANSFER_DST
// image, ALL regions with ALL VkBufferImageCopy fields carried (sub-region glTexSubImage2D-class
// uploads, mip levels, array layers, buffer strides -- the fixed one-full-image-region subset
// silently blocked real GL games). Encoding mirrors copy_image_to_buffer (the reviewed
// readback): args_i64=[dstImageLayout, regionCount, then 13 values per region: bufferOffset,
// bufferRowLength, bufferImageHeight, aspectMask, mipLevel, baseArrayLayer, layerCount,
// imageOffset.{x,y,z}, imageExtent.{w,h,d}]. The src/dst handles ride the dedicated fields; the
// worker validates each region against the image it knows (mips/layers/mip-extents).
VKAPI_ATTR void VKAPI_CALL CmdCopyBufferToImage(VkCommandBuffer commandBuffer, VkBuffer srcBuffer,
                                                VkImage dstImage, VkImageLayout dstImageLayout,
                                                std::uint32_t regionCount,
                                                const VkBufferImageCopy* pRegions) {
    auto* cb = reinterpret_cast<CommandBufferImpl*>(commandBuffer);
    const char* why = "";
    if (!vkr::icd_subset::copy_buffer_to_image_ok(dstImageLayout, regionCount, pRegions, &why)) {
        cb->local_invalid = true;
        cb->invalid_reason = why;
        return;
    }
    vkrpc::RecordedCommand c;
    c.kind = "copy_buffer_to_image";
    c.src_buffer = from_handle(srcBuffer);
    c.image = from_handle(dstImage);
    c.args_i64.push_back(static_cast<long long>(dstImageLayout));
    c.args_i64.push_back(static_cast<long long>(regionCount));
    for (std::uint32_t i = 0; i < regionCount; ++i) {
        const VkBufferImageCopy& r = pRegions[i];
        c.args_i64.push_back(static_cast<long long>(r.bufferOffset));
        c.args_i64.push_back(static_cast<long long>(r.bufferRowLength));
        c.args_i64.push_back(static_cast<long long>(r.bufferImageHeight));
        c.args_i64.push_back(static_cast<long long>(r.imageSubresource.aspectMask));
        c.args_i64.push_back(static_cast<long long>(r.imageSubresource.mipLevel));
        c.args_i64.push_back(static_cast<long long>(r.imageSubresource.baseArrayLayer));
        c.args_i64.push_back(static_cast<long long>(r.imageSubresource.layerCount));
        c.args_i64.push_back(static_cast<long long>(r.imageOffset.x));
        c.args_i64.push_back(static_cast<long long>(r.imageOffset.y));
        c.args_i64.push_back(static_cast<long long>(r.imageOffset.z));
        c.args_i64.push_back(static_cast<long long>(r.imageExtent.width));
        c.args_i64.push_back(static_cast<long long>(r.imageExtent.height));
        c.args_i64.push_back(static_cast<long long>(r.imageExtent.depth));
    }
    cb->recording.push_back(std::move(c));
}

// (GL/zink): faithful image->buffer readback (glReadPixels for offscreen PNG export +
// general framebuffer readback). args_u64=[srcImage, dstBuffer]; args_i64=[srcImageLayout,
// regionCount, then 13 values per region: bufferOffset, bufferRowLength, bufferImageHeight,
// aspectMask, mipLevel, baseArrayLayer, layerCount, imageOffset.{x,y,z}, imageExtent.{w,h,d}].
// All regions are forwarded faithfully (readback regions vary by app).
VKAPI_ATTR void VKAPI_CALL CmdCopyImageToBuffer(VkCommandBuffer commandBuffer, VkImage srcImage,
                                                VkImageLayout srcImageLayout, VkBuffer dstBuffer,
                                                std::uint32_t regionCount,
                                                const VkBufferImageCopy* pRegions) {
    auto* cb = reinterpret_cast<CommandBufferImpl*>(commandBuffer);
    vkrpc::RecordedCommand c;
    c.kind = "copy_image_to_buffer";
    c.args_u64.push_back(from_handle(srcImage));
    c.args_u64.push_back(from_handle(dstBuffer));
    c.args_i64.push_back(static_cast<long long>(srcImageLayout));
    c.args_i64.push_back(static_cast<long long>(regionCount));
    for (std::uint32_t i = 0; i < regionCount; ++i) {
        const VkBufferImageCopy& r = pRegions[i];
        c.args_i64.push_back(static_cast<long long>(r.bufferOffset));
        c.args_i64.push_back(static_cast<long long>(r.bufferRowLength));
        c.args_i64.push_back(static_cast<long long>(r.bufferImageHeight));
        c.args_i64.push_back(static_cast<long long>(r.imageSubresource.aspectMask));
        c.args_i64.push_back(static_cast<long long>(r.imageSubresource.mipLevel));
        c.args_i64.push_back(static_cast<long long>(r.imageSubresource.baseArrayLayer));
        c.args_i64.push_back(static_cast<long long>(r.imageSubresource.layerCount));
        c.args_i64.push_back(static_cast<long long>(r.imageOffset.x));
        c.args_i64.push_back(static_cast<long long>(r.imageOffset.y));
        c.args_i64.push_back(static_cast<long long>(r.imageOffset.z));
        c.args_i64.push_back(static_cast<long long>(r.imageExtent.width));
        c.args_i64.push_back(static_cast<long long>(r.imageExtent.height));
        c.args_i64.push_back(static_cast<long long>(r.imageExtent.depth));
    }
    cb->recording.push_back(std::move(c));
    // Offscreen readback: the dst buffer's memory holds GPU-written pixels after this command
    // executes (zink's glReadPixels staging -- OpenSCAD's -o png export). Note JUST that memory on
    // the CB -- the same surgical contract as CmdCopyQueryPoolResults above: armed at
    // vkQueueSubmit, promoted to downloadable at a PROVEN sync point.
    std::lock_guard<std::mutex> lk(g_mu);
    const auto it = g_buffers.find(from_handle(dstBuffer));
    if (it != g_buffers.end() && it->second.memory != 0) {
        cb->readback_dsts.push_back(it->second.memory);
    }
}

// (GL/zink): faithful buffer->buffer copy. args_u64 = [src, dst, (srcOff, dstOff, size) x
// N].
VKAPI_ATTR void VKAPI_CALL CmdCopyBuffer(VkCommandBuffer commandBuffer, VkBuffer srcBuffer,
                                         VkBuffer dstBuffer, std::uint32_t regionCount,
                                         const VkBufferCopy* pRegions) {
    auto* cb = reinterpret_cast<CommandBufferImpl*>(commandBuffer);
    vkrpc::RecordedCommand c;
    c.kind = "copy_buffer";
    c.args_u64.push_back(from_handle(srcBuffer));
    c.args_u64.push_back(from_handle(dstBuffer));
    for (std::uint32_t i = 0; i < regionCount; ++i) {
        c.args_u64.push_back(static_cast<std::uint64_t>(pRegions[i].srcOffset));
        c.args_u64.push_back(static_cast<std::uint64_t>(pRegions[i].dstOffset));
        c.args_u64.push_back(static_cast<std::uint64_t>(pRegions[i].size));
    }
    cb->recording.push_back(std::move(c));
    // Buffer-destination readback, the last named silent-stale class: the destination buffer's
    // memory holds GPU-written bytes after this command executes (compute SSBO results, GL's
    // glGetBufferSubData staging). The same surgical contract as CmdCopyImageToBuffer: note
    // JUST that memory on the CB; armed at vkQueueSubmit, promoted at a PROVEN sync point.
    std::lock_guard<std::mutex> lk(g_mu);
    const auto it = g_buffers.find(from_handle(dstBuffer));
    if (it != g_buffers.end() && it->second.memory != 0) {
        cb->readback_dsts.push_back(it->second.memory);
    }
}

// faithful vkCmdFillBuffer (Mesa >= 25 zink emits it for GL buffer clears/initialization --
// glClearBufferData and zink-internal buffer init). args_u64=[dstBuffer, dstOffset, size, data];
// size VK_WHOLE_SIZE rides intact as 0xFFFFFFFFFFFFFFFF. The filled bytes are GPU-written buffer
// content the app may read back (glGetBufferSubData), so the destination joins the same
// readback-promotion contract as copy_buffer's destination.
VKAPI_ATTR void VKAPI_CALL CmdFillBuffer(VkCommandBuffer commandBuffer, VkBuffer dstBuffer,
                                         VkDeviceSize dstOffset, VkDeviceSize size,
                                         std::uint32_t data) {
    auto* cb = reinterpret_cast<CommandBufferImpl*>(commandBuffer);
    vkrpc::RecordedCommand c;
    c.kind = "fill_buffer";
    c.args_u64.push_back(from_handle(dstBuffer));
    c.args_u64.push_back(static_cast<std::uint64_t>(dstOffset));
    c.args_u64.push_back(static_cast<std::uint64_t>(size));
    c.args_u64.push_back(static_cast<std::uint64_t>(data));
    cb->recording.push_back(std::move(c));
    std::lock_guard<std::mutex> lk(g_mu);
    const auto it = g_buffers.find(from_handle(dstBuffer));
    if (it != g_buffers.end() && it->second.memory != 0) {
        cb->readback_dsts.push_back(it->second.memory);
    }
}

// (GL/zink): faithful image->image blit. zink emits vkCmdBlitImage for the scaled 2:1 copies
// of glGenerateMipmap (the very call a GL-3.0 loader exposes) and for direct glBlitFramebuffer
// paths, so the ladder needs it wired. args_u64=[srcImage, dstImage]; args_i64=[srcLayout,
// dstLayout, filter, regionCount, then 20 values per region: srcSubresource
// (aspectMask, mipLevel, baseArrayLayer, layerCount), srcOffsets[0].xyz, srcOffsets[1].xyz,
// dstSubresource (4), dstOffsets[0].xyz, dstOffsets[1].xyz]. Offsets ride as SIGNED i64 (blit
// corners may be reversed for a flip). All regions forwarded faithfully.
VKAPI_ATTR void VKAPI_CALL CmdBlitImage(VkCommandBuffer commandBuffer, VkImage srcImage,
                                        VkImageLayout srcImageLayout, VkImage dstImage,
                                        VkImageLayout dstImageLayout, std::uint32_t regionCount,
                                        const VkImageBlit* pRegions, VkFilter filter) {
    auto* cb = reinterpret_cast<CommandBufferImpl*>(commandBuffer);
    vkrpc::RecordedCommand c;
    c.kind = "blit_image";
    c.args_u64.push_back(from_handle(srcImage));
    c.args_u64.push_back(from_handle(dstImage));
    c.args_i64.push_back(static_cast<long long>(srcImageLayout));
    c.args_i64.push_back(static_cast<long long>(dstImageLayout));
    c.args_i64.push_back(static_cast<long long>(filter));
    c.args_i64.push_back(static_cast<long long>(regionCount));
    for (std::uint32_t i = 0; i < regionCount; ++i) {
        const VkImageBlit& r = pRegions[i];
        c.args_i64.push_back(static_cast<long long>(r.srcSubresource.aspectMask));
        c.args_i64.push_back(static_cast<long long>(r.srcSubresource.mipLevel));
        c.args_i64.push_back(static_cast<long long>(r.srcSubresource.baseArrayLayer));
        c.args_i64.push_back(static_cast<long long>(r.srcSubresource.layerCount));
        for (int o = 0; o < 2; ++o) {
            c.args_i64.push_back(static_cast<long long>(r.srcOffsets[o].x));
            c.args_i64.push_back(static_cast<long long>(r.srcOffsets[o].y));
            c.args_i64.push_back(static_cast<long long>(r.srcOffsets[o].z));
        }
        c.args_i64.push_back(static_cast<long long>(r.dstSubresource.aspectMask));
        c.args_i64.push_back(static_cast<long long>(r.dstSubresource.mipLevel));
        c.args_i64.push_back(static_cast<long long>(r.dstSubresource.baseArrayLayer));
        c.args_i64.push_back(static_cast<long long>(r.dstSubresource.layerCount));
        for (int o = 0; o < 2; ++o) {
            c.args_i64.push_back(static_cast<long long>(r.dstOffsets[o].x));
            c.args_i64.push_back(static_cast<long long>(r.dstOffsets[o].y));
            c.args_i64.push_back(static_cast<long long>(r.dstOffsets[o].z));
        }
    }
    cb->recording.push_back(std::move(c));
}

// (GL/zink): faithful unscaled image->image copy (zink emits vkCmdCopyImage for
// glCopyTexSubImage2D-class transfers between same-format images, e.g. glmark2's [terrain]).
// args_u64=[srcImage, dstImage]; args_i64=[srcLayout, dstLayout, regionCount, then 17 values per
// region: srcSubresource (aspectMask, mipLevel, baseArrayLayer, layerCount), srcOffset.xyz,
// dstSubresource (4), dstOffset.xyz, extent.whd]. All regions forwarded faithfully.
VKAPI_ATTR void VKAPI_CALL CmdCopyImage(VkCommandBuffer commandBuffer, VkImage srcImage,
                                        VkImageLayout srcImageLayout, VkImage dstImage,
                                        VkImageLayout dstImageLayout, std::uint32_t regionCount,
                                        const VkImageCopy* pRegions) {
    auto* cb = reinterpret_cast<CommandBufferImpl*>(commandBuffer);
    vkrpc::RecordedCommand c;
    c.kind = "copy_image";
    c.args_u64.push_back(from_handle(srcImage));
    c.args_u64.push_back(from_handle(dstImage));
    c.args_i64.push_back(static_cast<long long>(srcImageLayout));
    c.args_i64.push_back(static_cast<long long>(dstImageLayout));
    c.args_i64.push_back(static_cast<long long>(regionCount));
    for (std::uint32_t i = 0; i < regionCount; ++i) {
        const VkImageCopy& r = pRegions[i];
        c.args_i64.push_back(static_cast<long long>(r.srcSubresource.aspectMask));
        c.args_i64.push_back(static_cast<long long>(r.srcSubresource.mipLevel));
        c.args_i64.push_back(static_cast<long long>(r.srcSubresource.baseArrayLayer));
        c.args_i64.push_back(static_cast<long long>(r.srcSubresource.layerCount));
        c.args_i64.push_back(static_cast<long long>(r.srcOffset.x));
        c.args_i64.push_back(static_cast<long long>(r.srcOffset.y));
        c.args_i64.push_back(static_cast<long long>(r.srcOffset.z));
        c.args_i64.push_back(static_cast<long long>(r.dstSubresource.aspectMask));
        c.args_i64.push_back(static_cast<long long>(r.dstSubresource.mipLevel));
        c.args_i64.push_back(static_cast<long long>(r.dstSubresource.baseArrayLayer));
        c.args_i64.push_back(static_cast<long long>(r.dstSubresource.layerCount));
        c.args_i64.push_back(static_cast<long long>(r.dstOffset.x));
        c.args_i64.push_back(static_cast<long long>(r.dstOffset.y));
        c.args_i64.push_back(static_cast<long long>(r.dstOffset.z));
        c.args_i64.push_back(static_cast<long long>(r.extent.width));
        c.args_i64.push_back(static_cast<long long>(r.extent.height));
        c.args_i64.push_back(static_cast<long long>(r.extent.depth));
    }
    cb->recording.push_back(std::move(c));
}

// --- Draw command recording ------------------------------
// These accumulate into the command buffer's recording (no RPC here); vkEndCommandBuffer flushes
// the whole begin->[cmds]->end stream in one record_command_buffer, exactly as the barrier/clear
// do.

// Shared by vkCmdBeginRenderPass (1.0) + vkCmdBeginRenderPass2 (core 1.2). Builds the
// begin_render_pass command and, for an IMAGELESS framebuffer, carries the deferred attachment
// views (VkRenderPassAttachmentBeginInfo) in args_u64 so the worker builds the framebuffer at
// begin.
void record_begin_render_pass(CommandBufferImpl* cb, const VkRenderPassBeginInfo* bi,
                              VkSubpassContents contents) {
    const char* why = "";
    if (!vkr::icd_subset::begin_render_pass_ok(bi, contents, &why)) {
        cb->local_invalid = true;
        cb->invalid_reason = why;
        return;
    }
    vkrpc::RecordedCommand c;
    c.kind = "begin_render_pass";
    c.render_pass = from_handle(bi->renderPass);
    c.framebuffer = from_handle(bi->framebuffer);
    c.render_area_x = bi->renderArea.offset.x;
    c.render_area_y = bi->renderArea.offset.y;
    c.render_area_w = static_cast<int>(bi->renderArea.extent.width);
    c.render_area_h = static_cast<int>(bi->renderArea.extent.height);
    // (GL/zink): carry the EXACT clear-value array faithfully. args_i64[0] =
    // clearValueCount; args_blob = the raw VkClearValue bytes (color OR depthStencil, preserved
    // verbatim). args_i64 being non-empty marks this as the faithful path for the worker; the named
    // r/g/b/a + depth_clear fields stay populated too so the legacy (vkcube/mock) named-field path
    // is intact.
    c.args_i64.push_back(static_cast<long long>(bi->clearValueCount));
    if (bi->clearValueCount > 0 && bi->pClearValues != nullptr) {
        c.args_blob.assign(reinterpret_cast<const char*>(bi->pClearValues),
                           static_cast<std::size_t>(bi->clearValueCount) * sizeof(VkClearValue));
        c.r = bi->pClearValues[0].color.float32[0];
        c.g = bi->pClearValues[0].color.float32[1];
        c.b = bi->pClearValues[0].color.float32[2];
        c.a = bi->pClearValues[0].color.float32[3];
        if (bi->clearValueCount >= 2) {
            c.has_depth_clear = true;
            c.depth_clear = bi->pClearValues[1].depthStencil.depth;
        }
    }
    for (auto* n = static_cast<const VkBaseInStructure*>(bi->pNext); n != nullptr; n = n->pNext) {
        if (n->sType == VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO) {
            const auto* ab = reinterpret_cast<const VkRenderPassAttachmentBeginInfo*>(n);
            for (std::uint32_t i = 0; i < ab->attachmentCount; ++i) {
                c.args_u64.push_back(from_handle(ab->pAttachments[i]));
            }
        }
    }
    icd_trace("CmdBeginRenderPass framebuffer=%llu area=%d,%d %ux%u deferred_views=%zu",
              static_cast<unsigned long long>(c.framebuffer), bi->renderArea.offset.x,
              bi->renderArea.offset.y, bi->renderArea.extent.width, bi->renderArea.extent.height,
              c.args_u64.size());
    cb->recording.push_back(std::move(c));
}

VKAPI_ATTR void VKAPI_CALL CmdBeginRenderPass(VkCommandBuffer commandBuffer,
                                              const VkRenderPassBeginInfo* pRenderPassBegin,
                                              VkSubpassContents contents) {
    record_begin_render_pass(reinterpret_cast<CommandBufferImpl*>(commandBuffer), pRenderPassBegin,
                             contents);
}

// (GL/zink): core 1.2 vkCmdBeginRenderPass2 (zink uses it with render-pass-2). The
// VkSubpassBeginInfo carries the contents; the rest maps to the same begin_render_pass command.
VKAPI_ATTR void VKAPI_CALL CmdBeginRenderPass2(VkCommandBuffer commandBuffer,
                                               const VkRenderPassBeginInfo* pRenderPassBegin,
                                               const VkSubpassBeginInfo* pSubpassBeginInfo) {
    const VkSubpassContents contents =
        pSubpassBeginInfo != nullptr ? pSubpassBeginInfo->contents : VK_SUBPASS_CONTENTS_INLINE;
    record_begin_render_pass(reinterpret_cast<CommandBufferImpl*>(commandBuffer), pRenderPassBegin,
                             contents);
}

VKAPI_ATTR void VKAPI_CALL CmdEndRenderPass(VkCommandBuffer commandBuffer) {
    auto* cb = reinterpret_cast<CommandBufferImpl*>(commandBuffer);
    vkrpc::RecordedCommand c;
    c.kind = "end_render_pass";
    cb->recording.push_back(std::move(c));
}

VKAPI_ATTR void VKAPI_CALL CmdEndRenderPass2(VkCommandBuffer commandBuffer,
                                             const VkSubpassEndInfo* /*pSubpassEndInfo*/) {
    auto* cb = reinterpret_cast<CommandBufferImpl*>(commandBuffer);
    vkrpc::RecordedCommand c;
    c.kind = "end_render_pass";
    cb->recording.push_back(std::move(c));
}

VKAPI_ATTR void VKAPI_CALL CmdBindPipeline(VkCommandBuffer commandBuffer,
                                           VkPipelineBindPoint bindPoint, VkPipeline pipeline) {
    auto* cb = reinterpret_cast<CommandBufferImpl*>(commandBuffer);
    const char* why = "";
    if (!vkr::icd_subset::bind_pipeline_ok(bindPoint, &why)) {
        cb->local_invalid = true;
        cb->invalid_reason = why;
        return;
    }
    vkrpc::RecordedCommand c;
    c.kind = "bind_pipeline";
    c.pipeline = from_handle(pipeline);
    // Compute: carry the bind point (the bind_descriptor_sets precedent -- absent =
    // GRAPHICS on old recordings). The worker cross-checks it against the pipeline's KIND.
    c.args_i64.push_back(static_cast<long long>(bindPoint));
    cb->recording.push_back(std::move(c));
}

// Compute: faithful dispatches. args_u64 = [x, y, z] (0 is a legal no-op); the
// indirect form carries the buffer + [offset] and the worker validates usage/align/range.
VKAPI_ATTR void VKAPI_CALL CmdDispatch(VkCommandBuffer commandBuffer, std::uint32_t x,
                                       std::uint32_t y, std::uint32_t z) {
    auto* cb = reinterpret_cast<CommandBufferImpl*>(commandBuffer);
    vkrpc::RecordedCommand c;
    c.kind = "dispatch";
    c.args_u64.push_back(x);
    c.args_u64.push_back(y);
    c.args_u64.push_back(z);
    cb->recording.push_back(std::move(c));
}

VKAPI_ATTR void VKAPI_CALL CmdDispatchIndirect(VkCommandBuffer commandBuffer, VkBuffer buffer,
                                               VkDeviceSize offset) {
    auto* cb = reinterpret_cast<CommandBufferImpl*>(commandBuffer);
    vkrpc::RecordedCommand c;
    c.kind = "dispatch_indirect";
    c.src_buffer = from_handle(buffer);
    c.args_u64.push_back(static_cast<std::uint64_t>(offset));
    cb->recording.push_back(std::move(c));
}

VKAPI_ATTR void VKAPI_CALL CmdSetViewport(VkCommandBuffer commandBuffer,
                                          std::uint32_t firstViewport, std::uint32_t viewportCount,
                                          const VkViewport* pViewports) {
    auto* cb = reinterpret_cast<CommandBufferImpl*>(commandBuffer);
    const char* why = "";
    if (!vkr::icd_subset::set_viewport_ok(firstViewport, viewportCount, &why) ||
        pViewports == nullptr) {
        cb->local_invalid = true;
        cb->invalid_reason = why;
        return;
    }
    // a single dynamic viewport (the pipeline declares viewportCount 1).
    vkrpc::RecordedCommand c;
    c.kind = "set_viewport";
    c.vp_x = pViewports[0].x;
    c.vp_y = pViewports[0].y;
    c.vp_w = pViewports[0].width;
    c.vp_h = pViewports[0].height;
    c.vp_min_depth = pViewports[0].minDepth;
    c.vp_max_depth = pViewports[0].maxDepth;
    cb->recording.push_back(std::move(c));
}

VKAPI_ATTR void VKAPI_CALL CmdSetScissor(VkCommandBuffer commandBuffer, std::uint32_t firstScissor,
                                         std::uint32_t scissorCount, const VkRect2D* pScissors) {
    auto* cb = reinterpret_cast<CommandBufferImpl*>(commandBuffer);
    const char* why = "";
    if (!vkr::icd_subset::set_scissor_ok(firstScissor, scissorCount, &why) ||
        pScissors == nullptr) {
        cb->local_invalid = true;
        cb->invalid_reason = why;
        return;
    }
    vkrpc::RecordedCommand c;
    c.kind = "set_scissor";
    c.sc_x = pScissors[0].offset.x;
    c.sc_y = pScissors[0].offset.y;
    c.sc_w = static_cast<int>(pScissors[0].extent.width);
    c.sc_h = static_cast<int>(pScissors[0].extent.height);
    cb->recording.push_back(std::move(c));
}

// (GL/zink): the core-1.0 dynamic-state set commands a pipeline may declare dynamic. Each
// rides the generic payload; the worker emits the matching vkCmdSet* (none gate draw readiness).
VKAPI_ATTR void VKAPI_CALL CmdSetLineWidth(VkCommandBuffer commandBuffer, float lineWidth) {
    auto* cb = reinterpret_cast<CommandBufferImpl*>(commandBuffer);
    vkrpc::RecordedCommand c;
    c.kind = "set_line_width";
    c.args_f64.push_back(static_cast<double>(lineWidth));
    cb->recording.push_back(std::move(c));
}
VKAPI_ATTR void VKAPI_CALL CmdSetDepthBias(VkCommandBuffer commandBuffer, float constantFactor,
                                           float clamp, float slopeFactor) {
    auto* cb = reinterpret_cast<CommandBufferImpl*>(commandBuffer);
    vkrpc::RecordedCommand c;
    c.kind = "set_depth_bias";
    c.args_f64 = {static_cast<double>(constantFactor), static_cast<double>(clamp),
                  static_cast<double>(slopeFactor)};
    cb->recording.push_back(std::move(c));
}
VKAPI_ATTR void VKAPI_CALL CmdSetBlendConstants(VkCommandBuffer commandBuffer,
                                                const float blendConstants[4]) {
    auto* cb = reinterpret_cast<CommandBufferImpl*>(commandBuffer);
    vkrpc::RecordedCommand c;
    c.kind = "set_blend_constants";
    for (int i = 0; i < 4; ++i) {
        c.args_f64.push_back(static_cast<double>(blendConstants[i]));
    }
    cb->recording.push_back(std::move(c));
}
VKAPI_ATTR void VKAPI_CALL CmdSetDepthBounds(VkCommandBuffer commandBuffer, float minDepthBounds,
                                             float maxDepthBounds) {
    auto* cb = reinterpret_cast<CommandBufferImpl*>(commandBuffer);
    vkrpc::RecordedCommand c;
    c.kind = "set_depth_bounds";
    c.args_f64 = {static_cast<double>(minDepthBounds), static_cast<double>(maxDepthBounds)};
    cb->recording.push_back(std::move(c));
}
VKAPI_ATTR void VKAPI_CALL CmdSetStencilCompareMask(VkCommandBuffer commandBuffer,
                                                    VkStencilFaceFlags faceMask,
                                                    std::uint32_t compareMask) {
    auto* cb = reinterpret_cast<CommandBufferImpl*>(commandBuffer);
    vkrpc::RecordedCommand c;
    c.kind = "set_stencil_compare_mask";
    c.args_i64 = {static_cast<long long>(faceMask), static_cast<long long>(compareMask)};
    cb->recording.push_back(std::move(c));
}
VKAPI_ATTR void VKAPI_CALL CmdSetStencilWriteMask(VkCommandBuffer commandBuffer,
                                                  VkStencilFaceFlags faceMask,
                                                  std::uint32_t writeMask) {
    auto* cb = reinterpret_cast<CommandBufferImpl*>(commandBuffer);
    vkrpc::RecordedCommand c;
    c.kind = "set_stencil_write_mask";
    c.args_i64 = {static_cast<long long>(faceMask), static_cast<long long>(writeMask)};
    cb->recording.push_back(std::move(c));
}
VKAPI_ATTR void VKAPI_CALL CmdSetStencilReference(VkCommandBuffer commandBuffer,
                                                  VkStencilFaceFlags faceMask,
                                                  std::uint32_t reference) {
    auto* cb = reinterpret_cast<CommandBufferImpl*>(commandBuffer);
    vkrpc::RecordedCommand c;
    c.kind = "set_stencil_reference";
    c.args_i64 = {static_cast<long long>(faceMask), static_cast<long long>(reference)};
    cb->recording.push_back(std::move(c));
}

// (the Vulkan-1.3 opener -- EDS1): VK_EXT_extended_dynamic_state's six state-setters, each
// a single u32 carried wide in args_i64[0]. Exposed ONLY on the native lane (the extension +
// extendedDynamicState feature are lane-gated above), so zink never records them. Each rides the
// generic payload; the worker emits the matching core-1.3 vkCmdSet* and none gate draw readiness.
// (Provenance: the original vkrelay had these as InnerCmdOp opcodes; re-implemented natively in
// vkrelay2's RecordedCommand/CmdKind model -- zero lines copied.)
VKAPI_ATTR void VKAPI_CALL CmdSetCullMode(VkCommandBuffer commandBuffer, VkCullModeFlags cullMode) {
    auto* cb = reinterpret_cast<CommandBufferImpl*>(commandBuffer);
    vkrpc::RecordedCommand c;
    c.kind = "set_cull_mode";
    c.args_i64 = {static_cast<long long>(cullMode)};
    cb->recording.push_back(std::move(c));
}
VKAPI_ATTR void VKAPI_CALL CmdSetFrontFace(VkCommandBuffer commandBuffer, VkFrontFace frontFace) {
    auto* cb = reinterpret_cast<CommandBufferImpl*>(commandBuffer);
    vkrpc::RecordedCommand c;
    c.kind = "set_front_face";
    c.args_i64 = {static_cast<long long>(frontFace)};
    cb->recording.push_back(std::move(c));
}
VKAPI_ATTR void VKAPI_CALL CmdSetPrimitiveTopology(VkCommandBuffer commandBuffer,
                                                   VkPrimitiveTopology primitiveTopology) {
    auto* cb = reinterpret_cast<CommandBufferImpl*>(commandBuffer);
    vkrpc::RecordedCommand c;
    c.kind = "set_primitive_topology";
    c.args_i64 = {static_cast<long long>(primitiveTopology)};
    cb->recording.push_back(std::move(c));
}
VKAPI_ATTR void VKAPI_CALL CmdSetDepthTestEnable(VkCommandBuffer commandBuffer,
                                                 VkBool32 depthTestEnable) {
    auto* cb = reinterpret_cast<CommandBufferImpl*>(commandBuffer);
    vkrpc::RecordedCommand c;
    c.kind = "set_depth_test_enable";
    c.args_i64 = {static_cast<long long>(depthTestEnable)};
    cb->recording.push_back(std::move(c));
}
VKAPI_ATTR void VKAPI_CALL CmdSetDepthWriteEnable(VkCommandBuffer commandBuffer,
                                                  VkBool32 depthWriteEnable) {
    auto* cb = reinterpret_cast<CommandBufferImpl*>(commandBuffer);
    vkrpc::RecordedCommand c;
    c.kind = "set_depth_write_enable";
    c.args_i64 = {static_cast<long long>(depthWriteEnable)};
    cb->recording.push_back(std::move(c));
}
VKAPI_ATTR void VKAPI_CALL CmdSetDepthCompareOp(VkCommandBuffer commandBuffer,
                                                VkCompareOp depthCompareOp) {
    auto* cb = reinterpret_cast<CommandBufferImpl*>(commandBuffer);
    vkrpc::RecordedCommand c;
    c.kind = "set_depth_compare_op";
    c.args_i64 = {static_cast<long long>(depthCompareOp)};
    cb->recording.push_back(std::move(c));
}

// Vulkan 1.3 support: the remaining EDS1 setters + the core-1.3 EDS2 subset + vkCmdResolveImage.
// Registered unconditionally like the EDS six above (a proc addr is not advertising); the worker
// enforces the device gates (EDS extension-or-vk13 for the EDS1 group, the honest vk13 device for
// the EDS2 toggles). Each flattens per the layout documented on its CmdKind.
VKAPI_ATTR void VKAPI_CALL CmdSetViewportWithCount(VkCommandBuffer commandBuffer,
                                                   std::uint32_t viewportCount,
                                                   const VkViewport* pViewports) {
    auto* cb = reinterpret_cast<CommandBufferImpl*>(commandBuffer);
    vkrpc::RecordedCommand c;
    c.kind = "set_viewport_with_count";
    c.args_i64 = {static_cast<long long>(viewportCount)};
    for (std::uint32_t i = 0; i < viewportCount; ++i) {
        const VkViewport& v = pViewports[i];
        c.args_f64.push_back(static_cast<double>(v.x));
        c.args_f64.push_back(static_cast<double>(v.y));
        c.args_f64.push_back(static_cast<double>(v.width));
        c.args_f64.push_back(static_cast<double>(v.height));
        c.args_f64.push_back(static_cast<double>(v.minDepth));
        c.args_f64.push_back(static_cast<double>(v.maxDepth));
    }
    cb->recording.push_back(std::move(c));
}
VKAPI_ATTR void VKAPI_CALL CmdSetScissorWithCount(VkCommandBuffer commandBuffer,
                                                  std::uint32_t scissorCount,
                                                  const VkRect2D* pScissors) {
    auto* cb = reinterpret_cast<CommandBufferImpl*>(commandBuffer);
    vkrpc::RecordedCommand c;
    c.kind = "set_scissor_with_count";
    c.args_i64.push_back(static_cast<long long>(scissorCount));
    for (std::uint32_t i = 0; i < scissorCount; ++i) {
        const VkRect2D& s = pScissors[i];
        c.args_i64.push_back(static_cast<long long>(s.offset.x));
        c.args_i64.push_back(static_cast<long long>(s.offset.y));
        c.args_i64.push_back(static_cast<long long>(s.extent.width));
        c.args_i64.push_back(static_cast<long long>(s.extent.height));
    }
    cb->recording.push_back(std::move(c));
}
VKAPI_ATTR void VKAPI_CALL CmdBindVertexBuffers2(
    VkCommandBuffer commandBuffer, std::uint32_t firstBinding, std::uint32_t bindingCount,
    const VkBuffer* pBuffers, const VkDeviceSize* pOffsets, const VkDeviceSize* pSizes,
    const VkDeviceSize* pStrides) {
    auto* cb = reinterpret_cast<CommandBufferImpl*>(commandBuffer);
    vkrpc::RecordedCommand c;
    c.kind = "bind_vertex_buffers2";
    c.args_i64 = {static_cast<long long>(firstBinding), static_cast<long long>(bindingCount),
                  pSizes != nullptr ? 1ll : 0ll, pStrides != nullptr ? 1ll : 0ll};
    for (std::uint32_t i = 0; i < bindingCount; ++i) {
        c.args_u64.push_back(from_handle(pBuffers[i]));
    }
    for (std::uint32_t i = 0; i < bindingCount; ++i) {
        c.args_u64.push_back(static_cast<std::uint64_t>(pOffsets[i]));
    }
    if (pSizes != nullptr) {
        for (std::uint32_t i = 0; i < bindingCount; ++i) {
            c.args_u64.push_back(static_cast<std::uint64_t>(pSizes[i]));
        }
    }
    if (pStrides != nullptr) {
        for (std::uint32_t i = 0; i < bindingCount; ++i) {
            c.args_u64.push_back(static_cast<std::uint64_t>(pStrides[i]));
        }
    }
    cb->recording.push_back(std::move(c));
}
VKAPI_ATTR void VKAPI_CALL CmdSetDepthBoundsTestEnable(VkCommandBuffer commandBuffer,
                                                       VkBool32 depthBoundsTestEnable) {
    auto* cb = reinterpret_cast<CommandBufferImpl*>(commandBuffer);
    vkrpc::RecordedCommand c;
    c.kind = "set_depth_bounds_test_enable";
    c.args_i64 = {static_cast<long long>(depthBoundsTestEnable)};
    cb->recording.push_back(std::move(c));
}
VKAPI_ATTR void VKAPI_CALL CmdSetStencilTestEnable(VkCommandBuffer commandBuffer,
                                                   VkBool32 stencilTestEnable) {
    auto* cb = reinterpret_cast<CommandBufferImpl*>(commandBuffer);
    vkrpc::RecordedCommand c;
    c.kind = "set_stencil_test_enable";
    c.args_i64 = {static_cast<long long>(stencilTestEnable)};
    cb->recording.push_back(std::move(c));
}
VKAPI_ATTR void VKAPI_CALL CmdSetStencilOp(VkCommandBuffer commandBuffer,
                                           VkStencilFaceFlags faceMask, VkStencilOp failOp,
                                           VkStencilOp passOp, VkStencilOp depthFailOp,
                                           VkCompareOp compareOp) {
    auto* cb = reinterpret_cast<CommandBufferImpl*>(commandBuffer);
    vkrpc::RecordedCommand c;
    c.kind = "set_stencil_op";
    c.args_i64 = {static_cast<long long>(faceMask), static_cast<long long>(failOp),
                  static_cast<long long>(passOp), static_cast<long long>(depthFailOp),
                  static_cast<long long>(compareOp)};
    cb->recording.push_back(std::move(c));
}
VKAPI_ATTR void VKAPI_CALL CmdSetRasterizerDiscardEnable(VkCommandBuffer commandBuffer,
                                                         VkBool32 rasterizerDiscardEnable) {
    auto* cb = reinterpret_cast<CommandBufferImpl*>(commandBuffer);
    vkrpc::RecordedCommand c;
    c.kind = "set_rasterizer_discard_enable";
    c.args_i64 = {static_cast<long long>(rasterizerDiscardEnable)};
    cb->recording.push_back(std::move(c));
}
VKAPI_ATTR void VKAPI_CALL CmdSetDepthBiasEnable(VkCommandBuffer commandBuffer,
                                                 VkBool32 depthBiasEnable) {
    auto* cb = reinterpret_cast<CommandBufferImpl*>(commandBuffer);
    vkrpc::RecordedCommand c;
    c.kind = "set_depth_bias_enable";
    c.args_i64 = {static_cast<long long>(depthBiasEnable)};
    cb->recording.push_back(std::move(c));
}
VKAPI_ATTR void VKAPI_CALL CmdSetPrimitiveRestartEnable(VkCommandBuffer commandBuffer,
                                                        VkBool32 primitiveRestartEnable) {
    auto* cb = reinterpret_cast<CommandBufferImpl*>(commandBuffer);
    vkrpc::RecordedCommand c;
    c.kind = "set_primitive_restart_enable";
    c.args_i64 = {static_cast<long long>(primitiveRestartEnable)};
    cb->recording.push_back(std::move(c));
}
VKAPI_ATTR void VKAPI_CALL CmdResolveImage(VkCommandBuffer commandBuffer, VkImage srcImage,
                                           VkImageLayout srcImageLayout, VkImage dstImage,
                                           VkImageLayout dstImageLayout, std::uint32_t regionCount,
                                           const VkImageResolve* pRegions) {
    auto* cb = reinterpret_cast<CommandBufferImpl*>(commandBuffer);
    vkrpc::RecordedCommand c;
    c.kind = "resolve_image";
    c.args_u64.push_back(from_handle(srcImage));
    c.args_u64.push_back(from_handle(dstImage));
    c.args_i64.push_back(static_cast<long long>(srcImageLayout));
    c.args_i64.push_back(static_cast<long long>(dstImageLayout));
    c.args_i64.push_back(static_cast<long long>(regionCount));
    for (std::uint32_t i = 0; i < regionCount; ++i) {
        const VkImageResolve& r = pRegions[i];
        c.args_i64.push_back(static_cast<long long>(r.srcSubresource.aspectMask));
        c.args_i64.push_back(static_cast<long long>(r.srcSubresource.mipLevel));
        c.args_i64.push_back(static_cast<long long>(r.srcSubresource.baseArrayLayer));
        c.args_i64.push_back(static_cast<long long>(r.srcSubresource.layerCount));
        c.args_i64.push_back(static_cast<long long>(r.srcOffset.x));
        c.args_i64.push_back(static_cast<long long>(r.srcOffset.y));
        c.args_i64.push_back(static_cast<long long>(r.srcOffset.z));
        c.args_i64.push_back(static_cast<long long>(r.dstSubresource.aspectMask));
        c.args_i64.push_back(static_cast<long long>(r.dstSubresource.mipLevel));
        c.args_i64.push_back(static_cast<long long>(r.dstSubresource.baseArrayLayer));
        c.args_i64.push_back(static_cast<long long>(r.dstSubresource.layerCount));
        c.args_i64.push_back(static_cast<long long>(r.dstOffset.x));
        c.args_i64.push_back(static_cast<long long>(r.dstOffset.y));
        c.args_i64.push_back(static_cast<long long>(r.dstOffset.z));
        c.args_i64.push_back(static_cast<long long>(r.extent.width));
        c.args_i64.push_back(static_cast<long long>(r.extent.height));
        c.args_i64.push_back(static_cast<long long>(r.extent.depth));
    }
    cb->recording.push_back(std::move(c));
}

// Vulkan 1.3 support: the VK_KHR_copy_commands2 family (core 1.3). Each *2 command is
// spec-identical to its original plus pNext extensibility, so with every pNext empty the mapping
// onto the classic entry is EXACT -- rebuild the region array field-for-field and call the
// classic recorder directly, making the recorded wire byte-identical to the *1 path. Any pNext
// (top-level or per-region) is fail-closed at record time, never silently dropped.
VKAPI_ATTR void VKAPI_CALL CmdCopyBuffer2(VkCommandBuffer commandBuffer,
                                          const VkCopyBufferInfo2* pCopyBufferInfo) {
    auto* cb = reinterpret_cast<CommandBufferImpl*>(commandBuffer);
    const VkCopyBufferInfo2& info = *pCopyBufferInfo;
    // Vulkan 1.3 support: copy_commands2 is a core-1.3 surface -- on a
    // 1.2-reported device (the default/zink lane) these names must not silently work, and the
    // classic down-conversion below erases the distinction before the worker's gates, so the
    // version gate lives here on the command buffer's stamped device flag.
    if (!cb->vk13_device) {
        cb->local_invalid = true;
        cb->invalid_reason = "copy_commands2 requires an apiVersion-1.3 device";
        return;
    }
    bool pnext_clear = info.pNext == nullptr;
    for (std::uint32_t i = 0; pnext_clear && i < info.regionCount; ++i) {
        pnext_clear = info.pRegions[i].pNext == nullptr;
    }
    if (!pnext_clear) {
        cb->local_invalid = true;
        cb->invalid_reason = "copy2 pNext not supported (fail closed)";
        return;
    }
    std::vector<VkBufferCopy> regions(info.regionCount);
    for (std::uint32_t i = 0; i < info.regionCount; ++i) {
        regions[i].srcOffset = info.pRegions[i].srcOffset;
        regions[i].dstOffset = info.pRegions[i].dstOffset;
        regions[i].size = info.pRegions[i].size;
    }
    CmdCopyBuffer(commandBuffer, info.srcBuffer, info.dstBuffer, info.regionCount, regions.data());
}
VKAPI_ATTR void VKAPI_CALL CmdCopyImage2(VkCommandBuffer commandBuffer,
                                         const VkCopyImageInfo2* pCopyImageInfo) {
    auto* cb = reinterpret_cast<CommandBufferImpl*>(commandBuffer);
    const VkCopyImageInfo2& info = *pCopyImageInfo;
    // Vulkan 1.3 support: copy_commands2 is a core-1.3 surface -- on a
    // 1.2-reported device (the default/zink lane) these names must not silently work, and the
    // classic down-conversion below erases the distinction before the worker's gates, so the
    // version gate lives here on the command buffer's stamped device flag.
    if (!cb->vk13_device) {
        cb->local_invalid = true;
        cb->invalid_reason = "copy_commands2 requires an apiVersion-1.3 device";
        return;
    }
    bool pnext_clear = info.pNext == nullptr;
    for (std::uint32_t i = 0; pnext_clear && i < info.regionCount; ++i) {
        pnext_clear = info.pRegions[i].pNext == nullptr;
    }
    if (!pnext_clear) {
        cb->local_invalid = true;
        cb->invalid_reason = "copy2 pNext not supported (fail closed)";
        return;
    }
    std::vector<VkImageCopy> regions(info.regionCount);
    for (std::uint32_t i = 0; i < info.regionCount; ++i) {
        regions[i].srcSubresource = info.pRegions[i].srcSubresource;
        regions[i].srcOffset = info.pRegions[i].srcOffset;
        regions[i].dstSubresource = info.pRegions[i].dstSubresource;
        regions[i].dstOffset = info.pRegions[i].dstOffset;
        regions[i].extent = info.pRegions[i].extent;
    }
    CmdCopyImage(commandBuffer, info.srcImage, info.srcImageLayout, info.dstImage,
                 info.dstImageLayout, info.regionCount, regions.data());
}
VKAPI_ATTR void VKAPI_CALL CmdCopyBufferToImage2(
    VkCommandBuffer commandBuffer, const VkCopyBufferToImageInfo2* pCopyBufferToImageInfo) {
    auto* cb = reinterpret_cast<CommandBufferImpl*>(commandBuffer);
    const VkCopyBufferToImageInfo2& info = *pCopyBufferToImageInfo;
    // Vulkan 1.3 support: copy_commands2 is a core-1.3 surface -- on a
    // 1.2-reported device (the default/zink lane) these names must not silently work, and the
    // classic down-conversion below erases the distinction before the worker's gates, so the
    // version gate lives here on the command buffer's stamped device flag.
    if (!cb->vk13_device) {
        cb->local_invalid = true;
        cb->invalid_reason = "copy_commands2 requires an apiVersion-1.3 device";
        return;
    }
    bool pnext_clear = info.pNext == nullptr;
    for (std::uint32_t i = 0; pnext_clear && i < info.regionCount; ++i) {
        pnext_clear = info.pRegions[i].pNext == nullptr;
    }
    if (!pnext_clear) {
        cb->local_invalid = true;
        cb->invalid_reason = "copy2 pNext not supported (fail closed)";
        return;
    }
    std::vector<VkBufferImageCopy> regions(info.regionCount);
    for (std::uint32_t i = 0; i < info.regionCount; ++i) {
        regions[i].bufferOffset = info.pRegions[i].bufferOffset;
        regions[i].bufferRowLength = info.pRegions[i].bufferRowLength;
        regions[i].bufferImageHeight = info.pRegions[i].bufferImageHeight;
        regions[i].imageSubresource = info.pRegions[i].imageSubresource;
        regions[i].imageOffset = info.pRegions[i].imageOffset;
        regions[i].imageExtent = info.pRegions[i].imageExtent;
    }
    CmdCopyBufferToImage(commandBuffer, info.srcBuffer, info.dstImage, info.dstImageLayout,
                         info.regionCount, regions.data());
}
VKAPI_ATTR void VKAPI_CALL CmdCopyImageToBuffer2(
    VkCommandBuffer commandBuffer, const VkCopyImageToBufferInfo2* pCopyImageToBufferInfo) {
    auto* cb = reinterpret_cast<CommandBufferImpl*>(commandBuffer);
    const VkCopyImageToBufferInfo2& info = *pCopyImageToBufferInfo;
    // Vulkan 1.3 support: copy_commands2 is a core-1.3 surface -- on a
    // 1.2-reported device (the default/zink lane) these names must not silently work, and the
    // classic down-conversion below erases the distinction before the worker's gates, so the
    // version gate lives here on the command buffer's stamped device flag.
    if (!cb->vk13_device) {
        cb->local_invalid = true;
        cb->invalid_reason = "copy_commands2 requires an apiVersion-1.3 device";
        return;
    }
    bool pnext_clear = info.pNext == nullptr;
    for (std::uint32_t i = 0; pnext_clear && i < info.regionCount; ++i) {
        pnext_clear = info.pRegions[i].pNext == nullptr;
    }
    if (!pnext_clear) {
        cb->local_invalid = true;
        cb->invalid_reason = "copy2 pNext not supported (fail closed)";
        return;
    }
    std::vector<VkBufferImageCopy> regions(info.regionCount);
    for (std::uint32_t i = 0; i < info.regionCount; ++i) {
        regions[i].bufferOffset = info.pRegions[i].bufferOffset;
        regions[i].bufferRowLength = info.pRegions[i].bufferRowLength;
        regions[i].bufferImageHeight = info.pRegions[i].bufferImageHeight;
        regions[i].imageSubresource = info.pRegions[i].imageSubresource;
        regions[i].imageOffset = info.pRegions[i].imageOffset;
        regions[i].imageExtent = info.pRegions[i].imageExtent;
    }
    CmdCopyImageToBuffer(commandBuffer, info.srcImage, info.srcImageLayout, info.dstBuffer,
                         info.regionCount, regions.data());
}
VKAPI_ATTR void VKAPI_CALL CmdBlitImage2(VkCommandBuffer commandBuffer,
                                         const VkBlitImageInfo2* pBlitImageInfo) {
    auto* cb = reinterpret_cast<CommandBufferImpl*>(commandBuffer);
    const VkBlitImageInfo2& info = *pBlitImageInfo;
    // Vulkan 1.3 support: copy_commands2 is a core-1.3 surface -- on a
    // 1.2-reported device (the default/zink lane) these names must not silently work, and the
    // classic down-conversion below erases the distinction before the worker's gates, so the
    // version gate lives here on the command buffer's stamped device flag.
    if (!cb->vk13_device) {
        cb->local_invalid = true;
        cb->invalid_reason = "copy_commands2 requires an apiVersion-1.3 device";
        return;
    }
    bool pnext_clear = info.pNext == nullptr;
    for (std::uint32_t i = 0; pnext_clear && i < info.regionCount; ++i) {
        pnext_clear = info.pRegions[i].pNext == nullptr;
    }
    if (!pnext_clear) {
        cb->local_invalid = true;
        cb->invalid_reason = "copy2 pNext not supported (fail closed)";
        return;
    }
    std::vector<VkImageBlit> regions(info.regionCount);
    for (std::uint32_t i = 0; i < info.regionCount; ++i) {
        regions[i].srcSubresource = info.pRegions[i].srcSubresource;
        regions[i].srcOffsets[0] = info.pRegions[i].srcOffsets[0];
        regions[i].srcOffsets[1] = info.pRegions[i].srcOffsets[1];
        regions[i].dstSubresource = info.pRegions[i].dstSubresource;
        regions[i].dstOffsets[0] = info.pRegions[i].dstOffsets[0];
        regions[i].dstOffsets[1] = info.pRegions[i].dstOffsets[1];
    }
    CmdBlitImage(commandBuffer, info.srcImage, info.srcImageLayout, info.dstImage,
                 info.dstImageLayout, info.regionCount, regions.data(), info.filter);
}
VKAPI_ATTR void VKAPI_CALL CmdResolveImage2(VkCommandBuffer commandBuffer,
                                            const VkResolveImageInfo2* pResolveImageInfo) {
    auto* cb = reinterpret_cast<CommandBufferImpl*>(commandBuffer);
    const VkResolveImageInfo2& info = *pResolveImageInfo;
    // Vulkan 1.3 support: copy_commands2 is a core-1.3 surface -- on a
    // 1.2-reported device (the default/zink lane) these names must not silently work, and the
    // classic down-conversion below erases the distinction before the worker's gates, so the
    // version gate lives here on the command buffer's stamped device flag.
    if (!cb->vk13_device) {
        cb->local_invalid = true;
        cb->invalid_reason = "copy_commands2 requires an apiVersion-1.3 device";
        return;
    }
    bool pnext_clear = info.pNext == nullptr;
    for (std::uint32_t i = 0; pnext_clear && i < info.regionCount; ++i) {
        pnext_clear = info.pRegions[i].pNext == nullptr;
    }
    if (!pnext_clear) {
        cb->local_invalid = true;
        cb->invalid_reason = "copy2 pNext not supported (fail closed)";
        return;
    }
    std::vector<VkImageResolve> regions(info.regionCount);
    for (std::uint32_t i = 0; i < info.regionCount; ++i) {
        regions[i].srcSubresource = info.pRegions[i].srcSubresource;
        regions[i].srcOffset = info.pRegions[i].srcOffset;
        regions[i].dstSubresource = info.pRegions[i].dstSubresource;
        regions[i].dstOffset = info.pRegions[i].dstOffset;
        regions[i].extent = info.pRegions[i].extent;
    }
    CmdResolveImage(commandBuffer, info.srcImage, info.srcImageLayout, info.dstImage,
                    info.dstImageLayout, info.regionCount, regions.data());
}

// (native lane): VK_KHR_dynamic_rendering. Flatten VkRenderingInfo into the
// generic arrays (the begin_render_pass clearValue-union + copy_buffer_to_image variable-count
// precedents): args_i64 = [flags, area.{x,y,w,h}, layerCount, viewMask, colorAttachmentCount,
// dsPresence(bit0=depth,bit1=stencil)] then, per attachment in order color[0..n-1], depth?,
// stencil?: [imageLayout, loadOp, storeOp]; args_u64 = one GUEST image-view handle per attachment
// (0 = a real null attachment); args_blob = one raw 16-byte VkClearValue per attachment. The worker
// translates the guest views + rebuilds VkRenderingInfo + replays via the *KHR PFN. Carried
// faithfully; the backends enforce the envelope (no flags/multiview, positive layerCount).
// What the wire does NOT carry -- MSAA resolve + any pNext -- is fail-closed HERE at record time
// (never silently dropped). (Provenance: the original vkrelay carried the same fields as an
// InnerCmdOp; re- implemented natively in vkrelay2's RecordedCommand model -- zero lines copied.)
static_assert(sizeof(VkClearValue) == vkrpc::kClearValueBytes, "VkClearValue size drift");
VKAPI_ATTR void VKAPI_CALL CmdBeginRendering(VkCommandBuffer commandBuffer,
                                             const VkRenderingInfo* pRenderingInfo) {
    auto* cb = reinterpret_cast<CommandBufferImpl*>(commandBuffer);
    if (pRenderingInfo == nullptr || pRenderingInfo->pNext != nullptr) {
        cb->local_invalid = true;
        cb->invalid_reason = "begin_rendering with null info or unsupported VkRenderingInfo pNext";
        return;
    }
    const VkRenderingInfo& ri = *pRenderingInfo;
    const bool has_depth = ri.pDepthAttachment != nullptr;
    const bool has_stencil = ri.pStencilAttachment != nullptr;
    vkrpc::RecordedCommand c;
    c.kind = "begin_rendering";
    c.args_i64 = {
        static_cast<long long>(ri.flags),
        static_cast<long long>(ri.renderArea.offset.x),
        static_cast<long long>(ri.renderArea.offset.y),
        static_cast<long long>(ri.renderArea.extent.width),
        static_cast<long long>(ri.renderArea.extent.height),
        static_cast<long long>(ri.layerCount),
        static_cast<long long>(ri.viewMask),
        static_cast<long long>(ri.colorAttachmentCount),
        static_cast<long long>((has_depth ? 1 : 0) | (has_stencil ? 2 : 0)),
    };
    bool ok = true;
    const auto push_attachment = [&](const VkRenderingAttachmentInfo& a) {
        // Fail-closed: the wire does not carry MSAA resolve or an attachment pNext, so
        // reject them here rather than silently dropping (a follow-on adds the resolve fields).
        if (a.pNext != nullptr || a.resolveMode != VK_RESOLVE_MODE_NONE ||
            a.resolveImageView != VK_NULL_HANDLE) {
            ok = false;
            return;
        }
        c.args_i64.push_back(static_cast<long long>(a.imageLayout));
        c.args_i64.push_back(static_cast<long long>(a.loadOp));
        c.args_i64.push_back(static_cast<long long>(a.storeOp));
        c.args_u64.push_back(from_handle(a.imageView));
        c.args_blob.append(reinterpret_cast<const char*>(&a.clearValue), sizeof(VkClearValue));
    };
    for (std::uint32_t i = 0; i < ri.colorAttachmentCount; ++i) {
        push_attachment(ri.pColorAttachments[i]);
    }
    if (has_depth) {
        push_attachment(*ri.pDepthAttachment);
    }
    if (has_stencil) {
        push_attachment(*ri.pStencilAttachment);
    }
    if (!ok) {
        cb->local_invalid = true;
        cb->invalid_reason = "begin_rendering MSAA resolve / attachment pNext not supported";
        return;
    }
    cb->recording.push_back(std::move(c));
}
VKAPI_ATTR void VKAPI_CALL CmdEndRendering(VkCommandBuffer commandBuffer) {
    auto* cb = reinterpret_cast<CommandBufferImpl*>(commandBuffer);
    vkrpc::RecordedCommand c;
    c.kind = "end_rendering";
    cb->recording.push_back(std::move(c));
}

VKAPI_ATTR void VKAPI_CALL CmdDraw(VkCommandBuffer commandBuffer, std::uint32_t vertexCount,
                                   std::uint32_t instanceCount, std::uint32_t firstVertex,
                                   std::uint32_t firstInstance) {
    auto* cb = reinterpret_cast<CommandBufferImpl*>(commandBuffer);
    vkrpc::RecordedCommand c;
    c.kind = "draw";
    c.vertex_count = static_cast<long long>(vertexCount);
    c.instance_count = static_cast<long long>(instanceCount);
    c.first_vertex = static_cast<long long>(firstVertex);
    c.first_instance = static_cast<long long>(firstInstance);
    cb->recording.push_back(std::move(c));
}

// (GL/zink): indexed draws. bind_index_buffer args_u64=[buffer, offset],
// args_i64=[indexType].
VKAPI_ATTR void VKAPI_CALL CmdBindIndexBuffer(VkCommandBuffer commandBuffer, VkBuffer buffer,
                                              VkDeviceSize offset, VkIndexType indexType) {
    auto* cb = reinterpret_cast<CommandBufferImpl*>(commandBuffer);
    vkrpc::RecordedCommand c;
    c.kind = "bind_index_buffer";
    c.args_u64.push_back(from_handle(buffer));
    c.args_u64.push_back(static_cast<std::uint64_t>(offset));
    c.args_i64.push_back(static_cast<long long>(indexType));
    cb->recording.push_back(std::move(c));
}

// draw_indexed args_i64=[indexCount, instanceCount, firstIndex, vertexOffset, firstInstance].
VKAPI_ATTR void VKAPI_CALL CmdDrawIndexed(VkCommandBuffer commandBuffer, std::uint32_t indexCount,
                                          std::uint32_t instanceCount, std::uint32_t firstIndex,
                                          std::int32_t vertexOffset, std::uint32_t firstInstance) {
    auto* cb = reinterpret_cast<CommandBufferImpl*>(commandBuffer);
    vkrpc::RecordedCommand c;
    c.kind = "draw_indexed";
    c.args_i64.push_back(static_cast<long long>(indexCount));
    c.args_i64.push_back(static_cast<long long>(instanceCount));
    c.args_i64.push_back(static_cast<long long>(firstIndex));
    c.args_i64.push_back(static_cast<long long>(vertexOffset));
    c.args_i64.push_back(static_cast<long long>(firstInstance));
    cb->recording.push_back(std::move(c));
}

// Core-1.0 indirect draws. The worker-vocabulary gate prevents a new ICD from sending unknown
// record kinds to an older worker; the shared structural predicate is re-run by both backends
// against their authoritative object tables before any host command is emitted.
void record_core_indirect_draw(VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset,
                               std::uint32_t drawCount, std::uint32_t stride, bool indexed) {
    auto* cb = reinterpret_cast<CommandBufferImpl*>(commandBuffer);
    const char* why = "";
    if (!vkr::icd_subset::core_indirect_worker_ok(cb->worker_core_indirect_draw, &why)) {
        cb->local_invalid = true;
        cb->invalid_reason = why;
        return;
    }
    const std::uint64_t handle = from_handle(buffer);
    BufferInfo info;
    bool live = false;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        const auto it = g_buffers.find(handle);
        live = it != g_buffers.end();
        if (live) {
            info = it->second;
        }
    }
    const bool bound = live && info.memory != 0;
    if (!vkr::icd_subset::draw_indirect_ok(live, bound, info.usage, info.size, offset, drawCount,
                                           stride, indexed, cb->multi_draw_indirect_enabled,
                                           &why)) {
        cb->local_invalid = true;
        cb->invalid_reason = why;
        return;
    }
    cb->recording.push_back(vkrpc::make_core_indirect_draw_command(
        handle, static_cast<std::uint64_t>(offset), drawCount, stride, indexed,
        cb->worker_core_indirect_draw_scalar_payload));
}

VKAPI_ATTR void VKAPI_CALL CmdDrawIndirect(VkCommandBuffer commandBuffer, VkBuffer buffer,
                                           VkDeviceSize offset, std::uint32_t drawCount,
                                           std::uint32_t stride) {
    record_core_indirect_draw(commandBuffer, buffer, offset, drawCount, stride, false);
}

VKAPI_ATTR void VKAPI_CALL CmdDrawIndexedIndirect(VkCommandBuffer commandBuffer, VkBuffer buffer,
                                                  VkDeviceSize offset, std::uint32_t drawCount,
                                                  std::uint32_t stride) {
    record_core_indirect_draw(commandBuffer, buffer, offset, drawCount, stride, true);
}

// Vulkan-1.2 / VK_KHR_draw_indirect_count. Both public spellings share this recorder. The
// worker-vocabulary and enabled-device facts are stamped into the command buffer at allocation,
// allowing the void entry points to fail locally and deterministically.
void record_core_indirect_count_draw(VkCommandBuffer commandBuffer, VkBuffer buffer,
                                     VkDeviceSize offset, VkBuffer countBuffer,
                                     VkDeviceSize countBufferOffset, std::uint32_t maxDrawCount,
                                     std::uint32_t stride, bool indexed) {
    auto* cb = reinterpret_cast<CommandBufferImpl*>(commandBuffer);
    const char* why = "";
    if (!vkr::icd_subset::core_indirect_count_worker_ok(cb->worker_core_indirect_draw_count,
                                                        &why)) {
        cb->local_invalid = true;
        cb->invalid_reason = why;
        return;
    }
    const std::uint64_t handle = from_handle(buffer);
    const std::uint64_t count_handle = from_handle(countBuffer);
    BufferInfo info;
    BufferInfo count_info;
    bool live = false;
    bool count_live = false;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        const auto it = g_buffers.find(handle);
        live = it != g_buffers.end();
        if (live) {
            info = it->second;
        }
        const auto count_it = g_buffers.find(count_handle);
        count_live = count_it != g_buffers.end();
        if (count_live) {
            count_info = count_it->second;
        }
    }
    if (!vkr::icd_subset::draw_indirect_count_ok(
            cb->draw_indirect_count_enabled, live, live && info.memory != 0, info.usage, info.size,
            count_live, count_live && count_info.memory != 0, count_info.usage, count_info.size,
            offset, countBufferOffset, maxDrawCount, stride, indexed, &why)) {
        cb->local_invalid = true;
        cb->invalid_reason = why;
        return;
    }
    cb->recording.push_back(vkrpc::make_core_indirect_count_draw_command(
        handle, static_cast<std::uint64_t>(offset), count_handle,
        static_cast<std::uint64_t>(countBufferOffset), maxDrawCount, stride, indexed));
}

VKAPI_ATTR void VKAPI_CALL CmdDrawIndirectCount(VkCommandBuffer commandBuffer, VkBuffer buffer,
                                                VkDeviceSize offset, VkBuffer countBuffer,
                                                VkDeviceSize countBufferOffset,
                                                std::uint32_t maxDrawCount, std::uint32_t stride) {
    record_core_indirect_count_draw(commandBuffer, buffer, offset, countBuffer, countBufferOffset,
                                    maxDrawCount, stride, false);
}

VKAPI_ATTR void VKAPI_CALL CmdDrawIndexedIndirectCount(
    VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset, VkBuffer countBuffer,
    VkDeviceSize countBufferOffset, std::uint32_t maxDrawCount, std::uint32_t stride) {
    record_core_indirect_count_draw(commandBuffer, buffer, offset, countBuffer, countBufferOffset,
                                    maxDrawCount, stride, true);
}

// --- (GL/zink): VK_EXT_transform_feedback + VK_EXT_conditional_rendering ----------------
// The two extensions gating zink's OpenGL 3.0 (see kDeviceExtAllowlist). zink only emits these
// when the app actually drives GL transform feedback / conditional rendering; the steady-state
// command stream is unchanged. All plain data + buffer handles, recorded faithfully and
// validated worker-side (mock == real).

// bind_transform_feedback_buffers: args_i64=[firstBinding, bindingCount, hasSizes];
// args_u64=[buffer x N, offset x N, size x N (only when hasSizes=1; VK_WHOLE_SIZE rides raw)].
VKAPI_ATTR void VKAPI_CALL CmdBindTransformFeedbackBuffersEXT(
    VkCommandBuffer commandBuffer, std::uint32_t firstBinding, std::uint32_t bindingCount,
    const VkBuffer* pBuffers, const VkDeviceSize* pOffsets, const VkDeviceSize* pSizes) {
    auto* cb = reinterpret_cast<CommandBufferImpl*>(commandBuffer);
    vkrpc::RecordedCommand c;
    c.kind = "bind_transform_feedback_buffers";
    c.args_i64.push_back(static_cast<long long>(firstBinding));
    c.args_i64.push_back(static_cast<long long>(bindingCount));
    c.args_i64.push_back(pSizes != nullptr ? 1 : 0);
    for (std::uint32_t i = 0; i < bindingCount; ++i) {
        c.args_u64.push_back(from_handle(pBuffers[i]));
    }
    for (std::uint32_t i = 0; i < bindingCount; ++i) {
        c.args_u64.push_back(static_cast<std::uint64_t>(pOffsets[i]));
    }
    if (pSizes != nullptr) {
        for (std::uint32_t i = 0; i < bindingCount; ++i) {
            c.args_u64.push_back(static_cast<std::uint64_t>(pSizes[i]));
        }
    }
    cb->recording.push_back(std::move(c));
}

// begin/end_transform_feedback share one payload shape: args_i64=[firstCounterBuffer,
// counterBufferCount, hasOffsets]; args_u64=[counterBuffer x N (0 = VK_NULL_HANDLE = no counter),
// offset x N (only when hasOffsets=1)]. counterBufferCount may be 0 (no counters).
void record_transform_feedback_scope(VkCommandBuffer commandBuffer, const char* kind,
                                     std::uint32_t firstCounterBuffer,
                                     std::uint32_t counterBufferCount,
                                     const VkBuffer* pCounterBuffers,
                                     const VkDeviceSize* pCounterBufferOffsets) {
    auto* cb = reinterpret_cast<CommandBufferImpl*>(commandBuffer);
    vkrpc::RecordedCommand c;
    c.kind = kind;
    c.args_i64.push_back(static_cast<long long>(firstCounterBuffer));
    c.args_i64.push_back(static_cast<long long>(counterBufferCount));
    c.args_i64.push_back(pCounterBufferOffsets != nullptr ? 1 : 0);
    for (std::uint32_t i = 0; i < counterBufferCount; ++i) {
        c.args_u64.push_back(pCounterBuffers != nullptr ? from_handle(pCounterBuffers[i]) : 0);
    }
    if (pCounterBufferOffsets != nullptr) {
        for (std::uint32_t i = 0; i < counterBufferCount; ++i) {
            c.args_u64.push_back(static_cast<std::uint64_t>(pCounterBufferOffsets[i]));
        }
    }
    cb->recording.push_back(std::move(c));
}

VKAPI_ATTR void VKAPI_CALL CmdBeginTransformFeedbackEXT(VkCommandBuffer commandBuffer,
                                                        std::uint32_t firstCounterBuffer,
                                                        std::uint32_t counterBufferCount,
                                                        const VkBuffer* pCounterBuffers,
                                                        const VkDeviceSize* pCounterBufferOffsets) {
    record_transform_feedback_scope(commandBuffer, "begin_transform_feedback", firstCounterBuffer,
                                    counterBufferCount, pCounterBuffers, pCounterBufferOffsets);
}

VKAPI_ATTR void VKAPI_CALL CmdEndTransformFeedbackEXT(VkCommandBuffer commandBuffer,
                                                      std::uint32_t firstCounterBuffer,
                                                      std::uint32_t counterBufferCount,
                                                      const VkBuffer* pCounterBuffers,
                                                      const VkDeviceSize* pCounterBufferOffsets) {
    record_transform_feedback_scope(commandBuffer, "end_transform_feedback", firstCounterBuffer,
                                    counterBufferCount, pCounterBuffers, pCounterBufferOffsets);
}

// draw_indirect_byte_count (glDrawTransformFeedback: vertex count read from the counter buffer on
// the GPU): args_u64=[counterBuffer]; args_i64=[instanceCount, firstInstance,
// counterBufferOffset, counterOffset, vertexStride]. Draw-readiness is validated worker-side
// exactly like draw/draw_indexed.
VKAPI_ATTR void VKAPI_CALL CmdDrawIndirectByteCountEXT(
    VkCommandBuffer commandBuffer, std::uint32_t instanceCount, std::uint32_t firstInstance,
    VkBuffer counterBuffer, VkDeviceSize counterBufferOffset, std::uint32_t counterOffset,
    std::uint32_t vertexStride) {
    auto* cb = reinterpret_cast<CommandBufferImpl*>(commandBuffer);
    vkrpc::RecordedCommand c;
    c.kind = "draw_indirect_byte_count";
    c.args_u64.push_back(from_handle(counterBuffer));
    c.args_i64.push_back(static_cast<long long>(instanceCount));
    c.args_i64.push_back(static_cast<long long>(firstInstance));
    c.args_i64.push_back(static_cast<long long>(counterBufferOffset));
    c.args_i64.push_back(static_cast<long long>(counterOffset));
    c.args_i64.push_back(static_cast<long long>(vertexStride));
    cb->recording.push_back(std::move(c));
}

// begin_conditional_rendering (GL 3.0 glBeginConditionalRender): args_u64=[buffer];
// args_i64=[offset, flags]. end_conditional_rendering carries no args. Balance + scope symmetry
// (begun-outside ends outside, begun-inside ends in the SAME render pass) are validated
// worker-side.
VKAPI_ATTR void VKAPI_CALL CmdBeginConditionalRenderingEXT(
    VkCommandBuffer commandBuffer,
    const VkConditionalRenderingBeginInfoEXT* pConditionalRenderingBegin) {
    auto* cb = reinterpret_cast<CommandBufferImpl*>(commandBuffer);
    vkrpc::RecordedCommand c;
    c.kind = "begin_conditional_rendering";
    if (pConditionalRenderingBegin != nullptr) {
        c.args_u64.push_back(from_handle(pConditionalRenderingBegin->buffer));
        c.args_i64.push_back(static_cast<long long>(pConditionalRenderingBegin->offset));
        c.args_i64.push_back(static_cast<long long>(pConditionalRenderingBegin->flags));
    }
    cb->recording.push_back(std::move(c));
}

VKAPI_ATTR void VKAPI_CALL CmdEndConditionalRenderingEXT(VkCommandBuffer commandBuffer) {
    auto* cb = reinterpret_cast<CommandBufferImpl*>(commandBuffer);
    vkrpc::RecordedCommand c;
    c.kind = "end_conditional_rendering";
    cb->recording.push_back(std::move(c));
}

// (GL/zink): push constants. desc_layout = pipeline layout; args_i64=[stageFlags, offset,
// size]; args_blob = the raw constant bytes. zink uses push constants for its shader uniforms.
VKAPI_ATTR void VKAPI_CALL CmdPushConstants(VkCommandBuffer commandBuffer, VkPipelineLayout layout,
                                            VkShaderStageFlags stageFlags, std::uint32_t offset,
                                            std::uint32_t size, const void* pValues) {
    auto* cb = reinterpret_cast<CommandBufferImpl*>(commandBuffer);
    vkrpc::RecordedCommand c;
    c.kind = "push_constants";
    c.desc_layout = from_handle(layout);
    c.args_i64.push_back(static_cast<long long>(stageFlags));
    c.args_i64.push_back(static_cast<long long>(offset));
    c.args_i64.push_back(static_cast<long long>(size));
    if (pValues != nullptr && size > 0) {
        c.args_blob.assign(static_cast<const char*>(pValues), size);
    }
    cb->recording.push_back(std::move(c));
}

VKAPI_ATTR void VKAPI_CALL CmdBindVertexBuffers(VkCommandBuffer commandBuffer,
                                                std::uint32_t firstBinding,
                                                std::uint32_t bindingCount,
                                                const VkBuffer* pBuffers,
                                                const VkDeviceSize* pOffsets) {
    auto* cb = reinterpret_cast<CommandBufferImpl*>(commandBuffer);
    const char* why = "";
    if (!vkr::icd_subset::bind_vertex_buffers_ok(firstBinding, bindingCount, pBuffers, pOffsets,
                                                 &why)) {
        cb->local_invalid = true;
        cb->invalid_reason = why;
        return;
    }
    vkrpc::RecordedCommand c;
    c.kind = "bind_vertex_buffers";
    c.first_binding = static_cast<int>(firstBinding);
    for (std::uint32_t i = 0; i < bindingCount; ++i) {
        c.vertex_buffers.push_back(from_handle(pBuffers[i]));
        c.vertex_buffer_offsets.push_back(static_cast<std::uint64_t>(pOffsets[i]));
    }
    cb->recording.push_back(std::move(c));
}

VKAPI_ATTR void VKAPI_CALL CmdBindDescriptorSets(VkCommandBuffer commandBuffer,
                                                 VkPipelineBindPoint bindPoint,
                                                 VkPipelineLayout layout, std::uint32_t firstSet,
                                                 std::uint32_t descriptorSetCount,
                                                 const VkDescriptorSet* pDescriptorSets,
                                                 std::uint32_t dynamicOffsetCount,
                                                 const std::uint32_t* pDynamicOffsets) {
    auto* cb = reinterpret_cast<CommandBufferImpl*>(commandBuffer);
    const char* why = "";
    if (!vkr::icd_subset::bind_descriptor_sets_ok(bindPoint, firstSet, descriptorSetCount,
                                                  dynamicOffsetCount, &why)) {
        cb->local_invalid = true;
        cb->invalid_reason = why;
        return;
    }
    if (pDescriptorSets == nullptr) {
        cb->local_invalid = true;
        cb->invalid_reason = "bind_descriptor_sets has a null descriptor-set array";
        return;
    }
    // (GL/zink): faithful forwarding. args_i64[0] = bindPoint; args_u64 = the dynamic
    // offsets (in binding order). The worker resolves the layout + each set and emits
    // vkCmdBindDescriptorSets verbatim; the real driver is the binding-correctness authority.
    vkrpc::RecordedCommand c;
    c.kind = "bind_descriptor_sets";
    c.desc_layout = from_handle(layout);
    c.first_set = static_cast<int>(firstSet);
    c.args_i64.push_back(static_cast<long long>(bindPoint));
    for (std::uint32_t i = 0; i < descriptorSetCount; ++i) {
        c.descriptor_sets.push_back(from_handle(pDescriptorSets[i]));
    }
    if (pDynamicOffsets != nullptr) {
        for (std::uint32_t i = 0; i < dynamicOffsetCount; ++i) {
            c.args_u64.push_back(static_cast<std::uint64_t>(pDynamicOffsets[i]));
        }
    }
    cb->recording.push_back(std::move(c));
}

VKAPI_ATTR VkResult VKAPI_CALL EndCommandBuffer(VkCommandBuffer commandBuffer) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto* cb = reinterpret_cast<CommandBufferImpl*>(commandBuffer);
    // A vkCmd* outside the subset marked the buffer locally invalid; fail closed here rather
    // than forward a silently-canonicalized stream.
    if (cb->local_invalid) {
        std::fprintf(stderr, "vkrelay2-icd: command buffer rejected -- %s\n", cb->invalid_reason);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    try {
        vkrpc::RecordCommandBufferRequest req;
        req.command_buffer = cb->worker;
        req.one_time_submit = cb->one_time_submit;
        req.commands = cb->recording;
        const std::uint32_t request_id = next_id();
        if (icd_trace_enabled()) {
            const std::string wire = req.to_wire();
            icd_trace("RecordCommandBuffer request=%u cb=%llu commands=%zu bytes=%zu hash=0x%llx",
                      request_id, static_cast<unsigned long long>(req.command_buffer),
                      req.commands.size(), wire.size(),
                      static_cast<unsigned long long>(trace_hash_bytes(wire.data(), wire.size())));
        }
        // the binary body when the worker advertised it (94-97% of the JSON
        // record handler was parse+hex); JSON when not (old worker / VKRELAY2_NO_RAW_RECORD=1).
        const vkrpc::StatusResponse r =
            g_raw_record ? vkrpc::record_command_buffer_raw(*g_rpc, request_id, req)
                         : vkrpc::record_command_buffer(*g_rpc, request_id, req);
        if (!r.ok) {
            std::fprintf(stderr, "vkrelay2-icd: record_command_buffer rejected -- %s\n",
                         r.reason.c_str());
        }
        return r.ok ? VK_SUCCESS : fault();
    } catch (const std::exception&) {
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
}

VKAPI_ATTR VkResult VKAPI_CALL AcquireNextImageKHR(VkDevice, VkSwapchainKHR swapchain,
                                                   std::uint64_t timeout, VkSemaphore semaphore,
                                                   VkFence fence, std::uint32_t* pImageIndex) {
    std::lock_guard<std::mutex> lk(g_mu);
    try {
        vkrpc::AcquireNextImageRequest req;
        req.swapchain = from_handle(swapchain);
        req.timeout = timeout;
        req.semaphore = semaphore == VK_NULL_HANDLE ? 0 : from_handle(semaphore);
        req.fence = fence == VK_NULL_HANDLE ? 0 : from_handle(fence);
        const vkrpc::AcquireNextImageResponse r = vkrpc::acquire_next_image(*g_rpc, next_id(), req);
        if (!r.ok) {
            caps_cache().invalidate_swapchain(req.swapchain); // fault touching the surface
            return fault();
        }
        // rule: ANY non-success acquire result (SUBOPTIMAL / OUT_OF_DATE / errors) is
        // the honest resize/geometry signal -- the next caps poll must re-query the worker.
        if (r.result != vkrpc::kVkSuccess) {
            caps_cache().invalidate_swapchain(req.swapchain);
        }
        if (pImageIndex != nullptr) {
            *pImageIndex = static_cast<std::uint32_t>(r.image_index);
        }
        return static_cast<VkResult>(r.result);
    } catch (const std::exception&) {
        caps_cache().invalidate_swapchain(from_handle(swapchain)); // before OUT_OF_DATE
        return VK_ERROR_OUT_OF_DATE_KHR;
    }
}

// Coherent-flush-at-submit: ship every mapped allocation's dirty chunks BEFORE any
// child submit, so the GPU never reads stale bytes from a persistently-mapped, never-explicitly-
// flushed coherent buffer. A failed upload fails the submit (commit only on ack, so the bytes
// re-send next time); the whole sweep ships once up front (the app cannot legally interleave new
// mapped writes inside one vkQueueSubmit / vkQueueSubmit2). Caller holds g_mu; returns false on a
// ship failure (the caller returns fault). Shared by QueueSubmit + QueueSubmit2 (
// submit2 runs the SAME sweep once before its single submit2 RPC).
bool coherent_flush_at_submit_locked() {
    // The sweep's diff cost lives OUTSIDE every RPC op, so the op table cannot
    // see it -- the upload_sweep profile line does. Off-path stays free of clock reads.
    vkrpc::RpcProfile* prof = vkrpc::profile_if_enabled();
    const std::uint64_t sweep_t0 = prof != nullptr ? vkrpc::profile_clock_us() : 0;
    // The soft-dirty pre-filter: self-tested ONCE at the first sweep (never trust kernel config
    // alone); any reset failure disables it permanently -- full diff.
    if (!g_softdirty_tried) {
        g_softdirty_tried = true;
        g_softdirty_on = std::getenv("VKRELAY2_NO_SOFT_DIRTY") == nullptr && g_softdirty.init();
        icd_trace("upload sweep: soft-dirty page filter %s",
                  g_softdirty_on ? "ENABLED" : "unavailable -- full-diff sweeps");
    }
    // The race-free epoch protocol: a soft-dirty reset happens ONLY here, immediately followed by
    // force_rebaseline -> this very sweep full-diffs everything, so no write can hide between a
    // pagemap read and a reset. Sweep 0 is the first baseline; every 64th re-baselines to shed
    // pages that were written once and never again (the kernel bit is sticky until reset).
    if (g_softdirty_on && g_sweep_counter % sweep_rebaseline_every() == 0) {
        if (g_softdirty.reset()) {
            g_tracker.force_rebaseline();
        } else {
            g_softdirty_on = false; // fail closed: full diffs from here on
            icd_trace("upload sweep: soft-dirty reset FAILED -- filter disabled");
        }
    }
    ++g_sweep_counter;
    vkr::icd_mem::PageFilter filter;
    if (g_softdirty_on) {
        filter = [](const std::byte* base, std::uint64_t size,
                    std::vector<vkr::icd_mem::Range>& out) {
            return g_softdirty.read_dirty(base, size, out);
        };
    }
    vkr::icd_mem::SweepIo io;
    const vkr::icd_mem::DirtySnapshot snap = g_tracker.snapshot_pending(filter, &io);
    if (prof != nullptr) {
        prof->upload_sweep.count += 1;
        prof->upload_sweep.scan_bytes += io.eligible_bytes;
        prof->upload_sweep.filtered_bytes += io.diffed_bytes;
        prof->upload_sweep.payload_bytes += snap.byte_count();
        prof->upload_sweep.us += vkrpc::profile_clock_us() - sweep_t0;
    }
    if (!snap.empty() && !ship_snapshot(snap)) {
        // a FAILED upload after a baseline sweep would otherwise go silently
        // stale -- the baseline's soft-dirty reset already consumed the kernel bits. Force fresh
        // full-diff baselines (flags only) so the retry contract holds under the epoch scheme.
        if (g_softdirty_on) {
            g_tracker.force_rebaseline();
        }
        return false;
    }
    return true;
}

VKAPI_ATTR VkResult VKAPI_CALL QueueSubmit(VkQueue queue, std::uint32_t submitCount,
                                           const VkSubmitInfo* pSubmits, VkFence fence) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto* q = reinterpret_cast<QueueImpl*>(queue);
    try {
        if (!coherent_flush_at_submit_locked()) {
            return fault(); // a dirty-upload failure fails the submit before any child submit
        }
        // One queue_submit RPC per VkSubmitInfo; the fence rides on the last one (Vulkan
        // signals the fence after all submitted batches complete). The clear canary uses 1.
        for (std::uint32_t s = 0; s < submitCount; ++s) {
            const VkSubmitInfo& si = pSubmits[s];
            vkrpc::QueueSubmitRequest req;
            req.queue = q->worker;
            // (GL/zink): timeline wait/signal values ride a VkTimelineSemaphoreSubmitInfo
            // in the submit's pNext (zink chains it). Carry them parallel to the wait/signal
            // arrays; a binary semaphore's slot is ignored.
            const VkTimelineSemaphoreSubmitInfo* tsi = nullptr;
            for (auto* n = static_cast<const VkBaseInStructure*>(si.pNext); n != nullptr;
                 n = n->pNext) {
                if (n->sType == VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO) {
                    tsi = reinterpret_cast<const VkTimelineSemaphoreSubmitInfo*>(n);
                }
            }
            for (std::uint32_t i = 0; i < si.waitSemaphoreCount; ++i) {
                vkrpc::SubmitWait w;
                w.semaphore = from_handle(si.pWaitSemaphores[i]);
                w.stage = si.pWaitDstStageMask != nullptr
                              ? static_cast<long long>(si.pWaitDstStageMask[i])
                              : 0;
                w.value = (tsi != nullptr && tsi->pWaitSemaphoreValues != nullptr &&
                           i < tsi->waitSemaphoreValueCount)
                              ? tsi->pWaitSemaphoreValues[i]
                              : 0;
                req.waits.push_back(w);
            }
            for (std::uint32_t i = 0; i < si.commandBufferCount; ++i) {
                auto* cb = reinterpret_cast<CommandBufferImpl*>(si.pCommandBuffers[i]);
                req.command_buffers.push_back(cb->worker);
            }
            for (std::uint32_t i = 0; i < si.signalSemaphoreCount; ++i) {
                req.signal_semaphores.push_back(from_handle(si.pSignalSemaphores[i]));
                req.signal_values.push_back((tsi != nullptr &&
                                             tsi->pSignalSemaphoreValues != nullptr &&
                                             i < tsi->signalSemaphoreValueCount)
                                                ? tsi->pSignalSemaphoreValues[i]
                                                : 0);
            }
            const bool last = (s + 1 == submitCount);
            req.fence = (last && fence != VK_NULL_HANDLE) ? from_handle(fence) : 0;
            const vkrpc::QueueSubmitResponse r = vkrpc::queue_submit(*g_rpc, next_id(), req);
            if (!r.ok) {
                return fault();
            }
            if (r.result != vkrpc::kVkSuccess) {
                return static_cast<VkResult>(r.result);
            }
            // ARM this batch's readback destinations now that the worker
            // accepted the submit -- never at record time. The record carries the batch's
            // completion proofs: the CALL's fence (Vulkan signals it after ALL the call's batches
            // complete, so it proves every batch) and this batch's TIMELINE signal values (a
            // binary signal is not host-waitable and is skipped). A later PROVEN sync point
            // promotes exactly this work to downloadable.
            //
            // Queue order: these same proofs also prove every EARLIER submit on
            // this queue (a signal operation's first sync scope includes all prior submission-
            // order work, spec 7.3.1/7.4.1) -- append them to every older same-queue record, so a
            // readback batch submitted with NO proof of its own is promoted by a later fence-only
            // or timeline-signalling submit, not only by an idle wait. This runs for EVERY
            // accepted batch (dsts or not); a record is created only when the batch carries dsts.
            {
                std::vector<std::pair<std::uint64_t, std::uint64_t>> signals;
                for (std::size_t i = 0; i < req.signal_semaphores.size(); ++i) {
                    if (req.signal_values[i] > 0) {
                        signals.emplace_back(req.signal_semaphores[i], req.signal_values[i]);
                    }
                }
                const std::uint64_t call_fence = fence != VK_NULL_HANDLE ? from_handle(fence) : 0;
                for (ReadbackSubmission& old : g_readback_submitted) {
                    if (old.queue != q->worker) {
                        continue;
                    }
                    // Once per call (last batch), and deduplicated: a record
                    // created by an EARLIER batch of this same call already carries the call
                    // fence; promotion is idempotent either way, this just keeps records tidy.
                    if (call_fence != 0 && last &&
                        std::find(old.fences.begin(), old.fences.end(), call_fence) ==
                            old.fences.end()) {
                        old.fences.push_back(call_fence);
                    }
                    old.signals.insert(old.signals.end(), signals.begin(), signals.end());
                }
                ReadbackSubmission sub;
                for (std::uint32_t i = 0; i < si.commandBufferCount; ++i) {
                    auto* cb = reinterpret_cast<CommandBufferImpl*>(si.pCommandBuffers[i]);
                    sub.dsts.insert(sub.dsts.end(), cb->readback_dsts.begin(),
                                    cb->readback_dsts.end());
                }
                if (!sub.dsts.empty()) {
                    sub.queue = q->worker;
                    if (call_fence != 0) {
                        sub.fences.push_back(call_fence);
                    }
                    sub.signals = std::move(signals);
                    g_readback_submitted.push_back(std::move(sub));
                }
            }
        }
        // A fence-only submit (submitCount == 0) still must signal the fence.
        if (submitCount == 0 && fence != VK_NULL_HANDLE) {
            vkrpc::QueueSubmitRequest req;
            req.queue = q->worker;
            req.fence = from_handle(fence);
            const vkrpc::QueueSubmitResponse r = vkrpc::queue_submit(*g_rpc, next_id(), req);
            if (!r.ok) {
                return fault();
            }
            if (r.result == vkrpc::kVkSuccess) {
                // Queue order: this fence signals after all EARLIER work on the
                // queue, so it is a completion proof for every armed same-queue readback.
                for (ReadbackSubmission& old : g_readback_submitted) {
                    if (old.queue == q->worker) {
                        old.fences.push_back(from_handle(fence));
                    }
                }
            }
            return static_cast<VkResult>(r.result);
        }
        return VK_SUCCESS;
    } catch (const std::exception&) {
        // an exception can unwind AFTER the sweep's soft-dirty reset
        // consumed the kernel bits but BEFORE every snapshot frame was acknowledged -- same
        // stale-retry hole as the ship failure above. Same cure: fresh full-diff baselines.
        if (g_softdirty_on) {
            g_tracker.force_rebaseline();
        }
        return VK_ERROR_DEVICE_LOST;
    }
}

// vkQueueSubmit2. The WHOLE VkSubmitInfo2[] batch + one fence in ONE RPC (one
// native pfn_queue_submit2 call), preserving the sync1 readback-proof bookkeeping:
// the sweep runs once up front; after the RPC each submit arms its command buffers' readback
// dsts; the call fence proves every submit; each submit's TIMELINE signal values prove that submit
// and all earlier submission-order work. Fail-closed on device-group / flags / pNext.
VKAPI_ATTR VkResult VKAPI_CALL QueueSubmit2(VkQueue queue, std::uint32_t submitCount,
                                            const VkSubmitInfo2* pSubmits, VkFence fence) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto* q = reinterpret_cast<QueueImpl*>(queue);
    try {
        if (submitCount > vkrpc::kMaxSync2SubmitInfos) {
            return fault();
        }
        if (!coherent_flush_at_submit_locked()) {
            return fault();
        }
        // Build the whole batch, rejecting every unserved field before any deref.
        if (submitCount != 0 && pSubmits == nullptr) {
            return fault(); // null/count fail-closed parsing
        }
        vkrpc::QueueSubmit2Request req;
        req.queue = q->worker;
        for (std::uint32_t s = 0; s < submitCount; ++s) {
            const VkSubmitInfo2& si = pSubmits[s];
            if (si.pNext != nullptr || si.flags != 0) {
                return fault(); // no submit pNext / protected-or-other submit flags
            }
            if ((si.waitSemaphoreInfoCount != 0 && si.pWaitSemaphoreInfos == nullptr) ||
                (si.commandBufferInfoCount != 0 && si.pCommandBufferInfos == nullptr) ||
                (si.signalSemaphoreInfoCount != 0 && si.pSignalSemaphoreInfos == nullptr) ||
                si.waitSemaphoreInfoCount > vkrpc::kMaxSync2SemaphoresPerSubmit ||
                si.signalSemaphoreInfoCount > vkrpc::kMaxSync2SemaphoresPerSubmit ||
                si.commandBufferInfoCount > vkrpc::kMaxSync2CommandBuffersPerSubmit) {
                return fault();
            }
            vkrpc::SubmitInfo2 out;
            for (std::uint32_t i = 0; i < si.waitSemaphoreInfoCount; ++i) {
                const VkSemaphoreSubmitInfo& w = si.pWaitSemaphoreInfos[i];
                if (w.pNext != nullptr || w.deviceIndex != 0) {
                    return fault();
                }
                out.waits.push_back(
                    {from_handle(w.semaphore), w.value, static_cast<std::uint64_t>(w.stageMask)});
            }
            for (std::uint32_t i = 0; i < si.commandBufferInfoCount; ++i) {
                const VkCommandBufferSubmitInfo& c = si.pCommandBufferInfos[i];
                // deviceMask 0 (= "all", the single device) or 1 (bit 0) both mean the one device.
                if (c.pNext != nullptr || (c.deviceMask != 0 && c.deviceMask != 1)) {
                    return fault();
                }
                auto* cb = reinterpret_cast<CommandBufferImpl*>(c.commandBuffer);
                out.command_buffers.push_back(cb->worker);
            }
            for (std::uint32_t i = 0; i < si.signalSemaphoreInfoCount; ++i) {
                const VkSemaphoreSubmitInfo& sg = si.pSignalSemaphoreInfos[i];
                if (sg.pNext != nullptr || sg.deviceIndex != 0) {
                    return fault();
                }
                out.signals.push_back({from_handle(sg.semaphore), sg.value,
                                       static_cast<std::uint64_t>(sg.stageMask)});
            }
            req.submits.push_back(std::move(out));
        }
        req.fence = fence != VK_NULL_HANDLE ? from_handle(fence) : 0;
        const vkrpc::QueueSubmitResponse r = vkrpc::queue_submit2(*g_rpc, next_id(), req);
        if (!r.ok) {
            return fault();
        }
        if (r.result != vkrpc::kVkSuccess) {
            return static_cast<VkResult>(r.result);
        }
        // readback-proof bookkeeping, per submit IN ORDER (the sync1 QueueSubmit contract
        // adapted to the single-RPC batch). The call fence proves every submit; a submit's timeline
        // signals prove that submit + all earlier submission-order work on the queue.
        const std::uint64_t call_fence = fence != VK_NULL_HANDLE ? from_handle(fence) : 0;
        for (std::uint32_t s = 0; s < submitCount; ++s) {
            std::vector<std::pair<std::uint64_t, std::uint64_t>> signals;
            for (const vkrpc::SemaphoreSubmit2& sg : req.submits[s].signals) {
                if (sg.value > 0) { // binary signals are not host-waitable
                    signals.emplace_back(sg.semaphore, sg.value);
                }
            }
            const bool last = (s + 1 == submitCount);
            for (ReadbackSubmission& old : g_readback_submitted) {
                if (old.queue != q->worker) {
                    continue;
                }
                if (call_fence != 0 && last &&
                    std::find(old.fences.begin(), old.fences.end(), call_fence) ==
                        old.fences.end()) {
                    old.fences.push_back(call_fence);
                }
                old.signals.insert(old.signals.end(), signals.begin(), signals.end());
            }
            ReadbackSubmission sub;
            for (std::uint32_t i = 0; i < pSubmits[s].commandBufferInfoCount; ++i) {
                auto* cb = reinterpret_cast<CommandBufferImpl*>(
                    pSubmits[s].pCommandBufferInfos[i].commandBuffer);
                sub.dsts.insert(sub.dsts.end(), cb->readback_dsts.begin(), cb->readback_dsts.end());
            }
            if (!sub.dsts.empty()) {
                sub.queue = q->worker;
                if (call_fence != 0) {
                    sub.fences.push_back(call_fence);
                }
                sub.signals = std::move(signals);
                g_readback_submitted.push_back(std::move(sub));
            }
        }
        // A fence-only vkQueueSubmit2 (submitCount == 0, a fence) signals after all EARLIER work on
        // the queue, so it is a completion proof for every armed same-queue readback -- the
        // per-submit loop above never runs, so append it here explicitly (
        // the sync1 QueueSubmit fence-only path does the same).
        if (submitCount == 0 && call_fence != 0) {
            for (ReadbackSubmission& old : g_readback_submitted) {
                if (old.queue == q->worker) {
                    old.fences.push_back(call_fence);
                }
            }
        }
        return VK_SUCCESS;
    } catch (const std::exception&) {
        if (g_softdirty_on) {
            g_tracker.force_rebaseline();
        }
        return VK_ERROR_DEVICE_LOST;
    }
}

VKAPI_ATTR VkResult VKAPI_CALL QueuePresentKHR(VkQueue queue,
                                               const VkPresentInfoKHR* pPresentInfo) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto* q = reinterpret_cast<QueueImpl*>(queue);
    try {
        vkrpc::QueuePresentRequest req;
        req.queue = q->worker;
        for (std::uint32_t i = 0; i < pPresentInfo->waitSemaphoreCount; ++i) {
            req.wait_semaphores.push_back(from_handle(pPresentInfo->pWaitSemaphores[i]));
        }
        for (std::uint32_t i = 0; i < pPresentInfo->swapchainCount; ++i) {
            vkrpc::PresentEntry e;
            e.swapchain = from_handle(pPresentInfo->pSwapchains[i]);
            e.image_index = static_cast<int>(pPresentInfo->pImageIndices[i]);
            req.presents.push_back(e);
        }
        const vkrpc::QueuePresentResponse r = vkrpc::queue_present(*g_rpc, next_id(), req);
        // rule, per target: invalidate a presented target's surface if
        // the AGGREGATE result is non-success, OR that target's results[i] is non-success, OR
        // results is missing/short for it (fail safe) -- multi-swapchain present stays honest.
        // An RPC fault (!ok) invalidates every target, then faults as before.
        {
            const bool aggregate_bad = !r.ok || r.result != vkrpc::kVkSuccess;
            for (std::size_t i = 0; i < req.presents.size(); ++i) {
                if (aggregate_bad || i >= r.results.size() || r.results[i] != vkrpc::kVkSuccess) {
                    caps_cache().invalidate_swapchain(req.presents[i].swapchain);
                }
            }
        }
        if (!r.ok) {
            return fault();
        }
        if (pPresentInfo->pResults != nullptr) {
            for (std::uint32_t i = 0; i < pPresentInfo->swapchainCount && i < r.results.size();
                 ++i) {
                pPresentInfo->pResults[i] = static_cast<VkResult>(r.results[i]);
            }
        }
        return static_cast<VkResult>(r.result);
    } catch (const std::exception&) {
        // invalidate every presented target before returning OUT_OF_DATE.
        for (std::uint32_t i = 0; i < pPresentInfo->swapchainCount; ++i) {
            caps_cache().invalidate_swapchain(from_handle(pPresentInfo->pSwapchains[i]));
        }
        return VK_ERROR_OUT_OF_DATE_KHR;
    }
}

// Idle waits are REAL worker-side waits (they were local success stubs, but
// a readback-completion sync point must actually prove completion -- an idle-synced client would
// otherwise download before the GPU copy finished). vkDeviceWaitIdle / vkQueueWaitIdle run on the
// worker; success proves EVERYTHING submitted is complete, so every armed readback promotes.
VKAPI_ATTR VkResult VKAPI_CALL DeviceWaitIdle(VkDevice device) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto* dev = reinterpret_cast<DeviceImpl*>(device);
    try {
        vkrpc::HandleRequest req;
        req.handle = dev->worker;
        const vkrpc::WaitIdleResponse r = vkrpc::device_wait_idle(*g_rpc, next_id(), req);
        if (!r.ok) {
            return fault();
        }
        if (r.result == 0) { // VK_SUCCESS -> the whole device is idle
            promote_readbacks_all_locked();
            download_readback_locked();
        }
        return static_cast<VkResult>(r.result);
    } catch (const std::exception&) {
        return VK_ERROR_DEVICE_LOST;
    }
}
VKAPI_ATTR VkResult VKAPI_CALL QueueWaitIdle(VkQueue queue) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto* q = reinterpret_cast<QueueImpl*>(queue);
    try {
        vkrpc::HandleRequest req;
        req.handle = q->worker;
        const vkrpc::WaitIdleResponse r = vkrpc::queue_wait_idle(*g_rpc, next_id(), req);
        if (!r.ok) {
            return fault();
        }
        if (r.result == 0) { // VK_SUCCESS -> all of this queue's submitted work is complete.
            // Promoting ALL armed submissions assumes the single-graphics-queue relay (true today:
            // the worker exposes one graphics family and zink submits to one queue); a
            // multi-queue subset would need per-queue records.
            promote_readbacks_all_locked();
            download_readback_locked();
        }
        return static_cast<VkResult>(r.result);
    } catch (const std::exception&) {
        return VK_ERROR_DEVICE_LOST;
    }
}

// =====================================================================================
// Proc-addr dispatch (loader interface v3: physical-device functions answer through GIPA)
// =====================================================================================
// Forward declarations: get_proc takes their addresses; they route back to get_proc.
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetInstanceProcAddr(VkInstance, const char* pName);
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetDeviceProcAddr(VkDevice, const char* pName);

// =====================================================================================
// VK_EXT_debug_utils object annotations: accept-and-ignore IS the faithful implementation
// =====================================================================================
// The Khronos loader offers VK_EXT_debug_utils to every application regardless of driver support,
// and Mesa >= 25 zink names its objects unconditionally right after CreateDevice -- so these
// device-dispatchable entry points MUST resolve to something callable or every newer-Mesa GL app
// dies at device bring-up (a named abort-stub was exactly that death). Names, tags, and labels
// are tooling metadata with NO rendering semantics: accepting and dropping them approximates
// nothing, and returning VK_SUCCESS is what the spec allows for a driver that stores nothing.
// Purely local -- no RPC.
VKAPI_ATTR VkResult VKAPI_CALL SetDebugUtilsObjectNameEXT(VkDevice,
                                                          const VkDebugUtilsObjectNameInfoEXT*) {
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL SetDebugUtilsObjectTagEXT(VkDevice,
                                                         const VkDebugUtilsObjectTagInfoEXT*) {
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL QueueBeginDebugUtilsLabelEXT(VkQueue, const VkDebugUtilsLabelEXT*) {}
VKAPI_ATTR void VKAPI_CALL QueueEndDebugUtilsLabelEXT(VkQueue) {}
VKAPI_ATTR void VKAPI_CALL QueueInsertDebugUtilsLabelEXT(VkQueue, const VkDebugUtilsLabelEXT*) {}
VKAPI_ATTR void VKAPI_CALL CmdBeginDebugUtilsLabelEXT(VkCommandBuffer,
                                                      const VkDebugUtilsLabelEXT*) {}
VKAPI_ATTR void VKAPI_CALL CmdEndDebugUtilsLabelEXT(VkCommandBuffer) {}
VKAPI_ATTR void VKAPI_CALL CmdInsertDebugUtilsLabelEXT(VkCommandBuffer,
                                                       const VkDebugUtilsLabelEXT*) {}

// =====================================================================================
// localization: named abort-stubs for unimplemented device functions
// =====================================================================================
// A device function we do not wire is, from zink's perspective, a NULL slot in its dispatch table
// -- and the first call lands on address 0x0 with a stripped backtrace (no name). Instead of NULL,
// hand back a distinct named stub per requested function: the first *call* then traces the exact
// culprit and aborts cleanly. zink loads its dispatch table unconditionally but only *calls*
// functions whose feature/extension it believes enabled, so a stub fires precisely on a real,
// must-implement gap -- never on an optional probe. Fail-loud during bring-up, strictly better than
// a silent SIGSEGV in a stripped frame.
constexpr int kStubSlots = 512;
std::array<std::string, kStubSlots> g_stub_names;
int g_stub_count = 0;
std::mutex g_stub_mu;

template <int I> VKAPI_ATTR void VKAPI_CALL unimpl_device_stub() {
    // This is the terminal fail-closed edge for a core/device command that the relay does not
    // implement. Keep it unconditional: without VKRELAY2_ICD_TRACE the old trace-only message was
    // lost immediately to abort(), leaving the application with no actionable function name.
    std::fprintf(stderr, "vkrelay2-icd: unimplemented device function called: %s\n",
                 g_stub_names[I].c_str());
    std::fflush(stderr);
    std::abort();
}
template <int... I>
std::array<PFN_vkVoidFunction, sizeof...(I)> make_stub_table(std::integer_sequence<int, I...>) {
    return {reinterpret_cast<PFN_vkVoidFunction>(&unimpl_device_stub<I>)...};
}
const std::array<PFN_vkVoidFunction, kStubSlots> g_stub_table =
    make_stub_table(std::make_integer_sequence<int, kStubSlots>{});

PFN_vkVoidFunction unimpl_device_stub_for(const char* name) {
    std::lock_guard<std::mutex> lk(g_stub_mu);
    // Idempotent across repeat resolves: reuse the slot already minted for this name.
    for (int i = 0; i < g_stub_count; ++i) {
        if (g_stub_names[i] == name)
            return g_stub_table[i];
    }
    if (g_stub_count >= kStubSlots)
        return nullptr;
    g_stub_names[g_stub_count] = name;
    return g_stub_table[g_stub_count++];
}

PFN_vkVoidFunction get_proc(const char* name) {
#define RET(fn)                                                                                    \
    if (std::strcmp(name, "vk" #fn) == 0)                                                          \
    return reinterpret_cast<PFN_vkVoidFunction>(&fn)
    RET(GetInstanceProcAddr);
    RET(GetDeviceProcAddr);
    // VK_EXT_debug_utils annotations (loader-offered to every app; metadata no-ops, see above).
    RET(SetDebugUtilsObjectNameEXT);
    RET(SetDebugUtilsObjectTagEXT);
    RET(QueueBeginDebugUtilsLabelEXT);
    RET(QueueEndDebugUtilsLabelEXT);
    RET(QueueInsertDebugUtilsLabelEXT);
    RET(CmdBeginDebugUtilsLabelEXT);
    RET(CmdEndDebugUtilsLabelEXT);
    RET(CmdInsertDebugUtilsLabelEXT);
    RET(EnumerateInstanceVersion);
    RET(EnumerateInstanceExtensionProperties);
    RET(EnumerateInstanceLayerProperties);
    RET(CreateInstance);
    RET(DestroyInstance);
    RET(EnumeratePhysicalDevices);
    RET(GetPhysicalDeviceProperties);
    RET(GetPhysicalDeviceProperties2);
    if (std::strcmp(name, "vkGetPhysicalDeviceProperties2KHR") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(&GetPhysicalDeviceProperties2);
    }
    RET(GetPhysicalDeviceFeatures);
    // (GL/zink): the 1.1 *2 physical-device getters (zink calls these on a 1.3 device).
    RET(GetPhysicalDeviceFeatures2);
    if (std::strcmp(name, "vkGetPhysicalDeviceFeatures2KHR") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(&GetPhysicalDeviceFeatures2);
    }
    RET(GetPhysicalDeviceQueueFamilyProperties);
    RET(GetPhysicalDeviceQueueFamilyProperties2);
    if (std::strcmp(name, "vkGetPhysicalDeviceQueueFamilyProperties2KHR") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(&GetPhysicalDeviceQueueFamilyProperties2);
    }
    RET(GetPhysicalDeviceMemoryProperties);
    RET(GetPhysicalDeviceMemoryProperties2);
    if (std::strcmp(name, "vkGetPhysicalDeviceMemoryProperties2KHR") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(&GetPhysicalDeviceMemoryProperties2);
    }
    RET(GetPhysicalDeviceFormatProperties);
    RET(GetPhysicalDeviceFormatProperties2);
    if (std::strcmp(name, "vkGetPhysicalDeviceFormatProperties2KHR") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(&GetPhysicalDeviceFormatProperties2);
    }
    RET(GetPhysicalDeviceImageFormatProperties); // loader-required (loader_icd_init_entries)
    RET(GetPhysicalDeviceImageFormatProperties2);
    if (std::strcmp(name, "vkGetPhysicalDeviceImageFormatProperties2KHR") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(&GetPhysicalDeviceImageFormatProperties2);
    }
    RET(GetPhysicalDeviceSparseImageFormatProperties); // loader-required (loader_icd_init_entries)
    RET(GetPhysicalDeviceSparseImageFormatProperties2);
    if (std::strcmp(name, "vkGetPhysicalDeviceSparseImageFormatProperties2KHR") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(&GetPhysicalDeviceSparseImageFormatProperties2);
    }
    RET(GetPhysicalDeviceExternalBufferProperties);
    if (std::strcmp(name, "vkGetPhysicalDeviceExternalBufferPropertiesKHR") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(&GetPhysicalDeviceExternalBufferProperties);
    }
    RET(GetPhysicalDeviceExternalSemaphoreProperties);
    if (std::strcmp(name, "vkGetPhysicalDeviceExternalSemaphorePropertiesKHR") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(&GetPhysicalDeviceExternalSemaphoreProperties);
    }
    RET(GetPhysicalDeviceExternalFenceProperties);
    if (std::strcmp(name, "vkGetPhysicalDeviceExternalFencePropertiesKHR") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(&GetPhysicalDeviceExternalFenceProperties);
    }
    RET(EnumeratePhysicalDeviceGroups);
    if (std::strcmp(name, "vkEnumeratePhysicalDeviceGroupsKHR") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(&EnumeratePhysicalDeviceGroups);
    }
    RET(GetPhysicalDeviceSurfaceSupportKHR);
    RET(GetPhysicalDeviceSurfaceCapabilitiesKHR);
    RET(GetPhysicalDeviceSurfaceFormatsKHR);
    RET(GetPhysicalDeviceSurfacePresentModesKHR);
    RET(GetPhysicalDeviceXcbPresentationSupportKHR);
    RET(CreateXcbSurfaceKHR);
    RET(GetPhysicalDeviceXlibPresentationSupportKHR); // Vulkan 1.3 support breadth (mpv/libplacebo)
    RET(CreateXlibSurfaceKHR);
    RET(DestroySurfaceKHR);
    RET(EnumerateDeviceExtensionProperties);
    RET(CreateDevice);
    RET(DestroyDevice);
    RET(GetDeviceQueue);
    RET(CreateSwapchainKHR);
    RET(DestroySwapchainKHR);
    RET(GetSwapchainImagesKHR);
    RET(CreateCommandPool);
    RET(DestroyCommandPool);
    RET(ResetCommandPool);   // (GL/zink): local success
    RET(ResetCommandBuffer); // (GL/zink): local success
    RET(AllocateCommandBuffers);
    RET(FreeCommandBuffers);
    RET(CreateFence);
    RET(DestroyFence);
    RET(ResetFences);
    RET(WaitForFences);
    RET(GetFenceStatus);
    RET(CreateSemaphore);
    RET(DestroySemaphore);
    RET(WaitSemaphores);
    RET(SignalSemaphore);
    RET(GetSemaphoreCounterValue);
    // the VkEvent object model (core 1.0) + the sync1 command events (recorded).
    RET(CreateEvent);
    RET(DestroyEvent);
    RET(GetEventStatus);
    RET(SetEvent);
    RET(ResetEvent);
    RET(CmdSetEvent);
    RET(CmdResetEvent);
    RET(CmdWaitEvents);
    RET(BeginCommandBuffer);
    RET(EndCommandBuffer);
    RET(CmdPipelineBarrier);
    RET(CmdClearColorImage);
    RET(CmdClearAttachments);
    // Draw surface: create/destroy + the draw vkCmd* recording entry points.
    RET(CreateImageView);
    RET(DestroyImageView);
    RET(CreateBufferView); // (GL/zink): texel buffer views
    RET(DestroyBufferView);
    RET(CreateShaderModule);
    RET(DestroyShaderModule);
    RET(CreateRenderPass);
    RET(CreateRenderPass2); // (GL/zink): core 1.2 render-pass-2
    if (std::strcmp(name, "vkCreateRenderPass2KHR") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(&CreateRenderPass2);
    RET(DestroyRenderPass);
    RET(CreateFramebuffer);
    RET(DestroyFramebuffer);
    RET(CreatePipelineLayout);
    RET(DestroyPipelineLayout);
    RET(CreateGraphicsPipelines);
    RET(CreateComputePipelines); // compute
    RET(DestroyPipeline);
    RET(CreatePipelineCache);  // real-app surface: local no-op cache for vkcube
    RET(DestroyPipelineCache); // real-app surface: local no-op cache for vkcube
    RET(GetPipelineCacheData); // (GL/zink): empty no-op cache readback
    RET(CmdSetLineWidth);      // (GL/zink): core-1.0 dynamic state set commands
    RET(CmdSetDepthBias);
    RET(CmdSetBlendConstants);
    RET(CmdSetDepthBounds);
    RET(CmdSetStencilCompareMask);
    RET(CmdSetStencilWriteMask);
    RET(CmdSetStencilReference);
    // (native lane -- EDS1): VK_EXT_extended_dynamic_state, core-1.3. Registered
    // unconditionally (registering a proc addr does not advertise the extension -- that is
    // lane-gated at enumeration); an app only CALLS these if it enabled the lane-gated feature, so
    // zink on the default lane never reaches them. Both the core and the *EXT alias names resolve.
    RET(CmdSetCullMode);
    RET(CmdSetFrontFace);
    RET(CmdSetPrimitiveTopology);
    RET(CmdSetDepthTestEnable);
    RET(CmdSetDepthWriteEnable);
    RET(CmdSetDepthCompareOp);
    if (std::strcmp(name, "vkCmdSetCullModeEXT") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(&CmdSetCullMode);
    if (std::strcmp(name, "vkCmdSetFrontFaceEXT") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(&CmdSetFrontFace);
    if (std::strcmp(name, "vkCmdSetPrimitiveTopologyEXT") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(&CmdSetPrimitiveTopology);
    if (std::strcmp(name, "vkCmdSetDepthTestEnableEXT") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(&CmdSetDepthTestEnable);
    if (std::strcmp(name, "vkCmdSetDepthWriteEnableEXT") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(&CmdSetDepthWriteEnable);
    if (std::strcmp(name, "vkCmdSetDepthCompareOpEXT") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(&CmdSetDepthCompareOp);
    // (native lane): VK_KHR_dynamic_rendering. Registered unconditionally (a
    // proc addr is not advertising; the extension is lane-gated at enumeration + the feature is
    // masked off the native lane, so zink on the default lane never records these). Both the core
    // and the *KHR alias names resolve.
    RET(CmdBeginRendering);
    RET(CmdEndRendering);
    if (std::strcmp(name, "vkCmdBeginRenderingKHR") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(&CmdBeginRendering);
    if (std::strcmp(name, "vkCmdEndRenderingKHR") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(&CmdEndRendering);
    // VK_KHR_synchronization2 command-buffer commands. Registered unconditionally
    // (their sync2 admission is gated on ext AND feature at the backends; masked off the native
    // lane so zink on the default lane never records them). Both the core and *KHR alias names
    // resolve.
    RET(CmdPipelineBarrier2);
    RET(CmdWriteTimestamp2);
    RET(CmdSetEvent2);
    RET(CmdResetEvent2);
    RET(CmdWaitEvents2);
    if (std::strcmp(name, "vkCmdPipelineBarrier2KHR") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(&CmdPipelineBarrier2);
    if (std::strcmp(name, "vkCmdWriteTimestamp2KHR") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(&CmdWriteTimestamp2);
    if (std::strcmp(name, "vkCmdSetEvent2KHR") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(&CmdSetEvent2);
    if (std::strcmp(name, "vkCmdResetEvent2KHR") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(&CmdResetEvent2);
    if (std::strcmp(name, "vkCmdWaitEvents2KHR") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(&CmdWaitEvents2);
    // Vulkan 1.3 support: the remaining EDS1 setters + the core-1.3 EDS2 subset, vkCmdResolveImage,
    // the copy_commands2 family, private data, the maintenance4 queries, and tool properties.
    // Registered unconditionally (a proc addr is not advertising; each entry's admission gate
    // lives on the backends or in the entry itself). Core names plus the promoted-extension
    // aliases: *EXT for the dynamic-state setters (VK_EXT_extended_dynamic_state{,2} spellings),
    // private data, and tool properties; *KHR for copy_commands2 and the maintenance4 queries.
    RET(CmdSetViewportWithCount);
    RET(CmdSetScissorWithCount);
    RET(CmdBindVertexBuffers2);
    RET(CmdSetDepthBoundsTestEnable);
    RET(CmdSetStencilTestEnable);
    RET(CmdSetStencilOp);
    RET(CmdSetRasterizerDiscardEnable);
    RET(CmdSetDepthBiasEnable);
    RET(CmdSetPrimitiveRestartEnable);
    if (std::strcmp(name, "vkCmdSetViewportWithCountEXT") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(&CmdSetViewportWithCount);
    if (std::strcmp(name, "vkCmdSetScissorWithCountEXT") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(&CmdSetScissorWithCount);
    if (std::strcmp(name, "vkCmdBindVertexBuffers2EXT") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(&CmdBindVertexBuffers2);
    if (std::strcmp(name, "vkCmdSetDepthBoundsTestEnableEXT") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(&CmdSetDepthBoundsTestEnable);
    if (std::strcmp(name, "vkCmdSetStencilTestEnableEXT") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(&CmdSetStencilTestEnable);
    if (std::strcmp(name, "vkCmdSetStencilOpEXT") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(&CmdSetStencilOp);
    if (std::strcmp(name, "vkCmdSetRasterizerDiscardEnableEXT") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(&CmdSetRasterizerDiscardEnable);
    if (std::strcmp(name, "vkCmdSetDepthBiasEnableEXT") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(&CmdSetDepthBiasEnable);
    if (std::strcmp(name, "vkCmdSetPrimitiveRestartEnableEXT") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(&CmdSetPrimitiveRestartEnable);
    RET(CmdResolveImage);
    RET(CmdResolveImage2);
    RET(CmdCopyBuffer2);
    RET(CmdCopyImage2);
    RET(CmdCopyBufferToImage2);
    RET(CmdCopyImageToBuffer2);
    RET(CmdBlitImage2);
    if (std::strcmp(name, "vkCmdResolveImage2KHR") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(&CmdResolveImage2);
    if (std::strcmp(name, "vkCmdCopyBuffer2KHR") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(&CmdCopyBuffer2);
    if (std::strcmp(name, "vkCmdCopyImage2KHR") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(&CmdCopyImage2);
    if (std::strcmp(name, "vkCmdCopyBufferToImage2KHR") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(&CmdCopyBufferToImage2);
    if (std::strcmp(name, "vkCmdCopyImageToBuffer2KHR") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(&CmdCopyImageToBuffer2);
    if (std::strcmp(name, "vkCmdBlitImage2KHR") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(&CmdBlitImage2);
    RET(CreatePrivateDataSlot);
    RET(DestroyPrivateDataSlot);
    RET(SetPrivateData);
    RET(GetPrivateData);
    if (std::strcmp(name, "vkCreatePrivateDataSlotEXT") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(&CreatePrivateDataSlot);
    if (std::strcmp(name, "vkDestroyPrivateDataSlotEXT") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(&DestroyPrivateDataSlot);
    if (std::strcmp(name, "vkSetPrivateDataEXT") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(&SetPrivateData);
    if (std::strcmp(name, "vkGetPrivateDataEXT") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(&GetPrivateData);
    RET(GetPhysicalDeviceToolProperties);
    if (std::strcmp(name, "vkGetPhysicalDeviceToolPropertiesEXT") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(&GetPhysicalDeviceToolProperties);
    RET(GetDeviceBufferMemoryRequirements);
    RET(GetDeviceImageMemoryRequirements);
    RET(GetDeviceImageSparseMemoryRequirements);
    if (std::strcmp(name, "vkGetDeviceBufferMemoryRequirementsKHR") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(&GetDeviceBufferMemoryRequirements);
    if (std::strcmp(name, "vkGetDeviceImageMemoryRequirementsKHR") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(&GetDeviceImageMemoryRequirements);
    if (std::strcmp(name, "vkGetDeviceImageSparseMemoryRequirementsKHR") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(&GetDeviceImageSparseMemoryRequirements);
    RET(CmdBeginRenderPass);
    RET(CmdEndRenderPass);
    RET(CmdBeginRenderPass2); // (GL/zink): core 1.2 render-pass-2 commands
    RET(CmdEndRenderPass2);
    if (std::strcmp(name, "vkCmdBeginRenderPass2KHR") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(&CmdBeginRenderPass2);
    if (std::strcmp(name, "vkCmdEndRenderPass2KHR") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(&CmdEndRenderPass2);
    RET(CmdBindPipeline);
    RET(CmdDispatch);         // compute
    RET(CmdDispatchIndirect); // compute
    RET(CmdSetViewport);
    RET(CmdSetScissor);
    RET(CmdDraw);
    RET(CmdDrawIndirect);
    RET(CmdDrawIndirectCount);
    // Host-visible memory + buffers.
    RET(CreateBuffer);
    RET(DestroyBuffer);
    RET(GetBufferMemoryRequirements);
    RET(BindBufferMemory);
    // (bufferDeviceAddress, core 1.2): registered unconditionally (a proc addr is not
    // advertising -- the feature is masked FALSE off the native lane, and the entry itself
    // fail-closes to 0 unless the device enabled the feature). CORE name only: the KHR/EXT alias
    // extensions are not advertised, so their names stay named abort-stubs by design
    // (alias policy: serve + test the core path).
    RET(GetBufferDeviceAddress);
    RET(AllocateMemory);
    RET(FreeMemory);
    RET(MapMemory);
    RET(UnmapMemory);
    RET(FlushMappedMemoryRanges);
    RET(CmdBindVertexBuffers);
    RET(CmdBindIndexBuffer); // (GL/zink): indexed draws
    RET(CmdDrawIndexed);
    RET(CmdDrawIndexedIndirect);
    RET(CmdDrawIndexedIndirectCount);
    if (std::strcmp(name, "vkCmdDrawIndirectCountKHR") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(&CmdDrawIndirectCount);
    if (std::strcmp(name, "vkCmdDrawIndexedIndirectCountKHR") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(&CmdDrawIndexedIndirectCount);
    RET(CmdPushConstants);
    // (GL/zink): the wired VK_EXT_transform_feedback + VK_EXT_conditional_rendering command
    // surfaces (the vkCmdBegin/EndQueryIndexedEXT pair stays a named abort-stub: XFB-stream queries
    // need the indexed pair, deferred -- transformFeedbackQueries is reported FALSE so zink never
    // starts one).
    RET(CmdBindTransformFeedbackBuffersEXT);
    RET(CmdBeginTransformFeedbackEXT);
    RET(CmdEndTransformFeedbackEXT);
    RET(CmdDrawIndirectByteCountEXT);
    RET(CmdBeginConditionalRenderingEXT);
    RET(CmdEndConditionalRenderingEXT);
    // Descriptor surface + per-frame UBO.
    RET(GetDescriptorSetLayoutSupport); // (GL/zink): advisory, answered local TRUE
    if (std::strcmp(name, "vkGetDescriptorSetLayoutSupportKHR") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(&GetDescriptorSetLayoutSupport);
    }
    RET(CreateDescriptorSetLayout);
    RET(DestroyDescriptorSetLayout);
    RET(CreateDescriptorPool);
    RET(DestroyDescriptorPool);
    RET(AllocateDescriptorSets);
    RET(UpdateDescriptorSets);
    RET(CmdBindDescriptorSets);
    // (GL/zink): descriptor update templates (core 1.1, + KHR aliases), shimmed ICD-side.
    RET(CreateDescriptorUpdateTemplate);
    RET(UpdateDescriptorSetWithTemplate);
    RET(DestroyDescriptorUpdateTemplate);
    if (std::strcmp(name, "vkCreateDescriptorUpdateTemplateKHR") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(&CreateDescriptorUpdateTemplate);
    if (std::strcmp(name, "vkUpdateDescriptorSetWithTemplateKHR") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(&UpdateDescriptorSetWithTemplate);
    if (std::strcmp(name, "vkDestroyDescriptorUpdateTemplateKHR") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(&DestroyDescriptorUpdateTemplate);
    // Textures + depth for literal vkcube.
    RET(CreateImage);
    RET(DestroyImage);
    RET(GetImageMemoryRequirements);
    RET(GetImageSubresourceLayout);
    RET(BindImageMemory);
    // (GL/zink): 1.1-core get-memory-requirements2 + bind-memory2 (KHR aliases).
    RET(GetBufferMemoryRequirements2);
    RET(GetImageMemoryRequirements2);
    RET(BindBufferMemory2);
    RET(BindImageMemory2);
    if (std::strcmp(name, "vkGetBufferMemoryRequirements2KHR") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(&GetBufferMemoryRequirements2);
    if (std::strcmp(name, "vkGetImageMemoryRequirements2KHR") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(&GetImageMemoryRequirements2);
    if (std::strcmp(name, "vkBindBufferMemory2KHR") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(&BindBufferMemory2);
    if (std::strcmp(name, "vkBindImageMemory2KHR") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(&BindImageMemory2);
    // (GL/zink): timeline-semaphore KHR aliases (zink resolves either the core 1.2 or the
    // VK_KHR_timeline_semaphore name).
    if (std::strcmp(name, "vkWaitSemaphoresKHR") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(&WaitSemaphores);
    if (std::strcmp(name, "vkSignalSemaphoreKHR") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(&SignalSemaphore);
    if (std::strcmp(name, "vkGetSemaphoreCounterValueKHR") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(&GetSemaphoreCounterValue);
    // Sampler and texture upload.
    RET(CreateSampler);
    RET(DestroySampler);
    // Query pools (GL 3.3 / occlusion / xfb queries).
    RET(CreateQueryPool);
    RET(DestroyQueryPool);
    RET(GetQueryPoolResults);
    RET(ResetQueryPool); // hostQueryReset: device-level, core 1.2 -- resolves whenever
                         // the device is >= 1.2 (the ICD reports 1.2 or 1.3), never the abort-stub
    RET(CmdResetQueryPool);
    RET(CmdBeginQuery);
    RET(CmdEndQuery);
    RET(CmdWriteTimestamp);
    RET(CmdCopyQueryPoolResults);
    RET(CmdCopyBufferToImage);
    RET(CmdCopyImageToBuffer);
    RET(CmdBlitImage);  // (GL/zink): glGenerateMipmap / glBlitFramebuffer ride vkCmdBlitImage
    RET(CmdCopyImage);  // (GL/zink): glCopyTexSubImage2D-class same-format transfers
    RET(CmdCopyBuffer); // (GL/zink): buffer->buffer copy
    RET(CmdFillBuffer); // faithful fill (Mesa >= 25 zink: GL buffer clears)
    RET(AcquireNextImageKHR);
    RET(QueueSubmit);
    RET(QueueSubmit2); // VK_KHR_synchronization2 queue submit (core + *KHR alias)
    if (std::strcmp(name, "vkQueueSubmit2KHR") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(&QueueSubmit2);
    RET(QueuePresentKHR);
    RET(DeviceWaitIdle);
    RET(QueueWaitIdle);
#undef RET
    return nullptr;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetInstanceProcAddr(VkInstance, const char* pName) {
    return get_proc(pName);
}

// Proc-address availability is itself an observable capability surface
// (libraries probe symbols before features), so GetDeviceProcAddr must be DEVICE-AWARE: the
// core-1.3-only names resolve only on an honest apiVersion-1.3 device, and a promoted
// extension's alias spellings resolve only where THAT extension was actually enabled. Returns
// true when `name` must answer NULL for this device (call-time gates stay as defense in depth).
bool device_proc_gated_off(const DeviceImpl* dev, const char* name) {
    if (vkr::icd_policy::indirect_count_khr_proc_name(name)) {
        return !vkr::icd_policy::indirect_count_khr_proc_available(
            dev->enabled_extensions.count(VK_KHR_DRAW_INDIRECT_COUNT_EXTENSION_NAME) != 0);
    }
    // Device-level commands that exist ONLY at core 1.3 (no suffix): dynamic rendering, sync2,
    // copy_commands2, private data, maintenance4's queries, and the EDS1/EDS2-subset core names.
    static const char* const kVk13CoreOnly[] = {
        "vkCmdBeginRendering",
        "vkCmdEndRendering",
        "vkCmdPipelineBarrier2",
        "vkCmdWriteTimestamp2",
        "vkQueueSubmit2",
        "vkCmdSetEvent2",
        "vkCmdResetEvent2",
        "vkCmdWaitEvents2",
        "vkCmdCopyBuffer2",
        "vkCmdCopyImage2",
        "vkCmdCopyBufferToImage2",
        "vkCmdCopyImageToBuffer2",
        "vkCmdBlitImage2",
        "vkCmdResolveImage2",
        "vkCreatePrivateDataSlot",
        "vkDestroyPrivateDataSlot",
        "vkSetPrivateData",
        "vkGetPrivateData",
        "vkGetDeviceBufferMemoryRequirements",
        "vkGetDeviceImageMemoryRequirements",
        "vkGetDeviceImageSparseMemoryRequirements",
        "vkCmdSetCullMode",
        "vkCmdSetFrontFace",
        "vkCmdSetPrimitiveTopology",
        "vkCmdSetViewportWithCount",
        "vkCmdSetScissorWithCount",
        "vkCmdBindVertexBuffers2",
        "vkCmdSetDepthTestEnable",
        "vkCmdSetDepthWriteEnable",
        "vkCmdSetDepthCompareOp",
        "vkCmdSetDepthBoundsTestEnable",
        "vkCmdSetStencilTestEnable",
        "vkCmdSetStencilOp",
        "vkCmdSetRasterizerDiscardEnable",
        "vkCmdSetDepthBiasEnable",
        "vkCmdSetPrimitiveRestartEnable",
    };
    for (const char* n : kVk13CoreOnly) {
        if (std::strcmp(name, n) == 0) {
            return !dev->vk13_device;
        }
    }
    // Promoted-extension alias spellings: gated on the ENABLED extension (an unadvertised
    // extension -- copy_commands2, private_data, maintenance4, EDS2 -- can never be enabled, so
    // its aliases answer NULL everywhere until the feature is advertised).
    static const struct {
        const char* name;
        const char* ext;
    } kAliases[] = {
        {"vkCmdSetCullModeEXT", "VK_EXT_extended_dynamic_state"},
        {"vkCmdSetFrontFaceEXT", "VK_EXT_extended_dynamic_state"},
        {"vkCmdSetPrimitiveTopologyEXT", "VK_EXT_extended_dynamic_state"},
        {"vkCmdSetViewportWithCountEXT", "VK_EXT_extended_dynamic_state"},
        {"vkCmdSetScissorWithCountEXT", "VK_EXT_extended_dynamic_state"},
        {"vkCmdBindVertexBuffers2EXT", "VK_EXT_extended_dynamic_state"},
        {"vkCmdSetDepthTestEnableEXT", "VK_EXT_extended_dynamic_state"},
        {"vkCmdSetDepthWriteEnableEXT", "VK_EXT_extended_dynamic_state"},
        {"vkCmdSetDepthCompareOpEXT", "VK_EXT_extended_dynamic_state"},
        {"vkCmdSetDepthBoundsTestEnableEXT", "VK_EXT_extended_dynamic_state"},
        {"vkCmdSetStencilTestEnableEXT", "VK_EXT_extended_dynamic_state"},
        {"vkCmdSetStencilOpEXT", "VK_EXT_extended_dynamic_state"},
        {"vkCmdSetRasterizerDiscardEnableEXT", "VK_EXT_extended_dynamic_state2"},
        {"vkCmdSetDepthBiasEnableEXT", "VK_EXT_extended_dynamic_state2"},
        {"vkCmdSetPrimitiveRestartEnableEXT", "VK_EXT_extended_dynamic_state2"},
        {"vkCmdBeginRenderingKHR", "VK_KHR_dynamic_rendering"},
        {"vkCmdEndRenderingKHR", "VK_KHR_dynamic_rendering"},
        {"vkCmdPipelineBarrier2KHR", "VK_KHR_synchronization2"},
        {"vkCmdWriteTimestamp2KHR", "VK_KHR_synchronization2"},
        {"vkQueueSubmit2KHR", "VK_KHR_synchronization2"},
        {"vkCmdSetEvent2KHR", "VK_KHR_synchronization2"},
        {"vkCmdResetEvent2KHR", "VK_KHR_synchronization2"},
        {"vkCmdWaitEvents2KHR", "VK_KHR_synchronization2"},
        {"vkCmdCopyBuffer2KHR", "VK_KHR_copy_commands2"},
        {"vkCmdCopyImage2KHR", "VK_KHR_copy_commands2"},
        {"vkCmdCopyBufferToImage2KHR", "VK_KHR_copy_commands2"},
        {"vkCmdCopyImageToBuffer2KHR", "VK_KHR_copy_commands2"},
        {"vkCmdBlitImage2KHR", "VK_KHR_copy_commands2"},
        {"vkCmdResolveImage2KHR", "VK_KHR_copy_commands2"},
        {"vkCreatePrivateDataSlotEXT", "VK_EXT_private_data"},
        {"vkDestroyPrivateDataSlotEXT", "VK_EXT_private_data"},
        {"vkSetPrivateDataEXT", "VK_EXT_private_data"},
        {"vkGetPrivateDataEXT", "VK_EXT_private_data"},
        {"vkGetDeviceBufferMemoryRequirementsKHR", "VK_KHR_maintenance4"},
        {"vkGetDeviceImageMemoryRequirementsKHR", "VK_KHR_maintenance4"},
        {"vkGetDeviceImageSparseMemoryRequirementsKHR", "VK_KHR_maintenance4"},
    };
    for (const auto& a : kAliases) {
        if (std::strcmp(name, a.name) == 0) {
            return dev->enabled_extensions.count(a.ext) == 0;
        }
    }
    return false;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetDeviceProcAddr(VkDevice device, const char* pName) {
    if (pName == nullptr) {
        return nullptr;
    }
    // Device-aware honesty gate. vk13_device / enabled_extensions are
    // immutable after CreateDevice, so this read is lock-free by design.
    if (device != VK_NULL_HANDLE) {
        const auto* dev = reinterpret_cast<const DeviceImpl*>(device);
        if (device_proc_gated_off(dev, pName)) {
            icd_trace("GetDeviceProcAddr '%s' -> NULL (not in this device's version/extension "
                      "surface)",
                      pName);
            return nullptr;
        }
    }
    PFN_vkVoidFunction p = get_proc(pName);
    if (p == nullptr) {
        // (GL/zink): a NULL device-function PFN is the prime suspect for a
        // post-CreateDevice crash -- zink resolves an advertised extension / core function we have
        // not wired and may call it. Hand back a named abort-stub (not NULL) so the first *call*
        // names the culprit and stops cleanly, instead of a silent SIGSEGV in a stripped frame.
        icd_trace("GetDeviceProcAddr MISS '%s' -> named abort-stub", pName);
        return unimpl_device_stub_for(pName);
    }
    return p;
}

} // namespace

// --- Exported loader interface (v3) -----------------------------------------
extern "C" {

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance instance,
                                                                   const char* pName) {
    return GetInstanceProcAddr(instance, pName);
}

// Cap at v3: v4+ requires also exporting vk_icdGetPhysicalDeviceProcAddr, which we do not (all
// our physical-device entry points answer through vk_icdGetInstanceProcAddr -- the v3 contract).
// Returning a higher version without that export crashes the loader's phys-device resolution.
VKAPI_ATTR VkResult VKAPI_CALL
vk_icdNegotiateLoaderICDInterfaceVersion(std::uint32_t* pSupportedVersion) {
    if (pSupportedVersion == nullptr) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (*pSupportedVersion > 3) {
        *pSupportedVersion = 3;
    }
    return VK_SUCCESS;
}

} // extern "C"
