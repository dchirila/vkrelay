// Compute pipelines + dispatch: in-process real-backend test on the host GPU.
//
// Constructs RealVulkanBackend directly and runs the whole compute chain the relay carries:
// queue-flags honesty (the selected family must advertise COMPUTE in DeviceCaps), compute
// pipeline create (+ the reject set), storage/uniform descriptors + push constants, bind at
// the COMPUTE point, dispatch, the shader-write -> transfer-read BUFFER barrier (never
// queue-order luck), vkCmdCopyBuffer into a host-visible readback buffer, fence,
// then read_memory_ranges and assert all 4096 results BYTE-EXACTLY
// (out[i] == in[i]*scale + bias + i, compute_spv.h). Also pins the per-bind-point kind check
// on the real validator (a compute pipeline bound at the GRAPHICS point rejects) and the
// in-flight negative set (dispatch without a pipeline; indirect misuse).
//
// Windows-only (WIN32 + Vulkan SDK); skips gracefully with no usable device. Validation-clean
// gate: a manual run with VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation.
#include "windows/worker/real_vulkan_backend.hpp"

#include "common/vkrpc/vulkan_session.hpp"
#include "tests/compute_spv.h"
#include "tests/test_assert.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include <vulkan/vulkan.h>

using namespace vkr;

namespace {
constexpr std::uint32_t kElems = 4096;
constexpr std::uint32_t kScale = 3;
constexpr std::uint32_t kBias = 7;
} // namespace

int main() {
    worker::RealVulkanBackend backend("", "", false);

    const vkrpc::CreateInstanceResponse ci = backend.create_instance({});
    if (!ci.ok) {
        std::fprintf(stderr, "integration_real_compute: skipped (no instance: %s)\n",
                     ci.reason.c_str());
        return vkr::test::finish("integration_real_compute");
    }
    vkrpc::EnumeratePhysicalDevicesRequest er;
    er.instance = ci.instance;
    const vkrpc::EnumeratePhysicalDevicesResponse en = backend.enumerate_physical_devices(er);
    if (!en.ok || en.devices.empty()) {
        std::fprintf(stderr, "integration_real_compute: skipped (no physical device)\n");
        return vkr::test::finish("integration_real_compute");
    }
    // Queue-flags honesty: the selected graphics family must advertise COMPUTE (every desktop
    // GPU's graphics family does; a device where it does not would skip, not lie).
    const std::uint64_t qflags = en.devices.front().caps.queue_flags;
    if ((qflags & VK_QUEUE_COMPUTE_BIT) == 0) {
        std::fprintf(stderr, "integration_real_compute: skipped (no COMPUTE on the family)\n");
        return vkr::test::finish("integration_real_compute");
    }
    VKR_CHECK((qflags & VK_QUEUE_GRAPHICS_BIT) != 0);

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

    vkrpc::GetPhysicalDeviceMemoryPropertiesRequest mpr;
    mpr.physical_device = phys;
    const auto mp = backend.get_physical_device_memory_properties(mpr);
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

    const std::uint64_t bytes = kElems * sizeof(std::uint32_t);
    auto make_bound_buffer = [&](std::uint64_t usage, const void* data, std::uint64_t nbytes,
                                 std::uint64_t& out_buffer, std::uint64_t& out_memory) {
        vkrpc::CreateBufferRequest cbr;
        cbr.device = cd.device;
        cbr.size = nbytes;
        cbr.usage = usage;
        cbr.sharing_mode = VK_SHARING_MODE_EXCLUSIVE;
        const vkrpc::CreateBufferResponse buf = backend.create_buffer(cbr);
        VKR_CHECK(buf.ok && buf.buffer != 0 && buf.mem_size >= nbytes);
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
        out_buffer = buf.buffer;
        out_memory = mem.memory;
    };

    std::vector<std::uint32_t> input(kElems);
    for (std::uint32_t i = 0; i < kElems; ++i) {
        input[i] = i * 5 + 1;
    }
    const std::uint32_t scale = kScale;
    std::uint64_t ssbo_in = 0, ssbo_in_mem = 0, ssbo_out = 0, ssbo_out_mem = 0, ubo = 0,
                  ubo_mem = 0, readback = 0, readback_mem = 0, indirect = 0, indirect_mem = 0;
    make_bound_buffer(vkrpc::kBufferUsageStorageBuffer, input.data(), bytes, ssbo_in, ssbo_in_mem);
    make_bound_buffer(vkrpc::kBufferUsageStorageBuffer | vkrpc::kBufferUsageTransferSrc, nullptr,
                      bytes, ssbo_out, ssbo_out_mem);
    make_bound_buffer(vkrpc::kBufferUsageUniformBuffer, &scale, sizeof(scale), ubo, ubo_mem);
    make_bound_buffer(vkrpc::kBufferUsageTransferDst, nullptr, bytes, readback, readback_mem);
    const std::uint32_t groups[3] = {kElems / 64, 1, 1};
    make_bound_buffer(vkrpc::kBufferUsageIndirectBuffer, groups, sizeof(groups), indirect,
                      indirect_mem);

    // Shader module + set layout {0,1: STORAGE; 2: UNIFORM} + pipeline layout (+ a push range).
    vkrpc::CreateShaderModuleRequest smr;
    smr.device = cd.device;
    smr.code.assign(reinterpret_cast<const char*>(kComputeSpv), sizeof(kComputeSpv));
    smr.code_size = smr.code.size();
    const vkrpc::CreateShaderModuleResponse sm = backend.create_shader_module(smr);
    VKR_CHECK(sm.ok && sm.shader_module != 0);
    vkrpc::CreateDescriptorSetLayoutRequest dslr;
    dslr.device = cd.device;
    for (int b = 0; b < 3; ++b) {
        vkrpc::DescriptorSetLayoutBindingDesc d;
        d.binding = b;
        d.descriptor_type =
            b == 2 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        d.descriptor_count = 1;
        d.stage_flags = VK_SHADER_STAGE_COMPUTE_BIT;
        dslr.bindings.push_back(d);
    }
    const auto dsl = backend.create_descriptor_set_layout(dslr);
    VKR_CHECK(dsl.ok);
    vkrpc::CreatePipelineLayoutRequest plr;
    plr.device = cd.device;
    plr.set_layout_count = 1;
    plr.set_layouts = {dsl.set_layout};
    plr.push_constant_range_count = 1;
    vkrpc::PushConstantRange pr;
    pr.stage_flags = VK_SHADER_STAGE_COMPUTE_BIT;
    pr.offset = 0;
    pr.size = sizeof(std::uint32_t);
    plr.push_constant_ranges.push_back(pr);
    const auto pl = backend.create_pipeline_layout(plr);
    VKR_CHECK(pl.ok);

    // The compute pipeline + the reject set.
    vkrpc::CreateComputePipelinesRequest cpr;
    cpr.device = cd.device;
    cpr.pipeline_cache = 0;
    cpr.layout = pl.pipeline_layout;
    cpr.shader_module = sm.shader_module;
    cpr.entry_point = "main";
    const auto cp = backend.create_compute_pipelines(cpr);
    VKR_CHECK(cp.ok && cp.pipeline != 0);
    {
        auto bad = cpr;
        bad.pipeline_cache = 5;
        VKR_CHECK(!backend.create_compute_pipelines(bad).ok);
        bad = cpr;
        bad.shader_module = 0xDEAD;
        VKR_CHECK(!backend.create_compute_pipelines(bad).ok);
        bad = cpr;
        bad.entry_point.clear();
        VKR_CHECK(!backend.create_compute_pipelines(bad).ok);
    }

    // Descriptor pool/set/writes.
    vkrpc::CreateDescriptorPoolRequest dpr;
    dpr.device = cd.device;
    dpr.max_sets = 1;
    dpr.pool_sizes.push_back({VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2});
    dpr.pool_sizes.push_back({VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1});
    const auto dpool = backend.create_descriptor_pool(dpr);
    VKR_CHECK(dpool.ok);
    vkrpc::AllocateDescriptorSetsRequest adr;
    adr.device = cd.device;
    adr.pool = dpool.pool;
    adr.set_layouts = {dsl.set_layout};
    const auto ds = backend.allocate_descriptor_sets(adr);
    VKR_CHECK(ds.ok && !ds.descriptor_sets.empty());
    const std::uint64_t dset = ds.descriptor_sets.front();
    {
        vkrpc::UpdateDescriptorSetsRequest ur;
        ur.device = cd.device;
        const std::uint64_t bufs[3] = {ssbo_in, ssbo_out, ubo};
        const std::uint64_t lens[3] = {bytes, bytes, sizeof(std::uint32_t)};
        for (int b = 0; b < 3; ++b) {
            vkrpc::WriteDescriptorSetDesc w;
            w.dst_set = dset;
            w.dst_binding = b;
            w.dst_array_element = 0;
            w.descriptor_type =
                b == 2 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            w.descriptor_count = 1;
            w.buffer_infos.push_back({bufs[b], 0, lens[b]});
            ur.writes.push_back(w);
        }
        VKR_CHECK(backend.update_descriptor_sets(ur).ok);
    }

    // Command buffer: bind(COMPUTE) + sets(COMPUTE) + push + dispatch + barrier + copy-out.
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
    const std::uint64_t cmd = cbs.command_buffers.front();

    auto build_recording = [&](bool use_indirect) {
        vkrpc::RecordCommandBufferRequest rec;
        rec.command_buffer = cmd;
        rec.one_time_submit = true;
        vkrpc::RecordedCommand bp;
        bp.kind = "bind_pipeline";
        bp.pipeline = cp.pipeline;
        bp.args_i64 = {1}; // COMPUTE
        vkrpc::RecordedCommand bs;
        bs.kind = "bind_descriptor_sets";
        bs.desc_layout = pl.pipeline_layout;
        bs.first_set = 0;
        bs.descriptor_sets = {dset};
        bs.args_i64 = {1}; // COMPUTE point
        vkrpc::RecordedCommand pc;
        pc.kind = "push_constants";
        pc.desc_layout = pl.pipeline_layout;
        pc.args_i64 = {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(std::uint32_t)};
        const std::uint32_t bias = kBias;
        pc.args_blob.assign(reinterpret_cast<const char*>(&bias), sizeof(bias));
        vkrpc::RecordedCommand d;
        if (use_indirect) {
            d.kind = "dispatch_indirect";
            d.src_buffer = indirect;
            d.args_u64 = {0};
        } else {
            d.kind = "dispatch";
            d.args_u64 = {kElems / 64, 1, 1};
        }
        vkrpc::RecordedCommand bar;
        bar.kind = "buffer_memory_barrier";
        bar.src_buffer = ssbo_out;
        bar.src_stage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        bar.dst_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        bar.src_access = VK_ACCESS_SHADER_WRITE_BIT;
        bar.dst_access = VK_ACCESS_TRANSFER_READ_BIT;
        bar.args_u64 = {0, ~0ull}; // WHOLE_SIZE
        vkrpc::RecordedCommand cpy;
        cpy.kind = "copy_buffer";
        cpy.args_u64 = {ssbo_out, readback, 0, 0, bytes};
        rec.commands = {bp, bs, pc, d, bar, cpy};
        return rec;
    };

    // Negatives on the REAL validator first: kind/bind-point mismatch + no-pipeline dispatch +
    // indirect misuse.
    {
        vkrpc::RecordCommandBufferRequest r;
        r.command_buffer = cmd;
        vkrpc::RecordedCommand wrong;
        wrong.kind = "bind_pipeline";
        wrong.pipeline = cp.pipeline;
        wrong.args_i64 = {0}; // a compute pipeline at the GRAPHICS point
        r.commands = {wrong};
        VKR_CHECK(!backend.record_command_buffer(r).ok);
        vkrpc::RecordedCommand d;
        d.kind = "dispatch";
        d.args_u64 = {1, 1, 1};
        r.commands = {d}; // no pipeline bound
        VKR_CHECK(!backend.record_command_buffer(r).ok);
        auto rec = build_recording(true);
        rec.commands[3].args_u64 = {2}; // misaligned indirect offset
        VKR_CHECK(!backend.record_command_buffer(rec).ok);
    }

    vkrpc::CreateFenceRequest cfr;
    cfr.device = cd.device;
    const auto fence = backend.create_fence(cfr);
    VKR_CHECK(fence.ok);

    auto run_and_check = [&](bool use_indirect, const char* label) {
        VKR_CHECK(backend.record_command_buffer(build_recording(use_indirect)).ok);
        vkrpc::QueueSubmitRequest sub;
        sub.queue = q.queue;
        sub.command_buffers = {cmd};
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
        vkrpc::ResetFencesRequest rf;
        rf.fences = {fence.fence};
        VKR_CHECK(backend.reset_fences(rf).ok);

        vkrpc::ReadMemoryRangesRequest rr;
        vkrpc::MemoryUpload rd;
        rd.memory = readback_mem;
        rd.ranges.push_back(vkrpc::MemoryUploadRange{0, bytes});
        rr.reads.push_back(rd);
        const auto res = backend.read_memory_ranges(rr);
        VKR_CHECK(res.ok);
        VKR_CHECK_EQ(res.payload.size(), static_cast<std::size_t>(bytes));
        const auto* out = reinterpret_cast<const std::uint32_t*>(res.payload.data());
        std::uint32_t bad = 0;
        for (std::uint32_t i = 0; i < kElems; ++i) {
            const std::uint32_t expect = input[i] * kScale + kBias + i;
            if (out[i] != expect) {
                ++bad;
            }
        }
        VKR_CHECK_EQ(bad, 0u);
        std::fprintf(stderr, "integration_real_compute: %s -- %u results exact\n", label, kElems);
    };
    run_and_check(false, "dispatch");
    run_and_check(true, "dispatch_indirect");

    return vkr::test::finish("integration_real_compute");
}
