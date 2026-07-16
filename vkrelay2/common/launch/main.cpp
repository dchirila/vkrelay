// Thin entry point for vkrelay2-launch. Recovers argv as UTF-8 from the OS so
// Unicode arguments survive on Windows, then delegates to the launch CLI.
#include "common/argv/command_line.hpp"
#include "common/launch/launch_cli.hpp"

int main(int argc, char** argv) {
    const std::vector<std::string> args =
        vkr::argv::os_args_utf8(argc, argv, /*include_program=*/false);
    return vkr::launch::run_cli(args);
}
