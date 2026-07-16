// vkrelay2 geometry coordinate mapping -- the SINGLE X-root <-> Win32
// transform seam.
//
// This makes the sidecar the geometry authority: a guest toplevel's X-root coordinates drive the
// host Win32 window's position. X-root (x,y) means the guest CLIENT/drawable origin: for a
// chromed overlapped toplevel the Win32 FRAME must be offset by the caption/border so
// the CLIENT -- not the frame -- lands at the mapped coordinate; for a WS_POPUP (no chrome) it is
// identity (the popup placement stays correct).
//
// The same 1:1 physical seam generalizes: X-root (0,0) maps to the immutable session
// snapshot's virtual-desktop top-left, which may be negative. The window thread is PMv1-aware, so
// every value here remains in PHYSICAL pixels; DPI is metadata, never another transform.
//
// map_root_client_to_win32_frame is pure given its inputs (it calls only AdjustWindowRectExForDpi),
// so the frame/client math is unit-tested directly (no window needed).
#ifndef VKRELAY2_WINDOWS_WORKER_WINDOWING_COORDINATE_MAP_H
#define VKRELAY2_WINDOWS_WORKER_WINDOWING_COORDINATE_MAP_H

#include <windows.h>

namespace vkr::worker::windowing {

struct FrameInsets {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
    bool valid = false;
};

// Measured-inset authority. Read the actual distance from the HWND frame to each client edge
// on the window thread. This remains correct when a PMv1 window's GetDpiForWindow changes at a
// monitor boundary but its non-client chrome does not scale (the production 96->144 failure), and
// also accounts for DWM/frame behavior that a style+DPI prediction cannot observe.
inline FrameInsets read_real_frame_insets(HWND hwnd) {
    FrameInsets insets;
    RECT frame{};
    RECT client{};
    POINT client_origin{};
    if (hwnd == nullptr || GetWindowRect(hwnd, &frame) == FALSE ||
        GetClientRect(hwnd, &client) == FALSE || ClientToScreen(hwnd, &client_origin) == FALSE) {
        return insets;
    }
    const int client_w = static_cast<int>(client.right - client.left);
    const int client_h = static_cast<int>(client.bottom - client.top);
    insets.left = static_cast<int>(client_origin.x - frame.left);
    insets.top = static_cast<int>(client_origin.y - frame.top);
    insets.right = static_cast<int>(frame.right - (client_origin.x + client_w));
    insets.bottom = static_cast<int>(frame.bottom - (client_origin.y + client_h));
    insets.valid = insets.left >= 0 && insets.top >= 0 && insets.right >= 0 && insets.bottom >= 0;
    return insets;
}

// Pure placement using a previously measured live inset authority.
inline RECT map_root_client_to_win32_frame_with_insets(int root_x, int root_y, int client_w,
                                                       int client_h, POINT work_origin,
                                                       const FrameInsets& insets) {
    const LONG client_x = work_origin.x + root_x;
    const LONG client_y = work_origin.y + root_y;
    return RECT{client_x - insets.left, client_y - insets.top, client_x + client_w + insets.right,
                client_y + client_h + insets.bottom};
}

// Legacy/direct-test origin only. Production gets its origin from the pinned DisplayLayout and
// never calls this environment query from a live mapping path.
inline POINT win32_work_origin() {
    RECT wa{0, 0, 0, 0};
    if (SystemParametersInfo(SPI_GETWORKAREA, 0, &wa, 0)) {
        return POINT{wa.left, wa.top};
    }
    return POINT{0, 0};
}

// Map an X-root CLIENT origin (root_x, root_y) + client size (client_w, client_h) to the Win32
// OUTER window frame RECT, so the window's CLIENT lands at the mapped root coordinate. The mapping
// is 1:1 physical: the client screen origin = work_origin + (root_x, root_y); the frame inset comes
// from AdjustWindowRectExForDpi(style, exstyle, dpi) -- so for WS_POPUP (no chrome) the result is
// identity (frame == client). Pure given (work_origin, style, exstyle, dpi), hence unit-testable.
//
// A position-only move uses ONLY the returned top-left (left/top) for a SWP_NOSIZE move and
// DISCARDS the width/height -- size application is handled by the separate resize path. The full
// RECT is returned so that resize path can reuse this one helper for the size half.
inline RECT map_root_client_to_win32_frame(int root_x, int root_y, int client_w, int client_h,
                                           POINT work_origin, DWORD style, DWORD exstyle,
                                           UINT dpi) {
    // Inset rect for a zero-origin client of (client_w, client_h): after the adjust, left/top are
    // <= 0 (the left/top non-client insets) and right/bottom are client_w/h plus the right/bottom
    // insets. For WS_POPUP this stays {0,0,client_w,client_h} (no insets) -> identity below.
    RECT inset{0, 0, client_w, client_h};
    AdjustWindowRectExForDpi(&inset, style, FALSE, exstyle, dpi);
    const LONG client_x = work_origin.x + root_x;
    const LONG client_y = work_origin.y + root_y;
    RECT outer;
    outer.left = client_x + inset.left;     // client_x - left_inset  (inset.left <= 0)
    outer.top = client_y + inset.top;       // client_y - top_inset   (inset.top  <= 0)
    outer.right = client_x + inset.right;   // client_x + client_w + right_inset
    outer.bottom = client_y + inset.bottom; // client_y + client_h + bottom_inset
    return outer;
}

// An X-root CLIENT geometry: the client origin in X-root coords + the client extent. The result of
// the inverse map (a Win32-user gesture -> a guest request).
struct RootClientGeometry {
    int root_x = 0;
    int root_y = 0;
    int client_w = 0;
    int client_h = 0;
};

// Read a window's ACTUAL client geometry in X-root coords straight from the
// live HWND -- the ground truth of where the client the user sees + clicks actually is. Uses
// GetClientRect (real client extent) + ClientToScreen (real client screen origin) -
// the pinned session origin, so it reflects any Win32/DWM realization (the initial frame clamp, DWM
// invisible borders) that the pure AdjustWindowRectExForDpi inverse cannot see. Use this as
// the LIVE source of truth for the reverse (user/system gesture) + realization-feedback
// paths; keep the pure map_*_win32_frame helpers for planned-placement math + round-trip unit
// tests. MUST be called on the window thread (ClientToScreen scales by the CALLING thread's
// DPI-awareness context; the window thread is PMv1-aware, so this returns PHYSICAL px consistent
// with the placement map).
inline RootClientGeometry read_real_root_client_geometry(HWND hwnd, POINT host_origin) {
    RootClientGeometry g;
    RECT client{};
    POINT origin{0, 0};
    if (GetClientRect(hwnd, &client) && ClientToScreen(hwnd, &origin)) {
        g.root_x = static_cast<int>(origin.x - host_origin.x);
        g.root_y = static_cast<int>(origin.y - host_origin.y);
        g.client_w = static_cast<int>(client.right - client.left);
        g.client_h = static_cast<int>(client.bottom - client.top);
    }
    return g;
}

// The EXACT INVERSE of map_root_client_to_win32_frame -- map a Win32 OUTER frame RECT
// (screen coords) back to the X-root CLIENT origin + extent, so a Win32-user move/resize of the
// host becomes a guest geometry REQUEST in the same space the sidecar authors. The non-client
// insets are constant (caption/border, size-independent), so a zero-rect probe recovers them:
// AdjustWindowRectExForDpi({0,0,0,0}) = {left_inset<=0, top_inset<=0, right_inset>=0,
// bottom_inset>=0}. Then client_w = frame_w - (right_inset - left_inset); client screen origin =
// frame.left - left_inset; root = client_origin - work_origin. For WS_POPUP (no insets) this is
// identity (minus the work origin) -- the inverse of the forward seam's identity. Pure given
// (work_origin, style, exstyle, dpi); the round-trip inverse(forward(p)) == p is unit-tested so the
// two seams cannot drift.
inline RootClientGeometry map_win32_frame_to_root_client(const RECT& frame, POINT work_origin,
                                                         DWORD style, DWORD exstyle, UINT dpi) {
    RECT probe{0, 0, 0, 0};
    AdjustWindowRectExForDpi(&probe, style, FALSE, exstyle, dpi); // {il<=0, it<=0, ir>=0, ib>=0}
    const LONG frame_w = frame.right - frame.left;
    const LONG frame_h = frame.bottom - frame.top;
    const LONG client_w = frame_w - (probe.right - probe.left); // frame_w - (left+right borders)
    const LONG client_h = frame_h - (probe.bottom - probe.top); // frame_h - (caption + bottom)
    const LONG client_x = frame.left - probe.left;              // probe.left <= 0
    const LONG client_y = frame.top - probe.top;                // probe.top  <= 0
    RootClientGeometry g;
    g.root_x = static_cast<int>(client_x - work_origin.x);
    g.root_y = static_cast<int>(client_y - work_origin.y);
    g.client_w = static_cast<int>(client_w < 0 ? 0 : client_w);
    g.client_h = static_cast<int>(client_h < 0 ? 0 : client_h);
    return g;
}

} // namespace vkr::worker::windowing

#endif // VKRELAY2_WINDOWS_WORKER_WINDOWING_COORDINATE_MAP_H
