// POSIX implementation of vkr::process::Process.
//
// Uses posix_spawnp with an argv array (never system/popen/bash -c). Optional
// stdout capture pipe and optional new process group so the whole child group
// can be killed with one signal.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE // for posix_spawn_file_actions_addchdir_np
#endif

#include "common/process/process.hpp"

#include "common/process/line_queue.hpp"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <map>
#include <spawn.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char** environ;

namespace vkr::process {
namespace {

std::vector<std::string>
build_environment(const std::vector<std::pair<std::string, std::string>>& overrides) {
    std::map<std::string, std::string> env;
    for (char** e = environ; e != nullptr && *e != nullptr; ++e) {
        std::string entry(*e);
        const std::size_t eq = entry.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        env[entry.substr(0, eq)] = entry.substr(eq + 1);
    }
    for (const auto& kv : overrides) {
        env[kv.first] = kv.second;
    }
    std::vector<std::string> out;
    out.reserve(env.size());
    for (const auto& kv : env) {
        out.push_back(kv.first + "=" + kv.second);
    }
    return out;
}

} // namespace

struct Process::Impl {
    pid_t pid = -1;
    bool new_group = false;
    bool capture = false;
    int read_end = -1;
    std::thread reader;
    bool reaped = false;
    int exit_code = 0;

    LineQueue queue;

    void reap_blocking() {
        if (reaped || pid <= 0) {
            return;
        }
        int status = 0;
        if (::waitpid(pid, &status, 0) == pid) {
            if (WIFEXITED(status)) {
                exit_code = WEXITSTATUS(status);
            } else if (WIFSIGNALED(status)) {
                exit_code = 128 + WTERMSIG(status);
            }
            reaped = true;
        }
    }

    void kill_all() {
        if (pid <= 0) {
            return;
        }
        if (new_group) {
            ::kill(-pid, SIGKILL);
        } else {
            ::kill(pid, SIGKILL);
        }
    }

    ~Impl() {
        if (!reaped && pid > 0) {
            kill_all();
        }
        if (reader.joinable()) {
            reader.join();
        }
        if (read_end >= 0) {
            ::close(read_end);
        }
        reap_blocking();
    }
};

Process::Process() = default;
Process::~Process() = default;
Process::Process(Process&&) noexcept = default;
Process& Process::operator=(Process&&) noexcept = default;

Process Process::spawn(const SpawnRequest& request) {
    if (request.argv.empty()) {
        throw SpawnError("spawn requires a non-empty argv");
    }

    auto impl = std::make_unique<Impl>();
    impl->new_group = request.new_group;
    impl->capture = request.capture_stdout;

    int pipe_fds[2] = {-1, -1};
    if (request.capture_stdout) {
        if (::pipe(pipe_fds) != 0) {
            throw SpawnError(std::string("pipe failed: ") + std::strerror(errno));
        }
    }

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);

    short spawn_flags = 0;
    if (request.new_group) {
        spawn_flags |= POSIX_SPAWN_SETPGROUP;
        posix_spawnattr_setpgroup(&attr, 0); // new group, pgid == child pid
    }
    if (request.reset_child_signals) {
        // Clean the signal environment for the child: an EMPTY signal mask
        // + ALL dispositions reset to default. Without this, a launcher that BLOCKS INT/TERM/HUP
        // (so a sigwait thread owns them) would leak that blocked mask into the app via
        // posix_spawn, and a later graceful SIGTERM would land on an app that has SIGTERM blocked.
        sigset_t empty;
        sigemptyset(&empty);
        posix_spawnattr_setsigmask(&attr, &empty);
        sigset_t all;
        sigfillset(&all);
        posix_spawnattr_setsigdefault(&attr, &all);
        spawn_flags |= POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF;
    }
    if (spawn_flags != 0) {
        posix_spawnattr_setflags(&attr, spawn_flags);
    }
    if (!request.cwd.empty()) {
        posix_spawn_file_actions_addchdir_np(&actions, request.cwd.c_str());
    }
    if (request.capture_stdout) {
        posix_spawn_file_actions_adddup2(&actions, pipe_fds[1], STDOUT_FILENO);
        posix_spawn_file_actions_addclose(&actions, pipe_fds[0]);
        posix_spawn_file_actions_addclose(&actions, pipe_fds[1]);
    }

    std::vector<std::string> args = request.argv;
    std::vector<char*> argv_c;
    argv_c.reserve(args.size() + 1);
    for (auto& a : args) {
        argv_c.push_back(a.data());
    }
    argv_c.push_back(nullptr);

    std::vector<std::string> env = build_environment(request.env_overrides);
    std::vector<char*> envp;
    envp.reserve(env.size() + 1);
    for (auto& e : env) {
        envp.push_back(e.data());
    }
    envp.push_back(nullptr);

    pid_t pid = -1;
    const int rc = ::posix_spawnp(&pid, argv_c[0], &actions, &attr, argv_c.data(), envp.data());

    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attr);

    if (request.capture_stdout) {
        ::close(pipe_fds[1]); // parent keeps only the read end
        impl->read_end = pipe_fds[0];
    }

    if (rc != 0) {
        if (impl->read_end >= 0) {
            ::close(impl->read_end);
            impl->read_end = -1;
        }
        throw SpawnError("posix_spawnp failed: " + std::string(std::strerror(rc)));
    }

    impl->pid = pid;

    if (request.capture_stdout) {
        int read_end = impl->read_end;
        LineQueue* queue = &impl->queue;
        impl->reader = std::thread([read_end, queue]() {
            char buf[4096];
            for (;;) {
                const ssize_t n = ::read(read_end, buf, sizeof(buf));
                if (n <= 0) {
                    break;
                }
                queue->push_bytes(buf, static_cast<std::size_t>(n));
            }
            queue->mark_eof();
        });
    }

    Process result;
    result.impl_ = std::move(impl);
    return result;
}

bool Process::valid() const {
    return impl_ != nullptr && impl_->pid > 0;
}
long long Process::pid() const {
    return impl_ ? static_cast<long long>(impl_->pid) : 0;
}

bool Process::read_stdout_line(int timeout_ms, std::string& line) {
    if (!impl_ || !impl_->capture) {
        return false;
    }
    return impl_->queue.pop_line(timeout_ms, line);
}

std::string Process::read_stdout_to_end() {
    if (!impl_ || !impl_->capture) {
        return std::string();
    }
    return impl_->queue.drain_to_end();
}

bool Process::wait(int timeout_ms, int& exit_code) {
    if (!impl_ || impl_->pid <= 0) {
        return false;
    }
    if (impl_->reaped) {
        exit_code = impl_->exit_code;
        return true;
    }
    if (timeout_ms < 0) {
        impl_->reap_blocking();
        exit_code = impl_->exit_code;
        return impl_->reaped;
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
        int status = 0;
        const pid_t r = ::waitpid(impl_->pid, &status, WNOHANG);
        if (r == impl_->pid) {
            if (WIFEXITED(status)) {
                impl_->exit_code = WEXITSTATUS(status);
            } else if (WIFSIGNALED(status)) {
                impl_->exit_code = 128 + WTERMSIG(status);
            }
            impl_->reaped = true;
            exit_code = impl_->exit_code;
            return true;
        }
        if (r < 0) {
            return false;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

void Process::terminate() {
    if (impl_) {
        impl_->kill_all();
    }
}

void Process::terminate_graceful(int grace_ms) {
    if (!impl_ || impl_->pid <= 0 || impl_->reaped) {
        return;
    }
    // Graceful phase: SIGTERM the child's process GROUP (when new_group) or the child, so a
    // cooperative app runs its own teardown (closing its Vulkan surfaces drives the clean HWND
    // destroy) before any hard kill.
    if (impl_->new_group) {
        ::kill(-impl_->pid, SIGTERM);
    } else {
        ::kill(impl_->pid, SIGTERM);
    }
    int ec = 0;
    if (wait(grace_ms, ec)) {
        return; // exited within the grace window -- no SIGKILL needed
    }
    // Escalation: hard-kill the group and reap so we do not leave a zombie.
    impl_->kill_all();
    impl_->reap_blocking();
}

} // namespace vkr::process
