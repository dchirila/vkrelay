// Unit tests for the ICD's fail-closed subset predicates.
//
// The void vkCmd* entrypoints and the create-info translators silently dropped unsupported fields
// before the worker could reject them; these predicates make the ICD reject out-of-subset input.
// They are pure, so they are tested directly here (no worker connection) on both platforms wherever
// vulkan.h is present -- which is exactly where the ICD / SDK is, so this runs under Vulkan_FOUND.
#include "linux/icd/icd_caps_cache.h"
#include "linux/icd/icd_subset.h"
#include "linux/icd/icd_version_policy.h"
#include "tests/test_assert.hpp"

#include <vulkan/vulkan.h>

#include <cstring>

using namespace vkr;

namespace {

// A fully in-subset graphics pipeline create-info (the shape the triangle canary builds), so a test
// can flip one field at a time and assert it is rejected.
struct ValidPipeline {
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
    VkGraphicsPipelineCreateInfo ci{};

    ValidPipeline() {
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        vp.viewportCount = 1;
        vp.scissorCount = 1;
        rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = VK_CULL_MODE_NONE;
        rs.frontFace = VK_FRONT_FACE_CLOCKWISE;
        rs.lineWidth = 1.0f;
        ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                             VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        cb.attachmentCount = 1;
        cb.pAttachments = &cba;
        ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        ds.dynamicStateCount = 2;
        ds.pDynamicStates = dyn;
        ci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        ci.stageCount = 2;
        ci.pStages = stages;
        ci.pVertexInputState = &vi;
        ci.pInputAssemblyState = &ia;
        ci.pViewportState = &vp;
        ci.pRasterizationState = &rs;
        ci.pMultisampleState = &ms;
        ci.pColorBlendState = &cb;
        ci.pDynamicState = &ds;
        ci.basePipelineIndex = -1;
    }
};

void test_simple_create_infos() {
    const char* why = "";
    std::uint64_t view_usage = 0;
    VkImageViewCreateInfo iv{};
    iv.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    VKR_CHECK(icd_subset::image_view_ok(&iv, &view_usage, &why));
    VKR_CHECK(view_usage == 0);
    iv.flags = 1;
    VKR_CHECK(!icd_subset::image_view_ok(&iv, &view_usage, &why));
    iv.flags = 0;
    int dummy = 0;
    iv.pNext = &dummy;
    VKR_CHECK(!icd_subset::image_view_ok(&iv, &view_usage, &why));
    // GL/zink: the ONE admitted pNext -- VkImageViewUsageCreateInfo. Its usage is captured;
    // a zero usage and a duplicate node are rejected.
    VkImageViewUsageCreateInfo ivu{};
    ivu.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO;
    ivu.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    iv.pNext = &ivu;
    VKR_CHECK(icd_subset::image_view_ok(&iv, &view_usage, &why));
    VKR_CHECK(view_usage == VK_IMAGE_USAGE_SAMPLED_BIT);
    ivu.usage = 0;
    VKR_CHECK(!icd_subset::image_view_ok(&iv, &view_usage, &why));
    ivu.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    VkImageViewUsageCreateInfo ivu2 = ivu;
    ivu.pNext = &ivu2;
    VKR_CHECK(!icd_subset::image_view_ok(&iv, &view_usage, &why));
    ivu.pNext = nullptr;
    iv.pNext = nullptr;

    VkShaderModuleCreateInfo sm{};
    sm.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    VKR_CHECK(icd_subset::shader_module_ok(&sm, &why));
    sm.pNext = &dummy;
    VKR_CHECK(!icd_subset::shader_module_ok(&sm, &why));

    VkFramebufferCreateInfo fb{};
    fb.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fb.attachmentCount = 1;
    VKR_CHECK(icd_subset::framebuffer_ok(&fb, &why));
    fb.attachmentCount = 2; // color + depth now in subset
    VKR_CHECK(icd_subset::framebuffer_ok(&fb, &why));
    fb.attachmentCount = 9; // MRT: up to 8 color + depth
    VKR_CHECK(icd_subset::framebuffer_ok(&fb, &why));
    fb.attachmentCount = 10; // beyond the sane bound
    VKR_CHECK(!icd_subset::framebuffer_ok(&fb, &why));
    fb.attachmentCount = 0; // a framebuffer needs at least one attachment
    VKR_CHECK(!icd_subset::framebuffer_ok(&fb, &why));

    VkPipelineLayoutCreateInfo pl{};
    pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    VKR_CHECK(icd_subset::pipeline_layout_ok(&pl, &why));
    pl.flags = 1;
    VKR_CHECK(!icd_subset::pipeline_layout_ok(&pl, &why));
}

void test_render_pass() {
    const char* why = "";
    constexpr std::uint32_t kMax = 8;  // a worker-advertised MRT limit
    constexpr std::uint32_t kNone = 0; // an OLD worker: no advertisement -> single-color gate
    VkAttachmentReference color{};
    color.attachment = 0;
    color.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkSubpassDescription sp{};
    sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sp.colorAttachmentCount = 1;
    sp.pColorAttachments = &color;
    VkAttachmentDescription att{};
    att.format = VK_FORMAT_B8G8R8A8_UNORM;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    VkRenderPassCreateInfo rp{};
    rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rp.attachmentCount = 1;
    rp.pAttachments = &att;
    rp.subpassCount = 1;
    rp.pSubpasses = &sp;
    VKR_CHECK(icd_subset::render_pass_ok(&rp, kMax, &why));
    VKR_CHECK(icd_subset::render_pass_ok(&rp, kNone, &why)); // single color: fine on an old worker

    // A single depth-stencil attachment is now in subset; the worker validates its
    // fields. An input attachment, by contrast, is still rejected.
    VkAttachmentReference depth{};
    depth.attachment = 1;
    depth.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkSubpassDescription sp_depth = sp;
    sp_depth.pDepthStencilAttachment = &depth;
    VkRenderPassCreateInfo rp_depth = rp;
    rp_depth.pSubpasses = &sp_depth;
    VKR_CHECK(icd_subset::render_pass_ok(&rp_depth, kMax, &why));
    VkSubpassDescription sp_input = sp;
    sp_input.inputAttachmentCount = 1;
    sp_input.pInputAttachments = &depth;
    VkRenderPassCreateInfo rp_input = rp;
    rp_input.pSubpasses = &sp_input;
    VKR_CHECK(!icd_subset::render_pass_ok(&rp_input, kMax, &why));

    // A compute subpass bind point and two subpasses are each rejected.
    VkSubpassDescription sp_compute = sp;
    sp_compute.pipelineBindPoint = VK_PIPELINE_BIND_POINT_COMPUTE;
    VkRenderPassCreateInfo rp_compute = rp;
    rp_compute.pSubpasses = &sp_compute;
    VKR_CHECK(!icd_subset::render_pass_ok(&rp_compute, kMax, &why));
    VkRenderPassCreateInfo rp_two = rp;
    rp_two.subpassCount = 2;
    VKR_CHECK(!icd_subset::render_pass_ok(&rp_two, kMax, &why));

    // MRT: N color refs (incl. an UNUSED hole) accepted up to the ADVERTISED limit; rejected past
    // it; rejected entirely (>1) toward an old worker that never advertised MRT (the capability
    // gate: a version-skewed pair degrades loudly, never silently).
    VkAttachmentReference two_colors[2] = {color, color};
    two_colors[1].attachment = 1;
    VkAttachmentDescription atts2[2] = {att, att};
    VkSubpassDescription sp_mrt = sp;
    sp_mrt.colorAttachmentCount = 2;
    sp_mrt.pColorAttachments = two_colors;
    VkRenderPassCreateInfo rp_mrt = rp;
    rp_mrt.attachmentCount = 2;
    rp_mrt.pAttachments = atts2;
    rp_mrt.pSubpasses = &sp_mrt;
    VKR_CHECK(icd_subset::render_pass_ok(&rp_mrt, kMax, &why));
    VKR_CHECK(!icd_subset::render_pass_ok(&rp_mrt, kNone, &why)); // old worker: LOUD, not silent
    VKR_CHECK(!icd_subset::render_pass_ok(&rp_mrt, 1, &why));     // past the advertised limit
    two_colors[1].attachment = VK_ATTACHMENT_UNUSED;              // a gapped glDrawBuffers shape
    VKR_CHECK(icd_subset::render_pass_ok(&rp_mrt, kMax, &why));
    two_colors[1].attachment = 1;

    // Uncarried attachment flags (e.g. MAY_ALIAS) are LOUD -- the wire does not carry them.
    VkAttachmentDescription att_flagged = att;
    att_flagged.flags = VK_ATTACHMENT_DESCRIPTION_MAY_ALIAS_BIT;
    VkRenderPassCreateInfo rp_flagged = rp;
    rp_flagged.pAttachments = &att_flagged;
    VKR_CHECK(!icd_subset::render_pass_ok(&rp_flagged, kMax, &why));

    // A color-count-0 subpass needs a depth attachment (zink's shadow-map depth-only pass).
    VkSubpassDescription sp_none = sp;
    sp_none.colorAttachmentCount = 0;
    sp_none.pColorAttachments = nullptr;
    VkRenderPassCreateInfo rp_none = rp;
    rp_none.pSubpasses = &sp_none;
    VKR_CHECK(!icd_subset::render_pass_ok(&rp_none, kMax, &why));
    VkSubpassDescription sp_depth_only = sp_none;
    sp_depth_only.pDepthStencilAttachment = &depth;
    VkRenderPassCreateInfo rp_depth_only = rp;
    rp_depth_only.pSubpasses = &sp_depth_only;
    VKR_CHECK(icd_subset::render_pass_ok(&rp_depth_only, kMax, &why));

    // Fail-closed pointer guards: a declared-but-null attachment or
    // dependency array is rejected, never dereferenced.
    VkRenderPassCreateInfo rp_null_atts = rp;
    rp_null_atts.pAttachments = nullptr;
    VKR_CHECK(!icd_subset::render_pass_ok(&rp_null_atts, kMax, &why));
    VkRenderPassCreateInfo rp_null_deps = rp;
    rp_null_deps.dependencyCount = 1;
    rp_null_deps.pDependencies = nullptr;
    VKR_CHECK(!icd_subset::render_pass_ok(&rp_null_deps, kMax, &why));
}

// MRT: the vkCreateRenderPass2 twin -- the path that used to have NO gate and silently dropped
// color refs beyond [0] (the empirically proven 2-color half-render). Same accept/reject matrix
// as v1 plus the v2-only uncarried surface: viewMask, correlated view masks, per-level pNext.
void test_render_pass2() {
    const char* why = "";
    constexpr std::uint32_t kMax = 8;
    constexpr std::uint32_t kNone = 0;
    int dummy = 0;
    VkAttachmentReference2 color{};
    color.sType = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2;
    color.attachment = 0;
    color.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkSubpassDescription2 sp{};
    sp.sType = VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2;
    sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sp.colorAttachmentCount = 1;
    sp.pColorAttachments = &color;
    VkAttachmentDescription2 att{};
    att.sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2;
    att.format = VK_FORMAT_B8G8R8A8_UNORM;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    VkRenderPassCreateInfo2 rp{};
    rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2;
    rp.attachmentCount = 1;
    rp.pAttachments = &att;
    rp.subpassCount = 1;
    rp.pSubpasses = &sp;
    VKR_CHECK(icd_subset::render_pass2_ok(&rp, kMax, false, &why));

    // MRT accept up to the limit; the capability gate is LOUD toward an old worker.
    VkAttachmentReference2 two_colors[2] = {color, color};
    two_colors[1].attachment = 1;
    VkAttachmentDescription2 atts2[2] = {att, att};
    VkSubpassDescription2 sp_mrt = sp;
    sp_mrt.colorAttachmentCount = 2;
    sp_mrt.pColorAttachments = two_colors;
    VkRenderPassCreateInfo2 rp_mrt = rp;
    rp_mrt.attachmentCount = 2;
    rp_mrt.pAttachments = atts2;
    rp_mrt.pSubpasses = &sp_mrt;
    VKR_CHECK(icd_subset::render_pass2_ok(&rp_mrt, kMax, false, &why));
    VKR_CHECK(!icd_subset::render_pass2_ok(&rp_mrt, kNone, false, &why));
    two_colors[1].attachment = VK_ATTACHMENT_UNUSED; // gapped draw buffers
    VKR_CHECK(icd_subset::render_pass2_ok(&rp_mrt, kMax, false, &why));
    two_colors[1].attachment = 1;

    // Required-feature audit (multiview): the subpass viewMask is fail-closed WITHOUT
    // the multiview feature, and CARRIED (accepted) WITH it -- the gate. The correlated view masks
    // and per-dependency viewOffset stay uncarried (LOUD) in BOTH cases -- only the subpass mask
    // crosses. pNext at every level (create-info / attachment / subpass / reference) also stays
    // loud.
    VkSubpassDescription2 sp_vm = sp;
    sp_vm.viewMask = 0x3;
    VkRenderPassCreateInfo2 rp_vm = rp;
    rp_vm.pSubpasses = &sp_vm;
    VKR_CHECK(!icd_subset::render_pass2_ok(&rp_vm, kMax, false, &why)); // no feature -> reject
    VKR_CHECK(icd_subset::render_pass2_ok(&rp_vm, kMax, true, &why));   // feature -> carried
    std::uint32_t corr = 0x1;
    VkRenderPassCreateInfo2 rp_corr = rp;
    rp_corr.correlatedViewMaskCount = 1;
    rp_corr.pCorrelatedViewMasks = &corr;
    VKR_CHECK(!icd_subset::render_pass2_ok(&rp_corr, kMax, false, &why));
    VKR_CHECK(!icd_subset::render_pass2_ok(&rp_corr, kMax, true, &why)); // loud even with multiview
    VkRenderPassCreateInfo2 rp_pnext = rp;
    rp_pnext.pNext = &dummy;
    VKR_CHECK(!icd_subset::render_pass2_ok(&rp_pnext, kMax, false, &why));
    VkAttachmentDescription2 att_pnext = att;
    att_pnext.pNext = &dummy;
    VkRenderPassCreateInfo2 rp_att_pnext = rp;
    rp_att_pnext.pAttachments = &att_pnext;
    VKR_CHECK(!icd_subset::render_pass2_ok(&rp_att_pnext, kMax, false, &why));
    VkSubpassDescription2 sp_pnext = sp;
    sp_pnext.pNext = &dummy;
    VkRenderPassCreateInfo2 rp_sp_pnext = rp;
    rp_sp_pnext.pSubpasses = &sp_pnext;
    VKR_CHECK(!icd_subset::render_pass2_ok(&rp_sp_pnext, kMax, false, &why));
    VkAttachmentReference2 color_pnext = color;
    color_pnext.pNext = &dummy;
    VkSubpassDescription2 sp_ref_pnext = sp;
    sp_ref_pnext.pColorAttachments = &color_pnext;
    VkRenderPassCreateInfo2 rp_ref_pnext = rp;
    rp_ref_pnext.pSubpasses = &sp_ref_pnext;
    VKR_CHECK(!icd_subset::render_pass2_ok(&rp_ref_pnext, kMax, false, &why));

    // Uncarried attachment flags + multi-subpass + input attachments: LOUD, same as v1.
    VkAttachmentDescription2 att_flagged = att;
    att_flagged.flags = VK_ATTACHMENT_DESCRIPTION_MAY_ALIAS_BIT;
    VkRenderPassCreateInfo2 rp_flagged = rp;
    rp_flagged.pAttachments = &att_flagged;
    VKR_CHECK(!icd_subset::render_pass2_ok(&rp_flagged, kMax, false, &why));
    VkRenderPassCreateInfo2 rp_two = rp;
    rp_two.subpassCount = 2;
    VKR_CHECK(!icd_subset::render_pass2_ok(&rp_two, kMax, false, &why));
    VkSubpassDescription2 sp_input = sp;
    sp_input.inputAttachmentCount = 1;
    sp_input.pInputAttachments = &color;
    VkRenderPassCreateInfo2 rp_input = rp;
    rp_input.pSubpasses = &sp_input;
    VKR_CHECK(!icd_subset::render_pass2_ok(&rp_input, kMax, false, &why));

    // Reference aspectMask: spec-used ONLY for input attachment
    // references (rejected wholesale), so in color/depth positions it carries no semantics --
    // and zink demonstrably sends UNINITIALIZED memory through it (0x967e878c observed on a real
    // OpenSCAD depth ref). ANY value must therefore be accepted in these positions; these checks
    // pin that disposition. A dependency pNext (e.g. a chained VkMemoryBarrier2) and a multiview
    // viewOffset DO carry dropped semantics and are rejected by name.
    VkAttachmentReference2 color_aspect = color;
    color_aspect.aspectMask = 0x967e878c; // the garbage zink actually sent -- must not reject
    VkSubpassDescription2 sp_aspect = sp;
    sp_aspect.pColorAttachments = &color_aspect;
    VkRenderPassCreateInfo2 rp_aspect = rp;
    rp_aspect.pSubpasses = &sp_aspect;
    VKR_CHECK(icd_subset::render_pass2_ok(&rp_aspect, kMax, false, &why));
    VkAttachmentReference2 depth_ref{};
    depth_ref.sType = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2;
    depth_ref.attachment = 0;
    depth_ref.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depth_ref.aspectMask = 0x967e878c;
    VkSubpassDescription2 sp_depth_aspect = sp;
    sp_depth_aspect.pDepthStencilAttachment = &depth_ref;
    VkRenderPassCreateInfo2 rp_depth_aspect = rp;
    rp_depth_aspect.pSubpasses = &sp_depth_aspect;
    VKR_CHECK(icd_subset::render_pass2_ok(&rp_depth_aspect, kMax, false, &why));
    VkSubpassDependency2 dep2{};
    dep2.sType = VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2;
    dep2.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep2.dstSubpass = 0;
    dep2.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep2.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkRenderPassCreateInfo2 rp_dep = rp;
    rp_dep.dependencyCount = 1;
    rp_dep.pDependencies = &dep2;
    VKR_CHECK(
        icd_subset::render_pass2_ok(&rp_dep, kMax, false, &why)); // a plain dependency is fine
    dep2.pNext = &dummy;
    VKR_CHECK(!icd_subset::render_pass2_ok(&rp_dep, kMax, false, &why));
    dep2.pNext = nullptr;
    dep2.viewOffset = 1;
    VKR_CHECK(!icd_subset::render_pass2_ok(&rp_dep, kMax, false, &why));
    VKR_CHECK(!icd_subset::render_pass2_ok(&rp_dep, kMax, true, &why)); // loud even with multiview
    dep2.viewOffset = 0;

    // Fail-closed pointer guards, v2 flavor.
    VkRenderPassCreateInfo2 rp_null_atts = rp;
    rp_null_atts.pAttachments = nullptr;
    VKR_CHECK(!icd_subset::render_pass2_ok(&rp_null_atts, kMax, false, &why));
    VkRenderPassCreateInfo2 rp_null_deps = rp;
    rp_null_deps.dependencyCount = 1;
    rp_null_deps.pDependencies = nullptr;
    VKR_CHECK(!icd_subset::render_pass2_ok(&rp_null_deps, kMax, false, &why));
}

void test_graphics_pipeline() {
    const char* why = "";
    {
        ValidPipeline p;
        VKR_CHECK(icd_subset::graphics_pipeline_ok(&p.ci, false, false, false, false, false, false,
                                                   &why));
    }
    {
        // (GL/zink): the full rasterization / multisample / colour-blend / depth-stencil
        // state is now CARRIED, so these previously-rejected shapes are ACCEPTED (the host gates).
        ValidPipeline p;
        VkPipelineDepthStencilStateCreateInfo dss{};
        dss.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        dss.depthTestEnable = VK_TRUE;
        dss.stencilTestEnable = VK_TRUE;     // stencil now carried
        dss.depthBoundsTestEnable = VK_TRUE; // depth bounds now carried
        p.ci.pDepthStencilState = &dss;
        VKR_CHECK(icd_subset::graphics_pipeline_ok(&p.ci, false, false, false, false, false, false,
                                                   &why));
    }
    {
        ValidPipeline p; // blending enabled -- now carried
        p.cba.blendEnable = VK_TRUE;
        VKR_CHECK(icd_subset::graphics_pipeline_ok(&p.ci, false, false, false, false, false, false,
                                                   &why));
    }
    {
        ValidPipeline p; // multiple dynamic viewports -- counts carried
        p.vp.viewportCount = 2;
        p.vp.scissorCount = 2;
        VKR_CHECK(icd_subset::graphics_pipeline_ok(&p.ci, false, false, false, false, false, false,
                                                   &why));
    }
    {
        ValidPipeline p; // MSAA -- carried
        p.ms.rasterizationSamples = VK_SAMPLE_COUNT_4_BIT;
        VKR_CHECK(icd_subset::graphics_pipeline_ok(&p.ci, false, false, false, false, false, false,
                                                   &why));
    }
    {
        ValidPipeline p; // non-FILL polygon mode + rasterizer discard -- carried
        p.rs.polygonMode = VK_POLYGON_MODE_LINE;
        p.rs.rasterizerDiscardEnable = VK_TRUE;
        p.rs.lineWidth = 2.0f;
        VKR_CHECK(icd_subset::graphics_pipeline_ok(&p.ci, false, false, false, false, false, false,
                                                   &why));
    }
    // Still fail-closed (un-carried state):
    {
        ValidPipeline p; // a specialization constant on a stage
        VkSpecializationInfo si{};
        p.stages[0].pSpecializationInfo = &si;
        VKR_CHECK(!icd_subset::graphics_pipeline_ok(&p.ci, false, false, false, false, false, false,
                                                    &why));
    }
    {
        ValidPipeline p; // a sample MASK > 32 samples is not carried
        p.ms.rasterizationSamples = VK_SAMPLE_COUNT_64_BIT;
        VKR_CHECK(!icd_subset::graphics_pipeline_ok(&p.ci, false, false, false, false, false, false,
                                                    &why));
    }
    {
        ValidPipeline p; // static viewports (non-dynamic) are not carried
        VkViewport vps{};
        p.vp.pViewports = &vps;
        VKR_CHECK(!icd_subset::graphics_pipeline_ok(&p.ci, false, false, false, false, false, false,
                                                    &why));
    }
    {
        ValidPipeline p; // a pNext chain on the create-info
        int dummy = 0;
        p.ci.pNext = &dummy;
        VKR_CHECK(!icd_subset::graphics_pipeline_ok(&p.ci, false, false, false, false, false, false,
                                                    &why));
    }
    // (GL/zink) RENDER CORRECTNESS: the rasterization pNext admits EXACTLY the depth-clip
    // (VK_EXT_depth_clip_enable) and line-state (VK_EXT_line_rasterization) structs zink chains --
    // their fields ride the wire + the worker rebuilds the pNext. Any other rasterization pNext, or
    // a rasterization flags word, stays fail-closed (not carried). Without these zink renders
    // incorrectly (OpenSCAD's CSG preview went black / mis-clipped).
    {
        ValidPipeline p; // depth-clip rasterization pNext is ACCEPTED
        VkPipelineRasterizationDepthClipStateCreateInfoEXT dc{};
        dc.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_DEPTH_CLIP_STATE_CREATE_INFO_EXT;
        dc.depthClipEnable = VK_FALSE;
        p.rs.pNext = &dc;
        VKR_CHECK(icd_subset::graphics_pipeline_ok(&p.ci, false, false, false, false, false, false,
                                                   &why));
    }
    {
        ValidPipeline p; // line-state rasterization pNext is ACCEPTED
        VkPipelineRasterizationLineStateCreateInfoEXT ls{};
        ls.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_LINE_STATE_CREATE_INFO_EXT;
        ls.lineRasterizationMode = VK_LINE_RASTERIZATION_MODE_RECTANGULAR_EXT;
        p.rs.pNext = &ls;
        VKR_CHECK(icd_subset::graphics_pipeline_ok(&p.ci, false, false, false, false, false, false,
                                                   &why));
    }
    {
        ValidPipeline p; // depth-clip CHAINED to line-state -- both admitted
        VkPipelineRasterizationDepthClipStateCreateInfoEXT dc{};
        dc.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_DEPTH_CLIP_STATE_CREATE_INFO_EXT;
        VkPipelineRasterizationLineStateCreateInfoEXT ls{};
        ls.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_LINE_STATE_CREATE_INFO_EXT;
        dc.pNext = &ls;
        p.rs.pNext = &dc;
        VKR_CHECK(icd_subset::graphics_pipeline_ok(&p.ci, false, false, false, false, false, false,
                                                   &why));
    }
    {
        ValidPipeline p; // an UNKNOWN rasterization pNext stays fail-closed
        int dummy = 0;
        p.rs.pNext = &dummy;
        VKR_CHECK(!icd_subset::graphics_pipeline_ok(&p.ci, false, false, false, false, false, false,
                                                    &why));
    }
    {
        ValidPipeline p; // a rasterization flags word stays fail-closed
        p.rs.flags = 1;
        VKR_CHECK(!icd_subset::graphics_pipeline_ok(&p.ci, false, false, false, false, false, false,
                                                    &why));
    }
    // Native lane: the VkPipelineRenderingCreateInfo
    // top-level pNext is admitted ONLY when allow_dynamic_rendering_pnext is TRUE (a native
    // DR-enabled device). With the flag FALSE -- the default/zink lane -- the SAME struct is
    // rejected, so the guard cannot be widened by accident. This is the pin that keeps the default
    // lane fail-closed.
    {
        ValidPipeline p;
        VkPipelineRenderingCreateInfo pri{};
        pri.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
        p.ci.pNext = &pri;
        VKR_CHECK(icd_subset::graphics_pipeline_ok(&p.ci, true, false, false, false, false, false,
                                                   &why)); // admitted on the DR lane
        VKR_CHECK(!icd_subset::graphics_pipeline_ok(&p.ci, false, false, false, false, false, false,
                                                    &why)); // rejected off it
    }
    {
        ValidPipeline p; // even on the DR lane, only VkPipelineRenderingCreateInfo is admitted
        int dummy = 0;
        p.ci.pNext = &dummy;
        VKR_CHECK(!icd_subset::graphics_pipeline_ok(&p.ci, true, false, false, false, false, false,
                                                    &why));
    }
    // vertex-attr-divisor: the VkPipelineVertexInputDivisorStateCreateInfoEXT vertex-input pNext is
    // admitted EXACTLY when allow_vertex_attribute_divisor is TRUE (the device enabled the ext),
    // and rejected off it -- structural admission only (content/feature gates live in
    // vkrpc::vertex_binding_divisors_ok). Every other vertex-input pNext stays fail-closed.
    {
        ValidPipeline p;
        VkPipelineVertexInputDivisorStateCreateInfoEXT vd{};
        vd.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_EXT;
        p.vi.pNext = &vd;
        VKR_CHECK(icd_subset::graphics_pipeline_ok(&p.ci, false, false, false, false,
                                                   /*divisor=*/true, false,
                                                   &why)); // admitted with the ext enabled
        VKR_CHECK(!icd_subset::graphics_pipeline_ok(&p.ci, false, false, false, false,
                                                    /*divisor=*/false, false,
                                                    &why)); // rejected without the ext
    }
    {
        ValidPipeline
            p; // an UNKNOWN vertex-input pNext stays fail-closed even with the ext enabled
        int dummy = 0;
        p.vi.pNext = &dummy;
        VKR_CHECK(!icd_subset::graphics_pipeline_ok(&p.ci, false, false, false, false,
                                                    /*divisor=*/true, false, &why));
    }
    {
        ValidPipeline p; // a nonzero vertex-input flags word stays fail-closed
        p.vi.flags = 1;
        VKR_CHECK(!icd_subset::graphics_pipeline_ok(&p.ci, false, false, false, false,
                                                    /*divisor=*/true, false, &why));
    }
    {
        ValidPipeline p; // TWO divisor pNext structs -- an sType must not repeat in a chain, so the
                         // second is rejected rather than silently collapsed.
        VkPipelineVertexInputDivisorStateCreateInfoEXT vd0{};
        vd0.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_EXT;
        VkPipelineVertexInputDivisorStateCreateInfoEXT vd1{};
        vd1.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_EXT;
        vd0.pNext = &vd1;
        p.vi.pNext = &vd0;
        VKR_CHECK(!icd_subset::graphics_pipeline_ok(&p.ci, false, false, false, false,
                                                    /*divisor=*/true, false, &why));
    }
    // geometry-stream: the VkPipelineRasterizationStateStreamCreateInfoEXT rasterization pNext is
    // admitted EXACTLY when allow_rasterization_stream is TRUE (the device enabled
    // VK_EXT_transform_feedback + geometryStreams), and rejected off it -- structural admission
    // only (the VALUE gates live in the shared vkrpc::rasterization_stream_ok). Reserved flags and
    // duplicate structs stay fail-closed; an unknown rasterization pNext stays rejected after the
    // widening.
    {
        ValidPipeline p;
        VkPipelineRasterizationStateStreamCreateInfoEXT ss{};
        ss.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_STREAM_CREATE_INFO_EXT;
        ss.rasterizationStream = 0; // explicit stream zero -- the named case
        p.rs.pNext = &ss;
        VKR_CHECK(icd_subset::graphics_pipeline_ok(&p.ci, false, false, false, false, false,
                                                   /*stream=*/true, &why));
        ss.rasterizationStream = 2; // nonzero rides too (values gate at the shared validator)
        VKR_CHECK(icd_subset::graphics_pipeline_ok(&p.ci, false, false, false, false, false,
                                                   /*stream=*/true, &why));
        VKR_CHECK(!icd_subset::graphics_pipeline_ok(&p.ci, false, false, false, false, false,
                                                    /*stream=*/false, &why)); // ext/feature off
        // The allow-off rejection must NAME the actual gate (the degraded
        // mixed-version path tells the user to update the worker), not fall into the generic
        // unknown-pNext reason that lists "stream" as supported.
        VKR_CHECK(std::strstr(why, "stream-capable worker") != nullptr);
    }
    {
        ValidPipeline p; // reserved flags must be 0 (fail-closed, not carried)
        VkPipelineRasterizationStateStreamCreateInfoEXT ss{};
        ss.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_STREAM_CREATE_INFO_EXT;
        ss.flags = 1;
        p.rs.pNext = &ss;
        VKR_CHECK(!icd_subset::graphics_pipeline_ok(&p.ci, false, false, false, false, false,
                                                    /*stream=*/true, &why));
    }
    {
        ValidPipeline p; // TWO stream structs -- a duplicate fails closed (never last-one-wins)
        VkPipelineRasterizationStateStreamCreateInfoEXT ss0{};
        ss0.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_STREAM_CREATE_INFO_EXT;
        VkPipelineRasterizationStateStreamCreateInfoEXT ss1{};
        ss1.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_STREAM_CREATE_INFO_EXT;
        ss0.pNext = &ss1;
        p.rs.pNext = &ss0;
        VKR_CHECK(!icd_subset::graphics_pipeline_ok(&p.ci, false, false, false, false, false,
                                                    /*stream=*/true, &why));
    }
    {
        ValidPipeline p; // duplicate DEPTH-CLIP structs fail closed too (same chain invariant)
        VkPipelineRasterizationDepthClipStateCreateInfoEXT dc0{};
        dc0.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_DEPTH_CLIP_STATE_CREATE_INFO_EXT;
        VkPipelineRasterizationDepthClipStateCreateInfoEXT dc1{};
        dc1.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_DEPTH_CLIP_STATE_CREATE_INFO_EXT;
        dc0.pNext = &dc1;
        p.rs.pNext = &dc0;
        VKR_CHECK(!icd_subset::graphics_pipeline_ok(&p.ci, false, false, false, false, false, false,
                                                    &why));
    }
    {
        ValidPipeline p; // an UNKNOWN rasterization pNext stays fail-closed even with stream on
        int dummy = 0;
        p.rs.pNext = &dummy;
        VKR_CHECK(!icd_subset::graphics_pipeline_ok(&p.ci, false, false, false, false, false,
                                                    /*stream=*/true, &why));
    }
}

void test_commands() {
    const char* why = "";
    VkClearValue clear{};
    VkRenderPassBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    bi.clearValueCount = 1;
    bi.pClearValues = &clear;
    VKR_CHECK(icd_subset::begin_render_pass_ok(&bi, VK_SUBPASS_CONTENTS_INLINE, &why));
    VKR_CHECK(!icd_subset::begin_render_pass_ok(&bi, VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS,
                                                &why));
    // (GL/zink) faithful clears: any clearValueCount is forwarded verbatim (the recorder
    // carries the exact array; the host driver matches them to the render pass). Zero clears (an
    // all-LOAD pass) and 3+ clears (multi-attachment) are now ACCEPTED; only a positive count with
    // a null array is structurally impossible.
    VkRenderPassBeginInfo bi_zero = bi;
    bi_zero.clearValueCount = 0;
    bi_zero.pClearValues = nullptr; // 0 clears + null array is valid (all-LOAD attachments)
    VKR_CHECK(icd_subset::begin_render_pass_ok(&bi_zero, VK_SUBPASS_CONTENTS_INLINE, &why));
    VkRenderPassBeginInfo bi_null = bi;
    bi_null.pClearValues = nullptr; // count 1 + null array is structurally impossible
    VKR_CHECK(!icd_subset::begin_render_pass_ok(&bi_null, VK_SUBPASS_CONTENTS_INLINE, &why));
    VkRenderPassBeginInfo bi_two = bi;
    bi_two.clearValueCount = 2; // color + depth clears
    VKR_CHECK(icd_subset::begin_render_pass_ok(&bi_two, VK_SUBPASS_CONTENTS_INLINE, &why));
    VkRenderPassBeginInfo bi_three = bi;
    bi_three.clearValueCount = 3; // multi-attachment clears now forwarded faithfully
    VKR_CHECK(icd_subset::begin_render_pass_ok(&bi_three, VK_SUBPASS_CONTENTS_INLINE, &why));
    int dummy = 0;
    bi.pNext = &dummy;
    VKR_CHECK(!icd_subset::begin_render_pass_ok(&bi, VK_SUBPASS_CONTENTS_INLINE, &why));

    VKR_CHECK(icd_subset::bind_pipeline_ok(VK_PIPELINE_BIND_POINT_GRAPHICS, &why));
    VKR_CHECK(icd_subset::bind_pipeline_ok(VK_PIPELINE_BIND_POINT_COMPUTE, &why));

    VKR_CHECK(icd_subset::set_viewport_ok(0, 1, &why));
    VKR_CHECK(!icd_subset::set_viewport_ok(1, 1, &why)); // firstViewport != 0
    VKR_CHECK(!icd_subset::set_viewport_ok(0, 2, &why)); // count != 1
    VKR_CHECK(icd_subset::set_scissor_ok(0, 1, &why));
    VKR_CHECK(!icd_subset::set_scissor_ok(0, 2, &why));
}

// Host-visible memory + buffers predicates.
void test_memory_buffer() {
    const char* why = "";
    int dummy = 0;

    VkBufferCreateInfo bc{};
    bc.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bc.size = 1024;
    bc.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    bc.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VKR_CHECK(icd_subset::buffer_ok(&bc, &why));
    {
        VkBufferCreateInfo b = bc;
        b.size = 0;
        VKR_CHECK(!icd_subset::buffer_ok(&b, &why));
        b = bc;
        // TRANSFER_DST + INDEX + STORAGE are now in the broadened buffer-usage subset.
        b.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                  VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        VKR_CHECK(icd_subset::buffer_ok(&b, &why));
        b = bc;
        // A usage bit outside the supported set (a high unused bit) is still rejected.
        b.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | static_cast<VkBufferUsageFlags>(0x10000000);
        VKR_CHECK(!icd_subset::buffer_ok(&b, &why));
        b = bc;
        b.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT; // UNIFORM now in subset
        VKR_CHECK(icd_subset::buffer_ok(&b, &why));
        b = bc;
        b.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT; // TRANSFER_SRC (staging) in subset
        VKR_CHECK(icd_subset::buffer_ok(&b, &why));
        b = bc;
        b.usage = 0; // empty usage rejected
        VKR_CHECK(!icd_subset::buffer_ok(&b, &why));
        b = bc;
        b.sharingMode = VK_SHARING_MODE_CONCURRENT;
        VKR_CHECK(!icd_subset::buffer_ok(&b, &why));
        b = bc;
        b.flags = 1;
        VKR_CHECK(!icd_subset::buffer_ok(&b, &why));
        b = bc;
        b.pNext = &dummy;
        VKR_CHECK(!icd_subset::buffer_ok(&b, &why));
    }

    VkMemoryAllocateInfo ma{};
    ma.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ma.allocationSize = 4096;
    ma.memoryTypeIndex = 1;
    VKR_CHECK(icd_subset::memory_allocate_ok(&ma, false, &why));
    VKR_CHECK(icd_subset::memory_allocate_ok(&ma, true, &why)); // plain allocate: feature-agnostic
    {
        VkMemoryAllocateInfo m = ma;
        m.allocationSize = 0;
        VKR_CHECK(!icd_subset::memory_allocate_ok(&m, false, &why));
        m = ma;
        m.pNext = &dummy; // a dedicated-allocation / export chain: rejected with OR without BDA
        VKR_CHECK(!icd_subset::memory_allocate_ok(&m, false, &why));
        VKR_CHECK(!icd_subset::memory_allocate_ok(&m, true, &why));
    }
    // bufferDeviceAddress: the flags-info pNext -- admitted ONLY as exactly
    // DEVICE_ADDRESS on a device with the enabled feature; deviceMask / captureReplay / unknown
    // bits fail closed by name.
    {
        VkMemoryAllocateFlagsInfo fi{};
        fi.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
        fi.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
        VkMemoryAllocateInfo m = ma;
        m.pNext = &fi;
        VKR_CHECK(icd_subset::memory_allocate_ok(&m, true, &why));
        VKR_CHECK(!icd_subset::memory_allocate_ok(&m, false, &why)); // feature off -> reject
        fi.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT | VK_MEMORY_ALLOCATE_DEVICE_MASK_BIT;
        VKR_CHECK(!icd_subset::memory_allocate_ok(&m, true, &why)); // deviceMask bit -> reject
        fi.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_CAPTURE_REPLAY_BIT;
        VKR_CHECK(!icd_subset::memory_allocate_ok(&m, true, &why)); // captureReplay -> reject
        fi.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
        fi.deviceMask = 1;
        VKR_CHECK(!icd_subset::memory_allocate_ok(&m, true, &why)); // nonzero deviceMask -> reject
        fi.deviceMask = 0;
        fi.pNext = &dummy; // a second, foreign node behind the flags-info -> reject
        VKR_CHECK(!icd_subset::memory_allocate_ok(&m, true, &why));
    }

    VkBuffer bufs[2] = {reinterpret_cast<VkBuffer>(0x10), reinterpret_cast<VkBuffer>(0x20)};
    VkDeviceSize offs[2] = {0, 0};
    VKR_CHECK(icd_subset::bind_vertex_buffers_ok(0, 1, bufs, offs, &why));
    VKR_CHECK(icd_subset::bind_vertex_buffers_ok(0, 2, bufs, offs, &why));
    VKR_CHECK(!icd_subset::bind_vertex_buffers_ok(1, 1, bufs, offs, &why)); // firstBinding != 0
    VKR_CHECK(!icd_subset::bind_vertex_buffers_ok(0, 0, bufs, offs, &why)); // empty
    VKR_CHECK(!icd_subset::bind_vertex_buffers_ok(0, 1, nullptr, offs, &why));
    VKR_CHECK(!icd_subset::bind_vertex_buffers_ok(0, 1, bufs, nullptr, &why));
    {
        VkBuffer with_null[2] = {reinterpret_cast<VkBuffer>(0x10), VK_NULL_HANDLE};
        VKR_CHECK(!icd_subset::bind_vertex_buffers_ok(0, 2, with_null, offs, &why));
    }
}

// Descriptor surface predicates.
void test_descriptor_surface() {
    const char* why = "";
    int dummy = 0;

    // Pipeline layout admits a set-layout list AND push-constant ranges (zink uses them).
    VkDescriptorSetLayout fake_sl = reinterpret_cast<VkDescriptorSetLayout>(0x10);
    VkPipelineLayoutCreateInfo pl{};
    pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &fake_sl;
    VKR_CHECK(icd_subset::pipeline_layout_ok(&pl, &why));
    {
        // A VALID push-constant range is accepted (nonzero stage, 4-aligned, within the byte cap).
        VkPushConstantRange ok_pc{};
        ok_pc.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        ok_pc.offset = 0;
        ok_pc.size = 64;
        VkPipelineLayoutCreateInfo p = pl;
        p.pushConstantRangeCount = 1;
        p.pPushConstantRanges = &ok_pc;
        VKR_CHECK(icd_subset::pipeline_layout_ok(&p, &why));
        // Out-of-subset push-constant ranges are rejected.
        VkPushConstantRange bad = ok_pc;
        bad.stageFlags = 0; // no stage
        p.pPushConstantRanges = &bad;
        VKR_CHECK(!icd_subset::pipeline_layout_ok(&p, &why));
        bad = ok_pc;
        bad.size = 3; // not 4-aligned
        p.pPushConstantRanges = &bad;
        VKR_CHECK(!icd_subset::pipeline_layout_ok(&p, &why));
        bad = ok_pc;
        bad.offset = 4;
        bad.size = icd_subset::kMaxPushConstantBytes; // offset+size over the cap
        p.pPushConstantRanges = &bad;
        VKR_CHECK(!icd_subset::pipeline_layout_ok(&p, &why));
        p = pl;
        p.pushConstantRangeCount = 1;
        p.pPushConstantRanges = nullptr; // count > 0 but null array
        VKR_CHECK(!icd_subset::pipeline_layout_ok(&p, &why));
        p = pl;
        p.setLayoutCount = icd_subset::kMaxPipelineLayoutSetLayouts + 1; // too many
        VKR_CHECK(!icd_subset::pipeline_layout_ok(&p, &why));
        p = pl;
        p.pSetLayouts = nullptr; // count > 0 but null array
        VKR_CHECK(!icd_subset::pipeline_layout_ok(&p, &why));
    }

    // Descriptor set layout: one UNIFORM_BUFFER binding, VERTEX stage.
    VkDescriptorSetLayoutBinding b{};
    b.binding = 0;
    b.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    b.descriptorCount = 1;
    b.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    VkDescriptorSetLayoutCreateInfo dsl{};
    dsl.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dsl.bindingCount = 1;
    dsl.pBindings = &b;
    VKR_CHECK(icd_subset::descriptor_set_layout_ok(&dsl, &why));
    {
        // A 2-binding layout (UNIFORM/VERTEX + COMBINED_IMAGE_SAMPLER/FRAGMENT)
        // is in subset.
        VkDescriptorSetLayoutBinding cis = b;
        cis.binding = 1;
        cis.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        cis.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutBinding both[2] = {b, cis};
        VkDescriptorSetLayoutCreateInfo d = dsl;
        d.bindingCount = 2;
        d.pBindings = both;
        VKR_CHECK(icd_subset::descriptor_set_layout_ok(&d, &why));
    }
    {
        // (GL/zink): faithful admission. A non-binding-flags pNext is rejected; immutable
        // samplers are rejected (not carried). descriptorIndexing: the UPDATE_AFTER_BIND_POOL
        // layout flag is no longer a STRUCTURAL reject here -- its admission is feature-gated at
        // the ICD entry via the shared vkrpc::descriptor_indexing_layout_ok.
        VkDescriptorSetLayoutCreateInfo d = dsl;
        d.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
        VKR_CHECK(icd_subset::descriptor_set_layout_ok(&d, &why)); // shape-ok; entry gates it
        d = dsl;
        d.pNext = &dummy; // a non-binding-flags pNext is rejected
        VKR_CHECK(!icd_subset::descriptor_set_layout_ok(&d, &why));
        d = dsl;
        d.pBindings = nullptr; // count > 0 but null
        VKR_CHECK(!icd_subset::descriptor_set_layout_ok(&d, &why));
        VkSampler fake_sampler = reinterpret_cast<VkSampler>(0x99);
        VkDescriptorSetLayoutBinding bad = b;
        bad.pImmutableSamplers = &fake_sampler; // immutable samplers not carried -> rejected
        d = dsl;
        d.pBindings = &bad;
        VKR_CHECK(!icd_subset::descriptor_set_layout_ok(&d, &why));
    }
    {
        // descriptorIndexing: the binding-flags pNext must be EXACT --
        // bindingCount 0 (flags all zero) or == the layout's bindingCount with a non-null array;
        // short/long/null-array shapes and duplicate structs reject.
        VkDescriptorBindingFlags one_flag = 0;
        VkDescriptorSetLayoutBindingFlagsCreateInfo bf{};
        bf.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
        bf.bindingCount = 0;
        bf.pBindingFlags = nullptr;
        VkDescriptorSetLayoutCreateInfo d = dsl; // dsl has bindingCount == 1
        d.pNext = &bf;
        VKR_CHECK(icd_subset::descriptor_set_layout_ok(&d, &why)); // count 0 = all-zero flags
        bf.bindingCount = 1;
        bf.pBindingFlags = &one_flag;
        VKR_CHECK(icd_subset::descriptor_set_layout_ok(&d, &why)); // exact match
        bf.bindingCount = 2;                                       // LONG: != the layout's 1
        VkDescriptorBindingFlags two_flags[2] = {0, 0};
        bf.pBindingFlags = two_flags;
        VKR_CHECK(!icd_subset::descriptor_set_layout_ok(&d, &why));
        bf.bindingCount = 1;
        bf.pBindingFlags = nullptr; // nonzero count with a NULL array
        VKR_CHECK(!icd_subset::descriptor_set_layout_ok(&d, &why));
        // SHORT: a 1-entry struct against a 2-binding layout.
        VkDescriptorSetLayoutBinding cis2 = b;
        cis2.binding = 1;
        VkDescriptorSetLayoutBinding both2[2] = {b, cis2};
        VkDescriptorSetLayoutCreateInfo d2 = dsl;
        d2.bindingCount = 2;
        d2.pBindings = both2;
        bf.bindingCount = 1;
        bf.pBindingFlags = &one_flag;
        d2.pNext = &bf;
        VKR_CHECK(!icd_subset::descriptor_set_layout_ok(&d2, &why));
        // Duplicate binding-flags structs reject.
        VkDescriptorSetLayoutBindingFlagsCreateInfo bf2 = bf;
        bf2.bindingCount = 0;
        bf2.pBindingFlags = nullptr;
        bf2.pNext = nullptr;
        VkDescriptorSetLayoutBindingFlagsCreateInfo bf1 = bf2;
        bf1.pNext = &bf2;
        d = dsl;
        d.pNext = &bf1;
        VKR_CHECK(!icd_subset::descriptor_set_layout_ok(&d, &why));
    }
    {
        // previously out-of-subset shapes are now ACCEPTED (faithfully forwarded) -- an
        // empty layout, a STORAGE_BUFFER type, a COMPUTE stage, and a binding-flags pNext.
        VkDescriptorSetLayoutCreateInfo d = dsl;
        d.bindingCount = 0;
        d.pBindings = nullptr; // empty layout (zink mints these)
        VKR_CHECK(icd_subset::descriptor_set_layout_ok(&d, &why));
        VkDescriptorSetLayoutBinding ok = b;
        ok.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        ok.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        d = dsl;
        d.pBindings = &ok;
        VKR_CHECK(icd_subset::descriptor_set_layout_ok(&d, &why));
        VkDescriptorBindingFlags bf = 0;
        VkDescriptorSetLayoutBindingFlagsCreateInfo bfci{};
        bfci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
        bfci.bindingCount = 1;
        bfci.pBindingFlags = &bf;
        d = dsl;
        d.pNext = &bfci; // binding-flags pNext is accepted (its flags are forwarded)
        VKR_CHECK(icd_subset::descriptor_set_layout_ok(&d, &why));
    }

    // Descriptor pool: exactly one UNIFORM_BUFFER size.
    VkDescriptorPoolSize ps{};
    ps.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    ps.descriptorCount = 4;
    VkDescriptorPoolCreateInfo dp{};
    dp.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dp.maxSets = 4;
    dp.poolSizeCount = 1;
    dp.pPoolSizes = &ps;
    VKR_CHECK(icd_subset::descriptor_pool_ok(&dp, &why));
    {
        // (GL/zink): faithful. maxSets 0, no pool sizes, and a zero-count size are still
        // rejected; FREE_DESCRIPTOR_SET, any type, duplicate types, and >2 sizes are now ACCEPTED
        // (the real pool is the per-type budget authority). descriptorIndexing: the
        // UPDATE_AFTER_BIND pool flag is no longer a structural reject here -- its admission is
        // feature-gated at the ICD entry (0 bits while masked -> fail-closed there).
        VkDescriptorPoolCreateInfo d = dp;
        d.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
        VKR_CHECK(icd_subset::descriptor_pool_ok(&d, &why)); // shape-ok; entry gates it
        d = dp;
        d.maxSets = 0;
        VKR_CHECK(!icd_subset::descriptor_pool_ok(&d, &why));
        d = dp;
        d.poolSizeCount = 0; // must have at least one
        VKR_CHECK(!icd_subset::descriptor_pool_ok(&d, &why));
        VkDescriptorPoolSize zero = ps;
        zero.descriptorCount = 0;
        d = dp;
        d.pPoolSizes = &zero;
        VKR_CHECK(!icd_subset::descriptor_pool_ok(&d, &why));
        d = dp;
        d.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT; // now accepted
        VKR_CHECK(icd_subset::descriptor_pool_ok(&d, &why));
        VkDescriptorPoolSize sb = ps;
        sb.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; // any type now accepted
        d = dp;
        d.pPoolSizes = &sb;
        VKR_CHECK(icd_subset::descriptor_pool_ok(&d, &why));
        VkDescriptorPoolSize three[3] = {ps, ps, ps}; // duplicate types + >2 sizes now accepted
        d = dp;
        d.poolSizeCount = 3;
        d.pPoolSizes = three;
        VKR_CHECK(icd_subset::descriptor_pool_ok(&d, &why));
    }

    // descriptorIndexing: the vkAllocateDescriptorSets pNext shape. A
    // ZERO-count variable-count struct is "as if absent" and needs NO feature; only a nonzero
    // count engages the feature gate + the must-parallel rule.
    {
        VkDescriptorSetLayout fake_layouts[2] = {reinterpret_cast<VkDescriptorSetLayout>(0x10),
                                                 reinterpret_cast<VkDescriptorSetLayout>(0x20)};
        VkDescriptorSetAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorSetCount = 2;
        ai.pSetLayouts = fake_layouts;
        VKR_CHECK(icd_subset::descriptor_set_alloc_ok(&ai, false, &why)); // no pNext
        VkDescriptorSetVariableDescriptorCountAllocateInfo vc{};
        vc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO;
        vc.descriptorSetCount = 0; // as if absent -- accepted WITHOUT the feature
        ai.pNext = &vc;
        VKR_CHECK(icd_subset::descriptor_set_alloc_ok(&ai, false, &why));
        VKR_CHECK(icd_subset::descriptor_set_alloc_ok(&ai, true, &why));
        const std::uint32_t counts[2] = {4, 8};
        vc.descriptorSetCount = 2;
        vc.pDescriptorCounts = counts;
        VKR_CHECK(!icd_subset::descriptor_set_alloc_ok(&ai, false, &why)); // nonzero needs it
        VKR_CHECK(icd_subset::descriptor_set_alloc_ok(&ai, true, &why));
        vc.descriptorSetCount = 1; // nonzero but not parallel to the allocation's 2
        VKR_CHECK(!icd_subset::descriptor_set_alloc_ok(&ai, true, &why));
        vc.descriptorSetCount = 2;
        vc.pDescriptorCounts = nullptr; // nonzero with a null array
        VKR_CHECK(!icd_subset::descriptor_set_alloc_ok(&ai, true, &why));
        ai.pNext = &dummy; // a foreign pNext stays rejected
        VKR_CHECK(!icd_subset::descriptor_set_alloc_ok(&ai, true, &why));
    }

    // bind_descriptor_sets (faithful): GRAPHICS or COMPUTE, any firstSet, dynamic offsets
    // carried. Only structurally-impossible requests are rejected (count 0, firstSet+count > kMax).
    VKR_CHECK(icd_subset::bind_descriptor_sets_ok(VK_PIPELINE_BIND_POINT_GRAPHICS, 0, 1, 0, &why));
    VKR_CHECK(icd_subset::bind_descriptor_sets_ok(VK_PIPELINE_BIND_POINT_COMPUTE, 0, 1, 0, &why));
    VKR_CHECK(icd_subset::bind_descriptor_sets_ok(VK_PIPELINE_BIND_POINT_GRAPHICS, 1, 1, 0, &why));
    VKR_CHECK(icd_subset::bind_descriptor_sets_ok(VK_PIPELINE_BIND_POINT_GRAPHICS, 0, 1, 1, &why));
    VKR_CHECK(icd_subset::bind_descriptor_sets_ok(VK_PIPELINE_BIND_POINT_GRAPHICS, 3, 2, 4, &why));
    // Structurally impossible: count 0, or firstSet+count past the bound.
    VKR_CHECK(!icd_subset::bind_descriptor_sets_ok(VK_PIPELINE_BIND_POINT_GRAPHICS, 0, 0, 0, &why));
    VKR_CHECK(
        !icd_subset::bind_descriptor_sets_ok(VK_PIPELINE_BIND_POINT_GRAPHICS, 31, 5, 0, &why));
    VKR_CHECK(
        !icd_subset::bind_descriptor_sets_ok(static_cast<VkPipelineBindPoint>(99), 0, 1, 0, &why));
}

// Images + depth predicates.
void test_image_depth() {
    const char* why = "";
    int dummy = 0;

    VkImageCreateInfo ic{};
    ic.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ic.imageType = VK_IMAGE_TYPE_2D;
    ic.format = VK_FORMAT_D16_UNORM;
    ic.extent = {256, 256, 1};
    ic.mipLevels = 1;
    ic.arrayLayers = 1;
    ic.samples = VK_SAMPLE_COUNT_1_BIT;
    ic.tiling = VK_IMAGE_TILING_OPTIMAL;
    ic.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    ic.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ic.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VKR_CHECK(icd_subset::image_ok(&ic, &why));
    {
        // (GL/zink): previously out-of-subset image shapes are now ACCEPTED (faithfully
        // forwarded; the host driver gates them) -- 3D, multi-mip, array, MSAA, STORAGE usage,
        // CUBE_COMPATIBLE flags, and a VkImageFormatListCreateInfo pNext.
        VkImageCreateInfo i = ic;
        i.imageType = VK_IMAGE_TYPE_3D;
        VKR_CHECK(icd_subset::image_ok(&i, &why));
        i = ic;
        i.mipLevels = 8;
        VKR_CHECK(icd_subset::image_ok(&i, &why));
        i = ic;
        i.arrayLayers = 6;
        VKR_CHECK(icd_subset::image_ok(&i, &why));
        i = ic;
        i.samples = VK_SAMPLE_COUNT_4_BIT;
        VKR_CHECK(icd_subset::image_ok(&i, &why));
        i = ic;
        i.usage = VK_IMAGE_USAGE_STORAGE_BIT;
        VKR_CHECK(icd_subset::image_ok(&i, &why));
        i = ic;
        i.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
        VKR_CHECK(icd_subset::image_ok(&i, &why));
        VkImageFormatListCreateInfo fl{};
        fl.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO;
        i = ic;
        i.pNext = &fl;
        VKR_CHECK(icd_subset::image_ok(&i, &why)); // format-list pNext accepted
        // Still rejected: a zero extent, zero usage, and a non-format-list pNext.
        i = ic;
        i.extent = {0, 256, 1};
        VKR_CHECK(!icd_subset::image_ok(&i, &why));
        i = ic;
        i.usage = 0;
        VKR_CHECK(!icd_subset::image_ok(&i, &why));
        i = ic;
        i.pNext = &dummy;
        VKR_CHECK(!icd_subset::image_ok(&i, &why));
    }
    // A SAMPLED color texture is in the subset too.
    {
        VkImageCreateInfo tex = ic;
        tex.format = VK_FORMAT_R8G8B8A8_UNORM;
        tex.tiling = VK_IMAGE_TILING_LINEAR;
        tex.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
        tex.initialLayout = VK_IMAGE_LAYOUT_PREINITIALIZED;
        VKR_CHECK(icd_subset::image_ok(&tex, &why));
    }

    // Memory class (Option 1 Tier 1): allocation admits any type the host can allocate
    // EXCEPT protected and host-visible-non-coherent; mappability is enforced later at vkMapMemory.
    VKR_CHECK(icd_subset::memory_class_ok(
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &why));
    VKR_CHECK(icd_subset::memory_class_ok(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &why));
    VKR_CHECK(icd_subset::memory_class_ok(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
                                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                          &why)); // device-local + mappable-coherent is admitted
                                                  // (the HOST_VISIBLE branch is satisfied)
    // flags==0 is now ADMITTED (the NVIDIA propertyFlags==0 device-accessible type -- non-mappable
    // but a legitimate allocation class; this was the sole api.smoke blocker on the RTX 4080).
    VKR_CHECK(icd_subset::memory_class_ok(0, &why));
    // Still fail-closed: HOST_VISIBLE without HOST_COHERENT (unmappable until Tier 2)...
    VKR_CHECK(!icd_subset::memory_class_ok(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, &why));
    // ...and PROTECTED in any combination (no protected queue/submit path), checked before the
    // host-visible branch so PROTECTED|DEVICE_LOCAL is rejected too, not silently admitted.
    VKR_CHECK(!icd_subset::memory_class_ok(VK_MEMORY_PROPERTY_PROTECTED_BIT, &why));
    VKR_CHECK(!icd_subset::memory_class_ok(
        VK_MEMORY_PROPERTY_PROTECTED_BIT | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &why));
}

// Sampler + texture-upload predicates.
void test_sampler_copy() {
    const char* why = "";
    int dummy = 0;

    // (GL/zink): a general GL sampler is forwarded faithfully -- LINEAR/mipmap/REPEAT/
    // anisotropy/compare/LOD/unnormalized all ride the wire (the worker validates against the
    // host); only the un-carried pNext/flags are fail-closed here.
    VkSamplerCreateInfo sc{};
    sc.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sc.magFilter = VK_FILTER_LINEAR;
    sc.minFilter = VK_FILTER_LINEAR;
    sc.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sc.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sc.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sc.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sc.maxLod = 8.0f;
    sc.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    VKR_CHECK(icd_subset::sampler_ok(&sc, &why)); // general sampler accepted now
    {
        // pNext + flags remain out of subset (not carried to the worker).
        VkSamplerCreateInfo s = sc;
        s.flags = 1;
        VKR_CHECK(!icd_subset::sampler_ok(&s, &why));
        s = sc;
        s.pNext = &dummy;
        VKR_CHECK(!icd_subset::sampler_ok(&s, &why));
    }

    // copy_buffer_to_image, widened faithful (the ExtremeTuxRacer texture-upload fix + the
    // depth/stencil upload widening): sub-region offsets, mips, layers, buffer strides,
    // multi-region, AND any single legal transfer aspect (COLOR/DEPTH/STENCIL) are all ADMITTED
    // (carried on the wire; the worker checks aspect-vs-image-format + bounds). What stays
    // fail-closed HERE is named -- a non-single/illegal aspect (0, multi-bit, plane/metadata),
    // non-2D shapes, illegal dst layouts, sub-extent strides.
    VkBufferImageCopy region{};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {16, 16, 1};
    VKR_CHECK(icd_subset::copy_buffer_to_image_ok(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region,
                                                  &why));
    {
        // The old fixed-subset rejects that are now ADMITTED (the widening's whole point):
        VKR_CHECK(icd_subset::copy_buffer_to_image_ok(VK_IMAGE_LAYOUT_GENERAL, 1, &region,
                                                      &why)); // GENERAL is spec-legal
        VkBufferImageCopy r = region;
        r.imageOffset = {3, 5, 0}; // sub-region destination (glTexSubImage2D -- the etr reject)
        VKR_CHECK(
            icd_subset::copy_buffer_to_image_ok(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r, &why));
        r = region;
        r.bufferOffset = 16; // staging suballocation
        r.bufferRowLength = 64;
        r.bufferImageHeight = 32;
        VKR_CHECK(
            icd_subset::copy_buffer_to_image_ok(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r, &why));
        r = region;
        r.imageSubresource.mipLevel = 3; // mip upload (worker bounds-checks vs the image)
        r.imageSubresource.baseArrayLayer = 1;
        r.imageSubresource.layerCount = 2;
        VKR_CHECK(
            icd_subset::copy_buffer_to_image_ok(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r, &why));
        VkBufferImageCopy two[2] = {region, region}; // multi-region batches
        two[1].imageOffset = {8, 8, 0};
        VKR_CHECK(icd_subset::copy_buffer_to_image_ok(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 2, two,
                                                      &why));
        r = region;
        r.imageSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT; // depth upload now carried
        VKR_CHECK(
            icd_subset::copy_buffer_to_image_ok(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r, &why));
        r = region;
        r.imageSubresource.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT; // stencil upload now carried
        VKR_CHECK(
            icd_subset::copy_buffer_to_image_ok(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r, &why));

        // The named fail-closed remainder:
        VKR_CHECK(!icd_subset::copy_buffer_to_image_ok(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 1,
                                                       &region, &why)); // illegal dst layout
        VKR_CHECK(!icd_subset::copy_buffer_to_image_ok(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                                                       &region, &why)); // zero regions
        VKR_CHECK(!icd_subset::copy_buffer_to_image_ok(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                                                       nullptr, &why)); // null regions
        r = region;
        r.imageSubresource.aspectMask = 0; // no aspect
        VKR_CHECK(!icd_subset::copy_buffer_to_image_ok(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r,
                                                       &why));
        r = region;
        r.imageSubresource.aspectMask =
            VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT; // not a single copy aspect
        VKR_CHECK(!icd_subset::copy_buffer_to_image_ok(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r,
                                                       &why));
        r = region;
        r.imageSubresource.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT; // plane aspect not admitted
        VKR_CHECK(!icd_subset::copy_buffer_to_image_ok(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r,
                                                       &why));
        r = region;
        r.imageSubresource.layerCount = 0;
        VKR_CHECK(!icd_subset::copy_buffer_to_image_ok(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r,
                                                       &why));
        r = region;
        r.imageExtent = {16, 16, 2}; // 3D copy out of the 2D scope
        VKR_CHECK(!icd_subset::copy_buffer_to_image_ok(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r,
                                                       &why));
        r = region;
        r.imageExtent = {0, 16, 1}; // degenerate extent
        VKR_CHECK(!icd_subset::copy_buffer_to_image_ok(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r,
                                                       &why));
        r = region;
        r.imageOffset = {-1, 0, 0}; // negative offset
        VKR_CHECK(!icd_subset::copy_buffer_to_image_ok(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r,
                                                       &why));
        r = region;
        r.imageOffset = {0, 0, 1}; // z out of the 2D scope
        VKR_CHECK(!icd_subset::copy_buffer_to_image_ok(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r,
                                                       &why));
        r = region;
        r.bufferRowLength = 8; // spec VU: 0 or >= extent width (16)
        VKR_CHECK(!icd_subset::copy_buffer_to_image_ok(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r,
                                                       &why));
        r = region;
        r.bufferImageHeight = 8; // spec VU: 0 or >= extent height (16)
        VKR_CHECK(!icd_subset::copy_buffer_to_image_ok(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r,
                                                       &why));
        // A multi-region call is rejected when ANY region violates a rule (a both-aspects region is
        // not a legal single copy aspect, even though DEPTH alone now IS admitted).
        VkBufferImageCopy bad2[2] = {region, region};
        bad2[1].imageSubresource.aspectMask =
            VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        VKR_CHECK(!icd_subset::copy_buffer_to_image_ok(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 2,
                                                       bad2, &why));
    }
}

// --- Compute pipelines + dispatch ------------------------------------------------------------
void test_compute_subset() {
    const char* why = "";
    // bind_pipeline: GRAPHICS and COMPUTE admitted; anything else stays a named reject.
    VKR_CHECK(icd_subset::bind_pipeline_ok(VK_PIPELINE_BIND_POINT_GRAPHICS, &why));
    VKR_CHECK(icd_subset::bind_pipeline_ok(VK_PIPELINE_BIND_POINT_COMPUTE, &why));
    VKR_CHECK(!icd_subset::bind_pipeline_ok(VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, &why));

    // compute_pipeline_ok: the in-subset shape, then one named reject per fail-closed field.
    VkComputePipelineCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = reinterpret_cast<VkShaderModule>(0x1);
    ci.stage.pName = "main";
    ci.basePipelineIndex = -1;
    VKR_CHECK(icd_subset::compute_pipeline_ok(&ci, false, false, false, false, &why));
    {
        auto c = ci;
        c.basePipelineIndex = 0; // ignored without the DERIVATIVE flag (apps zero-init it)
        VKR_CHECK(icd_subset::compute_pipeline_ok(&c, false, false, false, false, &why));
    }
    int dummy = 0;
    {
        auto c = ci;
        c.pNext = &dummy;
        VKR_CHECK(!icd_subset::compute_pipeline_ok(&c, false, false, false, false, &why));
    }
    {
        auto c = ci;
        c.flags = VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT;
        VKR_CHECK(!icd_subset::compute_pipeline_ok(&c, false, false, false, false, &why));
    }
    {
        auto c = ci;
        c.basePipelineHandle = reinterpret_cast<VkPipeline>(0x2); // derivatives
        VKR_CHECK(!icd_subset::compute_pipeline_ok(&c, false, false, false, false, &why));
    }
    {
        auto c = ci;
        c.stage.pNext = &dummy;
        VKR_CHECK(!icd_subset::compute_pipeline_ok(&c, false, false, false, false, &why));
    }
    VkSpecializationInfo spec{};
    {
        auto c = ci;
        c.stage.pSpecializationInfo = &spec; // named reject (the known fast-follow)
        VKR_CHECK(!icd_subset::compute_pipeline_ok(&c, false, false, false, false, &why));
    }
    {
        auto c = ci;
        c.stage.stage = VK_SHADER_STAGE_VERTEX_BIT; // not COMPUTE
        VKR_CHECK(!icd_subset::compute_pipeline_ok(&c, false, false, false, false, &why));
    }
    {
        auto c = ci;
        c.stage.module = VK_NULL_HANDLE;
        VKR_CHECK(!icd_subset::compute_pipeline_ok(&c, false, false, false, false, &why));
    }
    {
        auto c = ci;
        c.stage.pName = "";
        VKR_CHECK(!icd_subset::compute_pipeline_ok(&c, false, false, false, false, &why));
    }
}

// --- Surface-caps cache state machine (icd_caps_cache.h) --------------------
// Pure-header pins per the locked design (test list): live-swapchain gating,
// (physical_device, surface) keying, per-rule invalidation, the disabled escape hatch, counters.
// A plain int stands in for the caps value -- the template keeps the machine type-agnostic.
void test_caps_cache() {
    using Cache = vkr::icd_caps::CapsCache<int>;
    constexpr std::uint64_t kPd1 = 11, kPd2 = 12; // two physical devices
    constexpr std::uint64_t kSurfA = 100, kSurfB = 200;
    constexpr std::uint64_t kScA1 = 1001, kScA2 = 1002, kScB1 = 2001;

    // Bring-up stays uncached: no live swapchain -> store is a no-op and lookup misses.
    {
        Cache c(true);
        c.store(kPd1, kSurfA, 7);
        VKR_CHECK(c.lookup(kPd1, kSurfA) == nullptr);
        VKR_CHECK(!c.is_live(kSurfA));
        VKR_CHECK_EQ(c.hits(), 0u);
        VKR_CHECK_EQ(c.misses(), 1u);
    }
    // Live surface: store -> hit; keying separates two physical devices on ONE surface.
    {
        Cache c(true);
        c.on_swapchain_created(kScA1, kSurfA);
        VKR_CHECK(c.is_live(kSurfA));
        VKR_CHECK_EQ(c.surface_of(kScA1), kSurfA);
        c.store(kPd1, kSurfA, 7);
        c.store(kPd2, kSurfA, 8);
        const int* v1 = c.lookup(kPd1, kSurfA);
        const int* v2 = c.lookup(kPd2, kSurfA);
        VKR_CHECK(v1 != nullptr && *v1 == 7);
        VKR_CHECK(v2 != nullptr && *v2 == 8);
        VKR_CHECK_EQ(c.hits(), 2u);
        // A THIRD device never stored still misses (no cross-device bleed).
        VKR_CHECK(c.lookup(31, kSurfA) == nullptr);
    }
    // Swapchain CREATE invalidates (recreate flow: the first poll after create re-queries),
    // and surface-wide: both physical devices' entries drop.
    {
        Cache c(true);
        c.on_swapchain_created(kScA1, kSurfA);
        c.store(kPd1, kSurfA, 7);
        c.store(kPd2, kSurfA, 8);
        c.on_swapchain_created(kScA2, kSurfA);
        VKR_CHECK(c.lookup(kPd1, kSurfA) == nullptr);
        VKR_CHECK(c.lookup(kPd2, kSurfA) == nullptr);
        VKR_CHECK_EQ(c.invalidations(), 2u); // erased entries, not events
        c.store(kPd1, kSurfA, 9);            // re-primes (still live via both swapchains)
        const int* v = c.lookup(kPd1, kSurfA);
        VKR_CHECK(v != nullptr && *v == 9);
    }
    // Destroying the LAST swapchain drops liveness: entry gone AND stores gate off again.
    {
        Cache c(true);
        c.on_swapchain_created(kScA1, kSurfA);
        c.store(kPd1, kSurfA, 7);
        c.on_swapchain_destroyed(kScA1);
        VKR_CHECK(!c.is_live(kSurfA));
        VKR_CHECK(c.lookup(kPd1, kSurfA) == nullptr);
        c.store(kPd1, kSurfA, 7);
        VKR_CHECK(c.lookup(kPd1, kSurfA) == nullptr);
        VKR_CHECK_EQ(c.surface_of(kScA1), 0u);
        c.on_swapchain_destroyed(kScA1); // unknown handle now: a no-op
    }
    // Destroying ONE of two swapchains keeps the surface live (still presented-to) but
    // invalidates; a re-store then serves again.
    {
        Cache c(true);
        c.on_swapchain_created(kScA1, kSurfA);
        c.on_swapchain_created(kScA2, kSurfA);
        c.store(kPd1, kSurfA, 7);
        c.on_swapchain_destroyed(kScA1);
        VKR_CHECK(c.is_live(kSurfA));
        VKR_CHECK(c.lookup(kPd1, kSurfA) == nullptr);
        c.store(kPd1, kSurfA, 8);
        const int* v = c.lookup(kPd1, kSurfA);
        VKR_CHECK(v != nullptr && *v == 8);
    }
    // Result-signal invalidation targets exactly the touched swapchain's surface: surface B's
    // entry survives an invalidation on surface A's swapchain (the per-target present rule).
    {
        Cache c(true);
        c.on_swapchain_created(kScA1, kSurfA);
        c.on_swapchain_created(kScB1, kSurfB);
        c.store(kPd1, kSurfA, 7);
        c.store(kPd1, kSurfB, 8);
        c.invalidate_swapchain(kScA1);
        VKR_CHECK(c.lookup(kPd1, kSurfA) == nullptr);
        const int* vb = c.lookup(kPd1, kSurfB);
        VKR_CHECK(vb != nullptr && *vb == 8);
        c.invalidate_swapchain(9999); // unknown swapchain: a no-op, B still cached
        const int* vb2 = c.lookup(kPd1, kSurfB);
        VKR_CHECK(vb2 != nullptr && *vb2 == 8);
    }
    // Surface destroy erases EVERYTHING for the surface: mappings, liveness, cache.
    {
        Cache c(true);
        c.on_swapchain_created(kScA1, kSurfA);
        c.on_swapchain_created(kScA2, kSurfA);
        c.store(kPd1, kSurfA, 7);
        c.on_surface_destroyed(kSurfA);
        VKR_CHECK(!c.is_live(kSurfA));
        VKR_CHECK_EQ(c.surface_of(kScA1), 0u);
        VKR_CHECK_EQ(c.surface_of(kScA2), 0u);
        VKR_CHECK(c.lookup(kPd1, kSurfA) == nullptr);
    }
    // Disabled (VKRELAY2_NO_CAPS_CACHE=1): every lookup misses even with a live swapchain and a
    // would-be store; lifecycle/invalidation hooks stay harmless no-ops on the empty cache.
    {
        Cache c(false);
        c.on_swapchain_created(kScA1, kSurfA);
        c.store(kPd1, kSurfA, 7);
        VKR_CHECK(c.lookup(kPd1, kSurfA) == nullptr);
        c.invalidate_swapchain(kScA1);
        VKR_CHECK_EQ(c.hits(), 0u);
        VKR_CHECK_EQ(c.invalidations(), 0u);
        VKR_CHECK_EQ(c.misses(), 1u);
    }
}

// The Vulkan-1.3 opener: the native-lane predicate. ONLY the exact marker "1" selects the
// native lane; every other value (nullptr / "" / "0" / a stray string) is the default zink-safe
// lane -- strict, so a contaminated/garbage env can never uncap the steering.
void test_native_lane_policy() {
    using vkr::icd_policy::native_lane_enabled;
    VKR_CHECK(native_lane_enabled("1"));
    VKR_CHECK(!native_lane_enabled(nullptr)); // unset -> default lane
    VKR_CHECK(!native_lane_enabled(""));
    VKR_CHECK(!native_lane_enabled("0"));
    VKR_CHECK(!native_lane_enabled("2"));
    VKR_CHECK(!native_lane_enabled("true"));
    VKR_CHECK(!native_lane_enabled("1 ")); // no trimming -- exact match only
    VKR_CHECK(!native_lane_enabled("yes"));
}

} // namespace

int main() {
    test_native_lane_policy();
    test_simple_create_infos();
    test_render_pass();
    test_render_pass2();
    test_graphics_pipeline();
    test_commands();
    test_memory_buffer();
    test_descriptor_surface();
    test_image_depth();
    test_sampler_copy();
    test_caps_cache();
    test_compute_subset();
    return vkr::test::finish("unit_icd_subset");
}
