#include "windows/supervisor/self_test.hpp"

#include "common/logging/logging.hpp"
#include "common/process/process.hpp"

#include <string>

#if defined(_WIN32)
#include <windows.h>
#else
#include <limits.h>
#include <unistd.h>
#endif

namespace vkr::supervisor {
namespace {

constexpr char kComponent[] = "supervisor";

std::string this_exe_path() {
#if defined(_WIN32)
    std::wstring buf(1024, L'\0');
    const DWORD n = ::GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    buf.resize(n);
    std::string out(buf.size() * 4, '\0');
    const int m = ::WideCharToMultiByte(CP_UTF8, 0, buf.data(), static_cast<int>(buf.size()),
                                        out.data(), static_cast<int>(out.size()), nullptr, nullptr);
    out.resize(static_cast<std::size_t>(m));
    return out;
#else
    char buf[PATH_MAX];
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf));
    if (n <= 0) {
        return std::string();
    }
    return std::string(buf, static_cast<std::size_t>(n));
#endif
}

std::string dir_of(const std::string& path) {
    const std::size_t slash = path.find_last_of("/\\");
    if (slash == std::string::npos) {
        return ".";
    }
    return path.substr(0, slash);
}

bool starts_with(const std::string& s, const char* prefix) {
    return s.rfind(prefix, 0) == 0;
}

} // namespace

std::string default_worker_path() {
    const std::string dir = dir_of(this_exe_path());
#if defined(_WIN32)
    return dir + "\\vkrelay2-worker.exe";
#else
    return dir + "/vkrelay2-worker";
#endif
}

int run_self_test(const std::string& worker_path_override) {
    const std::string worker =
        worker_path_override.empty() ? default_worker_path() : worker_path_override;
    VKR_INFO(kComponent) << "self-test starting; worker=" << worker;

    // Check A: spawn a finite-heartbeat worker and observe a clean exit.
    try {
        process::SpawnRequest req;
        req.argv = {worker, "--count", "3", "--interval-ms", "30", "--id", "selftest-a"};
        req.capture_stdout = true;
        req.new_group = true;
        process::Process child = process::Process::spawn(req);
        VKR_INFO(kComponent) << "check A: spawned worker"
                             << "; pid=" << child.pid();

        int heartbeats = 0;
        std::string line;
        while (child.read_stdout_line(2000, line)) {
            if (starts_with(line, "HEARTBEAT")) {
                ++heartbeats;
                VKR_DEBUG(kComponent) << "check A: " << line;
            }
        }

        int exit_code = -1;
        const bool exited = child.wait(5000, exit_code);
        if (!exited) {
            VKR_ERROR(kComponent) << "check A FAILED: worker did not exit";
            return 1;
        }
        if (heartbeats < 1) {
            VKR_ERROR(kComponent) << "check A FAILED: no heartbeat observed";
            return 1;
        }
        if (exit_code != 0) {
            VKR_ERROR(kComponent) << "check A FAILED: worker exit code " << exit_code;
            return 1;
        }
        VKR_INFO(kComponent) << "check A PASSED: " << heartbeats
                             << " heartbeats, clean exit (code 0)";
    } catch (const std::exception& e) {
        VKR_ERROR(kComponent) << "check A FAILED: " << e.what();
        return 1;
    }

    // Check B: spawn an endless worker, observe a heartbeat, kill it, and
    // confirm this supervisor process keeps running.
    try {
        process::SpawnRequest req;
        req.argv = {worker, "--count", "0", "--interval-ms", "30", "--id", "selftest-b"};
        req.capture_stdout = true;
        req.new_group = true;
        process::Process child = process::Process::spawn(req);
        VKR_INFO(kComponent) << "check B: spawned endless worker"
                             << "; pid=" << child.pid();

        bool saw_heartbeat = false;
        std::string line;
        for (int i = 0; i < 50 && !saw_heartbeat; ++i) {
            if (child.read_stdout_line(3000, line) && starts_with(line, "HEARTBEAT")) {
                saw_heartbeat = true;
            }
        }
        if (!saw_heartbeat) {
            VKR_ERROR(kComponent) << "check B FAILED: no heartbeat before kill";
            return 1;
        }

        VKR_INFO(kComponent) << "check B: terminating worker group";
        child.terminate();

        int exit_code = 0;
        const bool exited = child.wait(5000, exit_code);
        if (!exited) {
            VKR_ERROR(kComponent) << "check B FAILED: killed worker did not exit";
            return 1;
        }
        VKR_INFO(kComponent) << "check B PASSED: worker killed (exit code " << exit_code
                             << "); supervisor still running";
    } catch (const std::exception& e) {
        VKR_ERROR(kComponent) << "check B FAILED: " << e.what();
        return 1;
    }

    VKR_INFO(kComponent) << "self-test PASSED";
    return 0;
}

} // namespace vkr::supervisor
