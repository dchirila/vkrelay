// Real-GPU proof that a wedged vkAcquireNextImageKHR is INTERRUPTIBLE on session abort -- the
// GPU-level half of the orphan-window fix that the headless tests cannot show.
//
// It reproduces the exact Blender wedge on the host GPU: create a real surface + swapchain, then
// acquire images (nonblocking) until the swapchain is EXHAUSTED, so the next acquire with the
// guest's UINT64_MAX timeout has no image and blocks. That blocking acquire is issued on a thread;
// the main thread then calls backend.abort_session() (what the liveness observer does on app
// peer-close) and asserts the blocked acquire RELEASES within a bounded deadline with a clean
// "session aborting" fault -- instead of parking the pump thread forever (the pre-fix leak).
//
// Windows-only (WIN32 + Vulkan SDK). Skips gracefully with no usable device/surface/swapchain. A
// hard CTest TIMEOUT bounds a regression that fails to release.
#include "windows/worker/real_vulkan_backend.hpp"

#include "common/vkrpc/vulkan_session.hpp"
#include "tests/test_assert.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

#include <vulkan/vulkan.h>

using namespace vkr;

int main() {
    worker::RealVulkanBackend backend("", "", false);

    const vkrpc::CreateInstanceResponse ci = backend.create_instance({});
    if (!ci.ok) {
        std::fprintf(stderr, "integration_real_acquire_abort: skipped (no instance)\n");
        return vkr::test::finish("integration_real_acquire_abort");
    }
    vkrpc::EnumeratePhysicalDevicesRequest er;
    er.instance = ci.instance;
    const vkrpc::EnumeratePhysicalDevicesResponse en = backend.enumerate_physical_devices(er);
    if (!en.ok || en.devices.empty()) {
        std::fprintf(stderr, "integration_real_acquire_abort: skipped (no physical device)\n");
        return vkr::test::finish("integration_real_acquire_abort");
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
    if (!surf.ok) {
        std::fprintf(stderr, "integration_real_acquire_abort: skipped (no surface)\n");
        return vkr::test::finish("integration_real_acquire_abort");
    }
    vkrpc::GetSurfaceFormatsRequest sfmt_req;
    sfmt_req.physical_device = phys;
    sfmt_req.surface = surf.surface;
    const vkrpc::GetSurfaceFormatsResponse sfmt = backend.get_surface_formats(sfmt_req);
    VKR_CHECK(sfmt.ok && !sfmt.formats.empty());
    vkrpc::GetSurfaceCapabilitiesRequest scap_req;
    scap_req.physical_device = phys;
    scap_req.surface = surf.surface;
    const vkrpc::GetSurfaceCapabilitiesResponse scap = backend.get_surface_capabilities(scap_req);
    VKR_CHECK(scap.ok);
    vkrpc::GetSurfacePresentModesRequest spm_req;
    spm_req.physical_device = phys;
    spm_req.surface = surf.surface;
    const vkrpc::GetSurfacePresentModesResponse spm = backend.get_surface_present_modes(spm_req);
    VKR_CHECK(spm.ok && !spm.present_modes.empty());

    int present_mode = spm.present_modes.front();
    for (const int m : spm.present_modes) {
        if (m == VK_PRESENT_MODE_FIFO_KHR) {
            present_mode = m;
            break;
        }
    }
    vkrpc::CreateSwapchainRequest scr;
    scr.device = cd.device;
    scr.surface = surf.surface;
    scr.image_format = sfmt.formats.front().format;
    scr.color_space = sfmt.formats.front().color_space;
    scr.present_mode = present_mode;
    scr.width = 256;
    scr.height = 256;
    scr.min_image_count = static_cast<int>(scap.min_image_count);
    scr.image_usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    const vkrpc::CreateSwapchainResponse sc = backend.create_swapchain(scr);
    if (!sc.ok) {
        std::fprintf(stderr, "integration_real_acquire_abort: skipped (no swapchain: %s)\n",
                     sc.reason.c_str());
        return vkr::test::finish("integration_real_acquire_abort");
    }

    // Exhaust the swapchain: acquire (timeout 0 => nonblocking) until no image is available. Each
    // successful acquire holds an image (fresh semaphore per attempt, none presented).
    bool exhausted = false;
    for (int i = 0; i < 16 && !exhausted; ++i) {
        const vkrpc::CreateSemaphoreResponse s = backend.create_semaphore({cd.device});
        VKR_CHECK(s.ok);
        vkrpc::AcquireNextImageRequest a;
        a.swapchain = sc.swapchain;
        a.timeout = 0;
        a.semaphore = s.semaphore;
        const vkrpc::AcquireNextImageResponse r = backend.acquire_next_image(a);
        VKR_CHECK(r.ok);
        if (r.result == VK_NOT_READY || r.result == VK_TIMEOUT) {
            exhausted = true; // this attempt's semaphore was NOT signaled; drop it
        }
    }
    if (!exhausted) {
        // A driver with a very deep swapchain -- can't cheaply force the wedge; skip rather than
        // lie.
        std::fprintf(stderr, "integration_real_acquire_abort: skipped (swapchain not exhausted)\n");
        return vkr::test::finish("integration_real_acquire_abort");
    }

    // The wedge: a blocking acquire (UINT64_MAX) on a thread. With the swapchain exhausted it has
    // no image, so it loops in the abort-aware poll -- releasing ONLY via abort.
    const vkrpc::CreateSemaphoreResponse block_sem = backend.create_semaphore({cd.device});
    VKR_CHECK(block_sem.ok);
    std::atomic<bool> returned{false};
    vkrpc::AcquireNextImageResponse blocked{};
    std::thread blocker([&] {
        vkrpc::AcquireNextImageRequest a;
        a.swapchain = sc.swapchain;
        a.timeout = UINT64_MAX;
        a.semaphore = block_sem.semaphore;
        blocked = backend.acquire_next_image(a);
        returned.store(true, std::memory_order_relaxed);
    });

    // Confirm it is genuinely wedged (has not returned after several poll quanta).
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    VKR_CHECK(!returned.load());

    // Abort (what the observer does on app death) and time the release.
    const auto t0 = std::chrono::steady_clock::now();
    backend.abort_session();
    long long release_ms = -1;
    for (int i = 0; i < 400; ++i) { // up to ~4 s, returns early
        if (returned.load(std::memory_order_relaxed)) {
            release_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - t0)
                             .count();
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    blocker.join();

    VKR_CHECK(returned.load()); // the wedged acquire released
    VKR_CHECK(release_ms >= 0); // within the bound
    VKR_CHECK(!blocked.ok);     // as a clean session-abort fault, not a spurious image
    std::fprintf(stderr,
                 "integration_real_acquire_abort: wedged acquire released %lld ms after abort\n",
                 release_ms);

    return vkr::test::finish("integration_real_acquire_abort");
}
