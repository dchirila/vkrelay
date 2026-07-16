// Launcher/session reliability: a stalled control client must NOT wedge the
// supervisor's one-at-a-time serve loop. This reproduces the exact do_serve() pattern -- accept ->
// set_read_timeout(idle) -> serve_session in a try/catch -> loop -- in a server thread, then
// proves:
//
//   1. Client A connects and NEVER sends a Hello (a stalled / half-open control client). Without a
//      per-connection read deadline the server would block forever in the handshake recv, so the
//      loop would never return to accept() and EVERY later launch would hang -- the intermittent
//      bring-up hang the user hit.
//   2. With the deadline, A's recv times out, serve_session throws, the loop drops A and accepts
//   the
//      next connection. Client B then completes a full handshake + GPU-list round trip, proving the
//      loop was freed.
//
// Ordering is deterministic (no fixed sleeps): the server publishes an accepted-count the client
// spins on, so A is guaranteed to occupy the loop before B connects. A short idle deadline keeps
// the one real wait small. Mirrors do_serve()'s
// conn->set_read_timeout(default_serve_idle_timeout_ms()).
#include "common/control/control_service.hpp"
#include "common/protocol/gpu.hpp"
#include "common/protocol/ids.hpp"
#include "common/protocol/messages.hpp"
#include "common/transport/tcp.hpp"
#include "common/transport/transport.hpp"
#include "tests/test_assert.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

using namespace vkr;

namespace {
// Short enough to keep the one real wait small; long enough that a healthy handshake never trips
// it.
constexpr int kIdleMs = 400;
} // namespace

int main() {
    std::unique_ptr<transport::Listener> listener;
    try {
        listener = transport::tcp_listen(0); // ephemeral loopback port
    } catch (const std::exception& e) {
        ::vkr::test::report_fail(__FILE__, __LINE__, std::string("listen failed: ") + e.what());
        return vkr::test::finish("integration_serve_deadline");
    }
    const int port = listener->port();
    VKR_CHECK(port > 0);

    protocol::IdAllocator ids;
    const auto devices = protocol::probe_mocked();
    std::atomic<int> accepted{0};
    std::atomic<int> timed_out_sessions{0};
    std::string served_worker_id;

    // The server thread is do_serve()'s loop: serve exactly two connections (the stalled A, then
    // the healthy B) with a per-connection read deadline, then stop.
    std::thread server([&] {
        for (int i = 0; i < 2; ++i) {
            std::unique_ptr<transport::Connection> conn;
            try {
                conn = listener->accept();
            } catch (const std::exception&) {
                break;
            }
            conn->set_read_timeout(kIdleMs); // <- the fix under test
            accepted.fetch_add(1);
            transport::MessageChannel channel(*conn);
            try {
                served_worker_id = control::serve_session(channel, ids, devices);
            } catch (const std::exception&) {
                // A timed-out / stalled session lands here; the loop continues to accept the next.
                timed_out_sessions.fetch_add(1);
            }
        }
    });

    // Client A: connect, then go silent forever (no Hello). Kept alive until the end.
    std::unique_ptr<transport::Connection> stalled;
    try {
        stalled = transport::tcp_connect("127.0.0.1", port);
    } catch (const std::exception& e) {
        ::vkr::test::report_fail(__FILE__, __LINE__, std::string("A connect failed: ") + e.what());
    }

    // Deterministically wait until the server has accepted A and is blocked serving it, so B is
    // guaranteed to be the SECOND connection (proving A could not wedge the loop). Bounded spin.
    for (int i = 0; i < 2000 && accepted.load() < 1; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    VKR_CHECK_EQ(accepted.load(), 1);

    // Client B: a full, healthy handshake + GPU list. It connects while A still holds the loop; it
    // must be served once A's read deadline frees the loop.
    const auto t_start = std::chrono::steady_clock::now();
    try {
        auto conn = transport::tcp_connect("127.0.0.1", port);
        conn->set_read_timeout(5000); // generous client deadline; the server frees in ~kIdleMs
        transport::MessageChannel channel(*conn);

        protocol::ClientHello hello;
        hello.app_instance_id = "integration-serve-deadline";
        const protocol::ServerHello server_hello = transport::client_handshake(channel, hello);
        VKR_CHECK(!server_hello.worker_id.empty());
        VKR_CHECK_EQ(server_hello.protocol_version, protocol::kProtocolVersion);

        channel.send(protocol::MessageType::GpuListRequest, json::Value::make_object());
        protocol::MessageType type = protocol::MessageType::Unknown;
        json::Value body;
        VKR_CHECK(channel.recv(type, body));
        VKR_CHECK(type == protocol::MessageType::GpuListResponse);
        VKR_CHECK_EQ(protocol::gpu_list_from_body(body).size(), devices.size());
        // B's connection closes at scope exit, ending its (healthy) session.
    } catch (const std::exception& e) {
        ::vkr::test::report_fail(__FILE__, __LINE__, std::string("B failed: ") + e.what());
    }
    const auto waited_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - t_start)
                               .count();

    server.join();
    if (stalled) {
        stalled->close();
    }

    // B was served (loop freed) and A was the session that timed out.
    VKR_CHECK_EQ(accepted.load(), 2);
    VKR_CHECK_EQ(timed_out_sessions.load(), 1);
    VKR_CHECK(!served_worker_id.empty());
    // B should have been served shortly after A's deadline fired -- never an unbounded hang.
    // Generous upper bound (CI jitter) that is still far below "wedged forever".
    VKR_CHECK(waited_ms < 5000);
    return vkr::test::finish("integration_serve_deadline");
}
