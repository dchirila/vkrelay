// vkrelay2 canary helper: declare an explicit window POSITION via WM_NORMAL_HINTS.
//
// The sidecar CENTERs a freshly-mapped toplevel that declared NO position (no
// USPosition/PPosition in WM_NORMAL_HINTS). A boundary-smoke canary that maps at a DELIBERATE
// position (and whose smoke asserts that position) must therefore declare PPosition -- otherwise
// the sidecar would (correctly) treat it as unpositioned and center it. This is the proper ICCCM
// behavior
// for a client that wants a specific position; it keeps every existing positioned canary at its
// chosen spot.
#ifndef VKRELAY2_LINUX_SIDECAR_CANARY_HINTS_H
#define VKRELAY2_LINUX_SIDECAR_CANARY_HINTS_H

#include <xcb/xcb.h>

#include <cstdint>

namespace vkr::canary {

// Set WM_NORMAL_HINTS (WM_SIZE_HINTS) with the PPosition flag, declaring this toplevel's map
// position as program-specified. The obsolete x/y/w/h fields stay 0 (a modern WM reads the window's
// actual geometry; vkrelay2's sidecar reads only the flags word). Call after create, before map.
inline void canary_declare_position(xcb_connection_t* c, xcb_window_t w) {
    std::uint32_t hints[18] = {
        0};             // full WM_SIZE_HINTS length; only the flags word is meaningful here
    hints[0] = 1u << 2; // PPosition (program-specified position)
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, w, XCB_ATOM_WM_NORMAL_HINTS,
                        XCB_ATOM_WM_SIZE_HINTS, 32, 18, hints);
}

} // namespace vkr::canary

#endif // VKRELAY2_LINUX_SIDECAR_CANARY_HINTS_H
