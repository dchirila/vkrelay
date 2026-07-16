// vkrelay2 Vulkan-1.3 dynamic-rendering canary -- the native lane + VK_KHR_dynamic_rendering.
//
// This native-Vulkan canary runs a surfaceless Vulkan program
// through the real loader + the session-pinned vkrelay2 ICD on the NATIVE lane
// (vkrun --frontend vulkan13 -> VKRELAY2_NATIVE_LANE=1). It proves, end to end on
// the real GPU, that the steering-safe native lane serves VK_KHR_dynamic_rendering: a
// NULL-renderpass pipeline (with a VkPipelineRenderingCreateInfo) plus vkCmdBeginRenderingKHR /
// vkCmdEndRenderingKHR bracketing a draw actually reach the host and produce the right pixels --
// not merely that a proc addr resolves. Its assertions:
//   - the device reports apiVersion >= 1.2 (DR rode as an EXTENSION on the 1.2 device; the
//     required-feature audit then flipped the native lane to a conformant 1.3 on
//     a 1.3-capable host -- where DR is CORE, still reachable via its KHR alias, which this
//     exercises);
//   - VK_KHR_dynamic_rendering + VK_KHR_synchronization2 are PRESENT in the device extensions
//   (their
//     KHR aliases stay advertised even at 1.3);
//   - VkPhysicalDeviceDynamicRenderingFeatures.dynamicRendering == TRUE (the lane-gated feature
//   mask
//     reports TRUE only where the surface is wired); the 1.3 rollup's maintenance4 is version-
//     adaptive (FALSE at 1.2, TRUE once Vulkan 1.3 support serves the full required matrix);
//   - a POSITIVE DR draw: an offscreen image is transitioned to COLOR_ATTACHMENT via an explicit
//     barrier, vkCmdBeginRenderingKHR clears it to a known color (loadOp CLEAR) and a bufferless
//     triangle is drawn, then vkCmdEndRenderingKHR + a barrier to TRANSFER_SRC + a copy-to-buffer.
//     The CORNER pixel MUST equal the clear color (the load-op clear landed under dynamic
//     rendering) and the CENTER pixel MUST differ from it (the triangle drew inside the
//     dynamically-begun rendering, through the NULL-renderpass pipeline). Together they prove begin
//     + pipeline + draw + end end to end.
// The steering-intact half (a DEFAULT/zink run still sees 1.2 and NO dynamic_rendering) is asserted
// by the smoke, not this canary (it only runs on the native lane).
//
// Greppable markers on stdout; run_vk13_dynrender_smoke.sh gates on them. Skips cleanly (exit 0)
// when no ICD/worker stack is reachable; FAILs (nonzero) on a real regression.

#include <vulkan/vulkan.h>

#include "tests/triangle_spv.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

constexpr std::uint32_t kDim = 256;
constexpr VkDeviceSize kBufBytes = static_cast<VkDeviceSize>(kDim) * kDim * 4;
constexpr VkFormat kColorFormat = VK_FORMAT_R8G8B8A8_UNORM;

bool check(VkResult r, const char* what) {
    if (r != VK_SUCCESS) {
        std::printf("VK13-DYNREND-CANARY: FAIL (%s -> VkResult %d)\n", what, static_cast<int>(r));
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

// One color-aspect image-layout barrier over the whole single subresource (app_image_barrier_ok
// forwards it faithfully). Dynamic rendering does no automatic transitions, so the app owns them.
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
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

} // namespace

int main() {
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    // Declare 1.2: DR rides as an EXTENSION on a 1.2-reported device (the 1.3 bump is Vulkan 1.3
    // support). A native app targeting DR-on-1.2 uses the *KHR entrypoints, exactly as here.
    app.apiVersion = VK_API_VERSION_1_2;
    app.pApplicationName = "vkrelay2-vk13-dynrender-canary";
    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    VkInstance instance = VK_NULL_HANDLE;
    if (vkCreateInstance(&ici, nullptr, &instance) != VK_SUCCESS) {
        std::printf("VK13-DYNREND-CANARY: SKIP (vkCreateInstance failed -- no ICD/worker?)\n");
        return 0;
    }

    std::uint32_t pd_count = 0;
    vkEnumeratePhysicalDevices(instance, &pd_count, nullptr);
    if (pd_count == 0) {
        std::printf("VK13-DYNREND-CANARY: FAIL (no physical device)\n");
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
    std::printf("VK13-DYNREND-CANARY: device '%s' apiVersion=%u.%u\n", props.deviceName, maj, min);

    // --- Version proof: DR is available on the native lane. It rode as an EXTENSION on a 1.2
    // device through the whole DR bring-up; the required-feature audit then flipped
    // the native lane to a conformant 1.3 (multiview served), so the device now honestly
    // reports 1.3 on a 1.3-capable host -- where DR is CORE (still reachable via its KHR alias,
    // which this canary exercises). Accept >= 1.2; full13 is the full Vulkan 1.3 state that also
    // serves maintenance4 below. ---
    const bool full13 = (maj > 1) || (maj == 1 && min >= 3);
    if (!(maj == 1 && min >= 2)) {
        std::printf(
            "VK13-DYNREND-CANARY: FAIL (device apiVersion %u.%u, expected >= 1.2 for the DR "
            "surface)\n",
            maj, min);
        vkDestroyInstance(instance, nullptr);
        return 1;
    }

    // --- Extension exposure: dynamic_rendering present (native lane). synchronization2 is ALSO
    // present now that the relay serves it (this probe flipped from negative when sync2 landed --
    // an intentional, flagged edit, exactly as DR flipped the EDS canary's dynamic_rendering
    // probe). ---
    std::uint32_t ext_count = 0;
    vkEnumerateDeviceExtensionProperties(phys, nullptr, &ext_count, nullptr);
    std::vector<VkExtensionProperties> exts(ext_count);
    vkEnumerateDeviceExtensionProperties(phys, nullptr, &ext_count, exts.data());
    const bool dynr = has_ext(exts, VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME);
    const bool sync2 = has_ext(exts, VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME);
    std::printf("VK13-DYNREND-CANARY: extensions dynamic_rendering=%d synchronization2=%d\n",
                dynr ? 1 : 0, sync2 ? 1 : 0);
    if (!dynr) {
        std::printf("VK13-DYNREND-CANARY: FAIL (VK_KHR_dynamic_rendering not advertised on the "
                    "native lane)\n");
        vkDestroyInstance(instance, nullptr);
        return 1;
    }
    if (!sync2) {
        std::printf("VK13-DYNREND-CANARY: FAIL (VK_KHR_synchronization2 not advertised on the "
                    "native lane -- the relay serves it)\n");
        vkDestroyInstance(instance, nullptr);
        return 1;
    }

    // --- Feature mask: dynamicRendering reported TRUE on the native lane. ---
    VkPhysicalDeviceDynamicRenderingFeatures dr_feat{};
    dr_feat.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES;
    VkPhysicalDeviceFeatures2 feat2{};
    feat2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    feat2.pNext = &dr_feat;
    vkGetPhysicalDeviceFeatures2(phys, &feat2);
    std::printf("VK13-DYNREND-CANARY: dynamicRendering feature=%d\n",
                dr_feat.dynamicRendering ? 1 : 0);
    if (!dr_feat.dynamicRendering) {
        std::printf("VK13-DYNREND-CANARY: FAIL (dynamicRendering reported FALSE on the native "
                    "lane)\n");
        vkDestroyInstance(instance, nullptr);
        return 1;
    }

    // --- Rollup mask: the Vulkan 1.3 ROLLUP feature struct reports dynamicRendering AND
    // synchronization2 TRUE. maintenance4 is version-adaptive: FALSE while the native
    // lane honestly reports 1.2 (unwired / a non-1.3 host), TRUE once Vulkan 1.3 support makes
    // it a conformant 1.3 device -- maintenance4 is a REQUIRED 1.3 feature, so the vouch serves it
    // exactly when the version bumps. This exercises the icd forward_capability_chain rollup mask +
    // the Vulkan 1.3 canary's mask-lift on the real GPU. ---
    VkPhysicalDeviceVulkan13Features feat13{};
    feat13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    VkPhysicalDeviceFeatures2 feat2b{};
    feat2b.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    feat2b.pNext = &feat13;
    vkGetPhysicalDeviceFeatures2(phys, &feat2b);
    std::printf(
        "VK13-DYNREND-CANARY: rollup dynamicRendering=%d synchronization2=%d maintenance4=%d "
        "(full13=%d)\n",
        feat13.dynamicRendering ? 1 : 0, feat13.synchronization2 ? 1 : 0,
        feat13.maintenance4 ? 1 : 0, full13 ? 1 : 0);
    if (!feat13.dynamicRendering || !feat13.synchronization2 ||
        (feat13.maintenance4 != VK_FALSE) != full13) {
        std::printf("VK13-DYNREND-CANARY: FAIL (Vulkan 1.3 rollup wrong -- expected "
                    "dynamicRendering=1, synchronization2=1, maintenance4=%d at this version)\n",
                    full13 ? 1 : 0);
        vkDestroyInstance(instance, nullptr);
        return 1;
    }

    VkPhysicalDeviceMemoryProperties memprops{};
    vkGetPhysicalDeviceMemoryProperties(phys, &memprops);

    // --- Device with VK_KHR_dynamic_rendering enabled (extension + the feature chained). ---
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
    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.pNext = &enable_dr;
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
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkShaderModule vert = VK_NULL_HANDLE;
    VkShaderModule frag = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    PFN_vkCmdBeginRenderingKHR pfn_begin = nullptr;
    PFN_vkCmdEndRenderingKHR pfn_end = nullptr;

    if (!check(vkCreateDevice(phys, &dci, nullptr, &device), "vkCreateDevice")) {
        goto teardown;
    }
    vkGetDeviceQueue(device, 0, 0, &queue);

    // The *KHR entrypoints must resolve AND be called (proc-addr != null is not a proof on its
    // own).
    pfn_begin = reinterpret_cast<PFN_vkCmdBeginRenderingKHR>(
        vkGetDeviceProcAddr(device, "vkCmdBeginRenderingKHR"));
    pfn_end = reinterpret_cast<PFN_vkCmdEndRenderingKHR>(
        vkGetDeviceProcAddr(device, "vkCmdEndRenderingKHR"));
    if (pfn_begin == nullptr || pfn_end == nullptr) {
        std::printf("VK13-DYNREND-CANARY: FAIL (vkCmdBegin/EndRenderingKHR did not resolve on the "
                    "native lane)\n");
        goto teardown;
    }

    {
        // Offscreen color target (COLOR_ATTACHMENT + TRANSFER_SRC for readback), device-local.
        VkImageCreateInfo imgci{};
        imgci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imgci.imageType = VK_IMAGE_TYPE_2D;
        imgci.format = kColorFormat;
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
            std::printf("VK13-DYNREND-CANARY: FAIL (no device-local memory type)\n");
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
        ivci.format = kColorFormat;
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
            std::printf("VK13-DYNREND-CANARY: FAIL (no host-visible coherent memory type)\n");
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

        vert = make_shader(device, vkr::triangle_spv::kVert, vkr::triangle_spv::kVertBytes);
        frag = make_shader(device, vkr::triangle_spv::kFrag, vkr::triangle_spv::kFragBytes);
        if (vert == VK_NULL_HANDLE || frag == VK_NULL_HANDLE) {
            std::printf("VK13-DYNREND-CANARY: FAIL (vkCreateShaderModule)\n");
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
        rs.cullMode = VK_CULL_MODE_NONE; // draw regardless of winding
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
        VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo ds{};
        ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        ds.dynamicStateCount = 2;
        ds.pDynamicStates = dyn;
        // THE dynamic-rendering pipeline: renderPass == VK_NULL_HANDLE + a
        // VkPipelineRenderingCreate Info declaring the color format. Without it the NVIDIA driver
        // segfaults; with it the host builds a NULL-renderpass pipeline the begin/end-rendering
        // commands drive.
        VkFormat color_format = kColorFormat;
        VkPipelineRenderingCreateInfo pri{};
        pri.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
        pri.colorAttachmentCount = 1;
        pri.pColorAttachmentFormats = &color_format;
        VkGraphicsPipelineCreateInfo gpci{};
        gpci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        gpci.pNext = &pri;
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
        gpci.renderPass = VK_NULL_HANDLE; // dynamic rendering
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

        // The clear color the corner must equal (blue); the triangle draws a different color at the
        // center. Distinctive so an all-black failure (nothing drew / cleared) is unambiguous.
        VkClearValue clear{};
        clear.color.float32[0] = 0.0f;
        clear.color.float32[1] = 0.0f;
        clear.color.float32[2] = 1.0f;
        clear.color.float32[3] = 1.0f;
        const unsigned char clear_bytes[4] = {0, 0, 255, 255};

        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (!check(vkBeginCommandBuffer(cmd, &bi), "vkBeginCommandBuffer")) {
            goto teardown;
        }
        // UNDEFINED -> COLOR_ATTACHMENT_OPTIMAL (dynamic rendering does no automatic transition).
        image_barrier(cmd, image, VK_IMAGE_LAYOUT_UNDEFINED,
                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0,
                      VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

        VkRenderingAttachmentInfo color{};
        color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        color.imageView = view;
        color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color.clearValue = clear;
        VkRenderingInfo ri{};
        ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        ri.renderArea.extent = {kDim, kDim};
        ri.layerCount = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &color;
        pfn_begin(cmd, &ri); // vkCmdBeginRenderingKHR -- the command under test
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        VkViewport viewport{};
        viewport.width = static_cast<float>(kDim);
        viewport.height = static_cast<float>(kDim);
        viewport.maxDepth = 1.0f;
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        VkRect2D scissor{};
        scissor.extent = {kDim, kDim};
        vkCmdSetScissor(cmd, 0, 1, &scissor);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        pfn_end(cmd); // vkCmdEndRenderingKHR
        // COLOR_ATTACHMENT_OPTIMAL -> TRANSFER_SRC_OPTIMAL, then copy out.
        image_barrier(cmd, image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                      VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                      VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {kDim, kDim, 1};
        vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &region);
        if (!check(vkEndCommandBuffer(cmd), "vkEndCommandBuffer")) {
            goto teardown;
        }
        VkSubmitInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        if (!check(vkQueueSubmit(queue, 1, &si, fence), "vkQueueSubmit")) {
            goto teardown;
        }
        vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);

        void* p = nullptr;
        if (!check(vkMapMemory(device, buf_mem, 0, VK_WHOLE_SIZE, 0, &p), "vkMapMemory")) {
            goto teardown;
        }
        const auto* pixels = static_cast<const unsigned char*>(p);
        unsigned char center[4];
        unsigned char corner[4];
        const VkDeviceSize center_off = (static_cast<VkDeviceSize>(kDim / 2) * kDim + kDim / 2) * 4;
        std::memcpy(center, pixels + center_off, 4);
        std::memcpy(corner, pixels + 0, 4); // the (0,0) corner the triangle does not cover
        vkUnmapMemory(device, buf_mem);

        std::printf("VK13-DYNREND-CANARY: center=%02x%02x%02x%02x corner=%02x%02x%02x%02x "
                    "clear=%02x%02x%02x%02x\n",
                    center[0], center[1], center[2], center[3], corner[0], corner[1], corner[2],
                    corner[3], clear_bytes[0], clear_bytes[1], clear_bytes[2], clear_bytes[3]);
        const bool corner_is_clear = std::memcmp(corner, clear_bytes, 4) == 0;
        const bool center_drew = std::memcmp(center, clear_bytes, 4) != 0 &&
                                 (center[0] != 0 || center[1] != 0 || center[2] != 0);
        if (corner_is_clear && center_drew) {
            std::printf("VK13-DYNREND-CANARY: PASS (native lane dynamic rendering: NULL-renderpass "
                        "pipeline + vkCmdBegin/EndRenderingKHR clear+draw reached the host)\n");
            rc = 0;
        } else {
            std::printf("VK13-DYNREND-CANARY: FAIL (dynamic rendering did not produce the expected "
                        "pixels: corner_is_clear=%d center_drew=%d)\n",
                        corner_is_clear ? 1 : 0, center_drew ? 1 : 0);
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
