// vkrelay2 popup classification sanity predicates.
//
// PURE, dependency-free helpers the sidecar uses to decide whether a self-mapped override-redirect
// window is a real popup worth representing, vs toolkit scratch/helper junk that merely happens to
// be override-redirect (drag icons, IME candidate scratch windows, Tk's ARGB helper, ...). Kept
// here -- not in the sidecar binary -- so they are unit-tested directly (mirrors the icd_subset.h /
// window_registry.hpp pure-predicate pattern). The X-touching parts (InputOnly class, root-sized,
// geometry/name reads) stay in the sidecar; these decide on the already-read dims + name.
#ifndef VKRELAY2_COMMON_SIDECAR_POPUP_CLASSIFY_H
#define VKRELAY2_COMMON_SIDECAR_POPUP_CLASSIFY_H

#include <string>

namespace vkr::sidecar {

// Minimum width/height (px) for a represented popup. Override-redirect helpers (drag icons, IME/
// compositing scratch windows) are typically 1x1 or a few px; a real menu/tooltip/dropdown/combo is
// larger. Below this we drop -- the untyped owner-gated fallback must still be a
// plausible popup, not any tiny override-redirect helper that resolves an owner.
constexpr unsigned kMinPopupDim = 4;

// Is (width,height) a plausible popup size (not zero / not degenerate-tiny)? Root-sized rejection
// needs the screen dims, so it stays in the sidecar; this is the dimension floor.
inline bool popup_size_ok(unsigned width, unsigned height) {
    return width >= kMinPopupDim && height >= kMinPopupDim;
}

// A self-mapped override-redirect window that covers the WHOLE root is not a popup -- it is an
// app FULLSCREEN toplevel. SFML 2.5 (the ExtremeTuxRacer class) maps fullscreen windows
// override-redirect whenever the WM advertises no EWMH, then busy-waits in sf::Window::create for
// a non-obscured VisibilityNotify; rootless Xwayland reports an UNREDIRECTED window FullyObscured
// (nothing is really presented -- the worker shows captured pixels in host windows), so DROPPING
// the window both hides the app on the host AND hangs it forever in that spin. The sidecar must
// register it as a normal (surface-capable) toplevel instead: cap_arm's composite redirect is what
// flips the server's visibility to Unobscured and releases the app.
//
// Named future hardening: this is dimension-only. A root-SIZED
// override-redirect window mapped at a positive offset would classify as fullscreen without
// literally covering the root. No real app has shown that shape; if one does, widen to take (x,y)
// and require x <= 0 && y <= 0 && x + w >= root_w && y + h >= root_h.
inline bool is_fullscreen_toplevel(unsigned width, unsigned height, unsigned root_width,
                                   unsigned root_height) {
    return width >= root_width && height >= root_height;
}

// Known toolkit scratch/helper window markers (matched as a substring of WM_CLASS instance+class):
// override-redirect windows that advertise themselves but are never user-facing popups. This is the
// extensible "named hook" -- grow the list as real apps surface more. `name` is the
// WM_CLASS buffer (instance/class joined; NULs replaced with spaces by the caller).
inline bool is_known_toolkit_junk(const std::string& wm_class) {
    static const char* const kJunkMarkers[] = {
        "x11argb", // Tk's hidden ARGB compositing helper window
    };
    for (const char* marker : kJunkMarkers) {
        if (wm_class.find(marker) != std::string::npos) {
            return true;
        }
    }
    return false;
}

} // namespace vkr::sidecar

#endif // VKRELAY2_COMMON_SIDECAR_POPUP_CLASSIFY_H
