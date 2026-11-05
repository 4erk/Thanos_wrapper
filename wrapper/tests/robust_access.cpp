#include "../src/vk/RobustAccess.h"
#include <cstdio>
#define CHECK(x) do { if (!(x)) { std::printf("FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)
using namespace thanosray::vk::robustness;
int main()
{
    VkPhysicalDeviceFeatures native{}; native.robustBufferAccess=VK_TRUE;
    VkPhysicalDeviceRobustness2FeaturesEXT extra{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT};
    extra.robustBufferAccess2=extra.robustImageAccess2=extra.nullDescriptor=VK_TRUE;
    // The legacy pEnabledFeatures path must preserve all unrelated requests.
    VkPhysicalDeviceFeatures original{}; original.shaderInt64=VK_TRUE;
    VkDeviceCreateInfo ci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; ci.pEnabledFeatures=&original;
    Storage a;
    auto enabled=Apply(ci,a,native,extra,true,true);
    CHECK(enabled.buffer && enabled.buffer2 && enabled.image2 && enabled.nullDescriptor);
    CHECK(!original.robustBufferAccess && original.shaderInt64);
    CHECK(ci.pEnabledFeatures!=&original && ci.pEnabledFeatures->shaderInt64);
    CHECK(ci.pNext==&a.extra && a.extra.pNext==nullptr);
    // With Features2, reuse an existing robustness node instead of duplicating it.
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceRobustness2FeaturesEXT existing{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT};
    f2.pNext=&f12; f12.pNext=&existing; f2.features.shaderInt64=VK_TRUE;
    ci.pEnabledFeatures=nullptr; ci.pNext=&f2; Storage b;
    enabled=Apply(ci,b,native,extra,true,true);
    CHECK(ci.pNext==&f2 && f12.pNext==&existing && !existing.pNext && !ci.pEnabledFeatures);
    CHECK(f2.features.shaderInt64 && f2.features.robustBufferAccess && existing.robustBufferAccess2);
    // Do not enable incompatible buffer robustness for update-after-bind.
    f2.features.robustBufferAccess=VK_FALSE; existing.robustBufferAccess2=VK_FALSE;
    f12.descriptorBindingStorageBufferUpdateAfterBind=VK_TRUE; Storage c;
    enabled=Apply(ci,c,native,extra,true,false);
    CHECK(!enabled.buffer && !enabled.buffer2 && enabled.nullDescriptor && enabled.image2);
    // Unsupported extension/features must not be invented.
    ci.pNext=nullptr; ci.pEnabledFeatures=nullptr; native={}; Storage d;
    enabled=Apply(ci,d,native,extra,false,false);
    CHECK(!ci.pNext && !enabled.buffer && !enabled.buffer2 && !enabled.needsExtension);
    puts("PASS: feature lifetimes, original preservation, Features2 reuse, update-after-bind compatibility and unsupported fallback");
}
