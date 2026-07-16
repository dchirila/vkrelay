// vkrelay2 core-1.0 synchronization canary -- the VkEvent object model + vkGetFenceStatus.
//
// A surfaceless native Vulkan program runs through the real loader and the session-pinned vkrelay2
// ICD (run_vk_event_smoke.sh). Unlike the vk13 canaries, it advertises NOTHING -- events and
// vkGetFenceStatus are core Vulkan 1.0 -- so this runs on the
// DEFAULT lane and asserts no extension/rollup state; it proves the three layers of the event
// surface plus the fence-status completeness fix reach the real GPU:
//   - vkGetFenceStatus: a signaled-create fence reports VK_SUCCESS, an unsignaled one VK_NOT_READY;
//     a submit signals it (VK_SUCCESS after the wait), a reset returns it to VK_NOT_READY.
//   - host event round-trip: vkCreateEvent -> VK_EVENT_RESET; vkSetEvent -> VK_EVENT_SET;
//     vkResetEvent -> VK_EVENT_RESET.
//   - device-set event: a command buffer with vkCmdSetEvent, submitted + fenced, makes the host
//     vkGetEventStatus report VK_EVENT_SET (the device set reached the host).
//   - vkCmdWaitEvents guarding a transfer: a HOST-set event (no device-set race) gates a command
//     buffer that transitions an offscreen image UNDEFINED->TRANSFER_DST inside the wait's image
//     barrier, clears it to a known color, transitions it to TRANSFER_SRC, and copies it to a
//     readback buffer. The readback MUST equal the clear color -- proof the wait released and the
//     wait_events barrier's layout transition reached the host (a wrong old/new layout breaks the
//     clear/copy).
//
// Greppable markers on stdout; run_vk_event_smoke.sh gates on them. Skips cleanly (exit 0) when no
// ICD/worker stack is reachable; FAILs (nonzero) on a real regression.

#include <vulkan/vulkan.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

constexpr std::uint32_t kDim = 64;
constexpr VkDeviceSize kBufBytes = static_cast<VkDeviceSize>(kDim) * kDim * 4;
constexpr VkFormat kColorFormat = VK_FORMAT_R8G8B8A8_UNORM;
// The known clear color, as R8G8B8A8_UNORM bytes: (0x33, 0x99, 0xCC, 0xFF).
constexpr std::uint8_t kClearR = 0x33;
constexpr std::uint8_t kClearG = 0x99;
constexpr std::uint8_t kClearB = 0xCC;
constexpr std::uint8_t kClearA = 0xFF;

bool check(VkResult r, const char* what) {
    if (r != VK_SUCCESS) {
        std::printf("VK-EVENT-CANARY: FAIL (%s -> VkResult %d)\n", what, static_cast<int>(r));
        return false;
    }
    return true;
}

int find_memory_type(const VkPhysicalDeviceMemoryProperties& mp, std::uint32_t type_bits,
                     VkMemoryPropertyFlags required, VkMemoryPropertyFlags forbidden) {
    for (std::uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if ((type_bits & (1u << i)) == 0) {
            continue;
        }
        const VkMemoryPropertyFlags f = mp.memoryTypes[i].propertyFlags;
        if ((f & required) == required && (f & forbidden) == 0) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// Reset + begin a command buffer for one-time submit.
bool begin_cb(VkCommandBuffer cmd) {
    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    return check(vkBeginCommandBuffer(cmd, &bi), "vkBeginCommandBuffer");
}

// Submit `cmd` on `queue`, signal `fence`, wait for it, then reset the fence.
bool submit_wait(VkQueue queue, VkCommandBuffer cmd, VkFence fence, VkDevice device) {
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    if (!check(vkQueueSubmit(queue, 1, &si, fence), "vkQueueSubmit")) {
        return false;
    }
    if (!check(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX), "vkWaitForFences")) {
        return false;
    }
    return check(vkResetFences(device, 1, &fence), "vkResetFences");
}

} // namespace

int main() {
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    // Core 1.0 objects -- no extension, no version claim beyond the baseline.
    app.apiVersion = VK_API_VERSION_1_2;
    app.pApplicationName = "vkrelay2-vk-event-canary";
    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    VkInstance instance = VK_NULL_HANDLE;
    if (vkCreateInstance(&ici, nullptr, &instance) != VK_SUCCESS) {
        std::printf("VK-EVENT-CANARY: SKIP (vkCreateInstance failed -- no ICD/worker?)\n");
        return 0;
    }

    std::uint32_t pd_count = 0;
    vkEnumeratePhysicalDevices(instance, &pd_count, nullptr);
    if (pd_count == 0) {
        std::printf("VK-EVENT-CANARY: FAIL (no physical device)\n");
        vkDestroyInstance(instance, nullptr);
        return 2;
    }
    std::vector<VkPhysicalDevice> pds(pd_count);
    vkEnumeratePhysicalDevices(instance, &pd_count, pds.data());
    VkPhysicalDevice phys = pds[0];

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(phys, &props);
    std::printf("VK-EVENT-CANARY: device '%s' apiVersion=%u.%u\n", props.deviceName,
                VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion));

    VkPhysicalDeviceMemoryProperties memprops{};
    vkGetPhysicalDeviceMemoryProperties(phys, &memprops);

    const float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = 0;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;

    int rc = 1;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory img_mem = VK_NULL_HANDLE;
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory buf_mem = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkFence fence_sig = VK_NULL_HANDLE;
    VkEvent event = VK_NULL_HANDLE;

    if (!check(vkCreateDevice(phys, &dci, nullptr, &device), "vkCreateDevice")) {
        goto teardown;
    }
    vkGetDeviceQueue(device, 0, 0, &queue);

    {
        // --- vkGetFenceStatus: signaled create -> VK_SUCCESS, unsignaled -> VK_NOT_READY. ---
        VkFenceCreateInfo fci_sig{};
        fci_sig.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fci_sig.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        if (!check(vkCreateFence(device, &fci_sig, nullptr, &fence_sig),
                   "vkCreateFence(signaled)")) {
            goto teardown;
        }
        VkFenceCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        if (!check(vkCreateFence(device, &fci, nullptr, &fence), "vkCreateFence")) {
            goto teardown;
        }
        const VkResult sig_status = vkGetFenceStatus(device, fence_sig);
        const VkResult unsig_status = vkGetFenceStatus(device, fence);
        std::printf("VK-EVENT-CANARY: fence_status signaled=%d unsignaled=%d\n",
                    static_cast<int>(sig_status), static_cast<int>(unsig_status));
        if (sig_status != VK_SUCCESS || unsig_status != VK_NOT_READY) {
            std::printf(
                "VK-EVENT-CANARY: FAIL (vkGetFenceStatus wrong: expected signaled=SUCCESS(0)"
                " unsignaled=NOT_READY(1))\n");
            goto teardown;
        }

        // --- Host event round-trip: create -> RESET, set -> SET, reset -> RESET. ---
        VkEventCreateInfo eci{};
        eci.sType = VK_STRUCTURE_TYPE_EVENT_CREATE_INFO;
        if (!check(vkCreateEvent(device, &eci, nullptr, &event), "vkCreateEvent")) {
            goto teardown;
        }
        const VkResult s0 = vkGetEventStatus(device, event);
        if (!check(vkSetEvent(device, event), "vkSetEvent")) {
            goto teardown;
        }
        const VkResult s1 = vkGetEventStatus(device, event);
        if (!check(vkResetEvent(device, event), "vkResetEvent")) {
            goto teardown;
        }
        const VkResult s2 = vkGetEventStatus(device, event);
        std::printf("VK-EVENT-CANARY: host_event fresh=%d set=%d reset=%d\n", static_cast<int>(s0),
                    static_cast<int>(s1), static_cast<int>(s2));
        if (s0 != VK_EVENT_RESET || s1 != VK_EVENT_SET || s2 != VK_EVENT_RESET) {
            std::printf("VK-EVENT-CANARY: FAIL (host event round-trip wrong: expected "
                        "fresh=RESET(4) set=SET(3) reset=RESET(4))\n");
            goto teardown;
        }

        // --- Offscreen image (TRANSFER_DST + TRANSFER_SRC) + host-visible readback buffer. ---
        VkImageCreateInfo imgci{};
        imgci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imgci.imageType = VK_IMAGE_TYPE_2D;
        imgci.format = kColorFormat;
        imgci.extent = {kDim, kDim, 1};
        imgci.mipLevels = 1;
        imgci.arrayLayers = 1;
        imgci.samples = VK_SAMPLE_COUNT_1_BIT;
        imgci.tiling = VK_IMAGE_TILING_OPTIMAL;
        imgci.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        imgci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (!check(vkCreateImage(device, &imgci, nullptr, &image), "vkCreateImage")) {
            goto teardown;
        }
        VkMemoryRequirements img_req{};
        vkGetImageMemoryRequirements(device, image, &img_req);
        const int img_type = find_memory_type(memprops, img_req.memoryTypeBits,
                                              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
        if (img_type < 0) {
            std::printf("VK-EVENT-CANARY: FAIL (no device-local memory type)\n");
            goto teardown;
        }
        VkMemoryAllocateInfo img_ai{};
        img_ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        img_ai.allocationSize = img_req.size;
        img_ai.memoryTypeIndex = static_cast<std::uint32_t>(img_type);
        if (!check(vkAllocateMemory(device, &img_ai, nullptr, &img_mem), "vkAllocateMemory(img)") ||
            !check(vkBindImageMemory(device, image, img_mem, 0), "vkBindImageMemory")) {
            goto teardown;
        }

        VkBufferCreateInfo bci{};
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size = kBufBytes;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (!check(vkCreateBuffer(device, &bci, nullptr, &buf), "vkCreateBuffer")) {
            goto teardown;
        }
        VkMemoryRequirements buf_req{};
        vkGetBufferMemoryRequirements(device, buf, &buf_req);
        const int buf_type = find_memory_type(
            memprops, buf_req.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0);
        if (buf_type < 0) {
            std::printf("VK-EVENT-CANARY: FAIL (no host-visible coherent memory type)\n");
            goto teardown;
        }
        VkMemoryAllocateInfo buf_ai{};
        buf_ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        buf_ai.allocationSize = buf_req.size;
        buf_ai.memoryTypeIndex = static_cast<std::uint32_t>(buf_type);
        if (!check(vkAllocateMemory(device, &buf_ai, nullptr, &buf_mem), "vkAllocateMemory(buf)") ||
            !check(vkBindBufferMemory(device, buf, buf_mem, 0), "vkBindBufferMemory")) {
            goto teardown;
        }

        VkCommandPoolCreateInfo cpci{};
        cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        cpci.queueFamilyIndex = 0;
        if (!check(vkCreateCommandPool(device, &cpci, nullptr, &pool), "vkCreateCommandPool")) {
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

        // --- Device-set event: a CB with vkCmdSetEvent, submitted + fenced, makes the host status
        // SET (and, as a bonus, the submit signals `fence` so vkGetFenceStatus reports SUCCESS).
        // ---
        if (!check(vkResetEvent(device, event), "vkResetEvent(before device-set)")) {
            goto teardown;
        }
        if (!begin_cb(cmd)) {
            goto teardown;
        }
        vkCmdSetEvent(cmd, event, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);
        if (!check(vkEndCommandBuffer(cmd), "vkEndCommandBuffer(set_event)")) {
            goto teardown;
        }
        {
            // Submit but keep the fence signaled long enough to poll it.
            VkSubmitInfo si{};
            si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            si.commandBufferCount = 1;
            si.pCommandBuffers = &cmd;
            if (!check(vkQueueSubmit(queue, 1, &si, fence), "vkQueueSubmit(set_event)") ||
                !check(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX),
                       "vkWaitForFences(set_event)")) {
                goto teardown;
            }
        }
        const VkResult fence_after_submit = vkGetFenceStatus(device, fence);
        const VkResult event_after_device_set = vkGetEventStatus(device, event);
        std::printf("VK-EVENT-CANARY: after_submit fence=%d device_set_event=%d\n",
                    static_cast<int>(fence_after_submit), static_cast<int>(event_after_device_set));
        if (fence_after_submit != VK_SUCCESS || event_after_device_set != VK_EVENT_SET) {
            std::printf(
                "VK-EVENT-CANARY: FAIL (a submitted vkCmdSetEvent did not reach the host, or"
                " the fence did not signal)\n");
            goto teardown;
        }
        // Reset the fence to unsignaled and confirm vkGetFenceStatus follows.
        if (!check(vkResetFences(device, 1, &fence), "vkResetFences(after device-set)")) {
            goto teardown;
        }
        if (vkGetFenceStatus(device, fence) != VK_NOT_READY) {
            std::printf("VK-EVENT-CANARY: FAIL (a reset fence still reports signaled)\n");
            goto teardown;
        }

        // --- vkCmdWaitEvents guarding a transfer: HOST-set the event, then a CB that waits on it,
        // transitions the image UNDEFINED->TRANSFER_DST inside the wait's image barrier, clears it,
        // transitions it to TRANSFER_SRC, and copies it out. Readback MUST equal the clear color.
        // ---
        if (!check(vkSetEvent(device, event), "vkSetEvent(host, for wait)")) {
            goto teardown;
        }
        if (!begin_cb(cmd)) {
            goto teardown;
        }
        {
            // The wait's image barrier owns the UNDEFINED->TRANSFER_DST transition.
            VkImageMemoryBarrier to_dst{};
            to_dst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            to_dst.srcAccessMask = 0;
            to_dst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            to_dst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            to_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            to_dst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            to_dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            to_dst.image = image;
            to_dst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            // A global memory barrier in the same wait, too (exercises the memory-barrier slot).
            VkMemoryBarrier glob{};
            glob.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            glob.srcAccessMask = 0;
            glob.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            // A host-signaled event waits from HOST stage.
            vkCmdWaitEvents(cmd, 1, &event, VK_PIPELINE_STAGE_HOST_BIT,
                            VK_PIPELINE_STAGE_TRANSFER_BIT, 1, &glob, 0, nullptr, 1, &to_dst);

            VkClearColorValue clear{};
            clear.float32[0] = static_cast<float>(kClearR) / 255.0f;
            clear.float32[1] = static_cast<float>(kClearG) / 255.0f;
            clear.float32[2] = static_cast<float>(kClearB) / 255.0f;
            clear.float32[3] = static_cast<float>(kClearA) / 255.0f;
            VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdClearColorImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1,
                                 &range);

            // Plain pipeline barrier TRANSFER_DST -> TRANSFER_SRC for the copy.
            VkImageMemoryBarrier to_src{};
            to_src.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            to_src.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            to_src.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            to_src.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            to_src.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            to_src.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            to_src.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            to_src.image = image;
            to_src.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                                 &to_src);

            VkBufferImageCopy copy{};
            copy.bufferOffset = 0;
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageExtent = {kDim, kDim, 1};
            vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &copy);
        }
        if (!check(vkEndCommandBuffer(cmd), "vkEndCommandBuffer(wait_events)")) {
            goto teardown;
        }
        if (!submit_wait(queue, cmd, fence, device)) {
            goto teardown;
        }

        // Map the readback and check the top-left pixel equals the clear color.
        void* mapped = nullptr;
        if (!check(vkMapMemory(device, buf_mem, 0, kBufBytes, 0, &mapped), "vkMapMemory")) {
            goto teardown;
        }
        const auto* px = static_cast<const std::uint8_t*>(mapped);
        std::printf("VK-EVENT-CANARY: readback pixel0=%02x%02x%02x%02x expected=%02x%02x%02x%02x\n",
                    px[0], px[1], px[2], px[3], kClearR, kClearG, kClearB, kClearA);
        const bool ok =
            px[0] == kClearR && px[1] == kClearG && px[2] == kClearB && px[3] == kClearA;
        vkUnmapMemory(device, buf_mem);
        if (!ok) {
            std::printf("VK-EVENT-CANARY: FAIL (wait_events-guarded clear did not land -- the wait "
                        "did not release or the image barrier's layout transition was wrong)\n");
            goto teardown;
        }

        std::printf("VK-EVENT-CANARY: PASS (vkGetFenceStatus + the VkEvent object model + host/"
                    "device set + a vkCmdWaitEvents-guarded transfer all reached the real GPU)\n");
        rc = 0;
    }

teardown:
    if (device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device);
    }
    if (event != VK_NULL_HANDLE) {
        vkDestroyEvent(device, event, nullptr);
    }
    if (fence != VK_NULL_HANDLE) {
        vkDestroyFence(device, fence, nullptr);
    }
    if (fence_sig != VK_NULL_HANDLE) {
        vkDestroyFence(device, fence_sig, nullptr);
    }
    if (buf != VK_NULL_HANDLE) {
        vkDestroyBuffer(device, buf, nullptr);
    }
    if (buf_mem != VK_NULL_HANDLE) {
        vkFreeMemory(device, buf_mem, nullptr);
    }
    if (image != VK_NULL_HANDLE) {
        vkDestroyImage(device, image, nullptr);
    }
    if (img_mem != VK_NULL_HANDLE) {
        vkFreeMemory(device, img_mem, nullptr);
    }
    if (pool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(device, pool, nullptr);
    }
    if (device != VK_NULL_HANDLE) {
        vkDestroyDevice(device, nullptr);
    }
    vkDestroyInstance(instance, nullptr);
    if (rc == 0) {
        std::printf("VK-EVENT-CANARY: done\n");
    }
    return rc;
}
