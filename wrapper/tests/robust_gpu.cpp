#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <vulkan/vulkan.h>
#include <cstdio>
#include <vector>
#include <fstream>
#include <filesystem>
#define OK(call) do { auto r=(call); if(r!=VK_SUCCESS) { printf("FAIL line %d result=%d: %s\n",__LINE__,r,#call);return 1; } } while(0)
#define DEV(name) reinterpret_cast<PFN_##name>(gdp(device,#name))

int wmain(int argc,wchar_t** argv)
{
    if(argc<3) { puts("Usage: robust_gpu.exe wrapper.dll robust.spv [features2]"); return 2; }
    // This test intentionally uses null/OOB descriptors: require the explicit
    // wrapper opt-in, so it is never run on a device without enabled guards.
    const auto ini=std::filesystem::path(argv[1]).parent_path()/L"Thanos-wrapper.ini";
    if(!GetPrivateProfileIntW(L"experiments",L"RobustAccess",0,ini.c_str())) { puts("SKIP: RobustAccess=1 is required");return 77; }
    HMODULE lib=LoadLibraryW(argv[1]); if(!lib) return 3;
    auto gip=(PFN_vkGetInstanceProcAddr)GetProcAddress(lib,"vkGetInstanceProcAddr");
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.apiVersion=VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ici.pApplicationInfo=&app;
    VkInstance instance{}; OK(((PFN_vkCreateInstance)gip(nullptr,"vkCreateInstance"))(&ici,nullptr,&instance));
    auto enumerate=(PFN_vkEnumeratePhysicalDevices)gip(instance,"vkEnumeratePhysicalDevices");
    uint32_t count=0; OK(enumerate(instance,&count,nullptr)); if(!count) return 77;
    std::vector<VkPhysicalDevice> physical(count); OK(enumerate(instance,&count,physical.data())); auto phys=physical[0];
    // Query the system loader directly to avoid mistaking spoofed capabilities
    // for native support. Physical-device handles belong to the same loader.
    wchar_t sys[MAX_PATH]; GetSystemDirectoryW(sys,MAX_PATH); wcscat_s(sys,L"\\vulkan-1.dll");
    auto nativeLib=GetModuleHandleW(L"vulkan-1_real.dll"); if(!nativeLib)nativeLib=LoadLibraryW(sys);
    auto nativeGip=(PFN_vkGetInstanceProcAddr)GetProcAddress(nativeLib,"vkGetInstanceProcAddr");
    VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    VkPhysicalDeviceRobustness2FeaturesEXT robust{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT}; features.pNext=&robust;
    ((PFN_vkGetPhysicalDeviceFeatures2)nativeGip(instance,"vkGetPhysicalDeviceFeatures2"))(phys,&features);
    if(!robust.nullDescriptor||!robust.robustBufferAccess2||!features.features.robustBufferAccess) { puts("SKIP: native guards unsupported");return 77; }
    auto getQueues=(PFN_vkGetPhysicalDeviceQueueFamilyProperties)gip(instance,"vkGetPhysicalDeviceQueueFamilyProperties");
    uint32_t nq=0; getQueues(phys,&nq,nullptr); std::vector<VkQueueFamilyProperties> queues(nq);getQueues(phys,&nq,queues.data());
    uint32_t family=0; while(family<nq && !(queues[family].queueFlags&VK_QUEUE_COMPUTE_BIT)) ++family; if(family==nq)return 77;
    float priority=1; VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};qci.queueFamilyIndex=family;qci.queueCount=1;qci.pQueuePriorities=&priority;
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};f12.bufferDeviceAddress=VK_TRUE;
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};f2.pNext=&f12;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};dci.pNext=argc>3?(void*)&f2:(void*)&f12;dci.queueCreateInfoCount=1;dci.pQueueCreateInfos=&qci;
    VkDevice device{}; OK(((PFN_vkCreateDevice)gip(instance,"vkCreateDevice"))(phys,&dci,nullptr,&device));
    auto gdp=(PFN_vkGetDeviceProcAddr)gip(instance,"vkGetDeviceProcAddr"); VkQueue queue{};DEV(vkGetDeviceQueue)(device,family,0,&queue);
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};bci.size=4096;bci.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;bci.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer buffer{};OK(DEV(vkCreateBuffer)(device,&bci,nullptr,&buffer));VkMemoryRequirements requirements{};DEV(vkGetBufferMemoryRequirements)(device,buffer,&requirements);
    VkPhysicalDeviceMemoryProperties memoryProperties{};((PFN_vkGetPhysicalDeviceMemoryProperties)gip(instance,"vkGetPhysicalDeviceMemoryProperties"))(phys,&memoryProperties);
    uint32_t type=0; const auto needed=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    while(type<memoryProperties.memoryTypeCount && (!(requirements.memoryTypeBits&(1u<<type)) || (memoryProperties.memoryTypes[type].propertyFlags&needed)!=needed)) ++type;
    if(type==memoryProperties.memoryTypeCount)return 77;
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};mai.allocationSize=requirements.size;mai.memoryTypeIndex=type;VkDeviceMemory memory{};
    OK(DEV(vkAllocateMemory)(device,&mai,nullptr,&memory));OK(DEV(vkBindBufferMemory)(device,buffer,memory,0));
    void* mapped{};OK(DEV(vkMapMemory)(device,memory,0,VK_WHOLE_SIZE,0,&mapped));auto* values=(uint32_t*)mapped;for(int i=0;i<1024;++i)values[i]=0xa5a5a5a5u;
    VkDescriptorSetLayoutBinding bindings[2]{}; for(uint32_t i=0;i<2;++i){bindings[i].binding=i;bindings[i].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;bindings[i].descriptorCount=1;bindings[i].stageFlags=VK_SHADER_STAGE_COMPUTE_BIT;}
    VkDescriptorSetLayoutCreateInfo slci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};slci.bindingCount=2;slci.pBindings=bindings;VkDescriptorSetLayout layout{};
    OK(DEV(vkCreateDescriptorSetLayout)(device,&slci,nullptr,&layout));VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,2};
    VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};dpci.maxSets=1;dpci.poolSizeCount=1;dpci.pPoolSizes=&poolSize;VkDescriptorPool descriptors{};
    OK(DEV(vkCreateDescriptorPool)(device,&dpci,nullptr,&descriptors));VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};dsai.descriptorPool=descriptors;dsai.descriptorSetCount=1;dsai.pSetLayouts=&layout;VkDescriptorSet set{};
    OK(DEV(vkAllocateDescriptorSets)(device,&dsai,&set));VkDescriptorBufferInfo infos[2]{{buffer,0,16},{VK_NULL_HANDLE,0,VK_WHOLE_SIZE}};
    VkWriteDescriptorSet writes[2]{};for(uint32_t i=0;i<2;++i){writes[i].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;writes[i].dstSet=set;writes[i].dstBinding=i;writes[i].descriptorCount=1;writes[i].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;writes[i].pBufferInfo=&infos[i];}
    DEV(vkUpdateDescriptorSets)(device,2,writes,0,nullptr);
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};plci.setLayoutCount=1;plci.pSetLayouts=&layout;VkPipelineLayout pipelineLayout{};
    OK(DEV(vkCreatePipelineLayout)(device,&plci,nullptr,&pipelineLayout));
    std::ifstream stream(std::filesystem::path(argv[2]),std::ios::binary|std::ios::ate);if(!stream)return 4;size_t bytes=(size_t)stream.tellg();if(bytes<20||bytes%4)return 5;std::vector<uint32_t> code(bytes/4);stream.seekg(0);stream.read((char*)code.data(),bytes);if(!stream)return 6;
    VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};smci.codeSize=bytes;smci.pCode=code.data();VkShaderModule shader{};OK(DEV(vkCreateShaderModule)(device,&smci,nullptr,&shader));
    VkComputePipelineCreateInfo cpci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};cpci.stage.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;cpci.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;cpci.stage.module=shader;cpci.stage.pName="main";cpci.layout=pipelineLayout;VkPipeline pipeline{};
    OK(DEV(vkCreateComputePipelines)(device,VK_NULL_HANDLE,1,&cpci,nullptr,&pipeline));
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};pci.queueFamilyIndex=family;VkCommandPool pool{};OK(DEV(vkCreateCommandPool)(device,&pci,nullptr,&pool));
    VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};cbai.commandPool=pool;cbai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;cbai.commandBufferCount=1;VkCommandBuffer cb{};OK(DEV(vkAllocateCommandBuffers)(device,&cbai,&cb));
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};OK(DEV(vkBeginCommandBuffer)(cb,&begin));DEV(vkCmdBindPipeline)(cb,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);DEV(vkCmdBindDescriptorSets)(cb,VK_PIPELINE_BIND_POINT_COMPUTE,pipelineLayout,0,1,&set,0,nullptr);DEV(vkCmdDispatch)(cb,1,1,1);
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};barrier.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
    DEV(vkCmdPipelineBarrier)(cb,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&barrier,0,nullptr,0,nullptr);OK(DEV(vkEndCommandBuffer)(cb));
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};VkFence fence{};OK(DEV(vkCreateFence)(device,&fci,nullptr,&fence));VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};si.commandBufferCount=1;si.pCommandBuffers=&cb;OK(DEV(vkQueueSubmit)(queue,1,&si,fence));OK(DEV(vkWaitForFences)(device,1,&fence,VK_TRUE,5000000000ull));
    bool valid=values[0]==0 && values[1]==456;for(int i=2;i<1024;++i)valid &= values[i]==0xa5a5a5a5u;
    printf("%s: GPU null read=%u valid write=%u OOB sentinel=%08x (Features2=%d)\n",valid?"PASS":"FAIL",values[0],values[1],values[512],argc>3);
    DEV(vkDestroyFence)(device,fence,nullptr);DEV(vkDestroyCommandPool)(device,pool,nullptr);DEV(vkDestroyPipeline)(device,pipeline,nullptr);DEV(vkDestroyShaderModule)(device,shader,nullptr);DEV(vkDestroyPipelineLayout)(device,pipelineLayout,nullptr);DEV(vkDestroyDescriptorPool)(device,descriptors,nullptr);DEV(vkDestroyDescriptorSetLayout)(device,layout,nullptr);DEV(vkUnmapMemory)(device,memory);DEV(vkDestroyBuffer)(device,buffer,nullptr);DEV(vkFreeMemory)(device,memory,nullptr);DEV(vkDestroyDevice)(device,nullptr);((PFN_vkDestroyInstance)gip(instance,"vkDestroyInstance"))(instance,nullptr);
    return valid?0:7;
}
