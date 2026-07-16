// vkrelay2 resize canary (boundary-smoke helper, WSL/xcb).
//
// A pure-xcb client (NO Vulkan) that maps ONE toplevel at a DELIBERATE position + size A, then --
// once the WM has mapped it -- issues its OWN ConfigureRequest RESIZE to size B (a guest-driven
// resize, the path a real toolkit takes when its window changes size). must forward that
// resize to the worker: sidecar ConfigureRequest -> forward_update -> registry size_changed ->
// apply_size -> the host CLIENT resizes. run_resize_smoke.sh proves it worker-VISIBLE over the wire
// (DebugEnumWindows include_actual shows the host client converged to B) -- the in-process
// integration_real_backend test drives the registry directly and so cannot cover the sidecar's
// X-event -> forward_update translation, which is exactly where a regression would hide.
//
// Declares PPosition (canary_hints) so HONORS the chosen position instead of centering it --
// the resize keeps that position (WIDTH|HEIGHT only), so the smoke asserts a fixed x/y and the new
// w/h. Prints "RESIZE-CANARY: xid=<n> ..." then "RESIZE-CANARY: resized" after issuing the resize;
// then runs until killed so the smoke can query the worker.
#include "canary_hints.h"

#include <xcb/xcb.h>

#include <poll.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace {
constexpr std::uint32_t kFill = 0x00335577; // RRGGBB
constexpr std::int16_t kX = 120;
constexpr std::int16_t kY = 90;
constexpr std::uint16_t kAW = 200; // initial size A
constexpr std::uint16_t kAH = 150;
constexpr std::uint16_t kBW = 360; // resized-to size B
constexpr std::uint16_t kBH = 260;
} // namespace

int main() {
    int screen_num = 0;
    xcb_connection_t* c = xcb_connect(nullptr, &screen_num);
    if (c == nullptr || xcb_connection_has_error(c)) {
        std::printf("RESIZE-CANARY: FAIL (cannot connect to X)\n");
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
    const std::uint32_t vals[2] = {kFill,
                                   XCB_EVENT_MASK_STRUCTURE_NOTIFY | XCB_EVENT_MASK_EXPOSURE};
    xcb_create_window(c, XCB_COPY_FROM_PARENT, w, screen->root, kX, kY, kAW, kAH, 0,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, mask, vals);
    vkr::canary::canary_declare_position(c, w); // PPosition -> honors kX,kY (no centering)
    xcb_map_window(c, w);
    xcb_flush(c);
    std::printf("RESIZE-CANARY: xid=%u x=%d y=%d w=%u h=%u -> w=%u h=%u\n",
                static_cast<unsigned>(w), kX, kY, kAW, kAH, kBW, kBH);
    std::fflush(stdout);

    // Wait until the WM maps us (the first ConfigureNotify reporting our realized geometry), then
    // issue the guest RESIZE (WIDTH|HEIGHT only -- keep the position) so the sidecar forwards a
    // SIZE change, not a move. Bounded; if the WM never maps us the resize is still issued (the
    // smoke then fails).
    const int fd = xcb_get_file_descriptor(c);
    bool mapped = false;
    for (int i = 0; i < 50 && !mapped; ++i) {
        struct pollfd pfd = {fd, POLLIN, 0};
        poll(&pfd, 1, 100);
        xcb_generic_event_t* ev;
        while ((ev = xcb_poll_for_event(c)) != nullptr) {
            if ((ev->response_type & ~0x80) == XCB_CONFIGURE_NOTIFY) {
                mapped = true;
            }
            free(ev);
        }
    }
    const std::uint32_t cfg[2] = {kBW, kBH};
    xcb_configure_window(c, w, XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, cfg);
    xcb_flush(c);
    std::printf("RESIZE-CANARY: resized\n");
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
