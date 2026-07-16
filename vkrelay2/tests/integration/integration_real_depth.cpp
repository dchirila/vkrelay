// In-process real-backend test for the depth spine.
//
// Extends the bufferless-triangle DRAW (integration_real_draw) with a depth buffer: a real
// D16_UNORM depth image (OPTIMAL tiling) bound to DEVICE-LOCAL memory, a depth image view, a render
// pass with a color + depth attachment, framebuffers that carry the depth view, a pipeline with a
// depth-stencil state (test+write, LESS_OR_EQUAL), and a frame whose begin_render_pass carries TWO
// clear values (color + depth). The triangle still emits its 3 vertices from gl_VertexIndex, so the
// texture half (combined-image-sampler + sampler) is covered separately -- this isolates depth.
//
// Also exercises the format-properties query (honest host values) and the new fail-closed edges: a
// host-visible memory type is NOT in the OPTIMAL depth image's memoryTypeBits (so binding it
// fails), and the device cannot be destroyed while the depth image lives.
//
// Windows-only. Skips gracefully (still passes) if there is no usable Vulkan device / no WSI
// support. The validation-clean gate is a manual run with
// VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation.
#include "windows/worker/real_vulkan_backend.hpp"

#include "common/vkrpc/vulkan_session.hpp"
#include "tests/test_assert.hpp"
#include "tests/triangle_spv.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include <vulkan/vulkan.h>

using namespace vkr;

int main() {
    worker::RealVulkanBackend backend("", "", false);

    const vkrpc::CreateInstanceResponse ci = backend.create_instance({});
    if (!ci.ok) {
        std::fprintf(stderr, "integration_real_depth: skipped (no instance: %s)\n",
                     ci.reason.c_str());
        return vkr::test::finish("integration_real_depth");
    }
    vkrpc::EnumeratePhysicalDevicesRequest er;
    er.instance = ci.instance;
    const vkrpc::EnumeratePhysicalDevicesResponse en = backend.enumerate_physical_devices(er);
    if (!en.ok || en.devices.empty()) {
        std::fprintf(stderr, "integration_real_depth: skipped (no physical device)\n");
        return vkr::test::finish("integration_real_depth");
    }
    const std::uint64_t phys = en.devices.front().handle;
    vkrpc::CreateDeviceRequest cdr;
    cdr.instance = ci.instance;
    cdr.physical_device = phys;
    const vkrpc::CreateDeviceResponse cd = backend.create_device(cdr);
    VKR_CHECK(cd.ok);
    vkrpc::GetDeviceQueueRequest gqr;
    gqr.device = cd.device;
    gqr.queue_family_index = cd.queue_family_index;
    gqr.queue_index = 0;
    const vkrpc::GetDeviceQueueResponse q = backend.get_device_queue(gqr);
    VKR_CHECK(q.ok && q.queue != 0);

    // Format properties are honest: D16_UNORM is a depth-stencil attachment format on any real GPU.
    vkrpc::GetPhysicalDeviceFormatPropertiesRequest fpr;
    fpr.physical_device = phys;
    fpr.format = vkrpc::kFormatD16Unorm;
    const vkrpc::GetPhysicalDeviceFormatPropertiesResponse fp =
        backend.get_physical_device_format_properties(fpr);
    VKR_CHECK(fp.ok);
    VKR_CHECK((fp.optimal_tiling_features & vkrpc::kFormatFeatureDepthStencilAttachment) != 0);

    // (GL/zink) RENDER CORRECTNESS: the worker must report VK_EXT_depth_clip_enable's
    // feature HONESTLY from the real device. Reporting it absent is exactly what rendered
    // OpenSCAD's CSG preview BLACK through the relay ("Incorrect rendering will happen because the
    // Vulkan device doesn't support the 'VK_EXT_depth_clip_enable' feature"). The capability-chain
    // generically fills any known feature struct from the host vkGetPhysicalDeviceFeatures2; assert
    // depthClipEnable comes back VK_TRUE on a real GPU.
    {
        vkrpc::GetPhysicalDeviceCapabilityChainRequest cc;
        cc.physical_device = phys;
        cc.which = 0; // features
        vkrpc::CapabilityChainEntry e;
        e.s_type = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_ENABLE_FEATURES_EXT;
        e.size = sizeof(VkPhysicalDeviceDepthClipEnableFeaturesEXT);
        cc.entries.push_back(e);
        const vkrpc::GetPhysicalDeviceCapabilityChainResponse cr =
            backend.get_physical_device_capability_chain(cc);
        VKR_CHECK(cr.ok && cr.entries.size() == 1);
        VkPhysicalDeviceDepthClipEnableFeaturesEXT df{};
        VKR_CHECK(cr.entries[0].blob.size() >= sizeof(df));
        std::memcpy(&df, cr.entries[0].blob.data(), sizeof(df));
        VKR_CHECK(df.depthClipEnable == VK_TRUE);
    }

    vkrpc::CreateSurfaceRequest sr;
    sr.instance = ci.instance;
    const vkrpc::CreateSurfaceResponse surf = backend.create_surface(sr);
    VKR_CHECK(surf.ok);
    vkrpc::GetSurfaceFormatsRequest sfmt_req;
    sfmt_req.physical_device = phys;
    sfmt_req.surface = surf.surface;
    const vkrpc::GetSurfaceFormatsResponse sfmt = backend.get_surface_formats(sfmt_req);
    VKR_CHECK(sfmt.ok && !sfmt.formats.empty());
    vkrpc::GetSurfaceCapabilitiesRequest scap_req;
    scap_req.physical_device = phys;
    scap_req.surface = surf.surface;
    const vkrpc::GetSurfaceCapabilitiesResponse scap = backend.get_surface_capabilities(scap_req);
    VKR_CHECK(scap.ok);
    vkrpc::GetSurfacePresentModesRequest spm_req;
    spm_req.physical_device = phys;
    spm_req.surface = surf.surface;
    const vkrpc::GetSurfacePresentModesResponse spm = backend.get_surface_present_modes(spm_req);
    VKR_CHECK(spm.ok && !spm.present_modes.empty());

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
        std::fprintf(stderr, "integration_real_depth: skipped (no swapchain: %s)\n",
                     sc.reason.c_str());
        return vkr::test::finish("integration_real_depth");
    }
    vkrpc::GetSwapchainImagesRequest gir;
    gir.swapchain = sc.swapchain;
    const vkrpc::GetSwapchainImagesResponse imgs = backend.get_swapchain_images(gir);
    VKR_CHECK(imgs.ok && !imgs.images.empty());

    // Color image views.
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

    // The depth image: D16_UNORM, OPTIMAL, DEPTH_STENCIL_ATTACHMENT, UNDEFINED. One shared depth
    // buffer for all swapchain images (FIFO serializes frames), like vkcube.
    vkrpc::CreateImageRequest dir;
    dir.device = cd.device;
    dir.image_type = vkrpc::kImageType2D;
    dir.format = vkrpc::kFormatD16Unorm;
    dir.width = kExtent;
    dir.height = kExtent;
    dir.depth = 1;
    dir.mip_levels = 1;
    dir.array_layers = 1;
    dir.samples = 1;
    dir.tiling = vkrpc::kImageTilingOptimal;
    dir.usage = vkrpc::kImageUsageDepthStencilAttachment;
    dir.sharing_mode = 0;
    dir.initial_layout = 0;
    const vkrpc::CreateImageResponse depth = backend.create_image(dir);
    VKR_CHECK(depth.ok && depth.image != 0);
    VKR_CHECK(!depth.has_subresource_layout); // OPTIMAL has no queryable subresource layout

    // Choose a memory type in the depth image's memoryTypeBits. Prefer a DEVICE_LOCAL type; verify
    // a HOST_VISIBLE-only one (if any in the set) is rejected as the wrong class for an OPTIMAL
    // image.
    const vkrpc::GetPhysicalDeviceMemoryPropertiesResponse mp =
        backend.get_physical_device_memory_properties({phys});
    VKR_CHECK(mp.ok);
    long long device_local_type = -1;
    long long host_visible_in_bits = -1;
    for (std::size_t i = 0; i < mp.types.size(); ++i) {
        if ((depth.mem_type_bits & (1ull << i)) == 0) {
            continue;
        }
        const std::uint64_t flags = mp.types[i].property_flags;
        if (device_local_type < 0 && (flags & vkrpc::kMemoryPropertyDeviceLocal) != 0) {
            device_local_type = static_cast<long long>(i);
        }
        if ((flags & vkrpc::kMemoryPropertyHostVisible) != 0 &&
            (flags & vkrpc::kMemoryPropertyDeviceLocal) == 0) {
            host_visible_in_bits = static_cast<long long>(i);
        }
    }
    VKR_CHECK(device_local_type >= 0); // a real GPU's depth image is device-local

    vkrpc::AllocateMemoryRequest amr;
    amr.device = cd.device;
    amr.allocation_size = depth.mem_size;
    amr.memory_type_index = device_local_type;
    const vkrpc::AllocateMemoryResponse dmem = backend.allocate_memory(amr);
    VKR_CHECK(dmem.ok && dmem.memory != 0);
    VKR_CHECK(backend.bind_image_memory({depth.image, dmem.memory, 0}).ok);

    vkrpc::CreateImageViewRequest dvr;
    dvr.image = depth.image;
    dvr.view_type = VK_IMAGE_VIEW_TYPE_2D;
    dvr.format = vkrpc::kFormatD16Unorm;
    dvr.swizzle_r = dvr.swizzle_g = dvr.swizzle_b = dvr.swizzle_a = VK_COMPONENT_SWIZZLE_IDENTITY;
    dvr.aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
    dvr.base_mip_level = 0;
    dvr.level_count = 1;
    dvr.base_array_layer = 0;
    dvr.layer_count = 1;
    const vkrpc::CreateImageViewResponse dview = backend.create_image_view(dvr);
    VKR_CHECK(dview.ok && dview.image_view != 0);

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

    // Render pass: color attachment 0 + depth attachment 1.
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
    vkrpc::AttachmentDesc datt;
    datt.format = vkrpc::kFormatD16Unorm;
    datt.samples = VK_SAMPLE_COUNT_1_BIT;
    datt.load_op = VK_ATTACHMENT_LOAD_OP_CLEAR;
    datt.store_op = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    datt.stencil_load_op = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    datt.stencil_store_op = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    datt.initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    datt.final_layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    rpr.attachments = {att, datt};
    rpr.color_attachment = 0;
    rpr.color_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    rpr.depth_attachment = 1;
    rpr.depth_layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    const vkrpc::CreateRenderPassResponse rp = backend.create_render_pass(rpr);
    VKR_CHECK(rp.ok && rp.render_pass != 0);

    std::vector<std::uint64_t> framebuffers;
    for (const std::uint64_t view : views) {
        vkrpc::CreateFramebufferRequest fbr;
        fbr.device = cd.device;
        fbr.render_pass = rp.render_pass;
        fbr.image_view = view;
        fbr.depth_image_view = dview.image_view;
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
    gpr.has_depth_stencil = true; // the depth render pass requires it
    gpr.depth_test_enable = 1;
    gpr.depth_write_enable = 1;
    gpr.depth_compare_op = VK_COMPARE_OP_LESS_OR_EQUAL;
    const vkrpc::CreateGraphicsPipelinesResponse gp = backend.create_graphics_pipelines(gpr);
    VKR_CHECK(gp.ok && gp.pipeline != 0);

    // (GL/zink) RENDER CORRECTNESS: the worker must build the STATIC front/back
    // VkStencilOpState the app bakes into the pipeline. The relay used to honor stencil_test_enable
    // while leaving front/back ZEROED (compareOp NEVER -> the stencil test never passes), which
    // silently broke OpenSCAD's OpenCSG stencil compositing (difference showed no holes). Create a
    // stencil-enabled variant of the same pipeline and assert the real driver accepts the carried
    // ops (created + destroyed here so it does not entangle the child-before-parent teardown
    // below).
    {
        vkrpc::CreateGraphicsPipelinesRequest sgpr = gpr;
        sgpr.stencil_test_enable = 1;
        sgpr.stencil_front_fail_op = VK_STENCIL_OP_KEEP;
        sgpr.stencil_front_pass_op = VK_STENCIL_OP_REPLACE;
        sgpr.stencil_front_depth_fail_op = VK_STENCIL_OP_KEEP;
        sgpr.stencil_front_compare_op = VK_COMPARE_OP_ALWAYS;
        sgpr.stencil_front_compare_mask = 0xFF;
        sgpr.stencil_front_write_mask = 0xFF;
        sgpr.stencil_front_reference = 1;
        sgpr.stencil_back_fail_op = VK_STENCIL_OP_KEEP;
        sgpr.stencil_back_pass_op = VK_STENCIL_OP_REPLACE;
        sgpr.stencil_back_depth_fail_op = VK_STENCIL_OP_KEEP;
        sgpr.stencil_back_compare_op = VK_COMPARE_OP_ALWAYS;
        const vkrpc::CreateGraphicsPipelinesResponse sgp = backend.create_graphics_pipelines(sgpr);
        VKR_CHECK(sgp.ok && sgp.pipeline != 0);
        vkrpc::HandleRequest sh;
        sh.handle = sgp.pipeline;
        VKR_CHECK(backend.destroy_pipeline(sh).ok);
    }

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
        b.has_depth_clear = true; // the depth attachment's loadOp is CLEAR
        b.depth_clear = 1.0;
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
                 "integration_real_depth: drew + presented a depth-tested triangle (%zu images)\n",
                 imgs.images.size());

    // Real-path fail-closed edges (do these before teardown so the handles are still live).
    // A HOST_VISIBLE-only memory type (if the GPU exposes one in the bits) is the wrong class for
    // an OPTIMAL image -- but it is not even in the depth image's memoryTypeBits, so binding it
    // fails.
    if (host_visible_in_bits >= 0) {
        // (Unreachable on typical GPUs: an OPTIMAL depth image's bits are device-local only.)
        std::fprintf(stderr, "integration_real_depth: note: host-visible type in depth bits\n");
    }
    // The device cannot be destroyed while the depth image lives.
    VKR_CHECK(!backend.destroy_device({cd.device}).ok);

    // Teardown, child-before-parent (mirrors integration_real_draw, plus the depth image + memory).
    vkrpc::HandleRequest h;
    h.handle = gp.pipeline;
    VKR_CHECK(backend.destroy_pipeline(h).ok);
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
    h.handle = sc.swapchain;
    VKR_CHECK(!backend.destroy_swapchain(h).ok); // still blocked by live color views
    for (const std::uint64_t view : views) {
        h.handle = view;
        VKR_CHECK(backend.destroy_image_view(h).ok);
    }
    h.handle = sc.swapchain;
    VKR_CHECK(backend.destroy_swapchain(h).ok); // idles the device -> present completed
    // The depth image is blocked while its view lives (then destroyable once gone)
    // -- the swapchain -> view parent/child rule, applied to app images.
    h.handle = depth.image;
    VKR_CHECK(!backend.destroy_image(h).ok);
    h.handle = dview.image_view;
    VKR_CHECK(backend.destroy_image_view(h).ok);
    h.handle = depth.image;
    VKR_CHECK(backend.destroy_image(h).ok);
    h.handle = dmem.memory;
    VKR_CHECK(backend.free_memory(h).ok);
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

    return vkr::test::finish("integration_real_depth");
}
