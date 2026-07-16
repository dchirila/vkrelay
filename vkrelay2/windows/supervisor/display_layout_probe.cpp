#include "windows/supervisor/display_layout_probe.hpp"

#include "common/logging/logging.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <limits>
#include <map>
#include <utility>

#ifdef _WIN32
#include "common/argv/command_line.hpp"

#include <shellscalingapi.h>
#include <windows.h>
#endif

namespace vkr::supervisor {
namespace {

constexpr char kComponent[] = "display-probe";
constexpr int kCaptureAttempts = 3;
constexpr int kQdcAttempts = 3;

std::string normalized_device_name(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return value;
}

bool preferred_path(const RawDisplayPath& candidate, const RawDisplayPath& current) {
    return candidate.stable_id < current.stable_id;
}

#ifdef _WIN32

struct ThreadDpiContext {
    DPI_AWARENESS_CONTEXT previous = nullptr;
    ThreadDpiContext() {
        previous = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    }
    ~ThreadDpiContext() {
        if (previous != nullptr) {
            SetThreadDpiAwarenessContext(previous);
        }
    }
    bool ok() const { return previous != nullptr; }
};

display::DisplayRotation rotation_from_qdc(DISPLAYCONFIG_ROTATION rotation) {
    switch (rotation) {
    case DISPLAYCONFIG_ROTATION_ROTATE90:
        return display::DisplayRotation::Rotate90;
    case DISPLAYCONFIG_ROTATION_ROTATE180:
        return display::DisplayRotation::Rotate180;
    case DISPLAYCONFIG_ROTATION_ROTATE270:
        return display::DisplayRotation::Rotate270;
    default:
        return display::DisplayRotation::Identity;
    }
}

display::DisplayRotation rotation_from_devmode(DWORD rotation) {
    switch (rotation) {
    case DMDO_90:
        return display::DisplayRotation::Rotate90;
    case DMDO_180:
        return display::DisplayRotation::Rotate180;
    case DMDO_270:
        return display::DisplayRotation::Rotate270;
    default:
        return display::DisplayRotation::Identity;
    }
}

std::string qdc_stable_id(const DISPLAYCONFIG_PATH_TARGET_INFO& target) {
    char buffer[96]{};
    std::snprintf(buffer, sizeof(buffer), "qdc:%08x:%08x:%08x",
                  static_cast<unsigned>(target.adapterId.HighPart),
                  static_cast<unsigned>(target.adapterId.LowPart),
                  static_cast<unsigned>(target.id));
    return buffer;
}

bool capture_qdc_paths(std::vector<RawDisplayPath>& paths, std::string& reason) {
    constexpr UINT32 flags = QDC_ONLY_ACTIVE_PATHS | QDC_VIRTUAL_MODE_AWARE;
    for (int attempt = 0; attempt < kQdcAttempts; ++attempt) {
        UINT32 path_count = 0;
        UINT32 mode_count = 0;
        LONG rc = GetDisplayConfigBufferSizes(flags, &path_count, &mode_count);
        if (rc != ERROR_SUCCESS) {
            reason = "GetDisplayConfigBufferSizes failed (error " + std::to_string(rc) + ")";
            return false;
        }
        std::vector<DISPLAYCONFIG_PATH_INFO> qdc_paths(path_count);
        std::vector<DISPLAYCONFIG_MODE_INFO> modes(mode_count);
        rc = QueryDisplayConfig(flags, &path_count, qdc_paths.data(), &mode_count, modes.data(),
                                nullptr);
        if (rc == ERROR_INSUFFICIENT_BUFFER) {
            continue;
        }
        if (rc != ERROR_SUCCESS) {
            reason = "QueryDisplayConfig failed (error " + std::to_string(rc) + ")";
            return false;
        }
        qdc_paths.resize(path_count);
        for (const DISPLAYCONFIG_PATH_INFO& path : qdc_paths) {
            if ((path.flags & DISPLAYCONFIG_PATH_ACTIVE) == 0 || !path.targetInfo.targetAvailable) {
                continue;
            }
            DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
            source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
            source.header.size = sizeof(source);
            source.header.adapterId = path.sourceInfo.adapterId;
            source.header.id = path.sourceInfo.id;
            rc = DisplayConfigGetDeviceInfo(&source.header);
            if (rc != ERROR_SUCCESS) {
                reason = "DisplayConfigGetDeviceInfo(source name) failed (error " +
                         std::to_string(rc) + ")";
                return false;
            }
            RawDisplayPath raw;
            raw.source_device_name = argv::wide_to_utf8(source.viewGdiDeviceName);
            raw.stable_id = qdc_stable_id(path.targetInfo);
            raw.rotation = rotation_from_qdc(path.targetInfo.rotation);
            paths.push_back(std::move(raw));
        }
        return true;
    }
    reason = "QueryDisplayConfig buffer changed on all 3 bounded attempts";
    return false;
}

struct EnumContext {
    std::vector<RawDisplayMonitor>* monitors = nullptr;
    std::vector<std::string>* warnings = nullptr;
    bool pmv2_window_dpi = false;
    std::string error;
};

bool read_monitor_dpi(HMONITOR hmonitor, const MONITORINFOEXW& info, UINT& dpi_x, UINT& dpi_y,
                      bool pmv2_window_dpi, std::vector<std::string>& warnings) {
    if (pmv2_window_dpi) {
        const int x = info.rcMonitor.left + (info.rcMonitor.right - info.rcMonitor.left) / 2;
        const int y = info.rcMonitor.top + (info.rcMonitor.bottom - info.rcMonitor.top) / 2;
        HWND probe = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC", L"", WS_POPUP,
                                     x, y, 1, 1, nullptr, nullptr, nullptr, nullptr);
        if (probe != nullptr) {
            const HMONITOR landed = MonitorFromWindow(probe, MONITOR_DEFAULTTONULL);
            const UINT dpi = landed == hmonitor ? GetDpiForWindow(probe) : 0;
            DestroyWindow(probe);
            if (dpi != 0) {
                dpi_x = dpi;
                dpi_y = dpi;
                return true;
            }
        }
    }

    UINT fallback_x = 0;
    UINT fallback_y = 0;
    if (SUCCEEDED(GetDpiForMonitor(hmonitor, MDT_EFFECTIVE_DPI, &fallback_x, &fallback_y)) &&
        fallback_x != 0 && fallback_y != 0) {
        dpi_x = fallback_x;
        dpi_y = fallback_y;
        warnings.push_back("dpi fallback GetDpiForMonitor: " + argv::wide_to_utf8(info.szDevice));
        return true;
    }
    dpi_x = USER_DEFAULT_SCREEN_DPI;
    dpi_y = USER_DEFAULT_SCREEN_DPI;
    warnings.push_back("dpi fallback 96: " + argv::wide_to_utf8(info.szDevice));
    return true;
}

BOOL CALLBACK enum_monitor(HMONITOR hmonitor, HDC, LPRECT, LPARAM param) {
    auto& context = *reinterpret_cast<EnumContext*>(param);
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(hmonitor, &info)) {
        context.error = "GetMonitorInfoW failed (error " + std::to_string(GetLastError()) + ")";
        return FALSE;
    }
    DISPLAY_DEVICEW device{};
    BOOL have_device = FALSE;
    for (DWORD index = 0;; ++index) {
        DISPLAY_DEVICEW candidate{};
        candidate.cb = sizeof(candidate);
        if (!EnumDisplayDevicesW(nullptr, index, &candidate, 0)) {
            break;
        }
        if (_wcsicmp(candidate.DeviceName, info.szDevice) == 0) {
            device = candidate;
            have_device = TRUE;
            break;
        }
    }

    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    const BOOL have_mode = EnumDisplaySettingsExW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode, 0);

    UINT dpi_x = USER_DEFAULT_SCREEN_DPI;
    UINT dpi_y = USER_DEFAULT_SCREEN_DPI;
    read_monitor_dpi(hmonitor, info, dpi_x, dpi_y, context.pmv2_window_dpi, *context.warnings);

    RawDisplayMonitor monitor;
    monitor.device_name = argv::wide_to_utf8(info.szDevice);
    monitor.bounds = {info.rcMonitor.left, info.rcMonitor.top,
                      info.rcMonitor.right - info.rcMonitor.left,
                      info.rcMonitor.bottom - info.rcMonitor.top};
    monitor.work = {info.rcWork.left, info.rcWork.top, info.rcWork.right - info.rcWork.left,
                    info.rcWork.bottom - info.rcWork.top};
    monitor.dpi_x = dpi_x;
    monitor.dpi_y = dpi_y;
    monitor.fallback_rotation = have_mode ? rotation_from_devmode(mode.dmDisplayOrientation)
                                          : display::DisplayRotation::Identity;
    monitor.primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
    monitor.attached_to_desktop =
        have_device && (device.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) != 0;
    monitor.mirroring_driver =
        have_device && (device.StateFlags & DISPLAY_DEVICE_MIRRORING_DRIVER) != 0;
    context.monitors->push_back(std::move(monitor));
    return TRUE;
}

DisplayLayoutProbeResult capture_once(const std::string& snapshot_id) {
    DisplayLayoutProbeResult result;
    ThreadDpiContext dpi_context;
    if (!dpi_context.ok()) {
        // The context governs GetMonitorInfoW geometry as well as the preferred DPI read. Without
        // it an unmanifested mixed-DPI supervisor can receive virtualized rectangles, so capture
        // must fail and take the bounded whole-snapshot retry. DPI-read failures after a valid
        // context still use the non-fatal GetDpiForMonitor -> 96 metadata chain below.
        result.reason = "SetThreadDpiAwarenessContext(PMv2) failed (error " +
                        std::to_string(GetLastError()) + ")";
        return result;
    }

    std::vector<RawDisplayPath> paths;
    if (!capture_qdc_paths(paths, result.reason)) {
        // QDC is the preferred identity source, not geometry authority. Reconciliation may use
        // attached non-mirroring GDI fallbacks, so retain the failure as a diagnostic.
        result.warnings.push_back("QDC identity unavailable: " + result.reason);
        result.reason.clear();
        paths.clear();
    }

    std::vector<RawDisplayMonitor> monitors;
    EnumContext context{&monitors, &result.warnings, dpi_context.ok(), {}};
    if (!EnumDisplayMonitors(nullptr, nullptr, enum_monitor, reinterpret_cast<LPARAM>(&context))) {
        result.reason = context.error.empty() ? "EnumDisplayMonitors failed (error " +
                                                    std::to_string(GetLastError()) + ")"
                                              : context.error;
        return result;
    }
    VirtualScreenMetrics metrics;
    metrics.bounds = {GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
                      GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN)};
    return reconcile_display_layout(snapshot_id, monitors, paths, metrics,
                                    std::move(result.warnings));
}

#endif // _WIN32

} // namespace

DisplayLayoutProbeResult
reconcile_display_layout(const std::string& snapshot_id,
                         const std::vector<RawDisplayMonitor>& raw_monitors,
                         const std::vector<RawDisplayPath>& active_paths,
                         const VirtualScreenMetrics& metrics, std::vector<std::string> warnings) {
    DisplayLayoutProbeResult result;
    result.warnings = std::move(warnings);

    std::multimap<std::string, RawDisplayPath> paths_by_source;
    for (const RawDisplayPath& path : active_paths) {
        paths_by_source.emplace(normalized_device_name(path.source_device_name), path);
    }

    std::vector<display::MonitorDesc> monitors;
    for (const RawDisplayMonitor& raw : raw_monitors) {
        if (raw.mirroring_driver) {
            continue;
        }
        const std::string source = normalized_device_name(raw.device_name);
        const auto range = paths_by_source.equal_range(source);
        const RawDisplayPath* representative = nullptr;
        for (auto it = range.first; it != range.second; ++it) {
            if (representative == nullptr || preferred_path(it->second, *representative)) {
                representative = &it->second;
            }
        }
        if (representative == nullptr && !raw.attached_to_desktop) {
            continue; // invisible pseudo-monitor with no active QDC path
        }
        display::MonitorDesc monitor;
        monitor.stable_id = representative != nullptr
                                ? representative->stable_id
                                : "gdi:" + normalized_device_name(raw.device_name);
        monitor.device_name = raw.device_name;
        monitor.bounds = raw.bounds;
        monitor.work = raw.work;
        monitor.dpi_x = raw.dpi_x;
        monitor.dpi_y = raw.dpi_y;
        monitor.rotation =
            representative != nullptr ? representative->rotation : raw.fallback_rotation;
        monitor.primary = raw.primary;
        monitors.push_back(std::move(monitor));
    }
    monitors = display::deduplicate_mirrored_monitors(monitors);
    if (monitors.empty()) {
        result.reason = "display probe found no active non-mirroring monitors";
        return result;
    }

    display::DisplayLayout layout;
    layout.snapshot_id = snapshot_id;
    layout.monitors = std::move(monitors);
    std::int64_t left = std::numeric_limits<std::int64_t>::max();
    std::int64_t top = std::numeric_limits<std::int64_t>::max();
    std::int64_t right = std::numeric_limits<std::int64_t>::min();
    std::int64_t bottom = std::numeric_limits<std::int64_t>::min();
    for (const display::MonitorDesc& monitor : layout.monitors) {
        left = std::min(left, static_cast<std::int64_t>(monitor.bounds.x));
        top = std::min(top, static_cast<std::int64_t>(monitor.bounds.y));
        right = std::max(right, static_cast<std::int64_t>(monitor.bounds.x) + monitor.bounds.width);
        bottom =
            std::max(bottom, static_cast<std::int64_t>(monitor.bounds.y) + monitor.bounds.height);
        if (monitor.primary) {
            layout.primary_monitor_id = monitor.stable_id;
        }
    }
    const std::int64_t width = right - left;
    const std::int64_t height = bottom - top;
    if (left < std::numeric_limits<std::int32_t>::min() ||
        left > std::numeric_limits<std::int32_t>::max() ||
        top < std::numeric_limits<std::int32_t>::min() ||
        top > std::numeric_limits<std::int32_t>::max() || width <= 0 ||
        width > std::numeric_limits<std::int32_t>::max() || height <= 0 ||
        height > std::numeric_limits<std::int32_t>::max()) {
        result.reason = "display monitor union violates the signed-32 coordinate/dimension cap "
                        "(-2147483648..2147483647)";
        return result;
    }
    layout.virtual_bounds = {static_cast<std::int32_t>(left), static_cast<std::int32_t>(top),
                             static_cast<std::int32_t>(width), static_cast<std::int32_t>(height)};
    const display::ValidationResult validation = display::validate_display_layout(layout);
    if (!validation.ok) {
        result.reason = validation.reason;
        return result;
    }
    // The accepted monitor union is authoritative; the system metrics are an independent
    // coherence check so a torn desktop snapshot is retried instead of being published.
    if (!(layout.virtual_bounds == metrics.bounds)) {
        result.reason = "accepted monitor union disagrees with SM_X/Y/CX/CYVIRTUALSCREEN";
        return result;
    }
    result.ok = true;
    result.layout = std::move(layout);
    return result;
}

DisplayLayoutProbeResult probe_display_layout(const std::string& snapshot_id) {
#ifdef _WIN32
    DisplayLayoutProbeResult last;
    for (int attempt = 0; attempt < kCaptureAttempts; ++attempt) {
        last = capture_once(snapshot_id);
        if (last.ok) {
            for (const std::string& warning : last.warnings) {
                VKR_WARN(kComponent) << warning;
            }
            return last;
        }
    }
    last.reason += " after 3 bounded topology-capture attempts";
    return last;
#else
    DisplayLayoutProbeResult result;
    result.reason = "Windows display probe unavailable on this platform";
    (void) snapshot_id;
    return result;
#endif
}

} // namespace vkr::supervisor
