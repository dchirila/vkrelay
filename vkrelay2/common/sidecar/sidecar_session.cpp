#include "common/sidecar/sidecar_session.hpp"

#include "common/logging/logging.hpp"
#include "common/protocol/wire.hpp"

namespace vkr::sidecar {
namespace {

constexpr char kComponent[] = "vkrpc-sidecar";

// Tolerant readers (mirror the protocol/vkrpc decoders): missing/mistyped fields fall back.
int get_int(const json::Value& obj, const std::string& key, int fallback) {
    const json::Value* v = obj.find(key);
    return (v != nullptr && v->is_integer()) ? static_cast<int>(v->as_int()) : fallback;
}

// Reads the full 64-bit JSON integer WITHOUT narrowing -- the caller range-checks before assigning
// to a narrower field, so an out-of-range header value is rejected, not silently truncated.
long long get_i64(const json::Value& obj, const std::string& key, long long fallback) {
    const json::Value* v = obj.find(key);
    return (v != nullptr && v->is_integer()) ? v->as_int() : fallback;
}

bool get_bool(const json::Value& obj, const std::string& key, bool fallback) {
    const json::Value* v = obj.find(key);
    return (v != nullptr && v->is_bool()) ? v->as_bool() : fallback;
}

std::string get_string(const json::Value& obj, const std::string& key,
                       const std::string& fallback = "") {
    const json::Value* v = obj.find(key);
    return (v != nullptr && v->is_string()) ? v->as_string() : fallback;
}

// Decimal-string u64 (the project's no-narrowing handle convention; mirrors vulkan_session.cpp):
// xid/generation ride as strings so a 64-bit value is never truncated. A non-decimal / overflow /
// absent value reads 0.
std::uint64_t parse_handle_str(const std::string& s) {
    if (s.empty()) {
        return 0;
    }
    for (const char c : s) {
        if (c < '0' || c > '9') {
            return 0;
        }
    }
    try {
        std::size_t pos = 0;
        const unsigned long long value = std::stoull(s, &pos);
        return pos == s.size() ? static_cast<std::uint64_t>(value) : 0;
    } catch (const std::exception&) {
        return 0;
    }
}
json::Value handle_value(std::uint64_t handle) {
    return json::Value(std::to_string(handle));
}
std::uint64_t get_handle(const json::Value& obj, const std::string& key) {
    const json::Value* v = obj.find(key);
    return (v != nullptr && v->is_string()) ? parse_handle_str(v->as_string()) : 0;
}

} // namespace

json::Value SidecarNegotiateRequest::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("protocol_version", json::Value(protocol_version));
    return b;
}

SidecarNegotiateRequest SidecarNegotiateRequest::from_body(const json::Value& body) {
    SidecarNegotiateRequest r;
    r.protocol_version = get_int(body, "protocol_version", 0);
    return r;
}

json::Value SidecarNegotiateResponse::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("ok", json::Value(ok));
    b.set("protocol_version", json::Value(protocol_version));
    b.set("reason", json::Value(reason));
    return b;
}

SidecarNegotiateResponse SidecarNegotiateResponse::from_body(const json::Value& body) {
    SidecarNegotiateResponse r;
    r.ok = get_bool(body, "ok", false);
    r.protocol_version = get_int(body, "protocol_version", 0);
    r.reason = get_string(body, "reason");
    return r;
}

json::Value SidecarReadyRequest::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("scan_generation", json::Value(scan_generation));
    b.set("has_xcomposite", json::Value(has_xcomposite));
    b.set("has_xtest", json::Value(has_xtest));
    b.set("has_xfixes", json::Value(has_xfixes));
    b.set("initial_toplevels", json::Value(initial_toplevels));
    b.set("root_width", json::Value(static_cast<int>(root_width)));
    b.set("root_height", json::Value(static_cast<int>(root_height)));
    if (!display_snapshot_id.empty()) {
        b.set("display_snapshot_id", json::Value(display_snapshot_id));
    }
    return b;
}

SidecarReadyRequest SidecarReadyRequest::from_body(const json::Value& body) {
    SidecarReadyRequest r;
    r.scan_generation = get_int(body, "scan_generation", 0);
    r.has_xcomposite = get_bool(body, "has_xcomposite", false);
    r.has_xtest = get_bool(body, "has_xtest", false);
    r.has_xfixes = get_bool(body, "has_xfixes", false);
    r.initial_toplevels = get_int(body, "initial_toplevels", 0);
    r.root_width = static_cast<std::uint32_t>(get_int(body, "root_width", 0));
    r.root_height = static_cast<std::uint32_t>(get_int(body, "root_height", 0));
    r.display_snapshot_id = get_string(body, "display_snapshot_id");
    return r;
}

json::Value SidecarReadyResponse::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("ok", json::Value(ok));
    b.set("reason", json::Value(reason));
    return b;
}

SidecarReadyResponse SidecarReadyResponse::from_body(const json::Value& body) {
    SidecarReadyResponse r;
    r.ok = get_bool(body, "ok", false);
    r.reason = get_string(body, "reason");
    return r;
}

json::Value SidecarRegisterToplevelRequest::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("xid", handle_value(xid));
    b.set("generation", handle_value(generation));
    b.set("role", json::Value(role));
    b.set("title", json::Value(title));
    b.set("x", json::Value(x));
    b.set("y", json::Value(y));
    b.set("width", json::Value(static_cast<int>(width)));
    b.set("height", json::Value(static_cast<int>(height)));
    b.set("override_redirect", json::Value(override_redirect));
    b.set("is_popup", json::Value(is_popup));
    b.set("owner_xid", handle_value(owner_xid));
    b.set("popup_kind", json::Value(static_cast<int>(popup_kind)));
    return b;
}

SidecarRegisterToplevelRequest SidecarRegisterToplevelRequest::from_body(const json::Value& body) {
    SidecarRegisterToplevelRequest r;
    r.xid = get_handle(body, "xid");
    r.generation = get_handle(body, "generation");
    r.role = get_string(body, "role");
    r.title = get_string(body, "title");
    r.x = get_int(body, "x", 0);
    r.y = get_int(body, "y", 0);
    r.width = static_cast<std::uint32_t>(get_int(body, "width", 0));
    r.height = static_cast<std::uint32_t>(get_int(body, "height", 0));
    r.override_redirect = get_bool(body, "override_redirect", false);
    r.is_popup = get_bool(body, "is_popup", false);
    r.owner_xid = get_handle(body, "owner_xid");
    r.popup_kind = static_cast<std::uint32_t>(get_int(body, "popup_kind", 0));
    return r;
}

json::Value SidecarUpdateToplevelRequest::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("xid", handle_value(xid));
    b.set("generation", handle_value(generation));
    b.set("role", json::Value(role));
    b.set("x", json::Value(x));
    b.set("y", json::Value(y));
    b.set("width", json::Value(static_cast<int>(width)));
    b.set("height", json::Value(static_cast<int>(height)));
    b.set("z_order", json::Value(static_cast<int>(z_order)));
    return b;
}

SidecarUpdateToplevelRequest SidecarUpdateToplevelRequest::from_body(const json::Value& body) {
    SidecarUpdateToplevelRequest r;
    r.xid = get_handle(body, "xid");
    r.generation = get_handle(body, "generation");
    r.role = get_string(body, "role");
    r.x = get_int(body, "x", 0);
    r.y = get_int(body, "y", 0);
    r.width = static_cast<std::uint32_t>(get_int(body, "width", 0));
    r.height = static_cast<std::uint32_t>(get_int(body, "height", 0));
    r.z_order = static_cast<std::uint32_t>(get_int(body, "z_order", 0));
    return r;
}

json::Value SidecarUnregisterToplevelRequest::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("xid", handle_value(xid));
    b.set("generation", handle_value(generation));
    return b;
}

SidecarUnregisterToplevelRequest
SidecarUnregisterToplevelRequest::from_body(const json::Value& body) {
    SidecarUnregisterToplevelRequest r;
    r.xid = get_handle(body, "xid");
    r.generation = get_handle(body, "generation");
    return r;
}

json::Value SidecarSetVisibilityRequest::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("xid", handle_value(xid));
    b.set("generation", handle_value(generation));
    b.set("visibility_state", json::Value(static_cast<int>(visibility_state)));
    return b;
}

SidecarSetVisibilityRequest SidecarSetVisibilityRequest::from_body(const json::Value& body) {
    SidecarSetVisibilityRequest r;
    r.xid = get_handle(body, "xid");
    r.generation = get_handle(body, "generation");
    r.visibility_state = static_cast<std::uint32_t>(get_int(body, "visibility_state", 0));
    return r;
}

json::Value SidecarToplevelResponse::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("ok", json::Value(ok));
    b.set("reason", json::Value(reason));
    b.set("xid", handle_value(xid));
    b.set("applied", json::Value(applied));
    b.set("representation", json::Value(representation));
    b.set("epoch", handle_value(epoch));
    return b;
}

SidecarToplevelResponse SidecarToplevelResponse::from_body(const json::Value& body) {
    SidecarToplevelResponse r;
    r.ok = get_bool(body, "ok", false);
    r.reason = get_string(body, "reason");
    r.xid = get_handle(body, "xid");
    r.applied = get_bool(body, "applied", false);
    r.representation = get_string(body, "representation", "none");
    r.epoch = get_handle(body, "epoch");
    return r;
}

std::string SidecarPaintChromeRequest::to_wire() const {
    json::Value h = json::Value::make_object();
    h.set("xid", handle_value(xid));
    h.set("lifecycle_generation", handle_value(lifecycle_generation));
    h.set("seq", handle_value(seq));
    h.set("src_w", handle_value(src_w));
    h.set("src_h", handle_value(src_h));
    h.set("dirty_x", json::Value(dirty_x));
    h.set("dirty_y", json::Value(dirty_y));
    h.set("dirty_w", handle_value(dirty_w));
    h.set("dirty_h", handle_value(dirty_h));
    h.set("stride", handle_value(stride));
    h.set("format", json::Value(format));
    h.set("payload_size", handle_value(static_cast<std::uint64_t>(pixels.size())));
    const std::string json = h.dump(0);
    std::string out(4, '\0');
    protocol::store_le32(static_cast<std::uint32_t>(json.size()),
                         reinterpret_cast<unsigned char*>(&out[0]));
    out += json;
    out += pixels;
    return out;
}

SidecarPaintChromeRequest SidecarPaintChromeRequest::from_wire(const std::string& body,
                                                               std::string& err) {
    err.clear();
    if (body.size() < 4) {
        err = "paint_chrome body shorter than 4-byte length prefix";
        return SidecarPaintChromeRequest{};
    }
    const std::uint32_t json_len =
        protocol::load_le32(reinterpret_cast<const unsigned char*>(body.data()));
    if (json_len > kMaxAuxChromeJsonHeaderBytes) {
        err = "paint_chrome json header exceeds cap";
        return SidecarPaintChromeRequest{};
    }
    if (static_cast<std::size_t>(4) + json_len > body.size()) {
        err = "paint_chrome json header runs past end of body";
        return SidecarPaintChromeRequest{};
    }
    json::Value h;
    std::string jerr;
    if (!json::Value::try_parse(body.substr(4, json_len), h, jerr)) {
        err = "paint_chrome json header parse: " + jerr;
        return SidecarPaintChromeRequest{};
    }
    SidecarPaintChromeRequest r;
    r.xid = get_handle(h, "xid");
    r.lifecycle_generation = get_handle(h, "lifecycle_generation");
    r.seq = get_handle(h, "seq");
    r.format = get_int(h, "format", 0);
    // Read the signed origins WIDE (int64) and range-check before narrowing to int32: get_int would
    // narrow with static_cast<int>, so a header origin like 4294967296 would truncate to a
    // plausible small/zero int and pass the dirty-rect checks (a malformed body -> a wrong paint).
    // Reject < 0 or > INT_MAX instead.
    const long long dirty_x_raw = get_i64(h, "dirty_x", 0);
    const long long dirty_y_raw = get_i64(h, "dirty_y", 0);
    if (dirty_x_raw < 0 || dirty_x_raw > 0x7FFFFFFFLL || dirty_y_raw < 0 ||
        dirty_y_raw > 0x7FFFFFFFLL) {
        err = "paint_chrome dirty origin out of range";
        return SidecarPaintChromeRequest{};
    }
    r.dirty_x = static_cast<std::int32_t>(dirty_x_raw);
    r.dirty_y = static_cast<std::int32_t>(dirty_y_raw);
    // All sizes decode wide (u64) so the bounds arithmetic below cannot overflow; each is range-
    // checked to u32 before it lands in the struct.
    const std::uint64_t src_w = get_handle(h, "src_w");
    const std::uint64_t src_h = get_handle(h, "src_h");
    const std::uint64_t dirty_w = get_handle(h, "dirty_w");
    const std::uint64_t dirty_h = get_handle(h, "dirty_h");
    const std::uint64_t stride = get_handle(h, "stride");
    const std::uint64_t payload_size = get_handle(h, "payload_size");

    // Decoder discipline: reject every malformed body BEFORE the GDI path.
    if (r.format != kAuxChromeFormatBgra8) {
        err = "paint_chrome unknown pixel format";
        return SidecarPaintChromeRequest{};
    }
    if (src_w == 0 || src_h == 0 || dirty_w == 0 || dirty_h == 0) {
        err = "paint_chrome zero dimension";
        return SidecarPaintChromeRequest{};
    }
    // Each size feeds an `int` (the Win32 DIB / paint API, and the worker's source-buffer
    // indexing), so cap at INT_MAX -- not just u32 -- to forbid a value that would truncate to a
    // negative int.
    if (src_w > 0x7FFFFFFFull || src_h > 0x7FFFFFFFull || dirty_w > 0x7FFFFFFFull ||
        dirty_h > 0x7FFFFFFFull || stride > 0x7FFFFFFFull) {
        err = "paint_chrome dimension exceeds INT_MAX";
        return SidecarPaintChromeRequest{};
    }
    // Bound the FULL source backing store the worker allocates (src_w*src_h*4), not just the dirty
    // payload -- otherwise a 1x1 dirty rect could smuggle a UINT32_MAX source size past the payload
    // cap and drive a huge allocation / size_t overflow. With each dim <= INT_MAX, src_w*src_h fits
    // in u64 with no overflow; compare against (cap / 4) to bound the *4 without overflowing.
    if (src_w * src_h >
        static_cast<std::uint64_t>(kMaxAuxChromeBackingStoreBytes) / kAuxChromeBytesPerPixel) {
        err = "paint_chrome source backing store exceeds the cap";
        return SidecarPaintChromeRequest{};
    }
    // Wide-arithmetic bounds: the dirty rect must lie within the source.
    if (static_cast<std::uint64_t>(r.dirty_x) + dirty_w > src_w ||
        static_cast<std::uint64_t>(r.dirty_y) + dirty_h > src_h) {
        err = "paint_chrome dirty rect outside source";
        return SidecarPaintChromeRequest{};
    }
    if (stride < dirty_w * static_cast<std::uint64_t>(kAuxChromeBytesPerPixel)) {
        err = "paint_chrome stride smaller than a dirty row";
        return SidecarPaintChromeRequest{};
    }
    if (payload_size > kMaxAuxChromeWirePayloadBytes) {
        err = "paint_chrome payload exceeds the per-frame wire cap";
        return SidecarPaintChromeRequest{};
    }
    if (payload_size != stride * dirty_h) {
        err = "paint_chrome payload_size does not match stride * dirty_h";
        return SidecarPaintChromeRequest{};
    }
    const std::size_t tail_len = body.size() - 4 - json_len;
    if (tail_len != payload_size) {
        err = "paint_chrome tail length does not match payload_size";
        return SidecarPaintChromeRequest{};
    }
    r.src_w = static_cast<std::uint32_t>(src_w);
    r.src_h = static_cast<std::uint32_t>(src_h);
    r.dirty_w = static_cast<std::uint32_t>(dirty_w);
    r.dirty_h = static_cast<std::uint32_t>(dirty_h);
    r.stride = static_cast<std::uint32_t>(stride);
    r.pixels.assign(body, static_cast<std::size_t>(4) + json_len, std::string::npos);
    return r;
}

json::Value SidecarPaintResponse::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("ok", json::Value(ok));
    b.set("reason", json::Value(reason));
    b.set("xid", handle_value(xid));
    b.set("applied", json::Value(applied));
    b.set("representation", json::Value(representation));
    b.set("shown", json::Value(shown));
    b.set("last_seq", handle_value(last_seq));
    return b;
}

SidecarPaintResponse SidecarPaintResponse::from_body(const json::Value& body) {
    SidecarPaintResponse r;
    r.ok = get_bool(body, "ok", false);
    r.reason = get_string(body, "reason");
    r.xid = get_handle(body, "xid");
    r.applied = get_bool(body, "applied", false);
    r.representation = get_string(body, "representation", "none");
    r.shown = get_bool(body, "shown", false);
    r.last_seq = get_handle(body, "last_seq");
    return r;
}

json::Value SidecarDebugChromeStateRequest::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("xid", handle_value(xid));
    b.set("sample_x", json::Value(sample_x));
    b.set("sample_y", json::Value(sample_y));
    return b;
}

SidecarDebugChromeStateRequest SidecarDebugChromeStateRequest::from_body(const json::Value& body) {
    SidecarDebugChromeStateRequest r;
    r.xid = get_handle(body, "xid");
    r.sample_x = get_int(body, "sample_x", 0);
    r.sample_y = get_int(body, "sample_y", 0);
    return r;
}

json::Value SidecarDebugChromeStateResponse::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("ok", json::Value(ok));
    b.set("reason", json::Value(reason));
    b.set("xid", handle_value(xid));
    b.set("representation", json::Value(representation));
    b.set("shown", json::Value(shown));
    b.set("last_seq", handle_value(last_seq));
    b.set("has_pixel", json::Value(has_pixel));
    b.set("pixel_bgra", handle_value(pixel_bgra));
    return b;
}

SidecarDebugChromeStateResponse
SidecarDebugChromeStateResponse::from_body(const json::Value& body) {
    SidecarDebugChromeStateResponse r;
    r.ok = get_bool(body, "ok", false);
    r.reason = get_string(body, "reason");
    r.xid = get_handle(body, "xid");
    r.representation = get_string(body, "representation", "none");
    r.shown = get_bool(body, "shown", false);
    r.last_seq = get_handle(body, "last_seq");
    r.has_pixel = get_bool(body, "has_pixel", false);
    r.pixel_bgra = static_cast<std::uint32_t>(get_handle(body, "pixel_bgra"));
    return r;
}

json::Value SidecarInputEvent::to_value() const {
    json::Value b = json::Value::make_object();
    b.set("xid", handle_value(xid));
    b.set("epoch", handle_value(epoch));
    b.set("seq", handle_value(seq));
    b.set("kind", json::Value(static_cast<int>(kind)));
    b.set("client_x", json::Value(client_x));
    b.set("client_y", json::Value(client_y));
    b.set("button", json::Value(button));
    b.set("wheel", json::Value(wheel));
    b.set("vk", json::Value(vk));
    b.set("scancode", json::Value(scancode));
    b.set("modifiers", json::Value(static_cast<int>(modifiers)));
    b.set("pressed", json::Value(pressed));
    b.set("root_x", json::Value(root_x)); // GeometryRequest fields (0 for other kinds)
    b.set("root_y", json::Value(root_y));
    b.set("req_w", json::Value(static_cast<int>(req_w)));
    b.set("req_h", json::Value(static_cast<int>(req_h)));
    b.set("host_request", json::Value(static_cast<int>(host_request)));
    return b;
}

SidecarInputEvent SidecarInputEvent::from_value(const json::Value& v) {
    SidecarInputEvent e;
    e.xid = get_handle(v, "xid");
    e.epoch = get_handle(v, "epoch");
    e.seq = get_handle(v, "seq");
    // kind/coords/etc decode tolerantly; an out-of-enum kind stays a numeric value and is dropped
    // by the consumer (never injected). Coords carry signed (a click just off the client edge is
    // legal).
    e.kind = static_cast<std::uint32_t>(get_int(v, "kind", 0));
    e.client_x = get_int(v, "client_x", 0);
    e.client_y = get_int(v, "client_y", 0);
    e.button = get_int(v, "button", 0);
    e.wheel = get_int(v, "wheel", 0);
    e.vk = get_int(v, "vk", 0);
    e.scancode = get_int(v, "scancode", 0);
    e.modifiers = static_cast<std::uint32_t>(get_int(v, "modifiers", 0));
    e.pressed = get_bool(v, "pressed", false);
    e.root_x = get_int(v, "root_x", 0); // GeometryRequest fields (tolerant; 0 default)
    e.root_y = get_int(v, "root_y", 0);
    e.req_w = static_cast<std::uint32_t>(get_int(v, "req_w", 0));
    e.req_h = static_cast<std::uint32_t>(get_int(v, "req_h", 0));
    e.host_request = static_cast<std::uint32_t>(get_int(v, "host_request", 0));
    return e;
}

json::Value SidecarPollInputRequest::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("since_seq", handle_value(since_seq));
    return b;
}

SidecarPollInputRequest SidecarPollInputRequest::from_body(const json::Value& body) {
    SidecarPollInputRequest r;
    r.since_seq = get_handle(body, "since_seq");
    return r;
}

json::Value SidecarPollInputResponse::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("ok", json::Value(ok));
    b.set("reason", json::Value(reason));
    json::Array arr;
    for (const auto& e : events) {
        arr.emplace_back(e.to_value());
    }
    b.set("events", json::Value(std::move(arr)));
    b.set("next_seq", handle_value(next_seq));
    b.set("dropped", json::Value(dropped));
    return b;
}

SidecarPollInputResponse SidecarPollInputResponse::from_body(const json::Value& body) {
    SidecarPollInputResponse r;
    r.ok = get_bool(body, "ok", false);
    r.reason = get_string(body, "reason");
    const json::Value* arr = body.find("events");
    if (arr != nullptr && arr->is_array()) {
        for (const auto& e : arr->as_array()) {
            r.events.push_back(SidecarInputEvent::from_value(e));
        }
    }
    r.next_seq = get_handle(body, "next_seq");
    r.dropped = get_bool(body, "dropped", false);
    return r;
}

json::Value SidecarDebugEnqueueInputRequest::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("xid", handle_value(xid));
    b.set("epoch", handle_value(epoch));
    json::Array arr;
    for (const auto& e : events) {
        arr.emplace_back(e.to_value());
    }
    b.set("events", json::Value(std::move(arr)));
    return b;
}

SidecarDebugEnqueueInputRequest
SidecarDebugEnqueueInputRequest::from_body(const json::Value& body) {
    SidecarDebugEnqueueInputRequest r;
    r.xid = get_handle(body, "xid");
    r.epoch = get_handle(body, "epoch");
    const json::Value* arr = body.find("events");
    if (arr != nullptr && arr->is_array()) {
        // Bound what one debug request can stuff into the ring (the ring itself also caps; this
        // just stops a single message from forcing a huge response object).
        for (const auto& e : arr->as_array()) {
            if (r.events.size() >= kMaxInputQueueEvents) {
                break;
            }
            r.events.push_back(SidecarInputEvent::from_value(e));
        }
    }
    return r;
}

json::Value SidecarDebugEnqueueInputResponse::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("ok", json::Value(ok));
    b.set("reason", json::Value(reason));
    b.set("xid", handle_value(xid));
    b.set("enqueued", json::Value(enqueued));
    return b;
}

SidecarDebugEnqueueInputResponse
SidecarDebugEnqueueInputResponse::from_body(const json::Value& body) {
    SidecarDebugEnqueueInputResponse r;
    r.ok = get_bool(body, "ok", false);
    r.reason = get_string(body, "reason");
    r.xid = get_handle(body, "xid");
    r.enqueued = get_int(body, "enqueued", 0);
    return r;
}

std::string SidecarSetCursorRequest::to_wire() const {
    json::Value h = json::Value::make_object();
    h.set("xid", handle_value(xid));
    h.set("epoch", handle_value(epoch));
    h.set("width", handle_value(width));
    h.set("height", handle_value(height));
    h.set("xhot", json::Value(xhot));
    h.set("yhot", json::Value(yhot));
    h.set("format", json::Value(format));
    h.set("payload_size", handle_value(static_cast<std::uint64_t>(pixels.size())));
    const std::string json = h.dump(0);
    std::string out(4, '\0');
    protocol::store_le32(static_cast<std::uint32_t>(json.size()),
                         reinterpret_cast<unsigned char*>(&out[0]));
    out += json;
    out += pixels;
    return out;
}

SidecarSetCursorRequest SidecarSetCursorRequest::from_wire(const std::string& body,
                                                           std::string& err) {
    err.clear();
    if (body.size() < 4) {
        err = "set_cursor body shorter than 4-byte length prefix";
        return SidecarSetCursorRequest{};
    }
    const std::uint32_t json_len =
        protocol::load_le32(reinterpret_cast<const unsigned char*>(body.data()));
    if (json_len > kMaxCursorJsonHeaderBytes) {
        err = "set_cursor json header exceeds cap";
        return SidecarSetCursorRequest{};
    }
    if (static_cast<std::size_t>(4) + json_len > body.size()) {
        err = "set_cursor json header runs past end of body";
        return SidecarSetCursorRequest{};
    }
    json::Value h;
    std::string jerr;
    if (!json::Value::try_parse(body.substr(4, json_len), h, jerr)) {
        err = "set_cursor json header parse: " + jerr;
        return SidecarSetCursorRequest{};
    }
    SidecarSetCursorRequest r;
    r.xid = get_handle(h, "xid");
    r.epoch = get_handle(h, "epoch");
    r.format = get_int(h, "format", 0);
    // Read dims wide + range-check before narrowing (mirrors PaintChrome's discipline).
    const std::uint64_t width = get_handle(h, "width");
    const std::uint64_t height = get_handle(h, "height");
    const std::int64_t xhot_raw = get_i64(h, "xhot", 0);
    const std::int64_t yhot_raw = get_i64(h, "yhot", 0);
    const std::uint64_t payload_size = get_handle(h, "payload_size");
    if (r.format != kCursorFormatBgra8) {
        err = "set_cursor unknown pixel format";
        return SidecarSetCursorRequest{};
    }
    if (width == 0 || height == 0) {
        err = "set_cursor zero dimension";
        return SidecarSetCursorRequest{};
    }
    if (width > kMaxCursorDim || height > kMaxCursorDim) {
        err = "set_cursor dimension exceeds the cap";
        return SidecarSetCursorRequest{};
    }
    // Hotspot must lie within the image (a valid X cursor's always does); rejects a narrowing/OOB.
    if (xhot_raw < 0 || xhot_raw >= static_cast<std::int64_t>(width) || yhot_raw < 0 ||
        yhot_raw >= static_cast<std::int64_t>(height)) {
        err = "set_cursor hotspot out of range";
        return SidecarSetCursorRequest{};
    }
    const std::uint64_t expect = width * height * 4; // each dim <= 256 -> no overflow
    if (payload_size != expect) {
        err = "set_cursor payload_size does not match width*height*4";
        return SidecarSetCursorRequest{};
    }
    if (payload_size > kMaxCursorPayloadBytes) {
        err = "set_cursor payload exceeds the cap";
        return SidecarSetCursorRequest{};
    }
    const std::size_t tail_len = body.size() - 4 - json_len;
    if (tail_len != payload_size) {
        err = "set_cursor tail length does not match payload_size";
        return SidecarSetCursorRequest{};
    }
    r.width = static_cast<std::uint32_t>(width);
    r.height = static_cast<std::uint32_t>(height);
    r.xhot = static_cast<std::int32_t>(xhot_raw);
    r.yhot = static_cast<std::int32_t>(yhot_raw);
    r.pixels.assign(body, static_cast<std::size_t>(4) + json_len, std::string::npos);
    return r;
}

json::Value SidecarSetCursorResponse::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("ok", json::Value(ok));
    b.set("reason", json::Value(reason));
    b.set("xid", handle_value(xid));
    b.set("applied", json::Value(applied));
    return b;
}

SidecarSetCursorResponse SidecarSetCursorResponse::from_body(const json::Value& body) {
    SidecarSetCursorResponse r;
    r.ok = get_bool(body, "ok", false);
    r.reason = get_string(body, "reason");
    r.xid = get_handle(body, "xid");
    r.applied = get_bool(body, "applied", false);
    return r;
}

json::Value SidecarDebugCursorStateRequest::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("xid", handle_value(xid));
    b.set("sample_x", json::Value(sample_x));
    b.set("sample_y", json::Value(sample_y));
    return b;
}

SidecarDebugCursorStateRequest SidecarDebugCursorStateRequest::from_body(const json::Value& body) {
    SidecarDebugCursorStateRequest r;
    r.xid = get_handle(body, "xid");
    r.sample_x = get_int(body, "sample_x", 0);
    r.sample_y = get_int(body, "sample_y", 0);
    return r;
}

json::Value SidecarDebugCursorStateResponse::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("ok", json::Value(ok));
    b.set("reason", json::Value(reason));
    b.set("xid", handle_value(xid));
    b.set("has_cursor", json::Value(has_cursor));
    b.set("width", handle_value(width));
    b.set("height", handle_value(height));
    b.set("xhot", json::Value(xhot));
    b.set("yhot", json::Value(yhot));
    b.set("has_pixel", json::Value(has_pixel));
    b.set("pixel_bgra", handle_value(pixel_bgra));
    return b;
}

SidecarDebugCursorStateResponse
SidecarDebugCursorStateResponse::from_body(const json::Value& body) {
    SidecarDebugCursorStateResponse r;
    r.ok = get_bool(body, "ok", false);
    r.reason = get_string(body, "reason");
    r.xid = get_handle(body, "xid");
    r.has_cursor = get_bool(body, "has_cursor", false);
    r.width = static_cast<std::uint32_t>(get_handle(body, "width"));
    r.height = static_cast<std::uint32_t>(get_handle(body, "height"));
    r.xhot = get_int(body, "xhot", 0);
    r.yhot = get_int(body, "yhot", 0);
    r.has_pixel = get_bool(body, "has_pixel", false);
    r.pixel_bgra = static_cast<std::uint32_t>(get_handle(body, "pixel_bgra"));
    return r;
}

json::Value SidecarDebugEnumWindowsRequest::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("include_actual", json::Value(include_actual));
    return b;
}

SidecarDebugEnumWindowsRequest SidecarDebugEnumWindowsRequest::from_body(const json::Value& body) {
    SidecarDebugEnumWindowsRequest r;
    r.include_actual = get_bool(body, "include_actual", false);
    return r;
}

json::Value SidecarWindowInfo::to_value() const {
    json::Value v = json::Value::make_object();
    v.set("xid", handle_value(xid));
    v.set("representation", json::Value(representation));
    v.set("toplevel_registered", json::Value(toplevel_registered));
    v.set("has_surface", json::Value(has_surface));
    v.set("generation", handle_value(generation));
    v.set("epoch", handle_value(epoch));
    v.set("last_paint_seq", handle_value(last_paint_seq));
    v.set("shown", json::Value(shown));
    v.set("role", json::Value(role));
    v.set("title", json::Value(title));
    v.set("x", json::Value(x));
    v.set("y", json::Value(y));
    v.set("width", json::Value(static_cast<int>(width)));
    v.set("height", json::Value(static_cast<int>(height)));
    v.set("is_popup", json::Value(is_popup));
    v.set("owner_xid", handle_value(owner_xid));
    v.set("popup_kind", json::Value(static_cast<int>(popup_kind)));
    v.set("z_order", json::Value(static_cast<int>(z_order)));
    v.set("visibility_state", json::Value(static_cast<int>(visibility_state))); // (authored)
    // actual host geometry (only meaningful when has_actual; serialized always so the
    // decoder is total). frame/actual coords are ints; last_host_apply_seq rides as decimal-string
    // u64.
    v.set("has_actual", json::Value(has_actual));
    v.set("actual_x", json::Value(actual_x));
    v.set("actual_y", json::Value(actual_y));
    v.set("actual_width", json::Value(static_cast<int>(actual_width)));
    v.set("actual_height", json::Value(static_cast<int>(actual_height)));
    v.set("frame_x", json::Value(frame_x));
    v.set("frame_y", json::Value(frame_y));
    v.set("frame_width", json::Value(static_cast<int>(frame_width)));
    v.set("frame_height", json::Value(static_cast<int>(frame_height)));
    v.set("last_host_apply_seq", handle_value(last_host_apply_seq));
    v.set("clamped", json::Value(clamped));
    v.set("host_visible", json::Value(host_visible)); // host-observed (include_actual)
    v.set("host_iconic", json::Value(host_iconic));
    return v;
}

SidecarWindowInfo SidecarWindowInfo::from_value(const json::Value& v) {
    SidecarWindowInfo w;
    w.xid = get_handle(v, "xid");
    w.representation = get_string(v, "representation", "none");
    w.toplevel_registered = get_bool(v, "toplevel_registered", false);
    w.has_surface = get_bool(v, "has_surface", false);
    w.generation = get_handle(v, "generation");
    w.epoch = get_handle(v, "epoch");
    w.last_paint_seq = get_handle(v, "last_paint_seq");
    w.shown = get_bool(v, "shown", false);
    w.role = get_string(v, "role");
    w.title = get_string(v, "title");
    w.x = get_int(v, "x", 0);
    w.y = get_int(v, "y", 0);
    w.width = static_cast<std::uint32_t>(get_int(v, "width", 0));
    w.height = static_cast<std::uint32_t>(get_int(v, "height", 0));
    w.is_popup = get_bool(v, "is_popup", false);
    w.owner_xid = get_handle(v, "owner_xid");
    w.popup_kind = static_cast<std::uint32_t>(get_int(v, "popup_kind", 0));
    w.z_order = static_cast<std::uint32_t>(get_int(v, "z_order", 0));
    w.visibility_state = static_cast<std::uint32_t>(get_int(v, "visibility_state", 0));
    w.has_actual = get_bool(v, "has_actual", false);
    w.actual_x = get_int(v, "actual_x", 0);
    w.actual_y = get_int(v, "actual_y", 0);
    w.actual_width = static_cast<std::uint32_t>(get_int(v, "actual_width", 0));
    w.actual_height = static_cast<std::uint32_t>(get_int(v, "actual_height", 0));
    w.frame_x = get_int(v, "frame_x", 0);
    w.frame_y = get_int(v, "frame_y", 0);
    w.frame_width = static_cast<std::uint32_t>(get_int(v, "frame_width", 0));
    w.frame_height = static_cast<std::uint32_t>(get_int(v, "frame_height", 0));
    w.last_host_apply_seq = get_handle(v, "last_host_apply_seq");
    w.clamped = get_bool(v, "clamped", false);
    w.host_visible = get_bool(v, "host_visible", false);
    w.host_iconic = get_bool(v, "host_iconic", false);
    return w;
}

json::Value SidecarDebugEnumWindowsResponse::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("ok", json::Value(ok));
    b.set("reason", json::Value(reason));
    json::Array arr;
    for (const auto& w : windows) {
        arr.emplace_back(w.to_value());
    }
    b.set("windows", json::Value(std::move(arr)));
    return b;
}

SidecarDebugEnumWindowsResponse
SidecarDebugEnumWindowsResponse::from_body(const json::Value& body) {
    SidecarDebugEnumWindowsResponse r;
    r.ok = get_bool(body, "ok", false);
    r.reason = get_string(body, "reason");
    const json::Value* arr = body.find("windows");
    if (arr != nullptr && arr->is_array()) {
        for (const auto& w : arr->as_array()) {
            r.windows.push_back(SidecarWindowInfo::from_value(w));
        }
    }
    return r;
}

json::Value SidecarDebugCaptureWindowRequest::to_body() const {
    json::Value b = json::Value::make_object();
    b.set("xid", handle_value(xid));
    b.set("layer", json::Value(layer));
    b.set("expected_epoch", handle_value(expected_epoch));
    b.set("expected_lifecycle_generation", handle_value(expected_lifecycle_generation));
    b.set("min_last_seq", handle_value(min_last_seq));
    return b;
}

SidecarDebugCaptureWindowRequest
SidecarDebugCaptureWindowRequest::from_body(const json::Value& body) {
    SidecarDebugCaptureWindowRequest r;
    r.xid = get_handle(body, "xid");
    r.layer = get_string(body, "layer");
    r.expected_epoch = get_handle(body, "expected_epoch");
    r.expected_lifecycle_generation = get_handle(body, "expected_lifecycle_generation");
    r.min_last_seq = get_handle(body, "min_last_seq");
    return r;
}

std::string SidecarDebugCaptureWindowResponse::to_wire() const {
    json::Value h = json::Value::make_object();
    h.set("ok", json::Value(ok));
    h.set("status", json::Value(status));
    h.set("reason", json::Value(reason));
    h.set("xid", handle_value(xid));
    h.set("layer", json::Value(layer));
    h.set("representation", json::Value(representation));
    h.set("generation", handle_value(generation));
    h.set("epoch", handle_value(epoch));
    h.set("last_paint_seq", handle_value(last_paint_seq));
    h.set("shown", json::Value(shown));
    h.set("xhot", json::Value(xhot));
    h.set("yhot", json::Value(yhot));
    h.set("width", handle_value(width));
    h.set("height", handle_value(height));
    h.set("stride", handle_value(stride));
    h.set("format", json::Value(format));
    h.set("needed_bytes", handle_value(needed_bytes));
    const std::string json = h.dump(0);
    std::string out(4, '\0');
    protocol::store_le32(static_cast<std::uint32_t>(json.size()),
                         reinterpret_cast<unsigned char*>(&out[0]));
    out += json;
    out += pixels; // BGRA tail (empty unless status == "ok")
    return out;
}

SidecarDebugCaptureWindowResponse
SidecarDebugCaptureWindowResponse::from_wire(const std::string& body, std::string& err) {
    err.clear();
    if (body.size() < 4) {
        err = "capture body shorter than 4-byte length prefix";
        return SidecarDebugCaptureWindowResponse{};
    }
    const std::uint32_t json_len =
        protocol::load_le32(reinterpret_cast<const unsigned char*>(body.data()));
    if (json_len > kMaxCaptureJsonHeaderBytes) {
        err = "capture json header exceeds cap";
        return SidecarDebugCaptureWindowResponse{};
    }
    if (static_cast<std::size_t>(4) + json_len > body.size()) {
        err = "capture json header runs past end of body";
        return SidecarDebugCaptureWindowResponse{};
    }
    json::Value h;
    std::string jerr;
    if (!json::Value::try_parse(body.substr(4, json_len), h, jerr)) {
        err = "capture json header parse: " + jerr;
        return SidecarDebugCaptureWindowResponse{};
    }
    SidecarDebugCaptureWindowResponse r;
    r.ok = get_bool(h, "ok", false);
    r.status = get_string(h, "status");
    // The ok/status contract: status must be a known token and `ok` iff
    // status == "ok". The tool trusts this before writing a PNG, so a header like
    // ok=true/status="mismatch" or ok=false/status="ok" is rejected, not silently accepted.
    if (r.status != "ok" && r.status != "empty" && r.status != "absent" && r.status != "mismatch" &&
        r.status != "too_large" && r.status != "bad_layer") {
        err = "capture unknown status";
        return SidecarDebugCaptureWindowResponse{};
    }
    if (r.ok != (r.status == "ok")) {
        err = "capture ok/status inconsistent";
        return SidecarDebugCaptureWindowResponse{};
    }
    r.reason = get_string(h, "reason");
    r.xid = get_handle(h, "xid");
    r.layer = get_string(h, "layer");
    r.representation = get_string(h, "representation", "none");
    r.generation = get_handle(h, "generation");
    r.epoch = get_handle(h, "epoch");
    r.last_paint_seq = get_handle(h, "last_paint_seq");
    r.shown = get_bool(h, "shown", false);
    r.xhot = get_int(h, "xhot", 0);
    r.yhot = get_int(h, "yhot", 0);
    r.format = get_int(h, "format", 0);
    r.needed_bytes = get_handle(h, "needed_bytes");
    // Sizes decode WIDE (u64) so the bounds arithmetic cannot overflow; range-checked before
    // narrowing to the u32 fields.
    const std::uint64_t width = get_handle(h, "width");
    const std::uint64_t height = get_handle(h, "height");
    const std::uint64_t stride = get_handle(h, "stride");
    if (width > 0x7FFFFFFFull || height > 0x7FFFFFFFull || stride > 0x7FFFFFFFull) {
        err = "capture dimension exceeds INT_MAX";
        return SidecarDebugCaptureWindowResponse{};
    }
    const std::size_t tail_len = body.size() - 4 - json_len;
    if (r.ok) {
        // A successful capture carries an EXACT BGRA tail. Strict: BGRA8
        // only, nonzero dims, stride >= a full row, the source within the frame cap, exact tail
        // length.
        if (r.format != kCaptureFormatBgra8) {
            err = "capture unknown pixel format";
            return SidecarDebugCaptureWindowResponse{};
        }
        if (width == 0 || height == 0) {
            err = "capture zero dimension";
            return SidecarDebugCaptureWindowResponse{};
        }
        if (stride < width * 4ull) {
            err = "capture stride smaller than a row";
            return SidecarDebugCaptureWindowResponse{};
        }
        if (height * stride > static_cast<std::uint64_t>(kMaxCapturePayloadBytes)) {
            err = "capture payload exceeds the frame cap";
            return SidecarDebugCaptureWindowResponse{};
        }
        if (static_cast<std::uint64_t>(tail_len) != height * stride) {
            err = "capture tail length does not match height * stride";
            return SidecarDebugCaptureWindowResponse{};
        }
        r.pixels.assign(body, static_cast<std::size_t>(4) + json_len, std::string::npos);
    } else {
        // Every non-OK status (empty/absent/mismatch/too_large/bad_layer) carries NO pixel tail --
        // the structured hdr-json is the whole result.
        if (tail_len != 0) {
            err = "capture non-ok response carries a pixel tail";
            return SidecarDebugCaptureWindowResponse{};
        }
    }
    r.width = static_cast<std::uint32_t>(width);
    r.height = static_cast<std::uint32_t>(height);
    r.stride = static_cast<std::uint32_t>(stride);
    return r;
}

void serve_sidecar_rpc(vkrpc::RpcChannel& channel, SidecarBackend& backend) {
    vkrpc::RpcMessage req;
    try {
        while (channel.recv(req)) {
            vkrpc::RpcMessage resp;
            resp.op = req.op;
            resp.request_id = req.request_id;

            // Requests must carry status 0 (protocol contract); a nonzero status is malformed.
            if (req.status != 0) {
                resp.status = static_cast<std::uint32_t>(vkrpc::RpcStatus::BadRequest);
                VKR_WARN(kComponent) << "sidecar rpc request with nonzero status " << req.status;
                channel.send(resp);
                continue;
            }

            json::Value body;
            auto parse_body = [&]() -> bool {
                std::string err;
                if (json::Value::try_parse(req.body, body, err)) {
                    return true;
                }
                resp.status = static_cast<std::uint32_t>(vkrpc::RpcStatus::BadRequest);
                channel.send(resp);
                return false;
            };
            auto reply = [&](const json::Value& jbody) {
                resp.status = static_cast<std::uint32_t>(vkrpc::RpcStatus::Ok);
                resp.body = jbody.dump(0);
                channel.send(resp);
            };
            // a BINARY-bodied OK response (the capture op). The transport status is Ok
            // (the request was served); the capture result (ok/too_large/mismatch/absent) lives in
            // the response's own hdr-json. Raw bytes, not JSON-dumped.
            auto reply_wire = [&](const std::string& raw) {
                resp.status = static_cast<std::uint32_t>(vkrpc::RpcStatus::Ok);
                resp.body = raw;
                channel.send(resp);
            };

            switch (static_cast<SidecarOp>(req.op)) {
            case SidecarOp::Negotiate:
                if (parse_body()) {
                    reply(backend.negotiate(SidecarNegotiateRequest::from_body(body)).to_body());
                }
                break;
            case SidecarOp::Ready:
                if (parse_body()) {
                    reply(backend.sidecar_ready(SidecarReadyRequest::from_body(body)).to_body());
                }
                break;
            case SidecarOp::RegisterToplevel:
                if (parse_body()) {
                    reply(backend.register_toplevel(SidecarRegisterToplevelRequest::from_body(body))
                              .to_body());
                }
                break;
            case SidecarOp::UpdateToplevel:
                if (parse_body()) {
                    reply(backend.update_toplevel(SidecarUpdateToplevelRequest::from_body(body))
                              .to_body());
                }
                break;
            case SidecarOp::UnregisterToplevel:
                if (parse_body()) {
                    reply(
                        backend
                            .unregister_toplevel(SidecarUnregisterToplevelRequest::from_body(body))
                            .to_body());
                }
                break;
            case SidecarOp::SetToplevelVisibility:
                if (parse_body()) {
                    reply(backend.set_visibility(SidecarSetVisibilityRequest::from_body(body))
                              .to_body());
                }
                break;
            case SidecarOp::PaintChrome: {
                // BINARY-bodied (like CreateShaderModule): decode the [u32 json_len][json][BGRA]
                // body via from_wire (NOT parse_body, which expects JSON). A framing/discipline
                // fault is BadRequest -- a malformed body never reaches the paint path.
                std::string pcerr;
                const SidecarPaintChromeRequest preq =
                    SidecarPaintChromeRequest::from_wire(req.body, pcerr);
                if (!pcerr.empty()) {
                    resp.status = static_cast<std::uint32_t>(vkrpc::RpcStatus::BadRequest);
                    VKR_WARN(kComponent) << "paint_chrome decode rejected: " << pcerr;
                    channel.send(resp);
                    break;
                }
                reply(backend.paint_chrome(preq).to_body());
                break;
            }
            case SidecarOp::DebugChromeState:
                if (parse_body()) {
                    reply(
                        backend.debug_chrome_state(SidecarDebugChromeStateRequest::from_body(body))
                            .to_body());
                }
                break;
            case SidecarOp::PollInput:
                if (parse_body()) {
                    reply(backend.poll_input(SidecarPollInputRequest::from_body(body)).to_body());
                }
                break;
            case SidecarOp::DebugEnqueueInput:
                if (parse_body()) {
                    reply(backend
                              .debug_enqueue_input(SidecarDebugEnqueueInputRequest::from_body(body))
                              .to_body());
                }
                break;
            case SidecarOp::SetCursor: {
                // BINARY-bodied (like PaintChrome): decode via from_wire; a framing/discipline
                // fault is BadRequest so a malformed body never reaches the HCURSOR build.
                std::string scerr;
                const SidecarSetCursorRequest sreq =
                    SidecarSetCursorRequest::from_wire(req.body, scerr);
                if (!scerr.empty()) {
                    resp.status = static_cast<std::uint32_t>(vkrpc::RpcStatus::BadRequest);
                    VKR_WARN(kComponent) << "set_cursor decode rejected: " << scerr;
                    channel.send(resp);
                    break;
                }
                reply(backend.set_cursor(sreq).to_body());
                break;
            }
            case SidecarOp::DebugCursorState:
                if (parse_body()) {
                    reply(
                        backend.debug_cursor_state(SidecarDebugCursorStateRequest::from_body(body))
                            .to_body());
                }
                break;
            case SidecarOp::DebugEnumWindows:
                if (parse_body()) {
                    reply(
                        backend.debug_enum_windows(SidecarDebugEnumWindowsRequest::from_body(body))
                            .to_body());
                }
                break;
            case SidecarOp::DebugCaptureWindow:
                // Request is plain JSON; the RESPONSE is binary-framed (reply_wire). A non-OK
                // capture result rides in the response hdr-json, so this still replies Ok at the
                // transport.
                if (parse_body()) {
                    reply_wire(
                        backend
                            .debug_capture_window(SidecarDebugCaptureWindowRequest::from_body(body))
                            .to_wire());
                }
                break;
            default:
                // An unrecognized op (incl. a Vulkan op mis-sent to this plane) is forward-
                // compatibly rejected -- the sidecar dispatcher never knows Vulkan op numbers.
                resp.status = static_cast<std::uint32_t>(vkrpc::RpcStatus::UnknownOp);
                VKR_WARN(kComponent) << "unknown sidecar op " << req.op;
                channel.send(resp);
                break;
            }
        }
    } catch (const std::exception& e) {
        VKR_INFO(kComponent) << "sidecar rpc session ended: " << e.what();
    }
}

} // namespace vkr::sidecar
