// Real (loader-backed) GPU enumeration for the worker.
//
// Worker-only: the supervisor never links Vulkan, so when it wants the real
// adapter list it spawns `vkrelay2-worker --list-gpus`, which calls this. Built
// only on Windows with the Vulkan SDK (see CMakeLists), alongside the real
// backend. Maps the host's real VkPhysicalDevices into the daemon-side GpuDevice
// shape so the existing --list-gpus / selector plumbing works unchanged.
#ifndef VKRELAY2_WINDOWS_WORKER_REAL_GPU_PROBE_HPP
#define VKRELAY2_WINDOWS_WORKER_REAL_GPU_PROBE_HPP

#include "common/protocol/gpu.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace vkr::worker {

// Enumerates the host's real Vulkan physical devices. Returns an empty vector if
// there is no usable loader/driver or no device (the caller then falls back to
// the mocked list). Creates a transient instance at min(loader version, worker
// ceiling), so a lesser-Vulkan host still enumerates.
std::vector<protocol::GpuDevice> probe_real_gpus();

// Formats an n-byte LUID as "0x" + lowercase hex (byte 0 first). The probe uses it
// for GpuDevice.luid; the real backend uses it to match a requested LUID against a
// host VkPhysicalDeviceIDProperties.deviceLUID by string equality -- so a launched
// session selects exactly the device the daemon enumerated. One formatter, one wire.
std::string format_luid(const std::uint8_t* luid, std::size_t n);

} // namespace vkr::worker

#endif // VKRELAY2_WINDOWS_WORKER_REAL_GPU_PROBE_HPP
