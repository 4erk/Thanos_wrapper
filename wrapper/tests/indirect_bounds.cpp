#include "../src/vk/GpuCheckpoints.h"
#include <cstdio>
#include <limits>
#define CHECK(x) do {if(!(x)){printf("FAIL line %d: %s\n",__LINE__,#x); return 1;}} while(0)
using namespace thanosray::vk;
static auto device=reinterpret_cast<VkDevice>(uintptr_t(0x100));
static auto other=reinterpret_cast<VkDevice>(uintptr_t(0x101));
static auto cb=reinterpret_cast<VkCommandBuffer>(uintptr_t(0x200));
static auto args=reinterpret_cast<VkBuffer>(uintptr_t(0x300));
static auto count=reinterpret_cast<VkBuffer>(uintptr_t(0x400));
static auto unknown=reinterpret_cast<VkBuffer>(uintptr_t(0x500));
static VkBuffer nextBuffer{};
static int calls{},destroyed{};
static uint32_t lastMaximum{};
static uint64_t lastMarker{};
static bool correct=true;
static void VKAPI_CALL SetCheckpoint(VkCommandBuffer command,const void* token)
{correct &= command==cb; lastMarker=static_cast<uint64_t>(reinterpret_cast<uintptr_t>(token));}
static VkResult VKAPI_CALL Create(VkDevice,const VkBufferCreateInfo*,const VkAllocationCallbacks*,VkBuffer* output)
{*output=nextBuffer; return VK_SUCCESS;}
static void VKAPI_CALL Destroy(VkDevice,VkBuffer,const VkAllocationCallbacks*) {++destroyed;}
static void VKAPI_CALL Draw(VkCommandBuffer command,VkBuffer b,VkDeviceSize o,VkBuffer c,VkDeviceSize co,uint32_t n,uint32_t s)
{correct &= command==cb && b==args && !o && c==count && !co && s==20; lastMaximum=n; ++calls;}
int main()
{
    using indirect::Capacity;
    CHECK(Capacity(20,0,20,20,2)==1);
    CHECK(Capacity(40,0,20,20,2)==2);
    CHECK(Capacity(39,0,20,20,2)==1);
    CHECK(Capacity(100,100,20,20,2)==0);
    CHECK(Capacity(100,101,20,20,2)==0);
    CHECK(Capacity(100,0,0,20,2)==0);
    CHECK(Capacity(20,0,0,20,1)==0);
    CHECK(Capacity(100,0,21,20,2)==0);
    CHECK(Capacity(100,0,24,20,4)==4);
    CHECK(Capacity(32,0,16,16,3)==2);
    CHECK(Capacity(0,0,20,20,0)==0);
    CHECK(Capacity(UINT64_MAX,0,20,20,UINT32_MAX)==UINT32_MAX);
    CHECK(Capacity(UINT64_MAX,UINT64_MAX-19,20,20,2)==0);
    CHECK(Capacity(UINT64_MAX,UINT64_MAX-20,20,20,2)==1);
    if(!indirect::Track()){puts("SKIP: enable IndirectBounds or GpuCheckpoints for tracking test");return 77;}
    auto create=reinterpret_cast<PFN_vkCreateBuffer>(checkpoints::Wrap("vkCreateBuffer",reinterpret_cast<PFN_vkVoidFunction>(&Create)));
    auto destroy=reinterpret_cast<PFN_vkDestroyBuffer>(checkpoints::Wrap("vkDestroyBuffer",reinterpret_cast<PFN_vkVoidFunction>(&Destroy)));
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};info.size=20;info.usage=VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
    VkBuffer out{};nextBuffer=args;CHECK(create(device,&info,nullptr,&out)==VK_SUCCESS && out==args);
    info.size=4;nextBuffer=count;CHECK(create(device,&info,nullptr,&out)==VK_SUCCESS);
    checkpoints::commands[cb]={device,VK_NULL_HANDLE,&SetCheckpoint};
    auto draw=reinterpret_cast<PFN_vkCmdDrawIndexedIndirectCount>(checkpoints::Wrap("vkCmdDrawIndexedIndirectCount",reinterpret_cast<PFN_vkVoidFunction>(&Draw)));
    draw(cb,args,0,count,0,2,20);
    CHECK(calls==1 && correct && lastMaximum==(indirect::Enabled()?1u:2u));
    if(checkpoints::Requested()) {
        const auto& marker=checkpoints::markers[lastMarker%checkpoints::capacity];
        CHECK(lastMarker && marker.id==lastMarker && marker.after && marker.hasDraw);
        CHECK(marker.draw.buffer==args && marker.draw.countBuffer==count);
        CHECK(marker.draw.bufferSize==20 && marker.draw.countSize==4 && marker.draw.stride==20);
        CHECK(marker.draw.requested==2 && marker.draw.forwarded==lastMaximum);
    }
    auto inspected=indirect::Inspect(device,args,0,count,1,1,20,true);
    CHECK(inspected.skipped==indirect::Enabled());
    inspected=indirect::Inspect(device,args,0,count,4,1,20,true);
    CHECK(inspected.skipped==indirect::Enabled());
    inspected=indirect::Inspect(device,args,20,count,0,1,20,true);
    CHECK(inspected.skipped==indirect::Enabled());
    inspected=indirect::Inspect(device,args,0,count,0,1,0,true);
    CHECK(inspected.skipped==indirect::Enabled());
    inspected=indirect::Inspect(device,args,0,count,0,0,21,true);
    CHECK(inspected.skipped==indirect::Enabled());
    // Unknown handles, including identical handles on other devices, pass through.
    inspected=indirect::Inspect(other,args,0,count,0,123,20,true);
    CHECK(!inspected.bufferKnown && !inspected.countKnown && !inspected.skipped && inspected.forwarded==123);
    inspected=indirect::Inspect(device,unknown,0,count,0,123,20,true);
    CHECK(!inspected.bufferKnown && !inspected.skipped && inspected.forwarded==123);
    if(indirect::Enabled()) {draw(cb,args,0,count,4,1,20);CHECK(calls==1);}
    destroy(device,args,nullptr);CHECK(destroyed==1);
    inspected=indirect::Inspect(device,args,0,count,0,2,20,true);CHECK(!inspected.bufferKnown && inspected.forwarded==2);
    info.size=80;nextBuffer=args;CHECK(create(device,&info,nullptr,&out)==VK_SUCCESS);
    inspected=indirect::Inspect(device,args,0,count,0,3,20,true);CHECK(inspected.forwarded==3 && inspected.bufferSize==80);
    checkpoints::ForgetDevice(device);CHECK(indirect::buffers.empty() && checkpoints::commands.empty());
    printf("PASS: indirect capacity/overflow, argument forwarding, invalid ranges, unknown resources and lifetime; guard=%d\n",indirect::Enabled());
}
