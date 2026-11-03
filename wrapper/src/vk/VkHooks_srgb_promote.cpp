//#include "pch.h"
#include "VkHooks.h"
#include "VkProxyExports.h"
#include "VkProxyState.h"

#include "../log/Log.h"

#include <vector>
#include <string>
#include <unordered_set>
#include <algorithm>
#include <cstring>
#include <cstdint>
#include <cstdlib>

namespace thanosray {
    namespace vk {

        // Export indices (0-based) for the System32 vulkan-1.dll export order that the project was generated from.
        static constexpr int IDX_vkEnumerateDeviceExtensionProperties = 160;
        static constexpr int IDX_vkGetDeviceProcAddr = 185;
        static constexpr int IDX_vkShowKeys = 0; // unused placeholder, keep build stable
        static constexpr int IDX_vkGetInstanceProcAddr = 201;

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

            uint32_t capacity = (pProperties && pPropertyCount) ? *pPropertyCount : 0;

            VkResult r = st.real_vkEnumerateDeviceExtensionProperties(
                physicalDevice, pLayerName, pPropertyCount, pProperties);

            if (!pPropertyCount) return r;

            // Count query
            if (!pProperties) {
                *pPropertyCount += kSpoofExtCount;
                thanosray::log::Logf("[EXT] Count query: +%u spoof extensions (baseline+RT)", kSpoofExtCount);
                return r;
            }

            uint32_t realCount = *pPropertyCount;

            bool missing[kSpoofExtCount] = {};
            uint32_t missingCount = 0;
            for (uint32_t i = 0; i < kSpoofExtCount; i++) {
                if (!HasExtension(pProperties, realCount, kSpoofExts[i])) {
                    missing[i] = true;
                    missingCount++;
                }
            }

            if (missingCount == 0) return r;

            uint32_t required = realCount + missingCount;
            if (capacity < required) {
                *pPropertyCount = required;
                thanosray::log::Logf("[EXT] Not enough space: need %u, had %u -> VK_INCOMPLETE", required, capacity);
                return VK_INCOMPLETE;
            }

            uint32_t outIndex = realCount;
            for (uint32_t i = 0; i < kSpoofExtCount; i++) {
                if (missing[i]) {
                    WriteExtension(pProperties[outIndex], kSpoofExts[i], 1);
                    outIndex++;
                }
            }

            thanosray::log::Logf("[EXT] Injected %u spoof extensions into enumerate output", missingCount);
            *pPropertyCount = outIndex;
            return r;
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

        static void VKAPI_CALL Hook_vkCmdTraceRaysKHR(
            VkCommandBuffer,
            const VkStridedDeviceAddressRegionKHR*,
            const VkStridedDeviceAddressRegionKHR*,
            const VkStridedDeviceAddressRegionKHR*,
            const VkStridedDeviceAddressRegionKHR*,
            uint32_t,
            uint32_t,
            uint32_t)
        {
            static bool logged = false;
            if (!logged) {
                thanosray::log::Logf("[RT] vkCmdTraceRaysKHR intercepted (NO-OP)");
                logged = true;
            }
        }

        static PFN_vkVoidFunction VKAPI_CALL Hook_vkGetDeviceProcAddr(VkDevice device, const char* pName)
        {
            auto& st = State();
            PFN_vkVoidFunction fn = nullptr;
            if (st.real_vkGetDeviceProcAddr) {
                fn = st.real_vkGetDeviceProcAddr(device, pName);
            }

            if (pName) {
                if (std::strcmp(pName, "vkCmdTraceRaysKHR") == 0) {
                    thanosray::log::Logf("[GDPA] providing stub: %s", pName);
                    return (PFN_vkVoidFunction)&Hook_vkCmdTraceRaysKHR;
                }
            }

            return fn;
        }

        static PFN_vkVoidFunction VKAPI_CALL Hook_vkGetInstanceProcAddr(VkInstance instance, const char* pName)
        {
            auto& st = State();
            PFN_vkVoidFunction fn = nullptr;
            if (st.real_vkGetInstanceProcAddr) {
                fn = st.real_vkGetInstanceProcAddr(instance, pName);
            }

            if (!pName) return fn;

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

            return fn;
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
        static T* AppendCopy(std::vector<uint8_t>& blob, const T* src)
        {
            size_t off = blob.size();
            blob.resize(off + sizeof(T));
            T* dst = reinterpret_cast<T*>(blob.data() + off);
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
            std::vector<uint8_t>& storage)
        {
            storage.clear();
            storage.reserve(1024);

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
            auto& st = State();

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

            thanosray::log::Logf("[vkCreateDevice] passing(driver)=%u", (uint32_t)driverExts.size());
            for (size_t i = 0; i < driverExts.size(); ++i) {
                thanosray::log::Logf("  pass[%zu]=%s", i, driverExts[i]);
            }

            DumpInChain(pCreateInfo->pNext, "[vkCreateDevice] original");

            std::vector<uint8_t> pnextStorage;
            const void* sanitizedPNext = BuildSanitizedDeviceCreatePNext(
                physicalDevice, pCreateInfo->pNext, pnextStorage);

            DumpInChain(sanitizedPNext, "[vkCreateDevice] sanitized");

            VkDeviceCreateInfo patched = *pCreateInfo;
            patched.enabledExtensionCount = (uint32_t)driverExts.size();
            patched.ppEnabledExtensionNames = driverExts.empty() ? nullptr : driverExts.data();
            patched.pNext = sanitizedPNext;

            thanosray::log::Logf("[vkCreateDevice] calling real vkCreateDevice...");
            VkResult r = st.real_vkCreateDevice(physicalDevice, &patched, pAllocator, pDevice);
            thanosray::log::Logf("[vkCreateDevice] real returned: %d", (int)r);
            return r;
        }

        
        // --------------------------------------------------------------------
        // Swapchain hooks (export-level, forward-only)
        //
        // NOTE: This project uses MASM thunks that jump through g_procTable by
        // export index, so hooking via vkGetDeviceProcAddr is not sufficient
        // to observe swapchain/present on many titles.
        //
        // These hooks are LOGGING + FORWARDING ONLY (no behavior change).
        // --------------------------------------------------------------------

        // Indices are from exports_cpp.inc (X(name, index))
        static constexpr int IDX_vkAcquireNextImage2KHR   = 0;
        static constexpr int IDX_vkAcquireNextImageKHR    = 1;
        static constexpr int IDX_vkCreateSwapchainKHR     = 131;
        static constexpr int IDX_vkDestroySwapchainKHR    = 157;
        static constexpr int IDX_vkGetSwapchainImagesKHR  = 238;
        static constexpr int IDX_vkQueuePresentKHR        = 244;

        static PFN_vkAcquireNextImage2KHR   g_real_vkAcquireNextImage2KHR  = nullptr;

        static bool SwapchainSrgbEnabled()
        {
            static int cached = -1;
            if (cached != -1) return cached != 0;

            char* buf = nullptr;
            size_t len = 0;
            errno_t err = _dupenv_s(&buf, &len, "THR_SWAPCHAIN_SRGB");
            const char* v = (err == 0) ? buf : nullptr;

            cached = (v && v[0] && v[0] != '0') ? 1 : 0;
            if (cached) {
                thanosray::log::Logf("[SWAP] sRGB promotion ENABLED (THR_SWAPCHAIN_SRGB=1)");
            }
            if (buf) free(buf);
            return cached != 0;
        }

        static VkFormat PromoteToSrgbIfPossible(VkFormat fmt)
        {
            switch (fmt) {
            case VK_FORMAT_B8G8R8A8_UNORM: return VK_FORMAT_B8G8R8A8_SRGB;
            case VK_FORMAT_R8G8B8A8_UNORM: return VK_FORMAT_R8G8B8A8_SRGB;
            default: return fmt;
            }
        }
        static PFN_vkAcquireNextImageKHR    g_real_vkAcquireNextImageKHR   = nullptr;
        static PFN_vkCreateSwapchainKHR     g_real_vkCreateSwapchainKHR    = nullptr;
        static PFN_vkDestroySwapchainKHR    g_real_vkDestroySwapchainKHR   = nullptr;
        static PFN_vkGetSwapchainImagesKHR  g_real_vkGetSwapchainImagesKHR = nullptr;
        static PFN_vkQueuePresentKHR        g_real_vkQueuePresentKHR       = nullptr;

        static const char* VkFormatName_Short(VkFormat f)
        {
            switch (f) {
            case VK_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
            case VK_FORMAT_B8G8R8A8_SRGB:  return "B8G8R8A8_SRGB";
            case VK_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
            case VK_FORMAT_R8G8B8A8_SRGB:  return "R8G8B8A8_SRGB";
            default: return "other";
            }
        }

        static VkResult VKAPI_CALL Hook_vkCreateSwapchainKHR(
            VkDevice device,
            const VkSwapchainCreateInfoKHR* pCreateInfo,
            const VkAllocationCallbacks* pAllocator,
            VkSwapchainKHR* pSwapchain)
        {
            if (!g_real_vkCreateSwapchainKHR) {
                thanosray::log::Logf("[SWAP] ERROR: real vkCreateSwapchainKHR is null");
                return VK_ERROR_INITIALIZATION_FAILED;
            }

            if (pCreateInfo) {
                thanosray::log::Logf("[SWAP] vkCreateSwapchainKHR fmt=%s(%d) extent=%ux%u images=%u usage=0x%08X",
                    VkFormatName_Short(pCreateInfo->imageFormat), (int)pCreateInfo->imageFormat,
                    pCreateInfo->imageExtent.width, pCreateInfo->imageExtent.height,
                    pCreateInfo->minImageCount, (uint32_t)pCreateInfo->imageUsage);
            }

            // Optional sRGB promotion: some engines assume the swapchain is sRGB but request UNORM.
            // This can make the whole image appear too dark on some paths. Controlled by THR_SWAPCHAIN_SRGB=1.
            if (pCreateInfo && SwapchainSrgbEnabled()) {
                VkSwapchainCreateInfoKHR ci = *pCreateInfo;
                VkFormat promoted = PromoteToSrgbIfPossible(ci.imageFormat);
                if (promoted != ci.imageFormat) {
                    ci.imageFormat = promoted;
                    thanosray::log::Logf("[SWAP] trying promote fmt -> %s(%d)", VkFormatName_Short(ci.imageFormat), (int)ci.imageFormat);
                    VkResult r = g_real_vkCreateSwapchainKHR(device, &ci, pAllocator, pSwapchain);
                    if (r == VK_SUCCESS) return r;

                    // Fallback: if driver rejects SRGB, retry the original request.
                    thanosray::log::Logf("[SWAP] promote failed r=%d, retrying original fmt=%s(%d)",
                        (int)r, VkFormatName_Short(pCreateInfo->imageFormat), (int)pCreateInfo->imageFormat);
                    return g_real_vkCreateSwapchainKHR(device, pCreateInfo, pAllocator, pSwapchain);
                }
            }

            return g_real_vkCreateSwapchainKHR(device, pCreateInfo, pAllocator, pSwapchain);
        }

        static void VKAPI_CALL Hook_vkDestroySwapchainKHR(
            VkDevice device,
            VkSwapchainKHR swapchain,
            const VkAllocationCallbacks* pAllocator)
        {
            if (!g_real_vkDestroySwapchainKHR) return;
            g_real_vkDestroySwapchainKHR(device, swapchain, pAllocator);
        }

        static VkResult VKAPI_CALL Hook_vkGetSwapchainImagesKHR(
            VkDevice device,
            VkSwapchainKHR swapchain,
            uint32_t* pSwapchainImageCount,
            VkImage* pSwapchainImages)
        {
            if (!g_real_vkGetSwapchainImagesKHR) {
                thanosray::log::Logf("[SWAP] ERROR: real vkGetSwapchainImagesKHR is null");
                return VK_ERROR_INITIALIZATION_FAILED;
            }

            VkResult r = g_real_vkGetSwapchainImagesKHR(device, swapchain, pSwapchainImageCount, pSwapchainImages);

            // Log the "count query" call pattern
            if (pSwapchainImageCount && !pSwapchainImages) {
                thanosray::log::Logf("[SWAP] vkGetSwapchainImagesKHR count=%u (r=%d)", *pSwapchainImageCount, (int)r);
            }

            return r;
        }

        static VkResult VKAPI_CALL Hook_vkQueuePresentKHR(
            VkQueue queue,
            const VkPresentInfoKHR* pPresentInfo)
        {
            if (!g_real_vkQueuePresentKHR) {
                thanosray::log::Logf("[SWAP] ERROR: real vkQueuePresentKHR is null");
                return VK_ERROR_INITIALIZATION_FAILED;
            }

            VkResult r = g_real_vkQueuePresentKHR(queue, pPresentInfo);

            // Only log failures to keep logs light.
            if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
                thanosray::log::Logf("[SWAP] vkQueuePresentKHR r=%d", (int)r);
            }

            return r;
        }

        static VkResult VKAPI_CALL Hook_vkAcquireNextImageKHR(
            VkDevice device,
            VkSwapchainKHR swapchain,
            uint64_t timeout,
            VkSemaphore semaphore,
            VkFence fence,
            uint32_t* pImageIndex)
        {
            if (!g_real_vkAcquireNextImageKHR) {
                thanosray::log::Logf("[SWAP] ERROR: real vkAcquireNextImageKHR is null");
                return VK_ERROR_INITIALIZATION_FAILED;
            }
            return g_real_vkAcquireNextImageKHR(device, swapchain, timeout, semaphore, fence, pImageIndex);
        }

        static VkResult VKAPI_CALL Hook_vkAcquireNextImage2KHR(
            VkDevice device,
            const VkAcquireNextImageInfoKHR* pAcquireInfo,
            uint32_t* pImageIndex)
        {
            if (!g_real_vkAcquireNextImage2KHR) {
                thanosray::log::Logf("[SWAP] ERROR: real vkAcquireNextImage2KHR is null");
                return VK_ERROR_INITIALIZATION_FAILED;
            }
            return g_real_vkAcquireNextImage2KHR(device, pAcquireInfo, pImageIndex);
        }

void InstallHooks()
        {
            g_procTable[IDX_vkGetDeviceProcAddr] = (void*)&Hook_vkGetDeviceProcAddr;
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

            // Swapchain hooks (export-level). Forward-only (logging + pass-through).
            // Capture real pointers BEFORE replacing g_procTable entries.
            g_real_vkAcquireNextImage2KHR  = (PFN_vkAcquireNextImage2KHR)g_procTable[IDX_vkAcquireNextImage2KHR];
            g_procTable[IDX_vkAcquireNextImage2KHR] = (void*)&Hook_vkAcquireNextImage2KHR;
            thanosray::log::Logf("Installed hook: vkAcquireNextImage2KHR");

            g_real_vkAcquireNextImageKHR   = (PFN_vkAcquireNextImageKHR)g_procTable[IDX_vkAcquireNextImageKHR];
            g_procTable[IDX_vkAcquireNextImageKHR] = (void*)&Hook_vkAcquireNextImageKHR;
            thanosray::log::Logf("Installed hook: vkAcquireNextImageKHR");

            g_real_vkCreateSwapchainKHR    = (PFN_vkCreateSwapchainKHR)g_procTable[IDX_vkCreateSwapchainKHR];
            g_procTable[IDX_vkCreateSwapchainKHR] = (void*)&Hook_vkCreateSwapchainKHR;
            thanosray::log::Logf("Installed hook: vkCreateSwapchainKHR");

            g_real_vkDestroySwapchainKHR   = (PFN_vkDestroySwapchainKHR)g_procTable[IDX_vkDestroySwapchainKHR];
            g_procTable[IDX_vkDestroySwapchainKHR] = (void*)&Hook_vkDestroySwapchainKHR;
            thanosray::log::Logf("Installed hook: vkDestroySwapchainKHR");

            g_real_vkGetSwapchainImagesKHR = (PFN_vkGetSwapchainImagesKHR)g_procTable[IDX_vkGetSwapchainImagesKHR];
            g_procTable[IDX_vkGetSwapchainImagesKHR] = (void*)&Hook_vkGetSwapchainImagesKHR;
            thanosray::log::Logf("Installed hook: vkGetSwapchainImagesKHR");

            g_real_vkQueuePresentKHR       = (PFN_vkQueuePresentKHR)g_procTable[IDX_vkQueuePresentKHR];
            g_procTable[IDX_vkQueuePresentKHR] = (void*)&Hook_vkQueuePresentKHR;
            thanosray::log::Logf("Installed hook: vkQueuePresentKHR");

        }

    } // namespace vk
} // namespace thanosray
