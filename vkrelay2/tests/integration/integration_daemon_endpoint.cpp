// Default daemon endpoint discovery + bind override.
//
// Proves the "no typed host:port" path end to end: a supervisor bound with
// --bind 0.0.0.0 on an ephemeral port, reached by the launcher both explicitly
// (--daemon) and via the env-defaulted well-known endpoint, returns the same list;
// and a launcher pointed at a dead port falls back to the local mock instead of
// hanging. argv: <supervisor> <worker> <launch>.
#include "common/control/daemon_endpoint.hpp"
#include "common/process/process.hpp"
#include "common/protocol/gpu.hpp"
#include "common/transport/tcp.hpp"
#include "common/transport/transport.hpp"
#include "tests/test_assert.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <exception>
#include <fstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace vkr;

namespace {

// Captured stdout differs cosmetically from an in-process string: the line-based
// capture drops the trailing newline, and Windows text-mode adds CRs. Normalize
// both before comparing so the test asserts content, not capture artifacts.
std::string normalize(std::string s) {
    s.erase(std::remove(s.begin(), s.end(), '\r'), s.end());
    while (!s.empty() && s.back() == '\n') {
        s.pop_back();
    }
    return s;
}

std::string run_capture(const std::string& bin, const std::vector<std::string>& extra_args,
                        const std::vector<std::pair<std::string, std::string>>& env) {
    process::SpawnRequest req;
    req.argv.push_back(bin);
    for (const auto& a : extra_args) {
        req.argv.push_back(a);
    }
    req.env_overrides = env;
    req.capture_stdout = true;
    process::Process child = process::Process::spawn(req);
    const std::string out = child.read_stdout_to_end();
    int code = -1;
    child.wait(5000, code);
    return out;
}

// Reads the supervisor's chosen port from its --port-file, polling until written.
int read_port_file(const std::string& path) {
    for (int i = 0; i < 200; ++i) { // up to ~10s
        std::ifstream f(path);
        if (f) {
            int port = 0;
            f >> port;
            if (port > 0) {
                return port;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return 0;
}

// A port that is currently closed (bind ephemeral, learn the port, release it).
int closed_port() {
    auto listener = transport::tcp_listen(0);
    const int port = listener->port();
    listener->close();
    return port;
}

} // namespace

int main(int argc, char** argv) {
    const std::string supervisor = argc > 1 ? argv[1] : "";
    const std::string worker = argc > 2 ? argv[2] : "";
    const std::string launch = argc > 3 ? argv[3] : "";

    // An invalid bind address is rejected, not silently ignored.
    bool threw = false;
    try {
        transport::tcp_listen(0, "definitely-not-an-ip");
    } catch (const std::exception&) {
        threw = true;
    }
    VKR_CHECK(threw);

    if (supervisor.empty() || worker.empty() || launch.empty()) {
        return vkr::test::finish("integration_daemon_endpoint");
    }

    const std::string port_file = "integration_daemon_endpoint.port";
    std::remove(port_file.c_str());

    // Daemon bound on all interfaces (--bind 0.0.0.0) so NAT-mode WSL could reach
    // it; here we confirm it is still loopback-reachable.
    process::SpawnRequest sreq;
    sreq.argv = {supervisor, "--serve",     "--bind",  "0.0.0.0",  "--port",
                 "0",        "--port-file", port_file, "--worker", worker};
    sreq.new_group = true;
    process::Process sup = process::Process::spawn(sreq);

    const int dport = read_port_file(port_file);
    VKR_CHECK(dport > 0);

    if (dport > 0) {
        // Explicit endpoint vs the env-defaulted well-known endpoint: same daemon,
        // so byte-identical output -- proving the default resolution reaches it.
        // VKRELAY2_ALLOW_MOCK_GPUS=1 is the testing escape hatch: on a host whose
        // worker has no real Vulkan (every WSL/Linux ctest run) the daemon stamps
        // its list real=false and the default policy refuses to print it.
        const std::string explicit_out =
            run_capture(launch, {"--list-gpus", "--daemon", "127.0.0.1:" + std::to_string(dport)},
                        {{"VKRELAY2_ALLOW_MOCK_GPUS", "1"}});
        const std::string default_out =
            run_capture(launch, {"--list-gpus"},
                        {{"VKRELAY2_DAEMON_HOST", "127.0.0.1"},
                         {"VKRELAY2_DAEMON_PORT", std::to_string(dport)},
                         {"VKRELAY2_ALLOW_MOCK_GPUS", "1"}});
        VKR_CHECK(!explicit_out.empty());
        VKR_CHECK_EQ(normalize(default_out), normalize(explicit_out));

        // A dead default endpoint must NOT print the local mock list as if it were
        // hardware: the default policy fails with a clear message and EMPTY stdout.
        const std::string refused_out =
            run_capture(launch, {"--list-gpus"},
                        {{"VKRELAY2_DAEMON_HOST", "127.0.0.1"},
                         {"VKRELAY2_DAEMON_PORT", std::to_string(closed_port())}});
        VKR_CHECK(refused_out.empty());

        // With the testing escape hatch the offline fallback still prints the local
        // mock list (not hang or crash), so tooling can exercise the path offline.
        const std::string mock_expected = protocol::format_gpu_list(protocol::probe_mocked());
        const std::string fallback_out =
            run_capture(launch, {"--list-gpus"},
                        {{"VKRELAY2_DAEMON_HOST", "127.0.0.1"},
                         {"VKRELAY2_DAEMON_PORT", std::to_string(closed_port())},
                         {"VKRELAY2_ALLOW_MOCK_GPUS", "1"}});
        VKR_CHECK_EQ(normalize(fallback_out), normalize(mock_expected));
    }

    sup.terminate();
    std::remove(port_file.c_str());
    return vkr::test::finish("integration_daemon_endpoint");
}
