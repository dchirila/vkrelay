// app-lifetime-teardown: Process::terminate_graceful escalation + the reset_child_signals guard.
//
// Self-spawning (no external helper): re-execs itself as `--child <mode>` so the test is
// self-contained. The POSIX cases prove (1) a cooperative app honoring SIGTERM exits during the
// graceful phase WELL under the grace -- AND that reset_child_signals delivered SIGTERM even though
// the parent has it BLOCKED (the launcher's sigwait posture); (2) WITHOUT reset_child_signals the
// child inherits the parent's blocked SIGTERM and is escalated to SIGKILL (the poisoning the option
// prevents); (3) an app that IGNORES SIGTERM is escalated to SIGKILL after the grace and reaped. On
// Windows terminate_graceful degrades to a hard terminate; we just assert it kills a running child.
#include "common/process/process.hpp"
#include "tests/test_assert.hpp"

#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>

#ifndef _WIN32
#include <csignal>
#include <pthread.h>
#endif

namespace {

int child_main(const std::string& mode) {
#ifndef _WIN32
    if (mode == "cooperative") {
        std::signal(SIGTERM, [](int) { std::_Exit(0); }); // a well-behaved app exits on SIGTERM
    } else if (mode == "ignore") {
        std::signal(SIGTERM, SIG_IGN); // an app that ignores SIGTERM (only SIGKILL stops it)
    }
#endif
    (void) mode;
    // Readiness barrier: only after the signal disposition is in place do we announce readiness, so
    // the parent never sends SIGTERM into the window before it is installed (which would hit the
    // default action, not the handler/ignore).
    std::puts("ready");
    std::fflush(stdout);
    std::this_thread::sleep_for(std::chrono::seconds(30)); // the parent terminates us first
    return 7;                                              // not expected within the test window
}

vkr::process::Process spawn_child(const std::string& self, const std::string& mode, bool reset) {
    vkr::process::SpawnRequest req;
    req.argv = {self, "--child", mode};
    req.new_group = true; // so terminate_graceful signals the whole group
    req.reset_child_signals = reset;
    req.capture_stdout = true; // for the readiness barrier
    return vkr::process::Process::spawn(req);
}

// Block until the child has printed "ready" (its signal disposition is installed).
void await_ready(vkr::process::Process& child) {
    std::string line;
    VKR_CHECK(child.read_stdout_line(5000, line));
    VKR_CHECK_EQ(line, std::string("ready"));
}

} // namespace

int main(int argc, char** argv) {
    if (argc >= 3 && std::string(argv[1]) == "--child") {
        return child_main(argv[2]);
    }
    const std::string self = argv[0];

#ifndef _WIN32
    // Simulate the launcher's posture: SIGTERM BLOCKED in the parent (a sigwait thread would own
    // it).
    sigset_t blk;
    sigemptyset(&blk);
    sigaddset(&blk, SIGTERM);
    sigset_t prev;
    ::pthread_sigmask(SIG_BLOCK, &blk, &prev);

    // (1) reset_child_signals gives the child a CLEAN env: despite the parent's blocked SIGTERM,
    // the
    //     cooperative child receives it and exits 0 well under the grace (graceful phase, no
    //     SIGKILL).
    {
        auto child = spawn_child(self, "cooperative", /*reset=*/true);
        await_ready(child);
        const auto t0 = std::chrono::steady_clock::now();
        child.terminate_graceful(3000);
        const auto dt = std::chrono::steady_clock::now() - t0;
        int code = -1;
        VKR_CHECK(child.wait(0, code)); // terminate_graceful already reaped
        VKR_CHECK_EQ(code, 0);          // exited via its SIGTERM handler, not SIGKILL
        VKR_CHECK(dt < std::chrono::milliseconds(2500)); // returned fast -> SIGTERM was delivered
    }
    // (2) WITHOUT reset_child_signals the child inherits the parent's BLOCKED SIGTERM, so the
    // graceful
    //     phase cannot deliver it -> escalation to SIGKILL. This is exactly the poisoning the
    //     option prevents.
    {
        auto child = spawn_child(self, "cooperative", /*reset=*/false);
        await_ready(child);
        child.terminate_graceful(300); // short grace
        int code = -1;
        VKR_CHECK(child.wait(0, code));
        VKR_CHECK_EQ(code, 128 + SIGKILL); // SIGTERM was blocked in the child -> hard kill + reap
    }
    // (3) An app that IGNORES SIGTERM is escalated to SIGKILL after the grace and reaped.
    {
        auto child = spawn_child(self, "ignore", /*reset=*/true);
        await_ready(child);
        child.terminate_graceful(300);
        int code = -1;
        VKR_CHECK(child.wait(0, code));
        VKR_CHECK_EQ(code, 128 + SIGKILL);
    }

    ::pthread_sigmask(SIG_SETMASK, &prev, nullptr);
#else
    // Windows: terminate_graceful degrades to the hard Job-Object terminate; assert it kills a
    // running child and the child is reaped.
    {
        auto child = spawn_child(self, "sleep", /*reset=*/false);
        await_ready(child);
        child.terminate_graceful(300);
        int code = -1;
        VKR_CHECK(child.wait(3000, code));
    }
#endif

    return vkr::test::finish("integration_process_teardown");
}
