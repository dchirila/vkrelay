// vkrelay2 feedback canary (boundary-smoke helper, WSL/xcb).
//
// A pure-xcb client (NO Vulkan): maps ONE normal toplevel at a known X-root position, then OBSERVES
// its own geometry. run_feedback_smoke.sh runs the sidecar with --debug-feedback-move X,Y (a move)
// or X,Y,W,H (a resize), which seeds a Win32-USER-origin GeometryRequest (the REVERSE
// direction) into the worker ring for this toplevel; the sidecar drains it and authors the GUEST
// move/resize (xcb_configure_window). The canary sees the resulting ConfigureNotify and prints its
// new position AND size, so the smoke proves -- over the wire -- that a worker-origin host gesture
// moved/resized the guest. Prints its xid + start, then each ConfigureNotify position+size; runs
// until killed.
#include "canary_hints.h" // declare PPosition so the WM honors the deliberate map position
#include <xcb/xcb.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace {
constexpr std::uint32_t kFill = 0x00446688; // RRGGBB
constexpr std::uint16_t kW = 300;
constexpr std::uint16_t kH = 220;
constexpr std::int16_t kX = 50;
constexpr std::int16_t kY = 60;
} // namespace

int main() {
    int screen_num = 0;
    xcb_connection_t* c = xcb_connect(nullptr, &screen_num);
    if (c == nullptr || xcb_connection_has_error(c)) {
        std::printf("FEEDBACK-CANARY: FAIL (cannot connect to X)\n");
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
    vkr::canary::canary_declare_position(c, win); // positions deliberately (kX,kY baseline)
    xcb_map_window(c, win);
    xcb_flush(c);

    // Wait for the WM's MapNotify (the sidecar mapped + registered us), then announce the xid +
    // start.
    bool mapped = false;
    while (!mapped) {
        xcb_generic_event_t* ev = xcb_wait_for_event(c);
        if (ev == nullptr) {
            std::printf("FEEDBACK-CANARY: FAIL (connection dropped before map)\n");
            return 1;
        }
        const std::uint8_t t = ev->response_type & ~0x80;
        if (t == XCB_EXPOSE) {
            const xcb_expose_event_t* ee = reinterpret_cast<xcb_expose_event_t*>(ev);
            xcb_clear_area(c, 0, ee->window, 0, 0, 0, 0);
            xcb_flush(c);
        } else if (t == XCB_MAP_NOTIFY) {
            const xcb_map_notify_event_t* mn = reinterpret_cast<xcb_map_notify_event_t*>(ev);
            mapped = (mn->window == win);
        }
        free(ev);
    }
    std::printf("FEEDBACK-CANARY: xid=%u start=%d,%d\n", static_cast<unsigned>(win),
                static_cast<int>(kX), static_cast<int>(kY));
    std::fflush(stdout);

    // Observe geometry: each ConfigureNotify is the realized geometry. The sidecar's authored move
    // (from the seeded user GeometryRequest) arrives here -- print every position so the smoke can
    // assert the guest followed the request. Re-fill on Expose; run until killed.
    for (;;) {
        xcb_generic_event_t* ev = xcb_wait_for_event(c);
        if (ev == nullptr) {
            break;
        }
        const std::uint8_t t = ev->response_type & ~0x80;
        if (t == XCB_EXPOSE) {
            const xcb_expose_event_t* ee = reinterpret_cast<xcb_expose_event_t*>(ev);
            xcb_clear_area(c, 0, ee->window, 0, 0, 0, 0);
            xcb_flush(c);
        } else if (t == XCB_CONFIGURE_NOTIFY) {
            const xcb_configure_notify_event_t* cn =
                reinterpret_cast<xcb_configure_notify_event_t*>(ev);
            if (cn->window == win) {
                // Position AND size: proves the guest followed a user MOVE, and that a user RESIZE
                // request makes the guest converge to the requested EXTENT too.
                std::printf("FEEDBACK-CANARY: configure=%d,%d size=%d,%d\n",
                            static_cast<int>(cn->x), static_cast<int>(cn->y),
                            static_cast<int>(cn->width), static_cast<int>(cn->height));
                std::fflush(stdout);
            }
        }
        free(ev);
    }
    xcb_disconnect(c);
    return 0;
}
