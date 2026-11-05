#include "../src/vk/RuntimeDiagnostics.h"
#include <cstdio>
#define CHECK(x) do { if(!(x)) { printf("FAIL line %d: %s\n",__LINE__,#x); return 1; } } while(0)
using namespace thanosray::vk;
static auto device=reinterpret_cast<VkDevice>(uintptr_t(0x100));
static auto queue=reinterpret_cast<VkQueue>(uintptr_t(0x200));
static auto cb=reinterpret_cast<VkCommandBuffer>(uintptr_t(0x300));
static auto pool=reinterpret_cast<VkCommandPool>(uintptr_t(0x400));
static auto pipeline=reinterpret_cast<VkPipeline>(uintptr_t(0x500));
static std::vector<const void*> tokens;
static int actionCalls{}, reports{}, presents{};
static bool argsCorrect=true;
static void VKAPI_CALL Set(VkCommandBuffer command,const void* marker) { argsCorrect &= command==cb; tokens.push_back(marker); }
static void VKAPI_CALL Get(VkQueue q,uint32_t* count,VkCheckpointDataNV* data)
{
    argsCorrect &= q==queue; ++reports;
    if(!data) { *count=100; return; }
    argsCorrect &= *count==64 && data[0].sType==VK_STRUCTURE_TYPE_CHECKPOINT_DATA_NV;
    *count=1; data[0].stage=VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT; data[0].pCheckpointMarker=const_cast<void*>(tokens.back());
}
static void VKAPI_CALL Dispatch(VkCommandBuffer command,uint32_t x,uint32_t y,uint32_t z)
{ argsCorrect &= command==cb && x==2 && y==3 && z==4 && tokens.size()==1; ++actionCalls; }
static void VKAPI_CALL Bind(VkCommandBuffer command,VkPipelineBindPoint point,VkPipeline value)
{ argsCorrect &= command==cb && point==VK_PIPELINE_BIND_POINT_COMPUTE && value==pipeline; }
static void VKAPI_CALL Queue(VkDevice d,uint32_t family,uint32_t index,VkQueue* output)
{ argsCorrect &= d==device && family==0 && index==0; *output=queue; }
static PFN_vkVoidFunction VKAPI_CALL Resolve(VkDevice,const char* name)
{
    if(!strcmp(name,"vkCmdSetCheckpointNV")) return reinterpret_cast<PFN_vkVoidFunction>(&Set);
    if(!strcmp(name,"vkGetQueueCheckpointDataNV")) return reinterpret_cast<PFN_vkVoidFunction>(&Get);
    if(!strcmp(name,"vkGetDeviceQueue")) return reinterpret_cast<PFN_vkVoidFunction>(&Queue);
    return nullptr;
}
static VkResult VKAPI_CALL Allocate(VkDevice d,const VkCommandBufferAllocateInfo*,VkCommandBuffer* output)
{ argsCorrect &= d==device; *output=cb; return VK_SUCCESS; }
static VkResult VKAPI_CALL Begin(VkCommandBuffer,const VkCommandBufferBeginInfo*) { return VK_SUCCESS; }
static VkResult VKAPI_CALL Present(VkQueue q,const VkPresentInfoKHR* info)
{ argsCorrect &= q==queue && info==nullptr; ++presents; return VK_ERROR_OUT_OF_DATE_KHR; }
int main()
{
    if(!checkpoints::Requested() || !runtime::Enabled()) { puts("SKIP: GpuCheckpoints=1 and RuntimeTrace=1 required"); return 77; }
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}; qci.queueCount=1;
    VkDeviceCreateInfo ci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};ci.queueCreateInfoCount=1;ci.pQueueCreateInfos=&qci;
    checkpoints::OnDevice(device,&Resolve,ci,true);
    checkpoints::realAllocate=&Allocate; checkpoints::realBegin=&Begin;
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};ai.commandPool=pool;ai.commandBufferCount=1;
    VkCommandBuffer output{}; CHECK(checkpoints::Allocate(device,&ai,&output)==VK_SUCCESS);
    CHECK(checkpoints::Begin(cb,nullptr)==VK_SUCCESS);
    auto bind=reinterpret_cast<PFN_vkCmdBindPipeline>(checkpoints::Wrap("vkCmdBindPipeline",reinterpret_cast<PFN_vkVoidFunction>(&Bind)));
    bind(cb,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);
    auto action=reinterpret_cast<PFN_vkCmdDispatch>(checkpoints::Wrap("vkCmdDispatch",reinterpret_cast<PFN_vkVoidFunction>(&Dispatch)));
    action(cb,2,3,4);
    CHECK(tokens.size()==2 && actionCalls==1 && argsCorrect && reports==0);
    auto id=reinterpret_cast<uintptr_t>(tokens.back());
    CHECK(checkpoints::markers[id%checkpoints::capacity].after);
    CHECK(checkpoints::markers[id%checkpoints::capacity].pipeline==pipeline);
    runtime::CheckFault(VK_SUCCESS,queue); CHECK(reports==0);
    runtime::CheckFault(VK_ERROR_DEVICE_LOST,queue); CHECK(reports==2);
    runtime::CheckFault(VK_ERROR_DEVICE_LOST,device); CHECK(reports==2);
    CHECK(checkpoints::Begin(cb,nullptr)==VK_SUCCESS);
    CHECK(checkpoints::commands[cb].compute==VK_NULL_HANDLE);
    // Exercise bounded storage and ensure old markers cannot silently alias.
    for(size_t i=0;i<checkpoints::capacity;++i) checkpoints::Mark(cb,"test",VK_PIPELINE_BIND_POINT_COMPUTE,false);
    CHECK(checkpoints::markers[id%checkpoints::capacity].id!=id);
    checkpoints::ForgetDevice(device);
    CHECK(checkpoints::commands.empty() && !checkpoints::QueueDevice(queue));
    auto count=tokens.size(); checkpoints::Mark(cb,"freed",VK_PIPELINE_BIND_POINT_COMPUTE,false); CHECK(tokens.size()==count);
    // Pure deadline policy: no catch-up burst after a long pause.
    pacing::Clock::time_point previous{};
    const auto start=pacing::Clock::time_point(std::chrono::seconds(100));
    CHECK(pacing::Reserve(start,previous,30)==start);
    CHECK(pacing::Reserve(start,previous,30)==start+std::chrono::nanoseconds(33333333));
    CHECK(pacing::Reserve(start+std::chrono::seconds(5),previous,30)==start+std::chrono::seconds(5));
    runtime::realPresent=&Present;
    const auto started=pacing::Clock::now();
    for(int i=0;i<4;++i) CHECK(runtime::QueuePresent(queue,nullptr)==VK_ERROR_OUT_OF_DATE_KHR);
    CHECK(presents==4 && argsCorrect);
    if(GetPrivateProfileIntW(L"experiments",L"FrameLimit",0,diagnostics::ConfigPath().c_str())==30)
        CHECK(pacing::Clock::now()-started>=std::chrono::milliseconds(95));
    puts("PASS: checkpoint order, arguments, pipeline identity, bounded reporting/history, lifetime cleanup and pacing");
}
