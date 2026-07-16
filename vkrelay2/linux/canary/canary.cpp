// vkrelay2 clear canary.
//
// A minimal, intentionally-boring native Vulkan program: it creates an xcb window on the
// private Xwayland display, then drives instance -> device -> surface -> swapchain ->
// acquire -> record(clear) -> submit -> wait -> present for a few frames. Run through the
// real Vulkan loader with VK_ICD_FILENAMES pinned to the vkrelay2 ICD (the launcher does
// that), the clear color appears in a real Win32 window on the Windows desktop via
// worker-present. It is a loader/ICD/session proof, not a rendering-feature proof -- the
// only commands are an image barrier + clear (a minimal command subset).
//
// It prints greppable markers to stderr (the boundary smoke asserts the chain). On any
// failure it returns non-zero with a clear message; if no usable Vulkan device is present it
// exits 0 with a "skipped" note so a driverless box does not fail the smoke.

#include <vulkan/vulkan.h>
#include <vulkan/vulkan_xcb.h>

#include <xcb/xcb.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <vector>

namespace {

constexpr int kFrames = 3;

void log(const char* msg) {
    std::fprintf(stderr, "vkrelay2-canary: %s\n", msg);
}

bool check(VkResult r, const char* what) {
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
        std::fprintf(stderr, "vkrelay2-canary: %s failed (VkResult %d)\n", what,
                     static_cast<int>(r));
        return false;
    }
    return true;
}

void sleep_ms(long ms) {
    timespec ts{ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&ts, nullptr);
}

} // namespace

int main() {
    // 1) A real xcb window on the (private) X display. In worker-present mode the pixels go to
    //    a host Win32 window; this xcb window is the app's placeholder (geometry/input come in
    //    via the sidecar). If we cannot reach an X server, skip cleanly.
    xcb_connection_t* xcb = xcb_connect(nullptr, nullptr);
    if (xcb == nullptr || xcb_connection_has_error(xcb)) {
        log("skipped (no X display)");
        return 0;
    }
    const xcb_setup_t* setup = xcb_get_setup(xcb);
    xcb_screen_t* screen = xcb_setup_roots_iterator(setup).data;
    xcb_window_t window = xcb_generate_id(xcb);
    xcb_create_window(xcb, XCB_COPY_FROM_PARENT, window, screen->root, 0, 0, 256, 256, 0,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, 0, nullptr);
    xcb_map_window(xcb, window);
    xcb_flush(xcb);
    log("xcb window created");

    // 2) Instance with the WSI surface extensions (our ICD advertises both).
    const char* inst_exts[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_XCB_SURFACE_EXTENSION_NAME};
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.apiVersion = VK_API_VERSION_1_1;
    app.pApplicationName = "vkrelay2-canary";
    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = 2;
    ici.ppEnabledExtensionNames = inst_exts;
    VkInstance instance = VK_NULL_HANDLE;
    if (vkCreateInstance(&ici, nullptr, &instance) != VK_SUCCESS) {
        // No loader/ICD/worker reachable: a legitimate skip on a box without the stack up.
        log("skipped (vkCreateInstance failed -- no ICD/worker?)");
        xcb_disconnect(xcb);
        return 0;
    }
    log("instance created");

    int rc = 1; // pessimistic; set to 0 only on a clean run
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    std::uint32_t pd_count = 0;
    vkEnumeratePhysicalDevices(instance, &pd_count, nullptr);
    if (pd_count == 0) {
        log("no physical device");
        vkDestroyInstance(instance, nullptr);
        xcb_disconnect(xcb);
        return 1;
    }
    std::vector<VkPhysicalDevice> phys_devs(pd_count);
    vkEnumeratePhysicalDevices(instance, &pd_count, phys_devs.data());
    phys = phys_devs[0];
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(phys, &props);
    std::fprintf(stderr, "vkrelay2-canary: device '%s'\n", props.deviceName);

    // 3) Device with one graphics queue (family 0) + VK_KHR_swapchain.
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
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;

    if (!check(vkCreateDevice(phys, &dci, nullptr, &device), "vkCreateDevice")) {
        goto teardown;
    }
    log("device created");
    vkGetDeviceQueue(device, 0, 0, &queue);

    // 4) Surface (xcb -> worker Win32 surface) + WSI queries to choose swapchain params.
    {
        VkXcbSurfaceCreateInfoKHR sci{};
        sci.sType = VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR;
        sci.connection = xcb;
        sci.window = window;
        if (!check(vkCreateXcbSurfaceKHR(instance, &sci, nullptr, &surface),
                   "vkCreateXcbSurfaceKHR")) {
            goto teardown;
        }
        log("surface created");

        VkBool32 supported = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(phys, 0, surface, &supported);
        VkSurfaceCapabilitiesKHR caps{};
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, surface, &caps);
        std::uint32_t fmt_count = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &fmt_count, nullptr);
        std::vector<VkSurfaceFormatKHR> formats(fmt_count);
        vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &fmt_count, formats.data());
        std::uint32_t pm_count = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(phys, surface, &pm_count, nullptr);
        std::vector<VkPresentModeKHR> modes(pm_count);
        vkGetPhysicalDeviceSurfacePresentModesKHR(phys, surface, &pm_count, modes.data());
        if (fmt_count == 0 || pm_count == 0) {
            log("surface reports no formats/present modes");
            goto teardown;
        }
        log("surface queried");

        VkSurfaceFormatKHR chosen = formats[0];
        for (const auto& f : formats) {
            if (f.format == VK_FORMAT_B8G8R8A8_UNORM &&
                f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                chosen = f;
                break;
            }
        }
        VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
        const bool transfer_dst = (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) != 0;
        if (!transfer_dst) {
            log("surface lacks TRANSFER_DST -- cannot clear; skipping the clear frames");
        }
        std::uint32_t want = caps.minImageCount < 2 ? 2 : caps.minImageCount;
        if (caps.maxImageCount != 0 && want > caps.maxImageCount) {
            want = caps.maxImageCount;
        }

        VkSwapchainCreateInfoKHR scci{};
        scci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        scci.surface = surface;
        scci.minImageCount = want;
        scci.imageFormat = chosen.format;
        scci.imageColorSpace = chosen.colorSpace;
        scci.imageExtent = {256, 256}; // the app picks its extent (dynamic extent)
        scci.imageArrayLayers = 1;
        VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        if (transfer_dst) {
            usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        }
        scci.imageUsage = usage;
        scci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        scci.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
        scci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        scci.presentMode = mode;
        scci.clipped = VK_TRUE;
        if (!check(vkCreateSwapchainKHR(device, &scci, nullptr, &swapchain),
                   "vkCreateSwapchainKHR")) {
            goto teardown;
        }
        log("swapchain created");

        std::uint32_t img_count = 0;
        vkGetSwapchainImagesKHR(device, swapchain, &img_count, nullptr);
        std::vector<VkImage> images(img_count);
        vkGetSwapchainImagesKHR(device, swapchain, &img_count, images.data());

        VkCommandPoolCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = 0;
        if (!check(vkCreateCommandPool(device, &pci, nullptr, &pool), "vkCreateCommandPool")) {
            goto teardown;
        }
        VkCommandBufferAllocateInfo cbai{};
        cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cbai.commandPool = pool;
        cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbai.commandBufferCount = 1;
        if (!check(vkAllocateCommandBuffers(device, &cbai, &cmd), "vkAllocateCommandBuffers")) {
            goto teardown;
        }
        VkFenceCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        if (!check(vkCreateFence(device, &fci, nullptr, &fence), "vkCreateFence")) {
            goto teardown;
        }

        if (!transfer_dst) {
            // We proved loader/ICD/session/surface/swapchain; without TRANSFER_DST we cannot
            // record a clear, so stop here cleanly (a rare WSI configuration).
            rc = 0;
            log("done (no clear -- surface without TRANSFER_DST)");
            goto teardown;
        }

        int presented = 0;
        for (int frame = 0; frame < kFrames; ++frame) {
            // Per-frame binary semaphores (avoids cross-frame reuse hazards in this simple
            // fully-CPU-synced loop).
            VkSemaphore sem_acq = VK_NULL_HANDLE;
            VkSemaphore sem_done = VK_NULL_HANDLE;
            VkSemaphoreCreateInfo semci{};
            semci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
            if (!check(vkCreateSemaphore(device, &semci, nullptr, &sem_acq), "vkCreateSemaphore") ||
                !check(vkCreateSemaphore(device, &semci, nullptr, &sem_done),
                       "vkCreateSemaphore")) {
                goto teardown;
            }

            std::uint32_t image_index = 0;
            VkResult acq = vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, sem_acq,
                                                 VK_NULL_HANDLE, &image_index);
            if (acq == VK_ERROR_OUT_OF_DATE_KHR) {
                vkDestroySemaphore(device, sem_acq, nullptr);
                vkDestroySemaphore(device, sem_done, nullptr);
                continue; // no resize path here; just skip
            }
            if (!check(acq, "vkAcquireNextImageKHR")) {
                vkDestroySemaphore(device, sem_acq, nullptr);
                vkDestroySemaphore(device, sem_done, nullptr);
                goto teardown;
            }

            VkCommandBufferBeginInfo bi{};
            bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vkBeginCommandBuffer(cmd, &bi);
            VkImageSubresourceRange range{};
            range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            range.levelCount = 1;
            range.layerCount = 1;
            VkImageMemoryBarrier to_dst{};
            to_dst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            to_dst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            to_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            to_dst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            to_dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            to_dst.image = images[image_index];
            to_dst.subresourceRange = range;
            to_dst.srcAccessMask = 0;
            to_dst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                                 &to_dst);
            VkClearColorValue color{};
            color.float32[0] = 0.12f;
            color.float32[1] = 0.56f;
            color.float32[2] = 1.0f;
            color.float32[3] = 1.0f;
            vkCmdClearColorImage(cmd, images[image_index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                 &color, 1, &range);
            VkImageMemoryBarrier to_present = to_dst;
            to_present.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            to_present.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            to_present.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            to_present.dstAccessMask = 0;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1,
                                 &to_present);
            vkEndCommandBuffer(cmd);

            const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
            VkSubmitInfo subi{};
            subi.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            subi.waitSemaphoreCount = 1;
            subi.pWaitSemaphores = &sem_acq;
            subi.pWaitDstStageMask = &wait_stage;
            subi.commandBufferCount = 1;
            subi.pCommandBuffers = &cmd;
            subi.signalSemaphoreCount = 1;
            subi.pSignalSemaphores = &sem_done;
            if (!check(vkQueueSubmit(queue, 1, &subi, fence), "vkQueueSubmit")) {
                vkDestroySemaphore(device, sem_acq, nullptr);
                vkDestroySemaphore(device, sem_done, nullptr);
                goto teardown;
            }
            vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
            vkResetFences(device, 1, &fence);

            VkPresentInfoKHR pi{};
            pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
            pi.waitSemaphoreCount = 1;
            pi.pWaitSemaphores = &sem_done;
            pi.swapchainCount = 1;
            pi.pSwapchains = &swapchain;
            pi.pImageIndices = &image_index;
            VkResult pres = vkQueuePresentKHR(queue, &pi);
            vkDestroySemaphore(device, sem_acq, nullptr);
            vkDestroySemaphore(device, sem_done, nullptr);
            if (pres == VK_SUCCESS || pres == VK_SUBOPTIMAL_KHR) {
                ++presented;
                std::fprintf(stderr, "vkrelay2-canary: presented frame %d\n", frame);
            }
            sleep_ms(150); // keep the window visible a moment
        }
        rc = (presented >= 1) ? 0 : 1;
        std::fprintf(stderr, "vkrelay2-canary: presented %d frame(s)\n", presented);
    }

teardown:
    if (fence != VK_NULL_HANDLE) {
        vkDestroyFence(device, fence, nullptr);
    }
    if (pool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(device, pool, nullptr);
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
    if (rc == 0) {
        log("done");
    }
    return rc;
}
