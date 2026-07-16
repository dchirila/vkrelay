// vkrelay2 placement canary (boundary-smoke helper, WSL/xcb).
//
// A pure-xcb client (NO Vulkan) that maps ONE normal toplevel with NO position hint -- it sets no
// WM_NORMAL_HINTS at all, so there is no USPosition/PPosition intent. This is the common toolkit
// default (Qt/GTK/Tk apps that let the WM place them), NOT glxgears: glxgears actually declares
// USPosition at 0,0, which the sidecar deliberately HONORS (the worker's on-screen clamp then keeps
// its chrome visible). This canary is the proof for the no-position case. Under the sidecar WM, the
// sidecar must CENTER such a window on the X root *before* mapping it (configuring the X window's
// position), and the worker then realizes the host there (+ an on-screen clamp).
// run_placement_smoke.sh proves -- worker-visible, over the wire (DebugEnumWindows include_actual)
// -- that the ACTUAL host CLIENT origin landed at the sidecar's centered origin (read from the
// sidecar log), NOT at the requested 0,0. Prints "PLACEMENT-CANARY: xid=<n> w=<w> h=<h>"; runs
// until killed.
//
// Deliberately does NOT include canary_hints.h: the whole point is to map UNpositioned.
#include <xcb/xcb.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace {
constexpr std::uint32_t kFill = 0x00224466; // RRGGBB
constexpr std::uint16_t kW = 400;
constexpr std::uint16_t kH = 300;
} // namespace

int main() {
    int screen_num = 0;
    xcb_connection_t* c = xcb_connect(nullptr, &screen_num);
    if (c == nullptr || xcb_connection_has_error(c)) {
        std::printf("PLACEMENT-CANARY: FAIL (cannot connect to X)\n");
        return 1;
    }
    const xcb_setup_t* setup = xcb_get_setup(c);
    xcb_screen_iterator_t it = xcb_setup_roots_iterator(setup);
    for (int i = 0; i < screen_num; i++) {
        xcb_screen_next(&it);
    }
    xcb_screen_t* screen = it.data;

    const xcb_window_t w = xcb_generate_id(c);
    const std::uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
    const std::uint32_t vals[2] = {kFill, XCB_EVENT_MASK_EXPOSURE};
    // Created at the origin with NO WM_NORMAL_HINTS -> the WM must place (center) it.
    xcb_create_window(c, XCB_COPY_FROM_PARENT, w, screen->root, 0, 0, kW, kH, 0,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, mask, vals);
    xcb_map_window(c, w);
    xcb_flush(c);

    std::printf("PLACEMENT-CANARY: xid=%u w=%u h=%u\n", static_cast<unsigned>(w), kW, kH);
    std::fflush(stdout);

    for (;;) {
        xcb_generic_event_t* ev = xcb_wait_for_event(c);
        if (ev == nullptr) {
            break;
        }
        if ((ev->response_type & ~0x80) == XCB_EXPOSE) {
            xcb_clear_area(c, 0, w, 0, 0, 0, 0); // refill with BACK_PIXEL
            xcb_flush(c);
        }
        free(ev);
    }
    xcb_disconnect(c);
    return 0;
}
