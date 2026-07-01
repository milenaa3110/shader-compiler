// vk_host_mesh.cpp — Vulkan offscreen mesh renderer (indexed VBO + IBO).
// Loads either a procedural icosphere (subdivision-controlled tri count) or
// a Wavefront OBJ file, renders NFRAMES frames with the shader pair compiled
// from mesh_vs_vk.src + mesh_fs.src, encodes an MP4.
//
// Usage:
//   ./spirv_vulkan_mesh_host <vert.spv> <frag.spv> <name> [nframes] [mesh-spec]
//   <mesh-spec>:
//     icosphere:N        — generate a unit icosphere with N subdivision levels
//                          (0 → 20 tris, 1 → 80, 2 → 320, 3 → 1280, 4 → 5120, ...)
//     <path/to/file.obj> — load an OBJ file (positions + normals; quads → tris)
//
// Defaults: name=mesh, nframes=300, mesh-spec=icosphere:3 (1280 tris)

#include "icosphere.h"
#include "mesh_data.h"
#include "obj_loader.h"
#include "../../src/common/error_utils_fmt.h"
#include "vk_pick_device.h"

#include <vulkan/vulkan.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#include "stb_image.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <sys/stat.h>
#include <vector>

static constexpr VkFormat COLOR_FMT = VK_FORMAT_R8G8B8A8_UNORM;
static constexpr VkFormat DEPTH_FMT = VK_FORMAT_D32_SFLOAT;
static int   W = 768, H = 768, NFRAMES = 300;
static float FPS = 30.f;

static void check(VkResult r, const char* where) {
    if (r != VK_SUCCESS) {
        logErrorFmt("Vulkan error {} at {}", (int)r, where);
        std::exit(1);
    }
}
#define VK(expr) check((expr), #expr)

static std::vector<uint32_t> readSpv(const char* path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) {
        logErrorFmt("Cannot open {}", path);
        std::exit(1);
    }
    size_t sz = f.tellg(); f.seekg(0);
    std::vector<uint32_t> data(sz / 4);
    f.read(reinterpret_cast<char*>(data.data()), sz);
    return data;
}

static void writePPM(const char* path, int w, int h, const uint8_t* rgba) {
    FILE* f = std::fopen(path, "wb");
    if (!f) return;
    std::fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; ++i) std::fwrite(rgba + i * 4, 1, 3, f);
    std::fclose(f);
}

static Mesh loadMesh(const std::string& spec) {
    if (spec.rfind("icosphere:", 0) == 0) {
        int subs = std::atoi(spec.c_str() + 10);
        if (subs < 0) subs = 0;
        if (subs > 7) subs = 7;          // 327k tris cap
        Mesh m = icosphere::generate(subs);
        std::cerr << "[mesh] icosphere subs=" << subs
                  << ": " << m.vertices.size() << " verts, "
                  << m.triangleCount() << " tris\n";
        return m;
    }
    Mesh m;
    if (!obj::load(spec.c_str(), m)) std::exit(1);
    obj::normalize_to_unit(m);
    return m;
}

int main(int argc, char** argv) {
    const char* vertSpv  = (argc > 1) ? argv[1] : "build/spirv/mesh.vert.spv";
    const char* fragSpv  = (argc > 2) ? argv[2] : "build/spirv/mesh.frag.spv";
    const char* animName = (argc > 3) ? argv[3] : "mesh";
    if (argc > 4) NFRAMES = std::atoi(argv[4]);
    std::string meshSpec = (argc > 5) ? argv[5] : "icosphere:3";

    Mesh mesh = loadMesh(meshSpec);
    if (mesh.indices.empty()) {
        logError("Mesh has no triangles");
        return 1;
    }

    // ── Vulkan instance ──────────────────────────────────────────────────────
    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "vk_host_mesh";
    appInfo.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo iCI{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    iCI.pApplicationInfo = &appInfo;
    VkInstance instance;
    VK(vkCreateInstance(&iCI, nullptr, &instance));

    uint32_t pdCount = 0; vkEnumeratePhysicalDevices(instance, &pdCount, nullptr);
    if (pdCount == 0) { logError("No Vulkan device"); return 1; }
    std::vector<VkPhysicalDevice> pds(pdCount);
    vkEnumeratePhysicalDevices(instance, &pdCount, pds.data());
    VkPhysicalDevice phys = vkpick::best(pds);

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(phys, &props);
    std::cout << "Vulkan device: " << props.deviceName << "\n";

    uint32_t qFamCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &qFamCount, nullptr);
    std::vector<VkQueueFamilyProperties> qFams(qFamCount);
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &qFamCount, qFams.data());
    uint32_t qFamilyIdx = 0;
    for (uint32_t i = 0; i < qFamCount; ++i)
        if (qFams[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { qFamilyIdx = i; break; }

    float qPrio = 1.f;
    VkDeviceQueueCreateInfo qCI{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qCI.queueFamilyIndex = qFamilyIdx; qCI.queueCount = 1; qCI.pQueuePriorities = &qPrio;
    VkDeviceCreateInfo dCI{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dCI.queueCreateInfoCount = 1; dCI.pQueueCreateInfos = &qCI;
    VkDevice device;
    VK(vkCreateDevice(phys, &dCI, nullptr, &device));
    VkQueue queue; vkGetDeviceQueue(device, qFamilyIdx, 0, &queue);

    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(phys, &memProps);
    auto findMem = [&](uint32_t mask, VkMemoryPropertyFlags want) -> uint32_t {
        for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i)
            if ((mask & (1u << i)) &&
                (memProps.memoryTypes[i].propertyFlags & want) == want)
                return i;
        logError("No matching memory type"); std::exit(1);
    };

    auto makeBuffer = [&](VkDeviceSize sz, VkBufferUsageFlags usage,
                          VkMemoryPropertyFlags props,
                          VkBuffer& buf, VkDeviceMemory& mem) {
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = sz; bci.usage = usage;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VK(vkCreateBuffer(device, &bci, nullptr, &buf));
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(device, buf, &req);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize  = req.size;
        ai.memoryTypeIndex = findMem(req.memoryTypeBits, props);
        VK(vkAllocateMemory(device, &ai, nullptr, &mem));
        VK(vkBindBufferMemory(device, buf, mem, 0));
    };

    // ── Color + depth attachments ─────────────────────────────────────────────
    VkImageCreateInfo colorCI{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    colorCI.imageType = VK_IMAGE_TYPE_2D; colorCI.format = COLOR_FMT;
    colorCI.extent = {(uint32_t)W, (uint32_t)H, 1};
    colorCI.mipLevels = 1; colorCI.arrayLayers = 1;
    colorCI.samples = VK_SAMPLE_COUNT_1_BIT;
    colorCI.tiling = VK_IMAGE_TILING_OPTIMAL;
    colorCI.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    colorCI.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage colorImg; VkDeviceMemory colorMem;
    VK(vkCreateImage(device, &colorCI, nullptr, &colorImg));
    {
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(device, colorImg, &req);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize  = req.size;
        ai.memoryTypeIndex = findMem(req.memoryTypeBits,
                                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VK(vkAllocateMemory(device, &ai, nullptr, &colorMem));
        VK(vkBindImageMemory(device, colorImg, colorMem, 0));
    }

    VkImageCreateInfo depthCI = colorCI;
    depthCI.format = DEPTH_FMT;
    depthCI.usage  = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    VkImage depthImg; VkDeviceMemory depthMem;
    VK(vkCreateImage(device, &depthCI, nullptr, &depthImg));
    {
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(device, depthImg, &req);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize  = req.size;
        ai.memoryTypeIndex = findMem(req.memoryTypeBits,
                                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VK(vkAllocateMemory(device, &ai, nullptr, &depthMem));
        VK(vkBindImageMemory(device, depthImg, depthMem, 0));
    }

    auto makeView = [&](VkImage img, VkFormat fmt, VkImageAspectFlags aspect) {
        VkImageViewCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        ci.image = img; ci.viewType = VK_IMAGE_VIEW_TYPE_2D; ci.format = fmt;
        ci.subresourceRange = {aspect, 0, 1, 0, 1};
        VkImageView v; VK(vkCreateImageView(device, &ci, nullptr, &v));
        return v;
    };
    VkImageView colorView = makeView(colorImg, COLOR_FMT, VK_IMAGE_ASPECT_COLOR_BIT);
    VkImageView depthView = makeView(depthImg, DEPTH_FMT, VK_IMAGE_ASPECT_DEPTH_BIT);

    // ── VBO + IBO via staging ─────────────────────────────────────────────────
    VkDeviceSize vboSize = sizeof(Vertex)   * mesh.vertices.size();
    VkDeviceSize iboSize = sizeof(uint32_t) * mesh.indices.size();
    VkBuffer vbo, ibo, staging;
    VkDeviceMemory vboMem, iboMem, stagingMem;
    makeBuffer(vboSize,
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, vbo, vboMem);
    makeBuffer(iboSize,
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT  | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, ibo, iboMem);
    VkDeviceSize stagingSize = std::max(vboSize, iboSize);
    makeBuffer(stagingSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        staging, stagingMem);

    // Helper: copy `data` (sz bytes) into `dst` GPU buffer via staging.
    VkCommandPoolCreateInfo cpCI{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpCI.queueFamilyIndex = qFamilyIdx;
    cpCI.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VkCommandPool cmdPool; VK(vkCreateCommandPool(device, &cpCI, nullptr, &cmdPool));

    auto uploadBuffer = [&](VkBuffer dst, const void* data, VkDeviceSize sz) {
        void* m;
        VK(vkMapMemory(device, stagingMem, 0, sz, 0, &m));
        std::memcpy(m, data, sz);
        vkUnmapMemory(device, stagingMem);

        VkCommandBufferAllocateInfo cbAI{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cbAI.commandPool = cmdPool;
        cbAI.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbAI.commandBufferCount = 1;
        VkCommandBuffer cmd; VK(vkAllocateCommandBuffers(device, &cbAI, &cmd));
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VK(vkBeginCommandBuffer(cmd, &bi));
        VkBufferCopy cp{0, 0, sz};
        vkCmdCopyBuffer(cmd, staging, dst, 1, &cp);
        VK(vkEndCommandBuffer(cmd));
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
        VK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
        VK(vkQueueWaitIdle(queue));
        vkFreeCommandBuffers(device, cmdPool, 1, &cmd);
    };
    uploadBuffer(vbo, mesh.vertices.data(), vboSize);
    uploadBuffer(ibo, mesh.indices.data(),  iboSize);

    // ── Per-material GPU resources ────────────────────────────────────────────
    // Each material gets its own VkImage / VkImageView / VkDescriptorSet so
    // we can switch between them per draw range. Materials without map_Kd
    // get a 1×1 white image so the same pipeline can be used; uKd then
    // carries their colour.
    struct MatGPU {
        VkImage         img    = VK_NULL_HANDLE;
        VkDeviceMemory  mem    = VK_NULL_HANDLE;
        VkImageView     view   = VK_NULL_HANDLE;
        VkDescriptorSet ds     = VK_NULL_HANDLE;
        float           kd[4]  = {1.f, 1.f, 1.f, 1.f};
    };

    // Shared sampler for all materials.
    VkSamplerCreateInfo sCI{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sCI.magFilter = VK_FILTER_LINEAR;
    sCI.minFilter = VK_FILTER_LINEAR;
    sCI.addressModeU = sCI.addressModeV = sCI.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sCI.maxLod = 0.f;
    VkSampler sampler;
    VK(vkCreateSampler(device, &sCI, nullptr, &sampler));

    // Shared descriptor set layout (binding 0 = combined image sampler).
    VkDescriptorSetLayoutBinding dslb{};
    dslb.binding = 0;
    dslb.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    dslb.descriptorCount = 1;
    dslb.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo dslCI{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dslCI.bindingCount = 1; dslCI.pBindings = &dslb;
    VkDescriptorSetLayout dsLayout;
    VK(vkCreateDescriptorSetLayout(device, &dslCI, nullptr, &dsLayout));

    // Pool sized for every material plus one fallback "default" slot used
    // when the OBJ has no MTL at all (icosphere / bunny).
    uint32_t numMatSlots = (uint32_t)mesh.materials.size() + 1u;
    VkDescriptorPoolSize dpSz{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, numMatSlots};
    VkDescriptorPoolCreateInfo dpCI{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpCI.maxSets = numMatSlots; dpCI.poolSizeCount = 1; dpCI.pPoolSizes = &dpSz;
    VkDescriptorPool descPool;
    VK(vkCreateDescriptorPool(device, &dpCI, nullptr, &descPool));

    // Helper: upload one material's image + create view + alloc + write descriptor.
    auto uploadMaterial = [&](const uint8_t* rgba, int w, int h, MatGPU& out) {
        VkDeviceSize bytes = (VkDeviceSize)w * h * 4;

        VkBuffer stg; VkDeviceMemory stgMem;
        makeBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            stg, stgMem);
        { void* m; VK(vkMapMemory(device, stgMem, 0, bytes, 0, &m));
          std::memcpy(m, rgba, bytes); vkUnmapMemory(device, stgMem); }

        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format    = VK_FORMAT_R8G8B8A8_UNORM;
        ci.extent    = {(uint32_t)w, (uint32_t)h, 1};
        ci.mipLevels = 1; ci.arrayLayers = 1;
        ci.samples   = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling    = VK_IMAGE_TILING_OPTIMAL;
        ci.usage     = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VK(vkCreateImage(device, &ci, nullptr, &out.img));
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(device, out.img, &req);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize  = req.size;
        ai.memoryTypeIndex = findMem(req.memoryTypeBits,
                                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VK(vkAllocateMemory(device, &ai, nullptr, &out.mem));
        VK(vkBindImageMemory(device, out.img, out.mem, 0));

        // Upload via a one-shot command buffer with the standard
        // UNDEFINED → TRANSFER_DST → SHADER_READ_ONLY transitions.
        VkCommandBufferAllocateInfo cbAI{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cbAI.commandPool = cmdPool;
        cbAI.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbAI.commandBufferCount = 1;
        VkCommandBuffer cmd; VK(vkAllocateCommandBuffers(device, &cbAI, &cmd));
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VK(vkBeginCommandBuffer(cmd, &bi));

        VkImageMemoryBarrier b1{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b1.srcAccessMask = 0;
        b1.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b1.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b1.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b1.srcQueueFamilyIndex = b1.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b1.image = out.img;
        b1.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                             0, nullptr, 0, nullptr, 1, &b1);

        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent      = {(uint32_t)w, (uint32_t)h, 1};
        vkCmdCopyBufferToImage(cmd, stg, out.img,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        VkImageMemoryBarrier b2 = b1;
        b2.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b2.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        b2.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b2.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                             0, nullptr, 0, nullptr, 1, &b2);
        VK(vkEndCommandBuffer(cmd));
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
        VK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
        VK(vkQueueWaitIdle(queue));
        vkFreeCommandBuffers(device, cmdPool, 1, &cmd);
        vkDestroyBuffer(device, stg, nullptr);
        vkFreeMemory(device, stgMem, nullptr);

        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = out.img;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = VK_FORMAT_R8G8B8A8_UNORM;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VK(vkCreateImageView(device, &vi, nullptr, &out.view));

        VkDescriptorSetAllocateInfo dsAI{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dsAI.descriptorPool = descPool;
        dsAI.descriptorSetCount = 1; dsAI.pSetLayouts = &dsLayout;
        VK(vkAllocateDescriptorSets(device, &dsAI, &out.ds));
        VkDescriptorImageInfo dii{};
        dii.sampler = sampler; dii.imageView = out.view;
        dii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkWriteDescriptorSet wds{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        wds.dstSet = out.ds; wds.dstBinding = 0;
        wds.descriptorCount = 1;
        wds.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        wds.pImageInfo = &dii;
        vkUpdateDescriptorSets(device, 1, &wds, 0, nullptr);
    };

    // Load each material: real texture if map_Kd is set, otherwise 1×1 white.
    std::vector<MatGPU> matGPU(mesh.materials.size());
    for (size_t m = 0; m < mesh.materials.size(); ++m) {
        const auto& mat = mesh.materials[m];
        matGPU[m].kd[0] = mat.diffuse[0];
        matGPU[m].kd[1] = mat.diffuse[1];
        matGPU[m].kd[2] = mat.diffuse[2];
        matGPU[m].kd[3] = 1.f;
        if (!mat.diffuseMap.empty()) {
            int w, h, n;
            uint8_t* img = stbi_load(mat.diffuseMap.c_str(), &w, &h, &n, 4);
            if (!img) {
                logErrorFmt("Failed to load texture {}: {}", mat.diffuseMap, stbi_failure_reason());
                std::exit(1);
            }
            std::cerr << "[mesh] texture " << mat.diffuseMap << ": "
                      << w << "x" << h << " RGBA\n";
            uploadMaterial(img, w, h, matGPU[m]);
            stbi_image_free(img);
        } else {
            uint8_t white[4] = {255, 255, 255, 255};
            uploadMaterial(white, 1, 1, matGPU[m]);
        }
    }
    // Default slot: 1×1 white image with a visibly-warm tan Kd, used for
    // meshes that have no MTL at all. (See rv_host_mesh.cpp for why the value
    // changed from (234,214,184) — the older near-white pale-beige clamped
    // to 255 in highlights and the bunny read as plain white.)
    MatGPU defaultMat;
    defaultMat.kd[0] = 210.f/255; defaultMat.kd[1] = 170.f/255;
    defaultMat.kd[2] = 130.f/255; defaultMat.kd[3] = 1.f;
    {
        uint8_t white[4] = {255, 255, 255, 255};
        uploadMaterial(white, 1, 1, defaultMat);
    }

    // ── Render pass + framebuffer ─────────────────────────────────────────────
    VkAttachmentDescription atts[2]{};
    atts[0].format = COLOR_FMT; atts[0].samples = VK_SAMPLE_COUNT_1_BIT;
    atts[0].loadOp  = VK_ATTACHMENT_LOAD_OP_CLEAR;
    atts[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    atts[0].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    atts[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    atts[0].initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    atts[0].finalLayout    = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    atts[1].format = DEPTH_FMT; atts[1].samples = VK_SAMPLE_COUNT_1_BIT;
    atts[1].loadOp  = VK_ATTACHMENT_LOAD_OP_CLEAR;
    atts[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    atts[1].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    atts[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    atts[1].initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    atts[1].finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depthRef{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1; sub.pColorAttachments = &colorRef;
    sub.pDepthStencilAttachment = &depthRef;
    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL; dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                       VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dep.dstStageMask = dep.srcStageMask;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo rpCI{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpCI.attachmentCount = 2; rpCI.pAttachments = atts;
    rpCI.subpassCount = 1;    rpCI.pSubpasses   = &sub;
    rpCI.dependencyCount = 1; rpCI.pDependencies = &dep;
    VkRenderPass renderPass; VK(vkCreateRenderPass(device, &rpCI, nullptr, &renderPass));

    VkImageView fbViews[2] = {colorView, depthView};
    VkFramebufferCreateInfo fbCI{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fbCI.renderPass = renderPass; fbCI.attachmentCount = 2;
    fbCI.pAttachments = fbViews; fbCI.width = W; fbCI.height = H; fbCI.layers = 1;
    VkFramebuffer framebuffer;
    VK(vkCreateFramebuffer(device, &fbCI, nullptr, &framebuffer));

    // ── Shaders + pipeline layout (push: float uTime) ─────────────────────────
    auto makeShader = [&](const char* path) -> VkShaderModule {
        auto code = readSpv(path);
        VkShaderModuleCreateInfo smCI{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        smCI.codeSize = code.size() * 4; smCI.pCode = code.data();
        VkShaderModule sm; VK(vkCreateShaderModule(device, &smCI, nullptr, &sm));
        return sm;
    };
    VkShaderModule vsMod = makeShader(vertSpv);
    VkShaderModule fsMod = makeShader(fragSpv);

    // Push constants: { float uTime @ 0; float pad[3]; float uKd[4] @ 16 } = 32 bytes.
    // The 16-byte gap matches the std140 vec4 alignment in the FS uniform block,
    // so the same blob is read identically by VS (only uTime) and FS (uTime+uKd).
    struct MeshPC {
        float uTime;
        float pad[3];
        float uKd[4];
    };
    static_assert(sizeof(MeshPC) == 32, "Push-constant layout mismatch");
    VkPushConstantRange pcr{
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        0, sizeof(MeshPC)
    };
    VkPipelineLayoutCreateInfo plCI{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plCI.setLayoutCount = 1;          plCI.pSetLayouts = &dsLayout;
    plCI.pushConstantRangeCount = 1;  plCI.pPushConstantRanges = &pcr;
    VkPipelineLayout pipeLayout;
    VK(vkCreatePipelineLayout(device, &plCI, nullptr, &pipeLayout));

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;   stages[0].module = vsMod; stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = fsMod; stages[1].pName = "main";

    // ── Vertex input (binding 0 → Vertex {pos, normal, uv}) ───────────────────
    VkVertexInputBindingDescription bind{};
    bind.binding = 0; bind.stride = sizeof(Vertex); bind.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    VkVertexInputAttributeDescription attrs[3]{};
    attrs[0].location = 0; attrs[0].binding = 0;
    attrs[0].format = VK_FORMAT_R32G32B32_SFLOAT; attrs[0].offset = offsetof(Vertex, pos);
    attrs[1].location = 1; attrs[1].binding = 0;
    attrs[1].format = VK_FORMAT_R32G32B32_SFLOAT; attrs[1].offset = offsetof(Vertex, normal);
    attrs[2].location = 2; attrs[2].binding = 0;
    attrs[2].format = VK_FORMAT_R32G32_SFLOAT;    attrs[2].offset = offsetof(Vertex, uv);
    VkPipelineVertexInputStateCreateInfo viState{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    viState.vertexBindingDescriptionCount = 1;   viState.pVertexBindingDescriptions   = &bind;
    viState.vertexAttributeDescriptionCount = 3; viState.pVertexAttributeDescriptions = attrs;

    VkPipelineInputAssemblyStateCreateInfo iaState{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    iaState.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    // Negative-height viewport (VK 1.1 / VK_KHR_maintenance1) flips the Y axis
    // at the viewport stage. This lets us share the VS source with the RISC-V
    // software rasterizer (OpenGL-style NDC: y up) instead of negating
    // gl_Position.y in the shader.
    VkViewport viewport{0, (float)H, (float)W, -(float)H, 0.f, 1.f};
    VkRect2D scissor{{0,0},{(uint32_t)W,(uint32_t)H}};
    VkPipelineViewportStateCreateInfo vpState{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vpState.viewportCount = 1; vpState.pViewports = &viewport;
    vpState.scissorCount  = 1; vpState.pScissors  = &scissor;

    VkPipelineRasterizationStateCreateInfo rsState{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rsState.polygonMode = VK_POLYGON_MODE_FILL;
    rsState.cullMode    = VK_CULL_MODE_BACK_BIT;
    rsState.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rsState.lineWidth   = 1.f;

    VkPipelineMultisampleStateCreateInfo msState{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    msState.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo dsState{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    dsState.depthTestEnable  = VK_TRUE;
    dsState.depthWriteEnable = VK_TRUE;
    dsState.depthCompareOp   = VK_COMPARE_OP_LESS;

    VkPipelineColorBlendAttachmentState cbAtt{};
    cbAtt.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cbState{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cbState.attachmentCount = 1; cbState.pAttachments = &cbAtt;

    VkGraphicsPipelineCreateInfo gpCI{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gpCI.stageCount = 2; gpCI.pStages = stages;
    gpCI.pVertexInputState   = &viState;
    gpCI.pInputAssemblyState = &iaState;
    gpCI.pViewportState      = &vpState;
    gpCI.pRasterizationState = &rsState;
    gpCI.pMultisampleState   = &msState;
    gpCI.pDepthStencilState  = &dsState;
    gpCI.pColorBlendState    = &cbState;
    gpCI.layout = pipeLayout;
    gpCI.renderPass = renderPass;
    VkPipeline pipeline;
    VK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gpCI, nullptr, &pipeline));

    // ── Readback buffer ───────────────────────────────────────────────────────
    VkDeviceSize readSz = (VkDeviceSize)W * H * 4;
    VkBuffer readBuf; VkDeviceMemory readMem;
    makeBuffer(readSz, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        readBuf, readMem);

    // ── Per-frame command buffer ──────────────────────────────────────────────
    VkCommandBufferAllocateInfo cbAI{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbAI.commandPool = cmdPool;
    cbAI.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbAI.commandBufferCount = 1;
    VkCommandBuffer cmd; VK(vkAllocateCommandBuffers(device, &cbAI, &cmd));

    VkFenceCreateInfo fenceCI{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence; VK(vkCreateFence(device, &fenceCI, nullptr, &fence));

    mkdir("result", 0755);
    char mp4[256], ff_cmd[512];
    std::snprintf(mp4, sizeof(mp4), "result/%s.mp4", animName);
    std::snprintf(ff_cmd, sizeof(ff_cmd),
        "ffmpeg -y -f rawvideo -pixel_format rgb24 -video_size %dx%d "
        "-framerate %d -i pipe:0 -c:v libx264 -pix_fmt yuv420p -crf 18 %s 2>/dev/null",
        W, H, (int)FPS, mp4);
    FILE* ffpipe = nullptr;

    std::vector<uint8_t> pixels(W * H * 4);
    double total_ms = 0.0;
    uint32_t indexCount = (uint32_t)mesh.indices.size();

    for (int frame = 0; frame < NFRAMES; ++frame) {
        auto t0 = std::chrono::high_resolution_clock::now();

        MeshPC pc{};
        pc.uTime = frame / FPS;

        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        VK(vkBeginCommandBuffer(cmd, &bi));

        VkClearValue clearVals[2]{};
        clearVals[0].color = {{0.05f, 0.06f, 0.08f, 1.f}};
        clearVals[1].depthStencil = {1.f, 0};
        VkRenderPassBeginInfo rpBegin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        rpBegin.renderPass  = renderPass;
        rpBegin.framebuffer = framebuffer;
        rpBegin.renderArea  = {{0,0},{(uint32_t)W,(uint32_t)H}};
        rpBegin.clearValueCount = 2; rpBegin.pClearValues = clearVals;

        vkCmdBeginRenderPass(cmd, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        VkDeviceSize off = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &vbo, &off);
        vkCmdBindIndexBuffer(cmd, ibo, 0, VK_INDEX_TYPE_UINT32);

        // Per draw range: bind that material's descriptor + push its uKd,
        // then issue a vkCmdDrawIndexed for the range. If the OBJ has no MTL
        // (icosphere / bunny) fall back to one big draw with the default
        // beige Kd / 1×1 white texture.
        auto issueDraw = [&](uint32_t firstIndex, uint32_t ic, const MatGPU& m) {
            std::memcpy(pc.uKd, m.kd, sizeof(pc.uKd));
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeLayout,
                                    0, 1, &m.ds, 0, nullptr);
            vkCmdPushConstants(cmd, pipeLayout,
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                               0, sizeof(MeshPC), &pc);
            vkCmdDrawIndexed(cmd, ic, 1, firstIndex, 0, 0);
        };

        if (mesh.ranges.empty()) {
            issueDraw(0, indexCount, defaultMat);
        } else {
            for (const auto& r : mesh.ranges) {
                int m = (r.materialId >= 0 && r.materialId < (int)matGPU.size())
                          ? r.materialId : 0;
                issueDraw(r.firstIndex, r.indexCount, matGPU[m]);
            }
        }
        vkCmdEndRenderPass(cmd);

        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent      = {(uint32_t)W, (uint32_t)H, 1};
        vkCmdCopyImageToBuffer(cmd, colorImg,
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               readBuf, 1, &region);
        VK(vkEndCommandBuffer(cmd));

        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
        VK(vkQueueSubmit(queue, 1, &si, fence));
        VK(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX));
        VK(vkResetFences(device, 1, &fence));
        VK(vkResetCommandBuffer(cmd, 0));

        auto t1 = std::chrono::high_resolution_clock::now();
        total_ms += std::chrono::duration<double, std::milli>(t1 - t0).count();

        void* mapped;
        VK(vkMapMemory(device, readMem, 0, readSz, 0, &mapped));
        std::memcpy(pixels.data(), mapped, pixels.size());
        vkUnmapMemory(device, readMem);

        if (!ffpipe) ffpipe = popen(ff_cmd, "w");
        if (ffpipe)
            for (int i = 0; i < W * H; ++i) std::fwrite(pixels.data() + i*4, 1, 3, ffpipe);

        // Write PPM at a fixed frame index so cross-backend comparisons land
        // on the same rotation regardless of NFRAMES (RV defaults to 60, GPU
        // to 300). Falls back to NFRAMES/2 for very short runs.
        int ppm_frame = (NFRAMES >= 60) ? 30 : (NFRAMES / 2);
        if (frame == ppm_frame) {
            char ppm[256];
            std::snprintf(ppm, sizeof(ppm), "result/%s.ppm", animName);
            writePPM(ppm, W, H, pixels.data());
        }
    }
    if (ffpipe) pclose(ffpipe);

    std::cout << "[" << animName << "] tris: " << mesh.triangleCount()
              << ", verts: " << mesh.vertices.size()
              << ", avg: " << total_ms / NFRAMES << " ms/frame  ("
              << 1000.0 * NFRAMES / total_ms << " fps)\n";
    std::cout << "[" << animName << "] Output: " << mp4 << "\n";

    // Cleanup (omit per-frame destroy ordering — process exit will reclaim).
    vkDestroyFence(device, fence, nullptr);
    vkDestroyCommandPool(device, cmdPool, nullptr);
    vkDestroyPipeline(device, pipeline, nullptr);
    vkDestroyPipelineLayout(device, pipeLayout, nullptr);
    vkDestroyShaderModule(device, vsMod, nullptr);
    vkDestroyShaderModule(device, fsMod, nullptr);
    vkDestroyFramebuffer(device, framebuffer, nullptr);
    vkDestroyRenderPass(device, renderPass, nullptr);
    vkDestroyImageView(device, depthView, nullptr);
    vkDestroyImage(device, depthImg, nullptr);   vkFreeMemory(device, depthMem, nullptr);
    vkDestroyImageView(device, colorView, nullptr);
    vkDestroyImage(device, colorImg, nullptr);   vkFreeMemory(device, colorMem, nullptr);
    vkDestroyBuffer(device, readBuf, nullptr);   vkFreeMemory(device, readMem, nullptr);
    vkDestroyBuffer(device, vbo, nullptr);       vkFreeMemory(device, vboMem, nullptr);
    vkDestroyBuffer(device, ibo, nullptr);       vkFreeMemory(device, iboMem, nullptr);
    vkDestroyBuffer(device, staging, nullptr);   vkFreeMemory(device, stagingMem, nullptr);
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);
    return 0;
}
