#include "common/protocol/messages.hpp"
#include "tests/test_assert.hpp"

#include <array>
#include <string>

using namespace vkr;
using namespace vkr::protocol;

namespace {

void test_type_strings() {
    VKR_CHECK(message_type_from_string("hello") == MessageType::Hello);
    VKR_CHECK(message_type_from_string("gpu_list_response") == MessageType::GpuListResponse);
    VKR_CHECK(message_type_from_string("close_session") == MessageType::CloseSession);
    VKR_CHECK(message_type_from_string("session_closed") == MessageType::SessionClosed);
    VKR_CHECK(message_type_from_string("nope") == MessageType::Unknown);
    VKR_CHECK_EQ(std::string(to_string(MessageType::Heartbeat)), std::string("heartbeat"));
}

void test_message_type_abi_golden() {
    // This table is the cross-TU process ABI, not merely source ordering. Existing numeric values
    // and wire names are immutable; new entries append with the next unused value.
    struct Entry {
        MessageType type;
        int value;
        const char* wire_name;
    };
    constexpr std::array<Entry, 19> golden{{
        {MessageType::Hello, 0, "hello"},
        {MessageType::HelloAck, 1, "hello_ack"},
        {MessageType::Error, 2, "error"},
        {MessageType::GpuListRequest, 3, "gpu_list_request"},
        {MessageType::GpuListResponse, 4, "gpu_list_response"},
        {MessageType::GpuSelectRequest, 5, "gpu_select_request"},
        {MessageType::GpuSelectResponse, 6, "gpu_select_response"},
        {MessageType::WorkerHello, 7, "worker_hello"},
        {MessageType::WorkerAck, 8, "worker_ack"},
        {MessageType::Heartbeat, 9, "heartbeat"},
        {MessageType::LaunchSession, 10, "launch_session"},
        {MessageType::SessionStarted, 11, "session_started"},
        {MessageType::AppHello, 12, "app_hello"},
        {MessageType::AppAck, 13, "app_ack"},
        {MessageType::SidecarHello, 14, "sidecar_hello"},
        {MessageType::SidecarAck, 15, "sidecar_ack"},
        {MessageType::CloseSession, 16, "close_session"},
        {MessageType::SessionClosed, 17, "session_closed"},
        {MessageType::Unknown, 18, "unknown"},
    }};
    for (const Entry& entry : golden) {
        VKR_CHECK_EQ(static_cast<int>(entry.type), entry.value);
        VKR_CHECK_EQ(std::string(to_string(entry.type)), std::string(entry.wire_name));
        if (entry.type != MessageType::Unknown) {
            VKR_CHECK(message_type_from_string(entry.wire_name) == entry.type);
        }
    }
}

void test_close_session_round_trip() {
    CloseSession close;
    close.worker_id = "wkr-42";
    close.app_token = "unguessable-app-token";
    const CloseSession close_rt = CloseSession::from_body(close.to_body());
    VKR_CHECK_EQ(close_rt.worker_id, close.worker_id);
    VKR_CHECK_EQ(close_rt.app_token, close.app_token);

    SessionClosed closed;
    closed.worker_id = close.worker_id;
    closed.accepted = true;
    closed.reason = "session ended";
    const SessionClosed closed_rt = SessionClosed::from_body(closed.to_body());
    VKR_CHECK_EQ(closed_rt.worker_id, closed.worker_id);
    VKR_CHECK(closed_rt.accepted);
    VKR_CHECK_EQ(closed_rt.reason, closed.reason);
}

void test_envelope_round_trip() {
    ClientHello hello;
    hello.app_instance_id = "app-1";
    hello.gpu_selector = "high-performance";
    hello.display_backend = "wayland";

    const std::string payload = encode_message(MessageType::Hello, hello.to_body());
    MessageType type = MessageType::Unknown;
    json::Value body;
    std::string err;
    VKR_CHECK(decode_message(payload, type, body, err));
    VKR_CHECK(type == MessageType::Hello);

    const ClientHello decoded = ClientHello::from_body(body);
    VKR_CHECK_EQ(decoded.app_instance_id, hello.app_instance_id);
    VKR_CHECK_EQ(decoded.gpu_selector, hello.gpu_selector);
    VKR_CHECK_EQ(decoded.display_backend, hello.display_backend);
    VKR_CHECK_EQ(decoded.protocol_version, kProtocolVersion);
}

void test_gpu_list_round_trip() {
    const std::vector<GpuDevice> devices = probe_mocked();
    const std::string payload =
        encode_message(MessageType::GpuListResponse, gpu_list_to_body(devices));

    MessageType type = MessageType::Unknown;
    json::Value body;
    std::string err;
    VKR_CHECK(decode_message(payload, type, body, err));
    VKR_CHECK(type == MessageType::GpuListResponse);

    const std::vector<GpuDevice> back = gpu_list_from_body(body);
    VKR_CHECK_EQ(back.size(), devices.size());
    if (back.size() == devices.size()) {
        for (std::size_t i = 0; i < devices.size(); ++i) {
            VKR_CHECK_EQ(back[i].name, devices[i].name);
            VKR_CHECK_EQ(back[i].vendor_id, devices[i].vendor_id);
            VKR_CHECK_EQ(back[i].device_id, devices[i].device_id);
            VKR_CHECK(back[i].type == devices[i].type);
            VKR_CHECK_EQ(back[i].usable, devices[i].usable);
            VKR_CHECK_EQ(back[i].roles.size(), devices[i].roles.size());
        }
    }
}

void test_server_hello_worker_backend() {
    // Round-trips the advertised backend through the wire.
    ServerHello hello;
    hello.supervisor_session_id = "sup-1";
    hello.worker_id = "wkr-1";
    hello.worker_backend = "real";
    const std::string payload = encode_message(MessageType::HelloAck, hello.to_body());
    MessageType type = MessageType::Unknown;
    json::Value body;
    std::string err;
    VKR_CHECK(decode_message(payload, type, body, err));
    VKR_CHECK(type == MessageType::HelloAck);
    const ServerHello back = ServerHello::from_body(body);
    VKR_CHECK_EQ(back.worker_id, std::string("wkr-1"));
    VKR_CHECK_EQ(back.worker_backend, std::string("real"));

    // Default is "real" off a "real" daemon; but an absent/legacy field must read as "mock" -- the
    // fail-closed direction (never assume a daemon serves real host Vulkan).
    json::Value legacy = json::Value::make_object();
    legacy.set("protocol_version", json::Value(kProtocolVersion));
    legacy.set("worker_id", json::Value(std::string("wkr-9")));
    const ServerHello fallback = ServerHello::from_body(legacy);
    VKR_CHECK_EQ(fallback.worker_backend, std::string("mock"));
}

void test_server_hello_host_display() {
    // GD: the host's addressable window space rides the handshake (additive). Round-trip.
    ServerHello hello;
    hello.host_display.work_w = 2560;
    hello.host_display.work_h = 1528;
    hello.host_display.dpi = 144;
    const std::string payload = encode_message(MessageType::HelloAck, hello.to_body());
    MessageType type = MessageType::Unknown;
    json::Value body;
    std::string err;
    VKR_CHECK(decode_message(payload, type, body, err));
    const ServerHello back = ServerHello::from_body(body);
    VKR_CHECK_EQ(back.host_display.work_w, 2560);
    VKR_CHECK_EQ(back.host_display.work_h, 1528);
    VKR_CHECK_EQ(back.host_display.dpi, 144);

    // Absent (an old daemon) -> all-zero = unknown, the keep-display-default direction.
    json::Value legacy = json::Value::make_object();
    legacy.set("protocol_version", json::Value(kProtocolVersion));
    const ServerHello fallback = ServerHello::from_body(legacy);
    VKR_CHECK_EQ(fallback.host_display.work_w, 0);
    VKR_CHECK_EQ(fallback.host_display.work_h, 0);
    VKR_CHECK_EQ(fallback.host_display.dpi, 0);

    // All-zero (unknown) is OMITTED from the body, so an old client sees an unchanged HelloAck.
    ServerHello unknown;
    VKR_CHECK(unknown.to_body().find("host_work_w") == nullptr);
}

void test_server_hello_display_layout_codec() {
    ServerHello hello;
    VKR_CHECK(hello.display_layout.status == display::LayoutDecodeStatus::Absent);
    VKR_CHECK(hello.to_body().find("display_layout") == nullptr);

    display::DisplayLayout layout;
    layout.snapshot_id = "sup-1/display-1";
    layout.primary_monitor_id = "monitor-a";
    layout.virtual_bounds = {-1920, 0, 3840, 1080};
    display::MonitorDesc left;
    left.stable_id = "monitor-b";
    left.device_name = "\\\\.\\DISPLAY2";
    left.bounds = {-1920, 0, 1920, 1080};
    left.work = left.bounds;
    left.dpi_x = 144;
    left.dpi_y = 144;
    display::MonitorDesc primary = left;
    primary.stable_id = "monitor-a";
    primary.device_name = "\\\\.\\DISPLAY1";
    primary.bounds = {0, 0, 1920, 1080};
    primary.work = primary.bounds;
    primary.dpi_x = 96;
    primary.dpi_y = 96;
    primary.primary = true;
    layout.monitors = {left, primary};
    VKR_CHECK(display::validate_display_layout(layout).ok);

    hello.display_layout.status = display::LayoutDecodeStatus::Valid;
    hello.display_layout.layout = layout;
    const ServerHello back = ServerHello::from_body(hello.to_body());
    VKR_CHECK(back.display_layout.status == display::LayoutDecodeStatus::Valid);
    VKR_CHECK_EQ(back.display_layout.layout.snapshot_id, layout.snapshot_id);
    VKR_CHECK_EQ(back.display_layout.layout.monitors.size(), static_cast<std::size_t>(2));

    json::Value malformed = hello.to_body();
    malformed.set("display_layout", json::Value("not-an-object"));
    VKR_CHECK(ServerHello::from_body(malformed).display_layout.status ==
              display::LayoutDecodeStatus::Malformed);

    json::Value unsupported = display::display_layout_to_json(layout);
    unsupported.set("schema_version", json::Value(2));
    json::Value future_body = json::Value::make_object();
    future_body.set("display_layout", std::move(unsupported));
    VKR_CHECK(ServerHello::from_body(future_body).display_layout.status ==
              display::LayoutDecodeStatus::UnsupportedSchema);

    WorkerAck worker_ack;
    worker_ack.ok = true;
    worker_ack.display_layout.status = display::LayoutDecodeStatus::Valid;
    worker_ack.display_layout.layout = layout;
    const WorkerAck worker_ack_back = WorkerAck::from_body(worker_ack.to_body());
    VKR_CHECK(worker_ack_back.display_layout.status == display::LayoutDecodeStatus::Valid);
    VKR_CHECK_EQ(worker_ack_back.display_layout.layout.snapshot_id, layout.snapshot_id);
}

void test_select_response_round_trip() {
    GpuSelectResponse resp;
    resp.ok = true;
    resp.reason = "selected X";
    resp.device = probe_mocked().front();
    const std::string payload = encode_message(MessageType::GpuSelectResponse, resp.to_body());

    MessageType type = MessageType::Unknown;
    json::Value body;
    std::string err;
    VKR_CHECK(decode_message(payload, type, body, err));
    const GpuSelectResponse back = GpuSelectResponse::from_body(body);
    VKR_CHECK_EQ(back.ok, true);
    VKR_CHECK_EQ(back.device.name, resp.device.name);
}

void test_fractional_version_not_rounded() {
    // Via the wire path: a fractional protocol_version must not round to 1.
    const std::string payload =
        R"({"type":"hello","body":{"protocol_version":1.4,"app_instance_id":"x"}})";
    MessageType type = MessageType::Unknown;
    json::Value body;
    std::string err;
    VKR_CHECK(decode_message(payload, type, body, err));
    const ClientHello frac = ClientHello::from_body(body);
    VKR_CHECK_EQ(frac.protocol_version, 0); // rejected, falls back -- not 1

    // An exact integer still reads correctly.
    json::Value good = json::Value::make_object();
    good.set("protocol_version", json::Value(1));
    VKR_CHECK_EQ(ClientHello::from_body(good).protocol_version, 1);
}

// Sidecar plane: the sidecar handshake messages round-trip, and the additive
// sidecar_plane_port / sidecar_* fields on WorkerHello / SessionStarted round-trip while an
// absent field reads as 0/"" (an older worker/supervisor without a sidecar plane stays valid).
void test_sidecar_messages_round_trip() {
    VKR_CHECK(message_type_from_string("sidecar_hello") == MessageType::SidecarHello);
    VKR_CHECK(message_type_from_string("sidecar_ack") == MessageType::SidecarAck);
    VKR_CHECK_EQ(std::string(to_string(MessageType::SidecarHello)), std::string("sidecar_hello"));

    SidecarHello hello;
    hello.sidecar_token = "side-tok";
    const SidecarHello h_back = SidecarHello::from_body(hello.to_body());
    VKR_CHECK_EQ(h_back.sidecar_token, std::string("side-tok"));
    VKR_CHECK_EQ(h_back.protocol_version, kProtocolVersion);

    SidecarAck ack;
    ack.ok = true;
    ack.reason = "ok";
    ack.display_layout.status = display::LayoutDecodeStatus::Valid;
    ack.display_layout.layout.snapshot_id = "sidecar-snapshot";
    ack.display_layout.layout.virtual_bounds = {-1920, 0, 3840, 1080};
    ack.display_layout.layout.primary_monitor_id = "right";
    display::MonitorDesc left;
    left.stable_id = "left";
    left.device_name = "\\\\.\\DISPLAY1";
    left.bounds = {-1920, 0, 1920, 1080};
    left.work = left.bounds;
    left.dpi_x = left.dpi_y = 96;
    display::MonitorDesc right = left;
    right.stable_id = "right";
    right.device_name = "\\\\.\\DISPLAY2";
    right.bounds.x = 0;
    right.work = right.bounds;
    right.primary = true;
    ack.display_layout.layout.monitors = {left, right};
    const SidecarAck a_back = SidecarAck::from_body(ack.to_body());
    VKR_CHECK_EQ(a_back.ok, true);
    VKR_CHECK_EQ(a_back.reason, std::string("ok"));
    VKR_CHECK(a_back.display_layout.status == display::LayoutDecodeStatus::Valid);
    VKR_CHECK_EQ(a_back.display_layout.layout.snapshot_id, std::string("sidecar-snapshot"));
    VKR_CHECK_EQ(a_back.display_layout.layout.monitors.size(), static_cast<std::size_t>(2));
    VKR_CHECK(SidecarAck::from_body(json::Value::make_object()).display_layout.status ==
              display::LayoutDecodeStatus::Absent);

    WorkerHello wh;
    wh.worker_id = "wkr-1";
    wh.data_plane_port = 5000;
    wh.sidecar_plane_port = 6000;
    const WorkerHello wh_back = WorkerHello::from_body(wh.to_body());
    VKR_CHECK_EQ(wh_back.data_plane_port, 5000);
    VKR_CHECK_EQ(wh_back.sidecar_plane_port, 6000);
    // A legacy WorkerHello without the sidecar field reads it as 0 (no sidecar plane).
    json::Value legacy_wh = json::Value::make_object();
    legacy_wh.set("worker_id", json::Value(std::string("wkr-9")));
    legacy_wh.set("data_plane_port", json::Value(5001));
    VKR_CHECK_EQ(WorkerHello::from_body(legacy_wh).sidecar_plane_port, 0);

    SessionStarted ss;
    ss.worker_id = "wkr-1";
    ss.data_plane_port = 5000;
    ss.app_token = "app-t";
    ss.sidecar_plane_host = "127.0.0.1";
    ss.sidecar_plane_port = 6000;
    ss.sidecar_token = "side-t";
    ss.op_trace_path = "C:\\Temp\\vkrelay2-optrace-wkr-9.jsonl";
    ss.display_snapshot_id = "sup-1/display-7";
    const SessionStarted ss_back = SessionStarted::from_body(ss.to_body());
    VKR_CHECK_EQ(ss_back.sidecar_plane_host, std::string("127.0.0.1"));
    VKR_CHECK_EQ(ss_back.sidecar_plane_port, 6000);
    VKR_CHECK_EQ(ss_back.sidecar_token, std::string("side-t"));
    VKR_CHECK_EQ(ss_back.op_trace_path, std::string("C:\\Temp\\vkrelay2-optrace-wkr-9.jsonl"));
    VKR_CHECK_EQ(ss_back.display_snapshot_id, std::string("sup-1/display-7"));
    // A legacy SessionStarted without sidecar fields reads them as ""/0 (no sidecar plane).
    json::Value legacy_ss = json::Value::make_object();
    legacy_ss.set("worker_id", json::Value(std::string("wkr-9")));
    legacy_ss.set("data_plane_port", json::Value(5001));
    const SessionStarted ss_legacy = SessionStarted::from_body(legacy_ss);
    VKR_CHECK_EQ(ss_legacy.sidecar_plane_port, 0);
    VKR_CHECK(ss_legacy.sidecar_token.empty());
    VKR_CHECK(ss_legacy.op_trace_path.empty());
    VKR_CHECK(ss_legacy.display_snapshot_id.empty());
}

// LaunchSession.profile_enabled is ADDITIVE -- round-trips when set, and an old
// launcher's body (no field) decodes false, so a skewed pair degrades loudly at the smoke (which
// asserts both ends), never silently half-profiles.
void test_launch_session_profile_flag() {
    LaunchSession s;
    s.app_instance_id = "app-1";
    s.profile_enabled = true;
    s.display_snapshot_id = "sup-1/display-3";
    const LaunchSession rt = LaunchSession::from_body(s.to_body());
    VKR_CHECK(rt.profile_enabled);
    VKR_CHECK_EQ(rt.app_instance_id, std::string("app-1"));
    VKR_CHECK_EQ(rt.display_snapshot_id, std::string("sup-1/display-3"));
    json::Value old_body = json::Value::make_object(); // an old launcher never sets the field
    old_body.set("app_instance_id", json::Value(std::string("app-2")));
    VKR_CHECK(!LaunchSession::from_body(old_body).profile_enabled);
    VKR_CHECK(LaunchSession::from_body(old_body).display_snapshot_id.empty());

    s.op_trace_enabled = true;
    const LaunchSession trace_rt = LaunchSession::from_body(s.to_body());
    VKR_CHECK(trace_rt.op_trace_enabled);
    VKR_CHECK(!LaunchSession::from_body(old_body).op_trace_enabled);

    s.input_trace_enabled = true;
    const LaunchSession input_trace_rt = LaunchSession::from_body(s.to_body());
    VKR_CHECK(input_trace_rt.input_trace_enabled);
    VKR_CHECK(!LaunchSession::from_body(old_body).input_trace_enabled);
}

void test_malformed_rejected() {
    MessageType type = MessageType::Unknown;
    json::Value body;
    std::string err;
    VKR_CHECK(!decode_message("not json", type, body, err));
    VKR_CHECK(!decode_message("[1,2,3]", type, body, err));       // not an object
    VKR_CHECK(!decode_message("{\"body\":{}}", type, body, err)); // missing type
    // Well-formed but unrecognized type decodes to Unknown (graceful).
    VKR_CHECK(decode_message("{\"type\":\"from_the_future\",\"body\":{}}", type, body, err));
    VKR_CHECK(type == MessageType::Unknown);
}

} // namespace

int main() {
    test_type_strings();
    test_message_type_abi_golden();
    test_envelope_round_trip();
    test_close_session_round_trip();
    test_gpu_list_round_trip();
    test_server_hello_worker_backend();
    test_server_hello_host_display();
    test_server_hello_display_layout_codec();
    test_select_response_round_trip();
    test_fractional_version_not_rounded();
    test_sidecar_messages_round_trip();
    test_launch_session_profile_flag();
    test_malformed_rejected();
    return vkr::test::finish("unit_messages");
}
