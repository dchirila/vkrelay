// App/ICD-connection trigger + reconnect handoff. An app connects to the supervisor
// control plane, sends LaunchSession, receives SessionStarted with the worker's
// data-plane endpoint + reconnect token, then connects to that endpoint and proves
// itself with the token. This is the seam Vulkan forwarding builds on.
#include "windows/supervisor/worker_supervisor.hpp"

#include "common/control/control_service.hpp"
#include "common/protocol/gpu.hpp"
#include "common/protocol/ids.hpp"
#include "common/protocol/messages.hpp"
#include "common/transport/tcp.hpp"
#include "common/transport/transport.hpp"
#include "tests/test_assert.hpp"

#include <memory>
#include <string>
#include <thread>
#include <utility>

using namespace vkr;

namespace {

constexpr char kAppId[] = "integration-app";

// Connects to a worker's data plane and returns whether AppHello(token) is
// accepted.
bool data_plane_accepts(const std::string& host, int port, const std::string& token) {
    try {
        auto conn = transport::tcp_connect(host, port);
        transport::MessageChannel channel(*conn);
        protocol::AppHello hello;
        hello.app_token = token;
        channel.send(protocol::MessageType::AppHello, hello.to_body());
        protocol::MessageType type = protocol::MessageType::Unknown;
        json::Value body;
        if (!channel.recv(type, body) || type != protocol::MessageType::AppAck) {
            return false;
        }
        return protocol::AppAck::from_body(body).ok;
    } catch (const std::exception&) {
        return false;
    }
}

// Sends a LaunchSession with the given app_instance_id and returns the reply
// (type + body) verbatim. Throws on EOF.
std::pair<protocol::MessageType, json::Value> send_launch(transport::MessageChannel& channel,
                                                          const std::string& gpu_selector,
                                                          const std::string& app_instance_id) {
    protocol::LaunchSession req;
    req.app_instance_id = app_instance_id;
    req.gpu_selector = gpu_selector;
    channel.send(protocol::MessageType::LaunchSession, req.to_body());
    protocol::MessageType type = protocol::MessageType::Unknown;
    json::Value body;
    if (!channel.recv(type, body)) {
        throw std::runtime_error("no reply to launch_session");
    }
    return {type, body};
}

// Sends a LaunchSession (matching handshake identity) and returns SessionStarted
// (throws on Error/EOF).
protocol::SessionStarted launch_session(transport::MessageChannel& channel,
                                        const std::string& gpu_selector) {
    const auto reply = send_launch(channel, gpu_selector, kAppId);
    if (reply.first != protocol::MessageType::SessionStarted) {
        throw std::runtime_error("expected session_started");
    }
    return protocol::SessionStarted::from_body(reply.second);
}

protocol::SessionClosed close_session(transport::MessageChannel& channel,
                                      const protocol::SessionStarted& session,
                                      const std::string& token) {
    protocol::CloseSession req;
    req.worker_id = session.worker_id;
    req.app_token = token;
    channel.send(protocol::MessageType::CloseSession, req.to_body());
    protocol::MessageType type = protocol::MessageType::Unknown;
    json::Value body;
    if (!channel.recv(type, body) || type != protocol::MessageType::SessionClosed) {
        throw std::runtime_error("expected session_closed");
    }
    return protocol::SessionClosed::from_body(body);
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: integration_app_session <worker-path>\n");
        return 2;
    }

    supervisor::WorkerSupervisorConfig cfg;
    cfg.worker_path = argv[1];
    cfg.heartbeat_timeout_ms = 5000;
    cfg.worker_interval_ms = 50;
    cfg.worker_count = 0; // sessions run until the supervisor kills them

    supervisor::WorkerSupervisor sup(cfg);

    auto app_listener = transport::tcp_listen(0);
    const int app_port = app_listener->port();
    protocol::IdAllocator ids;
    const auto devices = protocol::probe_mocked();

    // App-control server: one connection, served with the supervisor as the
    // session launcher (so LaunchSession is handled).
    std::thread server([&] {
        try {
            auto conn = app_listener->accept();
            transport::MessageChannel channel(*conn);
            control::serve_session(channel, ids, devices, &sup);
        } catch (const std::exception&) {
        }
    });

    try {
        auto conn = transport::tcp_connect("127.0.0.1", app_port);
        transport::MessageChannel channel(*conn);
        protocol::ClientHello hello;
        hello.app_instance_id = kAppId;
        transport::client_handshake(channel, hello);

        // Launch four sessions. Before any app connects to a data plane, the
        // workers are registered and alive -> four supervised workers, each
        // bound to the canonical handshake identity. The count is asserted here,
        // up front: once an app connects and later disconnects, that worker ends
        // its session (app-EOF lifecycle; see integration_vulkan_rpc).
        const protocol::SessionStarted s1 = launch_session(channel, "auto");
        const protocol::SessionStarted s2 = launch_session(channel, "high-performance");
        const protocol::SessionStarted s3 = launch_session(channel, "auto");
        const protocol::SessionStarted s4 = launch_session(channel, "auto");
        VKR_CHECK(!s1.worker_id.empty());
        VKR_CHECK(s1.data_plane_port > 0 && s2.data_plane_port > 0 && s3.data_plane_port > 0);
        VKR_CHECK(!s1.app_token.empty());
        VKR_CHECK(s1.worker_id != s2.worker_id && s2.worker_id != s3.worker_id);
        VKR_CHECK_EQ(sup.active_count(), static_cast<std::size_t>(4));
        for (const auto& st : sup.snapshot()) {
            VKR_CHECK_EQ(st.app_instance_id, std::string(kAppId));
        }

        // A launch_session naming a different app_instance_id than the handshake
        // is rejected (no worker spawned), and the control connection stays usable.
        const auto mismatch = send_launch(channel, "auto", "someone-else");
        VKR_CHECK(mismatch.first == protocol::MessageType::Error);
        if (mismatch.first == protocol::MessageType::Error) {
            VKR_CHECK_EQ(protocol::ErrorMsg::from_body(mismatch.second).code,
                         std::string("app_identity_mismatch"));
        }
        VKR_CHECK_EQ(sup.active_count(), static_cast<std::size_t>(4));

        // A pure-X11 target never opens the Vulkan data plane. Its launcher's explicit close is
        // authenticated by the session token and must acknowledge only after the worker process
        // is actually terminal (which also proves its HWNDs are reclaimed). A bad token neither
        // closes nor enumerates the live session; the valid request remains usable afterward.
        const protocol::SessionClosed rejected = close_session(channel, s4, "wrong-token");
        VKR_CHECK(!rejected.accepted);
        VKR_CHECK_EQ(rejected.reason, std::string("session credentials do not match"));
        std::string wrong_identity_reason;
        VKR_CHECK(
            !sup.close_session("someone-else", s4.worker_id, s4.app_token, wrong_identity_reason));
        VKR_CHECK_EQ(wrong_identity_reason, std::string("session credentials do not match"));
        VKR_CHECK_EQ(sup.active_count(), static_cast<std::size_t>(4));
        const protocol::SessionClosed closed = close_session(channel, s4, s4.app_token);
        VKR_CHECK(closed.accepted);
        VKR_CHECK_EQ(sup.active_count(), static_cast<std::size_t>(3));
        bool s4_terminal = false;
        for (const auto& st : sup.snapshot()) {
            if (st.worker_id == s4.worker_id) {
                s4_terminal = st.state == protocol::WorkerState::Killed ||
                              st.state == protocol::WorkerState::Exited;
            }
        }
        VKR_CHECK(s4_terminal);

        // Data-plane handshake on s1: the app token from SessionStarted is accepted.
        VKR_CHECK(data_plane_accepts(s1.data_plane_host, s1.data_plane_port, s1.app_token));

        // s2: a wrong token must be rejected WITHOUT stranding the session -- the
        // real app can still reconnect with the correct token afterward.
        VKR_CHECK(!data_plane_accepts(s2.data_plane_host, s2.data_plane_port, "wrong-token"));
        VKR_CHECK(data_plane_accepts(s2.data_plane_host, s2.data_plane_port, s2.app_token));

        // s3: a silent peer (connects, never sends app_hello) must not wedge the
        // one data-plane slot: the worker times out the handshake and the real
        // app still gets in.
        auto idle = transport::tcp_connect(s3.data_plane_host, s3.data_plane_port);
        VKR_CHECK(data_plane_accepts(s3.data_plane_host, s3.data_plane_port, s3.app_token));
        idle->close();
    } catch (const std::exception& e) {
        ::vkr::test::report_fail(__FILE__, __LINE__, std::string("app flow failed: ") + e.what());
    }

    server.join();
    sup.shutdown();
    return vkr::test::finish("integration_app_session");
}
