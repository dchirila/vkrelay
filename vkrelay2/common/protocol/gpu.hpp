// Daemon-side GPU discovery + selection contract.
//
// probe_mocked() returns reference adapters for deterministic tests; the same structs and selector
// grammar carry real Vulkan adapter enumeration.
#ifndef VKRELAY2_COMMON_PROTOCOL_GPU_HPP
#define VKRELAY2_COMMON_PROTOCOL_GPU_HPP

#include <cstdint>
#include <string>
#include <vector>

namespace vkr::protocol {

enum class GpuDeviceType { Discrete, Integrated, Cpu, Virtual, Other };
const char* to_string(GpuDeviceType type);

struct GpuDevice {
    int index = 0;
    std::string name;
    std::uint32_t vendor_id = 0;
    std::uint32_t device_id = 0;
    GpuDeviceType type = GpuDeviceType::Other;
    std::string api_version;
    std::string driver_name;
    std::string luid;
    bool usable = false;
    std::string reason;             // "ok" or a stable diagnostic reason code
    std::vector<std::string> roles; // top-level-present, readback-composite, software-test
    bool default_high_performance = false;
    bool default_integrated = false;
};

// Deterministic fallback used until real Vulkan probing is available.
std::vector<GpuDevice> probe_mocked();

// Returns a pasteable, bug-report-friendly listing.
std::string format_gpu_list(const std::vector<GpuDevice>& devices);

enum class SelectorKind { Auto, HighPerformance, Integrated, Vendor, Device, Luid, Index, Name };

struct GpuSelector {
    SelectorKind kind = SelectorKind::Auto;
    std::uint32_t vendor_id = 0;
    std::uint32_t device_id = 0;
    std::string luid;
    int index = 0;
    std::string name;
    std::string raw;
};

struct SelectorParse {
    bool ok = false;
    GpuSelector selector;
    std::string error;
};

// Parses a GPU selector string.
SelectorParse parse_selector(const std::string& text);

// True when the selector is stable but discouraged in saved configs (index:).
bool selector_is_unstable(const GpuSelector& selector);

// Returns the chosen device or nullptr; sets `reason` either way.
const GpuDevice* select_device(const std::vector<GpuDevice>& devices, const GpuSelector& selector,
                               std::string& reason);

} // namespace vkr::protocol

#endif // VKRELAY2_COMMON_PROTOCOL_GPU_HPP
