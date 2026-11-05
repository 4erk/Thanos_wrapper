//#include "pch.h"
#include "VkHooks.h"
#include "VkProxyExports.h"
#include "VkProxyState.h"
#include "RtFallback.h"
#include "ShaderDiagnostics.h"
#include "RuntimeDiagnostics.h"
#include "RobustAccess.h"


#include "../log/Log.h"
#include "../../settings.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <mutex>
#include <unordered_map>
#include <atomic>
#include <fstream>
#include <cstdlib>
#include <vector>
#include <string>
#include <unordered_set>
#include <algorithm>
#include <cstring>
#include <cstdint>
#include <type_traits>
using namespace thanosray::log;
static const char* kVkHooksBuildId =
"VkHooks.cpp BUILD_ID 4erk-doom-robust-access-6";
// ---- Forward decls (must appear AFTER Vulkan types are visible) ----

// ============================================================================
// LANDMARK 00: VkHooks.cpp (stable baseline + additive debug/ShadowLift support)
// - We do NOT remove existing functionality.
// - Any new behavior is gated and should fail-open (game continues).
// - ShadowLift is optional; logs will tell us if it actually runs.
// ============================================================================

// ============================================================================
// LANDMARK 35: RT output discovery + debug lift (storage-image descriptors)
// Goal:
//  - Track which VkImageViews are written as STORAGE_IMAGE (ray outputs)
//  - After vkCmdTraceRaysKHR, touch those images so we SEE an effect
//
// This is deliberately fail-open and debug-first:
//  - If we can't resolve an image or can't clear it, we just skip.
//  - This should NOT break the wrapper.
// ============================================================================



static std::atomic<bool> g_sl_inited{ false };
// ShadowLift defaults (may be overridden by env/ini later)
static std::atomic<float> g_sl_strength{ 0.00f }; // default lift
static std::atomic<float> g_sl_toe{ 0.30f };
static std::atomic<float> g_sl_ceil{ 1.00f };

// Forward decl so hotkey handler can safely call into the ShadowLift config.
namespace thanosray { namespace vk { static void EnsureShadowLiftDefaultsLate(); } }

// ============================================================================
// LANDMARK HOTKEYS: runtime toggles (no swapchain/image mods unless enabled)
// - Ctrl+Shift+Q toggles "Lantern/ShadowLift" runtime enable.
// - Ctrl+Shift+R reloads Thanos-ray.ini immediately.
// This is intentionally simple and polling-based (no hooks into the game).
// ============================================================================
static std::atomic<bool> g_runtimeLanternEnable{ false };

static bool HotkeyChordDown(int vkKey)
{
    // High bit set => key down.
    const bool ctrl  = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool shift = (GetAsyncKeyState(VK_SHIFT)   & 0x8000) != 0;
    const bool key   = (GetAsyncKeyState(vkKey)      & 0x8000) != 0;
    return ctrl && shift && key;
}

static void PollHotkeys()
{
    // Prove this polling path is actually being executed in-game.
    static bool s_loggedActive = false;
    if (!s_loggedActive) {
        s_loggedActive = true;
        thanosray::log::Logf("[HOTKEY] PollHotkeys active (Ctrl+Shift+Q toggle, Ctrl+Shift+R reload)");
    }

    // Edge-detect so we toggle once per keypress.
    static bool prevToggle = false;
    static bool prevReload = false;

    const bool downToggle = HotkeyChordDown('Q');
    const bool downReload = HotkeyChordDown('R');

    if (downToggle && !prevToggle) {
        const bool newState = !g_runtimeLanternEnable.load(std::memory_order_relaxed);
        g_runtimeLanternEnable.store(newState, std::memory_order_relaxed);

        // When disabled, force lift strength to 0 so even accidental calls are no-op.
        if (!newState) {
            g_sl_strength.store(0.0f, std::memory_order_relaxed);
        } else {
            // If user set THR_SHADOW_LIFT, keep it. Otherwise choose a visible default.
            thanosray::vk::EnsureShadowLiftDefaultsLate();
            float cur = g_sl_strength.load(std::memory_order_relaxed);
            if (cur <= 0.0f) g_sl_strength.store(0.30f, std::memory_order_relaxed);
        }

        thanosray::log::Logf("[HOTKEY] Ctrl+Shift+Q => Lantern %s (lift=%.3f)",
            newState ? "ENABLED" : "DISABLED",
            g_sl_strength.load(std::memory_order_relaxed));
    }

    if (downReload && !prevReload) {
        thanosray::Settings_ReloadNow();
        // Re-apply defaults after reload when enabled
        if (g_runtimeLanternEnable.load(std::memory_order_relaxed)) {
            thanosray::vk::EnsureShadowLiftDefaultsLate();
        }
        thanosray::log::Logf("[HOTKEY] Ctrl+Shift+R => Reloaded INI");
    }

    prevToggle = downToggle;
    prevReload = downReload;
}


namespace thanosray {
    namespace vk {

        struct ShadowLiftDevice;
        struct SwapchainState;

        static void TryShadowLiftFromAcquire(VkDevice device, VkSwapchainKHR swapchain, uint32_t imageIndex);

        static bool ShadowLiftInitLocked(ShadowLiftDevice& d, VkDevice device);
        static bool EnsureSwapchainDescriptorsLocked(ShadowLiftDevice& d, SwapchainState& s);

        static void NoteCreateImage(VkDevice dev, VkImage img, const VkImageCreateInfo& ci);
        static void NoteUpdateDescriptorSets(uint32_t writeCount, const VkWriteDescriptorSet* writes);
        static void NoteCmdBindDescriptorSets(
            VkCommandBuffer cb,
            uint32_t descriptorSetCount,
            const VkDescriptorSet* pSets);
        static void DebugLiftAfterTraceRays(VkCommandBuffer cb);
        static void NoteCreateImageView(VkImageView view, const VkImageViewCreateInfo& vi);



        
        static void EnsureShadowLiftEnv()
        {
            static bool done = false;
            if (done) return;
            done = true;

            char buf[64] = {};
            DWORD n = GetEnvironmentVariableA("THR_SHADOW_LIFT", buf, sizeof(buf));

            if (n == 0) {
                // Postprocessing must remain opt-in; do not alter game images.
                SetEnvironmentVariableA("THR_SHADOW_LIFT", "0.00");
                SetEnvironmentVariableA("THR_SHADOW_TOE", "0.00");
                SetEnvironmentVariableA("THR_SHADOW_CEIL", "1.00");

                thanosray::log::Logf(
                    "[SL] THR_SHADOW_LIFT not set; defaulting to 0.00 (self-initialized)");
            }
            else {
                thanosray::log::Logf(
                    "[SL] THR_SHADOW_LIFT already set to %s", buf);
            }
        }

        static void EnsureShadowLiftDefaultsLate()
        {
            bool expected = false;
            if (!g_sl_inited.compare_exchange_strong(expected, true))
                return;

            // Optional: allow overriding via WinAPI env WITHOUT touching CRT.
            // This is safe even late.
            auto ReadWinEnvFloat = [](const char* name, float& out)->bool {
                char buf[64] = {};
                DWORD n = GetEnvironmentVariableA(name, buf, (DWORD)sizeof(buf));
                if (n == 0 || n >= sizeof(buf)) return false;
                out = (float)atof(buf);
                return true;
                };

            float v;


            if (ReadWinEnvFloat("THR_SHADOW_LIFT", v)) g_sl_strength.store(v);
            if (ReadWinEnvFloat("THR_SHADOW_TOE", v)) g_sl_toe.store(v);
            if (ReadWinEnvFloat("THR_SHADOW_CEIL", v)) g_sl_ceil.store(v);

            thanosray::log::Logf("[SL] defaults active: lift=%.3f toe=%.3f ceil=%.3f",
                g_sl_strength.load(), g_sl_toe.load(), g_sl_ceil.load());
        }


        // -------------------------------------------------------------------------
// LANDMARK: GDPA-captured swapchain entrypoints
// Some engines fetch swapchain/present function pointers directly via
// vkGetDeviceProcAddr and call them (bypassing export thunks). These globals
// store the "real" function pointers returned by the driver.
// -------------------------------------------------------------------------
// Forward declarations for GDPA wrappers
static VkResult VKAPI_CALL HookGDPA_vkCreateSwapchainKHR(
    VkDevice device,
    const VkSwapchainCreateInfoKHR* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkSwapchainKHR* pSwapchain);

static VkResult VKAPI_CALL HookGDPA_vkGetSwapchainImagesKHR(
    VkDevice device,
    VkSwapchainKHR swapchain,
    uint32_t* pSwapchainImageCount,
    VkImage* pSwapchainImages);

//static VkResult VKAPI_CALL HookGDPA_vkQueuePresentKHR(
//    VkQueue queue,
//    const VkPresentInfoKHR* pPresentInfo);

// ============================================================================
// LANDMARK RF30: RT fallback - real fns + hook forward decls
// - These hooks only TRACK state for later RT fallback.
// - They are fail-open: if real pointer is null, we log and return.
// ============================================================================

static PFN_vkUpdateDescriptorSets            g_real_vkUpdateDescriptorSets = nullptr;
static PFN_vkUpdateDescriptorSetWithTemplate g_real_vkUpdateDescriptorSetWithTemplate = nullptr;
static PFN_vkCmdBindDescriptorSets           g_real_vkCmdBindDescriptorSets = nullptr;
static PFN_vkCmdBindPipeline                 g_real_vkCmdBindPipeline = nullptr;

static PFN_vkCreateImage                     g_real_vkCreateImage = nullptr;
static PFN_vkDestroyImage                    g_real_vkDestroyImage = nullptr;
static PFN_vkCreateImageView                 g_real_vkCreateImageView = nullptr;
static PFN_vkDestroyImageView                g_real_vkDestroyImageView = nullptr;

// For Phase 0b (optional ambient clear via vkCmdClearColorImage)
static PFN_vkCmdPipelineBarrier              g_real_vkCmdPipelineBarrier = nullptr;
static PFN_vkCmdClearColorImage              g_real_vkCmdClearColorImage = nullptr;


// ---- Extra RT/AS logging hooks (pass-through when possible) ----
static PFN_vkGetBufferDeviceAddress            g_real_vkGetBufferDeviceAddress = nullptr;
static PFN_vkGetBufferDeviceAddressKHR         g_real_vkGetBufferDeviceAddressKHR = nullptr;

#ifdef VK_KHR_ray_tracing_pipeline
static PFN_vkCmdTraceRaysKHR g_real_vkCmdTraceRaysKHR = nullptr;
static PFN_vkCmdTraceRaysIndirectKHR g_real_vkCmdTraceRaysIndirectKHR = nullptr;
static PFN_vkCmdTraceRaysIndirect2KHR g_real_vkCmdTraceRaysIndirect2KHR = nullptr;
static PFN_vkCreateRayTracingPipelinesKHR      g_real_vkCreateRayTracingPipelinesKHR = nullptr;
static PFN_vkGetRayTracingShaderGroupHandlesKHR g_real_vkGetRayTracingShaderGroupHandlesKHR = nullptr;
static PFN_vkGetRayTracingShaderGroupHandlesKHR g_real_vkGetRayTracingShaderGroupHandlesKHR2 = nullptr; // alias if needed
static PFN_vkGetRayTracingShaderGroupStackSizeKHR g_real_vkGetRayTracingShaderGroupStackSizeKHR = nullptr;
#endif

#ifdef VK_KHR_acceleration_structure
static PFN_vkCreateAccelerationStructureKHR     g_real_vkCreateAccelerationStructureKHR = nullptr;
static PFN_vkDestroyAccelerationStructureKHR    g_real_vkDestroyAccelerationStructureKHR = nullptr;
static PFN_vkGetAccelerationStructureBuildSizesKHR g_real_vkGetAccelerationStructureBuildSizesKHR = nullptr;
static PFN_vkCmdBuildAccelerationStructuresKHR  g_real_vkCmdBuildAccelerationStructuresKHR = nullptr;
static PFN_vkBuildAccelerationStructuresKHR     g_real_vkBuildAccelerationStructuresKHR = nullptr;
static PFN_vkGetAccelerationStructureDeviceAddressKHR g_real_vkGetAccelerationStructureDeviceAddressKHR = nullptr;
static PFN_vkCmdCopyAccelerationStructureKHR    g_real_vkCmdCopyAccelerationStructureKHR = nullptr;
static PFN_vkCmdWriteAccelerationStructuresPropertiesKHR g_real_vkCmdWriteAccelerationStructuresPropertiesKHR = nullptr;
#endif


static PFN_vkGetDeviceProcAddr   g_real_vkGetDeviceProcAddr = nullptr;
static PFN_vkGetInstanceProcAddr g_real_vkGetInstanceProcAddr = nullptr;



// Hook wrappers
static void VKAPI_CALL Hook_vkUpdateDescriptorSets(
    VkDevice device, uint32_t descriptorWriteCount,
    const VkWriteDescriptorSet* pDescriptorWrites,
    uint32_t descriptorCopyCount,
    const VkCopyDescriptorSet* pDescriptorCopies);

static void VKAPI_CALL Hook_vkUpdateDescriptorSetWithTemplate(
    VkDevice device, VkDescriptorSet descriptorSet,
    VkDescriptorUpdateTemplate descriptorUpdateTemplate,
    const void* pData);

static void VKAPI_CALL Hook_vkCmdBindDescriptorSets(
    VkCommandBuffer commandBuffer,
    VkPipelineBindPoint pipelineBindPoint,
    VkPipelineLayout layout,
    uint32_t firstSet,
    uint32_t descriptorSetCount,
    const VkDescriptorSet* pDescriptorSets,
    uint32_t dynamicOffsetCount,
    const uint32_t* pDynamicOffsets);

static void VKAPI_CALL Hook_vkCmdBindPipeline(
    VkCommandBuffer commandBuffer,
    VkPipelineBindPoint pipelineBindPoint,
    VkPipeline pipeline);

static VkResult VKAPI_CALL Hook_vkCreateImage(
    VkDevice device,
    const VkImageCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkImage* pImage);

static void VKAPI_CALL Hook_vkDestroyImage(
    VkDevice device,
    VkImage image,
    const VkAllocationCallbacks* pAllocator);

static VkResult VKAPI_CALL Hook_vkCreateImageView(
    VkDevice device,
    const VkImageViewCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkImageView* pView);

static void VKAPI_CALL Hook_vkDestroyImageView(
    VkDevice device,
    VkImageView view,
    const VkAllocationCallbacks* pAllocator);

static VkResult VKAPI_CALL Hook_vkAcquireNextImageKHR(
    VkDevice device,
    VkSwapchainKHR swapchain,
    uint64_t timeout,
    VkSemaphore semaphore,
    VkFence fence,
    uint32_t* pImageIndex);
static VkResult VKAPI_CALL Hook_vkAcquireNextImage2KHR(
    VkDevice device,
    const VkAcquireNextImageInfoKHR* pAcquireInfo,
    uint32_t* pImageIndex);



        // Export indices (0-based) for the System32 vulkan-1.dll export order that the project was generated from.
        static constexpr int IDX_vkEnumerateDeviceExtensionProperties = 160;
        static constexpr int IDX_vkGetDeviceProcAddr = 185;
        static constexpr int IDX_vkGetDeviceQueue = 186;
        static constexpr int IDX_vkGetDeviceQueue2 = 187;
        // Swapchain export indices (from exports_cpp.inc)
        static constexpr int IDX_vkAcquireNextImage2KHR = 0;
        static constexpr int IDX_vkAcquireNextImageKHR  = 1;
        static constexpr int IDX_vkCreateSwapchainKHR   = 131;
        static constexpr int IDX_vkDestroySwapchainKHR  = 157;
        static constexpr int IDX_vkGetSwapchainImagesKHR= 238;
        static constexpr int IDX_vkQueuePresentKHR      = 244;
        static constexpr int IDX_vkShowKeys = 0; // unused placeholder, keep build stable
        static constexpr int IDX_vkGetInstanceProcAddr = 201;
        // LANDMARK RF-INDEX: Entry points we hook for RT fallback tracking
        static constexpr int IDX_vkUpdateDescriptorSets = 262;
        static constexpr int IDX_vkUpdateDescriptorSetWithTemplate = 261;
        static constexpr int IDX_vkCmdBindDescriptorSets = 14;
        static constexpr int IDX_vkCmdBindPipeline = 18;
        static constexpr int IDX_vkCreateImage = 117;
        static constexpr int IDX_vkCreateImageView = 118;
        static constexpr int IDX_vkDestroyImage = 143;
        static constexpr int IDX_vkDestroyImageView = 144;
        static constexpr int IDX_vkCmdPipelineBarrier = 52;
        static constexpr int IDX_vkCmdClearColorImage = 24;



        // Added (from your exports_cpp.inc export order)
        static constexpr int IDX_vkGetPhysicalDeviceFeatures2 = 210;
        static constexpr int IDX_vkGetPhysicalDeviceProperties2 = 219;

        static constexpr int IDX_vkCreateDevice = 109;

        // ---- Spoofed extension list (what the engine "sees") ----
        static const char* kSpoofExts[] = {
            // RT set
            "VK_KHR_deferred_host_operations",
            "VK_KHR_acceleration_structure",
            "VK_KHR_ray_tracing_pipeline",
            "VK_KHR_ray_query",

            // "baseline" requirements
            "VK_KHR_shader_draw_parameters",
            "VK_KHR_8bit_storage",
            "VK_KHR_16bit_storage",
            "VK_KHR_buffer_device_address",
            "VK_EXT_descriptor_indexing",
            "VK_KHR_separate_depth_stencil_layouts",
            "VK_KHR_shader_float16_int8",

            // used for nullDescriptor in some engines
            "VK_EXT_robustness2",
        };
        static constexpr uint32_t kSpoofExtCount = (uint32_t)(sizeof(kSpoofExts) / sizeof(kSpoofExts[0]));
        // -----------------------------------------------------------------------------
// LANDMARK: GDPA swapchain hook globals (captured real function pointers)
// These are filled when vkGetDeviceProcAddr returns the real function pointer.
// -----------------------------------------------------------------------------
        static PFN_vkCreateSwapchainKHR      g_gdpa_vkCreateSwapchainKHR = nullptr;
        static PFN_vkDestroySwapchainKHR     g_gdpa_vkDestroySwapchainKHR = nullptr;
        static PFN_vkGetSwapchainImagesKHR   g_gdpa_vkGetSwapchainImagesKHR = nullptr;
        static PFN_vkAcquireNextImageKHR     g_gdpa_vkAcquireNextImageKHR = nullptr;
        static PFN_vkAcquireNextImage2KHR    g_gdpa_vkAcquireNextImage2KHR = nullptr;
        static PFN_vkQueuePresentKHR         g_gdpa_vkQueuePresentKHR = nullptr;
        static PFN_vkGetDeviceQueue  g_gdpa_vkGetDeviceQueue = nullptr;
        static PFN_vkGetDeviceQueue2 g_gdpa_vkGetDeviceQueue2 = nullptr;
        static PFN_vkGetDeviceQueue  g_real_vkGetDeviceQueue = nullptr;
        static PFN_vkGetDeviceQueue2 g_real_vkGetDeviceQueue2 = nullptr;
        static PFN_vkQueuePresentKHR g_real_vkQueuePresentKHR = nullptr;

        /*
        static float ShadowLiftStrength() { return g_sl_strength.load(std::memory_order_relaxed); }
        static float ShadowLiftToe() { return g_sl_toe.load(std::memory_order_relaxed); }
        static float ShadowLiftCeil() { return g_sl_ceil.load(std::memory_order_relaxed); }
        */

        // -----------------------------------------------------------------------------
// LANDMARK: GDPA swapchain hook forward declarations
// -----------------------------------------------------------------------------
        static VkResult VKAPI_CALL HookGDPA_vkCreateSwapchainKHR(
            VkDevice device,
            const VkSwapchainCreateInfoKHR* pCreateInfo,
            const VkAllocationCallbacks* pAllocator,
            VkSwapchainKHR* pSwapchain);

        static void VKAPI_CALL HookGDPA_vkDestroySwapchainKHR(
            VkDevice device,
            VkSwapchainKHR swapchain,
            const VkAllocationCallbacks* pAllocator);

        static VkResult VKAPI_CALL HookGDPA_vkGetSwapchainImagesKHR(
            VkDevice device,
            VkSwapchainKHR swapchain,
            uint32_t* pSwapchainImageCount,
            VkImage* pSwapchainImages);

        static VkResult VKAPI_CALL HookGDPA_vkAcquireNextImageKHR(
            VkDevice device,
            VkSwapchainKHR swapchain,
            uint64_t timeout,
            VkSemaphore semaphore,
            VkFence fence,
            uint32_t* pImageIndex);

        static VkResult VKAPI_CALL HookGDPA_vkAcquireNextImage2KHR(
            VkDevice device,
            const VkAcquireNextImageInfoKHR* pAcquireInfo,
            uint32_t* pImageIndex);

        static VkResult VKAPI_CALL HookGDPA_vkQueuePresentKHR(
            VkQueue queue,
            const VkPresentInfoKHR* pPresentInfo);
        static void ApplyShadowLiftToSwapchainImage(
            VkQueue queue,
            const VkPresentInfoKHR* presentInfo,   // <-- NEW (can be null)
            VkSwapchainKHR sc,
            uint32_t imageIndex);

        static void VKAPI_CALL Hook_vkCmdTraceRaysKHR(
            VkCommandBuffer commandBuffer,
            const VkStridedDeviceAddressRegionKHR*,
            const VkStridedDeviceAddressRegionKHR*,
            const VkStridedDeviceAddressRegionKHR*,
            const VkStridedDeviceAddressRegionKHR*,
            uint32_t, uint32_t, uint32_t);

        static VkResult VKAPI_CALL Hook_vkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* pPresentInfo);



        // ---- RT/AS entrypoints we may want to log (GDPA hook provides them) ----
        static VkDeviceAddress VKAPI_CALL Hook_vkGetBufferDeviceAddress(VkDevice device, const VkBufferDeviceAddressInfo* pInfo);

#ifdef VK_KHR_ray_tracing_pipeline
        static VkResult VKAPI_CALL Hook_vkCreateRayTracingPipelinesKHR(
            VkDevice device, VkDeferredOperationKHR deferredOperation, VkPipelineCache pipelineCache,
            uint32_t createInfoCount, const VkRayTracingPipelineCreateInfoKHR* pCreateInfos,
            const VkAllocationCallbacks* pAllocator, VkPipeline* pPipelines);

        static VkResult VKAPI_CALL Hook_vkGetRayTracingShaderGroupHandlesKHR(
            VkDevice device, VkPipeline pipeline,
            uint32_t firstGroup, uint32_t groupCount, size_t dataSize, void* pData);
#endif

#ifdef VK_KHR_acceleration_structure
        static VkResult VKAPI_CALL Hook_vkCreateAccelerationStructureKHR(
            VkDevice device, const VkAccelerationStructureCreateInfoKHR* pCreateInfo,
            const VkAllocationCallbacks* pAllocator, VkAccelerationStructureKHR* pAS);

        static void VKAPI_CALL Hook_vkDestroyAccelerationStructureKHR(
            VkDevice device, VkAccelerationStructureKHR as, const VkAllocationCallbacks* pAllocator);

        static void VKAPI_CALL Hook_vkGetAccelerationStructureBuildSizesKHR(
            VkDevice device, VkAccelerationStructureBuildTypeKHR buildType,
            const VkAccelerationStructureBuildGeometryInfoKHR* pBuildInfo,
            const uint32_t* pMaxPrimitiveCounts,
            VkAccelerationStructureBuildSizesInfoKHR* pSizeInfo);

        static void VKAPI_CALL Hook_vkCmdBuildAccelerationStructuresKHR(
            VkCommandBuffer cb, uint32_t infoCount,
            const VkAccelerationStructureBuildGeometryInfoKHR* pInfos,
            const VkAccelerationStructureBuildRangeInfoKHR* const* ppBuildRangeInfos);

        static VkDeviceAddress VKAPI_CALL Hook_vkGetAccelerationStructureDeviceAddressKHR(
            VkDevice device, const VkAccelerationStructureDeviceAddressInfoKHR* pInfo);

        static void VKAPI_CALL Hook_vkCmdCopyAccelerationStructureKHR(
            VkCommandBuffer cb, const VkCopyAccelerationStructureInfoKHR* pInfo);
#endif



        static void* HookReturnIfMatch(const char* name)
        {
            if (!name) return nullptr;

            // Descriptor tracking
            if (strcmp(name, "vkUpdateDescriptorSets") == 0) {
                return (void*)&Hook_vkUpdateDescriptorSets;
            }
            if (strcmp(name, "vkUpdateDescriptorSetWithTemplate") == 0) {
                return (void*)&Hook_vkUpdateDescriptorSetWithTemplate;
            }

            // Bind tracking
            if (strcmp(name, "vkCmdBindDescriptorSets") == 0) {
                return (void*)&Hook_vkCmdBindDescriptorSets;
            }
            if (strcmp(name, "vkCmdBindPipeline") == 0) {
                return (void*)&Hook_vkCmdBindPipeline;
            }

            // Resource tracking (optional but useful)
            if (strcmp(name, "vkCreateImage") == 0) {
                return (void*)&Hook_vkCreateImage;
            }
            if (strcmp(name, "vkDestroyImage") == 0) {
                return (void*)&Hook_vkDestroyImage;
            }
            if (strcmp(name, "vkCreateImageView") == 0) {
                return (void*)&Hook_vkCreateImageView;
            }
            if (strcmp(name, "vkDestroyImageView") == 0) {
                return (void*)&Hook_vkDestroyImageView;
            }

            // If you already hook TraceRays via exports, no need here.
            // But if the game fetches it dynamically, you can also return your TraceRays hook:
            if (strcmp(name, "vkCmdTraceRaysKHR") == 0) {
                return (void*)&Hook_vkCmdTraceRaysKHR;
            }

            return nullptr;
        }
        /*
        static PFN_vkVoidFunction VKAPI_CALL Hook_vkGetDeviceProcAddr(
            VkDevice device, const char* pName)
        {
            // Ask real first so we can store the real pointers
            PFN_vkVoidFunction real = nullptr;
            if (g_real_vkGetDeviceProcAddr) {
                real = g_real_vkGetDeviceProcAddr(device, pName);
            }

            // Cache real function pointers when we see them
            if (pName && real) {
                if (!g_real_vkUpdateDescriptorSets && strcmp(pName, "vkUpdateDescriptorSets") == 0)
                    g_real_vkUpdateDescriptorSets = (PFN_vkUpdateDescriptorSets)real;

                if (!g_real_vkUpdateDescriptorSetWithTemplate && strcmp(pName, "vkUpdateDescriptorSetWithTemplate") == 0)
                    g_real_vkUpdateDescriptorSetWithTemplate = (PFN_vkUpdateDescriptorSetWithTemplate)real;

                if (!g_real_vkCmdBindDescriptorSets && strcmp(pName, "vkCmdBindDescriptorSets") == 0)
                    g_real_vkCmdBindDescriptorSets = (PFN_vkCmdBindDescriptorSets)real;

                if (!g_real_vkCmdBindPipeline && strcmp(pName, "vkCmdBindPipeline") == 0)
                    g_real_vkCmdBindPipeline = (PFN_vkCmdBindPipeline)real;

                if (!g_real_vkCreateImage && strcmp(pName, "vkCreateImage") == 0)
                    g_real_vkCreateImage = (PFN_vkCreateImage)real;

                if (!g_real_vkDestroyImage && strcmp(pName, "vkDestroyImage") == 0)
                    g_real_vkDestroyImage = (PFN_vkDestroyImage)real;

                if (!g_real_vkCreateImageView && strcmp(pName, "vkCreateImageView") == 0)
                    g_real_vkCreateImageView = (PFN_vkCreateImageView)real;

                if (!g_real_vkDestroyImageView && strcmp(pName, "vkDestroyImageView") == 0)
                    g_real_vkDestroyImageView = (PFN_vkDestroyImageView)real;
            }

            // If this is one of the functions we want to intercept, return our hook pointer.
            if (void* hook = HookReturnIfMatch(pName)) {
                return (PFN_vkVoidFunction)hook;
            }

            // Otherwise, pass through
            return real;
        }

        */

        static bool HasExtension(const VkExtensionProperties* props, uint32_t count, const char* name)
        {
            for (uint32_t i = 0; i < count; i++) {
                if (std::strcmp(props[i].extensionName, name) == 0) return true;
            }
            return false;
        }

        static void WriteExtension(VkExtensionProperties& out, const char* name, uint32_t specVersion)
        {
            strncpy_s(out.extensionName, VK_MAX_EXTENSION_NAME_SIZE, name, _TRUNCATE);
            out.extensionName[VK_MAX_EXTENSION_NAME_SIZE - 1] = 0;
            out.specVersion = specVersion;
        }


        // Forward declare so Hook_vkGetInstanceProcAddr can return it safely.
        static VkResult VKAPI_CALL Hook_vkCreateDevice(
            VkPhysicalDevice physicalDevice,
            const VkDeviceCreateInfo* pCreateInfo,
            const VkAllocationCallbacks* pAllocator,
            VkDevice* pDevice);

        // ---- Hooks ----

        static VkResult VKAPI_CALL Hook_vkEnumerateDeviceExtensionProperties(
            VkPhysicalDevice physicalDevice,
            const char* pLayerName,
            uint32_t* pPropertyCount,
            VkExtensionProperties* pProperties)
        {
            auto& st = State();
            if (!st.real_vkEnumerateDeviceExtensionProperties) {
                thanosray::log::Logf("[EXT] ERROR: real vkEnumerateDeviceExtensionProperties is null");
                return VK_ERROR_INITIALIZATION_FAILED;
            }

            PollHotkeys();

            if (!pPropertyCount || pLayerName)
                return st.real_vkEnumerateDeviceExtensionProperties(physicalDevice, pLayerName, pPropertyCount, pProperties);
            uint32_t count = 0;
            VkResult r = st.real_vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &count, nullptr);
            if (r != VK_SUCCESS) return r;
            std::vector<VkExtensionProperties> extensions(count);
            r = st.real_vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &count, extensions.data());
            if (r != VK_SUCCESS) return r;
            extensions.resize(count);
            auto append = [&](const char* name) {
                if (!HasExtension(extensions.data(), (uint32_t)extensions.size(), name)) {
                    VkExtensionProperties prop{};
                    WriteExtension(prop, name, 1);
                    extensions.push_back(prop);
                }
            };
            for (const char* name : kSpoofExts) append(name);
            // Diagnostic opt-in: isolates DOOM's NVIDIA RT-loader gate. This is
            // not a mesh-shader implementation; mesh feature bits stay native.
            if (diagnostics::PascalGateExperiment()) {
                append(VK_NV_MESH_SHADER_EXTENSION_NAME);
                thanosray::log::LogOncef("pascal-gate", "[EXPERIMENT] NV_mesh_shader name exposed for RT initialization; no mesh emulation");
            }
            if (!pProperties) {
                *pPropertyCount = (uint32_t)extensions.size();
                return VK_SUCCESS;
            }
            const uint32_t copied = std::min(*pPropertyCount, (uint32_t)extensions.size());
            std::copy_n(extensions.data(), copied, pProperties);
            *pPropertyCount = copied;
            return copied < extensions.size() ? VK_INCOMPLETE : VK_SUCCESS;
        }

        static thread_local bool g_inProps2 = false;

        static void VKAPI_CALL Hook_vkGetPhysicalDeviceProperties2(
            VkPhysicalDevice physicalDevice,
            VkPhysicalDeviceProperties2* pProperties)
        {
            auto& st = State();
            if (!st.real_vkGetPhysicalDeviceProperties2 || !pProperties) {
                if (st.real_vkGetPhysicalDeviceProperties2) {
                    st.real_vkGetPhysicalDeviceProperties2(physicalDevice, pProperties);
                }
                return;
            }

            if (g_inProps2) {
                st.real_vkGetPhysicalDeviceProperties2(physicalDevice, pProperties);
                return;
            }
            g_inProps2 = true;

            st.real_vkGetPhysicalDeviceProperties2(physicalDevice, pProperties);

            VkBaseOutStructure* node = (VkBaseOutStructure*)pProperties->pNext;
            while (node) {
                if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES) {
                    auto* sg = (VkPhysicalDeviceSubgroupProperties*)node;
                    uint32_t before = sg->supportedStages;

                    sg->supportedStages |=
                        VK_SHADER_STAGE_RAYGEN_BIT_KHR |
                        VK_SHADER_STAGE_ANY_HIT_BIT_KHR |
                        VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR |
                        VK_SHADER_STAGE_MISS_BIT_KHR |
                        VK_SHADER_STAGE_INTERSECTION_BIT_KHR |
                        VK_SHADER_STAGE_CALLABLE_BIT_KHR;

                    if (sg->subgroupSize == 0) sg->subgroupSize = 32;

                    thanosray::log::Logf("[PROP] Subgroup supportedStages: 0x%08X -> 0x%08X", before, sg->supportedStages);
                }
                node = (VkBaseOutStructure*)node->pNext;
            }

            g_inProps2 = false;
        }

        static thread_local bool g_inFeat2 = false;

        // ============================================================================
// LANDMARK RF35: RT fallback - hook implementations (fail-open)
// ============================================================================
/*
        static void VKAPI_CALL Hook_vkUpdateDescriptorSets(
            VkDevice device, uint32_t descriptorWriteCount,
            const VkWriteDescriptorSet* pDescriptorWrites,
            uint32_t descriptorCopyCount,
            const VkCopyDescriptorSet* pDescriptorCopies)
        {
            if (pDescriptorWrites && descriptorWriteCount) {
                thanosray::vk::rtfb::OnUpdateDescriptorSets(descriptorWriteCount, pDescriptorWrites);
            }

            if (g_real_vkUpdateDescriptorSets) {
                g_real_vkUpdateDescriptorSets(device, descriptorWriteCount, pDescriptorWrites,
                    descriptorCopyCount, pDescriptorCopies);
                return;
            }
        }

        static void VKAPI_CALL Hook_vkUpdateDescriptorSetWithTemplate(
            VkDevice device, VkDescriptorSet descriptorSet,
            VkDescriptorUpdateTemplate descriptorUpdateTemplate,
            const void* pData)
        {
            // Template decoding is non-trivial; Phase 0: just forward.
            if (g_real_vkUpdateDescriptorSetWithTemplate) {
                g_real_vkUpdateDescriptorSetWithTemplate(device, descriptorSet, descriptorUpdateTemplate, pData);
                return;
            }
        }

        static void VKAPI_CALL Hook_vkCmdBindDescriptorSets(
            VkCommandBuffer commandBuffer,
            VkPipelineBindPoint pipelineBindPoint,
            VkPipelineLayout layout,
            uint32_t firstSet,
            uint32_t descriptorSetCount,
            const VkDescriptorSet* pDescriptorSets,
            uint32_t dynamicOffsetCount,
            const uint32_t* pDynamicOffsets)
        {
            if (pDescriptorSets && descriptorSetCount) {
                thanosray::vk::rtfb::OnCmdBindDescriptorSets(
                    commandBuffer, pipelineBindPoint, layout, firstSet, descriptorSetCount, pDescriptorSets);
            }

            if (g_real_vkCmdBindDescriptorSets) {
                g_real_vkCmdBindDescriptorSets(commandBuffer, pipelineBindPoint, layout, firstSet,
                    descriptorSetCount, pDescriptorSets,
                    dynamicOffsetCount, pDynamicOffsets);
                return;
            }
        }

        static void VKAPI_CALL Hook_vkCmdBindPipeline(
            VkCommandBuffer commandBuffer,
            VkPipelineBindPoint pipelineBindPoint,
            VkPipeline pipeline)
        {
            thanosray::vk::rtfb::OnCmdBindPipeline(commandBuffer, pipelineBindPoint, pipeline);

            if (g_real_vkCmdBindPipeline) {
                g_real_vkCmdBindPipeline(commandBuffer, pipelineBindPoint, pipeline);
                return;
            }
        }

        static VkResult VKAPI_CALL Hook_vkCreateImage(
            VkDevice device,
            const VkImageCreateInfo* pCreateInfo,
            const VkAllocationCallbacks* pAllocator,
            VkImage* pImage)
        {
            if (!g_real_vkCreateImage) return VK_ERROR_INITIALIZATION_FAILED;

            VkResult r = g_real_vkCreateImage(device, pCreateInfo, pAllocator, pImage);
            if (r == VK_SUCCESS && pCreateInfo && pImage && *pImage) {
                thanosray::vk::rtfb::OnCreateImage(*pImage, *pCreateInfo);
            }
            return r;
        }

        static void VKAPI_CALL Hook_vkDestroyImage(
            VkDevice device,
            VkImage image,
            const VkAllocationCallbacks* pAllocator)
        {
            if (image) thanosray::vk::rtfb::OnDestroyImage(image);
            if (g_real_vkDestroyImage) g_real_vkDestroyImage(device, image, pAllocator);
        }

        static VkResult VKAPI_CALL Hook_vkCreateImageView(
            VkDevice device,
            const VkImageViewCreateInfo* pCreateInfo,
            const VkAllocationCallbacks* pAllocator,
            VkImageView* pView)
        {
            if (!g_real_vkCreateImageView) return VK_ERROR_INITIALIZATION_FAILED;

            VkResult r = g_real_vkCreateImageView(device, pCreateInfo, pAllocator, pView);
            if (r == VK_SUCCESS && pCreateInfo && pView && *pView) {
                thanosray::vk::rtfb::OnCreateImageView(*pView, *pCreateInfo);
            }
            return r;
        }

        static void VKAPI_CALL Hook_vkDestroyImageView(
            VkDevice device,
            VkImageView view,
            const VkAllocationCallbacks* pAllocator)
        {
            if (view) thanosray::vk::rtfb::OnDestroyImageView(view);
            if (g_real_vkDestroyImageView) g_real_vkDestroyImageView(device, view, pAllocator);
        }
        */
        
        static void VKAPI_CALL Hook_vkGetPhysicalDeviceFeatures2(
            VkPhysicalDevice physicalDevice,
            VkPhysicalDeviceFeatures2* pFeatures)
        {
            auto& st = State();

            if (!st.real_vkGetPhysicalDeviceFeatures2 || !pFeatures) {
                if (st.real_vkGetPhysicalDeviceFeatures2) {
                    st.real_vkGetPhysicalDeviceFeatures2(physicalDevice, pFeatures);
                }
                return;
            }

            if (g_inFeat2) {
                st.real_vkGetPhysicalDeviceFeatures2(physicalDevice, pFeatures);
                return;
            }
            g_inFeat2 = true;

            // Real call first
            st.real_vkGetPhysicalDeviceFeatures2(physicalDevice, pFeatures);

            VkBaseOutStructure* node = (VkBaseOutStructure*)pFeatures->pNext;
            while (node) {
                switch (node->sType) {

                    // RT feature structs (spoof)
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR: {
                    auto* f = (VkPhysicalDeviceAccelerationStructureFeaturesKHR*)node;
                    f->accelerationStructure = VK_TRUE;
                    break;
                }
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR: {
                    auto* f = (VkPhysicalDeviceRayTracingPipelineFeaturesKHR*)node;
                    f->rayTracingPipeline = VK_TRUE;
                    f->rayTracingPipelineTraceRaysIndirect = VK_TRUE;
                    f->rayTraversalPrimitiveCulling = VK_TRUE;
                    break;
                }
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR: {
                    auto* f = (VkPhysicalDeviceRayQueryFeaturesKHR*)node;
                    f->rayQuery = VK_TRUE;
                    break;
                }

                                                                             // Common “baseline” spoof
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES: {
                    auto* f = (VkPhysicalDeviceVulkan12Features*)node;
                    f->shaderFloat16 = VK_TRUE;
                    f->shaderInt8 = VK_TRUE;
                    f->descriptorIndexing = VK_TRUE;
                    f->runtimeDescriptorArray = VK_TRUE;
                    f->descriptorBindingPartiallyBound = VK_TRUE;
                    f->descriptorBindingVariableDescriptorCount = VK_TRUE;
                    f->bufferDeviceAddress = VK_TRUE;
                    f->separateDepthStencilLayouts = VK_TRUE;
                    break;
                }

                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT: {
                    auto* f = (VkPhysicalDeviceRobustness2FeaturesEXT*)node;
                    f->nullDescriptor = VK_TRUE;
                    break;
                }

                default:
                    break;
                }

                node = (VkBaseOutStructure*)node->pNext;
            }

            static bool logged = false;
            if (!logged) {
                thanosray::log::Logf("[FEAT] vkGetPhysicalDeviceFeatures2 spoof active");
                logged = true;
            }

            g_inFeat2 = false;
        }

        // ============================================================================

        // ========================================================================
        // LANDMARK RT-LOG00: Common snapshot for RT-related logs
        // ========================================================================
        static void DumpRtGlobalsBrief(const char* tag)
        {
            const thanosray::Settings s = thanosray::Settings_Get();
            thanosray::log::LogOncef(tag,
                "[RTG] %s | Verbose=%d ExposureBoost=%.3f ShadowLift=%.3f FallbackGI=%d GI_Quality=%d GI_Steps=%d GI_RaysPerPix=%d GI_HalfRes=%d | SL_strength=%.3f toe=%.3f ceil=%.3f",
                tag,
                s.VerboseLog ? 1 : 0,
                s.ExposureBoost,
                s.ShadowLift,
                s.FallbackGI ? 1 : 0,
                (int)s.GI_Quality,
                (int)s.GI_Steps,
                (int)s.GI_RaysPerPix,
                s.GI_HalfRes ? 1 : 0,
                (float)g_sl_strength.load(),
                (float)g_sl_toe.load(),
                (float)g_sl_ceil.load());
        }

        // Preserve native ray dispatch. The driver supports RT pipelines on
        // Pascal; replacing dispatch with debug clears destroys their output.
        // ============================================================================

        static void VKAPI_CALL Hook_vkCmdTraceRaysKHR(
            VkCommandBuffer commandBuffer,
            const VkStridedDeviceAddressRegionKHR* raygen,
            const VkStridedDeviceAddressRegionKHR* miss,
            const VkStridedDeviceAddressRegionKHR* hit,
            const VkStridedDeviceAddressRegionKHR* callable,
            uint32_t width, uint32_t height, uint32_t depth)
        {
            if (g_real_vkCmdTraceRaysKHR) {
                thanosray::log::LogOncef("native.trace.direct", "[RT] Native vkCmdTraceRaysKHR %ux%ux%u", width, height, depth);
                g_real_vkCmdTraceRaysKHR(commandBuffer, raygen, miss, hit, callable, width, height, depth);
                return;
            }
            thanosray::log::LogOncef("native.trace.missing", "[RT] ERROR: native vkCmdTraceRaysKHR unavailable");
        }


        // ============================================================================
// LANDMARK RT-INDIRECT: Hook additional TraceRays entrypoints
// ============================================================================

        static void VKAPI_CALL Hook_vkCmdTraceRaysIndirectKHR(
            VkCommandBuffer commandBuffer,
            const VkStridedDeviceAddressRegionKHR* raygen,
            const VkStridedDeviceAddressRegionKHR* miss,
            const VkStridedDeviceAddressRegionKHR* hit,
            const VkStridedDeviceAddressRegionKHR* callable,
            VkDeviceAddress address)
        {
            if (g_real_vkCmdTraceRaysIndirectKHR) {
                thanosray::log::LogOncef("native.trace.indirect", "[RT] Native vkCmdTraceRaysIndirectKHR");
                g_real_vkCmdTraceRaysIndirectKHR(commandBuffer, raygen, miss, hit, callable, address);
            }
        }

        // Newer KHR entrypoint some engines use (if headers support it)
        static void VKAPI_CALL Hook_vkCmdTraceRaysIndirect2KHR(
            VkCommandBuffer commandBuffer,
            VkDeviceAddress address)
        {
            if (g_real_vkCmdTraceRaysIndirect2KHR) {
                thanosray::log::LogOncef("native.trace.indirect2", "[RT] Native vkCmdTraceRaysIndirect2KHR");
                g_real_vkCmdTraceRaysIndirect2KHR(commandBuffer, address);
            }
        }



        // ========================================================================
        // LANDMARK RT-LOG10: Additional RT/AS entrypoints — log once + pass-through
        // ========================================================================

        static VkDeviceAddress VKAPI_CALL Hook_vkGetBufferDeviceAddress(
            VkDevice device, const VkBufferDeviceAddressInfo* pInfo)
        {
            thanosray::log::LogOncef("[RT] vkGetBufferDeviceAddress",
                "[RT] vkGetBufferDeviceAddress(dev=%p buf=%p)", (void*)device, (void*)(pInfo ? pInfo->buffer : VK_NULL_HANDLE));
            if (g_real_vkGetBufferDeviceAddress) return g_real_vkGetBufferDeviceAddress(device, pInfo);
            if (g_real_vkGetBufferDeviceAddressKHR) return g_real_vkGetBufferDeviceAddressKHR(device, pInfo);
            return 0;
        }

#ifdef VK_KHR_ray_tracing_pipeline
        static VkResult VKAPI_CALL Hook_vkCreateRayTracingPipelinesKHR(
            VkDevice device,
            VkDeferredOperationKHR deferredOperation,
            VkPipelineCache pipelineCache,
            uint32_t createInfoCount,
            const VkRayTracingPipelineCreateInfoKHR* pCreateInfos,
            const VkAllocationCallbacks* pAllocator,
            VkPipeline* pPipelines)
        {
            DumpRtGlobalsBrief("[RT] vkCreateRayTracingPipelinesKHR");
            thanosray::log::LogOncef("[RT] vkCreateRayTracingPipelinesKHR",
                "[RT] vkCreateRayTracingPipelinesKHR(dev=%p count=%u cache=%p deferred=%p)",
                (void*)device, createInfoCount, (void*)pipelineCache, (void*)deferredOperation);

            if (!g_real_vkCreateRayTracingPipelinesKHR) {
                thanosray::log::Logf("[RT] WARN: no real vkCreateRayTracingPipelinesKHR resolved (returning VK_ERROR_FEATURE_NOT_PRESENT)");
                if (pPipelines && createInfoCount) {
                    for (uint32_t i = 0; i < createInfoCount; ++i) pPipelines[i] = VK_NULL_HANDLE;
                }
                return VK_ERROR_FEATURE_NOT_PRESENT;
            }
            VkResult result = g_real_vkCreateRayTracingPipelinesKHR(device, deferredOperation, pipelineCache, createInfoCount, pCreateInfos, pAllocator, pPipelines);
            thanosray::log::Logf("[PIPELINE] raytracing count=%u result=%d", createInfoCount, result);
            return result;
        }

        static VkResult VKAPI_CALL Hook_vkGetRayTracingShaderGroupHandlesKHR(
            VkDevice device, VkPipeline pipeline,
            uint32_t firstGroup, uint32_t groupCount,
            size_t dataSize, void* pData)
        {
            thanosray::log::LogOncef("[RT] vkGetRayTracingShaderGroupHandlesKHR",
                "[RT] vkGetRayTracingShaderGroupHandlesKHR(dev=%p pipe=%p first=%u count=%u size=%zu)",
                (void*)device, (void*)pipeline, firstGroup, groupCount, (size_t)dataSize);

            if (g_real_vkGetRayTracingShaderGroupHandlesKHR) {
                return g_real_vkGetRayTracingShaderGroupHandlesKHR(device, pipeline, firstGroup, groupCount, dataSize, pData);
            }
            if (g_real_vkGetRayTracingShaderGroupHandlesKHR2) {
                return g_real_vkGetRayTracingShaderGroupHandlesKHR2(device, pipeline, firstGroup, groupCount, dataSize, pData);
            }
            return VK_ERROR_FEATURE_NOT_PRESENT;
        }
#endif // VK_KHR_ray_tracing_pipeline

#ifdef VK_KHR_acceleration_structure
        static VkResult VKAPI_CALL Hook_vkCreateAccelerationStructureKHR(
            VkDevice device,
            const VkAccelerationStructureCreateInfoKHR* pCreateInfo,
            const VkAllocationCallbacks* pAllocator,
            VkAccelerationStructureKHR* pAS)
        {
            DumpRtGlobalsBrief("[AS] vkCreateAccelerationStructureKHR");
            VkAccelerationStructureTypeKHR t = pCreateInfo ? pCreateInfo->type : (VkAccelerationStructureTypeKHR)0;
            VkDeviceSize sz = pCreateInfo ? pCreateInfo->size : 0;
            VkBuffer buf = pCreateInfo ? pCreateInfo->buffer : VK_NULL_HANDLE;
            thanosray::log::LogOncef("[AS] vkCreateAccelerationStructureKHR",
                "[AS] vkCreateAccelerationStructureKHR(dev=%p type=%d size=%llu buf=%p offset=%llu flags=0x%x)",
                (void*)device, (int)t, (unsigned long long)sz, (void*)buf,
                (unsigned long long)(pCreateInfo ? pCreateInfo->offset : 0),
                (unsigned)(pCreateInfo ? pCreateInfo->createFlags : 0));

            if (!g_real_vkCreateAccelerationStructureKHR) return VK_ERROR_FEATURE_NOT_PRESENT;
            return g_real_vkCreateAccelerationStructureKHR(device, pCreateInfo, pAllocator, pAS);
        }

        static void VKAPI_CALL Hook_vkDestroyAccelerationStructureKHR(
            VkDevice device, VkAccelerationStructureKHR as, const VkAllocationCallbacks* pAllocator)
        {
            thanosray::log::LogOncef("[AS] vkDestroyAccelerationStructureKHR",
                "[AS] vkDestroyAccelerationStructureKHR(dev=%p as=%p)", (void*)device, (void*)as);
            if (g_real_vkDestroyAccelerationStructureKHR) {
                g_real_vkDestroyAccelerationStructureKHR(device, as, pAllocator);
            }
        }

        static void VKAPI_CALL Hook_vkGetAccelerationStructureBuildSizesKHR(
            VkDevice device, VkAccelerationStructureBuildTypeKHR buildType,
            const VkAccelerationStructureBuildGeometryInfoKHR* pBuildInfo,
            const uint32_t* pMaxPrimitiveCounts,
            VkAccelerationStructureBuildSizesInfoKHR* pSizeInfo)
        {
            thanosray::log::LogOncef("[AS] vkGetAccelerationStructureBuildSizesKHR",
                "[AS] vkGetAccelerationStructureBuildSizesKHR(dev=%p buildType=%d geomCount=%u flags=0x%x)",
                (void*)device, (int)buildType,
                pBuildInfo ? pBuildInfo->geometryCount : 0,
                (unsigned)(pBuildInfo ? pBuildInfo->flags : 0));

            if (g_real_vkGetAccelerationStructureBuildSizesKHR) {
                g_real_vkGetAccelerationStructureBuildSizesKHR(device, buildType, pBuildInfo, pMaxPrimitiveCounts, pSizeInfo);
            }
        }

        static void VKAPI_CALL Hook_vkCmdBuildAccelerationStructuresKHR(
            VkCommandBuffer cb, uint32_t infoCount,
            const VkAccelerationStructureBuildGeometryInfoKHR* pInfos,
            const VkAccelerationStructureBuildRangeInfoKHR* const* ppBuildRangeInfos)
        {
            thanosray::log::LogOncef("[AS] vkCmdBuildAccelerationStructuresKHR",
                "[AS] vkCmdBuildAccelerationStructuresKHR(cb=%p infoCount=%u)", (void*)cb, infoCount);

            if (g_real_vkCmdBuildAccelerationStructuresKHR) {
                g_real_vkCmdBuildAccelerationStructuresKHR(cb, infoCount, pInfos, ppBuildRangeInfos);
            }
        }

        static VkDeviceAddress VKAPI_CALL Hook_vkGetAccelerationStructureDeviceAddressKHR(
            VkDevice device, const VkAccelerationStructureDeviceAddressInfoKHR* pInfo)
        {
            VkAccelerationStructureKHR as = pInfo ? pInfo->accelerationStructure : VK_NULL_HANDLE;
            thanosray::log::LogOncef("[AS] vkGetAccelerationStructureDeviceAddressKHR",
                "[AS] vkGetAccelerationStructureDeviceAddressKHR(dev=%p as=%p)", (void*)device, (void*)as);

            if (g_real_vkGetAccelerationStructureDeviceAddressKHR) {
                return g_real_vkGetAccelerationStructureDeviceAddressKHR(device, pInfo);
            }
            return 0;
        }

        static void VKAPI_CALL Hook_vkCmdCopyAccelerationStructureKHR(
            VkCommandBuffer cb, const VkCopyAccelerationStructureInfoKHR* pInfo)
        {
            thanosray::log::LogOncef("[AS] vkCmdCopyAccelerationStructureKHR",
                "[AS] vkCmdCopyAccelerationStructureKHR(cb=%p src=%p dst=%p mode=%d)",
                (void*)cb,
                (void*)(pInfo ? pInfo->src : VK_NULL_HANDLE),
                (void*)(pInfo ? pInfo->dst : VK_NULL_HANDLE),
                (int)(pInfo ? pInfo->mode : 0));

            if (g_real_vkCmdCopyAccelerationStructureKHR) {
                g_real_vkCmdCopyAccelerationStructureKHR(cb, pInfo);
            }
        }

        static VkResult VKAPI_CALL Hook_vkQueuePresentKHR(
            VkQueue queue,
            const VkPresentInfoKHR* pPresentInfo
        );

#endif // VK_KHR_acceleration_structure

        static void VKAPI_CALL Hook_vkCmdPipelineBarrier(
            VkCommandBuffer commandBuffer,
            VkPipelineStageFlags srcStageMask,
            VkPipelineStageFlags dstStageMask,
            VkDependencyFlags dependencyFlags,
            uint32_t memoryBarrierCount,
            const VkMemoryBarrier* pMemoryBarriers,
            uint32_t bufferMemoryBarrierCount,
            const VkBufferMemoryBarrier* pBufferMemoryBarriers,
            uint32_t imageMemoryBarrierCount,
            const VkImageMemoryBarrier* pImageMemoryBarriers)
        {
            if (g_real_vkCmdPipelineBarrier) {
                g_real_vkCmdPipelineBarrier(commandBuffer, srcStageMask, dstStageMask, dependencyFlags,
                    memoryBarrierCount, pMemoryBarriers,
                    bufferMemoryBarrierCount, pBufferMemoryBarriers,
                    imageMemoryBarrierCount, pImageMemoryBarriers);
                return;
            }
        }

        static void VKAPI_CALL Hook_vkCmdClearColorImage(
            VkCommandBuffer commandBuffer,
            VkImage image,
            VkImageLayout imageLayout,
            const VkClearColorValue* pColor,
            uint32_t rangeCount,
            const VkImageSubresourceRange* pRanges)
        {
            if (g_real_vkCmdClearColorImage) {
                g_real_vkCmdClearColorImage(commandBuffer, image, imageLayout, pColor, rangeCount, pRanges);
                return;
            }
        }
        static HMODULE LoadRealVulkanModule()
        {
            // 1) Prefer local renamed DLL if present (your current approach)
            HMODULE m = GetModuleHandleA("vulkan-1_real.dll");
            if (m) return m;

            m = LoadLibraryExA("vulkan-1_real.dll", nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
            if (m) return m;

            // 2) Fallback: force-load Vulkan loader from System32
            // (avoids picking up your proxy again)
            char sysdir[MAX_PATH] = {};
            UINT n = GetSystemDirectoryA(sysdir, MAX_PATH);
            if (n == 0 || n > MAX_PATH - 1) return nullptr;

            char path[MAX_PATH] = {};
            sprintf_s(path, "%s\\vulkan-1.dll", sysdir);

            // Important: use LoadLibraryExA with an absolute path so it can’t recurse into your proxy.
            m = LoadLibraryExA(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
            return m;
        }

        // --- Forward decls so GDPA can return these hooks (defined later) ---
        static void VKAPI_CALL Hook_vkGetDeviceQueue(
            VkDevice device, uint32_t queueFamilyIndex, uint32_t queueIndex, VkQueue* pQueue);

        static void VKAPI_CALL Hook_vkGetDeviceQueue2(
            VkDevice device, const VkDeviceQueueInfo2* pQueueInfo, VkQueue* pQueue);

        static bool IsRtName(const char* n)
        {
            if (!n) return false;

            // Catch the usual RT core
            if (strstr(n, "TraceRays")) return true;
            if (strstr(n, "RayTracing")) return true;
            if (strstr(n, "AccelerationStructure")) return true;
            if (strstr(n, "ShaderBindingTable")) return true;
            if (strstr(n, "SBT")) return true;

            // Catch supporting stuff that often matters for RT paths
            if (strstr(n, "BufferDeviceAddress")) return true;
            if (strstr(n, "DeferredOperation")) return true;
            if (strstr(n, "PipelineLibrary")) return true;

            return false;
        }


        static PFN_vkVoidFunction VKAPI_CALL Hook_vkGetDeviceProcAddr(VkDevice device, const char* pName)
        {
            auto& st = State();
            PFN_vkVoidFunction fn = nullptr;
            // Engines may replace their resolver with the pointer obtained from
            // the resolver itself. Keep that path inside the proxy as well.
            if (device && pName && std::strcmp(pName, "vkGetDeviceProcAddr") == 0)
                return (PFN_vkVoidFunction)&Hook_vkGetDeviceProcAddr;
/*
            auto IsRtName = [](const char* n) -> bool {
                if (!n) return false;
                return strstr(n, "TraceRays") ||
                    strstr(n, "RayTracing") ||
                    strstr(n, "AccelerationStructure") ||
                    strstr(n, "ShaderGroup") ||
                    strstr(n, "ShaderBindingTable") ||
                    strstr(n, "BufferDeviceAddress") ||
                    strstr(n, "DeferredOperation");
                };
*/
// --- RT PROBE: Always log RT-ish proc requests ONCE per name (even if not hooked)
            if (IsRtName(pName))
            {
                char key[256];
                _snprintf_s(key, _TRUNCATE, "rtprobe.gdpa:%s", pName ? pName : "(null)");

                // If you have DumpRtGlobalsBrief(tag) already in this file, call it here too:
                // DumpRtGlobalsBrief(key);

                thanosray::log::LogOncef(
                    key,
                    "[RTPROBE][GDPA] dev=%p request=%s",
                    (void*)device,
                    pName ? pName : "(null)"
                );
            }

            // 1) Ask the real loader first
            if (st.real_vkGetDeviceProcAddr)
                fn = st.real_vkGetDeviceProcAddr(device, pName);

            // 2) Optional fallback GetProcAddress (your existing code)
            // ...
/*
            // 3) Decide what we RETURN (hook vs real)
            PFN_vkVoidFunction hook = FindHookForName(pName);   // <- whatever your project uses
            PFN_vkVoidFunction ret = hook ? hook : fn;

            // 4) RT-probe log ONCE per name (this is what you want)
            if (IsRtName(pName))
            {
                thanosray::log::LogOncef(std::string("rtprobe:") + pName,
                    "[RTPROBE] %s -> ret=%p (hook=%p real=%p)",
                    pName, (void*)ret, (void*)hook, (void*)fn);
            }

            */
        
        
            // 2) If it returns NULL, then try GetProcAddress on the real module
            if (!fn && device != VK_NULL_HANDLE && pName) {
                HMODULE rm = LoadRealVulkanModule();
                if (rm) {
                    fn = (PFN_vkVoidFunction)GetProcAddress(rm, pName);
                    if (fn) {
                        thanosray::log::Logf("[GDPA] fallback GetProcAddress(%s) -> %p", pName, (void*)fn);
                    }
                }


                // Continue to the wrapper's dispatch table. An unsupported driver
                // command can still have an explicitly implemented wrapper hook.
            }

            // 3) Now log what we actually resolved
            static uint32_t s_gdpaCount = 0;
            uint32_t n = ++s_gdpaCount;
            if (n <= 200) {
                thanosray::log::Logf("[GDPA] request[%u]: dev=%p name=%s real=%p",
                    n, (void*)device, pName ? pName : "(null)", (void*)fn);
            }

            // LANDMARK GDPA-TRACE: prove GDPA is being used by the game
            //static uint32_t s_gdpaCount = 0;
            //uint32_t n = ++s_gdpaCount;

            // Log the first ~200 GDPA requests so we can see what's happening.
            if (n <= 200) {
                thanosray::log::Logf("[GDPA] request[%u]: %s -> real=%p", n, pName ? pName : "(null)", (void*)fn);
            }

            if (st.real_vkGetDeviceProcAddr) {
                fn = st.real_vkGetDeviceProcAddr(device, pName);
            }
            // LANDMARK GDPA-FALLBACK: Some loader paths return NULL from vkGetDeviceProcAddr.
// If that happens, fall back to the exported trampoline in vulkan-1_real.dll.
            if (!fn && device != VK_NULL_HANDLE && pName && st.realModule) {
                fn = (PFN_vkVoidFunction)GetProcAddress(st.realModule, pName);
                if (fn) {
                    thanosray::log::Logf("[GDPA] fallback GetProcAddress(%s) -> %p", pName, (void*)fn);
                }
                else {
                    thanosray::log::Logf("[GDPA] fallback GetProcAddress(%s) FAILED", pName);
                }
                HMODULE realMod = GetModuleHandleA("vulkan-1_real.dll");
                if (!realMod) realMod = LoadRealVulkanModule();
                if (realMod) fn = (PFN_vkVoidFunction)GetProcAddress(realMod, pName);

            }


            if (!pName) return fn;

            // ----------------------------------------------------------------
            // LANDMARK 20: Device proc interception (GDPA)
            // - Keep existing behavior.
            // - Add OPTIONAL swapchain/present interception via GDPA.
            // ----------------------------------------------------------------

            if (std::strcmp(pName, "vkCmdTraceRaysKHR") == 0) {
                g_real_vkCmdTraceRaysKHR = (PFN_vkCmdTraceRaysKHR)fn;
                return fn ? (PFN_vkVoidFunction)&Hook_vkCmdTraceRaysKHR : nullptr;
            }
            if (std::strcmp(pName, "vkCmdTraceRaysIndirectKHR") == 0) {
                g_real_vkCmdTraceRaysIndirectKHR = (PFN_vkCmdTraceRaysIndirectKHR)fn;
                return fn ? (PFN_vkVoidFunction)&Hook_vkCmdTraceRaysIndirectKHR : nullptr;
            }
            if (std::strcmp(pName, "vkCmdTraceRaysIndirect2KHR") == 0) {
                g_real_vkCmdTraceRaysIndirect2KHR = (PFN_vkCmdTraceRaysIndirect2KHR)fn;
                return fn ? (PFN_vkVoidFunction)&Hook_vkCmdTraceRaysIndirect2KHR : nullptr;
            }
            // ----------------------------------------------------------------
// LANDMARK RTFB-GDPA: Provide RTFB tracking hooks via vkGetDeviceProcAddr
// Many engines call device commands ONLY through GDPA-returned pointers,
// bypassing the exported entrypoints you patched in g_procTable.
// ----------------------------------------------------------------
            //
            /*
            auto CaptureAndReturn = [&](const char* name, PFN_vkVoidFunction hook, void** outReal) -> PFN_vkVoidFunction
                {
              
                    // Never hand back a hook if we couldn't resolve a real function.
                    if (device == VK_NULL_HANDLE || !fn) {
                        return fn;
                    }

                    if (!*outReal) {
                        *outReal = (void*)fn;
                        thanosray::log::Logf("[GDPA] captured real: %s=%p", name, (void*)fn);
                    }

                    std::string key = std::string("gdpa.hook:") + name;
                    thanosray::log::LogOncef(key.c_str(), "[GDPA] returning hook: %s", name);
                    return hook;
                };
               // 
            */
                // LANDMARK GDPA-SAFEHOOK
            
            auto CaptureAndReturn = [&](const char* name, PFN_vkVoidFunction hook, void** outReal) -> PFN_vkVoidFunction
                {
                    if (!fn || device == VK_NULL_HANDLE) {
                        return fn; // never hand out a hook without a real target
                    }
                    if (!*outReal) {
                        *outReal = (void*)fn;
                        thanosray::log::Logf("[GDPA] captured real: %s=%p", name, (void*)fn);
                    }
                    std::string key = std::string("gdpa.hook:") + name;
                    thanosray::log::LogOncef(key.c_str(), "[GDPA] returning hook: %s", name);
                    return hook;
                };
                


// Descriptor tracking
if (std::strcmp(pName, "vkUpdateDescriptorSets") == 0) {
    return CaptureAndReturn(pName, (PFN_vkVoidFunction)&Hook_vkUpdateDescriptorSets, (void**)&g_real_vkUpdateDescriptorSets);
}
if (std::strcmp(pName, "vkUpdateDescriptorSetWithTemplate") == 0) {
    return CaptureAndReturn(pName, (PFN_vkVoidFunction)&Hook_vkUpdateDescriptorSetWithTemplate, (void**)&g_real_vkUpdateDescriptorSetWithTemplate);
}
/*
// Queue capture (needed so ShadowLift can find a present queue)
if (std::strcmp(pName, "vkGetDeviceQueue") == 0) {
    return CaptureAndReturn(pName, (PFN_vkVoidFunction)&Hook_vkGetDeviceQueue,
        (void**)&g_real_vkGetDeviceQueue);
}
if (std::strcmp(pName, "vkGetDeviceQueue2") == 0) {
    return CaptureAndReturn(pName, (PFN_vkVoidFunction)&Hook_vkGetDeviceQueue2,
        (void**)&g_real_vkGetDeviceQueue2);
}*/

if (std::strcmp(pName, "vkGetDeviceQueue") == 0) {
    if (!g_gdpa_vkGetDeviceQueue) {
        g_gdpa_vkGetDeviceQueue = (PFN_vkGetDeviceQueue)fn;
        thanosray::log::Logf("[GDPA] captured real: vkGetDeviceQueue=%p", (void*)g_gdpa_vkGetDeviceQueue);
    }
    return (PFN_vkVoidFunction)&Hook_vkGetDeviceQueue;
}

if (std::strcmp(pName, "vkGetDeviceQueue2") == 0) {
    if (!g_gdpa_vkGetDeviceQueue2) {
        g_gdpa_vkGetDeviceQueue2 = (PFN_vkGetDeviceQueue2)fn;
        thanosray::log::Logf("[GDPA] captured real: vkGetDeviceQueue2=%p", (void*)g_gdpa_vkGetDeviceQueue2);
    }
    return (PFN_vkVoidFunction)&Hook_vkGetDeviceQueue2;
}


if (std::strcmp(pName, "vkCmdBindDescriptorSets") == 0) {
    return CaptureAndReturn(pName, (PFN_vkVoidFunction)&Hook_vkCmdBindDescriptorSets, (void**)&g_real_vkCmdBindDescriptorSets);
}
if (std::strcmp(pName, "vkCmdBindPipeline") == 0) {
    return CaptureAndReturn(pName, (PFN_vkVoidFunction)&Hook_vkCmdBindPipeline, (void**)&g_real_vkCmdBindPipeline);
}


// Buffer device address (SBT / device pointers)
if (std::strcmp(pName, "vkGetBufferDeviceAddress") == 0) {
    return CaptureAndReturn(pName, (PFN_vkVoidFunction)&Hook_vkGetBufferDeviceAddress, (void**)&g_real_vkGetBufferDeviceAddress);
}
if (std::strcmp(pName, "vkGetBufferDeviceAddressKHR") == 0) {
    return CaptureAndReturn(pName, (PFN_vkVoidFunction)&Hook_vkGetBufferDeviceAddress, (void**)&g_real_vkGetBufferDeviceAddressKHR);
}

#ifdef VK_KHR_ray_tracing_pipeline
if (std::strcmp(pName, "vkCreateRayTracingPipelinesKHR") == 0) {
    return CaptureAndReturn(pName, (PFN_vkVoidFunction)&Hook_vkCreateRayTracingPipelinesKHR, (void**)&g_real_vkCreateRayTracingPipelinesKHR);
}
if (std::strcmp(pName, "vkGetRayTracingShaderGroupHandlesKHR") == 0) {
    return CaptureAndReturn(pName, (PFN_vkVoidFunction)&Hook_vkGetRayTracingShaderGroupHandlesKHR, (void**)&g_real_vkGetRayTracingShaderGroupHandlesKHR);
}
#endif

#ifdef VK_KHR_acceleration_structure
if (std::strcmp(pName, "vkCreateAccelerationStructureKHR") == 0) {
    return CaptureAndReturn(pName, (PFN_vkVoidFunction)&Hook_vkCreateAccelerationStructureKHR, (void**)&g_real_vkCreateAccelerationStructureKHR);
}
if (std::strcmp(pName, "vkDestroyAccelerationStructureKHR") == 0) {
    return CaptureAndReturn(pName, (PFN_vkVoidFunction)&Hook_vkDestroyAccelerationStructureKHR, (void**)&g_real_vkDestroyAccelerationStructureKHR);
}
if (std::strcmp(pName, "vkGetAccelerationStructureBuildSizesKHR") == 0) {
    return CaptureAndReturn(pName, (PFN_vkVoidFunction)&Hook_vkGetAccelerationStructureBuildSizesKHR, (void**)&g_real_vkGetAccelerationStructureBuildSizesKHR);
}
if (std::strcmp(pName, "vkCmdBuildAccelerationStructuresKHR") == 0) {
    return CaptureAndReturn(pName, (PFN_vkVoidFunction)&Hook_vkCmdBuildAccelerationStructuresKHR, (void**)&g_real_vkCmdBuildAccelerationStructuresKHR);
}
if (std::strcmp(pName, "vkGetAccelerationStructureDeviceAddressKHR") == 0) {
    return CaptureAndReturn(pName, (PFN_vkVoidFunction)&Hook_vkGetAccelerationStructureDeviceAddressKHR, (void**)&g_real_vkGetAccelerationStructureDeviceAddressKHR);
}
if (std::strcmp(pName, "vkCmdCopyAccelerationStructureKHR") == 0) {
    return CaptureAndReturn(pName, (PFN_vkVoidFunction)&Hook_vkCmdCopyAccelerationStructureKHR, (void**)&g_real_vkCmdCopyAccelerationStructureKHR);
}
#endif


// Image tracking
if (std::strcmp(pName, "vkCreateImage") == 0) {
    return CaptureAndReturn(pName, (PFN_vkVoidFunction)&Hook_vkCreateImage, (void**)&g_real_vkCreateImage);
}
if (std::strcmp(pName, "vkDestroyImage") == 0) {
    return CaptureAndReturn(pName, (PFN_vkVoidFunction)&Hook_vkDestroyImage, (void**)&g_real_vkDestroyImage);
}
if (std::strcmp(pName, "vkCreateImageView") == 0) {
    return CaptureAndReturn(pName, (PFN_vkVoidFunction)&Hook_vkCreateImageView, (void**)&g_real_vkCreateImageView);
}
if (std::strcmp(pName, "vkDestroyImageView") == 0) {
    return CaptureAndReturn(pName, (PFN_vkVoidFunction)&Hook_vkDestroyImageView, (void**)&g_real_vkDestroyImageView);
}

// Optional: allow RtFallback to do safe clear/barrier (Phase 0b)
if (std::strcmp(pName, "vkCmdPipelineBarrier") == 0) {
    auto ret = CaptureAndReturn(pName, (PFN_vkVoidFunction)&Hook_vkCmdPipelineBarrier, (void**)&g_real_vkCmdPipelineBarrier);
    return ret;
}
if (std::strcmp(pName, "vkCmdClearColorImage") == 0) {
    auto ret = CaptureAndReturn(pName, (PFN_vkVoidFunction)&Hook_vkCmdClearColorImage, (void**)&g_real_vkCmdClearColorImage);
    return ret;
}


            if (std::strcmp(pName, "vkCreateSwapchainKHR") == 0) {
                if (!g_gdpa_vkCreateSwapchainKHR) {
                    g_gdpa_vkCreateSwapchainKHR = (PFN_vkCreateSwapchainKHR)fn;
                    thanosray::log::Logf("[GDPA] captured real: vkCreateSwapchainKHR=%p", (void*)g_gdpa_vkCreateSwapchainKHR);
                }
                return (PFN_vkVoidFunction)&HookGDPA_vkCreateSwapchainKHR;
            }

            if (std::strcmp(pName, "vkGetSwapchainImagesKHR") == 0) {
                if (!g_gdpa_vkGetSwapchainImagesKHR) {
                    g_gdpa_vkGetSwapchainImagesKHR = (PFN_vkGetSwapchainImagesKHR)fn;
                    thanosray::log::Logf("[GDPA] captured real: vkGetSwapchainImagesKHR=%p", (void*)g_gdpa_vkGetSwapchainImagesKHR);
                }
                return (PFN_vkVoidFunction)&HookGDPA_vkGetSwapchainImagesKHR;
            }

            if (std::strcmp(pName, "vkQueuePresentKHR") == 0) {
                if (!g_gdpa_vkQueuePresentKHR) {
                    g_gdpa_vkQueuePresentKHR = (PFN_vkQueuePresentKHR)fn;
                    thanosray::log::Logf("[GDPA] captured real: vkQueuePresentKHR=%p", (void*)g_gdpa_vkQueuePresentKHR);
                }
                return (PFN_vkVoidFunction)&HookGDPA_vkQueuePresentKHR;
            }

            if (std::strcmp(pName, "vkAcquireNextImageKHR") == 0) {
                if (!g_gdpa_vkAcquireNextImageKHR) {
                    g_gdpa_vkAcquireNextImageKHR = (PFN_vkAcquireNextImageKHR)fn;
                    thanosray::log::Logf("[GDPA] captured real: vkAcquireNextImageKHR=%p", (void*)g_gdpa_vkAcquireNextImageKHR);
                }
                return (PFN_vkVoidFunction)&HookGDPA_vkAcquireNextImageKHR;
            }

            if (std::strcmp(pName, "vkAcquireNextImage2KHR") == 0) {
                if (!g_gdpa_vkAcquireNextImage2KHR) {
                    g_gdpa_vkAcquireNextImage2KHR = (PFN_vkAcquireNextImage2KHR)fn;
                    thanosray::log::Logf("[GDPA] captured real: vkAcquireNextImage2KHR=%p", (void*)g_gdpa_vkAcquireNextImage2KHR);
                }
                return (PFN_vkVoidFunction)&HookGDPA_vkAcquireNextImage2KHR;
            }
            // Present hook (THIS is where ShadowLift must run)
            if (std::strcmp(pName, "vkQueuePresentKHR") == 0) {
                return CaptureAndReturn(pName,
                    (PFN_vkVoidFunction)&Hook_vkQueuePresentKHR,
                    (void**)&g_real_vkQueuePresentKHR);
            }

            // Also capture queues so ShadowLift can submit safely
            if (std::strcmp(pName, "vkGetDeviceQueue") == 0) {
                return CaptureAndReturn(pName,
                    (PFN_vkVoidFunction)&Hook_vkGetDeviceQueue,
                    (void**)&g_real_vkGetDeviceQueue);
            }
            if (std::strcmp(pName, "vkGetDeviceQueue2") == 0) {
                return CaptureAndReturn(pName,
                    (PFN_vkVoidFunction)&Hook_vkGetDeviceQueue2,
                    (void**)&g_real_vkGetDeviceQueue2);
            }



            return runtime::Wrap(pName, diagnostics::Wrap(pName, fn));
        }

        static PFN_vkVoidFunction VKAPI_CALL Hook_vkGetInstanceProcAddr(VkInstance instance, const char* pName)
        {
            auto& st = State();
            PFN_vkVoidFunction fn = nullptr;
            if (st.real_vkGetInstanceProcAddr) {
                fn = st.real_vkGetInstanceProcAddr(instance, pName);
            }
            if (pName && (!fn || IsRtName(pName))) {
                thanosray::log::Logf("[GIPA-DIAG] instance=%p name=%s real=%p",
                    (void*)instance, pName, (void*)fn);
            }

            // ============================================================================
// ============================================================================
// LANDMARK GDPA-FALLBACK: If loader GDPA returns NULL, fall back to export from
// the real module.
// ============================================================================

            if (!fn && instance != VK_NULL_HANDLE && pName) {
                HMODULE realMod = GetModuleHandleA("vulkan-1_real.dll");
                if (!realMod) {
                    // Optional: load it if not loaded yet (see helper below)
                     realMod = LoadRealVulkanModule();
                }

                if (realMod) {
                    fn = (PFN_vkVoidFunction)GetProcAddress(realMod, pName);
                    if (fn) {
                        thanosray::log::Logf("[GDPA] fallback GetProcAddress(%s) -> %p", pName, (void*)fn);
                    }
                }
            }


            if (!pName) return fn;

            if (std::strcmp(pName, "vkGetInstanceProcAddr") == 0 && fn)
                return (PFN_vkVoidFunction)&Hook_vkGetInstanceProcAddr;
            if (std::strcmp(pName, "vkGetDeviceProcAddr") == 0 && instance && fn)
                return (PFN_vkVoidFunction)&Hook_vkGetDeviceProcAddr;
            if (std::strcmp(pName, "vkEnumerateDeviceExtensionProperties") == 0 && instance && fn)
                return (PFN_vkVoidFunction)&Hook_vkEnumerateDeviceExtensionProperties;

            if (std::strcmp(pName, "vkGetPhysicalDeviceProperties2") == 0 ||
                std::strcmp(pName, "vkGetPhysicalDeviceProperties2KHR") == 0) {
                thanosray::log::Logf("[GIPA] returning hook for %s", pName);
                return (PFN_vkVoidFunction)&Hook_vkGetPhysicalDeviceProperties2;
            }

            if (std::strcmp(pName, "vkGetPhysicalDeviceFeatures2") == 0 ||
                std::strcmp(pName, "vkGetPhysicalDeviceFeatures2KHR") == 0) {
                thanosray::log::Logf("[GIPA] returning hook for %s", pName);
                return (PFN_vkVoidFunction)&Hook_vkGetPhysicalDeviceFeatures2;
            }

            if (std::strcmp(pName, "vkCreateDevice") == 0) {
                thanosray::log::Logf("[GIPA] returning hook for %s", pName);
                return (PFN_vkVoidFunction)&Hook_vkCreateDevice;
            }
            if (std::strcmp(pName, "vkCmdTraceRaysKHR") == 0) {
                g_real_vkCmdTraceRaysKHR = (PFN_vkCmdTraceRaysKHR)fn;
                return fn ? (PFN_vkVoidFunction)&Hook_vkCmdTraceRaysKHR : nullptr;
            }
            if (std::strcmp(pName, "vkCmdTraceRaysIndirectKHR") == 0) {
                g_real_vkCmdTraceRaysIndirectKHR = (PFN_vkCmdTraceRaysIndirectKHR)fn;
                return fn ? (PFN_vkVoidFunction)&Hook_vkCmdTraceRaysIndirectKHR : nullptr;
            }
            if (std::strcmp(pName, "vkCmdTraceRaysIndirect2KHR") == 0) {
                g_real_vkCmdTraceRaysIndirect2KHR = (PFN_vkCmdTraceRaysIndirect2KHR)fn;
                return fn ? (PFN_vkVoidFunction)&Hook_vkCmdTraceRaysIndirect2KHR : nullptr;
            }


            return runtime::Wrap(pName, diagnostics::Wrap(pName, fn));
        }

        static bool IsRtExt(const char* n)
        {
            if (!n) return false;
            return (strcmp(n, "VK_KHR_deferred_host_operations") == 0) ||
                (strcmp(n, "VK_KHR_acceleration_structure") == 0) ||
                (strcmp(n, "VK_KHR_ray_tracing_pipeline") == 0) ||
                (strcmp(n, "VK_KHR_ray_query") == 0);
        }

        static std::unordered_set<std::string> QueryDriverExts(VkPhysicalDevice phys)
        {
            std::unordered_set<std::string> out;
            auto& st = State();
            if (!st.real_vkEnumerateDeviceExtensionProperties) return out;

            uint32_t count = 0;
            VkResult r = st.real_vkEnumerateDeviceExtensionProperties(phys, nullptr, &count, nullptr);
            if (r != VK_SUCCESS || count == 0) return out;

            std::vector<VkExtensionProperties> props(count);
            r = st.real_vkEnumerateDeviceExtensionProperties(phys, nullptr, &count, props.data());
            if (r != VK_SUCCESS) return out;

            out.reserve(count);
            for (uint32_t i = 0; i < count; ++i)
                out.insert(props[i].extensionName);

            return out;
        }

        // ---- CreateDevice pNext dump + feature clamping ----

        static const char* STypeName(VkStructureType s)
        {
            switch (s) {
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2: return "PHYSICAL_DEVICE_FEATURES_2";
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES: return "VULKAN_1_1_FEATURES";
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES: return "VULKAN_1_2_FEATURES";
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES: return "VULKAN_1_3_FEATURES";
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES: return "BUFFER_DEVICE_ADDRESS_FEATURES";
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES: return "SHADER_FLOAT16_INT8_FEATURES";
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES: return "8BIT_STORAGE_FEATURES";
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES: return "16BIT_STORAGE_FEATURES";
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SEPARATE_DEPTH_STENCIL_LAYOUTS_FEATURES: return "SEPARATE_DEPTH_STENCIL_LAYOUTS_FEATURES";
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT: return "ROBUSTNESS_2_FEATURES_EXT";
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR: return "ACCELERATION_STRUCTURE_FEATURES_KHR";
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR: return "RAY_TRACING_PIPELINE_FEATURES_KHR";
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR: return "RAY_QUERY_FEATURES_KHR";
            default: return "UNKNOWN";
            }
        }

        static void DumpInChain(const void* pNext, const char* tag)
        {
            thanosray::log::Logf("%s pNext chain:", tag);
            const VkBaseInStructure* n = reinterpret_cast<const VkBaseInStructure*>(pNext);
            int idx = 0;
            while (n) {
                const char* name = STypeName((VkStructureType)n->sType);
                if (std::strcmp(name, "UNKNOWN") == 0) {
                    thanosray::log::Logf("  [%d] sType=%d (UNKNOWN)", idx, (int)n->sType);
                }
                else {
                    thanosray::log::Logf("  [%d] %s", idx, name);
                }
                n = n->pNext;
                idx++;
                if (idx > 64) {
                    thanosray::log::Logf("  [!] chain too long, stopping dump");
                    break;
                }
            }
            if (idx == 0) thanosray::log::Logf("  (null)");
        }

        static bool IsRtFeatureStruct(VkStructureType s)
        {
            return s == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR ||
                s == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR ||
                s == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;
        }

        template <typename T>
        static T* AppendCopy(std::vector<std::vector<uint8_t>>& blob, const T* src)
        {
            // Separate allocations keep pNext pointers stable as the chain grows.
            blob.emplace_back(sizeof(T));
            T* dst = reinterpret_cast<T*>(blob.back().data());
            std::memcpy(dst, src, sizeof(T));
            return dst;
        }

        struct RealFeatPack {
            VkPhysicalDeviceFeatures2 core{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
            VkPhysicalDeviceVulkan11Features v11{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES };
            VkPhysicalDeviceVulkan12Features v12{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
            VkPhysicalDeviceBufferDeviceAddressFeatures bda{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES };
            VkPhysicalDeviceShaderFloat16Int8Features f16i8{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES };
            VkPhysicalDevice8BitStorageFeatures f8{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES };
            VkPhysicalDevice16BitStorageFeatures f16{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES };
            VkPhysicalDeviceSeparateDepthStencilLayoutsFeatures sep{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SEPARATE_DEPTH_STENCIL_LAYOUTS_FEATURES };
            VkPhysicalDeviceRobustness2FeaturesEXT rob2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT };

            void BuildChain()
            {
                core.pNext = &v11;
                v11.pNext = &v12;
                v12.pNext = &bda;
                bda.pNext = &f16i8;
                f16i8.pNext = &f8;
                f8.pNext = &f16;
                f16.pNext = &sep;
                sep.pNext = &rob2;
                rob2.pNext = nullptr;
            }
        };

        static RealFeatPack QueryRealFeatures(VkPhysicalDevice phys)
        {
            RealFeatPack out;
            out.BuildChain();

            auto& st = State();
            if (st.real_vkGetPhysicalDeviceFeatures2) {
                st.real_vkGetPhysicalDeviceFeatures2(phys, &out.core); // bypass spoof
            }
            return out;
        }

        static const void* BuildSanitizedDeviceCreatePNext(
            VkPhysicalDevice phys,
            const void* originalPNext,
            std::vector<std::vector<uint8_t>>& storage)
        {
            storage.clear();
            storage.reserve(16);

            const RealFeatPack real = QueryRealFeatures(phys);

            const VkBaseInStructure* src = reinterpret_cast<const VkBaseInStructure*>(originalPNext);

            VkBaseInStructure* head = nullptr;
            VkBaseInStructure* tail = nullptr;

            while (src) {
                VkStructureType sType = (VkStructureType)src->sType;

                // Always remove RT feature structs from device creation
                if (IsRtFeatureStruct(sType)) {
                    thanosray::log::Logf("[vkCreateDevice] DROP pNext RT feature struct: %s", STypeName(sType));
                    src = src->pNext;
                    continue;
                }

                VkBaseInStructure* copied = nullptr;

                switch (sType) {

                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2: {
                    auto* dst = AppendCopy(storage, (const VkPhysicalDeviceFeatures2*)src);
                    // Clamp a few core toggles (safe)
                    dst->features.robustBufferAccess &= real.core.features.robustBufferAccess;
                    dst->features.shaderInt16 &= real.core.features.shaderInt16;
                    dst->features.shaderInt64 &= real.core.features.shaderInt64;
                    dst->features.shaderStorageImageExtendedFormats &= real.core.features.shaderStorageImageExtendedFormats;
                    copied = (VkBaseInStructure*)dst;
                    break;
                }

                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES: {
                    auto* dst = AppendCopy(storage, (const VkPhysicalDeviceVulkan11Features*)src);
                    dst->shaderDrawParameters &= real.v11.shaderDrawParameters;
                    copied = (VkBaseInStructure*)dst;
                    break;
                }

                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES: {
                    auto* dst = AppendCopy(storage, (const VkPhysicalDeviceVulkan12Features*)src);

                    // Clamp the big crash-causers
                    dst->shaderFloat16 &= real.v12.shaderFloat16;
                    dst->shaderInt8 &= real.v12.shaderInt8;

                    dst->descriptorIndexing &= real.v12.descriptorIndexing;
                    dst->runtimeDescriptorArray &= real.v12.runtimeDescriptorArray;
                    dst->descriptorBindingPartiallyBound &= real.v12.descriptorBindingPartiallyBound;
                    dst->descriptorBindingVariableDescriptorCount &= real.v12.descriptorBindingVariableDescriptorCount;
                    dst->descriptorBindingUpdateUnusedWhilePending &= real.v12.descriptorBindingUpdateUnusedWhilePending;

                    dst->bufferDeviceAddress &= real.v12.bufferDeviceAddress;
                    dst->separateDepthStencilLayouts &= real.v12.separateDepthStencilLayouts;

                    // 8-bit storage group (some engines flip these)
                    dst->storageBuffer8BitAccess &= real.v12.storageBuffer8BitAccess;
                    dst->uniformAndStorageBuffer8BitAccess &= real.v12.uniformAndStorageBuffer8BitAccess;
                    dst->storagePushConstant8 &= real.v12.storagePushConstant8;

                    copied = (VkBaseInStructure*)dst;
                    break;
                }

                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES: {
                    auto* dst = AppendCopy(storage, (const VkPhysicalDeviceBufferDeviceAddressFeatures*)src);
                    dst->bufferDeviceAddress &= real.bda.bufferDeviceAddress;
                    dst->bufferDeviceAddressCaptureReplay &= real.bda.bufferDeviceAddressCaptureReplay;
                    dst->bufferDeviceAddressMultiDevice &= real.bda.bufferDeviceAddressMultiDevice;
                    copied = (VkBaseInStructure*)dst;
                    break;
                }

                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES: {
                    // The wrapper does not spoof these features. Dropping this
                    // node silently disables synchronization2 and dynamic rendering.
                    copied = (VkBaseInStructure*)AppendCopy(storage,
                        (const VkPhysicalDeviceVulkan13Features*)src);
                    break;
                }

                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEDICATED_ALLOCATION_IMAGE_ALIASING_FEATURES_NV: {
                    copied = (VkBaseInStructure*)AppendCopy(storage,
                        (const VkPhysicalDeviceDedicatedAllocationImageAliasingFeaturesNV*)src);
                    break;
                }

                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_INVOCATION_REORDER_FEATURES_NV: {
                    copied = (VkBaseInStructure*)AppendCopy(storage,
                        (const VkPhysicalDeviceRayTracingInvocationReorderFeaturesNV*)src);
                    break;
                }

                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES: {
                    auto* dst = AppendCopy(storage, (const VkPhysicalDeviceShaderFloat16Int8Features*)src);
                    dst->shaderFloat16 &= real.f16i8.shaderFloat16;
                    dst->shaderInt8 &= real.f16i8.shaderInt8;
                    copied = (VkBaseInStructure*)dst;
                    break;
                }

                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES: {
                    auto* dst = AppendCopy(storage, (const VkPhysicalDevice8BitStorageFeatures*)src);
                    dst->storageBuffer8BitAccess &= real.f8.storageBuffer8BitAccess;
                    dst->uniformAndStorageBuffer8BitAccess &= real.f8.uniformAndStorageBuffer8BitAccess;
                    dst->storagePushConstant8 &= real.f8.storagePushConstant8;
                    copied = (VkBaseInStructure*)dst;
                    break;
                }

                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES: {
                    auto* dst = AppendCopy(storage, (const VkPhysicalDevice16BitStorageFeatures*)src);
                    dst->storageBuffer16BitAccess &= real.f16.storageBuffer16BitAccess;
                    dst->uniformAndStorageBuffer16BitAccess &= real.f16.uniformAndStorageBuffer16BitAccess;
                    dst->storagePushConstant16 &= real.f16.storagePushConstant16;
                    dst->storageInputOutput16 &= real.f16.storageInputOutput16;
                    copied = (VkBaseInStructure*)dst;
                    break;
                }

                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SEPARATE_DEPTH_STENCIL_LAYOUTS_FEATURES: {
                    auto* dst = AppendCopy(storage, (const VkPhysicalDeviceSeparateDepthStencilLayoutsFeatures*)src);
                    dst->separateDepthStencilLayouts &= real.sep.separateDepthStencilLayouts;
                    copied = (VkBaseInStructure*)dst;
                    break;
                }

                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT: {
                    auto* dst = AppendCopy(storage, (const VkPhysicalDeviceRobustness2FeaturesEXT*)src);
                    dst->nullDescriptor &= real.rob2.nullDescriptor;
                    dst->robustBufferAccess2 &= real.rob2.robustBufferAccess2;
                    dst->robustImageAccess2 &= real.rob2.robustImageAccess2;
                    copied = (VkBaseInStructure*)dst;
                    break;
                }

                default:
                    // Still conservative for now:
                    thanosray::log::Logf("[vkCreateDevice] DROP unknown pNext node sType=%d (to avoid driver crash)", (int)sType);
                    copied = nullptr;
                    break;
                }

                if (copied) {
                    copied->pNext = nullptr;
                    if (!head) head = copied;
                    if (tail) tail->pNext = copied;
                    tail = copied;
                }

                src = src->pNext;
            }

            return head;
        }


        static VkResult VKAPI_CALL Hook_vkCreateDevice(
            VkPhysicalDevice physicalDevice,
            const VkDeviceCreateInfo* pCreateInfo,
            const VkAllocationCallbacks* pAllocator,
            VkDevice* pDevice)
        {

            static bool s_onceBuild = false;
            if (!s_onceBuild) {
                s_onceBuild = true;
                thanosray::log::Logf("[BUILD] %s", kVkHooksBuildId);
            }


            auto& st = State();
            EnsureShadowLiftEnv();
            if (!st.real_vkCreateDevice || !pCreateInfo || !pDevice) {
                return VK_ERROR_INITIALIZATION_FAILED;
            }

            const auto driverSupported = QueryDriverExts(physicalDevice);

            std::vector<const char*> driverExts;
            driverExts.reserve((size_t)pCreateInfo->enabledExtensionCount + 16);

            thanosray::log::Logf("[vkCreateDevice] requested=%u", pCreateInfo->enabledExtensionCount);

            for (uint32_t i = 0; i < pCreateInfo->enabledExtensionCount; ++i) {
                const char* n = pCreateInfo->ppEnabledExtensionNames ? pCreateInfo->ppEnabledExtensionNames[i] : nullptr;
                if (!n) continue;

                if (IsRtExt(n)) {
                    thanosray::log::Logf("[vkCreateDevice] DROP RT ext (engine-only): %s", n);
                    continue;
                }

                if (!driverSupported.empty() && driverSupported.find(n) == driverSupported.end()) {
                    thanosray::log::Logf("[vkCreateDevice] DROP unsupported by driver: %s", n);
                    continue;
                }

                driverExts.push_back(n);
            }

            // Pascal exposes native AS/RT pipeline entry points even though it
            // lacks rayQuery and Float16. The upstream proxy advertises RT but
            // strips all RT extensions/features. Restore the supported native
            // subset; this alone does not make DOOM's RT initialization succeed.
            VkPhysicalDeviceFeatures2 nativeFeatures{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            VkPhysicalDeviceAccelerationStructureFeaturesKHR nativeAS{
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
            VkPhysicalDeviceRayTracingPipelineFeaturesKHR nativeRT{
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR};
            VkPhysicalDeviceFaultFeaturesEXT nativeFault{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT};
            nativeFeatures.pNext = &nativeAS;
            nativeAS.pNext = &nativeRT;
            nativeRT.pNext = &nativeFault;
            if (st.real_vkGetPhysicalDeviceFeatures2)
                st.real_vkGetPhysicalDeviceFeatures2(physicalDevice, &nativeFeatures);
            const bool useNativeAS = nativeAS.accelerationStructure &&
                driverSupported.count(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME) &&
                driverSupported.count(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
            const bool useNativeRT = useNativeAS && nativeRT.rayTracingPipeline &&
                driverSupported.count(VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME);
            const bool useDeviceFault = runtime::Enabled() && nativeFault.deviceFault &&
                driverSupported.count(VK_EXT_DEVICE_FAULT_EXTENSION_NAME);
            auto addExtension = [&](const char* name) {
                if (std::none_of(driverExts.begin(), driverExts.end(),
                    [&](const char* old) { return std::strcmp(old, name) == 0; }))
                    driverExts.push_back(name);
            };
            if (useNativeAS) {
                addExtension(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
                addExtension(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
            }
            if (useNativeRT) addExtension(VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME);
            if (useDeviceFault) addExtension(VK_EXT_DEVICE_FAULT_EXTENSION_NAME);
            thanosray::log::Logf("[NATIVE-RT] accelerationStructure=%d rayTracingPipeline=%d",
                useNativeAS, useNativeRT);

            thanosray::log::Logf("[vkCreateDevice] passing(driver)=%u", (uint32_t)driverExts.size());
            for (size_t i = 0; i < driverExts.size(); ++i) {
                thanosray::log::Logf("  pass[%zu]=%s", i, driverExts[i]);
            }

            DumpInChain(pCreateInfo->pNext, "[vkCreateDevice] original");

            std::vector<std::vector<uint8_t>> pnextStorage;
            const void* sanitizedPNext = BuildSanitizedDeviceCreatePNext(
                physicalDevice, pCreateInfo->pNext, pnextStorage);

            if (useDeviceFault) {
                nativeFault.deviceFaultVendorBinary = VK_FALSE;
                nativeFault.pNext = const_cast<void*>(sanitizedPNext);
                sanitizedPNext = &nativeFault;
            }
            if (useNativeRT) {
                nativeRT.pNext = const_cast<void*>(sanitizedPNext);
                sanitizedPNext = &nativeRT;
            }
            if (useNativeAS) {
                nativeAS.pNext = const_cast<void*>(sanitizedPNext);
                sanitizedPNext = &nativeAS;
            }

            DumpInChain(sanitizedPNext, "[vkCreateDevice] sanitized");

            VkDeviceCreateInfo patched = *pCreateInfo;
            patched.enabledExtensionCount = (uint32_t)driverExts.size();
            patched.ppEnabledExtensionNames = driverExts.empty() ? nullptr : driverExts.data();
            patched.pNext = sanitizedPNext;

            robustness::Storage robustStorage;
            if (GetPrivateProfileIntW(L"experiments", L"RobustAccess", 0, diagnostics::ConfigPath().c_str())) {
                const auto supported = QueryRealFeatures(physicalDevice);
                VkPhysicalDeviceVulkan12Properties properties12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES};
                VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
                properties.pNext = &properties12;
                if (st.real_vkGetPhysicalDeviceProperties2)
                    st.real_vkGetPhysicalDeviceProperties2(physicalDevice, &properties);
                const auto enabled = robustness::Apply(patched, robustStorage, supported.core.features, supported.rob2,
                    driverSupported.count(VK_EXT_ROBUSTNESS_2_EXTENSION_NAME) != 0, properties12.robustBufferAccessUpdateAfterBind);
                if (enabled.needsExtension) addExtension(VK_EXT_ROBUSTNESS_2_EXTENSION_NAME);
                // addExtension can reallocate the vector: refresh the pointer.
                patched.enabledExtensionCount = (uint32_t)driverExts.size();
                patched.ppEnabledExtensionNames = driverExts.empty() ? nullptr : driverExts.data();
                thanosray::log::Logf("[ROBUST] buffer=%u buffer2=%u image2=%u nullDescriptor=%u updateAfterBind=%u",
                    enabled.buffer, enabled.buffer2, enabled.image2, enabled.nullDescriptor, properties12.robustBufferAccessUpdateAfterBind);
            }

            thanosray::log::Logf("[vkCreateDevice] calling real vkCreateDevice...");
            VkResult r = st.real_vkCreateDevice(physicalDevice, &patched, pAllocator, pDevice);
            thanosray::log::Logf("[vkCreateDevice] real returned: %d", (int)r);
            if (r == VK_SUCCESS)
                runtime::OnDeviceCreated(*pDevice, st.real_vkGetDeviceProcAddr, useDeviceFault);

            EnsureShadowLiftDefaultsLate();
            return r;
        }

        // -----------------------------------------------------------------------------------------
        // Shadow-lift postprocess (optional, OFF by default)
        //
        // Goal: lift ONLY deep shadows (missing indirect light) without washing out highlights.
        // This is applied as a compute pass on the swapchain image just before vkQueuePresentKHR.
        //
        // Enable by setting env var:
        //   THR_SHADOW_LIFT=0.20      (0 disables; typical 0.10-0.30)
        // Optional tuning:
        //   THR_SHADOW_TOE=0.25       (shadow range; lower = affect fewer pixels)
        //   THR_SHADOW_CEIL=0.85      (protect highlights; higher = protect less)
        //
        // Requires a SPIR-V compute shader placed next to the game's working directory:
        //   shadowlift.comp.spv
        //
        // If anything fails, we log once and fall back to normal present (no behavior change).
        // -----------------------------------------------------------------------------------------
        /**/

        /*
        static bool GetEnvFloat(const char* name, float& out)
        {
            out = 0.0f;
            char* buf = nullptr;
            size_t len = 0;
            if (_dupenv_s(&buf, &len, name) != 0 || !buf) return false;
            if (len <= 1) { free(buf); return false; }
            out = (float)atof(buf);
            free(buf);
            return true;
        }

        */
        static bool GetEnvFloat(const char* name, float& out)
        {
            // 1) Prefer WinAPI environment (matches EnsureShadowLiftEnv)
            char buf[128] = {};
            DWORD n = GetEnvironmentVariableA(name, buf, (DWORD)sizeof(buf));
            if (n > 0 && n < (DWORD)sizeof(buf)) {
                out = (float)atof(buf);
                return true;
            }

            // 2) Fallback: CRT environment (_dupenv_s)
            char* tmp = nullptr;
            size_t len = 0;
            if (_dupenv_s(&tmp, &len, name) == 0 && tmp && len > 1) {
                out = (float)atof(tmp);
                free(tmp);
                return true;
            }
            if (tmp) free(tmp);

            return false;
        }



        
        /*

        static float ShadowLiftStrength()
        {
            static std::atomic<int> cached{ -1 };
            static std::atomic<float> val{ 0.0f };
            int c = cached.load(std::memory_order_acquire);
            if (c == 1) return val.load(std::memory_order_relaxed);
            //float v = 0.0f;
            //GetEnvFloat("THR_SHADOW_LIFT", v);
            float v = 0.0f;
            bool ok = GetEnvFloat("THR_SHADOW_LIFT", v);

            static bool once = false;
            if (!once) {
                once = true;
                thanosray::log::Logf("[SL] THR_SHADOW_LIFT read: ok=%d value=%.3f", ok ? 1 : 0, v);
            }

            val.store(v, std::memory_order_relaxed);

            // cache state: 1 = valid (we have a value we want to keep)
            //              0 = invalid (env missing) -> re-check later
            cached.store(ok ? 1 : 0, std::memory_order_release);

            return v;
        }*/

        static float ShadowLiftStrength()
        {

            static std::atomic<int> cached{ -1 };
            static std::atomic<float> val{ 0.0f };

            int c = cached.load(std::memory_order_acquire);
            if (c == 1) return val.load(std::memory_order_relaxed);

            float v = 0.0f;
            bool ok = GetEnvFloat("THR_SHADOW_LIFT", v);

            val.store(ok ? v : 0.0f, std::memory_order_relaxed);
            cached.store(ok ? 1 : 0, std::memory_order_release);
            return ok ? v : 0.0f;
        }


        static float ShadowLiftToe()  { float v=0.00f; GetEnvFloat("THR_SHADOW_TOE", v);  return v; }
        static float ShadowLiftCeil() { float v=1.00f; GetEnvFloat("THR_SHADOW_CEIL", v); return v; }

        static bool ReadFileBytes(const char* path, std::vector<uint8_t>& out)
        {
            out.clear();
            std::ifstream f(path, std::ios::binary);
            if (!f.good()) return false;
            f.seekg(0, std::ios::end);
            std::streamoff sz = f.tellg();
            if (sz <= 0) return false;
            f.seekg(0, std::ios::beg);
            out.resize((size_t)sz);
            f.read((char*)out.data(), sz);
            return f.good();
        }
        struct ShadowLiftDevice
        {
            VkDevice dev = VK_NULL_HANDLE;
            VkQueue  presentQueue = VK_NULL_HANDLE;
            uint32_t queueFamily = 0;
            bool     queueFamilyValid = false;

            // Core
            PFN_vkGetDeviceProcAddr vkGetDeviceProcAddr = nullptr;
            PFN_vkCreateCommandPool vkCreateCommandPool = nullptr;
            PFN_vkDestroyCommandPool vkDestroyCommandPool = nullptr;
            PFN_vkAllocateCommandBuffers vkAllocateCommandBuffers = nullptr;
            PFN_vkFreeCommandBuffers vkFreeCommandBuffers = nullptr;
            PFN_vkBeginCommandBuffer vkBeginCommandBuffer = nullptr;
            PFN_vkEndCommandBuffer vkEndCommandBuffer = nullptr;
            PFN_vkResetCommandBuffer vkResetCommandBuffer = nullptr;
            PFN_vkQueueSubmit vkQueueSubmit = nullptr;
            PFN_vkWaitForFences vkWaitForFences = nullptr;
            PFN_vkResetFences vkResetFences = nullptr;
            PFN_vkCreateFence vkCreateFence = nullptr;
            PFN_vkDestroyFence vkDestroyFence = nullptr;

            PFN_vkCmdPipelineBarrier vkCmdPipelineBarrier = nullptr;
            PFN_vkCmdBindPipeline vkCmdBindPipeline = nullptr;
            PFN_vkCmdBindDescriptorSets vkCmdBindDescriptorSets = nullptr;
            PFN_vkCmdPushConstants vkCmdPushConstants = nullptr;
            PFN_vkCmdDispatch vkCmdDispatch = nullptr;

            // Descriptors / pipeline
            PFN_vkCreateShaderModule vkCreateShaderModule = nullptr;
            PFN_vkDestroyShaderModule vkDestroyShaderModule = nullptr;
            PFN_vkCreateDescriptorSetLayout vkCreateDescriptorSetLayout = nullptr;
            PFN_vkDestroyDescriptorSetLayout vkDestroyDescriptorSetLayout = nullptr;
            PFN_vkCreatePipelineLayout vkCreatePipelineLayout = nullptr;
            PFN_vkDestroyPipelineLayout vkDestroyPipelineLayout = nullptr;
            PFN_vkCreateComputePipelines vkCreateComputePipelines = nullptr;
            PFN_vkDestroyPipeline vkDestroyPipeline = nullptr;
            PFN_vkCreateDescriptorPool vkCreateDescriptorPool = nullptr;
            PFN_vkDestroyDescriptorPool vkDestroyDescriptorPool = nullptr;
            PFN_vkAllocateDescriptorSets vkAllocateDescriptorSets = nullptr;
            PFN_vkUpdateDescriptorSets vkUpdateDescriptorSets = nullptr;

            // Images
            PFN_vkCreateImageView vkCreateImageView = nullptr;
            PFN_vkDestroyImageView vkDestroyImageView = nullptr;

            // Cached objects
            VkCommandPool cmdPool = VK_NULL_HANDLE;
            VkFence fence = VK_NULL_HANDLE;

            VkShaderModule shader = VK_NULL_HANDLE;
            VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
            VkPipelineLayout ppl = VK_NULL_HANDLE;
            VkPipeline pipe = VK_NULL_HANDLE;
            VkDescriptorPool dsp = VK_NULL_HANDLE;

            bool initAttempted = false;
            bool initOk = false;
            bool loggedFail = false;
        };

        struct SwapchainState
        {
            VkDevice dev = VK_NULL_HANDLE;
            VkSwapchainKHR sc = VK_NULL_HANDLE;
            VkFormat fmt = VK_FORMAT_UNDEFINED;
            VkExtent2D extent{ 0,0 };
            std::vector<VkImage> images;
            std::vector<VkImageView> views;
            std::vector<VkDescriptorSet> sets;
        };

        static std::mutex g_shadowMtx;
        static std::unordered_map<VkDevice, ShadowLiftDevice> g_shadowDev;
        static std::unordered_map<VkSwapchainKHR, SwapchainState> g_swap;

        // Thread-local present wait semaphores for ShadowLift submit.
        // Set in Hook_vkQueuePresentKHR before calling ApplyShadowLiftToSwapchainImage.
        static thread_local uint32_t tl_presentWaitCount = 0;
        static thread_local const VkSemaphore* tl_presentWaitSems = nullptr;


        // Queue capture (export-level)


        /* restore if breaks
        static void ApplyShadowLiftToSwapchainImage(VkQueue queue, VkSwapchainKHR sc, uint32_t imageIndex)
        {
           // float strength = ShadowLiftStrength();
            static bool s_onceApply = false;
            if (!s_onceApply) {
                s_onceApply = true;
                thanosray::log::Logf("[SL] ApplyShadowLiftToSwapchainImage entered");
            }


            float strength = ShadowLiftStrength();
            static bool s_onceStrength = false;
            if (!s_onceStrength) {
                s_onceStrength = true;
                thanosray::log::Logf("[SL] strength=%.3f", strength);
            }


            if (strength <= 0.0f) { static bool once=false; if(!once){once=true; thanosray::log::Logf("[SL] skip: strength<=0");} return; }
            // ----------------------------------------------------------------
            // LANDMARK 60: ShadowLift prove-it logging
            // ----------------------------------------------------------------
            static bool s_loggedOnce = false;
            if (!s_loggedOnce) {
                s_loggedOnce = true;
                thanosray::log::Logf("[SL] ShadowLift enabled: strength=%.3f toe=%.3f ceil=%.3f",
                    strength, ShadowLiftToe(), ShadowLiftCeil());
            }


            std::lock_guard<std::mutex> lk(g_shadowMtx);
            auto sit = g_swap.find(sc);
            if (sit == g_swap.end()) {
                static bool once = false;
                if (!once) {
                    once = true;
                    thanosray::log::Logf("[SL] ApplyShadowLift: swapchain %p NOT FOUND in g_swap (size=%zu)",
                        (void*)sc, g_swap.size());
                }
                return;
            }



            if (sit == g_swap.end()) { static bool once=false; if(!once){once=true; thanosray::log::Logf("[SL] skip: swapchain not tracked");} return; }
            SwapchainState& s = sit->second;
            if (imageIndex >= s.images.size()) return;

            ShadowLiftDevice& d = g_shadowDev[s.dev];
            d.dev = s.dev;

            // queue family must have been captured earlier
            if (!ShadowLiftInitLocked(d, s.dev)) {
                if (!d.loggedFail) {
                    thanosray::log::Logf("[SHADOWLIFT] init failed (missing queue family / shader / functions). Falling back.");
                    d.loggedFail = true;
                }
                return;
            }
            if (!EnsureSwapchainDescriptorsLocked(d, s)) { static bool once=false; if(!once){once=true; thanosray::log::Logf("[SL] skip: EnsureSwapchainDescriptorsLocked failed");} return; }
            if (imageIndex >= s.images.size()) {
                static bool once2 = false;
                if (!once2) {
                    once2 = true;
                    thanosray::log::Logf("[SL] ApplyShadowLift: imageIndex=%u out of range (images=%zu) sc=%p",
                        imageIndex, s.images.size(), (void*)sc);
                }
                return;
            }

            // One-off command buffer
            VkCommandBufferAllocateInfo cba{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
            cba.commandPool = d.cmdPool;
            cba.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            cba.commandBufferCount = 1;
            VkCommandBuffer cb = VK_NULL_HANDLE;
            if (d.vkAllocateCommandBuffers(s.dev, &cba, &cb) != VK_SUCCESS) return;

            VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
            bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            if (d.vkBeginCommandBuffer(cb, &bi) != VK_SUCCESS) {
                d.vkFreeCommandBuffers(s.dev, d.cmdPool, 1, &cb);
                return;
            }

            VkImageMemoryBarrier toGen{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
            toGen.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            toGen.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            toGen.srcAccessMask = 0;
            toGen.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            toGen.image = s.images[imageIndex];
            toGen.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            toGen.subresourceRange.levelCount = 1;
            toGen.subresourceRange.layerCount = 1;

            d.vkCmdPipelineBarrier(cb,
                VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                0, 0, nullptr, 0, nullptr, 1, &toGen);

            d.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, d.pipe);
            VkDescriptorSet set = s.sets[imageIndex];
            d.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, d.ppl, 0, 1, &set, 0, nullptr);

            float pc[4] = { strength, ShadowLiftToe(), ShadowLiftCeil(), 0.0f };
            d.vkCmdPushConstants(cb, d.ppl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), pc);

            uint32_t gx = (s.extent.width + 15) / 16;
            uint32_t gy = (s.extent.height + 15) / 16;
            static bool onceGrid=false; if(!onceGrid){onceGrid=true; thanosray::log::Logf("[SL] dispatch grid %ux%u for extent %ux%u fmt=%d", gx, gy, s.extent.width, s.extent.height, (int)s.fmt);} 
            d.vkCmdDispatch(cb, gx, gy, 1);

            VkImageMemoryBarrier toPres{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
            toPres.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
            toPres.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            toPres.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            toPres.dstAccessMask = 0;
            toPres.image = s.images[imageIndex];
            toPres.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            toPres.subresourceRange.levelCount = 1;
            toPres.subresourceRange.layerCount = 1;

            d.vkCmdPipelineBarrier(cb,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                0, 0, nullptr, 0, nullptr, 1, &toPres);

            (void)d.vkEndCommandBuffer(cb);

            (void)d.vkResetFences(s.dev, 1, &d.fence);

            // Respect the app's present wait semaphores to avoid racing rendering work.
            uint32_t wc = tl_presentWaitCount;
            const VkSemaphore* ws = tl_presentWaitSems;
            if (wc > 0 && ws) {
                // cap to a small fixed array to avoid heap alloc
                if (wc > 16) wc = 16;
            } else {
                wc = 0;
                ws = nullptr;
            }

            VkPipelineStageFlags waitStages[16];
            for (uint32_t i = 0; i < wc; ++i) waitStages[i] = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;

            VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
            si.waitSemaphoreCount = wc;
            si.pWaitSemaphores = (wc ? ws : nullptr);
            si.pWaitDstStageMask = (wc ? waitStages : nullptr);
            si.commandBufferCount = 1;
            si.pCommandBuffers = &cb;

            // --- SHADOWLIFT GPU EXECUTION (TEMP DISABLED FOR STABILITY) ---


            // --------------------------------------------------------------------
// LANDMARK 61: SHADOWLIFT GPU EXECUTION (ENABLED)
// --------------------------------------------------------------------
// If this submit doesn't run, ShadowLift does nothing. (it keeps killing the wrapper

            if (d.vkQueueSubmit && d.vkWaitForFences) {
                // Submit the tiny "lift" pass and wait briefly so it's safe to recycle the command buffer.
                VkResult qs = d.vkQueueSubmit(queue, 1, &si, d.fence);
    static bool onceSubmit=false; if(!onceSubmit){onceSubmit=true; thanosray::log::Logf("[SL] vkQueueSubmit result=%d waitSema=%u", (int)qs, si.waitSemaphoreCount);} 
                if (qs == VK_SUCCESS) {
                    // Keep the wait short to avoid stalling too hard; lift is best-effort.
                    // You can increase this for debugging via THR_SL_WAIT_MS (milliseconds).
                    uint32_t waitMs = 5;
                    char buf[64] = {};
                    DWORD n = GetEnvironmentVariableA("THR_SL_WAIT_MS", buf, (DWORD)sizeof(buf));
                    if (n > 0) {
                        waitMs = (uint32_t)strtoul(buf, nullptr, 10);
                        if (waitMs > 500) waitMs = 500;
                    }
                    (void)d.vkWaitForFences(dev, 1, &d.fence, VK_TRUE, (uint64_t)waitMs * 1000000ULL);
                } else {
                    static bool onceSubmitFail = false;
                    if (!onceSubmitFail) {
                        onceSubmitFail = true;
                        thanosray::log::Logf("[SL] vkQueueSubmit failed: %d (ShadowLift not applied)", (int)qs);
                    }
                }
            } else {
                static bool onceNoFns = false;
                if (!onceNoFns) {
                    onceNoFns = true;
                    thanosray::log::Logf("[SL] Missing vkQueueSubmit/vkWaitForFences pointers (ShadowLift not applied)");
                }
            }


            /*
            if (d.vkQueueSubmit(queue, 1, &si, d.fence) == VK_SUCCESS) {
                (void)d.vkWaitForFences(s.dev, 1, &d.fence, VK_TRUE, 500000000ULL); // 0.5s
            }
            


            d.vkFreeCommandBuffers(s.dev, d.cmdPool, 1, &cb);
        }*/

static void ApplyShadowLiftToSwapchainImage(
    VkQueue queue,
    const VkPresentInfoKHR* presentInfo,   // <-- NEW (can be null)
    VkSwapchainKHR sc,
    uint32_t imageIndex)
{
    // One-time proof we hit
    static bool s_onceApply = false;
    if (!s_onceApply) { s_onceApply = true; thanosray::log::Logf("[SL] ApplyShadowLiftToSwapchainImage entered"); }

    float strength = ShadowLiftStrength();
    static bool s_onceStrength = false;
    if (!s_onceStrength) { s_onceStrength = true; thanosray::log::Logf("[SL] strength=%.3f", strength); }

    if (strength <= 0.0f) return;

    // Grab swapchain/device info quickly, then RELEASE the mutex before GPU work.
    VkDevice dev = VK_NULL_HANDLE;
    VkImage  img = VK_NULL_HANDLE;
    VkExtent2D extent{ 0,0 };
    VkDescriptorSet set = VK_NULL_HANDLE;

    {
        std::unique_lock<std::mutex> lk(g_shadowMtx);
        auto sit = g_swap.find(sc);
        if (sit == g_swap.end()) return;

        SwapchainState& s = sit->second;
        if (imageIndex >= s.images.size() || imageIndex >= s.sets.size()) { static bool once=false; if(!once){once=true; thanosray::log::Logf("[SL] skip: imageIndex out of range or descriptors not ready (idx=%u images=%zu sets=%zu)", imageIndex, (size_t)s.images.size(), (size_t)s.sets.size());} return; }

        dev = s.dev;
        img = s.images[imageIndex];
        extent = s.extent;
        set = s.sets[imageIndex];

        // Make sure ShadowLift device is initialized
        ShadowLiftDevice& d = g_shadowDev[dev];
        d.dev = dev;
        if (!ShadowLiftInitLocked(d, dev)) { static bool once=false; if(!once){once=true; thanosray::log::Logf("[SL] skip: ShadowLiftInitLocked failed");} return; }
        if (!EnsureSwapchainDescriptorsLocked(d, s)) return;
    }

    // Re-fetch device record after unlock (map entry exists)
    ShadowLiftDevice* pd = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_shadowMtx);
        pd = &g_shadowDev[dev];
    }
    ShadowLiftDevice& d = *pd;

    // Allocate command buffer
    VkCommandBufferAllocateInfo cba{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cba.commandPool = d.cmdPool;
    cba.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cba.commandBufferCount = 1;

    VkCommandBuffer cb = VK_NULL_HANDLE;
    if (d.vkAllocateCommandBuffers(dev, &cba, &cb) != VK_SUCCESS) return;

    VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (d.vkBeginCommandBuffer(cb, &bi) != VK_SUCCESS) {
        d.vkFreeCommandBuffers(dev, d.cmdPool, 1, &cb);
        return;
    }

    // Transition swapchain image to GENERAL for storage-image write
    VkImageMemoryBarrier toGen{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    toGen.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;     // best guess
    toGen.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    toGen.srcAccessMask = 0;
    toGen.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    toGen.image = img;
    toGen.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    toGen.subresourceRange.levelCount = 1;
    toGen.subresourceRange.layerCount = 1;

    d.vkCmdPipelineBarrier(cb,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &toGen);

    d.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, d.pipe);
    d.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, d.ppl, 0, 1, &set, 0, nullptr);

    float pc[4] = { strength, ShadowLiftToe(), ShadowLiftCeil(), 0.0f };
    d.vkCmdPushConstants(cb, d.ppl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), pc);

    uint32_t gx = (extent.width + 15) / 16;
    uint32_t gy = (extent.height + 15) / 16;
    d.vkCmdDispatch(cb, gx, gy, 1);

    VkImageMemoryBarrier toPres{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    toPres.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    toPres.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    toPres.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    toPres.dstAccessMask = 0;
    toPres.image = img;
    toPres.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    toPres.subresourceRange.levelCount = 1;
    toPres.subresourceRange.layerCount = 1;

    d.vkCmdPipelineBarrier(cb,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        0, 0, nullptr, 0, nullptr, 1, &toPres);

    (void)d.vkEndCommandBuffer(cb);

    // -------------------------
    // CRITICAL FIX:
    // Wait on the SAME semaphores that present will wait on, so we don't race the renderer.
    // -------------------------
    VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;

    std::vector<VkPipelineStageFlags> waitStages;
    if (presentInfo && presentInfo->waitSemaphoreCount && presentInfo->pWaitSemaphores) {
        waitStages.resize(presentInfo->waitSemaphoreCount, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        si.waitSemaphoreCount = presentInfo->waitSemaphoreCount;
        si.pWaitSemaphores = presentInfo->pWaitSemaphores;
        si.pWaitDstStageMask = waitStages.data();
    }

    (void)d.vkResetFences(dev, 1, &d.fence);
    
    
    // Release state lock before submitting GPU work.
 //   lk.unlock();
VkResult qs = d.vkQueueSubmit(queue, 1, &si, d.fence);
    if (qs == VK_SUCCESS) {
        // Debug-first: block until done so the effect is visible
        (void)d.vkWaitForFences(dev, 1, &d.fence, VK_TRUE, 500000000ULL); // 0.5s
    }
    else {
        static bool onceFail = false;
        if (!onceFail) {
            onceFail = true;
            auto sit = g_swap.find(sc);
            if (sit == g_swap.end()) return;
            SwapchainState& s = sit->second;
            VkClearColorValue cc{};
cc.float32[0] = 1.0f; // R
cc.float32[1] = 0.0f; // G
cc.float32[2] = 1.0f; // B
cc.float32[3] = 1.0f; // A

VkImageSubresourceRange range{};
range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
range.baseMipLevel = 0; range.levelCount = 1;
range.baseArrayLayer = 0; range.layerCount = 1;
///delete on error
vkCmdClearColorImage(cb, s.images[imageIndex], VK_IMAGE_LAYOUT_GENERAL, &cc, 1, &range);


            thanosray::log::Logf("[SL] vkQueueSubmit failed: %d", (int)qs);
            thanosray::log::Logf("[SL] submit: swapFmt=%d extent=%ux%u imgIndex=%u waitSems=%u",
                (int)s.fmt,
                s.extent.width, s.extent.height,
                (unsigned)imageIndex,
                (unsigned)si.waitSemaphoreCount);
        }
    }

    d.vkFreeCommandBuffers(dev, d.cmdPool, 1, &cb);
}


        static void VKAPI_CALL Hook_vkGetDeviceQueue(VkDevice device, uint32_t queueFamilyIndex, uint32_t queueIndex, VkQueue* pQueue)
        {
            PFN_vkGetDeviceQueue real = g_real_vkGetDeviceQueue ? g_real_vkGetDeviceQueue : g_gdpa_vkGetDeviceQueue;
            if (!real) return;

            real(device, queueFamilyIndex, queueIndex, pQueue);

            if (pQueue && *pQueue) {
                std::lock_guard<std::mutex> lk(g_shadowMtx);
                auto& d = g_shadowDev[device];
                d.dev = device;
                d.presentQueue = *pQueue; // best guess: app will present from this queue
                d.queueFamily = queueFamilyIndex;
                d.queueFamilyValid = true;

                static bool once = false;
                if (!once) {
                    once = true;
                    thanosray::log::Logf("[SL] GetDeviceQueue captured: q=%p family=%u index=%u",
                        (void*)*pQueue, queueFamilyIndex, queueIndex);
                }
            }
        }





        static void VKAPI_CALL Hook_vkGetDeviceQueue2(VkDevice device, const VkDeviceQueueInfo2* pQueueInfo, VkQueue* pQueue)
        {
            if (!g_real_vkGetDeviceQueue2) return;
            g_real_vkGetDeviceQueue2(device, pQueueInfo, pQueue);

            if (pQueueInfo && pQueue && *pQueue) {
                std::lock_guard<std::mutex> lk(g_shadowMtx);
                auto& d = g_shadowDev[device];
                d.dev = device;
                d.presentQueue = *pQueue;
                d.queueFamily = pQueueInfo->queueFamilyIndex;
                d.queueFamilyValid = true;
                static bool once = false;
                if (!once) {
                    once = true;
                    thanosray::log::Logf("[SL] GetDeviceQueue2 captured: q=%p family=%u",
                        (void*)*pQueue, pQueueInfo->queueFamilyIndex);
                }

            }
            PFN_vkGetDeviceQueue2 real = g_real_vkGetDeviceQueue2 ? g_real_vkGetDeviceQueue2 : g_gdpa_vkGetDeviceQueue2;
            if (!real) return;
            real(device, pQueueInfo, pQueue);

        }

        // Swapchain capture (export-level)
        static PFN_vkCreateSwapchainKHR    g_real_vkCreateSwapchainKHR = nullptr;
        static PFN_vkDestroySwapchainKHR   g_real_vkDestroySwapchainKHR = nullptr;
        static PFN_vkGetSwapchainImagesKHR g_real_vkGetSwapchainImagesKHR = nullptr;
        //static PFN_vkQueuePresentKHR       g_real_vkQueuePresentKHR = nullptr;
        static PFN_vkAcquireNextImageKHR   g_real_vkAcquireNextImageKHR = nullptr;
        static PFN_vkAcquireNextImage2KHR  g_real_vkAcquireNextImage2KHR = nullptr;
        static PFN_vkCmdPipelineBarrier GetCmdPipelineBarrier(VkDevice dev);
        static PFN_vkCmdClearColorImage GetCmdClearColorImage(VkDevice dev);


        // --------------------------------------------------------------------
        // LANDMARK 40: Optional GDPA-based swapchain/present hooks
        // Some games fetch swapchain/present via vkGetDeviceProcAddr; others call
        // export thunks directly. We support BOTH paths without breaking either.
        // These pointers are only used if the game requests them via GDPA.
        // --------------------------------------------------------------------
        static VkResult VKAPI_CALL Hook_vkCreateSwapchainKHR(
            VkDevice device,
            const VkSwapchainCreateInfoKHR* pCreateInfo,
            const VkAllocationCallbacks* pAllocator,
            VkSwapchainKHR* pSwapchain)
        {
            if (!g_real_vkCreateSwapchainKHR) return VK_ERROR_INITIALIZATION_FAILED;
            if (!pCreateInfo) return g_real_vkCreateSwapchainKHR(device, pCreateInfo, pAllocator, pSwapchain);

            // Attempt 1: request STORAGE usage for ShadowLift (fail-open)
            VkSwapchainCreateInfoKHR patched = *pCreateInfo;
            patched.imageUsage |= (VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);

            VkResult r = g_real_vkCreateSwapchainKHR(device, &patched, pAllocator, pSwapchain);

            // If driver rejects STORAGE usage, fall back to original so we don't break the game.
            if (r != VK_SUCCESS) {
                r = g_real_vkCreateSwapchainKHR(device, pCreateInfo, pAllocator, pSwapchain);
                if (r == VK_SUCCESS) {
                    thanosray::log::Logf("[SL] Swapchain STORAGE usage rejected; ShadowLift will be disabled for swapchain.");
                }
            }
            else {
                // Optional: log once so we know it succeeded.
                static bool once = false;
                if (!once) {
                    once = true;
                    thanosray::log::Logf("[SL] Swapchain created with STORAGE usage (ShadowLift compatible).");
                }
            }

            if (r == VK_SUCCESS && pSwapchain && *pSwapchain) {
                std::lock_guard<std::mutex> lk(g_shadowMtx);
                SwapchainState& s = g_swap[*pSwapchain];
                s.dev = device;
                s.sc = *pSwapchain;
                s.fmt = pCreateInfo->imageFormat;
                s.extent = pCreateInfo->imageExtent;
                s.images.clear();
                s.views.clear();
                s.sets.clear();
            }
            return r;
        }


        static void VKAPI_CALL Hook_vkDestroySwapchainKHR(
            VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks* pAllocator)
        {
            {
                std::lock_guard<std::mutex> lk(g_shadowMtx);
                auto it = g_swap.find(swapchain);
                if (it != g_swap.end()) {
                    // clean up image views (pipeline objects are per-device)
                    ShadowLiftDevice* dd = nullptr;
                    auto dit = g_shadowDev.find(device);
                    if (dit != g_shadowDev.end()) dd = &dit->second;
                    if (dd && dd->initOk && dd->vkDestroyImageView) {
                        for (auto v : it->second.views) if (v) dd->vkDestroyImageView(device, v, nullptr);
                    }
                    g_swap.erase(it);
                }
            }
            if (g_real_vkDestroySwapchainKHR) g_real_vkDestroySwapchainKHR(device, swapchain, pAllocator);
        }

        static VkResult VKAPI_CALL Hook_vkGetSwapchainImagesKHR(
            VkDevice device, VkSwapchainKHR swapchain, uint32_t* pSwapchainImageCount, VkImage* pSwapchainImages)
        {
            if (!g_real_vkGetSwapchainImagesKHR) return VK_ERROR_INITIALIZATION_FAILED;
            VkResult r = g_real_vkGetSwapchainImagesKHR(device, swapchain, pSwapchainImageCount, pSwapchainImages);
            if (r == VK_SUCCESS && pSwapchainImages && pSwapchainImageCount) {
                std::lock_guard<std::mutex> lk(g_shadowMtx);
                auto it = g_swap.find(swapchain);
                if (it != g_swap.end()) {
                    it->second.images.assign(pSwapchainImages, pSwapchainImages + (*pSwapchainImageCount));
                    it->second.views.clear();
                    it->second.sets.clear();
                }
            }


            return r;
        }

        // --------------------------------------------------------------------
// LANDMARK SL-ACQ: ShadowLift injection moved to AcquireNextImage
// Rationale: Some engines call vkQueuePresentKHR with swapchainCount==0.
// AcquireNextImage always identifies a concrete swapchain + image index.
// Fail-open: we always return the real result; ShadowLift is best-effort.
// --------------------------------------------------------------------
      /*
        static void TryShadowLiftFromAcquire(VkDevice device, VkSwapchainKHR swapchain, uint32_t imageIndex)
        {
            // AcquireNextImage timing is often unsafe to touch swapchain images (can cause DEVICE_LOST / black screen).
            // Keep this permanently disabled; we inject only from vkQueuePresentKHR.
            (void)device; (void)swapchain; (void)imageIndex;
            static bool once = false;
            if (!once) { once = true; thanosray::log::Logf("[SL] Acquire injection DISABLED (TryShadowLiftFromAcquire early-return)"); }
            return;
        }


        
        static void TryShadowLiftFromAcquire(VkDevice device, VkSwapchainKHR swapchain, uint32_t imageIndex)
        {
            // Keep it fail-open and cheap
            if (ShadowLiftStrength() <= 0.0f) return;

            VkQueue q = VK_NULL_HANDLE;
            {
                std::lock_guard<std::mutex> lk(g_shadowMtx);
                auto dit = g_shadowDev.find(device);
                if (dit != g_shadowDev.end())
                    q = dit->second.presentQueue;
            }
            if (!q) return;

            ApplyShadowLiftToSwapchainImage(q, swapchain, imageIndex);
        }


        static VkResult VKAPI_CALL Hook_vkAcquireNextImageKHR(
            VkDevice device,
            VkSwapchainKHR swapchain,
            uint64_t timeout,
            VkSemaphore semaphore,
            VkFence fence,
            uint32_t* pImageIndex)
        {
            // Hotkey polling must run in a per-frame hook. AcquireNextImage is called by essentially all swapchain apps.
            PollHotkeys();

            if (!g_real_vkAcquireNextImageKHR)
                return VK_ERROR_INITIALIZATION_FAILED;

            VkResult r = g_real_vkAcquireNextImageKHR(device, swapchain, timeout, semaphore, fence, pImageIndex);

            if ((r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) && pImageIndex) {
                static bool once = false;
                if (!once) {
                    once = true;
                    thanosray::log::Logf("[SL] AcquireNextImageKHR hook active (export path)");
                }
                TryShadowLiftFromAcquire(device, swapchain, *pImageIndex);
            }

            return r;
        }

        static VkResult VKAPI_CALL Hook_vkAcquireNextImage2KHR(
            VkDevice device,
            const VkAcquireNextImageInfoKHR* pAcquireInfo,
            uint32_t* pImageIndex)
        {
            // Hotkey polling must run in a per-frame hook. AcquireNextImage2 is used by some engines.
            PollHotkeys();

            if (!g_real_vkAcquireNextImage2KHR)
                return VK_ERROR_INITIALIZATION_FAILED;

            VkResult r = g_real_vkAcquireNextImage2KHR(device, pAcquireInfo, pImageIndex);

            if ((r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) && pAcquireInfo && pImageIndex) {
                static bool once = false;
                if (!once) {
                    once = true;
                    thanosray::log::Logf("[SL] AcquireNextImage2KHR hook active (export path)");
                }
                TryShadowLiftFromAcquire(device, pAcquireInfo->swapchain, *pImageIndex);
            }

            return r;
        }
        */

        static bool ShadowLiftInitLocked(ShadowLiftDevice& d, VkDevice device)
        {
            if (d.initAttempted) return d.initOk;
            d.initAttempted = true;

            auto& st = State();
            d.vkGetDeviceProcAddr = st.real_vkGetDeviceProcAddr;
            if (!d.vkGetDeviceProcAddr) {
                static bool onceNoGDPA = false;
                if (!onceNoGDPA) { onceNoGDPA = true; thanosray::log::Logf("[SL] ERROR: real vkGetDeviceProcAddr is null; cannot init ShadowLift."); }
                return false;
            }

            auto load = [&](auto& fn, const char* name) {
              //  fn = (decltype(fn))d.vkGetDeviceProcAddr(device, name);
                using FnT = std::remove_reference_t<decltype(fn)>;
                fn = reinterpret_cast<FnT>(d.vkGetDeviceProcAddr(device, name));
                return fn != nullptr;
            };

            bool ok = true;
            ok &= load(d.vkCreateCommandPool, "vkCreateCommandPool");
            ok &= load(d.vkDestroyCommandPool, "vkDestroyCommandPool");
            ok &= load(d.vkAllocateCommandBuffers, "vkAllocateCommandBuffers");
            ok &= load(d.vkFreeCommandBuffers, "vkFreeCommandBuffers");
            ok &= load(d.vkBeginCommandBuffer, "vkBeginCommandBuffer");
            ok &= load(d.vkEndCommandBuffer, "vkEndCommandBuffer");
            ok &= load(d.vkResetCommandBuffer, "vkResetCommandBuffer");
            ok &= load(d.vkQueueSubmit, "vkQueueSubmit");
            ok &= load(d.vkWaitForFences, "vkWaitForFences");
            ok &= load(d.vkResetFences, "vkResetFences");
            ok &= load(d.vkCreateFence, "vkCreateFence");
            ok &= load(d.vkDestroyFence, "vkDestroyFence");
            ok &= load(d.vkCmdPipelineBarrier, "vkCmdPipelineBarrier");
            ok &= load(d.vkCmdBindPipeline, "vkCmdBindPipeline");
            ok &= load(d.vkCmdBindDescriptorSets, "vkCmdBindDescriptorSets");
            ok &= load(d.vkCmdPushConstants, "vkCmdPushConstants");
            ok &= load(d.vkCmdDispatch, "vkCmdDispatch");
            ok &= load(d.vkCreateShaderModule, "vkCreateShaderModule");
            ok &= load(d.vkDestroyShaderModule, "vkDestroyShaderModule");
            ok &= load(d.vkCreateDescriptorSetLayout, "vkCreateDescriptorSetLayout");
            ok &= load(d.vkDestroyDescriptorSetLayout, "vkDestroyDescriptorSetLayout");
            ok &= load(d.vkCreatePipelineLayout, "vkCreatePipelineLayout");
            ok &= load(d.vkDestroyPipelineLayout, "vkDestroyPipelineLayout");
            ok &= load(d.vkCreateComputePipelines, "vkCreateComputePipelines");
            ok &= load(d.vkDestroyPipeline, "vkDestroyPipeline");
            ok &= load(d.vkCreateDescriptorPool, "vkCreateDescriptorPool");
            ok &= load(d.vkDestroyDescriptorPool, "vkDestroyDescriptorPool");
            ok &= load(d.vkAllocateDescriptorSets, "vkAllocateDescriptorSets");
            ok &= load(d.vkUpdateDescriptorSets, "vkUpdateDescriptorSets");
            ok &= load(d.vkCreateImageView, "vkCreateImageView");
            ok &= load(d.vkDestroyImageView, "vkDestroyImageView");

            if (!ok || !d.queueFamilyValid) return false;

            // Create command pool + fence
            VkCommandPoolCreateInfo cp{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
            cp.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            cp.queueFamilyIndex = d.queueFamily;
            if (d.vkCreateCommandPool(device, &cp, nullptr, &d.cmdPool) != VK_SUCCESS) return false;

            VkFenceCreateInfo fi{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
            if (d.vkCreateFence(device, &fi, nullptr, &d.fence) != VK_SUCCESS) return false;

            // Load shader bytes
            std::vector<uint8_t> bytes;
            if (!ReadFileBytes("shadowlift.comp.spv", bytes) || (bytes.size() % 4) != 0) {
                static bool onceMissing = false;
                if (!onceMissing) {
                    onceMissing = true;
                    thanosray::log::Logf("[SL] ERROR: failed to read shadowlift.comp.spv from working directory (place it next to the game EXE / CWD).");
                }
                return false;
            }

            VkShaderModuleCreateInfo sm{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
            sm.codeSize = bytes.size();
            sm.pCode = (const uint32_t*)bytes.data();
            if (d.vkCreateShaderModule(device, &sm, nullptr, &d.shader) != VK_SUCCESS) return false;

            // Descriptor set layout: binding 0 = storage image
            VkDescriptorSetLayoutBinding b{};
            b.binding = 0;
            b.descriptorCount = 1;
            b.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            b.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

            VkDescriptorSetLayoutCreateInfo dsl{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            dsl.bindingCount = 1;
            dsl.pBindings = &b;
            if (d.vkCreateDescriptorSetLayout(device, &dsl, nullptr, &d.dsl) != VK_SUCCESS) return false;

            // Push constants: strength, toe, ceil
            VkPushConstantRange pcr{};
            pcr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            pcr.offset = 0;
            pcr.size = 16;

            VkPipelineLayoutCreateInfo pl{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
            pl.setLayoutCount = 1;
            pl.pSetLayouts = &d.dsl;
            pl.pushConstantRangeCount = 1;
            pl.pPushConstantRanges = &pcr;
            if (d.vkCreatePipelineLayout(device, &pl, nullptr, &d.ppl) != VK_SUCCESS) return false;

            VkPipelineShaderStageCreateInfo stage{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
            stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
            stage.module = d.shader;
            stage.pName = "main";

            VkComputePipelineCreateInfo ci{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
            ci.stage = stage;
            ci.layout = d.ppl;
            if (d.vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &ci, nullptr, &d.pipe) != VK_SUCCESS) return false;

            // Descriptor pool (small; we allocate per swapchain image as needed)
            VkDescriptorPoolSize ps{};
            ps.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            ps.descriptorCount = 64;

            VkDescriptorPoolCreateInfo dp{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
            dp.maxSets = 64;
            dp.poolSizeCount = 1;
            dp.pPoolSizes = &ps;
            if (d.vkCreateDescriptorPool(device, &dp, nullptr, &d.dsp) != VK_SUCCESS) return false;

            d.initOk = true;
            return true;
        }

        static bool EnsureSwapchainDescriptorsLocked(ShadowLiftDevice& d, SwapchainState& s)
        {
            if (!d.initOk) return false;
            if (s.images.empty()) return false;
            if (!s.views.empty() && s.views.size() == s.images.size() && s.sets.size() == s.images.size()) return true;

            // create views + descriptor sets
            s.views.assign(s.images.size(), VK_NULL_HANDLE);
            s.sets.assign(s.images.size(), VK_NULL_HANDLE);

            // allocate descriptor sets
            std::vector<VkDescriptorSetLayout> layouts(s.images.size(), d.dsl);
            VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            ai.descriptorPool = d.dsp;
            ai.descriptorSetCount = (uint32_t)layouts.size();
            ai.pSetLayouts = layouts.data();
            if (d.vkAllocateDescriptorSets(s.dev, &ai, s.sets.data()) != VK_SUCCESS) return false;

            for (size_t i = 0; i < s.images.size(); ++i) {
                VkImageViewCreateInfo iv{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
                iv.image = s.images[i];
                iv.viewType = VK_IMAGE_VIEW_TYPE_2D;
                iv.format = s.fmt;
                iv.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                iv.subresourceRange.levelCount = 1;
                iv.subresourceRange.layerCount = 1;
                if (d.vkCreateImageView(s.dev, &iv, nullptr, &s.views[i]) != VK_SUCCESS) return false;

                VkDescriptorImageInfo di{};
                di.imageView = s.views[i];
                di.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

                VkWriteDescriptorSet w{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
                w.dstSet = s.sets[i];
                w.dstBinding = 0;
                w.descriptorCount = 1;
                w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                w.pImageInfo = &di;
                d.vkUpdateDescriptorSets(s.dev, 1, &w, 0, nullptr);
            }
            return true;
        }

      

        
        // --------------------------------------------------------------------
        // LANDMARK 41: GDPA wrapper hooks (used only when returned from GDPA)
        // --------------------------------------------------------------------
        static VkResult VKAPI_CALL HookGDPA_vkCreateSwapchainKHR(
            VkDevice device,
            const VkSwapchainCreateInfoKHR* pCreateInfo,
            const VkAllocationCallbacks* pAllocator,
            VkSwapchainKHR* pSwapchain)
        {
            PFN_vkCreateSwapchainKHR real = g_gdpa_vkCreateSwapchainKHR ? g_gdpa_vkCreateSwapchainKHR : g_real_vkCreateSwapchainKHR;
            if (!real) return VK_ERROR_INITIALIZATION_FAILED;

            // Validate that we are really seeing a swapchain create info (guards against index mismatch).
            if (pCreateInfo && pCreateInfo->sType == VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR &&
                pCreateInfo->imageExtent.width && pCreateInfo->imageExtent.height)
            {
                static bool once = false;
                if (!once) {
                    once = true;
                    thanosray::log::Logf("[SL] (GDPA) swapchain fmt=%d extent=%ux%u images=%u usage=0x%08X",
                        (int)pCreateInfo->imageFormat,
                        pCreateInfo->imageExtent.width, pCreateInfo->imageExtent.height,
                        pCreateInfo->minImageCount, (uint32_t)pCreateInfo->imageUsage);
                }
            }
            else if (pCreateInfo) {
                static bool onceBad = false;
                if (!onceBad) {
                    onceBad = true;
                    thanosray::log::Logf("[SL] WARNING: (GDPA) swapchain create info invalid (sType=%u extent=%ux%u).",
                        (uint32_t)pCreateInfo->sType, pCreateInfo->imageExtent.width, pCreateInfo->imageExtent.height);
                }
            }

            VkResult r = real(device, pCreateInfo, pAllocator, pSwapchain);

            // Mirror existing swapchain tracking, but only if inputs look sane.
            if (r == VK_SUCCESS && pCreateInfo && pCreateInfo->sType == VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR &&
                pSwapchain && *pSwapchain && pCreateInfo->imageExtent.width && pCreateInfo->imageExtent.height)
            {
                std::lock_guard<std::mutex> lk(g_shadowMtx);
                SwapchainState& s = g_swap[*pSwapchain];
                s.dev = device;
                s.sc = *pSwapchain;
                s.fmt = pCreateInfo->imageFormat;
                s.extent = pCreateInfo->imageExtent;
                s.images.clear();
                s.views.clear();
                s.sets.clear();
            }
            return r;
        }

        static VkResult VKAPI_CALL HookGDPA_vkGetSwapchainImagesKHR(
            VkDevice device, VkSwapchainKHR swapchain, uint32_t* pSwapchainImageCount, VkImage* pSwapchainImages)
        {
            PFN_vkGetSwapchainImagesKHR real = g_gdpa_vkGetSwapchainImagesKHR ? g_gdpa_vkGetSwapchainImagesKHR : g_real_vkGetSwapchainImagesKHR;
            if (!real) return VK_ERROR_INITIALIZATION_FAILED;

            VkResult r = real(device, swapchain, pSwapchainImageCount, pSwapchainImages);

            if (r == VK_SUCCESS && pSwapchainImages && pSwapchainImageCount) {
                std::lock_guard<std::mutex> lk(g_shadowMtx);
                auto it = g_swap.find(swapchain);
                if (it != g_swap.end()) {
                    it->second.images.assign(pSwapchainImages, pSwapchainImages + (*pSwapchainImageCount));
                    it->second.views.clear();
                    it->second.sets.clear();
                }
            }
            return r;
        }
        static VkResult VKAPI_CALL HookGDPA_vkAcquireNextImageKHR(
            VkDevice device,
            VkSwapchainKHR swapchain,
            uint64_t timeout,
            VkSemaphore semaphore,
            VkFence fence,
            uint32_t* pImageIndex)
        {
            PFN_vkAcquireNextImageKHR real = g_gdpa_vkAcquireNextImageKHR ? g_gdpa_vkAcquireNextImageKHR : g_real_vkAcquireNextImageKHR;
            if (!real) return VK_ERROR_INITIALIZATION_FAILED;

            VkResult r = real(device, swapchain, timeout, semaphore, fence, pImageIndex);

            if ((r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) && pImageIndex) {
                static bool once = false;
                if (!once) {
                    once = true;
                    thanosray::log::Logf("[SL] AcquireNextImageKHR hook active (GDPA path)");
                }
                TryShadowLiftFromAcquire(device, swapchain, *pImageIndex);
            }

            return r;
        }

        static VkResult VKAPI_CALL HookGDPA_vkAcquireNextImage2KHR(
            VkDevice device,
            const VkAcquireNextImageInfoKHR* pAcquireInfo,
            uint32_t* pImageIndex)
        {
            PFN_vkAcquireNextImage2KHR real = g_gdpa_vkAcquireNextImage2KHR ? g_gdpa_vkAcquireNextImage2KHR : g_real_vkAcquireNextImage2KHR;
            if (!real) return VK_ERROR_INITIALIZATION_FAILED;

            VkResult r = real(device, pAcquireInfo, pImageIndex);

            if ((r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) && pAcquireInfo && pImageIndex) {
                static bool once = false;
                if (!once) {
                    once = true;
                    thanosray::log::Logf("[SL] AcquireNextImage2KHR hook active (GDPA path)");
                }
                TryShadowLiftFromAcquire(device, pAcquireInfo->swapchain, *pImageIndex);
            }



            return r;
        }




        // ============================================================================
// LANDMARK RF35: RT fallback - hook implementations (fail-open)
// ============================================================================

        static void VKAPI_CALL HookGDPA_vkDestroySwapchainKHR(
            VkDevice device,
            VkSwapchainKHR swapchain,
            const VkAllocationCallbacks* pAllocator)
        {
            // Mirror cleanup done by export-path destroy hook (fail-open).
            {
                std::lock_guard<std::mutex> lk(g_shadowMtx);
                auto it = g_swap.find(swapchain);
                if (it != g_swap.end()) {
                    // If you store views per swapchain, free them here if you have function pointers.
                    // Otherwise just erase the state.
                    g_swap.erase(it);
                }
            }

            PFN_vkDestroySwapchainKHR real =
                g_gdpa_vkDestroySwapchainKHR ? g_gdpa_vkDestroySwapchainKHR : g_real_vkDestroySwapchainKHR;

            if (real)
                real(device, swapchain, pAllocator);
        }


        static void VKAPI_CALL Hook_vkUpdateDescriptorSets(
            VkDevice device, uint32_t descriptorWriteCount,
            const VkWriteDescriptorSet* pDescriptorWrites,
            uint32_t descriptorCopyCount,
            const VkCopyDescriptorSet* pDescriptorCopies)
        {
            // Track writes before forwarding (safe either way)
            if (pDescriptorWrites && descriptorWriteCount) {
                thanosray::vk::rtfb::OnUpdateDescriptorSets(descriptorWriteCount, pDescriptorWrites);
                thanosray::vk::NoteUpdateDescriptorSets(descriptorWriteCount, pDescriptorWrites);

            }

            if (g_real_vkUpdateDescriptorSets) {
                g_real_vkUpdateDescriptorSets(device, descriptorWriteCount, pDescriptorWrites,
                    descriptorCopyCount, pDescriptorCopies);
                return;
            }

            static bool once = false;
            if (!once) { thanosray::log::Logf("[RTFB] ERROR: real vkUpdateDescriptorSets is null"); once = true; }
        }

        static void VKAPI_CALL Hook_vkUpdateDescriptorSetWithTemplate(
            VkDevice device, VkDescriptorSet descriptorSet,
            VkDescriptorUpdateTemplate descriptorUpdateTemplate,
            const void* pData)
        {
            if (g_real_vkUpdateDescriptorSetWithTemplate) {
                // NOTE: Template decoding is non-trivial; we’re not decoding in Phase 0.
                // Still forward safely.
                g_real_vkUpdateDescriptorSetWithTemplate(device, descriptorSet, descriptorUpdateTemplate, pData);
                return;
            }
            static bool once = false;
            if (!once) { thanosray::log::Logf("[RTFB] ERROR: real vkUpdateDescriptorSetWithTemplate is null"); once = true; }
        }

        static void VKAPI_CALL Hook_vkCmdBindDescriptorSets(
            VkCommandBuffer commandBuffer,
            VkPipelineBindPoint pipelineBindPoint,
            VkPipelineLayout layout,
            uint32_t firstSet,
            uint32_t descriptorSetCount,
            const VkDescriptorSet* pDescriptorSets,
            uint32_t dynamicOffsetCount,
            const uint32_t* pDynamicOffsets)
        {
            // Track RT bind state (OnCmdBindDescriptorSets filters to RT bind point internally)
            if (pDescriptorSets && descriptorSetCount) {
                thanosray::vk::rtfb::OnCmdBindDescriptorSets(
                    commandBuffer, pipelineBindPoint, layout, firstSet, descriptorSetCount, pDescriptorSets);
                thanosray::vk::NoteCmdBindDescriptorSets(commandBuffer, descriptorSetCount, pDescriptorSets);

            }

            if (g_real_vkCmdBindDescriptorSets) {
                g_real_vkCmdBindDescriptorSets(commandBuffer, pipelineBindPoint, layout, firstSet,
                    descriptorSetCount, pDescriptorSets,
                    dynamicOffsetCount, pDynamicOffsets);
                return;
            }

            static bool once = false;
            if (!once) { thanosray::log::Logf("[RTFB] ERROR: real vkCmdBindDescriptorSets is null"); once = true; }
        }

        static void VKAPI_CALL Hook_vkCmdBindPipeline(
            VkCommandBuffer commandBuffer,
            VkPipelineBindPoint pipelineBindPoint,
            VkPipeline pipeline)
        {
            thanosray::vk::rtfb::OnCmdBindPipeline(commandBuffer, pipelineBindPoint, pipeline);

            if (g_real_vkCmdBindPipeline) {
                g_real_vkCmdBindPipeline(commandBuffer, pipelineBindPoint, pipeline);
                return;
            }
            static bool once = false;
            if (!once) { thanosray::log::Logf("[RTFB] ERROR: real vkCmdBindPipeline is null"); once = true; }
        }

        /*

        static VkResult VKAPI_CALL Hook_vkCreateImage(
            VkDevice device,
            const VkImageCreateInfo* pCreateInfo,
            const VkAllocationCallbacks* pAllocator,
            VkImage* pImage)
        {
            if (!g_real_vkCreateImage) {
                static bool once = false;
                if (!once) { thanosray::log::Logf("[RTFB] ERROR: real vkCreateImage is null"); once = true; }
                return VK_ERROR_INITIALIZATION_FAILED;
            }

            VkResult r = g_real_vkCreateImage(device, pCreateInfo, pAllocator, pImage);
            if (r == VK_SUCCESS && pCreateInfo && pImage && *pImage) {
                thanosray::vk::rtfb::OnCreateImage(*pImage, *pCreateInfo);
            }
            return r;
        }

        */
        static VkResult VKAPI_CALL Hook_vkCreateImage(
            VkDevice device,
            const VkImageCreateInfo* pCreateInfo,
            const VkAllocationCallbacks* pAllocator,
            VkImage* pImage)
        {
            if (!g_real_vkCreateImage) return VK_ERROR_INITIALIZATION_FAILED;
            if (!pCreateInfo) return g_real_vkCreateImage(device, pCreateInfo, pAllocator, pImage);

            // Gate: only do this if fallback is enabled
            // Enable by setting environment variable: THANOS_FALLBACK_GI=1
            char buf[8] = {};
            DWORD n = GetEnvironmentVariableA("THANOS_FALLBACK_GI", buf, (DWORD)sizeof(buf));
            const bool fallbackOn = (n != 0) && (buf[0] == '1' || buf[0] == 'y' || buf[0] == 'Y' || buf[0] == 't' || buf[0] == 'T');

            // Copy create info so we can safely tweak usage flags
            VkImageCreateInfo ci = *pCreateInfo;

            // Heuristic: likely RT output targets are 2D color storage images.
            // If so, add TRANSFER_DST so our existing vkCmdClearColorImage fallback can write to it.
            if (fallbackOn) {
                const bool is2D = (ci.imageType & VK_IMAGE_TYPE_2D);
                const bool isColor =
                    (ci.format != VK_FORMAT_D16_UNORM) &&
                    (ci.format != VK_FORMAT_X8_D24_UNORM_PACK32) &&
                    (ci.format != VK_FORMAT_D32_SFLOAT) &&
                    (ci.format != VK_FORMAT_S8_UINT) &&
                    (ci.format != VK_FORMAT_D16_UNORM_S8_UINT) &&
                    (ci.format != VK_FORMAT_D24_UNORM_S8_UINT) &&
                    (ci.format != VK_FORMAT_D32_SFLOAT_S8_UINT);

                const bool isStorage = (ci.usage & VK_IMAGE_USAGE_STORAGE_BIT) != 0;
                const bool alreadyTransferDst = (ci.usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) != 0;

                if (is2D && isColor && isStorage && !alreadyTransferDst) {
                    ci.usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;

                    static bool once = false;
                    if (!once) {
                        once = true;
                        thanosray::log::Logf("[RTFB] Hook_vkCreateImage: adding TRANSFER_DST to STORAGE image(s) to enable safe clear fallback");
                    }
                }
            }

            VkResult r = g_real_vkCreateImage(device, &ci, pAllocator, pImage);
            if (pImage && *pImage) {
                thanosray::vk::NoteCreateImage(device, *pImage, *pCreateInfo);
            }

            // Track with the *actual* create info used (ci), not the original
            if (r == VK_SUCCESS && pImage && *pImage) {
                thanosray::vk::rtfb::OnCreateImage(device,*pImage, ci);
            }
            return r;
        }




        static void VKAPI_CALL Hook_vkDestroyImage(
            VkDevice device,
            VkImage image,
            const VkAllocationCallbacks* pAllocator)
        {
            if (image) thanosray::vk::rtfb::OnDestroyImage(image);

            if (g_real_vkDestroyImage) {
                g_real_vkDestroyImage(device, image, pAllocator);
                return;
            }
            static bool once = false;
            if (!once) { thanosray::log::Logf("[RTFB] ERROR: real vkDestroyImage is null"); once = true; }
        }

        static VkResult VKAPI_CALL Hook_vkCreateImageView(
            VkDevice device,
            const VkImageViewCreateInfo* pCreateInfo,
            const VkAllocationCallbacks* pAllocator,
            VkImageView* pView)
        {
            if (!g_real_vkCreateImageView) {
                static bool once = false;
                if (!once) { thanosray::log::Logf("[RTFB] ERROR: real vkCreateImageView is null"); once = true; }
                return VK_ERROR_INITIALIZATION_FAILED;
            }

            VkResult r = g_real_vkCreateImageView(device, pCreateInfo, pAllocator, pView);
            if (r == VK_SUCCESS && pCreateInfo && pView && *pView) {
                thanosray::vk::rtfb::OnCreateImageView(*pView, *pCreateInfo);
            }
          //  thanosray::vk::NoteCreateImageView(*pView, *pCreateInfo);
            thanosray::vk::NoteCreateImageView(*pView, *pCreateInfo);


            return r;
        }

        static void VKAPI_CALL Hook_vkDestroyImageView(
            VkDevice device,
            VkImageView view,
            const VkAllocationCallbacks* pAllocator)
        {
            if (view) thanosray::vk::rtfb::OnDestroyImageView(view);

            if (g_real_vkDestroyImageView) {
                g_real_vkDestroyImageView(device, view, pAllocator);
                return;
            }
            static bool once = false;
            if (!once) { thanosray::log::Logf("[RTFB] ERROR: real vkDestroyImageView is null"); once = true; }
        }



          //reSTORE IF PROBLEMS
        static VkResult VKAPI_CALL HookGDPA_vkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* pPresentInfo)
        {
            PollHotkeys();
            static std::atomic<bool> g_seenPresent{ false };
            g_seenPresent.store(true, std::memory_order_relaxed);

            static std::atomic<bool> inPresent{ false };
            if (inPresent.exchange(true)) {
                PFN_vkQueuePresentKHR real = g_real_vkQueuePresentKHR;
                return real ? real(queue, pPresentInfo) : VK_ERROR_INITIALIZATION_FAILED;
            }


            static bool once = false;
            if (!once) {
                once = true;
                uint32_t scCount = (pPresentInfo ? pPresentInfo->swapchainCount : 0);
                void* pSC = (pPresentInfo ? (void*)pPresentInfo->pSwapchains : nullptr);
                void* pIdx = (pPresentInfo ? (void*)pPresentInfo->pImageIndices : nullptr);
                thanosray::log::Logf("[PRESENT] HIT: pPresentInfo=%p swapchainCount=%u pSwapchains=%p pImageIndices=%p",
                    (void*)pPresentInfo, scCount, pSC, pIdx);
            }

           // }
            
            if (pPresentInfo && pPresentInfo->swapchainCount && pPresentInfo->pSwapchains && pPresentInfo->pImageIndices) {
                for (uint32_t i = 0; i < pPresentInfo->swapchainCount; ++i) {
                    if (g_runtimeLanternEnable.load(std::memory_order_relaxed)) {
                        ApplyShadowLiftToSwapchainImage(queue, pPresentInfo, pPresentInfo->pSwapchains[i], pPresentInfo->pImageIndices[i]);
                    }
                }
            }
            
            PFN_vkQueuePresentKHR real = g_gdpa_vkQueuePresentKHR ? g_gdpa_vkQueuePresentKHR : g_real_vkQueuePresentKHR;

            inPresent.store(false);
            return real ? real(queue, pPresentInfo) : VK_ERROR_INITIALIZATION_FAILED;
        }
        /*
        
static VkResult VKAPI_CALL Hook_vkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* pPresentInfo)
{
    PFN_vkQueuePresentKHR real = g_real_vkQueuePresentKHR;
    if (!real) return VK_ERROR_INITIALIZATION_FAILED;

    // One-time confirmation.
    static bool once = false;
    if (!once) {
        once = true;
        thanosray::log::Logf("[PRESENT] Hook_vkQueuePresentKHR HIT");
    }

    // Provide present wait semaphores to ShadowLift submit to avoid racing the renderer.
    tl_presentWaitCount = (pPresentInfo ? pPresentInfo->waitSemaphoreCount : 0);
    tl_presentWaitSems  = (pPresentInfo ? pPresentInfo->pWaitSemaphores : nullptr);

    float strength = ShadowLiftStrength();
    if (strength > 0.0f &&
        pPresentInfo &&
        pPresentInfo->swapchainCount &&
        pPresentInfo->pSwapchains &&
        pPresentInfo->pImageIndices)
    {
        // If the app provided no wait semaphores, skip ShadowLift for safety.
        if (tl_presentWaitCount == 0 || !tl_presentWaitSems) {
            static bool onceNoWait = false;
            if (!onceNoWait) {
                onceNoWait = true;
                thanosray::log::Logf("[SL] Present has no wait semaphores; skipping ShadowLift to avoid DEVICE_LOST.");
            }
        } else {
            for (uint32_t i = 0; i < pPresentInfo->swapchainCount; ++i) {
                ApplyShadowLiftToSwapchainImage(queue, pPresentInfo->pSwapchains[i], pPresentInfo->pImageIndices[i]);
            }
        }
    }

    // Clear TLS to avoid accidental reuse.
    tl_presentWaitCount = 0;
    tl_presentWaitSems  = nullptr;

    return real(queue, pPresentInfo);
}*/

        static VkResult VKAPI_CALL Hook_vkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* pPresentInfo)
        {
            if (!g_real_vkQueuePresentKHR) {
                thanosray::log::Logf("[SL] ERROR: g_real_vkQueuePresentKHR is null");
                return VK_ERROR_INITIALIZATION_FAILED;
            }

            thanosray::log::LogOncef("present-hook-alive",
                "[SL] Hook_vkQueuePresentKHR alive: queue=%p waitCount=%u swapchainCount=%u",
                (void*)queue,
                pPresentInfo ? pPresentInfo->waitSemaphoreCount : 0,
                pPresentInfo ? pPresentInfo->swapchainCount : 0);

            return g_real_vkQueuePresentKHR(queue, pPresentInfo);
        }


        /*  reenable if theres an issue

static VkResult VKAPI_CALL Hook_vkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* pPresentInfo)
{
    PFN_vkQueuePresentKHR real = g_real_vkQueuePresentKHR;
    if (!real) return VK_ERROR_INITIALIZATION_FAILED;

    // one-time proof we are actually in present
    static std::atomic<int> s_hit{ 0 };
    if (s_hit.fetch_add(1) == 0) {
        thanosray::log::Logf("[SL][PRESENT] Hook_vkQueuePresentKHR is ACTIVE (first hit). queue=%p", (void*)queue);
    }

    if (pPresentInfo && pPresentInfo->swapchainCount && pPresentInfo->pSwapchains && pPresentInfo->pImageIndices) {
        for (uint32_t i = 0; i < pPresentInfo->swapchainCount; ++i) {
            ApplyShadowLiftToSwapchainImage(queue, pPresentInfo,
                pPresentInfo->pSwapchains[i],
                pPresentInfo->pImageIndices[i]);
        }
    }

    return real(queue, pPresentInfo);
}
*/

// ============================================================================
// LANDMARK SL-PRESENT: Apply ShadowLift right before present (NOT on acquire)
// This avoids the game overwriting our changes.
// ============================================================================
        /*
        static VkResult VKAPI_CALL Hook_vkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* pPresentInfo)
        {
            PFN_vkQueuePresentKHR real = g_real_vkQueuePresentKHR;
            if (!real) return VK_ERROR_INITIALIZATION_FAILED;

            // Log every present for now (later you can rate-limit)
            if (pPresentInfo) {
                thanosray::log::Logf("[PRESENT] swapchainCount=%u pSwapchains=%p pImageIndices=%p",
                    pPresentInfo->swapchainCount,
                    (void*)pPresentInfo->pSwapchains,
                    (void*)pPresentInfo->pImageIndices);
            }
            else {
                thanosray::log::Logf("[PRESENT] pPresentInfo=null");
            }

            // Apply ShadowLift to each swapchain image that will be presented.
            // IMPORTANT: must happen BEFORE calling the real present.
            if (pPresentInfo && pPresentInfo->swapchainCount && pPresentInfo->pSwapchains && pPresentInfo->pImageIndices) {
                for (uint32_t i = 0; i < pPresentInfo->swapchainCount; ++i) {
                    if (g_runtimeLanternEnable.load(std::memory_order_relaxed)) {
                        ApplyShadowLiftToSwapchainImage(queue, pPresentInfo, pPresentInfo->pSwapchains[i], pPresentInfo->pImageIndices[i]);
                    }
                }
            }

            return real(queue, pPresentInfo);
        }

        */



        void InstallHooks()
        {
            const char* const* names = ExportNameTable();
            for (int i = 0; i < kProcTableSize; ++i)
                g_procTable[i] = (void*)runtime::Wrap(names[i], diagnostics::Wrap(names[i], (PFN_vkVoidFunction)g_procTable[i]));
            static bool once = false;
            if (!once) {
                once = true;
                thanosray::log::Logf("[VKHOOKS-BUILD] %s", __FILE__);
            }
            g_procTable[IDX_vkGetDeviceProcAddr] = (void*)&Hook_vkGetDeviceProcAddr;
            // Queue capture (needed for shadowlift command pool)
            g_real_vkGetDeviceQueue  = (PFN_vkGetDeviceQueue) g_procTable[IDX_vkGetDeviceQueue];
            g_real_vkGetDeviceQueue2 = (PFN_vkGetDeviceQueue2)g_procTable[IDX_vkGetDeviceQueue2];
            g_procTable[IDX_vkGetDeviceQueue]  = (void*)&Hook_vkGetDeviceQueue;
            g_procTable[IDX_vkGetDeviceQueue2] = (void*)&Hook_vkGetDeviceQueue2;

            // Swapchain capture + present hook (shadowlift injection point)
            g_real_vkCreateSwapchainKHR    = (PFN_vkCreateSwapchainKHR)   g_procTable[IDX_vkCreateSwapchainKHR];
            g_real_vkDestroySwapchainKHR   = (PFN_vkDestroySwapchainKHR)  g_procTable[IDX_vkDestroySwapchainKHR];
            g_real_vkGetSwapchainImagesKHR = (PFN_vkGetSwapchainImagesKHR)g_procTable[IDX_vkGetSwapchainImagesKHR];
            g_real_vkQueuePresentKHR       = (PFN_vkQueuePresentKHR)      g_procTable[IDX_vkQueuePresentKHR];
            g_real_vkAcquireNextImageKHR = (PFN_vkAcquireNextImageKHR)g_procTable[IDX_vkAcquireNextImageKHR];
            g_real_vkAcquireNextImage2KHR = (PFN_vkAcquireNextImage2KHR)g_procTable[IDX_vkAcquireNextImage2KHR];


            g_procTable[IDX_vkCreateSwapchainKHR]    = (void*)&Hook_vkCreateSwapchainKHR;
            g_procTable[IDX_vkDestroySwapchainKHR]   = (void*)&Hook_vkDestroySwapchainKHR;
            g_procTable[IDX_vkGetSwapchainImagesKHR] = (void*)&Hook_vkGetSwapchainImagesKHR;
            g_procTable[IDX_vkQueuePresentKHR]       = (void*)&Hook_vkQueuePresentKHR;
            thanosray::log::Logf("Installed hook: vkGetDeviceProcAddr");

            g_procTable[IDX_vkGetInstanceProcAddr] = (void*)&Hook_vkGetInstanceProcAddr;
            thanosray::log::Logf("Installed hook: vkGetInstanceProcAddr");

            g_procTable[IDX_vkEnumerateDeviceExtensionProperties] = (void*)&Hook_vkEnumerateDeviceExtensionProperties;
            thanosray::log::Logf("Installed hook: vkEnumerateDeviceExtensionProperties");

            g_procTable[IDX_vkGetPhysicalDeviceFeatures2] = (void*)&Hook_vkGetPhysicalDeviceFeatures2;
            thanosray::log::Logf("Installed hook: vkGetPhysicalDeviceFeatures2");

            g_procTable[IDX_vkGetPhysicalDeviceProperties2] = (void*)&Hook_vkGetPhysicalDeviceProperties2;
            thanosray::log::Logf("Installed hook: vkGetPhysicalDeviceProperties2");

            g_procTable[IDX_vkCreateDevice] = (void*)&Hook_vkCreateDevice;
            thanosray::log::Logf("Installed hook: vkCreateDevice");
            g_procTable[IDX_vkAcquireNextImageKHR] = (void*)&Hook_vkAcquireNextImageKHR;
            g_procTable[IDX_vkAcquireNextImage2KHR] = (void*)&Hook_vkAcquireNextImage2KHR;



            //reinenable if issue
           // g_procTable[IDX_vkQueuePresentKHR] = (void*)&Hook_vkQueuePresentKHR;

            // ONLY do this once, at install time (not every GDPA call)
            if (!g_real_vkQueuePresentKHR) {
                auto candidate = (PFN_vkQueuePresentKHR)g_procTable[IDX_vkQueuePresentKHR];
                // don’t allow “real” to become our hook
                if (candidate && candidate != &Hook_vkQueuePresentKHR)
                    g_real_vkQueuePresentKHR = candidate;
            }

            if (g_procTable[IDX_vkQueuePresentKHR] != (void*)&Hook_vkQueuePresentKHR) {
                g_procTable[IDX_vkQueuePresentKHR] = (void*)&Hook_vkQueuePresentKHR;
                thanosray::log::Logf("[SL] Installed export hook: vkQueuePresentKHR real=%p hook=%p",
                    (void*)g_real_vkQueuePresentKHR, (void*)&Hook_vkQueuePresentKHR);
            }


            // LANDMARK RF50: also hook core entrypoints in export table (fail-open)
            
            /*
            g_real_vkUpdateDescriptorSets = (PFN_vkUpdateDescriptorSets)g_procTable[IDX_vkUpdateDescriptorSets];
            g_procTable[IDX_vkUpdateDescriptorSets] = (void*)&Hook_vkUpdateDescriptorSets;

            g_real_vkUpdateDescriptorSetWithTemplate = (PFN_vkUpdateDescriptorSetWithTemplate)g_procTable[IDX_vkUpdateDescriptorSetWithTemplate];
            g_procTable[IDX_vkUpdateDescriptorSetWithTemplate] = (void*)&Hook_vkUpdateDescriptorSetWithTemplate;

            g_real_vkCmdBindDescriptorSets = (PFN_vkCmdBindDescriptorSets)g_procTable[IDX_vkCmdBindDescriptorSets];
            g_procTable[IDX_vkCmdBindDescriptorSets] = (void*)&Hook_vkCmdBindDescriptorSets;

            g_real_vkCmdBindPipeline = (PFN_vkCmdBindPipeline)g_procTable[IDX_vkCmdBindPipeline];
            g_procTable[IDX_vkCmdBindPipeline] = (void*)&Hook_vkCmdBindPipeline;

            g_real_vkCreateImage = (PFN_vkCreateImage)g_procTable[IDX_vkCreateImage];
            g_procTable[IDX_vkCreateImage] = (void*)&Hook_vkCreateImage;

            g_real_vkDestroyImage = (PFN_vkDestroyImage)g_procTable[IDX_vkDestroyImage];
            g_procTable[IDX_vkDestroyImage] = (void*)&Hook_vkDestroyImage;

            g_real_vkCreateImageView = (PFN_vkCreateImageView)g_procTable[IDX_vkCreateImageView];
            g_procTable[IDX_vkCreateImageView] = (void*)&Hook_vkCreateImageView;

            g_real_vkDestroyImageView = (PFN_vkDestroyImageView)g_procTable[IDX_vkDestroyImageView];
            g_procTable[IDX_vkDestroyImageView] = (void*)&Hook_vkDestroyImageView;
            
            thanosray::log::Logf("[RTFB] Installed export-table tracking hooks");
            */

            // ============================================================================
// LANDMARK RF50: Install RT-fallback tracking hooks
// - Captures real entrypoints from g_procTable and replaces them with wrappers.
// - Fail-open: if an index is wrong / pointer is null, we log and keep going.
// ============================================================================

            auto CaptureAndHook = [&](int idx, void* hook, void** outReal, const char* name)
                {
                    void* real = g_procTable[idx];
                    if (!real) {
                        thanosray::log::Logf("[RTFB] WARN: %s real is null at idx=%d", name, idx);
                    }
                    if (outReal) *outReal = real;
                    g_procTable[idx] = hook;
                    thanosray::log::Logf("[RTFB] Installed hook: %s (idx=%d real=%p hook=%p)", name, idx, real, hook);
                };

            // Descriptor tracking
            CaptureAndHook(IDX_vkUpdateDescriptorSets,
                (void*)&Hook_vkUpdateDescriptorSets,
                (void**)&g_real_vkUpdateDescriptorSets,
                "vkUpdateDescriptorSets");

            CaptureAndHook(IDX_vkUpdateDescriptorSetWithTemplate,
                (void*)&Hook_vkUpdateDescriptorSetWithTemplate,
                (void**)&g_real_vkUpdateDescriptorSetWithTemplate,
                "vkUpdateDescriptorSetWithTemplate");

            CaptureAndHook(IDX_vkCmdBindDescriptorSets,
                (void*)&Hook_vkCmdBindDescriptorSets,
                (void**)&g_real_vkCmdBindDescriptorSets,
                "vkCmdBindDescriptorSets");

            CaptureAndHook(IDX_vkCmdBindPipeline,
                (void*)&Hook_vkCmdBindPipeline,
                (void**)&g_real_vkCmdBindPipeline,
                "vkCmdBindPipeline");

            // Image tracking
            CaptureAndHook(IDX_vkCreateImage,
                (void*)&Hook_vkCreateImage,
                (void**)&g_real_vkCreateImage,
                "vkCreateImage");

            CaptureAndHook(IDX_vkDestroyImage,
                (void*)&Hook_vkDestroyImage,
                (void**)&g_real_vkDestroyImage,
                "vkDestroyImage");

            CaptureAndHook(IDX_vkCreateImageView,
                (void*)&Hook_vkCreateImageView,
                (void**)&g_real_vkCreateImageView,
                "vkCreateImageView");

            CaptureAndHook(IDX_vkDestroyImageView,
                (void*)&Hook_vkDestroyImageView,
                (void**)&g_real_vkDestroyImageView,
                "vkDestroyImageView");

            // Optional Phase 0b helpers: capture these so RtFallback can do a safe clear/barrier.
            g_real_vkCmdPipelineBarrier = (PFN_vkCmdPipelineBarrier)g_procTable[IDX_vkCmdPipelineBarrier];
            g_real_vkCmdClearColorImage = (PFN_vkCmdClearColorImage)g_procTable[IDX_vkCmdClearColorImage];

            thanosray::vk::rtfb::RealCmdFns fns{};
            fns.vkCmdPipelineBarrier = g_real_vkCmdPipelineBarrier;
            fns.vkCmdClearColorImage = g_real_vkCmdClearColorImage;
            thanosray::vk::rtfb::SetRealCmdFns(fns);

            thanosray::log::Logf("[RTFB] RealCmdFns set: PipelineBarrier=%p ClearColorImage=%p",
                (void*)g_real_vkCmdPipelineBarrier, (void*)g_real_vkCmdClearColorImage);

        }



        // --------------------------------------------------------------------
// LANDMARK SL-ACQ: ShadowLift injection moved to AcquireNextImage
// Rationale: Some engines call vkQueuePresentKHR with swapchainCount==0.
// AcquireNextImage always identifies a concrete swapchain + image index.
// Fail-open: we always return the real result; ShadowLift is best-effort.
// --------------------------------------------------------------------
        static void TryShadowLiftFromAcquire(VkDevice device, VkSwapchainKHR swapchain, uint32_t imageIndex)
        {
            // Image acquisition signals a semaphore asynchronously. Touching the
            // image here without waiting on it races the renderer/presentation.
            // Any explicitly enabled postprocess belongs in the present path.
            (void)device; (void)swapchain; (void)imageIndex;
        }

        static VkResult VKAPI_CALL Hook_vkAcquireNextImageKHR(
            VkDevice device,
            VkSwapchainKHR swapchain,
            uint64_t timeout,
            VkSemaphore semaphore,
            VkFence fence,
            uint32_t* pImageIndex)
        {
            // Hotkey polling must run in a per-frame hook. AcquireNextImage is called reliably.
            PollHotkeys();

            if (!g_real_vkAcquireNextImageKHR)
                return VK_ERROR_INITIALIZATION_FAILED;

            VkResult r = g_real_vkAcquireNextImageKHR(device, swapchain, timeout, semaphore, fence, pImageIndex);



            if ((r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) && pImageIndex) {
                static bool once = false;
                if (!once) {
                    once = true;
                    thanosray::log::Logf("[SL] AcquireNextImageKHR hook active (export path)");
                }
                TryShadowLiftFromAcquire(device, swapchain, *pImageIndex);
            }

            return r;
        }

        static VkResult VKAPI_CALL Hook_vkAcquireNextImage2KHR(
            VkDevice device,
            const VkAcquireNextImageInfoKHR* pAcquireInfo,
            uint32_t* pImageIndex)
        {
            // Hotkey polling must run in a per-frame hook. AcquireNextImage2 is used by some engines.
            PollHotkeys();

            if (!g_real_vkAcquireNextImage2KHR)
                return VK_ERROR_INITIALIZATION_FAILED;

            VkResult r = g_real_vkAcquireNextImage2KHR(device, pAcquireInfo, pImageIndex);

            if ((r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) && pAcquireInfo && pImageIndex) {
                static bool once = false;
                if (!once) {
                    once = true;
                    thanosray::log::Logf("[SL] AcquireNextImage2KHR hook active (export path)");
                }
                TryShadowLiftFromAcquire(device, pAcquireInfo->swapchain, *pImageIndex);
            }

            return r;
        }


    




            static PFN_vkCmdPipelineBarrier GetCmdPipelineBarrier(VkDevice dev)
            {
                if (!dev) return nullptr;
                auto fp = (PFN_vkCmdPipelineBarrier)Hook_vkGetDeviceProcAddr(dev, "vkCmdPipelineBarrier");
                return fp;
            }

            static PFN_vkCmdClearColorImage GetCmdClearColorImage(VkDevice dev)
            {
                if (!dev) return nullptr;
                auto fp = (PFN_vkCmdClearColorImage)Hook_vkGetDeviceProcAddr(dev, "vkCmdClearColorImage");
                return fp;
            }

            struct ImgInfo
            {
                VkDevice device = VK_NULL_HANDLE;
                VkImageUsageFlags usage = 0;
                VkFormat format = VK_FORMAT_UNDEFINED;
                VkExtent3D extent = { 0,0,0 };
            };

            static std::mutex g_mtx;

            // Image + view bookkeeping (we can populate these from existing hooks)
            static std::unordered_map<VkImage, ImgInfo> g_imgInfo;          // image -> info
            static std::unordered_map<VkImageView, VkImage> g_viewToImage;  // view -> image

            // Descriptor tracking: descriptorSet -> list of storage image views used as outputs
            static std::unordered_map<VkDescriptorSet, std::vector<VkImageView>> g_setStorageViews;

            // Bind tracking: commandBuffer -> bound sets (we only need "latest snapshot")
            static std::unordered_map<VkCommandBuffer, std::vector<VkDescriptorSet>> g_cbBoundSets;

            static void NoteCreateImage(VkDevice dev, VkImage img, const VkImageCreateInfo& ci)
            {
                if (!img) return;
                std::lock_guard<std::mutex> lk(g_mtx);
                ImgInfo& ii = g_imgInfo[img];
                ii.device = dev;
                ii.usage = ci.usage;
                ii.format = ci.format;
                ii.extent = ci.extent;
            }

            static void NoteCreateImageView(VkImageView view, const VkImageViewCreateInfo& vi)
            {
                if (!view) return;
                std::lock_guard<std::mutex> lk(g_mtx);
                g_viewToImage[view] = vi.image;
            }

            static void NoteUpdateDescriptorSets(uint32_t writeCount, const VkWriteDescriptorSet* writes)
            {
                if (!writes || !writeCount) return;

                std::lock_guard<std::mutex> lk(g_mtx);

                for (uint32_t i = 0; i < writeCount; i++)
                {
                    const VkWriteDescriptorSet& w = writes[i];
                    if (!w.dstSet) continue;

                    // We're looking specifically for STORAGE_IMAGE outputs
                    if (w.descriptorType != VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) continue;
                    if (!w.pImageInfo || w.descriptorCount == 0) continue;

                    auto& vec = g_setStorageViews[w.dstSet];
                    // Keep it simple: append; we’ll de-dupe later during use
                    for (uint32_t j = 0; j < w.descriptorCount; j++)
                    {
                        VkImageView v = w.pImageInfo[j].imageView;
                        if (v) vec.push_back(v);
                    }
                }
            }

            static void NoteCmdBindDescriptorSets(
                VkCommandBuffer cb,
                uint32_t descriptorSetCount,
                const VkDescriptorSet* pSets)
            {
                if (!cb) return;
                std::lock_guard<std::mutex> lk(g_mtx);

                std::vector<VkDescriptorSet> snap;
                snap.reserve(descriptorSetCount);
                for (uint32_t i = 0; i < descriptorSetCount; i++)
                    if (pSets[i]) snap.push_back(pSets[i]);

                g_cbBoundSets[cb] = std::move(snap);
            }

            static void DebugLiftAfterTraceRays(VkCommandBuffer cb)
            {
                // Uses vkCmdClearColorImage as a *proof-of-hit*.
                // This requires images to have TRANSFER_DST usage. Many do; if not, we just skip.
                if (!cb) return;
                // We need a device to query device-level command functions.
                VkDevice dev = VK_NULL_HANDLE;
                {
                    std::lock_guard<std::mutex> lk(g_mtx);
                    // Find any image used by any view to get a device (cheap heuristic)
                    // Better: store cb->device when command buffers are allocated; but this works for now.
                    if (!g_imgInfo.empty()) dev = g_imgInfo.begin()->second.device;
                }
                if (!dev) return;

                PFN_vkCmdPipelineBarrier fpBarrier = GetCmdPipelineBarrier(dev);
                PFN_vkCmdClearColorImage fpClear = GetCmdClearColorImage(dev);
                if (!fpBarrier || !fpClear) return;


                std::vector<VkDescriptorSet> sets;
                {
                    std::lock_guard<std::mutex> lk(g_mtx);
                    auto it = g_cbBoundSets.find(cb);
                    if (it != g_cbBoundSets.end()) sets = it->second;
                }
                if (sets.empty()) return;

                // Build a unique list of candidate storage image views used by the bound sets
                std::vector<VkImageView> views;
                {
                    std::lock_guard<std::mutex> lk(g_mtx);
                    for (VkDescriptorSet s : sets)
                    {
                        auto sit = g_setStorageViews.find(s);
                        if (sit == g_setStorageViews.end()) continue;
                        for (VkImageView v : sit->second) if (v) views.push_back(v);
                    }
                }
                if (views.empty()) return;

                // De-dupe views (avoid blasting same target repeatedly)
                std::sort(views.begin(), views.end());
                views.erase(std::unique(views.begin(), views.end()), views.end());

                static uint32_t s_once = 0;
                if (s_once < 3) {
                    thanosray::log::Logf("[RT-OUT] DebugLiftAfterTraceRays: cb=%p views=%zu", (void*)cb, views.size());
                    s_once++;
                }

                for (VkImageView v : views)
                {
                    VkImage img = VK_NULL_HANDLE;
                    ImgInfo info{};
                    {
                        std::lock_guard<std::mutex> lk(g_mtx);
                        auto vit = g_viewToImage.find(v);
                        if (vit == g_viewToImage.end()) continue;
                        img = vit->second;

                        auto iit = g_imgInfo.find(img);
                        if (iit == g_imgInfo.end()) continue;
                        info = iit->second;
                    }

                    // We can only clear with vkCmdClearColorImage if image supports transfer dst.
                    if ((info.usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) == 0) continue;

                    // Barrier to GENERAL so we can clear safely.
                    VkImageMemoryBarrier b{};
                    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                    b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
                    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                    b.oldLayout = VK_IMAGE_LAYOUT_GENERAL;   // most RT outputs are GENERAL during writes
                    b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
                    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    b.image = img;
                    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                    b.subresourceRange.baseMipLevel = 0;
                    b.subresourceRange.levelCount = 1;
                    b.subresourceRange.baseArrayLayer = 0;
                    b.subresourceRange.layerCount = 1;

                    g_real_vkCmdPipelineBarrier(
                        cb,
                        VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                        VK_PIPELINE_STAGE_TRANSFER_BIT,
                        0,
                        0, nullptr,
                        0, nullptr,
                        1, &b);

                    // “Lift” as an ambient floor proof (dark scenes should visibly brighten).
                    // This is NOT the final algorithm; it is a “did we hit the right RT output?” step.
                    VkClearColorValue c{};
                    c.float32[0] = 0.18f; // ~18% gray
                    c.float32[1] = 0.18f;
                    c.float32[2] = 0.18f;
                    c.float32[3] = 1.00f;

                    VkImageSubresourceRange rr{};
                    rr.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                    rr.baseMipLevel = 0;
                    rr.levelCount = 1;
                    rr.baseArrayLayer = 0;
                    rr.layerCount = 1;

                    g_real_vkCmdClearColorImage(cb, img, VK_IMAGE_LAYOUT_GENERAL, &c, 1, &rr);

                    // Barrier back for shader reads in later composite passes.
                    VkImageMemoryBarrier b2 = b;
                    b2.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                    b2.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;

                    g_real_vkCmdPipelineBarrier(
                        cb,
                        VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                        0,
                        0, nullptr,
                        0, nullptr,
                        1, &b2);
                }
            }
  



    } // namespace vk


} // namespace thanosray
