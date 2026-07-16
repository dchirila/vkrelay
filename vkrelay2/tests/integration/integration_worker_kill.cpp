// Hard-path teardown guarantee: an UNCOOPERATIVE worker whose
// data-plane dispatcher wedges forever and IGNORES the graceful abort hook must STILL be reaped
// after its app disconnects -- proving the leak is bounded independently of graceful acquire
// cancellation.
//
// Real process orchestration: a WorkerSupervisor spawns a real vkrelay2-worker child with the
// `--wedge-data-plane` test hook (once an app establishes, the dispatcher blocks forever, ignoring
// abort_session). An app launches a session, connects to the data plane + establishes, then
// disconnects. The only thing that can end the worker is the HARD path: the liveness observer sets
// app_session_ended directly, the worker's main loop breaks and closes its worker-control channel,
// and the supervisor's control-channel-close grace kills the (still-wedged) child. The test asserts
// the worker leaves its active set within a bounded deadline.
//
// Without the fix (observer only calling abort_session, never app_session_ended) the
// worker would heartbeat forever and never be reaped -- this test would time out (its CTest TIMEOUT
// is the harness backstop).
#include "windows/supervisor/worker_supervisor.hpp"

#include "common/control/control_service.hpp"
#include "common/protocol/gpu.hpp"
#include "common/protocol/ids.hpp"
#include "common/protocol/messages.hpp"
#include "common/transport/tcp.hpp"
#include "common/transport/transport.hpp"
#include "tests/test_assert.hpp"

#include <chrono>
#include <memory>
#include <string>
#include <thread>

using namespace vkr;

namespace {
constexpr char kAppId[] = "integration-worker-kill";

// Establishes the data plane (AppHello -> AppAck ok) and RETURNS the still-open connection so the
// caller can hold it (keeping the worker's dispatcher wedged) and close it later to simulate app
// death. Returns nullptr if the handshake did not complete.
std::unique_ptr<transport::Connection> establish_data_plane(const std::string& host, int port,
                                                            const std::string& token) {
    try {
        auto conn = transport::tcp_connect(host, port);
        transport::MessageChannel channel(*conn);
        protocol::AppHello hello;
        hello.app_token = token;
        channel.send(protocol::MessageType::AppHello, hello.to_body());
        protocol::MessageType type = protocol::MessageType::Unknown;
        json::Value body;
        if (!channel.recv(type, body) || type != protocol::MessageType::AppAck ||
            !protocol::AppAck::from_body(body).ok) {
            return nullptr;
        }
        return conn;
    } catch (const std::exception&) {
        return nullptr;
    }
}
} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: integration_worker_kill <worker-path>\n");
        return 2;
    }

    supervisor::WorkerSupervisorConfig cfg;
    cfg.worker_path = argv[1];
    cfg.heartbeat_timeout_ms = 5000; // generous: the control-close kill must fire first, not this
    cfg.worker_interval_ms = 50;
    cfg.worker_count = 0;                           // run until killed
    cfg.extra_worker_args = {"--wedge-data-plane"}; // the uncooperative-dispatcher hook

    supervisor::WorkerSupervisor sup(cfg);

    auto app_listener = transport::tcp_listen(0);
    const int app_port = app_listener->port();
    protocol::IdAllocator ids;
    const auto devices = protocol::probe_mocked();
    std::thread server([&] {
        try {
            auto conn = app_listener->accept();
            transport::MessageChannel channel(*conn);
            control::serve_session(channel, ids, devices, &sup);
        } catch (const std::exception&) {
        }
    });

    bool reaped = false;
    long long reap_ms = -1;
    try {
        auto conn = transport::tcp_connect("127.0.0.1", app_port);
        transport::MessageChannel channel(*conn);
        protocol::ClientHello hello;
        hello.app_instance_id = kAppId;
        transport::client_handshake(channel, hello);

        protocol::LaunchSession req;
        req.app_instance_id = kAppId;
        req.gpu_selector = "auto";
        channel.send(protocol::MessageType::LaunchSession, req.to_body());
        protocol::MessageType type = protocol::MessageType::Unknown;
        json::Value body;
        VKR_CHECK(channel.recv(type, body));
        VKR_CHECK(type == protocol::MessageType::SessionStarted);
        const protocol::SessionStarted ss = protocol::SessionStarted::from_body(body);
        VKR_CHECK(ss.data_plane_port > 0 && !ss.app_token.empty());
        VKR_CHECK_EQ(sup.active_count(), static_cast<std::size_t>(1));

        // Establish + HOLD the data plane: the worker's dispatcher now wedges forever (ignoring
        // abort).
        std::unique_ptr<transport::Connection> app_dp =
            establish_data_plane(ss.data_plane_host, ss.data_plane_port, ss.app_token);
        VKR_CHECK(app_dp != nullptr);
        // The wedged worker is still alive + supervised.
        VKR_CHECK_EQ(sup.active_count(), static_cast<std::size_t>(1));

        // The app dies. Only the HARD path can now end the worker.
        const auto t0 = std::chrono::steady_clock::now();
        app_dp->close();

        // Poll until the supervisor reaps the (wedged) worker. active_count() drops to 0 only once
        // the supervisor OBSERVES the child's exit via proc.wait(), not merely on
        // the async terminate request -- so this genuinely proves PID death. Bounded; the CTest
        // TIMEOUT backstops the older behavior that never reaped the worker.
        for (int i = 0; i < 400; ++i) { // up to ~20 s, returns early
            if (sup.active_count() == 0) {
                reaped = true;
                reap_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - t0)
                              .count();
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        // The observed exit of a requested termination is classified Killed (not Exited/Crashed).
        if (reaped) {
            const auto snap = sup.snapshot();
            VKR_CHECK_EQ(static_cast<int>(snap.size()), 1);
            if (!snap.empty()) {
                VKR_CHECK(snap.front().state == protocol::WorkerState::Killed);
            }
        }
    } catch (const std::exception& e) {
        ::vkr::test::report_fail(__FILE__, __LINE__, std::string("flow failed: ") + e.what());
    }

    server.join();
    sup.shutdown();

    VKR_CHECK(reaped); // the wedged, abort-ignoring worker was reaped after the app disconnected
    if (reaped) {
        std::fprintf(stderr,
                     "integration_worker_kill: wedged worker reaped %lld ms after app close\n",
                     reap_ms);
    }
    return vkr::test::finish("integration_worker_kill");
}
