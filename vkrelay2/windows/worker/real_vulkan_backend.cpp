// Pull in the Win32 WSI entry points (vkCreateWin32SurfaceKHR /
// VkWin32SurfaceCreateInfoKHR) -- must precede the first <vulkan/vulkan.h>, which the
// backend header includes.
#define VK_USE_PLATFORM_WIN32_KHR
#include "windows/worker/real_vulkan_backend.hpp"

#include "common/vkrpc/device_loss_policy.h"

#include "common/logging/logging.hpp"
#include "common/sidecar/window_placement.hpp"
#include "common/vkrpc/feature_bits.h" // pack/unpack VkPhysicalDeviceFeatures <-> u64
#include "common/vkrpc/indirect_draw_validation.h"
#include "common/vkrpc/rpc_profile.h"        // record-handler phase timers
#include "windows/worker/acquire_poll.hpp"   // abort-aware acquire poll policy
#include "windows/worker/real_gpu_probe.hpp" // format_luid -- the LUID match key shares one formatter
#include "windows/worker/windowing/coordinate_map.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <vulkan/vulkan.h>

namespace vkr::worker {
namespace {

constexpr char kComponent[] = "vkrpc-real";

// A legal final vec3 fetch returns corrupt data deterministically on the AMD 610M. A standalone
// native one-draw workload reproduces it for the split-binding exact-end shape (padded and
// interleaved controls pass; see native_vertex_tail_probe.cpp). Through the relay, a single-binding
// exact-end fetch (a dedicated 12-byte point buffer) also corrupts, which no native workload has
// reproduced yet. One fetch-width of private host tail storage makes every observed variant stable.
// Keep the guest's logical size in RealBuffer and use this larger size only for host creation /
// maintenance4 requirements. This is a vendor-neutral virtualization accommodation: legal guest
// accesses are unchanged and every guest-visible range remains clamped to the logical end. See the
// AMD iGPU, driver-report, and draw-isolation collaboration sessions.
constexpr VkDeviceSize kHostVertexFetchTailGuardBytes = 16;

bool host_buffer_size(std::uint64_t logical_size, std::uint64_t usage, VkDeviceSize& out) {
    out = static_cast<VkDeviceSize>(logical_size);
    if ((usage & vkrpc::kBufferUsageVertexBuffer) == 0) {
        return true;
    }
    if (out > ~VkDeviceSize{0} - kHostVertexFetchTailGuardBytes) {
        return false;
    }
    out += kHostVertexFetchTailGuardBytes;
    return true;
}

// the OPTIONAL debug HWND title tag. When VKRELAY2_DEBUG_WINDOW_TITLES is set (and not
// "0"), the worker tags each window's title with its guest XID ("vkrelay2 [xid=0x...]") so the
// capture_window.ps1 dev helper can correlate an enumerated HWND back to a toplevel via
// -TitleMatch. Off by default -- user-facing window titles stay the generic "vkrelay2". Read at
// window-create time (rare), so a debug session can toggle it without a rebuild.
bool debug_window_titles() {
    const char* v = std::getenv("VKRELAY2_DEBUG_WINDOW_TITLES");
    return v != nullptr && v[0] != '\0' && !(v[0] == '0' && v[1] == '\0');
}

bool input_trace_enabled() {
    const char* v = std::getenv("VKRELAY2_INPUT_TRACE");
    return v != nullptr && v[0] != '\0' && !(v[0] == '0' && v[1] == '\0');
}

// VkPhysicalDeviceType -> the same strings the mock/probe use.
const char* device_type_string(VkPhysicalDeviceType type) {
    switch (type) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
        return "discrete";
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
        return "integrated";
    case VK_PHYSICAL_DEVICE_TYPE_CPU:
        return "cpu";
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
        return "virtual";
    default:
        return "other";
    }
}

bool version_less(int a_major, int a_minor, int b_major, int b_minor) {
    return a_major < b_major || (a_major == b_major && a_minor < b_minor);
}

vkrpc::DeviceCaps caps_from_props(const VkPhysicalDeviceProperties& p) {
    vkrpc::DeviceCaps c;
    c.device_name = p.deviceName;
    c.vendor_id = p.vendorID;
    c.device_id = p.deviceID;
    c.device_type = device_type_string(p.deviceType);
    // MRT capability gate: advertised ONLY because the relay now carries the full color-ref
    // vector (never advertise-then-hope). An old ICD ignores it; a new ICD refuses >1 color ref
    // toward a worker that did not advertise it.
    c.max_color_attachments = p.limits.maxColorAttachments;
    // geometry-stream: this worker validates (shared rasterization_stream_ok
    // against the host's cached TF properties) + replays the stream pipeline pNext. A worker that
    // does not advertise this gets the older ICD's fail-closed rejection of the stream shape.
    c.rasterization_stream_state = 1;
    c.core_indirect_draw = 1;
    c.core_indirect_draw_scalar_payload = 1;
    c.core_indirect_draw_count = 1;
    return c;
}

// device-loss probe: describe the monitor a window is currently on + the GPU adapter DRIVING
// that monitor. `adapter` (EnumDisplayDevices' DeviceString, e.g. "NVIDIA GeForce RTX 4080" vs
// "Intel(R) Iris(R) Xe Graphics") is compared against the render adapter's Vulkan deviceName to
// detect a cross-GPU direct present (the leading VK_ERROR_DEVICE_LOST hypothesis on an external
// monitor). All calls are thread-agnostic (no window-thread affinity), so this runs off any thread.
struct PresentTargetInfo {
    std::string monitor; // GDI device name, e.g. "\\.\DISPLAY2" ("" if unknown)
    std::string adapter; // the adapter driving that monitor (EnumDisplayDevices DeviceString)
    int x = 0, y = 0, w = 0, h = 0; // monitor rect (screen coords)
    unsigned dpi = 0;               // the window's DPI (GetDpiForWindow)
};

std::string wide_to_utf8(const wchar_t* w) {
    if (w == nullptr || w[0] == L'\0') {
        return {};
    }
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) {
        return {};
    }
    std::string s(static_cast<std::size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

PresentTargetInfo describe_present_target(HWND hwnd) {
    PresentTargetInfo t;
    if (hwnd == nullptr) {
        return t;
    }
    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(mon, reinterpret_cast<MONITORINFO*>(&mi))) {
        t.monitor = wide_to_utf8(mi.szDevice);
        t.x = mi.rcMonitor.left;
        t.y = mi.rcMonitor.top;
        t.w = mi.rcMonitor.right - mi.rcMonitor.left;
        t.h = mi.rcMonitor.bottom - mi.rcMonitor.top;
        // The GPU ADAPTER driving this display. EnumDisplayDevices(szDevice, ...) returns the
        // MONITOR product (e.g. "Lenovo DisplayHDR"), NOT the GPU -- so enumerate display ADAPTERS
        // (NULL device) and match the one whose DeviceName is this monitor's display
        // ("\\.\DISPLAYx"); its DeviceString is the GPU name ("NVIDIA GeForce RTX 4080 Laptop GPU"
        // vs "Intel(R) ..."), the cross-GPU signal against the render adapter.
        DISPLAY_DEVICEW ad{};
        ad.cb = sizeof(ad);
        for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &ad, 0) != FALSE; ++i) {
            if (wcscmp(ad.DeviceName, mi.szDevice) == 0) {
                t.adapter = wide_to_utf8(ad.DeviceString);
                break;
            }
            ad.cb = sizeof(ad);
        }
    }
    t.dpi = GetDpiForWindow(hwnd);
    return t;
}

// Loader instance-level version. vkEnumerateInstanceVersion is a 1.1+ global
// command; a 1.0 loader lacks it (treat as 1.0).
void query_instance_version(int& major, int& minor) {
    major = 1;
    minor = 0;
    if (auto pfn = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
            vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateInstanceVersion"))) {
        std::uint32_t iv = 0;
        if (pfn(&iv) == VK_SUCCESS) {
            major = static_cast<int>(VK_API_VERSION_MAJOR(iv));
            minor = static_cast<int>(VK_API_VERSION_MINOR(iv));
        }
    }
}

// VkApplicationInfo.apiVersion to request: min(instance-supported, worker ceiling)
// so instance creation succeeds on 1.0-1.2 loaders (a lesser host is negotiable,
// not a failure).
std::uint32_t instance_request_version(int inst_major, int inst_minor) {
    int major = vkrpc::kSupportedApiMajor;
    int minor = vkrpc::kSupportedApiMinor;
    if (version_less(inst_major, inst_minor, major, minor)) {
        major = inst_major;
        minor = inst_minor;
    }
    return VK_MAKE_API_VERSION(0, major, minor, 0);
}

// Selects the host physical device for the session. When `required` (the daemon resolved a concrete
// real device), selection is faithful: match the stable LUID when given, else the EXACT name, and
// fail closed (VK_NULL_HANDLE -> the caller reports "no device") rather than substitute another.
// When not required (auto/mock-world), selection is lenient: name substring, then discrete, then
// first. Also returns VK_NULL_HANDLE when there are no devices.
VkPhysicalDevice select_physical_device(VkInstance instance, bool required,
                                        const std::string& gpu_luid, const std::string& gpu_name,
                                        VkPhysicalDeviceProperties& out_props) {
    std::uint32_t count = 0;
    VkResult r = vkEnumeratePhysicalDevices(instance, &count, nullptr);
    if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || count == 0) {
        return VK_NULL_HANDLE;
    }
    std::vector<VkPhysicalDevice> devs(count);
    r = vkEnumeratePhysicalDevices(instance, &count, devs.data());
    if (r != VK_SUCCESS && r != VK_INCOMPLETE) {
        return VK_NULL_HANDLE;
    }
    devs.resize(count); // VK_INCOMPLETE: only the first `count` handles are valid
    if (devs.empty()) {
        return VK_NULL_HANDLE;
    }

    // Faithful path: serve exactly the resolved device or fail closed -- never a silent substitute.
    if (required) {
        // Prefer the stable LUID when the daemon supplied one.
        if (!gpu_luid.empty()) {
            // vkGetPhysicalDeviceProperties2 is core 1.1; resolve it so a 1.0 instance can't crash.
            auto props2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
                vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceProperties2"));
            bool any_luid_valid = false;
            if (props2 != nullptr) {
                for (VkPhysicalDevice d : devs) {
                    VkPhysicalDeviceIDProperties id{};
                    id.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
                    VkPhysicalDeviceProperties2 p2{};
                    p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
                    p2.pNext = &id;
                    props2(d, &p2);
                    if (id.deviceLUIDValid != VK_FALSE) {
                        any_luid_valid = true;
                        if (format_luid(id.deviceLUID, VK_LUID_SIZE) == gpu_luid) {
                            out_props = p2.properties;
                            return d;
                        }
                    }
                }
            }
            if (any_luid_valid) {
                return VK_NULL_HANDLE; // requested LUID is absent -> fail closed
            }
            // No host device reported a valid LUID (rare): fall through to the exact-name key.
        }
        // Exact-name key: when no LUID was supplied, or none was usable on the host. Fail closed if
        // the named device is absent -- a faithful selection never substitutes.
        if (!gpu_name.empty()) {
            for (VkPhysicalDevice d : devs) {
                VkPhysicalDeviceProperties p{};
                vkGetPhysicalDeviceProperties(d, &p);
                if (gpu_name == p.deviceName) {
                    out_props = p;
                    return d;
                }
            }
        }
        return VK_NULL_HANDLE; // required selection, no match -> fail closed
    }

    // Lenient path (not required -> auto/mock-world): name substring/type selector, then discrete,
    // then first. The type spellings make the in-process worker-backend tier explicitly
    // adapter-selectable via VKRELAY2_TEST_GPU without pretending to have a daemon-resolved LUID.
    VkPhysicalDevice chosen = devs[0];
    vkGetPhysicalDeviceProperties(chosen, &out_props);
    VkPhysicalDevice discrete = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties discrete_props{};
    VkPhysicalDevice integrated = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties integrated_props{};
    const bool want_integrated = gpu_name == "integrated";
    const bool want_discrete = gpu_name == "high-performance" || gpu_name == "discrete";
    for (VkPhysicalDevice d : devs) {
        VkPhysicalDeviceProperties p{};
        vkGetPhysicalDeviceProperties(d, &p);
        if (!want_integrated && !want_discrete && !gpu_name.empty() &&
            std::string(p.deviceName).find(gpu_name) != std::string::npos) {
            out_props = p;
            return d;
        }
        if (discrete == VK_NULL_HANDLE && p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            discrete = d;
            discrete_props = p;
        }
        if (integrated == VK_NULL_HANDLE &&
            p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) {
            integrated = d;
            integrated_props = p;
        }
    }
    if (want_integrated) {
        if (integrated != VK_NULL_HANDLE) {
            out_props = integrated_props;
        }
        return integrated;
    }
    if (want_discrete) {
        if (discrete != VK_NULL_HANDLE) {
            out_props = discrete_props;
        }
        return discrete;
    }
    if (discrete != VK_NULL_HANDLE) {
        out_props = discrete_props;
        return discrete;
    }
    return chosen;
}

// Validates one recorded command's fields BEFORE any vkCmd is emitted (validate-then-
// record). `transfer_dst` is whether the referenced image supports TRANSFER_DST (needed
// for a clear). Returns false + sets `err` on a malformed command. Layout/access values
// are passed through to the driver (the validation layer is the final enum check); the
// checks here reject the cases that would otherwise become invalid host usage silently
// (a zero sync1 stage mask, a clear on a non-TRANSFER_DST image, an unknown kind).
bool validate_recorded_command(const vkrpc::RecordedCommand& c, bool transfer_dst,
                               std::string& err) {
    const vkrpc::CmdKind k = vkrpc::recorded_command_kind(c);
    if (k == vkrpc::CmdKind::PipelineBarrier) {
        // Sync1 requires non-zero src/dst stage masks; access masks may be zero (e.g.
        // UNDEFINED->TRANSFER_DST has srcAccessMask 0); the aspect must be a non-zero mask.
        // Every flag mask must fit 32-bit VkFlags -- an oversized wire value is rejected
        // here, not silently truncated by the cast in emit_command.
        if (c.old_layout < 0 || c.new_layout < 0 || c.aspect < 1 ||
            !vkrpc::valid_stage_mask(c.src_stage) || !vkrpc::valid_stage_mask(c.dst_stage) ||
            !vkrpc::valid_access_mask(c.src_access) || !vkrpc::valid_access_mask(c.dst_access)) {
            err = "malformed pipeline_barrier command";
            return false;
        }
        return true;
    }
    if (k == vkrpc::CmdKind::ClearColorImage) {
        if (c.layout < 0) {
            err = "malformed clear_color_image command";
            return false;
        }
        if (!transfer_dst) {
            err = "clear_color_image target image lacks TRANSFER_DST usage";
            return false;
        }
        return true;
    }
    err = "unknown command kind";
    return false;
}

// rebuild the real VkMemoryBarrier2 / VkBufferMemoryBarrier2 /
// VkImageMemoryBarrier2 arrays from a typed DependencyInfo2 + the resolved host buffer/image
// handles (in wire order). 64-bit masks pass through full-width; queue families are NORMALIZED to
// VK_QUEUE_FAMILY_IGNORED (the recorder validated both were
// IGNORED-or-single-family with no ownership transfer, so IGNORED is the faithful host form; guest
// family 0 is never passed through as if it were the host family index). The out-vectors must
// outlive the pfn call.
void build_barriers2(const vkrpc::DependencyInfo2& d, const std::vector<VkBuffer>& bufs,
                     const std::vector<VkImage>& imgs, std::vector<VkMemoryBarrier2>& mem,
                     std::vector<VkBufferMemoryBarrier2>& buf,
                     std::vector<VkImageMemoryBarrier2>& img) {
    mem.assign(d.memory.size(), VkMemoryBarrier2{});
    for (std::size_t i = 0; i < d.memory.size(); ++i) {
        mem[i].sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
        mem[i].srcStageMask = d.memory[i].src_stage;
        mem[i].srcAccessMask = d.memory[i].src_access;
        mem[i].dstStageMask = d.memory[i].dst_stage;
        mem[i].dstAccessMask = d.memory[i].dst_access;
    }
    buf.assign(d.buffer.size(), VkBufferMemoryBarrier2{});
    for (std::size_t i = 0; i < d.buffer.size(); ++i) {
        buf[i].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
        buf[i].srcStageMask = d.buffer[i].src_stage;
        buf[i].srcAccessMask = d.buffer[i].src_access;
        buf[i].dstStageMask = d.buffer[i].dst_stage;
        buf[i].dstAccessMask = d.buffer[i].dst_access;
        buf[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        buf[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        buf[i].buffer = bufs[i];
        buf[i].offset = static_cast<VkDeviceSize>(d.buffer[i].offset);
        buf[i].size = static_cast<VkDeviceSize>(d.buffer[i].size);
    }
    img.assign(d.image.size(), VkImageMemoryBarrier2{});
    for (std::size_t i = 0; i < d.image.size(); ++i) {
        img[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        img[i].srcStageMask = d.image[i].src_stage;
        img[i].srcAccessMask = d.image[i].src_access;
        img[i].dstStageMask = d.image[i].dst_stage;
        img[i].dstAccessMask = d.image[i].dst_access;
        img[i].oldLayout = static_cast<VkImageLayout>(d.image[i].old_layout);
        img[i].newLayout = static_cast<VkImageLayout>(d.image[i].new_layout);
        img[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        img[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        img[i].image = imgs[i];
        img[i].subresourceRange.aspectMask = static_cast<VkImageAspectFlags>(d.image[i].aspect);
        img[i].subresourceRange.baseMipLevel = static_cast<std::uint32_t>(d.image[i].base_mip);
        img[i].subresourceRange.levelCount = static_cast<std::uint32_t>(d.image[i].level_count);
        img[i].subresourceRange.baseArrayLayer = static_cast<std::uint32_t>(d.image[i].base_layer);
        img[i].subresourceRange.layerCount = static_cast<std::uint32_t>(d.image[i].layer_count);
    }
}

VkDependencyInfo make_dependency_info2(const vkrpc::DependencyInfo2& d,
                                       const std::vector<VkMemoryBarrier2>& mem,
                                       const std::vector<VkBufferMemoryBarrier2>& buf,
                                       const std::vector<VkImageMemoryBarrier2>& img) {
    VkDependencyInfo di{};
    di.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    di.dependencyFlags = static_cast<VkDependencyFlags>(d.dependency_flags);
    di.memoryBarrierCount = static_cast<std::uint32_t>(mem.size());
    di.pMemoryBarriers = mem.empty() ? nullptr : mem.data();
    di.bufferMemoryBarrierCount = static_cast<std::uint32_t>(buf.size());
    di.pBufferMemoryBarriers = buf.empty() ? nullptr : buf.data();
    di.imageMemoryBarrierCount = static_cast<std::uint32_t>(img.size());
    di.pImageMemoryBarriers = img.empty() ? nullptr : img.data();
    return di;
}

// Records one (already-validated) command into `cmd`. Image commands (barrier/clear) use `image`;
// the draw subset uses the resolved render_pass/framebuffer/pipeline. Ints are cast to
// their Vulkan flag/enum types here (validated by the record state machine + the validation layer).
// `k` is the caller's already-computed kind -- the replay loop looks
// it up ONCE, so this helper does NOT recompute it (the "one lookup per command" invariant).
void emit_command(VkCommandBuffer cmd, const vkrpc::RecordedCommand& c, vkrpc::CmdKind k,
                  VkImage image, VkRenderPass render_pass, VkFramebuffer framebuffer,
                  VkPipeline pipeline, const std::vector<VkImageView>& begin_attachment_views,
                  VkExtent2D begin_framebuffer_extent) {
    if (k == vkrpc::CmdKind::BeginRenderPass) {
        // (GL/zink) faithful path (args_i64 non-empty): args_i64[0] = clearValueCount;
        // args_blob = the EXACT VkClearValue bytes. Forward them verbatim. Legacy named-field path
        // (vkcube/mock, args_i64 empty): color clear at [0], optional depth clear at [1].
        std::vector<VkClearValue> clears;
        if (!c.args_i64.empty()) {
            const auto count = static_cast<std::size_t>(c.args_i64[0]);
            clears.resize(count);
            if (count > 0 && c.args_blob.size() == count * sizeof(VkClearValue)) {
                std::memcpy(clears.data(), c.args_blob.data(), c.args_blob.size());
            }
        } else {
            clears.resize(c.has_depth_clear ? 2 : 1);
            clears[0].color.float32[0] = static_cast<float>(c.r);
            clears[0].color.float32[1] = static_cast<float>(c.g);
            clears[0].color.float32[2] = static_cast<float>(c.b);
            clears[0].color.float32[3] = static_cast<float>(c.a);
            if (c.has_depth_clear) {
                clears[1].depthStencil.depth = static_cast<float>(c.depth_clear);
                clears[1].depthStencil.stencil = 0;
            }
        }
        VkRenderPassBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        bi.renderPass = render_pass;
        bi.framebuffer = framebuffer;
        bi.renderArea.offset = {c.render_area_x, c.render_area_y};
        bi.renderArea.extent = {static_cast<std::uint32_t>(vkrpc::host_safe_render_extent(
                                    c.render_area_x, c.render_area_w,
                                    static_cast<int>(begin_framebuffer_extent.width))),
                                static_cast<std::uint32_t>(vkrpc::host_safe_render_extent(
                                    c.render_area_y, c.render_area_h,
                                    static_cast<int>(begin_framebuffer_extent.height)))};
        bi.clearValueCount = static_cast<std::uint32_t>(clears.size());
        bi.pClearValues = clears.empty() ? nullptr : clears.data();
        VkRenderPassAttachmentBeginInfo attachments{};
        if (!begin_attachment_views.empty()) {
            attachments.sType = VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO;
            attachments.attachmentCount = static_cast<std::uint32_t>(begin_attachment_views.size());
            attachments.pAttachments = begin_attachment_views.data();
            bi.pNext = &attachments;
        }
        vkCmdBeginRenderPass(cmd, &bi, VK_SUBPASS_CONTENTS_INLINE);
        return;
    }
    if (k == vkrpc::CmdKind::EndRenderPass) {
        vkCmdEndRenderPass(cmd);
        return;
    }
    if (k == vkrpc::CmdKind::BindPipeline) {
        // Compute: the carried bind point (args_i64[0]; absent = GRAPHICS so old
        // recordings replay unchanged). Validated to match the pipeline's kind upstream.
        const auto bind_point = c.args_i64.empty()
                                    ? VK_PIPELINE_BIND_POINT_GRAPHICS
                                    : static_cast<VkPipelineBindPoint>(c.args_i64[0]);
        vkCmdBindPipeline(cmd, bind_point, pipeline);
        return;
    }
    if (k == vkrpc::CmdKind::Dispatch) {
        vkCmdDispatch(cmd, static_cast<std::uint32_t>(c.args_u64[0]),
                      static_cast<std::uint32_t>(c.args_u64[1]),
                      static_cast<std::uint32_t>(c.args_u64[2]));
        return;
    }
    if (k == vkrpc::CmdKind::MemoryBarrierGlobal) {
        VkMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        b.srcAccessMask = static_cast<VkAccessFlags>(c.src_access);
        b.dstAccessMask = static_cast<VkAccessFlags>(c.dst_access);
        vkCmdPipelineBarrier(cmd, static_cast<VkPipelineStageFlags>(c.src_stage),
                             static_cast<VkPipelineStageFlags>(c.dst_stage), 0, 1, &b, 0, nullptr,
                             0, nullptr);
        return;
    }
    if (k == vkrpc::CmdKind::SetViewport) {
        VkViewport vp{};
        vp.x = static_cast<float>(c.vp_x);
        vp.y = static_cast<float>(c.vp_y);
        vp.width = static_cast<float>(c.vp_w);
        vp.height = static_cast<float>(c.vp_h);
        vp.minDepth = static_cast<float>(c.vp_min_depth);
        vp.maxDepth = static_cast<float>(c.vp_max_depth);
        vkCmdSetViewport(cmd, 0, 1, &vp);
        return;
    }
    if (k == vkrpc::CmdKind::SetScissor) {
        VkRect2D sc{};
        sc.offset = {c.sc_x, c.sc_y};
        sc.extent = {static_cast<std::uint32_t>(c.sc_w), static_cast<std::uint32_t>(c.sc_h)};
        vkCmdSetScissor(cmd, 0, 1, &sc);
        return;
    }
    if (k == vkrpc::CmdKind::Draw) {
        vkCmdDraw(cmd, static_cast<std::uint32_t>(c.vertex_count),
                  static_cast<std::uint32_t>(c.instance_count),
                  static_cast<std::uint32_t>(c.first_vertex),
                  static_cast<std::uint32_t>(c.first_instance));
        return;
    }
    if (k == vkrpc::CmdKind::PipelineBarrier) {
        // Replay the carried mip/layer range when present; the legacy
        // swapchain-color barrier left it missing (-1), which means the whole single subresource
        // (REMAINING_*).
        VkImageSubresourceRange range{};
        range.aspectMask = static_cast<VkImageAspectFlags>(c.aspect);
        const bool range_carried = c.barrier_base_mip >= 0 && c.barrier_level_count >= 0 &&
                                   c.barrier_base_layer >= 0 && c.barrier_layer_count >= 0;
        range.baseMipLevel = range_carried ? static_cast<std::uint32_t>(c.barrier_base_mip) : 0;
        range.levelCount = range_carried ? static_cast<std::uint32_t>(c.barrier_level_count)
                                         : VK_REMAINING_MIP_LEVELS;
        range.baseArrayLayer = range_carried ? static_cast<std::uint32_t>(c.barrier_base_layer) : 0;
        range.layerCount = range_carried ? static_cast<std::uint32_t>(c.barrier_layer_count)
                                         : VK_REMAINING_ARRAY_LAYERS;
        VkImageMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.oldLayout = static_cast<VkImageLayout>(c.old_layout);
        b.newLayout = static_cast<VkImageLayout>(c.new_layout);
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image;
        b.subresourceRange = range;
        b.srcAccessMask = static_cast<VkAccessFlags>(c.src_access);
        b.dstAccessMask = static_cast<VkAccessFlags>(c.dst_access);
        vkCmdPipelineBarrier(cmd, static_cast<VkPipelineStageFlags>(c.src_stage),
                             static_cast<VkPipelineStageFlags>(c.dst_stage), 0, 0, nullptr, 0,
                             nullptr, 1, &b);
        return;
    }
    // clear_color_image: full color subresource.
    VkImageSubresourceRange range{};
    range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    range.baseMipLevel = 0;
    range.levelCount = VK_REMAINING_MIP_LEVELS;
    range.baseArrayLayer = 0;
    range.layerCount = VK_REMAINING_ARRAY_LAYERS;
    VkClearColorValue color{};
    color.float32[0] = static_cast<float>(c.r);
    color.float32[1] = static_cast<float>(c.g);
    color.float32[2] = static_cast<float>(c.b);
    color.float32[3] = static_cast<float>(c.a);
    vkCmdClearColorImage(cmd, image, static_cast<VkImageLayout>(c.layout), &color, 1, &range);
}

// Packs (family, index) into a stable cache key, matching the mock's encoding.
std::uint64_t queue_key(std::uint32_t family, std::uint32_t index) {
    return (static_cast<std::uint64_t>(family) << 32) | index;
}

// First queue family with graphics support, or -1.
int graphics_queue_family(VkPhysicalDevice phys) {
    std::uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &count, families.data());
    for (std::uint32_t i = 0; i < count; ++i) {
        if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// Query pools (GL 3.3): the timestampValidBits of the chosen graphics family (0 if none / no
// timestamp support). zink gates ARB_timer_query on this being > 0; carried in DeviceCaps.
std::uint32_t graphics_family_timestamp_valid_bits(VkPhysicalDevice phys) {
    std::uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &count, families.data());
    for (std::uint32_t i = 0; i < count; ++i) {
        if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            return families[i].timestampValidBits;
        }
    }
    return 0;
}

// Compute: the chosen graphics family's REAL VkQueueFlags, carried in DeviceCaps so
// the ICD advertises compute HONESTLY (0 if no family -- the ICD then never sends compute).
std::uint64_t graphics_family_queue_flags(VkPhysicalDevice phys) {
    std::uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &count, families.data());
    for (std::uint32_t i = 0; i < count; ++i) {
        if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            return static_cast<std::uint64_t>(families[i].queueFlags);
        }
    }
    return 0;
}

// Destroys a transient instance on every return path of negotiate().
struct InstanceGuard {
    VkInstance instance = VK_NULL_HANDLE;
    ~InstanceGuard() {
        if (instance != VK_NULL_HANDLE) {
            vkDestroyInstance(instance, nullptr);
        }
    }
};

} // namespace

RealVulkanBackend::RealVulkanBackend(std::string gpu_name, std::string gpu_luid, bool gpu_required,
                                     const display::DisplayLayout* display_layout)
    : gpu_name_(std::move(gpu_name)), gpu_luid_(std::move(gpu_luid)), gpu_required_(gpu_required),
      present_fence_retire_requested_(
          vkrpc::present_fence_retire_requested(std::getenv("VKRELAY2_PRESENT_FENCE_RETIRE"))) {
    if (display_layout != nullptr) {
        display_layout_ = *display_layout;
        has_display_layout_ = true;
        windowing::WindowThread::set_host_origin(display_layout_.virtual_bounds.x,
                                                 display_layout_.virtual_bounds.y);
    } else {
        // Legacy/direct integration construction only. production always supplies the pinned
        // layout; retaining the prior work-origin behavior keeps old in-process tests compatible.
        const POINT legacy = windowing::win32_work_origin();
        windowing::WindowThread::set_host_origin(legacy.x, legacy.y);
    }
    // Test-only middle-tier selector: direct RealVulkanBackend tests normally construct the
    // lenient empty-name form. Let an explicit environment value choose the host adapter while
    // leaving every faithful production construction (gpu_required/name/LUID supplied) untouched.
    if (!gpu_required_ && gpu_name_.empty()) {
        const char* test_gpu = std::getenv("VKRELAY2_TEST_GPU");
        if (test_gpu != nullptr && test_gpu[0] != '\0') {
            gpu_name_ = test_gpu;
        }
    }
}

RealVulkanBackend::~RealVulkanBackend() {
    // Tear down anything the app left live, child-before-parent: device children
    // (swapchains, command pools, and the fence/semaphore/memory leaves) before their
    // devices -- a child outliving vkDestroyDevice is undefined -- then surfaces before
    // their instances, then devices/instances. Window destruction is marshaled to the
    // window thread, which is still alive here (it is a member, destroyed after this
    // body runs).
    //
    // Wait every live device idle first: a swapchain with an in-flight presented image
    // cannot be destroyed until those uses complete (VUID-...-swapchain-01282), and the
    // same idle guarantees the rest of the teardown frees nothing the GPU still reads.
    //
    // Lost-device containment: NEVER touch a latched-lost device here -- an
    // idle-wait or child-destroy on a wedged driver can block teardown forever. Its host objects
    // are abandoned to process termination (one session == one worker process), which is exactly
    // what the original vkrelay's lost_devices lesson prescribes. dev_vk_of below reports a lost
    // device as VK_NULL_HANDLE so every child-destroy loop skips its children uniformly.
    for (auto& kv : devices_) {
        if (kv.second.vk != VK_NULL_HANDLE && !kv.second.lost) {
            observe_device_result(kv.first, vkDeviceWaitIdle(kv.second.vk),
                                  "vkDeviceWaitIdle (destructor backstop)");
        }
    }
    // Draw-surface objects before the swapchains: framebuffers + pipelines first (they
    // reference render passes / layouts / views), then render passes / layouts / shaders, then
    // image views (which view swapchain images, so they must go before the swapchain frees those
    // images).
    const auto dev_vk_of = [&](std::uint64_t device) -> VkDevice {
        const auto d = devices_.find(device);
        // A latched-lost device reads as null here: its children are abandoned, never destroyed
        // through a possibly-wedged driver.
        return (d != devices_.end() && !d->second.lost) ? d->second.vk : VK_NULL_HANDLE;
    };
    for (auto& kv : framebuffers_) {
        VkDevice d = dev_vk_of(kv.second.device);
        if (kv.second.vk != VK_NULL_HANDLE && d != VK_NULL_HANDLE) {
            vkDestroyFramebuffer(d, kv.second.vk, nullptr);
        }
        if (d != VK_NULL_HANDLE) {
            for (auto& ce : kv.second.imageless_cache) { // (GL/zink): imageless cache
                vkDestroyFramebuffer(d, ce.second, nullptr);
            }
        }
    }
    for (auto& kv : pipelines_) {
        VkDevice d = dev_vk_of(kv.second.device);
        if (kv.second.vk != VK_NULL_HANDLE && d != VK_NULL_HANDLE) {
            vkDestroyPipeline(d, kv.second.vk, nullptr);
        }
    }
    for (auto& kv : render_passes_) {
        VkDevice d = dev_vk_of(kv.second.device);
        if (kv.second.vk != VK_NULL_HANDLE && d != VK_NULL_HANDLE) {
            vkDestroyRenderPass(d, kv.second.vk, nullptr);
        }
    }
    for (auto& kv : pipeline_layouts_) {
        VkDevice d = dev_vk_of(kv.second.device);
        if (kv.second.vk != VK_NULL_HANDLE && d != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(d, kv.second.vk, nullptr);
        }
    }
    for (auto& kv : shader_modules_) {
        VkDevice d = dev_vk_of(kv.second.device);
        if (kv.second.vk != VK_NULL_HANDLE && d != VK_NULL_HANDLE) {
            vkDestroyShaderModule(d, kv.second.vk, nullptr);
        }
    }
    for (auto& kv : image_views_) {
        VkDevice d = dev_vk_of(kv.second.device);
        if (kv.second.vk != VK_NULL_HANDLE && d != VK_NULL_HANDLE) {
            vkDestroyImageView(d, kv.second.vk, nullptr);
        }
    }
    // (GL/zink): texel buffer views before the buffers they view leave.
    for (auto& kv : buffer_views_) {
        VkDevice d = dev_vk_of(kv.second.device);
        if (kv.second.vk != VK_NULL_HANDLE && d != VK_NULL_HANDLE) {
            vkDestroyBufferView(d, kv.second.vk, nullptr);
        }
    }
    for (auto& kv : swapchains_) {
        VkDevice d = dev_vk_of(kv.second.device); // null for a lost device -> abandon
        // present-fence retirement: the worker-private present fences die with their
        // swapchain (before the device).
        for (VkFence f : kv.second.present_fences) {
            if (d != VK_NULL_HANDLE && f != VK_NULL_HANDLE) {
                vkDestroyFence(d, f, nullptr);
            }
        }
        if (kv.second.vk != VK_NULL_HANDLE && d != VK_NULL_HANDLE) {
            vkDestroySwapchainKHR(d, kv.second.vk, nullptr);
        }
    }
    // Buffers before the memory leaves they bind to (and before their device).
    for (auto& kv : buffers_) {
        VkDevice d = dev_vk_of(kv.second.device);
        if (kv.second.vk != VK_NULL_HANDLE && d != VK_NULL_HANDLE) {
            vkDestroyBuffer(d, kv.second.vk, nullptr);
        }
    }
    // App-created images likewise before the memory leaves + device. Swapchain images
    // are swapchain-owned (already torn down with the swapchain), so only sweep app_created ones.
    for (auto& kv : images_) {
        VkDevice d = dev_vk_of(kv.second.device);
        if (kv.second.app_created && kv.second.vk != VK_NULL_HANDLE && d != VK_NULL_HANDLE) {
            vkDestroyImage(d, kv.second.vk, nullptr);
        }
    }
    // Descriptor pools -- vkDestroyDescriptorPool frees the sets allocated from them
    // -- then the set layouts. Both before their device.
    for (auto& kv : descriptor_pools_) {
        VkDevice d = dev_vk_of(kv.second.device);
        if (kv.second.vk != VK_NULL_HANDLE && d != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(d, kv.second.vk, nullptr);
        }
    }
    for (auto& kv : descriptor_set_layouts_) {
        VkDevice d = dev_vk_of(kv.second.device);
        if (kv.second.vk != VK_NULL_HANDLE && d != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(d, kv.second.vk, nullptr);
        }
    }
    for (auto& kv : leaves_) {
        VkDevice d = dev_vk_of(kv.second.device); // null for a lost device -> abandon
        if (d == VK_NULL_HANDLE) {
            continue;
        }
        switch (kv.second.kind) {
        case LeafKind::Fence:
            vkDestroyFence(d, kv.second.fence, nullptr);
            break;
        case LeafKind::Semaphore:
            vkDestroySemaphore(d, kv.second.semaphore, nullptr);
            break;
        case LeafKind::Memory:
            vkFreeMemory(d, kv.second.memory, nullptr);
            break;
        }
    }
    for (auto& kv : pools_) {
        VkDevice d = dev_vk_of(kv.second.device); // null for a lost device -> abandon
        if (kv.second.vk != VK_NULL_HANDLE && d != VK_NULL_HANDLE) {
            vkDestroyCommandPool(d, kv.second.vk, nullptr);
        }
    }
    for (auto& kv : devices_) {
        // A lost device itself is abandoned too (vkDestroyDevice can also wedge on it).
        if (kv.second.vk != VK_NULL_HANDLE && !kv.second.lost) {
            vkDestroyDevice(kv.second.vk, nullptr);
        }
    }
    for (auto& kv : surfaces_) {
        const auto inst = instances_.find(kv.second.instance);
        // A tainted instance (it owned a lost, abandoned device) is not host-touched: its surfaces
        // are abandoned with it. The HWND destruction below stays (pure Win32, no driver entry).
        if (kv.second.vk != VK_NULL_HANDLE && inst != instances_.end() &&
            inst->second.vk != VK_NULL_HANDLE && !inst->second.lost_device_taint) {
            vkDestroySurfaceKHR(inst->second.vk, kv.second.vk, nullptr);
        }
        if (kv.second.hwnd != nullptr && window_thread_) {
            window_thread_->destroy_window(kv.second.hwnd);
        }
    }
    // any placeholder aux windows the sidecar plane left live (toplevels that never got
    // a surface and were not unregistered). Same window-thread teardown as surfaces. No lock needed
    // -- both planes have stopped before the backend is destroyed (session teardown closes both
    // listeners first).
    for (const auto& kv : placeholder_hwnds_) {
        if (kv.second != nullptr && window_thread_) {
            window_thread_->destroy_window(kv.second);
        }
    }
    placeholder_hwnds_.clear();
    for (auto& kv : instances_) {
        // A tainted instance is abandoned to process termination -- vkDestroyInstance under a live
        // abandoned VkDevice child is UB (observed AV in the loader/driver).
        if (kv.second.vk != VK_NULL_HANDLE && !kv.second.lost_device_taint) {
            vkDestroyInstance(kv.second.vk, nullptr);
        }
    }
}

vkrpc::CapabilitiesResponse RealVulkanBackend::negotiate(const vkrpc::CapabilitiesRequest& req) {
    vkrpc::CapabilitiesResponse resp;
    if (req.requested_api_major < 1 || req.requested_api_minor < 0) {
        resp.ok = false;
        resp.reason = "requested API version is invalid";
        return resp;
    }

    int inst_major = 1;
    int inst_minor = 0;
    query_instance_version(inst_major, inst_minor);

    // A transient instance just to query host capabilities + enumerate devices.
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.apiVersion = instance_request_version(inst_major, inst_minor);
    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;

    VkInstance instance = VK_NULL_HANDLE;
    if (vkCreateInstance(&ici, nullptr, &instance) != VK_SUCCESS || instance == VK_NULL_HANDLE) {
        resp.ok = false;
        resp.reason = "no usable Vulkan loader/driver";
        return resp;
    }
    const InstanceGuard guard{instance}; // destroyed on every return path below

    VkPhysicalDeviceProperties props{};
    if (select_physical_device(instance, gpu_required_, gpu_luid_, gpu_name_, props) ==
        VK_NULL_HANDLE) {
        resp.ok = false;
        resp.reason = "no Vulkan physical device";
        return resp;
    }

    const int dev_major = static_cast<int>(VK_API_VERSION_MAJOR(props.apiVersion));
    const int dev_minor = static_cast<int>(VK_API_VERSION_MINOR(props.apiVersion));

    // negotiated = min(requested, instance, device, worker-supported). A lesser
    // host (1.0-1.2) negotiates down to its real level rather than being rejected.
    int cap_major = vkrpc::kSupportedApiMajor;
    int cap_minor = vkrpc::kSupportedApiMinor;
    if (version_less(inst_major, inst_minor, cap_major, cap_minor)) {
        cap_major = inst_major;
        cap_minor = inst_minor;
    }
    if (version_less(dev_major, dev_minor, cap_major, cap_minor)) {
        cap_major = dev_major;
        cap_minor = dev_minor;
    }
    int nmaj = cap_major;
    int nmin = cap_minor;
    if (version_less(req.requested_api_major, req.requested_api_minor, cap_major, cap_minor)) {
        nmaj = req.requested_api_major;
        nmin = req.requested_api_minor;
    }

    resp.ok = true;
    resp.reason = "ok";
    resp.negotiated_api_major = nmaj;
    resp.negotiated_api_minor = nmin;
    resp.device = caps_from_props(props);

    VKR_INFO(kComponent) << "real negotiate -> device='" << resp.device.device_name << "' api "
                         << nmaj << "." << nmin << " (instance " << inst_major << "." << inst_minor
                         << ", device " << dev_major << "." << dev_minor << ")";
    return resp; // guard destroys the transient instance
}

sidecar::SidecarNegotiateResponse
RealVulkanBackend::negotiate(const sidecar::SidecarNegotiateRequest& req) {
    // Sidecar plane: a stateless version handshake, answered identically to the mock
    // (no host Vulkan involved) so mock == real on this plane. The registry ops are
    // where this single session object will join guest toplevels to surfaces/HWNDs.
    sidecar::SidecarNegotiateResponse resp;
    resp.protocol_version = sidecar::kSidecarProtocolVersion;
    if (req.protocol_version < 1) {
        resp.ok = false;
        resp.reason = "sidecar protocol version below the floor";
        return resp;
    }
    resp.ok = true;
    resp.reason = "ok";
    VKR_INFO(kComponent) << "sidecar negotiate -> ok (protocol " << resp.protocol_version << ")";
    return resp;
}

sidecar::SidecarReadyResponse
RealVulkanBackend::sidecar_ready(const sidecar::SidecarReadyRequest& req) {
    // record the readiness barrier (caps + initial scan). The launcher gates the app on
    // the sidecar's --ready-fd edge; the worker just records it (it already tolerates an early
    // surface via the born-correlated registry).
    sidecar::SidecarReadyResponse resp;
    if (has_display_layout_) {
        display::GuestDisplayState observed;
        observed.snapshot_id = req.display_snapshot_id;
        observed.actual_root_width = req.root_width;
        observed.actual_root_height = req.root_height;
        observed.output_model = display::OutputModel::SingleCanvas;
        const display::ValidationResult validation =
            display::validate_guest_display_state_against_layout(observed, display_layout_);
        if (!validation.ok) {
            resp.reason = validation.reason;
            VKR_ERROR(kComponent) << "sidecar ready rejected: " << validation.reason;
            return resp;
        }
    }
    sidecar_ready_ = true;
    // (cross-monitor maximize guard): publish the sidecar's OBSERVED guest root -- the single
    // source of truth for the guest-realizable extent. The WndProc caps
    // every host window's client to it on WM_GETMINMAXINFO, and the over-root guard in
    // get_surface_capabilities reads it back via WindowThread::guest_root_packed(). 0 = an older
    // sidecar that does not report it -> leave the cap unset (prior behavior). set_guest_root is
    // static (no window thread required); a window thread created later reads the same value.
    if (req.root_width != 0 && req.root_height != 0) {
        windowing::WindowThread::set_guest_root(req.root_width, req.root_height);
    }
    VKR_INFO(kComponent) << "sidecar ready: scan_generation=" << req.scan_generation
                         << " xcomposite=" << (req.has_xcomposite ? "yes" : "no")
                         << " xtest=" << (req.has_xtest ? "yes" : "no")
                         << " xfixes=" << (req.has_xfixes ? "yes" : "no")
                         << " initial_toplevels=" << req.initial_toplevels
                         << " guest-root=" << req.root_width << "x" << req.root_height
                         << " snapshot="
                         << (req.display_snapshot_id.empty() ? "legacy" : req.display_snapshot_id);
    resp.ok = true;
    resp.reason = "ok";
    return resp;
}

windowing::WindowThread* RealVulkanBackend::ensure_window_thread() {
    {
        std::lock_guard<std::mutex> lk(backend_mutex_);
        if (window_thread_) {
            return window_thread_.get();
        }
    }
    // Construct off-lock: the WindowThread ctor blocks until its message pump is ready, and the
    // rule is never to hold backend_mutex_ across a blocking wait. If two planes race
    // here, one fresh thread wins and the loser is destroyed off-lock (its dtor posts WM_QUIT +
    // joins).
    auto fresh =
        std::make_unique<windowing::WindowThread>(has_display_layout_ ? &display_layout_ : nullptr);
    std::unique_ptr<windowing::WindowThread> discard;
    windowing::WindowThread* result = nullptr;
    {
        std::lock_guard<std::mutex> lk(backend_mutex_);
        if (!window_thread_) {
            window_thread_ = std::move(fresh);
        } else {
            discard = std::move(fresh); // lost the race
        }
        result = window_thread_.get();
    }
    return result; // `discard` (if any) destroyed here, off-lock
}

void RealVulkanBackend::execute_create_placeholder(const sidecar::RegistryEffect& eff) {
    windowing::WindowThread* wt = ensure_window_thread();
    if (wt == nullptr) {
        // Could not start the window thread; the registry entry stays a placeholder with no HWND
        // (rare). A later unregister still drops the registry entry cleanly (no HWND to destroy).
        return;
    }
    // Placeholders start hidden, with one representation entry per toplevel. Chrome paint reveals
    // them. They are sized to the guest's reported geometry (best-effort static placement, followed
    // by live geometry updates). Zero/absent dimensions fall back to the surface default.
    const int w = eff.geometry.width > 0 ? static_cast<int>(eff.geometry.width) : 256;
    const int h = eff.geometry.height > 0 ? static_cast<int>(eff.geometry.height) : 256;
    windowing::CreatedWindow win;
    if (eff.is_popup) {
        // a popup host is an OWNED WS_POPUP window anchored to its owner toplevel's
        // host window (the static z-order). Resolve the owner HWND under the lock, then create
        // off-lock.
        HWND owner_hwnd = nullptr;
        {
            std::lock_guard<std::mutex> lk(backend_mutex_);
            owner_hwnd = hwnd_for_xid(eff.owner_xid);
        }
        if (owner_hwnd == nullptr) {
            // The owner's host window is not present (rare: it failed to create, or vanished).
            // Leave the registry popup entry without an HWND -- a later take_orphaned_popups (owner
            // teardown) or the popup's own unregister reaps it cleanly, exactly like the no-window
            // placeholder case above.
            return;
        }
        win = wt->create_popup_window(owner_hwnd, eff.geometry.x, eff.geometry.y, w, h); // off-lock
    } else {
        win = wt->create_hidden_window(w, h); // off-lock; blocks
    }
    HWND discard = nullptr;
    bool committed = false;
    std::uint64_t epoch = 0;
    {
        std::lock_guard<std::mutex> lk(backend_mutex_);
        // Re-check: did a racing create_surface promote/remove this placeholder (or, for a popup,
        // an owner teardown) while we were on the window thread? Commit the mapping only if the
        // registry still wants exactly this id -- and, for a popup, only if the owner is still
        // live.
        const bool wants = eff.is_popup
                               ? registry_.wants_popup(eff.xid, eff.placeholder_id, eff.owner_xid)
                               : registry_.wants_placeholder(eff.xid, eff.placeholder_id);
        if (win.hwnd != nullptr && wants) {
            placeholder_hwnds_[eff.placeholder_id] = win.hwnd;
            committed = true;
            epoch = registry_.epoch_for_xid(eff.xid);
        } else {
            discard = win.hwnd; // raced (promoted/destroyed/owner-gone) or creation failed
        }
    }
    if (discard != nullptr) {
        wt->destroy_window(discard); // off-lock
    } else if (committed) {
        // this placeholder is now the guest toplevel's visible representation -- bind
        // its slot so the WndProc captures input on it into the session ring, stamped with the xid
        // + this representation's epoch (off-lock, on the window thread). A later
        // unregister+re-register mints a NEW epoch, so this slot's in-flight events are then
        // dropped by poll_input's exact-epoch gate.
        wt->set_slot_input_target(win.hwnd, &input_queue_, eff.xid, epoch);
        if (debug_window_titles()) {
            wt->set_window_title_tag(win.hwnd, eff.xid); // dev-helper correlation
        }
        // place the brand-new host at the register geometry immediately
        // (initial placement -- a map-once-never-move app like vkcube otherwise stays at
        // CW_USEDEFAULT; a popup is routed through the coordinate helper / work-origin mapping
        // rather than its create-time raw x,y). Seq-gated; the window is still hidden. apply_size
        // is normally false at register (the app owns the extent) -> position-only.
        if (eff.host_apply_seq != 0) {
            wt->apply_geometry(
                win.hwnd, static_cast<int>(eff.geometry.x), static_cast<int>(eff.geometry.y),
                static_cast<int>(eff.geometry.width), static_cast<int>(eff.geometry.height),
                eff.host_apply_seq, eff.apply_size, eff.z_order); // z_order None on an establish
        }
    }
}

sidecar::SidecarToplevelResponse
RealVulkanBackend::register_toplevel(const sidecar::SidecarRegisterToplevelRequest& req) {
    sidecar::SidecarToplevelResponse resp;
    resp.xid = req.xid;
    resp.ok = true;
    // Classifier (widened for the fullscreen class): the sidecar sends
    // override_redirect windows in exactly two CLASSIFIED shapes -- a popup (is_popup, owned by
    // its anchor; the registry refuses it itself if owner_xid is not a live non-popup anchor) or a
    // FULLSCREEN toplevel (is_popup=false: a root-covering window, SFML 2.5's non-EWMH fullscreen
    // -- the ExtremeTuxRacer class). Both are deliberate sidecar decisions, so both register; the
    // old refuse-all-non-popup gate predates the fullscreen classification and stranded the
    // fullscreen window on the create_surface 256x256 default host.
    sidecar::RegistryEffect eff;
    {
        std::lock_guard<std::mutex> lk(backend_mutex_);
        eff = registry_.register_toplevel(req.xid, req.generation, req.role, req.title,
                                          {req.x, req.y, req.width, req.height}, req.is_popup,
                                          req.owner_xid, req.popup_kind);
    }
    if (eff.kind == sidecar::RegistryEffect::Kind::CreatePlaceholder) {
        execute_create_placeholder(eff); // the off-lock mutex dance (it also places the new HWND)
    } else if (eff.host_apply_seq != 0) {
        // a surface-first / re-register transition re-places the
        // ALREADY-created host HWND at the (re-)registered geometry (initial placement).
        // CreatePlaceholder places its own brand-new HWND inside execute_create_placeholder.
        //
        // register-late ADOPTION: a register's apply is normally position-only
        // (apply_size = extent_authoritative = false at register -- the app owns the extent). But a
        // surface-first DEFERRED surface (zink created its swapchain before this register, so it
        // has no app-chosen extent and fell back to the default) must ADOPT the now-known
        // registered geometry SIZE once -- else it stays stranded at the default until a later real
        // resize. So when this xid is backed by a deferred surface AND the registered geometry is
        // non-degenerate, force the size apply. This rides the register's generation/seq gate (so a
        // stale geometry cannot replay onto a recycled XID) and is NOT a registry-wide change.
        //
        // SURFACE-SPECIFIC -- adopt only when the surface that recorded the
        // deferred bit is STILL the current bound surface for this xid. Same-xid surface
        // replacement means an old surface's deferred bit must NOT force a size apply onto a newer
        // (possibly non-deferred) current surface.
        bool adopt_size = false;
        {
            std::lock_guard<std::mutex> lk(backend_mutex_);
            const auto d = deferred_extent_surface_by_xid_.find(eff.xid);
            adopt_size = d != deferred_extent_surface_by_xid_.end() && d->second != 0 &&
                         d->second == registry_.surface_for_xid(eff.xid);
        }
        adopt_size = adopt_size && eff.geometry.width > 0 && eff.geometry.height > 0;
        apply_geometry_move(eff.xid, eff.geometry, eff.host_apply_seq, eff.apply_size || adopt_size,
                            eff.z_order);
    }
    // If a surface already backs this xid, reconcile its slot's input epoch (a surface-backed remap
    // just minted a new one). A no-op for a placeholder-only / pending xid.
    if (eff.applied) {
        reconcile_surface_input_epoch(req.xid);
    }
    resp.applied = eff.applied;
    {
        std::lock_guard<std::mutex> lk(backend_mutex_);
        resp.representation =
            sidecar::representation_name(registry_.representation_for_xid(req.xid));
        resp.epoch = registry_.epoch_for_xid(req.xid);
    }
    resp.reason = "ok";
    return resp;
}

void RealVulkanBackend::reconcile_surface_input_epoch(std::uint64_t xid) {
    HWND hwnd = nullptr;
    std::uint64_t epoch = 0;
    {
        std::lock_guard<std::mutex> lk(backend_mutex_);
        const auto it = surface_input_hwnds_.find(xid);
        if (it == surface_input_hwnds_.end()) {
            return; // no surface backs this xid (placeholder-only / pending) -> nothing to rebind
        }
        hwnd = it->second;
        epoch = registry_.epoch_for_xid(xid);
    }
    // Off-lock (the window thread serializes; a window destroyed by a racing destroy_surface just
    // makes set_slot_input_target a no-op -- the slot is gone).
    if (hwnd != nullptr && window_thread_) {
        window_thread_->set_slot_input_target(hwnd, &input_queue_, xid, epoch);
    }
}

sidecar::SidecarToplevelResponse
RealVulkanBackend::update_toplevel(const sidecar::SidecarUpdateToplevelRequest& req) {
    sidecar::SidecarToplevelResponse resp;
    resp.xid = req.xid;
    resp.ok = true;
    sidecar::RegistryEffect eff;
    {
        std::lock_guard<std::mutex> lk(backend_mutex_);
        eff = registry_.update_toplevel(req.xid, req.generation, req.role,
                                        {req.x, req.y, req.width, req.height},
                                        static_cast<sidecar::ZOrder>(req.z_order));
        resp.representation =
            sidecar::representation_name(registry_.representation_for_xid(req.xid));
        resp.epoch = registry_.epoch_for_xid(req.xid);
    }
    // a position, size, or z-order change yields ApplyGeometry ->
    // drive a LIVE host move/resize/restack (the representation epoch is deliberately UNCHANGED by
    // an update, so in-flight input survives). apply_geometry_move resolves
    // the HWND + marshals the apply off-lock; eff.apply_size says whether to size the client (
    // resize), eff.z_order whether to restack.
    if (eff.kind == sidecar::RegistryEffect::Kind::ApplyGeometry) {
        // Reveal ordering: apply_geometry_move -> WindowThread::apply_geometry uses the
        // blocking run_on_thread path. For a popup its slot lock is uncontended, so drain_geometry
        // realizes the HWND rect before this UpdateToplevel RPC returns. A following in-order
        // SetToplevelVisibility RPC therefore cannot reveal the prior popup position.
        apply_geometry_move(req.xid, eff.geometry, eff.host_apply_seq, eff.apply_size, eff.z_order);
    }
    resp.applied = eff.applied;
    resp.reason = "ok";
    return resp;
}

void RealVulkanBackend::apply_geometry_move(std::uint64_t xid,
                                            const sidecar::ToplevelGeometry& geometry,
                                            std::uint64_t host_apply_seq, bool apply_size,
                                            sidecar::ZOrder z_order) {
    HWND hwnd = nullptr;
    {
        std::lock_guard<std::mutex> lk(backend_mutex_);
        hwnd = hwnd_for_xid(xid); // surface OR placeholder/popup; null for a None representation
    }
    // The per-host latest-desired cell + the seq gate live in the WindowSlot (window-thread-owned),
    // so apply_geometry coalesces a storm to the newest authored geometry and never blocks a
    // present.
    if (hwnd != nullptr && window_thread_) {
        window_thread_->apply_geometry(
            hwnd, static_cast<int>(geometry.x), static_cast<int>(geometry.y),
            static_cast<int>(geometry.width), static_cast<int>(geometry.height), host_apply_seq,
            apply_size, z_order);
    }
}

sidecar::SidecarToplevelResponse
RealVulkanBackend::unregister_toplevel(const sidecar::SidecarUnregisterToplevelRequest& req) {
    sidecar::SidecarToplevelResponse resp;
    resp.xid = req.xid;
    resp.ok = true;
    sidecar::RegistryEffect eff;
    HWND to_destroy = nullptr;
    HWND surface_cursor_to_clear = nullptr;
    std::vector<HWND> popups_to_destroy;
    {
        std::lock_guard<std::mutex> lk(backend_mutex_);
        eff = registry_.unregister_toplevel(req.xid, req.generation);
        if (eff.kind == sidecar::RegistryEffect::Kind::DestroyPlaceholder) {
            const auto it = placeholder_hwnds_.find(eff.placeholder_id);
            if (it != placeholder_hwnds_.end()) {
                to_destroy = it->second;
                placeholder_hwnds_.erase(it);
            }
        }
        // an applied unregister that leaves a SURFACE HWND alive (the surface
        // outlives the X toplevel under Model A) must also clear that window's installed cursor.
        // The epoch gate already rejects a NEW stale cursor, but the cursor installed during the
        // old lifecycle would otherwise survive into a re-register until a fresh one happens to
        // arrive. A placeholder-backed xid needs no clear here -- its HWND (and slot + HCURSOR) is
        // destroyed above.
        if (eff.applied) {
            const auto sh = surface_input_hwnds_.find(req.xid);
            if (sh != surface_input_hwnds_.end()) {
                surface_cursor_to_clear = sh->second;
            }
            // owner-teardown CASCADE. If this toplevel owned popups, drop
            // them from the registry too and collect their host HWNDs for off-lock destruction --
            // Win32 owned-window destruction alone would leave orphan logical popups in
            // DebugEnumWindows. A no-op for an xid that owns no popups (incl. a popup's own
            // unregister).
            for (const auto& pe : registry_.take_orphaned_popups(req.xid)) {
                if (pe.kind == sidecar::RegistryEffect::Kind::DestroyPlaceholder) {
                    const auto it = placeholder_hwnds_.find(pe.placeholder_id);
                    if (it != placeholder_hwnds_.end()) {
                        popups_to_destroy.push_back(it->second);
                        placeholder_hwnds_.erase(it);
                    }
                }
            }
        }
        resp.representation =
            sidecar::representation_name(registry_.representation_for_xid(req.xid));
        resp.epoch = registry_.epoch_for_xid(req.xid);
    }
    if (to_destroy != nullptr && window_thread_) {
        window_thread_->destroy_window(to_destroy); // off-lock
    }
    if (surface_cursor_to_clear != nullptr && window_thread_) {
        window_thread_->clear_window_cursor(surface_cursor_to_clear); // off-lock
    }
    for (HWND popup : popups_to_destroy) {
        if (window_thread_ != nullptr) {
            window_thread_->destroy_window(popup); // off-lock (the owned popup hosts)
        }
    }
    resp.applied = eff.applied;
    resp.reason = "ok";
    return resp;
}

bool RealVulkanBackend::paint_eligible_locked(std::uint64_t xid) const {
    const sidecar::Representation rep = registry_.representation_for_xid(xid);
    if (rep == sidecar::Representation::Placeholder) {
        return registry_.placeholder_shown(xid); // committed its first chrome paint
    }
    if (rep == sidecar::Representation::Surface) {
        const auto it = surfaces_.find(registry_.surface_for_xid(xid));
        return it != surfaces_.end() && it->second.shown; // had its first successful present
    }
    return false;
}

sidecar::SidecarToplevelResponse
RealVulkanBackend::set_visibility(const sidecar::SidecarSetVisibilityRequest& req) {
    sidecar::SidecarToplevelResponse resp;
    resp.xid = req.xid;
    resp.ok = true;
    sidecar::RegistryEffect eff;
    // The host windows to ShowWindow off-lock: the target + (if it is a non-popup owner) each owned
    // popup re-asserted per its OWN visibility (an owner hide/restore cascades the popups VISUALLY
    // but never resurrects one that was independently unmapped). All resolved under the lock; the
    // ShowWindow marshals off-lock (never an invoke under backend_mutex_).
    struct VisApply {
        HWND hwnd = nullptr;
        sidecar::VisibilityState state = sidecar::VisibilityState::Visible;
        bool should_show = false;
        std::uint64_t seq = 0;
    };
    std::vector<VisApply> applies;
    {
        std::lock_guard<std::mutex> lk(backend_mutex_);
        eff = registry_.set_visibility(req.xid, req.generation,
                                       static_cast<sidecar::VisibilityState>(req.visibility_state));
        resp.representation =
            sidecar::representation_name(registry_.representation_for_xid(req.xid));
        resp.epoch = registry_.epoch_for_xid(req.xid);
        if (eff.kind == sidecar::RegistryEffect::Kind::SetVisibility) {
            HWND h = hwnd_for_xid(req.xid);
            if (h != nullptr) {
                const bool show = eff.visibility_state == sidecar::VisibilityState::Visible &&
                                  paint_eligible_locked(req.xid);
                applies.push_back({h, eff.visibility_state, show, eff.host_apply_seq});
            }
            // Owner cascade: a non-popup's hide/show drives its owned
            // popups, each re-asserted per its OWN visibility. Reuses eff.host_apply_seq (a
            // different HWND -> a different slot/gate; the cascade is one logical mutation). An
            // owner restore shows a popup ONLY if the popup's own intent is Visible (no
            // resurrection).
            if (!registry_.is_popup(req.xid)) {
                const bool owner_visible =
                    eff.visibility_state == sidecar::VisibilityState::Visible;
                for (const auto& p : registry_.owned_popups(req.xid)) {
                    HWND ph = hwnd_for_xid(p.xid);
                    if (ph == nullptr) {
                        continue;
                    }
                    const bool popup_visible =
                        owner_visible && p.visibility_state == sidecar::VisibilityState::Visible;
                    const sidecar::VisibilityState pstate = popup_visible
                                                                ? sidecar::VisibilityState::Visible
                                                                : sidecar::VisibilityState::Hidden;
                    const bool pshow = popup_visible && paint_eligible_locked(p.xid);
                    applies.push_back({ph, pstate, pshow, eff.host_apply_seq});
                }
            }
        }
    }
    for (const auto& a : applies) {
        if (window_thread_ != nullptr) {
            window_thread_->apply_visibility(a.hwnd, a.state, a.should_show, a.seq);
        }
    }
    resp.applied = eff.applied;
    resp.reason = "ok";
    return resp;
}

sidecar::SidecarPaintResponse
RealVulkanBackend::paint_chrome(const sidecar::SidecarPaintChromeRequest& req) {
    sidecar::SidecarPaintResponse resp;
    resp.xid = req.xid;
    resp.ok = true;
    resp.reason = "ok";
    // The accept -> paint(hidden) -> commit -> show dance: decide under the lock; STAGE the DIB on
    // the window thread OFF-lock (never hold backend_mutex_ across an invoke) with the window still
    // hidden; commit shown/last_seq ONLY after a realized paint; reveal the window ONLY after the
    // commit re- check succeeds. A racing data-plane promote between accept and commit fails the
    // commit, so the placeholder is never briefly visible while uncommitted.
    sidecar::WindowRegistry::PlaceholderPaintDecision d;
    HWND hwnd = nullptr;
    {
        std::lock_guard<std::mutex> lk(backend_mutex_);
        d = registry_.accept_placeholder_paint(req.xid, req.lifecycle_generation, req.seq);
        if (d.accepted) {
            const auto it = placeholder_hwnds_.find(d.placeholder_id);
            if (it != placeholder_hwnds_.end()) {
                hwnd = it->second;
            }
        }
    }
    bool painted = false;
    if (d.accepted && hwnd != nullptr && window_thread_) {
        painted = window_thread_->paint_aux(
            hwnd, reinterpret_cast<const unsigned char*>(req.pixels.data()),
            static_cast<int>(req.src_w), static_cast<int>(req.src_h), req.dirty_x, req.dirty_y,
            static_cast<int>(req.dirty_w), static_cast<int>(req.dirty_h),
            static_cast<int>(req.stride)); // off-lock; window stays hidden
    }
    bool committed = false;
    bool intends_visible = true;
    {
        std::lock_guard<std::mutex> lk(backend_mutex_);
        if (painted) {
            committed =
                registry_.commit_placeholder_paint(req.xid, req.lifecycle_generation, req.seq);
        }
        resp.representation =
            sidecar::representation_name(registry_.representation_for_xid(req.xid));
        resp.shown = registry_.placeholder_shown(req.xid);
        resp.last_seq = registry_.last_paint_seq(req.xid);
        // unified reveal predicate: the first-paint reveal must ALSO honor the live
        // visibility intent. A placeholder that was hidden (UnmapNotify) before its first paint
        // still COMMITS (shown=true == paint-eligible) but must NOT be revealed; a later restore
        // (set_visibility Visible) then shows it via the now-true paint-eligibility.
        intends_visible =
            registry_.visibility_state_for_xid(req.xid) == sidecar::VisibilityState::Visible;
    }
    // Reveal ONLY after a committed first paint (subsequent paints repaint the already-shown window
    // inside paint_aux), AND only while the toplevel intends to be visible; a lost commit
    // race means the window was never shown.
    if (committed && d.first_paint && intends_visible && hwnd != nullptr && window_thread_) {
        window_thread_->show_aux_window(hwnd); // off-lock
    }
    resp.applied = committed; // a realized + committed paint
    return resp;
}

sidecar::SidecarDebugChromeStateResponse
RealVulkanBackend::debug_chrome_state(const sidecar::SidecarDebugChromeStateRequest& req) {
    sidecar::SidecarDebugChromeStateResponse resp;
    resp.xid = req.xid;
    resp.ok = true;
    resp.reason = "ok";
    HWND hwnd = nullptr;
    {
        std::lock_guard<std::mutex> lk(backend_mutex_);
        resp.representation =
            sidecar::representation_name(registry_.representation_for_xid(req.xid));
        resp.shown = registry_.placeholder_shown(req.xid);
        resp.last_seq = registry_.last_paint_seq(req.xid);
        const auto it = placeholder_hwnds_.find(registry_.placeholder_id_for_xid(req.xid));
        if (it != placeholder_hwnds_.end()) {
            hwnd = it->second;
        }
    }
    if (hwnd != nullptr && window_thread_) {
        std::uint32_t px = 0;
        if (window_thread_->sample_aux_pixel(hwnd, req.sample_x, req.sample_y, px)) {
            resp.pixel_bgra = px;
            resp.has_pixel = true;
        }
    }
    return resp;
}

sidecar::SidecarPollInputResponse
RealVulkanBackend::poll_input(const sidecar::SidecarPollInputRequest& req) {
    sidecar::SidecarPollInputResponse resp;
    resp.ok = true;
    resp.reason = "ok";
    // Drain under the ring's OWN mutex (the WndProc fills it concurrently on the window thread; no
    // backend_mutex_ here). Then apply the EXACT-EPOCH gate under backend_mutex_ (two locks, never
    // nested): keep only events whose epoch matches the xid's current representation epoch --
    // survives a resize (epoch unchanged), drops after unregister/destroy (epoch 0) or an
    // unregister+re-register / promote of the same xid (a new epoch).
    std::vector<sidecar::SidecarInputEvent> drained;
    std::uint64_t next_seq = req.since_seq;
    resp.dropped = input_queue_.drain(req.since_seq, drained, next_seq);
    resp.next_seq = next_seq;
    {
        std::lock_guard<std::mutex> lk(backend_mutex_);
        for (const auto& e : drained) {
            const std::uint64_t current_epoch = registry_.epoch_for_xid(e.xid);
            const bool accepted = current_epoch == e.epoch;
            if (input_trace_enabled()) {
                VKR_INFO(kComponent)
                    << "input-trace station=poll-input xid=" << e.xid << " event-epoch=" << e.epoch
                    << " current-epoch=" << current_epoch << " kind=" << e.kind << " seq=" << e.seq
                    << " decision=" << (accepted ? "accept" : "drop");
            }
            if (accepted) {
                resp.events.push_back(e);
            }
        }
    }
    return resp;
}

sidecar::SidecarDebugEnqueueInputResponse
RealVulkanBackend::debug_enqueue_input(const sidecar::SidecarDebugEnqueueInputRequest& req) {
    sidecar::SidecarDebugEnqueueInputResponse resp;
    resp.xid = req.xid;
    resp.ok = true;
    resp.reason = "ok";
    int enqueued = 0;
    for (const auto& e : req.events) {
        // The request's xid/epoch are authoritative (the WndProc stamps the slot's, never the
        // per-event's); the ring re-stamps a fresh session seq.
        input_queue_.enqueue(req.xid, req.epoch, e);
        ++enqueued;
    }
    resp.enqueued = enqueued;
    return resp;
}

HWND RealVulkanBackend::hwnd_for_xid(std::uint64_t xid) const {
    const auto s = surface_input_hwnds_.find(xid);
    if (s != surface_input_hwnds_.end()) {
        return s->second; // a surface window represents the xid (Model A)
    }
    const auto p = placeholder_hwnds_.find(registry_.placeholder_id_for_xid(xid));
    return p == placeholder_hwnds_.end() ? nullptr : p->second; // else its placeholder window
}

sidecar::SidecarSetCursorResponse
RealVulkanBackend::set_cursor(const sidecar::SidecarSetCursorRequest& req) {
    sidecar::SidecarSetCursorResponse resp;
    resp.xid = req.xid;
    resp.ok = true;
    resp.reason = "ok";
    HWND hwnd = nullptr;
    {
        std::lock_guard<std::mutex> lk(backend_mutex_);
        // Exact-epoch + still-registered gate: reject a cursor for an xid
        // that is no longer a registered toplevel (unregister/destroy) or whose epoch has moved on
        // (a remap), so a stale cursor never installs on a new X lifecycle -- including a
        // surface-backed entry an unregister left alive (toplevel_registered is then false even
        // though the surface HWND persists).
        if (registry_.toplevel_registered(req.xid) &&
            req.epoch == registry_.epoch_for_xid(req.xid)) {
            hwnd = hwnd_for_xid(req.xid);
        }
    }
    // Build + bind the HCURSOR on the window thread (off-lock). The decoder validated
    // pixels.size() == width*height*4, so the buffer is exact.
    if (hwnd != nullptr && window_thread_) {
        resp.applied = window_thread_->set_window_cursor(
            hwnd, reinterpret_cast<const unsigned char*>(req.pixels.data()),
            static_cast<int>(req.width), static_cast<int>(req.height), req.xhot, req.yhot);
    }
    return resp;
}

sidecar::SidecarDebugCursorStateResponse
RealVulkanBackend::debug_cursor_state(const sidecar::SidecarDebugCursorStateRequest& req) {
    sidecar::SidecarDebugCursorStateResponse resp;
    resp.xid = req.xid;
    resp.ok = true;
    resp.reason = "ok";
    HWND hwnd = nullptr;
    {
        std::lock_guard<std::mutex> lk(backend_mutex_);
        hwnd = hwnd_for_xid(req.xid);
    }
    if (hwnd != nullptr && window_thread_) {
        int w = 0, h = 0, xhot = 0, yhot = 0;
        bool has_pixel = false;
        std::uint32_t px = 0;
        if (window_thread_->debug_cursor(hwnd, req.sample_x, req.sample_y, w, h, xhot, yhot,
                                         has_pixel, px)) {
            resp.has_cursor = true;
            resp.width = static_cast<std::uint32_t>(w);
            resp.height = static_cast<std::uint32_t>(h);
            resp.xhot = xhot;
            resp.yhot = yhot;
            resp.has_pixel = has_pixel;
            resp.pixel_bgra = px;
        }
    }
    return resp;
}

sidecar::SidecarDebugEnumWindowsResponse
RealVulkanBackend::debug_enum_windows(const sidecar::SidecarDebugEnumWindowsRequest& req) {
    sidecar::SidecarDebugEnumWindowsResponse resp;
    resp.ok = true;
    resp.reason = "ok";
    // Snapshot the registry rows UNDER the lock; for include_actual also resolve each host HWND
    // into a parallel list THEN release the lock -- the window-thread reads (query_geometry) run
    // OFF-lock: never invoke the callback while holding backend_mutex_. The default (no
    // include_actual) stays a pure-registry, no-invoke query, identical to the mock (mock == real).
    std::vector<sidecar::WindowRegistry::Entry> entries;
    std::vector<HWND> hwnds; // parallel to `entries`; only filled when include_actual
    {
        std::lock_guard<std::mutex> lk(backend_mutex_);
        entries = registry_.snapshot();
        if (req.include_actual) {
            hwnds.reserve(entries.size());
            for (const auto& e : entries) {
                hwnds.push_back(hwnd_for_xid(e.xid));
            }
        }
    }
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const sidecar::WindowRegistry::Entry& e = entries[i];
        sidecar::SidecarWindowInfo w;
        w.xid = e.xid;
        w.representation = sidecar::representation_name(e.representation);
        w.toplevel_registered = e.toplevel_registered;
        w.has_surface = e.surface != 0;
        w.generation = e.generation;
        w.epoch = e.representation_epoch;
        w.last_paint_seq = e.last_paint_seq;
        w.shown = e.shown;
        w.role = e.role;
        w.title = e.title;
        w.x = e.geometry.x;
        w.y = e.geometry.y;
        w.width = e.geometry.width;
        w.height = e.geometry.height;
        w.is_popup = e.is_popup; // the worker-visible owner/z-order proof
        w.owner_xid = e.owner_xid;
        w.popup_kind = e.popup_kind;
        w.z_order = static_cast<std::uint32_t>(e.last_z_order); // last restack (pure registry)
        w.visibility_state = static_cast<std::uint32_t>(e.visibility_state); // authored visibility
        if (req.include_actual && hwnds[i] != nullptr && window_thread_) {
            // Worker-visible convergence proof: the host window's ACTUAL geometry,
            // read off the window thread. actual_x/actual_y are the CLIENT origin mapped back to
            // X-root coords (screen client origin - work origin), so they compare directly to the
            // authored x/y; frame_* is the raw Win32 OUTER rect (screen); actual_width/height is
            // the client extent. clamped flags that Win32 could not realize the authored geometry
            // -- the achieved POSITION differs (e.g. off-monitor) OR, once the sidecar is the
            // extent authority, the achieved SIZE differs (e.g. a tiny authored extent
            // clamped up to the Win32 minimum). So a clamp is visible in DebugEnumWindows.
            RECT frame{};
            int cw = 0, ch = 0;
            POINT origin{0, 0};
            POINT work{0, 0};
            std::uint64_t applied_seq = 0;
            bool host_visible = false, host_iconic = false;
            if (window_thread_->query_geometry(hwnds[i], frame, cw, ch, origin, work, applied_seq,
                                               host_visible, host_iconic)) {
                w.has_actual = true;
                w.host_visible = host_visible; // host-observed visibility (IsWindowVisible)
                w.host_iconic = host_iconic;
                w.actual_x = static_cast<std::int32_t>(origin.x - work.x);
                w.actual_y = static_cast<std::int32_t>(origin.y - work.y);
                w.actual_width = static_cast<std::uint32_t>(cw < 0 ? 0 : cw);
                w.actual_height = static_cast<std::uint32_t>(ch < 0 ? 0 : ch);
                w.frame_x = static_cast<std::int32_t>(frame.left);
                w.frame_y = static_cast<std::int32_t>(frame.top);
                w.frame_width = static_cast<std::uint32_t>(frame.right - frame.left);
                w.frame_height = static_cast<std::uint32_t>(frame.bottom - frame.top);
                w.last_host_apply_seq = applied_seq;
                const bool pos_clamped = w.actual_x != e.geometry.x || w.actual_y != e.geometry.y;
                // Size is sidecar-authored only when extent_authoritative (else the client is the
                // app's own swapchain extent, legitimately != the registered geometry -- not a
                // clamp).
                const bool size_clamped =
                    e.extent_authoritative &&
                    (w.actual_width != e.geometry.width || w.actual_height != e.geometry.height);
                w.clamped = applied_seq != 0 && (pos_clamped || size_clamped);
            }
        }
        resp.windows.push_back(std::move(w));
    }
    return resp;
}

sidecar::SidecarDebugCaptureWindowResponse
RealVulkanBackend::debug_capture_window(const sidecar::SidecarDebugCaptureWindowRequest& req) {
    sidecar::SidecarDebugCaptureWindowResponse resp;
    resp.xid = req.xid;
    resp.layer = req.layer;
    const bool is_chrome = req.layer == sidecar::kCaptureLayerChrome;
    const bool is_cursor = req.layer == sidecar::kCaptureLayerCursor;
    if (!is_chrome && !is_cursor) {
        resp.status = "bad_layer";
        resp.reason = "unknown capture layer";
        return resp;
    }
    // A small helper: do the lifecycle selectors (0 = do not check) match the
    // metadata currently in `resp`? Used both before the copy (preflight) and after (the re-check).
    const auto selectors_match = [&]() {
        return (req.expected_epoch == 0 || req.expected_epoch == resp.epoch) &&
               (req.expected_lifecycle_generation == 0 ||
                req.expected_lifecycle_generation == resp.generation) &&
               (req.min_last_seq == 0 || resp.last_paint_seq >= req.min_last_seq);
    };
    HWND snap_hwnd = nullptr;
    std::uint64_t snap_epoch = 0;
    {
        std::lock_guard<std::mutex> lk(backend_mutex_);
        // Metadata is always reported (so a stale artifact is spottable even without a selector).
        resp.representation =
            sidecar::representation_name(registry_.representation_for_xid(req.xid));
        resp.generation = registry_.generation_for_xid(req.xid);
        resp.epoch = registry_.epoch_for_xid(req.xid);
        resp.last_paint_seq = registry_.last_paint_seq(req.xid);
        resp.shown = registry_.placeholder_shown(req.xid);
        snap_hwnd = hwnd_for_xid(req.xid);
        if (snap_hwnd == nullptr) {
            resp.status = "absent";
            resp.reason = "no live window for xid";
            return resp;
        }
        if (!selectors_match()) {
            resp.status = "mismatch";
            resp.reason = "lifecycle selector did not match current registry state";
            return resp;
        }
        snap_epoch = resp.epoch; // the representation-instance identity to re-confirm post-copy
    }
    // Capture the source buffer OFF-lock (never a WindowThread invoke under backend_mutex_). A
    // window a racing destroy tore down makes this copy fail; the post-copy re-check below then
    // reports absent/mismatch (NOT "empty") -- see that block.
    int w = 0, h = 0, stride = 0, xhot = 0, yhot = 0;
    std::vector<unsigned char> px;
    bool got = false;
    if (window_thread_) {
        got = is_chrome
                  ? window_thread_->debug_capture_chrome(snap_hwnd, w, h, stride, px)
                  : window_thread_->debug_capture_cursor(snap_hwnd, w, h, xhot, yhot, stride, px);
    }
    // Post-copy re-check: a data-plane promote/unbind, an
    // unregister+re-register, or a placeholder destroy could have invalidated the lifecycle AFTER
    // the preflight, DURING the off-lock copy -- a screenshot is evidence, so it must reflect the
    // same lifecycle end to end. This runs REGARDLESS of `got`: a raced teardown also makes the
    // copy fail, and that must report `absent`/`mismatch`, NOT a misleading `empty` (window exists,
    // no content). Re-read metadata, then require the SAME HWND + the SAME representation epoch (a
    // benign resize bumps generation but not the epoch/HWND, so it still passes) AND the caller's
    // selectors. Mirrors the off-lock-then-recheck discipline (placeholder create / paint
    // commit).
    {
        std::lock_guard<std::mutex> lk(backend_mutex_);
        resp.representation =
            sidecar::representation_name(registry_.representation_for_xid(req.xid));
        resp.generation = registry_.generation_for_xid(req.xid);
        resp.epoch = registry_.epoch_for_xid(req.xid);
        resp.last_paint_seq = registry_.last_paint_seq(req.xid);
        resp.shown = registry_.placeholder_shown(req.xid);
        const HWND now = hwnd_for_xid(req.xid);
        if (now == nullptr) {
            resp.status = "absent";
            resp.reason = "window torn down during capture";
            return resp; // px discarded
        }
        if (now != snap_hwnd || resp.epoch != snap_epoch || !selectors_match()) {
            resp.status = "mismatch";
            resp.reason = "lifecycle changed during capture";
            return resp; // px discarded
        }
    }
    // The lifecycle is still live + unchanged, so a failed copy means the layer is GENUINELY empty
    // (chrome unpainted / no cursor / wrong layer for the representation), not a raced teardown.
    if (!got) {
        resp.status = "empty";
        resp.reason = "layer has no content";
        return resp;
    }
    const std::uint64_t bytes = static_cast<std::uint64_t>(h) * static_cast<std::uint64_t>(stride);
    if (bytes > static_cast<std::uint64_t>(sidecar::kMaxCapturePayloadBytes)) {
        resp.status = "too_large";
        resp.reason = "source exceeds the frame cap";
        resp.width = static_cast<std::uint32_t>(w);
        resp.height = static_cast<std::uint32_t>(h);
        resp.stride = static_cast<std::uint32_t>(stride);
        resp.needed_bytes = bytes;
        return resp;
    }
    resp.ok = true;
    resp.status = "ok";
    resp.width = static_cast<std::uint32_t>(w);
    resp.height = static_cast<std::uint32_t>(h);
    resp.stride = static_cast<std::uint32_t>(stride);
    resp.xhot = xhot;
    resp.yhot = yhot;
    resp.format = sidecar::kCaptureFormatBgra8;
    resp.pixels.assign(reinterpret_cast<const char*>(px.data()), px.size());
    return resp;
}

vkrpc::CreateInstanceResponse
RealVulkanBackend::create_instance(const vkrpc::CreateInstanceRequest&) {
    vkrpc::CreateInstanceResponse resp;
    int inst_major = 1;
    int inst_minor = 0;
    query_instance_version(inst_major, inst_minor);

    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.apiVersion = instance_request_version(inst_major, inst_minor);
    // The session instance enables the WSI surface extensions the worker needs to back
    // an app window (the worker owns presentation). These are standard on any Windows
    // Vulkan loader; if a host lacks them, vkCreateInstance fails and we report it.
    std::vector<const char*> instance_exts = {VK_KHR_SURFACE_EXTENSION_NAME,
                                              VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
    // cross-adapter retirement: VK_EXT_swapchain_maintenance1 (device, enabled privately at
    // create_device when supported) requires the VK_EXT_surface_maintenance1 instance extension,
    // which requires VK_KHR_get_surface_capabilities2. Enable both ONLY when the loader offers
    // them -- purely additive (nothing guest-visible), and their absence just means the
    // present-fence retirement stays off.
    if (present_fence_retire_requested_) {
        std::uint32_t n = 0;
        vkEnumerateInstanceExtensionProperties(nullptr, &n, nullptr);
        std::vector<VkExtensionProperties> avail(n);
        if (n != 0) {
            vkEnumerateInstanceExtensionProperties(nullptr, &n, avail.data());
        }
        bool has_caps2 = false;
        bool has_surf_maint1 = false;
        for (const auto& e : avail) {
            if (std::string(e.extensionName) == VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME) {
                has_caps2 = true;
            }
            if (std::string(e.extensionName) == VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME) {
                has_surf_maint1 = true;
            }
        }
        if (has_caps2 && has_surf_maint1) {
            instance_exts.push_back(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
            instance_exts.push_back(VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME);
        }
    }
    const bool surface_maintenance1_enabled = instance_exts.size() > 2;
    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = static_cast<std::uint32_t>(instance_exts.size());
    ici.ppEnabledExtensionNames = instance_exts.data();

    VkInstance vk = VK_NULL_HANDLE;
    const VkResult ir = vkCreateInstance(&ici, nullptr, &vk);
    if (ir != VK_SUCCESS || vk == VK_NULL_HANDLE) {
        // Report the actual VkResult, and which WSI extensions the loader offered, instead of a
        // vague guess -- this is the signal that tells a real WSI-extension gap apart from a loader
        // or driver-discovery problem (e.g. a worker spawned in a context where the ICD is not
        // found).
        std::uint32_t ext_count = 0;
        vkEnumerateInstanceExtensionProperties(nullptr, &ext_count, nullptr);
        std::vector<VkExtensionProperties> exts(ext_count);
        if (ext_count > 0) {
            vkEnumerateInstanceExtensionProperties(nullptr, &ext_count, exts.data());
        }
        bool has_surface = false;
        bool has_win32 = false;
        for (const auto& e : exts) {
            if (std::string(e.extensionName) == VK_KHR_SURFACE_EXTENSION_NAME) {
                has_surface = true;
            }
            if (std::string(e.extensionName) == VK_KHR_WIN32_SURFACE_EXTENSION_NAME) {
                has_win32 = true;
            }
        }
        VKR_WARN(kComponent) << "create_instance: vkCreateInstance failed result="
                             << static_cast<int>(ir) << " (loader offers " << ext_count
                             << " instance exts; VK_KHR_surface=" << has_surface
                             << " VK_KHR_win32_surface=" << has_win32 << ")";
        resp.ok = false;
        // Carry the extension detail in the reason too: it crosses the wire to the ICD, so the
        // app/canary log sees WHY (a real WSI-extension gap vs a loader/driver-discovery problem)
        // even when the worker's own stderr is not captured by the launcher.
        resp.reason = "vkCreateInstance failed (result=" + std::to_string(static_cast<int>(ir)) +
                      "; loader exts=" + std::to_string(ext_count) +
                      " VK_KHR_surface=" + (has_surface ? "1" : "0") +
                      " VK_KHR_win32_surface=" + (has_win32 ? "1" : "0") + ")";
        return resp;
    }
    const std::uint64_t handle = next_handle_++;
    RealInstance ri;
    ri.vk = vk;
    ri.surface_maintenance1 = surface_maintenance1_enabled;
    instances_.emplace(handle, ri);
    resp.ok = true;
    resp.reason = "ok";
    resp.instance = handle;
    return resp;
}

// Vulkan 1.3 support and required-feature audit: can the HOST honestly
// back an apiVersion-1.3 relay device -- AND does the RELAY actually serve the complete cumulative
// required matrix? The Feature Requirements are CUMULATIVE: a 1.3 device must support every feature
// 1.1 and 1.2 made required too, not just the Vulkan13Features family. hostQueryReset (the
// DEVICE-level vkResetQueryPool, required since 1.2) is now wired and participates in
// the gate/vouch; multiview (required since 1.1) is the one required feature still unserved.
// Honest-by-construction: while ANY kRelayServes* flag is false the gate keeps
// vk13_ready FALSE, so the native lane falls back to 1.2 and we never report 1.3 while a required
// feature is advertise-then-failed. Once both reporting and execution are wired, the vouch checks
// the whole served required matrix,
// host-TRUE:
//   - f10: robustBufferAccess (required for 1.0 with no portability_subset -- we do not advertise
//     it);
//   - f11: multiview (served past the gate) + shaderDrawParameters (CONDITIONAL: required only when
//     we advertise VK_KHR_shader_draw_parameters, i.e. iff the host lists it, since advertised =
//     allowlist INTERSECT host);
//   - f12: the unconditional 1.2 required members (subgroupBroadcastDynamicId,
//   imagelessFramebuffer,
//     uniformBufferStandardLayout, shaderSubgroupExtendedTypes, separateDepthStencilLayouts,
//     timelineSemaphore, hostQueryReset once served) + samplerMirrorClampToEdge (CONDITIONAL, same
//     advertised-alias rule) + bufferDeviceAddress (1.3-required) + the two 1.3-required
//     memory-model bits;
//   - f13: the Vulkan 1.3 canary's served family (robustImageAccess ... synchronization2).
// The third memory-model member (vulkanMemoryModelAvailabilityVisibilityChains) is NOT required and
// stays optional/pass-through. Stamped into DeviceCaps.vk13_ready at enumerate; the ICD keys the
// reported apiVersion (and everything Vulkan 1.3 support unmasks) on it, and create_device
// RE-derives it before trusting a client's vk13_device_enabled.

bool host_vk13_ready(VkPhysicalDevice pd) {
    // The relay-served gate lives in ONE place --
    // vkrpc::kRelayServes* in vulkan_session.hpp -- shared with the mock so the two backends cannot
    // skew. While the gate is open (a required feature not yet served end-to-end) `ready` is false
    // and the native lane honestly reports 1.2. Accumulated (not an early return) so /WX does not
    // flag the tail as unreachable while the flags are compile-time false.
    bool ready = vkrpc::kRelayServesFullVk13RequiredMatrix;

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(pd, &props);
    ready = ready && props.apiVersion >= VK_API_VERSION_1_3;

    // The two conditional required rows (shaderDrawParameters, samplerMirrorClampToEdge) bind only
    // when the relay WILL advertise their KHR alias -- and the advertised set is the allowlist
    // INTERSECTED with the host list, so the condition reduces to "the host lists the extension"
    // a valid 1.3 host that does not expose the KHR alias is not over-strictly
    // downgraded.
    std::uint32_t ext_n = 0;
    vkEnumerateDeviceExtensionProperties(pd, nullptr, &ext_n, nullptr);
    std::vector<VkExtensionProperties> exts(ext_n);
    if (ext_n != 0) {
        vkEnumerateDeviceExtensionProperties(pd, nullptr, &ext_n, exts.data());
    }
    const auto host_has_ext = [&](const char* name) {
        for (const auto& e : exts) {
            if (std::strcmp(e.extensionName, name) == 0) {
                return true;
            }
        }
        return false;
    };

    VkPhysicalDeviceVulkan13Features f13{};
    f13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    VkPhysicalDeviceVulkan12Features f12{};
    f12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    f12.pNext = &f13;
    VkPhysicalDeviceVulkan11Features f11{};
    f11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    f11.pNext = &f12;
    VkPhysicalDeviceFeatures2 f2{};
    f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    f2.pNext = &f11;
    vkGetPhysicalDeviceFeatures2(pd, &f2);
    const VkBool32 required[] = {
        // f10 (core) -- required with no portability_subset.
        f2.features.robustBufferAccess,
        // f11 -- multiview (reached only past the relay-served gate = served).
        f11.multiview,
        // f12 -- the unconditional 1.2 required members + hostQueryReset (served past the gate) +
        // bufferDeviceAddress (1.3-required) + the two 1.3-required memory-model bits.
        f12.subgroupBroadcastDynamicId,
        f12.imagelessFramebuffer,
        f12.uniformBufferStandardLayout,
        f12.shaderSubgroupExtendedTypes,
        f12.separateDepthStencilLayouts,
        f12.timelineSemaphore,
        f12.hostQueryReset,
        f12.bufferDeviceAddress,
        f12.vulkanMemoryModel,
        f12.vulkanMemoryModelDeviceScope,
        // f13 -- the Vulkan 1.3 canary's served family.
        f13.robustImageAccess,
        f13.inlineUniformBlock,
        f13.pipelineCreationCacheControl,
        f13.privateData,
        f13.shaderDemoteToHelperInvocation,
        f13.shaderTerminateInvocation,
        f13.subgroupSizeControl,
        f13.computeFullSubgroups,
        f13.shaderZeroInitializeWorkgroupMemory,
        f13.shaderIntegerDotProduct,
        f13.maintenance4,
        f13.dynamicRendering,
        f13.synchronization2,
    };
    for (const VkBool32 v : required) {
        ready = ready && v != VK_FALSE;
    }
    // The conditional required rows -- binding only when we will advertise the KHR alias.
    ready = ready && !(host_has_ext(VK_KHR_SHADER_DRAW_PARAMETERS_EXTENSION_NAME) &&
                       f11.shaderDrawParameters == VK_FALSE);
    ready = ready && !(host_has_ext(VK_KHR_SAMPLER_MIRROR_CLAMP_TO_EDGE_EXTENSION_NAME) &&
                       f12.samplerMirrorClampToEdge == VK_FALSE);
    return ready;
}

vkrpc::EnumeratePhysicalDevicesResponse
RealVulkanBackend::enumerate_physical_devices(const vkrpc::EnumeratePhysicalDevicesRequest& req) {
    vkrpc::EnumeratePhysicalDevicesResponse resp;
    const auto it = instances_.find(req.instance);
    if (it == instances_.end()) {
        resp.ok = false;
        resp.reason = "unknown instance handle";
        return resp;
    }
    // Mint the physical-device handle on first enumeration and cache it, so the
    // handle is stable across calls and create_device can validate against it (the
    // worker serves one assigned GPU).
    if (it->second.physical == 0) {
        VkPhysicalDeviceProperties props{};
        VkPhysicalDevice pd =
            select_physical_device(it->second.vk, gpu_required_, gpu_luid_, gpu_name_, props);
        if (pd == VK_NULL_HANDLE) {
            resp.ok = false;
            resp.reason = "no Vulkan physical device";
            return resp;
        }
        it->second.physical = next_handle_++;
        it->second.physical_vk = pd;
    }
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(it->second.physical_vk, &props);
    vkrpc::PhysicalDeviceEntry entry;
    entry.handle = it->second.physical;
    entry.caps = caps_from_props(props);
    entry.caps.timestamp_valid_bits =
        graphics_family_timestamp_valid_bits(it->second.physical_vk); // GL 3.3 (query pools)
    entry.caps.queue_flags = graphics_family_queue_flags(it->second.physical_vk); // compute honesty
    entry.caps.vk13_ready = host_vk13_ready(it->second.physical_vk) ? 1 : 0; // Vulkan 1.3 support
    // carry the HOST's real device-extension names so the ICD advertises its
    // allowlist INTERSECTED with them -- the guest never observes an extension this host cannot
    // enable.
    {
        std::uint32_t n = 0;
        vkEnumerateDeviceExtensionProperties(it->second.physical_vk, nullptr, &n, nullptr);
        std::vector<VkExtensionProperties> avail(n);
        if (n != 0) {
            vkEnumerateDeviceExtensionProperties(it->second.physical_vk, nullptr, &n, avail.data());
        }
        for (const auto& e : avail) {
            entry.device_extensions.emplace_back(e.extensionName);
        }
    }
    resp.devices.push_back(entry);
    resp.ok = true;
    resp.reason = "ok";
    return resp;
}

vkrpc::CreateDeviceResponse
RealVulkanBackend::create_device(const vkrpc::CreateDeviceRequest& req) {
    vkrpc::CreateDeviceResponse resp;
    const auto it = instances_.find(req.instance);
    if (it == instances_.end()) {
        resp.ok = false;
        resp.reason = "unknown instance handle";
        return resp;
    }
    // Physical-device selection enforced: only the device this instance enumerated.
    if (req.physical_device == 0 || req.physical_device != it->second.physical) {
        resp.ok = false;
        resp.reason = "physical device not enumerated from this instance "
                      "(call enumerate_physical_devices before create_device)";
        return resp;
    }
    const int family = graphics_queue_family(it->second.physical_vk);
    if (family < 0) {
        resp.ok = false;
        resp.reason = "no graphics queue family on the selected device";
        return resp;
    }

    // device-loss probe: cache the RENDER adapter identity (deviceName + LUID) so the
    // present/submit device-loss diagnostics can compare it against the adapter driving the
    // window's monitor (the cross-GPU-present check). Resolve props2 via the instance (core 1.1;
    // a 1.0 instance simply leaves the identity empty -> the diagnostic still logs names, just no
    // LUID).
    if (render_adapter_name_.empty()) {
        auto props2fn = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
            vkGetInstanceProcAddr(it->second.vk, "vkGetPhysicalDeviceProperties2"));
        if (props2fn != nullptr) {
            VkPhysicalDeviceIDProperties id{};
            id.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
            VkPhysicalDeviceProperties2 p2{};
            p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
            p2.pNext = &id;
            props2fn(it->second.physical_vk, &p2);
            render_adapter_name_ = p2.properties.deviceName;
            if (id.deviceLUIDValid != VK_FALSE) {
                render_adapter_luid_ = format_luid(id.deviceLUID, VK_LUID_SIZE);
            }
        } else {
            VkPhysicalDeviceProperties p{};
            vkGetPhysicalDeviceProperties(it->second.physical_vk, &p);
            render_adapter_name_ = p.deviceName;
        }
        VKR_INFO(kComponent) << "device-loss probe: render adapter '" << render_adapter_name_
                             << "' luid="
                             << (render_adapter_luid_.empty() ? "?" : render_adapter_luid_);
    }

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo qci{};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = static_cast<std::uint32_t>(family);
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;

    // (GL/zink): enable the app's requested device extensions (e.g. zink's maintenance1
    // etc.) alongside VK_KHR_swapchain (always, for the worker's presentation spine), CHECKED
    // against the host's real support: a host-missing requested extension FAILS
    // create_device here, loudly, BEFORE host vkCreateDevice -- never a silently-degraded device.
    // The ICD already filtered against its host-intersected allowlist, so a conformant app cannot
    // reach this; it is the worker-side safety net. A legacy request (empty list) reduces to
    // swapchain-only -- the vkcube/canary behavior is unchanged.
    std::set<std::string> host_exts;
    {
        std::uint32_t n = 0;
        vkEnumerateDeviceExtensionProperties(it->second.physical_vk, nullptr, &n, nullptr);
        std::vector<VkExtensionProperties> avail(n);
        if (n != 0) {
            vkEnumerateDeviceExtensionProperties(it->second.physical_vk, nullptr, &n, avail.data());
        }
        for (const auto& e : avail) {
            host_exts.insert(e.extensionName);
        }
    }
    std::vector<std::string> ext_storage;
    ext_storage.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    for (const auto& want : req.enabled_extensions) {
        if (want == VK_KHR_SWAPCHAIN_EXTENSION_NAME) {
            continue; // already added
        }
        if (host_exts.count(want) != 0) {
            ext_storage.push_back(want);
        } else {
            // FAIL the create instead of silently skipping -- a device that
            // "succeeds" without an extension the app enabled produces an invalid Vulkan contract
            // (extension commands would later be rejected mid-run). With the ICD advertising the
            // host-intersected list, a conformant app can no longer reach this; it stays as the
            // loud worker-side safety net.
            resp.ok = false;
            resp.reason = "host lacks requested device extension '" + want + "'";
            return resp;
        }
    }
    // cross-adapter retirement: privately enable
    // VK_EXT_swapchain_maintenance1 when the whole dependency chain holds -- the instance enabled
    // VK_EXT_surface_maintenance1, the host device lists the extension, and the host feature
    // queries TRUE. Worker-internal only (never advertised to or requested by the guest): presents
    // then attach a per-swapchain fence and a recreate waits the retiring swapchain's fences --
    // the spec's "safe to retire" signal, targeting the live-reproduced hybrid-GPU recreate
    // DEVICE_LOST. Diagnostic only: exact worker-side VKRELAY2_PRESENT_FENCE_RETIRE=1 opts in;
    // every other value is cold from instance-extension enumeration onward.
    bool present_fence_retire = false;
    if (present_fence_retire_requested_ && it->second.surface_maintenance1 &&
        host_exts.count(VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME) != 0) {
        VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT sm1q{};
        sm1q.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT;
        VkPhysicalDeviceFeatures2 f2q{};
        f2q.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        f2q.pNext = &sm1q;
        vkGetPhysicalDeviceFeatures2(it->second.physical_vk, &f2q);
        if (sm1q.swapchainMaintenance1 != VK_FALSE) {
            ext_storage.push_back(VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME);
            present_fence_retire = true;
        }
    }
    VKR_INFO(kComponent) << "present-fence retire (swapchain_maintenance1): "
                         << (present_fence_retire ? "ACTIVE" : "inactive");
    std::vector<const char*> ext_ptrs;
    ext_ptrs.reserve(ext_storage.size());
    for (const auto& e : ext_storage) {
        ext_ptrs.push_back(e.c_str());
    }

    // Base features are normalized against both the host and the spelling that will actually reach
    // vkCreateDevice. A Features2 chain owns the base members; otherwise pEnabledFeatures owns
    // them. Never silently clamp an over-request: a hostile/skewed RPC must fail closed instead of
    // creating a feature-OFF device while later command gates believe the feature is ON.
    VkPhysicalDeviceFeatures host_feats{};
    vkGetPhysicalDeviceFeatures(it->second.physical_vk, &host_feats);
    const std::uint64_t host_bits = vkrpc::pack_physical_device_features(host_feats);
    std::uint64_t enabled_base_bits = 0;
    VkPhysicalDeviceFeatures enabled_feats{};

    // (GL/zink): rebuild the app's enabled-feature pNext chain (VkPhysicalDeviceFeatures2 +
    // Vulkan11/12/13 + ext feature structs) from the forwarded blobs and hang it off
    // VkDeviceCreateInfo.pNext, so the real device enables exactly what the app requested. A
    // Features2 entry owns the base members and forbids pEnabledFeatures; an extension-only chain
    // legally coexists with pEnabledFeatures.
    constexpr std::uint32_t kHeaderBytes = static_cast<std::uint32_t>(sizeof(VkBaseOutStructure));
    constexpr std::uint32_t kMaxFeatureStructBytes = 4096;
    std::vector<std::vector<unsigned char>> chain_bufs;
    bool chain_has_features2 = false;
    std::uint64_t chain_base_bits = 0;
    for (const vkrpc::CapabilityChainEntry& e : req.enabled_feature_chain) {
        if (e.size < kHeaderBytes || e.size > kMaxFeatureStructBytes || e.blob.size() < e.size) {
            resp.ok = false;
            resp.reason = "enabled-feature chain entry malformed";
            return resp;
        }
        if (e.s_type == static_cast<std::uint32_t>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2)) {
            if (e.size != sizeof(VkPhysicalDeviceFeatures2) || chain_has_features2) {
                resp.ok = false;
                resp.reason = chain_has_features2
                                  ? "enabled-feature chain contains duplicate Features2"
                                  : "Features2 feature struct has a wrong size";
                return resp;
            }
            VkPhysicalDeviceFeatures2 features2{};
            std::memcpy(&features2, e.blob.data(), sizeof(features2));
            chain_base_bits = vkrpc::pack_physical_device_features(features2.features);
            chain_has_features2 = true;
        }
        std::vector<unsigned char> buf(e.blob.begin(), e.blob.begin() + e.size);
        chain_bufs.push_back(std::move(buf));
    }
    if (chain_has_features2) {
        if ((chain_base_bits & ~host_bits) != 0) {
            resp.ok = false;
            resp.reason = "Features2 enables a base feature the host does not support";
            return resp;
        }
        // New ICDs declare the scalar mirror authoritative, so require exact agreement in both
        // directions. Older ICDs left it zero when Features2 owned the base members; for those
        // requests allow chain-only enables, but still reject the dangerous inverse where the
        // scalar claims a bit that the actual host chain leaves disabled.
        const bool scalar_mismatch = req.enabled_feature_bits_authoritative
                                         ? req.enabled_feature_bits != chain_base_bits
                                         : (req.enabled_feature_bits & ~chain_base_bits) != 0;
        if (scalar_mismatch) {
            resp.ok = false;
            resp.reason = req.enabled_feature_bits_authoritative
                              ? "base-feature scalar disagrees with the Features2 chain"
                              : "base-feature scalar claims bits disabled in the Features2 chain";
            return resp;
        }
        enabled_base_bits = chain_base_bits;
    } else {
        if ((req.enabled_feature_bits & ~host_bits) != 0) {
            resp.ok = false;
            resp.reason = "requested base feature is not supported by the host";
            return resp;
        }
        enabled_base_bits = req.enabled_feature_bits;
        enabled_feats = vkrpc::unpack_physical_device_features(enabled_base_bits);
    }
    // (bufferDeviceAddress): NORMALIZE the BDA feature state against the
    // forwarded chain BEFORE the host device becomes live -- the scalar request bit alone must
    // never gate a served feature the host device did not actually enable. Derive what the chain
    // really enables (the standalone struct or the 1.2 rollup) and reject a scalar/chain mismatch
    // in EITHER direction, plus any captureReplay/multiDevice request (unwired, reported FALSE).
    // A conformant ICD can no longer send any of these (it rejects them with FEATURE_NOT_PRESENT);
    // this is the worker-side safety net that makes the hostile/stale-RPC answer the same
    // fail-closed one.
    // geometry-stream: the NORMALIZED enabled state (explicit scalar when sent, chain-derived when
    // the scalar is omitted by an older ICD) -- computed inside the normalization block below,
    // consumed at the RealDevice fill.
    bool geometry_streams_enabled = false;
    bool draw_indirect_count_enabled = false;
    {
        bool chain_bda = false;
        bool chain_bda_unwired_bits = false;
        bool chain_host_query_reset = false; // hardening
        bool chain_multiview = false;        // (the Vulkan11Features rollup spelling)
        bool chain_divisor = false;      // vertex-attr-divisor: vertexAttributeInstanceRateDivisor
        bool chain_zero_divisor = false; // ... vertexAttributeInstanceRateZeroDivisor
        bool chain_geometry_streams = false; // geometry-stream: geometryStreams (TF features)
        bool chain_draw_indirect_count = false;
        for (const vkrpc::CapabilityChainEntry& e : req.enabled_feature_chain) {
            // Exact-size gate for the KNOWN structs this normalization interprets:
            // an undersized known blob would otherwise reach the host vkCreateDevice as a
            // garbage-tailed struct. The ICD always sends size == sizeof (its sType->size table),
            // so any other size is a malformed/hostile frame. Deliberately NARROW -- unknown
            // sTypes stay pass-through (a newer ICD's new struct must not be rejected by an older
            // worker); the generic known-struct sweep is a recorded hardening follow-up.
            if (e.s_type == static_cast<std::uint32_t>(
                                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES)) {
                if (e.size != sizeof(VkPhysicalDeviceBufferDeviceAddressFeatures)) {
                    resp.ok = false;
                    resp.reason = "bufferDeviceAddress feature struct has a wrong size";
                    return resp;
                }
                VkPhysicalDeviceBufferDeviceAddressFeatures f{};
                std::memcpy(&f, e.blob.data(), sizeof(f));
                chain_bda = chain_bda || f.bufferDeviceAddress != VK_FALSE;
                chain_bda_unwired_bits = chain_bda_unwired_bits ||
                                         f.bufferDeviceAddressCaptureReplay != VK_FALSE ||
                                         f.bufferDeviceAddressMultiDevice != VK_FALSE;
            }
            if (e.s_type ==
                static_cast<std::uint32_t>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES)) {
                if (e.size != sizeof(VkPhysicalDeviceVulkan12Features)) {
                    resp.ok = false;
                    resp.reason = "Vulkan12Features feature struct has a wrong size";
                    return resp;
                }
                VkPhysicalDeviceVulkan12Features f{};
                std::memcpy(&f, e.blob.data(), sizeof(f));
                chain_bda = chain_bda || f.bufferDeviceAddress != VK_FALSE;
                chain_bda_unwired_bits = chain_bda_unwired_bits ||
                                         f.bufferDeviceAddressCaptureReplay != VK_FALSE ||
                                         f.bufferDeviceAddressMultiDevice != VK_FALSE;
                chain_host_query_reset = chain_host_query_reset || f.hostQueryReset != VK_FALSE;
                chain_draw_indirect_count =
                    chain_draw_indirect_count || f.drawIndirectCount != VK_FALSE;
            }
            // Required-feature audit (hardening): the standalone
            // hostQueryReset feature spelling, exact-size gated like the structs above.
            if (e.s_type == static_cast<std::uint32_t>(
                                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES)) {
                if (e.size != sizeof(VkPhysicalDeviceHostQueryResetFeatures)) {
                    resp.ok = false;
                    resp.reason = "hostQueryReset feature struct has a wrong size";
                    return resp;
                }
                VkPhysicalDeviceHostQueryResetFeatures f{};
                std::memcpy(&f, e.blob.data(), sizeof(f));
                chain_host_query_reset = chain_host_query_reset || f.hostQueryReset != VK_FALSE;
            }
            // Required-feature audit: the multiview feature rides EITHER the
            // Vulkan11Features ROLLUP or the standalone
            // VkPhysicalDeviceMultiviewFeatures (forwards it) -- the two spellings the ICD
            // reports host-TRUE, forwards, and detects. Exact-size gated like the structs above;
            // both fold into chain_multiview so the scalar agrees regardless of which spelling the
            // app used.
            if (e.s_type ==
                static_cast<std::uint32_t>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES)) {
                if (e.size != sizeof(VkPhysicalDeviceVulkan11Features)) {
                    resp.ok = false;
                    resp.reason = "Vulkan11Features feature struct has a wrong size";
                    return resp;
                }
                VkPhysicalDeviceVulkan11Features f{};
                std::memcpy(&f, e.blob.data(), sizeof(f));
                chain_multiview = chain_multiview || f.multiview != VK_FALSE;
            }
            if (e.s_type ==
                static_cast<std::uint32_t>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES)) {
                if (e.size != sizeof(VkPhysicalDeviceMultiviewFeatures)) {
                    resp.ok = false;
                    resp.reason = "multiview feature struct has a wrong size";
                    return resp;
                }
                VkPhysicalDeviceMultiviewFeatures f{};
                std::memcpy(&f, e.blob.data(), sizeof(f));
                chain_multiview = chain_multiview || f.multiview != VK_FALSE;
            }
            // vertex-attr-divisor: the two feature bits ride the standalone
            // VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT (ext-only, no rollup). Exact-size
            // gated like the structs above; both fold into the chain bools for the agreement
            // checks.
            if (e.s_type ==
                static_cast<std::uint32_t>(
                    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT)) {
                if (e.size != sizeof(VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT)) {
                    resp.ok = false;
                    resp.reason = "vertexAttributeDivisor feature struct has a wrong size";
                    return resp;
                }
                VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT f{};
                std::memcpy(&f, e.blob.data(), sizeof(f));
                chain_divisor = chain_divisor || f.vertexAttributeInstanceRateDivisor != VK_FALSE;
                chain_zero_divisor =
                    chain_zero_divisor || f.vertexAttributeInstanceRateZeroDivisor != VK_FALSE;
            }
            // geometry-stream: the geometryStreams feature rides the standalone
            // VkPhysicalDeviceTransformFeedbackFeaturesEXT (ext-only, no rollup). Same exact-size
            // gate + chain-bool fold for the scalar agreement check.
            if (e.s_type ==
                static_cast<std::uint32_t>(
                    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT)) {
                if (e.size != sizeof(VkPhysicalDeviceTransformFeedbackFeaturesEXT)) {
                    resp.ok = false;
                    resp.reason = "transformFeedback feature struct has a wrong size";
                    return resp;
                }
                VkPhysicalDeviceTransformFeedbackFeaturesEXT f{};
                std::memcpy(&f, e.blob.data(), sizeof(f));
                chain_geometry_streams = chain_geometry_streams || f.geometryStreams != VK_FALSE;
            }
        }
        if (chain_bda_unwired_bits) {
            resp.ok = false;
            resp.reason = "bufferDeviceAddressCaptureReplay/MultiDevice requested (unwired, "
                          "reported FALSE)";
            return resp;
        }
        if ((req.buffer_device_address_feature_enabled != 0) != chain_bda) {
            resp.ok = false;
            resp.reason = "bufferDeviceAddress scalar/feature-chain mismatch (the request bit and "
                          "the enabled-feature chain must agree)";
            return resp;
        }
        // Required-feature audit (hardening): the scalar
        // host_query_reset_feature_enabled must agree with what the forwarded chain actually
        // enables (either spelling), in EITHER direction -- so a skewed client cannot claim the
        // feature the host device did not enable (which would make the host vkResetQueryPool
        // invalid), nor drive a reset while the scalar hid an enable the host DID get.
        if ((req.host_query_reset_feature_enabled != 0) != chain_host_query_reset) {
            resp.ok = false;
            resp.reason = "hostQueryReset scalar/feature-chain mismatch (the request bit and the "
                          "enabled-feature chain must agree)";
            return resp;
        }
        // Required-feature audit: the scalar multiview_feature_enabled must agree with
        // what the forwarded chain (the Vulkan11Features rollup) actually enables, in EITHER
        // direction -- so a skewed client cannot claim multiview the host device did not enable
        // (which would make a host multiview render pass invalid), nor drive a viewMask pass while
        // the scalar hid an enable the host DID get.
        if ((req.multiview_feature_enabled != 0) != chain_multiview) {
            resp.ok = false;
            resp.reason = "multiview scalar/feature-chain mismatch (the request bit and the "
                          "enabled-feature chain must agree)";
            return resp;
        }
        // vertex-attr-divisor: both scalars must agree with the forwarded chain (either direction),
        // so a skewed client cannot claim a divisor feature the host device did not enable (which
        // would let a divisor != 1 / == 0 pipeline reach the host invalidly), nor drive a divisor
        // pipeline while the scalar hid an enable the host DID get.
        if ((req.vertex_attr_divisor_feature_enabled != 0) != chain_divisor) {
            resp.ok = false;
            resp.reason = "vertexAttributeInstanceRateDivisor scalar/feature-chain mismatch (the "
                          "request bit and the enabled-feature chain must agree)";
            return resp;
        }
        if ((req.vertex_attr_zero_divisor_feature_enabled != 0) != chain_zero_divisor) {
            resp.ok = false;
            resp.reason =
                "vertexAttributeInstanceRateZeroDivisor scalar/feature-chain mismatch (the "
                "request bit and the enabled-feature chain must agree)";
            return resp;
        }
        // geometry-stream: the scalar must agree with the forwarded chain (either direction), so a
        // skewed client cannot claim geometryStreams the host device did not enable (which would
        // let a stream pipeline reach the host invalidly), nor drive one while the scalar hid an
        // enable the host DID get. The scalar is THREE-STATE -- an older ICD
        // already forwards a geometryStreams=TRUE chain (zink enables it routinely; masking the
        // feature breaks Mesa 23.2 shaders) but has no scalar, decoding as the -1 OMITTED
        // sentinel. Agreement is enforced only for an explicit 0/1; when omitted, the chain alone
        // is the truth (exactly what an older worker did), so old-ICD/new-worker pairs keep
        // creating devices instead of regressing to a mismatch rejection. The feature also
        // structurally REQUIRES the extension: a chain that enables geometryStreams without
        // VK_EXT_transform_feedback in the enabled-extension list is self-contradictory (the host
        // vkCreateDevice would reject it anyway; fail closed here with the named reason instead).
        // omission is decided by wire-key ABSENCE at from_body (only the trusted
        // direct-backend test fixture may model it by passing the sentinel value); any other
        // out-of-domain value -- including the INVALID (-2) that a transmitted -1/2/wrong-type
        // decodes to -- rejects by name BEFORE the agreement/derive logic, so a hostile client
        // cannot claim legacy status to dodge the scalar/chain agreement check.
        if (!vkrpc::decode_three_state_scalar(
                req.geometry_streams_feature_enabled, chain_geometry_streams,
                "geometry_streams_feature_enabled", geometry_streams_enabled, resp.reason)) {
            resp.ok = false;
            return resp;
        }
        if (req.geometry_streams_feature_enabled >= 0 &&
            (req.geometry_streams_feature_enabled != 0) != chain_geometry_streams) {
            resp.ok = false;
            resp.reason = "geometryStreams scalar/feature-chain mismatch (the request bit and the "
                          "enabled-feature chain must agree)";
            return resp;
        }
        if (chain_geometry_streams &&
            std::find(ext_storage.begin(), ext_storage.end(),
                      VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME) == ext_storage.end()) {
            resp.ok = false;
            resp.reason = "geometryStreams enabled without VK_EXT_transform_feedback";
            return resp;
        }
        // Indirect-count has two legal enable paths: the featureless KHR extension or the promoted
        // Vulkan-1.2 feature. The additive scalar mirrors their OR. An old ICD omits the key, so
        // derive from the actual extension/chain instead of rejecting an otherwise valid request.
        const bool extension_draw_indirect_count =
            std::find(ext_storage.begin(), ext_storage.end(),
                      VK_KHR_DRAW_INDIRECT_COUNT_EXTENSION_NAME) != ext_storage.end();
        if (chain_draw_indirect_count) {
            // Hostile/stale RPC safety net: the forwarded f12 chain must not enable a feature the
            // selected physical device reports FALSE. Check before vkCreateDevice so the backend
            // returns a named feature rejection instead of collapsing the host's result into the
            // generic "vkCreateDevice failed" response.
            VkPhysicalDeviceVulkan12Features host_f12{};
            host_f12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
            VkPhysicalDeviceFeatures2 host_f2{};
            host_f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
            host_f2.pNext = &host_f12;
            vkGetPhysicalDeviceFeatures2(it->second.physical_vk, &host_f2);
            if (host_f12.drawIndirectCount == VK_FALSE) {
                resp.ok = false;
                resp.reason = "drawIndirectCount feature requested but the host reports it FALSE";
                return resp;
            }
        }
        const bool derived_draw_indirect_count =
            extension_draw_indirect_count || chain_draw_indirect_count;
        if (!vkrpc::decode_three_state_scalar(
                req.draw_indirect_count_enabled, derived_draw_indirect_count,
                "draw_indirect_count_enabled", draw_indirect_count_enabled, resp.reason)) {
            resp.ok = false;
            return resp;
        }
        if (req.draw_indirect_count_enabled >= 0 &&
            (req.draw_indirect_count_enabled != 0) != derived_draw_indirect_count) {
            resp.ok = false;
            resp.reason = "drawIndirectCount scalar/extension-feature-chain mismatch (the request "
                          "bit and enabled surface must agree)";
            return resp;
        }
    }
    // Descriptor indexing: the same normalization for the DI feature bits --
    // derive what the chain enables (the standalone DescriptorIndexingFeatures struct, exact-size
    // gated like the two structs above, or the 1.2 rollup) and reject a scalar/chain mismatch in
    // either direction BEFORE the host device exists. folds the two BUFFER shader
    // non-uniform-indexing members into their wire bits (they must normalize + host-verify like
    // every carried feature); the aggregate `descriptorIndexing` bit and the
    // remaining shader *ArrayIndexing bits stay unserved -- a chain that enables any of them
    // rejects by name.
    {
        // Defense in depth: the worker's CreateDevice POLICY clamps to the SERVED
        // subset, not merely the known universe -- a skewed/custom client sending a
        // deferred-but-known bit (an image/texel UAB class the relay never proved) dies here even
        // when its chain agrees and the host supports the class. Widening kDIFeatureServedBits at
        // the ICD AND both backends together is the intentional act when a class's proof lands
        // (the BDA/sync2/DR discipline). A chain-only deferred enable dies just below as a
        // scalar/chain mismatch.
        if ((req.descriptor_indexing_feature_bits & ~vkrpc::kDIFeatureServedBits) != 0) {
            resp.ok = false;
            resp.reason = "descriptor_indexing_feature_bits outside the served set";
            return resp;
        }
        std::uint64_t chain_di = 0;
        bool chain_di_unserved = false;
        const auto fold_di = [&](const VkBool32* f20) {
            // The 20 members of VkPhysicalDeviceDescriptorIndexingFeatures, in declaration order
            // (the 1.2 rollup repeats them verbatim after its aggregate bit): indices 0..9 = the
            // 3 dynamic-indexing + 7 non-uniform-indexing shader bits (of which ONLY 3 = uniform-
            // buffer and 5 = storage-buffer non-uniform indexing are served, as wire bits),
            // 10..18 = the 6 UpdateAfterBind + UpdateUnusedWhilePending + PartiallyBound +
            // VariableDescriptorCount gated bits, 19 = runtimeDescriptorArray (folded by name at
            // the call sites).
            const std::uint64_t shader_bits[10] = {
                0, // shaderInputAttachmentArrayDynamicIndexing (unserved)
                0, // shaderUniformTexelBufferArrayDynamicIndexing (unserved)
                0, // shaderStorageTexelBufferArrayDynamicIndexing (unserved)
                vkrpc::kDIFeatureShaderUniformBufferArrayNonUniformIndexing,
                0, // shaderSampledImageArrayNonUniformIndexing (unserved)
                vkrpc::kDIFeatureShaderStorageBufferArrayNonUniformIndexing,
                0, // shaderStorageImageArrayNonUniformIndexing (unserved)
                0, // shaderInputAttachmentArrayNonUniformIndexing (unserved)
                0, // shaderUniformTexelBufferArrayNonUniformIndexing (unserved)
                0, // shaderStorageTexelBufferArrayNonUniformIndexing (unserved)
            };
            for (std::size_t i = 0; i < 10; ++i) {
                if (f20[i] == VK_FALSE) {
                    continue;
                }
                if (shader_bits[i] != 0) {
                    chain_di |= shader_bits[i];
                } else {
                    chain_di_unserved = true;
                }
            }
            const std::uint64_t gated[9] = {
                vkrpc::kDIFeatureUpdateAfterBindUniformBuffer,
                vkrpc::kDIFeatureUpdateAfterBindSampledImage,
                vkrpc::kDIFeatureUpdateAfterBindStorageImage,
                vkrpc::kDIFeatureUpdateAfterBindStorageBuffer,
                vkrpc::kDIFeatureUpdateAfterBindUniformTexelBuffer,
                vkrpc::kDIFeatureUpdateAfterBindStorageTexelBuffer,
                vkrpc::kDIFeatureUpdateUnusedWhilePending,
                vkrpc::kDIFeaturePartiallyBound,
                vkrpc::kDIFeatureVariableDescriptorCount,
            };
            for (std::size_t i = 0; i < 9; ++i) {
                if (f20[10 + i] != VK_FALSE) {
                    chain_di |= gated[i];
                }
            }
        };
        for (const vkrpc::CapabilityChainEntry& e : req.enabled_feature_chain) {
            if (e.s_type == static_cast<std::uint32_t>(
                                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES)) {
                if (e.size != sizeof(VkPhysicalDeviceDescriptorIndexingFeatures)) {
                    resp.ok = false;
                    resp.reason = "descriptorIndexing feature struct has a wrong size";
                    return resp;
                }
                VkPhysicalDeviceDescriptorIndexingFeatures f{};
                std::memcpy(&f, e.blob.data(), sizeof(f));
                fold_di(&f.shaderInputAttachmentArrayDynamicIndexing);
                if (f.runtimeDescriptorArray != VK_FALSE) {
                    chain_di |= vkrpc::kDIFeatureRuntimeDescriptorArray;
                }
            }
            if (e.s_type ==
                static_cast<std::uint32_t>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES)) {
                // Exact size already enforced by the BDA normalization above.
                VkPhysicalDeviceVulkan12Features f{};
                std::memcpy(&f, e.blob.data(), sizeof(f));
                chain_di_unserved = chain_di_unserved || f.descriptorIndexing != VK_FALSE;
                fold_di(&f.shaderInputAttachmentArrayDynamicIndexing);
                if (f.runtimeDescriptorArray != VK_FALSE) {
                    chain_di |= vkrpc::kDIFeatureRuntimeDescriptorArray;
                }
            }
        }
        if (chain_di_unserved) {
            resp.ok = false;
            resp.reason = "descriptorIndexing aggregate / unserved shader-indexing bits requested";
            return resp;
        }
        if (req.descriptor_indexing_feature_bits != chain_di) {
            resp.ok = false;
            resp.reason = "descriptorIndexing scalar/feature-chain mismatch (the request bits and "
                          "the enabled-feature chain must agree)";
            return resp;
        }
    }
    // Vulkan 1.3 support: the same normalization for the vk13 scalar + the vk13_device flag. The
    // CreateDevice POLICY clamps to the SERVED set; the flag is RE-derived
    // from the host (a lying client cannot uncap); the chain-folded bits must equal the scalar
    // in both directions. The f13 rollup's dynamicRendering/synchronization2 members ride their
    // long-standing dedicated ints + verbatim blob, not this scalar.
    {
        if ((req.vk13_feature_bits & ~vkrpc::kVk13FeatureServedBits) != 0) {
            resp.ok = false;
            resp.reason = "vk13_feature_bits outside the served set";
            return resp;
        }
        if (req.vk13_device_enabled != 0 && !host_vk13_ready(it->second.physical_vk)) {
            resp.ok = false;
            resp.reason = "vk13 device requested but the host cannot back an apiVersion-1.3 "
                          "device (host below 1.3 or a required feature missing)";
            return resp;
        }
        constexpr std::uint64_t kMemoryModelBits =
            vkrpc::kVk13FeatureVulkanMemoryModel | vkrpc::kVk13FeatureVulkanMemoryModelDeviceScope |
            vkrpc::kVk13FeatureVulkanMemoryModelAvailabilityVisibilityChains;
        if ((req.vk13_feature_bits & ~kMemoryModelBits) != 0 && req.vk13_device_enabled == 0) {
            resp.ok = false;
            resp.reason = "vk13 features enabled without the vk13 device (only the memory-model "
                          "bits are reported off it)";
            return resp;
        }
        std::uint64_t chain_vk13 = 0;
        bool chain_vk13_unserved = false;
        const auto fold13 = [&](VkBool32 v, std::uint64_t bit) {
            if (v != VK_FALSE) {
                chain_vk13 |= bit;
            }
        };
        for (const vkrpc::CapabilityChainEntry& e : req.enabled_feature_chain) {
            const auto expect = [&](std::size_t want) {
                return e.size == want && e.blob.size() >= want;
            };
            switch (static_cast<int>(e.s_type)) {
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES: {
                if (!expect(sizeof(VkPhysicalDeviceVulkan13Features))) {
                    resp.ok = false;
                    resp.reason = "Vulkan13Features struct has a wrong size";
                    return resp;
                }
                VkPhysicalDeviceVulkan13Features f{};
                std::memcpy(&f, e.blob.data(), sizeof(f));
                fold13(f.robustImageAccess, vkrpc::kVk13FeatureRobustImageAccess);
                fold13(f.inlineUniformBlock, vkrpc::kVk13FeatureInlineUniformBlock);
                chain_vk13_unserved =
                    chain_vk13_unserved ||
                    f.descriptorBindingInlineUniformBlockUpdateAfterBind != VK_FALSE ||
                    f.textureCompressionASTC_HDR != VK_FALSE;
                fold13(f.pipelineCreationCacheControl,
                       vkrpc::kVk13FeaturePipelineCreationCacheControl);
                fold13(f.privateData, vkrpc::kVk13FeaturePrivateData);
                fold13(f.shaderDemoteToHelperInvocation,
                       vkrpc::kVk13FeatureShaderDemoteToHelperInvocation);
                fold13(f.shaderTerminateInvocation, vkrpc::kVk13FeatureShaderTerminateInvocation);
                fold13(f.subgroupSizeControl, vkrpc::kVk13FeatureSubgroupSizeControl);
                fold13(f.computeFullSubgroups, vkrpc::kVk13FeatureComputeFullSubgroups);
                fold13(f.shaderZeroInitializeWorkgroupMemory,
                       vkrpc::kVk13FeatureShaderZeroInitializeWorkgroupMemory);
                fold13(f.shaderIntegerDotProduct, vkrpc::kVk13FeatureShaderIntegerDotProduct);
                fold13(f.maintenance4, vkrpc::kVk13FeatureMaintenance4);
                break;
            }
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES: {
                // Exact size already enforced by the BDA normalization above.
                VkPhysicalDeviceVulkan12Features f{};
                std::memcpy(&f, e.blob.data(), sizeof(f));
                fold13(f.vulkanMemoryModel, vkrpc::kVk13FeatureVulkanMemoryModel);
                fold13(f.vulkanMemoryModelDeviceScope,
                       vkrpc::kVk13FeatureVulkanMemoryModelDeviceScope);
                fold13(f.vulkanMemoryModelAvailabilityVisibilityChains,
                       vkrpc::kVk13FeatureVulkanMemoryModelAvailabilityVisibilityChains);
                break;
            }
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES: {
                if (!expect(sizeof(VkPhysicalDeviceVulkanMemoryModelFeatures))) {
                    resp.ok = false;
                    resp.reason = "VulkanMemoryModelFeatures struct has a wrong size";
                    return resp;
                }
                VkPhysicalDeviceVulkanMemoryModelFeatures f{};
                std::memcpy(&f, e.blob.data(), sizeof(f));
                fold13(f.vulkanMemoryModel, vkrpc::kVk13FeatureVulkanMemoryModel);
                fold13(f.vulkanMemoryModelDeviceScope,
                       vkrpc::kVk13FeatureVulkanMemoryModelDeviceScope);
                fold13(f.vulkanMemoryModelAvailabilityVisibilityChains,
                       vkrpc::kVk13FeatureVulkanMemoryModelAvailabilityVisibilityChains);
                break;
            }
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_ROBUSTNESS_FEATURES: {
                if (!expect(sizeof(VkPhysicalDeviceImageRobustnessFeatures))) {
                    resp.ok = false;
                    resp.reason = "ImageRobustnessFeatures struct has a wrong size";
                    return resp;
                }
                VkPhysicalDeviceImageRobustnessFeatures f{};
                std::memcpy(&f, e.blob.data(), sizeof(f));
                fold13(f.robustImageAccess, vkrpc::kVk13FeatureRobustImageAccess);
                break;
            }
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INLINE_UNIFORM_BLOCK_FEATURES: {
                if (!expect(sizeof(VkPhysicalDeviceInlineUniformBlockFeatures))) {
                    resp.ok = false;
                    resp.reason = "InlineUniformBlockFeatures struct has a wrong size";
                    return resp;
                }
                VkPhysicalDeviceInlineUniformBlockFeatures f{};
                std::memcpy(&f, e.blob.data(), sizeof(f));
                fold13(f.inlineUniformBlock, vkrpc::kVk13FeatureInlineUniformBlock);
                chain_vk13_unserved =
                    chain_vk13_unserved ||
                    f.descriptorBindingInlineUniformBlockUpdateAfterBind != VK_FALSE;
                break;
            }
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_CREATION_CACHE_CONTROL_FEATURES: {
                if (!expect(sizeof(VkPhysicalDevicePipelineCreationCacheControlFeatures))) {
                    resp.ok = false;
                    resp.reason = "PipelineCreationCacheControlFeatures struct has a wrong size";
                    return resp;
                }
                VkPhysicalDevicePipelineCreationCacheControlFeatures f{};
                std::memcpy(&f, e.blob.data(), sizeof(f));
                fold13(f.pipelineCreationCacheControl,
                       vkrpc::kVk13FeaturePipelineCreationCacheControl);
                break;
            }
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIVATE_DATA_FEATURES: {
                if (!expect(sizeof(VkPhysicalDevicePrivateDataFeatures))) {
                    resp.ok = false;
                    resp.reason = "PrivateDataFeatures struct has a wrong size";
                    return resp;
                }
                VkPhysicalDevicePrivateDataFeatures f{};
                std::memcpy(&f, e.blob.data(), sizeof(f));
                fold13(f.privateData, vkrpc::kVk13FeaturePrivateData);
                break;
            }
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DEMOTE_TO_HELPER_INVOCATION_FEATURES: {
                if (!expect(sizeof(VkPhysicalDeviceShaderDemoteToHelperInvocationFeatures))) {
                    resp.ok = false;
                    resp.reason = "ShaderDemoteToHelperInvocationFeatures struct has a wrong size";
                    return resp;
                }
                VkPhysicalDeviceShaderDemoteToHelperInvocationFeatures f{};
                std::memcpy(&f, e.blob.data(), sizeof(f));
                fold13(f.shaderDemoteToHelperInvocation,
                       vkrpc::kVk13FeatureShaderDemoteToHelperInvocation);
                break;
            }
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_TERMINATE_INVOCATION_FEATURES: {
                if (!expect(sizeof(VkPhysicalDeviceShaderTerminateInvocationFeatures))) {
                    resp.ok = false;
                    resp.reason = "ShaderTerminateInvocationFeatures struct has a wrong size";
                    return resp;
                }
                VkPhysicalDeviceShaderTerminateInvocationFeatures f{};
                std::memcpy(&f, e.blob.data(), sizeof(f));
                fold13(f.shaderTerminateInvocation, vkrpc::kVk13FeatureShaderTerminateInvocation);
                break;
            }
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES: {
                if (!expect(sizeof(VkPhysicalDeviceSubgroupSizeControlFeatures))) {
                    resp.ok = false;
                    resp.reason = "SubgroupSizeControlFeatures struct has a wrong size";
                    return resp;
                }
                VkPhysicalDeviceSubgroupSizeControlFeatures f{};
                std::memcpy(&f, e.blob.data(), sizeof(f));
                fold13(f.subgroupSizeControl, vkrpc::kVk13FeatureSubgroupSizeControl);
                fold13(f.computeFullSubgroups, vkrpc::kVk13FeatureComputeFullSubgroups);
                break;
            }
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ZERO_INITIALIZE_WORKGROUP_MEMORY_FEATURES: {
                if (!expect(sizeof(VkPhysicalDeviceZeroInitializeWorkgroupMemoryFeatures))) {
                    resp.ok = false;
                    resp.reason = "ZeroInitializeWorkgroupMemoryFeatures struct has a wrong size";
                    return resp;
                }
                VkPhysicalDeviceZeroInitializeWorkgroupMemoryFeatures f{};
                std::memcpy(&f, e.blob.data(), sizeof(f));
                fold13(f.shaderZeroInitializeWorkgroupMemory,
                       vkrpc::kVk13FeatureShaderZeroInitializeWorkgroupMemory);
                break;
            }
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_INTEGER_DOT_PRODUCT_FEATURES: {
                if (!expect(sizeof(VkPhysicalDeviceShaderIntegerDotProductFeatures))) {
                    resp.ok = false;
                    resp.reason = "ShaderIntegerDotProductFeatures struct has a wrong size";
                    return resp;
                }
                VkPhysicalDeviceShaderIntegerDotProductFeatures f{};
                std::memcpy(&f, e.blob.data(), sizeof(f));
                fold13(f.shaderIntegerDotProduct, vkrpc::kVk13FeatureShaderIntegerDotProduct);
                break;
            }
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_FEATURES: {
                if (!expect(sizeof(VkPhysicalDeviceMaintenance4Features))) {
                    resp.ok = false;
                    resp.reason = "Maintenance4Features struct has a wrong size";
                    return resp;
                }
                VkPhysicalDeviceMaintenance4Features f{};
                std::memcpy(&f, e.blob.data(), sizeof(f));
                fold13(f.maintenance4, vkrpc::kVk13FeatureMaintenance4);
                break;
            }
            default:
                break;
            }
        }
        if (chain_vk13_unserved) {
            resp.ok = false;
            resp.reason = "unserved 1.3-family members requested "
                          "(inline-uniform-block UAB / ASTC-HDR)";
            return resp;
        }
        if (req.vk13_feature_bits != chain_vk13) {
            resp.ok = false;
            resp.reason = "vk13 scalar/feature-chain mismatch (the request bits and the "
                          "enabled-feature chain must agree)";
            return resp;
        }
    }
    for (std::size_t i = 0; i < chain_bufs.size(); ++i) {
        void* next =
            (i + 1 < chain_bufs.size()) ? static_cast<void*>(chain_bufs[i + 1].data()) : nullptr;
        std::memcpy(chain_bufs[i].data() + offsetof(VkBaseOutStructure, pNext), &next,
                    sizeof(void*));
    }
    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = static_cast<std::uint32_t>(ext_ptrs.size());
    dci.ppEnabledExtensionNames = ext_ptrs.data();
    if (!chain_bufs.empty()) {
        dci.pNext = chain_bufs[0].data();
    }
    // pEnabledFeatures is forbidden only when Features2 is present. An extension-only feature
    // chain may legally coexist with the base struct and must not silently disable its bits.
    dci.pEnabledFeatures = chain_has_features2 ? nullptr : &enabled_feats;
    // cross-adapter retirement: chain the swapchainMaintenance1 ENABLE struct. Prepending
    // works for both branches (an extension feature struct is legal alongside pEnabledFeatures;
    // only a second VkPhysicalDeviceFeatures2 would conflict).
    VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT sm1_enable{};
    if (present_fence_retire) {
        sm1_enable.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT;
        sm1_enable.swapchainMaintenance1 = VK_TRUE;
        sm1_enable.pNext = const_cast<void*>(dci.pNext);
        dci.pNext = &sm1_enable;
    }

    VkDevice vk = VK_NULL_HANDLE;
    if (vkCreateDevice(it->second.physical_vk, &dci, nullptr, &vk) != VK_SUCCESS ||
        vk == VK_NULL_HANDLE) {
        resp.ok = false;
        resp.reason = "vkCreateDevice failed";
        return resp;
    }
    const std::uint64_t handle = next_handle_++;
    RealDevice rd;
    rd.vk = vk;
    rd.instance = req.instance;
    rd.queue_family = static_cast<std::uint32_t>(family);
    rd.queue_count = 1; // one queue from one graphics family (see qci above)
    // (GL/zink): remember what this device actually enabled, and resolve the wired extension
    // command PFNs. Loaded ONLY when the extension is enabled, so the enabled-set validation and
    // "PFN is callable" stay one fact.
    rd.enabled_extensions.insert(ext_storage.begin(), ext_storage.end());
    rd.present_fence_retire = present_fence_retire; // cross-adapter retirement
    rd.dynamic_rendering_feature_enabled = req.dynamic_rendering_feature_enabled != 0;
    rd.synchronization2_feature_enabled = req.synchronization2_feature_enabled != 0;
    // Required-feature audit (hardening): normalized against the chain
    // just above; reset_query_pool fails closed unless set (mock == real).
    rd.host_query_reset_feature_enabled = req.host_query_reset_feature_enabled != 0;
    // Required-feature audit: normalized against the chain just above;
    // create_render_pass fails closed a viewMask pass unless set (mock == real).
    rd.multiview_feature_enabled = req.multiview_feature_enabled != 0;
    rd.buffer_device_address_feature_enabled = req.buffer_device_address_feature_enabled != 0;
    rd.vertex_attr_divisor_feature_enabled = req.vertex_attr_divisor_feature_enabled != 0;
    rd.vertex_attr_zero_divisor_feature_enabled = req.vertex_attr_zero_divisor_feature_enabled != 0;
    // geometry-stream: the NORMALIZED feature (explicit scalar chain-agreement-checked above;
    // omitted scalar derived from the chain) + the host's two stream
    // PROPERTIES, cached once here (immutable per physical device) so the pipeline-create gate
    // (shared vkrpc::rasterization_stream_ok) is table-local. Queried only when the extension is
    // enabled; core-1.1 props2 is safe (the worker instance requests >= 1.1 on this host path).
    rd.geometry_streams_feature_enabled = geometry_streams_enabled;
    rd.multi_draw_indirect_feature_enabled =
        (enabled_base_bits & vkrpc::kFeatureMultiDrawIndirect) != 0;
    rd.draw_indirect_count_enabled = draw_indirect_count_enabled;
    if (rd.enabled_extensions.count(VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME) != 0) {
        VkPhysicalDeviceTransformFeedbackPropertiesEXT tf_props{};
        tf_props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_PROPERTIES_EXT;
        VkPhysicalDeviceProperties2 p2{};
        p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        p2.pNext = &tf_props;
        vkGetPhysicalDeviceProperties2(it->second.physical_vk, &p2);
        rd.max_transform_feedback_streams = tf_props.maxTransformFeedbackStreams;
        rd.transform_feedback_rasterization_stream_select =
            tf_props.transformFeedbackRasterizationStreamSelect != VK_FALSE;
    }
    rd.descriptor_indexing_feature_bits = req.descriptor_indexing_feature_bits; // normalized above
    rd.vk13_feature_bits = req.vk13_feature_bits;  // Vulkan 1.3 support: normalized above
    rd.vk13_device = req.vk13_device_enabled != 0; // ... and host re-verified above
    if (rd.enabled_extensions.count(VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME) != 0) {
        rd.pfn_bind_tf_buffers = reinterpret_cast<PFN_vkCmdBindTransformFeedbackBuffersEXT>(
            vkGetDeviceProcAddr(vk, "vkCmdBindTransformFeedbackBuffersEXT"));
        rd.pfn_begin_tf = reinterpret_cast<PFN_vkCmdBeginTransformFeedbackEXT>(
            vkGetDeviceProcAddr(vk, "vkCmdBeginTransformFeedbackEXT"));
        rd.pfn_end_tf = reinterpret_cast<PFN_vkCmdEndTransformFeedbackEXT>(
            vkGetDeviceProcAddr(vk, "vkCmdEndTransformFeedbackEXT"));
        rd.pfn_draw_indirect_byte_count = reinterpret_cast<PFN_vkCmdDrawIndirectByteCountEXT>(
            vkGetDeviceProcAddr(vk, "vkCmdDrawIndirectByteCountEXT"));
    }
    if (rd.draw_indirect_count_enabled) {
        // Bind the spelling whose enable path made the command legal: KHR aliases for the enabled
        // extension, core names for the promoted Vulkan-1.2 feature. Do not fall back across an
        // unenabled surface merely because a loader happens to return its pointer.
        const bool khr_enabled =
            rd.enabled_extensions.count(VK_KHR_DRAW_INDIRECT_COUNT_EXTENSION_NAME) != 0;
        rd.pfn_draw_indirect_count =
            reinterpret_cast<PFN_vkCmdDrawIndirectCount>(vkGetDeviceProcAddr(
                vk, khr_enabled ? "vkCmdDrawIndirectCountKHR" : "vkCmdDrawIndirectCount"));
        rd.pfn_draw_indexed_indirect_count = reinterpret_cast<PFN_vkCmdDrawIndexedIndirectCount>(
            vkGetDeviceProcAddr(vk, khr_enabled ? "vkCmdDrawIndexedIndirectCountKHR"
                                                : "vkCmdDrawIndexedIndirectCount"));
        if (rd.pfn_draw_indirect_count == nullptr ||
            rd.pfn_draw_indexed_indirect_count == nullptr) {
            vkDestroyDevice(vk, nullptr);
            resp.ok = false;
            resp.reason = "indirect-count drawing enabled but its host commands did not resolve";
            return resp;
        }
    }
    if (rd.enabled_extensions.count(VK_EXT_CONDITIONAL_RENDERING_EXTENSION_NAME) != 0) {
        rd.pfn_begin_cond_render = reinterpret_cast<PFN_vkCmdBeginConditionalRenderingEXT>(
            vkGetDeviceProcAddr(vk, "vkCmdBeginConditionalRenderingEXT"));
        rd.pfn_end_cond_render = reinterpret_cast<PFN_vkCmdEndConditionalRenderingEXT>(
            vkGetDeviceProcAddr(vk, "vkCmdEndConditionalRenderingEXT"));
    }
    // (native lane -- EDS1): resolve the *EXT setters so replay serves the advertised
    // extension honestly. EDS is only ever advertised when the host advertises the
    // extension, so when the app enables it these aliases always resolve.
    // Vulkan 1.3 support: the full TWELVE EDS1 setters, admitted by the extension OR an honest vk13
    // device; each resolves the *EXT name first, falling back to the core name (a promoted-core
    // host may serve only the core spellings).
    if (rd.enabled_extensions.count(VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME) != 0 ||
        rd.vk13_device) {
        const auto resolve_eds = [&](const char* ext, const char* core) {
            PFN_vkVoidFunction p = vkGetDeviceProcAddr(vk, ext);
            return p != nullptr ? p : vkGetDeviceProcAddr(vk, core);
        };
        rd.pfn_set_cull_mode = reinterpret_cast<PFN_vkCmdSetCullModeEXT>(
            resolve_eds("vkCmdSetCullModeEXT", "vkCmdSetCullMode"));
        rd.pfn_set_front_face = reinterpret_cast<PFN_vkCmdSetFrontFaceEXT>(
            resolve_eds("vkCmdSetFrontFaceEXT", "vkCmdSetFrontFace"));
        rd.pfn_set_primitive_topology = reinterpret_cast<PFN_vkCmdSetPrimitiveTopologyEXT>(
            resolve_eds("vkCmdSetPrimitiveTopologyEXT", "vkCmdSetPrimitiveTopology"));
        rd.pfn_set_depth_test_enable = reinterpret_cast<PFN_vkCmdSetDepthTestEnableEXT>(
            resolve_eds("vkCmdSetDepthTestEnableEXT", "vkCmdSetDepthTestEnable"));
        rd.pfn_set_depth_write_enable = reinterpret_cast<PFN_vkCmdSetDepthWriteEnableEXT>(
            resolve_eds("vkCmdSetDepthWriteEnableEXT", "vkCmdSetDepthWriteEnable"));
        rd.pfn_set_depth_compare_op = reinterpret_cast<PFN_vkCmdSetDepthCompareOpEXT>(
            resolve_eds("vkCmdSetDepthCompareOpEXT", "vkCmdSetDepthCompareOp"));
        rd.pfn_set_viewport_with_count = reinterpret_cast<PFN_vkCmdSetViewportWithCountEXT>(
            resolve_eds("vkCmdSetViewportWithCountEXT", "vkCmdSetViewportWithCount"));
        rd.pfn_set_scissor_with_count = reinterpret_cast<PFN_vkCmdSetScissorWithCountEXT>(
            resolve_eds("vkCmdSetScissorWithCountEXT", "vkCmdSetScissorWithCount"));
        rd.pfn_bind_vertex_buffers2 = reinterpret_cast<PFN_vkCmdBindVertexBuffers2EXT>(
            resolve_eds("vkCmdBindVertexBuffers2EXT", "vkCmdBindVertexBuffers2"));
        rd.pfn_set_depth_bounds_test_enable =
            reinterpret_cast<PFN_vkCmdSetDepthBoundsTestEnableEXT>(
                resolve_eds("vkCmdSetDepthBoundsTestEnableEXT", "vkCmdSetDepthBoundsTestEnable"));
        rd.pfn_set_stencil_test_enable = reinterpret_cast<PFN_vkCmdSetStencilTestEnableEXT>(
            resolve_eds("vkCmdSetStencilTestEnableEXT", "vkCmdSetStencilTestEnable"));
        rd.pfn_set_stencil_op = reinterpret_cast<PFN_vkCmdSetStencilOpEXT>(
            resolve_eds("vkCmdSetStencilOpEXT", "vkCmdSetStencilOp"));
        // Fail closed (hardening): EDS is enabled but the loader/driver handed back a
        // null setter. On a conformant stack this never fires -- the host advertised the extension
        // (or is a 1.3 host serving the core names) -- but if it does, refuse the device here
        // rather than reach a null PFN at replay. Keeps the non-null-PFN-at-emit contract a fact,
        // not an assumption.
        if (rd.pfn_set_cull_mode == nullptr || rd.pfn_set_front_face == nullptr ||
            rd.pfn_set_primitive_topology == nullptr || rd.pfn_set_depth_test_enable == nullptr ||
            rd.pfn_set_depth_write_enable == nullptr || rd.pfn_set_depth_compare_op == nullptr ||
            rd.pfn_set_viewport_with_count == nullptr || rd.pfn_set_scissor_with_count == nullptr ||
            rd.pfn_bind_vertex_buffers2 == nullptr ||
            rd.pfn_set_depth_bounds_test_enable == nullptr ||
            rd.pfn_set_stencil_test_enable == nullptr || rd.pfn_set_stencil_op == nullptr) {
            vkDestroyDevice(vk, nullptr);
            resp.ok = false;
            resp.reason = "extended dynamic state enabled but its setters did not resolve on "
                          "this host";
            return resp;
        }
    }
    // Vulkan 1.3 support (EDS2 subset): the three enable-toggles core 1.3 absorbed from
    // VK_EXT_extended_dynamic_state2, resolved by CORE name only (the extension is not
    // advertised) on an honest vk13 device. Same fail-closed null guard as EDS1.
    if (rd.vk13_device) {
        rd.pfn_set_rasterizer_discard_enable =
            reinterpret_cast<PFN_vkCmdSetRasterizerDiscardEnableEXT>(
                vkGetDeviceProcAddr(vk, "vkCmdSetRasterizerDiscardEnable"));
        rd.pfn_set_depth_bias_enable = reinterpret_cast<PFN_vkCmdSetDepthBiasEnableEXT>(
            vkGetDeviceProcAddr(vk, "vkCmdSetDepthBiasEnable"));
        rd.pfn_set_primitive_restart_enable =
            reinterpret_cast<PFN_vkCmdSetPrimitiveRestartEnableEXT>(
                vkGetDeviceProcAddr(vk, "vkCmdSetPrimitiveRestartEnable"));
        if (rd.pfn_set_rasterizer_discard_enable == nullptr ||
            rd.pfn_set_depth_bias_enable == nullptr ||
            rd.pfn_set_primitive_restart_enable == nullptr) {
            vkDestroyDevice(vk, nullptr);
            resp.ok = false;
            resp.reason = "core-1.3 dynamic-state commands did not resolve on this host";
            return resp;
        }
    }
    // (native lane): resolve VK_KHR_dynamic_rendering's *KHR commands so replay
    // serves the ADVERTISED extension honestly (not core-1.3). Same fail-closed null guard as EDS:
    // if either resolves null, refuse the device rather than reach a null PFN at replay. The ICD
    // advertises DR only when the host does, so an enabled DR always resolves its *KHR PFNs.
    // Vulkan 1.3 support: on an honest vk13 device the DR commands are CORE -- resolve the KHR
    // names first (the advertised-extension path), falling back to the core names (a promoted-core
    // host); same fail-closed null guard either way.
    if (rd.enabled_extensions.count(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME) != 0 ||
        rd.vk13_device) {
        rd.pfn_begin_rendering = reinterpret_cast<PFN_vkCmdBeginRenderingKHR>(
            vkGetDeviceProcAddr(vk, "vkCmdBeginRenderingKHR"));
        if (rd.pfn_begin_rendering == nullptr) {
            rd.pfn_begin_rendering = reinterpret_cast<PFN_vkCmdBeginRenderingKHR>(
                vkGetDeviceProcAddr(vk, "vkCmdBeginRendering"));
        }
        rd.pfn_end_rendering = reinterpret_cast<PFN_vkCmdEndRenderingKHR>(
            vkGetDeviceProcAddr(vk, "vkCmdEndRenderingKHR"));
        if (rd.pfn_end_rendering == nullptr) {
            rd.pfn_end_rendering = reinterpret_cast<PFN_vkCmdEndRenderingKHR>(
                vkGetDeviceProcAddr(vk, "vkCmdEndRendering"));
        }
        if (rd.pfn_begin_rendering == nullptr || rd.pfn_end_rendering == nullptr) {
            vkDestroyDevice(vk, nullptr);
            resp.ok = false;
            resp.reason = "dynamic rendering enabled but vkCmdBegin/EndRendering did not "
                          "resolve on this host";
            return resp;
        }
    }
    // (native lane): resolve VK_KHR_synchronization2's six *KHR commands so
    // replay serves the ADVERTISED extension honestly. One fail-closed null guard over all six: if
    // any resolves null, refuse the device rather than reach a null PFN at replay. The ICD
    // advertises sync2 only when the host does, so an enabled sync2 always resolves its *KHR PFNs.
    // Vulkan 1.3 support: same KHR-first / core-fallback resolution for the sync2 commands on an
    // honest vk13 device (they are core there).
    if (rd.enabled_extensions.count(VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME) != 0 ||
        rd.vk13_device) {
        const auto resolve2 = [&](const char* khr, const char* core) {
            PFN_vkVoidFunction p = vkGetDeviceProcAddr(vk, khr);
            return p != nullptr ? p : vkGetDeviceProcAddr(vk, core);
        };
        rd.pfn_pipeline_barrier2 = reinterpret_cast<PFN_vkCmdPipelineBarrier2KHR>(
            resolve2("vkCmdPipelineBarrier2KHR", "vkCmdPipelineBarrier2"));
        rd.pfn_write_timestamp2 = reinterpret_cast<PFN_vkCmdWriteTimestamp2KHR>(
            resolve2("vkCmdWriteTimestamp2KHR", "vkCmdWriteTimestamp2"));
        rd.pfn_queue_submit2 = reinterpret_cast<PFN_vkQueueSubmit2KHR>(
            resolve2("vkQueueSubmit2KHR", "vkQueueSubmit2"));
        rd.pfn_set_event2 = reinterpret_cast<PFN_vkCmdSetEvent2KHR>(
            resolve2("vkCmdSetEvent2KHR", "vkCmdSetEvent2"));
        rd.pfn_reset_event2 = reinterpret_cast<PFN_vkCmdResetEvent2KHR>(
            resolve2("vkCmdResetEvent2KHR", "vkCmdResetEvent2"));
        rd.pfn_wait_events2 = reinterpret_cast<PFN_vkCmdWaitEvents2KHR>(
            resolve2("vkCmdWaitEvents2KHR", "vkCmdWaitEvents2"));
        if (rd.pfn_pipeline_barrier2 == nullptr || rd.pfn_write_timestamp2 == nullptr ||
            rd.pfn_queue_submit2 == nullptr || rd.pfn_set_event2 == nullptr ||
            rd.pfn_reset_event2 == nullptr || rd.pfn_wait_events2 == nullptr) {
            vkDestroyDevice(vk, nullptr);
            resp.ok = false;
            resp.reason = "synchronization2 enabled but its commands did not resolve on this host";
            return resp;
        }
    }
    // (bufferDeviceAddress, core 1.2 -- no extension): when the app enabled the feature
    // (the scalar/chain agreement was NORMALIZED above, so the rebuilt chain really carried
    // bufferDeviceAddress=TRUE to the host vkCreateDevice), verify the HOST physical device
    // actually supports it and resolve the CORE PFN, both fail-closed -- calling
    // vkGetBufferDeviceAddress on a device without the feature enabled is UB, never "just null".
    if (rd.buffer_device_address_feature_enabled) {
        VkPhysicalDeviceBufferDeviceAddressFeatures host_bda{};
        host_bda.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES;
        VkPhysicalDeviceFeatures2 host_f2{};
        host_f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        host_f2.pNext = &host_bda;
        vkGetPhysicalDeviceFeatures2(it->second.physical_vk, &host_f2);
        rd.pfn_get_buffer_device_address = reinterpret_cast<PFN_vkGetBufferDeviceAddress>(
            vkGetDeviceProcAddr(vk, "vkGetBufferDeviceAddress"));
        if (host_bda.bufferDeviceAddress == VK_FALSE ||
            rd.pfn_get_buffer_device_address == nullptr) {
            vkDestroyDevice(vk, nullptr);
            resp.ok = false;
            resp.reason = "bufferDeviceAddress feature enabled but the host does not support it "
                          "(or vkGetBufferDeviceAddress did not resolve)";
            return resp;
        }
    }
    // descriptorIndexing: when any DI bit is enabled (the chain-normalized set), verify the
    // HOST physical device supports each requested sub-feature -- the chain rode to the host
    // vkCreateDevice verbatim, so a host without one would have produced an invalid device.
    if (rd.descriptor_indexing_feature_bits != 0) {
        VkPhysicalDeviceDescriptorIndexingFeatures host_di{};
        host_di.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES;
        VkPhysicalDeviceFeatures2 host_f2{};
        host_f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        host_f2.pNext = &host_di;
        vkGetPhysicalDeviceFeatures2(it->second.physical_vk, &host_f2);
        const struct {
            std::uint64_t bit;
            VkBool32 host;
        } need[] = {
            {vkrpc::kDIFeatureUpdateAfterBindUniformBuffer,
             host_di.descriptorBindingUniformBufferUpdateAfterBind},
            {vkrpc::kDIFeatureUpdateAfterBindStorageBuffer,
             host_di.descriptorBindingStorageBufferUpdateAfterBind},
            {vkrpc::kDIFeatureUpdateAfterBindSampledImage,
             host_di.descriptorBindingSampledImageUpdateAfterBind},
            {vkrpc::kDIFeatureUpdateAfterBindStorageImage,
             host_di.descriptorBindingStorageImageUpdateAfterBind},
            {vkrpc::kDIFeatureUpdateAfterBindUniformTexelBuffer,
             host_di.descriptorBindingUniformTexelBufferUpdateAfterBind},
            {vkrpc::kDIFeatureUpdateAfterBindStorageTexelBuffer,
             host_di.descriptorBindingStorageTexelBufferUpdateAfterBind},
            {vkrpc::kDIFeatureUpdateUnusedWhilePending,
             host_di.descriptorBindingUpdateUnusedWhilePending},
            {vkrpc::kDIFeaturePartiallyBound, host_di.descriptorBindingPartiallyBound},
            {vkrpc::kDIFeatureVariableDescriptorCount,
             host_di.descriptorBindingVariableDescriptorCount},
            {vkrpc::kDIFeatureRuntimeDescriptorArray, host_di.runtimeDescriptorArray},
            {vkrpc::kDIFeatureShaderUniformBufferArrayNonUniformIndexing,
             host_di.shaderUniformBufferArrayNonUniformIndexing},
            {vkrpc::kDIFeatureShaderStorageBufferArrayNonUniformIndexing,
             host_di.shaderStorageBufferArrayNonUniformIndexing},
        };
        for (const auto& n : need) {
            if ((rd.descriptor_indexing_feature_bits & n.bit) != 0 && n.host == VK_FALSE) {
                vkDestroyDevice(vk, nullptr);
                resp.ok = false;
                resp.reason = "a requested descriptorIndexing sub-feature is not supported by the "
                              "host";
                return resp;
            }
        }
    }
    // Vulkan 1.3 support: verify the HOST supports each enabled vk13 bit (the chain rode to the
    // host vkCreateDevice verbatim), and resolve the maintenance4 query PFNs fail-closed when
    // that feature is enabled.
    if (rd.vk13_feature_bits != 0) {
        VkPhysicalDeviceVulkan13Features host_f13{};
        host_f13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
        VkPhysicalDeviceVulkan12Features host_f12{};
        host_f12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
        host_f12.pNext = &host_f13;
        VkPhysicalDeviceFeatures2 host_f2{};
        host_f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        host_f2.pNext = &host_f12;
        vkGetPhysicalDeviceFeatures2(it->second.physical_vk, &host_f2);
        const struct {
            std::uint64_t bit;
            VkBool32 host;
        } need13[] = {
            {vkrpc::kVk13FeatureRobustImageAccess, host_f13.robustImageAccess},
            {vkrpc::kVk13FeatureInlineUniformBlock, host_f13.inlineUniformBlock},
            {vkrpc::kVk13FeaturePipelineCreationCacheControl,
             host_f13.pipelineCreationCacheControl},
            {vkrpc::kVk13FeaturePrivateData, host_f13.privateData},
            {vkrpc::kVk13FeatureShaderDemoteToHelperInvocation,
             host_f13.shaderDemoteToHelperInvocation},
            {vkrpc::kVk13FeatureShaderTerminateInvocation, host_f13.shaderTerminateInvocation},
            {vkrpc::kVk13FeatureSubgroupSizeControl, host_f13.subgroupSizeControl},
            {vkrpc::kVk13FeatureComputeFullSubgroups, host_f13.computeFullSubgroups},
            {vkrpc::kVk13FeatureShaderZeroInitializeWorkgroupMemory,
             host_f13.shaderZeroInitializeWorkgroupMemory},
            {vkrpc::kVk13FeatureShaderIntegerDotProduct, host_f13.shaderIntegerDotProduct},
            {vkrpc::kVk13FeatureMaintenance4, host_f13.maintenance4},
            {vkrpc::kVk13FeatureVulkanMemoryModel, host_f12.vulkanMemoryModel},
            {vkrpc::kVk13FeatureVulkanMemoryModelDeviceScope,
             host_f12.vulkanMemoryModelDeviceScope},
            {vkrpc::kVk13FeatureVulkanMemoryModelAvailabilityVisibilityChains,
             host_f12.vulkanMemoryModelAvailabilityVisibilityChains},
        };
        for (const auto& n : need13) {
            if ((rd.vk13_feature_bits & n.bit) != 0 && n.host == VK_FALSE) {
                vkDestroyDevice(vk, nullptr);
                resp.ok = false;
                resp.reason = "a requested vk13 feature is not supported by the host";
                return resp;
            }
        }
        if ((rd.vk13_feature_bits & vkrpc::kVk13FeatureMaintenance4) != 0) {
            rd.pfn_get_device_buffer_memory_requirements =
                reinterpret_cast<PFN_vkGetDeviceBufferMemoryRequirements>(
                    vkGetDeviceProcAddr(vk, "vkGetDeviceBufferMemoryRequirements"));
            rd.pfn_get_device_image_memory_requirements =
                reinterpret_cast<PFN_vkGetDeviceImageMemoryRequirements>(
                    vkGetDeviceProcAddr(vk, "vkGetDeviceImageMemoryRequirements"));
            if (rd.pfn_get_device_buffer_memory_requirements == nullptr ||
                rd.pfn_get_device_image_memory_requirements == nullptr) {
                vkDestroyDevice(vk, nullptr);
                resp.ok = false;
                resp.reason = "maintenance4 enabled but its query commands did not resolve on "
                              "this host";
                return resp;
            }
        }
    }
    devices_.emplace(handle, rd);
    it->second.devices.insert(handle);
    resp.ok = true;
    resp.reason = "ok";
    resp.device = handle;
    resp.queue_family_index = family;
    resp.queue_count = static_cast<int>(rd.queue_count);
    return resp;
}

vkrpc::StatusResponse RealVulkanBackend::destroy_device(const vkrpc::HandleRequest& req) {
    vkrpc::StatusResponse resp;
    const auto it = devices_.find(req.handle);
    if (it == devices_.end()) {
        resp.ok = false;
        resp.reason = "unknown device handle";
        return resp;
    }
    // Device children must be destroyed first (per Vulkan, and matching the mock).
    // Queues are retrieved, not created, so they don't block.
    if (!it->second.pools.empty()) {
        resp.ok = false;
        resp.reason = "device has live command pools; destroy them first";
        return resp;
    }
    if (!it->second.leaves.empty()) {
        resp.ok = false;
        resp.reason = "device has live fences/semaphores/memory; destroy them first";
        return resp;
    }
    if (!it->second.swapchains.empty()) {
        resp.ok = false;
        resp.reason = "device has live swapchains; destroy them first";
        return resp;
    }
    // Draw-surface children block the device's destroy too.
    if (!it->second.image_views.empty() || !it->second.shader_modules.empty() ||
        !it->second.render_passes.empty() || !it->second.framebuffers.empty() ||
        !it->second.pipeline_layouts.empty() || !it->second.pipelines.empty()) {
        resp.ok = false;
        resp.reason =
            "device has live draw-surface objects (image views / shaders / render passes / "
            "framebuffers / pipeline layouts / pipelines); destroy them first";
        return resp;
    }
    if (!it->second.buffers.empty()) {
        resp.ok = false;
        resp.reason = "device has live buffers; destroy them first";
        return resp;
    }
    if (!it->second.descriptor_set_layouts.empty() || !it->second.descriptor_pools.empty()) {
        resp.ok = false;
        resp.reason = "device has live descriptor set layouts / pools; destroy them first";
        return resp;
    }
    if (!it->second.images.empty()) {
        resp.ok = false;
        resp.reason = "device has live images; destroy them first";
        return resp;
    }
    if (!it->second.samplers.empty()) {
        resp.ok = false;
        resp.reason = "device has live samplers; destroy them first";
        return resp;
    }
    if (!it->second.query_pools.empty()) {
        resp.ok = false;
        resp.reason = "device has live query pools; destroy them first";
        return resp;
    }
    // Lost-device containment: a lost device is ABANDONED, never handed back
    // to the driver (the instance carries the sticky taint so its own teardown is skipped too).
    if (!it->second.lost) {
        vkDestroyDevice(it->second.vk, nullptr);
    }
    const auto inst = instances_.find(it->second.instance);
    if (inst != instances_.end()) {
        inst->second.devices.erase(req.handle);
    }
    devices_.erase(it);
    resp.ok = true;
    resp.reason = "ok";
    return resp;
}

vkrpc::StatusResponse RealVulkanBackend::destroy_instance(const vkrpc::HandleRequest& req) {
    vkrpc::StatusResponse resp;
    const auto it = instances_.find(req.handle);
    if (it == instances_.end()) {
        resp.ok = false;
        resp.reason = "unknown instance handle";
        return resp;
    }
    if (!it->second.devices.empty()) {
        resp.ok = false;
        resp.reason = "instance has live devices; destroy them first";
        return resp;
    }
    if (!it->second.surfaces.empty()) {
        resp.ok = false;
        resp.reason = "instance has live surfaces; destroy them first";
        return resp;
    }
    // Lost-device containment: a tainted instance is abandoned (vkDestroyInstance under a live
    // abandoned VkDevice child is UB -- observed AV).
    if (!it->second.lost_device_taint) {
        vkDestroyInstance(it->second.vk, nullptr);
    }
    instances_.erase(it);
    resp.ok = true;
    resp.reason = "ok";
    return resp;
}

vkrpc::GetDeviceQueueResponse
RealVulkanBackend::get_device_queue(const vkrpc::GetDeviceQueueRequest& req) {
    vkrpc::GetDeviceQueueResponse resp;
    const auto it = devices_.find(req.device);
    if (it == devices_.end()) {
        resp.ok = false;
        resp.reason = "unknown device handle";
        return resp;
    }
    RealDevice& dev = it->second;
    // Only a (family, index) create_device actually requested may be retrieved:
    // vkGetDeviceQueue with any other family/index is undefined behavior. The app
    // must use the family/index reported by create_device. (from_body has already
    // rejected out-of-range wire values to a -1 sentinel, which fails here too.)
    if (req.queue_family_index < 0 ||
        static_cast<std::uint32_t>(req.queue_family_index) != dev.queue_family) {
        resp.ok = false;
        resp.reason = "queue family was not created on this device "
                      "(use the family reported by create_device)";
        return resp;
    }
    if (req.queue_index < 0 || static_cast<std::uint32_t>(req.queue_index) >= dev.queue_count) {
        resp.ok = false;
        resp.reason = "queue index out of range for the created family";
        return resp;
    }
    // Queues are retrieved, not created: the same (family, index) returns a stable
    // handle for the device's lifetime (the VkQueue is device-owned, no destroy).
    const std::uint64_t key =
        queue_key(dev.queue_family, static_cast<std::uint32_t>(req.queue_index));
    const auto cached = dev.queues.find(key);
    if (cached != dev.queues.end()) {
        resp.queue = cached->second;
        resp.ok = true;
        resp.reason = "ok";
        return resp;
    }
    VkQueue q = VK_NULL_HANDLE;
    vkGetDeviceQueue(dev.vk, dev.queue_family, static_cast<std::uint32_t>(req.queue_index), &q);
    if (q == VK_NULL_HANDLE) {
        resp.ok = false;
        resp.reason = "vkGetDeviceQueue returned a null queue";
        return resp;
    }
    const std::uint64_t handle = next_handle_++;
    dev.queues.emplace(key, handle);
    resp.queue = handle;
    resp.ok = true;
    resp.reason = "ok";
    return resp;
}
vkrpc::CreateCommandPoolResponse
RealVulkanBackend::create_command_pool(const vkrpc::CreateCommandPoolRequest& req) {
    vkrpc::CreateCommandPoolResponse resp;
    const auto it = devices_.find(req.device);
    if (it == devices_.end()) {
        resp.ok = false;
        resp.reason = "unknown device handle";
        return resp;
    }
    // The pool must target the queue family this device created queues for: it is
    // the only family with a usable queue, and vkCreateCommandPool requires a valid
    // family index. (from_body rejects out-of-range wire values to a -1 sentinel.)
    if (req.queue_family_index < 0 ||
        static_cast<std::uint32_t>(req.queue_family_index) != it->second.queue_family) {
        resp.ok = false;
        resp.reason = "queue family was not created on this device "
                      "(use the family reported by create_device)";
        return resp;
    }
    VkCommandPoolCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = it->second.queue_family;
    VkCommandPool vk = VK_NULL_HANDLE;
    if (vkCreateCommandPool(it->second.vk, &pci, nullptr, &vk) != VK_SUCCESS ||
        vk == VK_NULL_HANDLE) {
        resp.ok = false;
        resp.reason = "vkCreateCommandPool failed";
        return resp;
    }
    const std::uint64_t handle = next_handle_++;
    RealPool pool;
    pool.vk = vk;
    pool.device = req.device;
    pools_.emplace(handle, std::move(pool));
    it->second.pools.insert(handle);
    resp.ok = true;
    resp.reason = "ok";
    resp.command_pool = handle;
    return resp;
}

vkrpc::StatusResponse RealVulkanBackend::destroy_command_pool(const vkrpc::HandleRequest& req) {
    vkrpc::StatusResponse resp;
    const auto it = pools_.find(req.handle);
    if (it == pools_.end()) {
        resp.ok = false;
        resp.reason = "unknown command pool handle";
        return resp;
    }
    // vkDestroyCommandPool frees every command buffer allocated from it (Vulkan
    // semantics), so this never blocks on live buffers -- matching the mock. Drop the
    // freed buffers' metadata so a stale handle can't reach the driver later.
    for (const auto& b : it->second.buffers) {
        command_buffers_.erase(b.first);
    }
    const auto dev = devices_.find(it->second.device);
    if (dev != devices_.end()) {
        vkDestroyCommandPool(dev->second.vk, it->second.vk, nullptr);
        dev->second.pools.erase(req.handle);
    }
    pools_.erase(it);
    resp.ok = true;
    resp.reason = "ok";
    return resp;
}

vkrpc::AllocateCommandBuffersResponse
RealVulkanBackend::allocate_command_buffers(const vkrpc::AllocateCommandBuffersRequest& req) {
    vkrpc::AllocateCommandBuffersResponse resp;
    const auto it = pools_.find(req.command_pool);
    if (it == pools_.end()) {
        resp.ok = false;
        resp.reason = "unknown command pool handle";
        return resp;
    }
    constexpr long long kMaxBatch = 4096; // bound a hostile/buggy count
    if (req.count <= 0 || req.count > kMaxBatch) {
        resp.ok = false;
        resp.reason = "invalid command buffer count";
        return resp;
    }
    const auto dev = devices_.find(it->second.device);
    if (dev == devices_.end()) {
        resp.ok = false;
        resp.reason = "command pool has no live device";
        return resp;
    }
    const auto count = static_cast<std::uint32_t>(req.count);
    VkCommandBufferAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = it->second.vk;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = count;
    std::vector<VkCommandBuffer> bufs(count, VK_NULL_HANDLE);
    if (vkAllocateCommandBuffers(dev->second.vk, &ai, bufs.data()) != VK_SUCCESS) {
        resp.ok = false;
        resp.reason = "vkAllocateCommandBuffers failed";
        return resp;
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint64_t handle = next_handle_++;
        it->second.buffers.emplace(handle, bufs[i]);
        RealCmdBuffer cb;
        cb.vk = bufs[i];
        cb.device = it->second.device;
        cb.pool = req.command_pool;
        command_buffers_.emplace(handle, std::move(cb));
        resp.command_buffers.push_back(handle);
    }
    resp.ok = true;
    resp.reason = "ok";
    return resp;
}

vkrpc::StatusResponse
RealVulkanBackend::free_command_buffers(const vkrpc::FreeCommandBuffersRequest& req) {
    vkrpc::StatusResponse resp;
    const auto it = pools_.find(req.command_pool);
    if (it == pools_.end()) {
        resp.ok = false;
        resp.reason = "unknown command pool handle";
        return resp;
    }
    // An empty list is rejected (Vulkan requires commandBufferCount > 0; also closes
    // the malformed-decode-to-empty hole). Validate every handle belongs to this
    // pool and the batch has no duplicate (a dup would double-free), gathering the
    // VkCommandBuffers, before freeing any -- atomic, matching the mock.
    if (req.command_buffers.empty()) {
        resp.ok = false;
        resp.reason = "no command buffers specified";
        return resp;
    }
    std::set<std::uint64_t> seen;
    std::vector<VkCommandBuffer> to_free;
    to_free.reserve(req.command_buffers.size());
    for (const std::uint64_t b : req.command_buffers) {
        const auto buf = it->second.buffers.find(b);
        if (buf == it->second.buffers.end()) {
            resp.ok = false;
            resp.reason = "command buffer not allocated from this pool";
            return resp;
        }
        if (!seen.insert(b).second) {
            resp.ok = false;
            resp.reason = "duplicate command buffer in free request";
            return resp;
        }
        to_free.push_back(buf->second);
    }
    const auto dev = devices_.find(it->second.device);
    if (dev == devices_.end()) {
        resp.ok = false;
        resp.reason = "command pool has no live device";
        return resp;
    }
    vkFreeCommandBuffers(dev->second.vk, it->second.vk, static_cast<std::uint32_t>(to_free.size()),
                         to_free.data());
    for (const std::uint64_t b : req.command_buffers) {
        it->second.buffers.erase(b);
        command_buffers_.erase(b);
    }
    resp.ok = true;
    resp.reason = "ok";
    return resp;
}
vkrpc::StatusResponse RealVulkanBackend::destroy_leaf(std::uint64_t handle, LeafKind kind) {
    vkrpc::StatusResponse resp;
    const auto it = leaves_.find(handle);
    if (it == leaves_.end() || it->second.kind != kind) {
        // A wrong-kind handle (e.g. free_memory on a fence) is as invalid as an
        // unknown one -- the typed destroy must not free another object's handle.
        resp.ok = false;
        resp.reason = "unknown handle for this object type";
        return resp;
    }
    const auto dev = devices_.find(it->second.device);
    if (dev != devices_.end() && dev->second.vk != VK_NULL_HANDLE) {
        switch (kind) {
        case LeafKind::Fence:
            vkDestroyFence(dev->second.vk, it->second.fence, nullptr);
            break;
        case LeafKind::Semaphore:
            vkDestroySemaphore(dev->second.vk, it->second.semaphore, nullptr);
            break;
        case LeafKind::Memory:
            vkFreeMemory(dev->second.vk, it->second.memory, nullptr);
            break;
        case LeafKind::Event:
            vkDestroyEvent(dev->second.vk, it->second.event, nullptr);
            break;
        }
        dev->second.leaves.erase(handle);
    }
    leaves_.erase(it);
    resp.ok = true;
    resp.reason = "ok";
    return resp;
}

vkrpc::CreateFenceResponse RealVulkanBackend::create_fence(const vkrpc::CreateFenceRequest& req) {
    vkrpc::CreateFenceResponse resp;
    const auto it = devices_.find(req.device);
    if (it == devices_.end()) {
        resp.ok = false;
        resp.reason = "unknown device handle";
        return resp;
    }
    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    // Honor VK_FENCE_CREATE_SIGNALED_BIT so an app's first wait on a freshly-created frame fence
    // (created signaled) returns immediately instead of deadlocking.
    if (req.signaled) {
        fci.flags |= VK_FENCE_CREATE_SIGNALED_BIT;
    }
    VkFence vk = VK_NULL_HANDLE;
    if (vkCreateFence(it->second.vk, &fci, nullptr, &vk) != VK_SUCCESS || vk == VK_NULL_HANDLE) {
        resp.ok = false;
        resp.reason = "vkCreateFence failed";
        return resp;
    }
    const std::uint64_t handle = next_handle_++;
    RealLeaf leaf;
    leaf.kind = LeafKind::Fence;
    leaf.device = req.device;
    leaf.fence = vk;
    leaves_.emplace(handle, leaf);
    it->second.leaves.insert(handle);
    resp.ok = true;
    resp.reason = "ok";
    resp.fence = handle;
    return resp;
}

vkrpc::StatusResponse RealVulkanBackend::destroy_fence(const vkrpc::HandleRequest& req) {
    return destroy_leaf(req.handle, LeafKind::Fence);
}

vkrpc::CreateSemaphoreResponse
RealVulkanBackend::create_semaphore(const vkrpc::CreateSemaphoreRequest& req) {
    vkrpc::CreateSemaphoreResponse resp;
    const auto it = devices_.find(req.device);
    if (it == devices_.end()) {
        resp.ok = false;
        resp.reason = "unknown device handle";
        return resp;
    }
    VkSemaphoreCreateInfo sci{};
    sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    // (GL/zink): a timeline semaphore chains VkSemaphoreTypeCreateInfo with the initial
    // value (zink relies on these for batch sync).
    VkSemaphoreTypeCreateInfo type_ci{};
    const bool timeline = req.semaphore_type == 1;
    if (timeline) {
        type_ci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
        type_ci.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        type_ci.initialValue = req.initial_value;
        sci.pNext = &type_ci;
    }
    VkSemaphore vk = VK_NULL_HANDLE;
    if (vkCreateSemaphore(it->second.vk, &sci, nullptr, &vk) != VK_SUCCESS ||
        vk == VK_NULL_HANDLE) {
        resp.ok = false;
        resp.reason = "vkCreateSemaphore failed";
        return resp;
    }
    const std::uint64_t handle = next_handle_++;
    RealLeaf leaf;
    leaf.kind = LeafKind::Semaphore;
    leaf.device = req.device;
    leaf.semaphore = vk;
    leaf.is_timeline = timeline;
    leaves_.emplace(handle, leaf);
    it->second.leaves.insert(handle);
    resp.ok = true;
    resp.reason = "ok";
    resp.semaphore = handle;
    return resp;
}

vkrpc::StatusResponse RealVulkanBackend::destroy_semaphore(const vkrpc::HandleRequest& req) {
    return destroy_leaf(req.handle, LeafKind::Semaphore);
}

vkrpc::CreateEventResponse RealVulkanBackend::create_event(const vkrpc::CreateEventRequest& req) {
    // the VkEvent object, a device-child leaf beside fence/semaphore. Core 1.0 -- flags 0
    // (VK_EVENT_CREATE_DEVICE_ONLY_BIT is a 1.3/sync2 concept and is not supported here).
    vkrpc::CreateEventResponse resp;
    const auto it = devices_.find(req.device);
    if (it == devices_.end()) {
        resp.ok = false;
        resp.reason = "unknown device handle";
        return resp;
    }
    VkEventCreateInfo eci{};
    eci.sType = VK_STRUCTURE_TYPE_EVENT_CREATE_INFO;
    VkEvent vk = VK_NULL_HANDLE;
    if (vkCreateEvent(it->second.vk, &eci, nullptr, &vk) != VK_SUCCESS || vk == VK_NULL_HANDLE) {
        resp.ok = false;
        resp.reason = "vkCreateEvent failed";
        return resp;
    }
    const std::uint64_t handle = next_handle_++;
    RealLeaf leaf;
    leaf.kind = LeafKind::Event;
    leaf.device = req.device;
    leaf.event = vk;
    leaves_.emplace(handle, leaf);
    it->second.leaves.insert(handle);
    resp.ok = true;
    resp.reason = "ok";
    resp.event = handle;
    return resp;
}

vkrpc::StatusResponse RealVulkanBackend::destroy_event(const vkrpc::HandleRequest& req) {
    return destroy_leaf(req.handle, LeafKind::Event);
}

vkrpc::GetEventStatusResponse RealVulkanBackend::get_event_status(const vkrpc::HandleRequest& req) {
    // VK_EVENT_SET / VK_EVENT_RESET are the NORMAL non-VK_SUCCESS returns -- forward the
    // real VkResult verbatim (ok=true), the WaitForFences idiom. Only an unknown/wrong-kind handle
    // or a driver error faults.
    vkrpc::GetEventStatusResponse resp;
    const auto it = leaves_.find(req.handle);
    if (it == leaves_.end() || it->second.kind != LeafKind::Event) {
        resp.ok = false;
        resp.reason = "unknown handle for this object type";
        return resp;
    }
    const auto dev = devices_.find(it->second.device);
    if (dev == devices_.end() || dev->second.vk == VK_NULL_HANDLE) {
        resp.ok = false;
        resp.reason = "event's device is gone";
        return resp;
    }
    if (dev->second.lost) {
        resp.ok = true;
        resp.reason = "ok";
        resp.result = vkrpc::kVkErrorDeviceLost;
        return resp;
    }
    const VkResult r = observe_device_result(
        it->second.device, vkGetEventStatus(dev->second.vk, it->second.event), "vkGetEventStatus");
    if (r != VK_EVENT_SET && r != VK_EVENT_RESET && r != VK_ERROR_DEVICE_LOST) {
        resp.ok = false;
        resp.reason = "vkGetEventStatus failed";
        return resp;
    }
    resp.ok = true;
    resp.reason = "ok";
    resp.result = static_cast<int>(r);
    return resp;
}

vkrpc::StatusResponse RealVulkanBackend::set_event(const vkrpc::HandleRequest& req) {
    vkrpc::StatusResponse resp;
    const auto it = leaves_.find(req.handle);
    if (it == leaves_.end() || it->second.kind != LeafKind::Event) {
        resp.ok = false;
        resp.reason = "unknown handle for this object type";
        return resp;
    }
    const auto dev = devices_.find(it->second.device);
    if (dev == devices_.end() || dev->second.vk == VK_NULL_HANDLE) {
        resp.ok = false;
        resp.reason = "event's device is gone";
        return resp;
    }
    if (vkSetEvent(dev->second.vk, it->second.event) != VK_SUCCESS) {
        resp.ok = false;
        resp.reason = "vkSetEvent failed";
        return resp;
    }
    resp.ok = true;
    resp.reason = "ok";
    return resp;
}

vkrpc::StatusResponse RealVulkanBackend::reset_event(const vkrpc::HandleRequest& req) {
    vkrpc::StatusResponse resp;
    const auto it = leaves_.find(req.handle);
    if (it == leaves_.end() || it->second.kind != LeafKind::Event) {
        resp.ok = false;
        resp.reason = "unknown handle for this object type";
        return resp;
    }
    const auto dev = devices_.find(it->second.device);
    if (dev == devices_.end() || dev->second.vk == VK_NULL_HANDLE) {
        resp.ok = false;
        resp.reason = "event's device is gone";
        return resp;
    }
    if (vkResetEvent(dev->second.vk, it->second.event) != VK_SUCCESS) {
        resp.ok = false;
        resp.reason = "vkResetEvent failed";
        return resp;
    }
    resp.ok = true;
    resp.reason = "ok";
    return resp;
}

vkrpc::AllocateMemoryResponse
RealVulkanBackend::allocate_memory(const vkrpc::AllocateMemoryRequest& req) {
    vkrpc::AllocateMemoryResponse resp;
    const auto it = devices_.find(req.device);
    if (it == devices_.end()) {
        resp.ok = false;
        resp.reason = "unknown device handle";
        return resp;
    }
    if (req.allocation_size == 0) {
        resp.ok = false; // Vulkan requires allocationSize > 0 (also rejects a 0 decode)
        resp.reason = "invalid allocation size";
        return resp;
    }
    // memoryTypeIndex must be < the physical device's memoryTypeCount (a real
    // vkAllocateMemory requirement). The wide decode already rejected negatives /
    // out-of-uint32 to a sentinel; here we check against the actual count.
    const auto inst = instances_.find(it->second.instance);
    if (inst == instances_.end() || inst->second.physical_vk == VK_NULL_HANDLE) {
        resp.ok = false;
        resp.reason = "device has no physical device";
        return resp;
    }
    VkPhysicalDeviceMemoryProperties mem_props{};
    vkGetPhysicalDeviceMemoryProperties(inst->second.physical_vk, &mem_props);
    if (req.memory_type_index < 0 ||
        req.memory_type_index >= static_cast<long long>(mem_props.memoryTypeCount)) {
        resp.ok = false;
        resp.reason = "invalid memory type index";
        return resp;
    }
    // Memory-class reassertion (Option 1 Tier 1, vk13 audit): admit allocation of any type
    // the host driver can allocate EXCEPT protected (no protected queue/submit path) and
    // host-visible non-coherent (unmappable until Tier 2). A non-host-visible type (NVIDIA
    // propertyFlags==0, DEVICE_LOCAL-only) is admitted -- the GPU can use it; it is simply never
    // mapped. Mirrors the ICD's icd_subset::memory_class_ok verbatim, re-asserted independently so
    // a direct/hostile RPC gets the same fail-closed answer.
    {
        const VkMemoryPropertyFlags flags =
            mem_props.memoryTypes[req.memory_type_index].propertyFlags;
        if ((flags & VK_MEMORY_PROPERTY_PROTECTED_BIT) != 0) {
            resp.ok = false;
            resp.reason = "protected memory is not supported (no protected queue/submit path)";
            return resp;
        }
        if ((flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0 &&
            (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) == 0) {
            resp.ok = false;
            resp.reason = "host-visible memory without HOST_COHERENT is not yet supported (needs "
                          "Tier 2 flush/invalidate)";
            return resp;
        }
    }
    // (bufferDeviceAddress): the ONLY admitted allocate flag is DEVICE_ADDRESS, and only
    // when the device enabled the feature -- re-asserted at the worker independently of the ICD
    // (a direct/hostile RPC gets the same fail-closed answer), mock == real.
    if (req.allocate_flags != 0) {
        if ((req.allocate_flags & ~vkrpc::kMemoryAllocateDeviceAddressBit) != 0) {
            resp.ok = false;
            resp.reason = "memory allocate flags outside the supported set (only DEVICE_ADDRESS)";
            return resp;
        }
        if (!it->second.buffer_device_address_feature_enabled) {
            resp.ok = false;
            resp.reason = "DEVICE_ADDRESS allocate flag requires the enabled bufferDeviceAddress "
                          "feature";
            return resp;
        }
    }
    VkMemoryAllocateFlagsInfo mafi{};
    mafi.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
    mafi.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    if (req.allocate_flags != 0) {
        mai.pNext = &mafi; // the serialized flags-info, rebuilt (VUID 03339 needs it at bind)
    }
    mai.allocationSize = static_cast<VkDeviceSize>(req.allocation_size);
    mai.memoryTypeIndex = static_cast<std::uint32_t>(req.memory_type_index);
    VkDeviceMemory vk = VK_NULL_HANDLE;
    if (vkAllocateMemory(it->second.vk, &mai, nullptr, &vk) != VK_SUCCESS || vk == VK_NULL_HANDLE) {
        resp.ok = false;
        resp.reason = "vkAllocateMemory failed";
        return resp;
    }
    const std::uint64_t handle = next_handle_++;
    RealLeaf leaf;
    leaf.kind = LeafKind::Memory;
    leaf.device = req.device;
    leaf.memory = vk;
    // Record the size + the chosen type's property flags + index for the upload/bind
    // paths.
    leaf.memory_size = mai.allocationSize;
    leaf.memory_flags = mem_props.memoryTypes[mai.memoryTypeIndex].propertyFlags;
    leaf.memory_type_index = mai.memoryTypeIndex;
    leaf.memory_allocate_flags = req.allocate_flags;
    leaves_.emplace(handle, leaf);
    it->second.leaves.insert(handle);
    resp.ok = true;
    resp.reason = "ok";
    resp.memory = handle;
    return resp;
}

vkrpc::StatusResponse RealVulkanBackend::free_memory(const vkrpc::HandleRequest& req) {
    return destroy_leaf(req.handle, LeafKind::Memory);
}

// --- Host-visible memory + buffers ---------------------

vkrpc::GetPhysicalDeviceMemoryPropertiesResponse
RealVulkanBackend::get_physical_device_memory_properties(
    const vkrpc::GetPhysicalDeviceMemoryPropertiesRequest& req) {
    vkrpc::GetPhysicalDeviceMemoryPropertiesResponse resp;
    // The handle must be a physical device some instance enumerated (mirrors the surface-query
    // paths).
    const RealInstance* owner = nullptr;
    for (const auto& inst : instances_) {
        if (inst.second.physical == req.physical_device && req.physical_device != 0) {
            owner = &inst.second;
            break;
        }
    }
    if (owner == nullptr || owner->physical_vk == VK_NULL_HANDLE) {
        resp.ok = false;
        resp.reason = "unknown physical device handle";
        return resp;
    }
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(owner->physical_vk, &mp);
    for (std::uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        resp.types.push_back(
            vkrpc::MemoryType{static_cast<std::uint64_t>(mp.memoryTypes[i].propertyFlags),
                              static_cast<std::uint64_t>(mp.memoryTypes[i].heapIndex)});
    }
    for (std::uint32_t i = 0; i < mp.memoryHeapCount; ++i) {
        resp.heaps.push_back(
            vkrpc::MemoryHeap{static_cast<std::uint64_t>(mp.memoryHeaps[i].size),
                              static_cast<std::uint64_t>(mp.memoryHeaps[i].flags)});
    }
    resp.ok = true;
    resp.reason = "ok";
    return resp;
}

vkrpc::CreateBufferResponse
RealVulkanBackend::create_buffer(const vkrpc::CreateBufferRequest& req) {
    vkrpc::CreateBufferResponse resp;
    const auto dev = devices_.find(req.device);
    if (dev == devices_.end() || dev->second.vk == VK_NULL_HANDLE) {
        resp.ok = false;
        resp.reason = "unknown device handle";
        return resp;
    }
    // Buffer subset (re-validated at the worker, not just the ICD): a nonzero subset of {VERTEX,
    // UNIFORM} usage, EXCLUSIVE, size > 0.
    if (req.size == 0) {
        resp.ok = false;
        resp.reason = "buffer size must be > 0";
        return resp;
    }
    if (req.usage == 0 || (req.usage & ~vkrpc::kBufferUsageSubset) != 0) {
        resp.ok = false;
        resp.reason = "buffer usage must be a nonzero subset of {VERTEX_BUFFER, UNIFORM_BUFFER}";
        return resp;
    }
    if (req.sharing_mode != static_cast<int>(VK_SHARING_MODE_EXCLUSIVE)) {
        resp.ok = false;
        resp.reason = "buffer sharing mode must be EXCLUSIVE";
        return resp;
    }
    VkDeviceSize host_size = 0;
    if (!host_buffer_size(req.size, req.usage, host_size)) {
        resp.ok = false;
        resp.reason = "vertex-buffer host tail guard overflows VkDeviceSize";
        return resp;
    }
    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = host_size;
    bci.usage = static_cast<VkBufferUsageFlags>(req.usage);
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer vk = VK_NULL_HANDLE;
    if (vkCreateBuffer(dev->second.vk, &bci, nullptr, &vk) != VK_SUCCESS || vk == VK_NULL_HANDLE) {
        resp.ok = false;
        resp.reason = "vkCreateBuffer failed";
        return resp;
    }
    VkMemoryRequirements mr{};
    vkGetBufferMemoryRequirements(dev->second.vk, vk, &mr);
    const std::uint64_t h = next_handle_++;
    RealBuffer buf;
    buf.vk = vk;
    buf.device = req.device;
    buf.size = static_cast<VkDeviceSize>(req.size); // logical guest size; host tail is not visible
    buf.alignment = mr.alignment;
    buf.memory_type_bits = mr.memoryTypeBits;
    buf.usage = req.usage;
    buffers_.emplace(h, buf);
    dev->second.buffers.insert(h);
    resp.ok = true;
    resp.reason = "ok";
    resp.buffer = h;
    resp.mem_size = static_cast<std::uint64_t>(mr.size);
    resp.mem_alignment = static_cast<std::uint64_t>(mr.alignment);
    resp.mem_type_bits = static_cast<std::uint64_t>(mr.memoryTypeBits);
    return resp;
}

vkrpc::StatusResponse RealVulkanBackend::destroy_buffer(const vkrpc::HandleRequest& req) {
    vkrpc::StatusResponse resp;
    const auto it = buffers_.find(req.handle);
    if (it == buffers_.end()) {
        resp.ok = false;
        resp.reason = "unknown buffer handle";
        return resp;
    }
    invalidate_cbs_referencing(req.handle); // a recorded bind_vertex_buffers/copy baked this handle
    // (CB -> set -> buffer), via the shared helper extended to also
    // cover sampler/image-view referents: any descriptor set that CURRENTLY references this buffer
    // has its slot marked dangling, and any recorded CB that baked that set is invalidated.
    dangle_sets_referencing(req.handle);
    const auto dev = devices_.find(it->second.device);
    if (dev != devices_.end() && dev->second.vk != VK_NULL_HANDLE) {
        vkDestroyBuffer(dev->second.vk, it->second.vk, nullptr);
        dev->second.buffers.erase(req.handle);
    }
    buffers_.erase(it);
    resp.ok = true;
    resp.reason = "ok";
    return resp;
}

vkrpc::StatusResponse
RealVulkanBackend::bind_buffer_memory(const vkrpc::BindBufferMemoryRequest& req) {
    vkrpc::StatusResponse resp;
    const auto buf = buffers_.find(req.buffer);
    if (buf == buffers_.end()) {
        resp.ok = false;
        resp.reason = "unknown buffer handle";
        return resp;
    }
    const auto mem = leaves_.find(req.memory);
    if (mem == leaves_.end() || mem->second.kind != LeafKind::Memory) {
        resp.ok = false;
        resp.reason = "unknown memory handle";
        return resp;
    }
    if (buf->second.device != mem->second.device) {
        resp.ok = false;
        resp.reason = "buffer and memory are on different devices";
        return resp;
    }
    // Mirror the mock's fail-closed checks: these are Vulkan valid-usage
    // requirements, NOT VkResult error paths -- passing a bad request to vkBindBufferMemory is UB /
    // a validation VUID. Reject before the driver call.
    if (buf->second.bound_memory != 0) {
        resp.ok = false;
        resp.reason = "buffer is already bound to memory";
        return resp;
    }
    if (buf->second.memory_type_bits != 0 &&
        (buf->second.memory_type_bits & (1ull << mem->second.memory_type_index)) == 0) {
        resp.ok = false;
        resp.reason = "memory type is not in the buffer's supported memoryTypeBits";
        return resp;
    }
    if (buf->second.alignment != 0 &&
        (static_cast<VkDeviceSize>(req.memory_offset) % buf->second.alignment) != 0) {
        resp.ok = false;
        resp.reason = "bind offset does not satisfy the buffer's alignment";
        return resp;
    }
    if (req.memory_offset > static_cast<std::uint64_t>(mem->second.memory_size) ||
        req.memory_offset > ~0ull - static_cast<std::uint64_t>(buf->second.size) ||
        req.memory_offset + static_cast<std::uint64_t>(buf->second.size) >
            static_cast<std::uint64_t>(mem->second.memory_size)) {
        resp.ok = false;
        resp.reason = "bind range does not fit within the allocation";
        return resp;
    }
    // (bufferDeviceAddress): VUID-vkBindBufferMemory-bufferDeviceAddress-03339 -- a
    // SHADER_DEVICE_ADDRESS buffer may only bind to DEVICE_ADDRESS-allocated memory. A valid-usage
    // requirement (UB at the driver), so reject before the call, mock == real.
    if ((buf->second.usage & vkrpc::kBufferUsageShaderDeviceAddress) != 0 &&
        (mem->second.memory_allocate_flags & vkrpc::kMemoryAllocateDeviceAddressBit) == 0) {
        resp.ok = false;
        resp.reason = "SHADER_DEVICE_ADDRESS buffer requires DEVICE_ADDRESS-allocated memory "
                      "(VUID 03339)";
        return resp;
    }
    const auto dev = devices_.find(buf->second.device);
    if (dev == devices_.end() || dev->second.vk == VK_NULL_HANDLE) {
        resp.ok = false;
        resp.reason = "buffer has no live device";
        return resp;
    }
    if (vkBindBufferMemory(dev->second.vk, buf->second.vk, mem->second.memory,
                           static_cast<VkDeviceSize>(req.memory_offset)) != VK_SUCCESS) {
        resp.ok = false;
        resp.reason = "vkBindBufferMemory failed";
        return resp;
    }
    buf->second.bound_memory = req.memory;
    buf->second.bound_offset = static_cast<VkDeviceSize>(req.memory_offset);
    resp.ok = true;
    resp.reason = "ok";
    return resp;
}

vkrpc::GetBufferDeviceAddressResponse
RealVulkanBackend::get_buffer_device_address(const vkrpc::GetBufferDeviceAddressRequest& req) {
    // (bufferDeviceAddress): the REAL host VkDeviceAddress, verbatim -- the guest's
    // shaders execute on this device, so the address it minted is exactly what they dereference;
    // any translation would break it. Every invalid use is fail-closed BEFORE the driver call
    // (querying an unbound / usage-less buffer is a VUID, and a bad address here becomes a live
    // wild GPU pointer -- the TDR blast radius the design doc names).
    vkrpc::GetBufferDeviceAddressResponse resp;
    const auto dev = devices_.find(req.device);
    if (dev == devices_.end() || dev->second.vk == VK_NULL_HANDLE) {
        resp.ok = false;
        resp.reason = "unknown device handle";
        return resp;
    }
    if (!dev->second.buffer_device_address_feature_enabled ||
        dev->second.pfn_get_buffer_device_address == nullptr) {
        resp.ok = false;
        resp.reason = "get_buffer_device_address requires the enabled bufferDeviceAddress feature";
        return resp;
    }
    const auto buf = buffers_.find(req.buffer);
    if (buf == buffers_.end() || buf->second.device != req.device ||
        buf->second.vk == VK_NULL_HANDLE) {
        resp.ok = false;
        resp.reason = "unknown buffer handle for this device";
        return resp;
    }
    if ((buf->second.usage & vkrpc::kBufferUsageShaderDeviceAddress) == 0) {
        resp.ok = false;
        resp.reason = "buffer was not created with SHADER_DEVICE_ADDRESS usage";
        return resp;
    }
    if (buf->second.bound_memory == 0) {
        resp.ok = false;
        resp.reason = "buffer is not bound to memory (bind before querying its address)";
        return resp;
    }
    VkBufferDeviceAddressInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    info.buffer = buf->second.vk;
    const VkDeviceAddress addr = dev->second.pfn_get_buffer_device_address(dev->second.vk, &info);
    if (addr == 0) {
        resp.ok = false;
        resp.reason = "host vkGetBufferDeviceAddress returned 0";
        return resp;
    }
    resp.ok = true;
    resp.reason = "ok";
    resp.device_address = static_cast<std::uint64_t>(addr);
    return resp;
}

// --- Textures + depth = literal vkcube  --------

vkrpc::GetPhysicalDeviceFormatPropertiesResponse
RealVulkanBackend::get_physical_device_format_properties(
    const vkrpc::GetPhysicalDeviceFormatPropertiesRequest& req) {
    vkrpc::GetPhysicalDeviceFormatPropertiesResponse resp;
    const RealInstance* owner = nullptr;
    for (const auto& inst : instances_) {
        if (inst.second.physical == req.physical_device && req.physical_device != 0) {
            owner = &inst.second;
            break;
        }
    }
    if (owner == nullptr || owner->physical_vk == VK_NULL_HANDLE) {
        resp.ok = false;
        resp.reason = "unknown physical device handle";
        return resp;
    }
    // The honest host table -- the app (e.g. vkcube) selects its texture-upload path from these.
    const VkFormat fmt = static_cast<VkFormat>(req.format);
    VkFormatProperties fp{};
    vkGetPhysicalDeviceFormatProperties(owner->physical_vk, fmt, &fp);
    resp.ok = true;
    resp.reason = "ok";
    resp.linear_tiling_features = static_cast<std::uint64_t>(fp.linearTilingFeatures);
    resp.optimal_tiling_features = static_cast<std::uint64_t>(fp.optimalTilingFeatures);
    resp.buffer_features = static_cast<std::uint64_t>(fp.bufferFeatures);
    // (GL/zink): the 64-bit VkFormatProperties3 (core 1.3 / VK_KHR_format_feature_flags2),
    // queried via FormatProperties2. zink probes VkFormatProperties3 on a 1.3 device; without it
    // Mesa builds ZERO GLX configs. Fall back to mirroring the 32-bit flags per field if the host
    // driver does not fill the 64-bit struct (older driver / feature_flags2 unsupported).
    VkFormatProperties3 fp3{};
    fp3.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3;
    VkFormatProperties2 fp2{};
    fp2.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2;
    fp2.pNext = &fp3;
    vkGetPhysicalDeviceFormatProperties2(owner->physical_vk, fmt, &fp2);
    resp.linear_tiling_features2 =
        fp3.linearTilingFeatures != 0 ? fp3.linearTilingFeatures : resp.linear_tiling_features;
    resp.optimal_tiling_features2 =
        fp3.optimalTilingFeatures != 0 ? fp3.optimalTilingFeatures : resp.optimal_tiling_features;
    resp.buffer_features2 = fp3.bufferFeatures != 0 ? fp3.bufferFeatures : resp.buffer_features;
    return resp;
}

vkrpc::GetPhysicalDeviceFeaturesResponse RealVulkanBackend::get_physical_device_features(
    const vkrpc::GetPhysicalDeviceFeaturesRequest& req) {
    vkrpc::GetPhysicalDeviceFeaturesResponse resp;
    const RealInstance* owner = nullptr;
    for (const auto& inst : instances_) {
        if (inst.second.physical == req.physical_device && req.physical_device != 0) {
            owner = &inst.second;
            break;
        }
    }
    if (owner == nullptr || owner->physical_vk == VK_NULL_HANDLE) {
        resp.ok = false;
        resp.reason = "unknown physical device handle";
        return resp;
    }
    // (GL/zink): the host's REAL VkPhysicalDeviceFeatures, packed to the wire bitmask. zink
    // reads these to set GL caps; the vkcube subset returned an all-zero set (unbuildable for
    // zink).
    VkPhysicalDeviceFeatures feats{};
    vkGetPhysicalDeviceFeatures(owner->physical_vk, &feats);
    resp.ok = true;
    resp.reason = "ok";
    resp.feature_bits = vkrpc::pack_physical_device_features(feats);
    return resp;
}

vkrpc::GetPhysicalDeviceImageFormatPropertiesResponse
RealVulkanBackend::get_physical_device_image_format_properties(
    const vkrpc::GetPhysicalDeviceImageFormatPropertiesRequest& req) {
    vkrpc::GetPhysicalDeviceImageFormatPropertiesResponse resp;
    const RealInstance* owner = nullptr;
    for (const auto& inst : instances_) {
        if (inst.second.physical == req.physical_device && req.physical_device != 0) {
            owner = &inst.second;
            break;
        }
    }
    if (owner == nullptr || owner->physical_vk == VK_NULL_HANDLE) {
        resp.ok = false;
        resp.reason = "unknown physical device handle";
        return resp;
    }
    // (GL/zink): forward the host's real vkGetPhysicalDeviceImageFormatProperties (zink
    // queries it per format/usage while building configs). `result` carries the real VkResult
    // (incl. VK_ERROR_FORMAT_NOT_SUPPORTED), the maxima are meaningful only on VK_SUCCESS.
    VkImageFormatProperties props{};
    const VkResult r = vkGetPhysicalDeviceImageFormatProperties(
        owner->physical_vk, static_cast<VkFormat>(req.format),
        static_cast<VkImageType>(req.image_type), static_cast<VkImageTiling>(req.tiling),
        static_cast<VkImageUsageFlags>(req.usage), static_cast<VkImageCreateFlags>(req.flags),
        &props);
    resp.ok = true;
    resp.reason = "ok";
    resp.result = static_cast<int>(r);
    if (r == VK_SUCCESS) {
        resp.max_extent_width = props.maxExtent.width;
        resp.max_extent_height = props.maxExtent.height;
        resp.max_extent_depth = props.maxExtent.depth;
        resp.max_mip_levels = props.maxMipLevels;
        resp.max_array_layers = props.maxArrayLayers;
        resp.sample_counts = static_cast<std::uint32_t>(props.sampleCounts);
        resp.max_resource_size = static_cast<std::uint64_t>(props.maxResourceSize);
    }
    return resp;
}

vkrpc::GetPhysicalDevicePropertiesResponse RealVulkanBackend::get_physical_device_properties(
    const vkrpc::GetPhysicalDevicePropertiesRequest& req) {
    vkrpc::GetPhysicalDevicePropertiesResponse resp;
    const RealInstance* owner = nullptr;
    for (const auto& inst : instances_) {
        if (inst.second.physical == req.physical_device && req.physical_device != 0) {
            owner = &inst.second;
            break;
        }
    }
    if (owner == nullptr || owner->physical_vk == VK_NULL_HANDLE) {
        resp.ok = false;
        resp.reason = "unknown physical device handle";
        return resp;
    }
    // (GL/zink): the FULL host VkPhysicalDeviceProperties (incl. the real limits) forwarded
    // verbatim. zink rejects a device whose limits are zero.
    VkPhysicalDeviceProperties p{};
    vkGetPhysicalDeviceProperties(owner->physical_vk, &p);
    resp.ok = true;
    resp.reason = "ok";
    resp.props_blob.assign(reinterpret_cast<const char*>(&p), sizeof(p));
    return resp;
}

vkrpc::GetPhysicalDeviceCapabilityChainResponse
RealVulkanBackend::get_physical_device_capability_chain(
    const vkrpc::GetPhysicalDeviceCapabilityChainRequest& req) {
    vkrpc::GetPhysicalDeviceCapabilityChainResponse resp;
    const RealInstance* owner = nullptr;
    for (const auto& inst : instances_) {
        if (inst.second.physical == req.physical_device && req.physical_device != 0) {
            owner = &inst.second;
            break;
        }
    }
    if (owner == nullptr || owner->physical_vk == VK_NULL_HANDLE) {
        resp.ok = false;
        resp.reason = "unknown physical device handle";
        return resp;
    }
    // (GL/zink): build the requested pNext structs (zeroed, sType set), chain them, and let
    // the host vkGetPhysicalDevice{Features,Properties}2 fill them; return each struct's real
    // bytes. The ICD copies the FIELDS (after the sType+pNext header) into the app's matching pNext
    // nodes.
    constexpr std::uint32_t kHeaderBytes =
        static_cast<std::uint32_t>(sizeof(VkBaseOutStructure)); // {sType, pNext} = 16 on 64-bit
    constexpr std::uint32_t kMaxEntryBytes = 4096;              // per-entry sanity cap
    std::vector<std::vector<unsigned char>> bufs;
    bufs.reserve(req.entries.size());
    for (const auto& e : req.entries) {
        if (e.size < kHeaderBytes || e.size > kMaxEntryBytes) {
            resp.ok = false;
            resp.reason = "capability chain entry size out of range";
            return resp;
        }
        std::vector<unsigned char> buf(e.size, 0);
        const VkStructureType st = static_cast<VkStructureType>(e.s_type);
        std::memcpy(buf.data(), &st, sizeof(st));
        bufs.push_back(std::move(buf));
    }
    // Chain: each buffer's pNext -> the next buffer (the last is null).
    for (std::size_t i = 0; i < bufs.size(); ++i) {
        void* next = (i + 1 < bufs.size()) ? static_cast<void*>(bufs[i + 1].data()) : nullptr;
        std::memcpy(bufs[i].data() + offsetof(VkBaseOutStructure, pNext), &next, sizeof(void*));
    }
    void* head = bufs.empty() ? nullptr : static_cast<void*>(bufs[0].data());
    if (req.which == 0) {
        VkPhysicalDeviceFeatures2 f2{};
        f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        f2.pNext = head;
        vkGetPhysicalDeviceFeatures2(owner->physical_vk, &f2);
    } else {
        VkPhysicalDeviceProperties2 p2{};
        p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        p2.pNext = head;
        vkGetPhysicalDeviceProperties2(owner->physical_vk, &p2);
    }
    resp.ok = true;
    resp.reason = "ok";
    for (std::size_t i = 0; i < bufs.size(); ++i) {
        vkrpc::CapabilityChainEntry out;
        out.s_type = req.entries[i].s_type;
        out.blob.assign(reinterpret_cast<const char*>(bufs[i].data()), bufs[i].size());
        resp.entries.push_back(std::move(out));
    }
    return resp;
}

vkrpc::CreateImageResponse RealVulkanBackend::create_image(const vkrpc::CreateImageRequest& req) {
    vkrpc::CreateImageResponse resp;
    const auto dev = devices_.find(req.device);
    if (dev == devices_.end() || dev->second.vk == VK_NULL_HANDLE) {
        resp.ok = false;
        resp.reason = "unknown device handle";
        return resp;
    }
    // (GL/zink): FAITHFUL image. The vkcube subset (D16|RGBA8, 2D, single mip/layer/sample,
    // usage allowlist) is replaced by forwarding the full create-info -- any format / type / extent
    // extent, mip, layer, sample, tiling, usage, and flags, plus the VkImageFormatListCreateInfo
    // view-format list (mutable-format aliasing). The host driver is the authoritative gate. The
    // aspect is derived from the format (depth / stencil / depth+stencil / colour) for the view +
    // copy paths.
    if (req.width <= 0 || req.height <= 0 || req.depth <= 0 || req.usage == 0) {
        resp.ok = false;
        resp.reason = "image extent must be positive and usage nonzero";
        return resp;
    }
    const int f = req.format;
    // Aspect from the shared format->aspect table (single source of truth; the returned bits equal
    // the VK_IMAGE_ASPECT_* values, and the mock keys on the same helper, so mock == real for the
    // view + copy paths). Depth/stencil formats (124-130) yield DEPTH / STENCIL / DEPTH|STENCIL.
    const VkImageAspectFlags aspect = static_cast<VkImageAspectFlags>(vkrpc::format_aspect_mask(f));
    VkImageCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.flags = static_cast<VkImageCreateFlags>(req.image_flags);
    ici.imageType = static_cast<VkImageType>(req.image_type);
    ici.format = static_cast<VkFormat>(req.format);
    ici.extent = {static_cast<std::uint32_t>(req.width), static_cast<std::uint32_t>(req.height),
                  static_cast<std::uint32_t>(req.depth)};
    ici.mipLevels = static_cast<std::uint32_t>(req.mip_levels > 0 ? req.mip_levels : 1);
    ici.arrayLayers = static_cast<std::uint32_t>(req.array_layers > 0 ? req.array_layers : 1);
    ici.samples = static_cast<VkSampleCountFlagBits>(req.samples > 0 ? req.samples : 1);
    ici.tiling = static_cast<VkImageTiling>(req.tiling);
    ici.usage = static_cast<VkImageUsageFlags>(req.usage);
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE; // zink is single-queue; family indices are ignored
    ici.initialLayout = static_cast<VkImageLayout>(req.initial_layout);
    // Rebuild the mutable-format view list pNext when carried.
    std::vector<VkFormat> view_formats;
    VkImageFormatListCreateInfo fmt_list{};
    if (!req.view_formats.empty()) {
        for (const int vf : req.view_formats) {
            view_formats.push_back(static_cast<VkFormat>(vf));
        }
        fmt_list.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO;
        fmt_list.viewFormatCount = static_cast<std::uint32_t>(view_formats.size());
        fmt_list.pViewFormats = view_formats.data();
        ici.pNext = &fmt_list;
    }
    VkImage vk = VK_NULL_HANDLE;
    const VkResult ir = vkCreateImage(dev->second.vk, &ici, nullptr, &vk);
    if (ir != VK_SUCCESS || vk == VK_NULL_HANDLE) {
        resp.ok = false;
        resp.reason = "vkCreateImage failed (VkResult " + std::to_string(ir) + ")";
        return resp;
    }
    VkMemoryRequirements mr{};
    vkGetImageMemoryRequirements(dev->second.vk, vk, &mr);
    const std::uint64_t h = next_handle_++;
    RealImage im;
    im.vk = vk;
    im.device = req.device;
    im.app_created = true;
    im.format = static_cast<VkFormat>(req.format);
    im.aspect = aspect;
    im.tiling = ici.tiling;
    im.usage = req.usage;
    // Clear support derives from the REQUESTED usage, exactly like the swapchain path -- an app
    // image created with TRANSFER_DST is a legal vkCmdClearColorImage target (the
    // faithful create_image had left this flag defaulted false, wrongly rejecting such clears).
    im.transfer_dst =
        (req.usage & static_cast<std::uint64_t>(VK_IMAGE_USAGE_TRANSFER_DST_BIT)) != 0;
    im.extent = {ici.extent.width, ici.extent.height};
    im.mip_levels = ici.mipLevels;
    im.array_layers = ici.arrayLayers;
    im.alignment = mr.alignment;
    im.memory_type_bits = mr.memoryTypeBits;
    images_.emplace(h, im);
    dev->second.images.insert(h);
    resp.ok = true;
    resp.reason = "ok";
    resp.image = h;
    resp.mem_size = static_cast<std::uint64_t>(mr.size);
    resp.mem_alignment = static_cast<std::uint64_t>(mr.alignment);
    resp.mem_type_bits = static_cast<std::uint64_t>(mr.memoryTypeBits);
    // The subresource layout is queryable only for LINEAR tiling (the linear texture path reads
    // rowPitch); for OPTIMAL it is invalid to call vkGetImageSubresourceLayout.
    if (ici.tiling == VK_IMAGE_TILING_LINEAR) {
        VkImageSubresource sub{};
        sub.aspectMask = aspect;
        sub.mipLevel = 0;
        sub.arrayLayer = 0;
        VkSubresourceLayout sl{};
        vkGetImageSubresourceLayout(dev->second.vk, vk, &sub, &sl);
        resp.has_subresource_layout = true;
        resp.sr_offset = static_cast<std::uint64_t>(sl.offset);
        resp.sr_size = static_cast<std::uint64_t>(sl.size);
        resp.sr_row_pitch = static_cast<std::uint64_t>(sl.rowPitch);
    }
    return resp;
}

// Vulkan 1.3 support (maintenance4): memory requirements for a CREATE-INFO, no object created.
// The host's real answer via the core vkGetDeviceBufferMemoryRequirements /
// vkGetDeviceImageMemoryRequirements (PFNs resolved fail-closed at create_device iff the
// maintenance4 feature was enabled). The create-info rebuild mirrors create_buffer/create_image
// exactly, so "query then create" is guaranteed self-consistent.
vkrpc::CreateBufferResponse
RealVulkanBackend::get_device_buffer_memory_requirements(const vkrpc::CreateBufferRequest& req) {
    vkrpc::CreateBufferResponse resp;
    const auto dev = devices_.find(req.device);
    if (dev == devices_.end() || dev->second.vk == VK_NULL_HANDLE) {
        resp.ok = false;
        resp.reason = "unknown device handle";
        return resp;
    }
    if (dev->second.pfn_get_device_buffer_memory_requirements == nullptr) {
        resp.ok = false;
        resp.reason = "maintenance4 not enabled on this device";
        return resp;
    }
    if (req.size == 0 || req.usage == 0) {
        resp.ok = false;
        resp.reason = "buffer size and usage must be nonzero";
        return resp;
    }
    VkDeviceSize host_size = 0;
    if (!host_buffer_size(req.size, req.usage, host_size)) {
        resp.ok = false;
        resp.reason = "vertex-buffer host tail guard overflows VkDeviceSize";
        return resp;
    }
    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = host_size;
    bci.usage = static_cast<VkBufferUsageFlags>(req.usage);
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkDeviceBufferMemoryRequirements dbr{};
    dbr.sType = VK_STRUCTURE_TYPE_DEVICE_BUFFER_MEMORY_REQUIREMENTS;
    dbr.pCreateInfo = &bci;
    VkMemoryRequirements2 mr2{};
    mr2.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2;
    dev->second.pfn_get_device_buffer_memory_requirements(dev->second.vk, &dbr, &mr2);
    resp.ok = true;
    resp.reason = "ok";
    resp.mem_size = static_cast<std::uint64_t>(mr2.memoryRequirements.size);
    resp.mem_alignment = static_cast<std::uint64_t>(mr2.memoryRequirements.alignment);
    resp.mem_type_bits = static_cast<std::uint64_t>(mr2.memoryRequirements.memoryTypeBits);
    return resp;
}

vkrpc::CreateImageResponse
RealVulkanBackend::get_device_image_memory_requirements(const vkrpc::CreateImageRequest& req) {
    vkrpc::CreateImageResponse resp;
    const auto dev = devices_.find(req.device);
    if (dev == devices_.end() || dev->second.vk == VK_NULL_HANDLE) {
        resp.ok = false;
        resp.reason = "unknown device handle";
        return resp;
    }
    if (dev->second.pfn_get_device_image_memory_requirements == nullptr) {
        resp.ok = false;
        resp.reason = "maintenance4 not enabled on this device";
        return resp;
    }
    if (req.width <= 0 || req.height <= 0 || req.depth <= 0 || req.usage == 0) {
        resp.ok = false;
        resp.reason = "image extent must be positive and usage nonzero";
        return resp;
    }
    VkImageCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.flags = static_cast<VkImageCreateFlags>(req.image_flags);
    ici.imageType = static_cast<VkImageType>(req.image_type);
    ici.format = static_cast<VkFormat>(req.format);
    ici.extent = {static_cast<std::uint32_t>(req.width), static_cast<std::uint32_t>(req.height),
                  static_cast<std::uint32_t>(req.depth)};
    ici.mipLevels = static_cast<std::uint32_t>(req.mip_levels > 0 ? req.mip_levels : 1);
    ici.arrayLayers = static_cast<std::uint32_t>(req.array_layers > 0 ? req.array_layers : 1);
    ici.samples = static_cast<VkSampleCountFlagBits>(req.samples > 0 ? req.samples : 1);
    ici.tiling = static_cast<VkImageTiling>(req.tiling);
    ici.usage = static_cast<VkImageUsageFlags>(req.usage);
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = static_cast<VkImageLayout>(req.initial_layout);
    std::vector<VkFormat> view_formats;
    VkImageFormatListCreateInfo fmt_list{};
    if (!req.view_formats.empty()) {
        for (const int vf : req.view_formats) {
            view_formats.push_back(static_cast<VkFormat>(vf));
        }
        fmt_list.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO;
        fmt_list.viewFormatCount = static_cast<std::uint32_t>(view_formats.size());
        fmt_list.pViewFormats = view_formats.data();
        ici.pNext = &fmt_list;
    }
    VkDeviceImageMemoryRequirements dir{};
    dir.sType = VK_STRUCTURE_TYPE_DEVICE_IMAGE_MEMORY_REQUIREMENTS;
    dir.pCreateInfo = &ici;
    VkMemoryRequirements2 mr2{};
    mr2.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2;
    dev->second.pfn_get_device_image_memory_requirements(dev->second.vk, &dir, &mr2);
    resp.ok = true;
    resp.reason = "ok";
    resp.mem_size = static_cast<std::uint64_t>(mr2.memoryRequirements.size);
    resp.mem_alignment = static_cast<std::uint64_t>(mr2.memoryRequirements.alignment);
    resp.mem_type_bits = static_cast<std::uint64_t>(mr2.memoryRequirements.memoryTypeBits);
    return resp;
}

vkrpc::StatusResponse RealVulkanBackend::destroy_image(const vkrpc::HandleRequest& req) {
    vkrpc::StatusResponse resp;
    const auto it = images_.find(req.handle);
    if (it == images_.end() || !it->second.app_created) {
        resp.ok = false;
        resp.reason = "unknown image handle (must be an app-created image)";
        return resp;
    }
    // Blocked while a live view references it -- the swapchain -> view rule,
    // applied to app images; destroy the views first (a host vkDestroyImage with a live VkImageView
    // is UB).
    if (!it->second.image_views.empty()) {
        resp.ok = false;
        resp.reason = "image has live image views; destroy them first";
        return resp;
    }
    invalidate_cbs_referencing(req.handle); // a recorded copy/barrier may have baked this handle
    const auto dev = devices_.find(it->second.device);
    if (dev != devices_.end() && dev->second.vk != VK_NULL_HANDLE) {
        vkDestroyImage(dev->second.vk, it->second.vk, nullptr);
        dev->second.images.erase(req.handle);
    }
    images_.erase(it);
    resp.ok = true;
    resp.reason = "ok";
    return resp;
}

vkrpc::StatusResponse
RealVulkanBackend::bind_image_memory(const vkrpc::BindImageMemoryRequest& req) {
    vkrpc::StatusResponse resp;
    const auto img = images_.find(req.image);
    if (img == images_.end() || !img->second.app_created) {
        resp.ok = false;
        resp.reason = "unknown image handle (must be an app-created image)";
        return resp;
    }
    const auto mem = leaves_.find(req.memory);
    if (mem == leaves_.end() || mem->second.kind != LeafKind::Memory) {
        resp.ok = false;
        resp.reason = "unknown memory handle";
        return resp;
    }
    if (img->second.device != mem->second.device) {
        resp.ok = false;
        resp.reason = "image and memory are on different devices";
        return resp;
    }
    // Mirror the mock's fail-closed checks (these are Vulkan valid-usage requirements, not VkResult
    // error paths): not already bound, the memory type in the image's memoryTypeBits, the offset
    // aligned.
    if (img->second.bound_memory != 0) {
        resp.ok = false;
        resp.reason = "image is already bound to memory";
        return resp;
    }
    if (img->second.memory_type_bits != 0 &&
        (img->second.memory_type_bits & (1ull << mem->second.memory_type_index)) == 0) {
        resp.ok = false;
        resp.reason = "memory type is not in the image's supported memoryTypeBits";
        return resp;
    }
    if (img->second.alignment != 0 &&
        (static_cast<VkDeviceSize>(req.memory_offset) % img->second.alignment) != 0) {
        resp.ok = false;
        resp.reason = "bind offset does not satisfy the image's alignment";
        return resp;
    }
    const auto dev = devices_.find(img->second.device);
    if (dev == devices_.end() || dev->second.vk == VK_NULL_HANDLE) {
        resp.ok = false;
        resp.reason = "image has no live device";
        return resp;
    }
    if (vkBindImageMemory(dev->second.vk, img->second.vk, mem->second.memory,
                          static_cast<VkDeviceSize>(req.memory_offset)) != VK_SUCCESS) {
        resp.ok = false;
        resp.reason = "vkBindImageMemory failed";
        return resp;
    }
    img->second.bound_memory = req.memory;
    resp.ok = true;
    resp.reason = "ok";
    return resp;
}

vkrpc::CreateSamplerResponse
RealVulkanBackend::create_sampler(const vkrpc::CreateSamplerRequest& req) {
    vkrpc::CreateSamplerResponse resp;
    const auto dev = devices_.find(req.device);
    if (dev == devices_.end() || dev->second.vk == VK_NULL_HANDLE) {
        resp.reason = "unknown device handle";
        return resp;
    }
    // (GL/zink): build the sampler faithfully from the forwarded VkSamplerCreateInfo
    // fields. The host's real vkCreateSampler is the validator (it rejects e.g. anisotropy without
    // the feature). The vkcube-subset NEAREST/CLAMP-only re-validation is lifted.
    VkSamplerCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    ci.magFilter = static_cast<VkFilter>(req.mag_filter);
    ci.minFilter = static_cast<VkFilter>(req.min_filter);
    ci.mipmapMode = static_cast<VkSamplerMipmapMode>(req.mipmap_mode);
    ci.addressModeU = static_cast<VkSamplerAddressMode>(req.address_mode_u);
    ci.addressModeV = static_cast<VkSamplerAddressMode>(req.address_mode_v);
    ci.addressModeW = static_cast<VkSamplerAddressMode>(req.address_mode_w);
    ci.mipLodBias = static_cast<float>(req.mip_lod_bias);
    ci.anisotropyEnable = req.anisotropy_enable != 0 ? VK_TRUE : VK_FALSE;
    ci.maxAnisotropy = static_cast<float>(req.max_anisotropy);
    ci.compareEnable = req.compare_enable != 0 ? VK_TRUE : VK_FALSE;
    ci.compareOp = static_cast<VkCompareOp>(req.compare_op);
    ci.minLod = static_cast<float>(req.min_lod);
    ci.maxLod = static_cast<float>(req.max_lod);
    ci.borderColor = static_cast<VkBorderColor>(req.border_color);
    ci.unnormalizedCoordinates = req.unnormalized_coordinates != 0 ? VK_TRUE : VK_FALSE;
    VkSampler vk = VK_NULL_HANDLE;
    if (vkCreateSampler(dev->second.vk, &ci, nullptr, &vk) != VK_SUCCESS || vk == VK_NULL_HANDLE) {
        resp.reason = "vkCreateSampler failed";
        return resp;
    }
    const std::uint64_t h = next_handle_++;
    samplers_.emplace(h, RealSampler{vk, req.device});
    dev->second.samplers.insert(h);
    resp.ok = true;
    resp.reason = "ok";
    resp.sampler = h;
    return resp;
}

vkrpc::StatusResponse RealVulkanBackend::destroy_sampler(const vkrpc::HandleRequest& req) {
    vkrpc::StatusResponse resp;
    const auto it = samplers_.find(req.handle);
    if (it == samplers_.end()) {
        resp.ok = false;
        resp.reason = "unknown sampler handle";
        return resp;
    }
    // CB -> set -> sampler destroy consult: a combined-image-sampler slot
    // currently referencing this sampler is marked dangling + its CBs invalidated.
    dangle_sets_referencing(req.handle);
    const auto dev = devices_.find(it->second.device);
    if (dev != devices_.end() && dev->second.vk != VK_NULL_HANDLE) {
        vkDestroySampler(dev->second.vk, it->second.vk, nullptr);
        dev->second.samplers.erase(req.handle);
    }
    samplers_.erase(it);
    resp.ok = true;
    resp.reason = "ok";
    return resp;
}

// Query pools (GL 3.3 / occlusion / xfb queries). Faithful VkQueryPoolCreateInfo; the host driver
// is the authoritative gate on the type/count. Device child (blocks device destroy).
vkrpc::CreateQueryPoolResponse
RealVulkanBackend::create_query_pool(const vkrpc::CreateQueryPoolRequest& req) {
    vkrpc::CreateQueryPoolResponse resp;
    const auto dev = devices_.find(req.device);
    if (dev == devices_.end() || dev->second.vk == VK_NULL_HANDLE) {
        resp.reason = "unknown device handle";
        return resp;
    }
    // OCCLUSION / TIMESTAMP / PIPELINE_STATISTICS drive through the wired core commands
    // (vkCmdBeginQuery / EndQuery / WriteTimestamp). TRANSFORM_FEEDBACK_STREAM is DELIBERATELY not
    // accepted (query-pools): its only valid begin is vkCmdBeginQueryIndexedEXT,
    // which is not wired -- so the whole XFB-stream query path fails CLOSED at pool creation rather
    // than half-succeeding then aborting on the indexed begin. The indexed pair remains separate
    // future work and must be GL-verified before re-admitting the type.
    if (req.query_type != VK_QUERY_TYPE_OCCLUSION &&
        req.query_type != VK_QUERY_TYPE_PIPELINE_STATISTICS &&
        req.query_type != VK_QUERY_TYPE_TIMESTAMP) {
        resp.reason = "unsupported query type";
        return resp;
    }
    if (req.query_count == 0) {
        resp.reason = "query pool needs a non-zero query count";
        return resp;
    }
    VkQueryPoolCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    ci.queryType = static_cast<VkQueryType>(req.query_type);
    ci.queryCount = req.query_count;
    ci.pipelineStatistics = static_cast<VkQueryPipelineStatisticFlags>(req.pipeline_statistics);
    VkQueryPool pool = VK_NULL_HANDLE;
    const VkResult vr = vkCreateQueryPool(dev->second.vk, &ci, nullptr, &pool);
    if (vr != VK_SUCCESS || pool == VK_NULL_HANDLE) {
        resp.reason = "vkCreateQueryPool failed (VkResult " + std::to_string(vr) + ")";
        return resp;
    }
    const std::uint64_t h = next_handle_++;
    RealQueryPool qp;
    qp.vk = pool;
    qp.device = req.device;
    qp.type = static_cast<VkQueryType>(req.query_type);
    qp.count = req.query_count;
    query_pools_.emplace(h, qp);
    dev->second.query_pools.insert(h);
    resp.ok = true;
    resp.reason = "ok";
    resp.query_pool = h;
    return resp;
}

vkrpc::StatusResponse RealVulkanBackend::destroy_query_pool(const vkrpc::HandleRequest& req) {
    vkrpc::StatusResponse resp;
    const auto it = query_pools_.find(req.handle);
    if (it == query_pools_.end()) {
        resp.ok = false;
        resp.reason = "unknown query pool handle";
        return resp;
    }
    invalidate_cbs_referencing(req.handle); // a recorded query command baked this handle
    const auto dev = devices_.find(it->second.device);
    if (dev != devices_.end() && dev->second.vk != VK_NULL_HANDLE) {
        vkDestroyQueryPool(dev->second.vk, it->second.vk, nullptr);
        dev->second.query_pools.erase(req.handle);
    }
    query_pools_.erase(it);
    resp.ok = true;
    resp.reason = "ok";
    return resp;
}

// Synchronous query-results readback: call the real vkGetQueryPoolResults and ship the bytes + the
// real VkResult (VK_NOT_READY without WAIT is faithful). ok=false only for a relay/handle error.
vkrpc::GetQueryPoolResultsResponse
RealVulkanBackend::get_query_pool_results(const vkrpc::GetQueryPoolResultsRequest& req) {
    vkrpc::GetQueryPoolResultsResponse resp;
    const auto it = query_pools_.find(req.query_pool);
    if (it == query_pools_.end() || it->second.device != req.device) {
        resp.reason = "unknown query pool handle on the device";
        return resp;
    }
    // Overflow-safe range check (query-pools): first_query + query_count is uint32
    // and wraps (firstQuery=UINT32_MAX, queryCount=1 -> 0), which is REACHABLE through the public
    // vkGetQueryPoolResults, not just malformed JSON. Compare by subtraction after bounding first.
    if (req.first_query > it->second.count ||
        req.query_count > it->second.count - req.first_query) {
        resp.reason = "query range out of bounds";
        return resp;
    }
    const auto dev = devices_.find(it->second.device);
    if (dev == devices_.end() || dev->second.vk == VK_NULL_HANDLE) {
        resp.reason = "query pool has no live device";
        return resp;
    }
    if (dev->second.lost) {
        resp.ok = true;
        resp.reason = "ok";
        resp.vk_result = vkrpc::kVkErrorDeviceLost;
        return resp;
    }
    // Cap the app-requested size defensively (a query pool's results are tiny).
    constexpr std::uint64_t kMaxResultBytes = 1u << 20; // 1 MiB
    const std::size_t data_size =
        static_cast<std::size_t>(req.data_size < kMaxResultBytes ? req.data_size : kMaxResultBytes);
    std::string data(data_size, '\0');
    const VkResult vr = observe_device_result(
        it->second.device,
        vkGetQueryPoolResults(dev->second.vk, it->second.vk, req.first_query, req.query_count,
                              data_size, data_size == 0 ? nullptr : data.data(),
                              static_cast<VkDeviceSize>(req.stride),
                              static_cast<VkQueryResultFlags>(req.flags)),
        "vkGetQueryPoolResults");
    // SUCCESS or NOT_READY are both faithful answers; other results still ride back as the
    // VkResult.
    resp.ok = true;
    resp.reason = "ok";
    resp.vk_result = static_cast<int>(vr);
    resp.data = std::move(data);
    return resp;
}

// hostQueryReset: the DEVICE-level vkResetQueryPool -- reset a query range FROM THE HOST
// (no command buffer). The API returns void, so the response is just relay/handle status; the range
// validation is mock == real (an out-of-range reset fails closed rather than reaching the driver).
// The hostQueryReset feature must be enabled on the host device (the ICD forwards its f12 enable),
// which the host's own vkResetQueryPool validates.
vkrpc::StatusResponse RealVulkanBackend::reset_query_pool(const vkrpc::ResetQueryPoolRequest& req) {
    vkrpc::StatusResponse resp;
    // Fail closed: never call the host vkResetQueryPool on a device that did not
    // enable the hostQueryReset feature (Vulkan valid-usage violation / possible driver crash). The
    // ICD already no-ops before the wire; this is the worker-side safety net for a stale/hostile
    // frame (mock == real).
    const auto dev0 = devices_.find(req.device);
    if (dev0 == devices_.end() || dev0->second.vk == VK_NULL_HANDLE) {
        resp.reason = "unknown device handle";
        return resp;
    }
    if (!dev0->second.host_query_reset_feature_enabled) {
        resp.reason = "reset_query_pool without the enabled hostQueryReset feature";
        return resp;
    }
    const auto it = query_pools_.find(req.query_pool);
    if (it == query_pools_.end() || it->second.device != req.device) {
        resp.reason = "unknown query pool handle on the device";
        return resp;
    }
    // Overflow-safe range check (mock == real): first_query + query_count wraps in uint32. A zero
    // count is malformed (vkResetQueryPool requires queryCount > 0).
    if (req.query_count == 0 || req.first_query > it->second.count ||
        req.query_count > it->second.count - req.first_query) {
        resp.reason = "reset_query_pool range out of bounds";
        return resp;
    }
    const auto dev = devices_.find(it->second.device);
    if (dev == devices_.end() || dev->second.vk == VK_NULL_HANDLE) {
        resp.reason = "query pool has no live device";
        return resp;
    }
    vkResetQueryPool(dev->second.vk, it->second.vk, req.first_query, req.query_count);
    resp.ok = true;
    resp.reason = "ok";
    return resp;
}

vkrpc::StatusResponse
RealVulkanBackend::write_memory_ranges(const vkrpc::WriteMemoryRangesRequest& req) {
    vkrpc::StatusResponse resp;
    // Validate-then-apply (atomic): every target must be a known, HOST_VISIBLE|HOST_COHERENT memory
    // (the same fail-closed check as the mock), every range inside the
    // allocation, and the payload long enough for the summed ranges.
    const VkMemoryPropertyFlags kCoherent =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    std::uint64_t cursor = 0;
    for (const vkrpc::MemoryUpload& u : req.uploads) {
        const auto mem = leaves_.find(u.memory);
        if (mem == leaves_.end() || mem->second.kind != LeafKind::Memory) {
            resp.ok = false;
            resp.reason = "write_memory_ranges references unknown memory";
            return resp;
        }
        if ((mem->second.memory_flags & kCoherent) != kCoherent) {
            resp.ok = false;
            resp.reason = "write_memory_ranges target is not HOST_VISIBLE | HOST_COHERENT";
            return resp;
        }
        for (const vkrpc::MemoryUploadRange& r : u.ranges) {
            if (r.offset > static_cast<std::uint64_t>(mem->second.memory_size) ||
                r.offset > ~0ull - r.size ||
                r.offset + r.size > static_cast<std::uint64_t>(mem->second.memory_size)) {
                resp.ok = false;
                resp.reason = "write_memory_ranges range outside the allocation";
                return resp;
            }
            if (cursor + r.size > req.payload.size()) {
                resp.ok = false;
                resp.reason = "write_memory_ranges payload shorter than its ranges";
                return resp;
            }
            cursor += r.size;
        }
    }
    // Apply: map the whole allocation once, scatter each range, then unmap (coherent, so no
    // explicit flush is required for visibility -- the host write is visible to the device).
    cursor = 0;
    for (const vkrpc::MemoryUpload& u : req.uploads) {
        const RealLeaf& leaf = leaves_.at(u.memory);
        const auto dev = devices_.find(leaf.device);
        if (dev == devices_.end() || dev->second.vk == VK_NULL_HANDLE) {
            resp.ok = false;
            resp.reason = "memory has no live device";
            return resp;
        }
        void* mapped = nullptr;
        if (vkMapMemory(dev->second.vk, leaf.memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS ||
            mapped == nullptr) {
            resp.ok = false;
            resp.reason = "vkMapMemory failed in write_memory_ranges";
            return resp;
        }
        for (const vkrpc::MemoryUploadRange& r : u.ranges) {
            std::memcpy(static_cast<char*>(mapped) + r.offset, req.payload.data() + cursor,
                        static_cast<std::size_t>(r.size));
            cursor += r.size;
        }
        vkUnmapMemory(dev->second.vk, leaf.memory);
    }
    resp.ok = true;
    resp.reason = "ok";
    return resp;
}

// Host-visible memory readback: map the real VkDeviceMemory and copy the requested ranges out. The
// ICD calls this after a fence signals to refresh its shadow for clean mapped allocations, so an
// app that maps + reads a GPU-written buffer (a query-results copy destination, transform-feedback
// output, ...) sees the real bytes. Coherent memory is up to date after the fence; for robustness
// invalidate the mapped range before reading (a no-op on coherent memory).
vkrpc::ReadMemoryRangesResponse
RealVulkanBackend::read_memory_ranges(const vkrpc::ReadMemoryRangesRequest& req) {
    vkrpc::ReadMemoryRangesResponse resp;
    std::string out;
    for (const vkrpc::MemoryUpload& u : req.reads) {
        const auto memit = leaves_.find(u.memory);
        if (memit == leaves_.end() || memit->second.kind != LeafKind::Memory) {
            resp.ok = false;
            resp.reason = "read_memory_ranges references unknown memory";
            return resp;
        }
        const RealLeaf& leaf = memit->second;
        const auto dev = devices_.find(leaf.device);
        if (dev == devices_.end() || dev->second.vk == VK_NULL_HANDLE) {
            resp.ok = false;
            resp.reason = "memory has no live device";
            return resp;
        }
        // Worker-boundary validation (query-pools): every requested range must lie
        // inside the real allocation before we map + read -- exactly the guard write_memory_ranges
        // uses (overflow-safe: offset > size-of-allocation, or offset + size wraps). A malformed /
        // stale readback request must fail closed, never read past the VkDeviceMemory mapping.
        for (const vkrpc::MemoryUploadRange& r : u.ranges) {
            if (r.offset > static_cast<std::uint64_t>(leaf.memory_size) ||
                r.offset > ~0ull - r.size ||
                r.offset + r.size > static_cast<std::uint64_t>(leaf.memory_size)) {
                resp.ok = false;
                resp.reason = "read_memory_ranges range outside the allocation";
                return resp;
            }
        }
        void* mapped = nullptr;
        if (vkMapMemory(dev->second.vk, leaf.memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS ||
            mapped == nullptr) {
            resp.ok = false;
            resp.reason = "vkMapMemory failed in read_memory_ranges";
            return resp;
        }
        VkMappedMemoryRange inv{};
        inv.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        inv.memory = leaf.memory;
        inv.offset = 0;
        inv.size = VK_WHOLE_SIZE;
        vkInvalidateMappedMemoryRanges(dev->second.vk, 1, &inv); // no-op on coherent memory
        for (const vkrpc::MemoryUploadRange& r : u.ranges) {
            const char* src = static_cast<const char*>(mapped) + r.offset;
            out.append(src, static_cast<std::size_t>(r.size));
        }
        vkUnmapMemory(dev->second.vk, leaf.memory);
    }
    resp.ok = true;
    resp.reason = "ok";
    resp.payload = std::move(out);
    return resp;
}

vkrpc::CreateSurfaceResponse
RealVulkanBackend::create_surface(const vkrpc::CreateSurfaceRequest& req) {
    vkrpc::CreateSurfaceResponse resp;
    const auto it = instances_.find(req.instance);
    if (it == instances_.end()) {
        resp.ok = false;
        resp.reason = "unknown instance handle";
        return resp;
    }
    // The window thread (one per session, owns every HWND) is created lazily on the first
    // surface OR the first sidecar placeholder; ensure_window_thread() makes that cross-plane safe.
    // The window starts hidden and is revealed on first present.
    windowing::WindowThread* wt = ensure_window_thread();
    if (wt == nullptr) {
        resp.ok = false;
        resp.reason = "failed to start window thread";
        return resp;
    }
    const windowing::CreatedWindow win = wt->create_hidden_window(256, 256);
    if (win.hwnd == nullptr) {
        resp.ok = false;
        resp.reason = "failed to create backing window";
        return resp;
    }
    VkWin32SurfaceCreateInfoKHR sci{};
    sci.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    sci.hinstance = GetModuleHandleW(nullptr);
    sci.hwnd = win.hwnd;
    VkSurfaceKHR vk = VK_NULL_HANDLE;
    if (vkCreateWin32SurfaceKHR(it->second.vk, &sci, nullptr, &vk) != VK_SUCCESS ||
        vk == VK_NULL_HANDLE) {
        wt->destroy_window(win.hwnd);
        resp.ok = false;
        resp.reason = "vkCreateWin32SurfaceKHR failed";
        return resp;
    }
    const std::uint64_t handle = next_handle_++;
    RealSurface s;
    s.vk = vk;
    s.instance = req.instance;
    s.hwnd = win.hwnd;
    s.slot = win.slot;
    s.platform = req.platform;
    s.xid = req.xid;
    s.role_hint = req.role_hint;
    surfaces_.emplace(handle, std::move(s));
    it->second.surfaces.insert(handle);
    // Born-correlated surface<->XID: bind under the backend mutex (cross-plane
    // with the sidecar's register_toplevel) so the visible HWND is never an unregistered orphan. If
    // the sidecar registered this toplevel first, the bind PROMOTES it -- destroy the
    // now-superseded placeholder HWND off-lock (Model A: the surface HWND is the sole
    // representation).
    sidecar::RegistryEffect eff;
    HWND placeholder_to_destroy = nullptr;
    std::uint64_t epoch = 0;
    {
        std::lock_guard<std::mutex> lk(backend_mutex_);
        eff = registry_.bind_surface(req.xid, handle);
        if (eff.kind == sidecar::RegistryEffect::Kind::PromotePlaceholderToSurface) {
            const auto ph = placeholder_hwnds_.find(eff.placeholder_id);
            if (ph != placeholder_hwnds_.end()) {
                placeholder_to_destroy = ph->second;
                placeholder_hwnds_.erase(ph);
            }
        }
        epoch = registry_.epoch_for_xid(req.xid);
        // Mirror the surface HWND for the sidecar plane's input-epoch reconcile.
        if (req.xid != 0) {
            surface_input_hwnds_[req.xid] = win.hwnd; // last surface for an xid wins (matches bind)
        }
    }
    if (placeholder_to_destroy != nullptr) {
        wt->destroy_window(placeholder_to_destroy); // off-lock
    }
    // the surface HWND is the guest toplevel's representation (Model A) -- bind its
    // slot so the WndProc captures input on it. For a born-correlated surface (xid != 0); a legacy
    // untopologied surface (xid == 0) gets no input target (has_input_target() stays false). The
    // epoch is the registry's current representation epoch for the xid (bind_surface just minted it
    // for a real toplevel; 0 for an untopologied/pending surface, which carries no input anyway).
    if (req.xid != 0) {
        wt->set_slot_input_target(win.hwnd, &input_queue_, req.xid, epoch);
        if (debug_window_titles()) {
            wt->set_window_title_tag(win.hwnd, req.xid); // dev-helper correlation
        }
    }
    // if this surface PROMOTED/replaced a registered toplevel's
    // representation, place the brand-new surface HWND at the registry's current geometry -- a
    // placeholder already moved to the right X-root position must not be promoted to a
    // CW_USEDEFAULT surface. The registry stamps host_apply_seq on the bind effect for a registered
    // xid; a surface-first (no toplevel yet) carries no seq -- the later register places it.
    // Seq-gated, off-lock; the window is still hidden (shown on first present). eff.apply_size
    // carries the authority (normally false unless a resize was authored before the surface).
    if (eff.host_apply_seq != 0) {
        wt->apply_geometry(win.hwnd, static_cast<int>(eff.geometry.x),
                           static_cast<int>(eff.geometry.y), static_cast<int>(eff.geometry.width),
                           static_cast<int>(eff.geometry.height), eff.host_apply_seq,
                           eff.apply_size,
                           eff.z_order); // z_order None on an establish/promote
    }
    resp.ok = true;
    resp.reason = "ok";
    resp.surface = handle;
    return resp;
}

vkrpc::StatusResponse RealVulkanBackend::destroy_surface(const vkrpc::HandleRequest& req) {
    vkrpc::StatusResponse resp;
    const auto it = surfaces_.find(req.handle);
    if (it == surfaces_.end()) {
        resp.ok = false;
        resp.reason = "unknown surface handle";
        return resp;
    }
    if (!it->second.swapchains.empty()) {
        resp.ok = false;
        resp.reason = "surface has live swapchains; destroy them first";
        return resp;
    }
    const auto inst = instances_.find(it->second.instance);
    if (inst != instances_.end() && inst->second.vk != VK_NULL_HANDLE &&
        it->second.vk != VK_NULL_HANDLE) {
        // Lost-device containment: on a tainted instance the host surface is abandoned (an
        // abandoned swapchain may still reference it); tables/HWND still clean up below.
        if (!inst->second.lost_device_taint) {
            vkDestroySurfaceKHR(inst->second.vk, it->second.vk, nullptr);
        }
        inst->second.surfaces.erase(req.handle);
    }
    if (it->second.hwnd != nullptr && window_thread_) {
        window_thread_->destroy_window(it->second.hwnd);
    }
    // Surface-specific unbind under the backend mutex (cross-plane). A no-op when
    // the surface had no XID, or when a newer surface already rebound this XID; never a host effect
    // (a still-registered toplevel falls back to a representation-less entry, reaped by
    // unregister).
    {
        std::lock_guard<std::mutex> lk(backend_mutex_);
        registry_.unbind_surface(it->second.xid, req.handle);
        // Drop the surface-input HWND mirror IFF it still points at THIS surface's window (a newer
        // surface for a recycled xid may have already rebound it; surface-specific).
        const auto sh = surface_input_hwnds_.find(it->second.xid);
        if (sh != surface_input_hwnds_.end() && sh->second == it->second.hwnd) {
            surface_input_hwnds_.erase(sh);
        }
        // drop the deferred-extent record IFF it points at THIS surface --
        // surface-specific, keyed on the destroyed handle, NOT the current-hwnd check above: a
        // newer replacement surface for the same xid keeps its own record, and destroying a stale
        // non-recorded surface must not clear the recorded one.
        const auto d = deferred_extent_surface_by_xid_.find(it->second.xid);
        if (d != deferred_extent_surface_by_xid_.end() && d->second == req.handle) {
            deferred_extent_surface_by_xid_.erase(d);
        }
    }
    surfaces_.erase(it);
    resp.ok = true;
    resp.reason = "ok";
    return resp;
}

vkrpc::CreateSwapchainResponse
RealVulkanBackend::create_swapchain(const vkrpc::CreateSwapchainRequest& req) {
    vkrpc::CreateSwapchainResponse resp;
    const auto dev = devices_.find(req.device);
    if (dev == devices_.end()) {
        resp.ok = false;
        resp.reason = "unknown device handle";
        return resp;
    }
    // Lost-device latch: a recreate on a lost device must not re-enter the
    // host. Honest local answer -- the app observes DEVICE_LOST and gives up cleanly.
    if (dev->second.lost) {
        resp.ok = true;
        resp.reason = "device lost";
        resp.result = vkrpc::kVkErrorDeviceLost;
        resp.swapchain = 0;
        return resp;
    }
    const auto surf = surfaces_.find(req.surface);
    if (surf == surfaces_.end()) {
        resp.ok = false;
        resp.reason = "unknown surface handle";
        return resp;
    }
    if (surf->second.instance != dev->second.instance) {
        resp.ok = false;
        resp.reason = "surface and device belong to different instances";
        return resp;
    }
    // Reject malformed/sentinel request fields up front: the decoder maps missing,
    // wrong-typed, or out-of-range values to -1 precisely so the backend rejects them
    // rather than silently substituting its own. All these fields are required.
    // Reject malformed/sentinel fields up front. width/height must be > 0: the
    // worker reports min extent 1x1 and a Vulkan swapchain extent cannot be zero, so a 0 (or the
    // decoder's -1 for missing/out-of-range) is rejected, never defaulted.
    // (GL/zink): when the app DEFERRED the extent (use_current_extent), width/height are
    // not required -- the worker uses the host surface's real currentExtent. Otherwise they must be
    // > 0.
    if (req.image_format < 0 || req.color_space < 0 || req.present_mode < 0 ||
        req.min_image_count < 1 || req.image_usage < 1 ||
        (!req.use_current_extent && (req.width <= 0 || req.height <= 0))) {
        resp.ok = false;
        resp.reason = "malformed or missing swapchain parameters";
        return resp;
    }
    // (GL/zink): a deferred-extent create marks the surface so subsequent capability
    // queries advertise a CONCRETE, pinned currentExtent (kopper converges instead of looping).
    // Native apps pass a concrete extent and never flip this, keeping the app-picks range.
    if (req.use_current_extent) {
        surf->second.defers_extent = true;
        // record (cross-plane, under the mutex) WHICH surface deferred
        // this xid's extent -- surface-specific, so a later sidecar-plane register_toplevel adopts
        // the registered SIZE only when THIS surface is still the current bound one (a replacement
        // surface for the same xid must not inherit the deferred bit). req.surface is this
        // deferring surface.
        if (surf->second.xid != 0) {
            std::lock_guard<std::mutex> lk(backend_mutex_);
            deferred_extent_surface_by_xid_[surf->second.xid] = req.surface;
        }
    }
    if (!window_thread_) {
        resp.ok = false;
        resp.reason = "no window thread for surface";
        return resp;
    }
    const auto inst = instances_.find(dev->second.instance);
    if (inst == instances_.end() || inst->second.physical_vk == VK_NULL_HANDLE) {
        resp.ok = false;
        resp.reason = "device has no physical device";
        return resp;
    }
    // (integrity): if the sidecar has authored a live
    // extent for this surface's toplevel, the app MUST converge to the host's CURRENT realizable
    // client -- it does not get to resize the host. When authoritative we therefore SKIP
    // set_client_extent entirely (never move the host) and let the convergence check below enforce
    // that the app's requested extent equals the host's real currentExtent (the achieved client,
    // already clamped to what Win32 can realize -- so a tiny/oversized authored size pins to the
    // realizable extent, never a value the app can never reach -> no OUT_OF_DATE retry loop). A
    // mismatch returns OUT_OF_DATE without ever calling set_client_extent.
    bool extent_authoritative = false;
    {
        std::lock_guard<std::mutex> lk(backend_mutex_);
        extent_authoritative = registry_.authoritative_extent_for_xid(surf->second.xid).active;
    }
    VkPhysicalDevice phys = inst->second.physical_vk;
    VkSurfaceKHR vksurf = surf->second.vk;
    VkDevice dev_vk = dev->second.vk;
    const std::uint32_t present_family = dev->second.queue_family;
    HWND hwnd = surf->second.hwnd;
    windowing::WindowSlot* slot = surf->second.slot;
    const auto req_format = static_cast<VkFormat>(req.image_format);
    const auto req_color_space = static_cast<VkColorSpaceKHR>(req.color_space);
    const auto req_present_mode = static_cast<VkPresentModeKHR>(req.present_mode);

    struct CreateResult {
        bool ok = false;
        std::string reason;
        VkSwapchainKHR vk = VK_NULL_HANDLE;
        VkExtent2D extent{};
        std::uint32_t min_count = 0;
        bool transfer_dst = false;
        bool transfer_src = false;   // worker ADDED TRANSFER_SRC for the gated present-capture
        VkImageUsageFlags usage = 0; // the EFFECTIVE usage the images were created with
        int result =
            vkrpc::kVkSuccess; // -> OUT_OF_DATE if the window cannot converge to the request
    } cr;

    // hold the per-surface slot lock across the create (excludes present/acquire and geometry/slot
    // mutations). All HWND-touching WSI work
    // runs on the window thread; the session thread blocks here for it.
    std::unique_lock<std::mutex> slot_lk;
    if (slot != nullptr) {
        slot_lk = std::unique_lock<std::mutex>(slot->slot_lock());
    }
    // the app picked its extent (we report the sentinel currentExtent), so make the host
    // window's CLIENT rect become exactly that -- via the window-thread seam the sidecar geometry
    // authority will later drive. `actual` is what the window really became; the create below
    // detects a non-converging request and returns OUT_OF_DATE rather than presenting at a stale
    // extent.
    // App-driven sizing ONLY when the sidecar is NOT the extent authority: the app picks its extent
    // and we make the host client match it. When authoritative the sidecar already drove the client
    // (the resize apply), so we do NOT call set_client_extent (no move); the convergence check
    // below validates the app matched the host's real currentExtent instead.
    windowing::WindowThread::ClientExtent actual;
    if (!extent_authoritative && hwnd != nullptr) {
        if (req.width > 0 && req.height > 0) {
            // Native: the app chose a concrete extent -> make the host client match it.
            actual = window_thread_->set_client_extent(hwnd, req.width, req.height);
        } else if (req.use_current_extent) {
            // (GL/zink): a DEFERRED-extent surface lets the SURFACE dictate the size.
            // Default the host client to the toolkit toplevel's REAL geometry (the sidecar
            // registered it) so zink renders the app at its true window size, not the 256x256
            // placeholder default. Read the registry geometry under the backend mutex, then size
            // OFF-lock (never hold it across a window-thread call). Gated on a known geometry ->
            // native apps + the in-process tests (no sidecar -> empty registry) are unaffected; the
            // sidecar does NOT become the resize authority (that stays a explicit-resize
            // concern), so get_surface_capabilities pins this surface via its `defers_extent` flag
            // to the achieved client.
            sidecar::WindowRegistry::AuthoritativeExtent geo;
            {
                std::lock_guard<std::mutex> lk(backend_mutex_);
                geo = registry_.geometry_for_xid(surf->second.xid);
            }
            if (geo.active) {
                actual = window_thread_->set_client_extent(hwnd, static_cast<int>(geo.width),
                                                           static_cast<int>(geo.height));
            }
        }
    }
    const bool routed = window_thread_->invoke([&]() {
        // The device's queue family must support present to this surface.
        VkBool32 present_ok = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(phys, present_family, vksurf, &present_ok);
        if (!present_ok) {
            cr.reason = "device queue family does not support present to this surface";
            return;
        }
        VkSurfaceCapabilitiesKHR caps{};
        if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, vksurf, &caps) != VK_SUCCESS) {
            cr.reason = "vkGetPhysicalDeviceSurfaceCapabilitiesKHR failed";
            return;
        }

        // convergence : the window must have actually become the app's requested client
        // extent. If it did not -- a Win32 clamp (caption-min width / work-area), DPI, or a request
        // beyond what the host can realize -- do NOT create a swapchain at the stale host extent;
        // report OUT_OF_DATE so the app re-queries caps + retries (normal WSI). Never silently fall
        // back to caps.currentExtent (the older stale-extent bug). When the sidecar is the extent
        // authority we did NOT call set_client_extent, so `actual` is unset -- the gate is purely
        // that the app matched the host's real currentExtent (the realizable client the sidecar
        // drove).
        if (!req.use_current_extent && req.width > 0 && req.height > 0) {
            const auto rw = static_cast<std::uint32_t>(req.width);
            const auto rh = static_cast<std::uint32_t>(req.height);
            const bool client_ok =
                extent_authoritative || (actual.width == req.width && actual.height == req.height);
            if (!client_ok || caps.currentExtent.width != rw || caps.currentExtent.height != rh) {
                cr.result = vkrpc::kVkErrorOutOfDateKhr;
                cr.reason = "window did not converge to requested extent " +
                            std::to_string(req.width) + "x" + std::to_string(req.height) +
                            " (client " + std::to_string(actual.width) + "x" +
                            std::to_string(actual.height) + ", currentExtent " +
                            std::to_string(caps.currentExtent.width) + "x" +
                            std::to_string(caps.currentExtent.height) + ")";
                return;
            }
        }

        // Honor the requested (format, color space): use it only if the surface supports
        // it -- never silently substitute, since an app records work against the format
        // it asked for. A lone VK_FORMAT_UNDEFINED entry is the legacy "any format" case.
        std::uint32_t fmt_count = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(phys, vksurf, &fmt_count, nullptr);
        if (fmt_count == 0) {
            cr.reason = "surface reports no formats";
            return;
        }
        std::vector<VkSurfaceFormatKHR> formats(fmt_count);
        vkGetPhysicalDeviceSurfaceFormatsKHR(phys, vksurf, &fmt_count, formats.data());
        bool format_ok = false;
        if (formats.size() == 1 && formats[0].format == VK_FORMAT_UNDEFINED) {
            format_ok = req_format != VK_FORMAT_UNDEFINED;
        } else {
            for (const auto& f : formats) {
                if (f.format == req_format && f.colorSpace == req_color_space) {
                    format_ok = true;
                    break;
                }
            }
        }
        if (!format_ok) {
            cr.reason = "requested surface format/color space is not supported";
            return;
        }

        // Honor the requested present mode only if supported (FIFO is guaranteed by spec,
        // but an unsupported mode must fail rather than be silently swapped).
        std::uint32_t pm_count = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(phys, vksurf, &pm_count, nullptr);
        if (pm_count == 0) {
            cr.reason = "surface reports no present modes";
            return;
        }
        std::vector<VkPresentModeKHR> present_modes(pm_count);
        vkGetPhysicalDeviceSurfacePresentModesKHR(phys, vksurf, &pm_count, present_modes.data());
        bool present_mode_ok = false;
        for (const VkPresentModeKHR m : present_modes) {
            if (m == req_present_mode) {
                present_mode_ok = true;
                break;
            }
        }
        if (!present_mode_ok) {
            cr.reason = "requested present mode is not supported";
            return;
        }

        // Extent: after the client-rect resize, currentExtent reflects the requested
        // size; honor the 0xFFFFFFFF "app picks" sentinel by clamping the request.
        VkExtent2D extent = caps.currentExtent;
        if (extent.width == 0xFFFFFFFFu) {
            std::uint32_t w =
                req.width > 0 ? static_cast<std::uint32_t>(req.width) : caps.minImageExtent.width;
            std::uint32_t h = req.height > 0 ? static_cast<std::uint32_t>(req.height)
                                             : caps.minImageExtent.height;
            if (w < caps.minImageExtent.width)
                w = caps.minImageExtent.width;
            if (w > caps.maxImageExtent.width)
                w = caps.maxImageExtent.width;
            if (h < caps.minImageExtent.height)
                h = caps.minImageExtent.height;
            if (h > caps.maxImageExtent.height)
                h = caps.maxImageExtent.height;
            extent.width = w;
            extent.height = h;
        }

        // Image count: the request is a floor. Reject above the surface maximum
        // (unsatisfiable -- never silently lower it); raise a too-low request to the
        // minimum (the app learns the actual count from get_swapchain_images).
        if (caps.maxImageCount != 0 &&
            static_cast<std::uint32_t>(req.min_image_count) > caps.maxImageCount) {
            cr.reason = "requested min_image_count exceeds the surface maximum";
            return;
        }
        std::uint32_t min_count = static_cast<std::uint32_t>(req.min_image_count);
        if (min_count < caps.minImageCount) {
            min_count = caps.minImageCount;
        }

        VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        if (!(caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)) {
            for (std::uint32_t bit = 1; bit != 0; bit <<= 1) {
                if (caps.supportedCompositeAlpha & bit) {
                    alpha = static_cast<VkCompositeAlphaFlagBitsKHR>(bit);
                    break;
                }
            }
        }

        // App-declared image usage: the app, not the worker, decides usage.
        // Validate every requested bit against the surface's supportedUsageFlags -- an
        // unsupported bit fails with the shared capability reason (so a test can skip on
        // exactly that gap; in practice only TRANSFER_DST may be absent), never a silent
        // worker addition. transfer_dst (clear support) derives from the request.
        VkImageUsageFlags usage = static_cast<VkImageUsageFlags>(req.image_usage);
        // compat (GL/zink): jammy's stock Mesa 23.2 zink requests INPUT_ATTACHMENT
        // swapchain usage without masking it against the surface's supportedUsageFlags
        // (fixed in later Mesa). NVIDIA hosts advertise the bit so this never fired there;
        // Intel iGPU hosts do not, which would fail EVERY stock-zink swapchain. The bit is
        // only exercised by GL framebuffer-fetch paths none of the apps use, so
        // drop exactly this bit -- loudly -- when the surface lacks it. Every other
        // unsupported bit still fails closed below.
        const VkImageUsageFlags zink_compat_dropped =
            usage & VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT & ~caps.supportedUsageFlags;
        if (zink_compat_dropped != 0) {
            VKR_WARN(kComponent) << "real create_swapchain: dropping requested "
                                    "INPUT_ATTACHMENT usage the surface does not support "
                                    "(zink/Mesa 23.x compat; supportedUsageFlags 0x"
                                 << std::hex << caps.supportedUsageFlags << std::dec << ")";
            usage &= ~zink_compat_dropped;
        }
        if ((usage & caps.supportedUsageFlags) != usage) {
            // The shared reason string stays exact (tests key on it); the concrete bit gap
            // is only visible here, so log it -- "which bit" is the whole diagnosis on a
            // host whose surface advertises fewer usages than the app assumed.
            VKR_WARN(kComponent) << "real create_swapchain: requested usage 0x" << std::hex << usage
                                 << " exceeds surface supportedUsageFlags 0x"
                                 << caps.supportedUsageFlags << " (unsupported bits 0x"
                                 << (usage & ~caps.supportedUsageFlags) << ")" << std::dec;
            cr.reason = vkrpc::kSwapchainUsageNotSupportedReason;
            return;
        }
        cr.transfer_dst = (usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) != 0;

        // present-capture (debug, gated): when VKRELAY2_DEBUG_DUMP_PRESENT is set the
        // worker copies FROM the swapchain image, which requires VK_IMAGE_USAGE_TRANSFER_SRC_BIT.
        // Add it to the WORKER's swapchain (additive -- the app's recorded commands are unaffected)
        // ONLY if the surface advertises it; otherwise leave usage untouched, log, and let
        // present-capture skip (never create an image the capture copy would then
        // access illegally). No-op unless the env var is set, so production is unchanged.
        VkImageUsageFlags effective_usage = usage;
        if (std::getenv("VKRELAY2_DEBUG_DUMP_PRESENT") != nullptr) {
            if ((caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0) {
                effective_usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
                cr.transfer_src = true;
            } else {
                VKR_INFO(kComponent) << "real create_swapchain: present capture unavailable "
                                        "(surface lacks TRANSFER_SRC usage 0x"
                                     << std::hex << caps.supportedUsageFlags << std::dec << ")";
            }
        }
        cr.usage =
            effective_usage; // recorded onto the swapchain for the self-describing capture meta

        VkSwapchainCreateInfoKHR ci{};
        ci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        ci.surface = vksurf;
        ci.minImageCount = min_count;
        ci.imageFormat = req_format;
        ci.imageColorSpace = req_color_space;
        ci.imageExtent = extent;
        ci.imageArrayLayers = 1;
        ci.imageUsage = effective_usage;
        ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ci.preTransform = caps.currentTransform;
        ci.compositeAlpha = alpha;
        ci.presentMode = req_present_mode;
        ci.clipped = VK_TRUE;
        // (GL/zink): forward oldSwapchain so a recreate retires the previous swapchain on
        // this surface (else the host rejects the second one with NATIVE_WINDOW_IN_USE_KHR).
        //
        // cross-adapter retirement: on this hybrid laptop the
        // dGPU-rendered swapchain presents CROSS-ADAPTER to the iGPU-owned panel, and a recreate
        // that retires the old swapchain while a present of its images is still in flight loses
        // the device (live-reproduced; a device-wide idle does NOT cover the presentation engine).
        // When the device has present_fence_retire (VK_EXT_swapchain_maintenance1, enabled
        // privately), every worker present attached a per-swapchain fence; wait them ALL here
        // (finite timeout) before handing the old swapchain to oldSwapchain -- the spec's "safe to
        // retire/destroy" signal for a present's resources. On timeout or loss FAIL the create
        // (the session ends cleanly) rather than retiring anyway.
        if (req.old_swapchain != 0) {
            const auto old = swapchains_.find(req.old_swapchain);
            if (old != swapchains_.end() && old->second.device == req.device) {
                if (!old->second.present_fences.empty()) {
                    const auto t0 = std::chrono::steady_clock::now();
                    constexpr std::uint64_t kRetireFenceTimeoutNs = 2ull * 1000 * 1000 * 1000;
                    const VkResult fw = observe_device_result(
                        req.device,
                        vkWaitForFences(
                            dev_vk, static_cast<std::uint32_t>(old->second.present_fences.size()),
                            old->second.present_fences.data(), VK_TRUE, kRetireFenceTimeoutNs),
                        "vkWaitForFences (present-fence retire)");
                    const auto wait_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                             std::chrono::steady_clock::now() - t0)
                                             .count();
                    VKR_INFO(kComponent)
                        << "present-fence retire: waited " << old->second.present_fences.size()
                        << " fence(s) for old sc=" << req.old_swapchain << " in " << wait_ms
                        << " ms -> VkResult " << static_cast<int>(fw);
                    if (fw != VK_SUCCESS) {
                        cr.result = static_cast<int>(fw);
                        cr.reason = "present-fence retire wait failed (VkResult " +
                                    std::to_string(static_cast<int>(fw)) +
                                    "); refusing to retire the old swapchain";
                        return;
                    }
                    for (VkFence f : old->second.present_fences) {
                        vkDestroyFence(dev_vk, f, nullptr);
                    }
                    old->second.present_fences.clear();
                }
                ci.oldSwapchain = old->second.vk;
            }
        }

        VkSwapchainKHR vk = VK_NULL_HANDLE;
        const VkResult scr = observe_device_result(
            req.device, vkCreateSwapchainKHR(dev_vk, &ci, nullptr, &vk), "vkCreateSwapchainKHR");
        if (scr != VK_SUCCESS || vk == VK_NULL_HANDLE) {
            cr.result = static_cast<int>(scr);
            cr.reason = "vkCreateSwapchainKHR failed (VkResult " + std::to_string(scr) +
                        ", extent " + std::to_string(extent.width) + "x" +
                        std::to_string(extent.height) + ", usage 0x" + std::to_string(usage) +
                        ", supportedUsage 0x" + std::to_string(caps.supportedUsageFlags) + ")";
            return;
        }
        cr.ok = true;
        cr.vk = vk;
        cr.extent = extent;
        cr.min_count = min_count;
        // the fresh swapchain matches the window's current geometry -- record the
        // extent (for the WM_SIZE size-delta filter) and clear the dirty latch last, so
        // any WM_SIZE from our own resize above does not strand the first present.
        if (slot != nullptr) {
            slot->set_swapchain_extent(static_cast<int>(extent.width),
                                       static_cast<int>(extent.height));
            slot->clear_geometry_dirty();
        }
    });
    if (!routed) {
        resp.ok = false;
        resp.reason = "window thread unavailable";
        return resp;
    }
    if (cr.result == vkrpc::kVkErrorOutOfDateKhr || cr.result == vkrpc::kVkErrorDeviceLost) {
        // Ran cleanly, but the surface could not become the requested extent -- NOT a fault.
        // ok=true so the ICD returns VK_ERROR_OUT_OF_DATE_KHR (the app re-queries caps + retries),
        // not fault().
        resp.ok = true;
        resp.result = cr.result;
        resp.reason = cr.reason;
        resp.swapchain = 0;
        return resp;
    }
    if (!cr.ok) {
        resp.ok = false;
        resp.reason = cr.reason;
        return resp;
    }
    const std::uint64_t handle = next_handle_++;
    RealSwapchain sc;
    sc.vk = cr.vk;
    sc.device = req.device;
    sc.surface = req.surface;
    sc.extent = cr.extent;
    sc.image_format = req_format; // an image view over these images must match it
    sc.transfer_dst = cr.transfer_dst;
    sc.transfer_src = cr.transfer_src; // present-capture (debug): copy-from-image enabled
    sc.image_usage = cr.usage;         // effective usage, for the self-describing capture meta
    swapchains_.emplace(handle, std::move(sc));
    dev->second.swapchains.insert(handle);
    surf->second.swapchains.insert(handle);
    resp.ok = true;
    resp.reason = "ok";
    resp.swapchain = handle;
    VKR_INFO(kComponent) << "real create_swapchain -> " << cr.extent.width << "x"
                         << cr.extent.height << " format=" << static_cast<int>(req_format)
                         << " minImages=" << cr.min_count
                         << " use_current_extent=" << (req.use_current_extent ? 1 : 0)
                         << " req=" << req.width << "x" << req.height
                         << " oldSwapchain=" << req.old_swapchain;
    return resp;
}

vkrpc::StatusResponse RealVulkanBackend::destroy_swapchain(const vkrpc::HandleRequest& req) {
    vkrpc::StatusResponse resp;
    const auto it = swapchains_.find(req.handle);
    if (it == swapchains_.end()) {
        resp.ok = false;
        resp.reason = "unknown swapchain handle";
        return resp;
    }
    // an image view over this swapchain's images must be destroyed first (Vulkan destroy
    // ordering -- a view outlives its image otherwise). Mirrors the mock.
    if (!it->second.image_views.empty()) {
        resp.ok = false;
        resp.reason = "swapchain has live image views; destroy them first";
        return resp;
    }
    VKR_INFO(kComponent) << "real destroy_swapchain " << req.handle;
    const auto dev = devices_.find(it->second.device);
    const auto surf = surfaces_.find(it->second.surface);
    windowing::WindowSlot* slot = (surf != surfaces_.end()) ? surf->second.slot : nullptr;
    // hold the slot lock; route the destroy onto the window thread (HWND-touching
    // WSI). No active swapchain remains, so clear the slot's recorded extent.
    std::unique_lock<std::mutex> slot_lk;
    if (slot != nullptr) {
        slot_lk = std::unique_lock<std::mutex>(slot->slot_lock());
    }
    // Lost-device latch: NEVER touch a lost device -- drop the tables and
    // abandon the host objects (swapchain + any present fences) to process termination.
    if (dev != devices_.end() && dev->second.vk != VK_NULL_HANDLE && !dev->second.lost &&
        it->second.vk != VK_NULL_HANDLE) {
        VkDevice dev_vk = dev->second.vk;
        VkSwapchainKHR sc_vk = it->second.vk;
        std::vector<VkFence> fences = std::move(it->second.present_fences);
        it->second.present_fences.clear();
        // A presented image may still be in use by the queue, and
        // vkDestroySwapchainKHR requires those uses to have completed
        // (VUID-vkDestroySwapchainKHR-swapchain-01282). With present-fence retirement, wait this
        // swapchain's outstanding present fences first (finite -- the presentation-engine "safe to
        // destroy" signal a device-wide idle does not cover), then the idle. The idle's result is
        // CHECKED: a DEVICE_LOST from either wait latches the device and the
        // host destroy is skipped (abandoned).
        const auto destroy = [&]() {
            VkResult wr = VK_SUCCESS;
            if (!fences.empty()) {
                constexpr std::uint64_t kRetireFenceTimeoutNs = 2ull * 1000 * 1000 * 1000;
                wr = observe_device_result(
                    it->second.device,
                    vkWaitForFences(dev_vk, static_cast<std::uint32_t>(fences.size()),
                                    fences.data(), VK_TRUE, kRetireFenceTimeoutNs),
                    "vkWaitForFences (destroy swapchain)");
            }
            if (wr == VK_SUCCESS) {
                wr = observe_device_result(it->second.device, vkDeviceWaitIdle(dev_vk),
                                           "vkDeviceWaitIdle (destroy swapchain)");
            }
            if (wr == VK_ERROR_DEVICE_LOST) {
                return; // abandon the swapchain + fences (never re-enter the lost device)
            }
            for (VkFence f : fences) {
                vkDestroyFence(dev_vk, f, nullptr);
            }
            vkDestroySwapchainKHR(dev_vk, sc_vk, nullptr);
        };
        if (window_thread_) {
            window_thread_->invoke(destroy);
        } else {
            destroy();
        }
    }
    if (dev != devices_.end()) {
        dev->second.swapchains.erase(req.handle);
    }
    if (surf != surfaces_.end()) {
        surf->second.swapchains.erase(req.handle);
    }
    if (slot != nullptr) {
        slot->set_swapchain_extent(0, 0);
    }
    // Any command buffer recorded against this swapchain's images is now stale: its baked
    // host commands reference VkImages that die with the swapchain, so it must not reach
    // vkQueueSubmit. Invalidate it; a later re-record against a fresh swapchain revalidates it,
    // while queue_submit refuses an invalidated command buffer on the recorded check.
    for (auto& kv : command_buffers_) {
        if (kv.second.referenced_swapchains.count(req.handle) != 0) {
            kv.second.recorded = false;
            kv.second.referenced_surfaces.clear();
            kv.second.referenced_swapchains.clear();
        }
    }
    // The swapchain's images die with it: drop their resolvable entries so a destroyed
    // swapchain's image handle can no longer be referenced by record_command_buffer.
    for (const std::uint64_t img : it->second.images) {
        images_.erase(img);
    }
    swapchains_.erase(it);
    resp.ok = true;
    resp.reason = "ok";
    return resp;
}

bool RealVulkanBackend::ensure_swapchain_images(std::uint64_t swapchain_handle, RealSwapchain& sc,
                                                std::string& err) {
    if (!sc.images.empty()) {
        return true; // minted already (idempotent)
    }
    const auto dev = devices_.find(sc.device);
    if (dev == devices_.end() || dev->second.vk == VK_NULL_HANDLE) {
        err = "swapchain has no live device";
        return false;
    }
    std::uint32_t count = 0;
    if (vkGetSwapchainImagesKHR(dev->second.vk, sc.vk, &count, nullptr) != VK_SUCCESS ||
        count == 0) {
        err = "vkGetSwapchainImagesKHR failed";
        return false;
    }
    std::vector<VkImage> imgs(count, VK_NULL_HANDLE);
    if (vkGetSwapchainImagesKHR(dev->second.vk, sc.vk, &count, imgs.data()) != VK_SUCCESS) {
        err = "vkGetSwapchainImagesKHR (data) failed";
        return false;
    }
    imgs.resize(count);
    for (VkImage img : imgs) {
        const std::uint64_t handle = next_handle_++;
        sc.images.push_back(handle);
        sc.vk_images.push_back(img);
        // Register so record_command_buffer can resolve the image by handle.
        RealImage meta;
        meta.vk = img;
        meta.device = sc.device;
        meta.swapchain = swapchain_handle;
        meta.surface = sc.surface;
        meta.transfer_dst = sc.transfer_dst;
        meta.format = sc.image_format;
        meta.usage = sc.image_usage;
        meta.extent = sc.extent;
        meta.mip_levels = 1;
        meta.array_layers = 1;
        images_.emplace(handle, meta);
    }
    return true;
}

vkrpc::GetSwapchainImagesResponse
RealVulkanBackend::get_swapchain_images(const vkrpc::GetSwapchainImagesRequest& req) {
    vkrpc::GetSwapchainImagesResponse resp;
    const auto it = swapchains_.find(req.swapchain);
    if (it == swapchains_.end()) {
        resp.ok = false;
        resp.reason = "unknown swapchain handle";
        return resp;
    }
    if (!ensure_swapchain_images(req.swapchain, it->second, resp.reason)) {
        resp.ok = false;
        return resp;
    }
    resp.images = it->second.images;
    resp.ok = true;
    resp.reason = "ok";
    return resp;
}

bool RealVulkanBackend::resolve_queue(std::uint64_t queue, std::uint64_t& out_device,
                                      VkQueue& out_vk) {
    if (queue == 0) {
        return false;
    }
    for (auto& kv : devices_) {
        for (const auto& q : kv.second.queues) {
            if (q.second == queue) {
                const std::uint32_t family = static_cast<std::uint32_t>(q.first >> 32);
                const std::uint32_t index = static_cast<std::uint32_t>(q.first & 0xFFFFFFFFu);
                VkQueue vk = VK_NULL_HANDLE;
                vkGetDeviceQueue(kv.second.vk, family, index, &vk);
                if (vk == VK_NULL_HANDLE) {
                    return false;
                }
                out_device = kv.first;
                out_vk = vk;
                return true;
            }
        }
    }
    return false;
}

vkrpc::AcquireNextImageResponse
RealVulkanBackend::acquire_next_image(const vkrpc::AcquireNextImageRequest& req) {
    vkrpc::AcquireNextImageResponse resp;
    const auto it = swapchains_.find(req.swapchain);
    if (it == swapchains_.end()) {
        resp.ok = false;
        resp.reason = "unknown swapchain handle";
        return resp;
    }
    const auto dev = devices_.find(it->second.device);
    if (dev == devices_.end() || dev->second.vk == VK_NULL_HANDLE) {
        resp.ok = false;
        resp.reason = "swapchain has no live device";
        return resp;
    }
    // Lost-device latch: NEVER re-enter the host on a lost device -- the
    // post-loss vkAcquireNextImageKHR was observed blocking forever on the window thread (holding
    // slot locks), turning the recoverable loss into a hard session hang. Honest local answer.
    if (dev->second.lost) {
        resp.ok = true;
        resp.reason = "ok";
        resp.result = vkrpc::kVkErrorDeviceLost;
        return resp;
    }
    // Optional app sync objects: validate kind + device ownership when provided.
    VkSemaphore sem = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    if (req.semaphore != 0) {
        const auto leaf = leaves_.find(req.semaphore);
        if (leaf == leaves_.end() || leaf->second.kind != LeafKind::Semaphore ||
            leaf->second.device != it->second.device) {
            resp.ok = false;
            resp.reason = "semaphore is not a semaphore on the swapchain's device";
            return resp;
        }
        sem = leaf->second.semaphore;
    }
    if (req.fence != 0) {
        const auto leaf = leaves_.find(req.fence);
        if (leaf == leaves_.end() || leaf->second.kind != LeafKind::Fence ||
            leaf->second.device != it->second.device) {
            resp.ok = false;
            resp.reason = "fence is not a fence on the swapchain's device";
            return resp;
        }
        fence = leaf->second.fence;
    }
    const auto surf = surfaces_.find(it->second.surface);
    windowing::WindowSlot* slot = (surf != surfaces_.end()) ? surf->second.slot : nullptr;
    VkDevice dev_vk = dev->second.vk;
    VkSwapchainKHR sc_vk = it->second.vk;

    // take the slot lock blocking; while dirty, return OUT_OF_DATE without
    // calling the driver (and without advancing any image).
    std::unique_lock<std::mutex> slot_lk;
    if (slot != nullptr) {
        slot_lk = std::unique_lock<std::mutex>(slot->slot_lock());
    }
    if (slot != nullptr && slot->geometry_dirty()) {
        VKR_INFO(kComponent) << "acquire -> OUT_OF_DATE (geometry dirty) sc=" << req.swapchain;
        resp.ok = true;
        resp.reason = "ok";
        resp.result = vkrpc::kVkErrorOutOfDateKhr;
        return resp;
    }
    VKR_INFO(kComponent) << "acquire (not dirty) sc=" << req.swapchain;
    if (!window_thread_) {
        resp.ok = false;
        resp.reason = "window thread unavailable";
        return resp;
    }
    VkResult result = VK_ERROR_OUT_OF_DATE_KHR;
    std::uint32_t image_index = 0;
    std::string setup_err;
    bool aborted = false;
    // chunk the (possibly UINT64_MAX) guest acquire into bounded host waits so the pump
    // thread stays interruptible on session abort. 50 ms matches the observer's kLivenessQuantumMs
    // (~20 abort re-checks/s during a wedge); the ns value is the timeout unit for both the acquire
    // and the internal-fence wait.
    constexpr std::uint64_t kAcquireQuantumNs = 50ull * 1000 * 1000;
    const int kTimeoutResult = static_cast<int>(VK_TIMEOUT);
    const auto steady_now_ns = []() {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                              std::chrono::steady_clock::now().time_since_epoch())
                                              .count());
    };
    const auto is_aborting = [this]() { return session_aborting_.load(std::memory_order_relaxed); };
    const bool routed = window_thread_->invoke([&]() {
        // vkAcquireNextImageKHR needs at least one signal object. When the app supplies
        // none (the debug flow), use an internal fence and CPU-wait so the image
        // is ready to render on return; when it supplies its own, pass them through.
        VkFence internal = VK_NULL_HANDLE;
        VkFence signal_fence = fence;
        if (sem == VK_NULL_HANDLE && fence == VK_NULL_HANDLE) {
            VkFenceCreateInfo fci{};
            fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            // DEVICE_LOST is beyond vkCreateFence's documented result set, but post-loss drivers
            // can still return it. Observing it is cheap and avoids a later re-entry.
            const VkResult fr = observe_device_result(
                it->second.device, vkCreateFence(dev_vk, &fci, nullptr, &internal),
                "vkCreateFence (acquire internal)");
            if (fr != VK_SUCCESS || internal == VK_NULL_HANDLE) {
                if (fr == VK_ERROR_DEVICE_LOST) {
                    result = fr;
                    return;
                }
                // Don't call acquire with both signal objects null (invalid usage) --
                // surface a clean internal fault instead.
                setup_err = "vkCreateFence failed";
                return;
            }
            signal_fence = internal;
        }
        // Abort-aware acquire: a bounded host timeout per iteration; VK_TIMEOUT means "no image
        // yet"
        // -> loop and re-check abort. A timed-out attempt neither signals nor uses sem/fence, so
        // re-passing them stays within Vulkan's unsignaled/not-in-use rules.
        const AcquirePollOutcome acq = poll_acquire(
            req.timeout, kAcquireQuantumNs, kTimeoutResult,
            [&](std::uint64_t host_to_ns) {
                return static_cast<int>(vkAcquireNextImageKHR(dev_vk, sc_vk, host_to_ns, sem,
                                                              signal_fence, &image_index));
            },
            is_aborting, steady_now_ns);
        if (acq.aborted) {
            // Session tearing down (app peer-closed). The internal fence (if created) may still be
            // pending -- do NOT destroy it (invalid); process teardown owns recovery.
            aborted = true;
            return;
        }
        result = observe_device_result(it->second.device, static_cast<VkResult>(acq.result),
                                       "vkAcquireNextImageKHR");
        if (internal != VK_NULL_HANDLE) {
            // The wait guarantees the returned image is CPU-ready for the immediate debug-clear
            // submit. Poll with the same bounded, abort-aware quantum instead of an unbounded wait;
            // only destroy the fence once it has SIGNALED (not pending). On abort, leave it.
            if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) {
                VkResult wr = VK_TIMEOUT;
                while (!is_aborting()) {
                    wr = observe_device_result(
                        it->second.device,
                        vkWaitForFences(dev_vk, 1, &internal, VK_TRUE, kAcquireQuantumNs),
                        "vkWaitForFences (acquire internal)");
                    if (wr != VK_TIMEOUT) {
                        break;
                    }
                }
                if (is_aborting()) {
                    aborted = true;
                    return; // leave the possibly-pending internal fence undestroyed
                }
                if (wr != VK_SUCCESS) {
                    if (wr == VK_ERROR_DEVICE_LOST) {
                        result = wr;
                        return; // lost-device teardown abandons the private fence
                    }
                    setup_err = "vkWaitForFences failed";
                }
            }
            vkDestroyFence(dev_vk, internal, nullptr);
        }
    });
    if (!routed) {
        resp.ok = false;
        resp.reason = "window thread unavailable";
        return resp;
    }
    if (aborted) {
        // The guest is gone; return a clean fault. serve_vulkan_rpc unwinds when its send/recv
        // fails on the dead peer, so the dispatcher returns and teardown proceeds.
        resp.ok = false;
        resp.reason = "session aborting";
        resp.result = static_cast<int>(VK_ERROR_OUT_OF_DATE_KHR);
        return resp;
    }
    if (!setup_err.empty()) {
        resp.ok = false;
        resp.reason = setup_err;
        return resp;
    }
    VKR_INFO(kComponent) << "acquire result=" << static_cast<int>(result)
                         << " image=" << image_index;
    resp.ok = true;
    resp.reason = "ok";
    resp.result = static_cast<int>(result);
    resp.image_index = static_cast<int>(image_index);
    return resp;
}

std::vector<std::unique_lock<std::mutex>>
RealVulkanBackend::lock_surface_slots(const std::set<std::uint64_t>& surfaces) {
    std::vector<std::unique_lock<std::mutex>> locks;
    for (const std::uint64_t s : surfaces) { // std::set iterates in ascending order
        const auto surf = surfaces_.find(s);
        if (surf != surfaces_.end() && surf->second.slot != nullptr) {
            locks.emplace_back(surf->second.slot->slot_lock());
        }
    }
    return locks;
}

vkrpc::StatusResponse
RealVulkanBackend::record_command_buffer(const vkrpc::RecordCommandBufferRequest& req) {
    vkrpc::StatusResponse resp;
    // the validate-then-record structure times its two passes separately --
    // resolve+validate below, the vkBegin/vkCmd*/vkEnd replay inside do_record (on the thread
    // that runs it, so window-thread dispatch wait is excluded). Zero clock reads when off.
    vkrpc::RpcProfile* prof = vkrpc::profile_if_enabled();
    const std::uint64_t t_validate0 = prof != nullptr ? vkrpc::profile_clock_us() : 0;
    const auto cb = command_buffers_.find(req.command_buffer);
    if (cb == command_buffers_.end()) {
        resp.ok = false;
        resp.reason = "unknown command buffer handle";
        return resp;
    }
    const auto dev = devices_.find(cb->second.device);
    if (dev == devices_.end() || dev->second.vk == VK_NULL_HANDLE) {
        resp.ok = false;
        resp.reason = "command buffer has no live device";
        return resp;
    }
    // Validate the WHOLE stream before recording (validate-then-record, atomic): each
    // referenced image must resolve and live on the command buffer's device, and each
    // command's fields must be well-formed. Collect the (image, command) pairs to emit and
    // the referenced swapchain surfaces (whose slot locks serialize the recording).
    struct Resolved {
        const vkrpc::RecordedCommand* cmd = nullptr;
        VkImage image = VK_NULL_HANDLE;                // image commands
        VkRenderPass render_pass = VK_NULL_HANDLE;     // begin_render_pass
        VkFramebuffer framebuffer = VK_NULL_HANDLE;    // begin_render_pass
        VkPipeline pipeline = VK_NULL_HANDLE;          // bind_pipeline
        std::vector<VkBuffer> vbufs;                   // bind_vertex_buffers (resolved)
        std::vector<VkDeviceSize> voffs;               // bind_vertex_buffers offsets
        VkPipelineLayout desc_layout = VK_NULL_HANDLE; // bind_descriptor_sets
        std::vector<VkDescriptorSet> desc_sets;        // bind_descriptor_sets (resolved)
        VkBuffer src_buffer = VK_NULL_HANDLE;          // copy_buffer_to_image
        VkBuffer count_buffer = VK_NULL_HANDLE;        // indirect-count GPU draw-count source
        VkBuffer dst_buffer = VK_NULL_HANDLE;          // (GL/zink): copy_buffer dest
        VkBuffer index_buffer = VK_NULL_HANDLE;        // (GL/zink): bind_index_buffer
        VkDeviceSize index_offset = 0;
        VkIndexType index_type = VK_INDEX_TYPE_UINT16;
        vkrpc::CoreIndirectDrawArgs indirect_draw; // validated scalar/legacy-normalized payload
        vkrpc::CoreIndirectCountDrawArgs indirect_count_draw; // validated dedicated payload
        VkImage dst_image = VK_NULL_HANDLE;                   // (GL/zink): blit_image dest
        std::vector<VkDeviceSize> vsizes;        // (GL/zink): bind_transform_feedback sizes
        VkQueryPool query_pool = VK_NULL_HANDLE; // query pools: reset/begin/end/write_timestamp
        // (dynamic rendering): the resolved host image views for a begin_rendering command,
        // in the same order the wire carried them (color[0..n-1], depth?, stencil?); a null
        // attachment stays VK_NULL_HANDLE. replay rebuilds VkRenderingInfo from r.cmd's enums +
        // these views.
        std::vector<VkImageView> rendering_views;
        // Native imageless framebuffer: resolved host attachment views chained into
        // VkRenderPassBeginInfo during replay.
        std::vector<VkImageView> begin_attachment_views;
        VkExtent2D begin_framebuffer_extent{};
        // (sync1 command events): set_event/reset_event resolve the single VkEvent;
        // wait_events resolves the event SET plus its buffer/image barrier handles (host), in wire
        // order. The masks/layouts/ranges stay in r.cmd->args_u64 and are rebuilt at emit.
        VkEvent event = VK_NULL_HANDLE;
        std::vector<VkEvent> wait_events;
        std::vector<VkBuffer> wait_buffers;
        std::vector<VkImage> wait_images;
        // (sync2 commands): s2_events = the resolved event set (set/reset_event2 -> 1,
        // wait_events2 -> N). s2_dep_buffers[i] / s2_dep_images[i] = the host buffer/image handles
        // for deps2[i]'s barriers, in order (barrier2/set_event2 have one dependency; wait_events2
        // has one per event). write_timestamp2 reuses `query_pool` above. Masks/layouts/ranges stay
        // in r.cmd->deps2 and are rebuilt at emit (QFI normalized to IGNORED there).
        std::vector<VkEvent> s2_events;
        std::vector<std::vector<VkBuffer>> s2_dep_buffers;
        std::vector<std::vector<VkImage>> s2_dep_images;
    };
    std::vector<Resolved> resolved;
    resolved.reserve(req.commands.size());
    std::set<std::uint64_t> referenced_surfaces;
    std::set<std::uint64_t> referenced_swapchains;
    std::set<std::uint64_t> referenced_draw_objects; // render pass/framebuffer/view/pipeline
    const std::uint64_t device = cb->second.device;
    // Draw state machine: identical to the mock oracle's, resolving to real Vk handles. Render-pass
    // scope + framebuffer/pipeline compatibility + the required dynamic VIEWPORT+SCISSOR are
    // checked here so we never emit a host vkCmd* the validation layer would reject.
    // Render-scope state machine (dynamic rendering): ONE active scope with a
    // KIND, so render passes and dynamic rendering share the draw / copy / transform-feedback /
    // conditional-rendering / end-of-stream rules (mock == real). RenderPass and DynamicRendering
    // mutually exclude; each `end` requires its own kind.
    enum class RenderScope { None, RenderPass, DynamicRendering };
    RenderScope active_scope = RenderScope::None;
    VkFormat active_rp_format = VK_FORMAT_UNDEFINED;
    VkFormat active_rp_depth_format = VK_FORMAT_UNDEFINED; // pipeline-compat key
    // The active dynamic-rendering scope's per-attachment formats (from its attachment views) +
    // view mask -- the draw-time compatibility key a bound DR pipeline must match.
    std::vector<VkFormat> active_dr_color_formats;
    VkFormat active_dr_depth_format = VK_FORMAT_UNDEFINED;
    VkFormat active_dr_stencil_format = VK_FORMAT_UNDEFINED;
    std::uint32_t active_dr_view_mask = 0;
    // Compute: GRAPHICS and COMPUTE are SEPARATE bind points --
    // binding at one never disturbs the other. Index 0 = GRAPHICS, 1 = COMPUTE.
    std::uint64_t bound_pipeline_by_point[2] = {0, 0};
    bool viewport_set = false;
    bool scissor_set = false;
    bool index_bound = false;            // (GL/zink): set by bind_index_buffer
    std::set<int> bound_vertex_bindings; // binding indices set by bind_vertex_buffers
    // (GL/zink): faithful binding accumulates sets BY INDEX across binds (zink binds at any
    // firstSet, incrementally). Draw-readiness checks the pipeline layout's declared set indices
    // are all present + draw-ready here, while the bind itself just forwards. Per bind point
    // [0] = GRAPHICS, [1] = COMPUTE.
    std::map<std::uint32_t, std::uint64_t> bound_sets_by_index_by_point[2];
    // (GL/zink): transform-feedback + conditional-rendering scope state. XFB lives strictly
    // inside one render pass (begin/end balanced, the pass must not end mid-XFB). Conditional
    // rendering is balanced with SCOPE SYMMETRY (spec: begun outside a render pass -> ends
    // outside; begun inside -> ends inside the same pass) and must not survive the stream.
    bool xfb_active = false;
    bool cond_render_active = false;
    bool cond_render_began_in_rp = false;
    // Query pools: the (pool,query) pairs begun-but-not-ended in this command buffer. A query must
    // begin then end within the CB (no double-begin of the same index; balanced at stream end).
    std::set<std::pair<std::uint64_t, std::uint32_t>> active_queries;
    for (const vkrpc::RecordedCommand& c : req.commands) {
        // one lookup, then integer dispatch (the string-compare chain was a measured
        // slice of per-command validate time at ETR volumes). Unknown -> the final else, as ever.
        const vkrpc::CmdKind k = vkrpc::recorded_command_kind(c);
        if (k == vkrpc::CmdKind::PipelineBarrier || k == vkrpc::CmdKind::ClearColorImage) {
            const auto img = images_.find(c.image);
            if (img == images_.end() || img->second.device != device) {
                resp.ok = false;
                resp.reason = "command references an image not on the command buffer's device";
                return resp;
            }
            // An app-created image must be bound to memory before any command targets it (mirrors
            // the image-view guard -- a barrier/clear against an unbound VkImage
            // is a fail-closed worker-boundary violation). Swapchain images are swapchain-bound.
            if (img->second.app_created && img->second.bound_memory == 0) {
                resp.ok = false;
                resp.reason = "command references an app image not bound to memory";
                return resp;
            }
            if (!validate_recorded_command(c, img->second.transfer_dst, resp.reason)) {
                resp.ok = false;
                return resp;
            }
            // an app-image (texture) barrier is restricted to the upload transition
            // allowlist + a single-subresource range (the shared helper -> mock == real). A
            // swapchain-image barrier keeps the proven clear-frame form.
            if (k == vkrpc::CmdKind::PipelineBarrier && img->second.app_created &&
                !vkrpc::app_image_barrier_ok(c, img->second.usage, resp.reason)) {
                resp.ok = false;
                return resp;
            }
            Resolved r;
            r.cmd = &c;
            r.image = img->second.vk;
            resolved.push_back(r);
            referenced_surfaces.insert(img->second.surface);
            referenced_swapchains.insert(img->second.swapchain);
            // The recorded command bakes this image handle: destroying the image
            // must invalidate this command buffer. An app image has surface == swapchain == 0, so
            // the generic referenced-object set is what makes destroy_image invalidate it.
            referenced_draw_objects.insert(c.image);
        } else if (k == vkrpc::CmdKind::BeginRenderPass) {
            if (active_scope != RenderScope::None) {
                resp.ok = false;
                resp.reason = "begin_render_pass inside an active render pass";
                return resp;
            }
            const auto rp = render_passes_.find(c.render_pass);
            if (rp == render_passes_.end() || rp->second.device != device) {
                resp.ok = false;
                resp.reason = "begin_render_pass references a render pass not on the device";
                return resp;
            }
            const auto fb = framebuffers_.find(c.framebuffer);
            if (fb == framebuffers_.end() || fb->second.device != device) {
                resp.ok = false;
                resp.reason = "begin_render_pass references a framebuffer not on the device";
                return resp;
            }
            // Render-pass compatibility via the framebuffer's SNAPSHOT (taken at create, from
            // the then-live creating pass): Vulkan allows beginning a framebuffer with any
            // COMPATIBLE pass even after the creating pass was destroyed -- mpv/libplacebo does
            // exactly this (a temporary creation pass, destroyed at once), and the old
            // resolve-the-creating-pass-by-handle check rejected every one of its frames.
            if (fb->second.compat_color_format != rp->second.color_format ||
                fb->second.compat_depth_format != rp->second.depth_format) {
                resp.ok = false;
                resp.reason =
                    "begin_render_pass framebuffer is incompatible with the render pass (fb "
                    "color=" +
                    std::to_string(fb->second.compat_color_format) +
                    " depth=" + std::to_string(fb->second.compat_depth_format) +
                    " vs pass color=" + std::to_string(rp->second.color_format) +
                    " depth=" + std::to_string(rp->second.depth_format) + ")";
                return resp;
            }
            // begin_render_pass depth clear: a depth render pass
            // must carry an explicit depth clear in [0,1]; a color-only pass must not. This guard
            // applies to the LEGACY named-field path (vkcube/mock, args_i64 empty). The
            // faithful path (args_i64 non-empty) carries the EXACT clear array zink supplied -- the
            // render pass's own loadOp decides whether each clear is used, so no count/order guess
            // is needed.
            if (c.args_i64.empty()) {
                if (rp->second.depth_format != VK_FORMAT_UNDEFINED) {
                    if (!c.has_depth_clear || c.depth_clear < 0.0 || c.depth_clear > 1.0) {
                        resp.ok = false;
                        resp.reason =
                            "begin_render_pass on a depth render pass needs a depth clear in [0,1]";
                        return resp;
                    }
                } else if (c.has_depth_clear) {
                    resp.ok = false;
                    resp.reason = "begin_render_pass carries a depth clear but the render pass has "
                                  "no depth attachment";
                    return resp;
                }
            }
            if (c.render_area_x < 0 || c.render_area_y < 0 || c.render_area_w <= 0 ||
                c.render_area_h <= 0 ||
                c.render_area_x + c.render_area_w > static_cast<int>(fb->second.extent.width) ||
                c.render_area_y + c.render_area_h > static_cast<int>(fb->second.extent.height)) {
                resp.ok = false;
                resp.reason = "begin_render_pass render area is empty or outside the framebuffer";
                return resp;
            }
            VkFramebuffer fb_vk = VK_NULL_HANDLE;
            std::vector<VkImageView> begin_attachment_views;
            VkExtent2D begin_host_extent = fb->second.host_extent;
            if (fb->second.imageless) {
                // (GL/zink): build (or reuse a cached) regular framebuffer from the views
                // the begin supplied (args_u64). Caching avoids destroying a framebuffer a prior
                // submit may still be using.
                if (c.args_u64.empty()) {
                    resp.ok = false;
                    resp.reason = "imageless begin_render_pass carries no attachment views";
                    return resp;
                }
                // MRT: the begin must supply exactly the attachment count the
                // framebuffer declared at create -- a mismatch is OUR named rejection, not a
                // driver mystery. attachment_count 0 = a legacy/unknown declare (old ICD): the
                // driver stays the gate there.
                if (fb->second.attachment_count > 0 &&
                    c.args_u64.size() != static_cast<std::size_t>(fb->second.attachment_count)) {
                    resp.ok = false;
                    resp.reason = "imageless begin_render_pass view count does not match the "
                                  "framebuffer's declared attachment count";
                    return resp;
                }
                std::vector<VkImageView> views;
                VkExtent2D smallest_view_extent{UINT32_MAX, UINT32_MAX};
                for (const std::uint64_t vh : c.args_u64) {
                    const auto v = image_views_.find(vh);
                    if (v == image_views_.end() || v->second.device != device) {
                        resp.ok = false;
                        resp.reason = "imageless begin_render_pass view not on the device";
                        return resp;
                    }
                    views.push_back(v->second.vk);
                    referenced_draw_objects.insert(vh);
                    const auto vimg = images_.find(v->second.image);
                    if (vimg != images_.end()) {
                        smallest_view_extent.width =
                            std::min(smallest_view_extent.width, vimg->second.extent.width);
                        smallest_view_extent.height =
                            std::min(smallest_view_extent.height, vimg->second.extent.height);
                        referenced_surfaces.insert(vimg->second.surface);
                        referenced_swapchains.insert(vimg->second.swapchain);
                        // Required-feature audit: an imageless framebuffer's
                        // begin supplies its views HERE, so the multiview layer-sufficiency check
                        // lands here (the concrete-view path checks at create). Explicit named
                        // rejection + mock-shaped parity; the host vkCreateFramebuffer below is the
                        // authoritative VUID-02531 enforcer.
                        if (static_cast<int>(vimg->second.array_layers) <
                            vkrpc::multiview_required_layers(fb->second.view_mask)) {
                            resp.ok = false;
                            resp.reason =
                                "imageless begin_render_pass view has too few array layers "
                                "for the render pass viewMask (multiview)";
                            return resp;
                        }
                    }
                }
                if (!fb->second.native_imageless &&
                    (smallest_view_extent.width < fb->second.extent.width ||
                     smallest_view_extent.height < fb->second.extent.height)) {
                    VKR_WARN(kComponent)
                        << "imageless emulation extent mismatch: framebuffer=" << c.framebuffer
                        << " declared=" << fb->second.extent.width << "x"
                        << fb->second.extent.height << " render-area=" << c.render_area_w << "x"
                        << c.render_area_h << " smallest-view=" << smallest_view_extent.width << "x"
                        << smallest_view_extent.height;
                    begin_host_extent.width =
                        std::min(begin_host_extent.width, smallest_view_extent.width);
                    begin_host_extent.height =
                        std::min(begin_host_extent.height, smallest_view_extent.height);
                }
                if (fb->second.native_imageless) {
                    fb_vk = fb->second.vk;
                    begin_attachment_views = views;
                } else {
                    const auto cached = fb->second.imageless_cache.find(c.args_u64);
                    if (cached != fb->second.imageless_cache.end()) {
                        fb_vk = cached->second;
                    } else {
                        VkFramebufferCreateInfo fci{};
                        fci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
                        fci.renderPass = rp->second.vk;
                        fci.attachmentCount = static_cast<std::uint32_t>(views.size());
                        fci.pAttachments = views.data();
                        fci.width = begin_host_extent.width;
                        fci.height = begin_host_extent.height;
                        fci.layers = fb->second.layers;
                        VkFramebuffer built = VK_NULL_HANDLE;
                        const auto bdev = devices_.find(device);
                        const VkDevice bdev_vk =
                            bdev != devices_.end() ? bdev->second.vk : VK_NULL_HANDLE;
                        const VkResult fr = vkCreateFramebuffer(bdev_vk, &fci, nullptr, &built);
                        if (fr != VK_SUCCESS || built == VK_NULL_HANDLE) {
                            resp.ok = false;
                            resp.reason = "imageless framebuffer build failed (VkResult " +
                                          std::to_string(fr) + ")";
                            return resp;
                        }
                        fb->second.imageless_cache.emplace(c.args_u64, built);
                        fb_vk = built;
                    }
                }
            } else {
                // The framebuffer's attachment views must still be live -- a view destroyed after
                // the framebuffer was built leaves the VkFramebuffer holding a dangling
                // VkImageView, so recording over it is rejected (robustness).
                // MRT: a positional framebuffer checks EVERY carried view; the legacy scalar shape
                // keeps its color(+depth) pair.
                if (!fb->second.attachment_views.empty()) {
                    for (const std::uint64_t vh : fb->second.attachment_views) {
                        const auto v = image_views_.find(vh);
                        if (v == image_views_.end()) {
                            resp.ok = false;
                            resp.reason =
                                "begin_render_pass framebuffer attachment view has been destroyed";
                            return resp;
                        }
                        referenced_draw_objects.insert(vh);
                        const auto vimg = images_.find(v->second.image);
                        if (vimg != images_.end()) {
                            referenced_surfaces.insert(vimg->second.surface);
                            referenced_swapchains.insert(vimg->second.swapchain);
                        }
                    }
                    fb_vk = fb->second.vk;
                } else {
                    const auto iv = image_views_.find(fb->second.image_view);
                    if (iv == image_views_.end()) {
                        resp.ok = false;
                        resp.reason = "begin_render_pass framebuffer color view has been destroyed";
                        return resp;
                    }
                    if (fb->second.depth_image_view != 0 &&
                        image_views_.find(fb->second.depth_image_view) == image_views_.end()) {
                        resp.ok = false;
                        resp.reason = "begin_render_pass framebuffer depth view has been destroyed";
                        return resp;
                    }
                    const auto img = images_.find(iv->second.image);
                    if (img != images_.end()) {
                        referenced_surfaces.insert(img->second.surface);
                        referenced_swapchains.insert(img->second.swapchain);
                    }
                    // The recorded stream bakes these host handles -- destroying any invalidates
                    // it.
                    referenced_draw_objects.insert(fb->second.image_view);
                    if (fb->second.depth_image_view != 0) {
                        referenced_draw_objects.insert(fb->second.depth_image_view);
                    }
                    fb_vk = fb->second.vk;
                }
            }
            referenced_draw_objects.insert(c.render_pass);
            referenced_draw_objects.insert(c.framebuffer);
            Resolved r;
            r.cmd = &c;
            r.render_pass = rp->second.vk;
            r.framebuffer = fb_vk;
            r.begin_attachment_views = std::move(begin_attachment_views);
            r.begin_framebuffer_extent = begin_host_extent;
            resolved.push_back(r);
            active_scope = RenderScope::RenderPass;
            active_rp_format = rp->second.color_format;
            active_rp_depth_format = rp->second.depth_format;
        } else if (k == vkrpc::CmdKind::EndRenderPass) {
            if (active_scope != RenderScope::RenderPass) {
                resp.ok = false;
                resp.reason = "end_render_pass without an active render pass";
                return resp;
            }
            if (xfb_active) {
                resp.ok = false;
                resp.reason = "end_render_pass with active transform feedback";
                return resp;
            }
            if (cond_render_active && cond_render_began_in_rp) {
                resp.ok = false;
                resp.reason =
                    "end_render_pass with conditional rendering begun inside the render pass";
                return resp;
            }
            active_scope = RenderScope::None;
            Resolved r;
            r.cmd = &c;
            resolved.push_back(r);
        } else if (k == vkrpc::CmdKind::BindPipeline) {
            const auto pp = pipelines_.find(c.pipeline);
            if (pp == pipelines_.end() || pp->second.device != device) {
                resp.ok = false;
                resp.reason = "bind_pipeline references a pipeline not on the device";
                return resp;
            }
            // Compute: args_i64[0] = bindPoint (absent = GRAPHICS, so old recordings
            // replay unchanged); it must MATCH the pipeline's kind (mock == real).
            const long long bp = c.args_i64.empty() ? 0 : c.args_i64[0];
            if (bp != 0 && bp != 1) {
                resp.ok = false;
                resp.reason = "bind_pipeline bind point must be GRAPHICS or COMPUTE";
                return resp;
            }
            if ((bp == 1) != pp->second.compute) {
                resp.ok = false;
                resp.reason = "bind_pipeline bind point does not match the pipeline's kind";
                return resp;
            }
            bound_pipeline_by_point[bp] = c.pipeline;
            referenced_draw_objects.insert(c.pipeline);
            Resolved r;
            r.cmd = &c;
            r.pipeline = pp->second.vk;
            resolved.push_back(r);
        } else if (k == vkrpc::CmdKind::SetViewport) {
            if (!(c.vp_w > 0.0) || c.vp_h == 0.0) {
                resp.ok = false;
                resp.reason = "set_viewport requires a positive width and non-zero height";
                return resp;
            }
            viewport_set = true;
            Resolved r;
            r.cmd = &c;
            resolved.push_back(r);
        } else if (k == vkrpc::CmdKind::SetScissor) {
            if (c.sc_w < 0 || c.sc_h < 0) {
                resp.ok = false;
                resp.reason = "set_scissor requires a non-negative extent";
                return resp;
            }
            scissor_set = true;
            Resolved r;
            r.cmd = &c;
            resolved.push_back(r);
        } else if (k == vkrpc::CmdKind::BindVertexBuffers) {
            // firstBinding 0, a non-empty equal-length buffers/offsets pair, each a
            // VERTEX_BUFFER on the device. The host VkBuffers are baked into the recording.
            if (c.first_binding != 0 || c.vertex_buffers.empty() ||
                c.vertex_buffers.size() != c.vertex_buffer_offsets.size()) {
                resp.ok = false;
                resp.reason = "bind_vertex_buffers must be firstBinding 0 with matching buffers/"
                              "offsets";
                return resp;
            }
            Resolved r;
            r.cmd = &c;
            for (std::size_t i = 0; i < c.vertex_buffers.size(); ++i) {
                const auto bf = buffers_.find(c.vertex_buffers[i]);
                if (bf == buffers_.end() || bf->second.device != device) {
                    resp.ok = false;
                    resp.reason = "bind_vertex_buffers references a buffer not on the device";
                    return resp;
                }
                r.vbufs.push_back(bf->second.vk);
                r.voffs.push_back(static_cast<VkDeviceSize>(c.vertex_buffer_offsets[i]));
                bound_vertex_bindings.insert(c.first_binding + static_cast<int>(i));
                referenced_draw_objects.insert(c.vertex_buffers[i]); // destroy invalidates this CB
            }
            resolved.push_back(std::move(r));
        } else if (k == vkrpc::CmdKind::BindDescriptorSets) {
            // (GL/zink): FAITHFUL forwarding. The layout + each set must be live on the
            // device (else the host call would dereference a stale handle); beyond that the bind is
            // forwarded verbatim at any firstSet, with dynamic offsets. Sets accumulate BY INDEX
            // ([firstSet, firstSet+count)) so a draw can check the bound pipeline layout's declared
            // set indices are all present + draw-ready. The real driver is the binding-correctness
            // authority.
            const auto pl = pipeline_layouts_.find(c.desc_layout);
            if (pl == pipeline_layouts_.end() || pl->second.device != device) {
                resp.ok = false;
                resp.reason = "bind_descriptor_sets references a pipeline layout not on the device";
                return resp;
            }
            // Compute: the carried bind point (args_i64[0], absent = GRAPHICS)
            // selects WHICH bind point's accumulated set map this feeds (per-point).
            const long long dbp = c.args_i64.empty() ? 0 : c.args_i64[0];
            if (dbp != 0 && dbp != 1) {
                resp.ok = false;
                resp.reason = "bind_descriptor_sets bind point must be GRAPHICS or COMPUTE";
                return resp;
            }
            Resolved r;
            r.cmd = &c;
            r.desc_layout = pl->second.vk;
            const auto first_set = static_cast<std::uint32_t>(c.first_set < 0 ? 0 : c.first_set);
            for (std::size_t i = 0; i < c.descriptor_sets.size(); ++i) {
                const auto sset = descriptor_sets_.find(c.descriptor_sets[i]);
                if (sset == descriptor_sets_.end() || sset->second.device != device) {
                    resp.ok = false;
                    resp.reason = "bind_descriptor_sets references a set not on the device";
                    return resp;
                }
                r.desc_sets.push_back(sset->second.vk);
                referenced_draw_objects.insert(c.descriptor_sets[i]); // destroy invalidates this CB
                bound_sets_by_index_by_point[dbp][first_set + static_cast<std::uint32_t>(i)] =
                    c.descriptor_sets[i];
            }
            referenced_draw_objects.insert(c.desc_layout);
            resolved.push_back(std::move(r));
        } else if (k == vkrpc::CmdKind::BindIndexBuffer) {
            // (GL/zink): args_u64=[buffer, offset], args_i64=[indexType]. The buffer is
            // baked.
            if (c.args_u64.size() < 2 || c.args_i64.empty()) {
                resp.ok = false;
                resp.reason = "malformed bind_index_buffer";
                return resp;
            }
            const auto* index_buffer = vkrpc::live_device_buffer(buffers_, c.args_u64[0], device);
            if (index_buffer == nullptr || index_buffer->bound_memory == 0) {
                resp.ok = false;
                resp.reason = "bind_index_buffer references a buffer not live/bound on the device";
                return resp;
            }
            Resolved r;
            r.cmd = &c;
            r.index_buffer = index_buffer->vk;
            r.index_offset = static_cast<VkDeviceSize>(c.args_u64[1]);
            r.index_type = static_cast<VkIndexType>(c.args_i64[0]);
            resolved.push_back(r);
            referenced_draw_objects.insert(c.args_u64[0]);
            index_bound = true;
        } else if (k == vkrpc::CmdKind::PushConstants) {
            // (GL/zink): desc_layout = pipeline layout; args_i64=[stageFlags, offset,
            // size]; args_blob = the raw bytes. zink pushes shader uniforms this way.
            if (c.args_i64.size() < 3) {
                resp.ok = false;
                resp.reason = "malformed push_constants";
                return resp;
            }
            const auto pl = pipeline_layouts_.find(c.desc_layout);
            if (pl == pipeline_layouts_.end() || pl->second.device != device) {
                resp.ok = false;
                resp.reason = "push_constants references a pipeline layout not on the device";
                return resp;
            }
            const auto pc_size = static_cast<std::size_t>(c.args_i64[2]);
            if (c.args_blob.size() != pc_size) {
                resp.ok = false;
                resp.reason = "push_constants size does not match its payload";
                return resp;
            }
            Resolved r;
            r.cmd = &c;
            r.desc_layout = pl->second.vk;
            resolved.push_back(std::move(r));
            referenced_draw_objects.insert(c.desc_layout);
        } else if (k == vkrpc::CmdKind::Draw || k == vkrpc::CmdKind::DrawIndexed ||
                   k == vkrpc::CmdKind::DrawIndirectByteCount ||
                   k == vkrpc::CmdKind::DrawIndirect || k == vkrpc::CmdKind::DrawIndexedIndirect ||
                   k == vkrpc::CmdKind::DrawIndirectCount ||
                   k == vkrpc::CmdKind::DrawIndexedIndirectCount) {
            if (active_scope == RenderScope::None) {
                resp.ok = false;
                resp.reason = "draw outside an active render pass";
                return resp;
            }
            if ((k == vkrpc::CmdKind::DrawIndexed || k == vkrpc::CmdKind::DrawIndexedIndirect ||
                 k == vkrpc::CmdKind::DrawIndexedIndirectCount) &&
                !index_bound) {
                resp.ok = false;
                resp.reason =
                    k == vkrpc::CmdKind::DrawIndexed
                        ? "draw_indexed without a bound index buffer"
                        : (k == vkrpc::CmdKind::DrawIndexedIndirect
                               ? "draw_indexed_indirect without a bound index buffer"
                               : "draw_indexed_indirect_count without a bound index buffer");
                return resp;
            }
            // (GL/zink): the byte-count draw (glDrawTransformFeedback) shares full
            // draw-readiness below, plus its own gates: the extension must be enabled on the
            // device and the counter buffer live + bound.
            if (k == vkrpc::CmdKind::DrawIndirectByteCount) {
                if (dev->second.enabled_extensions.count(
                        VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME) == 0) {
                    resp.ok = false;
                    resp.reason =
                        "draw_indirect_byte_count requires VK_EXT_transform_feedback enabled "
                        "on the device";
                    return resp;
                }
            }
            if (bound_pipeline_by_point[0] == 0) {
                resp.ok = false;
                resp.reason = "draw without a bound graphics pipeline";
                return resp;
            }
            const auto pp = pipelines_.find(bound_pipeline_by_point[0]);
            if (pp == pipelines_.end()) {
                resp.ok = false;
                resp.reason = "draw's bound graphics pipeline is gone";
                return resp;
            }
            // Draw-time compatibility keys on the ACTIVE scope's KIND: a
            // dynamic-rendering scope compares the bound pipeline's declared formats + viewMask; a
            // render-pass scope compares the render pass's color/depth formats. A pipeline built
            // for the OTHER scope kind is rejected (mock == real).
            if (active_scope == RenderScope::DynamicRendering) {
                if (!pp->second.dynamic_rendering ||
                    pp->second.color_formats != active_dr_color_formats ||
                    pp->second.depth_format != active_dr_depth_format ||
                    pp->second.stencil_format != active_dr_stencil_format ||
                    pp->second.view_mask != active_dr_view_mask) {
                    resp.ok = false;
                    resp.reason = "draw's bound pipeline is incompatible with the active "
                                  "dynamic-rendering scope";
                    return resp;
                }
            } else {
                const auto pp_rp = render_passes_.find(pp->second.render_pass);
                if (pp->second.dynamic_rendering || pp_rp == render_passes_.end() ||
                    pp_rp->second.color_format != active_rp_format ||
                    pp_rp->second.depth_format != active_rp_depth_format) {
                    resp.ok = false;
                    resp.reason =
                        "draw's bound pipeline is incompatible with the active render pass";
                    return resp;
                }
            }
            if (!viewport_set || !scissor_set) {
                resp.ok = false;
                resp.reason = "draw without the required dynamic viewport/scissor set";
                return resp;
            }
            // a pipeline that declares vertex bindings needs each bound before the
            // draw.
            for (int vb = 0; vb < pp->second.vertex_binding_count; ++vb) {
                if (bound_vertex_bindings.count(vb) == 0) {
                    resp.ok = false;
                    resp.reason =
                        "draw with a pipeline that needs a vertex buffer, but the binding "
                        "was not bound";
                    return resp;
                }
            }
            // (GL/zink): every descriptor set BOUND for one of the pipeline layout's
            // declared indices must be draw-ready (live + not poisoned) -- this is the
            // descriptor-poison / freed-referent guard. A declared-but-unbound set is NOT rejected:
            // zink legitimately binds only the sets its shader samples (e.g. layout declares 3,
            // binds 0 + 2, leaves an unused set 1 unbound), and the worker cannot know from here
            // which sets the SPIR-V reads
            // -- the host driver (+ the validation layer) is the authority on a used-but-unbound
            // set.
            const auto pp_pl = pipeline_layouts_.find(pp->second.layout);
            if (pp_pl != pipeline_layouts_.end() && !pp_pl->second.set_layouts.empty()) {
                for (std::size_t si = 0; si < pp_pl->second.set_layouts.size(); ++si) {
                    const auto bound =
                        bound_sets_by_index_by_point[0].find(static_cast<std::uint32_t>(si));
                    if (bound == bound_sets_by_index_by_point[0].end()) {
                        continue; // declared-but-unbound: a set the shader may not use; the driver
                                  // decides
                    }
                    if (!descriptor_set_draw_ready(bound->second, device, resp.reason)) {
                        resp.ok = false;
                        return resp;
                    }
                }
            }
            Resolved r;
            r.cmd = &c;
            if (k == vkrpc::CmdKind::DrawIndexed) {
                if (c.args_i64.size() != 5) {
                    resp.ok = false;
                    resp.reason = "malformed draw_indexed command";
                    return resp;
                }
            } else if (k == vkrpc::CmdKind::DrawIndirectByteCount) {
                // args_u64=[counterBuffer]; args_i64=[instanceCount, firstInstance,
                // counterBufferOffset, counterOffset, vertexStride].
                if (c.args_u64.size() != 1 || c.args_i64.size() != 5 || c.args_i64[4] <= 0) {
                    resp.ok = false;
                    resp.reason = "malformed draw_indirect_byte_count command";
                    return resp;
                }
                const auto cbf = buffers_.find(c.args_u64[0]);
                if (cbf == buffers_.end() || cbf->second.device != device ||
                    cbf->second.bound_memory == 0) {
                    resp.ok = false;
                    resp.reason = "draw_indirect_byte_count counter buffer is not live/bound on "
                                  "the device";
                    return resp;
                }
                r.src_buffer = cbf->second.vk;
                referenced_draw_objects.insert(c.args_u64[0]);
            } else if (k == vkrpc::CmdKind::DrawIndirect ||
                       k == vkrpc::CmdKind::DrawIndexedIndirect) {
                vkrpc::CoreIndirectDrawArgs args;
                const char* why = "";
                if (!vkrpc::core_indirect_draw_args(c, args, &why)) {
                    resp.ok = false;
                    resp.reason = why;
                    return resp;
                }
                const auto* indirect_buffer =
                    vkrpc::live_device_buffer(buffers_, c.src_buffer, device);
                const vkrpc::IndirectBufferState buffer = vkrpc::indirect_buffer_state(
                    indirect_buffer, vkrpc::kBufferUsageIndirectBuffer);
                if (!vkrpc::core_indirect_draw_ok(buffer, args.offset, args.draw_count, args.stride,
                                                  k == vkrpc::CmdKind::DrawIndexedIndirect
                                                      ? vkrpc::kDrawIndexedIndirectCommandBytes
                                                      : vkrpc::kDrawIndirectCommandBytes,
                                                  dev->second.multi_draw_indirect_feature_enabled,
                                                  &why)) {
                    resp.ok = false;
                    resp.reason = why;
                    return resp;
                }
                r.src_buffer = indirect_buffer->vk;
                r.indirect_draw = args;
                referenced_draw_objects.insert(c.src_buffer);
            } else if (k == vkrpc::CmdKind::DrawIndirectCount ||
                       k == vkrpc::CmdKind::DrawIndexedIndirectCount) {
                vkrpc::CoreIndirectCountDrawArgs args;
                const char* why = "";
                if (!vkrpc::core_indirect_count_draw_args(c, args, &why)) {
                    resp.ok = false;
                    resp.reason = why;
                    return resp;
                }
                const auto* indirect_buffer =
                    vkrpc::live_device_buffer(buffers_, c.src_buffer, device);
                const auto* resolved_count_buffer =
                    vkrpc::live_device_buffer(buffers_, args.count_buffer, device);
                const vkrpc::IndirectBufferState buffer = vkrpc::indirect_buffer_state(
                    indirect_buffer, vkrpc::kBufferUsageIndirectBuffer);
                const vkrpc::IndirectBufferState count_buffer = vkrpc::indirect_buffer_state(
                    resolved_count_buffer, vkrpc::kBufferUsageIndirectBuffer);
                if (!vkrpc::core_indirect_count_draw_ok(
                        dev->second.draw_indirect_count_enabled, buffer, count_buffer, args.offset,
                        args.count_buffer_offset, args.max_draw_count, args.stride,
                        k == vkrpc::CmdKind::DrawIndexedIndirectCount
                            ? vkrpc::kDrawIndexedIndirectCommandBytes
                            : vkrpc::kDrawIndirectCommandBytes,
                        &why)) {
                    resp.ok = false;
                    resp.reason = why;
                    return resp;
                }
                r.src_buffer = indirect_buffer->vk;
                r.count_buffer = resolved_count_buffer->vk;
                r.indirect_count_draw = args;
                referenced_draw_objects.insert(c.src_buffer);
                referenced_draw_objects.insert(args.count_buffer);
            } else if (c.vertex_count < 0 || c.instance_count < 0 || c.first_vertex < 0 ||
                       c.first_instance < 0) {
                resp.ok = false;
                resp.reason = "malformed draw command";
                return resp;
            }
            resolved.push_back(r);
        } else if (k == vkrpc::CmdKind::Dispatch || k == vkrpc::CmdKind::DispatchIndirect) {
            // Compute: outside a render pass, with a COMPUTE pipeline bound; the
            // COMPUTE-point bound sets must be draw-ready (the draw treatment, per bind point).
            if (active_scope != RenderScope::None) {
                resp.ok = false;
                resp.reason = "dispatch inside an active render pass";
                return resp;
            }
            if (bound_pipeline_by_point[1] == 0) {
                resp.ok = false;
                resp.reason = "dispatch without a bound compute pipeline";
                return resp;
            }
            const auto pp = pipelines_.find(bound_pipeline_by_point[1]);
            if (pp == pipelines_.end()) {
                resp.ok = false;
                resp.reason = "dispatch's bound compute pipeline is gone";
                return resp;
            }
            const auto pp_pl = pipeline_layouts_.find(pp->second.layout);
            if (pp_pl != pipeline_layouts_.end() && !pp_pl->second.set_layouts.empty()) {
                for (std::size_t si = 0; si < pp_pl->second.set_layouts.size(); ++si) {
                    const auto bound =
                        bound_sets_by_index_by_point[1].find(static_cast<std::uint32_t>(si));
                    if (bound == bound_sets_by_index_by_point[1].end()) {
                        continue; // declared-but-unbound: the driver decides (the draw rule)
                    }
                    if (!descriptor_set_draw_ready(bound->second, device, resp.reason)) {
                        resp.ok = false;
                        return resp;
                    }
                }
            }
            Resolved r;
            r.cmd = &c;
            if (k == vkrpc::CmdKind::Dispatch) {
                // args_u64 = [x, y, z]; zero dimensions are a LEGAL no-op. The host driver is
                // the maxComputeWorkGroupCount authority.
                if (c.args_u64.size() != 3) {
                    resp.ok = false;
                    resp.reason = "malformed dispatch command";
                    return resp;
                }
            } else { // dispatch_indirect
                if (c.args_u64.size() != 1) {
                    resp.ok = false;
                    resp.reason = "malformed dispatch_indirect command";
                    return resp;
                }
                const std::uint64_t off = c.args_u64[0];
                const auto* indirect_buffer =
                    vkrpc::live_device_buffer(buffers_, c.src_buffer, device);
                const vkrpc::IndirectBufferState buffer = vkrpc::indirect_buffer_state(
                    indirect_buffer, vkrpc::kBufferUsageIndirectBuffer);
                const char* why = "";
                if (!vkrpc::dispatch_indirect_ok(buffer, off, &why)) {
                    resp.ok = false;
                    resp.reason = why;
                    return resp;
                }
                r.src_buffer = indirect_buffer->vk;
                referenced_draw_objects.insert(c.src_buffer);
            }
            resolved.push_back(r);
        } else if (k == vkrpc::CmdKind::MemoryBarrierGlobal) {
            // Compute: a GLOBAL VkMemoryBarrier. Outside-pass only.
            if (active_scope != RenderScope::None) {
                resp.ok = false;
                resp.reason = "memory_barrier inside an active render pass";
                return resp;
            }
            if (!vkrpc::valid_stage_mask_or_none(c.src_stage) ||
                !vkrpc::valid_stage_mask_or_none(c.dst_stage) ||
                !vkrpc::valid_access_mask(c.src_access) ||
                !vkrpc::valid_access_mask(c.dst_access)) {
                resp.ok = false;
                resp.reason = "malformed memory_barrier command";
                return resp;
            }
            Resolved r;
            r.cmd = &c;
            resolved.push_back(r);
        } else if (k == vkrpc::CmdKind::BufferMemoryBarrier) {
            // Compute: one VkBufferMemoryBarrier (offset/size in
            // args_u64; ~0 = WHOLE_SIZE; ownership transfers were rejected at the ICD).
            if (active_scope != RenderScope::None) {
                resp.ok = false;
                resp.reason = "buffer_memory_barrier inside an active render pass";
                return resp;
            }
            if (!vkrpc::valid_stage_mask_or_none(c.src_stage) ||
                !vkrpc::valid_stage_mask_or_none(c.dst_stage) ||
                !vkrpc::valid_access_mask(c.src_access) ||
                !vkrpc::valid_access_mask(c.dst_access) || c.args_u64.size() != 2) {
                resp.ok = false;
                resp.reason = "malformed buffer_memory_barrier command";
                return resp;
            }
            const auto bbf = buffers_.find(c.src_buffer);
            if (bbf == buffers_.end() || bbf->second.device != device ||
                bbf->second.bound_memory == 0) {
                resp.ok = false;
                resp.reason = "buffer_memory_barrier buffer is not live/bound on the device";
                return resp;
            }
            const std::uint64_t off = c.args_u64[0];
            const std::uint64_t sz = c.args_u64[1];
            if (off >= bbf->second.size || sz == 0 ||
                (sz != ~0ull && (off + sz > bbf->second.size || off + sz < off))) {
                resp.ok = false;
                resp.reason = "buffer_memory_barrier range is out of the buffer's bounds";
                return resp;
            }
            Resolved r;
            r.cmd = &c;
            r.src_buffer = bbf->second.vk;
            referenced_draw_objects.insert(c.src_buffer);
            resolved.push_back(r);
        } else if (k == vkrpc::CmdKind::CmdSetEvent || k == vkrpc::CmdKind::CmdResetEvent) {
            // sync1 set_event/reset_event -- event handle (args_u64[0]) + a non-zero
            // 32-bit stageMask (src_stage). Not inside a render pass instance. Resolve the event to
            // its host VkEvent (mock == real).
            if (active_scope != RenderScope::None) {
                resp.ok = false;
                resp.reason = "set_event/reset_event inside an active render pass";
                return resp;
            }
            if (c.args_u64.size() != 1 || !vkrpc::valid_stage_mask(c.src_stage)) {
                resp.ok = false;
                resp.reason = "malformed set_event/reset_event command";
                return resp;
            }
            const auto ev = leaves_.find(c.args_u64[0]);
            if (ev == leaves_.end() || ev->second.kind != LeafKind::Event ||
                ev->second.device != device) {
                resp.ok = false;
                resp.reason = "command references an event not on the command buffer's device";
                return resp;
            }
            Resolved r;
            r.cmd = &c;
            r.event = ev->second.event;
            referenced_draw_objects.insert(c.args_u64[0]);
            resolved.push_back(r);
        } else if (k == vkrpc::CmdKind::CmdWaitEvents) {
            // the atomic sync1 wait_events -- one command carrying the event SET plus the
            // three barrier arrays, flattened fixed-slot into args_u64 (header [eventCount,
            // memCount, bufCount, imgCount], then events, memory[2], buffer[5], image[10]). Resolve
            // the event set + buffer/image barrier handles to host handles (mock == real length
            // re-derivation).
            if (active_scope != RenderScope::None) {
                resp.ok = false;
                resp.reason = "wait_events inside an active render pass";
                return resp;
            }
            if (c.args_u64.size() < vkrpc::kWaitEventsHeaderSlots ||
                !vkrpc::valid_stage_mask(c.src_stage) || !vkrpc::valid_stage_mask(c.dst_stage)) {
                resp.ok = false;
                resp.reason = "malformed wait_events command";
                return resp;
            }
            const std::uint64_t ev_count = c.args_u64[0];
            const std::uint64_t mem_count = c.args_u64[1];
            const std::uint64_t buf_count = c.args_u64[2];
            const std::uint64_t img_count = c.args_u64[3];
            if (ev_count > vkrpc::kMaxWaitEventsBarriers ||
                mem_count > vkrpc::kMaxWaitEventsBarriers ||
                buf_count > vkrpc::kMaxWaitEventsBarriers ||
                img_count > vkrpc::kMaxWaitEventsBarriers || ev_count == 0) {
                resp.ok = false;
                resp.reason = "wait_events count is zero or exceeds the supported cap";
                return resp;
            }
            const std::uint64_t expect = vkrpc::kWaitEventsHeaderSlots + ev_count +
                                         mem_count * vkrpc::kWaitEventsMemorySlots +
                                         buf_count * vkrpc::kWaitEventsBufferSlots +
                                         img_count * vkrpc::kWaitEventsImageSlots;
            if (c.args_u64.size() != expect) {
                resp.ok = false;
                resp.reason = "wait_events payload length does not match its header counts";
                return resp;
            }
            Resolved r;
            r.cmd = &c;
            std::size_t cur = vkrpc::kWaitEventsHeaderSlots;
            for (std::uint64_t i = 0; i < ev_count; ++i) {
                const std::uint64_t ev_h = c.args_u64[cur++];
                const auto ev = leaves_.find(ev_h);
                if (ev == leaves_.end() || ev->second.kind != LeafKind::Event ||
                    ev->second.device != device) {
                    resp.ok = false;
                    resp.reason = "wait_events references an event not on the device";
                    return resp;
                }
                r.wait_events.push_back(ev->second.event);
                referenced_draw_objects.insert(ev_h);
            }
            for (std::uint64_t i = 0; i < mem_count; ++i) {
                const long long src_access = static_cast<long long>(c.args_u64[cur++]);
                const long long dst_access = static_cast<long long>(c.args_u64[cur++]);
                if (!vkrpc::valid_access_mask(src_access) ||
                    !vkrpc::valid_access_mask(dst_access)) {
                    resp.ok = false;
                    resp.reason = "wait_events memory barrier has an out-of-range access mask";
                    return resp;
                }
            }
            for (std::uint64_t i = 0; i < buf_count; ++i) {
                const long long src_access = static_cast<long long>(c.args_u64[cur++]);
                const long long dst_access = static_cast<long long>(c.args_u64[cur++]);
                const std::uint64_t buf_h = c.args_u64[cur++];
                const std::uint64_t off = c.args_u64[cur++];
                const std::uint64_t sz = c.args_u64[cur++];
                if (!vkrpc::valid_access_mask(src_access) ||
                    !vkrpc::valid_access_mask(dst_access)) {
                    resp.ok = false;
                    resp.reason = "wait_events buffer barrier has an out-of-range access mask";
                    return resp;
                }
                const auto bbf = buffers_.find(buf_h);
                if (bbf == buffers_.end() || bbf->second.device != device ||
                    bbf->second.bound_memory == 0) {
                    resp.ok = false;
                    resp.reason =
                        "wait_events buffer barrier buffer is not live/bound on the device";
                    return resp;
                }
                if (off >= bbf->second.size ||
                    (sz != ~0ull && (off + sz > bbf->second.size || off + sz < off)) || sz == 0) {
                    resp.ok = false;
                    resp.reason = "wait_events buffer barrier range is out of bounds";
                    return resp;
                }
                r.wait_buffers.push_back(bbf->second.vk);
                referenced_draw_objects.insert(buf_h);
            }
            for (std::uint64_t i = 0; i < img_count; ++i) {
                const long long src_access = static_cast<long long>(c.args_u64[cur++]);
                const long long dst_access = static_cast<long long>(c.args_u64[cur++]);
                cur += 2; // oldLayout, newLayout (rebuilt at emit)
                const std::uint64_t img_h = c.args_u64[cur++];
                const std::uint64_t aspect = c.args_u64[cur++];
                cur += 4; // baseMip, levelCount, baseLayer, layerCount (rebuilt at emit)
                if (!vkrpc::valid_access_mask(src_access) ||
                    !vkrpc::valid_access_mask(dst_access) || aspect < 1) {
                    resp.ok = false;
                    resp.reason = "wait_events image barrier is malformed";
                    return resp;
                }
                const auto img = images_.find(img_h);
                if (img == images_.end() || img->second.device != device) {
                    resp.ok = false;
                    resp.reason = "wait_events image barrier references an image not on the device";
                    return resp;
                }
                if (img->second.app_created && img->second.bound_memory == 0) {
                    resp.ok = false;
                    resp.reason = "wait_events image barrier references an app image not bound";
                    return resp;
                }
                r.wait_images.push_back(img->second.vk);
                referenced_draw_objects.insert(img_h);
            }
            resolved.push_back(r);
        } else if (k == vkrpc::CmdKind::PipelineBarrier2 || k == vkrpc::CmdKind::WriteTimestamp2 ||
                   k == vkrpc::CmdKind::CmdSetEvent2 || k == vkrpc::CmdKind::CmdResetEvent2 ||
                   k == vkrpc::CmdKind::CmdWaitEvents2) {
            // the VK_KHR_synchronization2 commands. Gate on the enabled
            // extension AND the feature (mock == real, the DR precedent). Vulkan 1.3 support: on an
            // honest vk13 device sync2 is CORE -- the feature alone admits it.
            if ((dev->second.enabled_extensions.count(VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME) ==
                     0 &&
                 !dev->second.vk13_device) ||
                !dev->second.synchronization2_feature_enabled) {
                resp.ok = false;
                resp.reason = "synchronization2 command requires the synchronization2 feature";
                return resp;
            }
            Resolved r;
            r.cmd = &c;
            // Structural validate (shared) + resolve each dependency's buffer/image handles to host
            // handles on this device, appending an entry (per dependency) to s2_dep_buffers/images.
            const auto resolve_dep2 = [&](const vkrpc::DependencyInfo2& d) -> bool {
                if (!vkrpc::validate_dependency_info2(d, resp.reason)) {
                    return false;
                }
                std::vector<VkBuffer> bufs;
                for (const vkrpc::BufferMemoryBarrier2& b : d.buffer) {
                    const auto bbf = buffers_.find(b.buffer);
                    if (bbf == buffers_.end() || bbf->second.device != device ||
                        bbf->second.bound_memory == 0) {
                        resp.reason = "sync2 buffer barrier buffer is not live/bound on the device";
                        return false;
                    }
                    if (b.size == 0 || b.offset >= bbf->second.size ||
                        (b.size != ~0ull &&
                         (b.offset + b.size > bbf->second.size || b.offset + b.size < b.offset))) {
                        resp.reason = "sync2 buffer barrier range is out of bounds";
                        return false;
                    }
                    bufs.push_back(bbf->second.vk);
                    referenced_draw_objects.insert(b.buffer);
                }
                std::vector<VkImage> imgs;
                for (const vkrpc::ImageMemoryBarrier2& im : d.image) {
                    const auto img = images_.find(im.image);
                    if (img == images_.end() || img->second.device != device) {
                        resp.reason = "sync2 image barrier references an image not on the device";
                        return false;
                    }
                    if (img->second.app_created && img->second.bound_memory == 0) {
                        resp.reason = "sync2 image barrier references an app image not bound";
                        return false;
                    }
                    imgs.push_back(img->second.vk);
                    referenced_draw_objects.insert(im.image);
                }
                r.s2_dep_buffers.push_back(std::move(bufs));
                r.s2_dep_images.push_back(std::move(imgs));
                return true;
            };
            const bool is_event2 = k == vkrpc::CmdKind::CmdSetEvent2 ||
                                   k == vkrpc::CmdKind::CmdResetEvent2 ||
                                   k == vkrpc::CmdKind::CmdWaitEvents2;
            if (is_event2 && active_scope != RenderScope::None) {
                resp.ok = false;
                resp.reason = "sync2 event command inside an active render pass";
                return resp;
            }
            if (k == vkrpc::CmdKind::PipelineBarrier2) {
                if (c.deps2.size() != 1 || !c.args_u64.empty() || !resolve_dep2(c.deps2[0])) {
                    resp.ok = false;
                    if (resp.reason.empty()) {
                        resp.reason = "malformed pipeline_barrier2 command";
                    }
                    return resp;
                }
            } else if (k == vkrpc::CmdKind::WriteTimestamp2) {
                if (c.args_u64.size() != 2 || c.args_i64.size() != 1 || !c.deps2.empty() ||
                    !vkrpc::valid_timestamp_stage2(c.args_u64[1])) {
                    resp.ok = false;
                    resp.reason = "malformed write_timestamp2 command";
                    return resp;
                }
                const auto qp = query_pools_.find(c.args_u64[0]);
                if (qp == query_pools_.end() || qp->second.device != device) {
                    resp.ok = false;
                    resp.reason = "write_timestamp2 references a query pool not on the device";
                    return resp;
                }
                if (c.args_i64[0] < 0 ||
                    static_cast<std::uint32_t>(c.args_i64[0]) >= qp->second.count) {
                    resp.ok = false;
                    resp.reason = "write_timestamp2 index out of bounds";
                    return resp;
                }
                r.query_pool = qp->second.vk;
                referenced_draw_objects.insert(c.args_u64[0]);
            } else if (k == vkrpc::CmdKind::CmdSetEvent2) {
                if (c.args_u64.size() != 1 || c.deps2.size() != 1) {
                    resp.ok = false;
                    resp.reason = "malformed set_event2 command";
                    return resp;
                }
                const auto ev = leaves_.find(c.args_u64[0]);
                if (ev == leaves_.end() || ev->second.kind != LeafKind::Event ||
                    ev->second.device != device) {
                    resp.ok = false;
                    resp.reason = "set_event2 references an event not on the device";
                    return resp;
                }
                if (!resolve_dep2(c.deps2[0])) {
                    resp.ok = false;
                    return resp;
                }
                r.s2_events.push_back(ev->second.event);
                referenced_draw_objects.insert(c.args_u64[0]);
            } else if (k == vkrpc::CmdKind::CmdResetEvent2) {
                if (c.args_u64.size() != 2 || !c.deps2.empty()) {
                    resp.ok = false;
                    resp.reason = "malformed reset_event2 command";
                    return resp;
                }
                const auto ev = leaves_.find(c.args_u64[0]);
                if (ev == leaves_.end() || ev->second.kind != LeafKind::Event ||
                    ev->second.device != device) {
                    resp.ok = false;
                    resp.reason = "reset_event2 references an event not on the device";
                    return resp;
                }
                r.s2_events.push_back(ev->second.event);
                referenced_draw_objects.insert(c.args_u64[0]);
            } else { // CmdWaitEvents2: N events paired with N dependencies.
                if (c.args_u64.empty() || c.deps2.size() != c.args_u64.size() ||
                    c.deps2.size() > vkrpc::kMaxSync2Dependencies) {
                    resp.ok = false;
                    resp.reason = "malformed wait_events2 command";
                    return resp;
                }
                for (std::size_t i = 0; i < c.args_u64.size(); ++i) {
                    const auto ev = leaves_.find(c.args_u64[i]);
                    if (ev == leaves_.end() || ev->second.kind != LeafKind::Event ||
                        ev->second.device != device) {
                        resp.ok = false;
                        resp.reason = "wait_events2 references an event not on the device";
                        return resp;
                    }
                    if (!resolve_dep2(c.deps2[i])) {
                        resp.ok = false;
                        return resp;
                    }
                    r.s2_events.push_back(ev->second.event);
                    referenced_draw_objects.insert(c.args_u64[i]);
                }
            }
            resolved.push_back(std::move(r));
        } else if (k == vkrpc::CmdKind::CopyBufferToImage) {
            // Faithful sub-region texture upload (widened from the one-full-image-region
            // subset, which blocked zink's glTexSubImage2D-class uploads):
            // args_i64=[dstImageLayout, regionCount, 13 per region] -- the copy_image_to_buffer
            // convention. Mock == real: a live TRANSFER_SRC staging buffer into a live
            // TRANSFER_DST COLOR app image, one of the two spec-legal dst layouts, outside any
            // render pass, and every region's subresource + bounds checked against the image's
            // tracked mips/layers/mip-level extents (overflow-safe u64 math). Both handles bake
            // into the CB.
            if (active_scope != RenderScope::None) {
                resp.ok = false;
                resp.reason = "copy_buffer_to_image inside an active render pass";
                return resp;
            }
            const auto cimg = images_.find(c.image);
            if (cimg == images_.end() || cimg->second.device != device ||
                !cimg->second.app_created) {
                resp.ok = false;
                resp.reason = "copy_buffer_to_image dest is not an app image on the device";
                return resp;
            }
            if (cimg->second.bound_memory == 0) {
                resp.ok = false;
                resp.reason = "copy_buffer_to_image dest image is not bound to memory";
                return resp;
            }
            if ((cimg->second.usage & static_cast<std::uint64_t>(vkrpc::kImageUsageTransferDst)) ==
                0) {
                resp.ok = false;
                resp.reason = "copy_buffer_to_image dest image lacks TRANSFER_DST usage";
                return resp;
            }
            // (No image-level COLOR-only gate: a depth/stencil destination image is legal now. The
            // per-region check below enforces the region's aspect is one the image actually has.)
            const auto sbf = buffers_.find(c.src_buffer);
            if (sbf == buffers_.end() || sbf->second.device != device) {
                resp.ok = false;
                resp.reason = "copy_buffer_to_image source buffer is not on the device";
                return resp;
            }
            if ((sbf->second.usage & vkrpc::kBufferUsageTransferSrc) == 0) {
                resp.ok = false;
                resp.reason = "copy_buffer_to_image source buffer lacks TRANSFER_SRC usage";
                return resp;
            }
            if (sbf->second.bound_memory == 0) {
                resp.ok = false;
                resp.reason = "copy_buffer_to_image source buffer is not bound to memory";
                return resp;
            }
            if (c.args_i64.size() < 2) {
                resp.ok = false;
                resp.reason = "malformed copy_buffer_to_image";
                return resp;
            }
            if (c.args_i64[0] != static_cast<long long>(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) &&
                c.args_i64[0] != static_cast<long long>(VK_IMAGE_LAYOUT_GENERAL)) {
                resp.ok = false;
                resp.reason = "copy_buffer_to_image dst layout must be TRANSFER_DST_OPTIMAL or "
                              "GENERAL";
                return resp;
            }
            // Overflow-safe shape check (division form, no multiply to wrap): exactly 13 i64 per
            // region after [layout, regionCount], and regionCount must match (>= 1).
            const auto cbti_regions = static_cast<std::size_t>(c.args_i64[1]);
            const std::size_t cbti_tail = c.args_i64.size() - 2;
            if (cbti_regions == 0 || cbti_tail % 13 != 0 || cbti_regions != cbti_tail / 13) {
                resp.ok = false;
                resp.reason = "copy_buffer_to_image region payload malformed";
                return resp;
            }
            for (std::size_t ri = 0; ri < cbti_regions; ++ri) {
                const long long* r = &c.args_i64[2 + ri * 13];
                const long long aspect = r[3], mip = r[4], base_layer = r[5], layers = r[6];
                const long long off_x = r[7], off_y = r[8], off_z = r[9];
                const long long ext_w = r[10], ext_h = r[11], ext_d = r[12];
                // Exactly one aspect bit, and it must be an aspect the destination image actually
                // has: COLOR into a color image; DEPTH or STENCIL into a depth/stencil image; a
                // combined-DS image takes DEPTH or STENCIL, one per region (never both in one
                // region -- rejected by the single-bit rule). The real vkCmdCopyBufferToImage stays
                // the authority on the format-specific byte packing.
                const bool single_aspect =
                    aspect == static_cast<long long>(VK_IMAGE_ASPECT_COLOR_BIT) ||
                    aspect == static_cast<long long>(VK_IMAGE_ASPECT_DEPTH_BIT) ||
                    aspect == static_cast<long long>(VK_IMAGE_ASPECT_STENCIL_BIT);
                if (!single_aspect ||
                    (aspect & static_cast<long long>(cimg->second.aspect)) != aspect) {
                    resp.ok = false;
                    resp.reason = "copy_buffer_to_image region aspect must be a single aspect "
                                  "present in the destination image";
                    return resp;
                }
                if (mip < 0 || mip >= static_cast<long long>(cimg->second.mip_levels)) {
                    resp.ok = false;
                    resp.reason = "copy_buffer_to_image region mip level is out of the image's "
                                  "range";
                    return resp;
                }
                if (base_layer < 0 || layers < 1 ||
                    base_layer + layers > static_cast<long long>(cimg->second.array_layers)) {
                    resp.ok = false;
                    resp.reason = "copy_buffer_to_image region layer range is out of the image's "
                                  "range";
                    return resp;
                }
                // The mip-level extent: max(1, base >> mip) per axis (2D images: depth stays 1).
                const std::uint64_t mip_w = std::max<std::uint64_t>(
                    1, static_cast<std::uint64_t>(cimg->second.extent.width) >>
                           static_cast<unsigned>(mip));
                const std::uint64_t mip_h = std::max<std::uint64_t>(
                    1, static_cast<std::uint64_t>(cimg->second.extent.height) >>
                           static_cast<unsigned>(mip));
                if (off_x < 0 || off_y < 0 || off_z != 0 || ext_w < 1 || ext_h < 1 || ext_d != 1 ||
                    static_cast<std::uint64_t>(off_x) + static_cast<std::uint64_t>(ext_w) > mip_w ||
                    static_cast<std::uint64_t>(off_y) + static_cast<std::uint64_t>(ext_h) > mip_h) {
                    resp.ok = false;
                    resp.reason = "copy_buffer_to_image region is out of the mip level's bounds";
                    return resp;
                }
                // Buffer-stride VUs: rowLength/imageHeight are 0 (tightly packed) or >= the
                // extent; bufferOffset must land inside the staging buffer (byte-exact packing is
                // format-dependent -- the host driver is the authoritative gate there).
                const long long buf_off = r[0], row_len = r[1], img_h = r[2];
                if (buf_off < 0 || static_cast<std::uint64_t>(buf_off) >= sbf->second.size ||
                    (row_len != 0 && row_len < ext_w) || (img_h != 0 && img_h < ext_h)) {
                    resp.ok = false;
                    resp.reason = "copy_buffer_to_image region buffer offset/stride is invalid";
                    return resp;
                }
            }
            Resolved r;
            r.cmd = &c;
            r.image = cimg->second.vk;
            r.src_buffer = sbf->second.vk;
            resolved.push_back(r);
            referenced_draw_objects.insert(c.src_buffer);
            referenced_draw_objects.insert(c.image);
        } else if (k == vkrpc::CmdKind::CopyBuffer) {
            // (GL/zink): faithful buffer->buffer copy. args_u64 = [src, dst, (srcOff,
            // dstOff, size) x regionCount]. Both buffers must be live + bound on the device; both
            // are baked (destroying either invalidates this CB). A transfer command -- outside any
            // render pass.
            if (active_scope != RenderScope::None) {
                resp.ok = false;
                resp.reason = "copy_buffer inside an active render pass";
                return resp;
            }
            if (c.args_u64.size() < 5 || (c.args_u64.size() - 2) % 3 != 0) {
                resp.ok = false;
                resp.reason = "copy_buffer payload malformed";
                return resp;
            }
            const auto sbf = buffers_.find(c.args_u64[0]);
            const auto dbf = buffers_.find(c.args_u64[1]);
            if (sbf == buffers_.end() || sbf->second.device != device ||
                sbf->second.bound_memory == 0 || dbf == buffers_.end() ||
                dbf->second.device != device || dbf->second.bound_memory == 0) {
                resp.ok = false;
                resp.reason = "copy_buffer references a buffer not live/bound on the device";
                return resp;
            }
            Resolved r;
            r.cmd = &c;
            r.src_buffer = sbf->second.vk;
            r.dst_buffer = dbf->second.vk;
            resolved.push_back(r);
            referenced_draw_objects.insert(c.args_u64[0]);
            referenced_draw_objects.insert(c.args_u64[1]);
        } else if (k == vkrpc::CmdKind::FillBuffer) {
            // faithful vkCmdFillBuffer (Mesa >= 25 zink: GL buffer clears). args_u64 =
            // [dstBuffer, dstOffset, size, data]; size VK_WHOLE_SIZE = fill-to-end. A transfer
            // command -- outside any render pass; spec 4-byte alignment on offset/size (mock ==
            // real validation).
            if (active_scope != RenderScope::None) {
                resp.ok = false;
                resp.reason = "fill_buffer inside an active render pass";
                return resp;
            }
            if (c.args_u64.size() != 4) {
                resp.ok = false;
                resp.reason = "fill_buffer payload malformed";
                return resp;
            }
            constexpr std::uint64_t kWholeSize = ~0ull; // VK_WHOLE_SIZE
            const std::uint64_t fb_offset = c.args_u64[1];
            const std::uint64_t fb_size = c.args_u64[2];
            if (fb_offset % 4 != 0 ||
                (fb_size != kWholeSize && (fb_size == 0 || fb_size % 4 != 0))) {
                resp.ok = false;
                resp.reason = "fill_buffer offset/size violates the 4-byte alignment rule";
                return resp;
            }
            const auto fbf = buffers_.find(c.args_u64[0]);
            if (fbf == buffers_.end() || fbf->second.device != device ||
                fbf->second.bound_memory == 0) {
                resp.ok = false;
                resp.reason = "fill_buffer references a buffer not live/bound on the device";
                return resp;
            }
            Resolved r;
            r.cmd = &c;
            r.dst_buffer = fbf->second.vk;
            resolved.push_back(r);
            referenced_draw_objects.insert(c.args_u64[0]);
        } else if (k == vkrpc::CmdKind::CopyImageToBuffer) {
            // (GL/zink): image->buffer readback (glReadPixels for offscreen PNG export).
            // args_u64=[srcImage, dstBuffer]; args_i64=[srcImageLayout, regionCount, 13 per
            // region]. A transfer command, outside any render pass. The source app image + the dest
            // buffer are baked (destroying either invalidates this CB).
            if (active_scope != RenderScope::None) {
                resp.ok = false;
                resp.reason = "copy_image_to_buffer inside an active render pass";
                return resp;
            }
            if (c.args_u64.size() < 2 || c.args_i64.size() < 2) {
                resp.ok = false;
                resp.reason = "malformed copy_image_to_buffer";
                return resp;
            }
            const auto cimg = images_.find(c.args_u64[0]);
            // The source may be an app image (must be memory-bound) OR a swapchain image
            // (swapchain- owned, no app-bound memory) -- glReadPixels off the default framebuffer
            // reads the swapchain image. Require a live same-device image; require bound memory
            // only for app images.
            if (cimg == images_.end() || cimg->second.device != device ||
                (cimg->second.app_created && cimg->second.bound_memory == 0)) {
                resp.ok = false;
                resp.reason = "copy_image_to_buffer source is not a live image on the device";
                return resp;
            }
            const auto dbf = buffers_.find(c.args_u64[1]);
            if (dbf == buffers_.end() || dbf->second.device != device ||
                dbf->second.bound_memory == 0) {
                resp.ok = false;
                resp.reason = "copy_image_to_buffer dest buffer is not live/bound on the device";
                return resp;
            }
            // Overflow-safe shape check (division form, no multiply to wrap -- the query-
            // range lesson): exactly 13 i64 per region after [layout, regionCount], and regionCount
            // must match. A negative regionCount casts huge -> mismatch. Mock == real.
            const auto region_count = static_cast<std::size_t>(c.args_i64[1]);
            const std::size_t tail = c.args_i64.size() - 2;
            if (tail % 13 != 0 || region_count != tail / 13) {
                resp.ok = false;
                resp.reason = "copy_image_to_buffer region payload malformed";
                return resp;
            }
            Resolved r;
            r.cmd = &c;
            r.image = cimg->second.vk;
            r.dst_buffer = dbf->second.vk;
            resolved.push_back(r);
            referenced_draw_objects.insert(c.args_u64[0]);
            referenced_draw_objects.insert(c.args_u64[1]);
            // developer proof: when VKRELAY2_DEBUG_DUMP_READBACK is set, queue this dest
            // buffer to dump as raw pixels after the next fence wait (region 0's imageExtent gives
            // the dimensions; the readback buffer is tightly packed RGBA/BGRA8).
            if (std::getenv("VKRELAY2_DEBUG_DUMP_READBACK") != nullptr && c.args_i64.size() >= 15) {
                PendingReadbackDump dump;
                dump.device = device;
                dump.buffer = c.args_u64[1];
                dump.width = static_cast<int>(c.args_i64[12]);  // region 0 imageExtent.width
                dump.height = static_cast<int>(c.args_i64[13]); // region 0 imageExtent.height
                pending_readback_dumps_.push_back(dump);
            }
        } else if (k == vkrpc::CmdKind::ClearAttachments) {
            // (GL/zink): faithful IN-RENDER-PASS scissored clear (zink emits it for a
            // scissored/partial glClear -- notably the first frame after a window resize).
            // Plain data, no handle resolution; requires an ACTIVE render pass (spec).
            // args_i64=[attachmentCount, rectCount, per-attachment (aspect, colorAttachment),
            // per-rect (x, y, w, h, baseArrayLayer, layerCount)]; args_u64 = 4 raw VkClearValue
            // words per attachment.
            if (active_scope == RenderScope::None) {
                resp.ok = false;
                resp.reason = "clear_attachments outside an active render pass";
                return resp;
            }
            if (c.args_i64.size() < 2) {
                resp.ok = false;
                resp.reason = "malformed clear_attachments";
                return resp;
            }
            const auto att_count = static_cast<std::size_t>(c.args_i64[0]);
            const auto rect_count = static_cast<std::size_t>(c.args_i64[1]);
            if (att_count == 0 || rect_count == 0 ||
                c.args_i64.size() != 2 + att_count * 2 + rect_count * 6 ||
                c.args_u64.size() != att_count * 4) {
                resp.ok = false;
                resp.reason = "clear_attachments payload malformed";
                return resp;
            }
            Resolved r;
            r.cmd = &c;
            resolved.push_back(r);
        } else if (k == vkrpc::CmdKind::BlitImage) {
            // (GL/zink): faithful image->image blit (glGenerateMipmap's scaled 2:1 copies,
            // direct glBlitFramebuffer). args_u64=[srcImage, dstImage]; args_i64=[srcLayout,
            // dstLayout, filter, regionCount, 20 per region]. A transfer command -- outside any
            // render pass; both images live on the device (an app image also bound to memory).
            if (active_scope != RenderScope::None) {
                resp.ok = false;
                resp.reason = "blit_image inside an active render pass";
                return resp;
            }
            if (c.args_u64.size() != 2 || c.args_i64.size() < 4) {
                resp.ok = false;
                resp.reason = "malformed blit_image";
                return resp;
            }
            const auto region_count = static_cast<std::size_t>(c.args_i64[3]);
            if (region_count == 0 || c.args_i64.size() != 4 + region_count * 20 ||
                (c.args_i64[2] != VK_FILTER_NEAREST && c.args_i64[2] != VK_FILTER_LINEAR)) {
                resp.ok = false;
                resp.reason = "blit_image payload malformed";
                return resp;
            }
            const auto simg = images_.find(c.args_u64[0]);
            const auto dimg = images_.find(c.args_u64[1]);
            if (simg == images_.end() || simg->second.device != device || dimg == images_.end() ||
                dimg->second.device != device) {
                resp.ok = false;
                resp.reason = "blit_image references an image not on the device";
                return resp;
            }
            if ((simg->second.app_created && simg->second.bound_memory == 0) ||
                (dimg->second.app_created && dimg->second.bound_memory == 0)) {
                resp.ok = false;
                resp.reason = "blit_image references an app image not bound to memory";
                return resp;
            }
            Resolved r;
            r.cmd = &c;
            r.image = simg->second.vk;
            r.dst_image = dimg->second.vk;
            resolved.push_back(r);
            for (const std::uint64_t ih : {c.args_u64[0], c.args_u64[1]}) {
                const auto& rec = images_.find(ih)->second;
                referenced_surfaces.insert(rec.surface);
                referenced_swapchains.insert(rec.swapchain);
                referenced_draw_objects.insert(ih);
            }
        } else if (k == vkrpc::CmdKind::CopyImage) {
            // (GL/zink): faithful unscaled image->image copy (glCopyTexSubImage2D-class).
            // args_u64=[srcImage, dstImage]; args_i64=[srcLayout, dstLayout, regionCount, 17 per
            // region]. Same gates as blit_image: a transfer command outside any render pass; both
            // images live on the device (an app image also bound to memory).
            if (active_scope != RenderScope::None) {
                resp.ok = false;
                resp.reason = "copy_image inside an active render pass";
                return resp;
            }
            if (c.args_u64.size() != 2 || c.args_i64.size() < 3) {
                resp.ok = false;
                resp.reason = "malformed copy_image";
                return resp;
            }
            const auto region_count = static_cast<std::size_t>(c.args_i64[2]);
            if (region_count == 0 || c.args_i64.size() != 3 + region_count * 17) {
                resp.ok = false;
                resp.reason = "copy_image payload malformed";
                return resp;
            }
            const auto simg = images_.find(c.args_u64[0]);
            const auto dimg = images_.find(c.args_u64[1]);
            if (simg == images_.end() || simg->second.device != device || dimg == images_.end() ||
                dimg->second.device != device) {
                resp.ok = false;
                resp.reason = "copy_image references an image not on the device";
                return resp;
            }
            if ((simg->second.app_created && simg->second.bound_memory == 0) ||
                (dimg->second.app_created && dimg->second.bound_memory == 0)) {
                resp.ok = false;
                resp.reason = "copy_image references an app image not bound to memory";
                return resp;
            }
            Resolved r;
            r.cmd = &c;
            r.image = simg->second.vk;
            r.dst_image = dimg->second.vk;
            resolved.push_back(r);
            for (const std::uint64_t ih : {c.args_u64[0], c.args_u64[1]}) {
                const auto& rec = images_.find(ih)->second;
                referenced_surfaces.insert(rec.surface);
                referenced_swapchains.insert(rec.swapchain);
                referenced_draw_objects.insert(ih);
            }
        } else if (k == vkrpc::CmdKind::BindTransformFeedbackBuffers) {
            // (GL/zink): args_i64=[firstBinding, bindingCount, hasSizes]; args_u64=[buffer x
            // N, offset x N, size x N (hasSizes=1)]. Rebinding while XFB is active is a spec
            // violation; render-pass scope is unconstrained (spec: both).
            if (dev->second.enabled_extensions.count(VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME) ==
                0) {
                resp.ok = false;
                resp.reason = "bind_transform_feedback_buffers requires VK_EXT_transform_feedback "
                              "enabled on the device";
                return resp;
            }
            if (xfb_active) {
                resp.ok = false;
                resp.reason = "bind_transform_feedback_buffers while transform feedback is active";
                return resp;
            }
            if (c.args_i64.size() != 3 || c.args_i64[1] <= 0 ||
                (c.args_i64[2] != 0 && c.args_i64[2] != 1)) {
                resp.ok = false;
                resp.reason = "malformed bind_transform_feedback_buffers";
                return resp;
            }
            const auto count = static_cast<std::size_t>(c.args_i64[1]);
            const bool has_sizes = c.args_i64[2] == 1;
            if (c.args_u64.size() != count * (has_sizes ? 3 : 2)) {
                resp.ok = false;
                resp.reason = "bind_transform_feedback_buffers payload malformed";
                return resp;
            }
            Resolved r;
            r.cmd = &c;
            for (std::size_t i = 0; i < count; ++i) {
                const auto bf = buffers_.find(c.args_u64[i]);
                if (bf == buffers_.end() || bf->second.device != device ||
                    bf->second.bound_memory == 0) {
                    resp.ok = false;
                    resp.reason = "bind_transform_feedback_buffers references a buffer not "
                                  "live/bound on the device";
                    return resp;
                }
                r.vbufs.push_back(bf->second.vk);
                r.voffs.push_back(static_cast<VkDeviceSize>(c.args_u64[count + i]));
                if (has_sizes) {
                    r.vsizes.push_back(static_cast<VkDeviceSize>(c.args_u64[count * 2 + i]));
                }
                referenced_draw_objects.insert(c.args_u64[i]);
            }
            resolved.push_back(std::move(r));
        } else if (k == vkrpc::CmdKind::BeginTransformFeedback ||
                   k == vkrpc::CmdKind::EndTransformFeedback) {
            // (GL/zink): XFB scope inside an active render pass, balanced. args_i64=
            // [firstCounterBuffer, counterBufferCount, hasOffsets]; args_u64=[counterBuffer x N
            // (0 = none), offset x N (hasOffsets=1)]. counterBufferCount may be 0.
            const bool is_begin = k == vkrpc::CmdKind::BeginTransformFeedback;
            if (dev->second.enabled_extensions.count(VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME) ==
                0) {
                resp.ok = false;
                resp.reason = "transform feedback requires VK_EXT_transform_feedback enabled on "
                              "the device";
                return resp;
            }
            if (active_scope == RenderScope::None) {
                resp.ok = false;
                resp.reason = "transform feedback outside an active render pass";
                return resp;
            }
            if (is_begin && xfb_active) {
                resp.ok = false;
                resp.reason = "begin_transform_feedback while transform feedback is active";
                return resp;
            }
            if (!is_begin && !xfb_active) {
                resp.ok = false;
                resp.reason = "end_transform_feedback without active transform feedback";
                return resp;
            }
            if (c.args_i64.size() != 3 || c.args_i64[1] < 0 ||
                (c.args_i64[2] != 0 && c.args_i64[2] != 1)) {
                resp.ok = false;
                resp.reason = "malformed transform feedback scope command";
                return resp;
            }
            const auto count = static_cast<std::size_t>(c.args_i64[1]);
            const bool has_offsets = c.args_i64[2] == 1;
            if (c.args_u64.size() != count * (has_offsets ? 2 : 1)) {
                resp.ok = false;
                resp.reason = "transform feedback scope payload malformed";
                return resp;
            }
            Resolved r;
            r.cmd = &c;
            for (std::size_t i = 0; i < count; ++i) {
                if (c.args_u64[i] == 0) {
                    r.vbufs.push_back(VK_NULL_HANDLE); // no counter for this binding
                    if (has_offsets) {
                        r.voffs.push_back(static_cast<VkDeviceSize>(c.args_u64[count + i]));
                    }
                    continue;
                }
                const auto bf = buffers_.find(c.args_u64[i]);
                if (bf == buffers_.end() || bf->second.device != device ||
                    bf->second.bound_memory == 0) {
                    resp.ok = false;
                    resp.reason = "transform feedback counter buffer is not live/bound on the "
                                  "device";
                    return resp;
                }
                r.vbufs.push_back(bf->second.vk);
                if (has_offsets) {
                    r.voffs.push_back(static_cast<VkDeviceSize>(c.args_u64[count + i]));
                }
                referenced_draw_objects.insert(c.args_u64[i]);
            }
            xfb_active = is_begin;
            resolved.push_back(std::move(r));
        } else if (k == vkrpc::CmdKind::BeginConditionalRendering) {
            // (GL/zink): predicate begins here; args_u64=[buffer]; args_i64=[offset (4-byte
            // aligned), flags]. No nesting; scope symmetry is enforced at end/stream-end.
            if (dev->second.enabled_extensions.count(VK_EXT_CONDITIONAL_RENDERING_EXTENSION_NAME) ==
                0) {
                resp.ok = false;
                resp.reason = "conditional rendering requires VK_EXT_conditional_rendering "
                              "enabled on the device";
                return resp;
            }
            if (cond_render_active) {
                resp.ok = false;
                resp.reason = "begin_conditional_rendering while conditional rendering is active";
                return resp;
            }
            if (c.args_u64.size() != 1 || c.args_i64.size() != 2 || c.args_i64[0] < 0 ||
                c.args_i64[0] % 4 != 0) {
                resp.ok = false;
                resp.reason = "malformed begin_conditional_rendering";
                return resp;
            }
            const auto bf = buffers_.find(c.args_u64[0]);
            if (bf == buffers_.end() || bf->second.device != device ||
                bf->second.bound_memory == 0) {
                resp.ok = false;
                resp.reason = "begin_conditional_rendering buffer is not live/bound on the device";
                return resp;
            }
            cond_render_active = true;
            cond_render_began_in_rp = (active_scope != RenderScope::None);
            Resolved r;
            r.cmd = &c;
            r.src_buffer = bf->second.vk;
            resolved.push_back(r);
            referenced_draw_objects.insert(c.args_u64[0]);
        } else if (k == vkrpc::CmdKind::EndConditionalRendering) {
            if (dev->second.enabled_extensions.count(VK_EXT_CONDITIONAL_RENDERING_EXTENSION_NAME) ==
                0) {
                resp.ok = false;
                resp.reason = "conditional rendering requires VK_EXT_conditional_rendering "
                              "enabled on the device";
                return resp;
            }
            if (!cond_render_active) {
                resp.ok = false;
                resp.reason = "end_conditional_rendering without active conditional rendering";
                return resp;
            }
            // Spec scope symmetry: begun-outside must end outside; begun-inside must end inside
            // (the same pass -- the end_render_pass guard above enforces "same").
            if (cond_render_began_in_rp != (active_scope != RenderScope::None)) {
                resp.ok = false;
                resp.reason = "end_conditional_rendering in a different render-pass scope than "
                              "its begin";
                return resp;
            }
            cond_render_active = false;
            Resolved r;
            r.cmd = &c;
            resolved.push_back(r);
        } else if (k == vkrpc::CmdKind::ResetQueryPool || k == vkrpc::CmdKind::BeginQuery ||
                   k == vkrpc::CmdKind::EndQuery || k == vkrpc::CmdKind::WriteTimestamp) {
            // Query pools (GL 3.3 / occlusion / xfb queries). args_u64=[query_pool]; args_i64 per
            // kind: reset=[firstQuery, queryCount]; begin=[query, flags]; end=[query];
            // write_timestamp=[pipelineStage, query]. The pool must live on the device and every
            // query index be in range; begin/end are balanced (no double-begin of the same index).
            if (c.args_u64.size() != 1) {
                resp.ok = false;
                resp.reason = "malformed query command (missing pool handle)";
                return resp;
            }
            const auto qp = query_pools_.find(c.args_u64[0]);
            if (qp == query_pools_.end() || qp->second.device != device) {
                resp.ok = false;
                resp.reason = "query command references a query pool not on the device";
                return resp;
            }
            auto index_ok = [&](long long q) {
                return q >= 0 && static_cast<std::uint32_t>(q) < qp->second.count;
            };
            Resolved r;
            r.cmd = &c;
            r.query_pool = qp->second.vk;
            if (k == vkrpc::CmdKind::ResetQueryPool) {
                if (c.args_i64.size() != 2 || c.args_i64[0] < 0 || c.args_i64[1] <= 0 ||
                    static_cast<std::uint64_t>(c.args_i64[0]) +
                            static_cast<std::uint64_t>(c.args_i64[1]) >
                        qp->second.count) {
                    resp.ok = false;
                    resp.reason = "reset_query_pool range out of bounds";
                    return resp;
                }
            } else if (k == vkrpc::CmdKind::BeginQuery) {
                if (c.args_i64.size() != 2 || !index_ok(c.args_i64[0])) {
                    resp.ok = false;
                    resp.reason = "begin_query index out of bounds";
                    return resp;
                }
                const auto key =
                    std::make_pair(c.args_u64[0], static_cast<std::uint32_t>(c.args_i64[0]));
                if (!active_queries.insert(key).second) {
                    resp.ok = false;
                    resp.reason = "begin_query on a query already active in this command buffer";
                    return resp;
                }
            } else if (k == vkrpc::CmdKind::EndQuery) {
                if (c.args_i64.size() != 1 || !index_ok(c.args_i64[0])) {
                    resp.ok = false;
                    resp.reason = "end_query index out of bounds";
                    return resp;
                }
                const auto key =
                    std::make_pair(c.args_u64[0], static_cast<std::uint32_t>(c.args_i64[0]));
                if (active_queries.erase(key) == 0) {
                    resp.ok = false;
                    resp.reason = "end_query without a matching active begin_query";
                    return resp;
                }
            } else { // write_timestamp
                if (c.args_i64.size() != 2 || !index_ok(c.args_i64[1])) {
                    resp.ok = false;
                    resp.reason = "write_timestamp index out of bounds";
                    return resp;
                }
            }
            resolved.push_back(std::move(r));
            referenced_draw_objects.insert(c.args_u64[0]); // destroying the pool invalidates the CB
        } else if (k == vkrpc::CmdKind::CopyQueryPoolResults) {
            // zink's query-result read path: copy results into a host-visible buffer the app maps.
            // args_u64=[query_pool, dst_buffer]; args_i64=[firstQuery, queryCount, dstOffset,
            // stride, flags]. A transfer command -- outside any render pass.
            if (active_scope != RenderScope::None) {
                resp.ok = false;
                resp.reason = "copy_query_pool_results inside an active render pass";
                return resp;
            }
            if (c.args_u64.size() != 2 || c.args_i64.size() != 5) {
                resp.ok = false;
                resp.reason = "malformed copy_query_pool_results";
                return resp;
            }
            const auto qp = query_pools_.find(c.args_u64[0]);
            if (qp == query_pools_.end() || qp->second.device != device) {
                resp.ok = false;
                resp.reason = "copy_query_pool_results references a query pool not on the device";
                return resp;
            }
            if (c.args_i64[0] < 0 || c.args_i64[1] <= 0 ||
                static_cast<std::uint64_t>(c.args_i64[0]) +
                        static_cast<std::uint64_t>(c.args_i64[1]) >
                    qp->second.count) {
                resp.ok = false;
                resp.reason = "copy_query_pool_results query range out of bounds";
                return resp;
            }
            const auto dbf = buffers_.find(c.args_u64[1]);
            if (dbf == buffers_.end() || dbf->second.device != device ||
                dbf->second.bound_memory == 0) {
                resp.ok = false;
                resp.reason = "copy_query_pool_results dest buffer is not live/bound on the device";
                return resp;
            }
            Resolved r;
            r.cmd = &c;
            r.query_pool = qp->second.vk;
            r.dst_buffer = dbf->second.vk;
            resolved.push_back(r);
            referenced_draw_objects.insert(c.args_u64[0]);
            referenced_draw_objects.insert(c.args_u64[1]);
        } else if (k == vkrpc::CmdKind::SetLineWidth || k == vkrpc::CmdKind::SetDepthBias ||
                   k == vkrpc::CmdKind::SetBlendConstants || k == vkrpc::CmdKind::SetDepthBounds ||
                   k == vkrpc::CmdKind::SetStencilCompareMask ||
                   k == vkrpc::CmdKind::SetStencilWriteMask ||
                   k == vkrpc::CmdKind::SetStencilReference) {
            // (GL/zink): core-1.0 dynamic-state set commands -- no handle resolution, no
            // draw-readiness impact; just baked + emitted.
            Resolved r;
            r.cmd = &c;
            resolved.push_back(r);
        } else if (k == vkrpc::CmdKind::SetCullMode || k == vkrpc::CmdKind::SetFrontFace ||
                   k == vkrpc::CmdKind::SetPrimitiveTopology ||
                   k == vkrpc::CmdKind::SetDepthTestEnable ||
                   k == vkrpc::CmdKind::SetDepthWriteEnable ||
                   k == vkrpc::CmdKind::SetDepthCompareOp ||
                   k == vkrpc::CmdKind::SetDepthBoundsTestEnable ||
                   k == vkrpc::CmdKind::SetStencilTestEnable) {
            // (native lane -- EDS1): VK_EXT_extended_dynamic_state setters. A single u32 in
            // args_i64[0]; no handle resolution, no draw-readiness impact -- just baked + emitted
            // (mock == real). The enum value passes through to the host driver / validation layer.
            // The extension MUST be enabled: this guarantees the *EXT PFN is
            // non-null at replay -- we serve the advertised extension honestly, not core-1.3 by
            // accident. Vulkan 1.3 support: an honest vk13 device admits them too (they are core
            // there; create_device resolved the PFNs either way).
            if (dev->second.enabled_extensions.count(
                    VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME) == 0 &&
                !dev->second.vk13_device) {
                resp.ok = false;
                resp.reason = "extended-dynamic-state set requires VK_EXT_extended_dynamic_state "
                              "or an honest 1.3 device";
                return resp;
            }
            if (c.args_i64.size() != 1) {
                resp.ok = false;
                resp.reason = "malformed extended-dynamic-state set command";
                return resp;
            }
            Resolved r;
            r.cmd = &c;
            resolved.push_back(r);
        } else if (k == vkrpc::CmdKind::SetStencilOp) {
            // Vulkan 1.3 support (EDS1): args_i64=[faceMask, failOp, passOp, depthFailOp,
            // compareOp]; same extension-or-vk13 gate as the single-u32 setters, values pass
            // through to the host driver / validation layer.
            if (dev->second.enabled_extensions.count(
                    VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME) == 0 &&
                !dev->second.vk13_device) {
                resp.ok = false;
                resp.reason = "extended-dynamic-state set requires VK_EXT_extended_dynamic_state "
                              "or an honest 1.3 device";
                return resp;
            }
            if (c.args_i64.size() != 5) {
                resp.ok = false;
                resp.reason = "malformed set_stencil_op";
                return resp;
            }
            Resolved r;
            r.cmd = &c;
            resolved.push_back(r);
        } else if (k == vkrpc::CmdKind::SetViewportWithCount ||
                   k == vkrpc::CmdKind::SetScissorWithCount) {
            // Vulkan 1.3 support (EDS1): the with-count pair. set_viewport_with_count:
            // args_i64=[count], args_f64 = 6 per viewport; set_scissor_with_count: args_i64=
            // [count, then x, y, w, h per scissor]. Same extension-or-vk13 gate.
            if (dev->second.enabled_extensions.count(
                    VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME) == 0 &&
                !dev->second.vk13_device) {
                resp.ok = false;
                resp.reason = "extended-dynamic-state set requires VK_EXT_extended_dynamic_state "
                              "or an honest 1.3 device";
                return resp;
            }
            if (c.args_i64.empty() || c.args_i64[0] < 1) {
                resp.ok = false;
                resp.reason = "malformed with-count dynamic-state set command";
                return resp;
            }
            const auto wc_count = static_cast<std::size_t>(c.args_i64[0]);
            const bool wc_shape_ok =
                k == vkrpc::CmdKind::SetViewportWithCount
                    ? (c.args_i64.size() == 1 && c.args_f64.size() == wc_count * 6)
                    : (c.args_i64.size() == 1 + wc_count * 4);
            if (!wc_shape_ok) {
                resp.ok = false;
                resp.reason = "with-count dynamic-state payload malformed";
                return resp;
            }
            // A valid with-count set IS a viewport/scissor set: it satisfies the draw gate the
            // same as the classic commands (Mesa >= 25 zink records ONLY the with-count pair on a
            // 1.3 device, so without this latch every such draw was rejected).
            if (k == vkrpc::CmdKind::SetViewportWithCount) {
                viewport_set = true;
            } else {
                scissor_set = true;
            }
            Resolved r;
            r.cmd = &c;
            resolved.push_back(r);
        } else if (k == vkrpc::CmdKind::BindVertexBuffers2) {
            // Vulkan 1.3 support (EDS1): vkCmdBindVertexBuffers2. args_i64=[firstBinding, count,
            // has_sizes, has_strides]; args_u64=[buffers x N, offsets x N, sizes x N?, strides x
            // N?]. Same extension-or-vk13 gate; the buffer rules mirror bind_vertex_buffers
            // (live + bound on the device, handles baked, destroy invalidates this CB).
            if (dev->second.enabled_extensions.count(
                    VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME) == 0 &&
                !dev->second.vk13_device) {
                resp.ok = false;
                resp.reason = "extended-dynamic-state set requires VK_EXT_extended_dynamic_state "
                              "or an honest 1.3 device";
                return resp;
            }
            if (c.args_i64.size() != 4 || c.args_i64[0] < 0 || c.args_i64[1] <= 0 ||
                (c.args_i64[2] != 0 && c.args_i64[2] != 1) ||
                (c.args_i64[3] != 0 && c.args_i64[3] != 1)) {
                resp.ok = false;
                resp.reason = "malformed bind_vertex_buffers2";
                return resp;
            }
            const auto count = static_cast<std::size_t>(c.args_i64[1]);
            const std::size_t groups = 2 + static_cast<std::size_t>(c.args_i64[2]) +
                                       static_cast<std::size_t>(c.args_i64[3]);
            if (c.args_u64.size() != count * groups) {
                resp.ok = false;
                resp.reason = "bind_vertex_buffers2 payload malformed";
                return resp;
            }
            Resolved r;
            r.cmd = &c;
            for (std::size_t i = 0; i < count; ++i) {
                const auto bf = buffers_.find(c.args_u64[i]);
                if (bf == buffers_.end() || bf->second.device != device ||
                    bf->second.bound_memory == 0) {
                    resp.ok = false;
                    resp.reason = "bind_vertex_buffers2 references a buffer not live/bound on "
                                  "the device";
                    return resp;
                }
                const VkDeviceSize offset = static_cast<VkDeviceSize>(c.args_u64[count + i]);
                if (offset >= bf->second.size) {
                    resp.ok = false;
                    resp.reason = "bind_vertex_buffers2 offset is outside the logical buffer";
                    return resp;
                }
                r.vbufs.push_back(bf->second.vk);
                r.voffs.push_back(offset);
                if (c.args_i64[2] == 1) {
                    const VkDeviceSize requested =
                        static_cast<VkDeviceSize>(c.args_u64[count * 2 + i]);
                    const VkDeviceSize remaining = bf->second.size - offset;
                    if (requested != VK_WHOLE_SIZE && (requested == 0 || requested > remaining)) {
                        resp.ok = false;
                        resp.reason = "bind_vertex_buffers2 size is outside the logical buffer";
                        return resp;
                    }
                    // The host object may carry a private fetch guard. VK_WHOLE_SIZE is guest
                    // state, so materialize it against the guest-visible logical end.
                    r.vsizes.push_back(requested == VK_WHOLE_SIZE ? remaining : requested);
                }
                bound_vertex_bindings.insert(static_cast<int>(c.args_i64[0]) + static_cast<int>(i));
                referenced_draw_objects.insert(c.args_u64[i]); // destroy invalidates this CB
            }
            resolved.push_back(std::move(r));
        } else if (k == vkrpc::CmdKind::SetRasterizerDiscardEnable ||
                   k == vkrpc::CmdKind::SetDepthBiasEnable ||
                   k == vkrpc::CmdKind::SetPrimitiveRestartEnable) {
            // Vulkan 1.3 support (EDS2 subset): the three enable-toggles core 1.3 absorbed from
            // VK_EXT_extended_dynamic_state2 -- served on the honest vk13 device ONLY (the
            // extension is not advertised; the core PFNs resolved at create_device).
            // args_i64=[enable(0/1)].
            if (!dev->second.vk13_device) {
                resp.ok = false;
                resp.reason = "core-1.3 dynamic state requires the honest 1.3 device";
                return resp;
            }
            if (c.args_i64.size() != 1) {
                resp.ok = false;
                resp.reason = "malformed core-1.3 dynamic-state set command";
                return resp;
            }
            Resolved r;
            r.cmd = &c;
            resolved.push_back(r);
        } else if (k == vkrpc::CmdKind::ResolveImage) {
            // Vulkan 1.3 support: faithful MSAA image resolve (core 1.0 vkCmdResolveImage; the
            // copy2 family's vkCmdResolveImage2 maps onto it at record). args_u64=[srcImage,
            // dstImage]; args_i64=[srcLayout, dstLayout, regionCount, 17 per region]. Same gates
            // as blit_image: a transfer command outside any render pass; both images live on the
            // device (an app image also bound to memory).
            if (active_scope != RenderScope::None) {
                resp.ok = false;
                resp.reason = "resolve_image inside an active render pass";
                return resp;
            }
            if (c.args_u64.size() != 2 || c.args_i64.size() < 3) {
                resp.ok = false;
                resp.reason = "malformed resolve_image";
                return resp;
            }
            const auto region_count = static_cast<std::size_t>(c.args_i64[2]);
            if (region_count == 0 || c.args_i64.size() != 3 + region_count * 17) {
                resp.ok = false;
                resp.reason = "resolve_image payload malformed";
                return resp;
            }
            const auto simg = images_.find(c.args_u64[0]);
            const auto dimg = images_.find(c.args_u64[1]);
            if (simg == images_.end() || simg->second.device != device || dimg == images_.end() ||
                dimg->second.device != device) {
                resp.ok = false;
                resp.reason = "resolve_image references an image not on the device";
                return resp;
            }
            if ((simg->second.app_created && simg->second.bound_memory == 0) ||
                (dimg->second.app_created && dimg->second.bound_memory == 0)) {
                resp.ok = false;
                resp.reason = "resolve_image references an app image not bound to memory";
                return resp;
            }
            Resolved r;
            r.cmd = &c;
            r.image = simg->second.vk;
            r.dst_image = dimg->second.vk;
            resolved.push_back(r);
            for (const std::uint64_t ih : {c.args_u64[0], c.args_u64[1]}) {
                const auto& rec = images_.find(ih)->second;
                referenced_surfaces.insert(rec.surface);
                referenced_swapchains.insert(rec.swapchain);
                referenced_draw_objects.insert(ih);
            }
        } else if (k == vkrpc::CmdKind::BeginRendering) {
            // (native lane): VK_KHR_dynamic_rendering. Same validate as the mock
            // (mock == real), plus it RESOLVES each guest attachment view to its host VkImageView
            // for replay. Gate on the enabled extension; fail-closed envelope (no flags/multiview,
            // positive layerCount). Wire: args_i64 = [flags, area.{x,y,w,h}, layerCount, viewMask,
            // colorCount, dsPresence] then per attachment [imageLayout, loadOp, storeOp]; args_u64
            // = guest view per attachment (0 = null, NOT resolved); args_blob = raw VkClearValue
            // per attachment. The active scope's attachment formats (from the views) become the
            // draw-time DR compatibility key.
            // Vulkan 1.3 support: on an honest vk13 device dynamic rendering is CORE -- the feature
            // alone admits it.
            if ((dev->second.enabled_extensions.count(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME) ==
                     0 &&
                 !dev->second.vk13_device) ||
                !dev->second.dynamic_rendering_feature_enabled) {
                resp.ok = false;
                resp.reason = "begin_rendering requires VK_KHR_dynamic_rendering AND the "
                              "dynamicRendering feature enabled on the device";
                return resp;
            }
            if (active_scope != RenderScope::None) {
                resp.ok = false;
                resp.reason = "begin_rendering inside an active render scope";
                return resp;
            }
            if (c.args_i64.size() < 9) {
                resp.ok = false;
                resp.reason = "malformed begin_rendering header";
                return resp;
            }
            const long long dr_flags = c.args_i64[0];
            const long long dr_layers = c.args_i64[5];
            const long long dr_view_mask = c.args_i64[6];
            const long long dr_color_count = c.args_i64[7];
            const long long dr_ds_presence = c.args_i64[8];
            if (dr_flags != 0) {
                resp.ok = false;
                resp.reason = "begin_rendering flags not supported (no suspend/resume/secondary)";
                return resp;
            }
            // Required-feature audit: a dynamic-rendering viewMask (multiview) is
            // carried to the host VkRenderingInfo.viewMask below, gated on the multiview feature --
            // the same fail-closed rule as the RP2 path. A negative mask is a malformed
            // frame.
            if (dr_view_mask != 0) {
                if (!dev->second.multiview_feature_enabled) {
                    resp.ok = false;
                    resp.reason = "begin_rendering viewMask (multiview) requires the multiview "
                                  "feature enabled";
                    return resp;
                }
                if (dr_view_mask < 0) {
                    resp.ok = false;
                    resp.reason = "begin_rendering viewMask is negative (malformed)";
                    return resp;
                }
            }
            if (dr_layers <= 0) {
                resp.ok = false;
                resp.reason = "begin_rendering layerCount must be positive";
                return resp;
            }
            if (dr_color_count < 0 ||
                dr_color_count > vkrpc::kMaxDynamicRenderingColorAttachments ||
                dr_ds_presence < 0 || dr_ds_presence > 3) {
                resp.ok = false;
                resp.reason = "begin_rendering color attachment count / depth-stencil presence "
                              "out of range";
                return resp;
            }
            const bool dr_has_depth = (dr_ds_presence & 1) != 0;
            const bool dr_has_stencil = (dr_ds_presence & 2) != 0;
            const std::size_t n_attach = static_cast<std::size_t>(dr_color_count) +
                                         (dr_has_depth ? 1u : 0u) + (dr_has_stencil ? 1u : 0u);
            if (c.args_i64.size() != 9 + n_attach * 3 || c.args_u64.size() != n_attach ||
                c.args_blob.size() != n_attach * vkrpc::kClearValueBytes) {
                resp.ok = false;
                resp.reason = "malformed begin_rendering attachment payload";
                return resp;
            }
            Resolved r;
            r.cmd = &c;
            std::vector<VkFormat> dr_color_formats;
            dr_color_formats.reserve(static_cast<std::size_t>(dr_color_count));
            VkFormat dr_depth_format = VK_FORMAT_UNDEFINED;
            VkFormat dr_stencil_format = VK_FORMAT_UNDEFINED;
            for (std::size_t i = 0; i < n_attach; ++i) {
                const std::uint64_t view_h = c.args_u64[i];
                VkImageView vk_view = VK_NULL_HANDLE;
                VkFormat fmt = VK_FORMAT_UNDEFINED;
                if (view_h != 0) {
                    const auto iv = image_views_.find(view_h);
                    if (iv == image_views_.end() || iv->second.device != device) {
                        resp.ok = false;
                        resp.reason = "begin_rendering attachment view not on the device";
                        return resp;
                    }
                    vk_view = iv->second.vk;
                    fmt = iv->second.format;
                    referenced_draw_objects.insert(view_h); // destroy invalidates this CB
                    // Required-feature audit: multiview layer-sufficiency on each DR
                    // attachment (parity with the RP2 framebuffer check; the host is the
                    // authoritative enforcer).
                    const auto vimg = images_.find(iv->second.image);
                    if (vimg != images_.end() &&
                        static_cast<int>(vimg->second.array_layers) <
                            vkrpc::multiview_required_layers(static_cast<int>(dr_view_mask))) {
                        resp.ok = false;
                        resp.reason = "begin_rendering attachment has too few array layers for the "
                                      "viewMask (multiview)";
                        return resp;
                    }
                }
                r.rendering_views.push_back(vk_view);
                if (i < static_cast<std::size_t>(dr_color_count)) {
                    dr_color_formats.push_back(fmt);
                } else if (dr_has_depth && i == static_cast<std::size_t>(dr_color_count)) {
                    dr_depth_format = fmt;
                } else {
                    dr_stencil_format = fmt;
                }
            }
            resolved.push_back(std::move(r));
            active_scope = RenderScope::DynamicRendering;
            active_dr_color_formats = std::move(dr_color_formats);
            active_dr_depth_format = dr_depth_format;
            active_dr_stencil_format = dr_stencil_format;
            active_dr_view_mask = static_cast<std::uint32_t>(dr_view_mask);
        } else if (k == vkrpc::CmdKind::EndRendering) {
            if (active_scope != RenderScope::DynamicRendering) {
                resp.ok = false;
                resp.reason = "end_rendering without an active dynamic-rendering scope";
                return resp;
            }
            // Mirror the EndRenderPass guards (mock == real) -- a DR scope must
            // not close with transform feedback still active, nor with conditional rendering begun
            // inside it.
            if (xfb_active) {
                resp.ok = false;
                resp.reason = "end_rendering with active transform feedback";
                return resp;
            }
            if (cond_render_active && cond_render_began_in_rp) {
                resp.ok = false;
                resp.reason = "end_rendering with conditional rendering begun inside the scope";
                return resp;
            }
            active_scope = RenderScope::None;
            active_dr_color_formats.clear();
            active_dr_depth_format = VK_FORMAT_UNDEFINED;
            active_dr_stencil_format = VK_FORMAT_UNDEFINED;
            active_dr_view_mask = 0;
            Resolved r;
            r.cmd = &c;
            resolved.push_back(r);
        } else {
            resp.ok = false;
            resp.reason = "unknown command kind";
            return resp;
        }
    }
    if (active_scope != RenderScope::None) {
        resp.ok = false;
        resp.reason = "command stream ends inside an active render scope";
        return resp;
    }
    // Transform feedback must not survive the command buffer -- it cannot escape
    // a render pass (EndRenderPass) nor a DR scope (EndRendering), so reaching here with it active
    // is a scope-machine bug; fail closed rather than replay an invalid host stream (mock == real).
    if (xfb_active) {
        resp.ok = false;
        resp.reason = "command stream ends with active transform feedback";
        return resp;
    }
    if (cond_render_active) {
        // (GL/zink): a conditional-rendering scope must not survive the command buffer.
        resp.ok = false;
        resp.reason = "command stream ends with active conditional rendering";
        return resp;
    }
    if (!active_queries.empty()) {
        // Query pools: a begun query must end within the same command buffer (spec).
        resp.ok = false;
        resp.reason = "command stream ends with an active query (begin without end)";
        return resp;
    }

    VkCommandBuffer cmd_vk = cb->second.vk;
    const bool one_time = req.one_time_submit;

    if (prof != nullptr) {
        prof->record_phases.validate_us += vkrpc::profile_clock_us() - t_validate0;
    }
    // hold the referenced surfaces' slot locks; run the host recording on the window
    // thread -- a command stream touching swapchain-backed images must not race the
    // bridge mutating the window. (No referenced surface -> empty lock set; an empty
    // recording still runs begin/end.)
    std::vector<std::unique_lock<std::mutex>> locks = lock_surface_slots(referenced_surfaces);
    std::string rec_err;
    const auto do_record = [&]() {
        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        if (one_time) {
            bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        }
        // vkBeginCommandBuffer implicitly resets the buffer (the pool has
        // RESET_COMMAND_BUFFER_BIT), so re-recording is fine. Each call is checked so a
        // failure surfaces cleanly rather than continuing into a dependent call.
        if (vkBeginCommandBuffer(cmd_vk, &bi) != VK_SUCCESS) {
            rec_err = "vkBeginCommandBuffer failed";
            return;
        }
        for (const Resolved& r : resolved) {
            // one lookup, then integer dispatch (same as validate above).
            const vkrpc::CmdKind rk = vkrpc::recorded_command_kind(*r.cmd);
            if (rk == vkrpc::CmdKind::BindVertexBuffers) {
                vkCmdBindVertexBuffers(cmd_vk, static_cast<std::uint32_t>(r.cmd->first_binding),
                                       static_cast<std::uint32_t>(r.vbufs.size()), r.vbufs.data(),
                                       r.voffs.data());
            } else if (rk == vkrpc::CmdKind::BindDescriptorSets) {
                // args_i64[0] = bindPoint (GRAPHICS/COMPUTE); args_u64 = dynamic offsets.
                const auto bind_point = r.cmd->args_i64.empty()
                                            ? VK_PIPELINE_BIND_POINT_GRAPHICS
                                            : static_cast<VkPipelineBindPoint>(r.cmd->args_i64[0]);
                std::vector<std::uint32_t> dyn_offsets;
                dyn_offsets.reserve(r.cmd->args_u64.size());
                for (const std::uint64_t off : r.cmd->args_u64) {
                    dyn_offsets.push_back(static_cast<std::uint32_t>(off));
                }
                vkCmdBindDescriptorSets(
                    cmd_vk, bind_point, r.desc_layout,
                    static_cast<std::uint32_t>(r.cmd->first_set < 0 ? 0 : r.cmd->first_set),
                    static_cast<std::uint32_t>(r.desc_sets.size()), r.desc_sets.data(),
                    static_cast<std::uint32_t>(dyn_offsets.size()),
                    dyn_offsets.empty() ? nullptr : dyn_offsets.data());
            } else if (rk == vkrpc::CmdKind::CopyBufferToImage) {
                // Faithful replay: args_i64=[dstImageLayout, regionCount, 13 per region] (the
                // copy_image_to_buffer convention), every VkBufferImageCopy field verbatim --
                // validate() already bounds-checked each region against the tracked image.
                const auto region_count = static_cast<std::size_t>(r.cmd->args_i64[1]);
                std::vector<VkBufferImageCopy> regions;
                regions.reserve(region_count);
                for (std::size_t ri = 0; ri < region_count; ++ri) {
                    const long long* a = &r.cmd->args_i64[2 + ri * 13];
                    VkBufferImageCopy region{};
                    region.bufferOffset = static_cast<VkDeviceSize>(a[0]);
                    region.bufferRowLength = static_cast<std::uint32_t>(a[1]);
                    region.bufferImageHeight = static_cast<std::uint32_t>(a[2]);
                    region.imageSubresource.aspectMask = static_cast<VkImageAspectFlags>(a[3]);
                    region.imageSubresource.mipLevel = static_cast<std::uint32_t>(a[4]);
                    region.imageSubresource.baseArrayLayer = static_cast<std::uint32_t>(a[5]);
                    region.imageSubresource.layerCount = static_cast<std::uint32_t>(a[6]);
                    region.imageOffset = {static_cast<std::int32_t>(a[7]),
                                          static_cast<std::int32_t>(a[8]),
                                          static_cast<std::int32_t>(a[9])};
                    region.imageExtent = {static_cast<std::uint32_t>(a[10]),
                                          static_cast<std::uint32_t>(a[11]),
                                          static_cast<std::uint32_t>(a[12])};
                    regions.push_back(region);
                }
                vkCmdCopyBufferToImage(cmd_vk, r.src_buffer, r.image,
                                       static_cast<VkImageLayout>(r.cmd->args_i64[0]),
                                       static_cast<std::uint32_t>(regions.size()), regions.data());
            } else if (rk == vkrpc::CmdKind::DispatchIndirect) {
                // Compute: the resolved indirect buffer + args_u64=[offset].
                vkCmdDispatchIndirect(cmd_vk, r.src_buffer,
                                      static_cast<VkDeviceSize>(r.cmd->args_u64[0]));
            } else if (rk == vkrpc::CmdKind::BufferMemoryBarrier) {
                // Compute: one VkBufferMemoryBarrier; args_u64 =
                // [offset, size] (~0 rides through as VK_WHOLE_SIZE, same bit pattern).
                VkBufferMemoryBarrier b{};
                b.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
                b.srcAccessMask = static_cast<VkAccessFlags>(r.cmd->src_access);
                b.dstAccessMask = static_cast<VkAccessFlags>(r.cmd->dst_access);
                b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                b.buffer = r.src_buffer;
                b.offset = static_cast<VkDeviceSize>(r.cmd->args_u64[0]);
                b.size = static_cast<VkDeviceSize>(r.cmd->args_u64[1]);
                vkCmdPipelineBarrier(cmd_vk, static_cast<VkPipelineStageFlags>(r.cmd->src_stage),
                                     static_cast<VkPipelineStageFlags>(r.cmd->dst_stage), 0, 0,
                                     nullptr, 1, &b, 0, nullptr);
            } else if (rk == vkrpc::CmdKind::CmdSetEvent) {
                // sync1 vkCmdSetEvent -- the resolved host event + the 32-bit stageMask.
                vkCmdSetEvent(cmd_vk, r.event, static_cast<VkPipelineStageFlags>(r.cmd->src_stage));
            } else if (rk == vkrpc::CmdKind::CmdResetEvent) {
                vkCmdResetEvent(cmd_vk, r.event,
                                static_cast<VkPipelineStageFlags>(r.cmd->src_stage));
            } else if (rk == vkrpc::CmdKind::CmdWaitEvents) {
                // rebuild the sync1 barrier arrays from the flat args_u64 (masks/layouts/
                // ranges) + the resolved host buffer/image handles (wire order), then wait on the
                // resolved event set. Queue families are IGNORED (ownership transfers were rejected
                // at record). The header counts drove the resolution above, so the offsets align.
                const auto& u = r.cmd->args_u64;
                const auto mem_count = static_cast<std::uint32_t>(u[1]);
                const auto buf_count = static_cast<std::uint32_t>(u[2]);
                const auto img_count = static_cast<std::uint32_t>(u[3]);
                std::size_t cur = vkrpc::kWaitEventsHeaderSlots +
                                  static_cast<std::size_t>(u[0]); // skip header + events
                std::vector<VkMemoryBarrier> mem(mem_count);
                for (std::uint32_t i = 0; i < mem_count; ++i) {
                    mem[i].sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
                    mem[i].srcAccessMask = static_cast<VkAccessFlags>(u[cur++]);
                    mem[i].dstAccessMask = static_cast<VkAccessFlags>(u[cur++]);
                }
                std::vector<VkBufferMemoryBarrier> buf(buf_count);
                for (std::uint32_t i = 0; i < buf_count; ++i) {
                    buf[i].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
                    buf[i].srcAccessMask = static_cast<VkAccessFlags>(u[cur++]);
                    buf[i].dstAccessMask = static_cast<VkAccessFlags>(u[cur++]);
                    cur++; // guest buffer handle (resolved into r.wait_buffers)
                    buf[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    buf[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    buf[i].buffer = r.wait_buffers[i];
                    buf[i].offset = static_cast<VkDeviceSize>(u[cur++]);
                    buf[i].size = static_cast<VkDeviceSize>(u[cur++]); // ~0 = WHOLE_SIZE
                }
                std::vector<VkImageMemoryBarrier> img(img_count);
                for (std::uint32_t i = 0; i < img_count; ++i) {
                    img[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                    img[i].srcAccessMask = static_cast<VkAccessFlags>(u[cur++]);
                    img[i].dstAccessMask = static_cast<VkAccessFlags>(u[cur++]);
                    img[i].oldLayout = static_cast<VkImageLayout>(u[cur++]);
                    img[i].newLayout = static_cast<VkImageLayout>(u[cur++]);
                    cur++; // guest image handle (resolved into r.wait_images)
                    img[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    img[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    img[i].image = r.wait_images[i];
                    img[i].subresourceRange.aspectMask = static_cast<VkImageAspectFlags>(u[cur++]);
                    img[i].subresourceRange.baseMipLevel = static_cast<std::uint32_t>(u[cur++]);
                    img[i].subresourceRange.levelCount = static_cast<std::uint32_t>(u[cur++]);
                    img[i].subresourceRange.baseArrayLayer = static_cast<std::uint32_t>(u[cur++]);
                    img[i].subresourceRange.layerCount = static_cast<std::uint32_t>(u[cur++]);
                }
                vkCmdWaitEvents(cmd_vk, static_cast<std::uint32_t>(r.wait_events.size()),
                                r.wait_events.data(),
                                static_cast<VkPipelineStageFlags>(r.cmd->src_stage),
                                static_cast<VkPipelineStageFlags>(r.cmd->dst_stage), mem_count,
                                mem.empty() ? nullptr : mem.data(), buf_count,
                                buf.empty() ? nullptr : buf.data(), img_count,
                                img.empty() ? nullptr : img.data());
            } else if (rk == vkrpc::CmdKind::PipelineBarrier2) {
                // rebuild the sync2 barrier arrays from deps2[0] + the resolved host
                // handles (QFI normalized to IGNORED) and replay via the *KHR PFN.
                std::vector<VkMemoryBarrier2> mem;
                std::vector<VkBufferMemoryBarrier2> buf;
                std::vector<VkImageMemoryBarrier2> img;
                build_barriers2(r.cmd->deps2[0], r.s2_dep_buffers[0], r.s2_dep_images[0], mem, buf,
                                img);
                const VkDependencyInfo di = make_dependency_info2(r.cmd->deps2[0], mem, buf, img);
                dev->second.pfn_pipeline_barrier2(cmd_vk, &di);
            } else if (rk == vkrpc::CmdKind::WriteTimestamp2) {
                // args_u64=[queryPool, stageMask64], args_i64=[query]; 64-bit stage.
                dev->second.pfn_write_timestamp2(
                    cmd_vk, static_cast<VkPipelineStageFlags2>(r.cmd->args_u64[1]), r.query_pool,
                    static_cast<std::uint32_t>(r.cmd->args_i64[0]));
            } else if (rk == vkrpc::CmdKind::CmdSetEvent2) {
                std::vector<VkMemoryBarrier2> mem;
                std::vector<VkBufferMemoryBarrier2> buf;
                std::vector<VkImageMemoryBarrier2> img;
                build_barriers2(r.cmd->deps2[0], r.s2_dep_buffers[0], r.s2_dep_images[0], mem, buf,
                                img);
                const VkDependencyInfo di = make_dependency_info2(r.cmd->deps2[0], mem, buf, img);
                dev->second.pfn_set_event2(cmd_vk, r.s2_events[0], &di);
            } else if (rk == vkrpc::CmdKind::CmdResetEvent2) {
                dev->second.pfn_reset_event2(
                    cmd_vk, r.s2_events[0], static_cast<VkPipelineStageFlags2>(r.cmd->args_u64[1]));
            } else if (rk == vkrpc::CmdKind::CmdWaitEvents2) {
                // N events paired with N VkDependencyInfo. Build each dependency's
                // barrier arrays (they must all stay alive across the single pfn call), then wait.
                const std::size_t n = r.s2_events.size();
                std::vector<std::vector<VkMemoryBarrier2>> mem(n);
                std::vector<std::vector<VkBufferMemoryBarrier2>> buf(n);
                std::vector<std::vector<VkImageMemoryBarrier2>> img(n);
                std::vector<VkDependencyInfo> deps(n);
                for (std::size_t i = 0; i < n; ++i) {
                    build_barriers2(r.cmd->deps2[i], r.s2_dep_buffers[i], r.s2_dep_images[i],
                                    mem[i], buf[i], img[i]);
                    deps[i] = make_dependency_info2(r.cmd->deps2[i], mem[i], buf[i], img[i]);
                }
                dev->second.pfn_wait_events2(cmd_vk, static_cast<std::uint32_t>(n),
                                             r.s2_events.data(), deps.data());
            } else if (rk == vkrpc::CmdKind::CopyBuffer) {
                // args_u64 = [src, dst, (srcOff, dstOff, size) x regionCount].
                std::vector<VkBufferCopy> regions;
                for (std::size_t i = 2; i + 2 < r.cmd->args_u64.size(); i += 3) {
                    VkBufferCopy bc{};
                    bc.srcOffset = static_cast<VkDeviceSize>(r.cmd->args_u64[i]);
                    bc.dstOffset = static_cast<VkDeviceSize>(r.cmd->args_u64[i + 1]);
                    bc.size = static_cast<VkDeviceSize>(r.cmd->args_u64[i + 2]);
                    regions.push_back(bc);
                }
                vkCmdCopyBuffer(cmd_vk, r.src_buffer, r.dst_buffer,
                                static_cast<std::uint32_t>(regions.size()), regions.data());
            } else if (rk == vkrpc::CmdKind::FillBuffer) {
                // args_u64 = [dstBuffer, dstOffset, size, data]; size VK_WHOLE_SIZE rides intact.
                vkCmdFillBuffer(cmd_vk, r.dst_buffer, static_cast<VkDeviceSize>(r.cmd->args_u64[1]),
                                static_cast<VkDeviceSize>(r.cmd->args_u64[2]),
                                static_cast<std::uint32_t>(r.cmd->args_u64[3]));
            } else if (rk == vkrpc::CmdKind::CopyImageToBuffer) {
                // args_i64=[srcImageLayout, regionCount, then 13 per region]. Rebuild the regions
                // faithfully and read the color image back into the dest buffer.
                const auto& a = r.cmd->args_i64;
                const auto region_count = static_cast<std::size_t>(a[1]);
                std::vector<VkBufferImageCopy> regions;
                regions.reserve(region_count);
                for (std::size_t k = 0; k < region_count; ++k) {
                    const std::size_t b = 2 + k * 13;
                    VkBufferImageCopy region{};
                    region.bufferOffset = static_cast<VkDeviceSize>(a[b + 0]);
                    region.bufferRowLength = static_cast<std::uint32_t>(a[b + 1]);
                    region.bufferImageHeight = static_cast<std::uint32_t>(a[b + 2]);
                    region.imageSubresource.aspectMask = static_cast<VkImageAspectFlags>(a[b + 3]);
                    region.imageSubresource.mipLevel = static_cast<std::uint32_t>(a[b + 4]);
                    region.imageSubresource.baseArrayLayer = static_cast<std::uint32_t>(a[b + 5]);
                    region.imageSubresource.layerCount = static_cast<std::uint32_t>(a[b + 6]);
                    region.imageOffset = {static_cast<std::int32_t>(a[b + 7]),
                                          static_cast<std::int32_t>(a[b + 8]),
                                          static_cast<std::int32_t>(a[b + 9])};
                    region.imageExtent = {static_cast<std::uint32_t>(a[b + 10]),
                                          static_cast<std::uint32_t>(a[b + 11]),
                                          static_cast<std::uint32_t>(a[b + 12])};
                    regions.push_back(region);
                }
                vkCmdCopyImageToBuffer(cmd_vk, r.image, static_cast<VkImageLayout>(a[0]),
                                       r.dst_buffer, static_cast<std::uint32_t>(regions.size()),
                                       regions.data());
            } else if (rk == vkrpc::CmdKind::BindIndexBuffer) {
                vkCmdBindIndexBuffer(cmd_vk, r.index_buffer, r.index_offset, r.index_type);
            } else if (rk == vkrpc::CmdKind::PushConstants) {
                const auto& a = r.cmd->args_i64; // [stageFlags, offset, size]
                vkCmdPushConstants(cmd_vk, r.desc_layout, static_cast<VkShaderStageFlags>(a[0]),
                                   static_cast<std::uint32_t>(a[1]),
                                   static_cast<std::uint32_t>(a[2]), r.cmd->args_blob.data());
            } else if (rk == vkrpc::CmdKind::DrawIndexed) {
                const auto& a = r.cmd->args_i64; // [indexCount, instanceCount, firstIndex,
                                                 // vertexOffset, firstInstance]
                vkCmdDrawIndexed(cmd_vk, static_cast<std::uint32_t>(a[0]),
                                 static_cast<std::uint32_t>(a[1]), static_cast<std::uint32_t>(a[2]),
                                 static_cast<std::int32_t>(a[3]), static_cast<std::uint32_t>(a[4]));
            } else if (rk == vkrpc::CmdKind::DrawIndirect) {
                vkCmdDrawIndirect(cmd_vk, r.src_buffer,
                                  static_cast<VkDeviceSize>(r.indirect_draw.offset),
                                  static_cast<std::uint32_t>(r.indirect_draw.draw_count),
                                  static_cast<std::uint32_t>(r.indirect_draw.stride));
            } else if (rk == vkrpc::CmdKind::DrawIndexedIndirect) {
                vkCmdDrawIndexedIndirect(cmd_vk, r.src_buffer,
                                         static_cast<VkDeviceSize>(r.indirect_draw.offset),
                                         static_cast<std::uint32_t>(r.indirect_draw.draw_count),
                                         static_cast<std::uint32_t>(r.indirect_draw.stride));
            } else if (rk == vkrpc::CmdKind::DrawIndirectCount) {
                dev->second.pfn_draw_indirect_count(
                    cmd_vk, r.src_buffer, static_cast<VkDeviceSize>(r.indirect_count_draw.offset),
                    r.count_buffer,
                    static_cast<VkDeviceSize>(r.indirect_count_draw.count_buffer_offset),
                    static_cast<std::uint32_t>(r.indirect_count_draw.max_draw_count),
                    static_cast<std::uint32_t>(r.indirect_count_draw.stride));
            } else if (rk == vkrpc::CmdKind::DrawIndexedIndirectCount) {
                dev->second.pfn_draw_indexed_indirect_count(
                    cmd_vk, r.src_buffer, static_cast<VkDeviceSize>(r.indirect_count_draw.offset),
                    r.count_buffer,
                    static_cast<VkDeviceSize>(r.indirect_count_draw.count_buffer_offset),
                    static_cast<std::uint32_t>(r.indirect_count_draw.max_draw_count),
                    static_cast<std::uint32_t>(r.indirect_count_draw.stride));
            } else if (rk == vkrpc::CmdKind::ClearAttachments) {
                // Rebuild the attachment/rect arrays; the 4 raw words restore the
                // VkClearValue union bit-faithfully (color OR depth+stencil).
                const auto& a = r.cmd->args_i64;
                const auto& u = r.cmd->args_u64;
                const auto att_count = static_cast<std::size_t>(a[0]);
                const auto rect_count = static_cast<std::size_t>(a[1]);
                std::vector<VkClearAttachment> atts(att_count);
                for (std::size_t k = 0; k < att_count; ++k) {
                    atts[k].aspectMask = static_cast<VkImageAspectFlags>(a[2 + k * 2]);
                    atts[k].colorAttachment = static_cast<std::uint32_t>(a[2 + k * 2 + 1]);
                    for (int w = 0; w < 4; ++w) {
                        atts[k].clearValue.color.uint32[w] =
                            static_cast<std::uint32_t>(u[k * 4 + w]);
                    }
                }
                const std::size_t rb = 2 + att_count * 2;
                std::vector<VkClearRect> rects(rect_count);
                for (std::size_t k = 0; k < rect_count; ++k) {
                    rects[k].rect.offset = {static_cast<std::int32_t>(a[rb + k * 6]),
                                            static_cast<std::int32_t>(a[rb + k * 6 + 1])};
                    rects[k].rect.extent = {static_cast<std::uint32_t>(a[rb + k * 6 + 2]),
                                            static_cast<std::uint32_t>(a[rb + k * 6 + 3])};
                    rects[k].baseArrayLayer = static_cast<std::uint32_t>(a[rb + k * 6 + 4]);
                    rects[k].layerCount = static_cast<std::uint32_t>(a[rb + k * 6 + 5]);
                }
                vkCmdClearAttachments(cmd_vk, static_cast<std::uint32_t>(att_count), atts.data(),
                                      static_cast<std::uint32_t>(rect_count), rects.data());
            } else if (rk == vkrpc::CmdKind::BlitImage) {
                // rebuild the region array faithfully (subresources + SIGNED corner offsets,
                // which express flips) and blit src -> dst with the carried filter.
                const auto& a = r.cmd->args_i64;
                const auto region_count = static_cast<std::size_t>(a[3]);
                std::vector<VkImageBlit> regions;
                regions.reserve(region_count);
                for (std::size_t k = 0; k < region_count; ++k) {
                    const std::size_t b = 4 + k * 20;
                    VkImageBlit region{};
                    region.srcSubresource.aspectMask = static_cast<VkImageAspectFlags>(a[b + 0]);
                    region.srcSubresource.mipLevel = static_cast<std::uint32_t>(a[b + 1]);
                    region.srcSubresource.baseArrayLayer = static_cast<std::uint32_t>(a[b + 2]);
                    region.srcSubresource.layerCount = static_cast<std::uint32_t>(a[b + 3]);
                    for (int o = 0; o < 2; ++o) {
                        region.srcOffsets[o] = {static_cast<std::int32_t>(a[b + 4 + o * 3]),
                                                static_cast<std::int32_t>(a[b + 5 + o * 3]),
                                                static_cast<std::int32_t>(a[b + 6 + o * 3])};
                    }
                    region.dstSubresource.aspectMask = static_cast<VkImageAspectFlags>(a[b + 10]);
                    region.dstSubresource.mipLevel = static_cast<std::uint32_t>(a[b + 11]);
                    region.dstSubresource.baseArrayLayer = static_cast<std::uint32_t>(a[b + 12]);
                    region.dstSubresource.layerCount = static_cast<std::uint32_t>(a[b + 13]);
                    for (int o = 0; o < 2; ++o) {
                        region.dstOffsets[o] = {static_cast<std::int32_t>(a[b + 14 + o * 3]),
                                                static_cast<std::int32_t>(a[b + 15 + o * 3]),
                                                static_cast<std::int32_t>(a[b + 16 + o * 3])};
                    }
                    regions.push_back(region);
                }
                vkCmdBlitImage(cmd_vk, r.image, static_cast<VkImageLayout>(a[0]), r.dst_image,
                               static_cast<VkImageLayout>(a[1]),
                               static_cast<std::uint32_t>(regions.size()), regions.data(),
                               static_cast<VkFilter>(a[2]));
            } else if (rk == vkrpc::CmdKind::CopyImage) {
                // rebuild the region array faithfully and copy src -> dst unscaled.
                const auto& a = r.cmd->args_i64;
                const auto region_count = static_cast<std::size_t>(a[2]);
                std::vector<VkImageCopy> regions;
                regions.reserve(region_count);
                for (std::size_t k = 0; k < region_count; ++k) {
                    const std::size_t b = 3 + k * 17;
                    VkImageCopy region{};
                    region.srcSubresource.aspectMask = static_cast<VkImageAspectFlags>(a[b + 0]);
                    region.srcSubresource.mipLevel = static_cast<std::uint32_t>(a[b + 1]);
                    region.srcSubresource.baseArrayLayer = static_cast<std::uint32_t>(a[b + 2]);
                    region.srcSubresource.layerCount = static_cast<std::uint32_t>(a[b + 3]);
                    region.srcOffset = {static_cast<std::int32_t>(a[b + 4]),
                                        static_cast<std::int32_t>(a[b + 5]),
                                        static_cast<std::int32_t>(a[b + 6])};
                    region.dstSubresource.aspectMask = static_cast<VkImageAspectFlags>(a[b + 7]);
                    region.dstSubresource.mipLevel = static_cast<std::uint32_t>(a[b + 8]);
                    region.dstSubresource.baseArrayLayer = static_cast<std::uint32_t>(a[b + 9]);
                    region.dstSubresource.layerCount = static_cast<std::uint32_t>(a[b + 10]);
                    region.dstOffset = {static_cast<std::int32_t>(a[b + 11]),
                                        static_cast<std::int32_t>(a[b + 12]),
                                        static_cast<std::int32_t>(a[b + 13])};
                    region.extent = {static_cast<std::uint32_t>(a[b + 14]),
                                     static_cast<std::uint32_t>(a[b + 15]),
                                     static_cast<std::uint32_t>(a[b + 16])};
                    regions.push_back(region);
                }
                vkCmdCopyImage(cmd_vk, r.image, static_cast<VkImageLayout>(a[0]), r.dst_image,
                               static_cast<VkImageLayout>(a[1]),
                               static_cast<std::uint32_t>(regions.size()), regions.data());
            } else if (rk == vkrpc::CmdKind::BindTransformFeedbackBuffers) {
                dev->second.pfn_bind_tf_buffers(
                    cmd_vk, static_cast<std::uint32_t>(r.cmd->args_i64[0]),
                    static_cast<std::uint32_t>(r.vbufs.size()), r.vbufs.data(), r.voffs.data(),
                    r.vsizes.empty() ? nullptr : r.vsizes.data());
            } else if (rk == vkrpc::CmdKind::BeginTransformFeedback ||
                       rk == vkrpc::CmdKind::EndTransformFeedback) {
                const auto pfn = rk == vkrpc::CmdKind::BeginTransformFeedback
                                     ? dev->second.pfn_begin_tf
                                     : dev->second.pfn_end_tf;
                pfn(cmd_vk, static_cast<std::uint32_t>(r.cmd->args_i64[0]),
                    static_cast<std::uint32_t>(r.vbufs.size()),
                    r.vbufs.empty() ? nullptr : r.vbufs.data(),
                    r.voffs.empty() ? nullptr : r.voffs.data());
            } else if (rk == vkrpc::CmdKind::DrawIndirectByteCount) {
                const auto& a = r.cmd->args_i64; // [instanceCount, firstInstance,
                                                 // counterBufferOffset, counterOffset, stride]
                dev->second.pfn_draw_indirect_byte_count(
                    cmd_vk, static_cast<std::uint32_t>(a[0]), static_cast<std::uint32_t>(a[1]),
                    r.src_buffer, static_cast<VkDeviceSize>(a[2]), static_cast<std::uint32_t>(a[3]),
                    static_cast<std::uint32_t>(a[4]));
            } else if (rk == vkrpc::CmdKind::BeginConditionalRendering) {
                VkConditionalRenderingBeginInfoEXT info{};
                info.sType = VK_STRUCTURE_TYPE_CONDITIONAL_RENDERING_BEGIN_INFO_EXT;
                info.buffer = r.src_buffer;
                info.offset = static_cast<VkDeviceSize>(r.cmd->args_i64[0]);
                info.flags = static_cast<VkConditionalRenderingFlagsEXT>(r.cmd->args_i64[1]);
                dev->second.pfn_begin_cond_render(cmd_vk, &info);
            } else if (rk == vkrpc::CmdKind::EndConditionalRendering) {
                dev->second.pfn_end_cond_render(cmd_vk);
            } else if (rk == vkrpc::CmdKind::ResetQueryPool) {
                vkCmdResetQueryPool(cmd_vk, r.query_pool,
                                    static_cast<std::uint32_t>(r.cmd->args_i64[0]),
                                    static_cast<std::uint32_t>(r.cmd->args_i64[1]));
            } else if (rk == vkrpc::CmdKind::BeginQuery) {
                vkCmdBeginQuery(cmd_vk, r.query_pool,
                                static_cast<std::uint32_t>(r.cmd->args_i64[0]),
                                static_cast<VkQueryControlFlags>(r.cmd->args_i64[1]));
            } else if (rk == vkrpc::CmdKind::EndQuery) {
                vkCmdEndQuery(cmd_vk, r.query_pool, static_cast<std::uint32_t>(r.cmd->args_i64[0]));
            } else if (rk == vkrpc::CmdKind::WriteTimestamp) {
                vkCmdWriteTimestamp(cmd_vk,
                                    static_cast<VkPipelineStageFlagBits>(r.cmd->args_i64[0]),
                                    r.query_pool, static_cast<std::uint32_t>(r.cmd->args_i64[1]));
            } else if (rk == vkrpc::CmdKind::CopyQueryPoolResults) {
                const auto& a = r.cmd->args_i64; // [first, count, dstOffset, stride, flags]
                vkCmdCopyQueryPoolResults(
                    cmd_vk, r.query_pool, static_cast<std::uint32_t>(a[0]),
                    static_cast<std::uint32_t>(a[1]), r.dst_buffer, static_cast<VkDeviceSize>(a[2]),
                    static_cast<VkDeviceSize>(a[3]), static_cast<VkQueryResultFlags>(a[4]));
            } else if (rk == vkrpc::CmdKind::SetLineWidth) {
                vkCmdSetLineWidth(cmd_vk, static_cast<float>(r.cmd->args_f64[0]));
            } else if (rk == vkrpc::CmdKind::SetDepthBias) {
                const auto& f = r.cmd->args_f64;
                vkCmdSetDepthBias(cmd_vk, static_cast<float>(f[0]), static_cast<float>(f[1]),
                                  static_cast<float>(f[2]));
            } else if (rk == vkrpc::CmdKind::SetBlendConstants) {
                const float bc[4] = {
                    static_cast<float>(r.cmd->args_f64[0]), static_cast<float>(r.cmd->args_f64[1]),
                    static_cast<float>(r.cmd->args_f64[2]), static_cast<float>(r.cmd->args_f64[3])};
                vkCmdSetBlendConstants(cmd_vk, bc);
            } else if (rk == vkrpc::CmdKind::SetDepthBounds) {
                vkCmdSetDepthBounds(cmd_vk, static_cast<float>(r.cmd->args_f64[0]),
                                    static_cast<float>(r.cmd->args_f64[1]));
            } else if (rk == vkrpc::CmdKind::SetStencilCompareMask) {
                vkCmdSetStencilCompareMask(cmd_vk,
                                           static_cast<VkStencilFaceFlags>(r.cmd->args_i64[0]),
                                           static_cast<std::uint32_t>(r.cmd->args_i64[1]));
            } else if (rk == vkrpc::CmdKind::SetStencilWriteMask) {
                vkCmdSetStencilWriteMask(cmd_vk,
                                         static_cast<VkStencilFaceFlags>(r.cmd->args_i64[0]),
                                         static_cast<std::uint32_t>(r.cmd->args_i64[1]));
            } else if (rk == vkrpc::CmdKind::SetStencilReference) {
                vkCmdSetStencilReference(cmd_vk,
                                         static_cast<VkStencilFaceFlags>(r.cmd->args_i64[0]),
                                         static_cast<std::uint32_t>(r.cmd->args_i64[1]));
            } else if (rk == vkrpc::CmdKind::SetCullMode) {
                // (native lane -- EDS1): replay via the *EXT PFNs resolved at
                // create_device, so we serve the advertised VK_EXT_extended_dynamic_state honestly
                // on the interim 1.2+extension lane. The validate gate above guarantees the
                // extension is enabled, so these PFNs are non-null.
                dev->second.pfn_set_cull_mode(cmd_vk,
                                              static_cast<VkCullModeFlags>(r.cmd->args_i64[0]));
            } else if (rk == vkrpc::CmdKind::SetFrontFace) {
                dev->second.pfn_set_front_face(cmd_vk,
                                               static_cast<VkFrontFace>(r.cmd->args_i64[0]));
            } else if (rk == vkrpc::CmdKind::SetPrimitiveTopology) {
                dev->second.pfn_set_primitive_topology(
                    cmd_vk, static_cast<VkPrimitiveTopology>(r.cmd->args_i64[0]));
            } else if (rk == vkrpc::CmdKind::SetDepthTestEnable) {
                dev->second.pfn_set_depth_test_enable(cmd_vk,
                                                      static_cast<VkBool32>(r.cmd->args_i64[0]));
            } else if (rk == vkrpc::CmdKind::SetDepthWriteEnable) {
                dev->second.pfn_set_depth_write_enable(cmd_vk,
                                                       static_cast<VkBool32>(r.cmd->args_i64[0]));
            } else if (rk == vkrpc::CmdKind::SetDepthCompareOp) {
                dev->second.pfn_set_depth_compare_op(cmd_vk,
                                                     static_cast<VkCompareOp>(r.cmd->args_i64[0]));
            } else if (rk == vkrpc::CmdKind::SetDepthBoundsTestEnable) {
                // Vulkan 1.3 support (EDS1 remainder + EDS2 subset): same PFN discipline as the
                // six above (resolved + null-guarded at create_device; the validate gate
                // guarantees non-null here).
                dev->second.pfn_set_depth_bounds_test_enable(
                    cmd_vk, static_cast<VkBool32>(r.cmd->args_i64[0]));
            } else if (rk == vkrpc::CmdKind::SetStencilTestEnable) {
                dev->second.pfn_set_stencil_test_enable(cmd_vk,
                                                        static_cast<VkBool32>(r.cmd->args_i64[0]));
            } else if (rk == vkrpc::CmdKind::SetStencilOp) {
                const auto& a = r.cmd->args_i64; // [faceMask, fail, pass, depthFail, compare]
                dev->second.pfn_set_stencil_op(
                    cmd_vk, static_cast<VkStencilFaceFlags>(a[0]), static_cast<VkStencilOp>(a[1]),
                    static_cast<VkStencilOp>(a[2]), static_cast<VkStencilOp>(a[3]),
                    static_cast<VkCompareOp>(a[4]));
            } else if (rk == vkrpc::CmdKind::SetViewportWithCount) {
                // args_f64 = 6 per viewport [x, y, width, height, minDepth, maxDepth].
                const auto vp_count = static_cast<std::size_t>(r.cmd->args_i64[0]);
                std::vector<VkViewport> vps;
                vps.reserve(vp_count);
                for (std::size_t i = 0; i < vp_count; ++i) {
                    const double* f = &r.cmd->args_f64[i * 6];
                    VkViewport v{};
                    v.x = static_cast<float>(f[0]);
                    v.y = static_cast<float>(f[1]);
                    v.width = static_cast<float>(f[2]);
                    v.height = static_cast<float>(f[3]);
                    v.minDepth = static_cast<float>(f[4]);
                    v.maxDepth = static_cast<float>(f[5]);
                    vps.push_back(v);
                }
                dev->second.pfn_set_viewport_with_count(
                    cmd_vk, static_cast<std::uint32_t>(vps.size()), vps.data());
            } else if (rk == vkrpc::CmdKind::SetScissorWithCount) {
                // args_i64 = [count, then x, y, w, h per scissor].
                const auto sc_count = static_cast<std::size_t>(r.cmd->args_i64[0]);
                std::vector<VkRect2D> scs;
                scs.reserve(sc_count);
                for (std::size_t i = 0; i < sc_count; ++i) {
                    const long long* a = &r.cmd->args_i64[1 + i * 4];
                    VkRect2D s{};
                    s.offset.x = static_cast<std::int32_t>(a[0]);
                    s.offset.y = static_cast<std::int32_t>(a[1]);
                    s.extent.width = static_cast<std::uint32_t>(a[2]);
                    s.extent.height = static_cast<std::uint32_t>(a[3]);
                    scs.push_back(s);
                }
                dev->second.pfn_set_scissor_with_count(
                    cmd_vk, static_cast<std::uint32_t>(scs.size()), scs.data());
            } else if (rk == vkrpc::CmdKind::BindVertexBuffers2) {
                // The resolved buffers/offsets/sizes + strides straight off the wire; absent
                // pSizes/pStrides ride as null (the spec's "whole buffer / pipeline stride").
                const auto count = static_cast<std::size_t>(r.cmd->args_i64[1]);
                const bool has_sizes = r.cmd->args_i64[2] == 1;
                const bool has_strides = r.cmd->args_i64[3] == 1;
                std::vector<VkDeviceSize> strides;
                if (has_strides) {
                    const std::size_t base = count * (has_sizes ? 3 : 2);
                    strides.reserve(count);
                    for (std::size_t i = 0; i < count; ++i) {
                        strides.push_back(static_cast<VkDeviceSize>(r.cmd->args_u64[base + i]));
                    }
                }
                dev->second.pfn_bind_vertex_buffers2(
                    cmd_vk, static_cast<std::uint32_t>(r.cmd->args_i64[0]),
                    static_cast<std::uint32_t>(count), r.vbufs.data(), r.voffs.data(),
                    has_sizes ? r.vsizes.data() : nullptr, has_strides ? strides.data() : nullptr);
            } else if (rk == vkrpc::CmdKind::SetRasterizerDiscardEnable) {
                dev->second.pfn_set_rasterizer_discard_enable(
                    cmd_vk, static_cast<VkBool32>(r.cmd->args_i64[0]));
            } else if (rk == vkrpc::CmdKind::SetDepthBiasEnable) {
                dev->second.pfn_set_depth_bias_enable(cmd_vk,
                                                      static_cast<VkBool32>(r.cmd->args_i64[0]));
            } else if (rk == vkrpc::CmdKind::SetPrimitiveRestartEnable) {
                dev->second.pfn_set_primitive_restart_enable(
                    cmd_vk, static_cast<VkBool32>(r.cmd->args_i64[0]));
            } else if (rk == vkrpc::CmdKind::ResolveImage) {
                // Vulkan 1.3 support: rebuild the region array faithfully and resolve src -> dst
                // (vkCmdResolveImage is core 1.0 -- called directly, no PFN).
                const auto& a = r.cmd->args_i64;
                const auto region_count = static_cast<std::size_t>(a[2]);
                std::vector<VkImageResolve> regions;
                regions.reserve(region_count);
                for (std::size_t k = 0; k < region_count; ++k) {
                    const std::size_t b = 3 + k * 17;
                    VkImageResolve region{};
                    region.srcSubresource.aspectMask = static_cast<VkImageAspectFlags>(a[b + 0]);
                    region.srcSubresource.mipLevel = static_cast<std::uint32_t>(a[b + 1]);
                    region.srcSubresource.baseArrayLayer = static_cast<std::uint32_t>(a[b + 2]);
                    region.srcSubresource.layerCount = static_cast<std::uint32_t>(a[b + 3]);
                    region.srcOffset = {static_cast<std::int32_t>(a[b + 4]),
                                        static_cast<std::int32_t>(a[b + 5]),
                                        static_cast<std::int32_t>(a[b + 6])};
                    region.dstSubresource.aspectMask = static_cast<VkImageAspectFlags>(a[b + 7]);
                    region.dstSubresource.mipLevel = static_cast<std::uint32_t>(a[b + 8]);
                    region.dstSubresource.baseArrayLayer = static_cast<std::uint32_t>(a[b + 9]);
                    region.dstSubresource.layerCount = static_cast<std::uint32_t>(a[b + 10]);
                    region.dstOffset = {static_cast<std::int32_t>(a[b + 11]),
                                        static_cast<std::int32_t>(a[b + 12]),
                                        static_cast<std::int32_t>(a[b + 13])};
                    region.extent = {static_cast<std::uint32_t>(a[b + 14]),
                                     static_cast<std::uint32_t>(a[b + 15]),
                                     static_cast<std::uint32_t>(a[b + 16])};
                    regions.push_back(region);
                }
                vkCmdResolveImage(cmd_vk, r.image, static_cast<VkImageLayout>(a[0]), r.dst_image,
                                  static_cast<VkImageLayout>(a[1]),
                                  static_cast<std::uint32_t>(regions.size()), regions.data());
            } else if (rk == vkrpc::CmdKind::BeginRendering) {
                // (native lane): rebuild VkRenderingInfo from the flattened wire
                // + the resolved host views, and replay via the *KHR PFN (resolved + null-guarded
                // at create_device, so it is non-null here). validate() already checked the shape +
                // envelope. The attachment structs must outlive the pfn call, so they live here.
                static_assert(sizeof(VkClearValue) == vkrpc::kClearValueBytes,
                              "VkClearValue size drift");
                const auto& a = r.cmd->args_i64;
                const std::uint32_t color_count = static_cast<std::uint32_t>(a[7]);
                const bool has_depth = (a[8] & 1) != 0;
                const bool has_stencil = (a[8] & 2) != 0;
                std::vector<VkRenderingAttachmentInfo> atts;
                atts.reserve(r.rendering_views.size());
                for (std::size_t i = 0; i < r.rendering_views.size(); ++i) {
                    VkRenderingAttachmentInfo ai{};
                    ai.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
                    ai.imageView = r.rendering_views[i];
                    ai.imageLayout = static_cast<VkImageLayout>(a[9 + i * 3 + 0]);
                    ai.resolveMode = VK_RESOLVE_MODE_NONE; // fail-closes resolve
                    ai.loadOp = static_cast<VkAttachmentLoadOp>(a[9 + i * 3 + 1]);
                    ai.storeOp = static_cast<VkAttachmentStoreOp>(a[9 + i * 3 + 2]);
                    std::memcpy(&ai.clearValue, r.cmd->args_blob.data() + i * sizeof(VkClearValue),
                                sizeof(VkClearValue));
                    atts.push_back(ai);
                }
                VkRenderingInfo ri{};
                ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
                ri.flags = static_cast<VkRenderingFlags>(a[0]);
                ri.renderArea.offset.x = static_cast<std::int32_t>(a[1]);
                ri.renderArea.offset.y = static_cast<std::int32_t>(a[2]);
                ri.renderArea.extent.width = static_cast<std::uint32_t>(a[3]);
                ri.renderArea.extent.height = static_cast<std::uint32_t>(a[4]);
                ri.layerCount = static_cast<std::uint32_t>(a[5]);
                ri.viewMask = static_cast<std::uint32_t>(a[6]);
                ri.colorAttachmentCount = color_count;
                ri.pColorAttachments = color_count > 0 ? atts.data() : nullptr;
                ri.pDepthAttachment = has_depth ? &atts[color_count] : nullptr;
                ri.pStencilAttachment =
                    has_stencil ? &atts[color_count + (has_depth ? 1u : 0u)] : nullptr;
                dev->second.pfn_begin_rendering(cmd_vk, &ri);
            } else if (rk == vkrpc::CmdKind::EndRendering) {
                dev->second.pfn_end_rendering(cmd_vk);
            } else {
                emit_command(cmd_vk, *r.cmd, rk, r.image, r.render_pass, r.framebuffer, r.pipeline,
                             r.begin_attachment_views, r.begin_framebuffer_extent);
            }
        }
        if (vkEndCommandBuffer(cmd_vk) != VK_SUCCESS) {
            rec_err = "vkEndCommandBuffer failed";
            return;
        }
    };
    // Replay timing runs INSIDE the thread that records (window thread or inline), so the
    // window-thread dispatch wait lands in execute-minus-phases residue, not in replay_us.
    const auto do_record_profiled = [&]() {
        if (prof == nullptr) {
            do_record();
            return;
        }
        const std::uint64_t t_replay0 = vkrpc::profile_clock_us();
        do_record();
        prof->record_phases.replay_us += vkrpc::profile_clock_us() - t_replay0;
    };
    bool routed = true;
    if (window_thread_) {
        routed = window_thread_->invoke(do_record_profiled);
    } else {
        do_record_profiled();
    }
    if (!routed) {
        resp.ok = false;
        resp.reason = "window thread unavailable";
        return resp;
    }
    if (!rec_err.empty()) {
        // (GL/zink) diagnostic: a host recording failure (vkEndCommandBuffer) names no
        // offending command, so dump the recorded command-kind stream to localize it.
        std::string kinds;
        for (const auto& dc : req.commands) {
            kinds += vkrpc::recorded_command_kind_name(dc);
            kinds += " ";
        }
        std::fprintf(stderr, "vkrelay2-worker: record FAILED (%s) -- stream: %s\n", rec_err.c_str(),
                     kinds.c_str());
        // A failed recording leaves the buffer not validly recorded -- mark it so
        // queue_submit refuses it.
        cb->second.recorded = false;
        cb->second.referenced_surfaces.clear();
        cb->second.referenced_swapchains.clear();
        cb->second.referenced_draw_objects.clear();
        resp.ok = false;
        resp.reason = rec_err;
        return resp;
    }
    cb->second.recorded = true;
    cb->second.referenced_surfaces = std::move(referenced_surfaces);
    cb->second.referenced_swapchains = std::move(referenced_swapchains);
    cb->second.referenced_draw_objects = std::move(referenced_draw_objects);
    resp.ok = true;
    resp.reason = "ok";
    return resp;
}

vkrpc::QueueSubmitResponse RealVulkanBackend::queue_submit(const vkrpc::QueueSubmitRequest& req) {
    vkrpc::QueueSubmitResponse resp;
    std::uint64_t queue_device = 0;
    VkQueue queue = VK_NULL_HANDLE;
    if (!resolve_queue(req.queue, queue_device, queue)) {
        resp.ok = false;
        resp.reason = "unknown queue handle";
        return resp;
    }
    // Lost-device latch: answer locally, never re-enter the host.
    if (device_lost_latched(queue_device)) {
        resp.ok = true;
        resp.reason = "ok";
        resp.result = vkrpc::kVkErrorDeviceLost;
        return resp;
    }
    // command_buffers may be empty (a fence/semaphore-only submit is valid Vulkan). Each
    // must be a RECORDED buffer on the queue's device; gather the VkCommandBuffers and the
    // union of their referenced swapchain surfaces (to lock).
    std::vector<VkCommandBuffer> cmd_vks;
    std::set<std::uint64_t> referenced_surfaces;
    for (const std::uint64_t b : req.command_buffers) {
        const auto cb = command_buffers_.find(b);
        if (cb == command_buffers_.end() || cb->second.device != queue_device) {
            resp.ok = false;
            resp.reason = "command buffer is not on the submit queue's device";
            return resp;
        }
        if (!cb->second.recorded) {
            resp.ok = false;
            resp.reason = "command buffer has not been recorded";
            return resp;
        }
        cmd_vks.push_back(cb->second.vk);
        referenced_surfaces.insert(cb->second.referenced_surfaces.begin(),
                                   cb->second.referenced_surfaces.end());
    }
    // Wait semaphores + their stage masks (sync1: each mask must be non-zero AND fit 32-bit
    // VkFlags -- an oversized wire mask is rejected, not truncated by the cast below); signal
    // semaphores; optional fence -- each device-checked against the queue's device.
    // (GL/zink): collect timeline wait/signal VALUES parallel to the semaphore arrays, and
    // note whether ANY participating semaphore is a timeline -- only then is a
    // VkTimelineSemaphoreSubmitInfo chained (a test device that never enabled the timeline feature
    // must not receive the struct). A binary semaphore's value slot is ignored (carried as 0).
    std::vector<VkSemaphore> wait_sems;
    std::vector<VkPipelineStageFlags> wait_stages;
    std::vector<std::uint64_t> wait_values;
    bool any_timeline = false;
    for (const vkrpc::SubmitWait& w : req.waits) {
        const auto leaf = leaves_.find(w.semaphore);
        if (leaf == leaves_.end() || leaf->second.kind != LeafKind::Semaphore ||
            leaf->second.device != queue_device) {
            resp.ok = false;
            resp.reason = "wait semaphore is not a semaphore on the submit queue's device";
            return resp;
        }
        if (!vkrpc::valid_stage_mask(w.stage)) {
            resp.ok = false;
            resp.reason = "wait semaphore has an invalid (zero or out-of-range) stage mask";
            return resp;
        }
        wait_sems.push_back(leaf->second.semaphore);
        wait_stages.push_back(static_cast<VkPipelineStageFlags>(w.stage));
        wait_values.push_back(w.value);
        any_timeline = any_timeline || leaf->second.is_timeline;
    }
    std::vector<VkSemaphore> signal_sems;
    std::vector<std::uint64_t> signal_values;
    for (std::size_t i = 0; i < req.signal_semaphores.size(); ++i) {
        const auto leaf = leaves_.find(req.signal_semaphores[i]);
        if (leaf == leaves_.end() || leaf->second.kind != LeafKind::Semaphore ||
            leaf->second.device != queue_device) {
            resp.ok = false;
            resp.reason = "signal semaphore is not a semaphore on the submit queue's device";
            return resp;
        }
        signal_sems.push_back(leaf->second.semaphore);
        signal_values.push_back(i < req.signal_values.size() ? req.signal_values[i] : 0);
        any_timeline = any_timeline || leaf->second.is_timeline;
    }
    VkFence fence_vk = VK_NULL_HANDLE;
    if (req.fence != 0) {
        const auto leaf = leaves_.find(req.fence);
        if (leaf == leaves_.end() || leaf->second.kind != LeafKind::Fence ||
            leaf->second.device != queue_device) {
            resp.ok = false;
            resp.reason = "fence is not a fence on the submit queue's device";
            return resp;
        }
        fence_vk = leaf->second.fence;
    }

    // hold the union of referenced surfaces' slot locks; route vkQueueSubmit on the
    // window thread. No OUT_OF_DATE synthesis -- submit returns the honest VkResult;
    // a dirty surface is caught by the acquire/present bracketing the frame.
    std::vector<std::unique_lock<std::mutex>> locks = lock_surface_slots(referenced_surfaces);
    VkResult result = VK_ERROR_DEVICE_LOST;
    const auto do_submit = [&]() {
        VkSubmitInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.waitSemaphoreCount = static_cast<std::uint32_t>(wait_sems.size());
        si.pWaitSemaphores = wait_sems.empty() ? nullptr : wait_sems.data();
        si.pWaitDstStageMask = wait_stages.empty() ? nullptr : wait_stages.data();
        si.commandBufferCount = static_cast<std::uint32_t>(cmd_vks.size());
        si.pCommandBuffers = cmd_vks.empty() ? nullptr : cmd_vks.data();
        si.signalSemaphoreCount = static_cast<std::uint32_t>(signal_sems.size());
        si.pSignalSemaphores = signal_sems.empty() ? nullptr : signal_sems.data();
        // (GL/zink): chain the timeline wait/signal values only when a timeline semaphore
        // is present (the value counts must equal the wait/signal counts; binary slots are
        // ignored).
        VkTimelineSemaphoreSubmitInfo tsi{};
        if (any_timeline) {
            tsi.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
            tsi.waitSemaphoreValueCount = static_cast<std::uint32_t>(wait_values.size());
            tsi.pWaitSemaphoreValues = wait_values.empty() ? nullptr : wait_values.data();
            tsi.signalSemaphoreValueCount = static_cast<std::uint32_t>(signal_values.size());
            tsi.pSignalSemaphoreValues = signal_values.empty() ? nullptr : signal_values.data();
            si.pNext = &tsi;
        }
        result = observe_device_result(queue_device, vkQueueSubmit(queue, 1, &si, fence_vk),
                                       "vkQueueSubmit");
    };
    bool routed = true;
    if (window_thread_) {
        routed = window_thread_->invoke(do_submit);
    } else {
        do_submit();
    }
    if (!routed) {
        resp.ok = false;
        resp.reason = "window thread unavailable";
        return resp;
    }
    resp.ok = true;
    resp.reason = "ok";
    resp.result = static_cast<int>(result);
    if (result == VK_ERROR_DEVICE_LOST) {
        log_device_lost_on_submit(referenced_surfaces, "vkQueueSubmit");
    }
    // developer proof: if this submit included a readback copy, wait the queue idle (so the
    // copy has completed) and dump the dest buffer(s). Debug-only: pending_readback_dumps_ is empty
    // unless VKRELAY2_DEBUG_DUMP_READBACK is set, so this whole block is a no-op in production.
    if (result == VK_SUCCESS && !pending_readback_dumps_.empty()) {
        locks.clear(); // release the slot locks before blocking on the queue
        VkResult idle = VK_SUCCESS;
        const auto wait_idle = [&]() {
            idle = observe_device_result(queue_device, vkQueueWaitIdle(queue),
                                         "vkQueueWaitIdle (debug readback)");
        };
        bool idle_routed = true;
        if (window_thread_) {
            idle_routed = window_thread_->invoke(wait_idle);
        } else {
            wait_idle();
        }
        if (!idle_routed) {
            return resp; // submit succeeded; retain pending dumps for a later real sync point
        }
        if (idle == VK_SUCCESS) {
            debug_dump_readbacks();
        } else {
            pending_readback_dumps_.clear();
            result = idle;
            resp.result = static_cast<int>(idle);
        }
    }
    return resp;
}

vkrpc::QueueSubmitResponse RealVulkanBackend::queue_submit2(const vkrpc::QueueSubmit2Request& req) {
    // vkQueueSubmit2 -- the WHOLE VkSubmitInfo2[] batch + one fence in ONE native
    // pfn_queue_submit2 call (more faithful than sync1's per-VkSubmitInfo RPC loop). Per-submit
    // ordering / wait-signal association stay exact. Gate on the synchronization2 feature (mock ==
    // real). QFI/deviceIndex/flags policing already happened at the ICD recorder / at submit parse.
    vkrpc::QueueSubmitResponse resp;
    std::uint64_t queue_device = 0;
    VkQueue queue = VK_NULL_HANDLE;
    if (!resolve_queue(req.queue, queue_device, queue)) {
        resp.ok = false;
        resp.reason = "unknown queue handle";
        return resp;
    }
    const auto dev = devices_.find(queue_device);
    if (dev == devices_.end() ||
        (dev->second.enabled_extensions.count(VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME) == 0 &&
         !dev->second.vk13_device) ||
        !dev->second.synchronization2_feature_enabled || dev->second.pfn_queue_submit2 == nullptr) {
        resp.ok = false;
        resp.reason = "vkQueueSubmit2 requires the synchronization2 feature";
        return resp;
    }
    // Lost-device latch: answer locally, never re-enter the host.
    if (dev->second.lost) {
        resp.ok = true;
        resp.reason = "ok";
        resp.result = vkrpc::kVkErrorDeviceLost;
        return resp;
    }
    // Build every VkSubmitInfo2 + its VkSemaphoreSubmitInfo / VkCommandBufferSubmitInfo arrays. All
    // the per-submit storage must outlive the single pfn call, so it lives in these outer vectors.
    const std::size_t n = req.submits.size();
    std::vector<std::vector<VkSemaphoreSubmitInfo>> waits(n);
    std::vector<std::vector<VkCommandBufferSubmitInfo>> cbs(n);
    std::vector<std::vector<VkSemaphoreSubmitInfo>> signals(n);
    std::vector<VkSubmitInfo2> submit_infos(n);
    std::set<std::uint64_t> referenced_surfaces;
    for (std::size_t s = 0; s < n; ++s) {
        const vkrpc::SubmitInfo2& si = req.submits[s];
        for (const vkrpc::SemaphoreSubmit2& w : si.waits) {
            const auto leaf = leaves_.find(w.semaphore);
            if (leaf == leaves_.end() || leaf->second.kind != LeafKind::Semaphore ||
                leaf->second.device != queue_device) {
                resp.ok = false;
                resp.reason = "wait semaphore is not a semaphore on the submit queue's device";
                return resp;
            }
            VkSemaphoreSubmitInfo ssi{};
            ssi.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
            ssi.semaphore = leaf->second.semaphore;
            ssi.value = w.value;
            ssi.stageMask = static_cast<VkPipelineStageFlags2>(w.stage_mask);
            waits[s].push_back(ssi);
        }
        for (const std::uint64_t b : si.command_buffers) {
            const auto cb = command_buffers_.find(b);
            if (cb == command_buffers_.end() || cb->second.device != queue_device) {
                resp.ok = false;
                resp.reason = "command buffer is not on the submit queue's device";
                return resp;
            }
            if (!cb->second.recorded) {
                resp.ok = false;
                resp.reason = "command buffer has not been recorded";
                return resp;
            }
            VkCommandBufferSubmitInfo cbi{};
            cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
            cbi.commandBuffer = cb->second.vk;
            cbs[s].push_back(cbi);
            referenced_surfaces.insert(cb->second.referenced_surfaces.begin(),
                                       cb->second.referenced_surfaces.end());
        }
        for (const vkrpc::SemaphoreSubmit2& sig : si.signals) {
            const auto leaf = leaves_.find(sig.semaphore);
            if (leaf == leaves_.end() || leaf->second.kind != LeafKind::Semaphore ||
                leaf->second.device != queue_device) {
                resp.ok = false;
                resp.reason = "signal semaphore is not a semaphore on the submit queue's device";
                return resp;
            }
            VkSemaphoreSubmitInfo ssi{};
            ssi.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
            ssi.semaphore = leaf->second.semaphore;
            ssi.value = sig.value;
            ssi.stageMask = static_cast<VkPipelineStageFlags2>(sig.stage_mask);
            signals[s].push_back(ssi);
        }
        submit_infos[s] = VkSubmitInfo2{};
        submit_infos[s].sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
        submit_infos[s].waitSemaphoreInfoCount = static_cast<std::uint32_t>(waits[s].size());
        submit_infos[s].pWaitSemaphoreInfos = waits[s].empty() ? nullptr : waits[s].data();
        submit_infos[s].commandBufferInfoCount = static_cast<std::uint32_t>(cbs[s].size());
        submit_infos[s].pCommandBufferInfos = cbs[s].empty() ? nullptr : cbs[s].data();
        submit_infos[s].signalSemaphoreInfoCount = static_cast<std::uint32_t>(signals[s].size());
        submit_infos[s].pSignalSemaphoreInfos = signals[s].empty() ? nullptr : signals[s].data();
    }
    VkFence fence_vk = VK_NULL_HANDLE;
    if (req.fence != 0) {
        const auto leaf = leaves_.find(req.fence);
        if (leaf == leaves_.end() || leaf->second.kind != LeafKind::Fence ||
            leaf->second.device != queue_device) {
            resp.ok = false;
            resp.reason = "fence is not a fence on the submit queue's device";
            return resp;
        }
        fence_vk = leaf->second.fence;
    }
    // hold the referenced surfaces' slot locks; route the single submit2 on the window thread.
    std::vector<std::unique_lock<std::mutex>> locks = lock_surface_slots(referenced_surfaces);
    VkResult result = VK_ERROR_DEVICE_LOST;
    const auto do_submit = [&]() {
        result = observe_device_result(
            queue_device,
            dev->second.pfn_queue_submit2(queue, static_cast<std::uint32_t>(submit_infos.size()),
                                          submit_infos.empty() ? nullptr : submit_infos.data(),
                                          fence_vk),
            "vkQueueSubmit2");
    };
    bool routed = true;
    if (window_thread_) {
        routed = window_thread_->invoke(do_submit);
    } else {
        do_submit();
    }
    if (!routed) {
        resp.ok = false;
        resp.reason = "window thread unavailable";
        return resp;
    }
    resp.ok = true;
    resp.reason = "ok";
    resp.result = static_cast<int>(result);
    if (result == VK_ERROR_DEVICE_LOST) {
        log_device_lost_on_submit(referenced_surfaces, "vkQueueSubmit2");
    }
    if (result == VK_SUCCESS && !pending_readback_dumps_.empty()) {
        locks.clear();
        VkResult idle = VK_SUCCESS;
        const auto wait_idle = [&]() {
            idle = observe_device_result(queue_device, vkQueueWaitIdle(queue),
                                         "vkQueueWaitIdle (debug readback submit2)");
        };
        bool idle_routed = true;
        if (window_thread_) {
            idle_routed = window_thread_->invoke(wait_idle);
        } else {
            wait_idle();
        }
        if (!idle_routed) {
            return resp; // submit2 succeeded; retain pending dumps for a later real sync point
        }
        if (idle == VK_SUCCESS) {
            debug_dump_readbacks();
        } else {
            pending_readback_dumps_.clear();
            result = idle;
            resp.result = static_cast<int>(idle);
        }
    }
    return resp;
}

bool RealVulkanBackend::resolve_fence_array(const std::vector<std::uint64_t>& handles,
                                            VkDevice& out_device, std::vector<VkFence>& out_fences,
                                            std::string& err) {
    if (handles.empty()) {
        err = "no fences specified";
        return false;
    }
    std::uint64_t device = 0;
    for (const std::uint64_t f : handles) {
        const auto leaf = leaves_.find(f);
        if (leaf == leaves_.end() || leaf->second.kind != LeafKind::Fence) {
            err = "handle is not a fence";
            return false;
        }
        if (device == 0) {
            device = leaf->second.device;
        } else if (leaf->second.device != device) {
            err = "fences span multiple devices";
            return false;
        }
        out_fences.push_back(leaf->second.fence);
    }
    const auto dev = devices_.find(device);
    if (dev == devices_.end() || dev->second.vk == VK_NULL_HANDLE) {
        err = "fence has no live device";
        return false;
    }
    out_device = dev->second.vk;
    return true;
}

vkrpc::StatusResponse RealVulkanBackend::reset_fences(const vkrpc::ResetFencesRequest& req) {
    vkrpc::StatusResponse resp;
    VkDevice dev_vk = VK_NULL_HANDLE;
    std::vector<VkFence> fences;
    if (!resolve_fence_array(req.fences, dev_vk, fences, resp.reason)) {
        resp.ok = false;
        return resp;
    }
    if (vkResetFences(dev_vk, static_cast<std::uint32_t>(fences.size()), fences.data()) !=
        VK_SUCCESS) {
        resp.ok = false;
        resp.reason = "vkResetFences failed";
        return resp;
    }
    resp.ok = true;
    resp.reason = "ok";
    return resp;
}

vkrpc::GetFenceStatusResponse RealVulkanBackend::get_fence_status(const vkrpc::HandleRequest& req) {
    // the fence-family completeness fix. VK_SUCCESS (signaled) / VK_NOT_READY
    // (unsignaled) are both NORMAL returns forwarded verbatim (ok=true, the app reads `result`); an
    // unknown/ wrong-kind handle or a real driver error faults.
    vkrpc::GetFenceStatusResponse resp;
    const auto it = leaves_.find(req.handle);
    if (it == leaves_.end() || it->second.kind != LeafKind::Fence) {
        resp.ok = false;
        resp.reason = "unknown handle for this object type";
        return resp;
    }
    const auto dev = devices_.find(it->second.device);
    if (dev == devices_.end() || dev->second.vk == VK_NULL_HANDLE) {
        resp.ok = false;
        resp.reason = "fence has no live device";
        return resp;
    }
    if (dev->second.lost) {
        resp.ok = true;
        resp.reason = "ok";
        resp.result = vkrpc::kVkErrorDeviceLost;
        return resp;
    }
    const VkResult r = observe_device_result(
        it->second.device, vkGetFenceStatus(dev->second.vk, it->second.fence), "vkGetFenceStatus");
    if (r != VK_SUCCESS && r != VK_NOT_READY && r != VK_ERROR_DEVICE_LOST) {
        resp.ok = false;
        resp.reason = "vkGetFenceStatus failed";
        return resp;
    }
    resp.ok = true;
    resp.reason = "ok";
    resp.result = static_cast<int>(r);
    return resp;
}

vkrpc::WaitForFencesResponse
RealVulkanBackend::wait_for_fences(const vkrpc::WaitForFencesRequest& req) {
    vkrpc::WaitForFencesResponse resp;
    VkDevice dev_vk = VK_NULL_HANDLE;
    std::vector<VkFence> fences;
    if (!resolve_fence_array(req.fences, dev_vk, fences, resp.reason)) {
        resp.ok = false;
        return resp;
    }
    // Lost-device latch: a wait on a lost device can block forever -- answer
    // locally. The fences all live on one device (resolve validated), so the first leaf names it.
    const std::uint64_t fence_device = leaves_.find(req.fences.front())->second.device;
    if (device_lost_latched(fence_device)) {
        resp.ok = true;
        resp.reason = "ok";
        resp.result = vkrpc::kVkErrorDeviceLost;
        return resp;
    }
    const VkResult r = observe_device_result(
        fence_device,
        vkWaitForFences(dev_vk, static_cast<std::uint32_t>(fences.size()), fences.data(),
                        req.wait_all ? VK_TRUE : VK_FALSE, req.timeout),
        "vkWaitForFences");
    // VK_SUCCESS / VK_TIMEOUT (and any real driver error) are honest VkResults, not RPC
    // faults: the op ran, so ok=true and the app reads `result`.
    resp.ok = true;
    resp.reason = "ok";
    resp.result = static_cast<int>(r);
    // developer proof: the awaited work (incl. any copy_image_to_buffer) is now complete,
    // so the readback buffers hold the rendered pixels -- dump them (no-op unless the env var is
    // set).
    if (r == VK_SUCCESS) {
        debug_dump_readbacks();
    }
    return resp;
}

// REAL idle waits. The ICD's idle entry points now ride these instead of
// answering a local VK_SUCCESS, so "idle" is a true readback-completion proof: the driver call
// returns only when the queue's / device's submitted work is done. Honest VkResult; a hard driver
// error is an RPC fault (like wait_semaphores).
vkrpc::WaitIdleResponse RealVulkanBackend::queue_wait_idle(const vkrpc::HandleRequest& req) {
    vkrpc::WaitIdleResponse resp;
    std::uint64_t queue_device = 0;
    VkQueue queue = VK_NULL_HANDLE;
    if (!resolve_queue(req.handle, queue_device, queue)) {
        resp.ok = false;
        resp.reason = "unknown queue handle";
        return resp;
    }
    // Lost-device latch: an idle-wait on a lost device can block forever --
    // answer the honest VkResult locally instead of faulting or re-entering the host.
    if (device_lost_latched(queue_device)) {
        resp.ok = true;
        resp.reason = "ok";
        resp.result = vkrpc::kVkErrorDeviceLost;
        return resp;
    }
    const VkResult r =
        observe_device_result(queue_device, vkQueueWaitIdle(queue), "vkQueueWaitIdle");
    if (r == VK_ERROR_DEVICE_LOST) {
        resp.ok = true;
        resp.reason = "ok";
        resp.result = static_cast<int>(r);
        return resp;
    }
    if (r != VK_SUCCESS) {
        resp.ok = false;
        resp.reason = "vkQueueWaitIdle failed (VkResult " + std::to_string(r) + ")";
        return resp;
    }
    resp.ok = true;
    resp.reason = "ok";
    resp.result = static_cast<int>(r);
    debug_dump_readbacks(); // idle = the awaited copies are complete (same hook as a fence wait)
    return resp;
}

vkrpc::WaitIdleResponse RealVulkanBackend::device_wait_idle(const vkrpc::HandleRequest& req) {
    vkrpc::WaitIdleResponse resp;
    const auto dev = devices_.find(req.handle);
    if (dev == devices_.end() || dev->second.vk == VK_NULL_HANDLE) {
        resp.ok = false;
        resp.reason = "unknown device handle";
        return resp;
    }
    // Lost-device latch: an idle-wait on a lost device can block forever --
    // answer the honest VkResult locally instead of faulting or re-entering the host.
    if (dev->second.lost) {
        resp.ok = true;
        resp.reason = "ok";
        resp.result = vkrpc::kVkErrorDeviceLost;
        return resp;
    }
    const VkResult r =
        observe_device_result(req.handle, vkDeviceWaitIdle(dev->second.vk), "vkDeviceWaitIdle");
    if (r == VK_ERROR_DEVICE_LOST) {
        resp.ok = true;
        resp.reason = "ok";
        resp.result = static_cast<int>(r);
        return resp;
    }
    if (r != VK_SUCCESS) {
        resp.ok = false;
        resp.reason = "vkDeviceWaitIdle failed (VkResult " + std::to_string(r) + ")";
        return resp;
    }
    resp.ok = true;
    resp.reason = "ok";
    resp.result = static_cast<int>(r);
    debug_dump_readbacks();
    return resp;
}

// developer proof (VKRELAY2_DEBUG_DUMP_READBACK=<path>): write each queued readback dest
// buffer's raw pixels to "<path>.<n>.raw" + dims to "<path>.<n>.dims". Captures EXACTLY what the
// GPU rendered through the relay; a PowerShell helper turns the raw BGRA8 into a PNG. Best-effort:
// any per-buffer failure is skipped, and the queue is always cleared.
void RealVulkanBackend::debug_dump_readbacks() {
    const char* path = std::getenv("VKRELAY2_DEBUG_DUMP_READBACK");
    if (path == nullptr) {
        pending_readback_dumps_.clear();
        return;
    }
    static int idx = 0; // monotonic across waits so no readback overwrites a prior one
    for (const PendingReadbackDump& pr : pending_readback_dumps_) {
        const auto bf = buffers_.find(pr.buffer);
        const auto dev = devices_.find(pr.device);
        if (bf == buffers_.end() || dev == devices_.end() || dev->second.vk == VK_NULL_HANDLE ||
            pr.width <= 0 || pr.height <= 0) {
            continue;
        }
        const auto mem = leaves_.find(bf->second.bound_memory);
        if (mem == leaves_.end() || mem->second.kind != LeafKind::Memory) {
            continue;
        }
        const std::size_t bytes =
            static_cast<std::size_t>(pr.width) * static_cast<std::size_t>(pr.height) * 4u;
        void* mapped = nullptr;
        if (vkMapMemory(dev->second.vk, mem->second.memory, 0, VK_WHOLE_SIZE, 0, &mapped) !=
                VK_SUCCESS ||
            mapped == nullptr) {
            continue;
        }
        const char* src =
            static_cast<const char*>(mapped) + static_cast<std::size_t>(bf->second.bound_offset);
        const std::string raw_file = std::string(path) + "." + std::to_string(idx) + ".raw";
        if (FILE* f = std::fopen(raw_file.c_str(), "wb")) {
            std::fwrite(src, 1, bytes, f);
            std::fclose(f);
        }
        vkUnmapMemory(dev->second.vk, mem->second.memory);
        const std::string dims_file = std::string(path) + "." + std::to_string(idx) + ".dims";
        if (FILE* d = std::fopen(dims_file.c_str(), "w")) {
            std::fprintf(d, "%d %d\n", pr.width, pr.height);
            std::fclose(d);
        }
        std::fprintf(stderr, "vkrelay2-worker: DEBUG dumped readback %dx%d (%zu bytes) -> %s\n",
                     pr.width, pr.height, bytes, raw_file.c_str());
        ++idx;
    }
    pending_readback_dumps_.clear();
}

// (GL/zink): host timeline-semaphore ops. Called DIRECTLY on the RPC thread (NOT the window
// thread): vkWaitSemaphores blocks until the GPU work that signals the timeline completes, and that
// work is submitted on the window thread -- waiting on the window thread would deadlock. These
// three device entry points are thread-safe (no external sync), like vkWaitForFences above.
vkrpc::WaitSemaphoresResponse
RealVulkanBackend::wait_semaphores(const vkrpc::WaitSemaphoresRequest& req) {
    vkrpc::WaitSemaphoresResponse resp;
    const auto dev = devices_.find(req.device);
    if (dev == devices_.end() || dev->second.vk == VK_NULL_HANDLE) {
        resp.ok = false;
        resp.reason = "unknown device handle";
        return resp;
    }
    if (req.semaphores.empty() || req.semaphores.size() != req.values.size()) {
        resp.ok = false;
        resp.reason = "wait_semaphores: semaphores/values size mismatch or empty";
        return resp;
    }
    // Lost-device latch: a timeline wait on a lost device can block forever
    // (a lost device's pending signals never fire) -- answer the honest VkResult locally.
    if (dev->second.lost) {
        resp.ok = true;
        resp.reason = "ok";
        resp.result = vkrpc::kVkErrorDeviceLost;
        return resp;
    }
    std::vector<VkSemaphore> sems;
    sems.reserve(req.semaphores.size());
    for (const std::uint64_t s : req.semaphores) {
        const auto leaf = leaves_.find(s);
        if (leaf == leaves_.end() || leaf->second.kind != LeafKind::Semaphore ||
            leaf->second.device != req.device || !leaf->second.is_timeline) {
            resp.ok = false;
            resp.reason = "wait_semaphores references a timeline semaphore not on the device";
            return resp;
        }
        sems.push_back(leaf->second.semaphore);
    }
    VkSemaphoreWaitInfo wi{};
    wi.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
    wi.flags = req.wait_any ? VK_SEMAPHORE_WAIT_ANY_BIT : 0;
    wi.semaphoreCount = static_cast<std::uint32_t>(sems.size());
    wi.pSemaphores = sems.data();
    wi.pValues = req.values.data();
    const VkResult r = observe_device_result(
        req.device, vkWaitSemaphores(dev->second.vk, &wi, req.timeout), "vkWaitSemaphores");
    // VK_SUCCESS / VK_TIMEOUT are honest results the app reads; only a hard driver error is a
    // fault. DEVICE_LOST latches (never re-enter) and rides back honestly like the wait paths.
    if (r == VK_ERROR_DEVICE_LOST) {
        resp.ok = true;
        resp.reason = "ok";
        resp.result = static_cast<int>(r);
        return resp;
    }
    if (r != VK_SUCCESS && r != VK_TIMEOUT) {
        resp.ok = false;
        resp.reason = "vkWaitSemaphores failed (VkResult " + std::to_string(r) + ")";
        return resp;
    }
    resp.ok = true;
    resp.reason = "ok";
    resp.result = static_cast<int>(r);
    return resp;
}

vkrpc::StatusResponse
RealVulkanBackend::signal_semaphore(const vkrpc::SignalSemaphoreRequest& req) {
    vkrpc::StatusResponse resp;
    const auto dev = devices_.find(req.device);
    if (dev == devices_.end() || dev->second.vk == VK_NULL_HANDLE) {
        resp.ok = false;
        resp.reason = "unknown device handle";
        return resp;
    }
    const auto leaf = leaves_.find(req.semaphore);
    if (leaf == leaves_.end() || leaf->second.kind != LeafKind::Semaphore ||
        leaf->second.device != req.device || !leaf->second.is_timeline) {
        resp.ok = false;
        resp.reason = "signal_semaphore references a timeline semaphore not on the device";
        return resp;
    }
    VkSemaphoreSignalInfo sgi{};
    sgi.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO;
    sgi.semaphore = leaf->second.semaphore;
    sgi.value = req.value;
    const VkResult r = vkSignalSemaphore(dev->second.vk, &sgi);
    if (r != VK_SUCCESS) {
        resp.ok = false;
        resp.reason = "vkSignalSemaphore failed (VkResult " + std::to_string(r) + ")";
        return resp;
    }
    resp.ok = true;
    resp.reason = "ok";
    return resp;
}

vkrpc::GetSemaphoreCounterValueResponse
RealVulkanBackend::get_semaphore_counter_value(const vkrpc::GetSemaphoreCounterValueRequest& req) {
    vkrpc::GetSemaphoreCounterValueResponse resp;
    const auto dev = devices_.find(req.device);
    if (dev == devices_.end() || dev->second.vk == VK_NULL_HANDLE) {
        resp.ok = false;
        resp.reason = "unknown device handle";
        return resp;
    }
    const auto leaf = leaves_.find(req.semaphore);
    if (leaf == leaves_.end() || leaf->second.kind != LeafKind::Semaphore ||
        leaf->second.device != req.device || !leaf->second.is_timeline) {
        resp.ok = false;
        resp.reason =
            "get_semaphore_counter_value references a timeline semaphore not on the device";
        return resp;
    }
    if (dev->second.lost) {
        resp.ok = true;
        resp.reason = "ok";
        resp.result = vkrpc::kVkErrorDeviceLost;
        return resp;
    }
    std::uint64_t value = 0;
    const VkResult r = observe_device_result(
        req.device, vkGetSemaphoreCounterValue(dev->second.vk, leaf->second.semaphore, &value),
        "vkGetSemaphoreCounterValue");
    if (r != VK_SUCCESS && r != VK_ERROR_DEVICE_LOST) {
        resp.ok = false;
        resp.reason = "vkGetSemaphoreCounterValue failed (VkResult " + std::to_string(r) + ")";
        return resp;
    }
    resp.ok = true;
    resp.reason = "ok";
    resp.result = static_cast<int>(r);
    if (r == VK_SUCCESS) {
        resp.value = value;
    }
    return resp;
}

// Present-capture artifact metadata: map the common 4-byte swapchain formats to a
// human name + a byte channel-order tag ("BGRA"/"RGBA"), so raw_to_png.ps1 can interpret the .raw
// without a VkFormat table -- and fail clearly ("UNKNOWN") rather than mis-decode an unsupported
// one.
static const char* capture_format_name(VkFormat f) {
    switch (f) {
    case VK_FORMAT_B8G8R8A8_UNORM:
        return "VK_FORMAT_B8G8R8A8_UNORM";
    case VK_FORMAT_B8G8R8A8_SRGB:
        return "VK_FORMAT_B8G8R8A8_SRGB";
    case VK_FORMAT_R8G8B8A8_UNORM:
        return "VK_FORMAT_R8G8B8A8_UNORM";
    case VK_FORMAT_R8G8B8A8_SRGB:
        return "VK_FORMAT_R8G8B8A8_SRGB";
    case VK_FORMAT_A8B8G8R8_UNORM_PACK32:
        return "VK_FORMAT_A8B8G8R8_UNORM_PACK32";
    case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
        return "VK_FORMAT_A8B8G8R8_SRGB_PACK32";
    default:
        return "UNKNOWN";
    }
}
static const char* capture_channel_order(VkFormat f) {
    switch (f) {
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_SRGB:
        return "BGRA"; // Format32bppArgb in memory is BGRA -> direct copy
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_A8B8G8R8_UNORM_PACK32: // packed 0xAABBGGRR -> bytes R,G,B,A
    case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
        return "RGBA"; // converter swaps R<->B
    default:
        return "UNKNOWN"; // converter fails clearly
    }
}

bool RealVulkanBackend::begin_present_capture(std::uint64_t device, VkQueue queue, VkImage image,
                                              VkExtent2D extent, VkFormat format,
                                              VkImageUsageFlags usage,
                                              const std::vector<VkSemaphore>& app_wait_sems,
                                              PresentCapture& out) {
    // Runs on the window thread (called from inside the present invoke). Records + submits the
    // PRE-present copy: it WAITS the app's present-wait semaphores (so the app's render is done
    // before we read the image), transitions PRESENT_SRC->TRANSFER_SRC, copies image->staging,
    // transitions back to PRESENT_SRC, and SIGNALS out.done -- the present then chains on out.done
    // (never on the app's already-consumed binary semaphores). Any failed step frees what it
    // created and returns false, leaving the present on the app's own semaphores.
    out = PresentCapture{};
    const auto dev = devices_.find(device);
    if (dev == devices_.end() || dev->second.vk == VK_NULL_HANDLE) {
        return false;
    }
    const auto inst = instances_.find(dev->second.instance);
    if (inst == instances_.end() || inst->second.physical_vk == VK_NULL_HANDLE) {
        return false;
    }
    VkDevice vk = dev->second.vk;
    VkPhysicalDevice phys = inst->second.physical_vk;
    const std::uint32_t qfamily = dev->second.queue_family;
    out.device = device;
    out.vk = vk;
    out.extent = extent;
    out.format = format;
    out.usage = usage;
    out.bytes =
        static_cast<VkDeviceSize>(extent.width) * static_cast<VkDeviceSize>(extent.height) * 4u;

    auto fail = [&]() {
        if (out.done != VK_NULL_HANDLE) {
            vkDestroySemaphore(vk, out.done, nullptr);
        }
        if (out.pool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(vk, out.pool, nullptr);
        }
        if (out.mem != VK_NULL_HANDLE) {
            vkFreeMemory(vk, out.mem, nullptr);
        }
        if (out.buf != VK_NULL_HANDLE) {
            vkDestroyBuffer(vk, out.buf, nullptr);
        }
        out = PresentCapture{};
        return false;
    };

    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = out.bytes;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(vk, &bci, nullptr, &out.buf) != VK_SUCCESS) {
        return fail();
    }
    VkMemoryRequirements mr{};
    vkGetBufferMemoryRequirements(vk, out.buf, &mr);
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    std::uint32_t type_index = UINT32_MAX;
    const VkMemoryPropertyFlags want =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (std::uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if ((mr.memoryTypeBits & (1u << i)) != 0 &&
            (mp.memoryTypes[i].propertyFlags & want) == want) {
            type_index = i;
            break;
        }
    }
    if (type_index == UINT32_MAX) {
        return fail();
    }
    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = type_index;
    if (vkAllocateMemory(vk, &mai, nullptr, &out.mem) != VK_SUCCESS ||
        vkBindBufferMemory(vk, out.buf, out.mem, 0) != VK_SUCCESS) {
        return fail();
    }
    VkCommandPoolCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.queueFamilyIndex = qfamily;
    if (vkCreateCommandPool(vk, &pci, nullptr, &out.pool) != VK_SUCCESS) {
        return fail();
    }
    VkCommandBufferAllocateInfo cbai{};
    cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool = out.pool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(vk, &cbai, &cb) != VK_SUCCESS) {
        return fail();
    }
    VkSemaphoreCreateInfo sci{};
    sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    if (vkCreateSemaphore(vk, &sci, nullptr, &out.done) != VK_SUCCESS) {
        return fail();
    }
    VkCommandBufferBeginInfo cbbi{};
    cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &cbbi);
    VkImageMemoryBarrier bar{};
    bar.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bar.image = image;
    bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    bar.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    bar.srcAccessMask = 0;
    bar.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &bar);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {extent.width, extent.height, 1};
    vkCmdCopyImageToBuffer(cb, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, out.buf, 1, &region);
    bar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    bar.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    bar.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    bar.dstAccessMask = 0;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &bar);
    vkEndCommandBuffer(cb);
    // Wait the app's present-wait semaphores (binary, signaled by the app's render submit), so the
    // copy reads a fully-rendered image; signal out.done for the present to chain on.
    std::vector<VkPipelineStageFlags> wait_stages(app_wait_sems.size(),
                                                  VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.waitSemaphoreCount = static_cast<std::uint32_t>(app_wait_sems.size());
    si.pWaitSemaphores = app_wait_sems.empty() ? nullptr : app_wait_sems.data();
    si.pWaitDstStageMask = app_wait_sems.empty() ? nullptr : wait_stages.data();
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &out.done;
    const VkResult submit_result = observe_device_result(
        device, vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE), "vkQueueSubmit (present capture)");
    if (submit_result == VK_ERROR_DEVICE_LOST) {
        out = PresentCapture{}; // abandon temporary host objects to process termination
        return false;
    }
    if (submit_result != VK_SUCCESS) {
        return fail();
    }
    out.active = true;
    return true;
}

VkResult RealVulkanBackend::end_present_capture(VkQueue queue, PresentCapture& cap) {
    // Runs on the window thread AFTER the present (which waited on cap.done). A single queue
    // wait-idle covers both: the capture copy has completed (staging filled) AND the present has
    // consumed cap.done (so the semaphore is safe to destroy). Then map + write the artifact +
    // free.
    if (!cap.active) {
        return VK_SUCCESS;
    }
    const VkResult idle = observe_device_result(cap.device, vkQueueWaitIdle(queue),
                                                "vkQueueWaitIdle (present capture)");
    if (idle != VK_SUCCESS) {
        cap = PresentCapture{}; // pending/lost temporary objects are intentionally abandoned
        return idle;
    }
    const char* path = std::getenv("VKRELAY2_DEBUG_DUMP_PRESENT");
    const int this_seq = present_capture_seq_;
    void* ptr = nullptr;
    if (path != nullptr && vkMapMemory(cap.vk, cap.mem, 0, cap.bytes, 0, &ptr) == VK_SUCCESS &&
        ptr != nullptr) {
        const std::string raw = std::string(path) + "." + std::to_string(this_seq) + ".raw";
        if (FILE* f = std::fopen(raw.c_str(), "wb")) {
            std::fwrite(ptr, 1, static_cast<std::size_t>(cap.bytes), f);
            std::fclose(f);
        }
        vkUnmapMemory(cap.vk, cap.mem);
        const std::string dims = std::string(path) + "." + std::to_string(this_seq) + ".dims";
        if (FILE* d = std::fopen(dims.c_str(), "w")) {
            std::fprintf(d, "%u %u\n", cap.extent.width, cap.extent.height);
            std::fclose(d);
        }
        // Self-describing sidecar: VkFormat (+ name + byte order), extent, usage,
        // so the artifact is interpretable beyond bare dimensions. raw_to_png.ps1 prefers .meta
        // over the legacy .dims and fails clearly on order=UNKNOWN rather than mis-decoding.
        const std::string meta = std::string(path) + "." + std::to_string(this_seq) + ".meta";
        if (FILE* m = std::fopen(meta.c_str(), "w")) {
            std::fprintf(m, "format=%d\n", static_cast<int>(cap.format));
            std::fprintf(m, "format_name=%s\n", capture_format_name(cap.format));
            std::fprintf(m, "order=%s\n", capture_channel_order(cap.format));
            std::fprintf(m, "width=%u\n", cap.extent.width);
            std::fprintf(m, "height=%u\n", cap.extent.height);
            std::fprintf(m, "bpp=4\n");
            std::fprintf(m, "usage=0x%x\n", static_cast<unsigned>(cap.usage));
            std::fclose(m);
        }
        std::fprintf(stderr, "vkrelay2-worker: DEBUG dumped present %ux%u %s -> %s\n",
                     cap.extent.width, cap.extent.height, capture_format_name(cap.format),
                     raw.c_str());
    }
    ++present_capture_seq_;
    if (cap.done != VK_NULL_HANDLE) {
        vkDestroySemaphore(cap.vk, cap.done, nullptr);
    }
    if (cap.pool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(cap.vk, cap.pool, nullptr);
    }
    if (cap.mem != VK_NULL_HANDLE) {
        vkFreeMemory(cap.vk, cap.mem, nullptr);
    }
    if (cap.buf != VK_NULL_HANDLE) {
        vkDestroyBuffer(cap.vk, cap.buf, nullptr);
    }
    cap = PresentCapture{};
    return VK_SUCCESS;
}

vkrpc::QueuePresentResponse
RealVulkanBackend::queue_present(const vkrpc::QueuePresentRequest& req) {
    vkrpc::QueuePresentResponse resp;
    std::uint64_t queue_device = 0;
    VkQueue queue = VK_NULL_HANDLE;
    if (!resolve_queue(req.queue, queue_device, queue)) {
        resp.ok = false;
        resp.reason = "unknown queue handle";
        return resp;
    }
    if (req.presents.empty()) {
        resp.ok = false;
        resp.reason = "no present targets";
        return resp;
    }
    // Lost-device latch: answer locally, never re-enter the host.
    if (device_lost_latched(queue_device)) {
        resp.ok = true;
        resp.reason = "ok";
        resp.result = vkrpc::kVkErrorDeviceLost;
        resp.results.assign(req.presents.size(), vkrpc::kVkErrorDeviceLost);
        return resp;
    }
    // Resolve wait semaphores (optional). The debug path is CPU-synchronized, so
    // it passes none; a real app's render-done semaphores pass through here. Each must be
    // a semaphore on the present queue's device (a foreign-device semaphore is an
    // RPC/handle fault, not something to hand the host driver) -- mirrors acquire.
    std::vector<VkSemaphore> wait_sems;
    for (const std::uint64_t s : req.wait_semaphores) {
        const auto leaf = leaves_.find(s);
        if (leaf == leaves_.end() || leaf->second.kind != LeafKind::Semaphore ||
            leaf->second.device != queue_device) {
            resp.ok = false;
            resp.reason = "wait semaphore is not a semaphore on the present queue's device";
            return resp;
        }
        wait_sems.push_back(leaf->second.semaphore);
    }

    // Validate every target (known swapchain, on the queue's device, in-range index)
    // before any state change. Collect the unique surfaces to lock + dirty-check.
    struct Target {
        VkSwapchainKHR vk = VK_NULL_HANDLE;
        std::uint32_t image_index = 0;
        std::uint64_t surface = 0;
        windowing::WindowSlot* slot = nullptr;
    };
    std::vector<Target> targets;
    targets.reserve(req.presents.size());
    for (const vkrpc::PresentEntry& e : req.presents) {
        const auto sc = swapchains_.find(e.swapchain);
        if (sc == swapchains_.end()) {
            resp.ok = false;
            resp.reason = "unknown swapchain handle";
            return resp;
        }
        if (sc->second.device != queue_device) {
            resp.ok = false;
            resp.reason = "present queue and swapchain are on different devices";
            return resp;
        }
        if (!ensure_swapchain_images(e.swapchain, sc->second, resp.reason)) {
            resp.ok = false;
            return resp;
        }
        if (e.image_index < 0 ||
            static_cast<std::size_t>(e.image_index) >= sc->second.images.size()) {
            resp.ok = false;
            resp.reason = "image index out of range for swapchain";
            return resp;
        }
        const auto surf = surfaces_.find(sc->second.surface);
        Target t;
        t.vk = sc->second.vk;
        t.image_index = static_cast<std::uint32_t>(e.image_index);
        t.surface = sc->second.surface;
        t.slot = (surf != surfaces_.end()) ? surf->second.slot : nullptr;
        targets.push_back(t);
    }

    // lock the participating surface slots in a deterministic order (ascending
    // surface handle, de-duplicated) to avoid a lock-ordering deadlock.
    std::vector<std::uint64_t> surfaces_sorted;
    for (const Target& t : targets) {
        surfaces_sorted.push_back(t.surface);
    }
    std::sort(surfaces_sorted.begin(), surfaces_sorted.end());
    surfaces_sorted.erase(std::unique(surfaces_sorted.begin(), surfaces_sorted.end()),
                          surfaces_sorted.end());
    std::vector<std::unique_lock<std::mutex>> locks;
    for (const std::uint64_t s : surfaces_sorted) {
        const auto surf = surfaces_.find(s);
        if (surf != surfaces_.end() && surf->second.slot != nullptr) {
            locks.emplace_back(surf->second.slot->slot_lock());
        }
    }

    // if ANY target surface is dirty, skip the present entirely and report
    // OUT_OF_DATE for the whole batch (no per-target driver result exists).
    bool any_dirty = false;
    for (const Target& t : targets) {
        if (t.slot != nullptr && t.slot->geometry_dirty()) {
            any_dirty = true;
            break;
        }
    }
    resp.ok = true;
    resp.reason = "ok";
    if (any_dirty) {
        resp.result = vkrpc::kVkErrorOutOfDateKhr;
        resp.results.assign(req.presents.size(), vkrpc::kVkErrorOutOfDateKhr);
        return resp;
    }
    if (!window_thread_) {
        resp.ok = false;
        resp.reason = "window thread unavailable";
        return resp;
    }

    std::vector<VkSwapchainKHR> chains;
    std::vector<std::uint32_t> indices;
    for (const Target& t : targets) {
        chains.push_back(t.vk);
        indices.push_back(t.image_index);
    }
    std::vector<VkResult> results(targets.size(), VK_ERROR_OUT_OF_DATE_KHR);
    VkResult overall = VK_ERROR_OUT_OF_DATE_KHR;

    // present-capture (gated, debug): resolve the front target's image to copy BEFORE
    // presenting it -- a presented image is no longer app-owned until re-acquired,
    // so the copy must precede vkQueuePresentKHR. Arm only when VKRELAY2_DEBUG_DUMP_PRESENT is set,
    // the swapchain carries the worker-added TRANSFER_SRC, the per-run frame cap is not yet hit,
    // and the image resolves. swapchains_ is data-plane (single-threaded), so reading it here is
    // safe.
    VkImage capture_image = VK_NULL_HANDLE;
    VkExtent2D capture_extent{};
    VkFormat capture_format = VK_FORMAT_UNDEFINED;
    VkImageUsageFlags capture_usage = 0;
    if (std::getenv("VKRELAY2_DEBUG_DUMP_PRESENT") != nullptr && present_capture_seq_ < 64) {
        const auto sc = swapchains_.find(req.presents.front().swapchain);
        if (sc != swapchains_.end() && sc->second.transfer_src &&
            targets.front().image_index < sc->second.vk_images.size()) {
            capture_image = sc->second.vk_images[targets.front().image_index];
            capture_extent = sc->second.extent;
            capture_format = sc->second.image_format;
            capture_usage = sc->second.image_usage;
        }
    }

    // cross-adapter retirement: when the device enabled swapchainMaintenance1 (privately),
    // attach one worker-private fence per target swapchain to this present
    // (VkSwapchainPresentFenceInfoEXT) -- the spec's signal that THAT present's resources can be
    // freed. A later recreate waits the retiring swapchain's outstanding fences before the
    // oldSwapchain handoff. First reap already-signaled fences per target (vkGetFenceStatus) so
    // the outstanding list stays ~swapchain-depth. All-or-nothing per batch: if any fence create
    // fails, present without the chain (the retirement wait then has nothing new to wait, which is
    // simply the pre-experiment behavior).
    const auto pdev = devices_.find(queue_device);
    const bool fence_retire = pdev != devices_.end() && pdev->second.present_fence_retire;
    VkDevice pdev_vk = pdev != devices_.end() ? pdev->second.vk : VK_NULL_HANDLE;
    std::vector<VkFence> present_fences;
    VkResult fence_setup_result = VK_SUCCESS;
    if (fence_retire) {
        for (const vkrpc::PresentEntry& e : req.presents) {
            const auto sc = swapchains_.find(e.swapchain);
            if (sc == swapchains_.end()) {
                continue;
            }
            auto& outstanding = sc->second.present_fences;
            for (auto f = outstanding.begin(); f != outstanding.end();) {
                const VkResult status =
                    observe_device_result(queue_device, vkGetFenceStatus(pdev_vk, *f),
                                          "vkGetFenceStatus (present-fence retire)");
                if (status == VK_ERROR_DEVICE_LOST) {
                    fence_setup_result = status;
                    break;
                }
                if (status == VK_SUCCESS) {
                    vkDestroyFence(pdev_vk, *f, nullptr);
                    f = outstanding.erase(f);
                } else {
                    ++f;
                }
            }
            if (fence_setup_result == VK_ERROR_DEVICE_LOST) {
                break;
            }
        }
        if (fence_setup_result == VK_SUCCESS) {
            VkFenceCreateInfo fci{};
            fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            for (std::size_t i = 0; i < chains.size(); ++i) {
                VkFence f = VK_NULL_HANDLE;
                // Defensive beyond-spec observation: vkCreateFence does not list DEVICE_LOST, but
                // a driver already in loss recovery may still report it.
                const VkResult fr =
                    observe_device_result(queue_device, vkCreateFence(pdev_vk, &fci, nullptr, &f),
                                          "vkCreateFence (present-fence retire)");
                if (fr != VK_SUCCESS) {
                    fence_setup_result = fr;
                    if (fr != VK_ERROR_DEVICE_LOST) {
                        for (VkFence made : present_fences) {
                            vkDestroyFence(pdev_vk, made, nullptr);
                        }
                    }
                    present_fences.clear();
                    break;
                }
                present_fences.push_back(f);
            }
        }
    }
    if (fence_setup_result == VK_ERROR_DEVICE_LOST) {
        resp.result = vkrpc::kVkErrorDeviceLost;
        resp.results.assign(req.presents.size(), vkrpc::kVkErrorDeviceLost);
        return resp;
    }

    const bool routed = window_thread_->invoke([&]() {
        // Pre-present capture (if armed): record + submit the copy WAITING the app's present-wait
        // semaphores, SIGNALING its own (cap.done); on success the present chains on cap.done
        // instead of re-waiting the app's now-consumed binary semaphores. A begin failure leaves
        // the present on the app's own semaphores, so a capture fault can never affect rendering.
        PresentCapture cap;
        std::vector<VkSemaphore> present_waits = wait_sems;
        if (capture_image != VK_NULL_HANDLE && capture_extent.width != 0 &&
            capture_extent.height != 0 &&
            begin_present_capture(queue_device, queue, capture_image, capture_extent,
                                  capture_format, capture_usage, wait_sems, cap)) {
            present_waits.assign(1, cap.done);
        }
        if (device_lost_latched(queue_device)) {
            overall = VK_ERROR_DEVICE_LOST;
            std::fill(results.begin(), results.end(), VK_ERROR_DEVICE_LOST);
            return; // present-capture submit surfaced loss; never enter present/wait afterward
        }
        VkPresentInfoKHR pi{};
        pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        pi.waitSemaphoreCount = static_cast<std::uint32_t>(present_waits.size());
        pi.pWaitSemaphores = present_waits.empty() ? nullptr : present_waits.data();
        pi.swapchainCount = static_cast<std::uint32_t>(chains.size());
        pi.pSwapchains = chains.data();
        pi.pImageIndices = indices.data();
        pi.pResults = results.data();
        VkSwapchainPresentFenceInfoEXT pfi{};
        if (present_fences.size() == chains.size() && !present_fences.empty()) {
            pfi.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT;
            pfi.swapchainCount = static_cast<std::uint32_t>(present_fences.size());
            pfi.pFences = present_fences.data();
            pi.pNext = &pfi;
        }
        overall =
            observe_device_result(queue_device, vkQueuePresentKHR(queue, &pi), "vkQueuePresentKHR");
        for (VkResult& r : results) {
            r = observe_device_result(queue_device, r, "vkQueuePresentKHR (per-swapchain result)");
        }
        // After present consumed cap.done: wait the queue idle, write the artifact, free resources.
        // A lost present never enters the wait; its temporary objects are abandoned with the lost
        // device. If the capture wait itself reports loss, that becomes the overall present result.
        if (!device_lost_latched(queue_device)) {
            const VkResult capture_result = end_present_capture(queue, cap);
            if (capture_result == VK_ERROR_DEVICE_LOST) {
                overall = capture_result;
            }
        }
    });
    // Record the attached fences as outstanding on their swapchains (the retirement wait consumes
    // them). On a failed ROUTE (no present happened) destroy them instead.
    if (present_fences.size() == chains.size() && !present_fences.empty()) {
        if (routed) {
            for (std::size_t i = 0; i < req.presents.size() && i < present_fences.size(); ++i) {
                const auto sc = swapchains_.find(req.presents[i].swapchain);
                if (sc != swapchains_.end()) {
                    sc->second.present_fences.push_back(present_fences[i]);
                }
            }
        } else {
            for (VkFence f : present_fences) {
                vkDestroyFence(pdev_vk, f, nullptr);
            }
        }
    }
    if (!routed) {
        resp.ok = false;
        resp.reason = "window thread unavailable";
        return resp;
    }
    resp.result = static_cast<int>(overall);
    for (const VkResult r : results) {
        resp.results.push_back(static_cast<int>(r));
    }

    // device-loss probe: describe each present target's monitor + the adapter DRIVING it, and
    // compare to the render adapter. Log on a MONITOR CHANGE (a drag onto another monitor) or on
    // VK_ERROR_DEVICE_LOST -- steady-state is quiet, but every monitor transition AND the loss are
    // captured with the cross-GPU signal. Device loss is sticky, so the last target logged here is
    // the likely trigger even when a subsequent submit is what returns the loss to the guest.
    const bool present_device_lost = (overall == VK_ERROR_DEVICE_LOST);
    for (std::size_t i = 0; i < targets.size(); ++i) {
        const auto surf = surfaces_.find(targets[i].surface);
        if (surf == surfaces_.end() || surf->second.hwnd == nullptr) {
            continue;
        }
        const VkResult tr = (i < results.size()) ? results[i] : overall;
        const bool this_lost = present_device_lost || tr == VK_ERROR_DEVICE_LOST;
        const PresentTargetInfo pt = describe_present_target(surf->second.hwnd);
        if (pt.monitor != surf->second.dl_last_monitor || this_lost) {
            const bool cross = !pt.adapter.empty() && !render_adapter_name_.empty() &&
                               pt.adapter.find(render_adapter_name_) == std::string::npos &&
                               render_adapter_name_.find(pt.adapter) == std::string::npos;
            VKR_INFO(kComponent) << "device-loss probe: present xid=" << surf->second.xid
                                 << " result=" << static_cast<int>(tr) << " monitor=" << pt.monitor
                                 << " " << pt.w << "x" << pt.h << "@" << pt.x << "," << pt.y
                                 << " dpi=" << pt.dpi << " target-adapter='" << pt.adapter
                                 << "' render-adapter='" << render_adapter_name_ << "'"
                                 << (cross ? " [target-adapter != render-adapter]" : "")
                                 << (this_lost ? " [VK_ERROR_DEVICE_LOST on present]" : "");
            surf->second.dl_last_monitor = pt.monitor;
        }
    }

    // Show-on-first-present: reveal each target's surface on its first successful present
    // (VK_SUCCESS or VK_SUBOPTIMAL_KHR) -- never on a dirty/failed present.
    if (overall == VK_SUCCESS || overall == VK_SUBOPTIMAL_KHR) {
        for (const Target& t : targets) {
            const auto surf = surfaces_.find(t.surface);
            if (surf == surfaces_.end()) {
                continue;
            }
            // resize-diag: a successful present is forward progress -> reset the no-progress
            // counter ONLY. If we were mid-stall (a real no-progress run), log the recovery.
            // Do NOT reset rz_last_extent here -- preserving it across presents is
            // what rate-limits the log (an unchanged pinned extent must not re-log every steady
            // frame).
            if (surf->second.rz_cycles > 30) {
                VKR_INFO(kComponent) << "resize-diag: present resumed for xid=" << surf->second.xid
                                     << " after " << surf->second.rz_cycles << " caps re-queries";
            }
            surf->second.rz_cycles = 0;
            if (!surf->second.shown && surf->second.hwnd != nullptr) {
                window_thread_->show_window(surf->second.hwnd);
                surf->second.shown = true;
            }
        }
    }
    return resp;
}

HWND RealVulkanBackend::debug_surface_hwnd(std::uint64_t surface) const {
    const auto it = surfaces_.find(surface);
    return (it != surfaces_.end()) ? it->second.hwnd : nullptr;
}

void RealVulkanBackend::latch_device_lost(std::uint64_t device, const char* where) {
    const auto d = devices_.find(device);
    if (d == devices_.end() || d->second.lost) {
        return; // unknown, or already latched (log the transition only)
    }
    d->second.lost = true;
    // Taint the parent instance STICKILY: the lost device is abandoned undestroyed, so the
    // instance's own host teardown (surfaces + vkDestroyInstance) must be skipped too -- tearing
    // the instance down under a live abandoned device child is UB (observed AV).
    const auto inst = instances_.find(d->second.instance);
    if (inst != instances_.end()) {
        inst->second.lost_device_taint = true;
    }
    VKR_WARN(kComponent) << "device " << device << " LOST (latched at " << where
                         << "): all later acquire/submit/present/wait/recreate on it answer "
                            "VK_ERROR_DEVICE_LOST locally; the host driver is never re-entered";
}

VkResult RealVulkanBackend::observe_device_result(std::uint64_t device, VkResult result,
                                                  const char* where) {
    if (result == VK_ERROR_DEVICE_LOST) {
        latch_device_lost(device, where);
    }
    return result;
}

int RealVulkanBackend::debug_observe_device_result(std::uint64_t device, int result,
                                                   const char* where) {
    return static_cast<int>(observe_device_result(device, static_cast<VkResult>(result), where));
}

void RealVulkanBackend::log_device_lost_on_submit(
    const std::set<std::uint64_t>& referenced_surfaces, const char* which) {
    VKR_WARN(kComponent) << "device-loss probe: VK_ERROR_DEVICE_LOST on " << which
                         << " (render-adapter='" << render_adapter_name_
                         << "' luid=" << (render_adapter_luid_.empty() ? "?" : render_adapter_luid_)
                         << ")";
    for (const std::uint64_t s : referenced_surfaces) {
        const auto surf = surfaces_.find(s);
        if (surf == surfaces_.end() || surf->second.hwnd == nullptr) {
            continue;
        }
        const PresentTargetInfo pt = describe_present_target(surf->second.hwnd);
        const bool cross = !pt.adapter.empty() && !render_adapter_name_.empty() &&
                           pt.adapter.find(render_adapter_name_) == std::string::npos &&
                           render_adapter_name_.find(pt.adapter) == std::string::npos;
        VKR_WARN(kComponent) << "device-loss probe:   referenced xid=" << surf->second.xid
                             << " monitor=" << pt.monitor << " target-adapter='" << pt.adapter
                             << "'" << (cross ? " [target-adapter != render-adapter]" : "");
    }
}

bool RealVulkanBackend::resolve_surface_query(std::uint64_t physical_device, std::uint64_t surface,
                                              VkPhysicalDevice& out_phys, VkSurfaceKHR& out_surface,
                                              windowing::WindowSlot*& out_slot, std::string& err) {
    const auto surf = surfaces_.find(surface);
    if (surf == surfaces_.end()) {
        err = "unknown surface handle";
        return false;
    }
    const auto inst = instances_.find(surf->second.instance);
    if (inst == instances_.end() || inst->second.physical == 0 ||
        inst->second.physical != physical_device || inst->second.physical_vk == VK_NULL_HANDLE) {
        err = "physical device was not enumerated from this surface's instance";
        return false;
    }
    out_phys = inst->second.physical_vk;
    out_surface = surf->second.vk;
    out_slot = surf->second.slot;
    return true;
}

vkrpc::GetSurfaceCapabilitiesResponse
RealVulkanBackend::get_surface_capabilities(const vkrpc::GetSurfaceCapabilitiesRequest& req) {
    vkrpc::GetSurfaceCapabilitiesResponse resp;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkSurfaceKHR vksurf = VK_NULL_HANDLE;
    windowing::WindowSlot* slot = nullptr;
    if (!resolve_surface_query(req.physical_device, req.surface, phys, vksurf, slot, resp.reason)) {
        resp.ok = false;
        return resp;
    }
    if (!window_thread_) {
        resp.ok = false;
        resp.reason = "window thread unavailable";
        return resp;
    }
    // if the sidecar has authored a live extent for this surface's toplevel, pin
    // currentExtent to it so the app converges there. Read the registry authority under the backend
    // mutex BEFORE the window-thread invoke (never hold it across an invoke). The surface->xid map
    // is data-plane (single-threaded), so reading surfaces_ here is unguarded; only the registry
    // needs the lock.
    const std::uint64_t surface_xid = surfaces_.count(req.surface) ? surfaces_[req.surface].xid : 0;
    sidecar::WindowRegistry::AuthoritativeExtent authority;
    {
        std::lock_guard<std::mutex> lk(backend_mutex_);
        authority = registry_.authoritative_extent_for_xid(surface_xid);
    }
    // hold the per-surface slot lock across the routed host query (the query touches the
    // Win32 surface/HWND); run it on the window thread.
    std::unique_lock<std::mutex> slot_lk;
    if (slot != nullptr) {
        slot_lk = std::unique_lock<std::mutex>(slot->slot_lock());
    }
    HWND diag_hwnd = surfaces_.count(req.surface) ? surfaces_[req.surface].hwnd : nullptr;
    VkSurfaceCapabilitiesKHR caps{};
    VkPhysicalDeviceProperties physical_props{};
    VkResult qr = VK_ERROR_SURFACE_LOST_KHR;
    const bool routed = window_thread_->invoke([&]() {
        qr = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, vksurf, &caps);
        vkGetPhysicalDeviceProperties(phys, &physical_props);
    });
    if (!routed) {
        resp.ok = false;
        resp.reason = "window thread unavailable";
        return resp;
    }
    if (qr != VK_SUCCESS) {
        resp.ok = false;
        resp.reason = "vkGetPhysicalDeviceSurfaceCapabilitiesKHR failed";
        return resp;
    }
    resp.min_image_count = caps.minImageCount;
    resp.max_image_count = caps.maxImageCount;
    // A surface PINS its currentExtent (min == max == current) -- the Vulkan "the surface dictates
    // this exact extent" -- in two cases:
    //   * defers_extent: the app created a swapchain deferring the extent (use_current_extent;
    //   zink/
    //     kopper does this). kopper loops forever recreating when currentExtent is the 0xFFFFFFFF
    //     sentinel (it keeps deferring + re-checking against the X11 drawable and never converges);
    //     a concrete, stable currentExtent makes it converge on the first create.
    //   * authority.active: the sidecar has authored a live extent.
    // In both we pin to the REAL host caps.currentExtent (the realizable client extent, already
    // clamped to what Win32 can create), NOT a raw requested/authored size -- so the app never
    // strands in an OUT_OF_DATE retry loop chasing a size it can never reach.
    //
    // Otherwise (a native app that chose a concrete extent, never deferring) keep the app-picks
    // range: a sentinel currentExtent + a permissive [1x1 .. min(accepted guest root, stable
    // physical-device image/framebuffer limit)] range, so the app sizes the swapchain to its own
    // framebuffer and the worker resizes the host window to match at create. Win32 surface
    // min/maxImageExtent describe the CURRENT HWND here (256x256 before a resize), not a stable
    // future-size bound, so they cannot govern this dynamic relay range. No monitor work area
    // participates: the frozen session root is the guest-realizable policy authority. (The
    // The stale-window fix remains: never pin a window with no live authority.)
    const bool surf_defers =
        surfaces_.count(req.surface) ? surfaces_[req.surface].defers_extent : false;
    const bool pin_concrete = surf_defers || authority.active;
    // worker invariant guard: a pinned (deferred/zink) surface's
    // currentExtent must never exceed the OBSERVED guest root. If it does -- a programmatic path
    // that slipped past both the WM_GETMINMAXINFO interactive cap and the sidecar resize clamp --
    // DRIVE the real HWND client down to the root and re-query, rather than merely advertising a
    // smaller currentExtent while the HWND stays larger (that just relocates the mismatch to
    // host-client-vs-swapchain). This is the third net; it only fires off the normal path and, once
    // corrected, the HWND is <= root so it does not thrash. Read the root back from the single
    // source of truth the WndProc caps against.
    const std::uint64_t guest_root = windowing::WindowThread::guest_root_packed();
    const std::uint32_t groot_w = static_cast<std::uint32_t>(guest_root >> 32);
    const std::uint32_t groot_h = static_cast<std::uint32_t>(guest_root & 0xFFFFFFFFu);
    if (pin_concrete && groot_w != 0 && groot_h != 0 && diag_hwnd != nullptr &&
        (caps.currentExtent.width > groot_w || caps.currentExtent.height > groot_h)) {
        const int clamp_w = caps.currentExtent.width > groot_w
                                ? static_cast<int>(groot_w)
                                : static_cast<int>(caps.currentExtent.width);
        const int clamp_h = caps.currentExtent.height > groot_h
                                ? static_cast<int>(groot_h)
                                : static_cast<int>(caps.currentExtent.height);
        VKR_WARN(kComponent) << "resize-diag xid=" << surface_xid << " host client "
                             << caps.currentExtent.width << "x" << caps.currentExtent.height
                             << " EXCEEDS guest root " << groot_w << "x" << groot_h
                             << " -> driving HWND client to " << clamp_w << "x" << clamp_h
                             << " (over-root guard)";
        window_thread_->set_client_extent(diag_hwnd, clamp_w, clamp_h);
        VkResult rqr = VK_ERROR_SURFACE_LOST_KHR;
        const bool re_routed = window_thread_->invoke(
            [&]() { rqr = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, vksurf, &caps); });
        if (!re_routed || rqr != VK_SUCCESS) {
            resp.ok = false;
            resp.reason = "re-query after over-root correction failed";
            return resp;
        }
    }
    if (pin_concrete) {
        resp.current_extent_width = caps.currentExtent.width;
        resp.current_extent_height = caps.currentExtent.height;
        resp.min_image_extent_width = caps.currentExtent.width;
        resp.min_image_extent_height = caps.currentExtent.height;
        resp.max_image_extent_width = caps.currentExtent.width;
        resp.max_image_extent_height = caps.currentExtent.height;
    } else {
        const std::uint32_t host_image_limit = physical_props.limits.maxImageDimension2D;
        const std::uint32_t host_width_limit =
            std::min(host_image_limit, physical_props.limits.maxFramebufferWidth);
        const std::uint32_t host_height_limit =
            std::min(host_image_limit, physical_props.limits.maxFramebufferHeight);
        resp.current_extent_width = vkrpc::kDynamicExtentSentinel;
        resp.current_extent_height = vkrpc::kDynamicExtentSentinel;
        resp.min_image_extent_width = 1;
        resp.min_image_extent_height = 1;
        resp.max_image_extent_width =
            sidecar::surface_extent_ceiling_axis(groot_w, host_width_limit);
        resp.max_image_extent_height =
            sidecar::surface_extent_ceiling_axis(groot_h, host_height_limit);
    }
    // resize-diag: for a pinned (deferred/zink) surface, count caps re-queries since the last
    // successful present. The confirming signal is a pinned extent OVER the guest root followed by
    // the present heartbeat FLATLINING (rz_cycles climbs with NO intervening present) -- the
    // app-side stall after a successful oversize resize, NOT a recreate loop. Rate-limited: log on
    // extent change or every 30 no-progress cycles (never per call).
    if (pin_concrete) {
        const auto sit = surfaces_.find(req.surface);
        if (sit != surfaces_.end()) {
            RealSurface& s = sit->second;
            s.rz_cycles++;
            const std::uint32_t pe_w = static_cast<std::uint32_t>(resp.current_extent_width);
            const std::uint32_t pe_h = static_cast<std::uint32_t>(resp.current_extent_height);
            const bool extent_changed = pe_w != s.rz_last_extent_w || pe_h != s.rz_last_extent_h;
            // Base the flag on the OBSERVED guest root (the authority), not a
            // fresh primary-work re-probe. The guard above already corrects an over-root pin, so
            // this normally reports pinned <= guest-root; a residual over-root here (guest root
            // unknown, or a correction that did not converge) still surfaces.
            const bool over_guest_root =
                groot_w != 0 && groot_h != 0 && (pe_w > groot_w || pe_h > groot_h);
            if (extent_changed || s.rz_cycles % 30 == 0) {
                VKR_INFO(kComponent) << "resize-diag xid=" << surface_xid << " pinned=" << pe_w
                                     << "x" << pe_h << " guest-root=" << groot_w << "x" << groot_h
                                     << " cycles-since-present=" << s.rz_cycles
                                     << (over_guest_root ? " [pinned OVER guest root]" : "");
                s.rz_last_extent_w = pe_w;
                s.rz_last_extent_h = pe_h;
            }
        }
    }
    VKR_INFO(kComponent) << "get_surface_capabilities: surface=" << req.surface
                         << " xid=" << surface_xid << " extent "
                         << (pin_concrete ? "PINNED " : "range [1x1 .. ")
                         << resp.max_image_extent_width << "x" << resp.max_image_extent_height
                         << (pin_concrete ? (authority.active
                                                 ? " (sidecar authority, realizable currentExtent)"
                                                 : " (deferred-extent app, pinned currentExtent)")
                                          : "] (guest-root/device-limit realizable ceiling)");
    resp.max_image_array_layers = caps.maxImageArrayLayers;
    resp.supported_transforms = caps.supportedTransforms;
    resp.current_transform = caps.currentTransform;
    resp.supported_composite_alpha = caps.supportedCompositeAlpha;
    resp.supported_usage_flags = caps.supportedUsageFlags; // honest (incl. TRANSFER_DST)
    resp.ok = true;
    resp.reason = "ok";
    return resp;
}

vkrpc::GetSurfaceFormatsResponse
RealVulkanBackend::get_surface_formats(const vkrpc::GetSurfaceFormatsRequest& req) {
    vkrpc::GetSurfaceFormatsResponse resp;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkSurfaceKHR vksurf = VK_NULL_HANDLE;
    windowing::WindowSlot* slot = nullptr;
    if (!resolve_surface_query(req.physical_device, req.surface, phys, vksurf, slot, resp.reason)) {
        resp.ok = false;
        return resp;
    }
    if (!window_thread_) {
        resp.ok = false;
        resp.reason = "window thread unavailable";
        return resp;
    }
    std::unique_lock<std::mutex> slot_lk; // (the query touches the Win32 surface)
    if (slot != nullptr) {
        slot_lk = std::unique_lock<std::mutex>(slot->slot_lock());
    }
    VkResult qr = VK_ERROR_SURFACE_LOST_KHR;
    std::vector<VkSurfaceFormatKHR> formats;
    const bool routed = window_thread_->invoke([&]() {
        std::uint32_t count = 0;
        qr = vkGetPhysicalDeviceSurfaceFormatsKHR(phys, vksurf, &count, nullptr);
        if ((qr == VK_SUCCESS || qr == VK_INCOMPLETE) && count > 0) {
            formats.resize(count);
            qr = vkGetPhysicalDeviceSurfaceFormatsKHR(phys, vksurf, &count, formats.data());
            formats.resize(count);
        }
    });
    if (!routed) {
        resp.ok = false;
        resp.reason = "window thread unavailable";
        return resp;
    }
    if (qr != VK_SUCCESS && qr != VK_INCOMPLETE) {
        resp.ok = false;
        resp.reason = "vkGetPhysicalDeviceSurfaceFormatsKHR failed";
        return resp;
    }
    for (const VkSurfaceFormatKHR& f : formats) {
        resp.formats.push_back({static_cast<int>(f.format), static_cast<int>(f.colorSpace)});
    }
    resp.ok = true;
    resp.reason = "ok";
    return resp;
}

vkrpc::GetSurfacePresentModesResponse
RealVulkanBackend::get_surface_present_modes(const vkrpc::GetSurfacePresentModesRequest& req) {
    vkrpc::GetSurfacePresentModesResponse resp;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkSurfaceKHR vksurf = VK_NULL_HANDLE;
    windowing::WindowSlot* slot = nullptr;
    if (!resolve_surface_query(req.physical_device, req.surface, phys, vksurf, slot, resp.reason)) {
        resp.ok = false;
        return resp;
    }
    if (!window_thread_) {
        resp.ok = false;
        resp.reason = "window thread unavailable";
        return resp;
    }
    std::unique_lock<std::mutex> slot_lk; // (the query touches the Win32 surface)
    if (slot != nullptr) {
        slot_lk = std::unique_lock<std::mutex>(slot->slot_lock());
    }
    VkResult qr = VK_ERROR_SURFACE_LOST_KHR;
    std::vector<VkPresentModeKHR> modes;
    const bool routed = window_thread_->invoke([&]() {
        std::uint32_t count = 0;
        qr = vkGetPhysicalDeviceSurfacePresentModesKHR(phys, vksurf, &count, nullptr);
        if ((qr == VK_SUCCESS || qr == VK_INCOMPLETE) && count > 0) {
            modes.resize(count);
            qr = vkGetPhysicalDeviceSurfacePresentModesKHR(phys, vksurf, &count, modes.data());
            modes.resize(count);
        }
    });
    if (!routed) {
        resp.ok = false;
        resp.reason = "window thread unavailable";
        return resp;
    }
    if (qr != VK_SUCCESS && qr != VK_INCOMPLETE) {
        resp.ok = false;
        resp.reason = "vkGetPhysicalDeviceSurfacePresentModesKHR failed";
        return resp;
    }
    for (const VkPresentModeKHR m : modes) {
        resp.present_modes.push_back(static_cast<int>(m));
    }
    resp.ok = true;
    resp.reason = "ok";
    return resp;
}

vkrpc::GetSurfaceSupportResponse
RealVulkanBackend::get_surface_support(const vkrpc::GetSurfaceSupportRequest& req) {
    vkrpc::GetSurfaceSupportResponse resp;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkSurfaceKHR vksurf = VK_NULL_HANDLE;
    windowing::WindowSlot* slot = nullptr;
    if (!resolve_surface_query(req.physical_device, req.surface, phys, vksurf, slot, resp.reason)) {
        resp.ok = false;
        return resp;
    }
    // The worker exposes one queue family (its graphics family) as app-facing index 0.
    if (req.queue_family_index != 0) {
        resp.ok = false;
        resp.reason = "queue family index out of range (the worker exposes one family)";
        return resp;
    }
    if (!window_thread_) {
        resp.ok = false;
        resp.reason = "window thread unavailable";
        return resp;
    }
    std::unique_lock<std::mutex> slot_lk; // (the query touches the Win32 surface)
    if (slot != nullptr) {
        slot_lk = std::unique_lock<std::mutex>(slot->slot_lock());
    }
    VkResult qr = VK_ERROR_SURFACE_LOST_KHR;
    VkBool32 supported = VK_FALSE;
    const bool routed = window_thread_->invoke([&]() {
        const int fam = graphics_queue_family(phys);
        if (fam < 0) {
            qr = VK_ERROR_INITIALIZATION_FAILED;
            return;
        }
        qr = vkGetPhysicalDeviceSurfaceSupportKHR(phys, static_cast<std::uint32_t>(fam), vksurf,
                                                  &supported);
    });
    if (!routed) {
        resp.ok = false;
        resp.reason = "window thread unavailable";
        return resp;
    }
    if (qr != VK_SUCCESS) {
        resp.ok = false;
        resp.reason = "vkGetPhysicalDeviceSurfaceSupportKHR failed";
        return resp;
    }
    resp.supported = (supported == VK_TRUE);
    resp.ok = true;
    resp.reason = "ok";
    return resp;
}

// --- Draw surface ---------------------------------------
// Real host-driver implementations of the bufferless-triangle draw spine. The bounded create-info
// allowlist mirrors the mock (strict-but-total) so the two backends accept/reject the same streams;
// these create device-level objects (no HWND), so -- unlike present/record -- they run directly on
// the session thread (like create_command_pool / create_fence). A `<vulkan_core.h>` enum value used
// for validation is spelled with its VK_* name; the mock spells the same numbers out.

vkrpc::CreateImageViewResponse
RealVulkanBackend::create_image_view(const vkrpc::CreateImageViewRequest& req) {
    vkrpc::CreateImageViewResponse resp;
    const auto img = images_.find(req.image);
    if (img == images_.end()) {
        resp.ok = false;
        resp.reason = "unknown image handle (must be a live swapchain or app image)";
        return resp;
    }
    // (GL/zink): FAITHFUL image view -- any viewType / format (mutable-format aliasing) /
    // swizzle / aspect / mip+layer range, forwarded verbatim; the host driver is the authoritative
    // gate. (The vkcube subset forced 2D / matching format / identity swizzle / single
    // subresource.)
    if (img->second.app_created && img->second.bound_memory == 0) {
        resp.ok = false;
        resp.reason = "image view over an app image that is not bound to memory";
        return resp;
    }
    const auto dev = devices_.find(img->second.device);
    if (dev == devices_.end() || dev->second.vk == VK_NULL_HANDLE) {
        resp.ok = false;
        resp.reason = "image has no live device";
        return resp;
    }
    VkImageViewCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    ci.image = img->second.vk;
    ci.viewType = static_cast<VkImageViewType>(req.view_type);
    ci.format = static_cast<VkFormat>(req.format);
    ci.components.r = static_cast<VkComponentSwizzle>(req.swizzle_r);
    ci.components.g = static_cast<VkComponentSwizzle>(req.swizzle_g);
    ci.components.b = static_cast<VkComponentSwizzle>(req.swizzle_b);
    ci.components.a = static_cast<VkComponentSwizzle>(req.swizzle_a);
    ci.subresourceRange.aspectMask = static_cast<VkImageAspectFlags>(req.aspect);
    ci.subresourceRange.baseMipLevel = static_cast<std::uint32_t>(req.base_mip_level);
    ci.subresourceRange.levelCount =
        req.level_count < 0 ? VK_REMAINING_MIP_LEVELS : static_cast<std::uint32_t>(req.level_count);
    ci.subresourceRange.baseArrayLayer = static_cast<std::uint32_t>(req.base_array_layer);
    ci.subresourceRange.layerCount = req.layer_count < 0
                                         ? VK_REMAINING_ARRAY_LAYERS
                                         : static_cast<std::uint32_t>(req.layer_count);
    // (GL/zink): the app chained a VkImageViewUsageCreateInfo (usage-narrowing view); rebuild
    // it on the real create. The host driver is the authoritative gate on the narrowed set.
    VkImageViewUsageCreateInfo usage_ci{};
    if (req.view_usage != 0) {
        usage_ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO;
        usage_ci.usage = static_cast<VkImageUsageFlags>(req.view_usage);
        ci.pNext = &usage_ci;
    }
    VkImageView view = VK_NULL_HANDLE;
    const VkResult vr = vkCreateImageView(dev->second.vk, &ci, nullptr, &view);
    if (vr != VK_SUCCESS || view == VK_NULL_HANDLE) {
        resp.ok = false;
        resp.reason = "vkCreateImageView failed (VkResult " + std::to_string(vr) + ")";
        return resp;
    }
    const std::uint64_t h = next_handle_++;
    RealImageView iv;
    iv.vk = view;
    iv.device = img->second.device;
    iv.image = req.image;
    iv.swapchain = img->second.swapchain;
    iv.format = static_cast<VkFormat>(req.format); // (DR): the attachment-format compat key
    image_views_.emplace(h, iv);
    dev->second.image_views.insert(h);
    const auto sc = swapchains_.find(img->second.swapchain);
    if (sc != swapchains_.end()) {
        sc->second.image_views.insert(h); // swapchain -> view (blocks the swapchain's destroy)
    }
    if (img->second.app_created) {
        img->second.image_views.insert(h); // app image -> view (blocks the image's destroy)
    }
    resp.ok = true;
    resp.reason = "ok";
    resp.image_view = h;
    return resp;
}

void RealVulkanBackend::invalidate_cbs_referencing(std::uint64_t handle) {
    for (auto& kv : command_buffers_) {
        if (kv.second.referenced_draw_objects.count(handle) != 0) {
            kv.second.recorded = false;
            kv.second.referenced_surfaces.clear();
            kv.second.referenced_swapchains.clear();
            kv.second.referenced_draw_objects.clear();
        }
    }
}

void RealVulkanBackend::dangle_sets_referencing(std::uint64_t resource) {
    for (auto& [set_handle, set] : descriptor_sets_) {
        const auto sl = descriptor_set_layouts_.find(set.layout);
        bool affected = false;
        for (auto& [binding, slots] : set.slots) {
            // descriptorIndexing: a PARTIALLY_BOUND or UPDATE_AFTER_BIND
            // binding's dangle must NOT invalidate recorded CBs -- destroying a
            // not-dynamically-used resource (PARTIALLY_BOUND) or destroy-old/update-to-new before
            // submit (UAB) are spec-legal; the host judges the LIVE set at submit. The slot is
            // still cleared (bookkeeping: the referent is gone). Classic bindings keep the
            // fail-closed invalidate (mock == real).
            long long binding_flags = 0;
            if (sl != descriptor_set_layouts_.end()) {
                for (const RealDescriptorSetLayoutBinding& b : sl->second.bindings) {
                    if (b.binding == binding) {
                        binding_flags = b.binding_flags;
                        break;
                    }
                }
            }
            const bool host_owned =
                (binding_flags & (vkrpc::kDescriptorBindingPartiallyBound |
                                  vkrpc::kDescriptorBindingUpdateAfterBind)) != 0;
            for (RealDescriptorSlot& slot : slots) {
                // A repoint away from this resource before the destroy means no slot references it,
                // so nothing is invalidated -- correct, since the recorded CB binds the live set
                // handle, not a snapshot.
                if (slot.initialized &&
                    (slot.buffer == resource || slot.sampler == resource ||
                     slot.image_view == resource || slot.buffer_view == resource)) {
                    slot.initialized = false;
                    slot.buffer = 0;
                    slot.sampler = 0;
                    slot.image_view = 0;
                    slot.buffer_view = 0;
                    if (!host_owned) {
                        affected = true;
                    }
                }
            }
        }
        if (affected) {
            invalidate_cbs_referencing(set_handle);
        }
    }
}

vkrpc::StatusResponse RealVulkanBackend::destroy_image_view(const vkrpc::HandleRequest& req) {
    vkrpc::StatusResponse resp;
    const auto it = image_views_.find(req.handle);
    if (it == image_views_.end()) {
        resp.ok = false;
        resp.reason = "unknown image view handle";
        return resp;
    }
    invalidate_cbs_referencing(req.handle);
    // CB -> set -> image-view destroy consult: a combined-image-sampler slot
    // currently referencing this view is marked dangling + its CBs invalidated (mirrors the buffer
    // consult). A texture image cannot itself be destroyed while a view lives, so the view is the
    // descriptor's effective referent.
    dangle_sets_referencing(req.handle);
    const auto dev = devices_.find(it->second.device);
    if (dev != devices_.end() && dev->second.vk != VK_NULL_HANDLE) {
        vkDestroyImageView(dev->second.vk, it->second.vk, nullptr);
        dev->second.image_views.erase(req.handle);
    }
    const auto sc = swapchains_.find(it->second.swapchain);
    if (sc != swapchains_.end()) {
        sc->second.image_views.erase(req.handle);
    }
    const auto img = images_.find(it->second.image); // app image -> view link
    if (img != images_.end()) {
        img->second.image_views.erase(req.handle);
    }
    image_views_.erase(it);
    resp.ok = true;
    resp.reason = "ok";
    return resp;
}

vkrpc::CreateBufferViewResponse
RealVulkanBackend::create_buffer_view(const vkrpc::CreateBufferViewRequest& req) {
    // (GL/zink): faithful texel buffer view. The source buffer must exist + be bound; the
    // create-info (format/offset/range) is forwarded verbatim to the real host driver. No vkcube
    // subset -- zink is a trusted producer.
    vkrpc::CreateBufferViewResponse resp;
    const auto buf = buffers_.find(req.buffer);
    if (buf == buffers_.end()) {
        resp.ok = false;
        resp.reason = "unknown buffer handle";
        return resp;
    }
    if (buf->second.bound_memory == 0) {
        resp.ok = false;
        resp.reason = "buffer view over a buffer not bound to memory";
        return resp;
    }
    const auto dev = devices_.find(buf->second.device);
    if (dev == devices_.end() || dev->second.vk == VK_NULL_HANDLE) {
        resp.ok = false;
        resp.reason = "buffer has no live device";
        return resp;
    }
    VkBufferViewCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO;
    ci.buffer = buf->second.vk;
    ci.format = static_cast<VkFormat>(req.format);
    ci.offset = static_cast<VkDeviceSize>(req.offset);
    if (req.range == vkrpc::kVkWholeSize) {
        if (req.offset >= buf->second.size) {
            resp.ok = false;
            resp.reason = "buffer view VK_WHOLE_SIZE offset is outside the logical buffer";
            return resp;
        }
        ci.range = buf->second.size - static_cast<VkDeviceSize>(req.offset);
    } else {
        if (req.range == 0 || req.offset > buf->second.size ||
            req.range > buf->second.size - static_cast<VkDeviceSize>(req.offset)) {
            resp.ok = false;
            resp.reason = "buffer view range is outside the logical buffer";
            return resp;
        }
        ci.range = static_cast<VkDeviceSize>(req.range);
    }
    VkBufferView view = VK_NULL_HANDLE;
    if (vkCreateBufferView(dev->second.vk, &ci, nullptr, &view) != VK_SUCCESS ||
        view == VK_NULL_HANDLE) {
        resp.ok = false;
        resp.reason = "vkCreateBufferView failed";
        return resp;
    }
    const std::uint64_t h = next_handle_++;
    RealBufferView bv;
    bv.vk = view;
    bv.device = buf->second.device;
    bv.buffer = req.buffer;
    buffer_views_.emplace(h, bv);
    resp.ok = true;
    resp.reason = "ok";
    resp.buffer_view = h;
    return resp;
}

vkrpc::StatusResponse RealVulkanBackend::destroy_buffer_view(const vkrpc::HandleRequest& req) {
    vkrpc::StatusResponse resp;
    const auto it = buffer_views_.find(req.handle);
    if (it == buffer_views_.end()) {
        resp.ok = false;
        resp.reason = "unknown buffer view handle";
        return resp;
    }
    invalidate_cbs_referencing(req.handle);
    dangle_sets_referencing(req.handle); // texel-buffer descriptor slots referencing this view
    const auto dev = devices_.find(it->second.device);
    if (dev != devices_.end() && dev->second.vk != VK_NULL_HANDLE) {
        vkDestroyBufferView(dev->second.vk, it->second.vk, nullptr);
    }
    buffer_views_.erase(it);
    resp.ok = true;
    resp.reason = "ok";
    return resp;
}

vkrpc::CreateShaderModuleResponse
RealVulkanBackend::create_shader_module(const vkrpc::CreateShaderModuleRequest& req) {
    vkrpc::CreateShaderModuleResponse resp;
    const auto dev = devices_.find(req.device);
    if (dev == devices_.end() || dev->second.vk == VK_NULL_HANDLE) {
        resp.ok = false;
        resp.reason = "unknown device handle";
        return resp;
    }
    if (req.code_size == 0 || (req.code_size % 4) != 0 ||
        req.code_size > vkrpc::kMaxShaderCodeBytes || req.code.size() != req.code_size) {
        resp.ok = false;
        resp.reason = "malformed SPIR-V (size must be > 0, a multiple of 4, within the cap, and "
                      "match the payload length)";
        return resp;
    }
    // Copy into a u32 buffer: SPIR-V pCode must be 4-byte aligned, which std::string data is not
    // guaranteed to be.
    std::vector<std::uint32_t> words(static_cast<std::size_t>(req.code_size) / 4);
    std::memcpy(words.data(), req.code.data(), static_cast<std::size_t>(req.code_size));
    VkShaderModuleCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = static_cast<std::size_t>(req.code_size);
    ci.pCode = words.data();
    VkShaderModule sm = VK_NULL_HANDLE;
    if (vkCreateShaderModule(dev->second.vk, &ci, nullptr, &sm) != VK_SUCCESS ||
        sm == VK_NULL_HANDLE) {
        resp.ok = false;
        resp.reason = "vkCreateShaderModule failed";
        return resp;
    }
    const std::uint64_t h = next_handle_++;
    RealShaderModule m;
    m.vk = sm;
    m.device = req.device;
    shader_modules_.emplace(h, m);
    dev->second.shader_modules.insert(h);
    resp.ok = true;
    resp.reason = "ok";
    resp.shader_module = h;
    return resp;
}

vkrpc::StatusResponse RealVulkanBackend::destroy_shader_module(const vkrpc::HandleRequest& req) {
    vkrpc::StatusResponse resp;
    const auto it = shader_modules_.find(req.handle);
    if (it == shader_modules_.end()) {
        resp.ok = false;
        resp.reason = "unknown shader module handle";
        return resp;
    }
    const auto dev = devices_.find(it->second.device);
    if (dev != devices_.end() && dev->second.vk != VK_NULL_HANDLE) {
        vkDestroyShaderModule(dev->second.vk, it->second.vk, nullptr);
        dev->second.shader_modules.erase(req.handle);
    }
    shader_modules_.erase(it);
    resp.ok = true;
    resp.reason = "ok";
    return resp;
}

vkrpc::CreateRenderPassResponse
RealVulkanBackend::create_render_pass(const vkrpc::CreateRenderPassRequest& req) {
    vkrpc::CreateRenderPassResponse resp;
    const auto dev = devices_.find(req.device);
    if (dev == devices_.end() || dev->second.vk == VK_NULL_HANDLE) {
        resp.ok = false;
        resp.reason = "unknown device handle";
        return resp;
    }
    // Required-feature audit: a viewMask (multiview) render pass is admitted ONLY on a
    // device that enabled the multiview feature -- the fail-closed gate (mock == real; the safety
    // net for a hostile/stale RPC, since a conformant ICD only sends view_mask != 0 for such a
    // device). A negative mask is a malformed frame.
    if (req.view_mask != 0) {
        if (!dev->second.multiview_feature_enabled) {
            resp.ok = false;
            resp.reason = "render pass viewMask (multiview) requires the multiview feature enabled";
            return resp;
        }
        if (req.view_mask < 0) {
            resp.ok = false;
            resp.reason = "render pass viewMask is negative (malformed)";
            return resp;
        }
    }
    // (GL/zink): FAITHFUL single-subpass render pass. The attachments (any format / samples
    // samples, load/store operations, and layouts), the colour refs + optional depth ref, and the
    // dependencies are
    // built verbatim from the request and the host driver gates them. The vkcube subset
    // (PRESENT_SRC_KHR / D16 / CLEAR+STORE) is lifted -- zink renders to FBO targets with arbitrary
    // formats/layouts. MRT: a non-empty color_refs vector is the subpass's FULL ordered ref array
    // (UNUSED holes as -1); an empty one is a legacy single-scalar request (old-ICD compat).
    // (GL/zink): a DEPTH-ONLY pass (no colour ref at all) is legal -- zink emits one for a
    // shadow-map FBO (glmark2 [shadow]); at least one of colour/depth must exist.
    const bool has_depth = req.depth_attachment >= 0;
    const bool vector_refs = !req.color_refs.empty();
    bool any_used_color = false;
    if (vector_refs) {
        // MRT: honest host limit + per-ref range/layout validation (UNUSED holes are legal).
        VkPhysicalDeviceProperties pd_props{};
        const auto inst = instances_.find(dev->second.instance);
        if (inst == instances_.end() || inst->second.physical_vk == VK_NULL_HANDLE) {
            resp.ok = false;
            resp.reason = "render pass device has no resolvable physical device";
            return resp;
        }
        vkGetPhysicalDeviceProperties(inst->second.physical_vk, &pd_props);
        if (req.color_refs.size() > pd_props.limits.maxColorAttachments) {
            resp.ok = false;
            resp.reason = "render pass color attachment count exceeds the device limit";
            return resp;
        }
        for (const vkrpc::ColorRefDesc& cr : req.color_refs) {
            if (!vkrpc::color_ref_used(cr)) {
                continue;
            }
            if (cr.attachment < 0 ||
                static_cast<std::size_t>(cr.attachment) >= req.attachments.size() ||
                cr.layout < 0) {
                resp.ok = false;
                resp.reason = "render pass color ref out of range or with an invalid layout";
                return resp;
            }
            any_used_color = true;
        }
    }
    const bool has_color = vector_refs ? any_used_color : req.color_attachment >= 0;
    if (req.attachments.empty() || (!has_color && !has_depth) ||
        (!vector_refs && has_color &&
         static_cast<std::size_t>(req.color_attachment) >= req.attachments.size()) ||
        (has_depth && static_cast<std::size_t>(req.depth_attachment) >= req.attachments.size())) {
        resp.ok = false;
        resp.reason = "render pass needs >=1 attachment and an in-range colour/depth ref";
        return resp;
    }
    std::vector<VkAttachmentDescription> atts;
    for (const vkrpc::AttachmentDesc& ad : req.attachments) {
        VkAttachmentDescription d{};
        d.format = static_cast<VkFormat>(ad.format);
        d.samples = static_cast<VkSampleCountFlagBits>(ad.samples > 0 ? ad.samples : 1);
        d.loadOp = static_cast<VkAttachmentLoadOp>(ad.load_op);
        d.storeOp = static_cast<VkAttachmentStoreOp>(ad.store_op);
        d.stencilLoadOp = static_cast<VkAttachmentLoadOp>(ad.stencil_load_op);
        d.stencilStoreOp = static_cast<VkAttachmentStoreOp>(ad.stencil_store_op);
        d.initialLayout = static_cast<VkImageLayout>(ad.initial_layout);
        d.finalLayout = static_cast<VkImageLayout>(ad.final_layout);
        atts.push_back(d);
    }
    std::vector<VkSubpassDependency> deps;
    for (const vkrpc::SubpassDependencyDesc& sd : req.dependencies) {
        VkSubpassDependency vd{};
        vd.srcSubpass = static_cast<std::uint32_t>(sd.src_subpass);
        vd.dstSubpass = static_cast<std::uint32_t>(sd.dst_subpass);
        vd.srcStageMask = static_cast<VkPipelineStageFlags>(sd.src_stage);
        vd.dstStageMask = static_cast<VkPipelineStageFlags>(sd.dst_stage);
        vd.srcAccessMask = static_cast<VkAccessFlags>(sd.src_access);
        vd.dstAccessMask = static_cast<VkAccessFlags>(sd.dst_access);
        vd.dependencyFlags =
            static_cast<VkDependencyFlags>(sd.dependency_flags >= 0 ? sd.dependency_flags : 0);
        deps.push_back(vd);
    }
    // MRT: build the FULL reference vector when carried (UNUSED holes as VK_ATTACHMENT_UNUSED);
    // legacy scalar requests keep the old single-ref build byte-for-byte.
    std::vector<VkAttachmentReference> color_refs_vk;
    if (vector_refs) {
        for (const vkrpc::ColorRefDesc& cr : req.color_refs) {
            VkAttachmentReference r{};
            if (vkrpc::color_ref_used(cr)) {
                r.attachment = static_cast<std::uint32_t>(cr.attachment);
                r.layout = static_cast<VkImageLayout>(cr.layout);
            } else {
                r.attachment = VK_ATTACHMENT_UNUSED;
                r.layout = VK_IMAGE_LAYOUT_UNDEFINED;
            }
            color_refs_vk.push_back(r);
        }
    } else if (has_color) {
        VkAttachmentReference r{};
        r.attachment = static_cast<std::uint32_t>(req.color_attachment);
        r.layout = static_cast<VkImageLayout>(req.color_layout);
        color_refs_vk.push_back(r);
    }
    VkAttachmentReference depth_ref{};
    if (has_depth) {
        depth_ref.attachment = static_cast<std::uint32_t>(req.depth_attachment);
        depth_ref.layout = static_cast<VkImageLayout>(req.depth_layout);
    }
    // The compat key stays the FIRST USED color ref's format (single-color semantics unchanged).
    VkFormat color_format = VK_FORMAT_UNDEFINED;
    for (const VkAttachmentReference& r : color_refs_vk) {
        if (r.attachment != VK_ATTACHMENT_UNUSED) {
            color_format = static_cast<VkFormat>(req.attachments[r.attachment].format);
            break;
        }
    }
    const VkFormat depth_format =
        has_depth ? static_cast<VkFormat>(
                        req.attachments[static_cast<std::size_t>(req.depth_attachment)].format)
                  : VK_FORMAT_UNDEFINED;
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = static_cast<std::uint32_t>(color_refs_vk.size());
    subpass.pColorAttachments = color_refs_vk.empty() ? nullptr : color_refs_vk.data();
    subpass.pDepthStencilAttachment = has_depth ? &depth_ref : nullptr;
    VkRenderPassCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    ci.attachmentCount = static_cast<std::uint32_t>(atts.size());
    ci.pAttachments = atts.data();
    ci.subpassCount = 1;
    ci.pSubpasses = &subpass;
    ci.dependencyCount = static_cast<std::uint32_t>(deps.size());
    ci.pDependencies = deps.empty() ? nullptr : deps.data();
    // Required-feature audit: a viewMask (multiview) render pass is built with a REAL
    // VkRenderPassMultiviewCreateInfo chained on the v1 create info -- the core-1.1 way to express
    // a single-subpass multiview pass (pViewMasks[0] = the subpass mask; correlated masks stay 0,
    // the ICD subset rejects any correlation). The host driver enforces every multiview VUID from
    // here on (incl. the framebuffer attachment layer count, VUID-02531). The mask outlives this
    // scope (view_masks storage), so keep it alive until after vkCreateRenderPass.
    const std::uint32_t view_mask_u = static_cast<std::uint32_t>(req.view_mask);
    VkRenderPassMultiviewCreateInfo mv{};
    if (req.view_mask != 0) {
        mv.sType = VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO;
        mv.subpassCount = 1;
        mv.pViewMasks = &view_mask_u;
        ci.pNext = &mv;
    }
    VkRenderPass rp = VK_NULL_HANDLE;
    const VkResult rpr = vkCreateRenderPass(dev->second.vk, &ci, nullptr, &rp);
    if (rpr != VK_SUCCESS || rp == VK_NULL_HANDLE) {
        resp.ok = false;
        resp.reason = "vkCreateRenderPass failed (VkResult " + std::to_string(rpr) + ")";
        return resp;
    }
    const std::uint64_t h = next_handle_++;
    RealRenderPass r;
    r.vk = rp;
    r.device = req.device;
    r.color_format = color_format;
    r.depth_format = depth_format;
    // MRT metadata: the subpass shape framebuffer-view + pipeline-blend validation
    // compare against (empty color_refs = a legacy single-color pass).
    r.color_refs = req.color_refs;
    for (const vkrpc::AttachmentDesc& ad : req.attachments) {
        r.attachment_formats.push_back(static_cast<VkFormat>(ad.format));
    }
    r.view_mask = req.view_mask; // (0 = non-multiview)
    render_passes_.emplace(h, r);
    dev->second.render_passes.insert(h);
    resp.ok = true;
    resp.reason = "ok";
    resp.render_pass = h;
    return resp;
}

vkrpc::StatusResponse RealVulkanBackend::destroy_render_pass(const vkrpc::HandleRequest& req) {
    vkrpc::StatusResponse resp;
    const auto it = render_passes_.find(req.handle);
    if (it == render_passes_.end()) {
        resp.ok = false;
        resp.reason = "unknown render pass handle";
        return resp;
    }
    invalidate_cbs_referencing(req.handle);
    const auto dev = devices_.find(it->second.device);
    if (dev != devices_.end() && dev->second.vk != VK_NULL_HANDLE) {
        vkDestroyRenderPass(dev->second.vk, it->second.vk, nullptr);
        dev->second.render_passes.erase(req.handle);
    }
    render_passes_.erase(it);
    resp.ok = true;
    resp.reason = "ok";
    return resp;
}

vkrpc::CreateFramebufferResponse
RealVulkanBackend::create_framebuffer(const vkrpc::CreateFramebufferRequest& req) {
    vkrpc::CreateFramebufferResponse resp;
    const auto dev = devices_.find(req.device);
    if (dev == devices_.end() || dev->second.vk == VK_NULL_HANDLE) {
        resp.ok = false;
        resp.reason = "unknown device handle";
        return resp;
    }
    const auto rp = render_passes_.find(req.render_pass);
    if (rp == render_passes_.end() || rp->second.device != req.device) {
        resp.ok = false;
        resp.reason = "framebuffer render pass is unknown or on a different device";
        return resp;
    }
    // A new client forwards the complete VkFramebufferAttachmentsCreateInfo and receives a real
    // host imageless framebuffer. This matters during resize: materializing a regular framebuffer
    // at begin time can make an otherwise deferred attachment transition invalid before the host
    // driver sees the imageless semantics. Empty attachment_infos is the old-client compatibility
    // path and retains the former begin-time emulation.
    if (req.imageless) {
        if (req.width <= 0 || req.height <= 0 || req.layers <= 0) {
            resp.ok = false;
            resp.reason = "imageless framebuffer needs a positive extent and layer count";
            return resp;
        }
        VkFramebuffer native = VK_NULL_HANDLE;
        const bool native_imageless = !req.attachment_infos.empty();
        VkExtent2D host_extent{static_cast<std::uint32_t>(req.width),
                               static_cast<std::uint32_t>(req.height)};
        if (native_imageless) {
            if (req.attachment_count != static_cast<int>(req.attachment_infos.size())) {
                resp.ok = false;
                resp.reason =
                    "imageless framebuffer attachment count disagrees with attachment metadata";
                return resp;
            }
            std::vector<std::vector<VkFormat>> format_lists;
            format_lists.reserve(req.attachment_infos.size());
            for (const auto& src : req.attachment_infos) {
                if (src.width <= 0 || src.height <= 0 || src.layer_count <= 0 ||
                    src.view_formats.empty()) {
                    resp.ok = false;
                    resp.reason = "imageless framebuffer attachment metadata is malformed";
                    return resp;
                }
                std::vector<VkFormat> formats;
                formats.reserve(src.view_formats.size());
                for (const int format : src.view_formats) {
                    formats.push_back(static_cast<VkFormat>(format));
                }
                format_lists.push_back(std::move(formats));
            }
            const vkrpc::FramebufferExtentDesc safe_extent =
                vkrpc::host_safe_framebuffer_extent(req.width, req.height, req.attachment_infos);
            host_extent = {static_cast<std::uint32_t>(safe_extent.width),
                           static_cast<std::uint32_t>(safe_extent.height)};
            std::vector<VkFramebufferAttachmentImageInfo> image_infos;
            image_infos.reserve(req.attachment_infos.size());
            for (std::size_t i = 0; i < req.attachment_infos.size(); ++i) {
                const auto& src = req.attachment_infos[i];
                VkFramebufferAttachmentImageInfo dst{};
                dst.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_ATTACHMENT_IMAGE_INFO;
                dst.flags = static_cast<VkImageCreateFlags>(src.flags);
                dst.usage = static_cast<VkImageUsageFlags>(src.usage);
                dst.width = static_cast<std::uint32_t>(src.width);
                dst.height = static_cast<std::uint32_t>(src.height);
                dst.layerCount = static_cast<std::uint32_t>(src.layer_count);
                dst.viewFormatCount = static_cast<std::uint32_t>(format_lists[i].size());
                dst.pViewFormats = format_lists[i].empty() ? nullptr : format_lists[i].data();
                image_infos.push_back(dst);
            }
            VkFramebufferAttachmentsCreateInfo attachments{};
            attachments.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_ATTACHMENTS_CREATE_INFO;
            attachments.attachmentImageInfoCount = static_cast<std::uint32_t>(image_infos.size());
            attachments.pAttachmentImageInfos = image_infos.data();
            VkFramebufferCreateInfo fci{};
            fci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            fci.pNext = &attachments;
            fci.flags = VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT;
            fci.renderPass = rp->second.vk;
            fci.attachmentCount = static_cast<std::uint32_t>(req.attachment_count);
            fci.width = host_extent.width;
            fci.height = host_extent.height;
            fci.layers = static_cast<std::uint32_t>(req.layers);
            if (host_extent.width != static_cast<std::uint32_t>(req.width) ||
                host_extent.height != static_cast<std::uint32_t>(req.height)) {
                VKR_WARN(kComponent)
                    << "bounded malformed imageless framebuffer: requested=" << req.width << "x"
                    << req.height << " attachment-envelope=" << host_extent.width << "x"
                    << host_extent.height;
            }
            const VkResult fr = vkCreateFramebuffer(dev->second.vk, &fci, nullptr, &native);
            if (fr != VK_SUCCESS || native == VK_NULL_HANDLE) {
                resp.ok = false;
                resp.reason = "native imageless vkCreateFramebuffer failed (VkResult " +
                              std::to_string(static_cast<int>(fr)) + ")";
                return resp;
            }
        }
        const std::uint64_t h = next_handle_++;
        RealFramebuffer fb;
        fb.vk = native;
        fb.device = req.device;
        fb.render_pass = req.render_pass;
        fb.compat_color_format = rp->second.color_format; // compat snapshot (pass may die)
        fb.compat_depth_format = rp->second.depth_format;
        fb.extent = {static_cast<std::uint32_t>(req.width), static_cast<std::uint32_t>(req.height)};
        fb.host_extent = host_extent;
        fb.imageless = true;
        fb.native_imageless = native_imageless;
        fb.layers = static_cast<std::uint32_t>(req.layers > 0 ? req.layers : 1);
        fb.view_mask = rp->second.view_mask;        // snapshot for the begin-time layer check
        fb.attachment_count = req.attachment_count; // MRT: the begin must supply exactly this many
        framebuffers_.emplace(h, fb);
        dev->second.framebuffers.insert(h);
        resp.ok = true;
        resp.reason = "ok";
        resp.framebuffer = h;
        return resp;
    }
    // MRT: a positional view vector (attachment_views non-empty) is validated against the render
    // pass's attachment metadata slot by slot -- count, liveness, and per-position format. The
    // legacy scalar shape (empty vector: old ICD) keeps the exact old color+depth path below.
    if (!req.attachment_views.empty()) {
        if (req.attachment_views.size() != rp->second.attachment_formats.size()) {
            resp.ok = false;
            resp.reason = "framebuffer view count does not match the render pass attachment count";
            return resp;
        }
        if (req.width <= 0 || req.height <= 0 || req.layers != 1) {
            resp.ok = false;
            resp.reason = "framebuffer outside the supported subset (one layer, positive extent)";
            return resp;
        }
        std::vector<VkImageView> views_vk;
        for (std::size_t i = 0; i < req.attachment_views.size(); ++i) {
            const auto v = image_views_.find(req.attachment_views[i]);
            if (v == image_views_.end() || v->second.device != req.device) {
                resp.ok = false;
                resp.reason = "framebuffer attachment view is unknown or on a different device";
                return resp;
            }
            const auto vimg = images_.find(v->second.image);
            if (vimg == images_.end() || vimg->second.format != rp->second.attachment_formats[i]) {
                resp.ok = false;
                resp.reason =
                    "framebuffer view format does not match the render pass attachment (position)";
                return resp;
            }
            // Required-feature audit: explicit multiview layer-sufficiency pre-check (a
            // clear error + mock parity; the host vkCreateFramebuffer below is the authoritative
            // VUID-02531 enforcer).
            if (static_cast<int>(vimg->second.array_layers) <
                vkrpc::multiview_required_layers(rp->second.view_mask)) {
                resp.ok = false;
                resp.reason = "framebuffer attachment has too few array layers for the render pass "
                              "viewMask (multiview)";
                return resp;
            }
            views_vk.push_back(v->second.vk);
        }
        VkFramebufferCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fci.renderPass = rp->second.vk;
        fci.attachmentCount = static_cast<std::uint32_t>(views_vk.size());
        fci.pAttachments = views_vk.data();
        fci.width = static_cast<std::uint32_t>(req.width);
        fci.height = static_cast<std::uint32_t>(req.height);
        fci.layers = 1;
        VkFramebuffer built = VK_NULL_HANDLE;
        if (vkCreateFramebuffer(dev->second.vk, &fci, nullptr, &built) != VK_SUCCESS ||
            built == VK_NULL_HANDLE) {
            resp.ok = false;
            resp.reason = "vkCreateFramebuffer failed";
            return resp;
        }
        const std::uint64_t h = next_handle_++;
        RealFramebuffer f;
        f.vk = built;
        f.device = req.device;
        f.render_pass = req.render_pass;
        f.compat_color_format = rp->second.color_format; // compat snapshot (pass may die)
        f.compat_depth_format = rp->second.depth_format;
        f.image_view = req.attachment_views[0]; // legacy field: position 0 (liveness re-checks)
        f.attachment_views = req.attachment_views;
        f.extent = {static_cast<std::uint32_t>(req.width), static_cast<std::uint32_t>(req.height)};
        f.host_extent = f.extent;
        f.view_mask = rp->second.view_mask; // (0 = non-multiview)
        framebuffers_.emplace(h, f);
        dev->second.framebuffers.insert(h);
        resp.ok = true;
        resp.reason = "ok";
        resp.framebuffer = h;
        return resp;
    }
    const auto iv = image_views_.find(req.image_view);
    if (iv == image_views_.end() || iv->second.device != req.device) {
        resp.ok = false;
        resp.reason = "framebuffer image view is unknown or on a different device";
        return resp;
    }
    const auto img = images_.find(iv->second.image);
    if (img == images_.end() || img->second.format != rp->second.color_format) {
        resp.ok = false;
        resp.reason = "framebuffer image view format does not match the render pass attachment";
        return resp;
    }
    const auto sc = swapchains_.find(img->second.swapchain);
    if (req.width <= 0 || req.height <= 0 || req.layers != 1) {
        resp.ok = false;
        resp.reason = "framebuffer outside the supported subset (one layer, positive extent)";
        return resp;
    }
    if (sc == swapchains_.end() || req.width != static_cast<int>(sc->second.extent.width) ||
        req.height != static_cast<int>(sc->second.extent.height)) {
        resp.ok = false;
        resp.reason = "framebuffer extent does not match the swapchain extent";
        return resp;
    }
    // Depth attachment: the framebuffer must carry a depth view iff the render pass
    // has a depth attachment; the depth view's image format/aspect must match and its extent the
    // fb's.
    std::vector<VkImageView> attachments;
    attachments.push_back(iv->second.vk);
    if (rp->second.depth_format != VK_FORMAT_UNDEFINED) {
        const auto div = image_views_.find(req.depth_image_view);
        if (req.depth_image_view == 0 || div == image_views_.end() ||
            div->second.device != req.device) {
            resp.ok = false;
            resp.reason = "framebuffer for a depth render pass needs a depth view on the device";
            return resp;
        }
        const auto dimg = images_.find(div->second.image);
        if (dimg == images_.end() || !dimg->second.app_created ||
            dimg->second.format != rp->second.depth_format ||
            dimg->second.aspect != VK_IMAGE_ASPECT_DEPTH_BIT) {
            resp.ok = false;
            resp.reason = "framebuffer depth view does not match the render pass depth attachment";
            return resp;
        }
        if (dimg->second.extent.width != static_cast<std::uint32_t>(req.width) ||
            dimg->second.extent.height != static_cast<std::uint32_t>(req.height)) {
            resp.ok = false;
            resp.reason = "framebuffer depth view extent does not match the framebuffer extent";
            return resp;
        }
        attachments.push_back(div->second.vk);
    } else if (req.depth_image_view != 0) {
        resp.ok = false;
        resp.reason =
            "framebuffer carries a depth view but the render pass has no depth attachment";
        return resp;
    }
    VkFramebufferCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    ci.renderPass = rp->second.vk;
    ci.attachmentCount = static_cast<std::uint32_t>(attachments.size());
    ci.pAttachments = attachments.data();
    ci.width = static_cast<std::uint32_t>(req.width);
    ci.height = static_cast<std::uint32_t>(req.height);
    ci.layers = 1;
    VkFramebuffer fb = VK_NULL_HANDLE;
    if (vkCreateFramebuffer(dev->second.vk, &ci, nullptr, &fb) != VK_SUCCESS ||
        fb == VK_NULL_HANDLE) {
        resp.ok = false;
        resp.reason = "vkCreateFramebuffer failed";
        return resp;
    }
    const std::uint64_t h = next_handle_++;
    RealFramebuffer f;
    f.vk = fb;
    f.device = req.device;
    f.render_pass = req.render_pass;
    f.compat_color_format = rp->second.color_format; // compat snapshot (pass may die)
    f.compat_depth_format = rp->second.depth_format;
    f.image_view = req.image_view;
    f.depth_image_view = req.depth_image_view;
    f.extent = {static_cast<std::uint32_t>(req.width), static_cast<std::uint32_t>(req.height)};
    f.host_extent = f.extent;
    f.view_mask = rp->second.view_mask; // (host vkCreateFramebuffer enforces layer count)
    framebuffers_.emplace(h, f);
    dev->second.framebuffers.insert(h);
    resp.ok = true;
    resp.reason = "ok";
    resp.framebuffer = h;
    return resp;
}

vkrpc::StatusResponse RealVulkanBackend::destroy_framebuffer(const vkrpc::HandleRequest& req) {
    vkrpc::StatusResponse resp;
    const auto it = framebuffers_.find(req.handle);
    if (it == framebuffers_.end()) {
        resp.ok = false;
        resp.reason = "unknown framebuffer handle";
        return resp;
    }
    invalidate_cbs_referencing(req.handle);
    const auto dev = devices_.find(it->second.device);
    if (dev != devices_.end() && dev->second.vk != VK_NULL_HANDLE) {
        if (it->second.vk != VK_NULL_HANDLE) {
            vkDestroyFramebuffer(dev->second.vk, it->second.vk, nullptr);
        }
        for (auto& ce : it->second.imageless_cache) { // (GL/zink): imageless cache
            vkDestroyFramebuffer(dev->second.vk, ce.second, nullptr);
        }
        dev->second.framebuffers.erase(req.handle);
    }
    framebuffers_.erase(it);
    resp.ok = true;
    resp.reason = "ok";
    return resp;
}

vkrpc::CreatePipelineLayoutResponse
RealVulkanBackend::create_pipeline_layout(const vkrpc::CreatePipelineLayoutRequest& req) {
    vkrpc::CreatePipelineLayoutResponse resp;
    const auto dev = devices_.find(req.device);
    if (dev == devices_.end() || dev->second.vk == VK_NULL_HANDLE) {
        resp.ok = false;
        resp.reason = "unknown device handle";
        return resp;
    }
    //  +: the layout may reference set layouts AND push-constant ranges. The
    // carried counts must agree with the lists; each set layout must be known on this device.
    if (req.push_constant_range_count != static_cast<int>(req.push_constant_ranges.size())) {
        resp.ok = false;
        resp.reason = "pipeline layout push_constant_range_count disagrees with the carried list";
        return resp;
    }
    if (req.set_layout_count != static_cast<int>(req.set_layouts.size())) {
        resp.ok = false;
        resp.reason = "pipeline layout set_layout_count disagrees with the carried set-layout list";
        return resp;
    }
    if (req.set_layouts.size() > static_cast<std::size_t>(vkrpc::kMaxPipelineLayoutSetLayouts)) {
        resp.ok = false;
        resp.reason = "pipeline layout has too many set layouts"; // mock parity
        return resp;
    }
    std::vector<VkDescriptorSetLayout> vk_set_layouts;
    for (const std::uint64_t sl : req.set_layouts) {
        const auto it = descriptor_set_layouts_.find(sl);
        if (it == descriptor_set_layouts_.end() || it->second.device != req.device) {
            resp.ok = false;
            resp.reason = "pipeline layout references a set layout not on the device";
            return resp;
        }
        vk_set_layouts.push_back(it->second.vk);
    }
    std::vector<VkPushConstantRange> vk_push_ranges;
    for (const vkrpc::PushConstantRange& pc : req.push_constant_ranges) {
        VkPushConstantRange r2{};
        r2.stageFlags = static_cast<VkShaderStageFlags>(pc.stage_flags);
        r2.offset = pc.offset;
        r2.size = pc.size;
        vk_push_ranges.push_back(r2);
    }
    VkPipelineLayoutCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    ci.setLayoutCount = static_cast<std::uint32_t>(vk_set_layouts.size());
    ci.pSetLayouts = vk_set_layouts.empty() ? nullptr : vk_set_layouts.data();
    ci.pushConstantRangeCount = static_cast<std::uint32_t>(vk_push_ranges.size());
    ci.pPushConstantRanges = vk_push_ranges.empty() ? nullptr : vk_push_ranges.data();
    VkPipelineLayout pl = VK_NULL_HANDLE;
    if (vkCreatePipelineLayout(dev->second.vk, &ci, nullptr, &pl) != VK_SUCCESS ||
        pl == VK_NULL_HANDLE) {
        resp.ok = false;
        resp.reason = "vkCreatePipelineLayout failed";
        return resp;
    }
    const std::uint64_t h = next_handle_++;
    RealPipelineLayout l;
    l.vk = pl;
    l.device = req.device;
    l.set_layouts = req.set_layouts;
    pipeline_layouts_.emplace(h, l);
    dev->second.pipeline_layouts.insert(h);
    resp.ok = true;
    resp.reason = "ok";
    resp.pipeline_layout = h;
    return resp;
}

vkrpc::StatusResponse RealVulkanBackend::destroy_pipeline_layout(const vkrpc::HandleRequest& req) {
    vkrpc::StatusResponse resp;
    const auto it = pipeline_layouts_.find(req.handle);
    if (it == pipeline_layouts_.end()) {
        resp.ok = false;
        resp.reason = "unknown pipeline layout handle";
        return resp;
    }
    // A recorded bind_descriptor_sets bakes the pipeline-layout handle -> destroying
    // it invalidates any recorded CB that referenced it (matches the mock + pipeline/buffer/pool).
    invalidate_cbs_referencing(req.handle);
    const auto dev = devices_.find(it->second.device);
    if (dev != devices_.end() && dev->second.vk != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(dev->second.vk, it->second.vk, nullptr);
        dev->second.pipeline_layouts.erase(req.handle);
    }
    pipeline_layouts_.erase(it);
    resp.ok = true;
    resp.reason = "ok";
    return resp;
}

// --- Descriptor surface -------------------------------

vkrpc::CreateDescriptorSetLayoutResponse RealVulkanBackend::create_descriptor_set_layout(
    const vkrpc::CreateDescriptorSetLayoutRequest& req) {
    vkrpc::CreateDescriptorSetLayoutResponse resp;
    const auto dev = devices_.find(req.device);
    if (dev == devices_.end() || dev->second.vk == VK_NULL_HANDLE) {
        resp.reason = "unknown device handle";
        return resp;
    }
    // (GL/zink): FAITHFUL descriptor-set layout -- any descriptor type, any stages, an
    // optional empty layout, and the per-binding VkDescriptorBindingFlags (rebuilt into a
    // VkDescriptorSetLayoutBindingFlagsCreateInfo pNext when any flag is nonzero). The host driver
    // is the authoritative gate; the worker keeps only a sane upper bound. UPDATE_AFTER_BIND is
    // structurally absent (descriptor indexing is masked at the capability layer).
    if (req.bindings.size() > static_cast<std::size_t>(vkrpc::kMaxDescriptorSetLayoutBindings)) {
        resp.reason = "descriptor set layout binding count out of bounds";
        return resp;
    }
    // descriptorIndexing: the shared per-binding-flag admission (mock == real), gated on the
    // device's enabled kDIFeature* bits.
    {
        const char* why = "";
        if (!vkrpc::descriptor_indexing_layout_ok(req.layout_flags, req.bindings,
                                                  dev->second.descriptor_indexing_feature_bits,
                                                  &why)) {
            resp.reason = why;
            return resp;
        }
    }
    // Vulkan 1.3 support (inlineUniformBlock): the shared IUB binding admission (mock == real) --
    // gated on the device's enabled inlineUniformBlock bit; descriptorCount is a BYTE size
    // (positive multiple of 4, bounded). The rebuild below passes type + count verbatim.
    {
        const char* why = "";
        if (!vkrpc::inline_uniform_block_layout_ok(req.bindings, dev->second.vk13_feature_bits,
                                                   &why)) {
            resp.reason = why;
            return resp;
        }
    }
    std::vector<VkDescriptorSetLayoutBinding> vk_bindings;
    std::vector<VkDescriptorBindingFlags> vk_binding_flags;
    bool any_binding_flags = false;
    RealDescriptorSetLayout layout;
    layout.device = req.device;
    layout.layout_flags = req.layout_flags; // persisted (mock == real)
    for (const vkrpc::DescriptorSetLayoutBindingDesc& b : req.bindings) {
        if (b.binding < 0 || b.descriptor_count < 0 ||
            b.descriptor_count > vkrpc::kMaxDescriptorCount) {
            resp.reason = "descriptor binding number or count out of bounds";
            return resp;
        }
        VkDescriptorSetLayoutBinding vb{};
        vb.binding = static_cast<std::uint32_t>(b.binding);
        vb.descriptorType = static_cast<VkDescriptorType>(b.descriptor_type);
        vb.descriptorCount = static_cast<std::uint32_t>(b.descriptor_count);
        vb.stageFlags = static_cast<VkShaderStageFlags>(b.stage_flags);
        vk_bindings.push_back(vb);
        vk_binding_flags.push_back(static_cast<VkDescriptorBindingFlags>(b.binding_flags));
        if (b.binding_flags != 0) {
            any_binding_flags = true;
        }
        layout.bindings.push_back(
            {b.binding, b.descriptor_type, b.descriptor_count, b.binding_flags});
    }
    VkDescriptorSetLayoutCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    ci.flags = static_cast<VkDescriptorSetLayoutCreateFlags>(req.layout_flags);
    ci.bindingCount = static_cast<std::uint32_t>(vk_bindings.size());
    ci.pBindings = vk_bindings.empty() ? nullptr : vk_bindings.data();
    VkDescriptorSetLayoutBindingFlagsCreateInfo bf_ci{};
    if (any_binding_flags) {
        bf_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
        bf_ci.bindingCount = static_cast<std::uint32_t>(vk_binding_flags.size());
        bf_ci.pBindingFlags = vk_binding_flags.data();
        ci.pNext = &bf_ci;
    }
    VkDescriptorSetLayout vk = VK_NULL_HANDLE;
    if (vkCreateDescriptorSetLayout(dev->second.vk, &ci, nullptr, &vk) != VK_SUCCESS ||
        vk == VK_NULL_HANDLE) {
        resp.reason = "vkCreateDescriptorSetLayout failed";
        return resp;
    }
    const std::uint64_t h = next_handle_++;
    layout.vk = vk;
    descriptor_set_layouts_.emplace(h, std::move(layout));
    dev->second.descriptor_set_layouts.insert(h);
    resp.ok = true;
    resp.reason = "ok";
    resp.set_layout = h;
    return resp;
}

vkrpc::GetDescriptorSetLayoutSupportResponse RealVulkanBackend::get_descriptor_set_layout_support(
    const vkrpc::CreateDescriptorSetLayoutRequest& req) {
    // descriptorIndexing: the HOST's real answer (supported + maxVariableDescriptorCount) --
    // after the relay's own admission (a layout create would reject is honestly unsupported, not
    // "supported" then refused).
    vkrpc::GetDescriptorSetLayoutSupportResponse resp;
    const auto dev = devices_.find(req.device);
    if (dev == devices_.end() || dev->second.vk == VK_NULL_HANDLE) {
        resp.reason = "unknown device handle";
        return resp;
    }
    resp.ok = true;
    resp.reason = "ok";
    const char* why = "";
    if (req.bindings.size() > static_cast<std::size_t>(vkrpc::kMaxDescriptorSetLayoutBindings) ||
        !vkrpc::descriptor_indexing_layout_ok(req.layout_flags, req.bindings,
                                              dev->second.descriptor_indexing_feature_bits, &why) ||
        !vkrpc::inline_uniform_block_layout_ok(req.bindings, dev->second.vk13_feature_bits, &why)) {
        resp.supported = 0;
        return resp;
    }
    std::vector<VkDescriptorSetLayoutBinding> vk_bindings;
    std::vector<VkDescriptorBindingFlags> vk_binding_flags;
    bool any_binding_flags = false;
    for (const vkrpc::DescriptorSetLayoutBindingDesc& b : req.bindings) {
        if (b.binding < 0 || b.descriptor_count < 0 ||
            b.descriptor_count > vkrpc::kMaxDescriptorCount) {
            resp.supported = 0;
            return resp;
        }
        VkDescriptorSetLayoutBinding vb{};
        vb.binding = static_cast<std::uint32_t>(b.binding);
        vb.descriptorType = static_cast<VkDescriptorType>(b.descriptor_type);
        vb.descriptorCount = static_cast<std::uint32_t>(b.descriptor_count);
        vb.stageFlags = static_cast<VkShaderStageFlags>(b.stage_flags);
        vk_bindings.push_back(vb);
        vk_binding_flags.push_back(static_cast<VkDescriptorBindingFlags>(b.binding_flags));
        if (b.binding_flags != 0) {
            any_binding_flags = true;
        }
    }
    VkDescriptorSetLayoutCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    ci.flags = static_cast<VkDescriptorSetLayoutCreateFlags>(req.layout_flags);
    ci.bindingCount = static_cast<std::uint32_t>(vk_bindings.size());
    ci.pBindings = vk_bindings.empty() ? nullptr : vk_bindings.data();
    VkDescriptorSetLayoutBindingFlagsCreateInfo bf_ci{};
    if (any_binding_flags) {
        bf_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
        bf_ci.bindingCount = static_cast<std::uint32_t>(vk_binding_flags.size());
        bf_ci.pBindingFlags = vk_binding_flags.data();
        ci.pNext = &bf_ci;
    }
    VkDescriptorSetVariableDescriptorCountLayoutSupport vc_support{};
    vc_support.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_LAYOUT_SUPPORT;
    VkDescriptorSetLayoutSupport support{};
    support.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_SUPPORT;
    support.pNext = &vc_support;
    vkGetDescriptorSetLayoutSupport(dev->second.vk, &ci, &support);
    resp.supported = support.supported != VK_FALSE ? 1 : 0;
    resp.max_variable_descriptor_count =
        vkrpc::di_variable_binding(req.bindings) >= 0
            ? static_cast<std::uint64_t>(vc_support.maxVariableDescriptorCount)
            : 0;
    return resp;
}

vkrpc::StatusResponse
RealVulkanBackend::destroy_descriptor_set_layout(const vkrpc::HandleRequest& req) {
    vkrpc::StatusResponse resp;
    const auto it = descriptor_set_layouts_.find(req.handle);
    if (it == descriptor_set_layouts_.end()) {
        resp.ok = false;
        resp.reason = "unknown descriptor set layout handle";
        return resp;
    }
    // Conservative fail-closed (matches the mock): blocked while a live pipeline layout references
    // it or live sets were allocated from it.
    for (const auto& [h, pl] : pipeline_layouts_) {
        (void) h;
        for (const std::uint64_t sl : pl.set_layouts) {
            if (sl == req.handle) {
                resp.ok = false;
                resp.reason = "descriptor set layout is referenced by a live pipeline layout";
                return resp;
            }
        }
    }
    for (const auto& [h, set] : descriptor_sets_) {
        (void) h;
        if (set.layout == req.handle) {
            resp.ok = false;
            resp.reason = "descriptor set layout has live sets allocated from it";
            return resp;
        }
    }
    const auto dev = devices_.find(it->second.device);
    if (dev != devices_.end() && dev->second.vk != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(dev->second.vk, it->second.vk, nullptr);
        dev->second.descriptor_set_layouts.erase(req.handle);
    }
    descriptor_set_layouts_.erase(it);
    resp.ok = true;
    resp.reason = "ok";
    return resp;
}

vkrpc::CreateDescriptorPoolResponse
RealVulkanBackend::create_descriptor_pool(const vkrpc::CreateDescriptorPoolRequest& req) {
    vkrpc::CreateDescriptorPoolResponse resp;
    const auto dev = devices_.find(req.device);
    if (dev == devices_.end() || dev->second.vk == VK_NULL_HANDLE) {
        resp.reason = "unknown device handle";
        return resp;
    }
    if (req.max_sets < 1 || req.max_sets > vkrpc::kMaxDescriptorPoolSets) {
        resp.reason = "descriptor pool maxSets out of bounds";
        return resp;
    }
    // (GL/zink): FAITHFUL descriptor pool -- any pool-size types/counts, any flags (e.g.
    // FREE_DESCRIPTOR_SET), forwarded verbatim; the real pool is the authoritative per-type budget
    // (vkAllocateDescriptorSets returns OUT_OF_POOL_MEMORY when exhausted), so the worker no longer
    // mirrors per-type budgets -- only maxSets accounting stays (a clear early error).
    if (req.pool_sizes.empty()) {
        resp.reason = "descriptor pool must have at least one pool size";
        return resp;
    }
    // descriptorIndexing: the pool UPDATE_AFTER_BIND flag is a CONTAINER
    // flag (limit-bucket selection) -- valid without any UAB feature; recorded so allocate can
    // enforce that a UAB-POOL layout's sets come from such a pool (mock == real).
    const bool pool_uab = (req.flags & vkrpc::kDescriptorPoolCreateUpdateAfterBind) != 0;
    std::vector<VkDescriptorPoolSize> ps;
    for (const vkrpc::DescriptorPoolSizeDesc& s : req.pool_sizes) {
        if (s.descriptor_count < 1) {
            resp.reason = "descriptor pool size must have count >= 1";
            return resp;
        }
        VkDescriptorPoolSize vps{};
        vps.type = static_cast<VkDescriptorType>(s.type);
        vps.descriptorCount = static_cast<std::uint32_t>(s.descriptor_count);
        ps.push_back(vps);
    }
    // Vulkan 1.3 support (inlineUniformBlock): an INLINE_UNIFORM_BLOCK pool size's descriptorCount
    // is a BYTE budget and is only well-formed when VkDescriptorPoolInlineUniformBlockCreateInfo
    // rode the wire (max_inline_uniform_block_bindings > 0, the spec pairing). Either presence is
    // gated on the device's enabled inlineUniformBlock bit -- never chain the struct to a host
    // device that did not enable the feature (fail closed, mock == real).
    bool any_iub_pool_size = false;
    for (const vkrpc::DescriptorPoolSizeDesc& s : req.pool_sizes) {
        if (s.type != vkrpc::kDescriptorTypeInlineUniformBlock) {
            continue;
        }
        any_iub_pool_size = true;
        if ((s.descriptor_count % 4) != 0) {
            resp.reason = "inline-uniform-block pool size must be a multiple of 4 (a byte budget)";
            return resp;
        }
    }
    if ((any_iub_pool_size || req.max_inline_uniform_block_bindings > 0) &&
        (dev->second.vk13_feature_bits & vkrpc::kVk13FeatureInlineUniformBlock) == 0) {
        resp.reason = "inline uniform block requires the enabled inlineUniformBlock feature";
        return resp;
    }
    if (any_iub_pool_size && req.max_inline_uniform_block_bindings <= 0) {
        resp.reason = "an inline-uniform-block pool size requires "
                      "VkDescriptorPoolInlineUniformBlockCreateInfo";
        return resp;
    }
    VkDescriptorPoolCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    ci.flags = static_cast<VkDescriptorPoolCreateFlags>(req.flags);
    ci.maxSets = static_cast<std::uint32_t>(req.max_sets);
    ci.poolSizeCount = static_cast<std::uint32_t>(ps.size());
    ci.pPoolSizes = ps.data();
    // Vulkan 1.3 support (inlineUniformBlock): rebuild the pool pNext the ICD serialized -- stack
    // storage that outlives the vkCreateDescriptorPool call.
    VkDescriptorPoolInlineUniformBlockCreateInfo iub_ci{};
    if (req.max_inline_uniform_block_bindings > 0) {
        iub_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_INLINE_UNIFORM_BLOCK_CREATE_INFO;
        iub_ci.maxInlineUniformBlockBindings =
            static_cast<std::uint32_t>(req.max_inline_uniform_block_bindings);
        ci.pNext = &iub_ci;
    }
    VkDescriptorPool vk = VK_NULL_HANDLE;
    const VkResult pr = vkCreateDescriptorPool(dev->second.vk, &ci, nullptr, &vk);
    if (pr != VK_SUCCESS || vk == VK_NULL_HANDLE) {
        resp.reason = "vkCreateDescriptorPool failed (VkResult " + std::to_string(pr) + ")";
        return resp;
    }
    const std::uint64_t h = next_handle_++;
    RealDescriptorPool pool;
    pool.vk = vk;
    pool.device = req.device;
    pool.sets_remaining = req.max_sets;
    pool.update_after_bind = pool_uab;
    descriptor_pools_.emplace(h, std::move(pool));
    dev->second.descriptor_pools.insert(h);
    resp.ok = true;
    resp.reason = "ok";
    resp.pool = h;
    return resp;
}

vkrpc::StatusResponse RealVulkanBackend::destroy_descriptor_pool(const vkrpc::HandleRequest& req) {
    vkrpc::StatusResponse resp;
    const auto it = descriptor_pools_.find(req.handle);
    if (it == descriptor_pools_.end()) {
        resp.ok = false;
        resp.reason = "unknown descriptor pool handle";
        return resp;
    }
    // Destroying a pool frees all its sets (vkDestroyDescriptorPool). Each freed set invalidates
    // any recorded CB that baked it.
    for (const std::uint64_t set_handle : it->second.sets) {
        invalidate_cbs_referencing(set_handle);
        descriptor_sets_.erase(set_handle);
    }
    const auto dev = devices_.find(it->second.device);
    if (dev != devices_.end() && dev->second.vk != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(dev->second.vk, it->second.vk, nullptr);
        dev->second.descriptor_pools.erase(req.handle);
    }
    descriptor_pools_.erase(it);
    resp.ok = true;
    resp.reason = "ok";
    return resp;
}

vkrpc::AllocateDescriptorSetsResponse
RealVulkanBackend::allocate_descriptor_sets(const vkrpc::AllocateDescriptorSetsRequest& req) {
    vkrpc::AllocateDescriptorSetsResponse resp;
    const auto dev = devices_.find(req.device);
    if (dev == devices_.end() || dev->second.vk == VK_NULL_HANDLE) {
        resp.reason = "unknown device handle";
        return resp;
    }
    const auto pool = descriptor_pools_.find(req.pool);
    if (pool == descriptor_pools_.end() || pool->second.device != req.device) {
        resp.reason = "descriptor pool is unknown or on a different device";
        return resp;
    }
    if (req.set_layouts.empty() ||
        req.set_layouts.size() > static_cast<std::size_t>(vkrpc::kMaxAllocateDescriptorSets)) {
        resp.reason = "allocate descriptor sets count out of bounds";
        return resp;
    }
    // (GL/zink): each layout must be known + same-device, and the set count must fit
    // maxSets; the real pool is the authoritative per-type descriptor budget
    // (vkAllocateDescriptorSets returns OUT_OF_POOL_MEMORY on exhaustion), so the worker no longer
    // mirrors per-type budgets.
    // descriptorIndexing: a non-empty variable_counts must PARALLEL the set
    // list; per set the count is IGNORED for layouts without a variable last binding and must be
    // <= the layout's declared max for layouts with one. A UAB-pool layout's sets must come from
    // a UAB pool.
    if (!req.variable_counts.empty() && req.variable_counts.size() != req.set_layouts.size()) {
        resp.reason = "variable_counts must parallel set_layouts (or be absent)";
        return resp;
    }
    std::vector<VkDescriptorSetLayout> vk_layouts;
    for (std::size_t i = 0; i < req.set_layouts.size(); ++i) {
        const auto it = descriptor_set_layouts_.find(req.set_layouts[i]);
        if (it == descriptor_set_layouts_.end() || it->second.device != req.device) {
            resp.reason = "allocate references a set layout not on the device";
            return resp;
        }
        if ((it->second.layout_flags & vkrpc::kDescriptorSetLayoutCreateUpdateAfterBindPool) != 0 &&
            !pool->second.update_after_bind) {
            resp.reason = "UPDATE_AFTER_BIND_POOL layout requires an UPDATE_AFTER_BIND pool";
            return resp;
        }
        if (!req.variable_counts.empty()) {
            for (const RealDescriptorSetLayoutBinding& b : it->second.bindings) {
                if ((b.binding_flags & vkrpc::kDescriptorBindingVariableDescriptorCount) != 0 &&
                    req.variable_counts[i] > static_cast<std::uint64_t>(b.descriptor_count)) {
                    resp.reason = "variable descriptor count exceeds the layout's declared max";
                    return resp;
                }
            }
        }
        vk_layouts.push_back(it->second.vk);
    }
    if (static_cast<long long>(req.set_layouts.size()) > pool->second.sets_remaining) {
        resp.reason = "descriptor pool has no room for this many sets (maxSets exceeded)";
        return resp;
    }
    VkDescriptorSetAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = pool->second.vk;
    ai.descriptorSetCount = static_cast<std::uint32_t>(vk_layouts.size());
    ai.pSetLayouts = vk_layouts.data();
    // Rebuild the variable-count pNext for the host allocate (serialized, not dropped).
    std::vector<std::uint32_t> vc_counts;
    VkDescriptorSetVariableDescriptorCountAllocateInfo vc_ai{};
    if (!req.variable_counts.empty()) {
        for (const std::uint64_t c : req.variable_counts) {
            vc_counts.push_back(static_cast<std::uint32_t>(c));
        }
        vc_ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO;
        vc_ai.descriptorSetCount = static_cast<std::uint32_t>(vc_counts.size());
        vc_ai.pDescriptorCounts = vc_counts.data();
        ai.pNext = &vc_ai;
    }
    std::vector<VkDescriptorSet> vk_sets(vk_layouts.size(), VK_NULL_HANDLE);
    if (vkAllocateDescriptorSets(dev->second.vk, &ai, vk_sets.data()) != VK_SUCCESS) {
        resp.reason = "vkAllocateDescriptorSets failed";
        return resp;
    }
    for (std::size_t i = 0; i < req.set_layouts.size(); ++i) {
        const auto& layout = descriptor_set_layouts_.at(req.set_layouts[i]);
        const std::uint64_t h = next_handle_++;
        RealDescriptorSet set;
        set.vk = vk_sets[i];
        set.device = req.device;
        set.pool = req.pool;
        set.layout = req.set_layouts[i];
        for (const RealDescriptorSetLayoutBinding& b : layout.bindings) {
            std::size_t count = static_cast<std::size_t>(b.descriptor_count);
            if ((b.binding_flags & vkrpc::kDescriptorBindingVariableDescriptorCount) != 0) {
                count = req.variable_counts.empty()
                            ? 0
                            : static_cast<std::size_t>(req.variable_counts[i]);
                set.variable_count = static_cast<long long>(count);
            }
            set.slots[b.binding] = std::vector<RealDescriptorSlot>(count);
        }
        descriptor_sets_.emplace(h, std::move(set));
        pool->second.sets.insert(h);
        resp.descriptor_sets.push_back(h);
    }
    pool->second.sets_remaining -= static_cast<int>(req.set_layouts.size());
    resp.ok = true;
    resp.reason = "ok";
    return resp;
}

vkrpc::StatusResponse
RealVulkanBackend::update_descriptor_sets(const vkrpc::UpdateDescriptorSetsRequest& req) {
    vkrpc::StatusResponse resp;
    const auto dev = devices_.find(req.device);
    if (dev == devices_.end() || dev->second.vk == VK_NULL_HANDLE) {
        resp.ok = false;
        resp.reason = "unknown device handle";
        return resp;
    }
    // A rejected update POISONS every targeted set on req.device (marks slots uninitialized +
    // invalidates recorded CBs that bound it), so a previously-valid set cannot stay draw-ready
    // behind a rejected update (device-scoped). Mirrors the mock.
    std::set<std::uint64_t> targets;
    for (const vkrpc::WriteDescriptorSetDesc& w : req.writes) {
        const auto ds = descriptor_sets_.find(w.dst_set);
        if (ds != descriptor_sets_.end() && ds->second.device == req.device) {
            targets.insert(w.dst_set);
        }
    }
    auto poison_and_fail = [&](const char* why) {
        for (const std::uint64_t s : targets) {
            const auto ds = descriptor_sets_.find(s);
            if (ds == descriptor_sets_.end()) {
                continue;
            }
            for (auto& [binding, slots] : ds->second.slots) {
                (void) binding;
                for (RealDescriptorSlot& slot : slots) {
                    slot.initialized = false;
                    slot.buffer = 0;
                    slot.sampler = 0;
                    slot.image_view = 0;
                    slot.buffer_view = 0;
                }
            }
            invalidate_cbs_referencing(s);
        }
        vkrpc::StatusResponse r;
        r.ok = false;
        r.reason = why;
        return r;
    };
    if (req.writes.size() > static_cast<std::size_t>(vkrpc::kMaxDescriptorWrites)) {
        return poison_and_fail("too many descriptor writes in one update batch");
    }
    // (GL/zink): FAITHFUL all-type update -- build the host VkWriteDescriptorSet array and
    // let the host driver (+ optional validation layer) gate type-specific correctness. The inner
    // side-buffers are reserved so push_back never reallocates (each data() must stay a valid
    // pBufferInfo / pImageInfo / pTexelBufferView until vkUpdateDescriptorSets returns). The slot
    // model is kept generalized (referent per type) so the destroy-consult + draw-readiness still
    // work for every type.
    std::vector<VkWriteDescriptorSet> vk_writes;
    std::vector<std::vector<VkDescriptorBufferInfo>> info_store;
    std::vector<std::vector<VkDescriptorImageInfo>> image_info_store;
    std::vector<std::vector<VkBufferView>> texel_store;
    info_store.reserve(req.writes.size());
    image_info_store.reserve(req.writes.size());
    texel_store.reserve(req.writes.size());
    // Vulkan 1.3 support (inlineUniformBlock): per-write pNext storage, pre-sized to writes.size()
    // so the pointer a vk_write takes (indexed by its position) stays stable until
    // vkUpdateDescriptorSets returns; pData aliases the request's inline_data, which also outlives
    // the call.
    std::vector<VkWriteDescriptorSetInlineUniformBlock> inline_store(req.writes.size());
    for (const vkrpc::WriteDescriptorSetDesc& w : req.writes) {
        const auto ds = descriptor_sets_.find(w.dst_set);
        if (ds == descriptor_sets_.end() || ds->second.device != req.device) {
            return poison_and_fail("descriptor write targets a set not on the device");
        }
        const auto slot_it = ds->second.slots.find(w.dst_binding);
        if (slot_it == ds->second.slots.end()) {
            return poison_and_fail("descriptor write targets a binding not in the set's layout");
        }
        if (w.descriptor_count < 1 || w.descriptor_count > vkrpc::kMaxDescriptorCount) {
            return poison_and_fail("descriptor write descriptor count is invalid");
        }
        if (w.dst_array_element < 0 ||
            static_cast<long long>(w.dst_array_element) + w.descriptor_count >
                static_cast<long long>(slot_it->second.size())) {
            return poison_and_fail(
                "descriptor write array range exceeds the binding's descriptor count");
        }
        // Vulkan 1.3 support (inlineUniformBlock): an IUB binding's slots ARE its bytes, so the
        // write and binding must agree on the type in BOTH directions -- a mismatched pair would
        // mis-model byte slots as descriptor slots (or vice versa) and forward an invalid host
        // write. (The layout outlives its sets: destroy is blocked while sets are allocated.)
        {
            int binding_type = -1;
            const auto sl = descriptor_set_layouts_.find(ds->second.layout);
            if (sl != descriptor_set_layouts_.end()) {
                for (const RealDescriptorSetLayoutBinding& b : sl->second.bindings) {
                    if (b.binding == w.dst_binding) {
                        binding_type = b.descriptor_type;
                    }
                }
            }
            if ((w.descriptor_type == vkrpc::kDescriptorTypeInlineUniformBlock) !=
                (binding_type == vkrpc::kDescriptorTypeInlineUniformBlock)) {
                return poison_and_fail(
                    "descriptor write and binding disagree on the INLINE_UNIFORM_BLOCK type");
            }
        }
        const VkDescriptorType vk_type = static_cast<VkDescriptorType>(w.descriptor_type);
        const std::size_t n = static_cast<std::size_t>(w.descriptor_count);
        VkWriteDescriptorSet vw{};
        vw.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        vw.dstSet = ds->second.vk;
        vw.dstBinding = static_cast<std::uint32_t>(w.dst_binding);
        vw.dstArrayElement = static_cast<std::uint32_t>(w.dst_array_element);
        vw.descriptorCount = static_cast<std::uint32_t>(w.descriptor_count);
        vw.descriptorType = vk_type;
        switch (vk_type) {
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC: {
            if (w.buffer_infos.size() != n) {
                return poison_and_fail(
                    "descriptor write count disagrees with the buffer-info list");
            }
            std::vector<VkDescriptorBufferInfo> infos;
            infos.reserve(n);
            for (const vkrpc::DescriptorBufferInfoDesc& bi : w.buffer_infos) {
                // (GL/zink): a NULL-handle buffer is a VK_EXT_robustness2 null descriptor.
                // zink enables nullDescriptor (the feature is forwarded faithfully + thus enabled
                // on this device) and writes null for unused bindings -- forward it verbatim rather
                // than rejecting a perfectly valid robustness2 write.
                if (bi.buffer == 0) {
                    VkDescriptorBufferInfo info{};
                    info.buffer = VK_NULL_HANDLE;
                    info.offset = 0;
                    info.range = (bi.range == vkrpc::kVkWholeSize)
                                     ? VK_WHOLE_SIZE
                                     : static_cast<VkDeviceSize>(bi.range);
                    infos.push_back(info);
                    continue;
                }
                const auto bf = buffers_.find(bi.buffer);
                if (bf == buffers_.end() || bf->second.device != req.device) {
                    return poison_and_fail(
                        "descriptor write references a buffer not on the device");
                }
                if (bf->second.bound_memory == 0) {
                    return poison_and_fail("descriptor write buffer is not bound to memory");
                }
                // A non-null buffer descriptor's range must be > 0 or VK_WHOLE_SIZE
                // (VUID-VkDescriptorBufferInfo-range-00341); reject + poison a zero range rather
                // than forward an invalid write.
                if (bi.range == 0) {
                    return poison_and_fail("descriptor write buffer range is zero");
                }
                if (bi.offset >= bf->second.size) {
                    return poison_and_fail(
                        "descriptor write buffer offset is outside the logical buffer");
                }
                if (bi.range != vkrpc::kVkWholeSize &&
                    bi.range > bf->second.size - static_cast<VkDeviceSize>(bi.offset)) {
                    return poison_and_fail(
                        "descriptor write buffer range is outside the logical buffer");
                }
                VkDescriptorBufferInfo info{};
                info.buffer = bf->second.vk;
                info.offset = static_cast<VkDeviceSize>(bi.offset);
                info.range = (bi.range == vkrpc::kVkWholeSize)
                                 ? bf->second.size - static_cast<VkDeviceSize>(bi.offset)
                                 : static_cast<VkDeviceSize>(bi.range);
                infos.push_back(info);
            }
            info_store.push_back(std::move(infos));
            vw.pBufferInfo = info_store.back().data();
            break;
        }
        case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
        case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER: {
            if (w.texel_buffer_views.size() != n) {
                return poison_and_fail("descriptor write count disagrees with the texel-view list");
            }
            std::vector<VkBufferView> views;
            views.reserve(n);
            for (const std::uint64_t bvh : w.texel_buffer_views) {
                const auto bv = buffer_views_.find(bvh);
                if (bv == buffer_views_.end() || bv->second.device != req.device) {
                    return poison_and_fail(
                        "descriptor write references a buffer view not on the device");
                }
                views.push_back(bv->second.vk);
            }
            texel_store.push_back(std::move(views));
            vw.pTexelBufferView = texel_store.back().data();
            break;
        }
        case VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK: {
            // Vulkan 1.3 support (inlineUniformBlock): descriptorCount is the BYTE count and
            // dstArrayElement the BYTE offset (each a multiple of 4, the spec VUs); the slot
            // vector was sized to the binding's byte size, so the generic range check above
            // already bounds the write. The payload is the raw inline_data bytes -- an IUB write
            // references no objects, so the structured info lists must stay empty (an ICD that
            // saw a malformed pNext forwards EMPTY inline_data and the batch rejects here). The
            // host write chains VkWriteDescriptorSetInlineUniformBlock (dataSize == the count).
            if ((w.dst_array_element % 4) != 0) {
                return poison_and_fail("inline uniform block write dstArrayElement must be a "
                                       "multiple of 4 (a byte offset)");
            }
            if ((w.descriptor_count % 4) != 0) {
                return poison_and_fail("inline uniform block write descriptorCount must be a "
                                       "multiple of 4 (a byte count)");
            }
            if (w.inline_data.size() != n) {
                return poison_and_fail(
                    "inline uniform block write must carry exactly descriptorCount bytes");
            }
            if (!w.buffer_infos.empty() || !w.image_infos.empty() ||
                !w.texel_buffer_views.empty()) {
                return poison_and_fail(
                    "inline uniform block write must not carry buffer/image/texel infos");
            }
            VkWriteDescriptorSetInlineUniformBlock& ib = inline_store[vk_writes.size()];
            ib.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_INLINE_UNIFORM_BLOCK;
            ib.dataSize = static_cast<std::uint32_t>(w.descriptor_count);
            ib.pData = w.inline_data.data();
            vw.pNext = &ib;
            break;
        }
        default: { // image / sampler types: SAMPLER, SAMPLED_IMAGE, STORAGE_IMAGE,
                   // COMBINED_IMAGE_SAMPLER, INPUT_ATTACHMENT
            if (w.image_infos.size() != n) {
                return poison_and_fail("descriptor write count disagrees with the image-info list");
            }
            std::vector<VkDescriptorImageInfo> infos;
            infos.reserve(n);
            for (const vkrpc::DescriptorImageInfoDesc& ii : w.image_infos) {
                VkDescriptorImageInfo info{};
                if (ii.sampler != 0) {
                    const auto sm = samplers_.find(ii.sampler);
                    if (sm == samplers_.end() || sm->second.device != req.device) {
                        return poison_and_fail(
                            "descriptor write references a sampler not on the device");
                    }
                    info.sampler = sm->second.vk;
                }
                if (ii.image_view != 0) {
                    const auto iv = image_views_.find(ii.image_view);
                    if (iv == image_views_.end() || iv->second.device != req.device) {
                        return poison_and_fail(
                            "descriptor write references an image view not on the device");
                    }
                    info.imageView = iv->second.vk;
                }
                info.imageLayout =
                    static_cast<VkImageLayout>(ii.image_layout < 0 ? 0 : ii.image_layout);
                infos.push_back(info);
            }
            image_info_store.push_back(std::move(infos));
            vw.pImageInfo = image_info_store.back().data();
            break;
        }
        }
        vk_writes.push_back(vw);
    }
    if (!vk_writes.empty()) {
        vkUpdateDescriptorSets(dev->second.vk, static_cast<std::uint32_t>(vk_writes.size()),
                               vk_writes.data(), 0, nullptr);
    }
    // Apply the (generalized) slot model: mark each written slot initialized + record its referent.
    // Vulkan 1.3 support (inlineUniformBlock): an IUB write's byte-slots are marked by this same
    // loop (descriptor_count IS the byte count and every info list is empty, so the referents stay
    // 0
    // -- an IUB write references no objects, so no destroy-consult/dangle interaction).
    for (const vkrpc::WriteDescriptorSetDesc& w : req.writes) {
        RealDescriptorSet& set = descriptor_sets_.at(w.dst_set);
        std::vector<RealDescriptorSlot>& slots = set.slots.at(w.dst_binding);
        for (int i = 0; i < w.descriptor_count; ++i) {
            RealDescriptorSlot& slot = slots[static_cast<std::size_t>(w.dst_array_element + i)];
            const auto idx = static_cast<std::size_t>(i);
            slot.initialized = true;
            slot.buffer = 0;
            slot.sampler = 0;
            slot.image_view = 0;
            slot.buffer_view = 0;
            if (idx < w.buffer_infos.size()) {
                slot.buffer = w.buffer_infos[idx].buffer;
            } else if (idx < w.image_infos.size()) {
                slot.sampler = w.image_infos[idx].sampler;
                slot.image_view = w.image_infos[idx].image_view;
            } else if (idx < w.texel_buffer_views.size()) {
                slot.buffer_view = w.texel_buffer_views[idx];
            }
        }
    }
    resp.ok = true;
    resp.reason = "ok";
    return resp;
}

bool RealVulkanBackend::descriptor_set_draw_ready(std::uint64_t set, std::uint64_t device,
                                                  std::string& reason) const {
    const auto ds = descriptor_sets_.find(set);
    if (ds == descriptor_sets_.end() || ds->second.device != device) {
        reason = "draw bound a descriptor set not on the device";
        return false;
    }
    const auto sl = descriptor_set_layouts_.find(ds->second.layout);
    if (sl == descriptor_set_layouts_.end()) {
        reason = "draw bound a descriptor set whose layout no longer exists";
        return false;
    }
    // (GL/zink): a draw-bound set must have every layout-declared slot WRITTEN (the
    // destroy-consult marks a slot uninitialized when its referent is freed, so this also catches a
    // dangling reference). Type-specific resource liveness (was: CIS -> sampler+SAMPLED-image,
    // else -> UNIFORM buffer) is dropped -- it does not generalize across all descriptor types, and
    // the host driver (+ the destroy-consult) is the authority.
    //
    // descriptorIndexing (the readiness rework): a
    // PARTIALLY_BOUND binding may hold unwritten/dangling slots (dynamic USE is invisible to the
    // relay; the host/validation layer owns it), and an UPDATE_AFTER_BIND binding is exempt at
    // RECORD time (record-now-update-before-submit is the feature; replay binds live handles so
    // the late update lands). A variable-count binding's vector was sized to the ALLOCATED count,
    // so this loop inherently judges that count. CLASSIC bindings keep the fail-closed check
    // (mock == real).
    //
    // Vulkan 1.3 support (inlineUniformBlock): an IUB binding's slots ARE its bytes (one per byte,
    // sized at allocate), so the same rule composes unchanged -- every byte a classic IUB binding
    // declares must have been covered by a successful write.
    for (const RealDescriptorSetLayoutBinding& b : sl->second.bindings) {
        const auto slot_it = ds->second.slots.find(b.binding);
        if (slot_it == ds->second.slots.end()) {
            reason = "draw bound a descriptor set missing a layout-required binding";
            return false;
        }
        if ((b.binding_flags & (vkrpc::kDescriptorBindingPartiallyBound |
                                vkrpc::kDescriptorBindingUpdateAfterBind)) != 0) {
            continue; // host-owned completeness (PARTIALLY_BOUND / UPDATE_AFTER_BIND)
        }
        for (const RealDescriptorSlot& slot : slot_it->second) {
            if (!slot.initialized) {
                reason = "draw bound a descriptor set with an uninitialized (or dangling) slot";
                return false;
            }
        }
    }
    return true;
}

vkrpc::CreateGraphicsPipelinesResponse
RealVulkanBackend::create_graphics_pipelines(const vkrpc::CreateGraphicsPipelinesRequest& req) {
    vkrpc::CreateGraphicsPipelinesResponse resp;
    const auto dev = devices_.find(req.device);
    if (dev == devices_.end() || dev->second.vk == VK_NULL_HANDLE) {
        resp.ok = false;
        resp.reason = "unknown device handle";
        return resp;
    }
    // (GL/zink): FAITHFUL graphics pipeline -- build the full create-info from the carried
    // state (any stages / vertex input / topology / rasterization / multisample / colour-blend /
    // dynamic state); the host driver gates it. pipelineCache is ignored (no-op). The depth-stencil
    // cross-check stays a hardened guard: a depth render pass needs explicit depth-stencil state.
    std::vector<VkPipelineShaderStageCreateInfo> stages;
    // Vulkan 1.3 support (subgroupSizeControl): per-stage required-size pNext storage must outlive
    // the create call. Gated on the device's enabled vk13 bits -- a stage shape the device did not
    // enable rejects BEFORE the host sees it (mock == real, the ICD's predicate mirrored).
    std::vector<VkPipelineShaderStageRequiredSubgroupSizeCreateInfo> stage_subgroup(
        req.stages.size());
    std::size_t stage_idx = 0;
    for (const vkrpc::ShaderStageDesc& s : req.stages) {
        const auto sm = shader_modules_.find(s.module);
        if (sm == shader_modules_.end() || sm->second.device != req.device) {
            resp.ok = false;
            resp.reason = "pipeline stage references a shader module not on the device";
            return resp;
        }
        VkPipelineShaderStageCreateInfo st{};
        st.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        st.stage = static_cast<VkShaderStageFlagBits>(s.stage);
        st.module = sm->second.vk;
        st.pName = s.entry.empty() ? "main" : s.entry.c_str();
        if (s.stage_flags != 0 || s.required_subgroup_size != 0) {
            if ((dev->second.vk13_feature_bits & vkrpc::kVk13FeatureSubgroupSizeControl) == 0) {
                resp.ok = false;
                resp.reason = "pipeline stage subgroup-size shape requires the enabled "
                              "subgroupSizeControl feature";
                return resp;
            }
            const long long allowed =
                VK_PIPELINE_SHADER_STAGE_CREATE_ALLOW_VARYING_SUBGROUP_SIZE_BIT;
            if ((s.stage_flags & ~allowed) != 0) {
                resp.ok = false;
                resp.reason = "pipeline stage flags outside the served set";
                return resp;
            }
            st.flags = static_cast<VkPipelineShaderStageCreateFlags>(s.stage_flags);
            if (s.required_subgroup_size != 0) {
                auto& rss = stage_subgroup[stage_idx];
                rss.sType =
                    VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO;
                rss.requiredSubgroupSize = static_cast<std::uint32_t>(s.required_subgroup_size);
                st.pNext = &rss;
            }
        }
        stages.push_back(st);
        ++stage_idx;
    }
    if (stages.empty()) {
        resp.ok = false;
        resp.reason = "graphics pipeline has no shader stages";
        return resp;
    }
    // Vertex input: build the carried bindings/attributes verbatim (any binding/rate/format).
    std::vector<VkVertexInputBindingDescription> vk_bindings;
    std::vector<VkVertexInputAttributeDescription> vk_attrs;
    for (const vkrpc::VertexBindingDesc& b : req.vertex_bindings) {
        // Fail closed on an oversized stride rather than silently narrowing it to uint32: a
        // stride > UINT32_MAX would wrap to a degenerate value.
        if (b.stride > static_cast<long long>(UINT32_MAX)) {
            resp.ok = false;
            resp.reason = "vertex binding stride exceeds UINT32_MAX";
            return resp;
        }
        VkVertexInputBindingDescription d{};
        d.binding = static_cast<std::uint32_t>(b.binding);
        d.stride = static_cast<std::uint32_t>(b.stride < 0 ? 0 : b.stride);
        d.inputRate = static_cast<VkVertexInputRate>(b.input_rate < 0 ? 0 : b.input_rate);
        vk_bindings.push_back(d);
    }
    for (const vkrpc::VertexAttributeDesc& a : req.vertex_attributes) {
        if (a.offset > static_cast<long long>(UINT32_MAX)) {
            resp.ok = false;
            resp.reason = "vertex attribute offset exceeds UINT32_MAX";
            return resp;
        }
        VkVertexInputAttributeDescription d{};
        d.location = static_cast<std::uint32_t>(a.location < 0 ? 0 : a.location);
        d.binding = static_cast<std::uint32_t>(a.binding < 0 ? 0 : a.binding);
        d.format = static_cast<VkFormat>(a.format);
        d.offset = static_cast<std::uint32_t>(a.offset < 0 ? 0 : a.offset);
        vk_attrs.push_back(d);
    }
    // geometry-stream: re-assert the shared value gate independently of the ICD's structural
    // admission (the hostile-RPC safety boundary, mock == real): feature enabled, stream in range
    // against the CACHED host properties, nonzero stream only with StreamSelect. Named reject
    // before the host driver ever sees an invalid stream selection.
    {
        std::string stream_why;
        if (!vkrpc::rasterization_stream_ok(
                req.stream_state_present, req.rasterization_stream,
                dev->second.enabled_extensions.count(VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME) != 0,
                dev->second.geometry_streams_feature_enabled,
                dev->second.max_transform_feedback_streams,
                dev->second.transform_feedback_rasterization_stream_select, stream_why)) {
            resp.ok = false;
            resp.reason = stream_why;
            return resp;
        }
    }
    // vertex-attr-divisor: re-assert the same shared admission the ICD did (independent
    // fail-closed, mock == real), then build the VkVertexInputBindingDivisorDescriptionEXT array.
    // The vector + the divisor-state struct (below, chained on vi.pNext) must outlive
    // vkCreateGraphicsPipelines.
    {
        std::string div_why;
        if (!vkrpc::vertex_binding_divisors_ok(
                req.vertex_divisor_present, req.vertex_bindings, req.vertex_binding_divisors,
                dev->second.enabled_extensions.count(vkrpc::kVertexAttributeDivisorExtensionName) !=
                    0,
                dev->second.vertex_attr_divisor_feature_enabled,
                dev->second.vertex_attr_zero_divisor_feature_enabled, div_why)) {
            resp.ok = false;
            resp.reason = div_why;
            return resp;
        }
    }
    std::vector<VkVertexInputBindingDivisorDescriptionEXT> vk_divisors;
    for (const vkrpc::VertexBindingDivisorDesc& d : req.vertex_binding_divisors) {
        VkVertexInputBindingDivisorDescriptionEXT vd{};
        vd.binding = static_cast<std::uint32_t>(d.binding);
        vd.divisor = static_cast<std::uint32_t>(d.divisor);
        vk_divisors.push_back(vd);
    }
    VkPipelineVertexInputDivisorStateCreateInfoEXT vi_divisor{};
    vi_divisor.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_EXT;
    vi_divisor.vertexBindingDivisorCount = static_cast<std::uint32_t>(vk_divisors.size());
    vi_divisor.pVertexBindingDivisors = vk_divisors.data();
    const auto layout = pipeline_layouts_.find(req.layout);
    if (layout == pipeline_layouts_.end() || layout->second.device != req.device) {
        resp.ok = false;
        resp.reason = "pipeline layout is unknown or on a different device";
        return resp;
    }
    // (native lane): a dynamic-rendering pipeline (render_pass
    // == 0 + VkPipelineRenderingCreateInfo formats) keys its compatibility on the FORMATS, not a
    // render pass. Compute the shared locals (the host render pass, the color-attachment count for
    // the blend check, and -- for DR -- the VkFormat vector for the rebuilt pNext) up front so the
    // create-info build below is a single path. Fail-closed: render_pass != 0 WITH DR info, or a
    // viewMask, or a depth-stencil-presence mismatch, is rejected (mock == real). render_pass == 0
    // WITHOUT DR info never reaches here (a non-DR pipeline needs a render pass), and would be a
    // blind NULL-renderpass pipeline -- the NVIDIA segfault hazard -- rejected in the else.
    const bool is_dr = req.has_dynamic_rendering != 0;
    VkRenderPass rp_vk = VK_NULL_HANDLE;
    std::size_t color_ref_count = 0;
    std::vector<VkFormat> dr_color_vk;
    if (is_dr) {
        // The device must have enabled VK_KHR_dynamic_rendering AND the dynamicRendering feature
        // (mock == real). The ICD only admits the
        // VkPipelineRenderingCreateInfo pNext on such a device -- backend-side safety net.
        // Vulkan 1.3 support: on an honest vk13 device DR is CORE -- the feature alone admits it.
        if ((dev->second.enabled_extensions.count(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME) == 0 &&
             !dev->second.vk13_device) ||
            !dev->second.dynamic_rendering_feature_enabled) {
            resp.ok = false;
            resp.reason = "dynamic-rendering pipeline requires VK_KHR_dynamic_rendering AND the "
                          "dynamicRendering feature enabled on the device";
            return resp;
        }
        if (req.render_pass != 0) {
            resp.ok = false;
            resp.reason = "dynamic-rendering pipeline must not also target a render pass";
            return resp;
        }
        // Required-feature audit: the DR pipeline's viewMask rides
        // VkPipelineRenderingCreateInfo.viewMask below, gated on the multiview feature -- the same
        // fail-closed rule as begin_rendering + the RP2 path. It must equal the active DR scope's
        // viewMask at draw (the compat key already carries it).
        if (req.dr_view_mask != 0) {
            if (!dev->second.multiview_feature_enabled) {
                resp.ok = false;
                resp.reason = "dynamic-rendering pipeline viewMask (multiview) requires the "
                              "multiview feature enabled";
                return resp;
            }
            if (req.dr_view_mask < 0) {
                resp.ok = false;
                resp.reason = "dynamic-rendering pipeline viewMask is negative (malformed)";
                return resp;
            }
        }
        if (req.dr_color_formats.size() >
            static_cast<std::size_t>(vkrpc::kMaxDynamicRenderingColorAttachments)) {
            resp.ok = false;
            resp.reason = "dynamic-rendering pipeline color attachment count out of range";
            return resp;
        }
        const bool dr_has_depth_stencil = req.dr_depth_format != 0 || req.dr_stencil_format != 0;
        // Spec semantics, exactly: a DR pipeline that DECLARES a depth/stencil format MUST carry
        // the state; a pipeline that declares NO such format may still carry the struct -- the
        // spec says it is IGNORED then (Mesa >= 25 zink attaches a disabled depth-stencil struct
        // to color-only DR pipelines, which the old strict "iff" rejected). Ignoring means NOT
        // forwarding it to the driver (below), never approximating it.
        if (dr_has_depth_stencil && !req.has_depth_stencil) {
            resp.ok = false;
            resp.reason = "dynamic-rendering pipeline declares a depth/stencil format but carries "
                          "no depth-stencil state";
            return resp;
        }
        for (const int f : req.dr_color_formats) {
            dr_color_vk.push_back(static_cast<VkFormat>(f));
        }
        color_ref_count = dr_color_vk.size();
    } else {
        const auto rp = render_passes_.find(req.render_pass);
        if (rp == render_passes_.end() || rp->second.device != req.device) {
            resp.ok = false;
            resp.reason = "pipeline render pass is unknown or on a different device";
            return resp;
        }
        const bool rp_has_depth = rp->second.depth_format != VK_FORMAT_UNDEFINED;
        if (rp_has_depth && !req.has_depth_stencil) {
            resp.ok = false;
            resp.reason =
                "graphics pipeline targeting a depth render pass needs depth-stencil state";
            return resp;
        }
        rp_vk = rp->second.vk;
        color_ref_count = !rp->second.color_refs.empty()
                              ? rp->second.color_refs.size()
                              : (rp->second.color_format != VK_FORMAT_UNDEFINED ? 1u : 0u);
    }
    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    // vertex-attr-divisor: chain the rebuilt VkPipelineVertexInputDivisorStateCreateInfoEXT when
    // the wire carried one (present + validated above). Absent -> pNext stays null
    // (byte-identical).
    if (req.vertex_divisor_present != 0) {
        vi.pNext = &vi_divisor;
    }
    vi.vertexBindingDescriptionCount = static_cast<std::uint32_t>(vk_bindings.size());
    vi.pVertexBindingDescriptions = vk_bindings.empty() ? nullptr : vk_bindings.data();
    vi.vertexAttributeDescriptionCount = static_cast<std::uint32_t>(vk_attrs.size());
    vi.pVertexAttributeDescriptions = vk_attrs.empty() ? nullptr : vk_attrs.data();
    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = static_cast<VkPrimitiveTopology>(req.topology < 0 ? 0 : req.topology);
    ia.primitiveRestartEnable = req.primitive_restart_enable != 0 ? VK_TRUE : VK_FALSE;
    VkPipelineViewportStateCreateInfo vp{};
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = static_cast<std::uint32_t>(req.viewport_count > 0 ? req.viewport_count : 1);
    vp.scissorCount = static_cast<std::uint32_t>(req.scissor_count > 0 ? req.scissor_count : 1);
    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.depthClampEnable = req.depth_clamp_enable != 0 ? VK_TRUE : VK_FALSE;
    rs.rasterizerDiscardEnable = req.rasterizer_discard_enable != 0 ? VK_TRUE : VK_FALSE;
    rs.polygonMode = static_cast<VkPolygonMode>(req.polygon_mode < 0 ? 0 : req.polygon_mode);
    rs.cullMode = static_cast<VkCullModeFlags>(req.cull_mode < 0 ? 0 : req.cull_mode);
    rs.frontFace = static_cast<VkFrontFace>(req.front_face < 0 ? 0 : req.front_face);
    rs.depthBiasEnable = req.depth_bias_enable != 0 ? VK_TRUE : VK_FALSE;
    rs.depthBiasConstantFactor = static_cast<float>(req.depth_bias_constant);
    rs.depthBiasClamp = static_cast<float>(req.depth_bias_clamp);
    rs.depthBiasSlopeFactor = static_cast<float>(req.depth_bias_slope);
    rs.lineWidth = req.line_width > 0.0 ? static_cast<float>(req.line_width) : 1.0f;
    // (GL/zink) RENDER CORRECTNESS: rebuild the VkPipelineRasterizationDepthClipStateCreate
    // InfoEXT (VK_EXT_depth_clip_enable) the app chained on the rasterization pNext, so the real
    // pipeline clips against the near/far planes exactly as zink intends. Absent (present == 0) ->
    // no pNext, i.e. Vulkan's default clipping (vkcube/canary unchanged). The struct must outlive
    // vkCreateGraphicsPipelines below, so it lives in this function scope.
    // (GL/zink) RENDER CORRECTNESS: rebuild the rasterization pNext zink chained -- the
    // depth-clip state (VK_EXT_depth_clip_enable, near/far clipping a la GL_DEPTH_CLAMP) and/or the
    // line state (VK_EXT_line_rasterization, the GL line-rasterization mode). zink may chain BOTH,
    // so build the chain depth_clip -> line_state -> null. These structs must outlive
    // vkCreateGraphicsPipelines below, so they live in this function scope. Neither present -> no
    // pNext (vkcube/canary unchanged, Vulkan default clip + line rasterization).
    VkPipelineRasterizationDepthClipStateCreateInfoEXT depth_clip{};
    VkPipelineRasterizationLineStateCreateInfoEXT line_state{};
    VkPipelineRasterizationStateStreamCreateInfoEXT stream_state{};
    const void* rs_chain = nullptr;
    const void** rs_tail = &rs_chain;
    if (req.depth_clip_state_present != 0) {
        depth_clip.sType =
            VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_DEPTH_CLIP_STATE_CREATE_INFO_EXT;
        depth_clip.depthClipEnable = req.depth_clip_enable != 0 ? VK_TRUE : VK_FALSE;
        *rs_tail = &depth_clip;
        rs_tail = &depth_clip.pNext;
    }
    if (req.line_state_present != 0) {
        // The stippled feature is masked off upstream, so stippledLineEnable arrives FALSE; carry
        // the fields verbatim anyway.
        line_state.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_LINE_STATE_CREATE_INFO_EXT;
        line_state.lineRasterizationMode =
            static_cast<VkLineRasterizationModeEXT>(req.line_rasterization_mode);
        line_state.stippledLineEnable = req.line_stipple_enable != 0 ? VK_TRUE : VK_FALSE;
        line_state.lineStippleFactor = static_cast<std::uint32_t>(req.line_stipple_factor);
        line_state.lineStipplePattern = static_cast<std::uint16_t>(req.line_stipple_pattern);
        *rs_tail = &line_state;
        rs_tail = &line_state.pNext;
    }
    if (req.stream_state_present != 0) {
        // geometry-stream: validated above (shared rasterization_stream_ok against the cached host
        // properties), so the narrowing cast is range-proven. flags stays 0 (reserved; admission
        // rejected a nonzero and the wire does not carry it).
        stream_state.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_STREAM_CREATE_INFO_EXT;
        stream_state.rasterizationStream = static_cast<std::uint32_t>(req.rasterization_stream);
        *rs_tail = &stream_state;
        rs_tail = &stream_state.pNext;
    }
    rs.pNext = rs_chain;
    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = static_cast<VkSampleCountFlagBits>(
        req.rasterization_samples > 0 ? req.rasterization_samples : 1);
    ms.sampleShadingEnable = req.sample_shading_enable != 0 ? VK_TRUE : VK_FALSE;
    ms.minSampleShading = static_cast<float>(req.min_sample_shading);
    ms.alphaToCoverageEnable = req.alpha_to_coverage_enable != 0 ? VK_TRUE : VK_FALSE;
    ms.alphaToOneEnable = req.alpha_to_one_enable != 0 ? VK_TRUE : VK_FALSE;
    VkSampleMask sample_mask_word = 0;
    if (req.sample_mask >= 0) {
        sample_mask_word = static_cast<VkSampleMask>(req.sample_mask);
        ms.pSampleMask = &sample_mask_word;
    }
    // Colour blend: the carried per-attachment state, or one no-blend RGBA attachment by default.
    std::vector<VkPipelineColorBlendAttachmentState> blend_atts;
    for (const vkrpc::ColorBlendAttachmentDesc& a : req.color_blend_attachments) {
        VkPipelineColorBlendAttachmentState s{};
        s.blendEnable = a.blend_enable != 0 ? VK_TRUE : VK_FALSE;
        s.srcColorBlendFactor = static_cast<VkBlendFactor>(a.src_color_factor);
        s.dstColorBlendFactor = static_cast<VkBlendFactor>(a.dst_color_factor);
        s.colorBlendOp = static_cast<VkBlendOp>(a.color_blend_op);
        s.srcAlphaBlendFactor = static_cast<VkBlendFactor>(a.src_alpha_factor);
        s.dstAlphaBlendFactor = static_cast<VkBlendFactor>(a.dst_alpha_factor);
        s.alphaBlendOp = static_cast<VkBlendOp>(a.alpha_blend_op);
        s.colorWriteMask = static_cast<VkColorComponentFlags>(a.color_write_mask);
        blend_atts.push_back(s);
    }
    // (GL/zink): the default no-blend attachment applies only when the target render pass HAS
    // a colour attachment -- a depth-only (shadow-map) pass takes attachmentCount 0, matching its
    // subpass. MRT: the subpass's TRUE color-attachment count -- the ref vector's
    // length INCLUDING UNUSED holes (VUID: pColorBlendState->attachmentCount must equal it), or the
    // legacy 0/1 derived from the scalar compat key. For a dynamic-rendering pipeline it is the
    // declared color-format count (computed in the fork above).
    if (!blend_atts.empty() && blend_atts.size() != color_ref_count) {
        // The exact driver-tolerated VUID violation that made the 2-color probe silently
        // half-render -- OUR named rejection now, not the driver's discard.
        resp.ok = false;
        resp.reason = "pipeline color blend attachment count does not match the render pass "
                      "subpass color attachment count";
        return resp;
    }
    if (blend_atts.empty()) {
        // N-aware default: one no-blend RGBA state per subpass color attachment --
        // zero for a depth-only pass. The old single-default would have been a NEW half-render for
        // an MRT app that omits blend state.
        for (std::size_t i = 0; i < color_ref_count; ++i) {
            VkPipelineColorBlendAttachmentState s{};
            s.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                               VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
            blend_atts.push_back(s);
        }
    }
    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.logicOpEnable = req.logic_op_enable != 0 ? VK_TRUE : VK_FALSE;
    cb.logicOp = static_cast<VkLogicOp>(req.logic_op);
    cb.attachmentCount = static_cast<std::uint32_t>(blend_atts.size());
    cb.pAttachments = blend_atts.data();
    for (int i = 0; i < 4; ++i) {
        cb.blendConstants[i] = static_cast<float>(req.blend_constants[i]);
    }
    std::vector<VkDynamicState> dyn;
    for (const int d : req.dynamic_states) {
        dyn.push_back(static_cast<VkDynamicState>(d));
    }
    VkPipelineDynamicStateCreateInfo ds{};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    ds.dynamicStateCount = static_cast<std::uint32_t>(dyn.size());
    ds.pDynamicStates = dyn.empty() ? nullptr : dyn.data();
    VkPipelineDepthStencilStateCreateInfo dss{};
    dss.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    if (req.has_depth_stencil) {
        dss.depthTestEnable = req.depth_test_enable > 0 ? VK_TRUE : VK_FALSE;
        dss.depthWriteEnable = req.depth_write_enable > 0 ? VK_TRUE : VK_FALSE;
        dss.depthCompareOp =
            static_cast<VkCompareOp>(req.depth_compare_op < 0 ? 0 : req.depth_compare_op);
        dss.depthBoundsTestEnable = req.depth_bounds_test_enable != 0 ? VK_TRUE : VK_FALSE;
        dss.stencilTestEnable = req.stencil_test_enable != 0 ? VK_TRUE : VK_FALSE;
        // (GL/zink) RENDER CORRECTNESS: rebuild the STATIC front/back VkStencilOpState the
        // app baked into the pipeline (OpenSCAD's OpenCSG CSG compositing is stencil-driven). The
        // compare-mask / write-mask / reference are CORE dynamic state the recorder replays via
        // vkCmdSetStencil*; the ops + compareOp are static here. Leaving them zeroed (compareOp
        // NEVER) silently broke CSG -- carry them faithfully.
        dss.front.failOp = static_cast<VkStencilOp>(req.stencil_front_fail_op);
        dss.front.passOp = static_cast<VkStencilOp>(req.stencil_front_pass_op);
        dss.front.depthFailOp = static_cast<VkStencilOp>(req.stencil_front_depth_fail_op);
        dss.front.compareOp = static_cast<VkCompareOp>(req.stencil_front_compare_op);
        dss.front.compareMask = static_cast<std::uint32_t>(req.stencil_front_compare_mask);
        dss.front.writeMask = static_cast<std::uint32_t>(req.stencil_front_write_mask);
        dss.front.reference = static_cast<std::uint32_t>(req.stencil_front_reference);
        dss.back.failOp = static_cast<VkStencilOp>(req.stencil_back_fail_op);
        dss.back.passOp = static_cast<VkStencilOp>(req.stencil_back_pass_op);
        dss.back.depthFailOp = static_cast<VkStencilOp>(req.stencil_back_depth_fail_op);
        dss.back.compareOp = static_cast<VkCompareOp>(req.stencil_back_compare_op);
        dss.back.compareMask = static_cast<std::uint32_t>(req.stencil_back_compare_mask);
        dss.back.writeMask = static_cast<std::uint32_t>(req.stencil_back_write_mask);
        dss.back.reference = static_cast<std::uint32_t>(req.stencil_back_reference);
        dss.minDepthBounds = static_cast<float>(req.min_depth_bounds);
        dss.maxDepthBounds = static_cast<float>(req.max_depth_bounds);
    }
    VkPipelineTessellationStateCreateInfo tess{};
    tess.sType = VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO;
    tess.patchControlPoints = static_cast<std::uint32_t>(req.patch_control_points);
    VkGraphicsPipelineCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    ci.stageCount = static_cast<std::uint32_t>(stages.size());
    ci.pStages = stages.data();
    ci.pVertexInputState = &vi;
    ci.pInputAssemblyState = &ia;
    ci.pTessellationState = req.patch_control_points > 0 ? &tess : nullptr;
    ci.pViewportState = &vp;
    ci.pRasterizationState = &rs;
    ci.pMultisampleState = &ms;
    // A DR pipeline with NO declared depth/stencil format IGNORES any carried depth-stencil
    // state per spec -- honor that by not forwarding it (zink >= 25 ships a disabled struct on
    // color-only pipelines). Render-pass pipelines keep the strict iff admitted above.
    const bool ds_ignored_by_dr =
        req.has_dynamic_rendering != 0 && req.dr_depth_format == 0 && req.dr_stencil_format == 0;
    ci.pDepthStencilState = (req.has_depth_stencil && !ds_ignored_by_dr) ? &dss : nullptr;
    ci.pColorBlendState = &cb;
    ci.pDynamicState = &ds;
    // (native lane): a dynamic-rendering pipeline is created with renderPass ==
    // VK_NULL_HANDLE + a VkPipelineRenderingCreateInfo (the attachment formats) on the create-info
    // pNext -- forwarding it is MANDATORY (the NVIDIA driver segfaults on a NULL-renderpass
    // pipeline with no rendering info). The struct must outlive vkCreateGraphicsPipelines, so it
    // lives in this function scope; dr_color_vk (built in the fork) backs pColorAttachmentFormats.
    VkPipelineRenderingCreateInfo dr_rci{};
    if (is_dr) {
        dr_rci.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
        dr_rci.viewMask = static_cast<std::uint32_t>(req.dr_view_mask);
        dr_rci.colorAttachmentCount = static_cast<std::uint32_t>(dr_color_vk.size());
        dr_rci.pColorAttachmentFormats = dr_color_vk.empty() ? nullptr : dr_color_vk.data();
        dr_rci.depthAttachmentFormat = static_cast<VkFormat>(req.dr_depth_format);
        dr_rci.stencilAttachmentFormat = static_cast<VkFormat>(req.dr_stencil_format);
        ci.pNext = &dr_rci;
    }
    ci.layout = layout->second.vk;
    ci.renderPass = rp_vk; // VK_NULL_HANDLE for a dynamic-rendering pipeline
    ci.subpass = static_cast<std::uint32_t>(req.subpass < 0 ? 0 : req.subpass);
    ci.basePipelineIndex = -1;
    VkPipeline pipeline = VK_NULL_HANDLE;
    const VkResult pr =
        vkCreateGraphicsPipelines(dev->second.vk, VK_NULL_HANDLE, 1, &ci, nullptr, &pipeline);
    if (pr != VK_SUCCESS || pipeline == VK_NULL_HANDLE) {
        resp.ok = false;
        resp.reason = "vkCreateGraphicsPipelines failed (VkResult " + std::to_string(pr) + ")";
        return resp;
    }
    const std::uint64_t h = next_handle_++;
    RealPipeline p;
    p.vk = pipeline;
    p.device = req.device;
    p.layout = req.layout;
    p.render_pass = req.render_pass;
    p.vertex_binding_count = static_cast<int>(vk_bindings.size()); // 0 = bufferless
    p.has_depth = req.has_depth_stencil;
    // (dynamic rendering): the DR compatibility key (formats + viewMask) draw validation
    // compares against the active dynamic-rendering scope.
    if (is_dr) {
        p.dynamic_rendering = true;
        p.view_mask = static_cast<std::uint32_t>(req.dr_view_mask);
        p.color_formats = dr_color_vk;
        p.depth_format = static_cast<VkFormat>(req.dr_depth_format);
        p.stencil_format = static_cast<VkFormat>(req.dr_stencil_format);
    }
    pipelines_.emplace(h, p);
    dev->second.pipelines.insert(h);
    resp.ok = true;
    resp.reason = "ok";
    resp.pipeline = h;
    return resp;
}

// Compute: the bounded compute-create subset -- one COMPUTE stage, a live module +
// layout, entry point forwarded faithfully; pipeline_cache must be 0 (shape symmetry with the
// graphics path; the ICD never forwards the app's cache).
vkrpc::CreateComputePipelinesResponse
RealVulkanBackend::create_compute_pipelines(const vkrpc::CreateComputePipelinesRequest& req) {
    vkrpc::CreateComputePipelinesResponse resp;
    const auto dev = devices_.find(req.device);
    if (dev == devices_.end()) {
        resp.ok = false;
        resp.reason = "unknown device handle";
        return resp;
    }
    if (req.pipeline_cache != 0) {
        resp.ok = false;
        resp.reason = "compute pipeline requires pipelineCache == VK_NULL_HANDLE";
        return resp;
    }
    const auto sm = shader_modules_.find(req.shader_module);
    if (sm == shader_modules_.end() || sm->second.device != req.device) {
        resp.ok = false;
        resp.reason = "compute pipeline references a shader module not on the device";
        return resp;
    }
    const auto layout = pipeline_layouts_.find(req.layout);
    if (layout == pipeline_layouts_.end() || layout->second.device != req.device) {
        resp.ok = false;
        resp.reason = "compute pipeline references a pipeline layout not on the device";
        return resp;
    }
    if (req.entry_point.empty()) {
        resp.ok = false;
        resp.reason = "compute pipeline entry point must be non-empty";
        return resp;
    }
    VkComputePipelineCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = sm->second.vk;
    ci.stage.pName = req.entry_point.c_str();
    ci.layout = layout->second.vk;
    ci.basePipelineIndex = -1;
    // Vulkan 1.3 support (subgroupSizeControl): the compute stage's admitted flags + required-size
    // pNext, gated on the device's enabled vk13 bits (mock == real).
    VkPipelineShaderStageRequiredSubgroupSizeCreateInfo rss{};
    if (req.stage_flags != 0 || req.required_subgroup_size != 0) {
        if ((dev->second.vk13_feature_bits & vkrpc::kVk13FeatureSubgroupSizeControl) == 0) {
            resp.ok = false;
            resp.reason = "compute stage subgroup-size shape requires the enabled "
                          "subgroupSizeControl feature";
            return resp;
        }
        const long long allowed =
            VK_PIPELINE_SHADER_STAGE_CREATE_ALLOW_VARYING_SUBGROUP_SIZE_BIT |
            ((dev->second.vk13_feature_bits & vkrpc::kVk13FeatureComputeFullSubgroups) != 0
                 ? VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT
                 : 0);
        if ((req.stage_flags & ~allowed) != 0) {
            resp.ok = false;
            resp.reason = "compute stage flags outside the served set";
            return resp;
        }
        ci.stage.flags = static_cast<VkPipelineShaderStageCreateFlags>(req.stage_flags);
        if (req.required_subgroup_size != 0) {
            rss.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO;
            rss.requiredSubgroupSize = static_cast<std::uint32_t>(req.required_subgroup_size);
            ci.stage.pNext = &rss;
        }
    }
    VkPipeline pipeline = VK_NULL_HANDLE;
    const VkResult pr =
        vkCreateComputePipelines(dev->second.vk, VK_NULL_HANDLE, 1, &ci, nullptr, &pipeline);
    if (pr != VK_SUCCESS || pipeline == VK_NULL_HANDLE) {
        resp.ok = false;
        resp.reason = "vkCreateComputePipelines failed (VkResult " + std::to_string(pr) + ")";
        return resp;
    }
    const std::uint64_t h = next_handle_++;
    RealPipeline p;
    p.vk = pipeline;
    p.device = req.device;
    p.layout = req.layout;
    p.compute = true; // KIND: bind point must match (validated at record)
    pipelines_.emplace(h, p);
    dev->second.pipelines.insert(h);
    resp.ok = true;
    resp.reason = "ok";
    resp.pipeline = h;
    return resp;
}

vkrpc::StatusResponse RealVulkanBackend::destroy_pipeline(const vkrpc::HandleRequest& req) {
    vkrpc::StatusResponse resp;
    const auto it = pipelines_.find(req.handle);
    if (it == pipelines_.end()) {
        resp.ok = false;
        resp.reason = "unknown pipeline handle";
        return resp;
    }
    invalidate_cbs_referencing(req.handle);
    const auto dev = devices_.find(it->second.device);
    if (dev != devices_.end() && dev->second.vk != VK_NULL_HANDLE) {
        vkDestroyPipeline(dev->second.vk, it->second.vk, nullptr);
        dev->second.pipelines.erase(req.handle);
    }
    pipelines_.erase(it);
    resp.ok = true;
    resp.reason = "ok";
    return resp;
}

} // namespace vkr::worker
