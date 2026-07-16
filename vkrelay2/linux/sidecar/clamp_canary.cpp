// vkrelay2 placement-clamp boundary canary (WSL/xcb).
//
// A pure-xcb client (NO Vulkan) that proves the on-screen CLIENT-ORIGIN invariant on the X side
// (the authored truth): an app/sidecar-authored toplevel geometry must never
// leave the menu bar (client top-left) off the root, or menu clicks map to negative root coords and
// miss.
//
// It exercises BOTH clamped authoring paths and reads back its OWN X-root geometry (the
// ground truth of where the guest window actually IS -- not a sidecar-log scrape, not the worker's
// separately-clamped host):
//   - MAP with an EXPLICIT off-top position (WM_NORMAL_HINTS PPosition, so the WM honors-or-clamps
//   it
//     rather than centering) -> exercises maybe_center_toplevel's explicit-position clamp;
//   - then a self ConfigureRequest to an off-LEFT position -> exercises the XCB_CONFIGURE_REQUEST
//     clamp (the realistic Qt "restore a saved off-screen geometry after map" case).
// Without the clamp the sidecar honors both requests verbatim (mapped_at == the off-top request),
// so run_clamp_smoke.sh FAILS; with the clamp the client origin stays on-screen and the
// readbacks land at the clamped targets. The window is NOT reparented (transparent pass-through
// WM), so xcb_get_geometry / ConfigureNotify x,y are already X-ROOT coords.
#include "canary_hints.h" // declare PPosition so the WM honors-or-clamps (does not center) the map pos
#include <xcb/xcb.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace {
constexpr std::uint32_t kFill = 0x00446688; // RRGGBB
constexpr std::uint16_t kW = 300;
constexpr std::uint16_t kH = 200;
// Map off the top-LEFT (both axes negative): tests the maybe_center_toplevel explicit-position
// clamp.
constexpr std::int16_t kMapX = -100;
constexpr std::int16_t kMapY = -100;
// Then self-move off the LEFT only (x negative, y on-screen): tests the XCB_CONFIGURE_REQUEST clamp
// and that the clamp is PER-AXIS (y must be preserved).
constexpr std::int16_t kCfgX = -50;
constexpr std::int16_t kCfgY = 100;
} // namespace

int main() {
    int screen_num = 0;
    xcb_connection_t* c = xcb_connect(nullptr, &screen_num);
    if (c == nullptr || xcb_connection_has_error(c)) {
        std::printf("CLAMP-CANARY: FAIL (cannot connect to X)\n");
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
                                   XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_STRUCTURE_NOTIFY};
    xcb_create_window(c, XCB_COPY_FROM_PARENT, w, screen->root, kMapX, kMapY, kW, kH, 0,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, mask, vals);
    vkr::canary::canary_declare_position(c, w); // explicit position -> honor-or-clamp, never center
    xcb_map_window(c, w);
    xcb_flush(c);

    std::printf("CLAMP-CANARY: xid=%u map_req=%d,%d cfg_req=%d,%d\n", static_cast<unsigned>(w),
                static_cast<int>(kMapX), static_cast<int>(kMapY), static_cast<int>(kCfgX),
                static_cast<int>(kCfgY));
    std::fflush(stdout);

    bool cfg_sent = false;
    bool map_reported = false;
    bool cfg_reported = false;
    for (;;) {
        xcb_generic_event_t* ev = xcb_wait_for_event(c);
        if (ev == nullptr) {
            break; // connection error / shutdown (the smoke killed us)
        }
        const std::uint8_t type = ev->response_type & ~0x80;
        if (type == XCB_MAP_NOTIFY && !map_reported) {
            // The sidecar clamps BEFORE mapping, so the placed geometry is final by MapNotify. Read
            // it from the server (ground truth); not reparented -> x,y are X-root coords.
            xcb_get_geometry_reply_t* g =
                xcb_get_geometry_reply(c, xcb_get_geometry(c, w), nullptr);
            if (g != nullptr) {
                std::printf("CLAMP-CANARY: mapped_at %d,%d\n", static_cast<int>(g->x),
                            static_cast<int>(g->y));
                std::fflush(stdout);
                free(g);
                map_reported = true;
            }
            if (!cfg_sent) {
                const std::uint32_t xy[2] = {static_cast<std::uint32_t>(kCfgX),
                                             static_cast<std::uint32_t>(kCfgY)};
                xcb_configure_window(c, w, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, xy);
                xcb_flush(c);
                cfg_sent = true;
            }
        } else if (type == XCB_CONFIGURE_NOTIFY && cfg_sent && !cfg_reported) {
            // The first ConfigureNotify after our request carries the sidecar's applied (clamped)
            // position in X-root coords.
            const xcb_configure_notify_event_t* ce =
                reinterpret_cast<xcb_configure_notify_event_t*>(ev);
            std::printf("CLAMP-CANARY: configured_at %d,%d\n", static_cast<int>(ce->x),
                        static_cast<int>(ce->y));
            std::fflush(stdout);
            cfg_reported = true;
        } else if (type == XCB_EXPOSE) {
            xcb_clear_area(c, 0, w, 0, 0, 0, 0); // refill with BACK_PIXEL
            xcb_flush(c);
        }
        free(ev);
    }
    xcb_disconnect(c);
    return 0;
}
