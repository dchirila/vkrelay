// Worker lifecycle supervisor.
//
// Launches worker processes in Job Objects / process groups and supervises them
// over a dedicated worker-control TCP channel: each worker connects back,
// registers with a one-time token, and heartbeats. The supervisor kills workers
// that miss the heartbeat deadline, contains worker crashes (other workers and
// the supervisor survive), and terminates every worker on shutdown so no stale
// child processes are left behind.
//
// The supervisor exposes launch_worker() directly; wiring it to an app/ICD
// control connection (the reconnect handoff) comes later.
#ifndef VKRELAY2_WINDOWS_SUPERVISOR_WORKER_SUPERVISOR_HPP
#define VKRELAY2_WINDOWS_SUPERVISOR_WORKER_SUPERVISOR_HPP

#include "common/control/control_service.hpp"
#include "common/protocol/gpu.hpp"
#include "common/protocol/ids.hpp"
#include "common/protocol/worker_lifecycle.hpp"
#include "common/transport/transport.hpp"

#include <atomic>
#include <cstddef>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace vkr::supervisor {

struct WorkerSupervisorConfig {
    std::string worker_path;                    // path to the vkrelay2-worker executable
    int heartbeat_timeout_ms = 3000;            // kill a worker that goes silent this long
    int worker_interval_ms = 50;                // heartbeat cadence requested of workers
    int worker_count = 0;                       // heartbeats requested (0 = until killed)
    std::vector<std::string> extra_worker_args; // appended to the worker argv (tests/diagnostics)
    // The adapter list LaunchSession resolves selectors against (injected so it matches the list
    // the client's GpuListResponse showed). Empty -> the supervisor falls back to the mock list.
    std::vector<protocol::GpuDevice> devices;
    bool devices_real = false; // true = `devices` is the real probed list (enables LUID selection)
    // Sidecar plane: when true, launched app sessions also get a sidecar plane -- the worker opens
    // a second listener and the supervisor mints+reports its endpoint/token. Default false (no
    // sidecar) until the launcher actually starts a sidecar process, so the production app-run path
    // is unchanged meanwhile.
    bool sidecar_plane = false;
};

// Builds the worker process argv for a launch. Extracted (and free) so the launch-time selection
// contract -- the worker receives the real GPU's stable LUID as a faithful match key (only when
// the device list is real) plus `--vulkan-backend` -- is unit-testable without spawning. The LUID
// is passed iff `devices_real` and the device actually carries one; a mock/auto launch passes none
// (the worker then selects leniently).
std::vector<std::string> build_worker_argv(const std::string& worker_path, int worker_control_port,
                                           const std::string& worker_id, const std::string& token,
                                           const protocol::GpuDevice& device, bool devices_real,
                                           int worker_interval_ms, int heartbeat_count,
                                           bool data_plane, const std::string& app_token,
                                           bool sidecar_plane, const std::string& sidecar_token,
                                           const std::vector<std::string>& extra_worker_args);

struct WorkerStatus {
    std::string worker_id;
    std::string app_instance_id; // canonical app identity bound at launch (empty if none)
    std::string gpu_name;
    protocol::WorkerState state = protocol::WorkerState::Spawned;
    long long heartbeats = 0;
    bool connected = false;
};

class WorkerSupervisor : public control::SessionLauncher {
  public:
    explicit WorkerSupervisor(WorkerSupervisorConfig config);
    ~WorkerSupervisor() override;

    WorkerSupervisor(const WorkerSupervisor&) = delete;
    WorkerSupervisor& operator=(const WorkerSupervisor&) = delete;

    int worker_control_port() const { return wc_port_; }

    // Resolves the GPU selector, spawns a worker, and registers it. Returns the
    // worker id. Throws std::runtime_error if the selector is unusable, the
    // selector text is invalid, or the spawn fails. The first form uses the
    // configured heartbeat count; the second overrides it (0 = until killed).
    std::string launch_worker(const std::string& gpu_selector);
    std::string launch_worker(const std::string& gpu_selector, int heartbeat_count);

    // SessionLauncher: launch a worker with a data-plane endpoint for an app and
    // wait until it has registered and reported its data-plane port. The session
    // is bound to `app_instance_id`. Throws on failure (bad selector, spawn
    // failure, or registration timeout).
    control::SessionInfo launch_session(const std::string& app_instance_id,
                                        const std::string& gpu_selector,
                                        const std::string& display_backend,
                                        const std::string& graphics_frontend, bool profile_enabled,
                                        bool op_trace_enabled, bool input_trace_enabled,
                                        const display::DisplayLayout* display_layout) override;
    bool close_session(const std::string& app_instance_id, const std::string& worker_id,
                       const std::string& app_token, std::string& reason) override;

    std::vector<WorkerStatus> snapshot() const;
    std::size_t active_count() const; // sessions not in a terminal state
    long long total_heartbeats() const;

    void shutdown(); // idempotent; also called by the destructor

  private:
    struct Session;

    std::string spawn_and_register(const std::string& gpu_selector, int heartbeat_count,
                                   const std::string& app_instance_id, const std::string& app_token,
                                   bool data_plane, bool sidecar_plane, bool profile_enabled,
                                   bool op_trace_enabled, bool input_trace_enabled,
                                   const display::DisplayLayout* display_layout,
                                   std::string& gpu_name_out);
    void accept_loop();
    void handle_worker_connection(std::shared_ptr<transport::Connection> conn);
    void monitor_loop();
    Session* find_by_token(const std::string& token);      // caller holds mutex_
    Session* find_by_worker(const std::string& worker_id); // caller holds mutex_

    WorkerSupervisorConfig config_;
    std::vector<protocol::GpuDevice> devices_;
    bool devices_real_ = false; // whether devices_ is the real probed list (gates LUID selection)
    protocol::IdAllocator ids_;
    std::unique_ptr<transport::Listener> wc_listener_;
    int wc_port_ = 0;

    mutable std::mutex mutex_;
    std::map<std::string, std::unique_ptr<Session>> sessions_;

    std::atomic<bool> stopping_{false};
    std::thread accept_thread_;
    std::thread monitor_thread_;
    std::mutex handlers_mutex_;
    std::vector<std::thread> handler_threads_;

    // Every accepted control connection (worker or not) is tracked so shutdown
    // can cancel() it and release a handler blocked in recv.
    std::mutex conns_mutex_;
    std::set<std::shared_ptr<transport::Connection>> conns_;
};

} // namespace vkr::supervisor

#endif // VKRELAY2_WINDOWS_SUPERVISOR_WORKER_SUPERVISOR_HPP
