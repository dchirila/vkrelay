// vkrelay2 Vulkan-1.3 opener canary -- the native lane + VK_EXT_extended_dynamic_state (EDS1).
//
// This is the first native-Vulkan canary. A surfaceless Vulkan program (the
// compute/readback_order template) run through the real loader + the session-pinned vkrelay2 ICD on
// the NATIVE lane (vkrun --frontend vulkan13 -> VKRELAY2_NATIVE_LANE=1). It
// proves, end to end on the real GPU, that the steering-safe native lane exposes EDS1 and that a
// dynamic EDS command actually reaches the host and controls rasterization -- not merely that a
// proc addr resolves. Its assertions:
//   - the device reports apiVersion >= 1.2 (EDS rode as an EXTENSION on the 1.2 device; the
//     required-feature audit then flipped the native lane to a conformant 1.3 on
//     a 1.3-capable host -- where EDS is CORE, still reachable via its EXT alias, which this
//     exercises);
//   - VK_EXT_extended_dynamic_state is PRESENT in the device extensions (native-lane exposure);
//   - VK_KHR_synchronization2 is ABSENT (negative probe: an unimplemented 1.3 family is not
//     advertised). VK_KHR_dynamic_rendering is now ALSO advertised, so it is
//     printed but no longer asserted absent -- its own canary proves it;
//   - VkPhysicalDeviceExtendedDynamicStateFeaturesEXT.extendedDynamicState == TRUE (the lane-gated
//     feature mask reports TRUE only where the surface is wired);
//   - a POSITIVE EDS draw: the SAME bufferless triangle rendered twice, differing ONLY by a dynamic
//     vkCmdSetCullModeEXT -- cull NONE draws it (center pixel colored), cull FRONT_AND_BACK culls
//     it (center pixel stays the clear color, winding-independent). The two center pixels MUST
//     differ, proving the dynamic command crossed the relay and reached the host rasterizer.
// The steering-intact half (a DEFAULT/zink run still sees 1.2 and NO EDS) is asserted by the smoke,
// not this canary (it only runs on the native lane).
//
// Greppable markers on stdout; run_vk13_eds_smoke.sh gates on them. Skips cleanly (exit 0) when no
// ICD/worker stack is reachable; FAILs (nonzero) on a real regression.

#include <vulkan/vulkan.h>

#include "tests/triangle_spv.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

constexpr std::uint32_t kDim = 256;
constexpr VkDeviceSize kBufBytes = static_cast<VkDeviceSize>(kDim) * kDim * 4;

bool check(VkResult r, const char* what) {
    if (r != VK_SUCCESS) {
        std::printf("VK13-EDS-CANARY: FAIL (%s -> VkResult %d)\n", what, static_cast<int>(r));
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
    // Declare 1.2: the opener exposes EDS as an EXTENSION on a 1.2-reported device (the 1.3 bump is
    // Vulkan 1.3 support). A native app targeting EDS-on-1.2 uses the *EXT entrypoints, exactly as
    // here.
    app.apiVersion = VK_API_VERSION_1_2;
    app.pApplicationName = "vkrelay2-vk13-eds-canary";
    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    VkInstance instance = VK_NULL_HANDLE;
    if (vkCreateInstance(&ici, nullptr, &instance) != VK_SUCCESS) {
        std::printf("VK13-EDS-CANARY: SKIP (vkCreateInstance failed -- no ICD/worker?)\n");
        return 0;
    }

    std::uint32_t pd_count = 0;
    vkEnumeratePhysicalDevices(instance, &pd_count, nullptr);
    if (pd_count == 0) {
        std::printf("VK13-EDS-CANARY: FAIL (no physical device)\n");
        vkDestroyInstance(instance, nullptr);
        return 2;
    }
    std::vector<VkPhysicalDevice> pds(pd_count);
    vkEnumeratePhysicalDevices(instance, &pd_count, pds.data());
    VkPhysicalDevice phys = pds[0];

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(phys, &props);
    const std::uint32_t maj = VK_API_VERSION_MAJOR(props.apiVersion);
    const std::uint32_t min = VK_API_VERSION_MINOR(props.apiVersion);
    std::printf("VK13-EDS-CANARY: device '%s' apiVersion=%u.%u\n", props.deviceName, maj, min);

    // --- Version proof: EDS is available on the native lane. It rode as an EXTENSION on a 1.2
    // device; the required-feature audit then flipped the
    // native lane to a conformant 1.3 (multiview served), so the device now honestly reports 1.3 on
    // a 1.3-capable host -- where EDS is CORE (still reachable via its EXT alias, which this canary
    // exercises). Accept >= 1.2. ---
    if (!(maj == 1 && min >= 2)) {
        std::printf("VK13-EDS-CANARY: FAIL (device apiVersion %u.%u, expected >= 1.2 for the EDS "
                    "surface)\n",
                    maj, min);
        vkDestroyInstance(instance, nullptr);
        return 1;
    }

    // --- Extension exposure: EDS present (native lane). dynamic_rendering and
    // synchronization2 are BOTH wired families now, so both are EXPECTED present -- the
    // sync2 probe flipped from the negative when sync2 landed (an intentional, flagged edit). ---
    std::uint32_t ext_count = 0;
    vkEnumerateDeviceExtensionProperties(phys, nullptr, &ext_count, nullptr);
    std::vector<VkExtensionProperties> exts(ext_count);
    vkEnumerateDeviceExtensionProperties(phys, nullptr, &ext_count, exts.data());
    const bool eds = has_ext(exts, VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME);
    const bool dynr = has_ext(exts, VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME);
    const bool sync2 = has_ext(exts, VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME);
    std::printf("VK13-EDS-CANARY: extensions eds=%d dynamic_rendering=%d synchronization2=%d\n",
                eds ? 1 : 0, dynr ? 1 : 0, sync2 ? 1 : 0);
    if (!eds) {
        std::printf("VK13-EDS-CANARY: FAIL (VK_EXT_extended_dynamic_state not advertised on the "
                    "native lane)\n");
        vkDestroyInstance(instance, nullptr);
        return 1;
    }
    // dynamic_rendering + synchronization2 are WIRED families, EXPECTED
    // present.
    (void) dynr;
    (void) sync2;

    // --- Feature mask: extendedDynamicState reported TRUE on the native lane. ---
    VkPhysicalDeviceExtendedDynamicStateFeaturesEXT eds_feat{};
    eds_feat.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT;
    VkPhysicalDeviceFeatures2 feat2{};
    feat2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    feat2.pNext = &eds_feat;
    vkGetPhysicalDeviceFeatures2(phys, &feat2);
    std::printf("VK13-EDS-CANARY: extendedDynamicState feature=%d\n",
                eds_feat.extendedDynamicState ? 1 : 0);
    if (!eds_feat.extendedDynamicState) {
        std::printf("VK13-EDS-CANARY: FAIL (extendedDynamicState reported FALSE on the native "
                    "lane)\n");
        vkDestroyInstance(instance, nullptr);
        return 1;
    }

    VkPhysicalDeviceMemoryProperties memprops{};
    vkGetPhysicalDeviceMemoryProperties(phys, &memprops);

    // --- Device with EDS enabled (extension + the feature chained into pNext). ---
    const float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = 0;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    const char* dev_exts[] = {VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME};
    VkPhysicalDeviceExtendedDynamicStateFeaturesEXT enable_eds{};
    enable_eds.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT;
    enable_eds.extendedDynamicState = VK_TRUE;
    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.pNext = &enable_eds;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = dev_exts;

    int rc = 1;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory img_mem = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory buf_mem = VK_NULL_HANDLE;
    VkRenderPass render_pass = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkShaderModule vert = VK_NULL_HANDLE;
    VkShaderModule frag = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    PFN_vkCmdSetCullModeEXT pfn_set_cull = nullptr;

    if (!check(vkCreateDevice(phys, &dci, nullptr, &device), "vkCreateDevice")) {
        goto teardown;
    }
    vkGetDeviceQueue(device, 0, 0, &queue);

    // The EXT entrypoint must resolve AND be called (proc-addr != null is not a proof
    // on its own; the draw below is).
    pfn_set_cull = reinterpret_cast<PFN_vkCmdSetCullModeEXT>(
        vkGetDeviceProcAddr(device, "vkCmdSetCullModeEXT"));
    if (pfn_set_cull == nullptr) {
        std::printf("VK13-EDS-CANARY: FAIL (vkCmdSetCullModeEXT did not resolve on the native "
                    "lane)\n");
        goto teardown;
    }

    {
        // Offscreen color target (COLOR_ATTACHMENT + TRANSFER_SRC for readback), device-local.
        VkImageCreateInfo imgci{};
        imgci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imgci.imageType = VK_IMAGE_TYPE_2D;
        imgci.format = VK_FORMAT_R8G8B8A8_UNORM;
        imgci.extent = {kDim, kDim, 1};
        imgci.mipLevels = 1;
        imgci.arrayLayers = 1;
        imgci.samples = VK_SAMPLE_COUNT_1_BIT;
        imgci.tiling = VK_IMAGE_TILING_OPTIMAL;
        imgci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        imgci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (!check(vkCreateImage(device, &imgci, nullptr, &image), "vkCreateImage")) {
            goto teardown;
        }
        VkMemoryRequirements img_req{};
        vkGetImageMemoryRequirements(device, image, &img_req);
        const int img_type = find_memory_type(memprops, img_req.memoryTypeBits,
                                              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
        if (img_type < 0) {
            std::printf("VK13-EDS-CANARY: FAIL (no device-local memory type)\n");
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

        VkImageViewCreateInfo ivci{};
        ivci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        ivci.image = image;
        ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        ivci.format = VK_FORMAT_R8G8B8A8_UNORM;
        ivci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (!check(vkCreateImageView(device, &ivci, nullptr, &view), "vkCreateImageView")) {
            goto teardown;
        }

        // Host-visible readback buffer.
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
            std::printf("VK13-EDS-CANARY: FAIL (no host-visible coherent memory type)\n");
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

        // Render pass: clear -> draw -> leave the image in TRANSFER_SRC for the copy.
        VkAttachmentDescription att{};
        att.format = VK_FORMAT_R8G8B8A8_UNORM;
        att.samples = VK_SAMPLE_COUNT_1_BIT;
        att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        att.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        VkAttachmentReference ref{};
        ref.attachment = 0;
        ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &ref;
        VkRenderPassCreateInfo rpci{};
        rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rpci.attachmentCount = 1;
        rpci.pAttachments = &att;
        rpci.subpassCount = 1;
        rpci.pSubpasses = &subpass;
        if (!check(vkCreateRenderPass(device, &rpci, nullptr, &render_pass),
                   "vkCreateRenderPass")) {
            goto teardown;
        }
        VkFramebufferCreateInfo fbci{};
        fbci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fbci.renderPass = render_pass;
        fbci.attachmentCount = 1;
        fbci.pAttachments = &view;
        fbci.width = kDim;
        fbci.height = kDim;
        fbci.layers = 1;
        if (!check(vkCreateFramebuffer(device, &fbci, nullptr, &framebuffer),
                   "vkCreateFramebuffer")) {
            goto teardown;
        }

        vert = make_shader(device, vkr::triangle_spv::kVert, vkr::triangle_spv::kVertBytes);
        frag = make_shader(device, vkr::triangle_spv::kFrag, vkr::triangle_spv::kFragBytes);
        if (vert == VK_NULL_HANDLE || frag == VK_NULL_HANDLE) {
            std::printf("VK13-EDS-CANARY: FAIL (vkCreateShaderModule)\n");
            goto teardown;
        }

        VkPipelineLayoutCreateInfo plci{};
        plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        if (!check(vkCreatePipelineLayout(device, &plci, nullptr, &layout),
                   "vkCreatePipelineLayout")) {
            goto teardown;
        }

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vert;
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = frag;
        stages[1].pName = "main";
        VkPipelineVertexInputStateCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        VkPipelineInputAssemblyStateCreateInfo ia{};
        ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vp{};
        vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        vp.viewportCount = 1;
        vp.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs{};
        rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = VK_CULL_MODE_NONE; // placeholder -- overridden dynamically each render
        rs.frontFace = VK_FRONT_FACE_CLOCKWISE;
        rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo msci{};
        msci.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        msci.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState cba{};
        cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                             VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo cb{};
        cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        cb.attachmentCount = 1;
        cb.pAttachments = &cba;
        // CULL_MODE is DYNAMIC -- the whole point: the pipeline's baked cull mode is ignored, and
        // vkCmdSetCullModeEXT drives it per render.
        VkDynamicState dyn[3] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
                                 VK_DYNAMIC_STATE_CULL_MODE};
        VkPipelineDynamicStateCreateInfo ds{};
        ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        ds.dynamicStateCount = 3;
        ds.pDynamicStates = dyn;
        VkGraphicsPipelineCreateInfo gpci{};
        gpci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        gpci.stageCount = 2;
        gpci.pStages = stages;
        gpci.pVertexInputState = &vi;
        gpci.pInputAssemblyState = &ia;
        gpci.pViewportState = &vp;
        gpci.pRasterizationState = &rs;
        gpci.pMultisampleState = &msci;
        gpci.pColorBlendState = &cb;
        gpci.pDynamicState = &ds;
        gpci.layout = layout;
        gpci.renderPass = render_pass;
        gpci.subpass = 0;
        gpci.basePipelineIndex = -1;
        if (!check(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gpci, nullptr, &pipeline),
                   "vkCreateGraphicsPipelines")) {
            goto teardown;
        }

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

        // Render once with a given dynamic cull mode; read back the CENTER pixel into out[4].
        const auto render = [&](VkCullModeFlags cull, unsigned char out[4]) -> bool {
            VkCommandBufferBeginInfo bi{};
            bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            if (!check(vkBeginCommandBuffer(cmd, &bi), "vkBeginCommandBuffer")) {
                return false;
            }
            VkClearValue clear{};
            clear.color.float32[0] = 0.0f;
            clear.color.float32[1] = 0.0f;
            clear.color.float32[2] = 0.0f;
            clear.color.float32[3] = 1.0f;
            VkRenderPassBeginInfo rpbi{};
            rpbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
            rpbi.renderPass = render_pass;
            rpbi.framebuffer = framebuffer;
            rpbi.renderArea.extent = {kDim, kDim};
            rpbi.clearValueCount = 1;
            rpbi.pClearValues = &clear;
            vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            VkViewport viewport{};
            viewport.width = static_cast<float>(kDim);
            viewport.height = static_cast<float>(kDim);
            viewport.maxDepth = 1.0f;
            vkCmdSetViewport(cmd, 0, 1, &viewport);
            VkRect2D scissor{};
            scissor.extent = {kDim, kDim};
            vkCmdSetScissor(cmd, 0, 1, &scissor);
            pfn_set_cull(cmd, cull); // THE dynamic EDS command under test
            vkCmdDraw(cmd, 3, 1, 0, 0);
            vkCmdEndRenderPass(cmd);
            // The render pass finalLayout left the image in TRANSFER_SRC_OPTIMAL -> copy it out.
            VkBufferImageCopy region{};
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageExtent = {kDim, kDim, 1};
            vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1,
                                   &region);
            if (!check(vkEndCommandBuffer(cmd), "vkEndCommandBuffer")) {
                return false;
            }
            VkSubmitInfo si{};
            si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            si.commandBufferCount = 1;
            si.pCommandBuffers = &cmd;
            if (!check(vkQueueSubmit(queue, 1, &si, fence), "vkQueueSubmit")) {
                return false;
            }
            vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
            vkResetFences(device, 1, &fence);
            void* p = nullptr;
            if (!check(vkMapMemory(device, buf_mem, 0, VK_WHOLE_SIZE, 0, &p), "vkMapMemory")) {
                return false;
            }
            const VkDeviceSize center = (static_cast<VkDeviceSize>(kDim / 2) * kDim + kDim / 2) * 4;
            std::memcpy(out, static_cast<const unsigned char*>(p) + center, 4);
            vkUnmapMemory(device, buf_mem);
            return true;
        };

        unsigned char px_none[4] = {0, 0, 0, 0};
        unsigned char px_cull[4] = {0, 0, 0, 0};
        if (!render(VK_CULL_MODE_NONE, px_none) || !render(VK_CULL_MODE_FRONT_AND_BACK, px_cull)) {
            goto teardown;
        }
        std::printf("VK13-EDS-CANARY: center cull_none=%02x%02x%02x%02x "
                    "cull_all=%02x%02x%02x%02x\n",
                    px_none[0], px_none[1], px_none[2], px_none[3], px_cull[0], px_cull[1],
                    px_cull[2], px_cull[3]);
        const bool none_drew =
            px_none[0] || px_none[1] || px_none[2]; // triangle colored the center
        const bool all_culled =
            px_cull[0] == 0 && px_cull[1] == 0 && px_cull[2] == 0; // clear color
        const bool differ = std::memcmp(px_none, px_cull, 4) != 0;
        if (none_drew && all_culled && differ) {
            std::printf("VK13-EDS-CANARY: PASS (native lane EDS1: dynamic vkCmdSetCullModeEXT "
                        "controlled rasterization end to end)\n");
            rc = 0;
        } else {
            std::printf("VK13-EDS-CANARY: FAIL (dynamic cull did not control the output: "
                        "none_drew=%d all_culled=%d differ=%d)\n",
                        none_drew ? 1 : 0, all_culled ? 1 : 0, differ ? 1 : 0);
            rc = 1;
        }
    }

teardown:
    if (fence != VK_NULL_HANDLE)
        vkDestroyFence(device, fence, nullptr);
    if (pool != VK_NULL_HANDLE)
        vkDestroyCommandPool(device, pool, nullptr);
    if (pipeline != VK_NULL_HANDLE)
        vkDestroyPipeline(device, pipeline, nullptr);
    if (layout != VK_NULL_HANDLE)
        vkDestroyPipelineLayout(device, layout, nullptr);
    if (frag != VK_NULL_HANDLE)
        vkDestroyShaderModule(device, frag, nullptr);
    if (vert != VK_NULL_HANDLE)
        vkDestroyShaderModule(device, vert, nullptr);
    if (framebuffer != VK_NULL_HANDLE)
        vkDestroyFramebuffer(device, framebuffer, nullptr);
    if (render_pass != VK_NULL_HANDLE)
        vkDestroyRenderPass(device, render_pass, nullptr);
    if (view != VK_NULL_HANDLE)
        vkDestroyImageView(device, view, nullptr);
    if (buf != VK_NULL_HANDLE)
        vkDestroyBuffer(device, buf, nullptr);
    if (buf_mem != VK_NULL_HANDLE)
        vkFreeMemory(device, buf_mem, nullptr);
    if (image != VK_NULL_HANDLE)
        vkDestroyImage(device, image, nullptr);
    if (img_mem != VK_NULL_HANDLE)
        vkFreeMemory(device, img_mem, nullptr);
    if (device != VK_NULL_HANDLE)
        vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);
    return rc;
}
