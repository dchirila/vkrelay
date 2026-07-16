#include "common/protocol/smoke_matrix.hpp"

#include "common/util/json.hpp"

#include <array>

namespace vkr::protocol {

const char* to_string(DisplayBackend backend) {
    switch (backend) {
    case DisplayBackend::X11Xwayland:
        return "x11-xwayland";
    case DisplayBackend::WaylandNative:
        return "wayland-native";
    }
    return "x11-xwayland";
}

const char* to_string(GraphicsFrontend frontend) {
    switch (frontend) {
    case GraphicsFrontend::Vulkan13:
        return "vulkan13";
    case GraphicsFrontend::OpenGL46Zink:
        return "opengl46-zink";
    }
    return "vulkan13";
}

const char* to_string(SurfaceShape shape) {
    switch (shape) {
    case SurfaceShape::TopLevel:
        return "top-level";
    case SurfaceShape::PopupMenu:
        return "popup-menu";
    case SurfaceShape::EmbeddedSubsurface:
        return "embedded-subsurface";
    }
    return "top-level";
}

std::vector<SmokeEntry> default_matrix() {
    const std::array<DisplayBackend, 2> displays = {DisplayBackend::X11Xwayland,
                                                    DisplayBackend::WaylandNative};
    const std::array<GraphicsFrontend, 2> frontends = {GraphicsFrontend::Vulkan13,
                                                       GraphicsFrontend::OpenGL46Zink};
    const std::array<SurfaceShape, 3> shapes = {SurfaceShape::TopLevel, SurfaceShape::PopupMenu,
                                                SurfaceShape::EmbeddedSubsurface};

    std::vector<SmokeEntry> entries;
    for (DisplayBackend display : displays) {
        for (GraphicsFrontend frontend : frontends) {
            for (SurfaceShape shape : shapes) {
                SmokeEntry entry;
                entry.display = display;
                entry.frontend = frontend;
                entry.shape = shape;
                entry.gpu_selector = "auto";
                entry.canary = std::string(to_string(frontend)) + "-" + to_string(display) + "-" +
                               to_string(shape) + "-canary";
                entry.implemented = false;
                entries.push_back(entry);
            }
        }
    }
    return entries;
}

bool matrix_covers_all_backends(const std::vector<SmokeEntry>& entries) {
    bool seen[2][2] = {{false, false}, {false, false}};
    for (const auto& e : entries) {
        seen[static_cast<int>(e.display)][static_cast<int>(e.frontend)] = true;
    }
    return seen[0][0] && seen[0][1] && seen[1][0] && seen[1][1];
}

std::string matrix_to_json(const std::vector<SmokeEntry>& entries) {
    json::Value root = json::Value::make_object();
    root.set("version", json::Value(1));
    root.set("note", json::Value("session-1 mocked smoke metadata; no apps run yet"));
    json::Array arr;
    for (const auto& e : entries) {
        json::Value obj = json::Value::make_object();
        obj.set("display_backend", json::Value(to_string(e.display)));
        obj.set("graphics_frontend", json::Value(to_string(e.frontend)));
        obj.set("surface_shape", json::Value(to_string(e.shape)));
        obj.set("gpu_selector", json::Value(e.gpu_selector));
        obj.set("canary", json::Value(e.canary));
        obj.set("implemented", json::Value(e.implemented));
        arr.emplace_back(std::move(obj));
    }
    root.set("entries", json::Value(std::move(arr)));
    return root.dump(2);
}

} // namespace vkr::protocol
