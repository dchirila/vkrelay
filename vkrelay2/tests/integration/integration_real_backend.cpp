// in-process real-backend test for the app-recorded frame
// (record_command_buffer + queue_submit, with real semaphore/fence sync) AND the slot
// lock + geometry-dirty latch. Constructs RealVulkanBackend directly (same process, so
// the test can reach the surface's HWND) and drives a REAL Win32 WM_SIZE via SetWindowPos,
// then asserts acquire/present return VK_ERROR_OUT_OF_DATE_KHR without calling the host
// driver, and that recreating the swapchain clears the latch. This is the only way to
// exercise the real WndProc latch deterministically before the sidecar's resize verb
// integration_vulkan_real drives the worker as a separate process over RPC,
// which has no resize path yet. Windows-only (registered under WIN32 + Vulkan SDK).
// Skips gracefully (still passes) if no usable Vulkan device is present.
#include "windows/worker/real_vulkan_backend.hpp"

#include "common/vkrpc/vulkan_session.hpp"
#include "tests/test_assert.hpp"
#include "windows/worker/windowing/coordinate_map.h"
#include "windows/worker/windowing/window_thread.hpp" // WindowThread::set_guest_root (cap seam)

#include <shellscalingapi.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace vkr;

namespace {

struct DpiMonitorSample {
    RECT bounds{};
    UINT dpi = 0;
};

BOOL CALLBACK collect_dpi_monitor(HMONITOR monitor, HDC, LPRECT, LPARAM data) {
    auto& out = *reinterpret_cast<std::vector<DpiMonitorSample>*>(data);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    UINT dpi_x = 0;
    UINT dpi_y = 0;
    if (GetMonitorInfoW(monitor, &info) != FALSE &&
        SUCCEEDED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpi_x, &dpi_y)) && dpi_x != 0) {
        out.push_back({info.rcMonitor, dpi_x});
    }
    return TRUE;
}

// Resizes the surface's hidden window CLIENT rect to (w,h) via a cross-thread
// SetWindowPos. SetWindowPos sends WM_SIZE synchronously to the owning (window) thread,
// so the WndProc has marked the slot dirty by the time this returns.
void resize_client(HWND hwnd, int w, int h) {
    RECT rc{0, 0, w, h};
    // DPI-correct non-client metrics at the window's monitor DPI. The worker's window thread is
    // per-monitor aware, so this drives the client to exactly (w,h) physical pixels.
    AdjustWindowRectExForDpi(&rc, WS_OVERLAPPEDWINDOW, FALSE, 0, GetDpiForWindow(hwnd));
    SetWindowPos(hwnd, nullptr, 0, 0, rc.right - rc.left, rc.bottom - rc.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

display::DisplayLayout current_desktop_snapshot(const char* id) {
    display::DisplayLayout layout;
    layout.snapshot_id = id;
    display::MonitorDesc monitor;
    monitor.stable_id = "integration-primary";
    monitor.device_name = "integration-only";
    monitor.bounds = {GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
                      GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN)};
    monitor.work = monitor.bounds;
    monitor.dpi_x = 96;
    monitor.dpi_y = 96;
    monitor.primary = true;
    layout.virtual_bounds = monitor.bounds;
    layout.primary_monitor_id = monitor.stable_id;
    layout.monitors.push_back(std::move(monitor));
    return layout;
}

void test_static_topology_restart_latch() {
    // Level 1 keeps one immutable snapshot. Drive the real WndProc directly (no desktop
    // mutation or broadcast): unrelated settings do nothing, while both supported topology/work
    // signals latch a clean restart-required state. Destruction after the latch proves teardown
    // remains available; no handler re-probes and adopts a replacement layout.
    {
        const display::DisplayLayout layout = current_desktop_snapshot("integration/displaychange");
        worker::windowing::WindowThread windows(&layout);
        const worker::windowing::CreatedWindow created = windows.create_hidden_window(240, 160);
        VKR_CHECK(created.hwnd != nullptr);
        VKR_CHECK(!windows.display_restart_required());
        SendMessageW(created.hwnd, WM_SETTINGCHANGE, 0, 0);
        VKR_CHECK(!windows.display_restart_required());
        SendMessageW(created.hwnd, WM_DISPLAYCHANGE, 32, MAKELPARAM(1920, 1080));
        VKR_CHECK(windows.display_restart_required());
        windows.destroy_window(created.hwnd);
    }
    {
        const display::DisplayLayout layout = current_desktop_snapshot("integration/workarea");
        worker::windowing::WindowThread windows(&layout);
        const worker::windowing::CreatedWindow created = windows.create_hidden_window(240, 160);
        VKR_CHECK(created.hwnd != nullptr);
        VKR_CHECK(!windows.display_restart_required());
        SendMessageW(created.hwnd, WM_SETTINGCHANGE, SPI_SETWORKAREA, 0);
        VKR_CHECK(windows.display_restart_required());
        windows.destroy_window(created.hwnd);
    }
}

// Builds the app's "clear to (r,g,b)" recording for `cmd` over swapchain image `image`:
// transition UNDEFINED->TRANSFER_DST, clear the color, transition TRANSFER_DST->PRESENT_SRC
// (the shape debug_render_test_frame used to synthesize, now app-recorded). Uses the real
// VK_* enum values (this in-process test links Vulkan).
vkrpc::RecordCommandBufferRequest clear_recording(std::uint64_t cmd, std::uint64_t image, float r,
                                                  float g, float b) {
    vkrpc::RecordCommandBufferRequest rec;
    rec.command_buffer = cmd;
    rec.one_time_submit = true;

    vkrpc::RecordedCommand to_dst;
    to_dst.kind = "pipeline_barrier";
    to_dst.image = image;
    to_dst.old_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    to_dst.new_layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_dst.src_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    to_dst.dst_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    to_dst.src_access = 0;
    to_dst.dst_access = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_dst.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    rec.commands.push_back(to_dst);

    vkrpc::RecordedCommand clear;
    clear.kind = "clear_color_image";
    clear.image = image;
    clear.layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    clear.r = r;
    clear.g = g;
    clear.b = b;
    clear.a = 1.0;
    rec.commands.push_back(clear);

    vkrpc::RecordedCommand to_present;
    to_present.kind = "pipeline_barrier";
    to_present.image = image;
    to_present.old_layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_present.new_layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    to_present.src_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    to_present.dst_stage = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    to_present.src_access = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_present.dst_access = 0;
    to_present.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    rec.commands.push_back(to_present);
    return rec;
}

// The sidecar plane's placeholder lifecycle drives REAL hidden Win32 windows on the
// worker's window thread -- and needs NO Vulkan device (placeholders have no surface). Proves the
// real HWND executor stays in lockstep with the pure registry state machine: two
// toplevels -> two placeholder HWNDs; unregister one -> only ITS HWND is destroyed (the sibling's
// survives); override_redirect is refused; a stale generation is dropped. Skips gracefully if the
// window thread cannot make a window (a truly headless box).
void test_real_placeholder_executor() {
    using sidecar::Representation;
    worker::RealVulkanBackend backend("", "", /*gpu_required=*/false);

    sidecar::SidecarRegisterToplevelRequest a;
    a.xid = 0xA1;
    a.generation = 1;
    a.role = "toplevel";
    a.width = 300;
    a.height = 200;
    const sidecar::SidecarToplevelResponse ra = backend.register_toplevel(a);
    VKR_CHECK(ra.ok && ra.applied);
    VKR_CHECK_EQ(ra.representation, std::string("placeholder"));
    if (backend.debug_placeholder_hwnd_count() == 0) {
        std::fprintf(stderr,
                     "integration_real_backend: placeholder executor skipped (no window thread)\n");
        return;
    }
    VKR_CHECK(backend.debug_representation_for_xid(0xA1) == Representation::Placeholder);
    VKR_CHECK_EQ(backend.debug_registry_placeholder_count(), static_cast<std::size_t>(1));
    VKR_CHECK_EQ(backend.debug_placeholder_hwnd_count(), static_cast<std::size_t>(1));
    const HWND h_a = backend.debug_placeholder_hwnd_for_xid(0xA1);
    VKR_CHECK(h_a != nullptr);

    sidecar::SidecarRegisterToplevelRequest b;
    b.xid = 0xB2;
    b.generation = 1;
    b.width = 300;
    b.height = 200;
    VKR_CHECK(backend.register_toplevel(b).applied);
    VKR_CHECK_EQ(backend.debug_placeholder_hwnd_count(), static_cast<std::size_t>(2));
    const HWND h_b = backend.debug_placeholder_hwnd_for_xid(0xB2);
    VKR_CHECK(h_b != nullptr && h_b != h_a);

    // The FULLSCREEN class (the ExtremeTuxRacer fix): an override-redirect NON-popup registration
    // is a deliberate sidecar classification (root-covering, SFML 2.5's non-EWMH fullscreen) and
    // now registers a REAL placeholder HWND like any toplevel -- the old refuse-all-non-popup gate
    // stranded it on the create_surface 256x256 default host. Unregistered again so the
    // teardown-isolation checks below stay focused on 0xA1/0xB2.
    sidecar::SidecarRegisterToplevelRequest fullscreen;
    fullscreen.xid = 0xC3;
    fullscreen.generation = 1;
    fullscreen.override_redirect = true; // is_popup stays false
    fullscreen.width = 640;
    fullscreen.height = 480;
    VKR_CHECK(backend.register_toplevel(fullscreen).applied);
    VKR_CHECK_EQ(backend.debug_placeholder_hwnd_count(), static_cast<std::size_t>(3));
    VKR_CHECK(backend.debug_placeholder_hwnd_for_xid(0xC3) != nullptr);
    sidecar::SidecarUnregisterToplevelRequest fs_unr;
    fs_unr.xid = 0xC3;
    fs_unr.generation = 2;
    VKR_CHECK(backend.unregister_toplevel(fs_unr).applied);
    VKR_CHECK_EQ(backend.debug_placeholder_hwnd_count(), static_cast<std::size_t>(2));

    // Unregister 0xA1 -> ONLY its HWND is destroyed; 0xB2's HWND survives unchanged.
    sidecar::SidecarUnregisterToplevelRequest ua;
    ua.xid = 0xA1;
    ua.generation = 2;
    VKR_CHECK(backend.unregister_toplevel(ua).applied);
    VKR_CHECK_EQ(backend.debug_placeholder_hwnd_count(), static_cast<std::size_t>(1));
    VKR_CHECK(backend.debug_placeholder_hwnd_for_xid(0xA1) == nullptr);
    VKR_CHECK_EQ(backend.debug_placeholder_hwnd_for_xid(0xB2), h_b);

    // A stale unregister of 0xB2 is dropped -> its HWND survives.
    sidecar::SidecarUnregisterToplevelRequest stale;
    stale.xid = 0xB2;
    stale.generation = 1; // <= the current generation (1)
    VKR_CHECK(!backend.unregister_toplevel(stale).applied);
    VKR_CHECK_EQ(backend.debug_placeholder_hwnd_count(), static_cast<std::size_t>(1));

    // Clean unregister of 0xB2 -> all placeholders gone; the registry is empty.
    sidecar::SidecarUnregisterToplevelRequest ub;
    ub.xid = 0xB2;
    ub.generation = 2;
    VKR_CHECK(backend.unregister_toplevel(ub).applied);
    VKR_CHECK_EQ(backend.debug_placeholder_hwnd_count(), static_cast<std::size_t>(0));
    VKR_CHECK_EQ(backend.debug_registry_entry_count(), static_cast<std::size_t>(0));
    // The backend destructor now tears the window thread down with no live placeholders.
}

// the worker's initial-placement on-screen CLAMP. A toplevel registered with its CLIENT at
// the X-root origin (0,0) would land with its chrome (title bar, ABOVE the client) off the top of
// the work area; the clamp must shift the OUTER frame fully onto the monitor work area on the FIRST
// placement. Deterministic (GetWindowRect/GetMonitorInfo -- no foreground/z-order dependency).
// Skips if no window thread.
void test_real_initial_placement_clamp() {
    worker::RealVulkanBackend backend("", "", /*gpu_required=*/false);
    sidecar::SidecarRegisterToplevelRequest r;
    r.xid = 0xC10;
    r.generation = 1;
    r.role = "toplevel";
    r.x = 0; // CLIENT at the X-root origin -> chrome would be above the work area without the clamp
    r.y = 0;
    r.width = 300;
    r.height = 200;
    VKR_CHECK(backend.register_toplevel(r).applied);
    const HWND h = backend.debug_placeholder_hwnd_for_xid(0xC10);
    if (h == nullptr) {
        std::fprintf(stderr, "integration_real_backend: clamp test skipped (no window thread)\n");
        return;
    }
    RECT frame{};
    VKR_CHECK(GetWindowRect(h, &frame));
    MONITORINFO mi{};
    mi.cbSize = sizeof(MONITORINFO);
    VKR_CHECK(GetMonitorInfo(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi));
    // The whole outer frame (incl. the title bar) is on the work area -- the clamp fired. Without
    // it, frame.top would be ABOVE mi.rcWork.top (negative chrome offset).
    VKR_CHECK(frame.top >= mi.rcWork.top);
    VKR_CHECK(frame.left >= mi.rcWork.left);

    sidecar::SidecarUnregisterToplevelRequest u;
    u.xid = 0xC10;
    u.generation = 2;
    VKR_CHECK(backend.unregister_toplevel(u).applied);
}

// Both first-placement recovery and maximize use the pinned snapshot's work rectangle. The
// test deliberately insets that rectangle beyond the OS work area, proving the live APIs supply
// monitor identity only and cannot silently become a second work-area authority.
void test_real_snapshot_placement_and_maximize() {
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE);
    POINT origin{0, 0};
    const HMONITOR hm = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (hm == nullptr || !GetMonitorInfoW(hm, &mi)) {
        std::fprintf(stderr, "integration_real_backend: snapshot policy skipped (no monitor)\n");
        return;
    }
    display::DisplayLayout layout;
    layout.snapshot_id = "integration-m3";
    layout.primary_monitor_id = "primary";
    layout.virtual_bounds = {mi.rcMonitor.left, mi.rcMonitor.top,
                             mi.rcMonitor.right - mi.rcMonitor.left,
                             mi.rcMonitor.bottom - mi.rcMonitor.top};
    display::MonitorDesc monitor;
    monitor.stable_id = "primary";
    for (const wchar_t* p = mi.szDevice; *p != L'\0'; ++p) {
        monitor.device_name.push_back(static_cast<char>(*p));
    }
    monitor.bounds = layout.virtual_bounds;
    monitor.work = {monitor.bounds.x + 80, monitor.bounds.y + 60, monitor.bounds.width - 120,
                    monitor.bounds.height - 100};
    monitor.dpi_x = monitor.dpi_y = 96;
    monitor.primary = true;
    layout.monitors = {monitor};
    VKR_CHECK(display::validate_display_layout(layout).ok);

    worker::windowing::WindowThread::set_guest_root(0, 0);
    worker::RealVulkanBackend backend("", "", /*gpu_required=*/false, &layout);
    sidecar::SidecarReadyRequest ready;
    ready.display_snapshot_id = layout.snapshot_id;
    ready.root_width = static_cast<std::uint32_t>(layout.virtual_bounds.width);
    ready.root_height = static_cast<std::uint32_t>(layout.virtual_bounds.height);
    VKR_CHECK(backend.sidecar_ready(ready).ok);

    sidecar::SidecarRegisterToplevelRequest r;
    r.xid = 0xC11;
    r.generation = 1;
    r.width = 300;
    r.height = 200;
    VKR_CHECK(backend.register_toplevel(r).applied);
    const HWND h = backend.debug_placeholder_hwnd_for_xid(r.xid);
    if (h != nullptr) {
        RECT frame{};
        VKR_CHECK(GetWindowRect(h, &frame));
        VKR_CHECK(frame.left >= monitor.work.x);
        VKR_CHECK(frame.top >= monitor.work.y);

        MINMAXINFO mmi{};
        SendMessageW(h, WM_GETMINMAXINFO, 0, reinterpret_cast<LPARAM>(&mmi));
        VKR_CHECK_EQ(mmi.ptMaxPosition.x, monitor.work.x - monitor.bounds.x);
        VKR_CHECK_EQ(mmi.ptMaxPosition.y, monitor.work.y - monitor.bounds.y);
        VKR_CHECK_EQ(mmi.ptMaxSize.x, monitor.work.width);
        VKR_CHECK_EQ(mmi.ptMaxSize.y, monitor.work.height);
        VKR_CHECK(mmi.ptMaxTrackSize.x >= layout.virtual_bounds.width);
        VKR_CHECK(mmi.ptMaxTrackSize.y >= layout.virtual_bounds.height);
    }
    sidecar::SidecarUnregisterToplevelRequest u;
    u.xid = r.xid;
    u.generation = 2;
    VKR_CHECK(backend.unregister_toplevel(u).applied);
    worker::windowing::WindowThread::set_guest_root(0, 0);
}

// Test the chrome paint path against a REAL placeholder window + its window-thread DIB -- no
// Vulkan device needed. Proves paint_chrome composites the captured BGRA into the DIB, shows the
// window only AFTER a realized paint, and that DebugChromeState samples the actual painted
// pixel (the worker-visible proof seam, in-process here; over the wire in the boundary smoke).
// Skips gracefully if the window thread cannot make a window.
void test_real_chrome_paint() {
    worker::RealVulkanBackend backend("", "", /*gpu_required=*/false);

    sidecar::SidecarRegisterToplevelRequest reg;
    reg.xid = 0xD1;
    reg.generation = 1;
    reg.width = 8;
    reg.height = 4;
    VKR_CHECK(backend.register_toplevel(reg).applied);
    if (backend.debug_placeholder_hwnd_count() == 0) {
        std::fprintf(stderr, "integration_real_backend: chrome paint skipped (no window thread)\n");
        return;
    }

    // Not shown before any paint (the placeholder is created hidden).
    sidecar::SidecarDebugChromeStateRequest q0;
    q0.xid = 0xD1;
    VKR_CHECK(!backend.debug_chrome_state(q0).shown);

    // Paint a solid color over the whole 8x4 source (B=0x11 G=0x22 R=0x33 A=0xFF).
    sidecar::SidecarPaintChromeRequest paint;
    paint.xid = 0xD1;
    paint.lifecycle_generation = 1;
    paint.seq = 1;
    paint.src_w = 8;
    paint.src_h = 4;
    paint.dirty_w = 8;
    paint.dirty_h = 4;
    paint.stride = 32;
    paint.pixels.resize(32 * 4);
    for (std::size_t i = 0; i < paint.pixels.size(); i += 4) {
        paint.pixels[i + 0] = static_cast<char>(0x11);
        paint.pixels[i + 1] = static_cast<char>(0x22);
        paint.pixels[i + 2] = static_cast<char>(0x33);
        paint.pixels[i + 3] = static_cast<char>(0xFF);
    }
    const sidecar::SidecarPaintResponse presp = backend.paint_chrome(paint);
    VKR_CHECK(presp.ok && presp.applied && presp.shown);
    VKR_CHECK_EQ(presp.representation, std::string("placeholder"));
    VKR_CHECK_EQ(presp.last_seq, static_cast<std::uint64_t>(1));

    // DebugChromeState samples the REAL DIB on the window thread -> the painted color.
    sidecar::SidecarDebugChromeStateRequest q;
    q.xid = 0xD1;
    q.sample_x = 3;
    q.sample_y = 2;
    const sidecar::SidecarDebugChromeStateResponse dr = backend.debug_chrome_state(q);
    VKR_CHECK(dr.ok && dr.shown && dr.has_pixel);
    VKR_CHECK_EQ(dr.pixel_bgra, static_cast<std::uint32_t>(0xFF332211u));

    // An out-of-bounds sample reports no pixel.
    q.sample_x = 999;
    VKR_CHECK(!backend.debug_chrome_state(q).has_pixel);

    // Teardown leaves nothing live.
    sidecar::SidecarUnregisterToplevelRequest u;
    u.xid = 0xD1;
    u.generation = 2;
    VKR_CHECK(backend.unregister_toplevel(u).applied);
    VKR_CHECK_EQ(backend.debug_placeholder_hwnd_count(), static_cast<std::size_t>(0));
}

// The WndProc INPUT CAPTURE path against a REAL placeholder window -- no Vulkan device.
// Posts real WM_* messages to the placeholder HWND (SendMessageW dispatches synchronously on the
// window thread) and proves: neutral events land in the ring with the right kind/coords/key/focus;
// a MOTION FLOOD coalesces to a single motion while every non-motion event survives; WM_CLOSE is
// SWALLOWED (the worker never DestroyWindows -- it enqueues a Close, the guest stays the lifecycle
// authority); and a post-unregister poll DROPS stale events (the exact-epoch gate). Skips if
// the window thread cannot make a window.
void test_real_input_capture() {
    worker::RealVulkanBackend backend("", "", /*gpu_required=*/false);

    sidecar::SidecarRegisterToplevelRequest reg;
    reg.xid = 0xE1;
    reg.generation = 1;
    reg.width = 120;
    reg.height = 80;
    VKR_CHECK(backend.register_toplevel(reg).applied);
    const HWND hwnd = backend.debug_placeholder_hwnd_for_xid(0xE1);
    if (hwnd == nullptr) {
        std::fprintf(stderr,
                     "integration_real_backend: input capture skipped (no window thread)\n");
        return;
    }
    const std::uint64_t epoch =
        backend.debug_registry_epoch_for_xid(0xE1); // stamped on captured ev
    VKR_CHECK(epoch != 0);

    // Drive a real input burst (SendMessageW dispatches synchronously to the WndProc): focus, a
    // MOTION FLOOD (20 moves), a left click, a wheel notch up, a key 'A', then close.
    SendMessageW(hwnd, WM_SETFOCUS, 0, 0);
    for (int i = 0; i < 20; ++i) {
        SendMessageW(hwnd, WM_MOUSEMOVE, 0, MAKELPARAM(10 + i, 20 + i));
    }
    SendMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(30, 40));
    SendMessageW(hwnd, WM_LBUTTONUP, 0, MAKELPARAM(30, 40));
    SendMessageW(hwnd, WM_MOUSEWHEEL, MAKEWPARAM(0, WHEEL_DELTA), MAKELPARAM(200, 200));
    SendMessageW(hwnd, WM_KEYDOWN, 'A', 0);
    SendMessageW(hwnd, WM_KEYUP, 'A', 0);
    VKR_CHECK(SendMessageW(hwnd, WM_CLOSE, 0, 0) == 0); // swallowed (no DestroyWindow)
    VKR_CHECK_EQ(backend.debug_placeholder_hwnd_for_xid(0xE1), hwnd); // window NOT destroyed

    sidecar::SidecarPollInputRequest p;
    p.since_seq = 0;
    const sidecar::SidecarPollInputResponse r = backend.poll_input(p);
    VKR_CHECK(r.ok);
    // NOTE: this drives a REAL top-level HWND on an interactive machine, so the live cursor can
    // inject a stray genuine WM_MOUSEMOVE (it coalesces into the motion slot). The assertions are
    // therefore robust to EXTRA incidental input -- the system never spontaneously produces our
    // discrete events (button/key/wheel/focus/close) for this window, and coalescing is proven
    // exactly (==1) in the deterministic unit test (unit_sidecar). Here we prove the real WndProc
    // capture path: the flood COLLAPSED, and every discrete event got through despite it.
    int motions = 0, buttons = 0, wheels = 0, key_a = 0, focuses = 0, closes = 0, first_focus = -1,
        first_button = -1, idx = 0;
    for (const auto& e : r.events) {
        switch (static_cast<sidecar::InputEventKind>(e.kind)) {
        case sidecar::InputEventKind::Motion:
            ++motions;
            break;
        case sidecar::InputEventKind::Button:
            ++buttons;
            VKR_CHECK_EQ(e.button, sidecar::kInputButtonLeft);
            if (first_button < 0) {
                first_button = idx;
            }
            break;
        case sidecar::InputEventKind::Wheel:
            if (e.wheel == 1) {
                ++wheels;
            }
            break;
        case sidecar::InputEventKind::Key:
            if (e.vk == 'A') {
                ++key_a;
                VKR_CHECK_EQ(e.epoch, epoch); // stamped with the representation epoch at creation
            }
            break;
        case sidecar::InputEventKind::Focus:
            ++focuses;
            if (first_focus < 0 && e.pressed) {
                first_focus = idx;
            }
            break;
        case sidecar::InputEventKind::Close:
            ++closes;
            break;
        default:
            break;
        }
        ++idx;
    }
    // True per-XID coalescing keeps at most one pending motion for the placeholder's xid, so the
    // 20-move flood (and any stray real cursor motion, which shares the xid) collapses to
    // exactly 1.
    VKR_CHECK_EQ(motions, 1);
    VKR_CHECK(buttons >= 2); // left down + up got through despite the flood
    VKR_CHECK(wheels >= 1);  // wheel-up notch
    VKR_CHECK(key_a >= 2);   // key 'A' down + up
    VKR_CHECK(focuses >= 1); // focus gained
    VKR_CHECK(closes >= 1);  // WM_CLOSE enqueued a Close (and was swallowed -- see above)
    // Intent ordering: focus was captured before the first click (a stray real motion may precede
    // focus, but no discrete event does).
    VKR_CHECK(first_focus >= 0 && first_button > first_focus);

    // Stale gate: post more input, UNREGISTER (erases the entry -> epoch 0 + destroys the HWND),
    // then a poll at the advanced cursor DROPS the now-stale events (exact-epoch gate, the real
    // path). The remap accept/drop (re-register mints a NEW epoch) is proven dual-platform in
    // unit_sidecar's test_input_plane_epoch_gate.
    SendMessageW(hwnd, WM_MOUSEMOVE, 0, MAKELPARAM(1, 1));
    SendMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(1, 1));
    sidecar::SidecarUnregisterToplevelRequest u;
    u.xid = 0xE1;
    u.generation = 2;
    VKR_CHECK(backend.unregister_toplevel(u).applied);
    sidecar::SidecarPollInputRequest p2;
    p2.since_seq = r.next_seq;
    VKR_CHECK_EQ(backend.poll_input(p2).events.size(), static_cast<std::size_t>(0));
    VKR_CHECK_EQ(backend.debug_placeholder_hwnd_for_xid(0xE1), nullptr);
}

// (the acceptance suite -- the original two-injector defect): with TWO toplevels,
// focus is attributed to the INTENDED toplevel and NEVER cross-wired to the sibling (the original's
// bug was focus reclamation to a hidden surface stealing events from the visible toplevel). The
// worker stamps each captured focus event with the producing window's OWN guest xid; the sidecar
// (the sole X focus actor -- the ICD has no focus code) then xcb_set_input_focus's exactly that
// xid. This proves the worker-attribution half structurally + deterministically; the single-window
// end-to-end (FocusIn actually lands on the guest) + the focus-ownership LOG (contract 7.4) are
// proven by run_input_smoke.sh. Skips if the window thread cannot make a window.
void test_real_focus_ownership() {
    worker::RealVulkanBackend backend("", "", /*gpu_required=*/false);

    sidecar::SidecarRegisterToplevelRequest a;
    a.xid = 0xF1;
    a.generation = 1;
    a.width = 120;
    a.height = 80;
    VKR_CHECK(backend.register_toplevel(a).applied);
    sidecar::SidecarRegisterToplevelRequest b;
    b.xid = 0xF2;
    b.generation = 1;
    b.width = 120;
    b.height = 80;
    VKR_CHECK(backend.register_toplevel(b).applied);
    const HWND ha = backend.debug_placeholder_hwnd_for_xid(0xF1);
    const HWND hb = backend.debug_placeholder_hwnd_for_xid(0xF2);
    if (ha == nullptr || hb == nullptr) {
        std::fprintf(stderr,
                     "integration_real_backend: focus ownership skipped (no window thread)\n");
        return;
    }
    VKR_CHECK(ha != hb);

    // Move focus across the two distinct toplevels: focus A, then (kill A) focus B.
    SendMessageW(ha, WM_SETFOCUS, 0, 0);
    SendMessageW(ha, WM_KILLFOCUS, 0, 0);
    SendMessageW(hb, WM_SETFOCUS, 0, 0);

    sidecar::SidecarPollInputRequest p;
    p.since_seq = 0;
    const sidecar::SidecarPollInputResponse r = backend.poll_input(p);
    VKR_CHECK(r.ok);

    int a_gained = 0, b_gained = 0, foreign = 0;
    for (const auto& e : r.events) {
        if (static_cast<sidecar::InputEventKind>(e.kind) != sidecar::InputEventKind::Focus) {
            continue;
        }
        // Every focus event must be stamped with one of the two known toplevels -- never a foreign
        // or hidden xid -- and a gained event must be attributed to the window that actually gained
        // it.
        if (e.xid == 0xF1) {
            if (e.pressed) {
                ++a_gained;
            }
        } else if (e.xid == 0xF2) {
            if (e.pressed) {
                ++b_gained;
            }
        } else {
            ++foreign;
        }
    }
    VKR_CHECK_EQ(foreign, 0); // no focus event leaked to a foreign xid (the "not stolen" property)
    VKR_CHECK(a_gained >= 1); // A's WM_SETFOCUS attributed to A
    VKR_CHECK(b_gained >= 1); // B's WM_SETFOCUS attributed to B (not cross-wired to A)
}

// The SURFACE-backed remap slot-rebind, on the REAL backend. A real
// VkSurfaceKHR + window backs the xid; an unregister-while-live then re-register mints a new epoch
// AND the backend must re-stamp the SURFACE slot's epoch -- so REAL Win32 input captured after the
// remap carries the new epoch and is accepted (if the rebind were broken, it would stamp the old
// epoch and be dropped). Self-skips when no Vulkan instance/surface can be made.
void test_real_surface_input_remap() {
    worker::RealVulkanBackend backend("", "", /*gpu_required=*/false);
    const vkrpc::CreateInstanceResponse ci = backend.create_instance({});
    if (!ci.ok) {
        std::fprintf(stderr, "integration_real_backend: surface remap skipped (no Vulkan: %s)\n",
                     ci.reason.c_str());
        return;
    }
    vkrpc::CreateSurfaceRequest cs;
    cs.instance = ci.instance;
    cs.platform = "xcb";
    cs.xid = 0xE2;
    const vkrpc::CreateSurfaceResponse sr = backend.create_surface(cs);
    if (!sr.ok) {
        std::fprintf(stderr, "integration_real_backend: surface remap skipped (no surface: %s)\n",
                     sr.reason.c_str());
        return;
    }
    const HWND hwnd = backend.debug_surface_hwnd(sr.surface);
    VKR_CHECK(hwnd != nullptr);

    // First register over the surface: the slot was stamped at create_surface with epoch e1.
    sidecar::SidecarRegisterToplevelRequest reg;
    reg.xid = 0xE2;
    reg.generation = 1;
    reg.role = "toplevel";
    VKR_CHECK(backend.register_toplevel(reg).applied);
    const std::uint64_t e1 = backend.debug_registry_epoch_for_xid(0xE2);
    VKR_CHECK(e1 != 0);

    // Unregister WHILE the surface is live (permutation 4) -> the entry + surface persist, epoch
    // e1.
    sidecar::SidecarUnregisterToplevelRequest unr;
    unr.xid = 0xE2;
    unr.generation = 2;
    VKR_CHECK(backend.unregister_toplevel(unr).applied);
    VKR_CHECK_EQ(backend.debug_registry_epoch_for_xid(0xE2), e1);
    // Re-register -> the remap mints e2 AND reconcile_surface_input_epoch re-stamps the surface
    // slot.
    sidecar::SidecarRegisterToplevelRequest reg2;
    reg2.xid = 0xE2;
    reg2.generation = 3;
    reg2.role = "toplevel";
    VKR_CHECK(backend.register_toplevel(reg2).applied);
    const std::uint64_t e2 = backend.debug_registry_epoch_for_xid(0xE2);
    VKR_CHECK(e2 != 0 && e2 != e1);

    // REAL input captured AFTER the remap must carry the NEW epoch (the slot was rebound) ->
    // accepted by poll_input. A broken rebind would stamp e1 and the gate would drop it (key_b ==
    // 0).
    SendMessageW(hwnd, WM_KEYDOWN, 'B', 0);
    SendMessageW(hwnd, WM_KEYUP, 'B', 0);
    sidecar::SidecarPollInputRequest p;
    p.since_seq = 0;
    const sidecar::SidecarPollInputResponse rr = backend.poll_input(p);
    int key_b = 0;
    for (const auto& e : rr.events) {
        if (static_cast<sidecar::InputEventKind>(e.kind) == sidecar::InputEventKind::Key &&
            e.vk == 'B') {
            ++key_b;
            VKR_CHECK_EQ(e.epoch, e2); // stamped the NEW epoch -> the surface slot was rebound
        }
    }
    VKR_CHECK(key_b >= 2); // down + up captured + accepted (not dropped by the exact-epoch gate)

    vkrpc::HandleRequest ds;
    ds.handle = sr.surface;
    VKR_CHECK(backend.destroy_surface(ds).ok); // frees the window + the surface-input mirror
}

// the cursor path against a REAL placeholder window -- no Vulkan device. Proves
// set_cursor builds a real HCURSOR from the guest's BGRA image, DebugCursorState samples the built
// cursor (dims/hotspot/pixel), and WM_SETCURSOR for the client area is handled (applies it). Skips
// if the window thread cannot make a window.
void test_real_cursor() {
    worker::RealVulkanBackend backend("", "", /*gpu_required=*/false);

    sidecar::SidecarRegisterToplevelRequest reg;
    reg.xid = 0xF1;
    reg.generation = 1;
    reg.width = 64;
    reg.height = 48;
    VKR_CHECK(backend.register_toplevel(reg).applied);
    const HWND hwnd = backend.debug_placeholder_hwnd_for_xid(0xF1);
    if (hwnd == nullptr) {
        std::fprintf(stderr, "integration_real_backend: cursor skipped (no window thread)\n");
        return;
    }

    // Build + bind a known 8x6 cursor (B=0x99 G=0x66 R=0x33 A=0xFF -> sampled 0xFF336699), stamped
    // with the xid's live representation epoch (the worker gates on registered + exact epoch).
    sidecar::SidecarSetCursorRequest c;
    c.xid = 0xF1;
    c.epoch = backend.debug_registry_epoch_for_xid(0xF1);
    c.width = 8;
    c.height = 6;
    c.xhot = 2;
    c.yhot = 3;
    c.format = sidecar::kCursorFormatBgra8;
    c.pixels.resize(static_cast<std::size_t>(8) * 6 * 4);
    for (std::size_t i = 0; i < c.pixels.size(); i += 4) {
        c.pixels[i + 0] = static_cast<char>(0x99);
        c.pixels[i + 1] = static_cast<char>(0x66);
        c.pixels[i + 2] = static_cast<char>(0x33);
        c.pixels[i + 3] = static_cast<char>(0xFF);
    }
    const sidecar::SidecarSetCursorResponse sr = backend.set_cursor(c);
    VKR_CHECK(sr.ok && sr.applied);

    // The built cursor samples to the known color + carries the dims/hotspot.
    sidecar::SidecarDebugCursorStateRequest q;
    q.xid = 0xF1;
    q.sample_x = 4;
    q.sample_y = 2;
    const sidecar::SidecarDebugCursorStateResponse dr = backend.debug_cursor_state(q);
    VKR_CHECK(dr.ok && dr.has_cursor && dr.has_pixel);
    VKR_CHECK_EQ(dr.width, static_cast<std::uint32_t>(8));
    VKR_CHECK_EQ(dr.height, static_cast<std::uint32_t>(6));
    VKR_CHECK_EQ(dr.xhot, 2);
    VKR_CHECK_EQ(dr.yhot, 3);
    VKR_CHECK_EQ(dr.pixel_bgra, static_cast<std::uint32_t>(0xFF336699u));

    // WM_SETCURSOR for the CLIENT area is handled (the WndProc applies the cursor + returns TRUE).
    const LRESULT hit = SendMessageW(hwnd, WM_SETCURSOR, reinterpret_cast<WPARAM>(hwnd),
                                     MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));
    VKR_CHECK(hit == TRUE);

    // An out-of-bounds sample reports no pixel (but still has_cursor).
    q.sample_x = 999;
    const sidecar::SidecarDebugCursorStateResponse oob = backend.debug_cursor_state(q);
    VKR_CHECK(oob.has_cursor && !oob.has_pixel);

    // Teardown: unregister destroys the placeholder -> no cursor for the xid.
    sidecar::SidecarUnregisterToplevelRequest u;
    u.xid = 0xF1;
    u.generation = 2;
    VKR_CHECK(backend.unregister_toplevel(u).applied);
    VKR_CHECK(!backend.debug_cursor_state(q).has_cursor);
}

// on a SURFACE-backed toplevel the host HWND OUTLIVES the X toplevel
// (Model A), so an unregister-while-surface-live leaves the window alive. The cursor installed
// during that lifecycle must be CLEARED on unregister (not merely have future stale sets rejected),
// else it survives into a re-register. Self-skips when no Vulkan instance/surface can be made.
void test_real_cursor_surface_clear() {
    worker::RealVulkanBackend backend("", "", /*gpu_required=*/false);
    const vkrpc::CreateInstanceResponse ci = backend.create_instance({});
    if (!ci.ok) {
        std::fprintf(stderr, "integration_real_backend: cursor-clear skipped (no Vulkan: %s)\n",
                     ci.reason.c_str());
        return;
    }
    vkrpc::CreateSurfaceRequest cs;
    cs.instance = ci.instance;
    cs.platform = "xcb";
    cs.xid = 0xF3;
    const vkrpc::CreateSurfaceResponse sr = backend.create_surface(cs);
    if (!sr.ok) {
        std::fprintf(stderr, "integration_real_backend: cursor-clear skipped (no surface: %s)\n",
                     sr.reason.c_str());
        return;
    }
    const HWND hwnd = backend.debug_surface_hwnd(sr.surface);
    VKR_CHECK(hwnd != nullptr);

    sidecar::SidecarRegisterToplevelRequest reg;
    reg.xid = 0xF3;
    reg.generation = 1;
    reg.role = "toplevel";
    VKR_CHECK(backend.register_toplevel(reg).applied);

    // Install a cursor at the live epoch -> built + bound to the SURFACE HWND.
    sidecar::SidecarSetCursorRequest c;
    c.xid = 0xF3;
    c.epoch = backend.debug_registry_epoch_for_xid(0xF3);
    c.width = 8;
    c.height = 6;
    c.xhot = 1;
    c.yhot = 1;
    c.format = sidecar::kCursorFormatBgra8;
    c.pixels.resize(static_cast<std::size_t>(8) * 6 * 4);
    for (std::size_t i = 0; i < c.pixels.size(); i += 4) {
        c.pixels[i + 0] = static_cast<char>(0x99);
        c.pixels[i + 1] = static_cast<char>(0x66);
        c.pixels[i + 2] = static_cast<char>(0x33);
        c.pixels[i + 3] = static_cast<char>(0xFF);
    }
    VKR_CHECK(backend.set_cursor(c).applied);
    VKR_CHECK(backend.debug_cursor_state({0xF3, 0, 0}).has_cursor); // installed on the surface HWND

    // Unregister WHILE the surface is live -> the HWND persists, but the installed cursor is
    // CLEARED.
    sidecar::SidecarUnregisterToplevelRequest u;
    u.xid = 0xF3;
    u.generation = 2;
    VKR_CHECK(backend.unregister_toplevel(u).applied);
    VKR_CHECK(backend.debug_surface_hwnd(sr.surface) == hwnd);       // the surface window survives
    VKR_CHECK(!backend.debug_cursor_state({0xF3, 0, 0}).has_cursor); // cursor was cleared

    vkrpc::HandleRequest ds;
    ds.handle = sr.surface;
    VKR_CHECK(backend.destroy_surface(ds).ok);
}

// the real backend's debug_enum_windows reflects the SAME registry the mock drives
// (mock == real -- the dual-platform mapping is pinned in unit_sidecar test_enum_windows_mock; this
// proves the real backend builds it identically). No Vulkan device needed (registry-only read).
void test_real_enum_windows() {
    worker::RealVulkanBackend backend("", "", /*gpu_required=*/false);

    sidecar::SidecarRegisterToplevelRequest a;
    a.xid = 0xC1;
    a.generation = 1;
    a.role = "toplevel";
    a.title = "alpha";
    a.x = 5;
    a.y = 6;
    a.width = 200;
    a.height = 100;
    VKR_CHECK(backend.register_toplevel(a).applied);

    sidecar::SidecarRegisterToplevelRequest b;
    b.xid = 0xC2;
    b.generation = 1;
    b.role = "toplevel";
    VKR_CHECK(backend.register_toplevel(b).applied);

    sidecar::SidecarDebugEnumWindowsResponse e = backend.debug_enum_windows({});
    VKR_CHECK(e.ok);
    VKR_CHECK_EQ(e.windows.size(), static_cast<std::size_t>(2));
    // Sorted by xid: [0] = 0xC1, [1] = 0xC2.
    VKR_CHECK_EQ(e.windows[0].xid, static_cast<std::uint64_t>(0xC1));
    VKR_CHECK_EQ(e.windows[0].representation, std::string("placeholder"));
    VKR_CHECK(e.windows[0].toplevel_registered && !e.windows[0].has_surface);
    VKR_CHECK_EQ(e.windows[0].epoch, backend.debug_registry_epoch_for_xid(0xC1));
    VKR_CHECK(e.windows[0].epoch != 0);
    VKR_CHECK(!e.windows[0].shown);
    VKR_CHECK_EQ(e.windows[0].role, std::string("toplevel"));
    VKR_CHECK_EQ(e.windows[0].title, std::string("alpha"));
    VKR_CHECK_EQ(e.windows[0].x, 5);
    VKR_CHECK_EQ(e.windows[0].width, static_cast<std::uint32_t>(200));
    VKR_CHECK_EQ(e.windows[1].xid, static_cast<std::uint64_t>(0xC2));

    // Unregister one -> only its sibling remains.
    sidecar::SidecarUnregisterToplevelRequest u;
    u.xid = 0xC1;
    u.generation = 2;
    VKR_CHECK(backend.unregister_toplevel(u).applied);
    e = backend.debug_enum_windows({});
    VKR_CHECK_EQ(e.windows.size(), static_cast<std::size_t>(1));
    VKR_CHECK_EQ(e.windows[0].xid, static_cast<std::uint64_t>(0xC2));
}

// A REAL classified popup -> an OWNED WS_POPUP host. Proves the static z-order (the
// popup's Win32 owner IS the owner toplevel's host window), the non-activating style
// (WS_EX_NOACTIVATE so a popup click never steals the owner's focus), and the owner-teardown
// CASCADE (unregistering the owner destroys the popup's HWND, not just the owner's). No Vulkan
// device (placeholder owner). Skips gracefully if the window thread cannot make a window.
void test_real_popup() {
    worker::RealVulkanBackend backend("", "", /*gpu_required=*/false);

    sidecar::SidecarRegisterToplevelRequest owner;
    owner.xid = 0xE0;
    owner.generation = 1;
    owner.role = "toplevel";
    owner.width = 800;
    owner.height = 600;
    VKR_CHECK(backend.register_toplevel(owner).applied);
    if (backend.debug_placeholder_hwnd_count() == 0) {
        std::fprintf(stderr, "integration_real_backend: real popup skipped (no window thread)\n");
        return;
    }
    const HWND owner_hwnd = backend.debug_placeholder_hwnd_for_xid(0xE0);
    VKR_CHECK(owner_hwnd != nullptr);

    sidecar::SidecarRegisterToplevelRequest popup;
    popup.xid = 0xE1;
    popup.generation = 1;
    popup.role = "popup";
    popup.override_redirect = true;
    popup.is_popup = true;
    popup.owner_xid = 0xE0;
    popup.popup_kind = static_cast<std::uint32_t>(sidecar::PopupKind::Menu);
    popup.x = 120;
    popup.y = 140;
    popup.width = 160;
    popup.height = 220;
    VKR_CHECK(backend.register_toplevel(popup).applied);
    const HWND popup_hwnd = backend.debug_placeholder_hwnd_for_xid(0xE1);
    VKR_CHECK(popup_hwnd != nullptr && popup_hwnd != owner_hwnd);

    // Static z-order: the popup's Win32 OWNER is the owner toplevel's host window (owned windows
    // always stack above their owner + reap with it). GetWindow/GetWindowLongPtr are
    // thread-agnostic.
    VKR_CHECK_EQ(GetWindow(popup_hwnd, GW_OWNER), owner_hwnd);
    // Non-activating: the popup never steals the owner's activation/focus.
    const LONG_PTR ex = GetWindowLongPtrW(popup_hwnd, GWL_EXSTYLE);
    VKR_CHECK((ex & WS_EX_NOACTIVATE) != 0);
    VKR_CHECK((ex & WS_EX_TOOLWINDOW) != 0);
    // It is a WS_POPUP (no overlapped caption/border).
    const LONG_PTR style = GetWindowLongPtrW(popup_hwnd, GWL_STYLE);
    VKR_CHECK((style & WS_POPUP) != 0);

    // The worker-visible proof carries the linkage.
    sidecar::SidecarDebugEnumWindowsResponse e = backend.debug_enum_windows({});
    VKR_CHECK_EQ(e.windows.size(), static_cast<std::size_t>(2));
    for (const sidecar::SidecarWindowInfo& w : e.windows) {
        if (w.xid == 0xE1) {
            VKR_CHECK(w.is_popup);
            VKR_CHECK_EQ(w.owner_xid, static_cast<std::uint64_t>(0xE0));
            VKR_CHECK_EQ(w.popup_kind, static_cast<std::uint32_t>(sidecar::PopupKind::Menu));
        }
    }

    // Owner-teardown CASCADE: unregistering the owner destroys BOTH host HWNDs (the registry drops
    // the orphaned popup; the backend destroys its HWND off-lock). Without the cascade the popup
    // HWND would survive as an orphan logical window.
    sidecar::SidecarUnregisterToplevelRequest u;
    u.xid = 0xE0;
    u.generation = 2;
    VKR_CHECK(backend.unregister_toplevel(u).applied);
    VKR_CHECK_EQ(backend.debug_placeholder_hwnd_count(), static_cast<std::size_t>(0));
    VKR_CHECK(backend.debug_placeholder_hwnd_for_xid(0xE1) == nullptr);
    VKR_CHECK_EQ(backend.debug_registry_entry_count(), static_cast<std::size_t>(0));
    VKR_CHECK(backend.debug_enum_windows({}).windows.empty());
}

// The pure X-root-client -> Win32-frame coordinate math (the single transform seam).
// No window / device needed. Proves: a WS_POPUP (no chrome) maps
// IDENTITY (frame == client, the popup placement); a chromed WS_OVERLAPPEDWINDOW offsets the
// FRAME so the CLIENT -- not the frame -- lands at the mapped coordinate (the menu-placement
// lesson); and the work-area origin is applied (the 1:1 single-monitor base).
void test_coordinate_helper() {
    using vkr::worker::windowing::map_root_client_to_win32_frame;
    using vkr::worker::windowing::map_win32_frame_to_root_client;
    const POINT work{100, 200};
    const int rx = 30, ry = 40, cw = 640, ch = 480;
    const LONG client_x = work.x + rx; // 130
    const LONG client_y = work.y + ry; // 240

    // preserve exact forward/inverse behavior at representative 100%, 150%, and 200%
    // monitor DPIs. DPI changes only the supplied non-client metrics; the coordinate offset stays
    // 1:1 physical.
    for (const UINT dpi : {96u, 144u, 192u}) {
        // WS_POPUP: identity (no non-client inset) -> the frame IS the client at the mapped origin.
        const RECT pop = map_root_client_to_win32_frame(rx, ry, cw, ch, work, WS_POPUP, 0, dpi);
        VKR_CHECK_EQ(pop.left, client_x);
        VKR_CHECK_EQ(pop.top, client_y);
        VKR_CHECK_EQ(pop.right, client_x + cw);
        VKR_CHECK_EQ(pop.bottom, client_y + ch);
        const auto pop_back = map_win32_frame_to_root_client(pop, work, WS_POPUP, 0, dpi);
        VKR_CHECK_EQ(pop_back.root_x, rx);
        VKR_CHECK_EQ(pop_back.root_y, ry);
        VKR_CHECK_EQ(pop_back.client_w, cw);
        VKR_CHECK_EQ(pop_back.client_h, ch);

        // WS_OVERLAPPEDWINDOW: the frame is offset by the caption/border so the CLIENT lands at
        // (client_x, client_y). Recover the client origin from the frame using the SAME inset Win32
        // reports for a zero-origin client, computed independently here.
        RECT inset{0, 0, cw, ch};
        AdjustWindowRectExForDpi(&inset, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
        const RECT win =
            map_root_client_to_win32_frame(rx, ry, cw, ch, work, WS_OVERLAPPEDWINDOW, 0, dpi);
        VKR_CHECK(win.left <= client_x);
        VKR_CHECK(win.top < client_y);
        VKR_CHECK_EQ(win.left - inset.left, client_x);
        VKR_CHECK_EQ(win.top - inset.top, client_y);
        VKR_CHECK(win.right - win.left >= cw);
        VKR_CHECK(win.bottom - win.top >= ch);
        const auto win_back =
            map_win32_frame_to_root_client(win, work, WS_OVERLAPPEDWINDOW, 0, dpi);
        VKR_CHECK_EQ(win_back.root_x, rx);
        VKR_CHECK_EQ(win_back.root_y, ry);
        VKR_CHECK_EQ(win_back.client_w, cw);
        VKR_CHECK_EQ(win_back.client_h, ch);
    }

    // Measured chrome remains authoritative even if GetDpiForWindow metadata later
    // changes. This is the live PMv1 shape observed on the desk: 8px left/right, 31px top, 8px
    // bottom remains realized after the HWND reports 144 DPI.
    worker::windowing::FrameInsets measured{8, 31, 8, 8, true};
    const RECT measured_frame = worker::windowing::map_root_client_to_win32_frame_with_insets(
        rx, ry, cw, ch, work, measured);
    VKR_CHECK_EQ(measured_frame.left, client_x - 8);
    VKR_CHECK_EQ(measured_frame.top, client_y - 31);
    VKR_CHECK_EQ(measured_frame.right, client_x + cw + 8);
    VKR_CHECK_EQ(measured_frame.bottom, client_y + ch + 8);

    //   live fixture: the worker's one process-wide session origin is signed and maps
    // guest coordinates through the virtual-desktop top-left, not the primary work area.
    vkr::worker::windowing::WindowThread::set_host_origin(-6400, -222);
    const POINT virtual_origin = vkr::worker::windowing::WindowThread::host_origin();
    VKR_CHECK_EQ(virtual_origin.x, -6400);
    VKR_CHECK_EQ(virtual_origin.y, -222);
    const RECT far_frame =
        map_root_client_to_win32_frame(6400, 222, 100, 80, virtual_origin, WS_POPUP, 0, 96);
    VKR_CHECK_EQ(far_frame.left, 0);
    VKR_CHECK_EQ(far_frame.top, 0);
    const auto far_back =
        map_win32_frame_to_root_client(far_frame, virtual_origin, WS_POPUP, 0, 96);
    VKR_CHECK_EQ(far_back.root_x, 6400);
    VKR_CHECK_EQ(far_back.root_y, 222);
    const POINT legacy = vkr::worker::windowing::win32_work_origin();
    vkr::worker::windowing::WindowThread::set_host_origin(legacy.x, legacy.y);
}

void test_realization_adoption_token() {
    worker::windowing::WindowSlot slot;

    // Exact position-only feedback echo: adopt even if the live transient client size differs.
    slot.set_pending_realization_adoption(97, 86, 400, 300);
    VKR_CHECK(slot.consume_pending_realization_adoption(97, 86, 400, 300, false, 97, 86, 256, 256));
    // One-shot: the same geometry without new feedback provenance is an ordinary authored apply.
    VKR_CHECK(
        !slot.consume_pending_realization_adoption(97, 86, 400, 300, false, 97, 86, 256, 256));

    // A different desired request invalidates the token permanently.
    slot.set_pending_realization_adoption(97, 86, 400, 300);
    VKR_CHECK(
        !slot.consume_pending_realization_adoption(120, 90, 400, 300, false, 97, 86, 400, 300));
    VKR_CHECK(
        !slot.consume_pending_realization_adoption(97, 86, 400, 300, false, 97, 86, 400, 300));

    // A host move while feedback is in flight also invalidates it.
    slot.set_pending_realization_adoption(97, 86, 400, 300);
    VKR_CHECK(
        !slot.consume_pending_realization_adoption(97, 86, 400, 300, false, 98, 86, 400, 300));

    // A resize echo is adopted only when the live client extent matches exactly.
    slot.set_pending_realization_adoption(97, 86, 480, 320);
    VKR_CHECK(!slot.consume_pending_realization_adoption(97, 86, 480, 320, true, 97, 86, 400, 300));
    slot.set_pending_realization_adoption(97, 86, 480, 320);
    VKR_CHECK(slot.consume_pending_realization_adoption(97, 86, 480, 320, true, 97, 86, 480, 320));
}

// a REAL live MOVE. Register a toplevel (a placeholder HWND -- no Vulkan device), drive
// update_toplevel with a new POSITION, and assert via include_actual that the host window's ACTUAL
// CLIENT origin converged to the authored X-root coordinate (mapped back through the work origin),
// the frame contains the client, the geometry seq advanced, and the apply did not clamp. Then a
// stale-generation move is dropped (no regression). Skips gracefully if no window thread.
void test_real_move() {
    worker::RealVulkanBackend backend("", "", /*gpu_required=*/false);

    sidecar::SidecarRegisterToplevelRequest reg;
    reg.xid = 0xD4D1;
    reg.generation = 1;
    reg.role = "toplevel";
    reg.x = 10;
    reg.y = 20;
    reg.width = 300;
    reg.height = 200;
    VKR_CHECK(backend.register_toplevel(reg).applied);
    if (backend.debug_placeholder_hwnd_count() == 0) {
        std::fprintf(stderr, "integration_real_backend: real move skipped (no window thread)\n");
        return;
    }

    sidecar::SidecarDebugEnumWindowsRequest q;
    q.include_actual = true;

    // Move to (300, 250). The host window (hidden placeholder) repositions live; include_actual
    // reports the ACTUAL converged client origin in X-root coords.
    sidecar::SidecarUpdateToplevelRequest mv;
    mv.xid = 0xD4D1;
    mv.generation = 2;
    mv.x = 300;
    mv.y = 250;
    mv.width = 300;
    mv.height = 200;
    VKR_CHECK(backend.update_toplevel(mv).applied);

    sidecar::SidecarDebugEnumWindowsResponse e = backend.debug_enum_windows(q);
    VKR_CHECK_EQ(e.windows.size(), static_cast<std::size_t>(1));
    const sidecar::SidecarWindowInfo& w = e.windows[0];
    VKR_CHECK(w.has_actual);
    // The hard convergence gate: the host CLIENT origin (X-root coords) == the authored position.
    VKR_CHECK_EQ(w.actual_x, 300);
    VKR_CHECK_EQ(w.actual_y, 250);
    VKR_CHECK(!w.clamped);
    VKR_CHECK(w.last_host_apply_seq != 0);
    const std::uint64_t seq_after_move = w.last_host_apply_seq;
    // The OUTER frame is at least client-sized (an overlapped placeholder has a caption/border, so
    // the frame is strictly larger; the helper offset the frame so the CLIENT landed at the
    // authored origin -- proven by actual_x/actual_y above, which are the client origin in root
    // coords).
    VKR_CHECK(w.frame_width >= w.actual_width);
    VKR_CHECK(w.frame_height >= w.actual_height);
    VKR_CHECK(w.actual_width > 0 && w.actual_height > 0);

    // A stale-generation move is dropped: the position does not regress + the seq does not change.
    sidecar::SidecarUpdateToplevelRequest stale;
    stale.xid = 0xD4D1;
    stale.generation = 2; // equal -> dropped
    stale.x = 5;
    stale.y = 5;
    stale.width = 300;
    stale.height = 200;
    VKR_CHECK(!backend.update_toplevel(stale).applied);
    e = backend.debug_enum_windows(q);
    VKR_CHECK_EQ(e.windows[0].actual_x, 300);
    VKR_CHECK_EQ(e.windows[0].actual_y, 250);
    VKR_CHECK_EQ(e.windows[0].last_host_apply_seq, seq_after_move);

    // A second, newer move converges to the new position with a strictly-greater seq.
    sidecar::SidecarUpdateToplevelRequest mv2;
    mv2.xid = 0xD4D1;
    mv2.generation = 3;
    mv2.x = 150;
    mv2.y = 90;
    mv2.width = 300;
    mv2.height = 200;
    VKR_CHECK(backend.update_toplevel(mv2).applied);
    e = backend.debug_enum_windows(q);
    VKR_CHECK_EQ(e.windows[0].actual_x, 150);
    VKR_CHECK_EQ(e.windows[0].actual_y, 90);
    VKR_CHECK(e.windows[0].last_host_apply_seq > seq_after_move);

    sidecar::SidecarUnregisterToplevelRequest u;
    u.xid = 0xD4D1;
    u.generation = 4;
    VKR_CHECK(backend.unregister_toplevel(u).applied);
}

// INITIAL PLACEMENT -- a toplevel that maps once and NEVER self-moves
// (the vkcube / OpenSCAD class) must already be at its X-root position. Register a
// placeholder at a nonzero origin and assert include_actual reports the host CLIENT origin
// converged WITHOUT any update_toplevel. FAILS before the replay fix (the placeholder stayed at
// CW_USEDEFAULT). No device.
void test_real_initial_placement() {
    worker::RealVulkanBackend backend("", "", /*gpu_required=*/false);

    sidecar::SidecarRegisterToplevelRequest reg;
    reg.xid = 0xD402;
    reg.generation = 1;
    reg.role = "toplevel";
    reg.x = 220;
    reg.y = 140;
    reg.width = 300;
    reg.height = 200;
    VKR_CHECK(backend.register_toplevel(reg).applied);
    if (backend.debug_placeholder_hwnd_count() == 0) {
        std::fprintf(
            stderr,
            "integration_real_backend: real initial-placement skipped (no window thread)\n");
        return;
    }

    sidecar::SidecarDebugEnumWindowsRequest q;
    q.include_actual = true;
    const sidecar::SidecarDebugEnumWindowsResponse e = backend.debug_enum_windows(q);
    VKR_CHECK_EQ(e.windows.size(), static_cast<std::size_t>(1));
    const sidecar::SidecarWindowInfo& w = e.windows[0];
    VKR_CHECK(w.has_actual);
    VKR_CHECK_EQ(w.actual_x, 220); // placed at register -- no update_toplevel needed
    VKR_CHECK_EQ(w.actual_y, 140);
    VKR_CHECK(!w.clamped);
    VKR_CHECK(w.last_host_apply_seq != 0);

    sidecar::SidecarUnregisterToplevelRequest u;
    u.xid = 0xD402;
    u.generation = 2;
    VKR_CHECK(backend.unregister_toplevel(u).applied);
}

// A REAL live RESIZE on a placeholder HWND. A non-authoritative pure MOVE leaves the
// client extent untouched (the app still owns it); a sidecar-AUTHORED resize (an update_toplevel
// that changed w/h) actually resizes the host client to the authored size (include_actual reports
// the converged client extent). No Vulkan device (placeholder). Skips gracefully if no window
// thread.
void test_real_resize() {
    worker::RealVulkanBackend backend("", "", /*gpu_required=*/false);

    sidecar::SidecarRegisterToplevelRequest reg;
    reg.xid = 0xD502;
    reg.generation = 1;
    reg.role = "toplevel";
    reg.x = 100;
    reg.y = 80;
    reg.width = 300;
    reg.height = 200;
    VKR_CHECK(backend.register_toplevel(reg).applied);
    if (backend.debug_placeholder_hwnd_count() == 0) {
        std::fprintf(stderr, "integration_real_backend: real resize skipped (no window thread)\n");
        return;
    }

    sidecar::SidecarDebugEnumWindowsRequest q;
    q.include_actual = true;
    auto actual_of = [&](std::uint64_t xid, std::uint32_t& cw, std::uint32_t& ch) {
        for (const sidecar::SidecarWindowInfo& info : backend.debug_enum_windows(q).windows) {
            if (info.xid == xid) {
                cw = info.actual_width;
                ch = info.actual_height;
                return;
            }
        }
    };
    std::uint32_t w0 = 0, h0 = 0;
    actual_of(0xD502, w0, h0);
    VKR_CHECK(w0 > 0 && h0 > 0); // the placeholder client is sized at create (~300x200)

    // A pure MOVE (no size change) is NOT authoritative -> the client extent is unchanged.
    sidecar::SidecarUpdateToplevelRequest mv;
    mv.xid = 0xD502;
    mv.generation = 2;
    mv.x = 150;
    mv.y = 120;
    mv.width = 300;
    mv.height = 200;
    VKR_CHECK(backend.update_toplevel(mv).applied);
    std::uint32_t wm = 0, hm = 0;
    actual_of(0xD502, wm, hm);
    VKR_CHECK_EQ(wm, w0); // move did not resize
    VKR_CHECK_EQ(hm, h0);

    // A sidecar-AUTHORED resize -> the host client converges to the authored 520x360.
    sidecar::SidecarUpdateToplevelRequest rs;
    rs.xid = 0xD502;
    rs.generation = 3;
    rs.x = 150;
    rs.y = 120;
    rs.width = 520;
    rs.height = 360;
    VKR_CHECK(backend.update_toplevel(rs).applied);
    std::uint32_t wr = 0, hr = 0;
    actual_of(0xD502, wr, hr);
    VKR_CHECK_EQ(wr, static_cast<std::uint32_t>(520));
    VKR_CHECK_EQ(hr, static_cast<std::uint32_t>(360));

    // A TINY authored resize (1x1) Win32 clamps UP to the overlapped-window
    // minimum -- the achieved client is NOT 1x1, and include_actual surfaces the clamp
    // (clamped=true, actual extent > the authored 1x1). The realizable size, not the raw authored,
    // is what the proof reports (so an app pinned to it can converge -- the no-retry-loop fix).
    sidecar::SidecarUpdateToplevelRequest tiny;
    tiny.xid = 0xD502;
    tiny.generation = 4;
    tiny.x = 150;
    tiny.y = 120;
    tiny.width = 1;
    tiny.height = 1;
    VKR_CHECK(backend.update_toplevel(tiny).applied);
    sidecar::SidecarDebugEnumWindowsResponse te = backend.debug_enum_windows(q);
    for (const sidecar::SidecarWindowInfo& info : te.windows) {
        if (info.xid == 0xD502) {
            VKR_CHECK(info.has_actual);
            VKR_CHECK(info.actual_width >= 1 && info.actual_height >= 1); // a realizable client
            // The host realizes whatever Win32 allows for an authored 1x1 (it may clamp UP to the
            // window minimum, or realize 1x1 for a hidden window). The clamp flag is worker-visible
            // and TRUE iff the achieved client differs from the authored 1x1.
            const bool achieved_differs = info.actual_width != 1 || info.actual_height != 1;
            VKR_CHECK_EQ(info.clamped, achieved_differs);
        }
    }

    sidecar::SidecarUnregisterToplevelRequest u;
    u.xid = 0xD502;
    u.generation = 5;
    VKR_CHECK(backend.unregister_toplevel(u).applied);
}

// True iff window `a` is ABOVE `b` in the Win32 z-order: walk GW_HWNDNEXT (successively LOWER
// windows) from `a` and see if `b` is reached below it. Bounded (the desktop z-order is small).
// Both may be hidden -- hidden windows still hold a z-position SetWindowPos affects.
bool z_above(HWND a, HWND b) {
    HWND h = a;
    for (int i = 0; i < 20000 && h != nullptr; ++i) {
        h = GetWindow(h, GW_HWNDNEXT);
        if (h == b) {
            return true;
        }
    }
    return false;
}

// live z-order. Raise a toplevel -> its host HWND is at the top of the (non-topmost)
// stack; lower it -> at the bottom; an owned popup keeps riding its owner (raising the owner brings
// the popup with it, still above the owner). Worker-visible via GetWindow + DebugEnumWindows. No
// Vulkan device (placeholders; z-order applies to hidden windows). Skips if no window thread.
void test_real_z_order() {
    {
        worker::RealVulkanBackend backend("", "", /*gpu_required=*/false);
        auto reg_at = [&](std::uint64_t xid) {
            sidecar::SidecarRegisterToplevelRequest r;
            r.xid = xid;
            r.generation = 1;
            r.role = "toplevel";
            r.x = 40;
            r.y = 40;
            r.width = 200;
            r.height = 150;
            VKR_CHECK(backend.register_toplevel(r).applied);
        };
        reg_at(0xA01);
        reg_at(0xA02);
        reg_at(0xA03);
        const HWND a = backend.debug_placeholder_hwnd_for_xid(0xA01);
        const HWND b = backend.debug_placeholder_hwnd_for_xid(0xA02);
        const HWND c = backend.debug_placeholder_hwnd_for_xid(0xA03);
        if (a == nullptr || b == nullptr || c == nullptr) {
            std::fprintf(stderr, "integration_real_backend: z-order skipped (no window thread)\n");
            return;
        }

        // Environment gate: these checks need the session to honor SetWindowPos(HWND_TOP) for a
        // BACKGROUND process's windows. On a locked/disconnected desktop or under a dominating
        // always-on-top window (e.g. Task Manager), a background raise is a no-op -- NOT a vkrelay2
        // regression. Probe with the exact mechanism the test relies on (it also pre-raises A,
        // which the first assertion below expects anyway); SKIP cleanly if the session cannot
        // reorder.
        SetWindowPos(a, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        if (!z_above(a, c)) {
            std::fprintf(stderr,
                         "integration_real_backend: z-order skipped (session cannot reorder "
                         "background windows -- foreground lock / always-on-top)\n");
            return;
        }

        auto raise = [&](std::uint64_t xid, std::uint64_t gen, sidecar::ZOrder z) {
            sidecar::SidecarUpdateToplevelRequest m;
            m.xid = xid;
            m.generation = gen;
            m.x = 40;
            m.y = 40;
            m.width = 200;
            m.height = 150;
            m.z_order = static_cast<std::uint32_t>(z);
            VKR_CHECK(backend.update_toplevel(m).applied);
        };

        // Raise A -> A is above B and C.
        raise(0xA01, 2, sidecar::ZOrder::Raise);
        VKR_CHECK(z_above(a, b));
        VKR_CHECK(z_above(a, c));
        // The restack is worker-visible in DebugEnumWindows (sticky last z-order).
        for (const sidecar::SidecarWindowInfo& w : backend.debug_enum_windows({}).windows) {
            if (w.xid == 0xA01) {
                VKR_CHECK_EQ(w.z_order, static_cast<std::uint32_t>(sidecar::ZOrder::Raise));
            }
        }

        // Lower A -> A is now below B and C.
        raise(0xA01, 3, sidecar::ZOrder::Lower);
        VKR_CHECK(z_above(b, a));
        VKR_CHECK(z_above(c, a));

        sidecar::SidecarUnregisterToplevelRequest u;
        for (std::uint64_t xid : {0xA01ull, 0xA02ull, 0xA03ull}) {
            u.xid = xid;
            u.generation = 4;
            backend.unregister_toplevel(u);
        }
    }

    // Popup rides its owner: raising the owner toplevel keeps its owned WS_POPUP above it.
    {
        worker::RealVulkanBackend backend("", "", /*gpu_required=*/false);
        sidecar::SidecarRegisterToplevelRequest owner;
        owner.xid = 0xA10;
        owner.generation = 1;
        owner.role = "toplevel";
        owner.width = 400;
        owner.height = 300;
        VKR_CHECK(backend.register_toplevel(owner).applied);
        sidecar::SidecarRegisterToplevelRequest other;
        other.xid = 0xA11;
        other.generation = 1;
        other.role = "toplevel";
        other.width = 400;
        other.height = 300;
        VKR_CHECK(backend.register_toplevel(other).applied);
        const HWND ho = backend.debug_placeholder_hwnd_for_xid(0xA10);
        const HWND hother = backend.debug_placeholder_hwnd_for_xid(0xA11);
        if (ho == nullptr || hother == nullptr) {
            return; // no window thread
        }
        // Same environment gate as above: skip if this session cannot reorder background windows.
        SetWindowPos(ho, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        if (!z_above(ho, hother)) {
            std::fprintf(
                stderr, "integration_real_backend: z-order (popup) skipped (session cannot reorder "
                        "background windows)\n");
            return;
        }
        sidecar::SidecarRegisterToplevelRequest popup;
        popup.xid = 0xA12;
        popup.generation = 1;
        popup.role = "popup";
        popup.override_redirect = true;
        popup.is_popup = true;
        popup.owner_xid = 0xA10;
        popup.popup_kind = static_cast<std::uint32_t>(sidecar::PopupKind::Menu);
        popup.x = 20;
        popup.y = 20;
        popup.width = 120;
        popup.height = 160;
        VKR_CHECK(backend.register_toplevel(popup).applied);
        const HWND hp = backend.debug_placeholder_hwnd_for_xid(0xA12);
        VKR_CHECK(hp != nullptr);

        // Raise the owner above the other toplevel; its owned popup stays above the owner.
        sidecar::SidecarUpdateToplevelRequest m;
        m.xid = 0xA10;
        m.generation = 2;
        m.width = 400;
        m.height = 300;
        m.z_order = static_cast<std::uint32_t>(sidecar::ZOrder::Raise);
        VKR_CHECK(backend.update_toplevel(m).applied);
        VKR_CHECK(z_above(ho, hother)); // the owner was raised above the other toplevel
        VKR_CHECK(z_above(hp, ho));     // the popup rides its owner (owned windows stay above)
    }
}

// the REAL capture path -- debug_capture_window copies the actual window-thread chrome
// DIB / cursor image (mock == real: the mock returns the same statuses + pixels from its synthetic
// store, pinned in unit_sidecar test_capture_mock). No Vulkan device (placeholder + cursor). Skips
// gracefully if the window thread cannot make a window.
void test_real_capture() {
    worker::RealVulkanBackend backend("", "", /*gpu_required=*/false);

    // Absent before any window exists.
    VKR_CHECK_EQ(backend.debug_capture_window({0xCA, "chrome", 0, 0, 0}).status,
                 std::string("absent"));
    // Unknown layer.
    VKR_CHECK_EQ(backend.debug_capture_window({0xCA, "bogus", 0, 0, 0}).status,
                 std::string("bad_layer"));

    sidecar::SidecarRegisterToplevelRequest reg;
    reg.xid = 0xCA;
    reg.generation = 3;
    reg.width = 8;
    reg.height = 4;
    VKR_CHECK(backend.register_toplevel(reg).applied);
    if (backend.debug_placeholder_hwnd_count() == 0) {
        std::fprintf(stderr, "integration_real_backend: capture skipped (no window thread)\n");
        return;
    }

    // Chrome before any paint -> the DIB is empty -> "empty".
    VKR_CHECK_EQ(backend.debug_capture_window({0xCA, "chrome", 0, 0, 0}).status,
                 std::string("empty"));

    // Paint a solid 8x4 (B=0x11 G=0x22 R=0x33 A=0xFF) -> chrome captures the REAL DIB pixels.
    sidecar::SidecarPaintChromeRequest paint;
    paint.xid = 0xCA;
    paint.lifecycle_generation = 3;
    paint.seq = 1;
    paint.src_w = 8;
    paint.src_h = 4;
    paint.dirty_w = 8;
    paint.dirty_h = 4;
    paint.stride = 32;
    paint.pixels.resize(32 * 4);
    for (std::size_t i = 0; i < paint.pixels.size(); i += 4) {
        paint.pixels[i + 0] = static_cast<char>(0x11);
        paint.pixels[i + 1] = static_cast<char>(0x22);
        paint.pixels[i + 2] = static_cast<char>(0x33);
        paint.pixels[i + 3] = static_cast<char>(0xFF);
    }
    VKR_CHECK(backend.paint_chrome(paint).applied);

    const sidecar::SidecarDebugCaptureWindowResponse cap =
        backend.debug_capture_window({0xCA, "chrome", 0, 0, 0});
    VKR_CHECK(cap.ok && cap.status == "ok");
    VKR_CHECK_EQ(cap.representation, std::string("placeholder"));
    VKR_CHECK(cap.shown);
    VKR_CHECK_EQ(cap.width, static_cast<std::uint32_t>(8));
    VKR_CHECK_EQ(cap.height, static_cast<std::uint32_t>(4));
    VKR_CHECK_EQ(cap.stride, static_cast<std::uint32_t>(32));
    VKR_CHECK_EQ(cap.pixels.size(), static_cast<std::size_t>(8 * 4 * 4));
    // Every pixel is the painted BGRA (top-down, tight).
    VKR_CHECK_EQ(static_cast<unsigned char>(cap.pixels[0]), static_cast<unsigned char>(0x11));
    VKR_CHECK_EQ(static_cast<unsigned char>(cap.pixels[1]), static_cast<unsigned char>(0x22));
    VKR_CHECK_EQ(static_cast<unsigned char>(cap.pixels[2]), static_cast<unsigned char>(0x33));
    VKR_CHECK_EQ(static_cast<unsigned char>(cap.pixels[3]), static_cast<unsigned char>(0xFF));

    // A wrong lifecycle selector -> mismatch.
    sidecar::SidecarDebugCaptureWindowRequest mreq{0xCA, "chrome", 0, 0, 0};
    mreq.expected_lifecycle_generation = 999;
    VKR_CHECK_EQ(backend.debug_capture_window(mreq).status, std::string("mismatch"));

    // Cursor: none built -> empty; build one (registered + live epoch) -> capture + hotspot.
    VKR_CHECK_EQ(backend.debug_capture_window({0xCA, "cursor", 0, 0, 0}).status,
                 std::string("empty"));
    sidecar::SidecarSetCursorRequest cur;
    cur.xid = 0xCA;
    cur.epoch = backend.debug_registry_epoch_for_xid(0xCA);
    cur.width = 6;
    cur.height = 4;
    cur.xhot = 2;
    cur.yhot = 1;
    cur.format = sidecar::kCursorFormatBgra8;
    cur.pixels.resize(static_cast<std::size_t>(6) * 4 * 4);
    for (std::size_t i = 0; i < cur.pixels.size(); i += 4) {
        cur.pixels[i + 0] = static_cast<char>(0x99);
        cur.pixels[i + 1] = static_cast<char>(0x66);
        cur.pixels[i + 2] = static_cast<char>(0x33);
        cur.pixels[i + 3] = static_cast<char>(0xFF);
    }
    VKR_CHECK(backend.set_cursor(cur).applied);
    const sidecar::SidecarDebugCaptureWindowResponse ccap =
        backend.debug_capture_window({0xCA, "cursor", 0, 0, 0});
    VKR_CHECK(ccap.ok && ccap.status == "ok");
    VKR_CHECK_EQ(ccap.width, static_cast<std::uint32_t>(6));
    VKR_CHECK_EQ(ccap.height, static_cast<std::uint32_t>(4));
    VKR_CHECK_EQ(ccap.xhot, 2);
    VKR_CHECK_EQ(ccap.yhot, 1);
    VKR_CHECK_EQ(ccap.pixels.size(), static_cast<std::size_t>(6 * 4 * 4));
    VKR_CHECK_EQ(static_cast<unsigned char>(ccap.pixels[0]), static_cast<unsigned char>(0x99));

    sidecar::SidecarUnregisterToplevelRequest u;
    u.xid = 0xCA;
    u.generation = 4;
    VKR_CHECK(backend.unregister_toplevel(u).applied);
    VKR_CHECK_EQ(backend.debug_capture_window({0xCA, "chrome", 0, 0, 0}).status,
                 std::string("absent"));
}

// The optional debug HWND title tag. Off by default the worker window title stays the
// generic "vkrelay2"; with VKRELAY2_DEBUG_WINDOW_TITLES set, it carries the xid so the
// capture_window.ps1 dev helper can correlate HWND -> toplevel. Reads the title straight from the
// HWND (GetWindowTextA is cross-thread). Skips if the window thread cannot make a window.
void test_real_window_title_tag() {
    auto title_of = [](HWND hwnd) {
        char buf[256];
        const int n = GetWindowTextA(hwnd, buf, static_cast<int>(sizeof(buf)));
        return std::string(buf, n > 0 ? static_cast<std::size_t>(n) : 0);
    };

    // Default (flag off / empty): generic title.
    _putenv_s("VKRELAY2_DEBUG_WINDOW_TITLES", "");
    {
        worker::RealVulkanBackend backend("", "", /*gpu_required=*/false);
        sidecar::SidecarRegisterToplevelRequest reg;
        reg.xid = 0xD7;
        reg.generation = 1;
        reg.width = 8;
        reg.height = 4;
        VKR_CHECK(backend.register_toplevel(reg).applied);
        const HWND hwnd = backend.debug_placeholder_hwnd_for_xid(0xD7);
        if (hwnd == nullptr) {
            std::fprintf(stderr,
                         "integration_real_backend: title tag skipped (no window thread)\n");
            return;
        }
        VKR_CHECK_EQ(title_of(hwnd), std::string("vkrelay2"));
    }

    // Flag on: the title carries the xid (lowercase hex, no leading zeros).
    _putenv_s("VKRELAY2_DEBUG_WINDOW_TITLES", "1");
    {
        worker::RealVulkanBackend backend("", "", /*gpu_required=*/false);
        sidecar::SidecarRegisterToplevelRequest reg;
        reg.xid = 0xD8;
        reg.generation = 1;
        reg.width = 8;
        reg.height = 4;
        VKR_CHECK(backend.register_toplevel(reg).applied);
        const HWND hwnd = backend.debug_placeholder_hwnd_for_xid(0xD8);
        VKR_CHECK(hwnd != nullptr);
        VKR_CHECK_EQ(title_of(hwnd), std::string("vkrelay2 [xid=0xd8]"));
    }

    _putenv_s("VKRELAY2_DEBUG_WINDOW_TITLES", ""); // restore off (do not leak to other tests)
}

// helpers: a solid whole-window chrome paint (so a placeholder is paint-eligible + shown),
// and a lookup of an xid's enum row.
sidecar::SidecarPaintChromeRequest solid_paint(std::uint64_t xid, std::uint64_t gen,
                                               std::uint64_t seq, int w, int h) {
    sidecar::SidecarPaintChromeRequest p;
    p.xid = xid;
    p.lifecycle_generation = gen;
    p.seq = seq;
    p.src_w = static_cast<std::uint32_t>(w);
    p.src_h = static_cast<std::uint32_t>(h);
    p.dirty_w = static_cast<std::uint32_t>(w);
    p.dirty_h = static_cast<std::uint32_t>(h);
    p.stride = static_cast<std::uint32_t>(w) * 4;
    p.pixels.resize(static_cast<std::size_t>(w) * h * 4);
    for (std::size_t i = 0; i < p.pixels.size(); i += 4) {
        p.pixels[i + 0] = static_cast<char>(0x40);
        p.pixels[i + 1] = static_cast<char>(0x80);
        p.pixels[i + 2] = static_cast<char>(0xC0);
        p.pixels[i + 3] = static_cast<char>(0xFF);
    }
    return p;
}

const sidecar::SidecarWindowInfo* find_window(const sidecar::SidecarDebugEnumWindowsResponse& e,
                                              std::uint64_t xid) {
    for (const sidecar::SidecarWindowInfo& w : e.windows) {
        if (w.xid == xid) {
            return &w;
        }
    }
    return nullptr;
}

// A popup reconfigure is realized synchronously, including while hidden, and a subsequent
// visibility RPC reveals it at that new rectangle -- never at the prior same-XID placement. This
// pins both the mapped ConfigureNotify worker path and the remap Update->Visible worker ordering.
void test_real_popup_geometry_reveal_order() {
    worker::RealVulkanBackend backend("", "", /*gpu_required=*/false);
    sidecar::SidecarRegisterToplevelRequest owner;
    owner.xid = 0xF100;
    owner.generation = 1;
    owner.role = "toplevel";
    owner.x = 100;
    owner.y = 100;
    owner.width = 500;
    owner.height = 400;
    VKR_CHECK(backend.register_toplevel(owner).applied);
    if (backend.debug_placeholder_hwnd_count() == 0) {
        std::fprintf(stderr, "integration_real_backend: popup geometry skipped (no thread)\n");
        return;
    }

    sidecar::SidecarRegisterToplevelRequest popup;
    popup.xid = 0xF101;
    popup.generation = 1;
    popup.role = "popup";
    popup.override_redirect = true;
    popup.is_popup = true;
    popup.owner_xid = owner.xid;
    popup.popup_kind = static_cast<std::uint32_t>(sidecar::PopupKind::Menu);
    popup.x = 140;
    popup.y = 130;
    popup.width = 160;
    popup.height = 220;
    VKR_CHECK(backend.register_toplevel(popup).applied);
    const HWND popup_hwnd = backend.debug_placeholder_hwnd_for_xid(popup.xid);
    VKR_CHECK(popup_hwnd != nullptr);
    VKR_CHECK(backend.paint_chrome(solid_paint(popup.xid, 1, 1, popup.width, popup.height)).shown);

    sidecar::SidecarDebugEnumWindowsRequest q;
    q.include_actual = true;
    auto check_popup = [&](int x, int y, bool visible) {
        const sidecar::SidecarDebugEnumWindowsResponse e = backend.debug_enum_windows(q);
        const sidecar::SidecarWindowInfo* w = find_window(e, popup.xid);
        VKR_CHECK(w != nullptr && w->has_actual);
        VKR_CHECK_EQ(w->x, x);
        VKR_CHECK_EQ(w->y, y);
        VKR_CHECK_EQ(w->actual_x, x);
        VKR_CHECK_EQ(w->actual_y, y);
        VKR_CHECK_EQ(w->actual_width, popup.width);
        VKR_CHECK_EQ(w->actual_height, popup.height);
        VKR_CHECK_EQ(w->host_visible, visible);
        VKR_CHECK_EQ(backend.debug_placeholder_hwnd_for_xid(popup.xid), popup_hwnd); // same HWND
    };
    auto update = [&](std::uint64_t gen, int x, int y) {
        sidecar::SidecarUpdateToplevelRequest u;
        u.xid = popup.xid;
        u.generation = gen;
        u.x = x;
        u.y = y;
        u.width = popup.width;
        u.height = popup.height;
        VKR_CHECK(backend.update_toplevel(u).applied);
    };
    auto visibility = [&](std::uint64_t gen, sidecar::VisibilityState state) {
        sidecar::SidecarSetVisibilityRequest v;
        v.xid = popup.xid;
        v.generation = gen;
        v.visibility_state = static_cast<std::uint32_t>(state);
        VKR_CHECK(backend.set_visibility(v).applied);
    };

    // Mapped popup: the geometry-bearing update moves the already-visible HWND immediately.
    update(2, 360, 240);
    check_popup(360, 240, true);

    // Same-XID remap shape: hide, move while hidden, then reveal. The first sample after the
    // visibility call must already have the new rect, not merely converge eventually.
    visibility(3, sidecar::VisibilityState::Hidden);
    update(4, 620, 310);
    check_popup(620, 310, false);
    visibility(5, sidecar::VisibilityState::Visible);
    check_popup(620, 310, true);

    sidecar::SidecarUnregisterToplevelRequest unreg;
    unreg.xid = owner.xid;
    unreg.generation = 2;
    VKR_CHECK(backend.unregister_toplevel(unreg).applied); // owner cascade destroys popup too
}

// deterministic pin: a synthetic DPI suggested rectangle may be arbitrarily scaled/moved, but
// every slot-attached host representation keeps its current physical rectangle. Surface HWNDs use
// this same WndProc/WindowSlot path; exercise both overlapped placeholder and owned WS_POPUP
// styles.
void test_real_dpi_suggested_resize_suppressed() {
    worker::RealVulkanBackend backend("", "", /*gpu_required=*/false);
    sidecar::SidecarRegisterToplevelRequest owner;
    owner.xid = 0xF200;
    owner.generation = 1;
    owner.role = "toplevel";
    owner.x = 200;
    owner.y = 180;
    owner.width = 480;
    owner.height = 320;
    VKR_CHECK(backend.register_toplevel(owner).applied);
    if (backend.debug_placeholder_hwnd_count() == 0) {
        std::fprintf(stderr, "integration_real_backend: dpi-suggested-rect skipped (no thread)\n");
        return;
    }

    sidecar::SidecarRegisterToplevelRequest popup;
    popup.xid = 0xF201;
    popup.generation = 1;
    popup.role = "popup";
    popup.override_redirect = true;
    popup.is_popup = true;
    popup.owner_xid = owner.xid;
    popup.popup_kind = static_cast<std::uint32_t>(sidecar::PopupKind::Menu);
    popup.x = 240;
    popup.y = 220;
    popup.width = 120;
    popup.height = 180;
    VKR_CHECK(backend.register_toplevel(popup).applied);

    auto assert_ignored = [&](HWND hwnd) {
        VKR_CHECK(hwnd != nullptr);
        RECT frame_before{};
        RECT client_before{};
        VKR_CHECK(GetWindowRect(hwnd, &frame_before) != FALSE);
        VKR_CHECK(GetClientRect(hwnd, &client_before) != FALSE);
        const int fw = frame_before.right - frame_before.left;
        const int fh = frame_before.bottom - frame_before.top;
        RECT suggested{frame_before.left + 211, frame_before.top + 137,
                       frame_before.left + 211 + (fw * 3) / 2,
                       frame_before.top + 137 + (fh * 3) / 2};
        SendMessageW(hwnd, WM_DPICHANGED, MAKEWPARAM(144, 144),
                     reinterpret_cast<LPARAM>(&suggested));
        RECT frame_after{};
        RECT client_after{};
        VKR_CHECK(GetWindowRect(hwnd, &frame_after) != FALSE);
        VKR_CHECK(GetClientRect(hwnd, &client_after) != FALSE);
        VKR_CHECK_EQ(frame_after.left, frame_before.left);
        VKR_CHECK_EQ(frame_after.top, frame_before.top);
        VKR_CHECK_EQ(frame_after.right, frame_before.right);
        VKR_CHECK_EQ(frame_after.bottom, frame_before.bottom);
        VKR_CHECK_EQ(client_after.right - client_after.left,
                     client_before.right - client_before.left);
        VKR_CHECK_EQ(client_after.bottom - client_after.top,
                     client_before.bottom - client_before.top);
    };
    const HWND owner_hwnd = backend.debug_placeholder_hwnd_for_xid(owner.xid);
    assert_ignored(owner_hwnd);
    assert_ignored(backend.debug_placeholder_hwnd_for_xid(popup.xid));

    // The real PMv1 modal-loop shape first proposes a DPI-scaled WINDOWPOS, before delivering
    // WM_DPICHANGED. Reject that scaled extent in WM_WINDOWPOSCHANGING, then adopt only the DPI
    // notification's drag anchor. This pins the no apply-scaled-then-restore invariant directly.
    RECT modal_before{};
    RECT modal_client_before{};
    VKR_CHECK(GetWindowRect(owner_hwnd, &modal_before) != FALSE);
    VKR_CHECK(GetClientRect(owner_hwnd, &modal_client_before) != FALSE);
    const int modal_w = modal_before.right - modal_before.left;
    const int modal_h = modal_before.bottom - modal_before.top;
    RECT modal_suggested{modal_before.left + 173, modal_before.top + 91,
                         modal_before.left + 173 + (modal_w * 3) / 2,
                         modal_before.top + 91 + (modal_h * 3) / 2};
    SendMessageW(owner_hwnd, WM_ENTERSIZEMOVE, 0, 0);
    WINDOWPOS caption_proposal{};
    caption_proposal.hwnd = owner_hwnd;
    caption_proposal.x = modal_suggested.left;
    caption_proposal.y = modal_suggested.top;
    caption_proposal.cx = (modal_w * 3) / 2;
    caption_proposal.cy = (modal_h * 3) / 2;
    SendMessageW(owner_hwnd, WM_WINDOWPOSCHANGING, 0, reinterpret_cast<LPARAM>(&caption_proposal));
    VKR_CHECK_EQ(caption_proposal.cx, modal_w);
    VKR_CHECK_EQ(caption_proposal.cy, modal_h);
    SendMessageW(owner_hwnd, WM_DPICHANGED, MAKEWPARAM(144, 144),
                 reinterpret_cast<LPARAM>(&modal_suggested));
    SendMessageW(owner_hwnd, WM_EXITSIZEMOVE, 0, 0);
    RECT modal_after{};
    RECT modal_client_after{};
    VKR_CHECK(GetWindowRect(owner_hwnd, &modal_after) != FALSE);
    VKR_CHECK(GetClientRect(owner_hwnd, &modal_client_after) != FALSE);
    VKR_CHECK_EQ(modal_after.left, modal_suggested.left);
    VKR_CHECK_EQ(modal_after.top, modal_suggested.top);
    VKR_CHECK_EQ(modal_after.right - modal_after.left, modal_w);
    VKR_CHECK_EQ(modal_after.bottom - modal_after.top, modal_h);
    VKR_CHECK_EQ(modal_client_after.right - modal_client_after.left,
                 modal_client_before.right - modal_client_before.left);
    VKR_CHECK_EQ(modal_client_after.bottom - modal_client_after.top,
                 modal_client_before.bottom - modal_client_before.top);

    // A genuine border resize is not frozen to the gesture's starting extent: WM_SIZING updates
    // the physical user intent, and the same WINDOWPOS guard preserves that latest extent.
    RECT sizing{0, 0, modal_w + 97, modal_h + 53};
    SendMessageW(owner_hwnd, WM_ENTERSIZEMOVE, 0, 0);
    SendMessageW(owner_hwnd, WM_SIZING, WMSZ_BOTTOMRIGHT, reinterpret_cast<LPARAM>(&sizing));
    WINDOWPOS resize_proposal{};
    resize_proposal.hwnd = owner_hwnd;
    resize_proposal.cx = (sizing.right - sizing.left) * 3 / 2;
    resize_proposal.cy = (sizing.bottom - sizing.top) * 3 / 2;
    SendMessageW(owner_hwnd, WM_WINDOWPOSCHANGING, 0, reinterpret_cast<LPARAM>(&resize_proposal));
    VKR_CHECK_EQ(resize_proposal.cx, sizing.right - sizing.left);
    VKR_CHECK_EQ(resize_proposal.cy, sizing.bottom - sizing.top);
    SendMessageW(owner_hwnd, WM_EXITSIZEMOVE, 0, 0);

    sidecar::SidecarUnregisterToplevelRequest unreg;
    unreg.xid = owner.xid;
    unreg.generation = 2;
    VKR_CHECK(backend.unregister_toplevel(unreg).applied);
}

// A REAL hide/show lifecycle. Register + paint + show a placeholder, then HIDE it
// (set_visibility Hidden) and assert the host window is NOT visible while the REPRESENTATION +
// epoch SURVIVE (the bug fix: a hide is not a teardown). Restore (Visible) -> visible again at the
// SAME epoch (no rebuild). A stale-generation visibility op is dropped. The hard gate. Skips if
// no window thread.
void test_real_hide_show() {
    worker::RealVulkanBackend backend("", "", /*gpu_required=*/false);
    sidecar::SidecarRegisterToplevelRequest reg;
    reg.xid = 0x4D4A;
    reg.generation = 1;
    reg.role = "toplevel";
    reg.width = 16;
    reg.height = 8;
    VKR_CHECK(backend.register_toplevel(reg).applied);
    if (backend.debug_placeholder_hwnd_count() == 0) {
        std::fprintf(stderr, "integration_real_backend: hide/show skipped (no window thread)\n");
        return;
    }
    VKR_CHECK(backend.paint_chrome(solid_paint(0x4D4A, 1, 1, 16, 8)).shown); // shown after paint
    const std::uint64_t epoch0 = backend.debug_registry_epoch_for_xid(0x4D4A);

    sidecar::SidecarDebugEnumWindowsRequest q;
    q.include_actual = true;
    {
        const sidecar::SidecarDebugEnumWindowsResponse e = backend.debug_enum_windows(q);
        const sidecar::SidecarWindowInfo* w = find_window(e, 0x4D4A);
        VKR_CHECK(w != nullptr && w->has_actual && w->host_visible);
        VKR_CHECK_EQ(w->visibility_state,
                     static_cast<std::uint32_t>(sidecar::VisibilityState::Visible));
    }

    sidecar::SidecarSetVisibilityRequest hide;
    hide.xid = 0x4D4A;
    hide.generation = 2;
    hide.visibility_state = static_cast<std::uint32_t>(sidecar::VisibilityState::Hidden);
    VKR_CHECK(backend.set_visibility(hide).applied);
    {
        const sidecar::SidecarDebugEnumWindowsResponse e = backend.debug_enum_windows(q);
        const sidecar::SidecarWindowInfo* w = find_window(e, 0x4D4A);
        VKR_CHECK(w != nullptr && !w->host_visible); // SW_HIDE
        VKR_CHECK_EQ(w->visibility_state,
                     static_cast<std::uint32_t>(sidecar::VisibilityState::Hidden));
    }
    // The bug fix: the representation + epoch SURVIVED the hide (not a teardown).
    VKR_CHECK_EQ(backend.debug_registry_epoch_for_xid(0x4D4A), epoch0);
    VKR_CHECK(backend.debug_placeholder_hwnd_for_xid(0x4D4A) != nullptr);

    sidecar::SidecarSetVisibilityRequest show;
    show.xid = 0x4D4A;
    show.generation = 3;
    show.visibility_state = static_cast<std::uint32_t>(sidecar::VisibilityState::Visible);
    VKR_CHECK(backend.set_visibility(show).applied);
    {
        const sidecar::SidecarDebugEnumWindowsResponse e = backend.debug_enum_windows(q);
        const sidecar::SidecarWindowInfo* w = find_window(e, 0x4D4A);
        VKR_CHECK(w != nullptr && w->host_visible); // restored (paint-eligible)
    }
    VKR_CHECK_EQ(backend.debug_registry_epoch_for_xid(0x4D4A), epoch0); // epoch unchanged

    // A stale-generation visibility op is dropped (no regression).
    sidecar::SidecarSetVisibilityRequest stale;
    stale.xid = 0x4D4A;
    stale.generation = 2; // < current 3
    stale.visibility_state = static_cast<std::uint32_t>(sidecar::VisibilityState::Hidden);
    VKR_CHECK(!backend.set_visibility(stale).applied);
    {
        const sidecar::SidecarDebugEnumWindowsResponse e = backend.debug_enum_windows(q);
        const sidecar::SidecarWindowInfo* w = find_window(e, 0x4D4A);
        VKR_CHECK(w != nullptr && w->host_visible); // still visible (stale hide dropped)
    }

    sidecar::SidecarUnregisterToplevelRequest u;
    u.xid = 0x4D4A;
    u.generation = 4;
    VKR_CHECK(backend.unregister_toplevel(u).applied);
    VKR_CHECK_EQ(backend.debug_placeholder_hwnd_count(), static_cast<std::size_t>(0));
}

// A destroy AFTER a hide still fully unregisters (the X window really went away). The
// hide kept the entry; the destroy must remove it. Skips if no window thread.
void test_real_destroy_after_hide() {
    worker::RealVulkanBackend backend("", "", /*gpu_required=*/false);
    sidecar::SidecarRegisterToplevelRequest reg;
    reg.xid = 0x4D4C;
    reg.generation = 1;
    reg.role = "toplevel";
    reg.width = 16;
    reg.height = 8;
    VKR_CHECK(backend.register_toplevel(reg).applied);
    if (backend.debug_placeholder_hwnd_count() == 0) {
        std::fprintf(stderr, "integration_real_backend: destroy-after-hide skipped (no thread)\n");
        return;
    }
    sidecar::SidecarSetVisibilityRequest hide;
    hide.xid = 0x4D4C;
    hide.generation = 2;
    hide.visibility_state = static_cast<std::uint32_t>(sidecar::VisibilityState::Hidden);
    VKR_CHECK(backend.set_visibility(hide).applied);
    VKR_CHECK(backend.debug_placeholder_hwnd_for_xid(0x4D4C) != nullptr); // entry kept across hide

    sidecar::SidecarUnregisterToplevelRequest u; // the real destroy
    u.xid = 0x4D4C;
    u.generation = 3;
    VKR_CHECK(backend.unregister_toplevel(u).applied);
    VKR_CHECK_EQ(backend.debug_placeholder_hwnd_count(), static_cast<std::size_t>(0));
    VKR_CHECK(backend.debug_enum_windows({}).windows.empty());
}

// unified reveal predicate: a hide/restore of an UNPAINTED
// placeholder must NOT reveal it on restore (intent Visible but not paint-eligible); the first
// paint AFTER the restore reveals it (the intent is honored at the paint-commit reveal too). Skips
// if no window thread.
void test_real_unpainted_not_revealed_on_restore() {
    worker::RealVulkanBackend backend("", "", /*gpu_required=*/false);
    sidecar::SidecarRegisterToplevelRequest reg;
    reg.xid = 0x4D4B;
    reg.generation = 1;
    reg.role = "toplevel";
    reg.width = 16;
    reg.height = 8;
    VKR_CHECK(backend.register_toplevel(reg).applied);
    if (backend.debug_placeholder_hwnd_count() == 0) {
        std::fprintf(stderr, "integration_real_backend: reveal-predicate skipped (no thread)\n");
        return;
    }
    sidecar::SidecarDebugEnumWindowsRequest q;
    q.include_actual = true;
    {
        const sidecar::SidecarDebugEnumWindowsResponse e = backend.debug_enum_windows(q);
        const sidecar::SidecarWindowInfo* w = find_window(e, 0x4D4B);
        VKR_CHECK(w != nullptr && !w->host_visible); // unpainted placeholder is hidden
    }
    sidecar::SidecarSetVisibilityRequest hide;
    hide.xid = 0x4D4B;
    hide.generation = 2;
    hide.visibility_state = static_cast<std::uint32_t>(sidecar::VisibilityState::Hidden);
    VKR_CHECK(backend.set_visibility(hide).applied);
    sidecar::SidecarSetVisibilityRequest show;
    show.xid = 0x4D4B;
    show.generation = 3;
    show.visibility_state = static_cast<std::uint32_t>(sidecar::VisibilityState::Visible);
    VKR_CHECK(backend.set_visibility(show).applied);
    {
        const sidecar::SidecarDebugEnumWindowsResponse e = backend.debug_enum_windows(q);
        const sidecar::SidecarWindowInfo* w = find_window(e, 0x4D4B);
        // Visible INTENT but still NOT paint-eligible -> NOT revealed.
        VKR_CHECK(w != nullptr && !w->host_visible);
        VKR_CHECK_EQ(w->visibility_state,
                     static_cast<std::uint32_t>(sidecar::VisibilityState::Visible));
    }
    // First paint (at the current generation 3) now reveals it -- the intent is Visible.
    VKR_CHECK(backend.paint_chrome(solid_paint(0x4D4B, 3, 1, 16, 8)).shown);
    {
        const sidecar::SidecarDebugEnumWindowsResponse e = backend.debug_enum_windows(q);
        const sidecar::SidecarWindowInfo* w = find_window(e, 0x4D4B);
        VKR_CHECK(w != nullptr && w->host_visible); // revealed on first paint (intent Visible)
    }
    sidecar::SidecarUnregisterToplevelRequest u;
    u.xid = 0x4D4B;
    u.generation = 4;
    VKR_CHECK(backend.unregister_toplevel(u).applied);
}

// popup visibility cascade (MUST-TEST): hiding an owner hides its
// owned popup; restoring the owner restores it; but an INDEPENDENTLY-hidden popup is NOT
// resurrected by an owner restore (each popup keeps its OWN visibility intent). Skips if no window
// thread.
void test_real_popup_visibility_cascade() {
    worker::RealVulkanBackend backend("", "", /*gpu_required=*/false);
    sidecar::SidecarRegisterToplevelRequest owner;
    owner.xid = 0x4D50;
    owner.generation = 1;
    owner.role = "toplevel";
    owner.width = 200;
    owner.height = 150;
    VKR_CHECK(backend.register_toplevel(owner).applied);
    if (backend.debug_placeholder_hwnd_count() == 0) {
        std::fprintf(stderr, "integration_real_backend: popup cascade skipped (no thread)\n");
        return;
    }
    sidecar::SidecarRegisterToplevelRequest popup;
    popup.xid = 0x4D51;
    popup.generation = 1;
    popup.role = "popup";
    popup.override_redirect = true;
    popup.is_popup = true;
    popup.owner_xid = 0x4D50;
    popup.popup_kind = static_cast<std::uint32_t>(sidecar::PopupKind::Menu);
    popup.width = 80;
    popup.height = 60;
    VKR_CHECK(backend.register_toplevel(popup).applied);
    VKR_CHECK(backend.paint_chrome(solid_paint(0x4D50, 1, 1, 200, 150)).shown);
    VKR_CHECK(backend.paint_chrome(solid_paint(0x4D51, 1, 1, 80, 60)).shown);

    sidecar::SidecarDebugEnumWindowsRequest q;
    q.include_actual = true;
    auto host_visible = [&](std::uint64_t xid) {
        const sidecar::SidecarDebugEnumWindowsResponse e = backend.debug_enum_windows(q);
        const sidecar::SidecarWindowInfo* w = find_window(e, xid);
        VKR_CHECK(w != nullptr);
        return w != nullptr && w->host_visible;
    };
    auto set_vis = [&](std::uint64_t xid, std::uint64_t gen, sidecar::VisibilityState s) {
        sidecar::SidecarSetVisibilityRequest r;
        r.xid = xid;
        r.generation = gen;
        r.visibility_state = static_cast<std::uint32_t>(s);
        VKR_CHECK(backend.set_visibility(r).applied);
    };
    VKR_CHECK(host_visible(0x4D50) && host_visible(0x4D51)); // both shown

    // Hide the OWNER -> the owned popup is cascaded hidden too.
    set_vis(0x4D50, 2, sidecar::VisibilityState::Hidden);
    VKR_CHECK(!host_visible(0x4D50));
    VKR_CHECK(!host_visible(0x4D51)); // cascade

    // Restore the OWNER -> both visible again (the popup's own intent is still Visible).
    set_vis(0x4D50, 3, sidecar::VisibilityState::Visible);
    VKR_CHECK(host_visible(0x4D50) && host_visible(0x4D51));

    // INDEPENDENTLY hide the popup (its own intent -> Hidden).
    set_vis(0x4D51, 2, sidecar::VisibilityState::Hidden);
    VKR_CHECK(host_visible(0x4D50) && !host_visible(0x4D51));

    // Hide + restore the OWNER -> the popup STAYS hidden (no resurrection of its own Hidden
    // intent).
    set_vis(0x4D50, 4, sidecar::VisibilityState::Hidden);
    set_vis(0x4D50, 5, sidecar::VisibilityState::Visible);
    VKR_CHECK(host_visible(0x4D50));
    VKR_CHECK(!host_visible(0x4D51)); // independently-hidden popup not resurrected

    // Owner teardown cascades both away.
    sidecar::SidecarUnregisterToplevelRequest u;
    u.xid = 0x4D50;
    u.generation = 6;
    VKR_CHECK(backend.unregister_toplevel(u).applied);
    VKR_CHECK_EQ(backend.debug_placeholder_hwnd_count(), static_cast<std::size_t>(0));
}

// the INVERSE coordinate seam round-trips the forward seam: a
// Win32 frame produced from an X-root client geometry maps back to exactly that geometry, for both
// a chromed overlapped window and a (identity) WS_POPUP, incl. a negative origin. Pure (no window).
void test_coordinate_inverse() {
    using vkr::worker::windowing::map_root_client_to_win32_frame;
    using vkr::worker::windowing::map_win32_frame_to_root_client;
    using vkr::worker::windowing::RootClientGeometry;
    const POINT work{100, 200};
    const UINT dpi = 96;
    auto round_trip = [&](int rx, int ry, int cw, int ch, DWORD style) {
        const RECT frame = map_root_client_to_win32_frame(rx, ry, cw, ch, work, style, 0, dpi);
        const RootClientGeometry g = map_win32_frame_to_root_client(frame, work, style, 0, dpi);
        VKR_CHECK_EQ(g.root_x, rx);
        VKR_CHECK_EQ(g.root_y, ry);
        VKR_CHECK_EQ(g.client_w, cw);
        VKR_CHECK_EQ(g.client_h, ch);
    };
    round_trip(30, 40, 640, 480, WS_OVERLAPPEDWINDOW);   // chromed: frame offset, recovered exactly
    round_trip(0, 0, 800, 600, WS_POPUP);                // identity (no chrome)
    round_trip(-50, -20, 320, 240, WS_OVERLAPPEDWINDOW); // negative root origin round-trips
}

// the Win32-USER-origin geometry feedback (the reverse direction). A simulated user
// modal move/commit (WM_ENTERSIZEMOVE -> SetWindowPos -> WM_EXITSIZEMOVE) enqueues exactly ONE
// GeometryRequest carrying the inverse-mapped X-root client origin; worker-ORIGIN applies (a
// sidecar-authored move/resize + a visibility change -- the set_client_extent / apply_visibility
// paths) enqueue NONE (the self-apply guard / forward-only-on-commit invariant).
// Skips if no window thread.
void test_real_user_geometry_feedback() {
    // The worker's window thread is per-monitor-DPI-aware (physical px). This test drives a
    // cross-thread SetWindowPos + computes frames via the coordinate seam, so the TEST thread must
    // share that DPI context -- else a scaled panel virtualizes this thread's SetWindowPos +
    // work-area query and the inverse-mapped rect would not match (same guard as the resize test
    // below).
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE);
    worker::RealVulkanBackend backend("", "", /*gpu_required=*/false);
    sidecar::SidecarRegisterToplevelRequest reg;
    reg.xid = 0x4DB1;
    reg.generation = 1;
    reg.role = "toplevel";
    reg.x = 40;
    reg.y = 50;
    reg.width = 320;
    reg.height = 240;
    VKR_CHECK(backend.register_toplevel(reg).applied);
    const HWND hwnd = backend.debug_placeholder_hwnd_for_xid(0x4DB1);
    if (hwnd == nullptr) {
        std::fprintf(stderr,
                     "integration_real_backend: user-feedback skipped (no window thread)\n");
        return;
    }

    // Simulate the user moving the host so its CLIENT lands at a NEW X-root origin (differs from
    // the register baseline, so the no-op-churn guard does not suppress), then the modal commit.
    const POINT work = vkr::worker::windowing::win32_work_origin();
    const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE));
    const DWORD exstyle = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
    const UINT dpi = GetDpiForWindow(hwnd);
    const int want_rx = 220, want_ry = 160;
    const RECT frame = vkr::worker::windowing::map_root_client_to_win32_frame(
        want_rx, want_ry, 320, 240, work, style, exstyle, dpi);
    SendMessageW(hwnd, WM_ENTERSIZEMOVE, 0, 0);
    SetWindowPos(hwnd, nullptr, frame.left, frame.top, frame.right - frame.left,
                 frame.bottom - frame.top, SWP_NOZORDER | SWP_NOACTIVATE);
    SendMessageW(hwnd, WM_EXITSIZEMOVE, 0, 0);

    sidecar::SidecarPollInputRequest p;
    p.since_seq = 0;
    const sidecar::SidecarPollInputResponse r = backend.poll_input(p);
    VKR_CHECK(r.ok);
    int geo = 0, got_rx = 0, got_ry = 0;
    for (const auto& e : r.events) {
        if (static_cast<sidecar::InputEventKind>(e.kind) ==
            sidecar::InputEventKind::GeometryRequest) {
            ++geo;
            got_rx = e.root_x;
            got_ry = e.root_y;
            VKR_CHECK_EQ(e.host_request,
                         static_cast<std::uint32_t>(sidecar::HostRequestKind::Geometry));
        }
    }
    VKR_CHECK_EQ(geo, 1); // exactly one committed user gesture forwarded
    VKR_CHECK_EQ(got_rx, want_rx);
    VKR_CHECK_EQ(got_ry, want_ry);

    // a user RESIZE gesture (the committed CLIENT size CHANGES) forwards the new
    // req_w/req_h -- the worker leg of "user resize -> authoritative extent". The sidecar then
    // authors the guest resize (only on a real size delta -- user_request_is_resize) and the echoed
    // w/h drives the extent_authoritative caps-pin (proven for a changed size in
    // test_real_capture; the sidecar's same-size-move-no-pin + guest convergence are in the
    // boundary smoke). Here we prove the worker forwards the user's NEW committed extent.
    const int rwant_rx = 220, rwant_ry = 160, rwant_w = 400, rwant_h = 300;
    const RECT rframe = vkr::worker::windowing::map_root_client_to_win32_frame(
        rwant_rx, rwant_ry, rwant_w, rwant_h, work, style, exstyle, dpi);
    SendMessageW(hwnd, WM_ENTERSIZEMOVE, 0, 0);
    RECT sizing_frame = rframe;
    SendMessageW(hwnd, WM_SIZING, WMSZ_BOTTOMRIGHT, reinterpret_cast<LPARAM>(&sizing_frame));
    SetWindowPos(hwnd, nullptr, rframe.left, rframe.top, rframe.right - rframe.left,
                 rframe.bottom - rframe.top, SWP_NOZORDER | SWP_NOACTIVATE);
    SendMessageW(hwnd, WM_EXITSIZEMOVE, 0, 0);
    sidecar::SidecarPollInputRequest rp;
    rp.since_seq = r.next_seq;
    const sidecar::SidecarPollInputResponse rr = backend.poll_input(rp);
    VKR_CHECK(rr.ok);
    int rgeo = 0, got_rw = 0, got_rh = 0, rgot_rx = 0, rgot_ry = 0;
    for (const auto& e : rr.events) {
        if (static_cast<sidecar::InputEventKind>(e.kind) ==
            sidecar::InputEventKind::GeometryRequest) {
            ++rgeo;
            rgot_rx = e.root_x;
            rgot_ry = e.root_y;
            got_rw = static_cast<int>(e.req_w);
            got_rh = static_cast<int>(e.req_h);
        }
    }
    VKR_CHECK_EQ(rgeo, 1); // exactly one committed user resize forwarded
    VKR_CHECK_EQ(rgot_rx, rwant_rx);
    VKR_CHECK_EQ(rgot_ry, rwant_ry);
    VKR_CHECK_EQ(got_rw, rwant_w); // the user's NEW committed extent rode in req_w/req_h
    VKR_CHECK_EQ(got_rh, rwant_h);

    // No-feedback invariant (the self-apply guard / commit-only path): WORKER-origin host mutations
    // must NOT enqueue a GeometryRequest. Drive a sidecar move + a sidecar resize
    // (set_client_extent)
    // + a visibility change (apply_visibility) -- the exact paths an early loop would hide.
    std::uint64_t cursor = rr.next_seq;
    sidecar::SidecarUpdateToplevelRequest mv;
    mv.xid = 0x4DB1;
    mv.generation = 2;
    mv.x = 300;
    mv.y = 300;
    mv.width = 320;
    mv.height = 240;
    VKR_CHECK(backend.update_toplevel(mv).applied);
    sidecar::SidecarUpdateToplevelRequest rs; // a resize -> set_client_extent + the latch
    rs.xid = 0x4DB1;
    rs.generation = 3;
    rs.x = 300;
    rs.y = 300;
    rs.width = 400;
    rs.height = 300;
    VKR_CHECK(backend.update_toplevel(rs).applied);
    sidecar::SidecarSetVisibilityRequest hide; // apply_visibility
    hide.xid = 0x4DB1;
    hide.generation = 4;
    hide.visibility_state = static_cast<std::uint32_t>(sidecar::VisibilityState::Hidden);
    VKR_CHECK(backend.set_visibility(hide).applied);
    sidecar::SidecarPollInputRequest p2;
    p2.since_seq = cursor;
    const sidecar::SidecarPollInputResponse r2 = backend.poll_input(p2);
    for (const auto& e : r2.events) {
        VKR_CHECK(static_cast<sidecar::InputEventKind>(e.kind) !=
                  sidecar::InputEventKind::GeometryRequest); // no worker-origin feedback loop
    }

    sidecar::SidecarUnregisterToplevelRequest u;
    u.xid = 0x4DB1;
    u.generation = 5;
    VKR_CHECK(backend.unregister_toplevel(u).applied);
}

// A Win32-USER minimize/restore (WM_SYSCOMMAND SC_MINIMIZE / SC_RESTORE) forwards a
// GeometryRequest{Minimize}/{Restore} (the reverse direction), and the worker's set_visibility
// apply reports the host as Iconic (IsIconic && IsWindowVisible -- taskbar-restorable, NOT SW_HIDE)
// or Visible. The sidecar's pending-iconify echo guard is a Linux-side concern proven by the
// boundary smoke (run_iconify_smoke); here we prove the WORKER legs. Skips if no window thread.
void test_real_user_minimize_restore() {
    worker::RealVulkanBackend backend("", "", /*gpu_required=*/false);
    sidecar::SidecarRegisterToplevelRequest reg;
    reg.xid = 0x4DB2;
    reg.generation = 1;
    reg.role = "toplevel";
    reg.x = 100; // ON-screen (no initial clamp or reconcile GeometryRequest to confuse the
    reg.y = 100; // minimize/restore assertions below); this test is position-independent.
    reg.width = 16;
    reg.height = 8;
    VKR_CHECK(backend.register_toplevel(reg).applied);
    const HWND hwnd = backend.debug_placeholder_hwnd_for_xid(0x4DB2);
    if (hwnd == nullptr) {
        std::fprintf(stderr, "integration_real_backend: minimize/restore skipped (no thread)\n");
        return;
    }
    // Paint-eligible so a later set_visibility(Visible) reveals (SW_SHOWNA) rather than SW_HIDE.
    VKR_CHECK(backend.paint_chrome(solid_paint(0x4DB2, 1, 1, 16, 8)).shown);

    sidecar::SidecarDebugEnumWindowsRequest q;
    q.include_actual = true;
    auto vis = [&](std::uint32_t* state, bool* visible, bool* iconic) {
        // Bind the response to a NAMED local before find_window -- it returns a pointer INTO the
        // response's vector, so the response must outlive the deref (a temporary would dangle).
        const sidecar::SidecarDebugEnumWindowsResponse e = backend.debug_enum_windows(q);
        const sidecar::SidecarWindowInfo* w = find_window(e, 0x4DB2);
        VKR_CHECK(w != nullptr);
        *state = w->visibility_state;
        *visible = w->host_visible;
        *iconic = w->host_iconic;
    };

    // (1) The user minimizes the host (the caption box / taskbar) -> WM_SYSCOMMAND SC_MINIMIZE. The
    // WndProc forwards exactly one GeometryRequest{Minimize} + falls through to DefWindowProcW.
    SendMessageW(hwnd, WM_SYSCOMMAND, SC_MINIMIZE, 0);
    sidecar::SidecarPollInputRequest p;
    p.since_seq = 0;
    const sidecar::SidecarPollInputResponse r = backend.poll_input(p);
    VKR_CHECK(r.ok);
    int mins = 0;
    for (const auto& e : r.events) {
        if (static_cast<sidecar::InputEventKind>(e.kind) ==
            sidecar::InputEventKind::GeometryRequest) {
            ++mins;
            VKR_CHECK_EQ(e.host_request,
                         static_cast<std::uint32_t>(sidecar::HostRequestKind::Minimize));
        }
    }
    VKR_CHECK_EQ(mins, 1);

    // (2) The sidecar authors Iconic (the echo) -> SW_SHOWMINNOACTIVE: the host is iconic AND still
    // IsWindowVisible (taskbar-restorable), NOT SW_HIDE. host_iconic && host_visible.
    sidecar::SidecarSetVisibilityRequest icon;
    icon.xid = 0x4DB2;
    icon.generation = 2;
    icon.visibility_state = static_cast<std::uint32_t>(sidecar::VisibilityState::Iconic);
    VKR_CHECK(backend.set_visibility(icon).applied);
    std::uint32_t st = 0;
    bool v = false, ic = false;
    vis(&st, &v, &ic);
    VKR_CHECK_EQ(st, static_cast<std::uint32_t>(sidecar::VisibilityState::Iconic));
    VKR_CHECK(v && ic); // IsWindowVisible && IsIconic -- not SW_HIDE

    // (3) The user restores (taskbar) -> WM_SYSCOMMAND SC_RESTORE -> one GeometryRequest{Restore}.
    SendMessageW(hwnd, WM_SYSCOMMAND, SC_RESTORE, 0);
    sidecar::SidecarPollInputRequest p2;
    p2.since_seq = r.next_seq;
    const sidecar::SidecarPollInputResponse r2 = backend.poll_input(p2);
    VKR_CHECK(r2.ok);
    int res = 0;
    for (const auto& e : r2.events) {
        if (static_cast<sidecar::InputEventKind>(e.kind) ==
            sidecar::InputEventKind::GeometryRequest) {
            ++res;
            VKR_CHECK_EQ(e.host_request,
                         static_cast<std::uint32_t>(sidecar::HostRequestKind::Restore));
        }
    }
    VKR_CHECK_EQ(res, 1);

    // (4) The sidecar authors Visible (the echo) -> the host is visible and NOT iconic.
    sidecar::SidecarSetVisibilityRequest show;
    show.xid = 0x4DB2;
    show.generation = 3;
    show.visibility_state = static_cast<std::uint32_t>(sidecar::VisibilityState::Visible);
    VKR_CHECK(backend.set_visibility(show).applied);
    vis(&st, &v, &ic);
    VKR_CHECK_EQ(st, static_cast<std::uint32_t>(sidecar::VisibilityState::Visible));
    VKR_CHECK(v && !ic); // visible, no longer iconic

    sidecar::SidecarUnregisterToplevelRequest u;
    u.xid = 0x4DB2;
    u.generation = 4;
    VKR_CHECK(backend.unregister_toplevel(u).applied);
}

// A Win32-USER MAXIMIZE feeds the ACTUAL maximized host geometry back to the
// guest (the reverse GeometryRequest path), and a stale sidecar-authored geometry can no longer
// unmaximize / snap the host off-screen while the user has it zoomed (the zoomed-host guard). This
// is the "do not fight a user who just maximized" fix. Proves the WORKER legs; the sidecar authors
// the guest move+resize from the echoed GeometryRequest (apply_user_geometry /
// user_request_is_resize, covered by test_real_user_geometry_feedback). Skips if no window thread.
// RED by construction: before the fix, SC_MAXIMIZE forwards nothing (geo==0) and the stale apply
// unmaximizes the host (IsZoomed false).
void test_real_user_maximize() {
    worker::RealVulkanBackend backend("", "", /*gpu_required=*/false);
    sidecar::SidecarRegisterToplevelRequest reg;
    reg.xid = 0x4DB3;
    reg.generation = 1;
    reg.role = "toplevel";
    reg.x = 40;
    reg.y = 50;
    reg.width = 320;
    reg.height = 240;
    VKR_CHECK(backend.register_toplevel(reg).applied);
    const HWND hwnd = backend.debug_placeholder_hwnd_for_xid(0x4DB3);
    if (hwnd == nullptr) {
        std::fprintf(stderr, "integration_real_backend: maximize skipped (no window thread)\n");
        return;
    }
    // Paint-eligible + shown, so it is a normal visible overlapped window that can be maximized.
    VKR_CHECK(backend.paint_chrome(solid_paint(0x4DB3, 1, 1, 320, 240)).shown);

    // (1) The user MAXIMIZES the host (caption box / system menu / Win+Up) -> WM_SYSCOMMAND
    // SC_MAXIMIZE -> DefWindowProcW maximizes (host is now IsZoomed) -> WM_SIZE(SIZE_MAXIMIZED) ->
    // the worker forwards EXACTLY ONE GeometryRequest carrying the ACTUAL maximized client origin +
    // extent (on-screen, larger than the 320x240 register size).
    SendMessageW(hwnd, WM_SYSCOMMAND, SC_MAXIMIZE, 0);
    VKR_CHECK(IsZoomed(hwnd)); // the host actually maximized
    sidecar::SidecarPollInputRequest p;
    p.since_seq = 0;
    const sidecar::SidecarPollInputResponse r = backend.poll_input(p);
    VKR_CHECK(r.ok);
    int geo = 0, gx = 0, gy = 0, gw = 0, gh = 0;
    for (const auto& e : r.events) {
        if (static_cast<sidecar::InputEventKind>(e.kind) ==
            sidecar::InputEventKind::GeometryRequest) {
            ++geo;
            gx = e.root_x;
            gy = e.root_y;
            gw = static_cast<int>(e.req_w);
            gh = static_cast<int>(e.req_h);
            VKR_CHECK_EQ(e.host_request,
                         static_cast<std::uint32_t>(sidecar::HostRequestKind::Geometry));
        }
    }
    VKR_CHECK_EQ(geo, 1);            // exactly one maximize feedback
    VKR_CHECK(gx >= 0 && gy >= 0);   // the maximized client origin is ON-screen
    VKR_CHECK(gw > 320 && gh > 240); // and larger than the normal size (the whole work area)

    // (2) The sidecar echoes that maximized geometry back (update_toplevel). While the host is
    // zoomed the guard suppresses the HWND move/size, so the echo is a no-op and it stays
    // maximized.
    sidecar::SidecarUpdateToplevelRequest echo;
    echo.xid = 0x4DB3;
    echo.generation = 2;
    echo.x = gx;
    echo.y = gy;
    echo.width = static_cast<std::uint32_t>(gw);
    echo.height = static_cast<std::uint32_t>(gh);
    VKR_CHECK(backend.update_toplevel(echo).applied);
    VKR_CHECK(IsZoomed(hwnd)); // still maximized after the echo

    // (3) THE BUG: a STALE off-screen authored geometry (the old default that the Win-key
    // activation round-trip re-asserts) drains while the host is maximized. The zoomed-host guard
    // keeps it maximized -- it must NOT unmaximize or snap the host to the off-screen default.
    sidecar::SidecarUpdateToplevelRequest stale;
    stale.xid = 0x4DB3;
    stale.generation = 3;
    stale.x = -32;
    stale.y = -32;
    stale.width = 320;
    stale.height = 240;
    VKR_CHECK(backend.update_toplevel(stale).applied);
    VKR_CHECK(IsZoomed(hwnd)); // STILL maximized -- the stale default did not win

    sidecar::SidecarUnregisterToplevelRequest u;
    u.xid = 0x4DB3;
    u.generation = 4;
    VKR_CHECK(backend.unregister_toplevel(u).applied);
}

// (cross-monitor maximize guard) -- interactive cap. Sets a SMALL synthetic guest root, then
// maximizes a toplevel on the (larger) real monitor: with the WM_GETMINMAXINFO cap the realized
// host CLIENT and the forwarded maximize GeometryRequest both stay <= the guest root, so the guest
// surface can never exceed its output (the cross-monitor stall trigger). RED without the cap: the
// realized client would be the whole monitor work area (>> the synthetic root). No GPU needed.
// Restores the process-global root on every path (it is shared across tests). Skips if no window
// thread.
void test_real_m0_maximize_cap() {
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE);
    // A guest root smaller than any real monitor work area, so a maximize WOULD exceed it uncapped.
    constexpr int kRootW = 400, kRootH = 300;
    worker::windowing::WindowThread::set_guest_root(kRootW, kRootH);
    struct RootGuard {
        ~RootGuard() { worker::windowing::WindowThread::set_guest_root(0, 0); }
    } root_guard;

    worker::RealVulkanBackend backend("", "", /*gpu_required=*/false);
    sidecar::SidecarRegisterToplevelRequest reg;
    reg.xid = 0x4DB4;
    reg.generation = 1;
    reg.role = "toplevel";
    reg.x = 40;
    reg.y = 50;
    reg.width = 320;
    reg.height = 240;
    VKR_CHECK(backend.register_toplevel(reg).applied);
    const HWND hwnd = backend.debug_placeholder_hwnd_for_xid(0x4DB4);
    if (hwnd == nullptr) {
        std::fprintf(stderr,
                     "integration_real_backend: m0 maximize-cap skipped (no window thread)\n");
        return;
    }
    VKR_CHECK(backend.paint_chrome(solid_paint(0x4DB4, 1, 1, 320, 240)).shown);

    SendMessageW(hwnd, WM_SYSCOMMAND, SC_MAXIMIZE, 0);
    VKR_CHECK(IsZoomed(hwnd)); // the host maximized (Win32 still zooms; we only capped the SIZE)

    // (1) The realized host CLIENT is capped to the guest root (never larger). Read on this
    // PMv1-aware thread so GetClientRect is physical px.
    RECT rc{};
    VKR_CHECK(GetClientRect(hwnd, &rc) != 0);
    const int cw = static_cast<int>(rc.right - rc.left);
    const int ch = static_cast<int>(rc.bottom - rc.top);
    VKR_CHECK(cw > 0 && ch > 0);
    VKR_CHECK(cw <= kRootW); // capped -- not the whole (larger) monitor work area
    VKR_CHECK(ch <= kRootH);

    // (2) The forwarded maximize feedback therefore carries an extent <= the guest root
    // too, so the sidecar authors a guest resize the guest output can realize.
    sidecar::SidecarPollInputRequest p;
    p.since_seq = 0;
    const sidecar::SidecarPollInputResponse r = backend.poll_input(p);
    VKR_CHECK(r.ok);
    int geo = 0;
    for (const auto& e : r.events) {
        if (static_cast<sidecar::InputEventKind>(e.kind) ==
            sidecar::InputEventKind::GeometryRequest) {
            ++geo;
            VKR_CHECK(static_cast<int>(e.req_w) <= kRootW);
            VKR_CHECK(static_cast<int>(e.req_h) <= kRootH);
        }
    }
    VKR_CHECK_EQ(geo, 1);

    sidecar::SidecarUnregisterToplevelRequest u;
    u.xid = 0x4DB4;
    u.generation = 2;
    VKR_CHECK(backend.unregister_toplevel(u).applied);
}

// Win32's default max-track is derived from the virtual screen and can be SMALLER than a
// guest-root client plus its outer chrome (7600x2160 client needs a 2199px outer height on this
// desk, while the default max-track is 2180). The worker must raise/lower ptMaxTrackSize exactly so
// a root-sized client realizes byte-for-byte, while the separate maximize test above proves
// ptMaxSize remains monitor-specific.
void test_real_m0_root_sized_track_cap() {
    constexpr int kRootW = 7600;
    constexpr int kRootH = 2160;
    worker::windowing::WindowThread::set_guest_root(kRootW, kRootH);
    struct RootGuard {
        ~RootGuard() { worker::windowing::WindowThread::set_guest_root(0, 0); }
    } root_guard;

    worker::RealVulkanBackend backend("", "", /*gpu_required=*/false);
    sidecar::SidecarRegisterToplevelRequest reg;
    reg.xid = 0x4DB6;
    reg.generation = 1;
    reg.role = "toplevel";
    reg.x = 100;
    reg.y = 100;
    reg.width = 320;
    reg.height = 240;
    VKR_CHECK(backend.register_toplevel(reg).applied);
    const HWND hwnd = backend.debug_placeholder_hwnd_for_xid(0x4DB6);
    if (hwnd == nullptr) {
        std::fprintf(stderr,
                     "integration_real_backend: m0 root-track skipped (no window thread)\n");
        return;
    }
    const worker::windowing::FrameInsets insets = worker::windowing::read_real_frame_insets(hwnd);
    VKR_CHECK(insets.valid);
    VKR_CHECK(SetWindowPos(hwnd, nullptr, 0, 0, kRootW + insets.left + insets.right,
                           kRootH + insets.top + insets.bottom,
                           SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE) != FALSE);
    RECT client{};
    VKR_CHECK(GetClientRect(hwnd, &client) != FALSE);
    VKR_CHECK_EQ(static_cast<int>(client.right - client.left), kRootW);
    VKR_CHECK_EQ(static_cast<int>(client.bottom - client.top), kRootH);

    sidecar::SidecarUnregisterToplevelRequest u;
    u.xid = 0x4DB6;
    u.generation = 2;
    VKR_CHECK(backend.unregister_toplevel(u).applied);
}

// (cross-monitor maximize guard) -- worker over-root guard + convergence. A deferred
// (zink-style) surface whose sidecar-authored client is driven LARGER than a small synthetic guest
// root (a programmatic path that bypasses the interactive cap). get_surface_capabilities must (a)
// drive the real HWND client back down to the root -- not merely advertise a smaller currentExtent
// -- and (b) pin currentExtent <= root; then the deferred swapchain recreates + acquires cleanly at
// the corrected extent (presentation converges, the anti-stall proof). GPU-gated (skips with no
// device/WSI). Restores the process-global root on every path.
void test_real_m0_over_root_guard() {
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE);
    constexpr int kRootW = 400, kRootH = 300;
    worker::windowing::WindowThread::set_guest_root(kRootW, kRootH);
    struct RootGuard {
        ~RootGuard() { worker::windowing::WindowThread::set_guest_root(0, 0); }
    } root_guard;

    worker::RealVulkanBackend backend("", "", /*gpu_required=*/false);
    const vkrpc::CreateInstanceResponse ci = backend.create_instance({});
    if (!ci.ok) {
        std::fprintf(stderr, "integration_real_backend: m0 over-root skipped (no Vulkan: %s)\n",
                     ci.reason.c_str());
        return;
    }
    vkrpc::EnumeratePhysicalDevicesRequest er;
    er.instance = ci.instance;
    const vkrpc::EnumeratePhysicalDevicesResponse en = backend.enumerate_physical_devices(er);
    if (!en.ok || en.devices.empty()) {
        std::fprintf(stderr, "integration_real_backend: m0 over-root skipped (no device)\n");
        return;
    }
    const std::uint64_t phys = en.devices.front().handle;
    vkrpc::CreateDeviceRequest cdr;
    cdr.instance = ci.instance;
    cdr.physical_device = phys;
    const vkrpc::CreateDeviceResponse cd = backend.create_device(cdr);
    VKR_CHECK(cd.ok);

    // Register OVERSIZE (800x600 -- larger than the 400x300 synthetic root, but small enough to fit
    // any real monitor so the sidecar-authored resize actually drives the HWND there).
    const std::uint64_t kXid = 0x4DB5;
    sidecar::SidecarRegisterToplevelRequest reg;
    reg.xid = kXid;
    reg.generation = 1;
    reg.role = "toplevel";
    reg.x = 60;
    reg.y = 60;
    reg.width = 800;
    reg.height = 600;
    VKR_CHECK(backend.register_toplevel(reg).applied);

    vkrpc::CreateSurfaceRequest csr;
    csr.instance = ci.instance;
    csr.platform = "xcb";
    csr.xid = kXid;
    const vkrpc::CreateSurfaceResponse surf = backend.create_surface(csr);
    if (!surf.ok) {
        std::fprintf(stderr, "integration_real_backend: m0 over-root skipped (no surface: %s)\n",
                     surf.reason.c_str());
        return;
    }
    vkrpc::GetSurfaceFormatsRequest fmt_req;
    fmt_req.physical_device = phys;
    fmt_req.surface = surf.surface;
    const vkrpc::GetSurfaceFormatsResponse fmt = backend.get_surface_formats(fmt_req);
    VKR_CHECK(fmt.ok && !fmt.formats.empty());

    vkrpc::CreateSwapchainRequest scr;
    scr.device = cd.device;
    scr.surface = surf.surface;
    scr.image_format = fmt.formats.front().format;
    scr.color_space = fmt.formats.front().color_space;
    scr.present_mode = 2;          // FIFO
    scr.use_current_extent = true; // DEFERRED (zink/kopper)
    scr.min_image_count = 2;
    scr.image_usage = vkrpc::kImageUsageColorAttachment;
    const vkrpc::CreateSwapchainResponse sc = backend.create_swapchain(scr);
    if (!sc.ok && sc.result == vkrpc::kVkSuccess) {
        std::fprintf(stderr, "integration_real_backend: m0 over-root skipped (no swapchain)\n");
        return;
    }
    VKR_CHECK(sc.ok && sc.result == vkrpc::kVkSuccess && sc.swapchain != 0);

    // The caps query is where the over-root guard runs: the HWND client (800x600) exceeds the root
    // (400x300), so it drives the client down and pins the corrected extent.
    vkrpc::GetSurfaceCapabilitiesRequest cap;
    cap.physical_device = phys;
    cap.surface = surf.surface;
    const vkrpc::GetSurfaceCapabilitiesResponse caps = backend.get_surface_capabilities(cap);
    VKR_CHECK(caps.ok);
    // (a) pinned currentExtent never exceeds the guest root.
    VKR_CHECK(caps.current_extent_width <= static_cast<std::uint64_t>(kRootW));
    VKR_CHECK(caps.current_extent_height <= static_cast<std::uint64_t>(kRootH));
    // (b) and the REAL HWND client was actually driven down (not just a smaller advertised extent).
    const HWND hwnd = backend.debug_surface_hwnd(surf.surface);
    RECT rc{};
    VKR_CHECK(hwnd != nullptr && GetClientRect(hwnd, &rc) != 0);
    VKR_CHECK(static_cast<int>(rc.right - rc.left) <= kRootW);
    VKR_CHECK(static_cast<int>(rc.bottom - rc.top) <= kRootH);

    // (c) convergence: recreate the deferred swapchain at the corrected extent and acquire cleanly
    // (presentation resumes -- the anti-stall proof, not a wedge).
    vkrpc::HandleRequest dh;
    dh.handle = sc.swapchain;
    VKR_CHECK(backend.destroy_swapchain(dh).ok);
    const vkrpc::CreateSwapchainResponse sc2 = backend.create_swapchain(scr);
    VKR_CHECK(sc2.ok && sc2.result == vkrpc::kVkSuccess && sc2.swapchain != 0);
    vkrpc::GetSwapchainImagesRequest gir;
    gir.swapchain = sc2.swapchain;
    VKR_CHECK(backend.get_swapchain_images(gir).ok);
    vkrpc::AcquireNextImageRequest air;
    air.swapchain = sc2.swapchain;
    air.timeout = UINT64_MAX;
    VKR_CHECK_EQ(backend.acquire_next_image(air).result, vkrpc::kVkSuccess);

    vkrpc::HandleRequest dh2;
    dh2.handle = sc2.swapchain;
    VKR_CHECK(backend.destroy_swapchain(dh2).ok);
    vkrpc::HandleRequest sdh;
    sdh.handle = surf.surface;
    VKR_CHECK(backend.destroy_surface(sdh).ok);
    sidecar::SidecarUnregisterToplevelRequest ureg;
    ureg.xid = kXid;
    ureg.generation = 2;
    backend.unregister_toplevel(ureg);
}

// Lost-device containment: after any host command returns
// VK_ERROR_DEVICE_LOST the worker must NEVER re-enter the host driver on that device -- the
// live-observed hard hang was a post-loss vkAcquireNextImageKHR blocking forever on the window
// thread (holding slot locks). The observer seam forces the latch on a HEALTHY device: if a later
// acquire/submit/recreate/wait entered the host it would return VK_SUCCESS, so the asserted
// VK_ERROR_DEVICE_LOST proves the local short-circuit. GPU-gated (skips without a device/WSI).
void test_real_device_lost_containment() {
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE);
    worker::RealVulkanBackend backend("", "", /*gpu_required=*/false);
    const vkrpc::CreateInstanceResponse ci = backend.create_instance({});
    if (!ci.ok) {
        std::fprintf(stderr, "integration_real_backend: lost-containment skipped (no Vulkan)\n");
        return;
    }
    vkrpc::EnumeratePhysicalDevicesRequest er;
    er.instance = ci.instance;
    const vkrpc::EnumeratePhysicalDevicesResponse en = backend.enumerate_physical_devices(er);
    if (!en.ok || en.devices.empty()) {
        std::fprintf(stderr, "integration_real_backend: lost-containment skipped (no device)\n");
        return;
    }
    vkrpc::CreateDeviceRequest cdr;
    cdr.instance = ci.instance;
    cdr.physical_device = en.devices.front().handle;
    VkPhysicalDeviceTimelineSemaphoreFeatures timeline{};
    timeline.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;
    timeline.timelineSemaphore = VK_TRUE;
    vkrpc::CapabilityChainEntry timeline_entry;
    timeline_entry.s_type = static_cast<std::uint32_t>(timeline.sType);
    timeline_entry.size = sizeof(timeline);
    timeline_entry.blob.assign(reinterpret_cast<const char*>(&timeline), sizeof(timeline));
    cdr.enabled_feature_chain.push_back(timeline_entry);
    const vkrpc::CreateDeviceResponse cd = backend.create_device(cdr);
    VKR_CHECK(cd.ok);
    VKR_CHECK(!backend.debug_instance_surface_maintenance1(ci.instance));
    VKR_CHECK(!backend.debug_device_present_fence_retire(cd.device));
    vkrpc::GetDeviceQueueRequest gqr;
    gqr.device = cd.device;
    gqr.queue_family_index = cd.queue_family_index;
    gqr.queue_index = 0;
    const vkrpc::GetDeviceQueueResponse q = backend.get_device_queue(gqr);
    VKR_CHECK(q.ok && q.queue != 0);

    const std::uint64_t kXid = 0x4DC9;
    sidecar::SidecarRegisterToplevelRequest reg;
    reg.xid = kXid;
    reg.generation = 1;
    reg.role = "toplevel";
    reg.x = 60;
    reg.y = 60;
    reg.width = 320;
    reg.height = 240;
    VKR_CHECK(backend.register_toplevel(reg).applied);
    vkrpc::CreateSurfaceRequest csr;
    csr.instance = ci.instance;
    csr.platform = "xcb";
    csr.xid = kXid;
    const vkrpc::CreateSurfaceResponse surf = backend.create_surface(csr);
    if (!surf.ok) {
        std::fprintf(stderr, "integration_real_backend: lost-containment skipped (no surface)\n");
        return;
    }
    vkrpc::GetSurfaceFormatsRequest fmt_req;
    fmt_req.physical_device = en.devices.front().handle;
    fmt_req.surface = surf.surface;
    const vkrpc::GetSurfaceFormatsResponse fmt = backend.get_surface_formats(fmt_req);
    VKR_CHECK(fmt.ok && !fmt.formats.empty());
    vkrpc::CreateSwapchainRequest scr;
    scr.device = cd.device;
    scr.surface = surf.surface;
    scr.image_format = fmt.formats.front().format;
    scr.color_space = fmt.formats.front().color_space;
    scr.present_mode = 2;          // FIFO
    scr.use_current_extent = true; // deferred (zink-style)
    scr.min_image_count = 2;
    scr.image_usage = vkrpc::kImageUsageColorAttachment;
    const vkrpc::CreateSwapchainResponse sc = backend.create_swapchain(scr);
    if (!sc.ok || sc.result != vkrpc::kVkSuccess || sc.swapchain == 0) {
        std::fprintf(stderr, "integration_real_backend: lost-containment skipped (no swapchain)\n");
        return;
    }
    vkrpc::GetSwapchainImagesRequest gir;
    gir.swapchain = sc.swapchain;
    VKR_CHECK(backend.get_swapchain_images(gir).ok);
    // Baseline: the device is HEALTHY -- acquire succeeds against the real host.
    vkrpc::AcquireNextImageRequest air;
    air.swapchain = sc.swapchain;
    air.timeout = UINT64_MAX;
    VKR_CHECK_EQ(backend.acquire_next_image(air).result, vkrpc::kVkSuccess);

    vkrpc::CreateFenceRequest fence_req;
    fence_req.device = cd.device;
    const vkrpc::CreateFenceResponse fence = backend.create_fence(fence_req);
    VKR_CHECK(fence.ok);
    vkrpc::CreateEventRequest event_req;
    event_req.device = cd.device;
    const vkrpc::CreateEventResponse event = backend.create_event(event_req);
    VKR_CHECK(event.ok);
    vkrpc::CreateQueryPoolRequest query_req;
    query_req.device = cd.device;
    query_req.query_type = VK_QUERY_TYPE_OCCLUSION;
    query_req.query_count = 1;
    const vkrpc::CreateQueryPoolResponse query = backend.create_query_pool(query_req);
    VKR_CHECK(query.ok);
    vkrpc::CreateSemaphoreRequest timeline_req;
    timeline_req.device = cd.device;
    timeline_req.semaphore_type = 1;
    timeline_req.initial_value = 3;
    const vkrpc::CreateSemaphoreResponse timeline_sem = backend.create_semaphore(timeline_req);
    VKR_CHECK(timeline_sem.ok);

    // The central observer is identity for normal statuses and sticky for DEVICE_LOST.
    VKR_CHECK_EQ(backend.debug_observe_device_result(cd.device, VK_TIMEOUT, "test timeout"),
                 static_cast<int>(VK_TIMEOUT));
    VKR_CHECK(!backend.debug_device_lost_latched(cd.device));
    VKR_CHECK_EQ(backend.debug_observe_device_result(cd.device, VK_ERROR_DEVICE_LOST, "test loss"),
                 static_cast<int>(VK_ERROR_DEVICE_LOST));
    VKR_CHECK(backend.debug_device_lost_latched(cd.device));

    // Every subsequent device op must answer VK_ERROR_DEVICE_LOST LOCALLY: the
    // real device is healthy, so a host call would have returned VK_SUCCESS -- the DEVICE_LOST
    // results below prove the host was never entered.
    {
        const vkrpc::AcquireNextImageResponse a = backend.acquire_next_image(air);
        VKR_CHECK(a.ok);
        VKR_CHECK_EQ(a.result, vkrpc::kVkErrorDeviceLost);
    }
    {
        vkrpc::QueueSubmitRequest sub; // an empty submit is valid Vulkan
        sub.queue = q.queue;
        const vkrpc::QueueSubmitResponse s = backend.queue_submit(sub);
        VKR_CHECK(s.ok);
        VKR_CHECK_EQ(s.result, vkrpc::kVkErrorDeviceLost);
    }
    {
        vkrpc::CreateSwapchainRequest rec = scr; // a recreate must not re-enter the host either
        rec.old_swapchain = sc.swapchain;
        const vkrpc::CreateSwapchainResponse r = backend.create_swapchain(rec);
        VKR_CHECK(r.ok);
        VKR_CHECK_EQ(r.result, vkrpc::kVkErrorDeviceLost);
        VKR_CHECK_EQ(r.swapchain, 0ull);
    }
    {
        vkrpc::HandleRequest w;
        w.handle = cd.device;
        const vkrpc::WaitIdleResponse r = backend.device_wait_idle(w);
        VKR_CHECK(r.ok);
        VKR_CHECK_EQ(r.result, vkrpc::kVkErrorDeviceLost);
        w.handle = q.queue;
        const vkrpc::WaitIdleResponse qr = backend.queue_wait_idle(w);
        VKR_CHECK(qr.ok);
        VKR_CHECK_EQ(qr.result, vkrpc::kVkErrorDeviceLost);
    }
    {
        vkrpc::WaitForFencesRequest r;
        r.fences = {fence.fence};
        r.wait_all = true;
        r.timeout = UINT64_MAX;
        const auto w = backend.wait_for_fences(r);
        VKR_CHECK(w.ok);
        VKR_CHECK_EQ(w.result, vkrpc::kVkErrorDeviceLost);
    }
    {
        vkrpc::WaitSemaphoresRequest r;
        r.device = cd.device;
        r.semaphores = {timeline_sem.semaphore};
        r.values = {4};
        r.timeout = UINT64_MAX;
        const auto w = backend.wait_semaphores(r);
        VKR_CHECK(w.ok);
        VKR_CHECK_EQ(w.result, vkrpc::kVkErrorDeviceLost);
    }
    {
        vkrpc::QueuePresentRequest r;
        r.queue = q.queue;
        r.presents.push_back({sc.swapchain, 0});
        const auto p = backend.queue_present(r);
        VKR_CHECK(p.ok);
        VKR_CHECK_EQ(p.result, vkrpc::kVkErrorDeviceLost);
        VKR_CHECK_EQ(p.results.size(), static_cast<std::size_t>(1));
        VKR_CHECK_EQ(p.results.front(), vkrpc::kVkErrorDeviceLost);
    }
    {
        vkrpc::HandleRequest r;
        r.handle = fence.fence;
        const auto s = backend.get_fence_status(r);
        VKR_CHECK(s.ok);
        VKR_CHECK_EQ(s.result, vkrpc::kVkErrorDeviceLost);
        r.handle = event.event;
        const auto e = backend.get_event_status(r);
        VKR_CHECK(e.ok);
        VKR_CHECK_EQ(e.result, vkrpc::kVkErrorDeviceLost);
    }
    {
        vkrpc::GetQueryPoolResultsRequest r;
        r.device = cd.device;
        r.query_pool = query.query_pool;
        r.first_query = 0;
        r.query_count = 1;
        r.data_size = 8;
        r.stride = 8;
        r.flags = VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT;
        const auto qres = backend.get_query_pool_results(r);
        VKR_CHECK(qres.ok);
        VKR_CHECK_EQ(qres.vk_result, vkrpc::kVkErrorDeviceLost);
        VKR_CHECK(qres.data.empty());
    }
    {
        vkrpc::GetSemaphoreCounterValueRequest r;
        r.device = cd.device;
        r.semaphore = timeline_sem.semaphore;
        const auto c = backend.get_semaphore_counter_value(r);
        VKR_CHECK(c.ok);
        VKR_CHECK_EQ(c.result, vkrpc::kVkErrorDeviceLost);
    }
    // Cleanup: destroy_swapchain/dtor ABANDON the lost device's host objects (never re-enter);
    // the calls still succeed at the table level so the guest teardown does not fault.
    vkrpc::HandleRequest dh;
    dh.handle = sc.swapchain;
    VKR_CHECK(backend.destroy_swapchain(dh).ok);
    vkrpc::HandleRequest sdh;
    sdh.handle = surf.surface;
    VKR_CHECK(backend.destroy_surface(sdh).ok);
    sidecar::SidecarUnregisterToplevelRequest ureg;
    ureg.xid = kXid;
    ureg.generation = 2;
    backend.unregister_toplevel(ureg);
}

// (geometry-feedback): a Win32 snap / non-drag reposition (Aero Snap, Win+Arrow) is a
// SetWindowPos with NO WM_ENTERSIZEMOVE bracket and no SC_MAXIMIZE/SC_RESTORE, so before the fix it
// forwarded NO GeometryRequest and the guest registry went stale. The WM_WINDOWPOSCHANGED handler
// now forwards a genuine non-drag move; a worker-origin apply (SelfApplyScope) and a z-order-only
// change still forward nothing. Cross-thread SetWindowPos is synchronous (the owning window thread
// processes WM_WINDOWPOSCHANGED before it returns), so the forward is enqueued before poll. RED
// before the fix: the snap forwards 0. Skips if no window thread.
void test_real_snap_forward() {
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE);
    worker::RealVulkanBackend backend("", "", /*gpu_required=*/false);
    sidecar::SidecarRegisterToplevelRequest reg;
    reg.xid = 0x4DC7;
    reg.generation = 1;
    reg.role = "toplevel";
    reg.x = 100;
    reg.y = 100;
    reg.width = 400;
    reg.height = 300;
    VKR_CHECK(backend.register_toplevel(reg).applied);
    const HWND hwnd = backend.debug_placeholder_hwnd_for_xid(0x4DC7);
    if (hwnd == nullptr) {
        std::fprintf(stderr, "integration_real_backend: snap-forward skipped (no window thread)\n");
        return;
    }
    VKR_CHECK(backend.paint_chrome(solid_paint(0x4DC7, 1, 1, 400, 300)).shown);

    std::uint64_t seq = 0;
    struct GeoEv {
        int rx = 0, ry = 0, w = 0, h = 0;
        std::uint32_t host_request = 0;
    };
    auto poll_geo = [&](GeoEv& last) -> int {
        sidecar::SidecarPollInputRequest p;
        p.since_seq = seq;
        const sidecar::SidecarPollInputResponse r = backend.poll_input(p);
        int geo = 0;
        for (const auto& e : r.events) {
            if (static_cast<sidecar::InputEventKind>(e.kind) ==
                sidecar::InputEventKind::GeometryRequest) {
                ++geo;
                last.rx = e.root_x;
                last.ry = e.root_y;
                last.w = static_cast<int>(e.req_w);
                last.h = static_cast<int>(e.req_h);
                last.host_request = e.host_request;
            }
        }
        seq = r.next_seq;
        return geo;
    };
    // The EXACT realized X-root client geometry the worker forwards -- the SAME readback the
    // reverse path uses (ClientToScreen minus the pinned host origin), so the assertion is against
    // ground truth, not a re-derivation (the stale-origin field IS the bug).
    auto realized = [&]() {
        return worker::windowing::read_real_root_client_geometry(
            hwnd, worker::windowing::WindowThread::host_origin());
    };
    // A move+resize snap = a non-drag SetWindowPos. Compute the outer rect for a known CLIENT size
    // at the window's DPI so the forwarded req_w/req_h are exact.
    auto snap_move_resize = [&](int x, int y, int cw, int ch) {
        RECT rc{0, 0, cw, ch};
        AdjustWindowRectExForDpi(&rc, WS_OVERLAPPEDWINDOW, FALSE, 0, GetDpiForWindow(hwnd));
        SetWindowPos(hwnd, nullptr, x, y, rc.right - rc.left, rc.bottom - rc.top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    };

    GeoEv drain;
    poll_geo(drain); // drain any placement-time forwards

    // (1) A move+resize snap forwards EXACTLY ONE GeometryRequest carrying the exact realized ROOT
    // ORIGIN + the snapped client extent + HostRequestKind::Geometry. The origin is the field whose
    // staleness caused the user-visible input misplacement; == 1 rules out the feedback/storm
    // class.
    snap_move_resize(0, 0, 640, 480);
    const worker::windowing::RootClientGeometry r1 = realized();
    GeoEv e1;
    VKR_CHECK_EQ(poll_geo(e1), 1);
    VKR_CHECK_EQ(e1.w, 640);
    VKR_CHECK_EQ(e1.h, 480);
    VKR_CHECK_EQ(e1.rx, r1.root_x);
    VKR_CHECK_EQ(e1.ry, r1.root_y);
    VKR_CHECK_EQ(e1.host_request, static_cast<std::uint32_t>(sidecar::HostRequestKind::Geometry));

    // (2) A MOVE-ONLY snap (SWP_NOSIZE -> NO WM_SIZE at all: the path the old triggers could never
    // observe) forwards exactly one request with the new origin and the UNCHANGED
    // extent.
    SetWindowPos(hwnd, nullptr, 300, 200, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    const worker::windowing::RootClientGeometry r2 = realized();
    GeoEv e2;
    VKR_CHECK_EQ(poll_geo(e2), 1);
    VKR_CHECK_EQ(e2.w, 640); // extent unchanged by a pure move
    VKR_CHECK_EQ(e2.h, 480);
    VKR_CHECK_EQ(e2.rx, r2.root_x);
    VKR_CHECK_EQ(e2.ry, r2.root_y);
    VKR_CHECK(r2.root_x != r1.root_x || r2.root_y != r1.root_y); // it actually moved
    VKR_CHECK_EQ(e2.host_request, static_cast<std::uint32_t>(sidecar::HostRequestKind::Geometry));

    // (3) A worker-origin apply (sidecar update_toplevel under SelfApplyScope) forwards NOTHING --
    // no feedback echo.
    sidecar::SidecarUpdateToplevelRequest up;
    up.xid = 0x4DC7;
    up.generation = 2;
    up.x = 150;
    up.y = 120;
    up.width = 640;
    up.height = 480;
    VKR_CHECK(backend.update_toplevel(up).applied);
    GeoEv e3;
    VKR_CHECK_EQ(poll_geo(e3), 0);

    // (4) A z-order-only change (SWP_NOMOVE | SWP_NOSIZE) forwards nothing.
    SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    GeoEv e4;
    VKR_CHECK_EQ(poll_geo(e4), 0);

    sidecar::SidecarUnregisterToplevelRequest u;
    u.xid = 0x4DC7;
    u.generation = 3;
    VKR_CHECK(backend.unregister_toplevel(u).applied);
}

// Interactive title-bar click drift: a window authored at the top-left
// gets its host frame clamped on-screen, so the REALIZED client diverges from the authored (0,0).
// The worker RECONCILES that back to the guest (one GeometryRequest with the real client root), and
// after convergence a no-op title-bar click (WM_ENTERSIZEMOVE/EXITSIZEMOVE, no move) forwards
// NOTHING (the no-op guard on the real-readback baseline), while a genuine move still forwards.
// RED by construction: pre-fix the initial clamp forwards no reconcile AND
// the no-op click forwards a spurious (11,45)-class request. Skips if no window thread.
void test_real_caption_click_no_drift() {
    worker::RealVulkanBackend backend("", "", /*gpu_required=*/false);
    sidecar::SidecarRegisterToplevelRequest reg;
    reg.xid = 0x4DC1;
    reg.generation = 1;
    reg.role = "toplevel";
    reg.x = 0; // top-left -> the worker initial frame clamp fires, diverging realized from authored
    reg.y = 0;
    reg.width = 400;
    reg.height = 300;
    VKR_CHECK(backend.register_toplevel(reg).applied);
    const HWND hwnd = backend.debug_placeholder_hwnd_for_xid(0x4DC1);
    if (hwnd == nullptr) {
        std::fprintf(stderr,
                     "integration_real_backend: caption-drift skipped (no window thread)\n");
        return;
    }

    // (1) The initial frame clamp moved the realized client off the authored (0,0); the worker
    // reconciles EXACTLY ONE GeometryRequest carrying the REAL client root origin (on-screen, below
    // the authored top because the caption was pushed on-screen).
    sidecar::SidecarPollInputRequest p;
    p.since_seq = 0;
    const sidecar::SidecarPollInputResponse r = backend.poll_input(p);
    VKR_CHECK(r.ok);
    int recon = 0, rx = 0, ry = 0, rw = 0, rh = 0;
    for (const auto& e : r.events) {
        if (static_cast<sidecar::InputEventKind>(e.kind) ==
            sidecar::InputEventKind::GeometryRequest) {
            ++recon;
            rx = e.root_x;
            ry = e.root_y;
            rw = static_cast<int>(e.req_w);
            rh = static_cast<int>(e.req_h);
            VKR_CHECK_EQ(e.host_request,
                         static_cast<std::uint32_t>(sidecar::HostRequestKind::Geometry));
        }
    }
    VKR_CHECK_EQ(recon, 1); // one reconcile correction for the clamp
    VKR_CHECK(ry > 0); // the realized client is below the authored top (caption pushed on-screen)
    // The reconciled coords are the REAL host client origin (DebugEnumWindows actual,
    // ClientToScreen).
    sidecar::SidecarDebugEnumWindowsRequest q;
    q.include_actual = true;
    {
        const sidecar::SidecarDebugEnumWindowsResponse e = backend.debug_enum_windows(q);
        const sidecar::SidecarWindowInfo* w = find_window(e, 0x4DC1);
        VKR_CHECK(w != nullptr && w->has_actual);
        VKR_CHECK_EQ(rx, w->actual_x);
        VKR_CHECK_EQ(ry, w->actual_y);
    }

    // Apply the sidecar echo of the correction (update_toplevel with the real geometry), as the
    // real flow would. (2) Echo convergence: actual == authored, no residual clamp delta.
    sidecar::SidecarUpdateToplevelRequest echo;
    echo.xid = 0x4DC1;
    echo.generation = 2;
    echo.x = rx;
    echo.y = ry;
    echo.width = static_cast<std::uint32_t>(rw);
    echo.height = static_cast<std::uint32_t>(rh);
    VKR_CHECK(backend.update_toplevel(echo).applied);
    {
        const sidecar::SidecarDebugEnumWindowsResponse e = backend.debug_enum_windows(q);
        const sidecar::SidecarWindowInfo* w = find_window(e, 0x4DC1);
        VKR_CHECK(w != nullptr && w->has_actual);
        VKR_CHECK_EQ(w->actual_x, rx); // converged: authored (echo) == actual
        VKR_CHECK_EQ(w->actual_y, ry);
        VKR_CHECK(!w->clamped); // no residual position delta
    }

    // (1b) A NO-OP title-bar click (enter/exit size-move, no SetWindowPos) forwards NOTHING now
    // that realized == authored -- the caption-drift fix.
    SendMessageW(hwnd, WM_ENTERSIZEMOVE, 0, 0);
    SendMessageW(hwnd, WM_EXITSIZEMOVE, 0, 0);
    sidecar::SidecarPollInputRequest p2;
    p2.since_seq = r.next_seq;
    const sidecar::SidecarPollInputResponse r2 = backend.poll_input(p2);
    int extra = 0;
    for (const auto& e : r2.events) {
        if (static_cast<sidecar::InputEventKind>(e.kind) ==
            sidecar::InputEventKind::GeometryRequest) {
            ++extra;
        }
    }
    VKR_CHECK_EQ(extra, 0); // a no-op caption click no longer drifts the window

    // (3) A GENUINE user move to a new client origin still forwards exactly one request with the
    // real (ClientToScreen-derived) coords -- the no-op guard did not become too broad.
    const POINT work = vkr::worker::windowing::win32_work_origin();
    const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE));
    const DWORD exstyle = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
    const UINT dpi = GetDpiForWindow(hwnd);
    const int want_rx = 250, want_ry = 180;
    const RECT frame = vkr::worker::windowing::map_root_client_to_win32_frame(
        want_rx, want_ry, 400, 300, work, style, exstyle, dpi);
    SendMessageW(hwnd, WM_ENTERSIZEMOVE, 0, 0);
    SetWindowPos(hwnd, nullptr, frame.left, frame.top, frame.right - frame.left,
                 frame.bottom - frame.top, SWP_NOZORDER | SWP_NOACTIVATE);
    SendMessageW(hwnd, WM_EXITSIZEMOVE, 0, 0);
    sidecar::SidecarPollInputRequest p3;
    p3.since_seq = r2.next_seq;
    const sidecar::SidecarPollInputResponse r3 = backend.poll_input(p3);
    int moved = 0, mx = 0, my = 0;
    for (const auto& e : r3.events) {
        if (static_cast<sidecar::InputEventKind>(e.kind) ==
            sidecar::InputEventKind::GeometryRequest) {
            ++moved;
            mx = e.root_x;
            my = e.root_y;
        }
    }
    VKR_CHECK_EQ(moved, 1); // a real move still forwards
    VKR_CHECK_EQ(mx, want_rx);
    VKR_CHECK_EQ(my, want_ry);

    sidecar::SidecarUnregisterToplevelRequest u;
    u.xid = 0x4DC1;
    u.generation = 3;
    VKR_CHECK(backend.unregister_toplevel(u).applied);
}

// A POSITION-only frame-clamp reconcile must carry the AUTHORED
// client size, NOT the transient realized host client size -- otherwise a surface HWND still at its
// create_surface default could be fed back as a user RESIZE and grab extent authority (an
// -class leak). Prove it on the REAL surface-promote path: register a top-left toplevel with a
// LARGE authored size, then create_surface (promote) -- the fresh surface HWND is placed
// position-only at its default size while its initial frame clamp fires, so the reconcile's
// realized size diverges from the authored size. Assert the reconcile carries the AUTHORED size.
// GPU-gated (self-skips without a Vulkan instance/surface).
void test_real_reconcile_position_only_keeps_authored_size() {
    worker::RealVulkanBackend backend("", "", /*gpu_required=*/false);
    const vkrpc::CreateInstanceResponse ci = backend.create_instance({});
    if (!ci.ok) {
        std::fprintf(stderr, "integration_real_backend: reconcile-size skipped (no Vulkan)\n");
        return;
    }
    sidecar::SidecarRegisterToplevelRequest reg;
    reg.xid = 0xD4C2;
    reg.generation = 1;
    reg.role = "toplevel";
    reg.x = 0; // top-left -> the (fresh surface slot's) initial frame clamp fires
    reg.y = 0;
    reg.width = 400; // a LARGE authored size vs the surface HWND's default
    reg.height = 300;
    VKR_CHECK(backend.register_toplevel(reg).applied);
    if (backend.debug_placeholder_hwnd_for_xid(0xD4C2) == nullptr) {
        std::fprintf(stderr,
                     "integration_real_backend: reconcile-size skipped (no window thread)\n");
        return;
    }
    // Drain the placeholder's own initial-clamp reconcile (its realized size == authored,
    // harmless).
    sidecar::SidecarPollInputRequest p0;
    p0.since_seq = 0;
    const sidecar::SidecarPollInputResponse r0 = backend.poll_input(p0);

    // PROMOTE to a surface: a brand-new surface HWND is created at its default size and placed
    // position-only at the registry geometry, so its FRESH initial clamp fires with a realized size
    // that diverges from the authored 400x300.
    vkrpc::CreateSurfaceRequest csr;
    csr.instance = ci.instance;
    csr.platform = "xcb";
    csr.xid = 0xD4C2;
    const vkrpc::CreateSurfaceResponse surf = backend.create_surface(csr);
    if (!surf.ok) {
        std::fprintf(stderr, "integration_real_backend: reconcile-size skipped (no surface)\n");
        sidecar::SidecarUnregisterToplevelRequest us;
        us.xid = 0xD4C2;
        us.generation = 2;
        backend.unregister_toplevel(us);
        return;
    }
    VKR_CHECK(backend.debug_representation_for_xid(0xD4C2) == sidecar::Representation::Surface);

    // The surface HWND's realized client size is the transient default, NOT the authored 400x300 --
    // so the reconcile carrying the authored size below actually proves the fix.
    sidecar::SidecarDebugEnumWindowsRequest q;
    q.include_actual = true;
    {
        const sidecar::SidecarDebugEnumWindowsResponse e = backend.debug_enum_windows(q);
        const sidecar::SidecarWindowInfo* w = find_window(e, 0xD4C2);
        VKR_CHECK(w != nullptr && w->has_actual);
        VKR_CHECK(w->actual_width != 400 || w->actual_height != 300); // realized size diverged
    }

    // The surface's initial frame-clamp reconcile must carry the AUTHORED size (400x300), NOT the
    // transient realized surface size -- else the sidecar would read it as a resize and grab extent
    // authority (an extent-authority leak).
    sidecar::SidecarPollInputRequest p1;
    p1.since_seq = r0.next_seq;
    const sidecar::SidecarPollInputResponse r1 = backend.poll_input(p1);
    int geo = 0, gw = 0, gh = 0;
    for (const auto& e : r1.events) {
        if (static_cast<sidecar::InputEventKind>(e.kind) ==
            sidecar::InputEventKind::GeometryRequest) {
            ++geo;
            gw = static_cast<int>(e.req_w);
            gh = static_cast<int>(e.req_h);
        }
    }
    VKR_CHECK(geo >= 1);   // the surface's initial clamp reconciled its position
    VKR_CHECK_EQ(gw, 400); // carries the AUTHORED size, not the transient realized surface size
    VKR_CHECK_EQ(gh, 300);

    vkrpc::HandleRequest dh;
    dh.handle = surf.surface;
    backend.destroy_surface(dh);
    sidecar::SidecarUnregisterToplevelRequest u;
    u.xid = 0xD4C2;
    u.generation = 2;
    VKR_CHECK(backend.unregister_toplevel(u).applied);
}

// Live zink-surface resize: the DEFERRED-extent (use_current_extent) counterpart of the
// resize-convergence crux (the 0xD505 block in main() proves the NATIVE app-picks path).
// zink/kopper DEFERS its swapchain extent, so the SURFACE dictates the size: caps pin currentExtent
// to the live host client, and a sidecar-AUTHORED size change is the ONE live-resize authority.
// Three clusters:
//   (1) register-first: register at A, create surface, create a DEFERRED swapchain -> the host
//   client and caps are A (the deferred surface adopts the toplevel registry geometry).
//   (2) live resize: update to B -> the host client tracks B, a prior acquire returns OUT_OF_DATE
//       through the geometry-dirty latch, caps pin B, and re-creating the deferred swapchain
//       converges at B + clears the latch.
//   (3) surface-first / register-late ADOPTION: create the surface + a deferred swapchain BEFORE
//       registration (the host falls to the default size), then register at the intended size ->
//       the host ADOPTS it. Without this narrow rule, a register's apply is position-only and the
//       window remains stranded at the default size.
// Windows real backend; skips cleanly with no usable device / WSI. Reads GetClientRect, so the test
// thread must be per-monitor-DPI-aware (physical px, like integration_real_extent).
void test_real_deferred_resize() {
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE);
    worker::RealVulkanBackend backend("", "", /*gpu_required=*/false);
    const vkrpc::CreateInstanceResponse ci = backend.create_instance({});
    if (!ci.ok) {
        std::fprintf(stderr, "integration_real_backend: deferred-resize skipped (no Vulkan: %s)\n",
                     ci.reason.c_str());
        return;
    }
    vkrpc::EnumeratePhysicalDevicesRequest er;
    er.instance = ci.instance;
    const vkrpc::EnumeratePhysicalDevicesResponse en = backend.enumerate_physical_devices(er);
    if (!en.ok || en.devices.empty()) {
        std::fprintf(stderr, "integration_real_backend: deferred-resize skipped (no device)\n");
        return;
    }
    const std::uint64_t phys = en.devices.front().handle;
    vkrpc::CreateDeviceRequest cdr;
    cdr.instance = ci.instance;
    cdr.physical_device = phys;
    const vkrpc::CreateDeviceResponse cd = backend.create_device(cdr);
    VKR_CHECK(cd.ok);

    auto client_of = [&](std::uint64_t surface, int& w, int& h) {
        const HWND hwnd = backend.debug_surface_hwnd(surface);
        RECT r{};
        if (hwnd != nullptr && GetClientRect(hwnd, &r) != 0) {
            w = static_cast<int>(r.right - r.left);
            h = static_cast<int>(r.bottom - r.top);
        } else {
            w = h = -1;
        }
    };

    // ---- Cluster 1: register-first deferred surface -> host client + caps == A ----
    const std::uint64_t kXid = 0xDEF2;
    const int kAW = 320, kAH = 240;
    sidecar::SidecarRegisterToplevelRequest reg;
    reg.xid = kXid;
    reg.generation = 1;
    reg.role = "toplevel";
    reg.x = 100;
    reg.y = 80;
    reg.width = kAW;
    reg.height = kAH;
    VKR_CHECK(backend.register_toplevel(reg).applied);

    vkrpc::CreateSurfaceRequest csr;
    csr.instance = ci.instance;
    csr.platform = "xcb";
    csr.xid = kXid;
    const vkrpc::CreateSurfaceResponse surf = backend.create_surface(csr);
    if (!surf.ok) {
        std::fprintf(stderr, "integration_real_backend: deferred-resize skipped (no surface: %s)\n",
                     surf.reason.c_str());
        return;
    }
    vkrpc::GetSurfaceFormatsRequest fmt_req;
    fmt_req.physical_device = phys;
    fmt_req.surface = surf.surface;
    const vkrpc::GetSurfaceFormatsResponse fmt = backend.get_surface_formats(fmt_req);
    VKR_CHECK(fmt.ok && !fmt.formats.empty());
    auto deferred_scr = [&](std::uint64_t surface) {
        vkrpc::CreateSwapchainRequest scr;
        scr.device = cd.device;
        scr.surface = surface;
        scr.image_format = fmt.formats.front().format;
        scr.color_space = fmt.formats.front().color_space;
        scr.present_mode = 2;          // FIFO (spec-guaranteed)
        scr.use_current_extent = true; // DEFERRED -- the surface dictates the extent (zink/kopper)
        scr.min_image_count = 2;
        scr.image_usage = vkrpc::kImageUsageColorAttachment;
        return scr;
    };
    auto caps_of = [&](std::uint64_t surface) {
        vkrpc::GetSurfaceCapabilitiesRequest cap;
        cap.physical_device = phys;
        cap.surface = surface;
        return backend.get_surface_capabilities(cap);
    };

    const vkrpc::CreateSwapchainResponse sc = backend.create_swapchain(deferred_scr(surf.surface));
    if (!sc.ok && sc.result == vkrpc::kVkSuccess) {
        std::fprintf(stderr, "integration_real_backend: deferred-resize skipped (no swapchain)\n");
        return;
    }
    VKR_CHECK(sc.ok && sc.result == vkrpc::kVkSuccess && sc.swapchain != 0);
    int cw = 0, ch = 0;
    client_of(surf.surface, cw, ch);
    VKR_CHECK_EQ(cw,
                 kAW); // deferred surface sized to the toplevel registry geometry
    VKR_CHECK_EQ(ch, kAH);
    {
        const vkrpc::GetSurfaceCapabilitiesResponse caps = caps_of(surf.surface);
        VKR_CHECK(caps.ok);
        VKR_CHECK_EQ(caps.current_extent_width, static_cast<std::uint64_t>(kAW)); // pinned (defers)
        VKR_CHECK_EQ(caps.current_extent_height, static_cast<std::uint64_t>(kAH));
    }

    // ---- Cluster 2: a sidecar-authored resize to B tracks live ----
    const int kBW = 480, kBH = 320;
    sidecar::SidecarUpdateToplevelRequest up;
    up.xid = kXid;
    up.generation = 2;
    up.x = 100;
    up.y = 80;
    up.width = kBW;
    up.height = kBH;
    VKR_CHECK(backend.update_toplevel(up).applied);
    client_of(surf.surface, cw, ch);
    VKR_CHECK_EQ(cw, kBW); // The host client tracked the sidecar-authored extent authority.
    VKR_CHECK_EQ(ch, kBH);
    {
        // The resize tripped the latch -> a prior acquire returns OUT_OF_DATE
        // (recreate-driven).
        vkrpc::AcquireNextImageRequest air;
        air.swapchain = sc.swapchain;
        VKR_CHECK_EQ(backend.acquire_next_image(air).result, vkrpc::kVkErrorOutOfDateKhr);
        const vkrpc::GetSurfaceCapabilitiesResponse caps = caps_of(surf.surface);
        VKR_CHECK(caps.ok);
        VKR_CHECK_EQ(caps.current_extent_width, static_cast<std::uint64_t>(kBW)); // pin tracks B
        VKR_CHECK_EQ(caps.current_extent_height, static_cast<std::uint64_t>(kBH));
    }
    // Re-create the DEFERRED swapchain -> converges at B and clears the latch.
    vkrpc::HandleRequest dh0;
    dh0.handle = sc.swapchain;
    VKR_CHECK(backend.destroy_swapchain(dh0).ok);
    const vkrpc::CreateSwapchainResponse sc2 = backend.create_swapchain(deferred_scr(surf.surface));
    VKR_CHECK(sc2.ok && sc2.result == vkrpc::kVkSuccess && sc2.swapchain != 0);
    {
        vkrpc::GetSwapchainImagesRequest gir;
        gir.swapchain = sc2.swapchain;
        VKR_CHECK(backend.get_swapchain_images(gir).ok);
        vkrpc::AcquireNextImageRequest air;
        air.swapchain = sc2.swapchain;
        air.timeout = UINT64_MAX;
        VKR_CHECK_EQ(backend.acquire_next_image(air).result, vkrpc::kVkSuccess); // latch cleared
    }
    vkrpc::HandleRequest dh1;
    dh1.handle = sc2.swapchain;
    VKR_CHECK(backend.destroy_swapchain(dh1).ok);
    vkrpc::HandleRequest sdh;
    sdh.handle = surf.surface;
    VKR_CHECK(backend.destroy_surface(sdh).ok);
    sidecar::SidecarUnregisterToplevelRequest ureg;
    ureg.xid = kXid;
    ureg.generation = 3;
    backend.unregister_toplevel(ureg);

    // ---- Cluster 3: surface-first / register-late ADOPTION ----
    const std::uint64_t kXid2 = 0xDEF3;
    const int kA2W = 400, kA2H = 300;
    vkrpc::CreateSurfaceRequest csr2;
    csr2.instance = ci.instance;
    csr2.platform = "xcb";
    csr2.xid = kXid2;
    const vkrpc::CreateSurfaceResponse surf2 = backend.create_surface(csr2);
    VKR_CHECK(surf2.ok);
    // A DEFERRED swapchain BEFORE any register -> geometry is unknown, so the host stays at the
    // create-surface default rather than adopting the registered extent.
    const vkrpc::CreateSwapchainResponse sc3 =
        backend.create_swapchain(deferred_scr(surf2.surface));
    VKR_CHECK(sc3.ok && sc3.result == vkrpc::kVkSuccess && sc3.swapchain != 0);
    int pre_w = 0, pre_h = 0;
    client_of(surf2.surface, pre_w, pre_h);
    VKR_CHECK(pre_w != kA2W || pre_h != kA2H); // not yet at the (still unknown) registered size
    // Register LATE at -> the already-deferred surface ADOPTS the registered size once (.3).
    sidecar::SidecarRegisterToplevelRequest reg2;
    reg2.xid = kXid2;
    reg2.generation = 1;
    reg2.role = "toplevel";
    reg2.x = 120;
    reg2.y = 90;
    reg2.width = kA2W;
    reg2.height = kA2H;
    VKR_CHECK(backend.register_toplevel(reg2).applied);
    int post_w = 0, post_h = 0;
    client_of(surf2.surface, post_w, post_h);
    VKR_CHECK_EQ(post_w, kA2W); // adoption: the host client converged to the late-registered size
    VKR_CHECK_EQ(post_h, kA2H);
    {
        // caps now report the adopted size (deferred pin == the live host client).
        const vkrpc::GetSurfaceCapabilitiesResponse caps = caps_of(surf2.surface);
        VKR_CHECK(caps.ok);
        VKR_CHECK_EQ(caps.current_extent_width, static_cast<std::uint64_t>(kA2W));
        VKR_CHECK_EQ(caps.current_extent_height, static_cast<std::uint64_t>(kA2H));
    }

    vkrpc::HandleRequest dh3;
    dh3.handle = sc3.swapchain;
    VKR_CHECK(backend.destroy_swapchain(dh3).ok);
    vkrpc::HandleRequest sdh2;
    sdh2.handle = surf2.surface;
    VKR_CHECK(backend.destroy_surface(sdh2).ok);
    sidecar::SidecarUnregisterToplevelRequest ureg2;
    ureg2.xid = kXid2;
    ureg2.generation = 2;
    backend.unregister_toplevel(ureg2);

    // ---- Cluster 4: the deferred bit is SURFACE-SPECIFIC -- it must NOT leak
    // across same-xid surface replacement. S1 defers xid X; a NON-deferred S2 then replaces it as
    // the CURRENT surface (S1 still live); a register-late must NOT force S2 to adopt the
    // registered size (S2 never deferred). With the old xid-keyed set this wrongly resized S2. ----
    const std::uint64_t kXid3 = 0xDEF4;
    const int kC4W = 440, kC4H = 320;
    vkrpc::CreateSurfaceRequest s1r;
    s1r.instance = ci.instance;
    s1r.platform = "xcb";
    s1r.xid = kXid3;
    const vkrpc::CreateSurfaceResponse s1 = backend.create_surface(s1r);
    VKR_CHECK(s1.ok);
    // S1 defers -> records the deferred bit for X under S1's handle.
    const vkrpc::CreateSwapchainResponse s1sc = backend.create_swapchain(deferred_scr(s1.surface));
    VKR_CHECK(s1sc.ok && s1sc.result == vkrpc::kVkSuccess && s1sc.swapchain != 0);
    // S2 replaces X as the current surface (S1 still live) and does NOT defer (no swapchain).
    vkrpc::CreateSurfaceRequest s2r;
    s2r.instance = ci.instance;
    s2r.platform = "xcb";
    s2r.xid = kXid3;
    const vkrpc::CreateSurfaceResponse s2 = backend.create_surface(s2r);
    VKR_CHECK(s2.ok && s2.surface != s1.surface);
    // Register late: the surface-specific rule sees the recorded deferred surface (S1) is NO
    // longer the current bound surface (S2), so it does NOT force a size apply -> S2 keeps its
    // default (256x256), NOT the registered size. (The old xid-keyed set would have resized S2 at
    // registration.)
    sidecar::SidecarRegisterToplevelRequest s2reg;
    s2reg.xid = kXid3;
    s2reg.generation = 1;
    s2reg.role = "toplevel";
    s2reg.x = 130;
    s2reg.y = 100;
    s2reg.width = kC4W;
    s2reg.height = kC4H;
    VKR_CHECK(backend.register_toplevel(s2reg).applied);
    int s2w = 0, s2h = 0;
    client_of(s2.surface, s2w, s2h);
    VKR_CHECK(s2w != kC4W ||
              s2h != kC4H); // the deferred bit did NOT leak onto the replacement surface
    VKR_CHECK_EQ(s2w, 256); // S2 kept its default size (no forced size adoption)
    VKR_CHECK_EQ(s2h, 256);

    vkrpc::HandleRequest s1dh;
    s1dh.handle = s1sc.swapchain;
    VKR_CHECK(backend.destroy_swapchain(s1dh).ok);
    vkrpc::HandleRequest s1sdh;
    s1sdh.handle = s1.surface;
    VKR_CHECK(backend.destroy_surface(s1sdh).ok);
    vkrpc::HandleRequest s2sdh;
    s2sdh.handle = s2.surface;
    VKR_CHECK(backend.destroy_surface(s2sdh).ok);
    sidecar::SidecarUnregisterToplevelRequest s2u;
    s2u.xid = kXid3;
    s2u.generation = 2;
    backend.unregister_toplevel(s2u);

    std::fprintf(stderr, "integration_real_backend: deferred-extent resize tracks A->B + "
                         "register-late adoption + surface-specific deferred bit verified\n");
}

// an unpinned surface's selectable extent range is governed by the accepted guest root,
// not by the primary monitor work area. This live desk deliberately has a 7600x2160 guest root and
// a 1200x1872 portrait-primary work area, so the old independent SPI_GETWORKAREA ceiling is
// observably wrong. The host Vulkan surface limit is expected to admit this root on the supported
// test GPU; a smaller hard host limit is covered separately by the pure ceiling helper.
void test_real_unpinned_caps_use_guest_root() {
    constexpr std::uint32_t kRootW = 7600;
    constexpr std::uint32_t kRootH = 2160;
    worker::windowing::WindowThread::set_guest_root(kRootW, kRootH);
    struct RootGuard {
        ~RootGuard() { worker::windowing::WindowThread::set_guest_root(0, 0); }
    } root_guard;

    worker::RealVulkanBackend backend("", "", /*gpu_required=*/false);
    const vkrpc::CreateInstanceResponse ci = backend.create_instance({});
    if (!ci.ok) {
        std::fprintf(stderr, "integration_real_backend: root-caps skipped (no Vulkan: %s)\n",
                     ci.reason.c_str());
        return;
    }
    vkrpc::EnumeratePhysicalDevicesRequest er;
    er.instance = ci.instance;
    const vkrpc::EnumeratePhysicalDevicesResponse en = backend.enumerate_physical_devices(er);
    if (!en.ok || en.devices.empty()) {
        std::fprintf(stderr, "integration_real_backend: root-caps skipped (no device)\n");
        return;
    }

    vkrpc::CreateSurfaceRequest sr;
    sr.instance = ci.instance;
    const vkrpc::CreateSurfaceResponse surf = backend.create_surface(sr);
    VKR_CHECK(surf.ok);
    if (!surf.ok) {
        return;
    }
    vkrpc::GetSurfaceCapabilitiesRequest cr;
    cr.physical_device = en.devices.front().handle;
    cr.surface = surf.surface;
    const vkrpc::GetSurfaceCapabilitiesResponse caps = backend.get_surface_capabilities(cr);
    VKR_CHECK(caps.ok);
    VKR_CHECK_EQ(caps.current_extent_width,
                 static_cast<std::uint64_t>(vkrpc::kDynamicExtentSentinel));
    VKR_CHECK_EQ(caps.current_extent_height,
                 static_cast<std::uint64_t>(vkrpc::kDynamicExtentSentinel));
    VKR_CHECK_EQ(caps.max_image_extent_width, static_cast<std::uint64_t>(kRootW));
    VKR_CHECK_EQ(caps.max_image_extent_height, static_cast<std::uint64_t>(kRootH));

    vkrpc::HandleRequest h;
    h.handle = surf.surface;
    VKR_CHECK(backend.destroy_surface(h).ok);
    h.handle = ci.instance;
    VKR_CHECK(backend.destroy_instance(h).ok);
}

// Mixed-DPI live canary: a PMv1 surface window moved from the low-DPI
// monitor to the high-DPI monitor must preserve the exact client origin when the host-authored
// geometry echoes through the sidecar, and any realization correction must be adopted without a
// second SetWindowPos. This is application-agnostic: it drives the same HWND/registry feedback
// plane every surface-backed or placeholder toplevel uses.
void test_real_cross_dpi_geometry_convergence() {
    std::vector<DpiMonitorSample> monitors;
    VKR_CHECK(EnumDisplayMonitors(nullptr, nullptr, collect_dpi_monitor,
                                  reinterpret_cast<LPARAM>(&monitors)) != FALSE);
    if (monitors.size() < 2) {
        std::fprintf(stderr, "integration_real_backend: cross-DPI skipped (<2 monitors)\n");
        return;
    }
    const auto by_dpi = [](const DpiMonitorSample& a, const DpiMonitorSample& b) {
        return a.dpi < b.dpi;
    };
    const auto low = std::min_element(monitors.begin(), monitors.end(), by_dpi);
    const auto high = std::max_element(monitors.begin(), monitors.end(), by_dpi);
    for (const DpiMonitorSample& monitor : monitors) {
        std::fprintf(stderr, "integration_real_backend: monitor [%ld,%ld..%ld,%ld] dpi=%u\n",
                     monitor.bounds.left, monitor.bounds.top, monitor.bounds.right,
                     monitor.bounds.bottom, monitor.dpi);
    }
    if (low == monitors.end() || high == monitors.end() || low->dpi == high->dpi) {
        std::fprintf(stderr, "integration_real_backend: cross-DPI skipped (uniform DPI)\n");
        return;
    }

    const POINT old_origin = worker::windowing::WindowThread::host_origin();
    const std::uint64_t old_root = worker::windowing::WindowThread::guest_root_packed();
    const int virtual_x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int virtual_y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int virtual_w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int virtual_h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    worker::windowing::WindowThread::set_host_origin(virtual_x, virtual_y);
    worker::windowing::WindowThread::set_guest_root(static_cast<std::uint32_t>(virtual_w),
                                                    static_cast<std::uint32_t>(virtual_h));
    struct GeometryGlobalsGuard {
        POINT origin;
        std::uint64_t root;
        ~GeometryGlobalsGuard() {
            worker::windowing::WindowThread::set_host_origin(origin.x, origin.y);
            worker::windowing::WindowThread::set_guest_root(
                static_cast<std::uint32_t>(root >> 32),
                static_cast<std::uint32_t>(root & 0xFFFFFFFFu));
        }
    } globals_guard{old_origin, old_root};

    constexpr std::uint64_t kXid = 0xD91A;
    constexpr std::uint32_t kWidth = 400;
    constexpr std::uint32_t kHeight = 300;
    const auto root_x = [&](const RECT& r) { return r.left + 100 - virtual_x; };
    const auto root_y = [&](const RECT& r) { return r.top + 100 - virtual_y; };

    worker::RealVulkanBackend backend("", "", /*gpu_required=*/false);
    sidecar::SidecarRegisterToplevelRequest reg;
    reg.xid = kXid;
    reg.generation = 1;
    reg.role = "toplevel";
    reg.x = root_x(low->bounds);
    reg.y = root_y(low->bounds);
    reg.width = kWidth;
    reg.height = kHeight;
    VKR_CHECK(backend.register_toplevel(reg).applied);
    const HWND hwnd = backend.debug_placeholder_hwnd_for_xid(kXid);
    if (hwnd == nullptr) {
        std::fprintf(stderr, "integration_real_backend: cross-DPI skipped (no window thread)\n");
        return;
    }
    VKR_CHECK(backend.paint_chrome(solid_paint(kXid, 1, 1, kWidth, kHeight)).shown);

    // Clear any initial-clamp feedback, then make a non-initial authored move low -> high. The
    // first move is computed while GetDpiForWindow still reports the low DPI; after landing, the
    // HWND reports the high DPI and exposes the PMv1 realized-vs-predicted inset mismatch.
    sidecar::SidecarPollInputRequest clear_req;
    const sidecar::SidecarPollInputResponse initial = backend.poll_input(clear_req);
    sidecar::SidecarUpdateToplevelRequest to_high;
    to_high.xid = kXid;
    to_high.generation = 2;
    to_high.x = root_x(high->bounds);
    to_high.y = root_y(high->bounds);
    to_high.width = kWidth;
    to_high.height = kHeight;
    VKR_CHECK(backend.update_toplevel(to_high).applied);
    sidecar::SidecarPollInputRequest after_high_req;
    after_high_req.since_seq = initial.next_seq;
    const sidecar::SidecarPollInputResponse after_high = backend.poll_input(after_high_req);
    const UINT landed_dpi = GetDpiForWindow(hwnd);
    if (landed_dpi != high->dpi) {
        // The full worker process transitions on this desk (the production/OpenSCAD canary below
        // is authoritative), but Windows may pin GetDpiForWindow to the process's startup DPI in
        // this heavily in-process test binary after earlier HWNDs have fixed process DPI state.
        std::fprintf(stderr,
                     "integration_real_backend: cross-DPI skipped (in-process HWND stayed %u, "
                     "target monitor %u)\n",
                     landed_dpi, high->dpi);
        return;
    }

    // Model a real host drag wholly within the high-DPI monitor. The WndProc forwards the exact
    // realized client geometry once on WM_EXITSIZEMOVE.
    RECT frame{};
    VKR_CHECK(GetWindowRect(hwnd, &frame) != FALSE);
    SendMessageW(hwnd, WM_ENTERSIZEMOVE, 0, 0);
    VKR_CHECK(SetWindowPos(hwnd, nullptr, frame.left + 37, frame.top + 29, 0, 0,
                           SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE) != FALSE);
    SendMessageW(hwnd, WM_EXITSIZEMOVE, 0, 0);
    sidecar::SidecarPollInputRequest move_req;
    move_req.since_seq = after_high.next_seq;
    const sidecar::SidecarPollInputResponse moved = backend.poll_input(move_req);
    int move_events = 0;
    sidecar::SidecarInputEvent move{};
    for (const auto& event : moved.events) {
        if (static_cast<sidecar::InputEventKind>(event.kind) ==
            sidecar::InputEventKind::GeometryRequest) {
            ++move_events;
            move = event;
        }
    }
    VKR_CHECK_EQ(move_events, 1);
    if (move_events != 1) {
        return;
    }

    sidecar::SidecarUpdateToplevelRequest echo;
    echo.xid = kXid;
    echo.generation = 3;
    echo.x = move.root_x;
    echo.y = move.root_y;
    echo.width = move.req_w;
    echo.height = move.req_h;
    VKR_CHECK(backend.update_toplevel(echo).applied);

    sidecar::SidecarDebugEnumWindowsRequest query;
    query.include_actual = true;
    sidecar::SidecarDebugEnumWindowsResponse actual_response = backend.debug_enum_windows(query);
    const sidecar::SidecarWindowInfo* actual = find_window(actual_response, kXid);
    VKR_CHECK(actual != nullptr && actual->has_actual);
    if (actual != nullptr) {
        VKR_CHECK_EQ(actual->actual_x, move.root_x); // Exact host->guest->host fidelity.
        VKR_CHECK_EQ(actual->actual_y, move.root_y);
        VKR_CHECK_EQ(actual->actual_width, move.req_w);
        VKR_CHECK_EQ(actual->actual_height, move.req_h);
    }

    sidecar::SidecarPollInputRequest correction_req;
    correction_req.since_seq = moved.next_seq;
    const sidecar::SidecarPollInputResponse correction = backend.poll_input(correction_req);
    int correction_events = 0;
    sidecar::SidecarInputEvent corrected{};
    for (const auto& event : correction.events) {
        if (static_cast<sidecar::InputEventKind>(event.kind) ==
            sidecar::InputEventKind::GeometryRequest) {
            ++correction_events;
            corrected = event;
        }
    }
    VKR_CHECK_EQ(correction_events, 0); // Measured insets must make the first echo exact.

    // Under the older DPI-derived behavior, the first echo produces one correction. Exercise that
    // correction to pin: its
    // echo must be ACKed at the already-realized position, never applied one inset-delta farther.
    if (correction_events == 1) {
        sidecar::SidecarUpdateToplevelRequest adoption;
        adoption.xid = kXid;
        adoption.generation = 4;
        adoption.x = corrected.root_x;
        adoption.y = corrected.root_y;
        adoption.width = corrected.req_w;
        adoption.height = corrected.req_h;
        VKR_CHECK(backend.update_toplevel(adoption).applied);
        actual_response = backend.debug_enum_windows(query);
        actual = find_window(actual_response, kXid);
        VKR_CHECK(actual != nullptr && actual->has_actual);
        if (actual != nullptr) {
            VKR_CHECK_EQ(actual->actual_x, corrected.root_x);
            VKR_CHECK_EQ(actual->actual_y, corrected.root_y);
        }
        sidecar::SidecarPollInputRequest extra_req;
        extra_req.since_seq = correction.next_seq;
        const sidecar::SidecarPollInputResponse extra = backend.poll_input(extra_req);
        int extra_corrections = 0;
        for (const auto& event : extra.events) {
            extra_corrections += static_cast<sidecar::InputEventKind>(event.kind) ==
                                         sidecar::InputEventKind::GeometryRequest
                                     ? 1
                                     : 0;
        }
        VKR_CHECK_EQ(extra_corrections, 0);
    }

    sidecar::SidecarUnregisterToplevelRequest unreg;
    unreg.xid = kXid;
    unreg.generation = 5;
    VKR_CHECK(backend.unregister_toplevel(unreg).applied);
}

} // namespace

int main() {
    // This binary owns the default-off proof. The backend reads std::getenv, so update MSVC's CRT
    // environment copy rather than only the Win32 process environment block.
    VKR_CHECK_EQ(_putenv_s("VKRELAY2_PRESENT_FENCE_RETIRE", ""), 0);
    // The placeholder-executor + chrome-paint + input-capture tests need no Vulkan device, so run
    // them first/unconditionally. The coordinate-helper math is pure (no window), so it runs first.
    test_coordinate_helper();
    test_static_topology_restart_latch();
    test_realization_adoption_token();
    test_real_placeholder_executor();
    test_real_initial_placement_clamp();
    test_real_snapshot_placement_and_maximize();
    test_real_chrome_paint();
    test_real_input_capture();
    test_real_focus_ownership();
    test_real_surface_input_remap();
    test_real_cursor();
    test_real_cursor_surface_clear();
    test_real_enum_windows();
    test_real_capture();
    test_real_window_title_tag();
    test_real_popup();
    test_real_popup_geometry_reveal_order();
    test_real_dpi_suggested_resize_suppressed();
    test_real_move();
    test_real_initial_placement();
    test_real_resize();
    test_real_z_order();
    test_real_hide_show();
    test_real_destroy_after_hide();
    test_real_unpainted_not_revealed_on_restore();
    test_real_popup_visibility_cascade();
    test_coordinate_inverse();
    test_real_user_geometry_feedback();
    test_real_user_minimize_restore();
    test_real_user_maximize();
    test_real_m0_maximize_cap();         // WM_GETMINMAXINFO cap bounds a cross-monitor maximize
    test_real_m0_root_sized_track_cap(); // root client can exceed default max-track exactly
    test_real_m0_over_root_guard();      // worker over-root guard drives HWND down + converges
    test_real_unpinned_caps_use_guest_root();   // no primary-work surface-size authority
    test_real_cross_dpi_geometry_convergence(); // mixed-DPI fidelity + one-echo adoption
    test_real_device_lost_containment();        // post-loss ops never re-enter the host driver
    test_real_snap_forward();                   // a non-drag snap forwards the realized geometry
    test_real_caption_click_no_drift();
    test_real_reconcile_position_only_keeps_authored_size();
    test_real_deferred_resize(); // deferred-extent (zink) live resize + register-late
                                 // adoption

    // The worker's window thread is per-monitor-DPI-aware (PMv1), so it sizes the HWND in
    // physical pixels; this test drives a cross-thread resize (resize_client) and the WndProc
    // compares client sizes, so the test thread must match the window's DPI context (else a scaled
    // panel would virtualize this thread's SetWindowPos and the size-delta the latch sees would not
    // be the one intended).
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE);
    // Not required + empty keys -> lenient selection (best available device).
    worker::RealVulkanBackend backend("", "", /*gpu_required=*/false);

    const vkrpc::CreateInstanceResponse ci = backend.create_instance({});
    if (!ci.ok) {
        std::fprintf(stderr, "integration_real_backend: skipped (no usable Vulkan: %s)\n",
                     ci.reason.c_str());
        return vkr::test::finish("integration_real_backend");
    }
    vkrpc::EnumeratePhysicalDevicesRequest er;
    er.instance = ci.instance;
    const vkrpc::EnumeratePhysicalDevicesResponse en = backend.enumerate_physical_devices(er);
    if (!en.ok || en.devices.empty()) {
        std::fprintf(stderr, "integration_real_backend: skipped (no physical device)\n");
        return vkr::test::finish("integration_real_backend");
    }

    // Faithful selection fails closed: a --gpu-required launch whose key
    // matches no host device must report "no device" rather than substitute another. A device
    // exists here (above), so neither the LUID nor the exact-name key below can match -> enumerate
    // must fail. Two keys exercised: (a) bogus LUID + bogus name; (b) no LUID + bogus name, which
    // is exactly the no-LUID exact-name path.
    auto strict_fails_closed = [](const char* name, const char* luid) {
        worker::RealVulkanBackend strict(name, luid, /*gpu_required=*/true);
        const vkrpc::CreateInstanceResponse sci = strict.create_instance({});
        VKR_CHECK(sci.ok); // instance creation does not depend on device selection
        vkrpc::EnumeratePhysicalDevicesRequest ser;
        ser.instance = sci.instance;
        const vkrpc::EnumeratePhysicalDevicesResponse sen = strict.enumerate_physical_devices(ser);
        VKR_CHECK(!sen.ok); // required selection, no match -> fail closed
    };
    strict_fails_closed("vkrelay2-no-such-gpu", "0xdeadbeefdeadbeef"); // bogus LUID + bogus name
    strict_fails_closed("vkrelay2-no-such-gpu", "");                   // no LUID -> exact-name path
    vkrpc::CreateDeviceRequest cdr;
    cdr.instance = ci.instance;
    cdr.physical_device = en.devices.front().handle;
    const vkrpc::CreateDeviceResponse cd = backend.create_device(cdr);
    VKR_CHECK(cd.ok);
    vkrpc::GetDeviceQueueRequest gqr;
    gqr.device = cd.device;
    gqr.queue_family_index = cd.queue_family_index;
    gqr.queue_index = 0;
    const vkrpc::GetDeviceQueueResponse q = backend.get_device_queue(gqr);
    VKR_CHECK(q.ok && q.queue != 0);

    vkrpc::CreateSurfaceRequest sr;
    sr.instance = ci.instance;
    sr.platform = "xcb";
    sr.xid = 0x4242; // a born-correlated guest XID
    const vkrpc::CreateSurfaceResponse surf = backend.create_surface(sr);
    VKR_CHECK(surf.ok);
    // The real backend pends the same worker-home registry entry as the mock (mock == real).
    VKR_CHECK_EQ(backend.debug_registry_surface_for_xid(0x4242), surf.surface);

    // promote executor: a toplevel the sidecar registers FIRST gets a placeholder HWND;
    // the app's create_surface for that XID PROMOTES it -- the placeholder HWND is destroyed and
    // the surface HWND becomes the sole representation (Model A). A distinct XID, so it does not
    // disturb the 0x4242 surface the rest of this test drives.
    {
        sidecar::SidecarRegisterToplevelRequest preg;
        preg.xid = 0xBEEF;
        preg.generation = 1;
        preg.width = 256;
        preg.height = 256;
        VKR_CHECK(backend.register_toplevel(preg).applied);
        VKR_CHECK(backend.debug_representation_for_xid(0xBEEF) ==
                  sidecar::Representation::Placeholder);
        const std::size_t ph_before = backend.debug_placeholder_hwnd_count();
        VKR_CHECK(ph_before >= 1);
        vkrpc::CreateSurfaceRequest psr;
        psr.instance = ci.instance;
        psr.platform = "xcb";
        psr.xid = 0xBEEF;
        const vkrpc::CreateSurfaceResponse psurf = backend.create_surface(psr);
        VKR_CHECK(psurf.ok && psurf.surface != 0);
        VKR_CHECK(backend.debug_representation_for_xid(0xBEEF) == sidecar::Representation::Surface);
        VKR_CHECK_EQ(backend.debug_placeholder_hwnd_count(),
                     ph_before - 1); // placeholder destroyed
        VKR_CHECK_EQ(backend.debug_registry_surface_for_xid(0xBEEF), psurf.surface);
        vkrpc::HandleRequest pdh;
        pdh.handle = psurf.surface;
        VKR_CHECK(backend.destroy_surface(pdh).ok);
        VKR_CHECK_EQ(backend.debug_registry_surface_for_xid(0xBEEF), static_cast<std::uint64_t>(0));
    }

    // a placeholder MOVED then PROMOTED to a surface must KEEP its
    // position -- the new surface HWND inherits the placeholder's current X-root geometry (not
    // CW_USEDEFAULT). Register at (10,10), move to (340,260), then create_surface (promote); the
    // surface's actual CLIENT origin must be the moved (340,260).
    {
        sidecar::SidecarRegisterToplevelRequest preg;
        preg.xid = 0xD403;
        preg.generation = 1;
        preg.role = "toplevel";
        preg.x = 10;
        preg.y = 10;
        preg.width = 320;
        preg.height = 240;
        VKR_CHECK(backend.register_toplevel(preg).applied);
        sidecar::SidecarUpdateToplevelRequest pmv;
        pmv.xid = 0xD403;
        pmv.generation = 2;
        pmv.x = 340;
        pmv.y = 260;
        pmv.width = 320;
        pmv.height = 240;
        VKR_CHECK(backend.update_toplevel(pmv).applied);
        vkrpc::CreateSurfaceRequest psr;
        psr.instance = ci.instance;
        psr.platform = "xcb";
        psr.xid = 0xD403;
        const vkrpc::CreateSurfaceResponse psurf = backend.create_surface(psr);
        VKR_CHECK(psurf.ok);
        VKR_CHECK(backend.debug_representation_for_xid(0xD403) == sidecar::Representation::Surface);
        sidecar::SidecarDebugEnumWindowsRequest pq;
        pq.include_actual = true;
        const sidecar::SidecarDebugEnumWindowsResponse pe = backend.debug_enum_windows(pq);
        bool found = false;
        for (const sidecar::SidecarWindowInfo& w : pe.windows) {
            if (w.xid == 0xD403) {
                found = true;
                VKR_CHECK(w.has_actual);
                VKR_CHECK_EQ(w.actual_x, 340); // the promoted surface inherited the moved position
                VKR_CHECK_EQ(w.actual_y, 260);
            }
        }
        VKR_CHECK(found);
        vkrpc::HandleRequest pdh;
        pdh.handle = psurf.surface;
        VKR_CHECK(backend.destroy_surface(pdh).ok);
        sidecar::SidecarUnregisterToplevelRequest pu;
        pu.xid = 0xD403;
        pu.generation = 3;
        backend.unregister_toplevel(pu);
    }

    // a surface that arrives BEFORE the sidecar register, then a
    // register at a nonzero origin, must MOVE the already-created surface to the registered
    // position.
    {
        vkrpc::CreateSurfaceRequest psr;
        psr.instance = ci.instance;
        psr.platform = "xcb";
        psr.xid = 0xD404;
        const vkrpc::CreateSurfaceResponse psurf = backend.create_surface(psr);
        VKR_CHECK(psurf.ok);
        sidecar::SidecarRegisterToplevelRequest preg;
        preg.xid = 0xD404;
        preg.generation = 1;
        preg.role = "toplevel";
        preg.x = 170;
        preg.y = 110;
        preg.width = 256;
        preg.height = 256;
        VKR_CHECK(backend.register_toplevel(preg).applied);
        VKR_CHECK(backend.debug_representation_for_xid(0xD404) == sidecar::Representation::Surface);
        sidecar::SidecarDebugEnumWindowsRequest pq;
        pq.include_actual = true;
        const sidecar::SidecarDebugEnumWindowsResponse pe = backend.debug_enum_windows(pq);
        bool found = false;
        for (const sidecar::SidecarWindowInfo& w : pe.windows) {
            if (w.xid == 0xD404) {
                found = true;
                VKR_CHECK(w.has_actual);
                VKR_CHECK_EQ(w.actual_x, 170); // the surface-first window moved to the register pos
                VKR_CHECK_EQ(w.actual_y, 110);
            }
        }
        VKR_CHECK(found);
        vkrpc::HandleRequest pdh;
        pdh.handle = psurf.surface;
        VKR_CHECK(backend.destroy_surface(pdh).ok);
        sidecar::SidecarUnregisterToplevelRequest pu;
        pu.xid = 0xD404;
        pu.generation = 2;
        backend.unregister_toplevel(pu);
    }

    // the §2.4 resize-convergence crux on the REAL GPU. A registered surface-backed
    // toplevel renders at the app's own extent; a sidecar-AUTHORED resize trips the latch
    // (acquire -> OUT_OF_DATE), get_surface_capabilities PINS currentExtent to the host's
    // REALIZABLE currentExtent under authority (== the authored size here since 480x320 is
    // realizable), create_swapchain at the app's stale extent is REFUSED (OUT_OF_DATE, latch still
    // set), and only create_swapchain at the pinned extent converges (clears the latch). The
    // on-screen visual half is run_geometry_smoke.sh + capture_window.ps1; the tiny/unrealizable
    // case is below.
    {
        const std::uint64_t rphys = en.devices.front().handle;
        sidecar::SidecarRegisterToplevelRequest rreg;
        rreg.xid = 0xD505;
        rreg.generation = 1;
        rreg.role = "toplevel";
        rreg.width = 320;
        rreg.height = 240;
        VKR_CHECK(backend.register_toplevel(rreg).applied);
        vkrpc::CreateSurfaceRequest rsr;
        rsr.instance = ci.instance;
        rsr.platform = "xcb";
        rsr.xid = 0xD505;
        const vkrpc::CreateSurfaceResponse rsurf = backend.create_surface(rsr);
        VKR_CHECK(rsurf.ok);
        vkrpc::GetSurfaceFormatsRequest rfmt_req;
        rfmt_req.physical_device = rphys;
        rfmt_req.surface = rsurf.surface;
        const vkrpc::GetSurfaceFormatsResponse rfmt = backend.get_surface_formats(rfmt_req);
        VKR_CHECK(rfmt.ok && !rfmt.formats.empty());
        auto make_rscr = [&](int w, int h) {
            vkrpc::CreateSwapchainRequest scr;
            scr.device = cd.device;
            scr.surface = rsurf.surface;
            scr.image_format = rfmt.formats.front().format;
            scr.color_space = rfmt.formats.front().color_space;
            scr.present_mode = 2; // FIFO (guaranteed by spec)
            scr.width = w;
            scr.height = h;
            scr.min_image_count = 2;
            scr.image_usage = vkrpc::kImageUsageColorAttachment | vkrpc::kImageUsageTransferDst;
            return scr;
        };
        // App owns the extent before any authored resize -> a 300x200 swapchain converges.
        const vkrpc::CreateSwapchainResponse rsc0 = backend.create_swapchain(make_rscr(300, 200));
        VKR_CHECK(rsc0.ok && rsc0.result == vkrpc::kVkSuccess && rsc0.swapchain != 0);

        // Sidecar authors a resize to 480x320 -> the host client resizes (WM_SIZE) -> the
        // latch.
        sidecar::SidecarUpdateToplevelRequest rrs;
        rrs.xid = 0xD505;
        rrs.generation = 2;
        rrs.width = 480;
        rrs.height = 320;
        VKR_CHECK(backend.update_toplevel(rrs).applied);
        vkrpc::AcquireNextImageRequest rair;
        rair.swapchain = rsc0.swapchain;
        VKR_CHECK_EQ(backend.acquire_next_image(rair).result, vkrpc::kVkErrorOutOfDateKhr);

        // get_surface_capabilities now PINS currentExtent to the host's realizable currentExtent
        // under authority (== 480x320 here, the realizable authored size).
        vkrpc::GetSurfaceCapabilitiesRequest rcap;
        rcap.physical_device = rphys;
        rcap.surface = rsurf.surface;
        const vkrpc::GetSurfaceCapabilitiesResponse rcaps = backend.get_surface_capabilities(rcap);
        VKR_CHECK(rcaps.ok);
        VKR_CHECK_EQ(rcaps.current_extent_width, static_cast<std::uint64_t>(480));
        VKR_CHECK_EQ(rcaps.current_extent_height, static_cast<std::uint64_t>(320));
        VKR_CHECK_EQ(rcaps.min_image_extent_width, static_cast<std::uint64_t>(480));
        VKR_CHECK_EQ(rcaps.max_image_extent_width, static_cast<std::uint64_t>(480));

        // A create_swapchain at the app's stale extent is refused -> OUT_OF_DATE, no HWND touch.
        const vkrpc::CreateSwapchainResponse rsc_bad =
            backend.create_swapchain(make_rscr(300, 200));
        VKR_CHECK(rsc_bad.ok && rsc_bad.result == vkrpc::kVkErrorOutOfDateKhr &&
                  rsc_bad.swapchain == 0);

        // Destroy the stale swapchain, then create at the AUTHORED extent -> converges (clears
        // latch).
        vkrpc::HandleRequest rdh0;
        rdh0.handle = rsc0.swapchain;
        VKR_CHECK(backend.destroy_swapchain(rdh0).ok);
        const vkrpc::CreateSwapchainResponse rsc1 = backend.create_swapchain(make_rscr(480, 320));
        VKR_CHECK(rsc1.ok && rsc1.result == vkrpc::kVkSuccess && rsc1.swapchain != 0);
        vkrpc::GetSwapchainImagesRequest rgir;
        rgir.swapchain = rsc1.swapchain;
        VKR_CHECK(backend.get_swapchain_images(rgir).ok);
        vkrpc::AcquireNextImageRequest rair1;
        rair1.swapchain = rsc1.swapchain;
        VKR_CHECK_EQ(backend.acquire_next_image(rair1).result, vkrpc::kVkSuccess);

        // include_actual: the real host CLIENT extent converged to the authored 480x320.
        sidecar::SidecarDebugEnumWindowsRequest rq;
        rq.include_actual = true;
        for (const sidecar::SidecarWindowInfo& w : backend.debug_enum_windows(rq).windows) {
            if (w.xid == 0xD505) {
                VKR_CHECK(w.has_actual);
                VKR_CHECK_EQ(w.actual_width, static_cast<std::uint32_t>(480));
                VKR_CHECK_EQ(w.actual_height, static_cast<std::uint32_t>(320));
            }
        }
        vkrpc::HandleRequest rdh1;
        rdh1.handle = rsc1.swapchain;
        VKR_CHECK(backend.destroy_swapchain(rdh1).ok);

        // A TINY authored resize (1x1) must NOT strand the app. Win32 clamps the
        // host client UP; get_surface_capabilities reports the REALIZABLE currentExtent (not 1x1),
        // and a create_swapchain AT THAT reported extent converges (no OUT_OF_DATE retry loop). A
        // request at the raw 1x1 (which the host cannot realize) is refused.
        sidecar::SidecarUpdateToplevelRequest rtiny;
        rtiny.xid = 0xD505;
        rtiny.generation = 3;
        rtiny.width = 1;
        rtiny.height = 1;
        VKR_CHECK(backend.update_toplevel(rtiny).applied);
        const vkrpc::GetSurfaceCapabilitiesResponse tcaps = backend.get_surface_capabilities(rcap);
        VKR_CHECK(tcaps.ok);
        // currentExtent is the REALIZABLE client (whatever Win32 gave the authored 1x1 -- a clamp
        // up to the window minimum, or 1x1), never the raw authored size if unrealizable.
        VKR_CHECK(tcaps.current_extent_width >= 1 && tcaps.current_extent_height >= 1);
        // The no-retry-loop proof: create_swapchain AT the reported realizable extent converges
        // (kVkSuccess), regardless of whether Win32 clamped. Before the fix, caps pinned the raw
        // 1x1 and a create there would loop on OUT_OF_DATE.
        const vkrpc::CreateSwapchainResponse rsc_tiny =
            backend.create_swapchain(make_rscr(static_cast<int>(tcaps.current_extent_width),
                                               static_cast<int>(tcaps.current_extent_height)));
        VKR_CHECK(rsc_tiny.ok && rsc_tiny.result == vkrpc::kVkSuccess && rsc_tiny.swapchain != 0);
        // A zero-extent request is a hard reject, not an OUT_OF_DATE retry.
        VKR_CHECK(!backend.create_swapchain(make_rscr(0, 240)).ok);
        vkrpc::HandleRequest rdh2;
        rdh2.handle = rsc_tiny.swapchain;
        VKR_CHECK(backend.destroy_swapchain(rdh2).ok);

        vkrpc::HandleRequest rsdh;
        rsdh.handle = rsurf.surface;
        VKR_CHECK(backend.destroy_surface(rsdh).ok);
        sidecar::SidecarUnregisterToplevelRequest ru;
        ru.xid = 0xD505;
        ru.generation = 4;
        backend.unregister_toplevel(ru);
    }

    // WSI capability queries on the real host surface (so the validation-layer pass also
    // covers vkGetPhysicalDeviceSurface*KHR): the sentinel extent, honest formats/present
    // modes/support, and whether TRANSFER_DST (needed for the clear) is available.
    const std::uint64_t phys = en.devices.front().handle;
    vkrpc::GetSurfaceCapabilitiesRequest scap_req;
    scap_req.physical_device = phys;
    scap_req.surface = surf.surface;
    const vkrpc::GetSurfaceCapabilitiesResponse scap = backend.get_surface_capabilities(scap_req);
    VKR_CHECK(scap.ok);
    VKR_CHECK_EQ(scap.current_extent_width, vkrpc::kDynamicExtentSentinel);
    vkrpc::GetSurfaceFormatsRequest sfmt_req;
    sfmt_req.physical_device = phys;
    sfmt_req.surface = surf.surface;
    const vkrpc::GetSurfaceFormatsResponse sfmt = backend.get_surface_formats(sfmt_req);
    VKR_CHECK(sfmt.ok && !sfmt.formats.empty());
    vkrpc::GetSurfacePresentModesRequest spm_req;
    spm_req.physical_device = phys;
    spm_req.surface = surf.surface;
    const vkrpc::GetSurfacePresentModesResponse spm = backend.get_surface_present_modes(spm_req);
    VKR_CHECK(spm.ok && !spm.present_modes.empty());
    vkrpc::GetSurfaceSupportRequest ssup_req;
    ssup_req.physical_device = phys;
    ssup_req.queue_family_index = 0;
    ssup_req.surface = surf.surface;
    const vkrpc::GetSurfaceSupportResponse ssup = backend.get_surface_support(ssup_req);
    VKR_CHECK(ssup.ok && ssup.supported);

    // Drive the swapchain params from the queried caps (the ICD's flow): prefer
    // B8G8R8A8_UNORM/SRGB + FIFO; TRANSFER_DST only if advertised.
    int chosen_format = sfmt.formats.front().format;
    int chosen_color_space = sfmt.formats.front().color_space;
    for (const auto& f : sfmt.formats) {
        if (f.format == 44 && f.color_space == 0) {
            chosen_format = f.format;
            chosen_color_space = f.color_space;
            break;
        }
    }
    int chosen_present_mode = spm.present_modes.front();
    for (const int m : spm.present_modes) {
        if (m == 2) {
            chosen_present_mode = m;
            break;
        }
    }
    const bool can_clear = (scap.supported_usage_flags &
                            static_cast<std::uint64_t>(vkrpc::kImageUsageTransferDst)) != 0;

    vkrpc::CreateSwapchainRequest scr;
    scr.device = cd.device;
    scr.surface = surf.surface;
    scr.image_format = chosen_format;
    scr.color_space = chosen_color_space;
    scr.present_mode = chosen_present_mode;
    scr.width = 256;
    scr.height = 256;
    scr.min_image_count = static_cast<int>(scap.min_image_count);
    scr.image_usage = can_clear
                          ? (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)
                          : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    const vkrpc::CreateSwapchainResponse sc = backend.create_swapchain(scr);
    if (!sc.ok) {
        // A device with no WSI present support for our hidden window: a legitimate skip.
        std::fprintf(stderr, "integration_real_backend: skipped (no swapchain: %s)\n",
                     sc.reason.c_str());
        return vkr::test::finish("integration_real_backend");
    }
    vkrpc::GetSwapchainImagesRequest gir;
    gir.swapchain = sc.swapchain;
    const vkrpc::GetSwapchainImagesResponse imgs = backend.get_swapchain_images(gir);
    VKR_CHECK(imgs.ok && !imgs.images.empty());

    // A command pool + buffer for the app's recorded clear.
    vkrpc::CreateCommandPoolRequest cpr;
    cpr.device = cd.device;
    cpr.queue_family_index = cd.queue_family_index;
    const vkrpc::CreateCommandPoolResponse pool = backend.create_command_pool(cpr);
    VKR_CHECK(pool.ok);
    vkrpc::AllocateCommandBuffersRequest abr;
    abr.command_pool = pool.command_pool;
    abr.count = 1;
    const vkrpc::AllocateCommandBuffersResponse bufs = backend.allocate_command_buffers(abr);
    VKR_CHECK(bufs.ok && bufs.command_buffers.size() == 1);
    const std::uint64_t cmd = bufs.command_buffers[0];

    // Sync objects threaded through the frame (freed at teardown, after a device-idling
    // destroy_swapchain): acquire signals sem_acq; submit waits sem_acq, signals sem_done +
    // fence; present waits sem_done.
    std::uint64_t fence_h = 0, sem_acq_h = 0, sem_done_h = 0;

    // Clean frame: acquire(signal sem) -> record(barrier+clear+barrier) -> submit(wait sem,
    // signal sem, fence) -> wait fence -> present(wait sem). Shows the window.
    if (can_clear) {
        vkrpc::CreateSemaphoreRequest csr;
        csr.device = cd.device;
        const vkrpc::CreateSemaphoreResponse sem_acq = backend.create_semaphore(csr);
        const vkrpc::CreateSemaphoreResponse sem_done = backend.create_semaphore(csr);
        VKR_CHECK(sem_acq.ok && sem_done.ok);
        vkrpc::CreateFenceRequest cfr;
        cfr.device = cd.device;
        const vkrpc::CreateFenceResponse fence = backend.create_fence(cfr);
        VKR_CHECK(fence.ok);
        sem_acq_h = sem_acq.semaphore;
        sem_done_h = sem_done.semaphore;
        fence_h = fence.fence;

        vkrpc::AcquireNextImageRequest ar;
        ar.swapchain = sc.swapchain;
        ar.timeout = UINT64_MAX;
        ar.semaphore = sem_acq.semaphore; // real binary semaphore, signaled on acquire
        const vkrpc::AcquireNextImageResponse acq = backend.acquire_next_image(ar);
        VKR_CHECK(acq.ok);
        VKR_CHECK_EQ(acq.result, vkrpc::kVkSuccess);

        const vkrpc::RecordCommandBufferRequest rec =
            clear_recording(cmd, imgs.images[acq.image_index], 0.12f, 0.56f, 1.0f);
        VKR_CHECK(backend.record_command_buffer(rec).ok);

        vkrpc::QueueSubmitRequest sub;
        sub.queue = q.queue;
        sub.waits.push_back({sem_acq.semaphore, VK_PIPELINE_STAGE_TRANSFER_BIT});
        sub.command_buffers = {cmd};
        sub.signal_semaphores = {sem_done.semaphore};
        sub.fence = fence.fence;
        const vkrpc::QueueSubmitResponse s = backend.queue_submit(sub);
        VKR_CHECK(s.ok);
        VKR_CHECK_EQ(s.result, vkrpc::kVkSuccess);

        vkrpc::WaitForFencesRequest wf;
        wf.fences = {fence.fence};
        wf.wait_all = true;
        wf.timeout = UINT64_MAX;
        const vkrpc::WaitForFencesResponse w = backend.wait_for_fences(wf);
        VKR_CHECK(w.ok);
        VKR_CHECK_EQ(w.result, vkrpc::kVkSuccess);

        vkrpc::QueuePresentRequest pr;
        pr.queue = q.queue;
        pr.wait_semaphores = {sem_done.semaphore};
        pr.presents.push_back({sc.swapchain, acq.image_index});
        const vkrpc::QueuePresentResponse pres = backend.queue_present(pr);
        VKR_CHECK(pres.ok);
        VKR_CHECK(pres.result == vkrpc::kVkSuccess || pres.result == vkrpc::kVkSuboptimalKhr);
        std::fprintf(stderr, "integration_real_backend: app-recorded clear frame presented\n");
    } else {
        std::fprintf(stderr, "integration_real_backend: clear unavailable (no TRANSFER_DST)\n");
    }

    // Drive a REAL WM_SIZE: resize the surface's HWND to a different client size. The
    // WndProc's size-delta filter marks the slot geometry-dirty.
    const HWND hwnd = backend.debug_surface_hwnd(surf.surface);
    VKR_CHECK(hwnd != nullptr);
    resize_client(hwnd, 400, 300);

    // While dirty: acquire and present return OUT_OF_DATE (ok=true) without the driver.
    vkrpc::AcquireNextImageRequest ar_dirty;
    ar_dirty.swapchain = sc.swapchain;
    ar_dirty.timeout = UINT64_MAX;
    const vkrpc::AcquireNextImageResponse acq_dirty = backend.acquire_next_image(ar_dirty);
    VKR_CHECK(acq_dirty.ok);
    VKR_CHECK_EQ(acq_dirty.result, vkrpc::kVkErrorOutOfDateKhr);
    vkrpc::QueuePresentRequest pr_dirty;
    pr_dirty.queue = q.queue;
    pr_dirty.presents.push_back({sc.swapchain, 0});
    const vkrpc::QueuePresentResponse pres_dirty = backend.queue_present(pr_dirty);
    VKR_CHECK(pres_dirty.ok);
    VKR_CHECK_EQ(pres_dirty.result, vkrpc::kVkErrorOutOfDateKhr);

    // Recreate at the new size clears the latch (clear-after-sync): destroy the old
    // swapchain, create a new one matching the window.
    vkrpc::HandleRequest h_swc;
    h_swc.handle = sc.swapchain;
    VKR_CHECK(backend.destroy_swapchain(h_swc).ok);

    // The clean frame's command buffer was recorded against sc's image; destroying sc
    // invalidated it, so a submit of cmd now must be refused (its baked commands reference a
    // dead VkImage) -- never reaching vkQueueSubmit.
    if (can_clear) {
        vkrpc::QueueSubmitRequest sub_stale;
        sub_stale.queue = q.queue;
        sub_stale.command_buffers = {cmd};
        VKR_CHECK(!backend.queue_submit(sub_stale).ok);
    }

    vkrpc::CreateSwapchainRequest scr2 = scr;
    scr2.width = 400;
    scr2.height = 300;
    const vkrpc::CreateSwapchainResponse sc2 = backend.create_swapchain(scr2);
    VKR_CHECK(sc2.ok);
    vkrpc::GetSwapchainImagesRequest gir2;
    gir2.swapchain = sc2.swapchain;
    VKR_CHECK(backend.get_swapchain_images(gir2).ok);
    vkrpc::AcquireNextImageRequest ar2;
    ar2.swapchain = sc2.swapchain;
    ar2.timeout = UINT64_MAX;
    const vkrpc::AcquireNextImageResponse acq_clean = backend.acquire_next_image(ar2);
    VKR_CHECK(acq_clean.ok);
    VKR_CHECK_EQ(acq_clean.result, vkrpc::kVkSuccess); // latch cleared

    std::fprintf(stderr,
                 "integration_real_backend: geometry-dirty latch verified on a real WM_SIZE\n");

    // Ordered teardown. destroy_swapchain idles the device (flushing the clean frame's
    // submit/present), so the sync leaves are safe to free; then pool, surface, device,
    // instance (child-before-parent).
    vkrpc::HandleRequest h;
    h.handle = sc2.swapchain;
    VKR_CHECK(backend.destroy_swapchain(h).ok);
    if (fence_h != 0) {
        h.handle = fence_h;
        VKR_CHECK(backend.destroy_fence(h).ok);
        h.handle = sem_acq_h;
        VKR_CHECK(backend.destroy_semaphore(h).ok);
        h.handle = sem_done_h;
        VKR_CHECK(backend.destroy_semaphore(h).ok);
    }
    h.handle = pool.command_pool;
    VKR_CHECK(backend.destroy_command_pool(h).ok);
    h.handle = surf.surface;
    VKR_CHECK(backend.destroy_surface(h).ok);
    // Destroy drops the registry entry.
    VKR_CHECK_EQ(backend.debug_registry_surface_for_xid(0x4242), static_cast<std::uint64_t>(0));
    h.handle = cd.device;
    VKR_CHECK(backend.destroy_device(h).ok);
    h.handle = ci.instance;
    VKR_CHECK(backend.destroy_instance(h).ok);

    return vkr::test::finish("integration_real_backend");
}
