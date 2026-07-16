// vkrelay2 sidecar configure-passthrough probe (boundary-smoke helper, WSL/xcb).
//
// Runs as a CLIENT under the sidecar WM (which holds SubstructureRedirect on the root). It creates
// a window, maps it, then issues a ConfigureWindow (move + resize) and waits for the
// ConfigureNotify that reports the requested geometry. If the WM passes ConfigureRequest through
// (the correct minimal-WM behavior), the window becomes the requested size/position; a WM that
// drops ConfigureRequest leaves it unchanged and this probe times out and FAILS. Prints a single
// CONFIGURE-PROBE: PASS/FAIL line and exits 0/1. Not part of the product build (xcb only).
#include <xcb/xcb.h>

#include <poll.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>

int main() {
    int screen_num = 0;
    xcb_connection_t* c = xcb_connect(nullptr, &screen_num);
    if (c == nullptr || xcb_connection_has_error(c)) {
        std::printf("CONFIGURE-PROBE: FAIL (cannot connect to X)\n");
        return 1;
    }
    const xcb_setup_t* setup = xcb_get_setup(c);
    xcb_screen_iterator_t it = xcb_setup_roots_iterator(setup);
    for (int i = 0; i < screen_num; i++) {
        xcb_screen_next(&it);
    }
    xcb_screen_t* screen = it.data;

    const xcb_window_t w = xcb_generate_id(c);
    const std::uint32_t vals[2] = {screen->black_pixel, XCB_EVENT_MASK_STRUCTURE_NOTIFY};
    xcb_create_window(c, XCB_COPY_FROM_PARENT, w, screen->root, 0, 0, 100, 100, 0,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual,
                      XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK, vals);
    xcb_map_window(c, w);

    // The target geometry the WM must honor (move + resize). The window starts 100x100 at 0,0.
    const std::uint32_t kW = 300, kH = 200;
    const std::int32_t kX = 50, kY = 60;
    const std::uint32_t cfg[4] = {static_cast<std::uint32_t>(kX), static_cast<std::uint32_t>(kY),
                                  kW, kH};
    xcb_configure_window(c, w,
                         XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH |
                             XCB_CONFIG_WINDOW_HEIGHT,
                         cfg);
    xcb_flush(c);

    const int fd = xcb_get_file_descriptor(c);
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    bool ok = false;
    while (!ok) {
        struct pollfd pfd = {fd, POLLIN, 0};
        poll(&pfd, 1, 200);
        xcb_generic_event_t* ev;
        while ((ev = xcb_poll_for_event(c)) != nullptr) {
            if ((ev->response_type & ~0x80) == XCB_CONFIGURE_NOTIFY) {
                const xcb_configure_notify_event_t* ce =
                    reinterpret_cast<xcb_configure_notify_event_t*>(ev);
                if (ce->width == kW && ce->height == kH && ce->x == kX && ce->y == kY) {
                    ok = true;
                }
            }
            free(ev);
        }
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        const double el = (now.tv_sec - start.tv_sec) + (now.tv_nsec - start.tv_nsec) / 1e9;
        if (el > 5.0) {
            break;
        }
    }

    if (ok) {
        std::printf("CONFIGURE-PROBE: PASS (WM honored ConfigureRequest -> %ux%u+%d+%d)\n", kW, kH,
                    kX, kY);
    } else {
        std::printf("CONFIGURE-PROBE: FAIL (WM did not apply the requested geometry)\n");
    }
    xcb_disconnect(c);
    return ok ? 0 : 1;
}
