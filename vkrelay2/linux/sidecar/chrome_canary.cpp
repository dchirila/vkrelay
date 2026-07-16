// vkrelay2 chrome canary (boundary-smoke helper, WSL/xcb; delayed mode).
//
// A pure-xcb client (NO Vulkan) that maps a normal toplevel filled with a KNOWN solid color, then
// idles. Under the sidecar WM (which holds SubstructureRedirect + XComposite-redirects the
// toplevel), this is a non-surface "chrome" window: the sidecar captures its pixels and ships them
// to the worker, which paints them into the placeholder HWND. run_chrome_smoke.sh then proves --
// via the worker- visible DebugChromeState wire query, not a log -- that the worker's DIB holds
// this exact color.
//
// The color is fixed at 0x336699 (R=0x33 G=0x66 B=0x99); on a TrueColor 8-8-8 visual the worker's
// captured BGRA packs to pixel_bgra = 0xFF336699 (B | G<<8 | R<<16 | A<<24). The window XID is
// printed so the smoke can query it. Prints "CHROME-CANARY: xid=<n>" and runs until killed.
//
// Delayed mode (`--delay-color-ms <N>`): start the window BLACK, then N ms after map switch the
// fill to the known color and repaint WITH NO Expose (xcb_clear_area exposures=0). This reproduces
// the exact failure -- a non-GL toolkit window that paints its real content a beat after its first
// Expose
// -- so a passing run_recapture_smoke PROVES the sidecar's recapture (not an Expose) shipped the
// late color. Default (no flag): the known color from the start (the solid-chrome behavior).
// `--shape-center-after-ms <N>` changes only the BOUNDING shape after map and reports any later
// Expose, allowing the smoke to prove whether the steady recapture poll observes a shape-only edge.
#include "canary_hints.h" // declare PPosition so the WM keeps the canary at its fixed origin
#include <xcb/xcb.h>
#ifdef VKRELAY2_HAVE_XCB_SHAPE
#include <xcb/shape.h>
#endif

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <poll.h>

namespace {
constexpr std::uint32_t kCanaryColor = 0x00336699; // 0xRRGGBB on a TrueColor 8-8-8 visual
constexpr std::uint32_t kCanaryBlack = 0x00000000;
constexpr std::uint16_t kCanaryW = 200;
constexpr std::uint16_t kCanaryH = 150;

std::uint64_t now_ms() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}
} // namespace

int main(int argc, char** argv) {
    // Delayed-color mode (default off -> solid color from the start).
    long delay_ms = -1;
    bool shape_center = false;
    long shape_center_after_ms = -1;
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--delay-color-ms") == 0) {
            delay_ms = std::atol(argv[i + 1]);
        } else if (std::strcmp(argv[i], "--shape-center-after-ms") == 0) {
            shape_center_after_ms = std::atol(argv[i + 1]);
        }
    }
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--shape-center") == 0) {
            shape_center = true;
        }
    }

    int screen_num = 0;
    xcb_connection_t* c = xcb_connect(nullptr, &screen_num);
    if (c == nullptr || xcb_connection_has_error(c)) {
        std::printf("CHROME-CANARY: FAIL (cannot connect to X)\n");
        return 1;
    }
    const xcb_setup_t* setup = xcb_get_setup(c);
    xcb_screen_iterator_t it = xcb_setup_roots_iterator(setup);
    for (int i = 0; i < screen_num; i++) {
        xcb_screen_next(&it);
    }
    xcb_screen_t* screen = it.data;

    const std::uint32_t initial_color = (delay_ms >= 0) ? kCanaryBlack : kCanaryColor;
    const xcb_window_t w = xcb_generate_id(c);
    // CW_BACK_PIXEL fills the window with the (initial) color: the server's fill is what XComposite
    // redirects into the backing pixmap, so the captured chrome IS this color. EXPOSURE so we
    // redraw.
    const std::uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
    const std::uint32_t vals[2] = {initial_color, XCB_EVENT_MASK_EXPOSURE};
    xcb_create_window(c, XCB_COPY_FROM_PARENT, w, screen->root, 0, 0, kCanaryW, kCanaryH, 0,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, mask, vals);
    vkr::canary::canary_declare_position(c, w); // keep the canary at its fixed origin
    if (shape_center) {
#ifdef VKRELAY2_HAVE_XCB_SHAPE
        // A binary center shape. The worker must see black at (5,5), regardless of whatever bytes
        // XComposite exposes outside this region, and the canary color at (100,75).
        const xcb_rectangle_t center{50, 40, 100, 70};
        xcb_shape_rectangles(c, XCB_SHAPE_SO_SET, XCB_SHAPE_SK_BOUNDING, XCB_CLIP_ORDERING_UNSORTED,
                             w, 0, 0, 1, &center);
#else
        std::printf("CHROME-CANARY: FAIL (--shape-center needs libxcb-shape)\n");
        xcb_disconnect(c);
        return 1;
#endif
    }
    xcb_map_window(c, w);
    xcb_flush(c);

    std::printf("CHROME-CANARY: xid=%u\n", static_cast<unsigned>(w));
    std::fflush(stdout);

    // Idle: keep the window mapped, refill on Expose, and switch to the known color once
    // after the delay -- WITHOUT an Expose, so only the sidecar's recapture can pick the color up.
    // Poll the X fd so the timed switch fires without an event. Runs until the connection drops
    // (smoke kills us).
    bool colored = (delay_ms < 0);
    const std::uint64_t deadline =
        now_ms() + (delay_ms >= 0 ? static_cast<std::uint64_t>(delay_ms) : 0);
    bool shape_changed = (shape_center_after_ms < 0);
    bool delayed_shape_applied = false;
    const std::uint64_t shape_deadline =
        now_ms() +
        (shape_center_after_ms >= 0 ? static_cast<std::uint64_t>(shape_center_after_ms) : 0);
    const int xfd = xcb_get_file_descriptor(c);
    for (;;) {
        xcb_generic_event_t* ev = nullptr;
        while ((ev = xcb_poll_for_event(c)) != nullptr) {
            if ((ev->response_type & ~0x80) == XCB_EXPOSE) {
                if (delayed_shape_applied) {
                    std::printf("CHROME-CANARY: post-shape Expose\n");
                    std::fflush(stdout);
                }
                xcb_clear_area(c, 0, w, 0, 0, 0, 0); // refill with the CURRENT BACK_PIXEL
                xcb_flush(c);
            }
            free(ev);
        }
        if (xcb_connection_has_error(c)) {
            break;
        }
        if (!colored && now_ms() >= deadline) {
            const std::uint32_t col = kCanaryColor;
            xcb_change_window_attributes(c, w, XCB_CW_BACK_PIXEL, &col);
            // exposures=0: our own repaint generates NO Expose -> the late color reaches the worker
            // ONLY via the sidecar's recapture (the recapture proof).
            xcb_clear_area(c, 0, w, 0, 0, 0, 0);
            xcb_flush(c);
            colored = true;
            std::printf("CHROME-CANARY: colored\n");
            std::fflush(stdout);
        }
        if (!shape_changed && now_ms() >= shape_deadline) {
#ifdef VKRELAY2_HAVE_XCB_SHAPE
            const xcb_rectangle_t center{50, 40, 100, 70};
            xcb_shape_rectangles(c, XCB_SHAPE_SO_SET, XCB_SHAPE_SK_BOUNDING,
                                 XCB_CLIP_ORDERING_UNSORTED, w, 0, 0, 1, &center);
            xcb_flush(c);
            shape_changed = true;
            delayed_shape_applied = true;
            std::printf("CHROME-CANARY: shaped\n");
            std::fflush(stdout);
#else
            std::printf("CHROME-CANARY: FAIL (--shape-center-after-ms needs libxcb-shape)\n");
            break;
#endif
        }
        struct pollfd pfd;
        pfd.fd = xfd;
        pfd.events = POLLIN;
        poll(&pfd, 1, 50);
    }
    xcb_disconnect(c);
    return 0;
}
