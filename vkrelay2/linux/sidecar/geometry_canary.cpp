// vkrelay2 geometry canary (boundary-smoke helper, WSL/xcb).
//
// A pure-xcb client (NO Vulkan) that maps TWO normal toplevels under the sidecar WM:
//   - a STATIC window mapped at a known nonzero X-root position that NEVER self-moves -- proves
//     INITIAL PLACEMENT (the vkcube / OpenSCAD class that maps once and never moves).
//     The worker must place the host at the register geometry without any
//     ConfigureRequest.
//   - a MOVER window that, after mapping, MOVES ITSELF across a sequence of positions via
//     xcb_configure_window -- proves the live UpdateToplevel move path over the wire.
//
// Under the sidecar WM (SubstructureRedirect on root), the map registers each toplevel and each
// self-move arrives as a ConfigureRequest the sidecar honors + forwards as an UpdateToplevel.
// run_geometry_smoke.sh proves -- worker-visible, over the wire -- that the worker's ACTUAL host
// CLIENT origin converged to BOTH the static map position and the mover's final position
// (DebugEnumWindows include_actual). Prints both xids + expected positions; runs until killed.
#include "canary_hints.h" // declare PPosition so the WM honors the deliberate map position
#include <xcb/xcb.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace {
constexpr std::uint32_t kFill = 0x00335577; // RRGGBB
constexpr std::uint16_t kW = 320;
constexpr std::uint16_t kH = 240;
// The static window's fixed map position (never moved) -- the initial-placement proof.
constexpr std::int16_t kStaticX = 120;
constexpr std::int16_t kStaticY = 90;
// The mover's move sequence; the LAST entry is the final authored position the smoke asserts.
constexpr std::int16_t kMovesX[] = {40, 180, 260};
constexpr std::int16_t kMovesY[] = {30, 120, 175};
constexpr int kMoveCount = 3;
// The mover's final self-RESIZE: after the moves it resizes itself (at the final position)
// so the smoke proves the worker converged the host CLIENT extent to the authored size over the
// wire.
constexpr std::uint16_t kMoverW = 500;
constexpr std::uint16_t kMoverH = 340;

void sync(xcb_connection_t* c) {
    xcb_get_input_focus_reply_t* r = xcb_get_input_focus_reply(c, xcb_get_input_focus(c), nullptr);
    free(r);
}

xcb_window_t make_window(xcb_connection_t* c, xcb_screen_t* screen, std::int16_t x,
                         std::int16_t y) {
    const xcb_window_t win = xcb_generate_id(c);
    const std::uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
    const std::uint32_t vals[2] = {kFill, XCB_EVENT_MASK_EXPOSURE};
    xcb_create_window(c, XCB_COPY_FROM_PARENT, win, screen->root, x, y, kW, kH, 0,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, mask, vals);
    vkr::canary::canary_declare_position(c, win); // this canary positions deliberately
    xcb_map_window(c, win);
    xcb_flush(c);
    sync(c); // round-trip so the MapRequest is registered before the next step
    return win;
}

void move_to(xcb_connection_t* c, xcb_window_t w, std::int16_t x, std::int16_t y) {
    const std::uint32_t vals[2] = {static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y)};
    xcb_configure_window(c, w, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, vals);
    xcb_flush(c);
    sync(c); // round-trip so the ConfigureRequest reaches the sidecar before the next
}

void resize_to(xcb_connection_t* c, xcb_window_t w, std::uint16_t cw, std::uint16_t ch) {
    const std::uint32_t vals[2] = {cw, ch};
    xcb_configure_window(c, w, XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, vals);
    xcb_flush(c);
    sync(c);
}

void raise_self(xcb_connection_t* c, xcb_window_t w) {
    const std::uint32_t vals[1] = {XCB_STACK_MODE_ABOVE};
    xcb_configure_window(c, w, XCB_CONFIG_WINDOW_STACK_MODE, vals);
    xcb_flush(c);
    sync(c);
}
} // namespace

int main() {
    int screen_num = 0;
    xcb_connection_t* c = xcb_connect(nullptr, &screen_num);
    if (c == nullptr || xcb_connection_has_error(c)) {
        std::printf("GEOMETRY-CANARY: FAIL (cannot connect to X)\n");
        return 1;
    }
    const xcb_setup_t* setup = xcb_get_setup(c);
    xcb_screen_iterator_t it = xcb_setup_roots_iterator(setup);
    for (int i = 0; i < screen_num; i++) {
        xcb_screen_next(&it);
    }
    xcb_screen_t* screen = it.data;

    // The static window: map at a known nonzero position, NEVER move it.
    const xcb_window_t stat = make_window(c, screen, kStaticX, kStaticY);
    // The mover: map at the first position, self-move across the sequence, then self-RESIZE.
    const xcb_window_t mover = make_window(c, screen, kMovesX[0], kMovesY[0]);
    for (int i = 0; i < kMoveCount; ++i) {
        move_to(c, mover, kMovesX[i], kMovesY[i]);
    }
    resize_to(c, mover, kMoverW, kMoverH);
    // the mover self-RAISES (an X Above request) -- the sidecar forwards a Raise; the worker
    // restacks the host + records the sticky last z-order (the smoke asserts it over the wire).
    raise_self(c, mover);

    std::printf(
        "GEOMETRY-CANARY: static=%u static_pos=%d,%d mover=%u mover_final=%d,%d mover_size=%d,%d\n",
        static_cast<unsigned>(stat), static_cast<int>(kStaticX), static_cast<int>(kStaticY),
        static_cast<unsigned>(mover), static_cast<int>(kMovesX[kMoveCount - 1]),
        static_cast<int>(kMovesY[kMoveCount - 1]), static_cast<int>(kMoverW),
        static_cast<int>(kMoverH));
    std::fflush(stdout);

    // Idle: keep both windows mapped (re-fill on Expose) until the connection drops (the smoke
    // kills us).
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
