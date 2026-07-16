// Cross-platform process spawning for vkrelay2.
//
// One wrapper owns process creation on each platform:
//   * Windows: a single CreateProcessW call site (UTF-16 conversion, command
//     line construction, environment block, cwd, handle inheritance, Job
//     Object assignment).
//   * POSIX: posix_spawnp with an argv array (never system/popen/bash -c).
//
// Spawning always takes a structured argv array + env-override map + cwd. A
// target command line is never assembled from a shell string.
#ifndef VKRELAY2_COMMON_PROCESS_PROCESS_HPP
#define VKRELAY2_COMMON_PROCESS_PROCESS_HPP

#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace vkr::process {

struct SpawnError : std::runtime_error {
    explicit SpawnError(const std::string& msg) : std::runtime_error(msg) {}
};

struct SpawnRequest {
    std::vector<std::string> argv;                                  // UTF-8; argv[0] is the program
    std::vector<std::pair<std::string, std::string>> env_overrides; // applied over parent env
    std::string cwd;                                                // UTF-8; empty => inherit
    bool capture_stdout = false; // redirect child stdout to a pipe
    bool new_group = false;      // Windows: own Job Object; POSIX: own process group
    // Give the child a CLEAN signal environment (app-lifetime-teardown). POSIX: reset the child's
    // signal mask to empty and all dispositions to default, so a launcher that BLOCKS signals (to
    // run a sigwait thread that owns SIGINT/SIGTERM/SIGHUP) does not leak that blocked mask into
    // the app
    // -- otherwise a later graceful SIGTERM would land on an app that has SIGTERM blocked. No-op on
    // Windows (no POSIX signal model). Only meaningful when the spawner blocks signals; harmless
    // otherwise.
    bool reset_child_signals = false;
    // Pass this process's stdin/stdout/stderr through to the child so its output
    // streams byte-for-byte (no capture/buffering). Ignored when capture_stdout
    // is set. POSIX inherits fds naturally; Windows wires the std handles so a
    // redirected (piped/file) stdout still works.
    bool inherit_streams = false;
};

// Owns a spawned child. Moving transfers ownership. Destroying a Process that
// still owns a running child terminates it (the disposable-worker model).
class Process {
  public:
    Process();
    ~Process();
    Process(Process&&) noexcept;
    Process& operator=(Process&&) noexcept;
    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;

    static Process spawn(const SpawnRequest& request);

    bool valid() const;
    long long pid() const;

    // Captured stdout (requires capture_stdout). read_stdout_line returns the
    // next line without its trailing newline; false on EOF or timeout
    // (timeout_ms < 0 waits indefinitely). read_stdout_to_end blocks until the
    // child closes stdout and returns everything that remains.
    bool read_stdout_line(int timeout_ms, std::string& line);
    std::string read_stdout_to_end();

    // Waits up to timeout_ms (negative => infinite). Returns true if the child
    // has exited and sets exit_code.
    bool wait(int timeout_ms, int& exit_code);

    // Terminates the child (and its group/job when new_group was requested).
    void terminate();

    // Graceful termination with escalation (app-lifetime-teardown).
    //   POSIX: SIGTERM the child (its process GROUP when new_group was requested), wait+reap up to
    //          grace_ms for a clean exit, then SIGKILL + wait+reap if it has not exited. This lets
    //          a cooperative app run its own teardown (e.g. an app closing its Vulkan surfaces,
    //          which drives the clean HWND-destroy path) before the hard kill.
    //   Windows: DEGRADES to the hard Job-Object terminate() -- Windows has no clean SIGTERM
    //          equivalent for an arbitrary child, so there is no graceful phase. Named honestly so
    //          POSIX callers get the escalation and Windows callers are not misled.
    // Idempotent; safe on an already-exited child.
    void terminate_graceful(int grace_ms);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace vkr::process

#endif // VKRELAY2_COMMON_PROCESS_PROCESS_HPP
