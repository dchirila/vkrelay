// vkrelay2 cube canary.
//
// The on-screen artifact for the LITERAL-vkcube shape -- the deterministic, in-repo counterpart to
// LunarG's vkcube (which run_vkcube.sh drives through the relay as the public on-screen gate). A
// native Vulkan program that creates an xcb window, then drives the whole spine: a BUFFERLESS
// spinning cube whose geometry lives INSIDE the uniform buffer (mvp mat4 + 36 positions + 36 attrs,
// indexed by gl_VertexIndex -- zero vertex bindings, draw of 36); a 2-binding descriptor set (0
// UNIFORM_BUFFER/VERTEX, 1 COMBINED_IMAGE_SAMPLER/FRAGMENT); a D16_UNORM depth attachment; and a
// sampled R8G8B8A8_UNORM texture uploaded via vkcube's RUNTIME path choice (linear-mapped if the
// driver advertises linearTilingFeatures & SAMPLED, else staging). Per frame it rewrites only the
// MVP (the spin) and never explicitly flushes -- the coherent-flush-at-submit sweep ships the
// bytes.
//
// Run through the real loader with VK_ICD_FILENAMES pinned to our ICD (the launcher does that), the
// spinning textured cube appears in a real Win32 window on the Windows desktop via worker-present.
// Prints greppable markers to stderr (the boundary smoke asserts the chain). Returns non-zero on a
// real failure; exits 0 with a "skipped" note if there is no X display / no usable Vulkan device.

#include <vulkan/vulkan.h>
#include <vulkan/vulkan_xcb.h>

#include <xcb/xcb.h>

#include "tests/cube_geometry.h"
#include "tests/cube_spv.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <vector>

namespace {

constexpr int kFrames = 60;
constexpr std::uint32_t kExtent = 256;
constexpr int kTex = 16; // checkerboard texture side

void log(const char* msg) {
    std::fprintf(stderr, "vkrelay2-cube: %s\n", msg);
}

bool check(VkResult r, const char* what) {
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
        std::fprintf(stderr, "vkrelay2-cube: %s failed (VkResult %d)\n", what, static_cast<int>(r));
        return false;
    }
    return true;
}

void sleep_ms(long ms) {
    timespec ts{ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&ts, nullptr);
}

VkShaderModule make_shader(VkDevice device, const std::uint32_t* code, std::uint32_t bytes) {
    VkShaderModuleCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = bytes;
    ci.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    vkCreateShaderModule(device, &ci, nullptr, &m);
    return m;
}

// Picks a memory type allowed by `bits` whose flags include all of `want`, or UINT32_MAX.
std::uint32_t pick_memory_type(const VkPhysicalDeviceMemoryProperties& mprops, std::uint32_t bits,
                               VkMemoryPropertyFlags want) {
    for (std::uint32_t i = 0; i < mprops.memoryTypeCount; ++i) {
        if ((bits & (1u << i)) != 0 && (mprops.memoryTypes[i].propertyFlags & want) == want) {
            return i;
        }
    }
    return UINT32_MAX;
}

// Fills `dst` (kTex*kTex*4 logical pixels) with a checkerboard, row `y` starting at byte `row_off`.
void checker_row(unsigned char* dst, std::size_t row_off, int y) {
    for (int x = 0; x < kTex; ++x) {
        const std::size_t o = row_off + static_cast<std::size_t>(x) * 4;
        const bool on = ((x ^ y) & 1) != 0;
        dst[o + 0] = on ? 0xff : 0x20;
        dst[o + 1] = 0x40;
        dst[o + 2] = on ? 0x20 : 0xff;
        dst[o + 3] = 0xff;
    }
}

} // namespace

int main() {
    xcb_connection_t* xcb = xcb_connect(nullptr, nullptr);
    if (xcb == nullptr || xcb_connection_has_error(xcb)) {
        log("skipped (no X display)");
        return 0;
    }
    const xcb_setup_t* setup = xcb_get_setup(xcb);
    xcb_screen_t* screen = xcb_setup_roots_iterator(setup).data;
    xcb_window_t window = xcb_generate_id(xcb);
    xcb_create_window(xcb, XCB_COPY_FROM_PARENT, window, screen->root, 0, 0, kExtent, kExtent, 0,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, 0, nullptr);
    xcb_map_window(xcb, window);
    xcb_flush(xcb);
    log("xcb window created");

    const char* inst_exts[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_XCB_SURFACE_EXTENSION_NAME};
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.apiVersion = VK_API_VERSION_1_1;
    app.pApplicationName = "vkrelay2-cube";
    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = 2;
    ici.ppEnabledExtensionNames = inst_exts;
    VkInstance instance = VK_NULL_HANDLE;
    if (vkCreateInstance(&ici, nullptr, &instance) != VK_SUCCESS) {
        log("skipped (vkCreateInstance failed -- no ICD/worker?)");
        xcb_disconnect(xcb);
        return 0;
    }
    log("instance created");

    int rc = 1; // pessimistic; 0 only on a clean run
    std::uint32_t pd_count = 0;
    vkEnumeratePhysicalDevices(instance, &pd_count, nullptr);
    if (pd_count == 0) {
        log("no physical device");
        vkDestroyInstance(instance, nullptr);
        xcb_disconnect(xcb);
        return 1;
    }
    std::vector<VkPhysicalDevice> phys_devs(pd_count);
    vkEnumeratePhysicalDevices(instance, &pd_count, phys_devs.data());
    VkPhysicalDevice phys = phys_devs[0];
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(phys, &props);
    std::fprintf(stderr, "vkrelay2-cube: device '%s'\n", props.deviceName);

    const float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = 0;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    const char* dev_exts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = dev_exts;

    // All handles declared up front so the goto teardown stays in their scope.
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkCommandBuffer upload_cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkRenderPass render_pass = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkShaderModule vert = VK_NULL_HANDLE;
    VkShaderModule frag = VK_NULL_HANDLE;
    VkBuffer ubo = VK_NULL_HANDLE;
    VkDeviceMemory ubo_mem = VK_NULL_HANDLE;
    void* ubo_mapped = nullptr;
    VkImage tex_image = VK_NULL_HANDLE;
    VkDeviceMemory tex_mem = VK_NULL_HANDLE;
    VkImageView tex_view = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    VkBuffer staging_buf = VK_NULL_HANDLE; // staging path only
    VkDeviceMemory staging_mem = VK_NULL_HANDLE;
    VkImage depth_image = VK_NULL_HANDLE;
    VkDeviceMemory depth_mem = VK_NULL_HANDLE;
    VkImageView depth_view = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkDescriptorPool desc_pool = VK_NULL_HANDLE;
    VkDescriptorSet desc_set = VK_NULL_HANDLE;
    std::vector<VkImageView> views;
    std::vector<VkFramebuffer> framebuffers;
    bool path_is_linear = false;

    if (!check(vkCreateDevice(phys, &dci, nullptr, &device), "vkCreateDevice")) {
        goto teardown;
    }
    log("device created");
    vkGetDeviceQueue(device, 0, 0, &queue);

    {
        VkPhysicalDeviceMemoryProperties mprops{};
        vkGetPhysicalDeviceMemoryProperties(phys, &mprops);
        const VkMemoryPropertyFlags coherent =
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

        // --- The texture: vkcube's runtime path choice from the real format properties ---
        VkFormatProperties fmt_props{};
        vkGetPhysicalDeviceFormatProperties(phys, VK_FORMAT_R8G8B8A8_UNORM, &fmt_props);
        const bool linear_supported =
            (fmt_props.linearTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0;

        // Create the texture image: LINEAR/PREINITIALIZED if we can map it, else OPTIMAL/UNDEFINED.
        auto make_tex_image = [&](VkImageTiling tiling, VkImageUsageFlags usage,
                                  VkImageLayout initial) {
            VkImageCreateInfo ic{};
            ic.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            ic.imageType = VK_IMAGE_TYPE_2D;
            ic.format = VK_FORMAT_R8G8B8A8_UNORM;
            ic.extent = {kTex, kTex, 1};
            ic.mipLevels = 1;
            ic.arrayLayers = 1;
            ic.samples = VK_SAMPLE_COUNT_1_BIT;
            ic.tiling = tiling;
            ic.usage = usage;
            ic.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            ic.initialLayout = initial;
            VkImage img = VK_NULL_HANDLE;
            vkCreateImage(device, &ic, nullptr, &img);
            return img;
        };

        if (linear_supported) {
            tex_image = make_tex_image(VK_IMAGE_TILING_LINEAR, VK_IMAGE_USAGE_SAMPLED_BIT,
                                       VK_IMAGE_LAYOUT_PREINITIALIZED);
            if (tex_image == VK_NULL_HANDLE) {
                log("vkCreateImage(texture, linear) failed");
                goto teardown;
            }
            VkMemoryRequirements treq{};
            vkGetImageMemoryRequirements(device, tex_image, &treq);
            const std::uint32_t ttype = pick_memory_type(mprops, treq.memoryTypeBits, coherent);
            if (ttype != UINT32_MAX) {
                path_is_linear = true;
                VkMemoryAllocateInfo tmai{};
                tmai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
                tmai.allocationSize = treq.size;
                tmai.memoryTypeIndex = ttype;
                if (!check(vkAllocateMemory(device, &tmai, nullptr, &tex_mem),
                           "vkAllocateMemory(texture)") ||
                    !check(vkBindImageMemory(device, tex_image, tex_mem, 0),
                           "vkBindImageMemory(texture)")) {
                    goto teardown;
                }
                // Write the checkerboard directly into the mapped image at the real rowPitch.
                VkImageSubresource sub{};
                sub.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                VkSubresourceLayout sl{};
                vkGetImageSubresourceLayout(device, tex_image, &sub, &sl);
                void* mapped = nullptr;
                if (!check(vkMapMemory(device, tex_mem, 0, VK_WHOLE_SIZE, 0, &mapped),
                           "vkMapMemory(texture)") ||
                    mapped == nullptr) {
                    goto teardown;
                }
                auto* base = static_cast<unsigned char*>(mapped) + sl.offset;
                for (int y = 0; y < kTex; ++y) {
                    checker_row(base, static_cast<std::size_t>(sl.rowPitch) * y, y);
                }
                vkUnmapMemory(device, tex_mem);
            } else {
                // No host-visible type for a linear image; fall back to staging.
                vkDestroyImage(device, tex_image, nullptr);
                tex_image = VK_NULL_HANDLE;
            }
        }

        if (!path_is_linear) {
            tex_image = make_tex_image(VK_IMAGE_TILING_OPTIMAL,
                                       VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                       VK_IMAGE_LAYOUT_UNDEFINED);
            if (tex_image == VK_NULL_HANDLE) {
                log("vkCreateImage(texture, optimal) failed");
                goto teardown;
            }
            VkMemoryRequirements treq{};
            vkGetImageMemoryRequirements(device, tex_image, &treq);
            const std::uint32_t ttype =
                pick_memory_type(mprops, treq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            if (ttype == UINT32_MAX) {
                log("no device-local memory type for the texture");
                goto teardown;
            }
            VkMemoryAllocateInfo tmai{};
            tmai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            tmai.allocationSize = treq.size;
            tmai.memoryTypeIndex = ttype;
            if (!check(vkAllocateMemory(device, &tmai, nullptr, &tex_mem),
                       "vkAllocateMemory(texture)") ||
                !check(vkBindImageMemory(device, tex_image, tex_mem, 0),
                       "vkBindImageMemory(texture)")) {
                goto teardown;
            }
            // The staging buffer: TRANSFER_SRC host-visible coherent, filled with the checkerboard.
            const VkDeviceSize tex_bytes = static_cast<VkDeviceSize>(kTex) * kTex * 4;
            VkBufferCreateInfo sbi{};
            sbi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            sbi.size = tex_bytes;
            sbi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
            sbi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            if (!check(vkCreateBuffer(device, &sbi, nullptr, &staging_buf),
                       "vkCreateBuffer(staging)")) {
                goto teardown;
            }
            VkMemoryRequirements sreq{};
            vkGetBufferMemoryRequirements(device, staging_buf, &sreq);
            const std::uint32_t stype = pick_memory_type(mprops, sreq.memoryTypeBits, coherent);
            if (stype == UINT32_MAX) {
                log("no host-visible coherent memory type for the staging buffer");
                goto teardown;
            }
            VkMemoryAllocateInfo smai{};
            smai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            smai.allocationSize = sreq.size;
            smai.memoryTypeIndex = stype;
            if (!check(vkAllocateMemory(device, &smai, nullptr, &staging_mem),
                       "vkAllocateMemory(staging)") ||
                !check(vkBindBufferMemory(device, staging_buf, staging_mem, 0),
                       "vkBindBufferMemory(staging)")) {
                goto teardown;
            }
            void* mapped = nullptr;
            if (!check(vkMapMemory(device, staging_mem, 0, VK_WHOLE_SIZE, 0, &mapped),
                       "vkMapMemory(staging)") ||
                mapped == nullptr) {
                goto teardown;
            }
            auto* base = static_cast<unsigned char*>(mapped);
            for (int y = 0; y < kTex; ++y) {
                checker_row(base, static_cast<std::size_t>(y) * kTex * 4, y);
            }
            vkUnmapMemory(device, staging_mem);
        }
        std::fprintf(stderr, "vkrelay2-cube: texture upload path = %s\n",
                     path_is_linear ? "linear-mapped" : "staging");

        VkImageViewCreateInfo tvci{};
        tvci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        tvci.image = tex_image;
        tvci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        tvci.format = VK_FORMAT_R8G8B8A8_UNORM;
        tvci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        tvci.subresourceRange.levelCount = 1;
        tvci.subresourceRange.layerCount = 1;
        if (!check(vkCreateImageView(device, &tvci, nullptr, &tex_view),
                   "vkCreateImageView(tex)")) {
            goto teardown;
        }

        VkSamplerCreateInfo smci{};
        smci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        smci.magFilter = VK_FILTER_NEAREST;
        smci.minFilter = VK_FILTER_NEAREST;
        smci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        smci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        smci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        smci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (!check(vkCreateSampler(device, &smci, nullptr, &sampler), "vkCreateSampler")) {
            goto teardown;
        }
        log("texture + sampler created");

        // --- The depth buffer: D16_UNORM, OPTIMAL, device-local ---
        VkImageCreateInfo dic{};
        dic.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        dic.imageType = VK_IMAGE_TYPE_2D;
        dic.format = VK_FORMAT_D16_UNORM;
        dic.extent = {kExtent, kExtent, 1};
        dic.mipLevels = 1;
        dic.arrayLayers = 1;
        dic.samples = VK_SAMPLE_COUNT_1_BIT;
        dic.tiling = VK_IMAGE_TILING_OPTIMAL;
        dic.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        dic.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        dic.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (!check(vkCreateImage(device, &dic, nullptr, &depth_image), "vkCreateImage(depth)")) {
            goto teardown;
        }
        VkMemoryRequirements dreq{};
        vkGetImageMemoryRequirements(device, depth_image, &dreq);
        const std::uint32_t dtype =
            pick_memory_type(mprops, dreq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (dtype == UINT32_MAX) {
            log("no device-local memory type for the depth image");
            goto teardown;
        }
        VkMemoryAllocateInfo dmai{};
        dmai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        dmai.allocationSize = dreq.size;
        dmai.memoryTypeIndex = dtype;
        if (!check(vkAllocateMemory(device, &dmai, nullptr, &depth_mem),
                   "vkAllocateMemory(depth)") ||
            !check(vkBindImageMemory(device, depth_image, depth_mem, 0),
                   "vkBindImageMemory(depth)")) {
            goto teardown;
        }
        VkImageViewCreateInfo dvci{};
        dvci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        dvci.image = depth_image;
        dvci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        dvci.format = VK_FORMAT_D16_UNORM;
        dvci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        dvci.subresourceRange.levelCount = 1;
        dvci.subresourceRange.layerCount = 1;
        if (!check(vkCreateImageView(device, &dvci, nullptr, &depth_view),
                   "vkCreateImageView(depth)")) {
            goto teardown;
        }
        log("depth image + view created");

        // --- The UBO: mvp + 36 positions + 36 attrs (1216 bytes), persistently mapped ---
        VkBufferCreateInfo ubci{};
        ubci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        ubci.size = vkr::cube_geom::kUboBytes;
        ubci.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        ubci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (!check(vkCreateBuffer(device, &ubci, nullptr, &ubo), "vkCreateBuffer(ubo)")) {
            goto teardown;
        }
        VkMemoryRequirements ureq{};
        vkGetBufferMemoryRequirements(device, ubo, &ureq);
        const std::uint32_t utype = pick_memory_type(mprops, ureq.memoryTypeBits, coherent);
        if (utype == UINT32_MAX) {
            log("no host-visible coherent memory type for the UBO");
            goto teardown;
        }
        VkMemoryAllocateInfo umai{};
        umai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        umai.allocationSize = ureq.size;
        umai.memoryTypeIndex = utype;
        if (!check(vkAllocateMemory(device, &umai, nullptr, &ubo_mem), "vkAllocateMemory(ubo)") ||
            !check(vkBindBufferMemory(device, ubo, ubo_mem, 0), "vkBindBufferMemory(ubo)") ||
            !check(vkMapMemory(device, ubo_mem, 0, VK_WHOLE_SIZE, 0, &ubo_mapped),
                   "vkMapMemory(ubo)") ||
            ubo_mapped == nullptr) {
            goto teardown;
        }
        {
            float ubo_floats[vkr::cube_geom::kUboFloats];
            vkr::cube_geom::build_ubo(0.0f, ubo_floats); // mvp + geometry, frame 0
            std::memcpy(ubo_mapped, ubo_floats, vkr::cube_geom::kUboBytes);
        }
        log("uniform buffer mapped (persistent, no explicit flush)");

        // --- The 2-binding descriptor set ---
        VkDescriptorSetLayoutBinding binds[2]{};
        binds[0].binding = 0;
        binds[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        binds[0].descriptorCount = 1;
        binds[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        binds[1].binding = 1;
        binds[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binds[1].descriptorCount = 1;
        binds[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo dslci{};
        dslci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        dslci.bindingCount = 2;
        dslci.pBindings = binds;
        if (!check(vkCreateDescriptorSetLayout(device, &dslci, nullptr, &set_layout),
                   "vkCreateDescriptorSetLayout")) {
            goto teardown;
        }
        VkDescriptorPoolSize dps[2]{};
        dps[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        dps[0].descriptorCount = 1;
        dps[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        dps[1].descriptorCount = 1;
        VkDescriptorPoolCreateInfo dpci{};
        dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        dpci.maxSets = 1;
        dpci.poolSizeCount = 2;
        dpci.pPoolSizes = dps;
        if (!check(vkCreateDescriptorPool(device, &dpci, nullptr, &desc_pool),
                   "vkCreateDescriptorPool")) {
            goto teardown;
        }
        VkDescriptorSetAllocateInfo dsai{};
        dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsai.descriptorPool = desc_pool;
        dsai.descriptorSetCount = 1;
        dsai.pSetLayouts = &set_layout;
        if (!check(vkAllocateDescriptorSets(device, &dsai, &desc_set),
                   "vkAllocateDescriptorSets")) {
            goto teardown;
        }
        VkDescriptorBufferInfo dbi{};
        dbi.buffer = ubo;
        dbi.offset = 0;
        dbi.range = VK_WHOLE_SIZE;
        VkDescriptorImageInfo dii{};
        dii.sampler = sampler;
        dii.imageView = tex_view;
        dii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkWriteDescriptorSet wds[2]{};
        wds[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wds[0].dstSet = desc_set;
        wds[0].dstBinding = 0;
        wds[0].descriptorCount = 1;
        wds[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        wds[0].pBufferInfo = &dbi;
        wds[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wds[1].dstSet = desc_set;
        wds[1].dstBinding = 1;
        wds[1].descriptorCount = 1;
        wds[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        wds[1].pImageInfo = &dii;
        vkUpdateDescriptorSets(device, 2, wds, 0, nullptr);
        log("descriptor set updated (UBO + combined-image-sampler)");

        // --- Surface / swapchain / color views ---
        VkXcbSurfaceCreateInfoKHR sci{};
        sci.sType = VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR;
        sci.connection = xcb;
        sci.window = window;
        if (!check(vkCreateXcbSurfaceKHR(instance, &sci, nullptr, &surface),
                   "vkCreateXcbSurfaceKHR")) {
            goto teardown;
        }
        log("surface created");

        VkSurfaceCapabilitiesKHR caps{};
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, surface, &caps);
        std::uint32_t fmt_count = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &fmt_count, nullptr);
        std::vector<VkSurfaceFormatKHR> formats(fmt_count);
        vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &fmt_count, formats.data());
        std::uint32_t pm_count = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(phys, surface, &pm_count, nullptr);
        if (fmt_count == 0 || pm_count == 0) {
            log("surface reports no formats/present modes");
            goto teardown;
        }
        VkSurfaceFormatKHR chosen = formats[0];
        for (const auto& f : formats) {
            if (f.format == VK_FORMAT_B8G8R8A8_UNORM &&
                f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                chosen = f;
                break;
            }
        }
        std::uint32_t want_imgs = caps.minImageCount < 2 ? 2 : caps.minImageCount;
        if (caps.maxImageCount != 0 && want_imgs > caps.maxImageCount) {
            want_imgs = caps.maxImageCount;
        }
        VkSwapchainCreateInfoKHR scci{};
        scci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        scci.surface = surface;
        scci.minImageCount = want_imgs;
        scci.imageFormat = chosen.format;
        scci.imageColorSpace = chosen.colorSpace;
        scci.imageExtent = {kExtent, kExtent};
        scci.imageArrayLayers = 1;
        scci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        scci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        scci.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
        scci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        scci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        scci.clipped = VK_TRUE;
        if (!check(vkCreateSwapchainKHR(device, &scci, nullptr, &swapchain),
                   "vkCreateSwapchainKHR")) {
            goto teardown;
        }
        log("swapchain created");

        std::uint32_t img_count = 0;
        vkGetSwapchainImagesKHR(device, swapchain, &img_count, nullptr);
        std::vector<VkImage> images(img_count);
        vkGetSwapchainImagesKHR(device, swapchain, &img_count, images.data());
        for (VkImage image : images) {
            VkImageViewCreateInfo ivci{};
            ivci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            ivci.image = image;
            ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
            ivci.format = chosen.format;
            ivci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            ivci.subresourceRange.levelCount = 1;
            ivci.subresourceRange.layerCount = 1;
            VkImageView view = VK_NULL_HANDLE;
            if (!check(vkCreateImageView(device, &ivci, nullptr, &view), "vkCreateImageView")) {
                goto teardown;
            }
            views.push_back(view);
        }

        vert = make_shader(device, vkr::cube_spv::kVert, vkr::cube_spv::kVertBytes);
        frag = make_shader(device, vkr::cube_spv::kFrag, vkr::cube_spv::kFragBytes);
        if (vert == VK_NULL_HANDLE || frag == VK_NULL_HANDLE) {
            log("vkCreateShaderModule failed");
            goto teardown;
        }
        log("shaders created");

        // --- Render pass: color attachment 0 + depth attachment 1 ---
        VkAttachmentDescription atts[2]{};
        atts[0].format = chosen.format;
        atts[0].samples = VK_SAMPLE_COUNT_1_BIT;
        atts[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        atts[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        atts[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        atts[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        atts[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        atts[0].finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        atts[1].format = VK_FORMAT_D16_UNORM;
        atts[1].samples = VK_SAMPLE_COUNT_1_BIT;
        atts[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        atts[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        atts[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        atts[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        atts[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        atts[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        VkAttachmentReference color_ref{};
        color_ref.attachment = 0;
        color_ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        VkAttachmentReference depth_ref{};
        depth_ref.attachment = 1;
        depth_ref.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &color_ref;
        subpass.pDepthStencilAttachment = &depth_ref;
        VkRenderPassCreateInfo rpci{};
        rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rpci.attachmentCount = 2;
        rpci.pAttachments = atts;
        rpci.subpassCount = 1;
        rpci.pSubpasses = &subpass;
        if (!check(vkCreateRenderPass(device, &rpci, nullptr, &render_pass),
                   "vkCreateRenderPass")) {
            goto teardown;
        }
        for (VkImageView view : views) {
            VkImageView fb_atts[2] = {view, depth_view};
            VkFramebufferCreateInfo fbci{};
            fbci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            fbci.renderPass = render_pass;
            fbci.attachmentCount = 2;
            fbci.pAttachments = fb_atts;
            fbci.width = kExtent;
            fbci.height = kExtent;
            fbci.layers = 1;
            VkFramebuffer fb = VK_NULL_HANDLE;
            if (!check(vkCreateFramebuffer(device, &fbci, nullptr, &fb), "vkCreateFramebuffer")) {
                goto teardown;
            }
            framebuffers.push_back(fb);
        }
        log("render pass + framebuffers created");

        // --- Pipeline (bufferless, depth-stencil, cull NONE) ---
        VkPipelineLayoutCreateInfo plci{};
        plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        plci.setLayoutCount = 1;
        plci.pSetLayouts = &set_layout;
        if (!check(vkCreatePipelineLayout(device, &plci, nullptr, &layout),
                   "vkCreatePipelineLayout")) {
            goto teardown;
        }
        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vert;
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = frag;
        stages[1].pName = "main";
        VkPipelineVertexInputStateCreateInfo vi{}; // bufferless: zero bindings/attributes
        vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        VkPipelineInputAssemblyStateCreateInfo ia{};
        ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vp{};
        vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        vp.viewportCount = 1;
        vp.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs{};
        rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = VK_CULL_MODE_NONE; // depth sorts the cube; winding never hides a face
        rs.frontFace = VK_FRONT_FACE_CLOCKWISE;
        rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms{};
        ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo dss{};
        dss.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        dss.depthTestEnable = VK_TRUE;
        dss.depthWriteEnable = VK_TRUE;
        dss.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
        VkPipelineColorBlendAttachmentState cba{};
        cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                             VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo cb{};
        cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        cb.attachmentCount = 1;
        cb.pAttachments = &cba;
        VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo ds{};
        ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        ds.dynamicStateCount = 2;
        ds.pDynamicStates = dyn;
        VkGraphicsPipelineCreateInfo gpci{};
        gpci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        gpci.stageCount = 2;
        gpci.pStages = stages;
        gpci.pVertexInputState = &vi;
        gpci.pInputAssemblyState = &ia;
        gpci.pViewportState = &vp;
        gpci.pRasterizationState = &rs;
        gpci.pMultisampleState = &ms;
        gpci.pDepthStencilState = &dss;
        gpci.pColorBlendState = &cb;
        gpci.pDynamicState = &ds;
        gpci.layout = layout;
        gpci.renderPass = render_pass;
        gpci.subpass = 0;
        gpci.basePipelineIndex = -1;
        if (!check(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gpci, nullptr, &pipeline),
                   "vkCreateGraphicsPipelines")) {
            goto teardown;
        }
        log("graphics pipeline created");

        // --- Command pool + buffers + fence ---
        VkCommandPoolCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = 0;
        if (!check(vkCreateCommandPool(device, &pci, nullptr, &pool), "vkCreateCommandPool")) {
            goto teardown;
        }
        VkCommandBuffer cbs[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
        VkCommandBufferAllocateInfo cbai{};
        cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cbai.commandPool = pool;
        cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbai.commandBufferCount = 2;
        if (!check(vkAllocateCommandBuffers(device, &cbai, cbs), "vkAllocateCommandBuffers")) {
            goto teardown;
        }
        upload_cmd = cbs[0];
        cmd = cbs[1];
        VkFenceCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        if (!check(vkCreateFence(device, &fci, nullptr, &fence), "vkCreateFence")) {
            goto teardown;
        }

        // --- One-time texture upload: transition (+ copy on the staging path) to SHADER_READ_ONLY.
        {
            VkCommandBufferBeginInfo bi{};
            bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vkBeginCommandBuffer(upload_cmd, &bi);
            VkImageSubresourceRange range{};
            range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            range.levelCount = 1;
            range.layerCount = 1;
            VkImageMemoryBarrier barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = tex_image;
            barrier.subresourceRange = range;
            if (path_is_linear) {
                // PREINITIALIZED -> SHADER_READ_ONLY (host writes already in the mapped image).
                barrier.oldLayout = VK_IMAGE_LAYOUT_PREINITIALIZED;
                barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                barrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
                barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                vkCmdPipelineBarrier(upload_cmd, VK_PIPELINE_STAGE_HOST_BIT,
                                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0,
                                     nullptr, 1, &barrier);
            } else {
                // UNDEFINED -> TRANSFER_DST -> (copy) -> SHADER_READ_ONLY.
                barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                barrier.srcAccessMask = 0;
                barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                vkCmdPipelineBarrier(upload_cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                     VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                                     &barrier);
                VkBufferImageCopy region{};
                region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                region.imageSubresource.layerCount = 1;
                region.imageExtent = {kTex, kTex, 1};
                vkCmdCopyBufferToImage(upload_cmd, staging_buf, tex_image,
                                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
                barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                vkCmdPipelineBarrier(upload_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0,
                                     nullptr, 1, &barrier);
            }
            vkEndCommandBuffer(upload_cmd);
            VkSubmitInfo subi{};
            subi.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            subi.commandBufferCount = 1;
            subi.pCommandBuffers = &upload_cmd;
            if (!check(vkQueueSubmit(queue, 1, &subi, fence), "vkQueueSubmit(upload)")) {
                goto teardown;
            }
            vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
            vkResetFences(device, 1, &fence);
        }
        log("texture uploaded to SHADER_READ_ONLY");

        int presented = 0;
        for (int frame = 0; frame < kFrames; ++frame) {
            // Rewrite ONLY the per-frame MVP (the spin); geometry stays. No explicit flush -- the
            // coherent-flush-at-submit sweep ships it.
            float spin[16];
            vkr::cube_geom::mvp(static_cast<float>(frame) * 0.08f, spin);
            std::memcpy(ubo_mapped, spin, sizeof(spin));

            VkSemaphore sem_acq = VK_NULL_HANDLE;
            VkSemaphore sem_done = VK_NULL_HANDLE;
            VkSemaphoreCreateInfo semci{};
            semci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
            if (!check(vkCreateSemaphore(device, &semci, nullptr, &sem_acq), "vkCreateSemaphore") ||
                !check(vkCreateSemaphore(device, &semci, nullptr, &sem_done),
                       "vkCreateSemaphore")) {
                goto teardown;
            }

            std::uint32_t image_index = 0;
            VkResult acq = vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, sem_acq,
                                                 VK_NULL_HANDLE, &image_index);
            if (acq == VK_ERROR_OUT_OF_DATE_KHR) {
                // Fixed-size canary: a dropped frame would let the animation gate pass without
                // spinning, so treat OUT_OF_DATE as a failure (the UBO-canary rule).
                log("acquire returned OUT_OF_DATE (unexpected for a fixed-size canary)");
                vkDestroySemaphore(device, sem_acq, nullptr);
                vkDestroySemaphore(device, sem_done, nullptr);
                goto teardown;
            }
            if (!check(acq, "vkAcquireNextImageKHR")) {
                vkDestroySemaphore(device, sem_acq, nullptr);
                vkDestroySemaphore(device, sem_done, nullptr);
                goto teardown;
            }

            VkCommandBufferBeginInfo bi{};
            bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vkBeginCommandBuffer(cmd, &bi);
            VkClearValue clears[2]{};
            clears[0].color.float32[0] = 0.05f;
            clears[0].color.float32[1] = 0.05f;
            clears[0].color.float32[2] = 0.08f;
            clears[0].color.float32[3] = 1.0f;
            clears[1].depthStencil.depth = 1.0f;
            VkRenderPassBeginInfo rpbi{};
            rpbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
            rpbi.renderPass = render_pass;
            rpbi.framebuffer = framebuffers[image_index];
            rpbi.renderArea.extent = {kExtent, kExtent};
            rpbi.clearValueCount = 2;
            rpbi.pClearValues = clears;
            vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            VkViewport viewport{};
            viewport.width = static_cast<float>(kExtent);
            viewport.height = static_cast<float>(kExtent);
            viewport.maxDepth = 1.0f;
            vkCmdSetViewport(cmd, 0, 1, &viewport);
            VkRect2D scissor{};
            scissor.extent = {kExtent, kExtent};
            vkCmdSetScissor(cmd, 0, 1, &scissor);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &desc_set,
                                    0, nullptr);
            vkCmdDraw(cmd, vkr::cube_geom::kCubeVertices, 1, 0, 0); // 36, bufferless
            vkCmdEndRenderPass(cmd);
            vkEndCommandBuffer(cmd);

            const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            VkSubmitInfo subi{};
            subi.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            subi.waitSemaphoreCount = 1;
            subi.pWaitSemaphores = &sem_acq;
            subi.pWaitDstStageMask = &wait_stage;
            subi.commandBufferCount = 1;
            subi.pCommandBuffers = &cmd;
            subi.signalSemaphoreCount = 1;
            subi.pSignalSemaphores = &sem_done;
            if (!check(vkQueueSubmit(queue, 1, &subi, fence), "vkQueueSubmit")) {
                vkDestroySemaphore(device, sem_acq, nullptr);
                vkDestroySemaphore(device, sem_done, nullptr);
                goto teardown;
            }
            vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
            vkResetFences(device, 1, &fence);

            VkPresentInfoKHR pi{};
            pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
            pi.waitSemaphoreCount = 1;
            pi.pWaitSemaphores = &sem_done;
            pi.swapchainCount = 1;
            pi.pSwapchains = &swapchain;
            pi.pImageIndices = &image_index;
            VkResult pres = vkQueuePresentKHR(queue, &pi);
            vkDestroySemaphore(device, sem_acq, nullptr);
            vkDestroySemaphore(device, sem_done, nullptr);
            if (pres == VK_SUCCESS || pres == VK_SUBOPTIMAL_KHR) {
                ++presented;
                if (frame == 0 || frame == kFrames - 1) {
                    std::fprintf(stderr, "vkrelay2-cube: presented frame %d\n", frame);
                }
            } else {
                std::fprintf(stderr,
                             "vkrelay2-cube: present did not succeed on frame %d (VkResult %d)\n",
                             frame, static_cast<int>(pres));
            }
            sleep_ms(33);
        }
        // The proof is the per-frame spinning cube: success requires EVERY frame to present.
        rc = (presented == kFrames) ? 0 : 1;
        std::fprintf(stderr, "vkrelay2-cube: presented %d frame(s)\n", presented);
    }

teardown:
    if (pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(device, pipeline, nullptr);
    }
    if (layout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device, layout, nullptr);
    }
    if (desc_pool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device, desc_pool, nullptr); // frees desc_set
    }
    if (set_layout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device, set_layout, nullptr);
    }
    for (VkFramebuffer fb : framebuffers) {
        vkDestroyFramebuffer(device, fb, nullptr);
    }
    if (render_pass != VK_NULL_HANDLE) {
        vkDestroyRenderPass(device, render_pass, nullptr);
    }
    if (vert != VK_NULL_HANDLE) {
        vkDestroyShaderModule(device, vert, nullptr);
    }
    if (frag != VK_NULL_HANDLE) {
        vkDestroyShaderModule(device, frag, nullptr);
    }
    for (VkImageView view : views) {
        vkDestroyImageView(device, view, nullptr);
    }
    if (depth_view != VK_NULL_HANDLE) {
        vkDestroyImageView(device, depth_view, nullptr);
    }
    if (depth_image != VK_NULL_HANDLE) {
        vkDestroyImage(device, depth_image, nullptr);
    }
    if (depth_mem != VK_NULL_HANDLE) {
        vkFreeMemory(device, depth_mem, nullptr);
    }
    if (tex_view != VK_NULL_HANDLE) {
        vkDestroyImageView(device, tex_view, nullptr);
    }
    if (sampler != VK_NULL_HANDLE) {
        vkDestroySampler(device, sampler, nullptr);
    }
    if (tex_image != VK_NULL_HANDLE) {
        vkDestroyImage(device, tex_image, nullptr);
    }
    if (tex_mem != VK_NULL_HANDLE) {
        vkFreeMemory(device, tex_mem, nullptr);
    }
    if (staging_buf != VK_NULL_HANDLE) {
        vkDestroyBuffer(device, staging_buf, nullptr);
    }
    if (staging_mem != VK_NULL_HANDLE) {
        vkFreeMemory(device, staging_mem, nullptr);
    }
    if (ubo != VK_NULL_HANDLE) {
        vkDestroyBuffer(device, ubo, nullptr); // before freeing its memory (implicitly unmaps)
    }
    if (ubo_mem != VK_NULL_HANDLE) {
        vkFreeMemory(device, ubo_mem, nullptr);
    }
    if (fence != VK_NULL_HANDLE) {
        vkDestroyFence(device, fence, nullptr);
    }
    if (pool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(device, pool, nullptr);
    }
    if (swapchain != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(device, swapchain, nullptr);
    }
    if (surface != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(instance, surface, nullptr);
    }
    if (device != VK_NULL_HANDLE) {
        vkDestroyDevice(device, nullptr);
    }
    vkDestroyInstance(instance, nullptr);
    xcb_disconnect(xcb);
    if (rc == 0) {
        log("done");
    }
    return rc;
}
