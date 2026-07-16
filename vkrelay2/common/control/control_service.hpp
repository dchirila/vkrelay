// Server-side control session: handshake, then answer control requests until
// the peer disconnects. Shared by the supervisor
// --serve loop and the loopback integration test so both exercise one code path.
#ifndef VKRELAY2_COMMON_CONTROL_CONTROL_SERVICE_HPP
#define VKRELAY2_COMMON_CONTROL_CONTROL_SERVICE_HPP

#include "common/display/display_layout.hpp"
#include "common/protocol/gpu.hpp"
#include "common/protocol/ids.hpp"
#include "common/transport/transport.hpp"

#include <string>
#include <vector>

namespace vkr::control {

// Result of launching a worker for an app/session. The app reconnects to the worker's
// data plane at host:port and proves itself with app_token.
struct SessionInfo {
    std::string worker_id;
    std::string data_plane_host;
    int data_plane_port = 0;
    std::string app_token;
    // Sidecar plane: present only when the session was launched with a sidecar plane
    // (empty/0 otherwise). The sidecar connects to host:port and proves itself with sidecar_token.
    std::string sidecar_plane_host;
    int sidecar_plane_port = 0;
    std::string sidecar_token;
    std::string op_trace_path;
    std::string display_snapshot_id;
};

// Supervisor-owned seam. A fresh validated snapshot is advertised on each handshake, while a
// later LaunchSession resolves the exact ID returned by the earlier query into an owned copy.
// Implementations keep a bounded process-lifetime cache; control code never re-probes geometry.
class DisplaySnapshotProvider {
  public:
    virtual ~DisplaySnapshotProvider() = default;
    virtual display::DisplayLayoutDecodeResult capture_for_handshake() = 0;
    virtual bool resolve_copy(const std::string& snapshot_id, display::DisplayLayout& layout,
                              std::string& reason) const = 0;
};

// Abstraction so the control session (common) can trigger a worker launch
// without depending on the supervisor implementation. WorkerSupervisor
// implements this. Throws std::exception on failure. `app_instance_id` is the
// canonical app identity from the handshake; the launched session is bound to it
// (routing keys off worker_id / app_instance_id, per the protocol contract).
class SessionLauncher {
  public:
    virtual ~SessionLauncher() = default;
    // `profile_enabled`: spawn this session's worker with VKRELAY2_PROFILE=1 --
    // profiling is session-scoped, never a daemon-lifetime accident.
    // `op_trace_enabled` and the narrowly allow-listed `input_trace_enabled` follow the same
    // ownership rule; neither is an arbitrary environment injection surface.
    virtual SessionInfo launch_session(const std::string& app_instance_id,
                                       const std::string& gpu_selector,
                                       const std::string& display_backend,
                                       const std::string& graphics_frontend, bool profile_enabled,
                                       bool op_trace_enabled, bool input_trace_enabled,
                                       const display::DisplayLayout* display_layout) = 0;

    // Explicit app-lifetime teardown for a launched worker that may never receive an AppHello
    // (pure-X11/placeholder applications do not load the Vulkan ICD). Implementations authenticate
    // BOTH the canonical handshake app id and the per-session app token. The default keeps small
    // test launchers source-compatible while failing closed.
    virtual bool close_session(const std::string& app_instance_id, const std::string& worker_id,
                               const std::string& app_token, std::string& reason) {
        (void) app_instance_id;
        (void) worker_id;
        (void) app_token;
        reason = "session close unsupported";
        return false;
    }
};

// Runs one control session on `channel`: performs the server handshake (minting
// ids via `ids`), then handles GpuListRequest / GpuSelectRequest / Heartbeat
// until the client disconnects. When `launcher` is non-null, also handles
// LaunchSession (app trigger -> worker launch -> SessionStarted reconnect
// details). `devices` is the adapter list to report. Returns the assigned
// connection id. Throws transport::TransportError on a protocol violation.
// `devices_real` stamps the GpuListResponse provenance (real host enumeration vs the
// mocked fallback) so a user-facing --list-gpus can refuse to print mocks as hardware.
// Defaults to true: test harnesses that serve probe_mocked() directly keep printing
// (the deliberate testing scenario); the production supervisor passes its probe result.
std::string serve_session(transport::MessageChannel& channel, protocol::IdAllocator& ids,
                          const std::vector<protocol::GpuDevice>& devices,
                          SessionLauncher* launcher = nullptr,
                          const std::string& worker_backend = "mock",
                          const protocol::HostDisplay& host_display = {},
                          DisplaySnapshotProvider* display_snapshots = nullptr,
                          bool devices_real = true);

} // namespace vkr::control

#endif // VKRELAY2_COMMON_CONTROL_CONTROL_SERVICE_HPP
