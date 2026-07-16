#include "common/argv/argv_split.hpp"

namespace vkr::argv {

SplitArgs split_on_double_dash(const std::vector<std::string>& args) {
    SplitArgs result;
    bool after = false;
    for (const auto& token : args) {
        if (!after && token == "--") {
            result.has_separator = true;
            after = true;
            continue;
        }
        if (after) {
            result.app_argv.push_back(token);
        } else {
            result.options.push_back(token);
        }
    }
    return result;
}

SplitArgs split_on_double_dash(int argc, const char* const argv[], int start) {
    std::vector<std::string> args;
    for (int i = start; i < argc; ++i) {
        args.emplace_back(argv[i]);
    }
    return split_on_double_dash(args);
}

} // namespace vkr::argv
