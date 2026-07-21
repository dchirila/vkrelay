// Fail-closed predicates for the bounded create-info / command subset.
//
// The ICD calls these to REJECT any unsupported Vulkan input BEFORE forwarding it --
// strict-but-total: a field outside the subset is rejected, never silently canonicalized into
// the subset shape. They cover the fields the wire does NOT carry to the
// worker (the carried fields are validated by the worker + mock, which the dual-platform tests
// already exercise); these are the would-be-silently-dropped ones. Pure + header-only so they are
// unit-tested directly (unit_icd_subset) with no worker connection -- on both platforms (vulkan.h
// is present wherever the ICD/SDK is).
//
// Each `*_ok` returns true if the input is INSIDE the subset (safe to forward). On false it sets
// `*reason` to a short static string naming the offending field.
#ifndef VKRELAY2_LINUX_ICD_ICD_SUBSET_H
#define VKRELAY2_LINUX_ICD_ICD_SUBSET_H

#include "common/vkrpc/indirect_draw_validation.h"

#include <vulkan/vulkan.h>

#include <cstdint>

namespace vkr::icd_subset {

// GL/zink: the ONE admitted image-view pNext is VkImageViewUsageCreateInfo (core 1.1 /
// KHR_maintenance2, which the ICD advertises) -- zink chains it to NARROW a view's usage below
// its image's (e.g. a sampled-only view of a render target; glmark2's [ideas] scene died on the
// blanket rejection). Its usage flags are captured into *view_usage (0 = no such node) and carried
// to the worker, which chains it on the real create. Anything else stays fail-closed.
inline bool image_view_ok(const VkImageViewCreateInfo* ci, std::uint64_t* view_usage,
                          const char** reason) {
    *view_usage = 0;
    for (const auto* node = static_cast<const VkBaseInStructure*>(ci->pNext); node != nullptr;
         node = node->pNext) {
        if (node->sType != VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO) {
            *reason = "image view pNext not supported";
            return false;
        }
        const auto* u = reinterpret_cast<const VkImageViewUsageCreateInfo*>(node);
        if (u->usage == 0) {
            *reason = "image view usage pNext with zero usage";
            return false;
        }
        if (*view_usage != 0) {
            *reason = "image view has duplicate usage pNext nodes";
            return false;
        }
        *view_usage = u->usage;
    }
    if (ci->flags != 0) {
        *reason = "image view flags not supported";
        return false;
    }
    return true;
}

inline bool shader_module_ok(const VkShaderModuleCreateInfo* ci, const char** reason) {
    if (ci->pNext != nullptr) {
        *reason = "shader module pNext not supported";
        return false;
    }
    if (ci->flags != 0) {
        *reason = "shader module flags not supported";
        return false;
    }
    return true;
}

// MRT: shared color-attachment-count gate. `max_color_attachments` is the worker-advertised
// DeviceCaps value; 0 = unknown (an old worker) CAPS THE GATE AT 1 -- the pre-MRT single-color
// behavior -- so a version-skewed ICD/worker pair rejects LOUDLY here instead of the worker
// silently building a narrower pass. A depth-only pass (0 color refs) is legal (zink emits one
// for a shadow-map FBO) when a depth attachment exists; the has-anything check is the caller's
// (worker validates too).
inline bool render_pass_color_count_ok(std::uint32_t color_count, const void* color_refs,
                                       std::uint32_t max_color_attachments, const char** reason) {
    const std::uint32_t effective_max = max_color_attachments == 0 ? 1u : max_color_attachments;
    if (color_count > effective_max) {
        *reason = max_color_attachments == 0
                      ? "render pass color attachment count > 1 (worker did not advertise MRT)"
                      : "render pass color attachment count exceeds the device limit";
        return false;
    }
    if (color_count > 0 && color_refs == nullptr) {
        *reason = "render pass declares color attachments but carries no reference array";
        return false;
    }
    return true;
}

// MRT: widened fields are CARRIED, uncarried fields are LOUD. The wire carries
// attachments (format/samples/ops/layouts -- NOT flags), the full color-ref array, one depth ref,
// and dependencies. Everything else is rejected by name here: a second subpass, non-graphics bind,
// subpass flags, input/resolve/preserve attachments, attachment flags, and any pNext.
inline bool render_pass_ok(const VkRenderPassCreateInfo* ci, std::uint32_t max_color_attachments,
                           const char** reason) {
    if (ci->pNext != nullptr) {
        *reason = "render pass pNext not supported";
        return false;
    }
    if (ci->flags != 0) {
        *reason = "render pass flags not supported";
        return false;
    }
    // Fail-closed pointer guards (MRT): these predicates are the wall, so they
    // must be total -- never dereference an array the caller did not actually provide.
    if (ci->attachmentCount > 0 && ci->pAttachments == nullptr) {
        *reason = "render pass declares attachments but carries no attachment array";
        return false;
    }
    if (ci->dependencyCount > 0 && ci->pDependencies == nullptr) {
        *reason = "render pass declares dependencies but carries no dependency array";
        return false;
    }
    for (std::uint32_t i = 0; i < ci->attachmentCount; ++i) {
        if (ci->pAttachments[i].flags != 0) { // MAY_ALIAS etc. -- the wire does not carry flags
            *reason = "render pass attachment flags not carried";
            return false;
        }
    }
    if (ci->subpassCount != 1 || ci->pSubpasses == nullptr) {
        *reason = "render pass must have exactly one subpass";
        return false;
    }
    const VkSubpassDescription& sp = ci->pSubpasses[0];
    if (sp.flags != 0 || sp.pipelineBindPoint != VK_PIPELINE_BIND_POINT_GRAPHICS) {
        *reason = "render pass subpass must be a plain graphics subpass";
        return false;
    }
    if (!render_pass_color_count_ok(sp.colorAttachmentCount, sp.pColorAttachments,
                                    max_color_attachments, reason)) {
        return false;
    }
    if (sp.colorAttachmentCount == 0 && sp.pDepthStencilAttachment == nullptr) {
        *reason = "render pass subpass needs at least one color or depth attachment";
        return false;
    }
    // A single depth-stencil attachment is allowed (its fields are carried + the
    // worker validates them); input/resolve/preserve stay out of subset.
    if (sp.inputAttachmentCount != 0 || sp.pResolveAttachments != nullptr ||
        sp.preserveAttachmentCount != 0) {
        *reason = "render pass subpass has unsupported attachments (input/resolve/preserve)";
        return false;
    }
    return true;
}

// MRT: the vkCreateRenderPass2 (core 1.2) twin -- zink's preferred entry point. Before this
// checker existed the v2 path forwarded pColorAttachments[0] and SILENTLY dropped the rest (the
// empirically proven 2-color half-render) and would have silently dropped multi-subpass too. Same
// rule: widened fields carried, uncarried fields loud -- additionally rejecting the v2-only
// uncarried surface: correlated view masks, per-dependency viewOffset, and pNext at EVERY level
// (create-info, attachment, subpass, reference -- e.g. stencil-layout structs).
//
// Required-feature audit (multiview): the single subpass's viewMask IS now carried (to
// CreateRenderPassRequest.view_mask -> a real host multiview render pass) when `multiview_enabled`
// (the device enabled the multiview feature). A viewMask on a device WITHOUT the feature is
// rejected fail-closed. The correlated view masks and per-dependency viewOffset remain UNCARRIED
// (loud) -- only the subpass mask crosses the wire.
inline bool render_pass2_ok(const VkRenderPassCreateInfo2* ci, std::uint32_t max_color_attachments,
                            bool multiview_enabled, const char** reason) {
    if (ci->pNext != nullptr) {
        *reason = "render pass2 pNext not supported";
        return false;
    }
    if (ci->flags != 0) {
        *reason = "render pass2 flags not supported";
        return false;
    }
    if (ci->correlatedViewMaskCount != 0) {
        *reason = "render pass2 correlated view masks not supported (multiview)";
        return false;
    }
    // Fail-closed pointer guards (MRT): total predicates, no blind dereference.
    if (ci->attachmentCount > 0 && ci->pAttachments == nullptr) {
        *reason = "render pass declares attachments but carries no attachment array";
        return false;
    }
    if (ci->dependencyCount > 0 && ci->pDependencies == nullptr) {
        *reason = "render pass declares dependencies but carries no dependency array";
        return false;
    }
    for (std::uint32_t i = 0; i < ci->attachmentCount; ++i) {
        const VkAttachmentDescription2& a = ci->pAttachments[i];
        if (a.pNext != nullptr) {
            *reason = "render pass2 attachment pNext not supported";
            return false;
        }
        if (a.flags != 0) {
            *reason = "render pass attachment flags not carried";
            return false;
        }
    }
    // Uncarried v2 dependency surface (MRT): the wire carries the v1-style
    // dependency fields only -- a pNext (e.g. a chained VkMemoryBarrier2) or a multiview
    // viewOffset would be silently dropped, so both are loud.
    for (std::uint32_t i = 0; i < ci->dependencyCount; ++i) {
        if (ci->pDependencies[i].pNext != nullptr) {
            *reason = "render pass2 dependency pNext not supported";
            return false;
        }
        if (ci->pDependencies[i].viewOffset != 0) {
            *reason = "render pass2 dependency viewOffset not supported (multiview)";
            return false;
        }
    }
    if (ci->subpassCount != 1 || ci->pSubpasses == nullptr) {
        *reason = "render pass must have exactly one subpass";
        return false;
    }
    const VkSubpassDescription2& sp = ci->pSubpasses[0];
    if (sp.pNext != nullptr) {
        *reason = "render pass2 subpass pNext not supported";
        return false;
    }
    if (sp.flags != 0 || sp.pipelineBindPoint != VK_PIPELINE_BIND_POINT_GRAPHICS) {
        *reason = "render pass subpass must be a plain graphics subpass";
        return false;
    }
    // Required-feature audit: the subpass viewMask is carried when the device enabled
    // the multiview feature; otherwise it stays fail-closed. (The correlated masks above + the
    // dependency viewOffset below remain uncarried in either case.)
    if (sp.viewMask != 0 && !multiview_enabled) {
        *reason = "render pass2 subpass viewMask requires the multiview feature enabled";
        return false;
    }
    if (!render_pass_color_count_ok(sp.colorAttachmentCount, sp.pColorAttachments,
                                    max_color_attachments, reason)) {
        return false;
    }
    if (sp.colorAttachmentCount == 0 && sp.pDepthStencilAttachment == nullptr) {
        *reason = "render pass subpass needs at least one color or depth attachment";
        return false;
    }
    if (sp.inputAttachmentCount != 0 || sp.pResolveAttachments != nullptr ||
        sp.preserveAttachmentCount != 0) {
        *reason = "render pass subpass has unsupported attachments (input/resolve/preserve)";
        return false;
    }
    for (std::uint32_t i = 0; i < sp.colorAttachmentCount; ++i) {
        if (sp.pColorAttachments[i].pNext != nullptr) {
            *reason = "render pass2 color reference pNext not supported";
            return false;
        }
        // Reference aspectMask is deliberately NOT checked (MRT): the
        // spec defines it as used ONLY for input attachment references -- which this checker
        // rejects wholesale -- so in a color/depth position the field carries NO semantics to
        // drop. Decisive empirical evidence: zink sends UNINITIALIZED memory through it
        // (observed 0x967e878c on a depth ref; a strict identity rule rejected real OpenSCAD
        // render passes on garbage bytes). Not carrying a spec-ignored field is not a silent
        // drop; if input attachments are ever carried, THEIR aspectMask becomes meaningful and
        // must ride the wire.
    }
    if (sp.pDepthStencilAttachment != nullptr) {
        if (sp.pDepthStencilAttachment->pNext != nullptr) {
            *reason = "render pass2 depth reference pNext not supported";
            return false;
        }
        // aspectMask: same disposition as the color refs above (spec-ignored position).
    }
    return true;
}

// --- Images + depth--------------------------------------------------------

// GL/zink: faithful image admission. The vkcube subset (no pNext/flags, 2D, single
// mip/layer/sample, usage in {TRANSFER_DST, SAMPLED, DEPTH_STENCIL}) is replaced by sane-bounds
// checks: the VkImageFormatListCreateInfo pNext (mutable-format aliasing -- carried) is accepted,
// all flags / types / usages / mip+layer+sample counts / tilings are forwarded, and the host driver
// is the authoritative gate (an unsupported combination fails the real vkCreateImage, returned
// honestly). Only a non-format-list pNext (which the wire does NOT carry) is fail-closed, and the
// extent must be positive. The memory CLASS guard stays at vkAllocateMemory (memory_class_ok),
// which admits any non-protected, non-(host-visible-non-coherent) type -- mappability is enforced
// separately at vkMapMemory (Option 1 Tier 1).
inline bool image_ok(const VkImageCreateInfo* ci, const char** reason) {
    for (auto* n = static_cast<const VkBaseInStructure*>(ci->pNext); n != nullptr; n = n->pNext) {
        if (n->sType != VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO) {
            *reason = "image pNext not supported (only format-list)";
            return false;
        }
    }
    if (ci->extent.width == 0 || ci->extent.height == 0 || ci->extent.depth == 0) {
        *reason = "image extent must be positive";
        return false;
    }
    if (ci->usage == 0) {
        *reason = "image usage must be nonzero";
        return false;
    }
    return true;
}

// Allocate-time memory-class admission. Mappability is a
// vkMapMemory-time property, NOT an allocation-time one: a non-host-visible type -- e.g. the NVIDIA
// propertyFlags==0 device-accessible sysmem type, or a DEVICE_LOCAL-only type -- is a legitimate
// allocation class the app never maps, so admit it and let the worker's real vkAllocateMemory be
// the authority (an unsupported type fails there, honestly). Two classes stay fail-closed here:
//   - PROTECTED: needs the protected queue/submit path the relay does not implement.
//   - HOST_VISIBLE without HOST_COHERENT: on_map cannot mirror it until Tier 2, so rejecting it at
//     ALLOCATE (not at map) avoids a fresh advertise-then-fail deeper in the app. Neither the RTX
//     4080 nor the AMD 610M exposes this class (measured through the relay), so nothing regresses.
// The predicate is mirrored verbatim in both backends (mock + real) -- see the "memory-class
// reassertion" blocks -- so a direct/hostile RPC gets the same fail-closed answer independently of
// the ICD. vkMapMemory still enforces HOST_VISIBLE|HOST_COHERENT before creating a shadow.
inline bool memory_class_ok(VkMemoryPropertyFlags flags, const char** reason) {
    if ((flags & VK_MEMORY_PROPERTY_PROTECTED_BIT) != 0) {
        *reason = "protected memory is not supported (no protected queue/submit path)";
        return false;
    }
    if ((flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0 &&
        (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) == 0) {
        *reason = "host-visible memory without HOST_COHERENT is not yet supported (needs Tier 2 "
                  "flush/invalidate)";
        return false;
    }
    return true;
}

// --- Sampler + texture upload--------------------------------------

// The bounded sampler subset (vkcube's exact sampler): NEAREST mag/min/mipmap, CLAMP_TO_EDGE on all
// axes, anisotropy + compare disabled, LOD fixed at 0, normalized coordinates. borderColor is NOT
// checked: with CLAMP_TO_EDGE the border is never sampled, so the value is unobservable (and not
// carried to the worker, which fixes it).
inline bool sampler_ok(const VkSamplerCreateInfo* ci, const char** reason) {
    // GL/zink: a general GL sampler is forwarded faithfully -- all VkSamplerCreateInfo
    // scalar fields (filters, address modes, LOD, anisotropy, compare, border color) ride the wire
    // and the worker's real vkCreateSampler validates them. Only the un-carried pNext/flags are
    // fail-closed here. (The vkcube subset's NEAREST/CLAMP-only restriction is lifted.)
    if (ci->pNext != nullptr) {
        *reason = "sampler pNext not supported";
        return false;
    }
    if (ci->flags != 0) {
        *reason = "sampler flags not supported";
        return false;
    }
    return true;
}

// Faithful copy_buffer_to_image admission (widened from the earlier fixed-region subset,
// which admitted exactly one full-image mip-0 region and silently blocked every SUB-REGION texture
// upload -- zink's glTexSubImage2D path, the first thing a real GL game does after the menu).
// All VkBufferImageCopy fields for all regions now RIDE THE WIRE (the copy_image_to_buffer
// 13-value convention, the readback shape); the worker validates each region's
// subresource + bounds against the image it knows (mips/layers/extent) and the host driver stays
// the authoritative gate. What remains fail-closed HERE (named, spec-anchored, ICD-checkable
// without the image): the two spec-legal transfer-dst layouts (SHARED_PRESENT deliberately out
// until a target demands it), a single legal transfer aspect (exactly one of COLOR/DEPTH/STENCIL --
// the aspect-vs-image-format agreement is the worker's job since the ICD does not track the image
// here), the 2D scope (depth 1, z 0 -- matching the tracked 2D image state), and the buffer-stride
// VUs (rowLength/imageHeight are 0 or >= the extent, per spec). Depth/stencil uploads (zink's
// staging path for a depth texture) ride the same regions -- the aspect already rides the wire.
inline bool copy_buffer_to_image_ok(VkImageLayout dstLayout, std::uint32_t regionCount,
                                    const VkBufferImageCopy* regions, const char** reason) {
    if (dstLayout != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && dstLayout != VK_IMAGE_LAYOUT_GENERAL) {
        *reason = "copy_buffer_to_image dst layout must be TRANSFER_DST_OPTIMAL or GENERAL";
        return false;
    }
    if (regionCount == 0 || regions == nullptr) {
        *reason = "copy_buffer_to_image needs at least one region";
        return false;
    }
    for (std::uint32_t i = 0; i < regionCount; ++i) {
        const VkBufferImageCopy& r = regions[i];
        // Exactly one legal transfer aspect. Comparing against the three single-bit values rejects
        // 0, any multi-bit combo (e.g. DEPTH|STENCIL -- not a legal single buffer-copy region), and
        // any other bit (METADATA / plane). Whether that aspect actually belongs to the dest image
        // is the worker's check (it tracks the image's format-derived aspect).
        const VkImageAspectFlags am = r.imageSubresource.aspectMask;
        if (am != VK_IMAGE_ASPECT_COLOR_BIT && am != VK_IMAGE_ASPECT_DEPTH_BIT &&
            am != VK_IMAGE_ASPECT_STENCIL_BIT) {
            *reason =
                "copy_buffer_to_image region aspect must be exactly one of COLOR/DEPTH/STENCIL";
            return false;
        }
        if (r.imageSubresource.layerCount == 0) {
            *reason = "copy_buffer_to_image region layerCount must be >= 1";
            return false;
        }
        if (r.imageExtent.width == 0 || r.imageExtent.height == 0 || r.imageExtent.depth != 1) {
            *reason = "copy_buffer_to_image region extent must be a positive 2D extent (depth 1)";
            return false;
        }
        if (r.imageOffset.x < 0 || r.imageOffset.y < 0 || r.imageOffset.z != 0) {
            *reason = "copy_buffer_to_image region offset must be non-negative with z 0";
            return false;
        }
        if (r.bufferRowLength != 0 && r.bufferRowLength < r.imageExtent.width) {
            *reason = "copy_buffer_to_image region bufferRowLength must be 0 or >= extent width";
            return false;
        }
        if (r.bufferImageHeight != 0 && r.bufferImageHeight < r.imageExtent.height) {
            *reason = "copy_buffer_to_image region bufferImageHeight must be 0 or >= extent height";
            return false;
        }
    }
    return true;
}

inline bool framebuffer_ok(const VkFramebufferCreateInfo* ci, const char** reason) {
    // GL/zink: zink HARD-REQUIRES imageless framebuffers -- it creates them with the
    // VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT flag + a VkFramebufferAttachmentsCreateInfo pNext, and
    // supplies the actual image views at vkCmdBeginRenderPass (VkRenderPassAttachmentBeginInfo).
    // The relay carries `imageless` + the deferred views and builds a regular framebuffer at begin
    // time. A non-imageless framebuffer (vkcube) keeps its concrete-view pNext-free shape.
    const bool imageless = (ci->flags & VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT) != 0;
    for (auto* n = static_cast<const VkBaseInStructure*>(ci->pNext); n != nullptr; n = n->pNext) {
        if (n->sType != VK_STRUCTURE_TYPE_FRAMEBUFFER_ATTACHMENTS_CREATE_INFO) {
            *reason = "framebuffer pNext not supported (only attachments)";
            return false;
        }
    }
    if (!imageless && ci->pNext != nullptr) {
        *reason = "non-imageless framebuffer must not carry an attachments pNext";
        return false;
    }
    // MRT: up to 8 color + 1 depth. A sane bound, not a host limit -- the worker validates the
    // exact count/formats against the render pass, and the host driver is the final gate. (The
    // old {1,2} gate could not distinguish color+color from color+depth, which is exactly how the
    // 2-color silent half-render slipped past it.)
    constexpr std::uint32_t kMaxFramebufferAttachments = 9;
    if (ci->attachmentCount < 1 || ci->attachmentCount > kMaxFramebufferAttachments) {
        *reason = "framebuffer attachment count outside 1..9 (8 color + depth)";
        return false;
    }
    return true;
}

// Subset caps (mirror vkrpc::kMax* in vulkan_session.hpp -- kept in lockstep by hand, as the
// mock's numeric constants and the predicate's real VK_* enums already are). GL/zink
// widened these from the vkcube subset: zink builds larger pools + more bindings than vkcube's
// 2-binding layout. They remain sane upper bounds (a malformed huge value is still rejected), not
// host limits -- the host driver is the authoritative gate.
constexpr std::uint32_t kMaxPipelineLayoutSetLayouts = 32;
constexpr std::uint32_t kMaxDescriptorSetLayoutBindings = 4096;
constexpr std::uint32_t kMaxDescriptorCount = 65536;
constexpr std::uint32_t kMaxDescriptorPoolSets = 65536;
constexpr std::uint32_t kMaxBoundDescriptorSets = 32;
// GL/zink: push-constant bounds. 256 bytes is a common host cap and >= the 128-byte
// Vulkan minimum; 8 ranges is generous (zink uses 1-2). Ranges are validated faithfully OR
// rejected.
constexpr std::uint32_t kMaxPushConstantRanges = 8;
constexpr std::uint32_t kMaxPushConstantBytes = 256;

inline bool pipeline_layout_ok(const VkPipelineLayoutCreateInfo* ci, const char** reason) {
    if (ci->pNext != nullptr) {
        *reason = "pipeline layout pNext not supported";
        return false;
    }
    if (ci->flags != 0) {
        *reason = "pipeline layout flags not supported";
        return false;
    }
    // GL/zink: push-constant ranges are forwarded faithfully (zink uses them). Validate
    // each: a nonzero stage mask, a positive 4-byte-aligned offset/size, within the byte cap.
    if (ci->pushConstantRangeCount > kMaxPushConstantRanges) {
        *reason = "pipeline layout has too many push-constant ranges";
        return false;
    }
    if (ci->pushConstantRangeCount != 0 && ci->pPushConstantRanges == nullptr) {
        *reason = "pipeline layout pushConstantRangeCount > 0 but pPushConstantRanges is null";
        return false;
    }
    for (std::uint32_t i = 0; i < ci->pushConstantRangeCount; ++i) {
        const VkPushConstantRange& pc = ci->pPushConstantRanges[i];
        if (pc.stageFlags == 0 || pc.size == 0 || (pc.offset % 4) != 0 || (pc.size % 4) != 0 ||
            pc.offset > kMaxPushConstantBytes || pc.size > kMaxPushConstantBytes ||
            pc.offset + pc.size > kMaxPushConstantBytes) {
            *reason = "push-constant range out of subset (stage/align/size bound)";
            return false;
        }
    }
    // The layout may reference set layouts (carried + validated by the worker); here
    // bound the count + guard the array.
    if (ci->setLayoutCount > kMaxPipelineLayoutSetLayouts) {
        *reason = "pipeline layout has too many set layouts";
        return false;
    }
    if (ci->setLayoutCount != 0 && ci->pSetLayouts == nullptr) {
        *reason = "pipeline layout setLayoutCount > 0 but pSetLayouts is null";
        return false;
    }
    return true;
}

// --- Host-visible memory + buffers------------------------------------------

inline bool buffer_ok(const VkBufferCreateInfo* ci, const char** reason) {
    if (ci->pNext != nullptr || ci->flags != 0) {
        *reason = "buffer pNext/flags not supported";
        return false;
    }
    if (ci->size == 0) {
        *reason = "buffer size must be > 0";
        return false;
    }
    // GL/zink: usage must be a NONZERO subset of the core buffer usages a general GL
    // driver creates (index/storage/transfer-dst/indirect/texel beyond the vkcube
    // vertex/uniform/transfer-src). Specific behaviors still key on specific bits; the worker's
    // real vkCreateBuffer validates.
    {
        const VkBufferUsageFlags kSubset =
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
            VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT |
            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
            VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
            VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_CONDITIONAL_RENDERING_BIT_EXT |
            VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT |
            VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_COUNTER_BUFFER_BIT_EXT |
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        if (ci->usage == 0 || (ci->usage & ~kSubset) != 0) {
            *reason = "buffer usage outside the supported set";
            return false;
        }
    }
    if (ci->sharingMode != VK_SHARING_MODE_EXCLUSIVE) {
        *reason = "buffer sharing mode must be EXCLUSIVE";
        return false;
    }
    return true;
}

// Core indirect draws use the same total structural predicate at the ICD and both worker
// backends. `indexed` selects the host command struct size; GPU-owned command contents are not
// inspected (their validity remains the application's responsibility).
inline bool core_indirect_worker_ok(bool supported, const char** reason) {
    if (!supported) {
        *reason = "core indirect draw requires a worker advertising core_indirect_draw support";
        return false;
    }
    return true;
}

inline bool core_indirect_count_worker_ok(bool supported, const char** reason) {
    if (!supported) {
        *reason = "indirect-count draw requires a worker advertising "
                  "core_indirect_draw_count support";
        return false;
    }
    return true;
}

inline bool draw_indirect_ok(bool buffer_live, bool buffer_bound, VkBufferUsageFlags usage,
                             VkDeviceSize buffer_size, VkDeviceSize offset,
                             std::uint32_t draw_count, std::uint32_t stride, bool indexed,
                             bool multi_draw_indirect_enabled, const char** reason) {
    return vkr::vkrpc::core_indirect_draw_ok(
        buffer_live, buffer_bound, (usage & VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT) != 0,
        static_cast<std::uint64_t>(buffer_size), static_cast<std::uint64_t>(offset),
        static_cast<long long>(draw_count), static_cast<long long>(stride),
        indexed ? vkr::vkrpc::kDrawIndexedIndirectCommandBytes
                : vkr::vkrpc::kDrawIndirectCommandBytes,
        multi_draw_indirect_enabled, reason);
}

inline bool draw_indirect_count_ok(bool command_enabled, bool buffer_live, bool buffer_bound,
                                   VkBufferUsageFlags usage, VkDeviceSize buffer_size,
                                   bool count_buffer_live, bool count_buffer_bound,
                                   VkBufferUsageFlags count_buffer_usage,
                                   VkDeviceSize count_buffer_size, VkDeviceSize offset,
                                   VkDeviceSize count_buffer_offset, std::uint32_t max_draw_count,
                                   std::uint32_t stride, bool indexed, const char** reason) {
    vkr::vkrpc::IndirectBufferState buffer;
    buffer.live = buffer_live;
    buffer.bound = buffer_bound;
    buffer.has_indirect_usage = (usage & VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT) != 0;
    buffer.size = static_cast<std::uint64_t>(buffer_size);
    vkr::vkrpc::IndirectBufferState count_buffer;
    count_buffer.live = count_buffer_live;
    count_buffer.bound = count_buffer_bound;
    count_buffer.has_indirect_usage =
        (count_buffer_usage & VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT) != 0;
    count_buffer.size = static_cast<std::uint64_t>(count_buffer_size);
    return vkr::vkrpc::core_indirect_count_draw_ok(
        command_enabled, buffer, count_buffer, static_cast<std::uint64_t>(offset),
        static_cast<std::uint64_t>(count_buffer_offset), static_cast<long long>(max_draw_count),
        static_cast<long long>(stride),
        indexed ? vkr::vkrpc::kDrawIndexedIndirectCommandBytes
                : vkr::vkrpc::kDrawIndirectCommandBytes,
        reason);
}

// bufferDeviceAddress: the ONE admitted pNext is a VkMemoryAllocateFlagsInfo carrying
// exactly VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT, and only when the device enabled the
// bufferDeviceAddress feature (the caller passes its DeviceImpl state -- the guard is
// feature-scoped, never weakened globally). DEVICE_MASK / CAPTURE_REPLAY flags, a nonzero
// deviceMask, and every other pNext (dedicated/export/opaque-capture-address) stay fail-closed
// by name. Without the flag the host rejects binding a SHADER_DEVICE_ADDRESS buffer
// (VUID-vkBindBufferMemory-bufferDeviceAddress-03339), so serializing it is load-bearing.
inline bool memory_allocate_ok(const VkMemoryAllocateInfo* ci, bool buffer_device_address_enabled,
                               const char** reason) {
    for (auto* n = static_cast<const VkBaseInStructure*>(ci->pNext); n != nullptr; n = n->pNext) {
        if (n->sType != VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO) {
            *reason = "memory allocate pNext not supported (only flags-info, and only with "
                      "bufferDeviceAddress enabled)";
            return false;
        }
        const auto* fi = reinterpret_cast<const VkMemoryAllocateFlagsInfo*>(n);
        if (!buffer_device_address_enabled) {
            *reason = "memory allocate flags-info requires the enabled bufferDeviceAddress "
                      "feature";
            return false;
        }
        if ((fi->flags &
             ~static_cast<VkMemoryAllocateFlags>(VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT)) != 0) {
            *reason = "memory allocate flags outside the supported set (only DEVICE_ADDRESS; "
                      "captureReplay/deviceMask rejected)";
            return false;
        }
        if (fi->deviceMask != 0) {
            *reason = "memory allocate deviceMask not supported (multiDevice unwired)";
            return false;
        }
    }
    if (ci->allocationSize == 0) {
        *reason = "memory allocation size must be > 0";
        return false;
    }
    return true;
}

inline bool bind_vertex_buffers_ok(std::uint32_t first_binding, std::uint32_t binding_count,
                                   const VkBuffer* buffers, const VkDeviceSize* offsets,
                                   const char** reason) {
    if (first_binding != 0) {
        *reason = "bind_vertex_buffers must use firstBinding 0";
        return false;
    }
    if (binding_count == 0 || buffers == nullptr || offsets == nullptr) {
        *reason = "bind_vertex_buffers requires a non-empty buffers + offsets pair";
        return false;
    }
    for (std::uint32_t i = 0; i < binding_count; ++i) {
        if (buffers[i] == VK_NULL_HANDLE) {
            *reason = "bind_vertex_buffers has a null buffer handle";
            return false;
        }
    }
    return true;
}

// --- Descriptor surface----------------------------------------------------

// GL/zink: faithful descriptor-set-layout admission. The vkcube subset (no pNext/flags,
// UNIFORM_BUFFER|COMBINED_IMAGE_SAMPLER, VERTEX|FRAGMENT only) is replaced by sane-bounds checks --
// the binding-flags pNext is accepted (its per-binding flags are forwarded), all descriptor types
// and stages are admitted, and the host driver is the authoritative gate.
//
// descriptorIndexing: the binding-flags pNext must be EXACT, not best-effort
// (the spec rule): a struct whose bindingCount != 0 must match the create-info's
// bindingCount with a non-null pBindingFlags; bindingCount == 0 (or an absent struct) means every
// binding flag is zero. Duplicate structs are rejected. The per-flag FEATURE gating (which flags
// are admissible on which types, per the enabled kDIFeature* bits) is the shared
// vkrpc::descriptor_indexing_layout_ok, called by the ICD entry + both backends -- this predicate
// owns only the structural shape.
inline bool descriptor_set_layout_ok(const VkDescriptorSetLayoutCreateInfo* ci,
                                     const char** reason) {
    bool saw_binding_flags = false;
    for (auto* n = static_cast<const VkBaseInStructure*>(ci->pNext); n != nullptr; n = n->pNext) {
        if (n->sType != VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO) {
            *reason = "descriptor set layout pNext not supported (only binding-flags)";
            return false;
        }
        if (saw_binding_flags) {
            *reason = "descriptor set layout has duplicate binding-flags structs";
            return false;
        }
        saw_binding_flags = true;
        const auto* bf = reinterpret_cast<const VkDescriptorSetLayoutBindingFlagsCreateInfo*>(n);
        if (bf->bindingCount != 0 &&
            (bf->bindingCount != ci->bindingCount || bf->pBindingFlags == nullptr)) {
            *reason = "binding-flags bindingCount must be 0 or match the layout's bindingCount "
                      "(with a non-null flags array)";
            return false;
        }
    }
    if (ci->bindingCount > kMaxDescriptorSetLayoutBindings) {
        *reason = "descriptor set layout binding count out of bounds";
        return false;
    }
    if (ci->bindingCount != 0 && ci->pBindings == nullptr) {
        *reason = "descriptor set layout has bindings but pBindings is null";
        return false;
    }
    for (std::uint32_t i = 0; i < ci->bindingCount; ++i) {
        const VkDescriptorSetLayoutBinding& b = ci->pBindings[i];
        if (b.descriptorCount > kMaxDescriptorCount) {
            *reason = "descriptor binding count out of bounds";
            return false;
        }
        // Immutable samplers are not carried over the wire -- accepting them would silently drop
        // the app's samplers. Fail closed until they are forwarded (zink's GL path does not use
        // them).
        if (b.pImmutableSamplers != nullptr) {
            *reason = "descriptor binding immutable samplers not supported";
            return false;
        }
    }
    return true;
}

// descriptorIndexing: the vkAllocateDescriptorSets pNext shape. The ONE
// admitted struct is VkDescriptorSetVariableDescriptorCountAllocateInfo. The spec edge comes
// FIRST: a descriptorSetCount of 0 means "as if the struct were absent" (every variable length is
// zero) and is accepted even with the feature disabled -- only a NONZERO count engages the
// feature gate, and must then parallel the allocation's set count with a non-null array.
inline bool descriptor_set_alloc_ok(const VkDescriptorSetAllocateInfo* ai,
                                    bool variable_descriptor_count_enabled, const char** reason) {
    for (auto* n = static_cast<const VkBaseInStructure*>(ai->pNext); n != nullptr; n = n->pNext) {
        if (n->sType != VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO) {
            *reason = "descriptor set alloc pNext not supported (only variable-descriptor-count)";
            return false;
        }
        const auto* vc =
            reinterpret_cast<const VkDescriptorSetVariableDescriptorCountAllocateInfo*>(n);
        if (vc->descriptorSetCount == 0) {
            continue; // as if absent: every variable length is 0 (no feature needed)
        }
        if (!variable_descriptor_count_enabled) {
            *reason = "variable-descriptor-count requires its enabled feature";
            return false;
        }
        if (vc->descriptorSetCount != ai->descriptorSetCount || vc->pDescriptorCounts == nullptr) {
            *reason = "variable-count array must parallel the allocation's set count";
            return false;
        }
    }
    return true;
}

// GL/zink: faithful descriptor-pool admission. The vkcube subset (no pNext/flags, maxSets
// <= 256, one-or-two distinct {UNIFORM_BUFFER, COMBINED_IMAGE_SAMPLER} sizes) is replaced by
// sane-bounds checks; FREE_DESCRIPTOR_SET is admitted (zink uses it), any pool-size types/counts
// are forwarded, and the host driver is the authoritative gate. descriptorIndexing:
// the UPDATE_AFTER_BIND flag is a CONTAINER flag (limit-bucket selection), valid
// without any UAB feature -- it rides the wire; the per-binding UAB flags carry the feature gates.
inline bool descriptor_pool_ok(const VkDescriptorPoolCreateInfo* ci, const char** reason) {
    // Vulkan 1.3 support (inlineUniformBlock): the ONE admitted pool pNext is
    // VkDescriptorPoolInlineUniformBlockCreateInfo -- its maxInlineUniformBlockBindings rides the
    // wire, the worker rebuilds the struct, and both backends gate it on the enabled feature (an
    // IUB pool size without it, or either without the feature, fails closed there). Any other
    // sType rejects by name; duplicates reject like the binding-flags struct above.
    bool saw_iub = false;
    for (auto* n = static_cast<const VkBaseInStructure*>(ci->pNext); n != nullptr; n = n->pNext) {
        if (n->sType != VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_INLINE_UNIFORM_BLOCK_CREATE_INFO) {
            *reason = "descriptor pool pNext not supported (only inline-uniform-block)";
            return false;
        }
        if (saw_iub) {
            *reason = "descriptor pool has duplicate inline-uniform-block structs";
            return false;
        }
        saw_iub = true;
    }
    if (ci->maxSets == 0 || ci->maxSets > kMaxDescriptorPoolSets) {
        *reason = "descriptor pool maxSets out of bounds (1..kMax)";
        return false;
    }
    if (ci->poolSizeCount == 0 || ci->pPoolSizes == nullptr) {
        *reason = "descriptor pool must have at least one pool size";
        return false;
    }
    for (std::uint32_t i = 0; i < ci->poolSizeCount; ++i) {
        if (ci->pPoolSizes[i].descriptorCount == 0) {
            *reason = "descriptor pool size must have a nonzero count";
            return false;
        }
    }
    return true;
}

// GL/zink: FAITHFUL descriptor binding. zink binds sets at any firstSet (incrementally,
// e.g. set 0 then set 1), with dynamic offsets for dynamic UBO/SSBO. The bind point may be GRAPHICS
// or COMPUTE. The firstSet/set count/dynamic offsets all ride the wire and the worker resolves +
// forwards them faithfully; the real driver (and the validation layer) is the binding-correctness
// authority. The predicate only fail-closes on a structurally impossible request.
inline bool bind_descriptor_sets_ok(VkPipelineBindPoint bind_point, std::uint32_t first_set,
                                    std::uint32_t set_count, std::uint32_t dynamic_offset_count,
                                    const char** reason) {
    if (bind_point != VK_PIPELINE_BIND_POINT_GRAPHICS &&
        bind_point != VK_PIPELINE_BIND_POINT_COMPUTE) {
        *reason = "bind_descriptor_sets must be GRAPHICS or COMPUTE";
        return false;
    }
    if (set_count == 0 || first_set + set_count > kMaxBoundDescriptorSets) {
        *reason = "bind_descriptor_sets firstSet+count out of range (1..kMax)";
        return false;
    }
    (void) dynamic_offset_count; // carried faithfully; no subset bound
    return true;
}

// GL/zink: FAITHFUL graphics-pipeline admission. The full rasterization / multisample /
// colour-blend / depth-stencil / viewport-count / tessellation state is now CARRIED, so the host
// driver gates it; the predicate only fail-closes on state the wire does NOT carry: a pipeline /
// per-stage pNext or flags, derivatives, per-stage SPECIALIZATION constants, an explicit sample
// MASK, and STATIC viewports/scissors (zink uses dynamic). The depth-determining state keeps its
// hardened worker-side cross-check (a depth render pass needs depth-stencil state).
// Native lane, dynamic rendering: `allow_dynamic_rendering_
// pnext` is TRUE only when the caller is on the native lane AND the device enabled
// VK_KHR_dynamic_rendering. It ADMITS exactly one top-level pNext -- VkPipelineRenderingCreateInfo
// (the attachment formats a NULL-renderpass pipeline needs; the worker rebuilds the struct). With
// the flag FALSE (the default/zink lane, or a device that did not enable the extension) ANY
// top-level pNext is rejected in the ICD before any RPC -- the default lane's guard is
// byte-identical to before. The decision is kept AT the call site (a per-device bool), never a
// global relaxation.
// Vulkan 1.3 support: the pipeline-create shapes the 1.3 feature set admits, decided per-device
// at the call site (the allow_* bools derive from the ENABLED kVk13Feature* bits + the honest
// 1.3 device flag) -- a Vulkan-1.2 device keeps the byte-identical fail-closed guards.
//   - allow_cache_control_flags: FAIL_ON_PIPELINE_COMPILE_REQUIRED / EARLY_RETURN_ON_FAILURE
//     create flags (pipelineCreationCacheControl enabled). Served ICD-LOCALLY: the relay's
//     pipeline cache is an honest empty no-op, so compilation is ALWAYS required and the entry
//     answers VK_PIPELINE_COMPILE_REQUIRED without forwarding.
//   - allow_subgroup_size_control: the ALLOW_VARYING_SUBGROUP_SIZE stage flag + the
//     VkPipelineShaderStageRequiredSubgroupSizeCreateInfo stage pNext (subgroupSizeControl
//     enabled); REQUIRE_FULL_SUBGROUPS additionally needs allow_full_subgroups
//     (computeFullSubgroups) and is compute-only.
//   - allow_pipeline_feedback: VkPipelineCreationFeedbackCreateInfo top-level pNext (core 1.3,
//     featureless -- gated on the honest 1.3 device). The ICD accepts it and reports NO feedback
//     (VALID bit cleared), the spec-allowed "no feedback" answer.
inline bool pipeline_stage_shape_ok(const VkPipelineShaderStageCreateInfo& s,
                                    bool allow_subgroup_size_control, bool allow_full_subgroups,
                                    bool is_compute, const char** reason) {
    VkPipelineShaderStageCreateFlags allowed_flags = 0;
    if (allow_subgroup_size_control) {
        allowed_flags |= VK_PIPELINE_SHADER_STAGE_CREATE_ALLOW_VARYING_SUBGROUP_SIZE_BIT;
        if (allow_full_subgroups && is_compute) {
            allowed_flags |= VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT;
        }
    }
    if ((s.flags & ~allowed_flags) != 0) {
        *reason = "pipeline stage flags not supported (subgroup-size flags need the enabled "
                  "subgroupSizeControl/computeFullSubgroups features)";
        return false;
    }
    for (auto* n = static_cast<const VkBaseInStructure*>(s.pNext); n != nullptr; n = n->pNext) {
        if (allow_subgroup_size_control &&
            n->sType ==
                VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO) {
            continue; // the one admitted stage pNext, subgroupSizeControl only
        }
        *reason = "pipeline stage pNext not supported";
        return false;
    }
    if (s.pSpecializationInfo != nullptr) {
        *reason = "pipeline stage specialization constants not supported";
        return false;
    }
    return true;
}

inline bool graphics_pipeline_ok(const VkGraphicsPipelineCreateInfo* ci,
                                 bool allow_dynamic_rendering_pnext, bool allow_cache_control_flags,
                                 bool allow_subgroup_size_control, bool allow_pipeline_feedback,
                                 bool allow_vertex_attribute_divisor,
                                 bool allow_rasterization_stream, const char** reason) {
    VkPipelineCreateFlags allowed_flags = 0;
    if (allow_cache_control_flags) {
        allowed_flags |= VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT |
                         VK_PIPELINE_CREATE_EARLY_RETURN_ON_FAILURE_BIT;
    }
    if ((ci->flags & ~allowed_flags) != 0) {
        *reason = "graphics pipeline flags not supported";
        return false;
    }
    for (auto* n = static_cast<const VkBaseInStructure*>(ci->pNext); n != nullptr; n = n->pNext) {
        if (allow_dynamic_rendering_pnext &&
            n->sType == VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO) {
            continue; // the one admitted top-level pNext, native DR lane only
        }
        if (allow_pipeline_feedback &&
            n->sType == VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO) {
            continue; // core-1.3 feedback; the entry clears the VALID bits (= no feedback)
        }
        *reason = "graphics pipeline pNext not supported";
        return false;
    }
    if (ci->basePipelineHandle != VK_NULL_HANDLE) {
        *reason = "graphics pipeline derivatives not supported";
        return false;
    }
    for (std::uint32_t i = 0; i < ci->stageCount; ++i) {
        if (!pipeline_stage_shape_ok(ci->pStages[i], allow_subgroup_size_control,
                                     /*allow_full_subgroups=*/false, /*is_compute=*/false,
                                     reason)) {
            return false;
        }
    }
    if (ci->pVertexInputState != nullptr) {
        if (ci->pVertexInputState->flags != 0) {
            *reason = "graphics pipeline vertex-input flags not supported";
            return false;
        }
        // vertex-attr-divisor: walk the vertex-input pNext and admit EXACTLY
        // VkPipelineVertexInputDivisorStateCreateInfoEXT (VK_EXT_vertex_attribute_divisor), and
        // only when the device enabled the extension (zink chains it for instanced attributes --
        // Blender). This is STRUCTURAL admission only; the divisor CONTENT (bounded ranges, binding
        // references, no duplicates, and the enabled-feature value gates) is validated by
        // vkrpc::vertex_binding_divisors_ok after the ICD extracts the array. Every OTHER
        // vertex-input pNext stays fail-closed by name.
        bool saw_vertex_divisor = false;
        for (auto* n = static_cast<const VkBaseInStructure*>(ci->pVertexInputState->pNext);
             n != nullptr; n = n->pNext) {
            if (allow_vertex_attribute_divisor &&
                n->sType == VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_EXT) {
                // A pNext chain must not repeat an sType (Vulkan structure-chain rule); a second
                // divisor struct would be silently collapsed into one wire array.
                if (saw_vertex_divisor) {
                    *reason = "graphics pipeline has duplicate vertex-input divisor pNext structs";
                    return false;
                }
                saw_vertex_divisor = true;
                continue;
            }
            *reason = "graphics pipeline vertex-input pNext not supported";
            return false;
        }
    }
    if (ci->pInputAssemblyState != nullptr &&
        (ci->pInputAssemblyState->pNext != nullptr || ci->pInputAssemblyState->flags != 0)) {
        *reason = "graphics pipeline input-assembly pNext/flags not supported";
        return false;
    }
    if (ci->pViewportState != nullptr &&
        (ci->pViewportState->pNext != nullptr || ci->pViewportState->flags != 0 ||
         ci->pViewportState->pViewports != nullptr || ci->pViewportState->pScissors != nullptr)) {
        *reason = "graphics pipeline viewport state must be dynamic (no static viewports/scissors)";
        return false;
    }
    if (ci->pRasterizationState != nullptr && ci->pRasterizationState->flags != 0) {
        *reason = "graphics pipeline rasterization flags not supported";
        return false;
    }
    // GL/zink RENDER CORRECTNESS: zink chains depth-clip
    // (VkPipelineRasterizationDepthClipStateCreateInfoEXT / VK_EXT_depth_clip_enable), line state
    // (VkPipelineRasterizationLineStateCreateInfoEXT / VK_EXT_line_rasterization), and -- on a
    // device that enabled VK_EXT_transform_feedback + geometryStreams (allow_rasterization_stream)
    // -- the stream selection (VkPipelineRasterizationStateStreamCreateInfoEXT) on the
    // rasterization pNext. ADMIT exactly those structs (their fields ride the wire + the worker
    // rebuilds the pNext); any OTHER rasterization pNext stays fail-closed (the wire does not carry
    // it). Each admitted sType may appear ONCE -- a duplicate fails closed rather than serializing
    // last-one-wins (the wire carries one flattened copy, so a duplicate would be silently
    // dropped). The stream struct's `flags` is reserved (must be 0) and is not carried.
    if (ci->pRasterizationState != nullptr) {
        bool seen_depth_clip = false;
        bool seen_line_state = false;
        bool seen_stream = false;
        for (auto* n = static_cast<const VkBaseInStructure*>(ci->pRasterizationState->pNext);
             n != nullptr; n = n->pNext) {
            if (n->sType ==
                VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_DEPTH_CLIP_STATE_CREATE_INFO_EXT) {
                if (seen_depth_clip) {
                    *reason = "duplicate depth-clip rasterization pNext";
                    return false;
                }
                seen_depth_clip = true;
            } else if (n->sType ==
                       VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_LINE_STATE_CREATE_INFO_EXT) {
                if (seen_line_state) {
                    *reason = "duplicate line-state rasterization pNext";
                    return false;
                }
                seen_line_state = true;
            } else if (n->sType ==
                       VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_STREAM_CREATE_INFO_EXT) {
                if (!allow_rasterization_stream) {
                    // this sType is KNOWN (and listed as supported below), so the
                    // generic unknown-pNext reason would mislead. This branch is the deliberately
                    // degraded mixed-version path (an old worker would silently DROP the stream
                    // state) or a device missing the ext/feature -- name the actual gate so the
                    // user knows to update the worker, not to doubt the sType.
                    *reason = "rasterization-stream pNext rejected: it needs the enabled "
                              "VK_EXT_transform_feedback + geometryStreams feature and a "
                              "stream-capable worker (an old worker cannot serve it -- update it)";
                    return false;
                }
                if (seen_stream) {
                    *reason = "duplicate rasterization-stream pNext";
                    return false;
                }
                seen_stream = true;
                const auto* ss =
                    reinterpret_cast<const VkPipelineRasterizationStateStreamCreateInfoEXT*>(n);
                if (ss->flags != 0) {
                    *reason = "rasterization-stream flags are reserved (must be 0)";
                    return false;
                }
            } else {
                *reason = "graphics pipeline rasterization pNext not supported (only "
                          "depth-clip/line/stream)";
                return false;
            }
        }
    }
    if (ci->pMultisampleState != nullptr &&
        (ci->pMultisampleState->pNext != nullptr || ci->pMultisampleState->flags != 0 ||
         ci->pMultisampleState->rasterizationSamples > VK_SAMPLE_COUNT_32_BIT)) {
        // The sample MASK is carried (one VkSampleMask word, valid for <=32 samples); >32-sample
        // masks would need a second word, so cap there.
        *reason = "graphics pipeline multisample pNext/flags not supported (or >32 samples)";
        return false;
    }
    if (ci->pDepthStencilState != nullptr &&
        (ci->pDepthStencilState->pNext != nullptr || ci->pDepthStencilState->flags != 0)) {
        *reason = "graphics pipeline depth-stencil pNext/flags not supported";
        return false;
    }
    if (ci->pColorBlendState != nullptr &&
        (ci->pColorBlendState->pNext != nullptr || ci->pColorBlendState->flags != 0)) {
        *reason = "graphics pipeline color-blend pNext/flags not supported";
        return false;
    }
    if (ci->pDynamicState != nullptr &&
        (ci->pDynamicState->pNext != nullptr || ci->pDynamicState->flags != 0)) {
        *reason = "graphics pipeline dynamic-state pNext/flags not supported";
        return false;
    }
    return true;
}

// --- Command predicates (the void vkCmd* set the command buffer locally invalid on false) --------

inline bool begin_render_pass_ok(const VkRenderPassBeginInfo* bi, VkSubpassContents contents,
                                 const char** reason) {
    if (contents != VK_SUBPASS_CONTENTS_INLINE) {
        *reason = "begin_render_pass must be INLINE (no secondary command buffers)";
        return false;
    }
    // GL/zink: an imageless framebuffer's begin chains a VkRenderPassAttachmentBeginInfo
    // (the deferred attachment views). Accept it (the recorder carries the views); reject other
    // pNext.
    for (auto* n = static_cast<const VkBaseInStructure*>(bi->pNext); n != nullptr; n = n->pNext) {
        if (n->sType != VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO) {
            *reason = "begin_render_pass pNext not supported (only attachment-begin)";
            return false;
        }
    }
    // GL/zink: FAITHFUL clear forwarding -- the recorder carries the EXACT clear-value
    // array (count + raw bytes), so any count zink supplies (0 for an all-LOAD pass, N to cover N
    // attachments) is forwarded verbatim and the host driver matches them to the render pass. Only
    // a structurally impossible request (a positive count with a null array) is rejected.
    if (bi->clearValueCount > 0 && bi->pClearValues == nullptr) {
        *reason = "begin_render_pass has a clear count but a null clear-value array";
        return false;
    }
    return true;
}

inline bool bind_pipeline_ok(VkPipelineBindPoint bind_point, const char** reason) {
    // Compute: GRAPHICS or COMPUTE; anything else (ray tracing, ...) stays a named
    // reject. The bind point rides the wire and the worker cross-checks it against the
    // pipeline's KIND, so a mismatch can never silently bind.
    if (bind_point != VK_PIPELINE_BIND_POINT_GRAPHICS &&
        bind_point != VK_PIPELINE_BIND_POINT_COMPUTE) {
        *reason = "bind_pipeline must be GRAPHICS or COMPUTE";
        return false;
    }
    return true;
}

// Compute: FAITHFUL compute-pipeline admission -- fail-closed on everything the wire
// does not carry: derivatives, SPECIALIZATION constants (a named reject; carrying them is a
// separate follow-up), and any shape the 1.3 allow_* bools (documented above
// graphics_pipeline_ok) do not admit. The stage must be exactly COMPUTE with a module + a name.
inline bool compute_pipeline_ok(const VkComputePipelineCreateInfo* ci,
                                bool allow_cache_control_flags, bool allow_subgroup_size_control,
                                bool allow_full_subgroups, bool allow_pipeline_feedback,
                                const char** reason) {
    VkPipelineCreateFlags allowed_flags = 0;
    if (allow_cache_control_flags) {
        allowed_flags |= VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT |
                         VK_PIPELINE_CREATE_EARLY_RETURN_ON_FAILURE_BIT;
    }
    if ((ci->flags & ~allowed_flags) != 0) {
        *reason = "compute pipeline flags not supported";
        return false;
    }
    for (auto* n = static_cast<const VkBaseInStructure*>(ci->pNext); n != nullptr; n = n->pNext) {
        if (allow_pipeline_feedback &&
            n->sType == VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO) {
            continue; // core-1.3 feedback; the entry clears the VALID bits (= no feedback)
        }
        *reason = "compute pipeline pNext not supported";
        return false;
    }
    // The flags mask above already excludes DERIVATIVE_BIT (the index field is ignored without
    // it, and apps legitimately leave it 0); a non-null base handle is still rejected
    // explicitly, matching graphics_pipeline_ok.
    if (ci->basePipelineHandle != VK_NULL_HANDLE) {
        *reason = "compute pipeline derivatives not supported";
        return false;
    }
    const VkPipelineShaderStageCreateInfo& s = ci->stage;
    if (!pipeline_stage_shape_ok(s, allow_subgroup_size_control, allow_full_subgroups,
                                 /*is_compute=*/true, reason)) {
        return false;
    }
    if (s.stage != VK_SHADER_STAGE_COMPUTE_BIT || s.module == VK_NULL_HANDLE ||
        s.pName == nullptr || s.pName[0] == '\0') {
        *reason = "compute pipeline requires one COMPUTE stage with a module and entry point";
        return false;
    }
    return true;
}

inline bool set_viewport_ok(std::uint32_t first, std::uint32_t count, const char** reason) {
    if (first != 0 || count != 1) {
        *reason = "set_viewport must be firstViewport 0, count 1";
        return false;
    }
    return true;
}

inline bool set_scissor_ok(std::uint32_t first, std::uint32_t count, const char** reason) {
    if (first != 0 || count != 1) {
        *reason = "set_scissor must be firstScissor 0, count 1";
        return false;
    }
    return true;
}

} // namespace vkr::icd_subset

#endif // VKRELAY2_LINUX_ICD_ICD_SUBSET_H
