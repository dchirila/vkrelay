#ifndef VKRELAY2_COMMON_VKRPC_DEVICE_LOSS_POLICY_H
#define VKRELAY2_COMMON_VKRPC_DEVICE_LOSS_POLICY_H

#include <cstring>

namespace vkr::vkrpc {

// Worker-side diagnostic experiment. Only the exact value "1" opts in; an absent, empty, or
// otherwise-valued variable keeps the instance/device extension chain completely cold.
inline bool present_fence_retire_requested(const char* value) {
    return value != nullptr && std::strcmp(value, "1") == 0;
}

} // namespace vkr::vkrpc

#endif // VKRELAY2_COMMON_VKRPC_DEVICE_LOSS_POLICY_H
