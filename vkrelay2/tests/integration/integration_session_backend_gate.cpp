// Loopback integration test: the app-run backend gate fails closed BEFORE LaunchSession.
//
// `vkrelay2-launch --open-session --require-worker-backend real` must, against a daemon that
// advertises a "mock" session-worker backend, reject WITHOUT sending LaunchSession -- so a rejected
// app-run never creates an idle worker session. A
// "real" daemon accepts and launches exactly once. Drives the real CLI (run_cli) against a real
// loopback serve_session, with a counting launcher so "no session was created" is asserted
// directly.
#include "common/control/control_service.hpp"
#include "common/launch/launch_cli.hpp"
#include "common/protocol/gpu.hpp"
#include "common/protocol/ids.hpp"
#include "common/transport/tcp.hpp"
#include "common/transport/transport.hpp"
#include "tests/test_assert.hpp"
#include "windows/supervisor/display_snapshot_cache.hpp"

#include <atomic>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>

using namespace vkr;

namespace {

// Portable env set/clear for the VKRELAY2_PROFILE launcher-env case.
void set_env(const char* name, const char* value) {
#ifdef _WIN32
    _putenv((std::string(name) + "=" + value).c_str());
#else
    setenv(name, value, 1);
#endif
}
void clear_env(const char* name) {
#ifdef _WIN32
    _putenv((std::string(name) + "=").c_str());
#else
    unsetenv(name);
#endif
}

// Counts launch_session calls (so a rejected app-run can be shown to create zero sessions) and
// records the selector it was given (so the forwarded --gpu can be checked). Read after join().
struct CountingLauncher : control::SessionLauncher {
    std::atomic<int> launches{0};
    std::string last_selector;
    std::string last_display;
    std::string last_frontend;
    bool last_profile = false;
    bool last_op_trace = false;
    bool last_input_trace = false;
    std::string last_display_snapshot;
    control::SessionInfo launch_session(const std::string&, const std::string& gpu_selector,
                                        const std::string& display_backend,
                                        const std::string& graphics_frontend, bool profile_enabled,
                                        bool op_trace_enabled, bool input_trace_enabled,
                                        const display::DisplayLayout* display_layout) override {
        ++launches;
        last_selector = gpu_selector;
        last_display = display_backend;
        last_frontend = graphics_frontend;
        last_profile = profile_enabled;
        last_op_trace = op_trace_enabled;
        last_input_trace = input_trace_enabled;
        last_display_snapshot = display_layout == nullptr ? "" : display_layout->snapshot_id;
        control::SessionInfo info;
        info.worker_id = "wkr-test";
        info.data_plane_host = "127.0.0.1";
        info.data_plane_port = 5555;
        info.app_token = "tok-test";
        info.display_snapshot_id = last_display_snapshot;
        return info;
    }
};

// Serves exactly one control session (advertising `backend`) on a fresh ephemeral port, on a
// thread. Returns the port; the caller runs the CLI against it then joins. A counting launcher
// records whether LaunchSession was reached.
int serve_one(std::unique_ptr<transport::Listener>& listener, std::thread& server,
              protocol::IdAllocator& ids, CountingLauncher& launcher, const std::string& backend) {
    listener = transport::tcp_listen(0);
    const int port = listener->port();
    transport::Listener* lp = listener.get();
    server = std::thread([lp, &ids, &launcher, backend] {
        try {
            auto conn = lp->accept();
            transport::MessageChannel channel(*conn);
            const auto devices = protocol::probe_mocked();
            control::serve_session(channel, ids, devices, &launcher, backend);
        } catch (const std::exception&) {
            // The client-side assertions report failures; a closed listener ends the thread.
        }
    });
    return port;
}

} // namespace

int main() {
    protocol::IdAllocator ids;

    // 1. Mock daemon: --require-worker-backend real fails closed BEFORE LaunchSession (no session).
    {
        CountingLauncher launcher;
        std::unique_ptr<transport::Listener> listener;
        std::thread server;
        const int port = serve_one(listener, server, ids, launcher, "mock");
        const std::string daemon = "127.0.0.1:" + std::to_string(port);
        const int rc = launch::run_cli(
            {"--open-session", "--daemon", daemon, "--require-worker-backend", "real"});
        listener->close(); // unblock accept if the client never connected
        server.join();
        VKR_CHECK(rc != 0);                        // fail closed
        VKR_CHECK_EQ(launcher.launches.load(), 0); // and BEFORE any session was created
    }

    // 2. Real daemon: the gate passes, a session launches exactly once, and the user's session
    // options (--gpu / --display / --frontend) are the ones forwarded into LaunchSession (not the
    // defaults) -- the faithful app-run session contract.
    {
        CountingLauncher launcher;
        std::unique_ptr<transport::Listener> listener;
        std::thread server;
        const int port = serve_one(listener, server, ids, launcher, "real");
        const std::string daemon = "127.0.0.1:" + std::to_string(port);
        const int rc = launch::run_cli({"--open-session", "--daemon", daemon,
                                        "--require-worker-backend", "real", "--gpu", "integrated",
                                        "--display", "x11", "--frontend", "vulkan13"});
        listener->close();
        server.join();
        VKR_CHECK_EQ(rc, 0);                                             // accepted
        VKR_CHECK_EQ(launcher.launches.load(), 1);                       // launched exactly once
        VKR_CHECK_EQ(launcher.last_selector, std::string("integrated")); // forwarded selector
        VKR_CHECK_EQ(launcher.last_display, std::string("x11"));         // forwarded display
        VKR_CHECK_EQ(launcher.last_frontend, std::string("vulkan13"));   // forwarded frontend
        VKR_CHECK(!launcher.last_profile);  // profiling is OFF unless requested
        VKR_CHECK(!launcher.last_op_trace); // decoded-op tracing is also explicit/session-scoped
        VKR_CHECK(!launcher.last_input_trace);
    }

    // 2a. Input tracing is a narrowly allow-listed session boolean, not ambient daemon state or an
    // arbitrary environment map.
    {
        CountingLauncher launcher;
        std::unique_ptr<transport::Listener> listener;
        std::thread server;
        const int port = serve_one(listener, server, ids, launcher, "real");
        const std::string daemon = "127.0.0.1:" + std::to_string(port);
        set_env("VKRELAY2_INPUT_TRACE", "1");
        const int rc = launch::run_cli({"--open-session", "--daemon", daemon});
        clear_env("VKRELAY2_INPUT_TRACE");
        listener->close();
        server.join();
        VKR_CHECK_EQ(rc, 0);
        VKR_CHECK_EQ(launcher.launches.load(), 1);
        VKR_CHECK(launcher.last_input_trace);
        VKR_CHECK(!launcher.last_profile);
        VKR_CHECK(!launcher.last_op_trace);
    }

    // 3. Session-scoped profiling plumbing (test gap): `--profile` must
    // arrive at the launcher as profile_enabled=true -- this is what turns into the worker's
    // VKRELAY2_PROFILE env override, so a regression here silently disables worker-end dumps.
    {
        CountingLauncher launcher;
        std::unique_ptr<transport::Listener> listener;
        std::thread server;
        const int port = serve_one(listener, server, ids, launcher, "real");
        const std::string daemon = "127.0.0.1:" + std::to_string(port);
        const int rc = launch::run_cli({"--open-session", "--daemon", daemon, "--profile"});
        listener->close();
        server.join();
        VKR_CHECK_EQ(rc, 0);
        VKR_CHECK_EQ(launcher.launches.load(), 1);
        VKR_CHECK(launcher.last_profile); // the flag reached LaunchSession
    }

    // 4. The launcher-env spelling (VKRELAY2_PROFILE=1 with NO --profile flag) requests the same
    // session-scoped profiling -- the path run_profile_smoke.sh actually uses.
    {
        CountingLauncher launcher;
        std::unique_ptr<transport::Listener> listener;
        std::thread server;
        const int port = serve_one(listener, server, ids, launcher, "real");
        const std::string daemon = "127.0.0.1:" + std::to_string(port);
        set_env("VKRELAY2_PROFILE", "1");
        const int rc = launch::run_cli({"--open-session", "--daemon", daemon});
        clear_env("VKRELAY2_PROFILE");
        listener->close();
        server.join();
        VKR_CHECK_EQ(rc, 0);
        VKR_CHECK_EQ(launcher.launches.load(), 1);
        VKR_CHECK(launcher.last_profile); // the env reached LaunchSession
    }

    // 5. The AMD investigation trace has its own session field. The CLI flag and environment
    // spelling must reach SessionLauncher without coupling to profiling or daemon start state.
    {
        CountingLauncher launcher;
        std::unique_ptr<transport::Listener> listener;
        std::thread server;
        const int port = serve_one(listener, server, ids, launcher, "real");
        const std::string daemon = "127.0.0.1:" + std::to_string(port);
        const int rc = launch::run_cli({"--open-session", "--daemon", daemon, "--op-trace"});
        listener->close();
        server.join();
        VKR_CHECK_EQ(rc, 0);
        VKR_CHECK_EQ(launcher.launches.load(), 1);
        VKR_CHECK(launcher.last_op_trace);
        VKR_CHECK(!launcher.last_profile);
    }

    {
        CountingLauncher launcher;
        std::unique_ptr<transport::Listener> listener;
        std::thread server;
        const int port = serve_one(listener, server, ids, launcher, "real");
        const std::string daemon = "127.0.0.1:" + std::to_string(port);
        set_env("VKRELAY2_OP_TRACE", "1");
        const int rc = launch::run_cli({"--open-session", "--daemon", daemon});
        clear_env("VKRELAY2_OP_TRACE");
        listener->close();
        server.join();
        VKR_CHECK_EQ(rc, 0);
        VKR_CHECK_EQ(launcher.launches.load(), 1);
        VKR_CHECK(launcher.last_op_trace);
        VKR_CHECK(!launcher.last_profile);
    }

    // 6. An expired query ID gets exactly one retry using this open-session handshake's fresh
    // snapshot, but only because its bounds match the root dimensions exported by the query.
    {
        protocol::IdAllocator retry_ids("sup-retry");
        supervisor::DisplaySnapshotCache cache("sup-retry", 8, [](const std::string& snapshot_id) {
            supervisor::DisplayLayoutProbeResult result;
            result.ok = true;
            result.layout.snapshot_id = snapshot_id;
            result.layout.virtual_bounds = {0, 0, 800, 600};
            result.layout.primary_monitor_id = "only";
            display::MonitorDesc monitor;
            monitor.stable_id = "only";
            monitor.device_name = "DISPLAY1";
            monitor.bounds = result.layout.virtual_bounds;
            monitor.work = result.layout.virtual_bounds;
            monitor.dpi_x = 96;
            monitor.dpi_y = 96;
            monitor.primary = true;
            result.layout.monitors = {monitor};
            return result;
        });
        CountingLauncher launcher;
        auto listener = transport::tcp_listen(0);
        const int port = listener->port();
        std::thread server([&] {
            auto conn = listener->accept();
            transport::MessageChannel channel(*conn);
            control::serve_session(channel, retry_ids, protocol::probe_mocked(), &launcher, "real",
                                   {}, &cache);
        });
        set_env("VKRELAY2_DISPLAY_SNAPSHOT_ID", "sup-retry/display-expired");
        set_env("VKRELAY2_HOST_VIRTUAL_W", "800");
        set_env("VKRELAY2_HOST_VIRTUAL_H", "600");
        const std::string daemon = "127.0.0.1:" + std::to_string(port);
        const int rc = launch::run_cli({"--open-session", "--daemon", daemon});
        clear_env("VKRELAY2_DISPLAY_SNAPSHOT_ID");
        clear_env("VKRELAY2_HOST_VIRTUAL_W");
        clear_env("VKRELAY2_HOST_VIRTUAL_H");
        server.join();
        VKR_CHECK_EQ(rc, 0);
        VKR_CHECK_EQ(launcher.launches.load(), 1);
        VKR_CHECK_EQ(launcher.last_display_snapshot, std::string("sup-retry/display-1"));
    }

    // 7. The retry is forbidden when the fresh snapshot would require a different Weston root.
    {
        protocol::IdAllocator retry_ids("sup-resized");
        supervisor::DisplaySnapshotCache cache("sup-resized", 8,
                                               [](const std::string& snapshot_id) {
                                                   supervisor::DisplayLayoutProbeResult result;
                                                   result.ok = true;
                                                   result.layout.snapshot_id = snapshot_id;
                                                   result.layout.virtual_bounds = {0, 0, 900, 600};
                                                   result.layout.primary_monitor_id = "only";
                                                   display::MonitorDesc monitor;
                                                   monitor.stable_id = "only";
                                                   monitor.device_name = "DISPLAY1";
                                                   monitor.bounds = result.layout.virtual_bounds;
                                                   monitor.work = result.layout.virtual_bounds;
                                                   monitor.dpi_x = 96;
                                                   monitor.dpi_y = 96;
                                                   monitor.primary = true;
                                                   result.layout.monitors = {monitor};
                                                   return result;
                                               });
        CountingLauncher launcher;
        auto listener = transport::tcp_listen(0);
        const int port = listener->port();
        std::thread server([&] {
            auto conn = listener->accept();
            transport::MessageChannel channel(*conn);
            control::serve_session(channel, retry_ids, protocol::probe_mocked(), &launcher, "real",
                                   {}, &cache);
        });
        set_env("VKRELAY2_DISPLAY_SNAPSHOT_ID", "sup-resized/display-expired");
        set_env("VKRELAY2_HOST_VIRTUAL_W", "800");
        set_env("VKRELAY2_HOST_VIRTUAL_H", "600");
        const std::string daemon = "127.0.0.1:" + std::to_string(port);
        const int rc = launch::run_cli({"--open-session", "--daemon", daemon});
        clear_env("VKRELAY2_DISPLAY_SNAPSHOT_ID");
        clear_env("VKRELAY2_HOST_VIRTUAL_W");
        clear_env("VKRELAY2_HOST_VIRTUAL_H");
        server.join();
        VKR_CHECK(rc != 0);
        VKR_CHECK_EQ(launcher.launches.load(), 0);
    }

    return vkr::test::finish("integration_session_backend_gate");
}
