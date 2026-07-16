// vkrelay2 input canary (boundary-smoke helper, WSL/xcb).
//
// A pure-xcb client (NO Vulkan) that maps a normal toplevel, SELECTS pointer/button/key/focus
// input, and advertises WM_DELETE_WINDOW. Under the sidecar WM it is a non-surface toplevel: the
// worker makes a placeholder for it, and run_input_smoke.sh seeds a canonical neutral input
// sequence into the worker's ring (via the sidecar's --debug-inject), which the sidecar drains over
// the wire (PollInput) and injects into THIS window via XTest / focus / WM_DELETE_WINDOW.
//
// It is a STRICT ORACLE: it passes (exit 0, prints "INPUT-CANARY: PASS") ONLY after it
// has received -- as an in-order subsequence -- exactly the canonical discrete sequence
//   FocusIn, ButtonPress(1), ButtonRelease(1), ButtonPress(4 wheel-up), ButtonRelease(4),
//   KeyPress(a), KeyRelease(a), ClientMessage(WM_DELETE_WINDOW)
// and has also seen at least one pointer motion. Out-of-order, missing, or no-motion -> it times
// out and FAILS (exit 1). Motion is order-exempt because XTest warps the pointer before each click,
// so extra MotionNotify events are expected; the discrete events carry the ordering proof.
#include "canary_hints.h" // declare PPosition so the WM keeps the canary at its fixed origin
#include <xcb/xcb.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <poll.h>

namespace {

constexpr std::uint16_t kW = 200;
constexpr std::uint16_t kH = 150;
constexpr std::uint32_t kXKLowerA = 0x61; // XK_a (the injected key 'A' maps to unshifted 'a')
constexpr int kPollSliceMs = 200;
constexpr int kTotalDeadlineMs = 30000;

// The ordered discrete steps the oracle requires (motion is tracked separately, order-exempt).
enum class Step {
    FocusIn,
    Btn1Down,
    Btn1Up,
    Wheel4Down,
    Wheel4Up,
    KeyADown,
    KeyAUp,
    Close,
};

// Cached keyboard mapping so a received keycode -> its column-0 keysym (to verify 'a').
struct Keymap {
    std::uint8_t min_keycode = 0;
    std::uint8_t per = 0;
    std::vector<xcb_keysym_t> syms;
};

std::uint32_t keycode_to_keysym(const Keymap& km, xcb_keycode_t kc) {
    if (km.per == 0 || kc < km.min_keycode) {
        return 0;
    }
    const std::size_t idx = static_cast<std::size_t>(kc - km.min_keycode) * km.per; // column 0
    return idx < km.syms.size() ? km.syms[idx] : 0;
}

} // namespace

int main() {
    int screen_num = 0;
    xcb_connection_t* c = xcb_connect(nullptr, &screen_num);
    if (c == nullptr || xcb_connection_has_error(c)) {
        std::printf("INPUT-CANARY: FAIL (cannot connect to X)\n");
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
    const std::uint32_t emask = XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_POINTER_MOTION |
                                XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE |
                                XCB_EVENT_MASK_KEY_PRESS | XCB_EVENT_MASK_KEY_RELEASE |
                                XCB_EVENT_MASK_FOCUS_CHANGE;
    const std::uint32_t vals[2] = {screen->black_pixel, emask};
    xcb_create_window(c, XCB_COPY_FROM_PARENT, w, screen->root, 0, 0, kW, kH, 0,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, mask, vals);

    // Advertise WM_DELETE_WINDOW so the sidecar's close request reaches us as a ClientMessage.
    xcb_intern_atom_reply_t* rp =
        xcb_intern_atom_reply(c, xcb_intern_atom(c, 0, 12, "WM_PROTOCOLS"), nullptr);
    xcb_intern_atom_reply_t* rd =
        xcb_intern_atom_reply(c, xcb_intern_atom(c, 0, 16, "WM_DELETE_WINDOW"), nullptr);
    xcb_atom_t wm_protocols = rp != nullptr ? rp->atom : 0;
    xcb_atom_t wm_delete = rd != nullptr ? rd->atom : 0;
    free(rp);
    free(rd);
    if (wm_protocols != 0 && wm_delete != 0) {
        xcb_change_property(c, XCB_PROP_MODE_REPLACE, w, wm_protocols, XCB_ATOM_ATOM, 32, 1,
                            &wm_delete);
    }
    vkr::canary::canary_declare_position(c, w); // keep the canary at its fixed origin
    xcb_map_window(c, w);
    xcb_flush(c);

    std::printf("INPUT-CANARY: xid=%u\n", static_cast<unsigned>(w));
    std::fflush(stdout);

    // Cache the keyboard mapping (core xcb) for the key-keysym check.
    Keymap km;
    {
        const int count = static_cast<int>(setup->max_keycode) - setup->min_keycode + 1;
        if (count > 0) {
            xcb_get_keyboard_mapping_reply_t* m = xcb_get_keyboard_mapping_reply(
                c,
                xcb_get_keyboard_mapping(c, setup->min_keycode, static_cast<std::uint8_t>(count)),
                nullptr);
            if (m != nullptr) {
                km.min_keycode = setup->min_keycode;
                km.per = m->keysyms_per_keycode;
                const xcb_keysym_t* syms = xcb_get_keyboard_mapping_keysyms(m);
                const int n = xcb_get_keyboard_mapping_keysyms_length(m);
                if (syms != nullptr && n > 0) {
                    km.syms.assign(syms, syms + n);
                }
                free(m);
            }
        }
    }

    const Step expected[] = {Step::FocusIn,  Step::Btn1Down, Step::Btn1Up, Step::Wheel4Down,
                             Step::Wheel4Up, Step::KeyADown, Step::KeyAUp, Step::Close};
    const std::size_t total = sizeof(expected) / sizeof(expected[0]);
    std::size_t next = 0;
    bool saw_motion = false;

    const int xfd = xcb_get_file_descriptor(c);
    int elapsed = 0;
    while (next < total || !saw_motion) {
        xcb_generic_event_t* ev = nullptr;
        while ((ev = xcb_poll_for_event(c)) != nullptr) {
            const std::uint8_t t = ev->response_type & ~0x80;
            if (t == XCB_MOTION_NOTIFY) {
                saw_motion = true;
            } else if (next < total) {
                const Step want = expected[next];
                bool matched = false;
                if (t == XCB_FOCUS_IN && want == Step::FocusIn) {
                    matched = true;
                } else if (t == XCB_BUTTON_PRESS) {
                    const auto* b = reinterpret_cast<xcb_button_press_event_t*>(ev);
                    matched = (want == Step::Btn1Down && b->detail == 1) ||
                              (want == Step::Wheel4Down && b->detail == 4);
                } else if (t == XCB_BUTTON_RELEASE) {
                    const auto* b = reinterpret_cast<xcb_button_release_event_t*>(ev);
                    matched = (want == Step::Btn1Up && b->detail == 1) ||
                              (want == Step::Wheel4Up && b->detail == 4);
                } else if (t == XCB_KEY_PRESS) {
                    const auto* k = reinterpret_cast<xcb_key_press_event_t*>(ev);
                    matched =
                        (want == Step::KeyADown && keycode_to_keysym(km, k->detail) == kXKLowerA);
                } else if (t == XCB_KEY_RELEASE) {
                    const auto* k = reinterpret_cast<xcb_key_release_event_t*>(ev);
                    matched =
                        (want == Step::KeyAUp && keycode_to_keysym(km, k->detail) == kXKLowerA);
                } else if (t == XCB_CLIENT_MESSAGE) {
                    const auto* m = reinterpret_cast<xcb_client_message_event_t*>(ev);
                    matched = (want == Step::Close && m->data.data32[0] == wm_delete);
                }
                if (matched) {
                    ++next; // in-order subsequence: extra/duplicate events are ignored, not failed
                }
            }
            free(ev);
        }
        if (next >= total && saw_motion) {
            break;
        }
        if (xcb_connection_has_error(c)) {
            std::printf("INPUT-CANARY: FAIL (X connection error; next=%zu motion=%d)\n", next,
                        saw_motion ? 1 : 0);
            xcb_disconnect(c);
            return 1;
        }
        struct pollfd pfd;
        pfd.fd = xfd;
        pfd.events = POLLIN;
        const int pr = poll(&pfd, 1, kPollSliceMs);
        if (pr == 0) {
            elapsed += kPollSliceMs;
            if (elapsed >= kTotalDeadlineMs) {
                std::printf("INPUT-CANARY: FAIL (timeout; matched %zu/%zu, motion=%d)\n", next,
                            total, saw_motion ? 1 : 0);
                xcb_disconnect(c);
                return 1;
            }
        } else if (pr < 0 && errno != EINTR) {
            std::printf("INPUT-CANARY: FAIL (poll error)\n");
            xcb_disconnect(c);
            return 1;
        }
    }

    std::printf("INPUT-CANARY: PASS (received the full canonical input sequence)\n");
    std::fflush(stdout);
    xcb_disconnect(c);
    return 0;
}
