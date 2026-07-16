// argv-echo canary.
//
// Emits its own argv, cwd, and environment as a single line of JSON on stdout.
// The quoting regression tests launch this through the exact same spawn path
// as real apps and assert byte-for-byte argv/env/cwd equality.
#include "common/argv/command_line.hpp"
#include "common/util/json.hpp"

#include <cstdio>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
extern char** environ;
#endif

namespace {

std::string current_directory() {
#if defined(_WIN32)
    DWORD len = ::GetCurrentDirectoryW(0, nullptr);
    std::wstring buf(len, L'\0');
    len = ::GetCurrentDirectoryW(len, buf.data());
    buf.resize(len);
    return vkr::argv::wide_to_utf8(buf);
#else
    char buf[4096];
    if (::getcwd(buf, sizeof(buf)) == nullptr) {
        return std::string();
    }
    return std::string(buf);
#endif
}

vkr::json::Value environment_object() {
    vkr::json::Value env = vkr::json::Value::make_object();
#if defined(_WIN32)
    LPWCH strings = ::GetEnvironmentStringsW();
    if (strings != nullptr) {
        const wchar_t* p = strings;
        while (*p != L'\0') {
            std::wstring entry(p);
            p += entry.size() + 1;
            const std::size_t eq = entry.find(L'=', 1);
            if (eq == std::wstring::npos) {
                continue;
            }
            env.set(vkr::argv::wide_to_utf8(entry.substr(0, eq)),
                    vkr::json::Value(vkr::argv::wide_to_utf8(entry.substr(eq + 1))));
        }
        ::FreeEnvironmentStringsW(strings);
    }
#else
    for (char** e = environ; e != nullptr && *e != nullptr; ++e) {
        std::string entry(*e);
        const std::size_t eq = entry.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        env.set(entry.substr(0, eq), vkr::json::Value(entry.substr(eq + 1)));
    }
#endif
    return env;
}

} // namespace

int main(int argc, char** argv) {
    const std::vector<std::string> args =
        vkr::argv::os_args_utf8(argc, argv, /*include_program=*/true);

    vkr::json::Value root = vkr::json::Value::make_object();
    vkr::json::Array argv_arr;
    for (const auto& a : args) {
        argv_arr.emplace_back(a);
    }
    root.set("argv", vkr::json::Value(std::move(argv_arr)));
    root.set("cwd", vkr::json::Value(current_directory()));
    root.set("env", environment_object());

    const std::string line = root.dump(0);
    std::fwrite(line.data(), 1, line.size(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
    return 0;
}
