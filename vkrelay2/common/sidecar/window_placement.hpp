// vkrelay2 default window placement -- centering + on-screen clamp, pure/common.
//
// A toolkit toplevel that maps without specifying a position (glxgears) lands with its CLIENT at
// the X-root origin (~0,0); the worker places the client there (chrome-correct), so the title bar
// lands ABOVE the screen. The SIDECAR centers
// such a window -- it reads WM_NORMAL_HINTS, and if the app declared no position it configures the
// X window's centered origin BEFORE mapping (so the registry + the X-root coordinate system that
// popup x/y rides stay aligned), then the worker realizes it via the EXISTING register/promote
// placement and applies a safety CLAMP so the outer frame can never land off the work area. These
// are the pure pieces (no X, no Win32), exercised by unit_sidecar on both platforms; the sidecar +
// worker drivers do the I/O.
#ifndef VKRELAY2_COMMON_SIDECAR_WINDOW_PLACEMENT_HPP
#define VKRELAY2_COMMON_SIDECAR_WINDOW_PLACEMENT_HPP

#include <cstddef>
#include <cstdint>

namespace vkr::sidecar {

// ICCCM WM_SIZE_HINTS flags: the app declares whether it specified a window position.
inline constexpr std::uint32_t kWmSizeHintUSPosition = 1u << 0; // user-specified position
inline constexpr std::uint32_t kWmSizeHintPPosition = 1u << 2;  // program-specified position

// True iff the WM_NORMAL_HINTS property payload (a WM_SIZE_HINTS; the first 32-bit word is the
// flags field) declares an explicit window POSITION (USPosition or PPosition). A missing / empty /
// malformed property (n < 1) is treated as NO position intent -> the WM may place the window.
// `words` is the property value reinterpreted as 32-bit words; `n` is the word count.
inline bool wm_hints_has_explicit_position(const std::uint32_t* words, std::size_t n) {
    if (words == nullptr || n < 1) {
        return false;
    }
    return (words[0] & (kWmSizeHintUSPosition | kWmSizeHintPPosition)) != 0;
}

struct ClientOrigin {
    int x = 0;
    int y = 0;
};

// The centered CLIENT origin for a (client_w x client_h) window on a (root_w x root_h) root.
// Clamped to non-negative; if the client is at least as large as the root on an axis, the origin is
// 0 there (the worker clamp then keeps the oversize frame aligned to the work-area edge on the host
// side).
inline ClientOrigin centered_client_origin(int root_w, int root_h, int client_w, int client_h) {
    ClientOrigin o;
    o.x = root_w > client_w ? (root_w - client_w) / 2 : 0;
    o.y = root_h > client_h ? (root_h - client_h) / 2 : 0;
    return o;
}

// Clamp an app/sidecar-authored CLIENT origin (x,y) for a (w x h)
// client on a (root_w x root_h) root so the client top-left stays ON the root -- otherwise the menu
// bar lands above/left of the root and clicks map to negative root coords and miss. Per-axis: if
// the client is at least as large as the root on an axis, pin that axis to 0; else clamp into [0,
// root_size - client_size]. An already on-screen origin (incl. 0,0) is returned unchanged, so
// glxgears / the cursor canary at 0,0 are unaffected. This is the on-screen invariant authored on
// the X SIDE (the coordinate truth pointer injection + popup placement ride),
// NOT a worker-only HWND clamp (which would diverge host vs guest coords and still miss clicks).
// Applied to app/sidecar-authored toplevel geometry (maybe_center_toplevel's explicit-position path
// + XCB_CONFIGURE_REQUEST), NEVER to the Win32-user reverse path (a user may drag partly
// off-screen). Pure -> unit-tested on both platforms.
inline ClientOrigin clamp_client_origin_to_root(int root_w, int root_h, int x, int y, int w,
                                                int h) {
    ClientOrigin o{x, y};
    if (w >= root_w) {
        o.x = 0;
    } else if (o.x < 0) {
        o.x = 0;
    } else if (o.x > root_w - w) {
        o.x = root_w - w;
    }
    if (h >= root_h) {
        o.y = 0;
    } else if (o.y < 0) {
        o.y = 0;
    } else if (o.y > root_h - h) {
        o.y = root_h - h;
    }
    return o;
}

struct FramePos {
    int left = 0;
    int top = 0;
};

// Safety clamp: shift an OUTER frame [fl,ft,fr,fb) so it lies within the work area
// [wl,wt,wr,wb), PRESERVING size; if the frame is at least as large as the work area on an axis,
// align to that edge (top/left) rather than oscillating. Returns the adjusted top-left (size
// unchanged). The normal centered case already fits, so this is a no-op there; it only fires for an
// oversize window or an X-root vs Win32-work-area divergence. Pure -> unit-tested on both
// platforms.
inline FramePos clamp_frame_to_work_area(int fl, int ft, int fr, int fb, int wl, int wt, int wr,
                                         int wb) {
    const int fw = fr - fl;
    const int fh = fb - ft;
    const int ww = wr - wl;
    const int wh = wb - wt;
    FramePos p{fl, ft};
    if (fw >= ww) {
        p.left = wl; // wider than the work area -> pin to the left edge
    } else {
        if (p.left < wl) {
            p.left = wl; // off the left -> push right
        }
        if (p.left + fw > wr) {
            p.left = wr - fw; // off the right -> push left
        }
    }
    if (fh >= wh) {
        p.top = wt; // taller than the work area -> pin to the top edge
    } else {
        if (p.top < wt) {
            p.top = wt; // off the top -> push down
        }
        if (p.top + fh > wb) {
            p.top = wb - fh; // off the bottom -> push up
        }
    }
    return p;
}

// Cross-monitor maximize guard: the capped WM_GETMINMAXINFO limits. ptMaxSize bounds a
// MAXIMIZE; ptMaxTrackSize bounds a drag-RESIZE. Both are OUTER window dims (Win32 fills the struct
// in outer pixels), so the client cap must be converted to an outer cap before comparing.
struct MinMaxCap {
    int max_size_x = 0;
    int max_size_y = 0;
    int max_track_x = 0;
    int max_track_y = 0;
};

// Maximum app-selectable surface extent on one axis. The accepted guest root is the
// session policy authority, while a stable physical-device render-target limit is the hard host
// limit. Do not pass VkSurfaceCapabilitiesKHR::maxImageExtent here on Win32: it tracks the HWND's
// CURRENT extent and is not a stable future-resize ceiling. A zero guest extent is retained only
// for legacy/direct-test construction and falls back to the device limit; it must never trigger a
// fresh desktop/work-area query.
inline std::uint32_t surface_extent_ceiling_axis(std::uint32_t guest_extent,
                                                 std::uint32_t host_max_extent) {
    if (guest_extent == 0) {
        return host_max_extent;
    }
    return guest_extent < host_max_extent ? guest_extent : host_max_extent;
}

// Reduce a window's maximize/drag-resize OUTER limits so the realized CLIENT can never exceed
// (client_cap_w x client_cap_h) -- the guest root, the guest-realizable extent. This is the
// no-hang invariant on the interactive path: on a monitor LARGER than the session's guest root, a
// maximize/resize caps at root size (responsive, not wedged) instead of driving the guest surface
// past its output.
//
// `frame_extra_w/h` are the FIXED outer-minus-client border deltas for the window's style at its
// current realized chrome (the caller measures the live HWND's outer-vs-client deltas on the window
// thread, so PMv1 mixed-DPI chrome cannot disagree with a DPI-predicted adjustment). The `def_*`
// values are the Win32-provided monitor defaults already in MINMAXINFO. Maximize stays
// monitor-specific: ptMaxSize is reduced per axis only when the guest cap is smaller, and
// ptMaxPosition is untouched. Tracking is different: ptMaxTrackSize is set EXACTLY to the root
// outer extent, raising or lowering Win32's virtual-screen-derived default so a root-sized client
// is both realizable and bounded. A non-positive client cap or a negative frame delta passes the
// defaults through unchanged. Pure -> unit-tested on both platforms across chrome variants.
inline MinMaxCap cap_maxsize_to_client(int client_cap_w, int client_cap_h, int frame_extra_w,
                                       int frame_extra_h, int def_max_x, int def_max_y,
                                       int def_track_x, int def_track_y) {
    MinMaxCap c{def_max_x, def_max_y, def_track_x, def_track_y};
    if (client_cap_w <= 0 || client_cap_h <= 0 || frame_extra_w < 0 || frame_extra_h < 0) {
        return c; // guest root unknown or a bogus frame conversion -> never cap (never enlarge)
    }
    const int outer_cap_w = client_cap_w + frame_extra_w;
    const int outer_cap_h = client_cap_h + frame_extra_h;
    if (outer_cap_w < c.max_size_x) {
        c.max_size_x = outer_cap_w;
    }
    if (outer_cap_h < c.max_size_y) {
        c.max_size_y = outer_cap_h;
    }
    c.max_track_x = outer_cap_w;
    c.max_track_y = outer_cap_h;
    return c;
}

} // namespace vkr::sidecar

#endif // VKRELAY2_COMMON_SIDECAR_WINDOW_PLACEMENT_HPP
