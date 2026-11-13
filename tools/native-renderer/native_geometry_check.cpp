// Exercise point and rectangle expansion without geometry shaders through the
// production command processor, including original DMA indices and readback.
// --gpu-indices adds GPU-only producers, GPU rewrites and later CPU ownership.
#include <rex/cvar.h>
#include <rex/logging.h>
#if REX_HAS_VULKAN
#include <rex/graphics/vulkan/command_processor.h>
#include <rex/graphics/vulkan/graphics_system.h>
#endif
#if REX_HAS_D3D11
#include <rex/graphics/d3d11/graphics_system.h>
#endif
#if REX_HAS_D3D12
#include <rex/graphics/d3d12/graphics_system.h>
#endif
#include <rex/graphics/command_processor.h>
#include <rex/graphics/native_gpu_constants.h>
#include <rex/graphics/native_gpu_direct.h>
#include <rex/graphics/native_gpu_state.h>
#include <rex/graphics/native_gpu_coherency.h>
#include <rex/graphics/native_gpu_sync.h>
#include <rex/graphics/native_gpu_indirect.h>
#include <rex/graphics/native_gpu_shader_reference.h>
#include <rex/graphics/native_gpu_program.h>
#include <rex/graphics/native_gpu_predication.h>
#include <rex/graphics/native_gpu_ports.h>
#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/graphics/format/ucode.h>
#include <rex/runtime.h>
#include <rex/ui/presenter.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <stdexcept>
#include <string>
#include <vector>
namespace {
namespace xenos = rex::graphics::xenos;
namespace ucode = rex::graphics::ucode;
namespace reg = rex::graphics::reg;
namespace native_gpu_calls = rex::graphics::native_gpu_calls;
void Require(bool c, const char* m) {
  if (!c)
    throw std::runtime_error(m);
}
struct Buffer {
  void* mapped;
};
struct IndexConstants {
  uint32_t compute_memexport_vertex_count, vertex_index_load_address;
  xenos::Endian vertex_index_endian;
  int32_t vertex_base_index;
  uint32_t vertex_index_min, vertex_index_max, vertex_index_reset;
};
std::vector<uint32_t> Microcode(bool fetch, bool affine = false, bool rounded = false,
                              uint32_t vertex_group = 1, uint32_t vertex_stride = 16) {
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
  if (!fetch)
    return words;
  const std::array<uint32_t, 3> address = {words[3], words[4], words[5]};
  words = {1u | (3u << 12) | (3u << 16),
           uint32_t(ucode::ControlFlowOpcode::kExecEnd) << 12,
           0,
           (1u << 12) | (1u << 19) | (vertex_group << 20),
           0x688u | (uint32_t(xenos::VertexFormat::k_32_32_32_32_FLOAT) << 16),
           vertex_stride / 4};
  words.insert(words.end(), address.begin(), address.end());
  words.push_back(uint32_t(ucode::ExportRegister::kExportData0) | (1u << 15) | (15u << 16) |
                  (uint32_t(ucode::AluScalarOpcode::kRetainPrev) << 26));
  words.push_back(0);
  words.push_back((1u << 16) | (4u << 8) | (uint32_t(ucode::AluVectorOpcode::kAdd) << 24) |
                  (1u << 31));
  if (affine) {
    // r2.x = (r0.x + c5.x) * c5.y. Keep r0.x unchanged for the export address.
    // MUL_CONST's register bit 1 lives in src3_sel; its constant uses W swizzle.
    const std::array<uint32_t, 6> expression = {
        2u | (1u << 16) | (uint32_t(ucode::AluScalarOpcode::kRetainPrev) << 26),
        0,
        (5u << 8) | (uint32_t(ucode::AluVectorOpcode::kAdd) << 24) | (1u << 31),
        (2u << 8) | (1u << 20) | (uint32_t(ucode::AluScalarOpcode::kMulsc0) << 26),
        0x80,
        5u | (1u << 29)};
    words.insert(words.begin() + 3, expression.begin(), expression.end());
    words[0] = 1u | (5u << 12) | (3u << 20);
    words[9] |= 2u << 5;
    if (rounded)
      words[10] |= 1u << 15;
  }
  return words;
}

std::array<float, 4> VertexValue(int32_t index) {
  return {float(index) * .375f + .25f, -float(index) * 2 - .75f, float(index) * 4 + 1.25f,
          float(index) * .5f - 2.5f};
}

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
  bool fetch;
  bool affine = false, rounded = false;
  float offset = 0.0f, scale = 1.0f;
  bool large_input = false;
  uint32_t vertex_stride = 16;
};

std::array<float, 4> FetchedValue(uint32_t index, const Case& test) {
  volatile float address = float(index);
  if (test.affine) {
    address = address + test.offset;
    address = address * test.scale;
    if (test.rounded)
      address = address + 0.5f;
  }
  const float fetched = std::floor(address);
  Require(fetched >= -128 && fetched < (test.large_input ? 262144 : 512),
          "Fixture vertex address is outside its input stream");
  return VertexValue(int32_t(fetched));
}

template <typename Dispatch>
uint64_t Run(Buffer shared, Buffer system, Buffer constants, const Case& test, Dispatch dispatch,
             bool gpu_indices = false) {
  const uint32_t kSize = (test.large_input ? 8 : 2) * 1024 * 1024;
  constexpr uint32_t kOutput = 256 * 1024, kIndices = 4098, kInput = 1536 * 1024;
  std::vector<uint8_t> expected(kSize, 0xA7);
  auto* sc = static_cast<IndexConstants*>(system.mapped);
  *sc = {};
  sc->compute_memexport_vertex_count = test.count;
  sc->vertex_index_load_address = test.index_bits == 16 ? kIndices : kIndices + 2;
  sc->vertex_index_endian = xenos::Endian(test.endian);
  sc->vertex_base_index = test.base;
  sc->vertex_index_min = test.count > 50 ? 0 : 2;
  sc->vertex_index_max = test.count > 50 ? 70000 : 50;
  sc->vertex_index_reset = 31;
  auto* fc = static_cast<float*>(constants.mapped);
  std::array<float, 20> values = {0, 1, 0, 0, 0, 0, 0, 0, 1, 2, 4, .5f, .25f, -.5f, 3, -2};
  std::copy(values.begin(), values.end(), fc);
  fc[20] = test.offset;
  fc[21] = test.scale;
  auto* stream = reinterpret_cast<uint32_t*>(fc + 4);
  stream[0] = 0x40000000 | (kOutput >> 2);
  stream[1] = 0x4B000000;
  stream[2] = 0x4B000000 | (uint32_t(xenos::ColorFormat::k_32_32_32_32_FLOAT) << 8) |
              test.export_endian | (uint32_t(test.swap_red_blue) << 19);
  stream[3] = 0x4B000000 | 70001;
  const std::array<uint32_t, 9> indices = {7, 1, 11, 4, 31, 65, 8, 0, 19};
  for (uint32_t i = 0; test.index_bits && i < test.count; ++i) {
    uint32_t index = indices.at(i);
    if (test.index_bits == 32 && gpu_indices)
      index |= 0x40000000;  // Normal float bits survive the GPU producer's ALU.
    if (test.index_bits == 32 && i == 2)
      index = (index & 0xFFFFFF) | 0xDE000000;
    if (test.index_bits == 32 && test.reset && index == 31)
      index |= 0xAB000000;
    uint32_t encoded = Swap(index, test.endian);
    std::memcpy(expected.data() + sc->vertex_index_load_address + i * (test.index_bits / 8),
                &encoded, test.index_bits / 8);
  }
  if (test.fetch)
    for (int32_t i = -128; i < (test.large_input ? 262144 : 512); ++i) {
      auto value = VertexValue(i);
      const ptrdiff_t address = ptrdiff_t(kInput) + ptrdiff_t(i) * ptrdiff_t(test.vertex_stride);
      std::memcpy(expected.data() + address, value.data(), 16);
    }
  if (gpu_indices) {
    // Only the producer gets the real index bytes. The CPU's index page stays
    // stale throughout both consumer draws, including their bounds checks.
    const uint32_t kProducerInput = test.large_input ? 6 * 1024 * 1024 : 1792 * 1024;
    uint32_t first = sc->vertex_index_load_address & ~3u;
    std::memcpy(expected.data() + kProducerInput, expected.data() + first, 64);
    std::memset(expected.data() + first, 0xA7, 64);
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
    if (test.fetch)
      data = FetchedValue(index, test);
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
  if (gpu_indices) {
    const uint32_t kProducerInput = test.large_input ? 6 * 1024 * 1024 : 1792 * 1024;
    const uint32_t prefix = sc->vertex_index_load_address & 3u;
    uint32_t changed = 17 | (test.index_bits == 32 ? 0x40000000 : 0);
    changed = Swap(changed, test.endian);
    std::memcpy(expected.data() + kProducerInput + prefix, &changed, test.index_bits / 8);
    uint32_t first = sc->vertex_index_load_address & ~3u;
    std::memcpy(expected.data() + first, expected.data() + kProducerInput, 64);
    changed = Swap(21 | (test.index_bits == 32 ? 0x40000000 : 0), test.endian);
    std::memcpy(expected.data() + first + prefix, &changed, test.index_bits / 8);
    for (uint32_t original : {17u, 21u}) {
      uint32_t index = std::clamp((original + uint32_t(test.base)) & 0xFFFFFF, sc->vertex_index_min,
                                  sc->vertex_index_max);
      auto value = test.fetch ? FetchedValue(index, test)
                              : std::array<float, 4>{float(index) + .25f, float(index) * 2 - .5f,
                                                     float(index) * 4 + 3, float(index) * .5f - 2};
      std::memcpy(expected.data() + kOutput + index * 16, value.data(), 16);
    }
  }
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

struct DrawObserver {
  uint64_t draws = 0, indexed = 0, halfword_offsets = 0, epoch_after_issue = 0;
  uint64_t explicit_draws = 0;
} draw_observer;

#if REX_HAS_VULKAN
class ObservedVulkanCommandProcessor : public rex::graphics::vulkan::VulkanCommandProcessor {
 public:
  using VulkanCommandProcessor::VulkanCommandProcessor;

 protected:
  bool IssueDraw(xenos::PrimitiveType primitive, uint32_t count, IndexBufferInfo* indices,
                 bool explicit_mode) override {
    const auto control = register_file_->Get<reg::VGT_DRAW_INITIATOR>();
    Require(explicit_mode == xenos::IsMajorModeExplicit(control.major_mode, control.prim_type),
            "The shared consumer changed its native draw major mode");
    if (explicit_mode)
      ++draw_observer.explicit_draws;
    if (indices) {
      auto size = register_file_->Get<reg::VGT_DMA_SIZE>();
      uint32_t base = register_file_->values[rex::graphics::XE_GPU_REG_VGT_DMA_BASE];
      const uint32_t stride = control.index_size == xenos::IndexFormat::kInt16 ? 2 : 4;
      Require(indices->guest_base == base - base % stride && indices->format == control.index_size &&
                  indices->endianness == size.swap_mode && indices->count == count &&
                  indices->length == size_t(size.num_words) * stride,
              "The shared consumer changed its native index-buffer contract");
      ++draw_observer.indexed;
      if (stride == 2 && (base & 2))
        ++draw_observer.halfword_offsets;
    }
    ++draw_observer.draws;
    bool result = VulkanCommandProcessor::IssueDraw(primitive, count, indices, explicit_mode);
    draw_observer.epoch_after_issue = guest_progress_epoch();
    return result;
  }
};

class ObservedVulkanGraphicsSystem : public rex::graphics::vulkan::VulkanGraphicsSystem {
 protected:
  std::unique_ptr<rex::graphics::CommandProcessor> CreateCommandProcessor() override {
    return std::make_unique<ObservedVulkanCommandProcessor>(this, kernel_state_);
  }
};
#endif

void RunCommandProcessor(const std::filesystem::path& output, const std::string& backend,
                         bool gpu_indices, bool affine_addresses, uint32_t buffer_budget,
                         bool clear_caches, bool large_inputs, bool native_calls,
                         bool native_constants, bool native_vertices, bool native_state,
                         uint32_t vertex_stride, bool explicit_major, bool native_coherency,
                         bool native_shaders, bool native_predication, bool native_ports,
                         bool native_sync, bool native_indirect, bool shader_memory_cache,
                         bool native_reference, bool native_selected_ranges,
                         bool native_typed_indirect, bool native_direct) {
  auto set = [](const char* key, const char* value) {
    Require(rex::cvar::SetFlagByName(key, value), "Could not set fixture cvar");
  };
  for (auto setting :
       {std::pair{"readback_memexport_fast", "false"},
        std::pair{"async_shader_compilation", "false"},
        std::pair{"force_convert_quad_lists_to_triangle_lists", "true"},
        std::pair{"force_convert_triangle_fans_to_lists", "true"},
        std::pair{"primitive_processor_cache_min_indices", "1"},
        std::pair{"native_gpu_diagnostics", "true"}, std::pair{"native_gpu_strict", "true"}})
    set(setting.first, setting.second);
  set("native_gpu_report_path", (output / "native-coverage.json").string().c_str());
  if (buffer_budget)
    set("native_resolve_buffer_mb", std::to_string(buffer_budget).c_str());
  std::unique_ptr<rex::graphics::GraphicsSystem> graphics;
#if REX_HAS_VULKAN
  if (backend == "native" || backend == "compute") {
    set("vulkan_vertex_memexport_compute", backend == "compute" ? "true" : "false");
    for (auto setting :
         {std::pair{"vulkan_readback_memexport", "true"},
          std::pair{"vulkan_geometry_shader_primitives", "false"},
          std::pair{"render_target_path_vulkan", "native"},
          std::pair{"native_vertex_buffers", "true"}, std::pair{"native_texture_uploads", "true"},
          std::pair{"native_index_buffers", "true"},
          std::pair{"gpu_memory_mirror_on_demand", "true"},
          std::pair{"native_resolve_buffers", "true"},
          std::pair{"native_resolve_buffer_reads", "true"},
          std::pair{"native_resolve_buffer_lazy_memory", "true"}})
      set(setting.first, setting.second);
    graphics = std::make_unique<ObservedVulkanGraphicsSystem>();
  }
#endif
#if REX_HAS_D3D11
  if (backend == "dx11") {
    set("d3d11_native_vertex_buffers", "true");
    graphics = std::make_unique<rex::graphics::d3d11::D3D11GraphicsSystem>();
  }
#endif
#if REX_HAS_D3D12
  if (backend == "dx12" || backend == "dx12-bindful") {
    set("d3d12_bindless", backend == "dx12-bindful" ? "false" : "true");
    set("d3d12_readback_memexport", "true");
    set("render_target_path_d3d12", "rtv");
    set("d3d12_native_vertex_buffers", "true");
    graphics = std::make_unique<rex::graphics::d3d12::D3D12GraphicsSystem>();
  }
#endif
  Require(bool(graphics), "Requested backend is not built into this fixture");
  rex::Runtime runtime("", output / "userdata");
  auto* gpu = graphics.get();
  Require(graphics->SetupPresentation(nullptr) == 0, "Could not create offscreen presentation");
  rex::RuntimeConfig config;
  config.graphics = std::move(graphics);
  Require(runtime.Setup(std::move(config)) == 0, "Could not initialize the command processor");
  auto* cp = gpu->command_processor();
  if (native_calls || native_constants)
    Require(native_gpu_calls::Enabled(), "This check needs a matching native-call runtime");
  if (native_vertices)
    Require((native_gpu_calls::Capabilities() &
             (native_gpu_calls::kVerticesCapability | native_gpu_calls::kIndexedVerticesCapability |
              native_gpu_calls::kRangeDrawCapability)) ==
                (native_gpu_calls::kVerticesCapability | native_gpu_calls::kIndexedVerticesCapability |
                 native_gpu_calls::kRangeDrawCapability),
            "This check needs the native vertices consumer");
  if (native_state)
    Require(native_gpu_calls::Capabilities() & rex::graphics::native_gpu_state::kCapability,
            "This check needs the native state consumer");
  if (native_coherency)
    Require(native_gpu_calls::Capabilities() & rex::graphics::native_gpu_coherency::kCapability,
            "This check needs the native coherency consumer");
  if (native_shaders)
    Require(native_gpu_calls::Capabilities() & rex::graphics::native_gpu_program::kCapability,
            "This check needs the native program consumer");
  if (native_predication)
    Require(native_gpu_calls::Capabilities() & rex::graphics::native_gpu_predication::kCapability,
            "This check needs the native predication consumer");
  if (native_ports)
    Require(native_gpu_calls::Capabilities() & rex::graphics::native_gpu_ports::kCapability,
            "This check needs the native initialization port consumer");
  if (native_sync)
    Require(native_gpu_calls::Capabilities() & rex::graphics::native_gpu_sync::kCapability,
            "This check needs the native synchronization consumer");
  if (native_indirect)
    Require(native_gpu_calls::Capabilities() & rex::graphics::native_gpu_indirect::kCapability,
            "This check needs the native indirect-constant consumer");
  if (native_reference)
    Require(
        native_gpu_calls::Capabilities() & rex::graphics::native_gpu_shader_reference::kCapability,
        "This check needs the native shader-reference consumer");
  if (native_selected_ranges)
    Require(
        native_gpu_calls::Capabilities() & rex::graphics::native_gpu_predication::kRangeCapability,
        "This check needs the native conditional-range consumer");
  if (native_typed_indirect)
    Require(native_gpu_calls::Capabilities() & rex::graphics::native_gpu_indirect::kTypedCapability,
            "This check needs the native typed-constant consumer");
  if (native_direct)
    Require(native_gpu_calls::Capabilities() & rex::graphics::native_gpu_direct::kCapability,
            "This check needs the native direct-draw consumer");
  std::promise<void> finished;
  auto completion = finished.get_future();
  uint64_t bytes = 0;
  uint32_t cases = 0, cache_clears = 0;
  uint64_t native_submissions = 0, bare_submissions = 0, constant_updates = 0;
  uint64_t requested_explicit_draws = 0;
  uint64_t vertices_submissions = 0, skipped_vertices = 0;
  uint64_t indexed_vertices_submissions = 0, range_submissions = 0, indexed_range_submissions = 0;
  uint64_t skipped_ranges = 0;
  uint64_t skipped_bare = 0;
  uint64_t state_submissions = 0, state_registers = 0;
  uint64_t coherency_submissions = 0;
  uint64_t shader_submissions = 0, invalidation_submissions = 0;
  uint64_t mask_submissions = 0, selected_states = 0, skipped_states = 0;
  uint64_t restored_snapshots = 0;
  uint64_t port_submissions = 0, port_writes = 0, pixel_shader_bindings = 0;
  uint64_t sync_coherency = 0, sync_events = 0;
  uint64_t indirect_submissions = 0, indirect_words = 0;
  uint64_t shader_memory_bindings = 0;
  uint64_t shader_references = 0, skipped_shader_references = 0;
  uint64_t selected_ranges = 0, selected_range_words = 0, predicated_shader_references = 0;
  uint64_t typed_indirect_submissions = 0, typed_restore_calls = 0;
  uint32_t typed_indirect_types = 0;
  uint64_t direct_draws = 0, direct_indexed = 0, direct_auto = 0, direct_queries = 0;
  uint64_t direct_padded_auto = 0, direct_skips = 0;
  cp->CallInThread([&] {
    try {
      using namespace rex::graphics;
      constexpr uint32_t kPacket = 8 * 1024 * 1024;
      auto* packet = runtime.memory()->TranslatePhysical<uint32_t*>(kPacket);
      const auto submit_coherency = [&](const native_gpu_coherency::Call& call) {
        const auto words = native_gpu_coherency::Encode(call);
        for (uint32_t i = 0; i < words.size(); ++i)
          packet[i] = std::byteswap(words[i]);
        const uint64_t epoch = cp->guest_progress_epoch();
        cp->ExecutePacket(kPacket, uint32_t(words.size()));
        const auto* values = gpu->register_file()->values;
        Require(cp->guest_progress_epoch() == epoch &&
                    values[XE_GPU_REG_COHER_DEST_BASE_1] == call.destination_base &&
                    values[XE_GPU_REG_COHER_SIZE_HOST] == call.size &&
                    values[XE_GPU_REG_COHER_BASE_HOST] == call.base &&
                    values[XE_GPU_REG_COHER_STATUS_HOST] == 0,
                "Native coherency changed its state, wait result or guest write epoch");
        ++coherency_submissions;
        Require(native_gpu_coherency::CallCount() == coherency_submissions,
                "Native coherency did not execute through its real consumer");
      };
      auto submit_state = [&](uint32_t first, const uint32_t* values, uint32_t count) {
        uint32_t header;
        Require(native_gpu_state::EncodeHeader(first, count, header), "Could not encode native state");
        packet[0] = std::byteswap(header);
        for (uint32_t i = 0; i < count; ++i)
          packet[i + 1] = std::byteswap(values[i]);
        const uint64_t epoch = cp->guest_progress_epoch();
        cp->ExecutePacket(kPacket, count + 1);
        ++state_submissions;
        state_registers += count;
        Require(cp->guest_progress_epoch() == epoch &&
                    std::equal(values, values + count, gpu->register_file()->values + first),
                "Native state lost register data or changed the guest write epoch");
      };
      const auto selected_state = [&](uint32_t operand, uint32_t value, bool applied) {
        namespace predication = native_gpu_predication;
        uint32_t first;
        Require(predication::DecodeRegister(operand, first), "Invalid fixture state operand");
        const uint32_t old = gpu->register_file()->values[first];
        packet[0] = std::byteswap(predication::kStateHeader);
        packet[1] = std::byteswap(operand);
        packet[2] = std::byteswap(value);
        const uint64_t epoch = cp->guest_progress_epoch();
        cp->ExecutePacket(kPacket, 3);
        (applied ? selected_states : skipped_states)++;
        Require(gpu->register_file()->values[first] == (applied ? value : old) &&
                    cp->guest_progress_epoch() == epoch &&
                    predication::StateCount() == selected_states &&
                    predication::SkippedStateCount() == skipped_states,
                "Native conditional state lost its predicate, value, counter or write epoch");
      };
      const auto mask_half = [&](bool high, uint32_t value) {
        packet[0] = std::byteswap(high ? native_gpu_predication::kMaskHighHeader
                                       : native_gpu_predication::kMaskLowHeader);
        packet[1] = std::byteswap(value);
        const uint64_t epoch = cp->guest_progress_epoch();
        cp->ExecutePacket(kPacket, 2);
        ++mask_submissions;
        Require(cp->guest_progress_epoch() == epoch &&
                    native_gpu_predication::MaskCount() == mask_submissions,
                "Native mask update was skipped or changed the guest write epoch");
      };
      const auto submit_typed = [&](uint32_t type, uint32_t index, const uint32_t* values,
                                    uint32_t count) {
        constexpr uint32_t kSource = kPacket + 0x20000;
        const native_gpu_indirect::TypedCall call{
            kSource | 0xC0000000u, 0xA5008800u | (type << 16) | index, count | 0xF000u};
        native_gpu_indirect::TypedRange range;
        Require(native_gpu_indirect::DecodeTyped(call, range) && range.count == count,
                "Invalid typed indirect fixture range");
        auto* source = runtime.memory()->TranslatePhysical<uint32_t*>(range.address);
        auto* registers = gpu->register_file()->values;
        std::vector<uint32_t> before(registers + range.first_register - 1,
                                     registers + range.first_register + count + 1);
        packet[0] = std::byteswap(native_gpu_indirect::kTypedHeader);
        packet[1] = std::byteswap(call.address);
        packet[2] = std::byteswap(call.offset_type);
        packet[3] = std::byteswap(call.size);
        // Stage first, then replace the source, so the consumer must use the
        // current bytes. Preserve NaNs, signed zero and other raw bit patterns.
        for (uint32_t i = 0; i < count; ++i)
          source[i] = std::byteswap(values[i]);
        const uint64_t epoch = cp->guest_progress_epoch();
        cp->ExecutePacket(kPacket, 4);
        ++indirect_submissions;
        ++typed_indirect_submissions;
        indirect_words += count;
        typed_indirect_types |= 1u << type;
        Require(registers[range.first_register - 1] == before.front() &&
                    registers[range.first_register + count] == before.back() &&
                    std::equal(values, values + count, registers + range.first_register) &&
                    cp->guest_progress_epoch() == epoch &&
                    native_gpu_indirect::CallCount() == indirect_submissions &&
                    native_gpu_indirect::WordCount() == indirect_words &&
                    native_gpu_indirect::TypedCallCount() == typed_indirect_submissions,
                "Typed constants lost current bytes, raw values, neighbors, callbacks or epoch");
      };
      auto restore_base = [&](const auto& values) {
        if (!native_state) {
          cp->RestoreRegisters(0, values.data(), uint32_t(values.size()), true);
          return;
        }
        auto poison = values;
        for (uint32_t page : native_gpu_state::kPages) {
          const uint32_t count = page == 0x4800 ? 192 : page == 0x4900 ? 40 : page == 0x5000 ? 3 : 256;
          for (uint32_t i = 0; i < count; ++i)
            poison[page + i] ^= 0xA5A50102u;
        }
        cp->RestoreRegisters(0, poison.data(), uint32_t(poison.size()), true);
        for (uint32_t page : native_gpu_state::kPages) {
          const uint32_t count = page == 0x4800   ? 192
                                 : page == 0x4900 ? 40
                                 : page == 0x5000 ? 3
                                                  : 256;
          if (native_typed_indirect && page < 0x5000) {
            if (page == 0x4900) {
              submit_typed(2, 0, values.data() + page, 8);
              submit_typed(3, 0, values.data() + page + 8, 32);
              typed_restore_calls += 2;
            } else {
              submit_typed(page < 0x4000 ? 4 : 1, page < 0x4000 ? page - 0x2000 : 0,
                           values.data() + page, count);
              ++typed_restore_calls;
            }
          } else
            submit_state(page, values.data() + page, count);
        }
      };
      auto restore_snapshot = [&](const auto& values) {
        ++restored_snapshots;
        restore_base(values);
        if (native_predication) {
          for (uint32_t operand : {0x40000u, 0x40104u, 0x40203u}) {
            uint32_t first;
            Require(native_gpu_predication::DecodeRegister(operand, first),
                    "Invalid snapshot operand");
            const uint32_t poison = values[first] ^ 0xA5A50102u;
            cp->RestoreRegisters(first, &poison, 1, true);
            selected_state(operand, values[first], true);
          }
        }
      };
      auto execute = [&](xenos::Type3Opcode opcode, const std::vector<uint32_t>& payload,
                         bool predicated = false) {
        if (opcode == xenos::PM4_DRAW_INDX_2 && !payload.empty()) {
          reg::VGT_DRAW_INITIATOR control;
          control.value = payload[0];
          requested_explicit_draws +=
              xenos::IsMajorModeExplicit(control.major_mode, control.prim_type);
        }
        if (native_calls && opcode == xenos::PM4_DRAW_INDX_2) {
          Require(!payload.empty(), "The native draw has no arguments");
          native_gpu_calls::DrawCall call{};
          const auto& registers = gpu->register_file()->values;
          std::copy_n(registers + 0x48BA, call.fetch.size(), call.fetch.begin());
          call.coherent_size = registers[XE_GPU_REG_COHER_SIZE_HOST];
          call.coherent_base = registers[XE_GPU_REG_COHER_BASE_HOST];
          call.coherent_status = registers[XE_GPU_REG_COHER_STATUS_HOST];
          std::copy_n(registers + 0x5000, call.constant_flush.size(), call.constant_flush.begin());
          call.initiator = payload[0];
          const bool indexed = ((call.initiator >> 6) & 3) == 0;
          Require(payload.size() == (indexed ? 3u : 1u), "Unexpected native draw arguments");
          if (indexed) {
            call.index_base = payload[1];
            call.index_size = payload[2];
          }
          if (native_direct) {
            const bool query = direct_draws & 1;
            const bool padded = !indexed && (direct_auto++ & 1);
            std::vector<uint32_t> args;
            if (query)
              args.push_back(0xA5A50100);
            args.insert(args.end(), payload.begin(), payload.end());
            if (padded) {
              args.push_back(0xDEADBEEF);
              args.push_back(0x91E10DA5);
            }
            const uint32_t old =
                xenos::MakePacketType3(query ? xenos::PM4_DRAW_INDX : xenos::PM4_DRAW_INDX_2,
                                       uint16_t(args.size())) |
                ((direct_draws & 2) ? 1u : 0u);
            uint32_t header;
            Require(native_gpu_direct::EncodeLegacyHeader(old, header),
                    "Cannot encode live-ring draw arguments");
            const uint32_t before_base = registers[XE_GPU_REG_VGT_DMA_BASE];
            const uint32_t before_size = registers[XE_GPU_REG_VGT_DMA_SIZE];
            packet[0] = std::byteswap(header);
            for (uint32_t i = 0; i < args.size(); ++i)
              packet[i + 1] = std::byteswap(args[i]);
            cp->ExecutePacket(kPacket, uint32_t(args.size() + 1));
            ++native_submissions;
            ++direct_draws;
            direct_indexed += indexed;
            direct_queries += query;
            direct_padded_auto += padded;
            Require(
                native_gpu_direct::CallCount() == direct_draws &&
                    registers[XE_GPU_REG_VGT_DRAW_INITIATOR] == call.initiator &&
                    registers[XE_GPU_REG_VGT_DMA_BASE] ==
                        (indexed ? call.index_base : before_base) &&
                    registers[XE_GPU_REG_VGT_DMA_SIZE] == (indexed ? call.index_size : before_size),
                "Direct draw lost control/DMA values, unused operands or execution count");
            if (backend == "native" || backend == "compute")
              Require(cp->guest_progress_epoch() == draw_observer.epoch_after_issue,
                      "Direct draw advanced the epoch beyond its backend work");
            return;
          }
          std::array<uint32_t, 27> encoded{};
          const bool range = native_vertices && native_submissions % 3 == 0;
          const bool vertices = native_vertices && native_submissions % 3 == 1;
          const bool bare = !range && !vertices && !indexed && (native_submissions & 1);
          const uint32_t dwords = range ? (indexed ? 7 : 5)
                                  : vertices ? (indexed ? 18 : 16)
                                  : bare ? 2 : indexed ? 27 : 24;
          const uint32_t first_vertex = registers[XE_GPU_REG_VGT_INDX_OFFSET];
          if (vertices) {
            native_gpu_calls::VerticesDrawCall vertex_call{
                first_vertex, call.fetch, call.constant_flush, 0, call.initiator,
                call.index_base, call.index_size};
            Require(native_gpu_calls::EncodeVertices(vertex_call, {encoded.data(), dwords}),
                    "Could not encode native vertices");
            ++vertices_submissions;
            indexed_vertices_submissions += indexed;
          } else if (range) {
            native_gpu_calls::RangeDrawCall range_call{
                first_vertex, 0, call.initiator, call.index_base, call.index_size};
            Require(native_gpu_calls::EncodeRange(range_call, {encoded.data(), dwords}),
                    "Could not encode native range draw");
            ++range_submissions;
            indexed_range_submissions += indexed;
          } else if (bare) {
            encoded[0] = 0xC0000000u | (native_gpu_calls::kBareDrawOpcode << 8);
            encoded[1] = call.initiator;
            ++bare_submissions;
          } else {
            Require(native_gpu_calls::EncodeDraw(call, indexed, {encoded.data(), dwords}),
                    "Could not encode the native call transport");
          }
          if (vertices || range) {
            uint32_t wrong_offset = first_vertex ^ 0x1234u;
            cp->RestoreRegisters(XE_GPU_REG_VGT_INDX_OFFSET, &wrong_offset, 1, true);
          }
          if (!bare && !range) {
            // The call must restore the descriptor, not inherit the old one.
            // Fetch cases use this group's vertices on the real GPU, so an
            // omitted restore also fails the independent output comparison.
            std::array<uint32_t, 6> invalid_fetch{};
            cp->RestoreRegisters(0x48BA, invalid_fetch.data(), uint32_t(invalid_fetch.size()), true);
          }
          for (uint32_t i = 0; i < dwords; ++i)
            packet[i] = std::byteswap(encoded[i]);
          cp->ExecutePacket(kPacket, dwords);
          ++native_submissions;
          Require((!vertices && !range) ||
                      gpu->register_file()->values[XE_GPU_REG_VGT_INDX_OFFSET] == first_vertex,
                  "Native vertices did not restore its starting index");
          Require(std::equal(call.fetch.begin(), call.fetch.end(),
                              gpu->register_file()->values + 0x48BA),
                  "Native draw did not preserve its fetch descriptor");
          Require(std::equal(call.constant_flush.begin(), call.constant_flush.end(),
                              gpu->register_file()->values + 0x5000),
                  "Native draw did not preserve its constant flush state");
          if (backend == "native" || backend == "compute")
            Require(cp->guest_progress_epoch() == draw_observer.epoch_after_issue,
                    "Native draw transport advanced the guest epoch beyond its backend work");
          return;
        }
        uint32_t header = xenos::MakePacketType3(opcode, uint16_t(payload.size()));
        const bool shader = native_shaders && opcode == xenos::PM4_IM_LOAD_IMMEDIATE;
        if (shader) {
          Require(payload.size() >= 3 &&
                      native_gpu_program::ValidateShader(payload[0], payload[1],
                                                         uint32_t(payload.size())) &&
                      native_gpu_program::EncodeShaderHeader(payload[1], header),
                  "Invalid native shader binding in the fixture");
        }
        auto* other_shader =
            shader ? (payload[0] ? cp->active_vertex_shader() : cp->active_pixel_shader())
                   : nullptr;
        std::vector<uint32_t> emitted = payload;
        const bool reference = native_reference && (shader || opcode == xenos::PM4_IM_LOAD);
        if (reference) {
          if (shader) {
            constexpr uint32_t kShaderSource = kPacket + 0x60000;
            auto* source = runtime.memory()->TranslatePhysical<uint32_t*>(kShaderSource);
            for (uint32_t i = 2; i < payload.size(); ++i)
              source[i - 2] = std::byteswap(payload[i]);
            emitted = {kShaderSource | payload[0], payload[1]};
          }
          Require(emitted.size() == 2 &&
                      native_gpu_shader_reference::Validate({emitted[0], emitted[1]}),
                  "Invalid native shader reference in fixture");
          header = native_gpu_shader_reference::kHeader | (predicated ? 1 : 0);
        }
        packet[0] = std::byteswap(header);
        for (size_t i = 0; i < emitted.size(); ++i)
          packet[i + 1] = std::byteswap(emitted[i]);
        const uint64_t epoch = cp->guest_progress_epoch();
        cp->ExecutePacket(kPacket, uint32_t(emitted.size() + 1));
        if (reference) {
          ++shader_references;
          predicated_shader_references += predicated;
          Require(native_gpu_shader_reference::CallCount() == shader_references &&
                      cp->guest_progress_epoch() == epoch,
                  "Native shader reference lost a binding or changed the epoch");
        }
        if (shader) {
          ++shader_submissions;
          pixel_shader_bindings += payload[0] != 0;
          auto* active = payload[0] ? cp->active_pixel_shader() : cp->active_vertex_shader();
          Require(
              active && uint32_t(active->type()) == payload[0] &&
                  active->ucode_data().size() == payload.size() - 2 &&
                  std::equal(payload.begin() + 2, payload.end(), active->ucode_data().begin()) &&
                  (payload[0] ? cp->active_vertex_shader() : cp->active_pixel_shader()) ==
                      other_shader,
              "Native binding lost its shader stage, raw code or opposite-stage binding");
          Require(
              cp->guest_progress_epoch() == epoch &&
                  native_gpu_program::ShaderCount() == (native_reference ? 0 : shader_submissions),
              "Native shader binding did not execute or changed the guest write epoch");
        }
      };
      auto submit_indirect = [&](uint32_t first, const std::vector<uint32_t>& values) {
        if (native_typed_indirect) {
          submit_typed(0, first - 0x4000, values.data(), uint32_t(values.size()));
          return;
        }
        constexpr uint32_t kSource = kPacket + 0x20000;
        const native_gpu_indirect::Call call{kSource, first - 0x4000, uint32_t(values.size())};
        Require(native_gpu_indirect::Validate(call), "Invalid indirect constants in fixture");
        auto* source = runtime.memory()->TranslatePhysical<uint32_t*>(kSource);
        std::fill_n(source, values.size(), 0x91E10DA5u);
        packet[0] = std::byteswap(native_gpu_indirect::kHeader);
        packet[1] = std::byteswap(call.address);
        packet[2] = std::byteswap(call.index);
        packet[3] = std::byteswap(call.count);
        // Update the same source address after staging every record. Reusing
        // its pointer or a previous epoch must not capture stale constants.
        for (uint32_t i = 0; i < values.size(); ++i)
          source[i] = std::byteswap(values[i]);
        const auto* registers = gpu->register_file()->values;
        const uint32_t before = registers[first - 1], after = registers[first + values.size()];
        const uint64_t epoch = cp->guest_progress_epoch();
        cp->ExecutePacket(kPacket, 4);
        ++indirect_submissions;
        indirect_words += values.size();
        Require(registers[first - 1] == before && registers[first + values.size()] == after &&
                    std::equal(values.begin(), values.end(), registers + first) &&
                    cp->guest_progress_epoch() == epoch &&
                    native_gpu_indirect::CallCount() == indirect_submissions &&
                    native_gpu_indirect::WordCount() == indirect_words,
                "Indirect constants lost source data, neighbors, callbacks, bin behavior or epoch");
      };
      auto update_constants = [&](uint32_t first, const auto& values) {
        if (!native_constants) {
          cp->RestoreRegisters(first, values.data(), uint32_t(values.size()), true);
          return;
        }
        const uint32_t aligned_first = first & ~15u;
        const uint32_t count = (first - aligned_first + uint32_t(values.size()) + 15) & ~15u;
        uint32_t header;
        Require(rex::graphics::native_gpu_constants::EncodeHeader(aligned_first, count, header),
                "Could not encode native shader constants");
        std::vector<uint32_t> expected(gpu->register_file()->values + aligned_first,
                                       gpu->register_file()->values + aligned_first + count);
        std::copy(values.begin(), values.end(), expected.begin() + (first - aligned_first));
        if (native_indirect) {
          submit_indirect(aligned_first, expected);
        } else {
          packet[0] = std::byteswap(header);
          for (uint32_t i = 0; i < count; ++i)
            packet[i + 1] = std::byteswap(expected[i]);
          cp->ExecutePacket(kPacket, count + 1);
        }
        Require(std::equal(expected.begin(), expected.end(),
                           gpu->register_file()->values + aligned_first),
                "Native shader constant submission failed to update its complete range");
        ++constant_updates;
      };
      if (native_direct) {
        execute(xenos::PM4_SET_BIN_MASK, {0, 0});
        const uint32_t control = gpu->register_file()->values[XE_GPU_REG_VGT_DRAW_INITIATOR];
        const uint32_t base = gpu->register_file()->values[XE_GPU_REG_VGT_DMA_BASE];
        const uint32_t size = gpu->register_file()->values[XE_GPU_REG_VGT_DMA_SIZE];
        const uint64_t epoch = cp->guest_progress_epoch();
        packet[0] = std::byteswap(0xC0036A01u);
        packet[1] = std::byteswap(0xA5A50100u);
        packet[2] = std::byteswap(0x00030046u);  // Unsupported source, never decoded when rejected.
        packet[3] = std::byteswap(0xDEADBEEFu);
        packet[4] = std::byteswap(0x91E10DA5u);
        cp->ExecutePacket(kPacket, 5);
        ++direct_skips;
        Require(gpu->register_file()->values[XE_GPU_REG_VGT_DRAW_INITIATOR] == control &&
                    gpu->register_file()->values[XE_GPU_REG_VGT_DMA_BASE] == base &&
                    gpu->register_file()->values[XE_GPU_REG_VGT_DMA_SIZE] == size &&
                    cp->guest_progress_epoch() == epoch && native_gpu_direct::CallCount() == 0 &&
                    native_gpu_direct::SkipCount() == direct_skips,
                "Rejected direct call decoded arguments or changed state/epoch");
        execute(xenos::PM4_SET_BIN_MASK, {UINT32_MAX, UINT32_MAX});
      }
      if (native_typed_indirect) {
        constexpr std::array<uint32_t, 5> limits = {2048, 192, 8, 32, 1024};
        for (uint32_t type = 0; type < limits.size(); ++type) {
          for (const auto [index, count] :
               {std::pair{0u, 0u}, {0u, 1u}, {0u, limits[type]}, {limits[type] - 1, 1u}}) {
            // Supply a non-null pointer for an empty range as well.
            std::vector<uint32_t> values(std::max(count, 1u));
            for (uint32_t i = 0; i < values.size(); ++i)
              values[i] = (i & 1) ? 0x80000000u : 0x7FC00123u ^ i;
            submit_typed(type, index, values.data(), count);
          }
        }
      }
      if (native_selected_ranges) {
        execute(xenos::PM4_SET_BIN_SELECT, {UINT32_MAX, UINT32_MAX});
        for (const auto [first, count] : {std::pair{0x2000u, 2u},
                                          {0x2100u, 256u},
                                          {0x2208u, 1u},
                                          {0x2180u, 2u},
                                          {0x2001u, 1u}}) {
          for (bool applied : {false, true}) {
            execute(xenos::PM4_SET_BIN_MASK,
                    {applied ? UINT32_MAX : 0u, applied ? UINT32_MAX : 0u});
            auto* registers = gpu->register_file()->values;
            std::vector<uint32_t> before(registers + first - 1, registers + first + count + 1);
            std::vector<uint32_t> values(count);
            for (uint32_t i = 0; i < count; ++i)
              values[i] = 0x91E10DA5u ^ (first + i);
            packet[0] = std::byteswap(0xC0006F05u | (count << 16));
            packet[1] = std::byteswap(0x40000u | (first - 0x2000));
            for (uint32_t i = 0; i < count; ++i)
              packet[i + 2] = std::byteswap(values[i]);
            const uint64_t epoch = cp->guest_progress_epoch();
            cp->ExecutePacket(kPacket, count + 2);
            (applied ? selected_states : skipped_states)++;
            Require(cp->guest_progress_epoch() == epoch && registers[first - 1] == before.front() &&
                        registers[first + count] == before.back() &&
                        std::equal(registers + first, registers + first + count,
                                   applied ? values.begin() : before.begin() + 1),
                    "Conditional range lost raw values, neighbors, selection or epoch");
            ++selected_ranges;
            selected_range_words += count;
          }
        }
        execute(xenos::PM4_SET_BIN_MASK, {UINT32_MAX, UINT32_MAX});
      }
      if (shader_memory_cache) {
        constexpr uint32_t kShaderSource = kPacket + 0x40000;
        auto* source = runtime.memory()->TranslatePhysical<uint32_t*>(kShaderSource);
        const auto first = Microcode(false);
        auto changed = first;
        // MAD's constant offset changes from c3 to c2, retaining valid code
        // and the same length. No GPU completion or epoch change intervenes.
        changed[8] ^= 1;
        const uint64_t epoch = cp->guest_progress_epoch();
        auto* pixel = cp->active_pixel_shader();
        for (const auto& code : {first, changed, changed, first}) {
          for (uint32_t i = 0; i < code.size(); ++i)
            source[i] = std::byteswap(code[i]);
          execute(xenos::PM4_IM_LOAD, {kShaderSource, uint32_t(code.size())},
                  native_selected_ranges);
          ++shader_memory_bindings;
          auto* active = cp->active_vertex_shader();
          Require(active && active->type() == xenos::ShaderType::kVertex &&
                      active->ucode_data() == code && cp->active_pixel_shader() == pixel &&
                      cp->guest_progress_epoch() == epoch,
                  "Memory shader cache reused stale code, changed stage/binding or advanced epoch");
        }
      }
      if (native_reference) {
        auto* vertex = cp->active_vertex_shader();
        auto* pixel = cp->active_pixel_shader();
        const uint64_t epoch = cp->guest_progress_epoch();
        execute(xenos::PM4_SET_BIN_MASK, {0, 0});
        packet[0] = std::byteswap(native_gpu_shader_reference::kHeader | 1);
        // A skipped binding must not validate/read this out-of-range source.
        packet[1] = std::byteswap(0x1FFFFFFCu);
        packet[2] = std::byteswap(0xFFFFu);
        cp->ExecutePacket(kPacket, 3);
        ++skipped_shader_references;
        Require(cp->active_vertex_shader() == vertex && cp->active_pixel_shader() == pixel &&
                    cp->guest_progress_epoch() == epoch &&
                    native_gpu_shader_reference::CallCount() == shader_references &&
                    native_gpu_shader_reference::SkipCount() == skipped_shader_references,
                "Rejected shader reference changed a binding, source state, counters or epoch");
        execute(xenos::PM4_SET_BIN_MASK, {UINT32_MAX, UINT32_MAX});
      }
      if (native_vertices) {
        execute(xenos::PM4_SET_BIN_MASK, {0, 0});
        execute(xenos::PM4_SET_BIN_SELECT, {UINT32_MAX, UINT32_MAX});
        if (native_coherency)
          submit_coherency({0x91E10DA5u, 0x03010000, 0x1800, 0x2000});
        for (uint32_t probe = 0; probe < 4; ++probe) {
          const bool range = probe >= 2, indexed = probe & 1;
          const uint32_t old_control = 0x00090086, old_base = 0x12345678, old_size = 0x87654321;
          cp->RestoreRegisters(XE_GPU_REG_VGT_DRAW_INITIATOR, &old_control, 1, true);
          cp->RestoreRegisters(XE_GPU_REG_VGT_DMA_BASE, &old_base, 1, true);
          cp->RestoreRegisters(XE_GPU_REG_VGT_DMA_SIZE, &old_size, 1, true);
          native_gpu_calls::VerticesDrawCall skipped{
              17 + probe, {1, 2, 3, 4, 5, 6}, {1, 2, 3}, 0,
              indexed ? 0x00030806u : 0x00030086u, 0x200000, 0x40000006};
          std::array<uint32_t, 18> words;
          const uint32_t dwords = range ? (indexed ? 7 : 5) : indexed ? 18 : 16;
          std::array<uint32_t, 6> expected_fetch;
          std::array<uint32_t, 3> expected_flush;
          std::copy_n(gpu->register_file()->values + 0x48BA, 6, expected_fetch.begin());
          std::copy_n(gpu->register_file()->values + 0x5000, 3, expected_flush.begin());
          if (range) {
            native_gpu_calls::RangeDrawCall call{skipped.first_vertex, 0, skipped.initiator,
                                                 skipped.index_base, skipped.index_size};
            Require(native_gpu_calls::EncodeRange(call, {words.data(), dwords}),
                    "Could not encode the rejected range");
            ++range_submissions;
            indexed_range_submissions += indexed;
            ++skipped_ranges;
          } else {
            Require(native_gpu_calls::EncodeVertices(skipped, {words.data(), dwords}),
                    "Could not encode the rejected vertices");
            expected_fetch = skipped.fetch;
            expected_flush = skipped.constant_flush;
            ++vertices_submissions;
            indexed_vertices_submissions += indexed;
            ++skipped_vertices;
          }
          for (uint32_t i = 0; i < dwords; ++i)
            packet[i] = std::byteswap(words[i]);
          const uint64_t epoch = cp->guest_progress_epoch();
          cp->ExecutePacket(kPacket, dwords);
          const auto* values = gpu->register_file()->values;
          Require(values[XE_GPU_REG_VGT_INDX_OFFSET] == skipped.first_vertex &&
                      values[XE_GPU_REG_VGT_DRAW_INITIATOR] == old_control &&
                      values[XE_GPU_REG_VGT_DMA_BASE] == old_base &&
                      values[XE_GPU_REG_VGT_DMA_SIZE] == old_size &&
                      std::equal(expected_fetch.begin(), expected_fetch.end(), values + 0x48BA) &&
                      std::equal(expected_flush.begin(), expected_flush.end(), values + 0x5000) &&
                      cp->guest_progress_epoch() == epoch,
                  "Bin rejection skipped unconditional state or changed conditional draw/index state");
          ++native_submissions;
        }
        // The auxiliary triangle builder predicates the complete bare call.
        // Its rejected form must leave even the starting vertex untouched.
        const uint32_t old_offset = gpu->register_file()->values[XE_GPU_REG_VGT_INDX_OFFSET];
        const uint32_t old_control = gpu->register_file()->values[XE_GPU_REG_VGT_DRAW_INITIATOR];
        const uint64_t epoch = cp->guest_progress_epoch();
        packet[0] = std::byteswap(0xC0000001u | (native_gpu_calls::kBareDrawOpcode << 8));
        packet[1] = std::byteswap(0x00030086u);
        cp->ExecutePacket(kPacket, 2);
        Require(gpu->register_file()->values[XE_GPU_REG_VGT_INDX_OFFSET] == old_offset &&
                    gpu->register_file()->values[XE_GPU_REG_VGT_DRAW_INITIATOR] == old_control &&
                    cp->guest_progress_epoch() == epoch,
                "A rejected bare draw changed register state or the write epoch");
        ++skipped_bare;
        execute(xenos::PM4_SET_BIN_MASK, {UINT32_MAX, UINT32_MAX});
      }
      if (native_indirect) {
        execute(xenos::PM4_SET_BIN_MASK, {0, 0});
        for (const auto [index, count] : {std::pair{1u, 1u}, {1023u, 3u}, {2047u, 1u}, {0u, 2048u}})
          for (uint32_t version = 0; version < 2; ++version) {
            std::vector<uint32_t> values(count);
            constexpr std::array<uint32_t, 4> raw = {0x7FC00123, 0x80000000, 1, 0x3F800000};
            for (uint32_t i = 0; i < count; ++i)
              values[i] = raw[i % raw.size()] ^ version;
            submit_indirect(0x4000 + index, values);
          }
        execute(xenos::PM4_SET_BIN_MASK, {UINT32_MAX, UINT32_MAX});
      }
      if (native_sync) {
        execute(xenos::PM4_SET_BIN_MASK, {0, 0});
        const uint32_t destination = gpu->register_file()->values[XE_GPU_REG_COHER_DEST_BASE_1];
        const auto words = native_gpu_sync::Encode({0x05000000, 0x1800, 0x2000});
        for (uint32_t i = 0; i < words.size(); ++i)
          packet[i] = std::byteswap(words[i]);
        const uint64_t before = cp->guest_progress_epoch();
        cp->ExecutePacket(kPacket, uint32_t(words.size()));
        ++sync_coherency;
        const auto* values = gpu->register_file()->values;
        Require(cp->guest_progress_epoch() == before &&
                    values[XE_GPU_REG_COHER_DEST_BASE_1] == destination &&
                    values[XE_GPU_REG_COHER_SIZE_HOST] == 0x1800 &&
                    values[XE_GPU_REG_COHER_BASE_HOST] == 0x2000 &&
                    values[XE_GPU_REG_COHER_STATUS_HOST] == 0 &&
                    native_gpu_sync::CoherencyCount() == sync_coherency,
                "Short coherency changed the destination, wait, write epoch or bin behavior");
        constexpr uint32_t kResult = kPacket + 0x10000;
        auto* result = runtime.memory()->TranslatePhysical<uint32_t*>(kResult);
        for (uint32_t endian = 0; endian < 4; ++endian) {
          for (uint32_t counter : {0u, 0x80000000u}) {
            std::fill_n(result, 4, 0x91E10DA5u);
            const uint32_t value = 0x12345678u ^ endian;
            const uint32_t initiator = 27 | counter;
            execute(xenos::PM4_EVENT_WRITE_SHD, {initiator, kResult + 4 + endian, value});
            packet[0] = std::byteswap(native_gpu_sync::kEventHeader);
            packet[1] = std::byteswap(initiator);
            packet[2] = std::byteswap(kResult + 8 + endian);
            packet[3] = std::byteswap(value);
            const uint64_t epoch = cp->guest_progress_epoch();
            cp->ExecutePacket(kPacket, 4);
            ++sync_events;
            Require(result[0] == 0x91E10DA5u && result[3] == 0x91E10DA5u &&
                        result[1] == result[2] &&
                        (counter ||
                         result[2] == xenos::GpuSwap(value, static_cast<xenos::Endian>(endian))) &&
                        values[XE_GPU_REG_VGT_EVENT_INITIATOR] == 27 &&
                        cp->guest_progress_epoch() == epoch + 1 &&
                        native_gpu_sync::EventCount() == sync_events,
                    "Shader event lost its completion, endian mode, counter, neighbors or epoch");
          }
        }
        execute(xenos::PM4_SET_BIN_MASK, {UINT32_MAX, UINT32_MAX});
      }
      if (native_ports) {
        execute(xenos::PM4_SET_BIN_MASK, {0, 0});
        const auto port = [&](uint32_t header, std::initializer_list<uint32_t> values) {
          uint32_t reg;
          Require(native_gpu_ports::DecodeHeader(header, uint32_t(values.size()), reg),
                  "Could not encode native initialization port");
          std::array<uint32_t, 8> neighbors;
          std::copy_n(gpu->register_file()->values + reg + 1, neighbors.size(), neighbors.begin());
          packet[0] = std::byteswap(header);
          uint32_t i = 1;
          for (uint32_t value : values)
            packet[i++] = std::byteswap(value);
          const uint64_t epoch = cp->guest_progress_epoch();
          cp->ExecutePacket(kPacket, uint32_t(values.size() + 1));
          ++port_submissions;
          port_writes += values.size();
          Require(gpu->register_file()->values[reg] == *(values.end() - 1) &&
                      std::equal(neighbors.begin(), neighbors.end(),
                                 gpu->register_file()->values + reg + 1) &&
                      cp->guest_progress_epoch() == epoch &&
                      native_gpu_ports::CallCount() == port_submissions &&
                      native_gpu_ports::WriteCount() == port_writes,
                  "Initialization port lost callbacks, repetition, neighbors or write epoch");
        };
        port(native_gpu_ports::kWaitHeader, {0x20000});
        port(native_gpu_ports::kGprEightHeader, {1, 3, 5, 7, 9, 11, 13, 15});
        port(native_gpu_ports::kGprHeader, {0x12340});
        execute(xenos::PM4_SET_BIN_MASK, {UINT32_MAX, UINT32_MAX});
      }
      if (native_predication) {
        // Check each half independently, including preservation while the
        // other half changes. Every supported register is tested each time.
        const auto probe = [&](bool applied) {
          for (uint32_t operand : {0x40000u, 0x40104u, 0x40203u})
            selected_state(operand, 0x91E10DA5u ^ operand ^ uint32_t(selected_states), applied);
        };
        execute(xenos::PM4_SET_BIN_SELECT, {1, 0});
        mask_half(true, 1);
        mask_half(false, 0);
        probe(true);
        mask_half(false, 1);
        probe(true);
        mask_half(true, 0);
        probe(false);
        execute(xenos::PM4_SET_BIN_SELECT, {0, 1});
        probe(true);
        mask_half(true, 1);
        probe(true);
        mask_half(false, 0);
        probe(false);
        mask_half(false, UINT32_MAX);
        mask_half(true, UINT32_MAX);
        execute(xenos::PM4_SET_BIN_SELECT, {UINT32_MAX, UINT32_MAX});
      }
      if (native_shaders) {
        // Neither binding nor invalidation is gated by draw-bin rejection.
        execute(xenos::PM4_SET_BIN_MASK, {0, 0});
        packet[0] = std::byteswap(native_gpu_program::kInvalidateHeader);
        packet[1] = std::byteswap(0x300u);
        const uint64_t epoch = cp->guest_progress_epoch();
        cp->ExecutePacket(kPacket, 2);
        ++invalidation_submissions;
        Require(cp->guest_progress_epoch() == epoch &&
                    native_gpu_program::InvalidationCount() == invalidation_submissions,
                "Native invalidation was skipped or changed the guest write epoch");
        execute(xenos::PM4_SET_BIN_MASK, {UINT32_MAX, UINT32_MAX});
      }
      if (native_state) {
        // Partial runs exercise the header's high offset bit and page ends;
        // the real draw snapshots below replace this state before rendering.
        for (const auto& [first, count] :
             {std::pair{0x207Fu, 2u}, std::pair{0x2180u, 1u}, std::pair{0x22FFu, 1u},
              std::pair{0x23C0u, 16u}, std::pair{0x48BAu, 6u}, std::pair{0x4920u, 8u},
              std::pair{0x5000u, 3u}}) {
          std::array<uint32_t, 16> values;
          for (uint32_t i = 0; i < count; ++i)
            values[i] = first == 0x5000 ? (i == 1 ? 0x25000u : 0u) : 0x7FC00001u + i;
          submit_state(first, values.data(), count);
        }
      }
      IndexConstants sc;
      std::array<float, 24> fc{};
      Buffer shared{runtime.memory()->TranslatePhysical(0)};
      Buffer system{&sc};
      Buffer constants{fc.data()};
      namespace reg = rex::graphics::reg;
      using namespace rex::graphics;
      for (bool fetch : {false, true}) {
        if (large_inputs && !fetch)
          continue;
        for (uint32_t address_case = 0; address_case < (large_inputs                ? 2u
                                                        : fetch && affine_addresses ? 14u
                                                                                    : 1u);
             ++address_case) {
          if (large_inputs && !address_case)
            continue;
          for (auto primitive :
               {xenos::PrimitiveType::kPointList, xenos::PrimitiveType::kLineStrip,
                xenos::PrimitiveType::kLineLoop, xenos::PrimitiveType::kRectangleList,
                xenos::PrimitiveType::kQuadList, xenos::PrimitiveType::kTriangleStrip,
                xenos::PrimitiveType::kTriangleFan}) {
            if (address_case && primitive != xenos::PrimitiveType::kPointList &&
                primitive != xenos::PrimitiveType::kRectangleList)
              continue;
            for (uint32_t bits : {0u, 16u, 32u}) {
              Case test{primitive == xenos::PrimitiveType::kQuadList ? 8u : 9u,
                        bits,
                        bits == 32   ? 2u
                        : bits == 16 ? 1u
                                     : 0u,
                        0,
                        -3,
                        bits != 0 && (primitive == xenos::PrimitiveType::kLineStrip ||
                                      primitive == xenos::PrimitiveType::kTriangleStrip ||
                                      primitive == xenos::PrimitiveType::kTriangleFan),
                        false,
                        fetch};
              test.vertex_stride = vertex_stride;
              if (address_case) {
                const std::array offsets = {0.25f,   16.25f, -128.0f, 16.25f, -64.0f,  0.1f, 0.1f,
                                            -128.0f, -64.0f, -0.1f,   -64.0f, -128.0f, -4.0f};
                const std::array scales = {3.0f, 0.75f, -0.75f, 0.0f,  -1.5f,
                                           0.3f, 1.1f,  -0.3f,  -1.1f, std::nextafter(1.0f, 2.0f),
                                           1.0f, 0.3f,  0.3f};
                test.affine = true;
                test.rounded = address_case == 2 || address_case == 4 || address_case == 5 ||
                               address_case == 7 || address_case >= 9;
                test.offset = offsets[address_case - 1];
                test.scale = scales[address_case - 1];
              }
              if (large_inputs) {
                test.affine = test.large_input = true;
                test.rounded = false;
                test.offset = 4.0f;
                test.scale = 2048.0f;
              }
              bytes += Run(
                  shared, system, constants, test,
                  [&](uint32_t count) {
                    const uint32_t vertex_group = native_calls ? 31u : 1u;
                    auto code = Microcode(test.fetch, test.affine, test.rounded, vertex_group,
                                          test.vertex_stride);
                    std::vector<uint32_t> load = {uint32_t(xenos::ShaderType::kVertex),
                                                  uint32_t(code.size())};
                    load.insert(load.end(), code.begin(), code.end());
                    const bool reject_shader_bin = native_shaders && !shader_submissions;
                    if (reject_shader_bin)
                      execute(xenos::PM4_SET_BIN_MASK, {0, 0});
                    execute(xenos::PM4_IM_LOAD_IMMEDIATE, load);
                    if (reject_shader_bin)
                      execute(xenos::PM4_SET_BIN_MASK, {UINT32_MAX, UINT32_MAX});
                    std::vector<uint32_t> registers(RegisterFile::kRegisterCount, 0);
                    std::memcpy(registers.data() + XE_GPU_REG_SHADER_CONSTANT_000_X, fc.data(),
                                sizeof(fc));
                    reg::SQ_PROGRAM_CNTL program{};
                    program.vs_num_reg = test.affine ? 3 : test.fetch ? 2 : 1;
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
                    if (test.fetch) {
                      xenos::xe_gpu_vertex_fetch_t vertex{};
                      vertex.type = xenos::FetchConstantType::kVertex;
                      vertex.address = (1536 * 1024) >> 2;
                      vertex.size = 512 * (test.vertex_stride / 4);
                      std::memcpy(registers.data() + XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 +
                                      vertex_group * 6,
                                  &vertex, sizeof(vertex));
                    }
                    restore_snapshot(registers);
                    if (native_coherency)
                      submit_coherency(
                          {registers[XE_GPU_REG_COHER_DEST_BASE_1], 0x10000, 0, 0x1000});
                    cp->TracePlaybackWroteMemory(0, (test.large_input ? 8 : 2) * 1024 * 1024);
                    auto produce_indices = [&] {
                      // A fetched float export preserves the original raw bits,
                      // including 16-bit half-word alignment and DMA endianness.
                      auto producer_code = Microcode(true, false, false, vertex_group);
                      std::vector<uint32_t> producer_load = {uint32_t(xenos::ShaderType::kVertex),
                                                             uint32_t(producer_code.size())};
                      producer_load.insert(producer_load.end(), producer_code.begin(),
                                           producer_code.end());
                      execute(xenos::PM4_IM_LOAD_IMMEDIATE, producer_load);
                      auto producer_registers = registers;
                      std::array<float, 20> producer_constants{};
                      producer_constants[1] = 1;
                      auto* stream = reinterpret_cast<uint32_t*>(producer_constants.data() + 4);
                      stream[0] = 0x40000000 | ((sc.vertex_index_load_address & ~3u) >> 2);
                      stream[1] = 0x4B000000;
                      stream[2] =
                          0x4B000000 | (uint32_t(xenos::ColorFormat::k_32_32_32_32_FLOAT) << 8);
                      stream[3] = 0x4B000000 | 4;
                      std::memcpy(producer_registers.data() + XE_GPU_REG_SHADER_CONSTANT_000_X,
                                  producer_constants.data(), sizeof(producer_constants));
                      reg::SQ_PROGRAM_CNTL producer_program{};
                      producer_program.vs_num_reg = 2;
                      producer_registers[XE_GPU_REG_SQ_PROGRAM_CNTL] = producer_program.value;
                      producer_registers[XE_GPU_REG_VGT_INDX_OFFSET] = 0;
                      producer_registers[XE_GPU_REG_VGT_MIN_VTX_INDX] = 0;
                      producer_registers[XE_GPU_REG_VGT_MAX_VTX_INDX] = 65535;
                      xenos::xe_gpu_vertex_fetch_t input{};
                      input.type = xenos::FetchConstantType::kVertex;
                      input.address = (test.large_input ? 6 * 1024 * 1024 : 1792 * 1024) >> 2;
                      input.size = 16;
                      std::memcpy(
                          producer_registers.data() + XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 +
                              vertex_group * 6,
                          &input, sizeof(input));
                      restore_snapshot(producer_registers);
                      set("readback_memexport", "false");
                      if (backend == "native" || backend == "compute")
                        set("vulkan_readback_memexport", "false");
                      if (backend == "dx12" || backend == "dx12-bindful")
                        set("d3d12_readback_memexport", "false");
                      reg::VGT_DRAW_INITIATOR producer{};
                      producer.prim_type = xenos::PrimitiveType::kTriangleList;
                      producer.num_indices = 4;
                      producer.source_select = xenos::SourceSelect::kAutoIndex;
                      execute(xenos::PM4_DRAW_INDX_2, {producer.value});
                      restore_snapshot(registers);
                      execute(xenos::PM4_IM_LOAD_IMMEDIATE, load);
                      set("readback_memexport", "true");
                      if (backend == "native" || backend == "compute")
                        set("vulkan_readback_memexport", "true");
                      if (backend == "dx12" || backend == "dx12-bindful")
                        set("d3d12_readback_memexport", "true");
                    };
                    if (gpu_indices && bits) {
                      produce_indices();
                      if (clear_caches) {
                        cp->ClearCaches();
                        cp->IssueSwap(128 * 1024, 64, 32);
                        ++cache_clears;
                      }
                    }
                    reg::VGT_DRAW_INITIATOR initiator{};
                    initiator.major_mode =
                        explicit_major ? xenos::MajorMode::kExplicit : xenos::MajorMode::kImplicit;
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
                    // Change only the output constants between two draws in one
                    // frame. A stale/omitted second draw cannot match the CPU oracle;
                    // converted indices must retain their shadow on the cache hit.
                    std::array<uint32_t, 4> first_constants;
                    uint32_t bank = XE_GPU_REG_SHADER_CONSTANT_000_X + (test.fetch ? 16 : 12);
                    for (uint32_t component = 0; component < 4; ++component) {
                      float value = test.fetch ? 1.0f : fc[12 + component] + 1.0f;
                      first_constants[component] = std::bit_cast<uint32_t>(value);
                    }
                    if (native_constants) {
                      auto sentinel = first_constants;
                      for (auto& component : sentinel)
                        component = std::bit_cast<uint32_t>(std::bit_cast<float>(component) + 123.f);
                      cp->RestoreRegisters(bank, sentinel.data(), 4, true);
                    }
                    update_constants(bank, first_constants);
                    execute(xenos::PM4_DRAW_INDX_2, draw);
                    std::array<uint32_t, 4> final_constants;
                    std::memcpy(final_constants.data(), fc.data() + (test.fetch ? 16 : 12), 16);
                    update_constants(bank, final_constants);
                    execute(xenos::PM4_DRAW_INDX_2, draw);
                    if (gpu_indices && bits) {
                      const uint32_t kProducerInput =
                          test.large_input ? 6 * 1024 * 1024 : 1792 * 1024;
                      const uint32_t prefix = sc.vertex_index_load_address & 3u;
                      uint32_t changed = 17 | (bits == 32 ? 0x40000000 : 0);
                      changed = Swap(changed, test.endian);
                      std::memcpy(
                          runtime.memory()->TranslatePhysical<uint8_t*>(kProducerInput + prefix),
                          &changed, bits / 8);
                      cp->TracePlaybackWroteMemory(kProducerInput, 64);
                      produce_indices();
                      execute(xenos::PM4_DRAW_INDX_2, draw);
                      auto* cpu_indices = runtime.memory()->TranslatePhysical<uint8_t*>(
                          sc.vertex_index_load_address & ~3u);
                      Require(std::all_of(cpu_indices, cpu_indices + 64,
                                          [](uint8_t byte) { return byte == 0xA7; }),
                              "Native index proof published GPU bytes into stale CPU memory");
                      // CPU ownership replaces the complete stream. A cached GPU
                      // proof must not survive this write or its invalidation.
                      std::memcpy(cpu_indices, runtime.memory()->TranslatePhysical(kProducerInput),
                                  64);
                      changed = Swap(21 | (bits == 32 ? 0x40000000 : 0), test.endian);
                      std::memcpy(cpu_indices + prefix, &changed, bits / 8);
                      cp->TracePlaybackWroteMemory(sc.vertex_index_load_address & ~3u, 64);
                      execute(xenos::PM4_DRAW_INDX_2, draw);
                    }
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
                  },
                  gpu_indices && bits);
              ++cases;
            }
          }
        }
      }
      if (native_ports) {
        // Bind the same bytes under the other shader stage after rendering.
        // This proves stage/cache separation without changing pixel execution
        // in this memexport fixture, which is qualified separately above.
        const auto code = Microcode(false);
        std::vector<uint32_t> binding = {1, uint32_t(code.size())};
        binding.insert(binding.end(), code.begin(), code.end());
        execute(xenos::PM4_SET_BIN_MASK, {0, 0});
        execute(xenos::PM4_IM_LOAD_IMMEDIATE, binding);
      }
      finished.set_value();
    } catch (...) {
      finished.set_exception(std::current_exception());
    }
  });
  completion.get();
  if (backend == "native" || backend == "compute")
    Require(draw_observer.explicit_draws == requested_explicit_draws,
            "The requested explicit major mode did not reach every GPU draw");
  runtime.Shutdown();
  Require(rex::graphics::native_gpu_state::CallCount() == state_submissions &&
              rex::graphics::native_gpu_state::RegisterCount() == state_registers,
          "Native state runtime counters differ from submitted records");
  Require(rex::graphics::native_gpu_coherency::CallCount() == coherency_submissions,
          "Native coherency runtime counters differ from submitted records");
  Require(rex::graphics::native_gpu_program::ShaderCount() ==
                  (native_reference ? 0 : shader_submissions) &&
              rex::graphics::native_gpu_program::InvalidationCount() == invalidation_submissions,
          "Native program runtime counters differ from submitted records");
  Require(rex::graphics::native_gpu_predication::MaskCount() == mask_submissions &&
              rex::graphics::native_gpu_predication::StateCount() == selected_states &&
              rex::graphics::native_gpu_predication::SkippedStateCount() == skipped_states,
          "Native predication runtime counters differ from submitted records");
  Require(rex::graphics::native_gpu_ports::CallCount() == port_submissions &&
              rex::graphics::native_gpu_ports::WriteCount() == port_writes,
          "Native initialization port runtime counters differ from submitted records");
  Require(rex::graphics::native_gpu_sync::CoherencyCount() == sync_coherency &&
              rex::graphics::native_gpu_sync::EventCount() == sync_events,
          "Native synchronization runtime counters differ from submitted records");
  Require(rex::graphics::native_gpu_indirect::CallCount() == indirect_submissions &&
              rex::graphics::native_gpu_indirect::WordCount() == indirect_words &&
              rex::graphics::native_gpu_indirect::TypedCallCount() == typed_indirect_submissions,
          "Native indirect-constant runtime counters differ from submitted records");
  Require(rex::graphics::native_gpu_shader_reference::CallCount() == shader_references &&
              rex::graphics::native_gpu_shader_reference::SkipCount() == skipped_shader_references,
          "Native shader-reference counters differ from submitted records");
  Require(rex::graphics::native_gpu_direct::CallCount() == direct_draws &&
              rex::graphics::native_gpu_direct::SkipCount() == direct_skips,
          "Native direct-draw counters differ from submitted records");
  std::string report = "{\n  \"passed\": true,\n  \"scope\": \"production command processor\",\n";
  report +=
      "  \"cases\": " + std::to_string(cases) +
      ",\n  \"vertex_stride\": " + std::to_string(vertex_stride) +
      ",\n  \"explicit_major\": " + std::string(explicit_major ? "true" : "false") +
      ",\n  \"requested_explicit_draws\": " + std::to_string(requested_explicit_draws) +
      ",\n  \"native_coherency_calls\": " + std::to_string(coherency_submissions) +
      ",\n  \"native_sync_coherency_calls\": " + std::to_string(sync_coherency) +
      ",\n  \"native_shader_events\": " + std::to_string(sync_events) +
      ",\n  \"native_indirect_calls\": " + std::to_string(indirect_submissions) +
      ",\n  \"native_indirect_words\": " + std::to_string(indirect_words) +
      ",\n  \"native_typed_indirect_calls\": " + std::to_string(typed_indirect_submissions) +
      ",\n  \"typed_indirect_types\": " + std::to_string(typed_indirect_types) +
      ",\n  \"typed_restore_calls\": " + std::to_string(typed_restore_calls) +
      ",\n  \"direct_draws\": " + std::to_string(direct_draws) +
      ",\n  \"direct_indexed\": " + std::to_string(direct_indexed) +
      ",\n  \"direct_queries\": " + std::to_string(direct_queries) +
      ",\n  \"direct_padded_auto\": " + std::to_string(direct_padded_auto) +
      ",\n  \"direct_skips\": " + std::to_string(direct_skips) +
      ",\n  \"shader_memory_bindings\": " + std::to_string(shader_memory_bindings) +
      ",\n  \"native_shader_references\": " + std::to_string(shader_references) +
      ",\n  \"skipped_shader_references\": " + std::to_string(skipped_shader_references) +
      ",\n  \"predicated_shader_references\": " + std::to_string(predicated_shader_references) +
      ",\n  \"conditional_ranges\": " + std::to_string(selected_ranges) +
      ",\n  \"conditional_range_words\": " + std::to_string(selected_range_words) +
      ",\n  \"native_shader_calls\": " + std::to_string(shader_submissions) +
      ",\n  \"native_invalidation_calls\": " + std::to_string(invalidation_submissions) +
      ",\n  \"native_mask_calls\": " + std::to_string(mask_submissions) +
      ",\n  \"native_selected_states\": " + std::to_string(selected_states) +
      ",\n  \"native_skipped_states\": " + std::to_string(skipped_states) +
      ",\n  \"restored_snapshots\": " + std::to_string(restored_snapshots) +
      ",\n  \"native_port_calls\": " + std::to_string(port_submissions) +
      ",\n  \"native_port_writes\": " + std::to_string(port_writes) +
      ",\n  \"native_pixel_shader_bindings\": " + std::to_string(pixel_shader_bindings) +
      ",\n  \"observed_explicit_draws\": " + std::to_string(draw_observer.explicit_draws) +
      ",\n  \"exact_bytes\": " + std::to_string(bytes) +
      ",\n  \"presented_frames\": " + std::to_string(cases + cache_clears) +
      ",\n  \"cache_clears\": " + std::to_string(cache_clears) + ",\n";
  report += "  \"native_draw_calls\": " + std::string(native_calls ? "true" : "false") +
                    ",\n  \"native_state_calls\": " + std::to_string(state_submissions) +
                    ",\n  \"native_state_registers\": " + std::to_string(state_registers) +
                    ",\n  \"native_constant_updates\": " + std::to_string(constant_updates) +
                    ",\n  \"native_submissions\": " + std::to_string(native_submissions) +
                    ",\n  \"bare_submissions\": " + std::to_string(bare_submissions) +
                    ",\n  \"vertices_submissions\": " + std::to_string(vertices_submissions) +
                    ",\n  \"indexed_vertices_submissions\": " + std::to_string(indexed_vertices_submissions) +
                    ",\n  \"range_submissions\": " + std::to_string(range_submissions) +
                    ",\n  \"indexed_range_submissions\": " + std::to_string(indexed_range_submissions) +
                    ",\n  \"bin_skipped_ranges\": " + std::to_string(skipped_ranges) +
                    ",\n  \"bin_skipped_bare\": " + std::to_string(skipped_bare) +
                    ",\n  \"bin_skipped_vertices\": " + std::to_string(skipped_vertices) +
                    ",\n  \"observed_draws\": " + std::to_string(draw_observer.draws) +
                    ",\n  \"observed_indexed_draws\": " + std::to_string(draw_observer.indexed) +
                    ",\n  \"observed_halfword_offsets\": " +
                    std::to_string(draw_observer.halfword_offsets) + "\n}\n";
  std::ofstream file(output / "command-processor-report.json");
  file << report;
  Require(bool(file), "Could not write command processor report");
  std::fputs(report.c_str(), stdout);
}
}  // namespace
int main(int argc, char** argv) {
  try {
    Require(argc >= 3 && argc <= 27,
            "usage: native_geometry_check output native|compute|dx11|dx12|dx12-bindful "
            "[--gpu-indices] [--affine-addresses] [--buffer-budget-mb N] [--clear-caches] "
            "[--large-inputs] [--native-calls] [--native-constants] [--native-vertices] "
            "[--native-state] "
            "[--explicit-major] [--native-coherency] [--native-shaders] [--native-predication] "
            "[--native-ports] [--native-sync] [--native-indirect] [--shader-memory-cache] "
            "[--native-reference] [--native-selected-ranges] [--native-typed-indirect] "
            "[--native-direct] [--vertex-stride bytes]");
    bool gpu_indices = false, affine_addresses = false, clear_caches = false, large_inputs = false;
    bool native_calls = false;
    bool native_constants = false;
    bool native_vertices = false;
    bool native_state = false;
    bool explicit_major = false;
    bool native_coherency = false;
    bool native_shaders = false;
    bool native_predication = false;
    bool native_ports = false;
    bool native_sync = false;
    bool native_indirect = false;
    bool shader_memory_cache = false;
    bool native_reference = false;
    bool native_selected_ranges = false;
    bool native_typed_indirect = false;
    bool native_direct = false;
    uint32_t vertex_stride = 16;
    bool stride_set = false;
    uint32_t buffer_budget = 0;
    for (int i = 3; i < argc; ++i) {
      if (std::string(argv[i]) == "--gpu-indices" && !gpu_indices)
        gpu_indices = true;
      else if (std::string(argv[i]) == "--affine-addresses" && !affine_addresses)
        affine_addresses = true;
      else if (std::string(argv[i]) == "--large-inputs" && !large_inputs)
        large_inputs = true;
      else if (std::string(argv[i]) == "--clear-caches" && !clear_caches)
        clear_caches = true;
      else if (std::string(argv[i]) == "--native-calls" && !native_calls)
        native_calls = true;
      else if (std::string(argv[i]) == "--native-constants" && !native_constants)
        native_constants = true;
      else if (std::string(argv[i]) == "--native-vertices" && !native_vertices)
        native_vertices = true;
      else if (std::string(argv[i]) == "--native-state" && !native_state)
        native_state = true;
      else if (std::string(argv[i]) == "--explicit-major" && !explicit_major)
        explicit_major = true;
      else if (std::string(argv[i]) == "--native-coherency" && !native_coherency)
        native_coherency = true;
      else if (std::string(argv[i]) == "--native-shaders" && !native_shaders)
        native_shaders = true;
      else if (std::string(argv[i]) == "--native-predication" && !native_predication)
        native_predication = true;
      else if (std::string(argv[i]) == "--native-ports" && !native_ports)
        native_ports = true;
      else if (std::string(argv[i]) == "--native-sync" && !native_sync)
        native_sync = true;
      else if (std::string(argv[i]) == "--native-indirect" && !native_indirect)
        native_indirect = true;
      else if (std::string(argv[i]) == "--shader-memory-cache" && !shader_memory_cache)
        shader_memory_cache = true;
      else if (std::string(argv[i]) == "--native-reference" && !native_reference)
        native_reference = true;
      else if (std::string(argv[i]) == "--native-selected-ranges" && !native_selected_ranges)
        native_selected_ranges = true;
      else if (std::string(argv[i]) == "--native-typed-indirect" && !native_typed_indirect)
        native_typed_indirect = true;
      else if (std::string(argv[i]) == "--native-direct" && !native_direct)
        native_direct = true;
      else if (std::string(argv[i]) == "--vertex-stride" && !stride_set && i + 1 < argc) {
        size_t consumed = 0;
        auto value = std::stoul(argv[++i], &consumed);
        Require(consumed == std::strlen(argv[i]) && value >= 16 && value <= 64 && !(value & 3),
                "Vertex stride must be a multiple of four from 16 to 64 bytes");
        vertex_stride = uint32_t(value);
        stride_set = true;
      } else if (std::string(argv[i]) == "--buffer-budget-mb" && !buffer_budget && i + 1 < argc) {
        size_t consumed = 0;
        auto value = std::stoul(argv[++i], &consumed);
        Require(consumed == std::strlen(argv[i]) && value >= 1 && value <= 512,
                "Buffer budget must be between 1 and 512 MiB");
        buffer_budget = uint32_t(value);
      } else
        throw std::runtime_error("Unknown or duplicate fixture argument");
    }
    Require(!clear_caches || gpu_indices, "Cache-clear checks require GPU-owned indices");
    Require(!native_vertices || native_calls, "Native vertices checks require native calls");
    Require(!native_ports || native_shaders,
            "Initialization checks require native shader bindings");
    Require(!native_indirect || native_constants,
            "Indirect-constant checks require native constants");
    Require(!native_reference || (native_shaders && shader_memory_cache),
            "Shader-reference checks require native shaders and memory-cache probes");
    Require(!native_selected_ranges || (native_predication && native_reference),
            "Conditional-range checks require native predication and shader references");
    Require(!native_typed_indirect || (native_indirect && native_state),
            "Typed-constant checks require native indirect constants and native state");
    Require(!native_direct || (native_calls && native_vertices),
            "Direct-draw checks require the native call and vertices probes");
    Require(!large_inputs || vertex_stride == 16,
            "Large input fixtures use a 16-byte vertex stride");
    std::filesystem::path output(argv[1]);
    std::filesystem::create_directories(output);
    auto log = (output / "runtime.log").string();
    rex::InitLogging(log.c_str(), spdlog::level::info);

    RunCommandProcessor(output, argv[2], gpu_indices, affine_addresses, buffer_budget, clear_caches,
                        large_inputs, native_calls, native_constants, native_vertices, native_state,
                        vertex_stride, explicit_major, native_coherency, native_shaders,
                        native_predication, native_ports, native_sync, native_indirect,
                        shader_memory_cache, native_reference, native_selected_ranges,
                        native_typed_indirect, native_direct);
    return 0;
  } catch (const std::exception& e) {
    if (argc > 1) {
      std::ofstream(std::filesystem::path(argv[1]) / "failure.txt") << e.what();
    }
    return 1;
  }
}
