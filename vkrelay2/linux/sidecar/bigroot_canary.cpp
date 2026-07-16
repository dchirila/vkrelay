// vkrelay2 bigroot canary (guest-display-geometry boundary-smoke helper, WSL/xcb).
//
// A pure-xcb client (NO Vulkan) that reproduces the OpenSCAD unclickable-region bug shape: it maps
// a toplevel LARGER than the old default guest root (1024x640 weston-headless) at the explicit
// origin, and REPORTS (a) the guest root size and (b) the EXACT client coordinates of every
// ButtonPress it receives. Pointer injection warps an ABSOLUTE XTEST motion in root coords, and the
// X server clamps the pointer to the root -- so on an undersized root a click aimed beyond the root
// edge lands CLAMPED (wrong coords), while on a host-work-area-sized root it lands exactly.
// run_bigroot_smoke.sh asserts both the root size and the exact click coordinates. Optional
// `<width> <height> [x y]` arguments let the live gate place the canary around a queried-root
// point. Prints "BIGROOT-CANARY: xid=<n> root=<W>x<H>" then
// "BIGROOT-CANARY: click_at <client-x>,<client-y> root_at <root-x>,<root-y>" per press; runs until
// killed.
#include "canary_hints.h" // declare PPosition so the WM honors (or clamps) the origin, never centers
#include <xcb/xcb.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace {
constexpr std::uint32_t kFill = 0x00553311; // RRGGBB
// Larger than the old 1024x640 default root on BOTH axes (the OpenSCAD shape: window > root).
constexpr std::uint16_t kW = 1400;
constexpr std::uint16_t kH = 900;

bool parse_extent(const char* text, std::uint16_t& value) {
    if (text == nullptr || text[0] == '\0') {
        return false;
    }
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(text, &end, 10);
    if (end == text || *end != '\0' || parsed == 0 || parsed > 65535UL) {
        return false;
    }
    value = static_cast<std::uint16_t>(parsed);
    return true;
}

bool parse_origin(const char* text, std::int16_t& value) {
    if (text == nullptr || text[0] == '\0') {
        return false;
    }
    char* end = nullptr;
    const long parsed = std::strtol(text, &end, 10);
    if (end == text || *end != '\0' || parsed < -32768L || parsed > 32767L) {
        return false;
    }
    value = static_cast<std::int16_t>(parsed);
    return true;
}
} // namespace

int main(int argc, char** argv) {
    std::uint16_t width = kW;
    std::uint16_t height = kH;
    std::int16_t x = 0;
    std::int16_t y = 0;
    if (argc != 1 && ((argc != 3 && argc != 5) || !parse_extent(argv[1], width) ||
                      !parse_extent(argv[2], height) ||
                      (argc == 5 && (!parse_origin(argv[3], x) || !parse_origin(argv[4], y))))) {
        std::printf("BIGROOT-CANARY: FAIL (usage: %s [width height [x y]])\n", argv[0]);
        return 2;
    }
    int screen_num = 0;
    xcb_connection_t* c = xcb_connect(nullptr, &screen_num);
    if (c == nullptr || xcb_connection_has_error(c)) {
        std::printf("BIGROOT-CANARY: FAIL (cannot connect to X)\n");
        return 1;
    }
    const xcb_setup_t* setup = xcb_get_setup(c);
    xcb_screen_iterator_t it = xcb_setup_roots_iterator(setup);
    for (int i = 0; i < screen_num; i++) {
        xcb_screen_next(&it);
    }
    xcb_screen_t* screen = it.data;
    // The lane may aim at empty root space far from the small anchor window. Listen on the root
    // as well so the canary observes the exact XTest-delivered root coordinate.
    const std::uint32_t root_mask = XCB_EVENT_MASK_BUTTON_PRESS;
    xcb_change_window_attributes(c, screen->root, XCB_CW_EVENT_MASK, &root_mask);

    const xcb_window_t w = xcb_generate_id(c);
    const std::uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
    const std::uint32_t vals[2] = {kFill, XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_BUTTON_PRESS};
    xcb_create_window(c, XCB_COPY_FROM_PARENT, w, screen->root, x, y, width, height, 0,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, mask, vals);
    vkr::canary::canary_declare_position(c, w); // explicit origin (oversize is clamped to 0,0 here)
    xcb_map_window(c, w);
    xcb_flush(c);

    std::printf("BIGROOT-CANARY: xid=%u root=%ux%u\n", static_cast<unsigned>(w),
                static_cast<unsigned>(screen->width_in_pixels),
                static_cast<unsigned>(screen->height_in_pixels));
    std::fflush(stdout);

    for (;;) {
        xcb_generic_event_t* ev = xcb_wait_for_event(c);
        if (ev == nullptr) {
            break; // connection error / shutdown (the smoke killed us)
        }
        const std::uint8_t type = ev->response_type & ~0x80;
        if (type == XCB_BUTTON_PRESS) {
            const xcb_button_press_event_t* b = reinterpret_cast<xcb_button_press_event_t*>(ev);
            // event_x/event_y are window-relative CLIENT coords -- exactly what the injection
            // aimed.
            std::printf("BIGROOT-CANARY: click_at %d,%d root_at %d,%d\n",
                        static_cast<int>(b->event_x), static_cast<int>(b->event_y),
                        static_cast<int>(b->root_x), static_cast<int>(b->root_y));
            std::fflush(stdout);
        } else if (type == XCB_EXPOSE) {
            xcb_clear_area(c, 0, w, 0, 0, 0, 0); // refill with BACK_PIXEL
            xcb_flush(c);
        }
        free(ev);
    }
    xcb_disconnect(c);
    return 0;
}
