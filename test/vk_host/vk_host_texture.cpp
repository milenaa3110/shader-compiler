// vk_host_texture.cpp — Vulkan offscreen renderer with texture sampling.
// Creates a 512x512 procedural VkImage + VkSampler, binds as descriptor set 0/binding 0.
// The fragment shader samples the texture with animated UV distortion (9 taps).
//
// GPU ADVANTAGE: hardware TMU does bilinear filtering at zero ALU cost.
// CPU CHALLENGE: 9 bilinear samples × 4 lerps + 8 memory reads per sample per pixel.
//
// Usage: ./spirv_vulkan_texture_host quad.vert.spv texture_test.frag.spv [name] [nframes]
// Build: g++ -std=c++20 -O2 -o build/spirv/spirv_vulkan_texture_host \
//            test/vk_host/vk_host_texture.cpp -lvulkan

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

static constexpr VkFormat COLOR_FMT = VK_FORMAT_R8G8B8A8_UNORM;
static constexpr VkFormat TEX_FMT   = VK_FORMAT_R8G8B8A8_UNORM;
static constexpr int TEX_W = 512, TEX_H = 512;
static int W = 512, H = 512, NFRAMES = 60;
static float FPS = 30.f;

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

static uint32_t findMemType(VkPhysicalDevice pd, uint32_t bits, VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & props) == props) return i;
    std::cerr << "No suitable memory type\n"; std::exit(1);
}

static void writePPM(const char* path, int w, int h, const uint8_t* rgba) {
    std::ofstream f(path, std::ios::binary);
    f << "P6\n" << w << " " << h << "\n255\n";
    for (int i = 0; i < w*h; i++) f.write((char*)(rgba + i*4), 3);
}

// Generate procedural texture data (marble + color noise)
static std::vector<uint8_t> makeTexture() {
    std::vector<uint8_t> data(TEX_W * TEX_H * 4);
    for (int y = 0; y < TEX_H; y++) {
        for (int x = 0; x < TEX_W; x++) {
            float u = (float)x / TEX_W, v = (float)y / TEX_H;
            // Marble pattern
            float s = sinf((u + v) * 20.f + sinf(u * 15.f) * sinf(v * 12.f) * 2.f);
            float t = 0.5f + 0.5f * s;
            // Color channels with offset
            float r = 0.5f + 0.4f * sinf(t * 6.28f + 0.0f);
            float g = 0.5f + 0.4f * sinf(t * 6.28f + 2.1f);
            float b = 0.5f + 0.4f * sinf(t * 6.28f + 4.2f);
            int i = (y * TEX_W + x) * 4;
            data[i+0] = (uint8_t)(r * 255);
            data[i+1] = (uint8_t)(g * 255);
            data[i+2] = (uint8_t)(b * 255);
            data[i+3] = 255;
        }
    }
    return data;
}

int main(int argc, char** argv) {
    const char* vertSpv  = (argc > 1) ? argv[1] : "result/quad.vert.spv";
    const char* fragSpv  = (argc > 2) ? argv[2] : "result/texture_test.frag.spv";
    const char* animName = (argc > 3) ? argv[3] : "texture_test";
    if (argc > 4) NFRAMES = std::atoi(argv[4]);
    if (argc > 5) W = std::atoi(argv[5]);
    if (argc > 6) H = std::atoi(argv[6]);
    mkdir("result", 0755);

    // ── Instance + device (same pattern as spirv_vulkan_host) ────────────────
    VkApplicationInfo appInfo{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    appInfo.pApplicationName = "tex_bench"; appInfo.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo instCI{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instCI.pApplicationInfo = &appInfo;
    VkInstance instance; VK(vkCreateInstance(&instCI, nullptr, &instance));

    uint32_t devCount = 0;
    vkEnumeratePhysicalDevices(instance, &devCount, nullptr);
    if (!devCount) { std::cerr << "No device.\n"; return 1; }
    std::vector<VkPhysicalDevice> pds(devCount);
    vkEnumeratePhysicalDevices(instance, &devCount, pds.data());
    VkPhysicalDevice pd = pds[0];

    VkPhysicalDeviceProperties pdp; vkGetPhysicalDeviceProperties(pd, &pdp);
    std::cout << "[" << animName << "] GPU: " << pdp.deviceName << "\n";

    uint32_t qfCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &qfCount, nullptr);
    std::vector<VkQueueFamilyProperties> qfs(qfCount);
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &qfCount, qfs.data());
    uint32_t qfi = 0;
    for (uint32_t i = 0; i < qfCount; i++)
        if (qfs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { qfi = i; break; }

    float qprio = 1.f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = qfi; qci.queueCount = 1; qci.pQueuePriorities = &qprio;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
    VkDevice dev; VK(vkCreateDevice(pd, &dci, nullptr, &dev));
    VkQueue queue; vkGetDeviceQueue(dev, qfi, 0, &queue);

    // ── Create texture image ──────────────────────────────────────────────────
    auto texData = makeTexture();
    VkDeviceSize texSize = TEX_W * TEX_H * 4;

    // Staging buffer
    VkBuffer stageBuf; VkDeviceMemory stageMem;
    {
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = texSize; bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VK(vkCreateBuffer(dev, &bci, nullptr, &stageBuf));
        VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev, stageBuf, &mr);
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize = mr.size;
        mai.memoryTypeIndex = findMemType(pd, mr.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VK(vkAllocateMemory(dev, &mai, nullptr, &stageMem));
        VK(vkBindBufferMemory(dev, stageBuf, stageMem, 0));
        void* ptr; VK(vkMapMemory(dev, stageMem, 0, texSize, 0, &ptr));
        std::memcpy(ptr, texData.data(), texSize);
        vkUnmapMemory(dev, stageMem);
    }

    // Texture image
    VkImage texImg; VkDeviceMemory texMem;
    {
        VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ici.imageType = VK_IMAGE_TYPE_2D; ici.format = TEX_FMT;
        ici.extent = {(uint32_t)TEX_W, (uint32_t)TEX_H, 1};
        ici.mipLevels = 1; ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT; ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VK(vkCreateImage(dev, &ici, nullptr, &texImg));
        VkMemoryRequirements mr; vkGetImageMemoryRequirements(dev, texImg, &mr);
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize = mr.size;
        mai.memoryTypeIndex = findMemType(pd, mr.memoryTypeBits,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VK(vkAllocateMemory(dev, &mai, nullptr, &texMem));
        VK(vkBindImageMemory(dev, texImg, texMem, 0));
    }

    // Command pool for setup commands
    VkCommandPool cmdPool;
    {
        VkCommandPoolCreateInfo cpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        cpci.queueFamilyIndex = qfi;
        cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        VK(vkCreateCommandPool(dev, &cpci, nullptr, &cmdPool));
    }

    // Upload texture via staging buffer
    {
        VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cbai.commandPool = cmdPool; cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbai.commandBufferCount = 1;
        VkCommandBuffer cmd; VK(vkAllocateCommandBuffers(dev, &cbai, &cmd));

        VkCommandBufferBeginInfo cbbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &cbbi);

        // Transition UNDEFINED → TRANSFER_DST
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = texImg;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &barrier);

        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {(uint32_t)TEX_W, (uint32_t)TEX_H, 1};
        vkCmdCopyBufferToImage(cmd, stageBuf, texImg,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

        // Transition TRANSFER_DST → SHADER_READ_ONLY
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &barrier);

        vkEndCommandBuffer(cmd);
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
        VkFence fence; VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VK(vkCreateFence(dev, &fci, nullptr, &fence));
        VK(vkQueueSubmit(queue, 1, &si, fence));
        VK(vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX));
        vkDestroyFence(dev, fence, nullptr);
        vkFreeCommandBuffers(dev, cmdPool, 1, &cmd);
    }

    // Texture image view + sampler
    VkImageView texView;
    {
        VkImageViewCreateInfo ivci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        ivci.image = texImg; ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        ivci.format = TEX_FMT;
        ivci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VK(vkCreateImageView(dev, &ivci, nullptr, &texView));
    }
    VkSampler sampler;
    {
        VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
        sci.addressModeU = sci.addressModeV = sci.addressModeW =
            VK_SAMPLER_ADDRESS_MODE_REPEAT;
        VK(vkCreateSampler(dev, &sci, nullptr, &sampler));
    }

    // ── Descriptor set layout: binding 0 = combined image sampler ─────────────
    VkDescriptorSetLayoutBinding dslb{};
    dslb.binding = 0; dslb.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    dslb.descriptorCount = 1; dslb.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo dslci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dslci.bindingCount = 1; dslci.pBindings = &dslb;
    VkDescriptorSetLayout dsl; VK(vkCreateDescriptorSetLayout(dev, &dslci, nullptr, &dsl));

    // Push constant: float uTime at offset 0
    VkPushConstantRange pcr{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 4};
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 1; plci.pSetLayouts = &dsl;
    plci.pushConstantRangeCount = 1; plci.pPushConstantRanges = &pcr;
    VkPipelineLayout pipeLayout; VK(vkCreatePipelineLayout(dev, &plci, nullptr, &pipeLayout));

    // ── Framebuffer image (readback target) ───────────────────────────────────
    VkImage fbImg; VkDeviceMemory fbMem; VkImageView fbView;
    {
        VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ici.imageType = VK_IMAGE_TYPE_2D; ici.format = COLOR_FMT;
        ici.extent = {(uint32_t)W, (uint32_t)H, 1};
        ici.mipLevels = ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT; ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VK(vkCreateImage(dev, &ici, nullptr, &fbImg));
        VkMemoryRequirements mr; vkGetImageMemoryRequirements(dev, fbImg, &mr);
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize = mr.size;
        mai.memoryTypeIndex = findMemType(pd, mr.memoryTypeBits,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VK(vkAllocateMemory(dev, &mai, nullptr, &fbMem));
        VK(vkBindImageMemory(dev, fbImg, fbMem, 0));

        VkImageViewCreateInfo ivci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        ivci.image = fbImg; ivci.viewType = VK_IMAGE_VIEW_TYPE_2D; ivci.format = COLOR_FMT;
        ivci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VK(vkCreateImageView(dev, &ivci, nullptr, &fbView));
    }

    // Readback buffer
    VkBuffer readBuf; VkDeviceMemory readMem;
    {
        VkDeviceSize sz = W * H * 4;
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = sz; bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VK(vkCreateBuffer(dev, &bci, nullptr, &readBuf));
        VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev, readBuf, &mr);
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize = mr.size;
        mai.memoryTypeIndex = findMemType(pd, mr.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VK(vkAllocateMemory(dev, &mai, nullptr, &readMem));
        VK(vkBindBufferMemory(dev, readBuf, readMem, 0));
    }

    // ── Render pass ───────────────────────────────────────────────────────────
    VkAttachmentDescription att{};
    att.format = COLOR_FMT; att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

    VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1; subpass.pColorAttachments = &ref;

    VkRenderPassCreateInfo rpci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpci.attachmentCount = 1; rpci.pAttachments = &att;
    rpci.subpassCount = 1; rpci.pSubpasses = &subpass;
    VkRenderPass renderPass; VK(vkCreateRenderPass(dev, &rpci, nullptr, &renderPass));

    VkFramebufferCreateInfo fbci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fbci.renderPass = renderPass; fbci.attachmentCount = 1; fbci.pAttachments = &fbView;
    fbci.width = W; fbci.height = H; fbci.layers = 1;
    VkFramebuffer framebuffer; VK(vkCreateFramebuffer(dev, &fbci, nullptr, &framebuffer));

    // ── Shader modules ────────────────────────────────────────────────────────
    auto makeModule = [&](const char* path) {
        auto code = readSpv(path);
        VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        smci.codeSize = code.size() * 4; smci.pCode = code.data();
        VkShaderModule m; VK(vkCreateShaderModule(dev, &smci, nullptr, &m));
        return m;
    };
    VkShaderModule vertMod = makeModule(vertSpv);
    VkShaderModule fragMod = makeModule(fragSpv);

    // ── Graphics pipeline ─────────────────────────────────────────────────────
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertMod; stages[0].pName = "main";
    stages[1] = stages[0];
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = fragMod;

    VkPipelineVertexInputStateCreateInfo   vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport vp{0,0,(float)W,(float)H,0,1};
    VkRect2D sc{{0,0},{(uint32_t)W,(uint32_t)H}};
    VkPipelineViewportStateCreateInfo vs_state{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vs_state.viewportCount = 1; vs_state.pViewports = &vp;
    vs_state.scissorCount  = 1; vs_state.pScissors  = &sc;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_CLOCKWISE; rs.lineWidth = 1.f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1; cb.pAttachments = &cba;

    VkGraphicsPipelineCreateInfo gpci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gpci.stageCount = 2; gpci.pStages = stages;
    gpci.pVertexInputState = &vi; gpci.pInputAssemblyState = &ia;
    gpci.pViewportState = &vs_state; gpci.pRasterizationState = &rs;
    gpci.pMultisampleState = &ms; gpci.pColorBlendState = &cb;
    gpci.layout = pipeLayout; gpci.renderPass = renderPass;
    VkPipeline pipeline;
    VK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpci, nullptr, &pipeline));

    // ── Descriptor pool + set ─────────────────────────────────────────────────
    VkDescriptorPoolSize dpSz{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
    VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpci.maxSets = 1; dpci.poolSizeCount = 1; dpci.pPoolSizes = &dpSz;
    VkDescriptorPool dpool; VK(vkCreateDescriptorPool(dev, &dpci, nullptr, &dpool));

    VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dsai.descriptorPool = dpool; dsai.descriptorSetCount = 1; dsai.pSetLayouts = &dsl;
    VkDescriptorSet ds; VK(vkAllocateDescriptorSets(dev, &dsai, &ds));

    VkDescriptorImageInfo dii{sampler, texView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet wds{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    wds.dstSet = ds; wds.dstBinding = 0; wds.descriptorCount = 1;
    wds.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wds.pImageInfo = &dii;
    vkUpdateDescriptorSets(dev, 1, &wds, 0, nullptr);

    // ── Command buffer + fence ────────────────────────────────────────────────
    VkCommandBuffer cmd;
    {
        VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cbai.commandPool = cmdPool; cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbai.commandBufferCount = 1;
        VK(vkAllocateCommandBuffers(dev, &cbai, &cmd));
    }
    VkFence fence;
    { VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
      VK(vkCreateFence(dev, &fci, nullptr, &fence)); }

    // ── Render loop ───────────────────────────────────────────────────────────
    double total_ms = 0.0;
    std::cout << "[" << animName << "] GPU texture test: "
              << W << "x" << H << ", " << NFRAMES << " frames\n";

    for (int frame = 0; frame < NFRAMES; frame++) {
        float uTime = frame / FPS;

        VkCommandBufferBeginInfo cbbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        vkBeginCommandBuffer(cmd, &cbbi);

        VkClearValue clearVal{{0.f, 0.f, 0.f, 1.f}};
        VkRenderPassBeginInfo rpbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        rpbi.renderPass = renderPass; rpbi.framebuffer = framebuffer;
        rpbi.renderArea = {{0,0},{(uint32_t)W,(uint32_t)H}};
        rpbi.clearValueCount = 1; rpbi.pClearValues = &clearVal;

        vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
            pipeLayout, 0, 1, &ds, 0, nullptr);
        vkCmdPushConstants(cmd, pipeLayout,
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
            0, 4, &uTime);
        vkCmdDraw(cmd, 6, 1, 0, 0);
        vkCmdEndRenderPass(cmd);

        // Copy to readback buffer
        VkBufferImageCopy bic{};
        bic.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        bic.imageExtent = {(uint32_t)W, (uint32_t)H, 1};
        vkCmdCopyImageToBuffer(cmd, fbImg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            readBuf, 1, &bic);

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

        // Write PPM
        uint8_t* ptr;
        VK(vkMapMemory(dev, readMem, 0, W*H*4, 0, (void**)&ptr));
        char path[256];
        std::snprintf(path, sizeof(path), "result/%s_%03d.ppm", animName, frame);
        writePPM(path, W, H, ptr);
        vkUnmapMemory(dev, readMem);

        std::cout << "[" << animName << "] frame " << frame
                  << " (t=" << uTime << ") → " << path << "\n";
    }

    double avg = total_ms / NFRAMES;
    std::cout << "[" << animName << "] Vulkan avg: " << avg
              << " ms/frame  (" << (1000.0/avg) << " fps)\n";

    // Encode MP4
    char cmd_str[512];
    std::snprintf(cmd_str, sizeof(cmd_str),
        "ffmpeg -y -framerate %d -i result/%s_%%03d.ppm "
        "-c:v libx264 -pix_fmt yuv420p -crf 18 result/%s.mp4 2>/dev/null",
        (int)FPS, animName, animName);
    if (std::system(cmd_str) == 0)
        std::cout << "[" << animName << "] MP4: result/" << animName << ".mp4\n";

    // Cleanup
    vkDestroyFence(dev, fence, nullptr);
    vkDestroyPipeline(dev, pipeline, nullptr);
    vkDestroyShaderModule(dev, vertMod, nullptr);
    vkDestroyShaderModule(dev, fragMod, nullptr);
    vkDestroyDescriptorPool(dev, dpool, nullptr);
    vkDestroyPipelineLayout(dev, pipeLayout, nullptr);
    vkDestroyDescriptorSetLayout(dev, dsl, nullptr);
    vkDestroyFramebuffer(dev, framebuffer, nullptr);
    vkDestroyRenderPass(dev, renderPass, nullptr);
    vkDestroyImageView(dev, fbView, nullptr);
    vkDestroyImage(dev, fbImg, nullptr); vkFreeMemory(dev, fbMem, nullptr);
    vkDestroyBuffer(dev, readBuf, nullptr); vkFreeMemory(dev, readMem, nullptr);
    vkDestroyImageView(dev, texView, nullptr);
    vkDestroySampler(dev, sampler, nullptr);
    vkDestroyImage(dev, texImg, nullptr); vkFreeMemory(dev, texMem, nullptr);
    vkDestroyBuffer(dev, stageBuf, nullptr); vkFreeMemory(dev, stageMem, nullptr);
    vkDestroyCommandPool(dev, cmdPool, nullptr);
    vkDestroyDevice(dev, nullptr);
    vkDestroyInstance(instance, nullptr);
    return 0;
}
