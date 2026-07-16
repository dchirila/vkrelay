// Vulkan 1.2 bufferDeviceAddress: in-process real-backend test on the host GPU.
//
// Constructs RealVulkanBackend directly and runs the whole raw-GPU-pointer chain the relay
// carries: create_device with the bufferDeviceAddress FEATURE (the request bit + the rebuilt
// enabled-feature chain, so the host device really enables it), the DEVICE_ADDRESS allocate flag
// (serialized VkMemoryAllocateFlagsInfo), the VUID-03339 bind gate (a SHADER_DEVICE_ADDRESS
// buffer refuses non-DEVICE_ADDRESS memory BEFORE the driver call), get_buffer_device_address
// (real host VA: non-zero + stable, and the fail-closed negatives -- no feature / no usage /
// unbound), and the load-bearing proof: a DESCRIPTORLESS buffer-reference compute shader
// (tests/bda_spv.h) that reads AND writes its buffer only through the raw address handed in via
// push constants -- dispatch + barrier + copy-out + read_memory_ranges, all 4096 results
// byte-exact (data[i] == in[i]*scale + bias + i). A translated/faked address cannot pass.
//
// Windows-only (WIN32 + Vulkan SDK); skips gracefully with no usable device. Validation-clean
// gate: a manual run with VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation.
#include "windows/worker/real_vulkan_backend.hpp"

#include "common/vkrpc/vulkan_session.hpp"
#include "tests/bda_spv.h"
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

// The push-constant block bda_spv.h declares: the raw device address + scale + bias (std430).
struct PushBlock {
    std::uint64_t addr;
    std::uint32_t scale;
    std::uint32_t bias;
};
static_assert(sizeof(PushBlock) == 16, "push block must match the shader's std430 layout");
} // namespace

int main() {
    worker::RealVulkanBackend backend("", "", false);

    const vkrpc::CreateInstanceResponse ci = backend.create_instance({});
    if (!ci.ok) {
        std::fprintf(stderr, "integration_real_bda: skipped (no instance: %s)\n",
                     ci.reason.c_str());
        return vkr::test::finish("integration_real_bda");
    }
    vkrpc::EnumeratePhysicalDevicesRequest er;
    er.instance = ci.instance;
    const vkrpc::EnumeratePhysicalDevicesResponse en = backend.enumerate_physical_devices(er);
    if (!en.ok || en.devices.empty()) {
        std::fprintf(stderr, "integration_real_bda: skipped (no physical device)\n");
        return vkr::test::finish("integration_real_bda");
    }
    if ((en.devices.front().caps.queue_flags & VK_QUEUE_COMPUTE_BIT) == 0) {
        std::fprintf(stderr, "integration_real_bda: skipped (no COMPUTE on the family)\n");
        return vkr::test::finish("integration_real_bda");
    }
    const std::uint64_t phys = en.devices.front().handle;

    // --- A device WITHOUT the feature first: the DEVICE_ADDRESS allocate flag and
    // get_buffer_device_address must both fail closed. ---
    {
        vkrpc::CreateDeviceRequest cdr;
        cdr.instance = ci.instance;
        cdr.physical_device = phys;
        const vkrpc::CreateDeviceResponse cd = backend.create_device(cdr);
        VKR_CHECK(cd.ok);
        vkrpc::AllocateMemoryRequest amr;
        amr.device = cd.device;
        amr.allocation_size = 4096;
        amr.memory_type_index = 0;
        amr.allocate_flags = vkrpc::kMemoryAllocateDeviceAddressBit;
        VKR_CHECK(!backend.allocate_memory(amr).ok); // feature not enabled -> reject
        vkrpc::GetBufferDeviceAddressRequest gar;
        gar.device = cd.device;
        gar.buffer = 0xDEAD;
        VKR_CHECK(!backend.get_buffer_device_address(gar).ok);
        vkrpc::HandleRequest dd;
        dd.handle = cd.device;
        VKR_CHECK(backend.destroy_device(dd).ok);
    }

    // --- The scalar/chain normalization: the worker must reject a mismatch in
    // EITHER direction, and any captureReplay/multiDevice request, BEFORE a host device exists. ---
    VkPhysicalDeviceBufferDeviceAddressFeatures enable_bda{};
    enable_bda.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES;
    enable_bda.bufferDeviceAddress = VK_TRUE;
    vkrpc::CapabilityChainEntry fe;
    fe.s_type = static_cast<std::uint32_t>(enable_bda.sType);
    fe.size = sizeof(enable_bda);
    fe.blob.assign(reinterpret_cast<const char*>(&enable_bda), sizeof(enable_bda));
    {
        // Scalar=1 with NO chain: a hostile scalar/chain mismatch. Without this rejection, a host
        // device lacking the feature could otherwise serve BDA operations.
        vkrpc::CreateDeviceRequest bad;
        bad.instance = ci.instance;
        bad.physical_device = phys;
        bad.buffer_device_address_feature_enabled = 1;
        VKR_CHECK(!backend.create_device(bad).ok);
        // Chain enables BDA but scalar=0: the reverse mismatch.
        bad.buffer_device_address_feature_enabled = 0;
        bad.enabled_feature_chain.push_back(fe);
        VKR_CHECK(!backend.create_device(bad).ok);
        // captureReplay requested (unwired, reported FALSE): rejected by name even when the
        // scalar and bufferDeviceAddress agree.
        VkPhysicalDeviceBufferDeviceAddressFeatures cr = enable_bda;
        cr.bufferDeviceAddressCaptureReplay = VK_TRUE;
        vkrpc::CapabilityChainEntry cre;
        cre.s_type = fe.s_type;
        cre.size = sizeof(cr);
        cre.blob.assign(reinterpret_cast<const char*>(&cr), sizeof(cr));
        bad.enabled_feature_chain = {cre};
        bad.buffer_device_address_feature_enabled = 1;
        VKR_CHECK(!backend.create_device(bad).ok);
        // An UNDERSIZED known struct (>= the pNext header, < the real sizeof) must reject
        // regardless of the scalar -- it would otherwise reach the host vkCreateDevice as a
        // garbage-tailed struct (exact-size gate).
        vkrpc::CapabilityChainEntry runt = fe;
        runt.size = 24; // header (16) < 24 < sizeof(VkPhysicalDeviceBufferDeviceAddressFeatures)
        runt.blob.resize(24);
        bad.enabled_feature_chain = {runt};
        bad.buffer_device_address_feature_enabled = 0;
        VKR_CHECK(!backend.create_device(bad).ok);
        bad.buffer_device_address_feature_enabled = 1;
        VKR_CHECK(!backend.create_device(bad).ok);
    }

    // --- The BDA device: the feature bit + the rebuilt enabled-feature chain (the host device
    // must REALLY enable bufferDeviceAddress or the shader's PhysicalStorageBuffer64 access is
    // invalid). ---
    vkrpc::CreateDeviceRequest cdr;
    cdr.instance = ci.instance;
    cdr.physical_device = phys;
    cdr.buffer_device_address_feature_enabled = 1;
    cdr.enabled_feature_chain.push_back(fe);
    const vkrpc::CreateDeviceResponse cd = backend.create_device(cdr);
    if (!cd.ok) {
        // A host without bufferDeviceAddress support is a skip, not a failure (the worker's
        // fail-closed create is exactly the contract).
        std::fprintf(stderr, "integration_real_bda: skipped (host lacks bufferDeviceAddress: %s)\n",
                     cd.reason.c_str());
        return vkr::test::finish("integration_real_bda");
    }
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

    // --- Allocate-flags negatives on the BDA device: only DEVICE_ADDRESS is admitted. ---
    {
        vkrpc::AllocateMemoryRequest amr;
        amr.device = cd.device;
        amr.allocation_size = 4096;
        amr.memory_type_index = coherent_type;
        amr.allocate_flags = 0x1; // DEVICE_MASK -> reject by name
        VKR_CHECK(!backend.allocate_memory(amr).ok);
        amr.allocate_flags = 0x4; // DEVICE_ADDRESS_CAPTURE_REPLAY -> reject by name
        VKR_CHECK(!backend.allocate_memory(amr).ok);
    }

    const std::uint64_t bytes = kElems * sizeof(std::uint32_t);

    // The BDA buffer: SHADER_DEVICE_ADDRESS usage, DEVICE_ADDRESS memory.
    vkrpc::CreateBufferRequest cbr;
    cbr.device = cd.device;
    cbr.size = bytes;
    cbr.usage = vkrpc::kBufferUsageStorageBuffer | vkrpc::kBufferUsageShaderDeviceAddress |
                vkrpc::kBufferUsageTransferSrc;
    cbr.sharing_mode = VK_SHARING_MODE_EXCLUSIVE;
    const vkrpc::CreateBufferResponse bda_buf = backend.create_buffer(cbr);
    VKR_CHECK(bda_buf.ok && bda_buf.buffer != 0);

    // VUID 03339: binding it to NON-DEVICE_ADDRESS memory must fail closed BEFORE the driver.
    {
        vkrpc::AllocateMemoryRequest amr;
        amr.device = cd.device;
        amr.allocation_size = bda_buf.mem_size;
        amr.memory_type_index = coherent_type;
        const auto plain = backend.allocate_memory(amr);
        VKR_CHECK(plain.ok);
        vkrpc::BindBufferMemoryRequest bbr;
        bbr.buffer = bda_buf.buffer;
        bbr.memory = plain.memory;
        bbr.memory_offset = 0;
        VKR_CHECK(!backend.bind_buffer_memory(bbr).ok);
        // Unbound (the failed bind must not have stuck): the address query still rejects.
        vkrpc::GetBufferDeviceAddressRequest gar;
        gar.device = cd.device;
        gar.buffer = bda_buf.buffer;
        VKR_CHECK(!backend.get_buffer_device_address(gar).ok);
        vkrpc::HandleRequest fm;
        fm.handle = plain.memory;
        VKR_CHECK(backend.free_memory(fm).ok);
    }

    // The real bind: DEVICE_ADDRESS memory.
    vkrpc::AllocateMemoryRequest amr;
    amr.device = cd.device;
    amr.allocation_size = bda_buf.mem_size;
    amr.memory_type_index = coherent_type;
    amr.allocate_flags = vkrpc::kMemoryAllocateDeviceAddressBit;
    const auto bda_mem = backend.allocate_memory(amr);
    VKR_CHECK(bda_mem.ok && bda_mem.memory != 0);
    {
        vkrpc::BindBufferMemoryRequest bbr;
        bbr.buffer = bda_buf.buffer;
        bbr.memory = bda_mem.memory;
        bbr.memory_offset = 0;
        VKR_CHECK(backend.bind_buffer_memory(bbr).ok);
    }

    // --- The address: non-zero, stable, and the usage negative. ---
    vkrpc::GetBufferDeviceAddressRequest gar;
    gar.device = cd.device;
    gar.buffer = bda_buf.buffer;
    const auto addr1 = backend.get_buffer_device_address(gar);
    VKR_CHECK(addr1.ok && addr1.device_address != 0);
    const auto addr2 = backend.get_buffer_device_address(gar);
    VKR_CHECK(addr2.ok);
    VKR_CHECK_EQ(addr2.device_address, addr1.device_address);
    {
        // A buffer WITHOUT SHADER_DEVICE_ADDRESS usage: the query rejects even when bound.
        vkrpc::CreateBufferRequest plain;
        plain.device = cd.device;
        plain.size = 256;
        plain.usage = vkrpc::kBufferUsageStorageBuffer;
        plain.sharing_mode = VK_SHARING_MODE_EXCLUSIVE;
        const auto pb = backend.create_buffer(plain);
        VKR_CHECK(pb.ok);
        vkrpc::AllocateMemoryRequest pam;
        pam.device = cd.device;
        pam.allocation_size = pb.mem_size;
        pam.memory_type_index = coherent_type;
        const auto pm = backend.allocate_memory(pam);
        VKR_CHECK(pm.ok);
        vkrpc::BindBufferMemoryRequest pbb;
        pbb.buffer = pb.buffer;
        pbb.memory = pm.memory;
        pbb.memory_offset = 0;
        VKR_CHECK(backend.bind_buffer_memory(pbb).ok);
        vkrpc::GetBufferDeviceAddressRequest bad;
        bad.device = cd.device;
        bad.buffer = pb.buffer;
        VKR_CHECK(!backend.get_buffer_device_address(bad).ok);
        vkrpc::HandleRequest db;
        db.handle = pb.buffer;
        VKR_CHECK(backend.destroy_buffer(db).ok);
        vkrpc::HandleRequest fm;
        fm.handle = pm.memory;
        VKR_CHECK(backend.free_memory(fm).ok);
    }

    // --- Upload the input, then the descriptorless compute through the raw address. ---
    std::vector<std::uint32_t> input(kElems);
    for (std::uint32_t i = 0; i < kElems; ++i) {
        input[i] = i * 5 + 1;
    }
    {
        vkrpc::WriteMemoryRangesRequest wmr;
        vkrpc::MemoryUpload up;
        up.memory = bda_mem.memory;
        up.ranges.push_back(vkrpc::MemoryUploadRange{0, bytes});
        wmr.uploads.push_back(up);
        wmr.payload.assign(reinterpret_cast<const char*>(input.data()),
                           static_cast<std::size_t>(bytes));
        VKR_CHECK(backend.write_memory_ranges(wmr).ok);
    }
    // The readback buffer (plain TRANSFER_DST).
    std::uint64_t readback = 0, readback_mem = 0;
    {
        vkrpc::CreateBufferRequest rb;
        rb.device = cd.device;
        rb.size = bytes;
        rb.usage = vkrpc::kBufferUsageTransferDst;
        rb.sharing_mode = VK_SHARING_MODE_EXCLUSIVE;
        const auto b = backend.create_buffer(rb);
        VKR_CHECK(b.ok);
        vkrpc::AllocateMemoryRequest ram;
        ram.device = cd.device;
        ram.allocation_size = b.mem_size;
        ram.memory_type_index = coherent_type;
        const auto m = backend.allocate_memory(ram);
        VKR_CHECK(m.ok);
        vkrpc::BindBufferMemoryRequest bb;
        bb.buffer = b.buffer;
        bb.memory = m.memory;
        bb.memory_offset = 0;
        VKR_CHECK(backend.bind_buffer_memory(bb).ok);
        readback = b.buffer;
        readback_mem = m.memory;
    }

    // Shader + a descriptorless pipeline layout (push constants only).
    vkrpc::CreateShaderModuleRequest smr;
    smr.device = cd.device;
    smr.code.assign(reinterpret_cast<const char*>(kBdaSpv), sizeof(kBdaSpv));
    smr.code_size = smr.code.size();
    const vkrpc::CreateShaderModuleResponse sm = backend.create_shader_module(smr);
    VKR_CHECK(sm.ok && sm.shader_module != 0);
    vkrpc::CreatePipelineLayoutRequest plr;
    plr.device = cd.device;
    plr.set_layout_count = 0;
    plr.push_constant_range_count = 1;
    vkrpc::PushConstantRange pr;
    pr.stage_flags = VK_SHADER_STAGE_COMPUTE_BIT;
    pr.offset = 0;
    pr.size = sizeof(PushBlock);
    plr.push_constant_ranges.push_back(pr);
    const auto pl = backend.create_pipeline_layout(plr);
    VKR_CHECK(pl.ok);
    vkrpc::CreateComputePipelinesRequest cpr;
    cpr.device = cd.device;
    cpr.pipeline_cache = 0;
    cpr.layout = pl.pipeline_layout;
    cpr.shader_module = sm.shader_module;
    cpr.entry_point = "main";
    const auto cp = backend.create_compute_pipelines(cpr);
    VKR_CHECK(cp.ok && cp.pipeline != 0);

    // Record: bind(COMPUTE) + push(addr,scale,bias) + dispatch + barrier + copy-out. No
    // descriptor set anywhere -- the buffer is reachable ONLY through the raw address.
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
    {
        vkrpc::RecordCommandBufferRequest rec;
        rec.command_buffer = cmd;
        rec.one_time_submit = true;
        vkrpc::RecordedCommand bp;
        bp.kind = "bind_pipeline";
        bp.pipeline = cp.pipeline;
        bp.args_i64 = {1}; // COMPUTE
        vkrpc::RecordedCommand pc;
        pc.kind = "push_constants";
        pc.desc_layout = pl.pipeline_layout;
        pc.args_i64 = {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PushBlock)};
        PushBlock pb{};
        pb.addr = addr1.device_address;
        pb.scale = kScale;
        pb.bias = kBias;
        pc.args_blob.assign(reinterpret_cast<const char*>(&pb), sizeof(pb));
        vkrpc::RecordedCommand d;
        d.kind = "dispatch";
        d.args_u64 = {kElems / 64, 1, 1};
        vkrpc::RecordedCommand bar;
        bar.kind = "buffer_memory_barrier";
        bar.src_buffer = bda_buf.buffer;
        bar.src_stage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        bar.dst_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        bar.src_access = VK_ACCESS_SHADER_WRITE_BIT;
        bar.dst_access = VK_ACCESS_TRANSFER_READ_BIT;
        bar.args_u64 = {0, ~0ull}; // WHOLE_SIZE
        vkrpc::RecordedCommand cpy;
        cpy.kind = "copy_buffer";
        cpy.args_u64 = {bda_buf.buffer, readback, 0, 0, bytes};
        rec.commands = {bp, pc, d, bar, cpy};
        VKR_CHECK(backend.record_command_buffer(rec).ok);
    }

    vkrpc::CreateFenceRequest cfr;
    cfr.device = cd.device;
    const auto fence = backend.create_fence(cfr);
    VKR_CHECK(fence.ok);
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
    std::fprintf(stderr, "integration_real_bda: %u results exact through the raw device address\n",
                 kElems);

    return vkr::test::finish("integration_real_bda");
}
