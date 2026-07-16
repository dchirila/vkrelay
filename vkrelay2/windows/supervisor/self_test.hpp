// Supervisor self-test.
#ifndef VKRELAY2_WINDOWS_SUPERVISOR_SELF_TEST_HPP
#define VKRELAY2_WINDOWS_SUPERVISOR_SELF_TEST_HPP

#include <string>

namespace vkr::supervisor {

// Path to the worker executable next to this supervisor binary.
std::string default_worker_path();

// Launches a worker in its own Job Object / process group, observes a
// heartbeat, then kills the worker and confirms the supervisor survives.
// Returns 0 on success, non-zero on the first failed check.
int run_self_test(const std::string& worker_path_override);

} // namespace vkr::supervisor

#endif // VKRELAY2_WINDOWS_SUPERVISOR_SELF_TEST_HPP
