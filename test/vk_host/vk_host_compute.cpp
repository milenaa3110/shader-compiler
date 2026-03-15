// vk_host_compute.cpp — Conway's Game of Life multi-pass Vulkan benchmark.
//
// Demonstrates multi-pass dependency cost on GPU:
//   Each generation requires vkQueueSubmit + vkWaitForFences — fixed overhead per pass.
//   For NGENERATIONS passes: total = NGENERATIONS × (dispatch_overhead + compute_time).
//
// Compare with CPU (life_host.cpp): zero dispatch overhead, cache-friendly sequential loop.
//
// Usage: ./spirv_vulkan_life_host result/life.comp.spv [NGENERATIONS=1000] [GRID=256]
//
// Build: g++ -std=c++20 -O2 test/vk_host/vk_host_compute.cpp -o build/spirv/spirv_vulkan_life_host -lvulkan

#include <vulkan/vulkan.h>
#include <vector>
#include <iostream>
#include <fstream>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <cmath>
#include <chrono>
#include <random>
#include <sys/stat.h>

static void check(VkResult r, const char* w) {
    if (r != VK_SUCCESS) { std::cerr << "Vulkan error " << r << " at " << w << "\n"; std::exit(1); }
}
#define VK(e) check((e), #e)

static std::vector<uint32_t> readSpv(const char* path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) { std::cerr << "Cannot open " << path << "\n"; std::exit(1); }
    size_t sz = f.tellg(); f.seekg(0);
    std::vector<uint32_t> data(sz / 4);
    f.read(reinterpret_cast<char*>(data.data()), sz);
    return data;
}

int main(int argc, char** argv) {
    const char* spvPath  = (argc > 1) ? argv[1] : "result/life.comp.spv";
    int NGENERATIONS     = (argc > 2) ? std::atoi(argv[2]) : 1000;
    int GRID             = (argc > 3) ? std::atoi(argv[3]) : 256;
    int SNAP_EVERY       = (argc > 4) ? std::atoi(argv[4]) : 0;  // 0 = no animation

    std::cout << "Game of Life: " << GRID << "x" << GRID
              << " grid, " << NGENERATIONS << " generations\n";

    // ── Instance + device ─────────────────────────────────────────────────────
    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo instCI{};
    instCI.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instCI.pApplicationInfo = &appInfo;
    VkInstance instance;
    VK(vkCreateInstance(&instCI, nullptr, &instance));

    uint32_t devCount = 0;
    vkEnumeratePhysicalDevices(instance, &devCount, nullptr);
    std::vector<VkPhysicalDevice> physDevs(devCount);
    vkEnumeratePhysicalDevices(instance, &devCount, physDevs.data());
    VkPhysicalDevice physDev = physDevs[0];
    {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(physDev, &p);
        std::cout << "Vulkan device: " << p.deviceName << "\n";
    }

    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(physDev, &memProps);
    auto findMem = [&](uint32_t bits, VkMemoryPropertyFlags fl) -> uint32_t {
        for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i)
            if ((bits & (1u<<i)) && (memProps.memoryTypes[i].propertyFlags & fl) == fl) return i;
        std::cerr << "No memory type\n"; std::exit(1);
    };

    uint32_t qFamilyIdx = 0;
    {
        uint32_t n = 0; vkGetPhysicalDeviceQueueFamilyProperties(physDev, &n, nullptr);
        std::vector<VkQueueFamilyProperties> qf(n);
        vkGetPhysicalDeviceQueueFamilyProperties(physDev, &n, qf.data());
        for (uint32_t i = 0; i < n; ++i)
            if (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { qFamilyIdx = i; break; }
    }

    float prio = 1.f;
    VkDeviceQueueCreateInfo qCI{};
    qCI.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qCI.queueFamilyIndex = qFamilyIdx; qCI.queueCount = 1; qCI.pQueuePriorities = &prio;
    VkDeviceCreateInfo devCI{};
    devCI.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    devCI.queueCreateInfoCount = 1; devCI.pQueueCreateInfos = &qCI;
    VkDevice device; VK(vkCreateDevice(physDev, &devCI, nullptr, &device));
    VkQueue queue; vkGetDeviceQueue(device, qFamilyIdx, 0, &queue);

    // ── Ping-pong storage buffers ─────────────────────────────────────────────
    VkDeviceSize bufSz = (VkDeviceSize)GRID * GRID * sizeof(uint32_t);
    VkBuffer bufs[2]; VkDeviceMemory mems[2];
    for (int b = 0; b < 2; b++) {
        VkBufferCreateInfo bCI{};
        bCI.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bCI.size  = bufSz;
        bCI.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        VK(vkCreateBuffer(device, &bCI, nullptr, &bufs[b]));
        VkMemoryRequirements req; vkGetBufferMemoryRequirements(device, bufs[b], &req);
        VkMemoryAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize  = req.size;
        ai.memoryTypeIndex = findMem(req.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VK(vkAllocateMemory(device, &ai, nullptr, &mems[b]));
        VK(vkBindBufferMemory(device, bufs[b], mems[b], 0));
    }

    // Seed buffer 0 with random cells (~30% alive)
    {
        void* p; VK(vkMapMemory(device, mems[0], 0, bufSz, 0, &p));
        auto* cells = reinterpret_cast<uint32_t*>(p);
        std::mt19937 rng(42);
        for (int i = 0; i < GRID * GRID; i++)
            cells[i] = (rng() % 10 < 3) ? 1u : 0u;
        vkUnmapMemory(device, mems[0]);
        // Zero buffer 1
        VK(vkMapMemory(device, mems[1], 0, bufSz, 0, &p));
        std::memset(p, 0, bufSz); vkUnmapMemory(device, mems[1]);
    }

    // ── Descriptor set layout (2 storage buffers) ─────────────────────────────
    VkDescriptorSetLayoutBinding bindings[2]{};
    for (int b = 0; b < 2; b++) {
        bindings[b].binding         = (uint32_t)b;
        bindings[b].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[b].descriptorCount = 1;
        bindings[b].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo dsLayoutCI{};
    dsLayoutCI.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dsLayoutCI.bindingCount = 2; dsLayoutCI.pBindings = bindings;
    VkDescriptorSetLayout dsLayout; VK(vkCreateDescriptorSetLayout(device, &dsLayoutCI, nullptr, &dsLayout));

    // Push constant: uint width + uint height
    VkPushConstantRange pcRange{VK_SHADER_STAGE_COMPUTE_BIT, 0, 2 * sizeof(uint32_t)};

    VkPipelineLayoutCreateInfo plCI{};
    plCI.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plCI.setLayoutCount = 1; plCI.pSetLayouts = &dsLayout;
    plCI.pushConstantRangeCount = 1; plCI.pPushConstantRanges = &pcRange;
    VkPipelineLayout pipelineLayout; VK(vkCreatePipelineLayout(device, &plCI, nullptr, &pipelineLayout));

    // ── Descriptor pool + 2 sets (one per ping/pong orientation) ──────────────
    VkDescriptorPoolSize poolSz{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4};
    VkDescriptorPoolCreateInfo dpCI{};
    dpCI.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpCI.maxSets = 2; dpCI.poolSizeCount = 1; dpCI.pPoolSizes = &poolSz;
    VkDescriptorPool descPool; VK(vkCreateDescriptorPool(device, &dpCI, nullptr, &descPool));

    VkDescriptorSetLayout layouts[2] = {dsLayout, dsLayout};
    VkDescriptorSetAllocateInfo dsAlloc{};
    dsAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsAlloc.descriptorPool = descPool; dsAlloc.descriptorSetCount = 2; dsAlloc.pSetLayouts = layouts;
    VkDescriptorSet descSets[2]; VK(vkAllocateDescriptorSets(device, &dsAlloc, descSets));

    // descSets[0]: bufs[0]=read, bufs[1]=write
    // descSets[1]: bufs[1]=read, bufs[0]=write
    for (int s = 0; s < 2; s++) {
        VkDescriptorBufferInfo bi[2]{};
        bi[0].buffer = bufs[s];      bi[0].offset = 0; bi[0].range = bufSz;  // read
        bi[1].buffer = bufs[1 - s];  bi[1].offset = 0; bi[1].range = bufSz;  // write
        VkWriteDescriptorSet writes[2]{};
        for (int b = 0; b < 2; b++) {
            writes[b].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[b].dstSet          = descSets[s];
            writes[b].dstBinding      = (uint32_t)b;
            writes[b].descriptorCount = 1;
            writes[b].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[b].pBufferInfo     = &bi[b];
        }
        vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);
    }

    // ── Compute pipeline ───────────────────────────────────────────────────────
    auto spvCode = readSpv(spvPath);
    VkShaderModuleCreateInfo smCI{};
    smCI.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smCI.codeSize = spvCode.size() * 4; smCI.pCode = spvCode.data();
    VkShaderModule shaderMod; VK(vkCreateShaderModule(device, &smCI, nullptr, &shaderMod));

    VkComputePipelineCreateInfo cpCI{};
    cpCI.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cpCI.stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpCI.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
    cpCI.stage.module = shaderMod;
    cpCI.stage.pName  = "main";
    cpCI.layout       = pipelineLayout;
    VkPipeline pipeline; VK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpCI, nullptr, &pipeline));

    // ── Command pool + buffer ──────────────────────────────────────────────────
    VkCommandPoolCreateInfo cpoolCI{};
    cpoolCI.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpoolCI.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpoolCI.queueFamilyIndex = qFamilyIdx;
    VkCommandPool cmdPool; VK(vkCreateCommandPool(device, &cpoolCI, nullptr, &cmdPool));

    VkCommandBufferAllocateInfo cmdAlloc{};
    cmdAlloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmdAlloc.commandPool = cmdPool; cmdAlloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdAlloc.commandBufferCount = 1;
    VkCommandBuffer cmd; VK(vkAllocateCommandBuffers(device, &cmdAlloc, &cmd));

    VkFenceCreateInfo fCI{};
    fCI.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence; VK(vkCreateFence(device, &fCI, nullptr, &fence));

    struct { uint32_t width, height; } pc{(uint32_t)GRID, (uint32_t)GRID};
    uint32_t groupsX = ((uint32_t)GRID + 15) / 16;
    uint32_t groupsY = ((uint32_t)GRID + 15) / 16;

    // ── Helpers ───────────────────────────────────────────────────────────────
    // Record a batch of generations [startGen, startGen+count) into cmd.
    // Uses pipeline barriers between dispatches — no CPU roundtrip between gens.
    VkMemoryBarrier genBarrier{};
    genBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    genBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    genBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    auto recordBatch = [&](int startGen, int count) {
        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        VK(vkBeginCommandBuffer(cmd, &bi));
        for (int i = 0; i < count; i++) {
            int gen = startGen + i;
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout,
                                    0, 1, &descSets[gen & 1], 0, nullptr);
            vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                               0, sizeof(pc), &pc);
            vkCmdDispatch(cmd, groupsX, groupsY, 1);
            if (i < count - 1)
                vkCmdPipelineBarrier(cmd,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    0, 1, &genBarrier, 0, nullptr, 0, nullptr);
        }
        VK(vkEndCommandBuffer(cmd));
    };

    auto submitAndWait = [&]() {
        VkSubmitInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
        VK(vkQueueSubmit(queue, 1, &si, fence));
        VK(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX));
        VK(vkResetFences(device, 1, &fence));
        VK(vkResetCommandBuffer(cmd, 0));
    };

    auto saveFrame = [&](int bufIdx, const char* path) {
        void* p; VK(vkMapMemory(device, mems[bufIdx], 0, bufSz, 0, &p));
        auto* cells = reinterpret_cast<uint32_t*>(p);
        std::ofstream ppm(path, std::ios::binary);
        ppm << "P6\n" << GRID << " " << GRID << "\n255\n";
        for (int i = 0; i < GRID * GRID; i++) {
            uint8_t v = cells[i] ? 255 : 0;
            ppm.write(reinterpret_cast<const char*>(&v), 1);
            ppm.write(reinterpret_cast<const char*>(&v), 1);
            ppm.write(reinterpret_cast<const char*>(&v), 1);
        }
        vkUnmapMemory(device, mems[bufIdx]);
    };

    // ── Warmup (2 generations, barriers, one submit) ──────────────────────────
    recordBatch(0, 2);
    submitAndWait();

    mkdir("result", 0755);

    // ── Timed run ─────────────────────────────────────────────────────────────
    // Warmup used gens 0-1 (even count), so timed run starts at gen 2.
    // Because 2 is even, descSets[gen&1] indexing is unchanged.
    auto t0 = std::chrono::high_resolution_clock::now();
    int frameIdx = 0;

    if (SNAP_EVERY == 0) {
        // Benchmark: all generations in one command buffer, one submit
        recordBatch(2, NGENERATIONS);
        submitAndWait();
    } else {
        // Animation: batch SNAP_EVERY gens per submit so we can readback frames
        std::cout << "[life-gpu] Animation mode: saving frame every "
                  << SNAP_EVERY << " generations\n";
        for (int base = 0; base < NGENERATIONS; base += SNAP_EVERY) {
            int batch = std::min(SNAP_EVERY, NGENERATIONS - base);
            recordBatch(2 + base, batch);
            submitAndWait();
            char path[256];
            std::snprintf(path, sizeof(path), "result/life_gpu_%04d.ppm", frameIdx++);
            saveFrame((2 + base + batch) & 1, path);
        }
    }

    auto t1 = std::chrono::high_resolution_clock::now();
    double total_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    double per_gen  = total_ms / NGENERATIONS;
    double mpx_ms   = (double)GRID * GRID / 1e6 / per_gen;

    std::cout << "[life-gpu] " << NGENERATIONS << " generations in " << total_ms << " ms\n";
    std::cout << "[life-gpu] avg: " << per_gen << " ms/gen"
              << "  (" << 1000.0 / per_gen << " gen/s)"
              << "  " << mpx_ms << " Mpx/ms\n";

    // Save final state (only in non-animation mode)
    if (SNAP_EVERY == 0) {
        saveFrame(NGENERATIONS & 1, "result/life_gpu.ppm");
        std::cout << "[life-gpu] Final state: result/life_gpu.ppm\n";
    }

    // Encode MP4 if animation frames were saved, then delete PPMs
    if (SNAP_EVERY > 0 && frameIdx > 1) {
        char cmd_str[512];
        std::snprintf(cmd_str, sizeof(cmd_str),
            "ffmpeg -y -framerate 30 -i result/life_gpu_%%04d.ppm "
            "-c:v libx264 -pix_fmt yuv420p -vf scale=%d:%d -crf 18 "
            "result/life_gpu.mp4 2>/dev/null", GRID * 4, GRID * 4);
        if (std::system(cmd_str) == 0) {
            std::cout << "[life-gpu] Animation: result/life_gpu.mp4\n";
            for (int i = 0; i < frameIdx; i++) {
                char path[256];
                std::snprintf(path, sizeof(path), "result/life_gpu_%04d.ppm", i);
                std::remove(path);
            }
        }
    }

    // ── Cleanup ───────────────────────────────────────────────────────────────
    vkDestroyFence(device, fence, nullptr);
    vkDestroyCommandPool(device, cmdPool, nullptr);
    vkDestroyPipeline(device, pipeline, nullptr);
    vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
    vkDestroyShaderModule(device, shaderMod, nullptr);
    vkDestroyDescriptorPool(device, descPool, nullptr);
    vkDestroyDescriptorSetLayout(device, dsLayout, nullptr);
    for (int b = 0; b < 2; b++) {
        vkDestroyBuffer(device, bufs[b], nullptr);
        vkFreeMemory(device, mems[b], nullptr);
    }
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);
    return 0;
}
