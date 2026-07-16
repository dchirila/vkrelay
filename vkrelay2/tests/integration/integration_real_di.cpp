// descriptorIndexing: in-process real-backend test on the host GPU.
//
// Constructs RealVulkanBackend directly and proves the plumbing/model + the served set
// against the real driver: the scalar/chain normalization (mismatch either direction -- including
// the buffer shader non-uniform wire bits -- the aggregate `descriptorIndexing` bit, an
// UNSERVED shader-indexing bit, and an undersized standalone struct all reject BEFORE a host
// device exists); a device whose chain really enables the FULL served set (kDIFeatureServedBits);
// UPDATE_AFTER_BIND layout + pool admission (and the UAB-layout-needs-UAB-pool
// gate); variable-count allocation sized to the ALLOCATED count on the real driver; the honest
// host GetDescriptorSetLayoutSupport; and THE ordering crux (the lock):
// record a dispatch against a completely UNWRITTEN update-after-bind set (record-time readiness
// exemption), then write it, then destroy-old/update-to-new BEFORE submit (the UAB dangle
// no-invalidation rule), submit, and the byte-exact readback must show the NEW referent's data --
// proving replay reads the LIVE set at submit, not a record-time snapshot.
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
constexpr std::uint32_t kElems = 1024;
constexpr std::uint32_t kScale = 3;
constexpr std::uint32_t kBias = 7;
} // namespace

int main() {
    worker::RealVulkanBackend backend("", "", false);

    const vkrpc::CreateInstanceResponse ci = backend.create_instance({});
    if (!ci.ok) {
        std::fprintf(stderr, "integration_real_di: skipped (no instance: %s)\n", ci.reason.c_str());
        return vkr::test::finish("integration_real_di");
    }
    vkrpc::EnumeratePhysicalDevicesRequest er;
    er.instance = ci.instance;
    const vkrpc::EnumeratePhysicalDevicesResponse en = backend.enumerate_physical_devices(er);
    if (!en.ok || en.devices.empty()) {
        std::fprintf(stderr, "integration_real_di: skipped (no physical device)\n");
        return vkr::test::finish("integration_real_di");
    }
    if ((en.devices.front().caps.queue_flags & VK_QUEUE_COMPUTE_BIT) == 0) {
        std::fprintf(stderr, "integration_real_di: skipped (no COMPUTE on the family)\n");
        return vkr::test::finish("integration_real_di");
    }
    const std::uint64_t phys = en.devices.front().handle;

    // The gated sub-feature set this test drives -- the FULL served set (kDIFeatureServedBits:
    // the buffer-only classes plus the two BUFFER shader non-uniform-indexing wire bits) -- and
    // its chain spelling.
    const std::uint64_t kBits = vkrpc::kDIFeatureServedBits;
    VkPhysicalDeviceDescriptorIndexingFeatures enable_di{};
    enable_di.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES;
    enable_di.shaderUniformBufferArrayNonUniformIndexing = VK_TRUE;
    enable_di.shaderStorageBufferArrayNonUniformIndexing = VK_TRUE;
    enable_di.descriptorBindingUniformBufferUpdateAfterBind = VK_TRUE;
    enable_di.descriptorBindingStorageBufferUpdateAfterBind = VK_TRUE;
    enable_di.descriptorBindingUpdateUnusedWhilePending = VK_TRUE;
    enable_di.descriptorBindingPartiallyBound = VK_TRUE;
    enable_di.descriptorBindingVariableDescriptorCount = VK_TRUE;
    enable_di.runtimeDescriptorArray = VK_TRUE;
    vkrpc::CapabilityChainEntry fe;
    fe.s_type = static_cast<std::uint32_t>(enable_di.sType);
    fe.size = sizeof(enable_di);
    fe.blob.assign(reinterpret_cast<const char*>(&enable_di), sizeof(enable_di));

    // --- The normalization negatives (DI edition) -- all BEFORE any host
    // device exists. ---
    {
        vkrpc::CreateDeviceRequest bad;
        bad.instance = ci.instance;
        bad.physical_device = phys;
        bad.descriptor_indexing_feature_bits = kBits; // scalar without a chain
        VKR_CHECK(!backend.create_device(bad).ok);
        bad.descriptor_indexing_feature_bits = 0; // chain without the scalar
        bad.enabled_feature_chain.push_back(fe);
        VKR_CHECK(!backend.create_device(bad).ok);
        // The 1.2 rollup's AGGREGATE descriptorIndexing bit stays unserved (it asserts the
        // deferred image/texel minimum set).
        VkPhysicalDeviceVulkan12Features agg{};
        agg.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
        agg.descriptorIndexing = VK_TRUE;
        vkrpc::CapabilityChainEntry agg_e;
        agg_e.s_type = static_cast<std::uint32_t>(agg.sType);
        agg_e.size = sizeof(agg);
        agg_e.blob.assign(reinterpret_cast<const char*>(&agg), sizeof(agg));
        bad.enabled_feature_chain = {agg_e};
        bad.descriptor_indexing_feature_bits = 0;
        VKR_CHECK(!backend.create_device(bad).ok);
        // An UNSERVED shader-indexing bit (sampled-image non-uniform -- an image class, deferred
        // in unsupported modes still rejects.
        VkPhysicalDeviceDescriptorIndexingFeatures shader_bit{};
        shader_bit.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES;
        shader_bit.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
        vkrpc::CapabilityChainEntry sb_e;
        sb_e.s_type = static_cast<std::uint32_t>(shader_bit.sType);
        sb_e.size = sizeof(shader_bit);
        sb_e.blob.assign(reinterpret_cast<const char*>(&shader_bit), sizeof(shader_bit));
        bad.enabled_feature_chain = {sb_e};
        VKR_CHECK(!backend.create_device(bad).ok);
        // An undersized known struct rejects (the exact-size gate, DI edition).
        vkrpc::CapabilityChainEntry runt = fe;
        runt.size = 24;
        runt.blob.resize(24);
        bad.enabled_feature_chain = {runt};
        bad.descriptor_indexing_feature_bits = kBits;
        VKR_CHECK(!backend.create_device(bad).ok);
        // the two BUFFER shader non-uniform bits normalize like every other served bit -- a
        // scalar carrying them against a chain that does NOT enable the members is a mismatch.
        VkPhysicalDeviceDescriptorIndexingFeatures no_shader = enable_di;
        no_shader.shaderUniformBufferArrayNonUniformIndexing = VK_FALSE;
        no_shader.shaderStorageBufferArrayNonUniformIndexing = VK_FALSE;
        vkrpc::CapabilityChainEntry ns_e;
        ns_e.s_type = static_cast<std::uint32_t>(no_shader.sType);
        ns_e.size = sizeof(no_shader);
        ns_e.blob.assign(reinterpret_cast<const char*>(&no_shader), sizeof(no_shader));
        bad.enabled_feature_chain = {ns_e};
        bad.descriptor_indexing_feature_bits = kBits; // still claims the shader bits
        VKR_CHECK(!backend.create_device(bad).ok);
        // (the worker ServedBits clamp): a DEFERRED-but-known bit rejects even when
        // scalar and chain AGREE and the host supports the class -- sampled-image UAB is real on
        // this RTX driver, but never proved it, so the worker refuses to enable it.
        VkPhysicalDeviceDescriptorIndexingFeatures deferred = enable_di;
        deferred.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
        vkrpc::CapabilityChainEntry df_e;
        df_e.s_type = static_cast<std::uint32_t>(deferred.sType);
        df_e.size = sizeof(deferred);
        df_e.blob.assign(reinterpret_cast<const char*>(&deferred), sizeof(deferred));
        bad.enabled_feature_chain = {df_e};
        bad.descriptor_indexing_feature_bits = kBits | vkrpc::kDIFeatureUpdateAfterBindSampledImage;
        VKR_CHECK(!backend.create_device(bad).ok);
    }

    // --- The container flags need NO feature: a FEATURELESS device creates a
    // UAB_POOL layout (flagless bindings) + an UPDATE_AFTER_BIND pool and allocates from it on
    // the real driver; a plain pool still refuses the UAB layout; a DYNAMIC type inside a
    // UAB_POOL layout rejects (VUID 03000). ---
    {
        vkrpc::CreateDeviceRequest plain;
        plain.instance = ci.instance;
        plain.physical_device = phys;
        const auto pd = backend.create_device(plain);
        VKR_CHECK(pd.ok);
        vkrpc::CreateDescriptorSetLayoutRequest lr;
        lr.device = pd.device;
        lr.layout_flags = vkrpc::kDescriptorSetLayoutCreateUpdateAfterBindPool;
        vkrpc::DescriptorSetLayoutBindingDesc b;
        b.binding = 0;
        b.descriptor_type = vkrpc::kDescriptorTypeStorageBuffer;
        b.descriptor_count = 2;
        b.stage_flags = VK_SHADER_STAGE_COMPUTE_BIT;
        b.binding_flags = 0;
        lr.bindings.push_back(b);
        const auto lay = backend.create_descriptor_set_layout(lr);
        VKR_CHECK(lay.ok); // the container flag alone needs no feature, on the real driver
        // A FLAGLESS dynamic binding inside a UAB_POOL layout is spec-valid (the
        // dynamic ban, VUID 03001, is conditioned on an actual UPDATE_AFTER_BIND binding) -- the
        // real driver must accept it through the relay.
        vkrpc::CreateDescriptorSetLayoutRequest dyn = lr;
        dyn.bindings[0].descriptor_type = vkrpc::kDescriptorTypeUniformBufferDynamic;
        VKR_CHECK(backend.create_descriptor_set_layout(dyn).ok);
        auto pool_req = [&](long long flags) {
            vkrpc::CreateDescriptorPoolRequest r;
            r.device = pd.device;
            r.max_sets = 1;
            r.flags = flags;
            r.pool_sizes.push_back({vkrpc::kDescriptorTypeStorageBuffer, 4});
            return backend.create_descriptor_pool(r);
        };
        const auto p_plain = pool_req(0);
        VKR_CHECK(p_plain.ok);
        const auto p_uab = pool_req(vkrpc::kDescriptorPoolCreateUpdateAfterBind);
        VKR_CHECK(p_uab.ok); // container flag, featureless -> fine on the real driver
        vkrpc::AllocateDescriptorSetsRequest a;
        a.device = pd.device;
        a.pool = p_plain.pool;
        a.set_layouts = {lay.set_layout};
        VKR_CHECK(!backend.allocate_descriptor_sets(a).ok); // UAB layout needs a UAB pool
        a.pool = p_uab.pool;
        VKR_CHECK(backend.allocate_descriptor_sets(a).ok);
    }

    // --- The DI device (consistent scalar + chain; the host really enables the sub-features). ---
    vkrpc::CreateDeviceRequest cdr;
    cdr.instance = ci.instance;
    cdr.physical_device = phys;
    cdr.descriptor_indexing_feature_bits = kBits;
    cdr.enabled_feature_chain.push_back(fe);
    const vkrpc::CreateDeviceResponse cd = backend.create_device(cdr);
    if (!cd.ok) {
        std::fprintf(stderr, "integration_real_di: skipped (host lacks the DI sub-features: %s)\n",
                     cd.reason.c_str());
        return vkr::test::finish("integration_real_di");
    }
    vkrpc::GetDeviceQueueRequest gqr;
    gqr.device = cd.device;
    gqr.queue_family_index = cd.queue_family_index;
    gqr.queue_index = 0;
    const vkrpc::GetDeviceQueueResponse q = backend.get_device_queue(gqr);
    VKR_CHECK(q.ok && q.queue != 0);

    // The compute shader's layout {0,1: STORAGE; 2: UNIFORM}, every binding UPDATE_AFTER_BIND.
    vkrpc::CreateDescriptorSetLayoutRequest dslr;
    dslr.device = cd.device;
    dslr.layout_flags = vkrpc::kDescriptorSetLayoutCreateUpdateAfterBindPool;
    for (int b = 0; b < 3; ++b) {
        vkrpc::DescriptorSetLayoutBindingDesc d;
        d.binding = b;
        d.descriptor_type =
            b == 2 ? vkrpc::kDescriptorTypeUniformBuffer : vkrpc::kDescriptorTypeStorageBuffer;
        d.descriptor_count = 1;
        d.stage_flags = VK_SHADER_STAGE_COMPUTE_BIT;
        d.binding_flags = vkrpc::kDescriptorBindingUpdateAfterBind;
        dslr.bindings.push_back(d);
    }
    // The honest layout-support: the HOST's real answer for the UAB layout, and a variable-count
    // layout's max from the host (not the old local 1<<20 fiction).
    {
        const auto sup = backend.get_descriptor_set_layout_support(dslr);
        VKR_CHECK(sup.ok);
        VKR_CHECK_EQ(sup.supported, 1);
        vkrpc::CreateDescriptorSetLayoutRequest var;
        var.device = cd.device;
        vkrpc::DescriptorSetLayoutBindingDesc vb;
        vb.binding = 0;
        vb.descriptor_type = vkrpc::kDescriptorTypeStorageBuffer;
        vb.descriptor_count = 64;
        vb.stage_flags = VK_SHADER_STAGE_COMPUTE_BIT;
        vb.binding_flags = vkrpc::kDescriptorBindingVariableDescriptorCount |
                           vkrpc::kDescriptorBindingPartiallyBound;
        var.bindings.push_back(vb);
        const auto vsup = backend.get_descriptor_set_layout_support(var);
        VKR_CHECK(vsup.ok);
        VKR_CHECK_EQ(vsup.supported, 1);
        VKR_CHECK(vsup.max_variable_descriptor_count >= 64);
    }
    const auto dsl = backend.create_descriptor_set_layout(dslr);
    VKR_CHECK(dsl.ok);
    // (VUID 03001): once ANY binding carries UPDATE_AFTER_BIND, a DYNAMIC sibling
    // rejects -- even on this fully DI-enabled device, and BEFORE the host driver.
    {
        vkrpc::CreateDescriptorSetLayoutRequest sib;
        sib.device = cd.device;
        sib.layout_flags = vkrpc::kDescriptorSetLayoutCreateUpdateAfterBindPool;
        vkrpc::DescriptorSetLayoutBindingDesc b0;
        b0.binding = 0;
        b0.descriptor_type = vkrpc::kDescriptorTypeStorageBuffer;
        b0.descriptor_count = 1;
        b0.stage_flags = VK_SHADER_STAGE_COMPUTE_BIT;
        b0.binding_flags = vkrpc::kDescriptorBindingUpdateAfterBind;
        vkrpc::DescriptorSetLayoutBindingDesc b1;
        b1.binding = 1;
        b1.descriptor_type = vkrpc::kDescriptorTypeUniformBufferDynamic;
        b1.descriptor_count = 1;
        b1.stage_flags = VK_SHADER_STAGE_COMPUTE_BIT;
        b1.binding_flags = 0;
        sib.bindings = {b0, b1};
        VKR_CHECK(!backend.create_descriptor_set_layout(sib).ok);
    }

    // Pools: the UAB layout refuses a plain pool, then allocates from a UAB pool.
    auto make_pool = [&](long long flags) {
        vkrpc::CreateDescriptorPoolRequest r;
        r.device = cd.device;
        r.max_sets = 4;
        r.flags = flags;
        r.pool_sizes.push_back({vkrpc::kDescriptorTypeStorageBuffer, 128});
        r.pool_sizes.push_back({vkrpc::kDescriptorTypeUniformBuffer, 4});
        return backend.create_descriptor_pool(r);
    };
    const auto pool_plain = make_pool(0);
    VKR_CHECK(pool_plain.ok);
    const auto pool_uab = make_pool(vkrpc::kDescriptorPoolCreateUpdateAfterBind);
    VKR_CHECK(pool_uab.ok);
    {
        vkrpc::AllocateDescriptorSetsRequest r;
        r.device = cd.device;
        r.pool = pool_plain.pool;
        r.set_layouts = {dsl.set_layout};
        VKR_CHECK(!backend.allocate_descriptor_sets(r).ok); // UAB layout needs a UAB pool
        r.pool = pool_uab.pool;
        const auto a = backend.allocate_descriptor_sets(r);
        VKR_CHECK(a.ok && !a.descriptor_sets.empty());
    }
    vkrpc::AllocateDescriptorSetsRequest adr;
    adr.device = cd.device;
    adr.pool = pool_uab.pool;
    adr.set_layouts = {dsl.set_layout};
    const auto ds = backend.allocate_descriptor_sets(adr);
    VKR_CHECK(ds.ok);
    const std::uint64_t dset = ds.descriptor_sets.front();

    // Variable-count on the real driver: allocated count 16 out of a declared 64; the slot vector
    // bounds writes at the ALLOCATED count.
    {
        vkrpc::CreateDescriptorSetLayoutRequest var;
        var.device = cd.device;
        vkrpc::DescriptorSetLayoutBindingDesc vb;
        vb.binding = 0;
        vb.descriptor_type = vkrpc::kDescriptorTypeStorageBuffer;
        vb.descriptor_count = 64;
        vb.stage_flags = VK_SHADER_STAGE_COMPUTE_BIT;
        vb.binding_flags = vkrpc::kDescriptorBindingVariableDescriptorCount |
                           vkrpc::kDescriptorBindingPartiallyBound;
        var.bindings.push_back(vb);
        const auto vdsl = backend.create_descriptor_set_layout(var);
        VKR_CHECK(vdsl.ok);
        vkrpc::AllocateDescriptorSetsRequest r;
        r.device = cd.device;
        r.pool = pool_plain.pool;
        r.set_layouts = {vdsl.set_layout};
        r.variable_counts = {128}; // > the declared max 64
        VKR_CHECK(!backend.allocate_descriptor_sets(r).ok);
        r.variable_counts = {16};
        const auto a = backend.allocate_descriptor_sets(r);
        VKR_CHECK(a.ok);
    }

    // --- Buffers + the input patterns for the ordering crux. ---
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
        VKR_CHECK(buf.ok);
        vkrpc::AllocateMemoryRequest amr;
        amr.device = cd.device;
        amr.allocation_size = buf.mem_size;
        amr.memory_type_index = coherent_type;
        const vkrpc::AllocateMemoryResponse m = backend.allocate_memory(amr);
        VKR_CHECK(m.ok);
        vkrpc::BindBufferMemoryRequest bbr;
        bbr.buffer = buf.buffer;
        bbr.memory = m.memory;
        bbr.memory_offset = 0;
        VKR_CHECK(backend.bind_buffer_memory(bbr).ok);
        if (data != nullptr) {
            vkrpc::WriteMemoryRangesRequest wmr;
            vkrpc::MemoryUpload up;
            up.memory = m.memory;
            up.ranges.push_back(vkrpc::MemoryUploadRange{0, nbytes});
            wmr.uploads.push_back(up);
            wmr.payload.assign(reinterpret_cast<const char*>(data),
                               static_cast<std::size_t>(nbytes));
            VKR_CHECK(backend.write_memory_ranges(wmr).ok);
        }
        out_buffer = buf.buffer;
        out_memory = m.memory;
    };
    std::vector<std::uint32_t> in_old(kElems), in_new(kElems);
    for (std::uint32_t i = 0; i < kElems; ++i) {
        in_old[i] = 1000000 + i; // the STALE referent: its data must NOT reach the readback
        in_new[i] = i * 5 + 1;   // the LIVE referent updated in after the record
    }
    const std::uint32_t scale = kScale;
    std::uint64_t b_old = 0, m_old = 0, b_new = 0, m_new = 0, b_out = 0, m_out = 0, b_ubo = 0,
                  m_ubo = 0, b_read = 0, m_read = 0;
    make_bound_buffer(vkrpc::kBufferUsageStorageBuffer, in_old.data(), bytes, b_old, m_old);
    make_bound_buffer(vkrpc::kBufferUsageStorageBuffer, in_new.data(), bytes, b_new, m_new);
    make_bound_buffer(vkrpc::kBufferUsageStorageBuffer | vkrpc::kBufferUsageTransferSrc, nullptr,
                      bytes, b_out, m_out);
    make_bound_buffer(vkrpc::kBufferUsageUniformBuffer, &scale, sizeof(scale), b_ubo, m_ubo);
    make_bound_buffer(vkrpc::kBufferUsageTransferDst, nullptr, bytes, b_read, m_read);

    // Shader + pipeline (the compute canary kernel: out[i] = in[i]*scale + bias + i).
    vkrpc::CreateShaderModuleRequest smr;
    smr.device = cd.device;
    smr.code.assign(reinterpret_cast<const char*>(kComputeSpv), sizeof(kComputeSpv));
    smr.code_size = smr.code.size();
    const auto sm = backend.create_shader_module(smr);
    VKR_CHECK(sm.ok);
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
    vkrpc::CreateComputePipelinesRequest cpr;
    cpr.device = cd.device;
    cpr.pipeline_cache = 0;
    cpr.layout = pl.pipeline_layout;
    cpr.shader_module = sm.shader_module;
    cpr.entry_point = "main";
    const auto cp = backend.create_compute_pipelines(cpr);
    VKR_CHECK(cp.ok);

    vkrpc::CreateCommandPoolRequest cpq;
    cpq.device = cd.device;
    cpq.queue_family_index = cd.queue_family_index;
    const auto cpool = backend.create_command_pool(cpq);
    VKR_CHECK(cpool.ok);
    vkrpc::AllocateCommandBuffersRequest acb;
    acb.command_pool = cpool.command_pool;
    acb.count = 1;
    const auto cbs = backend.allocate_command_buffers(acb);
    VKR_CHECK(cbs.ok);
    const std::uint64_t cmd = cbs.command_buffers.front();

    // --- THE ordering crux: RECORD FIRST (the set is completely unwritten -- the UAB record-time
    // readiness exemption on the REAL validator), then write, then destroy-old/update-to-new,
    // then submit. ---
    {
        vkrpc::RecordCommandBufferRequest rec;
        rec.command_buffer = cmd;
        rec.one_time_submit = true;
        vkrpc::RecordedCommand bp;
        bp.kind = "bind_pipeline";
        bp.pipeline = cp.pipeline;
        bp.args_i64 = {1};
        vkrpc::RecordedCommand bs;
        bs.kind = "bind_descriptor_sets";
        bs.desc_layout = pl.pipeline_layout;
        bs.first_set = 0;
        bs.descriptor_sets = {dset};
        bs.args_i64 = {1};
        vkrpc::RecordedCommand pc;
        pc.kind = "push_constants";
        pc.desc_layout = pl.pipeline_layout;
        pc.args_i64 = {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(std::uint32_t)};
        const std::uint32_t bias = kBias;
        pc.args_blob.assign(reinterpret_cast<const char*>(&bias), sizeof(bias));
        vkrpc::RecordedCommand d;
        d.kind = "dispatch";
        d.args_u64 = {kElems / 64, 1, 1};
        vkrpc::RecordedCommand bar;
        bar.kind = "buffer_memory_barrier";
        bar.src_buffer = b_out;
        bar.src_stage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        bar.dst_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        bar.src_access = VK_ACCESS_SHADER_WRITE_BIT;
        bar.dst_access = VK_ACCESS_TRANSFER_READ_BIT;
        bar.args_u64 = {0, ~0ull};
        vkrpc::RecordedCommand cpy;
        cpy.kind = "copy_buffer";
        cpy.args_u64 = {b_out, b_read, 0, 0, bytes};
        rec.commands = {bp, bs, pc, d, bar, cpy};
        VKR_CHECK(backend.record_command_buffer(rec).ok); // unwritten UAB set records fine
    }
    auto write_binding = [&](int binding, std::uint64_t buffer, std::uint64_t range, int type) {
        vkrpc::UpdateDescriptorSetsRequest ur;
        ur.device = cd.device;
        vkrpc::WriteDescriptorSetDesc w;
        w.dst_set = dset;
        w.dst_binding = binding;
        w.dst_array_element = 0;
        w.descriptor_type = type;
        w.descriptor_count = 1;
        w.buffer_infos.push_back({buffer, 0, range});
        ur.writes.push_back(w);
        return backend.update_descriptor_sets(ur);
    };
    // Write the set AFTER the record: binding0 -> the OLD input first.
    VKR_CHECK(write_binding(0, b_old, bytes, vkrpc::kDescriptorTypeStorageBuffer).ok);
    VKR_CHECK(write_binding(1, b_out, bytes, vkrpc::kDescriptorTypeStorageBuffer).ok);
    VKR_CHECK(
        write_binding(2, b_ubo, sizeof(std::uint32_t), vkrpc::kDescriptorTypeUniformBuffer).ok);
    // Destroy the OLD referent (the UAB dangle must NOT invalidate the recorded CB), then update
    // the binding to the NEW referent -- all before the submit.
    {
        vkrpc::HandleRequest db;
        db.handle = b_old;
        VKR_CHECK(backend.destroy_buffer(db).ok);
    }
    VKR_CHECK(write_binding(0, b_new, bytes, vkrpc::kDescriptorTypeStorageBuffer).ok);

    vkrpc::CreateFenceRequest cfr;
    cfr.device = cd.device;
    const auto fence = backend.create_fence(cfr);
    VKR_CHECK(fence.ok);
    vkrpc::QueueSubmitRequest sub;
    sub.queue = q.queue;
    sub.command_buffers = {cmd};
    sub.fence = fence.fence;
    const auto s = backend.queue_submit(sub); // NOT invalidated: the submit must land
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
    rd.memory = m_read;
    rd.ranges.push_back(vkrpc::MemoryUploadRange{0, bytes});
    rr.reads.push_back(rd);
    const auto res = backend.read_memory_ranges(rr);
    VKR_CHECK(res.ok);
    VKR_CHECK_EQ(res.payload.size(), static_cast<std::size_t>(bytes));
    const auto* out = reinterpret_cast<const std::uint32_t*>(res.payload.data());
    std::uint32_t bad = 0;
    for (std::uint32_t i = 0; i < kElems; ++i) {
        const std::uint32_t expect = in_new[i] * kScale + kBias + i; // the NEW referent's data
        if (out[i] != expect) {
            ++bad;
        }
    }
    VKR_CHECK_EQ(bad, 0u);
    std::fprintf(stderr,
                 "integration_real_di: %u results exact from the LIVE (post-record) descriptor "
                 "set -- record -> write -> destroy-old -> update-to-new -> submit\n",
                 kElems);

    return vkr::test::finish("integration_real_di");
}
