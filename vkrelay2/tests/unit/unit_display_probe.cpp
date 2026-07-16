#include "tests/test_assert.hpp"
#include "windows/supervisor/display_layout_probe.hpp"
#include "windows/supervisor/display_snapshot_cache.hpp"

#include <string>
#include <vector>

using namespace vkr;
using namespace vkr::supervisor;

namespace {

RawDisplayMonitor raw(std::string device, display::RectI32 bounds, bool primary = false) {
    RawDisplayMonitor value;
    value.device_name = std::move(device);
    value.bounds = bounds;
    value.work = bounds;
    value.dpi_x = 96;
    value.dpi_y = 96;
    value.primary = primary;
    value.attached_to_desktop = true;
    return value;
}

void test_qdc_correlation_clone_dedup_and_pseudo_filter() {
    std::vector<RawDisplayMonitor> monitors = {raw("\\\\.\\DISPLAY1", {-1920, 0, 1920, 1080}),
                                               raw("\\\\.\\DISPLAY2", {0, 0, 1920, 1080}, true),
                                               raw("MIRROR", {0, 0, 1920, 1080}),
                                               raw("UNATTACHED", {4000, 0, 640, 480})};
    monitors[2].attached_to_desktop = false;
    monitors[2].mirroring_driver = true;
    monitors[3].attached_to_desktop = false;
    const std::vector<RawDisplayPath> paths = {
        {"\\\\.\\display1", "qdc:2", display::DisplayRotation::Identity},
        {"\\\\.\\DISPLAY1", "qdc:1", display::DisplayRotation::Rotate180},
        {"\\\\.\\DISPLAY2", "qdc:3", display::DisplayRotation::Identity}};
    const DisplayLayoutProbeResult result =
        reconcile_display_layout("sup/display-1", monitors, paths, {{-1920, 0, 3840, 1080}});
    VKR_CHECK(result.ok);
    VKR_CHECK_EQ(result.layout.monitors.size(), static_cast<std::size_t>(2));
    const display::MonitorDesc* left = display::find_monitor_by_id(result.layout, "qdc:1");
    VKR_CHECK(left != nullptr);
    if (left != nullptr) {
        VKR_CHECK(left->rotation == display::DisplayRotation::Rotate180);
    }
    VKR_CHECK_EQ(result.layout.primary_monitor_id, std::string("qdc:3"));
}

void test_gdi_fallback_and_metrics_mismatch() {
    const std::vector<RawDisplayMonitor> monitors = {
        raw("\\\\.\\DISPLAY9", {0, 0, 800, 600}, true)};
    const DisplayLayoutProbeResult fallback = reconcile_display_layout(
        "sup/display-2", monitors, {}, {{0, 0, 800, 600}}, {"QDC identity unavailable"});
    VKR_CHECK(fallback.ok);
    VKR_CHECK_EQ(fallback.layout.monitors.front().stable_id, std::string("gdi:\\\\.\\DISPLAY9"));
    VKR_CHECK_EQ(fallback.warnings.size(), static_cast<std::size_t>(1));

    const DisplayLayoutProbeResult mismatch =
        reconcile_display_layout("sup/display-3", monitors, {}, {{0, 0, 1024, 768}});
    VKR_CHECK(!mismatch.ok);
    VKR_CHECK(mismatch.reason.find("SM_X/Y/CX/CYVIRTUALSCREEN") != std::string::npos);
}

void test_exact_bounds_dedup_prefers_primary() {
    std::vector<RawDisplayMonitor> monitors = {raw("A", {0, 0, 800, 600}),
                                               raw("B", {0, 0, 800, 600}, true)};
    const std::vector<RawDisplayPath> paths = {{"A", "qdc:a", display::DisplayRotation::Identity},
                                               {"B", "qdc:b", display::DisplayRotation::Identity}};
    const DisplayLayoutProbeResult result =
        reconcile_display_layout("sup/display-4", monitors, paths, {{0, 0, 800, 600}});
    VKR_CHECK(result.ok);
    VKR_CHECK_EQ(result.layout.monitors.size(), static_cast<std::size_t>(1));
    VKR_CHECK_EQ(result.layout.primary_monitor_id, std::string("qdc:b"));
}

void test_portrait_rotation_is_metadata_not_geometry_transform() {
    // EnumDisplayMonitors already reports the post-rotation physical rectangle. Correlating a QDC
    // Rotate90 path must carry metadata only: swapping width/height again would double-rotate it.
    const std::vector<RawDisplayMonitor> monitors = {
        raw("\\\\.\\DISPLAY1", {0, 0, 1920, 1080}, true),
        raw("\\\\.\\DISPLAY2", {1920, -240, 1080, 1920})};
    const std::vector<RawDisplayPath> paths = {
        {"\\\\.\\DISPLAY1", "qdc:landscape", display::DisplayRotation::Identity},
        {"\\\\.\\DISPLAY2", "qdc:portrait", display::DisplayRotation::Rotate90}};
    const DisplayLayoutProbeResult result =
        reconcile_display_layout("sup/portrait", monitors, paths, {{0, -240, 3000, 1920}});
    VKR_CHECK(result.ok);
    const display::MonitorDesc* portrait =
        display::find_monitor_by_id(result.layout, "qdc:portrait");
    VKR_CHECK(portrait != nullptr);
    if (portrait != nullptr) {
        VKR_CHECK(portrait->bounds == (display::RectI32{1920, -240, 1080, 1920}));
        VKR_CHECK(portrait->rotation == display::DisplayRotation::Rotate90);
    }
    VKR_CHECK(result.layout.virtual_bounds == (display::RectI32{0, -240, 3000, 1920}));
}

void test_snapshot_cache_is_bounded_and_returns_copies() {
    int captures = 0;
    DisplaySnapshotCache cache("sup-cache", 2, [&](const std::string& snapshot_id) {
        ++captures;
        return reconcile_display_layout(
            snapshot_id, {raw("\\\\.\\DISPLAY1", {0, 0, 800, 600}, true)}, {}, {{0, 0, 800, 600}});
    });
    const display::DisplayLayoutDecodeResult first = cache.capture_for_handshake();
    const display::DisplayLayoutDecodeResult second = cache.capture_for_handshake();
    VKR_CHECK(first.status == display::LayoutDecodeStatus::Valid);
    VKR_CHECK(second.status == display::LayoutDecodeStatus::Valid);
    VKR_CHECK_EQ(first.layout.snapshot_id, std::string("sup-cache/display-1"));
    VKR_CHECK_EQ(second.layout.snapshot_id, std::string("sup-cache/display-2"));

    display::DisplayLayout copy;
    std::string reason;
    VKR_CHECK(cache.resolve_copy(first.layout.snapshot_id, copy, reason));
    copy.snapshot_id = "mutated-local-copy";
    display::DisplayLayout copy_again;
    VKR_CHECK(cache.resolve_copy(first.layout.snapshot_id, copy_again, reason));
    VKR_CHECK_EQ(copy_again.snapshot_id, first.layout.snapshot_id);
    VKR_CHECK(!cache.resolve_copy("another-supervisor/display-1", copy, reason));
    VKR_CHECK(reason.find("another supervisor") != std::string::npos);

    const display::DisplayLayoutDecodeResult third = cache.capture_for_handshake();
    VKR_CHECK(third.status == display::LayoutDecodeStatus::Valid);
    VKR_CHECK_EQ(cache.size(), static_cast<std::size_t>(2));
    VKR_CHECK(!cache.resolve_copy(first.layout.snapshot_id, copy, reason));
    VKR_CHECK(reason.find("unknown or expired") != std::string::npos);
    VKR_CHECK(cache.resolve_copy(second.layout.snapshot_id, copy, reason));
    VKR_CHECK_EQ(captures, 3);
}

void test_snapshot_cache_capture_failure_is_not_absence() {
    DisplaySnapshotCache cache("sup-fail", 2, [](const std::string&) {
        DisplayLayoutProbeResult failed;
        failed.reason = "synthetic PMv2 capture failure";
        return failed;
    });
    const display::DisplayLayoutDecodeResult result = cache.capture_for_handshake();
    VKR_CHECK(result.status == display::LayoutDecodeStatus::Malformed);
    VKR_CHECK(result.reason.find("PMv2") != std::string::npos);
    VKR_CHECK_EQ(cache.size(), static_cast<std::size_t>(0));
}

} // namespace

int main() {
    test_qdc_correlation_clone_dedup_and_pseudo_filter();
    test_gdi_fallback_and_metrics_mismatch();
    test_exact_bounds_dedup_prefers_primary();
    test_portrait_rotation_is_metadata_not_geometry_transform();
    test_snapshot_cache_is_bounded_and_returns_copies();
    test_snapshot_cache_capture_failure_is_not_absence();
    return vkr::test::finish("unit_display_probe");
}
