#include "common/protocol/messages.hpp"

namespace vkr::protocol {
namespace {

// Tolerant readers: missing or wrong-typed fields fall back to a default so
// decoders never throw on a peer that omits or mistypes an optional field.
std::string get_string(const json::Value& obj, const std::string& key,
                       const std::string& fallback = "") {
    const json::Value* v = obj.find(key);
    return (v != nullptr && v->is_string()) ? v->as_string() : fallback;
}

// Integer fields must be exact integers: a fractional value (e.g. a protocol
// version of 1.4) falls back rather than silently rounding to 1.
int get_int(const json::Value& obj, const std::string& key, int fallback = 0) {
    const json::Value* v = obj.find(key);
    return (v != nullptr && v->is_integer()) ? static_cast<int>(v->as_int()) : fallback;
}

long long get_i64(const json::Value& obj, const std::string& key, long long fallback = 0) {
    const json::Value* v = obj.find(key);
    return (v != nullptr && v->is_integer()) ? v->as_int() : fallback;
}

bool get_bool(const json::Value& obj, const std::string& key, bool fallback = false) {
    const json::Value* v = obj.find(key);
    return (v != nullptr && v->is_bool()) ? v->as_bool() : fallback;
}

GpuDeviceType gpu_type_from_string(const std::string& s) {
    if (s == "discrete") {
        return GpuDeviceType::Discrete;
    }
    if (s == "integrated") {
        return GpuDeviceType::Integrated;
    }
    if (s == "cpu") {
        return GpuDeviceType::Cpu;
    }
    if (s == "virtual") {
        return GpuDeviceType::Virtual;
    }
    return GpuDeviceType::Other;
}

} // namespace

const char* to_string(MessageType type) {
    switch (type) {
    case MessageType::Hello:
        return "hello";
    case MessageType::HelloAck:
        return "hello_ack";
    case MessageType::Error:
        return "error";
    case MessageType::GpuListRequest:
        return "gpu_list_request";
    case MessageType::GpuListResponse:
        return "gpu_list_response";
    case MessageType::GpuSelectRequest:
        return "gpu_select_request";
    case MessageType::GpuSelectResponse:
        return "gpu_select_response";
    case MessageType::WorkerHello:
        return "worker_hello";
    case MessageType::WorkerAck:
        return "worker_ack";
    case MessageType::Heartbeat:
        return "heartbeat";
    case MessageType::LaunchSession:
        return "launch_session";
    case MessageType::SessionStarted:
        return "session_started";
    case MessageType::CloseSession:
        return "close_session";
    case MessageType::SessionClosed:
        return "session_closed";
    case MessageType::AppHello:
        return "app_hello";
    case MessageType::AppAck:
        return "app_ack";
    case MessageType::SidecarHello:
        return "sidecar_hello";
    case MessageType::SidecarAck:
        return "sidecar_ack";
    case MessageType::Unknown:
        return "unknown";
    }
    return "unknown";
}

MessageType message_type_from_string(const std::string& name) {
    if (name == "hello") {
        return MessageType::Hello;
    }
    if (name == "hello_ack") {
        return MessageType::HelloAck;
    }
    if (name == "error") {
        return MessageType::Error;
    }
    if (name == "gpu_list_request") {
        return MessageType::GpuListRequest;
    }
    if (name == "gpu_list_response") {
        return MessageType::GpuListResponse;
    }
    if (name == "gpu_select_request") {
        return MessageType::GpuSelectRequest;
    }
    if (name == "gpu_select_response") {
        return MessageType::GpuSelectResponse;
    }
    if (name == "worker_hello") {
        return MessageType::WorkerHello;
    }
    if (name == "worker_ack") {
        return MessageType::WorkerAck;
    }
    if (name == "heartbeat") {
        return MessageType::Heartbeat;
    }
    if (name == "launch_session") {
        return MessageType::LaunchSession;
    }
    if (name == "session_started") {
        return MessageType::SessionStarted;
    }
    if (name == "close_session") {
        return MessageType::CloseSession;
    }
    if (name == "session_closed") {
        return MessageType::SessionClosed;
    }
    if (name == "app_hello") {
        return MessageType::AppHello;
    }
    if (name == "app_ack") {
        return MessageType::AppAck;
    }
    if (name == "sidecar_hello") {
        return MessageType::SidecarHello;
    }
    if (name == "sidecar_ack") {
        return MessageType::SidecarAck;
    }
    return MessageType::Unknown;
}

std::string encode_message(MessageType type, const json::Value& body) {
    json::Value root = json::Value::make_object();
    root.set("type", json::Value(to_string(type)));
    root.set("body", body);
    return root.dump(0);
}

bool decode_message(const std::string& payload, MessageType& type, json::Value& body,
                    std::string& error) {
    json::Value root;
    if (!json::Value::try_parse(payload, root, error)) {
        return false;
    }
    if (!root.is_object()) {
        error = "message is not a JSON object";
        return false;
    }
    const json::Value* type_value = root.find("type");
    if (type_value == nullptr || !type_value->is_string()) {
        error = "message missing string 'type'";
        return false;
    }
    type = message_type_from_string(type_value->as_string());
    const json::Value* body_value = root.find("body");
    body = (body_value != nullptr) ? *body_value : json::Value::make_object();
    return true;
}

json::Value ClientHello::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("protocol_version", json::Value(protocol_version));
    b.set("app_instance_id", json::Value(app_instance_id));
    b.set("gpu_selector", json::Value(gpu_selector));
    b.set("display_backend", json::Value(display_backend));
    b.set("graphics_frontend", json::Value(graphics_frontend));
    return b;
}

ClientHello ClientHello::from_body(const json::Value& body) {
    ClientHello h;
    h.protocol_version = get_int(body, "protocol_version", 0);
    h.app_instance_id = get_string(body, "app_instance_id");
    h.gpu_selector = get_string(body, "gpu_selector", "auto");
    h.display_backend = get_string(body, "display_backend", "auto");
    h.graphics_frontend = get_string(body, "graphics_frontend", "auto");
    return h;
}

json::Value ServerHello::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("protocol_version", json::Value(protocol_version));
    b.set("supervisor_session_id", json::Value(supervisor_session_id));
    b.set("worker_id", json::Value(worker_id));
    b.set("worker_backend", json::Value(worker_backend));
    // Host display space (additive; 0 = unknown), omitted when all-zero so an old client sees an
    // unchanged body in the common no-probe case.
    if (host_display.work_w > 0 || host_display.work_h > 0 || host_display.dpi > 0) {
        b.set("host_work_w", json::Value(host_display.work_w));
        b.set("host_work_h", json::Value(host_display.work_h));
        b.set("host_dpi", json::Value(host_display.dpi));
    }
    // Producer-side validation happens in server_handshake before a live supervisor reaches
    // this serializer. Absent remains byte-for-byte compatible with legacy/non-Windows servers.
    if (display_layout.status == display::LayoutDecodeStatus::Valid) {
        b.set("display_layout", display::display_layout_to_json(display_layout.layout));
    }
    return b;
}

ServerHello ServerHello::from_body(const json::Value& body) {
    ServerHello h;
    h.protocol_version = get_int(body, "protocol_version", 0);
    h.supervisor_session_id = get_string(body, "supervisor_session_id");
    h.worker_id = get_string(body, "worker_id");
    // Absent/non-string -> "mock": never assume a daemon is real (the fail-closed direction).
    h.worker_backend = get_string(body, "worker_backend", "mock");
    // Host display space (additive): absent -> 0 = unknown (an old daemon), the keep-default
    // direction.
    h.host_display.work_w = get_int(body, "host_work_w", 0);
    h.host_display.work_h = get_int(body, "host_work_h", 0);
    h.host_display.dpi = get_int(body, "host_dpi", 0);
    h.display_layout = display::decode_display_layout_field(body, "display_layout");
    return h;
}

json::Value ErrorMsg::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("code", json::Value(code));
    b.set("message", json::Value(message));
    return b;
}

ErrorMsg ErrorMsg::from_body(const json::Value& body) {
    ErrorMsg e;
    e.code = get_string(body, "code");
    e.message = get_string(body, "message");
    return e;
}

json::Value GpuSelectRequest::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("selector", json::Value(selector));
    return b;
}

GpuSelectRequest GpuSelectRequest::from_body(const json::Value& body) {
    GpuSelectRequest r;
    r.selector = get_string(body, "selector", "auto");
    return r;
}

json::Value GpuSelectResponse::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("ok", json::Value(ok));
    b.set("reason", json::Value(reason));
    b.set("device", gpu_device_to_json(device));
    return b;
}

GpuSelectResponse GpuSelectResponse::from_body(const json::Value& body) {
    GpuSelectResponse r;
    r.ok = get_bool(body, "ok");
    r.reason = get_string(body, "reason");
    const json::Value* device = body.find("device");
    if (device != nullptr) {
        r.device = gpu_device_from_json(*device);
    }
    return r;
}

json::Value WorkerHello::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("worker_id", json::Value(worker_id));
    b.set("token", json::Value(token));
    b.set("data_plane_port", json::Value(data_plane_port));
    b.set("sidecar_plane_port", json::Value(sidecar_plane_port));
    return b;
}

WorkerHello WorkerHello::from_body(const json::Value& body) {
    WorkerHello h;
    h.worker_id = get_string(body, "worker_id");
    h.token = get_string(body, "token");
    h.data_plane_port = get_int(body, "data_plane_port");
    h.sidecar_plane_port = get_int(body, "sidecar_plane_port");
    return h;
}

json::Value WorkerAck::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("ok", json::Value(ok));
    b.set("reason", json::Value(reason));
    if (display_layout.status == display::LayoutDecodeStatus::Valid) {
        b.set("display_layout", display::display_layout_to_json(display_layout.layout));
    }
    return b;
}

WorkerAck WorkerAck::from_body(const json::Value& body) {
    WorkerAck a;
    a.ok = get_bool(body, "ok");
    a.reason = get_string(body, "reason");
    a.display_layout = display::decode_display_layout_field(body, "display_layout");
    return a;
}

json::Value Heartbeat::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("worker_id", json::Value(worker_id));
    b.set("seq", json::Value(seq));
    return b;
}

Heartbeat Heartbeat::from_body(const json::Value& body) {
    Heartbeat h;
    h.worker_id = get_string(body, "worker_id");
    h.seq = get_i64(body, "seq");
    return h;
}

json::Value LaunchSession::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("app_instance_id", json::Value(app_instance_id));
    b.set("gpu_selector", json::Value(gpu_selector));
    b.set("display_backend", json::Value(display_backend));
    b.set("graphics_frontend", json::Value(graphics_frontend));
    b.set("profile_enabled", json::Value(profile_enabled)); // additive
    b.set("op_trace_enabled", json::Value(op_trace_enabled));
    b.set("input_trace_enabled", json::Value(input_trace_enabled));
    if (!display_snapshot_id.empty()) {
        b.set("display_snapshot_id", json::Value(display_snapshot_id));
    }
    return b;
}

LaunchSession LaunchSession::from_body(const json::Value& body) {
    LaunchSession s;
    s.app_instance_id = get_string(body, "app_instance_id");
    s.gpu_selector = get_string(body, "gpu_selector", "auto");
    s.display_backend = get_string(body, "display_backend", "auto");
    s.graphics_frontend = get_string(body, "graphics_frontend", "auto");
    s.profile_enabled = get_bool(body, "profile_enabled"); // absent -> false (old launcher)
    s.op_trace_enabled = get_bool(body, "op_trace_enabled");
    s.input_trace_enabled = get_bool(body, "input_trace_enabled");
    s.display_snapshot_id = get_string(body, "display_snapshot_id");
    return s;
}

json::Value SessionStarted::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("worker_id", json::Value(worker_id));
    b.set("data_plane_host", json::Value(data_plane_host));
    b.set("data_plane_port", json::Value(data_plane_port));
    b.set("app_token", json::Value(app_token));
    b.set("sidecar_plane_host", json::Value(sidecar_plane_host));
    b.set("sidecar_plane_port", json::Value(sidecar_plane_port));
    b.set("sidecar_token", json::Value(sidecar_token));
    b.set("op_trace_path", json::Value(op_trace_path));
    if (!display_snapshot_id.empty()) {
        b.set("display_snapshot_id", json::Value(display_snapshot_id));
    }
    return b;
}

SessionStarted SessionStarted::from_body(const json::Value& body) {
    SessionStarted s;
    s.worker_id = get_string(body, "worker_id");
    s.data_plane_host = get_string(body, "data_plane_host");
    s.data_plane_port = get_int(body, "data_plane_port");
    s.app_token = get_string(body, "app_token");
    s.sidecar_plane_host = get_string(body, "sidecar_plane_host");
    s.sidecar_plane_port = get_int(body, "sidecar_plane_port");
    s.sidecar_token = get_string(body, "sidecar_token");
    s.op_trace_path = get_string(body, "op_trace_path");
    s.display_snapshot_id = get_string(body, "display_snapshot_id");
    return s;
}

json::Value CloseSession::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("worker_id", json::Value(worker_id));
    b.set("app_token", json::Value(app_token));
    return b;
}

CloseSession CloseSession::from_body(const json::Value& body) {
    CloseSession s;
    s.worker_id = get_string(body, "worker_id");
    s.app_token = get_string(body, "app_token");
    return s;
}

json::Value SessionClosed::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("worker_id", json::Value(worker_id));
    b.set("accepted", json::Value(accepted));
    b.set("reason", json::Value(reason));
    return b;
}

SessionClosed SessionClosed::from_body(const json::Value& body) {
    SessionClosed s;
    s.worker_id = get_string(body, "worker_id");
    s.accepted = get_bool(body, "accepted");
    s.reason = get_string(body, "reason");
    return s;
}

json::Value AppHello::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("app_token", json::Value(app_token));
    return b;
}

AppHello AppHello::from_body(const json::Value& body) {
    AppHello h;
    h.app_token = get_string(body, "app_token");
    return h;
}

json::Value AppAck::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("ok", json::Value(ok));
    b.set("reason", json::Value(reason));
    return b;
}

AppAck AppAck::from_body(const json::Value& body) {
    AppAck a;
    a.ok = get_bool(body, "ok");
    a.reason = get_string(body, "reason");
    return a;
}

json::Value SidecarHello::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("protocol_version", json::Value(protocol_version));
    b.set("sidecar_token", json::Value(sidecar_token));
    return b;
}

SidecarHello SidecarHello::from_body(const json::Value& body) {
    SidecarHello h;
    h.protocol_version = get_int(body, "protocol_version", 0);
    h.sidecar_token = get_string(body, "sidecar_token");
    return h;
}

json::Value SidecarAck::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("ok", json::Value(ok));
    b.set("reason", json::Value(reason));
    if (display_layout.status == display::LayoutDecodeStatus::Valid) {
        const display::ValidationResult valid =
            display::validate_display_layout(display_layout.layout);
        if (valid.ok) {
            b.set("display_layout", display::display_layout_to_json(display_layout.layout));
        }
    }
    return b;
}

SidecarAck SidecarAck::from_body(const json::Value& body) {
    SidecarAck a;
    a.ok = get_bool(body, "ok");
    a.reason = get_string(body, "reason");
    a.display_layout = display::decode_display_layout_field(body, "display_layout");
    return a;
}

json::Value gpu_device_to_json(const GpuDevice& device) {
    json::Value v = json::Value::make_object();
    v.set("index", json::Value(device.index));
    v.set("name", json::Value(device.name));
    v.set("vendor_id", json::Value(static_cast<long long>(device.vendor_id)));
    v.set("device_id", json::Value(static_cast<long long>(device.device_id)));
    v.set("type", json::Value(to_string(device.type)));
    v.set("api_version", json::Value(device.api_version));
    v.set("driver_name", json::Value(device.driver_name));
    v.set("luid", json::Value(device.luid));
    v.set("usable", json::Value(device.usable));
    v.set("reason", json::Value(device.reason));
    json::Array roles;
    for (const auto& role : device.roles) {
        roles.emplace_back(role);
    }
    v.set("roles", json::Value(std::move(roles)));
    v.set("default_high_performance", json::Value(device.default_high_performance));
    v.set("default_integrated", json::Value(device.default_integrated));
    return v;
}

GpuDevice gpu_device_from_json(const json::Value& value) {
    GpuDevice d;
    d.index = get_int(value, "index");
    d.name = get_string(value, "name");
    d.vendor_id = static_cast<std::uint32_t>(get_i64(value, "vendor_id"));
    d.device_id = static_cast<std::uint32_t>(get_i64(value, "device_id"));
    d.type = gpu_type_from_string(get_string(value, "type", "other"));
    d.api_version = get_string(value, "api_version");
    d.driver_name = get_string(value, "driver_name");
    d.luid = get_string(value, "luid");
    d.usable = get_bool(value, "usable");
    d.reason = get_string(value, "reason");
    const json::Value* roles = value.find("roles");
    if (roles != nullptr && roles->is_array()) {
        for (const auto& role : roles->as_array()) {
            if (role.is_string()) {
                d.roles.push_back(role.as_string());
            }
        }
    }
    d.default_high_performance = get_bool(value, "default_high_performance");
    d.default_integrated = get_bool(value, "default_integrated");
    return d;
}

json::Value gpu_list_to_body(const std::vector<GpuDevice>& devices, bool real) {
    json::Value body = json::Value::make_object();
    json::Array arr;
    for (const auto& device : devices) {
        arr.emplace_back(gpu_device_to_json(device));
    }
    body.set("devices", json::Value(std::move(arr)));
    // Provenance marker: real=false says this list is the mocked fallback, not host
    // enumeration -- consumers must not present it as hardware. Additive field: an old
    // client ignores it; an old daemon omits it (see gpu_list_marked_mock).
    body.set("real", json::Value(real));
    return body;
}

bool gpu_list_marked_mock(const json::Value& body) {
    // Only an EXPLICIT real=false marks the list as mock. An absent field is a legacy
    // daemon whose provenance is unknown -- treated as real so a mixed-build pair keeps
    // working (the mock tells are still visible in the entries' "(mock)" driver names).
    const json::Value* real = body.find("real");
    return real != nullptr && real->is_bool() && !real->as_bool();
}

std::vector<GpuDevice> gpu_list_from_body(const json::Value& body) {
    std::vector<GpuDevice> devices;
    const json::Value* arr = body.find("devices");
    if (arr != nullptr && arr->is_array()) {
        for (const auto& entry : arr->as_array()) {
            devices.push_back(gpu_device_from_json(entry));
        }
    }
    return devices;
}

} // namespace vkr::protocol
