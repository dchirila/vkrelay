// One dedicated Win32 window thread and message pump for the worker, plus the per-window
// present/geometry state: a slot lock and a geometry-dirty latch.
//
// Every per-app HWND lives on a single window thread that runs the message pump.
// Window creation/destruction AND the HWND-touching WSI driver calls
// (create/acquire/present/destroy swapchain) are marshaled onto that thread via
// invoke(), so the host driver always sees the window the way a normal Win32 Vulkan
// app's UI thread presents it -- never racing window messages on another thread (the
// original's hardest crash class, an Optimus present-vs-reslot access violation).
//
// Each window carries a WindowSlot (mirrors the original's per-WindowHandle state):
//   - a per-surface slot lock: present/acquire take it blocking on the session
//     thread; future geometry/slot mutations on the pump thread try_lock + re-post so
//     the pump never blocks on the session thread (which may be mid-invoke to it);
//   - a geometry-dirty latch: WM_SIZE marks the slot dirty when the client size
//     no longer matches the active swapchain extent; while dirty the backend returns
//     VK_ERROR_OUT_OF_DATE_KHR without calling the driver; a fresh swapchain clears it.
// The latch + lock live here (atomic + mutex) so the session thread and the pump
// thread share them without racing the backend's handle tables.
//
// Windows-only; built only with the Vulkan SDK (see CMakeLists), alongside the real
// backend.
#ifndef VKRELAY2_WINDOWS_WORKER_WINDOWING_WINDOW_THREAD_HPP
#define VKRELAY2_WINDOWS_WORKER_WINDOWING_WINDOW_THREAD_HPP

#include "common/display/display_layout.hpp"
#include "common/sidecar/input_queue.hpp"
#include "common/sidecar/window_registry.hpp" // sidecar::ZOrder restack intent

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>

namespace vkr::worker::windowing {

// Per-window present/geometry state shared between the window thread (WndProc) and the
// session thread (WSI calls). Owned by WindowThread; lives until the window is
// destroyed. Non-copyable/movable (holds a mutex + atomics); the backend holds a stable
// raw pointer to it for the surface's lifetime.
class WindowSlot {
  public:
    explicit WindowSlot(const display::DisplayLayout* display_layout = nullptr,
                        std::atomic<bool>* display_restart_required = nullptr)
        : display_layout_(display_layout), display_restart_required_(display_restart_required) {}
    const display::DisplayLayout* display_layout() const { return display_layout_; }
    void note_display_configuration_change(const char* reason);
    ~WindowSlot(); // frees the cursor HCURSOR, if any
    WindowSlot(const WindowSlot&) = delete;
    WindowSlot& operator=(const WindowSlot&) = delete;

    // the present/geometry slot lock. The session thread locks it blocking around
    // acquire/present/create/destroy swapchain; geometry/slot mutations try_lock it.
    std::mutex& slot_lock() { return slot_lock_; }

    // the geometry-dirty latch.
    bool geometry_dirty() const { return geometry_dirty_.load(std::memory_order_acquire); }
    void clear_geometry_dirty() { geometry_dirty_.store(false, std::memory_order_release); }
    // Marks the slot dirty for any reason: a WM_SIZE delta or a slot/parent/composite mutation
    // regardless of size. Central so a future caller
    // does not inherit the size-delta shortcut by accident.
    void mark_geometry_dirty(const char* reason);

    // Records the active swapchain's client extent so WM_SIZE can tell a real geometry
    // change from show-time / same-size noise. (0,0) means "no active swapchain": a
    // WM_SIZE then is not a mismatch (nothing to invalidate yet).
    void set_swapchain_extent(int width, int height);

    // True if (width,height) equals the recorded swapchain extent, or no swapchain is
    // active. Used by the WndProc size-delta noise filter.
    bool size_matches_swapchain(int width, int height) const;

    // --- chrome paint (window-thread-only; NO locking -- aux_update / aux_blit /
    // aux_sample and the WM_PAINT that calls aux_blit all run on the window thread). A persistent
    // top-down BGRA8 buffer (the placeholder's base-layer chrome) that WM_PAINT StretchDIBits to
    // the client rect, so the painted chrome survives uncover/resize. --- Composite a captured
    // dirty rect into the buffer (resizing it when the source size changes). Returns false on a
    // degenerate source. Pixels are top-down BGRA8, src row pitch = `stride`.
    bool aux_update(const unsigned char* src, int src_w, int src_h, int dx, int dy, int dw, int dh,
                    int stride);
    // Paint the whole buffer, stretched to the client rect (called from WM_PAINT).
    void aux_blit(HDC hdc, const RECT& client) const;
    // The painted BGRA at (x,y) packed (B)|(G<<8)|(R<<16)|(A<<24); false if out of bounds /
    // unpainted.
    bool aux_sample(int x, int y, std::uint32_t& out_bgra) const;
    bool has_aux() const { return !aux_pixels_.empty(); }
    // copy the FULL chrome DIB (top-down BGRA8, stride = w*4) for PNG export. Returns
    // false if nothing has been painted yet.
    bool aux_capture(int& out_w, int& out_h, std::vector<unsigned char>& out_bgra) const;

    // --- input capture (window-thread-only; NO locking on these fields -- they are set
    // by set_input_target and read by the WndProc, both on the window thread; the InputQueue it
    // points at has its OWN dedicated mutex). The guest XID + representation epoch the WndProc
    // stamps captured events with, and the per-session ring it pushes into. --- Bind this window's
    // visible representation to a guest toplevel: the WndProc then captures Win32 input on it into
    // `queue` keyed by `xid`/`epoch`. Set when the backend creates the surface/placeholder window;
    // teardown (destroy_window) drops the slot, so the WndProc stops capturing.
    void set_input_target(sidecar::InputQueue* queue, std::uint64_t xid, std::uint64_t epoch) {
        input_queue_ = queue;
        guest_xid_ = xid;
        guest_epoch_ = epoch;
    }
    bool has_input_target() const { return input_queue_ != nullptr && guest_xid_ != 0; }
    std::uint64_t input_xid() const { return guest_xid_; }
    std::uint64_t input_epoch() const { return guest_epoch_; }
    // Stamp + push one captured event into the ring (no-op without a live input target). xid /
    // epoch come from the slot, not the caller (the WndProc builds only the neutral payload).
    void enqueue_input(sidecar::SidecarInputEvent ev) {
        if (input_queue_ != nullptr && guest_xid_ != 0) {
            input_queue_->enqueue(guest_xid_, guest_epoch_, ev);
        }
    }

    // --- cursor (window-thread-only; NO locking -- set_cursor / the WM_SETCURSOR that
    // calls apply_cursor / the debug sample all run on the window thread). The guest app's cursor,
    // built into an HCURSOR + kept as BGRA8 so the proof seam can sample it. --- Build a fresh
    // HCURSOR from `src` (top-down BGRA8, w*h*4) with hotspot (xhot,yhot). `apply_now` (set by the
    // caller iff the pointer is in this window's client rect) makes the new cursor the displayed
    // one BEFORE the old is destroyed -- the safe retire (the old is then never the displayed
    // cursor when DestroyCursor runs); when the pointer is elsewhere the old is already not
    // displayed, so it is destroyed without a (visible-elsewhere) SetCursor. Returns false on a
    // degenerate input.
    bool set_cursor(const unsigned char* src, int w, int h, int xhot, int yhot, bool apply_now);
    // Clear the installed cursor: drop the HCURSOR + BGRA mirror so
    // the proof seam reports no cursor and a later WM_SETCURSOR falls back to the system arrow.
    // Called when the guest X lifecycle that owned this cursor ends while the host window survives
    // (a surface-backed unregister leaves the surface HWND alive). `apply_default` (set by the
    // caller iff the pointer is in this window's client rect) restores the arrow BEFORE
    // DestroyCursor, so a stale custom cursor is never the displayed one when it is destroyed --
    // the same safe retire as set_cursor. Returns whether a cursor was actually cleared.
    bool clear_cursor(bool apply_default);
    // Apply the current cursor (called from WM_SETCURSOR for the client area).
    void apply_cursor() const;
    bool has_cursor() const { return cursor_ != nullptr; }
    // Proof seam: the built cursor's dims + hotspot, and the BGRA at (x,y) (false if no cursor /
    // OOB).
    void cursor_info(int& w, int& h, int& xhot, int& yhot) const;
    bool cursor_sample(int x, int y, std::uint32_t& out_bgra) const;
    // copy the FULL cursor image (top-down BGRA8, stride = w*4) + hotspot for PNG
    // export. Returns false if no cursor is built.
    bool cursor_capture(int& out_w, int& out_h, int& out_xhot, int& out_yhot,
                        std::vector<unsigned char>& out_bgra) const;

    // --- live geometry (window-thread-only; set_desired_geometry / the drain / the seq
    // accessors all run on the window thread, like the aux/input/cursor fields). The latest
    // sidecar-authored DESIRED geometry (X-root client origin + client size + its monotonic
    // registry host_apply_seq) is the per-host "latest-desired" coalescing cell; the highest
    // seq actually APPLIED to the HWND is the strictly-newer-wins gate so a
    // storm/repost converges to the newest authored geometry and never regresses. The slot_lock_
    // is a SEPARATE mutex (present serialization) -- these fields are not guarded by it. ---
    // Record a newer desired geometry (coalesce: a seq <= the current desired is ignored, so the
    // cell always holds the newest authored position).
    void set_desired_geometry(int root_x, int root_y, int client_w, int client_h, std::uint64_t seq,
                              bool apply_size, sidecar::ZOrder z_order) {
        // a z-order intent LATCHES independently of the seq coalesce -- a newer geometry-only
        // update (z_order None) must NOT clear a still-pending Raise/Lower (else a move that
        // coalesces over a reposted raise would drop it). The drain consumes it once applied.
        if (z_order != sidecar::ZOrder::None) {
            geom_desired_z_ = z_order;
        }
        if (seq <= geom_desired_seq_) {
            return;
        }
        geom_desired_x_ = root_x;
        geom_desired_y_ = root_y;
        geom_desired_w_ = client_w;
        geom_desired_h_ = client_h;
        geom_desired_seq_ = seq;
        geom_desired_apply_size_ = apply_size; // size the client (resize) vs SWP_NOSIZE (move)
    }
    // True iff the latest desired geometry has not yet been applied (the drain has work to do).
    bool has_pending_geometry() const { return geom_desired_seq_ > geom_applied_seq_; }
    // The latest desired geometry (meaningful iff has_pending_geometry()).
    void desired_geometry(int& root_x, int& root_y, int& client_w, int& client_h,
                          std::uint64_t& seq, bool& apply_size, sidecar::ZOrder& z_order) const {
        root_x = geom_desired_x_;
        root_y = geom_desired_y_;
        client_w = geom_desired_w_;
        client_h = geom_desired_h_;
        seq = geom_desired_seq_;
        apply_size = geom_desired_apply_size_;
        z_order = geom_desired_z_;
    }
    // Record that `seq` has been applied to the HWND (advances the apply gate) + consume the
    // one-shot z-order intent (it has now been folded into the SetWindowPos).
    void mark_geometry_applied(std::uint64_t seq) {
        geom_applied_seq_ = seq;
        geom_desired_z_ = sidecar::ZOrder::None;
    }
    // The highest geometry seq applied to this HWND (0 = no move yet); the proof seam reports
    // it.
    std::uint64_t last_host_apply_seq() const { return geom_applied_seq_; }

    // returns true exactly once, on the FIRST geometry apply (the initial register/promote
    // placement), so the worker's on-screen safety clamp runs only on initial placement -- never on
    // a later user/app move (which must win). Window-thread-only, like the geometry cell.
    bool consume_initial_placement() {
        const bool first = initial_placement_pending_;
        initial_placement_pending_ = false;
        return first;
    }

    // --- live visibility (window-thread-only, like the geometry cell). A SEPARATE
    // latest-desired cell with its OWN apply gate -- NOT the geometry gate. Visibility and geometry
    // are orthogonal at the HWND (a move never changes visibility, a hide never changes position),
    // so each converges to its own latest independently; they draw from the SAME monotonic registry
    // counter (host_apply_seq -- one total order at MINTING) but gate per-kind at APPLY. A shared
    // apply gate would be WRONG: geometry coalesces to its latest seq, so a later move (seq 6)
    // drains before an intermediate hide (seq 5) would advance the shared gate past 5 and silently
    // DROP the hide. Per-kind gates avoid that. `should_show` is the reveal predicate (intent ==
    // Visible && paint-eligible), computed by the backend under its lock and passed in (a Visible
    // window with an unpainted placeholder stays hidden until its first-paint reveal). ---
    void set_desired_visibility(sidecar::VisibilityState state, bool should_show,
                                std::uint64_t seq) {
        if (seq <= vis_desired_seq_) {
            return; // coalesce: keep the newest authored visibility
        }
        vis_desired_state_ = state;
        vis_desired_should_show_ = should_show;
        vis_desired_seq_ = seq;
    }
    bool has_pending_visibility() const { return vis_desired_seq_ > vis_applied_seq_; }
    void desired_visibility(sidecar::VisibilityState& state, bool& should_show,
                            std::uint64_t& seq) const {
        state = vis_desired_state_;
        should_show = vis_desired_should_show_;
        seq = vis_desired_seq_;
    }
    void mark_visibility_applied(std::uint64_t seq) { vis_applied_seq_ = seq; }

    // --- self-apply guard (window-thread-only). A depth counter set around EVERY
    // worker-origin SetWindowPos/ShowWindow (initial placement, geometry, visibility,
    // set_client_extent, show-after-first-paint) so the WndProc can tell a worker-driven host
    // mutation from a genuine Win32-USER gesture: the forward-enqueue path (the reverse
    // GeometryRequest) checks applying_self() and skips when set, so an echo / sidecar-driven apply
    // never loops back as a fake user request. It gates ONLY the forward-enqueue path -- NOT the
    // WM_SIZE dirty latch (a sidecar-authored resize still needs the latch). A depth
    // counter (not a bool) so nested applies compose. ---
    void enter_self_apply() { ++self_apply_depth_; }
    void leave_self_apply() {
        if (self_apply_depth_ > 0) {
            --self_apply_depth_;
        }
    }
    bool applying_self() const { return self_apply_depth_ > 0; }

    // the WM_ENTERSIZEMOVE..WM_EXITSIZEMOVE modal bracket (a genuine user move/resize
    // drag). The WndProc forwards a committed GeometryRequest on WM_EXITSIZEMOVE only if a matching
    // WM_ENTERSIZEMOVE opened the gesture (ignores a stray exit).
    void begin_user_move(int outer_w, int outer_h) {
        in_user_move_ = true;
        user_move_outer_w_ = outer_w;
        user_move_outer_h_ = outer_h;
    }
    void update_user_move_outer_extent(int outer_w, int outer_h) {
        if (in_user_move_ && outer_w > 0 && outer_h > 0) {
            user_move_outer_w_ = outer_w;
            user_move_outer_h_ = outer_h;
        }
    }
    bool user_move_outer_extent(int& outer_w, int& outer_h) const {
        if (!in_user_move_ || user_move_outer_w_ <= 0 || user_move_outer_h_ <= 0) {
            return false;
        }
        outer_w = user_move_outer_w_;
        outer_h = user_move_outer_h_;
        return true;
    }
    void end_user_move() {
        in_user_move_ = false;
        user_move_outer_w_ = 0;
        user_move_outer_h_ = 0;
    }
    bool in_user_move() const { return in_user_move_; }

    // the window's placement state at the last WM_WINDOWPOSCHANGED (0 normal / 1 iconic / 2
    // zoomed; -1 unknown before the first). The snap-forward path forwards ONLY an in-NORMAL-state
    // move (a snap / Win+Arrow): a placement-state TRANSITION (minimize/restore/maximize, and the
    // first placement from unknown) is owned by the WM_SYSCOMMAND path and must NOT also double-
    // forward as a Geometry request. Window-thread-only.
    int last_pos_state() const { return last_pos_state_; }
    void set_last_pos_state(int s) { last_pos_state_ = s; }

    // The last REALIZED host client geometry in X-root coords (read from the
    // live HWND after a worker apply / a user forward). Distinct from the AUTHORED desired cell:
    // the authored cell is what the sidecar asked for; this is what Win32 actually realized (which
    // can differ, e.g. the initial frame clamp moves the client so the caption stays on-screen).
    // The reverse path uses it as the no-op baseline (a gesture that did not change the realized
    // geometry forwards nothing -- the caption-click drift fix), and the realization-feedback
    // compares against the authored cell to decide whether to reconcile. Window-thread-only.
    void set_realized_geometry(int root_x, int root_y, int client_w, int client_h) {
        realized_x_ = root_x;
        realized_y_ = root_y;
        realized_w_ = client_w;
        realized_h_ = client_h;
        realized_valid_ = true;
    }
    bool realized_geometry(int& root_x, int& root_y, int& client_w, int& client_h) const {
        if (!realized_valid_) {
            return false;
        }
        root_x = realized_x_;
        root_y = realized_y_;
        client_w = realized_w_;
        client_h = realized_h_;
        return true;
    }

    // provenance for one realization-feedback GeometryRequest. When an authored apply lands
    // at a different live client position, the worker records the exact geometry it fed back. If
    // the sidecar later echoes that geometry while the HWND is still there, the drain ACKs the
    // newer sequence without applying the same fallible frame transform again. Any differing
    // desired/live geometry consumes and invalidates the token. Position-only adoption ignores a
    // transient client-size mismatch; an authoritative resize requires exact live size too.
    void set_pending_realization_adoption(int root_x, int root_y, int client_w, int client_h) {
        adoption_x_ = root_x;
        adoption_y_ = root_y;
        adoption_w_ = client_w;
        adoption_h_ = client_h;
        adoption_pending_ = true;
    }
    bool consume_pending_realization_adoption(int desired_x, int desired_y, int desired_w,
                                              int desired_h, bool apply_size, int live_x,
                                              int live_y, int live_w, int live_h) {
        if (!adoption_pending_) {
            return false;
        }
        adoption_pending_ = false;
        const bool desired_matches = desired_x == adoption_x_ && desired_y == adoption_y_ &&
                                     desired_w == adoption_w_ && desired_h == adoption_h_;
        const bool live_matches = live_x == adoption_x_ && live_y == adoption_y_ &&
                                  (!apply_size || (live_w == adoption_w_ && live_h == adoption_h_));
        return desired_matches && live_matches;
    }

    // A Win32-USER maximize / restore-from-maximize (WM_SYSCOMMAND
    // SC_MAXIMIZE / SC_RESTORE-when-zoomed) is not a drag, so it arrives via WM_SYSCOMMAND but its
    // final geometry is only known at the resulting WM_SIZE. Set a one-shot pending flag on the
    // SYSCOMMAND; the WM_SIZE (SIZE_MAXIMIZED / SIZE_RESTORED) consumes it and forwards the ACTUAL
    // final host geometry back to the guest via the same reverse path as a drag.
    // Window-thread-only.
    void set_pending_system_geometry() { pending_system_geometry_ = true; }
    bool consume_pending_system_geometry() {
        const bool p = pending_system_geometry_;
        pending_system_geometry_ = false;
        return p;
    }

    // the last sidecar-AUTHORED geometry (X-root client coords) -- the no-op-churn
    // baseline the WndProc compares a committed user rect against (so a user gesture that lands
    // back on the authored geometry does not become a redundant request). Returns false if none
    // authored yet.
    bool last_authored_geometry(int& root_x, int& root_y, int& client_w, int& client_h) const {
        if (geom_desired_seq_ == 0) {
            return false;
        }
        root_x = geom_desired_x_;
        root_y = geom_desired_y_;
        client_w = geom_desired_w_;
        client_h = geom_desired_h_;
        return true;
    }

  private:
    const display::DisplayLayout* display_layout_ = nullptr; // owned by the WindowThread
    // Shared by every HWND in one worker session. Notifications only latch a restart requirement;
    // they never mutate or replace the immutable snapshot above.
    std::atomic<bool>* display_restart_required_ = nullptr;
    std::mutex slot_lock_;
    std::atomic<bool> geometry_dirty_{false};
    std::atomic<std::uint64_t> swapchain_extent_{0}; // packed (w<<32)|h; 0 = none
    // aux paint, window-thread-only (no synchronization needed).
    std::vector<unsigned char> aux_pixels_; // top-down BGRA8, row pitch = aux_w_*4
    int aux_w_ = 0;
    int aux_h_ = 0;
    // input target, window-thread-only (set_input_target / WndProc, same thread).
    sidecar::InputQueue* input_queue_ = nullptr;
    std::uint64_t guest_xid_ = 0;
    std::uint64_t guest_epoch_ = 0;
    // cursor, window-thread-only. cursor_ is the HCURSOR WM_SETCURSOR applies; the BGRA
    // + dims/hotspot mirror it for the proof seam.
    HCURSOR cursor_ = nullptr;
    std::vector<unsigned char> cursor_bgra_; // top-down BGRA8, row pitch = cursor_w_*4
    int cursor_w_ = 0;
    int cursor_h_ = 0;
    int cursor_xhot_ = 0;
    int cursor_yhot_ = 0;
    // live geometry, window-thread-only. geom_desired_* is the latest authored desired
    // (X-root client origin + client size); geom_*_seq_ are the desired/applied gate.
    int geom_desired_x_ = 0;
    int geom_desired_y_ = 0;
    int geom_desired_w_ = 0;
    int geom_desired_h_ = 0;
    std::uint64_t geom_desired_seq_ = 0;
    std::uint64_t geom_applied_seq_ = 0;
    bool geom_desired_apply_size_ = false; // resize the client vs position-only move
    sidecar::ZOrder geom_desired_z_ = sidecar::ZOrder::None; // pending one-shot restack intent
    bool initial_placement_pending_ = true; // clamp on-screen on the first apply only
    // live visibility, window-thread-only. A separate cell + gate from geometry (see
    // the accessors above for why the gate is per-kind).
    sidecar::VisibilityState vis_desired_state_ = sidecar::VisibilityState::Visible;
    bool vis_desired_should_show_ = false;
    std::uint64_t vis_desired_seq_ = 0;
    std::uint64_t vis_applied_seq_ = 0;
    // self-apply guard + user-gesture bracket, window-thread-only.
    int self_apply_depth_ = 0;
    bool in_user_move_ = false;
    int user_move_outer_w_ = 0; // last pre-DPI user-intended outer extent during modal drag
    int user_move_outer_h_ = 0;
    int last_pos_state_ = -1; // last WM_WINDOWPOSCHANGED placement state (1 unknown)
    // Pending Win32-user maximize/restore-from-maximize whose final geometry is fed back on
    // the resulting WM_SIZE. Window-thread-only (set in WM_SYSCOMMAND, consumed in WM_SIZE).
    bool pending_system_geometry_ = false;
    // Last REALIZED host client geometry (X-root), used as the reverse-path no-op baseline.
    int realized_x_ = 0;
    int realized_y_ = 0;
    int realized_w_ = 0;
    int realized_h_ = 0;
    bool realized_valid_ = false;
    // one-shot provenance token for the realization GeometryRequest currently in flight.
    int adoption_x_ = 0;
    int adoption_y_ = 0;
    int adoption_w_ = 0;
    int adoption_h_ = 0;
    bool adoption_pending_ = false;
};

// A created window and its slot. The slot pointer stays valid until destroy_window().
struct CreatedWindow {
    HWND hwnd = nullptr;
    WindowSlot* slot = nullptr;
};

class WindowThread {
  public:
    explicit WindowThread(const display::DisplayLayout* display_layout = nullptr);
    // starts the thread; returns once the pump is ready
    ~WindowThread(); // posts WM_QUIT and joins
    WindowThread(const WindowThread&) = delete;
    WindowThread& operator=(const WindowThread&) = delete;

    // Creates a hidden top-level window with the given client size, on the window
    // thread, plus its WindowSlot. Returns {nullptr,nullptr} on failure.
    CreatedWindow create_hidden_window(int width, int height);
    // creates a hidden OWNED popup host on the window thread, plus its WindowSlot. The
    // window is an owned WS_POPUP (no caption/border, so the client rect == the window rect) with
    // WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE: it stacks ABOVE its owner and is reaped with it (the
    // static z-order), and never steals focus/activation from the owner toplevel. Positioned
    // at the popup's reported X ROOT coordinates (x,y) at physical size (width,height) -- WS_POPUP
    // is screen-positioned, so there is no Win32 client/chrome-origin mixing. Hidden until the
    // first-paint commit (popup chrome rides the existing paint path). Returns {nullptr,nullptr} on
    // failure (incl. a null owner).
    CreatedWindow create_popup_window(HWND owner, int x, int y, int width, int height);
    // Destroys a window created here (and its slot), on the window thread. No-op on
    // nullptr. The caller must drop any WindowSlot pointer to this window first.
    void destroy_window(HWND hwnd);
    // Reveals a window (no focus steal), on the window thread -- show-on-first-present.
    void show_window(HWND hwnd);

    // chrome paint: composite a captured dirty rect into the window's persistent BGRA
    // buffer (resized to src_w x src_h) and invalidate it -- WITHOUT showing the window. The window
    // stays HIDDEN here; the backend reveals it via show_aux_window ONLY after the registry commit
    // succeeds, so a placeholder is never visible while uncommitted (a data-plane promote that
    // races the paint fails the commit and the window is never shown). All on the window thread
    // Returns false if the window is unknown or the pump cannot be woken, so the backend only
    // commits after a realized paint. `src` is top-down BGRA8, row pitch `stride`; the dirty rect
    // (dx,dy,dw,dh) lies within the source (validated by the decoder).
    bool paint_aux(HWND hwnd, const unsigned char* src, int src_w, int src_h, int dx, int dy,
                   int dw, int dh, int stride);
    // Reveal a freshly-painted placeholder (no focus steal) and force its WM_PAINT, so the captured
    // chrome (already staged in the DIB by paint_aux) is on-screen before this returns. Called by
    // the backend only after the first paint's registry commit succeeds.
    void show_aux_window(HWND hwnd);
    // Sample the BGRA the window last painted at (x,y) -- the worker-visible proof seam. Returns
    // false (out_bgra untouched) if the window is unknown, the point is out of bounds, or nothing
    // painted.
    bool sample_aux_pixel(HWND hwnd, int x, int y, std::uint32_t& out_bgra);

    // bind a window's slot to a guest toplevel for input capture, on the window thread
    // (the slot's input fields are window-thread-only). The WndProc then captures Win32 input on it
    // into `queue` keyed by `xid`/`epoch`. No-op for an unknown HWND. The `queue` must outlive the
    // window thread (the backend owns it and declares it before the window thread).
    void set_slot_input_target(HWND hwnd, sidecar::InputQueue* queue, std::uint64_t xid,
                               std::uint64_t epoch);

    // build + bind the window's cursor (top-down BGRA8, w*h*4, hotspot xhot/yhot) on
    // the window thread; WM_SETCURSOR then applies it for the client area. No-op for an unknown
    // HWND. Returns whether a cursor was built. Sample the built cursor via debug_cursor (the proof
    // seam).
    bool set_window_cursor(HWND hwnd, const unsigned char* bgra, int w, int h, int xhot, int yhot);
    // clear the window's installed cursor on the window thread (the
    // guest X lifecycle ended while the host window survives). No-op for an unknown HWND. Restores
    // the system arrow only when the pointer is actually in this window's client rect. Returns
    // whether a cursor was cleared.
    bool clear_window_cursor(HWND hwnd);
    // Proof seam: the window's built cursor dims/hotspot + the BGRA at (x,y). out_* untouched (and
    // returns false) for an unknown HWND or no cursor.
    bool debug_cursor(HWND hwnd, int sample_x, int sample_y, int& out_w, int& out_h, int& out_xhot,
                      int& out_yhot, bool& out_has_pixel, std::uint32_t& out_bgra);

    // (source-layer capture): copy a window's FULL chrome DIB / cursor image off the
    // window thread for PNG export. Each returns false for an unknown HWND or an empty layer
    // (nothing painted / no cursor). Pixels are top-down BGRA8; `out_stride` == out_w*4.
    bool debug_capture_chrome(HWND hwnd, int& out_w, int& out_h, int& out_stride,
                              std::vector<unsigned char>& out_bgra);
    bool debug_capture_cursor(HWND hwnd, int& out_w, int& out_h, int& out_xhot, int& out_yhot,
                              int& out_stride, std::vector<unsigned char>& out_bgra);

    // (optional debug HWND title tag): set the window title to "vkrelay2 [xid=0x...]"
    // so the capture_window.ps1 dev helper can correlate an enumerated HWND back to a guest
    // toplevel. Gated by the caller (VKRELAY2_DEBUG_WINDOW_TITLES); no-op for an unknown HWND. The
    // title is readable from any thread via GetWindowText (the test reads it directly).
    void set_window_title_tag(HWND hwnd, std::uint64_t xid);

    // The actual client size a set_client_extent achieved (Win32 may clamp -- a caption-enforced
    // minimum width, the work-area, DPI -- so the caller can detect non-convergence). {0,0} on
    // failure.
    struct ClientExtent {
        int width = 0;
        int height = 0;
    };
    // Sizes the window's CLIENT rect to (width,height) on the window thread (the app picks its
    // extent, the worker makes the host window match) and returns the ACTUAL client size achieved.
    // This is the seam the sidecar geometry authority will later drive; create_swapchain uses
    // it and treats a non-converging result as VK_ERROR_OUT_OF_DATE_KHR rather than presenting at a
    // stale extent. The resulting WM_SIZE is dispatched inline on the window thread.
    ClientExtent set_client_extent(HWND hwnd, int width, int height);

    // live geometry apply (sidecar geometry authority): reposition -- and, when
    // `apply_size` is set (the sidecar is the extent authority for this xid), RESIZE -- the
    // host window so its CLIENT lands at the guest toplevel's X-root client origin (root_x, root_y)
    // at (client_w, client_h), generation/seq-sequenced. Records the latest-desired cell and drains
    // it on the window thread: the apply is seq-gated (drops a stale/coalesced apply) and
    // slot-serialized via try_lock + repost (so it never blocks a present holding the slot lock --
    // uncontended for placeholder/popup, which nothing else locks). When !apply_size the apply is
    // POSITION-ONLY (SWP_NOSIZE; client_w/h only compute the outer FRAME origin via
    // map_root_client_to_win32_frame so a chromed client lands right) -- no WM_SIZE, no latch
    // When apply_size and the client size changes, the sizing SetWindowPos trips WM_SIZE ->
    // the dirty latch, so the app recreates the swapchain at the authored extent. No-op for a
    // null/unknown HWND.
    void apply_geometry(HWND hwnd, int root_x, int root_y, int client_w, int client_h,
                        std::uint64_t host_apply_seq, bool apply_size, sidecar::ZOrder z_order);

    // live visibility apply (show/hide lifecycle, sidecar->worker): ShowWindow the host
    // to `state` (Hidden -> SW_HIDE; Visible -> SW_SHOWNA iff `should_show`, else stay hidden;
    // Iconic -> SW_SHOWMINNOACTIVE. `should_show` is the backend's reveal
    // predicate (intent == Visible && paint-eligible), so a Visible window whose placeholder has no
    // first-paint stays hidden until paint_chrome reveals it. Records the latest-desired visibility
    // cell + drains it on the window thread: seq-gated on its OWN gate (per-kind, NOT the geometry
    // gate -- see WindowSlot) and slot-serialized via try_lock + repost so it never blocks a
    // present. SW_SHOWNA / SW_SHOWMINNOACTIVE never activate, so a restore never steals focus.
    // No-op for a null/unknown HWND.
    void apply_visibility(HWND hwnd, sidecar::VisibilityState state, bool should_show,
                          std::uint64_t host_apply_seq);

    // proof seam (worker-visible convergence): read a window's ACTUAL host geometry off
    // the window thread -- the outer window rect + client extent (both via Get*Rect), the client
    // origin in SCREEN coords (ClientToScreen), the pinned host origin used for the 1:1 mapping,
    // and the highest applied geometry seq. The caller maps the client origin back to X-root coords
    // (subtract the work origin) for the convergence gate. Returns false for an unknown HWND.
    // adds the host-OBSERVED visibility (IsWindowVisible / IsIconic), read on the
    // window thread alongside the rects so the obs proof asserts host_visible tracks the authored
    // state across a hide/show.
    bool query_geometry(HWND hwnd, RECT& out_frame, int& out_client_w, int& out_client_h,
                        POINT& out_client_origin, POINT& out_work_origin,
                        std::uint64_t& out_applied_seq, bool& out_visible, bool& out_iconic);

    // Runs `fn` to completion on the window thread and blocks for it. Returns false
    // (without blocking) if the pump cannot be woken (thread gone / post failed), so the
    // backend gets a clean failure instead of hanging. Used to route HWND-touching WSI
    // driver calls onto the window thread.
    bool invoke(const std::function<void()>& fn) { return run_on_thread(fn); }

    // Level-1 static-topology seam. Display/work-area notifications latch this once and leave
    // the session snapshot and transform untouched. Public for deterministic integration coverage.
    bool display_restart_required() const {
        return display_restart_required_.load(std::memory_order_acquire);
    }

    // (cross-monitor maximize guard): publish the session's OBSERVED guest root (the sidecar's
    // X-root W/H from SidecarReady) so every host window's WndProc caps its client to it on
    // WM_GETMINMAXINFO. STATIC: one worker process == one session == one guest root, and the
    // WndProc is a free function with no WindowThread `this`, so the value lives in a file-scope
    // atomic that this writes -- callable regardless of whether a window thread has been
    // constructed yet (the backend calls it straight from sidecar_ready). (width,height)=0 clears
    // the cap.
    static void set_guest_root(std::uint32_t width, std::uint32_t height);
    // The published guest root packed (w<<32)|h, 0 if unset. The over-root worker guard in
    // get_surface_capabilities reads it back (single source of truth with the WndProc's cap).
    static std::uint64_t guest_root_packed();

    // the immutable session snapshot's virtual top-left in host screen space. Every
    // live root<->Win32 mapping reads this process-wide value (one worker == one session); no
    // window path independently re-probes the primary work area.
    static void set_host_origin(std::int32_t x, std::int32_t y);
    static POINT host_origin();

  private:
    display::DisplayLayout display_layout_;
    bool has_display_layout_ = false;
    std::atomic<bool> display_restart_required_{false};
    void run(); // thread body: class + pump
    bool run_on_thread(const std::function<void()>& fn);
    // Non-blocking enqueue of `fn` onto the window thread (no result wait). Used by the
    // geometry drain to REPOST itself when the slot try_lock fails -- a blocking re-invoke from
    // within a window-thread task would self-deadlock (the pump is busy running that task).
    void post_on_thread(std::function<void()> fn);
    // apply the slot's latest desired geometry to `hwnd` (window-thread-only).
    // Seq-gated (drops if nothing newer than the applied seq) + slot try_lock'd (reposts itself if
    // a present holds the slot, so the pump never blocks); position-only SWP_NOSIZE.
    void drain_geometry(HWND hwnd);
    // apply the slot's latest desired visibility to `hwnd` (window-thread-only).
    // Seq-gated on the visibility cell's OWN gate + slot try_lock'd (reposts if a present holds the
    // slot), mirroring drain_geometry.
    void drain_visibility(HWND hwnd);

    std::thread thread_;
    DWORD thread_id_ = 0;
    std::wstring class_name_;
    std::mutex mu_;
    std::condition_variable cv_;
    bool ready_ = false;
    std::deque<std::function<void()>> tasks_;
    // Hidden top-level sentinel so display/work-area broadcasts are observed even before the
    // application creates its first projected HWND. A message-only window would not receive
    // broadcast display messages.
    HWND display_observer_ = nullptr;
    // Owns the slots; only touched on the window thread (create/destroy run there).
    std::map<HWND, std::unique_ptr<WindowSlot>> slots_;
};

} // namespace vkr::worker::windowing

#endif // VKRELAY2_WINDOWS_WORKER_WINDOWING_WINDOW_THREAD_HPP
