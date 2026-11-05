#pragma once
#include <vulkan/vulkan.h>
#include <windows.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_set>
#include "../log/Log.h"

namespace thanosray::vk::diagnostics {
inline PFN_vkCreateShaderModule realShader = nullptr;
inline PFN_vkCreateComputePipelines realCompute = nullptr;
inline PFN_vkCreateGraphicsPipelines realGraphics = nullptr;

inline std::filesystem::path ConfigPath()
{
    HMODULE module = nullptr;
    wchar_t path[32768]{};
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&ConfigPath), &module);
    GetModuleFileNameW(module, path, 32768);
    return std::filesystem::path(path).parent_path() / L"Thanos-wrapper.ini";
}

inline bool PascalGateExperiment()
{
    static const bool enabled = [] {
        char value[8]{};
        if (GetEnvironmentVariableA("THANOS_PASCAL_RT_GATE", value, sizeof(value))) return value[0] == '1';
        return GetPrivateProfileIntW(L"experiments", L"PascalRTGate", 0, ConfigPath().c_str()) != 0;
    }();
    return enabled;
}

inline uint64_t InspectShader(const VkShaderModuleCreateInfo* info)
{
    if (!info || !info->pCode || info->codeSize % 4 || info->codeSize < 20) return 0;
    uint64_t hash = 14695981039346656037ull;
    const auto* bytes = reinterpret_cast<const unsigned char*>(info->pCode);
    for (size_t i = 0; i < info->codeSize; ++i) hash = (hash ^ bytes[i]) * 1099511628211ull;
    std::string caps, stages;
    bool float16 = false, valid = info->pCode[0] == 0x07230203;
    for (size_t offset = 5; valid && offset < info->codeSize / 4;) {
        const uint32_t header = info->pCode[offset];
        const uint32_t length = header >> 16, opcode = header & 0xffff;
        if (!length || length > info->codeSize / 4 - offset) { valid = false; break; }
        if (opcode == 17 && length == 2) caps += std::to_string(info->pCode[offset + 1]) + ",";
        if (opcode == 15 && length >= 4) stages += std::to_string(info->pCode[offset + 1]) + ",";
        if (opcode == 22 && length == 3 && info->pCode[offset + 2] == 16) float16 = true;
        offset += length;
    }
    log::Logf("[SHADER] hash=%016llx bytes=%zu valid=%d float16Type=%d stages=%s capabilities=%s",
        (unsigned long long)hash, info->codeSize, valid, float16, stages.c_str(), caps.c_str());
    static std::mutex mutex;
    static std::unordered_set<uint64_t> saved;
    static size_t savedBytes = 0;
    std::lock_guard<std::mutex> lock(mutex);
    wchar_t directory[32768]{};
    DWORD len = GetEnvironmentVariableW(L"THANOS_SHADER_DUMP_DIR", directory, 32768);
    if (!len) len = GetPrivateProfileStringW(L"diagnostics", L"ShaderDumpDirectory", L"", directory, 32768, ConfigPath().c_str());
    if (!len || len >= 32768 || saved.count(hash) || info->codeSize > 256 * 1024 * 1024 - savedBytes) return hash;
    char filename[40]{};
    std::snprintf(filename, sizeof(filename), "%016llx.spv", (unsigned long long)hash);
    std::ofstream output(std::filesystem::path(directory) / filename, std::ios::binary);
    if (output.write(reinterpret_cast<const char*>(info->pCode), info->codeSize)) {
        saved.insert(hash);
        savedBytes += info->codeSize;
    }
    return hash;
}

inline VkResult VKAPI_CALL CreateShader(VkDevice device, const VkShaderModuleCreateInfo* info,
    const VkAllocationCallbacks* allocator, VkShaderModule* module)
{
    const uint64_t hash = InspectShader(info);
    const VkResult result = realShader(device, info, allocator, module);
    log::Logf("[SHADER] hash=%016llx result=%d module=%p", (unsigned long long)hash,
        result, result == VK_SUCCESS && module ? (void*)*module : nullptr);
    return result;
}

inline VkResult VKAPI_CALL CreateCompute(VkDevice device, VkPipelineCache cache, uint32_t count,
    const VkComputePipelineCreateInfo* infos, const VkAllocationCallbacks* allocator, VkPipeline* pipelines)
{
    for (uint32_t i = 0; i < count; ++i)
        log::Logf("[PIPELINE] compute[%u] module=%p entry=%s", i, (void*)infos[i].stage.module, infos[i].stage.pName);
    const VkResult result = realCompute(device, cache, count, infos, allocator, pipelines);
    log::Logf("[PIPELINE] compute count=%u result=%d", count, result);
    if (result == VK_SUCCESS) for (uint32_t i=0; i<count; ++i)
        log::Logf("[PIPELINE-MAP] compute pipeline=%p module=%p entry=%s",
            (void*)pipelines[i], (void*)infos[i].stage.module, infos[i].stage.pName);
    return result;
}

inline VkResult VKAPI_CALL CreateGraphics(VkDevice device, VkPipelineCache cache, uint32_t count,
    const VkGraphicsPipelineCreateInfo* infos, const VkAllocationCallbacks* allocator, VkPipeline* pipelines)
{
    for (uint32_t i = 0; i < count; ++i)
        for (uint32_t j = 0; j < infos[i].stageCount; ++j)
            log::Logf("[PIPELINE] graphics[%u] stage=%x module=%p entry=%s", i,
                infos[i].pStages[j].stage, (void*)infos[i].pStages[j].module, infos[i].pStages[j].pName);
    const VkResult result = realGraphics(device, cache, count, infos, allocator, pipelines);
    log::Logf("[PIPELINE] graphics count=%u result=%d", count, result);
    if (result == VK_SUCCESS) for (uint32_t i=0; i<count; ++i)
        for (uint32_t j=0; j<infos[i].stageCount; ++j)
            log::Logf("[PIPELINE-MAP] graphics pipeline=%p stage=%x module=%p entry=%s",
                (void*)pipelines[i], infos[i].pStages[j].stage,
                (void*)infos[i].pStages[j].module, infos[i].pStages[j].pName);
    return result;
}

inline PFN_vkVoidFunction Wrap(const char* name, PFN_vkVoidFunction real)
{
    if (!name || !real) return real;
    if (std::strcmp(name, "vkCreateShaderModule") == 0) {
        if (!realShader) realShader = reinterpret_cast<PFN_vkCreateShaderModule>(real);
        return reinterpret_cast<PFN_vkVoidFunction>(&CreateShader);
    }
    if (std::strcmp(name, "vkCreateComputePipelines") == 0) {
        if (!realCompute) realCompute = reinterpret_cast<PFN_vkCreateComputePipelines>(real);
        return reinterpret_cast<PFN_vkVoidFunction>(&CreateCompute);
    }
    if (std::strcmp(name, "vkCreateGraphicsPipelines") == 0) {
        if (!realGraphics) realGraphics = reinterpret_cast<PFN_vkCreateGraphicsPipelines>(real);
        return reinterpret_cast<PFN_vkVoidFunction>(&CreateGraphics);
    }
    return real;
}
}
