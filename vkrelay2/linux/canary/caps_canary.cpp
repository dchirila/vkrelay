// vkrelay2 caps-convergence canary.
//
// The end-to-end proof for the ICD's surface-caps cache: a REAL Vulkan client through the loader
// + pinned vkrelay2 ICD against the MOCK daemon + real sidecar (the first Vulkan canary to drive
// the ICD against the mock backend -- the path under test is the ICD's cache, which no in-process
// integration test can reach). Flow:
//
//   1. Map one xcb toplevel at size A; Vulkan bring-up: instance -> device -> xcb surface ->
//      caps query (uncached: no live swapchain) -> swapchain at A.
//   2. Steady state: a few acquire -> present(SUCCESS) frames, polling caps each frame -- the
//      polls the cache exists for (the smoke asserts hits >= 1 via the ICD trace counters).
//   3. The canary issues its OWN ConfigureRequest resize A -> B (the resize_canary pattern). The
//      sidecar forwards it; the sidecar-AUTHORED resize marks the mock surface geometry-dirty
//      and pins the authored extent B, so the next acquire returns OUT_OF_DATE over the
//      wire -- the honest result signal the cache invalidates on.
//   4. THE ASSERTION (the locked design's crisp contract): the FIRST caps query
//      after that first non-success acquire/present must observe B -- i.e. it re-queried the
//      worker instead of serving the stale cached caps. A never-invalidate cache build fails
//      HERE (the RED variant).
//   5. Swapchain recreate at B (clears the latch) -> presents SUCCEED again -> PASS.
//
// Prints greppable CAPS-CANARY markers; exit 0 only on a full PASS (run_caps_smoke.sh owns the
// bring-up, so a failed step here is a FAIL, not a skip -- except the no-X/no-ICD cases which
// skip cleanly for driverless boxes).

#include <vulkan/vulkan.h>
#include <vulkan/vulkan_xcb.h>

#include <xcb/xcb.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <vector>

namespace {

constexpr std::uint16_t kAW = 320, kAH = 240; // size A (bring-up)
constexpr std::uint16_t kBW = 480, kBH = 360; // size B (the guest-requested resize)
constexpr int kSteadyFrames = 5;              // presents+polls before the resize
constexpr int kSignalWaitIters = 100;         // x 100ms: bounded wait for the resize signal

void logf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::fprintf(stderr, "CAPS-CANARY: ");
    std::vfprintf(stderr, fmt, ap);
    std::fprintf(stderr, "\n");
    va_end(ap);
    std::fflush(stderr);
}

void sleep_ms(long ms) {
    timespec ts{ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&ts, nullptr);
}

} // namespace

int main() {
    xcb_connection_t* xcb = xcb_connect(nullptr, nullptr);
    if (xcb == nullptr || xcb_connection_has_error(xcb)) {
        logf("SKIP (no X display)");
        return 0;
    }
    const xcb_setup_t* setup = xcb_get_setup(xcb);
    xcb_screen_t* screen = xcb_setup_roots_iterator(setup).data;
    const xcb_window_t window = xcb_generate_id(xcb);
    const std::uint32_t mask = XCB_CW_EVENT_MASK;
    const std::uint32_t vals[1] = {XCB_EVENT_MASK_STRUCTURE_NOTIFY};
    xcb_create_window(xcb, XCB_COPY_FROM_PARENT, window, screen->root, 60, 45, kAW, kAH, 0,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, mask, vals);
    xcb_map_window(xcb, window);
    xcb_flush(xcb);
    logf("xid=%u w=%u h=%u -> w=%u h=%u", static_cast<unsigned>(window), kAW, kAH, kBW, kBH);

    // Wait for the WM to map us (first ConfigureNotify), so the sidecar has registered the
    // toplevel before the Vulkan surface binds to its xid. Bounded; proceed regardless (the
    // registry adopts a surface-first ordering too).
    for (int i = 0; i < 50; ++i) {
        bool mapped = false;
        xcb_generic_event_t* ev;
        while ((ev = xcb_poll_for_event(xcb)) != nullptr) {
            if ((ev->response_type & ~0x80) == XCB_CONFIGURE_NOTIFY) {
                mapped = true;
            }
            free(ev);
        }
        if (mapped) {
            break;
        }
        sleep_ms(100);
    }

    const char* inst_exts[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_XCB_SURFACE_EXTENSION_NAME};
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.apiVersion = VK_API_VERSION_1_1;
    app.pApplicationName = "vkrelay2-caps-canary";
    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = 2;
    ici.ppEnabledExtensionNames = inst_exts;
    VkInstance instance = VK_NULL_HANDLE;
    if (vkCreateInstance(&ici, nullptr, &instance) != VK_SUCCESS) {
        logf("SKIP (vkCreateInstance failed -- no ICD/worker?)");
        xcb_disconnect(xcb);
        return 0;
    }

    int rc = 1;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkSemaphore sem = VK_NULL_HANDLE;

    {
        std::uint32_t pd_count = 0;
        vkEnumeratePhysicalDevices(instance, &pd_count, nullptr);
        if (pd_count == 0) {
            logf("FAIL (no physical device)");
            goto teardown;
        }
        std::vector<VkPhysicalDevice> pds(pd_count);
        vkEnumeratePhysicalDevices(instance, &pd_count, pds.data());
        const VkPhysicalDevice phys = pds[0];

        const float prio = 1.0f;
        VkDeviceQueueCreateInfo qci{};
        qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qci.queueFamilyIndex = 0;
        qci.queueCount = 1;
        qci.pQueuePriorities = &prio;
        const char* dev_exts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
        VkDeviceCreateInfo dci{};
        dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qci;
        dci.enabledExtensionCount = 1;
        dci.ppEnabledExtensionNames = dev_exts;
        if (vkCreateDevice(phys, &dci, nullptr, &device) != VK_SUCCESS) {
            logf("FAIL (vkCreateDevice)");
            goto teardown;
        }
        vkGetDeviceQueue(device, 0, 0, &queue);

        VkXcbSurfaceCreateInfoKHR sci{};
        sci.sType = VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR;
        sci.connection = xcb;
        sci.window = window;
        if (vkCreateXcbSurfaceKHR(instance, &sci, nullptr, &surface) != VK_SUCCESS) {
            logf("FAIL (vkCreateXcbSurfaceKHR)");
            goto teardown;
        }

        VkSurfaceCapabilitiesKHR caps{};
        if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, surface, &caps) != VK_SUCCESS) {
            logf("FAIL (initial caps query)");
            goto teardown;
        }
        std::uint32_t fmt_count = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &fmt_count, nullptr);
        if (fmt_count == 0) {
            logf("FAIL (no surface formats)");
            goto teardown;
        }
        std::vector<VkSurfaceFormatKHR> formats(fmt_count);
        vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &fmt_count, formats.data());

        VkSwapchainCreateInfoKHR scci{};
        scci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        scci.surface = surface;
        scci.minImageCount = caps.minImageCount < 2 ? 2 : caps.minImageCount;
        scci.imageFormat = formats[0].format;
        scci.imageColorSpace = formats[0].colorSpace;
        scci.imageExtent = {kAW, kAH};
        scci.imageArrayLayers = 1;
        scci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        scci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        scci.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
        scci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        scci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        scci.clipped = VK_TRUE;
        if (vkCreateSwapchainKHR(device, &scci, nullptr, &swapchain) != VK_SUCCESS) {
            logf("FAIL (vkCreateSwapchainKHR at %ux%u)", kAW, kAH);
            goto teardown;
        }
        logf("bringup ok (swapchain at %ux%u)", kAW, kAH);

        VkSemaphoreCreateInfo semci{};
        semci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        if (vkCreateSemaphore(device, &semci, nullptr, &sem) != VK_SUCCESS) {
            logf("FAIL (vkCreateSemaphore)");
            goto teardown;
        }

        // 2) Steady state: present + poll caps each frame. Every present must SUCCEED (nothing
        //    has resized) and every poll returns the same answer -- these are the per-frame polls
        //    the cache serves (counter-asserted by the smoke, not observable from here).
        for (int f = 0; f < kSteadyFrames; ++f) {
            std::uint32_t idx = 0;
            const VkResult acq =
                vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, sem, VK_NULL_HANDLE, &idx);
            if (acq != VK_SUCCESS) {
                logf("FAIL (steady acquire returned %d on frame %d)", static_cast<int>(acq), f);
                goto teardown;
            }
            VkPresentInfoKHR pi{};
            pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
            pi.waitSemaphoreCount = 1;
            pi.pWaitSemaphores = &sem;
            pi.swapchainCount = 1;
            pi.pSwapchains = &swapchain;
            pi.pImageIndices = &idx;
            const VkResult pres = vkQueuePresentKHR(queue, &pi);
            if (pres != VK_SUCCESS) {
                logf("FAIL (steady present returned %d on frame %d)", static_cast<int>(pres), f);
                goto teardown;
            }
            VkSurfaceCapabilitiesKHR poll{};
            if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, surface, &poll) != VK_SUCCESS) {
                logf("FAIL (steady caps poll on frame %d)", f);
                goto teardown;
            }
            sleep_ms(50);
        }
        logf("steady ok (%d presents + caps polls)", kSteadyFrames);

        // 3) The guest resize A -> B (ConfigureRequest; the sidecar forwards it as an authored
        //    resize, which latches the mock surface geometry-dirty + pins extent B).
        {
            const std::uint32_t cfg[2] = {kBW, kBH};
            xcb_configure_window(xcb, window, XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
                                 cfg);
            xcb_flush(xcb);
            logf("resized (requested %ux%u)", kBW, kBH);
        }

        // 4) Wait (bounded) for the honest signal, then assert the crisp contract: the FIRST
        //    caps query after the FIRST non-success acquire/present observes B.
        bool signaled = false;
        for (int i = 0; i < kSignalWaitIters && !signaled; ++i) {
            std::uint32_t idx = 0;
            const VkResult acq =
                vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, sem, VK_NULL_HANDLE, &idx);
            if (acq == VK_SUCCESS) {
                // Not resized yet: present (must succeed) + keep polling caps (cache hits).
                VkPresentInfoKHR pi{};
                pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
                pi.waitSemaphoreCount = 1;
                pi.pWaitSemaphores = &sem;
                pi.swapchainCount = 1;
                pi.pSwapchains = &swapchain;
                pi.pImageIndices = &idx;
                const VkResult pres = vkQueuePresentKHR(queue, &pi);
                if (pres == VK_SUCCESS) {
                    VkSurfaceCapabilitiesKHR poll{};
                    (void) vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, surface, &poll);
                    sleep_ms(100);
                    continue;
                }
                logf("signal (present returned %d on iter %d)", static_cast<int>(pres), i);
            } else {
                logf("signal (acquire returned %d on iter %d)", static_cast<int>(acq), i);
            }
            signaled = true;
            VkSurfaceCapabilitiesKHR after{};
            if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, surface, &after) != VK_SUCCESS) {
                logf("FAIL (caps query after the signal)");
                goto teardown;
            }
            if (after.currentExtent.width != kBW || after.currentExtent.height != kBH) {
                logf("FAIL (STALE caps after the signal: first query returned %ux%u, want %ux%u "
                     "-- the cache did not invalidate)",
                     after.currentExtent.width, after.currentExtent.height, kBW, kBH);
                goto teardown;
            }
            logf("converged (first caps query after the signal returned %ux%u)", kBW, kBH);
        }
        if (!signaled) {
            logf("FAIL (no non-success acquire/present within %d iters -- the sidecar resize "
                 "never latched)",
                 kSignalWaitIters);
            goto teardown;
        }

        // 5) Recreate at B (clears the latch) and prove presents flow again.
        vkDestroySwapchainKHR(device, swapchain, nullptr);
        swapchain = VK_NULL_HANDLE;
        scci.imageExtent = {kBW, kBH};
        if (vkCreateSwapchainKHR(device, &scci, nullptr, &swapchain) != VK_SUCCESS) {
            logf("FAIL (swapchain recreate at %ux%u)", kBW, kBH);
            goto teardown;
        }
        {
            std::uint32_t idx = 0;
            const VkResult acq =
                vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, sem, VK_NULL_HANDLE, &idx);
            if (acq != VK_SUCCESS) {
                logf("FAIL (post-recreate acquire returned %d)", static_cast<int>(acq));
                goto teardown;
            }
            VkPresentInfoKHR pi{};
            pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
            pi.waitSemaphoreCount = 1;
            pi.pWaitSemaphores = &sem;
            pi.swapchainCount = 1;
            pi.pSwapchains = &swapchain;
            pi.pImageIndices = &idx;
            const VkResult pres = vkQueuePresentKHR(queue, &pi);
            if (pres != VK_SUCCESS) {
                logf("FAIL (post-recreate present returned %d)", static_cast<int>(pres));
                goto teardown;
            }
        }
        logf("recreated + presented at %ux%u", kBW, kBH);
        logf("PASS");
        rc = 0;
    }

teardown:
    if (sem != VK_NULL_HANDLE) {
        vkDestroySemaphore(device, sem, nullptr);
    }
    if (swapchain != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(device, swapchain, nullptr);
    }
    if (surface != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(instance, surface, nullptr);
    }
    if (device != VK_NULL_HANDLE) {
        vkDestroyDevice(device, nullptr);
    }
    vkDestroyInstance(instance, nullptr);
    xcb_disconnect(xcb);
    return rc;
}
