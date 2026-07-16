// Transport non-consuming peer-close detection. An out-of-band liveness
// observer uses Connection::wait_peer_closed() to notice the app died while the RPC reader is
// blocked elsewhere (the WSI-pump wedge). This pins the primitive's contract on a real loopback
// socket pair:
//
//   1. Peer ALIVE (no data): wait_peer_closed(short) returns false (a bounded wait, not a hang).
//   2. Peer graceful close, NO pending payload (the exact wedge scenario -- the guest is blocked
//      awaiting a response, so nothing is queued before its FIN): wait_peer_closed returns true
//      promptly, and WITHOUT consuming bytes.
//   3. Peer sends payload THEN closes, observer never reads: on POSIX, POLLRDHUP reports the FIN
//   even
//      behind queued data (the strong contract) -> true. On Windows the graceful close is masked by
//      the pending byte until it drains (a documented select/MSG_PEEK limitation, immaterial to the
//      synchronous protocol) -> false while queued, then true once the byte is read.
//
// Deterministic: the close cases block until the FIN arrives (bounded by a generous timeout); the
// alive case is the only real wait (short). No fixed inter-thread sleeps.
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
constexpr int kAliveProbeMs = 100;  // bounded wait for the "still open" probe
constexpr int kClosedWaitMs = 3000; // generous upper bound for detecting a close (returns early)

// Establishes a connected loopback pair; returns the server (accepted) side, and the client side
// via out-param. Both throw on failure (the test reports and bails).
std::unique_ptr<transport::Connection>
make_pair(std::unique_ptr<transport::Connection>& client_out) {
    auto listener = transport::tcp_listen(0);
    const int port = listener->port();
    std::unique_ptr<transport::Connection> server;
    std::thread acceptor([&] {
        try {
            server = listener->accept();
        } catch (const std::exception&) {
        }
    });
    client_out = transport::tcp_connect("127.0.0.1", port);
    acceptor.join();
    return server;
}
} // namespace

int main() {
    // Case 1 + 2: a graceful close with no pending payload (the wedge scenario).
    try {
        std::unique_ptr<transport::Connection> client;
        std::unique_ptr<transport::Connection> server = make_pair(client);
        VKR_CHECK(server != nullptr && client != nullptr);

        // 1. Peer alive, silent: not closed (a bounded false, never a hang).
        VKR_CHECK(!server->wait_peer_closed(kAliveProbeMs));

        // 2. Client closes with nothing queued: the observer sees it promptly.
        const auto t0 = std::chrono::steady_clock::now();
        client->close();
        const bool closed = server->wait_peer_closed(kClosedWaitMs);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0)
                            .count();
        VKR_CHECK(closed);
        VKR_CHECK(ms < kClosedWaitMs); // returned early on the FIN, not on the timeout

        // Non-consuming: a subsequent read_some sees EOF (0), i.e. the primitive stole no bytes and
        // left the stream's close observable to the real reader.
        char b = 0;
        VKR_CHECK_EQ(server->read_some(&b, 1), static_cast<std::size_t>(0));
    } catch (const std::exception& e) {
        ::vkr::test::report_fail(__FILE__, __LINE__, std::string("case 1/2: ") + e.what());
    }

    // Case 3: payload queued BEFORE the FIN, observer never reads it.
    try {
        std::unique_ptr<transport::Connection> client;
        std::unique_ptr<transport::Connection> server = make_pair(client);
        VKR_CHECK(server != nullptr && client != nullptr);

        const char payload = 'X';
        client->write_all(&payload, 1);
        client->close(); // FIN now queued behind the 'X' on the server's receive side

#if defined(_WIN32)
        // Windows: the pending byte masks the FIN in select+MSG_PEEK until it drains. Document the
        // exact behavior: not-closed while queued, then closed once the byte is consumed.
        VKR_CHECK(!server->wait_peer_closed(kAliveProbeMs));
        char got = 0;
        VKR_CHECK_EQ(server->read_some(&got, 1), static_cast<std::size_t>(1));
        VKR_CHECK_EQ(got, 'X');
        VKR_CHECK(server->wait_peer_closed(kClosedWaitMs));
#else
        // POSIX: POLLRDHUP reports the FIN even with the 'X' still queued (the strong contract),
        // and the byte remains readable afterwards (non-consuming).
        VKR_CHECK(server->wait_peer_closed(kClosedWaitMs));
        char got = 0;
        VKR_CHECK_EQ(server->read_some(&got, 1), static_cast<std::size_t>(1));
        VKR_CHECK_EQ(got, 'X');
#endif
    } catch (const std::exception& e) {
        ::vkr::test::report_fail(__FILE__, __LINE__, std::string("case 3: ") + e.what());
    }

    return vkr::test::finish("integration_transport_peerclose");
}
