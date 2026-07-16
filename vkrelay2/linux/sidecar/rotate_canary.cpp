// vkrelay2 rotate canary (guest-Xwayland grab/recenter crash repro, WSL/xcb).
//
// A pure-xcb client (NO Vulkan, NO sidecar, NO injection) that reproduces Blender's view-rotate
// gesture at the protocol level: map + focus a toplevel, take an ACTIVE pointer grab, WARP the
// pointer to recenter repeatedly (as an app does to keep the cursor from drifting during a drag),
// then UNGRAB. On the private weston-headless + Xwayland stack the guest Xwayland has NO wl_seat
// (headless weston 9.0.0 creates no input devices), yet advertises pointer-constraints, so Xwayland
// attempts a seat-less pointer-warp emulation and null-derefs (0xa0) at grab teardown -- the exact
// crash, WITHOUT Blender or a human.
//
// ORACLE: the canary drives the sequence, then forces a server round-trip (GetInputFocus) and
// checks the X connection. Exit 0 + "ROTATE-CANARY: SURVIVED" iff Xwayland is still alive after the
// ungrab (the FIXED state / regression-gate green). Exit 1 + "ROTATE-CANARY: CRASH" iff the
// connection is lost (the crash reproduced). Exit 2 for setup errors.
//
// Isolation knobs (env), so once it reproduces we can bisect the exact trigger:
//   ROTATE_CANARY_NO_GRAB=1        -- skip the grab (warp-only): is the GRAB required?
//   ROTATE_CANARY_NO_LOOP_WARP=1   -- skip the recenter-warp LOOP (still does one centering warp).
//   ROTATE_CANARY_NO_CENTER_WARP=1 -- skip the initial centering warp too; with NO_LOOP_WARP a TRUE
//     no-warp grab/ungrab run -- expected to SURVIVE, since the crash is Xwayland's warp callback
//     xwl_cursor_warped_to() null-deref of xwl_seat->focus_window.
//   ROTATE_CANARY_WARPS=N          -- number of recenter warps inside the grab (default 12).
#include <xcb/xcb.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <poll.h>

namespace {

constexpr std::int16_t kWinW = 640;
constexpr std::int16_t kWinH = 480;

int env_int(const char* name, int fallback) {
    const char* v = std::getenv(name);
    if (v == nullptr || v[0] == '\0') {
        return fallback;
    }
    const int n = std::atoi(v);
    return n > 0 ? n : fallback;
}

bool env_set(const char* name) {
    const char* v = std::getenv(name);
    return v != nullptr && v[0] != '\0';
}

// Force a full server round-trip so any deferred crash surfaces now; return true iff the connection
// is still healthy afterward.
bool server_alive(xcb_connection_t* c) {
    if (xcb_connection_has_error(c) != 0) {
        return false;
    }
    xcb_get_input_focus_reply_t* r = xcb_get_input_focus_reply(c, xcb_get_input_focus(c), nullptr);
    if (r == nullptr) {
        return false; // no reply -> the server went away mid-request
    }
    free(r);
    return xcb_connection_has_error(c) == 0;
}

} // namespace

int main() {
    const bool no_grab = env_set("ROTATE_CANARY_NO_GRAB");
    const bool no_loop_warp = env_set("ROTATE_CANARY_NO_LOOP_WARP");
    const bool no_center_warp = env_set("ROTATE_CANARY_NO_CENTER_WARP");
    const int warps = env_int("ROTATE_CANARY_WARPS", 12);

    int screen_num = 0;
    xcb_connection_t* c = xcb_connect(nullptr, &screen_num);
    if (c == nullptr || xcb_connection_has_error(c) != 0) {
        std::printf("ROTATE-CANARY: FAIL (cannot connect to X)\n");
        return 2;
    }
    const xcb_setup_t* setup = xcb_get_setup(c);
    xcb_screen_iterator_t it = xcb_setup_roots_iterator(setup);
    for (int i = 0; i < screen_num; i++) {
        xcb_screen_next(&it);
    }
    xcb_screen_t* screen = it.data;
    const xcb_window_t root = screen->root;

    const xcb_window_t w = xcb_generate_id(c);
    const std::uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
    const std::uint32_t emask = XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_POINTER_MOTION |
                                XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE |
                                XCB_EVENT_MASK_STRUCTURE_NOTIFY;
    const std::uint32_t vals[2] = {screen->black_pixel, emask};
    xcb_create_window(c, XCB_COPY_FROM_PARENT, w, root, 0, 0, static_cast<std::uint16_t>(kWinW),
                      static_cast<std::uint16_t>(kWinH), 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                      screen->root_visual, mask, vals);
    xcb_map_window(c, w);
    xcb_flush(c);

    std::printf("ROTATE-CANARY: xid=%u no_grab=%d no_loop_warp=%d no_center_warp=%d warps=%d\n",
                static_cast<unsigned>(w), no_grab ? 1 : 0, no_loop_warp ? 1 : 0,
                no_center_warp ? 1 : 0, warps);
    std::fflush(stdout);

    // Settle: wait (bounded) for the map to be realized so the pointer/warp target is live.
    const int xfd = xcb_get_file_descriptor(c);
    for (int waited = 0; waited < 2000;) {
        xcb_generic_event_t* ev = nullptr;
        bool mapped = false;
        while ((ev = xcb_poll_for_event(c)) != nullptr) {
            if ((ev->response_type & ~0x80) == XCB_MAP_NOTIFY) {
                mapped = true;
            }
            free(ev);
        }
        if (mapped) {
            break;
        }
        if (!server_alive(c)) {
            std::printf("ROTATE-CANARY: CRASH (X died before the gesture)\n");
            xcb_disconnect(c);
            return 1;
        }
        struct pollfd pfd = {xfd, POLLIN, 0};
        poll(&pfd, 1, 100);
        waited += 100;
    }

    // Take focus and put the pointer inside the window -- the app-side precondition for Xwayland's
    // pointer-warp emulation (a warp destined into a focused surface).
    xcb_set_input_focus(c, XCB_INPUT_FOCUS_PARENT, w, XCB_CURRENT_TIME);
    if (!no_center_warp) {
        xcb_warp_pointer(c, XCB_NONE, w, 0, 0, 0, 0, kWinW / 2, kWinH / 2);
    }
    xcb_flush(c);

    // The gesture: [grab] -> recenter warps -> [ungrab], mirroring a middle-mouse view-rotate.
    if (!no_grab) {
        xcb_grab_pointer_reply_t* g = xcb_grab_pointer_reply(
            c,
            xcb_grab_pointer(c, 0 /*owner_events*/, w,
                             XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE |
                                 XCB_EVENT_MASK_POINTER_MOTION,
                             XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC, XCB_NONE /*confine*/,
                             XCB_NONE /*cursor*/, XCB_CURRENT_TIME),
            nullptr);
        if (g != nullptr) {
            std::printf("ROTATE-CANARY: grab status=%u\n", g->status);
            free(g);
        }
    }

    if (!no_loop_warp) {
        for (int i = 0; i < warps; i++) {
            // Nudge off-center then recenter -- the classic drag-recenter that keeps a warp
            // emulator active every frame.
            xcb_warp_pointer(c, XCB_NONE, w, 0, 0, 0, 0, kWinW / 2 + (i % 2 == 0 ? 20 : -20),
                             kWinH / 2);
            xcb_warp_pointer(c, XCB_NONE, w, 0, 0, 0, 0, kWinW / 2, kWinH / 2);
            xcb_flush(c);
            struct pollfd pfd = {xfd, POLLIN, 0};
            poll(&pfd, 1, 15); // ~frame cadence; also drains nothing critical
            if (!server_alive(c)) {
                std::printf("ROTATE-CANARY: CRASH (X died DURING the warp loop, i=%d)\n", i);
                xcb_disconnect(c);
                return 1;
            }
        }
    }

    if (!no_grab) {
        xcb_ungrab_pointer(c, XCB_CURRENT_TIME); // <-- the trigger point (grab teardown)
        xcb_flush(c);
    }

    // Force the server to process the ungrab and surface any deferred crash.
    if (!server_alive(c)) {
        std::printf("ROTATE-CANARY: CRASH (X connection lost at/after ungrab -- reproduced)\n");
        xcb_disconnect(c);
        return 1;
    }

    std::printf("ROTATE-CANARY: SURVIVED (grab+recenter+ungrab left Xwayland alive)\n");
    std::fflush(stdout);
    xcb_disconnect(c);
    return 0;
}
