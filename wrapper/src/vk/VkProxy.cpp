//#include "pch.h"
#include "VkProxy.h"
#include "VkProxyState.h"
#include "VkProxyExports.h"
#include "VkHooks.h"

#include "../io/PathUtil.h"
#include "../log/Log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace thanosray::vk {

    static VkProxyState g_state;

    VkProxyState& State()
    {
        return g_state;
    }

    static constexpr int IDX_vkEnumerateDeviceExtensionProperties = 160; // 0-based index
    static constexpr int IDX_vkGetDeviceProcAddr = 185;
    static constexpr int IDX_vkGetInstanceProcAddr = 201;

    void Initialize()
    {
        // One-time init guard
        if (InterlockedCompareExchange(&g_state.initialized, 1, 0) != 0) {
            return;
        }

        thanosray::log::Logf("=== vkproxy init ===");
        thanosray::log::Logf("=== BUILD TAG: 2026-01-20 C (proxy) ===");

        wchar_t realPath[MAX_PATH] = { 0 };
        if (!thanosray::io::BuildSiblingPath(realPath, MAX_PATH, L"vulkan-1_real.dll")) {
            thanosray::log::Logf("[FATAL] Failed to build path to vulkan-1_real.dll");
            return;
        }

        g_state.realModule = LoadLibraryW(realPath);
        if (!g_state.realModule) {
            thanosray::log::Logf("[FATAL] LoadLibraryW failed for vulkan-1_real.dll");
            MessageBoxA(nullptr,
                "vkproxy: Failed to load vulkan-1_real.dll.\n"
                "Copy C:\\Windows\\System32\\vulkan-1.dll into the game folder and rename it to vulkan-1_real.dll.",
                "vkproxy",
                MB_ICONERROR);
            return;
        }

        thanosray::log::Logf("Loaded vulkan-1_real.dll OK");

        // Fill proc table used by MASM thunks
        const char* const* names = ExportNameTable();
        for (int i = 0; i < kProcTableSize; i++) {
            FARPROC p = GetProcAddress(g_state.realModule, names[i]);
            g_procTable[i] = (void*)p;
            if (!p) {
                thanosray::log::Logf("[WARN] Missing export: %s (idx=%d)", names[i], i);
            }
        }

        // Resolve real core entrypoints from the filled table
        g_state.real_vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)g_procTable[IDX_vkGetInstanceProcAddr];
        g_state.real_vkGetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)g_procTable[IDX_vkGetDeviceProcAddr];
        g_state.real_vkEnumerateDeviceExtensionProperties =
            (PFN_vkEnumerateDeviceExtensionProperties)g_procTable[IDX_vkEnumerateDeviceExtensionProperties];

        // vkGetPhysicalDeviceProperties2 (try KHR alias too)
        g_state.real_vkGetPhysicalDeviceProperties2 =
            (PFN_vkGetPhysicalDeviceProperties2)GetProcAddress(g_state.realModule, "vkGetPhysicalDeviceProperties2");
        if (!g_state.real_vkGetPhysicalDeviceProperties2) {
            g_state.real_vkGetPhysicalDeviceProperties2 =
                (PFN_vkGetPhysicalDeviceProperties2)GetProcAddress(g_state.realModule, "vkGetPhysicalDeviceProperties2KHR");
        }

        // vkGetPhysicalDeviceFeatures2 (try KHR alias too)
        g_state.real_vkGetPhysicalDeviceFeatures2 =
            (PFN_vkGetPhysicalDeviceFeatures2)GetProcAddress(g_state.realModule, "vkGetPhysicalDeviceFeatures2");
        if (!g_state.real_vkGetPhysicalDeviceFeatures2) {
            g_state.real_vkGetPhysicalDeviceFeatures2 =
                (PFN_vkGetPhysicalDeviceFeatures2)GetProcAddress(g_state.realModule, "vkGetPhysicalDeviceFeatures2KHR");
        }

        // vkCreateDevice
        g_state.real_vkCreateDevice =
            (PFN_vkCreateDevice)GetProcAddress(g_state.realModule, "vkCreateDevice");

        if (!g_state.real_vkGetInstanceProcAddr || !g_state.real_vkGetDeviceProcAddr) {
            thanosray::log::Logf("[FATAL] Missing core procaddrs from real vulkan-1.dll");
            return;
        }
        if (!g_state.real_vkEnumerateDeviceExtensionProperties) {
            thanosray::log::Logf("[FATAL] Missing real vkEnumerateDeviceExtensionProperties");
            return;
        }
        if (!g_state.real_vkGetPhysicalDeviceFeatures2) {
            thanosray::log::Logf("[FATAL] Missing real vkGetPhysicalDeviceFeatures2 (and KHR alias) from vulkan-1_real.dll");
            return;
        }
        if (!g_state.real_vkCreateDevice) {
            thanosray::log::Logf("[FATAL] Missing real vkCreateDevice from vulkan-1_real.dll");
            return;
        }

        InstallHooks();
        thanosray::log::Logf("=== vkproxy ready ===");
    }

    void Shutdown()
    {
        thanosray::log::Shutdown();
    }

} // namespace thanosray::vk
