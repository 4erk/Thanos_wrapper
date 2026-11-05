#pragma once
#include "ShaderDiagnostics.h"
#include "IndirectBounds.h"
#include <array>
#include <unordered_map>
#include <vector>
#include <algorithm>

namespace thanosray::vk::checkpoints {
inline bool Requested()
{
    static const bool enabled = GetPrivateProfileIntW(L"diagnostics", L"GpuCheckpoints", 0,
        diagnostics::ConfigPath().c_str()) != 0;
    return enabled;
}
struct Device {
    PFN_vkCmdSetCheckpointNV set{};
    PFN_vkGetQueueCheckpointDataNV get{};
    std::vector<VkQueue> queues;
    bool reported{};
};
struct Command {
    VkDevice device{};
    VkCommandPool pool{};
    PFN_vkCmdSetCheckpointNV set{};
    VkPipeline graphics{}, compute{}, rays{};
};
struct Marker {
    uint64_t id{};
    VkCommandBuffer command{};
    VkPipeline pipeline{};
    const char* operation{};
    bool after{};
    bool hasDraw{};
    indirect::Draw draw{};
};
inline std::mutex mutex;
inline std::unordered_map<VkDevice, Device> devices;
inline std::unordered_map<VkQueue, VkDevice> queues;
inline std::unordered_map<VkCommandBuffer, Command> commands;
inline constexpr size_t capacity = 65536;
inline std::array<Marker, capacity> markers{};
inline uint64_t serial{};

inline void OnDevice(VkDevice device, PFN_vkGetDeviceProcAddr resolve, const VkDeviceCreateInfo& info, bool enabled)
{
    if (indirect::Track()) log::Logf("[INDIRECT-BOUNDS] device=%p guard=%d buffer-tracking=1",(void*)device,indirect::Enabled());
    if (!enabled) return;
    Device entry;
    entry.set = reinterpret_cast<PFN_vkCmdSetCheckpointNV>(resolve(device, "vkCmdSetCheckpointNV"));
    entry.get = reinterpret_cast<PFN_vkGetQueueCheckpointDataNV>(resolve(device, "vkGetQueueCheckpointDataNV"));
    if (!entry.set || !entry.get) return;
    auto get = reinterpret_cast<PFN_vkGetDeviceQueue>(resolve(device, "vkGetDeviceQueue"));
    auto get2 = reinterpret_cast<PFN_vkGetDeviceQueue2>(resolve(device, "vkGetDeviceQueue2"));
    for (uint32_t i=0; i<info.queueCreateInfoCount; ++i) {
        const auto& ci = info.pQueueCreateInfos[i];
        for (uint32_t j=0; j<ci.queueCount; ++j) {
            VkQueue queue{};
            if (!ci.flags && get) get(device, ci.queueFamilyIndex, j, &queue);
            else if (get2) {
                VkDeviceQueueInfo2 qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_INFO_2};
                qi.flags=ci.flags; qi.queueFamilyIndex=ci.queueFamilyIndex; qi.queueIndex=j;
                get2(device, &qi, &queue);
            }
            if (queue) entry.queues.push_back(queue);
        }
    }
    log::Logf("[CHECKPOINT] enabled device=%p queues=%zu capacity=%zu", (void*)device, entry.queues.size(), capacity);
    std::lock_guard lock(mutex);
    for (auto queue : entry.queues) queues[queue]=device;
    devices[device]=std::move(entry);
}
inline VkDevice QueueDevice(VkQueue queue)
{
    std::lock_guard lock(mutex);
    auto it=queues.find(queue);
    return it==queues.end() ? VK_NULL_HANDLE : it->second;
}
inline void ForgetDevice(VkDevice device)
{
    indirect::ForgetDevice(device);
    if (!indirect::Track()) return;
    std::lock_guard lock(mutex);
    std::erase_if(queues, [=](const auto& x){ return x.second==device; });
    std::erase_if(commands, [=](const auto& x){ return x.second.device==device; });
    devices.erase(device);
}
inline void Bind(VkCommandBuffer cb, VkPipelineBindPoint point, VkPipeline pipeline)
{
    if (!Requested()) return;
    std::lock_guard lock(mutex);
    auto it=commands.find(cb); if(it==commands.end()) return;
    if(point==VK_PIPELINE_BIND_POINT_COMPUTE) it->second.compute=pipeline;
    if(point==VK_PIPELINE_BIND_POINT_GRAPHICS) it->second.graphics=pipeline;
    if(point==VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR) it->second.rays=pipeline;
}
inline void Mark(VkCommandBuffer cb, const char* operation, VkPipelineBindPoint point, bool after, const indirect::Draw* draw=nullptr)
{
    if (!Requested()) return;
    PFN_vkCmdSetCheckpointNV set{};
    uint64_t id{};
    {
        std::lock_guard lock(mutex);
        auto it=commands.find(cb); if(it==commands.end()) return;
        const auto& c=it->second; set=c.set;
        if(!set) return;
        VkPipeline pipeline = point==VK_PIPELINE_BIND_POINT_COMPUTE ? c.compute :
            point==VK_PIPELINE_BIND_POINT_GRAPHICS ? c.graphics :
            point==VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR ? c.rays : VK_NULL_HANDLE;
        id=++serial;
        markers[id%capacity]={id,cb,pipeline,operation,after,draw!=nullptr,draw?*draw:indirect::Draw{}};
    }
    // Vulkan treats this integer token as opaque; no application pointer is
    // dereferenced by the driver. Old ring entries are explicitly marked expired.
    set(cb,reinterpret_cast<const void*>(static_cast<uintptr_t>(id)));
    if (id==1) log::Logf("[CHECKPOINT] first marker cb=%p pipeline tracking active; operation=%s",(void*)cb,operation);
}
template<class F, class... Args>
inline void Work(const char* operation, VkPipelineBindPoint point, F real, VkCommandBuffer cb, Args... args)
{
    Mark(cb,operation,point,false);
    real(cb,args...);
    Mark(cb,operation,point,true);
}
inline void Report(VkDevice device)
{
    if (!Requested()) return;
    Device copy;
    {
        std::lock_guard lock(mutex);
        auto it=devices.find(device);
        if(it==devices.end() || it->second.reported) return;
        it->second.reported=true;
        copy=it->second;
    }
    // Call only after native DEVICE_LOST. Never force a fault to obtain data.
    for(auto queue : copy.queues) {
        uint32_t count=0; copy.get(queue,&count,nullptr);
        log::Logf("[CHECKPOINT-FAULT] queue=%p count=%u",(void*)queue,count);
        count=std::min(count,64u);
        if(!count) continue;
        std::array<VkCheckpointDataNV,64> data{};
        for(auto& d:data) d.sType=VK_STRUCTURE_TYPE_CHECKPOINT_DATA_NV;
        copy.get(queue,&count,data.data());
        for(uint32_t i=0;i<std::min(count,64u);++i) {
            const auto id=static_cast<uint64_t>(reinterpret_cast<uintptr_t>(data[i].pCheckpointMarker));
            Marker marker;
            { std::lock_guard lock(mutex); marker=markers[id%capacity]; }
            if(id && marker.id==id) {
                log::Logf("[CHECKPOINT-FAULT] queue=%p stage=0x%x id=%llu cb=%p pipeline=%p %s %s",
                    (void*)queue,data[i].stage,id,(void*)marker.command,(void*)marker.pipeline,
                    marker.after?"after":"before",marker.operation);
                if(marker.hasDraw) {
                    const auto& d=marker.draw;
                    log::Logf("[CHECKPOINT-DRAW] id=%llu indexed=%d args=%p offset=%llu bytes=%llu known=%d count=%p offset=%llu bytes=%llu known=%d requested=%u forwarded=%u stride=%u skipped=%d",
                        id,d.indexed,(void*)d.buffer,d.offset,d.bufferSize,d.bufferKnown,(void*)d.countBuffer,d.countOffset,d.countSize,d.countKnown,
                        d.requested,d.forwarded,d.stride,d.skipped);
                }
            }
            else log::Logf("[CHECKPOINT-FAULT] queue=%p stage=0x%x id=%llu metadata=expired-or-external",
                (void*)queue,data[i].stage,id);
        }
    }
}

inline PFN_vkAllocateCommandBuffers realAllocate{};
inline PFN_vkFreeCommandBuffers realFree{};
inline PFN_vkBeginCommandBuffer realBegin{};
inline PFN_vkDestroyCommandPool realDestroyPool{};
inline PFN_vkCmdBindPipeline realBind{};
inline void VKAPI_CALL BindCommand(VkCommandBuffer cb,VkPipelineBindPoint point,VkPipeline pipeline)
{
    Bind(cb,point,pipeline);
    realBind(cb,point,pipeline);
}
inline VkResult VKAPI_CALL Allocate(VkDevice device,const VkCommandBufferAllocateInfo* info,VkCommandBuffer* output)
{
    auto result=realAllocate(device,info,output);
    if(result==VK_SUCCESS && indirect::Track()) {
        std::lock_guard lock(mutex); auto it=devices.find(device);
        for(uint32_t i=0;i<info->commandBufferCount;++i)
            commands[output[i]]={device,info->commandPool,it==devices.end()?nullptr:it->second.set};
    }
    return result;
}
inline void VKAPI_CALL Free(VkDevice device,VkCommandPool pool,uint32_t count,const VkCommandBuffer* buffers)
{
    if(indirect::Track()) { std::lock_guard lock(mutex); for(uint32_t i=0;i<count;++i) commands.erase(buffers[i]); }
    realFree(device,pool,count,buffers);
}
inline VkResult VKAPI_CALL Begin(VkCommandBuffer cb,const VkCommandBufferBeginInfo* info)
{
    auto result=realBegin(cb,info);
    if(result==VK_SUCCESS && indirect::Track()) {
        std::lock_guard lock(mutex); auto it=commands.find(cb);
        if(it!=commands.end()) it->second.graphics=it->second.compute=it->second.rays=VK_NULL_HANDLE;
    }
    return result;
}
inline void VKAPI_CALL DestroyPool(VkDevice device,VkCommandPool pool,const VkAllocationCallbacks* allocator)
{
    if(indirect::Track()) { std::lock_guard lock(mutex); std::erase_if(commands,[=](const auto& x){return x.second.device==device && x.second.pool==pool;}); }
    realDestroyPool(device,pool,allocator);
}

// Insert markers around valid action commands only: never between suspended
// render-pass instances at Begin/EndCommandBuffer or BindPipeline.
#define CHECKPOINT_COMMAND(name, point, params, ...) \
    inline PFN_##name real_##name{}; \
    inline void VKAPI_CALL hook_##name params { Work(#name,point,real_##name,cb,__VA_ARGS__); }
CHECKPOINT_COMMAND(vkCmdDispatch,VK_PIPELINE_BIND_POINT_COMPUTE,(VkCommandBuffer cb,uint32_t x,uint32_t y,uint32_t z),x,y,z)
CHECKPOINT_COMMAND(vkCmdDispatchIndirect,VK_PIPELINE_BIND_POINT_COMPUTE,(VkCommandBuffer cb,VkBuffer b,VkDeviceSize o),b,o)
CHECKPOINT_COMMAND(vkCmdDispatchBase,VK_PIPELINE_BIND_POINT_COMPUTE,(VkCommandBuffer cb,uint32_t a,uint32_t b,uint32_t c,uint32_t x,uint32_t y,uint32_t z),a,b,c,x,y,z)
CHECKPOINT_COMMAND(vkCmdDraw,VK_PIPELINE_BIND_POINT_GRAPHICS,(VkCommandBuffer cb,uint32_t v,uint32_t i,uint32_t f,uint32_t n),v,i,f,n)
CHECKPOINT_COMMAND(vkCmdDrawIndexed,VK_PIPELINE_BIND_POINT_GRAPHICS,(VkCommandBuffer cb,uint32_t v,uint32_t i,uint32_t f,int32_t o,uint32_t n),v,i,f,o,n)
CHECKPOINT_COMMAND(vkCmdDrawIndirect,VK_PIPELINE_BIND_POINT_GRAPHICS,(VkCommandBuffer cb,VkBuffer b,VkDeviceSize o,uint32_t n,uint32_t s),b,o,n,s)
CHECKPOINT_COMMAND(vkCmdDrawIndexedIndirect,VK_PIPELINE_BIND_POINT_GRAPHICS,(VkCommandBuffer cb,VkBuffer b,VkDeviceSize o,uint32_t n,uint32_t s),b,o,n,s)
#undef CHECKPOINT_COMMAND
inline PFN_vkCmdDrawIndirectCount real_vkCmdDrawIndirectCount{};
inline PFN_vkCmdDrawIndexedIndirectCount real_vkCmdDrawIndexedIndirectCount{};
template<class F> inline void DrawCount(const char* name,F real,VkCommandBuffer cb,VkBuffer b,VkDeviceSize offset,
    VkBuffer count,VkDeviceSize countOffset,uint32_t maximum,uint32_t stride,bool indexed)
{
    VkDevice device{};
    if(indirect::Track()) {std::lock_guard lock(mutex); auto it=commands.find(cb); if(it!=commands.end()) device=it->second.device;}
    const auto draw=indirect::Inspect(device,b,offset,count,countOffset,maximum,stride,indexed);
    Mark(cb,name,VK_PIPELINE_BIND_POINT_GRAPHICS,false,&draw);
    if(!draw.skipped) real(cb,b,offset,count,countOffset,draw.forwarded,stride);
    Mark(cb,name,VK_PIPELINE_BIND_POINT_GRAPHICS,true,&draw);
}
inline void VKAPI_CALL hook_vkCmdDrawIndirectCount(VkCommandBuffer cb,VkBuffer b,VkDeviceSize o,VkBuffer c,VkDeviceSize co,uint32_t n,uint32_t s)
{DrawCount("vkCmdDrawIndirectCount",real_vkCmdDrawIndirectCount,cb,b,o,c,co,n,s,false);}
inline void VKAPI_CALL hook_vkCmdDrawIndexedIndirectCount(VkCommandBuffer cb,VkBuffer b,VkDeviceSize o,VkBuffer c,VkDeviceSize co,uint32_t n,uint32_t s)
{DrawCount("vkCmdDrawIndexedIndirectCount",real_vkCmdDrawIndexedIndirectCount,cb,b,o,c,co,n,s,true);}
inline PFN_vkVoidFunction Wrap(const char* name,PFN_vkVoidFunction real)
{
    if(!name || !real) return real;
#define WRAP(api,hook,target) if(!std::strcmp(name,#api)) { if(!target) target=reinterpret_cast<PFN_##api>(real); return reinterpret_cast<PFN_vkVoidFunction>(&hook); }
    WRAP(vkAllocateCommandBuffers,Allocate,realAllocate)
    WRAP(vkFreeCommandBuffers,Free,realFree)
    WRAP(vkBeginCommandBuffer,Begin,realBegin)
    WRAP(vkDestroyCommandPool,DestroyPool,realDestroyPool)
    WRAP(vkCmdBindPipeline,BindCommand,realBind)
#define ACTION(api) WRAP(api,hook_##api,real_##api)
    ACTION(vkCmdDispatch) ACTION(vkCmdDispatchIndirect) ACTION(vkCmdDispatchBase)
    ACTION(vkCmdDraw) ACTION(vkCmdDrawIndexed) ACTION(vkCmdDrawIndirect) ACTION(vkCmdDrawIndexedIndirect)
    ACTION(vkCmdDrawIndirectCount) ACTION(vkCmdDrawIndexedIndirectCount)
#undef ACTION
    WRAP(vkCmdDispatchBaseKHR,hook_vkCmdDispatchBase,real_vkCmdDispatchBase)
    WRAP(vkCmdDrawIndirectCountKHR,hook_vkCmdDrawIndirectCount,real_vkCmdDrawIndirectCount)
    WRAP(vkCmdDrawIndexedIndirectCountKHR,hook_vkCmdDrawIndexedIndirectCount,real_vkCmdDrawIndexedIndirectCount)
#undef WRAP
    return indirect::Wrap(name,real);
}
}
