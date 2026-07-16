// Loopback control-plane integration test.
//
// Runs the real server control session in a thread and a client over a real
// 127.0.0.1 TCP connection: handshake (ids minted), GPU list, GPU select.
// Proves transport + handshake + GPU messages end to end without Vulkan.
#include "common/control/control_service.hpp"
#include "common/protocol/gpu.hpp"
#include "common/protocol/ids.hpp"
#include "common/protocol/messages.hpp"
#include "common/transport/tcp.hpp"
#include "common/transport/transport.hpp"
#include "tests/test_assert.hpp"

#include <string>
#include <thread>

using namespace vkr;

int main() {
    std::unique_ptr<transport::Listener> listener;
    try {
        listener = transport::tcp_listen(0); // ephemeral loopback port
    } catch (const std::exception& e) {
        ::vkr::test::report_fail(__FILE__, __LINE__, std::string("listen failed: ") + e.what());
        return vkr::test::finish("integration_handshake");
    }
    const int port = listener->port();
    VKR_CHECK(port > 0);

    protocol::IdAllocator ids;
    const auto devices = protocol::probe_mocked();
    std::string served_worker_id;

    std::thread server([&] {
        try {
            auto conn = listener->accept();
            transport::MessageChannel channel(*conn);
            served_worker_id = control::serve_session(channel, ids, devices);
        } catch (const std::exception&) {
            // Reported via the client-side assertions below.
        }
    });

    try {
        auto conn = transport::tcp_connect("127.0.0.1", port);
        transport::MessageChannel channel(*conn);

        protocol::ClientHello hello;
        hello.app_instance_id = "integration-handshake";
        hello.gpu_selector = "integrated";
        const protocol::ServerHello server_hello = transport::client_handshake(channel, hello);
        VKR_CHECK(!server_hello.supervisor_session_id.empty());
        VKR_CHECK(!server_hello.worker_id.empty());
        VKR_CHECK_EQ(server_hello.protocol_version, protocol::kProtocolVersion);

        // GPU list over the wire.
        channel.send(protocol::MessageType::GpuListRequest, json::Value::make_object());
        protocol::MessageType type = protocol::MessageType::Unknown;
        json::Value body;
        VKR_CHECK(channel.recv(type, body));
        VKR_CHECK(type == protocol::MessageType::GpuListResponse);
        const auto listed = protocol::gpu_list_from_body(body);
        VKR_CHECK_EQ(listed.size(), devices.size());

        // GPU select over the wire.
        protocol::GpuSelectRequest req;
        req.selector = "vendor:0x8086";
        channel.send(protocol::MessageType::GpuSelectRequest, req.to_body());
        VKR_CHECK(channel.recv(type, body));
        VKR_CHECK(type == protocol::MessageType::GpuSelectResponse);
        const auto resp = protocol::GpuSelectResponse::from_body(body);
        VKR_CHECK(resp.ok);
        VKR_CHECK_EQ(resp.device.vendor_id, static_cast<std::uint32_t>(0x8086));
        // Connection closes at scope exit, ending the server session.
    } catch (const std::exception& e) {
        ::vkr::test::report_fail(__FILE__, __LINE__, std::string("client failed: ") + e.what());
    }

    server.join();
    VKR_CHECK(!served_worker_id.empty());

    // Producer guard: status==Valid is not trusted. An invalid supervisor object is
    // rejected before serialization with the validator's source-side diagnostic.
    bool server_rejected_invalid = false;
    std::thread invalid_server([&] {
        try {
            auto conn = listener->accept();
            transport::MessageChannel channel(*conn);
            display::DisplayLayoutDecodeResult invalid;
            invalid.status =
                display::LayoutDecodeStatus::Valid; // object itself is intentionally empty
            transport::server_handshake(channel, ids, "mock", {}, invalid);
        } catch (const transport::TransportError&) {
            server_rejected_invalid = true;
        }
    });
    try {
        auto conn = transport::tcp_connect("127.0.0.1", port);
        transport::MessageChannel channel(*conn);
        protocol::ClientHello hello;
        hello.app_instance_id = "invalid-producer";
        transport::client_handshake(channel, hello);
        VKR_CHECK(false);
    } catch (const transport::TransportError& e) {
        VKR_CHECK(std::string(e.what()).find("display_layout_invalid") != std::string::npos);
    }
    invalid_server.join();
    VKR_CHECK(server_rejected_invalid);
    return vkr::test::finish("integration_handshake");
}
