// Worker-present extent negotiation: in-process real-backend proof that an app can choose
// its OWN swapchain extent and the worker makes the host Win32 window become exactly that.
//
// The literal-vkcube run exposed the bug: `get_surface_capabilities` advertised
// minImageExtent = maxImageExtent = the current (256) window, so an app that picked any other size
// was clamped to 256 (its framebuffers then mismatched). This test drives the fix directly,
// constructing RealVulkanBackend in-process so it can read the surface's HWND:
//   - the advertised extent range is permissive (currentExtent sentinel; min 1x1; max the monitor
//     work-area), NOT the 256 window;
//   - a swapchain created at a non-square 503x377 succeeds, and the surface's HWND client rect
//   becomes
//     EXACTLY 503x377 (convergence), so a framebuffer at 503x377 is accepted (no mismatch);
//   - a request the host cannot realize (beyond the work-area) returns ok=true + result=OUT_OF_DATE
//     and creates no swapchain, instead of silently falling back to a stale extent.
//
// Windows-only (registered under WIN32 + Vulkan SDK). Skips gracefully (still passes) if no usable
// Vulkan device / no WSI support.
#include "windows/worker/real_vulkan_backend.hpp"

#include "common/vkrpc/vulkan_session.hpp"
#include "tests/test_assert.hpp"

#include <cstdint>
#include <cstdio>

using namespace vkr;

int main() {
    // The worker's window thread is per-monitor-DPI-aware (PMv1), so it sizes the HWND and
    // reports currentExtent in PHYSICAL pixels. This test reads the HWND client rect directly, so
    // its own thread must be per-monitor-aware too -- otherwise GetClientRect would return a
    // virtualized size on a scaled panel and the convergence assertion would spuriously fail.
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE);

    worker::RealVulkanBackend backend("", "", false);

    const vkrpc::CreateInstanceResponse ci = backend.create_instance({});
    if (!ci.ok) {
        std::fprintf(stderr, "integration_real_extent: skipped (no instance: %s)\n",
                     ci.reason.c_str());
        return vkr::test::finish("integration_real_extent");
    }
    vkrpc::EnumeratePhysicalDevicesRequest er;
    er.instance = ci.instance;
    const vkrpc::EnumeratePhysicalDevicesResponse en = backend.enumerate_physical_devices(er);
    if (!en.ok || en.devices.empty()) {
        std::fprintf(stderr, "integration_real_extent: skipped (no physical device)\n");
        return vkr::test::finish("integration_real_extent");
    }
    const std::uint64_t phys = en.devices.front().handle;
    vkrpc::CreateDeviceRequest cdr;
    cdr.instance = ci.instance;
    cdr.physical_device = phys;
    const vkrpc::CreateDeviceResponse cd = backend.create_device(cdr);
    VKR_CHECK(cd.ok);

    vkrpc::CreateSurfaceRequest sr;
    sr.instance = ci.instance;
    const vkrpc::CreateSurfaceResponse surf = backend.create_surface(sr);
    VKR_CHECK(surf.ok);

    // --- The advertised range is permissive (app picks), not the 256 window ---
    vkrpc::GetSurfaceCapabilitiesRequest scap_req;
    scap_req.physical_device = phys;
    scap_req.surface = surf.surface;
    const vkrpc::GetSurfaceCapabilitiesResponse caps = backend.get_surface_capabilities(scap_req);
    VKR_CHECK(caps.ok);
    // sentinel currentExtent (the app sizes the swapchain to its own framebuffer).
    VKR_CHECK_EQ(static_cast<long long>(caps.current_extent_width),
                 static_cast<long long>(vkrpc::kDynamicExtentSentinel));
    // A 1x1 floor and a work-area ceiling large enough to request 503x377 -- NOT the 256 window
    // (which was the stale-extent root cause and would clamp the app).
    VKR_CHECK_EQ(static_cast<long long>(caps.min_image_extent_width), 1LL);
    VKR_CHECK_EQ(static_cast<long long>(caps.min_image_extent_height), 1LL);
    VKR_CHECK(caps.max_image_extent_width >= 503 && caps.max_image_extent_height >= 377);
    VKR_CHECK(caps.max_image_extent_width != 256); // not the hidden host window

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
    const int fmt = sfmt.formats.front().format;
    const int color_space = sfmt.formats.front().color_space;
    int present_mode = spm.present_modes.front();
    for (const int m : spm.present_modes) {
        if (m == VK_PRESENT_MODE_FIFO_KHR) {
            present_mode = m;
            break;
        }
    }

    auto make_swapchain_req = [&](int w, int h) {
        vkrpc::CreateSwapchainRequest scr;
        scr.device = cd.device;
        scr.surface = surf.surface;
        scr.image_format = fmt;
        scr.color_space = color_space;
        scr.present_mode = present_mode;
        scr.width = w;
        scr.height = h;
        scr.min_image_count = static_cast<int>(caps.min_image_count);
        scr.image_usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        return scr;
    };

    // --- A non-square 503x377 swapchain: the app's choice is honored and the HWND converges ---
    const int kW = 503;
    const int kH = 377;
    const vkrpc::CreateSwapchainResponse sc = backend.create_swapchain(make_swapchain_req(kW, kH));
    if (!sc.ok && sc.result == vkrpc::kVkSuccess) {
        // Genuinely no WSI swapchain support on this box (e.g. headless CI) -> skip cleanly.
        std::fprintf(stderr, "integration_real_extent: skipped (no swapchain: %s)\n",
                     sc.reason.c_str());
        return vkr::test::finish("integration_real_extent");
    }
    VKR_CHECK(sc.ok);
    VKR_CHECK_EQ(sc.result, vkrpc::kVkSuccess); // converged, not OUT_OF_DATE
    VKR_CHECK(sc.swapchain != 0);

    // The surface's real Win32 client rect is EXACTLY the app's requested extent (convergence).
    const HWND hwnd = backend.debug_surface_hwnd(surf.surface);
    VKR_CHECK(hwnd != nullptr);
    // The window is per-monitor-DPI-aware (PMv1), so its client/currentExtent are physical pixels
    // and the app's chosen extent is realized 1:1 (not virtualized/upscaled on a scaled panel).
    VKR_CHECK_EQ(GetAwarenessFromDpiAwarenessContext(GetWindowDpiAwarenessContext(hwnd)),
                 DPI_AWARENESS_PER_MONITOR_AWARE);
    RECT client{};
    VKR_CHECK(GetClientRect(hwnd, &client) != 0);
    VKR_CHECK_EQ(static_cast<long long>(client.right - client.left), static_cast<long long>(kW));
    VKR_CHECK_EQ(static_cast<long long>(client.bottom - client.top), static_cast<long long>(kH));

    // A framebuffer at 503x377 over the swapchain image is accepted (the original 500-vs-256
    // mismatch is gone): images -> view -> color render pass -> framebuffer at the chosen extent.
    vkrpc::GetSwapchainImagesRequest gir;
    gir.swapchain = sc.swapchain;
    const vkrpc::GetSwapchainImagesResponse imgs = backend.get_swapchain_images(gir);
    VKR_CHECK(imgs.ok && !imgs.images.empty());
    vkrpc::CreateImageViewRequest ivr;
    ivr.image = imgs.images.front();
    ivr.view_type = VK_IMAGE_VIEW_TYPE_2D;
    ivr.format = fmt;
    ivr.swizzle_r = ivr.swizzle_g = ivr.swizzle_b = ivr.swizzle_a = VK_COMPONENT_SWIZZLE_IDENTITY;
    ivr.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    ivr.base_mip_level = 0;
    ivr.level_count = 1;
    ivr.base_array_layer = 0;
    ivr.layer_count = 1;
    const vkrpc::CreateImageViewResponse iv = backend.create_image_view(ivr);
    VKR_CHECK(iv.ok && iv.image_view != 0);
    vkrpc::CreateRenderPassRequest rpr;
    rpr.device = cd.device;
    vkrpc::AttachmentDesc att;
    att.format = fmt;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.load_op = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att.store_op = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencil_load_op = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencil_store_op = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    att.final_layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    rpr.attachments = {att};
    rpr.color_attachment = 0;
    rpr.color_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    const vkrpc::CreateRenderPassResponse rp = backend.create_render_pass(rpr);
    VKR_CHECK(rp.ok && rp.render_pass != 0);
    vkrpc::CreateFramebufferRequest fbr;
    fbr.device = cd.device;
    fbr.render_pass = rp.render_pass;
    fbr.image_view = iv.image_view;
    fbr.width = kW;
    fbr.height = kH;
    fbr.layers = 1;
    const vkrpc::CreateFramebufferResponse fb = backend.create_framebuffer(fbr);
    VKR_CHECK(fb.ok && fb.framebuffer != 0); // accepted at 503x377 -- the bug is fixed

    // --- An unrealizable request returns OUT_OF_DATE, not a stale-extent swapchain ---
    // Far beyond any monitor work-area: the window cannot converge, so the worker must report
    // ok=true + result=OUT_OF_DATE and create NO swapchain (never silently fall back to caps).
    const vkrpc::CreateSwapchainResponse big =
        backend.create_swapchain(make_swapchain_req(100000, 100000));
    VKR_CHECK(big.ok); // ran cleanly -- not an RPC/handle fault
    VKR_CHECK_EQ(big.result, vkrpc::kVkErrorOutOfDateKhr);
    VKR_CHECK(big.swapchain == 0);

    std::fprintf(stderr,
                 "integration_real_extent: app-chosen 503x377 honored (HWND client converged); "
                 "unrealizable request -> OUT_OF_DATE\n");

    // Teardown (child-before-parent).
    vkrpc::HandleRequest h;
    h.handle = fb.framebuffer;
    VKR_CHECK(backend.destroy_framebuffer(h).ok);
    h.handle = rp.render_pass;
    VKR_CHECK(backend.destroy_render_pass(h).ok);
    h.handle = iv.image_view;
    VKR_CHECK(backend.destroy_image_view(h).ok);
    h.handle = sc.swapchain;
    VKR_CHECK(backend.destroy_swapchain(h).ok);
    h.handle = surf.surface;
    VKR_CHECK(backend.destroy_surface(h).ok);
    h.handle = cd.device;
    VKR_CHECK(backend.destroy_device(h).ok);
    h.handle = ci.instance;
    VKR_CHECK(backend.destroy_instance(h).ok);

    return vkr::test::finish("integration_real_extent");
}
