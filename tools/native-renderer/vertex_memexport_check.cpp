// Run hand-assembled Xenos vertex exports through the production translator
// and a device with vertex storage writes disabled. Check every output byte
// against a CPU description of the indices and export format.
#include <rex/platform.h>

// Declare the loader entry points before the runtime's VK_NO_PROTOTYPES
// headers, with the same platform and beta declarations as those headers.
#define VK_ENABLE_BETA_EXTENSIONS
#if REX_PLATFORM_GNU_LINUX
#define VK_USE_PLATFORM_XCB_KHR
#endif
#include <vulkan/vulkan.h>

#include <rex/cvar.h>
#include <rex/graphics/pipeline/shader/spirv.h>
#include <rex/graphics/pipeline/shader/spirv_translator.h>
#include <rex/graphics/vulkan/graphics_system.h>
#include <rex/memory.h>
#include <rex/runtime.h>
#include <rex/string/buffer.h>
#include <rex/ui/presenter.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using rex::graphics::Shader;
using rex::graphics::SpirvShader;
using rex::graphics::SpirvShaderTranslator;
namespace xenos = rex::graphics::xenos;
namespace ucode = rex::graphics::ucode;

void Require(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}
void Check(VkResult result) {
  if (result != VK_SUCCESS) {
    throw std::runtime_error("Vulkan error " + std::to_string(result));
  }
}

std::vector<uint32_t> Microcode() {
  // exece addr(1), count(2), ALU/ALU; nop
  std::vector<uint32_t> words = {1u | (2u << 12),
                                 uint32_t(ucode::ControlFlowOpcode::kExecEnd) << 12, 0};
  auto mad_export = [&](ucode::ExportRegister target, uint32_t scale, uint32_t offset) {
    words.push_back(uint32_t(target) | (1u << 15) | (15u << 16) |
                    (uint32_t(ucode::AluScalarOpcode::kRetainPrev) << 26));
    // r0.xxxx: component-relative swizzle [0, 3, 2, 1].
    words.push_back(0x6Cu << 16);
    words.push_back(offset | (scale << 8) | (uint32_t(ucode::AluVectorOpcode::kMad) << 24) |
                    (1u << 31));
  };
  // eA = index * {0, 1, 0, 0} + stream; eM0 = index * scale + offset.
  mad_export(ucode::ExportRegister::kExportAddress, 0, 1);
  mad_export(ucode::ExportRegister::kExportData0, 2, 3);
  return words;
}

std::vector<uint8_t> Translate(const SpirvShaderTranslator::Features& features,
                               Shader::HostVertexShaderType type) {
  auto words = Microcode();
  SpirvShader shader(xenos::ShaderType::kVertex, 0, words.data(), words.size(),
                     std::endian::native);
  rex::string::StringBuffer disassembly;
  shader.AnalyzeUcode(disassembly);
  Require(shader.memexport_eM_written() == 1 && shader.memexport_stream_constants().contains(1),
          "Hand-assembled shader was not recognized as a memory export");
  SpirvShaderTranslator translator(features, false, false, false);
  auto modification =
      SpirvShaderTranslator::Modification(translator.GetDefaultVertexShaderModification(1, type));
  modification.vertex.interpolator_mask = 0;
  auto translation = shader.GetOrCreateTranslation(modification.value);
  if (!translator.TranslateAnalyzedShader(*translation)) {
    std::fputs(shader.ucode_disassembly().c_str(), stderr);
    for (const auto& error : translation->errors())
      std::fprintf(stderr, "%s\n", error.message.c_str());
    throw std::runtime_error("Vertex export translation failed");
  }
  return translation->translated_binary();
}

struct Buffer {
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  void* mapped = nullptr;
};

struct Fixture {
  VkInstance instance = VK_NULL_HANDLE;
  VkPhysicalDevice physical = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;
  VkQueue queue = VK_NULL_HANDLE;
  VkCommandPool pool = VK_NULL_HANDLE;
  VkCommandBuffer commands = VK_NULL_HANDLE;
  VkDescriptorPool descriptors = VK_NULL_HANDLE;
  std::array<VkDescriptorSetLayout, 2> sets = {};
  VkPipelineLayout layout = VK_NULL_HANDLE;
  VkShaderModule shader = VK_NULL_HANDLE;
  VkPipeline pipeline = VK_NULL_HANDLE;
  VkPhysicalDeviceProperties properties = {};
  VkPhysicalDeviceMemoryProperties memory = {};
  std::vector<Buffer> buffers;

  Fixture() {
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo create{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    create.pApplicationInfo = &app;
    Check(vkCreateInstance(&create, nullptr, &instance));
    uint32_t count = 0;
    Check(vkEnumeratePhysicalDevices(instance, &count, nullptr));
    Require(count != 0, "No Vulkan device");
    std::vector<VkPhysicalDevice> devices(count);
    Check(vkEnumeratePhysicalDevices(instance, &count, devices.data()));
    physical = devices.front();
    vkGetPhysicalDeviceProperties(physical, &properties);
    vkGetPhysicalDeviceMemoryProperties(physical, &memory);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
    std::vector<VkQueueFamilyProperties> queues(count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, queues.data());
    uint32_t family = 0;
    while (family < count && !(queues[family].queueFlags & VK_QUEUE_COMPUTE_BIT))
      ++family;
    Require(family < count, "No compute queue");
    float priority = 1;
    VkDeviceQueueCreateInfo qc{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qc.queueFamilyIndex = family;
    qc.queueCount = 1;
    qc.pQueuePriorities = &priority;
    // No optional core feature is enabled, including vertexPipelineStoresAndAtomics,
    // shaderClipDistance and shaderCullDistance.
    VkDeviceCreateInfo dc{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dc.queueCreateInfoCount = 1;
    dc.pQueueCreateInfos = &qc;
    Check(vkCreateDevice(physical, &dc, nullptr, &device));
    vkGetDeviceQueue(device, family, 0, &queue);
    VkCommandPoolCreateInfo pc{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pc.queueFamilyIndex = family;
    pc.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    Check(vkCreateCommandPool(device, &pc, nullptr, &pool));
    VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ca.commandPool = pool;
    ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ca.commandBufferCount = 1;
    Check(vkAllocateCommandBuffers(device, &ca, &commands));
  }
  ~Fixture() {
    if (device) {
      vkDeviceWaitIdle(device);
      if (pipeline)
        vkDestroyPipeline(device, pipeline, nullptr);
      if (shader)
        vkDestroyShaderModule(device, shader, nullptr);
      if (layout)
        vkDestroyPipelineLayout(device, layout, nullptr);
      if (descriptors)
        vkDestroyDescriptorPool(device, descriptors, nullptr);
      for (auto set : sets)
        if (set)
          vkDestroyDescriptorSetLayout(device, set, nullptr);
      for (auto b : buffers) {
        if (b.mapped)
          vkUnmapMemory(device, b.memory);
        vkDestroyBuffer(device, b.buffer, nullptr);
        vkFreeMemory(device, b.memory, nullptr);
      }
      if (pool)
        vkDestroyCommandPool(device, pool, nullptr);
      vkDestroyDevice(device, nullptr);
    }
    if (instance)
      vkDestroyInstance(instance, nullptr);
  }
  Buffer MakeBuffer(VkDeviceSize size, VkBufferUsageFlags usage) {
    Buffer b;
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = size;
    info.usage = usage;
    Check(vkCreateBuffer(device, &info, nullptr, &b.buffer));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device, b.buffer, &req);
    uint32_t type = 0;
    constexpr auto flags =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    while (type < memory.memoryTypeCount &&
           (!(req.memoryTypeBits & (1u << type)) ||
            (memory.memoryTypes[type].propertyFlags & flags) != flags))
      ++type;
    Require(type < memory.memoryTypeCount, "No host-coherent memory");
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = type;
    Check(vkAllocateMemory(device, &alloc, nullptr, &b.memory));
    Check(vkBindBufferMemory(device, b.buffer, b.memory, 0));
    Check(vkMapMemory(device, b.memory, 0, VK_WHOLE_SIZE, 0, &b.mapped));
    buffers.push_back(b);
    return b;
  }

  std::array<VkDescriptorSet, 2> Prepare(const std::vector<uint8_t>& code, Buffer shared,
                                         Buffer system, Buffer constants, uint32_t shared_count) {
    VkDescriptorSetLayoutBinding storage{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, shared_count,
                                         VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo sc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    sc.bindingCount = 1;
    sc.pBindings = &storage;
    Check(vkCreateDescriptorSetLayout(device, &sc, nullptr, &sets[0]));
    std::array<VkDescriptorSetLayoutBinding, 2> uniforms = {
        {{0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
         {1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}}};
    sc.bindingCount = uint32_t(uniforms.size());
    sc.pBindings = uniforms.data();
    Check(vkCreateDescriptorSetLayout(device, &sc, nullptr, &sets[1]));
    VkPipelineLayoutCreateInfo lc{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    lc.setLayoutCount = uint32_t(sets.size());
    lc.pSetLayouts = sets.data();
    Check(vkCreatePipelineLayout(device, &lc, nullptr, &layout));
    VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    sm.codeSize = code.size();
    sm.pCode = reinterpret_cast<const uint32_t*>(code.data());
    Check(vkCreateShaderModule(device, &sm, nullptr, &shader));
    VkComputePipelineCreateInfo cp{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cp.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cp.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cp.stage.module = shader;
    cp.stage.pName = "main";
    cp.layout = layout;
    Check(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cp, nullptr, &pipeline));
    std::array<VkDescriptorPoolSize, 2> sizes = {{{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, shared_count},
                                                  {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2}}};
    VkDescriptorPoolCreateInfo pc{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pc.maxSets = 2;
    pc.poolSizeCount = uint32_t(sizes.size());
    pc.pPoolSizes = sizes.data();
    Check(vkCreateDescriptorPool(device, &pc, nullptr, &descriptors));
    std::array<VkDescriptorSet, 2> bound;
    VkDescriptorSetAllocateInfo da{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    da.descriptorPool = descriptors;
    da.descriptorSetCount = uint32_t(sets.size());
    da.pSetLayouts = sets.data();
    Check(vkAllocateDescriptorSets(device, &da, bound.data()));
    std::vector<VkDescriptorBufferInfo> shared_infos(shared_count,
                                                     {shared.buffer, 0, 2 * 1024 * 1024});
    std::array<VkDescriptorBufferInfo, 2> uniform_infos = {
        {{system.buffer, 0, sizeof(SpirvShaderTranslator::SystemConstants)},
         {constants.buffer, 0, 64}}};
    std::array<VkWriteDescriptorSet, 3> writes = {};
    for (uint32_t i = 0; i < writes.size(); ++i) {
      auto& write = writes[i];
      write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      write.dstSet = bound[i ? 1 : 0];
      write.dstBinding = i ? i - 1 : 0;
      write.descriptorCount = i ? 1 : shared_count;
      write.descriptorType =
          i ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      write.pBufferInfo = i ? &uniform_infos[i - 1] : shared_infos.data();
    }
    vkUpdateDescriptorSets(device, uint32_t(writes.size()), writes.data(), 0, nullptr);
    return bound;
  }
  void Dispatch(const std::array<VkDescriptorSet, 2>& bound, uint32_t count) {
    Check(vkResetCommandBuffer(commands, 0));
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    Check(vkBeginCommandBuffer(commands, &begin));
    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0,
                            uint32_t(bound.size()), bound.data(), 0, nullptr);
    const uint32_t width = SpirvShaderTranslator::kMemExportComputeDispatchX;
    vkCmdDispatch(commands, std::max(1u, std::min(count, width)),
                  std::max(1u, uint32_t((uint64_t(count) + width - 1) / width)), 1);
    VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    host.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                         0, 1, &host, 0, nullptr, 0, nullptr);
    Check(vkEndCommandBuffer(commands));
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &commands;
    Check(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));
    Check(vkQueueWaitIdle(queue));
  }
};

uint32_t Swap(uint32_t value, uint32_t endian) {
  switch (endian) {
    case 1:
      return ((value & 0x00FF00FF) << 8) | ((value & 0xFF00FF00) >> 8);
    case 2:
      return std::byteswap(value);
    case 3:
      return (value << 16) | (value >> 16);
    default:
      return value;
  }
}

struct Case {
  uint32_t count, index_bits, endian, export_endian;
  int32_t base;
  bool reset;
  bool swap_red_blue;
};

template <typename Dispatch>
uint64_t Run(Buffer shared, Buffer system, Buffer constants, const Case& test, Dispatch dispatch) {
  constexpr uint32_t kSize = 2 * 1024 * 1024, kOutput = 256 * 1024, kIndices = 4098;
  std::vector<uint8_t> expected(kSize, 0xA7);
  auto* sc = static_cast<SpirvShaderTranslator::SystemConstants*>(system.mapped);
  *sc = {};
  sc->compute_memexport_vertex_count = test.count;
  sc->vertex_index_load_address = test.index_bits == 16 ? kIndices : kIndices + 2;
  sc->vertex_index_endian = xenos::Endian(test.endian);
  sc->vertex_base_index = test.base;
  sc->vertex_index_min = test.count > 50 ? 0 : 2;
  sc->vertex_index_max = test.count > 50 ? 70000 : 50;
  sc->vertex_index_reset = 31;
  if (test.index_bits)
    sc->flags |= SpirvShaderTranslator::kSysFlag_ComputeOrPrimitiveVertexIndexLoad;
  if (test.index_bits == 32)
    sc->flags |= SpirvShaderTranslator::kSysFlag_ComputeOrPrimitiveVertexIndexLoad32Bit;
  if (test.reset)
    sc->flags |= SpirvShaderTranslator::kSysFlag_ComputeOrPrimitiveVertexIndexReset;
  auto* fc = static_cast<float*>(constants.mapped);
  std::array<float, 16> values = {0, 1, 0, 0, 0, 0, 0, 0, 1, 2, 4, .5f, .25f, -.5f, 3, -2};
  std::copy(values.begin(), values.end(), fc);
  auto* stream = reinterpret_cast<uint32_t*>(fc + 4);
  stream[0] = 0x40000000 | (kOutput >> 2);
  stream[1] = 0x4B000000;
  stream[2] = 0x4B000000 | (uint32_t(xenos::ColorFormat::k_32_32_32_32_FLOAT) << 8) |
              test.export_endian | (uint32_t(test.swap_red_blue) << 19);
  stream[3] = 0x4B000000 | 70001;
  const std::array<uint32_t, 9> indices = {7, 1, 11, 4, 31, 65, 8, 0, 19};
  for (uint32_t i = 0; test.index_bits && i < test.count; ++i) {
    uint32_t index = indices.at(i);
    if (test.index_bits == 32 && i == 2)
      index |= 0xDE000000;
    if (test.index_bits == 32 && test.reset && index == 31)
      index |= 0xAB000000;
    uint32_t encoded = Swap(index, test.endian);
    std::memcpy(expected.data() + sc->vertex_index_load_address + i * (test.index_bits / 8),
                &encoded, test.index_bits / 8);
  }
  std::memcpy(shared.mapped, expected.data(), expected.size());
  for (uint32_t i = 0; i < test.count; ++i) {
    uint32_t index = test.index_bits ? indices.at(i) : i;
    if (test.reset && index == 31)
      continue;
    index = std::clamp((index + uint32_t(test.base)) & 0xFFFFFF, sc->vertex_index_min,
                       sc->vertex_index_max);
    std::array<float, 4> data = {float(index) + .25f, float(index) * 2 - .5f, float(index) * 4 + 3,
                                 float(index) * .5f - 2};
    if (test.swap_red_blue)
      std::swap(data[0], data[2]);
    std::array<uint32_t, 4> words = std::bit_cast<std::array<uint32_t, 4>>(data);
    if (test.export_endian <= 3) {
      for (auto& word : words)
        word = Swap(word, test.export_endian);
    } else {
      if (test.export_endian == 4) {
        std::swap(words[0], words[1]);
        std::swap(words[2], words[3]);
      } else {
        std::reverse(words.begin(), words.end());
      }
      for (auto& word : words)
        word = std::byteswap(word);
    }
    std::memcpy(expected.data() + kOutput + index * 16, words.data(), sizeof(words));
  }
  dispatch(test.count);
  const auto* actual = static_cast<const uint8_t*>(shared.mapped);
  for (size_t i = 0; i < expected.size(); ++i) {
    if (actual[i] != expected[i]) {
      std::fprintf(stderr,
                   "Mismatch at %zu: %02X != %02X (count=%u, index=%u, endian=%u, "
                   "export=%u, base=%d, reset=%d, swap=%d)\n",
                   i, actual[i], expected[i], test.count, test.index_bits, test.endian,
                   test.export_endian, test.base, test.reset, test.swap_red_blue);
      throw std::runtime_error("Compute vertex memory export differs from the CPU reference");
    }
  }
  return expected.size();
}

void RunCommandProcessor(const std::filesystem::path& output) {
  for (auto setting :
       {std::pair{"vulkan_vertex_memexport_compute", "true"},
        std::pair{"vulkan_readback_memexport", "true"},
        std::pair{"readback_memexport_fast", "false"},
        std::pair{"async_shader_compilation", "false"},
        std::pair{"render_target_path_vulkan", "host"}, std::pair{"aot_shader_path", ""}}) {
    Require(rex::cvar::SetFlagByName(setting.first, setting.second), "Could not set fixture cvar");
  }
  rex::Runtime runtime("", output / "userdata");
  auto graphics = std::make_unique<rex::graphics::vulkan::VulkanGraphicsSystem>();
  auto* gpu = graphics.get();
  Require(graphics->SetupPresentation(nullptr) == 0, "Could not create offscreen presentation");
  rex::RuntimeConfig config;
  config.graphics = std::move(graphics);
  Require(runtime.Setup(std::move(config)) == 0, "Could not initialize the command processor");
  auto* cp = gpu->command_processor();
  std::promise<void> finished;
  auto completion = finished.get_future();
  uint64_t bytes = 0;
  uint32_t cases = 0;
  cp->CallInThread([&] {
    try {
      constexpr uint32_t kPacket = 2 * 1024 * 1024;
      auto* packet = runtime.memory()->TranslatePhysical<uint32_t*>(kPacket);
      auto execute = [&](xenos::Type3Opcode opcode, const std::vector<uint32_t>& payload) {
        packet[0] = std::byteswap(xenos::MakePacketType3(opcode, uint16_t(payload.size())));
        for (size_t i = 0; i < payload.size(); ++i)
          packet[i + 1] = std::byteswap(payload[i]);
        cp->ExecutePacket(kPacket, uint32_t(payload.size() + 1));
      };
      auto code = Microcode();
      std::vector<uint32_t> load = {uint32_t(xenos::ShaderType::kVertex), uint32_t(code.size())};
      load.insert(load.end(), code.begin(), code.end());
      execute(xenos::PM4_IM_LOAD_IMMEDIATE, load);
      SpirvShaderTranslator::SystemConstants sc;
      std::array<float, 16> fc;
      Buffer shared{VK_NULL_HANDLE, VK_NULL_HANDLE, runtime.memory()->TranslatePhysical(0)};
      Buffer system{VK_NULL_HANDLE, VK_NULL_HANDLE, &sc};
      Buffer constants{VK_NULL_HANDLE, VK_NULL_HANDLE, fc.data()};
      namespace reg = rex::graphics::reg;
      using namespace rex::graphics;
      for (auto primitive :
           {xenos::PrimitiveType::kPointList, xenos::PrimitiveType::kLineStrip,
            xenos::PrimitiveType::kLineLoop, xenos::PrimitiveType::kRectangleList,
            xenos::PrimitiveType::kQuadList, xenos::PrimitiveType::kTriangleStrip}) {
        for (uint32_t bits : {0u, 16u, 32u}) {
          const Case test{primitive == xenos::PrimitiveType::kQuadList ? 8u : 9u,
                          bits,
                          bits == 32   ? 2u
                          : bits == 16 ? 1u
                                       : 0u,
                          0,
                          -3,
                          bits != 0 && (primitive == xenos::PrimitiveType::kLineStrip ||
                                        primitive == xenos::PrimitiveType::kTriangleStrip),
                          false};
          bytes += Run(shared, system, constants, test, [&](uint32_t count) {
            std::vector<uint32_t> registers(RegisterFile::kRegisterCount, 0);
            std::memcpy(registers.data() + XE_GPU_REG_SHADER_CONSTANT_000_X, fc.data(), sizeof(fc));
            reg::SQ_PROGRAM_CNTL program{};
            program.vs_num_reg = 1;
            registers[XE_GPU_REG_SQ_PROGRAM_CNTL] = program.value;
            registers[XE_GPU_REG_VGT_INDX_OFFSET] = uint32_t(sc.vertex_base_index);
            registers[XE_GPU_REG_VGT_MIN_VTX_INDX] = sc.vertex_index_min;
            registers[XE_GPU_REG_VGT_MAX_VTX_INDX] = sc.vertex_index_max;
            registers[XE_GPU_REG_VGT_MULTI_PRIM_IB_RESET_INDX] = sc.vertex_index_reset;
            reg::PA_SU_SC_MODE_CNTL mode{};
            mode.multi_prim_ib_ena = test.reset;
            registers[XE_GPU_REG_PA_SU_SC_MODE_CNTL] = mode.value;
            // A real offscreen front buffer lets the frame boundary perform
            // accurate readback. The fast path intentionally returns an older
            // export and cannot qualify the result of the current draw.
            xenos::xe_gpu_texture_fetch_t front{};
            front.type = xenos::FetchConstantType::kTexture;
            front.format = xenos::TextureFormat::k_8_8_8_8;
            front.dimension = xenos::DataDimension::k2DOrStacked;
            front.base_address = (128 * 1024) >> 12;
            front.pitch = 2;
            front.size_2d.width = 63;
            front.size_2d.height = 31;
            front.swizzle = 0x688;
            std::memcpy(registers.data() + XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0, &front,
                        sizeof(front));
            cp->RestoreRegisters(0, registers.data(), uint32_t(registers.size()), true);
            cp->TracePlaybackWroteMemory(0, 2 * 1024 * 1024);
            reg::VGT_DRAW_INITIATOR initiator{};
            initiator.prim_type = primitive;
            initiator.num_indices = count;
            initiator.source_select =
                bits ? xenos::SourceSelect::kDMA : xenos::SourceSelect::kAutoIndex;
            initiator.index_size =
                bits == 32 ? xenos::IndexFormat::kInt32 : xenos::IndexFormat::kInt16;
            std::vector<uint32_t> draw = {initiator.value};
            if (bits) {
              reg::VGT_DMA_SIZE size{};
              size.num_words = count;
              size.swap_mode = xenos::Endian(test.endian);
              draw.push_back(sc.vertex_index_load_address);
              draw.push_back(size.value);
            }
            execute(xenos::PM4_DRAW_INDX_2, draw);
            const auto restored_front = gpu->register_file()->GetTextureFetch(0);
            Require(std::memcmp(&front, &restored_front, sizeof(front)) == 0,
                    "Draw changed the offscreen front-buffer descriptor");
            cp->IssueSwap(128 * 1024, 64, 32);
            rex::ui::RawImage frame;
            Require(gpu->presenter()->CaptureGuestOutput(frame) && frame.width == 64 &&
                        frame.height == 32,
                    "Frame was not presented after accurate memory export readback");
            for (uint32_t y = 0; y < frame.height; ++y) {
              for (uint32_t x = 0; x < frame.width; ++x) {
                for (uint32_t channel = 0; channel < 3; ++channel) {
                  int value = frame.data[y * frame.stride + x * 4 + channel];
                  Require(std::abs(value - 0xA7) <= 1,
                          "Offscreen presentation changed the flat front-buffer colour");
                }
              }
            }
          });
          ++cases;
        }
      }
      finished.set_value();
    } catch (...) {
      finished.set_exception(std::current_exception());
    }
  });
  completion.get();
  runtime.Shutdown();
  std::string report = "{\n  \"passed\": true,\n  \"scope\": \"production command processor\",\n";
  report += "  \"cases\": " + std::to_string(cases) +
            ",\n  \"exact_bytes\": " + std::to_string(bytes) +
            ",\n  \"presented_frames\": " + std::to_string(cases) + "\n}\n";
  std::ofstream file(output / "command-processor-report.json");
  file << report;
  Require(bool(file), "Could not write command processor report");
  std::fputs(report.c_str(), stdout);
}
}  // namespace

int main(int argc, char** argv) {
  try {
    Require(
        argc == 2 || (argc == 3 && (std::string(argv[2]) == "--translate-only" ||
                                    std::string(argv[2]) == "--command-processor")),
        "usage: vertex_memexport_check output-directory [--translate-only|--command-processor]");
    std::filesystem::path output(argv[1]);
    std::filesystem::create_directories(output);
    if (argc == 3 && std::string(argv[2]) == "--command-processor") {
      RunCommandProcessor(output);
      return 0;
    }
    Fixture f;
    SpirvShaderTranslator::Features features;
    features.spirv_version = spv::Spv_1_3;
    features.max_storage_buffer_range = f.properties.limits.maxStorageBufferRange;
    auto compute = Translate(features, Shader::HostVertexShaderType::kMemExportCompute);
    auto raster = Translate(features, Shader::HostVertexShaderType::kVertex);
    for (auto entry : {std::pair{"compute.spv", &compute}, std::pair{"raster.spv", &raster}}) {
      std::ofstream file(output / entry.first, std::ios::binary);
      file.write(reinterpret_cast<const char*>(entry.second->data()), entry.second->size());
      Require(bool(file), "Could not write translated shader");
    }
    if (argc == 3)
      return 0;
    auto shared = f.MakeBuffer(2 * 1024 * 1024, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    auto system = f.MakeBuffer(sizeof(SpirvShaderTranslator::SystemConstants),
                               VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    auto constants = f.MakeBuffer(64, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    auto bound = f.Prepare(compute, shared, system, constants,
                           1u << SpirvShaderTranslator::GetSharedMemoryStorageBufferCountLog2(
                               features.max_storage_buffer_range));
    uint64_t bytes = 0;
    uint32_t cases = 0;
    for (uint32_t bits : {0u, 16u, 32u}) {
      for (uint32_t endian = 0; endian < (bits == 32 ? 4u : bits == 16 ? 2u : 1u); ++endian) {
        for (uint32_t export_endian = 0; export_endian < 6; ++export_endian) {
          for (int32_t base : {-3, 0, 5}) {
            const Case test{9, bits, endian, export_endian, base, bits != 0, (cases & 1) != 0};
            bytes += Run(shared, system, constants, test,
                         [&](uint32_t count) { f.Dispatch(bound, count); });
            ++cases;
          }
        }
      }
    }
    for (uint32_t count : {0u, 1u, 65534u, 65535u, 65536u, 65537u}) {
      bytes += Run(shared, system, constants, {count, 0, 0, 0, 0, false, false},
                   [&](uint32_t vertices) { f.Dispatch(bound, vertices); });
      ++cases;
    }
    std::string report = "{\n  \"passed\": true,\n  \"vertex_storage_writes_enabled\": false,\n";
    report += "  \"cases\": " + std::to_string(cases) +
              ",\n  \"exact_bytes\": " + std::to_string(bytes) + "\n}\n";
    std::ofstream file(output / "report.json");
    file << report;
    Require(bool(file), "Could not write report");
    std::printf("Device: %s\n%s", f.properties.deviceName, report.c_str());
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
  }
}
