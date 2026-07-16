// vkrelay2 triangle canary.
//
// The deterministic on-screen artifact for the draw spine: a minimal native Vulkan program that
// creates an xcb window on the private Xwayland display, then drives the WHOLE bufferless-triangle
// draw surface -- image views, the two pre-compiled SPIR-V shaders (tests/triangle_spv.h), a render
// pass, framebuffers, an empty pipeline layout, a dynamic-VIEWPORT/SCISSOR graphics pipeline -- and
// per frame: acquire -> record(begin render pass / bind pipeline / set viewport / set scissor /
// draw(3) / end) -> submit -> wait -> present. Run through the real loader with VK_ICD_FILENAMES
// pinned to our ICD (the launcher does that), the spinning-free RGB triangle appears in a real
// Win32 window on the Windows desktop via worker-present. The triangle is bufferless (it emits its
// 3 vertices from gl_VertexIndex), so there is NO vertex buffer and no device memory -- that
// surface is covered by the cube canary (vkcube).
//
// Prints greppable markers to stderr (the boundary smoke asserts the chain). Returns non-zero on a
// real failure; exits 0 with a "skipped" note if there is no X display / no usable Vulkan device.

#include <vulkan/vulkan.h>
#include <vulkan/vulkan_xcb.h>

#include <xcb/xcb.h>

#include "tests/triangle_spv.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <vector>

namespace {

constexpr int kFrames = 3;
constexpr std::uint32_t kExtent = 256;

void log(const char* msg) {
    std::fprintf(stderr, "vkrelay2-triangle: %s\n", msg);
}

bool check(VkResult r, const char* what) {
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
        std::fprintf(stderr, "vkrelay2-triangle: %s failed (VkResult %d)\n", what,
                     static_cast<int>(r));
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
    app.pApplicationName = "vkrelay2-triangle";
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
    std::fprintf(stderr, "vkrelay2-triangle: device '%s'\n", props.deviceName);

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
    VkFence fence = VK_NULL_HANDLE;
    VkRenderPass render_pass = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkShaderModule vert = VK_NULL_HANDLE;
    VkShaderModule frag = VK_NULL_HANDLE;
    std::vector<VkImageView> views;
    std::vector<VkFramebuffer> framebuffers;

    if (!check(vkCreateDevice(phys, &dci, nullptr, &device), "vkCreateDevice")) {
        goto teardown;
    }
    log("device created");
    vkGetDeviceQueue(device, 0, 0, &queue);

    {
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
        log("surface queried");

        VkSurfaceFormatKHR chosen = formats[0];
        for (const auto& f : formats) {
            if (f.format == VK_FORMAT_B8G8R8A8_UNORM &&
                f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                chosen = f;
                break;
            }
        }
        std::uint32_t want = caps.minImageCount < 2 ? 2 : caps.minImageCount;
        if (caps.maxImageCount != 0 && want > caps.maxImageCount) {
            want = caps.maxImageCount;
        }

        // The draw path needs only COLOR_ATTACHMENT usage (no TRANSFER_DST -- there is no clear
        // command; the render pass loadOp=CLEAR clears the attachment).
        VkSwapchainCreateInfoKHR scci{};
        scci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        scci.surface = surface;
        scci.minImageCount = want;
        scci.imageFormat = chosen.format;
        scci.imageColorSpace = chosen.colorSpace;
        scci.imageExtent = {kExtent, kExtent}; // the app picks its extent (dynamic extent)
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

        // The draw surface: image views, shaders, render pass, framebuffers, layout, pipeline.
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

        vert = make_shader(device, vkr::triangle_spv::kVert, vkr::triangle_spv::kVertBytes);
        frag = make_shader(device, vkr::triangle_spv::kFrag, vkr::triangle_spv::kFragBytes);
        if (vert == VK_NULL_HANDLE || frag == VK_NULL_HANDLE) {
            log("vkCreateShaderModule failed");
            goto teardown;
        }
        log("shaders created");

        VkAttachmentDescription att{};
        att.format = chosen.format;
        att.samples = VK_SAMPLE_COUNT_1_BIT;
        att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        att.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        VkAttachmentReference ref{};
        ref.attachment = 0;
        ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &ref;
        VkRenderPassCreateInfo rpci{};
        rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rpci.attachmentCount = 1;
        rpci.pAttachments = &att;
        rpci.subpassCount = 1;
        rpci.pSubpasses = &subpass;
        if (!check(vkCreateRenderPass(device, &rpci, nullptr, &render_pass),
                   "vkCreateRenderPass")) {
            goto teardown;
        }
        for (VkImageView view : views) {
            VkFramebufferCreateInfo fbci{};
            fbci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            fbci.renderPass = render_pass;
            fbci.attachmentCount = 1;
            fbci.pAttachments = &view;
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

        VkPipelineLayoutCreateInfo plci{};
        plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
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
        VkPipelineVertexInputStateCreateInfo vi{};
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
        rs.cullMode = VK_CULL_MODE_NONE; // show the triangle regardless of winding
        rs.frontFace = VK_FRONT_FACE_CLOCKWISE;
        rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms{};
        ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
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

        VkCommandPoolCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = 0;
        if (!check(vkCreateCommandPool(device, &pci, nullptr, &pool), "vkCreateCommandPool")) {
            goto teardown;
        }
        VkCommandBufferAllocateInfo cbai{};
        cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cbai.commandPool = pool;
        cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbai.commandBufferCount = 1;
        if (!check(vkAllocateCommandBuffers(device, &cbai, &cmd), "vkAllocateCommandBuffers")) {
            goto teardown;
        }
        VkFenceCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        if (!check(vkCreateFence(device, &fci, nullptr, &fence), "vkCreateFence")) {
            goto teardown;
        }

        int presented = 0;
        for (int frame = 0; frame < kFrames; ++frame) {
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
                vkDestroySemaphore(device, sem_acq, nullptr);
                vkDestroySemaphore(device, sem_done, nullptr);
                continue; // no resize path here
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
            VkClearValue clear{};
            clear.color.float32[0] = 0.05f;
            clear.color.float32[1] = 0.05f;
            clear.color.float32[2] = 0.08f;
            clear.color.float32[3] = 1.0f;
            VkRenderPassBeginInfo rpbi{};
            rpbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
            rpbi.renderPass = render_pass;
            rpbi.framebuffer = framebuffers[image_index];
            rpbi.renderArea.extent = {kExtent, kExtent};
            rpbi.clearValueCount = 1;
            rpbi.pClearValues = &clear;
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
            vkCmdDraw(cmd, 3, 1, 0, 0);
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
                std::fprintf(stderr, "vkrelay2-triangle: presented frame %d\n", frame);
            }
            sleep_ms(150); // keep the window visible a moment
        }
        rc = (presented >= 1) ? 0 : 1;
        std::fprintf(stderr, "vkrelay2-triangle: presented %d frame(s)\n", presented);
    }

teardown:
    if (pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(device, pipeline, nullptr);
    }
    if (layout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device, layout, nullptr);
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
