// Portable display-topology model and pure coordinate policy.
#ifndef VKRELAY2_COMMON_DISPLAY_DISPLAY_LAYOUT_HPP
#define VKRELAY2_COMMON_DISPLAY_DISPLAY_LAYOUT_HPP

#include "common/util/json.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace vkr::display {

inline constexpr std::uint32_t kDisplayLayoutSchemaVersion = 1;
// Named policy caps. 16,384 matches the relay's conservative Vulkan image/surface ceiling
// and keeps every layout-authored guest position within XCB's signed-16 coordinate range. The
// separate area cap prevents a formally representable square canvas from demanding unbounded
// headless-compositor storage.
inline constexpr std::size_t kMaxDisplayMonitors = 32;
inline constexpr std::int32_t kMaxVirtualDisplayAxis = 16384;
inline constexpr std::uint64_t kMaxVirtualDisplayPixels = 64ull * 1024ull * 1024ull;
inline constexpr std::size_t kMaxSnapshotIdBytes = 128;
inline constexpr std::size_t kMaxMonitorIdBytes = 256;
inline constexpr std::size_t kMaxDeviceNameBytes = 512;

struct PointI32 {
    std::int32_t x = 0;
    std::int32_t y = 0;
};

// Rectangles are half-open: [x, x + width) x [y, y + height).
struct RectI32 {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t width = 0;
    std::int32_t height = 0;
};

bool operator==(const PointI32& a, const PointI32& b);
bool operator==(const RectI32& a, const RectI32& b);

enum class DisplayRotation : std::uint32_t { Identity = 0, Rotate90, Rotate180, Rotate270 };
enum class OutputModel : std::uint32_t { SingleCanvas = 0, PerMonitorOutputs };

struct MonitorDesc {
    std::string stable_id;
    std::string device_name; // diagnostic only; never an identity key
    RectI32 bounds;
    RectI32 work;
    std::uint32_t dpi_x = 0;
    std::uint32_t dpi_y = 0;
    DisplayRotation rotation = DisplayRotation::Identity;
    bool primary = false;
};

struct DisplayLayout {
    std::uint32_t schema_version = kDisplayLayoutSchemaVersion;
    std::string snapshot_id;
    RectI32 virtual_bounds;
    std::string primary_monitor_id;
    std::vector<MonitorDesc> monitors;
};

struct GuestDisplayState {
    std::string snapshot_id;
    std::uint32_t actual_root_width = 0;
    std::uint32_t actual_root_height = 0;
    OutputModel output_model = OutputModel::SingleCanvas;
};

struct ValidationResult {
    bool ok = false;
    std::string reason;
};

enum class LayoutDecodeStatus { Absent, Valid, Malformed, UnsupportedSchema };

struct DisplayLayoutDecodeResult {
    LayoutDecodeStatus status = LayoutDecodeStatus::Absent;
    DisplayLayout layout;
    std::string reason;
};

struct GuestDisplayStateDecodeResult {
    LayoutDecodeStatus status = LayoutDecodeStatus::Absent;
    GuestDisplayState state;
    std::string reason;
};

ValidationResult validate_display_layout(const DisplayLayout& layout);
ValidationResult validate_guest_display_state(const GuestDisplayState& state);
ValidationResult validate_guest_display_state_against_layout(const GuestDisplayState& state,
                                                             const DisplayLayout& layout);

json::Value display_layout_to_json(const DisplayLayout& layout);
DisplayLayoutDecodeResult decode_display_layout_field(const json::Value& parent,
                                                      const std::string& field_name);
json::Value guest_display_state_to_json(const GuestDisplayState& state);
GuestDisplayStateDecodeResult decode_guest_display_state_field(const json::Value& parent,
                                                               const std::string& field_name);

bool host_to_guest(const DisplayLayout& layout, PointI32 host, PointI32& guest);
bool guest_to_host(const DisplayLayout& layout, PointI32 guest, PointI32& host);
bool host_to_guest(const DisplayLayout& layout, const RectI32& host, RectI32& guest);
bool guest_to_host(const DisplayLayout& layout, const RectI32& guest, RectI32& host);

const MonitorDesc* find_monitor_by_id(const DisplayLayout& layout, const std::string& stable_id);
const MonitorDesc* find_monitor_containing(const DisplayLayout& layout, PointI32 point);
const MonitorDesc* find_monitor_largest_intersection(const DisplayLayout& layout,
                                                     const RectI32& rect);
const MonitorDesc* find_monitor_nearest(const DisplayLayout& layout, const RectI32& rect);

// Clamp to one explicitly selected work area. Size is preserved; an oversize axis aligns to the
// top/left edge. Checked arithmetic prevents a hostile origin/size from wrapping.
RectI32 clamp_rect_to_monitor_work(const RectI32& rect, const MonitorDesc& monitor);

// Placement policy: a normal window is clamped to the selected monitor's work area. When a
// window is larger than that work area on one axis, that axis is instead kept within the virtual
// desktop (or aligned to its low edge when it is larger than the whole desktop). This preserves
// app-owned size without stranding a root-sized window at the corner of one small monitor.
RectI32 clamp_rect_to_monitor_work_or_virtual(const RectI32& rect, const MonitorDesc& monitor,
                                              const RectI32& virtual_bounds);

enum class InitialPlacementMode {
    Preserve = 0,      // keep a work-reachable top row; otherwise recover to a concrete monitor
    CenterPrimary = 1, // no position intent (including an unowned splash/dialog)
    CenterOwner = 2,   // transient dialog: center over owner, then clamp to owner's monitor
};

// Place a guest-root CLIENT rectangle from the immutable host layout. `owner_guest` is required
// only for CenterOwner. Returns false on malformed/overflowing input. Preserve returns a rectangle
// byte-for-byte unchanged when its one-pixel client top row intersects any snapshot work area;
// otherwise recovery keeps the same size, intersects a concrete monitor, and never lands in a
// virtual-desktop hole.
bool place_guest_window(const DisplayLayout& layout, const RectI32& desired_guest,
                        InitialPlacementMode mode, const RectI32* owner_guest,
                        RectI32& placed_guest, const MonitorDesc** selected_monitor = nullptr);

// Snapshot-derived WM_GETMINMAXINFO work-area values. Position is relative to the selected
// monitor's full bounds; size is the maximized OUTER work span. The caller may only reduce size by
// a guest-realizability cap and must leave this monitor-relative position intact.
struct MonitorWorkPlacement {
    std::int32_t position_x = 0;
    std::int32_t position_y = 0;
    std::int32_t width = 0;
    std::int32_t height = 0;
};
MonitorWorkPlacement monitor_work_placement(const MonitorDesc& monitor);

// Exact-bounds duplicates represent one mirrored desktop source. Keep a deterministic
// representative (primary first, then lexicographic stable ID) before validation/serialization.
std::vector<MonitorDesc> deduplicate_mirrored_monitors(const std::vector<MonitorDesc>& monitors);

} // namespace vkr::display

#endif // VKRELAY2_COMMON_DISPLAY_DISPLAY_LAYOUT_HPP
