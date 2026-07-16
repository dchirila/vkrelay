// Depth/stencil copy_buffer_to_image (the zink depth-texture staging path): in-process real-backend
// round-trip on the host GPU. Proves the widened copy admits a DEPTH-aspect region AND that the
// data survives bit-exactly, not just that the submit succeeds.
//
// The chain: fill a TRANSFER_SRC host-visible staging buffer with 16 known D32_SFLOAT depth values
// -> vkCmdCopyBufferToImage (DEPTH aspect) into a 4x4 D32_SFLOAT DEVICE_LOCAL OPTIMAL image
// (usage TRANSFER_DST|TRANSFER_SRC), with the UNDEFINED->TRANSFER_DST barrier -> a
// TRANSFER_DST->TRANSFER_SRC barrier -> vkCmdCopyImageToBuffer (DEPTH aspect) back into a
// TRANSFER_DST host-visible readback buffer -> fence -> read_memory_ranges and byte-compare. D32
// is chosen because its depth aspect is a straight IEEE-754 float, so a transfer round-trip is
// bit-exact (no format packing to reason about -- that stays the host driver's job).
//
// Windows-only (WIN32 + Vulkan SDK). Skips gracefully (still passes) with no usable device, or if
// the host does not advertise TRANSFER_SRC|TRANSFER_DST on D32_SFLOAT. Validation-clean gate: a
// manual run with VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation.
#include "windows/worker/real_vulkan_backend.hpp"

#include "common/vkrpc/vulkan_session.hpp"
#include "tests/test_assert.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include <vulkan/vulkan.h>

using namespace vkr;

namespace {
// Picks a memory type in `type_bits` matching device-local vs host-coherent; -1 if none.
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
        std::fprintf(stderr, "integration_real_depth_upload: skipped (no instance: %s)\n",
                     ci.reason.c_str());
        return vkr::test::finish("integration_real_depth_upload");
    }
    vkrpc::EnumeratePhysicalDevicesRequest er;
    er.instance = ci.instance;
    const vkrpc::EnumeratePhysicalDevicesResponse en = backend.enumerate_physical_devices(er);
    if (!en.ok || en.devices.empty()) {
        std::fprintf(stderr, "integration_real_depth_upload: skipped (no physical device)\n");
        return vkr::test::finish("integration_real_depth_upload");
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

    // Honest gate: only proceed if the host advertises D32_SFLOAT as both a transfer source and
    // destination (an OPTIMAL depth image). Every desktop GPU does; a driver that does not SKIPS.
    vkrpc::GetPhysicalDeviceFormatPropertiesRequest fpr;
    fpr.physical_device = phys;
    fpr.format = vkrpc::kFormatD32Sfloat;
    const vkrpc::GetPhysicalDeviceFormatPropertiesResponse fp =
        backend.get_physical_device_format_properties(fpr);
    VKR_CHECK(fp.ok);
    const std::uint64_t need = vkrpc::kFormatFeatureTransferSrc | vkrpc::kFormatFeatureTransferDst;
    if ((fp.optimal_tiling_features & need) != need) {
        std::fprintf(stderr,
                     "integration_real_depth_upload: skipped (host lacks TRANSFER_SRC|DST on "
                     "D32_SFLOAT)\n");
        return vkr::test::finish("integration_real_depth_upload");
    }

    vkrpc::GetPhysicalDeviceMemoryPropertiesRequest mpr;
    mpr.physical_device = phys;
    const auto mp = backend.get_physical_device_memory_properties(mpr);
    VKR_CHECK(mp.ok && !mp.types.empty());

    // 16 known depth values -- i/16 for i in [0,16): dyadic, exactly representable in float, so the
    // transfer round-trip must be bit-exact.
    constexpr int kW = 4, kH = 4, kN = kW * kH;
    std::vector<float> input(kN);
    for (int i = 0; i < kN; ++i) {
        input[i] = static_cast<float>(i) / 16.0f;
    }
    const std::uint64_t nbytes = static_cast<std::uint64_t>(kN) * sizeof(float);

    auto make_bound_buffer = [&](std::uint64_t usage, const void* data, std::uint64_t& out_buf,
                                 std::uint64_t& out_mem) {
        vkrpc::CreateBufferRequest cbr;
        cbr.device = cd.device;
        cbr.size = nbytes;
        cbr.usage = usage;
        cbr.sharing_mode = VK_SHARING_MODE_EXCLUSIVE;
        const vkrpc::CreateBufferResponse buf = backend.create_buffer(cbr);
        VKR_CHECK(buf.ok && buf.buffer != 0 && buf.mem_size >= nbytes);
        const long long t = pick_type(mp, buf.mem_type_bits, /*want_device_local=*/false);
        VKR_CHECK(t >= 0);
        vkrpc::AllocateMemoryRequest amr;
        amr.device = cd.device;
        amr.allocation_size = buf.mem_size;
        amr.memory_type_index = static_cast<int>(t);
        const vkrpc::AllocateMemoryResponse mem = backend.allocate_memory(amr);
        VKR_CHECK(mem.ok && mem.memory != 0);
        VKR_CHECK(backend.bind_buffer_memory({buf.buffer, mem.memory, 0}).ok);
        if (data != nullptr) {
            vkrpc::WriteMemoryRangesRequest wmr;
            vkrpc::MemoryUpload up;
            up.memory = mem.memory;
            up.ranges.push_back(vkrpc::MemoryUploadRange{0, nbytes});
            wmr.uploads.push_back(up);
            wmr.payload.assign(reinterpret_cast<const char*>(data),
                               static_cast<std::size_t>(nbytes));
            VKR_CHECK(backend.write_memory_ranges(wmr).ok);
        }
        out_buf = buf.buffer;
        out_mem = mem.memory;
    };

    std::uint64_t staging = 0, staging_mem = 0, readback = 0, readback_mem = 0;
    make_bound_buffer(vkrpc::kBufferUsageTransferSrc, input.data(), staging, staging_mem);
    make_bound_buffer(static_cast<std::uint64_t>(vkrpc::kBufferUsageTransferDst), nullptr, readback,
                      readback_mem);

    // The depth image: D32_SFLOAT, OPTIMAL, TRANSFER_DST|TRANSFER_SRC, DEVICE_LOCAL. The round-trip
    // needs TRANSFER_SRC (the readback); the real create_image forwards usage verbatim.
    vkrpc::CreateImageRequest dir;
    dir.device = cd.device;
    dir.image_type = vkrpc::kImageType2D;
    dir.format = vkrpc::kFormatD32Sfloat;
    dir.width = kW;
    dir.height = kH;
    dir.depth = 1;
    dir.mip_levels = 1;
    dir.array_layers = 1;
    dir.samples = 1;
    dir.tiling = vkrpc::kImageTilingOptimal;
    dir.usage = static_cast<std::uint64_t>(vkrpc::kImageUsageTransferDst) |
                static_cast<std::uint64_t>(vkrpc::kImageUsageTransferSrc);
    dir.sharing_mode = 0;
    dir.initial_layout = 0;
    const vkrpc::CreateImageResponse dimg = backend.create_image(dir);
    VKR_CHECK(dimg.ok && dimg.image != 0);
    const long long img_type = pick_type(mp, dimg.mem_type_bits, /*want_device_local=*/true);
    VKR_CHECK(img_type >= 0);
    vkrpc::AllocateMemoryRequest iam;
    iam.device = cd.device;
    iam.allocation_size = dimg.mem_size;
    iam.memory_type_index = static_cast<int>(img_type);
    const vkrpc::AllocateMemoryResponse imem = backend.allocate_memory(iam);
    VKR_CHECK(imem.ok && imem.memory != 0);
    VKR_CHECK(backend.bind_image_memory({dimg.image, imem.memory, 0}).ok);

    // The DEPTH-aspect upload + readback recording.
    auto depth_barrier = [&](int old_layout, int new_layout, long long src_stage,
                             long long dst_stage, long long src_access, long long dst_access) {
        vkrpc::RecordedCommand c;
        c.kind = "pipeline_barrier";
        c.image = dimg.image;
        c.old_layout = old_layout;
        c.new_layout = new_layout;
        c.aspect = vkrpc::kImageAspectDepth;
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
    vkrpc::RecordedCommand up;
    up.kind = "copy_buffer_to_image";
    up.src_buffer = staging;
    up.image = dimg.image;
    up.args_i64 = {VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   1,
                   0,
                   0,
                   0,
                   vkrpc::kImageAspectDepth,
                   0,
                   0,
                   1,
                   0,
                   0,
                   0,
                   kW,
                   kH,
                   1};
    vkrpc::RecordedCommand down;
    down.kind = "copy_image_to_buffer";
    down.args_u64 = {dimg.image, readback};
    down.args_i64 = {VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     1,
                     0,
                     0,
                     0,
                     vkrpc::kImageAspectDepth,
                     0,
                     0,
                     1,
                     0,
                     0,
                     0,
                     kW,
                     kH,
                     1};

    vkrpc::CreateCommandPoolRequest cpq;
    cpq.device = cd.device;
    cpq.queue_family_index = cd.queue_family_index;
    const auto cpool = backend.create_command_pool(cpq);
    VKR_CHECK(cpool.ok);
    vkrpc::AllocateCommandBuffersRequest acb;
    acb.command_pool = cpool.command_pool;
    acb.count = 1;
    const auto cbs = backend.allocate_command_buffers(acb);
    VKR_CHECK(cbs.ok && !cbs.command_buffers.empty());

    vkrpc::RecordCommandBufferRequest rec;
    rec.command_buffer = cbs.command_buffers.front();
    rec.one_time_submit = true;
    rec.commands = {depth_barrier(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                  VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                  0, VK_ACCESS_TRANSFER_WRITE_BIT),
                    up,
                    depth_barrier(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                  VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT),
                    down};
    VKR_CHECK(backend.record_command_buffer(rec).ok);

    vkrpc::CreateFenceRequest cfr;
    cfr.device = cd.device;
    const auto fence = backend.create_fence(cfr);
    VKR_CHECK(fence.ok);
    vkrpc::QueueSubmitRequest sub;
    sub.queue = q.queue;
    sub.command_buffers = {rec.command_buffer};
    sub.fence = fence.fence;
    const auto s = backend.queue_submit(sub);
    VKR_CHECK(s.ok);
    VKR_CHECK_EQ(s.result, vkrpc::kVkSuccess);
    vkrpc::WaitForFencesRequest wf;
    wf.fences = {fence.fence};
    wf.wait_all = true;
    wf.timeout = ~0ull;
    const auto w = backend.wait_for_fences(wf);
    VKR_CHECK(w.ok);
    VKR_CHECK_EQ(w.result, vkrpc::kVkSuccess);

    // Read back and compare bit-exactly.
    vkrpc::ReadMemoryRangesRequest rr;
    vkrpc::MemoryUpload rd;
    rd.memory = readback_mem;
    rd.ranges.push_back(vkrpc::MemoryUploadRange{0, nbytes});
    rr.reads.push_back(rd);
    const auto res = backend.read_memory_ranges(rr);
    VKR_CHECK(res.ok);
    VKR_CHECK_EQ(res.payload.size(), static_cast<std::size_t>(nbytes));
    VKR_CHECK(std::memcmp(res.payload.data(), input.data(), static_cast<std::size_t>(nbytes)) == 0);
    std::fprintf(stderr,
                 "integration_real_depth_upload: %d D32_SFLOAT depth texels round-tripped "
                 "bit-exactly\n",
                 kN);

    return vkr::test::finish("integration_real_depth_upload");
}
