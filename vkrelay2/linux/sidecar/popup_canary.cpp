// vkrelay2 popup canary (boundary-smoke helper, WSL/xcb).
//
// A pure-xcb client (NO Vulkan) that maps a normal toplevel (the owner) and then a child
// OVERRIDE-REDIRECT popup that advertises _NET_WM_WINDOW_TYPE_MENU + WM_TRANSIENT_FOR = the
// toplevel, at a known root offset, filled with a KNOWN solid color. Under the sidecar WM, the
// toplevel registers via its MapRequest; the override-redirect popup self-maps and is detected via
// MapNotify, classified, owner-resolved to the toplevel, and given a placeholder-class host OWNED
// by + above the owner. run_popup_smoke.sh then proves -- worker-visible, over the wire, not by
// log-scrape -- that the worker represents the popup with owner_xid == toplevel + is_popup
// (DebugEnumWindows) and that its captured chrome reaches the worker's DIB (DebugChromeState).
//
// The popup color is fixed at 0x9933CC (R=0x99 G=0x33 B=0xCC); on a TrueColor 8-8-8 visual the
// worker's captured BGRA packs to pixel_bgra = 0xFF9933CC (B | G<<8 | R<<16 | A<<24). Both XIDs are
// printed. Prints "POPUP-CANARY: toplevel=<n> popup=<n>" and runs until killed.
//
// --delay-color-ms <N> (popup chrome recapture): reproduce a real Qt menu, which paints its
// items a beat AFTER its first Expose into the redirected pixmap with NO further Expose. With N>0
// the popup MAPS BLACK, then after N monotonic ms switches its background to the content color and
// repaints it with exposures=0 (NO Expose is generated) -- so only chrome RECAPTURE (not the
// Expose/map cap_ship) can carry the content to the worker. "POPUP-CANARY: delayed-paint applied"
// is then printed. With N==0 (default) the popup carries its content color from before the map
// (paint-before-map), the original one-shot-friendly behavior.
#include "canary_hints.h" // declare PPosition on the owner so the WM does not center it
#include <xcb/xcb.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <poll.h>

namespace {
constexpr std::uint32_t kOwnerColor = 0x00224466; // owner toplevel fill (RRGGBB)
constexpr std::uint32_t kPopupColor = 0x009933CC; // popup fill (RRGGBB) -> BGRA 0xFF9933CC
constexpr std::uint16_t kOwnerW = 400;
constexpr std::uint16_t kOwnerH = 300;
constexpr std::uint16_t kPopupW = 160;
constexpr std::uint16_t kPopupH = 220;
constexpr std::int16_t kPopupX = 60; // known X root placement (the smoke can cross-check geometry)
constexpr std::int16_t kPopupY = 80;

xcb_atom_t intern(xcb_connection_t* c, const char* name) {
    xcb_intern_atom_reply_t* r = xcb_intern_atom_reply(
        c, xcb_intern_atom(c, 0, static_cast<std::uint16_t>(strlen(name)), name), nullptr);
    if (r == nullptr) {
        return 0;
    }
    const xcb_atom_t a = r->atom;
    free(r);
    return a;
}
} // namespace

int main(int argc, char** argv) {
    // --delay-color-ms <N>: paint the popup's content color N ms AFTER map (0 = before map).
    long delay_color_ms = 0;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--delay-color-ms") == 0 && i + 1 < argc) {
            delay_color_ms = std::strtol(argv[++i], nullptr, 10);
            if (delay_color_ms < 0) {
                delay_color_ms = 0;
            }
        }
    }

    int screen_num = 0;
    xcb_connection_t* c = xcb_connect(nullptr, &screen_num);
    if (c == nullptr || xcb_connection_has_error(c)) {
        std::printf("POPUP-CANARY: FAIL (cannot connect to X)\n");
        return 1;
    }
    const xcb_setup_t* setup = xcb_get_setup(c);
    xcb_screen_iterator_t it = xcb_setup_roots_iterator(setup);
    for (int i = 0; i < screen_num; i++) {
        xcb_screen_next(&it);
    }
    xcb_screen_t* screen = it.data;

    // 1) The owner toplevel (normal, non-override-redirect). The sidecar registers it on
    // MapRequest.
    const xcb_window_t owner = xcb_generate_id(c);
    {
        const std::uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
        const std::uint32_t vals[2] = {kOwnerColor, XCB_EVENT_MASK_EXPOSURE};
        xcb_create_window(c, XCB_COPY_FROM_PARENT, owner, screen->root, 0, 0, kOwnerW, kOwnerH, 0,
                          XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, mask, vals);
        vkr::canary::canary_declare_position(c, owner); // keep the owner at its fixed origin
        xcb_map_window(c, owner);
    }
    // Flush + round-trip so the owner's MapRequest reaches (and is registered by) the sidecar
    // BEFORE the popup's MapNotify -- so the popup's WM_TRANSIENT_FOR owner resolves to a tracked
    // toplevel.
    xcb_flush(c);
    xcb_get_input_focus_reply_t* sync =
        xcb_get_input_focus_reply(c, xcb_get_input_focus(c), nullptr);
    free(sync);

    // 2) The override-redirect popup. Advertise the popup window-type + transient-for BEFORE
    // mapping, so the sidecar reads them on the MapNotify.
    const xcb_window_t popup = xcb_generate_id(c);
    const std::uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK;
    // Delayed mode maps BLACK (the pre-content frame a one-shot capture would ship) and paints the
    // content color later; immediate mode carries the content color from before the map.
    const std::uint32_t initial_popup_color = (delay_color_ms > 0) ? 0x00000000u : kPopupColor;
    const std::uint32_t vals[3] = {initial_popup_color, 1u, XCB_EVENT_MASK_EXPOSURE};
    xcb_create_window(c, XCB_COPY_FROM_PARENT, popup, screen->root, kPopupX, kPopupY, kPopupW,
                      kPopupH, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, mask, vals);
    const xcb_atom_t net_wm_window_type = intern(c, "_NET_WM_WINDOW_TYPE");
    const xcb_atom_t net_wm_window_type_menu = intern(c, "_NET_WM_WINDOW_TYPE_MENU");
    if (net_wm_window_type != 0 && net_wm_window_type_menu != 0) {
        xcb_change_property(c, XCB_PROP_MODE_REPLACE, popup, net_wm_window_type, XCB_ATOM_ATOM, 32,
                            1, &net_wm_window_type_menu);
    }
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, popup, XCB_ATOM_WM_TRANSIENT_FOR, XCB_ATOM_WINDOW,
                        32, 1, &owner);
    xcb_map_window(c, popup);
    xcb_flush(c);

    std::printf("POPUP-CANARY: toplevel=%u popup=%u\n", static_cast<unsigned>(owner),
                static_cast<unsigned>(popup));
    std::fflush(stdout);

    // Idle: keep both windows mapped (re-fill on Expose) until the connection drops (the smoke
    // kills us). BACK_PIXEL covers the fill; clear_area re-asserts it. The loop is deadline-aware
    // (poll with a short timeout, drain non-blocking) so the delayed content paint can fire N ms
    // after map WITHOUT an Expose -- exercising the chrome-recapture path, not the Expose cap_ship.
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    bool delayed_done = (delay_color_ms == 0); // immediate mode already carries the content color
    const int xfd = xcb_get_file_descriptor(c);
    for (;;) {
        xcb_generic_event_t* ev = nullptr;
        while ((ev = xcb_poll_for_event(c)) != nullptr) {
            if ((ev->response_type & ~0x80) == XCB_EXPOSE) {
                const xcb_expose_event_t* ee = reinterpret_cast<xcb_expose_event_t*>(ev);
                xcb_clear_area(c, 0, ee->window, 0, 0, 0, 0);
                xcb_flush(c);
            }
            free(ev);
        }
        if (xcb_connection_has_error(c)) {
            break; // connection error / shutdown (the smoke killed us)
        }
        if (!delayed_done) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            const long elapsed_ms =
                (now.tv_sec - t0.tv_sec) * 1000L + (now.tv_nsec - t0.tv_nsec) / 1000000L;
            if (elapsed_ms >= delay_color_ms) {
                // Switch the popup background to the content color + repaint with exposures=0 (NO
                // Expose): the redirected pixmap now holds the content, but only a recapture poll
                // can carry it to the worker -- exactly the real Qt-menu paint-after-map case.
                const std::uint32_t bp = kPopupColor;
                xcb_change_window_attributes(c, popup, XCB_CW_BACK_PIXEL, &bp);
                xcb_clear_area(c, 0, popup, 0, 0, 0, 0);
                xcb_flush(c);
                delayed_done = true;
                std::printf("POPUP-CANARY: delayed-paint applied\n");
                std::fflush(stdout);
            }
        }
        struct pollfd pfd;
        pfd.fd = xfd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        poll(&pfd, 1, 20); // wake to re-check the deadline / drain buffered events
    }
    xcb_disconnect(c);
    return 0;
}
