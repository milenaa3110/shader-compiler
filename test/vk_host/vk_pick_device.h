// vk_pick_device.h — choose a Vulkan physical device, preferring a real GPU
// (discrete > integrated > virtual) over a CPU software renderer such as
// LavaPipe / llvmpipe.
//
// The hosts used to take physDevs[0] — the first device the loader enumerates,
// which on a headless box is usually LavaPipe. For GPU benchmarking we want
// actual graphics hardware; the CPU renderer is only a last-resort fallback.
//
// Override with VK_DEVICE_INDEX=<n> to force a specific enumerated device
// (e.g. VK_DEVICE_INDEX=0 to deliberately benchmark LavaPipe again).
#pragma once

#include <vulkan/vulkan.h>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace vkpick {

// Higher = more preferred. Real GPUs outrank the CPU software renderer.
inline int typeRank(VkPhysicalDeviceType t) {
    switch (t) {
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:   return 4;
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return 3;
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:    return 2;
        case VK_PHYSICAL_DEVICE_TYPE_CPU:            return 1;  // LavaPipe / llvmpipe
        default:                                     return 0;
    }
}

// Pick the highest-ranked device. VK_DEVICE_INDEX, if set and in range, wins.
inline VkPhysicalDevice best(const std::vector<VkPhysicalDevice>& devs) {
    if (devs.empty()) return VK_NULL_HANDLE;

    if (const char* env = std::getenv("VK_DEVICE_INDEX")) {
        int i = std::atoi(env);
        if (i >= 0 && i < static_cast<int>(devs.size())) return devs[i];
    }

    VkPhysicalDevice chosen = devs[0];
    int bestScore = -1;
    for (VkPhysicalDevice d : devs) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(d, &p);
        int s = typeRank(p.deviceType);
        if (s > bestScore) { bestScore = s; chosen = d; }
    }

    // Warn loudly when the only option is the CPU renderer — a benchmark run
    // there is software, not GPU, and the numbers must not be read as GPU.
    VkPhysicalDeviceProperties cp;
    vkGetPhysicalDeviceProperties(chosen, &cp);
    if (cp.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU)
        std::fprintf(stderr,
            "[vk] WARNING: no GPU found; falling back to CPU renderer '%s' "
            "(LavaPipe). These are NOT GPU numbers.\n", cp.deviceName);
    return chosen;
}

}  // namespace vkpick
