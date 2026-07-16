// vkrelay2 lifecycle canary (boundary-smoke helper, WSL/xcb).
//
// A pure-xcb client (NO Vulkan): maps ONE normal toplevel under the sidecar WM, then UNMAPS it and
// REMAPS it -- the show/hide lifecycle over the wire. Under the sidecar WM (SubstructureRedirect on
// root): the first map registers it; the unmap is forwarded as set_visibility(Hidden) -- NOT a
// teardown, so the representation (and its epoch) survives; the remap (a MapRequest of a
// still-tracked window) is forwarded as set_visibility(Visible), a RESTORE at the SAME
// representation epoch. The canary synchronizes with the WM via StructureNotify (it waits for the
// WM's MapNotify/UnmapNotify on its window) so the smoke's log + worker assertions race nothing.
// Prints its xid + map position; runs until killed. run_lifecycle_smoke.sh proves -- over the wire
// -- the sidecar event mapping (its log) AND that the worker kept the SAME entry visible again at
// the SAME epoch (DebugEnumWindows): the hidden-but-represented bug fix.
#include "canary_hints.h" // declare PPosition so the WM honors the deliberate map position
#include <xcb/xcb.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace {
constexpr std::uint32_t kFill = 0x00224466; // RRGGBB
constexpr std::uint16_t kW = 300;
constexpr std::uint16_t kH = 220;
constexpr std::int16_t kX = 60;
constexpr std::int16_t kY = 80;

// Pump events until a StructureNotify of `type` (MapNotify / UnmapNotify) for `win` arrives
// (re-filling on Expose meanwhile, so the window has pixels for the sidecar's chrome capture).
// Returns false if the connection drops first.
bool wait_for(xcb_connection_t* c, xcb_window_t win, std::uint8_t type) {
    for (;;) {
        xcb_generic_event_t* ev = xcb_wait_for_event(c);
        if (ev == nullptr) {
            return false;
        }
        const std::uint8_t t = ev->response_type & ~0x80;
        bool hit = false;
        if (t == XCB_EXPOSE) {
            const xcb_expose_event_t* ee = reinterpret_cast<xcb_expose_event_t*>(ev);
            xcb_clear_area(c, 0, ee->window, 0, 0, 0, 0);
            xcb_flush(c);
        } else if (t == type) {
            // MapNotify / UnmapNotify both carry `window` as the second field.
            const xcb_map_notify_event_t* mn = reinterpret_cast<xcb_map_notify_event_t*>(ev);
            hit = (mn->window == win);
        }
        free(ev);
        if (hit) {
            return true;
        }
    }
}
} // namespace

int main() {
    int screen_num = 0;
    xcb_connection_t* c = xcb_connect(nullptr, &screen_num);
    if (c == nullptr || xcb_connection_has_error(c)) {
        std::printf("LIFECYCLE-CANARY: FAIL (cannot connect to X)\n");
        return 1;
    }
    const xcb_setup_t* setup = xcb_get_setup(c);
    xcb_screen_iterator_t it = xcb_setup_roots_iterator(setup);
    for (int i = 0; i < screen_num; i++) {
        xcb_screen_next(&it);
    }
    xcb_screen_t* screen = it.data;

    const xcb_window_t win = xcb_generate_id(c);
    const std::uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
    const std::uint32_t vals[2] = {kFill,
                                   XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_STRUCTURE_NOTIFY};
    xcb_create_window(c, XCB_COPY_FROM_PARENT, win, screen->root, kX, kY, kW, kH, 0,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, mask, vals);
    vkr::canary::canary_declare_position(c, win); // positions deliberately (kX,kY)

    // Map -> the WM (SubstructureRedirect) registers + maps it; wait for the WM's MapNotify.
    xcb_map_window(c, win);
    xcb_flush(c);
    if (!wait_for(c, win, XCB_MAP_NOTIFY)) {
        std::printf("LIFECYCLE-CANARY: FAIL (initial map)\n");
        return 1;
    }

    // Unmap -> the WM forwards set_visibility(Hidden) (hidden-but-represented); wait for
    // UnmapNotify.
    xcb_unmap_window(c, win);
    xcb_flush(c);
    if (!wait_for(c, win, XCB_UNMAP_NOTIFY)) {
        std::printf("LIFECYCLE-CANARY: FAIL (unmap)\n");
        return 1;
    }

    // Remap -> a MapRequest of a still-tracked window -> set_visibility(Visible) RESTORE (not a
    // fresh register); wait for MapNotify.
    xcb_map_window(c, win);
    xcb_flush(c);
    if (!wait_for(c, win, XCB_MAP_NOTIFY)) {
        std::printf("LIFECYCLE-CANARY: FAIL (remap)\n");
        return 1;
    }

    std::printf("LIFECYCLE-CANARY: xid=%u pos=%d,%d done=1\n", static_cast<unsigned>(win),
                static_cast<int>(kX), static_cast<int>(kY));
    std::fflush(stdout);

    // Idle: keep the window mapped (re-fill on Expose) until the connection drops (the smoke kills
    // us).
    for (;;) {
        xcb_generic_event_t* ev = xcb_wait_for_event(c);
        if (ev == nullptr) {
            break;
        }
        if ((ev->response_type & ~0x80) == XCB_EXPOSE) {
            const xcb_expose_event_t* ee = reinterpret_cast<xcb_expose_event_t*>(ev);
            xcb_clear_area(c, 0, ee->window, 0, 0, 0, 0);
            xcb_flush(c);
        }
        free(ev);
    }
    xcb_disconnect(c);
    return 0;
}
