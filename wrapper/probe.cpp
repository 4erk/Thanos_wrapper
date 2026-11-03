#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <vulkan/vulkan.h>
#include <cstdio>
#include <cstring>
#include <vector>

int wmain(int argc, wchar_t** argv) {
    wchar_t path[MAX_PATH];
    GetSystemDirectoryW(path, MAX_PATH);
    wcscat_s(path, L"\\vulkan-1.dll");
    const bool wrapper = argc > 1;
    HMODULE lib=LoadLibraryW(wrapper ? argv[1] : path);
    if (!lib) return 1;
    auto gip=(PFN_vkGetInstanceProcAddr)GetProcAddress(lib,"vkGetInstanceProcAddr");
    if (!gip) return 3;
    auto create=(PFN_vkCreateInstance)gip(nullptr,"vkCreateInstance");
    VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO}; ai.apiVersion=VK_API_VERSION_1_3;
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ci.pApplicationInfo=&ai;
    VkInstance instance{}; auto r=create(&ci,nullptr,&instance);
    if(r!=VK_SUCCESS) {printf("create instance %d\n",r);return 2;}
    auto enumerate=(PFN_vkEnumeratePhysicalDevices)gip(instance,"vkEnumeratePhysicalDevices");
    uint32_t count=0; enumerate(instance,&count,nullptr); std::vector<VkPhysicalDevice> ps(count);
    enumerate(instance,&count,ps.data());
    auto props=(PFN_vkGetPhysicalDeviceProperties)gip(instance,"vkGetPhysicalDeviceProperties");
    auto features=(PFN_vkGetPhysicalDeviceFeatures2)gip(instance,"vkGetPhysicalDeviceFeatures2");
    auto exts=(PFN_vkEnumerateDeviceExtensionProperties)gip(instance,"vkEnumerateDeviceExtensionProperties");
    uint32_t tested=0;
    for(auto p:ps) {
        VkPhysicalDeviceProperties pr{};props(p,&pr);printf("GPU: %s\n",pr.deviceName);
        auto formatProps=(PFN_vkGetPhysicalDeviceFormatProperties2)gip(instance,"vkGetPhysicalDeviceFormatProperties2");
        VkFormatProperties2 format{VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2};
        formatProps(p,VK_FORMAT_R16G16B16A16_UNORM,&format);
        printf("R16G16B16A16_UNORM AS vertex support=%u (buffer flags=%08x)\n",
            !!(format.formatProperties.bufferFeatures & VK_FORMAT_FEATURE_ACCELERATION_STRUCTURE_VERTEX_BUFFER_BIT_KHR),
            format.formatProperties.bufferFeatures);
        VkPhysicalDeviceFeatures2 f{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        VkPhysicalDeviceVulkan12Features v12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        VkPhysicalDeviceVulkan13Features v13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        VkPhysicalDeviceAccelerationStructureFeaturesKHR as{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
        VkPhysicalDeviceRayTracingPipelineFeaturesKHR rt{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR};
        VkPhysicalDeviceRayQueryFeaturesKHR rq{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
        f.pNext=&v12;v12.pNext=&v13;v13.pNext=&as;as.pNext=&rt;rt.pNext=&rq;features(p,&f);
        printf("Float16=%u BDA=%u sync2=%u rendering=%u AS=%u RTpipeline=%u rayQuery=%u\n",v12.shaderFloat16,v12.bufferDeviceAddress,v13.synchronization2,v13.dynamicRendering,as.accelerationStructure,rt.rayTracingPipeline,rq.rayQuery);
        uint32_t n=0;exts(p,nullptr,&n,nullptr);std::vector<VkExtensionProperties> ep(n);exts(p,nullptr,&n,ep.data());
        ep.resize(n);
        if (wrapper) {
            uint32_t expected = 0;
            if (exts(p, nullptr, &expected, nullptr) != VK_SUCCESS || expected != n) {
                puts("FAIL: extension count query disagrees with returned array"); return 10;
            }
            VkExtensionProperties shortList[2]{};
            std::memset(&shortList[1], 0x5a, sizeof(shortList[1]));
            uint32_t capacity = 1;
            if (n > 1 && (exts(p, nullptr, &capacity, shortList) != VK_INCOMPLETE || capacity != 1)) {
                puts("FAIL: truncated enumeration count/result"); return 11;
            }
            const auto* sentinel = reinterpret_cast<const unsigned char*>(&shortList[1]);
            for (size_t i = 0; i < sizeof(shortList[1]); ++i)
                if (sentinel[i] != 0x5a) { puts("FAIL: enumeration overflow"); return 12; }
            capacity = 0;
            if (n && (exts(p, nullptr, &capacity, shortList) != VK_INCOMPLETE || capacity)) {
                puts("FAIL: zero-capacity enumeration"); return 13;
            }
            puts("PASS: exact and bounded extension enumeration");
        }
        auto hasExtension=[&](const char* name) {for(const auto& e:ep) if(!strcmp(e.extensionName,name)) return true; return false;};
        for(auto e:ep) if(strstr(e.extensionName,"ray_")||strstr(e.extensionName,"acceleration")||strstr(e.extensionName,"deferred")||strstr(e.extensionName,"pipeline_library")||strstr(e.extensionName,"mesh_shader")) printf("%s\n",e.extensionName);
        printf("NV mesh shader extension=%d\n",hasExtension(VK_NV_MESH_SHADER_EXTENSION_NAME));
        if (wrapper && rq.rayQuery && !hasExtension(VK_KHR_RAY_QUERY_EXTENSION_NAME)) {
            puts("FAIL: feature and extension enumeration disagree"); return 9;
        }
        if (!as.accelerationStructure || !v12.bufferDeviceAddress) continue;
        auto queues=(PFN_vkGetPhysicalDeviceQueueFamilyProperties)gip(instance,"vkGetPhysicalDeviceQueueFamilyProperties");
        uint32_t nq=0; queues(p,&nq,nullptr); std::vector<VkQueueFamilyProperties> qp(nq);
        queues(p,&nq,qp.data()); uint32_t qi=0;
        while (qi<nq && !(qp[qi].queueFlags & VK_QUEUE_COMPUTE_BIT)) ++qi;
        if (qi==nq) return 4;
        float priority=1.0f;
        VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qci.queueFamilyIndex=qi; qci.queueCount=1; qci.pQueuePriorities=&priority;
        VkPhysicalDeviceVulkan12Features enabled12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        enabled12.bufferDeviceAddress=VK_TRUE;
        VkPhysicalDeviceVulkan13Features enabled13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        enabled13.synchronization2=v13.synchronization2;
        enabled13.dynamicRendering=v13.dynamicRendering;
        enabled12.pNext=&enabled13;
        VkPhysicalDeviceAccelerationStructureFeaturesKHR enabledAS{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
        enabledAS.accelerationStructure=VK_TRUE;
        const char* extensions[]={VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME};
        VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        dci.pNext=&enabled12; dci.queueCreateInfoCount=1; dci.pQueueCreateInfos=&qci;
        // Reproduce the game's omission: the proxy must enable native AS.
        if (!wrapper) { enabled13.pNext=&enabledAS; dci.enabledExtensionCount=2; dci.ppEnabledExtensionNames=extensions; }
        VkDevice device{};
        r=((PFN_vkCreateDevice)gip(instance,"vkCreateDevice"))(p,&dci,nullptr,&device);
        printf("Device creation: %d\n",r); if(r!=VK_SUCCESS) return 5;
        auto gdp=(PFN_vkGetDeviceProcAddr)gip(instance,"vkGetDeviceProcAddr");
        auto nested=(PFN_vkGetDeviceProcAddr)gdp(device,"vkGetDeviceProcAddr");
        auto exported=(PFN_vkGetDeviceProcAddr)GetProcAddress(lib,"vkGetDeviceProcAddr");
        if (!nested || (wrapper && nested!=gdp)) {puts("FAIL: resolver escaped proxy");return 6;}
        const char* command="vkGetAccelerationStructureBuildSizesKHR";
        auto sizes=(PFN_vkGetAccelerationStructureBuildSizesKHR)nested(device,command);
        auto direct=(PFN_vkGetAccelerationStructureBuildSizesKHR)exported(device,command);
        printf("Build-sizes pointers: nested=%p export=%p\n",(void*)sizes,(void*)direct);
        if (!sizes || !direct || (wrapper && sizes!=direct)) return 7;
        VkAccelerationStructureGeometryKHR geometry{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
        geometry.geometryType=VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        geometry.geometry.triangles.sType=VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
        geometry.geometry.triangles.vertexFormat=VK_FORMAT_R32G32B32_SFLOAT;
        geometry.geometry.triangles.vertexStride=12;
        geometry.geometry.triangles.maxVertex=3;
        geometry.geometry.triangles.indexType=VK_INDEX_TYPE_UINT32;
        VkAccelerationStructureBuildGeometryInfoKHR bi{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
        bi.type=VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        bi.mode=VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        bi.geometryCount=1; bi.pGeometries=&geometry;
        uint32_t primitives=1;
        VkAccelerationStructureBuildSizesInfoKHR result{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
        sizes(device,VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,&bi,&primitives,&result);
        printf("AS size=%llu scratch=%llu\n",(unsigned long long)result.accelerationStructureSize,(unsigned long long)result.buildScratchSize);
        if (!result.accelerationStructureSize) return 8;
        ((PFN_vkDestroyDevice)nested(device,"vkDestroyDevice"))(device,nullptr);
        ++tested;
    }
    ((PFN_vkDestroyInstance)gip(instance,"vkDestroyInstance"))(instance,nullptr);
    if (!tested) {puts("SKIP: no GPU with native AS/BDA was tested");return 77;}
    puts("PASS: resolver and native AS size query (not a DOOM compatibility test)");
}
