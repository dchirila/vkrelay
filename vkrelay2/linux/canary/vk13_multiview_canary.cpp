// vkrelay2 multiview SERVE-PROOF canary -- the required-feature audit's release gate.
//
// Multiview is REQUIRED since Vulkan 1.1. The relay wires it (viewMask through render-pass2 AND
// dynamic rendering -- serve, don't mask); this canary is the end-to-end proof that it
// actually WORKS on the real GPU through the relay -- the gate that must PASS before the relay
// flips kRelayServesMultiview (and the native lane starts vouching a conformant 1.3).
//
// It renders a full-screen triangle whose fragment color is chosen STRICTLY by gl_ViewIndex (view 0
// = red, view 1 = green) into a 2-layer image with a subpass viewMask of 0b11, then reads back each
// array layer. Layer 0 MUST be all-red and layer 1 MUST be all-green -- if multiview did not run
// per-view on the host, both layers would carry the same color, so this distinguishes a real
// multiview serve from a silent single-view fallback. It does this TWICE: once through a
// vkCreateRenderPass2 viewMask pass and once through a vkCmdBeginRenderingKHR viewMask
// scope. PASS requires BOTH.
//
// Surfaceless native Vulkan through the real loader + the session-pinned vkrelay2 ICD. Greppable
// markers on stdout; run_vk13_multiview_smoke.sh gates on them. Skips cleanly (exit 0) when no
// ICD/worker stack is reachable; FAILs (nonzero) on a real regression.

#include <vulkan/vulkan.h>

#include "tests/multiview_spv.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

constexpr std::uint32_t kDim = 64;
constexpr std::uint32_t kLayers = 2;
constexpr std::uint32_t kViewMask = 0x3; // views 0 and 1
constexpr VkFormat kColorFormat = VK_FORMAT_R8G8B8A8_UNORM;
constexpr VkDeviceSize kLayerBytes = static_cast<VkDeviceSize>(kDim) * kDim * 4;
constexpr VkDeviceSize kBufBytes = kLayerBytes * kLayers;

const unsigned char kRed[4] = {255, 0, 0, 255};   // view 0
const unsigned char kGreen[4] = {0, 255, 0, 255}; // view 1

bool check(VkResult r, const char* what) {
    if (r != VK_SUCCESS) {
        std::printf("VK13-MULTIVIEW-CANARY: FAIL (%s -> VkResult %d)\n", what, static_cast<int>(r));
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

VkShaderModule make_shader(VkDevice device, const std::uint32_t* code, std::uint32_t bytes) {
    VkShaderModuleCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = bytes;
    ci.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    vkCreateShaderModule(device, &ci, nullptr, &m);
    return m;
}

// A whole-image layer-aware color barrier (dynamic rendering owns its own transitions).
void image_barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to,
                   VkAccessFlags src_access, VkAccessFlags dst_access,
                   VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage) {
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask = src_access;
    b.dstAccessMask = dst_access;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, kLayers};
    vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

// A per-path bundle of the GPU objects the render + readback needs.
struct Target {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory img_mem = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory buf_mem = VK_NULL_HANDLE;
};

bool make_target(VkDevice device, const VkPhysicalDeviceMemoryProperties& mp, Target& t) {
    VkImageCreateInfo imgci{};
    imgci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imgci.imageType = VK_IMAGE_TYPE_2D;
    imgci.format = kColorFormat;
    imgci.extent = {kDim, kDim, 1};
    imgci.mipLevels = 1;
    imgci.arrayLayers = kLayers; // the multiview layers gl_ViewIndex addresses
    imgci.samples = VK_SAMPLE_COUNT_1_BIT;
    imgci.tiling = VK_IMAGE_TILING_OPTIMAL;
    imgci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    imgci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!check(vkCreateImage(device, &imgci, nullptr, &t.image), "vkCreateImage")) {
        return false;
    }
    VkMemoryRequirements img_req{};
    vkGetImageMemoryRequirements(device, t.image, &img_req);
    const int img_type =
        find_memory_type(mp, img_req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
    if (img_type < 0) {
        std::printf("VK13-MULTIVIEW-CANARY: FAIL (no device-local memory type)\n");
        return false;
    }
    VkMemoryAllocateInfo img_ai{};
    img_ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    img_ai.allocationSize = img_req.size;
    img_ai.memoryTypeIndex = static_cast<std::uint32_t>(img_type);
    if (!check(vkAllocateMemory(device, &img_ai, nullptr, &t.img_mem), "vkAllocateMemory(img)") ||
        !check(vkBindImageMemory(device, t.image, t.img_mem, 0), "vkBindImageMemory")) {
        return false;
    }
    VkImageViewCreateInfo ivci{};
    ivci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    ivci.image = t.image;
    ivci.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY; // a multiview attachment is an array view
    ivci.format = kColorFormat;
    ivci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, kLayers};
    if (!check(vkCreateImageView(device, &ivci, nullptr, &t.view), "vkCreateImageView")) {
        return false;
    }
    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = kBufBytes;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (!check(vkCreateBuffer(device, &bci, nullptr, &t.buf), "vkCreateBuffer")) {
        return false;
    }
    VkMemoryRequirements buf_req{};
    vkGetBufferMemoryRequirements(device, t.buf, &buf_req);
    const int buf_type = find_memory_type(
        mp, buf_req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0);
    if (buf_type < 0) {
        std::printf("VK13-MULTIVIEW-CANARY: FAIL (no host-visible coherent memory type)\n");
        return false;
    }
    VkMemoryAllocateInfo buf_ai{};
    buf_ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    buf_ai.allocationSize = buf_req.size;
    buf_ai.memoryTypeIndex = static_cast<std::uint32_t>(buf_type);
    if (!check(vkAllocateMemory(device, &buf_ai, nullptr, &t.buf_mem), "vkAllocateMemory(buf)") ||
        !check(vkBindBufferMemory(device, t.buf, t.buf_mem, 0), "vkBindBufferMemory")) {
        return false;
    }
    return true;
}

void destroy_target(VkDevice device, Target& t) {
    if (t.view != VK_NULL_HANDLE)
        vkDestroyImageView(device, t.view, nullptr);
    if (t.buf != VK_NULL_HANDLE)
        vkDestroyBuffer(device, t.buf, nullptr);
    if (t.buf_mem != VK_NULL_HANDLE)
        vkFreeMemory(device, t.buf_mem, nullptr);
    if (t.image != VK_NULL_HANDLE)
        vkDestroyImage(device, t.image, nullptr);
    if (t.img_mem != VK_NULL_HANDLE)
        vkFreeMemory(device, t.img_mem, nullptr);
    t = Target{};
}

// Copy BOTH layers into the readback buffer (layer L -> offset L*kLayerBytes) and assert layer 0 is
// all-red + layer 1 all-green (sampled at the center; the triangle is full-screen so every pixel of
// each layer carries its view's color). The image is already in TRANSFER_SRC_OPTIMAL.
bool copy_and_check(VkDevice device, VkQueue queue, VkCommandPool pool, const char* tag,
                    const Target& t) {
    VkCommandBufferAllocateInfo cbai{};
    cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool = pool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (!check(vkAllocateCommandBuffers(device, &cbai, &cmd), "vkAllocateCommandBuffers(copy)")) {
        return false;
    }
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (!check(vkBeginCommandBuffer(cmd, &bi), "vkBeginCommandBuffer(copy)")) {
        return false;
    }
    VkBufferImageCopy regions[kLayers]{};
    for (std::uint32_t l = 0; l < kLayers; ++l) {
        regions[l].bufferOffset = static_cast<VkDeviceSize>(l) * kLayerBytes;
        regions[l].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, l, 1};
        regions[l].imageExtent = {kDim, kDim, 1};
    }
    vkCmdCopyImageToBuffer(cmd, t.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t.buf, kLayers,
                           regions);
    if (!check(vkEndCommandBuffer(cmd), "vkEndCommandBuffer(copy)")) {
        return false;
    }
    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence = VK_NULL_HANDLE;
    if (!check(vkCreateFence(device, &fci, nullptr, &fence), "vkCreateFence(copy)")) {
        return false;
    }
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    bool ok = check(vkQueueSubmit(queue, 1, &si, fence), "vkQueueSubmit(copy)");
    if (ok) {
        vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
        void* p = nullptr;
        if (check(vkMapMemory(device, t.buf_mem, 0, VK_WHOLE_SIZE, 0, &p), "vkMapMemory")) {
            const auto* px = static_cast<const unsigned char*>(p);
            const VkDeviceSize center = (static_cast<VkDeviceSize>(kDim / 2) * kDim + kDim / 2) * 4;
            unsigned char l0[4];
            unsigned char l1[4];
            std::memcpy(l0, px + center, 4);
            std::memcpy(l1, px + kLayerBytes + center, 4);
            vkUnmapMemory(device, t.buf_mem);
            std::printf("VK13-MULTIVIEW-CANARY: %s layer0=%02x%02x%02x%02x layer1=%02x%02x%02x%02x "
                        "(expect red then green)\n",
                        tag, l0[0], l0[1], l0[2], l0[3], l1[0], l1[1], l1[2], l1[3]);
            const bool l0_red = std::memcmp(l0, kRed, 4) == 0;
            const bool l1_green = std::memcmp(l1, kGreen, 4) == 0;
            ok = l0_red && l1_green;
            if (!ok) {
                std::printf("VK13-MULTIVIEW-CANARY: FAIL (%s per-view color wrong: layer0_red=%d "
                            "layer1_green=%d -- multiview did not run per-view)\n",
                            tag, l0_red ? 1 : 0, l1_green ? 1 : 0);
            }
        } else {
            ok = false;
        }
    }
    vkDestroyFence(device, fence, nullptr);
    return ok;
}

// The shared graphics-pipeline fixed-function state (both paths draw the same full-screen
// triangle).
struct PipeParts {
    VkPipelineShaderStageCreateInfo stages[2]{};
    VkPipelineVertexInputStateCreateInfo vi{};
    VkPipelineInputAssemblyStateCreateInfo ia{};
    VkPipelineViewportStateCreateInfo vp{};
    VkPipelineRasterizationStateCreateInfo rs{};
    VkPipelineMultisampleStateCreateInfo ms{};
    VkPipelineColorBlendAttachmentState cba{};
    VkPipelineColorBlendStateCreateInfo cb{};
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds{};
};

void fill_pipe_parts(PipeParts& p, VkShaderModule vert, VkShaderModule frag) {
    p.stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    p.stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    p.stages[0].module = vert;
    p.stages[0].pName = "main";
    p.stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    p.stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    p.stages[1].module = frag;
    p.stages[1].pName = "main";
    p.vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    p.ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    p.ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    p.vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    p.vp.viewportCount = 1;
    p.vp.scissorCount = 1;
    p.rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    p.rs.polygonMode = VK_POLYGON_MODE_FILL;
    p.rs.cullMode = VK_CULL_MODE_NONE;
    p.rs.frontFace = VK_FRONT_FACE_CLOCKWISE;
    p.rs.lineWidth = 1.0f;
    p.ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    p.ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    p.cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    p.cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    p.cb.attachmentCount = 1;
    p.cb.pAttachments = &p.cba;
    p.ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    p.ds.dynamicStateCount = 2;
    p.ds.pDynamicStates = p.dyn;
}

void set_viewport_scissor(VkCommandBuffer cmd) {
    VkViewport vp{};
    vp.width = static_cast<float>(kDim);
    vp.height = static_cast<float>(kDim);
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &vp);
    VkRect2D sc{};
    sc.extent = {kDim, kDim};
    vkCmdSetScissor(cmd, 0, 1, &sc);
}

// --- Path A: a vkCreateRenderPass2 viewMask render pass + framebuffer. ---
bool run_rp2(VkDevice device, const VkPhysicalDeviceMemoryProperties& mp, VkQueue queue,
             VkCommandPool pool, VkShaderModule vert, VkShaderModule frag) {
    Target t;
    VkRenderPass rp = VK_NULL_HANDLE;
    VkFramebuffer fb = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    bool ok = false;
    if (!make_target(device, mp, t)) {
        goto done;
    }
    {
        // A single-subpass multiview render pass: subpass.viewMask = 0b11. The render pass owns the
        // UNDEFINED -> COLOR_ATTACHMENT -> TRANSFER_SRC layout transition (finalLayout), so no
        // manual barrier is needed for this path.
        VkAttachmentDescription2 att{};
        att.sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2;
        att.format = kColorFormat;
        att.samples = VK_SAMPLE_COUNT_1_BIT;
        att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        att.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        VkAttachmentReference2 ref{};
        ref.sType = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2;
        ref.attachment = 0;
        ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        ref.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        VkSubpassDescription2 sp{};
        sp.sType = VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2;
        sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sp.viewMask = kViewMask; // carried to a real host multiview render pass
        sp.colorAttachmentCount = 1;
        sp.pColorAttachments = &ref;
        VkRenderPassCreateInfo2 rpci{};
        rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2;
        rpci.attachmentCount = 1;
        rpci.pAttachments = &att;
        rpci.subpassCount = 1;
        rpci.pSubpasses = &sp;
        if (!check(vkCreateRenderPass2(device, &rpci, nullptr, &rp), "vkCreateRenderPass2")) {
            goto done;
        }
        VkFramebufferCreateInfo fbci{};
        fbci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fbci.renderPass = rp;
        fbci.attachmentCount = 1;
        fbci.pAttachments = &t.view;
        fbci.width = kDim;
        fbci.height = kDim;
        fbci.layers = 1; // multiview: layers come from the viewMask, not this field
        if (!check(vkCreateFramebuffer(device, &fbci, nullptr, &fb), "vkCreateFramebuffer")) {
            goto done;
        }
        VkPipelineLayoutCreateInfo plci{};
        plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        if (!check(vkCreatePipelineLayout(device, &plci, nullptr, &layout),
                   "vkCreatePipelineLayout")) {
            goto done;
        }
        PipeParts pp;
        fill_pipe_parts(pp, vert, frag);
        VkGraphicsPipelineCreateInfo gpci{};
        gpci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        gpci.stageCount = 2;
        gpci.pStages = pp.stages;
        gpci.pVertexInputState = &pp.vi;
        gpci.pInputAssemblyState = &pp.ia;
        gpci.pViewportState = &pp.vp;
        gpci.pRasterizationState = &pp.rs;
        gpci.pMultisampleState = &pp.ms;
        gpci.pColorBlendState = &pp.cb;
        gpci.pDynamicState = &pp.ds;
        gpci.layout = layout;
        gpci.renderPass = rp; // the pipeline inherits the render pass's viewMask
        gpci.subpass = 0;
        gpci.basePipelineIndex = -1;
        if (!check(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gpci, nullptr, &pipeline),
                   "vkCreateGraphicsPipelines(rp2)")) {
            goto done;
        }
        VkCommandBufferAllocateInfo cbai{};
        cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cbai.commandPool = pool;
        cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbai.commandBufferCount = 1;
        if (!check(vkAllocateCommandBuffers(device, &cbai, &cmd),
                   "vkAllocateCommandBuffers(rp2)")) {
            goto done;
        }
        VkFenceCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        if (!check(vkCreateFence(device, &fci, nullptr, &fence), "vkCreateFence(rp2)")) {
            goto done;
        }
        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (!check(vkBeginCommandBuffer(cmd, &bi), "vkBeginCommandBuffer(rp2)")) {
            goto done;
        }
        VkClearValue clear{};
        clear.color.float32[3] = 1.0f; // clear to opaque black; the triangle overwrites every pixel
        VkRenderPassBeginInfo rpbi{};
        rpbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rpbi.renderPass = rp;
        rpbi.framebuffer = fb;
        rpbi.renderArea.extent = {kDim, kDim};
        rpbi.clearValueCount = 1;
        rpbi.pClearValues = &clear;
        vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        set_viewport_scissor(cmd);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRenderPass(cmd);
        if (!check(vkEndCommandBuffer(cmd), "vkEndCommandBuffer(rp2)")) {
            goto done;
        }
        VkSubmitInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        if (!check(vkQueueSubmit(queue, 1, &si, fence), "vkQueueSubmit(rp2)")) {
            goto done;
        }
        vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
        ok = copy_and_check(device, queue, pool, "RP2", t);
    }
done:
    if (fence != VK_NULL_HANDLE)
        vkDestroyFence(device, fence, nullptr);
    if (pipeline != VK_NULL_HANDLE)
        vkDestroyPipeline(device, pipeline, nullptr);
    if (layout != VK_NULL_HANDLE)
        vkDestroyPipelineLayout(device, layout, nullptr);
    if (fb != VK_NULL_HANDLE)
        vkDestroyFramebuffer(device, fb, nullptr);
    if (rp != VK_NULL_HANDLE)
        vkDestroyRenderPass(device, rp, nullptr);
    destroy_target(device, t);
    return ok;
}

// --- Path B: a vkCmdBeginRenderingKHR viewMask dynamic-rendering scope. ---
bool run_dr(VkDevice device, const VkPhysicalDeviceMemoryProperties& mp, VkQueue queue,
            VkCommandPool pool, VkShaderModule vert, VkShaderModule frag,
            PFN_vkCmdBeginRenderingKHR pfn_begin, PFN_vkCmdEndRenderingKHR pfn_end) {
    Target t;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    bool ok = false;
    if (!make_target(device, mp, t)) {
        goto done;
    }
    {
        VkPipelineLayoutCreateInfo plci{};
        plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        if (!check(vkCreatePipelineLayout(device, &plci, nullptr, &layout),
                   "vkCreatePipelineLayout")) {
            goto done;
        }
        PipeParts pp;
        fill_pipe_parts(pp, vert, frag);
        VkFormat color_format = kColorFormat;
        VkPipelineRenderingCreateInfo pri{};
        pri.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
        pri.viewMask = kViewMask; // DR pipeline multiview
        pri.colorAttachmentCount = 1;
        pri.pColorAttachmentFormats = &color_format;
        VkGraphicsPipelineCreateInfo gpci{};
        gpci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        gpci.pNext = &pri;
        gpci.stageCount = 2;
        gpci.pStages = pp.stages;
        gpci.pVertexInputState = &pp.vi;
        gpci.pInputAssemblyState = &pp.ia;
        gpci.pViewportState = &pp.vp;
        gpci.pRasterizationState = &pp.rs;
        gpci.pMultisampleState = &pp.ms;
        gpci.pColorBlendState = &pp.cb;
        gpci.pDynamicState = &pp.ds;
        gpci.layout = layout;
        gpci.renderPass = VK_NULL_HANDLE; // dynamic rendering
        gpci.subpass = 0;
        gpci.basePipelineIndex = -1;
        if (!check(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gpci, nullptr, &pipeline),
                   "vkCreateGraphicsPipelines(dr)")) {
            goto done;
        }
        VkCommandBufferAllocateInfo cbai{};
        cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cbai.commandPool = pool;
        cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbai.commandBufferCount = 1;
        if (!check(vkAllocateCommandBuffers(device, &cbai, &cmd), "vkAllocateCommandBuffers(dr)")) {
            goto done;
        }
        VkFenceCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        if (!check(vkCreateFence(device, &fci, nullptr, &fence), "vkCreateFence(dr)")) {
            goto done;
        }
        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (!check(vkBeginCommandBuffer(cmd, &bi), "vkBeginCommandBuffer(dr)")) {
            goto done;
        }
        // Dynamic rendering owns its transitions: UNDEFINED -> COLOR_ATTACHMENT (both layers).
        image_barrier(cmd, t.image, VK_IMAGE_LAYOUT_UNDEFINED,
                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0,
                      VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
        VkClearValue clear{};
        clear.color.float32[3] = 1.0f;
        VkRenderingAttachmentInfo color{};
        color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        color.imageView = t.view;
        color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color.clearValue = clear;
        VkRenderingInfo ri{};
        ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        ri.renderArea.extent = {kDim, kDim};
        ri.layerCount = 1;       // multiview: the views drive the layers
        ri.viewMask = kViewMask; // DR viewMask, carried to the host
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &color;
        pfn_begin(cmd, &ri);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        set_viewport_scissor(cmd);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        pfn_end(cmd);
        // COLOR_ATTACHMENT -> TRANSFER_SRC (both layers), then copy_and_check does the copy.
        image_barrier(cmd, t.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                      VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                      VK_PIPELINE_STAGE_TRANSFER_BIT);
        if (!check(vkEndCommandBuffer(cmd), "vkEndCommandBuffer(dr)")) {
            goto done;
        }
        VkSubmitInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        if (!check(vkQueueSubmit(queue, 1, &si, fence), "vkQueueSubmit(dr)")) {
            goto done;
        }
        vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
        ok = copy_and_check(device, queue, pool, "DR", t);
    }
done:
    if (fence != VK_NULL_HANDLE)
        vkDestroyFence(device, fence, nullptr);
    if (pipeline != VK_NULL_HANDLE)
        vkDestroyPipeline(device, pipeline, nullptr);
    if (layout != VK_NULL_HANDLE)
        vkDestroyPipelineLayout(device, layout, nullptr);
    destroy_target(device, t);
    return ok;
}

} // namespace

int main() {
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.apiVersion = VK_API_VERSION_1_2; // multiview is core 1.1; DR rides as a 1.2 extension
    app.pApplicationName = "vkrelay2-vk13-multiview-canary";
    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    VkInstance instance = VK_NULL_HANDLE;
    if (vkCreateInstance(&ici, nullptr, &instance) != VK_SUCCESS) {
        std::printf("VK13-MULTIVIEW-CANARY: SKIP (vkCreateInstance failed -- no ICD/worker?)\n");
        return 0;
    }

    std::uint32_t pd_count = 0;
    vkEnumeratePhysicalDevices(instance, &pd_count, nullptr);
    if (pd_count == 0) {
        std::printf("VK13-MULTIVIEW-CANARY: FAIL (no physical device)\n");
        vkDestroyInstance(instance, nullptr);
        return 2;
    }
    std::vector<VkPhysicalDevice> pds(pd_count);
    vkEnumeratePhysicalDevices(instance, &pd_count, pds.data());
    VkPhysicalDevice phys = pds[0];

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(phys, &props);
    std::printf("VK13-MULTIVIEW-CANARY: device '%s' apiVersion=%u.%u\n", props.deviceName,
                VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion));

    // The multiview feature must be reported (the Vulkan11Features rollup spelling -- the one the
    // relay serves + forwards). A canary that enables a feature the device does not report would be
    // meaningless.
    VkPhysicalDeviceVulkan11Features feat11{};
    feat11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    VkPhysicalDeviceFeatures2 feat2{};
    feat2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    feat2.pNext = &feat11;
    vkGetPhysicalDeviceFeatures2(phys, &feat2);
    std::printf("VK13-MULTIVIEW-CANARY: multiview feature=%d\n", feat11.multiview ? 1 : 0);
    if (!feat11.multiview) {
        std::printf(
            "VK13-MULTIVIEW-CANARY: FAIL (multiview reported FALSE -- required since 1.1)\n");
        vkDestroyInstance(instance, nullptr);
        return 1;
    }

    VkPhysicalDeviceMemoryProperties memprops{};
    vkGetPhysicalDeviceMemoryProperties(phys, &memprops);

    // Device: enable multiview (via the Vulkan11Features rollup) AND VK_KHR_dynamic_rendering (for
    // the DR path). The relay's scalar/chain agreement checks these against the forwarded chain.
    const float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = 0;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    const char* dev_exts[] = {VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME};
    VkPhysicalDeviceDynamicRenderingFeatures enable_dr{};
    enable_dr.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES;
    enable_dr.dynamicRendering = VK_TRUE;
    VkPhysicalDeviceVulkan11Features enable_mv{};
    enable_mv.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    enable_mv.multiview = VK_TRUE;
    enable_mv.pNext = &enable_dr;
    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.pNext = &enable_mv;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = dev_exts;

    int rc = 1;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    VkShaderModule vert = VK_NULL_HANDLE;
    VkShaderModule frag = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    PFN_vkCmdBeginRenderingKHR pfn_begin = nullptr;
    PFN_vkCmdEndRenderingKHR pfn_end = nullptr;
    bool rp2_ok = false;
    bool dr_ok = false;

    if (!check(vkCreateDevice(phys, &dci, nullptr, &device), "vkCreateDevice")) {
        goto teardown;
    }
    vkGetDeviceQueue(device, 0, 0, &queue);
    pfn_begin = reinterpret_cast<PFN_vkCmdBeginRenderingKHR>(
        vkGetDeviceProcAddr(device, "vkCmdBeginRenderingKHR"));
    pfn_end = reinterpret_cast<PFN_vkCmdEndRenderingKHR>(
        vkGetDeviceProcAddr(device, "vkCmdEndRenderingKHR"));
    if (pfn_begin == nullptr || pfn_end == nullptr) {
        std::printf("VK13-MULTIVIEW-CANARY: FAIL (vkCmdBegin/EndRenderingKHR did not resolve)\n");
        goto teardown;
    }
    vert = make_shader(device, vkr::multiview_spv::kVert, vkr::multiview_spv::kVertBytes);
    frag = make_shader(device, vkr::multiview_spv::kFrag, vkr::multiview_spv::kFragBytes);
    if (vert == VK_NULL_HANDLE || frag == VK_NULL_HANDLE) {
        std::printf("VK13-MULTIVIEW-CANARY: FAIL (vkCreateShaderModule)\n");
        goto teardown;
    }
    {
        VkCommandPoolCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = 0;
        if (!check(vkCreateCommandPool(device, &pci, nullptr, &pool), "vkCreateCommandPool")) {
            goto teardown;
        }
    }

    rp2_ok = run_rp2(device, memprops, queue, pool, vert, frag);
    dr_ok = run_dr(device, memprops, queue, pool, vert, frag, pfn_begin, pfn_end);
    if (rp2_ok && dr_ok) {
        std::printf("VK13-MULTIVIEW-CANARY: PASS (multiview served end-to-end: per-view "
                    "gl_ViewIndex color landed on layer 0 (red) + layer 1 (green) through BOTH the "
                    "render-pass2 viewMask path AND the dynamic-rendering viewMask path)\n");
        rc = 0;
    } else {
        std::printf("VK13-MULTIVIEW-CANARY: FAIL (rp2_ok=%d dr_ok=%d)\n", rp2_ok ? 1 : 0,
                    dr_ok ? 1 : 0);
        rc = 1;
    }

teardown:
    if (pool != VK_NULL_HANDLE)
        vkDestroyCommandPool(device, pool, nullptr);
    if (frag != VK_NULL_HANDLE)
        vkDestroyShaderModule(device, frag, nullptr);
    if (vert != VK_NULL_HANDLE)
        vkDestroyShaderModule(device, vert, nullptr);
    if (device != VK_NULL_HANDLE)
        vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);
    return rc;
}
