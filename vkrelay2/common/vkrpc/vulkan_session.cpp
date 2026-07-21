#include "common/vkrpc/vulkan_session.hpp"

#include "common/logging/logging.hpp"
#include "common/protocol/wire.hpp"
#include "common/vkrpc/indirect_draw_validation.h"
#include "common/vkrpc/rpc_profile.h"

#include <algorithm>
#include <chrono>
#include <climits>
#include <csignal> // std::sig_atomic_t (the shared one-shot dump flag, both platforms)
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <string_view>   // cmd_kind_from_string's static lookup table
#include <unordered_map> // ditto
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace vkr::vkrpc {
namespace {

constexpr char kComponent[] = "vkrpc";

// AMD-iGPU investigation: one session-owned trace at the worker's DECODED request boundary. The
// supervisor sets VKRELAY2_OP_TRACE_PATH only for a LaunchSession that explicitly requested it;
// daemon reuse therefore cannot accidentally enable tracing. Lines are bounded and carry exact
// scalar/payload hashes, selected decoded draw/pipeline fields, and first-seen ordinals rather than
// raw object handles or application payloads. The raw and ordinal hashes of generic u64 payloads
// are both retained: the first detects true value changes, while the second discounts
// adapter-dependent handle numbering during a cross-run diff.
class DecodedOpTrace {
  public:
    DecodedOpTrace() {
        const char* path = std::getenv("VKRELAY2_OP_TRACE_PATH");
        if (path == nullptr || path[0] == '\0') {
            return;
        }
        out_.open(path, std::ios::out | std::ios::trunc);
        if (!out_) {
            std::fprintf(stderr, "vkrelay2-optrace: failed to open %s\n", path);
            return;
        }
        std::fprintf(stderr, "vkrelay2-optrace: path=%s\n", path);
        out_ << "{\"event\":\"header\",\"version\":2,\"max_recordings\":" << kMaxRecordings
             << ",\"max_commands\":" << kMaxCommands
             << ",\"max_upload_ranges\":" << kMaxUploadRanges << ",\"max_events\":" << kMaxEvents
             << "}\n";
        out_.flush();
    }

    void record_command_buffer(std::uint32_t request_id, const RecordCommandBufferRequest& req) {
        if (!can_emit() || recordings_ >= kMaxRecordings || commands_ >= kMaxCommands) {
            note_truncated();
            return;
        }
        const std::uint64_t recording = recordings_++;
        out_ << "{\"event\":\"record\",\"seq\":" << seq_++ << ",\"request\":" << request_id
             << ",\"recording\":" << recording << ",\"command_buffer\":\"cb#"
             << ordinal("cb", req.command_buffer)
             << "\",\"one_time\":" << (req.one_time_submit ? "true" : "false")
             << ",\"commands\":" << req.commands.size() << "}\n";

        for (std::size_t i = 0; i < req.commands.size(); ++i) {
            if (!can_emit() || commands_ >= kMaxCommands) {
                note_truncated();
                break;
            }
            const RecordedCommand& c = req.commands[i];
            TraceHash scalars;
            hash_scalar_fields(scalars, c);
            TraceHash u64_raw;
            TraceHash u64_ordinal;
            u64_raw.add_vector(c.args_u64);
            u64_ordinal.add(static_cast<std::uint64_t>(c.args_u64.size()));
            for (const std::uint64_t value : c.args_u64) {
                u64_ordinal.add(ordinal("u64", value));
            }
            TraceHash deps_raw;
            TraceHash deps_ordinal;
            TraceHash deps_scalar;
            TraceHash deps_handles;
            hash_dependencies(deps_raw, c.deps2, true, false);
            hash_dependencies(deps_ordinal, c.deps2, true, true);
            hash_dependencies(deps_scalar, c.deps2, false, false);
            hash_dependency_handles(deps_handles, c.deps2);
            const std::string named = named_handles(c);
            out_ << "{\"event\":\"command\",\"seq\":" << seq_++ << ",\"recording\":" << recording
                 << ",\"index\":" << i << ",\"kind\":\"" << c.kind << "\",\"scalar_hash\":\""
                 << hex(scalars.value) << "\",\"u64_raw_hash\":\"" << hex(u64_raw.value)
                 << "\",\"u64_ordinal_hash\":\"" << hex(u64_ordinal.value)
                 << "\",\"blob_bytes\":" << c.args_blob.size() << ",\"blob_hash\":\""
                 << hex(hash_bytes(c.args_blob.data(), c.args_blob.size()))
                 << "\",\"deps_raw_hash\":\"" << hex(deps_raw.value)
                 << "\",\"deps_ordinal_hash\":\"" << hex(deps_ordinal.value)
                 << "\",\"deps_scalar_hash\":\"" << hex(deps_scalar.value)
                 << "\",\"deps_handle_hash\":\"" << hex(deps_handles.value) << "\",\"handles\":\""
                 << named << '"';
            write_command_details(c);
            out_ << "}\n";
            ++commands_;
        }
        out_.flush();
    }

    void create_graphics_pipeline(std::uint32_t request_id,
                                  const CreateGraphicsPipelinesRequest& req,
                                  const CreateGraphicsPipelinesResponse& resp) {
        if (!can_emit()) {
            return;
        }
        out_ << "{\"event\":\"create_graphics_pipeline\",\"seq\":" << seq_++
             << ",\"request\":" << request_id << ",\"ok\":" << (resp.ok ? "true" : "false")
             << ",\"pipeline\":\"pipeline#" << ordinal("pipeline", resp.pipeline)
             << "\",\"topology\":" << req.topology << ",\"stages\":[";
        for (std::size_t i = 0; i < req.stages.size(); ++i) {
            if (i != 0) {
                out_ << ',';
            }
            const ShaderStageDesc& s = req.stages[i];
            out_ << "{\"stage\":" << s.stage << ",\"module\":\"shader_module#"
                 << ordinal("shader_module", s.module) << "\"}";
        }
        out_ << "],\"bindings\":[";
        for (std::size_t i = 0; i < req.vertex_bindings.size(); ++i) {
            if (i != 0) {
                out_ << ',';
            }
            const VertexBindingDesc& b = req.vertex_bindings[i];
            out_ << "{\"binding\":" << b.binding << ",\"stride\":" << b.stride
                 << ",\"input_rate\":" << b.input_rate << '}';
        }
        out_ << "],\"attributes\":[";
        for (std::size_t i = 0; i < req.vertex_attributes.size(); ++i) {
            if (i != 0) {
                out_ << ',';
            }
            const VertexAttributeDesc& a = req.vertex_attributes[i];
            out_ << "{\"location\":" << a.location << ",\"binding\":" << a.binding
                 << ",\"format\":" << a.format << ",\"offset\":" << a.offset << '}';
        }
        out_ << "]}\n";
        out_.flush();
    }

    void create_shader_module(std::uint32_t request_id, const CreateShaderModuleRequest& req,
                              const CreateShaderModuleResponse& resp) {
        if (!can_emit()) {
            return;
        }
        out_ << "{\"event\":\"create_shader_module\",\"seq\":" << seq_++
             << ",\"request\":" << request_id << ",\"ok\":" << (resp.ok ? "true" : "false")
             << ",\"shader_module\":\"shader_module#"
             << ordinal("shader_module", resp.shader_module) << "\",\"bytes\":" << req.code.size()
             << ",\"code_hash\":\"" << hex(hash_bytes(req.code.data(), req.code.size())) << "\"}\n";
        out_.flush();
    }

    void queue_submit(std::uint32_t request_id, const QueueSubmitRequest& req,
                      const QueueSubmitResponse& resp) {
        if (!can_emit()) {
            return;
        }
        out_ << "{\"event\":\"submit\",\"seq\":" << seq_++ << ",\"request\":" << request_id
             << ",\"ok\":" << (resp.ok ? "true" : "false") << ",\"result\":" << resp.result
             << ",\"command_buffers\":[";
        write_handle_ordinals("cb", req.command_buffers);
        out_ << "]}\n";
        out_.flush();
    }

    void queue_submit2(std::uint32_t request_id, const QueueSubmit2Request& req,
                       const QueueSubmitResponse& resp) {
        if (!can_emit()) {
            return;
        }
        out_ << "{\"event\":\"submit2\",\"seq\":" << seq_++ << ",\"request\":" << request_id
             << ",\"ok\":" << (resp.ok ? "true" : "false") << ",\"result\":" << resp.result
             << ",\"submits\":[";
        for (std::size_t i = 0; i < req.submits.size(); ++i) {
            if (i != 0) {
                out_ << ',';
            }
            out_ << "{\"command_buffers\":[";
            write_handle_ordinals("cb", req.submits[i].command_buffers);
            out_ << "]}";
        }
        out_ << "]}\n";
        out_.flush();
    }

    void write_memory_ranges(std::uint32_t request_id, const WriteMemoryRangesRequest& req) {
        if (!can_emit()) {
            return;
        }
        TraceHash shape;
        std::size_t range_count = 0;
        std::size_t payload_cursor = 0;
        shape.add(static_cast<std::uint64_t>(req.uploads.size()));
        for (std::size_t upload_index = 0; upload_index < req.uploads.size(); ++upload_index) {
            const MemoryUpload& upload = req.uploads[upload_index];
            const std::uint64_t memory_ordinal = ordinal("memory", upload.memory);
            shape.add(memory_ordinal);
            shape.add(static_cast<std::uint64_t>(upload.ranges.size()));
            range_count += upload.ranges.size();
            for (std::size_t range_index = 0; range_index < upload.ranges.size(); ++range_index) {
                const MemoryUploadRange& range = upload.ranges[range_index];
                shape.add(range.offset);
                shape.add(range.size);
                const std::size_t range_size = static_cast<std::size_t>(range.size);
                if (can_emit() && upload_ranges_ < kMaxUploadRanges) {
                    out_ << "{\"event\":\"upload_range\",\"seq\":" << seq_++
                         << ",\"request\":" << request_id << ",\"upload\":" << upload_index
                         << ",\"range\":" << range_index << ",\"memory\":\"memory#"
                         << memory_ordinal << "\",\"offset\":" << range.offset
                         << ",\"bytes\":" << range.size << ",\"payload_hash\":\""
                         << hex(hash_bytes(req.payload.data() + payload_cursor, range_size))
                         << "\"}\n";
                    ++upload_ranges_;
                } else {
                    note_truncated();
                }
                payload_cursor += range_size;
            }
        }
        if (can_emit()) {
            out_ << "{\"event\":\"upload\",\"seq\":" << seq_++ << ",\"request\":" << request_id
                 << ",\"allocations\":" << req.uploads.size() << ",\"ranges\":" << range_count
                 << ",\"bytes\":" << req.payload.size() << ",\"shape_hash\":\"" << hex(shape.value)
                 << "\",\"payload_hash\":\""
                 << hex(hash_bytes(req.payload.data(), req.payload.size())) << "\"}\n";
        }
        out_.flush();
    }

    void allocate_memory(std::uint32_t request_id, const AllocateMemoryRequest& req,
                         const AllocateMemoryResponse& resp) {
        if (!can_emit()) {
            return;
        }
        out_ << "{\"event\":\"allocate_memory\",\"seq\":" << seq_++ << ",\"request\":" << request_id
             << ",\"ok\":" << (resp.ok ? "true" : "false") << ",\"memory\":\"memory#"
             << ordinal("memory", resp.memory) << "\",\"bytes\":" << req.allocation_size
             << ",\"type_index\":" << req.memory_type_index << ",\"flags\":" << req.allocate_flags
             << "}\n";
    }

    void create_buffer(std::uint32_t request_id, const CreateBufferRequest& req,
                       const CreateBufferResponse& resp) {
        if (!can_emit()) {
            return;
        }
        out_ << "{\"event\":\"create_buffer\",\"seq\":" << seq_++ << ",\"request\":" << request_id
             << ",\"ok\":" << (resp.ok ? "true" : "false") << ",\"buffer\":\"buffer#"
             << ordinal("buffer", resp.buffer) << "\",\"bytes\":" << req.size
             << ",\"usage\":" << req.usage << ",\"mem_bytes\":" << resp.mem_size
             << ",\"mem_alignment\":" << resp.mem_alignment
             << ",\"mem_type_bits\":" << resp.mem_type_bits << "}\n";
    }

    void bind_buffer_memory(std::uint32_t request_id, const BindBufferMemoryRequest& req,
                            const StatusResponse& resp) {
        if (!can_emit()) {
            return;
        }
        out_ << "{\"event\":\"bind_buffer_memory\",\"seq\":" << seq_++
             << ",\"request\":" << request_id << ",\"ok\":" << (resp.ok ? "true" : "false")
             << ",\"buffer\":\"buffer#" << ordinal("buffer", req.buffer)
             << "\",\"memory\":\"memory#" << ordinal("memory", req.memory)
             << "\",\"offset\":" << req.memory_offset << "}\n";
    }

    void create_image(std::uint32_t request_id, const CreateImageRequest& req,
                      const CreateImageResponse& resp) {
        if (!can_emit()) {
            return;
        }
        out_ << "{\"event\":\"create_image\",\"seq\":" << seq_++ << ",\"request\":" << request_id
             << ",\"ok\":" << (resp.ok ? "true" : "false") << ",\"image\":\"image#"
             << ordinal("image", resp.image) << "\",\"format\":" << req.format << ",\"extent\":["
             << req.width << ',' << req.height << ',' << req.depth
             << "],\"mips\":" << req.mip_levels << ",\"layers\":" << req.array_layers
             << ",\"samples\":" << req.samples << ",\"tiling\":" << req.tiling
             << ",\"usage\":" << req.usage << ",\"flags\":" << req.image_flags
             << ",\"mem_bytes\":" << resp.mem_size << ",\"mem_alignment\":" << resp.mem_alignment
             << ",\"mem_type_bits\":" << resp.mem_type_bits << "}\n";
    }

    void bind_image_memory(std::uint32_t request_id, const BindImageMemoryRequest& req,
                           const StatusResponse& resp) {
        if (!can_emit()) {
            return;
        }
        out_ << "{\"event\":\"bind_image_memory\",\"seq\":" << seq_++
             << ",\"request\":" << request_id << ",\"ok\":" << (resp.ok ? "true" : "false")
             << ",\"image\":\"image#" << ordinal("image", req.image) << "\",\"memory\":\"memory#"
             << ordinal("memory", req.memory) << "\",\"offset\":" << req.memory_offset << "}\n";
    }

    void create_swapchain(std::uint32_t request_id, const CreateSwapchainRequest& req,
                          const CreateSwapchainResponse& resp) {
        if (!can_emit()) {
            return;
        }
        out_ << "{\"event\":\"create_swapchain\",\"seq\":" << seq_++
             << ",\"request\":" << request_id << ",\"ok\":" << (resp.ok ? "true" : "false")
             << ",\"result\":" << resp.result << ",\"swapchain\":\"swapchain#"
             << ordinal("swapchain", resp.swapchain) << "\",\"old_swapchain\":\"swapchain#"
             << ordinal("swapchain", req.old_swapchain) << "\",\"extent\":[" << req.width << ','
             << req.height << "],\"min_images\":" << req.min_image_count
             << ",\"present_mode\":" << req.present_mode << "}\n";
    }

    void get_swapchain_images(std::uint32_t request_id, const GetSwapchainImagesRequest& req,
                              const GetSwapchainImagesResponse& resp) {
        if (!can_emit()) {
            return;
        }
        out_ << "{\"event\":\"get_swapchain_images\",\"seq\":" << seq_++
             << ",\"request\":" << request_id << ",\"ok\":" << (resp.ok ? "true" : "false")
             << ",\"swapchain\":\"swapchain#" << ordinal("swapchain", req.swapchain)
             << "\",\"images\":[";
        for (std::size_t i = 0; i < resp.images.size(); ++i) {
            if (i != 0) {
                out_ << ',';
            }
            out_ << "\"image#" << ordinal("image", resp.images[i]) << "\"";
        }
        out_ << "]}\n";
    }

    void create_image_view(std::uint32_t request_id, const CreateImageViewRequest& req,
                           const CreateImageViewResponse& resp) {
        if (!can_emit()) {
            return;
        }
        out_ << "{\"event\":\"create_image_view\",\"seq\":" << seq_++
             << ",\"request\":" << request_id << ",\"ok\":" << (resp.ok ? "true" : "false")
             << ",\"image_view\":\"image_view#" << ordinal("image_view", resp.image_view)
             << "\",\"image\":\"image#" << ordinal("image", req.image)
             << "\",\"format\":" << req.format << ",\"aspect\":" << req.aspect << "}\n";
    }

    void acquire_next_image(std::uint32_t request_id, const AcquireNextImageRequest& req,
                            const AcquireNextImageResponse& resp) {
        if (!can_emit()) {
            return;
        }
        out_ << "{\"event\":\"acquire\",\"seq\":" << seq_++ << ",\"request\":" << request_id
             << ",\"ok\":" << (resp.ok ? "true" : "false") << ",\"result\":" << resp.result
             << ",\"swapchain\":\"swapchain#" << ordinal("swapchain", req.swapchain)
             << "\",\"image_index\":" << resp.image_index << "}\n";
    }

    void queue_present(std::uint32_t request_id, const QueuePresentRequest& req,
                       const QueuePresentResponse& resp) {
        if (!can_emit()) {
            return;
        }
        out_ << "{\"event\":\"present\",\"seq\":" << seq_++ << ",\"request\":" << request_id
             << ",\"ok\":" << (resp.ok ? "true" : "false") << ",\"result\":" << resp.result
             << ",\"targets\":[";
        for (std::size_t i = 0; i < req.presents.size(); ++i) {
            if (i != 0) {
                out_ << ',';
            }
            out_ << "{\"swapchain\":\"swapchain#" << ordinal("swapchain", req.presents[i].swapchain)
                 << "\",\"image_index\":" << req.presents[i].image_index << '}';
        }
        out_ << "]}\n";
    }

  private:
    struct TraceHash {
        std::uint64_t value = 1469598103934665603ull;

        template <typename T> void add(const T& v) {
            const auto* bytes = reinterpret_cast<const unsigned char*>(&v);
            for (std::size_t i = 0; i < sizeof(T); ++i) {
                value ^= bytes[i];
                value *= 1099511628211ull;
            }
        }

        template <typename T> void add_vector(const std::vector<T>& values) {
            add(static_cast<std::uint64_t>(values.size()));
            for (const T& item : values) {
                add(item);
            }
        }
    };

    static constexpr std::uint64_t kMaxRecordings = 4096;
    static constexpr std::uint64_t kMaxCommands = 100000;
    static constexpr std::uint64_t kMaxUploadRanges = 20000;
    static constexpr std::uint64_t kMaxEvents = 150000;

    bool can_emit() {
        if (!out_) {
            return false;
        }
        if (seq_ >= kMaxEvents) {
            note_truncated();
            return false;
        }
        return true;
    }

    static std::uint64_t hash_bytes(const void* data, std::size_t size) {
        TraceHash h;
        const auto* bytes = static_cast<const unsigned char*>(data);
        for (std::size_t i = 0; i < size; ++i) {
            h.add(bytes[i]);
        }
        return h.value;
    }

    static std::string hex(std::uint64_t value) {
        std::ostringstream out;
        out << "0x" << std::hex << std::setfill('0') << std::setw(16) << value;
        return out.str();
    }

    void write_handle_ordinals(const char* category, const std::vector<std::uint64_t>& values) {
        for (std::size_t i = 0; i < values.size(); ++i) {
            if (i != 0) {
                out_ << ',';
            }
            out_ << '"' << category << '#' << ordinal(category, values[i]) << '"';
        }
    }

    void write_command_details(const RecordedCommand& c) {
        if (c.kind == "bind_pipeline") {
            out_ << ",\"pipeline\":\"pipeline#" << ordinal("pipeline", c.pipeline)
                 << "\",\"bind_point\":" << (c.args_i64.empty() ? 0 : c.args_i64[0]);
        } else if (c.kind == "bind_vertex_buffers") {
            out_ << ",\"first_binding\":" << c.first_binding << ",\"vertex_buffers\":[";
            for (std::size_t i = 0; i < c.vertex_buffers.size(); ++i) {
                if (i != 0) {
                    out_ << ',';
                }
                out_ << "{\"buffer\":\"buffer#" << ordinal("buffer", c.vertex_buffers[i])
                     << "\",\"offset\":";
                if (i < c.vertex_buffer_offsets.size()) {
                    out_ << c.vertex_buffer_offsets[i];
                } else {
                    out_ << "null";
                }
                out_ << '}';
            }
            out_ << ']';
        } else if (c.kind == "bind_vertex_buffers2" && c.args_i64.size() == 4 &&
                   c.args_i64[1] > 0) {
            const std::size_t count = static_cast<std::size_t>(c.args_i64[1]);
            out_ << ",\"first_binding\":" << c.args_i64[0] << ",\"vertex_buffers\":[";
            for (std::size_t i = 0; i < count && count + i < c.args_u64.size(); ++i) {
                if (i != 0) {
                    out_ << ',';
                }
                out_ << "{\"buffer\":\"buffer#" << ordinal("buffer", c.args_u64[i])
                     << "\",\"offset\":" << c.args_u64[count + i];
                std::size_t cursor = count * 2;
                if (c.args_i64[2] == 1 && cursor + i < c.args_u64.size()) {
                    out_ << ",\"size\":" << c.args_u64[cursor + i];
                    cursor += count;
                }
                if (c.args_i64[3] == 1 && cursor + i < c.args_u64.size()) {
                    out_ << ",\"stride\":" << c.args_u64[cursor + i];
                }
                out_ << '}';
            }
            out_ << ']';
        } else if (c.kind == "bind_index_buffer" && c.args_u64.size() >= 2 && !c.args_i64.empty()) {
            out_ << ",\"index_buffer\":\"buffer#" << ordinal("buffer", c.args_u64[0])
                 << "\",\"index_offset\":" << c.args_u64[1] << ",\"index_type\":" << c.args_i64[0];
        } else if (c.kind == "draw") {
            out_ << ",\"vertex_count\":" << c.vertex_count
                 << ",\"instance_count\":" << c.instance_count
                 << ",\"first_vertex\":" << c.first_vertex
                 << ",\"first_instance\":" << c.first_instance;
        } else if (c.kind == "draw_indexed" && c.args_i64.size() == 5) {
            out_ << ",\"index_count\":" << c.args_i64[0] << ",\"instance_count\":" << c.args_i64[1]
                 << ",\"first_index\":" << c.args_i64[2] << ",\"vertex_offset\":" << c.args_i64[3]
                 << ",\"first_instance\":" << c.args_i64[4];
        }
    }

    std::uint64_t ordinal(const char* category, std::uint64_t value) {
        if (value == 0) {
            return 0;
        }
        const std::pair<std::string, std::uint64_t> key{category, value};
        const auto found = ordinals_.find(key);
        if (found != ordinals_.end()) {
            return found->second;
        }
        const std::uint64_t next = ++next_ordinal_[category];
        ordinals_.emplace(key, next);
        return next;
    }

    static void hash_scalar_fields(TraceHash& h, const RecordedCommand& c) {
        h.add(c.old_layout);
        h.add(c.new_layout);
        h.add(c.src_stage);
        h.add(c.dst_stage);
        h.add(c.src_access);
        h.add(c.dst_access);
        h.add(c.aspect);
        h.add(c.layout);
        h.add(c.r);
        h.add(c.g);
        h.add(c.b);
        h.add(c.a);
        h.add(c.has_depth_clear);
        h.add(c.depth_clear);
        h.add(c.render_area_x);
        h.add(c.render_area_y);
        h.add(c.render_area_w);
        h.add(c.render_area_h);
        h.add(c.vp_x);
        h.add(c.vp_y);
        h.add(c.vp_w);
        h.add(c.vp_h);
        h.add(c.vp_min_depth);
        h.add(c.vp_max_depth);
        h.add(c.sc_x);
        h.add(c.sc_y);
        h.add(c.sc_w);
        h.add(c.sc_h);
        h.add(c.vertex_count);
        h.add(c.instance_count);
        h.add(c.first_vertex);
        h.add(c.first_instance);
        h.add(c.first_binding);
        h.add_vector(c.vertex_buffer_offsets);
        h.add(c.first_set);
        h.add(c.copy_width);
        h.add(c.copy_height);
        h.add(c.copy_depth);
        h.add(c.barrier_base_mip);
        h.add(c.barrier_level_count);
        h.add(c.barrier_base_layer);
        h.add(c.barrier_layer_count);
        h.add_vector(c.args_i64);
        h.add_vector(c.args_f64);
    }

    void hash_dependencies(TraceHash& h, const std::vector<DependencyInfo2>& deps,
                           bool include_handles, bool normalize_handles) {
        h.add(static_cast<std::uint64_t>(deps.size()));
        for (const DependencyInfo2& d : deps) {
            h.add(d.dependency_flags);
            h.add(static_cast<std::uint64_t>(d.memory.size()));
            for (const MemoryBarrier2& m : d.memory) {
                h.add(m.src_stage);
                h.add(m.src_access);
                h.add(m.dst_stage);
                h.add(m.dst_access);
            }
            h.add(static_cast<std::uint64_t>(d.buffer.size()));
            for (const BufferMemoryBarrier2& b : d.buffer) {
                h.add(b.src_stage);
                h.add(b.src_access);
                h.add(b.dst_stage);
                h.add(b.dst_access);
                h.add(b.src_queue_family);
                h.add(b.dst_queue_family);
                if (include_handles) {
                    h.add(normalize_handles ? ordinal("buffer", b.buffer) : b.buffer);
                }
                h.add(b.offset);
                h.add(b.size);
            }
            h.add(static_cast<std::uint64_t>(d.image.size()));
            for (const ImageMemoryBarrier2& i : d.image) {
                h.add(i.src_stage);
                h.add(i.src_access);
                h.add(i.dst_stage);
                h.add(i.dst_access);
                h.add(i.old_layout);
                h.add(i.new_layout);
                h.add(i.src_queue_family);
                h.add(i.dst_queue_family);
                if (include_handles) {
                    h.add(normalize_handles ? ordinal("image", i.image) : i.image);
                }
                h.add(i.aspect);
                h.add(i.base_mip);
                h.add(i.level_count);
                h.add(i.base_layer);
                h.add(i.layer_count);
            }
        }
    }

    void hash_dependency_handles(TraceHash& h, const std::vector<DependencyInfo2>& deps) {
        h.add(static_cast<std::uint64_t>(deps.size()));
        for (const DependencyInfo2& d : deps) {
            h.add(static_cast<std::uint64_t>(d.buffer.size()));
            for (const BufferMemoryBarrier2& b : d.buffer) {
                h.add(ordinal("buffer", b.buffer));
            }
            h.add(static_cast<std::uint64_t>(d.image.size()));
            for (const ImageMemoryBarrier2& i : d.image) {
                h.add(ordinal("image", i.image));
            }
        }
    }

    std::string named_handles(const RecordedCommand& c) {
        std::ostringstream out;
        bool first = true;
        const auto one = [&](const char* category, std::uint64_t value) {
            if (value == 0) {
                return;
            }
            if (!first) {
                out << ',';
            }
            first = false;
            out << category << '#' << ordinal(category, value);
        };
        one("image", c.image);
        one("render_pass", c.render_pass);
        one("framebuffer", c.framebuffer);
        one("pipeline", c.pipeline);
        for (const std::uint64_t value : c.vertex_buffers) {
            one("buffer", value);
        }
        one("layout", c.desc_layout);
        for (const std::uint64_t value : c.descriptor_sets) {
            one("descriptor_set", value);
        }
        // Imageless framebuffers supply their concrete attachment views in args_u64 at begin.
        // Keep those identities visible as image-view ordinals: a generic u64 hash cannot tell us
        // whether zink alternated attachments correctly across the first good frame and the first
        // corrupt AMD frame.
        if (c.kind == "begin_render_pass") {
            for (const std::uint64_t value : c.args_u64) {
                one("image_view", value);
            }
        }
        one("buffer", c.src_buffer);
        return out.str();
    }

    void note_truncated() {
        if (out_ && !truncated_) {
            truncated_ = true;
            out_ << "{\"event\":\"truncated\",\"seq\":" << seq_++
                 << ",\"recordings\":" << recordings_ << ",\"commands\":" << commands_
                 << ",\"upload_ranges\":" << upload_ranges_ << "}\n";
            out_.flush();
        }
    }

    std::ofstream out_;
    std::map<std::pair<std::string, std::uint64_t>, std::uint64_t> ordinals_;
    std::map<std::string, std::uint64_t> next_ordinal_;
    std::uint64_t seq_ = 0;
    std::uint64_t recordings_ = 0;
    std::uint64_t commands_ = 0;
    std::uint64_t upload_ranges_ = 0;
    bool truncated_ = false;
};

// Tolerant readers (mirror messages.cpp): a missing or wrong-typed field falls
// back to a default so a body decoder never throws on a sloppy peer.
int get_int(const json::Value& obj, const std::string& key, int fallback) {
    const json::Value* v = obj.find(key);
    return (v != nullptr && v->is_integer()) ? static_cast<int>(v->as_int()) : fallback;
}
long long get_i64(const json::Value& obj, const std::string& key, long long fallback) {
    const json::Value* v = obj.find(key);
    return (v != nullptr && v->is_integer()) ? v->as_int() : fallback;
}
// Decodes a small non-negative Vulkan index / extent / enum. Reads wide and maps a
// missing / wrong-typed / out-of-[0, INT_MAX] value to -1, so a malformed wire
// value is rejected by validation rather than truncated by a narrowing cast into a
// plausible small int. (Used wherever an int field feeds a real Vulkan call.)
int get_index(const json::Value& obj, const std::string& key) {
    const long long v = get_i64(obj, key, -1);
    return (v >= 0 && v <= INT_MAX) ? static_cast<int>(v) : -1;
}
std::string get_string(const json::Value& obj, const std::string& key) {
    const json::Value* v = obj.find(key);
    return (v != nullptr && v->is_string()) ? v->as_string() : std::string();
}
// Reads a JSON number as a double (clear-color components ride as floats). A missing /
// wrong-typed field falls back, so a decoder never throws on a sloppy peer.
double get_number(const json::Value& obj, const std::string& key, double fallback) {
    const json::Value* v = obj.find(key);
    return (v != nullptr && v->is_number()) ? v->as_number() : fallback;
}
bool get_bool(const json::Value& obj, const std::string& key) {
    const json::Value* v = obj.find(key);
    return (v != nullptr && v->is_bool()) && v->as_bool();
}

// Handles are 64-bit opaque values. JSON numbers here are double-backed and
// cannot carry the full 64-bit range exactly, so handles travel as decimal
// strings. Parsing is digit-only and total: empty / sign / whitespace /
// non-decimal / overflow all yield 0 (null), never a mangled value.
std::uint64_t parse_handle_str(const std::string& s) {
    if (s.empty()) {
        return 0;
    }
    for (const char c : s) {
        if (c < '0' || c > '9') {
            return 0;
        }
    }
    try {
        std::size_t pos = 0;
        const unsigned long long value = std::stoull(s, &pos);
        return pos == s.size() ? static_cast<std::uint64_t>(value) : 0;
    } catch (const std::exception&) {
        return 0; // overflow
    }
}
json::Value handle_value(std::uint64_t handle) {
    return json::Value(std::to_string(handle));
}
std::uint64_t get_handle(const json::Value& obj, const std::string& key) {
    const json::Value* v = obj.find(key);
    return (v != nullptr && v->is_string()) ? parse_handle_str(v->as_string()) : 0;
}
json::Value handle_array(const std::vector<std::uint64_t>& handles) {
    json::Array arr;
    for (const std::uint64_t h : handles) {
        arr.emplace_back(handle_value(h));
    }
    return json::Value(std::move(arr));
}
std::vector<std::uint64_t> get_handle_array(const json::Value& obj, const std::string& key) {
    std::vector<std::uint64_t> out;
    const json::Value* v = obj.find(key);
    if (v != nullptr && v->is_array()) {
        // Decode one entry per element, preserving array length: a malformed
        // element (non-string, or non-decimal string) becomes handle 0, exactly
        // as scalar handles do, so it fails the semantic check downstream rather
        // than being silently dropped (which could turn a malformed request into
        // an empty, "successful" no-op).
        for (const auto& e : v->as_array()) {
            out.push_back(e.is_string() ? parse_handle_str(e.as_string()) : 0);
        }
    }
    return out;
}

// Packs a queue family + index into a single cache key.
std::uint64_t queue_key(int family, int index) {
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(family)) << 32) |
           static_cast<std::uint32_t>(index);
}

// GL/zink: hex-encode raw bytes for JSON transport (capability blobs -- verbatim
// VkPhysicalDeviceProperties / pNext structs). Total + ASCII-safe; small structs, so the 2x size is
// fine. decode tolerates only well-formed even-length hex (odd/garbage -> empty, fail-closed).
std::string to_hex(const std::string& bytes) {
    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const unsigned char c : bytes) {
        out.push_back(kHex[c >> 4]);
        out.push_back(kHex[c & 0xF]);
    }
    return out;
}
int hex_nibble(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}
std::string from_hex(const std::string& hex) {
    if ((hex.size() % 2) != 0) {
        return {};
    }
    std::string out;
    out.reserve(hex.size() / 2);
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        const int hi = hex_nibble(hex[i]);
        const int lo = hex_nibble(hex[i + 1]);
        if (hi < 0 || lo < 0) {
            return {}; // malformed -> empty (fail-closed)
        }
        out.push_back(static_cast<char>((hi << 4) | lo));
    }
    return out;
}
std::string get_hex_blob(const json::Value& obj, const std::string& key) {
    const json::Value* v = obj.find(key);
    return (v != nullptr && v->is_string()) ? from_hex(v->as_string()) : std::string{};
}
// GL/zink: a JSON number field as double (sampler lodBias/anisotropy/lod). Default on a
// missing/non-number key.
double get_double(const json::Value& obj, const std::string& key, double def) {
    const json::Value* v = obj.find(key);
    return (v != nullptr && v->is_number()) ? v->as_number() : def;
}

// LE scalar/vector writers + a fail-closed reader for the binary record body
// (RecordCommandBufferRequest::to_wire/from_wire). Doubles ride bit-cast (u64), so NaN/inf/-0.0
// survive byte-exact -- STRICTER than the JSON path. The reader never reads past the body: any
// short field trips `fail` and every later read returns the type's zero value.
void wire_put_u64(std::string& out, std::uint64_t v) {
    unsigned char b[8];
    for (int i = 0; i < 8; ++i) {
        b[i] = static_cast<unsigned char>(v >> (8 * i));
    }
    out.append(reinterpret_cast<const char*>(b), 8);
}
void wire_put_i64(std::string& out, long long v) {
    wire_put_u64(out, static_cast<std::uint64_t>(v));
}
void wire_put_f64(std::string& out, double v) {
    std::uint64_t bits = 0;
    std::memcpy(&bits, &v, 8);
    wire_put_u64(out, bits);
}
void wire_put_str(std::string& out, const std::string& s) {
    wire_put_u64(out, s.size());
    out += s;
}
void wire_put_vec_u64(std::string& out, const std::vector<std::uint64_t>& v) {
    wire_put_u64(out, v.size());
    for (const std::uint64_t x : v) {
        wire_put_u64(out, x);
    }
}

struct WireReader {
    const std::string& body;
    std::size_t pos = 0;
    bool fail = false;

    std::size_t remaining() const { return body.size() - pos; }
    std::uint64_t get_u64() {
        if (fail || remaining() < 8) {
            fail = true;
            return 0;
        }
        std::uint64_t v = 0;
        for (int i = 0; i < 8; ++i) {
            v |= static_cast<std::uint64_t>(static_cast<unsigned char>(body[pos + i])) << (8 * i);
        }
        pos += 8;
        return v;
    }
    long long get_i64() { return static_cast<long long>(get_u64()); }
    double get_f64() {
        const std::uint64_t bits = get_u64();
        double v = 0.0;
        std::memcpy(&v, &bits, 8);
        return fail ? 0.0 : v;
    }
    // Length-prefixed bytes; the declared length must fit the remaining body (alloc-bomb guard:
    // nothing is reserved beyond what the wire actually carries).
    std::string get_str() {
        const std::uint64_t n = get_u64();
        if (fail || n > remaining()) {
            fail = true;
            return {};
        }
        std::string s = body.substr(pos, static_cast<std::size_t>(n));
        pos += static_cast<std::size_t>(n);
        return s;
    }
    std::vector<std::uint64_t> get_vec_u64() {
        const std::uint64_t n = get_u64();
        if (fail || n > remaining() / 8) {
            fail = true;
            return {};
        }
        std::vector<std::uint64_t> v;
        v.reserve(static_cast<std::size_t>(n));
        for (std::uint64_t i = 0; i < n; ++i) {
            v.push_back(get_u64());
        }
        return v;
    }
};

// The typed VK_KHR_synchronization2 DependencyInfo2 sparse-wire codec (one nested
// group). Masks ride u64 (unsplit 64-bit), queue-family indices + layouts + ranges ride i64 (wide),
// handles ride u64. Decode is fail-closed: every nested count is bounded by BOTH the semantic cap
// and the remaining-body structural floor, so a malformed stream is rejected, never
// over-allocated. Per-barrier minimum wire bytes: memory 4*8=32, buffer 9*8=72, image 14*8=112.
void wire_put_dep2(std::string& out, const DependencyInfo2& d) {
    wire_put_u64(out, d.dependency_flags);
    wire_put_u64(out, d.memory.size());
    for (const MemoryBarrier2& m : d.memory) {
        wire_put_u64(out, m.src_stage);
        wire_put_u64(out, m.src_access);
        wire_put_u64(out, m.dst_stage);
        wire_put_u64(out, m.dst_access);
    }
    wire_put_u64(out, d.buffer.size());
    for (const BufferMemoryBarrier2& b : d.buffer) {
        wire_put_u64(out, b.src_stage);
        wire_put_u64(out, b.src_access);
        wire_put_u64(out, b.dst_stage);
        wire_put_u64(out, b.dst_access);
        wire_put_i64(out, b.src_queue_family);
        wire_put_i64(out, b.dst_queue_family);
        wire_put_u64(out, b.buffer);
        wire_put_u64(out, b.offset);
        wire_put_u64(out, b.size);
    }
    wire_put_u64(out, d.image.size());
    for (const ImageMemoryBarrier2& im : d.image) {
        wire_put_u64(out, im.src_stage);
        wire_put_u64(out, im.src_access);
        wire_put_u64(out, im.dst_stage);
        wire_put_u64(out, im.dst_access);
        wire_put_i64(out, im.old_layout);
        wire_put_i64(out, im.new_layout);
        wire_put_i64(out, im.src_queue_family);
        wire_put_i64(out, im.dst_queue_family);
        wire_put_u64(out, im.image);
        wire_put_i64(out, im.aspect);
        wire_put_i64(out, im.base_mip);
        wire_put_i64(out, im.level_count);
        wire_put_i64(out, im.base_layer);
        wire_put_i64(out, im.layer_count);
    }
}

DependencyInfo2 wire_get_dep2(WireReader& rd) {
    DependencyInfo2 d;
    d.dependency_flags = rd.get_u64();
    const std::uint64_t mem_n = rd.get_u64();
    if (rd.fail || mem_n > kMaxSync2BarriersPerDep || mem_n > rd.remaining() / 32) {
        rd.fail = true;
        return d;
    }
    d.memory.reserve(static_cast<std::size_t>(mem_n));
    for (std::uint64_t i = 0; i < mem_n; ++i) {
        MemoryBarrier2 m;
        m.src_stage = rd.get_u64();
        m.src_access = rd.get_u64();
        m.dst_stage = rd.get_u64();
        m.dst_access = rd.get_u64();
        d.memory.push_back(m);
    }
    const std::uint64_t buf_n = rd.get_u64();
    if (rd.fail || buf_n > kMaxSync2BarriersPerDep || buf_n > rd.remaining() / 72) {
        rd.fail = true;
        return d;
    }
    d.buffer.reserve(static_cast<std::size_t>(buf_n));
    for (std::uint64_t i = 0; i < buf_n; ++i) {
        BufferMemoryBarrier2 b;
        b.src_stage = rd.get_u64();
        b.src_access = rd.get_u64();
        b.dst_stage = rd.get_u64();
        b.dst_access = rd.get_u64();
        b.src_queue_family = rd.get_i64();
        b.dst_queue_family = rd.get_i64();
        b.buffer = rd.get_u64();
        b.offset = rd.get_u64();
        b.size = rd.get_u64();
        d.buffer.push_back(b);
    }
    const std::uint64_t img_n = rd.get_u64();
    if (rd.fail || img_n > kMaxSync2BarriersPerDep || img_n > rd.remaining() / 112) {
        rd.fail = true;
        return d;
    }
    d.image.reserve(static_cast<std::size_t>(img_n));
    for (std::uint64_t i = 0; i < img_n; ++i) {
        ImageMemoryBarrier2 im;
        im.src_stage = rd.get_u64();
        im.src_access = rd.get_u64();
        im.dst_stage = rd.get_u64();
        im.dst_access = rd.get_u64();
        im.old_layout = static_cast<int>(rd.get_i64());
        im.new_layout = static_cast<int>(rd.get_i64());
        im.src_queue_family = rd.get_i64();
        im.dst_queue_family = rd.get_i64();
        im.image = rd.get_u64();
        im.aspect = rd.get_i64();
        im.base_mip = rd.get_i64();
        im.level_count = rd.get_i64();
        im.base_layer = rd.get_i64();
        im.layer_count = rd.get_i64();
        d.image.push_back(im);
    }
    return d;
}

// Vulkan enum/flag values the mock validates the bounded create-info subset against. The
// mock never links Vulkan, so the numeric values are spelled out here; they match <vulkan_core.h>
// (the real backend uses the real VK_* enums, so the two stay in lockstep).
namespace vk3b {
constexpr int kImageViewType2D = 1;                   // VK_IMAGE_VIEW_TYPE_2D
constexpr int kComponentSwizzleIdentity = 0;          // VK_COMPONENT_SWIZZLE_IDENTITY
constexpr int kImageAspectColorBit = 1;               // VK_IMAGE_ASPECT_COLOR_BIT
constexpr int kAttachmentLoadOpClear = 1;             // VK_ATTACHMENT_LOAD_OP_CLEAR
constexpr int kAttachmentStoreOpStore = 0;            // VK_ATTACHMENT_STORE_OP_STORE
constexpr int kAttachmentLoadOpDontCare = 2;          // VK_ATTACHMENT_LOAD_OP_DONT_CARE
constexpr int kAttachmentStoreOpDontCare = 1;         // VK_ATTACHMENT_STORE_OP_DONT_CARE
constexpr int kImageLayoutUndefined = 0;              // VK_IMAGE_LAYOUT_UNDEFINED
constexpr int kImageLayoutColorAttachmentOptimal = 2; // VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
constexpr int kImageLayoutPresentSrcKhr = 1000001002; // VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
constexpr long long kSubpassExternal = 0xFFFFFFFFLL;  // VK_SUBPASS_EXTERNAL
constexpr int kSampleCount1 = 1;                      // VK_SAMPLE_COUNT_1_BIT
constexpr int kShaderStageVertex = 1;                 // VK_SHADER_STAGE_VERTEX_BIT
constexpr int kShaderStageFragment = 16;              // VK_SHADER_STAGE_FRAGMENT_BIT
constexpr int kPrimitiveTopologyTriangleList = 3;     // VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST
constexpr int kDynamicStateViewport = 0;              // VK_DYNAMIC_STATE_VIEWPORT
constexpr int kDynamicStateScissor = 1;               // VK_DYNAMIC_STATE_SCISSOR
// The cull-mode subset is exactly {NONE = 0, FRONT = 1, BACK = 2}; FRONT_AND_BACK (3) is out.
constexpr int kCullModeMaxSubset = 2;     // VK_CULL_MODE_BACK_BIT (the largest value in the subset)
constexpr int kFrontFaceClockwise = 1;    // VK_FRONT_FACE_CLOCKWISE (0 = CCW, 1 = CW)
constexpr int kDependencyByRegionBit = 1; // VK_DEPENDENCY_BY_REGION_BIT
// Query pools (GL 3.3 / occlusion queries): VkQueryType values (mock has no Vulkan headers).
// TRANSFORM_FEEDBACK_STREAM is intentionally NOT admitted (its indexed begin/end are unwired).
constexpr int kQueryTypeOcclusion = 0;          // VK_QUERY_TYPE_OCCLUSION
constexpr int kQueryTypePipelineStatistics = 1; // VK_QUERY_TYPE_PIPELINE_STATISTICS
constexpr int kQueryTypeTimestamp = 2;          // VK_QUERY_TYPE_TIMESTAMP
constexpr int kVkSuccess = 0;                   // VK_SUCCESS
} // namespace vk3b

// Vulkan enum values the vertex-input subset validates against (mock has no Vulkan
// headers); they match <vulkan_core.h>.
namespace vk3c1 {
constexpr int kVertexInputRateVertex = 0;      // VK_VERTEX_INPUT_RATE_VERTEX
constexpr int kSharingModeExclusive = 0;       // VK_SHARING_MODE_EXCLUSIVE
constexpr int kFormatR8G8B8A8Unorm = 37;       // VK_FORMAT_R8G8B8A8_UNORM
constexpr int kFormatR32G32Sfloat = 103;       // VK_FORMAT_R32G32_SFLOAT
constexpr int kFormatR32G32B32Sfloat = 106;    // VK_FORMAT_R32G32B32_SFLOAT
constexpr int kFormatR32G32B32A32Sfloat = 109; // VK_FORMAT_R32G32B32A32_SFLOAT
// The vertex-attribute format subset (float2/3/4 + RGBA8) -- enough for position + color/uv.
inline bool is_vertex_format(int f) {
    return f == kFormatR8G8B8A8Unorm || f == kFormatR32G32Sfloat || f == kFormatR32G32B32Sfloat ||
           f == kFormatR32G32B32A32Sfloat;
}
} // namespace vk3c1

// Vulkan enum values the image/depth subset validates against (mock has no Vulkan
// headers); they match <vulkan_core.h>.
namespace vk3c3 {
constexpr int kFormatUndefined = 0;                          // VK_FORMAT_UNDEFINED
constexpr int kAttachmentLoadOpClear = 1;                    // VK_ATTACHMENT_LOAD_OP_CLEAR
constexpr int kAttachmentStoreOpDontCare = 1;                // VK_ATTACHMENT_STORE_OP_DONT_CARE
constexpr int kImageLayoutDepthStencilAttachmentOptimal = 3; // VK_IMAGE_LAYOUT_*_OPTIMAL
// A sampled-color image's aspect is COLOR; a depth image's is DEPTH. A texture/staging image uses
// OPTIMAL or LINEAR tiling; a depth image is OPTIMAL.
inline bool is_depth_format(int f) {
    return f == kFormatD16Unorm;
}
} // namespace vk3c3

// True if (a_major, a_minor) < (b_major, b_minor).
bool version_less(int a_major, int a_minor, int b_major, int b_minor) {
    return a_major < b_major || (a_major == b_major && a_minor < b_minor);
}

// the per-PROCESS profile.
// nullptr unless VKRELAY2_PROFILE=1 -- the hooks' only cost when off is this null check. One
// instance covers whichever END this process is (the ICD process only ever runs the client hooks,
// the worker process only the serve hook). Access is single-threaded by construction (the ICD
// data plane is serialized under the ICD's g_mu; the worker data plane is one thread) -- no locks.
RpcProfile* profile_instance() {
    static const bool enabled = [] {
        const char* v = std::getenv("VKRELAY2_PROFILE");
        return v != nullptr && v[0] == '1';
    }();
    if (!enabled) {
        return nullptr;
    }
    static RpcProfile profile = [] {
        RpcProfile p;
        const char* f = std::getenv("VKRELAY2_PROFILE_FRAMES");
        p.ring_enabled = f != nullptr && f[0] == '1';
        return p;
    }();
    return &profile;
}

std::uint64_t profile_now_us() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

// One-shot guard shared by ALL dump triggers (teardown, atexit, and the POSIX signal path
// below): volatile sig_atomic_t because the signal handler reads and writes it.
volatile std::sig_atomic_t g_profile_dumped = 0;

// One-shot dump (teardown only, never hot-path): stderr by default (the launcher's app.log tee /
// the daemon's worker-log redirect pick it up), or appended to a VKRELAY2_PROFILE_OUT-derived
// <path>.<end>.<pid> (per-end/per-pid, so client and worker never clobber one file). Triggered
// from the DestroyInstance client stub, the serve-loop end (worker), an atexit backstop, and --
// client end, POSIX only -- a fatal-signal handler.
void profile_dump_once() {
    RpcProfile* p = profile_instance();
    if (p == nullptr) {
        return;
    }
    if (g_profile_dumped != 0) {
        return;
    }
    g_profile_dumped = 1;
    std::FILE* out = stderr;
    if (const char* base = std::getenv("VKRELAY2_PROFILE_OUT")) {
#ifdef _WIN32
        const int pid = _getpid();
#else
        const int pid = static_cast<int>(getpid());
#endif
        const std::string path = std::string(base) + "." +
                                 (p->used_as_worker ? "worker" : "client") + "." +
                                 std::to_string(pid);
        if (std::FILE* f = std::fopen(path.c_str(), "a")) {
            out = f;
        }
    }
    p->dump([&](const std::string& line) { std::fprintf(out, "%s\n", line.c_str()); });
    if (out != stderr) {
        std::fclose(out);
    } else {
        std::fflush(stderr);
    }
}

void profile_register_atexit() {
    static bool registered = false;
    if (!registered) {
        registered = true;
        std::atexit(profile_dump_once);
    }
}

#ifndef _WIN32
// timeout(1) signals its command's whole PROCESS GROUP, so a profiled app dies
// on SIGTERM with atexit never running -- exactly how 6.2 lost the client tables for glxgears
// and the OpenSCAD GUI. When profiling is ON (and ONLY then -- off must not change signal
// dispositions), the first client hook installs a one-shot handler for the default-terminate
// signals a run ends with (TERM / INT / HUP). The handler emits the dump via the
// async-signal-safe format_dump + write(2) -- no allocation, stdio, or locks -- then restores
// the saved disposition and re-raises, so the exit status is exactly what the app would have
// had. A signal the app itself handles or ignores is left alone (installation checks SIG_DFL),
// and an app that installs its own handler LATER simply wins -- both faithful.
constexpr int kProfileSignals[] = {SIGTERM, SIGINT, SIGHUP};
RpcProfile* g_profile_sig_profile = nullptr; // raw pointer: no static-init in the handler
struct sigaction g_profile_sig_prev[3];
char g_profile_sig_path[512];      // resolved at install (getenv is not handler-safe); "" = stderr
char g_profile_sig_buf[96 * 1024]; // worst-case dump ~55 KB (129 op lines + frames + ring)

void profile_signal_dump(int sig) {
    if (g_profile_dumped == 0 && g_profile_sig_profile != nullptr) {
        g_profile_dumped = 1;
        const std::size_t n =
            g_profile_sig_profile->format_dump(g_profile_sig_buf, sizeof(g_profile_sig_buf));
        int fd = 2;
        if (g_profile_sig_path[0] != '\0') {
            const int f = ::open(g_profile_sig_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
            if (f >= 0) {
                fd = f;
            }
        }
        std::size_t off = 0;
        while (off < n) {
            const ssize_t w = ::write(fd, g_profile_sig_buf + off, n - off);
            if (w <= 0) {
                break;
            }
            off += static_cast<std::size_t>(w);
        }
        if (fd != 2) {
            ::close(fd);
        }
    }
    for (std::size_t i = 0; i < 3; ++i) {
        if (kProfileSignals[i] == sig) {
            ::sigaction(sig, &g_profile_sig_prev[i], nullptr);
        }
    }
    ::raise(sig);
}

void profile_register_signal_dump(RpcProfile* prof) {
    static bool registered = false;
    if (registered) {
        return;
    }
    registered = true;
    g_profile_sig_profile = prof;
    if (const char* base = std::getenv("VKRELAY2_PROFILE_OUT")) {
        // Mirror profile_dump_once's <path>.client.<pid> so either path lands in the same file.
        std::snprintf(g_profile_sig_path, sizeof(g_profile_sig_path), "%s.client.%d", base,
                      static_cast<int>(getpid()));
    }
    for (std::size_t i = 0; i < 3; ++i) {
        struct sigaction cur{};
        if (::sigaction(kProfileSignals[i], nullptr, &cur) != 0 || cur.sa_handler != SIG_DFL) {
            continue; // the app owns this signal (handler or SIG_IGN) -- do not change behavior
        }
        g_profile_sig_prev[i] = cur;
        struct sigaction sa{};
        sa.sa_handler = profile_signal_dump;
        sigfillset(&sa.sa_mask); // no nested dumps mid-format
        sa.sa_flags = 0;
        ::sigaction(kProfileSignals[i], &sa, nullptr);
    }
}
#else
void profile_register_signal_dump(RpcProfile*) {} // client end is POSIX; worker dumps at serve-end
#endif

// Client-side request/response round-trip: send, await the correlated reply, and
// return its parsed JSON body. Throws on EOF, a correlation mismatch, or an RPC
// status that is not Ok (a body-level !ok is the caller's to interpret).
json::Value call_rpc(RpcChannel& channel, RpcOp op, std::uint32_t request_id,
                     const json::Value& body) {
    // client hook: the window covers the request marshal (the dump(0) below) + wire +
    // worker + response parse. A throwing call skips the record (errors are not steady state).
    // QueuePresent counts INTO the frame it closes, then marks the next (locked frame semantics).
    RpcProfile* prof = profile_instance();
    std::uint64_t t0 = 0;
    if (prof != nullptr) {
        profile_register_atexit();
        profile_register_signal_dump(prof); // client end only
        t0 = profile_now_us();
    }
    RpcMessage out;
    out.op = static_cast<std::uint32_t>(op);
    out.request_id = request_id;
    out.body = body.dump(0);
    channel.send(out);

    RpcMessage in;
    if (!channel.recv(in)) {
        throw transport::TransportError("rpc closed before response");
    }
    if (in.request_id != request_id) {
        throw transport::TransportError("rpc response correlation id mismatch");
    }
    if (in.status != static_cast<std::uint32_t>(RpcStatus::Ok)) {
        throw transport::TransportError(std::string("rpc error: ") +
                                        to_string(static_cast<RpcStatus>(in.status)));
    }
    json::Value parsed;
    std::string err;
    if (!json::Value::try_parse(in.body, parsed, err)) {
        throw transport::TransportError("malformed rpc response body: " + err);
    }
    if (prof != nullptr) {
        const std::uint64_t t1 = profile_now_us();
        prof->record(out.op, out.body.size(), in.body.size(), t1 - t0);
        if (op == RpcOp::QueuePresent) {
            prof->frame_mark(t1);
        }
    }
    return parsed;
}

// Variant for the binary-bodied ops (create_shader_module): the request body is raw bytes
// ([u32 json_len][json header][raw SPIR-V]) rather than a JSON document. The response is still
// JSON.
json::Value call_rpc_raw(RpcChannel& channel, RpcOp op, std::uint32_t request_id,
                         const std::string& raw_body) {
    RpcProfile* prof = profile_instance();
    std::uint64_t t0 = 0;
    if (prof != nullptr) {
        profile_register_atexit();
        profile_register_signal_dump(prof); // client end only
        t0 = profile_now_us();
    }
    RpcMessage out;
    out.op = static_cast<std::uint32_t>(op);
    out.request_id = request_id;
    out.body = raw_body;
    channel.send(out);

    RpcMessage in;
    if (!channel.recv(in)) {
        throw transport::TransportError("rpc closed before response");
    }
    if (in.request_id != request_id) {
        throw transport::TransportError("rpc response correlation id mismatch");
    }
    if (in.status != static_cast<std::uint32_t>(RpcStatus::Ok)) {
        throw transport::TransportError(std::string("rpc error: ") +
                                        to_string(static_cast<RpcStatus>(in.status)));
    }
    json::Value parsed;
    std::string err;
    if (!json::Value::try_parse(in.body, parsed, err)) {
        throw transport::TransportError("malformed rpc response body: " + err);
    }
    if (prof != nullptr) {
        prof->record(out.op, out.body.size(), in.body.size(), profile_now_us() - t0);
    }
    return parsed;
}

// Variant for ops whose RESPONSE body is raw bytes (the raw readback reply):
// the JSON request goes out normally, the correlated reply comes back as an opaque string the
// caller decodes with the op's from_wire. Same envelope, correlation, and status handling as
// call_rpc -- only the body parse is the caller's.
std::string call_rpc_response_raw(RpcChannel& channel, RpcOp op, std::uint32_t request_id,
                                  const json::Value& body) {
    RpcProfile* prof = profile_instance();
    std::uint64_t t0 = 0;
    if (prof != nullptr) {
        profile_register_atexit();
        profile_register_signal_dump(prof); // client end only
        t0 = profile_now_us();
    }
    RpcMessage out;
    out.op = static_cast<std::uint32_t>(op);
    out.request_id = request_id;
    out.body = body.dump(0);
    channel.send(out);

    RpcMessage in;
    if (!channel.recv(in)) {
        throw transport::TransportError("rpc closed before response");
    }
    if (in.request_id != request_id) {
        throw transport::TransportError("rpc response correlation id mismatch");
    }
    if (in.status != static_cast<std::uint32_t>(RpcStatus::Ok)) {
        throw transport::TransportError(std::string("rpc error: ") +
                                        to_string(static_cast<RpcStatus>(in.status)));
    }
    if (prof != nullptr) {
        prof->record(out.op, out.body.size(), in.body.size(), profile_now_us() - t0);
    }
    return std::move(in.body);
}

} // namespace

// the backends time their record-handler phases into the same per-process
// profile the RPC hooks use (see vulkan_session.hpp).
RpcProfile* profile_if_enabled() {
    return profile_instance();
}
std::uint64_t profile_clock_us() {
    return profile_now_us();
}

json::Value CapabilitiesRequest::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("requested_api_major", json::Value(requested_api_major));
    b.set("requested_api_minor", json::Value(requested_api_minor));
    return b;
}

CapabilitiesRequest CapabilitiesRequest::from_body(const json::Value& body) {
    CapabilitiesRequest r;
    r.requested_api_major = get_int(body, "requested_api_major", 1);
    r.requested_api_minor = get_int(body, "requested_api_minor", 0);
    return r;
}

json::Value DeviceCaps::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("device_name", json::Value(device_name));
    b.set("vendor_id", json::Value(static_cast<long long>(vendor_id)));
    b.set("device_id", json::Value(static_cast<long long>(device_id)));
    b.set("device_type", json::Value(device_type));
    b.set("timestamp_valid_bits", json::Value(static_cast<long long>(timestamp_valid_bits)));
    b.set("max_color_attachments", json::Value(static_cast<long long>(max_color_attachments)));
    b.set("raw_readback", json::Value(raw_readback)); //   (additive)
    b.set("raw_record", json::Value(raw_record));     //   (additive)
    // Compute (additive): u64 rides wide as a decimal string (the VkFlags pattern).
    b.set("queue_flags", handle_value(queue_flags));
    b.set("vk13_ready",
          json::Value(static_cast<long long>(vk13_ready))); // Vulkan 1.3 support (additive)
    // geometry-stream (additive): worker validates + replays the stream pipeline pNext.
    b.set("rasterization_stream_state",
          json::Value(static_cast<long long>(rasterization_stream_state)));
    b.set("core_indirect_draw", json::Value(static_cast<long long>(core_indirect_draw)));
    return b;
}

DeviceCaps DeviceCaps::from_body(const json::Value& body) {
    DeviceCaps d;
    d.device_name = get_string(body, "device_name");
    d.vendor_id = static_cast<std::uint32_t>(get_i64(body, "vendor_id", 0));
    d.device_id = static_cast<std::uint32_t>(get_i64(body, "device_id", 0));
    d.device_type = get_string(body, "device_type");
    d.timestamp_valid_bits =
        static_cast<std::uint32_t>(get_i64(body, "timestamp_valid_bits", 0)); // absent -> 0
    d.max_color_attachments =
        static_cast<std::uint32_t>(get_i64(body, "max_color_attachments", 0)); // absent -> 0
    d.raw_readback = get_bool(body, "raw_readback"); // absent -> false (old worker)
    d.raw_record = get_bool(body, "raw_record");     // absent -> false (old worker)
    d.queue_flags = get_handle(body, "queue_flags"); // wide u64; absent -> 0 (old worker)
    d.vk13_ready = static_cast<std::uint32_t>(get_i64(body, "vk13_ready", 0)); // absent -> 0
    d.rasterization_stream_state = static_cast<std::uint32_t>(
        get_i64(body, "rasterization_stream_state", 0)); // absent -> 0 (old worker)
    d.core_indirect_draw =
        static_cast<std::uint32_t>(get_i64(body, "core_indirect_draw", 0)); // absent -> 0
    return d;
}

json::Value CapabilitiesResponse::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("ok", json::Value(ok));
    b.set("reason", json::Value(reason));
    b.set("negotiated_api_major", json::Value(negotiated_api_major));
    b.set("negotiated_api_minor", json::Value(negotiated_api_minor));
    b.set("device", device.to_body());
    return b;
}

CapabilitiesResponse CapabilitiesResponse::from_body(const json::Value& body) {
    CapabilitiesResponse r;
    r.ok = get_bool(body, "ok");
    r.reason = get_string(body, "reason");
    r.negotiated_api_major = get_int(body, "negotiated_api_major", 0);
    r.negotiated_api_minor = get_int(body, "negotiated_api_minor", 0);
    const json::Value* dev = body.find("device");
    if (dev != nullptr) {
        r.device = DeviceCaps::from_body(*dev);
    }
    return r;
}

json::Value CreateInstanceRequest::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("application_name", json::Value(application_name));
    return b;
}

CreateInstanceRequest CreateInstanceRequest::from_body(const json::Value& body) {
    CreateInstanceRequest r;
    r.application_name = get_string(body, "application_name");
    return r;
}

json::Value CreateInstanceResponse::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("ok", json::Value(ok));
    b.set("reason", json::Value(reason));
    b.set("instance", handle_value(instance));
    return b;
}

CreateInstanceResponse CreateInstanceResponse::from_body(const json::Value& body) {
    CreateInstanceResponse r;
    r.ok = get_bool(body, "ok");
    r.reason = get_string(body, "reason");
    r.instance = get_handle(body, "instance");
    return r;
}

json::Value EnumeratePhysicalDevicesRequest::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("instance", handle_value(instance));
    return b;
}

EnumeratePhysicalDevicesRequest
EnumeratePhysicalDevicesRequest::from_body(const json::Value& body) {
    EnumeratePhysicalDevicesRequest r;
    r.instance = get_handle(body, "instance");
    return r;
}

json::Value PhysicalDeviceEntry::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("handle", handle_value(handle));
    b.set("caps", caps.to_body());
    json::Array exts; // host device extensions (absent/empty -> policy-only fallback)
    for (const auto& e : device_extensions) {
        exts.emplace_back(json::Value(e));
    }
    b.set("device_extensions", json::Value(std::move(exts)));
    return b;
}

PhysicalDeviceEntry PhysicalDeviceEntry::from_body(const json::Value& body) {
    PhysicalDeviceEntry e;
    e.handle = get_handle(body, "handle");
    const json::Value* c = body.find("caps");
    if (c != nullptr) {
        e.caps = DeviceCaps::from_body(*c);
    }
    const json::Value* exts = body.find("device_extensions");
    if (exts != nullptr && exts->is_array()) {
        for (const auto& x : exts->as_array()) {
            if (x.is_string()) {
                e.device_extensions.push_back(x.as_string());
            }
        }
    }
    return e;
}

json::Value EnumeratePhysicalDevicesResponse::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("ok", json::Value(ok));
    b.set("reason", json::Value(reason));
    json::Array arr;
    for (const auto& d : devices) {
        arr.emplace_back(d.to_body());
    }
    b.set("devices", json::Value(std::move(arr)));
    return b;
}

EnumeratePhysicalDevicesResponse
EnumeratePhysicalDevicesResponse::from_body(const json::Value& body) {
    EnumeratePhysicalDevicesResponse r;
    r.ok = get_bool(body, "ok");
    r.reason = get_string(body, "reason");
    const json::Value* arr = body.find("devices");
    if (arr != nullptr && arr->is_array()) {
        for (const auto& entry : arr->as_array()) {
            r.devices.push_back(PhysicalDeviceEntry::from_body(entry));
        }
    }
    return r;
}

json::Value CreateDeviceRequest::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("instance", handle_value(instance));
    b.set("physical_device", handle_value(physical_device));
    // GL/zink: the app's enabled device extensions (already host-intersected by the ICD)
    // + requested features (u64 bitmask), so the worker enables them on the real device. Additive.
    json::Array exts;
    for (const auto& e : enabled_extensions) {
        exts.emplace_back(json::Value(e));
    }
    b.set("enabled_extensions", json::Value(std::move(exts)));
    b.set("enabled_feature_bits", handle_value(enabled_feature_bits));
    b.set("dynamic_rendering_feature_enabled", json::Value(dynamic_rendering_feature_enabled));
    b.set("synchronization2_feature_enabled", json::Value(synchronization2_feature_enabled));
    b.set("host_query_reset_feature_enabled", json::Value(host_query_reset_feature_enabled));
    b.set("multiview_feature_enabled", json::Value(multiview_feature_enabled));
    b.set("buffer_device_address_feature_enabled",
          json::Value(buffer_device_address_feature_enabled));
    b.set("vertex_attr_divisor_feature_enabled", json::Value(vertex_attr_divisor_feature_enabled));
    b.set("vertex_attr_zero_divisor_feature_enabled",
          json::Value(vertex_attr_zero_divisor_feature_enabled));
    b.set("geometry_streams_feature_enabled", json::Value(geometry_streams_feature_enabled));
    b.set("descriptor_indexing_feature_bits", handle_value(descriptor_indexing_feature_bits));
    b.set("vk13_feature_bits", handle_value(vk13_feature_bits));
    b.set("vk13_device_enabled", json::Value(vk13_device_enabled));
    json::Array fc;
    for (const auto& e : enabled_feature_chain) {
        fc.emplace_back(e.to_body());
    }
    b.set("enabled_feature_chain", json::Value(std::move(fc)));
    return b;
}

CreateDeviceRequest CreateDeviceRequest::from_body(const json::Value& body) {
    CreateDeviceRequest r;
    r.instance = get_handle(body, "instance");
    r.physical_device = get_handle(body, "physical_device");
    const json::Value* exts = body.find("enabled_extensions");
    if (exts != nullptr && exts->is_array()) {
        for (const auto& e : exts->as_array()) {
            if (e.is_string()) {
                r.enabled_extensions.push_back(e.as_string());
            }
        }
    }
    r.enabled_feature_bits = get_handle(body, "enabled_feature_bits");
    r.dynamic_rendering_feature_enabled = get_int(body, "dynamic_rendering_feature_enabled", 0);
    r.synchronization2_feature_enabled = get_int(body, "synchronization2_feature_enabled", 0);
    r.host_query_reset_feature_enabled = get_int(body, "host_query_reset_feature_enabled", 0);
    r.multiview_feature_enabled = get_int(body, "multiview_feature_enabled", 0);
    r.buffer_device_address_feature_enabled =
        get_int(body, "buffer_device_address_feature_enabled", 0);
    r.vertex_attr_divisor_feature_enabled = get_int(body, "vertex_attr_divisor_feature_enabled", 0);
    r.vertex_attr_zero_divisor_feature_enabled =
        get_int(body, "vertex_attr_zero_divisor_feature_enabled", 0);
    // geometry-stream: the OMITTED state is a PRESENCE fact -- only
    // an ABSENT key (an older ICD, which already forwards the TF feature chain but has no
    // scalar) decodes it; the worker then derives the enabled state from the chain instead of
    // mismatch-rejecting the old payload. A PRESENT value must be exactly 0 or 1; anything else
    // (a forged -1 claiming legacy status, a wrong type, out-of-range) decodes INVALID and
    // create_device rejects it by name -- a hostile client cannot transmit its way past the
    // scalar/chain agreement check.
    if (body.find("geometry_streams_feature_enabled") == nullptr) {
        r.geometry_streams_feature_enabled = kGeometryStreamsScalarOmitted;
    } else {
        const int gs_v = get_int(body, "geometry_streams_feature_enabled",
                                 kGeometryStreamsScalarInvalid); // wrong type -> invalid
        r.geometry_streams_feature_enabled =
            (gs_v == 0 || gs_v == 1) ? gs_v : kGeometryStreamsScalarInvalid;
    }
    r.descriptor_indexing_feature_bits = get_handle(body, "descriptor_indexing_feature_bits");
    r.vk13_feature_bits = get_handle(body, "vk13_feature_bits");
    r.vk13_device_enabled = get_int(body, "vk13_device_enabled", 0);
    const json::Value* fc = body.find("enabled_feature_chain");
    if (fc != nullptr && fc->is_array()) {
        for (const auto& e : fc->as_array()) {
            r.enabled_feature_chain.push_back(CapabilityChainEntry::from_body(e));
        }
    }
    return r;
}

json::Value CreateDeviceResponse::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("ok", json::Value(ok));
    b.set("reason", json::Value(reason));
    b.set("device", handle_value(device));
    b.set("queue_family_index", json::Value(queue_family_index));
    b.set("queue_count", json::Value(queue_count));
    return b;
}

CreateDeviceResponse CreateDeviceResponse::from_body(const json::Value& body) {
    CreateDeviceResponse r;
    r.ok = get_bool(body, "ok");
    r.reason = get_string(body, "reason");
    r.device = get_handle(body, "device");
    r.queue_family_index = get_int(body, "queue_family_index", 0);
    r.queue_count = get_int(body, "queue_count", 0);
    return r;
}

json::Value HandleRequest::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("handle", handle_value(handle));
    return b;
}

HandleRequest HandleRequest::from_body(const json::Value& body) {
    HandleRequest r;
    r.handle = get_handle(body, "handle");
    return r;
}

json::Value StatusResponse::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("ok", json::Value(ok));
    b.set("reason", json::Value(reason));
    return b;
}

StatusResponse StatusResponse::from_body(const json::Value& body) {
    StatusResponse r;
    r.ok = get_bool(body, "ok");
    r.reason = get_string(body, "reason");
    return r;
}

json::Value GetDeviceQueueRequest::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("device", handle_value(device));
    b.set("queue_family_index", json::Value(queue_family_index));
    b.set("queue_index", json::Value(queue_index));
    return b;
}

GetDeviceQueueRequest GetDeviceQueueRequest::from_body(const json::Value& body) {
    GetDeviceQueueRequest r;
    r.device = get_handle(body, "device");
    // Wide decode + -1 sentinel (see get_index): a missing / wrong-typed /
    // out-of-range family or index must be rejected by the backend's queue
    // validation, never truncated into a value that looks like a created
    // (family, index). 0 is valid only when explicitly sent.
    r.queue_family_index = get_index(body, "queue_family_index");
    r.queue_index = get_index(body, "queue_index");
    return r;
}

json::Value GetDeviceQueueResponse::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("ok", json::Value(ok));
    b.set("reason", json::Value(reason));
    b.set("queue", handle_value(queue));
    return b;
}

GetDeviceQueueResponse GetDeviceQueueResponse::from_body(const json::Value& body) {
    GetDeviceQueueResponse r;
    r.ok = get_bool(body, "ok");
    r.reason = get_string(body, "reason");
    r.queue = get_handle(body, "queue");
    return r;
}

json::Value CreateCommandPoolRequest::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("device", handle_value(device));
    b.set("queue_family_index", json::Value(queue_family_index));
    return b;
}

CreateCommandPoolRequest CreateCommandPoolRequest::from_body(const json::Value& body) {
    CreateCommandPoolRequest r;
    r.device = get_handle(body, "device");
    // Wide decode + -1 sentinel (see get_index): the backend's family check rejects
    // a missing / out-of-range value rather than a truncation matching a real family.
    r.queue_family_index = get_index(body, "queue_family_index");
    return r;
}

json::Value CreateCommandPoolResponse::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("ok", json::Value(ok));
    b.set("reason", json::Value(reason));
    b.set("command_pool", handle_value(command_pool));
    return b;
}

CreateCommandPoolResponse CreateCommandPoolResponse::from_body(const json::Value& body) {
    CreateCommandPoolResponse r;
    r.ok = get_bool(body, "ok");
    r.reason = get_string(body, "reason");
    r.command_pool = get_handle(body, "command_pool");
    return r;
}

json::Value AllocateCommandBuffersRequest::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("command_pool", handle_value(command_pool));
    b.set("count", json::Value(count));
    return b;
}

AllocateCommandBuffersRequest AllocateCommandBuffersRequest::from_body(const json::Value& body) {
    AllocateCommandBuffersRequest r;
    r.command_pool = get_handle(body, "command_pool");
    r.count = get_i64(body, "count", 0); // wide read; the cap is checked before narrowing
    return r;
}

json::Value AllocateCommandBuffersResponse::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("ok", json::Value(ok));
    b.set("reason", json::Value(reason));
    b.set("command_buffers", handle_array(command_buffers));
    return b;
}

AllocateCommandBuffersResponse AllocateCommandBuffersResponse::from_body(const json::Value& body) {
    AllocateCommandBuffersResponse r;
    r.ok = get_bool(body, "ok");
    r.reason = get_string(body, "reason");
    r.command_buffers = get_handle_array(body, "command_buffers");
    return r;
}

json::Value FreeCommandBuffersRequest::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("command_pool", handle_value(command_pool));
    b.set("command_buffers", handle_array(command_buffers));
    return b;
}

FreeCommandBuffersRequest FreeCommandBuffersRequest::from_body(const json::Value& body) {
    FreeCommandBuffersRequest r;
    r.command_pool = get_handle(body, "command_pool");
    r.command_buffers = get_handle_array(body, "command_buffers");
    return r;
}

json::Value CreateFenceRequest::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("device", handle_value(device));
    b.set("signaled", json::Value(signaled));
    return b;
}

CreateFenceRequest CreateFenceRequest::from_body(const json::Value& body) {
    CreateFenceRequest r;
    r.device = get_handle(body, "device");
    r.signaled = get_bool(body, "signaled"); // absent -> false (back-compat)
    return r;
}

json::Value CreateFenceResponse::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("ok", json::Value(ok));
    b.set("reason", json::Value(reason));
    b.set("fence", handle_value(fence));
    return b;
}

CreateFenceResponse CreateFenceResponse::from_body(const json::Value& body) {
    CreateFenceResponse r;
    r.ok = get_bool(body, "ok");
    r.reason = get_string(body, "reason");
    r.fence = get_handle(body, "fence");
    return r;
}

json::Value CreateSemaphoreRequest::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("device", handle_value(device));
    b.set("semaphore_type", json::Value(static_cast<double>(semaphore_type)));
    b.set("initial_value", handle_value(initial_value));
    return b;
}

CreateSemaphoreRequest CreateSemaphoreRequest::from_body(const json::Value& body) {
    CreateSemaphoreRequest r;
    r.device = get_handle(body, "device");
    r.semaphore_type = get_int(body, "semaphore_type", 0); // legacy -> 0 (binary)
    r.initial_value = get_handle(body, "initial_value");
    return r;
}

json::Value CreateSemaphoreResponse::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("ok", json::Value(ok));
    b.set("reason", json::Value(reason));
    b.set("semaphore", handle_value(semaphore));
    return b;
}

CreateSemaphoreResponse CreateSemaphoreResponse::from_body(const json::Value& body) {
    CreateSemaphoreResponse r;
    r.ok = get_bool(body, "ok");
    r.reason = get_string(body, "reason");
    r.semaphore = get_handle(body, "semaphore");
    return r;
}

json::Value CreateEventRequest::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("device", handle_value(device));
    return b;
}

CreateEventRequest CreateEventRequest::from_body(const json::Value& body) {
    CreateEventRequest r;
    r.device = get_handle(body, "device");
    return r;
}

json::Value CreateEventResponse::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("ok", json::Value(ok));
    b.set("reason", json::Value(reason));
    b.set("event", handle_value(event));
    return b;
}

CreateEventResponse CreateEventResponse::from_body(const json::Value& body) {
    CreateEventResponse r;
    r.ok = get_bool(body, "ok");
    r.reason = get_string(body, "reason");
    r.event = get_handle(body, "event");
    return r;
}

json::Value GetEventStatusResponse::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("ok", json::Value(ok));
    b.set("reason", json::Value(reason));
    b.set("result", json::Value(result));
    return b;
}

GetEventStatusResponse GetEventStatusResponse::from_body(const json::Value& body) {
    GetEventStatusResponse r;
    r.ok = get_bool(body, "ok");
    r.reason = get_string(body, "reason");
    r.result = static_cast<int>(get_i64(body, "result", 0));
    return r;
}

json::Value AllocateMemoryRequest::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("device", handle_value(device));
    b.set("allocation_size", handle_value(allocation_size)); // 64-bit VkDeviceSize
    b.set("memory_type_index", json::Value(memory_type_index));
    b.set("allocate_flags", handle_value(allocate_flags)); // VkMemoryAllocateFlags (wide)
    return b;
}

AllocateMemoryRequest AllocateMemoryRequest::from_body(const json::Value& body) {
    AllocateMemoryRequest r;
    r.device = get_handle(body, "device");
    r.allocation_size = get_handle(body, "allocation_size"); // decimal string, full 64-bit
    // Fallback to a negative sentinel (not 0): a missing / null / wrong-typed
    // index must be rejected by the range check, not silently allocate type 0
    // (which is a legitimate index only when explicitly sent).
    r.memory_type_index = get_i64(body, "memory_type_index", -1);
    r.allocate_flags = get_handle(body, "allocate_flags"); // missing (legacy) decodes to 0
    return r;
}

json::Value AllocateMemoryResponse::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("ok", json::Value(ok));
    b.set("reason", json::Value(reason));
    b.set("memory", handle_value(memory));
    return b;
}

AllocateMemoryResponse AllocateMemoryResponse::from_body(const json::Value& body) {
    AllocateMemoryResponse r;
    r.ok = get_bool(body, "ok");
    r.reason = get_string(body, "reason");
    r.memory = get_handle(body, "memory");
    return r;
}

json::Value GetBufferDeviceAddressRequest::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("device", handle_value(device));
    b.set("buffer", handle_value(buffer));
    return b;
}

GetBufferDeviceAddressRequest GetBufferDeviceAddressRequest::from_body(const json::Value& body) {
    GetBufferDeviceAddressRequest r;
    r.device = get_handle(body, "device");
    r.buffer = get_handle(body, "buffer");
    return r;
}

json::Value GetBufferDeviceAddressResponse::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("ok", json::Value(ok));
    b.set("reason", json::Value(reason));
    b.set("device_address", handle_value(device_address)); // full 64-bit GPU VA (wide)
    return b;
}

GetBufferDeviceAddressResponse GetBufferDeviceAddressResponse::from_body(const json::Value& body) {
    GetBufferDeviceAddressResponse r;
    r.ok = get_bool(body, "ok");
    r.reason = get_string(body, "reason");
    r.device_address = get_handle(body, "device_address");
    return r;
}

json::Value CreateSurfaceRequest::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("instance", handle_value(instance));
    b.set("platform", json::Value(platform));
    b.set("xid", handle_value(xid));
    b.set("role_hint", json::Value(role_hint));
    return b;
}

CreateSurfaceRequest CreateSurfaceRequest::from_body(const json::Value& body) {
    CreateSurfaceRequest r;
    r.instance = get_handle(body, "instance");
    r.platform = get_string(body, "platform");
    r.xid = get_handle(body, "xid");
    // role_hint is advisory; an absent field (legacy create_surface) defaults to "UnknownPending".
    const std::string role = get_string(body, "role_hint");
    r.role_hint = role.empty() ? "UnknownPending" : role;
    return r;
}

json::Value CreateSurfaceResponse::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("ok", json::Value(ok));
    b.set("reason", json::Value(reason));
    b.set("surface", handle_value(surface));
    return b;
}

CreateSurfaceResponse CreateSurfaceResponse::from_body(const json::Value& body) {
    CreateSurfaceResponse r;
    r.ok = get_bool(body, "ok");
    r.reason = get_string(body, "reason");
    r.surface = get_handle(body, "surface");
    return r;
}

json::Value CreateSwapchainRequest::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("device", handle_value(device));
    b.set("surface", handle_value(surface));
    b.set("image_format", json::Value(image_format));
    b.set("color_space", json::Value(color_space));
    b.set("width", json::Value(width));
    b.set("height", json::Value(height));
    b.set("min_image_count", json::Value(min_image_count));
    b.set("present_mode", json::Value(present_mode));
    b.set("image_usage", json::Value(image_usage));
    b.set("use_current_extent", json::Value(use_current_extent));
    b.set("old_swapchain", handle_value(old_swapchain));
    return b;
}

CreateSwapchainRequest CreateSwapchainRequest::from_body(const json::Value& body) {
    CreateSwapchainRequest r;
    r.device = get_handle(body, "device");
    r.surface = get_handle(body, "surface");
    // Extents / counts / enums decode wide with a -1 sentinel (not get_int, which
    // would truncate an out-of-range value into a plausible small int before the
    // real backend can reject it against the surface's capabilities).
    r.image_format = get_index(body, "image_format");
    r.color_space = get_index(body, "color_space");
    r.width = get_index(body, "width");
    r.height = get_index(body, "height");
    r.min_image_count = get_index(body, "min_image_count");
    r.present_mode = get_index(body, "present_mode");
    // image_usage is a VkImageUsageFlags bitmask; wide-decode + -1 sentinel so a
    // missing / out-of-range value is rejected (a usage of 0 is invalid -- the app
    // must request at least one bit).
    r.image_usage = get_index(body, "image_usage");
    r.use_current_extent = get_bool(body, "use_current_extent");
    r.old_swapchain = get_handle(body, "old_swapchain");
    return r;
}

json::Value CreateSwapchainResponse::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("ok", json::Value(ok));
    b.set("reason", json::Value(reason));
    b.set("swapchain", handle_value(swapchain));
    b.set("result", json::Value(result));
    return b;
}

CreateSwapchainResponse CreateSwapchainResponse::from_body(const json::Value& body) {
    CreateSwapchainResponse r;
    r.ok = get_bool(body, "ok");
    r.reason = get_string(body, "reason");
    r.swapchain = get_handle(body, "swapchain");
    r.result =
        static_cast<int>(get_i64(body, "result", 0)); // VkResult is signed; absent -> SUCCESS
    return r;
}

json::Value GetSwapchainImagesRequest::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("swapchain", handle_value(swapchain));
    return b;
}

GetSwapchainImagesRequest GetSwapchainImagesRequest::from_body(const json::Value& body) {
    GetSwapchainImagesRequest r;
    r.swapchain = get_handle(body, "swapchain");
    return r;
}

json::Value GetSwapchainImagesResponse::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("ok", json::Value(ok));
    b.set("reason", json::Value(reason));
    b.set("images", handle_array(images));
    return b;
}

GetSwapchainImagesResponse GetSwapchainImagesResponse::from_body(const json::Value& body) {
    GetSwapchainImagesResponse r;
    r.ok = get_bool(body, "ok");
    r.reason = get_string(body, "reason");
    r.images = get_handle_array(body, "images");
    return r;
}

json::Value AcquireNextImageRequest::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("swapchain", handle_value(swapchain));
    b.set("timeout", handle_value(timeout)); // 64-bit ns, decimal string
    b.set("semaphore", handle_value(semaphore));
    b.set("fence", handle_value(fence));
    return b;
}

AcquireNextImageRequest AcquireNextImageRequest::from_body(const json::Value& body) {
    AcquireNextImageRequest r;
    r.swapchain = get_handle(body, "swapchain");
    r.timeout = get_handle(body, "timeout"); // full 64-bit (UINT64_MAX = block)
    r.semaphore = get_handle(body, "semaphore");
    r.fence = get_handle(body, "fence");
    return r;
}

json::Value AcquireNextImageResponse::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("ok", json::Value(ok));
    b.set("reason", json::Value(reason));
    b.set("image_index", json::Value(image_index));
    b.set("result", json::Value(result));
    return b;
}

AcquireNextImageResponse AcquireNextImageResponse::from_body(const json::Value& body) {
    AcquireNextImageResponse r;
    r.ok = get_bool(body, "ok");
    r.reason = get_string(body, "reason");
    r.image_index = get_int(body, "image_index", 0);
    r.result = static_cast<int>(get_i64(body, "result", 0)); // VkResult is signed
    return r;
}

json::Value QueuePresentRequest::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("queue", handle_value(queue));
    b.set("wait_semaphores", handle_array(wait_semaphores));
    json::Array arr;
    for (const auto& e : presents) {
        json::Value pe = json::Value::make_object();
        pe.set("swapchain", handle_value(e.swapchain));
        pe.set("image_index", json::Value(e.image_index));
        arr.emplace_back(std::move(pe));
    }
    b.set("presents", json::Value(std::move(arr)));
    return b;
}

QueuePresentRequest QueuePresentRequest::from_body(const json::Value& body) {
    QueuePresentRequest r;
    r.queue = get_handle(body, "queue");
    r.wait_semaphores = get_handle_array(body, "wait_semaphores");
    const json::Value* arr = body.find("presents");
    if (arr != nullptr && arr->is_array()) {
        for (const auto& e : arr->as_array()) {
            PresentEntry pe;
            pe.swapchain = get_handle(e, "swapchain");
            // Wide decode + -1 sentinel: a missing / out-of-range index is rejected
            // against image_count, never truncated into a plausible valid index.
            pe.image_index = get_index(e, "image_index");
            r.presents.push_back(pe);
        }
    }
    return r;
}

json::Value QueuePresentResponse::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("ok", json::Value(ok));
    b.set("reason", json::Value(reason));
    b.set("result", json::Value(result));
    json::Array arr;
    for (const int res : results) {
        arr.emplace_back(json::Value(res));
    }
    b.set("results", json::Value(std::move(arr)));
    return b;
}

QueuePresentResponse QueuePresentResponse::from_body(const json::Value& body) {
    QueuePresentResponse r;
    r.ok = get_bool(body, "ok");
    r.reason = get_string(body, "reason");
    r.result = static_cast<int>(get_i64(body, "result", 0));
    const json::Value* arr = body.find("results");
    if (arr != nullptr && arr->is_array()) {
        for (const auto& e : arr->as_array()) {
            r.results.push_back(e.is_integer() ? static_cast<int>(e.as_int()) : 0);
        }
    }
    return r;
}

json::Value RecordCommandBufferRequest::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("command_buffer", handle_value(command_buffer));
    b.set("one_time_submit", json::Value(one_time_submit));
    json::Array arr;
    for (const RecordedCommand& c : commands) {
        json::Value cv = json::Value::make_object();
        cv.set("kind", json::Value(c.kind));
        cv.set("image", handle_value(c.image));
        cv.set("old_layout", json::Value(c.old_layout));
        cv.set("new_layout", json::Value(c.new_layout));
        cv.set("src_stage", json::Value(c.src_stage));
        cv.set("dst_stage", json::Value(c.dst_stage));
        cv.set("src_access", json::Value(c.src_access));
        cv.set("dst_access", json::Value(c.dst_access));
        cv.set("aspect", json::Value(c.aspect));
        cv.set("layout", json::Value(c.layout));
        cv.set("r", json::Value(c.r));
        cv.set("g", json::Value(c.g));
        cv.set("b", json::Value(c.b));
        cv.set("a", json::Value(c.a));
        // Draw subset.
        cv.set("render_pass", handle_value(c.render_pass));
        cv.set("framebuffer", handle_value(c.framebuffer));
        // begin_render_pass depth clear: explicit presence flag.
        cv.set("has_depth_clear", json::Value(c.has_depth_clear));
        cv.set("depth_clear", json::Value(c.depth_clear));
        cv.set("pipeline", handle_value(c.pipeline));
        cv.set("render_area_x", json::Value(c.render_area_x));
        cv.set("render_area_y", json::Value(c.render_area_y));
        cv.set("render_area_w", json::Value(c.render_area_w));
        cv.set("render_area_h", json::Value(c.render_area_h));
        cv.set("vp_x", json::Value(c.vp_x));
        cv.set("vp_y", json::Value(c.vp_y));
        cv.set("vp_w", json::Value(c.vp_w));
        cv.set("vp_h", json::Value(c.vp_h));
        cv.set("vp_min_depth", json::Value(c.vp_min_depth));
        cv.set("vp_max_depth", json::Value(c.vp_max_depth));
        cv.set("sc_x", json::Value(c.sc_x));
        cv.set("sc_y", json::Value(c.sc_y));
        cv.set("sc_w", json::Value(c.sc_w));
        cv.set("sc_h", json::Value(c.sc_h));
        cv.set("vertex_count", json::Value(c.vertex_count));
        cv.set("instance_count", json::Value(c.instance_count));
        cv.set("first_vertex", json::Value(c.first_vertex));
        cv.set("first_instance", json::Value(c.first_instance));
        // bind_vertex_buffers.
        cv.set("first_binding", json::Value(c.first_binding));
        cv.set("vertex_buffers", handle_array(c.vertex_buffers));
        cv.set("vertex_buffer_offsets", handle_array(c.vertex_buffer_offsets));
        // bind_descriptor_sets.
        cv.set("desc_layout", handle_value(c.desc_layout));
        cv.set("first_set", json::Value(c.first_set));
        cv.set("descriptor_sets", handle_array(c.descriptor_sets));
        // copy_buffer_to_image + generalized pipeline_barrier.
        cv.set("src_buffer", handle_value(c.src_buffer));
        cv.set("copy_width", json::Value(c.copy_width));
        cv.set("copy_height", json::Value(c.copy_height));
        cv.set("copy_depth", json::Value(c.copy_depth));
        cv.set("barrier_base_mip", json::Value(c.barrier_base_mip));
        cv.set("barrier_level_count", json::Value(c.barrier_level_count));
        cv.set("barrier_base_layer", json::Value(c.barrier_base_layer));
        cv.set("barrier_layer_count", json::Value(c.barrier_layer_count));
        // GL/zink: generic positional payload for the broad faithful command set.
        cv.set("args_u64", handle_array(c.args_u64));
        {
            json::Array ai;
            for (const long long v : c.args_i64) {
                ai.emplace_back(json::Value(v));
            }
            cv.set("args_i64", json::Value(std::move(ai)));
            json::Array af;
            for (const double v : c.args_f64) {
                af.emplace_back(json::Value(v));
            }
            cv.set("args_f64", json::Value(std::move(af)));
        }
        cv.set("args_blob", json::Value(to_hex(c.args_blob)));
        // The typed DependencyInfo2 vector (JSON fallback path; the primary
        // record path is the sparse binary above). Each barrier is a positional number array --
        // masks as decimal strings (u64), QFI/layouts/ranges as numbers (wide).
        if (!c.deps2.empty()) {
            json::Array deps;
            for (const DependencyInfo2& d : c.deps2) {
                json::Value dv = json::Value::make_object();
                dv.set("flags", handle_value(d.dependency_flags));
                json::Array mem;
                for (const MemoryBarrier2& m : d.memory) {
                    json::Array a;
                    a.emplace_back(handle_value(m.src_stage));
                    a.emplace_back(handle_value(m.src_access));
                    a.emplace_back(handle_value(m.dst_stage));
                    a.emplace_back(handle_value(m.dst_access));
                    mem.emplace_back(json::Value(std::move(a)));
                }
                dv.set("mem", json::Value(std::move(mem)));
                json::Array buf;
                for (const BufferMemoryBarrier2& b2 : d.buffer) {
                    json::Array a;
                    a.emplace_back(handle_value(b2.src_stage));
                    a.emplace_back(handle_value(b2.src_access));
                    a.emplace_back(handle_value(b2.dst_stage));
                    a.emplace_back(handle_value(b2.dst_access));
                    a.emplace_back(json::Value(b2.src_queue_family));
                    a.emplace_back(json::Value(b2.dst_queue_family));
                    a.emplace_back(handle_value(b2.buffer));
                    a.emplace_back(handle_value(b2.offset));
                    a.emplace_back(handle_value(b2.size));
                    buf.emplace_back(json::Value(std::move(a)));
                }
                dv.set("buf", json::Value(std::move(buf)));
                json::Array img;
                for (const ImageMemoryBarrier2& im : d.image) {
                    json::Array a;
                    a.emplace_back(handle_value(im.src_stage));
                    a.emplace_back(handle_value(im.src_access));
                    a.emplace_back(handle_value(im.dst_stage));
                    a.emplace_back(handle_value(im.dst_access));
                    a.emplace_back(json::Value(static_cast<long long>(im.old_layout)));
                    a.emplace_back(json::Value(static_cast<long long>(im.new_layout)));
                    a.emplace_back(json::Value(im.src_queue_family));
                    a.emplace_back(json::Value(im.dst_queue_family));
                    a.emplace_back(handle_value(im.image));
                    a.emplace_back(json::Value(im.aspect));
                    a.emplace_back(json::Value(im.base_mip));
                    a.emplace_back(json::Value(im.level_count));
                    a.emplace_back(json::Value(im.base_layer));
                    a.emplace_back(json::Value(im.layer_count));
                    img.emplace_back(json::Value(std::move(a)));
                }
                dv.set("img", json::Value(std::move(img)));
                deps.emplace_back(std::move(dv));
            }
            cv.set("deps2", json::Value(std::move(deps)));
        }
        arr.emplace_back(std::move(cv));
    }
    b.set("commands", json::Value(std::move(arr)));
    return b;
}

RecordCommandBufferRequest RecordCommandBufferRequest::from_body(const json::Value& body) {
    RecordCommandBufferRequest r;
    r.command_buffer = get_handle(body, "command_buffer");
    r.one_time_submit = get_bool(body, "one_time_submit");
    const json::Value* arr = body.find("commands");
    if (arr != nullptr && arr->is_array()) {
        for (const auto& e : arr->as_array()) {
            RecordedCommand c;
            c.kind = get_string(e, "kind");
            c.image = get_handle(e, "image");
            // Layouts / aspect decode wide + -1 sentinel (a malformed enum is rejected by
            // validation, not truncated into a plausible value).
            c.old_layout = get_index(e, "old_layout");
            c.new_layout = get_index(e, "new_layout");
            // Stage / access masks are VkFlags; carried wide. A missing / negative value
            // becomes -1 so a zero/invalid stage mask is rejected (sync1 needs non-zero).
            c.src_stage = get_i64(e, "src_stage", -1);
            c.dst_stage = get_i64(e, "dst_stage", -1);
            c.src_access = get_i64(e, "src_access", -1);
            c.dst_access = get_i64(e, "dst_access", -1);
            c.aspect = get_index(e, "aspect");
            c.layout = get_index(e, "layout");
            c.r = get_number(e, "r", 0.0);
            c.g = get_number(e, "g", 0.0);
            c.b = get_number(e, "b", 0.0);
            c.a = get_number(e, "a", 0.0);
            // Draw subset. Handles via get_handle; extents/counts wide with -1 sentinel
            // so a malformed value is rejected by validation, not truncated; viewport floats via
            // get_number.
            c.render_pass = get_handle(e, "render_pass");
            c.framebuffer = get_handle(e, "framebuffer");
            // begin_render_pass depth clear: presence flag + value (default false/0.0
            // so an omitting peer is treated as "no depth clear carried", rejected for a depth
            // pass).
            c.has_depth_clear = get_bool(e, "has_depth_clear");
            c.depth_clear = get_number(e, "depth_clear", 0.0);
            c.pipeline = get_handle(e, "pipeline");
            c.render_area_x = get_int(e, "render_area_x", 0);
            c.render_area_y = get_int(e, "render_area_y", 0);
            c.render_area_w = get_index(e, "render_area_w");
            c.render_area_h = get_index(e, "render_area_h");
            c.vp_x = get_number(e, "vp_x", 0.0);
            c.vp_y = get_number(e, "vp_y", 0.0);
            c.vp_w = get_number(e, "vp_w", 0.0);
            c.vp_h = get_number(e, "vp_h", 0.0);
            c.vp_min_depth = get_number(e, "vp_min_depth", 0.0);
            c.vp_max_depth = get_number(e, "vp_max_depth", 0.0);
            c.sc_x = get_int(e, "sc_x", 0);
            c.sc_y = get_int(e, "sc_y", 0);
            c.sc_w = get_index(e, "sc_w");
            c.sc_h = get_index(e, "sc_h");
            c.vertex_count = get_i64(e, "vertex_count", -1);
            c.instance_count = get_i64(e, "instance_count", -1);
            c.first_vertex = get_i64(e, "first_vertex", -1);
            c.first_instance = get_i64(e, "first_instance", -1);
            // bind_vertex_buffers: first_binding wide (-1 sentinel); the parallel
            // buffers/offsets arrays preserve length (a malformed entry -> handle 0, rejected
            // later).
            c.first_binding = get_index(e, "first_binding");
            c.vertex_buffers = get_handle_array(e, "vertex_buffers");
            c.vertex_buffer_offsets = get_handle_array(e, "vertex_buffer_offsets");
            // bind_descriptor_sets: desc_layout handle; first_set wide (-1 sentinel);
            // the set list preserves length (a malformed entry -> handle 0, rejected later).
            c.desc_layout = get_handle(e, "desc_layout");
            c.first_set = get_index(e, "first_set");
            c.descriptor_sets = get_handle_array(e, "descriptor_sets");
            // copy_buffer_to_image + generalized pipeline_barrier: src buffer
            // handle; copy extent wide (-1 sentinel, rejected by validation); barrier mip/layer
            // range wide with -1 = missing (the worker treats it as the whole single subresource --
            // the legacy swapchain-color barrier shape).
            c.src_buffer = get_handle(e, "src_buffer");
            c.copy_width = get_i64(e, "copy_width", -1);
            c.copy_height = get_i64(e, "copy_height", -1);
            c.copy_depth = get_i64(e, "copy_depth", -1);
            c.barrier_base_mip = get_index(e, "barrier_base_mip");
            c.barrier_level_count = get_index(e, "barrier_level_count");
            c.barrier_base_layer = get_index(e, "barrier_base_layer");
            c.barrier_layer_count = get_index(e, "barrier_layer_count");
            // GL/zink: generic positional payload.
            c.args_u64 = get_handle_array(e, "args_u64");
            const json::Value* ai = e.find("args_i64");
            if (ai != nullptr && ai->is_array()) {
                for (const auto& v : ai->as_array()) {
                    c.args_i64.push_back(
                        static_cast<long long>(v.is_number() ? v.as_number() : 0.0));
                }
            }
            const json::Value* af = e.find("args_f64");
            if (af != nullptr && af->is_array()) {
                for (const auto& v : af->as_array()) {
                    c.args_f64.push_back(v.is_number() ? v.as_number() : 0.0);
                }
            }
            c.args_blob = get_hex_blob(e, "args_blob");
            // The typed DependencyInfo2 vector (JSON fallback path). Positional
            // per-barrier number arrays; masks/handles are decimal strings, the rest numbers.
            const json::Value* deps = e.find("deps2");
            if (deps != nullptr && deps->is_array()) {
                const auto u64_at = [](const json::Array& a, std::size_t i) -> std::uint64_t {
                    return (i < a.size() && a[i].is_string()) ? parse_handle_str(a[i].as_string())
                                                              : 0;
                };
                const auto i64_at = [](const json::Array& a, std::size_t i) -> long long {
                    return (i < a.size() && a[i].is_number())
                               ? static_cast<long long>(a[i].as_number())
                               : 0;
                };
                for (const auto& dv : deps->as_array()) {
                    if (!dv.is_object()) {
                        continue;
                    }
                    DependencyInfo2 d;
                    d.dependency_flags = get_handle(dv, "flags");
                    const json::Value* mem = dv.find("mem");
                    if (mem != nullptr && mem->is_array()) {
                        for (const auto& mv : mem->as_array()) {
                            if (!mv.is_array()) {
                                continue;
                            }
                            const json::Array& a = mv.as_array();
                            MemoryBarrier2 m;
                            m.src_stage = u64_at(a, 0);
                            m.src_access = u64_at(a, 1);
                            m.dst_stage = u64_at(a, 2);
                            m.dst_access = u64_at(a, 3);
                            d.memory.push_back(m);
                        }
                    }
                    const json::Value* buf = dv.find("buf");
                    if (buf != nullptr && buf->is_array()) {
                        for (const auto& bv : buf->as_array()) {
                            if (!bv.is_array()) {
                                continue;
                            }
                            const json::Array& a = bv.as_array();
                            BufferMemoryBarrier2 b2;
                            b2.src_stage = u64_at(a, 0);
                            b2.src_access = u64_at(a, 1);
                            b2.dst_stage = u64_at(a, 2);
                            b2.dst_access = u64_at(a, 3);
                            b2.src_queue_family = i64_at(a, 4);
                            b2.dst_queue_family = i64_at(a, 5);
                            b2.buffer = u64_at(a, 6);
                            b2.offset = u64_at(a, 7);
                            b2.size = u64_at(a, 8);
                            d.buffer.push_back(b2);
                        }
                    }
                    const json::Value* img = dv.find("img");
                    if (img != nullptr && img->is_array()) {
                        for (const auto& iv : img->as_array()) {
                            if (!iv.is_array()) {
                                continue;
                            }
                            const json::Array& a = iv.as_array();
                            ImageMemoryBarrier2 im;
                            im.src_stage = u64_at(a, 0);
                            im.src_access = u64_at(a, 1);
                            im.dst_stage = u64_at(a, 2);
                            im.dst_access = u64_at(a, 3);
                            im.old_layout = static_cast<int>(i64_at(a, 4));
                            im.new_layout = static_cast<int>(i64_at(a, 5));
                            im.src_queue_family = i64_at(a, 6);
                            im.dst_queue_family = i64_at(a, 7);
                            im.image = u64_at(a, 8);
                            im.aspect = i64_at(a, 9);
                            im.base_mip = i64_at(a, 10);
                            im.level_count = i64_at(a, 11);
                            im.base_layer = i64_at(a, 12);
                            im.layer_count = i64_at(a, 13);
                            d.image.push_back(im);
                        }
                    }
                    c.deps2.push_back(std::move(d));
                }
            }
            r.commands.push_back(std::move(c));
        }
    }
    return r;
}

// the double presence test compares BIT PATTERNS, not values -- a legit -0.0 or NaN
// differs from the +0.0 default's all-zero bits and is carried byte-exact (value comparison would
// drop -0.0 and choke on NaN). Matches the codec's bit-cast f64 transport.
namespace {
std::uint64_t f64_bits(double v) {
    std::uint64_t bits = 0;
    std::memcpy(&bits, &v, 8);
    return bits;
}
} // namespace

// ONE hash lookup per command replaces the ~40-arm string-compare dispatch chains in
// both backends (the chains were a measured slice of the worker's per-command validate time at
// ETR volumes). The table is built once; unknown kinds map to Unknown, which every chain rejects
// exactly as the old final `else` did.
CmdKind cmd_kind_from_string(const std::string& kind) {
    static const std::unordered_map<std::string_view, CmdKind> kMap = {
        {"pipeline_barrier", CmdKind::PipelineBarrier},
        {"clear_color_image", CmdKind::ClearColorImage},
        {"begin_render_pass", CmdKind::BeginRenderPass},
        {"end_render_pass", CmdKind::EndRenderPass},
        {"bind_pipeline", CmdKind::BindPipeline},
        {"set_viewport", CmdKind::SetViewport},
        {"set_scissor", CmdKind::SetScissor},
        {"draw", CmdKind::Draw},
        {"draw_indexed", CmdKind::DrawIndexed},
        {"draw_indirect_byte_count", CmdKind::DrawIndirectByteCount},
        {"bind_vertex_buffers", CmdKind::BindVertexBuffers},
        {"bind_descriptor_sets", CmdKind::BindDescriptorSets},
        {"bind_index_buffer", CmdKind::BindIndexBuffer},
        {"push_constants", CmdKind::PushConstants},
        {"dispatch", CmdKind::Dispatch},
        {"dispatch_indirect", CmdKind::DispatchIndirect},
        {"memory_barrier", CmdKind::MemoryBarrierGlobal},
        {"buffer_memory_barrier", CmdKind::BufferMemoryBarrier},
        {"copy_buffer_to_image", CmdKind::CopyBufferToImage},
        {"copy_buffer", CmdKind::CopyBuffer},
        {"copy_image_to_buffer", CmdKind::CopyImageToBuffer},
        {"clear_attachments", CmdKind::ClearAttachments},
        {"blit_image", CmdKind::BlitImage},
        {"copy_image", CmdKind::CopyImage},
        {"bind_transform_feedback_buffers", CmdKind::BindTransformFeedbackBuffers},
        {"begin_transform_feedback", CmdKind::BeginTransformFeedback},
        {"end_transform_feedback", CmdKind::EndTransformFeedback},
        {"begin_conditional_rendering", CmdKind::BeginConditionalRendering},
        {"end_conditional_rendering", CmdKind::EndConditionalRendering},
        {"reset_query_pool", CmdKind::ResetQueryPool},
        {"begin_query", CmdKind::BeginQuery},
        {"end_query", CmdKind::EndQuery},
        {"write_timestamp", CmdKind::WriteTimestamp},
        {"copy_query_pool_results", CmdKind::CopyQueryPoolResults},
        {"set_line_width", CmdKind::SetLineWidth},
        {"set_depth_bias", CmdKind::SetDepthBias},
        {"set_blend_constants", CmdKind::SetBlendConstants},
        {"set_depth_bounds", CmdKind::SetDepthBounds},
        {"set_stencil_compare_mask", CmdKind::SetStencilCompareMask},
        {"set_stencil_write_mask", CmdKind::SetStencilWriteMask},
        {"set_stencil_reference", CmdKind::SetStencilReference},
        {"set_cull_mode", CmdKind::SetCullMode},
        {"set_front_face", CmdKind::SetFrontFace},
        {"set_primitive_topology", CmdKind::SetPrimitiveTopology},
        {"set_depth_test_enable", CmdKind::SetDepthTestEnable},
        {"set_depth_write_enable", CmdKind::SetDepthWriteEnable},
        {"set_depth_compare_op", CmdKind::SetDepthCompareOp},
        {"begin_rendering", CmdKind::BeginRendering},
        {"end_rendering", CmdKind::EndRendering},
        {"set_event", CmdKind::CmdSetEvent},
        {"reset_event", CmdKind::CmdResetEvent},
        {"wait_events", CmdKind::CmdWaitEvents},
        {"pipeline_barrier2", CmdKind::PipelineBarrier2},
        {"write_timestamp2", CmdKind::WriteTimestamp2},
        {"set_event2", CmdKind::CmdSetEvent2},
        {"reset_event2", CmdKind::CmdResetEvent2},
        {"wait_events2", CmdKind::CmdWaitEvents2},
        {"set_viewport_with_count", CmdKind::SetViewportWithCount},
        {"set_scissor_with_count", CmdKind::SetScissorWithCount},
        {"bind_vertex_buffers2", CmdKind::BindVertexBuffers2},
        {"set_depth_bounds_test_enable", CmdKind::SetDepthBoundsTestEnable},
        {"set_stencil_test_enable", CmdKind::SetStencilTestEnable},
        {"set_stencil_op", CmdKind::SetStencilOp},
        {"set_rasterizer_discard_enable", CmdKind::SetRasterizerDiscardEnable},
        {"set_depth_bias_enable", CmdKind::SetDepthBiasEnable},
        {"set_primitive_restart_enable", CmdKind::SetPrimitiveRestartEnable},
        {"resolve_image", CmdKind::ResolveImage},
        {"fill_buffer", CmdKind::FillBuffer},
        {"draw_indirect", CmdKind::DrawIndirect},
        {"draw_indexed_indirect", CmdKind::DrawIndexedIndirect},
    };
    const auto it = kMap.find(std::string_view(kind));
    return it != kMap.end() ? it->second : CmdKind::Unknown;
}

bool validate_dependency_info2(const DependencyInfo2& d, std::string& reason) {
    if (d.memory.size() > kMaxSync2BarriersPerDep || d.buffer.size() > kMaxSync2BarriersPerDep ||
        d.image.size() > kMaxSync2BarriersPerDep) {
        reason = "sync2 dependency barrier count exceeds the supported cap";
        return false;
    }
    const auto qfi_ok = [](long long s, long long dd) {
        return (s == kVkQueueFamilyIgnored && dd == kVkQueueFamilyIgnored) || (s == 0 && dd == 0);
    };
    for (const MemoryBarrier2& m : d.memory) {
        if (!valid_stage_mask2(m.src_stage) || !valid_stage_mask2(m.dst_stage) ||
            !valid_access_mask2(m.src_access) || !valid_access_mask2(m.dst_access)) {
            reason = "sync2 memory barrier has an out-of-range mask";
            return false;
        }
    }
    for (const BufferMemoryBarrier2& b : d.buffer) {
        if (!valid_stage_mask2(b.src_stage) || !valid_stage_mask2(b.dst_stage) ||
            !valid_access_mask2(b.src_access) || !valid_access_mask2(b.dst_access)) {
            reason = "sync2 buffer barrier has an out-of-range mask";
            return false;
        }
        if (!qfi_ok(b.src_queue_family, b.dst_queue_family)) {
            reason = "sync2 buffer barrier queue-family ownership transfer not supported";
            return false;
        }
    }
    for (const ImageMemoryBarrier2& im : d.image) {
        if (!valid_stage_mask2(im.src_stage) || !valid_stage_mask2(im.dst_stage) ||
            !valid_access_mask2(im.src_access) || !valid_access_mask2(im.dst_access)) {
            reason = "sync2 image barrier has an out-of-range mask";
            return false;
        }
        if (!qfi_ok(im.src_queue_family, im.dst_queue_family)) {
            reason = "sync2 image barrier queue-family ownership transfer not supported";
            return false;
        }
        if (im.old_layout < 0 || im.new_layout < 0 || im.aspect < 1) {
            reason = "sync2 image barrier has a malformed layout/aspect";
            return false;
        }
    }
    return true;
}

// the record wire is SPARSE (version 2). v1 serialized every RecordedCommand field for
// every command (~420 bytes each), so an ETR-class stream (~840 commands/record, ~4700/frame)
// shipped ~390 KB per record and the worker burned ~60% of its record handler DECODING default
// fields. v2 writes kind + a u64 PRESENCE MASK per command, then only the field groups whose
// value differs from the struct default (bit set <=> group present; an absent group decodes to
// the default the struct already holds, so encode->decode is identity for every representable
// command by construction). Group bits and the field order WITHIN groups are FROZEN; the header
// version gates the format fail-closed (a v1 body is rejected, never mis-decoded). Ints still
// ride wide as i64; doubles bit-cast.
namespace {
enum : std::uint64_t {
    kRecBarrierCore = 1ull << 0,   // image, old/new_layout, src/dst_stage, src/dst_access, aspect,
                                   // layout
    kRecClearColor = 1ull << 1,    // r, g, b, a
    kRecRenderPass = 1ull << 2,    // render_pass, framebuffer, has_depth_clear, depth_clear,
                                   // render_area_{x,y,w,h}
    kRecPipeline = 1ull << 3,      // pipeline
    kRecViewport = 1ull << 4,      // vp_{x,y,w,h,min_depth,max_depth}
    kRecScissor = 1ull << 5,       // sc_{x,y,w,h}
    kRecDraw = 1ull << 6,          // vertex_count, instance_count, first_vertex, first_instance
    kRecVertexBufs = 1ull << 7,    // first_binding, vertex_buffers, vertex_buffer_offsets
    kRecDescSets = 1ull << 8,      // desc_layout, first_set, descriptor_sets
    kRecCopy = 1ull << 9,          // src_buffer, copy_{width,height,depth}
    kRecBarrierRange = 1ull << 10, // barrier_{base_mip,level_count,base_layer,layer_count}
    kRecArgsU64 = 1ull << 11,
    kRecArgsI64 = 1ull << 12,
    kRecArgsF64 = 1ull << 13,
    kRecArgsBlob = 1ull << 14,
    kRecDeps2 = 1ull << 15,           // the typed DependencyInfo2 vector
    kRecKnownMask = (1ull << 16) - 1, // decode rejects any bit beyond the known set (fail-closed)
};
} // namespace

std::string RecordCommandBufferRequest::to_wire() const {
    json::Value h = json::Value::make_object();
    h.set("v", json::Value(2));
    h.set("command_buffer", handle_value(command_buffer));
    h.set("one_time_submit", json::Value(one_time_submit));
    h.set("count", json::Value(static_cast<long long>(commands.size())));
    const std::string json = h.dump(0);
    std::string out(4, '\0');
    protocol::store_le32(static_cast<std::uint32_t>(json.size()),
                         reinterpret_cast<unsigned char*>(&out[0]));
    out += json;
    for (const RecordedCommand& c : commands) {
        std::uint64_t mask = 0;
        if (c.image != 0 || c.old_layout != 0 || c.new_layout != 0 || c.src_stage != 0 ||
            c.dst_stage != 0 || c.src_access != 0 || c.dst_access != 0 || c.aspect != 0 ||
            c.layout != 0) {
            mask |= kRecBarrierCore;
        }
        if (f64_bits(c.r) != 0 || f64_bits(c.g) != 0 || f64_bits(c.b) != 0 || f64_bits(c.a) != 0) {
            mask |= kRecClearColor;
        }
        if (c.render_pass != 0 || c.framebuffer != 0 || c.has_depth_clear ||
            f64_bits(c.depth_clear) != 0 || c.render_area_x != 0 || c.render_area_y != 0 ||
            c.render_area_w != -1 || c.render_area_h != -1) {
            mask |= kRecRenderPass;
        }
        if (c.pipeline != 0) {
            mask |= kRecPipeline;
        }
        if (f64_bits(c.vp_x) != 0 || f64_bits(c.vp_y) != 0 || f64_bits(c.vp_w) != 0 ||
            f64_bits(c.vp_h) != 0 || f64_bits(c.vp_min_depth) != 0 ||
            f64_bits(c.vp_max_depth) != 0) {
            mask |= kRecViewport;
        }
        if (c.sc_x != 0 || c.sc_y != 0 || c.sc_w != -1 || c.sc_h != -1) {
            mask |= kRecScissor;
        }
        if (c.vertex_count != -1 || c.instance_count != -1 || c.first_vertex != -1 ||
            c.first_instance != -1) {
            mask |= kRecDraw;
        }
        if (c.first_binding != -1 || !c.vertex_buffers.empty() ||
            !c.vertex_buffer_offsets.empty()) {
            mask |= kRecVertexBufs;
        }
        if (c.desc_layout != 0 || c.first_set != -1 || !c.descriptor_sets.empty()) {
            mask |= kRecDescSets;
        }
        if (c.src_buffer != 0 || c.copy_width != -1 || c.copy_height != -1 || c.copy_depth != -1) {
            mask |= kRecCopy;
        }
        if (c.barrier_base_mip != -1 || c.barrier_level_count != -1 || c.barrier_base_layer != -1 ||
            c.barrier_layer_count != -1) {
            mask |= kRecBarrierRange;
        }
        if (!c.args_u64.empty()) {
            mask |= kRecArgsU64;
        }
        if (!c.args_i64.empty()) {
            mask |= kRecArgsI64;
        }
        if (!c.args_f64.empty()) {
            mask |= kRecArgsF64;
        }
        if (!c.args_blob.empty()) {
            mask |= kRecArgsBlob;
        }
        if (!c.deps2.empty()) {
            mask |= kRecDeps2;
        }
        wire_put_str(out, c.kind);
        wire_put_u64(out, mask);
        if (mask & kRecBarrierCore) {
            wire_put_u64(out, c.image);
            wire_put_i64(out, c.old_layout);
            wire_put_i64(out, c.new_layout);
            wire_put_i64(out, c.src_stage);
            wire_put_i64(out, c.dst_stage);
            wire_put_i64(out, c.src_access);
            wire_put_i64(out, c.dst_access);
            wire_put_i64(out, c.aspect);
            wire_put_i64(out, c.layout);
        }
        if (mask & kRecClearColor) {
            wire_put_f64(out, c.r);
            wire_put_f64(out, c.g);
            wire_put_f64(out, c.b);
            wire_put_f64(out, c.a);
        }
        if (mask & kRecRenderPass) {
            wire_put_u64(out, c.render_pass);
            wire_put_u64(out, c.framebuffer);
            wire_put_u64(out, c.has_depth_clear ? 1 : 0);
            wire_put_f64(out, c.depth_clear);
            wire_put_i64(out, c.render_area_x);
            wire_put_i64(out, c.render_area_y);
            wire_put_i64(out, c.render_area_w);
            wire_put_i64(out, c.render_area_h);
        }
        if (mask & kRecPipeline) {
            wire_put_u64(out, c.pipeline);
        }
        if (mask & kRecViewport) {
            wire_put_f64(out, c.vp_x);
            wire_put_f64(out, c.vp_y);
            wire_put_f64(out, c.vp_w);
            wire_put_f64(out, c.vp_h);
            wire_put_f64(out, c.vp_min_depth);
            wire_put_f64(out, c.vp_max_depth);
        }
        if (mask & kRecScissor) {
            wire_put_i64(out, c.sc_x);
            wire_put_i64(out, c.sc_y);
            wire_put_i64(out, c.sc_w);
            wire_put_i64(out, c.sc_h);
        }
        if (mask & kRecDraw) {
            wire_put_i64(out, c.vertex_count);
            wire_put_i64(out, c.instance_count);
            wire_put_i64(out, c.first_vertex);
            wire_put_i64(out, c.first_instance);
        }
        if (mask & kRecVertexBufs) {
            wire_put_i64(out, c.first_binding);
            wire_put_vec_u64(out, c.vertex_buffers);
            wire_put_vec_u64(out, c.vertex_buffer_offsets);
        }
        if (mask & kRecDescSets) {
            wire_put_u64(out, c.desc_layout);
            wire_put_i64(out, c.first_set);
            wire_put_vec_u64(out, c.descriptor_sets);
        }
        if (mask & kRecCopy) {
            wire_put_u64(out, c.src_buffer);
            wire_put_i64(out, c.copy_width);
            wire_put_i64(out, c.copy_height);
            wire_put_i64(out, c.copy_depth);
        }
        if (mask & kRecBarrierRange) {
            wire_put_i64(out, c.barrier_base_mip);
            wire_put_i64(out, c.barrier_level_count);
            wire_put_i64(out, c.barrier_base_layer);
            wire_put_i64(out, c.barrier_layer_count);
        }
        if (mask & kRecArgsU64) {
            wire_put_vec_u64(out, c.args_u64);
        }
        if (mask & kRecArgsI64) {
            wire_put_u64(out, c.args_i64.size());
            for (const long long v : c.args_i64) {
                wire_put_i64(out, v);
            }
        }
        if (mask & kRecArgsF64) {
            wire_put_u64(out, c.args_f64.size());
            for (const double v : c.args_f64) {
                wire_put_f64(out, v);
            }
        }
        if (mask & kRecArgsBlob) {
            wire_put_str(out, c.args_blob);
        }
        if (mask & kRecDeps2) {
            wire_put_u64(out, c.deps2.size());
            for (const DependencyInfo2& d : c.deps2) {
                wire_put_dep2(out, d);
            }
        }
    }
    return out;
}

RecordCommandBufferRequest RecordCommandBufferRequest::from_wire(const std::string& body,
                                                                 std::string& err) {
    err.clear();
    if (body.size() < 4) {
        err = "record body shorter than 4-byte length prefix";
        return RecordCommandBufferRequest{};
    }
    const std::uint32_t json_len =
        protocol::load_le32(reinterpret_cast<const unsigned char*>(body.data()));
    if (json_len > kMaxBinaryJsonHeaderBytes) {
        err = "record json header exceeds cap";
        return RecordCommandBufferRequest{};
    }
    if (static_cast<std::size_t>(4) + json_len > body.size()) {
        err = "record json header runs past end of body";
        return RecordCommandBufferRequest{};
    }
    json::Value h;
    std::string jerr;
    if (!json::Value::try_parse(body.substr(4, json_len), h, jerr)) {
        err = "record json header parse: " + jerr;
        return RecordCommandBufferRequest{};
    }
    if (get_int(h, "v", -1) != 2) {
        err = "record wire version not supported";
        return RecordCommandBufferRequest{};
    }
    RecordCommandBufferRequest r;
    r.command_buffer = get_handle(h, "command_buffer");
    r.one_time_submit = get_bool(h, "one_time_submit");
    const long long count = get_i64(h, "count", -1);
    if (count < 0) {
        err = "record header count missing";
        return RecordCommandBufferRequest{};
    }
    WireReader rd{body, static_cast<std::size_t>(4) + json_len};
    // Structural floor: the smallest sparse command (a 4-char kind + the mask, no groups) is
    // 16 bytes, so an absurd count is rejected before any allocation.
    if (static_cast<std::uint64_t>(count) > rd.remaining() / 16) {
        err = "record header count exceeds body";
        return RecordCommandBufferRequest{};
    }
    r.commands.reserve(static_cast<std::size_t>(count));
    for (long long i = 0; i < count && !rd.fail; ++i) {
        RecordedCommand c;
        c.kind = rd.get_str();
        const std::uint64_t mask = rd.get_u64();
        if ((mask & ~kRecKnownMask) != 0) {
            err = "record command carries an unknown field group";
            return RecordCommandBufferRequest{};
        }
        if (mask & kRecBarrierCore) {
            c.image = rd.get_u64();
            c.old_layout = static_cast<int>(rd.get_i64());
            c.new_layout = static_cast<int>(rd.get_i64());
            c.src_stage = rd.get_i64();
            c.dst_stage = rd.get_i64();
            c.src_access = rd.get_i64();
            c.dst_access = rd.get_i64();
            c.aspect = static_cast<int>(rd.get_i64());
            c.layout = static_cast<int>(rd.get_i64());
        }
        if (mask & kRecClearColor) {
            c.r = rd.get_f64();
            c.g = rd.get_f64();
            c.b = rd.get_f64();
            c.a = rd.get_f64();
        }
        if (mask & kRecRenderPass) {
            c.render_pass = rd.get_u64();
            c.framebuffer = rd.get_u64();
            c.has_depth_clear = rd.get_u64() != 0;
            c.depth_clear = rd.get_f64();
            c.render_area_x = static_cast<int>(rd.get_i64());
            c.render_area_y = static_cast<int>(rd.get_i64());
            c.render_area_w = static_cast<int>(rd.get_i64());
            c.render_area_h = static_cast<int>(rd.get_i64());
        }
        if (mask & kRecPipeline) {
            c.pipeline = rd.get_u64();
        }
        if (mask & kRecViewport) {
            c.vp_x = rd.get_f64();
            c.vp_y = rd.get_f64();
            c.vp_w = rd.get_f64();
            c.vp_h = rd.get_f64();
            c.vp_min_depth = rd.get_f64();
            c.vp_max_depth = rd.get_f64();
        }
        if (mask & kRecScissor) {
            c.sc_x = static_cast<int>(rd.get_i64());
            c.sc_y = static_cast<int>(rd.get_i64());
            c.sc_w = static_cast<int>(rd.get_i64());
            c.sc_h = static_cast<int>(rd.get_i64());
        }
        if (mask & kRecDraw) {
            c.vertex_count = rd.get_i64();
            c.instance_count = rd.get_i64();
            c.first_vertex = rd.get_i64();
            c.first_instance = rd.get_i64();
        }
        if (mask & kRecVertexBufs) {
            c.first_binding = static_cast<int>(rd.get_i64());
            c.vertex_buffers = rd.get_vec_u64();
            c.vertex_buffer_offsets = rd.get_vec_u64();
        }
        if (mask & kRecDescSets) {
            c.desc_layout = rd.get_u64();
            c.first_set = static_cast<int>(rd.get_i64());
            c.descriptor_sets = rd.get_vec_u64();
        }
        if (mask & kRecCopy) {
            c.src_buffer = rd.get_u64();
            c.copy_width = rd.get_i64();
            c.copy_height = rd.get_i64();
            c.copy_depth = rd.get_i64();
        }
        if (mask & kRecBarrierRange) {
            c.barrier_base_mip = static_cast<int>(rd.get_i64());
            c.barrier_level_count = static_cast<int>(rd.get_i64());
            c.barrier_base_layer = static_cast<int>(rd.get_i64());
            c.barrier_layer_count = static_cast<int>(rd.get_i64());
        }
        if (mask & kRecArgsU64) {
            c.args_u64 = rd.get_vec_u64();
        }
        if (mask & kRecArgsI64) {
            const std::uint64_t n = rd.get_u64();
            if (rd.fail || n > rd.remaining() / 8) {
                rd.fail = true;
            } else {
                c.args_i64.reserve(static_cast<std::size_t>(n));
                for (std::uint64_t k = 0; k < n; ++k) {
                    c.args_i64.push_back(rd.get_i64());
                }
            }
        }
        if (mask & kRecArgsF64) {
            const std::uint64_t n = rd.get_u64();
            if (rd.fail || n > rd.remaining() / 8) {
                rd.fail = true;
            } else {
                c.args_f64.reserve(static_cast<std::size_t>(n));
                for (std::uint64_t k = 0; k < n; ++k) {
                    c.args_f64.push_back(rd.get_f64());
                }
            }
        }
        if (mask & kRecArgsBlob) {
            c.args_blob = rd.get_str();
        }
        if (mask & kRecDeps2) {
            const std::uint64_t n = rd.get_u64();
            // min bytes per DependencyInfo2 (all-empty) = flags + 3 count words = 32.
            if (rd.fail || n > kMaxSync2Dependencies || n > rd.remaining() / 32) {
                rd.fail = true;
            } else {
                c.deps2.reserve(static_cast<std::size_t>(n));
                for (std::uint64_t k = 0; k < n && !rd.fail; ++k) {
                    c.deps2.push_back(wire_get_dep2(rd));
                }
            }
        }
        r.commands.push_back(std::move(c));
    }
    if (rd.fail) {
        err = "record command stream truncated or malformed";
        return RecordCommandBufferRequest{};
    }
    if (rd.pos != body.size()) {
        err = "record command stream carries trailing bytes";
        return RecordCommandBufferRequest{};
    }
    return r;
}

json::Value QueueSubmitRequest::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("queue", handle_value(queue));
    json::Array warr;
    for (const SubmitWait& w : waits) {
        json::Value wv = json::Value::make_object();
        wv.set("semaphore", handle_value(w.semaphore));
        wv.set("stage", json::Value(w.stage));
        wv.set("value", handle_value(w.value)); // timeline wait value
        warr.emplace_back(std::move(wv));
    }
    b.set("waits", json::Value(std::move(warr)));
    b.set("command_buffers", handle_array(command_buffers));
    b.set("signal_semaphores", handle_array(signal_semaphores));
    b.set("signal_values", handle_array(signal_values)); // timeline signal values
    b.set("fence", handle_value(fence));
    return b;
}

QueueSubmitRequest QueueSubmitRequest::from_body(const json::Value& body) {
    QueueSubmitRequest r;
    r.queue = get_handle(body, "queue");
    const json::Value* warr = body.find("waits");
    if (warr != nullptr && warr->is_array()) {
        for (const auto& e : warr->as_array()) {
            SubmitWait w;
            w.semaphore = get_handle(e, "semaphore");
            // Wait stage mask: wide + -1 sentinel so a missing / zero mask is rejected
            // (sync1 requires a non-zero pWaitDstStageMask entry per wait semaphore).
            w.stage = get_i64(e, "stage", -1);
            w.value = get_handle(e, "value"); // legacy -> 0 (binary, ignored)
            r.waits.push_back(w);
        }
    }
    r.command_buffers = get_handle_array(body, "command_buffers");
    r.signal_semaphores = get_handle_array(body, "signal_semaphores");
    r.signal_values = get_handle_array(body, "signal_values"); // legacy -> empty
    r.fence = get_handle(body, "fence");
    return r;
}

namespace {
json::Value sem_submit2_to_json(const SemaphoreSubmit2& s) {
    json::Value v = json::Value::make_object();
    v.set("semaphore", handle_value(s.semaphore));
    v.set("value", handle_value(s.value));
    v.set("stage", handle_value(s.stage_mask)); // 64-bit VkPipelineStageFlags2
    return v;
}
SemaphoreSubmit2 sem_submit2_from_json(const json::Value& e) {
    SemaphoreSubmit2 s;
    s.semaphore = get_handle(e, "semaphore");
    s.value = get_handle(e, "value");
    s.stage_mask = get_handle(e, "stage");
    return s;
}
} // namespace

json::Value QueueSubmit2Request::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("queue", handle_value(queue));
    json::Array sarr;
    for (const SubmitInfo2& si : submits) {
        json::Value sv = json::Value::make_object();
        json::Array waits_a;
        for (const SemaphoreSubmit2& w : si.waits) {
            waits_a.emplace_back(sem_submit2_to_json(w));
        }
        sv.set("waits", json::Value(std::move(waits_a)));
        sv.set("command_buffers", handle_array(si.command_buffers));
        json::Array signals_a;
        for (const SemaphoreSubmit2& s : si.signals) {
            signals_a.emplace_back(sem_submit2_to_json(s));
        }
        sv.set("signals", json::Value(std::move(signals_a)));
        sarr.emplace_back(std::move(sv));
    }
    b.set("submits", json::Value(std::move(sarr)));
    b.set("fence", handle_value(fence));
    return b;
}

QueueSubmit2Request QueueSubmit2Request::from_body(const json::Value& body) {
    QueueSubmit2Request r;
    r.queue = get_handle(body, "queue");
    const json::Value* sarr = body.find("submits");
    if (sarr != nullptr && sarr->is_array()) {
        for (const auto& e : sarr->as_array()) {
            SubmitInfo2 si;
            const json::Value* waits_a = e.find("waits");
            if (waits_a != nullptr && waits_a->is_array()) {
                for (const auto& w : waits_a->as_array()) {
                    si.waits.push_back(sem_submit2_from_json(w));
                }
            }
            si.command_buffers = get_handle_array(e, "command_buffers");
            const json::Value* signals_a = e.find("signals");
            if (signals_a != nullptr && signals_a->is_array()) {
                for (const auto& s : signals_a->as_array()) {
                    si.signals.push_back(sem_submit2_from_json(s));
                }
            }
            r.submits.push_back(std::move(si));
        }
    }
    r.fence = get_handle(body, "fence");
    return r;
}

json::Value QueueSubmitResponse::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("ok", json::Value(ok));
    b.set("reason", json::Value(reason));
    b.set("result", json::Value(result));
    return b;
}

QueueSubmitResponse QueueSubmitResponse::from_body(const json::Value& body) {
    QueueSubmitResponse r;
    r.ok = get_bool(body, "ok");
    r.reason = get_string(body, "reason");
    r.result = static_cast<int>(get_i64(body, "result", 0));
    return r;
}

// --- GL/zink: timeline-semaphore host ops
// -----------------------------------------------
json::Value WaitSemaphoresRequest::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("device", handle_value(device));
    b.set("semaphores", handle_array(semaphores));
    b.set("values", handle_array(values));
    b.set("timeout", handle_value(timeout));
    b.set("wait_any", json::Value(static_cast<double>(wait_any)));
    return b;
}

WaitSemaphoresRequest WaitSemaphoresRequest::from_body(const json::Value& body) {
    WaitSemaphoresRequest r;
    r.device = get_handle(body, "device");
    r.semaphores = get_handle_array(body, "semaphores");
    r.values = get_handle_array(body, "values");
    r.timeout = get_handle(body, "timeout");
    r.wait_any = get_int(body, "wait_any", 0);
    return r;
}

json::Value WaitSemaphoresResponse::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("ok", json::Value(ok));
    b.set("reason", json::Value(reason));
    b.set("result", json::Value(static_cast<double>(result)));
    return b;
}

WaitSemaphoresResponse WaitSemaphoresResponse::from_body(const json::Value& body) {
    WaitSemaphoresResponse r;
    r.ok = get_bool(body, "ok");
    r.reason = get_string(body, "reason");
    r.result = static_cast<int>(get_i64(body, "result", 0));
    return r;
}

json::Value SignalSemaphoreRequest::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("device", handle_value(device));
    b.set("semaphore", handle_value(semaphore));
    b.set("value", handle_value(value));
    return b;
}

SignalSemaphoreRequest SignalSemaphoreRequest::from_body(const json::Value& body) {
    SignalSemaphoreRequest r;
    r.device = get_handle(body, "device");
    r.semaphore = get_handle(body, "semaphore");
    r.value = get_handle(body, "value");
    return r;
}

json::Value GetSemaphoreCounterValueRequest::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("device", handle_value(device));
    b.set("semaphore", handle_value(semaphore));
    return b;
}

GetSemaphoreCounterValueRequest
GetSemaphoreCounterValueRequest::from_body(const json::Value& body) {
    GetSemaphoreCounterValueRequest r;
    r.device = get_handle(body, "device");
    r.semaphore = get_handle(body, "semaphore");
    return r;
}

json::Value GetSemaphoreCounterValueResponse::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("ok", json::Value(ok));
    b.set("reason", json::Value(reason));
    b.set("result", json::Value(result));
    b.set("value", handle_value(value));
    return b;
}

GetSemaphoreCounterValueResponse
GetSemaphoreCounterValueResponse::from_body(const json::Value& body) {
    GetSemaphoreCounterValueResponse r;
    r.ok = get_bool(body, "ok");
    r.reason = get_string(body, "reason");
    r.result = get_int(body, "result", 0);
    r.value = get_handle(body, "value");
    return r;
}

json::Value ResetFencesRequest::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("fences", handle_array(fences));
    return b;
}

ResetFencesRequest ResetFencesRequest::from_body(const json::Value& body) {
    ResetFencesRequest r;
    r.fences = get_handle_array(body, "fences");
    return r;
}

json::Value WaitForFencesRequest::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("fences", handle_array(fences));
    b.set("wait_all", json::Value(wait_all));
    b.set("timeout", handle_value(timeout)); // 64-bit ns, decimal string
    return b;
}

WaitForFencesRequest WaitForFencesRequest::from_body(const json::Value& body) {
    WaitForFencesRequest r;
    r.fences = get_handle_array(body, "fences");
    r.wait_all = get_bool(body, "wait_all");
    r.timeout = get_handle(body, "timeout"); // full 64-bit (UINT64_MAX = block)
    return r;
}

json::Value WaitForFencesResponse::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("ok", json::Value(ok));
    b.set("reason", json::Value(reason));
    b.set("result", json::Value(result));
    return b;
}

WaitForFencesResponse WaitForFencesResponse::from_body(const json::Value& body) {
    WaitForFencesResponse r;
    r.ok = get_bool(body, "ok");
    r.reason = get_string(body, "reason");
    r.result = static_cast<int>(get_i64(body, "result", 0));
    return r;
}

json::Value GetFenceStatusResponse::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("ok", json::Value(ok));
    b.set("reason", json::Value(reason));
    b.set("result", json::Value(result));
    return b;
}

GetFenceStatusResponse GetFenceStatusResponse::from_body(const json::Value& body) {
    GetFenceStatusResponse r;
    r.ok = get_bool(body, "ok");
    r.reason = get_string(body, "reason");
    r.result = static_cast<int>(get_i64(body, "result", 0));
    return r;
}

json::Value WaitIdleResponse::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("ok", json::Value(ok));
    b.set("reason", json::Value(reason));
    b.set("result", json::Value(result));
    return b;
}

WaitIdleResponse WaitIdleResponse::from_body(const json::Value& body) {
    WaitIdleResponse r;
    r.ok = get_bool(body, "ok");
    r.reason = get_string(body, "reason");
    r.result = static_cast<int>(get_i64(body, "result", 0));
    return r;
}

json::Value GetSurfaceCapabilitiesRequest::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("physical_device", handle_value(physical_device));
    b.set("surface", handle_value(surface));
    return b;
}

GetSurfaceCapabilitiesRequest GetSurfaceCapabilitiesRequest::from_body(const json::Value& body) {
    GetSurfaceCapabilitiesRequest r;
    r.physical_device = get_handle(body, "physical_device");
    r.surface = get_handle(body, "surface");
    return r;
}

json::Value GetSurfaceCapabilitiesResponse::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("ok", json::Value(ok));
    b.set("reason", json::Value(reason));
    // Every Vulkan u32 field rides wide (as a JSON number; all < 2^53 so exact) so a driver
    // value is never truncated. The current extent carries the 0xFFFFFFFF dynamic-extent
    // sentinel, which exceeds INT_MAX -- hence the wide carry throughout.
    b.set("min_image_count", json::Value(static_cast<long long>(min_image_count)));
    b.set("max_image_count", json::Value(static_cast<long long>(max_image_count)));
    b.set("current_extent_width", json::Value(static_cast<long long>(current_extent_width)));
    b.set("current_extent_height", json::Value(static_cast<long long>(current_extent_height)));
    b.set("min_image_extent_width", json::Value(static_cast<long long>(min_image_extent_width)));
    b.set("min_image_extent_height", json::Value(static_cast<long long>(min_image_extent_height)));
    b.set("max_image_extent_width", json::Value(static_cast<long long>(max_image_extent_width)));
    b.set("max_image_extent_height", json::Value(static_cast<long long>(max_image_extent_height)));
    b.set("max_image_array_layers", json::Value(static_cast<long long>(max_image_array_layers)));
    b.set("supported_transforms", json::Value(static_cast<long long>(supported_transforms)));
    b.set("current_transform", json::Value(static_cast<long long>(current_transform)));
    b.set("supported_composite_alpha",
          json::Value(static_cast<long long>(supported_composite_alpha)));
    b.set("supported_usage_flags", json::Value(static_cast<long long>(supported_usage_flags)));
    return b;
}

GetSurfaceCapabilitiesResponse GetSurfaceCapabilitiesResponse::from_body(const json::Value& body) {
    GetSurfaceCapabilitiesResponse r;
    r.ok = get_bool(body, "ok");
    r.reason = get_string(body, "reason");
    r.min_image_count = static_cast<std::uint64_t>(get_i64(body, "min_image_count", 0));
    r.max_image_count = static_cast<std::uint64_t>(get_i64(body, "max_image_count", 0));
    r.current_extent_width = static_cast<std::uint64_t>(get_i64(body, "current_extent_width", 0));
    r.current_extent_height = static_cast<std::uint64_t>(get_i64(body, "current_extent_height", 0));
    r.min_image_extent_width =
        static_cast<std::uint64_t>(get_i64(body, "min_image_extent_width", 0));
    r.min_image_extent_height =
        static_cast<std::uint64_t>(get_i64(body, "min_image_extent_height", 0));
    r.max_image_extent_width =
        static_cast<std::uint64_t>(get_i64(body, "max_image_extent_width", 0));
    r.max_image_extent_height =
        static_cast<std::uint64_t>(get_i64(body, "max_image_extent_height", 0));
    r.max_image_array_layers =
        static_cast<std::uint64_t>(get_i64(body, "max_image_array_layers", 0));
    r.supported_transforms = static_cast<std::uint64_t>(get_i64(body, "supported_transforms", 0));
    r.current_transform = static_cast<std::uint64_t>(get_i64(body, "current_transform", 0));
    r.supported_composite_alpha =
        static_cast<std::uint64_t>(get_i64(body, "supported_composite_alpha", 0));
    r.supported_usage_flags = static_cast<std::uint64_t>(get_i64(body, "supported_usage_flags", 0));
    return r;
}

json::Value GetSurfaceFormatsRequest::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("physical_device", handle_value(physical_device));
    b.set("surface", handle_value(surface));
    return b;
}

GetSurfaceFormatsRequest GetSurfaceFormatsRequest::from_body(const json::Value& body) {
    GetSurfaceFormatsRequest r;
    r.physical_device = get_handle(body, "physical_device");
    r.surface = get_handle(body, "surface");
    return r;
}

json::Value GetSurfaceFormatsResponse::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("ok", json::Value(ok));
    b.set("reason", json::Value(reason));
    json::Array arr;
    for (const SurfaceFormat& f : formats) {
        json::Value fv = json::Value::make_object();
        fv.set("format", json::Value(f.format));
        fv.set("color_space", json::Value(f.color_space));
        arr.emplace_back(std::move(fv));
    }
    b.set("formats", json::Value(std::move(arr)));
    return b;
}

GetSurfaceFormatsResponse GetSurfaceFormatsResponse::from_body(const json::Value& body) {
    GetSurfaceFormatsResponse r;
    r.ok = get_bool(body, "ok");
    r.reason = get_string(body, "reason");
    const json::Value* arr = body.find("formats");
    if (arr != nullptr && arr->is_array()) {
        for (const auto& e : arr->as_array()) {
            SurfaceFormat f;
            f.format = get_index(e, "format");
            f.color_space = get_index(e, "color_space");
            r.formats.push_back(f);
        }
    }
    return r;
}

json::Value GetSurfacePresentModesRequest::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("physical_device", handle_value(physical_device));
    b.set("surface", handle_value(surface));
    return b;
}

GetSurfacePresentModesRequest GetSurfacePresentModesRequest::from_body(const json::Value& body) {
    GetSurfacePresentModesRequest r;
    r.physical_device = get_handle(body, "physical_device");
    r.surface = get_handle(body, "surface");
    return r;
}

json::Value GetSurfacePresentModesResponse::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("ok", json::Value(ok));
    b.set("reason", json::Value(reason));
    json::Array arr;
    for (const int m : present_modes) {
        arr.emplace_back(json::Value(m));
    }
    b.set("present_modes", json::Value(std::move(arr)));
    return b;
}

GetSurfacePresentModesResponse GetSurfacePresentModesResponse::from_body(const json::Value& body) {
    GetSurfacePresentModesResponse r;
    r.ok = get_bool(body, "ok");
    r.reason = get_string(body, "reason");
    const json::Value* arr = body.find("present_modes");
    if (arr != nullptr && arr->is_array()) {
        for (const auto& e : arr->as_array()) {
            r.present_modes.push_back(e.is_integer() ? static_cast<int>(e.as_int()) : -1);
        }
    }
    return r;
}

json::Value GetSurfaceSupportRequest::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("physical_device", handle_value(physical_device));
    b.set("queue_family_index", json::Value(queue_family_index));
    b.set("surface", handle_value(surface));
    return b;
}

GetSurfaceSupportRequest GetSurfaceSupportRequest::from_body(const json::Value& body) {
    GetSurfaceSupportRequest r;
    r.physical_device = get_handle(body, "physical_device");
    r.queue_family_index = get_index(body, "queue_family_index"); // wide + -1 sentinel
    r.surface = get_handle(body, "surface");
    return r;
}

json::Value GetSurfaceSupportResponse::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("ok", json::Value(ok));
    b.set("reason", json::Value(reason));
    b.set("supported", json::Value(supported));
    return b;
}

GetSurfaceSupportResponse GetSurfaceSupportResponse::from_body(const json::Value& body) {
    GetSurfaceSupportResponse r;
    r.ok = get_bool(body, "ok");
    r.reason = get_string(body, "reason");
    r.supported = get_bool(body, "supported");
    return r;
}

// --- Draw surface serialization -------------------------

json::Value CreateImageViewRequest::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("image", handle_value(image));
    b.set("view_type", json::Value(view_type));
    b.set("format", json::Value(format));
    b.set("swizzle_r", json::Value(swizzle_r));
    b.set("swizzle_g", json::Value(swizzle_g));
    b.set("swizzle_b", json::Value(swizzle_b));
    b.set("swizzle_a", json::Value(swizzle_a));
    b.set("aspect", json::Value(aspect));
    b.set("base_mip_level", json::Value(base_mip_level));
    b.set("level_count", json::Value(level_count));
    b.set("base_array_layer", json::Value(base_array_layer));
    b.set("layer_count", json::Value(layer_count));
    b.set("view_usage", handle_value(view_usage)); // usage-narrowing pNext (0 = absent)
    return b;
}

CreateImageViewRequest CreateImageViewRequest::from_body(const json::Value& body) {
    CreateImageViewRequest r;
    r.image = get_handle(body, "image");
    r.view_type = get_index(body, "view_type");
    r.format = get_index(body, "format");
    r.swizzle_r = get_index(body, "swizzle_r");
    r.swizzle_g = get_index(body, "swizzle_g");
    r.swizzle_b = get_index(body, "swizzle_b");
    r.swizzle_a = get_index(body, "swizzle_a");
    r.aspect = get_index(body, "aspect");
    r.base_mip_level = get_index(body, "base_mip_level");
    r.level_count = get_index(body, "level_count");
    r.base_array_layer = get_index(body, "base_array_layer");
    r.layer_count = get_index(body, "layer_count");
    r.view_usage = get_handle(body, "view_usage"); // absent (old client) -> 0
    return r;
}

json::Value CreateImageViewResponse::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("ok", json::Value(ok));
    b.set("reason", json::Value(reason));
    b.set("image_view", handle_value(image_view));
    return b;
}

CreateImageViewResponse CreateImageViewResponse::from_body(const json::Value& body) {
    CreateImageViewResponse r;
    r.ok = get_bool(body, "ok");
    r.reason = get_string(body, "reason");
    r.image_view = get_handle(body, "image_view");
    return r;
}

json::Value CreateBufferViewRequest::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("buffer", handle_value(buffer));
    b.set("format", json::Value(format));
    b.set("offset", handle_value(offset)); // VkDeviceSize: wide (decimal string)
    b.set("range", handle_value(range));   // VkDeviceSize (may be VK_WHOLE_SIZE)
    return b;
}

CreateBufferViewRequest CreateBufferViewRequest::from_body(const json::Value& body) {
    CreateBufferViewRequest r;
    r.buffer = get_handle(body, "buffer");
    r.format = get_index(body, "format");
    r.offset = get_handle(body, "offset");
    r.range = get_handle(body, "range");
    return r;
}

json::Value CreateBufferViewResponse::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("ok", json::Value(ok));
    b.set("reason", json::Value(reason));
    b.set("buffer_view", handle_value(buffer_view));
    return b;
}

CreateBufferViewResponse CreateBufferViewResponse::from_body(const json::Value& body) {
    CreateBufferViewResponse r;
    r.ok = get_bool(body, "ok");
    r.reason = get_string(body, "reason");
    r.buffer_view = get_handle(body, "buffer_view");
    return r;
}

std::string CreateShaderModuleRequest::to_wire() const {
    json::Value h = json::Value::make_object();
    h.set("device", handle_value(device));
    h.set("code_size", handle_value(code_size)); // wide (decimal string)
    const std::string json = h.dump(0);
    std::string out(4, '\0');
    protocol::store_le32(static_cast<std::uint32_t>(json.size()),
                         reinterpret_cast<unsigned char*>(&out[0]));
    out += json;
    out += code;
    return out;
}

CreateShaderModuleRequest CreateShaderModuleRequest::from_wire(const std::string& body,
                                                               std::string& err) {
    err.clear();
    if (body.size() < 4) {
        err = "shader module body shorter than 4-byte length prefix";
        return CreateShaderModuleRequest{};
    }
    const std::uint32_t json_len =
        protocol::load_le32(reinterpret_cast<const unsigned char*>(body.data()));
    if (json_len > kMaxBinaryJsonHeaderBytes) {
        err = "shader module json header exceeds cap";
        return CreateShaderModuleRequest{};
    }
    if (static_cast<std::size_t>(4) + json_len > body.size()) {
        err = "shader module json header runs past end of body";
        return CreateShaderModuleRequest{};
    }
    const std::string json_text = body.substr(4, json_len);
    json::Value h;
    std::string jerr;
    if (!json::Value::try_parse(json_text, h, jerr)) {
        err = "shader module json header parse: " + jerr;
        return CreateShaderModuleRequest{};
    }
    CreateShaderModuleRequest r;
    r.device = get_handle(h, "device");
    r.code_size = get_handle(h, "code_size"); // full 64-bit (decimal string)
    // The 1 MiB SPIR-V cap is part of the binary-tail framing: reject an oversize-but-
    // well-framed body as a decoder fault here, before copying the tail -- not as a later
    // body-level ok=false. (The backend keeps the same guard as defense in depth.)
    if (r.code_size > kMaxShaderCodeBytes) {
        err = "shader module code_size exceeds the cap";
        return CreateShaderModuleRequest{};
    }
    const std::size_t tail_len = body.size() - 4 - json_len;
    // Exact (single blob): trailing bytes are a fault, not ignored. Comparing the real
    // tail length against the claimed code_size also bounds the copy (a huge code_size is rejected
    // here, never allocated).
    if (tail_len != r.code_size) {
        err = "shader module tail length does not match code_size";
        return CreateShaderModuleRequest{};
    }
    r.code.assign(body, static_cast<std::size_t>(4) + json_len, std::string::npos);
    return r;
}

json::Value CreateShaderModuleResponse::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("ok", json::Value(ok));
    b.set("reason", json::Value(reason));
    b.set("shader_module", handle_value(shader_module));
    return b;
}

CreateShaderModuleResponse CreateShaderModuleResponse::from_body(const json::Value& body) {
    CreateShaderModuleResponse r;
    r.ok = get_bool(body, "ok");
    r.reason = get_string(body, "reason");
    r.shader_module = get_handle(body, "shader_module");
    return r;
}

json::Value CreateRenderPassRequest::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("device", handle_value(device));
    json::Array atts;
    for (const AttachmentDesc& a : attachments) {
        json::Value av = json::Value::make_object();
        av.set("format", json::Value(a.format));
        av.set("samples", json::Value(a.samples));
        av.set("load_op", json::Value(a.load_op));
        av.set("store_op", json::Value(a.store_op));
        av.set("stencil_load_op", json::Value(a.stencil_load_op));
        av.set("stencil_store_op", json::Value(a.stencil_store_op));
        av.set("initial_layout", json::Value(a.initial_layout));
        av.set("final_layout", json::Value(a.final_layout));
        atts.emplace_back(std::move(av));
    }
    b.set("attachments", json::Value(std::move(atts)));
    b.set("color_attachment", json::Value(color_attachment));
    b.set("color_layout", json::Value(color_layout));
    b.set("depth_attachment", json::Value(depth_attachment)); // -1 = no depth
    b.set("depth_layout", json::Value(depth_layout));
    if (!color_refs.empty()) { // MRT: the full ordered ref array (-1 = UNUSED hole)
        json::Array refs;
        for (const ColorRefDesc& r : color_refs) {
            json::Value rv = json::Value::make_object();
            rv.set("attachment", json::Value(r.attachment));
            rv.set("layout", json::Value(r.layout));
            refs.emplace_back(std::move(rv));
        }
        b.set("color_refs", json::Value(std::move(refs)));
    }
    json::Array deps;
    for (const SubpassDependencyDesc& d : dependencies) {
        json::Value dv = json::Value::make_object();
        dv.set("src_subpass", json::Value(d.src_subpass));
        dv.set("dst_subpass", json::Value(d.dst_subpass));
        dv.set("src_stage", json::Value(d.src_stage));
        dv.set("dst_stage", json::Value(d.dst_stage));
        dv.set("src_access", json::Value(d.src_access));
        dv.set("dst_access", json::Value(d.dst_access));
        dv.set("dependency_flags", json::Value(d.dependency_flags));
        deps.emplace_back(std::move(dv));
    }
    b.set("dependencies", json::Value(std::move(deps)));
    if (view_mask != 0) { // absent -> 0 = a plain non-multiview pass (peer round-trips)
        b.set("view_mask", json::Value(view_mask));
    }
    return b;
}

CreateRenderPassRequest CreateRenderPassRequest::from_body(const json::Value& body) {
    CreateRenderPassRequest r;
    r.device = get_handle(body, "device");
    const json::Value* atts = body.find("attachments");
    if (atts != nullptr && atts->is_array()) {
        for (const auto& e : atts->as_array()) {
            AttachmentDesc a;
            a.format = get_index(e, "format");
            a.samples = get_index(e, "samples");
            a.load_op = get_index(e, "load_op");
            a.store_op = get_index(e, "store_op");
            a.stencil_load_op = get_index(e, "stencil_load_op");
            a.stencil_store_op = get_index(e, "stencil_store_op");
            a.initial_layout = get_index(e, "initial_layout");
            a.final_layout = get_index(e, "final_layout");
            r.attachments.push_back(a);
        }
    }
    r.color_attachment = get_index(body, "color_attachment");
    r.color_layout = get_index(body, "color_layout");
    // -1 (the get_index miss sentinel) means no depth attachment.
    r.depth_attachment = get_index(body, "depth_attachment");
    r.depth_layout = get_index(body, "depth_layout");
    const json::Value* refs = body.find("color_refs");
    if (refs != nullptr && refs->is_array()) { // MRT (absent -> empty = legacy single-color)
        for (const auto& e : refs->as_array()) {
            ColorRefDesc cr;
            cr.attachment = get_i64(e, "attachment", kColorRefUnused);
            cr.layout = get_int(e, "layout", -1);
            r.color_refs.push_back(cr);
        }
    }
    const json::Value* deps = body.find("dependencies");
    if (deps != nullptr && deps->is_array()) {
        for (const auto& e : deps->as_array()) {
            SubpassDependencyDesc d;
            d.src_subpass = get_i64(e, "src_subpass", -1);
            d.dst_subpass = get_i64(e, "dst_subpass", -1);
            d.src_stage = get_i64(e, "src_stage", -1);
            d.dst_stage = get_i64(e, "dst_stage", -1);
            d.src_access = get_i64(e, "src_access", -1);
            d.dst_access = get_i64(e, "dst_access", -1);
            d.dependency_flags = get_i64(e, "dependency_flags", -1);
            r.dependencies.push_back(d);
        }
    }
    r.view_mask = get_int(body, "view_mask", 0); // absent -> 0 = non-multiview
    return r;
}

json::Value CreateRenderPassResponse::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("ok", json::Value(ok));
    b.set("reason", json::Value(reason));
    b.set("render_pass", handle_value(render_pass));
    return b;
}

CreateRenderPassResponse CreateRenderPassResponse::from_body(const json::Value& body) {
    CreateRenderPassResponse r;
    r.ok = get_bool(body, "ok");
    r.reason = get_string(body, "reason");
    r.render_pass = get_handle(body, "render_pass");
    return r;
}

json::Value FramebufferAttachmentInfoDesc::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("flags", handle_value(flags));
    b.set("usage", handle_value(usage));
    b.set("width", json::Value(width));
    b.set("height", json::Value(height));
    b.set("layer_count", json::Value(layer_count));
    json::Array formats;
    for (const int format : view_formats) {
        formats.emplace_back(json::Value(format));
    }
    b.set("view_formats", json::Value(std::move(formats)));
    return b;
}

FramebufferAttachmentInfoDesc FramebufferAttachmentInfoDesc::from_body(const json::Value& body) {
    FramebufferAttachmentInfoDesc r;
    r.flags = get_handle(body, "flags");
    r.usage = get_handle(body, "usage");
    r.width = get_index(body, "width");
    r.height = get_index(body, "height");
    r.layer_count = get_index(body, "layer_count");
    const json::Value* formats = body.find("view_formats");
    if (formats != nullptr && formats->is_array()) {
        for (const auto& format : formats->as_array()) {
            r.view_formats.push_back(
                static_cast<int>(format.is_number() ? format.as_number() : 0.0));
        }
    }
    return r;
}

FramebufferExtentDesc
host_safe_framebuffer_extent(int requested_width, int requested_height,
                             const std::vector<FramebufferAttachmentInfoDesc>& attachment_infos) {
    FramebufferExtentDesc result{requested_width, requested_height};
    for (const auto& info : attachment_infos) {
        if (info.width > 0) {
            result.width = std::min(result.width, info.width);
        }
        if (info.height > 0) {
            result.height = std::min(result.height, info.height);
        }
    }
    return result;
}

int host_safe_render_extent(int offset, int requested_extent, int framebuffer_extent) {
    if (offset < 0 || requested_extent <= 0 || framebuffer_extent <= offset) {
        return 0;
    }
    return std::min(requested_extent, framebuffer_extent - offset);
}

json::Value CreateFramebufferRequest::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("device", handle_value(device));
    b.set("render_pass", handle_value(render_pass));
    b.set("image_view", handle_value(image_view));
    b.set("depth_image_view", handle_value(depth_image_view)); // 0 = none
    b.set("width", json::Value(width));
    b.set("height", json::Value(height));
    b.set("layers", json::Value(layers));
    b.set("imageless", json::Value(imageless));
    b.set("attachment_count", json::Value(attachment_count));
    json::Array infos;
    for (const auto& info : attachment_infos) {
        infos.emplace_back(info.to_body());
    }
    b.set("attachment_infos", json::Value(std::move(infos)));
    if (!attachment_views.empty()) { // MRT: positional concrete views
        b.set("attachment_views", handle_array(attachment_views));
    }
    return b;
}

CreateFramebufferRequest CreateFramebufferRequest::from_body(const json::Value& body) {
    CreateFramebufferRequest r;
    r.device = get_handle(body, "device");
    r.render_pass = get_handle(body, "render_pass");
    r.image_view = get_handle(body, "image_view");
    r.depth_image_view = get_handle(body, "depth_image_view");
    r.width = get_index(body, "width");
    r.height = get_index(body, "height");
    r.layers = get_index(body, "layers");
    r.imageless = get_bool(body, "imageless");
    r.attachment_count = get_index(body, "attachment_count");
    const json::Value* infos = body.find("attachment_infos");
    if (infos != nullptr && infos->is_array()) {
        for (const auto& info : infos->as_array()) {
            r.attachment_infos.push_back(FramebufferAttachmentInfoDesc::from_body(info));
        }
    }
    r.attachment_views = get_handle_array(body, "attachment_views"); // MRT (absent -> empty)
    return r;
}

json::Value CreateFramebufferResponse::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("ok", json::Value(ok));
    b.set("reason", json::Value(reason));
    b.set("framebuffer", handle_value(framebuffer));
    return b;
}

CreateFramebufferResponse CreateFramebufferResponse::from_body(const json::Value& body) {
    CreateFramebufferResponse r;
    r.ok = get_bool(body, "ok");
    r.reason = get_string(body, "reason");
    r.framebuffer = get_handle(body, "framebuffer");
    return r;
}

json::Value PushConstantRange::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("stage_flags", handle_value(stage_flags));
    b.set("offset", handle_value(offset));
    b.set("size", handle_value(size));
    return b;
}

PushConstantRange PushConstantRange::from_body(const json::Value& body) {
    PushConstantRange r;
    r.stage_flags = static_cast<std::uint32_t>(get_handle(body, "stage_flags"));
    r.offset = static_cast<std::uint32_t>(get_handle(body, "offset"));
    r.size = static_cast<std::uint32_t>(get_handle(body, "size"));
    return r;
}

json::Value CreatePipelineLayoutRequest::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("device", handle_value(device));
    b.set("set_layout_count", json::Value(set_layout_count));
    b.set("push_constant_range_count", json::Value(push_constant_range_count));
    b.set("set_layouts",
          handle_array(set_layouts)); // empty = the empty layout
    json::Array pcs;
    for (const auto& pc : push_constant_ranges) {
        pcs.emplace_back(pc.to_body());
    }
    b.set("push_constant_ranges",
          json::Value(std::move(pcs))); // empty = no push constants
    return b;
}

CreatePipelineLayoutRequest CreatePipelineLayoutRequest::from_body(const json::Value& body) {
    CreatePipelineLayoutRequest r;
    r.device = get_handle(body, "device");
    r.set_layout_count = get_index(body, "set_layout_count");
    r.push_constant_range_count = get_index(body, "push_constant_range_count");
    r.set_layouts = get_handle_array(body, "set_layouts");
    const json::Value* pcs = body.find("push_constant_ranges");
    if (pcs != nullptr && pcs->is_array()) {
        for (const auto& e : pcs->as_array()) {
            r.push_constant_ranges.push_back(PushConstantRange::from_body(e));
        }
    }
    return r;
}

json::Value CreatePipelineLayoutResponse::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("ok", json::Value(ok));
    b.set("reason", json::Value(reason));
    b.set("pipeline_layout", handle_value(pipeline_layout));
    return b;
}

CreatePipelineLayoutResponse CreatePipelineLayoutResponse::from_body(const json::Value& body) {
    CreatePipelineLayoutResponse r;
    r.ok = get_bool(body, "ok");
    r.reason = get_string(body, "reason");
    r.pipeline_layout = get_handle(body, "pipeline_layout");
    return r;
}

json::Value CreateGraphicsPipelinesRequest::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("device", handle_value(device));
    b.set("pipeline_cache", handle_value(pipeline_cache));
    json::Array st;
    for (const ShaderStageDesc& s : stages) {
        json::Value sv = json::Value::make_object();
        sv.set("stage", json::Value(s.stage));
        sv.set("module", handle_value(s.module));
        sv.set("entry", json::Value(s.entry));
        sv.set("stage_flags", json::Value(s.stage_flags));
        sv.set("required_subgroup_size", json::Value(s.required_subgroup_size));
        st.emplace_back(std::move(sv));
    }
    b.set("stages", json::Value(std::move(st)));
    b.set("topology", json::Value(topology));
    b.set("vertex_binding_count", json::Value(vertex_binding_count));
    b.set("vertex_attribute_count", json::Value(vertex_attribute_count));
    json::Array vbs;
    for (const VertexBindingDesc& vb : vertex_bindings) {
        json::Value v = json::Value::make_object();
        v.set("binding", json::Value(vb.binding));
        v.set("stride", json::Value(vb.stride));
        v.set("input_rate", json::Value(vb.input_rate));
        vbs.emplace_back(std::move(v));
    }
    b.set("vertex_bindings", json::Value(std::move(vbs)));
    json::Array vas;
    for (const VertexAttributeDesc& va : vertex_attributes) {
        json::Value v = json::Value::make_object();
        v.set("location", json::Value(va.location));
        v.set("binding", json::Value(va.binding));
        v.set("format", json::Value(va.format));
        v.set("offset", json::Value(va.offset));
        vas.emplace_back(std::move(v));
    }
    b.set("vertex_attributes", json::Value(std::move(vas)));
    b.set("vertex_divisor_present", json::Value(vertex_divisor_present));
    json::Array vds;
    for (const VertexBindingDivisorDesc& vd : vertex_binding_divisors) {
        json::Value v = json::Value::make_object();
        v.set("binding", json::Value(vd.binding));
        v.set("divisor", json::Value(vd.divisor));
        vds.emplace_back(std::move(v));
    }
    b.set("vertex_binding_divisors", json::Value(std::move(vds)));
    b.set("cull_mode", json::Value(cull_mode));
    b.set("front_face", json::Value(front_face));
    json::Array ds;
    for (const int d : dynamic_states) {
        ds.emplace_back(json::Value(d));
    }
    b.set("dynamic_states", json::Value(std::move(ds)));
    b.set("layout", handle_value(layout));
    b.set("render_pass", handle_value(render_pass));
    b.set("subpass", json::Value(subpass));
    // Dynamic rendering: the DR presence flag + the attachment formats it renders to
    // (present only when has_dynamic_rendering; an omitting peer leaves these at their defaults).
    b.set("has_dynamic_rendering", json::Value(has_dynamic_rendering));
    if (has_dynamic_rendering != 0) {
        b.set("dr_view_mask", json::Value(dr_view_mask));
        json::Array cfmts;
        for (const int f : dr_color_formats) {
            cfmts.emplace_back(json::Value(f));
        }
        b.set("dr_color_formats", json::Value(std::move(cfmts)));
        b.set("dr_depth_format", json::Value(dr_depth_format));
        b.set("dr_stencil_format", json::Value(dr_stencil_format));
    }
    // Depth-stencil state: presence flag + the carried test/write/compare.
    b.set("has_depth_stencil", json::Value(has_depth_stencil));
    b.set("depth_test_enable", json::Value(depth_test_enable));
    b.set("depth_write_enable", json::Value(depth_write_enable));
    b.set("depth_compare_op", json::Value(depth_compare_op));
    // GL/zink: the faithful rasterization / multisample / colour-blend / stencil state.
    b.set("polygon_mode", json::Value(polygon_mode));
    b.set("depth_clamp_enable", json::Value(depth_clamp_enable));
    b.set("rasterizer_discard_enable", json::Value(rasterizer_discard_enable));
    b.set("depth_bias_enable", json::Value(depth_bias_enable));
    b.set("depth_bias_constant", json::Value(depth_bias_constant));
    b.set("depth_bias_clamp", json::Value(depth_bias_clamp));
    b.set("depth_bias_slope", json::Value(depth_bias_slope));
    b.set("line_width", json::Value(line_width));
    // GL/zink: the depth-clip rasterization pNext (VK_EXT_depth_clip_enable).
    b.set("depth_clip_state_present", json::Value(depth_clip_state_present));
    b.set("depth_clip_enable", json::Value(depth_clip_enable));
    // GL/zink: the line-state rasterization pNext (VK_EXT_line_rasterization).
    b.set("line_state_present", json::Value(line_state_present));
    b.set("line_rasterization_mode", json::Value(line_rasterization_mode));
    b.set("line_stipple_enable", json::Value(line_stipple_enable));
    b.set("line_stipple_factor", json::Value(line_stipple_factor));
    b.set("line_stipple_pattern", json::Value(line_stipple_pattern));
    b.set("stream_state_present", json::Value(stream_state_present));
    b.set("rasterization_stream", json::Value(rasterization_stream));
    b.set("primitive_restart_enable", json::Value(primitive_restart_enable));
    b.set("rasterization_samples", json::Value(rasterization_samples));
    b.set("sample_shading_enable", json::Value(sample_shading_enable));
    b.set("sample_mask", json::Value(sample_mask));
    b.set("min_sample_shading", json::Value(min_sample_shading));
    b.set("alpha_to_coverage_enable", json::Value(alpha_to_coverage_enable));
    b.set("alpha_to_one_enable", json::Value(alpha_to_one_enable));
    b.set("viewport_count", json::Value(viewport_count));
    b.set("scissor_count", json::Value(scissor_count));
    b.set("patch_control_points", json::Value(patch_control_points));
    b.set("depth_bounds_test_enable", json::Value(depth_bounds_test_enable));
    b.set("stencil_test_enable", json::Value(stencil_test_enable));
    // GL/zink: the static stencil op state (front/back) + depth bounds (OpenCSG stencil).
    b.set("stencil_front_fail_op", json::Value(stencil_front_fail_op));
    b.set("stencil_front_pass_op", json::Value(stencil_front_pass_op));
    b.set("stencil_front_depth_fail_op", json::Value(stencil_front_depth_fail_op));
    b.set("stencil_front_compare_op", json::Value(stencil_front_compare_op));
    b.set("stencil_front_compare_mask", json::Value(stencil_front_compare_mask));
    b.set("stencil_front_write_mask", json::Value(stencil_front_write_mask));
    b.set("stencil_front_reference", json::Value(stencil_front_reference));
    b.set("stencil_back_fail_op", json::Value(stencil_back_fail_op));
    b.set("stencil_back_pass_op", json::Value(stencil_back_pass_op));
    b.set("stencil_back_depth_fail_op", json::Value(stencil_back_depth_fail_op));
    b.set("stencil_back_compare_op", json::Value(stencil_back_compare_op));
    b.set("stencil_back_compare_mask", json::Value(stencil_back_compare_mask));
    b.set("stencil_back_write_mask", json::Value(stencil_back_write_mask));
    b.set("stencil_back_reference", json::Value(stencil_back_reference));
    b.set("min_depth_bounds", json::Value(min_depth_bounds));
    b.set("max_depth_bounds", json::Value(max_depth_bounds));
    b.set("logic_op_enable", json::Value(logic_op_enable));
    b.set("logic_op", json::Value(logic_op));
    {
        json::Array bc;
        for (const double v : blend_constants) {
            bc.emplace_back(json::Value(v));
        }
        b.set("blend_constants", json::Value(std::move(bc)));
        json::Array cba;
        for (const ColorBlendAttachmentDesc& a : color_blend_attachments) {
            json::Value v = json::Value::make_object();
            v.set("blend_enable", json::Value(a.blend_enable));
            v.set("src_color_factor", json::Value(a.src_color_factor));
            v.set("dst_color_factor", json::Value(a.dst_color_factor));
            v.set("color_blend_op", json::Value(a.color_blend_op));
            v.set("src_alpha_factor", json::Value(a.src_alpha_factor));
            v.set("dst_alpha_factor", json::Value(a.dst_alpha_factor));
            v.set("alpha_blend_op", json::Value(a.alpha_blend_op));
            v.set("color_write_mask", json::Value(a.color_write_mask));
            cba.emplace_back(std::move(v));
        }
        b.set("color_blend_attachments", json::Value(std::move(cba)));
    }
    return b;
}

CreateGraphicsPipelinesRequest CreateGraphicsPipelinesRequest::from_body(const json::Value& body) {
    CreateGraphicsPipelinesRequest r;
    r.device = get_handle(body, "device");
    r.pipeline_cache = get_handle(body, "pipeline_cache");
    const json::Value* st = body.find("stages");
    if (st != nullptr && st->is_array()) {
        for (const auto& e : st->as_array()) {
            ShaderStageDesc s;
            s.stage = get_index(e, "stage");
            s.module = get_handle(e, "module");
            s.entry = get_string(e, "entry");
            s.stage_flags = get_i64(e, "stage_flags", 0);
            s.required_subgroup_size = get_int(e, "required_subgroup_size", 0);
            r.stages.push_back(std::move(s));
        }
    }
    r.topology = get_index(body, "topology");
    r.vertex_binding_count = get_index(body, "vertex_binding_count");
    r.vertex_attribute_count = get_index(body, "vertex_attribute_count");
    const json::Value* vbs = body.find("vertex_bindings");
    if (vbs != nullptr && vbs->is_array()) {
        for (const auto& e : vbs->as_array()) {
            VertexBindingDesc vb;
            vb.binding = get_index(e, "binding");
            vb.stride = get_i64(e, "stride", -1);
            vb.input_rate = get_index(e, "input_rate");
            r.vertex_bindings.push_back(vb);
        }
    }
    const json::Value* vas = body.find("vertex_attributes");
    if (vas != nullptr && vas->is_array()) {
        for (const auto& e : vas->as_array()) {
            VertexAttributeDesc va;
            va.location = get_index(e, "location");
            va.binding = get_index(e, "binding");
            va.format = get_index(e, "format");
            va.offset = get_i64(e, "offset", -1);
            r.vertex_attributes.push_back(va);
        }
    }
    r.vertex_divisor_present = get_int(body, "vertex_divisor_present", 0);
    const json::Value* vds = body.find("vertex_binding_divisors");
    if (vds != nullptr && vds->is_array()) {
        for (const auto& e : vds->as_array()) {
            VertexBindingDivisorDesc vd;
            vd.binding = get_index(e, "binding");
            vd.divisor = get_i64(e, "divisor", -1);
            r.vertex_binding_divisors.push_back(vd);
        }
    }
    r.cull_mode = get_index(body, "cull_mode");
    r.front_face = get_index(body, "front_face");
    const json::Value* ds = body.find("dynamic_states");
    if (ds != nullptr && ds->is_array()) {
        for (const auto& e : ds->as_array()) {
            r.dynamic_states.push_back(e.is_integer() ? static_cast<int>(e.as_int()) : -1);
        }
    }
    r.layout = get_handle(body, "layout");
    r.render_pass = get_handle(body, "render_pass");
    r.subpass = get_index(body, "subpass");
    // (dynamic rendering): absent in an omitting peer's body -> has_dynamic_rendering 0 (a
    // render-pass or compute pipeline, unchanged).
    r.has_dynamic_rendering = get_int(body, "has_dynamic_rendering", 0);
    r.dr_view_mask = get_int(body, "dr_view_mask", 0);
    const json::Value* cfmts = body.find("dr_color_formats");
    if (cfmts != nullptr && cfmts->is_array()) {
        for (const auto& e : cfmts->as_array()) {
            r.dr_color_formats.push_back(e.is_integer() ? static_cast<int>(e.as_int()) : 0);
        }
    }
    r.dr_depth_format = get_int(body, "dr_depth_format", 0);
    r.dr_stencil_format = get_int(body, "dr_stencil_format", 0);
    r.has_depth_stencil = get_bool(body, "has_depth_stencil");
    r.depth_test_enable = get_index(body, "depth_test_enable");
    r.depth_write_enable = get_index(body, "depth_write_enable");
    r.depth_compare_op = get_index(body, "depth_compare_op");
    // GL/zink: defaults match the vkcube-equivalent values for an omitting peer.
    r.polygon_mode = get_int(body, "polygon_mode", 0);
    r.depth_clamp_enable = get_int(body, "depth_clamp_enable", 0);
    r.rasterizer_discard_enable = get_int(body, "rasterizer_discard_enable", 0);
    r.depth_bias_enable = get_int(body, "depth_bias_enable", 0);
    r.depth_bias_constant = get_number(body, "depth_bias_constant", 0.0);
    r.depth_bias_clamp = get_number(body, "depth_bias_clamp", 0.0);
    r.depth_bias_slope = get_number(body, "depth_bias_slope", 0.0);
    r.line_width = get_number(body, "line_width", 1.0);
    // GL/zink: the depth-clip rasterization pNext (VK_EXT_depth_clip_enable). Absent in
    // an omitting peer's body -> present 0 (default-clip, no pNext rebuilt on the worker).
    r.depth_clip_state_present = get_int(body, "depth_clip_state_present", 0);
    r.depth_clip_enable = get_int(body, "depth_clip_enable", 1);
    // GL/zink: the line-state rasterization pNext (VK_EXT_line_rasterization).
    r.line_state_present = get_int(body, "line_state_present", 0);
    r.line_rasterization_mode = get_int(body, "line_rasterization_mode", 0);
    r.line_stipple_enable = get_int(body, "line_stipple_enable", 0);
    r.line_stipple_factor = get_int(body, "line_stipple_factor", 1);
    r.line_stipple_pattern = get_int(body, "line_stipple_pattern", 0);
    r.stream_state_present = get_int(body, "stream_state_present", 0);
    r.rasterization_stream = get_i64(body, "rasterization_stream", 0);
    r.primitive_restart_enable = get_int(body, "primitive_restart_enable", 0);
    r.rasterization_samples = get_int(body, "rasterization_samples", 1);
    r.sample_shading_enable = get_int(body, "sample_shading_enable", 0);
    r.sample_mask = get_i64(body, "sample_mask", -1);
    r.min_sample_shading = get_number(body, "min_sample_shading", 0.0);
    r.alpha_to_coverage_enable = get_int(body, "alpha_to_coverage_enable", 0);
    r.alpha_to_one_enable = get_int(body, "alpha_to_one_enable", 0);
    r.viewport_count = get_int(body, "viewport_count", 1);
    r.scissor_count = get_int(body, "scissor_count", 1);
    r.patch_control_points = get_int(body, "patch_control_points", 0);
    r.depth_bounds_test_enable = get_int(body, "depth_bounds_test_enable", 0);
    r.stencil_test_enable = get_int(body, "stencil_test_enable", 0);
    // GL/zink: the static stencil op state (front/back) + depth bounds. Omitting peers
    // default to an all-KEEP / compareOp ALWAYS no-op (stencil disabled -> these are ignored).
    r.stencil_front_fail_op = get_int(body, "stencil_front_fail_op", 0);
    r.stencil_front_pass_op = get_int(body, "stencil_front_pass_op", 0);
    r.stencil_front_depth_fail_op = get_int(body, "stencil_front_depth_fail_op", 0);
    r.stencil_front_compare_op = get_int(body, "stencil_front_compare_op", 7);
    r.stencil_front_compare_mask = get_int(body, "stencil_front_compare_mask", 0);
    r.stencil_front_write_mask = get_int(body, "stencil_front_write_mask", 0);
    r.stencil_front_reference = get_int(body, "stencil_front_reference", 0);
    r.stencil_back_fail_op = get_int(body, "stencil_back_fail_op", 0);
    r.stencil_back_pass_op = get_int(body, "stencil_back_pass_op", 0);
    r.stencil_back_depth_fail_op = get_int(body, "stencil_back_depth_fail_op", 0);
    r.stencil_back_compare_op = get_int(body, "stencil_back_compare_op", 7);
    r.stencil_back_compare_mask = get_int(body, "stencil_back_compare_mask", 0);
    r.stencil_back_write_mask = get_int(body, "stencil_back_write_mask", 0);
    r.stencil_back_reference = get_int(body, "stencil_back_reference", 0);
    r.min_depth_bounds = get_number(body, "min_depth_bounds", 0.0);
    r.max_depth_bounds = get_number(body, "max_depth_bounds", 1.0);
    r.logic_op_enable = get_int(body, "logic_op_enable", 0);
    r.logic_op = get_int(body, "logic_op", 0);
    const json::Value* bc = body.find("blend_constants");
    if (bc != nullptr && bc->is_array()) {
        std::size_t i = 0;
        for (const auto& e : bc->as_array()) {
            if (i < 4) {
                r.blend_constants[i++] = e.is_number() ? e.as_number() : 0.0;
            }
        }
    }
    const json::Value* cba = body.find("color_blend_attachments");
    if (cba != nullptr && cba->is_array()) {
        for (const auto& e : cba->as_array()) {
            ColorBlendAttachmentDesc a;
            a.blend_enable = get_int(e, "blend_enable", 0);
            a.src_color_factor = get_int(e, "src_color_factor", 0);
            a.dst_color_factor = get_int(e, "dst_color_factor", 0);
            a.color_blend_op = get_int(e, "color_blend_op", 0);
            a.src_alpha_factor = get_int(e, "src_alpha_factor", 0);
            a.dst_alpha_factor = get_int(e, "dst_alpha_factor", 0);
            a.alpha_blend_op = get_int(e, "alpha_blend_op", 0);
            a.color_write_mask = get_int(e, "color_write_mask", 0xF);
            r.color_blend_attachments.push_back(a);
        }
    }
    return r;
}

json::Value CreateGraphicsPipelinesResponse::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("ok", json::Value(ok));
    b.set("reason", json::Value(reason));
    b.set("pipeline", handle_value(pipeline));
    return b;
}

CreateGraphicsPipelinesResponse
CreateGraphicsPipelinesResponse::from_body(const json::Value& body) {
    CreateGraphicsPipelinesResponse r;
    r.ok = get_bool(body, "ok");
    r.reason = get_string(body, "reason");
    r.pipeline = get_handle(body, "pipeline");
    return r;
}

// --- Compute pipelines --------------------------------------------

json::Value CreateComputePipelinesRequest::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("device", handle_value(device));
    b.set("pipeline_cache", handle_value(pipeline_cache));
    b.set("layout", handle_value(layout));
    b.set("shader_module", handle_value(shader_module));
    b.set("entry_point", json::Value(entry_point));
    b.set("stage_flags", json::Value(stage_flags));
    b.set("required_subgroup_size", json::Value(required_subgroup_size));
    return b;
}

CreateComputePipelinesRequest CreateComputePipelinesRequest::from_body(const json::Value& body) {
    CreateComputePipelinesRequest r;
    r.device = get_handle(body, "device");
    r.pipeline_cache = get_handle(body, "pipeline_cache");
    r.layout = get_handle(body, "layout");
    r.shader_module = get_handle(body, "shader_module");
    r.entry_point = get_string(body, "entry_point");
    r.stage_flags = get_i64(body, "stage_flags", 0);
    r.required_subgroup_size = get_int(body, "required_subgroup_size", 0);
    return r;
}

json::Value CreateComputePipelinesResponse::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("ok", json::Value(ok));
    b.set("reason", json::Value(reason));
    b.set("pipeline", handle_value(pipeline));
    return b;
}

CreateComputePipelinesResponse CreateComputePipelinesResponse::from_body(const json::Value& body) {
    CreateComputePipelinesResponse r;
    r.ok = get_bool(body, "ok");
    r.reason = get_string(body, "reason");
    r.pipeline = get_handle(body, "pipeline");
    return r;
}

// --- Host-visible memory + buffers serialization ------

json::Value GetPhysicalDeviceMemoryPropertiesRequest::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("physical_device", handle_value(physical_device));
    return b;
}

GetPhysicalDeviceMemoryPropertiesRequest
GetPhysicalDeviceMemoryPropertiesRequest::from_body(const json::Value& body) {
    GetPhysicalDeviceMemoryPropertiesRequest r;
    r.physical_device = get_handle(body, "physical_device");
    return r;
}

json::Value GetPhysicalDeviceMemoryPropertiesResponse::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("ok", json::Value(ok));
    b.set("reason", json::Value(reason));
    json::Array ts;
    for (const MemoryType& t : types) {
        json::Value tv = json::Value::make_object();
        tv.set("property_flags", handle_value(t.property_flags)); // wide (VkFlags)
        tv.set("heap_index", handle_value(t.heap_index));
        ts.emplace_back(std::move(tv));
    }
    b.set("types", json::Value(std::move(ts)));
    json::Array hs;
    for (const MemoryHeap& h : heaps) {
        json::Value hv = json::Value::make_object();
        hv.set("size", handle_value(h.size)); // VkDeviceSize
        hv.set("flags", handle_value(h.flags));
        hs.emplace_back(std::move(hv));
    }
    b.set("heaps", json::Value(std::move(hs)));
    return b;
}

GetPhysicalDeviceMemoryPropertiesResponse
GetPhysicalDeviceMemoryPropertiesResponse::from_body(const json::Value& body) {
    GetPhysicalDeviceMemoryPropertiesResponse r;
    r.ok = get_bool(body, "ok");
    r.reason = get_string(body, "reason");
    const json::Value* ts = body.find("types");
    if (ts != nullptr && ts->is_array()) {
        for (const auto& e : ts->as_array()) {
            MemoryType t;
            t.property_flags = get_handle(e, "property_flags");
            t.heap_index = get_handle(e, "heap_index");
            r.types.push_back(t);
        }
    }
    const json::Value* hs = body.find("heaps");
    if (hs != nullptr && hs->is_array()) {
        for (const auto& e : hs->as_array()) {
            MemoryHeap h;
            h.size = get_handle(e, "size");
            h.flags = get_handle(e, "flags");
            r.heaps.push_back(h);
        }
    }
    return r;
}

json::Value CreateBufferRequest::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("device", handle_value(device));
    b.set("size", handle_value(size));   // VkDeviceSize
    b.set("usage", handle_value(usage)); // VkBufferUsageFlags (wide)
    b.set("sharing_mode", json::Value(sharing_mode));
    return b;
}

CreateBufferRequest CreateBufferRequest::from_body(const json::Value& body) {
    CreateBufferRequest r;
    r.device = get_handle(body, "device");
    r.size = get_handle(body, "size");
    r.usage = get_handle(body, "usage");
    r.sharing_mode = get_index(body, "sharing_mode");
    return r;
}

json::Value CreateBufferResponse::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("ok", json::Value(ok));
    b.set("reason", json::Value(reason));
    b.set("buffer", handle_value(buffer));
    b.set("mem_size", handle_value(mem_size));
    b.set("mem_alignment", handle_value(mem_alignment));
    b.set("mem_type_bits", handle_value(mem_type_bits));
    return b;
}

CreateBufferResponse CreateBufferResponse::from_body(const json::Value& body) {
    CreateBufferResponse r;
    r.ok = get_bool(body, "ok");
    r.reason = get_string(body, "reason");
    r.buffer = get_handle(body, "buffer");
    r.mem_size = get_handle(body, "mem_size");
    r.mem_alignment = get_handle(body, "mem_alignment");
    r.mem_type_bits = get_handle(body, "mem_type_bits");
    return r;
}

json::Value BindBufferMemoryRequest::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("buffer", handle_value(buffer));
    b.set("memory", handle_value(memory));
    b.set("memory_offset", handle_value(memory_offset));
    return b;
}

BindBufferMemoryRequest BindBufferMemoryRequest::from_body(const json::Value& body) {
    BindBufferMemoryRequest r;
    r.buffer = get_handle(body, "buffer");
    r.memory = get_handle(body, "memory");
    r.memory_offset = get_handle(body, "memory_offset");
    return r;
}

std::string WriteMemoryRangesRequest::to_wire() const {
    json::Value h = json::Value::make_object();
    json::Array ups;
    for (const MemoryUpload& u : uploads) {
        json::Value uv = json::Value::make_object();
        uv.set("memory", handle_value(u.memory));
        json::Array rs;
        for (const MemoryUploadRange& r : u.ranges) {
            json::Value rv = json::Value::make_object();
            rv.set("offset", handle_value(r.offset));
            rv.set("size", handle_value(r.size));
            rs.emplace_back(std::move(rv));
        }
        uv.set("ranges", json::Value(std::move(rs)));
        ups.emplace_back(std::move(uv));
    }
    h.set("uploads", json::Value(std::move(ups)));
    const std::string json = h.dump(0);
    std::string out(4, '\0');
    protocol::store_le32(static_cast<std::uint32_t>(json.size()),
                         reinterpret_cast<unsigned char*>(&out[0]));
    out += json;
    out += payload;
    return out;
}

WriteMemoryRangesRequest WriteMemoryRangesRequest::from_wire(const std::string& body,
                                                             std::string& err) {
    err.clear();
    if (body.size() < 4) {
        err = "write_memory_ranges body shorter than 4-byte length prefix";
        return WriteMemoryRangesRequest{};
    }
    const std::uint32_t json_len =
        protocol::load_le32(reinterpret_cast<const unsigned char*>(body.data()));
    if (json_len > kMaxBinaryJsonHeaderBytes) {
        err = "write_memory_ranges json header exceeds cap";
        return WriteMemoryRangesRequest{};
    }
    if (static_cast<std::size_t>(4) + json_len > body.size()) {
        err = "write_memory_ranges json header runs past end of body";
        return WriteMemoryRangesRequest{};
    }
    json::Value h;
    std::string jerr;
    if (!json::Value::try_parse(body.substr(4, json_len), h, jerr)) {
        err = "write_memory_ranges json header parse: " + jerr;
        return WriteMemoryRangesRequest{};
    }
    WriteMemoryRangesRequest r;
    std::set<std::uint64_t> seen_memories;
    std::uint64_t total = 0; // sum of all range sizes (must equal the tail length)
    const json::Value* ups = h.find("uploads");
    if (ups != nullptr && ups->is_array()) {
        for (const auto& ue : ups->as_array()) {
            MemoryUpload u;
            u.memory = get_handle(ue, "memory");
            if (!seen_memories.insert(u.memory).second) {
                err = "write_memory_ranges has a duplicate memory handle";
                return WriteMemoryRangesRequest{};
            }
            std::uint64_t prev_end = 0;
            bool have_prev = false;
            const json::Value* rs = ue.find("ranges");
            if (rs != nullptr && rs->is_array()) {
                for (const auto& re : rs->as_array()) {
                    MemoryUploadRange rg;
                    rg.offset = get_handle(re, "offset");
                    rg.size = get_handle(re, "size");
                    if (rg.size == 0) {
                        err = "write_memory_ranges has a zero-size range";
                        return WriteMemoryRangesRequest{};
                    }
                    if (rg.offset > ~0ull - rg.size) {
                        err = "write_memory_ranges range offset+size overflows";
                        return WriteMemoryRangesRequest{};
                    }
                    if (have_prev && rg.offset < prev_end) {
                        err = "write_memory_ranges ranges not sorted/disjoint per allocation";
                        return WriteMemoryRangesRequest{};
                    }
                    prev_end = rg.offset + rg.size;
                    have_prev = true;
                    if (total > ~0ull - rg.size) {
                        err = "write_memory_ranges total size overflows";
                        return WriteMemoryRangesRequest{};
                    }
                    total += rg.size;
                    u.ranges.push_back(rg);
                }
            }
            r.uploads.push_back(std::move(u));
        }
    }
    const std::size_t tail_len = body.size() - 4 - json_len;
    if (total != tail_len) {
        err = "write_memory_ranges payload length does not match the summed range sizes";
        return WriteMemoryRangesRequest{};
    }
    if (tail_len > kMaxMemoryUploadBytes) {
        err = "write_memory_ranges payload exceeds the per-call cap";
        return WriteMemoryRangesRequest{};
    }
    // Full encoded frame must fit the transport cap: the RPC frame payload is the
    // 12-byte RPC header + this whole [u32 json_len][json][tail] body.
    if (kRpcHeaderBytes + 4 + static_cast<std::size_t>(json_len) + tail_len >
        protocol::kMaxFrameBytes) {
        err = "write_memory_ranges encoded frame exceeds the transport cap";
        return WriteMemoryRangesRequest{};
    }
    r.payload.assign(body, static_cast<std::size_t>(4) + json_len, std::string::npos);
    return r;
}

// --- Descriptor surface serialization -----------------

json::Value CreateDescriptorSetLayoutRequest::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("device", handle_value(device));
    b.set("layout_flags", json::Value(layout_flags));
    json::Array bs;
    for (const DescriptorSetLayoutBindingDesc& bd : bindings) {
        json::Value v = json::Value::make_object();
        v.set("binding", json::Value(bd.binding));
        v.set("descriptor_type", json::Value(bd.descriptor_type));
        v.set("descriptor_count", json::Value(bd.descriptor_count));
        v.set("stage_flags", json::Value(bd.stage_flags));
        v.set("binding_flags", json::Value(bd.binding_flags));
        bs.emplace_back(std::move(v));
    }
    b.set("bindings", json::Value(std::move(bs)));
    return b;
}

CreateDescriptorSetLayoutRequest
CreateDescriptorSetLayoutRequest::from_body(const json::Value& body) {
    CreateDescriptorSetLayoutRequest r;
    r.device = get_handle(body, "device");
    r.layout_flags = get_i64(body, "layout_flags", 0);
    const json::Value* bs = body.find("bindings");
    if (bs != nullptr && bs->is_array()) {
        for (const auto& e : bs->as_array()) {
            DescriptorSetLayoutBindingDesc bd;
            bd.binding = get_index(e, "binding");
            bd.descriptor_type = get_index(e, "descriptor_type");
            bd.descriptor_count = get_index(e, "descriptor_count");
            bd.stage_flags = get_i64(e, "stage_flags", -1);
            bd.binding_flags = get_i64(e, "binding_flags", 0);
            r.bindings.push_back(bd);
        }
    }
    return r;
}

json::Value CreateDescriptorSetLayoutResponse::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("ok", json::Value(ok));
    b.set("reason", json::Value(reason));
    b.set("set_layout", handle_value(set_layout));
    return b;
}

CreateDescriptorSetLayoutResponse
CreateDescriptorSetLayoutResponse::from_body(const json::Value& body) {
    CreateDescriptorSetLayoutResponse r;
    r.ok = get_bool(body, "ok");
    r.reason = get_string(body, "reason");
    r.set_layout = get_handle(body, "set_layout");
    return r;
}

json::Value GetDescriptorSetLayoutSupportResponse::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("ok", json::Value(ok));
    b.set("reason", json::Value(reason));
    b.set("supported", json::Value(supported));
    b.set("max_variable_descriptor_count", handle_value(max_variable_descriptor_count));
    return b;
}

GetDescriptorSetLayoutSupportResponse
GetDescriptorSetLayoutSupportResponse::from_body(const json::Value& body) {
    GetDescriptorSetLayoutSupportResponse r;
    r.ok = get_bool(body, "ok");
    r.reason = get_string(body, "reason");
    r.supported = get_int(body, "supported", 0);
    r.max_variable_descriptor_count = get_handle(body, "max_variable_descriptor_count");
    return r;
}

json::Value CreateDescriptorPoolRequest::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("device", handle_value(device));
    b.set("max_sets", json::Value(max_sets));
    b.set("flags", json::Value(flags));
    json::Array ps;
    for (const DescriptorPoolSizeDesc& s : pool_sizes) {
        json::Value v = json::Value::make_object();
        v.set("type", json::Value(s.type));
        v.set("descriptor_count", json::Value(s.descriptor_count));
        ps.emplace_back(std::move(v));
    }
    b.set("pool_sizes", json::Value(std::move(ps)));
    b.set("max_inline_uniform_block_bindings", json::Value(max_inline_uniform_block_bindings));
    return b;
}

CreateDescriptorPoolRequest CreateDescriptorPoolRequest::from_body(const json::Value& body) {
    CreateDescriptorPoolRequest r;
    r.device = get_handle(body, "device");
    r.max_sets = get_index(body, "max_sets");
    r.flags = get_i64(body, "flags", 0);
    const json::Value* ps = body.find("pool_sizes");
    if (ps != nullptr && ps->is_array()) {
        for (const auto& e : ps->as_array()) {
            DescriptorPoolSizeDesc s;
            s.type = get_index(e, "type");
            s.descriptor_count = get_index(e, "descriptor_count");
            r.pool_sizes.push_back(s);
        }
    }
    r.max_inline_uniform_block_bindings = get_int(body, "max_inline_uniform_block_bindings", 0);
    return r;
}

json::Value CreateDescriptorPoolResponse::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("ok", json::Value(ok));
    b.set("reason", json::Value(reason));
    b.set("pool", handle_value(pool));
    return b;
}

CreateDescriptorPoolResponse CreateDescriptorPoolResponse::from_body(const json::Value& body) {
    CreateDescriptorPoolResponse r;
    r.ok = get_bool(body, "ok");
    r.reason = get_string(body, "reason");
    r.pool = get_handle(body, "pool");
    return r;
}

json::Value AllocateDescriptorSetsRequest::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("device", handle_value(device));
    b.set("pool", handle_value(pool));
    b.set("set_layouts", handle_array(set_layouts));
    b.set("variable_counts", handle_array(variable_counts)); // empty = pNext absent
    return b;
}

AllocateDescriptorSetsRequest AllocateDescriptorSetsRequest::from_body(const json::Value& body) {
    AllocateDescriptorSetsRequest r;
    r.device = get_handle(body, "device");
    r.pool = get_handle(body, "pool");
    r.set_layouts = get_handle_array(body, "set_layouts");
    r.variable_counts = get_handle_array(body, "variable_counts"); // missing (legacy) -> empty
    return r;
}

json::Value AllocateDescriptorSetsResponse::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("ok", json::Value(ok));
    b.set("reason", json::Value(reason));
    b.set("descriptor_sets", handle_array(descriptor_sets));
    return b;
}

AllocateDescriptorSetsResponse AllocateDescriptorSetsResponse::from_body(const json::Value& body) {
    AllocateDescriptorSetsResponse r;
    r.ok = get_bool(body, "ok");
    r.reason = get_string(body, "reason");
    r.descriptor_sets = get_handle_array(body, "descriptor_sets");
    return r;
}

json::Value UpdateDescriptorSetsRequest::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("device", handle_value(device));
    json::Array ws;
    for (const WriteDescriptorSetDesc& w : writes) {
        json::Value wv = json::Value::make_object();
        wv.set("dst_set", handle_value(w.dst_set));
        wv.set("dst_binding", json::Value(w.dst_binding));
        wv.set("dst_array_element", json::Value(w.dst_array_element));
        wv.set("descriptor_type", json::Value(w.descriptor_type));
        wv.set("descriptor_count", json::Value(w.descriptor_count));
        json::Array bis;
        for (const DescriptorBufferInfoDesc& bi : w.buffer_infos) {
            json::Value v = json::Value::make_object();
            v.set("buffer", handle_value(bi.buffer));
            v.set("offset", handle_value(bi.offset)); // VkDeviceSize -> decimal string
            v.set("range", handle_value(bi.range));   // VK_WHOLE_SIZE round-trips as UINT64_MAX
            bis.emplace_back(std::move(v));
        }
        wv.set("buffer_infos", json::Value(std::move(bis)));
        // Image-info path: combined-image-sampler writes carry {sampler,
        // image_view, image_layout}. Exactly one of buffer_infos / image_infos is populated (keyed
        // by descriptor_type); both arrays serialize unconditionally (an empty one round-trips
        // empty).
        json::Array iis;
        for (const DescriptorImageInfoDesc& ii : w.image_infos) {
            json::Value v = json::Value::make_object();
            v.set("sampler", handle_value(ii.sampler));
            v.set("image_view", handle_value(ii.image_view));
            v.set("image_layout", json::Value(ii.image_layout));
            iis.emplace_back(std::move(v));
        }
        wv.set("image_infos", json::Value(std::move(iis)));
        // Texel-buffer path (zink): UNIFORM/STORAGE_TEXEL_BUFFER writes carry buffer-view
        // handles. Populated only for the texel types; round-trips empty otherwise.
        json::Array tbv;
        for (const std::uint64_t v : w.texel_buffer_views) {
            tbv.emplace_back(handle_value(v));
        }
        wv.set("texel_buffer_views", json::Value(std::move(tbv)));
        // Inline-uniform-block path (Vulkan 1.3 support): the write's raw bytes, hex on the wire.
        // Populated only for INLINE_UNIFORM_BLOCK; round-trips empty otherwise.
        wv.set("inline_data", json::Value(to_hex(w.inline_data)));
        ws.emplace_back(std::move(wv));
    }
    b.set("writes", json::Value(std::move(ws)));
    return b;
}

UpdateDescriptorSetsRequest UpdateDescriptorSetsRequest::from_body(const json::Value& body) {
    UpdateDescriptorSetsRequest r;
    r.device = get_handle(body, "device");
    const json::Value* ws = body.find("writes");
    if (ws != nullptr && ws->is_array()) {
        for (const auto& e : ws->as_array()) {
            WriteDescriptorSetDesc w;
            w.dst_set = get_handle(e, "dst_set");
            w.dst_binding = get_index(e, "dst_binding");
            w.dst_array_element = get_index(e, "dst_array_element");
            w.descriptor_type = get_index(e, "descriptor_type");
            w.descriptor_count = get_index(e, "descriptor_count");
            const json::Value* bis = e.find("buffer_infos");
            if (bis != nullptr && bis->is_array()) {
                for (const auto& be : bis->as_array()) {
                    DescriptorBufferInfoDesc bi;
                    bi.buffer = get_handle(be, "buffer");
                    bi.offset = get_handle(be, "offset"); // decimal-string u64
                    bi.range = get_handle(be, "range");   // VK_WHOLE_SIZE survives as UINT64_MAX
                    w.buffer_infos.push_back(bi);
                }
            }
            // Image-info path: combined-image-sampler {sampler, image_view,
            // image_layout}. image_layout wide (-1 sentinel, rejected by validation if not
            // SHADER_READ_ONLY_OPTIMAL); the array preserves length (a malformed handle -> 0).
            const json::Value* iis = e.find("image_infos");
            if (iis != nullptr && iis->is_array()) {
                for (const auto& ie : iis->as_array()) {
                    DescriptorImageInfoDesc ii;
                    ii.sampler = get_handle(ie, "sampler");
                    ii.image_view = get_handle(ie, "image_view");
                    ii.image_layout = get_index(ie, "image_layout");
                    w.image_infos.push_back(ii);
                }
            }
            w.texel_buffer_views = get_handle_array(e, "texel_buffer_views");
            w.inline_data = get_hex_blob(e, "inline_data");
            r.writes.push_back(std::move(w));
        }
    }
    return r;
}

// --- Textures + depth serialization -------------------

json::Value GetPhysicalDeviceFormatPropertiesRequest::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("physical_device", handle_value(physical_device));
    b.set("format", json::Value(format));
    return b;
}

GetPhysicalDeviceFormatPropertiesRequest
GetPhysicalDeviceFormatPropertiesRequest::from_body(const json::Value& body) {
    GetPhysicalDeviceFormatPropertiesRequest r;
    r.physical_device = get_handle(body, "physical_device");
    r.format = get_index(body, "format");
    return r;
}

json::Value GetPhysicalDeviceFormatPropertiesResponse::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("ok", json::Value(ok));
    b.set("reason", json::Value(reason));
    // VkFormatFeatureFlags are 32-bit; carry decimal-string u64 for uniformity with the other flag
    // fields (and so a future 64-bit VkFormatFeatureFlags2 path stays lossless).
    b.set("linear_tiling_features", handle_value(linear_tiling_features));
    b.set("optimal_tiling_features", handle_value(optimal_tiling_features));
    b.set("buffer_features", handle_value(buffer_features));
    // GL/zink: the 64-bit VkFormatProperties3 features (decimal-string u64).
    b.set("linear_tiling_features2", handle_value(linear_tiling_features2));
    b.set("optimal_tiling_features2", handle_value(optimal_tiling_features2));
    b.set("buffer_features2", handle_value(buffer_features2));
    return b;
}

GetPhysicalDeviceFormatPropertiesResponse
GetPhysicalDeviceFormatPropertiesResponse::from_body(const json::Value& body) {
    GetPhysicalDeviceFormatPropertiesResponse r;
    r.ok = get_bool(body, "ok");
    r.reason = get_string(body, "reason");
    r.linear_tiling_features = get_handle(body, "linear_tiling_features");
    r.optimal_tiling_features = get_handle(body, "optimal_tiling_features");
    r.buffer_features = get_handle(body, "buffer_features");
    // Additive (legacy responses lack these -> get_handle returns 0).
    r.linear_tiling_features2 = get_handle(body, "linear_tiling_features2");
    r.optimal_tiling_features2 = get_handle(body, "optimal_tiling_features2");
    r.buffer_features2 = get_handle(body, "buffer_features2");
    return r;
}

json::Value GetPhysicalDeviceFeaturesRequest::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("physical_device", handle_value(physical_device));
    return b;
}

GetPhysicalDeviceFeaturesRequest
GetPhysicalDeviceFeaturesRequest::from_body(const json::Value& body) {
    GetPhysicalDeviceFeaturesRequest r;
    r.physical_device = get_handle(body, "physical_device");
    return r;
}

json::Value GetPhysicalDeviceFeaturesResponse::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("ok", json::Value(ok));
    b.set("reason", json::Value(reason));
    b.set("feature_bits", handle_value(feature_bits));
    return b;
}

GetPhysicalDeviceFeaturesResponse
GetPhysicalDeviceFeaturesResponse::from_body(const json::Value& body) {
    GetPhysicalDeviceFeaturesResponse r;
    r.ok = get_bool(body, "ok");
    r.reason = get_string(body, "reason");
    r.feature_bits = get_handle(body, "feature_bits");
    return r;
}

json::Value GetPhysicalDeviceImageFormatPropertiesRequest::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("physical_device", handle_value(physical_device));
    b.set("format", json::Value(format));
    b.set("image_type", json::Value(image_type));
    b.set("tiling", json::Value(tiling));
    b.set("usage", handle_value(usage));
    b.set("flags", handle_value(flags));
    return b;
}

GetPhysicalDeviceImageFormatPropertiesRequest
GetPhysicalDeviceImageFormatPropertiesRequest::from_body(const json::Value& body) {
    GetPhysicalDeviceImageFormatPropertiesRequest r;
    r.physical_device = get_handle(body, "physical_device");
    r.format = get_index(body, "format");
    r.image_type = get_index(body, "image_type");
    r.tiling = get_index(body, "tiling");
    r.usage = get_handle(body, "usage");
    r.flags = get_handle(body, "flags");
    return r;
}

json::Value GetPhysicalDeviceImageFormatPropertiesResponse::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("ok", json::Value(ok));
    b.set("reason", json::Value(reason));
    b.set("result", json::Value(result));
    b.set("max_extent_width", handle_value(max_extent_width));
    b.set("max_extent_height", handle_value(max_extent_height));
    b.set("max_extent_depth", handle_value(max_extent_depth));
    b.set("max_mip_levels", handle_value(max_mip_levels));
    b.set("max_array_layers", handle_value(max_array_layers));
    b.set("sample_counts", handle_value(sample_counts));
    b.set("max_resource_size", handle_value(max_resource_size));
    return b;
}

GetPhysicalDeviceImageFormatPropertiesResponse
GetPhysicalDeviceImageFormatPropertiesResponse::from_body(const json::Value& body) {
    GetPhysicalDeviceImageFormatPropertiesResponse r;
    r.ok = get_bool(body, "ok");
    r.reason = get_string(body, "reason");
    // VkResult can be negative (errors); get_index would clamp -> use get_i64. Default -11
    // (VK_ERROR_FORMAT_NOT_SUPPORTED) fails closed on a missing/malformed field.
    r.result = static_cast<int>(get_i64(body, "result", -11));
    r.max_extent_width = static_cast<std::uint32_t>(get_handle(body, "max_extent_width"));
    r.max_extent_height = static_cast<std::uint32_t>(get_handle(body, "max_extent_height"));
    r.max_extent_depth = static_cast<std::uint32_t>(get_handle(body, "max_extent_depth"));
    r.max_mip_levels = static_cast<std::uint32_t>(get_handle(body, "max_mip_levels"));
    r.max_array_layers = static_cast<std::uint32_t>(get_handle(body, "max_array_layers"));
    r.sample_counts = static_cast<std::uint32_t>(get_handle(body, "sample_counts"));
    r.max_resource_size = get_handle(body, "max_resource_size");
    return r;
}

json::Value GetPhysicalDevicePropertiesRequest::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("physical_device", handle_value(physical_device));
    return b;
}

GetPhysicalDevicePropertiesRequest
GetPhysicalDevicePropertiesRequest::from_body(const json::Value& body) {
    GetPhysicalDevicePropertiesRequest r;
    r.physical_device = get_handle(body, "physical_device");
    return r;
}

json::Value GetPhysicalDevicePropertiesResponse::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("ok", json::Value(ok));
    b.set("reason", json::Value(reason));
    b.set("props_blob", json::Value(to_hex(props_blob)));
    return b;
}

GetPhysicalDevicePropertiesResponse
GetPhysicalDevicePropertiesResponse::from_body(const json::Value& body) {
    GetPhysicalDevicePropertiesResponse r;
    r.ok = get_bool(body, "ok");
    r.reason = get_string(body, "reason");
    r.props_blob = get_hex_blob(body, "props_blob");
    return r;
}

json::Value CapabilityChainEntry::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("s_type", handle_value(s_type));
    b.set("size", handle_value(size));
    b.set("blob", json::Value(to_hex(blob)));
    return b;
}

CapabilityChainEntry CapabilityChainEntry::from_body(const json::Value& body) {
    CapabilityChainEntry e;
    e.s_type = static_cast<std::uint32_t>(get_handle(body, "s_type"));
    e.size = static_cast<std::uint32_t>(get_handle(body, "size"));
    e.blob = get_hex_blob(body, "blob");
    return e;
}

json::Value GetPhysicalDeviceCapabilityChainRequest::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("physical_device", handle_value(physical_device));
    b.set("which", handle_value(which));
    json::Array arr;
    for (const auto& e : entries) {
        arr.emplace_back(e.to_body());
    }
    b.set("entries", json::Value(std::move(arr)));
    return b;
}

GetPhysicalDeviceCapabilityChainRequest
GetPhysicalDeviceCapabilityChainRequest::from_body(const json::Value& body) {
    GetPhysicalDeviceCapabilityChainRequest r;
    r.physical_device = get_handle(body, "physical_device");
    r.which = static_cast<std::uint32_t>(get_handle(body, "which"));
    const json::Value* arr = body.find("entries");
    if (arr != nullptr && arr->is_array()) {
        for (const auto& e : arr->as_array()) {
            r.entries.push_back(CapabilityChainEntry::from_body(e));
        }
    }
    return r;
}

json::Value GetPhysicalDeviceCapabilityChainResponse::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("ok", json::Value(ok));
    b.set("reason", json::Value(reason));
    json::Array arr;
    for (const auto& e : entries) {
        arr.emplace_back(e.to_body());
    }
    b.set("entries", json::Value(std::move(arr)));
    return b;
}

GetPhysicalDeviceCapabilityChainResponse
GetPhysicalDeviceCapabilityChainResponse::from_body(const json::Value& body) {
    GetPhysicalDeviceCapabilityChainResponse r;
    r.ok = get_bool(body, "ok");
    r.reason = get_string(body, "reason");
    const json::Value* arr = body.find("entries");
    if (arr != nullptr && arr->is_array()) {
        for (const auto& e : arr->as_array()) {
            r.entries.push_back(CapabilityChainEntry::from_body(e));
        }
    }
    return r;
}

json::Value CreateImageRequest::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("device", handle_value(device));
    b.set("image_type", json::Value(image_type));
    b.set("format", json::Value(format));
    b.set("width", json::Value(width));
    b.set("height", json::Value(height));
    b.set("depth", json::Value(depth));
    b.set("mip_levels", json::Value(mip_levels));
    b.set("array_layers", json::Value(array_layers));
    b.set("samples", json::Value(samples));
    b.set("tiling", json::Value(tiling));
    b.set("usage", handle_value(usage)); // VkImageUsageFlags wide
    b.set("sharing_mode", json::Value(sharing_mode));
    b.set("initial_layout", json::Value(initial_layout));
    b.set("image_flags", json::Value(image_flags));
    {
        json::Array vf;
        for (const int f : view_formats) {
            vf.emplace_back(json::Value(f));
        }
        b.set("view_formats", json::Value(std::move(vf)));
    }
    return b;
}

CreateImageRequest CreateImageRequest::from_body(const json::Value& body) {
    CreateImageRequest r;
    r.device = get_handle(body, "device");
    r.image_type = get_index(body, "image_type");
    r.format = get_index(body, "format");
    r.width = get_index(body, "width");
    r.height = get_index(body, "height");
    r.depth = get_index(body, "depth");
    r.mip_levels = get_index(body, "mip_levels");
    r.array_layers = get_index(body, "array_layers");
    r.samples = get_index(body, "samples");
    r.tiling = get_index(body, "tiling");
    r.usage = get_handle(body, "usage");
    r.sharing_mode = get_index(body, "sharing_mode");
    r.initial_layout = get_index(body, "initial_layout");
    r.image_flags = get_i64(body, "image_flags", 0);
    const json::Value* vf = body.find("view_formats");
    if (vf != nullptr && vf->is_array()) {
        for (const auto& e : vf->as_array()) {
            r.view_formats.push_back(static_cast<int>(e.is_number() ? e.as_number() : 0.0));
        }
    }
    return r;
}

json::Value CreateImageResponse::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("ok", json::Value(ok));
    b.set("reason", json::Value(reason));
    b.set("image", handle_value(image));
    b.set("mem_size", handle_value(mem_size));
    b.set("mem_alignment", handle_value(mem_alignment));
    b.set("mem_type_bits", handle_value(mem_type_bits));
    b.set("has_subresource_layout", json::Value(has_subresource_layout));
    b.set("sr_offset", handle_value(sr_offset));
    b.set("sr_size", handle_value(sr_size));
    b.set("sr_row_pitch", handle_value(sr_row_pitch));
    return b;
}

CreateImageResponse CreateImageResponse::from_body(const json::Value& body) {
    CreateImageResponse r;
    r.ok = get_bool(body, "ok");
    r.reason = get_string(body, "reason");
    r.image = get_handle(body, "image");
    r.mem_size = get_handle(body, "mem_size");
    r.mem_alignment = get_handle(body, "mem_alignment");
    r.mem_type_bits = get_handle(body, "mem_type_bits");
    r.has_subresource_layout = get_bool(body, "has_subresource_layout");
    r.sr_offset = get_handle(body, "sr_offset");
    r.sr_size = get_handle(body, "sr_size");
    r.sr_row_pitch = get_handle(body, "sr_row_pitch");
    return r;
}

json::Value BindImageMemoryRequest::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("image", handle_value(image));
    b.set("memory", handle_value(memory));
    b.set("memory_offset", handle_value(memory_offset));
    return b;
}

BindImageMemoryRequest BindImageMemoryRequest::from_body(const json::Value& body) {
    BindImageMemoryRequest r;
    r.image = get_handle(body, "image");
    r.memory = get_handle(body, "memory");
    r.memory_offset = get_handle(body, "memory_offset");
    return r;
}

// --- Sampler serialization  -------------------

json::Value CreateSamplerRequest::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("device", handle_value(device));
    b.set("mag_filter", json::Value(mag_filter));
    b.set("min_filter", json::Value(min_filter));
    b.set("mipmap_mode", json::Value(mipmap_mode));
    b.set("address_mode_u", json::Value(address_mode_u));
    b.set("address_mode_v", json::Value(address_mode_v));
    b.set("address_mode_w", json::Value(address_mode_w));
    b.set("anisotropy_enable", json::Value(anisotropy_enable));
    b.set("compare_enable", json::Value(compare_enable));
    // GL/zink: the rest of VkSamplerCreateInfo.
    b.set("mip_lod_bias", json::Value(mip_lod_bias));
    b.set("max_anisotropy", json::Value(max_anisotropy));
    b.set("compare_op", json::Value(compare_op));
    b.set("min_lod", json::Value(min_lod));
    b.set("max_lod", json::Value(max_lod));
    b.set("border_color", json::Value(border_color));
    b.set("unnormalized_coordinates", json::Value(unnormalized_coordinates));
    return b;
}

CreateSamplerRequest CreateSamplerRequest::from_body(const json::Value& body) {
    CreateSamplerRequest r;
    r.device = get_handle(body, "device");
    // Enum fields decode wide + -1 sentinel so a malformed value is rejected, not truncated.
    r.mag_filter = get_index(body, "mag_filter");
    r.min_filter = get_index(body, "min_filter");
    r.mipmap_mode = get_index(body, "mipmap_mode");
    r.address_mode_u = get_index(body, "address_mode_u");
    r.address_mode_v = get_index(body, "address_mode_v");
    r.address_mode_w = get_index(body, "address_mode_w");
    r.anisotropy_enable = get_index(body, "anisotropy_enable");
    r.compare_enable = get_index(body, "compare_enable");
    // additive (legacy requests default these). border_color/unnormalized default to 0.
    r.mip_lod_bias = get_double(body, "mip_lod_bias", 0.0);
    r.max_anisotropy = get_double(body, "max_anisotropy", 1.0);
    r.min_lod = get_double(body, "min_lod", 0.0);
    r.max_lod = get_double(body, "max_lod", 0.0);
    // Enum/bool fields: a missing/malformed value coerces to 0 (a valid default), not the -1
    // sentinel (which as a VkBool32/VkCompareOp would be garbage).
    const int co = get_index(body, "compare_op");
    r.compare_op = co < 0 ? 0 : co;
    const int bc = get_index(body, "border_color");
    r.border_color = bc < 0 ? 0 : bc;
    const int uc = get_index(body, "unnormalized_coordinates");
    r.unnormalized_coordinates = uc < 0 ? 0 : uc;
    return r;
}

json::Value CreateSamplerResponse::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("ok", json::Value(ok));
    b.set("reason", json::Value(reason));
    b.set("sampler", handle_value(sampler));
    return b;
}

CreateSamplerResponse CreateSamplerResponse::from_body(const json::Value& body) {
    CreateSamplerResponse r;
    r.ok = get_bool(body, "ok");
    r.reason = get_string(body, "reason");
    r.sampler = get_handle(body, "sampler");
    return r;
}

// --- Query pools (GL 3.3 / occlusion / xfb queries) -------------------------
json::Value CreateQueryPoolRequest::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("device", handle_value(device));
    b.set("query_type", json::Value(query_type));
    b.set("query_count", json::Value(static_cast<long long>(query_count)));
    b.set("pipeline_statistics", handle_value(pipeline_statistics));
    return b;
}

CreateQueryPoolRequest CreateQueryPoolRequest::from_body(const json::Value& body) {
    CreateQueryPoolRequest r;
    r.device = get_handle(body, "device");
    r.query_type = get_index(body, "query_type");
    r.query_count = static_cast<std::uint32_t>(get_i64(body, "query_count", 0));
    r.pipeline_statistics = get_handle(body, "pipeline_statistics");
    return r;
}

json::Value CreateQueryPoolResponse::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("ok", json::Value(ok));
    b.set("reason", json::Value(reason));
    b.set("query_pool", handle_value(query_pool));
    return b;
}

CreateQueryPoolResponse CreateQueryPoolResponse::from_body(const json::Value& body) {
    CreateQueryPoolResponse r;
    r.ok = get_bool(body, "ok");
    r.reason = get_string(body, "reason");
    r.query_pool = get_handle(body, "query_pool");
    return r;
}

json::Value GetQueryPoolResultsRequest::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("device", handle_value(device));
    b.set("query_pool", handle_value(query_pool));
    b.set("first_query", json::Value(static_cast<long long>(first_query)));
    b.set("query_count", json::Value(static_cast<long long>(query_count)));
    b.set("data_size", handle_value(data_size));
    b.set("stride", handle_value(stride));
    b.set("flags", handle_value(flags));
    return b;
}

GetQueryPoolResultsRequest GetQueryPoolResultsRequest::from_body(const json::Value& body) {
    GetQueryPoolResultsRequest r;
    r.device = get_handle(body, "device");
    r.query_pool = get_handle(body, "query_pool");
    r.first_query = static_cast<std::uint32_t>(get_i64(body, "first_query", 0));
    r.query_count = static_cast<std::uint32_t>(get_i64(body, "query_count", 0));
    r.data_size = get_handle(body, "data_size");
    r.stride = get_handle(body, "stride");
    r.flags = get_handle(body, "flags");
    return r;
}

json::Value GetQueryPoolResultsResponse::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("ok", json::Value(ok));
    b.set("reason", json::Value(reason));
    b.set("vk_result", json::Value(vk_result));
    b.set("data", json::Value(to_hex(data))); // raw result bytes, hex-encoded
    return b;
}

GetQueryPoolResultsResponse GetQueryPoolResultsResponse::from_body(const json::Value& body) {
    GetQueryPoolResultsResponse r;
    r.ok = get_bool(body, "ok");
    r.reason = get_string(body, "reason");
    // Preserve NEGATIVE VkResults: get_index clamps any negative to
    // -1, so VK_ERROR_DEVICE_LOST (-4) etc. would be reported as the wrong error. Decode wide and
    // range-check to int (the struct carries the real VkResult, not just SUCCESS/NOT_READY).
    const long long vr = get_i64(body, "vk_result", 0);
    r.vk_result = (vr >= INT_MIN && vr <= INT_MAX) ? static_cast<int>(vr) : 0;
    r.data = get_hex_blob(body, "data");
    return r;
}

json::Value ResetQueryPoolRequest::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("device", handle_value(device));
    b.set("query_pool", handle_value(query_pool));
    b.set("first_query", json::Value(static_cast<long long>(first_query)));
    b.set("query_count", json::Value(static_cast<long long>(query_count)));
    return b;
}

ResetQueryPoolRequest ResetQueryPoolRequest::from_body(const json::Value& body) {
    ResetQueryPoolRequest r;
    r.device = get_handle(body, "device");
    r.query_pool = get_handle(body, "query_pool");
    r.first_query = static_cast<std::uint32_t>(get_i64(body, "first_query", 0));
    r.query_count = static_cast<std::uint32_t>(get_i64(body, "query_count", 0));
    return r;
}

// --- Host-visible memory readback (worker -> ICD) ---------------------------
json::Value ReadMemoryRangesRequest::to_body() const {
    json::Value b = json::Value::make_object();
    json::Array rs;
    for (const MemoryUpload& u : reads) {
        json::Value uv = json::Value::make_object();
        uv.set("memory", handle_value(u.memory));
        json::Array ranges;
        for (const MemoryUploadRange& r : u.ranges) {
            json::Value rv = json::Value::make_object();
            rv.set("offset", handle_value(r.offset));
            rv.set("size", handle_value(r.size));
            ranges.emplace_back(std::move(rv));
        }
        uv.set("ranges", json::Value(std::move(ranges)));
        rs.emplace_back(std::move(uv));
    }
    b.set("reads", json::Value(std::move(rs)));
    b.set("raw_response", json::Value(raw_response)); //   (additive)
    return b;
}

ReadMemoryRangesRequest ReadMemoryRangesRequest::from_body(const json::Value& body) {
    ReadMemoryRangesRequest r;
    r.raw_response = get_bool(body, "raw_response"); // absent -> false (old client)
    const json::Value* rs = body.find("reads");
    if (rs != nullptr && rs->is_array()) {
        for (const auto& uv : rs->as_array()) {
            MemoryUpload u;
            u.memory = get_handle(uv, "memory");
            const json::Value* ranges = uv.find("ranges");
            if (ranges != nullptr && ranges->is_array()) {
                for (const auto& rv : ranges->as_array()) {
                    MemoryUploadRange mr;
                    mr.offset = get_handle(rv, "offset");
                    mr.size = get_handle(rv, "size");
                    u.ranges.push_back(mr);
                }
            }
            r.reads.push_back(std::move(u));
        }
    }
    return r;
}

json::Value ReadMemoryRangesResponse::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("ok", json::Value(ok));
    b.set("reason", json::Value(reason));
    b.set("payload", json::Value(to_hex(payload))); // raw bytes, hex-encoded
    return b;
}

ReadMemoryRangesResponse ReadMemoryRangesResponse::from_body(const json::Value& body) {
    ReadMemoryRangesResponse r;
    r.ok = get_bool(body, "ok");
    r.reason = get_string(body, "reason");
    r.payload = get_hex_blob(body, "payload");
    return r;
}

std::string ReadMemoryRangesResponse::to_wire() const {
    // [u32 json_len][{ok,reason} header][raw payload] -- the
    // WriteMemoryRangesRequest::to_wire framing in the response direction. The payload rides as
    // raw bytes: no hex doubling, no multi-MB JSON string to dump or parse.
    json::Value h = json::Value::make_object();
    h.set("ok", json::Value(ok));
    h.set("reason", json::Value(reason));
    const std::string json = h.dump(0);
    std::string out(4, '\0');
    protocol::store_le32(static_cast<std::uint32_t>(json.size()),
                         reinterpret_cast<unsigned char*>(&out[0]));
    out += json;
    out += payload;
    return out;
}

ReadMemoryRangesResponse ReadMemoryRangesResponse::from_wire(const std::string& body,
                                                             std::string& err) {
    err.clear();
    if (body.size() < 4) {
        err = "read_memory_ranges response shorter than 4-byte length prefix";
        return ReadMemoryRangesResponse{};
    }
    const std::uint32_t json_len =
        protocol::load_le32(reinterpret_cast<const unsigned char*>(body.data()));
    if (json_len > kMaxBinaryJsonHeaderBytes) {
        err = "read_memory_ranges response json header exceeds cap";
        return ReadMemoryRangesResponse{};
    }
    if (static_cast<std::size_t>(4) + json_len > body.size()) {
        err = "read_memory_ranges response json header runs past end of body";
        return ReadMemoryRangesResponse{};
    }
    json::Value h;
    std::string jerr;
    if (!json::Value::try_parse(body.substr(4, json_len), h, jerr)) {
        err = "read_memory_ranges response json header parse: " + jerr;
        return ReadMemoryRangesResponse{};
    }
    ReadMemoryRangesResponse r;
    r.ok = get_bool(h, "ok");
    r.reason = get_string(h, "reason");
    r.payload = body.substr(4 + json_len);
    return r;
}

MockVulkanBackend::MockVulkanBackend(const std::string& gpu_name,
                                     const display::DisplayLayout* display_layout) {
    if (display_layout != nullptr) {
        display_layout_ = *display_layout;
        has_display_layout_ = true;
    }
    for (const auto& d : protocol::probe_mocked()) {
        if (d.name == gpu_name) {
            device_ = d;
            have_device_ = true;
            return;
        }
    }
}

CapabilitiesResponse MockVulkanBackend::negotiate(const CapabilitiesRequest& req) {
    CapabilitiesResponse resp;
    if (!have_device_) {
        resp.ok = false;
        resp.reason = "no usable device selected for this worker";
        return resp;
    }
    if (req.requested_api_major < 1) {
        resp.ok = false;
        resp.reason = "requested API below the Vulkan 1.0 floor";
        return resp;
    }
    if (req.requested_api_minor < 0) {
        resp.ok = false;
        resp.reason = "requested API minor version is negative";
        return resp;
    }

    // negotiated = min(requested, worker-supported).
    int nmaj = kSupportedApiMajor;
    int nmin = kSupportedApiMinor;
    if (version_less(req.requested_api_major, req.requested_api_minor, kSupportedApiMajor,
                     kSupportedApiMinor)) {
        nmaj = req.requested_api_major;
        nmin = req.requested_api_minor;
    }

    resp.ok = true;
    resp.reason = "ok";
    resp.negotiated_api_major = nmaj;
    resp.negotiated_api_minor = nmin;
    resp.device = device_caps();
    return resp;
}

sidecar::SidecarNegotiateResponse
MockVulkanBackend::negotiate(const sidecar::SidecarNegotiateRequest& req) {
    // Sidecar plane: a stateless version handshake -- accept any client at or below
    // our sidecar protocol version, echo the version we speak. The mock and the real worker
    // answer identically (no host state involved), keeping mock == real on this plane.
    sidecar::SidecarNegotiateResponse resp;
    resp.protocol_version = sidecar::kSidecarProtocolVersion;
    if (req.protocol_version < 1) {
        resp.ok = false;
        resp.reason = "sidecar protocol version below the floor";
        return resp;
    }
    resp.ok = true;
    resp.reason = "ok";
    return resp;
}

sidecar::SidecarReadyResponse
MockVulkanBackend::sidecar_ready(const sidecar::SidecarReadyRequest& req) {
    // record that the sidecar has claimed the WM, probed caps, and finished its initial
    // root scan. The worker tolerates an early surface (it pends a born-correlated registry entry
    // regardless), so this is worker bookkeeping; the launcher gates the app on readiness.
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
            return resp;
        }
    }
    sidecar_ready_ = true;
    resp.ok = true;
    resp.reason = "ok";
    return resp;
}

void MockVulkanBackend::apply_mock_effect(const sidecar::RegistryEffect& eff) {
    // The mock has no real HWND; it tracks placeholder ids so its executor path is exercised and
    // the structural tests can assert it stays in lockstep with the registry's placeholder_count
    // (the real backend does the same against actual HWNDs). Caller holds backend_mutex_.
    switch (eff.kind) {
    case sidecar::RegistryEffect::Kind::CreatePlaceholder:
        mock_placeholders_.insert(eff.placeholder_id);
        break;
    case sidecar::RegistryEffect::Kind::DestroyPlaceholder:
    case sidecar::RegistryEffect::Kind::PromotePlaceholderToSurface:
        mock_placeholders_.erase(eff.placeholder_id);
        // the placeholder (and its chrome) is gone -- drop the synthetic pixel store too,
        // so a later DebugChromeState by XID cannot report stale pixels for a now-Surface/None
        // toplevel. Mirrors the real backend losing the window-thread DIB when the placeholder HWND
        // dies. Do the same for the synthetic cursor (the window the HCURSOR was bound to
        // is gone).
        mock_chrome_.erase(eff.xid);
        mock_cursors_.erase(eff.xid);
        // the placeholder/popup host is gone (destroyed, or promoted to a surface) ->
        // drop the synthetic geometry cell. For a Promote the host_apply_seq recording below
        // re-adds it at the inherited position (the new surface inherits the placeholder's place)
        // -- the erase here clears the OLD host's cell first.
        mock_geometry_.erase(eff.xid);
        break;
    case sidecar::RegistryEffect::Kind::ApplyGeometry:
    case sidecar::RegistryEffect::Kind::SetVisibility: // no host placement (visibility only)
    case sidecar::RegistryEffect::Kind::None:
        break;
    }
    // (mock == real for the authored position + seq): a GEOMETRY-bearing effect carrying
    // a host_apply_seq is a host placement -- a live move (ApplyGeometry), an initial placement
    // (CreatePlaceholder), or a promote/re-register (PromotePlaceholderToSurface / a surface-first
    // register). Record the last-applied geometry (the mock's stand-in for the real SetWindowPos),
    // coalesced on seq so an out-of-order apply cannot regress. Runs AFTER the kind switch so a
    // Promote (which erases above) re-records the inherited position. A SetVisibility effect ALSO
    // carries a host_apply_seq (the shared ordering counter) but is NOT a placement -- it must NOT
    // clobber the geometry cell to the effect's zero geometry, so it is excluded here.
    if (eff.kind != sidecar::RegistryEffect::Kind::SetVisibility && eff.host_apply_seq != 0 &&
        eff.host_apply_seq > mock_geometry_[eff.xid].seq) {
        MockGeometry& g = mock_geometry_[eff.xid];
        g.x = eff.geometry.x;
        g.y = eff.geometry.y;
        g.width = eff.geometry.width;
        g.height = eff.geometry.height;
        g.seq = eff.host_apply_seq;
    }
    // (mock == real): a sidecar-AUTHORED resize (ApplyGeometry with apply_size, from an
    // update_toplevel that changed w/h) marks the bound surface geometry-dirty -- the mock's
    // stand-in for the real WM_SIZE -> latch, so the app's next acquire/present returns
    // OUT_OF_DATE and it recreates the swapchain at the authored extent (which clears the latch).
    if (eff.kind == sidecar::RegistryEffect::Kind::ApplyGeometry && eff.apply_size) {
        const std::uint64_t surf = registry_.surface_for_xid(eff.xid);
        if (surf != 0) {
            const auto s = surfaces_.find(surf);
            if (s != surfaces_.end()) {
                s->second.geometry_dirty = true;
            }
        }
    }
}

sidecar::SidecarToplevelResponse
MockVulkanBackend::register_toplevel(const sidecar::SidecarRegisterToplevelRequest& req) {
    sidecar::SidecarToplevelResponse resp;
    resp.xid = req.xid;
    resp.ok = true;
    // Classifier (widened for the fullscreen class): the sidecar sends
    // override_redirect windows in exactly two CLASSIFIED shapes -- a popup (is_popup, owned by its
    // anchor) or a FULLSCREEN toplevel (is_popup=false: a root-covering window, SFML 2.5's
    // non-EWMH fullscreen -- the ExtremeTuxRacer class). Both are deliberate sidecar decisions, so
    // both register; the old refuse-all-non-popup gate predates the fullscreen classification and
    // stranded the fullscreen window on the create_surface 256x256 default host (mock == real at
    // the registry level; the mock has no real HWND, so its executor just tracks the placeholder).
    std::lock_guard<std::mutex> lk(backend_mutex_);
    const sidecar::RegistryEffect eff = registry_.register_toplevel(
        req.xid, req.generation, req.role, req.title, {req.x, req.y, req.width, req.height},
        req.is_popup, req.owner_xid, req.popup_kind);
    apply_mock_effect(eff);
    resp.applied = eff.applied;
    resp.representation = sidecar::representation_name(registry_.representation_for_xid(req.xid));
    resp.epoch = registry_.epoch_for_xid(req.xid);
    resp.reason = "ok";
    return resp;
}

sidecar::SidecarToplevelResponse
MockVulkanBackend::update_toplevel(const sidecar::SidecarUpdateToplevelRequest& req) {
    sidecar::SidecarToplevelResponse resp;
    resp.xid = req.xid;
    resp.ok = true;
    std::lock_guard<std::mutex> lk(backend_mutex_);
    const sidecar::RegistryEffect eff = registry_.update_toplevel(
        req.xid, req.generation, req.role, {req.x, req.y, req.width, req.height},
        static_cast<sidecar::ZOrder>(req.z_order));
    // a position/size/z-order change yields ApplyGeometry -> apply_mock_effect records
    // the last-applied geometry (the mock's stand-in for the real backend's live SetWindowPos). The
    // representation epoch is UNCHANGED by an update (a move keeps the same representation). The
    // z-order intent is recorded in the registry Entry (reported via DebugEnumWindows).
    apply_mock_effect(eff);
    resp.applied = eff.applied;
    resp.representation = sidecar::representation_name(registry_.representation_for_xid(req.xid));
    resp.epoch = registry_.epoch_for_xid(req.xid);
    resp.reason = "ok";
    return resp;
}

sidecar::SidecarToplevelResponse
MockVulkanBackend::unregister_toplevel(const sidecar::SidecarUnregisterToplevelRequest& req) {
    sidecar::SidecarToplevelResponse resp;
    resp.xid = req.xid;
    resp.ok = true;
    std::lock_guard<std::mutex> lk(backend_mutex_);
    const sidecar::RegistryEffect eff = registry_.unregister_toplevel(req.xid, req.generation);
    apply_mock_effect(eff);
    // (mock == real): an applied unregister ends the X lifecycle that owned the
    // cursor, so it does NOT cross into a re-register. The real worker clears the surviving surface
    // HWND's cursor (a placeholder HWND is destroyed with its HCURSOR by apply_mock_effect /
    // DestroyPlaceholder); the mock drops its synthetic store for the surface-live case the effect
    // does not cover. Idempotent for the placeholder case apply_mock_effect already erased.
    if (eff.applied) {
        mock_cursors_.erase(req.xid);
        // (mock == real): owner-teardown CASCADE -- drop this toplevel's
        // popups too (the real worker also destroys their host HWNDs off-lock). apply_mock_effect
        // on each DestroyPlaceholder erases the popup's fake placeholder id + its synthetic stores.
        // A no-op for an xid that owns no popups.
        for (const auto& pe : registry_.take_orphaned_popups(req.xid)) {
            apply_mock_effect(pe);
        }
    }
    resp.applied = eff.applied;
    resp.representation = sidecar::representation_name(registry_.representation_for_xid(req.xid));
    resp.epoch = registry_.epoch_for_xid(req.xid);
    resp.reason = "ok";
    return resp;
}

sidecar::SidecarToplevelResponse
MockVulkanBackend::set_visibility(const sidecar::SidecarSetVisibilityRequest& req) {
    sidecar::SidecarToplevelResponse resp;
    resp.xid = req.xid;
    resp.ok = true;
    std::lock_guard<std::mutex> lk(backend_mutex_);
    // (mock == real for the DECISION): the registry flips the live visibility
    // (generation-gated, strictly-newer-wins) and yields a SetVisibility effect. The mock has no
    // HWND, so there is no ShowWindow to apply -- the authored visibility_state lives in the
    // registry Entry and is reported via DebugEnumWindows. apply_mock_effect handles the
    // SetVisibility kind (a no-op for the mock geometry/placeholder bookkeeping; explicitly
    // excluded from the geometry-record block). CRUCIALLY the representation epoch is NOT bumped (a
    // hide is not a teardown), so the same entry + epoch survive a hide/show -- the boundary smoke
    // asserts exactly that.
    const sidecar::RegistryEffect eff = registry_.set_visibility(
        req.xid, req.generation, static_cast<sidecar::VisibilityState>(req.visibility_state));
    apply_mock_effect(eff);
    resp.applied = eff.applied;
    resp.representation = sidecar::representation_name(registry_.representation_for_xid(req.xid));
    resp.epoch = registry_.epoch_for_xid(req.xid);
    resp.reason = "ok";
    return resp;
}

void MockVulkanBackend::mock_paint_chrome(const sidecar::SidecarPaintChromeRequest& req) {
    // The mock's stand-in for the real DIB: keep a top-down BGRA8 buffer per xid (stride == w*4)
    // and composite the dirty rect into it at its offset, so DebugChromeState samples what was
    // painted. The decoder already validated all bounds, so the indexing here is safe. Caller holds
    // the mutex.
    MockChrome& c = mock_chrome_[req.xid];
    if (c.w != req.src_w || c.h != req.src_h) {
        c.w = req.src_w;
        c.h = req.src_h;
        c.bgra.assign(static_cast<std::size_t>(req.src_w) * req.src_h * 4, 0);
    }
    const std::size_t dst_stride = static_cast<std::size_t>(req.src_w) * 4;
    for (std::uint32_t row = 0; row < req.dirty_h; ++row) {
        const std::size_t src_off = static_cast<std::size_t>(row) * req.stride;
        const std::size_t dst_off = (static_cast<std::size_t>(req.dirty_y) + row) * dst_stride +
                                    static_cast<std::size_t>(req.dirty_x) * 4;
        std::memcpy(c.bgra.data() + dst_off,
                    reinterpret_cast<const unsigned char*>(req.pixels.data()) + src_off,
                    static_cast<std::size_t>(req.dirty_w) * 4);
    }
}

sidecar::SidecarPaintResponse
MockVulkanBackend::paint_chrome(const sidecar::SidecarPaintChromeRequest& req) {
    sidecar::SidecarPaintResponse resp;
    resp.xid = req.xid;
    resp.ok = true;
    resp.reason = "ok";
    std::lock_guard<std::mutex> lk(backend_mutex_);
    // The accept -> paint -> commit dance (the mock has no window thread, so paint never
    // blocks/fails: accept, composite into the synthetic buffer, then commit). A dropped paint (not
    // a Placeholder / stale generation / non-newer seq) does not touch the buffer or the registry.
    const sidecar::WindowRegistry::PlaceholderPaintDecision d =
        registry_.accept_placeholder_paint(req.xid, req.lifecycle_generation, req.seq);
    if (d.accepted) {
        mock_paint_chrome(req);
        registry_.commit_placeholder_paint(req.xid, req.lifecycle_generation, req.seq);
    }
    resp.applied = d.accepted;
    resp.representation = sidecar::representation_name(registry_.representation_for_xid(req.xid));
    resp.shown = registry_.placeholder_shown(req.xid);
    resp.last_seq = registry_.last_paint_seq(req.xid);
    return resp;
}

sidecar::SidecarDebugChromeStateResponse
MockVulkanBackend::debug_chrome_state(const sidecar::SidecarDebugChromeStateRequest& req) {
    sidecar::SidecarDebugChromeStateResponse resp;
    resp.xid = req.xid;
    resp.ok = true;
    resp.reason = "ok";
    std::lock_guard<std::mutex> lk(backend_mutex_);
    resp.representation = sidecar::representation_name(registry_.representation_for_xid(req.xid));
    resp.shown = registry_.placeholder_shown(req.xid);
    resp.last_seq = registry_.last_paint_seq(req.xid);
    const auto it = mock_chrome_.find(req.xid);
    if (it != mock_chrome_.end() && req.sample_x >= 0 && req.sample_y >= 0 &&
        static_cast<std::uint32_t>(req.sample_x) < it->second.w &&
        static_cast<std::uint32_t>(req.sample_y) < it->second.h) {
        const std::size_t off = (static_cast<std::size_t>(req.sample_y) * it->second.w +
                                 static_cast<std::size_t>(req.sample_x)) *
                                4;
        const unsigned char* p = it->second.bgra.data() + off;
        resp.pixel_bgra =
            static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
            (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
        resp.has_pixel = true;
    }
    return resp;
}

sidecar::SidecarPollInputResponse
MockVulkanBackend::poll_input(const sidecar::SidecarPollInputRequest& req) {
    sidecar::SidecarPollInputResponse resp;
    resp.ok = true;
    resp.reason = "ok";
    // Drain under the ring's OWN mutex (no backend_mutex_ held here). Then apply the exact-epoch
    // gate under the backend mutex (two locks, never nested): keep only events whose epoch matches
    // the xid's CURRENT representation epoch -- survives a resize (epoch unchanged), drops after
    // unregister/destroy (epoch 0) or an unregister+re-register / promote (a new epoch). mock ==
    // real: identical drain + gate logic.
    std::vector<sidecar::SidecarInputEvent> drained;
    std::uint64_t next_seq = req.since_seq;
    resp.dropped = input_queue_.drain(req.since_seq, drained, next_seq);
    resp.next_seq = next_seq;
    {
        std::lock_guard<std::mutex> lk(backend_mutex_);
        for (const auto& e : drained) {
            if (registry_.epoch_for_xid(e.xid) == e.epoch) {
                resp.events.push_back(e);
            }
        }
    }
    return resp;
}

sidecar::SidecarDebugEnqueueInputResponse
MockVulkanBackend::debug_enqueue_input(const sidecar::SidecarDebugEnqueueInputRequest& req) {
    sidecar::SidecarDebugEnqueueInputResponse resp;
    resp.xid = req.xid;
    resp.ok = true;
    resp.reason = "ok";
    int enqueued = 0;
    for (const auto& e : req.events) {
        // The request's xid/epoch are authoritative (the WndProc stamps the slot's, not the
        // per-event's); the ring re-stamps a fresh session seq. A 0 xid is ignored by the ring.
        input_queue_.enqueue(req.xid, req.epoch, e);
        ++enqueued;
    }
    resp.enqueued = enqueued;
    return resp;
}

sidecar::SidecarSetCursorResponse
MockVulkanBackend::set_cursor(const sidecar::SidecarSetCursorRequest& req) {
    sidecar::SidecarSetCursorResponse resp;
    resp.xid = req.xid;
    resp.ok = true;
    resp.reason = "ok";
    std::lock_guard<std::mutex> lk(backend_mutex_);
    // Exact-epoch + still-registered gate (mock == real): reject a cursor for
    // an xid that is no longer a registered toplevel or whose epoch has moved on, so a stale cursor
    // never installs across unregister/remap (incl. a surface-backed entry an unregister left
    // alive). The decoder already validated pixels.size() == width*height*4, so the store is exact.
    if (!registry_.toplevel_registered(req.xid) || req.epoch != registry_.epoch_for_xid(req.xid)) {
        resp.applied = false;
        return resp;
    }
    MockCursor& c = mock_cursors_[req.xid];
    c.w = req.width;
    c.h = req.height;
    c.xhot = req.xhot;
    c.yhot = req.yhot;
    c.bgra.assign(req.pixels.begin(), req.pixels.end());
    resp.applied = true;
    return resp;
}

sidecar::SidecarDebugCursorStateResponse
MockVulkanBackend::debug_cursor_state(const sidecar::SidecarDebugCursorStateRequest& req) {
    sidecar::SidecarDebugCursorStateResponse resp;
    resp.xid = req.xid;
    resp.ok = true;
    resp.reason = "ok";
    std::lock_guard<std::mutex> lk(backend_mutex_);
    const auto it = mock_cursors_.find(req.xid);
    if (it == mock_cursors_.end()) {
        return resp; // has_cursor = false
    }
    resp.has_cursor = true;
    resp.width = it->second.w;
    resp.height = it->second.h;
    resp.xhot = it->second.xhot;
    resp.yhot = it->second.yhot;
    if (req.sample_x >= 0 && req.sample_y >= 0 &&
        static_cast<std::uint32_t>(req.sample_x) < it->second.w &&
        static_cast<std::uint32_t>(req.sample_y) < it->second.h) {
        const std::size_t off = (static_cast<std::size_t>(req.sample_y) * it->second.w +
                                 static_cast<std::size_t>(req.sample_x)) *
                                4;
        const unsigned char* p = it->second.bgra.data() + off;
        resp.pixel_bgra =
            static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
            (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
        resp.has_pixel = true;
    }
    return resp;
}

sidecar::SidecarDebugEnumWindowsResponse
MockVulkanBackend::debug_enum_windows(const sidecar::SidecarDebugEnumWindowsRequest& req) {
    sidecar::SidecarDebugEnumWindowsResponse resp;
    resp.ok = true;
    resp.reason = "ok";
    std::lock_guard<std::mutex> lk(backend_mutex_);
    for (const auto& e : registry_.snapshot()) {
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
        w.is_popup = e.is_popup; // the worker-visible owner/z-order proof (mock == real)
        w.owner_xid = e.owner_xid;
        w.popup_kind = e.popup_kind;
        w.z_order = static_cast<std::uint32_t>(e.last_z_order); // last restack (pure registry)
        w.visibility_state = static_cast<std::uint32_t>(e.visibility_state); // authored visibility
        if (req.include_actual) {
            // The mock has no HWND, so the host-OBSERVED visibility MIRRORS
            // the authored intent -- mock == real for the lifecycle DECISION (the real backend
            // reads IsWindowVisible/IsIconic). A minimized Win32 window is STILL IsWindowVisible
            // (only SW_HIDE clears it), so Iconic reports host_visible=true AND host_iconic=true;
            // only Hidden is host_visible=false. The paint-eligibility
            // nuance of the real reveal predicate is a real-backend concern tested in
            // integration_real_backend.
            w.host_visible = e.visibility_state == sidecar::VisibilityState::Visible ||
                             e.visibility_state == sidecar::VisibilityState::Iconic;
            w.host_iconic = e.visibility_state == sidecar::VisibilityState::Iconic;
            // the mock has no HWND, so "actual" is the last-APPLIED move geometry (mock
            // == real for the authored x/y + seq). has_actual is set only once a move was applied
            // (a recorded cell), mirroring the real backend reporting a converged position; before
            // any move there is nothing to compare. The mock does not model the host client extent
            // or the Win32 frame (those are real-only), so they stay 0.
            const auto g = mock_geometry_.find(e.xid);
            if (g != mock_geometry_.end()) {
                w.has_actual = true;
                w.actual_x = g->second.x;
                w.actual_y = g->second.y;
                // the mock has no HWND, so "actual" extent is the last-applied authored size
                // (mock == real once the sidecar is the extent authority; the real backend reads
                // the realized client rect). 0 stays 0 until an authored resize records w/h.
                w.actual_width = g->second.width;
                w.actual_height = g->second.height;
                w.last_host_apply_seq = g->second.seq;
            }
        }
        resp.windows.push_back(std::move(w));
    }
    return resp;
}

sidecar::SidecarDebugCaptureWindowResponse
MockVulkanBackend::debug_capture_window(const sidecar::SidecarDebugCaptureWindowRequest& req) {
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
    std::lock_guard<std::mutex> lk(backend_mutex_);
    // Metadata is always reported (so a stale artifact is spottable even without a selector).
    const sidecar::Representation rep = registry_.representation_for_xid(req.xid);
    resp.representation = sidecar::representation_name(rep);
    resp.generation = registry_.generation_for_xid(req.xid);
    resp.epoch = registry_.epoch_for_xid(req.xid);
    resp.last_paint_seq = registry_.last_paint_seq(req.xid);
    resp.shown = registry_.placeholder_shown(req.xid);
    // absent: no live window representation (mock == real: real's hwnd_for_xid would be null).
    if (rep == sidecar::Representation::None) {
        resp.status = "absent";
        resp.reason = "no live representation for xid";
        return resp;
    }
    // Lifecycle selectors: 0 = do not check.
    if ((req.expected_epoch != 0 && req.expected_epoch != resp.epoch) ||
        (req.expected_lifecycle_generation != 0 &&
         req.expected_lifecycle_generation != resp.generation) ||
        (req.min_last_seq != 0 && resp.last_paint_seq < req.min_last_seq)) {
        resp.status = "mismatch";
        resp.reason = "lifecycle selector did not match current registry state";
        return resp;
    }
    // Read the synthetic source buffer for the requested layer (the mock's stand-in for the real
    // backend's DIB / HCURSOR).
    const unsigned char* src = nullptr;
    std::uint64_t w = 0, h = 0;
    if (is_chrome) {
        const auto it = mock_chrome_.find(req.xid);
        if (it != mock_chrome_.end() && !it->second.bgra.empty()) {
            src = it->second.bgra.data();
            w = it->second.w;
            h = it->second.h;
        }
    } else {
        const auto it = mock_cursors_.find(req.xid);
        if (it != mock_cursors_.end() && !it->second.bgra.empty()) {
            src = it->second.bgra.data();
            w = it->second.w;
            h = it->second.h;
            resp.xhot = it->second.xhot;
            resp.yhot = it->second.yhot;
        }
    }
    if (src == nullptr) {
        // The window exists but this layer has no content (chrome unpainted / no cursor / wrong
        // layer for the representation, e.g. chrome on a Surface).
        resp.status = "empty";
        resp.reason = "layer has no content";
        return resp;
    }
    const std::uint64_t stride = w * 4;
    if (h * stride > static_cast<std::uint64_t>(sidecar::kMaxCapturePayloadBytes)) {
        resp.status = "too_large";
        resp.reason = "source exceeds the frame cap";
        resp.width = static_cast<std::uint32_t>(w);
        resp.height = static_cast<std::uint32_t>(h);
        resp.stride = static_cast<std::uint32_t>(stride);
        resp.needed_bytes = h * stride;
        return resp;
    }
    resp.ok = true;
    resp.status = "ok";
    resp.width = static_cast<std::uint32_t>(w);
    resp.height = static_cast<std::uint32_t>(h);
    resp.stride = static_cast<std::uint32_t>(stride);
    resp.format = sidecar::kCaptureFormatBgra8;
    resp.pixels.assign(reinterpret_cast<const char*>(src), static_cast<std::size_t>(h * stride));
    return resp;
}

DeviceCaps MockVulkanBackend::device_caps() const {
    DeviceCaps caps;
    caps.device_name = device_.name;
    caps.vendor_id = device_.vendor_id;
    caps.device_id = device_.device_id;
    caps.device_type = protocol::to_string(device_.type);
    caps.max_color_attachments = 8; // MRT: mirrors the typical real host limit
    // Compute: the mock's one family is GRAPHICS|COMPUTE|TRANSFER (1|2|4) -- it
    // validates dispatch structurally, so it must advertise honestly like the real family.
    caps.queue_flags = 0x1 | 0x2 | 0x4;
    // Required-feature audit: the mock is a test ORACLE, so it must be as honest as the real
    // worker. It models a 1.3-capable host but serves the complete cumulative required matrix only
    // when every kRelayServes* gate is true. Otherwise it honestly reports 1.2, exactly like the
    // real worker's host_vk13_ready.
    caps.vk13_ready = kRelayServesFullVk13RequiredMatrix ? 1 : 0;
    // geometry-stream: the mock validates (shared rasterization_stream_ok) + models the stream
    // pNext, so it advertises the capability exactly like the real worker (mock == real).
    caps.rasterization_stream_state = 1;
    caps.core_indirect_draw = 1;
    return caps;
}

CreateInstanceResponse MockVulkanBackend::create_instance(const CreateInstanceRequest&) {
    CreateInstanceResponse resp;
    if (!have_device_) {
        resp.ok = false;
        resp.reason = "no usable device selected for this worker";
        return resp;
    }
    const std::uint64_t instance = next_handle_++;
    instances_.emplace(instance, Instance{}); // physical device minted on enumerate
    resp.ok = true;
    resp.reason = "ok";
    resp.instance = instance;
    return resp;
}

EnumeratePhysicalDevicesResponse
MockVulkanBackend::enumerate_physical_devices(const EnumeratePhysicalDevicesRequest& req) {
    EnumeratePhysicalDevicesResponse resp;
    const auto it = instances_.find(req.instance);
    if (it == instances_.end()) {
        resp.ok = false;
        resp.reason = "unknown instance handle";
        return resp;
    }
    // Mint the physical-device handle on first enumeration and cache it, so the
    // handle is stable across calls AND a create_device that never enumerated has
    // no valid handle to present (selection is enforced by enumeration, not by
    // guessing a monotonic id).
    if (it->second.physical_device == 0) {
        it->second.physical_device = next_handle_++;
    }
    PhysicalDeviceEntry entry;
    entry.handle = it->second.physical_device;
    entry.caps = device_caps();
    resp.devices.push_back(entry);
    resp.ok = true;
    resp.reason = "ok";
    return resp;
}

CreateDeviceResponse MockVulkanBackend::create_device(const CreateDeviceRequest& req) {
    CreateDeviceResponse resp;
    const auto it = instances_.find(req.instance);
    if (it == instances_.end()) {
        resp.ok = false;
        resp.reason = "unknown instance handle";
        return resp;
    }
    // Physical-device selection is enforced here: a device can only be created on
    // the physical device this instance enumerated (the worker's selected GPU).
    // Before enumeration the instance has no physical-device handle (0), so a
    // create_device that skipped enumerate -- even one guessing a handle value --
    // is rejected.
    if (req.physical_device == 0 || req.physical_device != it->second.physical_device) {
        resp.ok = false;
        resp.reason = "physical device not enumerated from this instance "
                      "(call enumerate_physical_devices before create_device)";
        return resp;
    }
    const std::uint64_t device = next_handle_++;
    it->second.devices.insert(device);
    Device dev;
    dev.instance = req.instance;
    // The spine creates one queue from one graphics family. The mock has no real
    // families, so it reports a deterministic (family 0, count 1) -- the same shape
    // the real backend reports -- and validates get_device_queue against it.
    dev.queue_family = 0;
    dev.queue_count = 1;
    // (GL/zink): remember the enabled extensions -- extension commands (transform feedback /
    // conditional rendering) validate against this set, mock == real.
    dev.enabled_exts.insert(req.enabled_extensions.begin(), req.enabled_extensions.end());
    dev.dynamic_rendering_feature_enabled = req.dynamic_rendering_feature_enabled != 0;
    dev.synchronization2_feature_enabled = req.synchronization2_feature_enabled != 0;
    // Required-feature audit (hardening): the enabled hostQueryReset
    // feature; reset_query_pool fails closed unless set (mock == real).
    dev.host_query_reset_feature_enabled = req.host_query_reset_feature_enabled != 0;
    // Required-feature audit: the enabled multiview feature; a viewMask render pass
    // fails closed unless set (mock == real). The mock is a Vulkan-free SCALAR oracle -- it records
    // the scalar the ICD derived from the enabled-feature chain; the WORKER is the site that
    // re-derives multiview from the forwarded chain and rejects a scalar/chain mismatch.
    dev.multiview_feature_enabled = req.multiview_feature_enabled != 0;
    dev.buffer_device_address_feature_enabled = req.buffer_device_address_feature_enabled != 0;
    dev.vertex_attr_divisor_feature_enabled = req.vertex_attr_divisor_feature_enabled != 0;
    dev.vertex_attr_zero_divisor_feature_enabled =
        req.vertex_attr_zero_divisor_feature_enabled != 0;
    // geometry-stream: the scalar oracle (the WORKER re-derives from the chain + agreement-checks;
    // the mock records the scalar, like multiview/divisor). `> 0` deliberately --
    // the -1 omitted sentinel (an older ICD has no scalar) reads as DISABLED here because the
    // mock does not interpret feature-chain blobs; the real worker is the derive-from-chain site.
    // The structural extension invariant IS mirrored (mock == real): a true scalar without
    // VK_EXT_transform_feedback is self-contradictory and rejects by the worker's exact reason.
    // (mirrored too): out-of-domain scalar values -- including the INVALID (-2) that a
    // forged/transmitted -1 decodes to -- reject by the worker's exact reason before anything else.
    if (req.geometry_streams_feature_enabled != kGeometryStreamsScalarOmitted &&
        req.geometry_streams_feature_enabled != 0 && req.geometry_streams_feature_enabled != 1) {
        resp.ok = false;
        resp.reason = "geometry_streams_feature_enabled must be 0 or 1 when present (omission "
                      "is wire-key absence, not a transmittable value)";
        it->second.devices.erase(device);
        return resp;
    }
    if (req.geometry_streams_feature_enabled > 0 &&
        dev.enabled_exts.count(kTransformFeedbackExtensionName) == 0) {
        resp.ok = false;
        resp.reason = "geometryStreams enabled without VK_EXT_transform_feedback";
        it->second.devices.erase(device);
        return resp;
    }
    dev.geometry_streams_feature_enabled = req.geometry_streams_feature_enabled > 0;
    dev.multi_draw_indirect_feature_enabled =
        (req.enabled_feature_bits & kFeatureMultiDrawIndirect) != 0;
    // descriptorIndexing: CreateDevice POLICY clamps to the SERVED buffer-only
    // subset -- a deferred-but-known bit (an image/texel UAB class) is rejected exactly like an
    // unknown one, so a skewed/custom client can never enable an unproven class past the ICD.
    // kDIFeatureAllBits stays the serialization universe and the pure validator's test surface;
    // widening ServedBits (ICD + both backends together) is the intentional act when a class's
    // proof lands.
    if ((req.descriptor_indexing_feature_bits & ~kDIFeatureServedBits) != 0) {
        resp.ok = false;
        resp.reason = "descriptor_indexing_feature_bits outside the served set";
        it->second.devices.erase(device);
        return resp;
    }
    dev.descriptor_indexing_feature_bits = req.descriptor_indexing_feature_bits;
    // Required-feature audit (mock == real): mirror the real worker's
    // create_device re-check (`if (vk13_device_enabled && !host_vk13_ready) reject`). While the
    // relay-served gate is open (a required feature unserved -> device_caps reports 1.2), enabling
    // the vk13 DEVICE is fail-closed -- the same defense the ICD leans on so a lying client cannot
    // uncap 1.3. In normal operation the ICD never sets this flag while the device reports 1.2.
    if (req.vk13_device_enabled != 0 && !kRelayServesFullVk13RequiredMatrix) {
        resp.ok = false;
        resp.reason = "vk13 device requested but the relay does not serve the full 1.3 "
                      "required matrix";
        it->second.devices.erase(device);
        return resp;
    }
    // The served-set clamp for the vk13 scalar: only the served kVk13FeatureServedBits may
    // be enabled; features enabled WITHOUT the vk13 device are the memory-model bits only (mock ==
    // real).
    if ((req.vk13_feature_bits & ~kVk13FeatureServedBits) != 0) {
        resp.ok = false;
        resp.reason = "vk13_feature_bits outside the served set";
        it->second.devices.erase(device);
        return resp;
    }
    constexpr std::uint64_t kMemoryModelBits =
        kVk13FeatureVulkanMemoryModel | kVk13FeatureVulkanMemoryModelDeviceScope |
        kVk13FeatureVulkanMemoryModelAvailabilityVisibilityChains;
    if ((req.vk13_feature_bits & ~kMemoryModelBits) != 0 && req.vk13_device_enabled == 0) {
        resp.ok = false;
        resp.reason = "vk13 features enabled without the vk13 device (only the memory-model "
                      "bits are reported off it)";
        it->second.devices.erase(device);
        return resp;
    }
    dev.vk13_feature_bits = req.vk13_feature_bits;
    dev.vk13_device = req.vk13_device_enabled != 0;
    devices_.emplace(device, dev);
    resp.ok = true;
    resp.reason = "ok";
    resp.device = device;
    resp.queue_family_index = dev.queue_family;
    resp.queue_count = dev.queue_count;
    return resp;
}

StatusResponse MockVulkanBackend::destroy_device(const HandleRequest& req) {
    StatusResponse resp;
    const auto it = devices_.find(req.handle);
    if (it == devices_.end()) {
        resp.ok = false;
        resp.reason = "unknown device handle";
        return resp;
    }
    // Child objects (command pools; fences/semaphores/memory) must be destroyed
    // first, per Vulkan. Queues are retrieved, not created, so they die with the
    // device and do not block it.
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
    // Descriptor surface children block the device's destroy too.
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
    const auto inst = instances_.find(it->second.instance);
    if (inst != instances_.end()) {
        inst->second.devices.erase(req.handle);
    }
    devices_.erase(it);
    resp.ok = true;
    resp.reason = "ok";
    return resp;
}

StatusResponse MockVulkanBackend::destroy_instance(const HandleRequest& req) {
    StatusResponse resp;
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
    instances_.erase(it);
    resp.ok = true;
    resp.reason = "ok";
    return resp;
}

GetDeviceQueueResponse MockVulkanBackend::get_device_queue(const GetDeviceQueueRequest& req) {
    GetDeviceQueueResponse resp;
    const auto it = devices_.find(req.device);
    if (it == devices_.end()) {
        resp.ok = false;
        resp.reason = "unknown device handle";
        return resp;
    }
    // Only a (family, index) that create_device actually created is retrievable --
    // on the real backend, vkGetDeviceQueue with any other is undefined behavior,
    // so the mock enforces the same contract (the app must use the family/index
    // reported by create_device).
    const Device& dev = it->second;
    if (req.queue_family_index != dev.queue_family) {
        resp.ok = false;
        resp.reason = "queue family was not created on this device "
                      "(use the family reported by create_device)";
        return resp;
    }
    if (req.queue_index < 0 || req.queue_index >= dev.queue_count) {
        resp.ok = false;
        resp.reason = "queue index out of range for the created family";
        return resp;
    }
    // Queues are retrieved, not created: the same (family, index) returns a stable
    // handle for the device's lifetime.
    const std::uint64_t key = queue_key(req.queue_family_index, req.queue_index);
    auto& cache = it->second.queues;
    const auto cached = cache.find(key);
    if (cached != cache.end()) {
        resp.queue = cached->second;
    } else {
        resp.queue = next_handle_++;
        cache.emplace(key, resp.queue);
        queue_to_device_[resp.queue] = req.device; // for present's queue validation
    }
    resp.ok = true;
    resp.reason = "ok";
    return resp;
}

CreateCommandPoolResponse
MockVulkanBackend::create_command_pool(const CreateCommandPoolRequest& req) {
    CreateCommandPoolResponse resp;
    const auto it = devices_.find(req.device);
    if (it == devices_.end()) {
        resp.ok = false;
        resp.reason = "unknown device handle";
        return resp;
    }
    // Mirror the real backend: a pool targets the family create_device created
    // queues for (the only usable one). A wrong / malformed family is rejected.
    if (req.queue_family_index != it->second.queue_family) {
        resp.ok = false;
        resp.reason = "queue family was not created on this device "
                      "(use the family reported by create_device)";
        return resp;
    }
    const std::uint64_t pool = next_handle_++;
    Pool p;
    p.device = req.device;
    pools_.emplace(pool, p);
    it->second.pools.insert(pool);
    resp.ok = true;
    resp.reason = "ok";
    resp.command_pool = pool;
    return resp;
}

StatusResponse MockVulkanBackend::destroy_command_pool(const HandleRequest& req) {
    StatusResponse resp;
    const auto it = pools_.find(req.handle);
    if (it == pools_.end()) {
        resp.ok = false;
        resp.reason = "unknown command pool handle";
        return resp;
    }
    // Destroying a pool frees all command buffers allocated from it (Vulkan
    // semantics), so this never blocks on live buffers.
    for (const std::uint64_t b : it->second.buffers) {
        command_buffers_.erase(b);
    }
    const auto dev = devices_.find(it->second.device);
    if (dev != devices_.end()) {
        dev->second.pools.erase(req.handle);
    }
    pools_.erase(it);
    resp.ok = true;
    resp.reason = "ok";
    return resp;
}

AllocateCommandBuffersResponse
MockVulkanBackend::allocate_command_buffers(const AllocateCommandBuffersRequest& req) {
    AllocateCommandBuffersResponse resp;
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
    for (long long i = 0; i < req.count; ++i) {
        const std::uint64_t buffer = next_handle_++;
        it->second.buffers.insert(buffer);
        CmdBuffer cb;
        cb.pool = req.command_pool;
        cb.device = it->second.device;
        command_buffers_.emplace(buffer, std::move(cb));
        resp.command_buffers.push_back(buffer);
    }
    resp.ok = true;
    resp.reason = "ok";
    return resp;
}

StatusResponse MockVulkanBackend::free_command_buffers(const FreeCommandBuffersRequest& req) {
    StatusResponse resp;
    const auto it = pools_.find(req.command_pool);
    if (it == pools_.end()) {
        resp.ok = false;
        resp.reason = "unknown command pool handle";
        return resp;
    }
    // An empty list is rejected (Vulkan requires commandBufferCount > 0). This
    // also closes a malformed-request hole: a missing / null / wrong-typed
    // command_buffers field decodes to an empty vector, which must not succeed as
    // a no-op on a valid pool.
    if (req.command_buffers.empty()) {
        resp.ok = false;
        resp.reason = "no command buffers specified";
        return resp;
    }
    // Validate every buffer belongs to this pool, and reject a batch that repeats
    // a handle (the duplicate would otherwise pass validation while still live and
    // then double-free), before freeing any (atomic).
    std::set<std::uint64_t> seen;
    for (const std::uint64_t b : req.command_buffers) {
        const auto cb = command_buffers_.find(b);
        if (cb == command_buffers_.end() || cb->second.pool != req.command_pool) {
            resp.ok = false;
            resp.reason = "command buffer not allocated from this pool";
            return resp;
        }
        if (!seen.insert(b).second) {
            resp.ok = false;
            resp.reason = "duplicate command buffer in free request";
            return resp;
        }
    }
    for (const std::uint64_t b : req.command_buffers) {
        command_buffers_.erase(b);
        it->second.buffers.erase(b);
    }
    resp.ok = true;
    resp.reason = "ok";
    return resp;
}

std::uint64_t MockVulkanBackend::create_device_leaf(std::uint64_t device, LeafKind kind,
                                                    std::string& err) {
    const auto it = devices_.find(device);
    if (it == devices_.end()) {
        err = "unknown device handle";
        return 0;
    }
    const std::uint64_t handle = next_handle_++;
    leaves_.emplace(handle, Leaf{device, kind});
    it->second.leaves.insert(handle);
    return handle;
}

bool MockVulkanBackend::destroy_device_leaf(std::uint64_t handle, LeafKind kind, std::string& err) {
    const auto it = leaves_.find(handle);
    if (it == leaves_.end() || it->second.kind != kind) {
        // A wrong-kind handle (e.g. destroy_fence on a semaphore) is as invalid as
        // an unknown one -- the typed destroy must not free another object's handle.
        err = "unknown handle for this object type";
        return false;
    }
    const auto dev = devices_.find(it->second.device);
    if (dev != devices_.end()) {
        dev->second.leaves.erase(handle);
    }
    leaves_.erase(it);
    return true;
}

CreateFenceResponse MockVulkanBackend::create_fence(const CreateFenceRequest& req) {
    CreateFenceResponse resp;
    resp.fence = create_device_leaf(req.device, LeafKind::Fence, resp.reason);
    resp.ok = resp.fence != 0;
    if (resp.ok) {
        resp.reason = "ok";
        // VK_FENCE_CREATE_SIGNALED_BIT creates an already-signaled fence, so a
        // vkGetFenceStatus before any submit honestly reports VK_SUCCESS.
        if (req.signaled) {
            leaves_.at(resp.fence).is_signaled = true;
        }
    }
    return resp;
}

StatusResponse MockVulkanBackend::destroy_fence(const HandleRequest& req) {
    StatusResponse resp;
    resp.ok = destroy_device_leaf(req.handle, LeafKind::Fence, resp.reason);
    if (resp.ok) {
        resp.reason = "ok";
    }
    return resp;
}

CreateSemaphoreResponse MockVulkanBackend::create_semaphore(const CreateSemaphoreRequest& req) {
    CreateSemaphoreResponse resp;
    resp.semaphore = create_device_leaf(req.device, LeafKind::Semaphore, resp.reason);
    resp.ok = resp.semaphore != 0;
    if (resp.ok) {
        resp.reason = "ok";
        if (req.semaphore_type == 1) { // TIMELINE
            Leaf& leaf = leaves_.at(resp.semaphore);
            leaf.is_timeline = true;
            leaf.timeline_value = req.initial_value;
        }
    }
    return resp;
}

StatusResponse MockVulkanBackend::destroy_semaphore(const HandleRequest& req) {
    StatusResponse resp;
    resp.ok = destroy_device_leaf(req.handle, LeafKind::Semaphore, resp.reason);
    if (resp.ok) {
        resp.reason = "ok";
    }
    return resp;
}

GetFenceStatusResponse MockVulkanBackend::get_fence_status(const HandleRequest& req) {
    // fence-family completeness. VK_SUCCESS (signaled) / VK_NOT_READY (unsignaled) are
    // both NORMAL returns (ok=true, the app reads `result`); only an unknown/wrong-kind handle
    // faults.
    GetFenceStatusResponse resp;
    const auto it = leaves_.find(req.handle);
    if (it == leaves_.end() || it->second.kind != LeafKind::Fence) {
        resp.ok = false;
        resp.reason = "unknown handle for this object type";
        return resp;
    }
    resp.ok = true;
    resp.reason = "ok";
    resp.result = it->second.is_signaled ? kVkSuccess : kVkNotReady;
    return resp;
}

CreateEventResponse MockVulkanBackend::create_event(const CreateEventRequest& req) {
    CreateEventResponse resp;
    resp.event = create_device_leaf(req.device, LeafKind::Event, resp.reason);
    resp.ok = resp.event != 0;
    if (resp.ok) {
        resp.reason = "ok"; // a fresh event is unset (is_set defaults false = VK_EVENT_RESET)
    }
    return resp;
}

StatusResponse MockVulkanBackend::destroy_event(const HandleRequest& req) {
    StatusResponse resp;
    resp.ok = destroy_device_leaf(req.handle, LeafKind::Event, resp.reason);
    if (resp.ok) {
        resp.reason = "ok";
    }
    return resp;
}

GetEventStatusResponse MockVulkanBackend::get_event_status(const HandleRequest& req) {
    // VK_EVENT_SET (3) / VK_EVENT_RESET (4) are NORMAL returns (ok=true, app reads
    // `result`); an unknown/wrong-kind handle faults.
    GetEventStatusResponse resp;
    const auto it = leaves_.find(req.handle);
    if (it == leaves_.end() || it->second.kind != LeafKind::Event) {
        resp.ok = false;
        resp.reason = "unknown handle for this object type";
        return resp;
    }
    resp.ok = true;
    resp.reason = "ok";
    resp.result = it->second.is_set ? kVkEventSet : kVkEventReset;
    return resp;
}

StatusResponse MockVulkanBackend::set_event(const HandleRequest& req) {
    StatusResponse resp;
    const auto it = leaves_.find(req.handle);
    if (it == leaves_.end() || it->second.kind != LeafKind::Event) {
        resp.ok = false;
        resp.reason = "unknown handle for this object type";
        return resp;
    }
    it->second.is_set = true;
    resp.ok = true;
    resp.reason = "ok";
    return resp;
}

StatusResponse MockVulkanBackend::reset_event(const HandleRequest& req) {
    StatusResponse resp;
    const auto it = leaves_.find(req.handle);
    if (it == leaves_.end() || it->second.kind != LeafKind::Event) {
        resp.ok = false;
        resp.reason = "unknown handle for this object type";
        return resp;
    }
    it->second.is_set = false;
    resp.ok = true;
    resp.reason = "ok";
    return resp;
}

AllocateMemoryResponse MockVulkanBackend::allocate_memory(const AllocateMemoryRequest& req) {
    AllocateMemoryResponse resp;
    if (req.allocation_size == 0) {
        // Vulkan requires allocationSize > 0; also rejects a missing/malformed
        // size (which decodes to 0).
        resp.ok = false;
        resp.reason = "invalid allocation size";
        return resp;
    }
    // memoryTypeIndex is an unsigned Vulkan index. DeviceCaps reports a real memory-properties
    // table, so the mock is now a proper oracle: reject negative AND any index >= memoryTypeCount
    // (a validation-clean backend would). Carried wide so a narrowing decode can't sneak one
    // through.
    const std::vector<MemoryType> types = mock_memory_types();
    if (req.memory_type_index < 0 ||
        req.memory_type_index >= static_cast<long long>(types.size())) {
        resp.ok = false;
        resp.reason = "invalid memory type index";
        return resp;
    }
    // Memory-class reassertion (Option 1 Tier 1, vk13 audit): the worker independently
    // admits allocation of any advertised type EXCEPT protected (no protected queue/submit path)
    // and host-visible non-coherent (unmappable until Tier 2). A non-host-visible type
    // (propertyFlags==0 device-accessible, or DEVICE_LOCAL-only) is admitted -- it is never mapped.
    // Mirrors the ICD's icd_subset::memory_class_ok verbatim, re-asserted so a direct/hostile RPC
    // gets the same answer.
    {
        const std::uint64_t flags =
            types[static_cast<std::size_t>(req.memory_type_index)].property_flags;
        if ((flags & kMemoryPropertyProtected) != 0) {
            resp.ok = false;
            resp.reason = "protected memory is not supported (no protected queue/submit path)";
            return resp;
        }
        if ((flags & kMemoryPropertyHostVisible) != 0 &&
            (flags & kMemoryPropertyHostCoherent) == 0) {
            resp.ok = false;
            resp.reason = "host-visible memory without HOST_COHERENT is not yet supported (needs "
                          "Tier 2 flush/invalidate)";
            return resp;
        }
    }
    // (bufferDeviceAddress): the ONLY admitted allocate flag is DEVICE_ADDRESS, and only
    // when the device enabled the feature -- everything else (DEVICE_MASK, CAPTURE_REPLAY, unknown
    // future bits) is fail-closed by name, mock == real.
    if (req.allocate_flags != 0) {
        if ((req.allocate_flags & ~kMemoryAllocateDeviceAddressBit) != 0) {
            resp.ok = false;
            resp.reason = "memory allocate flags outside the supported set (only DEVICE_ADDRESS)";
            return resp;
        }
        const auto dev = devices_.find(req.device);
        if (dev == devices_.end() || !dev->second.buffer_device_address_feature_enabled) {
            resp.ok = false;
            resp.reason = "DEVICE_ADDRESS allocate flag requires the enabled bufferDeviceAddress "
                          "feature";
            return resp;
        }
    }
    resp.memory = create_device_leaf(req.device, LeafKind::Memory, resp.reason);
    resp.ok = resp.memory != 0;
    if (resp.ok) {
        MemoryObject mo;
        mo.device = req.device;
        mo.size = req.allocation_size;
        mo.type_index = static_cast<std::uint32_t>(req.memory_type_index);
        mo.property_flags = types[static_cast<std::size_t>(req.memory_type_index)].property_flags;
        mo.allocate_flags = req.allocate_flags;
        memory_objects_.emplace(resp.memory, std::move(mo));
        resp.reason = "ok";
    }
    return resp;
}

StatusResponse MockVulkanBackend::free_memory(const HandleRequest& req) {
    StatusResponse resp;
    resp.ok = destroy_device_leaf(req.handle, LeafKind::Memory, resp.reason);
    if (resp.ok) {
        memory_objects_.erase(req.handle); // discard the backing bytes (implicit unmap)
        resp.reason = "ok";
    }
    return resp;
}

std::vector<MemoryType> MockVulkanBackend::mock_memory_types() const {
    return {
        MemoryType{kMemoryPropertyDeviceLocal, 0},
        MemoryType{kMemoryPropertyHostVisible | kMemoryPropertyHostCoherent, 1},
        // A propertyFlags==0 device-accessible type (NVIDIA advertises exactly this as its type 0):
        // not host-visible, not device-local. It is a legitimate allocation class the app never
        // maps, so Option 1 Tier 1 admits it. Kept LAST so existing indices (0=DEVICE_LOCAL,
        // 1=HV|HC) and the tests that pick them by index are unchanged; heapIndex 1 is the
        // non-device-local heap.
        MemoryType{0, 1},
    };
}

std::vector<MemoryHeap> MockVulkanBackend::mock_memory_heaps() const {
    return {
        MemoryHeap{256ull * 1024 * 1024, 0x1}, // VK_MEMORY_HEAP_DEVICE_LOCAL_BIT
        MemoryHeap{256ull * 1024 * 1024, 0},
    };
}

GetPhysicalDeviceMemoryPropertiesResponse MockVulkanBackend::get_physical_device_memory_properties(
    const GetPhysicalDeviceMemoryPropertiesRequest& req) {
    GetPhysicalDeviceMemoryPropertiesResponse resp;
    // Validate the handle against enumerated instance state -- it must be a
    // physical device some instance enumerated, like the other physical-device-keyed paths.
    bool known_physical = false;
    for (const auto& inst : instances_) {
        if (inst.second.physical_device == req.physical_device && req.physical_device != 0) {
            known_physical = true;
            break;
        }
    }
    if (!known_physical) {
        resp.ok = false;
        resp.reason = "unknown physical device handle";
        return resp;
    }
    resp.ok = true;
    resp.reason = "ok";
    resp.types = mock_memory_types();
    resp.heaps = mock_memory_heaps();
    return resp;
}

CreateBufferResponse MockVulkanBackend::create_buffer(const CreateBufferRequest& req) {
    CreateBufferResponse resp;
    const auto dev = devices_.find(req.device);
    if (dev == devices_.end()) {
        resp.ok = false;
        resp.reason = "unknown device handle";
        return resp;
    }
    if (req.size == 0) {
        resp.ok = false;
        resp.reason = "buffer size must be > 0";
        return resp;
    }
    // Usage must be a nonzero subset of {VERTEX_BUFFER, UNIFORM_BUFFER, TRANSFER_SRC}.
    // bind_vertex_buffers tests the VERTEX bit; descriptor writes test the UNIFORM bit; and
    // copy_buffer_to_image uses TRANSFER_SRC for texture staging uploads.
    if (req.usage == 0 || (req.usage & ~kBufferUsageSubset) != 0) {
        resp.ok = false;
        resp.reason = "buffer usage must be a nonzero subset of {VERTEX_BUFFER, UNIFORM_BUFFER, "
                      "TRANSFER_SRC}";
        return resp;
    }
    if (req.sharing_mode != vk3c1::kSharingModeExclusive) {
        resp.ok = false;
        resp.reason = "buffer sharing mode must be EXCLUSIVE";
        return resp;
    }
    const std::uint64_t h = next_handle_++;
    // Honest-ish requirements: the requested size, a 16-byte alignment, and every reported type
    // compatible (the app picks the HOST_VISIBLE|HOST_COHERENT one). type_bits = a bit per type.
    // Stored on the buffer so bind enforces the same contract.
    Buffer buf;
    buf.device = req.device;
    buf.size = req.size;
    buf.usage = req.usage;
    buf.alignment = 16;
    buf.memory_type_bits = (1ull << mock_memory_types().size()) - 1ull;
    buffers_.emplace(h, buf);
    dev->second.buffers.insert(h);
    resp.ok = true;
    resp.reason = "ok";
    resp.buffer = h;
    resp.mem_size = req.size;
    resp.mem_alignment = buf.alignment;
    resp.mem_type_bits = buf.memory_type_bits;
    return resp;
}

StatusResponse MockVulkanBackend::destroy_buffer(const HandleRequest& req) {
    StatusResponse resp;
    const auto it = buffers_.find(req.handle);
    if (it == buffers_.end()) {
        resp.ok = false;
        resp.reason = "unknown buffer handle";
        return resp;
    }
    invalidate_cbs_referencing(req.handle); // a recorded bind_vertex_buffers baked this handle
    // (CB -> set -> buffer): consult the *current* set->buffer mapping
    // (the shared helper also covers sampler/image-view referents).
    dangle_sets_referencing(req.handle);
    const auto dev = devices_.find(it->second.device);
    if (dev != devices_.end()) {
        dev->second.buffers.erase(req.handle);
    }
    buffers_.erase(it);
    resp.ok = true;
    resp.reason = "ok";
    return resp;
}

StatusResponse MockVulkanBackend::bind_buffer_memory(const BindBufferMemoryRequest& req) {
    StatusResponse resp;
    const auto buf = buffers_.find(req.buffer);
    if (buf == buffers_.end()) {
        resp.ok = false;
        resp.reason = "unknown buffer handle";
        return resp;
    }
    const auto mem = memory_objects_.find(req.memory);
    if (mem == memory_objects_.end()) {
        resp.ok = false;
        resp.reason = "unknown memory handle";
        return resp;
    }
    if (buf->second.device != mem->second.device) {
        resp.ok = false;
        resp.reason = "buffer and memory are on different devices";
        return resp;
    }
    if (buf->second.bound_memory != 0) {
        resp.ok = false;
        resp.reason = "buffer is already bound to memory";
        return resp;
    }
    // The memory type must be one the buffer's requirements allow.
    if (buf->second.memory_type_bits != 0 &&
        (buf->second.memory_type_bits & (1ull << mem->second.type_index)) == 0) {
        resp.ok = false;
        resp.reason = "memory type is not in the buffer's supported memoryTypeBits";
        return resp;
    }
    // The bind offset must respect the buffer's required alignment.
    if (buf->second.alignment != 0 && (req.memory_offset % buf->second.alignment) != 0) {
        resp.ok = false;
        resp.reason = "bind offset does not satisfy the buffer's alignment";
        return resp;
    }
    // offset + buffer size must fit inside the allocation (overflow-safe).
    if (req.memory_offset > mem->second.size || req.memory_offset > ~0ull - buf->second.size ||
        req.memory_offset + buf->second.size > mem->second.size) {
        resp.ok = false;
        resp.reason = "bind range does not fit within the allocation";
        return resp;
    }
    // (bufferDeviceAddress): VUID-vkBindBufferMemory-bufferDeviceAddress-03339 -- a
    // SHADER_DEVICE_ADDRESS buffer may only bind to DEVICE_ADDRESS-allocated memory. A valid-usage
    // requirement (UB at the real driver), so reject before the bind, mock == real. This is the
    // exact trap the reference relay documented: forwarding the usage bit while dropping the
    // allocate flag "succeeds" until the address is dereferenced.
    if ((buf->second.usage & kBufferUsageShaderDeviceAddress) != 0 &&
        (mem->second.allocate_flags & kMemoryAllocateDeviceAddressBit) == 0) {
        resp.ok = false;
        resp.reason = "SHADER_DEVICE_ADDRESS buffer requires DEVICE_ADDRESS-allocated memory "
                      "(VUID 03339)";
        return resp;
    }
    buf->second.bound_memory = req.memory;
    buf->second.bound_offset = req.memory_offset;
    resp.ok = true;
    resp.reason = "ok";
    return resp;
}

GetBufferDeviceAddressResponse
MockVulkanBackend::get_buffer_device_address(const GetBufferDeviceAddressRequest& req) {
    // (bufferDeviceAddress): the mock has no real GPU (and never runs shaders), so it
    // returns a DETERMINISTIC per-buffer non-zero token -- stable across calls, distinct per
    // buffer -- exercising the wire + every gate the real backend enforces: enabled feature,
    // live buffer on that device, SHADER_DEVICE_ADDRESS usage, bound memory.
    GetBufferDeviceAddressResponse resp;
    const auto dev = devices_.find(req.device);
    if (dev == devices_.end()) {
        resp.ok = false;
        resp.reason = "unknown device handle";
        return resp;
    }
    if (!dev->second.buffer_device_address_feature_enabled) {
        resp.ok = false;
        resp.reason = "get_buffer_device_address requires the enabled bufferDeviceAddress feature";
        return resp;
    }
    const auto buf = buffers_.find(req.buffer);
    if (buf == buffers_.end() || buf->second.device != req.device) {
        resp.ok = false;
        resp.reason = "unknown buffer handle for this device";
        return resp;
    }
    if ((buf->second.usage & kBufferUsageShaderDeviceAddress) == 0) {
        resp.ok = false;
        resp.reason = "buffer was not created with SHADER_DEVICE_ADDRESS usage";
        return resp;
    }
    if (buf->second.bound_memory == 0) {
        resp.ok = false;
        resp.reason = "buffer is not bound to memory (bind before querying its address)";
        return resp;
    }
    resp.ok = true;
    resp.reason = "ok";
    // 0xB0DA in the top bits keeps the token non-zero and visibly synthetic; the handle in the
    // low bits keeps it distinct per buffer and stable across calls.
    resp.device_address = (0xB0DAull << 48) | (req.buffer << 8);
    return resp;
}

StatusResponse MockVulkanBackend::write_memory_ranges(const WriteMemoryRangesRequest& req) {
    StatusResponse resp;
    // Validate-then-apply (atomic): every memory known, every range inside its allocation, and the
    // payload long enough for the summed ranges. (The wire decoder already enforced sorted/disjoint
    // ranges, no duplicate memory, and payload-length == summed sizes; this re-checks bounds vs the
    // actual allocation sizes the worker would.)
    std::uint64_t cursor = 0;
    const std::uint64_t kHostVisibleCoherent =
        kMemoryPropertyHostVisible | kMemoryPropertyHostCoherent;
    for (const MemoryUpload& u : req.uploads) {
        const auto mem = memory_objects_.find(u.memory);
        if (mem == memory_objects_.end()) {
            resp.ok = false;
            resp.reason = "write_memory_ranges references unknown memory";
            return resp;
        }
        // Defense even if the ICD tracker is bypassed/regresses: only coherent host-visible memory
        // is a legal upload target. The real backend keeps the same check.
        if ((mem->second.property_flags & kHostVisibleCoherent) != kHostVisibleCoherent) {
            resp.ok = false;
            resp.reason = "write_memory_ranges target is not HOST_VISIBLE | HOST_COHERENT";
            return resp;
        }
        for (const MemoryUploadRange& r : u.ranges) {
            if (r.offset > mem->second.size || r.offset > ~0ull - r.size ||
                r.offset + r.size > mem->second.size) {
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
    cursor = 0;
    for (const MemoryUpload& u : req.uploads) {
        MemoryObject& mo = memory_objects_[u.memory];
        if (mo.bytes.size() < static_cast<std::size_t>(mo.size)) {
            mo.bytes.assign(static_cast<std::size_t>(mo.size), std::byte{0});
        }
        for (const MemoryUploadRange& r : u.ranges) {
            std::memcpy(mo.bytes.data() + r.offset, req.payload.data() + cursor,
                        static_cast<std::size_t>(r.size));
            cursor += r.size;
        }
    }
    resp.ok = true;
    resp.reason = "ok";
    return resp;
}

// Host-visible memory readback: the mock returns its stored bytes (no GPU, but it faithfully echoes
// whatever was written -- so a unit test round-trips upload -> readback).
ReadMemoryRangesResponse MockVulkanBackend::read_memory_ranges(const ReadMemoryRangesRequest& req) {
    ReadMemoryRangesResponse resp;
    std::string out;
    for (const MemoryUpload& u : req.reads) {
        const auto mem = memory_objects_.find(u.memory);
        if (mem == memory_objects_.end()) {
            resp.ok = false;
            resp.reason = "read_memory_ranges references unknown memory";
            return resp;
        }
        for (const MemoryUploadRange& r : u.ranges) {
            if (r.offset > mem->second.size || r.offset > ~0ull - r.size ||
                r.offset + r.size > mem->second.size) {
                resp.ok = false;
                resp.reason = "read_memory_ranges range outside the allocation";
                return resp;
            }
            const auto& bytes = mem->second.bytes;
            for (std::uint64_t i = 0; i < r.size; ++i) {
                const std::uint64_t idx = r.offset + i;
                out.push_back(idx < bytes.size() ? static_cast<char>(bytes[idx]) : '\0');
            }
        }
    }
    resp.ok = true;
    resp.reason = "ok";
    resp.payload = std::move(out);
    return resp;
}

// --- Textures + depth = literal vkcube  --------

GetPhysicalDeviceFormatPropertiesResponse MockVulkanBackend::get_physical_device_format_properties(
    const GetPhysicalDeviceFormatPropertiesRequest& req) {
    GetPhysicalDeviceFormatPropertiesResponse resp;
    bool known_physical = false;
    for (const auto& inst : instances_) {
        if (inst.second.physical_device == req.physical_device && req.physical_device != 0) {
            known_physical = true;
            break;
        }
    }
    if (!known_physical) {
        resp.reason = "unknown physical device handle";
        return resp;
    }
    // A canned-but-honest table: the texture format is sampleable both tiled and linear (so the
    // oracle exercises both upload paths); the depth format is a depth-stencil
    // attachment only when OPTIMAL. Other formats report no features (the fail-closed default).
    resp.ok = true;
    resp.reason = "ok";
    if (req.format == vk3c1::kFormatR8G8B8A8Unorm) {
        resp.linear_tiling_features =
            kFormatFeatureSampledImage | kFormatFeatureTransferSrc | kFormatFeatureTransferDst;
        resp.optimal_tiling_features = kFormatFeatureSampledImage | kFormatFeatureColorAttachment |
                                       kFormatFeatureTransferSrc | kFormatFeatureTransferDst;
    } else if (is_depth_stencil_format(req.format)) {
        // A depth/stencil format the mock admits is usable as a DS attachment AND as a transfer
        // target/source -- one consistent story with create_image, so the mock
        // never says "cannot transfer" at query time then creates/copies it at command time.
        resp.optimal_tiling_features = kFormatFeatureDepthStencilAttachment |
                                       kFormatFeatureTransferSrc | kFormatFeatureTransferDst;
    }
    // GL/zink: the mock has no real device, so the 64-bit VkFormatProperties3 features
    // MIRROR the 32-bit flags (the low VkFormatFeatureFlags2 bits coincide with
    // VkFormatFeatureFlags), keeping mock == real for the round-trip + the ICD's pNext fill.
    resp.linear_tiling_features2 = resp.linear_tiling_features;
    resp.optimal_tiling_features2 = resp.optimal_tiling_features;
    resp.buffer_features2 = resp.buffer_features;
    return resp;
}

GetPhysicalDeviceFeaturesResponse
MockVulkanBackend::get_physical_device_features(const GetPhysicalDeviceFeaturesRequest& req) {
    GetPhysicalDeviceFeaturesResponse resp;
    bool known_physical = false;
    for (const auto& inst : instances_) {
        if (inst.second.physical_device == req.physical_device && req.physical_device != 0) {
            known_physical = true;
            break;
        }
    }
    if (!known_physical) {
        resp.reason = "unknown physical device handle";
        return resp;
    }
    // The mock has no real device; it advertises the full core feature set (all 55 VkBool32 bits)
    // deterministically, so mock == real for the round-trip + the ICD's unpack. (The real backend
    // forwards the host's actual features.)
    resp.ok = true;
    resp.reason = "ok";
    resp.feature_bits = (std::uint64_t{1} << 55) - 1;
    return resp;
}

GetPhysicalDeviceImageFormatPropertiesResponse
MockVulkanBackend::get_physical_device_image_format_properties(
    const GetPhysicalDeviceImageFormatPropertiesRequest& req) {
    GetPhysicalDeviceImageFormatPropertiesResponse resp;
    bool known_physical = false;
    for (const auto& inst : instances_) {
        if (inst.second.physical_device == req.physical_device && req.physical_device != 0) {
            known_physical = true;
            break;
        }
    }
    if (!known_physical) {
        resp.reason = "unknown physical device handle";
        return resp;
    }
    // The mock has no real device; it reports a canned-but-plausible OK for any 2D image request
    // and FORMAT_NOT_SUPPORTED (-11) otherwise, deterministically. (The real backend forwards the
    // host's actual vkGetPhysicalDeviceImageFormatProperties.)
    resp.ok = true;
    resp.reason = "ok";
    if (req.image_type != kImageType2D) {
        resp.result = -11; // VK_ERROR_FORMAT_NOT_SUPPORTED
        return resp;
    }
    resp.result = 0; // VK_SUCCESS
    resp.max_extent_width = 16384;
    resp.max_extent_height = 16384;
    resp.max_extent_depth = 1;
    resp.max_mip_levels = 15;
    resp.max_array_layers = 2048;
    resp.sample_counts = 0x1 | 0x4; // VK_SAMPLE_COUNT_1_BIT | _4_BIT
    resp.max_resource_size = std::uint64_t{1} << 31;
    return resp;
}

GetPhysicalDevicePropertiesResponse
MockVulkanBackend::get_physical_device_properties(const GetPhysicalDevicePropertiesRequest& req) {
    GetPhysicalDevicePropertiesResponse resp;
    bool known_physical = false;
    for (const auto& inst : instances_) {
        if (inst.second.physical_device == req.physical_device && req.physical_device != 0) {
            known_physical = true;
            break;
        }
    }
    if (!known_physical) {
        resp.reason = "unknown physical device handle";
        return resp;
    }
    // The mock has no real device / no vulkan.h, so it returns an EMPTY blob: the ICD then keeps
    // its synthesized minimal properties (the mock is not the ICD's runtime peer -- the real worker
    // is).
    resp.ok = true;
    resp.reason = "ok";
    return resp;
}

GetPhysicalDeviceCapabilityChainResponse MockVulkanBackend::get_physical_device_capability_chain(
    const GetPhysicalDeviceCapabilityChainRequest& req) {
    GetPhysicalDeviceCapabilityChainResponse resp;
    bool known_physical = false;
    for (const auto& inst : instances_) {
        if (inst.second.physical_device == req.physical_device && req.physical_device != 0) {
            known_physical = true;
            break;
        }
    }
    if (!known_physical) {
        resp.reason = "unknown physical device handle";
        return resp;
    }
    // The mock echoes each requested sType with an EMPTY blob (no real device to fill from); the
    // ICD then leaves those pNext structs zeroed. Preserves request order + count for the
    // round-trip test.
    resp.ok = true;
    resp.reason = "ok";
    for (const auto& e : req.entries) {
        CapabilityChainEntry out;
        out.s_type = e.s_type;
        resp.entries.push_back(out);
    }
    return resp;
}

CreateImageResponse MockVulkanBackend::create_image(const CreateImageRequest& req) {
    CreateImageResponse resp;
    const auto dev = devices_.find(req.device);
    if (dev == devices_.end()) {
        resp.reason = "unknown device handle";
        return resp;
    }
    if (req.image_type != kImageType2D) {
        resp.reason = "image must be 2D";
        return resp;
    }
    // Aspect from the shared format->aspect table (mock == real). A depth/stencil format (124-130)
    // is admitted as a copy target now; color is still only R8G8B8A8_UNORM (the mock's texture).
    const int aspect_mask = format_aspect_mask(req.format);
    const bool is_ds = is_depth_stencil_format(req.format);
    const bool is_color = req.format == vk3c1::kFormatR8G8B8A8Unorm;
    if (!is_ds && !is_color) {
        resp.reason = "image format must be R8G8B8A8_UNORM or a depth/stencil format";
        return resp;
    }
    // Mip/layer counts are tracked faithfully now (the widened copy_buffer_to_image validates each
    // region's mip/layer bounds against them; the real backend forwards both to the host driver
    // verbatim) -- sane structural ceilings, not host limits. The rest of the allowlist
    // (formats/usages/tiling) remains the open faithful-broadening.
    if (req.width <= 0 || req.height <= 0 || req.depth != 1 || req.mip_levels < 1 ||
        req.mip_levels > 16 || req.array_layers < 1 || req.array_layers > 2048 ||
        req.samples != vk3b::kSampleCount1) {
        resp.reason = "image must be a 2D image (mips 1..16, layers 1..2048, samples 1)";
        return resp;
    }
    if (req.tiling != kImageTilingOptimal && req.tiling != kImageTilingLinear) {
        resp.reason = "image tiling must be OPTIMAL or LINEAR";
        return resp;
    }
    if (req.usage == 0 || (req.usage & ~kImageUsageSubset) != 0) {
        resp.reason = "image usage must be a nonzero subset of {SAMPLED, TRANSFER_SRC, "
                      "TRANSFER_DST, DEPTH_STENCIL_ATTACHMENT, COLOR_ATTACHMENT}";
        return resp;
    }
    if (req.sharing_mode != vk3c1::kSharingModeExclusive) {
        resp.reason = "image sharing mode must be EXCLUSIVE";
        return resp;
    }
    // Depth/stencil: OPTIMAL tiling, UNDEFINED initial layout; usage may COMBINE
    // DEPTH_STENCIL_ATTACHMENT (render), TRANSFER_SRC/DST (staging upload+readback), and SAMPLED
    // (sampled depth) -- the previous DEPTH_STENCIL_ATTACHMENT-only rule
    // blocked the copy endpoints. Color texture: no DEPTH_STENCIL_ATTACHMENT usage; PREINITIALIZED
    // only for LINEAR. The aspect is the format's natural aspect (mock == real).
    const int aspect = aspect_mask;
    if (is_ds) {
        const std::uint64_t kDsUsageAllowed =
            kImageUsageDepthStencilAttachment | static_cast<std::uint64_t>(kImageUsageTransferSrc) |
            static_cast<std::uint64_t>(kImageUsageTransferDst) | kImageUsageSampled;
        if (req.tiling != kImageTilingOptimal || req.usage == 0 ||
            (req.usage & ~kDsUsageAllowed) != 0 || req.initial_layout != kImageLayoutUndefinedC3) {
            resp.reason = "depth/stencil image must be OPTIMAL, initial layout "
                          "UNDEFINED, usage a nonzero subset of {DEPTH_STENCIL_ATTACHMENT, "
                          "TRANSFER_SRC, TRANSFER_DST, SAMPLED}";
            return resp;
        }
    } else {
        if ((req.usage & kImageUsageDepthStencilAttachment) != 0) {
            resp.reason = "color image must not have DEPTH_STENCIL_ATTACHMENT usage";
            return resp;
        }
        if (req.initial_layout != kImageLayoutUndefinedC3 &&
            req.initial_layout != kImageLayoutPreinitialized) {
            resp.reason = "image initial layout must be UNDEFINED or PREINITIALIZED";
            return resp;
        }
        if (req.initial_layout == kImageLayoutPreinitialized && req.tiling != kImageTilingLinear) {
            resp.reason = "PREINITIALIZED initial layout requires LINEAR tiling";
            return resp;
        }
    }
    // Honest-ish requirements: bytes-per-pixel * extent, a 256-byte alignment, and type bits
    // matching the tiling's natural class -- OPTIMAL binds device-local, LINEAR binds host-visible
    // (so bind enforces the right memory class, mirroring a real driver). Stored on the image for
    // bind.
    const std::uint64_t bpp = format_mock_texel_bytes(req.format);
    const std::uint64_t row = static_cast<std::uint64_t>(req.width) * bpp;
    const std::uint64_t size = row * static_cast<std::uint64_t>(req.height);
    std::uint64_t type_bits = 0;
    const std::vector<MemoryType> types = mock_memory_types();
    const std::uint64_t kHostVisibleCoherent =
        kMemoryPropertyHostVisible | kMemoryPropertyHostCoherent;
    for (std::size_t i = 0; i < types.size(); ++i) {
        const bool host = (types[i].property_flags & kHostVisibleCoherent) == kHostVisibleCoherent;
        const bool devlocal = (types[i].property_flags & kMemoryPropertyDeviceLocal) != 0;
        if ((req.tiling == kImageTilingLinear && host) ||
            (req.tiling == kImageTilingOptimal && devlocal && !host)) {
            type_bits |= (1ull << i);
        }
    }
    const std::uint64_t h = next_handle_++;
    Image im;
    im.device = req.device;
    im.app_created = true;
    im.format = req.format;
    im.aspect = aspect;
    im.tiling = req.tiling;
    im.usage = req.usage;
    // Clear support derives from the REQUESTED usage, exactly like the swapchain path (mock ==
    // real): an app image created with TRANSFER_DST is a legal clear target.
    im.transfer_dst = (req.usage & static_cast<std::uint64_t>(kImageUsageTransferDst)) != 0;
    im.width = req.width;
    im.height = req.height;
    im.mip_levels = req.mip_levels > 0 ? req.mip_levels : 1;
    im.array_layers = req.array_layers > 0 ? req.array_layers : 1;
    im.alignment = 256;
    im.memory_type_bits = type_bits;
    images_.emplace(h, im);
    dev->second.images.insert(h);
    resp.ok = true;
    resp.reason = "ok";
    resp.image = h;
    resp.mem_size = size;
    resp.mem_alignment = im.alignment;
    resp.mem_type_bits = type_bits;
    // The subresource layout is queryable only for LINEAR tiling (vkcube's linear texture path
    // reads rowPitch to write mapped rows).
    if (req.tiling == kImageTilingLinear) {
        resp.has_subresource_layout = true;
        resp.sr_offset = 0;
        resp.sr_size = size;
        resp.sr_row_pitch = row;
    }
    return resp;
}

StatusResponse MockVulkanBackend::destroy_image(const HandleRequest& req) {
    StatusResponse resp;
    const auto it = images_.find(req.handle);
    if (it == images_.end() || !it->second.app_created) {
        resp.ok = false;
        resp.reason = "unknown image handle (must be an app-created image)";
        return resp;
    }
    // An app image is blocked while a live view references it -- the same
    // parent/child rule as swapchain -> view; destroy the views first.
    if (!it->second.image_views.empty()) {
        resp.ok = false;
        resp.reason = "image has live image views; destroy them first";
        return resp;
    }
    invalidate_cbs_referencing(req.handle); // a recorded copy/barrier may have baked this handle
    const auto dev = devices_.find(it->second.device);
    if (dev != devices_.end()) {
        dev->second.images.erase(req.handle);
    }
    images_.erase(it);
    resp.ok = true;
    resp.reason = "ok";
    return resp;
}

StatusResponse MockVulkanBackend::bind_image_memory(const BindImageMemoryRequest& req) {
    StatusResponse resp;
    const auto img = images_.find(req.image);
    if (img == images_.end() || !img->second.app_created) {
        resp.ok = false;
        resp.reason = "unknown image handle (must be an app-created image)";
        return resp;
    }
    const auto mem = memory_objects_.find(req.memory);
    if (mem == memory_objects_.end()) {
        resp.ok = false;
        resp.reason = "unknown memory handle";
        return resp;
    }
    if (img->second.device != mem->second.device) {
        resp.ok = false;
        resp.reason = "image and memory are on different devices";
        return resp;
    }
    if (img->second.bound_memory != 0) {
        resp.ok = false;
        resp.reason = "image is already bound to memory";
        return resp;
    }
    if (img->second.memory_type_bits != 0 &&
        (img->second.memory_type_bits & (1ull << mem->second.type_index)) == 0) {
        resp.ok = false;
        resp.reason = "memory type is not in the image's supported memoryTypeBits";
        return resp;
    }
    if (img->second.alignment != 0 && (req.memory_offset % img->second.alignment) != 0) {
        resp.ok = false;
        resp.reason = "bind offset does not satisfy the image's alignment";
        return resp;
    }
    img->second.bound_memory = req.memory;
    img->second.bound_offset = req.memory_offset;
    resp.ok = true;
    resp.reason = "ok";
    return resp;
}

CreateSamplerResponse MockVulkanBackend::create_sampler(const CreateSamplerRequest& req) {
    CreateSamplerResponse resp;
    const auto dev = devices_.find(req.device);
    if (dev == devices_.end()) {
        resp.reason = "unknown device handle";
        return resp;
    }
    // GL/zink: general samplers are accepted (mock == real -- the real worker forwards
    // the faithful VkSamplerCreateInfo to the host's vkCreateSampler). Minimal sanity: the
    // wire-required enum fields must be present (non-negative), the rest ride through.
    if (req.mag_filter < 0 || req.min_filter < 0 || req.mipmap_mode < 0 || req.address_mode_u < 0 ||
        req.address_mode_v < 0 || req.address_mode_w < 0 || req.anisotropy_enable < 0 ||
        req.compare_enable < 0) {
        resp.reason = "sampler create-info has a missing/malformed field";
        return resp;
    }
    const std::uint64_t h = next_handle_++;
    samplers_.emplace(h, Sampler{req.device});
    dev->second.samplers.insert(h);
    resp.ok = true;
    resp.reason = "ok";
    resp.sampler = h;
    return resp;
}

StatusResponse MockVulkanBackend::destroy_sampler(const HandleRequest& req) {
    StatusResponse resp;
    const auto it = samplers_.find(req.handle);
    if (it == samplers_.end()) {
        resp.ok = false;
        resp.reason = "unknown sampler handle";
        return resp;
    }
    // CB -> set -> sampler destroy consult: a combined-image-sampler slot
    // currently referencing this sampler is marked dangling + its CBs invalidated (mirrors the
    // buffer/image-view consult).
    dangle_sets_referencing(req.handle);
    const auto dev = devices_.find(it->second.device);
    if (dev != devices_.end()) {
        dev->second.samplers.erase(req.handle);
    }
    samplers_.erase(it);
    resp.ok = true;
    resp.reason = "ok";
    return resp;
}

// Query pools (GL 3.3 / occlusion / xfb queries). The mock is the oracle: it validates the
// create-info + tracks the pool (type/count) so recorded query commands range-check identically to
// the real backend, and answers GetQueryPoolResults with a benign zero-filled result (no GPU).
CreateQueryPoolResponse MockVulkanBackend::create_query_pool(const CreateQueryPoolRequest& req) {
    CreateQueryPoolResponse resp;
    const auto dev = devices_.find(req.device);
    if (dev == devices_.end()) {
        resp.reason = "unknown device handle";
        return resp;
    }
    // mock == real: OCCLUSION / TIMESTAMP / PIPELINE_STATISTICS only. TRANSFORM_FEEDBACK_STREAM is
    // NOT accepted -- its indexed begin/end are not wired, so it
    // fails closed at creation rather than aborting later. A zero count is malformed.
    if (req.query_type != vk3b::kQueryTypeOcclusion &&
        req.query_type != vk3b::kQueryTypePipelineStatistics &&
        req.query_type != vk3b::kQueryTypeTimestamp) {
        resp.reason = "unsupported query type";
        return resp;
    }
    if (req.query_count == 0) {
        resp.reason = "query pool needs a non-zero query count";
        return resp;
    }
    const std::uint64_t h = next_handle_++;
    query_pools_.emplace(h, QueryPool{req.device, req.query_type, req.query_count});
    dev->second.query_pools.insert(h);
    resp.ok = true;
    resp.reason = "ok";
    resp.query_pool = h;
    return resp;
}

StatusResponse MockVulkanBackend::destroy_query_pool(const HandleRequest& req) {
    StatusResponse resp;
    const auto it = query_pools_.find(req.handle);
    if (it == query_pools_.end()) {
        resp.ok = false;
        resp.reason = "unknown query pool handle";
        return resp;
    }
    // A recorded stream that referenced this pool (reset/begin/end/write_timestamp bakes the
    // handle) is invalidated so a later submit is refused rather than replaying a freed pool.
    invalidate_cbs_referencing(req.handle);
    const auto dev = devices_.find(it->second.device);
    if (dev != devices_.end()) {
        dev->second.query_pools.erase(req.handle);
    }
    query_pools_.erase(it);
    resp.ok = true;
    resp.reason = "ok";
    return resp;
}

GetQueryPoolResultsResponse
MockVulkanBackend::get_query_pool_results(const GetQueryPoolResultsRequest& req) {
    GetQueryPoolResultsResponse resp;
    const auto it = query_pools_.find(req.query_pool);
    if (it == query_pools_.end() || it->second.device != req.device) {
        resp.reason = "unknown query pool handle on the device";
        return resp;
    }
    // Overflow-safe (mock == real): first_query + query_count wraps in
    // uint32. Compare by subtraction after bounding first.
    if (req.first_query > it->second.count ||
        req.query_count > it->second.count - req.first_query) {
        resp.reason = "query range out of bounds";
        return resp;
    }
    // No GPU: report a benign, well-formed zero result (VK_SUCCESS, data zero-filled to data_size).
    // The mock is the wire/lifetime oracle, not a results oracle.
    resp.ok = true;
    resp.reason = "ok";
    resp.vk_result = vk3b::kVkSuccess;
    resp.data.assign(static_cast<std::size_t>(req.data_size), '\0');
    return resp;
}

StatusResponse MockVulkanBackend::reset_query_pool(const ResetQueryPoolRequest& req) {
    // hostQueryReset: the device-level reset. Same handle + range validation as
    // get_query_pool_results (mock == real); no GPU state to clear, so a valid range succeeds.
    StatusResponse resp;
    // Fail closed: the device must have enabled the hostQueryReset feature. A
    // client that never enabled it (a skewed/hostile caller) is refused rather than modelling an
    // invalid host reset -- mock == real (the real worker refuses the same, and the ICD no-ops
    // before the wire).
    const auto dev = devices_.find(req.device);
    if (dev == devices_.end()) {
        resp.reason = "unknown device handle";
        return resp;
    }
    if (!dev->second.host_query_reset_feature_enabled) {
        resp.reason = "reset_query_pool without the enabled hostQueryReset feature";
        return resp;
    }
    const auto it = query_pools_.find(req.query_pool);
    if (it == query_pools_.end() || it->second.device != req.device) {
        resp.reason = "unknown query pool handle on the device";
        return resp;
    }
    // Overflow-safe range check (mock == real). A zero count is
    // malformed (vkResetQueryPool requires queryCount > 0).
    if (req.query_count == 0 || req.first_query > it->second.count ||
        req.query_count > it->second.count - req.first_query) {
        resp.reason = "reset_query_pool range out of bounds";
        return resp;
    }
    resp.ok = true;
    resp.reason = "ok";
    return resp;
}

CreateSurfaceResponse MockVulkanBackend::create_surface(const CreateSurfaceRequest& req) {
    CreateSurfaceResponse resp;
    const auto it = instances_.find(req.instance);
    if (it == instances_.end()) {
        resp.ok = false;
        resp.reason = "unknown instance handle";
        return resp;
    }
    const std::uint64_t surface = next_handle_++;
    Surface s;
    s.instance = req.instance;
    s.platform = req.platform;
    s.xid = req.xid;
    s.role_hint = req.role_hint;
    surfaces_.emplace(surface, s);
    it->second.surfaces.insert(surface);
    // Born-correlated surface<->XID: bind under the backend mutex (cross-plane
    // with the sidecar's register_toplevel) and execute any promote (a placeholder the sidecar
    // created first is superseded by this surface). The mock's promote just drops the fake
    // placeholder id.
    {
        std::lock_guard<std::mutex> lk(backend_mutex_);
        apply_mock_effect(registry_.bind_surface(req.xid, surface));
    }
    resp.ok = true;
    resp.reason = "ok";
    resp.surface = surface;
    return resp;
}

StatusResponse MockVulkanBackend::destroy_surface(const HandleRequest& req) {
    StatusResponse resp;
    const auto it = surfaces_.find(req.handle);
    if (it == surfaces_.end()) {
        resp.ok = false;
        resp.reason = "unknown surface handle";
        return resp;
    }
    // A swapchain targets a surface; the swapchain must be destroyed first.
    if (!it->second.swapchains.empty()) {
        resp.ok = false;
        resp.reason = "surface has live swapchains; destroy them first";
        return resp;
    }
    const auto inst = instances_.find(it->second.instance);
    if (inst != instances_.end()) {
        inst->second.surfaces.erase(req.handle);
    }
    // Surface-specific unbind under the backend mutex (no-op when the surface had
    // no XID, or when a newer surface already rebound this XID; never a host effect -- a
    // still-registered toplevel falls back to a representation-less entry, reaped by unregister).
    {
        std::lock_guard<std::mutex> lk(backend_mutex_);
        apply_mock_effect(registry_.unbind_surface(it->second.xid, req.handle));
    }
    surfaces_.erase(it);
    resp.ok = true;
    resp.reason = "ok";
    return resp;
}

CreateSwapchainResponse MockVulkanBackend::create_swapchain(const CreateSwapchainRequest& req) {
    CreateSwapchainResponse resp;
    const auto dev = devices_.find(req.device);
    if (dev == devices_.end()) {
        resp.ok = false;
        resp.reason = "unknown device handle";
        return resp;
    }
    const auto surf = surfaces_.find(req.surface);
    if (surf == surfaces_.end()) {
        resp.ok = false;
        resp.reason = "unknown surface handle";
        return resp;
    }
    // The surface must belong to the same instance as the device (you present a
    // device's images to a surface created from that device's instance).
    if (surf->second.instance != dev->second.instance) {
        resp.ok = false;
        resp.reason = "surface and device belong to different instances";
        return resp;
    }
    // Reject malformed/sentinel request fields, consistent with the real backend: the
    // decoder maps missing/wrong-typed/out-of-range values to -1 so they are rejected
    // rather than silently substituted. (The mock has no surface caps, so it cannot
    // validate format/present-mode support -- that is the real backend's job.) width/height must be
    // > 0: min extent is 1x1 and a Vulkan swapchain extent cannot be zero.
    if (req.image_format < 0 || req.color_space < 0 || req.present_mode < 0 || req.width <= 0 ||
        req.height <= 0 || req.min_image_count < 1 || req.image_usage < 1) {
        resp.ok = false;
        resp.reason = "malformed or missing swapchain parameters";
        return resp;
    }
    // (integrity, mock == real): if the sidecar has authored a live extent for this
    // surface's toplevel, a create_swapchain at any OTHER extent returns OUT_OF_DATE (the app
    // re-queries caps + retries) -- the app can never drag the host off the authored extent.
    {
        std::lock_guard<std::mutex> lk(backend_mutex_);
        const sidecar::WindowRegistry::AuthoritativeExtent authority =
            registry_.authoritative_extent_for_xid(surf->second.xid);
        if (authority.active && (static_cast<std::uint32_t>(req.width) != authority.width ||
                                 static_cast<std::uint32_t>(req.height) != authority.height)) {
            resp.ok = true; // not a fault: the app re-queries caps + retries
            resp.result = kVkErrorOutOfDateKhr;
            resp.swapchain = 0;
            resp.reason = "swapchain extent does not match the sidecar-authoritative extent";
            return resp;
        }
    }
    // The mock has no surface caps, so it cannot validate image_usage against
    // supportedUsageFlags (that is the real backend's job); it only requires a non-zero
    // usage and records whether TRANSFER_DST was requested (for clear validation).
    const std::uint64_t swapchain = next_handle_++;
    Swapchain sc;
    sc.device = req.device;
    sc.surface = req.surface;
    sc.transfer_dst = (req.image_usage & kImageUsageTransferDst) != 0;
    sc.image_format = req.image_format; // an image view over these images must match it
    sc.width = req.width;               // a framebuffer over these images must match it
    sc.height = req.height;
    // Image count derives from the requested minimum (a real swapchain returns at
    // least that many); clamp to a sane ceiling for the headless model.
    int n = req.min_image_count;
    if (n < 2) {
        n = 2;
    }
    if (n > 8) {
        n = 8;
    }
    sc.image_count = n;
    swapchains_.emplace(swapchain, sc);
    dev->second.swapchains.insert(swapchain);
    surf->second.swapchains.insert(swapchain);
    // a fresh swapchain matches the window's current geometry, so the dirty latch
    // clears -- present/acquire may flow again. (The real backend also syncs the HWND
    // client rect first; the mock has no window.)
    surf->second.geometry_dirty = false;
    resp.ok = true;
    resp.reason = "ok";
    resp.swapchain = swapchain;
    // The mock reports the sentinel currentExtent and has no HWND, so it cannot model the real
    // worker's extent convergence -- its create always succeeds (kVkSuccess). The OUT_OF_DATE path
    // is a real-worker behavior; the wire `result` field round-trips regardless
    // (unit_vkrpc/fuzz_vkrpc).
    resp.result = kVkSuccess;
    return resp;
}

StatusResponse MockVulkanBackend::destroy_swapchain(const HandleRequest& req) {
    StatusResponse resp;
    const auto it = swapchains_.find(req.handle);
    if (it == swapchains_.end()) {
        resp.ok = false;
        resp.reason = "unknown swapchain handle";
        return resp;
    }
    // an image view over this swapchain's images must be destroyed first (a view
    // outlives its image otherwise). Mirrors the real backend's Vulkan destroy ordering.
    if (!it->second.image_views.empty()) {
        resp.ok = false;
        resp.reason = "swapchain has live image views; destroy them first";
        return resp;
    }
    const auto dev = devices_.find(it->second.device);
    if (dev != devices_.end()) {
        dev->second.swapchains.erase(req.handle);
    }
    const auto surf = surfaces_.find(it->second.surface);
    if (surf != surfaces_.end()) {
        surf->second.swapchains.erase(req.handle);
    }
    // Any command buffer recorded against this swapchain's images is now stale: its baked
    // commands reference images that die with the swapchain, so it must not be submitted.
    // Invalidate it (a later re-record against a fresh swapchain revalidates it). Mirrors
    // the real backend; queue_submit then refuses it on the recorded check.
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

void MockVulkanBackend::ensure_swapchain_images(std::uint64_t swapchain_handle, Swapchain& sc) {
    // Mint the image handles once and return them stably (idempotent, like a real
    // vkGetSwapchainImagesKHR over a swapchain's lifetime); register each in images_ so
    // record_command_buffer can resolve a referenced image to its device/surface/usage.
    if (!sc.images.empty()) {
        return;
    }
    for (int i = 0; i < sc.image_count; ++i) {
        const std::uint64_t img = next_handle_++;
        sc.images.push_back(img);
        Image meta;
        meta.device = sc.device;
        meta.swapchain = swapchain_handle;
        meta.surface = sc.surface;
        meta.transfer_dst = sc.transfer_dst;
        meta.format = sc.image_format;
        images_.emplace(img, meta);
    }
}

GetSwapchainImagesResponse
MockVulkanBackend::get_swapchain_images(const GetSwapchainImagesRequest& req) {
    GetSwapchainImagesResponse resp;
    const auto it = swapchains_.find(req.swapchain);
    if (it == swapchains_.end()) {
        resp.ok = false;
        resp.reason = "unknown swapchain handle";
        return resp;
    }
    ensure_swapchain_images(req.swapchain, it->second);
    resp.images = it->second.images;
    resp.ok = true;
    resp.reason = "ok";
    return resp;
}

AcquireNextImageResponse MockVulkanBackend::acquire_next_image(const AcquireNextImageRequest& req) {
    AcquireNextImageResponse resp;
    const auto it = swapchains_.find(req.swapchain);
    if (it == swapchains_.end()) {
        resp.ok = false;
        resp.reason = "unknown swapchain handle";
        return resp;
    }
    // latch: while the target surface is geometry-dirty, acquire returns
    // OUT_OF_DATE without "calling the driver" and does NOT advance the rotation -- the
    // app must recreate the swapchain (which clears the latch).
    const auto surf = surfaces_.find(it->second.surface);
    if (surf != surfaces_.end() && surf->second.geometry_dirty) {
        resp.ok = true;
        resp.reason = "ok";
        resp.result = kVkErrorOutOfDateKhr;
        return resp;
    }
    ensure_swapchain_images(req.swapchain, it->second);
    resp.image_index = it->second.next_image;
    it->second.next_image = (it->second.next_image + 1) % it->second.image_count;
    resp.result = kVkSuccess;
    resp.ok = true;
    resp.reason = "ok";
    return resp;
}

QueuePresentResponse MockVulkanBackend::queue_present(const QueuePresentRequest& req) {
    QueuePresentResponse resp;
    const auto qd = queue_to_device_.find(req.queue);
    if (qd == queue_to_device_.end()) {
        resp.ok = false;
        resp.reason = "unknown queue handle";
        return resp;
    }
    if (req.presents.empty()) {
        resp.ok = false;
        resp.reason = "no present targets";
        return resp;
    }
    // Each wait semaphore must be a semaphore on the present queue's device (mirrors the
    // real backend; a foreign-device or wrong-kind handle is an RPC fault).
    for (const std::uint64_t s : req.wait_semaphores) {
        const auto leaf = leaves_.find(s);
        if (leaf == leaves_.end() || leaf->second.kind != LeafKind::Semaphore ||
            leaf->second.device != qd->second) {
            resp.ok = false;
            resp.reason = "wait semaphore is not a semaphore on the present queue's device";
            return resp;
        }
    }
    // Validate every target (known swapchain, in-range image index) before any state
    // change -- a malformed target is an RPC fault (ok=false), not a present result.
    for (const PresentEntry& e : req.presents) {
        const auto sc = swapchains_.find(e.swapchain);
        if (sc == swapchains_.end()) {
            resp.ok = false;
            resp.reason = "unknown swapchain handle";
            return resp;
        }
        if (e.image_index < 0 || e.image_index >= sc->second.image_count) {
            resp.ok = false;
            resp.reason = "image index out of range for swapchain";
            return resp;
        }
    }
    // if ANY target surface is dirty, skip the present entirely and report
    // OUT_OF_DATE for the whole batch (no per-target driver result exists).
    bool any_dirty = false;
    for (const PresentEntry& e : req.presents) {
        const auto sc = swapchains_.find(e.swapchain);
        const auto surf = surfaces_.find(sc->second.surface);
        if (surf != surfaces_.end() && surf->second.geometry_dirty) {
            any_dirty = true;
            break;
        }
    }
    resp.ok = true;
    resp.reason = "ok";
    if (any_dirty) {
        resp.result = kVkErrorOutOfDateKhr;
        resp.results.assign(req.presents.size(), kVkErrorOutOfDateKhr);
        return resp;
    }
    resp.result = kVkSuccess;
    resp.results.assign(req.presents.size(), kVkSuccess);
    // Show-on-first-present: a successful present reveals each target's surface.
    for (const PresentEntry& e : req.presents) {
        const auto sc = swapchains_.find(e.swapchain);
        const auto surf = surfaces_.find(sc->second.surface);
        if (surf != surfaces_.end()) {
            surf->second.shown = true;
        }
    }
    return resp;
}

StatusResponse MockVulkanBackend::record_command_buffer(const RecordCommandBufferRequest& req) {
    // The mock has no GPU; it models the command-buffer state machine structurally:
    // validate the whole stream (validate-then-record, atomic), then flip `recorded` and
    // store the referenced swapchain surfaces (queue_submit would lock their union).
    // the whole mock body is the validate pass (no replay -- replay_us
    // honestly stays 0 on this backend); accumulated on the success path only.
    RpcProfile* prof = profile_instance();
    const std::uint64_t t_validate0 = prof != nullptr ? profile_now_us() : 0;
    StatusResponse resp;
    const auto cb = command_buffers_.find(req.command_buffer);
    if (cb == command_buffers_.end()) {
        resp.ok = false;
        resp.reason = "unknown command buffer handle";
        return resp;
    }
    const std::uint64_t device = cb->second.device;
    std::set<std::uint64_t> referenced_surfaces;
    std::set<std::uint64_t> referenced_swapchains;
    std::set<std::uint64_t> referenced_draw_objects; // render pass/framebuffer/view/pipeline
    // Draw state machine: enough to reject what a validation-clean real backend would. It checks
    // render-pass scope and compatibility, a compatible bound graphics pipeline, and the dynamic
    // VIEWPORT+SCISSOR state that the pipeline declares.
    // Render-scope state machine (dynamic rendering): ONE active scope with a
    // KIND, so render passes and dynamic rendering share the draw / copy / transform-feedback /
    // conditional-rendering / end-of-stream rules instead of drifting into parallel booleans.
    // RenderPass and DynamicRendering mutually exclude; each `end` requires its own kind.
    enum class RenderScope { None, RenderPass, DynamicRendering };
    RenderScope active_scope = RenderScope::None;
    int active_rp_format = 0;       // the active render pass's color format (pipeline compat key)
    int active_rp_depth_format = 0; // its depth format (0 = no depth); pipeline compat key
    // The active dynamic-rendering scope's per-attachment formats (from its attachment views) +
    // view mask -- the draw-time compatibility key a bound DR pipeline must match.
    std::vector<int> active_dr_color_formats;
    int active_dr_depth_format = 0;
    int active_dr_stencil_format = 0;
    int active_dr_view_mask = 0;
    // Compute: GRAPHICS and COMPUTE are SEPARATE bind points in
    // Vulkan -- binding at one never disturbs the other. Index 0 = GRAPHICS, 1 = COMPUTE, for
    // both the bound pipeline and the descriptor-bind record (exactness per point).
    std::uint64_t bound_pipeline_by_point[2] = {0, 0};
    std::uint64_t bound_desc_layout_by_point[2] = {0, 0};
    std::vector<std::uint64_t> bound_desc_sets_by_point[2];
    bool viewport_set = false;           // dynamic viewport set since vkBeginCommandBuffer (sticky)
    bool scissor_set = false;            // dynamic scissor likewise
    std::set<int> bound_vertex_bindings; // binding indices bound by bind_vertex_buffers
    bool index_bound = false;
    // (GL/zink): transform-feedback + conditional-rendering scope state (mock == real; see
    // the real backend for the spec rationale). The device record is needed for the enabled-
    // extension gate on those commands.
    const auto mdev = devices_.find(device);
    if (mdev == devices_.end()) {
        resp.ok = false;
        resp.reason = "command buffer has no live device";
        return resp;
    }
    bool xfb_active = false;
    bool cond_render_active = false;
    bool cond_render_began_in_rp = false;
    // Query pools (mock == real): the (pool,query) pairs begun-but-not-ended in this command
    // buffer.
    std::set<std::pair<std::uint64_t, std::uint32_t>> active_queries;
    for (const RecordedCommand& c : req.commands) {
        // one lookup, then integer dispatch (the string-compare chain was a measured
        // slice of per-command validate time at ETR volumes). Unknown -> the final else, as ever.
        const CmdKind k = cmd_kind_from_string(c.kind);
        if (k == CmdKind::PipelineBarrier || k == CmdKind::ClearColorImage) {
            // Image-targeting commands: the referenced image must resolve and live on
            // the command buffer's device.
            const auto img = images_.find(c.image);
            if (img == images_.end() || img->second.device != device) {
                resp.ok = false;
                resp.reason = "command references an image not on the command buffer's device";
                return resp;
            }
            // An app-created image must be bound to memory before any command targets it
            // (mirrors the image-view guard -- a barrier/clear against an unbound VkImage
            // is a fail-closed worker-boundary violation). Swapchain images are swapchain-bound.
            if (img->second.app_created && img->second.bound_memory == 0) {
                resp.ok = false;
                resp.reason = "command references an app image not bound to memory";
                return resp;
            }
            if (k == CmdKind::PipelineBarrier) {
                // Layouts/aspect wide-decoded (>=0; aspect a non-zero mask); sync1 stage masks
                // must be non-zero AND fit 32-bit VkFlags; access masks may be zero but must fit.
                if (c.old_layout < 0 || c.new_layout < 0 || c.aspect < 1 ||
                    !valid_stage_mask(c.src_stage) || !valid_stage_mask(c.dst_stage) ||
                    !valid_access_mask(c.src_access) || !valid_access_mask(c.dst_access)) {
                    resp.ok = false;
                    resp.reason = "malformed pipeline_barrier command";
                    return resp;
                }
                // an app-image (texture) barrier is restricted to the upload
                // transition allowlist + a single-subresource range (shared helper -> mock ==
                // real). A swapchain-image barrier keeps the proven clear-frame form
                // (validated loosely above): restricting it would break the clear-frame canary, and
                // the cube draw path uses render-pass-owned transitions, not explicit barriers.
                if (img->second.app_created &&
                    !app_image_barrier_ok(c, img->second.usage, resp.reason)) {
                    resp.ok = false;
                    return resp;
                }
            } else { // clear_color_image
                if (c.layout < 0) {
                    resp.ok = false;
                    resp.reason = "malformed clear_color_image command";
                    return resp;
                }
                if (!img->second.transfer_dst) {
                    resp.ok = false;
                    resp.reason = "clear_color_image target image lacks TRANSFER_DST usage";
                    return resp;
                }
            }
            referenced_surfaces.insert(img->second.surface);
            referenced_swapchains.insert(img->second.swapchain);
            // The recorded command bakes this image handle: destroying the image
            // must invalidate this command buffer. Swapchain images are also covered via
            // referenced_swapchains, but an app image has surface == swapchain == 0, so the generic
            // referenced-object set is the only thing that makes destroy_image invalidate it.
            referenced_draw_objects.insert(c.image);
        } else if (k == CmdKind::BeginRenderPass) {
            // Draw subset. begin/end render pass nest; the render pass + framebuffer
            // must live on the command buffer's device.
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
            // The framebuffer must be compatible with the render pass begun (Vulkan: matching
            // attachment format/samples; for the single-color subset that reduces to the
            // color format), checked via the framebuffer's SNAPSHOT of its creating pass --
            // Vulkan lets the creating pass be destroyed and the framebuffer begun with any
            // COMPATIBLE pass (mpv/libplacebo does exactly this), so the creating pass is never
            // resolved by handle here (mock == real).
            if (fb->second.compat_color_format != rp->second.color_format ||
                fb->second.compat_depth_format != rp->second.depth_format) {
                resp.ok = false;
                resp.reason = "begin_render_pass framebuffer is incompatible with the render pass";
                return resp;
            }
            // begin_render_pass depth clear: a depth render pass
            // (loadOp CLEAR) must carry an explicit depth clear in [0,1]; a color-only pass must
            // not. has_depth_clear is the presence flag -- a missing depth clear is NOT silently
            // 0.0.
            if (rp->second.depth_format != 0) {
                if (!c.has_depth_clear || c.depth_clear < 0.0 || c.depth_clear > 1.0) {
                    resp.ok = false;
                    resp.reason =
                        "begin_render_pass on a depth render pass needs a depth clear in [0,1]";
                    return resp;
                }
            } else if (c.has_depth_clear) {
                resp.ok = false;
                resp.reason = "begin_render_pass carries a depth clear but the render pass has no "
                              "depth attachment";
                return resp;
            }
            // Render area: non-negative offset, positive extent, fully inside the framebuffer.
            if (c.render_area_x < 0 || c.render_area_y < 0 || c.render_area_w <= 0 ||
                c.render_area_h <= 0 || c.render_area_x + c.render_area_w > fb->second.width ||
                c.render_area_y + c.render_area_h > fb->second.height) {
                resp.ok = false;
                resp.reason = "begin_render_pass render area is empty or outside the framebuffer";
                return resp;
            }
            if (fb->second.imageless) {
                if (c.args_u64.size() != static_cast<std::size_t>(fb->second.attachment_count)) {
                    resp.ok = false;
                    resp.reason = "imageless begin_render_pass view count mismatch";
                    return resp;
                }
                for (const std::uint64_t view_handle : c.args_u64) {
                    const auto view = image_views_.find(view_handle);
                    if (view == image_views_.end() || view->second.device != device) {
                        resp.ok = false;
                        resp.reason = "imageless begin_render_pass view not on the device";
                        return resp;
                    }
                    const auto image = images_.find(view->second.image);
                    if (image != images_.end()) {
                        referenced_surfaces.insert(image->second.surface);
                        referenced_swapchains.insert(image->second.swapchain);
                    }
                    referenced_draw_objects.insert(view_handle);
                }
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
                referenced_draw_objects.insert(fb->second.image_view);
                if (fb->second.depth_image_view != 0) {
                    referenced_draw_objects.insert(fb->second.depth_image_view);
                }
            }
            referenced_draw_objects.insert(c.render_pass);
            referenced_draw_objects.insert(c.framebuffer);
            active_scope = RenderScope::RenderPass;
            active_rp_format = rp->second.color_format;
            active_rp_depth_format = rp->second.depth_format;
        } else if (k == CmdKind::EndRenderPass) {
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
        } else if (k == CmdKind::BindPipeline) {
            const auto pp = pipelines_.find(c.pipeline);
            if (pp == pipelines_.end() || pp->second.device != device) {
                resp.ok = false;
                resp.reason = "bind_pipeline references a pipeline not on the device";
                return resp;
            }
            // Compute: args_i64[0] = bindPoint (absent/empty = GRAPHICS, so old
            // recordings replay unchanged -- the bind_descriptor_sets precedent). The bind
            // point must MATCH the pipeline's kind (a graphics pipeline bound as COMPUTE or
            // vice versa rejects the record).
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
        } else if (k == CmdKind::SetViewport) {
            viewport_set = true; // dynamic state is sticky for the recording (per command buffer)
        } else if (k == CmdKind::SetScissor) {
            scissor_set = true;
        } else if (k == CmdKind::BindVertexBuffers) {
            // firstBinding 0, a non-empty equal-length buffers/offsets pair, each
            // buffer a VERTEX_BUFFER on the command buffer's device. The handles are baked into the
            // recording.
            if (c.first_binding != 0) {
                resp.ok = false;
                resp.reason = "bind_vertex_buffers requires firstBinding 0";
                return resp;
            }
            if (c.vertex_buffers.empty() ||
                c.vertex_buffers.size() != c.vertex_buffer_offsets.size()) {
                resp.ok = false;
                resp.reason = "bind_vertex_buffers buffers/offsets are empty or mismatched";
                return resp;
            }
            for (std::size_t i = 0; i < c.vertex_buffers.size(); ++i) {
                const auto bf = buffers_.find(c.vertex_buffers[i]);
                if (bf == buffers_.end() || bf->second.device != device) {
                    resp.ok = false;
                    resp.reason = "bind_vertex_buffers references a buffer not on the device";
                    return resp;
                }
                if ((bf->second.usage & kBufferUsageVertexBuffer) == 0) {
                    resp.ok = false;
                    resp.reason = "bind_vertex_buffers buffer lacks VERTEX_BUFFER usage";
                    return resp;
                }
                bound_vertex_bindings.insert(c.first_binding + static_cast<int>(i));
                referenced_draw_objects.insert(c.vertex_buffers[i]); // destroy invalidates this CB
            }
        } else if (k == CmdKind::BindDescriptorSets) {
            // GRAPHICS bind point, firstSet 0, no dynamic offsets. The pipeline layout
            // must be known + same-device; the bound set count must EQUAL the layout's
            // setLayoutCount (a zero-set-layout pipeline layout rejects the
            // bind), and each set must have been allocated from the EXACT set layout at that
            // index. The layout + set handles are baked into the recording (destroying
            // any invalidates the CB).
            if (c.first_set != 0) {
                resp.ok = false;
                resp.reason = "bind_descriptor_sets requires firstSet 0";
                return resp;
            }
            const auto pl = pipeline_layouts_.find(c.desc_layout);
            if (pl == pipeline_layouts_.end() || pl->second.device != device) {
                resp.ok = false;
                resp.reason = "bind_descriptor_sets references a pipeline layout not on the device";
                return resp;
            }
            if (pl->second.set_layouts.empty()) {
                resp.ok = false;
                resp.reason = "bind_descriptor_sets against a pipeline layout with no set layouts";
                return resp;
            }
            if (c.descriptor_sets.size() != pl->second.set_layouts.size()) {
                resp.ok = false;
                resp.reason = "bind_descriptor_sets count must equal the pipeline layout's "
                              "setLayoutCount";
                return resp;
            }
            for (std::size_t i = 0; i < c.descriptor_sets.size(); ++i) {
                const auto ds = descriptor_sets_.find(c.descriptor_sets[i]);
                if (ds == descriptor_sets_.end() || ds->second.device != device) {
                    resp.ok = false;
                    resp.reason = "bind_descriptor_sets references a set not on the device";
                    return resp;
                }
                if (ds->second.layout != pl->second.set_layouts[i]) {
                    resp.ok = false;
                    resp.reason =
                        "bound descriptor set was not allocated from the pipeline layout's "
                        "set layout at that index";
                    return resp;
                }
                referenced_draw_objects.insert(c.descriptor_sets[i]); // destroy invalidates this CB
            }
            referenced_draw_objects.insert(c.desc_layout);
            // Compute: the carried bind point (args_i64[0], absent = GRAPHICS)
            // selects WHICH bind point's descriptor record this replaces (per-point).
            const long long dbp = c.args_i64.empty() ? 0 : c.args_i64[0];
            if (dbp != 0 && dbp != 1) {
                resp.ok = false;
                resp.reason = "bind_descriptor_sets bind point must be GRAPHICS or COMPUTE";
                return resp;
            }
            bound_desc_layout_by_point[dbp] = c.desc_layout;
            bound_desc_sets_by_point[dbp] = c.descriptor_sets;
        } else if (k == CmdKind::BindIndexBuffer) {
            if (c.args_u64.size() < 2 || c.args_i64.empty()) {
                resp.ok = false;
                resp.reason = "malformed bind_index_buffer";
                return resp;
            }
            const auto ibf = buffers_.find(c.args_u64[0]);
            if (ibf == buffers_.end() || ibf->second.device != device ||
                ibf->second.bound_memory == 0) {
                resp.ok = false;
                resp.reason = "bind_index_buffer references a buffer not live/bound on the device";
                return resp;
            }
            referenced_draw_objects.insert(c.args_u64[0]);
            index_bound = true;
        } else if (k == CmdKind::Draw || k == CmdKind::DrawIndirectByteCount ||
                   k == CmdKind::DrawIndirect || k == CmdKind::DrawIndexedIndirect) {
            if (active_scope == RenderScope::None) {
                resp.ok = false;
                resp.reason = "draw outside an active render pass";
                return resp;
            }
            if (k == CmdKind::DrawIndexedIndirect && !index_bound) {
                resp.ok = false;
                resp.reason = "draw_indexed_indirect without a bound index buffer";
                return resp;
            }
            // (GL/zink): the byte-count draw (glDrawTransformFeedback) shares full
            // draw-readiness, plus its own gates (mock == real).
            if (k == CmdKind::DrawIndirectByteCount &&
                mdev->second.enabled_exts.count("VK_EXT_transform_feedback") == 0) {
                resp.ok = false;
                resp.reason = "draw_indirect_byte_count requires VK_EXT_transform_feedback "
                              "enabled on the device";
                return resp;
            }
            // A graphics pipeline must be bound, and it must be compatible with the active render
            // pass (Vulkan: render-pass-compatible; reduces to matching color format).
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
            // for the OTHER scope kind is rejected (a DR pipeline in a render pass, or vice versa).
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
            // The pipeline declares VIEWPORT+SCISSOR dynamic, so both must be set before a draw.
            if (!viewport_set || !scissor_set) {
                resp.ok = false;
                resp.reason = "draw without the required dynamic viewport/scissor set";
                return resp;
            }
            // a pipeline that declares vertex bindings needs each bound;
            // a bufferless pipeline (count 0) imposes no requirement.
            for (int vb = 0; vb < pp->second.vertex_binding_count; ++vb) {
                if (bound_vertex_bindings.count(vb) == 0) {
                    resp.ok = false;
                    resp.reason =
                        "draw with a pipeline that needs a vertex buffer, but the binding "
                        "was not bound";
                    return resp;
                }
            }
            // a pipeline whose layout declares set layouts requires a layout-exact
            // descriptor bind whose sets are all draw-ready. A layout with no set
            // layouts imposes no requirement.
            const auto pp_pl = pipeline_layouts_.find(pp->second.layout);
            if (pp_pl != pipeline_layouts_.end() && !pp_pl->second.set_layouts.empty()) {
                if (bound_desc_layout_by_point[0] != pp->second.layout ||
                    bound_desc_sets_by_point[0].size() != pp_pl->second.set_layouts.size()) {
                    resp.ok = false;
                    resp.reason = "draw without a descriptor bind matching the pipeline's layout";
                    return resp;
                }
                for (const std::uint64_t set_handle : bound_desc_sets_by_point[0]) {
                    if (!descriptor_set_draw_ready(set_handle, device, resp.reason)) {
                        resp.ok = false;
                        return resp;
                    }
                }
            }
            if (k == CmdKind::DrawIndirectByteCount) {
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
                referenced_draw_objects.insert(c.args_u64[0]);
            } else if (k == CmdKind::DrawIndirect || k == CmdKind::DrawIndexedIndirect) {
                if (c.args_u64.size() != 1 || c.args_i64.size() != 2) {
                    resp.ok = false;
                    resp.reason = "malformed core indirect draw command";
                    return resp;
                }
                const auto ibf = buffers_.find(c.src_buffer);
                const bool live = ibf != buffers_.end() && ibf->second.device == device;
                const bool bound = live && ibf->second.bound_memory != 0;
                const bool usage = live && (ibf->second.usage & kBufferUsageIndirectBuffer) != 0;
                const char* why = "";
                if (!core_indirect_draw_ok(
                        live, bound, usage, live ? ibf->second.size : 0, c.args_u64[0],
                        c.args_i64[0], c.args_i64[1],
                        k == CmdKind::DrawIndexedIndirect ? kDrawIndexedIndirectCommandBytes
                                                          : kDrawIndirectCommandBytes,
                        mdev->second.multi_draw_indirect_feature_enabled, &why)) {
                    resp.ok = false;
                    resp.reason = why;
                    return resp;
                }
                referenced_draw_objects.insert(c.src_buffer);
            } else if (c.vertex_count < 0 || c.instance_count < 0 || c.first_vertex < 0 ||
                       c.first_instance < 0) {
                // u32 draw args carried wide; a missing/negative value (-1 sentinel) is rejected.
                resp.ok = false;
                resp.reason = "malformed draw command";
                return resp;
            }
        } else if (k == CmdKind::Dispatch || k == CmdKind::DispatchIndirect) {
            // Compute. Dispatch is legal OUTSIDE a render pass only, with a COMPUTE
            // pipeline bound and (when its layout declares sets) a layout-exact COMPUTE-point
            // descriptor bind whose sets are all ready -- the draw treatment, per bind point.
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
                if (bound_desc_layout_by_point[1] != pp->second.layout ||
                    bound_desc_sets_by_point[1].size() != pp_pl->second.set_layouts.size()) {
                    resp.ok = false;
                    resp.reason =
                        "dispatch without a descriptor bind matching the pipeline's layout";
                    return resp;
                }
                for (const std::uint64_t set_handle : bound_desc_sets_by_point[1]) {
                    if (!descriptor_set_draw_ready(set_handle, device, resp.reason)) {
                        resp.ok = false;
                        return resp;
                    }
                }
            }
            if (k == CmdKind::Dispatch) {
                // args_u64 = [x, y, z]. Zero dimensions are a LEGAL no-op (spec: only the
                // maxComputeWorkGroupCount ceiling applies); the mock gates at the spec
                // minimum-maximum (65535) so a mock-passing stream stays real-plausible.
                if (c.args_u64.size() != 3) {
                    resp.ok = false;
                    resp.reason = "malformed dispatch command";
                    return resp;
                }
                for (const std::uint64_t g : c.args_u64) {
                    if (g > 65535) {
                        resp.ok = false;
                        resp.reason = "dispatch group count exceeds the spec-minimum limit";
                        return resp;
                    }
                }
            } else { // dispatch_indirect
                // src_buffer = the indirect buffer; args_u64 = [offset]. The buffer must be
                // live + bound + INDIRECT usage; offset 4-aligned with 12 bytes in range.
                if (c.args_u64.size() != 1) {
                    resp.ok = false;
                    resp.reason = "malformed dispatch_indirect command";
                    return resp;
                }
                const auto ibf = buffers_.find(c.src_buffer);
                if (ibf == buffers_.end() || ibf->second.device != device ||
                    ibf->second.bound_memory == 0) {
                    resp.ok = false;
                    resp.reason = "dispatch_indirect buffer is not live/bound on the device";
                    return resp;
                }
                if ((ibf->second.usage & kBufferUsageIndirectBuffer) == 0) {
                    resp.ok = false;
                    resp.reason = "dispatch_indirect buffer lacks INDIRECT_BUFFER usage";
                    return resp;
                }
                const std::uint64_t off = c.args_u64[0];
                if (off % 4 != 0 || off + 12 > ibf->second.size || off + 12 < off) {
                    resp.ok = false;
                    resp.reason = "dispatch_indirect offset misaligned or out of range";
                    return resp;
                }
                referenced_draw_objects.insert(c.src_buffer);
            }
        } else if (k == CmdKind::MemoryBarrierGlobal) {
            // Compute: a GLOBAL VkMemoryBarrier -- the stage/access
            // fields only. Outside-render-pass only (in-pass barriers need subpass
            // self-dependencies this subset does not model).
            if (active_scope != RenderScope::None) {
                resp.ok = false;
                resp.reason = "memory_barrier inside an active render pass";
                return resp;
            }
            if (!valid_stage_mask_or_none(c.src_stage) || !valid_stage_mask_or_none(c.dst_stage) ||
                !valid_access_mask(c.src_access) || !valid_access_mask(c.dst_access)) {
                resp.ok = false;
                resp.reason = "malformed memory_barrier command";
                return resp;
            }
        } else if (k == CmdKind::BufferMemoryBarrier) {
            // Compute: one VkBufferMemoryBarrier. src_buffer = the
            // buffer; args_u64 = [offset, size] (size ~0 = VK_WHOLE_SIZE). Ownership transfers
            // were rejected at the ICD (both family indices must be IGNORED).
            if (active_scope != RenderScope::None) {
                resp.ok = false;
                resp.reason = "buffer_memory_barrier inside an active render pass";
                return resp;
            }
            if (!valid_stage_mask_or_none(c.src_stage) || !valid_stage_mask_or_none(c.dst_stage) ||
                !valid_access_mask(c.src_access) || !valid_access_mask(c.dst_access) ||
                c.args_u64.size() != 2) {
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
            if (off >= bbf->second.size ||
                (sz != ~0ull && (off + sz > bbf->second.size || off + sz < off)) || sz == 0) {
                resp.ok = false;
                resp.reason = "buffer_memory_barrier range is out of the buffer's bounds";
                return resp;
            }
            referenced_draw_objects.insert(c.src_buffer);
        } else if (k == CmdKind::CmdSetEvent || k == CmdKind::CmdResetEvent) {
            // sync1 vkCmdSetEvent / vkCmdResetEvent -- event handle (args_u64[0]) + a
            // non-zero 32-bit stageMask (src_stage). Not inside a render pass instance (spec VUID).
            if (active_scope != RenderScope::None) {
                resp.ok = false;
                resp.reason = "set_event/reset_event inside an active render pass";
                return resp;
            }
            if (c.args_u64.size() != 1 || !valid_stage_mask(c.src_stage)) {
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
            referenced_draw_objects.insert(c.args_u64[0]);
        } else if (k == CmdKind::CmdWaitEvents) {
            // the atomic sync1 vkCmdWaitEvents -- one command carrying the event SET plus
            // the three barrier arrays, flattened fixed-slot into args_u64 (header [eventCount,
            // memCount, bufCount, imgCount], then events, memory[2], buffer[5], image[10]).
            // src_stage/dst_stage are the global stage masks. Outside a render pass.
            if (active_scope != RenderScope::None) {
                resp.ok = false;
                resp.reason = "wait_events inside an active render pass";
                return resp;
            }
            if (c.args_u64.size() < kWaitEventsHeaderSlots || !valid_stage_mask(c.src_stage) ||
                !valid_stage_mask(c.dst_stage)) {
                resp.ok = false;
                resp.reason = "malformed wait_events command";
                return resp;
            }
            const std::uint64_t ev_count = c.args_u64[0];
            const std::uint64_t mem_count = c.args_u64[1];
            const std::uint64_t buf_count = c.args_u64[2];
            const std::uint64_t img_count = c.args_u64[3];
            if (ev_count > kMaxWaitEventsBarriers || mem_count > kMaxWaitEventsBarriers ||
                buf_count > kMaxWaitEventsBarriers || img_count > kMaxWaitEventsBarriers ||
                ev_count == 0) {
                resp.ok = false;
                resp.reason = "wait_events count is zero or exceeds the supported cap";
                return resp;
            }
            const std::uint64_t expect =
                kWaitEventsHeaderSlots + ev_count + mem_count * kWaitEventsMemorySlots +
                buf_count * kWaitEventsBufferSlots + img_count * kWaitEventsImageSlots;
            if (c.args_u64.size() != expect) {
                resp.ok = false;
                resp.reason = "wait_events payload length does not match its header counts";
                return resp;
            }
            std::size_t cur = kWaitEventsHeaderSlots;
            for (std::uint64_t i = 0; i < ev_count; ++i) {
                const std::uint64_t ev_h = c.args_u64[cur++];
                const auto ev = leaves_.find(ev_h);
                if (ev == leaves_.end() || ev->second.kind != LeafKind::Event ||
                    ev->second.device != device) {
                    resp.ok = false;
                    resp.reason = "wait_events references an event not on the device";
                    return resp;
                }
                referenced_draw_objects.insert(ev_h);
            }
            for (std::uint64_t i = 0; i < mem_count; ++i) {
                const long long src_access = static_cast<long long>(c.args_u64[cur++]);
                const long long dst_access = static_cast<long long>(c.args_u64[cur++]);
                if (!valid_access_mask(src_access) || !valid_access_mask(dst_access)) {
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
                if (!valid_access_mask(src_access) || !valid_access_mask(dst_access)) {
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
                referenced_draw_objects.insert(buf_h);
            }
            for (std::uint64_t i = 0; i < img_count; ++i) {
                const long long src_access = static_cast<long long>(c.args_u64[cur++]);
                const long long dst_access = static_cast<long long>(c.args_u64[cur++]);
                const std::uint64_t old_layout = c.args_u64[cur++];
                const std::uint64_t new_layout = c.args_u64[cur++];
                const std::uint64_t img_h = c.args_u64[cur++];
                const std::uint64_t aspect = c.args_u64[cur++];
                cur +=
                    4; // baseMip, levelCount, baseLayer, layerCount (carried; range not re-checked)
                (void) old_layout;
                (void) new_layout;
                if (!valid_access_mask(src_access) || !valid_access_mask(dst_access) ||
                    aspect < 1) {
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
                referenced_draw_objects.insert(img_h);
            }
        } else if (k == CmdKind::PipelineBarrier2 || k == CmdKind::WriteTimestamp2 ||
                   k == CmdKind::CmdSetEvent2 || k == CmdKind::CmdResetEvent2 ||
                   k == CmdKind::CmdWaitEvents2) {
            // the VK_KHR_synchronization2 commands. Gate on the extension AND the
            // feature -- mock == real (the real backend checks both, so the mock
            // must too; feature-without-extension rejects).
            // Vulkan 1.3 support: on an honest vk13 device sync2 is CORE -- the feature alone
            // admits it.
            if ((mdev->second.enabled_exts.count(kSync2ExtensionName) == 0 &&
                 !mdev->second.vk13_device) ||
                !mdev->second.synchronization2_feature_enabled) {
                resp.ok = false;
                resp.reason = "synchronization2 command requires the synchronization2 feature";
                return resp;
            }
            // A DependencyInfo2's structural checks (shared) + buffer/image handle liveness on the
            // CB's device + join to referenced_draw_objects. Returns false + sets resp.reason.
            const auto check_dep2 = [&](const DependencyInfo2& d) -> bool {
                if (!validate_dependency_info2(d, resp.reason)) {
                    return false;
                }
                for (const BufferMemoryBarrier2& b : d.buffer) {
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
                    referenced_draw_objects.insert(b.buffer);
                }
                for (const ImageMemoryBarrier2& im : d.image) {
                    const auto img = images_.find(im.image);
                    if (img == images_.end() || img->second.device != device) {
                        resp.reason = "sync2 image barrier references an image not on the device";
                        return false;
                    }
                    if (img->second.app_created && img->second.bound_memory == 0) {
                        resp.reason = "sync2 image barrier references an app image not bound";
                        return false;
                    }
                    referenced_draw_objects.insert(im.image);
                }
                return true;
            };
            // The event commands must be outside a render pass (spec); barrier2 / write_timestamp2
            // are legal in or out, so they carry no scope restriction.
            const bool is_event2 = k == CmdKind::CmdSetEvent2 || k == CmdKind::CmdResetEvent2 ||
                                   k == CmdKind::CmdWaitEvents2;
            if (is_event2 && active_scope != RenderScope::None) {
                resp.ok = false;
                resp.reason = "sync2 event command inside an active render pass";
                return resp;
            }
            if (k == CmdKind::PipelineBarrier2) {
                if (c.deps2.size() != 1 || !c.args_u64.empty()) {
                    resp.ok = false;
                    resp.reason = "malformed pipeline_barrier2 command";
                    return resp;
                }
                if (!check_dep2(c.deps2[0])) {
                    resp.ok = false;
                    return resp;
                }
            } else if (k == CmdKind::WriteTimestamp2) {
                if (c.args_u64.size() != 2 || c.args_i64.size() != 1 || !c.deps2.empty() ||
                    !valid_timestamp_stage2(c.args_u64[1])) {
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
                referenced_draw_objects.insert(c.args_u64[0]);
            } else if (k == CmdKind::CmdSetEvent2) {
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
                referenced_draw_objects.insert(c.args_u64[0]);
                if (!check_dep2(c.deps2[0])) {
                    resp.ok = false;
                    return resp;
                }
            } else if (k == CmdKind::CmdResetEvent2) {
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
                referenced_draw_objects.insert(c.args_u64[0]);
            } else { // CmdWaitEvents2: N events paired with N dependencies.
                if (c.args_u64.empty() || c.deps2.size() != c.args_u64.size() ||
                    c.deps2.size() > kMaxSync2Dependencies) {
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
                    referenced_draw_objects.insert(c.args_u64[i]);
                    if (!check_dep2(c.deps2[i])) {
                        resp.ok = false;
                        return resp;
                    }
                }
            }
        } else if (k == CmdKind::CopyBufferToImage) {
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
            if ((cimg->second.usage & static_cast<std::uint64_t>(kImageUsageTransferDst)) == 0) {
                resp.ok = false;
                resp.reason = "copy_buffer_to_image dest image lacks TRANSFER_DST usage";
                return resp;
            }
            // (No image-level COLOR-only gate: a depth/stencil destination image is legal now. The
            // per-region check below enforces the region's aspect is one the image actually has --
            // mock == real.)
            const auto sbf = buffers_.find(c.src_buffer);
            if (sbf == buffers_.end() || sbf->second.device != device) {
                resp.ok = false;
                resp.reason = "copy_buffer_to_image source buffer is not on the device";
                return resp;
            }
            if ((sbf->second.usage & kBufferUsageTransferSrc) == 0) {
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
            if (c.args_i64[0] != kImageLayoutTransferDstOptimal &&
                c.args_i64[0] != kImageLayoutGeneral) {
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
                // combined-DS image takes DEPTH or STENCIL, one per region (never both -- rejected
                // by the single-bit rule). Mirrors the real worker exactly.
                const bool single_aspect = aspect == kImageAspectColor ||
                                           aspect == kImageAspectDepth ||
                                           aspect == kImageAspectStencil;
                if (!single_aspect || (aspect & cimg->second.aspect) != aspect) {
                    resp.ok = false;
                    resp.reason = "copy_buffer_to_image region aspect must be a single aspect "
                                  "present in the destination image";
                    return resp;
                }
                if (mip < 0 || mip >= cimg->second.mip_levels) {
                    resp.ok = false;
                    resp.reason = "copy_buffer_to_image region mip level is out of the image's "
                                  "range";
                    return resp;
                }
                if (base_layer < 0 || layers < 1 ||
                    base_layer + layers > cimg->second.array_layers) {
                    resp.ok = false;
                    resp.reason = "copy_buffer_to_image region layer range is out of the image's "
                                  "range";
                    return resp;
                }
                // The mip-level extent: max(1, base >> mip) per axis (2D images: depth stays 1).
                const std::uint64_t mip_w =
                    std::max<std::uint64_t>(1, static_cast<std::uint64_t>(cimg->second.width) >>
                                                   static_cast<unsigned>(mip));
                const std::uint64_t mip_h =
                    std::max<std::uint64_t>(1, static_cast<std::uint64_t>(cimg->second.height) >>
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
            referenced_draw_objects.insert(c.src_buffer);
            referenced_draw_objects.insert(c.image);
        } else if (k == CmdKind::CopyBuffer) {
            // GL/zink: faithful buffer->buffer copy. args_u64 = [src, dst, (srcOff,
            // dstOff, size) x N]. Both buffers must be live + bound on the device; both bake into
            // the CB.
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
            referenced_draw_objects.insert(c.args_u64[0]);
            referenced_draw_objects.insert(c.args_u64[1]);
        } else if (k == CmdKind::FillBuffer) {
            // faithful vkCmdFillBuffer (Mesa >= 25 zink: GL buffer clears). args_u64 =
            // [dstBuffer, dstOffset, size, data]. A transfer command -- outside any render pass;
            // spec alignment: dstOffset %4 == 0; size is VK_WHOLE_SIZE or a positive multiple of
            // 4. The buffer must be live + bound on the device and bakes into the CB (mock ==
            // real validation, no execution).
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
            referenced_draw_objects.insert(c.args_u64[0]);
        } else if (k == CmdKind::CopyImageToBuffer) {
            // (GL/zink): image->buffer readback (glReadPixels for offscreen PNG export) -- mock
            // == real validation (a transfer command outside any render pass, exact payload shape,
            // live source image + live/bound dest buffer), no execution. args_u64=[srcImage,
            // dstBuffer]; args_i64=[srcImageLayout, regionCount, 13 per region]. The source may be
            // an app image (must be memory-bound) OR a swapchain image (swapchain-owned, no
            // app-bound memory) -- glReadPixels off the default framebuffer reads the swapchain
            // image. Both bake into the CB.
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
            // range lesson): the payload after [layout, regionCount] must be exactly 13 i64 per
            // region, and regionCount must match. A negative regionCount casts huge -> mismatch.
            const auto region_count = static_cast<std::size_t>(c.args_i64[1]);
            const std::size_t tail = c.args_i64.size() - 2;
            if (tail % 13 != 0 || region_count != tail / 13) {
                resp.ok = false;
                resp.reason = "copy_image_to_buffer region payload malformed";
                return resp;
            }
            referenced_draw_objects.insert(c.args_u64[0]);
            referenced_draw_objects.insert(c.args_u64[1]);
        } else if (k == CmdKind::ClearAttachments) {
            // (GL/zink): faithful IN-RENDER-PASS scissored clear -- mock == real validation
            // (an active render pass + the exact payload shape), no execution.
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
        } else if (k == CmdKind::BlitImage) {
            // (GL/zink): faithful image->image blit -- mock == real validation (outside any
            // render pass, exact payload shape, both images live + bound), no execution.
            // args_u64=[srcImage, dstImage]; args_i64=[srcLayout, dstLayout, filter, regionCount,
            // 20 per region].
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
                (c.args_i64[2] != 0 && c.args_i64[2] != 1)) { // VK_FILTER_NEAREST / _LINEAR
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
            for (const std::uint64_t ih : {c.args_u64[0], c.args_u64[1]}) {
                const auto& rec = images_.find(ih)->second;
                referenced_surfaces.insert(rec.surface);
                referenced_swapchains.insert(rec.swapchain);
                referenced_draw_objects.insert(ih);
            }
        } else if (k == CmdKind::CopyImage) {
            // (GL/zink): faithful unscaled image->image copy -- mock == real validation
            // (outside any render pass, exact payload shape, both images live + bound), no
            // execution. args_u64=[srcImage, dstImage]; args_i64=[srcLayout, dstLayout,
            // regionCount, 17 per region].
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
            for (const std::uint64_t ih : {c.args_u64[0], c.args_u64[1]}) {
                const auto& rec = images_.find(ih)->second;
                referenced_surfaces.insert(rec.surface);
                referenced_swapchains.insert(rec.swapchain);
                referenced_draw_objects.insert(ih);
            }
        } else if (k == CmdKind::BindTransformFeedbackBuffers) {
            // (GL/zink): mock == real (see the real backend for the spec rationale).
            if (mdev->second.enabled_exts.count("VK_EXT_transform_feedback") == 0) {
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
            for (std::size_t i = 0; i < count; ++i) {
                const auto bf = buffers_.find(c.args_u64[i]);
                if (bf == buffers_.end() || bf->second.device != device ||
                    bf->second.bound_memory == 0) {
                    resp.ok = false;
                    resp.reason = "bind_transform_feedback_buffers references a buffer not "
                                  "live/bound on the device";
                    return resp;
                }
                referenced_draw_objects.insert(c.args_u64[i]);
            }
        } else if (k == CmdKind::BeginTransformFeedback || k == CmdKind::EndTransformFeedback) {
            // (GL/zink): mock == real. XFB scope inside an active render pass, balanced.
            const bool is_begin = k == CmdKind::BeginTransformFeedback;
            if (mdev->second.enabled_exts.count("VK_EXT_transform_feedback") == 0) {
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
            for (std::size_t i = 0; i < count; ++i) {
                if (c.args_u64[i] == 0) {
                    continue; // no counter for this binding
                }
                const auto bf = buffers_.find(c.args_u64[i]);
                if (bf == buffers_.end() || bf->second.device != device ||
                    bf->second.bound_memory == 0) {
                    resp.ok = false;
                    resp.reason = "transform feedback counter buffer is not live/bound on the "
                                  "device";
                    return resp;
                }
                referenced_draw_objects.insert(c.args_u64[i]);
            }
            xfb_active = is_begin;
        } else if (k == CmdKind::BeginConditionalRendering) {
            // (GL/zink): mock == real. No nesting; scope symmetry enforced at end/stream-end.
            if (mdev->second.enabled_exts.count("VK_EXT_conditional_rendering") == 0) {
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
            referenced_draw_objects.insert(c.args_u64[0]);
        } else if (k == CmdKind::EndConditionalRendering) {
            if (mdev->second.enabled_exts.count("VK_EXT_conditional_rendering") == 0) {
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
            if (cond_render_began_in_rp != (active_scope != RenderScope::None)) {
                resp.ok = false;
                resp.reason = "end_conditional_rendering in a different render-pass scope than "
                              "its begin";
                return resp;
            }
            cond_render_active = false;
        } else if (k == CmdKind::ResetQueryPool || k == CmdKind::BeginQuery ||
                   k == CmdKind::EndQuery || k == CmdKind::WriteTimestamp) {
            // Query pools (mock == real): args_u64=[query_pool]; args_i64 per kind (see the real
            // backend). Pool live on device, indices in range, begin/end balanced.
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
            if (k == CmdKind::ResetQueryPool) {
                if (c.args_i64.size() != 2 || c.args_i64[0] < 0 || c.args_i64[1] <= 0 ||
                    static_cast<std::uint64_t>(c.args_i64[0]) +
                            static_cast<std::uint64_t>(c.args_i64[1]) >
                        qp->second.count) {
                    resp.ok = false;
                    resp.reason = "reset_query_pool range out of bounds";
                    return resp;
                }
            } else if (k == CmdKind::BeginQuery) {
                if (c.args_i64.size() != 2 || !index_ok(c.args_i64[0])) {
                    resp.ok = false;
                    resp.reason = "begin_query index out of bounds";
                    return resp;
                }
                if (!active_queries
                         .insert(std::make_pair(c.args_u64[0],
                                                static_cast<std::uint32_t>(c.args_i64[0])))
                         .second) {
                    resp.ok = false;
                    resp.reason = "begin_query on a query already active in this command buffer";
                    return resp;
                }
            } else if (k == CmdKind::EndQuery) {
                if (c.args_i64.size() != 1 || !index_ok(c.args_i64[0])) {
                    resp.ok = false;
                    resp.reason = "end_query index out of bounds";
                    return resp;
                }
                if (active_queries.erase(std::make_pair(
                        c.args_u64[0], static_cast<std::uint32_t>(c.args_i64[0]))) == 0) {
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
            referenced_draw_objects.insert(c.args_u64[0]);
        } else if (k == CmdKind::CopyQueryPoolResults) {
            // zink's query-result read path (mock == real): pool live + dst buffer live/bound +
            // range in bounds; outside any render pass. args_u64=[query_pool, dst_buffer];
            // args_i64=[first, count, dstOffset, stride, flags].
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
            referenced_draw_objects.insert(c.args_u64[0]);
            referenced_draw_objects.insert(c.args_u64[1]);
        } else if (k == CmdKind::SetCullMode || k == CmdKind::SetFrontFace ||
                   k == CmdKind::SetPrimitiveTopology || k == CmdKind::SetDepthTestEnable ||
                   k == CmdKind::SetDepthWriteEnable || k == CmdKind::SetDepthCompareOp ||
                   k == CmdKind::SetDepthBoundsTestEnable || k == CmdKind::SetStencilTestEnable) {
            // (native lane -- EDS1): VK_EXT_extended_dynamic_state setters. A single u32
            // in args_i64[0]; no handle resolution, no draw-readiness impact -- just baked +
            // emitted (mock == real; the real backend replays via the *EXT PFNs). The extension
            // MUST be enabled (mirrors the real backend's PFN gate, so a command is
            // only accepted where its replay entrypoint truly exists). The enum VALUE passes
            // through to the host driver / validation layer; only the arg-count shape is checked.
            // Vulkan 1.3 support: an honest vk13 device admits them too (they are core there).
            if (mdev->second.enabled_exts.count("VK_EXT_extended_dynamic_state") == 0 &&
                !mdev->second.vk13_device) {
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
        } else if (k == CmdKind::SetStencilOp) {
            // Vulkan 1.3 support (EDS1): args_i64=[faceMask, failOp, passOp, depthFailOp,
            // compareOp]; same extension-or-vk13 gate as the single-u32 setters (mock == real).
            if (mdev->second.enabled_exts.count("VK_EXT_extended_dynamic_state") == 0 &&
                !mdev->second.vk13_device) {
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
        } else if (k == CmdKind::SetViewportWithCount || k == CmdKind::SetScissorWithCount) {
            // Vulkan 1.3 support (EDS1): the with-count pair (mock == real validation, no
            // execution). set_viewport_with_count: args_i64=[count], args_f64 = 6 per viewport;
            // set_scissor_with_count: args_i64=[count, then x, y, w, h per scissor].
            if (mdev->second.enabled_exts.count("VK_EXT_extended_dynamic_state") == 0 &&
                !mdev->second.vk13_device) {
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
                k == CmdKind::SetViewportWithCount
                    ? (c.args_i64.size() == 1 && c.args_f64.size() == wc_count * 6)
                    : (c.args_i64.size() == 1 + wc_count * 4);
            if (!wc_shape_ok) {
                resp.ok = false;
                resp.reason = "with-count dynamic-state payload malformed";
                return resp;
            }
            // A valid with-count set satisfies the draw gate exactly like the classic command
            // (mock == real; Mesa >= 25 zink records ONLY the with-count pair on a 1.3 device).
            if (k == CmdKind::SetViewportWithCount) {
                viewport_set = true;
            } else {
                scissor_set = true;
            }
        } else if (k == CmdKind::BindVertexBuffers2) {
            // Vulkan 1.3 support (EDS1): vkCmdBindVertexBuffers2 (mock == real validation).
            // args_i64=[firstBinding, count, has_sizes, has_strides]; args_u64=[buffers x N,
            // offsets x N, sizes x N?, strides x N?]. The buffer rules mirror the mock's
            // bind_vertex_buffers (live + bound on the device, VERTEX_BUFFER usage, handles
            // baked, destroy invalidates this CB).
            if (mdev->second.enabled_exts.count("VK_EXT_extended_dynamic_state") == 0 &&
                !mdev->second.vk13_device) {
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
            for (std::size_t i = 0; i < count; ++i) {
                const auto bf = buffers_.find(c.args_u64[i]);
                if (bf == buffers_.end() || bf->second.device != device ||
                    bf->second.bound_memory == 0) {
                    resp.ok = false;
                    resp.reason = "bind_vertex_buffers2 references a buffer not live/bound on "
                                  "the device";
                    return resp;
                }
                if ((bf->second.usage & kBufferUsageVertexBuffer) == 0) {
                    resp.ok = false;
                    resp.reason = "bind_vertex_buffers2 buffer lacks VERTEX_BUFFER usage";
                    return resp;
                }
                const std::uint64_t offset = c.args_u64[count + i];
                if (offset >= bf->second.size) {
                    resp.ok = false;
                    resp.reason = "bind_vertex_buffers2 offset is outside the logical buffer";
                    return resp;
                }
                if (c.args_i64[2] == 1) {
                    const std::uint64_t size = c.args_u64[count * 2 + i];
                    if (size != kVkWholeSize && (size == 0 || size > bf->second.size - offset)) {
                        resp.ok = false;
                        resp.reason = "bind_vertex_buffers2 size is outside the logical buffer";
                        return resp;
                    }
                }
                bound_vertex_bindings.insert(static_cast<int>(c.args_i64[0]) + static_cast<int>(i));
                referenced_draw_objects.insert(c.args_u64[i]); // destroy invalidates this CB
            }
        } else if (k == CmdKind::SetRasterizerDiscardEnable || k == CmdKind::SetDepthBiasEnable ||
                   k == CmdKind::SetPrimitiveRestartEnable) {
            // Vulkan 1.3 support (EDS2 subset): the three enable-toggles core 1.3 absorbed from
            // VK_EXT_extended_dynamic_state2 -- served on the honest vk13 device ONLY (mock ==
            // real; the extension is not advertised). args_i64=[enable(0/1)].
            if (!mdev->second.vk13_device) {
                resp.ok = false;
                resp.reason = "core-1.3 dynamic state requires the honest 1.3 device";
                return resp;
            }
            if (c.args_i64.size() != 1) {
                resp.ok = false;
                resp.reason = "malformed core-1.3 dynamic-state set command";
                return resp;
            }
        } else if (k == CmdKind::ResolveImage) {
            // Vulkan 1.3 support: faithful MSAA image resolve -- mock == real validation (outside
            // any render pass, exact payload shape, both images live + bound), no execution.
            // args_u64=[srcImage, dstImage]; args_i64=[srcLayout, dstLayout, regionCount, 17 per
            // region].
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
            for (const std::uint64_t ih : {c.args_u64[0], c.args_u64[1]}) {
                const auto& rec = images_.find(ih)->second;
                referenced_surfaces.insert(rec.surface);
                referenced_swapchains.insert(rec.swapchain);
                referenced_draw_objects.insert(ih);
            }
        } else if (k == CmdKind::BeginRendering) {
            // (native lane): VK_KHR_dynamic_rendering. Gate on the enabled
            // extension (mock == real; a command is only accepted where its replay entrypoint truly
            // exists). args_i64 = [flags, area.x, area.y, area.w, area.h, layerCount, viewMask,
            // colorCount, dsPresence] then per attachment (color[0..n-1], depth?, stencil?):
            // [imageLayout, loadOp, storeOp]; args_u64 = one guest image-view handle per attachment
            // (0 = a real null attachment, NOT looked up); args_blob = one raw VkClearValue per
            // attachment. The active scope's attachment formats (from the views) become the
            // draw-time DR compatibility key. Fail-closed envelope: no flags
            // (suspend/resume/ secondary), no multiview (viewMask), positive layerCount.
            // Vulkan 1.3 support: on an honest vk13 device DR is CORE -- the feature alone admits
            // it.
            if ((mdev->second.enabled_exts.count("VK_KHR_dynamic_rendering") == 0 &&
                 !mdev->second.vk13_device) ||
                !mdev->second.dynamic_rendering_feature_enabled) {
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
            // Required-feature audit: a dynamic-rendering viewMask is admitted only on a
            // multiview-enabled device (mock == real; the worker carries it to the host
            // VkRenderingInfo.viewMask). A negative mask is a malformed frame.
            if (dr_view_mask != 0) {
                if (!mdev->second.multiview_feature_enabled) {
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
            if (dr_color_count < 0 || dr_color_count > kMaxDynamicRenderingColorAttachments ||
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
            if (c.args_i64.size() != 9 + static_cast<std::size_t>(n_attach) * 3 ||
                c.args_u64.size() != n_attach ||
                c.args_blob.size() != n_attach * kClearValueBytes) {
                resp.ok = false;
                resp.reason = "malformed begin_rendering attachment payload";
                return resp;
            }
            std::vector<int> dr_color_formats;
            dr_color_formats.reserve(static_cast<std::size_t>(dr_color_count));
            int dr_depth_format = 0;
            int dr_stencil_format = 0;
            for (std::size_t i = 0; i < n_attach; ++i) {
                const std::uint64_t view_h = c.args_u64[i];
                int fmt = 0;
                if (view_h != 0) {
                    const auto iv = image_views_.find(view_h);
                    if (iv == image_views_.end() || iv->second.device != device) {
                        resp.ok = false;
                        resp.reason = "begin_rendering attachment view not on the device";
                        return resp;
                    }
                    fmt = iv->second.format;
                    referenced_draw_objects.insert(view_h); // destroy invalidates this CB
                    // Required-feature audit: multiview layer-sufficiency on each DR
                    // attachment (mock == real; the worker's host is the authoritative enforcer).
                    const auto vimg = images_.find(iv->second.image);
                    if (vimg != images_.end() &&
                        vimg->second.array_layers <
                            multiview_required_layers(static_cast<int>(dr_view_mask))) {
                        resp.ok = false;
                        resp.reason = "begin_rendering attachment has too few array layers for the "
                                      "viewMask (multiview)";
                        return resp;
                    }
                }
                if (i < static_cast<std::size_t>(dr_color_count)) {
                    dr_color_formats.push_back(fmt);
                } else if (dr_has_depth && i == static_cast<std::size_t>(dr_color_count)) {
                    dr_depth_format = fmt;
                } else {
                    dr_stencil_format = fmt;
                }
            }
            active_scope = RenderScope::DynamicRendering;
            active_dr_color_formats = std::move(dr_color_formats);
            active_dr_depth_format = dr_depth_format;
            active_dr_stencil_format = dr_stencil_format;
            active_dr_view_mask = static_cast<int>(dr_view_mask);
        } else if (k == CmdKind::EndRendering) {
            if (active_scope != RenderScope::DynamicRendering) {
                resp.ok = false;
                resp.reason = "end_rendering without an active dynamic-rendering scope";
                return resp;
            }
            // Mirror the EndRenderPass guards -- a DR scope must not close with
            // transform feedback still active, nor with conditional rendering begun inside it (the
            // generalized state machine now lets both begin in ANY scope, so both ends must guard).
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
            active_dr_depth_format = 0;
            active_dr_stencil_format = 0;
            active_dr_view_mask = 0;
        } else {
            resp.ok = false;
            resp.reason = "unknown command kind";
            return resp;
        }
    }
    if (!active_queries.empty()) {
        resp.ok = false;
        resp.reason = "command stream ends with an active query (begin without end)";
        return resp;
    }
    if (active_scope != RenderScope::None) {
        resp.ok = false;
        resp.reason = "command stream ends inside an active render scope";
        return resp;
    }
    // Transform feedback must not survive the command buffer. It cannot escape a
    // render pass (EndRenderPass guards it) nor a DR scope (EndRendering guards it), so reaching
    // here with it active would be a scope-machine bug -- assert it fails closed rather than
    // replaying an invalid host stream.
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
    cb->second.recorded = true;
    cb->second.referenced_surfaces = std::move(referenced_surfaces);
    cb->second.referenced_swapchains = std::move(referenced_swapchains);
    cb->second.referenced_draw_objects = std::move(referenced_draw_objects);
    if (prof != nullptr) {
        prof->record_phases.validate_us += profile_now_us() - t_validate0;
    }
    resp.ok = true;
    resp.reason = "ok";
    return resp;
}

QueueSubmitResponse MockVulkanBackend::queue_submit(const QueueSubmitRequest& req) {
    QueueSubmitResponse resp;
    const auto qd = queue_to_device_.find(req.queue);
    if (qd == queue_to_device_.end()) {
        resp.ok = false;
        resp.reason = "unknown queue handle";
        return resp;
    }
    const std::uint64_t device = qd->second;
    // command_buffers may be empty (a fence/semaphore-only submit is valid). Each must be
    // a recorded buffer on the queue's device.
    for (const std::uint64_t b : req.command_buffers) {
        const auto cb = command_buffers_.find(b);
        if (cb == command_buffers_.end() || cb->second.device != device) {
            resp.ok = false;
            resp.reason = "command buffer is not on the submit queue's device";
            return resp;
        }
        if (!cb->second.recorded) {
            resp.ok = false;
            resp.reason = "command buffer has not been recorded";
            return resp;
        }
    }
    // Wait semaphores: each a semaphore on the queue's device, with a valid sync1 stage mask
    // (non-zero AND fitting 32-bit VkFlags -- an oversized mask is rejected, not truncated).
    for (const SubmitWait& w : req.waits) {
        const auto leaf = leaves_.find(w.semaphore);
        if (leaf == leaves_.end() || leaf->second.kind != LeafKind::Semaphore ||
            leaf->second.device != device) {
            resp.ok = false;
            resp.reason = "wait semaphore is not a semaphore on the submit queue's device";
            return resp;
        }
        if (!valid_stage_mask(w.stage)) {
            resp.ok = false;
            resp.reason = "wait semaphore has an invalid (zero or out-of-range) stage mask";
            return resp;
        }
    }
    // Signal semaphores + optional fence: each on the queue's device, right kind.
    for (const std::uint64_t s : req.signal_semaphores) {
        const auto leaf = leaves_.find(s);
        if (leaf == leaves_.end() || leaf->second.kind != LeafKind::Semaphore ||
            leaf->second.device != device) {
            resp.ok = false;
            resp.reason = "signal semaphore is not a semaphore on the submit queue's device";
            return resp;
        }
    }
    // GL/zink: a submit SIGNALS each timeline signal semaphore to its carried value. The
    // mock executes synchronously, so advance the counter now (a binary signal / missing value is a
    // no-op). signal_values is parallel to signal_semaphores; absent (legacy) -> all 0.
    for (std::size_t i = 0; i < req.signal_semaphores.size(); ++i) {
        const auto leaf = leaves_.find(req.signal_semaphores[i]);
        if (leaf != leaves_.end() && leaf->second.is_timeline) {
            const std::uint64_t v = (i < req.signal_values.size()) ? req.signal_values[i] : 0;
            if (v > leaf->second.timeline_value) {
                leaf->second.timeline_value = v;
            }
        }
    }
    if (req.fence != 0) {
        const auto leaf = leaves_.find(req.fence);
        if (leaf == leaves_.end() || leaf->second.kind != LeafKind::Fence ||
            leaf->second.device != device) {
            resp.ok = false;
            resp.reason = "fence is not a fence on the submit queue's device";
            return resp;
        }
        // the mock executes synchronously, so the submitted work "completes" and its
        // fence is signaled -- a subsequent vkGetFenceStatus honestly reports VK_SUCCESS.
        leaf->second.is_signaled = true;
    }
    resp.ok = true;
    resp.reason = "ok";
    resp.result = kVkSuccess;
    return resp;
}

QueueSubmitResponse MockVulkanBackend::queue_submit2(const QueueSubmit2Request& req) {
    // vkQueueSubmit2 -- per-submit validation of the VkSubmitInfo2 vector (mock == real),
    // then advance timelines + signal the fence (the mock runs synchronously). sync2 wait stages
    // may be VK_PIPELINE_STAGE_2_NONE (0), so no non-zero-stage rule (valid_stage_mask2 permits
    // any).
    QueueSubmitResponse resp;
    const auto qd = queue_to_device_.find(req.queue);
    if (qd == queue_to_device_.end()) {
        resp.ok = false;
        resp.reason = "unknown queue handle";
        return resp;
    }
    const std::uint64_t device = qd->second;
    const auto sdev = devices_.find(device);
    if (sdev == devices_.end() ||
        (sdev->second.enabled_exts.count(kSync2ExtensionName) == 0 && !sdev->second.vk13_device) ||
        !sdev->second.synchronization2_feature_enabled) {
        resp.ok = false;
        resp.reason = "vkQueueSubmit2 requires the synchronization2 feature";
        return resp;
    }
    for (const SubmitInfo2& si : req.submits) {
        for (const std::uint64_t b : si.command_buffers) {
            const auto cb = command_buffers_.find(b);
            if (cb == command_buffers_.end() || cb->second.device != device) {
                resp.ok = false;
                resp.reason = "command buffer is not on the submit queue's device";
                return resp;
            }
            if (!cb->second.recorded) {
                resp.ok = false;
                resp.reason = "command buffer has not been recorded";
                return resp;
            }
        }
        for (const SemaphoreSubmit2& w : si.waits) {
            const auto leaf = leaves_.find(w.semaphore);
            if (leaf == leaves_.end() || leaf->second.kind != LeafKind::Semaphore ||
                leaf->second.device != device) {
                resp.ok = false;
                resp.reason = "wait semaphore is not a semaphore on the submit queue's device";
                return resp;
            }
        }
        for (const SemaphoreSubmit2& s : si.signals) {
            const auto leaf = leaves_.find(s.semaphore);
            if (leaf == leaves_.end() || leaf->second.kind != LeafKind::Semaphore ||
                leaf->second.device != device) {
                resp.ok = false;
                resp.reason = "signal semaphore is not a semaphore on the submit queue's device";
                return resp;
            }
        }
        for (const SemaphoreSubmit2& s : si.signals) {
            const auto leaf = leaves_.find(s.semaphore);
            if (leaf != leaves_.end() && leaf->second.is_timeline &&
                s.value > leaf->second.timeline_value) {
                leaf->second.timeline_value = s.value;
            }
        }
    }
    if (req.fence != 0) {
        const auto leaf = leaves_.find(req.fence);
        if (leaf == leaves_.end() || leaf->second.kind != LeafKind::Fence ||
            leaf->second.device != device) {
            resp.ok = false;
            resp.reason = "fence is not a fence on the submit queue's device";
            return resp;
        }
        leaf->second.is_signaled = true;
    }
    resp.ok = true;
    resp.reason = "ok";
    resp.result = kVkSuccess;
    return resp;
}

bool MockVulkanBackend::validate_fence_array(const std::vector<std::uint64_t>& fences,
                                             std::string& err) const {
    if (fences.empty()) {
        err = "no fences specified";
        return false;
    }
    std::uint64_t device = 0;
    for (const std::uint64_t f : fences) {
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
    }
    return true;
}

StatusResponse MockVulkanBackend::reset_fences(const ResetFencesRequest& req) {
    StatusResponse resp;
    resp.ok = validate_fence_array(req.fences, resp.reason);
    if (resp.ok) {
        resp.reason = "ok";
        // reset returns each fence to unsignaled, so a following vkGetFenceStatus reports
        // VK_NOT_READY until the next submit re-signals it. (Validated above -- all are Fence
        // leaves.)
        for (const std::uint64_t f : req.fences) {
            leaves_.at(f).is_signaled = false;
        }
    }
    return resp;
}

WaitForFencesResponse MockVulkanBackend::wait_for_fences(const WaitForFencesRequest& req) {
    WaitForFencesResponse resp;
    if (!validate_fence_array(req.fences, resp.reason)) {
        resp.ok = false;
        return resp;
    }
    // The mock has no real fence state; the wait "succeeds" immediately. (kVkTimeout is
    // the real backend's honest result when a real wait times out.)
    resp.ok = true;
    resp.reason = "ok";
    resp.result = kVkSuccess;
    return resp;
}

// REAL idle waits -- mock == real validation (a live queue/device handle);
// the mock executes synchronously, so an idle wait is immediately satisfied.
WaitIdleResponse MockVulkanBackend::queue_wait_idle(const HandleRequest& req) {
    WaitIdleResponse resp;
    if (queue_to_device_.find(req.handle) == queue_to_device_.end()) {
        resp.ok = false;
        resp.reason = "unknown queue handle";
        return resp;
    }
    resp.ok = true;
    resp.reason = "ok";
    resp.result = kVkSuccess;
    return resp;
}

WaitIdleResponse MockVulkanBackend::device_wait_idle(const HandleRequest& req) {
    WaitIdleResponse resp;
    if (devices_.find(req.handle) == devices_.end()) {
        resp.ok = false;
        resp.reason = "unknown device handle";
        return resp;
    }
    resp.ok = true;
    resp.reason = "ok";
    resp.result = kVkSuccess;
    return resp;
}

WaitSemaphoresResponse MockVulkanBackend::wait_semaphores(const WaitSemaphoresRequest& req) {
    WaitSemaphoresResponse resp;
    if (req.semaphores.size() != req.values.size() || req.semaphores.empty()) {
        resp.ok = false;
        resp.reason = "wait_semaphores: semaphores/values size mismatch or empty";
        return resp;
    }
    // The mock advances timelines synchronously (signal_semaphore + queue_submit signal values), so
    // a wait is satisfiable iff the current counter already meets the requested value. wait-all
    // (the default) needs every pair satisfied; wait-any needs one. An unsatisfied wait is an
    // honest VK_TIMEOUT (not an RPC fault), mirroring the real backend.
    bool all = true;
    bool any = false;
    for (std::size_t i = 0; i < req.semaphores.size(); ++i) {
        const auto leaf = leaves_.find(req.semaphores[i]);
        if (leaf == leaves_.end() || leaf->second.kind != LeafKind::Semaphore ||
            !leaf->second.is_timeline || leaf->second.device != req.device) {
            resp.ok = false;
            resp.reason = "wait_semaphores references a timeline semaphore not on the device";
            return resp;
        }
        const bool met = leaf->second.timeline_value >= req.values[i];
        all = all && met;
        any = any || met;
    }
    resp.ok = true;
    resp.reason = "ok";
    resp.result = (req.wait_any ? any : all) ? kVkSuccess : kVkTimeout;
    return resp;
}

StatusResponse MockVulkanBackend::signal_semaphore(const SignalSemaphoreRequest& req) {
    StatusResponse resp;
    const auto leaf = leaves_.find(req.semaphore);
    if (leaf == leaves_.end() || leaf->second.kind != LeafKind::Semaphore ||
        !leaf->second.is_timeline || leaf->second.device != req.device) {
        resp.ok = false;
        resp.reason = "signal_semaphore references a timeline semaphore not on the device";
        return resp;
    }
    // A timeline value may only ADVANCE (VUID-VkSemaphoreSignalInfo-value); reject a regression.
    if (req.value < leaf->second.timeline_value) {
        resp.ok = false;
        resp.reason = "signal_semaphore value is below the current timeline value";
        return resp;
    }
    leaf->second.timeline_value = req.value;
    resp.ok = true;
    resp.reason = "ok";
    return resp;
}

GetSemaphoreCounterValueResponse
MockVulkanBackend::get_semaphore_counter_value(const GetSemaphoreCounterValueRequest& req) {
    GetSemaphoreCounterValueResponse resp;
    const auto leaf = leaves_.find(req.semaphore);
    if (leaf == leaves_.end() || leaf->second.kind != LeafKind::Semaphore ||
        !leaf->second.is_timeline || leaf->second.device != req.device) {
        resp.ok = false;
        resp.reason =
            "get_semaphore_counter_value references a timeline semaphore not on the device";
        return resp;
    }
    resp.ok = true;
    resp.reason = "ok";
    resp.value = leaf->second.timeline_value;
    return resp;
}

namespace {
// Validates a {physical_device, surface} query pair against the mock tables: the surface must
// exist and the physical-device handle must be the one its instance enumerated. Returns the
// surface's instance handle (0 + sets `err` on rejection).
template <typename Surfaces, typename Instances>
bool surface_query_ok(const Surfaces& surfaces, const Instances& instances,
                      std::uint64_t physical_device, std::uint64_t surface, std::string& err) {
    const auto surf = surfaces.find(surface);
    if (surf == surfaces.end()) {
        err = "unknown surface handle";
        return false;
    }
    const auto inst = instances.find(surf->second.instance);
    if (inst == instances.end() || inst->second.physical_device == 0 ||
        inst->second.physical_device != physical_device) {
        err = "physical device was not enumerated from this surface's instance";
        return false;
    }
    return true;
}
} // namespace

GetSurfaceCapabilitiesResponse
MockVulkanBackend::get_surface_capabilities(const GetSurfaceCapabilitiesRequest& req) {
    GetSurfaceCapabilitiesResponse resp;
    if (!surface_query_ok(surfaces_, instances_, req.physical_device, req.surface, resp.reason)) {
        resp.ok = false;
        return resp;
    }
    // Canned caps (the mock has no host surface). currentExtent is the dynamic-extent
    // sentinel; supported usage advertises COLOR_ATTACHMENT | TRANSFER_DST so a clear canary
    // can request TRANSFER_DST honestly.
    resp.min_image_count = 2;
    resp.max_image_count = 8;
    resp.current_extent_width = kDynamicExtentSentinel;
    resp.current_extent_height = kDynamicExtentSentinel;
    resp.min_image_extent_width = 1;
    resp.min_image_extent_height = 1;
    resp.max_image_extent_width = 16384;
    resp.max_image_extent_height = 16384;
    // (mock == real): if the sidecar has authored a live extent for this surface's
    // toplevel, pin currentExtent (min == max == current == authored) so the app converges there.
    {
        std::lock_guard<std::mutex> lk(backend_mutex_);
        const auto s = surfaces_.find(req.surface);
        const sidecar::WindowRegistry::AuthoritativeExtent authority =
            (s != surfaces_.end()) ? registry_.authoritative_extent_for_xid(s->second.xid)
                                   : sidecar::WindowRegistry::AuthoritativeExtent{};
        if (authority.active) {
            resp.current_extent_width = authority.width;
            resp.current_extent_height = authority.height;
            resp.min_image_extent_width = authority.width;
            resp.min_image_extent_height = authority.height;
            resp.max_image_extent_width = authority.width;
            resp.max_image_extent_height = authority.height;
        }
    }
    resp.max_image_array_layers = 1;
    resp.supported_transforms = 0x1;      // VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR
    resp.current_transform = 0x1;         // VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR
    resp.supported_composite_alpha = 0x1; // VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
    resp.supported_usage_flags =
        static_cast<std::uint64_t>(kImageUsageColorAttachment | kImageUsageTransferDst);
    resp.ok = true;
    resp.reason = "ok";
    return resp;
}

GetSurfaceFormatsResponse
MockVulkanBackend::get_surface_formats(const GetSurfaceFormatsRequest& req) {
    GetSurfaceFormatsResponse resp;
    if (!surface_query_ok(surfaces_, instances_, req.physical_device, req.surface, resp.reason)) {
        resp.ok = false;
        return resp;
    }
    resp.formats.push_back({44, 0}); // VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR
    resp.ok = true;
    resp.reason = "ok";
    return resp;
}

GetSurfacePresentModesResponse
MockVulkanBackend::get_surface_present_modes(const GetSurfacePresentModesRequest& req) {
    GetSurfacePresentModesResponse resp;
    if (!surface_query_ok(surfaces_, instances_, req.physical_device, req.surface, resp.reason)) {
        resp.ok = false;
        return resp;
    }
    resp.present_modes.push_back(2); // VK_PRESENT_MODE_FIFO_KHR (guaranteed by spec)
    resp.ok = true;
    resp.reason = "ok";
    return resp;
}

GetSurfaceSupportResponse
MockVulkanBackend::get_surface_support(const GetSurfaceSupportRequest& req) {
    GetSurfaceSupportResponse resp;
    if (!surface_query_ok(surfaces_, instances_, req.physical_device, req.surface, resp.reason)) {
        resp.ok = false;
        return resp;
    }
    // The mock exposes exactly one queue family (index 0, the graphics+present family
    // create_device reports); any other family index is not a family the app can use.
    if (req.queue_family_index != 0) {
        resp.ok = false;
        resp.reason = "queue family index out of range (the worker exposes one family)";
        return resp;
    }
    resp.supported = true;
    resp.ok = true;
    resp.reason = "ok";
    return resp;
}

void MockVulkanBackend::debug_mark_surface_dirty(std::uint64_t surface) {
    const auto it = surfaces_.find(surface);
    if (it != surfaces_.end()) {
        it->second.geometry_dirty = true;
    }
}

// --- Draw surface ---------------------------------------
// The mock has no GPU; it models the handle lifecycle + the bounded create-info allowlist
// (strict-but-total: an out-of-subset field is rejected, never defaulted) + destroy ordering, so
// the full create -> record(draw) -> submit -> present chain round-trips headless on both
// platforms.

CreateImageViewResponse MockVulkanBackend::create_image_view(const CreateImageViewRequest& req) {
    CreateImageViewResponse resp;
    const auto img = images_.find(req.image);
    if (img == images_.end()) {
        resp.ok = false;
        resp.reason = "unknown image handle (must be a live swapchain or app image)";
        return resp;
    }
    // GL/zink: FAITHFUL image view -- any viewType / format / swizzle / aspect /
    // mip+layer range (the vkcube 2D/identity/single-subresource subset is lifted; the host driver
    // gates the real backend, and the mock tracks only device + source image for lifetime
    // bookkeeping). An app image must be bound to memory before a view is created over it (Vulkan
    // requires it).
    if (img->second.app_created && img->second.bound_memory == 0) {
        resp.ok = false;
        resp.reason = "image view over an app image that is not bound to memory";
        return resp;
    }
    const std::uint64_t device = img->second.device;
    const std::uint64_t swapchain = img->second.swapchain;
    const std::uint64_t h = next_handle_++;
    ImageView iv;
    iv.device = device;
    iv.image = req.image;
    iv.swapchain = swapchain;
    iv.format = req.format; // (dynamic rendering): the DR attachment-format compat key
    image_views_.emplace(h, iv);
    devices_[device].image_views.insert(h);
    const auto sc = swapchains_.find(swapchain);
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

void MockVulkanBackend::invalidate_cbs_referencing(std::uint64_t handle) {
    for (auto& kv : command_buffers_) {
        if (kv.second.referenced_draw_objects.count(handle) != 0) {
            kv.second.recorded = false;
            kv.second.referenced_surfaces.clear();
            kv.second.referenced_swapchains.clear();
            kv.second.referenced_draw_objects.clear();
        }
    }
}

void MockVulkanBackend::dangle_sets_referencing(std::uint64_t resource) {
    for (auto& [set_handle, set] : descriptor_sets_) {
        const auto sl = descriptor_set_layouts_.find(set.layout);
        bool affected = false;
        for (auto& [binding, slots] : set.slots) {
            // descriptorIndexing: a PARTIALLY_BOUND or UPDATE_AFTER_BIND
            // binding's dangle must NOT invalidate recorded CBs -- destroying a
            // not-dynamically-used resource (PARTIALLY_BOUND) or destroy-old/update-to-new before
            // submit (UAB) are spec-legal; the host judges the LIVE set at submit. The slot is
            // still cleared (bookkeeping: the referent is gone). Classic bindings keep the
            // fail-closed invalidate.
            long long binding_flags = 0;
            if (sl != descriptor_set_layouts_.end()) {
                for (const DescriptorSetLayoutBinding& b : sl->second.bindings) {
                    if (b.binding == binding) {
                        binding_flags = b.binding_flags;
                        break;
                    }
                }
            }
            const bool host_owned = (binding_flags & (kDescriptorBindingPartiallyBound |
                                                      kDescriptorBindingUpdateAfterBind)) != 0;
            for (DescriptorSlot& slot : slots) {
                // A repoint away from this resource before the destroy means no slot references it,
                // so nothing is invalidated -- correct, since the recorded CB binds the live set
                // handle, not a snapshot.
                if (slot.initialized &&
                    (slot.buffer == resource || slot.sampler == resource ||
                     slot.image_view == resource || slot.buffer_view == resource)) {
                    slot.initialized = false; // dangling: a draw with this set now fails validation
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

StatusResponse MockVulkanBackend::destroy_image_view(const HandleRequest& req) {
    StatusResponse resp;
    const auto it = image_views_.find(req.handle);
    if (it == image_views_.end()) {
        resp.ok = false;
        resp.reason = "unknown image view handle";
        return resp;
    }
    invalidate_cbs_referencing(req.handle);
    // CB -> set -> image-view destroy consult: a combined-image-sampler slot
    // currently referencing this view is marked dangling + its CBs invalidated (mirrors the
    // buffer consult). A texture image cannot itself be destroyed while a view lives, so the view
    // is the descriptor's effective referent.
    dangle_sets_referencing(req.handle);
    const auto dev = devices_.find(it->second.device);
    if (dev != devices_.end()) {
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

CreateBufferViewResponse MockVulkanBackend::create_buffer_view(const CreateBufferViewRequest& req) {
    // GL/zink: faithful texel buffer view. No vkcube-shaped subset -- the source buffer
    // must exist (and be bound), the view inherits its device. zink mints these for texel-buffer
    // descriptors and a null-descriptor dummy at screen init.
    CreateBufferViewResponse resp;
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
    if (req.range == kVkWholeSize) {
        if (req.offset >= buf->second.size) {
            resp.ok = false;
            resp.reason = "buffer view VK_WHOLE_SIZE offset is outside the logical buffer";
            return resp;
        }
    } else if (req.range == 0 || req.offset > buf->second.size ||
               req.range > buf->second.size - req.offset) {
        resp.ok = false;
        resp.reason = "buffer view range is outside the logical buffer";
        return resp;
    }
    const std::uint64_t h = next_handle_++;
    BufferView bv;
    bv.device = buf->second.device;
    bv.buffer = req.buffer;
    buffer_views_.emplace(h, bv);
    resp.ok = true;
    resp.reason = "ok";
    resp.buffer_view = h;
    return resp;
}

StatusResponse MockVulkanBackend::destroy_buffer_view(const HandleRequest& req) {
    StatusResponse resp;
    const auto it = buffer_views_.find(req.handle);
    if (it == buffer_views_.end()) {
        resp.ok = false;
        resp.reason = "unknown buffer view handle";
        return resp;
    }
    invalidate_cbs_referencing(req.handle);
    dangle_sets_referencing(req.handle); // texel-buffer descriptor slots referencing this view
    buffer_views_.erase(it);
    resp.ok = true;
    resp.reason = "ok";
    return resp;
}

CreateShaderModuleResponse
MockVulkanBackend::create_shader_module(const CreateShaderModuleRequest& req) {
    CreateShaderModuleResponse resp;
    const auto dev = devices_.find(req.device);
    if (dev == devices_.end()) {
        resp.ok = false;
        resp.reason = "unknown device handle";
        return resp;
    }
    // SPIR-V is a stream of u32 words within the cap; the payload must match code_size
    // (the wire decoder already enforced ==, this is the backend's semantic guard).
    if (req.code_size == 0 || (req.code_size % 4) != 0 || req.code_size > kMaxShaderCodeBytes ||
        req.code.size() != req.code_size) {
        resp.ok = false;
        resp.reason = "malformed SPIR-V (size must be > 0, a multiple of 4, within the cap, and "
                      "match the payload length)";
        return resp;
    }
    const std::uint64_t h = next_handle_++;
    shader_modules_.emplace(h, ShaderModule{req.device});
    dev->second.shader_modules.insert(h);
    resp.ok = true;
    resp.reason = "ok";
    resp.shader_module = h;
    return resp;
}

StatusResponse MockVulkanBackend::destroy_shader_module(const HandleRequest& req) {
    StatusResponse resp;
    const auto it = shader_modules_.find(req.handle);
    if (it == shader_modules_.end()) {
        resp.ok = false;
        resp.reason = "unknown shader module handle";
        return resp;
    }
    const auto dev = devices_.find(it->second.device);
    if (dev != devices_.end()) {
        dev->second.shader_modules.erase(req.handle);
    }
    shader_modules_.erase(it);
    resp.ok = true;
    resp.reason = "ok";
    return resp;
}

CreateRenderPassResponse MockVulkanBackend::create_render_pass(const CreateRenderPassRequest& req) {
    CreateRenderPassResponse resp;
    const auto dev = devices_.find(req.device);
    if (dev == devices_.end()) {
        resp.ok = false;
        resp.reason = "unknown device handle";
        return resp;
    }
    // Required-feature audit: a viewMask (multiview) render pass is admitted ONLY on a
    // device that enabled the multiview feature -- the fail-closed gate (mock == real). A
    // conformant ICD only ever sends view_mask != 0 for such a device (the *2 path carries it), so
    // this is the hostile/stale-RPC safety net. A negative mask is a malformed frame.
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
    // MRT: a non-empty color_refs vector is the widened faithful path -- mock == real validation
    // (count vs the advertised limit, per-ref range/layout with UNUSED holes, at least one used
    // color or depth), any attachment ops. An empty vector keeps the EXACT legacy vkcube-shaped
    // subset below (old-ICD compatibility + the existing unit tests).
    if (!req.color_refs.empty()) {
        if (req.color_refs.size() > 8) { // mirrors device_caps().max_color_attachments
            resp.ok = false;
            resp.reason = "render pass color attachment count exceeds the device limit";
            return resp;
        }
        bool any_used = false;
        for (const ColorRefDesc& cr : req.color_refs) {
            if (!color_ref_used(cr)) {
                continue;
            }
            if (cr.attachment < 0 ||
                static_cast<std::size_t>(cr.attachment) >= req.attachments.size() ||
                cr.layout < 0) {
                resp.ok = false;
                resp.reason = "render pass color ref out of range or with an invalid layout";
                return resp;
            }
            any_used = true;
        }
        const bool vec_has_depth = req.depth_attachment >= 0;
        if (req.attachments.empty() || (!any_used && !vec_has_depth) ||
            (vec_has_depth &&
             static_cast<std::size_t>(req.depth_attachment) >= req.attachments.size())) {
            resp.ok = false;
            resp.reason = "render pass needs >=1 attachment and an in-range colour/depth ref";
            return resp;
        }
        const std::uint64_t h = next_handle_++;
        RenderPass rp;
        rp.device = req.device;
        for (const ColorRefDesc& cr : req.color_refs) {
            if (color_ref_used(cr)) { // the compat key: the FIRST USED ref's format
                rp.color_format = req.attachments[static_cast<std::size_t>(cr.attachment)].format;
                break;
            }
        }
        rp.depth_format =
            vec_has_depth ? req.attachments[static_cast<std::size_t>(req.depth_attachment)].format
                          : 0;
        rp.color_refs = req.color_refs;
        for (const AttachmentDesc& ad : req.attachments) {
            rp.attachment_formats.push_back(ad.format);
        }
        rp.view_mask = req.view_mask; // (0 = non-multiview)
        render_passes_.emplace(h, rp);
        dev->second.render_passes.insert(h);
        resp.ok = true;
        resp.reason = "ok";
        resp.render_pass = h;
        return resp;
    }
    // One color attachment (index 0) + optionally one depth attachment (index 1).
    const bool has_depth = req.depth_attachment >= 0;
    const std::size_t want_atts = has_depth ? 2u : 1u;
    if (req.attachments.size() != want_atts) {
        resp.ok = false;
        resp.reason = "render pass attachment count must be 1 (color) or 2 (color + depth)";
        return resp;
    }
    const AttachmentDesc& a = req.attachments[0];
    if (a.format < 0 || a.samples != vk3b::kSampleCount1 ||
        a.load_op != vk3b::kAttachmentLoadOpClear || a.store_op != vk3b::kAttachmentStoreOpStore ||
        a.stencil_load_op != vk3b::kAttachmentLoadOpDontCare ||
        a.stencil_store_op != vk3b::kAttachmentStoreOpDontCare ||
        a.initial_layout != vk3b::kImageLayoutUndefined ||
        a.final_layout != vk3b::kImageLayoutPresentSrcKhr) {
        resp.ok = false;
        resp.reason = "render pass color attachment outside the subset (samples 1, loadOp CLEAR, "
                      "storeOp STORE, stencil DONT_CARE, UNDEFINED -> PRESENT_SRC_KHR)";
        return resp;
    }
    if (req.color_attachment != 0 || req.color_layout != vk3b::kImageLayoutColorAttachmentOptimal) {
        resp.ok = false;
        resp.reason = "render pass subpass must reference attachment 0 as COLOR_ATTACHMENT_OPTIMAL";
        return resp;
    }
    // Depth attachment: index 1, D16_UNORM, loadOp CLEAR, storeOp DONT_CARE (vkcube
    // does not store depth), stencil DONT_CARE, UNDEFINED -> DEPTH_STENCIL_ATTACHMENT_OPTIMAL (the
    // layout transition is render-pass-owned). depthStencil ref layout == DEPTH_STENCIL_*_OPTIMAL.
    int depth_format = 0;
    if (has_depth) {
        if (req.depth_attachment != 1 ||
            req.depth_layout != vk3c3::kImageLayoutDepthStencilAttachmentOptimal) {
            resp.ok = false;
            resp.reason = "render pass depth ref must be attachment 1 as "
                          "DEPTH_STENCIL_ATTACHMENT_OPTIMAL";
            return resp;
        }
        const AttachmentDesc& d = req.attachments[1];
        if (!vk3c3::is_depth_format(d.format) || d.samples != vk3b::kSampleCount1 ||
            d.load_op != vk3b::kAttachmentLoadOpClear ||
            d.store_op != vk3b::kAttachmentStoreOpDontCare ||
            d.stencil_load_op != vk3b::kAttachmentLoadOpDontCare ||
            d.stencil_store_op != vk3b::kAttachmentStoreOpDontCare ||
            d.initial_layout != vk3b::kImageLayoutUndefined ||
            d.final_layout != vk3c3::kImageLayoutDepthStencilAttachmentOptimal) {
            resp.ok = false;
            resp.reason = "render pass depth attachment outside the subset (D16_UNORM, samples 1, "
                          "loadOp CLEAR, storeOp DONT_CARE, stencil DONT_CARE, UNDEFINED -> "
                          "DEPTH_STENCIL_ATTACHMENT_OPTIMAL)";
            return resp;
        }
        depth_format = d.format;
    }
    if (req.dependencies.size() > 2) {
        resp.ok = false;
        resp.reason = "render pass allows at most two dependencies";
        return resp;
    }
    for (const SubpassDependencyDesc& d : req.dependencies) {
        const bool src_ok = d.src_subpass == 0 || d.src_subpass == vk3b::kSubpassExternal;
        const bool dst_ok = d.dst_subpass == 0 || d.dst_subpass == vk3b::kSubpassExternal;
        if (!src_ok || !dst_ok || !valid_stage_mask(d.src_stage) ||
            !valid_stage_mask(d.dst_stage) || !valid_access_mask(d.src_access) ||
            !valid_access_mask(d.dst_access) || d.dependency_flags < 0 ||
            d.dependency_flags > vk3b::kDependencyByRegionBit) {
            resp.ok = false;
            resp.reason = "render pass dependency outside the supported subset (subpass in "
                          "{EXTERNAL, 0}, valid masks, flags in {0, BY_REGION})";
            return resp;
        }
    }
    const std::uint64_t h = next_handle_++;
    RenderPass rp;
    rp.device = req.device;
    rp.color_format = a.format;
    rp.depth_format = depth_format; // 0 = no depth attachment
    rp.view_mask = req.view_mask;   // (0 = non-multiview)
    render_passes_.emplace(h, rp);
    dev->second.render_passes.insert(h);
    resp.ok = true;
    resp.reason = "ok";
    resp.render_pass = h;
    return resp;
}

StatusResponse MockVulkanBackend::destroy_render_pass(const HandleRequest& req) {
    StatusResponse resp;
    const auto it = render_passes_.find(req.handle);
    if (it == render_passes_.end()) {
        resp.ok = false;
        resp.reason = "unknown render pass handle";
        return resp;
    }
    invalidate_cbs_referencing(req.handle);
    const auto dev = devices_.find(it->second.device);
    if (dev != devices_.end()) {
        dev->second.render_passes.erase(req.handle);
    }
    render_passes_.erase(it);
    resp.ok = true;
    resp.reason = "ok";
    return resp;
}

CreateFramebufferResponse
MockVulkanBackend::create_framebuffer(const CreateFramebufferRequest& req) {
    CreateFramebufferResponse resp;
    const auto dev = devices_.find(req.device);
    if (dev == devices_.end()) {
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
    // Required-feature audit: a multiview render pass (view_mask != 0) needs every
    // attachment image to cover at least multiview_required_layers(view_mask) array layers -- the
    // highest set view bit + 1 (VUID-02531; the host vkCreateFramebuffer is the authoritative
    // enforcer, this is the shared mock-checkable pre-check keyed on the backing image's array
    // layers). The framebuffer's own `layers` stays 1 for multiview (the layer count comes from the
    // views), so the existing layers == 1 gate below is unchanged.
    const int fb_required_layers = multiview_required_layers(rp->second.view_mask);
    if (req.imageless) {
        if (req.width <= 0 || req.height <= 0 || req.layers <= 0 || req.attachment_count <= 0) {
            resp.ok = false;
            resp.reason = "imageless framebuffer has invalid dimensions or attachment count";
            return resp;
        }
        if (!req.attachment_infos.empty() &&
            req.attachment_infos.size() != static_cast<std::size_t>(req.attachment_count)) {
            resp.ok = false;
            resp.reason =
                "imageless framebuffer attachment count disagrees with attachment metadata";
            return resp;
        }
        const std::uint64_t h = next_handle_++;
        Framebuffer fb;
        fb.device = req.device;
        fb.render_pass = req.render_pass;
        fb.compat_color_format = rp->second.color_format;
        fb.compat_depth_format = rp->second.depth_format;
        fb.width = req.width;
        fb.height = req.height;
        fb.imageless = true;
        fb.attachment_count = req.attachment_count;
        framebuffers_.emplace(h, fb);
        dev->second.framebuffers.insert(h);
        resp.ok = true;
        resp.reason = "ok";
        resp.framebuffer = h;
        return resp;
    }
    // MRT: a positional view vector is validated slot by slot against the render pass's attachment
    // metadata -- mock == real (count, liveness, per-position format). The legacy scalar shape
    // keeps the exact old color(+depth) path below.
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
            if (vimg->second.array_layers < fb_required_layers) { // multiview layers
                resp.ok = false;
                resp.reason = "framebuffer attachment has too few array layers for the render pass "
                              "viewMask (multiview)";
                return resp;
            }
        }
        const std::uint64_t h = next_handle_++;
        Framebuffer fb;
        fb.device = req.device;
        fb.render_pass = req.render_pass;
        fb.compat_color_format = rp->second.color_format; // compat snapshot (pass may die)
        fb.compat_depth_format = rp->second.depth_format;
        fb.image_view = req.attachment_views[0]; // legacy field: position 0
        fb.attachment_views = req.attachment_views;
        fb.width = req.width;
        fb.height = req.height;
        framebuffers_.emplace(h, fb);
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
    // The view's format must match the render pass's color attachment format (Vulkan requires it).
    const auto img = images_.find(iv->second.image);
    if (img == images_.end() || img->second.format != rp->second.color_format) {
        resp.ok = false;
        resp.reason = "framebuffer image view format does not match the render pass attachment";
        return resp;
    }
    if (img->second.array_layers < fb_required_layers) { // multiview layers (color)
        resp.ok = false;
        resp.reason = "framebuffer color attachment has too few array layers for the render pass "
                      "viewMask (multiview)";
        return resp;
    }
    // Positive extent, one layer, and the extent must equal the swapchain extent the view is over
    // (the canary sizes the framebuffer to the swapchain). The swapchain is live (the
    // view is) so its extent is resolvable.
    const auto sc = swapchains_.find(img->second.swapchain);
    if (req.width <= 0 || req.height <= 0 || req.layers != 1) {
        resp.ok = false;
        resp.reason = "framebuffer outside the supported subset (one layer, positive extent)";
        return resp;
    }
    if (sc == swapchains_.end() || req.width != sc->second.width ||
        req.height != sc->second.height) {
        resp.ok = false;
        resp.reason = "framebuffer extent does not match the swapchain extent";
        return resp;
    }
    // Depth attachment: the framebuffer must carry a depth view iff the render pass
    // has a depth attachment; the depth view's image format must match the render pass's depth
    // format and its extent must equal the framebuffer extent.
    if (rp->second.depth_format != 0) {
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
            dimg->second.aspect != kImageAspectDepth) {
            resp.ok = false;
            resp.reason = "framebuffer depth view does not match the render pass depth attachment";
            return resp;
        }
        if (dimg->second.array_layers < fb_required_layers) { // multiview layers (depth)
            resp.ok = false;
            resp.reason = "framebuffer depth attachment has too few array layers for the render "
                          "pass viewMask (multiview)";
            return resp;
        }
        if (dimg->second.width != req.width || dimg->second.height != req.height) {
            resp.ok = false;
            resp.reason = "framebuffer depth view extent does not match the framebuffer extent";
            return resp;
        }
    } else if (req.depth_image_view != 0) {
        resp.ok = false;
        resp.reason =
            "framebuffer carries a depth view but the render pass has no depth attachment";
        return resp;
    }
    const std::uint64_t h = next_handle_++;
    Framebuffer fb;
    fb.device = req.device;
    fb.render_pass = req.render_pass;
    fb.compat_color_format = rp->second.color_format; // compat snapshot (pass may die)
    fb.compat_depth_format = rp->second.depth_format;
    fb.image_view = req.image_view;
    fb.depth_image_view = req.depth_image_view;
    fb.width = req.width;
    fb.height = req.height;
    framebuffers_.emplace(h, fb);
    dev->second.framebuffers.insert(h);
    resp.ok = true;
    resp.reason = "ok";
    resp.framebuffer = h;
    return resp;
}

StatusResponse MockVulkanBackend::destroy_framebuffer(const HandleRequest& req) {
    StatusResponse resp;
    const auto it = framebuffers_.find(req.handle);
    if (it == framebuffers_.end()) {
        resp.ok = false;
        resp.reason = "unknown framebuffer handle";
        return resp;
    }
    invalidate_cbs_referencing(req.handle);
    const auto dev = devices_.find(it->second.device);
    if (dev != devices_.end()) {
        dev->second.framebuffers.erase(req.handle);
    }
    framebuffers_.erase(it);
    resp.ok = true;
    resp.reason = "ok";
    return resp;
}

CreatePipelineLayoutResponse
MockVulkanBackend::create_pipeline_layout(const CreatePipelineLayoutRequest& req) {
    CreatePipelineLayoutResponse resp;
    const auto dev = devices_.find(req.device);
    if (dev == devices_.end()) {
        resp.ok = false;
        resp.reason = "unknown device handle";
        return resp;
    }
    // GL/zink: push-constant ranges are accepted (count must agree with the carried list,
    // mock == real); each range is bounded the same way the ICD subset predicate validates it.
    if (req.push_constant_range_count != static_cast<int>(req.push_constant_ranges.size())) {
        resp.ok = false;
        resp.reason = "pipeline layout push_constant_range_count disagrees with the carried list";
        return resp;
    }
    if (req.push_constant_ranges.size() > static_cast<std::size_t>(kMaxPushConstantRanges)) {
        resp.ok = false;
        resp.reason = "pipeline layout has too many push-constant ranges";
        return resp;
    }
    for (const PushConstantRange& pc : req.push_constant_ranges) {
        if (pc.stage_flags == 0 || pc.size == 0 || (pc.offset % 4) != 0 || (pc.size % 4) != 0 ||
            pc.offset > kMaxPushConstantBytes || pc.size > kMaxPushConstantBytes ||
            pc.offset + pc.size > kMaxPushConstantBytes) {
            resp.ok = false;
            resp.reason = "push-constant range out of subset (stage/align/size bound)";
            return resp;
        }
    }
    // The carried count must agree with the handle list (keys bind/draw on it).
    if (req.set_layout_count != static_cast<int>(req.set_layouts.size())) {
        resp.ok = false;
        resp.reason = "pipeline layout set_layout_count disagrees with the carried set-layout list";
        return resp;
    }
    if (req.set_layouts.size() > static_cast<std::size_t>(kMaxPipelineLayoutSetLayouts)) {
        resp.ok = false;
        resp.reason = "pipeline layout has too many set layouts";
        return resp;
    }
    // Each referenced set layout must be a known layout on the same device.
    for (const std::uint64_t sl : req.set_layouts) {
        const auto it = descriptor_set_layouts_.find(sl);
        if (it == descriptor_set_layouts_.end() || it->second.device != req.device) {
            resp.ok = false;
            resp.reason = "pipeline layout references a set layout not on the device";
            return resp;
        }
    }
    const std::uint64_t h = next_handle_++;
    PipelineLayout pl;
    pl.device = req.device;
    pl.set_layouts = req.set_layouts;
    pipeline_layouts_.emplace(h, std::move(pl));
    dev->second.pipeline_layouts.insert(h);
    resp.ok = true;
    resp.reason = "ok";
    resp.pipeline_layout = h;
    return resp;
}

StatusResponse MockVulkanBackend::destroy_pipeline_layout(const HandleRequest& req) {
    StatusResponse resp;
    const auto it = pipeline_layouts_.find(req.handle);
    if (it == pipeline_layouts_.end()) {
        resp.ok = false;
        resp.reason = "unknown pipeline layout handle";
        return resp;
    }
    // A recorded bind_descriptor_sets bakes the pipeline-layout handle, so destroying
    // it must invalidate any recorded CB that referenced it -- a later submit then fails rather
    // than replaying against a freed layout (matches pipeline/buffer/pool
    // destruction).
    invalidate_cbs_referencing(req.handle);
    const auto dev = devices_.find(it->second.device);
    if (dev != devices_.end()) {
        dev->second.pipeline_layouts.erase(req.handle);
    }
    pipeline_layouts_.erase(it);
    resp.ok = true;
    resp.reason = "ok";
    return resp;
}

// --- Descriptor surface -------------------------------

CreateDescriptorSetLayoutResponse
MockVulkanBackend::create_descriptor_set_layout(const CreateDescriptorSetLayoutRequest& req) {
    CreateDescriptorSetLayoutResponse resp;
    const auto dev = devices_.find(req.device);
    if (dev == devices_.end()) {
        resp.reason = "unknown device handle";
        return resp;
    }
    // GL/zink: faithful -- any type/stages, optional empty layout, per-binding flags
    // recorded; the real backend / host is the authority. Only a sane upper bound is enforced.
    if (req.bindings.size() > static_cast<std::size_t>(kMaxDescriptorSetLayoutBindings)) {
        resp.reason = "descriptor set layout binding count out of bounds";
        return resp;
    }
    // descriptorIndexing: the shared per-binding-flag admission -- every DI flag is gated on
    // the device's enabled kDIFeature* bits (mock == real by the shared helper).
    {
        const char* why = "";
        if (!descriptor_indexing_layout_ok(req.layout_flags, req.bindings,
                                           dev->second.descriptor_indexing_feature_bits, &why)) {
            resp.reason = why;
            return resp;
        }
    }
    // Vulkan 1.3 support (inlineUniformBlock): the shared IUB binding admission (mock == real) --
    // gated on the device's enabled inlineUniformBlock bit; descriptorCount is a BYTE size
    // (positive multiple of 4, bounded so the byte-per-slot model below stays sane).
    {
        const char* why = "";
        if (!inline_uniform_block_layout_ok(req.bindings, dev->second.vk13_feature_bits, &why)) {
            resp.reason = why;
            return resp;
        }
    }
    DescriptorSetLayout layout;
    layout.device = req.device;
    layout.layout_flags = req.layout_flags; // persisted (mock == real)
    for (const DescriptorSetLayoutBindingDesc& b : req.bindings) {
        if (b.binding < 0 || b.descriptor_count < 0 || b.descriptor_count > kMaxDescriptorCount) {
            resp.reason = "descriptor binding has an invalid binding number or descriptor count";
            return resp;
        }
        DescriptorSetLayoutBinding lb;
        lb.binding = b.binding;
        lb.descriptor_type = b.descriptor_type;
        lb.descriptor_count = b.descriptor_count;
        lb.stage_flags = b.stage_flags;
        lb.binding_flags = b.binding_flags; // persisted (the older mock dropped these)
        layout.bindings.push_back(lb);
    }
    const std::uint64_t h = next_handle_++;
    descriptor_set_layouts_.emplace(h, std::move(layout));
    dev->second.descriptor_set_layouts.insert(h);
    resp.ok = true;
    resp.reason = "ok";
    resp.set_layout = h;
    return resp;
}

GetDescriptorSetLayoutSupportResponse
MockVulkanBackend::get_descriptor_set_layout_support(const CreateDescriptorSetLayoutRequest& req) {
    // descriptorIndexing: the mock's honest answer is its OWN admission -- a layout the
    // relay would reject at create is reported unsupported (never "supported" then rejected).
    GetDescriptorSetLayoutSupportResponse resp;
    const auto dev = devices_.find(req.device);
    if (dev == devices_.end()) {
        resp.reason = "unknown device handle";
        return resp;
    }
    resp.ok = true;
    resp.reason = "ok";
    const char* why = "";
    if (req.bindings.size() > static_cast<std::size_t>(kMaxDescriptorSetLayoutBindings) ||
        !descriptor_indexing_layout_ok(req.layout_flags, req.bindings,
                                       dev->second.descriptor_indexing_feature_bits, &why) ||
        !inline_uniform_block_layout_ok(req.bindings, dev->second.vk13_feature_bits, &why)) {
        resp.supported = 0;
        return resp;
    }
    resp.supported = 1;
    resp.max_variable_descriptor_count = di_variable_binding(req.bindings) >= 0
                                             ? static_cast<std::uint64_t>(kMaxDescriptorCount)
                                             : 0;
    return resp;
}

StatusResponse MockVulkanBackend::destroy_descriptor_set_layout(const HandleRequest& req) {
    StatusResponse resp;
    const auto it = descriptor_set_layouts_.find(req.handle);
    if (it == descriptor_set_layouts_.end()) {
        resp.ok = false;
        resp.reason = "unknown descriptor set layout handle";
        return resp;
    }
    // Conservative fail-closed: a set layout referenced by a live pipeline layout cannot
    // be destroyed; nor while a live set was allocated from it (keeps the handle graph total for
    // the bind/draw exactness checks). vkcube's teardown order (pool -> pipeline layout -> set
    // layout) satisfies both.
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
    if (dev != devices_.end()) {
        dev->second.descriptor_set_layouts.erase(req.handle);
    }
    descriptor_set_layouts_.erase(it);
    resp.ok = true;
    resp.reason = "ok";
    return resp;
}

// Vulkan 1.3 support (maintenance4): the mock's create-info-shaped memory-requirement queries
// answer EXACTLY what its create paths model (the requested size / 16-byte alignment for
// buffers; bpp*extent / 256 for images), so "query then create" is self-consistent, mock ==
// real. Gated on the enabled maintenance4 feature, the real backend's PFN gate mirrored.
CreateBufferResponse
MockVulkanBackend::get_device_buffer_memory_requirements(const CreateBufferRequest& req) {
    CreateBufferResponse resp;
    const auto dev = devices_.find(req.device);
    if (dev == devices_.end()) {
        resp.reason = "unknown device handle";
        return resp;
    }
    if ((dev->second.vk13_feature_bits & kVk13FeatureMaintenance4) == 0) {
        resp.reason = "maintenance4 not enabled on this device";
        return resp;
    }
    if (req.size == 0 || req.usage == 0) {
        resp.reason = "buffer size and usage must be nonzero";
        return resp;
    }
    resp.ok = true;
    resp.reason = "ok";
    resp.mem_size = req.size;
    resp.mem_alignment = 16;
    resp.mem_type_bits = (1ull << mock_memory_types().size()) - 1;
    return resp;
}

CreateImageResponse
MockVulkanBackend::get_device_image_memory_requirements(const CreateImageRequest& req) {
    CreateImageResponse resp;
    const auto dev = devices_.find(req.device);
    if (dev == devices_.end()) {
        resp.reason = "unknown device handle";
        return resp;
    }
    if ((dev->second.vk13_feature_bits & kVk13FeatureMaintenance4) == 0) {
        resp.reason = "maintenance4 not enabled on this device";
        return resp;
    }
    if (req.width <= 0 || req.height <= 0 || req.usage == 0) {
        resp.reason = "image extent must be positive and usage nonzero";
        return resp;
    }
    // Shared conservative texel size (mock == the create_image estimate; collapses the old
    // depth-only 124-130 duplicate that under-sized D32/D24S8 and disagreed with is_depth_format).
    const std::uint64_t bpp = format_mock_texel_bytes(req.format);
    resp.ok = true;
    resp.reason = "ok";
    resp.mem_size =
        static_cast<std::uint64_t>(req.width) * bpp * static_cast<std::uint64_t>(req.height);
    resp.mem_alignment = 256;
    std::uint64_t type_bits = 0;
    const std::vector<MemoryType> types = mock_memory_types();
    const std::uint64_t kHostVisibleCoherent =
        kMemoryPropertyHostVisible | kMemoryPropertyHostCoherent;
    for (std::size_t i = 0; i < types.size(); ++i) {
        const bool host = (types[i].property_flags & kHostVisibleCoherent) == kHostVisibleCoherent;
        const bool devlocal = (types[i].property_flags & kMemoryPropertyDeviceLocal) != 0;
        if ((req.tiling == kImageTilingLinear && host) ||
            (req.tiling == kImageTilingOptimal && devlocal && !host)) {
            type_bits |= (1ull << i);
        }
    }
    resp.mem_type_bits = type_bits;
    return resp;
}

CreateDescriptorPoolResponse
MockVulkanBackend::create_descriptor_pool(const CreateDescriptorPoolRequest& req) {
    CreateDescriptorPoolResponse resp;
    const auto dev = devices_.find(req.device);
    if (dev == devices_.end()) {
        resp.reason = "unknown device handle";
        return resp;
    }
    if (req.max_sets < 1 || req.max_sets > kMaxDescriptorPoolSets) {
        resp.reason =
            "descriptor pool maxSets must be 1.." + std::to_string(kMaxDescriptorPoolSets);
        return resp;
    }
    // GL/zink: faithful -- any pool-size types/counts, any flags; the real pool is the
    // per-type budget authority. Only a nonzero-count + at-least-one-size sanity check stays.
    if (req.pool_sizes.empty()) {
        resp.reason = "descriptor pool must have at least one pool size";
        return resp;
    }
    for (const DescriptorPoolSizeDesc& ps : req.pool_sizes) {
        if (ps.descriptor_count < 1) {
            resp.reason = "descriptor pool size must have count >= 1";
            return resp;
        }
    }
    // Vulkan 1.3 support (inlineUniformBlock): an INLINE_UNIFORM_BLOCK pool size's descriptorCount
    // is a BYTE budget and is only well-formed when VkDescriptorPoolInlineUniformBlockCreateInfo
    // rode the wire (max_inline_uniform_block_bindings > 0, the spec pairing). Either presence is
    // gated on the device's enabled inlineUniformBlock bit -- the real worker chains the struct to
    // the host, so a pre-IUB device fails closed HERE instead (mock == real).
    bool any_iub_pool_size = false;
    for (const DescriptorPoolSizeDesc& ps : req.pool_sizes) {
        if (ps.type != kDescriptorTypeInlineUniformBlock) {
            continue;
        }
        any_iub_pool_size = true;
        if ((ps.descriptor_count % 4) != 0) {
            resp.reason = "inline-uniform-block pool size must be a multiple of 4 (a byte budget)";
            return resp;
        }
    }
    if ((any_iub_pool_size || req.max_inline_uniform_block_bindings > 0) &&
        (dev->second.vk13_feature_bits & kVk13FeatureInlineUniformBlock) == 0) {
        resp.reason = "inline uniform block requires the enabled inlineUniformBlock feature";
        return resp;
    }
    if (any_iub_pool_size && req.max_inline_uniform_block_bindings <= 0) {
        resp.reason = "an inline-uniform-block pool size requires "
                      "VkDescriptorPoolInlineUniformBlockCreateInfo";
        return resp;
    }
    // descriptorIndexing: the pool UPDATE_AFTER_BIND flag is a CONTAINER
    // flag (limit-bucket selection) -- valid without any UAB feature; recorded so allocate can
    // enforce that a UAB-POOL layout's sets come from such a pool. The per-binding UAB flags are
    // where the feature gates live (the shared layout admission), mock == real.
    const bool pool_uab = (req.flags & kDescriptorPoolCreateUpdateAfterBind) != 0;
    const std::uint64_t h = next_handle_++;
    DescriptorPool pool;
    pool.device = req.device;
    pool.max_sets = req.max_sets;
    pool.sets_remaining = req.max_sets;
    pool.update_after_bind = pool_uab;
    descriptor_pools_.emplace(h, std::move(pool));
    dev->second.descriptor_pools.insert(h);
    resp.ok = true;
    resp.reason = "ok";
    resp.pool = h;
    return resp;
}

StatusResponse MockVulkanBackend::destroy_descriptor_pool(const HandleRequest& req) {
    StatusResponse resp;
    const auto it = descriptor_pools_.find(req.handle);
    if (it == descriptor_pools_.end()) {
        resp.ok = false;
        resp.reason = "unknown descriptor pool handle";
        return resp;
    }
    // Destroying a pool frees all its sets (Vulkan). Each freed set invalidates any recorded
    // command buffer that baked it (replay/destroy UAF guard).
    for (const std::uint64_t set_handle : it->second.sets) {
        invalidate_cbs_referencing(set_handle);
        descriptor_sets_.erase(set_handle);
    }
    const auto dev = devices_.find(it->second.device);
    if (dev != devices_.end()) {
        dev->second.descriptor_pools.erase(req.handle);
    }
    descriptor_pools_.erase(it);
    resp.ok = true;
    resp.reason = "ok";
    return resp;
}

AllocateDescriptorSetsResponse
MockVulkanBackend::allocate_descriptor_sets(const AllocateDescriptorSetsRequest& req) {
    AllocateDescriptorSetsResponse resp;
    const auto dev = devices_.find(req.device);
    if (dev == devices_.end()) {
        resp.reason = "unknown device handle";
        return resp;
    }
    const auto pool = descriptor_pools_.find(req.pool);
    if (pool == descriptor_pools_.end() || pool->second.device != req.device) {
        resp.reason = "descriptor pool is unknown or on a different device";
        return resp;
    }
    if (req.set_layouts.empty() ||
        req.set_layouts.size() > static_cast<std::size_t>(kMaxAllocateDescriptorSets)) {
        resp.reason = "allocate descriptor sets needs 1.." +
                      std::to_string(kMaxAllocateDescriptorSets) + " set layouts";
        return resp;
    }
    // Validate the WHOLE batch before minting any set (atomic accounting): each
    // layout known + same-device, the set count fits maxSets remaining, and the summed descriptors
    // fit the pool's remaining per-type budget (UNIFORM_BUFFER + COMBINED_IMAGE_SAMPLER).
    // descriptorIndexing: a non-empty variable_counts must PARALLEL the set
    // list; per set the count is IGNORED for layouts without a variable last binding and must be
    // <= the layout's declared max for layouts with one. A UAB-pool layout's sets must come from
    // a UAB pool.
    if (!req.variable_counts.empty() && req.variable_counts.size() != req.set_layouts.size()) {
        resp.reason = "variable_counts must parallel set_layouts (or be absent)";
        return resp;
    }
    for (std::size_t i = 0; i < req.set_layouts.size(); ++i) {
        const auto it = descriptor_set_layouts_.find(req.set_layouts[i]);
        if (it == descriptor_set_layouts_.end() || it->second.device != req.device) {
            resp.reason = "allocate references a set layout not on the device";
            return resp;
        }
        if ((it->second.layout_flags & kDescriptorSetLayoutCreateUpdateAfterBindPool) != 0 &&
            !pool->second.update_after_bind) {
            resp.reason = "UPDATE_AFTER_BIND_POOL layout requires an UPDATE_AFTER_BIND pool";
            return resp;
        }
        if (!req.variable_counts.empty()) {
            for (const DescriptorSetLayoutBinding& b : it->second.bindings) {
                if ((b.binding_flags & kDescriptorBindingVariableDescriptorCount) != 0 &&
                    req.variable_counts[i] > static_cast<std::uint64_t>(b.descriptor_count)) {
                    resp.reason = "variable descriptor count exceeds the layout's declared max";
                    return resp;
                }
            }
        }
    }
    if (static_cast<long long>(req.set_layouts.size()) > pool->second.sets_remaining) {
        resp.reason = "descriptor pool has no room for this many sets (maxSets exceeded)";
        return resp;
    }
    // All checks passed -- mint sets (slots start uninitialized). The per-type budget is the
    // real pool's job (the mock has no host pool, so maxSets is its only accounting). A variable
    // last binding's slot vector is sized to the ALLOCATED count (absent pNext -> 0 per spec);
    // non-variable bindings stay at the layout count. Vulkan 1.3 support (inlineUniformBlock): an
    // IUB binding's descriptor_count is its BYTE size, so its vector models one slot per byte
    // (bounded at layout create by kMaxInlineUniformBlockBytes).
    for (std::size_t i = 0; i < req.set_layouts.size(); ++i) {
        const std::uint64_t sl = req.set_layouts[i];
        const auto& layout = descriptor_set_layouts_.at(sl);
        const std::uint64_t h = next_handle_++;
        DescriptorSet set;
        set.device = req.device;
        set.pool = req.pool;
        set.layout = sl;
        for (const DescriptorSetLayoutBinding& b : layout.bindings) {
            std::size_t count = static_cast<std::size_t>(b.descriptor_count);
            if ((b.binding_flags & kDescriptorBindingVariableDescriptorCount) != 0) {
                count = req.variable_counts.empty()
                            ? 0
                            : static_cast<std::size_t>(req.variable_counts[i]);
                set.variable_count = static_cast<long long>(count);
            }
            set.slots[b.binding] = std::vector<DescriptorSlot>(count);
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

StatusResponse MockVulkanBackend::update_descriptor_sets(const UpdateDescriptorSetsRequest& req) {
    StatusResponse resp;
    const auto dev = devices_.find(req.device);
    if (dev == devices_.end()) {
        resp.ok = false;
        resp.reason = "unknown device handle";
        return resp;
    }
    // A REJECTED update is fail-closed: it POISONS every targeted set -- marks its slots
    // uninitialized + invalidates recorded CBs that bound it -- so a previously-valid set cannot
    // stay draw-ready behind a rejected update. A successful update
    // re-initializes the slots. (validate-then-apply still holds: no partial slot write ever
    // lands.)
    // Poison only sets that exist AND belong to req.device: a malformed
    // cross-device request must fail, but must not mutate another device's descriptor graph.
    // Deduped via the set.
    std::set<std::uint64_t> targets;
    for (const WriteDescriptorSetDesc& w : req.writes) {
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
                for (DescriptorSlot& slot : slots) {
                    slot.initialized = false;
                    slot.buffer = 0;
                    slot.sampler = 0;
                    slot.image_view = 0;
                    slot.buffer_view = 0;
                }
            }
            invalidate_cbs_referencing(s);
        }
        StatusResponse r;
        r.ok = false;
        r.reason = why;
        return r;
    };
    if (req.writes.size() > static_cast<std::size_t>(kMaxDescriptorWrites)) {
        return poison_and_fail("too many descriptor writes in one update batch");
    }
    // GL/zink: FAITHFUL all-type update. Validate existence + array bounds per write (the
    // set + binding exist on the device, info-list length matches the count, referenced resources
    // exist); type-specific semantics (UBO usage/range, CIS layout/sampled-image) are the host's
    // job. Validate-then-apply still holds: no slot is mutated until every write passes.
    for (const WriteDescriptorSetDesc& w : req.writes) {
        const auto ds = descriptor_sets_.find(w.dst_set);
        if (ds == descriptor_sets_.end() || ds->second.device != req.device) {
            return poison_and_fail("descriptor write targets a set not on the device");
        }
        const auto slot_it = ds->second.slots.find(w.dst_binding);
        if (slot_it == ds->second.slots.end()) {
            return poison_and_fail("descriptor write targets a binding not in the set's layout");
        }
        if (w.descriptor_count < 1 || w.descriptor_count > kMaxDescriptorCount) {
            return poison_and_fail("descriptor write descriptor count is invalid");
        }
        if (w.dst_array_element < 0 ||
            static_cast<long long>(w.dst_array_element) + w.descriptor_count >
                static_cast<long long>(slot_it->second.size())) {
            return poison_and_fail(
                "descriptor write array range exceeds the binding's descriptor count");
        }
        const std::size_t n = static_cast<std::size_t>(w.descriptor_count);
        const int t = w.descriptor_type;
        // Vulkan 1.3 support (inlineUniformBlock): an IUB binding's slots ARE its bytes, so the
        // write and binding must agree on the type in BOTH directions -- a mismatched pair would
        // mis-model byte slots as descriptor slots (or vice versa) and forward an invalid host
        // write. (The layout outlives its sets: destroy is blocked while sets are allocated.)
        {
            int binding_type = -1;
            const auto sl = descriptor_set_layouts_.find(ds->second.layout);
            if (sl != descriptor_set_layouts_.end()) {
                for (const DescriptorSetLayoutBinding& b : sl->second.bindings) {
                    if (b.binding == w.dst_binding) {
                        binding_type = b.descriptor_type;
                    }
                }
            }
            if ((t == kDescriptorTypeInlineUniformBlock) !=
                (binding_type == kDescriptorTypeInlineUniformBlock)) {
                return poison_and_fail(
                    "descriptor write and binding disagree on the INLINE_UNIFORM_BLOCK type");
            }
        }
        const bool is_buffer =
            (t == kDescriptorTypeUniformBuffer || t == kDescriptorTypeStorageBuffer ||
             t == kDescriptorTypeUniformBufferDynamic || t == kDescriptorTypeStorageBufferDynamic);
        const bool is_texel =
            (t == kDescriptorTypeUniformTexelBuffer || t == kDescriptorTypeStorageTexelBuffer);
        if (t == kDescriptorTypeInlineUniformBlock) {
            // Vulkan 1.3 support (inlineUniformBlock): descriptorCount is the BYTE count and
            // dstArrayElement the BYTE offset (each a multiple of 4, the spec VUs); the slot
            // vector was sized to the binding's byte size, so the generic range check above
            // already bounds the write. The payload is the raw inline_data bytes -- an IUB write
            // references no objects, so the structured info lists must stay empty (an ICD that
            // saw a malformed pNext forwards EMPTY inline_data and the batch rejects here).
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
        } else if (is_buffer) {
            if (w.buffer_infos.size() != n) {
                return poison_and_fail(
                    "descriptor write count disagrees with the buffer-info list");
            }
            for (const DescriptorBufferInfoDesc& bi : w.buffer_infos) {
                const auto bf = buffers_.find(bi.buffer);
                if (bf == buffers_.end() || bf->second.device != req.device) {
                    return poison_and_fail(
                        "descriptor write references a buffer not on the device");
                }
                if (bf->second.bound_memory == 0) {
                    return poison_and_fail("descriptor write buffer is not bound to memory");
                }
                if (bi.offset >= bf->second.size) {
                    return poison_and_fail(
                        "descriptor write buffer offset is outside the logical buffer");
                }
                if (bi.range != kVkWholeSize &&
                    (bi.range == 0 || bi.range > bf->second.size - bi.offset)) {
                    return poison_and_fail(
                        "descriptor write buffer range is outside the logical buffer");
                }
            }
        } else if (is_texel) {
            if (w.texel_buffer_views.size() != n) {
                return poison_and_fail("descriptor write count disagrees with the texel-view list");
            }
            for (const std::uint64_t bvh : w.texel_buffer_views) {
                const auto bv = buffer_views_.find(bvh);
                if (bv == buffer_views_.end() || bv->second.device != req.device) {
                    return poison_and_fail(
                        "descriptor write references a buffer view not on the device");
                }
            }
        } else { // image / sampler types
            if (w.image_infos.size() != n) {
                return poison_and_fail("descriptor write count disagrees with the image-info list");
            }
            for (const DescriptorImageInfoDesc& ii : w.image_infos) {
                if (ii.sampler != 0) {
                    const auto sm = samplers_.find(ii.sampler);
                    if (sm == samplers_.end() || sm->second.device != req.device) {
                        return poison_and_fail(
                            "descriptor write references a sampler not on the device");
                    }
                }
                if (ii.image_view != 0) {
                    const auto iv = image_views_.find(ii.image_view);
                    if (iv == image_views_.end() || iv->second.device != req.device) {
                        return poison_and_fail(
                            "descriptor write references an image view not on the device");
                    }
                }
            }
        }
    }
    // All writes valid -- apply (mark each slot initialized + record its generic referent).
    // Vulkan 1.3 support (inlineUniformBlock): an IUB write's byte-slots are marked by this same
    // loop (descriptor_count IS the byte count and every info list is empty, so the referents stay
    // 0
    // -- an IUB write references no objects, so no destroy-consult/dangle interaction).
    for (const WriteDescriptorSetDesc& w : req.writes) {
        DescriptorSet& set = descriptor_sets_.at(w.dst_set);
        std::vector<DescriptorSlot>& slots = set.slots.at(w.dst_binding);
        for (int i = 0; i < w.descriptor_count; ++i) {
            DescriptorSlot& slot = slots[static_cast<std::size_t>(w.dst_array_element + i)];
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

bool MockVulkanBackend::descriptor_set_draw_ready(std::uint64_t set, std::uint64_t device,
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
    // GL/zink: every layout-declared slot must be WRITTEN (the destroy-consult clears the
    // initialized flag when a referent is freed, so this also catches a dangling reference).
    // Type-specific resource liveness is dropped -- it does not generalize across all descriptor
    // types; the host driver (+ the destroy-consult) is the authority.
    //
    // descriptorIndexing (the readiness rework): a
    // PARTIALLY_BOUND binding may hold unwritten/dangling slots (dynamic USE is invisible to the
    // relay; the host/validation layer owns it), and an UPDATE_AFTER_BIND binding is exempt at
    // RECORD time (record-now-update-before-submit is the feature; replay binds live handles so
    // the late update lands). A variable-count binding's vector was sized to the ALLOCATED count,
    // so this loop inherently judges that count. CLASSIC bindings keep the fail-closed check.
    //
    // Vulkan 1.3 support (inlineUniformBlock): an IUB binding's slots ARE its bytes (one per byte,
    // sized at allocate), so the same rule composes unchanged -- every byte a classic IUB binding
    // declares must have been covered by a successful write.
    for (const DescriptorSetLayoutBinding& b : sl->second.bindings) {
        const auto slot_it = ds->second.slots.find(b.binding);
        if (slot_it == ds->second.slots.end()) {
            reason = "draw bound a descriptor set missing a layout-required binding";
            return false;
        }
        if ((b.binding_flags &
             (kDescriptorBindingPartiallyBound | kDescriptorBindingUpdateAfterBind)) != 0) {
            continue; // host-owned completeness (PARTIALLY_BOUND / UPDATE_AFTER_BIND)
        }
        for (const DescriptorSlot& slot : slot_it->second) {
            if (!slot.initialized) {
                reason = "draw bound a descriptor set with an uninitialized (or dangling) slot";
                return false;
            }
        }
    }
    return true;
}

CreateGraphicsPipelinesResponse
MockVulkanBackend::create_graphics_pipelines(const CreateGraphicsPipelinesRequest& req) {
    CreateGraphicsPipelinesResponse resp;
    const auto dev = devices_.find(req.device);
    if (dev == devices_.end()) {
        resp.ok = false;
        resp.reason = "unknown device handle";
        return resp;
    }
    // Vulkan 1.3 support (subgroupSizeControl): the stage shapes are gated on the enabled vk13
    // bits, mock == real (validate-only here). REQUIRE_FULL_SUBGROUPS is compute-only, so any
    // graphics stage carrying it rejects via the allowed mask.
    for (const ShaderStageDesc& s : req.stages) {
        if (s.stage_flags == 0 && s.required_subgroup_size == 0) {
            continue;
        }
        if ((dev->second.vk13_feature_bits & kVk13FeatureSubgroupSizeControl) == 0) {
            resp.ok = false;
            resp.reason = "pipeline stage subgroup-size shape requires the enabled "
                          "subgroupSizeControl feature";
            return resp;
        }
        if ((s.stage_flags & ~0x1ll) != 0) { // ALLOW_VARYING_SUBGROUP_SIZE only
            resp.ok = false;
            resp.reason = "pipeline stage flags outside the served set";
            return resp;
        }
    }
    if (req.pipeline_cache != 0) {
        resp.ok = false;
        resp.reason = "graphics pipeline requires pipelineCache == VK_NULL_HANDLE";
        return resp;
    }
    // Tess/geom breadth: FAITHFUL stage admission, mock == real. Any of the five
    // graphics stages (VERTEX/TESS_CONTROL/TESS_EVALUATION/GEOMETRY/FRAGMENT), no duplicates,
    // VERTEX required (a graphics pipeline without one is structurally invalid); each module
    // live + same-device; the entry point rides faithfully (the real backend substitutes "main"
    // for an empty one). The old exactly-VERTEX+FRAGMENT gate was a mock != real divergence:
    // zink's tessellation/geometry pipelines were REJECTED here while the faithful real path
    // accepted and rendered them (proven by vkrelay2-tessgeom-canary).
    {
        constexpr int kGraphicsStages = 1 | 2 | 4 | 8 | 16; // V|TCS|TES|G|F
        int seen = 0;
        if (req.stages.empty()) {
            resp.ok = false;
            resp.reason = "graphics pipeline has no shader stages";
            return resp;
        }
        for (const ShaderStageDesc& s : req.stages) {
            if (s.stage <= 0 || (s.stage & ~kGraphicsStages) != 0 ||
                (s.stage & (s.stage - 1)) != 0) {
                resp.ok = false;
                resp.reason = "graphics pipeline stage is not a single graphics stage bit";
                return resp;
            }
            if ((seen & s.stage) != 0) {
                resp.ok = false;
                resp.reason = "graphics pipeline has duplicate shader stages";
                return resp;
            }
            seen |= s.stage;
            const auto sm = shader_modules_.find(s.module);
            if (sm == shader_modules_.end() || sm->second.device != req.device) {
                resp.ok = false;
                resp.reason = "pipeline stage references a shader module not on the device";
                return resp;
            }
        }
        if ((seen & vk3b::kShaderStageVertex) == 0) {
            resp.ok = false;
            resp.reason = "graphics pipeline requires a VERTEX stage";
            return resp;
        }
        // Tessellation coupling (the real driver's structural rules, so mock-passing streams
        // stay real-plausible): BOTH tess stages or neither; tess stages <=> PATCH_LIST
        // topology; PATCH_LIST needs patchControlPoints in [1, 32] (the spec-minimum
        // maxTessellationPatchSize -- the host's real limit gates above that).
        const bool has_tcs = (seen & 2) != 0;
        const bool has_tes = (seen & 4) != 0;
        if (has_tcs != has_tes) {
            resp.ok = false;
            resp.reason = "graphics pipeline needs both tessellation stages or neither";
            return resp;
        }
        const bool patch_topology = req.topology == 10; // VK_PRIMITIVE_TOPOLOGY_PATCH_LIST
        if (req.topology < 0 || req.topology > 10) {
            resp.ok = false;
            resp.reason = "graphics pipeline topology out of range";
            return resp;
        }
        if (has_tcs != patch_topology) {
            resp.ok = false;
            resp.reason = "tessellation stages require PATCH_LIST topology (and vice versa)";
            return resp;
        }
        if (patch_topology && (req.patch_control_points < 1 || req.patch_control_points > 32)) {
            resp.ok = false;
            resp.reason = "PATCH_LIST requires patchControlPoints in [1, 32]";
            return resp;
        }
    }
    // Vertex input (vertex-attr-divisor: widened to MATCH the real backend, which copies the
    // bindings/attributes and forwards them to the host, validating only that stride/offset fit
    // u32). So the mock accepts multi-binding / INSTANCE-rate / any format and leaves the general
    // attribute-binding VUIDs to the host -- the real backend does not check them either, and a
    // mock stricter than real would BREAK mock == real. The carried arrays are
    // authoritative; the counts must agree with them. Full vertex-input
    // faithfulness beyond this stays the separate mock-broadening item.
    if (req.vertex_binding_count != static_cast<int>(req.vertex_bindings.size()) ||
        req.vertex_attribute_count != static_cast<int>(req.vertex_attributes.size())) {
        resp.ok = false;
        resp.reason = "vertex binding/attribute counts disagree with the carried arrays";
        return resp;
    }
    for (const VertexBindingDesc& vb : req.vertex_bindings) {
        if (vb.stride > kVkFlagsMax) {
            resp.ok = false;
            resp.reason = "vertex binding stride exceeds UINT32_MAX";
            return resp;
        }
    }
    for (const VertexAttributeDesc& va : req.vertex_attributes) {
        if (va.offset > kVkFlagsMax) {
            resp.ok = false;
            resp.reason = "vertex attribute offset exceeds UINT32_MAX";
            return resp;
        }
    }
    // vertex-attr-divisor: the SAME shared content + feature-gate validation the ICD and real
    // worker apply (mock == real). The device's enabled feature bits gate the divisor VALUES.
    {
        std::string div_why;
        if (!vertex_binding_divisors_ok(
                req.vertex_divisor_present, req.vertex_bindings, req.vertex_binding_divisors,
                dev->second.enabled_exts.count(kVertexAttributeDivisorExtensionName) != 0,
                dev->second.vertex_attr_divisor_feature_enabled,
                dev->second.vertex_attr_zero_divisor_feature_enabled, div_why)) {
            resp.ok = false;
            resp.reason = div_why;
            return resp;
        }
    }
    // geometry-stream: the SAME shared value gate the real worker applies, parameterized with the
    // mock's deterministic modeled properties (mock == real via the one helper).
    {
        std::string stream_why;
        if (!rasterization_stream_ok(
                req.stream_state_present, req.rasterization_stream,
                dev->second.enabled_exts.count(kTransformFeedbackExtensionName) != 0,
                dev->second.geometry_streams_feature_enabled, kMockMaxTransformFeedbackStreams,
                kMockTransformFeedbackStreamSelect, stream_why)) {
            resp.ok = false;
            resp.reason = stream_why;
            return resp;
        }
    }
    if (req.dynamic_states.size() != 2 || req.dynamic_states[0] != vk3b::kDynamicStateViewport ||
        req.dynamic_states[1] != vk3b::kDynamicStateScissor) {
        resp.ok = false;
        resp.reason = "graphics pipeline requires dynamic state {VIEWPORT, SCISSOR}";
        return resp;
    }
    if (req.cull_mode < 0 || req.cull_mode > vk3b::kCullModeMaxSubset || req.front_face < 0 ||
        req.front_face > vk3b::kFrontFaceClockwise) {
        resp.ok = false;
        resp.reason = "graphics pipeline has an out-of-range cull mode / front face";
        return resp;
    }
    const auto layout = pipeline_layouts_.find(req.layout);
    if (layout == pipeline_layouts_.end() || layout->second.device != req.device) {
        resp.ok = false;
        resp.reason = "pipeline layout is unknown or on a different device";
        return resp;
    }
    // (dynamic rendering): a DR pipeline (render_pass == 0 +
    // VkPipelineRenderingCreateInfo formats) validates its color-blend / depth-stencil state
    // against the CARRIED formats, not a render pass -- the formats are its draw-time compatibility
    // key. A render pass with DR info, or DR info with a viewMask, is rejected (fail-closed, mock
    // == real). Off the DR path (has_dynamic_rendering == 0) this is byte-identical to before.
    if (req.has_dynamic_rendering != 0) {
        // The device must have enabled VK_KHR_dynamic_rendering (a DR pipeline is
        // only valid where the extension is enabled; mock == real). The ICD only admits the
        // VkPipelineRenderingCreateInfo pNext on such a device, so a conformant app cannot reach
        // this; it is the backend-side safety net.
        // Vulkan 1.3 support: on an honest vk13 device DR is CORE -- the feature alone admits it.
        if ((dev->second.enabled_exts.count("VK_KHR_dynamic_rendering") == 0 &&
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
        // Required-feature audit: a DR pipeline viewMask is admitted only on a
        // multiview-enabled device (mock == real; the worker carries it to
        // VkPipelineRenderingCreateInfo.viewMask + keys draw compat on it). Negative = malformed.
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
        // Mirror the real backend's color-attachment cap so the mock oracle is
        // not wider than the real path (mock == real).
        if (req.dr_color_formats.size() >
            static_cast<std::size_t>(kMaxDynamicRenderingColorAttachments)) {
            resp.ok = false;
            resp.reason = "dynamic-rendering pipeline color attachment count out of range";
            return resp;
        }
        if (req.subpass > 0) {
            resp.ok = false;
            resp.reason = "dynamic-rendering pipeline subpass must be 0";
            return resp;
        }
        if (!req.color_blend_attachments.empty() &&
            req.color_blend_attachments.size() != req.dr_color_formats.size()) {
            resp.ok = false;
            resp.reason = "pipeline color blend attachment count does not match the "
                          "dynamic-rendering color attachment count";
            return resp;
        }
        const bool dr_has_depth_stencil = req.dr_depth_format != 0 || req.dr_stencil_format != 0;
        // Spec semantics, exactly (mock == real): a declared depth/stencil format REQUIRES the
        // state; carried state WITHOUT a declared format is legal and IGNORED (Mesa >= 25 zink
        // attaches a disabled depth-stencil struct to color-only DR pipelines; the old strict
        // "iff" rejected every one of them).
        if (dr_has_depth_stencil && !req.has_depth_stencil) {
            resp.ok = false;
            resp.reason = "dynamic-rendering pipeline declares a depth/stencil format but carries "
                          "no depth-stencil state";
            return resp;
        }
    } else {
        const auto rp = render_passes_.find(req.render_pass);
        if (rp == render_passes_.end() || rp->second.device != req.device) {
            resp.ok = false;
            resp.reason = "pipeline render pass is unknown or on a different device";
            return resp;
        }
        if (req.subpass != 0) {
            resp.ok = false;
            resp.reason = "graphics pipeline targets subpass 0";
            return resp;
        }
        // MRT (mock == real): an explicit blend array must match the subpass's TRUE
        // color-attachment count (the ref vector's length INCLUDING UNUSED holes; legacy passes
        // derive 0/1 from the scalar compat key) -- the driver-tolerated VUID violation that
        // silently half-rendered the 2-color probe is OUR named rejection.
        const std::size_t color_ref_count = !rp->second.color_refs.empty()
                                                ? rp->second.color_refs.size()
                                                : (rp->second.color_format != 0 ? 1u : 0u);
        if (!req.color_blend_attachments.empty() &&
            req.color_blend_attachments.size() != color_ref_count) {
            resp.ok = false;
            resp.reason = "pipeline color blend attachment count does not match the render pass "
                          "subpass color attachment count";
            return resp;
        }
        // Depth-stencil state: a pipeline must declare depth-stencil state IFF its
        // render pass has a depth attachment (a depth-attachment subpass requires
        // pDepthStencilState; a color-only one must not carry depth state in our subset). When
        // present: test/write are 0/1 and the compare op is LESS_OR_EQUAL (vkcube), no stencil, no
        // depth bounds (the worker fixes those).
        const bool rp_has_depth = rp->second.depth_format != 0;
        if (req.has_depth_stencil != rp_has_depth) {
            resp.ok = false;
            resp.reason =
                "graphics pipeline depth-stencil state must be present iff the render pass has "
                "a depth attachment";
            return resp;
        }
    }
    if (req.has_depth_stencil) {
        if ((req.depth_test_enable != 0 && req.depth_test_enable != 1) ||
            (req.depth_write_enable != 0 && req.depth_write_enable != 1) ||
            req.depth_compare_op != kCompareOpLessOrEqual) {
            resp.ok = false;
            resp.reason = "depth-stencil state is test/write in {0,1}, compare "
                          "LESS_OR_EQUAL, no stencil/bounds";
            return resp;
        }
    }
    const std::uint64_t h = next_handle_++;
    Pipeline pp;
    pp.device = req.device;
    pp.layout = req.layout;
    pp.render_pass = req.render_pass;
    pp.vertex_binding_count = static_cast<int>(req.vertex_bindings.size()); // 0 = bufferless
    pp.has_depth = req.has_depth_stencil;
    // (dynamic rendering): the DR compatibility key (formats + viewMask) draw validation
    // compares against the active dynamic-rendering scope.
    if (req.has_dynamic_rendering != 0) {
        pp.dynamic_rendering = true;
        pp.view_mask = req.dr_view_mask;
        pp.color_formats = req.dr_color_formats;
        pp.depth_format = req.dr_depth_format;
        pp.stencil_format = req.dr_stencil_format;
    }
    pipelines_.emplace(h, pp);
    dev->second.pipelines.insert(h);
    resp.ok = true;
    resp.reason = "ok";
    resp.pipeline = h;
    return resp;
}

// Compute: the bounded compute-create subset. The heavy pipeline state that makes
// graphics creation long does not exist here -- one COMPUTE stage referencing a live module +
// a live layout, entry point carried faithfully (non-empty). pipeline_cache must be 0 (the
// ICD's app cache is a local no-op; the field rides for shape symmetry only).
CreateComputePipelinesResponse
MockVulkanBackend::create_compute_pipelines(const CreateComputePipelinesRequest& req) {
    CreateComputePipelinesResponse resp;
    const auto dev = devices_.find(req.device);
    if (dev == devices_.end()) {
        resp.ok = false;
        resp.reason = "unknown device handle";
        return resp;
    }
    // Vulkan 1.3 support (subgroupSizeControl): mirror the real backend's stage-shape gates
    // (validate-only). ALLOW_VARYING = 0x1; REQUIRE_FULL_SUBGROUPS = 0x2 additionally needs the
    // computeFullSubgroups bit.
    if (req.stage_flags != 0 || req.required_subgroup_size != 0) {
        if ((dev->second.vk13_feature_bits & kVk13FeatureSubgroupSizeControl) == 0) {
            resp.ok = false;
            resp.reason = "compute stage subgroup-size shape requires the enabled "
                          "subgroupSizeControl feature";
            return resp;
        }
        const long long allowed =
            0x1ll |
            ((dev->second.vk13_feature_bits & kVk13FeatureComputeFullSubgroups) != 0 ? 0x2ll : 0ll);
        if ((req.stage_flags & ~allowed) != 0) {
            resp.ok = false;
            resp.reason = "compute stage flags outside the served set";
            return resp;
        }
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
    const auto pl = pipeline_layouts_.find(req.layout);
    if (pl == pipeline_layouts_.end() || pl->second.device != req.device) {
        resp.ok = false;
        resp.reason = "compute pipeline references a pipeline layout not on the device";
        return resp;
    }
    if (req.entry_point.empty()) {
        resp.ok = false;
        resp.reason = "compute pipeline entry point must be non-empty";
        return resp;
    }
    const std::uint64_t h = next_handle_++;
    Pipeline pp;
    pp.device = req.device;
    pp.layout = req.layout;
    pp.compute = true; // KIND: bind point must match (validated at record)
    pipelines_.emplace(h, pp);
    dev->second.pipelines.insert(h);
    resp.ok = true;
    resp.reason = "ok";
    resp.pipeline = h;
    return resp;
}

StatusResponse MockVulkanBackend::destroy_pipeline(const HandleRequest& req) {
    StatusResponse resp;
    const auto it = pipelines_.find(req.handle);
    if (it == pipelines_.end()) {
        resp.ok = false;
        resp.reason = "unknown pipeline handle";
        return resp;
    }
    invalidate_cbs_referencing(req.handle);
    const auto dev = devices_.find(it->second.device);
    if (dev != devices_.end()) {
        dev->second.pipelines.erase(req.handle);
    }
    pipelines_.erase(it);
    resp.ok = true;
    resp.reason = "ok";
    return resp;
}

void serve_vulkan_rpc(RpcChannel& channel, VulkanBackend& backend) {
    DecodedOpTrace op_trace;
    RpcMessage req;
    try {
        while (channel.recv(req)) {
            // worker hook: the reply lambda below is the single funnel every Ok
            // response takes -- the window covers body parse + backend handler + response marshal
            // (the worker-side "handler duration" the client-minus-worker delta subtracts).
            RpcProfile* prof = profile_instance();
            std::uint64_t iter_t0 = 0;
            if (prof != nullptr) {
                prof->used_as_worker = true;
                iter_t0 = profile_now_us();
            }
            RpcMessage resp;
            resp.op = req.op;
            resp.request_id = req.request_id;

            // Requests must carry status 0 (protocol contract); a nonzero status
            // is malformed and is rejected before any op can take effect.
            if (req.status != 0) {
                resp.status = static_cast<std::uint32_t>(RpcStatus::BadRequest);
                VKR_WARN(kComponent) << "rpc request with nonzero status " << req.status;
                channel.send(resp);
                continue;
            }

            // Parse the JSON body of a known op; on a malformed body, reply
            // BadRequest and skip the op. reply() sends an Ok response.
            json::Value body;
            auto parse_body = [&]() -> bool {
                std::string err;
                if (json::Value::try_parse(req.body, body, err)) {
                    return true;
                }
                resp.status = static_cast<std::uint32_t>(RpcStatus::BadRequest);
                channel.send(resp);
                return false;
            };
            auto reply = [&](const json::Value& jbody) {
                resp.status = static_cast<std::uint32_t>(RpcStatus::Ok);
                resp.body = jbody.dump(0);
                if (prof != nullptr) {
                    prof->record(req.op, resp.body.size(), req.body.size(),
                                 profile_now_us() - iter_t0);
                }
                channel.send(resp);
            };

            switch (static_cast<RpcOp>(req.op)) {
            case RpcOp::NegotiateCapabilities: {
                if (!parse_body()) {
                    break;
                }
                const CapabilitiesRequest creq = CapabilitiesRequest::from_body(body);
                const CapabilitiesResponse cresp = backend.negotiate(creq);
                VKR_INFO(kComponent)
                    << "negotiate caps requested=" << creq.requested_api_major << "."
                    << creq.requested_api_minor << " -> " << (cresp.ok ? "ok" : "fail")
                    << " negotiated=" << cresp.negotiated_api_major << "."
                    << cresp.negotiated_api_minor << " device=" << cresp.device.device_name;
                reply(cresp.to_body());
                break;
            }
            case RpcOp::CreateInstance:
                if (parse_body()) {
                    reply(
                        backend.create_instance(CreateInstanceRequest::from_body(body)).to_body());
                }
                break;
            case RpcOp::EnumeratePhysicalDevices:
                if (parse_body()) {
                    EnumeratePhysicalDevicesResponse er = backend.enumerate_physical_devices(
                        EnumeratePhysicalDevicesRequest::from_body(body));
                    // raw_readback and raw_record are SERVE-LOOP protocol
                    // capabilities (the raw codecs live in this loop, backend-agnostic), so the
                    // loop stamps them here rather than each backend advertising them.
                    for (auto& d : er.devices) {
                        d.caps.raw_readback = true;
                        d.caps.raw_record = true;
                    }
                    reply(er.to_body());
                }
                break;
            case RpcOp::CreateDevice: {
                if (!parse_body()) {
                    break;
                }
                const CreateDeviceRequest creq = CreateDeviceRequest::from_body(body);
                const CreateDeviceResponse cresp = backend.create_device(creq);
                VKR_INFO(kComponent) << "create_device instance=" << creq.instance
                                     << " physical_device=" << creq.physical_device << " -> "
                                     << (cresp.ok ? "ok" : cresp.reason);
                reply(cresp.to_body());
                break;
            }
            case RpcOp::DestroyDevice:
                if (parse_body()) {
                    reply(backend.destroy_device(HandleRequest::from_body(body)).to_body());
                }
                break;
            case RpcOp::DestroyInstance:
                if (parse_body()) {
                    reply(backend.destroy_instance(HandleRequest::from_body(body)).to_body());
                }
                break;
            case RpcOp::GetDeviceQueue:
                if (parse_body()) {
                    reply(
                        backend.get_device_queue(GetDeviceQueueRequest::from_body(body)).to_body());
                }
                break;
            case RpcOp::CreateCommandPool:
                if (parse_body()) {
                    reply(backend.create_command_pool(CreateCommandPoolRequest::from_body(body))
                              .to_body());
                }
                break;
            case RpcOp::DestroyCommandPool:
                if (parse_body()) {
                    reply(backend.destroy_command_pool(HandleRequest::from_body(body)).to_body());
                }
                break;
            case RpcOp::AllocateCommandBuffers:
                if (parse_body()) {
                    reply(backend
                              .allocate_command_buffers(
                                  AllocateCommandBuffersRequest::from_body(body))
                              .to_body());
                }
                break;
            case RpcOp::FreeCommandBuffers:
                if (parse_body()) {
                    reply(backend.free_command_buffers(FreeCommandBuffersRequest::from_body(body))
                              .to_body());
                }
                break;
            case RpcOp::CreateFence:
                if (parse_body()) {
                    reply(backend.create_fence(CreateFenceRequest::from_body(body)).to_body());
                }
                break;
            case RpcOp::DestroyFence:
                if (parse_body()) {
                    reply(backend.destroy_fence(HandleRequest::from_body(body)).to_body());
                }
                break;
            case RpcOp::CreateSemaphoreOp:
                if (parse_body()) {
                    reply(backend.create_semaphore(CreateSemaphoreRequest::from_body(body))
                              .to_body());
                }
                break;
            case RpcOp::DestroySemaphore:
                if (parse_body()) {
                    reply(backend.destroy_semaphore(HandleRequest::from_body(body)).to_body());
                }
                break;
            case RpcOp::AllocateMemory: {
                if (parse_body()) {
                    const AllocateMemoryRequest areq = AllocateMemoryRequest::from_body(body);
                    const AllocateMemoryResponse aresp = backend.allocate_memory(areq);
                    op_trace.allocate_memory(req.request_id, areq, aresp);
                    reply(aresp.to_body());
                }
                break;
            }
            case RpcOp::FreeMemory:
                if (parse_body()) {
                    reply(backend.free_memory(HandleRequest::from_body(body)).to_body());
                }
                break;
            case RpcOp::CreateSurface:
                if (parse_body()) {
                    reply(backend.create_surface(CreateSurfaceRequest::from_body(body)).to_body());
                }
                break;
            case RpcOp::DestroySurface:
                if (parse_body()) {
                    reply(backend.destroy_surface(HandleRequest::from_body(body)).to_body());
                }
                break;
            case RpcOp::CreateSwapchain:
                if (parse_body()) {
                    const CreateSwapchainRequest creq = CreateSwapchainRequest::from_body(body);
                    const CreateSwapchainResponse cresp = backend.create_swapchain(creq);
                    op_trace.create_swapchain(req.request_id, creq, cresp);
                    reply(cresp.to_body());
                }
                break;
            case RpcOp::DestroySwapchain:
                if (parse_body()) {
                    reply(backend.destroy_swapchain(HandleRequest::from_body(body)).to_body());
                }
                break;
            case RpcOp::GetSwapchainImages:
                if (parse_body()) {
                    const GetSwapchainImagesRequest greq =
                        GetSwapchainImagesRequest::from_body(body);
                    const GetSwapchainImagesResponse gresp = backend.get_swapchain_images(greq);
                    op_trace.get_swapchain_images(req.request_id, greq, gresp);
                    reply(gresp.to_body());
                }
                break;
            case RpcOp::AcquireNextImage:
                if (parse_body()) {
                    const AcquireNextImageRequest areq = AcquireNextImageRequest::from_body(body);
                    const AcquireNextImageResponse aresp = backend.acquire_next_image(areq);
                    op_trace.acquire_next_image(req.request_id, areq, aresp);
                    reply(aresp.to_body());
                }
                break;
            case RpcOp::QueuePresent:
                if (parse_body()) {
                    const QueuePresentRequest preq = QueuePresentRequest::from_body(body);
                    const QueuePresentResponse presp = backend.queue_present(preq);
                    op_trace.queue_present(req.request_id, preq, presp);
                    reply(presp.to_body());
                }
                break;
            case RpcOp::RecordCommandBuffer: {
                // the record handler's phase split -- json_parse (wire text ->
                // Value), decode (Value -> request incl. args_blob hex), execute (the backend
                // call; its validate/replay sub-split is timed inside the backends). Zero clock
                // reads when profiling is off.
                const std::uint64_t t_parse0 = prof != nullptr ? profile_now_us() : 0;
                if (!parse_body()) {
                    break;
                }
                const std::uint64_t t_decode0 = prof != nullptr ? profile_now_us() : 0;
                const RecordCommandBufferRequest rreq = RecordCommandBufferRequest::from_body(body);
                op_trace.record_command_buffer(req.request_id, rreq);
                const std::uint64_t t_exec0 = prof != nullptr ? profile_now_us() : 0;
                const StatusResponse rresp = backend.record_command_buffer(rreq);
                if (prof != nullptr) {
                    RecordPhaseStats& ph = prof->record_phases;
                    ph.count += 1;
                    ph.commands += rreq.commands.size();
                    ph.json_parse_us += t_decode0 - t_parse0;
                    ph.decode_us += t_exec0 - t_decode0;
                    ph.execute_us += profile_now_us() - t_exec0;
                }
                reply(rresp.to_body());
                break;
            }
            case RpcOp::RecordCommandBufferRaw: {
                // the binary record body -- no JSON parse, no hex. from_wire
                // feeds the SAME request struct into the unchanged validate-then-record backend
                // boundary; a malformed frame is BadRequest (mirroring parse_body), logged with
                // its decode reason. Phase timers: json_parse stays 0, decode = from_wire.
                const std::uint64_t t_decode0 = prof != nullptr ? profile_now_us() : 0;
                std::string werr;
                const RecordCommandBufferRequest rreq =
                    RecordCommandBufferRequest::from_wire(req.body, werr);
                if (!werr.empty()) {
                    VKR_WARN(kComponent) << "record_command_buffer_raw rejected: " << werr;
                    resp.status = static_cast<std::uint32_t>(RpcStatus::BadRequest);
                    channel.send(resp);
                    break;
                }
                op_trace.record_command_buffer(req.request_id, rreq);
                const std::uint64_t t_exec0 = prof != nullptr ? profile_now_us() : 0;
                const StatusResponse rresp = backend.record_command_buffer(rreq);
                if (prof != nullptr) {
                    RecordPhaseStats& ph = prof->record_phases;
                    ph.count += 1;
                    ph.commands += rreq.commands.size();
                    ph.decode_us += t_exec0 - t_decode0;
                    ph.execute_us += profile_now_us() - t_exec0;
                }
                reply(rresp.to_body());
                break;
            }
            case RpcOp::QueueSubmit:
                if (parse_body()) {
                    const QueueSubmitRequest sreq = QueueSubmitRequest::from_body(body);
                    const QueueSubmitResponse sresp = backend.queue_submit(sreq);
                    op_trace.queue_submit(req.request_id, sreq, sresp);
                    reply(sresp.to_body());
                }
                break;
            case RpcOp::QueueSubmit2:
                if (parse_body()) {
                    const QueueSubmit2Request sreq = QueueSubmit2Request::from_body(body);
                    const QueueSubmitResponse sresp = backend.queue_submit2(sreq);
                    op_trace.queue_submit2(req.request_id, sreq, sresp);
                    reply(sresp.to_body());
                }
                break;
            case RpcOp::ResetFences:
                if (parse_body()) {
                    reply(backend.reset_fences(ResetFencesRequest::from_body(body)).to_body());
                }
                break;
            case RpcOp::WaitForFences:
                if (parse_body()) {
                    reply(backend.wait_for_fences(WaitForFencesRequest::from_body(body)).to_body());
                }
                break;
            case RpcOp::GetFenceStatus:
                if (parse_body()) {
                    reply(backend.get_fence_status(HandleRequest::from_body(body)).to_body());
                }
                break;
            case RpcOp::CreateEventOp:
                if (parse_body()) {
                    reply(backend.create_event(CreateEventRequest::from_body(body)).to_body());
                }
                break;
            case RpcOp::DestroyEvent:
                if (parse_body()) {
                    reply(backend.destroy_event(HandleRequest::from_body(body)).to_body());
                }
                break;
            case RpcOp::GetEventStatus:
                if (parse_body()) {
                    reply(backend.get_event_status(HandleRequest::from_body(body)).to_body());
                }
                break;
            case RpcOp::SetEvent:
                if (parse_body()) {
                    reply(backend.set_event(HandleRequest::from_body(body)).to_body());
                }
                break;
            case RpcOp::ResetEvent:
                if (parse_body()) {
                    reply(backend.reset_event(HandleRequest::from_body(body)).to_body());
                }
                break;
            case RpcOp::QueueWaitIdle:
                if (parse_body()) {
                    reply(backend.queue_wait_idle(HandleRequest::from_body(body)).to_body());
                }
                break;
            case RpcOp::DeviceWaitIdle:
                if (parse_body()) {
                    reply(backend.device_wait_idle(HandleRequest::from_body(body)).to_body());
                }
                break;
            case RpcOp::WaitSemaphores:
                if (parse_body()) {
                    reply(
                        backend.wait_semaphores(WaitSemaphoresRequest::from_body(body)).to_body());
                }
                break;
            case RpcOp::SignalSemaphore:
                if (parse_body()) {
                    reply(backend.signal_semaphore(SignalSemaphoreRequest::from_body(body))
                              .to_body());
                }
                break;
            case RpcOp::GetSemaphoreCounterValue:
                if (parse_body()) {
                    reply(backend
                              .get_semaphore_counter_value(
                                  GetSemaphoreCounterValueRequest::from_body(body))
                              .to_body());
                }
                break;
            case RpcOp::GetSurfaceCapabilities:
                if (parse_body()) {
                    reply(backend
                              .get_surface_capabilities(
                                  GetSurfaceCapabilitiesRequest::from_body(body))
                              .to_body());
                }
                break;
            case RpcOp::GetSurfaceFormats:
                if (parse_body()) {
                    reply(backend.get_surface_formats(GetSurfaceFormatsRequest::from_body(body))
                              .to_body());
                }
                break;
            case RpcOp::GetSurfacePresentModes:
                if (parse_body()) {
                    reply(backend
                              .get_surface_present_modes(
                                  GetSurfacePresentModesRequest::from_body(body))
                              .to_body());
                }
                break;
            case RpcOp::GetSurfaceSupport:
                if (parse_body()) {
                    reply(backend.get_surface_support(GetSurfaceSupportRequest::from_body(body))
                              .to_body());
                }
                break;
            // Draw surface.
            case RpcOp::CreateImageView:
                if (parse_body()) {
                    const CreateImageViewRequest vreq = CreateImageViewRequest::from_body(body);
                    const CreateImageViewResponse vresp = backend.create_image_view(vreq);
                    op_trace.create_image_view(req.request_id, vreq, vresp);
                    reply(vresp.to_body());
                }
                break;
            case RpcOp::DestroyImageView:
                if (parse_body()) {
                    reply(backend.destroy_image_view(HandleRequest::from_body(body)).to_body());
                }
                break;
            case RpcOp::CreateBufferView:
                if (parse_body()) {
                    reply(backend.create_buffer_view(CreateBufferViewRequest::from_body(body))
                              .to_body());
                }
                break;
            case RpcOp::DestroyBufferView:
                if (parse_body()) {
                    reply(backend.destroy_buffer_view(HandleRequest::from_body(body)).to_body());
                }
                break;
            case RpcOp::CreateShaderModule: {
                // The only BINARY-bodied op: decode the [u32 json_len][json][SPIR-V] body via
                // from_wire (not parse_body, which expects JSON). A framing fault is BadRequest.
                std::string smerr;
                const CreateShaderModuleRequest creq =
                    CreateShaderModuleRequest::from_wire(req.body, smerr);
                if (!smerr.empty()) {
                    resp.status = static_cast<std::uint32_t>(RpcStatus::BadRequest);
                    channel.send(resp);
                    break;
                }
                const CreateShaderModuleResponse cresp = backend.create_shader_module(creq);
                op_trace.create_shader_module(req.request_id, creq, cresp);
                reply(cresp.to_body());
                break;
            }
            case RpcOp::DestroyShaderModule:
                if (parse_body()) {
                    reply(backend.destroy_shader_module(HandleRequest::from_body(body)).to_body());
                }
                break;
            case RpcOp::CreateRenderPass:
                if (parse_body()) {
                    reply(backend.create_render_pass(CreateRenderPassRequest::from_body(body))
                              .to_body());
                }
                break;
            case RpcOp::DestroyRenderPass:
                if (parse_body()) {
                    reply(backend.destroy_render_pass(HandleRequest::from_body(body)).to_body());
                }
                break;
            case RpcOp::CreateFramebuffer:
                if (parse_body()) {
                    reply(backend.create_framebuffer(CreateFramebufferRequest::from_body(body))
                              .to_body());
                }
                break;
            case RpcOp::DestroyFramebuffer:
                if (parse_body()) {
                    reply(backend.destroy_framebuffer(HandleRequest::from_body(body)).to_body());
                }
                break;
            case RpcOp::CreatePipelineLayout:
                if (parse_body()) {
                    reply(
                        backend.create_pipeline_layout(CreatePipelineLayoutRequest::from_body(body))
                            .to_body());
                }
                break;
            case RpcOp::DestroyPipelineLayout:
                if (parse_body()) {
                    reply(
                        backend.destroy_pipeline_layout(HandleRequest::from_body(body)).to_body());
                }
                break;
            case RpcOp::CreateGraphicsPipelines:
                if (parse_body()) {
                    const CreateGraphicsPipelinesRequest preq =
                        CreateGraphicsPipelinesRequest::from_body(body);
                    const CreateGraphicsPipelinesResponse presp =
                        backend.create_graphics_pipelines(preq);
                    op_trace.create_graphics_pipeline(req.request_id, preq, presp);
                    reply(presp.to_body());
                }
                break;
            case RpcOp::CreateComputePipelines:
                if (parse_body()) {
                    reply(backend
                              .create_compute_pipelines(
                                  CreateComputePipelinesRequest::from_body(body))
                              .to_body());
                }
                break;
            case RpcOp::DestroyPipeline:
                if (parse_body()) {
                    reply(backend.destroy_pipeline(HandleRequest::from_body(body)).to_body());
                }
                break;
            // Host-visible memory + buffers.
            case RpcOp::GetPhysicalDeviceMemoryProperties:
                if (parse_body()) {
                    reply(backend
                              .get_physical_device_memory_properties(
                                  GetPhysicalDeviceMemoryPropertiesRequest::from_body(body))
                              .to_body());
                }
                break;
            case RpcOp::CreateBuffer: {
                if (parse_body()) {
                    const CreateBufferRequest breq = CreateBufferRequest::from_body(body);
                    const CreateBufferResponse bresp = backend.create_buffer(breq);
                    op_trace.create_buffer(req.request_id, breq, bresp);
                    reply(bresp.to_body());
                }
                break;
            }
            case RpcOp::DestroyBuffer:
                if (parse_body()) {
                    reply(backend.destroy_buffer(HandleRequest::from_body(body)).to_body());
                }
                break;
            case RpcOp::BindBufferMemory: {
                if (parse_body()) {
                    const BindBufferMemoryRequest breq = BindBufferMemoryRequest::from_body(body);
                    const StatusResponse bresp = backend.bind_buffer_memory(breq);
                    op_trace.bind_buffer_memory(req.request_id, breq, bresp);
                    reply(bresp.to_body());
                }
                break;
            }
            case RpcOp::GetBufferDeviceAddress:
                if (parse_body()) {
                    reply(backend
                              .get_buffer_device_address(
                                  GetBufferDeviceAddressRequest::from_body(body))
                              .to_body());
                }
                break;
            case RpcOp::WriteMemoryRanges: {
                // BINARY-bodied like create_shader_module: decode the [u32 json_len][json][bytes]
                // via from_wire (not parse_body). A framing fault is BadRequest.
                std::string wmerr;
                const WriteMemoryRangesRequest wreq =
                    WriteMemoryRangesRequest::from_wire(req.body, wmerr);
                if (!wmerr.empty()) {
                    resp.status = static_cast<std::uint32_t>(RpcStatus::BadRequest);
                    channel.send(resp);
                    break;
                }
                op_trace.write_memory_ranges(req.request_id, wreq);
                reply(backend.write_memory_ranges(wreq).to_body());
                break;
            }
            case RpcOp::ReadMemoryRanges: { // host-visible readback
                if (!parse_body()) {
                    break;
                }
                const ReadMemoryRangesRequest rreq = ReadMemoryRangesRequest::from_body(body);
                const ReadMemoryRangesResponse rresp = backend.read_memory_ranges(rreq);
                if (rreq.raw_response) {
                    // the negotiated raw reply -- payload as raw bytes behind a
                    // small JSON header, no hex doubling / multi-MB JSON dump. Only when the
                    // CLIENT asked (an old client gets the JSON+hex body below, unchanged).
                    resp.status = static_cast<std::uint32_t>(RpcStatus::Ok);
                    resp.body = rresp.to_wire();
                    if (prof != nullptr) {
                        prof->record(req.op, resp.body.size(), req.body.size(),
                                     profile_now_us() - iter_t0);
                    }
                    channel.send(resp);
                } else {
                    reply(rresp.to_body()); // JSON in, hex-blob bytes out (pre)
                }
                break;
            }
            // Descriptor surface + per-frame UBO.
            case RpcOp::CreateDescriptorSetLayout:
                if (parse_body()) {
                    reply(backend
                              .create_descriptor_set_layout(
                                  CreateDescriptorSetLayoutRequest::from_body(body))
                              .to_body());
                }
                break;
            case RpcOp::GetDescriptorSetLayoutSupport:
                if (parse_body()) {
                    reply(backend
                              .get_descriptor_set_layout_support(
                                  CreateDescriptorSetLayoutRequest::from_body(body))
                              .to_body());
                }
                break;
            case RpcOp::GetDeviceBufferMemoryRequirements:
                if (parse_body()) {
                    reply(backend
                              .get_device_buffer_memory_requirements(
                                  CreateBufferRequest::from_body(body))
                              .to_body());
                }
                break;
            case RpcOp::GetDeviceImageMemoryRequirements:
                if (parse_body()) {
                    reply(backend
                              .get_device_image_memory_requirements(
                                  CreateImageRequest::from_body(body))
                              .to_body());
                }
                break;
            case RpcOp::DestroyDescriptorSetLayout:
                if (parse_body()) {
                    reply(backend.destroy_descriptor_set_layout(HandleRequest::from_body(body))
                              .to_body());
                }
                break;
            case RpcOp::CreateDescriptorPool:
                if (parse_body()) {
                    reply(
                        backend.create_descriptor_pool(CreateDescriptorPoolRequest::from_body(body))
                            .to_body());
                }
                break;
            case RpcOp::DestroyDescriptorPool:
                if (parse_body()) {
                    reply(
                        backend.destroy_descriptor_pool(HandleRequest::from_body(body)).to_body());
                }
                break;
            case RpcOp::AllocateDescriptorSets:
                if (parse_body()) {
                    reply(backend
                              .allocate_descriptor_sets(
                                  AllocateDescriptorSetsRequest::from_body(body))
                              .to_body());
                }
                break;
            case RpcOp::UpdateDescriptorSets:
                if (parse_body()) {
                    reply(
                        backend.update_descriptor_sets(UpdateDescriptorSetsRequest::from_body(body))
                            .to_body());
                }
                break;
            // Textures + depth for literal vkcube.
            case RpcOp::GetPhysicalDeviceFormatProperties:
                if (parse_body()) {
                    reply(backend
                              .get_physical_device_format_properties(
                                  GetPhysicalDeviceFormatPropertiesRequest::from_body(body))
                              .to_body());
                }
                break;
            // GL/zink: honest feature + image-format-support forwarding.
            case RpcOp::GetPhysicalDeviceFeatures:
                if (parse_body()) {
                    reply(backend
                              .get_physical_device_features(
                                  GetPhysicalDeviceFeaturesRequest::from_body(body))
                              .to_body());
                }
                break;
            case RpcOp::GetPhysicalDeviceImageFormatProperties:
                if (parse_body()) {
                    reply(backend
                              .get_physical_device_image_format_properties(
                                  GetPhysicalDeviceImageFormatPropertiesRequest::from_body(body))
                              .to_body());
                }
                break;
            case RpcOp::GetPhysicalDeviceProperties:
                if (parse_body()) {
                    reply(backend
                              .get_physical_device_properties(
                                  GetPhysicalDevicePropertiesRequest::from_body(body))
                              .to_body());
                }
                break;
            case RpcOp::GetPhysicalDeviceCapabilityChain:
                if (parse_body()) {
                    reply(backend
                              .get_physical_device_capability_chain(
                                  GetPhysicalDeviceCapabilityChainRequest::from_body(body))
                              .to_body());
                }
                break;
            case RpcOp::CreateImage: {
                if (parse_body()) {
                    const CreateImageRequest ireq = CreateImageRequest::from_body(body);
                    const CreateImageResponse iresp = backend.create_image(ireq);
                    op_trace.create_image(req.request_id, ireq, iresp);
                    reply(iresp.to_body());
                }
                break;
            }
            case RpcOp::DestroyImage:
                if (parse_body()) {
                    reply(backend.destroy_image(HandleRequest::from_body(body)).to_body());
                }
                break;
            case RpcOp::BindImageMemory: {
                if (parse_body()) {
                    const BindImageMemoryRequest ireq = BindImageMemoryRequest::from_body(body);
                    const StatusResponse iresp = backend.bind_image_memory(ireq);
                    op_trace.bind_image_memory(req.request_id, ireq, iresp);
                    reply(iresp.to_body());
                }
                break;
            }
            // Sampler support.
            case RpcOp::CreateSampler:
                if (parse_body()) {
                    reply(backend.create_sampler(CreateSamplerRequest::from_body(body)).to_body());
                }
                break;
            case RpcOp::DestroySampler:
                if (parse_body()) {
                    reply(backend.destroy_sampler(HandleRequest::from_body(body)).to_body());
                }
                break;
            // Query pools (GL 3.3 / occlusion / xfb queries).
            case RpcOp::CreateQueryPool:
                if (parse_body()) {
                    reply(backend.create_query_pool(CreateQueryPoolRequest::from_body(body))
                              .to_body());
                }
                break;
            case RpcOp::DestroyQueryPool:
                if (parse_body()) {
                    reply(backend.destroy_query_pool(HandleRequest::from_body(body)).to_body());
                }
                break;
            case RpcOp::GetQueryPoolResults:
                if (parse_body()) {
                    reply(
                        backend.get_query_pool_results(GetQueryPoolResultsRequest::from_body(body))
                            .to_body());
                }
                break;
            case RpcOp::ResetQueryPool:
                if (parse_body()) {
                    reply(
                        backend.reset_query_pool(ResetQueryPoolRequest::from_body(body)).to_body());
                }
                break;
            default:
                resp.status = static_cast<std::uint32_t>(RpcStatus::UnknownOp);
                VKR_WARN(kComponent) << "unknown rpc op " << req.op;
                channel.send(resp);
                break;
            }
        }
    } catch (const std::exception& e) {
        VKR_INFO(kComponent) << "rpc session ended: " << e.what();
    }
    // the worker's session is over (clean EOF or error) -- dump the serve-end profile.
    // One-shot (the atexit backstop is a no-op after this).
    profile_dump_once();
}

CapabilitiesResponse negotiate_capabilities(RpcChannel& channel, std::uint32_t request_id,
                                            const CapabilitiesRequest& req) {
    return CapabilitiesResponse::from_body(
        call_rpc(channel, RpcOp::NegotiateCapabilities, request_id, req.to_body()));
}

CreateInstanceResponse create_instance(RpcChannel& channel, std::uint32_t request_id,
                                       const CreateInstanceRequest& req) {
    return CreateInstanceResponse::from_body(
        call_rpc(channel, RpcOp::CreateInstance, request_id, req.to_body()));
}

EnumeratePhysicalDevicesResponse
enumerate_physical_devices(RpcChannel& channel, std::uint32_t request_id,
                           const EnumeratePhysicalDevicesRequest& req) {
    return EnumeratePhysicalDevicesResponse::from_body(
        call_rpc(channel, RpcOp::EnumeratePhysicalDevices, request_id, req.to_body()));
}

CreateDeviceResponse create_device(RpcChannel& channel, std::uint32_t request_id,
                                   const CreateDeviceRequest& req) {
    return CreateDeviceResponse::from_body(
        call_rpc(channel, RpcOp::CreateDevice, request_id, req.to_body()));
}

StatusResponse destroy_device(RpcChannel& channel, std::uint32_t request_id,
                              const HandleRequest& req) {
    return StatusResponse::from_body(
        call_rpc(channel, RpcOp::DestroyDevice, request_id, req.to_body()));
}

StatusResponse destroy_instance(RpcChannel& channel, std::uint32_t request_id,
                                const HandleRequest& req) {
    const StatusResponse r = StatusResponse::from_body(
        call_rpc(channel, RpcOp::DestroyInstance, request_id, req.to_body()));
    // the clean Vulkan teardown path -- dump the client-end profile now (the atexit
    // backstop covers interrupted runs; both are one-shot).
    profile_dump_once();
    return r;
}

GetDeviceQueueResponse get_device_queue(RpcChannel& channel, std::uint32_t request_id,
                                        const GetDeviceQueueRequest& req) {
    return GetDeviceQueueResponse::from_body(
        call_rpc(channel, RpcOp::GetDeviceQueue, request_id, req.to_body()));
}

CreateCommandPoolResponse create_command_pool(RpcChannel& channel, std::uint32_t request_id,
                                              const CreateCommandPoolRequest& req) {
    return CreateCommandPoolResponse::from_body(
        call_rpc(channel, RpcOp::CreateCommandPool, request_id, req.to_body()));
}

StatusResponse destroy_command_pool(RpcChannel& channel, std::uint32_t request_id,
                                    const HandleRequest& req) {
    return StatusResponse::from_body(
        call_rpc(channel, RpcOp::DestroyCommandPool, request_id, req.to_body()));
}

AllocateCommandBuffersResponse allocate_command_buffers(RpcChannel& channel,
                                                        std::uint32_t request_id,
                                                        const AllocateCommandBuffersRequest& req) {
    return AllocateCommandBuffersResponse::from_body(
        call_rpc(channel, RpcOp::AllocateCommandBuffers, request_id, req.to_body()));
}

StatusResponse free_command_buffers(RpcChannel& channel, std::uint32_t request_id,
                                    const FreeCommandBuffersRequest& req) {
    return StatusResponse::from_body(
        call_rpc(channel, RpcOp::FreeCommandBuffers, request_id, req.to_body()));
}

CreateFenceResponse create_fence(RpcChannel& channel, std::uint32_t request_id,
                                 const CreateFenceRequest& req) {
    return CreateFenceResponse::from_body(
        call_rpc(channel, RpcOp::CreateFence, request_id, req.to_body()));
}

StatusResponse destroy_fence(RpcChannel& channel, std::uint32_t request_id,
                             const HandleRequest& req) {
    return StatusResponse::from_body(
        call_rpc(channel, RpcOp::DestroyFence, request_id, req.to_body()));
}

CreateSemaphoreResponse create_semaphore(RpcChannel& channel, std::uint32_t request_id,
                                         const CreateSemaphoreRequest& req) {
    return CreateSemaphoreResponse::from_body(
        call_rpc(channel, RpcOp::CreateSemaphoreOp, request_id, req.to_body()));
}

StatusResponse destroy_semaphore(RpcChannel& channel, std::uint32_t request_id,
                                 const HandleRequest& req) {
    return StatusResponse::from_body(
        call_rpc(channel, RpcOp::DestroySemaphore, request_id, req.to_body()));
}

AllocateMemoryResponse allocate_memory(RpcChannel& channel, std::uint32_t request_id,
                                       const AllocateMemoryRequest& req) {
    return AllocateMemoryResponse::from_body(
        call_rpc(channel, RpcOp::AllocateMemory, request_id, req.to_body()));
}

StatusResponse free_memory(RpcChannel& channel, std::uint32_t request_id,
                           const HandleRequest& req) {
    return StatusResponse::from_body(
        call_rpc(channel, RpcOp::FreeMemory, request_id, req.to_body()));
}

GetBufferDeviceAddressResponse get_buffer_device_address(RpcChannel& channel,
                                                         std::uint32_t request_id,
                                                         const GetBufferDeviceAddressRequest& req) {
    return GetBufferDeviceAddressResponse::from_body(
        call_rpc(channel, RpcOp::GetBufferDeviceAddress, request_id, req.to_body()));
}

CreateSurfaceResponse create_surface(RpcChannel& channel, std::uint32_t request_id,
                                     const CreateSurfaceRequest& req) {
    return CreateSurfaceResponse::from_body(
        call_rpc(channel, RpcOp::CreateSurface, request_id, req.to_body()));
}

StatusResponse destroy_surface(RpcChannel& channel, std::uint32_t request_id,
                               const HandleRequest& req) {
    return StatusResponse::from_body(
        call_rpc(channel, RpcOp::DestroySurface, request_id, req.to_body()));
}

CreateSwapchainResponse create_swapchain(RpcChannel& channel, std::uint32_t request_id,
                                         const CreateSwapchainRequest& req) {
    return CreateSwapchainResponse::from_body(
        call_rpc(channel, RpcOp::CreateSwapchain, request_id, req.to_body()));
}

StatusResponse destroy_swapchain(RpcChannel& channel, std::uint32_t request_id,
                                 const HandleRequest& req) {
    return StatusResponse::from_body(
        call_rpc(channel, RpcOp::DestroySwapchain, request_id, req.to_body()));
}

GetSwapchainImagesResponse get_swapchain_images(RpcChannel& channel, std::uint32_t request_id,
                                                const GetSwapchainImagesRequest& req) {
    return GetSwapchainImagesResponse::from_body(
        call_rpc(channel, RpcOp::GetSwapchainImages, request_id, req.to_body()));
}

AcquireNextImageResponse acquire_next_image(RpcChannel& channel, std::uint32_t request_id,
                                            const AcquireNextImageRequest& req) {
    return AcquireNextImageResponse::from_body(
        call_rpc(channel, RpcOp::AcquireNextImage, request_id, req.to_body()));
}

QueuePresentResponse queue_present(RpcChannel& channel, std::uint32_t request_id,
                                   const QueuePresentRequest& req) {
    return QueuePresentResponse::from_body(
        call_rpc(channel, RpcOp::QueuePresent, request_id, req.to_body()));
}

StatusResponse record_command_buffer(RpcChannel& channel, std::uint32_t request_id,
                                     const RecordCommandBufferRequest& req) {
    return StatusResponse::from_body(
        call_rpc(channel, RpcOp::RecordCommandBuffer, request_id, req.to_body()));
}

StatusResponse record_command_buffer_raw(RpcChannel& channel, std::uint32_t request_id,
                                         const RecordCommandBufferRequest& req) {
    // binary body (to_wire), JSON response -- callers gate on
    // DeviceCaps.raw_record so an old worker never sees this op.
    return StatusResponse::from_body(
        call_rpc_raw(channel, RpcOp::RecordCommandBufferRaw, request_id, req.to_wire()));
}

QueueSubmitResponse queue_submit(RpcChannel& channel, std::uint32_t request_id,
                                 const QueueSubmitRequest& req) {
    return QueueSubmitResponse::from_body(
        call_rpc(channel, RpcOp::QueueSubmit, request_id, req.to_body()));
}

QueueSubmitResponse queue_submit2(RpcChannel& channel, std::uint32_t request_id,
                                  const QueueSubmit2Request& req) {
    return QueueSubmitResponse::from_body(
        call_rpc(channel, RpcOp::QueueSubmit2, request_id, req.to_body()));
}

StatusResponse reset_fences(RpcChannel& channel, std::uint32_t request_id,
                            const ResetFencesRequest& req) {
    return StatusResponse::from_body(
        call_rpc(channel, RpcOp::ResetFences, request_id, req.to_body()));
}

WaitForFencesResponse wait_for_fences(RpcChannel& channel, std::uint32_t request_id,
                                      const WaitForFencesRequest& req) {
    return WaitForFencesResponse::from_body(
        call_rpc(channel, RpcOp::WaitForFences, request_id, req.to_body()));
}

GetFenceStatusResponse get_fence_status(RpcChannel& channel, std::uint32_t request_id,
                                        const HandleRequest& req) {
    return GetFenceStatusResponse::from_body(
        call_rpc(channel, RpcOp::GetFenceStatus, request_id, req.to_body()));
}

CreateEventResponse create_event(RpcChannel& channel, std::uint32_t request_id,
                                 const CreateEventRequest& req) {
    return CreateEventResponse::from_body(
        call_rpc(channel, RpcOp::CreateEventOp, request_id, req.to_body()));
}

StatusResponse destroy_event(RpcChannel& channel, std::uint32_t request_id,
                             const HandleRequest& req) {
    return StatusResponse::from_body(
        call_rpc(channel, RpcOp::DestroyEvent, request_id, req.to_body()));
}

GetEventStatusResponse get_event_status(RpcChannel& channel, std::uint32_t request_id,
                                        const HandleRequest& req) {
    return GetEventStatusResponse::from_body(
        call_rpc(channel, RpcOp::GetEventStatus, request_id, req.to_body()));
}

StatusResponse set_event(RpcChannel& channel, std::uint32_t request_id, const HandleRequest& req) {
    return StatusResponse::from_body(call_rpc(channel, RpcOp::SetEvent, request_id, req.to_body()));
}

StatusResponse reset_event(RpcChannel& channel, std::uint32_t request_id,
                           const HandleRequest& req) {
    return StatusResponse::from_body(
        call_rpc(channel, RpcOp::ResetEvent, request_id, req.to_body()));
}

WaitSemaphoresResponse wait_semaphores(RpcChannel& channel, std::uint32_t request_id,
                                       const WaitSemaphoresRequest& req) {
    return WaitSemaphoresResponse::from_body(
        call_rpc(channel, RpcOp::WaitSemaphores, request_id, req.to_body()));
}

WaitIdleResponse queue_wait_idle(RpcChannel& channel, std::uint32_t request_id,
                                 const HandleRequest& req) {
    return WaitIdleResponse::from_body(
        call_rpc(channel, RpcOp::QueueWaitIdle, request_id, req.to_body()));
}

WaitIdleResponse device_wait_idle(RpcChannel& channel, std::uint32_t request_id,
                                  const HandleRequest& req) {
    return WaitIdleResponse::from_body(
        call_rpc(channel, RpcOp::DeviceWaitIdle, request_id, req.to_body()));
}

StatusResponse signal_semaphore(RpcChannel& channel, std::uint32_t request_id,
                                const SignalSemaphoreRequest& req) {
    return StatusResponse::from_body(
        call_rpc(channel, RpcOp::SignalSemaphore, request_id, req.to_body()));
}

GetSemaphoreCounterValueResponse
get_semaphore_counter_value(RpcChannel& channel, std::uint32_t request_id,
                            const GetSemaphoreCounterValueRequest& req) {
    return GetSemaphoreCounterValueResponse::from_body(
        call_rpc(channel, RpcOp::GetSemaphoreCounterValue, request_id, req.to_body()));
}

GetSurfaceCapabilitiesResponse get_surface_capabilities(RpcChannel& channel,
                                                        std::uint32_t request_id,
                                                        const GetSurfaceCapabilitiesRequest& req) {
    return GetSurfaceCapabilitiesResponse::from_body(
        call_rpc(channel, RpcOp::GetSurfaceCapabilities, request_id, req.to_body()));
}

GetSurfaceFormatsResponse get_surface_formats(RpcChannel& channel, std::uint32_t request_id,
                                              const GetSurfaceFormatsRequest& req) {
    return GetSurfaceFormatsResponse::from_body(
        call_rpc(channel, RpcOp::GetSurfaceFormats, request_id, req.to_body()));
}

GetSurfacePresentModesResponse get_surface_present_modes(RpcChannel& channel,
                                                         std::uint32_t request_id,
                                                         const GetSurfacePresentModesRequest& req) {
    return GetSurfacePresentModesResponse::from_body(
        call_rpc(channel, RpcOp::GetSurfacePresentModes, request_id, req.to_body()));
}

GetSurfaceSupportResponse get_surface_support(RpcChannel& channel, std::uint32_t request_id,
                                              const GetSurfaceSupportRequest& req) {
    return GetSurfaceSupportResponse::from_body(
        call_rpc(channel, RpcOp::GetSurfaceSupport, request_id, req.to_body()));
}

// --- Draw surface client helpers ------------------------

CreateImageViewResponse create_image_view(RpcChannel& channel, std::uint32_t request_id,
                                          const CreateImageViewRequest& req) {
    return CreateImageViewResponse::from_body(
        call_rpc(channel, RpcOp::CreateImageView, request_id, req.to_body()));
}

StatusResponse destroy_image_view(RpcChannel& channel, std::uint32_t request_id,
                                  const HandleRequest& req) {
    return StatusResponse::from_body(
        call_rpc(channel, RpcOp::DestroyImageView, request_id, req.to_body()));
}

CreateBufferViewResponse create_buffer_view(RpcChannel& channel, std::uint32_t request_id,
                                            const CreateBufferViewRequest& req) {
    return CreateBufferViewResponse::from_body(
        call_rpc(channel, RpcOp::CreateBufferView, request_id, req.to_body()));
}

StatusResponse destroy_buffer_view(RpcChannel& channel, std::uint32_t request_id,
                                   const HandleRequest& req) {
    return StatusResponse::from_body(
        call_rpc(channel, RpcOp::DestroyBufferView, request_id, req.to_body()));
}

CreateShaderModuleResponse create_shader_module(RpcChannel& channel, std::uint32_t request_id,
                                                const CreateShaderModuleRequest& req) {
    // Binary body: the request rides as raw [u32 json_len][json][SPIR-V] bytes, not a JSON
    // document.
    return CreateShaderModuleResponse::from_body(
        call_rpc_raw(channel, RpcOp::CreateShaderModule, request_id, req.to_wire()));
}

StatusResponse destroy_shader_module(RpcChannel& channel, std::uint32_t request_id,
                                     const HandleRequest& req) {
    return StatusResponse::from_body(
        call_rpc(channel, RpcOp::DestroyShaderModule, request_id, req.to_body()));
}

CreateRenderPassResponse create_render_pass(RpcChannel& channel, std::uint32_t request_id,
                                            const CreateRenderPassRequest& req) {
    return CreateRenderPassResponse::from_body(
        call_rpc(channel, RpcOp::CreateRenderPass, request_id, req.to_body()));
}

StatusResponse destroy_render_pass(RpcChannel& channel, std::uint32_t request_id,
                                   const HandleRequest& req) {
    return StatusResponse::from_body(
        call_rpc(channel, RpcOp::DestroyRenderPass, request_id, req.to_body()));
}

CreateFramebufferResponse create_framebuffer(RpcChannel& channel, std::uint32_t request_id,
                                             const CreateFramebufferRequest& req) {
    return CreateFramebufferResponse::from_body(
        call_rpc(channel, RpcOp::CreateFramebuffer, request_id, req.to_body()));
}

StatusResponse destroy_framebuffer(RpcChannel& channel, std::uint32_t request_id,
                                   const HandleRequest& req) {
    return StatusResponse::from_body(
        call_rpc(channel, RpcOp::DestroyFramebuffer, request_id, req.to_body()));
}

CreatePipelineLayoutResponse create_pipeline_layout(RpcChannel& channel, std::uint32_t request_id,
                                                    const CreatePipelineLayoutRequest& req) {
    return CreatePipelineLayoutResponse::from_body(
        call_rpc(channel, RpcOp::CreatePipelineLayout, request_id, req.to_body()));
}

StatusResponse destroy_pipeline_layout(RpcChannel& channel, std::uint32_t request_id,
                                       const HandleRequest& req) {
    return StatusResponse::from_body(
        call_rpc(channel, RpcOp::DestroyPipelineLayout, request_id, req.to_body()));
}

CreateGraphicsPipelinesResponse
create_graphics_pipelines(RpcChannel& channel, std::uint32_t request_id,
                          const CreateGraphicsPipelinesRequest& req) {
    return CreateGraphicsPipelinesResponse::from_body(
        call_rpc(channel, RpcOp::CreateGraphicsPipelines, request_id, req.to_body()));
}

CreateComputePipelinesResponse create_compute_pipelines(RpcChannel& channel,
                                                        std::uint32_t request_id,
                                                        const CreateComputePipelinesRequest& req) {
    return CreateComputePipelinesResponse::from_body(
        call_rpc(channel, RpcOp::CreateComputePipelines, request_id, req.to_body()));
}

StatusResponse destroy_pipeline(RpcChannel& channel, std::uint32_t request_id,
                                const HandleRequest& req) {
    return StatusResponse::from_body(
        call_rpc(channel, RpcOp::DestroyPipeline, request_id, req.to_body()));
}

// --- Host-visible memory + buffer client helpers ------

GetPhysicalDeviceMemoryPropertiesResponse
get_physical_device_memory_properties(RpcChannel& channel, std::uint32_t request_id,
                                      const GetPhysicalDeviceMemoryPropertiesRequest& req) {
    return GetPhysicalDeviceMemoryPropertiesResponse::from_body(
        call_rpc(channel, RpcOp::GetPhysicalDeviceMemoryProperties, request_id, req.to_body()));
}

CreateBufferResponse create_buffer(RpcChannel& channel, std::uint32_t request_id,
                                   const CreateBufferRequest& req) {
    return CreateBufferResponse::from_body(
        call_rpc(channel, RpcOp::CreateBuffer, request_id, req.to_body()));
}

StatusResponse destroy_buffer(RpcChannel& channel, std::uint32_t request_id,
                              const HandleRequest& req) {
    return StatusResponse::from_body(
        call_rpc(channel, RpcOp::DestroyBuffer, request_id, req.to_body()));
}

StatusResponse bind_buffer_memory(RpcChannel& channel, std::uint32_t request_id,
                                  const BindBufferMemoryRequest& req) {
    return StatusResponse::from_body(
        call_rpc(channel, RpcOp::BindBufferMemory, request_id, req.to_body()));
}

StatusResponse write_memory_ranges(RpcChannel& channel, std::uint32_t request_id,
                                   const WriteMemoryRangesRequest& req) {
    // Binary body: the request rides as raw [u32 json_len][json][bytes], not a JSON document.
    return StatusResponse::from_body(
        call_rpc_raw(channel, RpcOp::WriteMemoryRanges, request_id, req.to_wire()));
}

ReadMemoryRangesResponse read_memory_ranges(RpcChannel& channel, std::uint32_t request_id,
                                            const ReadMemoryRangesRequest& req) {
    if (req.raw_response) {
        // the worker replies raw ([len][{ok,reason}][payload]) because we asked;
        // a malformed frame fails CLOSED (ok=false + the decode error as the reason).
        std::string err;
        ReadMemoryRangesResponse r = ReadMemoryRangesResponse::from_wire(
            call_rpc_response_raw(channel, RpcOp::ReadMemoryRanges, request_id, req.to_body()),
            err);
        if (!err.empty()) {
            r.ok = false;
            r.reason = err;
        }
        return r;
    }
    return ReadMemoryRangesResponse::from_body(
        call_rpc(channel, RpcOp::ReadMemoryRanges, request_id, req.to_body()));
}

// --- Descriptor surface client helpers ----------------

CreateDescriptorSetLayoutResponse
create_descriptor_set_layout(RpcChannel& channel, std::uint32_t request_id,
                             const CreateDescriptorSetLayoutRequest& req) {
    return CreateDescriptorSetLayoutResponse::from_body(
        call_rpc(channel, RpcOp::CreateDescriptorSetLayout, request_id, req.to_body()));
}

GetDescriptorSetLayoutSupportResponse
get_descriptor_set_layout_support(RpcChannel& channel, std::uint32_t request_id,
                                  const CreateDescriptorSetLayoutRequest& req) {
    return GetDescriptorSetLayoutSupportResponse::from_body(
        call_rpc(channel, RpcOp::GetDescriptorSetLayoutSupport, request_id, req.to_body()));
}

CreateBufferResponse get_device_buffer_memory_requirements(RpcChannel& channel,
                                                           std::uint32_t request_id,
                                                           const CreateBufferRequest& req) {
    return CreateBufferResponse::from_body(
        call_rpc(channel, RpcOp::GetDeviceBufferMemoryRequirements, request_id, req.to_body()));
}

CreateImageResponse get_device_image_memory_requirements(RpcChannel& channel,
                                                         std::uint32_t request_id,
                                                         const CreateImageRequest& req) {
    return CreateImageResponse::from_body(
        call_rpc(channel, RpcOp::GetDeviceImageMemoryRequirements, request_id, req.to_body()));
}

StatusResponse destroy_descriptor_set_layout(RpcChannel& channel, std::uint32_t request_id,
                                             const HandleRequest& req) {
    return StatusResponse::from_body(
        call_rpc(channel, RpcOp::DestroyDescriptorSetLayout, request_id, req.to_body()));
}

CreateDescriptorPoolResponse create_descriptor_pool(RpcChannel& channel, std::uint32_t request_id,
                                                    const CreateDescriptorPoolRequest& req) {
    return CreateDescriptorPoolResponse::from_body(
        call_rpc(channel, RpcOp::CreateDescriptorPool, request_id, req.to_body()));
}

StatusResponse destroy_descriptor_pool(RpcChannel& channel, std::uint32_t request_id,
                                       const HandleRequest& req) {
    return StatusResponse::from_body(
        call_rpc(channel, RpcOp::DestroyDescriptorPool, request_id, req.to_body()));
}

AllocateDescriptorSetsResponse allocate_descriptor_sets(RpcChannel& channel,
                                                        std::uint32_t request_id,
                                                        const AllocateDescriptorSetsRequest& req) {
    return AllocateDescriptorSetsResponse::from_body(
        call_rpc(channel, RpcOp::AllocateDescriptorSets, request_id, req.to_body()));
}

StatusResponse update_descriptor_sets(RpcChannel& channel, std::uint32_t request_id,
                                      const UpdateDescriptorSetsRequest& req) {
    return StatusResponse::from_body(
        call_rpc(channel, RpcOp::UpdateDescriptorSets, request_id, req.to_body()));
}

// --- Textures + depth client helpers  ---------

GetPhysicalDeviceFormatPropertiesResponse
get_physical_device_format_properties(RpcChannel& channel, std::uint32_t request_id,
                                      const GetPhysicalDeviceFormatPropertiesRequest& req) {
    return GetPhysicalDeviceFormatPropertiesResponse::from_body(
        call_rpc(channel, RpcOp::GetPhysicalDeviceFormatProperties, request_id, req.to_body()));
}

GetPhysicalDeviceFeaturesResponse
get_physical_device_features(RpcChannel& channel, std::uint32_t request_id,
                             const GetPhysicalDeviceFeaturesRequest& req) {
    return GetPhysicalDeviceFeaturesResponse::from_body(
        call_rpc(channel, RpcOp::GetPhysicalDeviceFeatures, request_id, req.to_body()));
}

GetPhysicalDeviceImageFormatPropertiesResponse get_physical_device_image_format_properties(
    RpcChannel& channel, std::uint32_t request_id,
    const GetPhysicalDeviceImageFormatPropertiesRequest& req) {
    return GetPhysicalDeviceImageFormatPropertiesResponse::from_body(call_rpc(
        channel, RpcOp::GetPhysicalDeviceImageFormatProperties, request_id, req.to_body()));
}

GetPhysicalDevicePropertiesResponse
get_physical_device_properties(RpcChannel& channel, std::uint32_t request_id,
                               const GetPhysicalDevicePropertiesRequest& req) {
    return GetPhysicalDevicePropertiesResponse::from_body(
        call_rpc(channel, RpcOp::GetPhysicalDeviceProperties, request_id, req.to_body()));
}

GetPhysicalDeviceCapabilityChainResponse
get_physical_device_capability_chain(RpcChannel& channel, std::uint32_t request_id,
                                     const GetPhysicalDeviceCapabilityChainRequest& req) {
    return GetPhysicalDeviceCapabilityChainResponse::from_body(
        call_rpc(channel, RpcOp::GetPhysicalDeviceCapabilityChain, request_id, req.to_body()));
}

CreateImageResponse create_image(RpcChannel& channel, std::uint32_t request_id,
                                 const CreateImageRequest& req) {
    return CreateImageResponse::from_body(
        call_rpc(channel, RpcOp::CreateImage, request_id, req.to_body()));
}

StatusResponse destroy_image(RpcChannel& channel, std::uint32_t request_id,
                             const HandleRequest& req) {
    return StatusResponse::from_body(
        call_rpc(channel, RpcOp::DestroyImage, request_id, req.to_body()));
}

StatusResponse bind_image_memory(RpcChannel& channel, std::uint32_t request_id,
                                 const BindImageMemoryRequest& req) {
    return StatusResponse::from_body(
        call_rpc(channel, RpcOp::BindImageMemory, request_id, req.to_body()));
}

CreateSamplerResponse create_sampler(RpcChannel& channel, std::uint32_t request_id,
                                     const CreateSamplerRequest& req) {
    return CreateSamplerResponse::from_body(
        call_rpc(channel, RpcOp::CreateSampler, request_id, req.to_body()));
}

StatusResponse destroy_sampler(RpcChannel& channel, std::uint32_t request_id,
                               const HandleRequest& req) {
    return StatusResponse::from_body(
        call_rpc(channel, RpcOp::DestroySampler, request_id, req.to_body()));
}

CreateQueryPoolResponse create_query_pool(RpcChannel& channel, std::uint32_t request_id,
                                          const CreateQueryPoolRequest& req) {
    return CreateQueryPoolResponse::from_body(
        call_rpc(channel, RpcOp::CreateQueryPool, request_id, req.to_body()));
}

StatusResponse destroy_query_pool(RpcChannel& channel, std::uint32_t request_id,
                                  const HandleRequest& req) {
    return StatusResponse::from_body(
        call_rpc(channel, RpcOp::DestroyQueryPool, request_id, req.to_body()));
}

StatusResponse reset_query_pool(RpcChannel& channel, std::uint32_t request_id,
                                const ResetQueryPoolRequest& req) {
    return StatusResponse::from_body(
        call_rpc(channel, RpcOp::ResetQueryPool, request_id, req.to_body()));
}

GetQueryPoolResultsResponse get_query_pool_results(RpcChannel& channel, std::uint32_t request_id,
                                                   const GetQueryPoolResultsRequest& req) {
    return GetQueryPoolResultsResponse::from_body(
        call_rpc(channel, RpcOp::GetQueryPoolResults, request_id, req.to_body()));
}

} // namespace vkr::vkrpc
