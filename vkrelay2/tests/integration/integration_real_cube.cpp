// In-process real-backend test for the literal-vkcube shape -- a
// BUFFERLESS spinning cube on the host GPU, the deterministic regression half of the "vkcube on
// screen" gate (the on-screen half is run_vkcube.sh driving the real LunarG binary).
//
// This combines depth, texture, sampler, and combined-image-sampler support in the exact vkcube
// workload: geometry lives INSIDE the uniform buffer (mvp mat4 + 36 positions + 36
// attrs, indexed by gl_VertexIndex -- zero vertex bindings, draw of 36), a 2-binding set (0
// UNIFORM_BUFFER/VERTEX, 1 COMBINED_IMAGE_SAMPLER/FRAGMENT), a D16_UNORM depth attachment, and a
// sampled R8G8B8A8_UNORM texture.
//
// Like vkcube, the texture-upload path is chosen at RUNTIME from the real format properties: the
// LINEAR-mapped path (PREINITIALIZED host-visible image written at the real rowPitch ->
// SHADER_READ_ONLY) if the driver advertises linearTilingFeatures & SAMPLED, else the staging path
// (TRANSFER_SRC buffer -> copy_buffer_to_image into a device-local OPTIMAL image). So on a given
// GPU this test exercises exactly the path the canary + real vkcube will take -- closing the
// linear-path coverage the staging-only texture test does not exercise.
//
// Windows-only. Skips gracefully (still passes) if there is no usable Vulkan device / no WSI
// support. The validation-clean gate is a manual run with
// VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation.
#include "windows/worker/real_vulkan_backend.hpp"

#include "common/vkrpc/vulkan_session.hpp"
#include "tests/cube_geometry.h"
#include "tests/cube_spv.h"
#include "tests/real_backend_test_utils.hpp"
#include "tests/test_assert.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

using namespace vkr;

namespace {
using test::pick_type;
} // namespace

int main() {
    worker::RealVulkanBackend backend("", "", false);

    const vkrpc::CreateInstanceResponse ci = backend.create_instance({});
    if (!ci.ok) {
        std::fprintf(stderr, "integration_real_cube: skipped (no instance: %s)\n",
                     ci.reason.c_str());
        return vkr::test::finish("integration_real_cube");
    }
    vkrpc::EnumeratePhysicalDevicesRequest er;
    er.instance = ci.instance;
    const vkrpc::EnumeratePhysicalDevicesResponse en = backend.enumerate_physical_devices(er);
    if (!en.ok || en.devices.empty()) {
        std::fprintf(stderr, "integration_real_cube: skipped (no physical device)\n");
        return vkr::test::finish("integration_real_cube");
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

    // Honest format properties drive the upload-path choice (texture) and confirm depth (vkcube
    // hardcodes D16_UNORM but the format is a depth-stencil attachment on any real GPU).
    vkrpc::GetPhysicalDeviceFormatPropertiesRequest fpr_tex;
    fpr_tex.physical_device = phys;
    fpr_tex.format = vkrpc::kFormatR8G8B8A8Unorm;
    const vkrpc::GetPhysicalDeviceFormatPropertiesResponse fp_tex =
        backend.get_physical_device_format_properties(fpr_tex);
    VKR_CHECK(fp_tex.ok);
    VKR_CHECK((fp_tex.optimal_tiling_features & vkrpc::kFormatFeatureSampledImage) != 0);
    vkrpc::GetPhysicalDeviceFormatPropertiesRequest fpr_depth;
    fpr_depth.physical_device = phys;
    fpr_depth.format = vkrpc::kFormatD16Unorm;
    const vkrpc::GetPhysicalDeviceFormatPropertiesResponse fp_depth =
        backend.get_physical_device_format_properties(fpr_depth);
    VKR_CHECK(fp_depth.ok);
    VKR_CHECK((fp_depth.optimal_tiling_features & vkrpc::kFormatFeatureDepthStencilAttachment) !=
              0);

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
        std::fprintf(stderr, "integration_real_cube: skipped (no swapchain: %s)\n",
                     sc.reason.c_str());
        return vkr::test::finish("integration_real_cube");
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

    // --- The depth buffer: D16_UNORM, OPTIMAL, device-local, one shared image (FIFO serializes)
    // ---
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
    const long long depth_type = pick_type(mp, depth.mem_type_bits, /*want_device_local=*/true);
    VKR_CHECK(depth_type >= 0);
    vkrpc::AllocateMemoryRequest dam;
    dam.device = cd.device;
    dam.allocation_size = depth.mem_size;
    dam.memory_type_index = depth_type;
    const vkrpc::AllocateMemoryResponse dmem = backend.allocate_memory(dam);
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

    // --- The texture: vkcube's runtime path choice (linear-mapped vs staging) ---
    const int kTex = 16;
    const std::uint64_t kTexBytes = static_cast<std::uint64_t>(kTex) * kTex * 4;
    // The checkerboard the texture is filled with (row-major RGBA8), built once and laid out per
    // path.
    auto checker_row = [&](std::string& dst, std::size_t row_off, int y) {
        for (int x = 0; x < kTex; ++x) {
            const std::size_t o = row_off + static_cast<std::size_t>(x) * 4;
            const bool on = ((x ^ y) & 1) != 0;
            dst[o + 0] = on ? '\xff' : '\x20';
            dst[o + 1] = '\x40';
            dst[o + 2] = on ? '\x20' : '\xff';
            dst[o + 3] = '\xff';
        }
    };

    const bool linear_supported =
        (fp_tex.linear_tiling_features & vkrpc::kFormatFeatureSampledImage) != 0;

    std::uint64_t tex_image = 0;
    std::uint64_t tex_mem = 0;
    std::uint64_t tview_handle = 0;
    std::uint64_t staging_buffer = 0; // staging path only
    std::uint64_t staging_mem = 0;    // staging path only
    std::vector<vkrpc::RecordedCommand> upload;
    bool path_is_linear = false;

    auto barrier = [&](std::uint64_t image, int old_layout, int new_layout, long long src_stage,
                       long long dst_stage, long long src_access, long long dst_access) {
        vkrpc::RecordedCommand c;
        c.kind = "pipeline_barrier";
        c.image = image;
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

    // Try the linear path only if advertised AND a host-visible coherent memory type backs a LINEAR
    // SAMPLED image; otherwise fall through to staging (always available on a discrete GPU).
    if (linear_supported) {
        vkrpc::CreateImageRequest lir;
        lir.device = cd.device;
        lir.image_type = vkrpc::kImageType2D;
        lir.format = vkrpc::kFormatR8G8B8A8Unorm;
        lir.width = kTex;
        lir.height = kTex;
        lir.depth = 1;
        lir.mip_levels = 1;
        lir.array_layers = 1;
        lir.samples = 1;
        lir.tiling = vkrpc::kImageTilingLinear;
        lir.usage = vkrpc::kImageUsageSampled;
        lir.sharing_mode = 0;
        lir.initial_layout = vkrpc::kImageLayoutPreinitialized;
        const vkrpc::CreateImageResponse limg = backend.create_image(lir);
        VKR_CHECK(limg.ok && limg.image != 0);
        VKR_CHECK(limg.has_subresource_layout); // LINEAR: honest rowPitch
        const long long ltype = pick_type(mp, limg.mem_type_bits, /*want_device_local=*/false);
        if (ltype >= 0) {
            path_is_linear = true;
            tex_image = limg.image;
            vkrpc::AllocateMemoryRequest lam;
            lam.device = cd.device;
            lam.allocation_size = limg.mem_size;
            lam.memory_type_index = ltype;
            const vkrpc::AllocateMemoryResponse lmem = backend.allocate_memory(lam);
            VKR_CHECK(lmem.ok && lmem.memory != 0);
            tex_mem = lmem.memory;
            VKR_CHECK(backend.bind_image_memory({tex_image, tex_mem, 0}).ok);
            // Write the checkerboard directly into the mapped image at the real rowPitch.
            const std::uint64_t span = limg.sr_row_pitch * kTex;
            std::string px(static_cast<std::size_t>(span), '\0');
            for (int y = 0; y < kTex; ++y) {
                checker_row(px, static_cast<std::size_t>(limg.sr_row_pitch) * y, y);
            }
            vkrpc::WriteMemoryRangesRequest wr;
            vkrpc::MemoryUpload u;
            u.memory = tex_mem;
            u.ranges.push_back({limg.sr_offset, span});
            wr.uploads.push_back(u);
            wr.payload = std::move(px);
            VKR_CHECK(backend.write_memory_ranges(wr).ok);
            // Finalize: PREINITIALIZED -> SHADER_READ_ONLY (no copy on the linear path).
            upload.push_back(barrier(tex_image, vkrpc::kImageLayoutPreinitialized,
                                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                     VK_PIPELINE_STAGE_HOST_BIT,
                                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                     VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT));
        } else {
            VKR_CHECK(backend.destroy_image({limg.image}).ok); // never bound -> destroyable
        }
    }

    if (!path_is_linear) {
        // Staging path: an OPTIMAL device-local SAMPLED|TRANSFER_DST image, filled from a
        // TRANSFER_SRC host-visible buffer via copy_buffer_to_image, with the
        // UNDEFINED -> TRANSFER_DST -> SHADER_READ_ONLY transitions.
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
        const vkrpc::CreateImageResponse timg = backend.create_image(tir);
        VKR_CHECK(timg.ok && timg.image != 0);
        tex_image = timg.image;
        const long long ttype = pick_type(mp, timg.mem_type_bits, /*want_device_local=*/true);
        VKR_CHECK(ttype >= 0);
        vkrpc::AllocateMemoryRequest tam;
        tam.device = cd.device;
        tam.allocation_size = timg.mem_size;
        tam.memory_type_index = ttype;
        const vkrpc::AllocateMemoryResponse tmem = backend.allocate_memory(tam);
        VKR_CHECK(tmem.ok && tmem.memory != 0);
        tex_mem = tmem.memory;
        VKR_CHECK(backend.bind_image_memory({tex_image, tex_mem, 0}).ok);

        vkrpc::CreateBufferRequest sbq;
        sbq.device = cd.device;
        sbq.size = kTexBytes;
        sbq.usage = vkrpc::kBufferUsageTransferSrc;
        sbq.sharing_mode = 0;
        const vkrpc::CreateBufferResponse stg = backend.create_buffer(sbq);
        VKR_CHECK(stg.ok && stg.buffer != 0);
        staging_buffer = stg.buffer;
        const long long stype = pick_type(mp, stg.mem_type_bits, /*want_device_local=*/false);
        VKR_CHECK(stype >= 0);
        vkrpc::AllocateMemoryRequest sam;
        sam.device = cd.device;
        sam.allocation_size = stg.mem_size;
        sam.memory_type_index = stype;
        const vkrpc::AllocateMemoryResponse smem = backend.allocate_memory(sam);
        VKR_CHECK(smem.ok && smem.memory != 0);
        staging_mem = smem.memory;
        VKR_CHECK(backend.bind_buffer_memory({staging_buffer, staging_mem, 0}).ok);
        {
            std::string px(static_cast<std::size_t>(kTexBytes), '\0');
            for (int y = 0; y < kTex; ++y) {
                checker_row(px, static_cast<std::size_t>(y) * kTex * 4, y);
            }
            vkrpc::WriteMemoryRangesRequest wr;
            vkrpc::MemoryUpload u;
            u.memory = staging_mem;
            u.ranges.push_back({0, kTexBytes});
            wr.uploads.push_back(u);
            wr.payload = std::move(px);
            VKR_CHECK(backend.write_memory_ranges(wr).ok);
        }
        upload.push_back(barrier(tex_image, VK_IMAGE_LAYOUT_UNDEFINED,
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                 VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 0, VK_ACCESS_TRANSFER_WRITE_BIT));
        vkrpc::RecordedCommand copy;
        copy.kind = "copy_buffer_to_image";
        copy.src_buffer = staging_buffer;
        copy.image = tex_image;
        // The widened faithful payload: args_i64=[dstImageLayout, regionCount, 13 per region]
        // (one full-image mip-0 region here).
        copy.args_i64 = {VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         1,
                         0,
                         0,
                         0,
                         vkrpc::kImageAspectColor,
                         0,
                         0,
                         1,
                         0,
                         0,
                         0,
                         kTex,
                         kTex,
                         1};
        upload.push_back(copy);
        upload.push_back(barrier(tex_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                 VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT));
    }
    std::fprintf(stderr, "integration_real_cube: texture upload path = %s\n",
                 path_is_linear ? "linear-mapped" : "staging");

    vkrpc::CreateImageViewRequest tvr;
    tvr.image = tex_image;
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
    tview_handle = tview.image_view;

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

    // --- The UBO: mvp + 36 positions + 36 attrs (1216 bytes), host-visible coherent ---
    vkrpc::CreateBufferRequest ubq;
    ubq.device = cd.device;
    ubq.size = cube_geom::kUboBytes;
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
        float ubo_floats[cube_geom::kUboFloats];
        cube_geom::build_ubo(0.0f, ubo_floats); // a single deterministic frame (angle 0)
        std::string payload(reinterpret_cast<const char*>(ubo_floats), cube_geom::kUboBytes);
        vkrpc::WriteMemoryRangesRequest wr;
        vkrpc::MemoryUpload u;
        u.memory = umem.memory;
        u.ranges.push_back({0, cube_geom::kUboBytes});
        wr.uploads.push_back(u);
        wr.payload = std::move(payload);
        VKR_CHECK(backend.write_memory_ranges(wr).ok);
    }

    // --- The 2-binding descriptor set: 0 UNIFORM_BUFFER/VERTEX, 1 COMBINED_IMAGE_SAMPLER/FRAGMENT
    // ---
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
            {sampler.sampler, tview_handle, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
        wr.writes = {wu, wi};
        VKR_CHECK(backend.update_descriptor_sets(wr).ok);
    }

    // --- Shaders (cube SPIR-V) / color+depth render pass / framebuffers / depth-stencil pipeline
    // ---
    auto make_shader = [&](const std::uint32_t* spv, std::uint32_t bytes) {
        vkrpc::CreateShaderModuleRequest r;
        r.device = cd.device;
        r.code.assign(reinterpret_cast<const char*>(spv), bytes);
        r.code_size = bytes;
        return r;
    };
    const vkrpc::CreateShaderModuleResponse vs =
        backend.create_shader_module(make_shader(cube_spv::kVert, cube_spv::kVertBytes));
    const vkrpc::CreateShaderModuleResponse fs =
        backend.create_shader_module(make_shader(cube_spv::kFrag, cube_spv::kFragBytes));
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
    gpr.vertex_binding_count = 0; // bufferless: geometry comes from the UBO via gl_VertexIndex
    gpr.vertex_attribute_count = 0;
    gpr.cull_mode = VK_CULL_MODE_NONE; // depth (not winding) sorts the cube
    gpr.front_face = VK_FRONT_FACE_CLOCKWISE;
    gpr.dynamic_states = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    gpr.layout = pl.pipeline_layout;
    gpr.render_pass = rp.render_pass;
    gpr.subpass = 0;
    gpr.has_depth_stencil = true;
    gpr.depth_test_enable = 1;
    gpr.depth_write_enable = 1;
    gpr.depth_compare_op = VK_COMPARE_OP_LESS_OR_EQUAL;
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

    // VK_FENCE_CREATE_SIGNALED_BIT must be honored: a created-signaled fence is ready immediately
    // (a timeout-0 wait succeeds with NO submit), while the plain `fence` above is not (timeout-0
    // waits TIME OUT). Real frame loops (e.g. vkcube) create their per-frame fences signaled and
    // wait on them before first use; dropping the flag deadlocks that wait. This A/B proves the
    // flag round-trips the wire AND is applied on the real worker.
    {
        vkrpc::CreateFenceRequest sfr;
        sfr.device = cd.device;
        sfr.signaled = true;
        const vkrpc::CreateFenceResponse sfence = backend.create_fence(sfr);
        VKR_CHECK(sfence.ok);
        vkrpc::WaitForFencesRequest sw;
        sw.fences = {sfence.fence};
        sw.wait_all = true;
        sw.timeout = 0; // poll: signaled -> ready now
        const vkrpc::WaitForFencesResponse swr = backend.wait_for_fences(sw);
        VKR_CHECK(swr.ok);
        VKR_CHECK_EQ(swr.result, vkrpc::kVkSuccess);
        vkrpc::WaitForFencesRequest uw;
        uw.fences = {fence.fence};
        uw.wait_all = true;
        uw.timeout = 0; // poll: unsignaled, never submitted -> times out
        const vkrpc::WaitForFencesResponse uwr = backend.wait_for_fences(uw);
        VKR_CHECK(uwr.ok);
        VKR_CHECK_EQ(uwr.result, vkrpc::kVkTimeout);
        vkrpc::HandleRequest sh;
        sh.handle = sfence.fence;
        VKR_CHECK(backend.destroy_fence(sh).ok);
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

    // One command buffer: upload the texture (path-specific), then draw the bufferless cube with
    // depth + the 2-binding set.
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
        begin.has_depth_clear = true;
        begin.depth_clear = 1.0;
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
        draw.vertex_count = cube_geom::kCubeVertices; // 36 -- bufferless
        draw.instance_count = 1;
        draw.first_vertex = 0;
        draw.first_instance = 0;
        vkrpc::RecordedCommand end;
        end.kind = "end_render_pass";
        rec.commands = upload; // the path-specific texture upload first
        rec.commands.push_back(begin);
        rec.commands.push_back(bindp);
        rec.commands.push_back(vp);
        rec.commands.push_back(scc);
        rec.commands.push_back(binds);
        rec.commands.push_back(draw);
        rec.commands.push_back(end);
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
                 "integration_real_cube: drew + presented a textured depth cube (%zu images)\n",
                 imgs.images.size());

    // --- A couple of cube-shaped lifetime edges (before teardown, handles still live) ---
    // A live sampler blocks the device's destroy (device-child rule, spanning the combined-image-
    // sampler binding).
    VKR_CHECK(!backend.destroy_device({cd.device}).ok);

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
    h.handle = sampler.sampler;
    VKR_CHECK(backend.destroy_sampler(h).ok);
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
    h.handle = tex_image;
    VKR_CHECK(!backend.destroy_image(h).ok);
    h.handle = tview_handle;
    VKR_CHECK(backend.destroy_image_view(h).ok);
    h.handle = tex_image;
    VKR_CHECK(backend.destroy_image(h).ok);
    // The depth image is blocked while its view lives, then destroyable.
    h.handle = depth.image;
    VKR_CHECK(!backend.destroy_image(h).ok);
    h.handle = dview.image_view;
    VKR_CHECK(backend.destroy_image_view(h).ok);
    h.handle = depth.image;
    VKR_CHECK(backend.destroy_image(h).ok);
    if (staging_buffer != 0) {
        h.handle = staging_buffer;
        VKR_CHECK(backend.destroy_buffer(h).ok);
    }
    h.handle = ubo.buffer;
    VKR_CHECK(backend.destroy_buffer(h).ok);
    h.handle = tex_mem;
    VKR_CHECK(backend.free_memory(h).ok);
    if (staging_mem != 0) {
        h.handle = staging_mem;
        VKR_CHECK(backend.free_memory(h).ok);
    }
    h.handle = dmem.memory;
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

    return vkr::test::finish("integration_real_cube");
}
