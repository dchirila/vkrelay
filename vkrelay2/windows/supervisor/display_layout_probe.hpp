// Isolated Windows display-layout capture and portable reconciliation.
#ifndef VKRELAY2_WINDOWS_SUPERVISOR_DISPLAY_LAYOUT_PROBE_HPP
#define VKRELAY2_WINDOWS_SUPERVISOR_DISPLAY_LAYOUT_PROBE_HPP

#include "common/display/display_layout.hpp"

#include <string>
#include <vector>

namespace vkr::supervisor {

struct RawDisplayPath {
    std::string source_device_name;
    std::string stable_id; // adapter LUID + target ID
    display::DisplayRotation rotation = display::DisplayRotation::Identity;
};

struct RawDisplayMonitor {
    std::string device_name;
    display::RectI32 bounds;
    display::RectI32 work;
    std::uint32_t dpi_x = 96;
    std::uint32_t dpi_y = 96;
    display::DisplayRotation fallback_rotation = display::DisplayRotation::Identity;
    bool primary = false;
    bool attached_to_desktop = false;
    bool mirroring_driver = false;
};

struct VirtualScreenMetrics {
    display::RectI32 bounds;
};

struct DisplayLayoutProbeResult {
    bool ok = false;
    display::DisplayLayout layout;
    std::string reason;
    std::vector<std::string> warnings; // e.g. per-monitor DPI fallback markers
};

DisplayLayoutProbeResult reconcile_display_layout(
    const std::string& snapshot_id, const std::vector<RawDisplayMonitor>& raw_monitors,
    const std::vector<RawDisplayPath>& active_paths, const VirtualScreenMetrics& metrics,
    std::vector<std::string> warnings = {});

// Captures and validates one immutable snapshot. The ID is injected by the future supervisor
// allocator; this is deliberately not called from the live serve path.
DisplayLayoutProbeResult probe_display_layout(const std::string& snapshot_id);

} // namespace vkr::supervisor

#endif // VKRELAY2_WINDOWS_SUPERVISOR_DISPLAY_LAYOUT_PROBE_HPP
