// Draw-list extractor for GPU frame traces - the front half of the native
// command translator.
//
// A frame trace (F10 in-game) is the complete record of one frame: every PM4
// packet, register write and memory read the GPU consumed. This walks that
// stream the same way the emulated command processor does - a register shadow
// updated by packet actions - but instead of drawing, it emits one record per
// draw with the register state that defines it. That list, joined against the
// AOT shader manifest and the pipeline inventory, is exactly the input the
// native draw path replays.
//
// Memory payloads are skipped (only their ranges are tallied); the vertex,
// index and texture data step comes once the draw list itself is proven
// against a real trace.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include <rex/graphics/packet_disassembler.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/trace_protocol.h>
#include <snappy.h>
#define XXH_INLINE_ALL
#include <xxhash.h>

namespace fs = std::filesystem;
using namespace rex::graphics;

namespace {

constexpr uint32_t kRegisterCount = 0x5004;

struct Stats {
  std::map<std::string, size_t> packets_by_name;
  size_t packets = 0;
  size_t register_writes = 0;
  size_t draws = 0;
  size_t swaps = 0;
  size_t memory_reads = 0;
  uint64_t memory_read_bytes = 0;
  size_t memory_writes = 0;
  std::map<std::string, size_t> draws_by_packet;
};

// 512 MB guest physical space, reconstructed from the trace's MemoryRead
// payloads. The GPU only ever touched what the trace recorded, so anything a
// draw needs is guaranteed to be here by the time that draw executes.
constexpr uint32_t kGuestSpace = 512u << 20;

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: trace_draws <trace-file> [draws-out.jsonl]\n");
    return 2;
  }
  std::ifstream f(argv[1], std::ios::binary | std::ios::ate);
  if (!f) {
    std::fprintf(stderr, "cannot open %s\n", argv[1]);
    return 1;
  }
  std::vector<uint8_t> data(size_t(f.tellg()));
  f.seekg(0);
  f.read(reinterpret_cast<char*>(data.data()), std::streamsize(data.size()));

  const uint8_t* p = data.data();
  const uint8_t* end = p + data.size();

  const auto* header = reinterpret_cast<const TraceHeader*>(p);
  p += sizeof(TraceHeader);
  std::printf("trace: version %u, title %08X, %zu MB\n", header->version, header->title_id,
              data.size() >> 20);

  std::vector<uint32_t> regs(kRegisterCount, 0);
  Stats stats;

  // Reconstructed guest physical memory and the active shader pair, updated
  // by IM_LOAD / IM_LOAD_IMMEDIATE exactly as the command processor does.
  // Hashes use XXH3 over the raw big-endian bytes - the same function over
  // the same bytes as the runtime's LoadShader, so they join directly against
  // the AOT manifest.
  std::vector<uint8_t> guest(kGuestSpace, 0);
  uint64_t active_vs_hash = 0, active_ps_hash = 0;
  auto be32 = [](const uint8_t* q) {
    return uint32_t(q[0]) << 24 | uint32_t(q[1]) << 16 | uint32_t(q[2]) << 8 | uint32_t(q[3]);
  };

  std::ofstream draws_out;
  if (argc >= 3) {
    draws_out.open(argv[2]);
  }
  // Optional: dump every shader the frame loads, in the same format as the
  // dump_shaders cvar (host-endian dwords), so the trace itself back-fills
  // any shader missing from a play session's dump directory.
  fs::path shader_dump_dir;
  std::map<uint64_t, char> dumped;
  if (argc >= 4) {
    shader_dump_dir = argv[3];
    std::error_code dump_ec;
    fs::create_directories(shader_dump_dir, dump_ec);
  }
  auto dump_shader = [&](uint64_t hash, bool is_vs, const uint8_t* be_bytes,
                         uint32_t size_dwords) {
    // Keyed by (hash, stage): the game reuses some microcode blobs as both a
    // vertex and a pixel shader, and each stage needs its own translation.
    uint64_t dump_key = hash * 2 + (is_vs ? 1 : 0);
    if (shader_dump_dir.empty() || dumped.count(dump_key)) {
      return;
    }
    dumped[dump_key] = 1;
    char name[64];
    std::snprintf(name, sizeof(name), "shader_%016llX.ucode.bin.%s",
                  (unsigned long long)hash, is_vs ? "vert" : "frag");
    std::ofstream out(shader_dump_dir / name, std::ios::binary);
    for (uint32_t i = 0; i < size_dwords; ++i) {
      const uint8_t* q = be_bytes + i * 4;
      uint32_t host = uint32_t(q[0]) << 24 | uint32_t(q[1]) << 16 | uint32_t(q[2]) << 8 |
                      uint32_t(q[3]);
      out.write(reinterpret_cast<const char*>(&host), 4);
    }
  };

  // Registers of interest for a draw record: the same state the emulated
  // backend samples when it builds a pipeline and issues a draw.
  static const struct { uint32_t index; const char* name; } kDrawRegs[] = {
      {reg::SQ_PROGRAM_CNTL::register_index, "sq_program_cntl"},
      {reg::RB_MODECONTROL::register_index, "rb_modecontrol"},
      {reg::RB_SURFACE_INFO::register_index, "rb_surface_info"},
      {reg::RB_DEPTHCONTROL::register_index, "rb_depthcontrol"},
      {reg::RB_COLORCONTROL::register_index, "rb_colorcontrol"},
      {reg::RB_BLENDCONTROL::register_index, "rb_blendcontrol0"},
      {reg::RB_COLOR_INFO::register_index, "rb_color_info"},
      {reg::RB_DEPTH_INFO::register_index, "rb_depth_info"},
      {reg::PA_SU_SC_MODE_CNTL::register_index, "pa_su_sc_mode_cntl"},
      {reg::VGT_DRAW_INITIATOR::register_index, "vgt_draw_initiator"},
      {0x2318, "rb_copy_control"},
      {0x2319, "rb_copy_dest_base"},
      {0x231A, "rb_copy_dest_pitch"},
      {0x231B, "rb_copy_dest_info"},
      {0x4908 + 16, "loop16"},
      {0x4908 + 31, "loop31"},
  };

  // Memory pre-pass: a packet's memory reads are recorded after the packet
  // command itself, so IM_LOAD must hash against fully-populated memory (the
  // same content the runtime hashed at execution) instead of whatever an
  // in-order pass has seen so far.
  for (const uint8_t* q = p; q + 4 <= end;) {
    auto type = *reinterpret_cast<const TraceCommandType*>(q);
    if (type == TraceCommandType::kPacketStart) {
      auto cmd = reinterpret_cast<const PacketStartCommand*>(q);
      q += sizeof(*cmd) + cmd->count * 4;
    } else if (type == TraceCommandType::kMemoryRead ||
               type == TraceCommandType::kMemoryWrite) {
      auto cmd = reinterpret_cast<const MemoryCommand*>(q);
      const char* payload = reinterpret_cast<const char*>(q + sizeof(*cmd));
      q += sizeof(*cmd) + cmd->encoded_length;
      uint32_t base = cmd->base_ptr & (kGuestSpace - 1);
      if (uint64_t(base) + cmd->decoded_length <= kGuestSpace) {
        if (cmd->encoding_format == MemoryEncodingFormat::kSnappy) {
          std::string decoded;
          if (snappy::Uncompress(payload, cmd->encoded_length, &decoded))
            std::memcpy(guest.data() + base, decoded.data(), decoded.size());
        } else {
          std::memcpy(guest.data() + base, payload, cmd->decoded_length);
        }
      }
    } else if (type == TraceCommandType::kRegisters) {
      auto cmd = reinterpret_cast<const RegistersCommand*>(q);
      q += sizeof(*cmd) + cmd->encoded_length;
    } else if (type == TraceCommandType::kEdramSnapshot) {
      auto cmd = reinterpret_cast<const EdramSnapshotCommand*>(q);
      q += sizeof(*cmd) + cmd->encoded_length;
    } else if (type == TraceCommandType::kGammaRamp) {
      auto cmd = reinterpret_cast<const GammaRampCommand*>(q);
      q += sizeof(*cmd) + cmd->encoded_length;
    } else if (type == TraceCommandType::kEvent) {
      q += sizeof(EventCommand);
    } else if (type == TraceCommandType::kPrimaryBufferStart ||
               type == TraceCommandType::kIndirectBufferStart) {
      q += sizeof(PrimaryBufferStartCommand);
    } else {
      q += 4;
    }
  }

  size_t ib_draws_at_start = 0;
  uint32_t ib_base = 0, ib_count = 0;
  while (p + sizeof(uint32_t) <= end) {
    auto type = *reinterpret_cast<const TraceCommandType*>(p);
    switch (type) {
      case TraceCommandType::kPrimaryBufferStart: {
        auto cmd = reinterpret_cast<const PrimaryBufferStartCommand*>(p);
        std::printf("primary buffer %08X: %u dwords\n", cmd->base_ptr, cmd->count);
        p += sizeof(PrimaryBufferStartCommand);
        break;
      }
      case TraceCommandType::kPrimaryBufferEnd:
        p += sizeof(PrimaryBufferEndCommand);
        break;
      case TraceCommandType::kIndirectBufferStart: {
        auto cmd = reinterpret_cast<const IndirectBufferStartCommand*>(p);
        ib_draws_at_start = stats.draws;
        ib_base = cmd->base_ptr;
        ib_count = cmd->count;
        p += sizeof(IndirectBufferStartCommand);
        break;
      }
      case TraceCommandType::kIndirectBufferEnd:
        std::printf("  indirect buffer %08X: %u dwords, %zu draws\n", ib_base, ib_count,
                    stats.draws - ib_draws_at_start);
        p += sizeof(IndirectBufferEndCommand);
        break;
      case TraceCommandType::kPacketStart: {
        auto cmd = reinterpret_cast<const PacketStartCommand*>(p);
        p += sizeof(*cmd);
        const uint8_t* packet_ptr = p;
        p += cmd->count * sizeof(uint32_t);
        ++stats.packets;

        PacketInfo info;
        if (!PacketDisassembler::DisasmPacket(packet_ptr, &info)) {
          break;
        }
        for (const PacketAction& action : info.actions) {
          if (action.type == PacketAction::Type::kRegisterWrite &&
              action.register_write.index < kRegisterCount) {
            regs[action.register_write.index] = action.register_write.value;
            ++stats.register_writes;
          }
        }
        // Track the active shaders: type3 IM_LOAD (0x27, from guest memory)
        // and IM_LOAD_IMMEDIATE (0x2B, ucode inline in the packet).
        uint32_t head = be32(packet_ptr);
        if ((head >> 30) == 3) {
          uint32_t opcode = (head >> 8) & 0x7F;
          if (opcode == 0x27 && cmd->count >= 3) {
            uint32_t addr_type = be32(packet_ptr + 4);
            uint32_t size_dwords = be32(packet_ptr + 8) & 0xFFFF;
            uint32_t addr = (addr_type & ~0x3u) & (kGuestSpace - 1);
            uint64_t hash = XXH3_64bits(guest.data() + addr, size_dwords * 4);
            bool is_vs = (addr_type & 0x3) == 0;
            dump_shader(hash, is_vs, guest.data() + addr, size_dwords);
            if (is_vs) active_vs_hash = hash; else active_ps_hash = hash;
          } else if (opcode == 0x2B && cmd->count >= 4) {
            uint32_t shader_type = be32(packet_ptr + 4);
            uint32_t size_dwords = be32(packet_ptr + 8) & 0xFFFF;
            uint64_t hash = XXH3_64bits(packet_ptr + 12, size_dwords * 4);
            dump_shader(hash, shader_type == 0, packet_ptr + 12, size_dwords);
            if (shader_type == 0) active_vs_hash = hash; else active_ps_hash = hash;
          }
        }
        if (info.type_info) {
          ++stats.packets_by_name[info.type_info->name];
        }
        if (info.type_info && info.type_info->category == PacketCategory::kDraw) {
          ++stats.draws;
          ++stats.draws_by_packet[info.type_info->name];
          if (draws_out.is_open()) {
            char shader_buf[80];
            std::snprintf(shader_buf, sizeof(shader_buf),
                          ",\"vs\":\"%016llX\",\"ps\":\"%016llX\"",
                          (unsigned long long)active_vs_hash, (unsigned long long)active_ps_hash);
            draws_out << "{\"n\":" << stats.draws << ",\"packet\":\"" << info.type_info->name
                      << "\"" << shader_buf;
            char buf[64];
            for (const auto& r : kDrawRegs) {
              std::snprintf(buf, sizeof(buf), ",\"%s\":\"%08X\"", r.name, regs[r.index]);
              draws_out << buf;
            }
            // Texture fetch constants (type 2): slot and base address, to match
            // resolve destinations against the textures later draws read.
            draws_out << ",\"tex\":[";
            bool first_tex = true;
            for (uint32_t slot = 0; slot < 32; ++slot) {
              uint32_t dword0 = regs[0x4800 + slot * 6];
              uint32_t dword1 = regs[0x4800 + slot * 6 + 1];
              if ((dword0 & 3) != 2) {
                continue;
              }
              std::snprintf(buf, sizeof(buf), "%s[%u,\"%08X\",\"%08X\"]", first_tex ? "" : ",",
                            slot, dword1 & 0xFFFFF000u, regs[0x4800 + slot * 6 + 2]);
              draws_out << buf;
              first_tex = false;
            }
            draws_out << "]";
            // The draw's own VGT_DRAW_INITIATOR (primitive type, index count)
            // travels in the packet: DRAW_INDX has the viz query dword first,
            // DRAW_INDX_2 starts with it.
            uint32_t draw_opcode = (head >> 8) & 0x7F;
            uint32_t initiator_offset = draw_opcode == 0x22 ? 8 : 4;
            if (cmd->count * 4 >= initiator_offset + 4) {
              std::snprintf(buf, sizeof(buf), ",\"initiator\":\"%08X\"",
                            be32(packet_ptr + initiator_offset));
              draws_out << buf;
            }
            draws_out << "}\n";
          }
        }
        break;
      }
      case TraceCommandType::kPacketEnd:
        p += sizeof(PacketEndCommand);
        break;
      case TraceCommandType::kMemoryRead: {
        auto cmd = reinterpret_cast<const MemoryCommand*>(p);
        const char* payload = reinterpret_cast<const char*>(p + sizeof(*cmd));
        p += sizeof(*cmd) + cmd->encoded_length;
        ++stats.memory_reads;
        stats.memory_read_bytes += cmd->decoded_length;
        uint32_t base = cmd->base_ptr & (kGuestSpace - 1);
        if (uint64_t(base) + cmd->decoded_length <= kGuestSpace) {
          if (cmd->encoding_format == MemoryEncodingFormat::kSnappy) {
            std::string decoded;
            if (snappy::Uncompress(payload, cmd->encoded_length, &decoded) &&
                decoded.size() == cmd->decoded_length) {
              std::memcpy(guest.data() + base, decoded.data(), decoded.size());
            }
          } else {
            std::memcpy(guest.data() + base, payload, cmd->decoded_length);
          }
        }
        break;
      }
      case TraceCommandType::kMemoryWrite: {
        auto cmd = reinterpret_cast<const MemoryCommand*>(p);
        p += sizeof(*cmd) + cmd->encoded_length;
        ++stats.memory_writes;
        break;
      }
      case TraceCommandType::kEdramSnapshot: {
        auto cmd = reinterpret_cast<const EdramSnapshotCommand*>(p);
        p += sizeof(*cmd) + cmd->encoded_length;
        break;
      }
      case TraceCommandType::kEvent: {
        auto cmd = reinterpret_cast<const EventCommand*>(p);
        p += sizeof(*cmd);
        if (cmd->event_type == EventCommand::Type::kSwap) {
          ++stats.swaps;
        }
        break;
      }
      case TraceCommandType::kRegisters: {
        auto cmd = reinterpret_cast<const RegistersCommand*>(p);
        p += sizeof(*cmd);
        const char* payload = reinterpret_cast<const char*>(p);
        p += cmd->encoded_length;
        std::string decoded;
        const uint32_t* values = nullptr;
        if (cmd->encoding_format == MemoryEncodingFormat::kSnappy) {
          if (!snappy::Uncompress(payload, cmd->encoded_length, &decoded)) {
            std::fprintf(stderr, "bad snappy block in registers command\n");
            return 1;
          }
          values = reinterpret_cast<const uint32_t*>(decoded.data());
        } else {
          values = reinterpret_cast<const uint32_t*>(payload);
        }
        for (uint32_t i = 0; i < cmd->register_count; ++i) {
          uint32_t index = cmd->first_register + i;
          if (index < kRegisterCount) {
            regs[index] = values[i];
          }
        }
        break;
      }
      case TraceCommandType::kGammaRamp: {
        auto cmd = reinterpret_cast<const GammaRampCommand*>(p);
        p += sizeof(*cmd) + cmd->encoded_length;
        break;
      }
      default:
        std::fprintf(stderr, "unknown trace command %u at offset %zu - stopping\n",
                     uint32_t(type), size_t(p - data.data()));
        p = end;
        break;
    }
  }

  std::printf("packets: %zu  register writes: %zu  swaps: %zu\n", stats.packets,
              stats.register_writes, stats.swaps);
  std::printf("memory reads: %zu (%llu MB)  writes: %zu\n", stats.memory_reads,
              (unsigned long long)(stats.memory_read_bytes >> 20), stats.memory_writes);
  std::printf("packets by type:\n");
  for (const auto& [name, count] : stats.packets_by_name) {
    std::printf("  %-28s %zu\n", name.c_str(), count);
  }
  std::printf("draws: %zu\n", stats.draws);
  for (const auto& [name, count] : stats.draws_by_packet) {
    std::printf("  %-24s %zu\n", name.c_str(), count);
  }
  return 0;
}
