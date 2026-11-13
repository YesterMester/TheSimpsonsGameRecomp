// Native frame replayer, v1: renders a captured GPU frame trace through the
// ahead-of-time compiled shaders with no emulated backend in the loop.
//
// The trace supplies everything: guest memory (vertex/index data - the
// translated shaders fetch vertices themselves from the shared memory buffer
// using the fetch constants, so there is no vertex attribute plumbing at
// all), the register state per draw, and the shader ucode identities. This
// uploads guest memory once, slices the per-draw constant buffers straight
// out of the register shadow, builds pipelines from the register state, and
// plays every draw into one render target, written out as a PPM.
//
// Deliberate v1 simplifications, each a later work item: a single 1280x720
// color target regardless of EDRAM layout changes, dummy white textures in
// place of the real texture cache, rect/quad-list primitives skipped (they
// need the geometry expansion stage), and a minimal system-constants fill.
// The point of v1 is the whole path holding together end to end - real
// draws, real shaders, real state, real geometry.

#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <rex/graphics/native_gpu_calls.h>
#include <rex/graphics/native_gpu_direct.h>
#include <rex/graphics/native_gpu_program.h>
#include <rex/graphics/native_gpu_shader_reference.h>
#include <rex/graphics/packet_state.h>
#include <rex/graphics/packet_disassembler.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/pipeline/shader/spirv_translator.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/util/draw.h>
#include <rex/graphics/trace_protocol.h>
#include <snappy.h>
#define XXH_INLINE_ALL
#include <xxhash.h>

#define VK_NO_PROTOTYPES
#include <volk.h>

namespace rex::graphics::nativegs {
enum class PipelineGeometryShader : uint32_t { kNone, kPointList, kRectangleList, kQuadList };
union GeometryShaderKey {
  uint32_t key;
  struct {
    PipelineGeometryShader type : 2;
    uint32_t interpolator_count : 5;
    uint32_t user_clip_plane_count : 3;
    uint32_t user_clip_plane_cull : 1;
    uint32_t has_vertex_kill_and : 1;
    uint32_t has_point_size : 1;
    uint32_t has_point_coordinates : 1;
    uint32_t point_ps_ucp_mode : 2;
  };
  GeometryShaderKey() : key(0) {}
};
std::vector<unsigned int> GenerateGeometryShader(GeometryShaderKey key);
}  // namespace rex::graphics::nativegs

namespace fs = std::filesystem;
using json = nlohmann::json;
using namespace rex::graphics;

namespace {

constexpr uint32_t kRegisterCount = 0x5004;
constexpr uint32_t kGuestSpace = 512u << 20;
constexpr uint32_t kWidth = 1280, kHeight = 720;

// Xenos register file layout for the shader constant blocks.
constexpr uint32_t kRegFloatConstants = 0x4000;  // 512 vec4: VS 0-255, PS 256-511
constexpr uint32_t kRegFetchConstants = 0x4800;  // 32 x 6 dwords
constexpr uint32_t kRegBoolLoop = 0x4900;        // 8 bool dwords + 32 loop dwords

struct DrawCall {
  uint64_t vs_hash, ps_hash;
  uint32_t initiator;         // VGT_DRAW_INITIATOR value
  uint32_t index_base = 0;    // guest address of indices (DMA draws)
  uint32_t index_size = 0;    // dword with endianness in low bits
  std::vector<uint32_t> regs; // full register snapshot
};

std::vector<uint32_t> ReadWords(const fs::path& p) {
  std::ifstream f(p, std::ios::binary | std::ios::ate);
  if (!f) return {};
  std::vector<uint32_t> w(size_t(f.tellg()) / 4);
  f.seekg(0);
  f.read(reinterpret_cast<char*>(w.data()), std::streamsize(w.size() * 4));
  return w;
}

uint32_t be32(const uint8_t* q) {
  return uint32_t(q[0]) << 24 | uint32_t(q[1]) << 16 | uint32_t(q[2]) << 8 | uint32_t(q[3]);
}

#define CHECK_VK(expr)                                                                  \
  do {                                                                                  \
    VkResult r_ = (expr);                                                               \
    if (r_ != VK_SUCCESS) {                                                             \
      std::fprintf(stderr, "%s failed: %d (line %d)\n", #expr, int(r_), __LINE__);      \
      return 1;                                                                         \
    }                                                                                   \
  } while (0)

uint32_t FindMemoryType(VkPhysicalDevice gpu, uint32_t type_bits, VkMemoryPropertyFlags props) {
  VkPhysicalDeviceMemoryProperties mp;
  vkGetPhysicalDeviceMemoryProperties(gpu, &mp);
  for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
    if ((type_bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & props) == props) return i;
  }
  return UINT32_MAX;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 4) {
    std::fprintf(stderr, "usage: trace_replay <trace> <spv-dir> <out.ppm>\n");
    return 2;
  }

  // ---------------------------------------------------------------- trace
  std::ifstream tf(argv[1], std::ios::binary | std::ios::ate);
  if (!tf) { std::fprintf(stderr, "cannot open trace\n"); return 1; }
  std::vector<uint8_t> data(size_t(tf.tellg()));
  tf.seekg(0);
  tf.read(reinterpret_cast<char*>(data.data()), std::streamsize(data.size()));
  const uint8_t* p = data.data() + sizeof(TraceHeader);
  const uint8_t* end = data.data() + data.size();

  std::vector<uint8_t> guest(kGuestSpace, 0);
  PacketDisassembler::SetMemoryBase(guest.data(), kGuestSpace);
  std::vector<uint32_t> regs(kRegisterCount, 0);
  PacketState packet_state;
  uint64_t vs_hash = 0, ps_hash = 0;
  std::vector<DrawCall> draws;

  // A packet's memory reads are recorded AFTER the packet command in the
  // stream, so packets are processed DEFERRED - when the next PacketStart
  // arrives, every memory record belonging to the previous packet has been
  // applied and memory-indirect packets (LOAD_ALU_CONSTANT, IM_LOAD hashing)
  // read exactly the state the runtime's command processor saw.
  const uint8_t* pending_pkt = nullptr;
  uint32_t pending_count = 0;
  auto process_packet = [&](const uint8_t* pkt, uint32_t count) {
    // A native call carries state and the draw together. Apply its actions
    // before taking the draw snapshot, just as the runtime consumer does.
    PacketInfo info;
    bool decoded = PacketDisassembler::DisasmPacket(pkt, &info);
    uint32_t head = be32(pkt);
    PacketState::Result applied;
    if (decoded) {
      applied = packet_state.Apply(info, head, regs);
      if (!applied.executed)
        return;
    }
    if ((head >> 30) == 3) {
      uint32_t opcode = (head >> 8) & 0x7F;
      if ((opcode == 0x27 || (decoded && opcode == native_gpu_shader_reference::kOpcode)) &&
          count >= 3) {  // IM_LOAD
        uint32_t addr_type = be32(pkt + 4);
        uint32_t size_dwords = be32(pkt + 8) & 0xFFFF;
        uint32_t addr = (addr_type & ~0x3u) & (kGuestSpace - 1);
        uint64_t h = XXH3_64bits(guest.data() + addr, size_dwords * 4);
        if ((addr_type & 3) == 0) vs_hash = h; else ps_hash = h;
      } else if (count >= 4 && (opcode == 0x2B ||
                                (decoded && native_gpu_program::IsShaderHeader(head, count - 1)))) {
        uint32_t st = be32(pkt + 4);
        uint32_t size_dwords = be32(pkt + 8) & 0xFFFF;
        uint64_t h = XXH3_64bits(pkt + 12, size_dwords * 4);
        if (st == 0) vs_hash = h; else ps_hash = h;
      } else if (opcode == 0x22 && count >= 3 && (!decoded || applied.draw)) {  // DRAW_INDX
        DrawCall d;
        d.vs_hash = vs_hash; d.ps_hash = ps_hash;
        d.initiator = be32(pkt + 8);
        if (count >= 5) { d.index_base = be32(pkt + 12); d.index_size = be32(pkt + 16); }
        d.regs = regs;
        d.regs[reg::VGT_DRAW_INITIATOR::register_index] = d.initiator;
        draws.push_back(std::move(d));
      } else if (opcode == 0x36 && count >= 2 &&
                 (!decoded || applied.draw)) {  // DRAW_INDX_2 (auto)
        DrawCall d;
        d.vs_hash = vs_hash; d.ps_hash = ps_hash;
        d.initiator = be32(pkt + 4);
        d.regs = regs;
        d.regs[reg::VGT_DRAW_INITIATOR::register_index] = d.initiator;
        draws.push_back(std::move(d));
      } else if (decoded && applied.draw &&
                 (opcode == native_gpu_direct::kOpcode || opcode == native_gpu_calls::kOpcode ||
                  opcode == native_gpu_calls::kVerticesDrawOpcode ||
                  opcode == native_gpu_calls::kRangeDrawOpcode ||
                  opcode == native_gpu_calls::kBareDrawOpcode)) {
        DrawCall d;
        d.vs_hash = vs_hash; d.ps_hash = ps_hash;
        d.initiator = regs[reg::VGT_DRAW_INITIATOR::register_index];
        if (((d.initiator >> 6) & 3) == 0) {
          d.index_base = regs[XE_GPU_REG_VGT_DMA_BASE];
          d.index_size = regs[XE_GPU_REG_VGT_DMA_SIZE];
        }
        d.regs = regs;
        draws.push_back(std::move(d));
      }
    }
  };

  while (p + 4 <= end) {
    auto type = *reinterpret_cast<const TraceCommandType*>(p);
    if (type == TraceCommandType::kPacketStart) {
      auto cmd = reinterpret_cast<const PacketStartCommand*>(p);
      p += sizeof(*cmd);
      const uint8_t* raw_pkt = p;
      p += cmd->count * 4;
      // A packet's memory reads are recorded AFTER the packet command, so
      // defer this packet until the next PacketStart: by then every memory
      // record belonging to it has been applied, and memory-indirect packets
      // (LOAD_ALU_CONSTANT, IM_LOAD hashing) see the state the runtime saw.
      if (pending_pkt) process_packet(pending_pkt, pending_count);
      pending_pkt = raw_pkt;
      pending_count = cmd->count;
    } else if (type == TraceCommandType::kMemoryRead || type == TraceCommandType::kMemoryWrite) {
      auto cmd = reinterpret_cast<const MemoryCommand*>(p);
      const char* payload = reinterpret_cast<const char*>(p + sizeof(*cmd));
      p += sizeof(*cmd) + cmd->encoded_length;
      {
        // Both reads and writes carry guest memory content the frame depends
        // on - the trace player applies both, and the sky's vertex colors
        // arrive via a MemoryWrite.
        uint32_t base = cmd->base_ptr & (kGuestSpace - 1);
        if (uint64_t(base) + cmd->decoded_length <= kGuestSpace) {
          if (cmd->encoding_format == MemoryEncodingFormat::kSnappy) {
            std::string dec;
            if (snappy::Uncompress(payload, cmd->encoded_length, &dec))
              std::memcpy(guest.data() + base, dec.data(), dec.size());
            else
              std::fprintf(stderr, "snappy fail base=%08X len=%u\n", cmd->base_ptr,
                           cmd->decoded_length);
          } else {
            std::memcpy(guest.data() + base, payload, cmd->decoded_length);
          }
          if (const char* wa = getenv("REPLAY_WATCH")) {
            uint32_t target = uint32_t(strtoul(wa, nullptr, 16)) & (kGuestSpace - 1);
            if (target >= base && target < base + cmd->decoded_length) {
              std::fprintf(stderr, "mem %s base=%08X len=%u covers %08X (word there now: %02X%02X%02X%02X)\n",
                           type == TraceCommandType::kMemoryRead ? "READ " : "WRITE",
                           cmd->base_ptr, cmd->decoded_length, target,
                           guest[target], guest[target + 1], guest[target + 2], guest[target + 3]);
            }
          }
        }
      }
    } else if (type == TraceCommandType::kRegisters) {
      auto cmd = reinterpret_cast<const RegistersCommand*>(p);
      const char* payload = reinterpret_cast<const char*>(p + sizeof(*cmd));
      p += sizeof(*cmd) + cmd->encoded_length;
      std::string dec;
      const uint32_t* vals;
      if (cmd->encoding_format == MemoryEncodingFormat::kSnappy) {
        if (!snappy::Uncompress(payload, cmd->encoded_length, &dec)) return 1;
        vals = reinterpret_cast<const uint32_t*>(dec.data());
      } else {
        vals = reinterpret_cast<const uint32_t*>(payload);
      }
      for (uint32_t i = 0; i < cmd->register_count; ++i) {
        uint32_t idx = cmd->first_register + i;
        if (idx < kRegisterCount) regs[idx] = vals[i];
      }
    } else if (type == TraceCommandType::kEdramSnapshot) {
      auto cmd = reinterpret_cast<const EdramSnapshotCommand*>(p);
      p += sizeof(*cmd) + cmd->encoded_length;
    } else if (type == TraceCommandType::kGammaRamp) {
      auto cmd = reinterpret_cast<const GammaRampCommand*>(p);
      p += sizeof(*cmd) + cmd->encoded_length;
    } else if (type == TraceCommandType::kEvent) {
      p += sizeof(EventCommand);
    } else if (type == TraceCommandType::kPrimaryBufferStart ||
               type == TraceCommandType::kIndirectBufferStart) {
      p += sizeof(PrimaryBufferStartCommand);
    } else {  // buffer ends / packet end
      p += 4;
    }
  }
  if (pending_pkt) process_packet(pending_pkt, pending_count);
  std::printf("trace parsed: %zu draws\n", draws.size());
  // Native resolve convergence: the trace never records resolve pixels (only
  // GPU fences), so resolves must be produced by our own rendering. They are
  // written to a sidecar during execution and applied here on the next run -
  // running the replayer twice lets every pass sample the previous run's
  // resolved output, which converges for a single frame.
  fs::path sidecar = fs::path(argv[2]) / "resolved_mem.bin";
  {
    std::ifstream rf(sidecar, std::ios::binary);
    size_t applied = 0;
    while (rf) {
      uint32_t base = 0, size = 0;
      rf.read(reinterpret_cast<char*>(&base), 4);
      rf.read(reinterpret_cast<char*>(&size), 4);
      if (!rf || !size || uint64_t(base) + size > kGuestSpace) break;
      rf.read(reinterpret_cast<char*>(guest.data() + base), size);
      ++applied;
    }
    if (applied) std::printf("applied %zu resolved regions from sidecar\n", applied);
  }
  std::ofstream sidecar_out(sidecar, std::ios::binary);

  // ------------------------------------------------------------- manifest
  fs::path spv_dir = argv[2];
  json manifest;
  { std::ifstream mf(spv_dir / "manifest.json"); mf >> manifest; }
  struct TexBinding { uint32_t binding_index; uint32_t fetch_constant; };
  struct ShaderInfo { std::string file; std::string tex_sig; uint32_t color_targets;
                      uint64_t float_bitmap[4] = {}; bool float_dynamic = false;
                      uint64_t modification = 0;
                      uint32_t register_count = 0; bool dynamic_addressing = false;
                      uint32_t writes_interpolators = 0;
                      std::vector<TexBinding> tex_bindings;
                      // SPIR-V texture set: binding -> fetch constant, parsed
                      // from the translator's own variable names
                      // (xe_texture<fc>_<dim>_<u|s>), the authoritative map.
                      std::map<uint32_t, uint32_t> image_binding_fc;
                      // Same for samplers (xe_sampler<fc>_...).
                      std::map<uint32_t, uint32_t> sampler_binding_fc; };
  std::map<std::string, ShaderInfo> shaders;  // "<hash>_<type>"
  for (const auto& s : manifest["shaders"]) {
    ShaderInfo si = {s["spirv"].get<std::string>(), s.value("texture_set_signature", ""),
                     s.value("writes_color_targets", 0u)};
    if (s.contains("float_bitmap")) {
      for (int i = 0; i < 4; ++i)
        si.float_bitmap[i] = std::strtoull(
            s["float_bitmap"][i].get<std::string>().c_str(), nullptr, 16);
      si.float_dynamic = s.value("float_dynamic", false);
    }
    for (const auto& tb : s.value("texture_bindings", json::array()))
      si.tex_bindings.push_back({tb["binding_index"].get<uint32_t>(),
                                 tb["fetch_constant"].get<uint32_t>()});
    for (const auto& dsc : s.value("descriptors", json::array())) {
      if (dsc.value("set", 0u) < 2) continue;
      std::string nm = dsc.value("name", "");
      if (nm.rfind("xe_texture", 0) == 0)
        si.image_binding_fc[dsc["binding"].get<uint32_t>()] =
            uint32_t(std::atoi(nm.c_str() + 10));
      else if (nm.rfind("xe_sampler", 0) == 0)
        si.sampler_binding_fc[dsc["binding"].get<uint32_t>()] =
            uint32_t(std::atoi(nm.c_str() + 10));
    }
    si.register_count = s.value("register_count", 0u);
    si.dynamic_addressing = s.value("dynamic_register_addressing", false);
    si.writes_interpolators = uint32_t(std::strtoul(
        s.value("writes_interpolators", std::string("0")).c_str(), nullptr, 16));
    uint64_t mod_bits = std::strtoull(s["modification"].get<std::string>().c_str(), nullptr, 16);
    si.modification = mod_bits;
    bool is_rect_variant = ((mod_bits >> 32) & 0xF) == 10;
    std::string map_key = s["hash"].get<std::string>() + "_" + s["type"].get<std::string>();
    // Full-precision key for pair-exact selection, plus the plain key as the
    // fallback when a pair wasn't recorded.
    shaders[map_key + "_" + s["modification"].get<std::string>()] = si;
    if (!is_rect_variant && !shaders.count(map_key)) {
      shaders[map_key] = si;
    }
  }
  // The recorded pipelines name the exact specialization pair the runtime
  // used for each (vs, ps) - the interpolator masks that make the stages
  // agree. Prefer those over any single per-shader default.
  std::map<std::string, std::pair<std::string, std::string>> pair_mods;
  for (const auto& pr : manifest.value("pipelines", json::array())) {
    pair_mods[pr["vs"].get<std::string>() + "_" + pr["ps"].get<std::string>()] =
        {pr["vs_modification"].get<std::string>(), pr["ps_modification"].get<std::string>()};
  }
  char hex[24];
  auto hash_str = [&](uint64_t h) { std::snprintf(hex, sizeof(hex), "%016llX",
                                                  (unsigned long long)h); return std::string(hex); };

  // --------------------------------------------------------------- vulkan
  CHECK_VK(volkInitialize());
  VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.apiVersion = VK_API_VERSION_1_3;
  VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ici.pApplicationInfo = &app;
  VkInstance instance;
  CHECK_VK(vkCreateInstance(&ici, nullptr, &instance));
  volkLoadInstance(instance);
  uint32_t n_gpu = 1;
  VkPhysicalDevice gpu;
  vkEnumeratePhysicalDevices(instance, &n_gpu, &gpu);

  VkPhysicalDeviceVulkan13Features f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
  VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
  f12.pNext = &f13;
  VkPhysicalDeviceVulkan11Features f11 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
  f11.pNext = &f12;
  VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
  f2.pNext = &f11;
  vkGetPhysicalDeviceFeatures2(gpu, &f2);
  float prio = 1.0f;
  VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  qci.queueCount = 1; qci.pQueuePriorities = &prio;
  VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  dci.pNext = &f2; dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
  VkDevice dev;
  CHECK_VK(vkCreateDevice(gpu, &dci, nullptr, &dev));
  volkLoadDevice(dev);
  VkQueue queue;
  vkGetDeviceQueue(dev, 0, 0, &queue);

  auto make_buffer = [&](VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer* buf,
                         VkDeviceMemory* mem, void** map) {
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = size; bci.usage = usage;
    vkCreateBuffer(dev, &bci, nullptr, buf);
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(dev, *buf, &mr);
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = FindMemoryType(gpu, mr.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    vkAllocateMemory(dev, &mai, nullptr, mem);
    vkBindBufferMemory(dev, *buf, *mem, 0);
    if (map) vkMapMemory(dev, *mem, 0, size, 0, map);
  };

  // Shared memory: the guest image, one big SSBO the shaders vfetch from.
  VkBuffer shared_buf; VkDeviceMemory shared_mem; void* shared_map;
  make_buffer(kGuestSpace, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &shared_buf, &shared_mem,
              &shared_map);
  std::memcpy(shared_map, guest.data(), kGuestSpace);
  std::printf("shared memory uploaded (512 MB)\n");

  // Per-draw constants, sliced from each draw's register snapshot.
  // Layout per draw: system (aligned 1KB) | floatVS 4KB | floatPS 4KB |
  // boolloop 512B | fetch 1KB  => 12KB stride.
  constexpr uint32_t kSysOff = 0, kFvOff = 1024, kFpOff = 5120, kBlOff = 9216, kFetchOff = 9728;
  constexpr uint32_t kStride = 12288;
  VkBuffer const_buf; VkDeviceMemory const_mem; void* const_map;
  make_buffer(VkDeviceSize(kStride) * draws.size(),
              VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &const_buf, &const_mem, &const_map);

  // Index scratch: worst case all draws 32-bit.
  VkBuffer index_buf; VkDeviceMemory index_mem; void* index_map;
  make_buffer(96u << 20, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, &index_buf, &index_mem, &index_map);

  // ------------------------------------------------- shader modules
  std::map<std::string, VkShaderModule> modules;
  for (const auto& [key, info] : shaders) {
    auto words = ReadWords(spv_dir / info.file);
    VkShaderModuleCreateInfo smci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smci.codeSize = words.size() * 4;
    smci.pCode = words.data();
    VkShaderModule mod;
    if (vkCreateShaderModule(dev, &smci, nullptr, &mod) == VK_SUCCESS) modules[key] = mod;
  }

  // ------------------------------------------------- descriptor layouts
  auto make_set_layout = [&](std::vector<VkDescriptorSetLayoutBinding> b) {
    VkDescriptorSetLayoutCreateInfo ci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    ci.bindingCount = uint32_t(b.size()); ci.pBindings = b.data();
    VkDescriptorSetLayout l; vkCreateDescriptorSetLayout(dev, &ci, nullptr, &l); return l;
  };
  VkShaderStageFlags both = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
  VkDescriptorSetLayout set0 = make_set_layout(
      {{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, both, nullptr}});
  std::vector<VkDescriptorSetLayoutBinding> ubo_bindings;
  for (uint32_t i = 0; i < 5; ++i)
    ubo_bindings.push_back({i, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, both, nullptr});
  VkDescriptorSetLayout set1 = make_set_layout(ubo_bindings);
  VkDescriptorSetLayout empty_set = make_set_layout({});

  // Texture set layouts per signature ("3:0:SAMPLED_IMAGE;3:4:SAMPLER;...").
  auto parse_sig = [](const std::string& sig) {
    std::vector<std::pair<uint32_t, bool>> out;  // binding, is_image
    size_t pos = 0;
    while (pos < sig.size()) {
      size_t semi = sig.find(';', pos);
      std::string tok = sig.substr(pos, semi - pos);
      pos = semi == std::string::npos ? sig.size() : semi + 1;
      size_t c1 = tok.find(':'), c2 = tok.find(':', c1 + 1);
      if (c2 == std::string::npos) continue;
      out.push_back({uint32_t(std::stoul(tok.substr(c1 + 1, c2 - c1 - 1))),
                     tok.substr(c2 + 1).find("IMAGE") != std::string::npos});
    }
    return out;
  };
  std::map<std::string, VkDescriptorSetLayout> tex_layouts;
  std::map<std::string, VkPipelineLayout> pipe_layouts;
  tex_layouts[""] = empty_set;
  for (const auto& [key, info] : shaders) {
    if (tex_layouts.count(info.tex_sig)) continue;
    std::vector<VkDescriptorSetLayoutBinding> tb;
    for (auto [binding, is_image] : parse_sig(info.tex_sig))
      tb.push_back({binding, is_image ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE
                                      : VK_DESCRIPTOR_TYPE_SAMPLER, 1,
                    VK_SHADER_STAGE_FRAGMENT_BIT, nullptr});
    tex_layouts[info.tex_sig] = make_set_layout(tb);
  }
  for (const auto& [sig, tl] : tex_layouts) {
    VkDescriptorSetLayout sets[4] = {set0, set1, empty_set, tl};
    VkPipelineLayoutCreateInfo ci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    ci.setLayoutCount = sig.empty() ? 2 : 4; ci.pSetLayouts = sets;
    VkPipelineLayout pl; vkCreatePipelineLayout(dev, &ci, nullptr, &pl);
    pipe_layouts[sig] = pl;
  }

  // ------------------------------------------------- dummy texture + sampler
  VkImage dummy_img; VkDeviceMemory dummy_mem; VkImageView dummy_view; VkSampler sampler;
  {
    VkImageCreateInfo ici2 = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici2.imageType = VK_IMAGE_TYPE_2D; ici2.format = VK_FORMAT_R8G8B8A8_UNORM;
    ici2.extent = {1, 1, 1}; ici2.mipLevels = 1; ici2.arrayLayers = 1;
    ici2.samples = VK_SAMPLE_COUNT_1_BIT;
    ici2.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    vkCreateImage(dev, &ici2, nullptr, &dummy_img);
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(dev, dummy_img, &mr);
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = FindMemoryType(gpu, mr.memoryTypeBits,
                                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vkAllocateMemory(dev, &mai, nullptr, &dummy_mem);
    vkBindImageMemory(dev, dummy_img, dummy_mem, 0);
    VkImageViewCreateInfo vci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = dummy_img; vci.viewType = VK_IMAGE_VIEW_TYPE_2D; vci.format = ici2.format;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCreateImageView(dev, &vci, nullptr, &dummy_view);
    VkSamplerCreateInfo sci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = VK_FILTER_LINEAR;
    sci.minFilter = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.magFilter = VK_FILTER_LINEAR; sci.minFilter = VK_FILTER_LINEAR;
    sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    vkCreateSampler(dev, &sci, nullptr, &sampler);
  }
  // Per-texture samplers from the fetch constant's clamp and filter fields.
  std::map<uint32_t, VkSampler> fc_samplers;
  auto get_sampler = [&](const uint32_t* fc6) -> VkSampler {
    uint32_t clamp_x = (fc6[0] >> 10) & 7, clamp_y = (fc6[0] >> 13) & 7;
    uint32_t mag = (fc6[3] >> 19) & 3, min = (fc6[3] >> 21) & 3,
             mip = (fc6[3] >> 23) & 3;
    uint32_t key = clamp_x | (clamp_y << 3) | (mag << 6) | (min << 8) | (mip << 10);
    auto it = fc_samplers.find(key);
    if (it != fc_samplers.end()) return it->second;
    auto vk_clamp = [](uint32_t c) {
      switch (c) {
        case 0: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
        case 1: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        case 2: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        case 3: return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
        default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
      }
    };
    // Xenos TextureFilter: 0 nearest, 1 linear, 2 base map, 3 keep (use
    // linear for the "use fetch const high bits" cases - close enough here).
    VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = mag == 0 ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    si.minFilter = min == 0 ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = vk_clamp(clamp_x);
    si.addressModeV = vk_clamp(clamp_y);
    si.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    VkSampler s;
    vkCreateSampler(dev, &si, nullptr, &s);
    fc_samplers[key] = s;
    return s;
  };

  // ------------------------------------------------- real textures
  // Decode + untile + endian-swap the textures the frame's materials
  // reference, straight from reconstructed guest memory. Unsupported formats
  // fall back to the dummy so nothing blocks.
  struct GuestTex { VkImage img; VkDeviceMemory mem; VkImageView view; };
  std::map<uint64_t, VkImageView> tex_views;      // fc6-content hash -> view
  std::vector<GuestTex> owned_textures;
  std::vector<std::pair<VkBuffer, VkImage>> pending_copies;
  std::vector<std::pair<VkBuffer, VkDeviceMemory>> staging_buffers;
  std::vector<VkBufferImageCopy> pending_regions;
  auto upload_texture = [&](const uint32_t* fc6) -> VkImageView {
    if (getenv("REPLAY_FORCE_DUMMY")) return dummy_view;
    uint64_t key = XXH3_64bits(fc6, 24);
    auto it2 = tex_views.find(key);
    if (it2 != tex_views.end()) return it2->second;
    uint32_t fmt = fc6[1] & 0x3F;
    uint32_t endian2 = (fc6[1] >> 6) & 3;
    bool tiled = (fc6[0] >> 31) != 0;
    uint32_t base = ((fc6[1] >> 12) & 0xFFFFF) << 12;
    uint32_t w = (fc6[2] & 0x1FFF) + 1, h = ((fc6[2] >> 13) & 0x1FFF) + 1;
    uint32_t pitch_px = ((fc6[0] >> 22) & 0x1FF) << 5;
    VkFormat vkfmt; uint32_t block_dim, bpb, bpb_log2;
    switch (fmt) {
      case 2:  vkfmt = VK_FORMAT_R8_UNORM; block_dim = 1; bpb = 1; bpb_log2 = 0; break;
      case 6:  vkfmt = VK_FORMAT_R8G8B8A8_UNORM; block_dim = 1; bpb = 4; bpb_log2 = 2; break;
      case 18: vkfmt = VK_FORMAT_BC1_RGBA_UNORM_BLOCK; block_dim = 4; bpb = 8; bpb_log2 = 3; break;
      case 19: vkfmt = VK_FORMAT_BC2_UNORM_BLOCK; block_dim = 4; bpb = 16; bpb_log2 = 4; break;
      case 20: vkfmt = VK_FORMAT_BC3_UNORM_BLOCK; block_dim = 4; bpb = 16; bpb_log2 = 4; break;
      case 26: vkfmt = VK_FORMAT_R16G16B16A16_UNORM; block_dim = 1; bpb = 8; bpb_log2 = 3; break;
      case 23: vkfmt = VK_FORMAT_R32_SFLOAT; block_dim = 1; bpb = 4; bpb_log2 = 2; break;
      case 54: vkfmt = VK_FORMAT_A2B10G10R10_UNORM_PACK32; block_dim = 1; bpb = 4; bpb_log2 = 2;
               break;
      default:
        std::fprintf(stderr, "tex fallback: fmt=%u %ux%u tiled=%u endian=%u\n",
                     fmt, w, h, tiled, endian2);
        tex_views[key] = dummy_view; return dummy_view;
    }
    uint32_t wb = (w + block_dim - 1) / block_dim, hb = (h + block_dim - 1) / block_dim;
    uint32_t pitch_b = std::max(pitch_px / block_dim, wb);
    if (uint64_t(base) + uint64_t(pitch_b) * hb * bpb > kGuestSpace) {
      tex_views[key] = dummy_view; return dummy_view;
    }
    std::vector<uint8_t> out(size_t(wb) * hb * bpb);
    const uint8_t* src = guest.data() + base;
    for (uint32_t by = 0; by < hb; ++by) {
      for (uint32_t bx = 0; bx < wb; ++bx) {
        size_t so = tiled ? size_t(texture_util::GetTiledOffset2D(int32_t(bx), int32_t(by),
                                                                 pitch_b, bpb_log2))
                          : (size_t(by) * pitch_b + bx) * bpb;
        if (base + so + bpb > kGuestSpace) continue;
        std::memcpy(out.data() + (size_t(by) * wb + bx) * bpb, src + so, bpb);
      }
    }
    if (fmt == 23) {
      // k_24_8_FLOAT: 20e4 depth in the high 24 bits, stencil low. The
      // shaders sample it as a plain float depth value.
      for (size_t i = 0; i + 3 < out.size(); i += 4) {
        uint32_t v = uint32_t(out[i]) | uint32_t(out[i+1]) << 8 | uint32_t(out[i+2]) << 16 |
                     uint32_t(out[i+3]) << 24;
        if (endian2 == 2) v = __builtin_bswap32(v);
        float depth = xenos::Float20e4To32(v >> 8);
        std::memcpy(out.data() + i, &depth, 4);
      }
    } else if (endian2 == 1) {        // 8in16
      for (size_t i = 0; i + 1 < out.size(); i += 2) std::swap(out[i], out[i + 1]);
    } else if (endian2 == 2) { // 8in32
      for (size_t i = 0; i + 3 < out.size(); i += 4) {
        std::swap(out[i], out[i + 3]); std::swap(out[i + 1], out[i + 2]);
      }
    }
    if (getenv("REPLAY_DUMP_TEX") && fmt == 6 && w >= 128) {
      static int dumped_n = 0;
      if (dumped_n < 6) {
        char fn[128];
        std::snprintf(fn, sizeof(fn), "texdump_%d_%ux%u.ppm", dumped_n++, w, h);
        std::ofstream tex_out(fn, std::ios::binary);
        tex_out << "P6\n" << w << " " << h << "\n255\n";
        for (uint32_t px2 = 0; px2 < w * h; ++px2) {
          tex_out.put(char(out[px2 * 4])); tex_out.put(char(out[px2 * 4 + 1]));
          tex_out.put(char(out[px2 * 4 + 2]));
        }
      }
    }
    // Image + staging.
    GuestTex gt = {};
    VkImageCreateInfo ici3 = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici3.imageType = VK_IMAGE_TYPE_2D; ici3.format = vkfmt; ici3.extent = {w, h, 1};
    ici3.mipLevels = 1; ici3.arrayLayers = 1; ici3.samples = VK_SAMPLE_COUNT_1_BIT;
    ici3.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (vkCreateImage(dev, &ici3, nullptr, &gt.img) != VK_SUCCESS) {
      tex_views[key] = dummy_view; return dummy_view;
    }
    VkMemoryRequirements mr3; vkGetImageMemoryRequirements(dev, gt.img, &mr3);
    VkMemoryAllocateInfo mai3 = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai3.allocationSize = mr3.size;
    mai3.memoryTypeIndex = FindMemoryType(gpu, mr3.memoryTypeBits,
                                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vkAllocateMemory(dev, &mai3, nullptr, &gt.mem);
    vkBindImageMemory(dev, gt.img, gt.mem, 0);
    VkBuffer stage; VkDeviceMemory stage_mem; void* stage_map;
    make_buffer(out.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &stage, &stage_mem, &stage_map);
    std::memcpy(stage_map, out.data(), out.size());
    staging_buffers.push_back({stage, stage_mem});
    VkBufferImageCopy rgn = {};
    rgn.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    rgn.imageExtent = {w, h, 1};
    pending_copies.push_back({stage, gt.img});
    pending_regions.push_back(rgn);
    VkImageViewCreateInfo vci3 = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci3.image = gt.img; vci3.viewType = VK_IMAGE_VIEW_TYPE_2D; vci3.format = vkfmt;
    // The fetch constant's destination swizzle (dword 3 bits 1:12, 3 bits per
    // output channel: 0-3 select X-W, 4 = zero, 5 = one). The AOT shaders are
    // translated with host image-view swizzling, so it must be baked here.
    {
      uint32_t swz = (fc6[3] >> 1) & 0xFFF;
      auto map = [](uint32_t sel) {
        switch (sel) {
          case 0: return VK_COMPONENT_SWIZZLE_R;
          case 1: return VK_COMPONENT_SWIZZLE_G;
          case 2: return VK_COMPONENT_SWIZZLE_B;
          case 3: return VK_COMPONENT_SWIZZLE_A;
          case 4: return VK_COMPONENT_SWIZZLE_ZERO;
          default: return VK_COMPONENT_SWIZZLE_ONE;
        }
      };
      vci3.components = {map(swz & 7), map((swz >> 3) & 7), map((swz >> 6) & 7),
                         map((swz >> 9) & 7)};
    }
    vci3.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCreateImageView(dev, &vci3, nullptr, &gt.view);
    owned_textures.push_back(gt);
    tex_views[key] = gt.view;
    return gt.view;
  };

  // ------------------------------------------------- descriptor sets
  VkDescriptorPoolSize pool_sizes[] = {
      {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4},
      {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 8192},
      {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 256},
      {VK_DESCRIPTOR_TYPE_SAMPLER, 256}};
  VkDescriptorPoolCreateInfo dpci = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  dpci.maxSets = 2048; dpci.poolSizeCount = 4; dpci.pPoolSizes = pool_sizes;
  VkDescriptorPool pool; vkCreateDescriptorPool(dev, &dpci, nullptr, &pool);
  auto alloc_set = [&](VkDescriptorSetLayout l) {
    VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = pool; ai.descriptorSetCount = 1; ai.pSetLayouts = &l;
    VkDescriptorSet s2; vkAllocateDescriptorSets(dev, &ai, &s2); return s2;
  };
  VkDescriptorSet ds0 = alloc_set(set0);
  {
    VkDescriptorBufferInfo bi = {shared_buf, 0, kGuestSpace};
    VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = ds0; w.dstBinding = 0; w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w.pBufferInfo = &bi;
    vkUpdateDescriptorSets(dev, 1, &w, 0, nullptr);
  }
  std::map<std::string, VkDescriptorSet> tex_sets;
  for (const auto& [sig, tl] : tex_layouts) {
    if (sig.empty()) continue;
    VkDescriptorSet s2 = alloc_set(tl);
    for (auto [binding, is_image] : parse_sig(sig)) {
      VkDescriptorImageInfo ii = {};
      if (is_image) { ii.imageView = dummy_view;
                      ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; }
      else ii.sampler = sampler;
      VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      w.dstSet = s2; w.dstBinding = binding; w.descriptorCount = 1;
      w.descriptorType = is_image ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE
                                  : VK_DESCRIPTOR_TYPE_SAMPLER;
      w.pImageInfo = &ii;
      vkUpdateDescriptorSets(dev, 1, &w, 0, nullptr);
    }
    tex_sets[sig] = s2;
  }

  // ------------------------------------------------- render targets
  auto make_image = [&](VkFormat fmt, VkImageUsageFlags usage, VkImageAspectFlags aspect,
                        VkImage* img, VkDeviceMemory* mem, VkImageView* view) {
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D; ci.format = fmt; ci.extent = {kWidth, kHeight, 1};
    ci.mipLevels = 1; ci.arrayLayers = 1; ci.samples = VK_SAMPLE_COUNT_1_BIT; ci.usage = usage;
    vkCreateImage(dev, &ci, nullptr, img);
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(dev, *img, &mr);
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = FindMemoryType(gpu, mr.memoryTypeBits,
                                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vkAllocateMemory(dev, &mai, nullptr, mem);
    vkBindImageMemory(dev, *img, *mem, 0);
    VkImageViewCreateInfo vci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = *img; vci.viewType = VK_IMAGE_VIEW_TYPE_2D; vci.format = fmt;
    vci.subresourceRange = {aspect, 0, 1, 0, 1};
    vkCreateImageView(dev, &vci, nullptr, view);
  };
  // The guest's primary surfaces are 2_10_10_10: render in the same depth so
  // resolve rounding (10-bit color, 2-bit alpha) matches the hardware exactly.
  const VkFormat color_fmt = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
  const VkFormat depth_fmt = VK_FORMAT_D32_SFLOAT_S8_UINT;
  // One color[4]+depth attachment group per distinct guest color surface, so
  // shadow/reflection/sky passes stop stomping the presented image. All are
  // full-size; the guest's viewport confines each pass to its region.
  struct Surface {
    VkImage color_img[4]; VkDeviceMemory color_mem[4]; VkImageView color_view[4];
    VkImage depth_img; VkDeviceMemory depth_mem; VkImageView depth_view;
    bool cleared = false;
  };
  std::map<uint32_t, Surface> surfaces;
  auto get_surface = [&](uint32_t key) -> Surface& {
    auto it = surfaces.find(key);
    if (it != surfaces.end()) return it->second;
    Surface sf = {};
    for (int i = 0; i < 4; ++i)
      make_image(color_fmt, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                 VK_IMAGE_ASPECT_COLOR_BIT, &sf.color_img[i], &sf.color_mem[i], &sf.color_view[i]);
    make_image(depth_fmt, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
               VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
               &sf.depth_img, &sf.depth_mem, &sf.depth_view);
    return surfaces.emplace(key, sf).first->second;
  };
  VkBuffer readback_buf; VkDeviceMemory readback_mem; void* readback_map;
  make_buffer(kWidth * kHeight * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &readback_buf,
              &readback_mem, &readback_map);

  // ------------------------------------------------- per-draw prep
  using Sys = SpirvShaderTranslator::SystemConstants;
  static_assert(sizeof(Sys) <= 1024);
  uint8_t* cmap = static_cast<uint8_t*>(const_map);
  uint32_t index_cursor = 0;
  uint8_t* imap = static_cast<uint8_t*>(index_map);

  struct Encoded {
    VkPipeline pipe; VkPipelineLayout layout; VkDescriptorSet ds1; VkDescriptorSet ds3;
    bool indexed; uint32_t index_offset; VkIndexType index_type; uint32_t count;
    bool has_tex;
    uint32_t depth_key;
    VkViewport viewport; VkRect2D scissor;
    uint32_t surface_key;
  };
  struct Resolve {
    size_t after_encoded;
    uint32_t copy_control, dest_base, dest_pitch, dest_info, surface_key;
    uint32_t x, y, w, h;
  };
  std::vector<Resolve> resolves;
  std::vector<Encoded> encoded;
  std::map<uint64_t, VkPipeline> pipeline_cache;
  std::map<uint32_t, VkShaderModule> gs_modules;
  auto get_prim_gs = [&](uint64_t vs_mod, uint32_t which) -> VkShaderModule {
    using namespace rex::graphics::nativegs;
    GeometryShaderKey gk;
    gk.type = PipelineGeometryShader(which);
    gk.interpolator_count = uint32_t(std::popcount(uint32_t(vs_mod & 0xFFFF)));
    auto gi = gs_modules.find(gk.key);
    if (gi != gs_modules.end()) return gi->second;
    std::vector<unsigned int> code = GenerateGeometryShader(gk);
    VkShaderModuleCreateInfo mi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = code.size() * 4; mi.pCode = code.data();
    VkShaderModule m = VK_NULL_HANDLE;
    vkCreateShaderModule(dev, &mi, nullptr, &m);
    gs_modules[gk.key] = m;
    return m;
  };
  size_t skip_shader = 0, skip_prim = 0, skip_offscreen = 0, skip_copy = 0, skip_postresolve = 0;
  // EDRAM copy (resolve) draws are not geometry, and the presented image is
  // whatever the render target held at the LAST resolve - draws after it
  // (next frame's early passes, like the sky clear) must not appear.
  size_t last_resolve = draws.size();
  for (size_t ri = 0; ri < draws.size(); ++ri) {
    if ((draws[ri].regs[reg::RB_MODECONTROL::register_index] & 7) == 6) last_resolve = ri;
  }
  std::printf("last resolve at draw %zu of %zu\n", last_resolve, draws.size());
  // Draws that target a different EDRAM color surface than the one presented
  // (shadow maps, post buffers) must not paint into the main target. Until
  // per-surface render targets exist, the presented surface is taken to be
  // the most common RB_COLOR_INFO among the frame's draws.
  uint32_t primary_color_info = 0;
  {
    std::map<uint32_t, uint32_t> color_info_histogram;
    for (const DrawCall& d2 : draws)
      ++color_info_histogram[d2.regs[reg::RB_COLOR_INFO::register_index]];
    uint32_t best = 0;
    for (const auto& [info, count] : color_info_histogram)
      if (count > best) { best = count; primary_color_info = info; }
    std::printf("primary RB_COLOR_INFO: %08X (%u draws), %zu distinct surfaces\n",
                primary_color_info, best, color_info_histogram.size());
  }

  static const std::map<uint32_t, VkPrimitiveTopology> kPrim = {
      {1, VK_PRIMITIVE_TOPOLOGY_POINT_LIST},
      {13, VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY},
      {2, VK_PRIMITIVE_TOPOLOGY_LINE_LIST}, {3, VK_PRIMITIVE_TOPOLOGY_LINE_STRIP},
      {4, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST}, {5, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN},
      {6, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP}};

  size_t pair_hits = 0, pair_misses = 0;
  std::set<std::string> needed_pairs;
  long only_draw = getenv("REPLAY_ONLY") ? atol(getenv("REPLAY_ONLY")) : -1;
  long from_draw = getenv("REPLAY_FROM") ? atol(getenv("REPLAY_FROM")) : -1;
  for (size_t di = 0; di < draws.size(); ++di) {
    if (only_draw >= 0 && di != size_t(only_draw)) continue;
    if (from_draw >= 0 && di < size_t(from_draw)) continue;
    const DrawCall& d = draws[di];
    auto init = reinterpret_cast<const reg::VGT_DRAW_INITIATOR&>(d.initiator);
    bool rect_list = uint32_t(init.prim_type) == 8;
    bool point_list = uint32_t(init.prim_type) == 1;
    bool quad_list = uint32_t(init.prim_type) == 13;
    auto prim_it = kPrim.find(rect_list ? 4u : point_list ? 1u : quad_list ? 13u
                                        : uint32_t(init.prim_type));
    if (prim_it == kPrim.end()) { ++skip_prim; continue; }
    uint32_t edram_mode = d.regs[reg::RB_MODECONTROL::register_index] & 7;
    if (edram_mode == 6) {
      // A resolve: EDRAM -> guest memory copy. The trace does NOT carry the
      // written pixels (only 4-byte fences), so the native path must perform
      // it: render everything up to here, read the surface back, convert into
      // guest memory at the copy destination, and re-upload any textures that
      // sampled that range. Record the parameters; the executor consumes them.
      Resolve rz;
      rz.after_encoded = encoded.size();
      rz.copy_control = d.regs[0x2318];
      rz.dest_base = d.regs[0x2319];
      rz.dest_pitch = d.regs[0x231A];
      rz.dest_info = d.regs[0x231B];
      rz.surface_key = d.regs[reg::RB_COLOR_INFO::register_index] & 0xFFF;
      draw_util::Scissor rsc;
      {
        RegisterFile rrf;
        std::memcpy(rrf.values, d.regs.data(), sizeof(rrf.values));
        draw_util::GetScissor(rrf, rsc);
      }
      rz.x = rsc.offset[0]; rz.y = rsc.offset[1];
      rz.w = rsc.extent[0]; rz.h = rsc.extent[1];
      // The resolve rectangle comes from the copy draw's own three vertices
      // (screen-space corners in the stream at vfetch slot 0), not from the
      // scissor - downsampled targets (bloom) copy a small corner region.
      {
        const uint32_t* fc0 = d.regs.data() + kRegFetchConstants;
        uint32_t vaddr = (fc0[0] & ~3u) & (kGuestSpace - 1);
        uint32_t vwords = fc0[1] & 0xFFFFFF;
        if ((fc0[0] & 3) == 3 && vaddr && vwords >= 6 &&
            uint64_t(vaddr) + 24 <= kGuestSpace) {
          float xs[3], ys[3];
          bool ok = true;
          for (int vi = 0; vi < 3 && ok; ++vi) {
            uint32_t xv = be32(guest.data() + vaddr + vi * 8);
            uint32_t yv = be32(guest.data() + vaddr + vi * 8 + 4);
            std::memcpy(&xs[vi], &xv, 4);
            std::memcpy(&ys[vi], &yv, 4);
            if (!(std::isfinite(xs[vi]) && std::isfinite(ys[vi]) &&
                  std::fabs(xs[vi]) <= 8192.0f && std::fabs(ys[vi]) <= 8192.0f))
              ok = false;
          }
          if (ok) {
            float x0 = std::min({xs[0], xs[1], xs[2]}), x1 = std::max({xs[0], xs[1], xs[2]});
            float y0 = std::min({ys[0], ys[1], ys[2]}), y1 = std::max({ys[0], ys[1], ys[2]});
            uint32_t rx = uint32_t(std::max(0.0f, std::floor(x0)));
            uint32_t ry = uint32_t(std::max(0.0f, std::floor(y0)));
            uint32_t rw = uint32_t(std::max(0.0f, std::ceil(x1))) - rx;
            uint32_t rh = uint32_t(std::max(0.0f, std::ceil(y1))) - ry;
            if (rw && rh) { rz.x = rx; rz.y = ry; rz.w = rw; rz.h = rh; }
          }
          if (getenv("REPLAY_COPY_LOG"))
            std::fprintf(stderr,
                         "  copy-verts draw=%zu ok=%d fc0=%08X %08X v=(%.1f,%.1f)(%.1f,%.1f)(%.1f,%.1f)\n",
                         di, int(ok), fc0[0], fc0[1], xs[0], ys[0], xs[1], ys[1], xs[2], ys[2]);
        } else if (getenv("REPLAY_COPY_LOG")) {
          std::fprintf(stderr, "  copy-verts draw=%zu REJECT fc0=%08X %08X vaddr=%08X vwords=%u\n",
                       di, fc0[0], fc0[1], vaddr, vwords);
        }
      }
      resolves.push_back(rz);
      if (getenv("REPLAY_COPY_LOG"))
        std::fprintf(stderr,
                     "resolve draw=%zu ctl=%08X dest=%08X pitch=%u info=%08X rect=%u,%u %ux%u "
                     "after_enc=%zu\n",
                     di, rz.copy_control, rz.dest_base, rz.dest_pitch & 0x3FFF, rz.dest_info,
                     rz.x, rz.y, rz.w, rz.h, rz.after_encoded);
      ++skip_copy; continue;
    }
    if (edram_mode == 0) { ++skip_copy; continue; }
    if (di > last_resolve) { ++skip_postresolve; continue; }
    // Compute the modification pair from this draw's registers, mirroring
    // the runtime's GetCurrent{Vertex,Pixel}ShaderModification. The
    // interpolator mask is the piece that makes the two stages agree on
    // where every UV and color lives - without it a fallback pair can pass
    // interpolators in different slots and every texture samples a constant.
    std::string vs_mod_hex, ps_mod_hex;
    {
      auto sq_cntl = reinterpret_cast<const reg::SQ_PROGRAM_CNTL&>(
          d.regs[reg::SQ_PROGRAM_CNTL::register_index]);
      auto sq_misc = reinterpret_cast<const reg::SQ_CONTEXT_MISC&>(
          d.regs[reg::SQ_CONTEXT_MISC::register_index]);
      // Base info from the manifest entries under their plain keys.
      auto vs_base = shaders.find(hash_str(d.vs_hash) + "_vs");
      auto ps_base = shaders.find(hash_str(d.ps_hash) + "_ps");
      if (vs_base != shaders.end() && ps_base != shaders.end()) {
        const ShaderInfo& vsb = vs_base->second;
        const ShaderInfo& psb = ps_base->second;
        auto dyn_count = [](const ShaderInfo& sh, uint32_t num_reg) -> uint32_t {
          if (!sh.dynamic_addressing) return 0;
          return std::max(num_reg + 1u, sh.register_count);
        };
        uint32_t ps_dyn = dyn_count(psb, sq_cntl.ps_num_reg);
        uint32_t interp_count = std::min(16u, std::max(psb.register_count, ps_dyn));
        uint32_t ps_mask = interp_count >= 32 ? 0xFFFFu : ((1u << interp_count) - 1);
        uint32_t param_gen_pos = UINT32_MAX;
        if (sq_cntl.param_gen && sq_misc.param_gen_pos < interp_count) {
          ps_mask &= ~(1u << sq_misc.param_gen_pos);
          param_gen_pos = sq_misc.param_gen_pos;
        }
        uint32_t interp_mask = vsb.writes_interpolators & ps_mask;

        auto clip_cntl = reinterpret_cast<const reg::PA_CL_CLIP_CNTL&>(
            d.regs[reg::PA_CL_CLIP_CNTL::register_index]);
        uint32_t ucp = clip_cntl.clip_disable ? 0 : uint32_t(clip_cntl.ucp_ena);

        SpirvShaderTranslator::Modification vsm_bits(0);
        vsm_bits.vertex.interpolator_mask = interp_mask;
        vsm_bits.vertex.output_point_parameters = 0;
        vsm_bits.vertex.dynamic_addressable_register_count =
            dyn_count(vsb, sq_cntl.vs_num_reg);
        vsm_bits.vertex.user_clip_plane_count = uint32_t(std::popcount(ucp));
        vsm_bits.vertex.user_clip_plane_cull = 0;
        vsm_bits.vertex.vertex_kill_and = 0;
        vsm_bits.vertex.point_ps_ucp_mode = clip_cntl.ps_ucp_mode;
        // Always the plain vertex stage: rectangle/point/quad expansion is
        // done by the generated geometry shader, the same division of labor
        // the runtime uses on hardware with geometry shader support. The
        // "rectangle list as vertex loop" VS variants are the no-GS fallback
        // and are not used here.
        vsm_bits.vertex.host_vertex_shader_type = Shader::HostVertexShaderType::kVertex;
        vsm_bits.vertex.tessellation_mode = 0;

        SpirvShaderTranslator::Modification psm_bits(0);
        psm_bits.pixel.interpolator_mask = interp_mask;
        psm_bits.pixel.interpolators_centroid = 0;
        psm_bits.pixel.dynamic_addressable_register_count = ps_dyn;
        psm_bits.pixel.param_gen_enable = param_gen_pos != UINT32_MAX ? 1 : 0;
        psm_bits.pixel.param_gen_interpolator =
            param_gen_pos != UINT32_MAX ? param_gen_pos : 0;
        psm_bits.pixel.param_gen_point =
            uint32_t(param_gen_pos != UINT32_MAX && point_list);
        psm_bits.pixel.depth_stencil_mode =
            SpirvShaderTranslator::Modification::DepthStencilMode::kNoModifiers;

        char mh[24];
        std::snprintf(mh, sizeof(mh), "%016llX", (unsigned long long)vsm_bits.value);
        vs_mod_hex = mh;
        std::snprintf(mh, sizeof(mh), "%016llX", (unsigned long long)psm_bits.value);
        ps_mod_hex = mh;
      }
    }
    auto vs_it = shaders.end(), ps_it = shaders.end();
    if (!vs_mod_hex.empty()) {
      vs_it = shaders.find(hash_str(d.vs_hash) + "_vs_" + vs_mod_hex);
      ps_it = shaders.find(hash_str(d.ps_hash) + "_ps_" + ps_mod_hex);
    }
    if (vs_it != shaders.end() && ps_it != shaders.end()) {
      ++pair_hits;
    } else {
      ++pair_misses;
      if (!vs_mod_hex.empty()) {
        needed_pairs.insert(hash_str(d.vs_hash) + "," + vs_mod_hex + "," +
                            hash_str(d.ps_hash) + "," + ps_mod_hex);
      }
      // Fall back to the recorded pair or any variant so the frame still
      // renders while the exact variants are being compiled.
      auto pair_it = pair_mods.find(hash_str(d.vs_hash) + "_" + hash_str(d.ps_hash));
      vs_it = pair_it != pair_mods.end()
                  ? shaders.find(hash_str(d.vs_hash) + "_vs_" + pair_it->second.first)
                  : shaders.find(hash_str(d.vs_hash) + "_vs");
      if (vs_it == shaders.end()) vs_it = shaders.find(hash_str(d.vs_hash) + "_vs");
      ps_it = pair_it != pair_mods.end()
                  ? shaders.find(hash_str(d.ps_hash) + "_ps_" + pair_it->second.second)
                  : shaders.find(hash_str(d.ps_hash) + "_ps");
      if (ps_it == shaders.end()) ps_it = shaders.find(hash_str(d.ps_hash) + "_ps");
    }
    if (vs_it == shaders.end() || ps_it == shaders.end()) {
      if (getenv("REPLAY_SKIP_LOG"))
        std::fprintf(stderr, "skip draw=%zu vs=%s(%s) ps=%s(%s) prim=%u\n", di,
                     hash_str(d.vs_hash).c_str(), vs_it == shaders.end() ? "MISS" : "ok",
                     hash_str(d.ps_hash).c_str(), ps_it == shaders.end() ? "MISS" : "ok",
                     uint32_t(init.prim_type));
      ++skip_shader; continue;
    }
    VkShaderModule vsm = modules.count(vs_it->first) ? modules[vs_it->first] : VK_NULL_HANDLE;
    VkShaderModule psm = modules.count(ps_it->first) ? modules[ps_it->first] : VK_NULL_HANDLE;
    if (!vsm || !psm) { ++skip_shader; continue; }

    // Constants for this draw: the exact NDC transform the emulated backend
    // derives, straight from the same function (linked from the runtime lib).
    uint8_t* base = cmap + di * kStride;
    Sys sys = {};
    RegisterFile rf;
    std::memcpy(rf.values, d.regs.data(), sizeof(rf.values));
    auto normalized_depth = reinterpret_cast<const reg::RB_DEPTHCONTROL&>(
        d.regs[reg::RB_DEPTHCONTROL::register_index]);
    draw_util::ViewportInfo vpi;
    draw_util::GetHostViewportInfo(rf, 1, 1, false, kWidth, kHeight, false, normalized_depth,
                                   false, false, false, vpi);
    for (int i = 0; i < 3; ++i) {
      sys.ndc_scale[i] = vpi.ndc_scale[i];
      sys.ndc_offset[i] = vpi.ndc_offset[i];
    }
    // The translated VS clamps gl_VertexIndex into [vertex_index_min,
    // vertex_index_max] (VGT_MIN/MAX_VTX_INDX) - zeros here clamp every
    // vertex to index 0, which makes all primitives degenerate.
    sys.vertex_index_min = d.regs[0x2101];
    sys.vertex_index_max = d.regs[0x2100];
    sys.vertex_base_index = int32_t(d.regs[0x2102]);
    auto vte = reinterpret_cast<const reg::PA_CL_VTE_CNTL&>(
        d.regs[reg::PA_CL_VTE_CNTL::register_index]);
    if (vte.vtx_xy_fmt) sys.flags |= SpirvShaderTranslator::kSysFlag_XYDividedByW;
    if (vte.vtx_z_fmt) sys.flags |= SpirvShaderTranslator::kSysFlag_ZDividedByW;
    if (vte.vtx_w0_fmt) sys.flags |= SpirvShaderTranslator::kSysFlag_WNotReciprocal;
    // Alpha test: function bits of 0 mean "never pass", so a zeroed flags
    // word silently kills every fragment after shading. Mirror the runtime:
    // kAlways when the guest alpha test is off.
    auto colorcontrol = reinterpret_cast<const reg::RB_COLORCONTROL&>(
        d.regs[reg::RB_COLORCONTROL::register_index]);
    xenos::CompareFunction alpha_fn = colorcontrol.alpha_test_enable
                                          ? colorcontrol.alpha_func
                                          : xenos::CompareFunction::kAlways;
    sys.flags |= uint32_t(alpha_fn) << SpirvShaderTranslator::kSysFlag_AlphaPassIfLess_Shift;
    sys.alpha_test_reference = [&] {
      float v; uint32_t raw = d.regs[0x210E];  // RB_ALPHA_REF
      std::memcpy(&v, &raw, 4); return v;
    }();
    // Color exponent bias: zero bias must be exp2(0)=1, not 0, or every
    // color multiplies to black.
    for (int i = 0; i < 4; ++i) sys.color_exp_bias[i] = 1.0f;
    // Per-texture signedness (2 bits per channel, indexed by fetch-constant
    // slot): the shader decodes signed/gamma textures from these. Zeros read
    // every texture as plain unsigned, skipping the gamma decode entirely.
    {
      auto fill_signs = [&](const auto& bindings) {
        for (const auto& tb : bindings) {
          const uint32_t* fc6 = d.regs.data() + kRegFetchConstants + tb.fetch_constant * 6;
          uint32_t swz = (fc6[3] >> 1) & 0xFFF;
          uint8_t signs = 0;
          for (uint32_t c = 0; c < 4; ++c) {
            uint32_t sel = (swz >> (3 * c)) & 7;
            uint32_t s = sel < 4 ? (fc6[0] >> (2 + 2 * sel)) & 3 : 0;
            signs |= uint8_t(s << (2 * c));
          }
          uint32_t ti = tb.fetch_constant;
          sys.texture_swizzled_signs[ti >> 2] &=
              ~(uint32_t(0xFF) << (8 * (ti & 3)));
          sys.texture_swizzled_signs[ti >> 2] |= uint32_t(signs) << (8 * (ti & 3));
          if (getenv("REPLAY_SIGN_LOG") && di == size_t(atol(getenv("REPLAY_SIGN_LOG"))))
            std::fprintf(stderr,
                         "draw %zu fc%u: swz=%03X signs=%02X dw0=%08X dw1=%08X dw2=%08X "
                         "dw3=%08X (fmt=%u base=%08X %ux%u)\n",
                         di, ti, swz, signs, fc6[0], fc6[1], fc6[2], fc6[3], fc6[1] & 0x3F,
                         ((fc6[1] >> 12) & 0xFFFFF) << 12, (fc6[2] & 0x1FFF) + 1,
                         ((fc6[2] >> 13) & 0x1FFF) + 1);
        }
      };
      fill_signs(vs_it->second.tex_bindings);
      fill_signs(ps_it->second.tex_bindings);
    }
    std::memcpy(base + kSysOff, &sys, sizeof(sys));
    auto pack_floats = [&](uint8_t* dst, const uint64_t* bitmap, bool dynamic,
                           uint32_t reg_base) {
      const uint32_t* src = d.regs.data() + reg_base;
      if (dynamic) { std::memcpy(dst, src, 4096); return; }
      uint32_t out_vec = 0;
      for (uint32_t block = 0; block < 4; ++block) {
        uint64_t bits = bitmap[block];
        while (bits) {
          uint32_t bit = uint32_t(__builtin_ctzll(bits));
          bits &= bits - 1;
          uint32_t vec = block * 64 + bit;
          std::memcpy(dst + out_vec * 16, src + vec * 4, 16);
          ++out_vec;
        }
      }
    };
    pack_floats(base + kFvOff, vs_it->second.float_bitmap, vs_it->second.float_dynamic,
                kRegFloatConstants);
    pack_floats(base + kFpOff, ps_it->second.float_bitmap, ps_it->second.float_dynamic,
                kRegFloatConstants + 1024);
    std::memcpy(base + kBlOff, d.regs.data() + kRegBoolLoop, 160);
    if (getenv("REPLAY_BOOL_ONES")) std::memset(base + kBlOff, 0xFF, 32);
    std::memcpy(base + kFetchOff, d.regs.data() + kRegFetchConstants, 768);

    VkDescriptorSet ds1 = alloc_set(set1);
    VkDescriptorBufferInfo bis[5] = {
        {const_buf, di * kStride + kSysOff, sizeof(Sys)},
        {const_buf, di * kStride + kFvOff, 4096},
        {const_buf, di * kStride + kFpOff, 4096},
        {const_buf, di * kStride + kBlOff, 160},
        {const_buf, di * kStride + kFetchOff, 768}};
    VkWriteDescriptorSet ws[5];
    for (uint32_t i = 0; i < 5; ++i) {
      ws[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      ws[i].dstSet = ds1; ws[i].dstBinding = i; ws[i].descriptorCount = 1;
      ws[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; ws[i].pBufferInfo = &bis[i];
    }
    vkUpdateDescriptorSets(dev, 5, ws, 0, nullptr);

    // Indices.
    Encoded e = {};
    uint32_t num = uint32_t(init.num_indices);
    if (init.source_select == xenos::SourceSelect::kDMA) {
      uint32_t addr = (d.index_base & ~0x3u) & (kGuestSpace - 1);
      bool is32 = init.index_size == xenos::IndexFormat::kInt32;
      e.indexed = true; e.index_offset = index_cursor;
      e.index_type = is32 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
      const uint8_t* src = guest.data() + addr;
      if (is32) {
        for (uint32_t i = 0; i < num; ++i)
          reinterpret_cast<uint32_t*>(imap + index_cursor)[i] = be32(src + i * 4);
        index_cursor += num * 4;
      } else {
        for (uint32_t i = 0; i < num; ++i)
          reinterpret_cast<uint16_t*>(imap + index_cursor)[i] =
              uint16_t(src[i * 2] << 8 | src[i * 2 + 1]);
        index_cursor += num * 2;
      }
      index_cursor = (index_cursor + 3) & ~3u;
    }
    e.count = num;

    // Pipeline, cached on (vs, ps, prim, depth reg, blend reg, cull reg).
    auto depthctl = reinterpret_cast<const reg::RB_DEPTHCONTROL&>(
        d.regs[reg::RB_DEPTHCONTROL::register_index]);
    uint32_t blend_raw = d.regs[reg::RB_BLENDCONTROL::register_index];
    auto sumode = reinterpret_cast<const reg::PA_SU_SC_MODE_CNTL&>(
        d.regs[reg::PA_SU_SC_MODE_CNTL::register_index]);
    uint64_t pkey = (point_list ? 0xC2B2AE3D27D4EB4Full : 0) ^ (quad_list ? 0x165667B19E3779F9ull : 0) ^
                    (uint64_t(d.regs[0x210C]) << 21) ^
                    (uint64_t(d.regs[0x210D]) * 0xB5297A4Du) ^
                    (uint64_t(d.regs[0x210C]) * 0x68E31DA4u) ^
                    (uint64_t(d.regs[0x2201]) * 0x9E3779B1u) ^
                    (uint64_t(d.regs[0x2209]) * 0x85EBCA77u) ^
                    (uint64_t(d.regs[0x220A]) * 0xC2B2AE3Du) ^
                    (uint64_t(d.regs[0x220B]) * 0x27D4EB2Fu) ^
                    (uint64_t(d.regs[0x2104] & 0xFFFF) << 44) ^
                    (rect_list ? 0x9E3779B97F4A7C15ull : 0) ^
                    d.vs_hash ^ (d.ps_hash << 1) ^ (uint64_t(prim_it->second) << 40) ^
                    (uint64_t(d.regs[reg::RB_DEPTHCONTROL::register_index]) << 8) ^
                    (uint64_t(blend_raw) << 24) ^
                    (uint64_t(d.regs[reg::PA_SU_SC_MODE_CNTL::register_index] & 7) << 56);
    if (getenv("REPLAY_PIPE_LOG"))
      std::fprintf(stderr, "pipe draw=%zu vs=%s ps=%s rect=%d prim=%u\n", di,
                   hash_str(d.vs_hash).c_str(), hash_str(d.ps_hash).c_str(), int(rect_list),
                   uint32_t(init.prim_type));
    VkPipeline pipe;
    auto pc_it = pipeline_cache.find(pkey);
    if (pc_it != pipeline_cache.end()) {
      pipe = pc_it->second;
    } else {
      VkPipelineShaderStageCreateInfo stages[3] = {};
      uint32_t stage_n = 0;
      stages[stage_n] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
      stages[stage_n].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[stage_n].module = vsm;
      stages[stage_n].pName = "main"; ++stage_n;
      if (rect_list || point_list || quad_list) {
        VkShaderModule gsm = get_prim_gs(vs_it->second.modification,
                                         rect_list ? 2u : point_list ? 1u : 3u);
        if (gsm) {
          stages[stage_n] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
          stages[stage_n].stage = VK_SHADER_STAGE_GEOMETRY_BIT; stages[stage_n].module = gsm;
          stages[stage_n].pName = "main"; ++stage_n;
        }
      }
      static VkShaderModule debug_ps = VK_NULL_HANDLE;
      if (getenv("REPLAY_DEBUG_PS") && debug_ps == VK_NULL_HANDLE) {
        auto dw = ReadWords(spv_dir / "debug_uv.ps.spv");
        VkShaderModuleCreateInfo dmi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        dmi.codeSize = dw.size() * 4; dmi.pCode = dw.data();
        vkCreateShaderModule(dev, &dmi, nullptr, &debug_ps);
      }
      stages[stage_n] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
      stages[stage_n].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
      stages[stage_n].module = getenv("REPLAY_DEBUG_PS") ? debug_ps : psm;
      stages[stage_n].pName = "main"; ++stage_n;
      VkPipelineVertexInputStateCreateInfo vin = {
          VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
      VkPipelineInputAssemblyStateCreateInfo ia = {
          VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
      ia.topology = prim_it->second;
      ia.primitiveRestartEnable =
          (ia.topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP ||
           ia.topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN ||
           ia.topology == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP) ? VK_TRUE : VK_FALSE;
      VkPipelineViewportStateCreateInfo vp = {
          VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
      vp.viewportCount = 1; vp.scissorCount = 1;
      VkPipelineRasterizationStateCreateInfo rs = {
          VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
      rs.polygonMode = VK_POLYGON_MODE_FILL;
      rs.cullMode = getenv("REPLAY_NO_CULL") ? 0u
                    : (sumode.cull_front ? VK_CULL_MODE_FRONT_BIT : 0u) |
                      (sumode.cull_back ? VK_CULL_MODE_BACK_BIT : 0u);
      rs.frontFace = sumode.face ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
      rs.lineWidth = 1.0f;
      VkPipelineMultisampleStateCreateInfo ms = {
          VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
      ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
      VkPipelineDepthStencilStateCreateInfo dsst = {
          VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
      dsst.depthTestEnable = (depthctl.z_enable && !getenv("REPLAY_NO_DEPTH")) ? VK_TRUE : VK_FALSE;
      // Stencil: the cel shading builds shadow/ink masks with stencil-only
      // draws (color mask 0, z off) and the light passes test against them.
      // Without this the light layer floods the whole frame.
      dsst.stencilTestEnable = depthctl.stencil_enable ? VK_TRUE : VK_FALSE;
      if (depthctl.stencil_enable) {
        uint32_t srm = d.regs[0x210D];   // RB_STENCILREFMASK: ref|mask|write
        uint32_t srm_bf = d.regs[0x210C];
        auto face = [&](bool bf) {
          VkStencilOpState st = {};
          uint32_t raw = d.regs[reg::RB_DEPTHCONTROL::register_index];
          uint32_t fn  = bf ? (raw >> 20) & 7 : (raw >> 8) & 7;
          uint32_t fl  = bf ? (raw >> 23) & 7 : (raw >> 11) & 7;
          uint32_t zp  = bf ? (raw >> 26) & 7 : (raw >> 14) & 7;
          uint32_t zf  = bf ? (raw >> 29) & 7 : (raw >> 17) & 7;
          uint32_t rm  = (bf && depthctl.backface_enable) ? srm_bf : srm;
          st.compareOp = VkCompareOp(fn);
          st.failOp = VkStencilOp(fl);
          st.passOp = VkStencilOp(zp);
          st.depthFailOp = VkStencilOp(zf);
          st.reference = rm & 0xFF;
          st.compareMask = (rm >> 8) & 0xFF;
          st.writeMask = (rm >> 16) & 0xFF;
          return st;
        };
        dsst.front = face(false);
        dsst.back = face(depthctl.backface_enable != 0);
      }
      dsst.depthWriteEnable = depthctl.z_write_enable ? VK_TRUE : VK_FALSE;
      dsst.depthCompareOp = VkCompareOp(uint32_t(depthctl.zfunc));
      uint32_t stencil_refmask = d.regs[0x210C];  // RB_STENCILREFMASK
      dsst.stencilTestEnable = depthctl.stencil_enable ? VK_TRUE : VK_FALSE;
      if (depthctl.stencil_enable) {
        VkStencilOpState sf = {};
        sf.failOp = VkStencilOp(uint32_t(depthctl.stencilfail));
        sf.passOp = VkStencilOp(uint32_t(depthctl.stencilzpass));
        sf.depthFailOp = VkStencilOp(uint32_t(depthctl.stencilzfail));
        sf.compareOp = VkCompareOp(uint32_t(depthctl.stencilfunc));
        sf.reference = stencil_refmask & 0xFF;
        sf.compareMask = (stencil_refmask >> 8) & 0xFF;
        sf.writeMask = (stencil_refmask >> 16) & 0xFF;
        dsst.front = sf;
        VkStencilOpState sb = sf;
        if (depthctl.backface_enable) {
          sb.failOp = VkStencilOp(uint32_t(depthctl.stencilfail_bf));
          sb.passOp = VkStencilOp(uint32_t(depthctl.stencilzpass_bf));
          sb.depthFailOp = VkStencilOp(uint32_t(depthctl.stencilzfail_bf));
          sb.compareOp = VkCompareOp(uint32_t(depthctl.stencilfunc_bf));
        }
        dsst.back = sb;
      }
      VkPipelineColorBlendAttachmentState batt[4] = {};
      VkFormat cfmts[4] = {color_fmt, color_fmt, color_fmt, color_fmt};
      uint32_t color_mask = d.regs[0x2104];  // RB_COLOR_MASK, one nibble per RT
      for (int i = 0; i < 4; ++i) batt[i].colorWriteMask = (color_mask >> (4 * i)) & 0xF;
      if (getenv("REPLAY_MASK_LOG"))
        std::fprintf(stderr, "pipe draw=%zu mask_reg=%08X applied=%X%X%X%X\n", di, color_mask,
                     batt[0].colorWriteMask, batt[1].colorWriteMask, batt[2].colorWriteMask,
                     batt[3].colorWriteMask);
      {
        uint32_t sc = blend_raw & 0x1F ? blend_raw & 0x1F : 1;  // src factor (5 bits)
        // Xenos RB_BLENDCONTROL: srcblend 0:4, blendop 5:7, dstblend 8:12,
        // srcblend_alpha 16:20, blendop_alpha 21:23, dstblend_alpha 24:28.
        uint32_t src_c = blend_raw & 0x1F, op_c = (blend_raw >> 5) & 7,
                 dst_c = (blend_raw >> 8) & 0x1F;
        uint32_t src_a = (blend_raw >> 16) & 0x1F, op_a = (blend_raw >> 21) & 7,
                 dst_a = (blend_raw >> 24) & 0x1F;
        // Xenos blend factor codes are NOT Vulkan's: kSrcColor is 4 where
        // Vulkan's SRC_COLOR is 2, kDstColor is 8 where Vulkan's DST_COLOR is
        // 4, and so on. A direct cast turned every multiplicative cel-shade
        // blend into nonsense (dst_color became dst_alpha), which flattened
        // the whole lighting composition into saturated constants.
        auto xe_blend = [](uint32_t f) -> VkBlendFactor {
          switch (f) {
            case 0: return VK_BLEND_FACTOR_ZERO;
            case 1: return VK_BLEND_FACTOR_ONE;
            case 4: return VK_BLEND_FACTOR_SRC_COLOR;
            case 5: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
            case 6: return VK_BLEND_FACTOR_SRC_ALPHA;
            case 7: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            case 8: return VK_BLEND_FACTOR_DST_COLOR;
            case 9: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
            case 10: return VK_BLEND_FACTOR_DST_ALPHA;
            case 11: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
            case 12: return VK_BLEND_FACTOR_CONSTANT_COLOR;
            case 13: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
            case 14: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
            case 15: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
            case 16: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
            default: return VK_BLEND_FACTOR_ONE;
          }
        };
        static const VkBlendOp kOps[] = {VK_BLEND_OP_ADD, VK_BLEND_OP_SUBTRACT, VK_BLEND_OP_MIN,
                                         VK_BLEND_OP_MAX, VK_BLEND_OP_REVERSE_SUBTRACT};
        // Each render target has its own blend control register
        // (RB_BLENDCONTROL0 at 0x2201, 1..3 at 0x2209-0x220B) - the cel
        // passes rely on different blends per target.
        static const uint32_t kBlendRegs[4] = {0x2201, 0x2209, 0x220A, 0x220B};
        for (int i = 0; i < 4; ++i) {
          uint32_t braw = d.regs[kBlendRegs[i]];
          uint32_t bsrc_c = braw & 0x1F, bop_c = (braw >> 5) & 7, bdst_c = (braw >> 8) & 0x1F;
          uint32_t bsrc_a = (braw >> 16) & 0x1F, bop_a = (braw >> 21) & 7,
                   bdst_a = (braw >> 24) & 0x1F;
          bool noop = bsrc_c == 1 && bdst_c == 0 && bop_c == 0 && bsrc_a == 1 && bdst_a == 0 &&
                      bop_a == 0;
          if (noop) continue;
          batt[i].blendEnable = VK_TRUE;
          batt[i].srcColorBlendFactor = xe_blend(bsrc_c);
          batt[i].dstColorBlendFactor = xe_blend(bdst_c);
          batt[i].colorBlendOp = kOps[bop_c < 5 ? bop_c : 0];
          batt[i].srcAlphaBlendFactor = xe_blend(bsrc_a);
          batt[i].dstAlphaBlendFactor = xe_blend(bdst_a);
          batt[i].alphaBlendOp = kOps[bop_a < 5 ? bop_a : 0];
        }
        (void)src_c; (void)dst_c; (void)op_c; (void)src_a; (void)dst_a; (void)op_a;
      }
      VkPipelineColorBlendStateCreateInfo bl = {
          VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
      bl.attachmentCount = 4; bl.pAttachments = batt;
      VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
      VkPipelineDynamicStateCreateInfo dync = {
          VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
      dync.dynamicStateCount = 2; dync.pDynamicStates = dyn;
      VkPipelineRenderingCreateInfo ri = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
      ri.colorAttachmentCount = 4; ri.pColorAttachmentFormats = cfmts;
      ri.depthAttachmentFormat = depth_fmt; ri.stencilAttachmentFormat = depth_fmt;
      VkGraphicsPipelineCreateInfo pci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
      pci.pNext = &ri; pci.stageCount = stage_n; pci.pStages = stages;
      pci.pVertexInputState = &vin; pci.pInputAssemblyState = &ia; pci.pViewportState = &vp;
      pci.pRasterizationState = &rs; pci.pMultisampleState = &ms;
      pci.pDepthStencilState = &dsst; pci.pColorBlendState = &bl; pci.pDynamicState = &dync;
      pci.layout = pipe_layouts.at(ps_it->second.tex_sig);
      if (vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &pci, nullptr, &pipe) !=
          VK_SUCCESS) { ++skip_shader; continue; }
      pipeline_cache[pkey] = pipe;
    }

    draw_util::Scissor sc;
    draw_util::GetScissor(rf, sc);
    if (di < 3 || di == 500 ||
        (getenv("REPLAY_INSPECT") && di == size_t(atoi(getenv("REPLAY_INSPECT")))))
      std::printf("draw %zu: vp off(%u,%u) ext(%u,%u) z(%f,%f) ndc_s(%f,%f,%f) ndc_o(%f,%f,%f) flags=%X sc(%u,%u %ux%u)\n",
                  di, vpi.xy_offset[0], vpi.xy_offset[1], vpi.xy_extent[0], vpi.xy_extent[1],
                  vpi.z_min, vpi.z_max, vpi.ndc_scale[0], vpi.ndc_scale[1], vpi.ndc_scale[2],
                  vpi.ndc_offset[0], vpi.ndc_offset[1], vpi.ndc_offset[2], sys.flags,
                  sc.offset[0], sc.offset[1], sc.extent[0], sc.extent[1]);
    e.viewport = {float(vpi.xy_offset[0]), float(vpi.xy_offset[1]),
                  float(vpi.xy_extent[0] ? vpi.xy_extent[0] : 1),
                  float(vpi.xy_extent[1] ? vpi.xy_extent[1] : 1), vpi.z_min, vpi.z_max};
    e.scissor = {{int32_t(sc.offset[0]), int32_t(sc.offset[1])},
                 {sc.extent[0] ? sc.extent[0] : 1, sc.extent[1] ? sc.extent[1] : 1}};
    if (getenv("REPLAY_INSPECT") && di == size_t(atoi(getenv("REPLAY_INSPECT")))) {
      std::printf("== draw %zu: vs=%s ps=%s prim=%u num=%u src_sel=%u\n", di,
                  hash_str(d.vs_hash).c_str(), hash_str(d.ps_hash).c_str(),
                  uint32_t(init.prim_type), uint32_t(init.num_indices),
                  uint32_t(init.source_select));
      std::printf("  indx_offset=%08X min_vtx=%08X max_vtx=%08X multi_prim_reset=%08X\n",
                  d.regs[0x2102], d.regs[0x2101], d.regs[0x2100], d.regs[0x2103]);  // VGT_INDX_OFFSET
      std::printf("  boolloop: %08X %08X %08X %08X | loops %08X %08X\n",
                  d.regs[kRegBoolLoop], d.regs[kRegBoolLoop+1], d.regs[kRegBoolLoop+2],
                  d.regs[kRegBoolLoop+3], d.regs[kRegBoolLoop+8], d.regs[kRegBoolLoop+9]);
      for (uint32_t fc = 0; fc < 96; ++fc) {
        {
          uint32_t d0chk = d.regs[kRegFetchConstants + fc*2];
          if ((d0chk & 3) != 3) continue;  // only kVertex-typed slots
        }
        uint32_t d0 = d.regs[kRegFetchConstants + fc*2], d1 = d.regs[kRegFetchConstants + fc*2+1];
        uint32_t type = d0 & 3, addr_dw = d0 >> 2, endian = d1 & 3, size_w = (d1 >> 2) & 0xFFFFFF;
        std::printf("  vfetch[%u]: type=%u addr=0x%08X(dw) bytes=0x%08X endian=%u size=%u words\n",
                    fc, type, addr_dw, addr_dw*4, endian, size_w);
        if (type == 3 || (type != 0 && size_w)) {
          uint32_t byte_addr = (addr_dw * 4) & (kGuestSpace - 1);
          std::printf("    mem raw: ");
          for (int k = 0; k < 8; ++k) std::printf("%02X%02X%02X%02X ",
              guest[byte_addr+k*4], guest[byte_addr+k*4+1], guest[byte_addr+k*4+2], guest[byte_addr+k*4+3]);
          std::printf("\n    as BE floats: ");
          for (int k = 0; k < 6; ++k) {
            uint32_t v = be32(guest.data() + byte_addr + k*4);
            float fv; std::memcpy(&fv, &v, 4);
            std::printf("%g ", fv);
          }
          std::printf("\n");
        }
      }
      std::printf("  sq_program_cntl=%08X sq_context_misc=%08X\n",
                  d.regs[reg::SQ_PROGRAM_CNTL::register_index],
                  d.regs[reg::SQ_CONTEXT_MISC::register_index]);
      std::printf("  first floatVS packed vec4s: ");
      const float* pf = reinterpret_cast<const float*>(base + kFvOff);
      for (int k = 0; k < 36; ++k) std::printf("%g%s", pf[k], (k % 4 == 3) ? " | " : " ");
      std::printf("\n  first floatPS packed vec4s: ");
      const float* pp = reinterpret_cast<const float*>(base + kFpOff);
      for (int k = 0; k < 48; ++k) std::printf("%g%s", pp[k], (k % 4 == 3) ? " | " : " ");
      std::printf("\n");
      if (e.indexed) {
        std::printf("  first indices: ");
        const uint16_t* ip = reinterpret_cast<const uint16_t*>(imap + e.index_offset);
        for (int k = 0; k < 8; ++k) std::printf("%u ", ip[k]);
        std::printf("\n");
      }
    }
    // Key by EDRAM base only: RB_COLOR_INFO carries base and format, and the
    // game reinterprets the same EDRAM allocation under different formats
    // between passes (2_10_10_10 world, AS_10_10_10_10 lighting). Splitting
    // by full value gave each pass its own image and the frame never
    // composed - the world pass sat unseen under a separate flat light pass.
    e.surface_key = d.regs[reg::RB_COLOR_INFO::register_index] & 0xFFF;
    e.depth_key = d.regs[reg::RB_DEPTH_INFO::register_index];
    e.pipe = pipe;
    e.layout = pipe_layouts.at(ps_it->second.tex_sig);
    e.ds1 = ds1;
    e.has_tex = !ps_it->second.tex_sig.empty();
    e.ds3 = VK_NULL_HANDLE;
    if (e.has_tex) {
      // Resolve this draw's textures from its fetch constants and build (or
      // reuse) a descriptor set binding them. Image bindings come in pairs
      // per texture (unsigned/signed views), so binding N maps to texture
      // N/2 of the shader's texture list.
      // binding -> view via the fetch constant named in the SPIR-V variable.
      std::map<uint32_t, VkImageView> views;
      uint64_t set_key = XXH3_64bits(ps_it->second.tex_sig.data(),
                                     ps_it->second.tex_sig.size());
      for (const auto& [binding, fc] : ps_it->second.image_binding_fc) {
        const uint32_t* fc6 = d.regs.data() + kRegFetchConstants + fc * 6;
        views[binding] = upload_texture(fc6);
        set_key ^= XXH3_64bits(fc6, 24) * (binding * 2 + 31);
      }
      static std::map<uint64_t, VkDescriptorSet> draw_tex_sets;
      auto ts_it = draw_tex_sets.find(set_key);
      if (ts_it != draw_tex_sets.end()) {
        e.ds3 = ts_it->second;
      } else {
        VkDescriptorSet s3 = alloc_set(tex_layouts.at(ps_it->second.tex_sig));
        for (auto [binding, is_image] : parse_sig(ps_it->second.tex_sig)) {
          VkDescriptorImageInfo ii = {};
          if (is_image) {
            auto vit = views.find(binding);
            ii.imageView = (vit != views.end() && vit->second) ? vit->second : dummy_view;
            ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
          } else {
            ii.sampler = sampler;
            auto sfc = ps_it->second.sampler_binding_fc.find(binding);
            if (sfc != ps_it->second.sampler_binding_fc.end()) {
              const uint32_t* fc6 =
                  d.regs.data() + kRegFetchConstants + sfc->second * 6;
              ii.sampler = get_sampler(fc6);
            }
          }
          VkWriteDescriptorSet w2 = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
          w2.dstSet = s3; w2.dstBinding = binding; w2.descriptorCount = 1;
          w2.descriptorType = is_image ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE
                                       : VK_DESCRIPTOR_TYPE_SAMPLER;
          w2.pImageInfo = &ii;
          vkUpdateDescriptorSets(dev, 1, &w2, 0, nullptr);
        }
        draw_tex_sets[set_key] = s3;
        e.ds3 = s3;
      }
    }
    if (getenv("REPLAY_RECT_LOG") && rect_list) {
      std::fprintf(stderr, "rect draw=%zu ps=%s num=%u color_info=%08X ", di,
                   hash_str(d.ps_hash).c_str(), uint32_t(init.num_indices),
                   d.regs[reg::RB_COLOR_INFO::register_index]);
      for (const auto& tb : ps_it->second.tex_bindings) {
        const uint32_t* fc6 = d.regs.data() + kRegFetchConstants + tb.fetch_constant * 6;
        std::fprintf(stderr, "tex(fc%u fmt%u %ux%u) ", tb.fetch_constant, fc6[1] & 0x3F,
                     (fc6[2] & 0x1FFF) + 1, ((fc6[2] >> 13) & 0x1FFF) + 1);
      }
      std::fprintf(stderr, "\n");
    }
    if (getenv("REPLAY_VFETCH_CENSUS")) {
      // A draw whose every vertex stream reads zeroed memory is a no-op here
      // but may paint in the reference - count them to bound the trace gap.
      static size_t zero_draws = 0, census_draws = 0;
      bool any_stream = false, any_nonzero = false;
      for (uint32_t slot = 0; slot < 96; ++slot) {
        const uint32_t* fc = d.regs.data() + kRegFetchConstants + slot * 2;
        if ((fc[0] & 3) != 3) continue;
        uint32_t addr = (fc[0] & ~3u) & (kGuestSpace - 1);
        uint32_t size_words = fc[1] & 0xFFFFFF;
        if (!addr || !size_words || size_words > (64u << 20)) continue;
        any_stream = true;
        uint32_t check = std::min<uint32_t>(size_words * 4, 256);
        for (uint32_t b = 0; b < check && addr + b < kGuestSpace; ++b) {
          if (guest[addr + b]) { any_nonzero = true; break; }
        }
        if (any_nonzero) break;
      }
      ++census_draws;
      if (any_stream && !any_nonzero) {
        ++zero_draws;
        std::fprintf(stderr, "zero-stream draw=%zu enc=%zu vs=%s ps=%s num=%u\n", di,
                     encoded.size(), hash_str(d.vs_hash).c_str(), hash_str(d.ps_hash).c_str(),
                     uint32_t(init.num_indices));
      }
      if (census_draws == draws.size())
        std::fprintf(stderr, "census: %zu of %zu draws have all-zero vertex streams\n",
                     zero_draws, census_draws);
    }
    long enc_lo = getenv("REPLAY_ENC_FROM") ? atol(getenv("REPLAY_ENC_FROM")) : 940;
    if (getenv("REPLAY_ENC_LOG") && long(encoded.size()) >= enc_lo)
      std::fprintf(stderr,
                   "enc %zu = draw %zu vs=%s ps=%s rect=%d blend=%08X colorctl=%08X mask=%08X "
                   "cinfo=%08X depthctl=%08X sinfo=%08X\n",
                   encoded.size(), di, hash_str(d.vs_hash).c_str(), hash_str(d.ps_hash).c_str(),
                   int(rect_list), d.regs[reg::RB_BLENDCONTROL::register_index],
                   d.regs[reg::RB_COLORCONTROL::register_index],
                   d.regs[reg::RB_COLOR_MASK::register_index],
                   d.regs[reg::RB_COLOR_INFO::register_index],
                   d.regs[reg::RB_DEPTHCONTROL::register_index],
                   d.regs[reg::RB_SURFACE_INFO::register_index]);
    encoded.push_back(e);
  }
  if (getenv("REPLAY_TEXSURVEY")) {
    std::map<uint64_t, uint32_t> unique_tex;
    for (const DrawCall& d : draws) {
      auto ps2 = shaders.find(hash_str(d.ps_hash) + "_ps");
      if (ps2 == shaders.end()) continue;
      for (const auto& tb : ps2->second.tex_bindings) {
        const uint32_t* fc6 = d.regs.data() + kRegFetchConstants + tb.fetch_constant * 6;
        uint64_t key = XXH3_64bits(fc6, 24);
        if (unique_tex.count(key)) continue;
        uint32_t fmt = fc6[1] & 0x3F, endian2 = (fc6[1] >> 6) & 3, tiled = fc6[0] >> 31;
        uint32_t base = (fc6[1] >> 12) << 12;
        uint32_t wq = (fc6[2] & 0x1FFF) + 1, hq = ((fc6[2] >> 13) & 0x1FFF) + 1;
        unique_tex[key] = 1;
        std::printf("tex fc=%u fmt=%u endian=%u tiled=%u base=0x%08X %ux%u\n",
                    tb.fetch_constant, fmt, endian2, tiled, base, wq, hq);
      }
    }
    std::printf("unique textures: %zu\n", unique_tex.size());
  }
  std::printf("modification pairs: %zu exact, %zu fallback\n", pair_hits, pair_misses);
  if (!needed_pairs.empty()) {
    std::ofstream np(spv_dir / "needed_pairs.csv");
    np << "vs_hash,vs_modification,ps_hash,ps_modification\n";
    for (const std::string& line : needed_pairs) np << line << "\n";
    std::printf("wrote %zu needed modification pairs -> needed_pairs.csv "
                "(feed to aot_shaders and re-run)\n", needed_pairs.size());
  }
  std::printf("encoded %zu draws (%zu unique pipelines; skipped: %zu shader, %zu primitive, "
              "%zu offscreen, %zu copy, %zu post-resolve)\n",
              encoded.size(), pipeline_cache.size(), skip_shader, skip_prim, skip_offscreen,
              skip_copy, skip_postresolve);

  // ------------------------------------------------- record + submit
  VkCommandPoolCreateInfo cpci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  VkCommandPool cpool; vkCreateCommandPool(dev, &cpci, nullptr, &cpool);
  VkCommandBufferAllocateInfo cbai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  cbai.commandPool = cpool; cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cbai.commandBufferCount = 1;
  VkCommandBuffer cb; vkAllocateCommandBuffers(dev, &cbai, &cb);
  VkQueryPoolCreateInfo qpci = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
  qpci.queryType = VK_QUERY_TYPE_PIPELINE_STATISTICS;
  qpci.queryCount = 1;
  qpci.pipelineStatistics =
      VK_QUERY_PIPELINE_STATISTIC_VERTEX_SHADER_INVOCATIONS_BIT |
      VK_QUERY_PIPELINE_STATISTIC_CLIPPING_INVOCATIONS_BIT |
      VK_QUERY_PIPELINE_STATISTIC_CLIPPING_PRIMITIVES_BIT |
      VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT;
  VkQueryPool qpool; vkCreateQueryPool(dev, &qpci, nullptr, &qpool);
  VkCommandBufferBeginInfo cbbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  vkBeginCommandBuffer(cb, &cbbi);
  vkCmdResetQueryPool(cb, qpool, 0, 1);
  vkCmdBeginQuery(cb, qpool, 0, 0);

  auto barrier = [&](VkImage img, VkImageAspectFlags aspect, VkImageLayout from,
                     VkImageLayout to) {
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.oldLayout = from; b.newLayout = to; b.image = img;
    b.subresourceRange = {aspect, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
  };
  for (const Encoded& e0 : encoded) get_surface(e0.surface_key);
  for (auto& [sk, sf] : surfaces) {
    for (int i = 0; i < 4; ++i)
      barrier(sf.color_img[i], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
              VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    barrier(sf.depth_img, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
  }
  barrier(dummy_img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  for (size_t ti = 0; ti < pending_copies.size(); ++ti) {
    barrier(pending_copies[ti].second, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    vkCmdCopyBufferToImage(cb, pending_copies[ti].first, pending_copies[ti].second,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &pending_regions[ti]);
    barrier(pending_copies[ti].second, VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  }
  std::printf("uploaded %zu textures\n", pending_copies.size());

  bool next_begin_clears_depth = false;
  auto begin_surface = [&](Surface& sf) {
    static VkRenderingAttachmentInfo catt[4];
    static VkRenderingAttachmentInfo datt;
    for (int i = 0; i < 4; ++i) {
      catt[i] = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
      catt[i].imageView = sf.color_view[i];
      catt[i].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
      catt[i].loadOp = sf.cleared ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR;
      catt[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      catt[i].clearValue.color = {{0.05f, 0.05f, 0.08f, 1.0f}};
    }
    datt = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    datt.imageView = sf.depth_view;
    datt.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    datt.loadOp = (sf.cleared && !next_begin_clears_depth) ? VK_ATTACHMENT_LOAD_OP_LOAD
                                                            : VK_ATTACHMENT_LOAD_OP_CLEAR;
    datt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    datt.clearValue.depthStencil = {0.0f, 0};
    sf.cleared = true;
    next_begin_clears_depth = false;
    VkRenderingInfo rinfo = {VK_STRUCTURE_TYPE_RENDERING_INFO};
    rinfo.renderArea = {{0, 0}, {kWidth, kHeight}};
    rinfo.layerCount = 1; rinfo.colorAttachmentCount = 4;
    rinfo.pColorAttachments = catt; rinfo.pDepthAttachment = &datt;
    rinfo.pStencilAttachment = &datt;
    vkCmdBeginRendering(cb, &rinfo);
  };
  uint32_t current_surface = encoded.empty() ? 0 : encoded[0].surface_key;
  begin_surface(get_surface(current_surface));
  // Sanity probe: REPLAY_TRIANGLE=1 draws one hardcoded clip-space triangle
  // through this exact attachment/readback path, proving everything outside
  // the translated shaders. No descriptors, no vertex data.
  if (getenv("REPLAY_TRIANGLE")) {
    auto tvw = ReadWords(spv_dir / "debug_tri.vs.spv");
    auto tpw = ReadWords(spv_dir / "debug_tri.ps.spv");
    VkShaderModuleCreateInfo mi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    VkShaderModule tvm, tpm;
    mi.codeSize = tvw.size() * 4; mi.pCode = tvw.data();
    vkCreateShaderModule(dev, &mi, nullptr, &tvm);
    mi.codeSize = tpw.size() * 4; mi.pCode = tpw.data();
    vkCreateShaderModule(dev, &mi, nullptr, &tpm);
    VkPipelineShaderStageCreateInfo st[2] = {};
    st[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = tvm; st[0].pName = "main";
    st[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = tpm; st[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vin = {
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp2 = {
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp2.viewportCount = 1; vp2.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs2 = {
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs2.polygonMode = VK_POLYGON_MODE_FILL; rs2.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms2 = {
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms2.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds2 = {
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState ba[4] = {};
    for (int i = 0; i < 4; ++i) ba[i].colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo bl2 = {
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    bl2.attachmentCount = 4; bl2.pAttachments = ba;
    VkDynamicState dyn2[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy2 = {
        VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy2.dynamicStateCount = 2; dy2.pDynamicStates = dyn2;
    VkFormat cf2[4] = {color_fmt, color_fmt, color_fmt, color_fmt};
    VkPipelineRenderingCreateInfo ri2 = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    ri2.colorAttachmentCount = 4; ri2.pColorAttachmentFormats = cf2;
    ri2.depthAttachmentFormat = depth_fmt; ri2.stencilAttachmentFormat = depth_fmt;
    VkGraphicsPipelineCreateInfo pc2 = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pc2.pNext = &ri2; pc2.stageCount = 2; pc2.pStages = st;
    pc2.pVertexInputState = &vin; pc2.pInputAssemblyState = &ia; pc2.pViewportState = &vp2;
    pc2.pRasterizationState = &rs2; pc2.pMultisampleState = &ms2;
    pc2.pDepthStencilState = &ds2; pc2.pColorBlendState = &bl2; pc2.pDynamicState = &dy2;
    pc2.layout = pipe_layouts.at("");
    VkPipeline tp;
    if (vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &pc2, nullptr, &tp) == VK_SUCCESS) {
      VkViewport fv = {0, 0, float(kWidth), float(kHeight), 0, 1};
      VkRect2D fs = {{0, 0}, {kWidth, kHeight}};
      vkCmdSetViewport(cb, 0, 1, &fv); vkCmdSetScissor(cb, 0, 1, &fs);
      vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, tp);
      vkCmdDraw(cb, 3, 1, 0, 0);
      std::printf("sanity triangle drawn\n");
    }
  }
  size_t draw_limit = getenv("REPLAY_DRAWS") ? atoi(getenv("REPLAY_DRAWS")) : encoded.size();
  size_t drawn_n = 0;
  size_t next_resolve = 0;
  bool query_ended = false;
  auto flush_resolves = [&](size_t upto) {
    bool flushed = false;
    while (next_resolve < resolves.size() && resolves[next_resolve].after_encoded <= upto) {
      const Resolve& rz = resolves[next_resolve++];
      uint32_t src_sel = rz.copy_control & 7;
      if (src_sel >= 4) {
        // Depth resolve: the shadow-map path. Read our D32 depth back and
        // store it as the guest's 20e4-in-24-bit format (the same encoding
        // upload_texture's k_24_8_FLOAT path decodes, so the roundtrip is
        // exact for sampling).
        Surface& sfd = get_surface(rz.surface_key);
        vkCmdEndRendering(cb);
        if (!query_ended) { vkCmdEndQuery(cb, qpool, 0); query_ended = true; }
        barrier(sfd.depth_img, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
                VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy dr = {};
        dr.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
        dr.imageOffset = {int32_t(rz.x), int32_t(rz.y), 0};
        dr.imageExtent = {std::min(rz.w, kWidth - rz.x), std::min(rz.h, kHeight - rz.y), 1};
        vkCmdCopyImageToBuffer(cb, sfd.depth_img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               readback_buf, 1, &dr);
        vkEndCommandBuffer(cb);
        VkSubmitInfo dsi = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
        dsi.commandBufferCount = 1; dsi.pCommandBuffers = &cb;
        vkQueueSubmit(queue, 1, &dsi, VK_NULL_HANDLE);
        vkQueueWaitIdle(queue);
        uint32_t pitch_px = rz.dest_pitch & 0x3FFF;
        uint32_t out_w = dr.imageExtent.width, out_h = dr.imageExtent.height;
        const float* dpx = static_cast<const float*>(readback_map);
        uint32_t base = rz.dest_base & (kGuestSpace - 1);
        // Resolve destinations are tiled texture memory: size the region to
        // whole 32-row tiles so the scattered tiled offsets stay in bounds.
        uint32_t tiled_h = (out_h + 31) & ~31u;
        uint64_t dest_len = uint64_t(pitch_px) * tiled_h * 4;
        if (uint64_t(base) + dest_len <= kGuestSpace) {
          for (uint32_t yy = 0; yy < out_h; ++yy) {
            for (uint32_t xx = 0; xx < out_w; ++xx) {
              float dv = dpx[size_t(yy) * out_w + xx];
              uint32_t v = xenos::Float32To20e4(dv, true) << 8;
              uint8_t* dp = guest.data() + base +
                            size_t(texture_util::GetTiledOffset2D(
                                int32_t(xx), int32_t(yy), pitch_px, 2));
              dp[0] = uint8_t(v >> 24); dp[1] = uint8_t(v >> 16);
              dp[2] = uint8_t(v >> 8); dp[3] = uint8_t(v);
            }
          }
          uint32_t sc_base = base, sc_size = uint32_t(dest_len);
          sidecar_out.write(reinterpret_cast<const char*>(&sc_base), 4);
          sidecar_out.write(reinterpret_cast<const char*>(&sc_size), 4);
          sidecar_out.write(reinterpret_cast<const char*>(guest.data() + base),
                            std::streamsize(sc_size));
        }
        vkResetCommandBuffer(cb, 0);
        VkCommandBufferBeginInfo dbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        vkBeginCommandBuffer(cb, &dbi);
        barrier(sfd.depth_img, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        begin_surface(get_surface(current_surface));
        flushed = true;
        continue;
      }
      uint32_t dest_fmt = (rz.dest_info >> 7) & 0x3F;
      Surface& sf = get_surface(rz.surface_key);
      vkCmdEndRendering(cb);
      if (!query_ended) { vkCmdEndQuery(cb, qpool, 0); query_ended = true; }
      barrier(sf.color_img[src_sel], VK_IMAGE_ASPECT_COLOR_BIT,
              VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
      VkBufferImageCopy rr = {};
      rr.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      rr.imageOffset = {int32_t(rz.x), int32_t(rz.y), 0};
      rr.imageExtent = {std::min(rz.w, kWidth - rz.x), std::min(rz.h, kHeight - rz.y), 1};
      vkCmdCopyImageToBuffer(cb, sf.color_img[src_sel], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                             readback_buf, 1, &rr);
      vkEndCommandBuffer(cb);
      VkSubmitInfo fsi = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
      fsi.commandBufferCount = 1; fsi.pCommandBuffers = &cb;
      vkQueueSubmit(queue, 1, &fsi, VK_NULL_HANDLE);
      vkQueueWaitIdle(queue);
      // Convert A2B10G10R10 rows into the guest destination.
      uint32_t pitch_px = rz.dest_pitch & 0x3FFF;
      uint32_t out_w = rr.imageExtent.width, out_h = rr.imageExtent.height;
      const uint32_t* px = static_cast<const uint32_t*>(readback_map);
      uint32_t bpp = 4;
      uint32_t tiled_h = (out_h + 31) & ~31u;
      uint64_t dest_len = uint64_t(pitch_px) * tiled_h * bpp;
      uint32_t base = rz.dest_base & (kGuestSpace - 1);
      if (uint64_t(base) + dest_len <= kGuestSpace) {
        for (uint32_t yy = 0; yy < out_h; ++yy) {
          for (uint32_t xx = 0; xx < std::min(out_w, pitch_px); ++xx) {
            uint32_t hp = px[size_t(yy) * out_w + xx];
            uint32_t r = hp & 0x3FF, g = (hp >> 10) & 0x3FF, b = (hp >> 20) & 0x3FF,
                     a2 = hp >> 30;
            uint32_t v;
            if (rz.dest_info & (1u << 24)) std::swap(r, b);  // copy_dest_swap
            if (dest_fmt == 7 || dest_fmt == 10) {
              v = r | (g << 10) | (b << 20) | (a2 << 30);
            } else {
              // 8_8_8_8: narrow 10 -> 8 bits.
              v = (r >> 2) | ((g >> 2) << 8) | ((b >> 2) << 16) |
                  ((a2 * 85u) << 24);
            }
            uint8_t* dp = guest.data() + base +
                          size_t(texture_util::GetTiledOffset2D(
                              int32_t(xx), int32_t(yy), pitch_px, 2));
            if ((rz.dest_info & 7) == 0) {  // copy_dest_endian: none
              dp[0] = uint8_t(v); dp[1] = uint8_t(v >> 8);
              dp[2] = uint8_t(v >> 16); dp[3] = uint8_t(v >> 24);
            } else {  // 8in32 swap
              dp[0] = uint8_t(v >> 24); dp[1] = uint8_t(v >> 16);
              dp[2] = uint8_t(v >> 8); dp[3] = uint8_t(v);
            }
          }
        }
        uint32_t sc_base = base, sc_size = uint32_t(dest_len);
        sidecar_out.write(reinterpret_cast<const char*>(&sc_base), 4);
        sidecar_out.write(reinterpret_cast<const char*>(&sc_size), 4);
        sidecar_out.write(reinterpret_cast<const char*>(guest.data() + base),
                          std::streamsize(sc_size));
      }
      // Resume: new command buffer, restore layouts, reopen the surface.
      vkResetCommandBuffer(cb, 0);
      VkCommandBufferBeginInfo rbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
      vkBeginCommandBuffer(cb, &rbi);
      barrier(sf.color_img[src_sel], VK_IMAGE_ASPECT_COLOR_BIT,
              VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
      begin_surface(get_surface(current_surface));
      flushed = true;
    }
    return flushed;
  };
  size_t encoded_index = 0;
  for (const Encoded& e : encoded) {
    flush_resolves(encoded_index);
    ++encoded_index;
    if (drawn_n++ >= draw_limit) break;
    static uint32_t current_depth_key = encoded.empty() ? 0 : encoded[0].depth_key;
    bool depth_changed = e.depth_key != current_depth_key;
    if (depth_changed) current_depth_key = e.depth_key;
    if (e.surface_key != current_surface || depth_changed) {
      vkCmdEndRendering(cb);
      current_surface = e.surface_key;
      next_begin_clears_depth = depth_changed;
      begin_surface(get_surface(current_surface));
    }
    vkCmdSetViewport(cb, 0, 1, &e.viewport);
    vkCmdSetScissor(cb, 0, 1, &e.scissor);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, e.pipe);
    VkDescriptorSet dsets[4] = {ds0, e.ds1, VK_NULL_HANDLE, e.ds3};
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, e.layout, 0, 2, dsets, 0,
                            nullptr);
    if (e.has_tex)
      vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, e.layout, 3, 1, &e.ds3, 0,
                              nullptr);
    if (e.indexed) {
      vkCmdBindIndexBuffer(cb, index_buf, e.index_offset, e.index_type);
      vkCmdDrawIndexed(cb, e.count, 1, 0, 0, 0);
    } else {
      vkCmdDraw(cb, e.count, 1, 0, 0);
    }
  }
  // The frame's last resolve has no draw after it - flush it explicitly so
  // the presented front buffer (its destination) exists in guest memory.
  flush_resolves(encoded.size());
  vkCmdEndRendering(cb);
  if (!query_ended) vkCmdEndQuery(cb, qpool, 0);
  uint32_t present_key = primary_color_info & 0xFFF;
  if (getenv("REPLAY_PRESENT"))
    present_key = uint32_t(strtoul(getenv("REPLAY_PRESENT"), nullptr, 16));
  {
    std::printf("surfaces this frame:");
    for (auto& [sk, sf] : surfaces) std::printf(" %08X", sk);
    std::printf("  (presenting %08X)\n", present_key);
  }
  VkImage present_img = get_surface(present_key).color_img[0];
  barrier(present_img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  VkBufferImageCopy region = {};
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.imageExtent = {kWidth, kHeight, 1};
  vkCmdCopyImageToBuffer(cb, present_img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback_buf,
                         1, &region);
  vkEndCommandBuffer(cb);

  auto t0 = std::chrono::steady_clock::now();
  VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.commandBufferCount = 1; si.pCommandBuffers = &cb;
  CHECK_VK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
  CHECK_VK(vkQueueWaitIdle(queue));
  double ms2 = std::chrono::duration<double, std::milli>(
      std::chrono::steady_clock::now() - t0).count();
  std::printf("GPU executed %zu draws in %.2f ms\n", encoded.size(), ms2);
  {
    uint64_t qr[4] = {};
    vkGetQueryPoolResults(dev, qpool, 0, 1, sizeof(qr), qr, sizeof(qr),
                          VK_QUERY_RESULT_64_BIT);
    std::printf("stats: VS invocations=%llu clip_in=%llu clip_out=%llu FS invocations=%llu\n",
                (unsigned long long)qr[0], (unsigned long long)qr[1],
                (unsigned long long)qr[2], (unsigned long long)qr[3]);
  }

  // ------------------------------------------------- write PPM
  {
    std::ofstream out(argv[3], std::ios::binary);
    out << "P6\n" << kWidth << " " << kHeight << "\n255\n";
    // The guest's resolve applies the Xenos piecewise-linear gamma before
    // scan-out; approximate it so brightness is comparable to the reference.
    uint8_t gamma_lut[256];
    for (int i = 0; i < 256; ++i)
      gamma_lut[i] = uint8_t(std::lround(std::pow(i / 255.0, 1.0 / 2.2) * 255.0));
    const uint32_t* px = static_cast<const uint32_t*>(readback_map);
    for (uint32_t i = 0; i < kWidth * kHeight; ++i) {
      uint32_t v = px[i];  // A2B10G10R10: R in the low bits
      out.put(char(gamma_lut[(v & 0x3FF) >> 2]));
      out.put(char(gamma_lut[((v >> 10) & 0x3FF) >> 2]));
      out.put(char(gamma_lut[((v >> 20) & 0x3FF) >> 2]));
    }
  }
  std::printf("frame written to %s\n", argv[3]);
  return 0;
}
