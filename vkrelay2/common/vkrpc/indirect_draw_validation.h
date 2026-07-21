// Shared structural validation for the core indirect-draw family, including the Vulkan-1.2/KHR
// count-sourced variants.
//
// This header deliberately has no Vulkan dependency: the Linux ICD, mock backend, and real
// backend call the same predicate, so malformed or version-skewed streams fail identically before
// a host command is emitted.
#ifndef VKRELAY2_COMMON_VKRPC_INDIRECT_DRAW_VALIDATION_H
#define VKRELAY2_COMMON_VKRPC_INDIRECT_DRAW_VALIDATION_H

#include <cstdint>
#include <limits>

namespace vkr::vkrpc {

constexpr std::uint64_t kDrawIndirectCommandBytes = 16;
constexpr std::uint64_t kDrawIndexedIndirectCommandBytes = 20;

// VkPhysicalDeviceFeatures::multiDrawIndirect is bit 9 in the frozen core-1.0 feature struct; see
// feature_bits.h's canonical X-macro declaration order. Kept here because the mock wire layer does
// not include Vulkan headers.
constexpr std::uint64_t kFeatureMultiDrawIndirect = std::uint64_t{1} << 9;

struct IndirectBufferState {
    bool live = false;
    bool bound = false;
    bool has_indirect_usage = false;
    std::uint64_t size = 0;
};

// The mock and real object tables deliberately expose the same four buffer members. Centralizing
// this lookup keeps bind/indirect admission in lockstep without coupling this Vulkan-free header
// to either backend's private record type.
template <typename BufferMap, typename Device>
const typename BufferMap::mapped_type*
live_device_buffer(const BufferMap& buffers, std::uint64_t handle, const Device& device) {
    const auto it = buffers.find(handle);
    return it != buffers.end() && it->second.device == device ? &it->second : nullptr;
}

template <typename BufferMap, typename Device, typename Usage>
IndirectBufferState indirect_buffer_state(const BufferMap& buffers, std::uint64_t handle,
                                          const Device& device, Usage indirect_usage) {
    const auto* buffer = live_device_buffer(buffers, handle, device);
    IndirectBufferState state;
    state.live = buffer != nullptr;
    state.bound = buffer != nullptr && buffer->bound_memory != 0;
    state.has_indirect_usage =
        buffer != nullptr &&
        (buffer->usage & static_cast<decltype(buffer->usage)>(indirect_usage)) != 0;
    state.size = buffer != nullptr ? static_cast<std::uint64_t>(buffer->size) : 0;
    return state;
}

inline bool dispatch_indirect_ok(const IndirectBufferState& buffer, std::uint64_t offset,
                                 const char** reason) {
    if (!buffer.live || !buffer.bound) {
        *reason = "dispatch_indirect buffer is not live/bound on the device";
        return false;
    }
    if (!buffer.has_indirect_usage) {
        *reason = "dispatch_indirect buffer lacks INDIRECT_BUFFER usage";
        return false;
    }
    if ((offset % 4) != 0 || offset > buffer.size || 12 > buffer.size - offset) {
        *reason = "dispatch_indirect offset misaligned or out of range";
        return false;
    }
    return true;
}

inline bool core_indirect_draw_ok(const IndirectBufferState& buffer, std::uint64_t offset,
                                  long long draw_count, long long stride,
                                  std::uint64_t command_size, bool multi_draw_indirect_enabled,
                                  const char** reason);

inline bool core_indirect_draw_ok(bool buffer_live, bool buffer_bound, bool has_indirect_usage,
                                  std::uint64_t buffer_size, std::uint64_t offset,
                                  long long draw_count, long long stride,
                                  std::uint64_t command_size, bool multi_draw_indirect_enabled,
                                  const char** reason) {
    if (!buffer_live) {
        *reason = "indirect draw buffer is not live on the device";
        return false;
    }
    if (!buffer_bound) {
        *reason = "indirect draw buffer is not bound to memory";
        return false;
    }
    if (!has_indirect_usage) {
        *reason = "indirect draw buffer lacks INDIRECT_BUFFER usage";
        return false;
    }
    if (draw_count < 0 ||
        static_cast<unsigned long long>(draw_count) > std::numeric_limits<std::uint32_t>::max()) {
        *reason = "indirect drawCount is outside the uint32 range";
        return false;
    }
    if (stride < 0 ||
        static_cast<unsigned long long>(stride) > std::numeric_limits<std::uint32_t>::max()) {
        *reason = "indirect draw stride is outside the uint32 range";
        return false;
    }
    if (draw_count > 1 && !multi_draw_indirect_enabled) {
        *reason = "indirect drawCount > 1 requires the enabled multiDrawIndirect feature";
        return false;
    }
    if ((offset % 4) != 0) {
        *reason = "indirect draw offset is not 4-byte aligned";
        return false;
    }
    if (draw_count == 0) {
        return true; // no parameter bytes are accessed; offset alignment still applies
    }
    const std::uint64_t ustride = static_cast<std::uint64_t>(stride);
    const std::uint64_t ucount = static_cast<std::uint64_t>(draw_count);
    if (draw_count > 1 && ((ustride % 4) != 0 || ustride < command_size)) {
        *reason = "multi-draw indirect stride is misaligned or smaller than the command";
        return false;
    }

    // end = offset + (drawCount - 1) * stride + sizeof(command), with every operation proven
    // before it is performed. This is the exact byte range the host command may read.
    const std::uint64_t repeats = ucount - 1;
    if (repeats != 0 && ustride > (std::numeric_limits<std::uint64_t>::max() - offset) / repeats) {
        *reason = "indirect draw range overflows";
        return false;
    }
    const std::uint64_t last = offset + repeats * ustride;
    if (command_size > std::numeric_limits<std::uint64_t>::max() - last) {
        *reason = "indirect draw range overflows";
        return false;
    }
    if (last + command_size > buffer_size) {
        *reason = "indirect draw range exceeds the buffer";
        return false;
    }
    return true;
}

inline bool core_indirect_count_draw_ok(bool command_enabled, const IndirectBufferState& buffer,
                                        const IndirectBufferState& count_buffer,
                                        std::uint64_t offset, std::uint64_t count_buffer_offset,
                                        long long max_draw_count, long long stride,
                                        std::uint64_t command_size, const char** reason) {
    if (!command_enabled) {
        *reason = "indirect-count draw requires the enabled core feature or KHR extension";
        return false;
    }
    if (!buffer.live) {
        *reason = "indirect-count draw buffer is not live on the device";
        return false;
    }
    if (!buffer.bound) {
        *reason = "indirect-count draw buffer is not bound to memory";
        return false;
    }
    if (!buffer.has_indirect_usage) {
        *reason = "indirect-count draw buffer lacks INDIRECT_BUFFER usage";
        return false;
    }
    if (!count_buffer.live) {
        *reason = "indirect-count count buffer is not live on the device";
        return false;
    }
    if (!count_buffer.bound) {
        *reason = "indirect-count count buffer is not bound to memory";
        return false;
    }
    if (!count_buffer.has_indirect_usage) {
        *reason = "indirect-count count buffer lacks INDIRECT_BUFFER usage";
        return false;
    }
    if ((offset % 4) != 0) {
        *reason = "indirect-count draw offset is not 4-byte aligned";
        return false;
    }
    if ((count_buffer_offset % 4) != 0) {
        *reason = "indirect-count countBufferOffset is not 4-byte aligned";
        return false;
    }
    if (count_buffer_offset > count_buffer.size || 4 > count_buffer.size - count_buffer_offset) {
        *reason = "indirect-count four-byte count slot exceeds the count buffer";
        return false;
    }
    if (max_draw_count < 0 || static_cast<unsigned long long>(max_draw_count) >
                                  std::numeric_limits<std::uint32_t>::max()) {
        *reason = "indirect-count maxDrawCount is outside the uint32 range";
        return false;
    }
    if (stride < 0 ||
        static_cast<unsigned long long>(stride) > std::numeric_limits<std::uint32_t>::max()) {
        *reason = "indirect-count stride is outside the uint32 range";
        return false;
    }
    const std::uint64_t ustride = static_cast<std::uint64_t>(stride);
    if ((ustride % 4) != 0 || ustride < command_size) {
        *reason = "indirect-count stride is misaligned or smaller than the command";
        return false;
    }
    if (max_draw_count == 0) {
        return true;
    }

    const std::uint64_t repeats = static_cast<std::uint64_t>(max_draw_count) - 1;
    if (repeats != 0 && ustride > (std::numeric_limits<std::uint64_t>::max() - offset) / repeats) {
        *reason = "indirect-count draw range overflows";
        return false;
    }
    const std::uint64_t last = offset + repeats * ustride;
    if (command_size > std::numeric_limits<std::uint64_t>::max() - last) {
        *reason = "indirect-count draw range overflows";
        return false;
    }
    if (last + command_size > buffer.size) {
        *reason = "indirect-count draw range exceeds the buffer";
        return false;
    }
    return true;
}

inline bool core_indirect_draw_ok(const IndirectBufferState& buffer, std::uint64_t offset,
                                  long long draw_count, long long stride,
                                  std::uint64_t command_size, bool multi_draw_indirect_enabled,
                                  const char** reason) {
    return core_indirect_draw_ok(buffer.live, buffer.bound, buffer.has_indirect_usage, buffer.size,
                                 offset, draw_count, stride, command_size,
                                 multi_draw_indirect_enabled, reason);
}

} // namespace vkr::vkrpc

#endif // VKRELAY2_COMMON_VKRPC_INDIRECT_DRAW_VALIDATION_H
