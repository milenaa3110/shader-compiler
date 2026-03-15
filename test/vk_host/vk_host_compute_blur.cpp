// vk_host_compute_blur.cpp — Vulkan compute pipeline benchmark host.
// Runs a SPIR-V compute shader (Gaussian blur) on image data stored in
// storage buffers.  No graphics pipeline, no render pass, no framebuffer.
//
// GPU advantage: all workgroups dispatched simultaneously to independent
// shader units.  CPU comparison: same computation with OpenMP loops.
//
// Usage:
//   ./spirv_vulkan_compute_host result/blur.comp.spv blur [nruns=100]
//   Reads a synthetic noise image W×H, applies blur nruns times,
//   writes result/blur_gpu.ppm and prints avg ms/run.
//
// Build: g++ -std=c++20 -O2 -o build/spirv/spirv_vulkan_compute_host \
//            test/vk_host/vk_host_compute_blur.cpp -lvulkan

#include <vulkan/vulkan.h>
#include <vector>
#include <fstream>
#include <iostream>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <cmath>
#include <chrono>
#include <sys/stat.h>

static int W = 512, H = 512, NRUNS = 100;

static void check(VkResult r, const char* where) {
    if (r != VK_SUCCESS) { std::cerr << "Vulkan error " << r << " at " << where << "\n"; std::exit(1); }
}
#define VK(expr) check((expr), #expr)

static std::vector<uint32_t> readSpv(const char* path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) { std::cerr << "Cannot open " << path << "\n"; std::exit(1); }
    size_t sz = f.tellg(); f.seekg(0);
    std::vector<uint32_t> data(sz / 4);
    f.read(reinterpret_cast<char*>(data.data()), sz);
    return data;
}

// Find a memory type that satisfies the requirements
static uint32_t findMemType(VkPhysicalDevice pd, uint32_t typeBits, VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((typeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & props) == props)
            return i;
    std::cerr << "No suitable memory type\n"; std::exit(1);
}

// Create a host-visible storage buffer
static void makeBuffer(VkDevice dev, VkPhysicalDevice pd, VkDeviceSize sz,
                       VkBufferUsageFlags usage, VkMemoryPropertyFlags props,
                       VkBuffer& buf, VkDeviceMemory& mem) {
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = sz; bci.usage = usage;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VK(vkCreateBuffer(dev, &bci, nullptr, &buf));

    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(dev, buf, &mr);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = findMemType(pd, mr.memoryTypeBits, props);
    VK(vkAllocateMemory(dev, &mai, nullptr, &mem));
    VK(vkBindBufferMemory(dev, buf, mem, 0));
}

static void writePPM(const char* path, int w, int h, const float* rgba) {
    std::ofstream f(path, std::ios::binary);
    f << "P6\n" << w << " " << h << "\n255\n";
    for (int i = 0; i < w * h; i++) {
        auto clamp01 = [](float v){ return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); };
        uint8_t rgb[3] = {
            (uint8_t)(clamp01(rgba[i*4+0]) * 255.f),
            (uint8_t)(clamp01(rgba[i*4+1]) * 255.f),
            (uint8_t)(clamp01(rgba[i*4+2]) * 255.f),
        };
        f.write((char*)rgb, 3);
    }
}

int main(int argc, char** argv) {
    const char* compSpv  = (argc > 1) ? argv[1] : "result/blur.comp.spv";
    const char* outName  = (argc > 2) ? argv[2] : "blur";
    if (argc > 3) NRUNS  = std::atoi(argv[3]);

    mkdir("result", 0755);

    // ── Instance ──────────────────────────────────────────────────────────────
    VkApplicationInfo appInfo{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    appInfo.pApplicationName = "compute_bench";
    appInfo.apiVersion = VK_API_VERSION_1_1;

    VkInstanceCreateInfo instCI{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instCI.pApplicationInfo = &appInfo;

    VkInstance instance;
    VK(vkCreateInstance(&instCI, nullptr, &instance));

    uint32_t devCount = 0;
    vkEnumeratePhysicalDevices(instance, &devCount, nullptr);
    if (!devCount) { std::cerr << "No Vulkan device.\n"; return 1; }
    std::vector<VkPhysicalDevice> physDevs(devCount);
    vkEnumeratePhysicalDevices(instance, &devCount, physDevs.data());
    VkPhysicalDevice pd = physDevs[0];

    VkPhysicalDeviceProperties pdProps;
    vkGetPhysicalDeviceProperties(pd, &pdProps);
    std::cout << "[compute] GPU: " << pdProps.deviceName << "\n";

    // ── Queue family (prefer compute-capable) ─────────────────────────────────
    uint32_t qfCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &qfCount, nullptr);
    std::vector<VkQueueFamilyProperties> qfs(qfCount);
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &qfCount, qfs.data());
    uint32_t qfi = 0;
    for (uint32_t i = 0; i < qfCount; i++)
        if (qfs[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { qfi = i; break; }

    float qprio = 1.f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = qfi; qci.queueCount = 1; qci.pQueuePriorities = &qprio;

    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
    VkDevice dev;
    VK(vkCreateDevice(pd, &dci, nullptr, &dev));

    VkQueue queue;
    vkGetDeviceQueue(dev, qfi, 0, &queue);

    // ── Buffers: inBuf, outBuf (host-visible for easy read/write) ─────────────
    VkDeviceSize bufSize = (VkDeviceSize)W * H * 4 * sizeof(float);
    VkBuffer inBuf, outBuf;
    VkDeviceMemory inMem, outMem;

    makeBuffer(dev, pd, bufSize,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        inBuf, inMem);
    makeBuffer(dev, pd, bufSize,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        outBuf, outMem);

    // Fill input with procedural noise
    {
        float* ptr;
        VK(vkMapMemory(dev, inMem, 0, bufSize, 0, (void**)&ptr));
        for (int y = 0; y < H; y++) {
            for (int x = 0; x < W; x++) {
                int i = (y * W + x) * 4;
                float u = (float)x / W, v = (float)y / H;
                float n = 0.5f + 0.5f * sinf(u * 31.4f + v * 25.1f) *
                                        cosf(v * 18.8f - u * 12.6f);
                ptr[i+0] = n;
                ptr[i+1] = 0.5f + 0.5f * sinf(u * 17.3f + v * 22.7f);
                ptr[i+2] = 0.5f + 0.5f * cosf(u * 28.2f - v * 15.4f);
                ptr[i+3] = 1.f;
            }
        }
        vkUnmapMemory(dev, inMem);
    }

    // ── Descriptor set layout (binding 0=inBuf, binding 1=outBuf) ─────────────
    VkDescriptorSetLayoutBinding bindings[2]{};
    bindings[0].binding = 0; bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[0].descriptorCount = 1; bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[1] = bindings[0]; bindings[1].binding = 1;

    VkDescriptorSetLayoutCreateInfo dslci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dslci.bindingCount = 2; dslci.pBindings = bindings;
    VkDescriptorSetLayout dsl;
    VK(vkCreateDescriptorSetLayout(dev, &dslci, nullptr, &dsl));

    // ── Push constant: width + height ─────────────────────────────────────────
    VkPushConstantRange pcr{};
    pcr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT; pcr.size = 8;

    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 1; plci.pSetLayouts = &dsl;
    plci.pushConstantRangeCount = 1; plci.pPushConstantRanges = &pcr;
    VkPipelineLayout pipeLayout;
    VK(vkCreatePipelineLayout(dev, &plci, nullptr, &pipeLayout));

    // ── Compute shader module ─────────────────────────────────────────────────
    auto spvData = readSpv(compSpv);
    VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smci.codeSize = spvData.size() * 4; smci.pCode = spvData.data();
    VkShaderModule shaderModule;
    VK(vkCreateShaderModule(dev, &smci, nullptr, &shaderModule));

    // ── Compute pipeline ──────────────────────────────────────────────────────
    VkComputePipelineCreateInfo cpci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpci.stage.module = shaderModule;
    cpci.stage.pName = "main";
    cpci.layout = pipeLayout;
    VkPipeline pipeline;
    VK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, nullptr, &pipeline));

    // ── Descriptor pool + set ─────────────────────────────────────────────────
    VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2};
    VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpci.maxSets = 1; dpci.poolSizeCount = 1; dpci.pPoolSizes = &poolSize;
    VkDescriptorPool pool;
    VK(vkCreateDescriptorPool(dev, &dpci, nullptr, &pool));

    VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dsai.descriptorPool = pool; dsai.descriptorSetCount = 1; dsai.pSetLayouts = &dsl;
    VkDescriptorSet ds;
    VK(vkAllocateDescriptorSets(dev, &dsai, &ds));

    VkDescriptorBufferInfo dbi[2]{};
    dbi[0].buffer = inBuf;  dbi[0].range = VK_WHOLE_SIZE;
    dbi[1].buffer = outBuf; dbi[1].range = VK_WHOLE_SIZE;
    VkWriteDescriptorSet writes[2]{};
    for (int i = 0; i < 2; i++) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = ds; writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].pBufferInfo = &dbi[i];
    }
    vkUpdateDescriptorSets(dev, 2, writes, 0, nullptr);

    // ── Command pool + buffer ─────────────────────────────────────────────────
    VkCommandPoolCreateInfo cpoolCI{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpoolCI.queueFamilyIndex = qfi;
    cpoolCI.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VkCommandPool cmdPool;
    VK(vkCreateCommandPool(dev, &cpoolCI, nullptr, &cmdPool));

    VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbai.commandPool = cmdPool; cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    VkCommandBuffer cmd;
    VK(vkAllocateCommandBuffers(dev, &cbai, &cmd));

    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence;
    VK(vkCreateFence(dev, &fci, nullptr, &fence));

    uint32_t pushData[2] = { (uint32_t)W, (uint32_t)H };
    uint32_t gx = (W + 15) / 16, gy = (H + 15) / 16;

    // ── Warmup dispatch ───────────────────────────────────────────────────────
    {
        VkCommandBufferBeginInfo cbbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        vkBeginCommandBuffer(cmd, &cbbi);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeLayout, 0, 1, &ds, 0, nullptr);
        vkCmdPushConstants(cmd, pipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 8, pushData);
        vkCmdDispatch(cmd, gx, gy, 1);
        vkEndCommandBuffer(cmd);

        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
        VK(vkQueueSubmit(queue, 1, &si, fence));
        VK(vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX));
        vkResetFences(dev, 1, &fence);
        vkResetCommandBuffer(cmd, 0);
    }

    // ── Timed runs ────────────────────────────────────────────────────────────
    std::cout << "[" << outName << "] GPU compute: " << W << "x" << H
              << " Gaussian blur, " << NRUNS << " runs\n";

    double total_ms = 0.0;
    for (int run = 0; run < NRUNS; run++) {
        VkCommandBufferBeginInfo cbbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        vkBeginCommandBuffer(cmd, &cbbi);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeLayout, 0, 1, &ds, 0, nullptr);
        vkCmdPushConstants(cmd, pipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 8, pushData);
        vkCmdDispatch(cmd, gx, gy, 1);
        vkEndCommandBuffer(cmd);

        auto t0 = std::chrono::high_resolution_clock::now();
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
        VK(vkQueueSubmit(queue, 1, &si, fence));
        VK(vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX));
        auto t1 = std::chrono::high_resolution_clock::now();

        total_ms += std::chrono::duration<double, std::milli>(t1 - t0).count();
        vkResetFences(dev, 1, &fence);
        vkResetCommandBuffer(cmd, 0);
    }

    double avg = total_ms / NRUNS;
    std::cout << "[" << outName << "] GPU avg: " << avg << " ms/run  ("
              << (1000.0 / avg) << " runs/s)\n";
    std::cout << "[" << outName << "] Throughput: "
              << (W * H / avg / 1000.0) << " Mpixels/ms\n";

    // ── Read back and save PPM ────────────────────────────────────────────────
    {
        float* ptr;
        VK(vkMapMemory(dev, outMem, 0, bufSize, 0, (void**)&ptr));
        char path[256];
        std::snprintf(path, sizeof(path), "result/%s_gpu.ppm", outName);
        writePPM(path, W, H, ptr);
        std::cout << "[" << outName << "] Output: " << path << "\n";
        vkUnmapMemory(dev, outMem);
    }

    // Cleanup
    vkDestroyFence(dev, fence, nullptr);
    vkDestroyCommandPool(dev, cmdPool, nullptr);
    vkDestroyDescriptorPool(dev, pool, nullptr);
    vkDestroyPipeline(dev, pipeline, nullptr);
    vkDestroyShaderModule(dev, shaderModule, nullptr);
    vkDestroyPipelineLayout(dev, pipeLayout, nullptr);
    vkDestroyDescriptorSetLayout(dev, dsl, nullptr);
    vkDestroyBuffer(dev, inBuf, nullptr); vkFreeMemory(dev, inMem, nullptr);
    vkDestroyBuffer(dev, outBuf, nullptr); vkFreeMemory(dev, outMem, nullptr);
    vkDestroyDevice(dev, nullptr);
    vkDestroyInstance(instance, nullptr);
    return 0;
}
