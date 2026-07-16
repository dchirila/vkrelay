#include "common/launch/launch_cli.hpp"

#include "common/control/daemon_endpoint.hpp"
#include "common/process/process.hpp"
#include "common/protocol/gpu.hpp"
#include "common/protocol/launch_descriptor.hpp"
#include "common/protocol/messages.hpp"
#include "common/transport/tcp.hpp"
#include "common/transport/transport.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <utility>

// app-lifetime-teardown: the POSIX launcher owns the app's signals so the app cannot outlive it.
// The relay app-run is Linux-only; on Windows the managed helper degrades to a plain spawn+wait.
#ifndef _WIN32
#include <atomic>
#include <chrono>
#include <csignal>
#include <ctime>
#include <pthread.h>
#include <sys/types.h>
#include <thread>
#include <unistd.h>
#endif

namespace vkr::launch {
namespace {

using protocol::LaunchDescriptor;

std::string read_file_bytes(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("cannot read file: " + path);
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Splits a NUL-separated payload into fields, dropping a single trailing empty
// field if the payload ends with a separator.
std::vector<std::string> split_nul(const std::string& data) {
    std::vector<std::string> out;
    std::string current;
    for (char c : data) {
        if (c == '\0') {
            out.push_back(current);
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    if (!current.empty()) {
        out.push_back(current);
    }
    return out;
}

std::string strip_trailing_newline(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) {
        s.pop_back();
    }
    return s;
}

void print_usage() {
    std::puts("vkrelay2-launch - build/run a structured launch descriptor\n");
    std::puts("Usage:");
    std::puts("  vkrelay2-launch [options] -- <app> [args...]");
    std::puts("  vkrelay2-launch [options] --argv-file <nul-file> [--run|...]");
    std::puts("  vkrelay2-launch --run-descriptor <descriptor.json>");
    std::puts("  vkrelay2-launch --list-gpus [--daemon <host:port>]");
    std::puts("  vkrelay2-launch --ping [--daemon <host:port>]");
    std::puts("  vkrelay2-launch --query-host-display [--daemon <host:port>]");
    std::puts("  vkrelay2-launch --close-session --worker-id <id> --app-token <token>");
    std::puts("      (--query-host-display: print the daemon's host display space as KEY=VALUE");
    std::puts("       lines -- VKRELAY2_HOST_WORK_W/H + VKRELAY2_HOST_DPI; nothing if unknown)");
    std::puts("      (--ping: handshake health check -- exit 0 iff the daemon's control plane");
    std::puts("       answered; distinguishes a healthy daemon from a wedged one)");
    std::puts("      (no --daemon: uses the well-known endpoint 127.0.0.1:13579,");
    std::puts("       override via VKRELAY2_DAEMON_HOST / VKRELAY2_DAEMON_PORT)\n");
    std::puts("Options:");
    std::puts("  --gpu <selector>          GPU selector (auto, high-performance, ...)");
    std::puts("  --display <auto|x11|wayland>");
    std::puts("  --frontend <auto|vulkan13|opengl46zink>");
    std::puts("  --op-trace                session-scoped decoded worker-op trace");
    std::puts("  --close-session           terminate one authenticated launched session");
    std::puts("  --worker-id <id>          worker returned by --open-session");
    std::puts("  --app-token <token>       session token returned by --open-session");
    std::puts("  --cwd <path> | --cwd-file <path>");
    std::puts("  --env KEY=VALUE           (repeatable)");
    std::puts("  --env-file <nul-file>     NUL-separated KEY=VALUE entries");
    std::puts("  --argv-file <nul-file>    NUL-separated argv (instead of -- ...)");
    std::puts("  --descriptor-out <path>   write the descriptor JSON");
    std::puts("  --print-descriptor        print the descriptor JSON to stdout");
    std::puts("  --run                     round-trip the descriptor and spawn the app");
    std::puts("  --run-descriptor <path>   read a descriptor file and spawn the app");
}

// A tight read deadline for the throwaway --ping health probe -- it must detect
// a wedged daemon FAST (it runs on the launch hot path to decide reuse), not wait the full control
// deadline. The shell daemon_healthy() also wraps it with its own `timeout` belt.
constexpr int kPingTimeoutMs = 3000;

// The one testing escape hatch for printing MOCKED adapter lists as if they were
// hardware: --list-gpus (daemon-reported mock AND the offline local fallback) refuses
// by default, because a fake "NVIDIA T1200" on an Intel-only box reads as real and
// sends the user chasing the wrong problem.
bool allow_mock_gpus() {
    return std::getenv("VKRELAY2_ALLOW_MOCK_GPUS") != nullptr;
}

// Queries a running supervisor (--serve) for its adapter list over the control
// protocol, proving --list-gpus can resolve against the daemon, not WSL.
int query_daemon_list_gpus(const std::string& host, int port) {
    auto conn = transport::tcp_connect(host, port);
    // Bound every control read so a wedged/slow daemon throws instead of hanging the command.
    conn->set_read_timeout(control::default_control_timeout_ms());
    transport::MessageChannel channel(*conn);
    protocol::ClientHello hello;
    hello.app_instance_id = "vkrelay2-launch";
    transport::client_handshake(channel, hello);

    channel.send(protocol::MessageType::GpuListRequest, json::Value::make_object());
    protocol::MessageType type = protocol::MessageType::Unknown;
    json::Value body;
    if (!channel.recv(type, body) || type != protocol::MessageType::GpuListResponse) {
        throw std::runtime_error("daemon did not return a gpu list");
    }
    // A daemon that STAMPS its list as mock (real=false) has no real enumeration --
    // typically a Windows worker built without the Vulkan SDK. Refuse to print fake
    // adapters as hardware; the exit code and message carry the actual problem.
    // (A legacy daemon omits the stamp and prints as before -- see gpu_list_marked_mock.)
    if (protocol::gpu_list_marked_mock(body) && !allow_mock_gpus()) {
        std::fprintf(stderr,
                     "vkrelay2-launch: the daemon at %s:%d serves a MOCKED adapter list (its "
                     "worker has no real Vulkan enumeration -- typically a Windows build "
                     "without the Vulkan SDK).\nRefusing to print mock adapters. Rebuild the "
                     "Windows worker with the Vulkan SDK (see docs/building.md), restart the "
                     "daemon, and retry; or set VKRELAY2_ALLOW_MOCK_GPUS=1 (testing only).\n",
                     host.c_str(), port);
        return 3;
    }
    const std::vector<protocol::GpuDevice> devices = protocol::gpu_list_from_body(body);
    std::fputs(protocol::format_gpu_list(devices).c_str(), stdout);
    return 0;
}

// Protocol-level health check: a plain TCP connect only proves the listen
// backlog accepts -- it cannot tell a healthy daemon from one whose one-at-a-time control loop is
// wedged behind a stalled session. This does a real Hello/HelloAck round trip with a tight
// deadline: returns 0 iff the daemon answered the handshake, non-zero (connect refused, timed out,
// or protocol error) otherwise. The launcher's ensure_daemon uses it to decide reuse vs. "wedged
// daemon; restart it".
int ping_daemon(const std::string& host, int port) {
    try {
        auto conn = transport::tcp_connect(host, port, kPingTimeoutMs);
        conn->set_read_timeout(kPingTimeoutMs);
        transport::MessageChannel channel(*conn);
        protocol::ClientHello hello;
        hello.app_instance_id = "vkrelay2-ping";
        transport::client_handshake(channel, hello); // throws on Error / timeout / version mismatch
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "vkrelay2-launch: daemon health check failed at %s:%d: %s\n",
                     host.c_str(), port, e.what());
        return 1;
    }
}

bool parse_positive_decimal(const char* text, int& out) {
    if (text == nullptr || text[0] == '\0') {
        return false;
    }
    char* end = nullptr;
    const long value = std::strtol(text, &end, 10);
    if (end == text || *end != '\0' || value <= 0 || value > 2147483647L) {
        return false;
    }
    out = static_cast<int>(value);
    return true;
}

// Query the daemon's immutable virtual-desktop snapshot and print its ID + canvas as
// KEY=VALUE lines. Legacy primary-work metadata is still printed for old supervisors/launchers.
// Connection/handshake or present-but-unusable topology failure is non-zero (fail closed).
int query_host_display(const std::string& host, int port) {
    try {
        auto conn = transport::tcp_connect(host, port, kPingTimeoutMs);
        conn->set_read_timeout(kPingTimeoutMs);
        transport::MessageChannel channel(*conn);
        protocol::ClientHello hello;
        hello.app_instance_id = "vkrelay2-query-host-display";
        const protocol::ServerHello server = transport::client_handshake(channel, hello);
        if (server.display_layout.status == display::LayoutDecodeStatus::Malformed ||
            server.display_layout.status == display::LayoutDecodeStatus::UnsupportedSchema) {
            throw std::runtime_error("daemon advertised an unusable display layout: " +
                                     server.display_layout.reason);
        }
        if (server.display_layout.status == display::LayoutDecodeStatus::Valid) {
            const display::DisplayLayout& layout = server.display_layout.layout;
            std::printf("VKRELAY2_DISPLAY_SNAPSHOT_ID=%s\n", layout.snapshot_id.c_str());
            std::printf("VKRELAY2_HOST_VIRTUAL_W=%d\n", layout.virtual_bounds.width);
            std::printf("VKRELAY2_HOST_VIRTUAL_H=%d\n", layout.virtual_bounds.height);
        }
        if (server.host_display.work_w > 0 && server.host_display.work_h > 0) {
            std::printf("VKRELAY2_HOST_WORK_W=%d\n", server.host_display.work_w);
            std::printf("VKRELAY2_HOST_WORK_H=%d\n", server.host_display.work_h);
            if (server.host_display.dpi > 0) {
                std::printf("VKRELAY2_HOST_DPI=%d\n", server.host_display.dpi);
            }
        }
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "vkrelay2-launch: host display query failed at %s:%d: %s\n",
                     host.c_str(), port, e.what());
        return 1;
    }
}

// Establishes a worker session against the daemon: Hello handshake ->
// LaunchSession -> SessionStarted, then prints the data-plane endpoint + token as KEY=VALUE
// lines on stdout for the launcher to export into the app's environment (the ICD reads them at
// vkCreateInstance). Fail-closed: any problem throws (the caller returns non-zero) so the app
// is never launched against the system Vulkan stack.
int open_session(const std::string& host, int port, const std::string& app_id,
                 const std::string& selector, const std::string& display_backend,
                 const std::string& graphics_frontend, const std::string& require_worker_backend,
                 bool profile_enabled, bool op_trace_enabled, bool input_trace_enabled) {
    auto conn = transport::tcp_connect(host, port);
    // Bound every control read (HelloAck, then SessionStarted) so a wedged
    // daemon
    // -- e.g. one whose single control loop is stuck behind a stale session -- makes --open-session
    // FAIL CLOSED with a timeout instead of hanging the whole launcher bring-up.
    conn->set_read_timeout(control::default_control_timeout_ms());
    transport::MessageChannel channel(*conn);
    protocol::ClientHello hello;
    hello.app_instance_id = app_id;
    hello.gpu_selector = selector;
    hello.display_backend = display_backend;
    hello.graphics_frontend = graphics_frontend;
    const protocol::ServerHello server = transport::client_handshake(channel, hello);

    if (server.display_layout.status == display::LayoutDecodeStatus::Malformed ||
        server.display_layout.status == display::LayoutDecodeStatus::UnsupportedSchema) {
        throw std::runtime_error("daemon advertised an unusable display layout: " +
                                 server.display_layout.reason);
    }

    // Enforce the required session-worker backend BEFORE launching: the daemon advertises it in the
    // handshake, so a mismatch fails closed here and never sends LaunchSession -- otherwise we
    // would spawn an idle worker the app will never use (an app-run against a mock daemon would
    // leave a stale session). The shell helper keeps its own check as a defensive belt.
    if (!require_worker_backend.empty() && server.worker_backend != require_worker_backend) {
        std::fprintf(stderr,
                     "vkrelay2-launch: daemon launches '%s' session workers, not '%s' -- not "
                     "opening a session.\nRestart the daemon with: vkrelay2-supervisor --serve "
                     "--vulkan-backend %s\n",
                     server.worker_backend.c_str(), require_worker_backend.c_str(),
                     require_worker_backend.c_str());
        return 3;
    }

    protocol::LaunchSession req;
    req.app_instance_id = app_id;
    req.gpu_selector = selector;
    req.display_backend = display_backend;
    req.graphics_frontend = graphics_frontend;
    req.profile_enabled = profile_enabled; // session-scoped (additive; old daemon = off)
    req.op_trace_enabled = op_trace_enabled;
    req.input_trace_enabled = input_trace_enabled;
    const char* queried_snapshot = std::getenv("VKRELAY2_DISPLAY_SNAPSHOT_ID");
    if (queried_snapshot != nullptr) {
        req.display_snapshot_id = queried_snapshot;
    } else if (server.display_layout.status == display::LayoutDecodeStatus::Valid) {
        // Direct --open-session callers have no pre-start query. Pin this handshake's snapshot;
        // SidecarReady will still fail closed if their already-created root has different bounds.
        req.display_snapshot_id = server.display_layout.layout.snapshot_id;
    }

    auto launch_once = [&](protocol::SessionStarted& started, protocol::ErrorMsg& error) {
        channel.send(protocol::MessageType::LaunchSession, req.to_body());
        protocol::MessageType type = protocol::MessageType::Unknown;
        json::Value body;
        if (!channel.recv(type, body)) {
            throw std::runtime_error("daemon closed before session_started");
        }
        if (type == protocol::MessageType::Error) {
            error = protocol::ErrorMsg::from_body(body);
            return false;
        }
        if (type != protocol::MessageType::SessionStarted) {
            throw std::runtime_error("daemon did not return session_started");
        }
        started = protocol::SessionStarted::from_body(body);
        return true;
    };

    protocol::SessionStarted s;
    protocol::ErrorMsg launch_error;
    if (!launch_once(s, launch_error)) {
        if (launch_error.code != "display_snapshot_not_found" ||
            server.display_layout.status != display::LayoutDecodeStatus::Valid) {
            throw std::runtime_error("daemon rejected launch_session: " + launch_error.code + " (" +
                                     launch_error.message + ")");
        }

        // One bounded re-query/retry: this HelloAck is a freshly captured cache entry. It is safe
        // to substitute only when it describes the canvas Weston already created from the earlier
        // query. A dimension change requires a full display restart, not an in-place retry.
        const char* expected_w_text = std::getenv("VKRELAY2_HOST_VIRTUAL_W");
        const char* expected_h_text = std::getenv("VKRELAY2_HOST_VIRTUAL_H");
        int expected_w = 0;
        int expected_h = 0;
        const display::RectI32& fresh = server.display_layout.layout.virtual_bounds;
        if (expected_w_text == nullptr || expected_h_text == nullptr ||
            !parse_positive_decimal(expected_w_text, expected_w) ||
            !parse_positive_decimal(expected_h_text, expected_h) || expected_w != fresh.width ||
            expected_h != fresh.height) {
            throw std::runtime_error(
                "display snapshot expired and the fresh topology does not match the running guest "
                "root; restart the session");
        }
        req.display_snapshot_id = server.display_layout.layout.snapshot_id;
        std::fprintf(stderr,
                     "vkrelay2-launch: display snapshot expired; retrying once with fresh "
                     "same-size snapshot %s\n",
                     req.display_snapshot_id.c_str());
        launch_error = {};
        if (!launch_once(s, launch_error)) {
            throw std::runtime_error("daemon rejected the one bounded display-snapshot retry: " +
                                     launch_error.code + " (" + launch_error.message + ")");
        }
    }
    if (s.data_plane_port <= 0 || s.app_token.empty()) {
        throw std::runtime_error("daemon returned an incomplete session (no data plane/token)");
    }
    if (!req.display_snapshot_id.empty() && s.display_snapshot_id != req.display_snapshot_id) {
        throw std::runtime_error("daemon did not confirm the requested display snapshot");
    }
    const std::string dp_host = s.data_plane_host.empty() ? host : s.data_plane_host;
    std::printf("VKRELAY2_WORKER_ID=%s\n", s.worker_id.c_str());
    std::printf("VKRELAY2_DATA_PLANE_HOST=%s\n", dp_host.c_str());
    std::printf("VKRELAY2_DATA_PLANE_PORT=%d\n", s.data_plane_port);
    std::printf("VKRELAY2_APP_TOKEN=%s\n", s.app_token.c_str());
    // The daemon's session-worker backend (advertised at the handshake), so the launcher can fail
    // closed for an app-run against a mock daemon rather than launch a no-real-window session.
    std::printf("VKRELAY2_WORKER_BACKEND=%s\n", server.worker_backend.c_str());
    if (!s.display_snapshot_id.empty()) {
        std::printf("VKRELAY2_DISPLAY_SNAPSHOT_ID=%s\n", s.display_snapshot_id.c_str());
    }
    // Sidecar plane: when the session carries one, expose its endpoint + token so the
    // launcher can start a sidecar (the X11 WM) against the worker and gate the app on its
    // readiness. Absent/0 (the default until the daemon offers a sidecar plane) -> the launcher
    // simply skips it.
    if (s.sidecar_plane_port > 0 && !s.sidecar_token.empty()) {
        const std::string sc_host = s.sidecar_plane_host.empty() ? host : s.sidecar_plane_host;
        std::printf("VKRELAY2_SIDECAR_PLANE_HOST=%s\n", sc_host.c_str());
        std::printf("VKRELAY2_SIDECAR_PLANE_PORT=%d\n", s.sidecar_plane_port);
        std::printf("VKRELAY2_SIDECAR_TOKEN=%s\n", s.sidecar_token.c_str());
    }
    if (!s.op_trace_path.empty()) {
        std::printf("VKRELAY2_WORKER_OP_TRACE_PATH=%s\n", s.op_trace_path.c_str());
    }
    return 0;
}

// Explicit app-lifetime teardown for a worker whose target may never load the Vulkan ICD (and
// therefore never produce the AppHello/App-socket EOF that normally owns worker lifetime).
int close_session(const std::string& host, int port, const std::string& app_id,
                  const std::string& worker_id, const std::string& app_token) {
    if (worker_id.empty() || app_token.empty()) {
        throw std::runtime_error("--close-session requires --worker-id and --app-token");
    }
    auto conn = transport::tcp_connect(host, port);
    conn->set_read_timeout(control::default_control_timeout_ms());
    transport::MessageChannel channel(*conn);
    protocol::ClientHello hello;
    hello.app_instance_id = app_id;
    transport::client_handshake(channel, hello);

    protocol::CloseSession req;
    req.worker_id = worker_id;
    req.app_token = app_token;
    channel.send(protocol::MessageType::CloseSession, req.to_body());
    protocol::MessageType type = protocol::MessageType::Unknown;
    json::Value body;
    if (!channel.recv(type, body) || type != protocol::MessageType::SessionClosed) {
        throw std::runtime_error("daemon did not return session_closed");
    }
    const protocol::SessionClosed closed = protocol::SessionClosed::from_body(body);
    if (!closed.accepted) {
        std::fprintf(stderr, "vkrelay2-launch: session close rejected for %s: %s\n",
                     worker_id.c_str(), closed.reason.c_str());
        return 1;
    }
    return 0;
}

// Splits "host:port" (port required).
bool parse_host_port(const std::string& text, std::string& host, int& port) {
    const std::size_t colon = text.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= text.size()) {
        return false;
    }
    host = text.substr(0, colon);
    port = std::atoi(text.substr(colon + 1).c_str());
    return port > 0;
}

bool validate_enum(const std::string& value, const char* const* allowed, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
        if (value == allowed[i]) {
            return true;
        }
    }
    return false;
}

// app-lifetime-teardown: run the app under launcher signal OWNERSHIP so it CANNOT outlive the
// launcher. The Windows worker ends its session (and destroys its HWNDs) when the app's data-plane
// socket closes; that only happens if the app process actually dies. An app that ignores/survives
// SIGINT or hangs (e.g. Blender after a failed frame) would otherwise keep the socket open on
// CTRL+C and leak the Windows-side window. So on POSIX we: block SIGINT/SIGTERM/SIGHUP (so a
// sigwait thread owns them), spawn the app in its OWN process group with a clean signal environment
// (reset_child_signals, so our blocked mask does not poison the app), and on any launcher signal
// gracefully terminate the app GROUP -- SIGTERM (a cooperative app runs its clean Vulkan teardown
// -> the worker destroys the HWND via RPC), a short grace, then SIGKILL. Both the streaming and
// captured-output paths route through here (the signal can arrive while blocked in either wait() or
// read_stdout_to_end()). On Windows (the relay app-run is Linux-only) it degrades to spawn+wait in
// a Job Object.
int run_managed_child(process::SpawnRequest req, const std::string& output_path) {
    req.new_group = true; // own group so the whole app subtree dies as a unit
#ifndef _WIN32
    req.reset_child_signals = true; // do not leak our blocked signal mask into the app
    sigset_t block;
    sigemptyset(&block);
    sigaddset(&block, SIGINT);
    sigaddset(&block, SIGTERM);
    sigaddset(&block, SIGHUP);
    sigset_t prev;
    ::pthread_sigmask(SIG_BLOCK, &block, &prev);
    // RAII: restore the launcher's signal mask on EVERY exit path -- including a Process::spawn
    // throw below, so a spawn failure never leaves the launcher process with
    // INT/TERM/HUP blocked (a trap for future in-process / library-style callers).
    struct MaskGuard {
        sigset_t prev;
        ~MaskGuard() { ::pthread_sigmask(SIG_SETMASK, &prev, nullptr); }
    } mask_guard{prev};

    std::atomic<bool> done{false};
    std::thread sig_thread;
    // RAII: stop + join the sigwait thread on every exit path so it never outlives the locals it
    // captures. Declared AFTER sig_thread (and after `done`/`block`), so on unwind it destructs
    // FIRST -- joining the thread before the std::thread's own destructor and before those captured
    // locals die. This declaration order is load-bearing.
    struct ThreadGuard {
        std::atomic<bool>& done;
        std::thread& t;
        ~ThreadGuard() {
            done.store(true);
            if (t.joinable()) {
                t.join();
            }
        }
    } thread_guard{done, sig_thread};
#endif

    process::Process child = process::Process::spawn(req);

#ifndef _WIN32
    const pid_t pgid = static_cast<pid_t>(child.pid()); // == pgid (new_group)
    sig_thread = std::thread([&block, &done, pgid]() {
        const timespec poll{0, 200 * 1000 * 1000}; // 200 ms, so we notice `done` promptly
        while (!done.load()) {
            const int sig = ::sigtimedwait(&block, nullptr, &poll);
            if (sig <= 0) {
                continue; // timeout / EINTR -- re-check `done`
            }
            // Launcher teardown: graceful SIGTERM to the app group, a short grace (bail early if
            // the app already exited, so a cooperative app is never SIGKILLed), then the hard kill.
            // No reap here -- the main thread's wait() is the SINGLE reaper.
            ::kill(-pgid, SIGTERM);
            for (int i = 0; i < 20 && !done.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            if (!done.load()) {
                ::kill(-pgid, SIGKILL);
            }
            return;
        }
    });
#endif

    int exit_code = 0;
    std::string captured;
    if (req.capture_stdout) {
        captured = child.read_stdout_to_end(); // returns when the app closes stdout (i.e. exits)
    }
    child.wait(-1, exit_code); // single reaper
    // thread_guard (join) + mask_guard (restore) run on scope exit, in that order.

    if (req.capture_stdout && !output_path.empty()) {
        std::ofstream file(output_path, std::ios::binary);
        if (!file) {
            throw std::runtime_error("cannot write --run-output: " + output_path);
        }
        file.write(captured.data(), static_cast<std::streamsize>(captured.size()));
    }
    return exit_code;
}

// Spawns the descriptor's app and returns its exit code. argv/env/cwd cross to
// the child as structured data only. Runs under run_managed_child so the app's lifetime is bounded
// by the launcher's managed-child teardown.
//
// Default (no output_path): the child inherits this process's stdout, so its
// output streams through byte-for-byte with nothing added -- correct for
// interactive/long-running apps. With output_path set (--run-output), the
// child's stdout is captured and written to that file; this non-streaming path
// exists for the shell smoke tests, which read the file back.
int spawn_and_relay(const LaunchDescriptor& d, const std::string& output_path) {
    process::SpawnRequest req;
    req.argv = d.argv;
    req.env_overrides = d.env;
    req.cwd = d.cwd;
    if (output_path.empty()) {
        req.inherit_streams = true;
    } else {
        req.capture_stdout = true;
    }
    return run_managed_child(std::move(req), output_path);
}

} // namespace

int run_cli(const std::vector<std::string>& args) {
    LaunchDescriptor d;
    bool have_separator = false;
    std::vector<std::string> app_argv;
    std::string argv_file;
    std::string env_file;
    std::string cwd_file;
    std::string descriptor_out;
    std::string run_descriptor;
    std::string run_output;
    std::string daemon_addr;
    bool print_descriptor = false;
    bool run = false;
    bool list_gpus = false;
    bool ping = false;
    bool query_host_display_flag = false; // --query-host-display
    bool open_session_flag = false;
    bool close_session_flag = false;
    bool profile_flag = false; // --open-session --profile
    bool op_trace_flag = false;
    std::string app_id = "vkrelay2-app";
    std::string close_worker_id;
    std::string close_app_token;
    std::string
        require_worker_backend; // --open-session: fail closed unless the daemon advertises it

    auto need_value = [&](std::size_t& i, const char* opt) -> std::string {
        if (i + 1 >= args.size()) {
            throw std::runtime_error(std::string(opt) + " requires a value");
        }
        return args[++i];
    };

    try {
        for (std::size_t i = 0; i < args.size(); ++i) {
            const std::string& arg = args[i];
            if (have_separator) {
                app_argv.push_back(arg);
                continue;
            }
            if (arg == "--") {
                have_separator = true;
            } else if (arg == "--help" || arg == "-h") {
                print_usage();
                return 0;
            } else if (arg == "--list-gpus") {
                list_gpus = true;
            } else if (arg == "--ping") {
                ping = true;
            } else if (arg == "--query-host-display") {
                query_host_display_flag = true; // print the daemon's host display KEY=VALUEs
            } else if (arg == "--open-session") {
                open_session_flag = true;
            } else if (arg == "--close-session") {
                close_session_flag = true;
            } else if (arg == "--profile") {
                profile_flag = true; // session-scoped profiling
            } else if (arg == "--op-trace") {
                op_trace_flag = true;
            } else if (arg == "--app-id") {
                app_id = need_value(i, "--app-id");
            } else if (arg == "--worker-id") {
                close_worker_id = need_value(i, "--worker-id");
            } else if (arg == "--app-token") {
                close_app_token = need_value(i, "--app-token");
            } else if (arg == "--require-worker-backend") {
                require_worker_backend = need_value(i, "--require-worker-backend");
                const char* allowed[] = {"mock", "real"};
                if (!validate_enum(require_worker_backend, allowed, 2)) {
                    throw std::runtime_error("--require-worker-backend expects mock|real");
                }
            } else if (arg == "--daemon") {
                daemon_addr = need_value(i, "--daemon");
            } else if (arg == "--gpu") {
                d.session.gpu_selector = need_value(i, "--gpu");
                const auto parsed = protocol::parse_selector(d.session.gpu_selector);
                if (!parsed.ok) {
                    throw std::runtime_error("invalid --gpu selector: " + parsed.error);
                }
            } else if (arg == "--display") {
                d.session.display_backend = need_value(i, "--display");
                const char* allowed[] = {"auto", "x11", "wayland"};
                if (!validate_enum(d.session.display_backend, allowed, 3)) {
                    throw std::runtime_error("invalid --display backend");
                }
            } else if (arg == "--frontend") {
                d.session.graphics_frontend = need_value(i, "--frontend");
                const char* allowed[] = {"auto", "vulkan13", "opengl46zink"};
                if (!validate_enum(d.session.graphics_frontend, allowed, 3)) {
                    throw std::runtime_error("invalid --frontend");
                }
            } else if (arg == "--cwd") {
                d.cwd = need_value(i, "--cwd");
            } else if (arg == "--cwd-file") {
                cwd_file = need_value(i, "--cwd-file");
            } else if (arg == "--env") {
                const std::string kv = need_value(i, "--env");
                const std::size_t eq = kv.find('=');
                if (eq == std::string::npos) {
                    throw std::runtime_error("--env expects KEY=VALUE");
                }
                d.env.emplace_back(kv.substr(0, eq), kv.substr(eq + 1));
            } else if (arg == "--env-file") {
                env_file = need_value(i, "--env-file");
            } else if (arg == "--argv-file") {
                argv_file = need_value(i, "--argv-file");
            } else if (arg == "--descriptor-out") {
                descriptor_out = need_value(i, "--descriptor-out");
            } else if (arg == "--print-descriptor") {
                print_descriptor = true;
            } else if (arg == "--run") {
                run = true;
            } else if (arg == "--run-output") {
                run_output = need_value(i, "--run-output");
            } else if (arg == "--run-descriptor") {
                run_descriptor = need_value(i, "--run-descriptor");
            } else {
                throw std::runtime_error("unknown option: " + arg);
            }
        }

        if (list_gpus) {
            // Resolve the daemon endpoint: an explicit --daemon wins; otherwise the
            // well-known host:port (env-overridable), so no one has to type it.
            std::string host;
            int port = 0;
            const bool explicit_daemon = !daemon_addr.empty();
            if (explicit_daemon) {
                if (!parse_host_port(daemon_addr, host, port)) {
                    throw std::runtime_error("--daemon expects host:port");
                }
            } else {
                host = control::default_daemon_host();
                port = control::default_daemon_port();
            }
            try {
                return query_daemon_list_gpus(host, port);
            } catch (const std::exception& e) {
                if (explicit_daemon) {
                    throw; // an explicit endpoint failing is an error, not a fallback
                }
                // No daemon reachable at the default endpoint. NEVER print the local mock
                // list as if it were hardware (a fake discrete GPU misleads exactly the
                // machines where bring-up failed): fail with the cause and the remedy.
                // VKRELAY2_ALLOW_MOCK_GPUS=1 restores the offline mock print for tests.
                if (!allow_mock_gpus()) {
                    std::fprintf(stderr,
                                 "vkrelay2-launch: no daemon at %s:%d (%s); no adapter list "
                                 "available.\nStart the daemon with: vkrelay2-supervisor --serve "
                                 "(the launcher script auto-starts it), or set "
                                 "VKRELAY2_ALLOW_MOCK_GPUS=1 to print the local mock list "
                                 "(testing only).\n",
                                 host.c_str(), port, e.what());
                    return 3;
                }
                std::fprintf(stderr,
                             "vkrelay2-launch: no daemon at %s:%d (%s); showing the local mock "
                             "list.\nStart the daemon with: vkrelay2-supervisor --serve\n",
                             host.c_str(), port, e.what());
                std::fputs(protocol::format_gpu_list(protocol::probe_mocked()).c_str(), stdout);
                return 0;
            }
        }

        if (ping) {
            // Protocol-level health check. Resolve the endpoint like the other daemon modes,
            // then do a real handshake (ping_daemon catches its own errors and returns non-zero).
            std::string host;
            int port = 0;
            if (!daemon_addr.empty()) {
                if (!parse_host_port(daemon_addr, host, port)) {
                    throw std::runtime_error("--daemon expects host:port");
                }
            } else {
                host = control::default_daemon_host();
                port = control::default_daemon_port();
            }
            return ping_daemon(host, port);
        }

        if (query_host_display_flag) {
            // Resolve the endpoint like --ping, then print the advertised host display space.
            std::string host;
            int port = 0;
            if (!daemon_addr.empty()) {
                if (!parse_host_port(daemon_addr, host, port)) {
                    throw std::runtime_error("--daemon expects host:port");
                }
            } else {
                host = control::default_daemon_host();
                port = control::default_daemon_port();
            }
            return query_host_display(host, port);
        }

        if (close_session_flag) {
            std::string host;
            int port = 0;
            if (!daemon_addr.empty()) {
                if (!parse_host_port(daemon_addr, host, port)) {
                    throw std::runtime_error("--daemon expects host:port");
                }
            } else {
                host = control::default_daemon_host();
                port = control::default_daemon_port();
            }
            return close_session(host, port, app_id, close_worker_id, close_app_token);
        }

        if (open_session_flag) {
            // Resolve the daemon endpoint (explicit --daemon wins; else the well-known
            // env-overridable default). Fail-closed: an unreachable daemon throws (the outer
            // catch returns non-zero), so the launcher never proceeds to spawn the app.
            std::string host;
            int port = 0;
            if (!daemon_addr.empty()) {
                if (!parse_host_port(daemon_addr, host, port)) {
                    throw std::runtime_error("--daemon expects host:port");
                }
            } else {
                host = control::default_daemon_host();
                port = control::default_daemon_port();
            }
            // profiling rides the SESSION (never the daemon's ambient start env --
            // daemon reuse makes that an accident). --profile or VKRELAY2_PROFILE=1 in the
            // launcher env both request a profiled worker for this session.
            const char* prof_env = std::getenv("VKRELAY2_PROFILE");
            const bool profile = profile_flag || (prof_env != nullptr && prof_env[0] == '1');
            const char* trace_env = std::getenv("VKRELAY2_OP_TRACE");
            const bool op_trace = op_trace_flag || (trace_env != nullptr && trace_env[0] == '1');
            const char* input_trace_env = std::getenv("VKRELAY2_INPUT_TRACE");
            const bool input_trace = input_trace_env != nullptr && input_trace_env[0] == '1';
            return open_session(host, port, app_id, d.session.gpu_selector,
                                d.session.display_backend, d.session.graphics_frontend,
                                require_worker_backend, profile, op_trace, input_trace);
        }

        if (!run_descriptor.empty()) {
            const LaunchDescriptor parsed =
                LaunchDescriptor::from_json(read_file_bytes(run_descriptor));
            return spawn_and_relay(parsed, run_output);
        }

        // Assemble argv from a NUL-file or from the post-"--" tokens.
        if (!argv_file.empty()) {
            d.argv = split_nul(read_file_bytes(argv_file));
        } else {
            d.argv = app_argv;
        }
        if (!env_file.empty()) {
            for (const std::string& entry : split_nul(read_file_bytes(env_file))) {
                const std::size_t eq = entry.find('=');
                if (eq == std::string::npos) {
                    throw std::runtime_error("env-file entry missing '=': " + entry);
                }
                d.env.emplace_back(entry.substr(0, eq), entry.substr(eq + 1));
            }
        }
        if (!cwd_file.empty()) {
            d.cwd = strip_trailing_newline(read_file_bytes(cwd_file));
        }

        d.validate();

        if (!descriptor_out.empty()) {
            std::ofstream out(descriptor_out, std::ios::binary);
            if (!out) {
                throw std::runtime_error("cannot write descriptor: " + descriptor_out);
            }
            out << d.to_json(true);
        }
        if (print_descriptor) {
            std::fputs(d.to_json(true).c_str(), stdout);
            std::fputc('\n', stdout);
        }
        if (run) {
            // Round-trip through the wire format before spawning, proving the
            // descriptor survives serialization with no shell string anywhere.
            const LaunchDescriptor round_tripped = LaunchDescriptor::from_json(d.to_json());
            return spawn_and_relay(round_tripped, run_output);
        }
        if (descriptor_out.empty() && !print_descriptor) {
            // Default action: emit the descriptor so the path is observable.
            std::fputs(d.to_json(true).c_str(), stdout);
            std::fputc('\n', stdout);
        }
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "vkrelay2-launch error: %s\n", e.what());
        return 2;
    }
}

} // namespace vkr::launch
