// RPC instrumentation.
//
// A pure, header-only aggregation model (the icd_subset.h precedent) so the arithmetic is
// unit-tested directly on both platforms with no clocks involved. One RpcProfile per PROCESS:
// the ICD process records the CLIENT end of every call_rpc round-trip; the worker process records
// the SERVE end of every dispatched op. Nothing here reads clocks, allocates, locks, or writes --
// callers pass microsecond durations in and drain the dump through a sink at teardown, so the
// hot-path cost is integer arithmetic on fixed arrays (and zero when profiling is off: the hooks
// never construct this object).
//
// Frame semantics (locked, guardrail): the QueuePresent RPC is recorded INTO the
// frame it closes, then frame_mark() rolls to the next open frame. The first present only OPENS
// frame timing (present-to-present wall time needs two presents); ops before it are counted
// per-op but belong to no frame.
#ifndef VKRELAY2_COMMON_VKRPC_RPC_PROFILE_H
#define VKRELAY2_COMMON_VKRPC_RPC_PROFILE_H

#include "common/vkrpc/rpc.hpp"

#include <cstdint>
#include <cstring>
#include <string>

namespace vkr::vkrpc {

constexpr std::size_t kProfileMaxOps = 128; // fixed slots indexed by raw op value
constexpr std::size_t kProfileBuckets = 12; // <64us, <128, ... <65536us(64ms), >=64ms
constexpr std::size_t kProfileFrameRing = 512;

// NOTE: the RpcOp enumerators for the two Vulkan-create ops that collide with windows.h A/W macros
// are named `CreateSemaphoreOp` / `CreateEventOp` (rpc.hpp), so this header needs NO macro guard --
// the case labels below can never be macro-mangled.

// Total over ALL op values: a raw wire value outside the known enum -- a
// future peer or a malformed client -- must never index outside the table. Known ops get their
// wire name as a static literal; gaps/future values return nullptr (callers format op_<n>).
// The literal form exists so the async-signal-safe dump path below never allocates.
inline const char* profile_op_name_cstr(std::uint32_t op) {
    switch (static_cast<RpcOp>(op)) {
    case RpcOp::Invalid:
        return "invalid";
    case RpcOp::NegotiateCapabilities:
        return "negotiate_capabilities";
    case RpcOp::CreateInstance:
        return "create_instance";
    case RpcOp::EnumeratePhysicalDevices:
        return "enumerate_physical_devices";
    case RpcOp::CreateDevice:
        return "create_device";
    case RpcOp::DestroyDevice:
        return "destroy_device";
    case RpcOp::DestroyInstance:
        return "destroy_instance";
    case RpcOp::GetDeviceQueue:
        return "get_device_queue";
    case RpcOp::CreateCommandPool:
        return "create_command_pool";
    case RpcOp::DestroyCommandPool:
        return "destroy_command_pool";
    case RpcOp::AllocateCommandBuffers:
        return "allocate_command_buffers";
    case RpcOp::FreeCommandBuffers:
        return "free_command_buffers";
    case RpcOp::CreateFence:
        return "create_fence";
    case RpcOp::DestroyFence:
        return "destroy_fence";
    case RpcOp::CreateSemaphoreOp:
        return "create_semaphore";
    case RpcOp::DestroySemaphore:
        return "destroy_semaphore";
    case RpcOp::AllocateMemory:
        return "allocate_memory";
    case RpcOp::FreeMemory:
        return "free_memory";
    case RpcOp::CreateSurface:
        return "create_surface";
    case RpcOp::DestroySurface:
        return "destroy_surface";
    case RpcOp::CreateSwapchain:
        return "create_swapchain";
    case RpcOp::DestroySwapchain:
        return "destroy_swapchain";
    case RpcOp::GetSwapchainImages:
        return "get_swapchain_images";
    case RpcOp::AcquireNextImage:
        return "acquire_next_image";
    case RpcOp::QueuePresent:
        return "queue_present";
    case RpcOp::RecordCommandBuffer:
        return "record_command_buffer";
    case RpcOp::QueueSubmit:
        return "queue_submit";
    case RpcOp::ResetFences:
        return "reset_fences";
    case RpcOp::WaitForFences:
        return "wait_for_fences";
    case RpcOp::GetFenceStatus:
        return "get_fence_status";
    case RpcOp::GetSurfaceCapabilities:
        return "get_surface_capabilities";
    case RpcOp::GetSurfaceFormats:
        return "get_surface_formats";
    case RpcOp::GetSurfacePresentModes:
        return "get_surface_present_modes";
    case RpcOp::GetSurfaceSupport:
        return "get_surface_support";
    case RpcOp::CreateImageView:
        return "create_image_view";
    case RpcOp::DestroyImageView:
        return "destroy_image_view";
    case RpcOp::CreateShaderModule:
        return "create_shader_module";
    case RpcOp::DestroyShaderModule:
        return "destroy_shader_module";
    case RpcOp::CreateRenderPass:
        return "create_render_pass";
    case RpcOp::DestroyRenderPass:
        return "destroy_render_pass";
    case RpcOp::CreateFramebuffer:
        return "create_framebuffer";
    case RpcOp::DestroyFramebuffer:
        return "destroy_framebuffer";
    case RpcOp::CreatePipelineLayout:
        return "create_pipeline_layout";
    case RpcOp::DestroyPipelineLayout:
        return "destroy_pipeline_layout";
    case RpcOp::CreateGraphicsPipelines:
        return "create_graphics_pipelines";
    case RpcOp::DestroyPipeline:
        return "destroy_pipeline";
    case RpcOp::GetPhysicalDeviceMemoryProperties:
        return "get_physical_device_memory_properties";
    case RpcOp::CreateBuffer:
        return "create_buffer";
    case RpcOp::DestroyBuffer:
        return "destroy_buffer";
    case RpcOp::BindBufferMemory:
        return "bind_buffer_memory";
    case RpcOp::WriteMemoryRanges:
        return "write_memory_ranges";
    case RpcOp::CreateDescriptorSetLayout:
        return "create_descriptor_set_layout";
    case RpcOp::DestroyDescriptorSetLayout:
        return "destroy_descriptor_set_layout";
    case RpcOp::CreateDescriptorPool:
        return "create_descriptor_pool";
    case RpcOp::DestroyDescriptorPool:
        return "destroy_descriptor_pool";
    case RpcOp::AllocateDescriptorSets:
        return "allocate_descriptor_sets";
    case RpcOp::UpdateDescriptorSets:
        return "update_descriptor_sets";
    case RpcOp::GetPhysicalDeviceFormatProperties:
        return "get_physical_device_format_properties";
    case RpcOp::CreateImage:
        return "create_image";
    case RpcOp::DestroyImage:
        return "destroy_image";
    case RpcOp::BindImageMemory:
        return "bind_image_memory";
    case RpcOp::CreateSampler:
        return "create_sampler";
    case RpcOp::DestroySampler:
        return "destroy_sampler";
    case RpcOp::GetPhysicalDeviceFeatures:
        return "get_physical_device_features";
    case RpcOp::GetPhysicalDeviceImageFormatProperties:
        return "get_physical_device_image_format_properties";
    case RpcOp::GetPhysicalDeviceProperties:
        return "get_physical_device_properties";
    case RpcOp::GetPhysicalDeviceCapabilityChain:
        return "get_physical_device_capability_chain";
    case RpcOp::CreateBufferView:
        return "create_buffer_view";
    case RpcOp::DestroyBufferView:
        return "destroy_buffer_view";
    case RpcOp::WaitSemaphores:
        return "wait_semaphores";
    case RpcOp::SignalSemaphore:
        return "signal_semaphore";
    case RpcOp::GetSemaphoreCounterValue:
        return "get_semaphore_counter_value";
    case RpcOp::CreateQueryPool:
        return "create_query_pool";
    case RpcOp::DestroyQueryPool:
        return "destroy_query_pool";
    case RpcOp::GetQueryPoolResults:
        return "get_query_pool_results";
    case RpcOp::ResetQueryPool:
        return "reset_query_pool";
    case RpcOp::ReadMemoryRanges:
        return "read_memory_ranges";
    case RpcOp::QueueWaitIdle:
        return "queue_wait_idle";
    case RpcOp::DeviceWaitIdle:
        return "device_wait_idle";
    case RpcOp::RecordCommandBufferRaw:
        return "record_command_buffer_raw";
    case RpcOp::CreateComputePipelines:
        return "create_compute_pipelines";
    case RpcOp::CreateGraphicsPipelinesRaw:
        return "create_graphics_pipelines_raw";
    case RpcOp::CreateComputePipelinesRaw:
        return "create_compute_pipelines_raw";
    case RpcOp::CreateEventOp:
        return "create_event";
    case RpcOp::DestroyEvent:
        return "destroy_event";
    case RpcOp::GetEventStatus:
        return "get_event_status";
    case RpcOp::SetEvent:
        return "set_event";
    case RpcOp::ResetEvent:
        return "reset_event";
    case RpcOp::QueueSubmit2:
        return "queue_submit2";
    case RpcOp::GetBufferDeviceAddress:
        return "get_buffer_device_address";
    case RpcOp::GetDescriptorSetLayoutSupport:
        return "get_descriptor_set_layout_support";
    case RpcOp::GetDeviceBufferMemoryRequirements:
        return "get_device_buffer_memory_requirements";
    case RpcOp::GetDeviceImageMemoryRequirements:
        return "get_device_image_memory_requirements";
    }
    return nullptr; // the retired gap + anything future/unknown
}

inline std::string profile_op_name(std::uint32_t op) {
    const char* name = profile_op_name_cstr(op);
    return name != nullptr ? std::string(name) : "op_" + std::to_string(op);
}

// Latency histogram bucket for a microsecond duration: <64, <128, <256, <512, <1024, <2048,
// <4096, <8192, <16384, <32768, <65536, >=65536 (>= 64ms).
inline std::size_t profile_bucket(std::uint64_t us) {
    std::uint64_t edge = 64;
    for (std::size_t i = 0; i + 1 < kProfileBuckets; ++i) {
        if (us < edge) {
            return i;
        }
        edge <<= 1;
    }
    return kProfileBuckets - 1;
}

// ---- Async-signal-safe dump path ----
// timeout(1) signals its command's whole PROCESS GROUP, so a profiled app dies on SIGTERM with
// atexit never running -- exactly how 6.2 lost the client tables for glxgears / the OpenSCAD
// GUI. This bounded cursor lets a signal handler emit the SAME dump grammar with no allocation,
// stdio, locale, or locks (write(2) on the result is the only syscall the handler needs).
// Byte-identity with dump() is pinned in unit_vkrpc, so the strict profile_report.sh parser
// never grows a second grammar.
struct ProfileDumpCursor {
    char* buf;
    std::size_t cap;
    std::size_t len = 0;
    bool overflow = false;

    void put_raw(const char* s, std::size_t n) {
        if (overflow || len + n > cap) {
            overflow = true;
            return;
        }
        std::memcpy(buf + len, s, n);
        len += n;
    }
    void put_str(const char* s) { put_raw(s, std::strlen(s)); }
    void put_u64(std::uint64_t v) {
        char tmp[20]; // u64 max = 20 digits
        std::size_t n = 0;
        do {
            tmp[n++] = static_cast<char>('0' + (v % 10));
            v /= 10;
        } while (v != 0);
        if (overflow || len + n > cap) {
            overflow = true;
            return;
        }
        while (n > 0) {
            buf[len++] = tmp[--n];
        }
    }
};

struct ProfileOpStats {
    std::uint64_t count = 0;
    std::uint64_t bytes_out = 0;
    std::uint64_t bytes_in = 0;
    std::uint64_t total_us = 0;
    std::uint64_t min_us = ~0ull; // printed as 0 while count == 0
    std::uint64_t max_us = 0;
    std::uint32_t hist[kProfileBuckets] = {};

    void add(std::uint64_t out, std::uint64_t in, std::uint64_t us) {
        ++count;
        bytes_out += out;
        bytes_in += in;
        total_us += us;
        if (us < min_us) {
            min_us = us;
        }
        if (us > max_us) {
            max_us = us;
        }
        ++hist[profile_bucket(us)];
    }
};

// the WORKER record_command_buffer handler's internal phase split -- the
// go/no-go evidence. json_parse (wire text -> json::Value) + decode (json::Value ->
// request structs incl. args_blob hex) are what a binary-framed record stream would remove;
// validate (resolve + stream validation) + replay (vkBegin/vkCmd*/vkEnd, real backend only)
// would remain. execute is the whole backend call (validate + replay + slot locks +
// window-thread dispatch), so execute - validate - replay = dispatch/lock residue.
struct RecordPhaseStats {
    std::uint64_t count = 0;    // record ops measured
    std::uint64_t commands = 0; // RecordedCommands decoded across them
    std::uint64_t json_parse_us = 0;
    std::uint64_t decode_us = 0;
    std::uint64_t validate_us = 0;
    std::uint64_t replay_us = 0; // stays 0 on the mock backend (validate-only) -- honest
    std::uint64_t execute_us = 0;
};

// The ICD's coherent-flush-at-submit sweep (client end only). The sweep
// memcmps candidate shadow allocations OUTSIDE any RPC op, so its cost is invisible to the op
// table by construction -- these counters make it visible. scan_bytes = allocation bytes the
// sweep was ELIGIBLE to diff (the full-memcmp cost driver); filtered_bytes = bytes the page
// pre-filter actually let through to memcmp (== scan_bytes when no filter is active);
// payload_bytes = dirty bytes that actually shipped.
struct UploadSweepStats {
    std::uint64_t count = 0; // QUEUE-SUBMIT sweeps only (vkFlushMappedMemoryRanges' explicit
                             // snapshots are app-directed, unfiltered, and NOT counted here)
    std::uint64_t scan_bytes = 0;
    std::uint64_t filtered_bytes = 0;
    std::uint64_t payload_bytes = 0;
    std::uint64_t us = 0; // wall time of snapshot building (diff included)
};

struct RpcProfile {
    ProfileOpStats per_op[kProfileMaxOps];
    ProfileOpStats unknown; // raw op values >= kProfileMaxOps (never index out of the table)
    RecordPhaseStats record_phases; // worker end only (the serve loop + backend fill it)
    UploadSweepStats upload_sweep;  // client end only (the ICD's QUEUE-SUBMIT sweep fills it)
    bool used_as_worker = false;    // set by the serve hook; the dump's end label

    // Frame rollup (aggregates; bounded memory). "Open frame" accumulators gather ops until the
    // next present closes them.
    std::uint64_t frames = 0;
    std::uint64_t frame_ops_total = 0;
    std::uint64_t frame_bytes_total = 0;
    std::uint64_t frame_us_total = 0;
    std::uint64_t frame_us_min = ~0ull;
    std::uint64_t frame_us_max = 0;
    std::uint64_t open_frame_ops = 0;
    std::uint64_t open_frame_bytes = 0;
    bool frame_open = false;           // set by the first present (an explicit flag -- a 0
    std::uint64_t last_present_us = 0; // timestamp must not read as "no present yet")

    // Optional bounded ring of per-frame wall times (VKRELAY2_PROFILE_FRAMES=1): spikes are what
    // users feel; averages hide them. Fixed storage, overwrites oldest past kProfileFrameRing.
    bool ring_enabled = false;
    std::uint64_t ring_total = 0; // frames ever pushed (ring holds the last kProfileFrameRing)
    std::uint32_t frame_ring_us[kProfileFrameRing] = {};

    void record(std::uint32_t op, std::uint64_t bytes_out, std::uint64_t bytes_in,
                std::uint64_t us) {
        ProfileOpStats& s = op < kProfileMaxOps ? per_op[op] : unknown;
        s.add(bytes_out, bytes_in, us);
        ++open_frame_ops;
        open_frame_bytes += bytes_out + bytes_in;
    }

    // Called AFTER the QueuePresent op was record()ed (so the present counts into the frame it
    // closes). The first call only opens frame timing.
    void frame_mark(std::uint64_t now_us) {
        if (frame_open) {
            const std::uint64_t us = now_us - last_present_us;
            ++frames;
            frame_us_total += us;
            if (us < frame_us_min) {
                frame_us_min = us;
            }
            if (us > frame_us_max) {
                frame_us_max = us;
            }
            frame_ops_total += open_frame_ops;
            frame_bytes_total += open_frame_bytes;
            if (ring_enabled) {
                frame_ring_us[ring_total % kProfileFrameRing] =
                    us > ~std::uint32_t{0} ? ~std::uint32_t{0} : static_cast<std::uint32_t>(us);
                ++ring_total;
            }
        }
        frame_open = true;
        last_present_us = now_us;
        open_frame_ops = 0;
        open_frame_bytes = 0;
    }

    // Emits the fixed line grammar (one record per line, key=value, no unescaped spaces in
    // values) through `sink(const std::string&)`. Teardown-time only -- never on a hot path.
    template <typename Sink> void dump(Sink&& sink) const {
        const char* end_name = used_as_worker ? "worker" : "client";
        auto emit_op = [&](const std::string& name, const ProfileOpStats& s) {
            if (s.count == 0) {
                return;
            }
            std::string line = "VKRELAY2-PROFILE end=";
            line += end_name;
            line += " op=" + name;
            line += " count=" + std::to_string(s.count);
            line += " bytes_out=" + std::to_string(s.bytes_out);
            line += " bytes_in=" + std::to_string(s.bytes_in);
            line += " total_us=" + std::to_string(s.total_us);
            line += " min_us=" + std::to_string(s.count > 0 ? s.min_us : 0);
            line += " max_us=" + std::to_string(s.max_us);
            line += " hist=";
            for (std::size_t i = 0; i < kProfileBuckets; ++i) {
                if (i > 0) {
                    line += ",";
                }
                line += std::to_string(s.hist[i]);
            }
            sink(line);
        };
        for (std::size_t op = 0; op < kProfileMaxOps; ++op) {
            emit_op(profile_op_name(static_cast<std::uint32_t>(op)), per_op[op]);
        }
        emit_op("unknown", unknown);
        if (record_phases.count > 0) {
            std::string line = "VKRELAY2-PROFILE end=";
            line += end_name;
            line += " record_phases=" + std::to_string(record_phases.count);
            line += " commands=" + std::to_string(record_phases.commands);
            line += " json_parse_us=" + std::to_string(record_phases.json_parse_us);
            line += " decode_us=" + std::to_string(record_phases.decode_us);
            line += " validate_us=" + std::to_string(record_phases.validate_us);
            line += " replay_us=" + std::to_string(record_phases.replay_us);
            line += " execute_us=" + std::to_string(record_phases.execute_us);
            sink(line);
        }
        if (upload_sweep.count > 0) {
            std::string line = "VKRELAY2-PROFILE end=";
            line += end_name;
            line += " upload_sweep=" + std::to_string(upload_sweep.count);
            line += " scan_bytes=" + std::to_string(upload_sweep.scan_bytes);
            line += " filtered_bytes=" + std::to_string(upload_sweep.filtered_bytes);
            line += " payload_bytes=" + std::to_string(upload_sweep.payload_bytes);
            line += " sweep_us=" + std::to_string(upload_sweep.us);
            sink(line);
        }
        {
            std::string line = "VKRELAY2-PROFILE end=";
            line += end_name;
            line += " frames=" + std::to_string(frames);
            line += " frame_us_total=" + std::to_string(frame_us_total);
            line += " frame_us_min=" + std::to_string(frames > 0 ? frame_us_min : 0);
            line += " frame_us_max=" + std::to_string(frame_us_max);
            line += " frame_ops_total=" + std::to_string(frame_ops_total);
            line += " frame_bytes_total=" + std::to_string(frame_bytes_total);
            sink(line);
        }
        if (ring_enabled && ring_total > 0) {
            // Chronological order of the retained window (oldest first).
            const std::uint64_t n = ring_total < kProfileFrameRing ? ring_total : kProfileFrameRing;
            const std::uint64_t start = ring_total - n;
            std::string line = "VKRELAY2-PROFILE end=";
            line += end_name;
            line += " frame_ring_us=";
            for (std::uint64_t i = 0; i < n; ++i) {
                if (i > 0) {
                    line += ",";
                }
                line += std::to_string(frame_ring_us[(start + i) % kProfileFrameRing]);
            }
            sink(line);
        }
    }

    // The async-signal-safe twin of dump(): same records, each line '\n'-terminated, into a
    // caller-owned fixed buffer; returns bytes written. Whole lines only -- a line that would
    // overflow is rolled back and emission stops, so a truncated dump still STRICT-parses.
    // Byte-identity with dump() (modulo the line terminator) is pinned in unit_vkrpc.
    std::size_t format_dump(char* buf, std::size_t cap) const {
        ProfileDumpCursor c{buf, cap};
        const char* end_name = used_as_worker ? "worker" : "client";
        auto emit_op = [&](std::uint32_t op_or_unknown, bool is_unknown, const ProfileOpStats& s) {
            if (s.count == 0 || c.overflow) {
                return;
            }
            const std::size_t mark = c.len;
            c.put_str("VKRELAY2-PROFILE end=");
            c.put_str(end_name);
            c.put_str(" op=");
            if (is_unknown) {
                c.put_str("unknown");
            } else if (const char* name = profile_op_name_cstr(op_or_unknown)) {
                c.put_str(name);
            } else {
                c.put_str("op_");
                c.put_u64(op_or_unknown);
            }
            c.put_str(" count=");
            c.put_u64(s.count);
            c.put_str(" bytes_out=");
            c.put_u64(s.bytes_out);
            c.put_str(" bytes_in=");
            c.put_u64(s.bytes_in);
            c.put_str(" total_us=");
            c.put_u64(s.total_us);
            c.put_str(" min_us=");
            c.put_u64(s.count > 0 ? s.min_us : 0);
            c.put_str(" max_us=");
            c.put_u64(s.max_us);
            c.put_str(" hist=");
            for (std::size_t i = 0; i < kProfileBuckets; ++i) {
                if (i > 0) {
                    c.put_str(",");
                }
                c.put_u64(s.hist[i]);
            }
            c.put_str("\n");
            if (c.overflow) {
                c.len = mark; // drop the partial line; the dump stays parseable
            }
        };
        for (std::size_t op = 0; op < kProfileMaxOps; ++op) {
            emit_op(static_cast<std::uint32_t>(op), false, per_op[op]);
        }
        emit_op(0, true, unknown);
        if (!c.overflow && record_phases.count > 0) {
            const std::size_t mark = c.len;
            c.put_str("VKRELAY2-PROFILE end=");
            c.put_str(end_name);
            c.put_str(" record_phases=");
            c.put_u64(record_phases.count);
            c.put_str(" commands=");
            c.put_u64(record_phases.commands);
            c.put_str(" json_parse_us=");
            c.put_u64(record_phases.json_parse_us);
            c.put_str(" decode_us=");
            c.put_u64(record_phases.decode_us);
            c.put_str(" validate_us=");
            c.put_u64(record_phases.validate_us);
            c.put_str(" replay_us=");
            c.put_u64(record_phases.replay_us);
            c.put_str(" execute_us=");
            c.put_u64(record_phases.execute_us);
            c.put_str("\n");
            if (c.overflow) {
                c.len = mark;
            }
        }
        if (!c.overflow && upload_sweep.count > 0) {
            const std::size_t mark = c.len;
            c.put_str("VKRELAY2-PROFILE end=");
            c.put_str(end_name);
            c.put_str(" upload_sweep=");
            c.put_u64(upload_sweep.count);
            c.put_str(" scan_bytes=");
            c.put_u64(upload_sweep.scan_bytes);
            c.put_str(" filtered_bytes=");
            c.put_u64(upload_sweep.filtered_bytes);
            c.put_str(" payload_bytes=");
            c.put_u64(upload_sweep.payload_bytes);
            c.put_str(" sweep_us=");
            c.put_u64(upload_sweep.us);
            c.put_str("\n");
            if (c.overflow) {
                c.len = mark;
            }
        }
        if (!c.overflow) {
            const std::size_t mark = c.len;
            c.put_str("VKRELAY2-PROFILE end=");
            c.put_str(end_name);
            c.put_str(" frames=");
            c.put_u64(frames);
            c.put_str(" frame_us_total=");
            c.put_u64(frame_us_total);
            c.put_str(" frame_us_min=");
            c.put_u64(frames > 0 ? frame_us_min : 0);
            c.put_str(" frame_us_max=");
            c.put_u64(frame_us_max);
            c.put_str(" frame_ops_total=");
            c.put_u64(frame_ops_total);
            c.put_str(" frame_bytes_total=");
            c.put_u64(frame_bytes_total);
            c.put_str("\n");
            if (c.overflow) {
                c.len = mark;
            }
        }
        if (!c.overflow && ring_enabled && ring_total > 0) {
            const std::size_t mark = c.len;
            const std::uint64_t n = ring_total < kProfileFrameRing ? ring_total : kProfileFrameRing;
            const std::uint64_t start = ring_total - n;
            c.put_str("VKRELAY2-PROFILE end=");
            c.put_str(end_name);
            c.put_str(" frame_ring_us=");
            for (std::uint64_t i = 0; i < n; ++i) {
                if (i > 0) {
                    c.put_str(",");
                }
                c.put_u64(frame_ring_us[(start + i) % kProfileFrameRing]);
            }
            c.put_str("\n");
            if (c.overflow) {
                c.len = mark;
            }
        }
        return c.len;
    }
};

} // namespace vkr::vkrpc

#endif // VKRELAY2_COMMON_VKRPC_RPC_PROFILE_H
