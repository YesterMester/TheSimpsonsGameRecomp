#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <vector>

// Compare the original translated search shader and two native versions by
// rendering float pixels. The translated program is the independent reference.
static void Check(VkResult r) {
  if (r != VK_SUCCESS)
    throw std::runtime_error("Vulkan call failed: " + std::to_string(r));
}
struct Buffer {
  VkBuffer buffer;
  VkDeviceMemory memory;
  void* mapped;
};
struct Image {
  VkImage image;
  VkDeviceMemory memory;
  VkImageView view;
};
struct Fixture {
  VkInstance instance;
  VkPhysicalDevice physical;
  VkDevice device;
  VkQueue queue;
  uint32_t family;
  VkPhysicalDeviceMemoryProperties memory;
  VkCommandPool pool;
  VkCommandBuffer commands;
  std::vector<Buffer> buffers;
  std::vector<Image> images;
  std::vector<VkShaderModule> shaders;
  Fixture() {
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo create{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    create.pApplicationInfo = &app;
    Check(vkCreateInstance(&create, nullptr, &instance));
    uint32_t n = 0;
    Check(vkEnumeratePhysicalDevices(instance, &n, nullptr));
    std::vector<VkPhysicalDevice> devices(n);
    Check(vkEnumeratePhysicalDevices(instance, &n, devices.data()));
    if (!n) throw std::runtime_error("no Vulkan device");
    physical = devices[0];
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(physical, &properties);
    std::printf("Device: %s\n", properties.deviceName);
    vkGetPhysicalDeviceMemoryProperties(physical, &memory);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &n, nullptr);
    std::vector<VkQueueFamilyProperties> queues(n);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &n, queues.data());
    family = 0;
    while (family < queues.size() &&
           !(queues[family].queueFlags & VK_QUEUE_GRAPHICS_BIT))
      ++family;
    if (family == queues.size()) throw std::runtime_error("no graphics queue");
    float priority = 1;
    VkDeviceQueueCreateInfo qc{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qc.queueFamilyIndex = family;
    qc.queueCount = 1;
    qc.pQueuePriorities = &priority;
    VkPhysicalDeviceVulkan13Features f13{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features f12{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    f12.pNext = &f13;
    VkPhysicalDeviceFeatures2 features{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    features.pNext = &f12;
    vkGetPhysicalDeviceFeatures2(physical, &features);
    VkDeviceCreateInfo dc{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dc.pNext = &features;
    dc.queueCreateInfoCount = 1;
    dc.pQueueCreateInfos = &qc;
    Check(vkCreateDevice(physical, &dc, nullptr, &device));
    vkGetDeviceQueue(device, family, 0, &queue);
    VkCommandPoolCreateInfo pc{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pc.queueFamilyIndex = family;
    pc.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    Check(vkCreateCommandPool(device, &pc, nullptr, &pool));
    VkCommandBufferAllocateInfo ca{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ca.commandPool = pool;
    ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ca.commandBufferCount = 1;
    Check(vkAllocateCommandBuffers(device, &ca, &commands));
  }
  ~Fixture() {
    vkDeviceWaitIdle(device);
    for (auto b : buffers) {
      vkUnmapMemory(device, b.memory);
      vkDestroyBuffer(device, b.buffer, nullptr);
      vkFreeMemory(device, b.memory, nullptr);
    }
    for (auto i : images) {
      vkDestroyImageView(device, i.view, nullptr);
      vkDestroyImage(device, i.image, nullptr);
      vkFreeMemory(device, i.memory, nullptr);
    }
    for (auto s : shaders) vkDestroyShaderModule(device, s, nullptr);
    vkDestroyCommandPool(device, pool, nullptr);
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);
  }
  uint32_t MemoryType(uint32_t mask, VkMemoryPropertyFlags flags) {
    for (uint32_t i = 0; i < memory.memoryTypeCount; ++i)
      if ((mask & (1u << i)) &&
          (memory.memoryTypes[i].propertyFlags & flags) == flags)
        return i;
    throw std::runtime_error("no compatible memory");
  }
  Buffer MakeBuffer(VkDeviceSize size, VkBufferUsageFlags usage) {
    Buffer b{};
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = size;
    info.usage = usage;
    Check(vkCreateBuffer(device, &info, nullptr, &b.buffer));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device, b.buffer, &req);
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = MemoryType(
        req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    Check(vkAllocateMemory(device, &alloc, nullptr, &b.memory));
    Check(vkBindBufferMemory(device, b.buffer, b.memory, 0));
    Check(vkMapMemory(device, b.memory, 0, VK_WHOLE_SIZE, 0, &b.mapped));
    std::memset(b.mapped, 0, size);
    buffers.push_back(b);
    return b;
  }
  Image MakeImage(uint32_t width, uint32_t height, VkImageUsageFlags usage,
                  VkImageViewType view_type) {
    Image image{};
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = VK_FORMAT_R32G32B32A32_SFLOAT;
    info.extent = {width, height, 1};
    info.mipLevels = info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    Check(vkCreateImage(device, &info, nullptr, &image.image));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device, image.image, &req);
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex =
        MemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    Check(vkAllocateMemory(device, &alloc, nullptr, &image.memory));
    Check(vkBindImageMemory(device, image.image, image.memory, 0));
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = image.image;
    view.viewType = view_type;
    view.format = info.format;
    view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    Check(vkCreateImageView(device, &view, nullptr, &image.view));
    images.push_back(image);
    return image;
  }
  VkShaderModule Shader(const char* path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) throw std::runtime_error(path);
    auto size = f.tellg();
    if (size <= 0 || size % 4)
      throw std::runtime_error("invalid SPIR-V file size");
    std::vector<uint32_t> words(size_t(size) / 4);
    f.seekg(0);
    f.read(reinterpret_cast<char*>(words.data()), words.size() * 4);
    if (!f) throw std::runtime_error("failed to read SPIR-V file");
    VkShaderModuleCreateInfo create{
        VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    create.codeSize = words.size() * 4;
    create.pCode = words.data();
    VkShaderModule shader;
    Check(vkCreateShaderModule(device, &create, nullptr, &shader));
    shaders.push_back(shader);
    return shader;
  }
  void Begin() {
    Check(vkResetCommandBuffer(commands, 0));
    VkCommandBufferBeginInfo info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    Check(vkBeginCommandBuffer(commands, &info));
  }
  void End() {
    Check(vkEndCommandBuffer(commands));
    VkSubmitInfo info{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    info.commandBufferCount = 1;
    info.pCommandBuffers = &commands;
    Check(vkQueueSubmit(queue, 1, &info, VK_NULL_HANDLE));
    Check(vkQueueWaitIdle(queue));
  }
};

int main(int argc, char** argv) {
  try {
    if (argc != 5)
      throw std::runtime_error(
          "usage: shader_search_check vertex.spv translated.spv old.spv "
          "fixed.spv");
    Fixture f;
    auto system = f.MakeBuffer(1024, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    auto constants = f.MakeBuffer(4096, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    auto loops = f.MakeBuffer(160, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    auto fetch = f.MakeBuffer(768, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    auto upload = f.MakeBuffer(64 * 64 * 16, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    auto readback = f.MakeBuffer(16, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    auto texture = f.MakeImage(
        64, 64, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        VK_IMAGE_VIEW_TYPE_2D_ARRAY);
    auto output = f.MakeImage(
        1, 1,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        VK_IMAGE_VIEW_TYPE_2D);
    uint32_t random = 0x931CB24u;
    auto rng = [&] {
      random ^= random << 13;
      random ^= random >> 17;
      random ^= random << 5;
      return random;
    };
    auto* texels = static_cast<float*>(upload.mapped);
    for (size_t i = 0; i < 64 * 64 * 4; ++i)
      texels[i] = float(rng() & 7) * 0.125f;
    static_cast<float*>(system.mapped)[288 / 4] = 1;
    static_cast<uint32_t*>(fetch.mapped)[2] = 63 | (63 << 13);
    VkSamplerCreateInfo sc{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sc.magFilter = sc.minFilter = VK_FILTER_NEAREST;
    sc.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sc.addressModeU = sc.addressModeV = sc.addressModeW =
        VK_SAMPLER_ADDRESS_MODE_REPEAT;
    VkSampler sampler;
    Check(vkCreateSampler(f.device, &sc, nullptr, &sampler));
    std::array<VkDescriptorSetLayout, 4> layouts;
    for (unsigned i = 0; i < 4; ++i) {
      std::vector<VkDescriptorSetLayoutBinding> bindings;
      if (i == 1)
        for (uint32_t binding : {0u, 2u, 3u, 4u})
          bindings.push_back({binding, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                              VK_SHADER_STAGE_FRAGMENT_BIT, nullptr});
      if (i == 3) {
        bindings.push_back({0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1,
                            VK_SHADER_STAGE_FRAGMENT_BIT, nullptr});
        bindings.push_back({1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1,
                            VK_SHADER_STAGE_FRAGMENT_BIT, nullptr});
        bindings.push_back({2, VK_DESCRIPTOR_TYPE_SAMPLER, 1,
                            VK_SHADER_STAGE_FRAGMENT_BIT, nullptr});
      }
      VkDescriptorSetLayoutCreateInfo create{
          VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
      create.bindingCount = bindings.size();
      create.pBindings = bindings.data();
      Check(
          vkCreateDescriptorSetLayout(f.device, &create, nullptr, &layouts[i]));
    }
    VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 4},
                                    {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 2},
                                    {VK_DESCRIPTOR_TYPE_SAMPLER, 1}};
    VkDescriptorPoolCreateInfo dp{
        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dp.maxSets = 4;
    dp.poolSizeCount = 3;
    dp.pPoolSizes = sizes;
    VkDescriptorPool descriptors;
    Check(vkCreateDescriptorPool(f.device, &dp, nullptr, &descriptors));
    VkDescriptorSet sets[4];
    VkDescriptorSetAllocateInfo ds{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ds.descriptorPool = descriptors;
    ds.descriptorSetCount = 4;
    ds.pSetLayouts = layouts.data();
    Check(vkAllocateDescriptorSets(f.device, &ds, sets));
    Buffer uniforms[] = {system, constants, loops, fetch};
    uint32_t indices[] = {0, 2, 3, 4};
    for (unsigned i = 0; i < 4; ++i) {
      VkDescriptorBufferInfo bi{uniforms[i].buffer, 0, VK_WHOLE_SIZE};
      VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      write.dstSet = sets[1];
      write.dstBinding = indices[i];
      write.descriptorCount = 1;
      write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
      write.pBufferInfo = &bi;
      vkUpdateDescriptorSets(f.device, 1, &write, 0, nullptr);
    }
    for (unsigned i = 0; i < 3; ++i) {
      VkDescriptorImageInfo info{sampler, texture.view,
                                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
      VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      write.dstSet = sets[3];
      write.dstBinding = i;
      write.descriptorCount = 1;
      write.descriptorType = i == 2 ? VK_DESCRIPTOR_TYPE_SAMPLER
                                    : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
      write.pImageInfo = &info;
      vkUpdateDescriptorSets(f.device, 1, &write, 0, nullptr);
    }
    VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT, 0, 16};
    VkPipelineLayoutCreateInfo pl{
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 4;
    pl.pSetLayouts = layouts.data();
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &push;
    VkPipelineLayout layout;
    Check(vkCreatePipelineLayout(f.device, &pl, nullptr, &layout));
    VkAttachmentDescription attachment{};
    attachment.format = VK_FORMAT_R32G32B32A32_SFLOAT;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachment.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    VkAttachmentReference reference{0,
                                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &reference;
    VkSubpassDependency dependency{
        0,
        VK_SUBPASS_EXTERNAL,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        VK_ACCESS_TRANSFER_READ_BIT,
        0};
    VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rp.attachmentCount = 1;
    rp.pAttachments = &attachment;
    rp.subpassCount = 1;
    rp.pSubpasses = &subpass;
    rp.dependencyCount = 1;
    rp.pDependencies = &dependency;
    VkRenderPass pass;
    Check(vkCreateRenderPass(f.device, &rp, nullptr, &pass));
    VkFramebufferCreateInfo fc{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fc.renderPass = pass;
    fc.attachmentCount = 1;
    fc.pAttachments = &output.view;
    fc.width = fc.height = fc.layers = 1;
    VkFramebuffer framebuffer;
    Check(vkCreateFramebuffer(f.device, &fc, nullptr, &framebuffer));
    VkShaderModule vertex = f.Shader(argv[1]);
    VkPipeline pipelines[3];
    for (unsigned i = 0; i < 3; ++i) {
      VkPipelineShaderStageCreateInfo stages[2] = {
          {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
          {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
      stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
      stages[0].module = vertex;
      stages[0].pName = "main";
      stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
      stages[1].module = f.Shader(argv[i + 2]);
      stages[1].pName = "main";
      VkPipelineVertexInputStateCreateInfo vi{
          VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
      VkPipelineInputAssemblyStateCreateInfo ia{
          VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
      ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
      VkViewport viewport{0, 0, 1, 1, 0, 1};
      VkRect2D scissor{{0, 0}, {1, 1}};
      VkPipelineViewportStateCreateInfo vs{
          VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
      vs.viewportCount = vs.scissorCount = 1;
      vs.pViewports = &viewport;
      vs.pScissors = &scissor;
      VkPipelineRasterizationStateCreateInfo rs{
          VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
      rs.polygonMode = VK_POLYGON_MODE_FILL;
      rs.lineWidth = 1;
      VkPipelineMultisampleStateCreateInfo ms{
          VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
      ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
      VkPipelineColorBlendAttachmentState ba{};
      ba.colorWriteMask = 15;
      VkPipelineColorBlendStateCreateInfo bs{
          VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
      bs.attachmentCount = 1;
      bs.pAttachments = &ba;
      VkGraphicsPipelineCreateInfo create{
          VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
      create.stageCount = 2;
      create.pStages = stages;
      create.pVertexInputState = &vi;
      create.pInputAssemblyState = &ia;
      create.pViewportState = &vs;
      create.pRasterizationState = &rs;
      create.pMultisampleState = &ms;
      create.pColorBlendState = &bs;
      create.layout = layout;
      create.renderPass = pass;
      Check(vkCreateGraphicsPipelines(f.device, VK_NULL_HANDLE, 1, &create,
                                      nullptr, &pipelines[i]));
    }
    f.Begin();
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex =
        VK_QUEUE_FAMILY_IGNORED;
    barrier.image = texture.image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(f.commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &barrier);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {64, 64, 1};
    vkCmdCopyBufferToImage(f.commands, upload.buffer, texture.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier(f.commands, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &barrier);
    f.End();
    unsigned cases = 0, old_bad = 0, fixed_bad = 0;
    for (uint32_t count : {0u, 1u, 2u, 3u, 4u, 5u, 8u, 9u, 10u, 12u})
      for (int step : {-2, -1, 0, 1, 2})
        for (unsigned seed = 0; seed < 20; ++seed) {
          auto* c = static_cast<float*>(constants.mapped);
          for (unsigned i = 0; i < 256; ++i) {
            c[i * 4] = float(int(rng() & 31) - 16);
            c[i * 4 + 1] = float(int(rng() & 31) - 16);
            c[i * 4 + 2] = c[i * 4 + 3] = 0;
          }
          c[48 * 4] = c[49 * 4] = 64;
          c[50 * 4] = 1;
          c[254 * 4] = float(seed & 1);
          c[254 * 4 + 1] = float(seed % 4) * .125f;
          c[254 * 4 + 2] = 1;
          c[255 * 4] = .5f;
          c[255 * 4 + 1] = .5f;
          c[255 * 4 + 2] = 1;
          c[255 * 4 + 3] = float(1 + seed % 6) * .125f;
          uint32_t base = step < 0 ? 24u : 0u;
          static_cast<uint32_t*>(loops.mapped)[39] =
              count | (base << 8) | ((uint32_t(step) & 255) << 16);
          std::array<float, 4> input = {(float(rng() & 63) + .5f) / 64,
                                        (float(rng() & 63) + .5f) / 64, 0, 0};
          std::array<std::array<uint32_t, 4>, 3> results;
          for (unsigned i = 0; i < 3; ++i) {
            f.Begin();
            VkRenderPassBeginInfo begin{
                VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
            begin.renderPass = pass;
            begin.framebuffer = framebuffer;
            begin.renderArea = {{0, 0}, {1, 1}};
            vkCmdBeginRenderPass(f.commands, &begin,
                                 VK_SUBPASS_CONTENTS_INLINE);
            vkCmdBindPipeline(f.commands, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              pipelines[i]);
            vkCmdBindDescriptorSets(f.commands, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    layout, 0, 4, sets, 0, nullptr);
            vkCmdPushConstants(f.commands, layout, VK_SHADER_STAGE_VERTEX_BIT,
                               0, 16, input.data());
            vkCmdDraw(f.commands, 3, 1, 0, 0);
            vkCmdEndRenderPass(f.commands);
            copy.imageExtent = {1, 1, 1};
            vkCmdCopyImageToBuffer(f.commands, output.image,
                                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   readback.buffer, 1, &copy);
            VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            vkCmdPipelineBarrier(f.commands, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0,
                                 nullptr, 0, nullptr);
            f.End();
            std::memcpy(results[i].data(), readback.mapped, 16);
          }
          ++cases;
          old_bad += results[0] != results[1];
          fixed_bad += results[0] != results[2];
          if (results[0] != results[2] && fixed_bad <= 5)
            std::printf(
                "Mismatch: count=%u step=%d seed=%u translated=%08X old=%08X "
                "fixed=%08X\n",
                count, step, seed, results[0][0], results[1][0], results[2][0]);
        }
    std::printf(
        "SEARCH shader cases=%u old_mismatches=%u fixed_mismatches=%u\n", cases,
        old_bad, fixed_bad);
    vkDeviceWaitIdle(f.device);
    for (auto pipeline : pipelines)
      vkDestroyPipeline(f.device, pipeline, nullptr);
    vkDestroyFramebuffer(f.device, framebuffer, nullptr);
    vkDestroyRenderPass(f.device, pass, nullptr);
    vkDestroyPipelineLayout(f.device, layout, nullptr);
    vkDestroyDescriptorPool(f.device, descriptors, nullptr);
    for (auto l : layouts) vkDestroyDescriptorSetLayout(f.device, l, nullptr);
    vkDestroySampler(f.device, sampler, nullptr);
    return fixed_bad ? 1 : 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "%s\n", e.what());
    return 1;
  }
}
