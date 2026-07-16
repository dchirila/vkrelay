#pragma once

#include <array>
#include <cstdint>

namespace vkr::sidecar {

// A tracked override-redirect popup is guest-geometry-authored for its whole lifetime. Keep the
// remap operations as an executable plan so the sidecar driver and the unit pin share the exact
// ordering: converge the hidden HWND first, publish Visible second, and only then let chrome paint
// reveal it. This prevents a same-XID popup from flashing at its prior host position.
enum class PopupRemapAction : std::uint8_t {
    UpdateGeometry,
    SetVisible,
    PaintChrome,
};

constexpr std::array<PopupRemapAction, 3> popup_remap_actions() {
    return {PopupRemapAction::UpdateGeometry, PopupRemapAction::SetVisible,
            PopupRemapAction::PaintChrome};
}

constexpr bool popup_configure_forwards_geometry(bool tracked, bool is_popup) {
    return tracked && is_popup;
}

} // namespace vkr::sidecar
