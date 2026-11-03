#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <vulkan/vulkan.h>

namespace thanosray::vk {

static constexpr int kProcTableSize = 265;

// src/vk/VkProxyState.h

struct VkProxyState {
    HMODULE realModule = nullptr;
    volatile LONG initialized = 0;

    PFN_vkGetInstanceProcAddr real_vkGetInstanceProcAddr = nullptr;
    PFN_vkGetDeviceProcAddr   real_vkGetDeviceProcAddr = nullptr;
    PFN_vkEnumerateDeviceExtensionProperties real_vkEnumerateDeviceExtensionProperties = nullptr;
    PFN_vkGetPhysicalDeviceProperties2       real_vkGetPhysicalDeviceProperties2 = nullptr;


    // ADD THIS:
    PFN_vkGetPhysicalDeviceFeatures2         real_vkGetPhysicalDeviceFeatures2 = nullptr;
    PFN_vkCreateDevice real_vkCreateDevice = nullptr;

};


VkProxyState& State();

} // namespace thanosray::vk
