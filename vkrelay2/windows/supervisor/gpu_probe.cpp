#include "windows/supervisor/gpu_probe.hpp"

#include "common/logging/logging.hpp"
#include "common/process/process.hpp"
#include "common/protocol/messages.hpp"
#include "common/util/json.hpp"

#include <exception>
#include <string>
#include <vector>

namespace vkr::supervisor {
namespace {

constexpr char kComponent[] = "supervisor";

// Bounds how long we wait for the enumerate worker to exit before giving up and
// falling back to the mock. Real enumeration is a fast create-instance + query.
constexpr int kProbeTimeoutMs = 5000;

// Spawns `<worker> --list-gpus`, captures stdout, and parses the JSON device
// list. Returns true + fills `out` only on a clean (exit 0, parseable, non-empty)
// result.
bool probe_via_worker(const std::string& worker_path, std::vector<protocol::GpuDevice>& out) {
    process::SpawnRequest req;
    req.argv = {worker_path, "--list-gpus"};
    req.capture_stdout = true;
    req.new_group = true; // contained + killable if it misbehaves
    process::Process child = process::Process::spawn(req);

    // Wait with the timeout FIRST. A concurrent reader thread drains stdout (so the
    // child never blocks on a full pipe), which means read_stdout_to_end() only
    // returns once the child closes stdout -- i.e. it would block forever on a
    // worker that wedges with stdout open, bypassing the deadline. Waiting first
    // bounds that: on timeout we kill the worker (its stdout then EOFs) and fall
    // back; otherwise the child has exited and the drain returns promptly.
    int exit_code = -1;
    if (!child.wait(kProbeTimeoutMs, exit_code)) {
        child.terminate();
        VKR_WARN(kComponent) << "gpu probe worker did not exit in time";
        return false;
    }
    if (exit_code != 0) {
        return false; // worker reported it could not enumerate (no Vulkan / no device)
    }
    const std::string output = child.read_stdout_to_end(); // child has exited; drains to EOF
    json::Value body;
    std::string error;
    if (!json::Value::try_parse(output, body, error)) {
        VKR_WARN(kComponent) << "could not parse worker gpu list: " << error;
        return false;
    }
    std::vector<protocol::GpuDevice> devices = protocol::gpu_list_from_body(body);
    if (devices.empty()) {
        return false;
    }
    out = std::move(devices);
    return true;
}

} // namespace

GpuProbeResult probe_gpus(const std::string& worker_path) {
    if (!worker_path.empty()) {
        try {
            std::vector<protocol::GpuDevice> devices;
            if (probe_via_worker(worker_path, devices)) {
                VKR_INFO(kComponent)
                    << "enumerated " << devices.size() << " real Vulkan adapter(s) via the worker";
                return {std::move(devices), /*real=*/true};
            }
        } catch (const std::exception& e) {
            VKR_WARN(kComponent) << "gpu probe failed: " << e.what();
        }
    }
    VKR_INFO(kComponent) << "using the mocked GPU adapter list";
    return {protocol::probe_mocked(), /*real=*/false};
}

} // namespace vkr::supervisor
