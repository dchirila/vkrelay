// In-process real-backend test for the VBO draw spine -- a mapped vertex buffer
// drives the triangle.
//
// Constructs RealVulkanBackend directly and exercises the whole memory path against the host
// driver on the real GPU: query memory properties, allocate a HOST_VISIBLE|HOST_COHERENT
// allocation, create + bind a VERTEX buffer, upload three vertices (pos vec2 + color vec3) via
// write_memory_ranges (the bytes the ICD's shadow/dirty path would ship), build a pipeline whose
// vertex input matches the buffer (one binding stride 20, attributes at 0 / 8), then a real frame:
// acquire -> record(begin/bind/viewport/scissor/bindVertexBuffers/draw(3)/end) -> submit -> wait ->
// present.
//
// This test talks to the backend directly, so there is no ICD MappedMemoryTracker here -- it plays
// the ICD's role by calling write_memory_ranges itself. Byte-upload correctness is asserted at the
// mock/unit layer (unit_vkrpc / unit_icd_memory); this is the validation-clean real draw+present
// proof (no app-facing mapped readback gate). Windows-only (WIN32 + Vulkan
// SDK); skips gracefully if there is no usable device. Validation-clean gate: a manual run with
// VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation (grep stderr for Validation Error / VUID).
#include "windows/worker/real_vulkan_backend.hpp"

#include "common/vkrpc/vulkan_session.hpp"
#include "tests/test_assert.hpp"
#include "tests/vbo_spv.h"

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
        std::fprintf(stderr, "integration_real_vbo: skipped (no instance: %s)\n",
                     ci.reason.c_str());
        return vkr::test::finish("integration_real_vbo");
    }
    vkrpc::EnumeratePhysicalDevicesRequest er;
    er.instance = ci.instance;
    const vkrpc::EnumeratePhysicalDevicesResponse en = backend.enumerate_physical_devices(er);
    if (!en.ok || en.devices.empty()) {
        std::fprintf(stderr, "integration_real_vbo: skipped (no physical device)\n");
        return vkr::test::finish("integration_real_vbo");
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

    // Memory properties: find a HOST_VISIBLE | HOST_COHERENT type (the VBO upload target).
    vkrpc::GetPhysicalDeviceMemoryPropertiesRequest mpr;
    mpr.physical_device = phys;
    const vkrpc::GetPhysicalDeviceMemoryPropertiesResponse mp =
        backend.get_physical_device_memory_properties(mpr);
    VKR_CHECK(mp.ok && !mp.types.empty());
    int coherent_type = -1;
    for (std::size_t i = 0; i < mp.types.size(); ++i) {
        const std::uint64_t want =
            vkrpc::kMemoryPropertyHostVisible | vkrpc::kMemoryPropertyHostCoherent;
        if ((mp.types[i].property_flags & want) == want) {
            coherent_type = static_cast<int>(i);
            break;
        }
    }
    VKR_CHECK(coherent_type >= 0);

    // Three vertices: clip-space position (vec2) + color (vec3) interleaved, stride 20, color at 8.
    const float verts[] = {
        0.0f,  -0.5f, 1.0f, 0.0f, 0.0f, // red top
        0.5f,  0.5f,  0.0f, 1.0f, 0.0f, // green bottom-right
        -0.5f, 0.5f,  0.0f, 0.0f, 1.0f, // blue bottom-left
    };
    const std::uint32_t kStride = 20;
    const std::uint64_t kVboBytes = sizeof(verts); // 60

    // Buffer (VERTEX) + its requirements; allocate coherent memory sized to the requirements; bind.
    vkrpc::CreateBufferRequest cbr;
    cbr.device = cd.device;
    cbr.size = kVboBytes;
    cbr.usage = vkrpc::kBufferUsageVertexBuffer;
    cbr.sharing_mode = VK_SHARING_MODE_EXCLUSIVE;
    const vkrpc::CreateBufferResponse buf = backend.create_buffer(cbr);
    VKR_CHECK(buf.ok && buf.buffer != 0 && buf.mem_size >= kVboBytes);
    vkrpc::AllocateMemoryRequest amr;
    amr.device = cd.device;
    amr.allocation_size = buf.mem_size;
    amr.memory_type_index = coherent_type;
    const vkrpc::AllocateMemoryResponse mem = backend.allocate_memory(amr);
    VKR_CHECK(mem.ok && mem.memory != 0);
    vkrpc::BindBufferMemoryRequest bbr;
    bbr.buffer = buf.buffer;
    bbr.memory = mem.memory;
    bbr.memory_offset = 0;
    VKR_CHECK(backend.bind_buffer_memory(bbr).ok);
    // Real bind pre-validation: re-binding the same buffer, an out-of-range
    // offset, and (when alignment > 1) a misaligned offset must be rejected before the driver call.
    VKR_CHECK(!backend.bind_buffer_memory(bbr).ok); // already bound
    {
        const vkrpc::CreateBufferResponse buf2 = backend.create_buffer(cbr);
        VKR_CHECK(buf2.ok);
        vkrpc::BindBufferMemoryRequest oob;
        oob.buffer = buf2.buffer;
        oob.memory = mem.memory;
        oob.memory_offset = buf.mem_size + buf.mem_alignment; // past the allocation
        VKR_CHECK(!backend.bind_buffer_memory(oob).ok);
        if (buf.mem_alignment > 1) {
            vkrpc::BindBufferMemoryRequest mis;
            mis.buffer = buf2.buffer;
            mis.memory = mem.memory;
            mis.memory_offset = 1; // not a multiple of the required alignment
            VKR_CHECK(!backend.bind_buffer_memory(mis).ok);
        }
        vkrpc::HandleRequest hb2;
        hb2.handle = buf2.buffer;
        VKR_CHECK(backend.destroy_buffer(hb2).ok);
    }

    // Upload the vertices (what the ICD's coherent-flush-at-submit sweep would ship).
    vkrpc::WriteMemoryRangesRequest wmr;
    {
        vkrpc::MemoryUpload up;
        up.memory = mem.memory;
        up.ranges.push_back(vkrpc::MemoryUploadRange{0, kVboBytes});
        wmr.uploads.push_back(up);
        wmr.payload.assign(reinterpret_cast<const char*>(verts),
                           static_cast<std::size_t>(kVboBytes));
    }
    VKR_CHECK(backend.write_memory_ranges(wmr).ok);

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
        std::fprintf(stderr, "integration_real_vbo: skipped (no swapchain: %s)\n",
                     sc.reason.c_str());
        return vkr::test::finish("integration_real_vbo");
    }
    vkrpc::GetSwapchainImagesRequest gir;
    gir.swapchain = sc.swapchain;
    const vkrpc::GetSwapchainImagesResponse imgs = backend.get_swapchain_images(gir);
    VKR_CHECK(imgs.ok && !imgs.images.empty());

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

    auto make_shader = [&](const std::uint32_t* spv, std::size_t bytes) {
        vkrpc::CreateShaderModuleRequest r;
        r.device = cd.device;
        r.code.assign(reinterpret_cast<const char*>(spv), bytes);
        r.code_size = bytes;
        return r;
    };
    const vkrpc::CreateShaderModuleResponse vs =
        backend.create_shader_module(make_shader(vbo_spv::kVert, vbo_spv::kVertBytes));
    const vkrpc::CreateShaderModuleResponse fs =
        backend.create_shader_module(make_shader(vbo_spv::kFrag, vbo_spv::kFragBytes));
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
    // Vertex input matching the uploaded buffer: one VERTEX-rate binding (stride 20), pos at 0
    // (R32G32_SFLOAT), color at 8 (R32G32B32_SFLOAT).
    vkrpc::VertexBindingDesc vb;
    vb.binding = 0;
    vb.stride = static_cast<long long>(kStride);
    vb.input_rate = VK_VERTEX_INPUT_RATE_VERTEX;
    gpr.vertex_bindings = {vb};
    vkrpc::VertexAttributeDesc a_pos;
    a_pos.location = 0;
    a_pos.binding = 0;
    a_pos.format = VK_FORMAT_R32G32_SFLOAT;
    a_pos.offset = 0;
    vkrpc::VertexAttributeDesc a_col;
    a_col.location = 1;
    a_col.binding = 0;
    a_col.format = VK_FORMAT_R32G32B32_SFLOAT;
    a_col.offset = 8;
    gpr.vertex_attributes = {a_pos, a_col};
    gpr.vertex_binding_count = 1;
    gpr.vertex_attribute_count = 2;
    gpr.cull_mode = VK_CULL_MODE_NONE; // no culling -> winding-agnostic
    gpr.front_face = VK_FRONT_FACE_CLOCKWISE;
    gpr.dynamic_states = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    gpr.layout = pl.pipeline_layout;
    gpr.render_pass = rp.render_pass;
    gpr.subpass = 0;
    const vkrpc::CreateGraphicsPipelinesResponse gp = backend.create_graphics_pipelines(gpr);
    VKR_CHECK(gp.ok && gp.pipeline != 0);
    // Vertex-input range checks: an oversized stride / attribute offset is
    // rejected, not silently narrowed to uint32.
    {
        vkrpc::CreateGraphicsPipelinesRequest bad = gpr;
        bad.vertex_bindings[0].stride = (1LL << 32); // > UINT32_MAX
        VKR_CHECK(!backend.create_graphics_pipelines(bad).ok);
        bad = gpr;
        bad.vertex_attributes[0].offset = (1LL << 32);
        VKR_CHECK(!backend.create_graphics_pipelines(bad).ok);
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
        vkrpc::RecordedCommand bvb;
        bvb.kind = "bind_vertex_buffers";
        bvb.first_binding = 0;
        bvb.vertex_buffers = {buf.buffer};
        bvb.vertex_buffer_offsets = {0};
        vkrpc::RecordedCommand draw;
        draw.kind = "draw";
        draw.vertex_count = 3;
        draw.instance_count = 1;
        draw.first_vertex = 0;
        draw.first_instance = 0;
        vkrpc::RecordedCommand end;
        end.kind = "end_render_pass";
        rec.commands = {b, bind, vp, scc, bvb, draw, end};
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

    std::fprintf(stderr, "integration_real_vbo: drew + presented a VBO triangle (%zu images)\n",
                 imgs.images.size());

    // Teardown, child-before-parent (same ordering rationale as integration_real_draw); the buffer
    // + memory are freed after the pipeline (which referenced the buffer in the recording).
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
    VKR_CHECK(!backend.destroy_swapchain(h).ok); // blocked by live image views
    for (const std::uint64_t view : views) {
        h.handle = view;
        VKR_CHECK(backend.destroy_image_view(h).ok);
    }
    h.handle = sc.swapchain;
    VKR_CHECK(backend.destroy_swapchain(h).ok); // idles the device -> present complete
    // The device still has the live buffer -> destroy_device is blocked until the buffer is freed.
    h.handle = cd.device;
    VKR_CHECK(!backend.destroy_device(h).ok);
    h.handle = buf.buffer;
    VKR_CHECK(backend.destroy_buffer(h).ok);
    h.handle = mem.memory;
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

    return vkr::test::finish("integration_real_vbo");
}
