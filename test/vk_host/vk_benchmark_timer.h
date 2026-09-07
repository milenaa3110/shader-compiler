#pragma once
#include <vulkan/vulkan.h>
#include <cstdio>
#include <vector>
#include <cstdint>

// A pair is reset and written outside render passes, then read after the fence.
// Call close before destroying the device. Unsupported/failed queries never
// substitute CPU wall time for device time.
class VulkanBenchmarkTimer {
    VkDevice device;
    VkQueryPool pool = VK_NULL_HANDLE;
    uint32_t bits = 0;
    float period = 0;
    double total = 0;
    bool failed = false;
public:
    static uint64_t ticks(uint64_t begin, uint64_t end, uint32_t validBits) {
        return (end - begin) & (validBits >= 64 ? UINT64_MAX : ((uint64_t(1) << validBits) - 1));
    }
    VulkanBenchmarkTimer(VkPhysicalDevice physical, VkDevice dev, uint32_t family) : device(dev) {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(physical, &props);
        const char* type = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU ? "CPU" :
            props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? "discrete GPU" :
            props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? "integrated GPU" : "other";
        std::printf("Vulkan device: %s (type=%s)\n", props.deviceName, type);
        uint32_t count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
        std::vector<VkQueueFamilyProperties> families(count);
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
        bits = families.at(family).timestampValidBits;
        period = props.limits.timestampPeriod;
        if (!bits || period <= 0) return;
        VkQueryPoolCreateInfo ci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        ci.queryType = VK_QUERY_TYPE_TIMESTAMP;
        ci.queryCount = 2;
        if (vkCreateQueryPool(device, &ci, nullptr, &pool) != VK_SUCCESS) failed = true;
    }
    void begin(VkCommandBuffer cmd) {
        if (!pool) return;
        vkCmdResetQueryPool(cmd, pool, 0, 2);
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, pool, 0);
    }
    void end(VkCommandBuffer cmd) {
        if (pool) vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, pool, 1);
    }
    void collect() {
        if (!pool) return;
        uint64_t values[4]{};
        VkResult result = vkGetQueryPoolResults(device, pool, 0, 2, sizeof(values), values,
            2 * sizeof(uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
        if (result != VK_SUCCESS || !values[1] || !values[3]) { failed = true; return; }
        total += double(ticks(values[0], values[2], bits)) * period / 1e6;
    }
    void clear() { total = 0; }
    void report(int units, const char* unit) const {
        if (!pool || failed) std::printf("Vulkan device avg: unavailable (timestamp queries unsupported or failed)\n");
        else std::printf("Vulkan device avg: %.9f ms/%s\n", total / units, unit);
    }
    void close() { if (pool) vkDestroyQueryPool(device, pool, nullptr); pool = VK_NULL_HANDLE; }
};
