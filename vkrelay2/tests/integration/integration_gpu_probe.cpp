// Daemon-side GPU enumeration.
//
// The supervisor never links Vulkan; it learns the real adapter list by spawning
// `<worker> --list-gpus` and parsing its JSON, falling back to the mocked list
// when the worker can't enumerate (no Vulkan build, no loader/device, or a
// non-Windows worker). This drives probe_gpus() across the fallback paths on both
// platforms, and on a Vulkan-capable worker with a device asserts the list is real
// (no "(mock)" drivers). The worker path is argv[1].
#include "windows/supervisor/gpu_probe.hpp"

#include "common/process/process.hpp"
#include "common/protocol/gpu.hpp"
#include "common/protocol/messages.hpp"
#include "common/util/json.hpp"
#include "tests/test_assert.hpp"

#include <cstdio>
#include <exception>
#include <string>
#include <vector>

using namespace vkr;

namespace {

bool any_mock_driver(const std::vector<protocol::GpuDevice>& devices) {
    for (const auto& d : devices) {
        if (d.driver_name.find("(mock)") != std::string::npos) {
            return true;
        }
    }
    return false;
}

// Runs `<worker> --list-gpus` directly and reports whether it really enumerated
// (exit 0 + parseable, non-empty list), filling `out` with that list.
bool worker_enumerates(const std::string& worker_path, std::vector<protocol::GpuDevice>& out) {
    try {
        process::SpawnRequest req;
        req.argv = {worker_path, "--list-gpus"};
        req.capture_stdout = true;
        req.new_group = true;
        process::Process child = process::Process::spawn(req);
        const std::string output = child.read_stdout_to_end();
        int exit_code = -1;
        child.wait(5000, exit_code);
        if (exit_code != 0) {
            return false;
        }
        json::Value body;
        std::string error;
        if (!json::Value::try_parse(output, body, error)) {
            return false;
        }
        out = protocol::gpu_list_from_body(body);
        return !out.empty();
    } catch (const std::exception&) {
        return false;
    }
}

} // namespace

int main(int argc, char** argv) {
    const std::string worker_path = (argc > 1) ? argv[1] : "";
    const auto mock = protocol::probe_mocked();

    // An empty worker path goes straight to the mock (no spawn): a non-real result.
    const auto empty = supervisor::probe_gpus("");
    VKR_CHECK(!empty.real);
    VKR_CHECK(!empty.devices.empty());
    VKR_CHECK_EQ(empty.devices.size(), mock.size());
    if (!empty.devices.empty() && !mock.empty()) {
        VKR_CHECK_EQ(empty.devices.front().name, mock.front().name);
    }
    VKR_CHECK(any_mock_driver(empty.devices)); // it IS the mock list

    // A bogus worker path: the spawn fails but probe_gpus never throws -- it falls
    // back to the mock list (also not real).
    const auto bogus = supervisor::probe_gpus("vkrelay2-no-such-binary-xyz");
    VKR_CHECK(!bogus.real);
    VKR_CHECK_EQ(bogus.devices.size(), mock.size());
    VKR_CHECK(any_mock_driver(bogus.devices));

    // The real worker path: real list on a Vulkan-capable worker with a device,
    // else the mock fallback. Decide which by asking the worker directly.
    if (!worker_path.empty()) {
        const auto probed = supervisor::probe_gpus(worker_path);
        VKR_CHECK(!probed.devices.empty());

        std::vector<protocol::GpuDevice> direct;
        if (worker_enumerates(worker_path, direct)) {
            std::fprintf(stderr,
                         "integration_gpu_probe: worker enumerated %zu real adapter(s); "
                         "first='%s'\n",
                         direct.size(), direct.front().name.c_str());
            // probe_gpus must surface that real list: flagged real, same count, real
            // names, and no synthetic "(mock)" drivers.
            VKR_CHECK(probed.real);
            VKR_CHECK_EQ(probed.devices.size(), direct.size());
            VKR_CHECK(!probed.devices.front().name.empty());
            VKR_CHECK(!any_mock_driver(probed.devices));
        } else {
            std::fprintf(stderr, "integration_gpu_probe: worker cannot enumerate; "
                                 "probe_gpus fell back to the mock list\n");
            VKR_CHECK(!probed.real);
            VKR_CHECK_EQ(probed.devices.size(), mock.size());
            VKR_CHECK(any_mock_driver(probed.devices));
        }
    }

    return vkr::test::finish("integration_gpu_probe");
}
