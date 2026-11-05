#pragma once
#include "ShaderDiagnostics.h"
#include <unordered_map>
#include <algorithm>

namespace thanosray::vk::indirect {
inline bool Enabled()
{
    static const bool enabled=GetPrivateProfileIntW(L"experiments",L"IndirectBounds",0,diagnostics::ConfigPath().c_str())!=0;
    return enabled;
}
inline bool Track()
{
    static const bool enabled=Enabled() || GetPrivateProfileIntW(L"diagnostics",L"GpuCheckpoints",0,diagnostics::ConfigPath().c_str())!=0;
    return enabled;
}
struct Buffer { VkDeviceSize size{}; VkBufferUsageFlags usage{}; };
struct Draw {
    VkBuffer buffer{},countBuffer{};
    VkDeviceSize offset{},countOffset{},bufferSize{},countSize{};
    uint32_t requested{},forwarded{},stride{};
    bool bufferKnown{},countKnown{},indexed{},skipped{};
};
inline std::mutex mutex;
inline std::unordered_map<VkDevice,std::unordered_map<VkBuffer,Buffer>> buffers;

inline uint32_t Capacity(VkDeviceSize size,VkDeviceSize offset,uint32_t stride,uint32_t commandSize,uint32_t requested)
{
    if(!requested) return 0;
    if(offset>size || size-offset<commandSize) return 0;
    // IndirectCount requires a valid stride even for a single command.
    if(stride<commandSize || stride%4) return 0;
    if(requested==1) return 1;
    return static_cast<uint32_t>(std::min<uint64_t>(requested,1+(size-offset-commandSize)/stride));
}
inline Draw Inspect(VkDevice device,VkBuffer buffer,VkDeviceSize offset,VkBuffer countBuffer,
    VkDeviceSize countOffset,uint32_t maximum,uint32_t stride,bool indexed)
{
    Draw draw{buffer,countBuffer,offset,countOffset,0,0,maximum,maximum,stride,false,false,indexed,false};
    Buffer args{},count{};
    if(Track()) {
        std::lock_guard lock(mutex);
        auto dev=buffers.find(device);
        if(dev!=buffers.end()) {
            auto a=dev->second.find(buffer),c=dev->second.find(countBuffer);
            if(a!=dev->second.end()) {draw.bufferKnown=true; args=a->second; draw.bufferSize=args.size;}
            if(c!=dev->second.end()) {draw.countKnown=true; count=c->second; draw.countSize=count.size;}
        }
    }
    if(!Enabled()) return draw;
    if(draw.bufferKnown) {
        const uint32_t commandSize=indexed?sizeof(VkDrawIndexedIndirectCommand):sizeof(VkDrawIndirectCommand);
        draw.forwarded=Capacity(args.size,offset,stride,commandSize,maximum);
        if(offset%4 || stride<commandSize || stride%4) draw.skipped=true;
    }
    if(draw.countKnown && (countOffset>count.size || count.size-countOffset<4 ||
        countOffset%4)) draw.skipped=true;
    if(draw.forwarded==0 && maximum) draw.skipped=true;
    // Unknown resources are never guessed, read back or silently removed.
    if(draw.skipped || draw.forwarded!=maximum) {
        static std::atomic<uint32_t> warnings{0};
        if(warnings.fetch_add(1)<32)
            log::Logf("[INDIRECT-BOUNDS] indexed=%d args=%p offset=%llu bytes=%llu known=%d count=%p offset=%llu bytes=%llu known=%d requested=%u forwarded=%u stride=%u skipped=%d",
                indexed,(void*)buffer,offset,args.size,draw.bufferKnown,(void*)countBuffer,countOffset,count.size,draw.countKnown,
                maximum,draw.forwarded,stride,draw.skipped);
    }
    return draw;
}
inline void ForgetDevice(VkDevice device)
{
    if(Track()) {std::lock_guard lock(mutex); buffers.erase(device);}
}
inline PFN_vkCreateBuffer realCreate{};
inline PFN_vkDestroyBuffer realDestroy{};
inline VkResult VKAPI_CALL Create(VkDevice device,const VkBufferCreateInfo* info,const VkAllocationCallbacks* allocator,VkBuffer* output)
{
    auto result=realCreate(device,info,allocator,output);
    if(result==VK_SUCCESS && Track()) {
        std::lock_guard lock(mutex); buffers[device][*output]={info->size,info->usage};
    }
    return result;
}
inline void VKAPI_CALL Destroy(VkDevice device,VkBuffer buffer,const VkAllocationCallbacks* allocator)
{
    if(Track()) {std::lock_guard lock(mutex); auto it=buffers.find(device); if(it!=buffers.end()) it->second.erase(buffer);}
    realDestroy(device,buffer,allocator);
}
inline PFN_vkVoidFunction Wrap(const char* name,PFN_vkVoidFunction real)
{
    if(!name || !real) return real;
    if(!std::strcmp(name,"vkCreateBuffer")) {
        if(!realCreate) realCreate=reinterpret_cast<PFN_vkCreateBuffer>(real);
        return reinterpret_cast<PFN_vkVoidFunction>(&Create);
    }
    if(!std::strcmp(name,"vkDestroyBuffer")) {
        if(!realDestroy) realDestroy=reinterpret_cast<PFN_vkDestroyBuffer>(real);
        return reinterpret_cast<PFN_vkVoidFunction>(&Destroy);
    }
    return real;
}
}
