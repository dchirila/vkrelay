// In-process real-backend test for the bufferless-triangle DRAW spine.
//
// Constructs RealVulkanBackend directly and drives the whole draw surface against the host driver
// on the real GPU: image views over the swapchain images, the two pre-compiled SPIR-V shader
// modules (tests/triangle_spv.h), a single-color render pass (UNDEFINED -> PRESENT_SRC_KHR),
// framebuffers, an empty pipeline layout, a bufferless dynamic-VIEWPORT/SCISSOR graphics pipeline,
// then a real frame: acquire -> record(begin/bind/setViewport/setScissor/draw(3)/end) -> submit ->
// wait fence -> present. The triangle emits its 3 vertices from gl_VertexIndex, so there is NO
// vertex buffer and no device memory -- the entire mapped-memory surface is out of scope here (it
// belongs to the VBO / vkcube path).
//
// Windows-only (registered under WIN32 + Vulkan SDK). Skips gracefully (still passes) if there is
// no usable Vulkan device / no WSI support for the hidden window. The validation-clean gate is a
// manual run with VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation (grep stderr for Validation Error
// or VUID).
#include "windows/worker/real_vulkan_backend.hpp"

#include "common/vkrpc/vulkan_session.hpp"
#include "tests/test_assert.hpp"
#include "tests/triangle_spv.h"

#include <cstdint>
#include <cstdio>
#include <vector>

#include <vulkan/vulkan.h>

using namespace vkr;

int main() {
    worker::RealVulkanBackend backend("", "", false);

    const vkrpc::CreateInstanceResponse ci = backend.create_instance({});
    if (!ci.ok) {
        std::fprintf(stderr, "integration_real_draw: skipped (no instance: %s)\n",
                     ci.reason.c_str());
        return vkr::test::finish("integration_real_draw");
    }
    vkrpc::EnumeratePhysicalDevicesRequest er;
    er.instance = ci.instance;
    const vkrpc::EnumeratePhysicalDevicesResponse en = backend.enumerate_physical_devices(er);
    if (!en.ok || en.devices.empty()) {
        std::fprintf(stderr, "integration_real_draw: skipped (no physical device)\n");
        return vkr::test::finish("integration_real_draw");
    }
    vkrpc::CreateDeviceRequest cdr;
    cdr.instance = ci.instance;
    cdr.physical_device = en.devices.front().handle;
    const vkrpc::CreateDeviceResponse cd = backend.create_device(cdr);
    VKR_CHECK(cd.ok);
    vkrpc::GetDeviceQueueRequest gqr;
    gqr.device = cd.device;
    gqr.queue_family_index = cd.queue_family_index;
    gqr.queue_index = 0;
    const vkrpc::GetDeviceQueueResponse q = backend.get_device_queue(gqr);
    VKR_CHECK(q.ok && q.queue != 0);

    vkrpc::CreateSurfaceRequest sr;
    sr.instance = ci.instance;
    const vkrpc::CreateSurfaceResponse surf = backend.create_surface(sr);
    VKR_CHECK(surf.ok);

    vkrpc::GetSurfaceFormatsRequest sfmt_req;
    sfmt_req.physical_device = en.devices.front().handle;
    sfmt_req.surface = surf.surface;
    const vkrpc::GetSurfaceFormatsResponse sfmt = backend.get_surface_formats(sfmt_req);
    VKR_CHECK(sfmt.ok && !sfmt.formats.empty());
    vkrpc::GetSurfaceCapabilitiesRequest scap_req;
    scap_req.physical_device = en.devices.front().handle;
    scap_req.surface = surf.surface;
    const vkrpc::GetSurfaceCapabilitiesResponse scap = backend.get_surface_capabilities(scap_req);
    VKR_CHECK(scap.ok);
    vkrpc::GetSurfacePresentModesRequest spm_req;
    spm_req.physical_device = en.devices.front().handle;
    spm_req.surface = surf.surface;
    const vkrpc::GetSurfacePresentModesResponse spm = backend.get_surface_present_modes(spm_req);
    VKR_CHECK(spm.ok && !spm.present_modes.empty());

    // Prefer B8G8R8A8_UNORM + FIFO, else the first advertised; usage is COLOR_ATTACHMENT (the draw
    // path needs no TRANSFER_DST).
    int chosen_format = sfmt.formats.front().format;
    int chosen_color_space = sfmt.formats.front().color_space;
    for (const auto& f : sfmt.formats) {
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM && f.color_space == 0) {
            chosen_format = f.format;
            chosen_color_space = f.color_space;
            break;
        }
    }
    int chosen_present_mode = spm.present_modes.front();
    for (const int m : spm.present_modes) {
        if (m == VK_PRESENT_MODE_FIFO_KHR) {
            chosen_present_mode = m;
            break;
        }
    }
    const int kExtent = 256;
    vkrpc::CreateSwapchainRequest scr;
    scr.device = cd.device;
    scr.surface = surf.surface;
    scr.image_format = chosen_format;
    scr.color_space = chosen_color_space;
    scr.present_mode = chosen_present_mode;
    scr.width = kExtent;
    scr.height = kExtent;
    scr.min_image_count = static_cast<int>(scap.min_image_count);
    scr.image_usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    const vkrpc::CreateSwapchainResponse sc = backend.create_swapchain(scr);
    if (!sc.ok) {
        std::fprintf(stderr, "integration_real_draw: skipped (no swapchain: %s)\n",
                     sc.reason.c_str());
        return vkr::test::finish("integration_real_draw");
    }
    vkrpc::GetSwapchainImagesRequest gir;
    gir.swapchain = sc.swapchain;
    const vkrpc::GetSwapchainImagesResponse imgs = backend.get_swapchain_images(gir);
    VKR_CHECK(imgs.ok && !imgs.images.empty());

    // Image views + the render pass (single color attachment, UNDEFINED -> PRESENT_SRC_KHR, no
    // explicit dependency -- the acquire semaphore waited at COLOR_ATTACHMENT_OUTPUT plus the
    // implicit subpass dependency makes the transition validation-clean) + a framebuffer per image.
    std::vector<std::uint64_t> views;
    for (const std::uint64_t image : imgs.images) {
        vkrpc::CreateImageViewRequest ivr;
        ivr.image = image;
        ivr.view_type = VK_IMAGE_VIEW_TYPE_2D;
        ivr.format = chosen_format;
        ivr.swizzle_r = ivr.swizzle_g = ivr.swizzle_b = ivr.swizzle_a =
            VK_COMPONENT_SWIZZLE_IDENTITY;
        ivr.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
        ivr.base_mip_level = 0;
        ivr.level_count = 1;
        ivr.base_array_layer = 0;
        ivr.layer_count = 1;
        const vkrpc::CreateImageViewResponse iv = backend.create_image_view(ivr);
        VKR_CHECK(iv.ok && iv.image_view != 0);
        views.push_back(iv.image_view);
    }

    auto make_shader = [&](const std::uint32_t* spv, std::uint32_t bytes) {
        vkrpc::CreateShaderModuleRequest r;
        r.device = cd.device;
        r.code.assign(reinterpret_cast<const char*>(spv), bytes);
        r.code_size = bytes;
        return r;
    };
    const vkrpc::CreateShaderModuleResponse vs =
        backend.create_shader_module(make_shader(triangle_spv::kVert, triangle_spv::kVertBytes));
    const vkrpc::CreateShaderModuleResponse fs =
        backend.create_shader_module(make_shader(triangle_spv::kFrag, triangle_spv::kFragBytes));
    VKR_CHECK(vs.ok && fs.ok);

    vkrpc::CreateRenderPassRequest rpr;
    rpr.device = cd.device;
    vkrpc::AttachmentDesc att;
    att.format = chosen_format;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.load_op = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att.store_op = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencil_load_op = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencil_store_op = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    att.final_layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    rpr.attachments.push_back(att);
    rpr.color_attachment = 0;
    rpr.color_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    const vkrpc::CreateRenderPassResponse rp = backend.create_render_pass(rpr);
    VKR_CHECK(rp.ok && rp.render_pass != 0);

    std::vector<std::uint64_t> framebuffers;
    for (const std::uint64_t view : views) {
        vkrpc::CreateFramebufferRequest fbr;
        fbr.device = cd.device;
        fbr.render_pass = rp.render_pass;
        fbr.image_view = view;
        fbr.width = kExtent;
        fbr.height = kExtent;
        fbr.layers = 1;
        const vkrpc::CreateFramebufferResponse fb = backend.create_framebuffer(fbr);
        VKR_CHECK(fb.ok && fb.framebuffer != 0);
        framebuffers.push_back(fb.framebuffer);
    }

    vkrpc::CreatePipelineLayoutRequest plr;
    plr.device = cd.device;
    plr.set_layout_count = 0;
    plr.push_constant_range_count = 0;
    const vkrpc::CreatePipelineLayoutResponse pl = backend.create_pipeline_layout(plr);
    VKR_CHECK(pl.ok && pl.pipeline_layout != 0);

    vkrpc::CreateGraphicsPipelinesRequest gpr;
    gpr.device = cd.device;
    gpr.pipeline_cache = 0;
    vkrpc::ShaderStageDesc vstage;
    vstage.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vstage.module = vs.shader_module;
    vstage.entry = "main";
    vkrpc::ShaderStageDesc fstage;
    fstage.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    fstage.module = fs.shader_module;
    fstage.entry = "main";
    gpr.stages = {vstage, fstage};
    gpr.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    gpr.vertex_binding_count = 0;
    gpr.vertex_attribute_count = 0;
    gpr.cull_mode = VK_CULL_MODE_BACK_BIT;
    gpr.front_face = VK_FRONT_FACE_CLOCKWISE;
    gpr.dynamic_states = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    gpr.layout = pl.pipeline_layout;
    gpr.render_pass = rp.render_pass;
    gpr.subpass = 0;
    const vkrpc::CreateGraphicsPipelinesResponse gp = backend.create_graphics_pipelines(gpr);
    VKR_CHECK(gp.ok && gp.pipeline != 0);

    // geometry-stream (real-GPU proof): a SECOND device with
    // VK_EXT_transform_feedback + the geometryStreams feature (chain + scalar, agreement-checked
    // by the worker), then a real graphics pipeline carrying the stream pNext with EXPLICIT stream
    // zero -- the host vkCreateGraphicsPipelines itself exercises admission, wire encoding,
    // reconstruction, and the VUID-valid property gating. Skips (without failing) on a host whose
    // device lacks the extension. Also pins the create_device scalar/chain mismatch rejections in
    // both directions, a capability-conditioned nonzero-stream attempt, and the OLD-ICD payload
    // (omitted scalar + TRUE chain -> derive).
    {
        VkPhysicalDeviceTransformFeedbackFeaturesEXT tf{};
        tf.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT;
        tf.transformFeedback = VK_TRUE;
        tf.geometryStreams = VK_TRUE;
        vkrpc::CapabilityChainEntry tf_entry;
        tf_entry.s_type = static_cast<std::uint32_t>(
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT);
        tf_entry.size = sizeof(tf);
        tf_entry.blob.assign(reinterpret_cast<const char*>(&tf), sizeof(tf));
        vkrpc::CreateDeviceRequest scdr;
        scdr.instance = ci.instance;
        scdr.physical_device = en.devices.front().handle;
        scdr.enabled_extensions = {"VK_EXT_transform_feedback"};
        scdr.geometry_streams_feature_enabled = 1;
        scdr.enabled_feature_chain = {tf_entry};
        // Mismatch negatives FIRST (no device created): scalar without the chain, chain without
        // the scalar -- both directions reject before host vkCreateDevice.
        {
            vkrpc::CreateDeviceRequest bad = scdr;
            bad.enabled_feature_chain.clear();
            VKR_CHECK(!backend.create_device(bad).ok);
            bad = scdr;
            bad.geometry_streams_feature_enabled = 0;
            VKR_CHECK(!backend.create_device(bad).ok);
            // The INVALID scalar (what a forged/transmitted -1 decodes to)
            // rejects by name -- legacy status cannot be claimed past the normalization boundary.
            bad = scdr;
            bad.geometry_streams_feature_enabled = vkrpc::kGeometryStreamsScalarInvalid;
            const vkrpc::CreateDeviceResponse invr = backend.create_device(bad);
            VKR_CHECK(!invr.ok);
            VKR_CHECK(invr.reason.find("must be 0 or 1 when present") != std::string::npos);
        }
        const vkrpc::CreateDeviceResponse scd = backend.create_device(scdr);
        if (!scd.ok) {
            std::fprintf(stderr, "integration_real_draw: geometry-stream section skipped (%s)\n",
                         scd.reason.c_str());
        } else {
            const vkrpc::CreateShaderModuleResponse svs = backend.create_shader_module([&]() {
                auto r = make_shader(triangle_spv::kVert, triangle_spv::kVertBytes);
                r.device = scd.device;
                return r;
            }());
            const vkrpc::CreateShaderModuleResponse sfs = backend.create_shader_module([&]() {
                auto r = make_shader(triangle_spv::kFrag, triangle_spv::kFragBytes);
                r.device = scd.device;
                return r;
            }());
            VKR_CHECK(svs.ok && sfs.ok);
            vkrpc::CreateRenderPassRequest srpr = rpr;
            srpr.device = scd.device;
            const vkrpc::CreateRenderPassResponse srp = backend.create_render_pass(srpr);
            VKR_CHECK(srp.ok);
            vkrpc::CreatePipelineLayoutRequest splr;
            splr.device = scd.device;
            splr.set_layout_count = 0;
            splr.push_constant_range_count = 0;
            const vkrpc::CreatePipelineLayoutResponse spl = backend.create_pipeline_layout(splr);
            VKR_CHECK(spl.ok);
            vkrpc::CreateGraphicsPipelinesRequest sgpr = gpr;
            sgpr.device = scd.device;
            sgpr.stages[0].module = svs.shader_module;
            sgpr.stages[1].module = sfs.shader_module;
            sgpr.layout = spl.pipeline_layout;
            sgpr.render_pass = srp.render_pass;
            sgpr.stream_state_present = 1;
            sgpr.rasterization_stream = 0; // explicit stream zero -- the named case
            const vkrpc::CreateGraphicsPipelinesResponse sgp =
                backend.create_graphics_pipelines(sgpr);
            VKR_CHECK(sgp.ok && sgp.pipeline != 0);
            // Capability-conditioned nonzero stream: succeeds where the host advertises
            // StreamSelect + >1 streams; otherwise it must be the NAMED shared-validator reject
            // (never a crash/fault). The platform-neutral negative policy is pinned in unit_vkrpc;
            // this only proves the real path stays fail-closed on a host without the capability.
            vkrpc::CreateGraphicsPipelinesRequest sgpr1 = sgpr;
            sgpr1.rasterization_stream = 1;
            const vkrpc::CreateGraphicsPipelinesResponse sgp1 =
                backend.create_graphics_pipelines(sgpr1);
            if (sgp1.ok) {
                std::fprintf(stderr, "integration_real_draw: nonzero stream accepted (host has "
                                     "StreamSelect)\n");
                vkrpc::HandleRequest dh;
                dh.handle = sgp1.pipeline;
                VKR_CHECK(backend.destroy_pipeline(dh).ok);
            } else {
                VKR_CHECK(sgp1.reason.find("rasterization stream") != std::string::npos ||
                          sgp1.reason.find("StreamSelect") != std::string::npos);
            }
            vkrpc::HandleRequest sh;
            sh.handle = sgp.pipeline;
            VKR_CHECK(backend.destroy_pipeline(sh).ok);
            sh.handle = spl.pipeline_layout;
            VKR_CHECK(backend.destroy_pipeline_layout(sh).ok);
            sh.handle = srp.render_pass;
            VKR_CHECK(backend.destroy_render_pass(sh).ok);
            sh.handle = svs.shader_module;
            VKR_CHECK(backend.destroy_shader_module(sh).ok);
            sh.handle = sfs.shader_module;
            VKR_CHECK(backend.destroy_shader_module(sh).ok);
            sh.handle = scd.device;
            VKR_CHECK(backend.destroy_device(sh).ok);

            // The OLD-ICD payload. An older ICD already forwards this same
            // TRUE feature chain but has NO scalar key (decoding as the -1 omitted sentinel) --
            // the worker must DERIVE the enabled state from the chain instead of
            // mismatch-rejecting the old payload, and a stream-zero pipeline on the resulting
            // device proves the derived state actually landed (not merely that create_device
            // stopped rejecting).
            vkrpc::CreateDeviceRequest old_cdr = scdr;
            // The trusted direct-backend fixture models wire-key absence with the named sentinel;
            // an RPC client cannot transmit this state.
            old_cdr.geometry_streams_feature_enabled = vkrpc::kGeometryStreamsScalarOmitted;
            const vkrpc::CreateDeviceResponse old_cd = backend.create_device(old_cdr);
            VKR_CHECK(old_cd.ok);
            const vkrpc::CreateShaderModuleResponse ovs = backend.create_shader_module([&]() {
                auto r = make_shader(triangle_spv::kVert, triangle_spv::kVertBytes);
                r.device = old_cd.device;
                return r;
            }());
            const vkrpc::CreateShaderModuleResponse ofs = backend.create_shader_module([&]() {
                auto r = make_shader(triangle_spv::kFrag, triangle_spv::kFragBytes);
                r.device = old_cd.device;
                return r;
            }());
            VKR_CHECK(ovs.ok && ofs.ok);
            vkrpc::CreateRenderPassRequest orpr = rpr;
            orpr.device = old_cd.device;
            const vkrpc::CreateRenderPassResponse orp = backend.create_render_pass(orpr);
            VKR_CHECK(orp.ok);
            vkrpc::CreatePipelineLayoutRequest oplr;
            oplr.device = old_cd.device;
            oplr.set_layout_count = 0;
            oplr.push_constant_range_count = 0;
            const vkrpc::CreatePipelineLayoutResponse opl = backend.create_pipeline_layout(oplr);
            VKR_CHECK(opl.ok);
            vkrpc::CreateGraphicsPipelinesRequest ogpr = gpr;
            ogpr.device = old_cd.device;
            ogpr.stages[0].module = ovs.shader_module;
            ogpr.stages[1].module = ofs.shader_module;
            ogpr.layout = opl.pipeline_layout;
            ogpr.render_pass = orp.render_pass;
            ogpr.stream_state_present = 1;
            ogpr.rasterization_stream = 0;
            const vkrpc::CreateGraphicsPipelinesResponse ogp =
                backend.create_graphics_pipelines(ogpr);
            VKR_CHECK(ogp.ok && ogp.pipeline != 0);
            vkrpc::HandleRequest oh;
            oh.handle = ogp.pipeline;
            VKR_CHECK(backend.destroy_pipeline(oh).ok);
            oh.handle = opl.pipeline_layout;
            VKR_CHECK(backend.destroy_pipeline_layout(oh).ok);
            oh.handle = orp.render_pass;
            VKR_CHECK(backend.destroy_render_pass(oh).ok);
            oh.handle = ovs.shader_module;
            VKR_CHECK(backend.destroy_shader_module(oh).ok);
            oh.handle = ofs.shader_module;
            VKR_CHECK(backend.destroy_shader_module(oh).ok);
            oh.handle = old_cd.device;
            VKR_CHECK(backend.destroy_device(oh).ok);
        }
    }

    // Command pool + buffer + sync objects for the frame.
    vkrpc::CreateCommandPoolRequest cpr;
    cpr.device = cd.device;
    cpr.queue_family_index = cd.queue_family_index;
    const vkrpc::CreateCommandPoolResponse pool = backend.create_command_pool(cpr);
    VKR_CHECK(pool.ok);
    vkrpc::AllocateCommandBuffersRequest abr;
    abr.command_pool = pool.command_pool;
    abr.count = 1;
    const vkrpc::AllocateCommandBuffersResponse bufs = backend.allocate_command_buffers(abr);
    VKR_CHECK(bufs.ok && bufs.command_buffers.size() == 1);
    const std::uint64_t cmd = bufs.command_buffers[0];
    vkrpc::CreateSemaphoreRequest csr;
    csr.device = cd.device;
    const vkrpc::CreateSemaphoreResponse sem_acq = backend.create_semaphore(csr);
    const vkrpc::CreateSemaphoreResponse sem_done = backend.create_semaphore(csr);
    VKR_CHECK(sem_acq.ok && sem_done.ok);
    vkrpc::CreateFenceRequest cfr;
    cfr.device = cd.device;
    const vkrpc::CreateFenceResponse fence = backend.create_fence(cfr);
    VKR_CHECK(fence.ok);

    // The real frame: acquire -> record the bufferless triangle -> submit -> wait -> present.
    vkrpc::AcquireNextImageRequest ar;
    ar.swapchain = sc.swapchain;
    ar.timeout = UINT64_MAX;
    ar.semaphore = sem_acq.semaphore;
    const vkrpc::AcquireNextImageResponse acq = backend.acquire_next_image(ar);
    VKR_CHECK(acq.ok);
    VKR_CHECK_EQ(acq.result, vkrpc::kVkSuccess);
    const int idx = acq.image_index;
    VKR_CHECK(idx >= 0 && static_cast<std::size_t>(idx) < framebuffers.size());

    vkrpc::RecordCommandBufferRequest rec;
    rec.command_buffer = cmd;
    rec.one_time_submit = true;
    {
        vkrpc::RecordedCommand b;
        b.kind = "begin_render_pass";
        b.render_pass = rp.render_pass;
        b.framebuffer = framebuffers[static_cast<std::size_t>(idx)];
        b.render_area_w = kExtent;
        b.render_area_h = kExtent;
        b.r = 0.05;
        b.g = 0.05;
        b.b = 0.08;
        b.a = 1.0;
        vkrpc::RecordedCommand bind;
        bind.kind = "bind_pipeline";
        bind.pipeline = gp.pipeline;
        vkrpc::RecordedCommand vp;
        vp.kind = "set_viewport";
        vp.vp_w = static_cast<double>(kExtent);
        vp.vp_h = static_cast<double>(kExtent);
        vp.vp_max_depth = 1.0;
        vkrpc::RecordedCommand scc;
        scc.kind = "set_scissor";
        scc.sc_w = kExtent;
        scc.sc_h = kExtent;
        vkrpc::RecordedCommand draw;
        draw.kind = "draw";
        draw.vertex_count = 3;
        draw.instance_count = 1;
        draw.first_vertex = 0;
        draw.first_instance = 0;
        vkrpc::RecordedCommand end;
        end.kind = "end_render_pass";
        rec.commands = {b, bind, vp, scc, draw, end};
    }
    VKR_CHECK(backend.record_command_buffer(rec).ok);

    vkrpc::QueueSubmitRequest sub;
    sub.queue = q.queue;
    vkrpc::SubmitWait wait;
    wait.semaphore = sem_acq.semaphore;
    wait.stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    sub.waits = {wait};
    sub.command_buffers = {cmd};
    sub.signal_semaphores = {sem_done.semaphore};
    sub.fence = fence.fence;
    const vkrpc::QueueSubmitResponse s = backend.queue_submit(sub);
    VKR_CHECK(s.ok);
    VKR_CHECK_EQ(s.result, vkrpc::kVkSuccess);

    vkrpc::WaitForFencesRequest wf;
    wf.fences = {fence.fence};
    wf.wait_all = true;
    wf.timeout = UINT64_MAX;
    const vkrpc::WaitForFencesResponse w = backend.wait_for_fences(wf);
    VKR_CHECK(w.ok);
    VKR_CHECK_EQ(w.result, vkrpc::kVkSuccess);

    vkrpc::QueuePresentRequest pr;
    pr.queue = q.queue;
    pr.wait_semaphores = {sem_done.semaphore};
    vkrpc::PresentEntry pe;
    pe.swapchain = sc.swapchain;
    pe.image_index = idx;
    pr.presents = {pe};
    const vkrpc::QueuePresentResponse pres = backend.queue_present(pr);
    VKR_CHECK(pres.ok);
    VKR_CHECK(pres.result == vkrpc::kVkSuccess || pres.result == vkrpc::kVkSuboptimalKhr);

    std::fprintf(stderr,
                 "integration_real_draw: drew + presented a bufferless triangle (%zu images)\n",
                 imgs.images.size());

    // Teardown, child-before-parent. Order matters for validation-cleanliness: the present's wait
    // semaphore (sem_done) stays in use by the queue until the present completes, and the fence
    // only covers the SUBMIT, not the present -- so the sync objects can only be destroyed AFTER
    // destroy_swapchain, which idles the device (flushing the in-flight present). The swapchain in
    // turn blocks on its live image views, so: pipeline/layout/framebuffers/render pass/shaders ->
    // image views -> swapchain (idles) -> sync objects -> pool -> surface -> device -> instance.
    vkrpc::HandleRequest h;
    h.handle = gp.pipeline;
    VKR_CHECK(backend.destroy_pipeline(h).ok);
    // Destroying a baked draw object (the pipeline) invalidated the recorded command
    // buffer, so a resubmit of it now fails instead of replaying a freed host handle.
    {
        vkrpc::QueueSubmitRequest qs2;
        qs2.queue = q.queue;
        qs2.command_buffers = {cmd};
        VKR_CHECK(!backend.queue_submit(qs2).ok);
    }
    h.handle = pl.pipeline_layout;
    VKR_CHECK(backend.destroy_pipeline_layout(h).ok);
    for (const std::uint64_t fb : framebuffers) {
        h.handle = fb;
        VKR_CHECK(backend.destroy_framebuffer(h).ok);
    }
    h.handle = rp.render_pass;
    VKR_CHECK(backend.destroy_render_pass(h).ok);
    h.handle = vs.shader_module;
    VKR_CHECK(backend.destroy_shader_module(h).ok);
    h.handle = fs.shader_module;
    VKR_CHECK(backend.destroy_shader_module(h).ok);
    // The swapchain blocks on live image views: destroying it now must fail, then succeed after.
    h.handle = sc.swapchain;
    VKR_CHECK(!backend.destroy_swapchain(h).ok);
    for (const std::uint64_t view : views) {
        h.handle = view;
        VKR_CHECK(backend.destroy_image_view(h).ok);
    }
    h.handle = sc.swapchain;
    VKR_CHECK(backend.destroy_swapchain(h).ok); // idles the device -> the present has completed
    h.handle = fence.fence;
    VKR_CHECK(backend.destroy_fence(h).ok);
    h.handle = sem_acq.semaphore;
    VKR_CHECK(backend.destroy_semaphore(h).ok);
    h.handle = sem_done.semaphore;
    VKR_CHECK(backend.destroy_semaphore(h).ok);
    h.handle = pool.command_pool;
    VKR_CHECK(backend.destroy_command_pool(h).ok);
    h.handle = surf.surface;
    VKR_CHECK(backend.destroy_surface(h).ok);
    h.handle = cd.device;
    VKR_CHECK(backend.destroy_device(h).ok);
    h.handle = ci.instance;
    VKR_CHECK(backend.destroy_instance(h).ok);

    return vkr::test::finish("integration_real_draw");
}
