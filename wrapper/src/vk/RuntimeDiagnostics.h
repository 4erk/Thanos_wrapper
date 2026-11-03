#pragma once
#include "ShaderDiagnostics.h"
#include <utility>
#include <unordered_map>
#include <algorithm>

// Observe native results without changing return values, query data, waits or
// queue ordering. In particular, never fabricate success after DEVICE_LOST.
namespace thanosray::vk::runtime {
inline bool Enabled()
{
    static const bool enabled = GetPrivateProfileIntW(L"diagnostics", L"RuntimeTrace", 0,
        diagnostics::ConfigPath().c_str()) != 0;
    return enabled;
}

inline std::atomic<uint64_t> submits{0}, presents{0}, queries{0};
inline std::atomic<ULONGLONG> nextHeartbeat{0}, nextError{0};
struct FaultState { PFN_vkGetDeviceFaultInfoEXT query{}; bool reported{}; };
inline std::mutex faultMutex;
inline std::unordered_map<VkDevice, FaultState> faultDevices;

inline void OnDeviceCreated(VkDevice device, PFN_vkGetDeviceProcAddr resolve, bool faultEnabled)
{
    if (!Enabled() || !faultEnabled || !resolve) return;
    auto query = reinterpret_cast<PFN_vkGetDeviceFaultInfoEXT>(resolve(device, "vkGetDeviceFaultInfoEXT"));
    std::lock_guard<std::mutex> lock(faultMutex);
    faultDevices[device] = {query, false};
    log::Logf("[GPU] native device fault reporting enabled=%d device=%p", query != nullptr, (void*)device);
}

inline void ReportFault(VkDevice device)
{
    PFN_vkGetDeviceFaultInfoEXT query = nullptr;
    {
        std::lock_guard<std::mutex> lock(faultMutex);
        auto it = faultDevices.find(device);
        if (it == faultDevices.end() || it->second.reported || !it->second.query) return;
        it->second.reported = true;
        query = it->second.query;
    }
    VkDeviceFaultCountsEXT counts{VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT};
    const auto countResult = query(device, &counts, nullptr);
    log::Logf("[GPU-FAULT] device=%p countResult=%d addresses=%u vendors=%u binaryBytes=%llu",
        (void*)device, countResult, counts.addressInfoCount, counts.vendorInfoCount, counts.vendorBinarySize);
    if (countResult != VK_SUCCESS) return;
    VkDeviceFaultAddressInfoEXT addresses[64]{};
    VkDeviceFaultVendorInfoEXT vendors[64]{};
    counts.addressInfoCount = std::min(counts.addressInfoCount, 64u);
    counts.vendorInfoCount = std::min(counts.vendorInfoCount, 64u);
    counts.vendorBinarySize = 0;
    VkDeviceFaultInfoEXT info{VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT};
    info.pAddressInfos = addresses;
    info.pVendorInfos = vendors;
    const auto result = query(device, &counts, &info);
    log::Logf("[GPU-FAULT] result=%d description=%.*s", result, VK_MAX_DESCRIPTION_SIZE, info.description);
    if (result != VK_SUCCESS && result != VK_INCOMPLETE) return;
    for (uint32_t i = 0; i < std::min(counts.addressInfoCount, 64u); ++i)
        log::Logf("[GPU-FAULT] address[%u] type=%d address=0x%llx precision=%llu", i,
            addresses[i].addressType, addresses[i].reportedAddress, addresses[i].addressPrecision);
    for (uint32_t i = 0; i < std::min(counts.vendorInfoCount, 64u); ++i)
        log::Logf("[GPU-FAULT] vendor[%u] description=%.*s code=0x%llx data=0x%llx", i,
            VK_MAX_DESCRIPTION_SIZE, vendors[i].description, vendors[i].vendorFaultCode, vendors[i].vendorFaultData);
}

template<class First, class... Rest> inline void CheckFault(VkResult, First, Rest...) {}
template<class... Rest> inline void CheckFault(VkResult result, VkDevice device, Rest...)
{ if (result == VK_ERROR_DEVICE_LOST) ReportFault(device); }

inline bool Due(std::atomic<ULONGLONG>& next, ULONGLONG now, ULONGLONG interval)
{
    auto expected = next.load(std::memory_order_relaxed);
    return now >= expected && next.compare_exchange_strong(expected, now + interval,
        std::memory_order_relaxed);
}

inline VkResult Observe(const char* name, VkResult result, ULONGLONG started)
{
    if (!Enabled()) return result;
    const auto now = GetTickCount64();
    if (result < 0 && Due(nextError, now, 1000))
        log::Logf("[GPU-ERROR] tick=%llu thread=%lu call=%s result=%d elapsedMs=%llu%s",
            now, GetCurrentThreadId(), name, result, now - started,
            result == VK_ERROR_DEVICE_LOST ? " DEVICE_LOST" : "");
    if (Due(nextHeartbeat, now, 15000))
        log::Logf("[GPU] tick=%llu thread=%lu submits=%llu presents=%llu queries=%llu call=%s result=%d elapsedMs=%llu",
            now, GetCurrentThreadId(), submits.load(std::memory_order_relaxed),
            presents.load(std::memory_order_relaxed), queries.load(std::memory_order_relaxed),
            name, result, now - started);
    return result;
}

template<class Function, class... Args>
inline VkResult Call(const char* name, Function real, Args... args)
{
    if (!Enabled()) return real(args...);
    const auto started = GetTickCount64();
    const auto result = Observe(name, real(args...), started);
    CheckFault(result, args...);
    return result;
}

inline PFN_vkQueueSubmit realSubmit = nullptr;
inline PFN_vkQueuePresentKHR realPresent = nullptr;
inline PFN_vkQueueSubmit2 realSubmit2 = nullptr;
inline PFN_vkQueueSubmit2KHR realSubmit2KHR = nullptr;
inline PFN_vkWaitForFences realWaitFences = nullptr;
inline PFN_vkWaitSemaphores realWaitSemaphores = nullptr;
inline PFN_vkWaitSemaphoresKHR realWaitSemaphoresKHR = nullptr;
inline PFN_vkGetFenceStatus realFenceStatus = nullptr;
inline PFN_vkGetQueryPoolResults realQueryResults = nullptr;
inline PFN_vkQueueWaitIdle realQueueIdle = nullptr;
inline PFN_vkDeviceWaitIdle realDeviceIdle = nullptr;
inline PFN_vkAllocateMemory realAllocateMemory = nullptr;
inline PFN_vkDestroyDevice realDestroyDevice = nullptr;

inline void VKAPI_CALL DestroyDevice(VkDevice device, const VkAllocationCallbacks* allocator)
{
    if (Enabled()) {
        std::lock_guard<std::mutex> lock(faultMutex);
        faultDevices.erase(device);
    }
    realDestroyDevice(device, allocator);
}

inline VkResult VKAPI_CALL QueueSubmit(VkQueue q, uint32_t n, const VkSubmitInfo* info, VkFence fence)
{
    if (Enabled()) submits.fetch_add(1, std::memory_order_relaxed);
    return Call("vkQueueSubmit", realSubmit, q, n, info, fence);
}
inline VkResult VKAPI_CALL QueuePresent(VkQueue q, const VkPresentInfoKHR* info)
{
    if (Enabled()) presents.fetch_add(1, std::memory_order_relaxed);
    return Call("vkQueuePresentKHR", realPresent, q, info);
}
inline VkResult VKAPI_CALL QueueSubmit2(VkQueue q, uint32_t n, const VkSubmitInfo2* info, VkFence fence)
{
    if (Enabled()) submits.fetch_add(1, std::memory_order_relaxed);
    return Call("vkQueueSubmit2", realSubmit2, q, n, info, fence);
}
inline VkResult VKAPI_CALL QueueSubmit2KHR(VkQueue q, uint32_t n, const VkSubmitInfo2* info, VkFence fence)
{
    if (Enabled()) submits.fetch_add(1, std::memory_order_relaxed);
    return Call("vkQueueSubmit2KHR", realSubmit2KHR, q, n, info, fence);
}
inline VkResult VKAPI_CALL WaitForFences(VkDevice d, uint32_t n, const VkFence* fences, VkBool32 all, uint64_t timeout)
{ return Call("vkWaitForFences", realWaitFences, d, n, fences, all, timeout); }
inline VkResult VKAPI_CALL WaitSemaphores(VkDevice d, const VkSemaphoreWaitInfo* info, uint64_t timeout)
{ return Call("vkWaitSemaphores", realWaitSemaphores, d, info, timeout); }
inline VkResult VKAPI_CALL WaitSemaphoresKHR(VkDevice d, const VkSemaphoreWaitInfo* info, uint64_t timeout)
{ return Call("vkWaitSemaphoresKHR", realWaitSemaphoresKHR, d, info, timeout); }
inline VkResult VKAPI_CALL GetFenceStatus(VkDevice d, VkFence fence)
{ return Call("vkGetFenceStatus", realFenceStatus, d, fence); }
inline VkResult VKAPI_CALL QueueWaitIdle(VkQueue q)
{ return Call("vkQueueWaitIdle", realQueueIdle, q); }
inline VkResult VKAPI_CALL DeviceWaitIdle(VkDevice d)
{ return Call("vkDeviceWaitIdle", realDeviceIdle, d); }
inline VkResult VKAPI_CALL AllocateMemory(VkDevice d, const VkMemoryAllocateInfo* info,
    const VkAllocationCallbacks* alloc, VkDeviceMemory* memory)
{ return Call("vkAllocateMemory", realAllocateMemory, d, info, alloc, memory); }

inline VkResult VKAPI_CALL GetQueryPoolResults(VkDevice d, VkQueryPool pool, uint32_t first,
    uint32_t count, size_t size, void* data, VkDeviceSize stride, VkQueryResultFlags flags)
{
    if (!Enabled()) return realQueryResults(d, pool, first, count, size, data, stride, flags);
    queries.fetch_add(1, std::memory_order_relaxed);
    const auto started = GetTickCount64();
    const auto result = realQueryResults(d, pool, first, count, size, data, stride, flags);
    struct Poll { VkDevice device{}; VkQueryPool pool{}; uint32_t first{}, count{};
        VkResult result{VK_SUCCESS}; ULONGLONG since{}, next{}; uint64_t attempts{}; };
    thread_local Poll poll;
    if (result == VK_NOT_READY || result < 0) {
        if (poll.device != d || poll.pool != pool || poll.first != first || poll.count != count || poll.result != result)
            poll = {d, pool, first, count, result, started, started + 5000, 0};
        ++poll.attempts;
        const auto now = GetTickCount64();
        if (now >= poll.next) {
            log::Logf("[GPU-QUERY-STALL] tick=%llu thread=%lu pool=%p first=%u count=%u flags=0x%x result=%d durationMs=%llu attempts=%llu",
                now, GetCurrentThreadId(), (void*)pool, first, count, flags, result, now - poll.since, poll.attempts);
            poll.next = now + 5000;
        }
    } else {
        poll = {};
    }
    Observe("vkGetQueryPoolResults", result, started);
    if (result == VK_ERROR_DEVICE_LOST) ReportFault(d);
    return result;
}

inline PFN_vkVoidFunction Wrap(const char* name, PFN_vkVoidFunction real)
{
    if (!name || !real) return real;
#define RUNTIME_WRAP(api, hook, target) \
    if (std::strcmp(name, #api) == 0) { \
        if (!target) target = reinterpret_cast<PFN_##api>(real); \
        return reinterpret_cast<PFN_vkVoidFunction>(&hook); \
    }
    RUNTIME_WRAP(vkQueueSubmit, QueueSubmit, realSubmit)
    RUNTIME_WRAP(vkQueuePresentKHR, QueuePresent, realPresent)
    RUNTIME_WRAP(vkQueueSubmit2, QueueSubmit2, realSubmit2)
    RUNTIME_WRAP(vkQueueSubmit2KHR, QueueSubmit2KHR, realSubmit2KHR)
    RUNTIME_WRAP(vkWaitForFences, WaitForFences, realWaitFences)
    RUNTIME_WRAP(vkWaitSemaphores, WaitSemaphores, realWaitSemaphores)
    RUNTIME_WRAP(vkWaitSemaphoresKHR, WaitSemaphoresKHR, realWaitSemaphoresKHR)
    RUNTIME_WRAP(vkGetFenceStatus, GetFenceStatus, realFenceStatus)
    RUNTIME_WRAP(vkGetQueryPoolResults, GetQueryPoolResults, realQueryResults)
    RUNTIME_WRAP(vkQueueWaitIdle, QueueWaitIdle, realQueueIdle)
    RUNTIME_WRAP(vkDeviceWaitIdle, DeviceWaitIdle, realDeviceIdle)
    RUNTIME_WRAP(vkAllocateMemory, AllocateMemory, realAllocateMemory)
    RUNTIME_WRAP(vkDestroyDevice, DestroyDevice, realDestroyDevice)
#undef RUNTIME_WRAP
    return real;
}
}
