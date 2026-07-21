// Observable real-GPU proof for core vkCmdDrawIndexedIndirect replay.
//
// Two indexed indirect commands draw disjoint solid-color triangles into an offscreen RGBA8
// image. The first pass submits drawCount=1 and proves only the left triangle appears. When the
// host exposes multiDrawIndirect, a second pass submits drawCount=2 and proves both appear. Pixel
// readback makes this a render-semantic canary, not merely an API-acceptance test.
#include "windows/worker/real_vulkan_backend.hpp"

#include "common/vkrpc/indirect_draw_validation.h"
#include "common/vkrpc/vulkan_session.hpp"
#include "tests/test_assert.hpp"
#include "tests/vbo_spv.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include <vulkan/vulkan.h>

using namespace vkr;

namespace {

long long pick_type(const vkrpc::GetPhysicalDeviceMemoryPropertiesResponse& props,
                    std::uint64_t type_bits, bool device_local) {
    for (std::size_t i = 0; i < props.types.size() && i < 32; ++i) {
        if ((type_bits & (std::uint64_t{1} << i)) == 0) {
            continue;
        }
        const std::uint64_t flags = props.types[i].property_flags;
        const bool coherent = (flags & vkrpc::kMemoryPropertyHostVisible) != 0 &&
                              (flags & vkrpc::kMemoryPropertyHostCoherent) != 0;
        if ((device_local && (flags & vkrpc::kMemoryPropertyDeviceLocal) != 0) ||
            (!device_local && coherent)) {
            return static_cast<long long>(i);
        }
    }
    return -1;
}

struct BoundBuffer {
    std::uint64_t buffer = 0;
    std::uint64_t memory = 0;
};

} // namespace

int main() {
    worker::RealVulkanBackend backend("", "", false);
    const auto ci = backend.create_instance({});
    if (!ci.ok) {
        std::fprintf(stderr, "integration_real_indirect: skipped (no instance: %s)\n",
                     ci.reason.c_str());
        return vkr::test::finish("integration_real_indirect");
    }
    vkrpc::EnumeratePhysicalDevicesRequest er;
    er.instance = ci.instance;
    const auto en = backend.enumerate_physical_devices(er);
    if (!en.ok || en.devices.empty()) {
        std::fprintf(stderr, "integration_real_indirect: skipped (no physical device)\n");
        return vkr::test::finish("integration_real_indirect");
    }
    const std::uint64_t phys = en.devices.front().handle;
    vkrpc::GetPhysicalDeviceFeaturesRequest feature_req;
    feature_req.physical_device = phys;
    const auto features = backend.get_physical_device_features(feature_req);
    VKR_CHECK(features.ok);
    const bool multi = (features.feature_bits & vkrpc::kFeatureMultiDrawIndirect) != 0;
    vkrpc::CreateDeviceRequest cdr;
    cdr.instance = ci.instance;
    cdr.physical_device = phys;
    if (multi) {
        cdr.enabled_feature_bits = vkrpc::kFeatureMultiDrawIndirect;
    }
    const auto cd = backend.create_device(cdr);
    VKR_CHECK(cd.ok);
    vkrpc::GetDeviceQueueRequest gqr;
    gqr.device = cd.device;
    gqr.queue_family_index = cd.queue_family_index;
    const auto queue = backend.get_device_queue(gqr);
    VKR_CHECK(queue.ok);

    vkrpc::GetPhysicalDeviceFormatPropertiesRequest fpr;
    fpr.physical_device = phys;
    fpr.format = vkrpc::kFormatR8G8B8A8Unorm;
    const auto format_props = backend.get_physical_device_format_properties(fpr);
    const std::uint64_t format_need =
        vkrpc::kFormatFeatureColorAttachment | vkrpc::kFormatFeatureTransferSrc;
    if (!format_props.ok || (format_props.optimal_tiling_features & format_need) != format_need) {
        std::fprintf(stderr,
                     "integration_real_indirect: skipped (RGBA8 lacks color+transfer-src)\n");
        return vkr::test::finish("integration_real_indirect");
    }
    vkrpc::GetPhysicalDeviceMemoryPropertiesRequest mpr;
    mpr.physical_device = phys;
    const auto memory_props = backend.get_physical_device_memory_properties(mpr);
    VKR_CHECK(memory_props.ok);

    constexpr int kExtent = 64;
    constexpr std::uint64_t kReadbackBytes = kExtent * kExtent * 4;
    const float vertices[] = {
        -0.9f, -0.7f, 1.0f, 0.0f, 0.0f, // left, red
        -0.1f, -0.7f, 1.0f, 0.0f, 0.0f, -0.5f, 0.7f, 1.0f, 0.0f, 0.0f,
        0.1f,  -0.7f, 0.0f, 1.0f, 0.0f, // right, green
        0.9f,  -0.7f, 0.0f, 1.0f, 0.0f, 0.5f,  0.7f, 0.0f, 1.0f, 0.0f,
    };
    const std::uint32_t indices[] = {0, 1, 2, 3, 4, 5};
    const VkDrawIndexedIndirectCommand draws[] = {
        {3, 1, 0, 0, 0},
        {3, 1, 3, 0, 0},
    };

    auto make_buffer = [&](std::uint64_t usage, std::uint64_t size,
                           const void* initial) -> BoundBuffer {
        vkrpc::CreateBufferRequest req;
        req.device = cd.device;
        req.size = size;
        req.usage = usage;
        req.sharing_mode = VK_SHARING_MODE_EXCLUSIVE;
        const auto buffer = backend.create_buffer(req);
        VKR_CHECK(buffer.ok && buffer.mem_size >= size);
        const long long type = pick_type(memory_props, buffer.mem_type_bits, false);
        VKR_CHECK(type >= 0);
        vkrpc::AllocateMemoryRequest amr;
        amr.device = cd.device;
        amr.allocation_size = buffer.mem_size;
        amr.memory_type_index = static_cast<int>(type);
        const auto memory = backend.allocate_memory(amr);
        VKR_CHECK(memory.ok);
        VKR_CHECK(backend.bind_buffer_memory({buffer.buffer, memory.memory, 0}).ok);
        if (initial != nullptr) {
            vkrpc::WriteMemoryRangesRequest write;
            vkrpc::MemoryUpload upload;
            upload.memory = memory.memory;
            upload.ranges.push_back({0, size});
            write.uploads.push_back(upload);
            write.payload.assign(reinterpret_cast<const char*>(initial),
                                 static_cast<std::size_t>(size));
            VKR_CHECK(backend.write_memory_ranges(write).ok);
        }
        return {buffer.buffer, memory.memory};
    };
    const BoundBuffer vertex =
        make_buffer(vkrpc::kBufferUsageVertexBuffer, sizeof(vertices), vertices);
    const BoundBuffer index = make_buffer(vkrpc::kBufferUsageIndexBuffer, sizeof(indices), indices);
    const BoundBuffer indirect =
        make_buffer(vkrpc::kBufferUsageIndirectBuffer, sizeof(draws), draws);
    const BoundBuffer readback =
        make_buffer(vkrpc::kBufferUsageTransferDst, kReadbackBytes, nullptr);

    vkrpc::CreateImageRequest image_req;
    image_req.device = cd.device;
    image_req.image_type = vkrpc::kImageType2D;
    image_req.format = vkrpc::kFormatR8G8B8A8Unorm;
    image_req.width = kExtent;
    image_req.height = kExtent;
    image_req.depth = 1;
    image_req.mip_levels = 1;
    image_req.array_layers = 1;
    image_req.samples = 1;
    image_req.tiling = vkrpc::kImageTilingOptimal;
    image_req.usage = static_cast<std::uint64_t>(vkrpc::kImageUsageColorAttachment) |
                      static_cast<std::uint64_t>(vkrpc::kImageUsageTransferSrc);
    image_req.sharing_mode = VK_SHARING_MODE_EXCLUSIVE;
    image_req.initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    const auto image = backend.create_image(image_req);
    VKR_CHECK(image.ok);
    const long long image_type = pick_type(memory_props, image.mem_type_bits, true);
    VKR_CHECK(image_type >= 0);
    vkrpc::AllocateMemoryRequest image_amr;
    image_amr.device = cd.device;
    image_amr.allocation_size = image.mem_size;
    image_amr.memory_type_index = static_cast<int>(image_type);
    const auto image_memory = backend.allocate_memory(image_amr);
    VKR_CHECK(image_memory.ok);
    VKR_CHECK(backend.bind_image_memory({image.image, image_memory.memory, 0}).ok);
    vkrpc::CreateImageViewRequest ivr;
    ivr.image = image.image;
    ivr.view_type = VK_IMAGE_VIEW_TYPE_2D;
    ivr.format = vkrpc::kFormatR8G8B8A8Unorm;
    ivr.swizzle_r = ivr.swizzle_g = ivr.swizzle_b = ivr.swizzle_a = VK_COMPONENT_SWIZZLE_IDENTITY;
    ivr.aspect = vkrpc::kImageAspectColor;
    ivr.base_mip_level = 0;
    ivr.level_count = 1;
    ivr.base_array_layer = 0;
    ivr.layer_count = 1;
    const auto view = backend.create_image_view(ivr);
    VKR_CHECK(view.ok);

    auto shader_request = [&](const std::uint32_t* spv, std::size_t bytes) {
        vkrpc::CreateShaderModuleRequest req;
        req.device = cd.device;
        req.code.assign(reinterpret_cast<const char*>(spv), bytes);
        req.code_size = bytes;
        return req;
    };
    const auto vs =
        backend.create_shader_module(shader_request(vbo_spv::kVert, vbo_spv::kVertBytes));
    const auto fs =
        backend.create_shader_module(shader_request(vbo_spv::kFrag, vbo_spv::kFragBytes));
    VKR_CHECK(vs.ok && fs.ok);
    vkrpc::CreateRenderPassRequest rpr;
    rpr.device = cd.device;
    vkrpc::AttachmentDesc attachment;
    attachment.format = vkrpc::kFormatR8G8B8A8Unorm;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.load_op = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.store_op = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencil_load_op = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencil_store_op = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachment.final_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    rpr.attachments = {attachment};
    rpr.color_attachment = 0;
    rpr.color_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    const auto render_pass = backend.create_render_pass(rpr);
    VKR_CHECK(render_pass.ok);
    vkrpc::CreateFramebufferRequest fbr;
    fbr.device = cd.device;
    fbr.render_pass = render_pass.render_pass;
    fbr.attachment_views = {view.image_view};
    fbr.width = kExtent;
    fbr.height = kExtent;
    fbr.layers = 1;
    const auto framebuffer = backend.create_framebuffer(fbr);
    VKR_CHECK(framebuffer.ok);
    vkrpc::CreatePipelineLayoutRequest plr;
    plr.device = cd.device;
    plr.set_layout_count = 0;
    plr.push_constant_range_count = 0;
    const auto pipeline_layout = backend.create_pipeline_layout(plr);
    VKR_CHECK(pipeline_layout.ok);
    vkrpc::CreateGraphicsPipelinesRequest gpr;
    gpr.device = cd.device;
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
    vkrpc::VertexBindingDesc binding;
    binding.binding = 0;
    binding.stride = 20;
    binding.input_rate = VK_VERTEX_INPUT_RATE_VERTEX;
    gpr.vertex_bindings = {binding};
    vkrpc::VertexAttributeDesc pos;
    pos.location = 0;
    pos.binding = 0;
    pos.format = VK_FORMAT_R32G32_SFLOAT;
    pos.offset = 0;
    vkrpc::VertexAttributeDesc color;
    color.location = 1;
    color.binding = 0;
    color.format = VK_FORMAT_R32G32B32_SFLOAT;
    color.offset = 8;
    gpr.vertex_attributes = {pos, color};
    gpr.vertex_binding_count = 1;
    gpr.vertex_attribute_count = 2;
    gpr.cull_mode = VK_CULL_MODE_NONE;
    gpr.front_face = VK_FRONT_FACE_CLOCKWISE;
    gpr.dynamic_states = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    gpr.layout = pipeline_layout.pipeline_layout;
    gpr.render_pass = render_pass.render_pass;
    gpr.subpass = 0;
    const auto pipeline = backend.create_graphics_pipelines(gpr);
    VKR_CHECK(pipeline.ok);

    vkrpc::CreateCommandPoolRequest cpr;
    cpr.device = cd.device;
    cpr.queue_family_index = cd.queue_family_index;
    const auto pool = backend.create_command_pool(cpr);
    vkrpc::AllocateCommandBuffersRequest acbr;
    acbr.command_pool = pool.command_pool;
    acbr.count = 1;
    const auto command_buffers = backend.allocate_command_buffers(acbr);
    VKR_CHECK(pool.ok && command_buffers.ok && command_buffers.command_buffers.size() == 1);
    const std::uint64_t command_buffer = command_buffers.command_buffers.front();
    vkrpc::CreateFenceRequest fence_req;
    fence_req.device = cd.device;
    const auto fence = backend.create_fence(fence_req);
    VKR_CHECK(fence.ok);

    auto render = [&](long long draw_count) {
        vkrpc::RecordedCommand begin;
        begin.kind = "begin_render_pass";
        begin.render_pass = render_pass.render_pass;
        begin.framebuffer = framebuffer.framebuffer;
        begin.render_area_w = kExtent;
        begin.render_area_h = kExtent;
        begin.a = 1.0;
        vkrpc::RecordedCommand bind_pipeline;
        bind_pipeline.kind = "bind_pipeline";
        bind_pipeline.pipeline = pipeline.pipeline;
        vkrpc::RecordedCommand viewport;
        viewport.kind = "set_viewport";
        viewport.vp_w = kExtent;
        viewport.vp_h = kExtent;
        viewport.vp_max_depth = 1.0;
        vkrpc::RecordedCommand scissor;
        scissor.kind = "set_scissor";
        scissor.sc_w = kExtent;
        scissor.sc_h = kExtent;
        vkrpc::RecordedCommand bind_vertex;
        bind_vertex.kind = "bind_vertex_buffers";
        bind_vertex.first_binding = 0;
        bind_vertex.vertex_buffers = {vertex.buffer};
        bind_vertex.vertex_buffer_offsets = {0};
        vkrpc::RecordedCommand bind_index;
        bind_index.kind = "bind_index_buffer";
        bind_index.args_u64 = {index.buffer, 0};
        bind_index.args_i64 = {VK_INDEX_TYPE_UINT32};
        vkrpc::RecordedCommand draw;
        draw.kind = "draw_indexed_indirect";
        draw.src_buffer = indirect.buffer;
        draw.args_u64 = {0};
        draw.args_i64 = {draw_count, sizeof(VkDrawIndexedIndirectCommand)};
        vkrpc::RecordedCommand end;
        end.kind = "end_render_pass";
        vkrpc::RecordedCommand barrier;
        barrier.kind = "pipeline_barrier";
        barrier.image = image.image;
        barrier.old_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.new_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.aspect = vkrpc::kImageAspectColor;
        barrier.src_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        barrier.dst_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        barrier.src_access = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        barrier.dst_access = VK_ACCESS_TRANSFER_READ_BIT;
        barrier.barrier_level_count = 1;
        barrier.barrier_layer_count = 1;
        vkrpc::RecordedCommand copy;
        copy.kind = "copy_image_to_buffer";
        copy.args_u64 = {image.image, readback.buffer};
        copy.args_i64 = {VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
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
                         kExtent,
                         kExtent,
                         1};
        vkrpc::RecordCommandBufferRequest record;
        record.command_buffer = command_buffer;
        record.one_time_submit = true;
        record.commands = {begin,      bind_pipeline, viewport, scissor, bind_vertex,
                           bind_index, draw,          end,      barrier, copy};
        const auto recorded = backend.record_command_buffer(record);
        if (!recorded.ok) {
            std::fprintf(stderr, "integration_real_indirect: record rejected: %s\n",
                         recorded.reason.c_str());
        }
        VKR_CHECK(recorded.ok);
        vkrpc::QueueSubmitRequest submit;
        submit.queue = queue.queue;
        submit.command_buffers = {command_buffer};
        submit.fence = fence.fence;
        const auto submitted = backend.queue_submit(submit);
        VKR_CHECK(submitted.ok && submitted.result == vkrpc::kVkSuccess);
        vkrpc::WaitForFencesRequest wait;
        wait.fences = {fence.fence};
        wait.wait_all = true;
        wait.timeout = 10'000'000'000ull;
        const auto waited = backend.wait_for_fences(wait);
        VKR_CHECK(waited.ok && waited.result == vkrpc::kVkSuccess);
        vkrpc::ReadMemoryRangesRequest read;
        vkrpc::MemoryUpload range;
        range.memory = readback.memory;
        range.ranges.push_back({0, kReadbackBytes});
        read.reads.push_back(range);
        const auto pixels = backend.read_memory_ranges(read);
        VKR_CHECK(pixels.ok && pixels.payload.size() == kReadbackBytes);
        return pixels.payload;
    };
    auto pixel = [&](const std::string& bytes, int x, int y, int channel) {
        return static_cast<unsigned char>(bytes[(y * kExtent + x) * 4 + channel]);
    };
    const std::string one = render(1);
    VKR_CHECK(pixel(one, 16, 32, 0) > 200 && pixel(one, 16, 32, 1) < 40);
    VKR_CHECK(pixel(one, 48, 32, 0) < 40 && pixel(one, 48, 32, 1) < 40);
    if (multi) {
        vkrpc::ResetFencesRequest reset;
        reset.fences = {fence.fence};
        VKR_CHECK(backend.reset_fences(reset).ok);
        const std::string two = render(2);
        VKR_CHECK(pixel(two, 16, 32, 0) > 200 && pixel(two, 16, 32, 1) < 40);
        VKR_CHECK(pixel(two, 48, 32, 0) < 40 && pixel(two, 48, 32, 1) > 200);
    } else {
        std::fprintf(stderr,
                     "integration_real_indirect: multi-draw leg skipped (feature absent)\n");
    }
    std::fprintf(stderr, "integration_real_indirect: indexed indirect pixels verified%s\n",
                 multi ? " (drawCount 1 and 2)" : " (drawCount 1)");

    VKR_CHECK(backend.destroy_command_pool({pool.command_pool}).ok);
    VKR_CHECK(backend.destroy_fence({fence.fence}).ok);
    VKR_CHECK(backend.destroy_pipeline({pipeline.pipeline}).ok);
    VKR_CHECK(backend.destroy_pipeline_layout({pipeline_layout.pipeline_layout}).ok);
    VKR_CHECK(backend.destroy_framebuffer({framebuffer.framebuffer}).ok);
    VKR_CHECK(backend.destroy_render_pass({render_pass.render_pass}).ok);
    VKR_CHECK(backend.destroy_shader_module({vs.shader_module}).ok);
    VKR_CHECK(backend.destroy_shader_module({fs.shader_module}).ok);
    VKR_CHECK(backend.destroy_image_view({view.image_view}).ok);
    VKR_CHECK(backend.destroy_image({image.image}).ok);
    VKR_CHECK(backend.free_memory({image_memory.memory}).ok);
    for (const BoundBuffer buffer : {vertex, index, indirect, readback}) {
        VKR_CHECK(backend.destroy_buffer({buffer.buffer}).ok);
        VKR_CHECK(backend.free_memory({buffer.memory}).ok);
    }
    VKR_CHECK(backend.destroy_device({cd.device}).ok);
    VKR_CHECK(backend.destroy_instance({ci.instance}).ok);
    return vkr::test::finish("integration_real_indirect");
}
