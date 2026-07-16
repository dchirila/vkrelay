// Deterministic worker-teardown test. Composes the two halves of the fix on a real
// loopback socket pair, WITHOUT a Vulkan backend, and proves the full chain that bounds the
// orphan-window failure:
//
//   app peer-close  ->  out-of-band liveness observer fires abort  ->  the wedged acquire releases.
//
// A "wedged acquire" thread runs the real poll_acquire with an infinite guest timeout and a stub
// host-acquire that never returns an image (it always times out) -- i.e. the exact
// vkAcquireNextImageKHR wedge, minus the driver. It can ONLY escape via the abort path. The
// observer watches the server side of the pair; closing the client (the app dying) must make the
// observer fire abort_session, which flips the atomic the acquire poll checks, releasing it with
// aborted=true.
//
// Bounded + deterministic: short quanta; a steady-clock deadline is the hard bound (also mirrored
// by a CTest TIMEOUT) so a regression that fails to release cannot hang the gate.
#include "common/transport/tcp.hpp"
#include "common/transport/transport.hpp"
#include "tests/test_assert.hpp"
#include "windows/worker/acquire_poll.hpp"
#include "windows/worker/liveness_observer.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

using namespace vkr;

namespace {
constexpr int kQuantumMs = 20; // fast poll quantum for the test
constexpr int kBoundMs = 3000; // generous hard bound over the 20 ms quanta
} // namespace

int main() {

    std::unique_ptr<transport::Listener> listener;
    std::unique_ptr<transport::Connection> server;
    std::unique_ptr<transport::Connection> client;
    try {
        listener = transport::tcp_listen(0);
        const int port = listener->port();
        std::thread acceptor([&] {
            try {
                server = listener->accept();
            } catch (const std::exception&) {
            }
        });
        client = transport::tcp_connect("127.0.0.1", port);
        acceptor.join();
    } catch (const std::exception& e) {
        ::vkr::test::report_fail(__FILE__, __LINE__, std::string("pair setup: ") + e.what());
        return vkr::test::finish("integration_worker_teardown");
    }
    VKR_CHECK(server != nullptr && client != nullptr);

    // The session-abort atomic the real backend's acquire poll checks; abort_session() (here, the
    // observer callback) stores it.
    std::atomic<bool> session_aborting{false};

    // The wedged acquire: an infinite guest wait whose host acquire never yields an image. It
    // escapes ONLY when session_aborting flips. Records its outcome for the assertions.
    std::atomic<bool> acquire_done{false};
    std::atomic<bool> acquire_aborted{false};
    std::atomic<int> host_calls{0};
    std::thread wedged_acquire([&] {
        const worker::AcquirePollOutcome o = worker::poll_acquire(
            worker::kInfiniteTimeout, static_cast<std::uint64_t>(kQuantumMs) * 1000 * 1000,
            /*timeout_result=*/2,
            [&](std::uint64_t) {
                host_calls.fetch_add(1);
                return 2; /* always VK_TIMEOUT -> no image, keep waiting */
            },
            [&] { return session_aborting.load(std::memory_order_relaxed); },
            [] { return static_cast<std::uint64_t>(0); });
        acquire_aborted.store(o.aborted, std::memory_order_relaxed);
        acquire_done.store(true, std::memory_order_relaxed);
    });

    // The out-of-band observer on the server side: on peer-close it fires the abort hook.
    std::atomic<bool> observer_stop{false};
    std::atomic<bool> abort_fired{false};
    std::thread observer([&] {
        worker::run_liveness_observer(*server, observer_stop, kQuantumMs, [&] {
            abort_fired.store(true, std::memory_order_relaxed);
            session_aborting.store(true, std::memory_order_relaxed);
        });
    });

    // Do not let scheduler timing turn the final host_calls assertion into a race: establish that
    // the acquire has entered its host-poll loop before simulating the peer close.
    const auto start_deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(kBoundMs);
    while (host_calls.load(std::memory_order_relaxed) == 0 &&
           std::chrono::steady_clock::now() < start_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    VKR_CHECK(host_calls.load(std::memory_order_relaxed) >= 1);

    // Sanity: while the app is alive, nothing aborts and the acquire stays wedged.
    VKR_CHECK(!acquire_done.load());
    VKR_CHECK(!abort_fired.load());

    // The app dies.
    const auto t0 = std::chrono::steady_clock::now();
    client->close();

    // Bounded wait for the full chain to complete (observer -> abort -> acquire releases).
    bool released = false;
    while (
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0)
            .count() < kBoundMs) {
        if (acquire_done.load(std::memory_order_relaxed)) {
            released = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    observer_stop.store(true, std::memory_order_relaxed);
    observer.join();
    wedged_acquire.join();

    VKR_CHECK(released);               // the acquire escaped the wedge within the bound
    VKR_CHECK(abort_fired.load());     // via the observer's abort hook
    VKR_CHECK(acquire_aborted.load()); // and it reported the abort, not a spurious result
    VKR_CHECK(host_calls.load() >= 1); // it really did poll the host acquire (was wedged)

    if (client) {
        client->close();
    }
    return vkr::test::finish("integration_worker_teardown");
}
