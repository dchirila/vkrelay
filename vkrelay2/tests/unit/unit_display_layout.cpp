#include "common/display/display_layout.hpp"
#include "tests/test_assert.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

using namespace vkr;
using namespace vkr::display;

namespace {

struct Fixture {
    std::string name;
    DisplayLayout layout;
};

MonitorDesc monitor(std::string id, RectI32 bounds, bool primary = false, std::uint32_t dpi = 96,
                    DisplayRotation rotation = DisplayRotation::Identity) {
    MonitorDesc value;
    value.stable_id = std::move(id);
    value.device_name = "\\\\.\\DISPLAY-" + value.stable_id;
    value.bounds = bounds;
    value.work = bounds;
    value.dpi_x = dpi;
    value.dpi_y = dpi;
    value.rotation = rotation;
    value.primary = primary;
    return value;
}

DisplayLayout layout(std::string snapshot, std::vector<MonitorDesc> monitors) {
    DisplayLayout value;
    value.snapshot_id = std::move(snapshot);
    value.monitors = std::move(monitors);
    std::int64_t left = std::numeric_limits<std::int64_t>::max();
    std::int64_t top = std::numeric_limits<std::int64_t>::max();
    std::int64_t right = std::numeric_limits<std::int64_t>::min();
    std::int64_t bottom = std::numeric_limits<std::int64_t>::min();
    for (const MonitorDesc& item : value.monitors) {
        left = std::min(left, static_cast<std::int64_t>(item.bounds.x));
        top = std::min(top, static_cast<std::int64_t>(item.bounds.y));
        right = std::max(right, static_cast<std::int64_t>(item.bounds.x) + item.bounds.width);
        bottom = std::max(bottom, static_cast<std::int64_t>(item.bounds.y) + item.bounds.height);
        if (item.primary) {
            value.primary_monitor_id = item.stable_id;
        }
    }
    value.virtual_bounds = {static_cast<std::int32_t>(left), static_cast<std::int32_t>(top),
                            static_cast<std::int32_t>(right - left),
                            static_cast<std::int32_t>(bottom - top)};
    return value;
}

std::vector<Fixture> fixtures() {
    std::vector<Fixture> values;
    values.push_back({"single_fhd", layout("fx-single", {monitor("a", {0, 0, 1920, 1080}, true)})});
    values.push_back(
        {"dual_fhd_primary_left", layout("fx-dual", {monitor("a", {0, 0, 1920, 1080}, true),
                                                     monitor("b", {1920, 0, 1920, 1080})})});
    values.push_back({"triple_fhd_primary_left",
                      layout("fx-triple-left", {monitor("a", {0, 0, 1920, 1080}, true),
                                                monitor("b", {1920, 0, 1920, 1080}),
                                                monitor("c", {3840, 0, 1920, 1080})})});
    values.push_back({"triple_fhd_primary_middle",
                      layout("fx-triple-middle", {monitor("left", {-1920, 0, 1920, 1080}),
                                                  monitor("middle", {0, 0, 1920, 1080}, true),
                                                  monitor("right", {1920, 0, 1920, 1080})})});
    values.push_back({"triple_fhd_primary_right",
                      layout("fx-triple-right", {monitor("left", {-3840, 0, 1920, 1080}),
                                                 monitor("middle", {-1920, 0, 1920, 1080}),
                                                 monitor("right", {0, 0, 1920, 1080}, true)})});
    values.push_back({"portrait_right_of_landscape",
                      layout("fx-portrait-right", {monitor("landscape", {0, 0, 1920, 1080}, true),
                                                   monitor("portrait", {1920, 0, 1080, 1920}, false,
                                                           144, DisplayRotation::Rotate90)})});
    values.push_back(
        {"portrait_left_of_landscape",
         layout("fx-portrait-left", {monitor("portrait", {-1080, 0, 1080, 1920}, false, 144,
                                             DisplayRotation::Rotate270),
                                     monitor("landscape", {0, 0, 1920, 1080}, true)})});
    values.push_back(
        {"fhd_then_4k", layout("fx-fhd-4k", {monitor("fhd", {0, 0, 1920, 1080}, true, 96),
                                             monitor("4k", {1920, 0, 3840, 2160}, false, 192)})});
    values.push_back({"4k_then_fhd_negative_origin",
                      layout("fx-4k-fhd", {monitor("4k", {-3840, 0, 3840, 2160}, false, 192),
                                           monitor("fhd", {0, 0, 1920, 1080}, true, 96)})});
    values.push_back({"vertical_stack_negative_y",
                      layout("fx-vertical", {monitor("above", {0, -1080, 1920, 1080}),
                                             monitor("primary", {0, 0, 1920, 1080}, true)})});
    values.push_back(
        {"staggered", layout("fx-staggered", {monitor("low-left", {0, 400, 1920, 1080}, true),
                                              monitor("high-right", {1920, 0, 1920, 1080})})});

    DisplayLayout taskbars =
        layout("fx-taskbars", {monitor("left-taskbar", {0, 0, 1920, 1080}, true),
                               monitor("top-taskbar", {1920, 0, 1920, 1080}),
                               monitor("right-taskbar", {3840, 0, 1920, 1080})});
    taskbars.monitors[0].work = {48, 0, 1872, 1080};
    taskbars.monitors[0].dpi_y = 120; // prove per-axis metadata survives the model/codec
    taskbars.monitors[1].work = {1920, 40, 1920, 1040};
    taskbars.monitors[1].rotation = DisplayRotation::Rotate180;
    taskbars.monitors[2].work = {3840, 0, 1872, 1080};
    values.push_back({"per_monitor_taskbars", std::move(taskbars)});

    values.push_back({"staggered_negXY_mixed_dpi_portrait_primary",
                      layout("fx-live", {monitor("laptop", {-6400, 326, 2560, 1600}, false, 144),
                                         monitor("4k", {-3840, -222, 3840, 2160}, false, 144),
                                         monitor("primary-portrait", {0, 0, 1200, 1920}, true, 96,
                                                 DisplayRotation::Rotate90)})});
    values.back().layout.monitors[0].work = {-6400, 326, 2560, 1528};
    values.back().layout.monitors[1].work = {-3840, -222, 3840, 2088};
    values.back().layout.monitors[2].work = {0, 0, 1200, 1872};
    return values;
}

void assert_rect_inside_when_it_fits(const RectI32& rect, const RectI32& work) {
    VKR_CHECK(rect.x >= work.x);
    VKR_CHECK(rect.y >= work.y);
    VKR_CHECK(static_cast<std::int64_t>(rect.x) + rect.width <=
              static_cast<std::int64_t>(work.x) + work.width);
    VKR_CHECK(static_cast<std::int64_t>(rect.y) + rect.height <=
              static_cast<std::int64_t>(work.y) + work.height);
}

void test_full_fixture_matrix() {
    for (const Fixture& fixture : fixtures()) {
        const ValidationResult valid = validate_display_layout(fixture.layout);
        VKR_CHECK(valid.ok);

        json::Value parent = json::Value::make_object();
        parent.set("layout", display_layout_to_json(fixture.layout));
        const DisplayLayoutDecodeResult decoded = decode_display_layout_field(parent, "layout");
        VKR_CHECK(decoded.status == LayoutDecodeStatus::Valid);
        VKR_CHECK_EQ(decoded.layout.monitors.size(), fixture.layout.monitors.size());

        for (const MonitorDesc& item : fixture.layout.monitors) {
            const std::vector<PointI32> points = {
                {item.bounds.x, item.bounds.y},
                {static_cast<std::int32_t>(item.bounds.x + item.bounds.width - 1),
                 static_cast<std::int32_t>(item.bounds.y + item.bounds.height - 1)},
                {static_cast<std::int32_t>(item.bounds.x + item.bounds.width / 2),
                 static_cast<std::int32_t>(item.bounds.y + item.bounds.height / 2)}};
            for (PointI32 host : points) {
                PointI32 guest;
                PointI32 round_trip;
                VKR_CHECK(host_to_guest(fixture.layout, host, guest));
                VKR_CHECK(guest_to_host(fixture.layout, guest, round_trip));
                VKR_CHECK(round_trip == host);
            }

            const PointI32 center{
                static_cast<std::int32_t>(item.bounds.x + item.bounds.width / 2),
                static_cast<std::int32_t>(item.bounds.y + item.bounds.height / 2)};
            const MonitorDesc* containing = find_monitor_containing(fixture.layout, center);
            VKR_CHECK(containing != nullptr);
            if (containing != nullptr) {
                VKR_CHECK_EQ(containing->stable_id, item.stable_id);
            }
            const MonitorDesc* right_boundary = find_monitor_containing(
                fixture.layout,
                {static_cast<std::int32_t>(item.bounds.x + item.bounds.width), center.y});
            VKR_CHECK(right_boundary == nullptr || right_boundary->stable_id != item.stable_id);
            const MonitorDesc* bottom_boundary = find_monitor_containing(
                fixture.layout,
                {center.x, static_cast<std::int32_t>(item.bounds.y + item.bounds.height)});
            VKR_CHECK(bottom_boundary == nullptr || bottom_boundary->stable_id != item.stable_id);
            const MonitorDesc* largest =
                find_monitor_largest_intersection(fixture.layout, item.bounds);
            VKR_CHECK(largest != nullptr);
            if (largest != nullptr) {
                VKR_CHECK_EQ(largest->stable_id, item.stable_id);
            }
            const RectI32 center_rect{center.x, center.y, 1, 1};
            const MonitorDesc* nearest = find_monitor_nearest(fixture.layout, center_rect);
            VKR_CHECK(nearest != nullptr);
            if (nearest != nullptr) {
                VKR_CHECK_EQ(nearest->stable_id, item.stable_id);
            }

            RectI32 host_rect = item.work;
            RectI32 guest_rect;
            RectI32 round_trip_rect;
            VKR_CHECK(host_to_guest(fixture.layout, host_rect, guest_rect));
            VKR_CHECK(guest_to_host(fixture.layout, guest_rect, round_trip_rect));
            VKR_CHECK(round_trip_rect == host_rect);

            const MonitorWorkPlacement maximize = monitor_work_placement(item);
            VKR_CHECK_EQ(maximize.position_x, item.work.x - item.bounds.x);
            VKR_CHECK_EQ(maximize.position_y, item.work.y - item.bounds.y);
            VKR_CHECK_EQ(maximize.width, item.work.width);
            VKR_CHECK_EQ(maximize.height, item.work.height);

            const RectI32 recovered =
                clamp_rect_to_monitor_work({std::numeric_limits<std::int32_t>::min(),
                                            std::numeric_limits<std::int32_t>::max(), 100, 80},
                                           item);
            assert_rect_inside_when_it_fits(recovered, item.work);

            const MonitorDesc* preserved = find_monitor_by_id(decoded.layout, item.stable_id);
            VKR_CHECK(preserved != nullptr);
            if (preserved != nullptr) {
                VKR_CHECK_EQ(preserved->dpi_x, item.dpi_x);
                VKR_CHECK_EQ(preserved->dpi_y, item.dpi_y);
                VKR_CHECK(preserved->rotation == item.rotation);
                VKR_CHECK_EQ(preserved->device_name, item.device_name);
                VKR_CHECK_EQ(preserved->primary, item.primary);
            }
        }
    }

    const DisplayLayout live = fixtures().back().layout;
    PointI32 guest;
    VKR_CHECK(host_to_guest(live, {-6400, 326}, guest));
    VKR_CHECK((guest == PointI32{0, 548}));
    VKR_CHECK(host_to_guest(live, {0, 0}, guest));
    VKR_CHECK((guest == PointI32{6400, 222}));

    const std::vector<Fixture> all = fixtures();
    const auto staggered = std::find_if(
        all.begin(), all.end(), [](const Fixture& fixture) { return fixture.name == "staggered"; });
    VKR_CHECK(staggered != all.end());
    if (staggered != all.end()) {
        // This rectangle is inside the virtual bounds but in the monitor-free corner.
        const RectI32 virtual_hole{200, 100, 80, 60};
        VKR_CHECK(find_monitor_largest_intersection(staggered->layout, virtual_hole) == nullptr);
        const MonitorDesc* nearest = find_monitor_nearest(staggered->layout, virtual_hole);
        VKR_CHECK(nearest != nullptr);
        if (nearest != nullptr) {
            const RectI32 recovered = clamp_rect_to_monitor_work(virtual_hole, *nearest);
            assert_rect_inside_when_it_fits(recovered, nearest->work);
        }
    }
}

void test_m3_nonprimary_input_popup_round_trips() {
    const DisplayLayout live = fixtures().back().layout;
    for (const MonitorDesc& monitor : live.monitors) {
        if (monitor.primary) {
            continue;
        }
        RectI32 parent_host{monitor.work.x + 100, monitor.work.y + 120, 900, 700};
        RectI32 parent_guest;
        VKR_CHECK(host_to_guest(live, parent_host, parent_guest));

        // Captured host client (37,53) maps through the toplevel's guest-root origin and returns
        // byte-exactly on each non-primary monitor.
        const PointI32 input_guest{parent_guest.x + 37, parent_guest.y + 53};
        PointI32 input_host;
        PointI32 input_round_trip;
        VKR_CHECK(guest_to_host(live, input_guest, input_host));
        VKR_CHECK(host_to_guest(live, input_host, input_round_trip));
        VKR_CHECK(input_round_trip == input_guest);
        VKR_CHECK_EQ(input_host.x, parent_host.x + 37);
        VKR_CHECK_EQ(input_host.y, parent_host.y + 53);

        // Popup X-root geometry uses the same inverse transform, independent of owner-local
        // z-order. This catches a regression back to primary-work-origin mapping.
        const RectI32 popup_guest{parent_guest.x + 24, parent_guest.y + 45, 320, 240};
        RectI32 popup_host;
        RectI32 popup_round_trip;
        VKR_CHECK(guest_to_host(live, popup_guest, popup_host));
        VKR_CHECK(host_to_guest(live, popup_host, popup_round_trip));
        VKR_CHECK(popup_round_trip == popup_guest);
        VKR_CHECK_EQ(popup_host.x, parent_host.x + 24);
        VKR_CHECK_EQ(popup_host.y, parent_host.y + 45);
    }
}

void test_selection_tie_breaks() {
    DisplayLayout primary_tie =
        layout("fx-primary-tie",
               {monitor("left", {0, 0, 100, 100}), monitor("right", {200, 0, 100, 100}, true)});
    const MonitorDesc* nearest = find_monitor_nearest(primary_tie, {145, 25, 10, 10});
    VKR_CHECK(nearest != nullptr);
    if (nearest != nullptr) {
        VKR_CHECK_EQ(nearest->stable_id, std::string("right"));
    }
    const MonitorDesc* largest = find_monitor_largest_intersection(primary_tie, {50, 0, 200, 100});
    VKR_CHECK(largest != nullptr);
    if (largest != nullptr) {
        VKR_CHECK_EQ(largest->stable_id, std::string("right"));
    }

    DisplayLayout lexical_tie = layout(
        "fx-lexical-tie", {monitor("primary", {1000, 0, 100, 100}, true),
                           monitor("b", {0, 0, 100, 100}), monitor("a", {200, 0, 100, 100})});
    nearest = find_monitor_nearest(lexical_tie, {145, 25, 10, 10});
    VKR_CHECK(nearest != nullptr);
    if (nearest != nullptr) {
        VKR_CHECK_EQ(nearest->stable_id, std::string("a"));
    }

    nearest = find_monitor_nearest(lexical_tie, {0, 0, 0, 0});
    VKR_CHECK(nearest != nullptr);
    if (nearest != nullptr) {
        VKR_CHECK_EQ(nearest->stable_id, std::string("primary"));
    }
}

void test_m3_placement_policy() {
    for (const Fixture& fixture : fixtures()) {
        const MonitorDesc* primary =
            find_monitor_by_id(fixture.layout, fixture.layout.primary_monitor_id);
        VKR_CHECK(primary != nullptr);
        if (primary == nullptr) {
            continue;
        }

        RectI32 placed;
        const MonitorDesc* selected = nullptr;
        VKR_CHECK(place_guest_window(fixture.layout, {0, 0, 400, 300},
                                     InitialPlacementMode::CenterPrimary, nullptr, placed,
                                     &selected));
        VKR_CHECK(selected == primary);
        RectI32 placed_host;
        VKR_CHECK(guest_to_host(fixture.layout, placed, placed_host));
        assert_rect_inside_when_it_fits(placed_host, primary->work);

        // A saved/app-authored rectangle outside every monitor recovers to one concrete nearest
        // monitor, never merely to the virtual bounding box (which may contain holes).
        VKR_CHECK(place_guest_window(fixture.layout, {-500, -500, 320, 240},
                                     InitialPlacementMode::Preserve, nullptr, placed, &selected));
        VKR_CHECK(selected != nullptr);
        VKR_CHECK(guest_to_host(fixture.layout, placed, placed_host));
        if (selected != nullptr) {
            assert_rect_inside_when_it_fits(placed_host, selected->work);
            VKR_CHECK(find_monitor_largest_intersection(fixture.layout, placed_host) != nullptr);
        }

        // A transient is centered over its owner and constrained to the owner's monitor even when
        // another monitor would win from the dialog's stale requested coordinates.
        const MonitorDesc& owner_monitor = fixture.layout.monitors.back();
        RectI32 owner_guest;
        const RectI32 owner_host{owner_monitor.work.x + owner_monitor.work.width / 4,
                                 owner_monitor.work.y + owner_monitor.work.height / 4, 600, 500};
        VKR_CHECK(host_to_guest(fixture.layout, owner_host, owner_guest));
        VKR_CHECK(place_guest_window(fixture.layout, {0, 0, 300, 200},
                                     InitialPlacementMode::CenterOwner, &owner_guest, placed,
                                     &selected));
        VKR_CHECK(selected != nullptr);
        if (selected != nullptr) {
            VKR_CHECK_EQ(selected->stable_id, owner_monitor.stable_id);
            VKR_CHECK(guest_to_host(fixture.layout, placed, placed_host));
            assert_rect_inside_when_it_fits(placed_host, owner_monitor.work);
        }
    }

    const DisplayLayout live = fixtures().back().layout;
    RectI32 placed;
    const MonitorDesc* selected = nullptr;
    // Bigger-than-monitor policy: a full-root client keeps the virtual origin instead of being
    // pinned to the small portrait primary and hanging thousands of pixels beyond the desktop.
    VKR_CHECK(place_guest_window(live,
                                 {0, 0, live.virtual_bounds.width, live.virtual_bounds.height},
                                 InitialPlacementMode::CenterPrimary, nullptr, placed, &selected));
    VKR_CHECK((placed == RectI32{0, 0, 7600, 2160}));
    VKR_CHECK(selected != nullptr && selected->stable_id == "primary-portrait");

    const std::vector<Fixture> all = fixtures();
    const auto staggered = std::find_if(
        all.begin(), all.end(), [](const Fixture& fixture) { return fixture.name == "staggered"; });
    VKR_CHECK(staggered != all.end());
    if (staggered != all.end()) {
        // (200,100) is inside the bounding box but in the upper-left hole.
        VKR_CHECK(place_guest_window(staggered->layout, {200, 100, 80, 60},
                                     InitialPlacementMode::Preserve, nullptr, placed, &selected));
        RectI32 placed_host;
        VKR_CHECK(guest_to_host(staggered->layout, placed, placed_host));
        VKR_CHECK(find_monitor_largest_intersection(staggered->layout, placed_host) != nullptr);
        VKR_CHECK(!(placed.x == 200 && placed.y == 100));
    }

    const auto dual = std::find_if(
        all.begin(), all.end(), [](const Fixture& f) { return f.name == "dual_fhd_primary_left"; });
    VKR_CHECK(dual != all.end());
    if (dual != all.end()) {
        // Preserve: a user-authored adjacent-monitor straddler is already reachable. Selection
        // remains deterministic (the right monitor has the larger top-row intersection), but the
        // client rectangle is byte-identical rather than re-homed into that monitor.
        const RectI32 straddler{1800, 100, 400, 300};
        VKR_CHECK(place_guest_window(dual->layout, straddler, InitialPlacementMode::Preserve,
                                     nullptr, placed, &selected));
        VKR_CHECK(placed == straddler);
        VKR_CHECK(selected != nullptr && selected->stable_id == "b");

        // One physical pixel of reachable top row is deliberately sufficient.
        const RectI32 barely_reachable{3839, 100, 400, 300};
        VKR_CHECK(place_guest_window(dual->layout, barely_reachable, InitialPlacementMode::Preserve,
                                     nullptr, placed, &selected));
        VKR_CHECK(placed == barely_reachable);
        VKR_CHECK(selected != nullptr && selected->stable_id == "b");
    }

    const auto taskbars = std::find_if(
        all.begin(), all.end(), [](const Fixture& f) { return f.name == "per_monitor_taskbars"; });
    VKR_CHECK(taskbars != all.end());
    if (taskbars != all.end()) {
        // The body may overlap a reserved band when the client's top row remains reachable.
        const RectI32 body_over_taskbar{0, 100, 300, 240};
        VKR_CHECK(place_guest_window(taskbars->layout, body_over_taskbar,
                                     InitialPlacementMode::Preserve, nullptr, placed, &selected));
        VKR_CHECK(placed == body_over_taskbar);
        VKR_CHECK(selected != nullptr && selected->stable_id == "left-taskbar");

        // A top row wholly inside the top taskbar is unreachable and recovers to work y=40.
        const RectI32 title_in_taskbar{2020, 0, 400, 300};
        VKR_CHECK(place_guest_window(taskbars->layout, title_in_taskbar,
                                     InitialPlacementMode::Preserve, nullptr, placed, &selected));
        VKR_CHECK_EQ(placed.x, title_in_taskbar.x);
        VKR_CHECK_EQ(placed.y, 40);
        VKR_CHECK(selected != nullptr && selected->stable_id == "top-taskbar");
    }
}

void test_mirror_dedup_and_canonical_order() {
    const MonitorDesc secondary = monitor("z-target", {0, 0, 1920, 1080});
    const MonitorDesc primary = monitor("a-target", {0, 0, 1920, 1080}, true);
    DisplayLayout invalid = layout("fx-mirror", {secondary, primary});
    VKR_CHECK(!validate_display_layout(invalid).ok);

    std::vector<MonitorDesc> deduped = deduplicate_mirrored_monitors(invalid.monitors);
    VKR_CHECK_EQ(deduped.size(), static_cast<std::size_t>(1));
    VKR_CHECK_EQ(deduped.front().stable_id, std::string("a-target"));
    DisplayLayout valid = layout("fx-mirror-deduped", std::move(deduped));
    VKR_CHECK(validate_display_layout(valid).ok);

    DisplayLayout unordered = layout(
        "fx-order", {monitor("z", {0, 0, 100, 100}, true), monitor("a", {100, 0, 100, 100})});
    const json::Value encoded = display_layout_to_json(unordered);
    const json::Value* array = encoded.find("monitors");
    VKR_CHECK(array != nullptr && array->is_array());
    if (array != nullptr && array->is_array()) {
        VKR_CHECK_EQ(array->as_array()[0].find("stable_id")->as_string(), std::string("a"));
        VKR_CHECK_EQ(array->as_array()[1].find("stable_id")->as_string(), std::string("z"));
    }
}

void test_strict_decode_states_and_guest_state() {
    json::Value parent = json::Value::make_object();
    VKR_CHECK(decode_display_layout_field(parent, "display_layout").status ==
              LayoutDecodeStatus::Absent);
    parent.set("display_layout", json::Value(7));
    VKR_CHECK(decode_display_layout_field(parent, "display_layout").status ==
              LayoutDecodeStatus::Malformed);

    DisplayLayout sample = fixtures().front().layout;
    json::Value future = display_layout_to_json(sample);
    future.set("schema_version", json::Value(2));
    parent.set("display_layout", std::move(future));
    VKR_CHECK(decode_display_layout_field(parent, "display_layout").status ==
              LayoutDecodeStatus::UnsupportedSchema);

    json::Value with_unknown = display_layout_to_json(sample);
    with_unknown.set("future_addition", json::Value("ignored"));
    parent.set("display_layout", std::move(with_unknown));
    VKR_CHECK(decode_display_layout_field(parent, "display_layout").status ==
              LayoutDecodeStatus::Valid);

    json::Value fractional = display_layout_to_json(sample);
    fractional.find("virtual_bounds")->as_object(); // const lookup proves the field exists
    json::Value* mutable_bounds = nullptr;
    for (auto& pair : fractional.as_object()) {
        if (pair.first == "virtual_bounds") {
            mutable_bounds = &pair.second;
        }
    }
    VKR_CHECK(mutable_bounds != nullptr);
    if (mutable_bounds != nullptr) {
        mutable_bounds->set("width", json::Value(1920.5));
    }
    parent.set("display_layout", std::move(fractional));
    VKR_CHECK(decode_display_layout_field(parent, "display_layout").status ==
              LayoutDecodeStatus::Malformed);

    GuestDisplayState guest;
    guest.snapshot_id = "sup/display-1";
    guest.actual_root_width = 7600;
    guest.actual_root_height = 2160;
    guest.output_model = OutputModel::PerMonitorOutputs;
    VKR_CHECK(validate_guest_display_state(guest).ok);
    json::Value guest_parent = json::Value::make_object();
    guest_parent.set("guest", guest_display_state_to_json(guest));
    const GuestDisplayStateDecodeResult guest_back =
        decode_guest_display_state_field(guest_parent, "guest");
    VKR_CHECK(guest_back.status == LayoutDecodeStatus::Valid);
    VKR_CHECK_EQ(guest_back.state.snapshot_id, guest.snapshot_id);
    VKR_CHECK_EQ(guest_back.state.actual_root_width, guest.actual_root_width);
    VKR_CHECK(guest_back.state.output_model == guest.output_model);

    guest.snapshot_id = sample.snapshot_id;
    guest.actual_root_width = static_cast<std::uint32_t>(sample.virtual_bounds.width);
    guest.actual_root_height = static_cast<std::uint32_t>(sample.virtual_bounds.height);
    guest.output_model = OutputModel::SingleCanvas;
    VKR_CHECK(validate_guest_display_state_against_layout(guest, sample).ok);
    guest.snapshot_id = "wrong-snapshot";
    VKR_CHECK(!validate_guest_display_state_against_layout(guest, sample).ok);
    guest.snapshot_id = sample.snapshot_id;
    ++guest.actual_root_width;
    VKR_CHECK(!validate_guest_display_state_against_layout(guest, sample).ok);
}

void test_named_caps_and_overflow() {
    DisplayLayout axis = layout("bad-axis", {monitor("a", {0, 0, 16385, 1}, true)});
    const ValidationResult axis_result = validate_display_layout(axis);
    VKR_CHECK(!axis_result.ok);
    VKR_CHECK(axis_result.reason.find("16384") != std::string::npos);

    DisplayLayout area = layout("bad-area", {monitor("a", {0, 0, 16384, 4097}, true)});
    const ValidationResult area_result = validate_display_layout(area);
    VKR_CHECK(!area_result.ok);
    VKR_CHECK(area_result.reason.find("67108864") != std::string::npos);

    std::vector<MonitorDesc> too_many;
    for (std::size_t i = 0; i < kMaxDisplayMonitors + 1; ++i) {
        too_many.push_back(
            monitor("m" + std::to_string(i), {static_cast<std::int32_t>(i), 0, 1, 1}, i == 0));
    }
    DisplayLayout count = layout("bad-count", std::move(too_many));
    const ValidationResult count_result = validate_display_layout(count);
    VKR_CHECK(!count_result.ok);
    VKR_CHECK(count_result.reason.find("32") != std::string::npos);

    DisplayLayout narrowing;
    narrowing.snapshot_id = "bad-narrowing";
    narrowing.primary_monitor_id = "a";
    narrowing.virtual_bounds = {std::numeric_limits<std::int32_t>::max() - 10, 0, 20, 10};
    narrowing.monitors = {monitor("a", narrowing.virtual_bounds, true)};
    const ValidationResult narrowing_result = validate_display_layout(narrowing);
    VKR_CHECK(!narrowing_result.ok);
    VKR_CHECK(narrowing_result.reason.find("signed-32") != std::string::npos);

    DisplayLayout transform = fixtures().front().layout;
    PointI32 out;
    transform.virtual_bounds.x = std::numeric_limits<std::int32_t>::min();
    VKR_CHECK(!host_to_guest(transform, {std::numeric_limits<std::int32_t>::max(), 0}, out));
}

} // namespace

int main() {
    test_full_fixture_matrix();
    test_m3_nonprimary_input_popup_round_trips();
    test_selection_tie_breaks();
    test_m3_placement_policy();
    test_mirror_dedup_and_canonical_order();
    test_strict_decode_states_and_guest_state();
    test_named_caps_and_overflow();
    return vkr::test::finish("unit_display_layout");
}
