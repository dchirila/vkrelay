// vkrelay2 cursor canary (boundary-smoke helper, WSL/xcb).
//
// A pure-xcb client (NO Vulkan) that maps a normal toplevel and XDefineCursors a KNOWN solid-color
// cursor on it (a 16x16 cursor whose foreground is 0x336699). Under the sidecar WM, run_cursor_-
// smoke.sh warps the X pointer into this window (the sidecar's XTest), so the X display cursor
// becomes this cursor -> an XFixes CursorNotify -> the sidecar captures the cursor image and ships
// it to the worker, which builds an HCURSOR. The smoke then proves the worker's built cursor via
// the DebugCursorState wire op (vkrelay2-cursor-query).
//
// A solid source + solid mask 1-bit bitmap with foreground RGB (0x33,0x66,0x99) gives a cursor
// whose XFixes ARGB image is 0xFF336699 everywhere (alpha 255), which the worker samples to BGRA
// pixel_bgra = 0xFF336699 (B|G<<8|R<<16|A<<24). The window XID is printed so the smoke can query
// it.
//
// It does NOT advertise WM_DELETE_WINDOW, so the sidecar's canonical close request is a logged
// no-op and the canary stays mapped (keeping the cursor bound) until killed. Prints "CURSOR-CANARY:
// xid=<n>" and runs until killed.
#include "canary_hints.h" // declare PPosition so the WM keeps the canary at its fixed origin
#include <xcb/xcb.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace {
constexpr std::uint32_t kCanaryColor = 0x00336699; // 0xRRGGBB window fill (not the proof)
constexpr std::uint16_t kCanaryW = 200;
constexpr std::uint16_t kCanaryH = 150;
constexpr std::uint16_t kCursorDim = 16;
// The cursor's foreground, as 16-bit X color components (0x33 -> 0x3333, etc.). XFixes returns the
// 8-bit ARGB, so the worker samples BGRA 0xFF336699.
constexpr std::uint16_t kCurR = 0x3333;
constexpr std::uint16_t kCurG = 0x6666;
constexpr std::uint16_t kCurB = 0x9999;
} // namespace

int main() {
    int screen_num = 0;
    xcb_connection_t* c = xcb_connect(nullptr, &screen_num);
    if (c == nullptr || xcb_connection_has_error(c)) {
        std::printf("CURSOR-CANARY: FAIL (cannot connect to X)\n");
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
    const std::uint32_t vals[2] = {kCanaryColor, XCB_EVENT_MASK_EXPOSURE};
    xcb_create_window(c, XCB_COPY_FROM_PARENT, w, screen->root, 0, 0, kCanaryW, kCanaryH, 0,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, mask, vals);

    // Build a 16x16 1-bit cursor that is solid foreground everywhere: a source pixmap of all-1s + a
    // mask of all-1s, foreground = the known color. (Fill via a GC with foreground 1.)
    const xcb_pixmap_t src = xcb_generate_id(c);
    xcb_create_pixmap(c, 1, src, screen->root, kCursorDim, kCursorDim);
    const xcb_pixmap_t msk = xcb_generate_id(c);
    xcb_create_pixmap(c, 1, msk, screen->root, kCursorDim, kCursorDim);
    const xcb_gcontext_t gc = xcb_generate_id(c);
    const std::uint32_t gfg = 1; // set every bit
    xcb_create_gc(c, gc, src, XCB_GC_FOREGROUND, &gfg);
    const xcb_rectangle_t rect = {0, 0, kCursorDim, kCursorDim};
    xcb_poly_fill_rectangle(c, src, gc, 1, &rect);
    xcb_poly_fill_rectangle(c, msk, gc, 1, &rect);
    const xcb_cursor_t cur = xcb_generate_id(c);
    xcb_create_cursor(c, cur, src, msk, kCurR, kCurG, kCurB, /*back*/ 0, 0, 0, /*hotspot*/ 0, 0);
    const std::uint32_t cval = cur;
    xcb_change_window_attributes(c, w, XCB_CW_CURSOR, &cval);

    vkr::canary::canary_declare_position(c, w); // keep the canary at its fixed origin
    xcb_map_window(c, w);
    xcb_flush(c);

    std::printf("CURSOR-CANARY: xid=%u\n", static_cast<unsigned>(w));
    std::fflush(stdout);

    // Idle until the connection drops (the smoke kills us). Re-fill on Expose
    // (belt-and-suspenders).
    for (;;) {
        xcb_generic_event_t* ev = xcb_wait_for_event(c);
        if (ev == nullptr) {
            break;
        }
        if ((ev->response_type & ~0x80) == XCB_EXPOSE) {
            xcb_clear_area(c, 0, w, 0, 0, 0, 0);
            xcb_flush(c);
        }
        free(ev);
    }
    xcb_disconnect(c);
    return 0;
}
