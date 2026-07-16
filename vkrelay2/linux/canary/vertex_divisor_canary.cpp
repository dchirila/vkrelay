// vkrelay2 vertex-attr-divisor canary: create-only proof that a graphics pipeline whose
// vertex-input state chains VkPipelineVertexInputDivisorStateCreateInfoEXT
// (VK_EXT_vertex_attribute_divisor) -- the exact thing zink emits for instanced attributes, which
// Blender/zink hit -- creates on the real GPU through the relay. RED before the fix (the ICD
// rejected any vertex-input pNext); GREEN after.
//
// Deterministic + headless (no surface/swapchain/draw): the regression under test is pNext
// admission + RPC carry + worker rebuild + real vkCreateGraphicsPipelines, so a create-only
// pipeline with TWO bindings (0 = VERTEX-rate, 1 = INSTANCE-rate divisor 2) is the whole proof.
// Exit 0 on a clean create, 1 on failure, 0 (skip) if no ICD/worker is reachable.
#include <vulkan/vulkan.h>

#include "tests/vbo_spv.h"

#include <cstdint>
#include <cstdio>
#include <vector>

namespace {
void log(const char* m) {
    std::fprintf(stderr, "vkrelay2-vtxdivisor: %s\n", m);
}

VkShaderModule make_shader(VkDevice dev, const std::uint32_t* code, std::uint32_t bytes) {
    VkShaderModuleCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = bytes;
    ci.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    return vkCreateShaderModule(dev, &ci, nullptr, &m) == VK_SUCCESS ? m : VK_NULL_HANDLE;
}
} // namespace

int main() {
    VkApplicationInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    ai.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &ai;
    VkInstance instance = VK_NULL_HANDLE;
    if (vkCreateInstance(&ici, nullptr, &instance) != VK_SUCCESS) {
        log("skipped (vkCreateInstance failed -- no ICD/worker?)");
        return 0;
    }
    std::uint32_t pd_count = 0;
    vkEnumeratePhysicalDevices(instance, &pd_count, nullptr);
    if (pd_count == 0) {
        log("no physical device");
        vkDestroyInstance(instance, nullptr);
        return 1;
    }
    std::vector<VkPhysicalDevice> phys_devs(pd_count);
    vkEnumeratePhysicalDevices(instance, &pd_count, phys_devs.data());
    VkPhysicalDevice phys = phys_devs[0];
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(phys, &props);
    std::fprintf(stderr, "vkrelay2-vtxdivisor: device '%s'\n", props.deviceName);

    const float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = 0;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    // Enable the extension AND the feature (the divisor VALUE gate).
    // vertexAttributeInstanceRateDivisor is required for divisor != 1 -- exactly what the
    // ICD/worker re-derive + agreement-check.
    const char* dev_exts[] = {VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME};
    VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT div_feat{};
    div_feat.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT;
    div_feat.vertexAttributeInstanceRateDivisor = VK_TRUE;
    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.pNext = &div_feat;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = dev_exts;

    int rc = 1;
    VkDevice device = VK_NULL_HANDLE;
    VkRenderPass render_pass = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkShaderModule vert = VK_NULL_HANDLE;
    VkShaderModule frag = VK_NULL_HANDLE;
    if (vkCreateDevice(phys, &dci, nullptr, &device) != VK_SUCCESS) {
        log("vkCreateDevice failed (VK_EXT_vertex_attribute_divisor not enabled?)");
        vkDestroyInstance(instance, nullptr);
        return 1;
    }
    log("device created (VK_EXT_vertex_attribute_divisor + feature enabled)");

    vert = make_shader(device, vkr::vbo_spv::kVert, vkr::vbo_spv::kVertBytes);
    frag = make_shader(device, vkr::vbo_spv::kFrag, vkr::vbo_spv::kFragBytes);
    if (vert == VK_NULL_HANDLE || frag == VK_NULL_HANDLE) {
        log("vkCreateShaderModule failed");
        goto teardown;
    }

    {
        VkAttachmentDescription att{};
        att.format = VK_FORMAT_B8G8R8A8_UNORM;
        att.samples = VK_SAMPLE_COUNT_1_BIT;
        att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        att.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
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
        if (vkCreateRenderPass(device, &rpci, nullptr, &render_pass) != VK_SUCCESS) {
            log("vkCreateRenderPass failed");
            goto teardown;
        }
        VkPipelineLayoutCreateInfo plci{};
        plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        if (vkCreatePipelineLayout(device, &plci, nullptr, &layout) != VK_SUCCESS) {
            log("vkCreatePipelineLayout failed");
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
        // TWO bindings: binding 0 VERTEX-rate (pos, loc 0), binding 1 INSTANCE-rate (color, loc 1)
        // with divisor 2 -- the shape zink emits for per-instance attributes.
        VkVertexInputBindingDescription binds[2]{};
        binds[0].binding = 0;
        binds[0].stride = 8; // R32G32
        binds[0].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        binds[1].binding = 1;
        binds[1].stride = 12; // R32G32B32
        binds[1].inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;
        VkVertexInputAttributeDescription attrs[2]{};
        attrs[0].location = 0;
        attrs[0].binding = 0;
        attrs[0].format = VK_FORMAT_R32G32_SFLOAT;
        attrs[0].offset = 0;
        attrs[1].location = 1;
        attrs[1].binding = 1;
        attrs[1].format = VK_FORMAT_R32G32B32_SFLOAT;
        attrs[1].offset = 0;
        // The divisor pNext: binding 1 advances every 2 instances (needs the enabled feature).
        VkVertexInputBindingDivisorDescriptionEXT div{};
        div.binding = 1;
        div.divisor = 2;
        VkPipelineVertexInputDivisorStateCreateInfoEXT vidiv{};
        vidiv.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_EXT;
        vidiv.vertexBindingDivisorCount = 1;
        vidiv.pVertexBindingDivisors = &div;
        VkPipelineVertexInputStateCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vi.pNext = &vidiv; // the pNext the ICD used to reject wholesale
        vi.vertexBindingDescriptionCount = 2;
        vi.pVertexBindingDescriptions = binds;
        vi.vertexAttributeDescriptionCount = 2;
        vi.pVertexAttributeDescriptions = attrs;
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
        rs.cullMode = VK_CULL_MODE_NONE;
        rs.frontFace = VK_FRONT_FACE_CLOCKWISE;
        rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms{};
        ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
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
        VkGraphicsPipelineCreateInfo gpci{};
        gpci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        gpci.stageCount = 2;
        gpci.pStages = stages;
        gpci.pVertexInputState = &vi;
        gpci.pInputAssemblyState = &ia;
        gpci.pViewportState = &vp;
        gpci.pRasterizationState = &rs;
        gpci.pMultisampleState = &ms;
        gpci.pColorBlendState = &cb;
        gpci.pDynamicState = &ds;
        gpci.layout = layout;
        gpci.renderPass = render_pass;
        gpci.subpass = 0;
        gpci.basePipelineIndex = -1;
        const VkResult pr =
            vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gpci, nullptr, &pipeline);
        if (pr != VK_SUCCESS || pipeline == VK_NULL_HANDLE) {
            std::fprintf(stderr, "vkrelay2-vtxdivisor: vkCreateGraphicsPipelines FAILED (%d)\n",
                         static_cast<int>(pr));
            goto teardown;
        }
        log("PASS: divisor pipeline created on the real GPU");
        rc = 0;
    }

teardown:
    if (pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(device, pipeline, nullptr);
    }
    if (layout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device, layout, nullptr);
    }
    if (render_pass != VK_NULL_HANDLE) {
        vkDestroyRenderPass(device, render_pass, nullptr);
    }
    if (vert != VK_NULL_HANDLE) {
        vkDestroyShaderModule(device, vert, nullptr);
    }
    if (frag != VK_NULL_HANDLE) {
        vkDestroyShaderModule(device, frag, nullptr);
    }
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);
    return rc;
}
