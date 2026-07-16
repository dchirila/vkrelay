// vkrelay2 Vulkan 1.3 REQUIRED-FEATURE AUDIT canary: the
// machine-checked conformance invariant behind the honest-1.3 claim.
//
// Vulkan 1.3 support + hostQueryReset canaries prove individual served features; this one locks the
// WHOLE gate's promise: the relay must never report apiVersion >= 1.3 while a REQUIRED feature is
// advertise-then-failed. The one required feature the relay does not yet serve is multiview
// (required since Vulkan 1.1); it is not yet wired. So the load-bearing invariant is:
//
//     multiview is UNSERVED  =>  the device must report apiVersion < 1.3
//
// The canary reports the full required matrix (f10/f11/f12/f13 bits) for visibility, and enforces
// two things: (1) whenever the device reports apiVersion >= 1.3, EVERY required feature the
// `host_vk13_ready()` vouch covers must be reported TRUE (robustBufferAccess, multiview, the
// unconditional f12 rows, bufferDeviceAddress, the two memory-model rows, all f13 rows, and the two
// KHR-conditional rows when their alias is advertised); and (2) multiview must actually be SERVED
// at 1.3 -- probed by enabling it and attempting a viewMask render pass 2 (VK_ERROR_INITIALIZATION_
// FAILED today, VK_SUCCESS once multiview is served). It PASSES today (native lane honestly 1.2,
// multiview PENDING) and stays GREEN after multiview is served (1.3, multiview SERVED); it FAILS
// on a real regression -- a device that claims 1.3 while any required feature (multiview or
// otherwise) is masked FALSE or advertise-then-failed.
//
// Runs on EITHER lane. SKIPs cleanly when no ICD/worker stack is reachable.

#include <vulkan/vulkan.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {
// Attempt a viewMask (multiview) render pass 2 on a device that enabled multiview. Returns true iff
// the relay SERVED it (VK_SUCCESS); false on the current fail-closed rejection. Any created handle
// is destroyed.
//
// SCOPE: this is a lightweight TRIPWIRE for the invariant below -- it proves
// only that render-pass-2 view masks are admitted, NOT the full "served" surface. Full multiview
// support means viewMask through render-pass2 AND dynamic rendering AND per-view replay; before
// `kRelayServesMultiview` flips true, a DEDICATED multiview canary must exercise a real multiview
// RENDER across those surfaces (a partial RP2-create-only implementation must not be allowed to
// flip the gate). This canary's job is the version invariant (1.3 => served); the full serve-proof
// is a dedicated multiview canary's job.
bool multiview_render_pass_served(VkDevice device) {
    VkAttachmentDescription2 att{};
    att.sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2;
    att.format = VK_FORMAT_R8G8B8A8_UNORM;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkAttachmentReference2 ref{};
    ref.sType = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2;
    ref.attachment = 0;
    ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ref.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    VkSubpassDescription2 sp{};
    sp.sType = VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2;
    sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sp.viewMask = 0x3; // two views -- the multiview trigger (rejected until multiview is wired)
    sp.colorAttachmentCount = 1;
    sp.pColorAttachments = &ref;
    VkRenderPassCreateInfo2 rpci{};
    rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2;
    rpci.attachmentCount = 1;
    rpci.pAttachments = &att;
    rpci.subpassCount = 1;
    rpci.pSubpasses = &sp;
    VkRenderPass rp = VK_NULL_HANDLE;
    const VkResult r = vkCreateRenderPass2(device, &rpci, nullptr, &rp);
    if (r == VK_SUCCESS && rp != VK_NULL_HANDLE) {
        vkDestroyRenderPass(device, rp, nullptr);
        return true;
    }
    return false;
}
} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0); // never lose lines before an abort/crash

    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.apiVersion = VK_API_VERSION_1_3; // ask for 1.3; the relay answers with the honest version
    app.pApplicationName = "vkrelay2-vk13-required-audit-canary";
    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    VkInstance instance = VK_NULL_HANDLE;
    if (vkCreateInstance(&ici, nullptr, &instance) != VK_SUCCESS) {
        std::printf("VK13-AUDIT-CANARY: SKIP (vkCreateInstance failed -- no ICD/worker?)\n");
        return 0;
    }

    std::uint32_t pd_count = 0;
    vkEnumeratePhysicalDevices(instance, &pd_count, nullptr);
    if (pd_count == 0) {
        std::printf("VK13-AUDIT-CANARY: FAIL (no physical device)\n");
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
    const bool reports_13 = (maj > 1) || (maj == 1 && min >= 3);
    std::printf("VK13-AUDIT-CANARY: device '%s' apiVersion=%u.%u\n", props.deviceName, maj, min);

    // The required matrix, for visibility (f10 base + f11 + f12 + f13 rollups).
    VkPhysicalDeviceVulkan13Features f13{};
    f13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    VkPhysicalDeviceVulkan12Features f12{};
    f12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    f12.pNext = &f13;
    VkPhysicalDeviceVulkan11Features f11{};
    f11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    f11.pNext = &f12;
    VkPhysicalDeviceFeatures2 f2{};
    f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    f2.pNext = &f11;
    vkGetPhysicalDeviceFeatures2(phys, &f2);
    std::printf("VK13-AUDIT-CANARY: f10 robustBufferAccess=%d | f11 multiview=%d "
                "shaderDrawParameters=%d\n",
                f2.features.robustBufferAccess, f11.multiview, f11.shaderDrawParameters);
    std::printf(
        "VK13-AUDIT-CANARY: f12 samplerMirrorClampToEdge=%d imagelessFramebuffer=%d "
        "timelineSemaphore=%d hostQueryReset=%d bufferDeviceAddress=%d vulkanMemoryModel=%d\n",
        f12.samplerMirrorClampToEdge, f12.imagelessFramebuffer, f12.timelineSemaphore,
        f12.hostQueryReset, f12.bufferDeviceAddress, f12.vulkanMemoryModel);
    std::printf("VK13-AUDIT-CANARY: f13 dynamicRendering=%d synchronization2=%d maintenance4=%d "
                "inlineUniformBlock=%d\n",
                f13.dynamicRendering, f13.synchronization2, f13.maintenance4,
                f13.inlineUniformBlock);

    // Always-served spot check: robustBufferAccess is required at every version and forwarded
    // verbatim -- it must be TRUE on any device the relay presents.
    if (!f2.features.robustBufferAccess) {
        std::printf(
            "VK13-AUDIT-CANARY: FAIL (robustBufferAccess FALSE -- a required base feature)\n");
        vkDestroyInstance(instance, nullptr);
        return 1;
    }
    // The OPTIONAL f11/f12 members the relay does not serve must be masked FALSE on BOTH
    // lanes -- they rode the unmasked rollup as host-TRUE (an advertise-then-fail). A regression
    // that unmasks one reports TRUE here.
    {
        struct OptMask {
            const char* name;
            VkBool32 reported;
        };
        const OptMask masked_optional[] = {
            {"protectedMemory", f11.protectedMemory},
            {"samplerYcbcrConversion", f11.samplerYcbcrConversion},
            {"multiviewGeometryShader", f11.multiviewGeometryShader},
            {"multiviewTessellationShader", f11.multiviewTessellationShader},
            {"drawIndirectCount", f12.drawIndirectCount},
            {"shaderOutputViewportIndex", f12.shaderOutputViewportIndex},
            {"shaderOutputLayer", f12.shaderOutputLayer},
        };
        bool leak = false;
        for (const auto& m : masked_optional) {
            if (m.reported != VK_FALSE) {
                std::printf("VK13-AUDIT-CANARY: optional-unserved leaked TRUE: %s\n", m.name);
                leak = true;
            }
        }
        if (leak) {
            std::printf("VK13-AUDIT-CANARY: FAIL (an optional unserved f11/f12 member is reported "
                        "TRUE -- advertise-then-fail; the relay masks these FALSE)\n");
            vkDestroyInstance(instance, nullptr);
            return 1;
        }
        std::printf("VK13-AUDIT-CANARY: optional-unserved f11/f12 members masked FALSE\n");
    }
    // When the device reports >= 1.3 it must report EVERY required feature TRUE -- the SAME vouch
    // host_vk13_ready() enforces. A 1.3 claim with any masked-FALSE required
    // member is non-conformant, so the audit fails on it. The conditional-extension rows
    // (shaderDrawParameters, samplerMirrorClampToEdge) bind only when their KHR alias is advertised
    // -- enumerate the device extensions and apply the worker's exact condition.
    if (reports_13) {
        std::uint32_t ext_n = 0;
        vkEnumerateDeviceExtensionProperties(phys, nullptr, &ext_n, nullptr);
        std::vector<VkExtensionProperties> exts(ext_n);
        if (ext_n != 0) {
            vkEnumerateDeviceExtensionProperties(phys, nullptr, &ext_n, exts.data());
        }
        const auto has_ext = [&](const char* n) {
            for (const auto& e : exts) {
                if (std::strcmp(e.extensionName, n) == 0) {
                    return true;
                }
            }
            return false;
        };
        struct Req {
            const char* name;
            VkBool32 reported;
        };
        const Req required[] = {
            {"robustBufferAccess", f2.features.robustBufferAccess},
            {"multiview", f11.multiview},
            {"subgroupBroadcastDynamicId", f12.subgroupBroadcastDynamicId},
            {"imagelessFramebuffer", f12.imagelessFramebuffer},
            {"uniformBufferStandardLayout", f12.uniformBufferStandardLayout},
            {"shaderSubgroupExtendedTypes", f12.shaderSubgroupExtendedTypes},
            {"separateDepthStencilLayouts", f12.separateDepthStencilLayouts},
            {"timelineSemaphore", f12.timelineSemaphore},
            {"hostQueryReset", f12.hostQueryReset},
            {"bufferDeviceAddress", f12.bufferDeviceAddress},
            {"vulkanMemoryModel", f12.vulkanMemoryModel},
            {"vulkanMemoryModelDeviceScope", f12.vulkanMemoryModelDeviceScope},
            {"robustImageAccess", f13.robustImageAccess},
            {"inlineUniformBlock", f13.inlineUniformBlock},
            {"pipelineCreationCacheControl", f13.pipelineCreationCacheControl},
            {"privateData", f13.privateData},
            {"shaderDemoteToHelperInvocation", f13.shaderDemoteToHelperInvocation},
            {"shaderTerminateInvocation", f13.shaderTerminateInvocation},
            {"subgroupSizeControl", f13.subgroupSizeControl},
            {"computeFullSubgroups", f13.computeFullSubgroups},
            {"shaderZeroInitializeWorkgroupMemory", f13.shaderZeroInitializeWorkgroupMemory},
            {"shaderIntegerDotProduct", f13.shaderIntegerDotProduct},
            {"maintenance4", f13.maintenance4},
            {"dynamicRendering", f13.dynamicRendering},
            {"synchronization2", f13.synchronization2},
        };
        bool ok = true;
        for (const auto& r : required) {
            if (r.reported == VK_FALSE) {
                std::printf("VK13-AUDIT-CANARY: 1.3-required FALSE: %s\n", r.name);
                ok = false;
            }
        }
        if (has_ext(VK_KHR_SHADER_DRAW_PARAMETERS_EXTENSION_NAME) && !f11.shaderDrawParameters) {
            std::printf("VK13-AUDIT-CANARY: 1.3-required FALSE: shaderDrawParameters (KHR alias "
                        "advertised)\n");
            ok = false;
        }
        if (has_ext(VK_KHR_SAMPLER_MIRROR_CLAMP_TO_EDGE_EXTENSION_NAME) &&
            !f12.samplerMirrorClampToEdge) {
            std::printf(
                "VK13-AUDIT-CANARY: 1.3-required FALSE: samplerMirrorClampToEdge (KHR alias "
                "advertised)\n");
            ok = false;
        }
        if (!ok) {
            std::printf(
                "VK13-AUDIT-CANARY: FAIL (device reports 1.3 but a required feature is "
                "FALSE -- non-conformant, an advertise-then-fail the vouch must prevent)\n");
            vkDestroyInstance(instance, nullptr);
            return 1;
        }
    }

    // The multiview probe: multiview is reported TRUE (host pass-through). Enable it and see
    // whether the relay actually SERVES a viewMask render pass, or fail-closes it.
    bool multiview_served = false;
    if (f11.multiview) {
        std::uint32_t qf = 1;
        VkQueueFamilyProperties qfp{};
        vkGetPhysicalDeviceQueueFamilyProperties(phys, &qf, &qfp);
        const float prio = 1.0f;
        VkDeviceQueueCreateInfo qci{};
        qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qci.queueFamilyIndex = 0;
        qci.queueCount = 1;
        qci.pQueuePriorities = &prio;
        VkPhysicalDeviceVulkan11Features enable11{};
        enable11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
        enable11.multiview = VK_TRUE;
        VkDeviceCreateInfo dci{};
        dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        dci.pNext = &enable11;
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qci;
        VkDevice device = VK_NULL_HANDLE;
        if (vkCreateDevice(phys, &dci, nullptr, &device) == VK_SUCCESS) {
            multiview_served = multiview_render_pass_served(device);
            vkDestroyDevice(device, nullptr);
        }
    }
    std::printf("VK13-AUDIT-CANARY: multiview reported=%d served=%d (%s)\n", f11.multiview,
                multiview_served ? 1 : 0,
                multiview_served ? "SERVED" : "PENDING -- relay support must wire viewMask");

    // THE INVARIANT: the relay must never report apiVersion >= 1.3 while multiview (a required
    // feature) is advertise-then-failed. Unserved => the device must honestly stay below 1.3.
    if (reports_13 && f11.multiview && !multiview_served) {
        std::printf("VK13-AUDIT-CANARY: FAIL (device reports 1.3 while multiview is reported TRUE "
                    "but a viewMask render pass is rejected -- an advertise-then-fail of a "
                    "1.3-required feature)\n");
        vkDestroyInstance(instance, nullptr);
        return 1;
    }

    if (multiview_served) {
        std::printf("VK13-AUDIT-CANARY: PASS (multiview SERVED; the required matrix is honestly "
                    "backed at apiVersion %u.%u)\n",
                    maj, min);
    } else {
        std::printf(
            "VK13-AUDIT-CANARY: PASS (multiview PENDING and the relay honestly reports "
            "%u.%u < 1.3 -- the relay-served gate holds: no advertise-then-fail 1.3 claim)\n",
            maj, min);
    }
    vkDestroyInstance(instance, nullptr);
    std::printf("VK13-AUDIT-CANARY: done\n");
    return 0;
}
