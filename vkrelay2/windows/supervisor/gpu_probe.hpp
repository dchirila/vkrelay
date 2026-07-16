// Daemon-side GPU discovery.
//
// The supervisor must never link Vulkan (the rock-solid-daemon rule), so it
// learns the real adapter list by spawning a disposable worker
// (`<worker> --list-gpus`) and parsing its JSON output. Falls back to the mocked
// list when the worker can't enumerate (no Vulkan build, no loader/device, or a
// non-Windows worker), so callers always get a usable list and the dual-platform
// mock path is preserved.
#ifndef VKRELAY2_WINDOWS_SUPERVISOR_GPU_PROBE_HPP
#define VKRELAY2_WINDOWS_SUPERVISOR_GPU_PROBE_HPP

#include "common/protocol/gpu.hpp"

#include <string>
#include <vector>

namespace vkr::supervisor {

// The host adapter list plus whether it is the real (worker-probed) list or the
// mock fallback. `real` gates faithful launch-time selection: only a real list
// passes a device's stable LUID to the worker as an authoritative match key.
struct GpuProbeResult {
    std::vector<protocol::GpuDevice> devices;
    bool real = false;
};

// Returns the host adapter list: real (via the worker) when available, else the
// mocked list. An empty `worker_path` skips straight to the mock.
GpuProbeResult probe_gpus(const std::string& worker_path);

} // namespace vkr::supervisor

#endif // VKRELAY2_WINDOWS_SUPERVISOR_GPU_PROBE_HPP
