// vkrelay2 geometry-shot harness (on-screen verification, Windows).
//
// A tiny standalone dev harness that drives a REAL, VISIBLE worker window through a
// sidecar-authored RESIZE so run_resize_shot.ps1 can capture_window.ps1 the on-screen client BEFORE
// and AFTER. It uses a PLACEHOLDER host (a GDI-painted overlapped window) -- no Vulkan device
// needed -- so it runs on any Windows box AND PrintWindow captures real pixels (flip-model Vulkan
// content would screenshot black). It registers a toplevel (debug title tag on), paints a solid
// chrome to SHOW the window at the BEFORE extent, then on a step drives update_toplevel to the
// AFTER extent (the live resize) and repaints. It prints "ready=before"/"ready=after" + its
// HWND and steps on stdin lines, so the PowerShell wrapper captures at each pause and asserts the
// on-screen client extent grew.
//
// Windows + Vulkan SDK (links the real backend), but uses NO device. Not a ctest -- a
// dev/verification helper run by scripts/dev/run_resize_shot.ps1.
#include "windows/worker/real_vulkan_backend.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include <windows.h>

namespace {

// A solid-color whole-window chrome paint for `xid` at (w,h), generation/seq-tagged (the gate
// requires lifecycle_generation == the registry's current generation). BGRA.
vkr::sidecar::SidecarPaintChromeRequest solid_paint(std::uint64_t xid, std::uint64_t generation,
                                                    std::uint64_t seq, int w, int h,
                                                    unsigned char b, unsigned char g,
                                                    unsigned char r) {
    vkr::sidecar::SidecarPaintChromeRequest p;
    p.xid = xid;
    p.lifecycle_generation = generation;
    p.seq = seq;
    p.src_w = static_cast<std::uint32_t>(w);
    p.src_h = static_cast<std::uint32_t>(h);
    p.dirty_w = static_cast<std::uint32_t>(w);
    p.dirty_h = static_cast<std::uint32_t>(h);
    p.stride = static_cast<std::uint32_t>(w) * 4;
    p.pixels.resize(static_cast<std::size_t>(w) * h * 4);
    for (std::size_t i = 0; i < p.pixels.size(); i += 4) {
        p.pixels[i + 0] = static_cast<char>(b);
        p.pixels[i + 1] = static_cast<char>(g);
        p.pixels[i + 2] = static_cast<char>(r);
        p.pixels[i + 3] = static_cast<char>(0xFF);
    }
    return p;
}

void wait_step() {
    std::string line;
    std::getline(std::cin, line); // the PS wrapper writes a line to advance after each capture
}

long long as_ll(HWND h) {
    return static_cast<long long>(reinterpret_cast<std::intptr_t>(h));
}

// True iff window `a` is ABOVE `b` in the z-order (walk GW_HWNDNEXT -- lower windows -- from a).
bool z_above(HWND a, HWND b) {
    HWND h = a;
    for (int i = 0; i < 20000 && h != nullptr; ++i) {
        h = GetWindow(h, GW_HWNDNEXT);
        if (h == b) {
            return true;
        }
    }
    return false;
}

// Multi-window stacking shot: two OVERLAPPING visible placeholders (A teal, B orange),
// then RAISE A above B. capture_window.ps1 (PrintWindow) is occlusion-proof, so the per-window PNGs
// the wrapper grabs are the multi-window VISUAL record; the actual stacking flip is asserted here,
// worker-visible, via GetWindow (the same gate as integration_real_backend test_real_z_order).
int run_zorder() {
    constexpr std::uint64_t kA = 0x5A01, kB = 0x5A02;
    vkr::worker::RealVulkanBackend backend("", "", /*gpu_required=*/false);
    auto reg = [&](std::uint64_t xid, int x, int y, int w, int h) {
        vkr::sidecar::SidecarRegisterToplevelRequest r;
        r.xid = xid;
        r.generation = 1;
        r.role = "toplevel";
        r.x = x;
        r.y = y;
        r.width = w;
        r.height = h;
        return backend.register_toplevel(r).applied;
    };
    // A first, then B (B is created on top by default) -- they overlap.
    if (!reg(kA, 200, 160, 360, 260) || !reg(kB, 360, 280, 360, 260)) {
        std::fprintf(stderr, "GEOMETRY-SHOT: zorder register failed\n");
        return 1;
    }
    const HWND ha = backend.debug_placeholder_hwnd_for_xid(kA);
    const HWND hb = backend.debug_placeholder_hwnd_for_xid(kB);
    if (ha == nullptr || hb == nullptr) {
        std::fprintf(stderr, "GEOMETRY-SHOT: no window thread (headless box)\n");
        return 1;
    }
    backend.paint_chrome(solid_paint(kA, 1, 1, 360, 260, 0xAA, 0x66, 0x22)); // teal
    backend.paint_chrome(solid_paint(kB, 1, 1, 360, 260, 0x22, 0x88, 0xEE)); // orange
    std::printf("GEOMETRY-SHOT: hwndA=%lld hwndB=%lld B_above_A=%d ready=before\n", as_ll(ha),
                as_ll(hb), z_above(hb, ha) ? 1 : 0);
    std::fflush(stdout);
    wait_step();

    // Raise A above B (a sidecar-authored restack).
    vkr::sidecar::SidecarUpdateToplevelRequest m;
    m.xid = kA;
    m.generation = 2;
    m.x = 200;
    m.y = 160;
    m.width = 360;
    m.height = 260;
    m.z_order = static_cast<std::uint32_t>(vkr::sidecar::ZOrder::Raise);
    backend.update_toplevel(m);
    const bool a_on_top = z_above(ha, hb);
    std::printf("GEOMETRY-SHOT: hwndA=%lld hwndB=%lld A_above_B=%d ready=after\n", as_ll(ha),
                as_ll(hb), a_on_top ? 1 : 0);
    std::fflush(stdout);
    wait_step();
    return a_on_top ? 0 : 2; // nonzero if the raise did not put A on top
}

} // namespace

int main(int argc, char** argv) {
    // The placeholder gets the debug title tag ("vkrelay2 [xid=0x...]") so the capture helper
    // can also correlate it; we additionally print the HWND for a direct -Hwnd capture.
    _putenv_s("VKRELAY2_DEBUG_WINDOW_TITLES", "1");

    if (argc > 1 && std::string(argv[1]) == "zorder") {
        return run_zorder();
    }

    constexpr std::uint64_t kXid = 0x4D32; // recognizable test id
    constexpr int kBeforeW = 400, kBeforeH = 300;
    constexpr int kAfterW = 760, kAfterH = 520;

    vkr::worker::RealVulkanBackend backend("", "", /*gpu_required=*/false);

    vkr::sidecar::SidecarRegisterToplevelRequest reg;
    reg.xid = kXid;
    reg.generation = 1;
    reg.role = "toplevel";
    reg.x = 120;
    reg.y = 90;
    reg.width = kBeforeW;
    reg.height = kBeforeH;
    if (!backend.register_toplevel(reg).applied) {
        std::fprintf(stderr, "GEOMETRY-SHOT: register failed\n");
        return 1;
    }
    const HWND hwnd = backend.debug_placeholder_hwnd_for_xid(kXid);
    if (hwnd == nullptr) {
        std::fprintf(stderr, "GEOMETRY-SHOT: no window thread (headless box)\n");
        return 1;
    }

    // Show the window at the BEFORE extent (a solid teal chrome makes PrintWindow capture real
    // pixels).
    const vkr::sidecar::SidecarPaintResponse p0 =
        backend.paint_chrome(solid_paint(kXid, 1, 1, kBeforeW, kBeforeH, 0xAA, 0x66, 0x22));
    if (!p0.shown) {
        std::fprintf(stderr, "GEOMETRY-SHOT: paint/show failed\n");
        return 1;
    }
    std::printf("GEOMETRY-SHOT: hwnd=%lld before=%dx%d ready=before\n",
                static_cast<long long>(reinterpret_cast<std::intptr_t>(hwnd)), kBeforeW, kBeforeH);
    std::fflush(stdout);
    wait_step();

    // Sidecar-authored RESIZE to the AFTER extent (the live resize path) + repaint at the new size
    // (generation 2 -- update_toplevel advanced it).
    vkr::sidecar::SidecarUpdateToplevelRequest rs;
    rs.xid = kXid;
    rs.generation = 2;
    rs.x = 120;
    rs.y = 90;
    rs.width = kAfterW;
    rs.height = kAfterH;
    if (!backend.update_toplevel(rs).applied) {
        std::fprintf(stderr, "GEOMETRY-SHOT: resize update failed\n");
        return 1;
    }
    backend.paint_chrome(solid_paint(kXid, 2, 2, kAfterW, kAfterH, 0x22, 0x88, 0xCC));
    std::printf("GEOMETRY-SHOT: hwnd=%lld after=%dx%d ready=after\n",
                static_cast<long long>(reinterpret_cast<std::intptr_t>(hwnd)), kAfterW, kAfterH);
    std::fflush(stdout);
    wait_step();

    vkr::sidecar::SidecarUnregisterToplevelRequest u;
    u.xid = kXid;
    u.generation = 3;
    backend.unregister_toplevel(u);
    return 0;
}
