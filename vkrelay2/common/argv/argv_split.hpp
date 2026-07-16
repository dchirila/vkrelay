// Split a launcher command line into vkrelay2 options and the target app
// argv at the first "--" separator. Everything after
// "--" is preserved verbatim as an argv array and never re-encoded as a
// shell string.
#ifndef VKRELAY2_COMMON_ARGV_ARGV_SPLIT_HPP
#define VKRELAY2_COMMON_ARGV_ARGV_SPLIT_HPP

#include <string>
#include <vector>

namespace vkr::argv {

struct SplitArgs {
    std::vector<std::string> options;  // tokens before the first "--"
    bool has_separator = false;        // whether a "--" was present
    std::vector<std::string> app_argv; // tokens after the first "--"
};

SplitArgs split_on_double_dash(const std::vector<std::string>& args);
SplitArgs split_on_double_dash(int argc, const char* const argv[], int start = 1);

} // namespace vkr::argv

#endif // VKRELAY2_COMMON_ARGV_ARGV_SPLIT_HPP
