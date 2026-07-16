#include "common/control/control_service.hpp"
#include "common/protocol/ids.hpp"
#include "common/protocol/messages.hpp"
#include "common/transport/tcp.hpp"
#include "common/transport/transport.hpp"
#include "tests/test_assert.hpp"
#include "windows/supervisor/display_snapshot_cache.hpp"

#include <memory>
#include <string>
#include <thread>

using namespace vkr;

namespace {

display::DisplayLayout make_layout(const std::string& snapshot_id) {
    display::DisplayLayout layout;
    layout.snapshot_id = snapshot_id;
    layout.virtual_bounds = {-1920, -200, 3840, 1280};
    layout.primary_monitor_id = "right";
    display::MonitorDesc left;
    left.stable_id = "left";
    left.device_name = "\\\\.\\DISPLAY2";
    left.bounds = {-1920, -200, 1920, 1080};
    left.work = left.bounds;
    left.dpi_x = 144;
    left.dpi_y = 144;
    display::MonitorDesc right;
    right.stable_id = "right";
    right.device_name = "\\\\.\\DISPLAY1";
    right.bounds = {0, 0, 1920, 1080};
    right.work = right.bounds;
    right.dpi_x = 96;
    right.dpi_y = 96;
    right.primary = true;
    layout.monitors = {left, right};
    return layout;
}

struct RecordingLauncher final : control::SessionLauncher {
    display::DisplayLayout received;
    int launches = 0;

    control::SessionInfo launch_session(const std::string&, const std::string&, const std::string&,
                                        const std::string&, bool, bool, bool,
                                        const display::DisplayLayout* layout) override {
        ++launches;
        VKR_CHECK(layout != nullptr);
        if (layout != nullptr) {
            received = *layout;
        }
        control::SessionInfo info;
        info.worker_id = "wkr-pinned";
        info.data_plane_host = "127.0.0.1";
        info.data_plane_port = 5001;
        info.app_token = "app-token";
        info.display_snapshot_id = received.snapshot_id;
        return info;
    }
};

protocol::ServerHello handshake(transport::MessageChannel& channel, const std::string& app_id) {
    protocol::ClientHello hello;
    hello.app_instance_id = app_id;
    return transport::client_handshake(channel, hello);
}

} // namespace

int main() {
    protocol::IdAllocator ids("sup-pin");
    supervisor::DisplaySnapshotCache cache("sup-pin", 8, [](const std::string& snapshot_id) {
        supervisor::DisplayLayoutProbeResult result;
        result.layout = make_layout(snapshot_id);
        result.ok = display::validate_display_layout(result.layout).ok;
        return result;
    });
    RecordingLauncher launcher;
    auto listener = transport::tcp_listen(0);
    const int port = listener->port();

    std::thread server([&] {
        for (int i = 0; i < 3; ++i) {
            auto conn = listener->accept();
            transport::MessageChannel channel(*conn);
            control::serve_session(channel, ids, protocol::probe_mocked(), &launcher, "mock", {},
                                   &cache);
        }
    });

    std::string queried_id;
    {
        auto conn = transport::tcp_connect("127.0.0.1", port);
        transport::MessageChannel channel(*conn);
        const protocol::ServerHello hello = handshake(channel, "query");
        VKR_CHECK(hello.display_layout.status == display::LayoutDecodeStatus::Valid);
        queried_id = hello.display_layout.layout.snapshot_id;
        VKR_CHECK_EQ(queried_id, std::string("sup-pin/display-1"));
    }

    {
        auto conn = transport::tcp_connect("127.0.0.1", port);
        transport::MessageChannel channel(*conn);
        const protocol::ServerHello hello = handshake(channel, "app");
        VKR_CHECK_EQ(hello.display_layout.layout.snapshot_id, std::string("sup-pin/display-2"));
        protocol::LaunchSession request;
        request.app_instance_id = "app";
        request.display_snapshot_id = queried_id;
        channel.send(protocol::MessageType::LaunchSession, request.to_body());
        protocol::MessageType type = protocol::MessageType::Unknown;
        json::Value body;
        VKR_CHECK(channel.recv(type, body));
        VKR_CHECK(type == protocol::MessageType::SessionStarted);
        const protocol::SessionStarted started = protocol::SessionStarted::from_body(body);
        VKR_CHECK_EQ(started.display_snapshot_id, queried_id);
        VKR_CHECK_EQ(launcher.received.snapshot_id, queried_id);
        VKR_CHECK_EQ(launcher.received.virtual_bounds.x, -1920);
    }

    {
        auto conn = transport::tcp_connect("127.0.0.1", port);
        transport::MessageChannel channel(*conn);
        handshake(channel, "missing");
        protocol::LaunchSession request;
        request.app_instance_id = "missing";
        request.display_snapshot_id = "sup-pin/display-999";
        channel.send(protocol::MessageType::LaunchSession, request.to_body());
        protocol::MessageType type = protocol::MessageType::Unknown;
        json::Value body;
        VKR_CHECK(channel.recv(type, body));
        VKR_CHECK(type == protocol::MessageType::Error);
        const protocol::ErrorMsg error = protocol::ErrorMsg::from_body(body);
        VKR_CHECK_EQ(error.code, std::string("display_snapshot_not_found"));
    }

    server.join();
    VKR_CHECK_EQ(launcher.launches, 1);
    return test::finish("integration_display_snapshot");
}
