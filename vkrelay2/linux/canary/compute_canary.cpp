// vkrelay2 compute canary.
//
// The on-screen-less proof of the compute class: a native Vulkan client through the loader +
// pinned vkrelay2 ICD that runs `out[i] = in[i] * scale + bias + i` (compute_spv.h) over 4096
// u32s on the REAL GPU across the relay, and asserts every result byte-exactly. The chain it
// proves end to end: queue-flags honesty (COMPUTE advertised) -> compute pipeline create ->
// storage/uniform descriptors + push constants -> bind at the COMPUTE point -> dispatch -> the
// shader-write -> transfer-read BUFFER barrier (never queue-order luck) ->
// vkCmdCopyBuffer into a mapped readback buffer (the buffer-dst readback class this canary
// closes) -> fence -> exact values in the mapped bytes.
//
// No X window and no swapchain: compute is the first fully surfaceless canary. Prints
// greppable COMPUTE-CANARY markers; exit 0 only on a full PASS (SKIPs cleanly when no
// ICD/worker is reachable, so a driverless box does not fail the smoke).

#include <vulkan/vulkan.h>

#include "tests/compute_spv.h"

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

constexpr std::uint32_t kElems = 4096; // / local_size 64 = 64 groups
constexpr std::uint32_t kScale = 3;
constexpr std::uint32_t kBias = 7;

void logf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::fprintf(stderr, "COMPUTE-CANARY: ");
    std::vfprintf(stderr, fmt, ap);
    std::fprintf(stderr, "\n");
    va_end(ap);
    std::fflush(stderr);
}

} // namespace

int main() {
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.apiVersion = VK_API_VERSION_1_1;
    app.pApplicationName = "vkrelay2-compute-canary";
    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    VkInstance instance = VK_NULL_HANDLE;
    if (vkCreateInstance(&ici, nullptr, &instance) != VK_SUCCESS) {
        logf("SKIP (vkCreateInstance failed -- no ICD/worker?)");
        return 0;
    }

    int rc = 1;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    VkShaderModule module = VK_NULL_HANDLE;
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkDescriptorPool dpool = VK_NULL_HANDLE;
    VkCommandPool cpool = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkBuffer buf_in = VK_NULL_HANDLE, buf_out = VK_NULL_HANDLE, buf_ubo = VK_NULL_HANDLE,
             buf_read = VK_NULL_HANDLE;
    VkDeviceMemory mem_in = VK_NULL_HANDLE, mem_out = VK_NULL_HANDLE, mem_ubo = VK_NULL_HANDLE,
                   mem_read = VK_NULL_HANDLE;

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

        // The honesty gate this canary added: family 0 must advertise COMPUTE.
        std::uint32_t qf_count = 1;
        VkQueueFamilyProperties qf{};
        vkGetPhysicalDeviceQueueFamilyProperties(phys, &qf_count, &qf);
        if ((qf.queueFlags & VK_QUEUE_COMPUTE_BIT) == 0) {
            logf("FAIL (the worker did not advertise a COMPUTE queue)");
            goto teardown;
        }
        logf("queue family advertises COMPUTE (flags 0x%x)", qf.queueFlags);

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
        if (vkCreateDevice(phys, &dci, nullptr, &device) != VK_SUCCESS) {
            logf("FAIL (vkCreateDevice)");
            goto teardown;
        }
        vkGetDeviceQueue(device, 0, 0, &queue);

        // Host-visible | coherent memory type (the relay's mapped-shadow class).
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
            logf("FAIL (buffer/memory setup)");
            goto teardown;
        }

        // Fill the inputs through the mapped shadows; keep the READBACK buffer mapped for the
        // duration (the zink staging pattern the download contract serves).
        void* p = nullptr;
        if (vkMapMemory(device, mem_in, 0, VK_WHOLE_SIZE, 0, &p) != VK_SUCCESS) {
            logf("FAIL (map in)");
            goto teardown;
        }
        auto* in_words = static_cast<std::uint32_t*>(p);
        for (std::uint32_t i = 0; i < kElems; ++i) {
            in_words[i] = i * 5 + 1;
        }
        vkUnmapMemory(device, mem_in);
        if (vkMapMemory(device, mem_ubo, 0, VK_WHOLE_SIZE, 0, &p) != VK_SUCCESS) {
            logf("FAIL (map ubo)");
            goto teardown;
        }
        static_cast<std::uint32_t*>(p)[0] = kScale;
        vkUnmapMemory(device, mem_ubo);
        void* read_ptr = nullptr;
        if (vkMapMemory(device, mem_read, 0, VK_WHOLE_SIZE, 0, &read_ptr) != VK_SUCCESS) {
            logf("FAIL (map readback)");
            goto teardown;
        }
        std::memset(read_ptr, 0, bytes); // poison: stale zeros must NOT pass the assert

        // Shader module + descriptor machinery + the compute pipeline (with a push range).
        VkShaderModuleCreateInfo smci{};
        smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        smci.codeSize = sizeof(kComputeSpv);
        smci.pCode = kComputeSpv;
        if (vkCreateShaderModule(device, &smci, nullptr, &module) != VK_SUCCESS) {
            logf("FAIL (vkCreateShaderModule)");
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
            logf("FAIL (set layout)");
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
            logf("FAIL (pipeline layout)");
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
            logf("FAIL (vkCreateComputePipelines)");
            goto teardown;
        }
        logf("compute pipeline created");

        VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2},
                                         {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1}};
        VkDescriptorPoolCreateInfo dpci{};
        dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        dpci.maxSets = 1;
        dpci.poolSizeCount = 2;
        dpci.pPoolSizes = sizes;
        if (vkCreateDescriptorPool(device, &dpci, nullptr, &dpool) != VK_SUCCESS) {
            logf("FAIL (descriptor pool)");
            goto teardown;
        }
        VkDescriptorSetAllocateInfo dsai{};
        dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsai.descriptorPool = dpool;
        dsai.descriptorSetCount = 1;
        dsai.pSetLayouts = &dsl;
        VkDescriptorSet dset = VK_NULL_HANDLE;
        if (vkAllocateDescriptorSets(device, &dsai, &dset) != VK_SUCCESS) {
            logf("FAIL (descriptor set)");
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

        // Record: bind(COMPUTE) + sets + push + dispatch + barrier + copy-out.
        VkCommandPoolCreateInfo cpci2{};
        cpci2.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        cpci2.queueFamilyIndex = 0;
        if (vkCreateCommandPool(device, &cpci2, nullptr, &cpool) != VK_SUCCESS) {
            logf("FAIL (command pool)");
            goto teardown;
        }
        VkCommandBufferAllocateInfo cbai{};
        cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cbai.commandPool = cpool;
        cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbai.commandBufferCount = 1;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        if (vkAllocateCommandBuffers(device, &cbai, &cmd) != VK_SUCCESS) {
            logf("FAIL (command buffer)");
            goto teardown;
        }
        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &bi);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &dset, 0,
                                nullptr);
        const std::uint32_t bias = kBias;
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(bias), &bias);
        vkCmdDispatch(cmd, kElems / 64, 1, 1);
        // The dependency: shader writes -> transfer reads, on the out buffer.
        VkBufferMemoryBarrier bar{};
        bar.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        bar.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        bar.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.buffer = buf_out;
        bar.offset = 0;
        bar.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1, &bar, 0, nullptr);
        VkBufferCopy region{0, 0, bytes};
        vkCmdCopyBuffer(cmd, buf_out, buf_read, 1, &region);
        vkEndCommandBuffer(cmd);

        VkFenceCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        if (vkCreateFence(device, &fci, nullptr, &fence) != VK_SUCCESS) {
            logf("FAIL (fence)");
            goto teardown;
        }
        VkSubmitInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        if (vkQueueSubmit(queue, 1, &si, fence) != VK_SUCCESS) {
            logf("FAIL (vkQueueSubmit)");
            goto teardown;
        }
        if (vkWaitForFences(device, 1, &fence, VK_TRUE, ~0ull) != VK_SUCCESS) {
            logf("FAIL (vkWaitForFences)");
            goto teardown;
        }
        logf("dispatched %u groups + barrier + copy-out; fence signaled", kElems / 64);

        // Exact-value assert through the retained readback mapping.
        const auto* out_words = static_cast<const std::uint32_t*>(read_ptr);
        std::uint32_t bad = 0;
        for (std::uint32_t i = 0; i < kElems; ++i) {
            const std::uint32_t expect = (i * 5 + 1) * kScale + kBias + i;
            if (out_words[i] != expect) {
                if (bad < 4) {
                    logf("MISMATCH at %u: got %u want %u", i, out_words[i], expect);
                }
                ++bad;
            }
        }
        if (bad != 0) {
            logf("FAIL (%u of %u results wrong)", bad, kElems);
            goto teardown;
        }
        logf("all %u results EXACT (out[i] == in[i]*%u + %u + i)", kElems, kScale, kBias);
        logf("PASS");
        rc = 0;
    }

teardown:
    if (device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device);
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
        vkDestroyDevice(device, nullptr);
    }
    vkDestroyInstance(instance, nullptr);
    return rc;
}
