#include "vk_host/vk_benchmark_timer.h"
#include <cassert>
#include <cstring>

static uint32_t validBits = 32;
static int resets = 0, writes = 0, destroyed = 0;
static bool available = true;

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties(VkPhysicalDevice, VkPhysicalDeviceProperties* p) {
    *p = {};
    std::strcpy(p->deviceName, "mock CPU");
    p->deviceType = VK_PHYSICAL_DEVICE_TYPE_CPU;
    p->limits.timestampPeriod = 1000000; // 1 ms/tick, to make conversion observable.
}
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceQueueFamilyProperties(VkPhysicalDevice, uint32_t* count, VkQueueFamilyProperties* p) {
    *count = 1;
    if (p) { *p = {}; p->timestampValidBits = validBits; }
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateQueryPool(VkDevice, const VkQueryPoolCreateInfo* ci, const VkAllocationCallbacks*, VkQueryPool* p) {
    assert(ci->queryCount == 2 && ci->queryType == VK_QUERY_TYPE_TIMESTAMP);
    *p = reinterpret_cast<VkQueryPool>(uintptr_t(1));
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkCmdResetQueryPool(VkCommandBuffer, VkQueryPool, uint32_t first, uint32_t count) {
    assert(first == 0 && count == 2);
    ++resets;
    writes = 0;
}
VKAPI_ATTR void VKAPI_CALL vkCmdWriteTimestamp(VkCommandBuffer, VkPipelineStageFlagBits stage, VkQueryPool, uint32_t index) {
    assert(resets && index == unsigned(writes++));
    assert(stage == (index == 0 ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT));
}
VKAPI_ATTR VkResult VKAPI_CALL vkGetQueryPoolResults(VkDevice, VkQueryPool, uint32_t, uint32_t count, size_t size, void* data, VkDeviceSize stride, VkQueryResultFlags flags) {
    assert(writes == 2 && count == 2 && size == 32 && stride == 16);
    assert(flags == (VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT));
    uint64_t values[4] = {0xfffffffe, 1, 3, available ? 1u : 0u};
    std::memcpy(data, values, sizeof(values));
    return available ? VK_SUCCESS : VK_NOT_READY;
}
VKAPI_ATTR void VKAPI_CALL vkDestroyQueryPool(VkDevice, VkQueryPool, const VkAllocationCallbacks*) { ++destroyed; }

int main() {
    assert(VulkanBenchmarkTimer::ticks(UINT64_MAX - 1, 3, 64) == 5);
    assert(VulkanBenchmarkTimer::ticks(254, 3, 8) == 5);
    VulkanBenchmarkTimer timer(VK_NULL_HANDLE, VK_NULL_HANDLE, 0);
    for (int i = 0; i < 2; ++i) {
        timer.begin(VK_NULL_HANDLE); timer.end(VK_NULL_HANDLE); timer.collect();
    }
    timer.report(2, "frame"); // expected 5 ms/frame, including 32-bit wrap.
    available = false;
    timer.begin(VK_NULL_HANDLE); timer.end(VK_NULL_HANDLE); timer.collect();
    timer.report(1, "frame"); // must be unavailable, not the partial total.
    timer.close();
    assert(destroyed == 1);
    validBits = 0;
    VulkanBenchmarkTimer unsupported(VK_NULL_HANDLE, VK_NULL_HANDLE, 0);
    unsupported.begin(VK_NULL_HANDLE); unsupported.end(VK_NULL_HANDLE); unsupported.collect();
    unsupported.report(1, "frame");
    unsupported.close();
    assert(resets == 3 && destroyed == 1);
}
