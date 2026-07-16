// vkrelay2 Vulkan 1.3 support canary -- the honest apiVersion-1.3 device, end to end.
//
// A surfaceless native Vulkan program run through the real loader + the session-pinned vkrelay2
// ICD on the NATIVE lane (run_vk13_finale_smoke.sh --frontend vulkan13). It proves Vulkan 1.3
// support claim on the real GPU:
//   - the device reports apiVersion 1.3 (native lane; the worker vouched via vk13_ready) and the
//     instance reports >= 1.3;
//   - the f13 rollup reports every 1.3-REQUIRED feature TRUE (robustImageAccess,
//     inlineUniformBlock, pipelineCreationCacheControl, privateData, demote/terminate,
//     subgroupSizeControl, computeFullSubgroups, zeroInitWorkgroupMemory, integerDotProduct,
//     maintenance4, dynamicRendering, synchronization2) plus the required 1.2 memory-model bits,
//     while the two UNSERVED members (ASTC-HDR, inline-uniform-block UAB) stay FALSE;
//   - enabling an unserved member is VK_ERROR_FEATURE_NOT_PRESENT through the loader;
//   - vkGetPhysicalDeviceToolProperties answers 0 tools; private data round-trips
//     (create-slot/set/get/never-set-reads-0/destroy) on core names;
//   - maintenance4's vkGetDeviceBufferMemoryRequirements / vkGetDeviceImageMemoryRequirements
//     answer EXACTLY what creating the same buffer/image reports (host parity, no local fiction);
//   - pipelineCreationCacheControl: a compute create with FAIL_ON_PIPELINE_COMPILE_REQUIRED
//     answers VK_PIPELINE_COMPILE_REQUIRED (compilation is always required through the relay);
//   - subgroupSizeControl: the compute stage carries a RequiredSubgroupSize pNext (host min size)
//     when the host serves it for compute;
//   - inlineUniformBlock: a 64-byte INLINE_UNIFORM_BLOCK descriptor written via
//     VkWriteDescriptorSetInlineUniformBlock feeds a dispatch whose results read back byte-exact
//     THROUGH vkCmdCopyBuffer2 (copy_commands2);
//   - the core-name dynamic-state surface records + replays: all 12 EDS1 setters + the 3
//     core-1.3 EDS2 enables, by their CORE names, no extension enabled;
//   - dynamic rendering + synchronization2 run FEATURELESS-extension (core names, the rollup
//     feature only): vkCmdBeginRendering CLEARs an offscreen image (no draw, no render pass
//     object) and vkCmdPipelineBarrier2 carries every layout transition; the cleared pixels read
//     back exact through vkCmdCopyImageToBuffer2;
//   - vkCmdResolveImage2 resolves a CLEARed 4-sample image into a 1-sample image, read back
//     exact.
//
// Greppable markers on stdout; run_vk13_finale_smoke.sh gates on them. Skips cleanly (exit 0)
// when no ICD/worker stack is reachable; FAILs (nonzero) on a real regression.

#include <vulkan/vulkan.h>

#include "tests/iub_spv.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

namespace {
constexpr std::uint32_t kElems = 1024;  // / local_size 64 = 16 groups (one per IUB word)
constexpr std::uint32_t kIubBytes = 64; // 16 u32s
constexpr std::uint32_t kImgDim = 64;   // offscreen clear target
constexpr std::uint64_t kMagic = 0xC0FFEE;
} // namespace

int main() {
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.apiVersion = VK_API_VERSION_1_3;
    app.pApplicationName = "vkrelay2-vk13-finale-canary";
    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    VkInstance instance = VK_NULL_HANDLE;
    if (vkCreateInstance(&ici, nullptr, &instance) != VK_SUCCESS) {
        std::printf("VK13-FINALE-CANARY: SKIP (vkCreateInstance failed -- no ICD/worker?)\n");
        return 0;
    }

    std::uint32_t pd_count = 0;
    vkEnumeratePhysicalDevices(instance, &pd_count, nullptr);
    if (pd_count == 0) {
        std::printf("VK13-FINALE-CANARY: FAIL (no physical device)\n");
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
    std::printf("VK13-FINALE-CANARY: device '%s' apiVersion=%u.%u\n", props.deviceName, maj, min);
    if (!(maj == 1 && min == 3)) {
        // The 1.2-device leg pins the Vulkan 1.3 canary's HONEST-SURFACE negative: on a device
        // reporting 1.2 with no 1.3-family extension enabled, the core-1.3-only names AND the
        // promoted-extension alias spellings must not even RESOLVE
        // -- symbol availability is an observable capability surface (the record-time gates
        // stay as defense in depth behind it).
        std::uint32_t qfc = 1;
        VkQueueFamilyProperties qfp{};
        vkGetPhysicalDeviceQueueFamilyProperties(phys, &qfc, &qfp);
        const float prio12 = 1.0f;
        VkDeviceQueueCreateInfo q12{};
        q12.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        q12.queueFamilyIndex = 0;
        q12.queueCount = 1;
        q12.pQueuePriorities = &prio12;
        VkDeviceCreateInfo d12{};
        d12.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        d12.queueCreateInfoCount = 1;
        d12.pQueueCreateInfos = &q12;
        VkDevice dev12 = VK_NULL_HANDLE;
        if (vkCreateDevice(phys, &d12, nullptr, &dev12) == VK_SUCCESS) {
            const bool gated =
                vkGetDeviceProcAddr(dev12, "vkCmdCopyBuffer2") == nullptr &&
                vkGetDeviceProcAddr(dev12, "vkCmdBeginRendering") == nullptr &&
                vkGetDeviceProcAddr(dev12, "vkQueueSubmit2") == nullptr &&
                vkGetDeviceProcAddr(dev12, "vkCreatePrivateDataSlot") == nullptr &&
                vkGetDeviceProcAddr(dev12, "vkCmdSetRasterizerDiscardEnable") == nullptr &&
                vkGetDeviceProcAddr(dev12, "vkCmdPipelineBarrier2KHR") == nullptr; // ext not on
            const bool core_ok = vkGetDeviceProcAddr(dev12, "vkCmdCopyBuffer") != nullptr;
            std::printf("VK13-FINALE-CANARY: proc_gate_on_12_device=%d\n",
                        gated && core_ok ? 1 : 0);
            vkDestroyDevice(dev12, nullptr);
        }
        std::printf("VK13-FINALE-CANARY: FAIL (device apiVersion %u.%u, expected 1.3 -- on the "
                    "default lane this FAIL is the expected steering proof)\n",
                    maj, min);
        vkDestroyInstance(instance, nullptr);
        return 1;
    }

    // --- Feature exposure: the f13 rollup + the 1.2 memory-model members. ---
    VkPhysicalDeviceVulkan13Features f13{};
    f13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    VkPhysicalDeviceVulkan12Features f12{};
    f12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    f12.pNext = &f13;
    VkPhysicalDeviceFeatures2 feat2{};
    feat2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    feat2.pNext = &f12;
    vkGetPhysicalDeviceFeatures2(phys, &feat2);
    const VkBool32 served[] = {
        f13.robustImageAccess,
        f13.inlineUniformBlock,
        f13.pipelineCreationCacheControl,
        f13.privateData,
        f13.shaderDemoteToHelperInvocation,
        f13.shaderTerminateInvocation,
        f13.subgroupSizeControl,
        f13.computeFullSubgroups,
        f13.shaderZeroInitializeWorkgroupMemory,
        f13.shaderIntegerDotProduct,
        f13.maintenance4,
        f13.dynamicRendering,
        f13.synchronization2,
        f12.vulkanMemoryModel,
        f12.vulkanMemoryModelDeviceScope,
    };
    bool all_served = true;
    for (const VkBool32 v : served) {
        all_served = all_served && v != VK_FALSE;
    }
    const bool unserved_leak = f13.textureCompressionASTC_HDR != VK_FALSE ||
                               f13.descriptorBindingInlineUniformBlockUpdateAfterBind != VK_FALSE;
    std::printf("VK13-FINALE-CANARY: feature vk13_served=%d vk13_unserved_leak=%d\n",
                all_served ? 1 : 0, unserved_leak ? 1 : 0);
    if (!all_served || unserved_leak) {
        std::printf("VK13-FINALE-CANARY: FAIL (a 1.3-required feature reported FALSE, or an "
                    "unserved member leaked TRUE)\n");
        vkDestroyInstance(instance, nullptr);
        return 1;
    }

    // --- Tool properties: zero tools, honestly. ---
    std::uint32_t tools = 7;
    if (vkGetPhysicalDeviceToolProperties(phys, &tools, nullptr) != VK_SUCCESS || tools != 0) {
        std::printf("VK13-FINALE-CANARY: FAIL (vkGetPhysicalDeviceToolProperties != 0 tools)\n");
        vkDestroyInstance(instance, nullptr);
        return 1;
    }
    std::printf("VK13-FINALE-CANARY: tool_properties_count=0\n");

    // --- Subgroup-size-control properties (for the required-size pNext below). ---
    VkPhysicalDeviceSubgroupSizeControlProperties ssc_props{};
    ssc_props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_PROPERTIES;
    VkPhysicalDeviceProperties2 props2{};
    props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    props2.pNext = &ssc_props;
    vkGetPhysicalDeviceProperties2(phys, &props2);

    std::uint32_t qf_count = 1;
    VkQueueFamilyProperties qf{};
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &qf_count, &qf);
    if ((qf.queueFlags & (VK_QUEUE_COMPUTE_BIT | VK_QUEUE_GRAPHICS_BIT)) !=
        (VK_QUEUE_COMPUTE_BIT | VK_QUEUE_GRAPHICS_BIT)) {
        std::printf("VK13-FINALE-CANARY: SKIP (family 0 lacks COMPUTE+GRAPHICS)\n");
        vkDestroyInstance(instance, nullptr);
        return 0;
    }

    const float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = 0;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;

    // --- Negative create: an UNSERVED member (ASTC-HDR) must reject through the loader. ---
    {
        VkPhysicalDeviceVulkan13Features bad{};
        bad.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
        bad.textureCompressionASTC_HDR = VK_TRUE;
        VkDeviceCreateInfo dci{};
        dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        dci.pNext = &bad;
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qci;
        VkDevice rejected = VK_NULL_HANDLE;
        const VkResult r = vkCreateDevice(phys, &dci, nullptr, &rejected);
        std::printf("VK13-FINALE-CANARY: negative-create unserved=%d (want %d)\n", r,
                    VK_ERROR_FEATURE_NOT_PRESENT);
        if (r != VK_ERROR_FEATURE_NOT_PRESENT) {
            std::printf("VK13-FINALE-CANARY: FAIL (unserved enable did not reject)\n");
            if (rejected != VK_NULL_HANDLE) {
                vkDestroyDevice(rejected, nullptr);
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
    VkPrivateDataSlot pd_slot = VK_NULL_HANDLE;
    VkBuffer buf_out = VK_NULL_HANDLE, buf_copy = VK_NULL_HANDLE, buf_m4 = VK_NULL_HANDLE;
    VkBuffer buf_img_read = VK_NULL_HANDLE, buf_res_read = VK_NULL_HANDLE;
    VkDeviceMemory mem_out = VK_NULL_HANDLE, mem_copy = VK_NULL_HANDLE, mem_m4 = VK_NULL_HANDLE;
    VkDeviceMemory mem_img_read = VK_NULL_HANDLE, mem_res_read = VK_NULL_HANDLE;
    VkImage img_dr = VK_NULL_HANDLE, img_msaa = VK_NULL_HANDLE, img_res = VK_NULL_HANDLE;
    VkImageView view_dr = VK_NULL_HANDLE;
    VkDeviceMemory mem_img_dr = VK_NULL_HANDLE, mem_img_msaa = VK_NULL_HANDLE,
                   mem_img_res = VK_NULL_HANDLE;

    {
        // --- Vulkan 1.3 device: every served 1.3 feature via the f13 ROLLUP + the memory-model
        // bits via the standalone struct, all NESTED INSIDE a VkPhysicalDeviceFeatures2 wrapper
        // (the zink-style spelling). This pins the wrapper form end to end: the ICD's chain
        // capture walks the ONE flat pNext list the wrapper is part of, so the nested structs
        // must reach the worker and the host device -- a capture that missed wrapper-nested
        // structs would reject here with a scalar/chain mismatch. NO extensions. ---
        VkPhysicalDeviceVulkanMemoryModelFeatures mm{};
        mm.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES;
        mm.vulkanMemoryModel = VK_TRUE;
        mm.vulkanMemoryModelDeviceScope = VK_TRUE;
        mm.vulkanMemoryModelAvailabilityVisibilityChains =
            f12.vulkanMemoryModelAvailabilityVisibilityChains;
        VkPhysicalDeviceVulkan13Features enable13{};
        enable13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
        enable13.pNext = &mm;
        enable13.robustImageAccess = VK_TRUE;
        enable13.inlineUniformBlock = VK_TRUE;
        enable13.pipelineCreationCacheControl = VK_TRUE;
        enable13.privateData = VK_TRUE;
        enable13.shaderDemoteToHelperInvocation = VK_TRUE;
        enable13.shaderTerminateInvocation = VK_TRUE;
        enable13.subgroupSizeControl = VK_TRUE;
        enable13.computeFullSubgroups = VK_TRUE;
        enable13.shaderZeroInitializeWorkgroupMemory = VK_TRUE;
        enable13.shaderIntegerDotProduct = VK_TRUE;
        enable13.maintenance4 = VK_TRUE;
        enable13.dynamicRendering = VK_TRUE;
        enable13.synchronization2 = VK_TRUE;
        VkPhysicalDeviceFeatures2 wrap2{};
        wrap2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        wrap2.pNext = &enable13; // the wrapper spelling: features nested BEHIND Features2
        VkDeviceCreateInfo dci{};
        dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        dci.pNext = &wrap2;
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qci;
        if (vkCreateDevice(phys, &dci, nullptr, &device) != VK_SUCCESS) {
            std::printf("VK13-FINALE-CANARY: FAIL (vkCreateDevice with the served 1.3 set via "
                        "the Features2 wrapper spelling)\n");
            goto teardown;
        }
        vkGetDeviceQueue(device, 0, 0, &queue);
        // The honest-surface POSITIVE: on the 1.3 device the core-1.3
        // names resolve through the device-aware GetDeviceProcAddr.
        if (vkGetDeviceProcAddr(device, "vkCmdCopyBuffer2") == nullptr ||
            vkGetDeviceProcAddr(device, "vkCmdBeginRendering") == nullptr ||
            vkGetDeviceProcAddr(device, "vkQueueSubmit2") == nullptr ||
            vkGetDeviceProcAddr(device, "vkCreatePrivateDataSlot") == nullptr ||
            vkGetDeviceProcAddr(device, "vkCmdSetRasterizerDiscardEnable") == nullptr) {
            std::printf("VK13-FINALE-CANARY: FAIL (a core-1.3 name did not resolve on the 1.3 "
                        "device)\n");
            goto teardown;
        }
        std::printf("VK13-FINALE-CANARY: proc_gate_on_13_device=1\n");

        // --- Private data (core names, ICD-local store). ---
        VkPrivateDataSlotCreateInfo pdci{};
        pdci.sType = VK_STRUCTURE_TYPE_PRIVATE_DATA_SLOT_CREATE_INFO;
        if (vkCreatePrivateDataSlot(device, &pdci, nullptr, &pd_slot) != VK_SUCCESS) {
            std::printf("VK13-FINALE-CANARY: FAIL (vkCreatePrivateDataSlot)\n");
            goto teardown;
        }
        if (vkSetPrivateData(device, VK_OBJECT_TYPE_DEVICE, reinterpret_cast<std::uint64_t>(device),
                             pd_slot, kMagic) != VK_SUCCESS) {
            std::printf("VK13-FINALE-CANARY: FAIL (vkSetPrivateData)\n");
            goto teardown;
        }
        std::uint64_t got = 0;
        vkGetPrivateData(device, VK_OBJECT_TYPE_DEVICE, reinterpret_cast<std::uint64_t>(device),
                         pd_slot, &got);
        std::uint64_t never = 1;
        vkGetPrivateData(device, VK_OBJECT_TYPE_BUFFER, 0x1234, pd_slot, &never);
        std::printf("VK13-FINALE-CANARY: private_data roundtrip=%d never_set_zero=%d\n",
                    got == kMagic ? 1 : 0, never == 0 ? 1 : 0);
        if (got != kMagic || never != 0) {
            std::printf("VK13-FINALE-CANARY: FAIL (private data did not round-trip)\n");
            goto teardown;
        }

        VkPhysicalDeviceMemoryProperties mp{};
        vkGetPhysicalDeviceMemoryProperties(phys, &mp);
        const VkMemoryPropertyFlags want =
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        auto pick = [&](std::uint32_t bits, VkMemoryPropertyFlags need) {
            for (std::uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
                if ((bits & (1u << i)) != 0 && (mp.memoryTypes[i].propertyFlags & need) == need) {
                    return i;
                }
            }
            return UINT32_MAX;
        };
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
            const std::uint32_t type = pick(mr.memoryTypeBits, want);
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
        // Image memory: the relay's memory model serves HOST_VISIBLE|HOST_COHERENT or
        // DEVICE_LOCAL-ONLY types (a combined ReBAR type is rejected) -- pick accordingly.
        auto pick_image_type = [&](std::uint32_t bits) {
            for (std::uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
                const VkMemoryPropertyFlags f = mp.memoryTypes[i].propertyFlags;
                if ((bits & (1u << i)) != 0 && (f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0 &&
                    (f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == 0) {
                    return i;
                }
            }
            return pick(bits, want);
        };
        auto make_image = [&](std::uint32_t samples, VkImageUsageFlags usage, VkImage* img,
                              VkDeviceMemory* mem) {
            VkImageCreateInfo im{};
            im.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            im.imageType = VK_IMAGE_TYPE_2D;
            im.format = VK_FORMAT_R8G8B8A8_UNORM;
            im.extent = {kImgDim, kImgDim, 1};
            im.mipLevels = 1;
            im.arrayLayers = 1;
            im.samples = static_cast<VkSampleCountFlagBits>(samples);
            im.tiling = VK_IMAGE_TILING_OPTIMAL;
            im.usage = usage;
            im.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            im.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            if (vkCreateImage(device, &im, nullptr, img) != VK_SUCCESS) {
                return false;
            }
            VkMemoryRequirements mr{};
            vkGetImageMemoryRequirements(device, *img, &mr);
            const std::uint32_t type = pick_image_type(mr.memoryTypeBits);
            if (type == UINT32_MAX) {
                return false;
            }
            VkMemoryAllocateInfo mai{};
            mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            mai.allocationSize = mr.size;
            mai.memoryTypeIndex = type;
            return vkAllocateMemory(device, &mai, nullptr, mem) == VK_SUCCESS &&
                   vkBindImageMemory(device, *img, *mem, 0) == VK_SUCCESS;
        };

        // --- maintenance4: the create-info query must match the created object exactly. ---
        {
            VkBufferCreateInfo bci{};
            bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            bci.size = 4096;
            bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
            bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            VkDeviceBufferMemoryRequirements dbr{};
            dbr.sType = VK_STRUCTURE_TYPE_DEVICE_BUFFER_MEMORY_REQUIREMENTS;
            dbr.pCreateInfo = &bci;
            VkMemoryRequirements2 q{};
            q.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2;
            vkGetDeviceBufferMemoryRequirements(device, &dbr, &q);
            if (!make_buffer(bci.size, bci.usage, &buf_m4, &mem_m4)) {
                std::printf("VK13-FINALE-CANARY: FAIL (m4 parity buffer bring-up)\n");
                goto teardown;
            }
            VkMemoryRequirements created{};
            vkGetBufferMemoryRequirements(device, buf_m4, &created);
            const bool parity = q.memoryRequirements.size == created.size &&
                                q.memoryRequirements.alignment == created.alignment &&
                                q.memoryRequirements.memoryTypeBits == created.memoryTypeBits;
            std::printf("VK13-FINALE-CANARY: maintenance4 buffer_parity=%d\n", parity ? 1 : 0);
            if (!parity || q.memoryRequirements.size == 0) {
                std::printf("VK13-FINALE-CANARY: FAIL (maintenance4 buffer query != created)\n");
                goto teardown;
            }
        }

        // --- The IUB + SSBO layout, pool (with the IUB pool pNext), set, and writes. ---
        VkDescriptorSetLayoutBinding binds[2]{};
        binds[0].binding = 0;
        binds[0].descriptorType = VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK;
        binds[0].descriptorCount = kIubBytes; // BYTES
        binds[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        binds[1].binding = 1;
        binds[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        binds[1].descriptorCount = 1;
        binds[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        VkDescriptorSetLayoutCreateInfo dslci{};
        dslci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        dslci.bindingCount = 2;
        dslci.pBindings = binds;
        if (vkCreateDescriptorSetLayout(device, &dslci, nullptr, &dsl) != VK_SUCCESS) {
            std::printf("VK13-FINALE-CANARY: FAIL (inline-uniform-block set layout)\n");
            goto teardown;
        }
        VkDescriptorPoolInlineUniformBlockCreateInfo iub_pool{};
        iub_pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_INLINE_UNIFORM_BLOCK_CREATE_INFO;
        iub_pool.maxInlineUniformBlockBindings = 1;
        VkDescriptorPoolSize sizes[2]{};
        sizes[0].type = VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK;
        sizes[0].descriptorCount = kIubBytes; // a BYTE budget
        sizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        sizes[1].descriptorCount = 1;
        VkDescriptorPoolCreateInfo dpci{};
        dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        dpci.pNext = &iub_pool;
        dpci.maxSets = 1;
        dpci.poolSizeCount = 2;
        dpci.pPoolSizes = sizes;
        if (vkCreateDescriptorPool(device, &dpci, nullptr, &dpool) != VK_SUCCESS) {
            std::printf("VK13-FINALE-CANARY: FAIL (inline-uniform-block pool)\n");
            goto teardown;
        }
        VkDescriptorSetAllocateInfo dsai{};
        dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsai.descriptorPool = dpool;
        dsai.descriptorSetCount = 1;
        dsai.pSetLayouts = &dsl;
        VkDescriptorSet dset = VK_NULL_HANDLE;
        if (vkAllocateDescriptorSets(device, &dsai, &dset) != VK_SUCCESS) {
            std::printf("VK13-FINALE-CANARY: FAIL (allocate the IUB set)\n");
            goto teardown;
        }
        if (!make_buffer(kElems * 4,
                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                             VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, // BindVertexBuffers2 exercise
                         &buf_out, &mem_out) ||
            !make_buffer(kElems * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &buf_copy, &mem_copy) ||
            !make_buffer(kImgDim * kImgDim * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &buf_img_read,
                         &mem_img_read) ||
            !make_buffer(kImgDim * kImgDim * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &buf_res_read,
                         &mem_res_read)) {
            std::printf("VK13-FINALE-CANARY: FAIL (buffer bring-up)\n");
            goto teardown;
        }
        std::uint32_t iub_words[16];
        for (std::uint32_t j = 0; j < 16; ++j) {
            iub_words[j] = j * 3 + 7;
        }
        VkWriteDescriptorSetInlineUniformBlock iub_write{};
        iub_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_INLINE_UNIFORM_BLOCK;
        iub_write.dataSize = kIubBytes;
        iub_write.pData = iub_words;
        VkDescriptorBufferInfo out_info{buf_out, 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet writes[2]{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].pNext = &iub_write;
        writes[0].dstSet = dset;
        writes[0].dstBinding = 0;
        writes[0].dstArrayElement = 0; // byte offset
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK;
        writes[0].descriptorCount = kIubBytes; // byte count
        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = dset;
        writes[1].dstBinding = 1;
        writes[1].dstArrayElement = 0;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[1].descriptorCount = 1;
        writes[1].pBufferInfo = &out_info;
        vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);

        // --- The compute pipeline: COMPILE_REQUIRED first (cache control), then for real with
        // the RequiredSubgroupSize pNext where the host serves it for compute. ---
        VkShaderModuleCreateInfo smci{};
        smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        smci.codeSize = sizeof(kIubSpv);
        smci.pCode = kIubSpv;
        if (vkCreateShaderModule(device, &smci, nullptr, &module) != VK_SUCCESS) {
            std::printf("VK13-FINALE-CANARY: FAIL (IUB shader module)\n");
            goto teardown;
        }
        VkPipelineLayoutCreateInfo plci{};
        plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        plci.setLayoutCount = 1;
        plci.pSetLayouts = &dsl;
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
        cpci.flags = VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT;
        VkPipeline refused = VK_NULL_HANDLE;
        const VkResult ccr =
            vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpci, nullptr, &refused);
        std::printf("VK13-FINALE-CANARY: cache_control compile_required=%d null_handle=%d\n",
                    ccr == VK_PIPELINE_COMPILE_REQUIRED ? 1 : 0, refused == VK_NULL_HANDLE ? 1 : 0);
        if (ccr != VK_PIPELINE_COMPILE_REQUIRED || refused != VK_NULL_HANDLE) {
            std::printf("VK13-FINALE-CANARY: FAIL (FAIL_ON_PIPELINE_COMPILE_REQUIRED did not "
                        "answer VK_PIPELINE_COMPILE_REQUIRED)\n");
            goto teardown;
        }
        cpci.flags = 0;
        VkPipelineShaderStageRequiredSubgroupSizeCreateInfo rss{};
        if ((ssc_props.requiredSubgroupSizeStages & VK_SHADER_STAGE_COMPUTE_BIT) != 0 &&
            ssc_props.minSubgroupSize != 0) {
            rss.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO;
            rss.requiredSubgroupSize = ssc_props.minSubgroupSize;
            cpci.stage.pNext = &rss;
            std::printf("VK13-FINALE-CANARY: subgroup required_size=%u\n",
                        ssc_props.minSubgroupSize);
        }
        if (vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpci, nullptr, &pipeline) !=
            VK_SUCCESS) {
            std::printf("VK13-FINALE-CANARY: FAIL (compute pipeline with the subgroup-size "
                        "pNext)\n");
            goto teardown;
        }

        // --- Offscreen images: the DR clear target, the 4x MSAA resolve source + 1x dest. ---
        if (!make_image(1, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                        &img_dr, &mem_img_dr) ||
            !make_image(4, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                        &img_msaa, &mem_img_msaa) ||
            !make_image(1, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                        &img_res, &mem_img_res)) {
            std::printf("VK13-FINALE-CANARY: FAIL (image bring-up)\n");
            goto teardown;
        }
        VkImageViewCreateInfo vci{};
        vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vci.image = img_dr;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = VK_FORMAT_R8G8B8A8_UNORM;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (vkCreateImageView(device, &vci, nullptr, &view_dr) != VK_SUCCESS) {
            goto teardown;
        }

        VkCommandPoolCreateInfo cplci{};
        cplci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        cplci.queueFamilyIndex = 0;
        if (vkCreateCommandPool(device, &cplci, nullptr, &cpool) != VK_SUCCESS) {
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

        // --- The core-name dynamic-state surface: all 12 EDS1 setters + the 3 core-1.3 EDS2
        // enables, recorded featureless (a 1.3 device serves them unconditionally). ---
        vkCmdSetCullMode(cmd, VK_CULL_MODE_NONE);
        vkCmdSetFrontFace(cmd, VK_FRONT_FACE_COUNTER_CLOCKWISE);
        vkCmdSetPrimitiveTopology(cmd, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
        vkCmdSetDepthTestEnable(cmd, VK_FALSE);
        vkCmdSetDepthWriteEnable(cmd, VK_FALSE);
        vkCmdSetDepthCompareOp(cmd, VK_COMPARE_OP_ALWAYS);
        const VkViewport vp{0.0f, 0.0f, 64.0f, 64.0f, 0.0f, 1.0f};
        vkCmdSetViewportWithCount(cmd, 1, &vp);
        const VkRect2D sc{{0, 0}, {64, 64}};
        vkCmdSetScissorWithCount(cmd, 1, &sc);
        vkCmdSetDepthBoundsTestEnable(cmd, VK_FALSE);
        vkCmdSetStencilTestEnable(cmd, VK_FALSE);
        vkCmdSetStencilOp(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, VK_STENCIL_OP_KEEP,
                          VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_COMPARE_OP_ALWAYS);
        VkDeviceSize bvb_off = 0;
        VkDeviceSize bvb_size = kElems * 4; // explicit (validation: pSizes must be <= buffer size)
        VkDeviceSize bvb_stride = 16;
        vkCmdBindVertexBuffers2(cmd, 0, 1, &buf_out, &bvb_off, &bvb_size, &bvb_stride);
        vkCmdSetRasterizerDiscardEnable(cmd, VK_FALSE);
        vkCmdSetDepthBiasEnable(cmd, VK_FALSE);
        vkCmdSetPrimitiveRestartEnable(cmd, VK_FALSE);

        // --- The IUB dispatch + the copy2 buffer readback. ---
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &dset, 0,
                                nullptr);
        vkCmdDispatch(cmd, kElems / 64, 1, 1);
        // sync2 core name carries the compute->copy dependency.
        VkMemoryBarrier2 mb2{};
        mb2.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
        mb2.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        mb2.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
        mb2.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        mb2.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        VkDependencyInfo dep{};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.memoryBarrierCount = 1;
        dep.pMemoryBarriers = &mb2;
        vkCmdPipelineBarrier2(cmd, &dep);
        VkBufferCopy2 bc2{};
        bc2.sType = VK_STRUCTURE_TYPE_BUFFER_COPY_2;
        bc2.size = kElems * 4;
        VkCopyBufferInfo2 cbi2{};
        cbi2.sType = VK_STRUCTURE_TYPE_COPY_BUFFER_INFO_2;
        cbi2.srcBuffer = buf_out;
        cbi2.dstBuffer = buf_copy;
        cbi2.regionCount = 1;
        cbi2.pRegions = &bc2;
        vkCmdCopyBuffer2(cmd, &cbi2);

        // --- Featureless-core dynamic rendering: CLEAR the offscreen image, no draw. ---
        auto image_barrier2 = [&](VkImage img, VkImageLayout from, VkImageLayout to,
                                  VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
                                  VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access) {
            VkImageMemoryBarrier2 ib{};
            ib.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            ib.srcStageMask = src_stage;
            ib.srcAccessMask = src_access;
            ib.dstStageMask = dst_stage;
            ib.dstAccessMask = dst_access;
            ib.oldLayout = from;
            ib.newLayout = to;
            ib.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            ib.image = img;
            ib.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            VkDependencyInfo di{};
            di.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            di.imageMemoryBarrierCount = 1;
            di.pImageMemoryBarriers = &ib;
            vkCmdPipelineBarrier2(cmd, &di);
        };
        image_barrier2(img_dr, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                       VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                       VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                       VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        VkRenderingAttachmentInfo att{};
        att.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        att.imageView = view_dr;
        att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att.clearValue.color = {{1.0f, 0.0f, 1.0f, 1.0f}}; // magenta, exact in UNORM
        VkRenderingInfo ri{};
        ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        ri.renderArea = {{0, 0}, {kImgDim, kImgDim}};
        ri.layerCount = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &att;
        vkCmdBeginRendering(cmd, &ri);
        vkCmdEndRendering(cmd);
        image_barrier2(
            img_dr, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        VkBufferImageCopy2 bic2{};
        bic2.sType = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2;
        bic2.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        bic2.imageExtent = {kImgDim, kImgDim, 1};
        VkCopyImageToBufferInfo2 citb2{};
        citb2.sType = VK_STRUCTURE_TYPE_COPY_IMAGE_TO_BUFFER_INFO_2;
        citb2.srcImage = img_dr;
        citb2.srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        citb2.dstBuffer = buf_img_read;
        citb2.regionCount = 1;
        citb2.pRegions = &bic2;
        vkCmdCopyImageToBuffer2(cmd, &citb2);

        // --- vkCmdResolveImage2: clear the 4x image green, resolve to 1x, read back. ---
        image_barrier2(img_msaa, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_2_CLEAR_BIT,
                       VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VkClearColorValue green{};
        green.float32[1] = 1.0f;
        green.float32[3] = 1.0f;
        VkImageSubresourceRange whole{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cmd, img_msaa, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &green, 1,
                             &whole);
        image_barrier2(img_msaa, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_CLEAR_BIT,
                       VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_RESOLVE_BIT,
                       VK_ACCESS_2_TRANSFER_READ_BIT);
        image_barrier2(img_res, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_2_RESOLVE_BIT,
                       VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VkImageResolve2 res_region{};
        res_region.sType = VK_STRUCTURE_TYPE_IMAGE_RESOLVE_2;
        res_region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        res_region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        res_region.extent = {kImgDim, kImgDim, 1};
        VkResolveImageInfo2 rii2{};
        rii2.sType = VK_STRUCTURE_TYPE_RESOLVE_IMAGE_INFO_2;
        rii2.srcImage = img_msaa;
        rii2.srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        rii2.dstImage = img_res;
        rii2.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        rii2.regionCount = 1;
        rii2.pRegions = &res_region;
        vkCmdResolveImage2(cmd, &rii2);
        image_barrier2(img_res, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_RESOLVE_BIT,
                       VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
                       VK_ACCESS_2_TRANSFER_READ_BIT);
        VkCopyImageToBufferInfo2 res_read = citb2;
        res_read.srcImage = img_res;
        res_read.dstBuffer = buf_res_read;
        vkCmdCopyImageToBuffer2(cmd, &res_read);

        vkEndCommandBuffer(cmd);
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
            std::printf("VK13-FINALE-CANARY: FAIL (vkQueueSubmit)\n");
            goto teardown;
        }
        if (vkWaitForFences(device, 1, &fence, VK_TRUE, ~0ull) != VK_SUCCESS) {
            std::printf("VK13-FINALE-CANARY: FAIL (vkWaitForFences)\n");
            goto teardown;
        }

        // --- Asserts: IUB dispatch via copy2; DR clear pixels; resolve pixels. ---
        void* p = nullptr;
        if (vkMapMemory(device, mem_copy, 0, VK_WHOLE_SIZE, 0, &p) != VK_SUCCESS) {
            goto teardown;
        }
        std::uint32_t bad = 0;
        for (std::uint32_t i = 0; i < kElems; ++i) {
            // out[i] = vals[workgroup] + i, workgroup-uniform IUB indexing (see iub_spv.h: the
            // divergent-index variant is miscompiled by the AMD Windows driver, proven by a
            // native no-relay repro; delivery of all 16 words is what the relay must prove).
            const std::uint32_t want_v = (i / 64) * 3 + 7 + i;
            const std::uint32_t got_v = static_cast<const std::uint32_t*>(p)[i];
            if (got_v != want_v) {
                if (bad < 4) {
                    std::printf("VK13-FINALE-CANARY: iub mismatch[%u] got=%u want=%u\n", i, got_v,
                                want_v);
                }
                ++bad;
            }
        }
        vkUnmapMemory(device, mem_copy);
        std::printf("VK13-FINALE-CANARY: iub_dispatch_byte_exact=%d (%u values via "
                    "vkCmdCopyBuffer2)\n",
                    bad == 0 ? 1 : 0, kElems);
        if (bad != 0) {
            goto teardown;
        }
        if (vkMapMemory(device, mem_img_read, 0, VK_WHOLE_SIZE, 0, &p) != VK_SUCCESS) {
            goto teardown;
        }
        std::uint32_t bad_px = 0;
        for (std::uint32_t i = 0; i < kImgDim * kImgDim; ++i) {
            if (static_cast<const std::uint32_t*>(p)[i] != 0xFFFF00FFu) { // ABGR magenta
                ++bad_px;
            }
        }
        vkUnmapMemory(device, mem_img_read);
        std::printf("VK13-FINALE-CANARY: core_dynamic_rendering_clear_exact=%d\n",
                    bad_px == 0 ? 1 : 0);
        if (bad_px != 0) {
            std::printf("VK13-FINALE-CANARY: FAIL (%u wrong pixels from the featureless-core "
                        "vkCmdBeginRendering clear)\n",
                        bad_px);
            goto teardown;
        }
        if (vkMapMemory(device, mem_res_read, 0, VK_WHOLE_SIZE, 0, &p) != VK_SUCCESS) {
            goto teardown;
        }
        std::uint32_t bad_res = 0;
        for (std::uint32_t i = 0; i < kImgDim * kImgDim; ++i) {
            if (static_cast<const std::uint32_t*>(p)[i] != 0xFF00FF00u) { // ABGR green
                ++bad_res;
            }
        }
        vkUnmapMemory(device, mem_res_read);
        std::printf("VK13-FINALE-CANARY: resolve2_exact=%d\n", bad_res == 0 ? 1 : 0);
        if (bad_res != 0) {
            std::printf("VK13-FINALE-CANARY: FAIL (%u wrong pixels from vkCmdResolveImage2)\n",
                        bad_res);
            goto teardown;
        }
        std::printf("VK13-FINALE-CANARY: PASS (an honest apiVersion-1.3 device: required "
                    "features served, unserved members masked + rejected, tool "
                    "properties/private data/maintenance4/cache control/subgroup size answered, "
                    "an inline-uniform-block dispatch, the core-name dynamic-state surface, "
                    "featureless-core dynamic rendering + synchronization2, copy_commands2, and "
                    "vkCmdResolveImage2 all byte-exact on the real GPU)\n");
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
    if (view_dr != VK_NULL_HANDLE) {
        vkDestroyImageView(device, view_dr, nullptr);
    }
    for (VkImage im : {img_dr, img_msaa, img_res}) {
        if (im != VK_NULL_HANDLE) {
            vkDestroyImage(device, im, nullptr);
        }
    }
    if (pd_slot != VK_NULL_HANDLE) {
        vkDestroyPrivateDataSlot(device, pd_slot, nullptr);
    }
    for (VkBuffer b : {buf_out, buf_copy, buf_m4, buf_img_read, buf_res_read}) {
        if (b != VK_NULL_HANDLE) {
            vkDestroyBuffer(device, b, nullptr);
        }
    }
    for (VkDeviceMemory m : {mem_out, mem_copy, mem_m4, mem_img_read, mem_res_read, mem_img_dr,
                             mem_img_msaa, mem_img_res}) {
        if (m != VK_NULL_HANDLE) {
            vkFreeMemory(device, m, nullptr);
        }
    }
    if (device != VK_NULL_HANDLE) {
        vkDestroyDevice(device, nullptr);
    }
    vkDestroyInstance(instance, nullptr);
    if (rc == 0) {
        std::printf("VK13-FINALE-CANARY: done\n");
    }
    return rc;
}
