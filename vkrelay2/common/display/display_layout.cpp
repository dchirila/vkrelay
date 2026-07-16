#include "common/display/display_layout.hpp"

#include <algorithm>
#include <limits>
#include <set>
#include <tuple>
#include <utility>

namespace vkr::display {
namespace {

struct Edges64 {
    std::int64_t left = 0;
    std::int64_t top = 0;
    std::int64_t right = 0;
    std::int64_t bottom = 0;
};

bool valid_rotation(DisplayRotation rotation) {
    return rotation == DisplayRotation::Identity || rotation == DisplayRotation::Rotate90 ||
           rotation == DisplayRotation::Rotate180 || rotation == DisplayRotation::Rotate270;
}

bool valid_output_model(OutputModel model) {
    return model == OutputModel::SingleCanvas || model == OutputModel::PerMonitorOutputs;
}

bool edges(const RectI32& r, Edges64& out) {
    if (r.width <= 0 || r.height <= 0) {
        return false;
    }
    out.left = r.x;
    out.top = r.y;
    out.right = out.left + static_cast<std::int64_t>(r.width);
    out.bottom = out.top + static_cast<std::int64_t>(r.height);
    return out.right <= std::numeric_limits<std::int32_t>::max() &&
           out.bottom <= std::numeric_limits<std::int32_t>::max();
}

bool contains_rect(const RectI32& outer, const RectI32& inner) {
    Edges64 o;
    Edges64 i;
    return edges(outer, o) && edges(inner, i) && i.left >= o.left && i.top >= o.top &&
           i.right <= o.right && i.bottom <= o.bottom;
}

bool contains_point(const RectI32& rect, PointI32 point) {
    Edges64 e;
    return edges(rect, e) && point.x >= e.left && point.x < e.right && point.y >= e.top &&
           point.y < e.bottom;
}

std::uint64_t intersection_area(const RectI32& a, const RectI32& b) {
    Edges64 ae;
    Edges64 be;
    if (!edges(a, ae) || !edges(b, be)) {
        return 0;
    }
    const std::int64_t w = std::min(ae.right, be.right) - std::max(ae.left, be.left);
    const std::int64_t h = std::min(ae.bottom, be.bottom) - std::max(ae.top, be.top);
    if (w <= 0 || h <= 0) {
        return 0;
    }
    return static_cast<std::uint64_t>(w) * static_cast<std::uint64_t>(h);
}

// Manhattan rectangle-gap is intentional: unlike squared Euclidean distance it cannot overflow
// for two signed-32 coordinate ranges, and it has deterministic, sufficient nearest-monitor
// behavior for off-screen recovery. Intersecting/touching rectangles have distance zero.
std::uint64_t manhattan_gap(const RectI32& a, const RectI32& b) {
    Edges64 ae;
    Edges64 be;
    if (!edges(a, ae) || !edges(b, be)) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    const std::int64_t dx =
        ae.right < be.left ? be.left - ae.right : (be.right < ae.left ? ae.left - be.right : 0);
    const std::int64_t dy =
        ae.bottom < be.top ? be.top - ae.bottom : (be.bottom < ae.top ? ae.top - be.bottom : 0);
    return static_cast<std::uint64_t>(dx) + static_cast<std::uint64_t>(dy);
}

bool preferred(const MonitorDesc& candidate, const MonitorDesc& current) {
    if (candidate.primary != current.primary) {
        return candidate.primary;
    }
    return candidate.stable_id < current.stable_id;
}

bool bounded_string(const std::string& value, std::size_t cap, bool allow_empty) {
    return (allow_empty || !value.empty()) && value.size() <= cap &&
           value.find('\0') == std::string::npos;
}

const json::Value* required(const json::Value& object, const char* name, json::Type type,
                            std::string& reason) {
    const json::Value* value = object.find(name);
    if (value == nullptr || value->type() != type) {
        reason =
            std::string("display layout field '") + name + "' is missing or has the wrong type";
        return nullptr;
    }
    return value;
}

bool exact_i32(const json::Value& object, const char* name, std::int32_t& out,
               std::string& reason) {
    const json::Value* value = object.find(name);
    if (value == nullptr || !value->is_integer()) {
        reason = std::string("display layout field '") + name + "' is not an integer";
        return false;
    }
    const std::int64_t n = value->as_int();
    if (n < std::numeric_limits<std::int32_t>::min() ||
        n > std::numeric_limits<std::int32_t>::max()) {
        reason = std::string("display layout field '") + name + "' exceeds signed-32 range";
        return false;
    }
    out = static_cast<std::int32_t>(n);
    return true;
}

bool exact_u32(const json::Value& object, const char* name, std::uint32_t& out,
               std::string& reason) {
    const json::Value* value = object.find(name);
    if (value == nullptr || !value->is_integer()) {
        reason = std::string("display layout field '") + name + "' is not an integer";
        return false;
    }
    const std::int64_t n = value->as_int();
    if (n < 0 || static_cast<std::uint64_t>(n) > std::numeric_limits<std::uint32_t>::max()) {
        reason = std::string("display layout field '") + name + "' exceeds unsigned-32 range";
        return false;
    }
    out = static_cast<std::uint32_t>(n);
    return true;
}

bool decode_rect(const json::Value& value, RectI32& rect, std::string& reason) {
    if (!value.is_object()) {
        reason = "display layout rectangle is not an object";
        return false;
    }
    return exact_i32(value, "x", rect.x, reason) && exact_i32(value, "y", rect.y, reason) &&
           exact_i32(value, "width", rect.width, reason) &&
           exact_i32(value, "height", rect.height, reason);
}

json::Value rect_to_json(const RectI32& rect) {
    json::Value value = json::Value::make_object();
    value.set("x", json::Value(rect.x));
    value.set("y", json::Value(rect.y));
    value.set("width", json::Value(rect.width));
    value.set("height", json::Value(rect.height));
    return value;
}

const char* rotation_name(DisplayRotation rotation) {
    switch (rotation) {
    case DisplayRotation::Identity:
        return "identity";
    case DisplayRotation::Rotate90:
        return "rotate90";
    case DisplayRotation::Rotate180:
        return "rotate180";
    case DisplayRotation::Rotate270:
        return "rotate270";
    }
    return "invalid";
}

bool decode_rotation(const std::string& name, DisplayRotation& rotation) {
    if (name == "identity") {
        rotation = DisplayRotation::Identity;
    } else if (name == "rotate90") {
        rotation = DisplayRotation::Rotate90;
    } else if (name == "rotate180") {
        rotation = DisplayRotation::Rotate180;
    } else if (name == "rotate270") {
        rotation = DisplayRotation::Rotate270;
    } else {
        return false;
    }
    return true;
}

const char* output_model_name(OutputModel model) {
    return model == OutputModel::PerMonitorOutputs ? "per_monitor_outputs" : "single_canvas";
}

bool decode_output_model(const std::string& name, OutputModel& model) {
    if (name == "single_canvas") {
        model = OutputModel::SingleCanvas;
    } else if (name == "per_monitor_outputs") {
        model = OutputModel::PerMonitorOutputs;
    } else {
        return false;
    }
    return true;
}

} // namespace

bool operator==(const PointI32& a, const PointI32& b) {
    return a.x == b.x && a.y == b.y;
}

bool operator==(const RectI32& a, const RectI32& b) {
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}

ValidationResult validate_display_layout(const DisplayLayout& layout) {
    if (layout.schema_version != kDisplayLayoutSchemaVersion) {
        return {false, "unsupported display layout schema_version (supported value: 1)"};
    }
    if (!bounded_string(layout.snapshot_id, kMaxSnapshotIdBytes, false)) {
        return {false, "display layout snapshot_id is empty or exceeds 128 bytes"};
    }
    if (layout.monitors.empty()) {
        return {false, "display layout contains no monitors"};
    }
    if (layout.monitors.size() > kMaxDisplayMonitors) {
        return {false, "display layout monitor cap exceeded (maximum: 32)"};
    }
    Edges64 virtual_edges;
    if (!edges(layout.virtual_bounds, virtual_edges)) {
        return {false, "display layout virtual bounds are non-positive or violate the signed-32 "
                       "coordinate cap (-2147483648..2147483647)"};
    }
    if (layout.virtual_bounds.width > kMaxVirtualDisplayAxis ||
        layout.virtual_bounds.height > kMaxVirtualDisplayAxis) {
        return {false, "display layout axis cap exceeded (maximum: 16384 physical pixels)"};
    }
    const std::uint64_t area = static_cast<std::uint64_t>(layout.virtual_bounds.width) *
                               static_cast<std::uint64_t>(layout.virtual_bounds.height);
    if (area > kMaxVirtualDisplayPixels) {
        return {false, "display layout area cap exceeded (maximum: 67108864 physical pixels)"};
    }
    if (!bounded_string(layout.primary_monitor_id, kMaxMonitorIdBytes, false)) {
        return {false, "display layout primary_monitor_id is empty or exceeds 256 bytes"};
    }

    std::set<std::string> ids;
    std::set<std::tuple<std::int32_t, std::int32_t, std::int32_t, std::int32_t>> bounds_seen;
    std::size_t primary_count = 0;
    std::int64_t union_left = std::numeric_limits<std::int64_t>::max();
    std::int64_t union_top = std::numeric_limits<std::int64_t>::max();
    std::int64_t union_right = std::numeric_limits<std::int64_t>::min();
    std::int64_t union_bottom = std::numeric_limits<std::int64_t>::min();
    for (const MonitorDesc& monitor : layout.monitors) {
        if (!bounded_string(monitor.stable_id, kMaxMonitorIdBytes, false)) {
            return {false, "monitor stable_id is empty or exceeds 256 bytes"};
        }
        if (!bounded_string(monitor.device_name, kMaxDeviceNameBytes, true)) {
            return {false, "monitor device_name exceeds 512 bytes"};
        }
        if (!ids.insert(monitor.stable_id).second) {
            return {false,
                    "display layout contains duplicate monitor stable_id: " + monitor.stable_id};
        }
        if (!bounds_seen
                 .insert({monitor.bounds.x, monitor.bounds.y, monitor.bounds.width,
                          monitor.bounds.height})
                 .second) {
            return {false,
                    "display layout contains exact-bounds mirrored monitors; deduplicate first"};
        }
        Edges64 monitor_edges;
        Edges64 work_edges;
        if (!edges(monitor.bounds, monitor_edges)) {
            return {false,
                    "monitor bounds are non-positive or violate the signed-32 coordinate cap "
                    "(-2147483648..2147483647): " +
                        monitor.stable_id};
        }
        if (!edges(monitor.work, work_edges) || !contains_rect(monitor.bounds, monitor.work)) {
            return {false,
                    "monitor work area is non-positive or not contained in monitor bounds: " +
                        monitor.stable_id};
        }
        if (monitor.dpi_x == 0 || monitor.dpi_y == 0) {
            return {false, "monitor DPI metadata must be positive: " + monitor.stable_id};
        }
        if (!valid_rotation(monitor.rotation)) {
            return {false, "monitor rotation is invalid: " + monitor.stable_id};
        }
        if (monitor.primary) {
            ++primary_count;
            if (monitor.stable_id != layout.primary_monitor_id) {
                return {false, "primary monitor flag and primary_monitor_id disagree"};
            }
        }
        union_left = std::min(union_left, monitor_edges.left);
        union_top = std::min(union_top, monitor_edges.top);
        union_right = std::max(union_right, monitor_edges.right);
        union_bottom = std::max(union_bottom, monitor_edges.bottom);
    }
    if (primary_count != 1 || ids.count(layout.primary_monitor_id) != 1) {
        return {false, "display layout must contain exactly one primary monitor"};
    }
    const std::int64_t union_width = union_right - union_left;
    const std::int64_t union_height = union_bottom - union_top;
    if (union_width <= 0 || union_width > std::numeric_limits<std::int32_t>::max() ||
        union_height <= 0 || union_height > std::numeric_limits<std::int32_t>::max()) {
        return {false, "display layout monitor union violates the signed-32 dimension cap "
                       "(maximum: 2147483647)"};
    }
    const RectI32 computed{
        static_cast<std::int32_t>(union_left), static_cast<std::int32_t>(union_top),
        static_cast<std::int32_t>(union_width), static_cast<std::int32_t>(union_height)};
    if (!(computed == layout.virtual_bounds)) {
        return {false, "display layout virtual bounds do not equal the exact monitor union"};
    }
    return {true, {}};
}

ValidationResult validate_guest_display_state(const GuestDisplayState& state) {
    if (!bounded_string(state.snapshot_id, kMaxSnapshotIdBytes, false)) {
        return {false, "guest display snapshot_id is empty or exceeds 128 bytes"};
    }
    if (state.actual_root_width == 0 || state.actual_root_height == 0) {
        return {false, "guest display root dimensions must be positive"};
    }
    if (state.actual_root_width > static_cast<std::uint32_t>(kMaxVirtualDisplayAxis) ||
        state.actual_root_height > static_cast<std::uint32_t>(kMaxVirtualDisplayAxis)) {
        return {false, "guest display axis cap exceeded (maximum: 16384 physical pixels)"};
    }
    if (static_cast<std::uint64_t>(state.actual_root_width) * state.actual_root_height >
        kMaxVirtualDisplayPixels) {
        return {false, "guest display area cap exceeded (maximum: 67108864 physical pixels)"};
    }
    if (!valid_output_model(state.output_model)) {
        return {false, "guest display output_model is invalid"};
    }
    return {true, {}};
}

ValidationResult validate_guest_display_state_against_layout(const GuestDisplayState& state,
                                                             const DisplayLayout& layout) {
    const ValidationResult layout_validation = validate_display_layout(layout);
    if (!layout_validation.ok) {
        return {false, "pinned display layout is invalid: " + layout_validation.reason};
    }
    const ValidationResult state_validation = validate_guest_display_state(state);
    if (!state_validation.ok) {
        return state_validation;
    }
    if (state.snapshot_id != layout.snapshot_id) {
        return {false, "sidecar display snapshot ID does not match the worker session snapshot"};
    }
    if (state.actual_root_width != static_cast<std::uint32_t>(layout.virtual_bounds.width) ||
        state.actual_root_height != static_cast<std::uint32_t>(layout.virtual_bounds.height)) {
        return {false, "observed guest root dimensions do not match the pinned virtual bounds"};
    }
    return {true, {}};
}

json::Value display_layout_to_json(const DisplayLayout& layout) {
    json::Value value = json::Value::make_object();
    value.set("schema_version", json::Value(static_cast<int>(layout.schema_version)));
    value.set("snapshot_id", json::Value(layout.snapshot_id));
    value.set("virtual_bounds", rect_to_json(layout.virtual_bounds));
    value.set("primary_monitor_id", json::Value(layout.primary_monitor_id));
    json::Value monitors = json::Value::make_array();
    std::vector<MonitorDesc> ordered = layout.monitors;
    std::sort(ordered.begin(), ordered.end(),
              [](const MonitorDesc& a, const MonitorDesc& b) { return a.stable_id < b.stable_id; });
    for (const MonitorDesc& monitor : ordered) {
        json::Value item = json::Value::make_object();
        item.set("stable_id", json::Value(monitor.stable_id));
        item.set("device_name", json::Value(monitor.device_name));
        item.set("bounds", rect_to_json(monitor.bounds));
        item.set("work", rect_to_json(monitor.work));
        item.set("dpi_x", json::Value(static_cast<long long>(monitor.dpi_x)));
        item.set("dpi_y", json::Value(static_cast<long long>(monitor.dpi_y)));
        item.set("rotation", json::Value(rotation_name(monitor.rotation)));
        item.set("primary", json::Value(monitor.primary));
        monitors.as_array().push_back(std::move(item));
    }
    value.set("monitors", std::move(monitors));
    return value;
}

DisplayLayoutDecodeResult decode_display_layout_field(const json::Value& parent,
                                                      const std::string& field_name) {
    DisplayLayoutDecodeResult result;
    const json::Value* value = parent.find(field_name);
    if (value == nullptr) {
        return result;
    }
    result.status = LayoutDecodeStatus::Malformed;
    if (!value->is_object()) {
        result.reason = "display layout field is present but not an object";
        return result;
    }
    const json::Value* schema = value->find("schema_version");
    if (schema == nullptr || !schema->is_integer()) {
        result.reason = "display layout schema_version is missing or not an integer";
        return result;
    }
    const std::int64_t schema_number = schema->as_int();
    if (schema_number != kDisplayLayoutSchemaVersion) {
        result.status = LayoutDecodeStatus::UnsupportedSchema;
        result.reason = "unsupported display layout schema_version";
        return result;
    }
    result.layout.schema_version = static_cast<std::uint32_t>(schema_number);
    std::string reason;
    const json::Value* snapshot = required(*value, "snapshot_id", json::Type::String, reason);
    const json::Value* virtual_bounds =
        required(*value, "virtual_bounds", json::Type::Object, reason);
    const json::Value* primary = required(*value, "primary_monitor_id", json::Type::String, reason);
    const json::Value* monitors = required(*value, "monitors", json::Type::Array, reason);
    if (snapshot == nullptr || virtual_bounds == nullptr || primary == nullptr ||
        monitors == nullptr) {
        result.reason = reason;
        return result;
    }
    result.layout.snapshot_id = snapshot->as_string();
    result.layout.primary_monitor_id = primary->as_string();
    if (!decode_rect(*virtual_bounds, result.layout.virtual_bounds, result.reason)) {
        return result;
    }
    if (monitors->as_array().size() > kMaxDisplayMonitors) {
        result.reason = "display layout monitor cap exceeded (maximum: 32)";
        return result;
    }
    for (const json::Value& item : monitors->as_array()) {
        if (!item.is_object()) {
            result.reason = "display layout monitor entry is not an object";
            return result;
        }
        MonitorDesc monitor;
        const json::Value* id = required(item, "stable_id", json::Type::String, result.reason);
        const json::Value* device =
            required(item, "device_name", json::Type::String, result.reason);
        const json::Value* bounds = required(item, "bounds", json::Type::Object, result.reason);
        const json::Value* work = required(item, "work", json::Type::Object, result.reason);
        const json::Value* rotation = required(item, "rotation", json::Type::String, result.reason);
        const json::Value* primary_flag =
            required(item, "primary", json::Type::Bool, result.reason);
        if (id == nullptr || device == nullptr || bounds == nullptr || work == nullptr ||
            rotation == nullptr || primary_flag == nullptr) {
            return result;
        }
        monitor.stable_id = id->as_string();
        monitor.device_name = device->as_string();
        monitor.primary = primary_flag->as_bool();
        if (!decode_rect(*bounds, monitor.bounds, result.reason) ||
            !decode_rect(*work, monitor.work, result.reason) ||
            !exact_u32(item, "dpi_x", monitor.dpi_x, result.reason) ||
            !exact_u32(item, "dpi_y", monitor.dpi_y, result.reason) ||
            !decode_rotation(rotation->as_string(), monitor.rotation)) {
            if (result.reason.empty()) {
                result.reason = "display layout monitor rotation is invalid";
            }
            return result;
        }
        result.layout.monitors.push_back(std::move(monitor));
    }
    const ValidationResult validation = validate_display_layout(result.layout);
    if (!validation.ok) {
        result.reason = validation.reason;
        return result;
    }
    result.status = LayoutDecodeStatus::Valid;
    result.reason.clear();
    return result;
}

json::Value guest_display_state_to_json(const GuestDisplayState& state) {
    json::Value value = json::Value::make_object();
    value.set("snapshot_id", json::Value(state.snapshot_id));
    value.set("actual_root_width", json::Value(static_cast<long long>(state.actual_root_width)));
    value.set("actual_root_height", json::Value(static_cast<long long>(state.actual_root_height)));
    value.set("output_model", json::Value(output_model_name(state.output_model)));
    return value;
}

GuestDisplayStateDecodeResult decode_guest_display_state_field(const json::Value& parent,
                                                               const std::string& field_name) {
    GuestDisplayStateDecodeResult result;
    const json::Value* value = parent.find(field_name);
    if (value == nullptr) {
        return result;
    }
    result.status = LayoutDecodeStatus::Malformed;
    if (!value->is_object()) {
        result.reason = "guest display state is present but not an object";
        return result;
    }
    const json::Value* snapshot =
        required(*value, "snapshot_id", json::Type::String, result.reason);
    const json::Value* model = required(*value, "output_model", json::Type::String, result.reason);
    if (snapshot == nullptr || model == nullptr) {
        return result;
    }
    result.state.snapshot_id = snapshot->as_string();
    if (!exact_u32(*value, "actual_root_width", result.state.actual_root_width, result.reason) ||
        !exact_u32(*value, "actual_root_height", result.state.actual_root_height, result.reason) ||
        !decode_output_model(model->as_string(), result.state.output_model)) {
        if (result.reason.empty()) {
            result.reason = "guest display output_model is invalid";
        }
        return result;
    }
    const ValidationResult validation = validate_guest_display_state(result.state);
    if (!validation.ok) {
        result.reason = validation.reason;
        return result;
    }
    result.status = LayoutDecodeStatus::Valid;
    result.reason.clear();
    return result;
}

bool host_to_guest(const DisplayLayout& layout, PointI32 host, PointI32& guest) {
    const std::int64_t x = static_cast<std::int64_t>(host.x) - layout.virtual_bounds.x;
    const std::int64_t y = static_cast<std::int64_t>(host.y) - layout.virtual_bounds.y;
    if (x < std::numeric_limits<std::int32_t>::min() ||
        x > std::numeric_limits<std::int32_t>::max() ||
        y < std::numeric_limits<std::int32_t>::min() ||
        y > std::numeric_limits<std::int32_t>::max()) {
        return false;
    }
    guest = {static_cast<std::int32_t>(x), static_cast<std::int32_t>(y)};
    return true;
}

bool guest_to_host(const DisplayLayout& layout, PointI32 guest, PointI32& host) {
    const std::int64_t x = static_cast<std::int64_t>(guest.x) + layout.virtual_bounds.x;
    const std::int64_t y = static_cast<std::int64_t>(guest.y) + layout.virtual_bounds.y;
    if (x < std::numeric_limits<std::int32_t>::min() ||
        x > std::numeric_limits<std::int32_t>::max() ||
        y < std::numeric_limits<std::int32_t>::min() ||
        y > std::numeric_limits<std::int32_t>::max()) {
        return false;
    }
    host = {static_cast<std::int32_t>(x), static_cast<std::int32_t>(y)};
    return true;
}

bool host_to_guest(const DisplayLayout& layout, const RectI32& host, RectI32& guest) {
    PointI32 origin;
    if (!host_to_guest(layout, PointI32{host.x, host.y}, origin)) {
        return false;
    }
    guest = {origin.x, origin.y, host.width, host.height};
    Edges64 unused;
    return edges(guest, unused);
}

bool guest_to_host(const DisplayLayout& layout, const RectI32& guest, RectI32& host) {
    PointI32 origin;
    if (!guest_to_host(layout, PointI32{guest.x, guest.y}, origin)) {
        return false;
    }
    host = {origin.x, origin.y, guest.width, guest.height};
    Edges64 unused;
    return edges(host, unused);
}

const MonitorDesc* find_monitor_by_id(const DisplayLayout& layout, const std::string& stable_id) {
    for (const MonitorDesc& monitor : layout.monitors) {
        if (monitor.stable_id == stable_id) {
            return &monitor;
        }
    }
    return nullptr;
}

const MonitorDesc* find_monitor_containing(const DisplayLayout& layout, PointI32 point) {
    const MonitorDesc* best = nullptr;
    for (const MonitorDesc& monitor : layout.monitors) {
        if (contains_point(monitor.bounds, point) &&
            (best == nullptr || preferred(monitor, *best))) {
            best = &monitor;
        }
    }
    return best;
}

const MonitorDesc* find_monitor_largest_intersection(const DisplayLayout& layout,
                                                     const RectI32& rect) {
    const MonitorDesc* best = nullptr;
    std::uint64_t best_area = 0;
    for (const MonitorDesc& monitor : layout.monitors) {
        const std::uint64_t area = intersection_area(monitor.bounds, rect);
        if (area > best_area ||
            (area == best_area && area > 0 && (best == nullptr || preferred(monitor, *best)))) {
            best = &monitor;
            best_area = area;
        }
    }
    return best_area == 0 ? nullptr : best;
}

static const MonitorDesc* find_monitor_work_largest_intersection(const DisplayLayout& layout,
                                                                 const RectI32& rect) {
    const MonitorDesc* best = nullptr;
    std::uint64_t best_area = 0;
    for (const MonitorDesc& monitor : layout.monitors) {
        const std::uint64_t area = intersection_area(monitor.work, rect);
        if (area > best_area ||
            (area == best_area && area > 0 && (best == nullptr || preferred(monitor, *best)))) {
            best = &monitor;
            best_area = area;
        }
    }
    return best_area == 0 ? nullptr : best;
}

const MonitorDesc* find_monitor_nearest(const DisplayLayout& layout, const RectI32& rect) {
    // A degenerate query has a saturated gap to every monitor and deliberately recovers to the
    // normal deterministic tie-break (primary, then stable ID), rather than returning nullptr.
    const MonitorDesc* best = nullptr;
    std::uint64_t best_gap = std::numeric_limits<std::uint64_t>::max();
    for (const MonitorDesc& monitor : layout.monitors) {
        const std::uint64_t gap = manhattan_gap(monitor.bounds, rect);
        if (gap < best_gap || (gap == best_gap && (best == nullptr || preferred(monitor, *best)))) {
            best = &monitor;
            best_gap = gap;
        }
    }
    return best;
}

RectI32 clamp_rect_to_monitor_work(const RectI32& rect, const MonitorDesc& monitor) {
    Edges64 work;
    if (!edges(monitor.work, work) || rect.width <= 0 || rect.height <= 0) {
        return rect;
    }
    const auto clamp_axis = [](std::int32_t origin, std::int32_t size, std::int64_t low,
                               std::int64_t high) {
        const std::int64_t span = high - low;
        if (size >= span) {
            return static_cast<std::int32_t>(low);
        }
        const std::int64_t max_origin = high - size;
        return static_cast<std::int32_t>(
            std::max(low, std::min(static_cast<std::int64_t>(origin), max_origin)));
    };
    return {clamp_axis(rect.x, rect.width, work.left, work.right),
            clamp_axis(rect.y, rect.height, work.top, work.bottom), rect.width, rect.height};
}

RectI32 clamp_rect_to_monitor_work_or_virtual(const RectI32& rect, const MonitorDesc& monitor,
                                              const RectI32& virtual_bounds) {
    Edges64 work;
    Edges64 desktop;
    if (!edges(monitor.work, work) || !edges(virtual_bounds, desktop) || rect.width <= 0 ||
        rect.height <= 0) {
        return rect;
    }
    const auto clamp_axis = [](std::int32_t origin, std::int32_t size, std::int64_t work_low,
                               std::int64_t work_high, std::int64_t desktop_low,
                               std::int64_t desktop_high) {
        std::int64_t low = work_low;
        std::int64_t high = work_high;
        if (size > work_high - work_low) {
            low = desktop_low;
            high = desktop_high;
        }
        if (size >= high - low) {
            return static_cast<std::int32_t>(low);
        }
        const std::int64_t max_origin = high - size;
        return static_cast<std::int32_t>(
            std::max(low, std::min(static_cast<std::int64_t>(origin), max_origin)));
    };
    return {clamp_axis(rect.x, rect.width, work.left, work.right, desktop.left, desktop.right),
            clamp_axis(rect.y, rect.height, work.top, work.bottom, desktop.top, desktop.bottom),
            rect.width, rect.height};
}

bool place_guest_window(const DisplayLayout& layout, const RectI32& desired_guest,
                        InitialPlacementMode mode, const RectI32* owner_guest,
                        RectI32& placed_guest, const MonitorDesc** selected_monitor) {
    if (desired_guest.width <= 0 || desired_guest.height <= 0 ||
        (mode == InitialPlacementMode::CenterOwner && owner_guest == nullptr)) {
        return false;
    }
    RectI32 desired_host;
    if (!guest_to_host(layout, desired_guest, desired_host)) {
        return false;
    }

    RectI32 owner_host;
    if (owner_guest != nullptr && !guest_to_host(layout, *owner_guest, owner_host)) {
        return false;
    }

    if (mode == InitialPlacementMode::Preserve) {
        // Preserve is a reachability test, not normalization. One physical-pixel row measures the
        // GUEST CLIENT's own top/menu/title row against snapshot work areas without introducing a
        // DPI-sensitive grip constant. HOST caption reachability is deliberately owned by the
        // outer-frame containment and the realization reconcile, not by this client policy.
        // A positive intersection therefore keeps even a straddler, taskbar-overlapping body, or
        // one-pixel-visible authored position byte-for-byte unchanged.
        const RectI32 top_row{desired_host.x, desired_host.y, desired_host.width, 1};
        const MonitorDesc* reachable = find_monitor_work_largest_intersection(layout, top_row);
        if (reachable != nullptr) {
            placed_guest = desired_guest;
            if (selected_monitor != nullptr) {
                *selected_monitor = reachable;
            }
            return true;
        }
    }

    const MonitorDesc* monitor = nullptr;
    if (mode == InitialPlacementMode::CenterPrimary) {
        monitor = find_monitor_by_id(layout, layout.primary_monitor_id);
    } else if (mode == InitialPlacementMode::CenterOwner) {
        monitor = find_monitor_largest_intersection(layout, owner_host);
        if (monitor == nullptr) {
            monitor = find_monitor_nearest(layout, owner_host);
        }
    } else {
        monitor = find_monitor_largest_intersection(layout, desired_host);
        if (monitor == nullptr) {
            monitor = find_monitor_nearest(layout, desired_host);
        }
    }
    if (monitor == nullptr) {
        return false;
    }

    RectI32 candidate = desired_host;
    if (mode != InitialPlacementMode::Preserve) {
        const RectI32& anchor =
            mode == InitialPlacementMode::CenterOwner ? owner_host : monitor->work;
        const std::int64_t x = static_cast<std::int64_t>(anchor.x) +
                               (static_cast<std::int64_t>(anchor.width) - candidate.width) / 2;
        const std::int64_t y = static_cast<std::int64_t>(anchor.y) +
                               (static_cast<std::int64_t>(anchor.height) - candidate.height) / 2;
        if (x < std::numeric_limits<std::int32_t>::min() ||
            x > std::numeric_limits<std::int32_t>::max() ||
            y < std::numeric_limits<std::int32_t>::min() ||
            y > std::numeric_limits<std::int32_t>::max()) {
            return false;
        }
        candidate.x = static_cast<std::int32_t>(x);
        candidate.y = static_cast<std::int32_t>(y);
    }
    candidate = clamp_rect_to_monitor_work_or_virtual(candidate, *monitor, layout.virtual_bounds);
    if (!host_to_guest(layout, candidate, placed_guest)) {
        return false;
    }
    if (selected_monitor != nullptr) {
        *selected_monitor = monitor;
    }
    return true;
}

MonitorWorkPlacement monitor_work_placement(const MonitorDesc& monitor) {
    return {monitor.work.x - monitor.bounds.x, monitor.work.y - monitor.bounds.y,
            monitor.work.width, monitor.work.height};
}

std::vector<MonitorDesc> deduplicate_mirrored_monitors(const std::vector<MonitorDesc>& monitors) {
    std::vector<MonitorDesc> result;
    for (const MonitorDesc& monitor : monitors) {
        auto existing = std::find_if(result.begin(), result.end(), [&](const MonitorDesc& item) {
            return item.bounds == monitor.bounds;
        });
        if (existing == result.end()) {
            result.push_back(monitor);
        } else if (preferred(monitor, *existing)) {
            *existing = monitor;
        }
    }
    std::sort(result.begin(), result.end(),
              [](const MonitorDesc& a, const MonitorDesc& b) { return a.stable_id < b.stable_id; });
    return result;
}

} // namespace vkr::display
