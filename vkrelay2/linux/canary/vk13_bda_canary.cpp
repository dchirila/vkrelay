// vkrelay2 Vulkan-1.2 bufferDeviceAddress canary -- the native lane and raw-GPU-pointer path.
//
// A surfaceless native Vulkan program runs through the real
// loader + the session-pinned vkrelay2 ICD on the NATIVE lane (run_vk13_bda_smoke.sh
// --frontend vulkan13 -> VKRELAY2_NATIVE_LANE=1). It proves, end to end on the real GPU, that the
// relay serves a raw VkDeviceAddress FAITHFULLY: the address vkGetBufferDeviceAddress returns is
// the real host VA, and the GPU honors work that dereferences it. Assertions:
//   - the device reports apiVersion >= 1.2 (the exact version is the Vulkan 1.3 canary's
//   assertion);
//   - the Vulkan12Features rollup + the standalone BufferDeviceAddressFeatures struct report
//     bufferDeviceAddress=1 on the native lane, and captureReplay=0 + multiDevice=0 EVERYWHERE
//     (unwired -- fail-closed by name);
//   - memory allocated WITH VkMemoryAllocateFlagsInfo(DEVICE_ADDRESS) binds a
//     SHADER_DEVICE_ADDRESS buffer (the serialized-pNext contract, VUID 03339);
//   - vkGetBufferDeviceAddress returns a NON-ZERO address;
//   - a compute shader that reaches its buffer ONLY through that raw address (push-constant
//     GL_EXT_buffer_reference -- no descriptor set anywhere) reads AND writes through it:
//     data[i] = data[i]*scale + bias + i, copied out and asserted byte-exact. A faked/translated
//     address cannot pass this.
//
// Greppable markers on stdout; run_vk13_bda_smoke.sh gates on them. Skips cleanly (exit 0) when
// no ICD/worker stack is reachable; FAILs (nonzero) on a real regression.

#include <vulkan/vulkan.h>

#include "tests/bda_spv.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>

namespace {

constexpr std::uint32_t kElems = 4096; // / local_size 64 = 64 groups
constexpr std::uint32_t kScale = 3;
constexpr std::uint32_t kBias = 7;

// The push-constant block the shader declares: the raw device address + scale + bias.
struct PushBlock {
    VkDeviceAddress addr;
    std::uint32_t scale;
    std::uint32_t bias;
};
static_assert(sizeof(PushBlock) == 16, "push block must match the shader's std430 layout");

} // namespace

int main() {
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.apiVersion = VK_API_VERSION_1_2; // bufferDeviceAddress is core 1.2
    app.pApplicationName = "vkrelay2-vk13-bda-canary";
    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    VkInstance instance = VK_NULL_HANDLE;
    if (vkCreateInstance(&ici, nullptr, &instance) != VK_SUCCESS) {
        std::printf("VK13-BDA-CANARY: SKIP (vkCreateInstance failed -- no ICD/worker?)\n");
        return 0;
    }

    std::uint32_t pd_count = 0;
    vkEnumeratePhysicalDevices(instance, &pd_count, nullptr);
    if (pd_count == 0) {
        std::printf("VK13-BDA-CANARY: FAIL (no physical device)\n");
        vkDestroyInstance(instance, nullptr);
        return 2;
    }
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    pd_count = 1;
    vkEnumeratePhysicalDevices(instance, &pd_count, &phys);

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(phys, &props);
    const std::uint32_t maj = VK_API_VERSION_MAJOR(props.apiVersion);
    const std::uint32_t min = VK_API_VERSION_MINOR(props.apiVersion);
    std::printf("VK13-BDA-CANARY: device '%s' apiVersion=%u.%u\n", props.deviceName, maj, min);
    // bufferDeviceAddress is core 1.2: this canary needs AT LEAST 1.2. The exact reported
    // version is the Vulkan 1.3 canary's assertion (the native lane reports 1.3 once the worker
    // vouches vk13_ready); this canary's contract is the BDA feature surface, version-agnostic.
    if (!(maj == 1 && min >= 2)) {
        std::printf("VK13-BDA-CANARY: FAIL (device apiVersion %u.%u, expected >= 1.2)\n", maj, min);
        vkDestroyInstance(instance, nullptr);
        return 1;
    }

    // --- Feature exposure: the 1.2 rollup AND the standalone struct must agree. ---
    VkPhysicalDeviceVulkan12Features f12{};
    f12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    VkPhysicalDeviceFeatures2 feat2{};
    feat2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    feat2.pNext = &f12;
    vkGetPhysicalDeviceFeatures2(phys, &feat2);
    VkPhysicalDeviceBufferDeviceAddressFeatures bda_feat{};
    bda_feat.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES;
    VkPhysicalDeviceFeatures2 feat2b{};
    feat2b.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    feat2b.pNext = &bda_feat;
    vkGetPhysicalDeviceFeatures2(phys, &feat2b);
    std::printf("VK13-BDA-CANARY: feature bufferDeviceAddress=%d captureReplay=%d multiDevice=%d "
                "(standalone %d/%d/%d)\n",
                f12.bufferDeviceAddress ? 1 : 0, f12.bufferDeviceAddressCaptureReplay ? 1 : 0,
                f12.bufferDeviceAddressMultiDevice ? 1 : 0, bda_feat.bufferDeviceAddress ? 1 : 0,
                bda_feat.bufferDeviceAddressCaptureReplay ? 1 : 0,
                bda_feat.bufferDeviceAddressMultiDevice ? 1 : 0);
    if (f12.bufferDeviceAddressCaptureReplay || f12.bufferDeviceAddressMultiDevice ||
        bda_feat.bufferDeviceAddressCaptureReplay || bda_feat.bufferDeviceAddressMultiDevice) {
        std::printf("VK13-BDA-CANARY: FAIL (captureReplay/multiDevice leaked TRUE -- both are "
                    "unwired and must be masked)\n");
        vkDestroyInstance(instance, nullptr);
        return 1;
    }
    if (!f12.bufferDeviceAddress || !bda_feat.bufferDeviceAddress) {
        std::printf("VK13-BDA-CANARY: FAIL (bufferDeviceAddress reported FALSE -- on the native "
                    "lane it must be TRUE; on the default lane this FAIL is the expected "
                    "steering proof)\n");
        vkDestroyInstance(instance, nullptr);
        return 1;
    }

    std::uint32_t qf_count = 1;
    VkQueueFamilyProperties qf{};
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &qf_count, &qf);
    if ((qf.queueFlags & VK_QUEUE_COMPUTE_BIT) == 0) {
        std::printf("VK13-BDA-CANARY: SKIP (family 0 has no COMPUTE)\n");
        vkDestroyInstance(instance, nullptr);
        return 0;
    }

    int rc = 1;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    VkShaderModule module = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkCommandPool cpool = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkBuffer buf_bda = VK_NULL_HANDLE, buf_read = VK_NULL_HANDLE;
    VkDeviceMemory mem_bda = VK_NULL_HANDLE, mem_read = VK_NULL_HANDLE;

    {
        // --- Device with the bufferDeviceAddress FEATURE enabled (standalone struct; no
        // extension -- core 1.2). ---
        const float prio = 1.0f;
        VkDeviceQueueCreateInfo qci{};
        qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qci.queueFamilyIndex = 0;
        qci.queueCount = 1;
        qci.pQueuePriorities = &prio;
        VkPhysicalDeviceBufferDeviceAddressFeatures enable_bda{};
        enable_bda.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES;
        enable_bda.bufferDeviceAddress = VK_TRUE;
        VkDeviceCreateInfo dci{};
        dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        dci.pNext = &enable_bda;
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qci;
        if (vkCreateDevice(phys, &dci, nullptr, &device) != VK_SUCCESS) {
            std::printf("VK13-BDA-CANARY: FAIL (vkCreateDevice with bufferDeviceAddress)\n");
            goto teardown;
        }
        vkGetDeviceQueue(device, 0, 0, &queue);

        VkPhysicalDeviceMemoryProperties mp{};
        vkGetPhysicalDeviceMemoryProperties(phys, &mp);
        const VkMemoryPropertyFlags want =
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        auto pick = [&](std::uint32_t bits) {
            for (std::uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
                if ((bits & (1u << i)) != 0 && (mp.memoryTypes[i].propertyFlags & want) == want) {
                    return i;
                }
            }
            return UINT32_MAX;
        };
        const VkDeviceSize bytes = kElems * sizeof(std::uint32_t);

        // The BDA buffer: SHADER_DEVICE_ADDRESS usage + memory allocated WITH the
        // DEVICE_ADDRESS flag (the serialized VkMemoryAllocateFlagsInfo -- VUID 03339).
        {
            VkBufferCreateInfo bci{};
            bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            bci.size = bytes;
            bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                        VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
            bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            if (vkCreateBuffer(device, &bci, nullptr, &buf_bda) != VK_SUCCESS) {
                std::printf("VK13-BDA-CANARY: FAIL (create SHADER_DEVICE_ADDRESS buffer)\n");
                goto teardown;
            }
            VkMemoryRequirements mr{};
            vkGetBufferMemoryRequirements(device, buf_bda, &mr);
            const std::uint32_t type = pick(mr.memoryTypeBits);
            if (type == UINT32_MAX) {
                std::printf("VK13-BDA-CANARY: FAIL (no mappable memory type for the BDA "
                            "buffer)\n");
                goto teardown;
            }
            VkMemoryAllocateFlagsInfo mafi{};
            mafi.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
            mafi.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
            VkMemoryAllocateInfo mai{};
            mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            mai.pNext = &mafi;
            mai.allocationSize = mr.size;
            mai.memoryTypeIndex = type;
            if (vkAllocateMemory(device, &mai, nullptr, &mem_bda) != VK_SUCCESS) {
                std::printf("VK13-BDA-CANARY: FAIL (allocate with "
                            "VkMemoryAllocateFlagsInfo(DEVICE_ADDRESS))\n");
                goto teardown;
            }
            if (vkBindBufferMemory(device, buf_bda, mem_bda, 0) != VK_SUCCESS) {
                std::printf("VK13-BDA-CANARY: FAIL (bind the SHADER_DEVICE_ADDRESS buffer)\n");
                goto teardown;
            }
        }
        // The readback buffer (plain TRANSFER_DST, no flags).
        {
            VkBufferCreateInfo bci{};
            bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            bci.size = bytes;
            bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            if (vkCreateBuffer(device, &bci, nullptr, &buf_read) != VK_SUCCESS) {
                goto teardown;
            }
            VkMemoryRequirements mr{};
            vkGetBufferMemoryRequirements(device, buf_read, &mr);
            const std::uint32_t type = pick(mr.memoryTypeBits);
            if (type == UINT32_MAX) {
                goto teardown;
            }
            VkMemoryAllocateInfo mai{};
            mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            mai.allocationSize = mr.size;
            mai.memoryTypeIndex = type;
            if (vkAllocateMemory(device, &mai, nullptr, &mem_read) != VK_SUCCESS ||
                vkBindBufferMemory(device, buf_read, mem_read, 0) != VK_SUCCESS) {
                goto teardown;
            }
        }

        void* p = nullptr;
        if (vkMapMemory(device, mem_bda, 0, VK_WHOLE_SIZE, 0, &p) != VK_SUCCESS) {
            goto teardown;
        }
        for (std::uint32_t i = 0; i < kElems; ++i) {
            static_cast<std::uint32_t*>(p)[i] = i * 5 + 1;
        }
        vkUnmapMemory(device, mem_bda);
        void* read_ptr = nullptr;
        if (vkMapMemory(device, mem_read, 0, VK_WHOLE_SIZE, 0, &read_ptr) != VK_SUCCESS) {
            goto teardown;
        }
        std::memset(read_ptr, 0, bytes); // poison: stale zeros must NOT pass the assert

        // --- THE address: vkGetBufferDeviceAddress must return non-zero. ---
        VkBufferDeviceAddressInfo bdai{};
        bdai.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
        bdai.buffer = buf_bda;
        const VkDeviceAddress addr = vkGetBufferDeviceAddress(device, &bdai);
        std::printf("VK13-BDA-CANARY: device_address_nonzero=%d\n", addr != 0 ? 1 : 0);
        if (addr == 0) {
            std::printf("VK13-BDA-CANARY: FAIL (vkGetBufferDeviceAddress returned 0)\n");
            goto teardown;
        }

        VkShaderModuleCreateInfo smci{};
        smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        smci.codeSize = sizeof(kBdaSpv);
        smci.pCode = kBdaSpv;
        if (vkCreateShaderModule(device, &smci, nullptr, &module) != VK_SUCCESS) {
            std::printf("VK13-BDA-CANARY: FAIL (buffer-reference shader module)\n");
            goto teardown;
        }
        // NO descriptor set layout anywhere: the buffer is reachable only via the raw address.
        VkPushConstantRange push{};
        push.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        push.offset = 0;
        push.size = sizeof(PushBlock);
        VkPipelineLayoutCreateInfo plci{};
        plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        plci.pushConstantRangeCount = 1;
        plci.pPushConstantRanges = &push;
        if (vkCreatePipelineLayout(device, &plci, nullptr, &layout) != VK_SUCCESS) {
            goto teardown;
        }
        VkComputePipelineCreateInfo cpci{};
        cpci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        cpci.stage.module = module;
        cpci.stage.pName = "main";
        cpci.layout = layout;
        cpci.basePipelineIndex = -1;
        if (vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpci, nullptr, &pipeline) !=
            VK_SUCCESS) {
            std::printf("VK13-BDA-CANARY: FAIL (compute pipeline with the buffer-reference "
                        "shader)\n");
            goto teardown;
        }

        VkCommandPoolCreateInfo cpci2{};
        cpci2.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        cpci2.queueFamilyIndex = 0;
        if (vkCreateCommandPool(device, &cpci2, nullptr, &cpool) != VK_SUCCESS) {
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
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        PushBlock pb{};
        pb.addr = addr;
        pb.scale = kScale;
        pb.bias = kBias;
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pb), &pb);
        vkCmdDispatch(cmd, kElems / 64, 1, 1);
        // Core-1.0 barrier (this canary is about BDA, not sync2): order the through-the-address
        // shader writes before the copy reads.
        VkMemoryBarrier mb{};
        mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
        VkBufferCopy region{0, 0, bytes};
        vkCmdCopyBuffer(cmd, buf_bda, buf_read, 1, &region);
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
        if (vkQueueSubmit(queue, 1, &si, fence) != VK_SUCCESS) {
            std::printf("VK13-BDA-CANARY: FAIL (vkQueueSubmit)\n");
            goto teardown;
        }
        if (vkWaitForFences(device, 1, &fence, VK_TRUE, ~0ull) != VK_SUCCESS) {
            std::printf("VK13-BDA-CANARY: FAIL (vkWaitForFences)\n");
            goto teardown;
        }

        // --- The through-the-address readback: byte-exact data[i] = (i*5+1)*scale + bias + i. ---
        const auto* out_words = static_cast<const std::uint32_t*>(read_ptr);
        std::uint32_t bad = 0;
        for (std::uint32_t i = 0; i < kElems; ++i) {
            const std::uint32_t want_v = (i * 5 + 1) * kScale + kBias + i;
            if (out_words[i] != want_v) {
                if (bad < 4) {
                    std::printf("VK13-BDA-CANARY: mismatch[%u] got=%u want=%u\n", i, out_words[i],
                                want_v);
                }
                ++bad;
            }
        }
        if (bad != 0) {
            std::printf("VK13-BDA-CANARY: FAIL (%u/%u values wrong -- the GPU did not honor the "
                        "relayed device address)\n",
                        bad, kElems);
            goto teardown;
        }
        std::printf("VK13-BDA-CANARY: compute-through-raw-address byte-exact (%u values)\n",
                    kElems);
        std::printf("VK13-BDA-CANARY: PASS (native lane serves bufferDeviceAddress: the "
                    "DEVICE_ADDRESS allocation binds, vkGetBufferDeviceAddress is non-zero, and a "
                    "descriptorless buffer-reference compute shader read AND wrote through the "
                    "raw address on the real GPU)\n");
        rc = 0;
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
    if (pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(device, pipeline, nullptr);
    }
    if (layout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device, layout, nullptr);
    }
    if (module != VK_NULL_HANDLE) {
        vkDestroyShaderModule(device, module, nullptr);
    }
    for (VkBuffer b : {buf_bda, buf_read}) {
        if (b != VK_NULL_HANDLE) {
            vkDestroyBuffer(device, b, nullptr);
        }
    }
    for (VkDeviceMemory m : {mem_bda, mem_read}) {
        if (m != VK_NULL_HANDLE) {
            vkFreeMemory(device, m, nullptr);
        }
    }
    if (device != VK_NULL_HANDLE) {
        vkDestroyDevice(device, nullptr);
    }
    vkDestroyInstance(instance, nullptr);
    if (rc == 0) {
        std::printf("VK13-BDA-CANARY: done\n");
    }
    return rc;
}
