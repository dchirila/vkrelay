#include "windows/supervisor/display_layout_probe.hpp"

#include <cstdio>
#include <string>

#include <windows.h>

int main() {
    if (GetSystemMetrics(SM_CMONITORS) <= 0) {
        std::fprintf(stderr, "integration_display_probe: SKIP (no interactive desktop monitors)\n");
        return 77;
    }
    const vkr::supervisor::DisplayLayoutProbeResult result =
        vkr::supervisor::probe_display_layout("integration/display-1");
    for (const std::string& warning : result.warnings) {
        std::fprintf(stderr, "integration_display_probe: warning: %s\n", warning.c_str());
    }
    if (!result.ok) {
        std::fprintf(stderr, "integration_display_probe: FAIL: %s\n", result.reason.c_str());
        return 1;
    }
    std::fprintf(stderr, "integration_display_probe: PASS (%zu monitors, virtual %d,%d %dx%d)\n",
                 result.layout.monitors.size(), result.layout.virtual_bounds.x,
                 result.layout.virtual_bounds.y, result.layout.virtual_bounds.width,
                 result.layout.virtual_bounds.height);
    return 0;
}
