// vkrelay2 sidecar control plane.
//
// The sidecar (the X11 window-manager / geometry authority on the private display) talks to the
// worker over its OWN first-class control connection -- separate from the app/ICD Vulkan data
// plane. After a JSON SidecarHello/SidecarAck handshake (transport MessageChannel, token-gated,
// mirroring app_hello/app_ack), the connection switches to the binary RPC frame envelope
// (vkrpc::RpcChannel, REUSED) but carries the sidecar's OWN op space + dispatcher
// (serve_sidecar_rpc) and request structs -- NOT vkrpc::RpcOp / vkrpc::serve_vulkan_rpc. Session-
// mode separation is therefore STRUCTURAL: a Vulkan op never reaches the sidecar dispatcher (and
// vice versa); an unrecognized op is RpcStatus::UnknownOp, forward-compatibly.
//
// The plane carries a capability negotiate plus the worker-home toplevel-registry ops
// (register_toplevel / update_* / unregister_toplevel).
#ifndef VKRELAY2_COMMON_SIDECAR_SIDECAR_SESSION_HPP
#define VKRELAY2_COMMON_SIDECAR_SIDECAR_SESSION_HPP

#include "common/protocol/wire.hpp"
#include "common/util/json.hpp"
#include "common/vkrpc/rpc.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace vkr::sidecar {

// Sidecar control-plane protocol version (independent of the Vulkan RPC version).
constexpr int kSidecarProtocolVersion = 1;

// The sidecar op space. The sidecar has its own dispatcher (serve_sidecar_rpc), but it shares the
// RpcChannel frame header with the Vulkan plane, so the op NUMBERS must also be disjoint from
// vkrpc::RpcOp (1..62) -- otherwise a Vulkan opcode mis-sent to this plane would alias a real
// sidecar op instead of being rejected. Numbered from 200 to keep that gap; registry ops continue
// at 202 and above.
enum class SidecarOp : std::uint32_t {
    Invalid = 0,
    Negotiate = 200,
    Ready = 201, // the readiness barrier: emitted after WM-claim + caps + initial scan
    // Worker-home toplevel registry lifecycle. All carry a monotonic per-session
    // `generation`; the worker applies strictly-newer-wins and drops stale/equal ones.
    RegisterToplevel = 202,   // a guest toplevel mapped / seen in the initial scan
    UpdateToplevel = 203,     // its role/geometry changed (registry/debug state only)
    UnregisterToplevel = 204, // it unmapped / was destroyed
    // XComposite chrome pixels.
    PaintChrome = 205,      // BINARY-bodied: captured BGRA chrome painted into a placeholder HWND
    DebugChromeState = 206, // token-gated test query: a placeholder's representation/shown/seq + a
                            // sampled pixel (the worker-visible proof seam for the boundary smoke)
    // Input / focus / wheel / close. The worker captures Win32 input on its window and
    // QUEUES neutral events per guest XID; the sidecar (the RPC client) DRAINS them via PollInput
    // on a short timer and is the SOLE X-side injector (xcb_test_fake_input) / focus actor
    // (xcb_set_input_focus) / close requester (WM_DELETE_WINDOW). The ICD never touches input.
    PollInput = 207, // non-blocking drain of the worker's per-session input ring (since_seq cursor)
    // Cursor (the XFixes->Win32 reverse direction). The guest app changes its X cursor;
    // the sidecar (watching XFixes) ships the ARGB image + hotspot to the worker, which builds an
    // HCURSOR and applies it for the matching window's client-area WM_SETCURSOR.
    SetCursor = 208,         // BINARY-bodied: the guest's cursor image (BGRA8 + hotspot) for an xid
    DebugEnqueueInput = 209, // token-gated test seam: push a known event sequence into the worker's
                             // input ring (the worker-visible analogue of DebugChromeState) so the
                             // WSL boundary smoke drives sidecar->XTest->canary without real Win32
                             // input, which cannot originate on WSL.
    DebugCursorState = 210,  // token-gated test query: the worker's built cursor for an xid
                             // (dims/hotspot + a sampled pixel) -- the worker-visible cursor proof.
    // Screenshot / observability: enumerate the worker's window registry so a
    // capture/selection tool can name guest toplevels + their lifecycle. Pure registry-derived
    // state (mock == real); carries NO HWND (the per-window HWND is Win32-only + not in the common
    // registry -- the desktop helper correlates via the optional debug title tag).
    DebugEnumWindows = 211,
    // (screenshot/observability, THE GATE): capture a window's FULL source-layer BGRA
    // (the chrome DIB or the cursor image) so a Linux-side tool writes a PNG -- deterministic,
    // occlusion/minimize-proof, no desktop scraping. The RESPONSE is BINARY-bodied
    // ([u32 json_len][hdr-json][BGRA tail]); the request is plain JSON. Lifecycle-selective
    // and bounded by protocol::kMaxFrameBytes.
    DebugCaptureWindow = 212,
    // (show/hide lifecycle): set a registered toplevel's live VISIBILITY (the X
    // map/unmap state). Generation-tagged like the other lifecycle ops; the worker applies
    // strictly-newer-wins and ShowWindows the host (SW_HIDE / SW_SHOWNA). The KEY distinction from
    // UnregisterToplevel: a hide PRESERVES the representation (HWND/surface/chrome/epoch), so a
    // restore is free. Shares SidecarToplevelResponse with the register/update/unregister ops.
    SetToplevelVisibility = 213,
};

// Chrome aux-pixel transport caps + format. A source backing store may be larger than a
// single transport frame: PaintChrome carries a dirty rectangle, so the producer splits a large
// source into row bands. Keep those two bounds distinct. Conflating them used to let a root-sized
// capture pass the 96 MiB source check and then throw FrameError at RpcChannel's 16 MiB ceiling,
// killing the sidecar (and therefore input) while the Vulkan data plane kept rendering.
constexpr std::size_t kMaxAuxChromeBackingStoreBytes = 96u * 1024u * 1024u; // 96 MiB
constexpr std::size_t kMaxAuxChromeJsonHeaderBytes = 4096u;
constexpr std::size_t kMaxAuxChromeWirePayloadBytes =
    protocol::kMaxFrameBytes - kMaxAuxChromeJsonHeaderBytes - 1024u; // RPC + wire-header reserve
constexpr int kAuxChromeFormatBgra8 = 1; // VK-agnostic tag; the only format ships
constexpr int kAuxChromeBytesPerPixel = 4;

// Maximum number of full-width BGRA rows that fit in one PaintChrome RPC. Zero means even one row
// cannot fit. Pure/common so the producer's tiling invariant is directly unit-tested.
inline std::uint32_t max_aux_chrome_rows_per_wire(std::uint32_t width) {
    if (width == 0) {
        return 0;
    }
    const std::uint64_t row_bytes = static_cast<std::uint64_t>(width) * kAuxChromeBytesPerPixel;
    if (row_bytes > kMaxAuxChromeWirePayloadBytes) {
        return 0;
    }
    return static_cast<std::uint32_t>(kMaxAuxChromeWirePayloadBytes / row_bytes);
}

// Cursor transport caps + format. A cursor is small + bounded; the X core max cursor
// is implementation-defined but a generous 256x256 covers any realistic theme. BGRA8 only (X's
// uint32 ARGB cursor image is byte-order BGRA8 on a little-endian host, so it rides as the same
// byte format as chrome -- no per-channel conversion on the sidecar).
constexpr std::uint32_t kMaxCursorDim = 256;
constexpr std::size_t kMaxCursorPayloadBytes =
    static_cast<std::size_t>(kMaxCursorDim) * kMaxCursorDim * 4; // 256 KiB
constexpr std::size_t kMaxCursorJsonHeaderBytes = 1024u;
constexpr int kCursorFormatBgra8 = 1;

// Capture transport caps. Unlike PaintChrome's 96 MiB producer cap, a capture
// RESPONSE rides ONE RpcChannel frame, bounded by protocol::kMaxFrameBytes (16 MiB). So the
// BGRA tail must fit under that minus the json header + frame/RPC overhead. A
// capture whose source exceeds this returns a structured `too_large` result (with dims), never a
// transport failure; chunked capture is a deferred follow-up.
constexpr std::size_t kMaxCaptureJsonHeaderBytes = 4096u;
constexpr std::size_t kMaxCapturePayloadBytes =
    protocol::kMaxFrameBytes - kMaxCaptureJsonHeaderBytes - 1024u; // reserve json hdr + frame/RPC
constexpr int kCaptureFormatBgra8 = 1;
// The capture layer selector (which worker-owned source buffer to read).
constexpr char kCaptureLayerChrome[] = "chrome"; // the placeholder/chrome DIB
constexpr char kCaptureLayerCursor[] = "cursor"; // the built cursor image + hotspot

struct SidecarNegotiateRequest {
    int protocol_version = kSidecarProtocolVersion;

    json::Value to_body() const;
    static SidecarNegotiateRequest from_body(const json::Value& body);
};

struct SidecarNegotiateResponse {
    bool ok = false;
    int protocol_version = kSidecarProtocolVersion;
    std::string reason;

    json::Value to_body() const;
    static SidecarNegotiateResponse from_body(const json::Value& body);
};

// The readiness barrier. The sidecar emits this AFTER it owns the WM selection, has
// probed the required extensions, installed its event subscriptions, and completed its initial root
// scan -- never inferred from an external X11 probe. `scan_generation` is a monotonic id of that
// scan; the caps flags report extension presence for optional chrome, input, and cursor paths.
// `initial_toplevels` is the informational count from the scan; registration follows separately.
struct SidecarReadyRequest {
    int scan_generation = 0;
    bool has_xcomposite = false;
    bool has_xtest = false;
    bool has_xfixes = false;
    int initial_toplevels = 0;
    // (cross-monitor maximize guard): the sidecar's OBSERVED X root dimensions
    // (screen->width_in_pixels/height_in_pixels) at readiness -- the single source of truth for the
    // guest-realizable extent (the sidecar reports the actual root; the
    // worker caps against it rather than re-probing SPI_GETWORKAREA, which is only an approximation
    // and a SECOND authority). The worker bounds the host window client to this so a maximize on a
    // monitor LARGER than the session's guest root cannot drive the guest surface past its output
    // (the cross-monitor hang). 0 = not reported (older sidecar / mock) -> the worker keeps its
    // prior, uncapped behavior.
    std::uint32_t root_width = 0;
    std::uint32_t root_height = 0;
    // Exact immutable topology copied into the worker. Empty is legacy-only; the
    // worker requires equality before accepting the observed root dimensions.
    std::string display_snapshot_id;

    json::Value to_body() const;
    static SidecarReadyRequest from_body(const json::Value& body);
};

struct SidecarReadyResponse {
    bool ok = false;
    std::string reason;

    json::Value to_body() const;
    static SidecarReadyResponse from_body(const json::Value& body);
};

// --- Worker-home toplevel registry lifecycle
// ------------------------------------------
//
// The sidecar reports guest toplevels by XID (it does NOT know the worker's u64 VkSurfaceKHR --
// that is born-correlated on the app data plane via create_surface's xid). The worker joins these
// to any existing/future surface by XID and enforces one representation per toplevel.
// `generation` is a monotonic per-session lifecycle counter the sidecar stamps; the worker drops
// stale/equal ones. `xid`/`generation` ride as decimal-string u64 (the project's no-narrowing
// handle convention).

struct SidecarRegisterToplevelRequest {
    std::uint64_t xid = 0;
    std::uint64_t generation = 0;
    std::string role;  // sidecar-authored role (advisory; the ICD role_hint is separate)
    std::string title; // best-effort window title (debug)
    // Reported initial geometry for best-effort static placement; later updates provide live
    // authority.
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    // Classifier hint: an override_redirect window (popup/menu/tooltip). The sidecar CLASSIFIES
    // popups and forwards them with override_redirect=true plus the fields below; the worker gives
    // a classified popup a representation owned by its toplevel. An override_redirect window that
    // the sidecar does NOT classify as a popup is still refused by the worker
    // (override_redirect && !is_popup).
    bool override_redirect = false;
    // popup fields (0/false/None for an ordinary toplevel): `is_popup` marks a
    // classified override-redirect popup; `owner_xid` is the resolved live NON-popup owner toplevel
    // (the z-order anchor -- the worker owns the popup host to it); `popup_kind` is the EWMH window
    // type (a sidecar::PopupKind token; debug/obs/styling).
    bool is_popup = false;
    std::uint64_t owner_xid = 0;
    std::uint32_t popup_kind = 0;

    json::Value to_body() const;
    static SidecarRegisterToplevelRequest from_body(const json::Value& body);
};

struct SidecarUpdateToplevelRequest {
    std::uint64_t xid = 0;
    std::uint64_t generation = 0;
    std::string role; // empty = unchanged
    // The toplevel's CURRENT geometry (forward_update fills it from xcb_get_geometry on every
    // send). A real toplevel always has nonzero width/height; a DEGENERATE (zero w/h) geometry is
    // treated by the registry as "not provided" -- it keeps the prior geometry rather than
    // clobbering it to (0,0)/0x0 -- so a caller sending a pure restack/role-only update without
    // geometry is safe. Position 0,0 is valid (the size, not x/y, gates
    // "provided").
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    // a one-shot z-order intent (a sidecar::ZOrder value: 0 None / 1 Raise / 2 Lower).
    // Raise = bring the host to the top of the non-topmost stack (raise-active / an X Above
    // request), Lower = send it to the bottom; None = no z-order change. Generation-gated like the
    // geometry.
    std::uint32_t z_order = 0;

    json::Value to_body() const;
    static SidecarUpdateToplevelRequest from_body(const json::Value& body);
};

struct SidecarUnregisterToplevelRequest {
    std::uint64_t xid = 0;
    std::uint64_t generation = 0;

    json::Value to_body() const;
    static SidecarUnregisterToplevelRequest from_body(const json::Value& body);
};

// (show/hide lifecycle): set a registered toplevel's live visibility.
// `visibility_state` is a sidecar::VisibilityState value (0 Visible / 1 Hidden / 2 Iconic). The
// sidecar sends Visible/Hidden, while host-user minimization can also author Iconic. It is
// generation-tagged and strictly-newer-wins like the other lifecycle ops. Shares
// SidecarToplevelResponse.
struct SidecarSetVisibilityRequest {
    std::uint64_t xid = 0;
    std::uint64_t generation = 0;
    std::uint32_t visibility_state = 0;

    json::Value to_body() const;
    static SidecarSetVisibilityRequest from_body(const json::Value& body);
};

// Shared response for the three toplevel ops. `applied` is false when the op was dropped as a
// stale/equal generation (not an error -- the worker is idempotent); `representation` reports the
// joined host representation after the op ("none"|"placeholder"|"surface") for observability/tests.
struct SidecarToplevelResponse {
    bool ok = false;
    std::string reason;
    std::uint64_t xid = 0;
    bool applied = false;
    std::string representation = "none";
    // the xid's current representation epoch after the op (0 if it has no live entry).
    // The sidecar carries it so the boundary smoke can seed input at the worker's live epoch; in
    // production the worker stamps captured events with it directly (the sidecar just observes).
    std::uint64_t epoch = 0;

    json::Value to_body() const;
    static SidecarToplevelResponse from_body(const json::Value& body);
};

// --- Chrome pixels: the auxiliary-pixel transport
// -------------------------------------
//
// BINARY-bodied (like CreateShaderModule): the wire is [u32 json_len LE][json header][raw BGRA
// tail]. The JSON header carries the topology/metadata; the tail is the captured chrome for the
// dirty rect. `from_wire` is strict-but-total -- it enforces the full decoder discipline (caps,
// wide-arithmetic bounds, stride/payload consistency, exact tail length, BGRA8-only) and returns a
// default-constructed request + sets `err` on ANY framing fault, so a malformed body never reaches
// the GDI paint path.
struct SidecarPaintChromeRequest {
    std::uint64_t xid = 0;
    std::uint64_t lifecycle_generation = 0; // must EXACTLY match the registry's current generation
    std::uint64_t seq = 0;                  // per-toplevel monotonic; strictly-newer-wins
    std::uint32_t src_w = 0;                // full captured source size (the placeholder's content)
    std::uint32_t src_h = 0;
    std::int32_t dirty_x = 0; // the dirty rect within [0,src_w) x [0,src_h)
    std::int32_t dirty_y = 0;
    std::uint32_t dirty_w = 0;
    std::uint32_t dirty_h = 0;
    std::uint32_t stride = 0; // bytes per row of `pixels` (>= dirty_w * 4)
    int format = kAuxChromeFormatBgra8;
    std::string pixels; // raw tail: `stride * dirty_h` bytes of BGRA8

    std::string to_wire() const;
    static SidecarPaintChromeRequest from_wire(const std::string& body, std::string& err);
};

// PaintChrome response: `applied` is the registry's accept decision (false = dropped: not a
// Placeholder, stale generation, or non-newer seq -- the sidecar can stop capturing a no-longer-
// Placeholder xid); `shown`/`last_seq` reflect the realized paint state after commit.
struct SidecarPaintResponse {
    bool ok = false;
    std::string reason;
    std::uint64_t xid = 0;
    bool applied = false;
    std::string representation = "none";
    bool shown = false;
    std::uint64_t last_seq = 0;

    json::Value to_body() const;
    static SidecarPaintResponse from_body(const json::Value& body);
};

// DebugChromeState (token-gated test query): the worker-visible proof seam. Returns a
// placeholder's representation/shown/last_seq + the BGRA value the worker last PAINTED at
// (sample_x, sample_y) -- so the boundary smoke proves the DIB content from outside the worker,
// never by scraping a sidecar log.
struct SidecarDebugChromeStateRequest {
    std::uint64_t xid = 0;
    std::int32_t sample_x = 0;
    std::int32_t sample_y = 0;

    json::Value to_body() const;
    static SidecarDebugChromeStateRequest from_body(const json::Value& body);
};

struct SidecarDebugChromeStateResponse {
    bool ok = false;
    std::string reason;
    std::uint64_t xid = 0;
    std::string representation = "none";
    bool shown = false;
    std::uint64_t last_seq = 0;
    bool has_pixel = false;       // false if (sample_x,sample_y) is out of bounds / nothing painted
    std::uint32_t pixel_bgra = 0; // packed 0xAARRGGBB-style: (B) | (G<<8) | (R<<16) | (A<<24)

    json::Value to_body() const;
    static SidecarDebugChromeStateResponse from_body(const json::Value& body);
};

// --- Input transport: neutral events worker->sidecar
// ----------------------------
//
// The worker translates each Win32 input message into a NEUTRAL event (no X keycodes -- the Windows
// worker does not own the X keymap; the sidecar maps `vk` to an X keysym/keycode via the live
// keymap). Coordinates are window-relative CLIENT pixels (physical px; the window thread is
// PMv1-aware); the sidecar maps to X root coords (root = guest_origin + client) and warps the
// pointer immediately before a coord-bearing button/wheel so a coalesced/dropped motion never lands
// the action stale.

// Neutral key-modifier bitmask (worker-captured; the sidecar applies them via fake key state). VK-
// and X-agnostic flags, so the mock (no Win32 / no X) agrees with both ends.
constexpr std::uint32_t kInputModShift = 0x1;
constexpr std::uint32_t kInputModCtrl = 0x2;
constexpr std::uint32_t kInputModAlt = 0x4;

// Neutral pointer-button numbering (worker-captured; the sidecar maps to X button numbers, where
// 1=left 2=middle 3=right). Wheel is a SEPARATE kind (mapped to X Button4/Button5), never a button.
constexpr int kInputButtonLeft = 1;
constexpr int kInputButtonRight = 2;
constexpr int kInputButtonMiddle = 3;

// The ring + per-poll caps (bounded transport: the worker drops disposable motion first on
// overflow, and a single PollInput response is capped so it is always small regardless of a
// backlog).
constexpr std::size_t kMaxInputQueueEvents = 4096; // the per-session input ring SOFT cap
constexpr std::size_t kMaxPollInputEvents = 256;   // max events returned by one PollInput
// the ABSOLUTE hard ceiling. Between the soft cap and this, a PROTECTED (non-Motion)
// event is admitted past the soft cap rather than silently dropped when no disposable Motion can be
// evicted (a never-silent overflow). Only at this ceiling -- a pathological non-draining sidecar --
// is a protected event last-resort dropped (always with the `dropped` diagnostic set). Generous:
// real host events are human-paced + drained every poll tick, so this is never reached in practice.
constexpr std::size_t kMaxInputQueueHardCeiling = kMaxInputQueueEvents + 1024;

// Neutral worker->sidecar HOST-EVENT kind, covering input plus host-window events:
// the worker reports what the user did to the host window -- input, close, AND geometry/lifecycle
// requests). Numeric + wire-stable; an unknown kind on the wire decodes to Invalid and is dropped.
// All kinds EXCEPT Motion are NON-DISPOSABLE (the queue never drops them to make room -- a flood of
// disposable Motion can never starve a click/key/close/geometry/lifecycle intent).
enum class InputEventKind : std::uint32_t {
    Invalid = 0,
    Motion = 1, // pointer motion to (client_x, client_y) -- the ONLY disposable kind
    Button = 2, // pointer button press/release (`button`, pressed)
    Wheel = 3,  // vertical wheel notch at (client_x, client_y) (`wheel` = +1 up / -1 down)
    Key = 4,    // keyboard key press/release (`vk`/`scancode`/`modifiers`, pressed)
    Focus = 5,  // worker window gained (pressed=true) / lost (pressed=false) focus
    Close = 6,  // a WM_CLOSE on the worker window: ask the guest to close (WM_DELETE_WINDOW)
    // a Win32-user-origin geometry/lifecycle REQUEST (the reverse direction -- the
    // user moved/resized/minimized/restored the host window). Carries root_x/root_y (+ req_w/req_h)
    // + host_request below. Generation-LESS: it is a REQUEST; the sidecar authors the guest change
    // + mints the generation. The worker forwards it COMMIT-ONLY (on WM_EXITSIZEMOVE), so the echo
    // is idempotent.
    GeometryRequest = 7,
};

// the sub-kind of a GeometryRequest host event (which user gesture). `Geometry`
// carries the requested root_x/root_y client origin (+ req_w/req_h for a resize); Minimize/Restore
// carry no rect. Wire-stable; an unknown value decodes to Geometry.
enum class HostRequestKind : std::uint32_t {
    Geometry = 0, // a committed user move and/or resize -> root_x/root_y (+ req_w/req_h)
    Minimize = 1, // a user minimize (the minimize box / SC_MINIMIZE) -> the guest is iconified
    Restore = 2,  // a user restore (taskbar / SC_RESTORE) -> the guest is de-iconified/mapped
};

// One neutral input event. Carries the producing representation's `epoch` (the worker's exact-epoch
// stale gate -- see WindowRegistry::epoch_for_xid) + a per-session monotonic `seq` (the PollInput
// cursor). Rides in a PollInput/DebugEnqueueInput JSON array via to_value/from_value (xid/epoch/seq
// as decimal-string u64; the rest as ints/bool, tolerant-decoded).
struct SidecarInputEvent {
    std::uint64_t xid = 0;
    std::uint64_t epoch = 0;
    std::uint64_t seq = 0;
    std::uint32_t kind = 0; // InputEventKind
    std::int32_t client_x = 0;
    std::int32_t client_y = 0;
    int button = 0;              // Button: kInputButton*
    int wheel = 0;               // Wheel: +1 up / -1 down
    int vk = 0;                  // Key: Win32 virtual-key code (neutral; mapped on the sidecar)
    int scancode = 0;            // Key: Win32 scancode (diagnostic)
    std::uint32_t modifiers = 0; // kInputMod* bitmask
    bool pressed = false;        // Button/Key: down vs up; Focus: gained vs lost
    // GeometryRequest fields (0 for every other kind). EXPLICIT + honestly named (NOT
    // reusing client_x/client_y, which are Win32 client-LOCAL input px -- root_x/root_y are X-ROOT
    // CLIENT coords, the same space the sidecar authors): root_x/root_y is the requested host
    // CLIENT origin in X-root coords; req_w/req_h the requested CLIENT extent (0 for a pure move);
    // host_request is a HostRequestKind (Geometry/Minimize/Restore).
    std::int32_t root_x = 0;
    std::int32_t root_y = 0;
    std::uint32_t req_w = 0;
    std::uint32_t req_h = 0;
    std::uint32_t host_request = 0; // HostRequestKind (GeometryRequest only)

    json::Value to_value() const;
    static SidecarInputEvent from_value(const json::Value& v);
};

// PollInput (op 207): NON-BLOCKING. The worker returns queued events with seq > since_seq (up to
// kMaxPollInputEvents) and `next_seq` (the new cursor the sidecar passes next poll). `dropped` is
// true when an overflow discarded disposable motion since the last poll (diagnostic only).
struct SidecarPollInputRequest {
    std::uint64_t since_seq = 0;

    json::Value to_body() const;
    static SidecarPollInputRequest from_body(const json::Value& body);
};

struct SidecarPollInputResponse {
    bool ok = false;
    std::string reason;
    std::vector<SidecarInputEvent> events;
    std::uint64_t next_seq = 0;
    bool dropped = false;

    json::Value to_body() const;
    static SidecarPollInputResponse from_body(const json::Value& body);
};

// DebugEnqueueInput (op 209, token-gated test seam): push a known sequence into the worker's input
// ring for (xid, epoch), exactly as the WndProc would on real Win32 input -- the worker-visible way
// to drive sidecar->XTest->canary on WSL (where real Win32 input cannot originate). The boundary
// smoke passes the worker's CURRENT epoch (learned from register_toplevel's response) so the seeded
// events pass the exact-epoch gate. The carried per-event seq is ignored (the worker re-stamps a
// fresh session seq); xid/epoch on the request are authoritative (the per-event ones are ignored).
struct SidecarDebugEnqueueInputRequest {
    std::uint64_t xid = 0;
    std::uint64_t epoch = 0;
    std::vector<SidecarInputEvent> events;

    json::Value to_body() const;
    static SidecarDebugEnqueueInputRequest from_body(const json::Value& body);
};

struct SidecarDebugEnqueueInputResponse {
    bool ok = false;
    std::string reason;
    std::uint64_t xid = 0;
    int enqueued = 0; // how many events were accepted into the ring

    json::Value to_body() const;
    static SidecarDebugEnqueueInputResponse from_body(const json::Value& body);
};

// --- Cursor: the guest cursor image worker<-sidecar (the XFixes->Win32 direction)
// ----
//
// BINARY-bodied (like PaintChrome): `[u32 json_len LE][json header][raw BGRA tail]`. The header
// carries {xid, width, height, xhot, yhot, format, payload_size}; the tail is `width*height*4`
// bytes of BGRA8 (X's ARGB cursor image is byte-order BGRA8 on a little-endian host). `from_wire`
// is strict-but-total (caps + exact tail) and returns a default request + `err` on ANY framing
// fault, so a malformed body never reaches the HCURSOR build.
struct SidecarSetCursorRequest {
    std::uint64_t xid = 0;
    std::uint64_t epoch = 0; // the representation epoch (the exact-epoch gate)
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::int32_t xhot = 0; // hotspot within [0,width) x [0,height)
    std::int32_t yhot = 0;
    int format = kCursorFormatBgra8;
    std::string pixels; // width*height*4 bytes of BGRA8

    std::string to_wire() const;
    static SidecarSetCursorRequest from_wire(const std::string& body, std::string& err);
};

struct SidecarSetCursorResponse {
    bool ok = false;
    std::string reason;
    std::uint64_t xid = 0;
    bool applied = false; // true iff the worker built + bound an HCURSOR for the xid's window

    json::Value to_body() const;
    static SidecarSetCursorResponse from_body(const json::Value& body);
};

// DebugCursorState (token-gated test query, the worker-visible cursor proof seam): the cursor the
// worker last built for an xid -- its dims/hotspot + the BGRA the cursor holds at (sample_x,
// sample_y). So the boundary smoke proves the built HCURSOR from outside the worker.
struct SidecarDebugCursorStateRequest {
    std::uint64_t xid = 0;
    std::int32_t sample_x = 0;
    std::int32_t sample_y = 0;

    json::Value to_body() const;
    static SidecarDebugCursorStateRequest from_body(const json::Value& body);
};

struct SidecarDebugCursorStateResponse {
    bool ok = false;
    std::string reason;
    std::uint64_t xid = 0;
    bool has_cursor = false; // the worker has built + bound a cursor for this xid's window
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::int32_t xhot = 0;
    std::int32_t yhot = 0;
    bool has_pixel = false;       // false if (sample_x,sample_y) is out of bounds / no cursor
    std::uint32_t pixel_bgra = 0; // packed (B)|(G<<8)|(R<<16)|(A<<24)

    json::Value to_body() const;
    static SidecarDebugCursorStateResponse from_body(const json::Value& body);
};

// --- Observability: enumerate the window registry
// -------------------------------------
//
// DebugEnumWindows (op 211, token-gated): a registry-derived snapshot so a capture/selection tool
// can name guest toplevels and their lifecycle selectors. Pure registry state -> mock == real. The
// request has no fields (enumerate all); the response lists one SidecarWindowInfo per entry, sorted
// by xid. The lifecycle fields (generation / epoch / last_paint_seq) are exactly the selectors
// used by the capture gate, making stale artifacts apparent in the metadata.

struct SidecarDebugEnumWindowsRequest {
    // (live geometry proof): when true, the worker ALSO fills each entry's ACTUAL host
    // geometry (the actual_*/frame_*/last_host_apply_seq/clamped fields below). This makes the enum
    // do an off-lock window-thread read per host; the default (false)
    // keeps the pure-registry, no-invoke query.
    bool include_actual = false;

    json::Value to_body() const;
    static SidecarDebugEnumWindowsRequest from_body(const json::Value& body);
};

// One enumerated window. Registry identity + lifecycle selectors;
// xid/generation/epoch/last_paint_seq ride as decimal-string u64 (the no-narrowing handle
// convention). `representation` is the stable "none"|"placeholder"|"surface" name; `has_surface` is
// whether a VkSurfaceKHR is bound; `shown` is whether a placeholder has been painted+shown.
// Geometry is the sidecar-reported placement, updated whenever the sidecar authors a live change.
struct SidecarWindowInfo {
    std::uint64_t xid = 0;
    std::string representation = "none";
    bool toplevel_registered = false;
    bool has_surface = false;
    std::uint64_t generation = 0;
    std::uint64_t epoch = 0; // representation epoch (the cursor/surface lifecycle selector)
    std::uint64_t last_paint_seq = 0; // the chrome lifecycle selector
    bool shown = false;
    std::string role;
    std::string title;
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    // popup fields (the worker-visible proof for owner/z-order; 0/false/None for an
    // ordinary toplevel).
    bool is_popup = false;
    std::uint64_t owner_xid = 0;
    std::uint32_t popup_kind = 0;
    // the last z-order intent the sidecar authored for this toplevel (a sidecar::ZOrder
    // value: 0 None / 1 Raise / 2 Lower; sticky -- the most recent restack). Pure registry state,
    // so mock == real; the worker-visible record that a raise/lower was applied (the actual HWND
    // z-position is asserted in-process by integration_real_backend via GetWindow).
    std::uint32_t z_order = 0;
    // the toplevel's authored live VISIBILITY (a sidecar::VisibilityState value: 0
    // Visible / 1 Hidden / 2 Iconic). Pure registry state (mock == real), ALWAYS reported -- the
    // worker-visible record of a hide/show. The host-OBSERVED visibility (host_visible/host_iconic
    // below) is reported only with include_actual.
    std::uint32_t visibility_state = 0;
    // ACTUAL host geometry (filled ONLY when the request set include_actual; otherwise
    // has_actual=false + zeros). The hard convergence gate compares actual_x/actual_y -- the host
    // window's CLIENT origin mapped back to X-root coords -- to the authored x/y above. The real
    // backend reads these off the window thread (GetWindowRect/GetClientRect/ClientToScreen); the
    // mock reports its last-APPLIED move geometry (it has no HWND -- mock == real for the authored
    // position + seq, the only thing a move changes). frame_* is the Win32 OUTER window rect
    // in SCREEN coords (real only; 0 on the mock). actual_width/height is the host CLIENT extent
    // (real: GetClientRect; the mock leaves it 0 -- a move is position-only, while a resize changes
    // the extent).
    // last_host_apply_seq is the highest applied GEOMETRY seq (the geometry convergence proof) --
    // it is deliberately NOT the max over visibility applies: geometry and
    // visibility have SEPARATE per-kind apply gates (a shared one would let a coalesced later move
    // drop an intermediate hide), and the visibility convergence proof is host_visible below, not a
    // seq. So despite the general field name, this reports the geometry gate. clamped is true iff
    // the achieved origin differs from the authored one (Win32 repositioned it).
    bool has_actual = false;
    std::int32_t actual_x = 0;
    std::int32_t actual_y = 0;
    std::uint32_t actual_width = 0;
    std::uint32_t actual_height = 0;
    std::int32_t frame_x = 0;
    std::int32_t frame_y = 0;
    std::uint32_t frame_width = 0;
    std::uint32_t frame_height = 0;
    std::uint64_t last_host_apply_seq = 0;
    bool clamped = false;
    // the host-OBSERVED visibility (include_actual only; real backend reads it off the
    // window thread via IsWindowVisible/IsIconic). The hard gate asserts host_visible tracks the
    // authored visibility_state across a hide/show. host_iconic is reported now but only becomes
    // true for a host-user minimize. The mock has no HWND -- it mirrors the authored
    // state (host_visible = (visibility_state == Visible)), so mock == real for the Visible/Hidden
    // decision authors. NOTE: once Iconic IS authored, align the
    // mock with real Win32 -- a MINIMIZED window is still IsWindowVisible==true AND IsIconic==true,
    // so the mock's Iconic case must report host_visible=true (not false) + host_iconic=true to
    // stay mock == real. This distinction is harmless for callers that never author Iconic.
    bool host_visible = false;
    bool host_iconic = false;

    json::Value to_value() const;
    static SidecarWindowInfo from_value(const json::Value& v);
};

struct SidecarDebugEnumWindowsResponse {
    bool ok = false;
    std::string reason;
    std::vector<SidecarWindowInfo> windows; // sorted by xid

    json::Value to_body() const;
    static SidecarDebugEnumWindowsResponse from_body(const json::Value& body);
};

// --- Source-layer capture (THE GATE)
// -----------------------------------------------------
//
// DebugCaptureWindow (op 212, token-gated): return a window's FULL source-layer BGRA so a
// Linux-side tool writes a PNG. The REQUEST is plain JSON {xid, layer, optional lifecycle
// selectors}; the RESPONSE is BINARY-bodied ([u32 json_len LE][hdr-json][BGRA tail]) -- the tail is
// the captured pixels, present ONLY on a successful (status == "ok") capture; every non-OK status
// carries the structured hdr-json with an EMPTY tail (one decoder path, the hdr-json is the
// machine-readable part). `from_wire` is strict-but-total.
//
// Lifecycle-selective: the optional `expected_*` selectors let a caller pin
// the capture to a specific lifecycle so a re-registered XID's NEXT lifecycle is not captured by
// mistake; 0 means "do not check". The response ALWAYS carries the ACTUAL
// generation/epoch/last_paint_seq so a stale artifact is spottable even without a selector.

struct SidecarDebugCaptureWindowRequest {
    std::uint64_t xid = 0;
    std::string layer;                               // kCaptureLayerChrome | kCaptureLayerCursor
    std::uint64_t expected_epoch = 0;                // 0 = do not check (cursor/surface lifecycle)
    std::uint64_t expected_lifecycle_generation = 0; // 0 = do not check (chrome lifecycle)
    std::uint64_t min_last_seq = 0;                  // 0 = no minimum (chrome paint progress)

    json::Value to_body() const;
    static SidecarDebugCaptureWindowRequest from_body(const json::Value& body);
};

// Capture result. `ok` is true iff pixels are present (status == "ok"); otherwise `status` is one
// of "empty" (the window exists but the layer has no content), "absent" (no such
// window/representation), "mismatch" (a lifecycle selector did not match), "too_large" (source
// exceeds the frame cap -- `needed_bytes` reports the source size), or "bad_layer" (unknown layer
// string). Metadata (representation/generation/epoch/last_paint_seq/shown, and for cursor the
// hotspot) is filled whenever the window exists. `pixels` is the top-down BGRA8 tail, `stride`
// bytes per row.
struct SidecarDebugCaptureWindowResponse {
    bool ok = false;
    std::string status; // "ok"|"empty"|"absent"|"mismatch"|"too_large"|"bad_layer"
    std::string reason;
    std::uint64_t xid = 0;
    std::string layer;
    std::string representation = "none";
    std::uint64_t generation = 0;
    std::uint64_t epoch = 0;
    std::uint64_t last_paint_seq = 0;
    bool shown = false;    // chrome: the placeholder has been painted + shown
    std::int32_t xhot = 0; // cursor hotspot
    std::int32_t yhot = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t stride = 0; // bytes per row of `pixels` (== width*4 for our tight buffers)
    int format = kCaptureFormatBgra8;
    std::uint64_t needed_bytes = 0; // too_large: the source byte size that exceeded the cap
    std::string pixels; // BGRA8 tail: `stride * height` bytes (only when status == "ok")

    std::string to_wire() const;
    static SidecarDebugCaptureWindowResponse from_wire(const std::string& body, std::string& err);
};

// The worker-side sidecar surface. The session-owned backend (the worker's RealVulkanBackend, and
// the mock) implements this ALONGSIDE vkrpc::VulkanBackend, so a single object owns the Vulkan
// surface map AND the window registry -- the enforcement point. Keeping it a
// distinct, tiny interface keeps the Vulkan and sidecar op spaces separate while the concrete
// session object joins them.
class SidecarBackend {
  public:
    virtual ~SidecarBackend() = default;
    virtual SidecarNegotiateResponse negotiate(const SidecarNegotiateRequest& req) = 0;
    // Record the readiness barrier. The worker stores it; the launcher gates app
    // launch on the sidecar's own --ready-fd edge (raised only after this is acked), never on an
    // X11 probe.
    virtual SidecarReadyResponse sidecar_ready(const SidecarReadyRequest& req) = 0;
    // Worker-home toplevel registry lifecycle. XID-keyed (the sidecar never knows the
    // worker's u64 surface handle); the worker joins to surfaces by XID and enforces. All three
    // are generation-tagged and strictly-newer-wins, so a stale/equal op is dropped (ok=true,
    // applied=false) rather than failed. Mutations of the shared registry/HWND state run under the
    // backend mutex; HWND ops marshal onto the window thread off-lock.
    virtual SidecarToplevelResponse
    register_toplevel(const SidecarRegisterToplevelRequest& req) = 0;
    virtual SidecarToplevelResponse update_toplevel(const SidecarUpdateToplevelRequest& req) = 0;
    virtual SidecarToplevelResponse
    unregister_toplevel(const SidecarUnregisterToplevelRequest& req) = 0;
    // (show/hide lifecycle): set a registered toplevel's live visibility (X map/unmap).
    // Generation-tagged + strictly-newer-wins like the other lifecycle ops; a hide PRESERVES the
    // representation (the HWND/surface/chrome/epoch survive) so a restore is free. Shares
    // SidecarToplevelResponse.
    virtual SidecarToplevelResponse set_visibility(const SidecarSetVisibilityRequest& req) = 0;
    // Chrome pixels. paint_chrome runs the accept -> (window-thread paint) -> commit
    // dance; debug_chrome_state answers the worker-visible test query (representation/shown/seq + a
    // pixel sample). Both gate on the same registry state; neither moves pixels through the common
    // registry.
    virtual SidecarPaintResponse paint_chrome(const SidecarPaintChromeRequest& req) = 0;
    virtual SidecarDebugChromeStateResponse
    debug_chrome_state(const SidecarDebugChromeStateRequest& req) = 0;
    // Input plane. poll_input NON-BLOCKINGLY drains the worker's per-session input
    // ring (the WndProc fills it on the window thread; this runs on the sidecar plane thread) under
    // a small DEDICATED input mutex -- never the backend mutex -- and drops events whose xid no
    // longer has a live registry representation (the input-side staleness gate; see the .cpp).
    // debug_- enqueue_input is the token-gated test producer that stands in for the WndProc on WSL.
    virtual SidecarPollInputResponse poll_input(const SidecarPollInputRequest& req) = 0;
    virtual SidecarDebugEnqueueInputResponse
    debug_enqueue_input(const SidecarDebugEnqueueInputRequest& req) = 0;
    // Cursor plane: set_cursor builds an HCURSOR from the guest's BGRA cursor image
    // and binds it to the xid's window (applied for client-area WM_SETCURSOR; old cursors retired
    // safely); debug_cursor_state answers the worker-visible cursor proof query. Both find the
    // xid's window via the same surface/placeholder tables as the rest of the sidecar plane.
    virtual SidecarSetCursorResponse set_cursor(const SidecarSetCursorRequest& req) = 0;
    virtual SidecarDebugCursorStateResponse
    debug_cursor_state(const SidecarDebugCursorStateRequest& req) = 0;
    // Observability: a registry-derived snapshot of every guest toplevel + its
    // lifecycle. Pure registry read under the backend mutex; mock == real.
    virtual SidecarDebugEnumWindowsResponse
    debug_enum_windows(const SidecarDebugEnumWindowsRequest& req) = 0;
    // Observability (THE GATE): capture a window's full source-layer BGRA (chrome DIB
    // or cursor) for PNG export. Checks the lifecycle selectors + the frame cap; copies the
    // worker-owned buffer (real: off the window thread; mock: its synthetic store) -- mock == real.
    virtual SidecarDebugCaptureWindowResponse
    debug_capture_window(const SidecarDebugCaptureWindowRequest& req) = 0;
};

// Serves sidecar RPCs on `channel` until the sidecar disconnects (mirrors vkrpc::serve_vulkan_rpc,
// over the sidecar op space). Reuses the binary RPC frame codec; never throws on a clean EOF.
void serve_sidecar_rpc(vkrpc::RpcChannel& channel, SidecarBackend& backend);

} // namespace vkr::sidecar

#endif // VKRELAY2_COMMON_SIDECAR_SIDECAR_SESSION_HPP
