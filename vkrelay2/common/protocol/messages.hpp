// vkrelay2 control-plane message catalog.
//
// Each message is a JSON object {"type": <name>, "body": {...}} carried as a
// single frame payload (see wire.hpp). Bodies are JSON so fields can be added
// forward-compatibly; the negotiated protocol version lives in the Hello
// handshake, and version mismatch is a clean, typed error rather than a crash.
#ifndef VKRELAY2_COMMON_PROTOCOL_MESSAGES_HPP
#define VKRELAY2_COMMON_PROTOCOL_MESSAGES_HPP

#include "common/display/display_layout.hpp"
#include "common/protocol/gpu.hpp"
#include "common/util/json.hpp"

#include <string>
#include <vector>

namespace vkr::protocol {

constexpr int kProtocolVersion = 1;

// PROCESS ABI: append-only and explicitly numbered. Never insert, reorder, renumber, or reuse a
// value: MessageType constants are compiled into one TU while to_string/from_string live in
// another, so a mixed incremental build would silently serialize an existing type under the wrong
// wire name. Unknown=18 is reserved; add each new message before it using the next unused value
// (19, 20, ...), without changing any existing assignment.
enum class MessageType {
    Hello = 0,             // client -> server: open a session
    HelloAck = 1,          // server -> client: session accepted, ids assigned
    Error = 2,             // either direction: typed failure
    GpuListRequest = 3,    // client -> server
    GpuListResponse = 4,   // server -> client
    GpuSelectRequest = 5,  // client -> server
    GpuSelectResponse = 6, // server -> client
    WorkerHello = 7,       // worker -> supervisor: register on the worker-control channel
    WorkerAck = 8,         // supervisor -> worker: registration accepted/rejected
    Heartbeat = 9,         // worker -> supervisor liveness
    LaunchSession = 10,    // app -> supervisor: launch a worker for this app/session
    SessionStarted = 11,   // supervisor -> app: worker launched; reconnect details
    AppHello = 12,         // app -> worker: open the data plane with the reconnect token
    AppAck = 13,           // worker -> app: data plane accepted/rejected
    SidecarHello = 14,     // sidecar -> worker: open the sidecar plane with its token
    SidecarAck = 15,       // worker -> sidecar: sidecar plane accepted/rejected
    CloseSession = 16,     // launcher -> supervisor: authenticated app-lifetime teardown
    SessionClosed = 17,    // supervisor -> launcher: close accepted/rejected
    Unknown = 18           // unrecognized (forward-compat); handled gracefully
};

const char* to_string(MessageType type);
MessageType message_type_from_string(const std::string& name);

// Frame payload = encode_message(type, body). decode_message is total: it
// returns false on malformed JSON, and yields MessageType::Unknown (still true)
// for an unrecognized but well-formed message.
std::string encode_message(MessageType type, const json::Value& body);
bool decode_message(const std::string& payload, MessageType& type, json::Value& body,
                    std::string& error);

struct ClientHello {
    int protocol_version = kProtocolVersion;
    std::string app_instance_id;
    std::string gpu_selector = "auto";
    std::string display_backend = "auto";
    std::string graphics_frontend = "auto";

    json::Value to_body() const;
    static ClientHello from_body(const json::Value& body);
};

// Guest display geometry: the HOST's addressable window space, advertised by the daemon at the
// handshake so the launcher can size the private guest X root to it BEFORE the display starts --
// derived from the machine, never hardcoded. work_w/work_h = the primary monitor WORK AREA in
// PHYSICAL px (the exact space the worker's coordinate map realizes: X-root (0,0) = work-area
// top-left, 1:1 physical); dpi = the host system DPI (e.g. 144 at 150% scale) for guest font-scale
// parity. 0 = unknown (an old daemon, a probe failure, or a non-Windows/mock supervisor build) --
// the launcher then keeps the display's default size.
struct HostDisplay {
    int work_w = 0;
    int work_h = 0;
    int dpi = 0;
};

struct ServerHello {
    int protocol_version = kProtocolVersion;
    std::string supervisor_session_id; // supervisor-lifetime id
    std::string worker_id;             // per-connection id
    // The backend this daemon launches session workers with: "mock" | "real". Advertised at the
    // handshake so a client can verify BEFORE launching that the daemon serves real host Vulkan
    // (app-run / on-screen present needs "real" and fails closed otherwise). Defaults to "mock" so
    // a daemon that predates this field (or a genuinely mock daemon) is treated as mock -- the safe
    // direction (never assume real).
    std::string worker_backend = "mock";
    // The host's addressable window space (additive; absent -> all 0 = unknown).
    HostDisplay host_display;
    // Strict optional topology codec. A Windows production supervisor advertises a
    // validated cache entry; Absent remains the legacy/non-Windows shape. Present malformed or
    // unsupported data stays distinguishable from omission.
    display::DisplayLayoutDecodeResult display_layout;

    json::Value to_body() const;
    static ServerHello from_body(const json::Value& body);
};

struct ErrorMsg {
    std::string code;
    std::string message;

    json::Value to_body() const;
    static ErrorMsg from_body(const json::Value& body);
};

struct GpuSelectRequest {
    std::string selector = "auto";

    json::Value to_body() const;
    static GpuSelectRequest from_body(const json::Value& body);
};

struct GpuSelectResponse {
    bool ok = false;
    GpuDevice device;
    std::string reason;

    json::Value to_body() const;
    static GpuSelectResponse from_body(const json::Value& body);
};

struct WorkerHello {
    std::string worker_id;
    std::string token;          // one-time token issued by the supervisor at launch
    int data_plane_port = 0;    // worker's data-plane listener (0 if none)
    int sidecar_plane_port = 0; // worker's sidecar-plane listener (0 if none)

    json::Value to_body() const;
    static WorkerHello from_body(const json::Value& body);
};

struct WorkerAck {
    bool ok = false;
    std::string reason;
    // Full immutable session copy delivered over the authenticated one-time worker
    // registration channel, avoiding command-line/environment size limits.
    display::DisplayLayoutDecodeResult display_layout;

    json::Value to_body() const;
    static WorkerAck from_body(const json::Value& body);
};

struct Heartbeat {
    std::string worker_id;
    long long seq = 0;

    json::Value to_body() const;
    static Heartbeat from_body(const json::Value& body);
};

struct LaunchSession {
    std::string app_instance_id;
    std::string gpu_selector = "auto";
    std::string display_backend = "auto";
    std::string graphics_frontend = "auto";
    // Profiling is a property of the SESSION, not the daemon's
    // lifetime -- daemon reuse makes start-env inheritance an ambient accident. Additive: absent
    // decodes false (an old daemon ignores it; the profile smoke asserts BOTH ends, so a skewed
    // pair fails loudly rather than silently half-profiling). When set, the supervisor spawns the
    // session worker with VKRELAY2_PROFILE=1 in its environment.
    bool profile_enabled = false;
    // AMD-iGPU investigation: decoded worker-operation tracing is likewise session-scoped. The
    // supervisor supplies this worker alone with a unique VKRELAY2_OP_TRACE_PATH; an absent field
    // decodes false so protocol skew leaves tracing off rather than inheriting daemon state.
    bool op_trace_enabled = false;
    // Narrowly allow-list the diagnostic input-path trace switch across a reused daemon. This
    // is deliberately a boolean session property, not an arbitrary environment map.
    bool input_trace_enabled = false;
    // The immutable topology advertised by the earlier host-display query. A current
    // supervisor resolves this exact ID from its bounded process-lifetime cache before spawning;
    // an old supervisor ignores the additive field.
    std::string display_snapshot_id;

    json::Value to_body() const;
    static LaunchSession from_body(const json::Value& body);
};

struct SessionStarted {
    std::string worker_id;
    std::string data_plane_host;
    int data_plane_port = 0;
    std::string app_token; // present to the worker's data plane via AppHello
    // Sidecar plane: the sidecar connects to host:port and proves itself with
    // sidecar_token via SidecarHello. Empty/0 when the session has no sidecar plane (the
    // default until the launcher starts a sidecar), so an older app/launcher simply ignores it.
    std::string sidecar_plane_host;
    int sidecar_plane_port = 0;
    std::string sidecar_token;
    // Host artifact path for a requested worker op trace. Empty when tracing was not requested or
    // when an older supervisor ignored the additive LaunchSession field.
    std::string op_trace_path;
    // The exact cached snapshot copied into the worker launch contract. The launcher exports this
    // to the sidecar so SidecarReady can prove all participants name one immutable snapshot.
    std::string display_snapshot_id;

    json::Value to_body() const;
    static SessionStarted from_body(const json::Value& body);
};

// Explicit launcher-owned teardown for sessions whose target never opens the Vulkan data plane
// (for example a pure-X11 placeholder app). `app_token` is the same unguessable per-session token
// returned by SessionStarted; the supervisor also checks the control handshake's canonical app id.
struct CloseSession {
    std::string worker_id;
    std::string app_token;

    json::Value to_body() const;
    static CloseSession from_body(const json::Value& body);
};

struct SessionClosed {
    std::string worker_id;
    bool accepted = false;
    std::string reason;

    json::Value to_body() const;
    static SessionClosed from_body(const json::Value& body);
};

struct AppHello {
    std::string app_token;

    json::Value to_body() const;
    static AppHello from_body(const json::Value& body);
};

struct AppAck {
    bool ok = false;
    std::string reason;

    json::Value to_body() const;
    static AppAck from_body(const json::Value& body);
};

// Sidecar plane handshake, the sidecar's analogue of AppHello/AppAck. The sidecar
// proves itself to the worker's sidecar listener with the session's sidecar_token before the
// connection switches to the binary sidecar RPC envelope.
struct SidecarHello {
    int protocol_version = kProtocolVersion;
    std::string sidecar_token;

    json::Value to_body() const;
    static SidecarHello from_body(const json::Value& body);
};

struct SidecarAck {
    bool ok = false;
    std::string reason;
    // The exact immutable session snapshot already authenticated and accepted by the worker.
    // Additive/absent for legacy peers; production sidecars require Valid before applying
    // monitor-aware placement policy.
    display::DisplayLayoutDecodeResult display_layout;

    json::Value to_body() const;
    static SidecarAck from_body(const json::Value& body);
};

// GpuDevice <-> JSON (used by GpuListResponse and GpuSelectResponse).
json::Value gpu_device_to_json(const GpuDevice& device);
GpuDevice gpu_device_from_json(const json::Value& value);
// `real` marks the list's provenance: true = host enumeration, false = the mocked
// fallback. User-facing consumers refuse to present a mock list as hardware (the
// vkrelay2-launch --list-gpus printer gates on it; VKRELAY2_ALLOW_MOCK_GPUS=1 is the
// testing escape hatch). Additive JSON field -- old peers interoperate unchanged.
json::Value gpu_list_to_body(const std::vector<GpuDevice>& devices, bool real = true);
std::vector<GpuDevice> gpu_list_from_body(const json::Value& body);
// True iff the body EXPLICITLY carries real=false (absent = legacy daemon = not marked).
bool gpu_list_marked_mock(const json::Value& body);

} // namespace vkr::protocol

#endif // VKRELAY2_COMMON_PROTOCOL_MESSAGES_HPP
