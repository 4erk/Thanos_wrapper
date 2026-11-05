#pragma once
#include <vulkan/vulkan.h>

namespace thanosray::vk::robustness {
// Storage must outlive the real vkCreateDevice call. Apply only to the proxy's
// copied feature chain; never modify application-owned structures.
struct Storage {
    VkPhysicalDeviceFeatures core{};
    VkPhysicalDeviceRobustness2FeaturesEXT extra{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT};
};
struct Result { VkBool32 buffer{}, buffer2{}, image2{}, nullDescriptor{}; bool needsExtension{}; };

template<class T> inline bool UpdatesBuffersAfterBind(const T& f)
{
    return f.descriptorBindingUniformBufferUpdateAfterBind || f.descriptorBindingStorageBufferUpdateAfterBind ||
        f.descriptorBindingUniformTexelBufferUpdateAfterBind || f.descriptorBindingStorageTexelBufferUpdateAfterBind;
}

inline Result Apply(VkDeviceCreateInfo& info, Storage& storage,
    const VkPhysicalDeviceFeatures& nativeCore, const VkPhysicalDeviceRobustness2FeaturesEXT& nativeExtra,
    bool extensionSupported, bool robustUpdateAfterBind)
{
    VkPhysicalDeviceFeatures2* core2 = nullptr;
    VkPhysicalDeviceRobustness2FeaturesEXT* extra = nullptr;
    bool updatesBuffers = false;
    for (auto* node = reinterpret_cast<VkBaseOutStructure*>(const_cast<void*>(info.pNext)); node; node = node->pNext) {
        if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2)
            core2 = reinterpret_cast<VkPhysicalDeviceFeatures2*>(node);
        if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT)
            extra = reinterpret_cast<VkPhysicalDeviceRobustness2FeaturesEXT*>(node);
        if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES)
            updatesBuffers |= UpdatesBuffersAfterBind(*reinterpret_cast<VkPhysicalDeviceVulkan12Features*>(node));
        if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES)
            updatesBuffers |= UpdatesBuffersAfterBind(*reinterpret_cast<VkPhysicalDeviceDescriptorIndexingFeatures*>(node));
    }
    storage.core = core2 ? core2->features : (info.pEnabledFeatures ? *info.pEnabledFeatures : VkPhysicalDeviceFeatures{});
    const bool enableBuffer = nativeCore.robustBufferAccess && (!updatesBuffers || robustUpdateAfterBind);
    if (enableBuffer) storage.core.robustBufferAccess = VK_TRUE;
    if (core2) core2->features = storage.core;
    else info.pEnabledFeatures = &storage.core;

    if (extensionSupported) {
        if (!extra) {
            extra = &storage.extra;
            extra->pNext = const_cast<void*>(info.pNext);
            info.pNext = extra;
        }
        extra->robustBufferAccess2 |= enableBuffer && nativeExtra.robustBufferAccess2;
        extra->robustImageAccess2 |= nativeExtra.robustImageAccess2;
        extra->nullDescriptor |= nativeExtra.nullDescriptor;
    }
    return {storage.core.robustBufferAccess, extra ? extra->robustBufferAccess2 : VK_FALSE,
        extra ? extra->robustImageAccess2 : VK_FALSE, extra ? extra->nullDescriptor : VK_FALSE, extensionSupported};
}
}
