// Supervisor/worker lifecycle integration test. Drives a real WorkerSupervisor that
// spawns real worker processes
// which connect back over the worker-control channel and heartbeat.
//
// Covers: concurrent workers on different selected GPUs, heartbeat observation,
// crash/exit containment (one worker exits while another survives), and no
// stale sessions after shutdown.
#include "windows/supervisor/worker_supervisor.hpp"

#include "common/protocol/worker_lifecycle.hpp"
#include "common/transport/tcp.hpp"
#include "common/transport/transport.hpp"
#include "tests/test_assert.hpp"

#include <chrono>
#include <string>
#include <thread>

using namespace vkr;

namespace {

const supervisor::WorkerStatus* find(const std::vector<supervisor::WorkerStatus>& v,
                                     const std::string& id) {
    for (const auto& s : v) {
        if (s.worker_id == id) {
            return &s;
        }
    }
    return nullptr;
}

// Polls `pred` until it returns true or the deadline elapses.
template <typename Pred> bool wait_until(int timeout_ms, Pred pred) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return pred();
}

// Two concurrent workers on different GPU selectors, both heartbeating.
void test_concurrent_workers(const std::string& worker_path) {
    supervisor::WorkerSupervisorConfig cfg;
    cfg.worker_path = worker_path;
    cfg.heartbeat_timeout_ms = 3000;
    cfg.worker_interval_ms = 25;
    cfg.worker_count = 0; // run until killed

    supervisor::WorkerSupervisor sup(cfg);
    VKR_CHECK(sup.worker_control_port() > 0);

    const std::string a = sup.launch_worker("high-performance");
    const std::string b = sup.launch_worker("integrated");

    const bool up =
        wait_until(8000, [&] { return sup.active_count() == 2 && sup.total_heartbeats() >= 2; });
    VKR_CHECK(up);

    const auto snap = sup.snapshot();
    const auto* sa = find(snap, a);
    const auto* sb = find(snap, b);
    VKR_CHECK(sa != nullptr && sb != nullptr);
    if (sa != nullptr) {
        VKR_CHECK(sa->connected);
        VKR_CHECK(sa->state == protocol::WorkerState::Running);
    }
    if (sb != nullptr) {
        VKR_CHECK(sb->connected);
        // high-performance and integrated resolve to different adapters.
        VKR_CHECK(sa == nullptr || sa->gpu_name != sb->gpu_name);
    }

    sup.shutdown();
    VKR_CHECK_EQ(sup.active_count(), static_cast<std::size_t>(0)); // no stale sessions
}

// One worker exits on its own (simulated crash); the supervisor detects it and
// the other worker plus the supervisor survive.
void test_crash_containment(const std::string& worker_path) {
    supervisor::WorkerSupervisorConfig cfg;
    cfg.worker_path = worker_path;
    cfg.heartbeat_timeout_ms = 5000;
    cfg.worker_interval_ms = 20;
    cfg.worker_count = 0;

    supervisor::WorkerSupervisor sup(cfg);
    const std::string longlived = sup.launch_worker("auto", 0);  // until killed
    const std::string shortlived = sup.launch_worker("auto", 3); // exits after 3 beats

    const bool contained = wait_until(8000, [&] {
        const auto snap = sup.snapshot();
        const auto* sl = find(snap, longlived);
        const auto* ss = find(snap, shortlived);
        return sl != nullptr && ss != nullptr && sl->state == protocol::WorkerState::Running &&
               protocol::is_terminal(ss->state);
    });
    VKR_CHECK(contained);

    const auto snap = sup.snapshot();
    const auto* sl = find(snap, longlived);
    const auto* ss = find(snap, shortlived);
    if (sl != nullptr) {
        VKR_CHECK(sl->connected); // survivor still heartbeating
    }
    if (ss != nullptr) {
        // A clean exit (code 0 while Running) is classified Exited, never
        // mislabeled Crashed by the channel-close/exit race.
        VKR_CHECK(ss->state == protocol::WorkerState::Exited);
    }
    VKR_CHECK_EQ(sup.active_count(), static_cast<std::size_t>(1)); // only the survivor

    sup.shutdown();
}

// A worker that drops its control channel but keeps running is detected and
// killed; it cannot continue unsupervised.
void test_drop_while_alive_is_killed(const std::string& worker_path) {
    supervisor::WorkerSupervisorConfig cfg;
    cfg.worker_path = worker_path;
    cfg.heartbeat_timeout_ms = 5000; // so the kill comes from the channel-drop path
    cfg.worker_interval_ms = 20;
    cfg.extra_worker_args = {"--linger-ms", "4000"}; // drop channel, stay alive 4s

    supervisor::WorkerSupervisor sup(cfg);
    const std::string id = sup.launch_worker("auto", 2); // 2 beats, then drop + linger

    const bool killed = wait_until(6000, [&] {
        const auto snap = sup.snapshot();
        const auto* s = find(snap, id);
        return s != nullptr && s->state == protocol::WorkerState::Killed;
    });
    VKR_CHECK(killed);

    sup.shutdown();
}

// Shutdown must not hang on an accepted connection that never registers as a
// worker: cancel() releases the handler blocked in recv.
void test_shutdown_unblocks_nonworker(const std::string& worker_path) {
    supervisor::WorkerSupervisorConfig cfg;
    cfg.worker_path = worker_path;

    supervisor::WorkerSupervisor sup(cfg);
    auto conn = transport::tcp_connect("127.0.0.1", sup.worker_control_port());
    // Let the supervisor accept it and block its handler in recv.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    sup.shutdown(); // hangs (pre-fix) if the handler recv is not cancelled
    VKR_CHECK_EQ(sup.active_count(), static_cast<std::size_t>(0));

    // Launching after shutdown is rejected, not silently left unsupervised.
    bool threw = false;
    try {
        sup.launch_worker("auto");
    } catch (const std::exception&) {
        threw = true;
    }
    VKR_CHECK(threw);
    VKR_CHECK_EQ(sup.active_count(), static_cast<std::size_t>(0));
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: integration_supervisor_worker <worker-path>\n");
        return 2;
    }
    const std::string worker_path = argv[1];

    test_concurrent_workers(worker_path);
    test_crash_containment(worker_path);
    test_drop_while_alive_is_killed(worker_path);
    test_shutdown_unblocks_nonworker(worker_path);
    return vkr::test::finish("integration_supervisor_worker");
}
