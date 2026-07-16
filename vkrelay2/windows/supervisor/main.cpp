// vkrelay2 supervisor.
//
// The long-lived supervisor makes no host Vulkan calls and owns no app
// windows. It provides --self-test (spawn + heartbeat + kill a worker), GPU listing/selection,
// listener routing, and real worker supervision.
#include "windows/supervisor/display_snapshot_cache.hpp"
#include "windows/supervisor/gpu_probe.hpp"
#include "windows/supervisor/self_test.hpp"
#include "windows/supervisor/worker_supervisor.hpp"

#include "common/control/control_service.hpp"
#include "common/control/daemon_endpoint.hpp"
#include "common/logging/logging.hpp"
#include "common/protocol/gpu.hpp"
#include "common/protocol/ids.hpp"
#include "common/transport/tcp.hpp"
#include "common/transport/transport.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

void print_usage() {
    std::puts("vkrelay2-supervisor - vkrelay2 process supervisor (bootstrap)\n");
    std::puts("Usage:");
    std::puts("  vkrelay2-supervisor --self-test [--worker <path>] [--verbose]");
    std::puts("  vkrelay2-supervisor --serve [--port <n>] [--bind <addr>] [--once]");
    std::puts("                      [--vulkan-backend mock|real] [--verbose]");
    std::puts("  vkrelay2-supervisor --supervise [--workers <n>] [--gpu <sel>]");
    std::puts("                      [--duration-ms <m>] [--worker <path>]");
    std::puts("                      [--vulkan-backend mock|real] [--verbose]");
    std::puts("  vkrelay2-supervisor --list-gpus");
    std::puts("  vkrelay2-supervisor --gpu <selector>");
    std::puts("  vkrelay2-supervisor --help\n");
    std::puts("Selectors: auto | high-performance | integrated | vendor:<hex> |");
    std::puts("           device:<hex>:<hex> | luid:<id> | index:<n> | name:<substr>\n");
    std::puts("--serve defaults to the well-known port 13579 (override: --port or");
    std::puts("VKRELAY2_DAEMON_PORT) and binds 127.0.0.1 (override: --bind or VKRELAY2_BIND;");
    std::puts("use 0.0.0.0 to reach the daemon from NAT-mode WSL).");
}

int do_serve(int port, bool once, const std::string& port_file, const std::string& worker_override,
             const std::string& bind_address, const std::string& vulkan_backend) {
    vkr::protocol::IdAllocator ids;
    vkr::supervisor::DisplaySnapshotCache display_snapshots(ids.supervisor_session_id());
    vkr::control::DisplaySnapshotProvider* display_snapshot_provider = nullptr;
#ifdef _WIN32
    // Windows production handshakes capture a fresh topology and retain a bounded set so the
    // later open-session connection can resolve the exact snapshot queried before Weston start.
    display_snapshot_provider = &display_snapshots;
#endif
    vkr::supervisor::WorkerSupervisorConfig config;
    config.worker_path =
        worker_override.empty() ? vkr::supervisor::default_worker_path() : worker_override;
    // Forward the backend selection to every worker this daemon spawns. Default "mock" keeps the
    // headless/dual-platform behavior; "real" is what an on-screen session (worker-present to a
    // real Win32 window) needs -- the daemon path for the boundary canary.
    config.extra_worker_args = {"--vulkan-backend", vulkan_backend};
    // Sidecar plane: the daemon offers a sidecar (the X11 WM / geometry authority on the
    // private display) on every launched session -- the worker opens its second listener and the
    // supervisor reports the endpoint/token for the launcher to start a sidecar against. Always on
    // (the listener is harmless and idle if no sidecar connects, and a mock daemon must still offer
    // it so the boundary smoke can exercise the sidecar without a real GPU). The production app-run
    // path still fails closed on a mock daemon BEFORE this matters (it requires a real worker).
    config.sidecar_plane = true;
    // Real adapter list via a disposable worker (mock fallback if it can't probe). Inject it into
    // the supervisor so the GpuListResponse a client sees AND LaunchSession's device selection use
    // the SAME list -- and so a real list passes the resolved device's LUID to the worker as the
    // faithful match key (a mock-fallback list does not).
    const auto probe = vkr::supervisor::probe_gpus(config.worker_path);
    const auto& devices = probe.devices;
    config.devices = probe.devices;
    config.devices_real = probe.real;
    // production geometry comes only from the per-handshake DisplayLayout capture. The control
    // service derives the additive legacy host_work/host_dpi fields from that same snapshot, so no
    // serve-start SPI_GETWORKAREA read can become a second authority. Tests/non-Windows callers may
    // still inject HostDisplay explicitly through serve_session.
    const vkr::protocol::HostDisplay host_display;
    try {
        vkr::supervisor::WorkerSupervisor supervisor(config);
        auto listener = vkr::transport::tcp_listen(port, bind_address);
        if (!port_file.empty()) {
            std::FILE* f = std::fopen(port_file.c_str(), "wb");
            if (f != nullptr) {
                std::fprintf(f, "%d", listener->port());
                std::fclose(f);
            }
        }
        VKR_INFO("supervisor") << "serving control plane on " << bind_address << ":"
                               << listener->port()
                               << "; supervisor_session=" << ids.supervisor_session_id();
        // Bootstrap stub: app control connections are served one at a time.
        // LaunchSession drives the WorkerSupervisor, which supervises workers
        // concurrently. Concurrent app-control accept is a later hardening.
        //
        // Launcher/session reliability: each accepted control session is
        // short-lived (handshake -> at most a list / launch round trip -> the client disconnects),
        // so a per-connection idle read deadline is safe AND necessary: a stalled or half-open
        // client must not be able to wedge this single loop -- if it did, every later launch would
        // connect (the backlog accepts) yet never be served, which is exactly the intermittent
        // bring-up hang. A timed-out read throws TransportError out of serve_session, caught below;
        // the connection is then dropped and the loop returns to accept().
        const int idle_ms = vkr::control::default_serve_idle_timeout_ms();
        for (;;) {
            auto conn = listener->accept();
            conn->set_read_timeout(idle_ms);
            vkr::transport::MessageChannel channel(*conn);
            try {
                vkr::control::serve_session(channel, ids, devices, &supervisor, vulkan_backend,
                                            host_display, display_snapshot_provider, probe.real);
            } catch (const std::exception& e) {
                VKR_WARN("supervisor") << "session ended with error: " << e.what();
            }
            if (once) {
                break;
            }
        }
    } catch (const std::exception& e) {
        VKR_ERROR("supervisor") << "serve failed: " << e.what();
        return 1;
    }
    return 0;
}

int do_list_gpus(const std::string& worker_path) {
    const auto probe = vkr::supervisor::probe_gpus(worker_path);
    // Never present the mocked fallback as hardware: without real enumeration this
    // command fails with the cause, unless the testing escape hatch is set (then the
    // mock list prints under an explicit MOCKED header).
    if (!probe.real && std::getenv("VKRELAY2_ALLOW_MOCK_GPUS") == nullptr) {
        std::fputs("vkrelay2-supervisor: no real Vulkan adapter enumeration available (worker "
                   "missing, or built without the Vulkan SDK); refusing to print the mocked "
                   "adapter list.\nRebuild the worker with the Vulkan SDK (see docs/building.md) "
                   "or set VKRELAY2_ALLOW_MOCK_GPUS=1 (testing only).\n",
                   stderr);
        return 3;
    }
    std::fputs(probe.real ? "Daemon-side Vulkan adapters:\n"
                          : "Daemon-side Vulkan adapters (MOCKED -- not real hardware):\n",
               stdout);
    std::fputs(vkr::protocol::format_gpu_list(probe.devices).c_str(), stdout);
    return 0;
}

int do_select_gpu(const std::string& selector_text, const std::string& worker_path) {
    const auto parsed = vkr::protocol::parse_selector(selector_text);
    if (!parsed.ok) {
        std::fprintf(stderr, "invalid --gpu selector: %s\n", parsed.error.c_str());
        return 2;
    }
    if (vkr::protocol::selector_is_unstable(parsed.selector)) {
        std::fputs("warning: index: selectors are not stable across driver changes; "
                   "prefer luid/vendor/device/name in saved configs\n",
                   stderr);
    }
    const auto probe = vkr::supervisor::probe_gpus(worker_path);
    std::string reason;
    const vkr::protocol::GpuDevice* chosen =
        vkr::protocol::select_device(probe.devices, parsed.selector, reason);
    if (chosen == nullptr) {
        std::fprintf(stderr, "no usable adapter: %s\n", reason.c_str());
        return 1;
    }
    std::printf("%s\n", reason.c_str());
    return 0;
}

// Lifecycle demo: launch N workers, supervise them for a while, then
// shut down cleanly. Workers connect back over the worker-control channel and
// heartbeat; shutdown terminates them all.
int do_supervise(const std::string& worker_override, const std::string& gpu_selector, int workers,
                 int duration_ms, const std::string& vulkan_backend) {
    vkr::supervisor::WorkerSupervisorConfig config;
    config.worker_path =
        worker_override.empty() ? vkr::supervisor::default_worker_path() : worker_override;
    config.extra_worker_args = {"--vulkan-backend", vulkan_backend};
    try {
        vkr::supervisor::WorkerSupervisor supervisor(config);
        for (int i = 0; i < workers; ++i) {
            const std::string id = supervisor.launch_worker(gpu_selector);
            std::printf("launched %s\n", id.c_str());
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(duration_ms));
        std::printf("active=%zu total_heartbeats=%lld\n", supervisor.active_count(),
                    supervisor.total_heartbeats());
        for (const auto& s : supervisor.snapshot()) {
            std::printf("  %s gpu=\"%s\" state=%s heartbeats=%lld connected=%s\n",
                        s.worker_id.c_str(), s.gpu_name.c_str(), vkr::protocol::to_string(s.state),
                        s.heartbeats, s.connected ? "yes" : "no");
        }
        supervisor.shutdown();
    } catch (const std::exception& e) {
        VKR_ERROR("supervisor") << "supervise failed: " << e.what();
        return 1;
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    vkr::log::set_component("supervisor");

    std::vector<std::string> args(argv + 1, argv + argc);
    std::string worker_override;
    std::string gpu_selector;
    std::string vulkan_backend = "mock"; // forwarded to spawned workers; "real" for on-screen WSI
    bool self_test = false;
    bool list_gpus = false;
    bool serve = false;
    bool once = false;
    // --serve defaults to the well-known port (and env-overridable) so a client
    // can reach the daemon without anyone passing host:port; --port still wins.
    int port = vkr::control::default_daemon_port();
    std::string bind_address = vkr::control::default_bind_address();
    std::string port_file;
    bool supervise = false;
    int workers = 1;
    int duration_ms = 1500;

    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (arg == "--help" || arg == "-h") {
            print_usage();
            return 0;
        } else if (arg == "--self-test") {
            self_test = true;
        } else if (arg == "--serve") {
            serve = true;
        } else if (arg == "--once") {
            once = true;
        } else if (arg == "--port") {
            if (i + 1 >= args.size()) {
                std::fputs("--port requires a number\n", stderr);
                return 2;
            }
            port = std::atoi(args[++i].c_str());
        } else if (arg == "--bind") {
            if (i + 1 >= args.size()) {
                std::fputs("--bind requires an address (e.g. 127.0.0.1 or 0.0.0.0)\n", stderr);
                return 2;
            }
            bind_address = args[++i];
        } else if (arg == "--port-file") {
            if (i + 1 >= args.size()) {
                std::fputs("--port-file requires a path\n", stderr);
                return 2;
            }
            port_file = args[++i];
        } else if (arg == "--supervise") {
            supervise = true;
        } else if (arg == "--workers") {
            if (i + 1 >= args.size()) {
                std::fputs("--workers requires a number\n", stderr);
                return 2;
            }
            workers = std::atoi(args[++i].c_str());
        } else if (arg == "--duration-ms") {
            if (i + 1 >= args.size()) {
                std::fputs("--duration-ms requires a number\n", stderr);
                return 2;
            }
            duration_ms = std::atoi(args[++i].c_str());
        } else if (arg == "--list-gpus") {
            list_gpus = true;
        } else if (arg == "--verbose") {
            vkr::log::set_min_level(vkr::log::Level::Debug);
        } else if (arg == "--worker") {
            if (i + 1 >= args.size()) {
                std::fputs("--worker requires a path\n", stderr);
                return 2;
            }
            worker_override = args[++i];
        } else if (arg == "--gpu") {
            if (i + 1 >= args.size()) {
                std::fputs("--gpu requires a selector\n", stderr);
                return 2;
            }
            gpu_selector = args[++i];
        } else if (arg == "--vulkan-backend") {
            if (i + 1 >= args.size()) {
                std::fputs("--vulkan-backend requires mock|real\n", stderr);
                return 2;
            }
            vulkan_backend = args[++i];
            if (vulkan_backend != "mock" && vulkan_backend != "real") {
                std::fputs("--vulkan-backend must be mock or real\n", stderr);
                return 2;
            }
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", arg.c_str());
            print_usage();
            return 2;
        }
    }

    if (serve) {
        return do_serve(port, once, port_file, worker_override, bind_address, vulkan_backend);
    }
    if (supervise) {
        return do_supervise(worker_override, gpu_selector.empty() ? "auto" : gpu_selector, workers,
                            duration_ms, vulkan_backend);
    }
    const std::string worker_path =
        worker_override.empty() ? vkr::supervisor::default_worker_path() : worker_override;
    if (list_gpus) {
        return do_list_gpus(worker_path);
    }
    if (!gpu_selector.empty()) {
        return do_select_gpu(gpu_selector, worker_path);
    }
    if (self_test) {
        return vkr::supervisor::run_self_test(worker_override);
    }

    print_usage();
    return 0;
}
