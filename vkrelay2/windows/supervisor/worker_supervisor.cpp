#include "windows/supervisor/worker_supervisor.hpp"

#include "common/logging/logging.hpp"
#include "common/process/process.hpp"
#include "common/protocol/messages.hpp"
#include "common/transport/tcp.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <utility>

namespace vkr::supervisor {
namespace {

constexpr char kComponent[] = "supervisor";

std::atomic<std::uint64_t> g_token_counter{1};

std::string mint_token(const std::string& worker_id) {
    return "tok-" + std::to_string(g_token_counter.fetch_add(1, std::memory_order_relaxed)) + "-" +
           worker_id;
}

// Minimal RAII scope guard: runs `f` when it goes out of scope.
template <typename F> struct ScopeGuard {
    F f;
    ~ScopeGuard() { f(); }
};
template <typename F> ScopeGuard<F> make_scope_guard(F f) {
    return ScopeGuard<F>{std::move(f)};
}

using clock = std::chrono::steady_clock;

// Terminal state for a worker whose process has gone away: a clean exit while
// Running is Exited; anything else (exit before registering, non-zero code) is
// Crashed. Both are reachable from any non-terminal state except Exited, which
// is only legal from Running -- which is exactly when we report it.
protocol::WorkerState terminal_state(bool exited, int code, protocol::WorkerState current) {
    if (exited && code == 0 && current == protocol::WorkerState::Running) {
        return protocol::WorkerState::Exited;
    }
    return protocol::WorkerState::Crashed;
}

} // namespace

struct WorkerSupervisor::Session {
    std::string worker_id;
    std::string app_instance_id; // canonical app identity bound at launch (empty if none)
    std::string token;           // one-time worker-control token; cleared once consumed
    std::string app_token;       // token the app presents to the worker's data plane
    std::string sidecar_token;   // token the sidecar presents to the worker's sidecar plane
    std::string gpu_name;
    std::string op_trace_path; // per-session decoded worker-op artifact (empty when disabled)
    display::DisplayLayout display_layout; // immutable session-owned copy (empty for legacy tests)
    bool has_display_layout = false;
    int data_plane_port = 0;    // reported by the worker in WorkerHello (0 = none)
    int sidecar_plane_port = 0; // reported by the worker in WorkerHello (0 = none)
    process::Process proc;
    protocol::WorkerLifecycle lifecycle;
    clock::time_point last_heartbeat; // also the registration deadline clock while Spawned
    long long heartbeats = 0;
    bool connected = false;
    bool channel_closed = false; // worker-control channel dropped (set by handler)
    clock::time_point channel_closed_at;
    // terminate() is asynchronous (esp. Windows TerminateProcess/
    // TerminateJobObject), so a session is NOT terminal until proc.wait() OBSERVES the exit. We
    // record that termination was requested (so the observed exit classifies as Killed, not
    // Exited/Crashed) and when, to retry if the process has not died within the escalation window.
    bool termination_requested = false;
    clock::time_point termination_requested_at{};
    // The launcher owns the target process group and explicitly closes the worker session after
    // that group exits. Give an ordinary Vulkan App-socket EOF a short chance to make the worker
    // exit cleanly; a placeholder-only app has no App socket, so the monitor then terminates it.
    bool launcher_close_requested = false;
    clock::time_point launcher_close_requested_at{};
};

// Grace after a worker drops its control channel before we treat it as a
// misbehaving (still-alive) worker and kill it. Short enough to bound an
// unsupervised worker, long enough for a clean exit's process teardown to be
// reaped as Exited rather than mislabeled.
constexpr int kChannelCloseGraceMs = 1000;

// After requesting termination, how long to wait for proc.wait() to OBSERVE the exit before
// re-issuing the (asynchronous) terminate and logging that the child has not died yet.
constexpr int kTerminateEscalateMs = 1000;
constexpr int kLauncherCloseGraceMs = 250;

std::vector<std::string> build_worker_argv(const std::string& worker_path, int worker_control_port,
                                           const std::string& worker_id, const std::string& token,
                                           const protocol::GpuDevice& device, bool devices_real,
                                           int worker_interval_ms, int heartbeat_count,
                                           bool data_plane, const std::string& app_token,
                                           bool sidecar_plane, const std::string& sidecar_token,
                                           const std::vector<std::string>& extra_worker_args) {
    std::vector<std::string> argv = {worker_path,
                                     "--connect",
                                     "127.0.0.1:" + std::to_string(worker_control_port),
                                     "--worker-id",
                                     worker_id,
                                     "--token",
                                     token,
                                     "--gpu",
                                     device.name,
                                     "--interval-ms",
                                     std::to_string(worker_interval_ms),
                                     "--count",
                                     std::to_string(heartbeat_count)};
    // Faithful selection: a real probed list resolved a concrete device, so mark the launch
    // --gpu-required (the worker serves exactly that device or fails closed -- never a silent
    // substitute) and hand over the stable LUID as the preferred match key when the device carries
    // one (else the worker falls back to the exact --gpu name, still fail-closed). A mock/auto
    // launch passes neither, so the worker selects leniently.
    if (devices_real) {
        argv.push_back("--gpu-required");
        if (!device.luid.empty()) {
            argv.push_back("--gpu-luid");
            argv.push_back(device.luid);
        }
    }
    if (data_plane) {
        argv.push_back("--data-plane");
        argv.push_back("--app-token");
        argv.push_back(app_token);
    }
    if (sidecar_plane) {
        argv.push_back("--sidecar-plane");
        argv.push_back("--sidecar-token");
        argv.push_back(sidecar_token);
    }
    argv.insert(argv.end(), extra_worker_args.begin(), extra_worker_args.end());
    return argv;
}

// devices_ is the injected real probed list when available (so launch-time selection matches the
// GpuListResponse the client saw and passes the resolved LUID to the worker); else the mock list
// (lenient selection). Real adapter enumeration/display for --list-gpus and GpuListResponse goes
// through probe_gpus() at the CLI/serve paths.
WorkerSupervisor::WorkerSupervisor(WorkerSupervisorConfig config)
    : config_(std::move(config)),
      devices_(config_.devices.empty() ? protocol::probe_mocked() : config_.devices),
      devices_real_(!config_.devices.empty() && config_.devices_real) {
    wc_listener_ = transport::tcp_listen(0);
    wc_port_ = wc_listener_->port();
    VKR_INFO(kComponent) << "worker-control channel on 127.0.0.1:" << wc_port_
                         << "; supervisor_session=" << ids_.supervisor_session_id();
    accept_thread_ = std::thread(&WorkerSupervisor::accept_loop, this);
    monitor_thread_ = std::thread(&WorkerSupervisor::monitor_loop, this);
}

WorkerSupervisor::~WorkerSupervisor() {
    shutdown();
}

WorkerSupervisor::Session* WorkerSupervisor::find_by_token(const std::string& token) {
    for (auto& kv : sessions_) {
        if (kv.second->token == token) {
            return kv.second.get();
        }
    }
    return nullptr;
}

WorkerSupervisor::Session* WorkerSupervisor::find_by_worker(const std::string& worker_id) {
    auto it = sessions_.find(worker_id);
    return it == sessions_.end() ? nullptr : it->second.get();
}

std::string WorkerSupervisor::launch_worker(const std::string& gpu_selector) {
    return launch_worker(gpu_selector, config_.worker_count);
}

std::string WorkerSupervisor::launch_worker(const std::string& gpu_selector, int heartbeat_count) {
    std::string gpu_name;
    return spawn_and_register(gpu_selector, heartbeat_count, /*app_instance_id=*/"",
                              /*app_token=*/"", /*data_plane=*/false, /*sidecar_plane=*/false,
                              /*profile_enabled=*/false, /*op_trace_enabled=*/false,
                              /*input_trace_enabled=*/false,
                              /*display_layout=*/nullptr, gpu_name);
}

std::string WorkerSupervisor::spawn_and_register(
    const std::string& gpu_selector, int heartbeat_count, const std::string& app_instance_id,
    const std::string& app_token, bool data_plane, bool sidecar_plane, bool profile_enabled,
    bool op_trace_enabled, bool input_trace_enabled, const display::DisplayLayout* display_layout,
    std::string& gpu_name_out) {
    if (stopping_.load()) {
        throw std::runtime_error("supervisor is shutting down");
    }

    const auto parsed = protocol::parse_selector(gpu_selector);
    if (!parsed.ok) {
        throw std::runtime_error("invalid GPU selector: " + parsed.error);
    }
    std::string reason;
    const protocol::GpuDevice* device = protocol::select_device(devices_, parsed.selector, reason);
    if (device == nullptr) {
        throw std::runtime_error(reason);
    }
    gpu_name_out = device->name;

    const std::string worker_id = ids_.next_worker_id();
    const std::string token = mint_token(worker_id);
    // Sidecar plane: mint a distinct token the sidecar proves itself with, minted only
    // when this launch carries a sidecar plane (empty otherwise).
    const std::string sidecar_token = sidecar_plane ? ("sidecar-" + mint_token(worker_id)) : "";

    process::SpawnRequest req;
    req.argv =
        build_worker_argv(config_.worker_path, wc_port_, worker_id, token, *device, devices_real_,
                          config_.worker_interval_ms, heartbeat_count, data_plane, app_token,
                          sidecar_plane, sidecar_token, config_.extra_worker_args);
    req.new_group = true; // own Job Object / process group for atomic kill
    if (profile_enabled) {
        // Session-scoped profiling -- THIS worker profiles its
        // serve end regardless of the daemon's own start environment.
        req.env_overrides.emplace_back("VKRELAY2_PROFILE", "1");
    }
    if (input_trace_enabled) {
        req.env_overrides.emplace_back("VKRELAY2_INPUT_TRACE", "1");
    }
    if (display_layout != nullptr) {
        const display::ValidationResult validation =
            display::validate_display_layout(*display_layout);
        if (!validation.ok) {
            throw std::runtime_error("refusing invalid display snapshot at worker launch: " +
                                     validation.reason);
        }
    }
    std::string op_trace_path;
    if (op_trace_enabled) {
        const auto stamp = std::chrono::duration_cast<std::chrono::microseconds>(
                               std::chrono::system_clock::now().time_since_epoch())
                               .count();
        op_trace_path = (std::filesystem::temp_directory_path() /
                         ("vkrelay2-optrace-" + worker_id + "-" + std::to_string(stamp) + ".jsonl"))
                            .string();
        req.env_overrides.emplace_back("VKRELAY2_OP_TRACE_PATH", op_trace_path);
        VKR_INFO(kComponent) << "worker op trace path=" << op_trace_path;
    }

    process::Process proc = process::Process::spawn(req);

    auto session = std::make_unique<Session>();
    session->worker_id = worker_id;
    session->app_instance_id = app_instance_id;
    session->token = token;
    session->app_token = app_token;
    session->sidecar_token = sidecar_token;
    session->gpu_name = device->name;
    session->op_trace_path = op_trace_path;
    if (display_layout != nullptr) {
        session->display_layout = *display_layout;
        session->has_display_layout = true;
    }
    session->proc = std::move(proc);
    session->last_heartbeat = clock::now();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        // Re-check under the lock: shutdown() may have begun after the early
        // check above. If so, do not insert an unsupervised worker into a
        // torn-down supervisor -- kill the just-spawned process and fail. (If
        // shutdown's terminate pass instead runs after this insert, it kills
        // this session too, because that pass also takes mutex_.)
        if (stopping_.load()) {
            session->proc.terminate();
            throw std::runtime_error("supervisor is shutting down");
        }
        sessions_[worker_id] = std::move(session);
    }
    VKR_INFO(kComponent) << "launched worker " << worker_id << " on " << device->name
                         << " (selector " << gpu_selector << ")";
    return worker_id;
}

control::SessionInfo WorkerSupervisor::launch_session(
    const std::string& app_instance_id, const std::string& gpu_selector,
    const std::string& display_backend, const std::string& graphics_frontend, bool profile_enabled,
    bool op_trace_enabled, bool input_trace_enabled, const display::DisplayLayout* display_layout) {
    (void) display_backend;   // recorded by the app/ICD; not used by the heartbeat-only worker yet
    (void) graphics_frontend; // (display/frontend wiring into the worker comes later)

    std::string gpu_name;
    const bool sidecar_plane = config_.sidecar_plane;
    const std::string app_token = "app-" + mint_token(gpu_selector);
    const std::string worker_id =
        spawn_and_register(gpu_selector, /*heartbeat_count=*/0, app_instance_id, app_token,
                           /*data_plane=*/true, sidecar_plane, profile_enabled, op_trace_enabled,
                           input_trace_enabled, display_layout, gpu_name);

    // Wait until the worker registers and reports its data-plane port (and, when this session
    // carries a sidecar plane, its sidecar-plane port too -- both ports ride one WorkerHello).
    const auto deadline = clock::now() + std::chrono::milliseconds(config_.heartbeat_timeout_ms);
    for (;;) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            Session* s = find_by_worker(worker_id);
            const bool sidecar_ready =
                !sidecar_plane || (s != nullptr && s->sidecar_plane_port > 0);
            if (s != nullptr && s->connected && s->data_plane_port > 0 && sidecar_ready) {
                control::SessionInfo info;
                info.worker_id = worker_id;
                info.data_plane_host = "127.0.0.1";
                info.data_plane_port = s->data_plane_port;
                info.app_token = s->app_token;
                info.op_trace_path = s->op_trace_path;
                if (s->has_display_layout) {
                    info.display_snapshot_id = s->display_layout.snapshot_id;
                }
                if (sidecar_plane) {
                    info.sidecar_plane_host = "127.0.0.1";
                    info.sidecar_plane_port = s->sidecar_plane_port;
                    info.sidecar_token = s->sidecar_token;
                }
                return info;
            }
            if (s != nullptr && s->lifecycle.terminal()) {
                break; // worker died before reporting its data plane
            }
        }
        if (clock::now() >= deadline) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    // Failed to come up: terminate it and report failure.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        Session* s = find_by_worker(worker_id);
        if (s != nullptr) {
            s->proc.terminate();
        }
    }
    throw std::runtime_error("worker " + worker_id + " did not report a data-plane endpoint");
}

bool WorkerSupervisor::close_session(const std::string& app_instance_id,
                                     const std::string& worker_id, const std::string& app_token,
                                     std::string& reason) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        Session* s = find_by_worker(worker_id);
        // Deliberately one generic failure for not-found and credential mismatch: the close
        // endpoint must not become a worker/session enumeration oracle. Both the canonical
        // control-handshake identity and the unguessable per-session app token must match.
        if (s == nullptr || s->app_instance_id != app_instance_id || s->app_token != app_token) {
            reason = "session credentials do not match";
            return false;
        }
        if (s->lifecycle.terminal()) {
            reason = "session already ended"; // idempotent after ordinary app-data EOF
            return true;
        }
        if (!s->launcher_close_requested) {
            VKR_INFO(kComponent) << "launcher requested session close for worker " << worker_id;
            s->launcher_close_requested = true;
            s->launcher_close_requested_at = clock::now();
        }
    }

    // Make SessionClosed an actual teardown acknowledgement, not merely an intent receipt: the
    // shell cleanup may return immediately afterward, so wait until the monitor OBSERVES process
    // death (and therefore the OS has reclaimed every HWND). The monitor grants the short clean
    // EOF grace above, then terminates a placeholder-only worker. Bounded defensively.
    const auto deadline = clock::now() + std::chrono::milliseconds(config_.heartbeat_timeout_ms);
    while (clock::now() < deadline) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            Session* s = find_by_worker(worker_id);
            if (s != nullptr && s->lifecycle.terminal()) {
                reason = "session ended";
                return true;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    reason = "timed out waiting for worker teardown";
    return false;
}

void WorkerSupervisor::accept_loop() {
    while (!stopping_.load()) {
        std::shared_ptr<transport::Connection> conn;
        try {
            conn = wc_listener_->accept();
        } catch (const std::exception&) {
            break; // listener closed during shutdown
        }
        if (stopping_.load()) {
            break;
        }
        {
            std::lock_guard<std::mutex> lock(conns_mutex_);
            conns_.insert(conn);
        }
        std::lock_guard<std::mutex> lock(handlers_mutex_);
        handler_threads_.emplace_back(&WorkerSupervisor::handle_worker_connection, this, conn);
    }
}

void WorkerSupervisor::handle_worker_connection(std::shared_ptr<transport::Connection> conn) {
    // Deregister the connection on the way out, regardless of how we exit.
    const auto deregister = make_scope_guard([this, &conn] {
        std::lock_guard<std::mutex> lock(conns_mutex_);
        conns_.erase(conn);
    });

    transport::MessageChannel channel(*conn);
    protocol::MessageType type = protocol::MessageType::Unknown;
    json::Value body;

    try {
        if (!channel.recv(type, body) || type != protocol::MessageType::WorkerHello) {
            return;
        }
    } catch (const std::exception&) {
        return;
    }
    const protocol::WorkerHello hello = protocol::WorkerHello::from_body(body);

    // Match the one-time token to a session awaiting registration, retrying
    // briefly to cover the window between spawn and registry insertion. The
    // session must be Spawned and not yet connected; on success the token is
    // consumed so a duplicate/late connection with the same token is rejected.
    std::string worker_id;
    display::DisplayLayoutDecodeResult worker_display_layout;
    for (int attempt = 0; attempt < 200 && !stopping_.load(); ++attempt) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            Session* s = find_by_token(hello.token);
            if (s != nullptr && s->worker_id == hello.worker_id &&
                s->lifecycle.state() == protocol::WorkerState::Spawned && !s->connected) {
                if (s->lifecycle.transition(protocol::WorkerState::Handshaking) &&
                    s->lifecycle.transition(protocol::WorkerState::Running)) {
                    worker_id = s->worker_id;
                    s->connected = true;
                    s->last_heartbeat = clock::now();
                    s->data_plane_port = hello.data_plane_port;
                    s->sidecar_plane_port = hello.sidecar_plane_port; // 0 when no plane
                    if (s->has_display_layout) {
                        worker_display_layout.status = display::LayoutDecodeStatus::Valid;
                        worker_display_layout.layout = s->display_layout;
                    }
                    s->token.clear(); // consume: one-time token
                    break;
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    try {
        if (worker_id.empty()) {
            protocol::WorkerAck nak{false, "unknown or already-used worker token", {}};
            channel.send(protocol::MessageType::WorkerAck, nak.to_body());
            return;
        }
        protocol::WorkerAck ack{true, "", {}};
        ack.display_layout = std::move(worker_display_layout);
        channel.send(protocol::MessageType::WorkerAck, ack.to_body());
        VKR_INFO(kComponent) << "worker " << worker_id << " registered";

        for (;;) {
            if (!channel.recv(type, body)) {
                break; // worker closed the channel
            }
            if (type == protocol::MessageType::Heartbeat) {
                std::lock_guard<std::mutex> lock(mutex_);
                Session* s = find_by_worker(worker_id);
                if (s != nullptr) {
                    ++s->heartbeats;
                    s->last_heartbeat = clock::now();
                }
            }
        }
    } catch (const std::exception&) {
        // Fall through to the disconnect bookkeeping below.
    }

    // Record the channel close; the monitor thread owns terminalization (it
    // reaps a clean exit as Exited and kills a worker still alive past the
    // grace period). Doing it here would race the process exit.
    std::lock_guard<std::mutex> lock(mutex_);
    Session* s = find_by_worker(worker_id);
    if (s != nullptr) {
        s->connected = false;
        s->channel_closed = true;
        s->channel_closed_at = clock::now();
    }
}

void WorkerSupervisor::monitor_loop() {
    const auto timeout = std::chrono::milliseconds(config_.heartbeat_timeout_ms);
    const auto grace = std::chrono::milliseconds(kChannelCloseGraceMs);
    while (!stopping_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const auto now = clock::now();
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& kv : sessions_) {
            Session& s = *kv.second;
            if (s.lifecycle.terminal()) {
                continue;
            }

            // The process is GONE (exit OBSERVED, not merely requested). Classify:
            // a requested termination we now see exited is Killed; otherwise a clean exit while
            // Running is Exited, else Crashed. This is the only place a terminal state is entered,
            // so active_count() drops only once the child has actually died (the HWND-reclaim
            // proof).
            int code = 0;
            if (s.proc.valid() && s.proc.wait(0, code)) {
                s.lifecycle.transition(s.termination_requested
                                           ? protocol::WorkerState::Killed
                                           : terminal_state(true, code, s.lifecycle.state()));
                s.connected = false;
                continue;
            }

            // The process is still alive. Decide whether to REQUEST termination.
            const char* reason = nullptr;
            if (s.launcher_close_requested &&
                now - s.launcher_close_requested_at >
                    std::chrono::milliseconds(kLauncherCloseGraceMs)) {
                reason = "launcher closed app session";
            } else if (s.launcher_close_requested) {
                continue; // grace: let an ordinary App-socket EOF exit cleanly first
            } else if (s.channel_closed && now - s.channel_closed_at > grace) {
                // Dropped its control channel but kept running -> misbehaving.
                reason = "control channel dropped while process still alive";
            } else if (s.channel_closed) {
                continue; // within grace: let a clean exit be reaped above
            } else if (s.connected && now - s.last_heartbeat > timeout) {
                reason = "missed heartbeat deadline";
            } else if (!s.connected && now - s.last_heartbeat > timeout) {
                reason = "did not register before deadline";
            }
            if (reason != nullptr) {
                if (!s.termination_requested) {
                    // Request termination but stay NON-terminal: proc.wait() above observes the
                    // real exit on a later pass and only then transitions to Killed. terminate() is
                    // asynchronous, so requesting it is not proof the child died.
                    VKR_WARN(kComponent) << "terminating worker " << s.worker_id << ": " << reason;
                    s.proc.terminate();
                    s.termination_requested = true;
                    s.termination_requested_at = now;
                    s.connected = false;
                } else if (now - s.termination_requested_at >
                           std::chrono::milliseconds(kTerminateEscalateMs)) {
                    // Requested but still not observed exiting -> re-issue + log (not silent).
                    VKR_WARN(kComponent) << "worker " << s.worker_id
                                         << " still alive after terminate request; retrying";
                    s.proc.terminate();
                    s.termination_requested_at = now;
                }
            }
        }
    }
}

std::vector<WorkerStatus> WorkerSupervisor::snapshot() const {
    std::vector<WorkerStatus> out;
    std::lock_guard<std::mutex> lock(mutex_);
    out.reserve(sessions_.size());
    for (const auto& kv : sessions_) {
        const Session& s = *kv.second;
        WorkerStatus status;
        status.worker_id = s.worker_id;
        status.app_instance_id = s.app_instance_id;
        status.gpu_name = s.gpu_name;
        status.state = s.lifecycle.state();
        status.heartbeats = s.heartbeats;
        status.connected = s.connected;
        out.push_back(std::move(status));
    }
    return out;
}

std::size_t WorkerSupervisor::active_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::size_t n = 0;
    for (const auto& kv : sessions_) {
        if (!kv.second->lifecycle.terminal()) {
            ++n;
        }
    }
    return n;
}

long long WorkerSupervisor::total_heartbeats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    long long n = 0;
    for (const auto& kv : sessions_) {
        n += kv.second->heartbeats;
    }
    return n;
}

void WorkerSupervisor::shutdown() {
    if (stopping_.exchange(true)) {
        return;
    }
    if (wc_listener_ != nullptr) {
        wc_listener_->close(); // unblock the accept thread
    }
    // Cancel every accepted connection (worker or not) so any handler blocked
    // in recv is released, then kill the worker children. cancel() does not
    // close -- the handler's owning shared_ptr still closes on the way out.
    {
        std::lock_guard<std::mutex> lock(conns_mutex_);
        for (const auto& conn : conns_) {
            conn->cancel();
        }
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& kv : sessions_) {
            kv.second->proc.terminate(); // kill children
        }
    }
    if (accept_thread_.joinable()) {
        accept_thread_.join();
    }
    if (monitor_thread_.joinable()) {
        monitor_thread_.join();
    }
    std::vector<std::thread> handlers;
    {
        std::lock_guard<std::mutex> lock(handlers_mutex_);
        handlers.swap(handler_threads_);
    }
    for (auto& t : handlers) {
        if (t.joinable()) {
            t.join();
        }
    }
    std::lock_guard<std::mutex> lock(mutex_);
    sessions_.clear();
}

} // namespace vkr::supervisor
