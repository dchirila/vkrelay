// vkrelay2 Vulkan-1.2 descriptorIndexing canary -- the native lane and buffer-only bindless
// support.
//
// A surfaceless native Vulkan program run through the real loader + the session-pinned vkrelay2
// ICD on the NATIVE lane (run_vk13_di_smoke.sh --frontend vulkan13 -> VKRELAY2_NATIVE_LANE=1). It
// proves, end to end on the real GPU, that the relay serves the SERVED buffer-only
// descriptorIndexing subset faithfully -- and nothing more. Assertions:
//   - the device reports apiVersion >= 1.2 (the exact version is the Vulkan 1.3 canary's
//   assertion);
//   - the Vulkan12Features rollup + the standalone DescriptorIndexingFeatures struct AGREE and
//     report the 8 served members TRUE on the native lane, while the AGGREGATE `descriptorIndexing`
//     bit and every unserved member (image/texel UAB classes, their shader bits, the
//     dynamic-indexing bits) stay FALSE everywhere (fail-closed by name);
//   - enabling the aggregate or a deferred member through the loader is
//   VK_ERROR_FEATURE_NOT_PRESENT;
//   - the embedded SPIR-V really carries the served capabilities (OpCapability ShaderNonUniform +
//     RuntimeDescriptorArray + UniformBufferArrayNonUniformIndexing +
//     StorageBufferArrayNonUniformIndexing, plus a NonUniform decoration) -- scanned word by word
//     so an optimized-away access cannot fake the shader proof;
//   - vkGetDescriptorSetLayoutSupport answers through the worker (RpcOp 88) with the HOST's real
//     maxVariableDescriptorCount for the DI-shaped layout (not a local fiction);
//   - THE crux, through the loader: a dispatch is RECORDED against a still-unwritten
//     update-after-bind set (variable count 4 of a declared 8, two slots left unwritten under
//     PARTIALLY_BOUND), the descriptors are written AFTER recording, the first source buffer is
//     then DESTROYED and its slot repointed to a second source, and the submit computes byte-exact
//     results from the NEW referent -- update-after-bind reads the LIVE set, not a stale bake.
//
// Greppable markers on stdout; run_vk13_di_smoke.sh gates on them. Skips cleanly (exit 0) when no
// ICD/worker stack is reachable; FAILs (nonzero) on a real regression.

#include <vulkan/vulkan.h>

#include "tests/di_spv.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>

namespace {

constexpr std::uint32_t kElems = 1024;    // / local_size 64 = 16 groups
constexpr std::uint32_t kDeclaredMax = 8; // binding 1's declared descriptorCount
constexpr std::uint32_t kVarCount = 4;    // allocated variable count (below the declared max)
constexpr std::uint32_t kSrcSlot = 1;     // bufs[] slot the shader reads (slots 0/2 stay unwritten)
constexpr std::uint32_t kDstSlot = 3;     // bufs[] slot the shader writes
constexpr std::uint32_t kParamIdx = 1;    // params[] element the shader adds
constexpr std::uint32_t kScale = 3;
constexpr std::uint32_t kAdd = 11;   // params[1].add (params[0] holds a decoy)
constexpr std::uint32_t kDecoy = 99; // params[0].add -- must NOT appear in the result

// The push-constant block the shader declares: the four dynamic indices/factors.
struct PushBlock {
    std::uint32_t src, dst, pidx, scale;
};
static_assert(sizeof(PushBlock) == 16, "push block must match the shader's std430 layout");

// SPIR-V word-scan: the shader proof is only a proof if the module REALLY
// declares the served capabilities. OpCapability = 17 (wordcount 2), OpDecorate = 71; the
// descriptor-indexing capability/decoration values are core SPIR-V 1.5.
constexpr std::uint32_t kSpvOpCapability = 17;
constexpr std::uint32_t kSpvOpDecorate = 71;
constexpr std::uint32_t kSpvDecorationNonUniform = 5300;
constexpr std::uint32_t kSpvCapShaderNonUniform = 5301;
constexpr std::uint32_t kSpvCapRuntimeDescriptorArray = 5302;
constexpr std::uint32_t kSpvCapUniformBufferArrayNonUniformIndexing = 5306;
constexpr std::uint32_t kSpvCapStorageBufferArrayNonUniformIndexing = 5308;

bool spv_has_capability(const std::uint32_t* words, std::size_t count, std::uint32_t cap) {
    for (std::size_t i = 5; i < count;) { // 5-word header, then instructions
        const std::uint32_t wc = words[i] >> 16, op = words[i] & 0xFFFFu;
        if (wc == 0) {
            return false; // malformed stream
        }
        if (op == kSpvOpCapability && wc == 2 && i + 1 < count && words[i + 1] == cap) {
            return true;
        }
        i += wc;
    }
    return false;
}

bool spv_has_nonuniform_decoration(const std::uint32_t* words, std::size_t count) {
    for (std::size_t i = 5; i < count;) {
        const std::uint32_t wc = words[i] >> 16, op = words[i] & 0xFFFFu;
        if (wc == 0) {
            return false;
        }
        if (op == kSpvOpDecorate && wc >= 3 && i + 2 < count &&
            words[i + 2] == kSpvDecorationNonUniform) {
            return true;
        }
        i += wc;
    }
    return false;
}

} // namespace

int main() {
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.apiVersion = VK_API_VERSION_1_2; // descriptorIndexing is core 1.2
    app.pApplicationName = "vkrelay2-vk13-di-canary";
    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    VkInstance instance = VK_NULL_HANDLE;
    if (vkCreateInstance(&ici, nullptr, &instance) != VK_SUCCESS) {
        std::printf("VK13-DI-CANARY: SKIP (vkCreateInstance failed -- no ICD/worker?)\n");
        return 0;
    }

    std::uint32_t pd_count = 0;
    vkEnumeratePhysicalDevices(instance, &pd_count, nullptr);
    if (pd_count == 0) {
        std::printf("VK13-DI-CANARY: FAIL (no physical device)\n");
        vkDestroyInstance(instance, nullptr);
        return 2;
    }
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    pd_count = 1;
    vkEnumeratePhysicalDevices(instance, &pd_count, &phys);

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(phys, &props);
    const std::uint32_t maj = VK_API_VERSION_MAJOR(props.apiVersion);
    const std::uint32_t min = VK_API_VERSION_MINOR(props.apiVersion);
    std::printf("VK13-DI-CANARY: device '%s' apiVersion=%u.%u\n", props.deviceName, maj, min);
    // descriptorIndexing is core 1.2: this canary needs AT LEAST 1.2. The exact reported version
    // is the Vulkan 1.3 canary's assertion (the native lane reports 1.3 once the worker vouches
    // vk13_ready); this canary's contract is the served DI subset, version-agnostic.
    if (!(maj == 1 && min >= 2)) {
        std::printf("VK13-DI-CANARY: FAIL (device apiVersion %u.%u, expected >= 1.2)\n", maj, min);
        vkDestroyInstance(instance, nullptr);
        return 1;
    }

    // --- Feature exposure: the 1.2 rollup AND the standalone struct, member by member. ---
    VkPhysicalDeviceVulkan12Features f12{};
    f12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    VkPhysicalDeviceFeatures2 feat2{};
    feat2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    feat2.pNext = &f12;
    vkGetPhysicalDeviceFeatures2(phys, &feat2);
    VkPhysicalDeviceDescriptorIndexingFeatures dif{};
    dif.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES;
    VkPhysicalDeviceFeatures2 feat2b{};
    feat2b.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    feat2b.pNext = &dif;
    vkGetPhysicalDeviceFeatures2(phys, &feat2b);

    // The 8 served members, from BOTH spellings (they must agree).
    const VkBool32 served_rollup[8] = {
        f12.shaderUniformBufferArrayNonUniformIndexing,
        f12.shaderStorageBufferArrayNonUniformIndexing,
        f12.descriptorBindingUniformBufferUpdateAfterBind,
        f12.descriptorBindingStorageBufferUpdateAfterBind,
        f12.descriptorBindingUpdateUnusedWhilePending,
        f12.descriptorBindingPartiallyBound,
        f12.descriptorBindingVariableDescriptorCount,
        f12.runtimeDescriptorArray,
    };
    const VkBool32 served_standalone[8] = {
        dif.shaderUniformBufferArrayNonUniformIndexing,
        dif.shaderStorageBufferArrayNonUniformIndexing,
        dif.descriptorBindingUniformBufferUpdateAfterBind,
        dif.descriptorBindingStorageBufferUpdateAfterBind,
        dif.descriptorBindingUpdateUnusedWhilePending,
        dif.descriptorBindingPartiallyBound,
        dif.descriptorBindingVariableDescriptorCount,
        dif.runtimeDescriptorArray,
    };
    // Every UNSERVED member (both spellings) + the AGGREGATE (rollup only) must be FALSE.
    const VkBool32 unserved[] = {
        f12.descriptorIndexing,
        f12.shaderInputAttachmentArrayDynamicIndexing,
        f12.shaderUniformTexelBufferArrayDynamicIndexing,
        f12.shaderStorageTexelBufferArrayDynamicIndexing,
        f12.shaderSampledImageArrayNonUniformIndexing,
        f12.shaderStorageImageArrayNonUniformIndexing,
        f12.shaderInputAttachmentArrayNonUniformIndexing,
        f12.shaderUniformTexelBufferArrayNonUniformIndexing,
        f12.shaderStorageTexelBufferArrayNonUniformIndexing,
        f12.descriptorBindingSampledImageUpdateAfterBind,
        f12.descriptorBindingStorageImageUpdateAfterBind,
        f12.descriptorBindingUniformTexelBufferUpdateAfterBind,
        f12.descriptorBindingStorageTexelBufferUpdateAfterBind,
        dif.shaderInputAttachmentArrayDynamicIndexing,
        dif.shaderUniformTexelBufferArrayDynamicIndexing,
        dif.shaderStorageTexelBufferArrayDynamicIndexing,
        dif.shaderSampledImageArrayNonUniformIndexing,
        dif.shaderStorageImageArrayNonUniformIndexing,
        dif.shaderInputAttachmentArrayNonUniformIndexing,
        dif.shaderUniformTexelBufferArrayNonUniformIndexing,
        dif.shaderStorageTexelBufferArrayNonUniformIndexing,
        dif.descriptorBindingSampledImageUpdateAfterBind,
        dif.descriptorBindingStorageImageUpdateAfterBind,
        dif.descriptorBindingUniformTexelBufferUpdateAfterBind,
        dif.descriptorBindingStorageTexelBufferUpdateAfterBind,
    };
    bool served = true, agree = true, unserved_leak = false;
    for (int i = 0; i < 8; ++i) {
        served = served && served_rollup[i] != VK_FALSE;
        agree = agree && (served_rollup[i] != VK_FALSE) == (served_standalone[i] != VK_FALSE);
    }
    for (const VkBool32 v : unserved) {
        unserved_leak = unserved_leak || v != VK_FALSE;
    }
    std::printf("VK13-DI-CANARY: feature di_served=%d di_unserved_leak=%d di_agree=%d\n",
                served ? 1 : 0, unserved_leak ? 1 : 0, agree ? 1 : 0);
    if (unserved_leak) {
        std::printf("VK13-DI-CANARY: FAIL (an unserved descriptorIndexing member -- the aggregate "
                    "bit, an image/texel class, or a dynamic-indexing bit -- leaked TRUE)\n");
        vkDestroyInstance(instance, nullptr);
        return 1;
    }
    if (!agree) {
        std::printf("VK13-DI-CANARY: FAIL (the 1.2 rollup and the standalone struct disagree on "
                    "the served members)\n");
        vkDestroyInstance(instance, nullptr);
        return 1;
    }
    if (!served) {
        std::printf("VK13-DI-CANARY: FAIL (served descriptorIndexing members reported FALSE -- on "
                    "the native lane they must be TRUE; on the default lane this FAIL is the "
                    "expected steering proof)\n");
        vkDestroyInstance(instance, nullptr);
        return 1;
    }

    // --- The shader proof is only a proof if the module declares the capabilities. ---
    const std::size_t spv_words = sizeof(kDiSpv) / sizeof(kDiSpv[0]);
    const bool caps_ok =
        spv_has_capability(kDiSpv, spv_words, kSpvCapShaderNonUniform) &&
        spv_has_capability(kDiSpv, spv_words, kSpvCapRuntimeDescriptorArray) &&
        spv_has_capability(kDiSpv, spv_words, kSpvCapUniformBufferArrayNonUniformIndexing) &&
        spv_has_capability(kDiSpv, spv_words, kSpvCapStorageBufferArrayNonUniformIndexing) &&
        spv_has_nonuniform_decoration(kDiSpv, spv_words);
    std::printf("VK13-DI-CANARY: spv nonuniform+runtime-array capabilities present=%d\n",
                caps_ok ? 1 : 0);
    if (!caps_ok) {
        std::printf("VK13-DI-CANARY: FAIL (the embedded SPIR-V lost a served capability or every "
                    "NonUniform decoration -- the compiler optimized the proof away; regenerate "
                    "di_spv.h)\n");
        vkDestroyInstance(instance, nullptr);
        return 1;
    }

    std::uint32_t qf_count = 1;
    VkQueueFamilyProperties qf{};
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &qf_count, &qf);
    if ((qf.queueFlags & VK_QUEUE_COMPUTE_BIT) == 0) {
        std::printf("VK13-DI-CANARY: SKIP (family 0 has no COMPUTE)\n");
        vkDestroyInstance(instance, nullptr);
        return 0;
    }

    const float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = 0;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;

    // --- Negative creates through the loader: the aggregate and a deferred member must both be
    // VK_ERROR_FEATURE_NOT_PRESENT (they are reported FALSE). ---
    {
        VkPhysicalDeviceVulkan12Features enable_agg{};
        enable_agg.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
        enable_agg.descriptorIndexing = VK_TRUE;
        VkDeviceCreateInfo dci{};
        dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        dci.pNext = &enable_agg;
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qci;
        VkDevice rejected = VK_NULL_HANDLE;
        const VkResult r_agg = vkCreateDevice(phys, &dci, nullptr, &rejected);
        VkPhysicalDeviceDescriptorIndexingFeatures enable_def{};
        enable_def.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES;
        enable_def.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
        dci.pNext = &enable_def;
        VkDevice rejected2 = VK_NULL_HANDLE;
        const VkResult r_def = vkCreateDevice(phys, &dci, nullptr, &rejected2);
        std::printf("VK13-DI-CANARY: negative-create aggregate=%d deferred=%d (want %d = "
                    "FEATURE_NOT_PRESENT)\n",
                    r_agg, r_def, VK_ERROR_FEATURE_NOT_PRESENT);
        if (r_agg != VK_ERROR_FEATURE_NOT_PRESENT || r_def != VK_ERROR_FEATURE_NOT_PRESENT) {
            std::printf("VK13-DI-CANARY: FAIL (enabling the aggregate / a deferred member did not "
                        "reject with FEATURE_NOT_PRESENT)\n");
            if (rejected != VK_NULL_HANDLE) {
                vkDestroyDevice(rejected, nullptr);
            }
            if (rejected2 != VK_NULL_HANDLE) {
                vkDestroyDevice(rejected2, nullptr);
            }
            vkDestroyInstance(instance, nullptr);
            return 1;
        }
    }

    int rc = 1;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    VkDescriptorPool dpool = VK_NULL_HANDLE;
    VkShaderModule module = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkCommandPool cpool = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkBuffer buf_params0 = VK_NULL_HANDLE, buf_params1 = VK_NULL_HANDLE;
    VkBuffer buf_src_a = VK_NULL_HANDLE, buf_src_b = VK_NULL_HANDLE;
    VkBuffer buf_dst = VK_NULL_HANDLE, buf_read = VK_NULL_HANDLE;
    VkDeviceMemory mem_params0 = VK_NULL_HANDLE, mem_params1 = VK_NULL_HANDLE;
    VkDeviceMemory mem_src_a = VK_NULL_HANDLE, mem_src_b = VK_NULL_HANDLE;
    VkDeviceMemory mem_dst = VK_NULL_HANDLE, mem_read = VK_NULL_HANDLE;

    {
        // --- Device with EXACTLY the served subset enabled, via the STANDALONE spelling (the
        // proof matrix). ---
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
        VkDeviceCreateInfo dci{};
        dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        dci.pNext = &enable_di;
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qci;
        if (vkCreateDevice(phys, &dci, nullptr, &device) != VK_SUCCESS) {
            std::printf("VK13-DI-CANARY: FAIL (vkCreateDevice with the served descriptorIndexing "
                        "subset)\n");
            goto teardown;
        }
        vkGetDeviceQueue(device, 0, 0, &queue);

        // --- The DI-shaped layout: binding 0 = the UBO array [2] (UPDATE_AFTER_BIND); binding 1 =
        // the SSBO array [kDeclaredMax] (UPDATE_AFTER_BIND + UPDATE_UNUSED_WHILE_PENDING +
        // PARTIALLY_BOUND + VARIABLE_DESCRIPTOR_COUNT; the HIGHEST binding, per the VDC rule). ---
        VkDescriptorSetLayoutBinding binds[2]{};
        binds[0].binding = 0;
        binds[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        binds[0].descriptorCount = 2;
        binds[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        binds[1].binding = 1;
        binds[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        binds[1].descriptorCount = kDeclaredMax;
        binds[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        const VkDescriptorBindingFlags bind_flags[2] = {
            VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,
            VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
                VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT |
                VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
                VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT,
        };
        VkDescriptorSetLayoutBindingFlagsCreateInfo bfci{};
        bfci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
        bfci.bindingCount = 2;
        bfci.pBindingFlags = bind_flags;
        VkDescriptorSetLayoutCreateInfo dslci{};
        dslci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        dslci.pNext = &bfci;
        dslci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
        dslci.bindingCount = 2;
        dslci.pBindings = binds;

        // --- The HOST's layout-support answer (RpcOp 88) BEFORE creating: supported, and the
        // real maxVariableDescriptorCount (not a local fiction). ---
        VkDescriptorSetVariableDescriptorCountLayoutSupport var_sup{};
        var_sup.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_LAYOUT_SUPPORT;
        VkDescriptorSetLayoutSupport sup{};
        sup.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_SUPPORT;
        sup.pNext = &var_sup;
        vkGetDescriptorSetLayoutSupport(device, &dslci, &sup);
        std::printf("VK13-DI-CANARY: layout-support supported=%d maxVariableDescriptorCount=%u\n",
                    sup.supported ? 1 : 0, var_sup.maxVariableDescriptorCount);
        if (!sup.supported || var_sup.maxVariableDescriptorCount < kVarCount) {
            std::printf("VK13-DI-CANARY: FAIL (the host rejected the DI-shaped layout or answered "
                        "a variable-count max below %u)\n",
                        kVarCount);
            goto teardown;
        }

        if (vkCreateDescriptorSetLayout(device, &dslci, nullptr, &dsl) != VK_SUCCESS) {
            std::printf("VK13-DI-CANARY: FAIL (create the update-after-bind set layout)\n");
            goto teardown;
        }
        VkDescriptorPoolSize sizes[2]{};
        sizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        sizes[0].descriptorCount = 2;
        sizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        sizes[1].descriptorCount = kDeclaredMax;
        VkDescriptorPoolCreateInfo dpci{};
        dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        dpci.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
        dpci.maxSets = 1;
        dpci.poolSizeCount = 2;
        dpci.pPoolSizes = sizes;
        if (vkCreateDescriptorPool(device, &dpci, nullptr, &dpool) != VK_SUCCESS) {
            std::printf("VK13-DI-CANARY: FAIL (create the update-after-bind pool)\n");
            goto teardown;
        }
        const std::uint32_t var_count = kVarCount;
        VkDescriptorSetVariableDescriptorCountAllocateInfo vdcai{};
        vdcai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO;
        vdcai.descriptorSetCount = 1;
        vdcai.pDescriptorCounts = &var_count;
        VkDescriptorSetAllocateInfo dsai{};
        dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsai.pNext = &vdcai;
        dsai.descriptorPool = dpool;
        dsai.descriptorSetCount = 1;
        dsai.pSetLayouts = &dsl;
        VkDescriptorSet dset = VK_NULL_HANDLE;
        if (vkAllocateDescriptorSets(device, &dsai, &dset) != VK_SUCCESS) {
            std::printf("VK13-DI-CANARY: FAIL (allocate with variable count %u of declared %u)\n",
                        kVarCount, kDeclaredMax);
            goto teardown;
        }

        // --- Buffers (all HOST_VISIBLE|COHERENT): two UBOs, two sources, the destination, and
        // the readback staging buffer. ---
        VkPhysicalDeviceMemoryProperties mp{};
        vkGetPhysicalDeviceMemoryProperties(phys, &mp);
        const VkMemoryPropertyFlags want =
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        auto pick = [&](std::uint32_t bits) {
            for (std::uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
                if ((bits & (1u << i)) != 0 && (mp.memoryTypes[i].propertyFlags & want) == want) {
                    return i;
                }
            }
            return UINT32_MAX;
        };
        const VkDeviceSize bytes = kElems * sizeof(std::uint32_t);
        auto make_buffer = [&](VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer* buf,
                               VkDeviceMemory* mem) {
            VkBufferCreateInfo bci{};
            bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            bci.size = size;
            bci.usage = usage;
            bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            if (vkCreateBuffer(device, &bci, nullptr, buf) != VK_SUCCESS) {
                return false;
            }
            VkMemoryRequirements mr{};
            vkGetBufferMemoryRequirements(device, *buf, &mr);
            const std::uint32_t type = pick(mr.memoryTypeBits);
            if (type == UINT32_MAX) {
                return false;
            }
            VkMemoryAllocateInfo mai{};
            mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            mai.allocationSize = mr.size;
            mai.memoryTypeIndex = type;
            return vkAllocateMemory(device, &mai, nullptr, mem) == VK_SUCCESS &&
                   vkBindBufferMemory(device, *buf, *mem, 0) == VK_SUCCESS;
        };
        auto fill_u32 = [&](VkDeviceMemory mem, VkDeviceSize size,
                            std::uint32_t (*gen)(std::uint32_t)) {
            void* p = nullptr;
            if (vkMapMemory(device, mem, 0, VK_WHOLE_SIZE, 0, &p) != VK_SUCCESS) {
                return false;
            }
            for (VkDeviceSize i = 0; i < size / sizeof(std::uint32_t); ++i) {
                static_cast<std::uint32_t*>(p)[i] = gen(static_cast<std::uint32_t>(i));
            }
            vkUnmapMemory(device, mem);
            return true;
        };
        if (!make_buffer(16, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &buf_params0, &mem_params0) ||
            !make_buffer(16, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &buf_params1, &mem_params1) ||
            !make_buffer(bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &buf_src_a, &mem_src_a) ||
            !make_buffer(bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &buf_src_b, &mem_src_b) ||
            !make_buffer(bytes,
                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                         &buf_dst, &mem_dst) ||
            !make_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &buf_read, &mem_read)) {
            std::printf("VK13-DI-CANARY: FAIL (buffer bring-up)\n");
            goto teardown;
        }
        if (!fill_u32(mem_params0, 16, [](std::uint32_t) { return kDecoy; }) ||
            !fill_u32(mem_params1, 16, [](std::uint32_t) { return kAdd; }) ||
            !fill_u32(mem_src_a, bytes, [](std::uint32_t i) { return i + 1000u; }) || // decoy
            !fill_u32(mem_src_b, bytes, [](std::uint32_t i) { return i * 7 + 5; }) ||
            !fill_u32(mem_dst, bytes, [](std::uint32_t) { return 0u; }) ||
            !fill_u32(mem_read, bytes, [](std::uint32_t) { return 0u; })) { // poison
            std::printf("VK13-DI-CANARY: FAIL (buffer fill)\n");
            goto teardown;
        }

        VkShaderModuleCreateInfo smci{};
        smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        smci.codeSize = sizeof(kDiSpv);
        smci.pCode = kDiSpv;
        if (vkCreateShaderModule(device, &smci, nullptr, &module) != VK_SUCCESS) {
            std::printf("VK13-DI-CANARY: FAIL (bindless shader module)\n");
            goto teardown;
        }
        VkPushConstantRange push{};
        push.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        push.offset = 0;
        push.size = sizeof(PushBlock);
        VkPipelineLayoutCreateInfo plci{};
        plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        plci.setLayoutCount = 1;
        plci.pSetLayouts = &dsl;
        plci.pushConstantRangeCount = 1;
        plci.pPushConstantRanges = &push;
        if (vkCreatePipelineLayout(device, &plci, nullptr, &layout) != VK_SUCCESS) {
            goto teardown;
        }
        VkComputePipelineCreateInfo cpci{};
        cpci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        cpci.stage.module = module;
        cpci.stage.pName = "main";
        cpci.layout = layout;
        cpci.basePipelineIndex = -1;
        if (vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpci, nullptr, &pipeline) !=
            VK_SUCCESS) {
            std::printf("VK13-DI-CANARY: FAIL (compute pipeline with the nonuniform+runtime-array "
                        "shader)\n");
            goto teardown;
        }

        // --- THE crux ordering: RECORD against the still-unwritten set FIRST. ---
        VkCommandPoolCreateInfo cpci2{};
        cpci2.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        cpci2.queueFamilyIndex = 0;
        if (vkCreateCommandPool(device, &cpci2, nullptr, &cpool) != VK_SUCCESS) {
            goto teardown;
        }
        VkCommandBufferAllocateInfo cbai{};
        cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cbai.commandPool = cpool;
        cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbai.commandBufferCount = 1;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        if (vkAllocateCommandBuffers(device, &cbai, &cmd) != VK_SUCCESS) {
            goto teardown;
        }
        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &bi);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &dset, 0,
                                nullptr);
        PushBlock pb{};
        pb.src = kSrcSlot;
        pb.dst = kDstSlot;
        pb.pidx = kParamIdx;
        pb.scale = kScale;
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pb), &pb);
        vkCmdDispatch(cmd, kElems / 64, 1, 1);
        VkMemoryBarrier mb{};
        mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
        VkBufferCopy region{0, 0, bytes};
        vkCmdCopyBuffer(cmd, buf_dst, buf_read, 1, &region);
        vkEndCommandBuffer(cmd);
        std::printf("VK13-DI-CANARY: recorded against the UNWRITTEN update-after-bind set\n");

        // --- NOW write the descriptors (slots 0 and 2 of the SSBO array stay unwritten --
        // PARTIALLY_BOUND covers them). ---
        VkDescriptorBufferInfo dbi_params0{buf_params0, 0, VK_WHOLE_SIZE};
        VkDescriptorBufferInfo dbi_params1{buf_params1, 0, VK_WHOLE_SIZE};
        VkDescriptorBufferInfo dbi_src{buf_src_a, 0, VK_WHOLE_SIZE};
        VkDescriptorBufferInfo dbi_dst{buf_dst, 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet writes[4]{};
        for (VkWriteDescriptorSet& w : writes) {
            w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w.dstSet = dset;
            w.descriptorCount = 1;
        }
        writes[0].dstBinding = 0;
        writes[0].dstArrayElement = 0;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[0].pBufferInfo = &dbi_params0;
        writes[1].dstBinding = 0;
        writes[1].dstArrayElement = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[1].pBufferInfo = &dbi_params1;
        writes[2].dstBinding = 1;
        writes[2].dstArrayElement = kSrcSlot;
        writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[2].pBufferInfo = &dbi_src;
        writes[3].dstBinding = 1;
        writes[3].dstArrayElement = kDstSlot;
        writes[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[3].pBufferInfo = &dbi_dst;
        vkUpdateDescriptorSets(device, 4, writes, 0, nullptr);

        // --- Destroy the OLD source referent, repoint the slot to the NEW one (update-after-bind
        // must read the LIVE set at submit, not a stale bake of buf_src_a). ---
        vkDestroyBuffer(device, buf_src_a, nullptr);
        buf_src_a = VK_NULL_HANDLE;
        vkFreeMemory(device, mem_src_a, nullptr);
        mem_src_a = VK_NULL_HANDLE;
        VkDescriptorBufferInfo dbi_src_b{buf_src_b, 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet repoint{};
        repoint.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        repoint.dstSet = dset;
        repoint.dstBinding = 1;
        repoint.dstArrayElement = kSrcSlot;
        repoint.descriptorCount = 1;
        repoint.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        repoint.pBufferInfo = &dbi_src_b;
        vkUpdateDescriptorSets(device, 1, &repoint, 0, nullptr);
        std::printf("VK13-DI-CANARY: destroyed the old referent and repointed slot %u after "
                    "recording\n",
                    kSrcSlot);

        VkFenceCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        if (vkCreateFence(device, &fci, nullptr, &fence) != VK_SUCCESS) {
            goto teardown;
        }
        VkSubmitInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        if (vkQueueSubmit(queue, 1, &si, fence) != VK_SUCCESS) {
            std::printf("VK13-DI-CANARY: FAIL (vkQueueSubmit)\n");
            goto teardown;
        }
        if (vkWaitForFences(device, 1, &fence, VK_TRUE, ~0ull) != VK_SUCCESS) {
            std::printf("VK13-DI-CANARY: FAIL (vkWaitForFences)\n");
            goto teardown;
        }

        // --- Byte-exact from the NEW referent: dst[i] = (i*7+5)*kScale + kAdd + i. A stale bake
        // of buf_src_a (i+1000) or the decoy params[0] (99) cannot produce these values. ---
        void* read_ptr = nullptr;
        if (vkMapMemory(device, mem_read, 0, VK_WHOLE_SIZE, 0, &read_ptr) != VK_SUCCESS) {
            goto teardown;
        }
        const auto* out_words = static_cast<const std::uint32_t*>(read_ptr);
        std::uint32_t bad = 0;
        for (std::uint32_t i = 0; i < kElems; ++i) {
            const std::uint32_t want_v = (i * 7 + 5) * kScale + kAdd + i;
            if (out_words[i] != want_v) {
                if (bad < 4) {
                    std::printf("VK13-DI-CANARY: mismatch[%u] got=%u want=%u\n", i, out_words[i],
                                want_v);
                }
                ++bad;
            }
        }
        vkUnmapMemory(device, mem_read);
        if (bad != 0) {
            std::printf("VK13-DI-CANARY: FAIL (%u/%u values wrong -- the update-after-bind path "
                        "did not read the live set)\n",
                        bad, kElems);
            goto teardown;
        }
        std::printf("VK13-DI-CANARY: compute-through-bindless byte-exact (%u values)\n", kElems);
        std::printf("VK13-DI-CANARY: PASS (native lane serves the buffer-only descriptorIndexing "
                    "subset: served bits TRUE + aggregate/deferred FALSE, deferred enables reject, "
                    "the host answers layout support, and a dispatch recorded against an UNWRITTEN "
                    "update-after-bind set computed byte-exact from a referent repointed AFTER "
                    "recording, with partially-bound slots unwritten and the variable count below "
                    "its declared max)\n");
        rc = 0;
    }

teardown:
    if (device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device);
    }
    if (fence != VK_NULL_HANDLE) {
        vkDestroyFence(device, fence, nullptr);
    }
    if (cpool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(device, cpool, nullptr);
    }
    if (pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(device, pipeline, nullptr);
    }
    if (layout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device, layout, nullptr);
    }
    if (module != VK_NULL_HANDLE) {
        vkDestroyShaderModule(device, module, nullptr);
    }
    if (dpool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device, dpool, nullptr);
    }
    if (dsl != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device, dsl, nullptr);
    }
    for (VkBuffer b : {buf_params0, buf_params1, buf_src_a, buf_src_b, buf_dst, buf_read}) {
        if (b != VK_NULL_HANDLE) {
            vkDestroyBuffer(device, b, nullptr);
        }
    }
    for (VkDeviceMemory m : {mem_params0, mem_params1, mem_src_a, mem_src_b, mem_dst, mem_read}) {
        if (m != VK_NULL_HANDLE) {
            vkFreeMemory(device, m, nullptr);
        }
    }
    if (device != VK_NULL_HANDLE) {
        vkDestroyDevice(device, nullptr);
    }
    vkDestroyInstance(instance, nullptr);
    if (rc == 0) {
        std::printf("VK13-DI-CANARY: done\n");
    }
    return rc;
}
