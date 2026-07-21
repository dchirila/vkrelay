// Queue-order readback canary: proves a readback submitted with NO completion
// proof of its own is still promoted + downloaded when a LATER same-queue submit's proof is waited.
// Vulkan queue order: a fence/timeline signal operation's first synchronization scope includes ALL
// commands earlier in submission order on that queue (spec 7.3.1/7.4.1), so waiting that later
// proof legally synchronizes the earlier copy -- the relay must deliver the bytes, not stale zeros.
//
// Native Vulkan through the real loader + the session-pinned vkrelay2 ICD (no WSI, no GL):
//   phase A (fence proof):    clear image -> copy image->buf1, submitted with NO fence/semaphore;
//                             then an EMPTY fence-only vkQueueSubmit; wait THAT fence; map buf1.
//   phase B (timeline proof): clear image (2nd color) -> copy image->buf2, again unproven;
//                             then an EMPTY batch signalling a timeline value; vkWaitSemaphores;
//                             map buf2. buf2 uses vkBindBufferMemory2, so exact pixels also pin the
//                             Bind2 -> destination-memory -> fence-time download bookkeeping.
// Self-judging, greppable: run_readback_smoke.sh gates on the printed pixels + PASS. Pre-fix (RED)
// both phases read stale zeros (the proofs matched no armed record); post-fix the exact clear
// colors. Skips cleanly (exit 0) when no ICD/worker stack is reachable.
#include <vulkan/vulkan.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

constexpr std::uint32_t kDim = 16;
constexpr VkDeviceSize kBufBytes = kDim * kDim * 4;

bool check(VkResult r, const char* what) {
    if (r != VK_SUCCESS) {
        std::fprintf(stderr, "ORDER-CANARY: %s failed (VkResult %d)\n", what, static_cast<int>(r));
        return false;
    }
    return true;
}

// Finds a memory type in `type_bits` with exactly-suitable flags for the relay's memory classes.
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

void image_barrier(VkCommandBuffer cb, VkImage image, VkImageLayout from, VkImageLayout to,
                   VkAccessFlags src_access, VkAccessFlags dst_access) {
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask = src_access;
    b.dstAccessMask = dst_access;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &b);
}

// Records: image -> TRANSFER_DST, clear `color`, -> TRANSFER_SRC, copy into `buf`. `first` uses
// UNDEFINED as the initial layout (fresh image); a re-record transitions back from TRANSFER_SRC.
bool record_clear_copy(VkCommandBuffer cb, VkImage image, VkBuffer buf,
                       const VkClearColorValue& color, bool first) {
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    if (!check(vkBeginCommandBuffer(cb, &bi), "vkBeginCommandBuffer")) {
        return false;
    }
    image_barrier(cb, image,
                  first ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, first ? 0 : VK_ACCESS_TRANSFER_READ_BIT,
                  VK_ACCESS_TRANSFER_WRITE_BIT);
    VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cb, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &range);
    image_barrier(cb, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                  VK_ACCESS_TRANSFER_READ_BIT);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {kDim, kDim, 1};
    vkCmdCopyImageToBuffer(cb, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &region);
    return check(vkEndCommandBuffer(cb), "vkEndCommandBuffer");
}

// Submits `cb` with NO fence and NO semaphores: the readback intentionally carries no completion
// proof of its own -- the point of the canary.
bool submit_unproven(VkQueue queue, VkCommandBuffer cb) {
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    return check(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE), "vkQueueSubmit (unproven copy)");
}

// Maps `mem` and checks the first pixel equals rgba (tight 16x16x4 buffer, pixel 0 suffices for a
// full-image clear). Prints the greppable line either way.
bool map_and_check(VkDevice device, VkDeviceMemory mem, const unsigned char (&rgba)[4],
                   const char* phase) {
    void* p = nullptr;
    if (!check(vkMapMemory(device, mem, 0, VK_WHOLE_SIZE, 0, &p), "vkMapMemory")) {
        return false;
    }
    unsigned char px[4];
    std::memcpy(px, p, 4);
    vkUnmapMemory(device, mem);
    std::printf("ORDER-CANARY: %s pixel=%02x%02x%02x%02x expected=%02x%02x%02x%02x\n", phase, px[0],
                px[1], px[2], px[3], rgba[0], rgba[1], rgba[2], rgba[3]);
    return std::memcmp(px, rgba, 4) == 0;
}

} // namespace

int main() {
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.apiVersion = VK_API_VERSION_1_2; // timeline semaphores are core 1.2
    app.pApplicationName = "vkrelay2-readback-order-canary";
    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    VkInstance instance = VK_NULL_HANDLE;
    if (vkCreateInstance(&ici, nullptr, &instance) != VK_SUCCESS) {
        std::printf("ORDER-CANARY: SKIP (vkCreateInstance failed -- no ICD/worker?)\n");
        return 0;
    }
    std::uint32_t pd_count = 0;
    vkEnumeratePhysicalDevices(instance, &pd_count, nullptr);
    if (pd_count == 0) {
        std::printf("ORDER-CANARY: FAIL (no physical device)\n");
        return 2;
    }
    std::vector<VkPhysicalDevice> pds(pd_count);
    vkEnumeratePhysicalDevices(instance, &pd_count, pds.data());
    VkPhysicalDevice phys = pds[0];

    // Device: one graphics queue (family 0), timeline-semaphore feature requested faithfully.
    const float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = 0;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkPhysicalDeviceTimelineSemaphoreFeatures tsf{};
    tsf.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;
    tsf.timelineSemaphore = VK_TRUE;
    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.pNext = &tsf;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    VkDevice device = VK_NULL_HANDLE;
    if (!check(vkCreateDevice(phys, &dci, nullptr, &device), "vkCreateDevice")) {
        return 2;
    }
    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(device, 0, 0, &queue);

    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);

    // The clear/copy source image (device-local; the relay admits DEVICE_LOCAL-only or
    // HOST_VISIBLE|HOST_COHERENT memory classes -- prefer the former, fall back to the latter).
    VkImageCreateInfo imgci{};
    imgci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imgci.imageType = VK_IMAGE_TYPE_2D;
    imgci.format = VK_FORMAT_R8G8B8A8_UNORM;
    imgci.extent = {kDim, kDim, 1};
    imgci.mipLevels = 1;
    imgci.arrayLayers = 1;
    imgci.samples = VK_SAMPLE_COUNT_1_BIT;
    imgci.tiling = VK_IMAGE_TILING_OPTIMAL;
    imgci.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    imgci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imgci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage image = VK_NULL_HANDLE;
    if (!check(vkCreateImage(device, &imgci, nullptr, &image), "vkCreateImage")) {
        return 2;
    }
    VkMemoryRequirements imr{};
    vkGetImageMemoryRequirements(device, image, &imr);
    int img_type = find_memory_type(mp, imr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    if (img_type < 0) {
        img_type = find_memory_type(
            mp, imr.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0);
    }
    if (img_type < 0) {
        std::printf("ORDER-CANARY: FAIL (no admissible image memory type)\n");
        return 2;
    }
    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = imr.size;
    mai.memoryTypeIndex = static_cast<std::uint32_t>(img_type);
    VkDeviceMemory img_mem = VK_NULL_HANDLE;
    if (!check(vkAllocateMemory(device, &mai, nullptr, &img_mem), "vkAllocateMemory (image)") ||
        !check(vkBindImageMemory(device, image, img_mem, 0), "vkBindImageMemory")) {
        return 2;
    }

    // Two host-visible readback destination buffers, one per phase.
    VkBuffer bufs[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkDeviceMemory buf_mems[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    for (int i = 0; i < 2; ++i) {
        VkBufferCreateInfo bci{};
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size = kBufBytes;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (!check(vkCreateBuffer(device, &bci, nullptr, &bufs[i]), "vkCreateBuffer")) {
            return 2;
        }
        VkMemoryRequirements bmr{};
        vkGetBufferMemoryRequirements(device, bufs[i], &bmr);
        const int bt = find_memory_type(
            mp, bmr.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0);
        if (bt < 0) {
            std::printf("ORDER-CANARY: FAIL (no host-visible buffer memory type)\n");
            return 2;
        }
        VkMemoryAllocateInfo bai{};
        bai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        bai.allocationSize = bmr.size;
        bai.memoryTypeIndex = static_cast<std::uint32_t>(bt);
        if (!check(vkAllocateMemory(device, &bai, nullptr, &buf_mems[i]),
                   "vkAllocateMemory (buffer)")) {
            return 2;
        }
        if (i == 0) {
            if (!check(vkBindBufferMemory(device, bufs[i], buf_mems[i], 0), "vkBindBufferMemory")) {
                return 2;
            }
        } else {
            VkBindBufferMemoryInfo bind{};
            bind.sType = VK_STRUCTURE_TYPE_BIND_BUFFER_MEMORY_INFO;
            bind.buffer = bufs[i];
            bind.memory = buf_mems[i];
            if (!check(vkBindBufferMemory2(device, 1, &bind), "vkBindBufferMemory2")) {
                return 2;
            }
        }
    }

    VkCommandPoolCreateInfo cpi{};
    cpi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpi.queueFamilyIndex = 0;
    VkCommandPool pool = VK_NULL_HANDLE;
    if (!check(vkCreateCommandPool(device, &cpi, nullptr, &pool), "vkCreateCommandPool")) {
        return 2;
    }
    VkCommandBufferAllocateInfo cbai{};
    cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool = pool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 2;
    VkCommandBuffer cbs[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    if (!check(vkAllocateCommandBuffers(device, &cbai, cbs), "vkAllocateCommandBuffers")) {
        return 2;
    }

    // Phase A -- fence proof. Clear (0.8, 0.2, 0.6, 1.0) -> RGBA8 cc3399ff; the copy submit
    // carries NO proof; an EMPTY fence-only submit follows; waiting THAT fence must promote +
    // download the earlier copy (queue order).
    const VkClearColorValue color_a{{0.8f, 0.2f, 0.6f, 1.0f}};
    const unsigned char rgba_a[4] = {0xCC, 0x33, 0x99, 0xFF};
    if (!record_clear_copy(cbs[0], image, bufs[0], color_a, /*first=*/true) ||
        !submit_unproven(queue, cbs[0])) {
        return 2;
    }
    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence = VK_NULL_HANDLE;
    if (!check(vkCreateFence(device, &fci, nullptr, &fence), "vkCreateFence") ||
        !check(vkQueueSubmit(queue, 0, nullptr, fence), "vkQueueSubmit (fence-only)") ||
        !check(vkWaitForFences(device, 1, &fence, VK_TRUE, 5000000000ull), "vkWaitForFences")) {
        return 2;
    }
    const bool fence_ok = map_and_check(device, buf_mems[0], rgba_a, "fence-proof");

    // Phase B -- timeline proof. Clear (0.2, 0.6, 0.8, 1.0) -> 3399ccff; the copy submit again
    // unproven; an EMPTY batch signals timeline value 1; waiting that value must promote it.
    const VkClearColorValue color_b{{0.2f, 0.6f, 0.8f, 1.0f}};
    const unsigned char rgba_b[4] = {0x33, 0x99, 0xCC, 0xFF};
    if (!record_clear_copy(cbs[1], image, bufs[1], color_b, /*first=*/false) ||
        !submit_unproven(queue, cbs[1])) {
        return 2;
    }
    VkSemaphoreTypeCreateInfo sti{};
    sti.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
    sti.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    sti.initialValue = 0;
    VkSemaphoreCreateInfo sci{};
    sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    sci.pNext = &sti;
    VkSemaphore timeline = VK_NULL_HANDLE;
    if (!check(vkCreateSemaphore(device, &sci, nullptr, &timeline), "vkCreateSemaphore")) {
        return 2;
    }
    const std::uint64_t signal_value = 1;
    VkTimelineSemaphoreSubmitInfo tsi{};
    tsi.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
    tsi.signalSemaphoreValueCount = 1;
    tsi.pSignalSemaphoreValues = &signal_value;
    VkSubmitInfo empty{};
    empty.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    empty.pNext = &tsi;
    empty.signalSemaphoreCount = 1;
    empty.pSignalSemaphores = &timeline;
    if (!check(vkQueueSubmit(queue, 1, &empty, VK_NULL_HANDLE),
               "vkQueueSubmit (timeline-signal only)")) {
        return 2;
    }
    VkSemaphoreWaitInfo swi{};
    swi.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
    swi.semaphoreCount = 1;
    swi.pSemaphores = &timeline;
    swi.pValues = &signal_value;
    if (!check(vkWaitSemaphores(device, &swi, 5000000000ull), "vkWaitSemaphores")) {
        return 2;
    }
    const bool timeline_ok = map_and_check(device, buf_mems[1], rgba_b, "timeline-proof");

    std::printf("ORDER-CANARY: %s\n",
                (fence_ok && timeline_ok)
                    ? "PASS"
                    : (fence_ok ? "FAIL (timeline-proof readback stale)"
                                : (timeline_ok ? "FAIL (fence-proof readback stale)"
                                               : "FAIL (both readbacks stale)")));

    vkDeviceWaitIdle(device);
    vkDestroySemaphore(device, timeline, nullptr);
    vkDestroyFence(device, fence, nullptr);
    vkFreeCommandBuffers(device, pool, 2, cbs);
    vkDestroyCommandPool(device, pool, nullptr);
    for (int i = 0; i < 2; ++i) {
        vkDestroyBuffer(device, bufs[i], nullptr);
        vkFreeMemory(device, buf_mems[i], nullptr);
    }
    vkDestroyImage(device, image, nullptr);
    vkFreeMemory(device, img_mem, nullptr);
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);
    return (fence_ok && timeline_ok) ? 0 : 1;
}
