// Standalone native-Windows oracle for the AMD exact-tail vertex-fetch investigation.
//
// This executable links only the system Vulkan loader. It does not instantiate a vkrelay2
// backend, worker, ICD, or protocol object. For every physical device it renders the same
// two-triangle, vec2+vec3 workload with the final attribute ending exactly at VkBuffer::size and
// with 16 bytes of unused tail padding. It compares an interleaved binding with zink's equivalent
// split-binding form (the same buffer bound at offsets N and N+8), with robustBufferAccess disabled
// and (when supported) enabled. The AMD 610M fails only the exact split-binding case; NVIDIA passes
// every case. This is a minimal native reproducer for the driver behavior hidden by vkrelay2's
// private host-buffer tail guard.

// This is deliberately a diagnostic target, not a CTest gate: an affected driver is an expected
// and useful outcome. The process returns nonzero if any executed case renders incorrectly.

#include <cstddef>

#include "tests/vbo_spv.h"

#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

constexpr std::uint32_t kDim = 64;
constexpr VkDeviceSize kReadbackBytes = static_cast<VkDeviceSize>(kDim) * kDim * 4;
constexpr VkDeviceSize kStride = 20;
constexpr VkDeviceSize kVerticesPerQuad = 6;
constexpr VkDeviceSize kQuadBytes = kStride * kVerticesPerQuad;
constexpr VkDeviceSize kSlots = 16;
constexpr VkDeviceSize kPayloadBytes = kSlots * kQuadBytes;

enum class BindingStyle { Interleaved, Split };

const char* binding_style_name(BindingStyle style) {
    return style == BindingStyle::Split ? "split" : "interleaved";
}

struct Vertex {
    float x;
    float y;
    float r;
    float g;
    float b;
};

struct Resources {
    VkDevice device = VK_NULL_HANDLE;
    VkBuffer vertex_buffer = VK_NULL_HANDLE;
    VkDeviceMemory vertex_memory = VK_NULL_HANDLE;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory image_memory = VK_NULL_HANDLE;
    VkImageView image_view = VK_NULL_HANDLE;
    VkBuffer readback = VK_NULL_HANDLE;
    VkDeviceMemory readback_memory = VK_NULL_HANDLE;
    VkRenderPass render_pass = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkShaderModule vertex_shader = VK_NULL_HANDLE;
    VkShaderModule fragment_shader = VK_NULL_HANDLE;
    VkCommandPool command_pool = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;

    void destroy() {
        if (device == VK_NULL_HANDLE) {
            return;
        }
        vkDeviceWaitIdle(device);
        if (fence != VK_NULL_HANDLE)
            vkDestroyFence(device, fence, nullptr);
        if (command_pool != VK_NULL_HANDLE)
            vkDestroyCommandPool(device, command_pool, nullptr);
        if (pipeline != VK_NULL_HANDLE)
            vkDestroyPipeline(device, pipeline, nullptr);
        if (pipeline_layout != VK_NULL_HANDLE)
            vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
        if (fragment_shader != VK_NULL_HANDLE)
            vkDestroyShaderModule(device, fragment_shader, nullptr);
        if (vertex_shader != VK_NULL_HANDLE)
            vkDestroyShaderModule(device, vertex_shader, nullptr);
        if (framebuffer != VK_NULL_HANDLE)
            vkDestroyFramebuffer(device, framebuffer, nullptr);
        if (render_pass != VK_NULL_HANDLE)
            vkDestroyRenderPass(device, render_pass, nullptr);
        if (readback != VK_NULL_HANDLE)
            vkDestroyBuffer(device, readback, nullptr);
        if (readback_memory != VK_NULL_HANDLE)
            vkFreeMemory(device, readback_memory, nullptr);
        if (image_view != VK_NULL_HANDLE)
            vkDestroyImageView(device, image_view, nullptr);
        if (image != VK_NULL_HANDLE)
            vkDestroyImage(device, image, nullptr);
        if (image_memory != VK_NULL_HANDLE)
            vkFreeMemory(device, image_memory, nullptr);
        if (vertex_buffer != VK_NULL_HANDLE)
            vkDestroyBuffer(device, vertex_buffer, nullptr);
        if (vertex_memory != VK_NULL_HANDLE)
            vkFreeMemory(device, vertex_memory, nullptr);
        vkDestroyDevice(device, nullptr);
        device = VK_NULL_HANDLE;
    }
};

bool check(VkResult result, const char* what) {
    if (result == VK_SUCCESS) {
        return true;
    }
    std::printf("NATIVE-VERTEX-TAIL: ERROR %s -> VkResult %d\n", what, static_cast<int>(result));
    return false;
}

int find_memory_type(const VkPhysicalDeviceMemoryProperties& properties, std::uint32_t bits,
                     VkMemoryPropertyFlags required) {
    for (std::uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
        if ((bits & (1u << i)) != 0 &&
            (properties.memoryTypes[i].propertyFlags & required) == required) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

VkShaderModule make_shader(VkDevice device, const std::uint32_t* code, std::size_t bytes) {
    VkShaderModuleCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = bytes;
    info.pCode = code;
    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device, &info, nullptr, &module) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    return module;
}

bool is_red(const std::array<std::uint8_t, 4>& px) {
    return px[0] >= 250 && px[1] <= 5 && px[2] <= 5 && px[3] >= 250;
}

bool is_green(const std::array<std::uint8_t, 4>& px) {
    return px[0] <= 5 && px[1] >= 250 && px[2] <= 5 && px[3] >= 250;
}

bool run_case(VkPhysicalDevice physical, std::uint32_t queue_family, const char* device_name,
              bool robust, bool padded, BindingStyle binding_style) {
    Resources r;
    void* vertex_mapped = nullptr;
    VkPhysicalDeviceMemoryProperties memory_properties{};
    vkGetPhysicalDeviceMemoryProperties(physical, &memory_properties);

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info{};
    queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_info.queueFamilyIndex = queue_family;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;
    VkPhysicalDeviceFeatures features{};
    features.robustBufferAccess = robust ? VK_TRUE : VK_FALSE;
    VkDeviceCreateInfo device_info{};
    device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    device_info.pEnabledFeatures = &features;
    if (!check(vkCreateDevice(physical, &device_info, nullptr, &r.device), "vkCreateDevice")) {
        return false;
    }

    bool ok = false;
    VkQueue queue = VK_NULL_HANDLE;
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    const bool split_bindings = binding_style == BindingStyle::Split;
    vkGetDeviceQueue(r.device, queue_family, 0, &queue);

    do {
        const VkDeviceSize vertex_bytes = kPayloadBytes + (padded ? 16 : 0);
        VkBufferCreateInfo buffer_info{};
        buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buffer_info.size = vertex_bytes;
        buffer_info.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (!check(vkCreateBuffer(r.device, &buffer_info, nullptr, &r.vertex_buffer),
                   "vkCreateBuffer(vertex)"))
            break;
        VkMemoryRequirements vertex_requirements{};
        vkGetBufferMemoryRequirements(r.device, r.vertex_buffer, &vertex_requirements);
        const int vertex_type = find_memory_type(
            memory_properties, vertex_requirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (vertex_type < 0) {
            std::printf("NATIVE-VERTEX-TAIL: ERROR no coherent host-visible vertex memory\n");
            break;
        }
        VkMemoryAllocateInfo allocation{};
        allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocation.allocationSize = vertex_requirements.size;
        allocation.memoryTypeIndex = static_cast<std::uint32_t>(vertex_type);
        if (!check(vkAllocateMemory(r.device, &allocation, nullptr, &r.vertex_memory),
                   "vkAllocateMemory(vertex)") ||
            !check(vkBindBufferMemory(r.device, r.vertex_buffer, r.vertex_memory, 0),
                   "vkBindBufferMemory(vertex)"))
            break;

        std::printf("NATIVE-VERTEX-TAIL: setup device=\"%s\" binding_style=%s padded=%d "
                    "size=%llu req=%llu align=%llu bits=0x%x type=%d\n",
                    device_name, binding_style_name(binding_style), padded ? 1 : 0,
                    static_cast<unsigned long long>(vertex_bytes),
                    static_cast<unsigned long long>(vertex_requirements.size),
                    static_cast<unsigned long long>(vertex_requirements.alignment),
                    vertex_requirements.memoryTypeBits, vertex_type);

        const Vertex vertices[6] = {
            {-1.0f, -1.0f, 1.0f, 0.0f, 0.0f}, {1.0f, -1.0f, 1.0f, 0.0f, 0.0f},
            {1.0f, 1.0f, 1.0f, 0.0f, 0.0f},   {-1.0f, -1.0f, 0.0f, 1.0f, 0.0f},
            {1.0f, 1.0f, 0.0f, 1.0f, 0.0f},   {-1.0f, 1.0f, 0.0f, 1.0f, 0.0f},
        };
        if (!check(vkMapMemory(r.device, r.vertex_memory, 0, vertex_bytes, 0, &vertex_mapped),
                   "vkMapMemory(vertex)"))
            break;
        std::memset(vertex_mapped, 0, static_cast<std::size_t>(vertex_bytes));

        VkImageCreateInfo image_info{};
        image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        image_info.imageType = VK_IMAGE_TYPE_2D;
        image_info.format = VK_FORMAT_R8G8B8A8_UNORM;
        image_info.extent = {kDim, kDim, 1};
        image_info.mipLevels = 1;
        image_info.arrayLayers = 1;
        image_info.samples = VK_SAMPLE_COUNT_1_BIT;
        image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
        image_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (!check(vkCreateImage(r.device, &image_info, nullptr, &r.image), "vkCreateImage"))
            break;
        VkMemoryRequirements image_requirements{};
        vkGetImageMemoryRequirements(r.device, r.image, &image_requirements);
        int image_type = find_memory_type(memory_properties, image_requirements.memoryTypeBits,
                                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (image_type < 0)
            image_type = find_memory_type(memory_properties, image_requirements.memoryTypeBits, 0);
        if (image_type < 0)
            break;
        allocation.allocationSize = image_requirements.size;
        allocation.memoryTypeIndex = static_cast<std::uint32_t>(image_type);
        if (!check(vkAllocateMemory(r.device, &allocation, nullptr, &r.image_memory),
                   "vkAllocateMemory(image)") ||
            !check(vkBindImageMemory(r.device, r.image, r.image_memory, 0), "vkBindImageMemory"))
            break;
        VkImageViewCreateInfo view_info{};
        view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_info.image = r.image;
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = VK_FORMAT_R8G8B8A8_UNORM;
        view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (!check(vkCreateImageView(r.device, &view_info, nullptr, &r.image_view),
                   "vkCreateImageView"))
            break;

        buffer_info.size = kReadbackBytes;
        buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if (!check(vkCreateBuffer(r.device, &buffer_info, nullptr, &r.readback),
                   "vkCreateBuffer(readback)"))
            break;
        VkMemoryRequirements readback_requirements{};
        vkGetBufferMemoryRequirements(r.device, r.readback, &readback_requirements);
        const int readback_type = find_memory_type(
            memory_properties, readback_requirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (readback_type < 0)
            break;
        allocation.allocationSize = readback_requirements.size;
        allocation.memoryTypeIndex = static_cast<std::uint32_t>(readback_type);
        if (!check(vkAllocateMemory(r.device, &allocation, nullptr, &r.readback_memory),
                   "vkAllocateMemory(readback)") ||
            !check(vkBindBufferMemory(r.device, r.readback, r.readback_memory, 0),
                   "vkBindBufferMemory(readback)"))
            break;

        VkAttachmentDescription attachment{};
        attachment.format = VK_FORMAT_R8G8B8A8_UNORM;
        attachment.samples = VK_SAMPLE_COUNT_1_BIT;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attachment.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        VkAttachmentReference color_ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &color_ref;
        VkSubpassDependency dependency{};
        dependency.srcSubpass = 0;
        dependency.dstSubpass = VK_SUBPASS_EXTERNAL;
        dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
        dependency.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dependency.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        VkRenderPassCreateInfo render_pass_info{};
        render_pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        render_pass_info.attachmentCount = 1;
        render_pass_info.pAttachments = &attachment;
        render_pass_info.subpassCount = 1;
        render_pass_info.pSubpasses = &subpass;
        render_pass_info.dependencyCount = 1;
        render_pass_info.pDependencies = &dependency;
        if (!check(vkCreateRenderPass(r.device, &render_pass_info, nullptr, &r.render_pass),
                   "vkCreateRenderPass"))
            break;
        VkFramebufferCreateInfo framebuffer_info{};
        framebuffer_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebuffer_info.renderPass = r.render_pass;
        framebuffer_info.attachmentCount = 1;
        framebuffer_info.pAttachments = &r.image_view;
        framebuffer_info.width = kDim;
        framebuffer_info.height = kDim;
        framebuffer_info.layers = 1;
        if (!check(vkCreateFramebuffer(r.device, &framebuffer_info, nullptr, &r.framebuffer),
                   "vkCreateFramebuffer"))
            break;

        r.vertex_shader = make_shader(r.device, vkr::vbo_spv::kVert, vkr::vbo_spv::kVertBytes);
        r.fragment_shader = make_shader(r.device, vkr::vbo_spv::kFrag, vkr::vbo_spv::kFragBytes);
        if (r.vertex_shader == VK_NULL_HANDLE || r.fragment_shader == VK_NULL_HANDLE)
            break;
        VkPipelineLayoutCreateInfo layout_info{};
        layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        if (!check(vkCreatePipelineLayout(r.device, &layout_info, nullptr, &r.pipeline_layout),
                   "vkCreatePipelineLayout"))
            break;
        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = r.vertex_shader;
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = r.fragment_shader;
        stages[1].pName = "main";
        VkVertexInputBindingDescription bindings[2] = {
            {0, static_cast<std::uint32_t>(kStride), VK_VERTEX_INPUT_RATE_VERTEX},
            {1, static_cast<std::uint32_t>(kStride), VK_VERTEX_INPUT_RATE_VERTEX},
        };
        VkVertexInputAttributeDescription attributes[2] = {
            {0, 0, VK_FORMAT_R32G32_SFLOAT, 0},
            {1, split_bindings ? 1u : 0u, VK_FORMAT_R32G32B32_SFLOAT, split_bindings ? 0u : 8u},
        };
        VkPipelineVertexInputStateCreateInfo vertex_input{};
        vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vertex_input.vertexBindingDescriptionCount = split_bindings ? 2u : 1u;
        vertex_input.pVertexBindingDescriptions = bindings;
        vertex_input.vertexAttributeDescriptionCount = 2;
        vertex_input.pVertexAttributeDescriptions = attributes;
        VkPipelineInputAssemblyStateCreateInfo assembly{};
        assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo viewport_state{};
        viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewport_state.viewportCount = 1;
        viewport_state.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rasterization{};
        rasterization.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rasterization.polygonMode = VK_POLYGON_MODE_FILL;
        rasterization.cullMode = VK_CULL_MODE_NONE;
        rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rasterization.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo multisample{};
        multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState blend_attachment{};
        blend_attachment.colorWriteMask = 0xf;
        VkPipelineColorBlendStateCreateInfo blend{};
        blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        blend.attachmentCount = 1;
        blend.pAttachments = &blend_attachment;
        const VkDynamicState dynamic_states[2] = {VK_DYNAMIC_STATE_VIEWPORT,
                                                  VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamic{};
        dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamic.dynamicStateCount = 2;
        dynamic.pDynamicStates = dynamic_states;
        VkGraphicsPipelineCreateInfo pipeline_info{};
        pipeline_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipeline_info.stageCount = 2;
        pipeline_info.pStages = stages;
        pipeline_info.pVertexInputState = &vertex_input;
        pipeline_info.pInputAssemblyState = &assembly;
        pipeline_info.pViewportState = &viewport_state;
        pipeline_info.pRasterizationState = &rasterization;
        pipeline_info.pMultisampleState = &multisample;
        pipeline_info.pColorBlendState = &blend;
        pipeline_info.pDynamicState = &dynamic;
        pipeline_info.layout = r.pipeline_layout;
        pipeline_info.renderPass = r.render_pass;
        pipeline_info.subpass = 0;
        if (!check(vkCreateGraphicsPipelines(r.device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr,
                                             &r.pipeline),
                   "vkCreateGraphicsPipelines"))
            break;

        VkCommandPoolCreateInfo pool_info{};
        pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool_info.queueFamilyIndex = queue_family;
        if (!check(vkCreateCommandPool(r.device, &pool_info, nullptr, &r.command_pool),
                   "vkCreateCommandPool"))
            break;
        VkCommandBufferAllocateInfo command_info{};
        command_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        command_info.commandPool = r.command_pool;
        command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        command_info.commandBufferCount = 1;
        if (!check(vkAllocateCommandBuffers(r.device, &command_info, &command_buffer),
                   "vkAllocateCommandBuffers"))
            break;
        VkFenceCreateInfo fence_info{};
        fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        if (!check(vkCreateFence(r.device, &fence_info, nullptr, &r.fence), "vkCreateFence"))
            break;

        std::array<std::uint8_t, 4> a{};
        std::array<std::uint8_t, 4> b{};
        ok = true;
        int first_bad = -1;
        for (std::uint32_t frame = 0; frame < kSlots; ++frame) {
            if (frame != 0) {
                if (!check(vkResetFences(r.device, 1, &r.fence), "vkResetFences") ||
                    !check(vkResetCommandBuffer(command_buffer, 0), "vkResetCommandBuffer")) {
                    ok = false;
                    break;
                }
            }
            // Keep the coherent mapping alive and populate one previously-unused slot immediately
            // before its submit, matching zink's fresh-offset ring workload. Frame 15's final vec3
            // ends exactly at VkBuffer::size in the unpadded case.
            const VkDeviceSize draw_offset = static_cast<VkDeviceSize>(frame) * kQuadBytes;
            std::memcpy(static_cast<std::uint8_t*>(vertex_mapped) + draw_offset, vertices,
                        sizeof(vertices));

            VkCommandBufferBeginInfo begin{};
            begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            if (!check(vkBeginCommandBuffer(command_buffer, &begin), "vkBeginCommandBuffer")) {
                ok = false;
                break;
            }
            VkClearValue clear{};
            clear.color.float32[3] = 1.0f;
            VkRenderPassBeginInfo render_begin{};
            render_begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
            render_begin.renderPass = r.render_pass;
            render_begin.framebuffer = r.framebuffer;
            render_begin.renderArea.extent = {kDim, kDim};
            render_begin.clearValueCount = 1;
            render_begin.pClearValues = &clear;
            vkCmdBeginRenderPass(command_buffer, &render_begin, VK_SUBPASS_CONTENTS_INLINE);
            vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, r.pipeline);
            VkViewport viewport{0.0f, 0.0f, static_cast<float>(kDim), static_cast<float>(kDim),
                                0.0f, 1.0f};
            VkRect2D scissor{{0, 0}, {kDim, kDim}};
            vkCmdSetViewport(command_buffer, 0, 1, &viewport);
            vkCmdSetScissor(command_buffer, 0, 1, &scissor);
            if (split_bindings) {
                const VkBuffer buffers[2] = {r.vertex_buffer, r.vertex_buffer};
                const VkDeviceSize offsets[2] = {draw_offset, draw_offset + 8};
                vkCmdBindVertexBuffers(command_buffer, 0, 2, buffers, offsets);
            } else {
                vkCmdBindVertexBuffers(command_buffer, 0, 1, &r.vertex_buffer, &draw_offset);
            }
            vkCmdDraw(command_buffer, static_cast<std::uint32_t>(kVerticesPerQuad), 1, 0, 0);
            vkCmdEndRenderPass(command_buffer);
            VkBufferImageCopy copy{};
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageExtent = {kDim, kDim, 1};
            vkCmdCopyImageToBuffer(command_buffer, r.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   r.readback, 1, &copy);
            if (!check(vkEndCommandBuffer(command_buffer), "vkEndCommandBuffer")) {
                ok = false;
                break;
            }
            VkSubmitInfo submit{};
            submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &command_buffer;
            if (!check(vkQueueSubmit(queue, 1, &submit, r.fence), "vkQueueSubmit") ||
                !check(vkWaitForFences(r.device, 1, &r.fence, VK_TRUE, UINT64_MAX),
                       "vkWaitForFences")) {
                ok = false;
                break;
            }

            void* readback_mapped = nullptr;
            if (!check(vkMapMemory(r.device, r.readback_memory, 0, kReadbackBytes, 0,
                                   &readback_mapped),
                       "vkMapMemory(readback)")) {
                ok = false;
                break;
            }
            const auto* pixels = static_cast<const std::uint8_t*>(readback_mapped);
            const auto sample = [&](std::uint32_t x, std::uint32_t y) {
                std::array<std::uint8_t, 4> px{};
                std::memcpy(px.data(), pixels + (static_cast<std::size_t>(y) * kDim + x) * 4, 4);
                return px;
            };
            a = sample(48, 16);
            b = sample(16, 48);
            vkUnmapMemory(r.device, r.readback_memory);
            const bool frame_ok = (is_red(a) && is_green(b)) || (is_green(a) && is_red(b));
            if (!frame_ok && first_bad < 0) {
                first_bad = static_cast<int>(frame);
            }
            ok = frame_ok && ok;
        }
        std::printf("NATIVE-VERTEX-TAIL: device=\"%s\" binding_style=%s robust=%d padded=%d "
                    "frames=%u first_bad=%d a=%02x%02x%02x%02x b=%02x%02x%02x%02x %s\n",
                    device_name, binding_style_name(binding_style), robust ? 1 : 0, padded ? 1 : 0,
                    static_cast<unsigned>(kSlots), first_bad, a[0], a[1], a[2], a[3], b[0], b[1],
                    b[2], b[3], ok ? "PASS" : "FAIL");
    } while (false);

    if (vertex_mapped != nullptr) {
        vkUnmapMemory(r.device, r.vertex_memory);
    }
    r.destroy();
    return ok;
}

} // namespace

int main() {
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "vkrelay2-native-vertex-tail-probe";
    app.apiVersion = VK_API_VERSION_1_0;
    VkInstanceCreateInfo instance_info{};
    instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instance_info.pApplicationInfo = &app;
    VkInstance instance = VK_NULL_HANDLE;
    if (!check(vkCreateInstance(&instance_info, nullptr, &instance), "vkCreateInstance")) {
        return 2;
    }
    std::uint32_t count = 0;
    if (!check(vkEnumeratePhysicalDevices(instance, &count, nullptr),
               "vkEnumeratePhysicalDevices(count)") ||
        count == 0) {
        vkDestroyInstance(instance, nullptr);
        return 2;
    }
    std::vector<VkPhysicalDevice> devices(count);
    if (!check(vkEnumeratePhysicalDevices(instance, &count, devices.data()),
               "vkEnumeratePhysicalDevices(list)")) {
        vkDestroyInstance(instance, nullptr);
        return 2;
    }

    bool all_ok = true;
    for (VkPhysicalDevice physical : devices) {
        VkPhysicalDeviceProperties properties{};
        VkPhysicalDeviceFeatures features{};
        vkGetPhysicalDeviceProperties(physical, &properties);
        vkGetPhysicalDeviceFeatures(physical, &features);
        std::uint32_t family_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, nullptr);
        std::vector<VkQueueFamilyProperties> families(family_count);
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, families.data());
        std::uint32_t graphics_family = UINT32_MAX;
        for (std::uint32_t i = 0; i < family_count; ++i) {
            if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0) {
                graphics_family = i;
                break;
            }
        }
        std::printf("NATIVE-VERTEX-TAIL: enumerate device=\"%s\" vendor=0x%04x device=0x%04x "
                    "driver=0x%08x robust_supported=%d\n",
                    properties.deviceName, properties.vendorID, properties.deviceID,
                    properties.driverVersion, features.robustBufferAccess ? 1 : 0);
        if (graphics_family == UINT32_MAX) {
            std::printf("NATIVE-VERTEX-TAIL: SKIP no graphics queue\n");
            continue;
        }
        constexpr BindingStyle binding_styles[] = {BindingStyle::Interleaved, BindingStyle::Split};
        for (const bool robust : {false, true}) {
            if (robust && !features.robustBufferAccess)
                continue;
            for (const BindingStyle binding_style : binding_styles) {
                all_ok = run_case(physical, graphics_family, properties.deviceName, robust, false,
                                  binding_style) &&
                         all_ok;
                all_ok = run_case(physical, graphics_family, properties.deviceName, robust, true,
                                  binding_style) &&
                         all_ok;
            }
        }
    }
    vkDestroyInstance(instance, nullptr);
    return all_ok ? 0 : 1;
}
