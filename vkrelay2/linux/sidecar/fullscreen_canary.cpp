// vkrelay2 fullscreen canary (the ExtremeTuxRacer class -- boundary-smoke helper, WSL/xcb).
//
// A pure-xcb client (NO Vulkan) shaped exactly like SFML 2.5's non-EWMH fullscreen path: it maps a
// ROOT-SIZED OVERRIDE-REDIRECT toplevel and then waits for the one event SFML's sf::Window::create
// busy-waits on -- a VisibilityNotify with state != FullyObscured. Rootless Xwayland reports an
// UNREDIRECTED window FullyObscured (nothing is really presented; the worker shows captured pixels
// in host windows), and only a composite redirect flips it to Unobscured. So:
//   - PRE-FIX: the sidecar drops the root-sized override-redirect window (classified as popup
//     junk), nothing redirects it, the qualifying VisibilityNotify never arrives -> this canary
//     times out (exactly the silent multi-minute "hang" an SFML fullscreen app shows), FAIL;
//   - FIXED: the sidecar registers it as a fullscreen toplevel + cap_arm redirects it -> the
//     server emits VisibilityNotify Unobscured -> PASS.
// run_fullscreen_smoke.sh drives it through the real sidecar and also asserts the sidecar's
// registration decision in its log.
#include <xcb/xcb.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>

#include <unistd.h>

namespace {
constexpr std::uint32_t kFill = 0x00cc6633; // RRGGBB
constexpr int kDeadlineSeconds = 15;        // generous; the fixed path answers in milliseconds

double now_s() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) / 1e9;
}
} // namespace

int main() {
    int screen_num = 0;
    xcb_connection_t* c = xcb_connect(nullptr, &screen_num);
    if (c == nullptr || xcb_connection_has_error(c)) {
        std::printf("FULLSCREEN-CANARY: FAIL (cannot connect to X)\n");
        return 1;
    }
    const xcb_setup_t* setup = xcb_get_setup(c);
    xcb_screen_iterator_t it = xcb_setup_roots_iterator(setup);
    for (int i = 0; i < screen_num; i++) {
        xcb_screen_next(&it);
    }
    xcb_screen_t* screen = it.data;
    const std::uint16_t rw = screen->width_in_pixels;
    const std::uint16_t rh = screen->height_in_pixels;

    // SFML 2.5 fullscreen without EWMH: a root-sized override-redirect InputOutput window
    // selecting (among others) StructureNotify + VisibilityChange.
    const xcb_window_t w = xcb_generate_id(c);
    const std::uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK;
    const std::uint32_t vals[3] = {kFill, 1,
                                   XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_STRUCTURE_NOTIFY |
                                       XCB_EVENT_MASK_VISIBILITY_CHANGE};
    xcb_create_window(c, XCB_COPY_FROM_PARENT, w, screen->root, 0, 0, rw, rh, 0,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, mask, vals);
    xcb_map_window(c, w);
    xcb_flush(c);
    std::printf("FULLSCREEN-CANARY: xid=%u mapped override-redirect %ux%u (root %ux%u)\n",
                static_cast<unsigned>(w), rw, rh, rw, rh);
    std::fflush(stdout);

    // The SFML unblock condition, bounded: PASS on the first VisibilityNotify whose state is not
    // FullyObscured; FAIL on the deadline (SFML itself would spin forever).
    const double deadline = now_s() + kDeadlineSeconds;
    bool saw_fully_obscured = false;
    while (now_s() < deadline) {
        xcb_generic_event_t* ev = xcb_poll_for_event(c);
        if (ev == nullptr) {
            if (xcb_connection_has_error(c)) {
                std::printf("FULLSCREEN-CANARY: FAIL (X connection lost)\n");
                return 1;
            }
            usleep(20000);
            continue;
        }
        const std::uint8_t type = ev->response_type & ~0x80;
        if (type == XCB_MAP_NOTIFY) {
            std::printf("FULLSCREEN-CANARY: MapNotify\n");
            std::fflush(stdout);
        } else if (type == XCB_VISIBILITY_NOTIFY) {
            const xcb_visibility_notify_event_t* ve =
                reinterpret_cast<xcb_visibility_notify_event_t*>(ev);
            const bool obscured = ve->state == XCB_VISIBILITY_FULLY_OBSCURED;
            std::printf("FULLSCREEN-CANARY: VisibilityNotify %s\n",
                        obscured ? "FullyObscured" : "not-obscured");
            std::fflush(stdout);
            if (!obscured) {
                std::printf("FULLSCREEN-CANARY: PASS (non-obscured VisibilityNotify delivered -- "
                            "an SFML 2.5 fullscreen app leaves sf::Window::create)\n");
                free(ev);
                xcb_disconnect(c);
                return 0;
            }
            saw_fully_obscured = true;
        } else if (type == XCB_EXPOSE) {
            xcb_clear_area(c, 0, w, 0, 0, 0, 0); // refill with BACK_PIXEL
            xcb_flush(c);
        }
        free(ev);
    }
    std::printf("FULLSCREEN-CANARY: FAIL (no non-obscured VisibilityNotify within %ds%s -- an "
                "SFML 2.5 fullscreen app would spin forever in sf::Window::create)\n",
                kDeadlineSeconds,
                saw_fully_obscured ? "; saw only FullyObscured (window never redirected)" : "");
    xcb_disconnect(c);
    return 1;
}
