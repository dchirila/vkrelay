// Windows command-line construction that round-trips through
// CommandLineToArgvW / the MSVC CRT argv parser.
//
// The quoting rules only ever act on ASCII metacharacters (space, tab,
// newline, vertical tab, double-quote, backslash), none of which appear as
// UTF-8 continuation bytes, so the algorithm operates correctly on the
// UTF-8 byte representation and is testable on every platform. process_win
// converts the resulting UTF-8 command line to UTF-16 for CreateProcessW.
#ifndef VKRELAY2_COMMON_ARGV_COMMAND_LINE_HPP
#define VKRELAY2_COMMON_ARGV_COMMAND_LINE_HPP

#include <string>
#include <vector>

namespace vkr::argv {

// Quotes a single argument so CommandLineToArgvW recovers it verbatim.
std::string quote_argument(const std::string& arg);

// Joins argv (argv[0] included) into a single Windows command-line string.
std::string build_command_line(const std::vector<std::string>& argv);

// Returns this process's arguments decoded to UTF-8 from the OS-native form.
// On Windows this uses GetCommandLineW/CommandLineToArgvW so Unicode arguments
// survive regardless of the active code page; elsewhere it copies argv. When
// include_program is false, argv[0] (the program) is dropped.
std::vector<std::string> os_args_utf8(int argc, char** argv, bool include_program);

#if defined(_WIN32)
std::wstring utf8_to_wide(const std::string& utf8);
std::string wide_to_utf8(const std::wstring& wide);
#endif

} // namespace vkr::argv

#endif // VKRELAY2_COMMON_ARGV_COMMAND_LINE_HPP
