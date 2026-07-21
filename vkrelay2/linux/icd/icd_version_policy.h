// vkrelay2 ICD version / native-lane policy (the Vulkan-1.3 opener).
//
// PURE, dependency-free predicates the ICD uses to decide, per PROCESS, whether it serves the
// steering-safe NATIVE lane (where 1.3-family surfaces may be exposed) or the DEFAULT lane
// (zink / GL, preserving the legacy behavior). Kept here -- not in the ICD binary -- so
// the decision is unit-tested directly (the icd_subset.h pure-predicate precedent). The env READ
// stays in the ICD (a cached getenv); these decide on the already-read value.
//
// The native lane is a launcher-owned MARKER (VKRELAY2_NATIVE_LANE=1): the launcher's native
// frontend sets it and ACTIVELY neutralizes it for the zink/GL modes, so a contaminated parent
// shell can never uncap a zink run. Zink's steering is therefore unbreakable BY CONSTRUCTION -- the
// uncap is impossible on the default lane regardless of what zink itself requests. A lane MARKER
// (not a version literal): Vulkan 1.3 support flips the reported apiVersion to 1.3, but until then
// the lane only exposes 1.3-family surfaces as extensions on a device still reported as 1.2
// (honest).
#ifndef VKRELAY2_LINUX_ICD_ICD_VERSION_POLICY_H
#define VKRELAY2_LINUX_ICD_ICD_VERSION_POLICY_H

#include <cstring>

namespace vkr::icd_policy {

// Is the native (1.3-family-capable) lane selected for this process? ONLY the exact marker "1"
// enables it; anything else (nullptr / "" / "0" / any other string) is the DEFAULT zink-safe lane.
// Strict equality keeps a stray/garbage value from silently uncapping the steering.
inline bool native_lane_enabled(const char* env_value) {
    return env_value != nullptr && std::strcmp(env_value, "1") == 0;
}

// Indirect-count support separates protocol vocabulary from the host's core feature and optional
// KHR alias name. Older allowlist entries treat an empty host-extension list as the established
// old-worker/mock "unknown" case. This new extension deliberately does not: its advertisement is
// host-list-intersected, so both the additive vocabulary bit and a concrete host-list hit are
// required. That also keeps advertised-KHR => reported-f12-feature true for every peer shape.
inline bool indirect_count_extension_advertised(bool worker_support, bool host_extension_list_empty,
                                                bool host_has_extension) {
    return worker_support && !host_extension_list_empty && host_has_extension;
}

inline bool indirect_count_feature_reported(bool worker_support, bool host_feature) {
    return worker_support && host_feature;
}

inline bool indirect_count_device_enabled(bool extension_enabled, bool feature_enabled) {
    return extension_enabled || feature_enabled;
}

inline bool indirect_count_khr_proc_available(bool extension_enabled) {
    return extension_enabled;
}

inline bool indirect_count_core_proc_name(const char* name) {
    return name != nullptr && (std::strcmp(name, "vkCmdDrawIndirectCount") == 0 ||
                               std::strcmp(name, "vkCmdDrawIndexedIndirectCount") == 0);
}

inline bool indirect_count_khr_proc_name(const char* name) {
    return name != nullptr && (std::strcmp(name, "vkCmdDrawIndirectCountKHR") == 0 ||
                               std::strcmp(name, "vkCmdDrawIndexedIndirectCountKHR") == 0);
}

} // namespace vkr::icd_policy

#endif // VKRELAY2_LINUX_ICD_ICD_VERSION_POLICY_H
