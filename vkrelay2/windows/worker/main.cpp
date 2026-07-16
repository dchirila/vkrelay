// vkrelay2 worker.
//
// Disposable worker with two lifecycle modes:
//   * default: emit heartbeats on stdout (used by the supervisor self-test).
//   * --connect <host:port> --worker-id <id> --token <t>: connect to the
//     supervisor's worker-control channel, register (WorkerHello), then send
//     Heartbeat messages until the count is reached or the supervisor kills us.
#include "common/logging/logging.hpp"
#include "common/protocol/gpu.hpp"
#include "common/protocol/messages.hpp"
#include "common/sidecar/sidecar_session.hpp"
#include "common/transport/tcp.hpp"
#include "common/transport/transport.hpp"
#include "common/vkrpc/rpc.hpp"
#include "common/vkrpc/vulkan_session.hpp"
#include "windows/worker/liveness_observer.hpp"
#if defined(VKRELAY2_HAVE_VULKAN)
#include "windows/worker/real_gpu_probe.hpp"
#include "windows/worker/real_vulkan_backend.hpp"
#endif

#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <utility>

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace {

long long current_pid() {
#if defined(_WIN32)
    return static_cast<long long>(::GetCurrentProcessId());
#else
    return static_cast<long long>(::getpid());
#endif
}

struct Options {
    int count = 5; // number of heartbeats; 0 == run until killed
    int interval_ms = 100;
    std::string id = "worker";
    std::string connect; // host:port for the worker-control channel
    std::string token;
    std::string worker_id;
    std::string gpu;           // selected device name (display; exact key when required + no LUID)
    std::string gpu_luid;      // preferred stable match key (may be empty)
    bool gpu_required = false; // faithful selection: fail closed on a miss (no silent fallback)
    int linger_ms = 0;         // after heartbeats: drop the channel but stay alive this long
    bool wedge_data_plane =
        false;                  // TEST hook: block the established data plane forever, ignoring
                                // abort_session -- simulates an uncooperative dispatcher so the
                                // hard observer->app_session_ended->supervisor-kill path is proven
    std::string app_token;      // token the app must present on the data plane
    bool data_plane = false;    // open a data-plane listener for the app to reconnect to
    std::string sidecar_token;  // token the sidecar must present on the sidecar plane
    bool sidecar_plane = false; // open a sidecar-plane listener for the sidecar to connect to
    std::string vulkan_backend = "mock"; // "mock" | "real" (real needs a Vulkan build)
    bool list_gpus = false;              // enumerate real adapters as JSON on stdout, then exit
};

template <typename F> struct ScopeGuard {
    F f;
    ~ScopeGuard() { f(); }
};
template <typename F> ScopeGuard<F> make_scope_guard(F f) {
    return ScopeGuard<F>{std::move(f)};
}

// How long the data plane waits for an accepted peer to send app_hello before
// dropping it and listening again, so a silent/stalled peer cannot wedge the one
// data-plane connection a session gets. The real app sends immediately.
constexpr int kDataPlaneHelloTimeoutMs = 1000;

bool parse_int(const std::string& s, int& out) {
    try {
        std::size_t pos = 0;
        const int value = std::stoi(s, &pos);
        if (pos != s.size()) {
            return false;
        }
        out = value;
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

bool parse_host_port(const std::string& text, std::string& host, int& port) {
    const std::size_t colon = text.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= text.size()) {
        return false;
    }
    host = text.substr(0, colon);
    return parse_int(text.substr(colon + 1), port) && port > 0;
}

// --list-gpus: enumerate the host's real Vulkan adapters and write the device
// list as JSON to stdout (the supervisor spawns this and parses it; the worker is
// the only process that links Vulkan). Only stdout carries the JSON -- logs go to
// stderr -- so the parse is clean. Returns non-zero when real enumeration is
// unavailable (no Vulkan build, no loader, or no device), so the supervisor falls
// back to its mocked list.
int run_list_gpus() {
#if defined(VKRELAY2_HAVE_VULKAN)
    const std::vector<vkr::protocol::GpuDevice> devices = vkr::worker::probe_real_gpus();
    if (devices.empty()) {
        VKR_WARN("worker") << "no real Vulkan adapters enumerated";
        return 1;
    }
    std::fputs(vkr::protocol::gpu_list_to_body(devices).dump().c_str(), stdout);
    std::fflush(stdout);
    return 0;
#else
    VKR_WARN("worker") << "real GPU enumeration unavailable in this build";
    return 1;
#endif
}

int run_stdout_heartbeats(const Options& opts, long long pid) {
    for (int seq = 1; opts.count == 0 || seq <= opts.count; ++seq) {
        std::printf("HEARTBEAT seq=%d pid=%lld id=%s\n", seq, pid, opts.id.c_str());
        std::fflush(stdout);
        std::this_thread::sleep_for(std::chrono::milliseconds(opts.interval_ms));
    }
    VKR_INFO("worker") << "exiting cleanly";
    return 0;
}

// The single session-owned backend: ONE concrete object that implements BOTH the
// Vulkan data-plane interface (vkrpc::VulkanBackend) and the sidecar-plane interface
// (sidecar::SidecarBackend). The `sidecar` pointer is a non-owning view of the SAME object the
// `vulkan` unique_ptr owns, so both planes share the surface map and the window
// registry -- a separate sidecar backend would split that state and defeat cross-plane consistency.
struct SessionBackend {
    std::unique_ptr<vkr::vkrpc::VulkanBackend> vulkan;
    vkr::sidecar::SidecarBackend* sidecar = nullptr;
};

// Builds the session backend: the real loader-backed one when requested and compiled in, otherwise
// the mock. Both implement both interfaces; the sidecar view is taken from the concrete object at
// construction (a plain upcast -- no dynamic_cast).
SessionBackend make_backend(const std::string& gpu_name,
                            [[maybe_unused]] const std::string& gpu_luid,
                            [[maybe_unused]] bool gpu_required, bool use_real,
                            const vkr::display::DisplayLayout* display_layout) {
#if defined(VKRELAY2_HAVE_VULKAN)
    if (use_real) {
        VKR_INFO("worker") << "real Vulkan backend: gpu='" << gpu_name << "' gpu_luid='" << gpu_luid
                           << "' required=" << (gpu_required ? "yes" : "no");
        auto real = std::make_unique<vkr::worker::RealVulkanBackend>(gpu_name, gpu_luid,
                                                                     gpu_required, display_layout);
        vkr::sidecar::SidecarBackend* sc = real.get();
        return SessionBackend{std::move(real), sc};
    }
#else
    if (use_real) {
        VKR_WARN("worker") << "real Vulkan backend not available in this build; using mock";
    }
#endif
    // The mock ignores the GPU key (it selects no real device); only the name is kept for logs.
    auto mock = std::make_unique<vkr::vkrpc::MockVulkanBackend>(gpu_name, display_layout);
    vkr::sidecar::SidecarBackend* sc = mock.get();
    return SessionBackend{std::move(mock), sc};
}

// Data plane: accept app reconnects until one presents a valid token, then ack
// and serve Vulkan RPCs on that connection until the app disconnects.
// A peer that sends a bad token, the wrong message, or stalls past the handshake
// deadline is rejected and we keep listening, so a stray/buggy/silent first peer
// cannot strand the session. Returns true once an established app has been served
// and has disconnected (the disposable worker's reason for living is gone);
// returns false if the listener was closed first (worker shutdown).
bool run_data_plane(vkr::transport::Listener& listener, const std::string& app_token,
                    vkr::vkrpc::VulkanBackend& backend,
                    const std::function<void()>& on_established_peer_closed,
                    bool wedge_data_plane) {
    for (;;) {
        std::unique_ptr<vkr::transport::Connection> conn;
        try {
            conn = listener.accept();
        } catch (const std::exception&) {
            return false; // listener closed during shutdown
        }
        try {
            // Bound the handshake so a silent peer cannot hold the one slot.
            conn->set_read_timeout(kDataPlaneHelloTimeoutMs);
            vkr::transport::MessageChannel channel(*conn);
            vkr::protocol::MessageType type = vkr::protocol::MessageType::Unknown;
            vkr::json::Value body;
            if (!channel.recv(type, body) || type != vkr::protocol::MessageType::AppHello) {
                vkr::protocol::AppAck nak{false, "expected app_hello"};
                channel.send(vkr::protocol::MessageType::AppAck, nak.to_body());
                continue; // keep listening for the real app
            }
            const vkr::protocol::AppHello hello = vkr::protocol::AppHello::from_body(body);
            const bool ok = (hello.app_token == app_token);
            vkr::protocol::AppAck ack;
            ack.ok = ok;
            ack.reason = ok ? "" : "invalid app token";
            channel.send(vkr::protocol::MessageType::AppAck, ack.to_body());
            VKR_INFO("worker") << "data-plane app " << (ok ? "accepted" : "rejected");
            if (ok) {
                // Established: clear the handshake deadline and switch from the
                // JSON control envelope to the binary Vulkan RPC envelope on the
                // same connection. The app does not pipeline RPCs before app_ack,
                // so the MessageChannel buffer is empty at this point. The backend is
                // the session-owned object (shared with the sidecar plane).
                conn->set_read_timeout(0);
                // An out-of-band liveness observer watches THIS connection for peer-close
                // on its own thread. On app-death it fires TWO independent paths:
                //   HARD  -- on_established_peer_closed() sets the main loop's app_session_ended
                //            DIRECTLY, so the heartbeat loop breaks, the worker-control channel
                //            closes, and the supervisor's 1 s kill deadline ARMS -- even if the
                //            dispatcher never returns (a driver that ignores the acquire timeout or
                //            is stuck in another Vulkan call). This is the load-bearing guarantee.
                //   GRACEFUL -- backend.abort_session() releases the in-flight acquire poll so the
                //            worker usually exits cleanly within a quantum, before the kill
                //            deadline.
                // wait_peer_closed is non-consuming (safe beside serve_vulkan_rpc's reads); bounded
                // by the quantum, so it joins promptly on a normal EOF.
                std::atomic<bool> observer_stop{false};
                vkr::transport::Connection* app_conn = conn.get();
                std::thread observer([app_conn, &backend, &observer_stop,
                                      &on_established_peer_closed]() {
                    vkr::worker::run_liveness_observer(
                        *app_conn, observer_stop, vkr::worker::kLivenessQuantumMs,
                        [&backend, &on_established_peer_closed]() {
                            on_established_peer_closed(); // HARD path (independent of dispatcher)
                            backend.abort_session();      // GRACEFUL path
                        });
                });
                if (wedge_data_plane) {
                    // TEST hook: an uncooperative dispatcher that IGNORES abort
                    // and never returns. The graceful path (abort_session) does nothing here, so
                    // only the HARD path can end the worker: the observer's
                    // on_established_peer_closed() sets app_session_ended, the main loop breaks +
                    // closes worker-control, and the supervisor kills this process. This loop never
                    // exits on its own -- the process is terminated by the supervisor (or the test
                    // harness).
                    while (true) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    }
                }
                // serve_vulkan_rpc may THROW (e.g. a response send failing on the dead peer once
                // the abort unblocked the in-flight acquire) -- the session is over either way.
                // Catch so the observer is ALWAYS joined (a joinable std::thread destructor would
                // terminate) before conn is destroyed.
                try {
                    vkr::vkrpc::RpcChannel rpc(*conn);
                    vkr::vkrpc::serve_vulkan_rpc(rpc, backend);
                } catch (const std::exception&) {
                }
                observer_stop.store(true, std::memory_order_relaxed);
                observer.join(); // conn outlives the observer (joined before it is destroyed)
                return true;     // app disconnected; the session is done
            }
            // Wrong token: drop this peer and keep listening for the real app.
        } catch (const std::exception&) {
            // Peer error, handshake timeout, or shutdown: drop and keep listening.
        }
    }
}

// Sidecar plane: accept sidecar reconnects until one presents a valid sidecar_token,
// then ack and serve sidecar RPCs on that connection (over the session-owned backend) until the
// sidecar disconnects. Mirrors run_data_plane's reject-and-keep-listening discipline. Unlike the
// data plane, the sidecar does NOT own session lifetime (the app data plane does), so this keeps
// listening for a sidecar reconnect and only returns when the listener is closed at teardown.
void run_sidecar_plane(vkr::transport::Listener& listener, const std::string& sidecar_token,
                       vkr::sidecar::SidecarBackend& backend,
                       const vkr::display::DisplayLayoutDecodeResult& display_layout) {
    for (;;) {
        std::unique_ptr<vkr::transport::Connection> conn;
        try {
            conn = listener.accept();
        } catch (const std::exception&) {
            return; // listener closed during shutdown
        }
        try {
            conn->set_read_timeout(kDataPlaneHelloTimeoutMs);
            vkr::transport::MessageChannel channel(*conn);
            vkr::protocol::MessageType type = vkr::protocol::MessageType::Unknown;
            vkr::json::Value body;
            if (!channel.recv(type, body) || type != vkr::protocol::MessageType::SidecarHello) {
                vkr::protocol::SidecarAck nak;
                nak.reason = "expected sidecar_hello";
                channel.send(vkr::protocol::MessageType::SidecarAck, nak.to_body());
                continue; // keep listening for the real sidecar
            }
            const vkr::protocol::SidecarHello hello = vkr::protocol::SidecarHello::from_body(body);
            const bool ok = (hello.sidecar_token == sidecar_token);
            vkr::protocol::SidecarAck ack;
            ack.ok = ok;
            ack.reason = ok ? "" : "invalid sidecar token";
            if (ok) {
                ack.display_layout = display_layout;
            }
            channel.send(vkr::protocol::MessageType::SidecarAck, ack.to_body());
            VKR_INFO("worker") << "sidecar-plane " << (ok ? "accepted" : "rejected");
            if (ok) {
                // Established: switch to the binary RPC envelope and serve the sidecar's OWN op
                // space (NOT the Vulkan dispatcher -- structural session-mode separation).
                conn->set_read_timeout(0);
                vkr::vkrpc::RpcChannel rpc(*conn);
                vkr::sidecar::serve_sidecar_rpc(rpc, backend);
                continue; // sidecar disconnected; keep the plane open for a reconnect
            }
            // Wrong token: drop this peer and keep listening.
        } catch (const std::exception&) {
            // Peer error, handshake timeout, or shutdown: drop and keep listening.
        }
    }
}

// Connects to the supervisor's worker-control channel, registers (reporting the
// data-plane port when enabled), then heartbeats. Returns 0 on a clean finish
// (including the supervisor closing the channel, i.e. a kill), non-zero on a
// registration failure.
int run_worker_control(const Options& opts, long long pid) {
    std::string host;
    int port = 0;
    if (!parse_host_port(opts.connect, host, port)) {
        VKR_ERROR("worker") << "invalid --connect endpoint: " << opts.connect;
        return 2;
    }

    // The single session-owned backend is constructed after the authenticated WorkerAck delivers
    // the full pinned layout, but before either plane thread starts. The listeners may already be
    // bound so their ports can ride WorkerHello; their backlogs safely cover this short interval.
    SessionBackend session;

    std::unique_ptr<vkr::transport::Listener> dp_listener;
    int dp_port = 0;
    std::thread dp_thread;
    // Set when the established app's RPC stream ends: the disposable worker then
    // ends its session (App socket EOF closes the worker session, per the
    // process-lifecycle contract).
    std::atomic<bool> app_session_ended{false};
    if (opts.data_plane) {
        dp_listener = vkr::transport::tcp_listen(0);
        dp_port = dp_listener->port();
    }

    std::unique_ptr<vkr::transport::Listener> sc_listener;
    int sc_port = 0;
    std::thread sc_thread;
    if (opts.sidecar_plane) {
        sc_listener = vkr::transport::tcp_listen(0);
        sc_port = sc_listener->port();
    }

    const auto plane_cleanup = make_scope_guard([&]() {
        if (dp_listener) {
            dp_listener->close(); // unblock run_data_plane's accept
        }
        if (sc_listener) {
            sc_listener->close(); // unblock run_sidecar_plane's accept
        }
        if (dp_thread.joinable()) {
            dp_thread.join();
        }
        if (sc_thread.joinable()) {
            sc_thread.join();
        }
    });

    try {
        auto conn = vkr::transport::tcp_connect(host, port);
        vkr::transport::MessageChannel channel(*conn);

        vkr::protocol::WorkerHello hello;
        hello.worker_id = opts.worker_id;
        hello.token = opts.token;
        hello.data_plane_port = dp_port;
        hello.sidecar_plane_port = sc_port;
        channel.send(vkr::protocol::MessageType::WorkerHello, hello.to_body());

        vkr::protocol::MessageType type = vkr::protocol::MessageType::Unknown;
        vkr::json::Value body;
        if (!channel.recv(type, body) || type != vkr::protocol::MessageType::WorkerAck) {
            VKR_ERROR("worker") << "supervisor did not acknowledge registration";
            return 3;
        }
        const vkr::protocol::WorkerAck ack = vkr::protocol::WorkerAck::from_body(body);
        if (!ack.ok) {
            VKR_ERROR("worker") << "registration rejected: " << ack.reason;
            return 3;
        }
        if (ack.display_layout.status == vkr::display::LayoutDecodeStatus::Malformed ||
            ack.display_layout.status == vkr::display::LayoutDecodeStatus::UnsupportedSchema) {
            VKR_ERROR("worker") << "registration carried unusable display layout: "
                                << ack.display_layout.reason;
            return 3;
        }
        const vkr::display::DisplayLayout* display_layout = nullptr;
        if (ack.display_layout.status == vkr::display::LayoutDecodeStatus::Valid) {
            display_layout = &ack.display_layout.layout;
            VKR_INFO("worker") << "pinned display snapshot " << display_layout->snapshot_id
                               << " virtual=" << display_layout->virtual_bounds.width << "x"
                               << display_layout->virtual_bounds.height
                               << " origin=" << display_layout->virtual_bounds.x << ","
                               << display_layout->virtual_bounds.y;
        }
        if (opts.data_plane || opts.sidecar_plane) {
            session = make_backend(opts.gpu, opts.gpu_luid, opts.gpu_required,
                                   opts.vulkan_backend == "real", display_layout);
        }
        if (opts.data_plane) {
            vkr::transport::Listener* dpl = dp_listener.get();
            const std::string app_token = opts.app_token;
            const bool wedge = opts.wedge_data_plane;
            vkr::vkrpc::VulkanBackend* backend = session.vulkan.get();
            dp_thread = std::thread([dpl, app_token, backend, wedge, &app_session_ended]() {
                const auto on_peer_closed = [&app_session_ended]() {
                    app_session_ended.store(true);
                };
                if (run_data_plane(*dpl, app_token, *backend, on_peer_closed, wedge)) {
                    app_session_ended.store(true);
                }
            });
        }
        if (opts.sidecar_plane) {
            vkr::transport::Listener* scl = sc_listener.get();
            const std::string sidecar_token = opts.sidecar_token;
            vkr::sidecar::SidecarBackend* backend = session.sidecar;
            const vkr::display::DisplayLayoutDecodeResult sidecar_layout = ack.display_layout;
            sc_thread = std::thread([scl, sidecar_token, backend, sidecar_layout]() {
                run_sidecar_plane(*scl, sidecar_token, *backend, sidecar_layout);
            });
        }

        VKR_INFO("worker") << "registered with supervisor"
                           << "; worker_id=" << opts.worker_id << " gpu=" << opts.gpu
                           << " pid=" << pid;

        for (int seq = 1; opts.count == 0 || seq <= opts.count; ++seq) {
            if (app_session_ended.load()) {
                VKR_INFO("worker") << "app disconnected; ending session";
                break;
            }
            vkr::protocol::Heartbeat hb;
            hb.worker_id = opts.worker_id;
            hb.seq = seq;
            channel.send(vkr::protocol::MessageType::Heartbeat, hb.to_body());
            std::this_thread::sleep_for(std::chrono::milliseconds(opts.interval_ms));
        }
        if (opts.linger_ms > 0) {
            // Simulate a misbehaving worker: drop the control channel but keep
            // the process alive. The supervisor must detect this and kill us.
            conn->close();
            std::this_thread::sleep_for(std::chrono::milliseconds(opts.linger_ms));
        }
    } catch (const vkr::transport::TransportError& e) {
        // The supervisor closing the channel (e.g. a kill) surfaces here; treat
        // it as a clean end of the worker's life.
        VKR_INFO("worker") << "worker-control channel closed: " << e.what();
        return 0;
    }

    VKR_INFO("worker") << "exiting cleanly";
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    vkr::log::set_component("worker");

    Options opts;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto take_int = [&](int& dst) -> bool {
            if (i + 1 >= argc || !parse_int(argv[i + 1], dst)) {
                VKR_ERROR("worker") << "missing/invalid value for " << arg;
                return false;
            }
            ++i;
            return true;
        };
        auto take_str = [&](std::string& dst) -> bool {
            if (i + 1 >= argc) {
                VKR_ERROR("worker") << "missing value for " << arg;
                return false;
            }
            dst = argv[++i];
            return true;
        };
        if (arg == "--count") {
            if (!take_int(opts.count)) {
                return 2;
            }
        } else if (arg == "--interval-ms") {
            if (!take_int(opts.interval_ms)) {
                return 2;
            }
        } else if (arg == "--id") {
            if (!take_str(opts.id)) {
                return 2;
            }
        } else if (arg == "--connect") {
            if (!take_str(opts.connect)) {
                return 2;
            }
        } else if (arg == "--token") {
            if (!take_str(opts.token)) {
                return 2;
            }
        } else if (arg == "--worker-id") {
            if (!take_str(opts.worker_id)) {
                return 2;
            }
        } else if (arg == "--gpu") {
            if (!take_str(opts.gpu)) {
                return 2;
            }
        } else if (arg == "--gpu-luid") {
            if (!take_str(opts.gpu_luid)) {
                return 2;
            }
        } else if (arg == "--gpu-required") {
            opts.gpu_required = true;
        } else if (arg == "--linger-ms") {
            if (!take_int(opts.linger_ms)) {
                return 2;
            }
        } else if (arg == "--app-token") {
            if (!take_str(opts.app_token)) {
                return 2;
            }
        } else if (arg == "--data-plane") {
            opts.data_plane = true;
        } else if (arg == "--wedge-data-plane") {
            opts.wedge_data_plane = true; // TEST hook (see Options::wedge_data_plane)
        } else if (arg == "--sidecar-token") {
            if (!take_str(opts.sidecar_token)) {
                return 2;
            }
        } else if (arg == "--sidecar-plane") {
            opts.sidecar_plane = true;
        } else if (arg == "--list-gpus") {
            opts.list_gpus = true;
        } else if (arg == "--vulkan-backend") {
            if (!take_str(opts.vulkan_backend)) {
                return 2;
            }
            if (opts.vulkan_backend != "mock" && opts.vulkan_backend != "real") {
                VKR_ERROR("worker")
                    << "invalid --vulkan-backend (want mock|real): " << opts.vulkan_backend;
                return 2;
            }
        } else {
            VKR_ERROR("worker") << "unknown argument: " << arg;
            return 2;
        }
    }

    // Enumerate-and-exit mode: keep stdout to the JSON list only, so emit it
    // before any startup logging (which goes to stderr anyway).
    if (opts.list_gpus) {
        return run_list_gpus();
    }

    const long long pid = current_pid();
    VKR_INFO("worker") << "starting"
                       << "; id=" << opts.id;
    VKR_INFO("worker").kv("pid", pid).kv("count", opts.count).kv("interval_ms", opts.interval_ms);

    if (!opts.connect.empty()) {
        return run_worker_control(opts, pid);
    }
    return run_stdout_heartbeats(opts, pid);
}
