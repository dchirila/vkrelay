// vkrelay2 sidecar: the X11 window-manager / geometry-authority process for a
// private-display app session.
//
// The readiness barrier: connect to the worker's sidecar plane (SidecarHello +
// token, then the binary RPC envelope), claim the WM selection on the private X display, probe
// required extension presence, subscribe to substructure events, do an initial root scan, emit
// `sidecar_ready` to the worker, and THEN raise its `--ready-fd` edge so the launcher can release
// the app launch -- never log-scraped, never inferred from an external X11 probe.
//
// Guest-toplevel LIFECYCLE forwarding to the worker registry: the initial scan registers
// every currently-mapped, non-override_redirect root child (register_toplevel); the event loop then
// forwards MapRequest -> register, ConfigureRequest -> update (geometry/debug state only),
// and UnmapNotify/DestroyNotify -> unregister. Every forward carries a monotonic generation so the
// worker's strictly-newer-wins gate never loses a re-mapped toplevel to a stale unmap. The sidecar
// stays a TRANSPARENT pass-through WM here; live geometry AUTHORITY (overriding/constraining guest
// geometry) comes later.
//
// XComposite CHROME CAPTURE: for each tracked toplevel the sidecar redirects the
// window (AUTOMATIC) + names its backing pixmap, then on the capture triggers (initial scan / first
// Expose after a MapRequest / ConfigureNotify) reads the pixmap (xcb_get_image), converts the
// visual to BGRA8 (fail-closed on an unsupported visual), and ships the pixels to the worker via
// the binary PaintChrome op stamped with the toplevel's CURRENT lifecycle generation + a
// per-toplevel seq. The worker paints them into the placeholder. The whole capture half is
// conditionally compiled on libxcb-composite (VKRELAY2_HAVE_XCB_COMPOSITE); without it the sidecar
// still does lifecycle forwarding.
//
// Linux-only (xcb). Built only when libxcb is present (see CMakeLists). Core xcb always provides
// the WM path; XComposite/XShape/XTest/XFixes are separately compiled and linked when their
// development libraries exist, then runtime-gated by xcb_query_extension.
#include "common/logging/logging.hpp"
#include "common/protocol/messages.hpp"
#include "common/sidecar/sidecar_session.hpp"
#include "common/transport/tcp.hpp"
#include "common/transport/transport.hpp"
#include "common/util/json.hpp"
#include "common/vkrpc/rpc.hpp"

#include "common/sidecar/chrome_recapture.hpp" // pure recapture scheduler
#include "common/sidecar/input_queue.hpp"
#include "common/sidecar/popup_classify.h"     // popup sanity predicates (size + junk hook)
#include "common/sidecar/popup_lifecycle.hpp"  // popup configure/remap authority + ordering
#include "common/sidecar/window_placement.hpp" // pure centering + clamp helpers
#include "common/sidecar/window_registry.hpp"  // sidecar::PopupKind (classification token)

#include <xcb/xcb.h>
#ifdef VKRELAY2_HAVE_XCB_COMPOSITE
#include <xcb/composite.h>
#endif
#ifdef VKRELAY2_HAVE_XCB_SHAPE
#include <xcb/shape.h>
#endif
#ifdef VKRELAY2_HAVE_XCB_XTEST
#include <xcb/xtest.h>
#endif
#ifdef VKRELAY2_HAVE_XCB_XFIXES
#include <xcb/xfixes.h>
#endif

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <poll.h>
#include <unistd.h>

namespace {

constexpr char kComponent[] = "sidecar";

struct Options {
    std::string connect;       // worker sidecar-plane host:port
    std::string sidecar_token; // proven to the worker at SidecarHello
    int ready_fd = -1;         // write one byte here after the worker acks sidecar_ready (optional)
    std::string display;       // X display; empty -> $DISPLAY
    // Boundary-smoke seam: when set, the sidecar seeds a canonical neutral input
    // sequence into the worker's ring (DebugEnqueueInput) for each toplevel it registers, then
    // drains + XTest- injects it as usual -- so run_input_smoke.sh can prove worker-ring ->
    // PollInput -> XTest -> canary without real Win32 input (which cannot originate on WSL).
    // Test-only.
    bool debug_inject = false;
    // Reverse-feedback smoke seam (test-only): "X,Y" (move) or "X,Y,W,H"
    // (resize) -> seed ONE Win32-user-origin GeometryRequest for the first toplevel, so
    // run_feedback_smoke.sh proves the sidecar authors the GUEST move/resize from a worker-origin
    // request. Empty = off.
    std::string debug_feedback_move;
    // Iconify smoke seam (test-only): when set, ALSO seed a Win32-user-origin
    // GeometryRequest{Minimize} (after the move/resize above) for the first toplevel, so
    // run_iconify_smoke.sh proves the sidecar authors Iconic + the pending-iconify guard keeps the
    // host Iconic (not Hidden) over the wire. Off by default.
    bool debug_feedback_minimize = false;
    // Restore-leg smoke seam (test-only): with
    // --debug-feedback-minimize, ALSO seed a GeometryRequest{Restore} ONCE the iconify-unmap has
    // been consumed (the marker cleared), so run_iconify_smoke.sh exercises apply_user_restore over
    // the wire. Off by default.
    bool debug_feedback_restore = false;
    // Guest-display-geometry smoke seam (test-only): "X,Y" -> seed ONE left click at those
    // GUEST-ROOT coords for the first toplevel registered (motion + press + release through the
    // SAME worker-ring -> PollInput -> client-to-root translation -> XTest path). Seeding waits for
    // initial host-realization feedback, then derives client coords from the live X geometry.
    // run_bigroot_smoke.sh aims beyond the old default root to prove no root clamp. Empty = off.
    std::string debug_inject_click;
};

bool parse_host_port(const std::string& text, std::string& host, int& port) {
    const std::size_t colon = text.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= text.size()) {
        return false;
    }
    host = text.substr(0, colon);
    port = std::atoi(text.substr(colon + 1).c_str());
    return port > 0;
}

// Send one sidecar RPC and decode the reply. Returns false on a transport error or a non-Ok status.
bool send_rpc(vkr::vkrpc::RpcChannel& rpc, vkr::sidecar::SidecarOp op, const vkr::json::Value& body,
              vkr::json::Value& reply) {
    static std::uint32_t next_id = 1;
    vkr::vkrpc::RpcMessage req;
    req.op = static_cast<std::uint32_t>(op);
    req.request_id = next_id++;
    req.body = body.dump(0);
    rpc.send(req);
    vkr::vkrpc::RpcMessage resp;
    if (!rpc.recv(resp) || resp.status != 0) {
        return false;
    }
    std::string err;
    return vkr::json::Value::try_parse(resp.body, reply, err);
}

// Whether an X extension is present, by name, via core xcb (no per-extension lib needed).
bool has_extension(xcb_connection_t* c, const char* name) {
    xcb_query_extension_reply_t* r = xcb_query_extension_reply(
        c, xcb_query_extension(c, static_cast<std::uint16_t>(std::strlen(name)), name), nullptr);
    const bool present = (r != nullptr && r->present != 0);
    free(r);
    return present;
}

// A monotonic per-session lifecycle generation. Every forwarded register/update/unregister gets the
// next value, so the worker's strictly-newer-wins gate never drops a register below a later
// unregister, and a stale/reordered event cannot remove a freshly re-registered entry.
std::uint64_t g_generation = 0;
std::uint64_t next_generation() {
    return ++g_generation;
}

// Per-tracked-toplevel state. `generation` (the last lifecycle generation forwarded for this
// window) is always maintained so a chrome capture stamps the worker's CURRENT registry generation
// (the worker requires an EXACT match). The remaining fields are the XComposite capture
// resource: the redirect flag, the named backing pixmap, its size (for resize detection), and the
// per-toplevel monotonic chrome paint `seq`. Erased on unregister.
struct Tracked {
    std::uint64_t generation = 0;
    // The worker's CURRENT representation epoch for this xid (from the toplevel-op
    // response). The debug-inject seam seeds input at this epoch so it passes the worker's exact-
    // epoch gate; in production the worker stamps captured events with it directly.
    std::uint64_t epoch = 0;
    std::uint64_t seq = 0;
    bool redirected = false;
    std::uint32_t pixmap = 0;
    std::uint16_t pix_w = 0;
    std::uint16_t pix_h = 0;
    // A classified override-redirect popup + its resolved NON-popup owner toplevel.
    // Used to walk a transient_for chain to the nearest non-popup anchor (a popup-of-a-popup), and
    // to clear owner attribution on teardown. `is_popup` false / `owner` 0 for an ordinary
    // toplevel.
    bool is_popup = false;
    xcb_window_t owner = 0;
};
std::map<xcb_window_t, Tracked> g_tracked;
// Pending-iconify echo guard: a Win32-USER minimize makes the
// sidecar iconify the GUEST (unmap), which fires an UnmapNotify that maps to Hidden -- which
// would strip the host taskbar button. The marker -- keyed by xid, stamped with the live
// representation epoch + the drain tick it was armed at -- lets the matching UnmapNotify CONSUME
// only the Hidden downgrade while still running the rest of the unmap bookkeeping
// (cap_invalidate_pixmap), so the host stays Iconic (taskbar-restorable) and a later restore
// recaptures fresh chrome. Cleared on the matching unmap, Restore, unregister/destroy, and a safety
// sweep (a never-arriving iconify-unmap must not strand the marker and wrongly consume a much-later
// genuine unmap). Sidecar-thread-only (like g_tracked).
struct PendingIconify {
    std::uint64_t epoch = 0;
    std::uint64_t armed_tick = 0;
};
std::map<xcb_window_t, PendingIconify> g_pending_iconify;
// Monotonic drain-loop tick (advanced once per PollInput drain) for the pending-iconify safety
// sweep: a marker older than kPendingIconifySweepTicks drains is swept. The iconify-unmap is
// processed at the START of a loop iteration (process_x_event), BEFORE that iteration's drain, so
// two ticks give a late unmap a fair chance to consume the marker first while still bounding a
// stranded one.
std::uint64_t g_drain_tick = 0;
constexpr std::uint64_t kPendingIconifySweepTicks = 2;
// The guest window the pointer was last directed into (the cursor-change target -- the
// X "current cursor" reflects the window under the pointer, which we are the sole injector for).
// Declared here (before forward_unregister, which clears it on teardown). 0 = no interaction yet.
xcb_window_t g_last_pointer_xid = 0;
// The toplevel the sidecar last gave X input focus (apply_focus). A popup-owner
// resolution fallback for keyboard-opened menus (after WM_TRANSIENT_FOR and the pointer target).
// Cleared when its xid unregisters. 0 = no focus set yet.
xcb_window_t g_focus_xid = 0;
// EWMH popup-classification atoms, interned once at startup (0 until then). The
// _NET_WM_WINDOW_TYPE property + the popup-type values the allow-list accepts. WM_TRANSIENT_FOR is
// a predefined atom (XCB_ATOM_WM_TRANSIENT_FOR), so it is not interned here.
xcb_atom_t g_net_wm_window_type = 0;
xcb_atom_t g_net_wm_window_type_menu = 0;
xcb_atom_t g_net_wm_window_type_popup_menu = 0;
xcb_atom_t g_net_wm_window_type_dropdown_menu = 0;
xcb_atom_t g_net_wm_window_type_tooltip = 0;
xcb_atom_t g_net_wm_window_type_combo = 0;
// True only when chrome capture is BOTH compiled in (libxcb-composite) AND the XComposite extension
// is present on the display; the capture functions no-op otherwise.
bool g_chrome_capture = false;
// True only when xcb-shape is compiled in AND the display advertises SHAPE. Every chrome capture
// then queries the current BOUNDING region and defines out-of-shape XComposite storage as black.
bool g_shape_mask = false;

// Cross-monitor maximize guard: the X root dimensions, cached once at startup (the root is
// immutable for the session). The Win32-user-origin reverse-resize path clamps the requested guest
// SIZE to these before xcb_configure_window, so the guest X window can never be configured LARGER
// than its root even if a resize slips past the worker's WM_GETMINMAXINFO cap (defense-in-depth on
// a programmatic path). SIZE only -- the origin stays unclamped. 0 until
// set (no clamp).
std::uint16_t g_root_w = 0;
std::uint16_t g_root_h = 0;
// Exact immutable snapshot delivered by the authenticated worker handshake. This is the
// X-side placement/recovery authority; the root dimensions remain only the realizability cap.
vkr::display::DisplayLayout g_display_layout;
bool g_has_display_layout = false;

// Pure recapture scheduler for non-GL placeholder toplevels (sidecar-thread-only, like
// g_tracked). Driven by the lifecycle hooks (track/forget/on_resize) + the per-tick recapture pass;
// the actual XComposite capture/ship stays in the COMPOSITE-guarded helpers below.
vkr::sidecar::ChromeRecapturePolicy g_recapture;

// Monotonic milliseconds for the recapture cadence (the poll timeout is an implementation detail;
// the policy schedules off real timestamps).
std::uint64_t now_ms() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

// Send a sidecar RPC whose body is already an encoded WIRE string (the binary PaintChrome op uses
// to_wire, not a JSON dump). Decodes the JSON reply. Returns false on transport error / non-Ok
// status.
bool send_rpc_wire(vkr::vkrpc::RpcChannel& rpc, vkr::sidecar::SidecarOp op,
                   const std::string& wire_body, vkr::json::Value& reply) {
    static std::uint32_t next_id = 0x40000000; // disjoint id space from send_rpc, just for clarity
    vkr::vkrpc::RpcMessage req;
    req.op = static_cast<std::uint32_t>(op);
    req.request_id = next_id++;
    req.body = wire_body;
    // Producer-side framing backstop. RpcChannel::send throws on an oversized payload; a malformed
    // or future binary producer must not take down this long-lived WM/input actor.
    if (wire_body.size() + vkr::vkrpc::kRpcHeaderBytes > vkr::protocol::kMaxFrameBytes) {
        VKR_WARN(kComponent) << "binary sidecar RPC body exceeds the frame cap; op="
                             << static_cast<std::uint32_t>(op) << " bytes=" << wire_body.size();
        return false;
    }
    rpc.send(req);
    vkr::vkrpc::RpcMessage resp;
    if (!rpc.recv(resp) || resp.status != 0) {
        return false;
    }
    std::string err;
    return vkr::json::Value::try_parse(resp.body, reply, err);
}

// Forward a guest toplevel's appearance to the worker registry (the worker joins it to any surface
// by XID). Reads the window's current geometry for the placeholder's best-effort
// static placement. Lifecycle-forward failures are logged, not fatal --
// the WM keeps running. `override_redirect` is carried so the worker can double-check the
// classifier.
void forward_register(vkr::vkrpc::RpcChannel& rpc, xcb_connection_t* xc, xcb_window_t w,
                      bool override_redirect, bool is_popup = false, xcb_window_t owner = 0,
                      std::uint32_t popup_kind = 0) {
    vkr::sidecar::SidecarRegisterToplevelRequest req;
    req.xid = w;
    req.generation = next_generation();
    // Advisory role; the worker treats role + the popup fields as the sidecar's classification.
    req.role = is_popup ? "popup" : "toplevel";
    req.override_redirect = override_redirect;
    req.is_popup = is_popup;
    req.owner_xid = owner;
    req.popup_kind = popup_kind;
    xcb_get_geometry_reply_t* g = xcb_get_geometry_reply(xc, xcb_get_geometry(xc, w), nullptr);
    if (g != nullptr) {
        req.x = g->x;
        req.y = g->y;
        req.width = g->width;
        req.height = g->height;
        free(g);
    }
    vkr::json::Value resp;
    if (!send_rpc(rpc, vkr::sidecar::SidecarOp::RegisterToplevel, req.to_body(), resp)) {
        VKR_WARN(kComponent) << "register_toplevel forward failed for xid " << w;
        return;
    }
    const vkr::sidecar::SidecarToplevelResponse r =
        vkr::sidecar::SidecarToplevelResponse::from_body(resp);
    g_tracked[w].generation = req.generation; // chrome captures stamp this exact generation
    g_tracked[w].epoch = r.epoch;     // input is seeded/stamped at this representation epoch
    g_tracked[w].is_popup = is_popup; // owner-chain walk + teardown attribution
    g_tracked[w].owner = owner;
    // Any PLACEHOLDER chrome window -- INCLUDING classified popups -- is a
    // chrome-recapture candidate (a surface-backed window renders via GL present, not chrome, so it
    // stays out). track() starts the warm-up burst; the steady poll then keeps the content fresh
    // after the first Expose. Popups are tracked too -- a Qt menu paints its items a beat AFTER
    // map with no further Expose, so the one-shot map capture is BLACK and only the recapture poll
    // carries the real content. The teardown paths forget() popups (they are transient) and the
    // popup remap path re-tracks them.
    if (r.representation == "placeholder") {
        g_recapture.track(w, now_ms());
    }
    VKR_INFO(kComponent) << "register_toplevel xid=" << w << " gen=" << req.generation
                         << " epoch=" << r.epoch << " applied=" << (r.applied ? 1 : 0)
                         << " representation=" << r.representation
                         << " popup=" << (is_popup ? 1 : 0) << " owner=" << owner;
}

// Forward a registered toplevel's geometry change. A POSITION/SIZE change
// drives a LIVE host MOVE/RESIZE at the worker; a `z_order` (Raise/Lower) drives a live
// RESTACK (folded into the same apply). A no-op at the worker for an unregistered xid.
void forward_update(vkr::vkrpc::RpcChannel& rpc, xcb_connection_t* xc, xcb_window_t w,
                    vkr::sidecar::ZOrder z_order = vkr::sidecar::ZOrder::None) {
    vkr::sidecar::SidecarUpdateToplevelRequest req;
    req.xid = w;
    req.generation = next_generation();
    req.z_order = static_cast<std::uint32_t>(z_order);
    xcb_get_geometry_reply_t* g = xcb_get_geometry_reply(xc, xcb_get_geometry(xc, w), nullptr);
    if (g != nullptr) {
        req.x = g->x;
        req.y = g->y;
        req.width = g->width;
        req.height = g->height;
        free(g);
    }
    vkr::json::Value resp;
    if (!send_rpc(rpc, vkr::sidecar::SidecarOp::UpdateToplevel, req.to_body(), resp)) {
        VKR_WARN(kComponent) << "update_toplevel forward failed for xid " << w;
        return;
    }
    // A configure bumps the worker's registry generation, so subsequent CHROME captures must stamp
    // the new one (an in-flight capture at the old generation is dropped by the worker's
    // exact-match gate). The INPUT epoch is deliberately UNCHANGED by an update (the representation
    // is the same window), so in-flight input survives the resize -- refresh it from the response
    // regardless.
    const vkr::sidecar::SidecarToplevelResponse r =
        vkr::sidecar::SidecarToplevelResponse::from_body(resp);
    const auto it = g_tracked.find(w);
    if (it != g_tracked.end()) {
        it->second.generation = req.generation;
        it->second.epoch = r.epoch;
    }
    // The worker-visible sync signal for the boundary smoke (parity with forward_register /
    // forward_unregister): the forwarded geometry (position + size + z-order, so the smoke can
    // distinguish a move from a resize from a restack) + whether it applied at the worker.
    VKR_INFO(kComponent) << "update_toplevel xid=" << w << " gen=" << req.generation
                         << " x=" << req.x << " y=" << req.y << " w=" << req.width
                         << " h=" << req.height << " z=" << req.z_order
                         << " applied=" << (r.applied ? 1 : 0);
}

// Forward a guest toplevel's disappearance (unmap/destroy). Generation-tagged so a stale unmap
// cannot erase a freshly re-mapped entry. A no-op at the worker for an unknown xid.
void forward_unregister(vkr::vkrpc::RpcChannel& rpc, xcb_window_t w) {
    vkr::sidecar::SidecarUnregisterToplevelRequest req;
    req.xid = w;
    req.generation = next_generation();
    vkr::json::Value resp;
    if (!send_rpc(rpc, vkr::sidecar::SidecarOp::UnregisterToplevel, req.to_body(), resp)) {
        VKR_WARN(kComponent) << "unregister_toplevel forward failed for xid " << w;
        return;
    }
    const vkr::sidecar::SidecarToplevelResponse r =
        vkr::sidecar::SidecarToplevelResponse::from_body(resp);
    VKR_INFO(kComponent) << "unregister_toplevel xid=" << w << " gen=" << req.generation
                         << " applied=" << (r.applied ? 1 : 0);
    g_tracked.erase(w);         // capture resource was already freed by cap_stop
    g_pending_iconify.erase(w); // a torn-down xid cannot have a pending iconify-unmap
    if (w == g_last_pointer_xid) {
        g_last_pointer_xid = 0; // stop attributing cursor changes to a gone toplevel
    }
    if (w == g_focus_xid) {
        g_focus_xid = 0; // stop attributing popup ownership to a gone focus owner
    }
}

// forward a registered toplevel's live VISIBILITY change (an X unmap -> Hidden; a
// tracked remap -> Visible). Generation-tagged + strictly-newer-wins; the worker ShowWindows the
// host (SW_HIDE / SW_SHOWNA) while PRESERVING the representation (HWND/surface/chrome/epoch) -- so
// a restore is free, UNLIKE forward_unregister (now reserved for DestroyNotify). The representation
// epoch is unchanged across hide/show (refreshed from the response regardless, like
// forward_update). A no-op at the worker for an unknown xid.
void forward_visibility(vkr::vkrpc::RpcChannel& rpc, xcb_window_t w,
                        vkr::sidecar::VisibilityState state) {
    vkr::sidecar::SidecarSetVisibilityRequest req;
    req.xid = w;
    req.generation = next_generation();
    req.visibility_state = static_cast<std::uint32_t>(state);
    vkr::json::Value resp;
    if (!send_rpc(rpc, vkr::sidecar::SidecarOp::SetToplevelVisibility, req.to_body(), resp)) {
        VKR_WARN(kComponent) << "set_visibility forward failed for xid " << w;
        return;
    }
    const vkr::sidecar::SidecarToplevelResponse r =
        vkr::sidecar::SidecarToplevelResponse::from_body(resp);
    const auto it = g_tracked.find(w);
    if (it != g_tracked.end()) {
        it->second.generation = req.generation; // a visibility op bumps the gen (chrome re-stamps)
        it->second.epoch = r.epoch;             // epoch UNCHANGED across hide/show (refresh anyway)
    }
    VKR_INFO(kComponent) << "set_visibility xid=" << w << " gen=" << req.generation
                         << " state=" << req.visibility_state << " applied=" << (r.applied ? 1 : 0);
}

// --- XComposite chrome capture. Conditionally compiled on libxcb-composite;
// when absent (or the extension is missing at runtime) the cap_* functions no-op and the sidecar
// still does lifecycle forwarding + the WM pass-through. ---
#ifdef VKRELAY2_HAVE_XCB_COMPOSITE

// The visualtype for a window's visual id (carries the channel masks), or nullptr if not found.
const xcb_visualtype_t* find_visualtype(xcb_screen_t* screen, xcb_visualid_t vid) {
    for (xcb_depth_iterator_t di = xcb_screen_allowed_depths_iterator(screen); di.rem;
         xcb_depth_next(&di)) {
        for (xcb_visualtype_iterator_t vi = xcb_depth_visuals_iterator(di.data); vi.rem;
             xcb_visualtype_next(&vi)) {
            if (vi.data->visual_id == vid) {
                return vi.data;
            }
        }
    }
    return nullptr;
}

// Bits-per-pixel the server uses for `depth` (from the setup's pixmap formats), or 0 if unknown.
int bpp_for_depth(const xcb_setup_t* setup, std::uint8_t depth) {
    for (xcb_format_iterator_t fi = xcb_setup_pixmap_formats_iterator(setup); fi.rem;
         xcb_format_next(&fi)) {
        if (fi.data->depth == depth) {
            return fi.data->bits_per_pixel;
        }
    }
    return 0;
}

int trailing_zeros(std::uint32_t m) {
    int s = 0;
    while (s < 31 && ((m >> s) & 1u) == 0) {
        ++s;
    }
    return s;
}

// Canonical visual -> top-down BGRA8 conversion. Handles a 32-bpp TrueColor visual by
// its channel masks (the common Xwayland BGRX/BGRA 8-8-8 case is the no-scale fast path of this),
// forcing alpha 255. Returns an EMPTY buffer (fail-closed) for any unsupported visual/depth/bpp or
// a short image -- the caller logs the visual + skips the paint rather than shipping
// plausible-but-wrong color.
std::vector<unsigned char> visual_to_bgra(const unsigned char* data, int data_len, int w, int h,
                                          int bpp, int byte_order, const xcb_visualtype_t* vt) {
    std::vector<unsigned char> out;
    if (vt == nullptr || bpp != 32 || w <= 0 || h <= 0 || data_len < 0) {
        return out;
    }
    const std::uint32_t rm = vt->red_mask, gm = vt->green_mask, bm = vt->blue_mask;
    if (rm == 0 || gm == 0 || bm == 0) {
        return out;
    }
    const std::size_t pixels = static_cast<std::size_t>(w) * h;
    if (static_cast<std::size_t>(data_len) < pixels * 4) {
        return out; // server returned fewer bytes than a tight 32-bpp image
    }
    const int rs = trailing_zeros(rm), gs = trailing_zeros(gm), bs = trailing_zeros(bm);
    const std::uint32_t rmax = rm >> rs, gmax = gm >> gs, bmax = bm >> bs;
    const bool lsb = (byte_order == XCB_IMAGE_ORDER_LSB_FIRST);
    out.resize(pixels * 4);
    for (std::size_t i = 0; i < pixels; ++i) {
        const unsigned char* p = data + i * 4;
        const std::uint32_t px =
            lsb ? (static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
                   (static_cast<std::uint32_t>(p[2]) << 16) |
                   (static_cast<std::uint32_t>(p[3]) << 24))
                : ((static_cast<std::uint32_t>(p[0]) << 24) |
                   (static_cast<std::uint32_t>(p[1]) << 16) |
                   (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]));
        std::uint32_t r = (px & rm) >> rs;
        std::uint32_t g = (px & gm) >> gs;
        std::uint32_t b = (px & bm) >> bs;
        if (rmax != 255) {
            r = r * 255 / rmax;
        }
        if (gmax != 255) {
            g = g * 255 / gmax;
        }
        if (bmax != 255) {
            b = b * 255 / bmax;
        }
        out[i * 4 + 0] = static_cast<unsigned char>(b);
        out[i * 4 + 1] = static_cast<unsigned char>(g);
        out[i * 4 + 2] = static_cast<unsigned char>(r);
        out[i * 4 + 3] = 255;
    }
    return out;
}

#ifdef VKRELAY2_HAVE_XCB_SHAPE
// Query, do not cache, the current BOUNDING region on each already-scheduled chrome capture. Shape
// changes therefore participate in the normalized-frame hash without another event/cache lifetime.
// A query failure skips the capture: forwarding an unmasked rectangular pixmap would reintroduce
// undefined/stale bytes outside the client's drawable shape.
bool chrome_apply_bounding_shape(xcb_connection_t* xc, xcb_window_t w,
                                 std::vector<unsigned char>& bgra, std::uint16_t cw,
                                 std::uint16_t ch) {
    if (!g_shape_mask) {
        return true;
    }
    xcb_generic_error_t* err = nullptr;
    xcb_shape_get_rectangles_reply_t* reply = xcb_shape_get_rectangles_reply(
        xc, xcb_shape_get_rectangles(xc, w, XCB_SHAPE_SK_BOUNDING), &err);
    if (reply == nullptr) {
        if (err != nullptr) {
            VKR_WARN(kComponent) << "shape query failed for xid " << w << " (X error "
                                 << static_cast<int>(err->error_code) << ")";
        }
        free(err);
        return false;
    }
    const std::uint32_t count = reply->rectangles_len;
    const xcb_rectangle_t* xrects = xcb_shape_get_rectangles_rectangles(reply);
    std::vector<vkr::sidecar::ChromeShapeRect> rects;
    const std::size_t pixels = static_cast<std::size_t>(cw) * ch;
    if (count > pixels) {
        free(reply);
        VKR_WARN(kComponent) << "over-complex bounding shape for xid " << w << " (" << count
                             << " rectangles for " << pixels << " pixels); capture skipped";
        return false;
    }
    try {
        if (count > 0) {
            rects.reserve(count);
            for (std::uint32_t i = 0; i < count; ++i) {
                rects.push_back({xrects[i].x, xrects[i].y, xrects[i].width, xrects[i].height});
            }
        }
    } catch (...) {
        free(reply);
        return false;
    }
    free(reply);
    if (!vkr::sidecar::chrome_mask_outside_shape(bgra.data(), bgra.size(), cw, ch, rects)) {
        VKR_WARN(kComponent) << "invalid/over-complex bounding shape for xid " << w
                             << "; chrome capture skipped";
        return false;
    }
    return true;
}
#else
bool chrome_apply_bounding_shape(xcb_connection_t*, xcb_window_t, std::vector<unsigned char>&,
                                 std::uint16_t, std::uint16_t) {
    return true;
}
#endif

// Capture the toplevel's CURRENT chrome from its backing pixmap into a normalized BGRA frame.
// ConfigureNotify invalidates a named pixmap eagerly below; the dimension check here is a defensive
// fallback for a capture racing an as-yet-undrained configure event. Returns false (skip this tick)
// on any X failure (BadWindow / BadPixmap on a just-mapped or destroyed window), a zero
// or over-cap size, or an unsupported visual. This is the CAPTURE half of the old cap_ship (the
// ship half follows), so the recapture pass can hash the normalized BGRA and decide whether to
// ship before sending it.
bool chrome_capture_bgra(xcb_connection_t* xc, const xcb_setup_t* setup, xcb_screen_t* screen,
                         xcb_window_t w, std::vector<unsigned char>& out_bgra, std::uint16_t& out_w,
                         std::uint16_t& out_h) {
    const auto it = g_tracked.find(w);
    if (it == g_tracked.end() || !it->second.redirected) {
        return false; // not armed (the pixmap is named lazily below, on the first capture)
    }
    Tracked& t = it->second;
    xcb_get_geometry_reply_t* g = xcb_get_geometry_reply(xc, xcb_get_geometry(xc, w), nullptr);
    if (g == nullptr) {
        return false; // window gone / not viewable -> skip
    }
    const std::uint16_t cw = g->width, ch = g->height;
    free(g);
    if (cw == 0 || ch == 0) {
        return false;
    }
    // Producer-side cap: enforce the SAME source-store bound the worker decoder
    // does, BEFORE issuing a huge xcb_get_image / allocating a huge BGRA vector / building a huge
    // wire body. A guest can request a very large toplevel; skip + log it (dirty-rect tiling /
    // scaled capture is a follow-up) rather than letting the sidecar do the big work the worker
    // would reject.
    if (static_cast<std::uint64_t>(cw) * ch >
        vkr::sidecar::kMaxAuxChromeBackingStoreBytes / vkr::sidecar::kAuxChromeBytesPerPixel) {
        VKR_WARN(kComponent) << "chrome capture: xid " << w << " source " << cw << "x" << ch
                             << " exceeds the chrome cap; skipped";
        return false;
    }
    if (t.pixmap == 0) {
        // First capture after arming: the window is mapped now, so name its backing pixmap.
        t.pixmap = xcb_generate_id(xc);
        xcb_composite_name_window_pixmap(xc, w, t.pixmap);
        t.pix_w = cw;
        t.pix_h = ch;
    } else if (cw != t.pix_w || ch != t.pix_h) {
        // Resized: free the stale pixmap and re-name (the server reallocated the backing store).
        xcb_free_pixmap(xc, t.pixmap);
        t.pixmap = xcb_generate_id(xc);
        xcb_composite_name_window_pixmap(xc, w, t.pixmap);
        t.pix_w = cw;
        t.pix_h = ch;
    }
    xcb_get_window_attributes_reply_t* a =
        xcb_get_window_attributes_reply(xc, xcb_get_window_attributes(xc, w), nullptr);
    if (a == nullptr) {
        return false;
    }
    const xcb_visualtype_t* vt = find_visualtype(screen, a->visual);
    free(a);
    xcb_get_image_reply_t* img = xcb_get_image_reply(
        xc, xcb_get_image(xc, XCB_IMAGE_FORMAT_Z_PIXMAP, t.pixmap, 0, 0, cw, ch, ~0u), nullptr);
    if (img == nullptr) {
        return false; // BadMatch/BadPixmap (e.g. resize race) -> skip this tick
    }
    const int bpp = bpp_for_depth(setup, img->depth);
    std::vector<unsigned char> bgra =
        visual_to_bgra(xcb_get_image_data(img), xcb_get_image_data_length(img), cw, ch, bpp,
                       setup->image_byte_order, vt);
    const int img_depth = img->depth;
    free(img);
    if (bgra.empty()) {
        VKR_WARN(kComponent) << "chrome capture: unsupported visual/depth for xid " << w
                             << " (depth=" << img_depth << " bpp=" << bpp << "); fail-closed";
        return false;
    }
    if (!chrome_apply_bounding_shape(xc, w, bgra, cw, ch)) {
        return false;
    }
    out_bgra = std::move(bgra);
    out_w = cw;
    out_h = ch;
    return true;
}

// SHIP a captured BGRA frame to the worker (the binary PaintChrome op). Sources larger than one
// RpcChannel frame are split into full-width row bands using PaintChrome's dirty rectangle. Returns
// the last worker response; on a transport failure `representation` stays "none" (-> the recapture
// policy treats it as gone).
vkr::sidecar::SidecarPaintResponse chrome_ship_bgra(vkr::vkrpc::RpcChannel& rpc, xcb_window_t w,
                                                    const std::vector<unsigned char>& bgra,
                                                    std::uint16_t cw, std::uint16_t ch) {
    vkr::sidecar::SidecarPaintResponse r; // representation defaults to "none"
    Tracked& t = g_tracked[w];
    const std::uint32_t stride = static_cast<std::uint32_t>(cw) * 4;
    const std::uint32_t max_rows = vkr::sidecar::max_aux_chrome_rows_per_wire(cw);
    if (max_rows == 0) {
        VKR_WARN(kComponent) << "paint_chrome: one source row exceeds the wire cap for xid " << w
                             << " width=" << cw;
        return r;
    }
    for (std::uint32_t y = 0; y < ch;) {
        const std::uint32_t rows = std::min<std::uint32_t>(max_rows, ch - y);
        vkr::sidecar::SidecarPaintChromeRequest req;
        req.xid = w;
        req.lifecycle_generation = t.generation;
        req.seq = ++t.seq;
        req.src_w = cw;
        req.src_h = ch;
        req.dirty_x = 0;
        req.dirty_y = static_cast<std::int32_t>(y);
        req.dirty_w = cw;
        req.dirty_h = rows;
        req.stride = stride;
        req.format = vkr::sidecar::kAuxChromeFormatBgra8;
        const std::size_t offset = static_cast<std::size_t>(y) * stride;
        const std::size_t bytes = static_cast<std::size_t>(rows) * stride;
        req.pixels.assign(reinterpret_cast<const char*>(bgra.data() + offset), bytes);
        vkr::json::Value resp;
        if (!send_rpc_wire(rpc, vkr::sidecar::SidecarOp::PaintChrome, req.to_wire(), resp)) {
            VKR_WARN(kComponent) << "paint_chrome forward failed for xid " << w << " band=" << y
                                 << "+" << rows;
            return vkr::sidecar::SidecarPaintResponse{};
        }
        r = vkr::sidecar::SidecarPaintResponse::from_body(resp);
        VKR_INFO(kComponent) << "paint_chrome xid=" << w << " gen=" << req.lifecycle_generation
                             << " seq=" << req.seq << " " << cw << "x" << ch << " band=" << y << "+"
                             << rows << " applied=" << (r.applied ? 1 : 0)
                             << " shown=" << (r.shown ? 1 : 0);
        if (!r.applied || r.representation != "placeholder") {
            break; // stale/promoted/gone: later bands cannot become valid again
        }
        y += rows;
    }
    return r;
}

// Capture + UNCONDITIONALLY ship (the Expose / map / popup / restore trigger path -- unchanged
// behavior). Also syncs the recapture policy (record the capture + ship + the response's
// representation) so the per-tick recapture pass will not re-ship the identical frame and learns a
// surface promotion. No-op for an untracked xid in the policy (a window never tracked / already
// forgotten). C: placeholder popups ARE tracked now, so this also syncs their recapture state.
void cap_ship(vkr::vkrpc::RpcChannel& rpc, xcb_connection_t* xc, const xcb_setup_t* setup,
              xcb_screen_t* screen, xcb_window_t w) {
    if (!g_chrome_capture) {
        return;
    }
    std::vector<unsigned char> bgra;
    std::uint16_t cw = 0, ch = 0;
    if (!chrome_capture_bgra(xc, setup, screen, w, bgra, cw, ch)) {
        return;
    }
    const std::uint64_t now = now_ms();
    // C (black-flash fix): SUPPRESS the premature all-black map frame while the window is still
    // awaiting its first real content. The worker reveals a placeholder only on a committed first
    // paint, so not shipping the black frame keeps the popup/dialog HIDDEN until the toolkit paints
    // -- eliminating the split-second black flash. record_capture keeps the cadence advancing so
    // the warm-up recapture picks up the real (non-black) content within one interval. Bounded to
    // warm-up (holding_for_first_content), so a genuinely-black window still appears within 3 s.
    if (vkr::sidecar::chrome_frame_is_black(bgra.data(), bgra.size()) &&
        g_recapture.holding_for_first_content(w, now)) {
        g_recapture.record_capture(w, now);
        return;
    }
    const std::uint64_t hash = vkr::sidecar::chrome_content_hash(
        bgra.data(), bgra.size(), cw, ch, static_cast<std::uint32_t>(cw) * 4,
        vkr::sidecar::kAuxChromeFormatBgra8);
    const vkr::sidecar::SidecarPaintResponse r = chrome_ship_bgra(rpc, w, bgra, cw, ch);
    g_recapture.record_capture(w, now);
    // Record the shipped hash only when the worker COMMITTED it: a placeholder
    // paint that returns applied=false (stale generation / transient failure) must NOT update
    // last_shipped -- the next tick re-ships those uncommitted pixels. note_representation always
    // runs (surface/none stops recapture).
    if (r.applied) {
        g_recapture.record_ship(w, now, hash);
    }
    g_recapture.note_representation(w, r.representation);
}

// recapture pass: once per poll tick, for each recapture-tracked placeholder toplevel the
// policy says is DUE, capture its chrome, hash the normalized BGRA, and ship ONLY when the policy
// says to (warm-up always; steady on hash change / heartbeat). Feeds the response representation
// back so a surface promotion (or a gone window) stops recapture. Bounded to tracked PLACEHOLDER
// windows -- toplevels AND classified popups; surface-backed windows are never
// policy-tracked, so due() is false for them.
void chrome_recapture_tick(vkr::vkrpc::RpcChannel& rpc, xcb_connection_t* xc,
                           const xcb_setup_t* setup, xcb_screen_t* screen, std::uint64_t now) {
    if (!g_chrome_capture) {
        return;
    }
    for (const auto& kv : g_tracked) {
        const xcb_window_t w = kv.first;
        if (!g_recapture.due(w, now)) {
            continue;
        }
        g_recapture.record_capture(
            w, now); // throttle to the cadence even if the capture/ship is skipped
        std::vector<unsigned char> bgra;
        std::uint16_t cw = 0, ch = 0;
        if (!chrome_capture_bgra(xc, setup, screen, w, bgra, cw, ch)) {
            continue;
        }
        // C (black-flash fix): as in cap_ship, hold the premature all-black frame during the
        // first-content grace so the window is never revealed black (record_capture above already
        // advanced the cadence for this tick).
        if (vkr::sidecar::chrome_frame_is_black(bgra.data(), bgra.size()) &&
            g_recapture.holding_for_first_content(w, now)) {
            continue;
        }
        const std::uint64_t hash = vkr::sidecar::chrome_content_hash(
            bgra.data(), bgra.size(), cw, ch, static_cast<std::uint32_t>(cw) * 4,
            vkr::sidecar::kAuxChromeFormatBgra8);
        if (g_recapture.decide_ship(w, now, hash) != vkr::sidecar::ChromeShipDecision::Ship) {
            continue;
        }
        const vkr::sidecar::SidecarPaintResponse r = chrome_ship_bgra(rpc, w, bgra, cw, ch);
        if (r.applied) { // record only a COMMITTED frame; see cap_ship
            g_recapture.record_ship(w, now, hash);
        }
        g_recapture.note_representation(w, r.representation);
    }
}

// ARM a toplevel for chrome capture: select EXPOSURE (the redraw trigger) + XComposite-redirect
// (AUTOMATIC) + defensive subwindow redirect. Crucially this is called BEFORE xcb_map_window for a
// MapRequest window, so the Expose mask + the redirect are installed before the
// map can generate the first Expose -- otherwise a one-shot client that paints only on that first
// Expose would never be captured. The backing pixmap is named LAZILY on the first cap_ship (a
// window has no backing pixmap until it is mapped/viewable).
void cap_arm(xcb_connection_t* xc, xcb_window_t w) {
    if (!g_chrome_capture) {
        return;
    }
    const std::uint32_t emask = XCB_EVENT_MASK_EXPOSURE;
    xcb_change_window_attributes(xc, w, XCB_CW_EVENT_MASK, &emask);
    xcb_composite_redirect_window(xc, w, XCB_COMPOSITE_REDIRECT_AUTOMATIC);
    xcb_composite_redirect_subwindows(xc, w, XCB_COMPOSITE_REDIRECT_AUTOMATIC); // defensive
    g_tracked[w].redirected = true;
    xcb_flush(xc);
}

// Stop tracking: free the named pixmap and unredirect (mirroring cap_arm), best-effort (a
// destroyed window's requests error server-side and are ignored). The g_tracked entry is erased by
// forward_unregister, which runs right after this.
void cap_stop(xcb_connection_t* xc, xcb_window_t w) {
    const auto it = g_tracked.find(w);
    if (it == g_tracked.end()) {
        return;
    }
    if (it->second.pixmap != 0) {
        xcb_free_pixmap(xc, it->second.pixmap);
        it->second.pixmap = 0;
    }
    if (it->second.redirected) {
        xcb_composite_unredirect_subwindows(xc, w, XCB_COMPOSITE_REDIRECT_AUTOMATIC);
        xcb_composite_unredirect_window(xc, w, XCB_COMPOSITE_REDIRECT_AUTOMATIC);
        it->second.redirected = false;
    }
    xcb_flush(xc);
}

// a redirected window's named backing pixmap is INVALID after
// an UnmapNotify (the server frees the backing store on unmap). Free + zero OUR name while KEEPING
// the redirect + Expose mask armed (NOT a cap_stop -- we still own the window), so the first
// cap_ship after a remap takes the t.pixmap == 0 branch and re-names a FRESH pixmap + recaptures,
// rather than xcb_get_image-ing a stale pixmap forever. Best-effort.
void cap_invalidate_pixmap(xcb_connection_t* xc, xcb_window_t w) {
    const auto it = g_tracked.find(w);
    if (it == g_tracked.end() || it->second.pixmap == 0) {
        return;
    }
    xcb_free_pixmap(xc, it->second.pixmap);
    it->second.pixmap = 0;
    it->second.pix_w = 0;
    it->second.pix_h = 0;
    xcb_flush(xc);
}

// A redirected window gets a new server backing pixmap on every realized resize. Invalidate on
// the EVENT EDGE, rather than waiting for the settled capture's dimensions: a coalesced A -> B -> A
// storm ends at the original dimensions but the original named pixmap is still obsolete.
void cap_invalidate_pixmap_on_configure(xcb_connection_t* xc, xcb_window_t w,
                                        std::uint16_t configured_w, std::uint16_t configured_h) {
    const auto it = g_tracked.find(w);
    if (it == g_tracked.end()) {
        return;
    }
    const Tracked& t = it->second;
    if (vkr::sidecar::chrome_named_pixmap_invalidated_by_configure(t.pixmap != 0, t.pix_w, t.pix_h,
                                                                   configured_w, configured_h)) {
        cap_invalidate_pixmap(xc, w);
    }
}

#else // no libxcb-composite: chrome capture is compiled out (lifecycle forwarding still works).

void cap_ship(vkr::vkrpc::RpcChannel&, xcb_connection_t*, const xcb_setup_t*, xcb_screen_t*,
              xcb_window_t) {}
void cap_arm(xcb_connection_t*, xcb_window_t) {}
void cap_stop(xcb_connection_t*, xcb_window_t) {}
void cap_invalidate_pixmap(xcb_connection_t*, xcb_window_t) {}
void cap_invalidate_pixmap_on_configure(xcb_connection_t*, xcb_window_t, std::uint16_t,
                                        std::uint16_t) {}
void chrome_recapture_tick(vkr::vkrpc::RpcChannel&, xcb_connection_t*, const xcb_setup_t*,
                           xcb_screen_t*, std::uint64_t) {}

#endif // VKRELAY2_HAVE_XCB_COMPOSITE

// --- Input injection (XTest) + focus + close. The worker QUEUES neutral input on
// its window; the sidecar drains it via PollInput on a short timer and is the SOLE X-side injector
// (xcb_test_fake_input) / focus actor (xcb_set_input_focus) / close requester (WM_DELETE_WINDOW) --
// the ICD never touches input. POINTER/KEY/WHEEL injection needs XTEST (compiled in via
// libxcb-xtest AND present at runtime, g_xtest); FOCUS + CLOSE are CORE xcb and work even without
// XTEST. ---

// True only when injection is BOTH compiled in (libxcb-xtest) AND XTEST is present at runtime.
bool g_xtest = false;
bool g_input_trace = false;
// cursor: true only when XFixes is BOTH compiled in (libxcb-xfixes) AND present at
// runtime; the XFixes CursorNotify event base. (g_last_pointer_xid -- the cursor-change target --
// is declared earlier, near g_tracked, since forward_unregister clears it.)
bool g_xfixes = false;
std::uint8_t g_xfixes_event_base = 0;
// Diagnostic off-switch (env VKRELAY2_SIDECAR_NO_XTEST_MOTION=1): neutralize EVERY
// absolute XTEST pointer warp at the single warp_pointer chokepoint (the Motion path AND the
// pre-warps before button/wheel/key), while STILL injecting the button/wheel/key events. A
// bisection run then still exercises the app's grab (the middle-button press/release) but never
// warps -- isolating whether the absolute warp is what trips the guest Xwayland on a view-rotate.
// The paired NO_CURSOR_CAPTURE switch (applied at XFixes setup) isolates the cursor path instead.
bool g_no_xtest_motion = false;
// The sidecar's monotonic PollInput cursor (the worker advances seq; we never re-inject).
std::uint64_t g_input_cursor = 0;
// Boundary-smoke debug seeding (see Options::debug_inject); the per-xid seeded set is one-shot.
bool g_debug_inject = false;
std::set<xcb_window_t> g_seeded;
// reverse-feedback smoke seam (Options::debug_feedback_move): when set, seed ONE
// Win32-user-origin GeometryRequest (move to g_debug_feedback_x/y) into the worker ring for the
// first toplevel registered, so run_feedback_smoke.sh proves worker-ring -> PollInput -> the
// sidecar authors the GUEST move (real Win32 input cannot originate on WSL). One-shot. Test-only.
bool g_debug_feedback = false;
int g_debug_feedback_x = 0;
int g_debug_feedback_y = 0;
int g_debug_feedback_w = 0; // optional requested resize extent (0 => the request is a move)
int g_debug_feedback_h = 0;
bool g_debug_feedback_minimize = false; // also seed a Minimize after the move/resize
bool g_debug_feedback_restore = false;  // also seed a Restore once the iconify is consumed
// GD smoke seam (Options::debug_inject_click): schedule ONE left click at a GUEST-ROOT point for
// the first toplevel. After initial host-realization feedback settles, the sidecar derives client
// coordinates from the live X window and sends them through the ordinary worker-ring path.
bool g_debug_click = false;
int g_debug_click_x = 0;
int g_debug_click_y = 0;
bool g_click_seeded = false;
xcb_window_t g_click_xid = 0;
std::uint64_t g_click_due_ms = 0;
bool g_feedback_seeded = false;
xcb_window_t g_feedback_xid = 0;   // the xid the move/minimize was seeded for (restore leg)
bool g_feedback_iconified = false; // the seeded xid's iconify was authored (restore gate)
bool g_restore_seeded = false;     // the one-shot restore has been seeded
// ICCCM close atoms (interned once at startup; 0 until then -> close is a logged no-op).
xcb_atom_t g_wm_protocols = 0;
xcb_atom_t g_wm_delete_window = 0;
// the ICCCM WM_STATE atom (interned at startup; 0 -> set_wm_state is a no-op) + the
// state values it carries. Set on the guest when the sidecar authors Iconic/Visible -- ICCCM
// hygiene so a real toolkit reflects the minimized state -- but NEVER the primary decision (the
// pending-iconify marker is).
xcb_atom_t g_wm_state = 0;
constexpr std::uint32_t kWmStateNormal = 1; // ICCCM NormalState
constexpr std::uint32_t kWmStateIconic = 3; // ICCCM IconicState

// Sidecar = the SOLE X focus authority. On focus gain, set the guest's input focus +
// log; on focus loss, record only (do NOT force focus elsewhere -- the next focused worker window
// claims its own X toplevel).
void apply_focus(xcb_connection_t* xc, xcb_window_t w, bool gained) {
    if (gained) {
        xcb_set_input_focus(xc, XCB_INPUT_FOCUS_PARENT, w, XCB_CURRENT_TIME);
        g_focus_xid = w; // the current focus owner (a popup-owner resolution fallback)
        VKR_INFO(kComponent) << "focus -> xid=" << w << " (sidecar sole X focus authority)";
    } else {
        VKR_INFO(kComponent) << "focus lost xid=" << w << " (no forced X focus change)";
    }
}

// Ask the guest to close via the ICCCM WM_DELETE_WINDOW ClientMessage IFF it advertises it in
// WM_PROTOCOLS; otherwise log + no-op (NO xcb_kill_client fallback). The worker
// never self-destroys -- the guest closing its X window drives unregister.
void request_close(xcb_connection_t* xc, xcb_window_t w) {
    if (g_wm_protocols == 0 || g_wm_delete_window == 0) {
        VKR_WARN(kComponent) << "close requested xid=" << w << " but WM atoms unavailable; no-op";
        return;
    }
    xcb_get_property_reply_t* p = xcb_get_property_reply(
        xc, xcb_get_property(xc, 0, w, g_wm_protocols, XCB_ATOM_ATOM, 0, 64), nullptr);
    bool advertises = false;
    if (p != nullptr) {
        const xcb_atom_t* atoms = static_cast<const xcb_atom_t*>(xcb_get_property_value(p));
        const int n = static_cast<int>(xcb_get_property_value_length(p) / sizeof(xcb_atom_t));
        for (int i = 0; i < n; ++i) {
            if (atoms[i] == g_wm_delete_window) {
                advertises = true;
            }
        }
        free(p);
    }
    if (!advertises) {
        VKR_INFO(kComponent) << "close requested xid=" << w
                             << " but no advertised WM_DELETE_WINDOW; no-op";
        return;
    }
    xcb_client_message_event_t cm{};
    cm.response_type = XCB_CLIENT_MESSAGE;
    cm.format = 32;
    cm.window = w;
    cm.type = g_wm_protocols;
    cm.data.data32[0] = g_wm_delete_window;
    cm.data.data32[1] = XCB_CURRENT_TIME;
    xcb_send_event(xc, 0, w, XCB_EVENT_MASK_NO_EVENT, reinterpret_cast<const char*>(&cm));
    VKR_INFO(kComponent) << "close -> WM_DELETE_WINDOW xid=" << w;
}

#ifdef VKRELAY2_HAVE_XCB_XTEST

// vk (Win32 virtual-key) -> X keysym, for the bounded subset (letters lowercased, digits,
// Enter/Escape/Backspace/Tab/Space, arrows, and the Shift/Ctrl/Alt MODIFIER keys). The worker sends
// neutral vk; the sidecar owns the X keymap. Mapping the modifier keys here means they inject as
// their OWN real key press/release events (the worker captures WM_KEYDOWN/UP for VK_SHIFT etc.), so
// the X modifier state tracks naturally -- no synthetic before/after bracketing to leak a stuck
// modifier. IME / dead keys / AltGr / full layout fidelity are explicit
// follow-up. 0 = unmapped.
std::uint32_t vk_to_keysym(int vk) {
    if (vk >= 'A' && vk <= 'Z') {
        return 0x0061u + static_cast<std::uint32_t>(vk - 'A'); // XK_a..XK_z (unshifted)
    }
    if (vk >= '0' && vk <= '9') {
        return static_cast<std::uint32_t>(vk); // XK_0..XK_9 == 0x30..0x39
    }
    switch (vk) {
    case 0x10:
        return 0xFFE1u; // VK_SHIFT   -> Shift_L
    case 0x11:
        return 0xFFE3u; // VK_CONTROL -> Control_L
    case 0x12:
        return 0xFFE9u; // VK_MENU    -> Alt_L
    case 0x0D:
        return 0xFF0Du; // Return
    case 0x1B:
        return 0xFF1Bu; // Escape
    case 0x08:
        return 0xFF08u; // BackSpace
    case 0x09:
        return 0xFF09u; // Tab
    case 0x20:
        return 0x0020u; // space
    case 0x25:
        return 0xFF51u; // Left
    case 0x26:
        return 0xFF52u; // Up
    case 0x27:
        return 0xFF53u; // Right
    case 0x28:
        return 0xFF54u; // Down
    default:
        return 0u;
    }
}

// Cached keyboard mapping (core xcb_get_keyboard_mapping): so the sidecar maps a keysym to the X
// SERVER's keycode for the LIVE keymap (never a hardcoded keycode). Loaded once when injection is
// enabled.
struct Keymap {
    std::uint8_t min_keycode = 0;
    std::uint8_t per = 0;
    std::vector<xcb_keysym_t> syms;
    bool loaded = false;
};
Keymap g_keymap;

void load_keymap(xcb_connection_t* xc, const xcb_setup_t* setup) {
    const int count = static_cast<int>(setup->max_keycode) - setup->min_keycode + 1;
    if (count <= 0) {
        return;
    }
    xcb_get_keyboard_mapping_reply_t* r = xcb_get_keyboard_mapping_reply(
        xc, xcb_get_keyboard_mapping(xc, setup->min_keycode, static_cast<std::uint8_t>(count)),
        nullptr);
    if (r == nullptr) {
        return;
    }
    g_keymap.min_keycode = setup->min_keycode;
    g_keymap.per = r->keysyms_per_keycode;
    const xcb_keysym_t* syms = xcb_get_keyboard_mapping_keysyms(r);
    const int n = xcb_get_keyboard_mapping_keysyms_length(r);
    if (syms != nullptr && n > 0) {
        g_keymap.syms.assign(syms, syms + n);
        g_keymap.loaded = g_keymap.per > 0;
    }
    free(r);
}

// Find a keycode whose keymap carries `keysym` (preferring the unshifted column 0). 0 if none.
xcb_keycode_t keysym_to_keycode(std::uint32_t keysym) {
    if (!g_keymap.loaded || g_keymap.per == 0 || keysym == 0) {
        return 0;
    }
    const int per = g_keymap.per;
    const int rows = static_cast<int>(g_keymap.syms.size()) / per;
    for (int col = 0; col < per; ++col) {
        for (int row = 0; row < rows; ++row) {
            if (g_keymap.syms[static_cast<std::size_t>(row) * per + col] == keysym) {
                return static_cast<xcb_keycode_t>(g_keymap.min_keycode + row);
            }
        }
    }
    return 0;
}

// Warp the X pointer to the root coords of (client_x, client_y) within `w` -- the
// warp-before-action that keeps a coalesced/dropped motion from landing a later click at a stale
// position. Best-effort: a gone window simply skips (the click then no-ops
// harmlessly).
void warp_pointer(xcb_connection_t* xc, xcb_window_t root, xcb_window_t w, int client_x,
                  int client_y) {
    if (g_no_xtest_motion) {
        return; // Suppress every absolute warp; button/key events still inject.
    }
    xcb_translate_coordinates_reply_t* t = xcb_translate_coordinates_reply(
        xc,
        xcb_translate_coordinates(xc, w, root, static_cast<std::int16_t>(client_x),
                                  static_cast<std::int16_t>(client_y)),
        nullptr);
    if (t == nullptr) {
        return;
    }
    const std::int16_t rx = t->dst_x;
    const std::int16_t ry = t->dst_y;
    free(t);
    xcb_test_fake_input(xc, XCB_MOTION_NOTIFY, 0 /*absolute*/, XCB_CURRENT_TIME, root, rx, ry, 0);
}

// Inject one pointer/wheel/key event via XTest (caller already checked g_xtest + kind). Focus/close
// are handled by inject_event (core xcb) regardless of XTEST.
void inject_xtest(xcb_connection_t* xc, xcb_window_t root, xcb_window_t w,
                  const vkr::sidecar::SidecarInputEvent& ev) {
    using vkr::sidecar::InputEventKind;
    switch (static_cast<InputEventKind>(ev.kind)) {
    case InputEventKind::Motion:
        warp_pointer(xc, root, w, ev.client_x, ev.client_y);
        break;
    case InputEventKind::Button: {
        warp_pointer(xc, root, w, ev.client_x, ev.client_y);
        // Neutral button -> X button number (X: 1=left 2=middle 3=right).
        const std::uint8_t xbtn = ev.button == vkr::sidecar::kInputButtonLeft     ? 1
                                  : ev.button == vkr::sidecar::kInputButtonMiddle ? 2
                                  : ev.button == vkr::sidecar::kInputButtonRight  ? 3
                                                                                  : 0;
        if (xbtn != 0) {
            xcb_test_fake_input(xc, ev.pressed ? XCB_BUTTON_PRESS : XCB_BUTTON_RELEASE, xbtn,
                                XCB_CURRENT_TIME, root, 0, 0, 0);
        }
        break;
    }
    case InputEventKind::Wheel: {
        warp_pointer(xc, root, w, ev.client_x, ev.client_y);
        const std::uint8_t xbtn = ev.wheel > 0 ? 4 : 5; // Button4 up / Button5 down (one notch)
        xcb_test_fake_input(xc, XCB_BUTTON_PRESS, xbtn, XCB_CURRENT_TIME, root, 0, 0, 0);
        xcb_test_fake_input(xc, XCB_BUTTON_RELEASE, xbtn, XCB_CURRENT_TIME, root, 0, 0, 0);
        break;
    }
    case InputEventKind::Key: {
        // Idempotently re-claim focus immediately before a key (the original found this removed a
        // whole class of ambiguous keyboard failures). Modifier keys (Shift/Ctrl/
        // Alt) arrive as their OWN Key events (vk_to_keysym maps them), so the X modifier state
        // tracks via real press/release -- no synthetic bracketing.
        xcb_set_input_focus(xc, XCB_INPUT_FOCUS_PARENT, w, XCB_CURRENT_TIME);
        const xcb_keycode_t kc = keysym_to_keycode(vk_to_keysym(ev.vk));
        if (kc != 0) { // outside the bounded subset / unmapped on this keymap -> skip
            xcb_test_fake_input(xc, ev.pressed ? XCB_KEY_PRESS : XCB_KEY_RELEASE, kc,
                                XCB_CURRENT_TIME, root, 0, 0, 0);
        }
        break;
    }
    default:
        break;
    }
}

#else // no libxcb-xtest: pointer/key/wheel injection compiled out (focus/close still work).

void load_keymap(xcb_connection_t*, const xcb_setup_t*) {}
void inject_xtest(xcb_connection_t*, xcb_window_t, xcb_window_t,
                  const vkr::sidecar::SidecarInputEvent&) {}

#endif // VKRELAY2_HAVE_XCB_XTEST

// Inject one neutral event into the guest. Pointer/key/wheel need XTEST; focus/close are core xcb
// and are NOT gated on it.
void inject_event(xcb_connection_t* xc, xcb_window_t root,
                  const vkr::sidecar::SidecarInputEvent& ev) {
    const xcb_window_t w = static_cast<xcb_window_t>(ev.xid);
    using vkr::sidecar::InputEventKind;
    const InputEventKind kind = static_cast<InputEventKind>(ev.kind);
    if (g_input_trace) {
        VKR_INFO(kComponent) << "input-trace station=xtest-emit xid=" << ev.xid
                             << " epoch=" << ev.epoch << " kind=" << ev.kind
                             << " client=" << ev.client_x << "," << ev.client_y
                             << " button=" << ev.button << " pressed=" << ev.pressed
                             << " xtest=" << (g_xtest ? 1 : 0);
    }
    // a pointer event warps the X pointer into this window, so the X "current cursor"
    // now reflects it -- remember it as the target for the next XFixes CursorNotify.
    if (kind == InputEventKind::Motion || kind == InputEventKind::Button ||
        kind == InputEventKind::Wheel) {
        g_last_pointer_xid = w;
    }
    switch (kind) {
    case InputEventKind::Motion:
    case InputEventKind::Button:
    case InputEventKind::Wheel:
    case InputEventKind::Key:
        if (g_xtest) {
            inject_xtest(xc, root, w, ev);
        }
        break;
    case InputEventKind::Focus:
        apply_focus(xc, w, ev.pressed);
        break;
    case InputEventKind::Close:
        request_close(xc, w);
        break;
    default:
        break;
    }
}

// decide whether a Win32-user GeometryRequest carries a real RESIZE (vs a pure
// move). The WndProc always reports the committed CLIENT rect in req_w/req_h, so "nonzero" is NOT
// "resize requested": author a guest resize ONLY when the requested extent is
// positive AND differs from the guest's CURRENT extent. Fail CLOSED (treat as move-only) if the
// extent is non-positive or the guest geometry cannot be read -- never author a resize from
// incomplete state.
bool user_request_is_resize(xcb_connection_t* xc, xcb_window_t w, std::uint32_t req_w,
                            std::uint32_t req_h) {
    if (req_w == 0 || req_h == 0) {
        return false;
    }
    xcb_get_geometry_reply_t* g = xcb_get_geometry_reply(xc, xcb_get_geometry(xc, w), nullptr);
    if (g == nullptr) {
        return false; // fail closed: never author a resize from incomplete state
    }
    const bool differs = (static_cast<std::uint32_t>(g->width) != req_w ||
                          static_cast<std::uint32_t>(g->height) != req_h);
    free(g);
    return differs;
}

// set ICCCM WM_STATE on the guest (NormalState / IconicState) -- ICCCM hygiene + a
// diagnostic so a real toolkit (Qt) reflects the minimized state; NEVER the primary decision (the
// pending-iconify marker is). A no-op if the atom is unavailable. icon_window is None.
void set_wm_state(xcb_connection_t* xc, xcb_window_t w, std::uint32_t state) {
    if (g_wm_state == 0) {
        return;
    }
    const std::uint32_t data[2] = {state, XCB_NONE};
    xcb_change_property(xc, XCB_PROP_MODE_REPLACE, w, g_wm_state, g_wm_state, 32, 2, data);
}

// a Win32-USER minimize -> author Iconic + iconify the GUEST (the reverse
// direction). forward_visibility(Iconic) -> the worker SW_SHOWMINNOACTIVEs (idempotent; the host
// already minimized via DefWindowProcW); set WM_STATE=Iconic + unmap the guest. ARM the
// pending-iconify marker BEFORE the unmap so the guest's self-induced UnmapNotify is recognized as
// iconify-induced and suppresses ONLY the Hidden downgrade. A no-op for an
// untracked xid.
void apply_user_iconify(vkr::vkrpc::RpcChannel& rpc, xcb_connection_t* xc, xcb_window_t w) {
    const auto it = g_tracked.find(w);
    if (it == g_tracked.end()) {
        return;
    }
    g_pending_iconify[w] = PendingIconify{it->second.epoch, g_drain_tick};
    forward_visibility(rpc, w, vkr::sidecar::VisibilityState::Iconic);
    set_wm_state(xc, w, kWmStateIconic);
    xcb_unmap_window(xc, w);
    xcb_flush(xc);
    if (w == g_feedback_xid) {
        g_feedback_iconified = true; // Restore-leg smoke gate (test-only); otherwise inert.
    }
    VKR_INFO(kComponent) << "user_iconify xid=" << w
                         << " (Iconic; pending-iconify armed epoch=" << it->second.epoch << ")";
}

// a Win32-USER restore -> de-iconify the GUEST. Clear any pending-iconify marker,
// re-arm chrome capture BEFORE mapping (the MapRequest restore path's order, so the post-map Expose
// re-names a FRESH pixmap and recaptures -- never visually stale), set WM_STATE=Normal, map the
// guest, forward Visible. A no-op for an untracked xid.
void apply_user_restore(vkr::vkrpc::RpcChannel& rpc, xcb_connection_t* xc, xcb_window_t w) {
    const auto it = g_tracked.find(w);
    if (it == g_tracked.end()) {
        return;
    }
    g_pending_iconify.erase(w);
    cap_arm(xc, w);
    set_wm_state(xc, w, kWmStateNormal);
    xcb_map_window(xc, w);
    forward_visibility(rpc, w, vkr::sidecar::VisibilityState::Visible);
    xcb_flush(xc);
    VKR_INFO(kComponent) << "user_restore xid=" << w << " (Visible; pending-iconify cleared)";
}

// apply a Win32-USER-origin geometry REQUEST (the reverse direction). The worker
// forwarded what the user did to the host window; the sidecar is the geometry authority, so it
// authors the guest change here, then forwards the realized geometry back to the worker registry
// (the echo) so the worker's authoritative baseline converges. A move configures the guest to the
// requested X-root CLIENT origin; adds the SIZE half when the user actually resized
// (`user_request_is_resize`), so the echo carries the new w/h -> the `extent_authoritative`
// path pins `currentExtent` to the user's committed size. Minimize/Restore author Iconic/Visible
// (apply_user_iconify / apply_user_restore). A generation-less request -> forward_update mints the
// generation for the echo.
void apply_user_geometry(vkr::vkrpc::RpcChannel& rpc, xcb_connection_t* xc,
                         const vkr::sidecar::SidecarInputEvent& ev) {
    const xcb_window_t w = static_cast<xcb_window_t>(ev.xid);
    const auto hr = static_cast<vkr::sidecar::HostRequestKind>(ev.host_request);
    if (hr == vkr::sidecar::HostRequestKind::Minimize) {
        apply_user_iconify(rpc, xc, w);
        return;
    }
    if (hr == vkr::sidecar::HostRequestKind::Restore) {
        apply_user_restore(rpc, xc, w);
        return;
    }
    if (hr != vkr::sidecar::HostRequestKind::Geometry) {
        return;
    }
    const bool resize = user_request_is_resize(xc, w, ev.req_w, ev.req_h);
    // defense-in-depth: clamp the requested guest SIZE to the X root so the guest window can
    // never be configured LARGER than its output even if a resize bypasses the worker's
    // WM_GETMINMAXINFO cap. SIZE only: a user may drag partly off-screen, so origin is not
    // constrained here. In the normal path the host client is already <= root, so this is a no-op.
    std::uint32_t req_w = ev.req_w;
    std::uint32_t req_h = ev.req_h;
    if (resize) {
        if (g_root_w != 0 && req_w > g_root_w) {
            req_w = g_root_w;
        }
        if (g_root_h != 0 && req_h > g_root_h) {
            req_h = g_root_h;
        }
    }
    std::uint32_t vals[4];
    int n = 0;
    std::uint16_t mask = XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y;
    vals[n++] = static_cast<std::uint32_t>(ev.root_x);
    vals[n++] = static_cast<std::uint32_t>(ev.root_y);
    if (resize) {
        // A real user border resize: configure W/H too. The echo `forward_update` then sees a w/h
        // change -> the registry sets `extent_authoritative` -> caps-pin convergence.
        mask |= XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT;
        vals[n++] = req_w;
        vals[n++] = req_h;
    }
    xcb_configure_window(xc, w, mask, vals);
    xcb_flush(xc);
    // Echo: forward the authored geometry back to the worker (its apply is a guarded no-op since
    // the host is already there; this converges the worker's authoritative-geometry baseline, and a
    // real resize pins the extent through that same path.
    forward_update(rpc, xc, w);
    VKR_INFO(kComponent) << "user_geometry xid=" << w << " -> root=" << ev.root_x << ","
                         << ev.root_y << " " << (resize ? "resize" : "move-only")
                         << " req_size=" << ev.req_w << "x" << ev.req_h << " applied_size=" << req_w
                         << "x" << req_h
                         << ((resize && (req_w != ev.req_w || req_h != ev.req_h))
                                 ? " [size clamped to root]"
                                 : "");
}

// restore-leg smoke seam: once the seeded xid's iconify has been authored AND the
// iconify-unmap was CONSUMED (the pending-iconify marker cleared), seed ONE Win32-user-origin
// GeometryRequest{Restore} into the worker ring, so run_iconify_smoke.sh exercises
// apply_user_restore over the wire (not just the worker-side restore leg). One-shot. Test-only;
// inert unless
// --debug-feedback-restore is set.
void debug_seed_restore_request(vkr::vkrpc::RpcChannel& rpc) {
    if (!g_debug_feedback_restore || g_restore_seeded || !g_feedback_iconified ||
        g_feedback_xid == 0) {
        return;
    }
    if (g_pending_iconify.find(g_feedback_xid) != g_pending_iconify.end()) {
        return; // the iconify-unmap has not been consumed yet -- wait so the marker is not bypassed
    }
    const auto it = g_tracked.find(g_feedback_xid);
    if (it == g_tracked.end()) {
        return;
    }
    g_restore_seeded = true;
    vkr::sidecar::SidecarDebugEnqueueInputRequest req;
    req.xid = g_feedback_xid;
    req.epoch =
        it->second.epoch; // the worker's current representation epoch (passes the exact gate)
    vkr::sidecar::SidecarInputEvent e;
    e.kind = static_cast<std::uint32_t>(vkr::sidecar::InputEventKind::GeometryRequest);
    e.host_request = static_cast<std::uint32_t>(vkr::sidecar::HostRequestKind::Restore);
    req.events.push_back(e);
    vkr::json::Value resp;
    if (!send_rpc(rpc, vkr::sidecar::SidecarOp::DebugEnqueueInput, req.to_body(), resp)) {
        VKR_WARN(kComponent) << "debug-feedback restore seed failed for xid=" << g_feedback_xid;
        return;
    }
    VKR_INFO(kComponent) << "debug-feedback seeded Restore xid=" << g_feedback_xid;
}

void debug_seed_pending_click(vkr::vkrpc::RpcChannel& rpc, xcb_connection_t* xc, xcb_window_t root);

// Drain the worker's input ring (PollInput, non-blocking on the worker) and inject each event.
// Drops events for an xid the sidecar no longer tracks (unregister/destroy erased g_tracked) -- the
// X-side staleness gate, paired with the worker's own registry-liveness gate. A transport failure
// is logged and skipped (the next tick retries).
void drain_and_inject(vkr::vkrpc::RpcChannel& rpc, xcb_connection_t* xc, xcb_window_t root) {
    // pending-iconify safety sweep: advance the drain tick and drop any marker that
    // outlived its expected consuming UnmapNotify, so a never-arriving iconify-unmap cannot strand
    // the marker and wrongly consume a much-later genuine unmap. A real
    // iconify-unmap is processed at the START of a loop iteration, before this drain, so it always
    // consumes first.
    ++g_drain_tick;
    for (auto it = g_pending_iconify.begin(); it != g_pending_iconify.end();) {
        if (g_drain_tick - it->second.armed_tick >= kPendingIconifySweepTicks) {
            VKR_INFO(kComponent) << "pending-iconify swept (no consuming unmap) xid=" << it->first;
            it = g_pending_iconify.erase(it);
        } else {
            ++it;
        }
    }
    // restore-leg smoke seam (test-only): seed the Restore once the iconify-unmap was
    // consumed, BEFORE PollInput so this same drain picks it up. Inert without
    // --debug-feedback-restore.
    debug_seed_restore_request(rpc);
    // GD/ root-reachability seam: after initial realization settles, derive the live client
    // offset and enqueue the one-shot click immediately before this tick's PollInput.
    debug_seed_pending_click(rpc, xc, root);
    vkr::sidecar::SidecarPollInputRequest req;
    req.since_seq = g_input_cursor;
    vkr::json::Value resp;
    if (!send_rpc(rpc, vkr::sidecar::SidecarOp::PollInput, req.to_body(), resp)) {
        VKR_WARN(kComponent) << "poll_input failed";
        return;
    }
    const vkr::sidecar::SidecarPollInputResponse r =
        vkr::sidecar::SidecarPollInputResponse::from_body(resp);
    g_input_cursor = r.next_seq;
    if (r.dropped) {
        // the `dropped` diagnostic now covers any ring overflow -- a dropped disposable
        // motion (the common flood) OR protected-event soft-cap/hard-ceiling pressure.
        VKR_WARN(kComponent) << "worker host-event ring overflow (disposable motion dropped and/or "
                                "protected soft-cap overrun)";
    }
    for (const auto& ev : r.events) {
        const xcb_window_t w = static_cast<xcb_window_t>(ev.xid);
        const auto it = g_tracked.find(w);
        if (it == g_tracked.end()) {
            VKR_INFO(kComponent) << "drop stale input xid=" << w << " epoch=" << ev.epoch
                                 << " (sidecar no longer tracks it)";
            continue;
        }
        // Epoch reconciliation (epoch-split fix). The worker's poll_input gate is
        // authoritative: every returned event's epoch equals the xid's CURRENT worker
        // representation epoch. So:
        //  * ev.epoch > tracked: the worker's representation ADVANCED without the sidecar observing
        //    it -- a data-plane create_surface promoted this xid's placeholder to a GPU surface
        //    (and minted a new epoch), which is NOT a sidecar toplevel-op, so g_tracked was never
        //    refreshed. A forward worker epoch is authoritative -> ADOPT it + accept. Without this
        //    every real event is dropped once the surface binds (OpenSCAD looks alive but cannot be
        //    rotated). A future explicit transition can keep the chromed toplevel as the input
        //    identity for embedded GL.
        //  * ev.epoch < tracked: a genuinely STALE in-flight event from before a remap the sidecar
        //    already advanced past -> drop (the original defense-in-depth).
        const vkr::sidecar::InputEpochDecision decision =
            vkr::sidecar::reconcile_input_epoch(it->second.epoch, ev.epoch);
        if (decision == vkr::sidecar::InputEpochDecision::AdoptThenAccept) {
            VKR_INFO(kComponent) << "adopt worker epoch xid=" << w << " " << it->second.epoch
                                 << " -> " << ev.epoch << " (data-plane representation change)";
            it->second.epoch = ev.epoch;
        } else if (decision == vkr::sidecar::InputEpochDecision::DropStale) {
            VKR_INFO(kComponent) << "drop stale input xid=" << w << " epoch=" << ev.epoch
                                 << " < tracked " << it->second.epoch;
            continue;
        }
        if (g_input_trace) {
            VKR_INFO(kComponent) << "input-trace station=poll-receive xid=" << ev.xid
                                 << " epoch=" << ev.epoch << " tracked=" << it->second.epoch
                                 << " kind=" << ev.kind << " seq=" << ev.seq;
        }
        // a Win32-user-origin geometry/lifecycle REQUEST is authored against the GUEST
        // (xcb_configure_window), not XTest-injected. Handle + skip the input path.
        if (static_cast<vkr::sidecar::InputEventKind>(ev.kind) ==
            vkr::sidecar::InputEventKind::GeometryRequest) {
            apply_user_geometry(rpc, xc, ev);
            continue;
        }
        inject_event(xc, root, ev);
        // raise-active-toplevel: when a NON-popup toplevel gains focus, the sidecar (the
        // geometry authority) asks the worker to raise its host to the top of the toplevel
        // stack
        // -- a live restack folded into UpdateToplevel. The focus origin is the worker's own
        // WM_SETFOCUS, so this round-trip keeps the raise sidecar-authored (X-origin authority),
        // not a Win32-origin self-raise. Popups ride their owner, so they are not
        // raised here.
        if (static_cast<vkr::sidecar::InputEventKind>(ev.kind) ==
                vkr::sidecar::InputEventKind::Focus &&
            ev.pressed && !it->second.is_popup) {
            forward_update(rpc, xc, w, vkr::sidecar::ZOrder::Raise);
        }
    }
    xcb_flush(xc);
}

// Boundary-smoke seam: seed a canonical neutral input sequence into the worker's ring for `xid`
// (one-shot per xid), so run_input_smoke.sh proves worker-ring -> PollInput -> XTest/focus/close ->
// canary without real Win32 input. The canary asserts it receives exactly this sequence. Test-only
// (Options::debug_inject); a no-op otherwise.
void debug_seed_input(vkr::vkrpc::RpcChannel& rpc, xcb_window_t xid, std::uint64_t epoch) {
    if (!g_debug_inject || xid == 0 || g_seeded.count(xid) != 0) {
        return;
    }
    g_seeded.insert(xid);
    using vkr::sidecar::InputEventKind;
    using vkr::sidecar::SidecarInputEvent;
    vkr::sidecar::SidecarDebugEnqueueInputRequest req;
    req.xid = xid;
    req.epoch = epoch; // seed at the worker's current representation epoch (passes the exact gate)
    auto push = [&](InputEventKind k, int x, int y, int button, int wheel, int vk, bool pressed) {
        SidecarInputEvent e;
        e.kind = static_cast<std::uint32_t>(k);
        e.client_x = x;
        e.client_y = y;
        e.button = button;
        e.wheel = wheel;
        e.vk = vk;
        e.pressed = pressed;
        req.events.push_back(e);
    };
    // The canonical sequence the input canary expects (the warp/motion is order-exempt at the
    // canary because XTest warps before each click): focus, motion, left click, wheel-up, key 'A',
    // close.
    push(InputEventKind::Focus, 0, 0, 0, 0, 0, true);
    push(InputEventKind::Motion, 10, 10, 0, 0, 0, false);
    push(InputEventKind::Button, 10, 10, vkr::sidecar::kInputButtonLeft, 0, 0, true);
    push(InputEventKind::Button, 10, 10, vkr::sidecar::kInputButtonLeft, 0, 0, false);
    push(InputEventKind::Wheel, 10, 10, 0, 1, 0, false);
    push(InputEventKind::Key, 0, 0, 0, 0, 'A', true);
    push(InputEventKind::Key, 0, 0, 0, 0, 'A', false);
    push(InputEventKind::Close, 0, 0, 0, 0, 0, false);
    vkr::json::Value resp;
    if (!send_rpc(rpc, vkr::sidecar::SidecarOp::DebugEnqueueInput, req.to_body(), resp)) {
        VKR_WARN(kComponent) << "debug-inject seed failed for xid=" << xid;
        return;
    }
    VKR_INFO(kComponent) << "debug-inject seeded canonical input for xid=" << xid
                         << " epoch=" << epoch;
}

// Schedule the first toplevel as the debug click's coordinate anchor. Waiting briefly lets the
// initial host-realization feedback converge the X window before client coordinates are derived.
void debug_schedule_click(xcb_window_t xid) {
    if (!g_debug_click || xid == 0 || g_click_seeded || g_click_xid != 0) {
        return;
    }
    g_click_xid = xid;
    g_click_due_ms = now_ms() + 100;
}

// Seed ONE left click at the requested GUEST-ROOT point through the ordinary client-coordinate
// worker ring. The live translation is sampled immediately before enqueue; PollInput runs next in
// the same drain tick, so the production warp translates it back to the exact root point.
void debug_seed_pending_click(vkr::vkrpc::RpcChannel& rpc, xcb_connection_t* xc,
                              xcb_window_t root) {
    if (g_click_xid == 0 || g_click_seeded || now_ms() < g_click_due_ms) {
        return;
    }
    const auto tracked = g_tracked.find(g_click_xid);
    if (tracked == g_tracked.end()) {
        return;
    }
    xcb_translate_coordinates_reply_t* translated = xcb_translate_coordinates_reply(
        xc, xcb_translate_coordinates(xc, g_click_xid, root, 0, 0), nullptr);
    if (translated == nullptr) {
        return;
    }
    const int client_x = g_debug_click_x - static_cast<int>(translated->dst_x);
    const int client_y = g_debug_click_y - static_cast<int>(translated->dst_y);
    free(translated);
    if (client_x < -32768 || client_x > 32767 || client_y < -32768 || client_y > 32767) {
        VKR_WARN(kComponent) << "debug-inject-click root point cannot be represented relative to "
                                "the anchor window";
        g_click_seeded = true;
        return;
    }
    using vkr::sidecar::InputEventKind;
    vkr::sidecar::SidecarDebugEnqueueInputRequest req;
    req.xid = g_click_xid;
    req.epoch = tracked->second.epoch;
    auto push = [&](InputEventKind k, bool pressed) {
        vkr::sidecar::SidecarInputEvent e;
        e.kind = static_cast<std::uint32_t>(k);
        e.client_x = client_x;
        e.client_y = client_y;
        e.button = vkr::sidecar::kInputButtonLeft;
        e.pressed = pressed;
        req.events.push_back(e);
    };
    push(InputEventKind::Motion, false);
    push(InputEventKind::Button, true);
    push(InputEventKind::Button, false);
    vkr::json::Value resp;
    if (!send_rpc(rpc, vkr::sidecar::SidecarOp::DebugEnqueueInput, req.to_body(), resp)) {
        VKR_WARN(kComponent) << "debug-inject-click seed failed for xid=" << g_click_xid;
        return;
    }
    g_click_seeded = true;
    VKR_INFO(kComponent) << "debug-inject-click seeded guest=" << g_debug_click_x << ","
                         << g_debug_click_y << " as client=" << client_x << "," << client_y
                         << " for xid=" << g_click_xid << " epoch=" << req.epoch;
}

// reverse-feedback smoke seam: seed ONE Win32-user-origin GeometryRequest (a committed
// move/resize to g_debug_feedback_x/y[/w/h]) into the worker ring for `xid` (one-shot), so
// drain_and_inject authors the GUEST move/resize -- run_feedback_smoke.sh then asserts the canary's
// X window followed. When g_debug_feedback_minimize is set, ALSO append a
// GeometryRequest{Minimize} so run_iconify_smoke.sh proves the iconify + the pending-iconify guard.
// Test-only (Options::debug_feedback_move); a no-op otherwise.
void debug_seed_geometry_request(vkr::vkrpc::RpcChannel& rpc, xcb_window_t xid,
                                 std::uint64_t epoch) {
    if (!g_debug_feedback || xid == 0 || g_feedback_seeded) {
        return;
    }
    g_feedback_seeded = true;
    g_feedback_xid = xid; // restore-leg smoke: remember which xid the minimize was seeded for
    vkr::sidecar::SidecarDebugEnqueueInputRequest req;
    req.xid = xid;
    req.epoch = epoch; // the worker's current representation epoch (passes the exact gate)
    vkr::sidecar::SidecarInputEvent e;
    e.kind = static_cast<std::uint32_t>(vkr::sidecar::InputEventKind::GeometryRequest);
    e.host_request = static_cast<std::uint32_t>(vkr::sidecar::HostRequestKind::Geometry);
    e.root_x = g_debug_feedback_x;
    e.root_y = g_debug_feedback_y;
    // a nonzero W,H makes this a RESIZE request (req_w/req_h carry the committed extent);
    // 0 keeps it move-only -- so run_feedback_smoke.sh can drive either leg through the same seam.
    e.req_w = static_cast<std::uint32_t>(g_debug_feedback_w);
    e.req_h = static_cast<std::uint32_t>(g_debug_feedback_h);
    req.events.push_back(e);
    if (g_debug_feedback_minimize) {
        // a Win32-user MINIMIZE after the move/resize -> the sidecar authors Iconic + the
        // pending-iconify guard keeps the host Iconic (not Hidden). The move first sets has_actual
        // at the worker so run_iconify_smoke can query the position alongside the Iconic state.
        vkr::sidecar::SidecarInputEvent m;
        m.kind = static_cast<std::uint32_t>(vkr::sidecar::InputEventKind::GeometryRequest);
        m.host_request = static_cast<std::uint32_t>(vkr::sidecar::HostRequestKind::Minimize);
        req.events.push_back(m);
    }
    vkr::json::Value resp;
    if (!send_rpc(rpc, vkr::sidecar::SidecarOp::DebugEnqueueInput, req.to_body(), resp)) {
        VKR_WARN(kComponent) << "debug-feedback seed failed for xid=" << xid;
        return;
    }
    VKR_INFO(kComponent) << "debug-feedback seeded GeometryRequest xid=" << xid
                         << " -> root=" << g_debug_feedback_x << "," << g_debug_feedback_y
                         << " req_size=" << g_debug_feedback_w << "x" << g_debug_feedback_h
                         << " minimize=" << (g_debug_feedback_minimize ? 1 : 0)
                         << " epoch=" << epoch;
}

#ifdef VKRELAY2_HAVE_XCB_XFIXES
// capture the X display's CURRENT cursor (the guest app set it on the window under the
// pointer) and ship it to the worker for `xid`, which builds an HCURSOR + applies it to that xid's
// window. The XFixes cursor image is uint32 ARGB == byte-order BGRA8 on a little-endian host, so
// the pixels ride as BGRA8 with no conversion. Bounded + best-effort: a too-large cursor / fetch
// failure is skipped.
void ship_cursor(vkr::vkrpc::RpcChannel& rpc, xcb_connection_t* xc, xcb_window_t xid) {
    if (!g_xfixes || xid == 0) {
        return;
    }
    // Drop a cursor for an xid the sidecar no longer tracks (unregister/destroy), and stamp the
    // worker's CURRENT epoch so a stale cursor never installs on a new X lifecycle.
    // The worker re-gates on (toplevel_registered && epoch); this is the X-side half.
    const auto tracked = g_tracked.find(xid);
    if (tracked == g_tracked.end()) {
        return;
    }
    xcb_xfixes_get_cursor_image_reply_t* img =
        xcb_xfixes_get_cursor_image_reply(xc, xcb_xfixes_get_cursor_image(xc), nullptr);
    if (img == nullptr) {
        return;
    }
    const std::uint16_t w = img->width;
    const std::uint16_t h = img->height;
    if (w == 0 || h == 0 || w > vkr::sidecar::kMaxCursorDim || h > vkr::sidecar::kMaxCursorDim) {
        free(img); // outside the bounded subset (no realistic theme cursor is this big) -> skip
        return;
    }
    const std::uint32_t* pixels = xcb_xfixes_get_cursor_image_cursor_image(img);
    const int n = xcb_xfixes_get_cursor_image_cursor_image_length(img);
    if (pixels == nullptr || n < static_cast<int>(w) * h) {
        free(img);
        return;
    }
    vkr::sidecar::SidecarSetCursorRequest req;
    req.xid = xid;
    req.epoch = tracked->second.epoch; // the worker's current representation epoch for this xid
    req.width = w;
    req.height = h;
    // Clamp the hotspot into the image (the decoder requires it; a valid X cursor always
    // satisfies).
    req.xhot = img->xhot < w ? img->xhot : w - 1;
    req.yhot = img->yhot < h ? img->yhot : h - 1;
    req.format = vkr::sidecar::kCursorFormatBgra8;
    req.pixels.assign(reinterpret_cast<const char*>(pixels), static_cast<std::size_t>(w) * h * 4);
    free(img);
    vkr::json::Value resp;
    if (!send_rpc_wire(rpc, vkr::sidecar::SidecarOp::SetCursor, req.to_wire(), resp)) {
        VKR_WARN(kComponent) << "set_cursor forward failed for xid " << xid;
        return;
    }
    const vkr::sidecar::SidecarSetCursorResponse r =
        vkr::sidecar::SidecarSetCursorResponse::from_body(resp);
    VKR_INFO(kComponent) << "set_cursor xid=" << xid << " " << w << "x" << h
                         << " applied=" << (r.applied ? 1 : 0);
}
#else
void ship_cursor(vkr::vkrpc::RpcChannel&, xcb_connection_t*, xcb_window_t) {}
#endif

// classify an override-redirect window for popup representation. `candidate` is true
// if the window advertises a recognized popup window-type (_NET_WM_WINDOW_TYPE_{MENU,POPUP_MENU,
// DROPDOWN_MENU,TOOLTIP,COMBO}) OR is UNTYPED (the owner-gated fallback). A window that advertises
// a _NET_WM_WINDOW_TYPE naming ONLY non-popup types is NOT a candidate (it declared itself
// non-popup -> dropped). `kind` is the matched PopupKind token (None/0 when untyped).
struct PopupClass {
    bool candidate = false;
    std::uint32_t kind = 0;
};
PopupClass classify_popup_type(xcb_connection_t* xc, xcb_window_t w) {
    PopupClass pc;
    if (g_net_wm_window_type == 0) {
        pc.candidate = true; // atoms not interned (should not happen post-startup) -> untyped path
        return pc;
    }
    xcb_get_property_reply_t* r = xcb_get_property_reply(
        xc, xcb_get_property(xc, 0, w, g_net_wm_window_type, XCB_ATOM_ATOM, 0, 32), nullptr);
    if (r == nullptr) {
        pc.candidate = true; // property read failed -> untyped fallback (owner-gated downstream)
        return pc;
    }
    const int n = xcb_get_property_value_length(r) / static_cast<int>(sizeof(xcb_atom_t));
    if (n <= 0) {
        free(r);
        pc.candidate = true; // no window type -> untyped fallback
        return pc;
    }
    const xcb_atom_t* types = static_cast<const xcb_atom_t*>(xcb_get_property_value(r));
    for (int i = 0; i < n; ++i) {
        const xcb_atom_t t = types[i];
        if (t == g_net_wm_window_type_menu) {
            pc.kind = static_cast<std::uint32_t>(vkr::sidecar::PopupKind::Menu);
        } else if (t == g_net_wm_window_type_popup_menu) {
            pc.kind = static_cast<std::uint32_t>(vkr::sidecar::PopupKind::PopupMenu);
        } else if (t == g_net_wm_window_type_dropdown_menu) {
            pc.kind = static_cast<std::uint32_t>(vkr::sidecar::PopupKind::DropdownMenu);
        } else if (t == g_net_wm_window_type_tooltip) {
            pc.kind = static_cast<std::uint32_t>(vkr::sidecar::PopupKind::Tooltip);
        } else if (t == g_net_wm_window_type_combo) {
            pc.kind = static_cast<std::uint32_t>(vkr::sidecar::PopupKind::Combo);
        } else {
            continue; // unrecognized type atom -> keep scanning the priority-ordered list
        }
        pc.candidate = true;
        break; // first recognized popup type wins (EWMH priority order)
    }
    free(r);
    return pc; // typed-but-none-recognized leaves candidate=false (a non-popup window type)
}

// resolve a popup's NON-popup owner toplevel (the z-order anchor). WM_TRANSIENT_FOR
// (walking through any intermediate popups to the nearest non-popup) -> the last pointer target ->
// the focus owner -> 0 (drop). Each candidate must be a currently-tracked, non-popup toplevel.
xcb_window_t resolve_popup_owner(xcb_connection_t* xc, xcb_window_t w) {
    xcb_window_t transient = 0;
    xcb_get_property_reply_t* r = xcb_get_property_reply(
        xc, xcb_get_property(xc, 0, w, XCB_ATOM_WM_TRANSIENT_FOR, XCB_ATOM_WINDOW, 0, 1), nullptr);
    if (r != nullptr) {
        if (xcb_get_property_value_length(r) >= static_cast<int>(sizeof(xcb_window_t))) {
            transient = *static_cast<const xcb_window_t*>(xcb_get_property_value(r));
        }
        free(r);
    }
    if (transient != 0) {
        // Walk through intermediate popups to the nearest non-popup (bounded against a cycle).
        xcb_window_t t = transient;
        for (int hops = 0; hops < 8 && t != 0; ++hops) {
            const auto it = g_tracked.find(t);
            if (it == g_tracked.end()) {
                break; // not tracked -> fall through to the pointer/focus fallbacks
            }
            if (!it->second.is_popup) {
                return t; // a live non-popup toplevel -> the z-order anchor
            }
            t = it->second.owner; // a popup -> follow its owner edge
        }
    }
    const auto is_nonpopup_toplevel = [](xcb_window_t x) {
        const auto it = g_tracked.find(x);
        return it != g_tracked.end() && !it->second.is_popup;
    };
    if (g_last_pointer_xid != 0 && is_nonpopup_toplevel(g_last_pointer_xid)) {
        return g_last_pointer_xid;
    }
    if (g_focus_xid != 0 && is_nonpopup_toplevel(g_focus_xid)) {
        return g_focus_xid;
    }
    return 0;
}

// read a window's WM_CLASS as a single string (instance/class joined; NULs -> spaces)
// for the toolkit-junk filter. Empty when unset/unreadable.
std::string read_wm_class(xcb_connection_t* xc, xcb_window_t w) {
    xcb_get_property_reply_t* r = xcb_get_property_reply(
        xc, xcb_get_property(xc, 0, w, XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 0, 256), nullptr);
    if (r == nullptr) {
        return {};
    }
    const int len = xcb_get_property_value_length(r);
    std::string s(static_cast<const char*>(xcb_get_property_value(r)),
                  len > 0 ? static_cast<std::size_t>(len) : 0u);
    free(r);
    for (char& ch : s) {
        if (ch == '\0') {
            ch = ' '; // join "instance\0class\0" so a substring search spans both
        }
    }
    return s;
}

// an override-redirect window self-mapped (MapNotify, no MapRequest). Classify it:
// a ROOT-COVERING one is an app FULLSCREEN toplevel (SFML 2.5 fullscreen, the ExtremeTuxRacer
// class) and is registered as a normal toplevel; a popup with a resolvable owner that passes the
// sanity gates gets a worker representation (a placeholder-class host owned by + above its owner)
// + chrome capture. Dropped (logged) otherwise. Idempotent: a window already tracked is skipped
// (a duplicate MapNotify).
void maybe_register_popup(vkr::vkrpc::RpcChannel& rpc, xcb_connection_t* xc,
                          const xcb_setup_t* setup, xcb_screen_t* screen, xcb_window_t w) {
    if (g_tracked.find(w) != g_tracked.end()) {
        return; // already represented (duplicate MapNotify)
    }
    // Sanity: drop InputOnly helper windows (no pixels to represent/capture).
    xcb_get_window_attributes_reply_t* a =
        xcb_get_window_attributes_reply(xc, xcb_get_window_attributes(xc, w), nullptr);
    if (a != nullptr) {
        const bool input_only = a->_class == XCB_WINDOW_CLASS_INPUT_ONLY;
        free(a);
        if (input_only) {
            VKR_INFO(kComponent) << "popup drop xid=" << w << " (input-only)";
            return;
        }
    }
    // Sanity: drop zero/degenerate-tiny. The size floor (popup_size_ok) keeps the untyped
    // owner-gated fallback from representing 1x1 / icon-scratch helpers.
    xcb_get_geometry_reply_t* g = xcb_get_geometry_reply(xc, xcb_get_geometry(xc, w), nullptr);
    if (g == nullptr) {
        return; // gone already
    }
    const std::uint16_t gw = g->width;
    const std::uint16_t gh = g->height;
    free(g);
    if (!vkr::sidecar::popup_size_ok(gw, gh)) {
        VKR_INFO(kComponent) << "popup drop xid=" << w << " (zero/tiny " << gw << "x" << gh << ")";
        return;
    }
    // Sanity: drop known toolkit scratch/helper windows by WM_CLASS (the extensible named junk
    // hook).
    const std::string wm_class = read_wm_class(xc, w);
    if (vkr::sidecar::is_known_toolkit_junk(wm_class)) {
        VKR_INFO(kComponent) << "popup drop xid=" << w << " (toolkit junk wm_class=\"" << wm_class
                             << "\")";
        return;
    }
    // A root-covering override-redirect window is an app FULLSCREEN toplevel, not a popup: SFML
    // 2.5 (the ExtremeTuxRacer class) maps fullscreen this way when no EWMH WM is advertised, then
    // busy-waits in sf::Window::create for a non-obscured VisibilityNotify. Rootless Xwayland
    // reports an unredirected window FullyObscured, so DROPPING it (the old behavior) both hid the
    // app on the host and hung it forever in that spin. Register it as a normal toplevel: it gets
    // a host window + capture on the standard rails, and cap_arm's composite redirect flips the
    // server's visibility to Unobscured -- the exact event that releases the app.
    if (vkr::sidecar::is_fullscreen_toplevel(gw, gh, screen->width_in_pixels,
                                             screen->height_in_pixels)) {
        VKR_INFO(kComponent) << "fullscreen or-toplevel xid=" << w << " (" << gw << "x" << gh
                             << " covers root " << screen->width_in_pixels << "x"
                             << screen->height_in_pixels << ") -- registering as a toplevel";
        forward_register(rpc, xc, w, /*override_redirect=*/true);
        cap_arm(xc, w);
        cap_ship(rpc, xc, setup, screen, w);
        return;
    }
    const PopupClass pc = classify_popup_type(xc, w);
    if (!pc.candidate) {
        VKR_INFO(kComponent) << "popup drop xid=" << w << " (non-popup window type)";
        return;
    }
    const xcb_window_t owner = resolve_popup_owner(xc, w);
    if (owner == 0) {
        VKR_INFO(kComponent) << "popup drop xid=" << w << " (no resolvable owner)";
        return;
    }
    // Represent it, then capture chrome. An already-mapped popup gets no Expose, so arm + ship
    // immediately (mirrors the initial-scan already-mapped path).
    forward_register(rpc, xc, w, /*override_redirect=*/true, /*is_popup=*/true, owner, pc.kind);
    cap_arm(xc, w);
    cap_ship(rpc, xc, setup, screen, w);
}

// Place a freshly mapped toplevel on a concrete monitor. Reads WM_NORMAL_HINTS
// via a manual CORE-XCB property read (predefined atoms; no xcb-icccm). If the app set NO
// USPosition/PPosition
// intent it configures the CENTERED client origin; if it DID declare a position, clamp that
// position on-screen (an off-top/-left app origin must not hide the menu bar; an on-screen position
// incl. 0,0 is left as-is). Configured ON THE X WINDOW *before* it is mapped -- so the geometry
// forward_register reads AND the X-root coordinate system that popup x/y + input injection ride
// both reflect the on-screen origin. Called only on the fresh-register
// MapRequest path (not restore, not the already-mapped initial scan). The Win32-user reverse path
// (apply_user_geometry) is deliberately NOT clamped -- a user may drag partly off-screen.
void maybe_center_toplevel(xcb_connection_t* xc, xcb_screen_t* screen, xcb_window_t w,
                           bool fresh_map = true) {
    if (screen == nullptr) {
        return;
    }
    // WM_NORMAL_HINTS is a WM_SIZE_HINTS (format 32, up to 18 words); read enough to see the flags.
    xcb_get_property_reply_t* hints = xcb_get_property_reply(
        xc, xcb_get_property(xc, 0, w, XCB_ATOM_WM_NORMAL_HINTS, XCB_ATOM_WM_SIZE_HINTS, 0, 18),
        nullptr);
    bool explicit_pos = false;
    if (hints != nullptr) {
        const int len = xcb_get_property_value_length(hints);
        const std::size_t nwords = len > 0 ? static_cast<std::size_t>(len) / 4 : 0;
        explicit_pos = vkr::sidecar::wm_hints_has_explicit_position(
            reinterpret_cast<const std::uint32_t*>(xcb_get_property_value(hints)), nwords);
        free(hints);
    }
    // An app-declared position (USPosition/PPosition) is HONORED but CLAMPED on-screen: an
    // app that asks for an off-top/-left origin must not put its menu bar out of reach. An
    // on-screen position (incl. 0,0) is identity, so glxgears' USPosition 0,0 and the cursor
    // canary's capture warp are unaffected. A window with NO declared position is centered.
    xcb_get_geometry_reply_t* g = xcb_get_geometry_reply(xc, xcb_get_geometry(xc, w), nullptr);
    if (g == nullptr) {
        return;
    }
    const std::int16_t gx = g->x, gy = g->y;
    const std::uint16_t cw = g->width, ch = g->height;
    free(g);
    const bool preserve_existing = !fresh_map;
    if (g_has_display_layout) {
        vkr::display::InitialPlacementMode mode =
            (explicit_pos || preserve_existing) ? vkr::display::InitialPlacementMode::Preserve
                                                : vkr::display::InitialPlacementMode::CenterPrimary;
        vkr::display::RectI32 owner_rect;
        const vkr::display::RectI32* owner_ptr = nullptr;
        xcb_window_t owner = 0;
        if (!explicit_pos && !preserve_existing) {
            xcb_get_property_reply_t* tr = xcb_get_property_reply(
                xc, xcb_get_property(xc, 0, w, XCB_ATOM_WM_TRANSIENT_FOR, XCB_ATOM_WINDOW, 0, 1),
                nullptr);
            if (tr != nullptr) {
                if (xcb_get_property_value_length(tr) >= static_cast<int>(sizeof(xcb_window_t))) {
                    owner = *static_cast<const xcb_window_t*>(xcb_get_property_value(tr));
                }
                free(tr);
            }
            const auto tracked = g_tracked.find(owner);
            if (owner != 0 && tracked != g_tracked.end() && !tracked->second.is_popup) {
                xcb_get_geometry_reply_t* og =
                    xcb_get_geometry_reply(xc, xcb_get_geometry(xc, owner), nullptr);
                if (og != nullptr) {
                    owner_rect = {og->x, og->y, og->width, og->height};
                    owner_ptr = &owner_rect;
                    mode = vkr::display::InitialPlacementMode::CenterOwner;
                    free(og);
                }
            }
        }
        vkr::display::RectI32 placed;
        const vkr::display::MonitorDesc* selected = nullptr;
        if (vkr::display::place_guest_window(g_display_layout, {gx, gy, cw, ch}, mode, owner_ptr,
                                             placed, &selected)) {
            if (placed.x != gx || placed.y != gy) {
                const std::uint32_t xy[2] = {static_cast<std::uint32_t>(placed.x),
                                             static_cast<std::uint32_t>(placed.y)};
                xcb_configure_window(xc, w, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, xy);
            }
            VKR_INFO(kComponent) << "monitor placement xid=" << w << " " << cw << "x" << ch << " "
                                 << gx << "," << gy << " -> " << placed.x << "," << placed.y
                                 << " monitor="
                                 << (selected != nullptr ? selected->stable_id : "none")
                                 << " mode=" << static_cast<int>(mode) << " owner=" << owner;
            return;
        }
        VKR_WARN(kComponent) << "monitor placement failed for xid=" << w
                             << "; using legacy root policy";
    }
    if (explicit_pos || preserve_existing) {
        const vkr::sidecar::ClientOrigin o = vkr::sidecar::clamp_client_origin_to_root(
            screen->width_in_pixels, screen->height_in_pixels, gx, gy, cw, ch);
        if (o.x == gx && o.y == gy) {
            VKR_INFO(kComponent) << "honoring on-screen explicit position for xid=" << w;
            return; // already on-screen -> no needless configure
        }
        const std::uint32_t xy[2] = {static_cast<std::uint32_t>(o.x),
                                     static_cast<std::uint32_t>(o.y)};
        xcb_configure_window(xc, w, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, xy);
        VKR_INFO(kComponent) << "clamped explicit toplevel xid=" << w << " " << cw << "x" << ch
                             << " " << gx << "," << gy << " -> " << o.x << "," << o.y << " (root "
                             << screen->width_in_pixels << "x" << screen->height_in_pixels << ")";
        return;
    }
    const vkr::sidecar::ClientOrigin o = vkr::sidecar::centered_client_origin(
        screen->width_in_pixels, screen->height_in_pixels, cw, ch);
    const std::uint32_t xy[2] = {static_cast<std::uint32_t>(o.x), static_cast<std::uint32_t>(o.y)};
    xcb_configure_window(xc, w, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, xy);
    VKR_INFO(kComponent) << "centered toplevel xid=" << w << " " << cw << "x" << ch << " -> " << o.x
                         << "," << o.y << " (root " << screen->width_in_pixels << "x"
                         << screen->height_in_pixels << ")";
}

// Handle one redirected/notify X event: the transparent pass-through WM + the capture
// triggers + register-time input seeding + cursor capture + popup detection.
// Extracted so the main loop stays a clean timer loop that drains X events non-blockingly and never
// starves PollInput.
void process_x_event(vkr::vkrpc::RpcChannel& rpc, xcb_connection_t* xc, const xcb_setup_t* setup,
                     xcb_screen_t* screen, xcb_generic_event_t* ev) {
    const std::uint8_t kind = ev->response_type & ~0x80;
    if (kind == XCB_MAP_REQUEST) {
        const xcb_map_request_event_t* mr = reinterpret_cast<xcb_map_request_event_t*>(ev);
        // a tracked toplevel remapping is a RESTORE (not a fresh register) -- detect BEFORE
        // cap_arm, which creates the g_tracked entry for a fresh window.
        const bool restore = g_tracked.find(mr->window) != g_tracked.end();
        // ARM chrome capture BEFORE mapping: select Expose + redirect while the
        // window is still unmapped, so the first Expose generated by the map cannot fire before we
        // are listening. The first paint ships on that Expose (cap_ship names the pixmap lazily).
        // On a restore the pixmap was invalidated on unmap; re-arming re-selects the redirect so
        // the post-restore Expose re-names + recaptures fresh.
        cap_arm(xc, mr->window);
        // a FRESH (non-restore) toplevel with no app-declared position is centered on the X
        // root BEFORE mapping; a restore keeps its preserved position. Authoring the position on
        // the X window (not just the forwarded geometry) keeps the registry + X-root coords
        // aligned.
        if (!restore) {
            maybe_center_toplevel(xc, screen, mr->window);
        }
        xcb_map_window(xc, mr->window);
        xcb_flush(xc);
        // A MapRequest is only redirected to the WM for NON-override_redirect windows, so a mapped
        // client here is a guest toplevel by definition. A tracked remap is a RESTORE (forward
        // Visible, representation/epoch preserved); a fresh map = register + (
        // when debug-inject is on) seed the canonical input sequence for it.
        if (restore) {
            forward_visibility(rpc, mr->window, vkr::sidecar::VisibilityState::Visible);
            g_recapture.track(mr->window, now_ms()); // restart the warm-up burst on restore
        } else {
            forward_register(rpc, xc, mr->window, false);
            debug_seed_input(rpc, mr->window, g_tracked[mr->window].epoch);
            // GD bigroot smoke: schedule one guest-root click after initial realization settles.
            debug_schedule_click(mr->window);
            // reverse-feedback smoke: seed a Win32-user-origin GeometryRequest (one-shot).
            debug_seed_geometry_request(rpc, mr->window, g_tracked[mr->window].epoch);
        }
    } else if (kind == XCB_MAP_NOTIFY) {
        // an OVERRIDE-REDIRECT window self-maps -- it bypasses SubstructureRedirect
        // (no MapRequest fires) and surfaces here under the root's SUBSTRUCTURE_NOTIFY. Non-OR
        // toplevels also emit a MapNotify after we map them, but with override_redirect=false
        // (skipped -- they registered on their MapRequest). Only a direct child of root (event ==
        // root) is a candidate popup.
        const xcb_map_notify_event_t* mn = reinterpret_cast<xcb_map_notify_event_t*>(ev);
        if (mn->override_redirect != 0 && mn->event == screen->root) {
            // a tracked popup remapping is a RESTORE (maybe_register_popup would SKIP it as
            // a duplicate, stranding it Hidden forever). Forward Visible + recapture (an
            // already-mapped popup gets no Expose, so re-arm + ship now, like the initial
            // already-mapped path).
            if (g_tracked.find(mn->window) != g_tracked.end()) {
                cap_arm(xc, mn->window);
                // C: the UnmapNotify that hid this popup forgot it from the
                // recapture policy, so re-TRACK on remap (restart the warm-up burst) -- otherwise a
                // same-XID popup reshow (Qt can hide/reshow a transient widget, not just
                // destroy/recreate) would fall back to one-shot capture and paint black again. This
                // mirrors the MapRequest restore path's re-track for non-popup toplevels.
                g_recapture.track(mn->window, now_ms());
                // Qt may move a same-XID popup while it is unmapped. Execute the shared
                // remap plan literally: update its hidden HWND before publishing Visible, then
                // paint at the newest (visibility) lifecycle generation. Worker update_toplevel
                // synchronously drains popup geometry on the window thread, so the following
                // visibility RPC cannot reveal the old host rect (integration-pinned).
                for (const vkr::sidecar::PopupRemapAction action :
                     vkr::sidecar::popup_remap_actions()) {
                    switch (action) {
                    case vkr::sidecar::PopupRemapAction::UpdateGeometry:
                        forward_update(rpc, xc, mn->window);
                        break;
                    case vkr::sidecar::PopupRemapAction::SetVisible:
                        forward_visibility(rpc, mn->window, vkr::sidecar::VisibilityState::Visible);
                        break;
                    case vkr::sidecar::PopupRemapAction::PaintChrome:
                        cap_ship(rpc, xc, setup, screen, mn->window);
                        break;
                    }
                }
            } else {
                maybe_register_popup(rpc, xc, setup, screen, mn->window);
            }
        }
    } else if (kind == XCB_CONFIGURE_REQUEST) {
        // Apply the requested geometry. Build the value list from value_mask in the canonical order
        // X, Y, WIDTH, HEIGHT, BORDER_WIDTH, SIBLING, STACK_MODE.
        const xcb_configure_request_event_t* cr =
            reinterpret_cast<xcb_configure_request_event_t*>(ev);
        // Recover an app-authored client ORIGIN to a concrete snapshot monitor before
        // applying + forwarding (saved geometry may otherwise land off-screen or in a virtual
        // hole). Override-redirect popups self-map and never reach ConfigureRequest; the Win32-user
        // reverse path configures directly, so a real user may still drag partly off-screen.
        int clamped_x = cr->x;
        int clamped_y = cr->y;
        if ((cr->value_mask & (XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y)) != 0 &&
            screen != nullptr) {
            int cw = (cr->value_mask & XCB_CONFIG_WINDOW_WIDTH) ? static_cast<int>(cr->width) : 0;
            int ch = (cr->value_mask & XCB_CONFIG_WINDOW_HEIGHT) ? static_cast<int>(cr->height) : 0;
            int current_x = cr->x;
            int current_y = cr->y;
            if (cw == 0 || ch == 0 ||
                (cr->value_mask & (XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y)) !=
                    (XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y)) {
                xcb_get_geometry_reply_t* g =
                    xcb_get_geometry_reply(xc, xcb_get_geometry(xc, cr->window), nullptr);
                if (g != nullptr) {
                    if (cw == 0) {
                        cw = g->width;
                    }
                    if (ch == 0) {
                        ch = g->height;
                    }
                    current_x = g->x;
                    current_y = g->y;
                    free(g);
                }
            }
            const int requested_x = (cr->value_mask & XCB_CONFIG_WINDOW_X) ? cr->x : current_x;
            const int requested_y = (cr->value_mask & XCB_CONFIG_WINDOW_Y) ? cr->y : current_y;
            if (g_has_display_layout) {
                vkr::display::RectI32 placed;
                if (vkr::display::place_guest_window(
                        g_display_layout, {requested_x, requested_y, cw, ch},
                        vkr::display::InitialPlacementMode::Preserve, nullptr, placed)) {
                    clamped_x = placed.x;
                    clamped_y = placed.y;
                }
            } else {
                const vkr::sidecar::ClientOrigin o = vkr::sidecar::clamp_client_origin_to_root(
                    screen->width_in_pixels, screen->height_in_pixels, requested_x, requested_y, cw,
                    ch);
                clamped_x = o.x;
                clamped_y = o.y;
            }
        }
        std::uint32_t values[7];
        int n = 0;
        if (cr->value_mask & XCB_CONFIG_WINDOW_X) {
            values[n++] = static_cast<std::uint32_t>(clamped_x);
        }
        if (cr->value_mask & XCB_CONFIG_WINDOW_Y) {
            values[n++] = static_cast<std::uint32_t>(clamped_y);
        }
        if (cr->value_mask & XCB_CONFIG_WINDOW_WIDTH) {
            values[n++] = cr->width;
        }
        if (cr->value_mask & XCB_CONFIG_WINDOW_HEIGHT) {
            values[n++] = cr->height;
        }
        if (cr->value_mask & XCB_CONFIG_WINDOW_BORDER_WIDTH) {
            values[n++] = cr->border_width;
        }
        if (cr->value_mask & XCB_CONFIG_WINDOW_SIBLING) {
            values[n++] = cr->sibling;
        }
        if (cr->value_mask & XCB_CONFIG_WINDOW_STACK_MODE) {
            values[n++] = cr->stack_mode;
        }
        xcb_configure_window(xc, cr->window, cr->value_mask, values);
        xcb_flush(xc);
        // Forward the resulting geometry to the worker registry (the worker is the geometry
        // authority): move / resize, and the STACK_MODE -> a live restack (a
        // simple X stack request). Above -> Raise, Below -> Lower; other modes (TopIf/BottomIf/
        // Opposite, which need sibling/occlusion context we do not model) -> None. A no-op for an
        // unregistered window (e.g. a configure before the toplevel's MapRequest).
        vkr::sidecar::ZOrder z = vkr::sidecar::ZOrder::None;
        if (cr->value_mask & XCB_CONFIG_WINDOW_STACK_MODE) {
            if (cr->stack_mode == XCB_STACK_MODE_ABOVE) {
                z = vkr::sidecar::ZOrder::Raise;
            } else if (cr->stack_mode == XCB_STACK_MODE_BELOW) {
                z = vkr::sidecar::ZOrder::Lower;
            }
        }
        forward_update(rpc, xc, cr->window, z);
    } else if (kind == XCB_CONFIGURE_NOTIFY) {
        // The realized reconfigure (size/content): invalidate XComposite's named backing pixmap on
        // the resize EDGE, then re-capture after the resize holdoff settles. Stamped with the
        // generation forward_update just advanced.
        const xcb_configure_notify_event_t* cn =
            reinterpret_cast<xcb_configure_notify_event_t*>(ev);
        cap_invalidate_pixmap_on_configure(xc, cn->window, cn->width, cn->height);
        // override-redirect popups bypass ConfigureRequest, so this realized notification is
        // their only live geometry-bearing signal. Forward it BEFORE recapture so any subsequent
        // chrome is stamped against the advanced lifecycle generation. Ordinary tracked
        // toplevels retain their existing ConfigureRequest/host-feedback authority.
        const auto tracked = g_tracked.find(cn->window);
        if (vkr::sidecar::popup_configure_forwards_geometry(tracked != g_tracked.end(),
                                                            tracked != g_tracked.end() &&
                                                                tracked->second.is_popup)) {
            forward_update(rpc, xc, cn->window);
        }
        const std::uint64_t t = now_ms();
        g_recapture.on_resize(cn->window, t); // hold off recapture until geometry settles
        // during an active resize, SKIP the immediate cap_ship for a TRACKED
        // placeholder -- the steady recapture tick names the fresh pixmap and ships once geometry
        // settles, so a resize storm does not ship a full
        // XComposite capture on every ConfigureNotify. on_resize() set the holdoff just above, so a
        // tracked window is now in_resize_holdoff; UNTRACKED windows (a one-shot window not in the
        // policy) are not, so they keep their existing immediate-ship behavior. (Separate from
        // due(): we must not fold the recapture cadence into this immediate path.)
        if (!g_recapture.in_resize_holdoff(cn->window, t)) {
            cap_ship(rpc, xc, setup, screen, cn->window);
        }
    } else if (kind == XCB_EXPOSE) {
        // The toplevel's content is ready / changed -> capture + ship (the first paint for a
        // freshly mapped chrome window arrives here, with the rendered fill).
        const xcb_expose_event_t* ee = reinterpret_cast<xcb_expose_event_t*>(ev);
        cap_ship(rpc, xc, setup, screen, ee->window);
    } else if (kind == XCB_UNMAP_NOTIFY) {
        // an unmap is HIDDEN-BUT-REPRESENTED, NOT a teardown (the X window still exists;
        // this is a minimize / toolkit hide-to-reshow). Forward Visibility=Hidden (the worker
        // SW_HIDEs the host) and KEEP the representation (HWND/surface/chrome/epoch) + the
        // g_tracked entry + the input seed, so a remap is a free restore (no rebuild, epoch
        // unchanged). The named pixmap is invalid after unmap, so invalidate it (re-named on the
        // post-restore Expose); do NOT cap_stop (we still own the window). DestroyNotify (below) is
        // the real teardown.
        const xcb_unmap_notify_event_t* un = reinterpret_cast<xcb_unmap_notify_event_t*>(ev);
        const auto it = g_tracked.find(un->window);
        if (it != g_tracked.end()) {
            // The named backing pixmap is invalid after ANY unmap -> invalidate it regardless, so a
            // later restore recaptures FRESH chrome. This non-visibility bookkeeping runs even when
            // the pending-iconify guard consumes the Hidden downgrade.
            cap_invalidate_pixmap(xc, un->window);
            g_recapture.forget(un->window); // stop recapture while hidden (re-track on restore)
            // pending-iconify echo guard: if this unmap is the guest's self-induced iconify
            // (a Win32-USER minimize the sidecar authored), CONSUME only the Hidden downgrade -- so
            // the host stays Iconic (taskbar-restorable, NOT SW_HIDE) -- and clear the marker. An
            // unmap with NO (or a stale-epoch) marker keeps the Hidden behavior (a real
            // toolkit hide-to-reshow). The epoch guards against consuming an unrelated unmap of a
            // re-represented xid (UnmapNotify carries no sidecar generation).
            const auto pi = g_pending_iconify.find(un->window);
            if (pi != g_pending_iconify.end() && pi->second.epoch == it->second.epoch) {
                g_pending_iconify.erase(pi);
                VKR_INFO(kComponent) << "unmap consumed by pending-iconify xid=" << un->window
                                     << " (host stays Iconic)";
            } else {
                if (pi != g_pending_iconify.end()) {
                    g_pending_iconify.erase(pi); // stale-epoch marker -> drop it, forward Hidden
                }
                forward_visibility(rpc, un->window, vkr::sidecar::VisibilityState::Hidden);
            }
        }
    } else if (kind == XCB_DESTROY_NOTIFY) {
        // The X window is GONE for good -> the real teardown (unregister: drop the representation,
        // HWND, surface binding, epoch). Distinct from UnmapNotify above.
        const xcb_destroy_notify_event_t* dn = reinterpret_cast<xcb_destroy_notify_event_t*>(ev);
        cap_stop(xc, dn->window);
        g_recapture.forget(dn->window); // teardown -> stop recapture
        forward_unregister(rpc, dn->window);
        g_seeded.erase(dn->window);
    }
#ifdef VKRELAY2_HAVE_XCB_XFIXES
    else if (g_xfixes && kind == g_xfixes_event_base + XCB_XFIXES_CURSOR_NOTIFY) {
        // the X display cursor changed (the guest app set its cursor) -> capture + ship
        // it to the worker for the window the pointer is currently over (the warp target).
        ship_cursor(rpc, xc, g_last_pointer_xid);
    }
#endif
}

} // namespace

int run_sidecar(int argc, char** argv) {
    vkr::log::set_component(kComponent);

    Options opts;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto take = [&](std::string& dst) -> bool {
            if (i + 1 >= argc) {
                VKR_ERROR(kComponent) << "missing value for " << arg;
                return false;
            }
            dst = argv[++i];
            return true;
        };
        if (arg == "--connect") {
            if (!take(opts.connect)) {
                return 2;
            }
        } else if (arg == "--sidecar-token") {
            if (!take(opts.sidecar_token)) {
                return 2;
            }
        } else if (arg == "--display") {
            if (!take(opts.display)) {
                return 2;
            }
        } else if (arg == "--ready-fd") {
            std::string v;
            if (!take(v)) {
                return 2;
            }
            opts.ready_fd = std::atoi(v.c_str());
        } else if (arg == "--debug-inject") {
            opts.debug_inject = true; // boundary-smoke seam (test-only)
        } else if (arg == "--debug-inject-click") {
            if (!take(opts.debug_inject_click)) { // "X,Y" (GD bigroot smoke seam, test-only)
                return 2;
            }
        } else if (arg == "--debug-feedback-move") {
            if (!take(opts.debug_feedback_move)) { // "X,Y" / "X,Y,W,H" (feedback smoke seam)
                return 2;
            }
        } else if (arg == "--debug-feedback-minimize") {
            opts.debug_feedback_minimize = true; // iconify smoke seam (test-only)
        } else if (arg == "--debug-feedback-restore") {
            opts.debug_feedback_restore = true; // restore-leg smoke seam (test-only)
        } else {
            VKR_ERROR(kComponent) << "unknown argument: " << arg;
            return 2;
        }
    }

    std::string host;
    int port = 0;
    if (!parse_host_port(opts.connect, host, port)) {
        VKR_ERROR(kComponent) << "invalid --connect endpoint: " << opts.connect;
        return 2;
    }

    // 1) Connect to the worker's sidecar plane: SidecarHello/Ack (token-gated), then the binary RPC
    //    envelope with a capability negotiate.
    std::unique_ptr<vkr::transport::Connection> conn;
    try {
        conn = vkr::transport::tcp_connect(host, port);
    } catch (const std::exception& e) {
        VKR_ERROR(kComponent) << "cannot reach the worker sidecar plane " << opts.connect << ": "
                              << e.what();
        return 3;
    }
    vkr::transport::MessageChannel channel(*conn);
    vkr::protocol::SidecarHello hello;
    hello.sidecar_token = opts.sidecar_token;
    channel.send(vkr::protocol::MessageType::SidecarHello, hello.to_body());
    vkr::protocol::MessageType type = vkr::protocol::MessageType::Unknown;
    vkr::json::Value body;
    if (!channel.recv(type, body) || type != vkr::protocol::MessageType::SidecarAck) {
        VKR_ERROR(kComponent) << "worker rejected the sidecar handshake";
        return 3;
    }
    const vkr::protocol::SidecarAck sidecar_ack = vkr::protocol::SidecarAck::from_body(body);
    if (!sidecar_ack.ok) {
        VKR_ERROR(kComponent) << "worker rejected the sidecar handshake";
        return 3;
    }
    if (sidecar_ack.display_layout.status == vkr::display::LayoutDecodeStatus::Malformed ||
        sidecar_ack.display_layout.status == vkr::display::LayoutDecodeStatus::UnsupportedSchema) {
        VKR_ERROR(kComponent) << "worker sent unusable display layout: "
                              << sidecar_ack.display_layout.reason;
        return 3;
    }
    if (sidecar_ack.display_layout.status == vkr::display::LayoutDecodeStatus::Valid) {
        g_display_layout = sidecar_ack.display_layout.layout;
        g_has_display_layout = true;
        VKR_INFO(kComponent) << "pinned placement snapshot " << g_display_layout.snapshot_id
                             << " monitors=" << g_display_layout.monitors.size();
    } else {
        VKR_WARN(kComponent) << "legacy sidecar handshake has no display layout; root-only "
                                "placement fallback remains active";
    }
    vkr::vkrpc::RpcChannel rpc(*conn);
    vkr::json::Value nresp;
    if (!send_rpc(rpc, vkr::sidecar::SidecarOp::Negotiate,
                  vkr::sidecar::SidecarNegotiateRequest{}.to_body(), nresp) ||
        !vkr::sidecar::SidecarNegotiateResponse::from_body(nresp).ok) {
        VKR_ERROR(kComponent) << "sidecar negotiate failed";
        return 3;
    }
    VKR_INFO(kComponent) << "connected to worker sidecar plane " << opts.connect;

    // 2) Connect to the private X display and claim the WM selection (SubstructureRedirect on the
    //    root; checked so "another WM owns it" is a hard error, not a silent loss).
    const char* disp = opts.display.empty() ? nullptr : opts.display.c_str();
    int screen_num = 0;
    xcb_connection_t* xc = xcb_connect(disp, &screen_num);
    if (xc == nullptr || xcb_connection_has_error(xc)) {
        VKR_ERROR(kComponent) << "cannot connect to the X display";
        return 4;
    }
    const xcb_setup_t* setup = xcb_get_setup(xc);
    xcb_screen_iterator_t it = xcb_setup_roots_iterator(setup);
    for (int i = 0; i < screen_num; i++) {
        xcb_screen_next(&it);
    }
    xcb_screen_t* screen = it.data;
    const xcb_window_t root = screen->root;
    // cache the immutable root size for the reverse-resize size clamp (defense-in-depth).
    g_root_w = screen->width_in_pixels;
    g_root_h = screen->height_in_pixels;
    if (g_has_display_layout &&
        (g_root_w != static_cast<std::uint32_t>(g_display_layout.virtual_bounds.width) ||
         g_root_h != static_cast<std::uint32_t>(g_display_layout.virtual_bounds.height))) {
        VKR_ERROR(kComponent) << "placement snapshot/root mismatch: snapshot="
                              << g_display_layout.virtual_bounds.width << "x"
                              << g_display_layout.virtual_bounds.height << " root=" << g_root_w
                              << "x" << g_root_h;
        xcb_disconnect(xc);
        return 4;
    }
    const uint32_t root_mask =
        XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY;
    xcb_void_cookie_t ck =
        xcb_change_window_attributes_checked(xc, root, XCB_CW_EVENT_MASK, &root_mask);
    xcb_generic_error_t* werr = xcb_request_check(xc, ck);
    if (werr != nullptr) {
        VKR_ERROR(kComponent) << "could not claim the WM selection (error " << int(werr->error_code)
                              << " -- another window manager on this display?)";
        free(werr);
        xcb_disconnect(xc);
        return 5;
    }

    // intern the ICCCM close atoms once (WM_DELETE_WINDOW via WM_PROTOCOLS) + carry the
    // boundary-smoke debug-inject flag into the input layer.
    g_debug_inject = opts.debug_inject;
    g_input_trace = std::getenv("VKRELAY2_INPUT_TRACE") != nullptr;
    // reverse-feedback smoke seam: parse "X,Y" (a move) or "X,Y,W,H" (a resize) ->
    // seed one GeometryRequest (one-shot). Split on commas (no sscanf/new include).
    if (!opts.debug_feedback_move.empty()) {
        std::vector<std::string> parts;
        std::string cur;
        for (const char ch : opts.debug_feedback_move) {
            if (ch == ',') {
                parts.push_back(cur);
                cur.clear();
            } else {
                cur.push_back(ch);
            }
        }
        parts.push_back(cur);
        if (parts.size() >= 2) {
            g_debug_feedback_x = std::atoi(parts[0].c_str());
            g_debug_feedback_y = std::atoi(parts[1].c_str());
            if (parts.size() >= 4) {
                g_debug_feedback_w = std::atoi(parts[2].c_str());
                g_debug_feedback_h = std::atoi(parts[3].c_str());
            }
            g_debug_feedback = true;
        }
    }
    g_debug_feedback_minimize = opts.debug_feedback_minimize; // iconify smoke seam
    g_debug_feedback_restore = opts.debug_feedback_restore;   // restore-leg smoke seam
    // GD bigroot smoke seam: parse "X,Y" -> seed one guest-root click (one-shot).
    if (!opts.debug_inject_click.empty()) {
        const std::size_t comma = opts.debug_inject_click.find(',');
        if (comma != std::string::npos && comma > 0 && comma + 1 < opts.debug_inject_click.size()) {
            g_debug_click_x = std::atoi(opts.debug_inject_click.substr(0, comma).c_str());
            g_debug_click_y = std::atoi(opts.debug_inject_click.substr(comma + 1).c_str());
            g_debug_click = true;
        }
    }
    {
        xcb_intern_atom_cookie_t cp = xcb_intern_atom(xc, 0, 12, "WM_PROTOCOLS");
        xcb_intern_atom_cookie_t cd = xcb_intern_atom(xc, 0, 16, "WM_DELETE_WINDOW");
        xcb_intern_atom_cookie_t cs = xcb_intern_atom(xc, 0, 8, "WM_STATE"); // ICCCM hygiene
        xcb_intern_atom_reply_t* rp = xcb_intern_atom_reply(xc, cp, nullptr);
        xcb_intern_atom_reply_t* rd = xcb_intern_atom_reply(xc, cd, nullptr);
        xcb_intern_atom_reply_t* rs = xcb_intern_atom_reply(xc, cs, nullptr);
        if (rp != nullptr) {
            g_wm_protocols = rp->atom;
            free(rp);
        }
        if (rd != nullptr) {
            g_wm_delete_window = rd->atom;
            free(rd);
        }
        if (rs != nullptr) {
            g_wm_state = rs->atom;
            free(rs);
        }
    }
    // intern the EWMH popup-classification atoms once. _NET_WM_WINDOW_TYPE carries a
    // priority-ordered list of type atoms; the sidecar represents an override-redirect window only
    // if it advertises one of the popup types below (or is untyped with a resolvable owner).
    // Batched: fire all the cookies, then read all the replies.
    {
        struct AtomReq {
            const char* name;
            xcb_atom_t* out;
        };
        const AtomReq reqs[] = {
            {"_NET_WM_WINDOW_TYPE", &g_net_wm_window_type},
            {"_NET_WM_WINDOW_TYPE_MENU", &g_net_wm_window_type_menu},
            {"_NET_WM_WINDOW_TYPE_POPUP_MENU", &g_net_wm_window_type_popup_menu},
            {"_NET_WM_WINDOW_TYPE_DROPDOWN_MENU", &g_net_wm_window_type_dropdown_menu},
            {"_NET_WM_WINDOW_TYPE_TOOLTIP", &g_net_wm_window_type_tooltip},
            {"_NET_WM_WINDOW_TYPE_COMBO", &g_net_wm_window_type_combo},
        };
        constexpr int kNumAtoms = static_cast<int>(sizeof(reqs) / sizeof(reqs[0]));
        xcb_intern_atom_cookie_t cookies[kNumAtoms];
        for (int i = 0; i < kNumAtoms; ++i) {
            cookies[i] = xcb_intern_atom(xc, 0, static_cast<std::uint16_t>(strlen(reqs[i].name)),
                                         reqs[i].name);
        }
        for (int i = 0; i < kNumAtoms; ++i) {
            xcb_intern_atom_reply_t* r = xcb_intern_atom_reply(xc, cookies[i], nullptr);
            if (r != nullptr) {
                *reqs[i].out = r->atom;
                free(r);
            }
        }
    }

    // 3) Probe required extension presence for optional chrome/input/cursor paths, then perform the
    // initial root scan (count current
    //    guest toplevels), then emit sidecar_ready to the worker.
    vkr::sidecar::SidecarReadyRequest ready;
    ready.scan_generation = 1;
    ready.has_xcomposite = has_extension(xc, "Composite");
    ready.has_xtest = has_extension(xc, "XTEST");
    ready.has_xfixes = has_extension(xc, "XFIXES");
    const bool has_shape = has_extension(xc, "SHAPE");
    // (cross-monitor maximize guard): report the OBSERVED X root so the worker caps the host
    // window client to the guest-realizable extent (the single source of truth -- not a worker-side
    // SPI_GETWORKAREA re-probe). This is the actual root weston created, whatever selected it
    // (VKRELAY2_GUEST_ROOT override, the pinned virtual bounds, legacy work area, or default).
    ready.root_width = screen->width_in_pixels;
    ready.root_height = screen->height_in_pixels;
    if (const char* snapshot_id = std::getenv("VKRELAY2_DISPLAY_SNAPSHOT_ID")) {
        ready.display_snapshot_id = snapshot_id;
    }
    // chrome capture is live only when compiled in AND XComposite is present. Set
    // BEFORE the scan so initial toplevels are captured too.
#ifdef VKRELAY2_HAVE_XCB_COMPOSITE
    g_chrome_capture = ready.has_xcomposite;
    if (g_chrome_capture) {
        VKR_INFO(kComponent) << "chrome capture enabled (XComposite present)";
    } else {
        VKR_WARN(kComponent)
            << "chrome capture compiled in but XComposite absent on this display; disabled";
    }
#else
    VKR_INFO(kComponent) << "chrome capture disabled (built without libxcb-composite)";
#endif
    // XShape is optional for ordinary rectangular windows but required to define pixels outside a
    // shaped window's BOUNDING region. Querying on each capture replaces ShapeNotify/cache state.
#ifdef VKRELAY2_HAVE_XCB_SHAPE
    g_shape_mask = g_chrome_capture && has_shape;
    if (g_shape_mask) {
        VKR_INFO(kComponent) << "shape masking enabled (SHAPE present)";
    } else if (g_chrome_capture) {
        VKR_WARN(kComponent) << "shape masking compiled in but SHAPE absent; shaped chrome will be "
                                "rectangular";
    }
#else
    if (g_chrome_capture && has_shape) {
        VKR_WARN(kComponent) << "SHAPE present but shape masking disabled (built without "
                                "libxcb-shape)";
    }
#endif
    // Diagnostic off-switches (env, inherited by the fork/exec'd sidecar): isolate
    // WHICH grab-blind path trips the guest Xwayland on a Blender view-rotate. Both keep
    // buttons/keys live (so the app's grab still happens): NO_XTEST_MOTION neutralizes every
    // absolute warp (warp_pointer); NO_CURSOR_CAPTURE disables XFixes cursor capture (folded into
    // the g_xfixes gate below). Read once, logged for the record so sidecar.log pins the run's
    // configuration.
    g_no_xtest_motion = (std::getenv("VKRELAY2_SIDECAR_NO_XTEST_MOTION") != nullptr);
    const bool no_cursor_capture = (std::getenv("VKRELAY2_SIDECAR_NO_CURSOR_CAPTURE") != nullptr);
    if (g_no_xtest_motion) {
        VKR_WARN(kComponent) << "diagnostic: XTEST absolute pointer warps DISABLED "
                                "(VKRELAY2_SIDECAR_NO_XTEST_MOTION); buttons/keys still inject";
    }
    if (no_cursor_capture) {
        VKR_WARN(kComponent) << "diagnostic: XFixes cursor capture DISABLED "
                                "(VKRELAY2_SIDECAR_NO_CURSOR_CAPTURE)";
    }
    // input injection is live only when compiled in (libxcb-xtest) AND XTEST is
    // present. Focus/close are core xcb and work regardless. Load the keymap once so key injection
    // maps to the server's LIVE keycodes.
#ifdef VKRELAY2_HAVE_XCB_XTEST
    g_xtest = ready.has_xtest;
    if (g_xtest) {
        load_keymap(xc, setup);
        VKR_INFO(kComponent) << "input injection enabled (XTEST present)";
    } else {
        VKR_WARN(kComponent) << "input injection compiled in but XTEST absent; pointer/key/wheel "
                                "disabled (focus/close still work)";
    }
#else
    VKR_INFO(kComponent) << "input injection disabled (built without libxcb-xtest); focus/close "
                            "still work";
#endif
    // cursor capture is live only when compiled in (libxcb-xfixes) AND XFixes is
    // present. XFixes must be version-negotiated before use; capture its event base so the loop can
    // recognize CursorNotify, and subscribe to display-cursor changes on the root.
#ifdef VKRELAY2_HAVE_XCB_XFIXES
    g_xfixes = ready.has_xfixes && !no_cursor_capture; // NO_CURSOR_CAPTURE forces the path off
    if (g_xfixes) {
        free(xcb_xfixes_query_version_reply(xc, xcb_xfixes_query_version(xc, 4, 0),
                                            nullptr)); // negotiate (required before use)
        xcb_query_extension_reply_t* qe =
            xcb_query_extension_reply(xc, xcb_query_extension(xc, 6, "XFIXES"), nullptr);
        if (qe != nullptr) {
            g_xfixes_event_base = qe->first_event;
            free(qe);
        }
        xcb_xfixes_select_cursor_input(xc, root, XCB_XFIXES_CURSOR_NOTIFY_MASK_DISPLAY_CURSOR);
        xcb_flush(xc);
        VKR_INFO(kComponent) << "cursor capture enabled (XFixes present)";
    } else {
        VKR_WARN(kComponent) << "cursor capture compiled in but XFixes absent; cursor disabled";
    }
#else
    VKR_INFO(kComponent) << "cursor capture disabled (built without libxcb-xfixes)";
#endif
    // Initial root scan: register every CURRENTLY-MAPPED, non-override_redirect child of the root
    // as a guest toplevel (the conservative classifier -- popups/menus/tooltips are
    // override_redirect and are skipped; unmapped children are not live toplevels). Windows mapped
    // later arrive as MapRequest in the event loop. Usually empty here (the app launches only after
    // readiness).
    xcb_query_tree_reply_t* tree = xcb_query_tree_reply(xc, xcb_query_tree(xc, root), nullptr);
    if (tree != nullptr) {
        const xcb_window_t* children = xcb_query_tree_children(tree);
        const int n = xcb_query_tree_children_length(tree);
        int registered = 0;
        for (int i = 0; i < n; ++i) {
            xcb_get_window_attributes_reply_t* a = xcb_get_window_attributes_reply(
                xc, xcb_get_window_attributes(xc, children[i]), nullptr);
            if (a == nullptr) {
                continue;
            }
            const bool override_redirect = a->override_redirect != 0;
            const bool viewable = a->map_state == XCB_MAP_STATE_VIEWABLE;
            free(a);
            if (override_redirect || !viewable) {
                continue;
            }
            // A reconnect/early-app window did not pass through our MapRequest path. Apply the
            // same snapshot placement/recovery before registering it so the worker never inherits
            // stale off-screen geometry from the scan.
            maybe_center_toplevel(xc, screen, children[i], /*fresh_map=*/false);
            forward_register(rpc, xc, children[i], false);
            debug_seed_input(rpc, children[i], g_tracked[children[i]].epoch);
            // Already mapped + rendered -> arm, then capture immediately (no Expose is coming for
            // it).
            cap_arm(xc, children[i]);
            cap_ship(rpc, xc, setup, screen, children[i]);
            ++registered;
        }
        ready.initial_toplevels = registered;
        free(tree);
    }
    vkr::json::Value rresp;
    if (!send_rpc(rpc, vkr::sidecar::SidecarOp::Ready, ready.to_body(), rresp) ||
        !vkr::sidecar::SidecarReadyResponse::from_body(rresp).ok) {
        VKR_ERROR(kComponent) << "worker rejected sidecar_ready";
        xcb_disconnect(xc);
        return 6;
    }
    VKR_INFO(kComponent) << "sidecar ready (xcomposite=" << (ready.has_xcomposite ? 1 : 0)
                         << " xtest=" << (ready.has_xtest ? 1 : 0)
                         << " xfixes=" << (ready.has_xfixes ? 1 : 0)
                         << " initial_toplevels=" << ready.initial_toplevels
                         << " guest-root=" << ready.root_width << "x" << ready.root_height
                         << " snapshot="
                         << (ready.display_snapshot_id.empty() ? "legacy"
                                                               : ready.display_snapshot_id)
                         << ")";

    // 4) Raise the readiness edge to the launcher -- ONLY after the worker acked sidecar_ready, so
    //    the launcher's app-launch gate is an observable end-to-end event, never log-scraping.
    //    Write a complete LINE (trailing newline) so a `read` on the other end returns success
    //    rather than seeing EOF-without-newline on the partial byte.
    if (opts.ready_fd >= 0) {
        const char line[] = "ready\n";
        const std::size_t n = sizeof(line) - 1;
        if (::write(opts.ready_fd, line, n) != static_cast<ssize_t>(n)) {
            VKR_WARN(kComponent) << "could not raise the --ready-fd edge";
        }
        ::close(opts.ready_fd);
    }

    // 5) Keep the WM alive as a TRANSPARENT pass-through WM. Holding SubstructureRedirect means
    //    guest MapRequest/ConfigureRequest are redirected to us and DO NOT take effect unless we
    //    apply them -- so a minimal correct WM must honor both, or ordinary toolkit
    //    placement/resize is silently denied the moment the sidecar starts. The sidecar honors the
    //    client's request verbatim; geometry AUTHORITY (the sidecar overriding/constraining
    //    geometry) and forwarding lifecycle to the worker registry happen here. Exits when
    //    the X connection drops or we are killed.
    const int xfd = xcb_get_file_descriptor(xc);
    for (;;) {
        // the loop is TIMER-DRIVEN. Drain every queued X event NON-BLOCKINGLY (never
        // return to a blocking xcb_wait_for_event, which would starve PollInput -- a
        // guardrail), then drain + inject the worker's input ring, then wait up to ~8 ms for the
        // next X event OR a timeout (then poll input again). So input latency ~ the poll interval,
        // with one connection + one thread + all RPCs sequential.
        xcb_generic_event_t* ev = nullptr;
        while ((ev = xcb_poll_for_event(xc)) != nullptr) {
            process_x_event(rpc, xc, setup, screen, ev);
            free(ev);
        }
        if (xcb_connection_has_error(xc)) {
            break; // X connection error / shutdown
        }
        drain_and_inject(rpc, xc, root);
        // keep non-GL placeholder chrome fresh -- recapture+reship the due placeholders
        // (warm-up burst then steady hashed poll). No-op unless chrome capture is active; bounded
        // by the policy.
        chrome_recapture_tick(rpc, xc, setup, screen, now_ms());
        xcb_flush(xc);
        struct pollfd pfd;
        pfd.fd = xfd;
        pfd.events = POLLIN;
        const int pr = poll(&pfd, 1, 8);
        if (pr < 0 && errno != EINTR) {
            VKR_WARN(kComponent) << "poll() on the X fd failed; sidecar exiting";
            break;
        }
    }
    VKR_INFO(kComponent) << "X connection closed; sidecar exiting";
    xcb_disconnect(xc);
    return 0;
}

int main(int argc, char** argv) {
    try {
        return run_sidecar(argc, argv);
    } catch (const vkr::transport::TransportError& e) {
        // The worker owns this private plane and may disappear first during authenticated session
        // teardown. That peer-close is an expected terminal edge for the sidecar, not a reason to
        // call std::terminate and leave a core dump. Keep unrelated exceptions fail-fast.
        VKR_WARN(kComponent) << "worker sidecar plane closed; exiting: " << e.what();
        return 7;
    }
}
