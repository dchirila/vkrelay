// the sidecar RPC layer's body codecs. Round-trips the negotiate request /
// response and confirms the tolerant readers default missing fields (forward-compat), the same way
// the vkrpc/protocol decoders do. The dispatcher (serve_sidecar_rpc) + the token gate are exercised
// end to end in integration_sidecar_plane; this keeps the pure body codecs covered dual-platform.
#include "common/sidecar/chrome_recapture.hpp"
#include "common/sidecar/input_queue.hpp"
#include "common/sidecar/popup_classify.h"
#include "common/sidecar/popup_lifecycle.hpp"
#include "common/sidecar/sidecar_session.hpp"
#include "common/sidecar/window_placement.hpp"
#include "common/sidecar/window_registry.hpp"
#include "common/vkrpc/vulkan_session.hpp"
#include "tests/test_assert.hpp"

#include <cstdint>
#include <string>
#include <vector>

using namespace vkr;
using namespace vkr::sidecar;

namespace {

using Kind = RegistryEffect::Kind;

void test_popup_lifecycle_order() {
    using A = PopupRemapAction;
    constexpr std::array<A, 3> actions = popup_remap_actions();
    static_assert(actions[0] == A::UpdateGeometry);
    static_assert(actions[1] == A::SetVisible);
    static_assert(actions[2] == A::PaintChrome);

    VKR_CHECK(popup_configure_forwards_geometry(/*tracked=*/true, /*is_popup=*/true));
    VKR_CHECK(!popup_configure_forwards_geometry(/*tracked=*/true, /*is_popup=*/false));
    VKR_CHECK(!popup_configure_forwards_geometry(/*tracked=*/false, /*is_popup=*/true));
}

void test_negotiate_request_round_trip() {
    SidecarNegotiateRequest req;
    req.protocol_version = 1;
    const SidecarNegotiateRequest back = SidecarNegotiateRequest::from_body(req.to_body());
    VKR_CHECK_EQ(back.protocol_version, 1);

    // A body missing the version reads as 0 (rejected by the floor at the backend, not rounded).
    json::Value empty = json::Value::make_object();
    VKR_CHECK_EQ(SidecarNegotiateRequest::from_body(empty).protocol_version, 0);
}

void test_negotiate_response_round_trip() {
    SidecarNegotiateResponse resp;
    resp.ok = true;
    resp.protocol_version = kSidecarProtocolVersion;
    resp.reason = "ok";
    const SidecarNegotiateResponse back = SidecarNegotiateResponse::from_body(resp.to_body());
    VKR_CHECK_EQ(back.ok, true);
    VKR_CHECK_EQ(back.protocol_version, kSidecarProtocolVersion);
    VKR_CHECK_EQ(back.reason, std::string("ok"));

    // An absent ok reads false (the fail-closed direction).
    json::Value empty = json::Value::make_object();
    VKR_CHECK_EQ(SidecarNegotiateResponse::from_body(empty).ok, false);
}

// the readiness-barrier messages round-trip (caps flags + scan generation + the initial
// toplevel count), with absent fields defaulting (forward-compat).
void test_ready_round_trip() {
    SidecarReadyRequest req;
    req.scan_generation = 7;
    req.has_xcomposite = true;
    req.has_xtest = false;
    req.has_xfixes = true;
    req.initial_toplevels = 3;
    req.root_width = 2560; // the observed guest root rides the readiness barrier
    req.root_height = 1528;
    req.display_snapshot_id = "sup-1/display-2";
    const SidecarReadyRequest back = SidecarReadyRequest::from_body(req.to_body());
    VKR_CHECK_EQ(back.scan_generation, 7);
    VKR_CHECK_EQ(back.has_xcomposite, true);
    VKR_CHECK_EQ(back.has_xtest, false);
    VKR_CHECK_EQ(back.has_xfixes, true);
    VKR_CHECK_EQ(back.initial_toplevels, 3);
    VKR_CHECK_EQ(back.root_width, 2560u);
    VKR_CHECK_EQ(back.root_height, 1528u);
    VKR_CHECK_EQ(back.display_snapshot_id, std::string("sup-1/display-2"));
    // A legacy/empty body reads all-default (no caps, generation 0, root 0x0 -> the worker leaves
    // the cap unset).
    const SidecarReadyRequest empty = SidecarReadyRequest::from_body(json::Value::make_object());
    VKR_CHECK_EQ(empty.scan_generation, 0);
    VKR_CHECK_EQ(empty.has_xcomposite, false);
    VKR_CHECK_EQ(empty.root_width, 0u);
    VKR_CHECK_EQ(empty.root_height, 0u);
    VKR_CHECK(empty.display_snapshot_id.empty());

    SidecarReadyResponse resp;
    resp.ok = true;
    resp.reason = "ok";
    const SidecarReadyResponse rb = SidecarReadyResponse::from_body(resp.to_body());
    VKR_CHECK_EQ(rb.ok, true);
    VKR_CHECK_EQ(rb.reason, std::string("ok"));
    VKR_CHECK_EQ(SidecarReadyResponse::from_body(json::Value::make_object()).ok, false);
}

// the toplevel-registry lifecycle messages round-trip (xid/generation as decimal-string
// u64, geometry + role + the override_redirect classifier hint), absent fields defaulting.
void test_toplevel_messages_round_trip() {
    SidecarRegisterToplevelRequest reg;
    reg.xid = 0x1234;
    reg.generation = 9;
    reg.role = "toplevel";
    reg.title = "hello";
    reg.x = 10;
    reg.y = 20;
    reg.width = 640;
    reg.height = 480;
    reg.override_redirect = true;
    reg.is_popup = true;
    reg.owner_xid = 0x9999;
    reg.popup_kind = static_cast<std::uint32_t>(vkr::sidecar::PopupKind::DropdownMenu);
    const SidecarRegisterToplevelRequest rb =
        SidecarRegisterToplevelRequest::from_body(reg.to_body());
    VKR_CHECK_EQ(rb.xid, static_cast<std::uint64_t>(0x1234));
    VKR_CHECK_EQ(rb.generation, static_cast<std::uint64_t>(9));
    VKR_CHECK_EQ(rb.role, std::string("toplevel"));
    VKR_CHECK_EQ(rb.title, std::string("hello"));
    VKR_CHECK_EQ(rb.x, 10);
    VKR_CHECK_EQ(rb.y, 20);
    VKR_CHECK_EQ(rb.width, static_cast<std::uint32_t>(640));
    VKR_CHECK_EQ(rb.height, static_cast<std::uint32_t>(480));
    VKR_CHECK_EQ(rb.override_redirect, true);
    VKR_CHECK_EQ(rb.is_popup, true);
    VKR_CHECK_EQ(rb.owner_xid, static_cast<std::uint64_t>(0x9999));
    VKR_CHECK_EQ(rb.popup_kind, static_cast<std::uint32_t>(vkr::sidecar::PopupKind::DropdownMenu));
    // A legacy/empty body reads all-default (xid 0, generation 0, not override-redirect, not a
    // popup).
    const SidecarRegisterToplevelRequest empty =
        SidecarRegisterToplevelRequest::from_body(json::Value::make_object());
    VKR_CHECK_EQ(empty.xid, static_cast<std::uint64_t>(0));
    VKR_CHECK_EQ(empty.generation, static_cast<std::uint64_t>(0));
    VKR_CHECK_EQ(empty.override_redirect, false);
    VKR_CHECK_EQ(empty.is_popup, false);
    VKR_CHECK_EQ(empty.owner_xid, static_cast<std::uint64_t>(0));
    VKR_CHECK_EQ(empty.popup_kind, static_cast<std::uint32_t>(0));

    SidecarUpdateToplevelRequest upd;
    upd.xid = 0x55;
    upd.generation = 12;
    upd.role = "dialog";
    upd.width = 200;
    const SidecarUpdateToplevelRequest ub = SidecarUpdateToplevelRequest::from_body(upd.to_body());
    VKR_CHECK_EQ(ub.xid, static_cast<std::uint64_t>(0x55));
    VKR_CHECK_EQ(ub.generation, static_cast<std::uint64_t>(12));
    VKR_CHECK_EQ(ub.role, std::string("dialog"));
    VKR_CHECK_EQ(ub.width, static_cast<std::uint32_t>(200));

    SidecarUnregisterToplevelRequest unr;
    unr.xid = 0x99;
    unr.generation = 30;
    const SidecarUnregisterToplevelRequest unb =
        SidecarUnregisterToplevelRequest::from_body(unr.to_body());
    VKR_CHECK_EQ(unb.xid, static_cast<std::uint64_t>(0x99));
    VKR_CHECK_EQ(unb.generation, static_cast<std::uint64_t>(30));

    SidecarToplevelResponse resp;
    resp.ok = true;
    resp.reason = "ok";
    resp.xid = 0x1234;
    resp.applied = true;
    resp.representation = "placeholder";
    const SidecarToplevelResponse respb = SidecarToplevelResponse::from_body(resp.to_body());
    VKR_CHECK_EQ(respb.ok, true);
    VKR_CHECK_EQ(respb.xid, static_cast<std::uint64_t>(0x1234));
    VKR_CHECK_EQ(respb.applied, true);
    VKR_CHECK_EQ(respb.representation, std::string("placeholder"));
    // An absent representation defaults to "none"; absent applied/ok default false (fail-closed).
    const SidecarToplevelResponse rempty =
        SidecarToplevelResponse::from_body(json::Value::make_object());
    VKR_CHECK_EQ(rempty.representation, std::string("none"));
    VKR_CHECK_EQ(rempty.applied, false);
    VKR_CHECK_EQ(rempty.ok, false);
}

// --- The pure WindowRegistry transition engine: the state machine, tested before
// any HWND/pixels exist. The four arrival/teardown permutations + generation gating + two-XID
// independence + surface-specific unbind. The backend executor (commit 2) merely obeys these.

// Permutation 1: surface first, then register_toplevel -> NO placeholder is ever created.
void test_registry_surface_first() {
    WindowRegistry reg;
    const RegistryEffect e1 = reg.bind_surface(0x10, 0xA1);
    VKR_CHECK(e1.kind == Kind::None);
    VKR_CHECK(reg.representation_for_xid(0x10) == Representation::Surface);
    VKR_CHECK_EQ(reg.surface_for_xid(0x10), static_cast<std::uint64_t>(0xA1));
    VKR_CHECK_EQ(reg.placeholder_count(), static_cast<std::size_t>(0));

    const RegistryEffect e2 = reg.register_toplevel(0x10, 1, "app", "title", {10, 20, 300, 200});
    VKR_CHECK(e2.kind == Kind::None); // surface already represents it -> no placeholder
    VKR_CHECK(reg.representation_for_xid(0x10) == Representation::Surface);
    VKR_CHECK_EQ(reg.placeholder_count(), static_cast<std::size_t>(0));
    VKR_CHECK(reg.toplevel_registered(0x10));
}

// Permutation 2: register_toplevel first -> one placeholder; the surface PROMOTES it.
void test_registry_toplevel_first_promote() {
    WindowRegistry reg;
    const RegistryEffect e1 = reg.register_toplevel(0x20, 1, "app", "t", {0, 0, 640, 480});
    VKR_CHECK(e1.kind == Kind::CreatePlaceholder);
    VKR_CHECK_EQ(e1.xid, static_cast<std::uint64_t>(0x20));
    VKR_CHECK(e1.placeholder_id != 0);
    VKR_CHECK_EQ(e1.geometry.width, static_cast<std::uint32_t>(640));
    VKR_CHECK(reg.representation_for_xid(0x20) == Representation::Placeholder);
    VKR_CHECK_EQ(reg.placeholder_count(), static_cast<std::size_t>(1));
    const std::uint64_t pid = e1.placeholder_id;

    const RegistryEffect e2 = reg.bind_surface(0x20, 0xA2);
    VKR_CHECK(e2.kind == Kind::PromotePlaceholderToSurface);
    VKR_CHECK_EQ(e2.placeholder_id, pid); // the backend destroys THIS placeholder HWND
    VKR_CHECK_EQ(e2.surface, static_cast<std::uint64_t>(0xA2));
    VKR_CHECK(reg.representation_for_xid(0x20) == Representation::Surface);
    VKR_CHECK_EQ(reg.placeholder_count(), static_cast<std::size_t>(0));
    VKR_CHECK_EQ(reg.surface_for_xid(0x20), static_cast<std::uint64_t>(0xA2));
}

// Permutation 3: the surface is destroyed while the toplevel is still registered -> the entry falls
// back to None (no placeholder re-created, no teardown flash); a new surface re-promotes it.
void test_registry_surface_lost_while_registered() {
    WindowRegistry reg;
    reg.register_toplevel(0x30, 1, "a", "t", {});
    reg.bind_surface(0x30, 0xA3);
    VKR_CHECK(reg.representation_for_xid(0x30) == Representation::Surface);

    const RegistryEffect e = reg.unbind_surface(0x30, 0xA3);
    VKR_CHECK(e.kind == Kind::None);
    VKR_CHECK(reg.representation_for_xid(0x30) == Representation::None);
    VKR_CHECK_EQ(reg.surface_for_xid(0x30), static_cast<std::uint64_t>(0));
    VKR_CHECK_EQ(reg.size(), static_cast<std::size_t>(1)); // entry kept (toplevel still registered)
    VKR_CHECK(reg.toplevel_registered(0x30));
    VKR_CHECK_EQ(reg.placeholder_count(), static_cast<std::size_t>(0));

    const RegistryEffect e2 = reg.bind_surface(0x30, 0xA4);
    VKR_CHECK(e2.kind == Kind::None); // re-promote, no placeholder existed
    VKR_CHECK(reg.representation_for_xid(0x30) == Representation::Surface);
    VKR_CHECK_EQ(reg.placeholder_count(), static_cast<std::size_t>(0));
}

// Permutation 4: unregister_toplevel while the surface is live -> clear only the toplevel side; the
// live surface HWND is NOT torn down by a sidecar event. destroy_surface reaps the entry later.
void test_registry_unregister_with_live_surface() {
    WindowRegistry reg;
    reg.bind_surface(0x40, 0xA5);
    reg.register_toplevel(0x40, 1, "a", "t", {});
    VKR_CHECK(reg.representation_for_xid(0x40) == Representation::Surface);

    const RegistryEffect e = reg.unregister_toplevel(0x40, 2);
    VKR_CHECK(e.kind == Kind::None); // surface live -> no destroy effect
    VKR_CHECK(reg.representation_for_xid(0x40) == Representation::Surface);
    VKR_CHECK_EQ(reg.surface_for_xid(0x40), static_cast<std::uint64_t>(0xA5));
    VKR_CHECK_EQ(reg.size(), static_cast<std::size_t>(1));
    VKR_CHECK(!reg.toplevel_registered(0x40));

    const RegistryEffect e2 = reg.unbind_surface(0x40, 0xA5);
    VKR_CHECK(e2.kind == Kind::None);
    VKR_CHECK_EQ(reg.size(), static_cast<std::size_t>(0)); // now empty -> erased
}

// unregister_toplevel of a placeholder toplevel -> DestroyPlaceholder + the entry is erased.
void test_registry_unregister_placeholder() {
    WindowRegistry reg;
    const RegistryEffect e1 = reg.register_toplevel(0x50, 1, "a", "t", {});
    const std::uint64_t pid = e1.placeholder_id;
    const RegistryEffect e2 = reg.unregister_toplevel(0x50, 2);
    VKR_CHECK(e2.kind == Kind::DestroyPlaceholder);
    VKR_CHECK_EQ(e2.placeholder_id, pid);
    VKR_CHECK_EQ(reg.size(), static_cast<std::size_t>(0));
}

// Generation gating: strictly-newer wins; stale/equal register/update/unregister are dropped.
void test_registry_generation_gating() {
    WindowRegistry reg;
    reg.register_toplevel(0x60, 5, "a", "t", {}); // gen 5, placeholder
    VKR_CHECK_EQ(reg.generation_for_xid(0x60), static_cast<std::uint64_t>(5));

    const RegistryEffect stale = reg.register_toplevel(0x60, 3, "b", "t2", {});
    VKR_CHECK(stale.kind == Kind::None);
    VKR_CHECK_EQ(reg.generation_for_xid(0x60), static_cast<std::uint64_t>(5));

    reg.update_toplevel(0x60, 5, "c", {}); // equal generation -> dropped
    VKR_CHECK_EQ(reg.generation_for_xid(0x60), static_cast<std::uint64_t>(5));
    reg.update_toplevel(0x60, 6, "c", {1, 2, 3, 4}); // newer -> applies
    VKR_CHECK_EQ(reg.generation_for_xid(0x60), static_cast<std::uint64_t>(6));

    const RegistryEffect stale_unreg = reg.unregister_toplevel(0x60, 4); // stale -> survives
    VKR_CHECK(stale_unreg.kind == Kind::None);
    VKR_CHECK_EQ(reg.size(), static_cast<std::size_t>(1));
    VKR_CHECK(reg.toplevel_registered(0x60));

    const RegistryEffect newer_unreg = reg.unregister_toplevel(0x60, 7);
    VKR_CHECK(newer_unreg.kind == Kind::DestroyPlaceholder);
    VKR_CHECK_EQ(reg.size(), static_cast<std::size_t>(0));
}

// Two-XID independence: destroying one toplevel's representation leaves the other untouched.
void test_registry_two_xid_independence() {
    WindowRegistry reg;
    reg.register_toplevel(0xA, 1, "a", "t", {});
    reg.register_toplevel(0xB, 1, "b", "t", {});
    VKR_CHECK_EQ(reg.size(), static_cast<std::size_t>(2));
    VKR_CHECK_EQ(reg.placeholder_count(), static_cast<std::size_t>(2));

    const RegistryEffect e = reg.unregister_toplevel(0xA, 2);
    VKR_CHECK(e.kind == Kind::DestroyPlaceholder);
    VKR_CHECK_EQ(reg.size(), static_cast<std::size_t>(1));
    VKR_CHECK(reg.representation_for_xid(0xB) == Representation::Placeholder);
    VKR_CHECK_EQ(reg.placeholder_count(), static_cast<std::size_t>(1));
}

// Surface-specific unbind (the fix, now a pure-registry property): destroying an OLDER surface
// for a recycled XID does not erase the newer/active binding.
void test_registry_surface_specific_unbind() {
    WindowRegistry reg;
    reg.bind_surface(0x70, 0xA1); // surface-only entry (no toplevel)
    reg.bind_surface(0x70, 0xA2); // last-writer-wins rebind
    VKR_CHECK_EQ(reg.surface_for_xid(0x70), static_cast<std::uint64_t>(0xA2));

    reg.unbind_surface(0x70, 0xA1); // destroy the OLDER surface -> no-op
    VKR_CHECK_EQ(reg.surface_for_xid(0x70), static_cast<std::uint64_t>(0xA2));
    reg.unbind_surface(0x70, 0xA2); // destroy the current -> erased (no toplevel)
    VKR_CHECK_EQ(reg.surface_for_xid(0x70), static_cast<std::uint64_t>(0));
    VKR_CHECK_EQ(reg.size(), static_cast<std::size_t>(0));
}

// (pure registry): update_toplevel emits ApplyGeometry only on a POSITION change, with a
// strictly-monotonic host_apply_seq; a same-position update (size/role only) applies but yields
// None for a position-only no-op (size changes are handled separately); a stale/equal generation
// is dropped (None, !applied); an unknown xid is a
// no-op.
void test_registry_apply_geometry() {
    WindowRegistry reg;
    // Register places the new placeholder at its register geometry (initial placement)
    // -- a CreatePlaceholder effect carrying the geometry + a minted host_apply_seq.
    const RegistryEffect r = reg.register_toplevel(0x300, 1, "app", "t", {10, 20, 200, 100});
    VKR_CHECK(r.kind == Kind::CreatePlaceholder);
    VKR_CHECK_EQ(r.geometry.x, 10);
    VKR_CHECK_EQ(r.geometry.y, 20);
    VKR_CHECK(r.host_apply_seq != 0);

    // A position change -> ApplyGeometry carrying the new geometry + a strictly-greater
    // host_apply_seq.
    const RegistryEffect m1 = reg.update_toplevel(0x300, 2, "", {300, 200, 200, 100});
    VKR_CHECK(m1.applied);
    VKR_CHECK(m1.kind == Kind::ApplyGeometry);
    VKR_CHECK_EQ(m1.xid, static_cast<std::uint64_t>(0x300));
    VKR_CHECK_EQ(m1.geometry.x, 300);
    VKR_CHECK_EQ(m1.geometry.y, 200);
    VKR_CHECK(m1.host_apply_seq > r.host_apply_seq);

    // Another move -> host_apply_seq strictly advances again (monotonic; never reused).
    const RegistryEffect m2 = reg.update_toplevel(0x300, 3, "", {305, 200, 200, 100});
    VKR_CHECK(m2.kind == Kind::ApplyGeometry);
    VKR_CHECK(m2.host_apply_seq > m1.host_apply_seq);

    // A same-POSITION update whose SIZE changed yields ApplyGeometry with apply_size=true (
    // the sidecar becomes the extent authority). host_apply_seq strictly advances.
    const RegistryEffect sz = reg.update_toplevel(0x300, 4, "", {305, 200, 640, 480});
    VKR_CHECK(sz.applied);
    VKR_CHECK(sz.kind == Kind::ApplyGeometry);
    VKR_CHECK(sz.apply_size);
    VKR_CHECK(sz.host_apply_seq > m2.host_apply_seq);

    // A no-geometry-change update (role only, identical geometry) applies but yields Effect::None
    // -- no apply, no minted seq.
    const RegistryEffect noop = reg.update_toplevel(0x300, 5, "newrole", {305, 200, 640, 480});
    VKR_CHECK(noop.applied);
    VKR_CHECK(noop.kind == Kind::None);
    VKR_CHECK_EQ(noop.host_apply_seq, static_cast<std::uint64_t>(0));

    // A stale/equal generation is dropped entirely (no apply, no seq) even with a different
    // geometry.
    const RegistryEffect stale = reg.update_toplevel(0x300, 5, "", {999, 999, 640, 480});
    VKR_CHECK(!stale.applied);
    VKR_CHECK(stale.kind == Kind::None);

    // An unknown xid never creates an entry / never moves.
    const RegistryEffect unknown = reg.update_toplevel(0x999, 1, "", {1, 2, 3, 4});
    VKR_CHECK(!unknown.applied);
    VKR_CHECK(unknown.kind == Kind::None);
}

// (pure registry): a representation-establishing transition stamps the
// effect so the backend places the host at the CURRENT geometry -- initial placement (register), a
// promote that inherits the placeholder's (possibly moved) position, and a surface-first toplevel.
void test_registry_place_on_establish() {
    // (a) Initial placement: register a placeholder -> CreatePlaceholder carries geometry + seq.
    {
        WindowRegistry reg;
        const RegistryEffect e = reg.register_toplevel(0x400, 1, "app", "t", {200, 120, 300, 200});
        VKR_CHECK(e.kind == Kind::CreatePlaceholder);
        VKR_CHECK_EQ(e.geometry.x, 200);
        VKR_CHECK_EQ(e.geometry.y, 120);
        VKR_CHECK(e.host_apply_seq != 0);
    }
    // (b) Promote inherits the placeholder's CURRENT (moved) position: register, move, then bind a
    // surface -> the Promote effect carries the MOVED geometry + a fresh seq.
    {
        WindowRegistry reg;
        reg.register_toplevel(0x401, 1, "app", "t", {10, 10, 300, 200});
        reg.update_toplevel(0x401, 2, "", {333, 222, 300, 200}); // moved
        const RegistryEffect promote = reg.bind_surface(0x401, 0xA1);
        VKR_CHECK(promote.kind == Kind::PromotePlaceholderToSurface);
        VKR_CHECK_EQ(promote.geometry.x, 333); // the new surface inherits the moved position
        VKR_CHECK_EQ(promote.geometry.y, 222);
        VKR_CHECK(promote.host_apply_seq != 0);
    }
    // (c) Surface-first: a surface binds before the toplevel registers (no geometry yet -> no
    // placement on bind); the later register at nonzero x/y carries geometry + seq so the backend
    // moves the already-created surface.
    {
        WindowRegistry reg;
        const RegistryEffect bind = reg.bind_surface(0x402, 0xB2); // surface-only entry
        VKR_CHECK(bind.applied);
        VKR_CHECK_EQ(bind.host_apply_seq, static_cast<std::uint64_t>(0)); // no toplevel -> no place
        const RegistryEffect e = reg.register_toplevel(0x402, 1, "app", "t", {150, 90, 300, 200});
        VKR_CHECK(e.applied);
        VKR_CHECK(e.kind == Kind::None); // surface already present -> no placeholder created
        VKR_CHECK_EQ(e.geometry.x, 150); // but it carries geometry + seq to MOVE the surface
        VKR_CHECK_EQ(e.geometry.y, 90);
        VKR_CHECK(e.host_apply_seq != 0);
    }
    // (d) Surface OVERWRITE for a registered xid: a second surface binds while the first is still
    // bound -> the effect names the xid + carries the entry's geometry + a fresh seq (the
    // effect must carry the xid, else the mock records the placement under xid 0).
    {
        WindowRegistry reg;
        reg.register_toplevel(0x403, 1, "app", "t", {50, 60, 256, 256});
        reg.bind_surface(0x403, 0xC1);                           // promote -> Surface
        const RegistryEffect ov = reg.bind_surface(0x403, 0xC2); // overwrite (second surface)
        VKR_CHECK(ov.applied);
        VKR_CHECK(ov.kind == Kind::None);                        // no placeholder create/destroy
        VKR_CHECK_EQ(ov.xid, static_cast<std::uint64_t>(0x403)); // the effect NAMES its xid
        VKR_CHECK_EQ(ov.geometry.x, 50);
        VKR_CHECK_EQ(ov.geometry.y, 60);
        VKR_CHECK(ov.host_apply_seq != 0);
    }
}

// (pure registry): a SIZE change makes the sidecar the extent authority. A pure
// move (x/y only) does NOT; once a resize is authored, the authority sticks (every apply sizes)
// until unregister clears it.
void test_registry_resize_authority() {
    WindowRegistry reg;
    reg.register_toplevel(0x500, 1, "app", "t", {10, 20, 256, 256});
    VKR_CHECK(!reg.authoritative_extent_for_xid(0x500).active); // app owns the extent at register

    // A pure MOVE (x/y change, same w/h) -> ApplyGeometry but apply_size=false, NOT authoritative.
    const RegistryEffect mv = reg.update_toplevel(0x500, 2, "", {300, 200, 256, 256});
    VKR_CHECK(mv.kind == Kind::ApplyGeometry);
    VKR_CHECK(!mv.apply_size);
    VKR_CHECK(!reg.authoritative_extent_for_xid(0x500).active);

    // A SIZE change -> ApplyGeometry apply_size=true; the extent becomes authoritative at the new
    // w/h.
    const RegistryEffect rs = reg.update_toplevel(0x500, 3, "", {300, 200, 512, 384});
    VKR_CHECK(rs.kind == Kind::ApplyGeometry);
    VKR_CHECK(rs.apply_size);
    VKR_CHECK_EQ(rs.geometry.width, static_cast<std::uint32_t>(512));
    VKR_CHECK_EQ(rs.geometry.height, static_cast<std::uint32_t>(384));
    const auto auth = reg.authoritative_extent_for_xid(0x500);
    VKR_CHECK(auth.active);
    VKR_CHECK_EQ(auth.width, static_cast<std::uint32_t>(512));
    VKR_CHECK_EQ(auth.height, static_cast<std::uint32_t>(384));

    // A subsequent pure move keeps the authority (apply_size stays true; the sidecar still owns the
    // extent) at the unchanged authored size.
    const RegistryEffect mv2 = reg.update_toplevel(0x500, 4, "", {350, 250, 512, 384});
    VKR_CHECK(mv2.kind == Kind::ApplyGeometry);
    VKR_CHECK(mv2.apply_size);
    VKR_CHECK_EQ(reg.authoritative_extent_for_xid(0x500).width, static_cast<std::uint32_t>(512));

    // Unregister clears the authority (the app owns the extent again on a re-register).
    reg.unregister_toplevel(0x500, 5);
    VKR_CHECK(!reg.authoritative_extent_for_xid(0x500).active);
}

// (pure registry): a z-order intent (Raise/Lower) yields an ApplyGeometry effect
// carrying z_order -- even with no geometry change -- and the registry records a STICKY
// last_z_order (the most recent restack), reported in DebugEnumWindows. Generation-gated like the
// rest.
void test_registry_z_order() {
    WindowRegistry reg;
    reg.register_toplevel(0x600, 1, "app", "t", {10, 20, 200, 100});
    auto last_z = [&](std::uint64_t xid) {
        for (const WindowRegistry::Entry& e : reg.snapshot()) {
            if (e.xid == xid) {
                return e.last_z_order;
            }
        }
        return ZOrder::None;
    };
    VKR_CHECK(last_z(0x600) == ZOrder::None); // no restack yet

    // A PURE restack (z-only; DEGENERATE/zero geometry, as a caller that omits geometry would send)
    // still yields ApplyGeometry carrying z_order + a minted seq, but the guard
    // keeps the PRIOR geometry -- it does NOT clobber to (0,0)/0x0 and does NOT mark the extent
    // authoritative.
    const RegistryEffect rz = reg.update_toplevel(0x600, 2, "", {0, 0, 0, 0}, ZOrder::Raise);
    VKR_CHECK(rz.applied);
    VKR_CHECK(rz.kind == Kind::ApplyGeometry);
    VKR_CHECK(rz.z_order == ZOrder::Raise);
    VKR_CHECK(rz.host_apply_seq != 0);
    VKR_CHECK_EQ(rz.geometry.x, 10); // prior geometry preserved (not clobbered to 0,0)
    VKR_CHECK_EQ(rz.geometry.width, static_cast<std::uint32_t>(200));
    VKR_CHECK(!reg.authoritative_extent_for_xid(0x600).active); // a z-only request is NOT a resize
    VKR_CHECK(last_z(0x600) == ZOrder::Raise);

    // A subsequent pure move carries no restack intent, but last_z_order stays Raise (sticky).
    const RegistryEffect mv = reg.update_toplevel(0x600, 3, "", {50, 60, 200, 100});
    VKR_CHECK(mv.kind == Kind::ApplyGeometry);
    VKR_CHECK(mv.z_order == ZOrder::None);
    VKR_CHECK(last_z(0x600) == ZOrder::Raise);

    // A Lower updates the sticky record.
    const RegistryEffect lz = reg.update_toplevel(0x600, 4, "", {50, 60, 200, 100}, ZOrder::Lower);
    VKR_CHECK(lz.z_order == ZOrder::Lower);
    VKR_CHECK(last_z(0x600) == ZOrder::Lower);

    // A stale/equal generation is dropped (no restack applied).
    const RegistryEffect stale =
        reg.update_toplevel(0x600, 4, "", {50, 60, 200, 100}, ZOrder::Raise);
    VKR_CHECK(!stale.applied);
    VKR_CHECK(last_z(0x600) == ZOrder::Lower); // unchanged by the dropped update

    // Unregister clears the sticky record.
    reg.unregister_toplevel(0x600, 5);
    VKR_CHECK(last_z(0x600) == ZOrder::None);
}

// set_visibility flips the registry's live visibility (generation-gated,
// strictly-newer-wins), yields a SetVisibility effect on a CHANGE carrying the new state + a minted
// host_apply_seq (the SAME counter geometry uses -- one total order at minting), and CRUCIALLY does
// NOT bump the representation epoch or clear the extent authority (a hide is NOT a teardown). A
// re-register (a fresh map) resets to Visible.
void test_registry_set_visibility() {
    WindowRegistry reg;
    auto vis = [&](std::uint64_t xid) { return reg.visibility_state_for_xid(xid); };

    // Unknown xid: no-op (never creates an entry).
    VKR_CHECK(!reg.set_visibility(0x700, 1, VisibilityState::Hidden).applied);
    VKR_CHECK_EQ(reg.size(), static_cast<std::size_t>(0));

    reg.register_toplevel(0x700, 1, "app", "t", {10, 20, 200, 100});
    const std::uint64_t epoch0 = reg.epoch_for_xid(0x700);
    VKR_CHECK(vis(0x700) == VisibilityState::Visible); // a map is visible
    // Make the extent authoritative (a prior resize) so a hide can be shown to preserve it.
    reg.update_toplevel(0x700, 2, "", {10, 20, 320, 240});
    VKR_CHECK(reg.authoritative_extent_for_xid(0x700).active);

    // Hide -> SetVisibility effect (Hidden, minted seq); epoch + extent authority PRESERVED.
    const RegistryEffect h = reg.set_visibility(0x700, 3, VisibilityState::Hidden);
    VKR_CHECK(h.applied);
    VKR_CHECK(h.kind == Kind::SetVisibility);
    VKR_CHECK(h.visibility_state == VisibilityState::Hidden);
    VKR_CHECK(h.host_apply_seq != 0);
    VKR_CHECK(vis(0x700) == VisibilityState::Hidden);
    VKR_CHECK_EQ(reg.epoch_for_xid(0x700), epoch0);            // epoch UNCHANGED (not a teardown)
    VKR_CHECK(reg.authoritative_extent_for_xid(0x700).active); // extent authority preserved

    // One total order: a subsequent move mints a STRICTLY GREATER seq (shared host_apply counter).
    const RegistryEffect mv = reg.update_toplevel(0x700, 4, "", {30, 40, 320, 240});
    VKR_CHECK(mv.kind == Kind::ApplyGeometry);
    VKR_CHECK(mv.host_apply_seq > h.host_apply_seq);

    // Restore -> SetVisibility (Visible).
    const RegistryEffect s = reg.set_visibility(0x700, 5, VisibilityState::Visible);
    VKR_CHECK(s.kind == Kind::SetVisibility && s.visibility_state == VisibilityState::Visible);
    VKR_CHECK(vis(0x700) == VisibilityState::Visible);

    // A NO-CHANGE visibility op at a higher gen applies (consumes the gen) but yields Effect None.
    const RegistryEffect nc = reg.set_visibility(0x700, 6, VisibilityState::Visible);
    VKR_CHECK(nc.applied && nc.kind == Kind::None);

    // A stale/equal generation is dropped.
    VKR_CHECK(!reg.set_visibility(0x700, 6, VisibilityState::Hidden).applied);
    VKR_CHECK(vis(0x700) == VisibilityState::Visible);

    // Hide, then a re-register (a fresh map) RESETS visibility to Visible.
    reg.set_visibility(0x700, 7, VisibilityState::Hidden);
    VKR_CHECK(vis(0x700) == VisibilityState::Hidden);
    reg.register_toplevel(0x700, 8, "app", "t", {10, 20, 200, 100});
    VKR_CHECK(vis(0x700) == VisibilityState::Visible);
}

// the popup-cascade registry helpers (is_popup + the non-destructive owned_popups,
// each carrying the popup's OWN visibility intent so the backend re-asserts per popup on an owner
// hide/show).
void test_registry_owned_popups() {
    WindowRegistry reg;
    reg.register_toplevel(0x800, 1, "app", "owner", {0, 0, 400, 300});
    reg.register_toplevel(0x801, 1, "popup", "menu", {10, 10, 80, 60}, /*is_popup=*/true,
                          /*owner_xid=*/0x800, static_cast<std::uint32_t>(PopupKind::Menu));
    VKR_CHECK(!reg.is_popup(0x800));
    VKR_CHECK(reg.is_popup(0x801));
    std::vector<WindowRegistry::OwnedPopup> op = reg.owned_popups(0x800);
    VKR_CHECK_EQ(op.size(), static_cast<std::size_t>(1));
    VKR_CHECK_EQ(op[0].xid, static_cast<std::uint64_t>(0x801));
    VKR_CHECK(op[0].visibility_state == VisibilityState::Visible);
    // The popup's own hide is reflected (so an owner restore re-asserts it Hidden -> no
    // resurrection).
    reg.set_visibility(0x801, 2, VisibilityState::Hidden);
    VKR_CHECK(reg.owned_popups(0x800)[0].visibility_state == VisibilityState::Hidden);
    // A popup xid / a non-owner owns nothing.
    VKR_CHECK(reg.owned_popups(0x801).empty());
    VKR_CHECK(reg.owned_popups(0x999).empty());
}

// a classified popup with a LIVE non-popup owner gets a placeholder-class
// representation carrying owner_xid + is_popup + popup_kind; the CreatePlaceholder effect carries
// the same so the backend creates the OWNED host. A popup with no live owner is REFUSED.
void test_registry_popup_register() {
    WindowRegistry reg;
    // An owner toplevel (a placeholder; could equally be a surface).
    reg.register_toplevel(0x100, 1, "app", "owner", {0, 0, 800, 600});
    VKR_CHECK(reg.owner_live(0x100));

    // A popup owned by 0x100 -> accepted, placeholder-class, tagged.
    const RegistryEffect ep =
        reg.register_toplevel(0x200, 1, "popup", "menu", {40, 50, 120, 200}, /*is_popup=*/true,
                              /*owner_xid=*/0x100, static_cast<std::uint32_t>(PopupKind::Menu));
    VKR_CHECK(ep.applied);
    VKR_CHECK(ep.kind == Kind::CreatePlaceholder);
    VKR_CHECK(ep.is_popup);
    VKR_CHECK_EQ(ep.owner_xid, static_cast<std::uint64_t>(0x100));
    VKR_CHECK_EQ(ep.popup_kind, static_cast<std::uint32_t>(PopupKind::Menu));
    VKR_CHECK(reg.representation_for_xid(0x200) == Representation::Placeholder);
    // The popup geometry is its X root placement (carried for the WS_POPUP host).
    VKR_CHECK_EQ(ep.geometry.x, 40);
    VKR_CHECK_EQ(ep.geometry.y, 50);

    // A popup whose owner is unknown -> refused (no anchor). No entry created.
    const RegistryEffect orphan =
        reg.register_toplevel(0x300, 1, "popup", "menu", {0, 0, 10, 10}, /*is_popup=*/true,
                              /*owner_xid=*/0xDEAD, static_cast<std::uint32_t>(PopupKind::Menu));
    VKR_CHECK(!orphan.applied);
    VKR_CHECK(orphan.kind == Kind::None);
    VKR_CHECK(reg.representation_for_xid(0x300) == Representation::None);
    VKR_CHECK_EQ(reg.size(), static_cast<std::size_t>(2)); // owner + the one accepted popup

    // A popup cannot own another popup: owner_live(popup) is false, so a popup owned by 0x200 is
    // refused. (The sidecar resolves transient_for through popups to the nearest non-popup; this is
    // the registry's backstop.)
    VKR_CHECK(!reg.owner_live(0x200));
    const RegistryEffect nested =
        reg.register_toplevel(0x400, 1, "popup", "submenu", {0, 0, 10, 10}, /*is_popup=*/true,
                              /*owner_xid=*/0x200, static_cast<std::uint32_t>(PopupKind::Menu));
    VKR_CHECK(!nested.applied);
}

// owner-teardown cascade: unregistering an owner returns its own effect AND
// take_orphaned_popups returns a DestroyPlaceholder per owned popup (erasing them), leaving a
// sibling toplevel + its own popup untouched.
void test_registry_popup_owner_cascade() {
    WindowRegistry reg;
    reg.register_toplevel(0x100, 1, "app", "ownerA", {}); // owner A (placeholder)
    reg.register_toplevel(0x101, 1, "app", "ownerB", {}); // owner B (placeholder, sibling)
    const RegistryEffect p1 =
        reg.register_toplevel(0x200, 1, "popup", "m1", {0, 0, 10, 10}, true, 0x100,
                              static_cast<std::uint32_t>(PopupKind::Menu)); // A's popup #1
    const RegistryEffect p2 =
        reg.register_toplevel(0x201, 1, "popup", "m2", {0, 0, 10, 10}, true, 0x100,
                              static_cast<std::uint32_t>(PopupKind::Menu)); // A's popup #2
    reg.register_toplevel(0x202, 1, "popup", "mb", {0, 0, 10, 10}, true, 0x101,
                          static_cast<std::uint32_t>(PopupKind::Menu)); // B's popup
    VKR_CHECK_EQ(reg.size(), static_cast<std::size_t>(5));

    // Unregister owner A: its own placeholder is destroyed, then the cascade drops A's two popups.
    const RegistryEffect owner_eff = reg.unregister_toplevel(0x100, 2);
    VKR_CHECK(owner_eff.kind == Kind::DestroyPlaceholder);
    const std::vector<RegistryEffect> orphans = reg.take_orphaned_popups(0x100);
    VKR_CHECK_EQ(orphans.size(), static_cast<std::size_t>(2));
    for (const RegistryEffect& o : orphans) {
        VKR_CHECK(o.kind == Kind::DestroyPlaceholder);
        VKR_CHECK(o.is_popup);
        VKR_CHECK(o.placeholder_id == p1.placeholder_id || o.placeholder_id == p2.placeholder_id);
    }
    // A + its 2 popups gone; B + B's popup remain.
    VKR_CHECK_EQ(reg.size(), static_cast<std::size_t>(2));
    VKR_CHECK(reg.representation_for_xid(0x200) == Representation::None);
    VKR_CHECK(reg.representation_for_xid(0x201) == Representation::None);
    VKR_CHECK(reg.toplevel_registered(0x101));
    VKR_CHECK(reg.representation_for_xid(0x202) == Representation::Placeholder);

    // A non-owner unregister (a popup's own teardown) cascades nothing.
    VKR_CHECK(reg.take_orphaned_popups(0x202).empty());
}

// mock plane (mock == real): the worker accepts a classified popup over the wire,
// exposes it via DebugEnumWindows with is_popup/owner_xid, refuses an unowned popup, accepts an
// override-redirect NON-popup registration (the fullscreen-toplevel class -- SFML 2.5's non-EWMH
// fullscreen, the ExtremeTuxRacer fix), and cascades the popup on owner teardown.
void test_popup_plane_mock() {
    vkrpc::MockVulkanBackend be("NVIDIA T1200 Laptop GPU");

    SidecarRegisterToplevelRequest owner;
    owner.xid = 0x100;
    owner.generation = 1;
    owner.role = "toplevel";
    owner.width = 800;
    owner.height = 600;
    VKR_CHECK(be.register_toplevel(owner).applied);

    // A classified popup owned by 0x100 -> accepted.
    SidecarRegisterToplevelRequest popup;
    popup.xid = 0x200;
    popup.generation = 1;
    popup.override_redirect = true;
    popup.is_popup = true;
    popup.owner_xid = 0x100;
    popup.popup_kind = static_cast<std::uint32_t>(vkr::sidecar::PopupKind::DropdownMenu);
    popup.x = 40;
    popup.y = 50;
    popup.width = 120;
    popup.height = 200;
    VKR_CHECK(be.register_toplevel(popup).applied);

    // An unowned popup (owner not live) -> refused.
    SidecarRegisterToplevelRequest unowned = popup;
    unowned.xid = 0x300;
    unowned.owner_xid = 0xDEAD;
    VKR_CHECK(!be.register_toplevel(unowned).applied);

    // The FULLSCREEN class (the ExtremeTuxRacer fix): an override-redirect window the sidecar
    // deliberately classified as a NON-popup toplevel (SFML 2.5's non-EWMH fullscreen shape) now
    // REGISTERS like any toplevel -- the old refuse-all-non-popup gate stranded it on the
    // create_surface 256x256 default host. Unregistered again below so the popup-linkage enum
    // stays focused on owner + popup.
    SidecarRegisterToplevelRequest fullscreen;
    fullscreen.xid = 0x400;
    fullscreen.generation = 1;
    fullscreen.role = "toplevel";
    fullscreen.override_redirect = true; // is_popup stays false
    fullscreen.width = 2560;
    fullscreen.height = 1528;
    const SidecarToplevelResponse fs = be.register_toplevel(fullscreen);
    VKR_CHECK(fs.applied);
    VKR_CHECK_EQ(fs.representation, std::string("placeholder"));
    SidecarUnregisterToplevelRequest fs_unr;
    fs_unr.xid = 0x400;
    fs_unr.generation = 2;
    VKR_CHECK(be.unregister_toplevel(fs_unr).applied);

    // DebugEnumWindows (the worker-visible proof) shows the owner + popup with the right linkage.
    const SidecarDebugEnumWindowsResponse e = be.debug_enum_windows({});
    VKR_CHECK_EQ(e.windows.size(), static_cast<std::size_t>(2));
    const SidecarWindowInfo* owner_info = nullptr;
    const SidecarWindowInfo* popup_info = nullptr;
    for (const SidecarWindowInfo& w : e.windows) {
        if (w.xid == 0x100) {
            owner_info = &w;
        } else if (w.xid == 0x200) {
            popup_info = &w;
        }
    }
    VKR_CHECK(owner_info != nullptr && !owner_info->is_popup);
    VKR_CHECK(popup_info != nullptr);
    VKR_CHECK(popup_info->is_popup);
    VKR_CHECK_EQ(popup_info->owner_xid, static_cast<std::uint64_t>(0x100));
    VKR_CHECK_EQ(popup_info->popup_kind,
                 static_cast<std::uint32_t>(vkr::sidecar::PopupKind::DropdownMenu));
    VKR_CHECK_EQ(popup_info->representation, std::string("placeholder"));
    // Executor in lockstep: 1 owner placeholder + 1 popup placeholder.
    VKR_CHECK_EQ(be.debug_executor_placeholder_count(), static_cast<std::size_t>(2));

    // Owner teardown cascades the popup: nothing remains (owner + popup both gone).
    SidecarUnregisterToplevelRequest unr;
    unr.xid = 0x100;
    unr.generation = 2;
    VKR_CHECK(be.unregister_toplevel(unr).applied);
    const SidecarDebugEnumWindowsResponse e2 = be.debug_enum_windows({});
    VKR_CHECK(e2.windows.empty());
    VKR_CHECK_EQ(be.debug_executor_placeholder_count(), static_cast<std::size_t>(0));
}

// the pure popup-sanity predicates -- the size floor that keeps the
// untyped owner-gated fallback from representing 1x1 / icon-scratch helpers, and the named
// toolkit-junk hook (Tk x11argb). The X-touching gates (InputOnly, root-sized, the property reads)
// live in the sidecar; these decide on the already-read dims + WM_CLASS, so they are unit-tested
// here.
void test_popup_classify_sanity() {
    // Size floor: zero / degenerate-tiny dropped; a real popup size accepted.
    VKR_CHECK(!popup_size_ok(0, 0));
    VKR_CHECK(!popup_size_ok(1, 1));
    VKR_CHECK(!popup_size_ok(3, 50)); // too narrow
    VKR_CHECK(!popup_size_ok(50, 2)); // too short
    VKR_CHECK(popup_size_ok(kMinPopupDim, kMinPopupDim));
    VKR_CHECK(popup_size_ok(160, 220));
    // Named toolkit-junk hook: Tk's x11argb helper dropped (as a WM_CLASS substring); ordinary
    // classes accepted.
    VKR_CHECK(is_known_toolkit_junk("x11argb x11argb"));
    VKR_CHECK(is_known_toolkit_junk("tk x11argb")); // substring within the joined WM_CLASS
    VKR_CHECK(!is_known_toolkit_junk("someapp SomeApp"));
    VKR_CHECK(!is_known_toolkit_junk(""));
    // Fullscreen-toplevel rule (the ExtremeTuxRacer class): a root-COVERING override-redirect
    // window is an app fullscreen toplevel, registered as a toplevel -- never dropped (dropping
    // stranded SFML 2.5's sf::Window::create busy-wait forever) and never popup-classified.
    VKR_CHECK(is_fullscreen_toplevel(2560, 1528, 2560, 1528));  // exactly root-sized
    VKR_CHECK(is_fullscreen_toplevel(2600, 1600, 2560, 1528));  // larger than root still covers it
    VKR_CHECK(!is_fullscreen_toplevel(2559, 1528, 2560, 1528)); // one px short on either axis
    VKR_CHECK(!is_fullscreen_toplevel(2560, 1527, 2560, 1528)); //   -> stays in the popup lane
    VKR_CHECK(!is_fullscreen_toplevel(400, 300, 2560, 1528));   // an ordinary popup size
}

// Build a valid PaintChrome request filling the whole source with one BGRA color.
SidecarPaintChromeRequest make_paint(std::uint64_t xid, std::uint64_t gen, std::uint64_t seq,
                                     std::uint32_t w, std::uint32_t h, unsigned char b,
                                     unsigned char g, unsigned char r, unsigned char a) {
    SidecarPaintChromeRequest req;
    req.xid = xid;
    req.lifecycle_generation = gen;
    req.seq = seq;
    req.src_w = w;
    req.src_h = h;
    req.dirty_x = 0;
    req.dirty_y = 0;
    req.dirty_w = w;
    req.dirty_h = h;
    req.stride = w * 4;
    req.format = kAuxChromeFormatBgra8;
    req.pixels.resize(static_cast<std::size_t>(w) * h * 4);
    for (std::size_t i = 0; i < req.pixels.size(); i += 4) {
        req.pixels[i + 0] = static_cast<char>(b);
        req.pixels[i + 1] = static_cast<char>(g);
        req.pixels[i + 2] = static_cast<char>(r);
        req.pixels[i + 3] = static_cast<char>(a);
    }
    return req;
}

// the PaintChrome binary framing round-trips, and the response/query messages round-trip.
void test_chrome_messages_round_trip() {
    const SidecarPaintChromeRequest req = make_paint(0x1234, 7, 3, 4, 2, 0x10, 0x20, 0x30, 0xFF);
    std::string err;
    const SidecarPaintChromeRequest back = SidecarPaintChromeRequest::from_wire(req.to_wire(), err);
    VKR_CHECK(err.empty());
    VKR_CHECK_EQ(back.xid, static_cast<std::uint64_t>(0x1234));
    VKR_CHECK_EQ(back.lifecycle_generation, static_cast<std::uint64_t>(7));
    VKR_CHECK_EQ(back.seq, static_cast<std::uint64_t>(3));
    VKR_CHECK_EQ(back.src_w, static_cast<std::uint32_t>(4));
    VKR_CHECK_EQ(back.src_h, static_cast<std::uint32_t>(2));
    VKR_CHECK_EQ(back.stride, static_cast<std::uint32_t>(16));
    VKR_CHECK_EQ(back.pixels.size(), static_cast<std::size_t>(32));
    VKR_CHECK_EQ(static_cast<unsigned char>(back.pixels[1]), static_cast<unsigned char>(0x20));

    SidecarPaintResponse presp;
    presp.ok = true;
    presp.xid = 0x1234;
    presp.applied = true;
    presp.representation = "placeholder";
    presp.shown = true;
    presp.last_seq = 9;
    const SidecarPaintResponse pb = SidecarPaintResponse::from_body(presp.to_body());
    VKR_CHECK(pb.ok && pb.applied && pb.shown);
    VKR_CHECK_EQ(pb.representation, std::string("placeholder"));
    VKR_CHECK_EQ(pb.last_seq, static_cast<std::uint64_t>(9));

    SidecarDebugChromeStateRequest dq;
    dq.xid = 0x55;
    dq.sample_x = 12;
    dq.sample_y = 7;
    const SidecarDebugChromeStateRequest dqb =
        SidecarDebugChromeStateRequest::from_body(dq.to_body());
    VKR_CHECK_EQ(dqb.xid, static_cast<std::uint64_t>(0x55));
    VKR_CHECK_EQ(dqb.sample_x, 12);
    VKR_CHECK_EQ(dqb.sample_y, 7);

    SidecarDebugChromeStateResponse dr;
    dr.ok = true;
    dr.xid = 0x55;
    dr.representation = "placeholder";
    dr.shown = true;
    dr.last_seq = 4;
    dr.has_pixel = true;
    dr.pixel_bgra = 0xFF302010u;
    const SidecarDebugChromeStateResponse drb =
        SidecarDebugChromeStateResponse::from_body(dr.to_body());
    VKR_CHECK(drb.ok && drb.shown && drb.has_pixel);
    VKR_CHECK_EQ(drb.pixel_bgra, static_cast<std::uint32_t>(0xFF302010u));
}

// A root-sized placeholder can exceed RpcChannel's 16 MiB frame while remaining below the chrome
// backing-store cap. It must therefore split into bounded row bands; the old single-payload policy
// passed the 96 MiB check and killed the sidecar with an uncaught FrameError.
void test_chrome_wire_tiling_bounds() {
    constexpr std::uint32_t width = 7600;
    constexpr std::uint32_t height = 2160;
    const std::uint64_t source_bytes = static_cast<std::uint64_t>(width) * height * 4;
    VKR_CHECK(source_bytes > vkr::protocol::kMaxFrameBytes);
    VKR_CHECK(source_bytes <= kMaxAuxChromeBackingStoreBytes);

    const std::uint32_t rows = max_aux_chrome_rows_per_wire(width);
    VKR_CHECK(rows > 0);
    VKR_CHECK(rows < height);
    VKR_CHECK(static_cast<std::uint64_t>(rows) * width * 4 <= kMaxAuxChromeWirePayloadBytes);
    VKR_CHECK(static_cast<std::uint64_t>(rows + 1) * width * 4 > kMaxAuxChromeWirePayloadBytes);
    const std::uint32_t bands = (height + rows - 1) / rows;
    VKR_CHECK(bands > 1);

    // Ordinary frames remain one band, and invalid widths fail closed.
    VKR_CHECK(max_aux_chrome_rows_per_wire(2560) >= 1494);
    VKR_CHECK_EQ(max_aux_chrome_rows_per_wire(0), static_cast<std::uint32_t>(0));
}

// Test the decoder discipline: every malformed body is rejected BEFORE the paint path.
void test_chrome_decoder_discipline() {
    std::string err;
    // A short body (no length prefix) is rejected.
    (void) SidecarPaintChromeRequest::from_wire("ab", err);
    VKR_CHECK(!err.empty());

    // Unknown format.
    SidecarPaintChromeRequest bad = make_paint(1, 1, 1, 2, 2, 0, 0, 0, 0xFF);
    bad.format = 99;
    (void) SidecarPaintChromeRequest::from_wire(bad.to_wire(), err);
    VKR_CHECK(!err.empty());

    // Zero dimension.
    bad = make_paint(1, 1, 1, 2, 2, 0, 0, 0, 0xFF);
    bad.dirty_w = 0;
    bad.pixels.clear();
    (void) SidecarPaintChromeRequest::from_wire(bad.to_wire(), err);
    VKR_CHECK(!err.empty());

    // Dirty rect outside the source (wide-arithmetic bounds).
    bad = make_paint(1, 1, 1, 4, 4, 0, 0, 0, 0xFF);
    bad.dirty_x = 2;
    bad.dirty_w = 4; // 2 + 4 > 4
    bad.stride = 16;
    bad.pixels.assign(static_cast<std::size_t>(16) * bad.dirty_h, '\0');
    (void) SidecarPaintChromeRequest::from_wire(bad.to_wire(), err);
    VKR_CHECK(!err.empty());

    // Stride smaller than a dirty row.
    bad = make_paint(1, 1, 1, 4, 2, 0, 0, 0, 0xFF);
    bad.stride = 4; // < dirty_w(4) * 4
    bad.pixels.assign(static_cast<std::size_t>(4) * 2, '\0');
    (void) SidecarPaintChromeRequest::from_wire(bad.to_wire(), err);
    VKR_CHECK(!err.empty());

    // payload_size (== pixels.size()) does not match stride * dirty_h.
    bad = make_paint(1, 1, 1, 4, 2, 0, 0, 0, 0xFF);
    bad.stride = 16;
    bad.pixels.assign(16, '\0'); // expected 32
    (void) SidecarPaintChromeRequest::from_wire(bad.to_wire(), err);
    VKR_CHECK(!err.empty());

    // Tail truncation: a well-framed body with the last byte chopped.
    const std::string good = make_paint(1, 1, 1, 2, 2, 0, 0, 0, 0xFF).to_wire();
    (void) SidecarPaintChromeRequest::from_wire(good.substr(0, good.size() - 1), err);
    VKR_CHECK(!err.empty());

    // A tiny dirty rect must not smuggle an absurd SOURCE size past the payload cap (the worker
    // allocates src_w*src_h*4). A source dim above INT_MAX is rejected...
    bad = make_paint(1, 1, 1, 2, 2, 0, 0, 0, 0xFF);
    bad.src_w = 0xFFFFFFFFu; // > INT_MAX
    bad.src_h = 1;
    bad.dirty_x = 0;
    bad.dirty_y = 0;
    bad.dirty_w = 1;
    bad.dirty_h = 1;
    bad.stride = 4;
    bad.pixels.assign(4, '\0');
    (void) SidecarPaintChromeRequest::from_wire(bad.to_wire(), err);
    VKR_CHECK(!err.empty());

    // ...and the source product (each dim <= INT_MAX, but src_w*src_h*4 over the 96 MiB cap) too.
    bad = make_paint(1, 1, 1, 2, 2, 0, 0, 0, 0xFF);
    bad.src_w = 0x7FFFFFFFu;
    bad.src_h = 0x7FFFFFFFu;
    bad.dirty_x = 0;
    bad.dirty_y = 0;
    bad.dirty_w = 1;
    bad.dirty_h = 1;
    bad.stride = 4;
    bad.pixels.assign(4, '\0');
    (void) SidecarPaintChromeRequest::from_wire(bad.to_wire(), err);
    VKR_CHECK(!err.empty());

    // An out-of-range signed origin in the JSON header is rejected, not narrowed. to_wire cannot
    // emit this from the int32 field, so craft the body by hand: dirty_x = 2^32 must NOT truncate
    // to 0 and pass the dirty-rect checks.
    const std::string hdr =
        "{\"xid\":\"1\",\"lifecycle_generation\":\"1\",\"seq\":\"1\",\"src_w\":\"8\",\"src_h\":"
        "\"8\","
        "\"dirty_x\":4294967296,\"dirty_y\":0,\"dirty_w\":\"1\",\"dirty_h\":\"1\",\"stride\":\"4\","
        "\"format\":1,\"payload_size\":\"4\"}";
    std::string crafted(4, '\0');
    const std::uint32_t hlen = static_cast<std::uint32_t>(hdr.size());
    crafted[0] = static_cast<char>(hlen & 0xFF);
    crafted[1] = static_cast<char>((hlen >> 8) & 0xFF);
    crafted[2] = static_cast<char>((hlen >> 16) & 0xFF);
    crafted[3] = static_cast<char>((hlen >> 24) & 0xFF);
    crafted += hdr;
    crafted += std::string(4, '\0'); // the 4-byte BGRA tail (payload_size = stride*dirty_h = 4)
    (void) SidecarPaintChromeRequest::from_wire(crafted, err);
    VKR_CHECK(!err.empty());
    // The SAME body with an in-range origin (dirty_x:0) decodes clean -- the check is not
    // over-broad.
    std::string ok_hdr = hdr;
    const std::string from = "\"dirty_x\":4294967296";
    ok_hdr.replace(ok_hdr.find(from), from.size(), "\"dirty_x\":0");
    std::string okframe(4, '\0');
    const std::uint32_t olen = static_cast<std::uint32_t>(ok_hdr.size());
    okframe[0] = static_cast<char>(olen & 0xFF);
    okframe[1] = static_cast<char>((olen >> 8) & 0xFF);
    okframe[2] = static_cast<char>((olen >> 16) & 0xFF);
    okframe[3] = static_cast<char>((olen >> 24) & 0xFF);
    okframe += ok_hdr;
    okframe += std::string(4, '\0');
    (void) SidecarPaintChromeRequest::from_wire(okframe, err);
    VKR_CHECK(err.empty());

    // A well-formed body still decodes clean (the discipline is not over-broad).
    (void) SidecarPaintChromeRequest::from_wire(good, err);
    VKR_CHECK(err.empty());
}

// the pure registry accept -> commit gate (the state machine only, no pixels).
void test_registry_chrome_accept_commit() {
    WindowRegistry reg;
    reg.register_toplevel(0x10, 5, "a", "t", {0, 0, 100, 80}); // placeholder, gen 5

    // First accepted paint is `first_paint`; shown flips only on commit (not on accept) --.
    const WindowRegistry::PlaceholderPaintDecision d1 = reg.accept_placeholder_paint(0x10, 5, 1);
    VKR_CHECK(d1.accepted && d1.first_paint && d1.placeholder_id != 0);
    VKR_CHECK(!reg.placeholder_shown(0x10));
    VKR_CHECK(reg.commit_placeholder_paint(0x10, 5, 1));
    VKR_CHECK(reg.placeholder_shown(0x10));
    VKR_CHECK_EQ(reg.last_paint_seq(0x10), static_cast<std::uint64_t>(1));

    // A second paint is accepted but not first_paint.
    const WindowRegistry::PlaceholderPaintDecision d2 = reg.accept_placeholder_paint(0x10, 5, 2);
    VKR_CHECK(d2.accepted && !d2.first_paint);
    VKR_CHECK(reg.commit_placeholder_paint(0x10, 5, 2));
    VKR_CHECK_EQ(reg.last_paint_seq(0x10), static_cast<std::uint64_t>(2));

    // Stale seq dropped (strictly-newer); wrong generation dropped (EXACT match, not >=).
    VKR_CHECK(!reg.accept_placeholder_paint(0x10, 5, 2).accepted); // seq <= last
    VKR_CHECK(!reg.accept_placeholder_paint(0x10, 6, 3).accepted); // gen 6 != 5
    VKR_CHECK(!reg.accept_placeholder_paint(0x10, 4, 3).accepted); // gen 4 != 5
    VKR_CHECK(!reg.accept_placeholder_paint(0x99, 1, 1).accepted); // unknown xid

    // A Surface representation is never paint-accepted.
    reg.bind_surface(0x20, 0x5000); // surface-first -> Surface
    VKR_CHECK(!reg.accept_placeholder_paint(0x20, reg.generation_for_xid(0x20), 1).accepted);

    // Promote race: accept a placeholder paint, promote it, then commit must FAIL (re-check).
    reg.register_toplevel(0x30, 3, "a", "t", {});
    VKR_CHECK(reg.accept_placeholder_paint(0x30, 3, 1).accepted);
    reg.bind_surface(0x30, 0x6000); // promote -> Surface before the commit lands
    VKR_CHECK(!reg.commit_placeholder_paint(0x30, 3, 1));

    // A fresh placeholder (after unregister + re-register) resets shown/last_seq.
    reg.register_toplevel(0x40, 1, "a", "t", {});
    reg.accept_placeholder_paint(0x40, 1, 1);
    reg.commit_placeholder_paint(0x40, 1, 1);
    VKR_CHECK(reg.placeholder_shown(0x40));
    reg.unregister_toplevel(0x40, 2); // erased
    reg.register_toplevel(0x40, 3, "a", "t", {});
    VKR_CHECK(!reg.placeholder_shown(0x40));
    VKR_CHECK_EQ(reg.last_paint_seq(0x40), static_cast<std::uint64_t>(0));
}

// --- input plane ------------------------------------------------------------------

// Build a neutral input event of `kind` with a couple of payload fields set (the rest default).
SidecarInputEvent make_input(InputEventKind kind, int x = 0, int y = 0, int button = 0,
                             int wheel = 0, int vk = 0, bool pressed = false) {
    SidecarInputEvent e;
    e.kind = static_cast<std::uint32_t>(kind);
    e.client_x = x;
    e.client_y = y;
    e.button = button;
    e.wheel = wheel;
    e.vk = vk;
    e.pressed = pressed;
    return e;
}

// The neutral event + PollInput/DebugEnqueueInput messages round-trip (xid/epoch/seq as decimal-
// string u64; the rest tolerant-decoded), absent fields defaulting.
void test_input_messages_round_trip() {
    SidecarInputEvent e;
    e.xid = 0xABCD;
    e.epoch = 4;
    e.seq = 9;
    e.kind = static_cast<std::uint32_t>(InputEventKind::Button);
    e.client_x = -3; // signed: a click just off the client edge is legal
    e.client_y = 17;
    e.button = kInputButtonRight;
    e.vk = 0x41;
    e.scancode = 30;
    e.modifiers = kInputModShift | kInputModCtrl;
    e.pressed = true;
    const SidecarInputEvent eb = SidecarInputEvent::from_value(e.to_value());
    VKR_CHECK_EQ(eb.xid, static_cast<std::uint64_t>(0xABCD));
    VKR_CHECK_EQ(eb.epoch, static_cast<std::uint64_t>(4));
    VKR_CHECK_EQ(eb.seq, static_cast<std::uint64_t>(9));
    VKR_CHECK_EQ(eb.kind, static_cast<std::uint32_t>(InputEventKind::Button));
    VKR_CHECK_EQ(eb.client_x, -3);
    VKR_CHECK_EQ(eb.client_y, 17);
    VKR_CHECK_EQ(eb.button, kInputButtonRight);
    VKR_CHECK_EQ(eb.vk, 0x41);
    VKR_CHECK_EQ(eb.scancode, 30);
    VKR_CHECK_EQ(eb.modifiers, static_cast<std::uint32_t>(kInputModShift | kInputModCtrl));
    VKR_CHECK(eb.pressed);

    // a GeometryRequest host event round-trips its explicit X-root fields (incl. a
    // negative origin) + the host_request sub-kind.
    SidecarInputEvent g;
    g.xid = 0x55;
    g.epoch = 3;
    g.kind = static_cast<std::uint32_t>(InputEventKind::GeometryRequest);
    g.host_request = static_cast<std::uint32_t>(HostRequestKind::Geometry);
    g.root_x = -12;
    g.root_y = 34;
    g.req_w = 640;
    g.req_h = 480;
    const SidecarInputEvent gb = SidecarInputEvent::from_value(g.to_value());
    VKR_CHECK_EQ(gb.kind, static_cast<std::uint32_t>(InputEventKind::GeometryRequest));
    VKR_CHECK_EQ(gb.host_request, static_cast<std::uint32_t>(HostRequestKind::Geometry));
    VKR_CHECK_EQ(gb.root_x, -12);
    VKR_CHECK_EQ(gb.root_y, 34);
    VKR_CHECK_EQ(gb.req_w, static_cast<std::uint32_t>(640));
    VKR_CHECK_EQ(gb.req_h, static_cast<std::uint32_t>(480));

    SidecarPollInputRequest pr;
    pr.since_seq = 42;
    VKR_CHECK_EQ(SidecarPollInputRequest::from_body(pr.to_body()).since_seq,
                 static_cast<std::uint64_t>(42));

    SidecarPollInputResponse resp;
    resp.ok = true;
    resp.reason = "ok";
    resp.next_seq = 7;
    resp.dropped = true;
    resp.events.push_back(e);
    const SidecarPollInputResponse rb = SidecarPollInputResponse::from_body(resp.to_body());
    VKR_CHECK(rb.ok && rb.dropped);
    VKR_CHECK_EQ(rb.next_seq, static_cast<std::uint64_t>(7));
    VKR_CHECK_EQ(rb.events.size(), static_cast<std::size_t>(1));
    VKR_CHECK_EQ(rb.events[0].vk, 0x41);
    // An empty/legacy body reads fail-closed (no events, ok false).
    const SidecarPollInputResponse rempty =
        SidecarPollInputResponse::from_body(json::Value::make_object());
    VKR_CHECK(!rempty.ok);
    VKR_CHECK_EQ(rempty.events.size(), static_cast<std::size_t>(0));

    SidecarDebugEnqueueInputRequest dq;
    dq.xid = 0x10;
    dq.epoch = 2;
    dq.events.push_back(e);
    const SidecarDebugEnqueueInputRequest dqb =
        SidecarDebugEnqueueInputRequest::from_body(dq.to_body());
    VKR_CHECK_EQ(dqb.xid, static_cast<std::uint64_t>(0x10));
    VKR_CHECK_EQ(dqb.epoch, static_cast<std::uint64_t>(2));
    VKR_CHECK_EQ(dqb.events.size(), static_cast<std::size_t>(1));

    SidecarDebugEnqueueInputResponse dr;
    dr.ok = true;
    dr.xid = 0x10;
    dr.enqueued = 5;
    const SidecarDebugEnqueueInputResponse drb =
        SidecarDebugEnqueueInputResponse::from_body(dr.to_body());
    VKR_CHECK(drb.ok);
    VKR_CHECK_EQ(drb.enqueued, 5);

    // The toplevel response now carries the representation epoch.
    SidecarToplevelResponse tr;
    tr.ok = true;
    tr.xid = 0x10;
    tr.applied = true;
    tr.representation = "placeholder";
    tr.epoch = 12;
    const SidecarToplevelResponse trb = SidecarToplevelResponse::from_body(tr.to_body());
    VKR_CHECK_EQ(trb.epoch, static_cast<std::uint64_t>(12));
}

// The ring: FIFO order, monotonic seq, the since_seq cursor, xid-0 / invalid-kind ignored.
void test_input_queue_basic() {
    InputQueue q;
    q.enqueue(0x10, 1, make_input(InputEventKind::Focus, 0, 0, 0, 0, 0, true));
    q.enqueue(0x10, 1, make_input(InputEventKind::Button, 5, 6, kInputButtonLeft, 0, 0, true));
    std::vector<SidecarInputEvent> out;
    std::uint64_t next = 0;
    const bool dropped = q.drain(0, out, next);
    VKR_CHECK(!dropped);
    VKR_CHECK_EQ(out.size(), static_cast<std::size_t>(2));
    VKR_CHECK_EQ(out[0].xid, static_cast<std::uint64_t>(0x10));
    VKR_CHECK_EQ(out[0].epoch, static_cast<std::uint64_t>(1));
    VKR_CHECK(static_cast<InputEventKind>(out[0].kind) == InputEventKind::Focus);
    VKR_CHECK(out[0].seq < out[1].seq); // monotonic
    VKR_CHECK_EQ(next, out[1].seq);
    // A re-drain at the advanced cursor returns nothing (no re-delivery).
    out.clear();
    q.drain(next, out, next);
    VKR_CHECK_EQ(out.size(), static_cast<std::size_t>(0));
    // xid 0 and an out-of-range/Invalid kind are ignored by the producer.
    q.enqueue(0, 1, make_input(InputEventKind::Button, 0, 0, kInputButtonLeft, 0, 0, true));
    SidecarInputEvent bad;
    bad.kind = 999;
    q.enqueue(0x10, 1, bad);
    q.enqueue(0x10, 1, make_input(InputEventKind::Invalid));
    VKR_CHECK_EQ(q.size(), static_cast<std::size_t>(0));
}

// Motion coalescing (latest-pending-motion-per-XID) + non-motion priority -- the
// MOTION-FLOOD case: at most ONE pending motion per xid ever, regardless of non-motion events
// between runs; the latest motion lands LAST (the pointer ends at the newest position); every
// non-motion event survives.
void test_input_queue_motion_coalescing() {
    InputQueue q;
    for (int i = 0; i < 100; ++i) {
        q.enqueue(0x10, 1, make_input(InputEventKind::Motion, i, i));
    }
    VKR_CHECK_EQ(q.size(), static_cast<std::size_t>(1)); // 100 consecutive motions -> 1
    q.enqueue(0x10, 1, make_input(InputEventKind::Button, 99, 99, kInputButtonLeft, 0, 0, true));
    for (int i = 0; i < 50; ++i) {
        q.enqueue(0x10, 1, make_input(InputEventKind::Motion, 100 + i, 0));
    }
    // The second run REPLACES the first run's pending motion (same xid), so the ring is just
    // [button, motion(latest)] -- only ONE motion for the xid, now AFTER the button.
    VKR_CHECK_EQ(q.size(), static_cast<std::size_t>(2));
    std::vector<SidecarInputEvent> out;
    std::uint64_t next = 0;
    q.drain(0, out, next);
    VKR_CHECK_EQ(out.size(), static_cast<std::size_t>(2));
    VKR_CHECK(static_cast<InputEventKind>(out[0].kind) == InputEventKind::Button);
    VKR_CHECK(static_cast<InputEventKind>(out[1].kind) == InputEventKind::Motion);
    VKR_CHECK_EQ(out[1].client_x, 149); // the single surviving motion is the latest position

    // Two different xids keep their OWN single pending motion (independent coalescing).
    InputQueue q2;
    q2.enqueue(0xA, 1, make_input(InputEventKind::Motion, 1, 1));
    q2.enqueue(0xB, 1, make_input(InputEventKind::Motion, 2, 2));
    q2.enqueue(0xA, 1, make_input(InputEventKind::Motion, 3, 3)); // replaces xid A's pending motion
    VKR_CHECK_EQ(q2.size(), static_cast<std::size_t>(2));         // one per xid
}

// Overflow priority: a flood past the cap drops disposable MOTION first and never the Close at the
// front (pre-code guardrail). Each motion uses a DISTINCT xid so per-xid
// coalescing does not collapse them and the ring genuinely fills.
void test_input_queue_overflow_priority() {
    InputQueue q;
    const std::uint64_t close_xid = 0x7FFFFFFFull; // disjoint from the motion xid range below
    q.enqueue(close_xid, 1, make_input(InputEventKind::Close));
    for (std::size_t i = 0; i < kMaxInputQueueEvents + 64; ++i) {
        q.enqueue(i + 1, 1, make_input(InputEventKind::Motion, static_cast<int>(i), 0)); // distinct
    }
    VKR_CHECK(q.size() <= kMaxInputQueueEvents); // bounded
    // Drain everything (in <= kMaxPollInputEvents chunks) and assert the Close survived + is first.
    std::vector<SidecarInputEvent> out;
    std::uint64_t next = 0;
    bool dropped = false;
    bool have_first = false;
    bool first_is_close = false;
    bool close_seen = false;
    for (;;) {
        out.clear();
        const bool d = q.drain(next, out, next);
        dropped = dropped || d;
        if (out.empty()) {
            break;
        }
        if (!have_first) {
            have_first = true;
            first_is_close = static_cast<InputEventKind>(out[0].kind) == InputEventKind::Close;
        }
        for (const auto& e : out) {
            if (static_cast<InputEventKind>(e.kind) == InputEventKind::Close) {
                close_seen = true;
            }
        }
    }
    VKR_CHECK(dropped);        // an overflow drop happened
    VKR_CHECK(close_seen);     // the Close (intent) was never discarded
    VKR_CHECK(first_is_close); // and stayed at the front (oldest)
}

// the protected (non-disposable) guarantee made real in the data structure. (a) A
// motion FLOOD around a GeometryRequest never drops the request (or any protected event). (b) An
// all-protected overload past the soft cap is ADMITTED (never silently dropped) up to the absolute
// hard ceiling, with the overflow diagnostic set. Drains in <= kMaxPollInputEvents chunks.
void test_input_queue_protected_overflow() {
    // (a) One GeometryRequest, then a huge motion flood (distinct xids so no coalescing) -> the
    // request survives (disposable motion is dropped first, never the protected request).
    {
        InputQueue q;
        const std::uint64_t geo_xid = 0x6FFFFFFFull;
        SidecarInputEvent g = make_input(InputEventKind::GeometryRequest);
        g.host_request = static_cast<std::uint32_t>(HostRequestKind::Geometry);
        g.root_x = 77;
        g.root_y = 88;
        q.enqueue(geo_xid, 1, g);
        for (std::size_t i = 0; i < kMaxInputQueueEvents + 256; ++i) {
            q.enqueue(i + 1, 1, make_input(InputEventKind::Motion, static_cast<int>(i), 0));
        }
        VKR_CHECK(q.size() <= kMaxInputQueueEvents); // motion flood stayed bounded
        std::size_t geo = 0, found_x = 0;
        std::vector<SidecarInputEvent> out;
        std::uint64_t next = 0;
        for (;;) {
            out.clear();
            q.drain(next, out, next);
            if (out.empty()) {
                break;
            }
            for (const auto& e : out) {
                if (static_cast<InputEventKind>(e.kind) == InputEventKind::GeometryRequest) {
                    ++geo;
                    found_x = static_cast<std::size_t>(e.root_x);
                }
            }
        }
        VKR_CHECK_EQ(geo, static_cast<std::size_t>(1)); // the request survived the flood
        VKR_CHECK_EQ(found_x, static_cast<std::size_t>(77));
    }

    // (b) Fill the ring entirely with PROTECTED events (no Motion to evict), past the soft cap ->
    // all admitted (never silently dropped), bounded by the absolute hard ceiling; the overflow
    // diagnostic fires.
    {
        InputQueue q;
        const std::size_t n = kMaxInputQueueEvents + 200; // past the soft cap, under the ceiling
        for (std::size_t i = 0; i < n; ++i) {
            q.enqueue(i + 1, 1,
                      make_input(InputEventKind::Button, 0, 0, kInputButtonLeft, 0, 0,
                                 true)); // distinct xids: no coalescing
        }
        VKR_CHECK(q.size() > kMaxInputQueueEvents);       // admitted past the soft cap
        VKR_CHECK(q.size() <= kMaxInputQueueHardCeiling); // bounded by the absolute ceiling
        std::size_t buttons = 0;
        bool overflow_diag = false;
        std::vector<SidecarInputEvent> out;
        std::uint64_t next = 0;
        for (;;) {
            out.clear();
            overflow_diag = q.drain(next, out, next) || overflow_diag;
            if (out.empty()) {
                break;
            }
            for (const auto& e : out) {
                if (static_cast<InputEventKind>(e.kind) == InputEventKind::Button) {
                    ++buttons;
                }
            }
        }
        VKR_CHECK_EQ(buttons, n); // EVERY protected event survived (never silently dropped)
        VKR_CHECK(overflow_diag); // the overflow was RECORDED (never silent)
    }
}

// The worker-backend input plane through the MOCK (mock == real): debug_enqueue_input ->
// poll_input, with the EXACT-EPOCH gate. Proves the four cases that distinguish
// "intent must not be lost" from "stale must be dropped": live+matching epoch returned; a RESIZE
// (lifecycle bump, same epoch) keeps input; an UNREGISTER (epoch 0) drops it; an
// unregister+RE-REGISTER of the same XID mints a NEW epoch -- old-epoch input is dropped, new-epoch
// input accepted.
void test_input_plane_epoch_gate() {
    vkrpc::MockVulkanBackend be("mock-gpu");
    SidecarRegisterToplevelRequest reg;
    reg.xid = 0x10;
    reg.generation = 1;
    reg.role = "toplevel";
    const std::uint64_t e1 =
        be.register_toplevel(reg).epoch; // placeholder, representation epoch e1
    VKR_CHECK(e1 != 0);
    VKR_CHECK_EQ(be.debug_registry_epoch_for_xid(0x10), e1);

    auto enqueue = [&](std::uint64_t epoch, std::vector<SidecarInputEvent> evs) {
        SidecarDebugEnqueueInputRequest enq;
        enq.xid = 0x10;
        enq.epoch = epoch;
        enq.events = std::move(evs);
        return be.debug_enqueue_input(enq).enqueued;
    };

    // Live + matching epoch -> all returned.
    VKR_CHECK_EQ(
        enqueue(e1, {make_input(InputEventKind::Focus, 0, 0, 0, 0, 0, true),
                     make_input(InputEventKind::Button, 5, 6, kInputButtonLeft, 0, 0, true),
                     make_input(InputEventKind::Close)}),
        3);
    SidecarPollInputRequest p;
    p.since_seq = 0;
    SidecarPollInputResponse r = be.poll_input(p);
    VKR_CHECK(r.ok);
    VKR_CHECK_EQ(r.events.size(), static_cast<std::size_t>(3));
    VKR_CHECK(static_cast<InputEventKind>(r.events[0].kind) == InputEventKind::Focus);
    p.since_seq = r.next_seq;

    // RESIZE survives: an update bumps the lifecycle generation but NOT the epoch -> input kept.
    SidecarUpdateToplevelRequest upd;
    upd.xid = 0x10;
    upd.generation = 5;
    upd.width = 200;
    VKR_CHECK_EQ(be.update_toplevel(upd).epoch, e1); // epoch unchanged by a resize
    enqueue(e1, {make_input(InputEventKind::Motion, 7, 7)});
    r = be.poll_input(p);
    VKR_CHECK_EQ(r.events.size(), static_cast<std::size_t>(1)); // survived the resize
    p.since_seq = r.next_seq;

    // UNREGISTER + RE-REGISTER (XID reuse): enqueue OLD-epoch input, then drop the representation
    // and re-establish it -> a NEW epoch e2. The old-epoch input is dropped; new-epoch input
    // accepted.
    enqueue(e1, {make_input(InputEventKind::Button, 1, 1, kInputButtonLeft, 0, 0, true),
                 make_input(InputEventKind::Close)});
    SidecarUnregisterToplevelRequest unr;
    unr.xid = 0x10;
    unr.generation = 6;
    VKR_CHECK_EQ(be.unregister_toplevel(unr).epoch, static_cast<std::uint64_t>(0)); // entry erased
    SidecarRegisterToplevelRequest reg2;
    reg2.xid = 0x10;
    reg2.generation = 7;
    reg2.role = "toplevel";
    const std::uint64_t e2 = be.register_toplevel(reg2).epoch;
    VKR_CHECK(e2 != 0 && e2 != e1); // a fresh representation epoch
    r = be.poll_input(p);
    VKR_CHECK_EQ(r.events.size(), static_cast<std::size_t>(0)); // old-epoch input dropped
    p.since_seq = r.next_seq;
    enqueue(e2, {make_input(InputEventKind::Focus, 0, 0, 0, 0, 0, true)});
    r = be.poll_input(p);
    VKR_CHECK_EQ(r.events.size(), static_cast<std::size_t>(1)); // new-epoch input accepted
}

// Epoch-split fix: the sidecar's per-event epoch reconciliation. A data-plane
// create_surface promotion mints a NEWER worker epoch the sidecar never observed; a forward worker
// epoch is authoritative (ADOPT + accept), a backward one is genuinely stale (DROP), equal is
// current (ACCEPT). Pre-fix the sidecar dropped EVERY event whose epoch != tracked, discarding all
// input after a surface bound.
void test_input_epoch_reconcile() {
    using vkr::sidecar::InputEpochDecision;
    using vkr::sidecar::reconcile_input_epoch;
    // event == tracked -> accept.
    VKR_CHECK(reconcile_input_epoch(3, 3) == InputEpochDecision::Accept);
    // event > tracked (the unobserved promotion) -> adopt then accept (the bug's repro: tracked 3,
    // worker stamped 5).
    VKR_CHECK(reconcile_input_epoch(3, 5) == InputEpochDecision::AdoptThenAccept);
    // event < tracked (a stale in-flight event after a remap) -> drop.
    VKR_CHECK(reconcile_input_epoch(5, 3) == InputEpochDecision::DropStale);
    // a fresh entry (tracked 0) adopts any minted (>=1) worker epoch.
    VKR_CHECK(reconcile_input_epoch(0, 1) == InputEpochDecision::AdoptThenAccept);
}

// Pure registry: the SURFACE-backed remap. A surface-FIRST first registration and
// a geometry update do NOT change the epoch, but an unregister-while-surface-live then re-register
// of the same xid (a new X lifecycle over the same surface) DOES mint a fresh epoch.
void test_registry_surface_remap_epoch() {
    WindowRegistry reg;
    reg.bind_surface(0x80, 0xA1); // surface-first -> Surface, epoch e1
    const std::uint64_t e1 = reg.epoch_for_xid(0x80);
    VKR_CHECK(e1 != 0);
    reg.register_toplevel(0x80, 1, "a", "t", {}); // FIRST register over the surface -> epoch kept
    VKR_CHECK_EQ(reg.epoch_for_xid(0x80), e1);
    reg.update_toplevel(0x80, 2, "a", {1, 2, 3, 4}); // resize -> epoch kept
    VKR_CHECK_EQ(reg.epoch_for_xid(0x80), e1);
    reg.unregister_toplevel(0x80, 3); // surface live -> entry kept (permutation 4), epoch e1
    VKR_CHECK(reg.representation_for_xid(0x80) == Representation::Surface);
    VKR_CHECK_EQ(reg.epoch_for_xid(0x80), e1);
    reg.register_toplevel(0x80, 4, "a", "t", {}); // RE-register over the live surface -> NEW epoch
    const std::uint64_t e2 = reg.epoch_for_xid(0x80);
    VKR_CHECK(e2 != 0 && e2 != e1);
}

// The surface-backed remap through the MOCK backend's input gate (regression): bind
// a surface, register, enqueue old-epoch input, unregister WHILE the surface is live, re-register
// the same xid -> the epoch changes, the old-epoch input is dropped and new-epoch input is
// accepted.
void test_input_plane_surface_remap() {
    vkrpc::MockVulkanBackend be("NVIDIA T1200 Laptop GPU"); // a real mocked device -> has a device
    const vkrpc::CreateInstanceResponse inst = be.create_instance({});
    VKR_CHECK(inst.ok);
    vkrpc::CreateSurfaceRequest cs;
    cs.instance = inst.instance;
    cs.platform = "xcb";
    cs.xid = 0x80;
    VKR_CHECK(be.create_surface(cs).ok); // binds the surface to xid 0x80 -> epoch e1
    const std::uint64_t e1 = be.debug_registry_epoch_for_xid(0x80);
    VKR_CHECK(e1 != 0);

    SidecarRegisterToplevelRequest reg;
    reg.xid = 0x80;
    reg.generation = 1;
    reg.role = "toplevel";
    VKR_CHECK_EQ(be.register_toplevel(reg).epoch, e1); // first register over surface -> epoch kept

    SidecarDebugEnqueueInputRequest enq; // OLD-epoch surface input (still pending)
    enq.xid = 0x80;
    enq.epoch = e1;
    enq.events.push_back(make_input(InputEventKind::Button, 1, 1, kInputButtonLeft, 0, 0, true));
    be.debug_enqueue_input(enq);

    SidecarUnregisterToplevelRequest unr;
    unr.xid = 0x80;
    unr.generation = 2;
    VKR_CHECK_EQ(be.unregister_toplevel(unr).epoch, e1); // surface live -> entry kept, epoch e1
    SidecarRegisterToplevelRequest reg2;
    reg2.xid = 0x80;
    reg2.generation = 3;
    reg2.role = "toplevel";
    const std::uint64_t e2 = be.register_toplevel(reg2).epoch;
    VKR_CHECK(e2 != 0 && e2 != e1); // the remap minted a fresh epoch

    SidecarPollInputRequest p;
    p.since_seq = 0;
    SidecarPollInputResponse r = be.poll_input(p);
    VKR_CHECK_EQ(r.events.size(), static_cast<std::size_t>(0)); // old-epoch surface input dropped
    p.since_seq = r.next_seq;
    SidecarDebugEnqueueInputRequest enq2;
    enq2.xid = 0x80;
    enq2.epoch = e2;
    enq2.events.push_back(make_input(InputEventKind::Focus, 0, 0, 0, 0, 0, true));
    be.debug_enqueue_input(enq2);
    r = be.poll_input(p);
    VKR_CHECK_EQ(r.events.size(), static_cast<std::size_t>(1)); // new-epoch input accepted
}

// The EXACT epoch-split shape (the /tmp/vkrelay2-logs-66504 regression as a direct
// gate): a PLACEHOLDER toplevel is registered (the sidecar tracks epoch e1), then a DATA-PLANE
// create_surface PROMOTES the same xid to a GPU surface, which mints a NEW worker epoch e2 > e1
// WITHOUT any sidecar toplevel-op. Input is then stamped at the worker's current (promoted) epoch.
// Assert: (a) the worker's poll_input gate ACCEPTS e2 input (the promotion advanced epoch_for_xid),
// (b) the sidecar reconciliation ADOPTS the forward epoch (e1 -> e2), and (c) lingering OLD-epoch
// (e1) input is dropped (worker gate + reconcile both stale). Pre-fix the sidecar dropped every e2
// event.
void test_input_plane_promotion_adopt() {
    vkrpc::MockVulkanBackend be("NVIDIA T1200 Laptop GPU");
    const vkrpc::CreateInstanceResponse inst = be.create_instance({});
    VKR_CHECK(inst.ok);

    SidecarRegisterToplevelRequest reg; // placeholder FIRST (no surface yet) -> epoch e1
    reg.xid = 0x90;
    reg.generation = 1;
    reg.role = "toplevel";
    const std::uint64_t e1 = be.register_toplevel(reg).epoch;
    VKR_CHECK(e1 != 0);
    VKR_CHECK_EQ(be.debug_registry_epoch_for_xid(0x90), e1);

    vkrpc::CreateSurfaceRequest cs; // data-plane create_surface PROMOTES placeholder -> surface
    cs.instance = inst.instance;
    cs.platform = "xcb";
    cs.xid = 0x90;
    VKR_CHECK(be.create_surface(cs).ok);
    const std::uint64_t e2 = be.debug_registry_epoch_for_xid(0x90);
    VKR_CHECK(e2 != 0 && e2 > e1); // the promotion minted a NEW, higher worker epoch

    // The sidecar still tracks e1 but the worker now stamps/gates at e2 -> reconciliation must
    // ADOPT.
    VKR_CHECK(reconcile_input_epoch(e1, e2) == InputEpochDecision::AdoptThenAccept);

    // The worker gate accepts e2 input (it is epoch_for_xid now); a lingering e1 event is dropped.
    SidecarDebugEnqueueInputRequest old_ep;
    old_ep.xid = 0x90;
    old_ep.epoch = e1;
    old_ep.events.push_back(make_input(InputEventKind::Button, 1, 1, kInputButtonLeft, 0, 0, true));
    be.debug_enqueue_input(old_ep);
    SidecarDebugEnqueueInputRequest new_ep;
    new_ep.xid = 0x90;
    new_ep.epoch = e2;
    new_ep.events.push_back(make_input(InputEventKind::Focus, 0, 0, 0, 0, 0, true));
    be.debug_enqueue_input(new_ep);

    SidecarPollInputRequest p;
    p.since_seq = 0;
    const SidecarPollInputResponse r = be.poll_input(p);
    VKR_CHECK_EQ(r.events.size(),
                 static_cast<std::size_t>(1)); // only the e2 event survives the gate
    VKR_CHECK_EQ(static_cast<long long>(r.events.at(0).epoch), static_cast<long long>(e2));
    VKR_CHECK(reconcile_input_epoch(e2, e1) == InputEpochDecision::DropStale); // old epoch is stale
}

// --- cursor -----------------------------------------------------------------------

// Build a valid SetCursor request filling the whole cursor with one BGRA color.
SidecarSetCursorRequest make_cursor(std::uint64_t xid, std::uint32_t w, std::uint32_t h,
                                    std::int32_t xhot, std::int32_t yhot, unsigned char b,
                                    unsigned char g, unsigned char r, unsigned char a) {
    SidecarSetCursorRequest req;
    req.xid = xid;
    req.width = w;
    req.height = h;
    req.xhot = xhot;
    req.yhot = yhot;
    req.format = kCursorFormatBgra8;
    req.pixels.resize(static_cast<std::size_t>(w) * h * 4);
    for (std::size_t i = 0; i < req.pixels.size(); i += 4) {
        req.pixels[i + 0] = static_cast<char>(b);
        req.pixels[i + 1] = static_cast<char>(g);
        req.pixels[i + 2] = static_cast<char>(r);
        req.pixels[i + 3] = static_cast<char>(a);
    }
    return req;
}

// The SetCursor binary framing + the response/query messages round-trip.
void test_cursor_messages_round_trip() {
    SidecarSetCursorRequest req = make_cursor(0x1234, 4, 2, 1, 0, 0x10, 0x20, 0x30, 0xFF);
    req.epoch = 7;
    std::string err;
    const SidecarSetCursorRequest back = SidecarSetCursorRequest::from_wire(req.to_wire(), err);
    VKR_CHECK(err.empty());
    VKR_CHECK_EQ(back.xid, static_cast<std::uint64_t>(0x1234));
    VKR_CHECK_EQ(back.epoch, static_cast<std::uint64_t>(7));
    VKR_CHECK_EQ(back.width, static_cast<std::uint32_t>(4));
    VKR_CHECK_EQ(back.height, static_cast<std::uint32_t>(2));
    VKR_CHECK_EQ(back.xhot, 1);
    VKR_CHECK_EQ(back.yhot, 0);
    VKR_CHECK_EQ(back.pixels.size(), static_cast<std::size_t>(32));
    VKR_CHECK_EQ(static_cast<unsigned char>(back.pixels[1]), static_cast<unsigned char>(0x20));

    SidecarSetCursorResponse sresp;
    sresp.ok = true;
    sresp.xid = 0x1234;
    sresp.applied = true;
    const SidecarSetCursorResponse sb = SidecarSetCursorResponse::from_body(sresp.to_body());
    VKR_CHECK(sb.ok && sb.applied);
    VKR_CHECK_EQ(sb.xid, static_cast<std::uint64_t>(0x1234));

    SidecarDebugCursorStateRequest dq;
    dq.xid = 0x55;
    dq.sample_x = 3;
    dq.sample_y = 1;
    const SidecarDebugCursorStateRequest dqb =
        SidecarDebugCursorStateRequest::from_body(dq.to_body());
    VKR_CHECK_EQ(dqb.xid, static_cast<std::uint64_t>(0x55));
    VKR_CHECK_EQ(dqb.sample_x, 3);
    VKR_CHECK_EQ(dqb.sample_y, 1);

    SidecarDebugCursorStateResponse dr;
    dr.ok = true;
    dr.xid = 0x55;
    dr.has_cursor = true;
    dr.width = 4;
    dr.height = 2;
    dr.xhot = 1;
    dr.yhot = 0;
    dr.has_pixel = true;
    dr.pixel_bgra = 0xFF302010u;
    const SidecarDebugCursorStateResponse drb =
        SidecarDebugCursorStateResponse::from_body(dr.to_body());
    VKR_CHECK(drb.has_cursor && drb.has_pixel);
    VKR_CHECK_EQ(drb.width, static_cast<std::uint32_t>(4));
    VKR_CHECK_EQ(drb.xhot, 1);
    VKR_CHECK_EQ(drb.pixel_bgra, static_cast<std::uint32_t>(0xFF302010u));
}

// The SetCursor decoder rejects every malformed body BEFORE the HCURSOR build.
void test_cursor_decoder_discipline() {
    std::string err;
    (void) SidecarSetCursorRequest::from_wire("ab", err); // short (no length prefix)
    VKR_CHECK(!err.empty());

    SidecarSetCursorRequest bad = make_cursor(1, 2, 2, 0, 0, 0, 0, 0, 0xFF);
    bad.format = 99; // unknown format
    (void) SidecarSetCursorRequest::from_wire(bad.to_wire(), err);
    VKR_CHECK(!err.empty());

    bad = make_cursor(1, 2, 2, 0, 0, 0, 0, 0, 0xFF);
    bad.width = 0; // zero dimension
    bad.pixels.clear();
    (void) SidecarSetCursorRequest::from_wire(bad.to_wire(), err);
    VKR_CHECK(!err.empty());

    bad = make_cursor(1, 2, 2, 0, 0, 0, 0, 0, 0xFF);
    bad.width = kMaxCursorDim + 1; // dimension exceeds the cap
    bad.pixels.assign(static_cast<std::size_t>(kMaxCursorDim + 1) * 2 * 4, '\0');
    (void) SidecarSetCursorRequest::from_wire(bad.to_wire(), err);
    VKR_CHECK(!err.empty());

    bad = make_cursor(1, 4, 4, 0, 0, 0, 0, 0, 0xFF);
    bad.xhot = 4; // == width -> hotspot out of range
    (void) SidecarSetCursorRequest::from_wire(bad.to_wire(), err);
    VKR_CHECK(!err.empty());

    bad = make_cursor(1, 4, 2, 0, 0, 0, 0, 0, 0xFF);
    bad.pixels.assign(16, '\0'); // payload != width*height*4 (expected 32)
    (void) SidecarSetCursorRequest::from_wire(bad.to_wire(), err);
    VKR_CHECK(!err.empty());

    // A well-formed body still decodes clean (the discipline is not over-broad).
    (void) SidecarSetCursorRequest::from_wire(make_cursor(1, 2, 2, 0, 0, 0, 0, 0, 0xFF).to_wire(),
                                              err);
    VKR_CHECK(err.empty());
}

// The cursor plane through the MOCK (mock == real): set_cursor is gated on a registered toplevel +
// exact epoch; debug_cursor_state samples the built cursor; teardown drops it.
void test_cursor_plane_mock() {
    vkrpc::MockVulkanBackend be("mock-gpu");
    // B=0x99 G=0x66 R=0x33 A=0xFF -> sampled pixel_bgra 0xFF336699 (the canary's cursor color).
    SidecarSetCursorRequest c = make_cursor(0x10, 4, 2, 1, 0, 0x99, 0x66, 0x33, 0xFF);

    // No representation yet -> not applied; no cursor.
    VKR_CHECK(!be.set_cursor(c).applied);
    VKR_CHECK(!be.debug_cursor_state({0x10, 0, 0}).has_cursor);

    // Register -> placeholder -> set_cursor (at the live epoch) applies; the proof samples it.
    SidecarRegisterToplevelRequest reg;
    reg.xid = 0x10;
    reg.generation = 1;
    reg.role = "toplevel";
    be.register_toplevel(reg);
    c.epoch = be.debug_registry_epoch_for_xid(0x10);
    VKR_CHECK(!be.set_cursor(make_cursor(0x10, 4, 2, 1, 0, 0, 0, 0, 0xFF))
                   .applied); // epoch 0 -> rejected
    const SidecarSetCursorResponse sr = be.set_cursor(c);
    VKR_CHECK(sr.ok && sr.applied);

    SidecarDebugCursorStateRequest q;
    q.xid = 0x10;
    q.sample_x = 1;
    q.sample_y = 1;
    const SidecarDebugCursorStateResponse dr = be.debug_cursor_state(q);
    VKR_CHECK(dr.has_cursor && dr.has_pixel);
    VKR_CHECK_EQ(dr.width, static_cast<std::uint32_t>(4));
    VKR_CHECK_EQ(dr.height, static_cast<std::uint32_t>(2));
    VKR_CHECK_EQ(dr.xhot, 1);
    VKR_CHECK_EQ(dr.pixel_bgra, static_cast<std::uint32_t>(0xFF336699u));

    // Out of bounds -> no pixel (but still has_cursor).
    q.sample_x = 99;
    const SidecarDebugCursorStateResponse oob = be.debug_cursor_state(q);
    VKR_CHECK(oob.has_cursor && !oob.has_pixel);

    // Unregister -> the placeholder (and its cursor) is gone.
    SidecarUnregisterToplevelRequest unr;
    unr.xid = 0x10;
    unr.generation = 2;
    be.unregister_toplevel(unr);
    VKR_CHECK(!be.debug_cursor_state({0x10, 0, 0}).has_cursor);
}

// The cursor epoch+registered gate across a SURFACE-backed remap: a cursor
// for an xid that was unregistered while its surface persisted, or whose epoch moved on, is
// rejected; only a cursor at the live epoch of a registered toplevel is applied.
void test_cursor_plane_surface_remap() {
    vkrpc::MockVulkanBackend be("NVIDIA T1200 Laptop GPU");
    const vkrpc::CreateInstanceResponse inst = be.create_instance({});
    VKR_CHECK(inst.ok);
    vkrpc::CreateSurfaceRequest cs;
    cs.instance = inst.instance;
    cs.platform = "xcb";
    cs.xid = 0x80;
    VKR_CHECK(be.create_surface(cs).ok); // binds the surface to xid 0x80 -> epoch e1
    const std::uint64_t e1 = be.debug_registry_epoch_for_xid(0x80);

    auto cursor = [&](std::uint64_t epoch) {
        SidecarSetCursorRequest c = make_cursor(0x80, 4, 2, 1, 0, 0x99, 0x66, 0x33, 0xFF);
        c.epoch = epoch;
        return be.set_cursor(c).applied;
    };

    // Surface-only (not yet registered) -> rejected (not a registered toplevel).
    VKR_CHECK(!cursor(e1));
    SidecarRegisterToplevelRequest reg;
    reg.xid = 0x80;
    reg.generation = 1;
    reg.role = "toplevel";
    VKR_CHECK_EQ(be.register_toplevel(reg).epoch, e1);
    VKR_CHECK(cursor(e1)); // registered + matching epoch -> applied
    VKR_CHECK(be.debug_cursor_state({0x80, 0, 0}).has_cursor); // installed + visible to the proof

    // Unregister WHILE the surface is live -> the entry persists (epoch e1) but is no longer a
    // registered toplevel. Two things must hold: a NEW cursor at e1 is
    // rejected (stale-on-a-dead-lifecycle), AND the already-installed cursor is CLEARED so it does
    // not survive into the re-register below.
    SidecarUnregisterToplevelRequest unr;
    unr.xid = 0x80;
    unr.generation = 2;
    be.unregister_toplevel(unr);
    VKR_CHECK(!cursor(e1));
    VKR_CHECK(!be.debug_cursor_state({0x80, 0, 0}).has_cursor); // cleared on unregister

    // Re-register -> a NEW epoch e2. The old-epoch cursor is rejected; the new-epoch cursor
    // applies.
    SidecarRegisterToplevelRequest reg2;
    reg2.xid = 0x80;
    reg2.generation = 3;
    reg2.role = "toplevel";
    const std::uint64_t e2 = be.register_toplevel(reg2).epoch;
    VKR_CHECK(e2 != 0 && e2 != e1);
    VKR_CHECK(!cursor(e1));
    VKR_CHECK(cursor(e2));
}

// the DebugEnumWindows response body codec (a list of SidecarWindowInfo),
// round-tripped with all fields populated incl. a negative origin + the decimal-string u64
// lifecycle selectors.
void test_enum_windows_wire() {
    SidecarDebugEnumWindowsRequest rq; // no fields, but the codec must round-trip
    (void) SidecarDebugEnumWindowsRequest::from_body(rq.to_body());

    SidecarDebugEnumWindowsResponse resp;
    resp.ok = true;
    resp.reason = "ok";
    SidecarWindowInfo w1;
    w1.xid = 0xABCD1234u;
    w1.representation = "placeholder";
    w1.toplevel_registered = true;
    w1.has_surface = false;
    w1.generation = 5;
    w1.epoch = 9;
    w1.last_paint_seq = 3;
    w1.shown = true;
    w1.role = "toplevel";
    w1.title = "hello";
    w1.x = -7;
    w1.y = 12;
    w1.width = 640;
    w1.height = 480;
    w1.is_popup = true;
    w1.owner_xid = 0x5151;
    w1.popup_kind = static_cast<std::uint32_t>(vkr::sidecar::PopupKind::Tooltip);
    // actual host geometry fields (incl. a negative actual origin).
    w1.has_actual = true;
    w1.actual_x = -3;
    w1.actual_y = 14;
    w1.actual_width = 320;
    w1.actual_height = 200;
    w1.frame_x = 100;
    w1.frame_y = 50;
    w1.frame_width = 336;
    w1.frame_height = 239;
    w1.last_host_apply_seq = 42;
    w1.clamped = true;
    // visibility fields.
    w1.visibility_state = static_cast<std::uint32_t>(vkr::sidecar::VisibilityState::Hidden);
    w1.host_visible = false;
    w1.host_iconic = true;
    SidecarWindowInfo w2;
    w2.xid = 0x1;
    w2.representation = "surface";
    w2.has_surface = true;
    resp.windows = {w1, w2};

    const SidecarDebugEnumWindowsResponse back =
        SidecarDebugEnumWindowsResponse::from_body(resp.to_body());
    VKR_CHECK(back.ok);
    VKR_CHECK_EQ(back.reason, std::string("ok"));
    VKR_CHECK_EQ(back.windows.size(), static_cast<std::size_t>(2));
    const SidecarWindowInfo& a = back.windows[0];
    VKR_CHECK_EQ(a.xid, static_cast<std::uint64_t>(0xABCD1234u));
    VKR_CHECK_EQ(a.representation, std::string("placeholder"));
    VKR_CHECK(a.toplevel_registered && !a.has_surface && a.shown);
    VKR_CHECK_EQ(a.generation, static_cast<std::uint64_t>(5));
    VKR_CHECK_EQ(a.epoch, static_cast<std::uint64_t>(9));
    VKR_CHECK_EQ(a.last_paint_seq, static_cast<std::uint64_t>(3));
    VKR_CHECK_EQ(a.role, std::string("toplevel"));
    VKR_CHECK_EQ(a.title, std::string("hello"));
    VKR_CHECK_EQ(a.x, -7);
    VKR_CHECK_EQ(a.y, 12);
    VKR_CHECK_EQ(a.width, static_cast<std::uint32_t>(640));
    VKR_CHECK_EQ(a.height, static_cast<std::uint32_t>(480));
    VKR_CHECK(a.is_popup);
    VKR_CHECK_EQ(a.owner_xid, static_cast<std::uint64_t>(0x5151));
    VKR_CHECK_EQ(a.popup_kind, static_cast<std::uint32_t>(vkr::sidecar::PopupKind::Tooltip));
    VKR_CHECK(a.has_actual);
    VKR_CHECK_EQ(a.actual_x, -3);
    VKR_CHECK_EQ(a.actual_y, 14);
    VKR_CHECK_EQ(a.actual_width, static_cast<std::uint32_t>(320));
    VKR_CHECK_EQ(a.actual_height, static_cast<std::uint32_t>(200));
    VKR_CHECK_EQ(a.frame_x, 100);
    VKR_CHECK_EQ(a.frame_y, 50);
    VKR_CHECK_EQ(a.frame_width, static_cast<std::uint32_t>(336));
    VKR_CHECK_EQ(a.frame_height, static_cast<std::uint32_t>(239));
    VKR_CHECK_EQ(a.last_host_apply_seq, static_cast<std::uint64_t>(42));
    VKR_CHECK(a.clamped);
    VKR_CHECK_EQ(a.visibility_state,
                 static_cast<std::uint32_t>(vkr::sidecar::VisibilityState::Hidden));
    VKR_CHECK(!a.host_visible && a.host_iconic);
    const SidecarWindowInfo& b = back.windows[1];
    VKR_CHECK_EQ(b.xid, static_cast<std::uint64_t>(0x1));
    VKR_CHECK_EQ(b.representation, std::string("surface"));
    VKR_CHECK(b.has_surface && !b.toplevel_registered);
    VKR_CHECK(!b.is_popup && b.owner_xid == 0); // ordinary window defaults
    VKR_CHECK(!b.has_actual);                   // include_actual not set on this entry -> defaults
    VKR_CHECK_EQ(b.visibility_state,
                 static_cast<std::uint32_t>(vkr::sidecar::VisibilityState::Visible)); // default
    VKR_CHECK(!b.host_visible && !b.host_iconic);
}

// The SetToplevelVisibility request codec round-trips, including Iconic.
void test_set_visibility_wire() {
    SidecarSetVisibilityRequest r;
    r.xid = 0xDEADBEEFu;
    r.generation = 77;
    r.visibility_state = static_cast<std::uint32_t>(vkr::sidecar::VisibilityState::Iconic);
    const SidecarSetVisibilityRequest back = SidecarSetVisibilityRequest::from_body(r.to_body());
    VKR_CHECK_EQ(back.xid, static_cast<std::uint64_t>(0xDEADBEEFu));
    VKR_CHECK_EQ(back.generation, static_cast<std::uint64_t>(77));
    VKR_CHECK_EQ(back.visibility_state,
                 static_cast<std::uint32_t>(vkr::sidecar::VisibilityState::Iconic));
}

// the mock's debug_enum_windows reflects registry state, sorted by xid (mock == real:
// the real backend builds the identical mapping from the same WindowRegistry -- see
// integration_real_backend test_real_enum_windows).
void test_enum_windows_mock() {
    vkrpc::MockVulkanBackend be("NVIDIA T1200 Laptop GPU");
    // Empty registry -> ok, no windows.
    const SidecarDebugEnumWindowsResponse e0 = be.debug_enum_windows({});
    VKR_CHECK(e0.ok && e0.windows.empty());

    // A surface-only (pending) entry for xid 0x10, and a placeholder toplevel for xid 0x20.
    const vkrpc::CreateInstanceResponse inst = be.create_instance({});
    VKR_CHECK(inst.ok);
    vkrpc::CreateSurfaceRequest cs;
    cs.instance = inst.instance;
    cs.platform = "xcb";
    cs.xid = 0x10;
    VKR_CHECK(be.create_surface(cs).ok);

    SidecarRegisterToplevelRequest reg;
    reg.xid = 0x20;
    reg.generation = 7;
    reg.role = "toplevel";
    reg.title = "win-20";
    reg.x = 11;
    reg.y = 22;
    reg.width = 333;
    reg.height = 244;
    VKR_CHECK(be.register_toplevel(reg).applied);

    const SidecarDebugEnumWindowsResponse e = be.debug_enum_windows({});
    VKR_CHECK(e.ok);
    VKR_CHECK_EQ(e.windows.size(), static_cast<std::size_t>(2));
    // Sorted by xid: [0] = 0x10 (surface-only), [1] = 0x20 (placeholder).
    const SidecarWindowInfo& a = e.windows[0];
    VKR_CHECK_EQ(a.xid, static_cast<std::uint64_t>(0x10));
    VKR_CHECK_EQ(a.representation, std::string("surface"));
    VKR_CHECK(a.has_surface && !a.toplevel_registered);

    const SidecarWindowInfo& b = e.windows[1];
    VKR_CHECK_EQ(b.xid, static_cast<std::uint64_t>(0x20));
    VKR_CHECK_EQ(b.representation, std::string("placeholder"));
    VKR_CHECK(b.toplevel_registered && !b.has_surface);
    VKR_CHECK_EQ(b.generation, static_cast<std::uint64_t>(7));
    VKR_CHECK_EQ(b.epoch, be.debug_registry_epoch_for_xid(0x20));
    VKR_CHECK(b.epoch != 0);
    VKR_CHECK(!b.shown); // a fresh placeholder is unpainted until its first chrome paint
    VKR_CHECK_EQ(b.role, std::string("toplevel"));
    VKR_CHECK_EQ(b.title, std::string("win-20"));
    VKR_CHECK_EQ(b.x, 11);
    VKR_CHECK_EQ(b.y, 22);
    VKR_CHECK_EQ(b.width, static_cast<std::uint32_t>(333));
    VKR_CHECK_EQ(b.height, static_cast<std::uint32_t>(244));

    // Unregister the placeholder -> only the surface-only entry remains.
    SidecarUnregisterToplevelRequest unr;
    unr.xid = 0x20;
    unr.generation = 8;
    VKR_CHECK(be.unregister_toplevel(unr).applied);
    const SidecarDebugEnumWindowsResponse e2 = be.debug_enum_windows({});
    VKR_CHECK_EQ(e2.windows.size(), static_cast<std::size_t>(1));
    VKR_CHECK_EQ(e2.windows[0].xid, static_cast<std::uint64_t>(0x10));
}

// the mock's include_actual reports the last-APPLIED move geometry (mock == real for
// the authored x/y + seq -- the real backend reads the real HWND; see integration_real_backend
// test_real_move). Default enum (no include_actual) never fills the actual fields.
void test_apply_geometry_mock() {
    vkrpc::MockVulkanBackend be("NVIDIA T1200 Laptop GPU");
    SidecarRegisterToplevelRequest reg;
    reg.xid = 0x30;
    reg.generation = 1;
    reg.role = "toplevel";
    reg.x = 10;
    reg.y = 20;
    reg.width = 200;
    reg.height = 100;
    VKR_CHECK(be.register_toplevel(reg).applied);

    // Initial placement: register PLACES the host, so include_actual already
    // reports the registered geometry as actual (a map-once-never-move app is in the right place).
    SidecarDebugEnumWindowsRequest q;
    q.include_actual = true;
    std::uint64_t seq_after_register = 0;
    {
        const SidecarDebugEnumWindowsResponse e = be.debug_enum_windows(q);
        VKR_CHECK_EQ(e.windows.size(), static_cast<std::size_t>(1));
        VKR_CHECK(e.windows[0].has_actual);
        VKR_CHECK_EQ(e.windows[0].actual_x, 10);
        VKR_CHECK_EQ(e.windows[0].actual_y, 20);
        VKR_CHECK(e.windows[0].last_host_apply_seq != 0);
        seq_after_register = e.windows[0].last_host_apply_seq;
    }

    // Move it -> the last-applied geometry is reported as actual (+ a strictly-greater seq).
    SidecarUpdateToplevelRequest mv;
    mv.xid = 0x30;
    mv.generation = 2;
    mv.x = 300;
    mv.y = 250;
    mv.width = 200;
    mv.height = 100;
    VKR_CHECK(be.update_toplevel(mv).applied);
    std::uint64_t seq_after_move = 0;
    {
        const SidecarDebugEnumWindowsResponse e = be.debug_enum_windows(q);
        VKR_CHECK(e.windows[0].has_actual);
        VKR_CHECK_EQ(e.windows[0].actual_x, 300);
        VKR_CHECK_EQ(e.windows[0].actual_y, 250);
        VKR_CHECK(e.windows[0].last_host_apply_seq >
                  seq_after_register); // strictly newer than place
        seq_after_move = e.windows[0].last_host_apply_seq;
        // The DEFAULT enum (no include_actual) never fills the actual fields.
        const SidecarDebugEnumWindowsResponse plain = be.debug_enum_windows({});
        VKR_CHECK(!plain.windows[0].has_actual);
    }

    // A stale-generation move is dropped -> the reported actual position does not regress.
    SidecarUpdateToplevelRequest stale;
    stale.xid = 0x30;
    stale.generation = 2; // equal -> dropped
    stale.x = 5;
    stale.y = 5;
    VKR_CHECK(!be.update_toplevel(stale).applied);
    {
        const SidecarDebugEnumWindowsResponse e = be.debug_enum_windows(q);
        VKR_CHECK_EQ(e.windows[0].actual_x, 300);
        VKR_CHECK_EQ(e.windows[0].actual_y, 250);
        VKR_CHECK_EQ(e.windows[0].last_host_apply_seq, seq_after_move);
    }
}

// a surface PROMOTE then OVERWRITE for a registered xid records the
// inherited placement under the REAL xid (not xid 0). The overwrite's bind_surface effect must name
// the xid; otherwise include_actual would never reflect the overwrite's geometry seq on that
// window.
void test_apply_geometry_mock_overwrite() {
    vkrpc::MockVulkanBackend be("NVIDIA T1200 Laptop GPU");
    const vkrpc::CreateInstanceResponse inst = be.create_instance({});
    VKR_CHECK(inst.ok);

    SidecarRegisterToplevelRequest reg;
    reg.xid = 0x40;
    reg.generation = 1;
    reg.role = "toplevel";
    reg.x = 50;
    reg.y = 60;
    reg.width = 256;
    reg.height = 256;
    VKR_CHECK(be.register_toplevel(reg).applied); // placeholder placed at (50,60)

    auto seq_of = [&](std::uint64_t xid) -> std::uint64_t {
        SidecarDebugEnumWindowsRequest q;
        q.include_actual = true;
        for (const SidecarWindowInfo& w : be.debug_enum_windows(q).windows) {
            if (w.xid == xid) {
                return w.last_host_apply_seq;
            }
        }
        return 0;
    };

    vkrpc::CreateSurfaceRequest cs;
    cs.instance = inst.instance;
    cs.platform = "xcb";
    cs.xid = 0x40;
    const vkrpc::CreateSurfaceResponse s1 = be.create_surface(cs); // promote -> Surface
    VKR_CHECK(s1.ok);
    const std::uint64_t seq_after_promote = seq_of(0x40);
    VKR_CHECK(seq_after_promote != 0);

    // A SECOND surface for the same registered xid -> overwrite. The placement must record on 0x40.
    const vkrpc::CreateSurfaceResponse s2 = be.create_surface(cs);
    VKR_CHECK(s2.ok);
    const std::uint64_t seq_after_overwrite = seq_of(0x40);
    VKR_CHECK(seq_after_overwrite > seq_after_promote); // recorded on 0x40, NOT on xid 0
    // The actual position is still the registered (50,60) -- the overwrite inherits it.
    SidecarDebugEnumWindowsRequest q;
    q.include_actual = true;
    for (const SidecarWindowInfo& w : be.debug_enum_windows(q).windows) {
        if (w.xid == 0x40) {
            VKR_CHECK_EQ(w.actual_x, 50);
            VKR_CHECK_EQ(w.actual_y, 60);
        }
        VKR_CHECK(w.xid != 0); // no phantom xid-0 entry was ever enumerated
    }
}

// (mock == real): the FULL resize-convergence loop under sidecar extent authority. A
// surface-backed toplevel renders happily; a sidecar-authored resize trips the dirty latch
// (acquire/present -> OUT_OF_DATE); get_surface_capabilities then PINS currentExtent to the
// authored size; create_swapchain at a MISMATCHING extent is refused (OUT_OF_DATE, latch still
// set); only a create_swapchain at the authored extent converges (clears the latch); include_actual
// reports the authored client extent. This is the headless dual-platform model of the §2.4 crux
// (the real WM_SIZE/HWND half lives in integration_real_backend).
void test_resize_convergence_mock() {
    vkrpc::MockVulkanBackend be("NVIDIA T1200 Laptop GPU");
    const vkrpc::CreateInstanceResponse ci = be.create_instance({});
    VKR_CHECK(ci.ok);
    vkrpc::EnumeratePhysicalDevicesRequest er;
    er.instance = ci.instance;
    const vkrpc::EnumeratePhysicalDevicesResponse en = be.enumerate_physical_devices(er);
    vkrpc::CreateDeviceRequest cdr;
    cdr.instance = ci.instance;
    cdr.physical_device = en.devices.front().handle;
    const vkrpc::CreateDeviceResponse cd = be.create_device(cdr);
    vkrpc::GetDeviceQueueRequest gq;
    gq.device = cd.device;
    gq.queue_family_index = cd.queue_family_index;
    gq.queue_index = 0;
    const std::uint64_t queue = be.get_device_queue(gq).queue;
    VKR_CHECK(queue != 0);

    // Register the toplevel, then create its born-correlated surface (promote -> Surface).
    SidecarRegisterToplevelRequest reg;
    reg.xid = 0x50;
    reg.generation = 1;
    reg.role = "toplevel";
    reg.width = 256;
    reg.height = 256;
    VKR_CHECK(be.register_toplevel(reg).applied);
    vkrpc::CreateSurfaceRequest sr;
    sr.instance = ci.instance;
    sr.platform = "xcb";
    sr.xid = 0x50;
    const vkrpc::CreateSurfaceResponse surf = be.create_surface(sr);
    VKR_CHECK(surf.ok);

    auto make_scr = [&](int w, int h) {
        vkrpc::CreateSwapchainRequest scr;
        scr.device = cd.device;
        scr.surface = surf.surface;
        scr.image_format = 44;
        scr.color_space = 0;
        scr.present_mode = 2;
        scr.width = w;
        scr.height = h;
        scr.min_image_count = 3;
        scr.image_usage = vkrpc::kImageUsageColorAttachment | vkrpc::kImageUsageTransferDst;
        return scr;
    };
    // App owns the extent before any authored resize -> a 256x256 swapchain is fine.
    const vkrpc::CreateSwapchainResponse sc0 = be.create_swapchain(make_scr(256, 256));
    VKR_CHECK(sc0.ok && sc0.result == vkrpc::kVkSuccess);
    vkrpc::AcquireNextImageRequest air;
    air.swapchain = sc0.swapchain;
    VKR_CHECK_EQ(be.acquire_next_image(air).result, vkrpc::kVkSuccess);

    // Sidecar authors a RESIZE to 512x384 -> the bound surface goes dirty (the latch).
    SidecarUpdateToplevelRequest rs;
    rs.xid = 0x50;
    rs.generation = 2;
    rs.width = 512;
    rs.height = 384;
    VKR_CHECK(be.update_toplevel(rs).applied);
    VKR_CHECK_EQ(be.acquire_next_image(air).result, vkrpc::kVkErrorOutOfDateKhr);

    // get_surface_capabilities now PINS currentExtent = the authored 512x384 (min == max ==
    // current).
    vkrpc::GetSurfaceCapabilitiesRequest cap_req;
    cap_req.physical_device = en.devices.front().handle;
    cap_req.surface = surf.surface;
    const vkrpc::GetSurfaceCapabilitiesResponse caps = be.get_surface_capabilities(cap_req);
    VKR_CHECK(caps.ok);
    VKR_CHECK_EQ(caps.current_extent_width, static_cast<std::uint64_t>(512));
    VKR_CHECK_EQ(caps.current_extent_height, static_cast<std::uint64_t>(384));
    VKR_CHECK_EQ(caps.min_image_extent_width, static_cast<std::uint64_t>(512));
    VKR_CHECK_EQ(caps.max_image_extent_width, static_cast<std::uint64_t>(512));

    // A create_swapchain at the app's OWN (stale) extent is refused -> OUT_OF_DATE, latch still
    // set.
    const vkrpc::CreateSwapchainResponse sc_bad = be.create_swapchain(make_scr(256, 256));
    VKR_CHECK(sc_bad.ok && sc_bad.result == vkrpc::kVkErrorOutOfDateKhr && sc_bad.swapchain == 0);
    VKR_CHECK_EQ(be.acquire_next_image(air).result, vkrpc::kVkErrorOutOfDateKhr); // still dirty

    // A create_swapchain at the AUTHORED extent converges -> clears the latch; acquire flows again.
    const vkrpc::CreateSwapchainResponse sc1 = be.create_swapchain(make_scr(512, 384));
    VKR_CHECK(sc1.ok && sc1.result == vkrpc::kVkSuccess && sc1.swapchain != 0);
    vkrpc::AcquireNextImageRequest air1;
    air1.swapchain = sc1.swapchain;
    VKR_CHECK_EQ(be.acquire_next_image(air1).result, vkrpc::kVkSuccess);

    // include_actual reports the authored client extent (mock == real for the authored size).
    SidecarDebugEnumWindowsRequest q;
    q.include_actual = true;
    for (const SidecarWindowInfo& w : be.debug_enum_windows(q).windows) {
        if (w.xid == 0x50) {
            VKR_CHECK(w.has_actual);
            VKR_CHECK_EQ(w.actual_width, static_cast<std::uint32_t>(512));
            VKR_CHECK_EQ(w.actual_height, static_cast<std::uint32_t>(384));
        }
    }

    // Resize STORM: a burst of authored resizes converges to the FINAL
    // size with no regression -- the authoritative extent + host_apply_seq track only the newest,
    // and a create_swapchain at any non-final extent (incl. an earlier storm size) is refused until
    // the app catches up to the final. No stranded OUT_OF_DATE.
    const std::pair<int, int> storm[] = {{400, 300}, {640, 480}, {300, 220}, {720, 500}};
    std::uint64_t prev_seq = 0;
    std::uint32_t gen = 3;
    for (const auto& sz : storm) {
        SidecarUpdateToplevelRequest u;
        u.xid = 0x50;
        u.generation = ++gen;
        u.width = static_cast<std::uint32_t>(sz.first);
        u.height = static_cast<std::uint32_t>(sz.second);
        VKR_CHECK(be.update_toplevel(u).applied);
        // The geometry seq is strictly monotonic across the storm (no regression).
        std::uint64_t cur_seq = 0;
        for (const SidecarWindowInfo& w : be.debug_enum_windows(q).windows) {
            if (w.xid == 0x50) {
                cur_seq = w.last_host_apply_seq;
            }
        }
        VKR_CHECK(cur_seq > prev_seq);
        prev_seq = cur_seq;
    }
    // Caps now pin the FINAL storm extent; an earlier storm size is refused; the final converges.
    const vkrpc::GetSurfaceCapabilitiesResponse caps2 = be.get_surface_capabilities(cap_req);
    VKR_CHECK_EQ(caps2.current_extent_width, static_cast<std::uint64_t>(720));
    VKR_CHECK_EQ(caps2.current_extent_height, static_cast<std::uint64_t>(500));
    VKR_CHECK(be.create_swapchain(make_scr(640, 480)).result == vkrpc::kVkErrorOutOfDateKhr);
    VKR_CHECK(be.create_swapchain(make_scr(720, 500)).result == vkrpc::kVkSuccess);

    // A zero width/height swapchain request is malformed (min extent is 1x1) ->
    // ok=false (a hard reject, NOT an OUT_OF_DATE retry). Both 0-width and 0-height.
    VKR_CHECK(!be.create_swapchain(make_scr(0, 500)).ok);
    VKR_CHECK(!be.create_swapchain(make_scr(720, 0)).ok);
}

// (mock == real): a z-order update is reported in DebugEnumWindows as the sticky last
// restack (z_order: 1 Raise / 2 Lower). The real backend ALSO restacks the actual HWND (asserted
// via GetWindow in integration_real_backend); the registry record is the over-the-wire proof.
void test_z_order_mock() {
    vkrpc::MockVulkanBackend be("NVIDIA T1200 Laptop GPU");
    SidecarRegisterToplevelRequest reg;
    reg.xid = 0x60;
    reg.generation = 1;
    reg.role = "toplevel";
    reg.width = 200;
    reg.height = 100;
    VKR_CHECK(be.register_toplevel(reg).applied);
    auto z_of = [&](std::uint64_t xid) -> std::uint32_t {
        for (const SidecarWindowInfo& w : be.debug_enum_windows({}).windows) {
            if (w.xid == xid) {
                return w.z_order;
            }
        }
        return 0;
    };
    VKR_CHECK_EQ(z_of(0x60), static_cast<std::uint32_t>(0)); // None until a restack

    // A Raise (the wire carries the ZOrder value 1).
    SidecarUpdateToplevelRequest rz;
    rz.xid = 0x60;
    rz.generation = 2;
    rz.z_order = static_cast<std::uint32_t>(sidecar::ZOrder::Raise);
    VKR_CHECK(be.update_toplevel(rz).applied);
    VKR_CHECK_EQ(z_of(0x60), static_cast<std::uint32_t>(sidecar::ZOrder::Raise));

    // A Lower updates the sticky record.
    SidecarUpdateToplevelRequest lz;
    lz.xid = 0x60;
    lz.generation = 3;
    lz.z_order = static_cast<std::uint32_t>(sidecar::ZOrder::Lower);
    VKR_CHECK(be.update_toplevel(lz).applied);
    VKR_CHECK_EQ(z_of(0x60), static_cast<std::uint32_t>(sidecar::ZOrder::Lower));
}

// (mock == real): set_visibility flips the authored visibility reported by
// DebugEnumWindows; the mock mirrors the host-observed state (host_visible == authored Visible). A
// hide PRESERVES the entry + epoch + the recorded geometry (NOT a teardown); a destroy after a hide
// removes it. Stale ops dropped.
void test_set_visibility_mock() {
    vkrpc::MockVulkanBackend be("NVIDIA T1200 Laptop GPU");
    SidecarRegisterToplevelRequest reg;
    reg.xid = 0x70;
    reg.generation = 1;
    reg.role = "toplevel";
    reg.x = 10;
    reg.y = 20;
    reg.width = 200;
    reg.height = 100;
    const std::uint64_t epoch0 = be.register_toplevel(reg).epoch;
    VKR_CHECK(epoch0 != 0);

    SidecarDebugEnumWindowsRequest q;
    q.include_actual = true;
    auto row = [&](std::uint64_t xid) {
        SidecarWindowInfo out;
        for (const SidecarWindowInfo& w : be.debug_enum_windows(q).windows) {
            if (w.xid == xid) {
                out = w;
            }
        }
        return out;
    };
    {
        const SidecarWindowInfo w = row(0x70);
        VKR_CHECK_EQ(w.visibility_state, static_cast<std::uint32_t>(VisibilityState::Visible));
        VKR_CHECK(w.host_visible && !w.host_iconic); // mock mirrors the authored intent
    }

    // Move first (so the hide can be shown to preserve the recorded geometry).
    SidecarUpdateToplevelRequest mv;
    mv.xid = 0x70;
    mv.generation = 2;
    mv.x = 300;
    mv.y = 250;
    mv.width = 200;
    mv.height = 100;
    VKR_CHECK(be.update_toplevel(mv).applied);

    // Hide: applied, epoch UNCHANGED, geometry cell preserved, host_visible flips false.
    SidecarSetVisibilityRequest hide;
    hide.xid = 0x70;
    hide.generation = 3;
    hide.visibility_state = static_cast<std::uint32_t>(VisibilityState::Hidden);
    const SidecarToplevelResponse hr = be.set_visibility(hide);
    VKR_CHECK(hr.applied);
    VKR_CHECK_EQ(hr.epoch, epoch0); // a hide is not a teardown -> epoch unchanged
    {
        const SidecarWindowInfo w = row(0x70);
        VKR_CHECK_EQ(w.visibility_state, static_cast<std::uint32_t>(VisibilityState::Hidden));
        VKR_CHECK(!w.host_visible);
        VKR_CHECK(w.has_actual && w.actual_x == 300 && w.actual_y == 250); // geometry preserved
    }

    // Restore: epoch still unchanged, geometry STILL preserved, host_visible back true.
    SidecarSetVisibilityRequest show;
    show.xid = 0x70;
    show.generation = 4;
    show.visibility_state = static_cast<std::uint32_t>(VisibilityState::Visible);
    const SidecarToplevelResponse sr = be.set_visibility(show);
    VKR_CHECK(sr.applied);
    VKR_CHECK_EQ(sr.epoch, epoch0);
    {
        const SidecarWindowInfo w = row(0x70);
        VKR_CHECK_EQ(w.visibility_state, static_cast<std::uint32_t>(VisibilityState::Visible));
        VKR_CHECK(w.host_visible);
        VKR_CHECK(w.actual_x == 300 && w.actual_y == 250);
    }

    // A stale/equal-generation visibility op is dropped.
    SidecarSetVisibilityRequest stale;
    stale.xid = 0x70;
    stale.generation = 3;
    stale.visibility_state = static_cast<std::uint32_t>(VisibilityState::Hidden);
    VKR_CHECK(!be.set_visibility(stale).applied);
    VKR_CHECK_EQ(row(0x70).visibility_state, static_cast<std::uint32_t>(VisibilityState::Visible));

    // Iconic mirrors the REAL backend. A minimized Win32 window
    // is STILL IsWindowVisible (only SW_HIDE clears it), so Iconic reports host_visible=true AND
    // host_iconic=true -- mock == real for a minimized window. epoch + geometry still preserved
    // (Iconic is a visibility state, not a teardown).
    SidecarSetVisibilityRequest mini;
    mini.xid = 0x70;
    mini.generation = 5;
    mini.visibility_state = static_cast<std::uint32_t>(VisibilityState::Iconic);
    const SidecarToplevelResponse mr = be.set_visibility(mini);
    VKR_CHECK(mr.applied);
    VKR_CHECK_EQ(mr.epoch, epoch0);
    {
        const SidecarWindowInfo w = row(0x70);
        VKR_CHECK_EQ(w.visibility_state, static_cast<std::uint32_t>(VisibilityState::Iconic));
        VKR_CHECK(w.host_visible && w.host_iconic); // minimized is still visible (mock == real)
        VKR_CHECK(w.has_actual && w.actual_x == 300 && w.actual_y == 250); // geometry preserved
    }

    // Destroy AFTER a hide still fully unregisters (the X window really went away).
    SidecarSetVisibilityRequest hide2;
    hide2.xid = 0x70;
    hide2.generation = 6;
    hide2.visibility_state = static_cast<std::uint32_t>(VisibilityState::Hidden);
    VKR_CHECK(be.set_visibility(hide2).applied);
    SidecarUnregisterToplevelRequest u;
    u.xid = 0x70;
    u.generation = 7;
    VKR_CHECK(be.unregister_toplevel(u).applied);
    VKR_CHECK(be.debug_enum_windows({}).windows.empty());
}

// Build a valid OK capture response with a solid w*h BGRA tail.
SidecarDebugCaptureWindowResponse make_capture_ok(std::uint32_t w, std::uint32_t h, unsigned char b,
                                                  unsigned char g, unsigned char r,
                                                  unsigned char a) {
    SidecarDebugCaptureWindowResponse resp;
    resp.ok = true;
    resp.status = "ok";
    resp.xid = 0x77;
    resp.layer = kCaptureLayerChrome;
    resp.representation = "placeholder";
    resp.width = w;
    resp.height = h;
    resp.stride = w * 4;
    resp.format = kCaptureFormatBgra8;
    resp.pixels.resize(static_cast<std::size_t>(w) * h * 4);
    for (std::size_t i = 0; i < resp.pixels.size(); i += 4) {
        resp.pixels[i + 0] = static_cast<char>(b);
        resp.pixels[i + 1] = static_cast<char>(g);
        resp.pixels[i + 2] = static_cast<char>(r);
        resp.pixels[i + 3] = static_cast<char>(a);
    }
    return resp;
}

// the capture REQUEST (JSON) + RESPONSE (binary) codecs round-trip, incl. the OK tail,
// a too_large result (no tail), and a mismatch result (no tail).
void test_capture_wire() {
    SidecarDebugCaptureWindowRequest rq;
    rq.xid = 0xABCD;
    rq.layer = kCaptureLayerCursor;
    rq.expected_epoch = 4;
    rq.expected_lifecycle_generation = 7;
    rq.min_last_seq = 2;
    const SidecarDebugCaptureWindowRequest rqb =
        SidecarDebugCaptureWindowRequest::from_body(rq.to_body());
    VKR_CHECK_EQ(rqb.xid, static_cast<std::uint64_t>(0xABCD));
    VKR_CHECK_EQ(rqb.layer, std::string(kCaptureLayerCursor));
    VKR_CHECK_EQ(rqb.expected_epoch, static_cast<std::uint64_t>(4));
    VKR_CHECK_EQ(rqb.expected_lifecycle_generation, static_cast<std::uint64_t>(7));
    VKR_CHECK_EQ(rqb.min_last_seq, static_cast<std::uint64_t>(2));

    // OK with a 2x2 tail.
    SidecarDebugCaptureWindowResponse ok = make_capture_ok(2, 2, 0x99, 0x66, 0x33, 0xFF);
    ok.epoch = 5;
    ok.generation = 9;
    ok.last_paint_seq = 3;
    ok.shown = true;
    std::string err;
    const SidecarDebugCaptureWindowResponse okb =
        SidecarDebugCaptureWindowResponse::from_wire(ok.to_wire(), err);
    VKR_CHECK(err.empty());
    VKR_CHECK(okb.ok && okb.status == "ok");
    VKR_CHECK_EQ(okb.width, static_cast<std::uint32_t>(2));
    VKR_CHECK_EQ(okb.height, static_cast<std::uint32_t>(2));
    VKR_CHECK_EQ(okb.stride, static_cast<std::uint32_t>(8));
    VKR_CHECK_EQ(okb.epoch, static_cast<std::uint64_t>(5));
    VKR_CHECK_EQ(okb.generation, static_cast<std::uint64_t>(9));
    VKR_CHECK_EQ(okb.last_paint_seq, static_cast<std::uint64_t>(3));
    VKR_CHECK(okb.shown);
    VKR_CHECK_EQ(okb.pixels.size(), static_cast<std::size_t>(16));
    VKR_CHECK_EQ(static_cast<unsigned char>(okb.pixels[0]), static_cast<unsigned char>(0x99));

    // too_large: no tail, needed_bytes carried.
    SidecarDebugCaptureWindowResponse tl;
    tl.ok = false;
    tl.status = "too_large";
    tl.xid = 0x77;
    tl.width = 5000;
    tl.height = 5000;
    tl.stride = 20000;
    tl.needed_bytes = 100000000ull;
    const SidecarDebugCaptureWindowResponse tlb =
        SidecarDebugCaptureWindowResponse::from_wire(tl.to_wire(), err);
    VKR_CHECK(err.empty());
    VKR_CHECK(!tlb.ok && tlb.status == "too_large");
    VKR_CHECK_EQ(tlb.needed_bytes, static_cast<std::uint64_t>(100000000ull));
    VKR_CHECK(tlb.pixels.empty());

    // mismatch: no tail, metadata carried.
    SidecarDebugCaptureWindowResponse mm;
    mm.ok = false;
    mm.status = "mismatch";
    mm.xid = 0x77;
    mm.epoch = 2;
    const SidecarDebugCaptureWindowResponse mmb =
        SidecarDebugCaptureWindowResponse::from_wire(mm.to_wire(), err);
    VKR_CHECK(err.empty() && !mmb.ok && mmb.status == "mismatch");
    VKR_CHECK(mmb.pixels.empty());
}

// the capture RESPONSE decoder is strict-but-total. Every
// malformed body sets `err` and returns a default, so a bad capture never reaches the PNG writer.
void test_capture_decoder_discipline() {
    std::string err;

    // Short body (< 4-byte prefix).
    (void) SidecarDebugCaptureWindowResponse::from_wire("ab", err);
    VKR_CHECK(!err.empty());

    // json header length over cap.
    std::string overcap(4, '\0');
    protocol::store_le32(static_cast<std::uint32_t>(kMaxCaptureJsonHeaderBytes + 1),
                         reinterpret_cast<unsigned char*>(&overcap[0]));
    overcap += "xx";
    (void) SidecarDebugCaptureWindowResponse::from_wire(overcap, err);
    VKR_CHECK(!err.empty());

    // OK but the tail is one byte short of width*height*4.
    std::string good = make_capture_ok(2, 2, 1, 2, 3, 0xFF).to_wire();
    (void) SidecarDebugCaptureWindowResponse::from_wire(good.substr(0, good.size() - 1), err);
    VKR_CHECK(!err.empty());

    // OK with stride smaller than a full row.
    SidecarDebugCaptureWindowResponse bad_stride = make_capture_ok(4, 2, 1, 2, 3, 0xFF);
    bad_stride.stride = 8; // < width(4)*4 == 16
    (void) SidecarDebugCaptureWindowResponse::from_wire(bad_stride.to_wire(), err);
    VKR_CHECK(!err.empty());

    // OK with a zero dimension.
    SidecarDebugCaptureWindowResponse zero;
    zero.ok = true;
    zero.status = "ok";
    zero.width = 0;
    zero.height = 2;
    zero.stride = 0;
    (void) SidecarDebugCaptureWindowResponse::from_wire(zero.to_wire(), err);
    VKR_CHECK(!err.empty());

    // OK with an unknown pixel format.
    SidecarDebugCaptureWindowResponse bad_fmt = make_capture_ok(2, 2, 1, 2, 3, 0xFF);
    bad_fmt.format = 42;
    (void) SidecarDebugCaptureWindowResponse::from_wire(bad_fmt.to_wire(), err);
    VKR_CHECK(!err.empty());

    // OK whose claimed height*stride exceeds the frame cap (caught before the tail check, so no
    // huge buffer is needed).
    SidecarDebugCaptureWindowResponse over;
    over.ok = true;
    over.status = "ok";
    over.width = 2;
    over.height = 2;
    over.stride = static_cast<std::uint32_t>(kMaxCapturePayloadBytes / 2 + 4);
    (void) SidecarDebugCaptureWindowResponse::from_wire(over.to_wire(), err);
    VKR_CHECK(!err.empty());

    // A non-OK response must carry NO tail.
    std::string nonok = make_capture_ok(0, 0, 0, 0, 0, 0).to_wire(); // start from a frame
    {
        SidecarDebugCaptureWindowResponse mm;
        mm.ok = false;
        mm.status = "mismatch";
        nonok = mm.to_wire();
    }
    nonok += "X"; // smuggle a tail onto a non-ok response
    (void) SidecarDebugCaptureWindowResponse::from_wire(nonok, err);
    VKR_CHECK(!err.empty());

    // ok/status contract: ok=true but status != "ok".
    SidecarDebugCaptureWindowResponse incon = make_capture_ok(2, 2, 1, 2, 3, 0xFF);
    incon.status = "mismatch"; // ok stays true -> inconsistent
    (void) SidecarDebugCaptureWindowResponse::from_wire(incon.to_wire(), err);
    VKR_CHECK(!err.empty());

    // ok=false but status == "ok" (and no tail).
    SidecarDebugCaptureWindowResponse incon2;
    incon2.ok = false;
    incon2.status = "ok";
    (void) SidecarDebugCaptureWindowResponse::from_wire(incon2.to_wire(), err);
    VKR_CHECK(!err.empty());

    // An unknown status token is rejected.
    SidecarDebugCaptureWindowResponse unk;
    unk.ok = false;
    unk.status = "weird";
    (void) SidecarDebugCaptureWindowResponse::from_wire(unk.to_wire(), err);
    VKR_CHECK(!err.empty());

    // The valid frame still decodes clean (sanity).
    (void) SidecarDebugCaptureWindowResponse::from_wire(good, err);
    VKR_CHECK(err.empty());
}

// the mock's debug_capture_window contract (mock == real: the real backend builds the
// same statuses from the window-thread DIB/cursor -- see integration_real_backend
// test_real_capture).
void test_capture_mock() {
    vkrpc::MockVulkanBackend be("NVIDIA T1200 Laptop GPU");

    // Unknown layer -> bad_layer.
    VKR_CHECK_EQ(be.debug_capture_window({0x30, "bogus", 0, 0, 0}).status,
                 std::string("bad_layer"));
    // No window -> absent.
    VKR_CHECK_EQ(be.debug_capture_window({0x30, kCaptureLayerChrome, 0, 0, 0}).status,
                 std::string("absent"));

    // Register a placeholder + paint chrome (8x4 solid 0xFF332211) -> chrome captures the pixels.
    SidecarRegisterToplevelRequest reg;
    reg.xid = 0x30;
    reg.generation = 5;
    reg.role = "toplevel";
    VKR_CHECK(be.register_toplevel(reg).applied);
    VKR_CHECK(be.paint_chrome(make_paint(0x30, 5, 1, 8, 4, 0x11, 0x22, 0x33, 0xFF)).applied);

    SidecarDebugCaptureWindowResponse cap =
        be.debug_capture_window({0x30, kCaptureLayerChrome, 0, 0, 0});
    VKR_CHECK(cap.ok && cap.status == "ok");
    VKR_CHECK_EQ(cap.representation, std::string("placeholder"));
    VKR_CHECK(cap.shown);
    VKR_CHECK_EQ(cap.generation, static_cast<std::uint64_t>(5));
    VKR_CHECK(cap.epoch != 0);
    VKR_CHECK_EQ(cap.width, static_cast<std::uint32_t>(8));
    VKR_CHECK_EQ(cap.height, static_cast<std::uint32_t>(4));
    VKR_CHECK_EQ(cap.stride, static_cast<std::uint32_t>(32));
    VKR_CHECK_EQ(cap.pixels.size(), static_cast<std::size_t>(8 * 4 * 4));
    VKR_CHECK_EQ(static_cast<unsigned char>(cap.pixels[0]), static_cast<unsigned char>(0x11));
    VKR_CHECK_EQ(static_cast<unsigned char>(cap.pixels[2]), static_cast<unsigned char>(0x33));

    // Cursor layer: none built yet -> empty.
    VKR_CHECK_EQ(be.debug_capture_window({0x30, kCaptureLayerCursor, 0, 0, 0}).status,
                 std::string("empty"));
    // Build a cursor (registered + live epoch) -> cursor captures it + hotspot.
    SidecarSetCursorRequest cur = make_cursor(0x30, 4, 2, 1, 0, 0x99, 0x66, 0x33, 0xFF);
    cur.epoch = be.debug_registry_epoch_for_xid(0x30);
    VKR_CHECK(be.set_cursor(cur).applied);
    const SidecarDebugCaptureWindowResponse ccap =
        be.debug_capture_window({0x30, kCaptureLayerCursor, 0, 0, 0});
    VKR_CHECK(ccap.ok && ccap.status == "ok");
    VKR_CHECK_EQ(ccap.width, static_cast<std::uint32_t>(4));
    VKR_CHECK_EQ(ccap.height, static_cast<std::uint32_t>(2));
    VKR_CHECK_EQ(ccap.xhot, 1);

    // Lifecycle selectors: a wrong expected_* yields mismatch.
    SidecarDebugCaptureWindowRequest mreq{0x30, kCaptureLayerChrome, 0, 999, 0};
    VKR_CHECK_EQ(be.debug_capture_window(mreq).status, std::string("mismatch"));
    SidecarDebugCaptureWindowRequest ereq{0x30, kCaptureLayerChrome, 0, 0, 0};
    ereq.expected_epoch = cap.epoch + 100;
    VKR_CHECK_EQ(be.debug_capture_window(ereq).status, std::string("mismatch"));
    SidecarDebugCaptureWindowRequest sreq{0x30, kCaptureLayerChrome, 0, 0, 0};
    sreq.min_last_seq = 1000;
    VKR_CHECK_EQ(be.debug_capture_window(sreq).status, std::string("mismatch"));
    // The matching epoch + generation still captures.
    SidecarDebugCaptureWindowRequest good{0x30, kCaptureLayerChrome, 0, 0, 0};
    good.expected_epoch = cap.epoch;
    good.expected_lifecycle_generation = 5;
    VKR_CHECK(be.debug_capture_window(good).ok);

    // too_large: a 1-row source just over the frame cap -> structured too_large, no pixels.
    const std::uint32_t over_w = static_cast<std::uint32_t>(kMaxCapturePayloadBytes / 4 + 1);
    SidecarRegisterToplevelRequest big;
    big.xid = 0x31;
    big.generation = 1;
    VKR_CHECK(be.register_toplevel(big).applied);
    VKR_CHECK(be.paint_chrome(make_paint(0x31, 1, 1, over_w, 1, 0x11, 0x22, 0x33, 0xFF)).applied);
    const SidecarDebugCaptureWindowResponse big_cap =
        be.debug_capture_window({0x31, kCaptureLayerChrome, 0, 0, 0});
    VKR_CHECK(!big_cap.ok && big_cap.status == "too_large");
    VKR_CHECK(big_cap.needed_bytes > kMaxCapturePayloadBytes);
    VKR_CHECK(big_cap.pixels.empty());
}

} // namespace

// the pure chrome-recapture scheduler (warm-up burst -> steady placeholder poll,
// ship-on-change
// + heartbeat, stop-on-surface, resize holdoff). Pure -> identical on both platforms.
void test_chrome_recapture_policy() {
    using vkr::sidecar::ChromeRecapturePolicy;
    using vkr::sidecar::ChromeShipDecision;
    namespace sc = vkr::sidecar;

    // Warm-up: due immediately, throttled to the warm-up interval, and ALWAYS ships (even
    // identical).
    {
        ChromeRecapturePolicy p;
        p.track(0xA1, 0);
        VKR_CHECK(p.tracking(0xA1));
        VKR_CHECK(p.due(0xA1, 0)); // never captured -> due
        p.record_capture(0xA1, 0);
        VKR_CHECK(!p.due(0xA1, 0));                               // just captured
        VKR_CHECK(!p.due(0xA1, sc::kChromeWarmupIntervalMs - 1)); // interval not elapsed
        VKR_CHECK(p.due(0xA1, sc::kChromeWarmupIntervalMs));      // warm-up interval elapsed
        VKR_CHECK(p.decide_ship(0xA1, 0, 111) == ChromeShipDecision::Ship);
        p.record_ship(0xA1, 0, 111);
        // identical hash during warm-up still ships
        VKR_CHECK(p.decide_ship(0xA1, sc::kChromeWarmupIntervalMs, 111) ==
                  ChromeShipDecision::Ship);
    }

    // Warm-up -> steady: interval widens to the steady rate; steady ships only on a hash change.
    {
        ChromeRecapturePolicy p;
        p.track(0xB2, 0);
        const std::uint64_t t = sc::kChromeWarmupDurationMs + 10; // past warm-up
        p.record_capture(0xB2, t);
        p.record_ship(0xB2, t, 0xAAAA); // worker now holds AAAA
        VKR_CHECK(!p.due(0xB2, t + sc::kChromeSteadyIntervalMs - 1));
        VKR_CHECK(p.due(0xB2, t + sc::kChromeSteadyIntervalMs));
        VKR_CHECK(p.decide_ship(0xB2, t + sc::kChromeSteadyIntervalMs, 0xAAAA) ==
                  ChromeShipDecision::Suppress); // unchanged -> suppress
        VKR_CHECK(p.decide_ship(0xB2, t + sc::kChromeSteadyIntervalMs, 0xBBBB) ==
                  ChromeShipDecision::Ship); // changed -> ship
    }

    // Steady heartbeat: unchanged content still ships once the heartbeat elapses (so a
    // static-chrome surface promotion is still learned, and the worker stays fresh).
    {
        ChromeRecapturePolicy p;
        p.track(0xC3, 0);
        const std::uint64_t t = sc::kChromeWarmupDurationMs + 10;
        p.record_ship(0xC3, t, 0x1234);
        VKR_CHECK(p.decide_ship(0xC3, t + sc::kChromeHeartbeatMs - 1, 0x1234) ==
                  ChromeShipDecision::Suppress);
        VKR_CHECK(p.decide_ship(0xC3, t + sc::kChromeHeartbeatMs, 0x1234) ==
                  ChromeShipDecision::Ship);
    }

    // Stop on a non-placeholder representation (surface promotion / gone).
    {
        ChromeRecapturePolicy p;
        p.track(0xD4, 0);
        p.note_representation(0xD4, "placeholder"); // still a placeholder -> keep going
        VKR_CHECK(!p.stopped(0xD4));
        VKR_CHECK(p.due(0xD4, 0));
        p.note_representation(0xD4, "surface"); // promoted -> stop
        VKR_CHECK(p.stopped(0xD4));
        VKR_CHECK(!p.due(0xD4, 100000));
    }

    // Resize holdoff defers capture until geometry settles.
    {
        ChromeRecapturePolicy p;
        p.track(0xE5, 0);
        p.record_capture(0xE5, 0);
        p.on_resize(0xE5, 1000);
        VKR_CHECK(!p.due(0xE5, 1000));
        VKR_CHECK(!p.due(0xE5, 1000 + sc::kChromeResizeHoldoffMs - 1));
        VKR_CHECK(p.due(0xE5, 1000 + sc::kChromeResizeHoldoffMs));
    }

    // XComposite named-pixmap invalidation is driven by EVERY realized size edge. This catches a
    // coalesced A -> B -> A resize: A -> B invalidates the old name; while invalid, B -> A leaves
    // it invalid so the settled capture must name the new A-sized backing pixmap. A position-only
    // configure at A does not churn the current name.
    {
        VKR_CHECK(!sc::chrome_named_pixmap_invalidated_by_configure(true, 150, 100, 150, 100));
        VKR_CHECK(sc::chrome_named_pixmap_invalidated_by_configure(true, 150, 100, 900, 700));
        VKR_CHECK(!sc::chrome_named_pixmap_invalidated_by_configure(false, 0, 0, 150, 100));
    }

    // XShape BOUNDING mask: full/default regions are byte-identical; a center region preserves its
    // pixels and deterministically blacks out everything else; negative/outside coordinates clip.
    {
        std::vector<unsigned char> pixels(4 * 3 * 4, 0);
        for (std::size_t i = 0; i < pixels.size(); i += 4) {
            pixels[i + 0] = 0x99;
            pixels[i + 1] = 0x66;
            pixels[i + 2] = 0x33;
            pixels[i + 3] = 0xFF;
        }
        const std::vector<unsigned char> original = pixels;
        VKR_CHECK(
            sc::chrome_mask_outside_shape(pixels.data(), pixels.size(), 4, 3, {{0, 0, 4, 3}}));
        VKR_CHECK(pixels == original);

        VKR_CHECK(
            sc::chrome_mask_outside_shape(pixels.data(), pixels.size(), 4, 3, {{1, 1, 2, 1}}));
        auto is_black = [&](int x, int y) {
            const std::size_t i = (static_cast<std::size_t>(y) * 4 + x) * 4;
            return pixels[i] == 0 && pixels[i + 1] == 0 && pixels[i + 2] == 0 &&
                   pixels[i + 3] == 0xFF;
        };
        VKR_CHECK(is_black(0, 0));
        VKR_CHECK(!is_black(1, 1) && !is_black(2, 1));
        VKR_CHECK(is_black(3, 2));

        pixels = original;
        VKR_CHECK(
            sc::chrome_mask_outside_shape(pixels.data(), pixels.size(), 4, 3, {{-2, -1, 3, 2}}));
        VKR_CHECK(!is_black(0, 0)); // clipped one-pixel intersection survives
        VKR_CHECK(is_black(1, 0));
        VKR_CHECK(!sc::chrome_mask_outside_shape(pixels.data(), pixels.size(), 4, 3,
                                                 {{0, 0, 4, 3}, {0, 0, 1, 1}}));
        VKR_CHECK(!sc::chrome_mask_outside_shape(nullptr, 0, 4, 3, {}));
    }

    // in_resize_holdoff is the PURE gate the ConfigureNotify driver uses to skip its
    // immediate cap_ship during an active resize. Untracked -> false (a one-shot window not in the
    // policy keeps immediate ship); a tracked window with no resize -> false; within (now <
    // holdoff_until) -> true; at/after the holdoff boundary -> false. It is separate from due()
    // (which also folds the cadence interval).
    {
        ChromeRecapturePolicy p;
        VKR_CHECK(!p.in_resize_holdoff(0xE6, 1000)); // untracked
        p.track(0xE6, 0);
        VKR_CHECK(!p.in_resize_holdoff(0xE6, 1000)); // tracked, no resize yet
        p.on_resize(0xE6, 1000);
        VKR_CHECK(p.in_resize_holdoff(0xE6, 1000)); // at the resize
        VKR_CHECK(p.in_resize_holdoff(0xE6, 1000 + sc::kChromeResizeHoldoffMs - 1)); // within
        VKR_CHECK(!p.in_resize_holdoff(0xE6, 1000 + sc::kChromeResizeHoldoffMs)); // boundary -> due
        VKR_CHECK(!p.in_resize_holdoff(0xE6, 1000 + sc::kChromeResizeHoldoffMs + 5)); // past
        // A SECOND resize re-arms the holdoff window from the new now.
        p.on_resize(0xE6, 5000);
        VKR_CHECK(p.in_resize_holdoff(0xE6, 5000 + sc::kChromeResizeHoldoffMs - 1));
        VKR_CHECK(!p.in_resize_holdoff(0xE6, 5000 + sc::kChromeResizeHoldoffMs));
    }

    // forget() stops tracking; calls on an untracked xid are no-ops.
    {
        ChromeRecapturePolicy p;
        p.track(0xF6, 0);
        p.forget(0xF6);
        VKR_CHECK(!p.tracking(0xF6));
        VKR_CHECK(!p.due(0xF6, 0));
        p.record_capture(0x999, 5);
        p.record_ship(0x999, 5, 1);
        p.note_representation(0x999, "surface");
        VKR_CHECK(!p.tracking(0x999));
        VKR_CHECK(p.decide_ship(0x999, 5, 1) == ChromeShipDecision::Suppress);
    }

    // C (black-flash fix): holding_for_first_content gates the premature all-black map frame.
    {
        ChromeRecapturePolicy p;
        VKR_CHECK(!p.holding_for_first_content(0x1B, 0)); // untracked -> not holding
        p.track(0x1B, 0);
        VKR_CHECK(p.holding_for_first_content(0x1B, 0)); // tracked, nothing
                                                         // shipped, in warm-up
        VKR_CHECK(p.holding_for_first_content(0x1B, sc::kChromeWarmupDurationMs - 1)); // still warm
        VKR_CHECK(
            !p.holding_for_first_content(0x1B, sc::kChromeWarmupDurationMs)); // warm-up over
                                                                              // -> stop holding
        // Once ANY frame ships, never hold again (a window that later goes black ships normally).
        p.record_ship(0x1B, 10, 0xABCD);
        VKR_CHECK(!p.holding_for_first_content(0x1B, 20));
        // A surface promotion (stopped) also ends holding.
        ChromeRecapturePolicy q;
        q.track(0x1C, 0);
        q.note_representation(0x1C, "surface");
        VKR_CHECK(!q.holding_for_first_content(0x1C, 0));
    }

    // Re-arm (restore) restarts the warm-up burst even mid-steady.
    {
        ChromeRecapturePolicy p;
        p.track(0x17, 0);
        const std::uint64_t t = sc::kChromeWarmupDurationMs + 10;
        p.record_ship(0x17, t, 0x55);
        VKR_CHECK(p.decide_ship(0x17, t, 0x55) ==
                  ChromeShipDecision::Suppress);                             // steady, unchanged
        p.track(0x17, t);                                                    // re-arm
        VKR_CHECK(p.decide_ship(0x17, t, 0x55) == ChromeShipDecision::Ship); // warm-up again
    }

    // C: a classified POPUP has the SAME recapture lifecycle as a placeholder toplevel now -- track
    // on map, ship the paint-AFTER-map content during the warm-up burst, forget on unmap (no due
    // while hidden), and re-track on a same-XID remap (warm-up burst again). This is the
    // pure-policy shape behind popup recapture; the sidecar driver wires the popup's X lifecycle
    // (register /
    // UnmapNotify / DestroyNotify / popup MapNotify remap) to exactly these calls.
    {
        ChromeRecapturePolicy p;
        const std::uint64_t popup = 0x5a5a;
        p.track(popup, 0); // MapNotify: classify + register the popup
        VKR_CHECK(p.tracking(popup));
        VKR_CHECK(
            p.due(popup, 0)); // never captured -> due immediately (first, pre-content capture)
        p.record_ship(popup, 0, 0xB1AC); // the one-shot map capture: BLACK (pre-content)
        // The menu paints its items a beat later; warm-up ships the NEW content hash (and would
        // ship even an identical hash while in warm-up) so the real content reaches the worker.
        VKR_CHECK(p.decide_ship(popup, sc::kChromeWarmupIntervalMs, 0xC0DE) ==
                  ChromeShipDecision::Ship);
        p.record_ship(popup, sc::kChromeWarmupIntervalMs, 0xC0DE);
        // UnmapNotify forgets the transient popup: no longer tracked, never due while hidden.
        p.forget(popup);
        VKR_CHECK(!p.tracking(popup));
        VKR_CHECK(!p.due(popup, sc::kChromeWarmupDurationMs + 1000));
        // A same-XID remap re-tracks: due again, warm-up ships even an identical
        // hash -- otherwise a reshown menu would fall back to the one-shot black capture.
        const std::uint64_t t2 = sc::kChromeWarmupDurationMs + 2000;
        p.track(popup, t2);
        VKR_CHECK(p.tracking(popup));
        VKR_CHECK(p.due(popup, t2));
        VKR_CHECK(p.decide_ship(popup, t2, 0xC0DE) == ChromeShipDecision::Ship); // warm-up again
    }

    // Content hash: stable for identical input, sensitive to pixels AND dims (normalized BGRA).
    {
        const unsigned char a[] = {1, 2, 3, 4, 5, 6, 7, 8};
        const unsigned char b[] = {1, 2, 3, 4, 5, 6, 7, 9};
        const std::uint64_t ha = sc::chrome_content_hash(a, sizeof(a), 2, 1, 8, 0);
        VKR_CHECK(ha == sc::chrome_content_hash(a, sizeof(a), 2, 1, 8, 0)); // stable
        VKR_CHECK(ha != sc::chrome_content_hash(b, sizeof(b), 2, 1, 8, 0)); // pixel change
        VKR_CHECK(ha != sc::chrome_content_hash(a, sizeof(a), 1, 2, 8, 0)); // dim change
    }

    // C (black-flash fix): chrome_frame_is_black -- BLACK (B==G==R==0, alpha ignored) vs any
    // non-black pixel; a SOLID non-black colour (the popup canary's 0x9933CC) is NOT black.
    {
        const unsigned char black2[] = {0, 0, 0, 0xFF, 0, 0, 0, 0xFF}; // 2 opaque-black px
        VKR_CHECK(sc::chrome_frame_is_black(black2, sizeof(black2)));
        const unsigned char black_zero_alpha[] = {0, 0, 0, 0}; // alpha ignored -> still black
        VKR_CHECK(sc::chrome_frame_is_black(black_zero_alpha, sizeof(black_zero_alpha)));
        // BGRA for 0x9933CC = B=0xCC, G=0x33, R=0x99 -> NOT black.
        const unsigned char popup[] = {0xCC, 0x33, 0x99, 0xFF};
        VKR_CHECK(!sc::chrome_frame_is_black(popup, sizeof(popup)));
        // One non-black pixel among black ones -> not black (short-circuit).
        const unsigned char mixed[] = {0, 0, 0, 0xFF, 0, 0, 1, 0xFF};
        VKR_CHECK(!sc::chrome_frame_is_black(mixed, sizeof(mixed)));
        // Malformed / empty -> false (never suppress a bad frame).
        VKR_CHECK(!sc::chrome_frame_is_black(nullptr, 0));
        const unsigned char partial[] = {0, 0, 0}; // not 4-aligned
        VKR_CHECK(!sc::chrome_frame_is_black(partial, sizeof(partial)));
    }
}

// pure default-placement helpers (centering math, WM_NORMAL_HINTS position-intent parse, the
// worker on-screen clamp). Pure -> identical on both platforms.
void test_window_placement() {
    namespace sc = vkr::sidecar;

    // centered_client_origin: centers, clamps non-negative, oversize -> 0 on that axis.
    {
        sc::ClientOrigin o = sc::centered_client_origin(1000, 800, 200, 100);
        VKR_CHECK_EQ(o.x, 400);
        VKR_CHECK_EQ(o.y, 350);
        o = sc::centered_client_origin(100, 800, 200, 100); // client wider than root -> x=0
        VKR_CHECK_EQ(o.x, 0);
        VKR_CHECK_EQ(o.y, 350);
        o = sc::centered_client_origin(300, 300, 300, 300); // equal -> 0,0
        VKR_CHECK_EQ(o.x, 0);
        VKR_CHECK_EQ(o.y, 0);
    }

    // clamp_client_origin_to_root: per-axis on-screen clamp of an app/sidecar-authored
    // client origin. On-screen (incl. 0,0) unchanged; negative -> 0; past the right/bottom edge ->
    // pinned to root-client; client >= root on an axis -> 0 on that axis.
    {
        // on-screen origin unchanged (incl. the 0,0 identity that glxgears / cursor canary rely on)
        sc::ClientOrigin o = sc::clamp_client_origin_to_root(1000, 800, 200, 150, 300, 200);
        VKR_CHECK_EQ(o.x, 200);
        VKR_CHECK_EQ(o.y, 150);
        o = sc::clamp_client_origin_to_root(1000, 800, 0, 0, 300, 200);
        VKR_CHECK_EQ(o.x, 0);
        VKR_CHECK_EQ(o.y, 0);
        // off the top-LEFT (both axes negative) -> pinned to 0,0 (the observed -32,-32 menu-bar
        // bug)
        o = sc::clamp_client_origin_to_root(1000, 800, -100, -100, 300, 200);
        VKR_CHECK_EQ(o.x, 0);
        VKR_CHECK_EQ(o.y, 0);
        // per-axis: off the LEFT only -> x=0, y preserved (the ConfigureRequest clamp case)
        o = sc::clamp_client_origin_to_root(1000, 800, -50, 100, 300, 200);
        VKR_CHECK_EQ(o.x, 0);
        VKR_CHECK_EQ(o.y, 100);
        // past the right/bottom edge -> pinned so the client stays fully on-root
        o = sc::clamp_client_origin_to_root(1000, 800, 900, 700, 300, 200);
        VKR_CHECK_EQ(o.x, 700); // 1000 - 300
        VKR_CHECK_EQ(o.y, 600); // 800 - 200
        // client at least as large as the root on an axis -> pin that axis to 0
        o = sc::clamp_client_origin_to_root(1000, 800, -20, 300, 1000, 200);
        VKR_CHECK_EQ(o.x, 0);   // width == root_w -> x pinned
        VKR_CHECK_EQ(o.y, 300); // y still in range
    }

    // wm_hints_has_explicit_position: position flags only; missing/size-only/empty -> no intent.
    {
        VKR_CHECK(!sc::wm_hints_has_explicit_position(nullptr, 0));
        const std::uint32_t none = 0;
        VKR_CHECK(!sc::wm_hints_has_explicit_position(&none, 1));
        const std::uint32_t size_only = sc::kWmSizeHintUSPosition
                                        << 1; // USSize (1<<1), not position
        VKR_CHECK(!sc::wm_hints_has_explicit_position(&size_only, 1));
        const std::uint32_t us = sc::kWmSizeHintUSPosition;
        VKR_CHECK(sc::wm_hints_has_explicit_position(&us, 1));
        const std::uint32_t pp = sc::kWmSizeHintPPosition;
        VKR_CHECK(sc::wm_hints_has_explicit_position(&pp, 1));
        const std::uint32_t both = sc::kWmSizeHintUSPosition | sc::kWmSizeHintPPosition;
        VKR_CHECK(sc::wm_hints_has_explicit_position(&both, 1));
    }

    // clamp_frame_to_work_area: fits -> unchanged; off each edge -> pushed in (size preserved);
    // oversize -> pinned to top/left; honors a non-origin work area (taskbar).
    {
        // fully inside -> no change
        sc::FramePos p = sc::clamp_frame_to_work_area(100, 100, 400, 400, 0, 0, 1920, 1040);
        VKR_CHECK_EQ(p.left, 100);
        VKR_CHECK_EQ(p.top, 100);
        // chrome above the top (top<0) -> pushed down; size 300x300 preserved
        p = sc::clamp_frame_to_work_area(10, -30, 310, 270, 0, 0, 1920, 1040);
        VKR_CHECK_EQ(p.left, 10);
        VKR_CHECK_EQ(p.top, 0);
        // off the right + bottom -> pushed in (frame 200x200)
        p = sc::clamp_frame_to_work_area(1900, 1000, 2100, 1200, 0, 0, 1920, 1040);
        VKR_CHECK_EQ(p.left, 1720); // 1920 - 200
        VKR_CHECK_EQ(p.top, 840);   // 1040 - 200
        // larger than the work area -> pin to top/left
        p = sc::clamp_frame_to_work_area(0, 0, 3000, 2000, 0, 0, 1920, 1040);
        VKR_CHECK_EQ(p.left, 0);
        VKR_CHECK_EQ(p.top, 0);
        // non-origin work area (top taskbar at y<40) -> push below it
        p = sc::clamp_frame_to_work_area(10, 10, 310, 310, 0, 40, 1920, 1080);
        VKR_CHECK_EQ(p.left, 10);
        VKR_CHECK_EQ(p.top, 40);
    }

    // the accepted root is the surface policy ceiling, intersected with a stable Vulkan
    // physical-device image/framebuffer limit. A legacy/direct-test zero root falls back to that
    // device limit -- never a current Win32 surface extent or monitor work-area sample.
    {
        VKR_CHECK_EQ(sc::surface_extent_ceiling_axis(7600, 16384),
                     static_cast<std::uint32_t>(7600));
        VKR_CHECK_EQ(sc::surface_extent_ceiling_axis(20000, 16384),
                     static_cast<std::uint32_t>(16384));
        VKR_CHECK_EQ(sc::surface_extent_ceiling_axis(0, 16384), static_cast<std::uint32_t>(16384));
    }

    // cap_maxsize_to_client: reduce the OUTER maximize/drag limits per axis so the CLIENT never
    // exceeds the guest root. The client cap is converted to outer via the caller's frame deltas
    // (AdjustWindowRectExForDpi at the live DPI); this pure helper tests the per-axis-min
    // arithmetic across DPI/chrome variants + the never-enlarge / never-cap guards. Scenario
    // mirrors the bug: guest root 2560x1528, a larger secondary monitor whose Win32 default max is
    // ~3840x1552 outer.
    {
        const int root_w = 2560, root_h = 1528;
        // A generous monitor default (a larger secondary monitor) so the outer cap falls below it
        // on both axes at every DPI and the per-axis min actually applies -- the point of these
        // cases.
        const int def = 3856, defh = 2100;
        // 96 DPI standard chrome (~8px borders, ~31px caption): extra 16x39 -> outer cap 2576x1567
        // -> capped on BOTH axes (max + track alike).
        sc::MinMaxCap c = sc::cap_maxsize_to_client(root_w, root_h, 16, 39, def, defh, def, defh);
        VKR_CHECK_EQ(c.max_size_x, 2576);
        VKR_CHECK_EQ(c.max_size_y, 1567);
        VKR_CHECK_EQ(c.max_track_x, 2576);
        VKR_CHECK_EQ(c.max_track_y, 1567);
        // 144 DPI (1.5x) chrome: extra 24x59 -> outer cap 2584x1587 (the conversion tracks DPI).
        c = sc::cap_maxsize_to_client(root_w, root_h, 24, 59, def, defh, def, defh);
        VKR_CHECK_EQ(c.max_size_x, 2584);
        VKR_CHECK_EQ(c.max_size_y, 1587);
        // 192 DPI (2x) chrome: extra 32x78 -> outer cap 2592x1606.
        c = sc::cap_maxsize_to_client(root_w, root_h, 32, 78, def, defh, def, defh);
        VKR_CHECK_EQ(c.max_size_x, 2592);
        VKR_CHECK_EQ(c.max_size_y, 1606);
        // Borderless WS_POPUP: 0 frame delta -> outer cap == client cap exactly.
        c = sc::cap_maxsize_to_client(root_w, root_h, 0, 0, 3840, 1552, 3840, 1552);
        VKR_CHECK_EQ(c.max_size_x, 2560);
        VKR_CHECK_EQ(c.max_size_y, 1528);
        // Cap ABOVE the monitor default (a guest root larger than this monitor): preserve the
        // smaller monitor-specific MAXIMIZE default, but raise the TRACK limit exactly to the
        // guest-root outer extent. A programmatic root-sized SetWindowPos is otherwise truncated
        // by Win32's virtual-screen-derived default ptMaxTrackSize (the live 7600x2160 root
        // realized as 7600x2141 before this contract).
        c = sc::cap_maxsize_to_client(5000, 5000, 16, 39, 1936, 1056, 1936, 1056);
        VKR_CHECK_EQ(c.max_size_x, 1936);
        VKR_CHECK_EQ(c.max_size_y, 1056);
        VKR_CHECK_EQ(c.max_track_x, 5016);
        VKR_CHECK_EQ(c.max_track_y, 5039);
        // Mixed axes: root narrower but TALLER than the monitor -> cap x, keep y.
        c = sc::cap_maxsize_to_client(2560, 4000, 16, 39, 3840, 1552, 3840, 1552);
        VKR_CHECK_EQ(c.max_size_x, 2576);
        VKR_CHECK_EQ(c.max_size_y, 1552);
        VKR_CHECK_EQ(c.max_track_x, 2576);
        VKR_CHECK_EQ(c.max_track_y, 4039);
        // Guards: an unknown root (0) or a bogus (negative) frame delta -> pass the defaults
        // through unchanged (never cap, never enlarge).
        c = sc::cap_maxsize_to_client(0, 0, 16, 39, 3840, 1552, 3840, 1552);
        VKR_CHECK_EQ(c.max_size_x, 3840);
        VKR_CHECK_EQ(c.max_size_y, 1552);
        c = sc::cap_maxsize_to_client(root_w, root_h, -1, -1, 3840, 1552, 3840, 1552);
        VKR_CHECK_EQ(c.max_size_x, 3840);
        VKR_CHECK_EQ(c.max_size_y, 1552);
    }
}

int main() {
    test_popup_lifecycle_order();
    test_negotiate_request_round_trip();
    test_negotiate_response_round_trip();
    test_ready_round_trip();
    test_toplevel_messages_round_trip();
    test_registry_surface_first();
    test_registry_toplevel_first_promote();
    test_registry_surface_lost_while_registered();
    test_registry_unregister_with_live_surface();
    test_registry_unregister_placeholder();
    test_registry_generation_gating();
    test_registry_two_xid_independence();
    test_registry_surface_specific_unbind();
    test_registry_apply_geometry();
    test_registry_place_on_establish();
    test_registry_resize_authority();
    test_registry_z_order();
    test_registry_set_visibility();
    test_registry_owned_popups();
    test_registry_popup_register();
    test_registry_popup_owner_cascade();
    test_popup_plane_mock();
    test_popup_classify_sanity();
    test_chrome_messages_round_trip();
    test_chrome_wire_tiling_bounds();
    test_chrome_decoder_discipline();
    test_registry_chrome_accept_commit();
    test_input_messages_round_trip();
    test_input_queue_basic();
    test_input_queue_motion_coalescing();
    test_input_queue_overflow_priority();
    test_input_queue_protected_overflow();
    test_input_plane_epoch_gate();
    test_input_epoch_reconcile();
    test_input_plane_promotion_adopt();
    test_registry_surface_remap_epoch();
    test_input_plane_surface_remap();
    test_cursor_messages_round_trip();
    test_cursor_decoder_discipline();
    test_cursor_plane_mock();
    test_cursor_plane_surface_remap();
    test_enum_windows_wire();
    test_set_visibility_wire();
    test_enum_windows_mock();
    test_apply_geometry_mock();
    test_apply_geometry_mock_overwrite();
    test_resize_convergence_mock();
    test_z_order_mock();
    test_set_visibility_mock();
    test_capture_wire();
    test_capture_decoder_discipline();
    test_capture_mock();
    test_chrome_recapture_policy();
    test_window_placement();
    return vkr::test::finish("unit_sidecar");
}
