// Shared structural validation for core vkCmdDrawIndirect / vkCmdDrawIndexedIndirect.
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

} // namespace vkr::vkrpc

#endif // VKRELAY2_COMMON_VKRPC_INDIRECT_DRAW_VALIDATION_H
