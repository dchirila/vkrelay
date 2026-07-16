// In-process real-backend test for the descriptor / per-frame UBO path.
//
// Constructs RealVulkanBackend directly and exercises the whole descriptor surface against the
// host driver on the real GPU: a UNIFORM buffer (an mvp mat4) bound + uploaded via
// write_memory_ranges; a descriptor set layout (binding 0 = UNIFORM_BUFFER, VERTEX stage), pool,
// set, and vkUpdateDescriptorSets pointing the set at the UBO; a pipeline layout carrying the set
// layout; a pipeline whose vertex shader reads the transform from the UBO; then a real frame:
// acquire -> record(begin / bind pipeline / viewport / scissor / bind_descriptor_sets /
// bind_vertex_buffers / draw(3) / end) -> submit -> wait -> present. The UBO is an identity matrix,
// so the triangle renders iff the UBO bytes reached the GPU.
//
// It also exercises the real-path descriptor negatives: a write to an unbound buffer is rejected; a
// draw whose pipeline layout needs a set but binds none is refused; and a rejected update POISONS
// the targeted set so a previously-valid set's next draw is refused (the
// void-vkUpdateDescriptorSets fail-closed substitute). Byte-upload correctness is asserted at the
// mock/unit layer; this is the validation-clean real draw+present proof. Windows-only (WIN32 +
// Vulkan SDK); skips gracefully if there is no usable device. Validation-clean gate: a manual run
// with VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation (grep stderr for Validation Error / VUID).
#include "windows/worker/real_vulkan_backend.hpp"

#include "common/vkrpc/vulkan_session.hpp"
#include "tests/test_assert.hpp"
#include "tests/ubo_spv.h"

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
        std::fprintf(stderr, "integration_real_ubo: skipped (no instance: %s)\n",
                     ci.reason.c_str());
        return vkr::test::finish("integration_real_ubo");
    }
    vkrpc::EnumeratePhysicalDevicesRequest er;
    er.instance = ci.instance;
    const vkrpc::EnumeratePhysicalDevicesResponse en = backend.enumerate_physical_devices(er);
    if (!en.ok || en.devices.empty()) {
        std::fprintf(stderr, "integration_real_ubo: skipped (no physical device)\n");
        return vkr::test::finish("integration_real_ubo");
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

    // A HOST_VISIBLE | HOST_COHERENT type for both the VBO and the UBO uploads.
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

    // A small helper: create a buffer of `usage`, allocate coherent memory for it, bind, upload.
    auto make_bound_buffer = [&](std::uint64_t usage, const void* data, std::uint64_t bytes,
                                 std::uint64_t& out_buffer, std::uint64_t& out_memory) {
        vkrpc::CreateBufferRequest cbr;
        cbr.device = cd.device;
        cbr.size = bytes;
        cbr.usage = usage;
        cbr.sharing_mode = VK_SHARING_MODE_EXCLUSIVE;
        const vkrpc::CreateBufferResponse buf = backend.create_buffer(cbr);
        VKR_CHECK(buf.ok && buf.buffer != 0 && buf.mem_size >= bytes);
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
        vkrpc::WriteMemoryRangesRequest wmr;
        vkrpc::MemoryUpload up;
        up.memory = mem.memory;
        up.ranges.push_back(vkrpc::MemoryUploadRange{0, bytes});
        wmr.uploads.push_back(up);
        wmr.payload.assign(reinterpret_cast<const char*>(data), static_cast<std::size_t>(bytes));
        VKR_CHECK(backend.write_memory_ranges(wmr).ok);
        out_buffer = buf.buffer;
        out_memory = mem.memory;
    };

    // VBO: three vertices, clip-space position (vec2) + color (vec3), stride 20, color at 8.
    const float verts[] = {
        0.0f,  -0.5f, 1.0f, 0.0f, 0.0f, // red top
        0.5f,  0.5f,  0.0f, 1.0f, 0.0f, // green bottom-right
        -0.5f, 0.5f,  0.0f, 0.0f, 1.0f, // blue bottom-left
    };
    const std::uint32_t kStride = 20;
    std::uint64_t vbo = 0, vbo_mem = 0;
    make_bound_buffer(vkrpc::kBufferUsageVertexBuffer, verts, sizeof(verts), vbo, vbo_mem);

    // UBO: an identity mat4 (the per-frame transform). Identity -> the triangle renders unchanged,
    // so a visible/validation-clean draw proves the UBO bytes reached the GPU.
    const float ident[16] = {
        1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f,
    };
    std::uint64_t ubo = 0, ubo_mem = 0;
    make_bound_buffer(vkrpc::kBufferUsageUniformBuffer, ident, sizeof(ident), ubo, ubo_mem);

    // Descriptor set layout (binding 0 = UNIFORM_BUFFER, VERTEX) + pool + set.
    vkrpc::CreateDescriptorSetLayoutRequest dslr;
    dslr.device = cd.device;
    vkrpc::DescriptorSetLayoutBindingDesc bind0;
    bind0.binding = 0;
    bind0.descriptor_type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bind0.descriptor_count = 1;
    bind0.stage_flags = VK_SHADER_STAGE_VERTEX_BIT;
    dslr.bindings.push_back(bind0);
    const vkrpc::CreateDescriptorSetLayoutResponse dsl = backend.create_descriptor_set_layout(dslr);
    VKR_CHECK(dsl.ok && dsl.set_layout != 0);

    // Real-path parity: a pipeline layout with more than the rpc cap of set layouts
    // is rejected before the driver call (a host maxBoundDescriptorSets above our cap must not let
    // a direct worker request escape the advertised subset). Create kMax+1 simple set layouts,
    // request a pipeline layout over all of them -> rejected; clean them up.
    {
        std::vector<std::uint64_t> too_many;
        for (int i = 0; i <= vkrpc::kMaxPipelineLayoutSetLayouts; ++i) {
            const vkrpc::CreateDescriptorSetLayoutResponse extra =
                backend.create_descriptor_set_layout(dslr);
            VKR_CHECK(extra.ok);
            too_many.push_back(extra.set_layout);
        }
        vkrpc::CreatePipelineLayoutRequest over;
        over.device = cd.device;
        over.set_layout_count = static_cast<int>(too_many.size());
        over.push_constant_range_count = 0;
        over.set_layouts = too_many;
        VKR_CHECK(!backend.create_pipeline_layout(over).ok); // over the kMax set-layout cap
        for (const std::uint64_t sl : too_many) {
            vkrpc::HandleRequest h;
            h.handle = sl;
            VKR_CHECK(backend.destroy_descriptor_set_layout(h).ok);
        }
    }

    vkrpc::CreateDescriptorPoolRequest dpr;
    dpr.device = cd.device;
    dpr.max_sets = 2;
    dpr.pool_sizes.push_back(vkrpc::DescriptorPoolSizeDesc{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2});
    const vkrpc::CreateDescriptorPoolResponse dpool = backend.create_descriptor_pool(dpr);
    VKR_CHECK(dpool.ok && dpool.pool != 0);

    auto alloc_set = [&]() {
        vkrpc::AllocateDescriptorSetsRequest adsr;
        adsr.device = cd.device;
        adsr.pool = dpool.pool;
        adsr.set_layouts = {dsl.set_layout};
        const vkrpc::AllocateDescriptorSetsResponse r = backend.allocate_descriptor_sets(adsr);
        VKR_CHECK(r.ok && r.descriptor_sets.size() == 1);
        return r.descriptor_sets.front();
    };
    const std::uint64_t set = alloc_set();

    auto write_set = [&](std::uint64_t dst, std::uint64_t buffer, std::uint64_t range) {
        vkrpc::UpdateDescriptorSetsRequest u;
        u.device = cd.device;
        vkrpc::WriteDescriptorSetDesc w;
        w.dst_set = dst;
        w.dst_binding = 0;
        w.dst_array_element = 0;
        w.descriptor_type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        w.descriptor_count = 1;
        w.buffer_infos.push_back(vkrpc::DescriptorBufferInfoDesc{buffer, 0, range});
        u.writes.push_back(w);
        return backend.update_descriptor_sets(u);
    };
    VKR_CHECK(write_set(set, ubo, VK_WHOLE_SIZE).ok);
    // Real-path negative: a write to an unbound UNIFORM buffer is rejected (and poisons its
    // target).
    {
        vkrpc::CreateBufferRequest cbr;
        cbr.device = cd.device;
        cbr.size = sizeof(ident);
        cbr.usage = vkrpc::kBufferUsageUniformBuffer;
        cbr.sharing_mode = VK_SHARING_MODE_EXCLUSIVE;
        const std::uint64_t unbound = backend.create_buffer(cbr).buffer;
        VKR_CHECK(unbound != 0);
        const std::uint64_t set_neg = alloc_set();
        VKR_CHECK(!write_set(set_neg, unbound, VK_WHOLE_SIZE).ok);
        vkrpc::HandleRequest hub;
        hub.handle = unbound;
        VKR_CHECK(backend.destroy_buffer(hub).ok);
        // set_neg dies with the pool at teardown.
    }

    // Draw graph.
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
        std::fprintf(stderr, "integration_real_ubo: skipped (no swapchain: %s)\n",
                     sc.reason.c_str());
        return vkr::test::finish("integration_real_ubo");
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
        backend.create_shader_module(make_shader(ubo_spv::kVert, ubo_spv::kVertBytes));
    const vkrpc::CreateShaderModuleResponse fs =
        backend.create_shader_module(make_shader(ubo_spv::kFrag, ubo_spv::kFragBytes));
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

    // Pipeline layout WITH the descriptor set layout.
    vkrpc::CreatePipelineLayoutRequest plr;
    plr.device = cd.device;
    plr.set_layout_count = 1;
    plr.push_constant_range_count = 0;
    plr.set_layouts = {dsl.set_layout};
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
    gpr.cull_mode = VK_CULL_MODE_NONE;
    gpr.front_face = VK_FRONT_FACE_CLOCKWISE;
    gpr.dynamic_states = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    gpr.layout = pl.pipeline_layout;
    gpr.render_pass = rp.render_pass;
    gpr.subpass = 0;
    const vkrpc::CreateGraphicsPipelinesResponse gp = backend.create_graphics_pipelines(gpr);
    VKR_CHECK(gp.ok && gp.pipeline != 0);

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

    // The recorded UBO draw: bind the descriptor set before the draw.
    auto build_recording = [&](int idx) {
        vkrpc::RecordCommandBufferRequest rec;
        rec.command_buffer = cmd;
        rec.one_time_submit = true;
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
        vkrpc::RecordedCommand bds;
        bds.kind = "bind_descriptor_sets";
        bds.desc_layout = pl.pipeline_layout;
        bds.first_set = 0;
        bds.descriptor_sets = {set};
        vkrpc::RecordedCommand bvb;
        bvb.kind = "bind_vertex_buffers";
        bvb.first_binding = 0;
        bvb.vertex_buffers = {vbo};
        bvb.vertex_buffer_offsets = {0};
        vkrpc::RecordedCommand draw;
        draw.kind = "draw";
        draw.vertex_count = 3;
        draw.instance_count = 1;
        draw.first_vertex = 0;
        draw.first_instance = 0;
        vkrpc::RecordedCommand end;
        end.kind = "end_render_pass";
        rec.commands = {b, bind, vp, scc, bds, bvb, draw, end};
        return rec;
    };

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

    // (GL/zink) faithful contract: a draw whose pipeline layout declares a set that is not
    // bound is FORWARDED, not pre-rejected at record. zink legitimately leaves declared-but-unused
    // sets unbound, and the worker cannot know from here which sets the SPIR-V actually reads --
    // the host driver (+ the validation layer) is the authority on a used-but-unbound set. The
    // relay's descriptor draw guard is specifically the poison / freed-referent check on BOUND
    // sets, which the poison sub-test below still exercises.
    {
        vkrpc::RecordCommandBufferRequest rec = build_recording(idx);
        rec.commands.erase(rec.commands.begin() + 4); // drop the bind_descriptor_sets command
        VKR_CHECK(backend.record_command_buffer(rec).ok);
    }

    VKR_CHECK(backend.record_command_buffer(build_recording(idx)).ok);

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

    // Poison-on-failed-update (the void-vkUpdateDescriptorSets fail-closed substitute): after the
    // good frame, a rejected update to `set` (zero range) poisons it, so re-recording the same draw
    // is refused; a valid re-update restores it. Wait idle first so re-recording the buffer is
    // legal.
    vkrpc::WaitForFencesRequest wf2 = wf;
    backend.wait_for_fences(wf2);          // idle so re-recording the command buffer is legal
    VKR_CHECK(!write_set(set, ubo, 0).ok); // zero range -> reject + poison
    VKR_CHECK(!backend.record_command_buffer(build_recording(idx)).ok); // poisoned -> draw refused
    VKR_CHECK(write_set(set, ubo, VK_WHOLE_SIZE).ok);                   // re-validate
    VKR_CHECK(backend.record_command_buffer(build_recording(idx)).ok);  // draw-ready again

    std::fprintf(stderr,
                 "integration_real_ubo: drew + presented a UBO-transformed triangle (%zu "
                 "images)\n",
                 imgs.images.size());

    // Teardown, child-before-parent. The pipeline (referenced the descriptor set + layout in the
    // recording) goes before the pipeline layout / descriptor objects; the descriptor pool frees
    // its sets; the set layout goes after the pipeline layout that referenced it.
    vkrpc::HandleRequest h;
    h.handle = gp.pipeline;
    VKR_CHECK(backend.destroy_pipeline(h).ok);
    h.handle = pl.pipeline_layout;
    VKR_CHECK(backend.destroy_pipeline_layout(h).ok);
    h.handle = dsl.set_layout;
    VKR_CHECK(!backend.destroy_descriptor_set_layout(h).ok); // blocked: live sets in the pool
    h.handle = dpool.pool;
    VKR_CHECK(backend.destroy_descriptor_pool(h).ok); // frees the sets
    h.handle = dsl.set_layout;
    VKR_CHECK(backend.destroy_descriptor_set_layout(h).ok); // now unreferenced + no live sets
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
    h.handle = vbo;
    VKR_CHECK(backend.destroy_buffer(h).ok);
    h.handle = ubo;
    VKR_CHECK(backend.destroy_buffer(h).ok);
    h.handle = vbo_mem;
    VKR_CHECK(backend.free_memory(h).ok);
    h.handle = ubo_mem;
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

    return vkr::test::finish("integration_real_ubo");
}
