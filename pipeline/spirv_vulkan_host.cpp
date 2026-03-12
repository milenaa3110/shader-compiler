// spirv_vulkan_host.cpp — Vulkan offscreen animation renderer using SPIR-V shaders.
// Executes via LavaPipe (Mesa CPU Vulkan) without a display.
// Renders 60 frames of plasma animation, writes result/anim_spirv_NNN.ppm,
// then encodes result/anim_spirv.mp4 via ffmpeg.
//
// Build: see Makefile target anim-spirv-vulkan
// Requires: libvulkan-dev, LavaPipe ICD (libvulkan_lvp.so already present)

#include <vulkan/vulkan.h>

#include <vector>
#include <fstream>
#include <iostream>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <cmath>
#include <sys/stat.h>

static constexpr int W       = 512;
static constexpr int H       = 512;
static constexpr int NFRAMES = 60;
static constexpr float FPS   = 30.f;
static constexpr VkFormat COLOR_FMT = VK_FORMAT_R8G8B8A8_UNORM;

// ---------- helpers ----------

static void check(VkResult r, const char* where) {
    if (r != VK_SUCCESS) {
        std::cerr << "Vulkan error " << r << " at " << where << "\n";
        std::exit(1);
    }
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

static void writePPM(const char* path, int w, int h, const uint8_t* rgba) {
    std::ofstream f(path, std::ios::binary);
    f << "P6\n" << w << " " << h << "\n255\n";
    for (int i = 0; i < w * h; ++i)
        f.write(reinterpret_cast<const char*>(rgba + i * 4), 3); // drop alpha
}

// ---------- main ----------

int main(int argc, char** argv) {
    const char* vertSpv = (argc > 1) ? argv[1] : "result/anim.vert.spv";
    const char* fragSpv = (argc > 2) ? argv[2] : "result/anim.frag.spv";

    mkdir("result", 0755);

    // ── Instance ──────────────────────────────────────────────────────────────
    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "spirv_anim";
    appInfo.apiVersion = VK_API_VERSION_1_1;

    VkInstanceCreateInfo instCI{};
    instCI.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instCI.pApplicationInfo = &appInfo;

    VkInstance instance;
    VK(vkCreateInstance(&instCI, nullptr, &instance));

    // ── Physical device (pick first — LavaPipe on headless) ───────────────────
    uint32_t devCount = 0;
    vkEnumeratePhysicalDevices(instance, &devCount, nullptr);
    if (devCount == 0) { std::cerr << "No Vulkan device found.\n"; return 1; }
    std::vector<VkPhysicalDevice> physDevs(devCount);
    vkEnumeratePhysicalDevices(instance, &devCount, physDevs.data());
    VkPhysicalDevice physDev = physDevs[0];
    {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(physDev, &props);
        std::cout << "Vulkan device: " << props.deviceName << "\n";
    }

    // ── Logical device + graphics queue ──────────────────────────────────────
    uint32_t qFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physDev, &qFamilyCount, nullptr);
    std::vector<VkQueueFamilyProperties> qFamilies(qFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(physDev, &qFamilyCount, qFamilies.data());
    uint32_t qFamilyIdx = UINT32_MAX;
    for (uint32_t i = 0; i < qFamilyCount; ++i) {
        if (qFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { qFamilyIdx = i; break; }
    }
    if (qFamilyIdx == UINT32_MAX) { std::cerr << "No graphics queue.\n"; return 1; }

    float qPriority = 1.f;
    VkDeviceQueueCreateInfo qCI{};
    qCI.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qCI.queueFamilyIndex = qFamilyIdx;
    qCI.queueCount = 1;
    qCI.pQueuePriorities = &qPriority;

    VkDeviceCreateInfo devCI{};
    devCI.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    devCI.queueCreateInfoCount = 1;
    devCI.pQueueCreateInfos = &qCI;

    VkDevice device;
    VK(vkCreateDevice(physDev, &devCI, nullptr, &device));
    VkQueue queue;
    vkGetDeviceQueue(device, qFamilyIdx, 0, &queue);

    // ── Color image (GPU) + readback buffer (host-visible) ───────────────────
    VkImageCreateInfo imgCI{};
    imgCI.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imgCI.imageType = VK_IMAGE_TYPE_2D;
    imgCI.format = COLOR_FMT;
    imgCI.extent = {(uint32_t)W, (uint32_t)H, 1};
    imgCI.mipLevels = 1; imgCI.arrayLayers = 1;
    imgCI.samples = VK_SAMPLE_COUNT_1_BIT;
    imgCI.tiling = VK_IMAGE_TILING_OPTIMAL;
    imgCI.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    imgCI.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkImage colorImage;
    VK(vkCreateImage(device, &imgCI, nullptr, &colorImage));

    VkMemoryRequirements imgReqs;
    vkGetImageMemoryRequirements(device, colorImage, &imgReqs);

    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(physDev, &memProps);

    auto findMem = [&](uint32_t typeBits, VkMemoryPropertyFlags flags) -> uint32_t {
        for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i)
            if ((typeBits & (1u << i)) && (memProps.memoryTypes[i].propertyFlags & flags) == flags)
                return i;
        std::cerr << "No suitable memory type.\n"; std::exit(1);
    };

    VkMemoryAllocateInfo imgAlloc{};
    imgAlloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    imgAlloc.allocationSize = imgReqs.size;
    imgAlloc.memoryTypeIndex = findMem(imgReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkDeviceMemory imgMem;
    VK(vkAllocateMemory(device, &imgAlloc, nullptr, &imgMem));
    VK(vkBindImageMemory(device, colorImage, imgMem, 0));

    // Readback buffer
    VkDeviceSize readSz = (VkDeviceSize)W * H * 4;
    VkBufferCreateInfo bufCI{};
    bufCI.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufCI.size = readSz;
    bufCI.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBuffer readBuf;
    VK(vkCreateBuffer(device, &bufCI, nullptr, &readBuf));

    VkMemoryRequirements bufReqs;
    vkGetBufferMemoryRequirements(device, readBuf, &bufReqs);
    VkMemoryAllocateInfo bufAlloc{};
    bufAlloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    bufAlloc.allocationSize = bufReqs.size;
    bufAlloc.memoryTypeIndex = findMem(bufReqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkDeviceMemory readMem;
    VK(vkAllocateMemory(device, &bufAlloc, nullptr, &readMem));
    VK(vkBindBufferMemory(device, readBuf, readMem, 0));

    // ── Image view ───────────────────────────────────────────────────────────
    VkImageViewCreateInfo ivCI{};
    ivCI.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    ivCI.image = colorImage;
    ivCI.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ivCI.format = COLOR_FMT;
    ivCI.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView colorView;
    VK(vkCreateImageView(device, &ivCI, nullptr, &colorView));

    // ── Render pass ──────────────────────────────────────────────────────────
    VkAttachmentDescription colorAtt{};
    colorAtt.format = COLOR_FMT;
    colorAtt.samples = VK_SAMPLE_COUNT_1_BIT;
    colorAtt.loadOp  = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAtt.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAtt.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAtt.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    colorAtt.finalLayout   = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

    VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;

    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo rpCI{};
    rpCI.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpCI.attachmentCount = 1; rpCI.pAttachments = &colorAtt;
    rpCI.subpassCount    = 1; rpCI.pSubpasses   = &subpass;
    rpCI.dependencyCount = 1; rpCI.pDependencies = &dep;
    VkRenderPass renderPass;
    VK(vkCreateRenderPass(device, &rpCI, nullptr, &renderPass));

    // ── Framebuffer ──────────────────────────────────────────────────────────
    VkFramebufferCreateInfo fbCI{};
    fbCI.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbCI.renderPass = renderPass;
    fbCI.attachmentCount = 1; fbCI.pAttachments = &colorView;
    fbCI.width = W; fbCI.height = H; fbCI.layers = 1;
    VkFramebuffer framebuffer;
    VK(vkCreateFramebuffer(device, &fbCI, nullptr, &framebuffer));

    // ── Shader modules ───────────────────────────────────────────────────────
    auto makeShader = [&](const char* path) -> VkShaderModule {
        auto code = readSpv(path);
        VkShaderModuleCreateInfo smCI{};
        smCI.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        smCI.codeSize = code.size() * 4;
        smCI.pCode    = code.data();
        VkShaderModule sm;
        VK(vkCreateShaderModule(device, &smCI, nullptr, &sm));
        return sm;
    };
    VkShaderModule vertMod = makeShader(vertSpv);
    VkShaderModule fragMod = makeShader(fragSpv);

    // ── Pipeline layout (push constant: float uTime) ─────────────────────────
    VkPushConstantRange pcRange{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(float)};
    VkPipelineLayoutCreateInfo plCI{};
    plCI.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plCI.pushConstantRangeCount = 1;
    plCI.pPushConstantRanges = &pcRange;
    VkPipelineLayout pipelineLayout;
    VK(vkCreatePipelineLayout(device, &plCI, nullptr, &pipelineLayout));

    // ── Graphics pipeline ─────────────────────────────────────────────────────
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertMod;
    stages[0].pName  = "main";
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragMod;
    stages[1].pName  = "main";

    VkPipelineVertexInputStateCreateInfo viState{};
    viState.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo iaState{};
    iaState.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    iaState.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkViewport viewport{0, 0, (float)W, (float)H, 0.f, 1.f};
    VkRect2D scissor{{0,0},{(uint32_t)W,(uint32_t)H}};
    VkPipelineViewportStateCreateInfo vpState{};
    vpState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vpState.viewportCount = 1; vpState.pViewports = &viewport;
    vpState.scissorCount  = 1; vpState.pScissors  = &scissor;

    VkPipelineRasterizationStateCreateInfo rsState{};
    rsState.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rsState.polygonMode = VK_POLYGON_MODE_FILL;
    rsState.cullMode    = VK_CULL_MODE_NONE;
    rsState.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rsState.lineWidth   = 1.f;

    VkPipelineMultisampleStateCreateInfo msState{};
    msState.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    msState.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState cbAtt{};
    cbAtt.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cbState{};
    cbState.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cbState.attachmentCount = 1; cbState.pAttachments = &cbAtt;

    VkGraphicsPipelineCreateInfo gpCI{};
    gpCI.sType      = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gpCI.stageCount = 2; gpCI.pStages = stages;
    gpCI.pVertexInputState   = &viState;
    gpCI.pInputAssemblyState = &iaState;
    gpCI.pViewportState      = &vpState;
    gpCI.pRasterizationState = &rsState;
    gpCI.pMultisampleState   = &msState;
    gpCI.pColorBlendState    = &cbState;
    gpCI.layout     = pipelineLayout;
    gpCI.renderPass = renderPass;

    VkPipeline pipeline;
    VK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gpCI, nullptr, &pipeline));

    // ── Command pool + buffer ─────────────────────────────────────────────────
    VkCommandPoolCreateInfo cpCI{};
    cpCI.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpCI.queueFamilyIndex = qFamilyIdx;
    cpCI.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VkCommandPool cmdPool;
    VK(vkCreateCommandPool(device, &cpCI, nullptr, &cmdPool));

    VkCommandBufferAllocateInfo cbAI{};
    cbAI.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbAI.commandPool = cmdPool;
    cbAI.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbAI.commandBufferCount = 1;
    VkCommandBuffer cmd;
    VK(vkAllocateCommandBuffers(device, &cbAI, &cmd));

    VkFenceCreateInfo fenceCI{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence;
    VK(vkCreateFence(device, &fenceCI, nullptr, &fence));

    // ── Render loop ───────────────────────────────────────────────────────────
    std::vector<uint8_t> pixels(W * H * 4);

    for (int frame = 0; frame < NFRAMES; ++frame) {
        float uTime = frame / FPS;

        // Record
        VkCommandBufferBeginInfo beginI{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        VK(vkBeginCommandBuffer(cmd, &beginI));

        VkClearValue clearVal{};
        clearVal.color = {{0.f, 0.f, 0.f, 1.f}};
        VkRenderPassBeginInfo rpBegin{};
        rpBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rpBegin.renderPass  = renderPass;
        rpBegin.framebuffer = framebuffer;
        rpBegin.renderArea  = {{0,0},{(uint32_t)W,(uint32_t)H}};
        rpBegin.clearValueCount = 1; rpBegin.pClearValues = &clearVal;

        vkCmdBeginRenderPass(cmd, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT,
                           0, sizeof(float), &uTime);
        vkCmdDraw(cmd, 6, 1, 0, 0);  // 6 vertices, no vertex buffer
        vkCmdEndRenderPass(cmd);

        // Copy image → readback buffer
        VkBufferImageCopy region{};
        region.bufferOffset      = 0;
        region.bufferRowLength   = 0;
        region.bufferImageHeight = 0;
        region.imageSubresource  = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent       = {(uint32_t)W, (uint32_t)H, 1};
        vkCmdCopyImageToBuffer(cmd, colorImage,
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               readBuf, 1, &region);

        VK(vkEndCommandBuffer(cmd));

        // Submit
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
        VK(vkQueueSubmit(queue, 1, &si, fence));
        VK(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX));
        VK(vkResetFences(device, 1, &fence));

        // Read pixels
        void* mapped;
        VK(vkMapMemory(device, readMem, 0, readSz, 0, &mapped));
        std::memcpy(pixels.data(), mapped, pixels.size());
        vkUnmapMemory(device, readMem);

        // Write PPM
        char path[128];
        std::snprintf(path, sizeof(path), "result/anim_spirv_%03d.ppm", frame);
        writePPM(path, W, H, pixels.data());
        std::cout << "Frame " << frame << " (t=" << uTime << ") → " << path << "\n";

        VK(vkResetCommandBuffer(cmd, 0));
    }

    // ── Encode MP4 ───────────────────────────────────────────────────────────
    const char* mp4 = "result/anim_spirv.mp4";
    char cmd_str[512];
    std::snprintf(cmd_str, sizeof(cmd_str),
        "ffmpeg -y -framerate 30 -i result/anim_spirv_%%03d.ppm "
        "-c:v libx264 -pix_fmt yuv420p -crf 18 %s 2>/dev/null", mp4);
    if (std::system(cmd_str) == 0)
        std::cout << "MP4 written to " << mp4 << "\n";
    else
        std::cerr << "ffmpeg encoding failed\n";

    // ── Cleanup ───────────────────────────────────────────────────────────────
    vkDestroyFence(device, fence, nullptr);
    vkDestroyCommandPool(device, cmdPool, nullptr);
    vkDestroyPipeline(device, pipeline, nullptr);
    vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
    vkDestroyShaderModule(device, vertMod, nullptr);
    vkDestroyShaderModule(device, fragMod, nullptr);
    vkDestroyFramebuffer(device, framebuffer, nullptr);
    vkDestroyRenderPass(device, renderPass, nullptr);
    vkDestroyImageView(device, colorView, nullptr);
    vkDestroyImage(device, colorImage, nullptr);
    vkFreeMemory(device, imgMem, nullptr);
    vkDestroyBuffer(device, readBuf, nullptr);
    vkFreeMemory(device, readMem, nullptr);
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);

    return 0;
}