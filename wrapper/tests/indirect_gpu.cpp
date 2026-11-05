#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <vulkan/vulkan.h>
#include <vector>
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#define OK(call) do {auto r=(call);if(r!=VK_SUCCESS){printf("FAIL line %d result=%d: %s\n",__LINE__,r,#call);exit(1);}}while(0)
static PFN_vkGetInstanceProcAddr gip;
static PFN_vkGetDeviceProcAddr gdp;
static VkInstance instance;
static VkDevice device;
static VkPhysicalDeviceMemoryProperties memoryProperties;
static bool useGipa;
#define DEV(name) reinterpret_cast<PFN_##name>(useGipa?gip(instance,#name):gdp(device,#name))
static uint32_t MemoryType(uint32_t bits,VkMemoryPropertyFlags flags)
{
    for(uint32_t i=0;i<memoryProperties.memoryTypeCount;++i)
        if((bits&(1u<<i)) && (memoryProperties.memoryTypes[i].propertyFlags&flags)==flags) return i;
    puts("SKIP: suitable memory unavailable");exit(77);
}
struct Buffer {VkBuffer buffer{};VkDeviceMemory memory{};void* mapped{};};
static Buffer MakeBuffer(VkDeviceSize bytes,VkBufferUsageFlags usage)
{
    Buffer b;
    VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};ci.size=bytes;ci.usage=usage;
    OK(DEV(vkCreateBuffer)(device,&ci,nullptr,&b.buffer));
    VkMemoryRequirements req{};DEV(vkGetBufferMemoryRequirements)(device,b.buffer,&req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=req.size;
    ai.memoryTypeIndex=MemoryType(req.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    OK(DEV(vkAllocateMemory)(device,&ai,nullptr,&b.memory));
    OK(DEV(vkBindBufferMemory)(device,b.buffer,b.memory,0));
    OK(DEV(vkMapMemory)(device,b.memory,0,VK_WHOLE_SIZE,0,&b.mapped));
    return b;
}
static VkShaderModule Shader(const wchar_t* path)
{
    std::ifstream file(std::filesystem::path(path),std::ios::binary|std::ios::ate);
    if(!file){puts("Missing shader");exit(2);}
    size_t bytes=static_cast<size_t>(file.tellg());if(bytes<20 || bytes%4)exit(2);
    std::vector<uint32_t> code(bytes/4);file.seekg(0);file.read(reinterpret_cast<char*>(code.data()),bytes);if(!file)exit(2);
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};ci.codeSize=bytes;ci.pCode=code.data();
    VkShaderModule shader{};OK(DEV(vkCreateShaderModule)(device,&ci,nullptr,&shader));return shader;
}
int wmain(int argc,wchar_t** argv)
{
    if(argc<4){puts("Usage: indirect_gpu_test.exe wrapper.dll indirect.vert.spv indirect.frag.spv [gipa]");return 2;}
    const auto ini=std::filesystem::path(argv[1]).parent_path()/L"Thanos-wrapper.ini";
    if(!GetPrivateProfileIntW(L"experiments",L"IndirectBounds",0,ini.c_str())){puts("SKIP: current wrapper with IndirectBounds=1 required");return 77;}
    auto library=LoadLibraryW(argv[1]);if(!library)return 3;
    gip=reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(library,"vkGetInstanceProcAddr"));if(!gip)return 3;
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};app.apiVersion=VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};ici.pApplicationInfo=&app;
    OK(reinterpret_cast<PFN_vkCreateInstance>(gip(nullptr,"vkCreateInstance"))(&ici,nullptr,&instance));
    auto enumerate=reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(gip(instance,"vkEnumeratePhysicalDevices"));
    uint32_t count=0;OK(enumerate(instance,&count,nullptr));if(!count)return 77;
    std::vector<VkPhysicalDevice> physical(count);OK(enumerate(instance,&count,physical.data()));auto phys=physical[0];
    VkPhysicalDeviceVulkan13Features supported13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features supported12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};supported12.pNext=&supported13;
    VkPhysicalDeviceFeatures2 supported{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};supported.pNext=&supported12;
    reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(gip(instance,"vkGetPhysicalDeviceFeatures2"))(phys,&supported);
    if(!supported13.dynamicRendering || !supported12.drawIndirectCount || !supported.features.multiDrawIndirect)return 77;
    auto queueProps=reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(gip(instance,"vkGetPhysicalDeviceQueueFamilyProperties"));
    uint32_t nq=0;queueProps(phys,&nq,nullptr);std::vector<VkQueueFamilyProperties> queues(nq);queueProps(phys,&nq,queues.data());
    uint32_t family=0;while(family<nq && !(queues[family].queueFlags&VK_QUEUE_GRAPHICS_BIT))++family;if(family==nq)return 77;
    float priority=1;VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};qci.queueFamilyIndex=family;qci.queueCount=1;qci.pQueuePriorities=&priority;
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};f13.dynamicRendering=VK_TRUE;
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};f12.pNext=&f13;f12.bufferDeviceAddress=VK_TRUE;f12.drawIndirectCount=VK_TRUE;
    VkPhysicalDeviceFeatures f{};f.multiDrawIndirect=VK_TRUE;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};dci.pNext=&f12;dci.pEnabledFeatures=&f;dci.queueCreateInfoCount=1;dci.pQueueCreateInfos=&qci;
    OK(reinterpret_cast<PFN_vkCreateDevice>(gip(instance,"vkCreateDevice"))(phys,&dci,nullptr,&device));
    gdp=reinterpret_cast<PFN_vkGetDeviceProcAddr>(gip(instance,"vkGetDeviceProcAddr"));useGipa=argc>4;
    reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(gip(instance,"vkGetPhysicalDeviceMemoryProperties"))(phys,&memoryProperties);
    VkQueue queue{};DEV(vkGetDeviceQueue)(device,family,0,&queue);
    auto arguments=MakeBuffer(sizeof(VkDrawIndexedIndirectCommand),VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT);
    *static_cast<VkDrawIndexedIndirectCommand*>(arguments.mapped)={3,1,0,0,0};
    auto drawCount=MakeBuffer(4,VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT);*static_cast<uint32_t*>(drawCount.mapped)=1;
    auto indices=MakeBuffer(12,VK_BUFFER_USAGE_INDEX_BUFFER_BIT);for(uint32_t i=0;i<3;++i)static_cast<uint32_t*>(indices.mapped)[i]=i;
    auto readback=MakeBuffer(64,VK_BUFFER_USAGE_TRANSFER_DST_BIT);std::memset(readback.mapped,0,64);
    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};imageInfo.imageType=VK_IMAGE_TYPE_2D;imageInfo.format=VK_FORMAT_R8G8B8A8_UNORM;
    imageInfo.extent={4,4,1};imageInfo.mipLevels=1;imageInfo.arrayLayers=1;imageInfo.samples=VK_SAMPLE_COUNT_1_BIT;imageInfo.tiling=VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    VkImage image{};OK(DEV(vkCreateImage)(device,&imageInfo,nullptr,&image));VkMemoryRequirements req{};DEV(vkGetImageMemoryRequirements)(device,image,&req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=req.size;ai.memoryTypeIndex=MemoryType(req.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkDeviceMemory imageMemory{};OK(DEV(vkAllocateMemory)(device,&ai,nullptr,&imageMemory));OK(DEV(vkBindImageMemory)(device,image,imageMemory,0));
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};vi.image=image;vi.viewType=VK_IMAGE_VIEW_TYPE_2D;vi.format=imageInfo.format;vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    VkImageView view{};OK(DEV(vkCreateImageView)(device,&vi,nullptr,&view));
    VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};VkPipelineLayout layout{};OK(DEV(vkCreatePipelineLayout)(device,&li,nullptr,&layout));
    auto vertex=Shader(argv[2]),fragment=Shader(argv[3]);VkPipelineShaderStageCreateInfo stages[2]{};
    for(auto& stage:stages){stage.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;stage.pName="main";}
    stages[0].stage=VK_SHADER_STAGE_VERTEX_BIT;stages[0].module=vertex;stages[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT;stages[1].module=fragment;
    VkPipelineVertexInputStateCreateInfo vertexInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};assembly.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport viewport{0,0,4,4,0,1};VkRect2D scissor{{0,0},{4,4}};
    VkPipelineViewportStateCreateInfo viewportState{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};viewportState.viewportCount=1;viewportState.pViewports=&viewport;viewportState.scissorCount=1;viewportState.pScissors=&scissor;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};raster.polygonMode=VK_POLYGON_MODE_FILL;raster.lineWidth=1;
    VkPipelineMultisampleStateCreateInfo samples{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};samples.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState attachment{};attachment.colorWriteMask=15;
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};blend.attachmentCount=1;blend.pAttachments=&attachment;
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};rendering.colorAttachmentCount=1;rendering.pColorAttachmentFormats=&imageInfo.format;
    VkGraphicsPipelineCreateInfo pci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};pci.pNext=&rendering;pci.stageCount=2;pci.pStages=stages;pci.layout=layout;
    pci.pVertexInputState=&vertexInput;pci.pInputAssemblyState=&assembly;pci.pViewportState=&viewportState;pci.pRasterizationState=&raster;pci.pMultisampleState=&samples;pci.pColorBlendState=&blend;
    VkPipeline pipeline{};OK(DEV(vkCreateGraphicsPipelines)(device,VK_NULL_HANDLE,1,&pci,nullptr,&pipeline));
    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};poolInfo.queueFamilyIndex=family;VkCommandPool pool{};OK(DEV(vkCreateCommandPool)(device,&poolInfo,nullptr,&pool));
    VkCommandBufferAllocateInfo cbInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};cbInfo.commandPool=pool;cbInfo.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;cbInfo.commandBufferCount=1;
    VkCommandBuffer cb{};OK(DEV(vkAllocateCommandBuffers)(device,&cbInfo,&cb));VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};OK(DEV(vkBeginCommandBuffer)(cb,&begin));
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};barrier.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;barrier.newLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier.dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;barrier.image=image;barrier.subresourceRange=vi.subresourceRange;
    DEV(vkCmdPipelineBarrier)(cb,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,0,0,nullptr,0,nullptr,1,&barrier);
    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};color.imageView=view;color.imageLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;color.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR;color.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo render{VK_STRUCTURE_TYPE_RENDERING_INFO};render.renderArea=scissor;render.layerCount=1;render.colorAttachmentCount=1;render.pColorAttachments=&color;
    DEV(vkCmdBeginRendering)(cb,&render);DEV(vkCmdBindPipeline)(cb,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);DEV(vkCmdBindIndexBuffer)(cb,indices.buffer,0,VK_INDEX_TYPE_UINT32);
    // The current wrapper must clamp maxDrawCount=2 to the single command's
    // 20-byte capacity before calling the driver. GPU count itself remains 1.
    DEV(vkCmdDrawIndexedIndirectCount)(cb,arguments.buffer,0,drawCount.buffer,0,2,20);
    DEV(vkCmdEndRendering)(cb);
    barrier.oldLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;barrier.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
    DEV(vkCmdPipelineBarrier)(cb,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&barrier);
    VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={4,4,1};DEV(vkCmdCopyImageToBuffer)(cb,image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,readback.buffer,1,&copy);
    VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
    DEV(vkCmdPipelineBarrier)(cb,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&host,0,nullptr,0,nullptr);OK(DEV(vkEndCommandBuffer)(cb));
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};VkFence fence{};OK(DEV(vkCreateFence)(device,&fi,nullptr,&fence));
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&cb;OK(DEV(vkQueueSubmit)(queue,1,&submit,fence));OK(DEV(vkWaitForFences)(device,1,&fence,VK_TRUE,5000000000ull));
    auto pixels=static_cast<unsigned char*>(readback.mapped);bool valid=true;
    for(int i=0;i<16;++i)valid &= pixels[i*4]==255 && pixels[i*4+1]>=63 && pixels[i*4+1]<=65 && pixels[i*4+2]==0 && pixels[i*4+3]==255;
    printf("%s: guarded indexed indirect draw rendered 16 pixels; pixel=%u,%u,%u,%u resolver=%s\n",valid?"PASS":"FAIL",pixels[0],pixels[1],pixels[2],pixels[3],useGipa?"GIPA":"GDPA");
    DEV(vkDestroyFence)(device,fence,nullptr);DEV(vkDestroyCommandPool)(device,pool,nullptr);DEV(vkDestroyPipeline)(device,pipeline,nullptr);
    DEV(vkDestroyShaderModule)(device,vertex,nullptr);DEV(vkDestroyShaderModule)(device,fragment,nullptr);DEV(vkDestroyPipelineLayout)(device,layout,nullptr);
    DEV(vkDestroyImageView)(device,view,nullptr);DEV(vkDestroyImage)(device,image,nullptr);DEV(vkFreeMemory)(device,imageMemory,nullptr);
    for(auto b:{arguments,drawCount,indices,readback}){DEV(vkUnmapMemory)(device,b.memory);DEV(vkDestroyBuffer)(device,b.buffer,nullptr);DEV(vkFreeMemory)(device,b.memory,nullptr);}
    DEV(vkDestroyDevice)(device,nullptr);reinterpret_cast<PFN_vkDestroyInstance>(gip(instance,"vkDestroyInstance"))(instance,nullptr);
    return valid?0:1;
}
