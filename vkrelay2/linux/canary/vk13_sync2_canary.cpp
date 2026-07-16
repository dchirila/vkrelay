// vkrelay2 Vulkan-1.3 synchronization2 canary -- the native lane + VK_KHR_synchronization2.
//
// This native-Vulkan canary runs a surfaceless Vulkan program
// through the real loader + the session-pinned vkrelay2 ICD on the NATIVE lane
// (run_vk13_sync2_smoke.sh --frontend vulkan13 -> VKRELAY2_NATIVE_LANE=1). It proves, end to end on
// the real GPU, that the steering-safe native lane serves the whole VK_KHR_synchronization2 command
// family with FAITHFUL 64-bit masks. Assertions:
//   - the device reports apiVersion >= 1.2 (sync2 rode as an EXTENSION on the 1.2 device; the
//     required-feature audit then flipped the native lane to a conformant 1.3 on
//     a 1.3-capable host -- where sync2 is CORE, still reachable via its KHR alias);
//   - VK_KHR_synchronization2 is PRESENT + its feature TRUE + the Vulkan-1.3 rollup reports
//     synchronization2=1 (dynamicRendering=1 too; maintenance4 is version-adaptive: FALSE at 1.2,
//     TRUE once Vulkan 1.3 support serves the full required matrix);
//   - a COMPUTE read-after-write ACROSS vkCmdPipelineBarrier2: a dispatch writes an SSBO, a GLOBAL
//     VkMemoryBarrier2 (SHADER_WRITE -> TRANSFER_READ, 64-bit masks) orders it, a copy reads it
//     into a mapped readback buffer; every value must be byte-exact (a broken barrier2 ->
//     wrong/zero bytes). This is the load-bearing ordering proof (transfer-only barriers can pass
//     on luck).
//   - vkCmdWriteTimestamp2 records a timestamp query (asserted written when the queue reports
//     timestampValidBits > 0);
//   - vkCmdSetEvent2 on a VkEvent, submitted via vkQueueSubmit2, then vkGetEventStatus ==
//   VK_EVENT_SET
//     (the sync2 event command + the sync2 submit path both reached the host).
//   - the WHOLE thing is submitted via vkQueueSubmit2 (one VkSubmitInfo2 + fence).
//
// Greppable markers on stdout; run_vk13_sync2_smoke.sh gates on them. Skips cleanly (exit 0) when
// no ICD/worker stack is reachable; FAILs (nonzero) on a real regression.

#include <vulkan/vulkan.h>

#include "tests/compute_spv.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

constexpr std::uint32_t kElems = 4096; // / local_size 64 = 64 groups
constexpr std::uint32_t kScale = 3;
constexpr std::uint32_t kBias = 7;

bool has_ext(const std::vector<VkExtensionProperties>& exts, const char* name) {
    for (const auto& e : exts) {
        if (std::strcmp(e.extensionName, name) == 0) {
            return true;
        }
    }
    return false;
}

} // namespace

int main() {
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.apiVersion =
        VK_API_VERSION_1_2; // sync2 rides as an EXTENSION; the 1.3 bump is Vulkan 1.3 support
    app.pApplicationName = "vkrelay2-vk13-sync2-canary";
    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    VkInstance instance = VK_NULL_HANDLE;
    if (vkCreateInstance(&ici, nullptr, &instance) != VK_SUCCESS) {
        std::printf("VK13-SYNC2-CANARY: SKIP (vkCreateInstance failed -- no ICD/worker?)\n");
        return 0;
    }

    std::uint32_t pd_count = 0;
    vkEnumeratePhysicalDevices(instance, &pd_count, nullptr);
    if (pd_count == 0) {
        std::printf("VK13-SYNC2-CANARY: FAIL (no physical device)\n");
        vkDestroyInstance(instance, nullptr);
        return 2;
    }
    std::vector<VkPhysicalDevice> pds(pd_count);
    vkEnumeratePhysicalDevices(instance, &pd_count, pds.data());
    const VkPhysicalDevice phys = pds[0];

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(phys, &props);
    const std::uint32_t maj = VK_API_VERSION_MAJOR(props.apiVersion);
    const std::uint32_t min = VK_API_VERSION_MINOR(props.apiVersion);
    std::printf("VK13-SYNC2-CANARY: device '%s' apiVersion=%u.%u\n", props.deviceName, maj, min);
    // sync2 is available on the native lane. It rode as an EXTENSION on a 1.2 device through its
    // bring-up; the required-feature audit then flipped the native lane to a conformant
    // 1.3 (multiview served), so the device now honestly reports 1.3 on a 1.3-capable host -- where
    // sync2 is CORE (still reachable via its KHR alias, which this canary exercises). Accept
    // >= 1.2; full13 is the full Vulkan 1.3 state that also serves maintenance4 below.
    const bool full13 = (maj > 1) || (maj == 1 && min >= 3);
    if (!(maj == 1 && min >= 2)) {
        std::printf("VK13-SYNC2-CANARY: FAIL (device apiVersion %u.%u, expected >= 1.2 for the "
                    "sync2 surface)\n",
                    maj, min);
        vkDestroyInstance(instance, nullptr);
        return 1;
    }

    // --- Extension + feature + rollup exposure on the native lane. ---
    std::uint32_t ext_count = 0;
    vkEnumerateDeviceExtensionProperties(phys, nullptr, &ext_count, nullptr);
    std::vector<VkExtensionProperties> exts(ext_count);
    vkEnumerateDeviceExtensionProperties(phys, nullptr, &ext_count, exts.data());
    const bool sync2_ext = has_ext(exts, VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME);
    std::printf("VK13-SYNC2-CANARY: extensions synchronization2=%d\n", sync2_ext ? 1 : 0);
    if (!sync2_ext) {
        std::printf("VK13-SYNC2-CANARY: FAIL (VK_KHR_synchronization2 not advertised on the native "
                    "lane)\n");
        vkDestroyInstance(instance, nullptr);
        return 1;
    }
    VkPhysicalDeviceSynchronization2Features s2_feat{};
    s2_feat.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES;
    VkPhysicalDeviceFeatures2 feat2{};
    feat2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    feat2.pNext = &s2_feat;
    vkGetPhysicalDeviceFeatures2(phys, &feat2);
    std::printf("VK13-SYNC2-CANARY: synchronization2 feature=%d\n",
                s2_feat.synchronization2 ? 1 : 0);
    if (!s2_feat.synchronization2) {
        std::printf(
            "VK13-SYNC2-CANARY: FAIL (synchronization2 reported FALSE on the native lane)\n");
        vkDestroyInstance(instance, nullptr);
        return 1;
    }
    VkPhysicalDeviceVulkan13Features feat13{};
    feat13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    VkPhysicalDeviceFeatures2 feat2b{};
    feat2b.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    feat2b.pNext = &feat13;
    vkGetPhysicalDeviceFeatures2(phys, &feat2b);
    std::printf("VK13-SYNC2-CANARY: rollup synchronization2=%d dynamicRendering=%d maintenance4=%d "
                "(full13=%d)\n",
                feat13.synchronization2 ? 1 : 0, feat13.dynamicRendering ? 1 : 0,
                feat13.maintenance4 ? 1 : 0, full13 ? 1 : 0);
    // maintenance4 is version-adaptive: a REQUIRED 1.3 feature Vulkan 1.3 support serves exactly
    // when the native lane vouches 1.3 -- FALSE while it honestly reports 1.2, TRUE at 1.3.
    if (!feat13.synchronization2 || (feat13.maintenance4 != VK_FALSE) != full13) {
        std::printf("VK13-SYNC2-CANARY: FAIL (Vulkan 1.3 rollup wrong -- expected "
                    "synchronization2=1, maintenance4=%d at this version)\n",
                    full13 ? 1 : 0);
        vkDestroyInstance(instance, nullptr);
        return 1;
    }

    std::uint32_t qf_count = 1;
    VkQueueFamilyProperties qf{};
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &qf_count, &qf);
    if ((qf.queueFlags & VK_QUEUE_COMPUTE_BIT) == 0) {
        std::printf("VK13-SYNC2-CANARY: SKIP (family 0 has no COMPUTE)\n");
        vkDestroyInstance(instance, nullptr);
        return 0;
    }
    const bool timestamps_ok = qf.timestampValidBits > 0;

    int rc = 1;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    VkShaderModule module = VK_NULL_HANDLE;
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkDescriptorPool dpool = VK_NULL_HANDLE;
    VkCommandPool cpool = VK_NULL_HANDLE;
    VkQueryPool qpool = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkEvent event = VK_NULL_HANDLE;
    VkBuffer buf_in = VK_NULL_HANDLE, buf_out = VK_NULL_HANDLE, buf_ubo = VK_NULL_HANDLE,
             buf_read = VK_NULL_HANDLE;
    VkDeviceMemory mem_in = VK_NULL_HANDLE, mem_out = VK_NULL_HANDLE, mem_ubo = VK_NULL_HANDLE,
                   mem_read = VK_NULL_HANDLE;
    PFN_vkCmdPipelineBarrier2KHR pfn_barrier2 = nullptr;
    PFN_vkCmdWriteTimestamp2KHR pfn_ts2 = nullptr;
    PFN_vkQueueSubmit2KHR pfn_submit2 = nullptr;
    PFN_vkCmdSetEvent2KHR pfn_set_event2 = nullptr;

    {
        // --- Device with VK_KHR_synchronization2 enabled (extension + the feature chained). ---
        const float prio = 1.0f;
        VkDeviceQueueCreateInfo qci{};
        qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qci.queueFamilyIndex = 0;
        qci.queueCount = 1;
        qci.pQueuePriorities = &prio;
        const char* dev_exts[] = {VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME};
        VkPhysicalDeviceSynchronization2Features enable_s2{};
        enable_s2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES;
        enable_s2.synchronization2 = VK_TRUE;
        VkDeviceCreateInfo dci{};
        dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        dci.pNext = &enable_s2;
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qci;
        dci.enabledExtensionCount = 1;
        dci.ppEnabledExtensionNames = dev_exts;
        if (vkCreateDevice(phys, &dci, nullptr, &device) != VK_SUCCESS) {
            std::printf("VK13-SYNC2-CANARY: FAIL (vkCreateDevice)\n");
            goto teardown;
        }
        vkGetDeviceQueue(device, 0, 0, &queue);

        pfn_barrier2 = reinterpret_cast<PFN_vkCmdPipelineBarrier2KHR>(
            vkGetDeviceProcAddr(device, "vkCmdPipelineBarrier2KHR"));
        pfn_ts2 = reinterpret_cast<PFN_vkCmdWriteTimestamp2KHR>(
            vkGetDeviceProcAddr(device, "vkCmdWriteTimestamp2KHR"));
        pfn_submit2 = reinterpret_cast<PFN_vkQueueSubmit2KHR>(
            vkGetDeviceProcAddr(device, "vkQueueSubmit2KHR"));
        pfn_set_event2 = reinterpret_cast<PFN_vkCmdSetEvent2KHR>(
            vkGetDeviceProcAddr(device, "vkCmdSetEvent2KHR"));
        if (pfn_barrier2 == nullptr || pfn_ts2 == nullptr || pfn_submit2 == nullptr ||
            pfn_set_event2 == nullptr) {
            std::printf("VK13-SYNC2-CANARY: FAIL (the sync2 *KHR commands did not resolve on the "
                        "native lane)\n");
            goto teardown;
        }

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
        auto make_buffer = [&](VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer* buf,
                               VkDeviceMemory* mem) {
            VkBufferCreateInfo bci{};
            bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            bci.size = size;
            bci.usage = usage;
            bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            if (vkCreateBuffer(device, &bci, nullptr, buf) != VK_SUCCESS) {
                return false;
            }
            VkMemoryRequirements mr{};
            vkGetBufferMemoryRequirements(device, *buf, &mr);
            const std::uint32_t type = pick(mr.memoryTypeBits);
            if (type == UINT32_MAX) {
                return false;
            }
            VkMemoryAllocateInfo mai{};
            mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            mai.allocationSize = mr.size;
            mai.memoryTypeIndex = type;
            if (vkAllocateMemory(device, &mai, nullptr, mem) != VK_SUCCESS) {
                return false;
            }
            return vkBindBufferMemory(device, *buf, *mem, 0) == VK_SUCCESS;
        };
        const VkDeviceSize bytes = kElems * sizeof(std::uint32_t);
        if (!make_buffer(bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &buf_in, &mem_in) ||
            !make_buffer(bytes,
                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                         &buf_out, &mem_out) ||
            !make_buffer(256, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &buf_ubo, &mem_ubo) ||
            !make_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &buf_read, &mem_read)) {
            std::printf("VK13-SYNC2-CANARY: FAIL (buffer/memory setup)\n");
            goto teardown;
        }

        void* p = nullptr;
        if (vkMapMemory(device, mem_in, 0, VK_WHOLE_SIZE, 0, &p) != VK_SUCCESS) {
            goto teardown;
        }
        for (std::uint32_t i = 0; i < kElems; ++i) {
            static_cast<std::uint32_t*>(p)[i] = i * 5 + 1;
        }
        vkUnmapMemory(device, mem_in);
        if (vkMapMemory(device, mem_ubo, 0, VK_WHOLE_SIZE, 0, &p) != VK_SUCCESS) {
            goto teardown;
        }
        static_cast<std::uint32_t*>(p)[0] = kScale;
        vkUnmapMemory(device, mem_ubo);
        void* read_ptr = nullptr;
        if (vkMapMemory(device, mem_read, 0, VK_WHOLE_SIZE, 0, &read_ptr) != VK_SUCCESS) {
            goto teardown;
        }
        std::memset(read_ptr, 0, bytes); // poison: stale zeros must NOT pass the assert

        VkShaderModuleCreateInfo smci{};
        smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        smci.codeSize = sizeof(kComputeSpv);
        smci.pCode = kComputeSpv;
        if (vkCreateShaderModule(device, &smci, nullptr, &module) != VK_SUCCESS) {
            goto teardown;
        }
        VkDescriptorSetLayoutBinding binds[3]{};
        for (std::uint32_t b = 0; b < 3; ++b) {
            binds[b].binding = b;
            binds[b].descriptorType =
                b == 2 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            binds[b].descriptorCount = 1;
            binds[b].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo dslci{};
        dslci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        dslci.bindingCount = 3;
        dslci.pBindings = binds;
        if (vkCreateDescriptorSetLayout(device, &dslci, nullptr, &dsl) != VK_SUCCESS) {
            goto teardown;
        }
        VkPushConstantRange push{};
        push.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        push.offset = 0;
        push.size = sizeof(std::uint32_t);
        VkPipelineLayoutCreateInfo plci{};
        plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        plci.setLayoutCount = 1;
        plci.pSetLayouts = &dsl;
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
            goto teardown;
        }

        VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2},
                                         {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1}};
        VkDescriptorPoolCreateInfo dpci{};
        dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        dpci.maxSets = 1;
        dpci.poolSizeCount = 2;
        dpci.pPoolSizes = sizes;
        if (vkCreateDescriptorPool(device, &dpci, nullptr, &dpool) != VK_SUCCESS) {
            goto teardown;
        }
        VkDescriptorSetAllocateInfo dsai{};
        dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsai.descriptorPool = dpool;
        dsai.descriptorSetCount = 1;
        dsai.pSetLayouts = &dsl;
        VkDescriptorSet dset = VK_NULL_HANDLE;
        if (vkAllocateDescriptorSets(device, &dsai, &dset) != VK_SUCCESS) {
            goto teardown;
        }
        VkDescriptorBufferInfo infos[3] = {
            {buf_in, 0, bytes}, {buf_out, 0, bytes}, {buf_ubo, 0, sizeof(std::uint32_t)}};
        VkWriteDescriptorSet writes[3]{};
        for (std::uint32_t b = 0; b < 3; ++b) {
            writes[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[b].dstSet = dset;
            writes[b].dstBinding = b;
            writes[b].descriptorCount = 1;
            writes[b].descriptorType = binds[b].descriptorType;
            writes[b].pBufferInfo = &infos[b];
        }
        vkUpdateDescriptorSets(device, 3, writes, 0, nullptr);

        // A TIMESTAMP query pool for vkCmdWriteTimestamp2 + a VkEvent for vkCmdSetEvent2.
        VkQueryPoolCreateInfo qpci{};
        qpci.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        qpci.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qpci.queryCount = 1;
        if (vkCreateQueryPool(device, &qpci, nullptr, &qpool) != VK_SUCCESS) {
            goto teardown;
        }
        VkEventCreateInfo eci{};
        eci.sType = VK_STRUCTURE_TYPE_EVENT_CREATE_INFO;
        if (vkCreateEvent(device, &eci, nullptr, &event) != VK_SUCCESS) {
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
        vkCmdResetQueryPool(cmd, qpool, 0, 1);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &dset, 0,
                                nullptr);
        const std::uint32_t bias = kBias;
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(bias), &bias);
        vkCmdDispatch(cmd, kElems / 64, 1, 1);
        // vkCmdWriteTimestamp2 with a SINGLE 64-bit stage (compute shader completion).
        pfn_ts2(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, qpool, 0);
        // THE load-bearing barrier2: a GLOBAL VkMemoryBarrier2, SHADER_WRITE -> TRANSFER_READ, in a
        // VkDependencyInfo -- ordering the compute writes before the copy reads (64-bit masks).
        VkMemoryBarrier2 mb{};
        mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
        mb.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        mb.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
        mb.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        mb.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        VkDependencyInfo di{};
        di.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        di.memoryBarrierCount = 1;
        di.pMemoryBarriers = &mb;
        pfn_barrier2(cmd, &di);
        VkBufferCopy region{0, 0, bytes};
        vkCmdCopyBuffer(cmd, buf_out, buf_read, 1, &region);
        // vkCmdSetEvent2 (empty dependency) -- the device sets the event; the host reads it after.
        VkDependencyInfo empty_di{};
        empty_di.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        pfn_set_event2(cmd, event, &empty_di);
        vkEndCommandBuffer(cmd);

        VkFenceCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        if (vkCreateFence(device, &fci, nullptr, &fence) != VK_SUCCESS) {
            goto teardown;
        }
        // Submit the WHOLE thing via vkQueueSubmit2 (one VkSubmitInfo2 + the fence).
        VkCommandBufferSubmitInfo cbsi{};
        cbsi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
        cbsi.commandBuffer = cmd;
        VkSubmitInfo2 si2{};
        si2.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
        si2.commandBufferInfoCount = 1;
        si2.pCommandBufferInfos = &cbsi;
        if (pfn_submit2(queue, 1, &si2, fence) != VK_SUCCESS) {
            std::printf("VK13-SYNC2-CANARY: FAIL (vkQueueSubmit2)\n");
            goto teardown;
        }
        if (vkWaitForFences(device, 1, &fence, VK_TRUE, ~0ull) != VK_SUCCESS) {
            std::printf("VK13-SYNC2-CANARY: FAIL (vkWaitForFences)\n");
            goto teardown;
        }

        // --- The barrier2-ordered compute readback: byte-exact out[i] = in[i]*scale + bias + i.
        // ---
        const auto* out_words = static_cast<const std::uint32_t*>(read_ptr);
        std::uint32_t bad = 0;
        for (std::uint32_t i = 0; i < kElems; ++i) {
            const std::uint32_t want_v = (i * 5 + 1) * kScale + kBias + i;
            if (out_words[i] != want_v) {
                if (bad < 4) {
                    std::printf("VK13-SYNC2-CANARY: mismatch[%u] got=%u want=%u\n", i, out_words[i],
                                want_v);
                }
                ++bad;
            }
        }
        if (bad != 0) {
            std::printf("VK13-SYNC2-CANARY: FAIL (%u/%u values wrong -- the barrier2 did not order "
                        "the compute write before the copy)\n",
                        bad, kElems);
            goto teardown;
        }
        std::printf("VK13-SYNC2-CANARY: compute-RAW-across-barrier2 byte-exact (%u values)\n",
                    kElems);

        // --- vkCmdWriteTimestamp2: the query is written (when the queue supports timestamps). ---
        if (timestamps_ok) {
            std::uint64_t ts = 0;
            const VkResult qr =
                vkGetQueryPoolResults(device, qpool, 0, 1, sizeof(ts), &ts, sizeof(ts),
                                      VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
            std::printf("VK13-SYNC2-CANARY: write_timestamp2 result=%d value_nonzero=%d\n",
                        static_cast<int>(qr), ts != 0 ? 1 : 0);
            if (qr != VK_SUCCESS || ts == 0) {
                std::printf("VK13-SYNC2-CANARY: FAIL (vkCmdWriteTimestamp2 did not record a "
                            "non-zero timestamp)\n");
                goto teardown;
            }
        } else {
            std::printf("VK13-SYNC2-CANARY: write_timestamp2 recorded (timestampValidBits=0, value "
                        "not asserted)\n");
        }

        // --- vkCmdSetEvent2 reached the host: the event is SET after the submit completed. ---
        const VkResult ev = vkGetEventStatus(device, event);
        std::printf("VK13-SYNC2-CANARY: set_event2 -> event_status=%d\n", static_cast<int>(ev));
        if (ev != VK_EVENT_SET) {
            std::printf("VK13-SYNC2-CANARY: FAIL (vkCmdSetEvent2 did not set the event through the "
                        "sync2 submit path)\n");
            goto teardown;
        }

        std::printf(
            "VK13-SYNC2-CANARY: PASS (native lane serves VK_KHR_synchronization2: a compute "
            "RAW across vkCmdPipelineBarrier2, vkCmdWriteTimestamp2, vkCmdSetEvent2, and "
            "vkQueueSubmit2 all on the real GPU with faithful 64-bit masks)\n");
        rc = 0;
    }

teardown:
    if (device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device);
    }
    if (event != VK_NULL_HANDLE) {
        vkDestroyEvent(device, event, nullptr);
    }
    if (qpool != VK_NULL_HANDLE) {
        vkDestroyQueryPool(device, qpool, nullptr);
    }
    if (fence != VK_NULL_HANDLE) {
        vkDestroyFence(device, fence, nullptr);
    }
    if (cpool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(device, cpool, nullptr);
    }
    if (dpool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device, dpool, nullptr);
    }
    if (pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(device, pipeline, nullptr);
    }
    if (layout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device, layout, nullptr);
    }
    if (dsl != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device, dsl, nullptr);
    }
    if (module != VK_NULL_HANDLE) {
        vkDestroyShaderModule(device, module, nullptr);
    }
    for (VkBuffer b : {buf_in, buf_out, buf_ubo, buf_read}) {
        if (b != VK_NULL_HANDLE) {
            vkDestroyBuffer(device, b, nullptr);
        }
    }
    for (VkDeviceMemory m : {mem_in, mem_out, mem_ubo, mem_read}) {
        if (m != VK_NULL_HANDLE) {
            vkFreeMemory(device, m, nullptr);
        }
    }
    if (device != VK_NULL_HANDLE) {
        vkDestroyDevice(device, nullptr);
    }
    vkDestroyInstance(instance, nullptr);
    if (rc == 0) {
        std::printf("VK13-SYNC2-CANARY: done\n");
    }
    return rc;
}
