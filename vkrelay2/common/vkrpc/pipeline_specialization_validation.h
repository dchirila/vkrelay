// Shared, Vulkan-free validation surface for graphics/compute pipeline specialization constants.
// The Linux ICD, raw decoder, mock backend, and real backend all use the same normalized shape and
// predicate so malformed or version-skewed requests fail identically before a host call.
#ifndef VKRELAY2_COMMON_VKRPC_PIPELINE_SPECIALIZATION_VALIDATION_H
#define VKRELAY2_COMMON_VKRPC_PIPELINE_SPECIALIZATION_VALIDATION_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace vkr::vkrpc {

constexpr std::size_t kMaxSpecializationMapEntriesPerStage = 256;
constexpr std::size_t kMaxSpecializationMapEntriesPerRequest = 512;
constexpr std::size_t kMaxSpecializationDataBytesPerStage = 64u * 1024u;
constexpr std::size_t kMaxSpecializationDataBytesPerGraphicsRequest = 5u * 64u * 1024u;
constexpr std::size_t kMaxPipelineEntryPointNameBytes = 4096;

struct SpecializationMapEntryDesc {
    long long constant_id = -1;
    long long offset = -1;
    long long size = -1;
};

// `present` distinguishes a null pSpecializationInfo from a legal non-null empty struct. Wide
// signed entry fields retain hostile negative / over-u32 wire values until validation proves every
// narrowing cast.
struct SpecializationInfoDesc {
    bool present = false;
    std::vector<SpecializationMapEntryDesc> map_entries;
    std::string data;
};

// The input order is shader-stage order. This validates transport shape and VkSpecializationInfo
// bounds only; the host driver owns SPIR-V id/width checks.
inline bool pipeline_specialization_ok(const std::vector<const SpecializationInfoDesc*>& infos,
                                       std::size_t max_request_data_bytes, std::string& reason) {
    reason.clear();
    std::size_t total_entries = 0;
    std::size_t total_data = 0;
    for (const SpecializationInfoDesc* info : infos) {
        if (info == nullptr) {
            reason = "pipeline specialization descriptor is null";
            return false;
        }
        if (!info->present && (!info->map_entries.empty() || !info->data.empty())) {
            reason = "absent pipeline specialization carries entries or data";
            return false;
        }
        if (info->map_entries.size() > kMaxSpecializationMapEntriesPerStage) {
            reason = "pipeline specialization map entry count exceeds per-stage cap";
            return false;
        }
        if (info->data.size() > kMaxSpecializationDataBytesPerStage) {
            reason = "pipeline specialization data size exceeds per-stage cap";
            return false;
        }
        if (total_entries > std::numeric_limits<std::size_t>::max() - info->map_entries.size() ||
            total_data > std::numeric_limits<std::size_t>::max() - info->data.size()) {
            reason = "pipeline specialization request totals overflow";
            return false;
        }
        total_entries += info->map_entries.size();
        total_data += info->data.size();
    }
    if (total_entries > kMaxSpecializationMapEntriesPerRequest) {
        reason = "pipeline specialization map entry count exceeds request cap";
        return false;
    }
    if (total_data > max_request_data_bytes) {
        reason = "pipeline specialization data size exceeds request cap";
        return false;
    }

    for (const SpecializationInfoDesc* info : infos) {
        for (const SpecializationMapEntryDesc& entry : info->map_entries) {
            if (entry.constant_id < 0 || static_cast<unsigned long long>(entry.constant_id) >
                                             std::numeric_limits<std::uint32_t>::max()) {
                reason = "pipeline specialization constantID is outside uint32";
                return false;
            }
            if (entry.offset < 0 || static_cast<unsigned long long>(entry.offset) >
                                        std::numeric_limits<std::uint32_t>::max()) {
                reason = "pipeline specialization offset is outside uint32";
                return false;
            }
            if (entry.size < 0) {
                reason = "pipeline specialization size is negative or not representable";
                return false;
            }
            if constexpr (sizeof(std::size_t) < sizeof(unsigned long long)) {
                if (static_cast<unsigned long long>(entry.size) >
                    std::numeric_limits<std::size_t>::max()) {
                    reason = "pipeline specialization size is negative or not representable";
                    return false;
                }
            }
            const std::size_t offset = static_cast<std::size_t>(entry.offset);
            const std::size_t size = static_cast<std::size_t>(entry.size);
            if (offset >= info->data.size()) {
                reason = "pipeline specialization offset must be less than dataSize";
                return false;
            }
            if (size > info->data.size() - offset) {
                reason = "pipeline specialization size exceeds dataSize minus offset";
                return false;
            }
        }
        std::vector<long long> ids;
        ids.reserve(info->map_entries.size());
        for (const SpecializationMapEntryDesc& entry : info->map_entries) {
            ids.push_back(entry.constant_id);
        }
        std::sort(ids.begin(), ids.end());
        if (std::adjacent_find(ids.begin(), ids.end()) != ids.end()) {
            reason = "pipeline specialization constantID values must be unique";
            return false;
        }
    }
    return true;
}

inline bool pipeline_entry_point_name_ok(const std::string& name, std::string& reason) {
    reason.clear();
    if (name.size() > kMaxPipelineEntryPointNameBytes) {
        reason = "pipeline stage entry-point name exceeds 4096-byte cap";
        return false;
    }
    return true;
}

} // namespace vkr::vkrpc

#endif // VKRELAY2_COMMON_VKRPC_PIPELINE_SPECIALIZATION_VALIDATION_H
