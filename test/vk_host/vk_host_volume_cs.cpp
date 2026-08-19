// vk_host_volume_cs.cpp — GPU counterpart of test/rv_host/rv_host_volume_cs.cpp:
// dispatches volume_cs, which samples a 3D texture and writes the result through
// imageStore into a storage image.
//
// Two things here that no other Vulkan host in the tree needed:
//   * a VK_IMAGE_TYPE_3D sampled image bound as a combined image sampler, and
//   * device features shaderStorageImageRead/WriteWithoutFormat, required
//     because the shader's OpTypeImage has format Unknown (the emitter has no
//     format qualifier to carry through). Without them the module is rejected
//     at pipeline creation, not at validation.
//
// Shader interface (see `spirv-dis result/volume.comp.spv`):
//   push constant  { float uTime; uint uW; uint uH; }   offsets 0/4/8, compute
//   set 0 binding 0  combined image sampler, OpTypeImage %float 3D  (sampled)
//   set 0 binding 1  storage image,          OpTypeImage %float 2D  (Sampled=2)
//   local size 16x16x1

#include <sys/stat.h>
#include <vulkan/vulkan.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <vector>

#include "../../src/common/error_utils_fmt.h"
#include "../volume_data.h"
#include "vk_pick_device.h"

static constexpr VkFormat IMG_FMT = VK_FORMAT_R32G32B32A32_SFLOAT;
static int W = 256, H = 256, NFRAMES = 90;
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
    logErrorContext("IO", "Failed to open SPIR-V file: " + std::string(path));
    std::exit(1);
  }
  size_t sz = f.tellg();
  f.seekg(0);
  std::vector<uint32_t> data(sz / 4);
  f.read(reinterpret_cast<char*>(data.data()), sz);
  return data;
}

static uint32_t findMemType(VkPhysicalDevice pd, uint32_t bits,
                            VkMemoryPropertyFlags props) {
  VkPhysicalDeviceMemoryProperties mp;
  vkGetPhysicalDeviceMemoryProperties(pd, &mp);
  for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
    if ((bits & (1u << i)) &&
        (mp.memoryTypes[i].propertyFlags & props) == props)
      return i;
  logError("No suitable memory type");
  std::exit(1);
}

struct PushConsts {
  float uTime;
  uint32_t uW, uH;
};

int main(int argc, char** argv) {
  const char* compSpv = (argc > 1) ? argv[1] : "result/volume.comp.spv";
  const char* animName = (argc > 2) ? argv[2] : "volume_cs";
  if (argc > 3) NFRAMES = std::atoi(argv[3]);
  if (argc > 4) W = std::atoi(argv[4]);
  if (argc > 5) H = std::atoi(argv[5]);
  mkdir("result", 0755);

  // ── Instance + device ─────────────────────────────────────────────────────
  VkApplicationInfo appInfo{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  appInfo.pApplicationName = "volume_cs";
  appInfo.apiVersion = VK_API_VERSION_1_1;
  VkInstanceCreateInfo instCI{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  instCI.pApplicationInfo = &appInfo;
  VkInstance instance;
  VK(vkCreateInstance(&instCI, nullptr, &instance));

  uint32_t devCount = 0;
  vkEnumeratePhysicalDevices(instance, &devCount, nullptr);
  if (!devCount) {
    logError("No Vulkan device available");
    return 1;
  }
  std::vector<VkPhysicalDevice> pds(devCount);
  vkEnumeratePhysicalDevices(instance, &devCount, pds.data());
  VkPhysicalDevice pd = vkpick::best(pds);

  VkPhysicalDeviceProperties pdp;
  vkGetPhysicalDeviceProperties(pd, &pdp);
  std::cout << "[" << animName << "] GPU: " << pdp.deviceName << "\n";

  VkPhysicalDeviceFeatures avail{};
  vkGetPhysicalDeviceFeatures(pd, &avail);
  if (!avail.shaderStorageImageWriteWithoutFormat ||
      !avail.shaderStorageImageReadWithoutFormat) {
    logError("device lacks shaderStorageImageRead/WriteWithoutFormat");
    return 1;
  }
  VkPhysicalDeviceFeatures want{};
  want.shaderStorageImageWriteWithoutFormat = VK_TRUE;
  want.shaderStorageImageReadWithoutFormat = VK_TRUE;

  const int N = voldata::kN;
  if ((uint32_t)N > pdp.limits.maxImageDimension3D) {
    logErrorFmt("volume {} exceeds maxImageDimension3D {}", N,
                pdp.limits.maxImageDimension3D);
    return 1;
  }

  uint32_t qfCount = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(pd, &qfCount, nullptr);
  std::vector<VkQueueFamilyProperties> qfs(qfCount);
  vkGetPhysicalDeviceQueueFamilyProperties(pd, &qfCount, qfs.data());
  uint32_t qfi = 0;
  for (uint32_t i = 0; i < qfCount; i++)
    if (qfs[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
      qfi = i;
      break;
    }

  float qprio = 1.f;
  VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  qci.queueFamilyIndex = qfi;
  qci.queueCount = 1;
  qci.pQueuePriorities = &qprio;
  VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  dci.queueCreateInfoCount = 1;
  dci.pQueueCreateInfos = &qci;
  dci.pEnabledFeatures = &want;
  VkDevice dev;
  VK(vkCreateDevice(pd, &dci, nullptr, &dev));
  VkQueue queue;
  vkGetDeviceQueue(dev, qfi, 0, &queue);

  VkCommandPool cmdPool;
  {
    VkCommandPoolCreateInfo cpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpci.queueFamilyIndex = qfi;
    cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VK(vkCreateCommandPool(dev, &cpci, nullptr, &cmdPool));
  }

  auto oneShot = [&](auto&& record) {
    VkCommandBufferAllocateInfo cbai{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbai.commandPool = cmdPool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    VkCommandBuffer c;
    VK(vkAllocateCommandBuffers(dev, &cbai, &c));
    VkCommandBufferBeginInfo cbbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(c, &cbbi);
    record(c);
    vkEndCommandBuffer(c);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &c;
    VkFence f;
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VK(vkCreateFence(dev, &fci, nullptr, &f));
    VK(vkQueueSubmit(queue, 1, &si, f));
    VK(vkWaitForFences(dev, 1, &f, VK_TRUE, UINT64_MAX));
    vkDestroyFence(dev, f, nullptr);
    vkFreeCommandBuffers(dev, cmdPool, 1, &c);
  };

  // ── Bake + upload the 3D volume ───────────────────────────────────────────
  auto tv0 = std::chrono::high_resolution_clock::now();
  std::vector<float> vol = voldata::make(N);
  auto tv1 = std::chrono::high_resolution_clock::now();
  std::cout << "[" << animName << "] baked " << N << "^3 volume in "
            << std::chrono::duration<double, std::milli>(tv1 - tv0).count()
            << " ms\n";

  VkDeviceSize volSize = vol.size() * sizeof(float);
  VkBuffer stageBuf;
  VkDeviceMemory stageMem;
  {
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = volSize;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VK(vkCreateBuffer(dev, &bci, nullptr, &stageBuf));
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(dev, stageBuf, &mr);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = findMemType(pd, mr.memoryTypeBits,
                                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VK(vkAllocateMemory(dev, &mai, nullptr, &stageMem));
    VK(vkBindBufferMemory(dev, stageBuf, stageMem, 0));
    void* ptr;
    VK(vkMapMemory(dev, stageMem, 0, volSize, 0, &ptr));
    std::memcpy(ptr, vol.data(), volSize);
    vkUnmapMemory(dev, stageMem);
  }

  VkImage volImg;
  VkDeviceMemory volMem;
  {
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_3D;
    ici.format = IMG_FMT;
    ici.extent = {(uint32_t)N, (uint32_t)N, (uint32_t)N};
    ici.mipLevels = ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VK(vkCreateImage(dev, &ici, nullptr, &volImg));
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(dev, volImg, &mr);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex =
        findMemType(pd, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VK(vkAllocateMemory(dev, &mai, nullptr, &volMem));
    VK(vkBindImageMemory(dev, volImg, volMem, 0));
  }

  oneShot([&](VkCommandBuffer c) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = volImg;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &b);

    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {(uint32_t)N, (uint32_t)N, (uint32_t)N};
    vkCmdCopyBufferToImage(c, stageBuf, volImg,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &b);
  });

  VkImageView volView;
  {
    VkImageViewCreateInfo ivci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    ivci.image = volImg;
    ivci.viewType = VK_IMAGE_VIEW_TYPE_3D;
    ivci.format = IMG_FMT;
    ivci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VK(vkCreateImageView(dev, &ivci, nullptr, &volView));
  }
  VkSampler sampler;
  {
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    sci.addressModeU = sci.addressModeV = sci.addressModeW =
        VK_SAMPLER_ADDRESS_MODE_REPEAT;
    VK(vkCreateSampler(dev, &sci, nullptr, &sampler));
  }

  // ── Storage image the shader writes ───────────────────────────────────────
  VkImage outImg;
  VkDeviceMemory outMem;
  VkImageView outView;
  {
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = IMG_FMT;
    ici.extent = {(uint32_t)W, (uint32_t)H, 1};
    ici.mipLevels = ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VK(vkCreateImage(dev, &ici, nullptr, &outImg));
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(dev, outImg, &mr);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex =
        findMemType(pd, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VK(vkAllocateMemory(dev, &mai, nullptr, &outMem));
    VK(vkBindImageMemory(dev, outImg, outMem, 0));

    VkImageViewCreateInfo ivci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    ivci.image = outImg;
    ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ivci.format = IMG_FMT;
    ivci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VK(vkCreateImageView(dev, &ivci, nullptr, &outView));
  }

  // A storage image is accessed in GENERAL; it stays there and the per-frame
  // copy is bracketed by barriers rather than layout transitions.
  oneShot([&](VkCommandBuffer c) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = outImg;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    b.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &b);
  });

  VkBuffer readBuf;
  VkDeviceMemory readMem;
  const VkDeviceSize readSize = (VkDeviceSize)W * H * 4 * sizeof(float);
  {
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = readSize;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VK(vkCreateBuffer(dev, &bci, nullptr, &readBuf));
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(dev, readBuf, &mr);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = findMemType(pd, mr.memoryTypeBits,
                                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VK(vkAllocateMemory(dev, &mai, nullptr, &readMem));
    VK(vkBindBufferMemory(dev, readBuf, readMem, 0));
  }

  // ── Descriptors + compute pipeline ────────────────────────────────────────
  VkDescriptorSetLayoutBinding binds[2]{};
  binds[0].binding = 0;
  binds[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  binds[0].descriptorCount = 1;
  binds[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  binds[1].binding = 1;
  binds[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  binds[1].descriptorCount = 1;
  binds[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  VkDescriptorSetLayoutCreateInfo dslci{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  dslci.bindingCount = 2;
  dslci.pBindings = binds;
  VkDescriptorSetLayout dsl;
  VK(vkCreateDescriptorSetLayout(dev, &dslci, nullptr, &dsl));

  VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PushConsts)};
  VkPipelineLayoutCreateInfo plci{
      VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  plci.setLayoutCount = 1;
  plci.pSetLayouts = &dsl;
  plci.pushConstantRangeCount = 1;
  plci.pPushConstantRanges = &pcr;
  VkPipelineLayout pipeLayout;
  VK(vkCreatePipelineLayout(dev, &plci, nullptr, &pipeLayout));

  VkShaderModule compMod;
  {
    auto code = readSpv(compSpv);
    VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smci.codeSize = code.size() * 4;
    smci.pCode = code.data();
    VK(vkCreateShaderModule(dev, &smci, nullptr, &compMod));
  }
  VkComputePipelineCreateInfo cpci{
      VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
  cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  cpci.stage.module = compMod;
  cpci.stage.pName = "main";
  cpci.layout = pipeLayout;
  VkPipeline pipeline;
  VK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, nullptr,
                              &pipeline));

  VkDescriptorPoolSize poolSizes[2] = {
      {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1},
      {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1}};
  VkDescriptorPoolCreateInfo dpci{
      VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  dpci.maxSets = 1;
  dpci.poolSizeCount = 2;
  dpci.pPoolSizes = poolSizes;
  VkDescriptorPool dpool;
  VK(vkCreateDescriptorPool(dev, &dpci, nullptr, &dpool));

  VkDescriptorSetAllocateInfo dsai{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  dsai.descriptorPool = dpool;
  dsai.descriptorSetCount = 1;
  dsai.pSetLayouts = &dsl;
  VkDescriptorSet ds;
  VK(vkAllocateDescriptorSets(dev, &dsai, &ds));

  VkDescriptorImageInfo volInfo{sampler, volView,
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  VkDescriptorImageInfo outInfo{VK_NULL_HANDLE, outView,
                                VK_IMAGE_LAYOUT_GENERAL};
  VkWriteDescriptorSet writes[2]{};
  writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[0].dstSet = ds;
  writes[0].dstBinding = 0;
  writes[0].descriptorCount = 1;
  writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  writes[0].pImageInfo = &volInfo;
  writes[1] = writes[0];
  writes[1].dstBinding = 1;
  writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  writes[1].pImageInfo = &outInfo;
  vkUpdateDescriptorSets(dev, 2, writes, 0, nullptr);

  VkCommandBuffer cmd;
  {
    VkCommandBufferAllocateInfo cbai{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbai.commandPool = cmdPool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    VK(vkAllocateCommandBuffers(dev, &cbai, &cmd));
  }
  VkFence fence;
  {
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VK(vkCreateFence(dev, &fci, nullptr, &fence));
  }

  // ── Dispatch loop ─────────────────────────────────────────────────────────
  const uint32_t gx = (W + 15) / 16, gy = (H + 15) / 16;
  char ff_cmd[512];
  std::snprintf(ff_cmd, sizeof(ff_cmd),
      "ffmpeg -y -f rawvideo -pixel_format rgb24 -video_size %dx%d "
      "-framerate %d -i pipe:0 "
      "-c:v libx264 -pix_fmt yuv420p -crf 20 result/%s.mp4 2>/dev/null",
      W, H, (int)FPS, animName);
  FILE* ffpipe = nullptr;
  std::vector<uint8_t> rgb((size_t)W * H * 3);

  double total_ms = 0.0;
  std::cout << "[" << animName << "] " << W << "x" << H << ", " << NFRAMES
            << " frames, " << gx << "x" << gy << " workgroups\n";

  for (int frame = 0; frame < NFRAMES; frame++) {
    PushConsts pc{frame / FPS, (uint32_t)W, (uint32_t)H};

    VkCommandBufferBeginInfo cbbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    vkBeginCommandBuffer(cmd, &cbbi);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeLayout, 0,
                            1, &ds, 0, nullptr);
    vkCmdPushConstants(cmd, pipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                       sizeof(pc), &pc);
    vkCmdDispatch(cmd, gx, gy, 1);

    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = outImg;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &b);

    VkBufferImageCopy bic{};
    bic.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    bic.imageExtent = {(uint32_t)W, (uint32_t)H, 1};
    vkCmdCopyImageToBuffer(cmd, outImg, VK_IMAGE_LAYOUT_GENERAL, readBuf, 1,
                           &bic);
    vkEndCommandBuffer(cmd);

    auto t0 = std::chrono::high_resolution_clock::now();
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    VK(vkQueueSubmit(queue, 1, &si, fence));
    VK(vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX));
    auto t1 = std::chrono::high_resolution_clock::now();
    total_ms += std::chrono::duration<double, std::milli>(t1 - t0).count();

    vkResetFences(dev, 1, &fence);
    vkResetCommandBuffer(cmd, 0);

    float* ptr;
    VK(vkMapMemory(dev, readMem, 0, readSize, 0, (void**)&ptr));
    for (int i = 0; i < W * H; ++i)
      for (int c = 0; c < 3; ++c) {
        float v = ptr[(size_t)i * 4 + c];
        v = v < 0.f ? 0.f : (v > 1.f ? 1.f : v);
        rgb[(size_t)i * 3 + c] = (uint8_t)(v * 255.f + 0.5f);
      }
    vkUnmapMemory(dev, readMem);

    if (!ffpipe) {
      ffpipe = popen(ff_cmd, "w");
      if (!ffpipe) { logError("Cannot open ffmpeg pipe"); return 1; }
    }
    std::fwrite(rgb.data(), 1, rgb.size(), ffpipe);
    std::cout << "[" << animName << "] frame " << frame << " (t=" << pc.uTime
              << ")\n";
  }
  if (ffpipe) pclose(ffpipe);

  double avg = total_ms / NFRAMES;
  std::cout << "[" << animName << "] Vulkan avg: " << avg << " ms/frame  ("
            << (1000.0 / avg) << " fps)\n";
  std::cout << "[" << animName << "] MP4: result/" << animName << ".mp4\n";

  vkDestroyFence(dev, fence, nullptr);
  vkDestroyPipeline(dev, pipeline, nullptr);
  vkDestroyShaderModule(dev, compMod, nullptr);
  vkDestroyDescriptorPool(dev, dpool, nullptr);
  vkDestroyPipelineLayout(dev, pipeLayout, nullptr);
  vkDestroyDescriptorSetLayout(dev, dsl, nullptr);
  vkDestroyBuffer(dev, readBuf, nullptr);
  vkFreeMemory(dev, readMem, nullptr);
  vkDestroyImageView(dev, outView, nullptr);
  vkDestroyImage(dev, outImg, nullptr);
  vkFreeMemory(dev, outMem, nullptr);
  vkDestroyImageView(dev, volView, nullptr);
  vkDestroySampler(dev, sampler, nullptr);
  vkDestroyImage(dev, volImg, nullptr);
  vkFreeMemory(dev, volMem, nullptr);
  vkDestroyBuffer(dev, stageBuf, nullptr);
  vkFreeMemory(dev, stageMem, nullptr);
  vkDestroyCommandPool(dev, cmdPool, nullptr);
  vkDestroyDevice(dev, nullptr);
  vkDestroyInstance(instance, nullptr);
  return 0;
}
