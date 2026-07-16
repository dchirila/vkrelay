#include "common/argv/command_line.hpp"

#if defined(_WIN32)
#include <windows.h>

#include <shellapi.h>
#include <stdexcept>
#pragma comment(lib, "Shell32.lib")
#endif

namespace vkr::argv {

std::string quote_argument(const std::string& arg) {
    // An argument with no whitespace or quotes needs no quoting; empty
    // arguments must be quoted so they survive as a distinct token.
    if (!arg.empty() && arg.find_first_of(" \t\n\v\"") == std::string::npos) {
        return arg;
    }

    std::string out;
    out.push_back('"');
    for (auto it = arg.begin();; ++it) {
        std::size_t backslashes = 0;
        while (it != arg.end() && *it == '\\') {
            ++it;
            ++backslashes;
        }
        if (it == arg.end()) {
            // Escape trailing backslashes so they do not consume the
            // closing quote.
            out.append(backslashes * 2, '\\');
            break;
        }
        if (*it == '"') {
            // Escape backslashes that precede a quote, then the quote.
            out.append(backslashes * 2 + 1, '\\');
            out.push_back('"');
        } else {
            out.append(backslashes, '\\');
            out.push_back(*it);
        }
    }
    out.push_back('"');
    return out;
}

std::string build_command_line(const std::vector<std::string>& argv) {
    std::string cmd;
    for (std::size_t i = 0; i < argv.size(); ++i) {
        if (i != 0) {
            cmd.push_back(' ');
        }
        cmd += quote_argument(argv[i]);
    }
    return cmd;
}

std::vector<std::string> os_args_utf8(int argc, char** argv, bool include_program) {
    std::vector<std::string> out;
#if defined(_WIN32)
    (void) argc;
    (void) argv;
    int count = 0;
    LPWSTR* wide = ::CommandLineToArgvW(::GetCommandLineW(), &count);
    if (wide == nullptr) {
        throw std::runtime_error("CommandLineToArgvW failed");
    }
    for (int i = include_program ? 0 : 1; i < count; ++i) {
        out.push_back(wide_to_utf8(wide[i]));
    }
    ::LocalFree(wide);
#else
    for (int i = include_program ? 0 : 1; i < argc; ++i) {
        out.emplace_back(argv[i]);
    }
#endif
    return out;
}

#if defined(_WIN32)
std::wstring utf8_to_wide(const std::string& utf8) {
    if (utf8.empty()) {
        return std::wstring();
    }
    const int len = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                                          static_cast<int>(utf8.size()), nullptr, 0);
    if (len <= 0) {
        throw std::runtime_error("invalid UTF-8 in argument or environment");
    }
    std::wstring wide(static_cast<std::size_t>(len), L'\0');
    ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()),
                          wide.data(), len);
    return wide;
}

std::string wide_to_utf8(const std::wstring& wide) {
    if (wide.empty()) {
        return std::string();
    }
    const int len = ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                                          nullptr, 0, nullptr, nullptr);
    if (len <= 0) {
        throw std::runtime_error("invalid UTF-16 to UTF-8 conversion");
    }
    std::string utf8(static_cast<std::size_t>(len), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), utf8.data(), len,
                          nullptr, nullptr);
    return utf8;
}
#endif

} // namespace vkr::argv
