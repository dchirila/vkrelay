// In-process real-backend test for the texture + combined-image-sampler
// spine -- a bufferless TEXTURED triangle on the host GPU.
//
// Extends the bufferless triangle with the texture pieces: an R8G8B8A8_UNORM SAMPLED
// texture uploaded via the staging path (a TRANSFER_SRC host-visible buffer ->
// vkCmdCopyBufferToImage into a DEVICE_LOCAL OPTIMAL image, with the
// UNDEFINED->TRANSFER_DST->SHADER_READ_ONLY barriers), a NEAREST CLAMP_TO_EDGE sampler, and a
// 2-binding descriptor set (binding 0 UNIFORM_BUFFER/VERTEX, binding 1
// COMBINED_IMAGE_SAMPLER/FRAGMENT). The frame uploads the texture then draws + presents.
//
// Also exercises the real-path negatives: write_memory_ranges to DEVICE_LOCAL memory is rejected
// (it is not HOST_VISIBLE|HOST_COHERENT), an out-of-subset barrier transition is rejected at
// record, a combined-image-sampler write to a dead sampler poisons the set, the descriptor ->
// sampler destroy consult invalidates the recorded draw, and a live sampler blocks the device's
// destroy.
//
// Windows-only. Skips gracefully (still passes) if there is no usable Vulkan device / no WSI
// support. The validation-clean gate is a manual run with
// VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation.
#include "windows/worker/real_vulkan_backend.hpp"

#include "common/vkrpc/vulkan_session.hpp"
#include "tests/test_assert.hpp"
#include "tests/texture_spv.h"

#include <cstdint>
#include <cstdio>
#include <vector>

#include <vulkan/vulkan.h>

using namespace vkr;

namespace {
// Picks a memory type in `type_bits` matching a predicate over the property flags; -1 if none.
long long pick_type(const vkrpc::GetPhysicalDeviceMemoryPropertiesResponse& mp,
                    std::uint64_t type_bits, bool want_device_local) {
    for (std::size_t i = 0; i < mp.types.size(); ++i) {
        if ((type_bits & (1ull << i)) == 0) {
            continue;
        }
        const std::uint64_t f = mp.types[i].property_flags;
        const bool coherent = (f & vkrpc::kMemoryPropertyHostVisible) != 0 &&
                              (f & vkrpc::kMemoryPropertyHostCoherent) != 0;
        const bool device_local = (f & vkrpc::kMemoryPropertyDeviceLocal) != 0;
        if (want_device_local ? device_local : coherent) {
            return static_cast<long long>(i);
        }
    }
    return -1;
}
} // namespace

int main() {
    worker::RealVulkanBackend backend("", "", false);

    const vkrpc::CreateInstanceResponse ci = backend.create_instance({});
    if (!ci.ok) {
        std::fprintf(stderr, "integration_real_texture: skipped (no instance: %s)\n",
                     ci.reason.c_str());
        return vkr::test::finish("integration_real_texture");
    }
    vkrpc::EnumeratePhysicalDevicesRequest er;
    er.instance = ci.instance;
    const vkrpc::EnumeratePhysicalDevicesResponse en = backend.enumerate_physical_devices(er);
    if (!en.ok || en.devices.empty()) {
        std::fprintf(stderr, "integration_real_texture: skipped (no physical device)\n");
        return vkr::test::finish("integration_real_texture");
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

    // Format properties are honest: R8G8B8A8_UNORM is a sampled-image format on any real GPU.
    vkrpc::GetPhysicalDeviceFormatPropertiesRequest fpr;
    fpr.physical_device = phys;
    fpr.format = vkrpc::kFormatR8G8B8A8Unorm;
    const vkrpc::GetPhysicalDeviceFormatPropertiesResponse fp =
        backend.get_physical_device_format_properties(fpr);
    VKR_CHECK(fp.ok);
    VKR_CHECK((fp.optimal_tiling_features & vkrpc::kFormatFeatureSampledImage) != 0);

    const vkrpc::GetPhysicalDeviceMemoryPropertiesResponse mp =
        backend.get_physical_device_memory_properties({phys});
    VKR_CHECK(mp.ok);

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
        std::fprintf(stderr, "integration_real_texture: skipped (no swapchain: %s)\n",
                     sc.reason.c_str());
        return vkr::test::finish("integration_real_texture");
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

    // --- The texture: an R8G8B8A8_UNORM SAMPLED | TRANSFER_DST OPTIMAL image on DEVICE_LOCAL ---
    const int kTex = 16;
    vkrpc::CreateImageRequest tir;
    tir.device = cd.device;
    tir.image_type = vkrpc::kImageType2D;
    tir.format = vkrpc::kFormatR8G8B8A8Unorm;
    tir.width = kTex;
    tir.height = kTex;
    tir.depth = 1;
    tir.mip_levels = 1;
    tir.array_layers = 1;
    tir.samples = 1;
    tir.tiling = vkrpc::kImageTilingOptimal;
    tir.usage =
        vkrpc::kImageUsageSampled | static_cast<std::uint64_t>(vkrpc::kImageUsageTransferDst);
    tir.sharing_mode = 0;
    tir.initial_layout = 0;
    const vkrpc::CreateImageResponse tex = backend.create_image(tir);
    VKR_CHECK(tex.ok && tex.image != 0);
    const long long tex_type = pick_type(mp, tex.mem_type_bits, /*want_device_local=*/true);
    VKR_CHECK(tex_type >= 0);
    vkrpc::AllocateMemoryRequest tam;
    tam.device = cd.device;
    tam.allocation_size = tex.mem_size;
    tam.memory_type_index = tex_type;
    const vkrpc::AllocateMemoryResponse tmem = backend.allocate_memory(tam);
    VKR_CHECK(tmem.ok && tmem.memory != 0);
    VKR_CHECK(backend.bind_image_memory({tex.image, tmem.memory, 0}).ok);
    // Negative: write_memory_ranges to DEVICE_LOCAL memory is rejected (not HOST_VISIBLE|COHERENT).
    {
        vkrpc::WriteMemoryRangesRequest bad;
        vkrpc::MemoryUpload u;
        u.memory = tmem.memory;
        u.ranges.push_back({0, 4});
        bad.uploads.push_back(u);
        bad.payload = std::string(4, '\xff');
        VKR_CHECK(!backend.write_memory_ranges(bad).ok);
    }

    vkrpc::CreateImageViewRequest tvr;
    tvr.image = tex.image;
    tvr.view_type = VK_IMAGE_VIEW_TYPE_2D;
    tvr.format = vkrpc::kFormatR8G8B8A8Unorm;
    tvr.swizzle_r = tvr.swizzle_g = tvr.swizzle_b = tvr.swizzle_a = VK_COMPONENT_SWIZZLE_IDENTITY;
    tvr.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    tvr.base_mip_level = 0;
    tvr.level_count = 1;
    tvr.base_array_layer = 0;
    tvr.layer_count = 1;
    const vkrpc::CreateImageViewResponse tview = backend.create_image_view(tvr);
    VKR_CHECK(tview.ok && tview.image_view != 0);

    // --- The staging buffer: TRANSFER_SRC host-visible coherent, filled with a checkerboard ---
    const std::uint64_t kTexBytes = static_cast<std::uint64_t>(kTex) * kTex * 4;
    vkrpc::CreateBufferRequest sbq;
    sbq.device = cd.device;
    sbq.size = kTexBytes;
    sbq.usage = vkrpc::kBufferUsageTransferSrc;
    sbq.sharing_mode = 0;
    const vkrpc::CreateBufferResponse staging = backend.create_buffer(sbq);
    VKR_CHECK(staging.ok && staging.buffer != 0);
    const long long staging_type =
        pick_type(mp, staging.mem_type_bits, /*want_device_local=*/false);
    VKR_CHECK(staging_type >= 0);
    vkrpc::AllocateMemoryRequest sam;
    sam.device = cd.device;
    sam.allocation_size = staging.mem_size;
    sam.memory_type_index = staging_type;
    const vkrpc::AllocateMemoryResponse smem = backend.allocate_memory(sam);
    VKR_CHECK(smem.ok && smem.memory != 0);
    VKR_CHECK(backend.bind_buffer_memory({staging.buffer, smem.memory, 0}).ok);
    {
        vkrpc::WriteMemoryRangesRequest wr;
        vkrpc::MemoryUpload u;
        u.memory = smem.memory;
        u.ranges.push_back({0, kTexBytes});
        wr.uploads.push_back(u);
        std::string px(static_cast<std::size_t>(kTexBytes), '\0');
        for (int y = 0; y < kTex; ++y) {
            for (int x = 0; x < kTex; ++x) {
                const std::size_t o = (static_cast<std::size_t>(y) * kTex + x) * 4;
                const bool on = ((x ^ y) & 1) != 0;
                px[o + 0] = on ? '\xff' : '\x20';
                px[o + 1] = '\x40';
                px[o + 2] = on ? '\x20' : '\xff';
                px[o + 3] = '\xff';
            }
        }
        wr.payload = std::move(px);
        VKR_CHECK(backend.write_memory_ranges(wr).ok);
    }

    // --- The sampler: NEAREST, CLAMP_TO_EDGE, no anisotropy/compare ---
    vkrpc::CreateSamplerRequest smq;
    smq.device = cd.device;
    smq.mag_filter = vkrpc::kFilterNearest;
    smq.min_filter = vkrpc::kFilterNearest;
    smq.mipmap_mode = vkrpc::kSamplerMipmapModeNearest;
    smq.address_mode_u = vkrpc::kSamplerAddressModeClampToEdge;
    smq.address_mode_v = vkrpc::kSamplerAddressModeClampToEdge;
    smq.address_mode_w = vkrpc::kSamplerAddressModeClampToEdge;
    smq.anisotropy_enable = 0;
    smq.compare_enable = 0;
    const vkrpc::CreateSamplerResponse sampler = backend.create_sampler(smq);
    VKR_CHECK(sampler.ok && sampler.sampler != 0);

    // --- The UBO: a vec4 offset (binding 0), host-visible coherent ---
    vkrpc::CreateBufferRequest ubq;
    ubq.device = cd.device;
    ubq.size = 16;
    ubq.usage = vkrpc::kBufferUsageUniformBuffer;
    ubq.sharing_mode = 0;
    const vkrpc::CreateBufferResponse ubo = backend.create_buffer(ubq);
    VKR_CHECK(ubo.ok && ubo.buffer != 0);
    const long long ubo_type = pick_type(mp, ubo.mem_type_bits, /*want_device_local=*/false);
    VKR_CHECK(ubo_type >= 0);
    vkrpc::AllocateMemoryRequest uam;
    uam.device = cd.device;
    uam.allocation_size = ubo.mem_size;
    uam.memory_type_index = ubo_type;
    const vkrpc::AllocateMemoryResponse umem = backend.allocate_memory(uam);
    VKR_CHECK(umem.ok && umem.memory != 0);
    VKR_CHECK(backend.bind_buffer_memory({ubo.buffer, umem.memory, 0}).ok);
    {
        vkrpc::WriteMemoryRangesRequest wr;
        vkrpc::MemoryUpload u;
        u.memory = umem.memory;
        u.ranges.push_back({0, 16});
        wr.uploads.push_back(u);
        wr.payload = std::string(16, '\0'); // offset = {0,0,0,0}
        VKR_CHECK(backend.write_memory_ranges(wr).ok);
    }

    // --- The 2-binding descriptor set: 0 UNIFORM_BUFFER/VERTEX, 1 COMBINED_IMAGE_SAMPLER/FRAGMENT.
    vkrpc::CreateDescriptorSetLayoutRequest dslr;
    dslr.device = cd.device;
    {
        vkrpc::DescriptorSetLayoutBindingDesc b0;
        b0.binding = 0;
        b0.descriptor_type = vkrpc::kDescriptorTypeUniformBuffer;
        b0.descriptor_count = 1;
        b0.stage_flags = VK_SHADER_STAGE_VERTEX_BIT;
        vkrpc::DescriptorSetLayoutBindingDesc b1;
        b1.binding = 1;
        b1.descriptor_type = vkrpc::kDescriptorTypeCombinedImageSampler;
        b1.descriptor_count = 1;
        b1.stage_flags = VK_SHADER_STAGE_FRAGMENT_BIT;
        dslr.bindings = {b0, b1};
    }
    const vkrpc::CreateDescriptorSetLayoutResponse dsl = backend.create_descriptor_set_layout(dslr);
    VKR_CHECK(dsl.ok && dsl.set_layout != 0);
    vkrpc::CreateDescriptorPoolRequest dpr;
    dpr.device = cd.device;
    dpr.max_sets = 1;
    dpr.pool_sizes = {{vkrpc::kDescriptorTypeUniformBuffer, 1},
                      {vkrpc::kDescriptorTypeCombinedImageSampler, 1}};
    const vkrpc::CreateDescriptorPoolResponse dp = backend.create_descriptor_pool(dpr);
    VKR_CHECK(dp.ok && dp.pool != 0);
    vkrpc::AllocateDescriptorSetsRequest adr;
    adr.device = cd.device;
    adr.pool = dp.pool;
    adr.set_layouts = {dsl.set_layout};
    const vkrpc::AllocateDescriptorSetsResponse aset = backend.allocate_descriptor_sets(adr);
    VKR_CHECK(aset.ok && aset.descriptor_sets.size() == 1);
    const std::uint64_t set = aset.descriptor_sets[0];

    // Negative: a combined-image-sampler write to a DEAD sampler poisons the set.
    {
        vkrpc::UpdateDescriptorSetsRequest bad;
        bad.device = cd.device;
        vkrpc::WriteDescriptorSetDesc w;
        w.dst_set = set;
        w.dst_binding = 1;
        w.dst_array_element = 0;
        w.descriptor_type = vkrpc::kDescriptorTypeCombinedImageSampler;
        w.descriptor_count = 1;
        w.image_infos.push_back(
            {0xDEAD, tview.image_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
        bad.writes.push_back(w);
        VKR_CHECK(!backend.update_descriptor_sets(bad).ok);
    }
    // Valid: write both bindings.
    {
        vkrpc::UpdateDescriptorSetsRequest wr;
        wr.device = cd.device;
        vkrpc::WriteDescriptorSetDesc wu;
        wu.dst_set = set;
        wu.dst_binding = 0;
        wu.dst_array_element = 0;
        wu.descriptor_type = vkrpc::kDescriptorTypeUniformBuffer;
        wu.descriptor_count = 1;
        wu.buffer_infos.push_back({ubo.buffer, 0, vkrpc::kVkWholeSize});
        vkrpc::WriteDescriptorSetDesc wi;
        wi.dst_set = set;
        wi.dst_binding = 1;
        wi.dst_array_element = 0;
        wi.descriptor_type = vkrpc::kDescriptorTypeCombinedImageSampler;
        wi.descriptor_count = 1;
        wi.image_infos.push_back(
            {sampler.sampler, tview.image_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
        wr.writes = {wu, wi};
        VKR_CHECK(backend.update_descriptor_sets(wr).ok);
    }

    // --- Shaders / render pass (color only) / framebuffers / pipeline ---
    auto make_shader = [&](const std::uint32_t* spv, std::uint32_t bytes) {
        vkrpc::CreateShaderModuleRequest r;
        r.device = cd.device;
        r.code.assign(reinterpret_cast<const char*>(spv), bytes);
        r.code_size = bytes;
        return r;
    };
    const vkrpc::CreateShaderModuleResponse vs =
        backend.create_shader_module(make_shader(texture_spv::kVert, texture_spv::kVertBytes));
    const vkrpc::CreateShaderModuleResponse fs =
        backend.create_shader_module(make_shader(texture_spv::kFrag, texture_spv::kFragBytes));
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
    rpr.attachments = {att};
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

    // The texture-upload commands, shared by the draw + the negative-edge records.
    auto barrier = [&](int old_layout, int new_layout, long long src_stage, long long dst_stage,
                       long long src_access, long long dst_access) {
        vkrpc::RecordedCommand c;
        c.kind = "pipeline_barrier";
        c.image = tex.image;
        c.old_layout = old_layout;
        c.new_layout = new_layout;
        c.aspect = vkrpc::kImageAspectColor;
        c.src_stage = src_stage;
        c.dst_stage = dst_stage;
        c.src_access = src_access;
        c.dst_access = dst_access;
        c.barrier_base_mip = 0;
        c.barrier_level_count = 1;
        c.barrier_base_layer = 0;
        c.barrier_layer_count = 1;
        return c;
    };
    // The widened faithful upload payload (the copy_image_to_buffer 13-per-region convention):
    // args_i64=[dstImageLayout, regionCount, per region: bufferOffset, rowLength, imageHeight,
    // aspect, mip, baseLayer, layerCount, offX, offY, offZ, extW, extH, extD].
    auto copy_at = [&](long long off_x, long long off_y, long long ext_w, long long ext_h) {
        vkrpc::RecordedCommand c;
        c.kind = "copy_buffer_to_image";
        c.src_buffer = staging.buffer;
        c.image = tex.image;
        c.args_i64 = {VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      1,
                      0,
                      0,
                      0,
                      vkrpc::kImageAspectColor,
                      0,
                      0,
                      1,
                      off_x,
                      off_y,
                      0,
                      ext_w,
                      ext_h,
                      1};
        return c;
    };
    auto copy = [&]() { return copy_at(0, 0, kTex, kTex); };
    const vkrpc::RecordedCommand to_dst =
        barrier(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                VK_ACCESS_TRANSFER_WRITE_BIT);
    const vkrpc::RecordedCommand to_shader =
        barrier(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);

    // (GL/zink): barriers are forwarded FAITHFULLY now (the texture-upload allowlist is
    // lifted; the host driver is the authority). A direct UNDEFINED -> SHADER_READ_ONLY transition
    // -- which the old allowlist rejected -- is a valid Vulkan transition and is accepted at record
    // (a fresh command buffer, so the draw buffer is untouched).
    {
        const vkrpc::AllocateCommandBuffersResponse nb = backend.allocate_command_buffers(abr);
        vkrpc::RecordCommandBufferRequest faithful;
        faithful.command_buffer = nb.command_buffers[0];
        faithful.commands = {
            barrier(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                    VK_ACCESS_SHADER_READ_BIT)};
        VKR_CHECK(backend.record_command_buffer(faithful).ok);
    }
    // The widened upload (the ExtremeTuxRacer fix), pinned against the REAL backend's tracked
    // image: a SUB-REGION copy (the glTexSubImage2D shape the old one-full-image-region subset
    // rejected) records; an out-of-bounds one stays fail-closed.
    {
        const vkrpc::AllocateCommandBuffersResponse nb = backend.allocate_command_buffers(abr);
        vkrpc::RecordCommandBufferRequest sub_ok;
        sub_ok.command_buffer = nb.command_buffers[0];
        sub_ok.commands = {to_dst, copy_at(kTex / 4, kTex / 4, kTex / 2, kTex / 2)};
        VKR_CHECK(backend.record_command_buffer(sub_ok).ok);
        const vkrpc::AllocateCommandBuffersResponse nb2 = backend.allocate_command_buffers(abr);
        vkrpc::RecordCommandBufferRequest sub_oob;
        sub_oob.command_buffer = nb2.command_buffers[0];
        sub_oob.commands = {to_dst, copy_at(kTex / 2, kTex / 2, kTex, kTex)}; // spills the edge
        VKR_CHECK(!backend.record_command_buffer(sub_oob).ok);
    }
    // Negative: a command against an UNBOUND app image is rejected at record -- the
    // real worker never emits a barrier/copy against an unbound VkImage. Build a second
    // SAMPLED|TRANSFER_DST image and do NOT bind it.
    {
        const vkrpc::CreateImageResponse unbound = backend.create_image(tir);
        VKR_CHECK(unbound.ok && unbound.image != 0);
        const vkrpc::AllocateCommandBuffersResponse nb = backend.allocate_command_buffers(abr);
        vkrpc::RecordedCommand b = to_dst;
        b.image = unbound.image;
        vkrpc::RecordCommandBufferRequest bad;
        bad.command_buffer = nb.command_buffers[0];
        bad.commands = {b};
        VKR_CHECK(!backend.record_command_buffer(bad).ok);
        vkrpc::HandleRequest uh;
        uh.handle = unbound.image;
        VKR_CHECK(backend.destroy_image(uh).ok); // never bound, no views -> destroyable
    }

    vkrpc::AcquireNextImageRequest ar;
    ar.swapchain = sc.swapchain;
    ar.timeout = UINT64_MAX;
    ar.semaphore = sem_acq.semaphore;
    const vkrpc::AcquireNextImageResponse acq = backend.acquire_next_image(ar);
    VKR_CHECK(acq.ok);
    VKR_CHECK_EQ(acq.result, vkrpc::kVkSuccess);
    const int idx = acq.image_index;
    VKR_CHECK(idx >= 0 && static_cast<std::size_t>(idx) < framebuffers.size());

    // One command buffer: upload the texture (barrier, copy, barrier), then draw the textured
    // triangle sampling it.
    vkrpc::RecordCommandBufferRequest rec;
    rec.command_buffer = cmd;
    rec.one_time_submit = true;
    {
        vkrpc::RecordedCommand begin;
        begin.kind = "begin_render_pass";
        begin.render_pass = rp.render_pass;
        begin.framebuffer = framebuffers[static_cast<std::size_t>(idx)];
        begin.render_area_w = kExtent;
        begin.render_area_h = kExtent;
        begin.r = 0.05;
        begin.g = 0.05;
        begin.b = 0.08;
        begin.a = 1.0;
        vkrpc::RecordedCommand bindp;
        bindp.kind = "bind_pipeline";
        bindp.pipeline = gp.pipeline;
        vkrpc::RecordedCommand vp;
        vp.kind = "set_viewport";
        vp.vp_w = static_cast<double>(kExtent);
        vp.vp_h = static_cast<double>(kExtent);
        vp.vp_max_depth = 1.0;
        vkrpc::RecordedCommand scc;
        scc.kind = "set_scissor";
        scc.sc_w = kExtent;
        scc.sc_h = kExtent;
        vkrpc::RecordedCommand binds;
        binds.kind = "bind_descriptor_sets";
        binds.desc_layout = pl.pipeline_layout;
        binds.first_set = 0;
        binds.descriptor_sets = {set};
        vkrpc::RecordedCommand draw;
        draw.kind = "draw";
        draw.vertex_count = 3;
        draw.instance_count = 1;
        draw.first_vertex = 0;
        draw.first_instance = 0;
        vkrpc::RecordedCommand end;
        end.kind = "end_render_pass";
        rec.commands = {to_dst, copy(), to_shader, begin, bindp, vp, scc, binds, draw, end};
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
                 "integration_real_texture: drew + presented a textured triangle (%zu images)\n",
                 imgs.images.size());

    // --- Real-path lifetime edges (before teardown, handles still live) ---
    // The descriptor -> sampler destroy consult, on a MINIMAL command buffer that only binds the
    // set (no render pass -> no swapchain image touched, so no re-acquire is needed and the submit
    // stays validation-clean). Recording a lone bind_descriptor_sets bakes the set handle, so
    // destroying a referenced resource invalidates the buffer.
    const vkrpc::AllocateCommandBuffersResponse cbufs = backend.allocate_command_buffers(abr);
    VKR_CHECK(cbufs.ok && cbufs.command_buffers.size() == 1);
    const std::uint64_t bind_cmd = cbufs.command_buffers[0];
    vkrpc::RecordCommandBufferRequest bind_rec;
    bind_rec.command_buffer = bind_cmd;
    {
        vkrpc::RecordedCommand binds;
        binds.kind = "bind_descriptor_sets";
        binds.desc_layout = pl.pipeline_layout;
        binds.first_set = 0;
        binds.descriptor_sets = {set};
        bind_rec.commands = {binds};
    }
    vkrpc::QueueSubmitRequest bind_sub;
    bind_sub.queue = q.queue;
    bind_sub.command_buffers = {bind_cmd};
    bind_sub.fence = fence.fence;
    // Submittable while the sampler lives.
    VKR_CHECK(backend.record_command_buffer(bind_rec).ok);
    VKR_CHECK(backend.reset_fences({{fence.fence}}).ok);
    VKR_CHECK(backend.queue_submit(bind_sub).ok);
    VKR_CHECK(backend.wait_for_fences(wf).ok);
    // A live sampler blocks the device's destroy (device-child rule).
    VKR_CHECK(!backend.destroy_device({cd.device}).ok);
    // Destroying the bound sampler dangles the set; the recorded bind is no longer submittable.
    VKR_CHECK(backend.record_command_buffer(bind_rec).ok);
    VKR_CHECK(backend.destroy_sampler({sampler.sampler}).ok);
    VKR_CHECK(backend.reset_fences({{fence.fence}}).ok);
    VKR_CHECK(!backend.queue_submit(bind_sub).ok); // sampler destroy invalidated the recorded bind

    // --- Teardown, child-before-parent ---
    vkrpc::HandleRequest h;
    h.handle = gp.pipeline;
    VKR_CHECK(backend.destroy_pipeline(h).ok);
    h.handle = pl.pipeline_layout;
    VKR_CHECK(backend.destroy_pipeline_layout(h).ok);
    h.handle = dp.pool;
    VKR_CHECK(backend.destroy_descriptor_pool(h).ok); // frees the set
    h.handle = dsl.set_layout;
    VKR_CHECK(backend.destroy_descriptor_set_layout(h).ok);
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
    for (const std::uint64_t view : views) {
        h.handle = view;
        VKR_CHECK(backend.destroy_image_view(h).ok);
    }
    h.handle = sc.swapchain;
    VKR_CHECK(backend.destroy_swapchain(h).ok); // idles the device -> present completed
    // The texture image is blocked while its view lives, then destroyable once the view is gone.
    h.handle = tex.image;
    VKR_CHECK(!backend.destroy_image(h).ok);
    h.handle = tview.image_view;
    VKR_CHECK(backend.destroy_image_view(h).ok);
    h.handle = tex.image;
    VKR_CHECK(backend.destroy_image(h).ok);
    h.handle = staging.buffer;
    VKR_CHECK(backend.destroy_buffer(h).ok);
    h.handle = ubo.buffer;
    VKR_CHECK(backend.destroy_buffer(h).ok);
    h.handle = tmem.memory;
    VKR_CHECK(backend.free_memory(h).ok);
    h.handle = smem.memory;
    VKR_CHECK(backend.free_memory(h).ok);
    h.handle = umem.memory;
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

    return vkr::test::finish("integration_real_texture");
}
