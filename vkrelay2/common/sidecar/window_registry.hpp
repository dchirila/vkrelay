// vkrelay2 worker-home window registry -- a PURE transition engine.
//
// Joins a guest toplevel XID to its worker-side representation. The worker owns the surfaces and
// HWNDs, so it is the only place that can enforce ("one visible host representation per guest
// toplevel"). Both the data-plane backend (which mints surfaces) and the sidecar plane (which
// reports toplevels) feed it, which is why the session backend is a single object spanning both
// planes.
//
// This is a PURE state machine that DECIDES, but never does platform work. Each
// mutator returns a `RegistryEffect` ("create / destroy / promote a placeholder, or nothing"); the
// backend executes the HWND side (RealVulkanBackend against WindowThread; the mock against a fake
// id set). The registry therefore holds representation state {None, Placeholder, Surface} + an
// OPAQUE placeholder id (a monotonic u64 it mints) -- never a raw HWND -- so it compiles and is
// identically on the headless/mock path.
//
// Two independent axes per entry, deliberately separate:
//   - the SURFACE side (bind_surface/unbind_surface, fed by create_surface/destroy_surface on the
//     app data plane): order-independent and SURFACE-SPECIFIC (the fix), never generation-
//     tagged -- a u64 Vulkan surface handle, not a sidecar lifecycle event.
//   - the TOPLEVEL side (register_toplevel/update_toplevel/unregister_toplevel, fed by the
//   sidecar):
//     GENERATION-TAGGED, strictly-newer-wins (generation > entry.generation; <= is dropped as
//     stale), so a re-ordered/recycled-XID destroy cannot remove a freshly re-registered entry.
//
// The representation is the join of the two: a registered toplevel with a bound surface shows the
// surface HWND (Model A); a registered toplevel with no surface shows a gray placeholder; a surface
// with no registered toplevel is a born-correlated pending surface. See the four
// arrival/teardown permutations on the mutators below (the core).
//
// Thread-safety: NOT internally locked. The backend serializes all access under its single backend
// mutex, taken only for the (fast, non-blocking) transition -- never across a WindowThread invoke.
#ifndef VKRELAY2_COMMON_SIDECAR_WINDOW_REGISTRY_HPP
#define VKRELAY2_COMMON_SIDECAR_WINDOW_REGISTRY_HPP

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace vkr::sidecar {

// The host representation of a guest toplevel (exactly one per live toplevel). None = a
// registered toplevel that currently has neither a surface nor a placeholder (e.g. its surface was
// destroyed mid-teardown, permutation 3), OR a pending surface-only entry awaiting its toplevel.
enum class Representation {
    None = 0,
    Placeholder, // a gray aux window the worker created for a non-surface toplevel
    Surface,     // the app's worker-present surface HWND is the representation (Model A)
};

// A toplevel's reported geometry (static initial placement; live authority is applied separately).
struct ToplevelGeometry {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

// The EWMH window-type class of an override-redirect popup. A small token carried on
// the entry / effect / wire for obs + tests + future styling; the sidecar maps each
// _NET_WM_WINDOW_TYPE_* atom to one of these. None = not a popup (an ordinary toplevel).
enum class PopupKind : std::uint32_t {
    None = 0,
    Menu = 1,
    PopupMenu = 2,
    DropdownMenu = 3,
    Tooltip = 4,
    Combo = 5,
};

// A one-shot z-order intent carried on an UpdateToplevel / its effect. Raise = bring
// the host to the TOP of the (non-topmost) toplevel stack (raise-active-toplevel / an X Above
// request); Lower = send it to the bottom (an X Below request). None = no z-order change (a pure
// move/resize). Popups are NOT restacked here -- they ride their Win32 owner (raising the owner
// keeps owned popups above it). No HWND_TOPMOST.
enum class ZOrder : std::uint32_t {
    None = 0,
    Raise = 1,
    Lower = 2,
};

// Show/hide lifecycle: a registered toplevel's live visibility, authored by the
// sidecar (the X map/unmap state) and applied to the host window. `Visible` = mapped (ShowWindow
// SW_SHOWNA, gated on first-paint eligibility); `Hidden` = X-unmapped (SW_HIDE) but STILL
// REPRESENTED -- the HWND, surface binding, chrome DIB, and input epoch all survive, distinguishing
// a temporary hide (minimize / toolkit hide-to-reshow) from an UnregisterToplevel (the X window is
// gone). `Iconic` (a host-user minimize -> a taskbar-restorable SW_SHOWMINNOACTIVE) is carried by
// the enum/wire from day one but only EMITTED later (the Win32-user-origin feedback adds
// it); this path authors only Visible/Hidden. (An enum, not a bool, so a later stage adds
// behavior without a schema/protocol churn.)
enum class VisibilityState : std::uint32_t {
    Visible = 0,
    Hidden = 1,
    Iconic = 2,
};

// What the backend must do to the host after a transition. At most one per transition (the state
// machine never needs to create AND destroy a window in a single step). The backend translates this
// into WindowThread calls (real) or fake-id bookkeeping (mock).
struct RegistryEffect {
    enum class Kind {
        None = 0,
        CreatePlaceholder,  // create a hidden placeholder window for `xid` at `geometry`;
                            // record it under `placeholder_id`
        DestroyPlaceholder, // destroy the placeholder `placeholder_id` (toplevel went away)
        PromotePlaceholderToSurface, // destroy the placeholder `placeholder_id`; `surface` (its
                                     // worker-present HWND) is now the sole representation
        // Live geometry authority: an applied update_toplevel changed the
        // toplevel's sidecar-authored POSITION -> move the host window (surface OR placeholder/
        // popup) to `geometry`, generation/seq-sequenced. Carries `geometry` (the new X-root
        // client geometry) + `host_apply_seq` (a monotonic registry counter the window-thread apply
        // uses as its strictly-newer-wins gate). This is position-only; size + z-order
        // reuse this effect later.
        ApplyGeometry,
        // Show/hide lifecycle: an applied set_visibility CHANGED the toplevel's live
        // visibility -> apply it to the host window (ShowWindow). Carries `visibility_state` (the
        // new state) + `host_apply_seq` (the SAME monotonic counter geometry uses, so a hide/show
        // cannot reorder against a concurrent move at the HWND). The representation is preserved (a
        // hide is NOT a teardown), so this effect never destroys/erases.
        SetVisibility,
    };
    Kind kind = Kind::None;
    // True when the mutator actually changed registry state. A generation-gated op dropped as
    // stale/equal leaves this false (the worker reports it as ok-but-not-applied -- idempotent, not
    // an error). Note: kind==None does NOT imply !applied (e.g. a surface-first bind applies but
    // has no host effect).
    bool applied = false;
    std::uint64_t xid = 0;
    std::uint64_t placeholder_id = 0; // assigned on Create; the target on Destroy/Promote
    std::uint64_t surface = 0;        // the bound surface (Promote)
    ToplevelGeometry geometry;        // Create: where to place the placeholder
    // Popup metadata, meaningful on CreatePlaceholder (and echoed on a cascade
    // DestroyPlaceholder). When is_popup, the backend creates an OWNED WS_POPUP host anchored to
    // `owner_xid`'s host window (the static z-order) at `geometry` (X root coords) instead of an
    // ordinary overlapped placeholder. 0/false for an ordinary toplevel.
    bool is_popup = false;
    std::uint64_t owner_xid = 0;
    std::uint32_t popup_kind = 0;
    // ApplyGeometry only: a monotonic per-session geometry sequence the registry mints
    // for each applied geometry-changing update. The window-thread apply drops a mutation whose seq
    // is older than the last one applied to that HWND (a per-slot last_host_apply_seq) -- the
    // geometry analogue of the paint seq / the input epoch -- so a storm/repost converges to
    // the newest authored geometry and never regresses. 0 on every non-ApplyGeometry effect.
    std::uint64_t host_apply_seq = 0;
    // Whether this geometry apply must SIZE the host client to `geometry`'s w/h (the
    // sidecar is the extent authority for this xid) vs leave the client untouched (a position-only
    // move -- the app still owns its extent). True iff the entry's extent is authoritative at apply
    // time. The window-thread apply uses it to choose SWP_NOSIZE (move) vs a sizing SetWindowPos
    // (resize, which trips the dirty latch). 0/false on a non-geometry effect.
    bool apply_size = false;
    // A one-shot z-order intent for this apply (Raise/Lower/None). The window-thread
    // apply folds it into the same SetWindowPos (hWndInsertAfter = HWND_TOP/HWND_BOTTOM); None
    // keeps the current z-position (SWP_NOZORDER). Carried on an ApplyGeometry effect alongside
    // position/ size.
    ZOrder z_order = ZOrder::None;
    // SetVisibility only: the new live visibility to apply to the host window
    // (Visible -> SW_SHOWNA iff paint-eligible; Hidden -> SW_HIDE; Iconic deferred to a later
    // stage).
    // Visible on every non-SetVisibility effect (ignored there).
    VisibilityState visibility_state = VisibilityState::Visible;
};

// Stable wire/debug name of a representation ("none"|"placeholder"|"surface").
inline const char* representation_name(Representation r) {
    switch (r) {
    case Representation::Placeholder:
        return "placeholder";
    case Representation::Surface:
        return "surface";
    case Representation::None:
        break;
    }
    return "none";
}

class WindowRegistry {
  public:
    struct Entry {
        std::uint64_t xid = 0;
        std::uint64_t surface = 0; // bound VkSurfaceKHR handle (0 = none); surface-specific
        Representation representation = Representation::None;
        std::uint64_t placeholder_id = 0; // valid iff representation == Placeholder
        bool toplevel_registered = false; // the sidecar register_toplevel'd this xid
        std::uint64_t generation = 0;     // highest sidecar lifecycle generation applied
        // Input epoch: a monotonic id of the CURRENT host
        // representation instance. Bumped whenever the representation is (re-)ESTABLISHED -- a new
        // placeholder window (register), a surface bind/promote/overwrite -- but NOT on a mere
        // geometry update. So input captured for an old representation (e.g. before an
        // unregister+re-register of the same XID, or a placeholder->surface promote) is dropped by
        // the worker's EXACT-epoch poll gate, while user input is never lost to a concurrent
        // resize. This is the input-side analogue of the exact-generation paint gate, but keyed on
        // representation identity (not lifecycle generation, which a resize bumps).
        std::uint64_t representation_epoch = 0;
        std::string role;          // sidecar-authored (advisory; debug)
        std::string title;         // best-effort (debug)
        ToplevelGeometry geometry; // sidecar-reported (live authority; the authored extent)
        // Once the sidecar AUTHORS a resize (an update_toplevel that changed w/h), it
        // is the extent authority for this xid -- get_surface_capabilities pins currentExtent
        // to geometry.w/h and create_swapchain refuses a mismatching app extent. False until the
        // first authored resize (the app picks its own extent freely before that);
        // cleared on unregister so a re-register starts app-authored again.
        bool extent_authoritative = false;
        // the last z-order intent the sidecar authored (sticky; the most recent
        // Raise/Lower). Pure registry state reported in DebugEnumWindows -- the worker-visible
        // record that a restack was applied (the actual HWND z-position is asserted in-process via
        // GetWindow). Cleared on unregister.
        ZOrder last_z_order = ZOrder::None;
        // the toplevel's live visibility (X map/unmap state; adds Iconic). Set
        // Visible on register (a map), flipped by set_visibility (UnmapNotify -> Hidden, a tracked
        // remap -> Visible). Deliberately NOT a teardown: a Hidden entry keeps its representation,
        // surface binding, chrome, and representation_epoch, so a restore costs no rebuild.
        // Reported in DebugEnumWindows (the worker-visible proof). Reset to Visible on a
        // (re-)register.
        VisibilityState visibility_state = VisibilityState::Visible;
        // popup representation: an override-redirect popup is a placeholder-class host
        // (no surface) OWNED by + stacked above `owner_xid`'s host window. `is_popup` tags it;
        // `owner_xid` is the resolved live NON-popup z-order anchor; `popup_kind` is the EWMH
        // window type (a PopupKind token; debug/obs/styling). All 0/false for an ordinary toplevel.
        bool is_popup = false;
        std::uint64_t owner_xid = 0;
        std::uint32_t popup_kind = 0;
        // chrome-paint sub-state of a Placeholder (meaningful only while representation ==
        // Placeholder; reset when a fresh placeholder is created). `shown` flips true only AFTER
        // the backend has painted a valid first chrome buffer into the DIB and shown the window
        // (commit_placeholder_paint) -- never on accept alone, so a gray/uninitialized window is
        // never observable. `last_paint_seq` is the highest accepted per-toplevel paint seq.
        bool shown = false;
        std::uint64_t last_paint_seq = 0;
    };

    // The pure decision a chrome paint yields: no pixel bytes ever enter the registry.
    // The backend uses it to drive the window-thread paint, then commits the result.
    struct PlaceholderPaintDecision {
        bool accepted = false; // the paint is valid for the current representation/generation/seq
        bool first_paint =
            false; // this is the first accepted paint -> the backend shows the window
        std::uint64_t placeholder_id = 0; // the placeholder HWND to paint (0 if not accepted)
    };

    // --- Surface side (app data plane: create_surface/destroy_surface)
    // ----------------------------

    // Record the surface born-correlated with `xid` (a no-op for xid == 0). Order-independent:
    //   - no entry yet (surface-first, permutation 1): create a Surface entry -- the worker-present
    //     HWND already exists, so NO placeholder is created. Effect None.
    //   - a Placeholder entry (toplevel-first, permutation 2): PROMOTE -- the surface HWND replaces
    //     the placeholder. Effect PromotePlaceholderToSurface (the backend destroys the
    //     placeholder).
    //   - a None entry (its surface was lost while still registered, permutation 3): re-promote to
    //     Surface (no placeholder existed). Effect None.
    //   - a Surface entry (a second surface for the same xid before the first is destroyed): last-
    //     writer-wins overwrite of the bound handle. Effect None.
    RegistryEffect bind_surface(std::uint64_t xid, std::uint64_t surface) {
        if (xid == 0) {
            return {};
        }
        const auto it = entries_.find(xid);
        if (it == entries_.end()) {
            Entry e;
            e.xid = xid;
            e.surface = surface;
            e.representation = Representation::Surface;
            e.representation_epoch = ++next_epoch_; // a new surface representation (input gate)
            entries_.emplace(xid, e);
            RegistryEffect eff;
            eff.applied =
                true;      // created the surface entry (no host effect: the HWND already exists)
            eff.xid = xid; // every applied effect names its xid (effect contract)
            eff.surface = surface;
            return eff;
        }
        Entry& e = it->second;
        if (e.representation == Representation::Placeholder) {
            RegistryEffect eff;
            eff.kind = RegistryEffect::Kind::PromotePlaceholderToSurface;
            eff.applied = true;
            eff.xid = xid;
            eff.placeholder_id = e.placeholder_id;
            eff.surface = surface;
            e.surface = surface;
            e.representation = Representation::Surface;
            e.placeholder_id = 0;
            e.representation_epoch = ++next_epoch_; // placeholder -> surface: a new representation
            // the NEW surface HWND must inherit the placeholder's CURRENT
            // geometry -- a placeholder already moved to the right X-root position would otherwise
            // be promoted to a CW_USEDEFAULT surface. Stamp the effect so the backend places it.
            stamp_geometry_placement(eff, e);
            return eff;
        }
        // Surface (overwrite) or None (re-promote): record the handle, become Surface, no host
        // work. A new/replacement surface window backs it, so bump the epoch (the old surface's
        // in-flight input is then dropped by the worker's exact-epoch gate).
        e.surface = surface;
        e.representation = Representation::Surface;
        e.representation_epoch = ++next_epoch_;
        RegistryEffect eff;
        eff.applied = true;
        // Name the xid + surface BEFORE stamping: a stamped effect with xid == 0
        // makes the mock record the placement under xid 0, breaking mock == real for the
        // re-promote/overwrite case.
        eff.xid = xid;
        eff.surface = surface;
        // if a toplevel is registered, the new surface HWND inherits the entry's current
        // geometry (a re-promote / surface overwrite for a registered xid); a surface-only entry
        // has no authored geometry yet (the later register places it).
        if (e.toplevel_registered) {
            stamp_geometry_placement(eff, e);
        }
        return eff;
    }

    // Drop the surface side for `xid` (on destroy_surface) IFF its currently-bound surface is still
    // `surface` (surface-specific: destroying an OLDER surface for a recycled XID must not erase a
    // newer/active binding). A no-op for xid == 0, an unknown xid, or a non-matching surface.
    //   - the toplevel is still registered (permutation 3): clear only the surface side; the entry
    //     falls back to Representation::None -- a "toplevel without representation" -- and is kept
    //     until a new surface re-promotes it or unregister_toplevel clears it. NO placeholder is
    //     re-created (that would flash a gray window during the normal teardown order). Effect
    //     None.
    //   - no registered toplevel (a surface-only/pending entry): erase the entry. Effect None.
    // The surface HWND itself is destroyed by destroy_surface in the backend, not by an effect.
    RegistryEffect unbind_surface(std::uint64_t xid, std::uint64_t surface) {
        if (xid == 0) {
            return {};
        }
        const auto it = entries_.find(xid);
        if (it == entries_.end() || it->second.surface != surface) {
            return {};
        }
        Entry& e = it->second;
        e.surface = 0;
        if (e.toplevel_registered) {
            e.representation = Representation::None;
        } else {
            entries_.erase(it);
        }
        RegistryEffect eff;
        eff.applied = true;
        return eff;
    }

    // --- Toplevel side (sidecar plane) -- generation-tagged, strictly-newer-wins -----------------

    // Register (or re-register) a guest toplevel. Dropped (Effect None) when an entry already
    // exists at an equal/greater generation (stale). On apply:
    //   - a surface is already bound (surface-first, permutation 1): annotate role/geometry only --
    //     representation stays Surface, NO placeholder. A RE-registration after a prior unregister
    //     while the surface persisted (permutation-4 remap, a NEW X lifecycle over the same
    //     surface) mints a fresh epoch so stale surface input is dropped; a surface-FIRST first
    //     registration keeps the bind epoch (same representation). Effect None either way.
    //   - no surface yet, and not already a placeholder (toplevel-first, permutation 2): create one
    //     placeholder. Effect CreatePlaceholder (the backend makes a hidden window + records the
    //     id).
    //   - no surface, already a placeholder (a higher-generation re-register): annotate only, no
    //   new
    //     placeholder. Effect None.
    RegistryEffect register_toplevel(std::uint64_t xid, std::uint64_t generation,
                                     const std::string& role, const std::string& title,
                                     const ToplevelGeometry& geometry, bool is_popup = false,
                                     std::uint64_t owner_xid = 0, std::uint32_t popup_kind = 0) {
        if (xid == 0) {
            return {};
        }
        const auto it = entries_.find(xid);
        if (it != entries_.end() && generation <= it->second.generation) {
            return {}; // stale / equal generation
        }
        // an override-redirect popup is represented ONLY if it has a live, registered,
        // NON-popup owner (its z-order anchor). The sidecar resolves the owner before forwarding;
        // this is the registry's double-check. An unresolvable owner -> refuse (applied =
        // false), the popup analogue of the override_redirect refusal (which lived in the
        // backend).
        if (is_popup && !owner_live(owner_xid)) {
            return {};
        }
        // Capture the prior lifecycle state BEFORE overwriting it -- used to detect a
        // re-registration after an unregister (a surface-backed entry the unregister left alive).
        const bool existed = it != entries_.end();
        const bool was_registered = existed && it->second.toplevel_registered;
        const std::uint64_t prior_generation = existed ? it->second.generation : 0;
        Entry& e = (it == entries_.end()) ? entries_[xid] : it->second;
        e.xid = xid;
        e.generation = generation;
        e.toplevel_registered = true;
        e.visibility_state = VisibilityState::Visible; // a (re)map is visible
        e.role = role;
        e.title = title;
        e.geometry = geometry;
        e.is_popup = is_popup;
        e.owner_xid = owner_xid;
        e.popup_kind = popup_kind;
        RegistryEffect eff;
        eff.applied = true;
        eff.xid = xid;
        if (e.surface != 0) {
            e.representation =
                Representation::Surface; // permutation 1: surface wins, no placeholder
            // Re-register after an unregister while the surface persisted (prior generation > 0 +
            // was unregistered) -> a NEW representation lifecycle over the same surface HWND. Mint
            // a fresh epoch so the worker's exact-epoch gate drops surface input captured before
            // the remap (and the backend rebinds the surface slot to this epoch). A surface-FIRST
            // first registration (prior generation 0, never registered) keeps the bind_surface
            // epoch.
            if (existed && !was_registered && prior_generation > 0) {
                e.representation_epoch = ++next_epoch_;
            }
            // place the already-created surface HWND at the (re)registered
            // geometry -- a surface-first toplevel that never self-moves would otherwise stay at
            // the CW_USEDEFAULT surface position. The backend moves the current HWND when
            // host_apply_seq
            // != 0.
            stamp_geometry_placement(eff, e);
            return eff;
        }
        if (e.representation == Representation::Placeholder) {
            // re-register at a higher generation; keep the one placeholder, but re-places it
            // at the (possibly new) registered geometry.
            stamp_geometry_placement(eff, e);
            return eff;
        }
        e.representation = Representation::Placeholder;
        e.placeholder_id = ++next_placeholder_id_;
        e.representation_epoch = ++next_epoch_; // a new placeholder representation (input gate)
        e.shown = false; // a fresh placeholder is unpainted/hidden until its first chrome paint
        e.last_paint_seq = 0;
        eff.kind = RegistryEffect::Kind::CreatePlaceholder;
        eff.placeholder_id = e.placeholder_id;
        eff.geometry = geometry;
        eff.is_popup = is_popup; // an owned WS_POPUP host (vs an overlapped placeholder)
        eff.owner_xid = owner_xid;
        eff.popup_kind = popup_kind;
        // place the new placeholder at the register geometry immediately (initial placement;
        // a map-once-never-move app like vkcube otherwise stays at CW_USEDEFAULT). For a popup this
        // also routes placement through the coordinate helper / work-origin mapping (the
        // create-time raw x,y is superseded).
        stamp_geometry_placement(eff, e);
        return eff;
    }

    // Update a registered toplevel's role/geometry. Generation-gated like register. A no-op for an
    // unknown xid (never creates an entry) or a stale generation.
    //
    // (live geometry authority): an applied update whose POSITION (x/y) OR SIZE
    // (w/h) changed yields an ApplyGeometry effect (carrying the new geometry + a
    // freshly-minted host_apply_seq) so the backend moves/resizes the host window live; a role-only
    // update without geometry still applies but yields Effect::None. A stale/equal-generation
    // update is dropped (Effect::None, applied=false). The representation epoch is deliberately NOT
    // bumped by an update (a move/resize keeps the same representation), so in-flight input
    // survives (the rule).
    //
    // a SIZE change makes the sidecar the EXTENT authority -- it sets
    // `extent_authoritative` so `get_surface_capabilities` reports `currentExtent` = the authored
    // size and `create_swapchain` refuses a mismatching app extent. A pure move (size unchanged)
    // does NOT set it, so the app keeps picking its own extent until a resize is authored.
    // The effect's `apply_size` mirrors the post-update authority so the window-thread apply knows
    // whether to size the client (resize) or leave it (a pure move is position-only, SWP_NOSIZE).
    //
    // a `z_order` (Raise/Lower) is a one-shot restack intent -- it also yields an
    // ApplyGeometry effect (carrying z_order) even when the geometry is unchanged, so a focus-raise
    // or X stack request lands. It rides the same host_apply_seq gate.
    RegistryEffect update_toplevel(std::uint64_t xid, std::uint64_t generation,
                                   const std::string& role, const ToplevelGeometry& geometry,
                                   ZOrder z_order = ZOrder::None) {
        if (xid == 0) {
            return {};
        }
        const auto it = entries_.find(xid);
        if (it == entries_.end() || generation <= it->second.generation) {
            return {};
        }
        const ToplevelGeometry prior = it->second.geometry;
        // A degenerate geometry (zero width/height) means "geometry NOT provided" -- a real
        // toplevel always has a nonzero extent (forward_update fills it from xcb_get_geometry). So
        // a caller sending a pure-restack/role-only UpdateToplevel WITHOUT geometry (the decoder
        // defaults the absent fields to 0) keeps the PRIOR geometry instead of clobbering it to
        // (0,0) / marking the extent authoritative at 0x0 (hardening). Position
        // 0,0 IS valid, so the guard keys on the size, never on x/y.
        const bool has_geometry = (geometry.width > 0 && geometry.height > 0);
        const ToplevelGeometry effective = has_geometry ? geometry : prior;
        it->second.generation = generation;
        if (!role.empty()) {
            it->second.role = role;
        }
        it->second.geometry = effective;
        const bool pos_changed = has_geometry && (effective.x != prior.x || effective.y != prior.y);
        const bool size_changed =
            has_geometry && (effective.width != prior.width || effective.height != prior.height);
        if (size_changed) {
            it->second.extent_authoritative = true; // the sidecar now dictates the extent
        }
        if (z_order != ZOrder::None) {
            it->second.last_z_order = z_order; // sticky last restack (DebugEnumWindows proof)
        }
        RegistryEffect eff;
        eff.applied = true;
        eff.xid = xid;
        if (pos_changed || size_changed || z_order != ZOrder::None) {
            eff.kind = RegistryEffect::Kind::ApplyGeometry;
            eff.geometry = effective;
            eff.host_apply_seq = ++next_host_apply_seq_;
            eff.apply_size = it->second.extent_authoritative;
            eff.z_order = z_order; // fold the restack intent into the apply
        }
        return eff;
    }

    // (show/hide lifecycle, sidecar->worker): set a registered toplevel's live
    // VISIBILITY (the X map/unmap state; will also author Iconic for a host-user minimize).
    // Generation-gated like the other toplevel ops (strictly-newer-wins; a stale/equal generation
    // is dropped, Effect None, applied=false). A no-op for an unknown xid (never creates an entry).
    //
    // On an applied state CHANGE it yields a SetVisibility effect carrying the new state + a
    // freshly-minted host_apply_seq (the SAME monotonic counter geometry uses -- so a hide/show
    // cannot reorder against a concurrent move at the HWND). A no-change apply consumes the
    // generation but yields Effect::None (idempotent). Crucially the representation_epoch is NOT
    // bumped (a hide keeps the same representation -- that is the whole point: the HWND, the
    // surface binding, the chrome DIB, and the input epoch all SURVIVE a hide, unlike an
    // unregister), so in-flight input and the app's swapchain binding persist across hide<->show.
    RegistryEffect set_visibility(std::uint64_t xid, std::uint64_t generation,
                                  VisibilityState state) {
        if (xid == 0) {
            return {};
        }
        const auto it = entries_.find(xid);
        if (it == entries_.end() || generation <= it->second.generation) {
            return {};
        }
        Entry& e = it->second;
        e.generation = generation;
        const bool changed = (e.visibility_state != state);
        e.visibility_state = state;
        RegistryEffect eff;
        eff.applied = true;
        eff.xid = xid;
        if (changed) {
            eff.kind = RegistryEffect::Kind::SetVisibility;
            eff.visibility_state = state;
            eff.host_apply_seq = ++next_host_apply_seq_;
        }
        return eff;
    }

    // Unregister a guest toplevel (its X window DESTROYED -- gone for good; a mere unmap is a
    // set_visibility(Hidden), NOT an unregister). Generation-gated. On apply:
    //   - a placeholder represents it: destroy the placeholder; the entry becomes empty -> erased.
    //     Effect DestroyPlaceholder.
    //   - a surface still represents it (permutation 4): clear ONLY the toplevel side; the live
    //     surface HWND is NOT destroyed by this sidecar event -- it is reaped by unbind_surface on
    //     the real destroy_surface. The entry persists (still has a surface). Effect None.
    //   - None (its surface was already lost): the entry is now empty -> erased. Effect None.
    RegistryEffect unregister_toplevel(std::uint64_t xid, std::uint64_t generation) {
        if (xid == 0) {
            return {};
        }
        const auto it = entries_.find(xid);
        if (it == entries_.end() || generation <= it->second.generation) {
            return {};
        }
        Entry& e = it->second;
        e.generation = generation;
        e.toplevel_registered = false;
        e.role.clear();
        e.title.clear();
        e.geometry = {};
        e.extent_authoritative = false; // the authored extent ends with the X lifecycle (a
                                        // surface kept under permutation 4 reverts to app-authored)
        e.last_z_order = ZOrder::None;  // the restack record ends with the X lifecycle too
        RegistryEffect eff;
        eff.applied = true;
        eff.xid = xid;
        if (e.representation == Representation::Placeholder) {
            eff.kind = RegistryEffect::Kind::DestroyPlaceholder;
            eff.placeholder_id = e.placeholder_id;
            e.representation = Representation::None;
            e.placeholder_id = 0;
            entries_.erase(it); // no surface + no toplevel == empty
            return eff;
        }
        // Surface still bound -> keep the entry (permutation 4); else it is empty -> erase.
        if (e.surface == 0) {
            entries_.erase(it);
        }
        return eff;
    }

    // owner-teardown CASCADE: when an owner toplevel unregisters,
    // its popups must also drop from the registry/obs state -- Win32 owned-window destruction alone
    // would leave orphan logical popups in DebugEnumWindows. Erases every live popup entry whose
    // `owner_xid` matches and returns a DestroyPlaceholder effect per popup that had a placeholder
    // HWND (the backend destroys those HWNDs off-lock; a pending popup with no host yields an
    // applied effect with kind None). The backend calls this AFTER an applied
    // unregister_toplevel(owner_xid); it is a no-op for an xid that owns no popups (e.g. a popup's
    // own unregister, since popups own nothing). Order-independent (a popup never owns another --
    // the sidecar resolves owner_xid to the nearest non-popup).
    std::vector<RegistryEffect> take_orphaned_popups(std::uint64_t owner_xid) {
        std::vector<RegistryEffect> effects;
        if (owner_xid == 0) {
            return effects;
        }
        for (auto it = entries_.begin(); it != entries_.end();) {
            const Entry& e = it->second;
            if (e.is_popup && e.owner_xid == owner_xid) {
                RegistryEffect eff;
                eff.applied = true;
                eff.xid = e.xid;
                eff.is_popup = true;
                eff.owner_xid = owner_xid;
                if (e.representation == Representation::Placeholder) {
                    eff.kind = RegistryEffect::Kind::DestroyPlaceholder;
                    eff.placeholder_id = e.placeholder_id;
                }
                effects.push_back(eff);
                it = entries_.erase(it);
            } else {
                ++it;
            }
        }
        return effects;
    }

    // --- Chrome paint: the accept -> (backend paints) -> commit dance
    // --------------------

    // Decide whether a chrome paint is valid (the GATE; no mutation, no pixel bytes). Accepted iff
    // the xid is currently a Placeholder, the paint's `lifecycle_generation` EXACTLY matches the
    // entry's current generation (not >=, so an old paint cannot leak across an
    // unregister/re-register/ promotion), and `seq` is strictly newer than the last accepted paint.
    // `first_paint` is set when this is the first accepted paint for the placeholder (the backend
    // then shows the window). A paint for a Surface/None/unknown xid, a stale generation, or a
    // non-newer seq is not accepted.
    PlaceholderPaintDecision accept_placeholder_paint(std::uint64_t xid,
                                                      std::uint64_t lifecycle_generation,
                                                      std::uint64_t seq) const {
        PlaceholderPaintDecision d;
        const auto it = entries_.find(xid);
        if (it == entries_.end() || it->second.representation != Representation::Placeholder) {
            return d;
        }
        const Entry& e = it->second;
        if (lifecycle_generation != e.generation || seq <= e.last_paint_seq) {
            return d;
        }
        d.accepted = true;
        d.first_paint = !e.shown;
        d.placeholder_id = e.placeholder_id;
        return d;
    }

    // Commit an accepted paint AFTER the backend has painted a valid buffer into the DIB + shown
    // the window. Re-checks the gate (a racing promote/unregister between accept and commit
    // invalidates it): on success marks `shown` + advances `last_paint_seq` and returns true;
    // otherwise leaves the entry untouched and returns false (the backend discards -- the window is
    // being torn down / promoted by the racing op anyway). So `shown`/`last_paint_seq` reflect ONLY
    // a realized paint.
    bool commit_placeholder_paint(std::uint64_t xid, std::uint64_t lifecycle_generation,
                                  std::uint64_t seq) {
        const auto it = entries_.find(xid);
        if (it == entries_.end() || it->second.representation != Representation::Placeholder) {
            return false;
        }
        Entry& e = it->second;
        if (lifecycle_generation != e.generation || seq <= e.last_paint_seq) {
            return false;
        }
        e.shown = true;
        e.last_paint_seq = seq;
        return true;
    }

    // --- Read-only accessors (debug / the backend's post-effect re-check)
    // -------------------------

    std::uint64_t surface_for_xid(std::uint64_t xid) const {
        const auto it = entries_.find(xid);
        return it == entries_.end() ? 0 : it->second.surface;
    }
    Representation representation_for_xid(std::uint64_t xid) const {
        const auto it = entries_.find(xid);
        return it == entries_.end() ? Representation::None : it->second.representation;
    }
    std::uint64_t placeholder_id_for_xid(std::uint64_t xid) const {
        const auto it = entries_.find(xid);
        return it == entries_.end() ? 0 : it->second.placeholder_id;
    }
    bool toplevel_registered(std::uint64_t xid) const {
        const auto it = entries_.find(xid);
        return it != entries_.end() && it->second.toplevel_registered;
    }
    std::uint64_t generation_for_xid(std::uint64_t xid) const {
        const auto it = entries_.find(xid);
        return it == entries_.end() ? 0 : it->second.generation;
    }
    // the xid's live visibility (Visible if no live entry -- the default). Used by the
    // worker's reveal predicate (intent == Visible && paint-eligible) and the obs proof.
    VisibilityState visibility_state_for_xid(std::uint64_t xid) const {
        const auto it = entries_.find(xid);
        return it == entries_.end() ? VisibilityState::Visible : it->second.visibility_state;
    }
    bool is_popup(std::uint64_t xid) const {
        const auto it = entries_.find(xid);
        return it != entries_.end() && it->second.is_popup;
    }
    // visibility cascade: the live owned popups of `owner_xid`
    // (each with its OWN visibility intent), so the backend re-asserts each popup per its own state
    // on an owner hide/show -- an independently-hidden popup is NOT resurrected by an owner
    // restore. Non-destructive (unlike take_orphaned_popups). Empty for a non-owner / a popup xid.
    struct OwnedPopup {
        std::uint64_t xid = 0;
        VisibilityState visibility_state = VisibilityState::Visible;
    };
    std::vector<OwnedPopup> owned_popups(std::uint64_t owner_xid) const {
        std::vector<OwnedPopup> out;
        if (owner_xid == 0) {
            return out;
        }
        for (const auto& kv : entries_) {
            if (kv.second.is_popup && kv.second.owner_xid == owner_xid) {
                out.push_back({kv.second.xid, kv.second.visibility_state});
            }
        }
        return out;
    }
    // the sidecar-authoritative extent for `xid` (explicit state,
    // not an ad hoc caps tweak). `active` is true only when the sidecar has authored a live resize;
    // `width`/`height` are the AUTHORED size. The real backend keys its caps pin / create_swapchain
    // gate on `active` but uses the host's REALIZABLE currentExtent (the achieved client the resize
    // drove, clamped by Win32), not the raw authored size; the mock has no host
    // clamping so it uses width/height directly (mock == real for an unclamped resize). All-zero/
    // inactive otherwise (the app owns its extent).
    struct AuthoritativeExtent {
        bool active = false;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
    };
    AuthoritativeExtent authoritative_extent_for_xid(std::uint64_t xid) const {
        const auto it = entries_.find(xid);
        if (it == entries_.end() || !it->second.extent_authoritative) {
            return {};
        }
        return {true, it->second.geometry.width, it->second.geometry.height};
    }
    // (GL/zink): the toolkit toplevel's last sidecar-reported geometry for `xid`,
    // regardless of whether it is extent_authoritative (active iff a non-degenerate size is known).
    // A deferred-extent (zink) surface defaults its swapchain to this REAL window size instead of
    // the placeholder default -- so the app renders at its true size, not 256x256 -- without making
    // the sidecar the resize authority (that stays a explicit-resize concern).
    AuthoritativeExtent geometry_for_xid(std::uint64_t xid) const {
        const auto it = entries_.find(xid);
        if (it == entries_.end() || it->second.geometry.width == 0 ||
            it->second.geometry.height == 0) {
            return {};
        }
        return {true, it->second.geometry.width, it->second.geometry.height};
    }
    // input gate: the xid's CURRENT representation epoch (0 if no live
    // entry). The worker stamps each captured event with the producing representation's epoch and
    // drops, at poll time, any event whose epoch != this -- so input survives a geometry update
    // (the epoch is unchanged) but is dropped after an unregister/destroy (epoch 0) OR an
    // unregister+re-register / placeholder->surface promote of the same xid (a NEW epoch). Exact
    // match subsumes liveness: a gone entry's 0 never matches a minted (>=1) event epoch.
    std::uint64_t epoch_for_xid(std::uint64_t xid) const {
        const auto it = entries_.find(xid);
        return it == entries_.end() ? 0 : it->second.representation_epoch;
    }
    // whether a placeholder has been painted + shown, and its highest accepted paint seq.
    bool placeholder_shown(std::uint64_t xid) const {
        const auto it = entries_.find(xid);
        return it != entries_.end() && it->second.shown;
    }
    std::uint64_t last_paint_seq(std::uint64_t xid) const {
        const auto it = entries_.find(xid);
        return it == entries_.end() ? 0 : it->second.last_paint_seq;
    }
    // The backend's post-effect re-check (the mutex dance): after creating a placeholder HWND
    // off-lock, is the entry STILL waiting for exactly this placeholder (not promoted/destroyed by
    // a racing data-plane bind)? If not, the just-created HWND is discarded.
    bool wants_placeholder(std::uint64_t xid, std::uint64_t placeholder_id) const {
        const auto it = entries_.find(xid);
        return it != entries_.end() && it->second.representation == Representation::Placeholder &&
               it->second.placeholder_id == placeholder_id;
    }
    // is `owner_xid` a live, registered, NON-popup toplevel -- a valid popup z-order
    // anchor? Used by the popup register gate (refuse an unowned popup) AND the backend's
    // post-effect re-check (an owner torn down while the popup host was being created off-lock must
    // discard the host).
    bool owner_live(std::uint64_t owner_xid) const {
        if (owner_xid == 0) {
            return false;
        }
        const auto it = entries_.find(owner_xid);
        return it != entries_.end() && it->second.toplevel_registered && !it->second.is_popup;
    }
    // popup post-effect re-check (the off-lock mutex dance): after creating the owned
    // popup host off-lock, does the registry STILL want exactly this placeholder for this xid AND
    // is the owner still live? Either failing -> the backend discards the just-created host. (The
    // sidecar plane is sequential, so an owner teardown cannot actually interleave here -- the
    // owner-live arm is defense in depth mirroring the data-plane placeholder dance.)
    bool wants_popup(std::uint64_t xid, std::uint64_t placeholder_id,
                     std::uint64_t owner_xid) const {
        return wants_placeholder(xid, placeholder_id) && owner_live(owner_xid);
    }

    std::size_t size() const { return entries_.size(); }
    // (DebugEnumWindows): a copy of every entry, sorted by xid (std::map order), for
    // the observability enumeration query. Returns copies so the caller (the backend, building the
    // wire response off-lock-safe under its own mutex) holds no reference into the registry.
    std::vector<Entry> snapshot() const {
        std::vector<Entry> out;
        out.reserve(entries_.size());
        for (const auto& kv : entries_) {
            out.push_back(kv.second);
        }
        return out;
    }
    // Entries whose representation is currently a placeholder (the structural placeholder
    // count).
    std::size_t placeholder_count() const {
        std::size_t n = 0;
        for (const auto& kv : entries_) {
            if (kv.second.representation == Representation::Placeholder) {
                ++n;
            }
        }
        return n;
    }

  private:
    // stamp a representation-establishing/current effect so the backend
    // places the host HWND at the entry's CURRENT geometry, minting a fresh host_apply_seq so the
    // placement orders monotonically against later moves (the strictly-newer-wins gate). Used by
    // register_toplevel (initial placement / surface-first / re-register) and bind_surface
    // (promote / re-promote). The backend treats ANY effect with host_apply_seq != 0 as a move.
    void stamp_geometry_placement(RegistryEffect& eff, const Entry& e) {
        eff.geometry = e.geometry;
        eff.host_apply_seq = ++next_host_apply_seq_;
        // an establishing placement sizes the client only if the sidecar is already the
        // extent authority for this xid (normally false at register -- the app still owns its
        // extent -- so the placement is position-only; true e.g. for a surface that arrives after
        // an authored resize).
        eff.apply_size = e.extent_authoritative;
    }

    std::map<std::uint64_t, Entry> entries_;
    std::uint64_t next_placeholder_id_ =
        0;                         // monotonic; never reused within a session (no stale id)
    std::uint64_t next_epoch_ = 0; // monotonic representation epoch (never reused)
    std::uint64_t next_host_apply_seq_ = 0; // monotonic geometry seq (ApplyGeometry; never reused)
};

} // namespace vkr::sidecar

#endif // VKRELAY2_COMMON_SIDECAR_WINDOW_REGISTRY_HPP
