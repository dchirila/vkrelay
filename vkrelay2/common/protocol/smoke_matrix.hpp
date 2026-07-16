// Smoke/canary compatibility matrix metadata.
//
// The matrix represents display-backend x graphics-API coverage as structured data instead of
// stringly typed special cases. Entries can name real canaries as they become available.
#ifndef VKRELAY2_COMMON_PROTOCOL_SMOKE_MATRIX_HPP
#define VKRELAY2_COMMON_PROTOCOL_SMOKE_MATRIX_HPP

#include <string>
#include <vector>

namespace vkr::protocol {

enum class DisplayBackend { X11Xwayland, WaylandNative };
enum class GraphicsFrontend { Vulkan13, OpenGL46Zink };
enum class SurfaceShape { TopLevel, PopupMenu, EmbeddedSubsurface };

const char* to_string(DisplayBackend backend);
const char* to_string(GraphicsFrontend frontend);
const char* to_string(SurfaceShape shape);

struct SmokeEntry {
    DisplayBackend display;
    GraphicsFrontend frontend;
    SurfaceShape shape;
    std::string gpu_selector; // "auto" or a concrete selector
    std::string canary;       // Placeholder or real canary name.
    bool implemented = false; // session 1: all false (mocked)
};

std::vector<SmokeEntry> default_matrix();

// True iff every (display backend x graphics frontend) combination appears at
// least once. This is the session-1 completeness gate.
bool matrix_covers_all_backends(const std::vector<SmokeEntry>& entries);

std::string matrix_to_json(const std::vector<SmokeEntry>& entries);

} // namespace vkr::protocol

#endif // VKRELAY2_COMMON_PROTOCOL_SMOKE_MATRIX_HPP
