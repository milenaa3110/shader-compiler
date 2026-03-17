// vk_host_fragment.cpp — Vulkan offscreen animation renderer using SPIR-V shaders.
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
#include <chrono>
#include <sys/stat.h>

static constexpr VkFormat COLOR_FMT = VK_FORMAT_R8G8B8A8_UNORM;
// Runtime parameters (overridden from argv)
static int W = 512, H = 512, NFRAMES = 60;
static float FPS = 30.f;

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

// Write one RGBA frame as raw RGB to an open ffmpeg pipe (drops alpha)
static void writeFrameRGB(FILE* pipe, int w, int h, const uint8_t* rgba) {
    for (int i = 0; i < w * h; ++i)
        std::fwrite(rgba + i * 4, 1, 3, pipe);
}

// ---------- main ----------

int main(int argc, char** argv) {
    const char* vertSpv   = (argc > 1) ? argv[1] : "result/anim.vert.spv";
    const char* fragSpv   = (argc > 2) ? argv[2] : "result/anim.frag.spv";
    const char* animName  = (argc > 3) ? argv[3] : "spirv_anim";
    if (argc > 4) NFRAMES = std::atoi(argv[4]);
    if (argc > 5) W = std::atoi(argv[5]);
    if (argc > 6) H = std::atoi(argv[6]);
    static int VERT_COUNT = 6;
    if (argc > 7) VERT_COUNT = std::atoi(argv[7]);

    // --bench: pure GPU throughput mode — no readback, no ffmpeg.
    // Pre-records NFRAMES command buffers and submits them all at once.
    bool benchMode = false;
    for (int i = 1; i < argc; i++)
        if (std::string(argv[i]) == "--bench") benchMode = true;

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

    // ── Depth image ──────────────────────────────────────────────────────────
    static const VkFormat DEPTH_FMT = VK_FORMAT_D32_SFLOAT;
    VkImageCreateInfo depthImgCI{};
    depthImgCI.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    depthImgCI.imageType = VK_IMAGE_TYPE_2D;
    depthImgCI.format = DEPTH_FMT;
    depthImgCI.extent = {(uint32_t)W, (uint32_t)H, 1};
    depthImgCI.mipLevels = 1; depthImgCI.arrayLayers = 1;
    depthImgCI.samples = VK_SAMPLE_COUNT_1_BIT;
    depthImgCI.tiling = VK_IMAGE_TILING_OPTIMAL;
    depthImgCI.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    depthImgCI.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage depthImage;
    VK(vkCreateImage(device, &depthImgCI, nullptr, &depthImage));
    VkMemoryRequirements depthReqs;
    vkGetImageMemoryRequirements(device, depthImage, &depthReqs);
    VkMemoryAllocateInfo depthAlloc{};
    depthAlloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    depthAlloc.allocationSize = depthReqs.size;
    depthAlloc.memoryTypeIndex = findMem(depthReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkDeviceMemory depthMem;
    VK(vkAllocateMemory(device, &depthAlloc, nullptr, &depthMem));
    VK(vkBindImageMemory(device, depthImage, depthMem, 0));

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

    // ── Depth image view ─────────────────────────────────────────────────────
    VkImageViewCreateInfo divCI{};
    divCI.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    divCI.image = depthImage;
    divCI.viewType = VK_IMAGE_VIEW_TYPE_2D;
    divCI.format = DEPTH_FMT;
    divCI.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    VkImageView depthView;
    VK(vkCreateImageView(device, &divCI, nullptr, &depthView));

    // ── Render pass ──────────────────────────────────────────────────────────
    VkAttachmentDescription attachments[2]{};
    // attachment 0 — color
    attachments[0].format = COLOR_FMT;
    attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[0].loadOp  = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[0].finalLayout   = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    // attachment 1 — depth
    attachments[1].format = DEPTH_FMT;
    attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[1].loadOp  = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[1].finalLayout   = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depthRef{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;
    subpass.pDepthStencilAttachment = &depthRef;

    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dep.dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo rpCI{};
    rpCI.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpCI.attachmentCount = 2; rpCI.pAttachments = attachments;
    rpCI.subpassCount    = 1; rpCI.pSubpasses   = &subpass;
    rpCI.dependencyCount = 1; rpCI.pDependencies = &dep;
    VkRenderPass renderPass;
    VK(vkCreateRenderPass(device, &rpCI, nullptr, &renderPass));

    // ── Framebuffer ──────────────────────────────────────────────────────────
    VkImageView fbViews[2] = {colorView, depthView};
    VkFramebufferCreateInfo fbCI{};
    fbCI.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbCI.renderPass = renderPass;
    fbCI.attachmentCount = 2; fbCI.pAttachments = fbViews;
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
    VkPushConstantRange pcRange{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(float)};
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

    VkPipelineDepthStencilStateCreateInfo dsState{};
    dsState.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    dsState.depthTestEnable  = VK_TRUE;
    dsState.depthWriteEnable = VK_TRUE;
    dsState.depthCompareOp   = VK_COMPARE_OP_LESS;

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
    gpCI.pDepthStencilState  = &dsState;
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

    // ── ffmpeg pipe — opened lazily on the first frame so a killed process
    //    leaves no empty MP4 file on disk ────────────────────────────────────
    char mp4[256], ff_cmd[512];
    std::snprintf(mp4, sizeof(mp4), "result/%s.mp4", animName);
    std::snprintf(ff_cmd, sizeof(ff_cmd),
        "ffmpeg -y -f rawvideo -pixel_format rgb24 -video_size %dx%d "
        "-framerate %d -i pipe:0 "
        "-c:v libx264 -pix_fmt yuv420p -crf 18 %s 2>/dev/null",
        W, H, (int)FPS, mp4);
    FILE* ffpipe = nullptr;

    // ── Bench mode: all frames in one submit, no readback ────────────────────
    if (benchMode) {
        std::vector<VkCommandBuffer> cmds(NFRAMES);
        VkCommandBufferAllocateInfo cbAI2{};
        cbAI2.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cbAI2.commandPool = cmdPool;
        cbAI2.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbAI2.commandBufferCount = NFRAMES;
        VK(vkAllocateCommandBuffers(device, &cbAI2, cmds.data()));

        for (int frame = 0; frame < NFRAMES; ++frame) {
            float uTime = frame / FPS;
            VkCommandBufferBeginInfo beginI{};
            beginI.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            VK(vkBeginCommandBuffer(cmds[frame], &beginI));

            VkClearValue clearVals[2]{};
            clearVals[0].color = {{0.f, 0.f, 0.f, 1.f}};
            clearVals[1].depthStencil = {1.f, 0};
            VkRenderPassBeginInfo rpBegin{};
            rpBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
            rpBegin.renderPass  = renderPass;
            rpBegin.framebuffer = framebuffer;
            rpBegin.renderArea  = {{0,0},{(uint32_t)W,(uint32_t)H}};
            rpBegin.clearValueCount = 2; rpBegin.pClearValues = clearVals;

            vkCmdBeginRenderPass(cmds[frame], &rpBegin, VK_SUBPASS_CONTENTS_INLINE);
            vkCmdBindPipeline(cmds[frame], VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            vkCmdPushConstants(cmds[frame], pipelineLayout,
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                               0, sizeof(float), &uTime);
            vkCmdDraw(cmds[frame], VERT_COUNT, 1, 0, 0);
            vkCmdEndRenderPass(cmds[frame]);
            VK(vkEndCommandBuffer(cmds[frame]));
        }

        VkSubmitInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = NFRAMES;
        si.pCommandBuffers    = cmds.data();

        auto t0 = std::chrono::high_resolution_clock::now();
        VK(vkQueueSubmit(queue, 1, &si, fence));
        VK(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX));
        auto t1 = std::chrono::high_resolution_clock::now();

        double total_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        std::cout << "[" << animName << "] Vulkan avg: " << total_ms / NFRAMES
                  << " ms/frame  (" << 1000.0 * NFRAMES / total_ms << " fps)\n";

        vkFreeCommandBuffers(device, cmdPool, NFRAMES, cmds.data());
        vkDestroyFence(device, fence, nullptr);
        vkDestroyCommandPool(device, cmdPool, nullptr);
        vkDestroyPipeline(device, pipeline, nullptr);
        vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
        vkDestroyShaderModule(device, vertMod, nullptr);
        vkDestroyShaderModule(device, fragMod, nullptr);
        vkDestroyFramebuffer(device, framebuffer, nullptr);
        vkDestroyRenderPass(device, renderPass, nullptr);
        vkDestroyImageView(device, depthView, nullptr);
        vkDestroyImage(device, depthImage, nullptr);
        vkFreeMemory(device, depthMem, nullptr);
        vkDestroyImageView(device, colorView, nullptr);
        vkDestroyImage(device, colorImage, nullptr);
        vkFreeMemory(device, imgMem, nullptr);
        vkDestroyBuffer(device, readBuf, nullptr);
        vkFreeMemory(device, readMem, nullptr);
        vkDestroyDevice(device, nullptr);
        vkDestroyInstance(instance, nullptr);
        return 0;
    }

    // ── Animation render loop (per-frame submit + readback → ffmpeg) ──────────
    std::vector<uint8_t> pixels(W * H * 4);
    double total_ms = 0.0;

    for (int frame = 0; frame < NFRAMES; ++frame) {
        float uTime = frame / FPS;
        auto t0 = std::chrono::high_resolution_clock::now();

        // Record
        VkCommandBufferBeginInfo beginI{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        VK(vkBeginCommandBuffer(cmd, &beginI));

        VkClearValue clearVals[2]{};
        clearVals[0].color = {{0.f, 0.f, 0.f, 1.f}};
        clearVals[1].depthStencil = {1.f, 0};
        VkRenderPassBeginInfo rpBegin{};
        rpBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rpBegin.renderPass  = renderPass;
        rpBegin.framebuffer = framebuffer;
        rpBegin.renderArea  = {{0,0},{(uint32_t)W,(uint32_t)H}};
        rpBegin.clearValueCount = 2; rpBegin.pClearValues = clearVals;

        vkCmdBeginRenderPass(cmd, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        vkCmdPushConstants(cmd, pipelineLayout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(float), &uTime);
        vkCmdDraw(cmd, VERT_COUNT, 1, 0, 0);
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
        auto t1 = std::chrono::high_resolution_clock::now();
        total_ms += std::chrono::duration<double, std::milli>(t1 - t0).count();

        // Read pixels
        void* mapped;
        VK(vkMapMemory(device, readMem, 0, readSz, 0, &mapped));
        std::memcpy(pixels.data(), mapped, pixels.size());
        vkUnmapMemory(device, readMem);

        // Pipe raw RGB frame to ffmpeg (open pipe on first frame)
        if (!ffpipe) {
            ffpipe = popen(ff_cmd, "w");
            if (!ffpipe) { std::cerr << "Cannot open ffmpeg pipe\n"; std::exit(1); }
        }
        writeFrameRGB(ffpipe, W, H, pixels.data());
        std::cout << "[" << animName << "] frame " << frame
                  << " (t=" << uTime << ")\n";

        VK(vkResetCommandBuffer(cmd, 0));
    }

    if (ffpipe) pclose(ffpipe);
    std::cout << "[" << animName << "] Vulkan avg: " << total_ms / NFRAMES
              << " ms/frame  (" << 1000.0 * NFRAMES / total_ms << " fps)\n";
    std::cout << "[" << animName << "] MP4: " << mp4 << "\n";

    // ── Cleanup ───────────────────────────────────────────────────────────────
    vkDestroyFence(device, fence, nullptr);
    vkDestroyCommandPool(device, cmdPool, nullptr);
    vkDestroyPipeline(device, pipeline, nullptr);
    vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
    vkDestroyShaderModule(device, vertMod, nullptr);
    vkDestroyShaderModule(device, fragMod, nullptr);
    vkDestroyFramebuffer(device, framebuffer, nullptr);
    vkDestroyRenderPass(device, renderPass, nullptr);
    vkDestroyImageView(device, depthView, nullptr);
    vkDestroyImage(device, depthImage, nullptr);
    vkFreeMemory(device, depthMem, nullptr);
    vkDestroyImageView(device, colorView, nullptr);
    vkDestroyImage(device, colorImage, nullptr);
    vkFreeMemory(device, imgMem, nullptr);
    vkDestroyBuffer(device, readBuf, nullptr);
    vkFreeMemory(device, readMem, nullptr);
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);

    return 0;
}