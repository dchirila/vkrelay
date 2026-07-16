// vkrelay2-launch: builds, serializes, and (optionally) runs a structured
// launch descriptor.
//
// Two boundary styles are supported so neither shell needs to encode argv as
// a command string:
//   * Bash-friendly: options + "-- <app argv...>" passed directly (shell
//     arrays preserve argv).
//   * Quoting-hostile shells (PowerShell): structured input files
//     (--argv-file / --env-file / --cwd-file, NUL-separated) and a descriptor
//     file (--run-descriptor), so only simple path tokens cross the boundary.
#ifndef VKRELAY2_COMMON_LAUNCH_LAUNCH_CLI_HPP
#define VKRELAY2_COMMON_LAUNCH_LAUNCH_CLI_HPP

#include <string>
#include <vector>

namespace vkr::launch {

// Runs the launch CLI. `args` excludes argv[0]. Returns the process exit code
// (the child's exit code in --run modes).
int run_cli(const std::vector<std::string>& args);

} // namespace vkr::launch

#endif // VKRELAY2_COMMON_LAUNCH_LAUNCH_CLI_HPP
