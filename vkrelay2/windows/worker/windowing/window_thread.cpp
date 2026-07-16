#include "windows/worker/windowing/window_thread.hpp"

#include "common/logging/logging.hpp"
#include "common/sidecar/window_placement.hpp" // on-screen clamp helper
#include "windows/worker/windowing/coordinate_map.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <future>
#include <utility>

#include <windowsx.h> // GET_X_LPARAM / GET_Y_LPARAM / GET_WHEEL_DELTA_WPARAM

namespace vkr::worker::windowing {
namespace {

constexpr char kComponent[] = "vkrpc-window";

// Thread message that wakes the pump to drain queued tasks. Thread messages
// (PostThreadMessage) have a null hwnd and are not dispatchable, so the pump
// inspects msg.message directly.
constexpr UINT kRunTasks = WM_APP + 1;

std::uint64_t pack_extent(int width, int height) {
    const auto w = static_cast<std::uint32_t>(width < 0 ? 0 : width);
    const auto h = static_cast<std::uint32_t>(height < 0 ? 0 : height);
    return (static_cast<std::uint64_t>(w) << 32) | h;
}

// Cross-monitor maximize guard: the session's guest root (packed w<<32|h), the
// guest-realizable CLIENT extent the WndProc caps every host window to on WM_GETMINMAXINFO. One
// worker process == one session == one guest root, so a file-scope atomic is the right carrier: the
// WndProc is a free function reached via GWLP_USERDATA (it has no WindowThread `this`), and every
// window on this worker shares the one root. 0 = not reported yet (older sidecar /
// pre-SidecarReady) -> no cap. Set from the sidecar's OBSERVED X root (SidecarReady root_w/h) via
// WindowThread::set_guest_root.
std::atomic<std::uint64_t> g_guest_root_packed{0};
std::atomic<std::uint64_t> g_host_origin_packed{0};

// Gate the geometry-forward trace (VKRELAY2_GEOM_TRACE) -- the WM_WINDOWPOSCHANGED handler
// logs the realized geometry + whether it forwarded, so a user's snap repro confirms the message
// path end-to-end. Read once at static init; off (no log) unless the env var is set.
const bool g_geom_trace = std::getenv("VKRELAY2_GEOM_TRACE") != nullptr;
const bool g_input_trace = std::getenv("VKRELAY2_INPUT_TRACE") != nullptr;

// The kInputMod* bitmask from the current Win32 key state (the high bit marks a key held). The
// sidecar reapplies these as fake key state around an injected key.
std::uint32_t current_modifiers() {
    std::uint32_t m = 0;
    if (GetKeyState(VK_SHIFT) & 0x8000) {
        m |= sidecar::kInputModShift;
    }
    if (GetKeyState(VK_CONTROL) & 0x8000) {
        m |= sidecar::kInputModCtrl;
    }
    if (GetKeyState(VK_MENU) & 0x8000) {
        m |= sidecar::kInputModAlt;
    }
    return m;
}

// RAII self-apply guard. Marks the slot's host mutation as worker-origin for the
// duration of a SetWindowPos/ShowWindow, so the WndProc's reverse GeometryRequest forward path
// skips any WM_* those calls dispatch (synchronously, on this same window thread) -- a
// worker-driven apply / an echo never loops back as a fake user gesture. Gates ONLY the
// forward-enqueue path, never the WM_SIZE dirty latch. Window-thread-only.
struct SelfApplyScope {
    WindowSlot* slot;
    explicit SelfApplyScope(WindowSlot* s) : slot(s) {
        if (slot != nullptr) {
            slot->enter_self_apply();
        }
    }
    ~SelfApplyScope() {
        if (slot != nullptr) {
            slot->leave_self_apply();
        }
    }
    SelfApplyScope(const SelfApplyScope&) = delete;
    SelfApplyScope& operator=(const SelfApplyScope&) = delete;
};

// On a committed Win32-USER move/resize/maximize (WM_EXITSIZEMOVE, or the
// WM_SIZE feedback), turn the host window's ACTUAL client geometry into a guest geometry
// REQUEST -- the reverse direction (worker->sidecar). Read the REAL
// client geometry from the live HWND (read_real_root_client_geometry) rather than inverse-mapping
// the frame via AdjustWindowRectExForDpi -- the real readback is the ground truth of where the
// client the user clicks actually is (it sees Win32/DWM realization the pure inverse cannot).
// The decision maybe_forward_user_geometry made -- so the geom trace is DECISIVE about whether
// a candidate actually forwarded, and WHY not, rather than inferring from a
// later sidecar log. Only `Forwarded` enqueues a GeometryRequest.
enum class ForwardResult { Forwarded, SelfApply, NoInputTarget, NoChange, AuthoredEcho };

const char* forward_result_name(ForwardResult r) {
    switch (r) {
    case ForwardResult::Forwarded:
        return "forwarded";
    case ForwardResult::SelfApply:
        return "skip:self-apply";
    case ForwardResult::NoInputTarget:
        return "skip:no-input-target";
    case ForwardResult::NoChange:
        return "skip:no-change";
    case ForwardResult::AuthoredEcho:
        return "skip:authored-echo";
    }
    return "?";
}

// Enqueues ONE GeometryRequest iff the realized geometry actually CHANGED since the last realized
// baseline (the no-op guard: a no-op title-bar click that does not move the window forwards nothing
// -- the caption-drift fix), and it is not a churn back to the authored geometry. SKIPPED when
// applying_self() (worker-origin) or with no input target. Window-thread-only. req_w/req_h carry
// the committed client extent; the sidecar authors the guest resize on a real size delta
// (user_request_is_resize). Returns which decision it made (for the trace).
ForwardResult maybe_forward_user_geometry(WindowSlot& slot, HWND hwnd) {
    if (slot.applying_self()) {
        return ForwardResult::SelfApply;
    }
    if (!slot.has_input_target()) {
        return ForwardResult::NoInputTarget;
    }
    const RootClientGeometry g = read_real_root_client_geometry(hwnd, WindowThread::host_origin());
    int rx = 0, ry = 0, rw = 0, rh = 0;
    if (slot.realized_geometry(rx, ry, rw, rh) && g.root_x == rx && g.root_y == ry &&
        g.client_w == rw && g.client_h == rh) {
        return ForwardResult::NoChange; // the window did not move -- forward nothing
    }
    int ax = 0, ay = 0, aw = 0, ah = 0;
    if (slot.last_authored_geometry(ax, ay, aw, ah) && g.root_x == ax && g.root_y == ay &&
        g.client_w == aw && g.client_h == ah) {
        return ForwardResult::AuthoredEcho; // landed back on the authored geometry -- not a request
    }
    sidecar::SidecarInputEvent ev;
    ev.kind = static_cast<std::uint32_t>(sidecar::InputEventKind::GeometryRequest);
    ev.host_request = static_cast<std::uint32_t>(sidecar::HostRequestKind::Geometry);
    ev.root_x = g.root_x;
    ev.root_y = g.root_y;
    ev.req_w = static_cast<std::uint32_t>(g.client_w);
    ev.req_h = static_cast<std::uint32_t>(g.client_h);
    slot.enqueue_input(ev);
    slot.set_realized_geometry(g.root_x, g.root_y, g.client_w, g.client_h); // new baseline
    return ForwardResult::Forwarded;
}

// Translate one captured Win32 input message into a neutral event and push it into the slot's ring
// Coordinates are window-relative CLIENT pixels (physical px; the thread is
// PMv1-aware) -- the sidecar maps them to X root coords. Keys carry the Win32 VK/scancode (the
// worker does not own the X keymap; the sidecar maps `vk` to a keysym/keycode). The caller has
// already confirmed the slot has a live input target.
void capture_input(WindowSlot& slot, HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    using sidecar::InputEventKind;
    sidecar::SidecarInputEvent ev;
    ev.modifiers = current_modifiers();
    switch (msg) {
    case WM_MOUSEMOVE:
        ev.kind = static_cast<std::uint32_t>(InputEventKind::Motion);
        ev.client_x = GET_X_LPARAM(lparam);
        ev.client_y = GET_Y_LPARAM(lparam);
        break;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
        ev.kind = static_cast<std::uint32_t>(InputEventKind::Button);
        ev.pressed = (msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_MBUTTONDOWN);
        ev.button = (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP)   ? sidecar::kInputButtonLeft
                    : (msg == WM_RBUTTONDOWN || msg == WM_RBUTTONUP) ? sidecar::kInputButtonRight
                                                                     : sidecar::kInputButtonMiddle;
        ev.client_x = GET_X_LPARAM(lparam);
        ev.client_y = GET_Y_LPARAM(lparam);
        break;
    case WM_MOUSEWHEEL: {
        ev.kind = static_cast<std::uint32_t>(InputEventKind::Wheel);
        // The wheel message's lParam is SCREEN coords; map to this window's client area so the
        // sidecar warps to the right spot before the Button4/Button5 it injects.
        POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        ScreenToClient(hwnd, &pt);
        ev.client_x = pt.x;
        ev.client_y = pt.y;
        ev.wheel = GET_WHEEL_DELTA_WPARAM(wparam) > 0 ? 1 : -1;
        break;
    }
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYUP:
        ev.kind = static_cast<std::uint32_t>(InputEventKind::Key);
        ev.pressed = (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN);
        ev.vk = static_cast<int>(wparam);
        ev.scancode = static_cast<int>((lparam >> 16) & 0xFF);
        break;
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        ev.kind = static_cast<std::uint32_t>(InputEventKind::Focus);
        ev.pressed = (msg == WM_SETFOCUS);
        break;
    default:
        return;
    }
    if (g_input_trace) {
        VKR_INFO(kComponent) << "input-trace station=wndproc xid=" << slot.input_xid()
                             << " epoch=" << slot.input_epoch() << " kind=" << ev.kind
                             << " client=" << ev.client_x << "," << ev.client_y
                             << " button=" << ev.button << " pressed=" << ev.pressed;
    }
    slot.enqueue_input(ev);
    if (g_input_trace) {
        VKR_INFO(kComponent) << "input-trace station=ring-enqueue xid=" << slot.input_xid()
                             << " epoch=" << slot.input_epoch() << " kind=" << ev.kind;
    }
}

bool ascii_equals_wide(const std::string& ascii, const wchar_t* wide) {
    if (wide == nullptr) {
        return false;
    }
    std::size_t i = 0;
    for (; i < ascii.size() && wide[i] != L'\0'; ++i) {
        if (static_cast<unsigned char>(ascii[i]) != static_cast<unsigned int>(wide[i])) {
            return false;
        }
    }
    return i == ascii.size() && wide[i] == L'\0';
}

const display::MonitorDesc* snapshot_monitor_for_window(const display::DisplayLayout& layout,
                                                        HWND hwnd) {
    // MonitorFromWindow selects membership using Win32's current window rule. GetMonitorInfo is
    // used only to obtain the device identity; its live rcMonitor/rcWork are deliberately ignored.
    // device_name is an ephemeral within-session join from this live HMONITOR to a row in the
    // already-pinned snapshot, not a persistent identity key. stable_id remains canonical across
    // snapshots, and the snapshot-geometric fallback below handles a failed device-name join.
    MONITORINFOEXW live{};
    live.cbSize = sizeof(live);
    const HMONITOR handle = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    if (handle != nullptr && GetMonitorInfoW(handle, &live)) {
        for (const display::MonitorDesc& monitor : layout.monitors) {
            if (ascii_equals_wide(monitor.device_name, live.szDevice)) {
                return &monitor;
            }
        }
    }
    RECT frame{};
    if (!GetWindowRect(hwnd, &frame)) {
        return display::find_monitor_by_id(layout, layout.primary_monitor_id);
    }
    const display::RectI32 rect{frame.left, frame.top, frame.right - frame.left,
                                frame.bottom - frame.top};
    const display::MonitorDesc* monitor = display::find_monitor_largest_intersection(layout, rect);
    return monitor != nullptr ? monitor : display::find_monitor_nearest(layout, rect);
}

// The window procedure for every worker HWND. handles WM_SIZE (the geometry-dirty
// latch); handles WM_PAINT (placeholder chrome); captures pointer/wheel/key/
// focus input + WM_CLOSE and routes them to the guest via the sidecar. Captured input still
// falls through to DefWindowProcW for normal Win32 bookkeeping (e.g. focus activation); WM_CLOSE is
// SWALLOWED (the worker never self-destroys -- the guest is the lifecycle authority). The
// WindowSlot is stashed in GWLP_USERDATA at creation, so the proc reaches it without a shared map
// lookup (no table race); a torn-down window has a null slot (GWLP_USERDATA cleared first), so
// input capture stops at teardown.
LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    auto* slot = reinterpret_cast<WindowSlot*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (g_input_trace && slot != nullptr &&
        (msg == WM_ENTERSIZEMOVE || msg == WM_EXITSIZEMOVE || msg == WM_SIZING || msg == WM_SIZE ||
         msg == WM_WINDOWPOSCHANGING || msg == WM_WINDOWPOSCHANGED || msg == WM_DPICHANGED ||
         msg == WM_GETDPISCALEDSIZE)) {
        RECT frame{};
        RECT client{};
        GetWindowRect(hwnd, &frame);
        GetClientRect(hwnd, &client);
        VKR_INFO(kComponent) << "input-trace station=wndproc-geometry msg=" << msg
                             << " in_move=" << (slot->in_user_move() ? 1 : 0)
                             << " self=" << (slot->applying_self() ? 1 : 0)
                             << " frame=" << frame.left << "," << frame.top << " "
                             << (frame.right - frame.left) << "x" << (frame.bottom - frame.top)
                             << " client=" << (client.right - client.left) << "x"
                             << (client.bottom - client.top) << " dpi=" << GetDpiForWindow(hwnd)
                             << " wparam=" << static_cast<std::uint64_t>(wparam);
    }
    switch (msg) {
    case WM_DISPLAYCHANGE:
        if (slot != nullptr) {
            slot->note_display_configuration_change("WM_DISPLAYCHANGE");
        }
        break;
    case WM_SETTINGCHANGE:
        // SPI_SETWORKAREA is the taskbar/reserved-work-area mutation relevant to the snapshot.
        // Other user-setting broadcasts do not change this session's topology contract.
        if (slot != nullptr && wparam == SPI_SETWORKAREA) {
            slot->note_display_configuration_change("WM_SETTINGCHANGE/SPI_SETWORKAREA");
        }
        break;
    case WM_WINDOWPOSCHANGING:
        // Under PMv1, USER's modal move loop can replace the drag rectangle's physical extent
        // with the destination monitor's DPI-scaled extent *before* WM_DPICHANGED, then reuse that
        // scaled drag rectangle on every later mouse step. Correct the proposal before it is
        // realized: a caption move retains the extent captured at WM_ENTERSIZEMOVE, while a real
        // border resize receives its latest user-authored extent from WM_SIZING below. Position,
        // z-order, and all non-modal/programmatic applies remain untouched.
        if (slot != nullptr && slot->in_user_move() && lparam != 0) {
            auto* proposal = reinterpret_cast<WINDOWPOS*>(lparam);
            if ((proposal->flags & SWP_NOSIZE) == 0) {
                int outer_w = 0;
                int outer_h = 0;
                if (slot->user_move_outer_extent(outer_w, outer_h)) {
                    proposal->cx = outer_w;
                    proposal->cy = outer_h;
                }
            }
        }
        break;
    case WM_DPICHANGED:
        // Level-1 DPI policy: every worker-managed HWND represents a guest-authored
        // PHYSICAL-pixel rectangle. DefWindowProcW applies lParam's suggested rectangle during a
        // real cross-monitor caption drag (including its DPI-scaled size), which silently creates
        // a second extent authority and can dirty/resize the guest surface. A live slot identifies
        // all managed representations -- surface, placeholder, and popup -- so consume the
        // notification without adopting the suggested SIZE. Under PMv1 the real modal move loop
        // still supplies a new drag anchor even though WM_WINDOWPOSCHANGING above suppresses its
        // scaled extent, so during a user drag explicitly realize the suggested LEFT/TOP with the
        // gesture's physical outer extent. This position-only SetWindowPos keeps the window tied
        // to the cursor, and lets WM_EXITSIZEMOVE publish the final physical-pixel geometry.
        // Outside a modal drag, suppression alone preserves a sidecar/programmatic SetWindowPos.
        // Slot-less construction/teardown messages keep the default path.
        if (slot != nullptr) {
            const auto* suggested = reinterpret_cast<const RECT*>(lparam);
            RECT current{};
            if (slot->in_user_move() && suggested != nullptr && GetWindowRect(hwnd, &current)) {
                int outer_w = current.right - current.left;
                int outer_h = current.bottom - current.top;
                // On PMv1 the live rect can already be linearly scaled by the modal loop before
                // this handler runs. The bracket snapshots the pre-change extent, while WM_SIZING
                // refreshes it for a genuine border resize, so preserve the user's latest intent.
                slot->user_move_outer_extent(outer_w, outer_h);
                SelfApplyScope guard(slot);
                SetWindowPos(hwnd, nullptr, suggested->left, suggested->top, outer_w, outer_h,
                             SWP_NOZORDER | SWP_NOACTIVATE);
            }
            return 0;
        }
        break;
    case WM_GETMINMAXINFO: {
        // (cross-monitor maximize guard): cap this host window so its CLIENT can never exceed
        // the guest root (the guest-realizable extent). Win32 pre-fills the MINMAXINFO with the
        // defaults for the monitor the window is on; we REDUCE ptMaxSize (maximize) +
        // ptMaxTrackSize (drag- resize) per axis and leave ptMaxPosition untouched, so a maximize
        // on a LARGER secondary monitor caps at root size + stays at that monitor's top-left
        // (responsive, not wedged) -- the guest surface never exceeds its output. The cap is in
        // CLIENT px but the struct is in OUTER px, so convert with this HWND's measured live frame
        // extras. Do not predict them from GetDpiForWindow: under PMv1 that value changes across a
        // mixed-DPI boundary while the realized non-client chrome can remain 96-scaled. 0 root =
        // not reported yet -> no cap (fall through to DefWindowProcW).
        const std::uint64_t root = g_guest_root_packed.load(std::memory_order_acquire);
        if (root != 0 || (slot != nullptr && slot->display_layout() != nullptr)) {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lparam);
            const int root_w = root != 0 ? static_cast<int>(root >> 32) : 0;
            const int root_h = root != 0 ? static_cast<int>(root & 0xFFFFFFFFu) : 0;
            int extra_w = -1; // <0 => conversion failed -> the helper leaves the defaults uncapped
            int extra_h = -1;
            const FrameInsets insets = read_real_frame_insets(hwnd);
            if (insets.valid) {
                extra_w = insets.left + insets.right;
                extra_h = insets.top + insets.bottom;
            }
            if (slot != nullptr && slot->display_layout() != nullptr) {
                const display::MonitorDesc* monitor =
                    snapshot_monitor_for_window(*slot->display_layout(), hwnd);
                if (monitor != nullptr) {
                    // Snapshot work-area authority. Position is relative to the selected monitor's
                    // full bounds, matching WM_GETMINMAXINFO's secondary-monitor convention.
                    const display::MonitorWorkPlacement work =
                        display::monitor_work_placement(*monitor);
                    mmi->ptMaxPosition.x = work.position_x;
                    mmi->ptMaxPosition.y = work.position_y;
                    mmi->ptMaxSize.x = work.width;
                    mmi->ptMaxSize.y = work.height;
                }
            }
            if (root != 0) {
                const sidecar::MinMaxCap c = sidecar::cap_maxsize_to_client(
                    root_w, root_h, extra_w, extra_h, static_cast<int>(mmi->ptMaxSize.x),
                    static_cast<int>(mmi->ptMaxSize.y), static_cast<int>(mmi->ptMaxTrackSize.x),
                    static_cast<int>(mmi->ptMaxTrackSize.y));
                mmi->ptMaxSize.x = c.max_size_x;
                mmi->ptMaxSize.y = c.max_size_y;
                mmi->ptMaxTrackSize.x = c.max_track_x;
                mmi->ptMaxTrackSize.y = c.max_track_y;
            }
            return 0;
        }
        break;
    }
    case WM_SIZE:
        if (slot != nullptr) {
            // LOWORD/HIWORD(lParam) are the new CLIENT size. Only a real change vs the active
            // swapchain extent is a geometry mutation -- show-at-same-size and the worker's own
            // resize-to-extent must not strand the first present in OUT_OF_DATE.
            const int w = static_cast<int>(LOWORD(lparam));
            const int h = static_cast<int>(HIWORD(lparam));
            if (!slot->size_matches_swapchain(w, h)) {
                slot->mark_geometry_dirty("wm_size");
            }
            // A user MAXIMIZE / restore-from-maximize completed (WM_SYSCOMMAND set the
            // pending flag). Feed the ACTUAL final host geometry back to the guest via the SAME
            // reverse GeometryRequest path as a drag, so the registry/guest converge to what the
            // user made the host do (not a stale authored default). maybe_forward_user_geometry
            // carries the committed client origin + extent, so the sidecar authors the guest resize
            // too (user_request_is_resize -> the extent path). Only on MAXIMIZED/RESTORED
            // (a SIZE_MINIMIZED never carries the flag).
            if ((wparam == SIZE_MAXIMIZED || wparam == SIZE_RESTORED) &&
                slot->consume_pending_system_geometry()) {
                maybe_forward_user_geometry(*slot, hwnd);
            }
        }
        break;
    case WM_PAINT:
        // a placeholder with captured chrome paints its persistent BGRA buffer. A window
        // without aux pixels (a surface window) falls through to DefWindowProcW (system
        // background).
        if (slot != nullptr && slot->has_aux()) {
            PAINTSTRUCT ps;
            const HDC hdc = BeginPaint(hwnd, &ps);
            RECT client{};
            GetClientRect(hwnd, &client);
            slot->aux_blit(hdc, client);
            EndPaint(hwnd, &ps);
            return 0;
        }
        break;
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_MOUSEWHEEL:
    case WM_KEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYDOWN:
    case WM_SYSKEYUP:
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        // Capture -> queue, then fall through to DefWindowProcW for normal Win32 bookkeeping.
        if (slot != nullptr && slot->has_input_target()) {
            capture_input(*slot, hwnd, msg, wparam, lparam);
        }
        break;
    case WM_SETCURSOR:
        // apply the guest's cursor for the CLIENT area (the sidecar built it via
        // set_window_cursor). Returning TRUE stops DefWindowProcW from resetting it to the arrow.
        // Non-client areas (the caption/borders) fall through to the system cursor.
        if (slot != nullptr && slot->has_cursor() && LOWORD(lparam) == HTCLIENT) {
            slot->apply_cursor();
            return TRUE;
        }
        break;
    case WM_CLOSE:
        // Ask the GUEST to close (the sidecar sends the ICCCM WM_DELETE_WINDOW); the worker never
        // DestroyWindows here. Swallow so DefWindowProcW does not destroy the window (the guest
        // stays the lifecycle authority; its X-window teardown drives unregister).
        if (slot != nullptr && slot->has_input_target()) {
            sidecar::SidecarInputEvent ev;
            ev.kind = static_cast<std::uint32_t>(sidecar::InputEventKind::Close);
            slot->enqueue_input(ev);
        }
        return 0;
    case WM_ENTERSIZEMOVE:
        // a Win32-USER move/resize modal drag begins. Mark the bracket so the
        // WM_EXITSIZEMOVE below forwards only a genuine user gesture (a SetWindowPos never opens
        // this modal loop). Falls through to DefWindowProcW (the system runs the modal loop).
        if (slot != nullptr) {
            RECT frame{};
            if (GetWindowRect(hwnd, &frame)) {
                slot->begin_user_move(frame.right - frame.left, frame.bottom - frame.top);
            } else {
                slot->begin_user_move(0, 0);
            }
        }
        break;
    case WM_SIZING:
        // A real border resize can change the intended physical extent before crossing a DPI
        // boundary. Keep the latest sizing rectangle so WM_DPICHANGED preserves that user intent,
        // rather than reverting to the extent at WM_ENTERSIZEMOVE. Caption moves emit no WM_SIZING
        // and therefore retain their start extent exactly.
        if (slot != nullptr && slot->in_user_move() && lparam != 0) {
            const auto* sizing = reinterpret_cast<const RECT*>(lparam);
            slot->update_user_move_outer_extent(sizing->right - sizing->left,
                                                sizing->bottom - sizing->top);
        }
        break;
    case WM_EXITSIZEMOVE:
        // the user committed the move/resize -> forward the final frame as a guest
        // GeometryRequest (the reverse direction). Guarded against a worker-origin apply + no-op
        // churn inside the helper. Only if a matching WM_ENTERSIZEMOVE opened the gesture.
        if (slot != nullptr && slot->in_user_move()) {
            slot->end_user_move();
            maybe_forward_user_geometry(*slot, hwnd);
        }
        break;
    case WM_WINDOWPOSCHANGED: {
        // a NON-DRAG user/shell reposition -- Aero Snap (Win+Left/Right), or the snap
        // reposition that a drag-to-edge performs AFTER WM_EXITSIZEMOVE -- fires no
        // WM_ENTERSIZEMOVE bracket and is not an SC_MAXIMIZE/SC_RESTORE, so it slipped through both
        // existing forward triggers and the guest's registry geometry went stale (input then mapped
        // to the pre-snap origin). WM_WINDOWPOSCHANGED fires once per realized position/size
        // change, after realization, so the readback in maybe_forward_user_geometry is final.
        // Forward it, gated so it neither storms, echoes, nor double-forwards a minimize/maximize:
        //   * ONLY an in-NORMAL-state move -- the placement state (normal/iconic/zoomed) must be
        //     normal AND unchanged since the last WM_WINDOWPOSCHANGED. A state TRANSITION
        //     (minimize/restore/maximize, and the first placement from unknown) is owned by the
        //     WM_SYSCOMMAND path (it forwards Minimize/Restore or sets the maximize pending flag),
        //     so forwarding a Geometry for it would duplicate/garble it (a minimized window's rect
        //     is bogus). This is the robust gate -- no window-size or undocumented-flag dependence.
        //   * skip a z-order-only / no-geometry change (SWP_NOMOVE && SWP_NOSIZE);
        //   * only OUTSIDE a drag (in_user_move) -- during a drag this fires every step and
        //     WM_EXITSIZEMOVE owns the commit (drag path unchanged);
        //   * the helper's own guards do the rest -- applying_self() drops a worker-origin
        //   apply/echo
        //     (apply_geometry/drain_geometry/set_client_extent run under SelfApplyScope), and the
        //     no-op / authored-churn / no-input-target guards drop anything that did not actually
        //     move the realized client. Falls through to DefWindowProcW (which derives
        //     WM_SIZE/MOVE).
        if (slot != nullptr) {
            const int cur = IsIconic(hwnd) ? 1 : (IsZoomed(hwnd) ? 2 : 0);
            const int prev = slot->last_pos_state();
            slot->set_last_pos_state(cur);
            const auto* wp = reinterpret_cast<const WINDOWPOS*>(lparam);
            const bool geometry_change = wp == nullptr || (wp->flags & (SWP_NOMOVE | SWP_NOSIZE)) !=
                                                              (SWP_NOMOVE | SWP_NOSIZE);
            if (cur == 0 && prev == 0 && geometry_change && !slot->in_user_move()) {
                const ForwardResult fr = maybe_forward_user_geometry(*slot, hwnd);
                if (g_geom_trace) {
                    const RootClientGeometry g =
                        read_real_root_client_geometry(hwnd, WindowThread::host_origin());
                    VKR_INFO(kComponent)
                        << "geom-trace WM_WINDOWPOSCHANGED realized root=" << g.root_x << ","
                        << g.root_y << " client=" << g.client_w << "x" << g.client_h << " -> "
                        << forward_result_name(fr);
                }
            }
        }
        break;
    }
    case WM_SYSCOMMAND:
        // Handle a Win32-USER minimize/restore/MAXIMIZE (the caption box, the
        // taskbar, Win+D, the system menu). Minimize/restore-from-iconic forward a guest VISIBILITY
        // request (the sidecar authors Iconic/Visible). Maximize + restore-from-maximize are
        // GEOMETRY changes whose final rect is only known at the resulting WM_SIZE, so they set a
        // pending flag that WM_SIZE consumes to feed the ACTUAL host geometry back (so the
        // authored geometry then tracks the maximize instead of a stale default snapping the window
        // off-screen). Always FALLS THROUGH to DefWindowProcW so the host acts IMMEDIATELY and the
        // guest follows. Guarded against a worker-origin apply (apply_visibility/apply_geometry run
        // under SelfApplyScope) + no input target. wParam's low 4 bits are reserved by Win32, so
        // mask with 0xFFF0.
        if (slot != nullptr && !slot->applying_self() && slot->has_input_target()) {
            const WPARAM sc = wparam & 0xFFF0;
            if (sc == SC_MINIMIZE) {
                sidecar::SidecarInputEvent ev;
                ev.kind = static_cast<std::uint32_t>(sidecar::InputEventKind::GeometryRequest);
                ev.host_request = static_cast<std::uint32_t>(sidecar::HostRequestKind::Minimize);
                slot->enqueue_input(ev);
            } else if (sc == SC_RESTORE) {
                // Restore-from-ICONIC (un-minimize) is a visibility change -> forward Restore
                // (unchanged). Restore-from-ZOOMED (un-maximize) is a GEOMETRY change ->
                // defer to the resulting WM_SIZE(SIZE_RESTORED), which feeds the new normal rect
                // back. IsIconic/IsZoomed read the state BEFORE DefWindowProcW performs the
                // restore.
                if (IsIconic(hwnd)) {
                    sidecar::SidecarInputEvent ev;
                    ev.kind = static_cast<std::uint32_t>(sidecar::InputEventKind::GeometryRequest);
                    ev.host_request = static_cast<std::uint32_t>(sidecar::HostRequestKind::Restore);
                    slot->enqueue_input(ev);
                } else if (IsZoomed(hwnd)) {
                    slot->set_pending_system_geometry(); // un-maximize -> geometry feedback
                }
            } else if (sc == SC_MAXIMIZE) {
                slot->set_pending_system_geometry(); // maximize -> geometry feedback
            }
        }
        break;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

} // namespace

bool WindowSlot::aux_update(const unsigned char* src, int src_w, int src_h, int dx, int dy, int dw,
                            int dh, int stride) {
    if (src == nullptr || src_w <= 0 || src_h <= 0 || dw <= 0 || dh <= 0) {
        return false;
    }
    if (aux_w_ != src_w || aux_h_ != src_h) {
        aux_pixels_.assign(static_cast<std::size_t>(src_w) * src_h * 4, 0);
        aux_w_ = src_w;
        aux_h_ = src_h;
    }
    const std::size_t dst_stride = static_cast<std::size_t>(src_w) * 4;
    for (int row = 0; row < dh; ++row) {
        const std::size_t src_off = static_cast<std::size_t>(row) * stride;
        const std::size_t dst_off =
            static_cast<std::size_t>(dy + row) * dst_stride + static_cast<std::size_t>(dx) * 4;
        std::memcpy(aux_pixels_.data() + dst_off, src + src_off, static_cast<std::size_t>(dw) * 4);
    }
    return true;
}

void WindowSlot::aux_blit(HDC hdc, const RECT& client) const {
    if (aux_pixels_.empty() || aux_w_ <= 0 || aux_h_ <= 0) {
        return;
    }
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = aux_w_;
    bmi.bmiHeader.biHeight = -aux_h_; // negative => top-down (our buffer is top-down BGRA)
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    const int cw = static_cast<int>(client.right - client.left);
    const int ch = static_cast<int>(client.bottom - client.top);
    if (cw <= 0 || ch <= 0) {
        return;
    }
    StretchDIBits(hdc, 0, 0, cw, ch, 0, 0, aux_w_, aux_h_, aux_pixels_.data(), &bmi, DIB_RGB_COLORS,
                  SRCCOPY);
}

bool WindowSlot::aux_sample(int x, int y, std::uint32_t& out_bgra) const {
    if (aux_pixels_.empty() || x < 0 || y < 0 || x >= aux_w_ || y >= aux_h_) {
        return false;
    }
    const std::size_t off =
        (static_cast<std::size_t>(y) * aux_w_ + static_cast<std::size_t>(x)) * 4;
    const unsigned char* p = aux_pixels_.data() + off;
    out_bgra = static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
               (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
    return true;
}

bool WindowSlot::aux_capture(int& out_w, int& out_h, std::vector<unsigned char>& out_bgra) const {
    if (aux_pixels_.empty() || aux_w_ <= 0 || aux_h_ <= 0) {
        return false;
    }
    out_w = aux_w_;
    out_h = aux_h_;
    out_bgra = aux_pixels_; // top-down BGRA8, stride == aux_w_*4
    return true;
}

// Build a 32-bit alpha HCURSOR from top-down BGRA8 pixels (w*h*4) with hotspot (xhot,yhot). The
// color bitmap is a 32-bpp top-down DIB (BI_RGB == BGRA byte order, matching our
// buffer); the AND mask is an all-zero monochrome bitmap (CreateIconIndirect alpha-blends via the
// color alpha). Returns nullptr on failure.
HCURSOR build_cursor(const unsigned char* bgra, int w, int h, int xhot, int yhot) {
    if (bgra == nullptr || w <= 0 || h <= 0) {
        return nullptr;
    }
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h; // negative => top-down (our buffer is top-down BGRA)
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    const HDC hdc = GetDC(nullptr);
    const HBITMAP color = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, hdc);
    if (color == nullptr || bits == nullptr) {
        if (color != nullptr) {
            DeleteObject(color);
        }
        return nullptr;
    }
    std::memcpy(bits, bgra, static_cast<std::size_t>(w) * h * 4);
    const HBITMAP mask = CreateBitmap(w, h, 1, 1, nullptr); // all-zero AND mask
    if (mask == nullptr) {
        DeleteObject(color);
        return nullptr;
    }
    ICONINFO ii{};
    ii.fIcon = FALSE; // a cursor, not an icon
    ii.xHotspot = static_cast<DWORD>(xhot < 0 ? 0 : xhot);
    ii.yHotspot = static_cast<DWORD>(yhot < 0 ? 0 : yhot);
    ii.hbmMask = mask;
    ii.hbmColor = color;
    const HCURSOR hc = reinterpret_cast<HCURSOR>(CreateIconIndirect(&ii));
    DeleteObject(color);
    DeleteObject(mask);
    return hc;
}

WindowSlot::~WindowSlot() {
    if (cursor_ != nullptr) {
        DestroyCursor(cursor_); // the window is gone by now -> never the displayed cursor
    }
}

bool WindowSlot::set_cursor(const unsigned char* src, int w, int h, int xhot, int yhot,
                            bool apply_now) {
    if (src == nullptr || w <= 0 || h <= 0) {
        return false;
    }
    const HCURSOR fresh = build_cursor(src, w, h, xhot, yhot);
    if (fresh == nullptr) {
        return false;
    }
    // Safe retire (disposition): when the pointer is in this window's
    // client area (apply_now), make the new cursor the displayed one BEFORE destroying the old, so
    // the old is never the displayed cursor when DestroyCursor runs. When the pointer is elsewhere,
    // the old is not displayed anyway, so we do NOT SetCursor (which would briefly change the
    // global cursor over an unrelated window) -- the new one applies on the next WM_SETCURSOR.
    if (apply_now) {
        SetCursor(fresh);
    }
    const HCURSOR old = cursor_;
    cursor_ = fresh;
    cursor_bgra_.assign(src, src + static_cast<std::size_t>(w) * h * 4);
    cursor_w_ = w;
    cursor_h_ = h;
    cursor_xhot_ = xhot;
    cursor_yhot_ = yhot;
    if (old != nullptr) {
        DestroyCursor(old);
    }
    return true;
}

bool WindowSlot::clear_cursor(bool apply_default) {
    if (cursor_ == nullptr) {
        return false;
    }
    // Safe retire (mirrors set_cursor): when the pointer is in this window's client area the old
    // cursor is the displayed one, so restore the system arrow BEFORE destroying it -- the stale
    // custom cursor is then never the displayed cursor when DestroyCursor runs. When the pointer is
    // elsewhere the old is not displayed, so no SetCursor (which would touch the global cursor).
    if (apply_default) {
        SetCursor(LoadCursorW(nullptr, IDC_ARROW));
    }
    DestroyCursor(cursor_);
    cursor_ = nullptr;
    cursor_bgra_.clear();
    cursor_w_ = 0;
    cursor_h_ = 0;
    cursor_xhot_ = 0;
    cursor_yhot_ = 0;
    return true;
}

void WindowSlot::apply_cursor() const {
    if (cursor_ != nullptr) {
        SetCursor(cursor_);
    }
}

void WindowSlot::cursor_info(int& w, int& h, int& xhot, int& yhot) const {
    w = cursor_w_;
    h = cursor_h_;
    xhot = cursor_xhot_;
    yhot = cursor_yhot_;
}

bool WindowSlot::cursor_sample(int x, int y, std::uint32_t& out_bgra) const {
    if (cursor_bgra_.empty() || x < 0 || y < 0 || x >= cursor_w_ || y >= cursor_h_) {
        return false;
    }
    const std::size_t off =
        (static_cast<std::size_t>(y) * cursor_w_ + static_cast<std::size_t>(x)) * 4;
    const unsigned char* p = cursor_bgra_.data() + off;
    out_bgra = static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
               (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
    return true;
}

bool WindowSlot::cursor_capture(int& out_w, int& out_h, int& out_xhot, int& out_yhot,
                                std::vector<unsigned char>& out_bgra) const {
    if (cursor_ == nullptr || cursor_bgra_.empty() || cursor_w_ <= 0 || cursor_h_ <= 0) {
        return false;
    }
    out_w = cursor_w_;
    out_h = cursor_h_;
    out_xhot = cursor_xhot_;
    out_yhot = cursor_yhot_;
    out_bgra = cursor_bgra_; // top-down BGRA8, stride == cursor_w_*4
    return true;
}

void WindowSlot::mark_geometry_dirty(const char* reason) {
    geometry_dirty_.store(true, std::memory_order_release);
    VKR_INFO(kComponent) << "geometry dirty (" << (reason ? reason : "?") << ")";
}

void WindowSlot::set_swapchain_extent(int width, int height) {
    swapchain_extent_.store(pack_extent(width, height), std::memory_order_release);
}

bool WindowSlot::size_matches_swapchain(int width, int height) const {
    const std::uint64_t cur = swapchain_extent_.load(std::memory_order_acquire);
    if (cur == 0) {
        return true; // no active swapchain: nothing to invalidate
    }
    return cur == pack_extent(width, height);
}

void WindowSlot::note_display_configuration_change(const char* reason) {
    if (display_restart_required_ == nullptr || display_layout_ == nullptr ||
        display_restart_required_->exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    // Detection-only live metrics are logged for diagnosis and never become a placement,
    // transform, work-area, or root-size authority. The old transform remains stable beneath live
    // swapchains until the user starts a new graphical session.
    const int new_x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int new_y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int new_w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int new_h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    const display::RectI32& old = display_layout_->virtual_bounds;
    VKR_ERROR(kComponent) << "display configuration changed (" << (reason ? reason : "unknown")
                          << "); session snapshot=" << display_layout_->snapshot_id
                          << " old=" << old.x << "," << old.y << " " << old.width << "x"
                          << old.height << " observed=" << new_x << "," << new_y << " " << new_w
                          << "x" << new_h
                          << "; keeping the pinned layout -- restart this vkrelay2 graphical "
                             "session";
}

WindowThread::WindowThread(const display::DisplayLayout* display_layout) {
    if (display_layout != nullptr) {
        display_layout_ = *display_layout;
        has_display_layout_ = true;
    }
    static std::atomic<unsigned> counter{0};
    class_name_ = L"vkrelay2_worker_win_" + std::to_wstring(counter.fetch_add(1));
    thread_ = std::thread([this]() { run(); });
    std::unique_lock<std::mutex> lk(mu_);
    cv_.wait(lk, [this]() { return ready_; });
}

void WindowThread::set_guest_root(std::uint32_t width, std::uint32_t height) {
    // Publish the session's guest root for the WndProc's WM_GETMINMAXINFO cap. Release so a WndProc
    // on the window thread observes a fully-written value. Static: no `this` / no thread required.
    g_guest_root_packed.store(pack_extent(static_cast<int>(width), static_cast<int>(height)),
                              std::memory_order_release);
}

std::uint64_t WindowThread::guest_root_packed() {
    return g_guest_root_packed.load(std::memory_order_acquire);
}

void WindowThread::set_host_origin(std::int32_t x, std::int32_t y) {
    const std::uint64_t packed = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) << 32) |
                                 static_cast<std::uint32_t>(y);
    g_host_origin_packed.store(packed, std::memory_order_release);
}

POINT WindowThread::host_origin() {
    const std::uint64_t packed = g_host_origin_packed.load(std::memory_order_acquire);
    return POINT{static_cast<LONG>(static_cast<std::int32_t>(packed >> 32)),
                 static_cast<LONG>(static_cast<std::int32_t>(packed & 0xffffffffu))};
}

WindowThread::~WindowThread() {
    if (thread_.joinable()) {
        // If the post fails the thread has already exited, so join() still returns;
        // log rather than risk a silent stall.
        if (!PostThreadMessageW(thread_id_, WM_QUIT, 0, 0)) {
            VKR_WARN(kComponent) << "failed to post WM_QUIT to window thread";
        }
        thread_.join();
    }
}

void WindowThread::run() {
    thread_id_ = GetCurrentThreadId();
    // Make the window thread per-monitor-DPI-aware (PMv1) BEFORE any window class
    // or window is created here, so every HWND operation on this thread (CreateWindowExW,
    // SetWindowPos, GetClientRect, the surface caps query) is in PHYSICAL pixels -- the app's
    // chosen extent is realized 1:1 (crisp) instead of being rendered at a virtualized size and
    // upscaled by Windows on a scaled panel. PMv1 (not PMv2) deliberately does NOT auto-resize the
    // window on a monitor-DPI boundary crossing (PMv2 does, which races vkCreateSwapchainKHR and
    // has AV'd the NVIDIA driver); cross-monitor geometry stays under our latch / the sidecar
    // geometry authority. Failure is non-fatal (an old Windows without per-monitor support keeps
    // the DPI-unaware behavior).
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE);

    // Force the thread message queue to exist before anyone PostThreadMessage's us.
    MSG msg;
    PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = window_proc; // WM_SIZE drives the latch
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = class_name_.c_str();
    RegisterClassExW(&wc); // a redundant registration is harmless (per-instance name)

    if (has_display_layout_) {
        // Keep one invisible top-level recipient alive for the full worker session. HWND_MESSAGE
        // windows are excluded from broadcast messages, and relying only on app HWNDs would miss a
        // topology change between session creation and the first surface/toplevel.
        display_observer_ = CreateWindowExW(
            WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, class_name_.c_str(), L"vkrelay2 display observer",
            WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (display_observer_ != nullptr) {
            auto slot = std::make_unique<WindowSlot>(&display_layout_, &display_restart_required_);
            SetWindowLongPtrW(display_observer_, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(slot.get()));
            slots_.emplace(display_observer_, std::move(slot));
        } else {
            VKR_WARN(kComponent) << "could not create the display-change observer window";
        }
    }

    {
        std::lock_guard<std::mutex> lk(mu_);
        ready_ = true;
    }
    cv_.notify_all();

    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == kRunTasks) {
            std::deque<std::function<void()>> pending;
            {
                std::lock_guard<std::mutex> lk(mu_);
                pending.swap(tasks_);
            }
            for (auto& task : pending) {
                task();
            }
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (display_observer_ != nullptr) {
        SetWindowLongPtrW(display_observer_, GWLP_USERDATA, 0);
        DestroyWindow(display_observer_);
        slots_.erase(display_observer_);
        display_observer_ = nullptr;
    }
    UnregisterClassW(class_name_.c_str(), GetModuleHandleW(nullptr));
}

bool WindowThread::run_on_thread(const std::function<void()>& fn) {
    auto task = std::make_shared<std::packaged_task<void()>>(fn);
    std::future<void> done = task->get_future();
    {
        std::lock_guard<std::mutex> lk(mu_);
        tasks_.push_back([task]() { (*task)(); });
    }
    if (!PostThreadMessageW(thread_id_, kRunTasks, 0, 0)) {
        // The pump can't be woken (thread gone / queue full). Drop the task we just
        // queued -- the backend serves one caller at a time, so it is the last one --
        // so a stale wake cannot run it against an out-of-scope caller frame, and fail
        // instead of waiting forever.
        std::lock_guard<std::mutex> lk(mu_);
        if (!tasks_.empty()) {
            tasks_.pop_back();
        }
        VKR_WARN(kComponent) << "window thread wake failed; task abandoned";
        return false;
    }
    done.wait();
    return true;
}

void WindowThread::post_on_thread(std::function<void()> fn) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        tasks_.push_back(std::move(fn));
    }
    if (!PostThreadMessageW(thread_id_, kRunTasks, 0, 0)) {
        // The pump can't be woken (thread gone). Drop the task we just queued so a stale wake
        // cannot run it later; a geometry drain so abandoned just means the window keeps its last
        // position (the next authored move re-posts). No blocking wait here (the caller is the pump
        // itself).
        std::lock_guard<std::mutex> lk(mu_);
        if (!tasks_.empty()) {
            tasks_.pop_back();
        }
        VKR_WARN(kComponent) << "window thread wake failed; geometry drain abandoned";
    }
}

CreatedWindow WindowThread::create_hidden_window(int width, int height) {
    CreatedWindow created;
    const int w = width > 0 ? width : 256;
    const int h = height > 0 ? height : 256;
    run_on_thread([&]() {
        // Size the OUTER window so the client area is at least w x h; the window starts
        // hidden (shown on first present), so position/decoration don't matter yet. DPI-correct
        // non-client metrics (the thread is per-monitor-aware): use the primary monitor's DPI (the
        // window is placed there by CW_USEDEFAULT). The exact initial size is approximate -- it is
        // resized to the app's chosen extent by set_client_extent before any swapchain.
        RECT r{0, 0, w, h};
        AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, GetDpiForSystem());
        HWND hwnd =
            CreateWindowExW(0, class_name_.c_str(), L"vkrelay2", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                            CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, nullptr, nullptr,
                            GetModuleHandleW(nullptr), nullptr);
        if (hwnd == nullptr) {
            return;
        }
        auto slot = std::make_unique<WindowSlot>(has_display_layout_ ? &display_layout_ : nullptr,
                                                 &display_restart_required_);
        WindowSlot* slot_ptr = slot.get();
        // Stash the slot on the HWND so window_proc reaches it; the map owns its lifetime.
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(slot_ptr));
        // seed the placement-state gate explicitly at attach (the window is
        // hidden + normal here) instead of relying on a later self-apply SetWindowPos to dispatch
        // the first WM_WINDOWPOSCHANGED -- else a path that binds input without a later position
        // mutation would drop its first real snap as an unknown->normal transition.
        slot_ptr->set_last_pos_state(IsIconic(hwnd) ? 1 : (IsZoomed(hwnd) ? 2 : 0));
        slots_.emplace(hwnd, std::move(slot));
        created.hwnd = hwnd;
        created.slot = slot_ptr;
    });
    return created;
}

CreatedWindow WindowThread::create_popup_window(HWND owner, int x, int y, int width, int height) {
    CreatedWindow created;
    if (owner == nullptr) {
        return created; // a popup must anchor to its owner's host window (the z-order anchor)
    }
    const int w = width > 0 ? width : 1;
    const int h = height > 0 ? height : 1;
    run_on_thread([&]() {
        // Owned WS_POPUP host: WS_POPUP has no caption/border, so the window rect IS
        // the client rect -- pass the reported X root x/y + physical size verbatim (no
        // AdjustWindowRect; the thread is per-monitor-aware and the model is 1:1 physical). The
        // owner makes it stack ABOVE the owner toplevel + reap with it (the static z-order);
        // WS_EX_TOOLWINDOW keeps it off the taskbar; WS_EX_NOACTIVATE so clicking the popup never
        // steals the owner's activation/focus. It starts hidden (shown on the first chrome
        // paint commit).
        HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, class_name_.c_str(),
                                    L"vkrelay2", WS_POPUP, x, y, w, h, owner, nullptr,
                                    GetModuleHandleW(nullptr), nullptr);
        if (hwnd == nullptr) {
            return;
        }
        auto slot = std::make_unique<WindowSlot>(has_display_layout_ ? &display_layout_ : nullptr,
                                                 &display_restart_required_);
        WindowSlot* slot_ptr = slot.get();
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(slot_ptr));
        // seed the placement-state gate explicitly at attach (the window is
        // hidden + normal here) instead of relying on a later self-apply SetWindowPos to dispatch
        // the first WM_WINDOWPOSCHANGED -- else a path that binds input without a later position
        // mutation would drop its first real snap as an unknown->normal transition.
        slot_ptr->set_last_pos_state(IsIconic(hwnd) ? 1 : (IsZoomed(hwnd) ? 2 : 0));
        slots_.emplace(hwnd, std::move(slot));
        created.hwnd = hwnd;
        created.slot = slot_ptr;
    });
    return created;
}

void WindowThread::destroy_window(HWND hwnd) {
    if (hwnd == nullptr) {
        return;
    }
    run_on_thread([&]() {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        DestroyWindow(hwnd);
        slots_.erase(hwnd);
    });
}

void WindowThread::show_window(HWND hwnd) {
    if (hwnd == nullptr) {
        return;
    }
    run_on_thread([&]() {
        // Reveal without stealing focus / activation (the app's real toplevel, when it
        // exists, owns focus). A no-size-change here is filtered out by the latch. A
        // worker-origin show -- guarded so its WM_* never forwards as a user gesture.
        const auto it = slots_.find(hwnd);
        SelfApplyScope guard(it != slots_.end() ? it->second.get() : nullptr);
        ShowWindow(hwnd, SW_SHOWNA);
        SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    });
}

bool WindowThread::paint_aux(HWND hwnd, const unsigned char* src, int src_w, int src_h, int dx,
                             int dy, int dw, int dh, int stride) {
    if (hwnd == nullptr || src == nullptr) {
        return false;
    }
    bool painted = false;
    const bool ran = run_on_thread([&]() {
        const auto it = slots_.find(hwnd);
        if (it == slots_.end()) {
            return;
        }
        if (!it->second->aux_update(src, src_w, src_h, dx, dy, dw, dh, stride)) {
            return;
        }
        // Stage the pixels WITHOUT showing: invalidate the whole client (the blit stretches the
        // whole buffer); UpdateWindow repaints if the window is already visible, and is a no-op
        // while hidden (a hidden window gets no WM_PAINT), so the placeholder stays hidden until
        // show_aux_window.
        InvalidateRect(hwnd, nullptr, FALSE);
        UpdateWindow(hwnd);
        painted = true;
    });
    return ran && painted;
}

void WindowThread::show_aux_window(HWND hwnd) {
    if (hwnd == nullptr) {
        return;
    }
    run_on_thread([&]() {
        // Reveal without stealing focus, then force the staged WM_PAINT so the DIB chrome is
        // on-screen before this returns (the buffer was already filled by paint_aux, so there is no
        // gray flash). A worker-origin reveal is guarded.
        const auto it = slots_.find(hwnd);
        SelfApplyScope guard(it != slots_.end() ? it->second.get() : nullptr);
        ShowWindow(hwnd, SW_SHOWNA);
        SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        InvalidateRect(hwnd, nullptr, FALSE);
        UpdateWindow(hwnd);
    });
}

bool WindowThread::sample_aux_pixel(HWND hwnd, int x, int y, std::uint32_t& out_bgra) {
    if (hwnd == nullptr) {
        return false;
    }
    bool ok = false;
    const bool ran = run_on_thread([&]() {
        const auto it = slots_.find(hwnd);
        if (it != slots_.end()) {
            ok = it->second->aux_sample(x, y, out_bgra);
        }
    });
    return ran && ok;
}

void WindowThread::set_slot_input_target(HWND hwnd, sidecar::InputQueue* queue, std::uint64_t xid,
                                         std::uint64_t epoch) {
    if (hwnd == nullptr) {
        return;
    }
    run_on_thread([&]() {
        const auto it = slots_.find(hwnd);
        if (it != slots_.end()) {
            it->second->set_input_target(queue, xid, epoch);
        }
    });
}

bool WindowThread::set_window_cursor(HWND hwnd, const unsigned char* bgra, int w, int h, int xhot,
                                     int yhot) {
    if (hwnd == nullptr || bgra == nullptr) {
        return false;
    }
    bool ok = false;
    const bool ran = run_on_thread([&]() {
        const auto it = slots_.find(hwnd);
        if (it == slots_.end()) {
            return;
        }
        // Apply the cursor immediately only if the pointer is actually in this window's client area
        // (else SetCursor would briefly change the global cursor over an unrelated window); the
        // safe retire keys on the same condition.
        bool apply_now = false;
        POINT pt;
        RECT rc;
        if (GetCursorPos(&pt) && GetClientRect(hwnd, &rc)) {
            POINT cp = pt;
            ScreenToClient(hwnd, &cp);
            apply_now = PtInRect(&rc, cp) != FALSE;
        }
        ok = it->second->set_cursor(bgra, w, h, xhot, yhot, apply_now);
    });
    return ran && ok;
}

bool WindowThread::clear_window_cursor(HWND hwnd) {
    if (hwnd == nullptr) {
        return false;
    }
    bool cleared = false;
    const bool ran = run_on_thread([&]() {
        const auto it = slots_.find(hwnd);
        if (it == slots_.end()) {
            return;
        }
        // Restore the arrow only if the pointer is actually in this window's client area (the same
        // condition as set_window_cursor's apply_now) -- where the stale cursor is actually shown.
        bool apply_default = false;
        POINT pt;
        RECT rc;
        if (GetCursorPos(&pt) && GetClientRect(hwnd, &rc)) {
            POINT cp = pt;
            ScreenToClient(hwnd, &cp);
            apply_default = PtInRect(&rc, cp) != FALSE;
        }
        cleared = it->second->clear_cursor(apply_default);
    });
    return ran && cleared;
}

bool WindowThread::debug_cursor(HWND hwnd, int sample_x, int sample_y, int& out_w, int& out_h,
                                int& out_xhot, int& out_yhot, bool& out_has_pixel,
                                std::uint32_t& out_bgra) {
    if (hwnd == nullptr) {
        return false;
    }
    bool has = false;
    const bool ran = run_on_thread([&]() {
        const auto it = slots_.find(hwnd);
        if (it != slots_.end() && it->second->has_cursor()) {
            it->second->cursor_info(out_w, out_h, out_xhot, out_yhot);
            out_has_pixel = it->second->cursor_sample(sample_x, sample_y, out_bgra);
            has = true;
        }
    });
    return ran && has;
}

bool WindowThread::debug_capture_chrome(HWND hwnd, int& out_w, int& out_h, int& out_stride,
                                        std::vector<unsigned char>& out_bgra) {
    if (hwnd == nullptr) {
        return false;
    }
    bool got = false;
    const bool ran = run_on_thread([&]() {
        const auto it = slots_.find(hwnd);
        if (it != slots_.end() && it->second->aux_capture(out_w, out_h, out_bgra)) {
            out_stride = out_w * 4;
            got = true;
        }
    });
    return ran && got;
}

bool WindowThread::debug_capture_cursor(HWND hwnd, int& out_w, int& out_h, int& out_xhot,
                                        int& out_yhot, int& out_stride,
                                        std::vector<unsigned char>& out_bgra) {
    if (hwnd == nullptr) {
        return false;
    }
    bool got = false;
    const bool ran = run_on_thread([&]() {
        const auto it = slots_.find(hwnd);
        if (it != slots_.end() &&
            it->second->cursor_capture(out_w, out_h, out_xhot, out_yhot, out_bgra)) {
            out_stride = out_w * 4;
            got = true;
        }
    });
    return ran && got;
}

void WindowThread::set_window_title_tag(HWND hwnd, std::uint64_t xid) {
    if (hwnd == nullptr) {
        return;
    }
    char title[64];
    std::snprintf(title, sizeof(title), "vkrelay2 [xid=0x%llx]",
                  static_cast<unsigned long long>(xid));
    // ASCII title on a Unicode window: SetWindowTextA converts internally (the content is ASCII).
    run_on_thread([&]() { SetWindowTextA(hwnd, title); });
}

WindowThread::ClientExtent WindowThread::set_client_extent(HWND hwnd, int width, int height) {
    ClientExtent actual;
    if (hwnd == nullptr || width <= 0 || height <= 0) {
        return actual;
    }
    run_on_thread([&]() {
        // Size the OUTER window so the CLIENT area is width x height (the app's chosen extent
        // becomes the host window's client). Use the same measured live non-client authority as
        // movement + WM_GETMINMAXINFO; a DPI-predicted adjustment is wrong for PMv1 chrome that
        // remains 96-scaled after GetDpiForWindow flips to 144.
        const FrameInsets insets = read_real_frame_insets(hwnd);
        if (!insets.valid) {
            return;
        }
        const int outer_w = width + insets.left + insets.right;
        const int outer_h = height + insets.top + insets.bottom;
        // a worker-origin resize (app/swapchain-driven) -- guard the forward path. The
        // WM_SIZE this trips STILL drives the dirty latch (the guard gates ONLY the reverse
        // enqueue).
        const auto it = slots_.find(hwnd);
        SelfApplyScope guard(it != slots_.end() ? it->second.get() : nullptr);
        SetWindowPos(hwnd, nullptr, 0, 0, outer_w, outer_h,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        // Read back what the window actually became -- Win32 may clamp (caption-min width,
        // work-area).
        RECT client{};
        if (GetClientRect(hwnd, &client)) {
            actual.width = static_cast<int>(client.right - client.left);
            actual.height = static_cast<int>(client.bottom - client.top);
        }
    });
    return actual;
}

void WindowThread::apply_geometry(HWND hwnd, int root_x, int root_y, int client_w, int client_h,
                                  std::uint64_t host_apply_seq, bool apply_size,
                                  sidecar::ZOrder z_order) {
    if (hwnd == nullptr) {
        return;
    }
    // Record the latest desired geometry in the slot (coalescing), then drain it -- both on the
    // window thread (the slot's geometry cell is window-thread-only). The drain may repost itself
    // if the slot is busy, so this blocking call returns after the FIRST attempt (the apply is
    // idempotent + seq-gated; a deferred repost converges to the latest desired).
    run_on_thread(
        [this, hwnd, root_x, root_y, client_w, client_h, host_apply_seq, apply_size, z_order]() {
            const auto it = slots_.find(hwnd);
            if (it == slots_.end()) {
                return; // window torn down before the apply landed
            }
            it->second->set_desired_geometry(root_x, root_y, client_w, client_h, host_apply_seq,
                                             apply_size, z_order);
            drain_geometry(hwnd);
        });
}

void WindowThread::drain_geometry(HWND hwnd) {
    const auto it = slots_.find(hwnd);
    if (it == slots_.end()) {
        return; // window torn down between the post and the drain
    }
    WindowSlot& slot = *it->second;
    if (!slot.has_pending_geometry()) {
        return; // a newer drain already applied the latest desired (seq gate)
    }
    // take the slot lock NON-BLOCKING. A present/acquire on the session thread holds it
    // blocking AND may be mid-invoke to this window thread, so a blocking lock here would deadlock.
    // If busy, repost the drain so the pump proceeds to the present and we converge once the slot
    // frees. The slot is uncontended for placeholder/popup hosts (nothing else locks them) -- the
    // try_lock is then a free no-op.
    std::unique_lock<std::mutex> lk(slot.slot_lock(), std::try_to_lock);
    if (!lk.owns_lock()) {
        post_on_thread([this, hwnd]() { drain_geometry(hwnd); });
        return;
    }
    int rx = 0, ry = 0, cw = 0, ch = 0;
    std::uint64_t seq = 0;
    bool apply_size = false;
    sidecar::ZOrder z_order = sidecar::ZOrder::None;
    slot.desired_geometry(rx, ry, cw, ch, seq, apply_size, z_order);
    // if this desired geometry is the exact sidecar echo of our prior realization feedback,
    // and the HWND is still at that realized geometry, ACK it without running the fallible
    // client->frame transform again. Re-applying is what turned a stable PMv1 inset mismatch into
    // an unbounded (-3,-14)-per-echo walk. The one-shot token proves provenance; a different
    // desired geometry or a host move invalidates it. A z-order intent still needs a z-only
    // SetWindowPos below.
    const RootClientGeometry live_before = read_real_root_client_geometry(hwnd, host_origin());
    const bool adopt_realization = slot.consume_pending_realization_adoption(
        rx, ry, cw, ch, apply_size, live_before.root_x, live_before.root_y, live_before.client_w,
        live_before.client_h);
    if (adopt_realization) {
        slot.set_realized_geometry(live_before.root_x, live_before.root_y, live_before.client_w,
                                   live_before.client_h);
        if (z_order == sidecar::ZOrder::None) {
            slot.mark_geometry_applied(seq);
            return;
        }
    }
    // Compute the outer frame so the CLIENT lands at the mapped X-root client origin (chrome-
    // correct). Actual frame/client insets are measured on the window thread; the pinned virtual
    // origin is the 1:1 mapping base.
    const POINT work = host_origin();
    const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE));
    const FrameInsets insets = read_real_frame_insets(hwnd);
    if (!insets.valid) {
        VKR_WARN(kComponent) << "geometry apply skipped: unable to measure live frame insets";
        return;
    }
    const RECT outer = map_root_client_to_win32_frame_with_insets(rx, ry, cw, ch, work, insets);
    // safety clamp (initial placement ONLY): keep the OUTER frame fully inside this monitor's
    // work area, so a centered/registered window can never appear with its chrome off-screen even
    // when the X root != the Win32 work area. Runs once (the first apply); a later user/app move
    // must win, so it is NOT clamped. The normal centered case already fits -> a no-op (registry
    // x/y == actual).
    int place_left = outer.left;
    int place_top = outer.top;
    // Toplevels only: a WS_POPUP (owned menu/popup) is positioned relative to its owner,
    // not centered, so it keeps its own placement.
    if (!adopt_realization && (style & WS_POPUP) == 0 && slot.consume_initial_placement()) {
        if (has_display_layout_) {
            // choose from the authored client rectangle, then clamp the OUTER frame using only
            // the immutable snapshot. A bigger-than-monitor axis falls back to the virtual desktop
            // so a root-sized app is not stranded at the small primary's corner.
            display::RectI32 client_host;
            if (!display::guest_to_host(display_layout_, {rx, ry, cw, ch}, client_host)) {
                VKR_WARN(kComponent)
                    << "initial placement skipped: guest/client transform overflow";
                client_host = display_layout_.virtual_bounds;
            }
            const display::MonitorDesc* monitor =
                display::find_monitor_largest_intersection(display_layout_, client_host);
            if (monitor == nullptr) {
                monitor = display::find_monitor_nearest(display_layout_, client_host);
            }
            if (monitor != nullptr) {
                const display::RectI32 placed = display::clamp_rect_to_monitor_work_or_virtual(
                    {outer.left, outer.top, outer.right - outer.left, outer.bottom - outer.top},
                    *monitor, display_layout_.virtual_bounds);
                place_left = placed.x;
                place_top = placed.y;
            }
        } else {
            // Legacy/direct-test fallback only. Production always supplies the pinned layout.
            MONITORINFO mi{};
            mi.cbSize = sizeof(MONITORINFO);
            if (GetMonitorInfo(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi)) {
                const vkr::sidecar::FramePos p = vkr::sidecar::clamp_frame_to_work_area(
                    outer.left, outer.top, outer.right, outer.bottom, mi.rcWork.left, mi.rcWork.top,
                    mi.rcWork.right, mi.rcWork.bottom);
                place_left = p.left;
                place_top = p.top;
            }
        }
    }
    // move: position-only (SWP_NOSIZE) -- no WM_SIZE, no latch. resize (apply_size):
    // size the client too ONLY when it actually differs from the current client; the sizing
    // SetWindowPos trips WM_SIZE -> the dirty latch (the WndProc marks dirty vs the active
    // swapchain extent), so the app recreates at the authored extent. A same-size authored apply
    // stays position-only (idempotent; no spurious latch).
    UINT flags = SWP_NOACTIVATE;
    bool resize = false;
    if (apply_size && cw > 0 && ch > 0) {
        RECT cur{};
        if (GetClientRect(hwnd, &cur)) {
            resize = (cur.right - cur.left) != cw || (cur.bottom - cur.top) != ch;
        }
    }
    if (!resize) {
        flags |= SWP_NOSIZE;
    }
    // fold the one-shot z-order intent into the SAME SetWindowPos. Raise -> HWND_TOP (top of
    // the NON-topmost stack -- no HWND_TOPMOST), Lower -> HWND_BOTTOM; SWP_NOACTIVATE so a restack
    // never steals focus/activation. None -> SWP_NOZORDER (keep the current z-position). Owned
    // popups ride the owner automatically (Win32 keeps owned windows above their owner), so a
    // toplevel raise brings its popups with it.
    HWND insert_after = nullptr;
    if (z_order == sidecar::ZOrder::Raise) {
        insert_after = HWND_TOP;
    } else if (z_order == sidecar::ZOrder::Lower) {
        insert_after = HWND_BOTTOM;
    } else {
        flags |= SWP_NOZORDER;
    }
    if (adopt_realization) {
        flags |= SWP_NOMOVE | SWP_NOSIZE; // preserve the pending Raise/Lower, geometry already live
    }
    // While the user has the host MAXIMIZED, a
    // sidecar-authored move/resize must NOT unmaximize it or snap it off-screen. The maximize
    // feedback keeps the authored geometry current in the normal case; this is the safety net
    // against a delayed/duplicate stale apply (e.g. the Win-key activation round-trip re-asserting
    // the old default). Suppress the position/size (SWP_NOMOVE|SWP_NOSIZE) but still allow a
    // z-order-only apply, and consume the seq below for sequencing. IsZoomed is false for WS_POPUP
    // hosts, so popups are unaffected.
    if (IsZoomed(hwnd)) {
        flags |= SWP_NOMOVE | SWP_NOSIZE;
    }
    {
        // the sidecar-authored move/resize is worker-origin -- guard so its WM_* (incl. the
        // echo of a user gesture) never re-forwards as a fake user request. The WM_SIZE a resize
        // trips STILL drives the latch (the guard gates ONLY the reverse enqueue).
        SelfApplyScope guard(&slot);
        SetWindowPos(hwnd, insert_after, place_left, place_top, outer.right - outer.left,
                     outer.bottom - outer.top, flags);
    }
    slot.mark_geometry_applied(seq);
    // Reconcile realization back to the guest. Read the REAL client
    // geometry the HWND actually landed at (the initial frame clamp / DWM realization can differ
    // from the authored desired), and record it as the reverse-path no-op baseline. If it DIFFERS
    // from what the sidecar authored, feed the real client geometry back as an ordinary
    // GeometryRequest so the guest X window + registry converge to the geometry that actually
    // exists on the host -- one coordinate truth (same principle as B's maximize feedback). Skipped
    // while IsZoomed (B owns the maximized state; the zoomed-host guard suppressed the apply, so
    // there is nothing to reconcile). Converges in one step: the echo re-authors the realized
    // geometry, the next apply is non-initial (no clamp) so real == authored -> no further
    // correction. Position-triggered (a size delta rides the resize path). No-op for a window with
    // no input target.
    if (slot.has_input_target() && !IsZoomed(hwnd)) {
        const RootClientGeometry real = read_real_root_client_geometry(hwnd, host_origin());
        slot.set_realized_geometry(real.root_x, real.root_y, real.client_w, real.client_h);
        if (real.root_x != rx || real.root_y != ry) {
            sidecar::SidecarInputEvent ev;
            ev.kind = static_cast<std::uint32_t>(sidecar::InputEventKind::GeometryRequest);
            ev.host_request = static_cast<std::uint32_t>(sidecar::HostRequestKind::Geometry);
            ev.root_x = real.root_x;
            ev.root_y = real.root_y;
            // A POSITION-only frame-clamp reconcile must carry the AUTHORED
            // client size (cw/ch), NOT the transient realized host client size -- else a surface
            // HWND still at its 256x256 create_surface default could be fed back as a user RESIZE
            // (the sidecar's user_request_is_resize compares req_w/h to the X window size) and grab
            // extent authority, reintroducing a transient startup size as truth (an -class
            // leak). A size-authoritative apply (apply_size) may carry the realized size -- that is
            // the explicit resize path, not this position-only frame-clamp correction.
            ev.req_w = static_cast<std::uint32_t>(apply_size ? real.client_w : cw);
            ev.req_h = static_cast<std::uint32_t>(apply_size ? real.client_h : ch);
            slot.set_pending_realization_adoption(
                real.root_x, real.root_y, static_cast<int>(ev.req_w), static_cast<int>(ev.req_h));
            slot.enqueue_input(ev);
        }
    }
}

void WindowThread::apply_visibility(HWND hwnd, sidecar::VisibilityState state, bool should_show,
                                    std::uint64_t host_apply_seq) {
    if (hwnd == nullptr) {
        return;
    }
    // Record the latest desired visibility (coalescing) + drain it, both on the window thread (the
    // slot's visibility cell is window-thread-only), mirroring apply_geometry. The drain may repost
    // if the slot is busy; this blocking call returns after the first attempt (seq-gated +
    // idempotent, so a deferred repost converges to the latest authored visibility).
    run_on_thread([this, hwnd, state, should_show, host_apply_seq]() {
        const auto it = slots_.find(hwnd);
        if (it == slots_.end()) {
            return; // window torn down before the apply landed
        }
        it->second->set_desired_visibility(state, should_show, host_apply_seq);
        drain_visibility(hwnd);
    });
}

void WindowThread::drain_visibility(HWND hwnd) {
    const auto it = slots_.find(hwnd);
    if (it == slots_.end()) {
        return; // window torn down between the post and the drain
    }
    WindowSlot& slot = *it->second;
    if (!slot.has_pending_visibility()) {
        return; // a newer drain already applied the latest desired (the visibility seq gate)
    }
    // same discipline as drain_geometry: try_lock the slot NON-BLOCKING (a present/acquire on
    // the session thread may hold it AND be mid-invoke to this thread); repost if busy. Uncontended
    // for placeholder/popup hosts (a free no-op).
    std::unique_lock<std::mutex> lk(slot.slot_lock(), std::try_to_lock);
    if (!lk.owns_lock()) {
        post_on_thread([this, hwnd]() { drain_visibility(hwnd); });
        return;
    }
    sidecar::VisibilityState state = sidecar::VisibilityState::Visible;
    bool should_show = false;
    std::uint64_t seq = 0;
    slot.desired_visibility(state, should_show, seq);
    // Hidden -> SW_HIDE; Iconic -> SW_SHOWMINNOACTIVE (taskbar-restorable, no activation --
    // authors it; carried here so adds no apply code); Visible -> SW_SHOWNA iff the reveal
    // predicate held (intent == Visible && paint-eligible), else stay hidden until the first-paint
    // reveal. SW_SHOWNA / SW_SHOWMINNOACTIVE never activate, so a restore never steals focus.
    int cmd = SW_HIDE;
    if (state == sidecar::VisibilityState::Iconic) {
        cmd = SW_SHOWMINNOACTIVE;
    } else if (state == sidecar::VisibilityState::Visible) {
        cmd = should_show ? SW_SHOWNA : SW_HIDE;
    }
    {
        // a worker-origin visibility apply -- guard so its WM_* never re-forwards as a user
        // gesture.
        SelfApplyScope guard(&slot);
        ShowWindow(hwnd, cmd);
    }
    slot.mark_visibility_applied(seq);
}

bool WindowThread::query_geometry(HWND hwnd, RECT& out_frame, int& out_client_w, int& out_client_h,
                                  POINT& out_client_origin, POINT& out_work_origin,
                                  std::uint64_t& out_applied_seq, bool& out_visible,
                                  bool& out_iconic) {
    if (hwnd == nullptr) {
        return false;
    }
    bool ok = false;
    const bool ran = run_on_thread([&]() {
        const auto it = slots_.find(hwnd);
        if (it == slots_.end()) {
            return;
        }
        RECT frame{};
        RECT client{};
        if (!GetWindowRect(hwnd, &frame) || !GetClientRect(hwnd, &client)) {
            return;
        }
        POINT origin{0, 0};
        ClientToScreen(hwnd, &origin);
        out_frame = frame;
        out_client_w = static_cast<int>(client.right - client.left);
        out_client_h = static_cast<int>(client.bottom - client.top);
        out_client_origin = origin;
        out_work_origin = host_origin();
        out_applied_seq = it->second->last_host_apply_seq();
        out_visible = IsWindowVisible(hwnd) != FALSE; // host-observed visibility
        out_iconic = IsIconic(hwnd) != FALSE;
        ok = true;
    });
    return ran && ok;
}

} // namespace vkr::worker::windowing
