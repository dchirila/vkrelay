// vkrelay2 hostQueryReset canary (Vulkan 1.3 required-feature audit): the DEVICE-level
// vkResetQueryPool, end to end on the REAL backend through the loader + the session-pinned ICD.
//
// hostQueryReset is REQUIRED since Vulkan 1.2, but before this fix the relay wired only the
// COMMAND-buffer vkCmdResetQueryPool -- the device-level vkResetQueryPool (reset a query pool FROM
// THE HOST, no command buffer, which is what the feature actually is) was not relayed, so an app
// that queried hostQueryReset=TRUE and called vkResetQueryPool hit a null/abort. This canary proves
// the host-side reset is now served:
//   - hostQueryReset reports TRUE and a device enables it (via the forwarded f12 rollup);
//   - vkGetDeviceProcAddr resolves "vkResetQueryPool" (it did NOT before this fix -> RED);
//   - a HOST reset actually clears a query's availability: a timestamp is written to query 0 (so it
//     becomes AVAILABLE), then a host-side vkResetQueryPool makes it UNAVAILABLE again -- the
//     toggle available -> unavailable is driven ONLY by the host reset, so it proves the real call
//     reached the host GPU (mock == real range validation behind it).
//
// hostQueryReset is core 1.2, so this works on EITHER lane (the required-feature audit's honest
// gate currently keeps the native lane at 1.2 until multiview lands; the feature is served
// regardless). REAL backend required (a real timestamp + a real host reset); SKIPs cleanly
// otherwise.

#include <vulkan/vulkan.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {
constexpr std::uint32_t kQueryCount = 2;

// vkGetQueryPoolResults for query 0 with 64-bit + AVAILABILITY (no WAIT): res[0] = value, res[1] =
// availability word. Returns the availability (nonzero = available); *ok reports the call ran.
std::uint64_t query0_availability(VkDevice device, VkQueryPool pool, bool* ok) {
    std::uint64_t res[2] = {0, 0};
    const VkResult r =
        vkGetQueryPoolResults(device, pool, 0, 1, sizeof(res), res, sizeof(res),
                              VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
    // SUCCESS (available) or NOT_READY (unavailable) are both faithful; anything else is a fault.
    *ok = (r == VK_SUCCESS || r == VK_NOT_READY);
    return res[1];
}
} // namespace

int main() {
    // Unbuffered stdout: the relay pipes our output, so a block-buffered stdout would LOSE every
    // line on an abort/crash (the exact "no output before exit 134" trap). Flush as we go.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.apiVersion = VK_API_VERSION_1_2; // hostQueryReset is core 1.2 -- works on either lane
    app.pApplicationName = "vkrelay2-vk13-hostquery-canary";
    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    VkInstance instance = VK_NULL_HANDLE;
    if (vkCreateInstance(&ici, nullptr, &instance) != VK_SUCCESS) {
        std::printf("VK13-HOSTQUERY-CANARY: SKIP (vkCreateInstance failed -- no ICD/worker?)\n");
        return 0;
    }

    std::uint32_t pd_count = 0;
    vkEnumeratePhysicalDevices(instance, &pd_count, nullptr);
    if (pd_count == 0) {
        std::printf("VK13-HOSTQUERY-CANARY: FAIL (no physical device)\n");
        vkDestroyInstance(instance, nullptr);
        return 2;
    }
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    pd_count = 1;
    vkEnumeratePhysicalDevices(instance, &pd_count, &phys);

    // hostQueryReset must be reported TRUE (it is a required feature; unmasked f12 pass-through).
    VkPhysicalDeviceVulkan12Features f12{};
    f12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    VkPhysicalDeviceFeatures2 feat2{};
    feat2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    feat2.pNext = &f12;
    vkGetPhysicalDeviceFeatures2(phys, &feat2);
    std::printf("VK13-HOSTQUERY-CANARY: hostQueryReset=%d\n", f12.hostQueryReset ? 1 : 0);
    if (!f12.hostQueryReset) {
        std::printf("VK13-HOSTQUERY-CANARY: FAIL (hostQueryReset not reported -- a required 1.2 "
                    "feature)\n");
        vkDestroyInstance(instance, nullptr);
        return 1;
    }

    // Family 0 must be graphics-capable with valid timestamps (vkCmdWriteTimestamp).
    std::uint32_t qf_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &qf_count, nullptr);
    if (qf_count == 0) {
        std::printf("VK13-HOSTQUERY-CANARY: FAIL (no queue families)\n");
        vkDestroyInstance(instance, nullptr);
        return 1;
    }
    VkQueueFamilyProperties qf0{};
    std::uint32_t one = 1;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &one, &qf0);
    if ((qf0.queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0 || qf0.timestampValidBits == 0) {
        std::printf("VK13-HOSTQUERY-CANARY: SKIP (family 0 lacks graphics or timestamps)\n");
        vkDestroyInstance(instance, nullptr);
        return 0;
    }

    // Device with hostQueryReset ENABLED via the forwarded f12 rollup (the ICD captures the f12
    // struct + the worker enables it on the host device -- like timelineSemaphore/imagelessFB).
    const float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = 0;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkPhysicalDeviceVulkan12Features enable12{};
    enable12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    enable12.hostQueryReset = VK_TRUE;
    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.pNext = &enable12;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    VkDevice device = VK_NULL_HANDLE;
    if (vkCreateDevice(phys, &dci, nullptr, &device) != VK_SUCCESS) {
        std::printf("VK13-HOSTQUERY-CANARY: FAIL (vkCreateDevice with hostQueryReset enabled)\n");
        vkDestroyInstance(instance, nullptr);
        return 1;
    }

    int rc = 1;
    VkQueue queue = VK_NULL_HANDLE;
    VkQueryPool pool = VK_NULL_HANDLE;
    VkCommandPool cpool = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    vkGetDeviceQueue(device, 0, 0, &queue);

    // The device-level entry point must RESOLVE (the RED->GREEN pivot: it returned null before this
    // fix wired vkResetQueryPool).
    auto pfnReset =
        reinterpret_cast<PFN_vkResetQueryPool>(vkGetDeviceProcAddr(device, "vkResetQueryPool"));
    std::printf("VK13-HOSTQUERY-CANARY: reset_proc_resolves=%d\n", pfnReset != nullptr ? 1 : 0);
    if (pfnReset == nullptr) {
        std::printf("VK13-HOSTQUERY-CANARY: FAIL (vkResetQueryPool did not resolve -- the "
                    "device-level host reset is not wired)\n");
        goto teardown;
    }

    {
        VkQueryPoolCreateInfo qpci{};
        qpci.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        qpci.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qpci.queryCount = kQueryCount;
        if (vkCreateQueryPool(device, &qpci, nullptr, &pool) != VK_SUCCESS) {
            std::printf("VK13-HOSTQUERY-CANARY: FAIL (vkCreateQueryPool TIMESTAMP)\n");
            goto teardown;
        }

        // HOST reset the whole pool, then query 0 must be UNAVAILABLE (no write yet).
        pfnReset(device, pool, 0, kQueryCount);
        bool ok0 = false;
        const std::uint64_t avail_before = query0_availability(device, pool, &ok0);
        if (!ok0) {
            std::printf("VK13-HOSTQUERY-CANARY: FAIL (results read after host reset faulted)\n");
            goto teardown;
        }

        // Write a timestamp to query 0 on the GPU, so it becomes AVAILABLE.
        VkCommandPoolCreateInfo cplci{};
        cplci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        cplci.queueFamilyIndex = 0;
        if (vkCreateCommandPool(device, &cplci, nullptr, &cpool) != VK_SUCCESS) {
            goto teardown;
        }
        VkCommandBufferAllocateInfo cbai{};
        cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cbai.commandPool = cpool;
        cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbai.commandBufferCount = 1;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        if (vkAllocateCommandBuffers(device, &cbai, &cmd) != VK_SUCCESS) {
            goto teardown;
        }
        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &bi);
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, pool, 0);
        vkEndCommandBuffer(cmd);
        VkFenceCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        if (vkCreateFence(device, &fci, nullptr, &fence) != VK_SUCCESS) {
            goto teardown;
        }
        VkSubmitInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        if (vkQueueSubmit(queue, 1, &si, fence) != VK_SUCCESS ||
            vkWaitForFences(device, 1, &fence, VK_TRUE, ~0ull) != VK_SUCCESS) {
            std::printf("VK13-HOSTQUERY-CANARY: FAIL (timestamp submit/wait)\n");
            goto teardown;
        }
        bool ok1 = false;
        const std::uint64_t avail_after_write = query0_availability(device, pool, &ok1);

        // The crux: a HOST-side reset must make query 0 UNAVAILABLE again (no command buffer).
        pfnReset(device, pool, 0, kQueryCount);
        bool ok2 = false;
        const std::uint64_t avail_after_reset = query0_availability(device, pool, &ok2);
        if (!ok1 || !ok2) {
            std::printf("VK13-HOSTQUERY-CANARY: FAIL (a results read faulted)\n");
            goto teardown;
        }
        std::printf(
            "VK13-HOSTQUERY-CANARY: avail before=%llu after_write=%llu after_host_reset=%llu\n",
            static_cast<unsigned long long>(avail_before),
            static_cast<unsigned long long>(avail_after_write),
            static_cast<unsigned long long>(avail_after_reset));
        if (avail_before == 0 && avail_after_write != 0 && avail_after_reset == 0) {
            std::printf(
                "VK13-HOSTQUERY-CANARY: PASS (device-level vkResetQueryPool served: a HOST "
                "reset toggled query availability available->unavailable on the real GPU)\n");
            rc = 0;
        } else {
            std::printf("VK13-HOSTQUERY-CANARY: FAIL (host reset did not clear availability: "
                        "before=%llu after_write=%llu after_reset=%llu -- want 0/nonzero/0)\n",
                        static_cast<unsigned long long>(avail_before),
                        static_cast<unsigned long long>(avail_after_write),
                        static_cast<unsigned long long>(avail_after_reset));
        }
    }

teardown:
    if (device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device);
    }
    if (fence != VK_NULL_HANDLE) {
        vkDestroyFence(device, fence, nullptr);
    }
    if (cpool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(device, cpool, nullptr);
    }
    if (pool != VK_NULL_HANDLE) {
        vkDestroyQueryPool(device, pool, nullptr);
    }
    if (device != VK_NULL_HANDLE) {
        vkDestroyDevice(device, nullptr);
    }
    vkDestroyInstance(instance, nullptr);
    if (rc == 0) {
        std::printf("VK13-HOSTQUERY-CANARY: done\n");
    }
    return rc;
}
