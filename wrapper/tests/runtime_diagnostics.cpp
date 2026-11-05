#include "../src/vk/RuntimeDiagnostics.h"
#include <cstdio>
#include <cstring>

#define CHECK(x) do { if (!(x)) { std::printf("FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)
using namespace thanosray::vk::runtime;
static VkResult nextResult = VK_SUCCESS;
static bool argsCorrect = true;
static int faultCalls = 0;
static const auto device = reinterpret_cast<VkDevice>(uintptr_t(0x100));
static const auto pool = reinterpret_cast<VkQueryPool>(uintptr_t(0x200));
static uint64_t output[4]{};

static VkResult VKAPI_CALL Query(VkDevice d, VkQueryPool p, uint32_t first, uint32_t count,
    size_t size, void* data, VkDeviceSize stride, VkQueryResultFlags flags)
{
    argsCorrect &= d == device && p == pool && first == 3 && count == 4 && size == sizeof(output) &&
        data == output && stride == 8 && flags == VK_QUERY_RESULT_64_BIT;
    if (nextResult == VK_SUCCESS) std::memset(data, 0x42, size);
    return nextResult;
}
static VkResult VKAPI_CALL Fault(VkDevice d, VkDeviceFaultCountsEXT* counts, VkDeviceFaultInfoEXT* info)
{
    argsCorrect &= d == device;
    ++faultCalls;
    if (!info) { counts->addressInfoCount = 100; counts->vendorInfoCount = 100; return VK_SUCCESS; }
    argsCorrect &= counts->addressInfoCount == 64 && counts->vendorInfoCount == 64 && counts->vendorBinarySize == 0;
    std::strcpy(info->description, "synthetic device fault for pass-through regression test");
    for (uint32_t i = 0; i < counts->addressInfoCount; ++i) info->pAddressInfos[i] = {};
    for (uint32_t i = 0; i < counts->vendorInfoCount; ++i) info->pVendorInfos[i] = {};
    return VK_INCOMPLETE;
}
static PFN_vkVoidFunction VKAPI_CALL Resolve(VkDevice, const char* name)
{ return !std::strcmp(name, "vkGetDeviceFaultInfoEXT") ? reinterpret_cast<PFN_vkVoidFunction>(&Fault) : nullptr; }
static void VKAPI_CALL Destroy(VkDevice, const VkAllocationCallbacks*) {}

int main()
{
    realDestroyDevice = &Destroy;
    auto wrapped = reinterpret_cast<PFN_vkGetQueryPoolResults>(Wrap("vkGetQueryPoolResults", reinterpret_cast<PFN_vkVoidFunction>(&Query)));
    CHECK(wrapped && wrapped != &Query);
    CHECK(Wrap("vkUnknown", nullptr) == nullptr);
    OnDeviceCreated(device, &Resolve, true);
    for (auto result : {VK_SUCCESS, VK_NOT_READY, VK_ERROR_DEVICE_LOST, VK_ERROR_DEVICE_LOST, VK_ERROR_OUT_OF_HOST_MEMORY}) {
        nextResult = result;
        std::memset(output, 0xa5, sizeof(output));
        CHECK(wrapped(device, pool, 3, 4, sizeof(output), output, 8, VK_QUERY_RESULT_64_BIT) == result);
        for (auto value : output) CHECK(value == (result == VK_SUCCESS ? 0x4242424242424242ull : 0xa5a5a5a5a5a5a5a5ull));
    }
    CHECK(argsCorrect);
    CHECK(faultCalls == (Enabled() ? 2 : 0));
    DestroyDevice(device, nullptr);
    OnDeviceCreated(device, &Resolve, true);
    nextResult = VK_ERROR_DEVICE_LOST;
    CHECK(wrapped(device, pool, 3, 4, sizeof(output), output, 8, VK_QUERY_RESULT_64_BIT) == nextResult);
    CHECK(faultCalls == (Enabled() ? 4 : 0));
    const auto started = GetTickCount64();
    for (int i=0;i<6;++i)
        CHECK(wrapped(device,pool,3,4,sizeof(output),output,8,VK_QUERY_RESULT_64_BIT)==VK_ERROR_DEVICE_LOST);
    CHECK(GetTickCount64()-started >= 50);
    DestroyDevice(device, nullptr);
    std::printf("PASS: query arguments/data/results preserved; bounded once-per-device fault capture; RuntimeTrace=%d\n", Enabled());
}
