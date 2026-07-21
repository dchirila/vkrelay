// vkrelay2 (GL/zink): pack/unpack the 55 VkPhysicalDeviceFeatures VkBool32 fields to/from a
// u64 bitmask, so the platform-neutral wire layer (vulkan_session.*) can carry the host's real
// feature set WITHOUT including <vulkan/vulkan.h>. Both the worker (packs the real host features)
// and the Linux ICD (unpacks them for the app) include THIS header, so the bit order is defined
// once.
//
// VkPhysicalDeviceFeatures is a frozen core-1.0 struct of exactly 55 VkBool32 members; 55 <= 64, so
// a single u64 carries the whole set. The field list below is the canonical declaration order; the
// shared X-macro guarantees pack() and unpack() never drift.
#pragma once

#include <cstdint>

#include <vulkan/vulkan.h>

namespace vkr::vkrpc {

// The 55 VkPhysicalDeviceFeatures members, in declaration order (the wire bit index is the
// position).
#define VKR_FOR_EACH_PHYSICAL_DEVICE_FEATURE(X)                                                    \
    X(robustBufferAccess)                                                                          \
    X(fullDrawIndexUint32)                                                                         \
    X(imageCubeArray)                                                                              \
    X(independentBlend)                                                                            \
    X(geometryShader)                                                                              \
    X(tessellationShader)                                                                          \
    X(sampleRateShading)                                                                           \
    X(dualSrcBlend)                                                                                \
    X(logicOp)                                                                                     \
    X(multiDrawIndirect)                                                                           \
    X(drawIndirectFirstInstance)                                                                   \
    X(depthClamp)                                                                                  \
    X(depthBiasClamp)                                                                              \
    X(fillModeNonSolid)                                                                            \
    X(depthBounds)                                                                                 \
    X(wideLines)                                                                                   \
    X(largePoints)                                                                                 \
    X(alphaToOne)                                                                                  \
    X(multiViewport)                                                                               \
    X(samplerAnisotropy)                                                                           \
    X(textureCompressionETC2)                                                                      \
    X(textureCompressionASTC_LDR)                                                                  \
    X(textureCompressionBC)                                                                        \
    X(occlusionQueryPrecise)                                                                       \
    X(pipelineStatisticsQuery)                                                                     \
    X(vertexPipelineStoresAndAtomics)                                                              \
    X(fragmentStoresAndAtomics)                                                                    \
    X(shaderTessellationAndGeometryPointSize)                                                      \
    X(shaderImageGatherExtended)                                                                   \
    X(shaderStorageImageExtendedFormats)                                                           \
    X(shaderStorageImageMultisample)                                                               \
    X(shaderStorageImageReadWithoutFormat)                                                         \
    X(shaderStorageImageWriteWithoutFormat)                                                        \
    X(shaderUniformBufferArrayDynamicIndexing)                                                     \
    X(shaderSampledImageArrayDynamicIndexing)                                                      \
    X(shaderStorageBufferArrayDynamicIndexing)                                                     \
    X(shaderStorageImageArrayDynamicIndexing)                                                      \
    X(shaderClipDistance)                                                                          \
    X(shaderCullDistance)                                                                          \
    X(shaderFloat64)                                                                               \
    X(shaderInt64)                                                                                 \
    X(shaderInt16)                                                                                 \
    X(shaderResourceResidency)                                                                     \
    X(shaderResourceMinLod)                                                                        \
    X(sparseBinding)                                                                               \
    X(sparseResidencyBuffer)                                                                       \
    X(sparseResidencyImage2D)                                                                      \
    X(sparseResidencyImage3D)                                                                      \
    X(sparseResidency2Samples)                                                                     \
    X(sparseResidency4Samples)                                                                     \
    X(sparseResidency8Samples)                                                                     \
    X(sparseResidency16Samples)                                                                    \
    X(sparseResidencyAliased)                                                                      \
    X(variableMultisampleRate)                                                                     \
    X(inheritedQueries)

constexpr std::uint64_t pack_physical_device_features(const VkPhysicalDeviceFeatures& f) {
    std::uint64_t bits = 0;
    int i = 0;
#define VKR_PACK_ONE(name)                                                                         \
    if (f.name != VK_FALSE) {                                                                      \
        bits |= (std::uint64_t{1} << i);                                                           \
    }                                                                                              \
    ++i;
    VKR_FOR_EACH_PHYSICAL_DEVICE_FEATURE(VKR_PACK_ONE)
#undef VKR_PACK_ONE
    return bits;
}

inline VkPhysicalDeviceFeatures unpack_physical_device_features(std::uint64_t bits) {
    VkPhysicalDeviceFeatures f{};
    int i = 0;
#define VKR_UNPACK_ONE(name)                                                                       \
    f.name = ((bits >> i) & 0x1u) != 0 ? VK_TRUE : VK_FALSE;                                       \
    ++i;
    VKR_FOR_EACH_PHYSICAL_DEVICE_FEATURE(VKR_UNPACK_ONE)
#undef VKR_UNPACK_ONE
    return f;
}

} // namespace vkr::vkrpc
