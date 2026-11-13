// Check the real trace writer, reader and packet disassembler without a game
// or GPU. Backend execution is qualified separately by native_geometry_check.
#include <rex/graphics/native_gpu_calls.h>
#include <rex/graphics/native_gpu_constants.h>
#include <rex/graphics/native_gpu_state.h>
#include <rex/graphics/native_gpu_coherency.h>
#include <rex/graphics/native_gpu_program.h>
#include <rex/graphics/native_gpu_ports.h>
#include <rex/graphics/native_gpu_sync.h>
#include <rex/graphics/native_gpu_indirect.h>
#include <rex/graphics/packet_disassembler.h>
#include <rex/graphics/trace_reader.h>
#include <rex/graphics/trace_writer.h>
#include <rex/logging.h>

#include <array>
#include <bit>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <vector>

namespace gfx = rex::graphics;

void Require(bool passed, const char* message) {
  if (!passed)
    throw std::runtime_error(message);
}

int main(int argc, char** argv) {
  try {
    Require(argc == 2, "usage: native_gpu_trace_check output_directory");
    std::filesystem::path output(argv[1]);
    std::filesystem::create_directories(output);
    rex::InitLogging((output / "runtime.log").string().c_str(), spdlog::level::info);
    std::array<uint8_t, 8192> memory{};
    for (uint32_t scenario = 0; scenario < 26; ++scenario) {
      const bool indexed = scenario == 1 || scenario == 5 || scenario == 7;
      const bool bare = scenario == 2 || scenario == 8;
      const bool constants = scenario == 3;
      const bool vertices = scenario == 4 || scenario == 5;
      const bool range = scenario == 6 || scenario == 7;
      const bool state = scenario >= 9 && scenario < 16;
      const bool coherency = scenario == 16;
      std::vector<uint32_t> words(range      ? (indexed ? 7 : 5)
                                  : vertices ? (indexed ? 18 : 16)
                                  : indexed  ? 27
                                  : bare     ? 2
                                             : 24);
      gfx::native_gpu_calls::DrawCall call{};
      call.initiator = (3u << 16) | (indexed ? 0 : 0x80u) | 6;
      call.index_base = indexed ? 0x00123402u : 0;
      call.index_size = indexed ? 0x40000003u : 0;
      if (vertices) {
        gfx::native_gpu_calls::VerticesDrawCall vertex_call{
            7, {1, 2, 3, 4, 5, 6}, {7, 8, 9}, 0, call.initiator, call.index_base, call.index_size};
        Require(gfx::native_gpu_calls::EncodeVertices(vertex_call, words),
                "Could not encode native vertices trace input");
      } else if (range) {
        gfx::native_gpu_calls::RangeDrawCall range_call{7, 0, call.initiator, call.index_base,
                                                        call.index_size};
        Require(gfx::native_gpu_calls::EncodeRange(range_call, words),
                "Could not encode native range trace input");
      } else if (bare) {
        words[0] = 0xC0000000u | (gfx::native_gpu_calls::kBareDrawOpcode << 8);
        if (scenario == 8)
          words[0] |= 1;
        words[1] = call.initiator;
      } else {
        Require(gfx::native_gpu_calls::EncodeDraw(call, indexed, words),
                "Could not encode native draw trace input");
      }
      std::vector<uint32_t> draw(words.size());
      for (size_t i = 0; i < words.size(); ++i)
        draw[i] = std::byteswap(words[i]);
      Require(gfx::PacketDisassembler::GetPacketCategory(
                  reinterpret_cast<const uint8_t*>(draw.data())) == gfx::PacketCategory::kDraw,
              "Native draw was lost from trace command boundaries");
      gfx::PacketInfo draw_info;
      Require(gfx::PacketDisassembler::DisasmPacket(reinterpret_cast<const uint8_t*>(draw.data()),
                                                    &draw_info) &&
                  draw_info.type_info->category == gfx::PacketCategory::kDraw &&
                  draw_info.count == words.size(),
              "Native draw disassembly lost its type or size");
      auto has_register = [&](uint32_t index, uint32_t value) {
        for (const auto& action : draw_info.actions)
          if (action.type == gfx::PacketAction::Type::kRegisterWrite &&
              action.register_write.index == index && action.register_write.value == value)
            return true;
        return false;
      };
      Require(has_register(0x21FC, call.initiator), "Disassembly lost native draw control");
      if (indexed)
        Require(has_register(0x21FA, call.index_base) && has_register(0x21FB, call.index_size),
                "Disassembly lost native DMA base or size");
      if (vertices || range) {
        Require(has_register(0x2102, 7), "Disassembly lost the first vertex");
        for (uint32_t invalid : {1u, 0x8000u}) {
          auto malformed = draw;
          malformed[0] = std::byteswap(words[0] | invalid);
          gfx::PacketInfo rejected;
          Require(!gfx::PacketDisassembler::DisasmPacket(
                      reinterpret_cast<const uint8_t*>(malformed.data()), &rejected),
                  "Disassembly accepted a predicated or reserved native header");
        }
      }
      const auto path = output / ("native-" + std::to_string(scenario) + ".xtr");
      gfx::TraceWriter writer(memory.data());
      Require(writer.Open(path, 0x45410809), "Could not open the native trace");
      writer.WritePrimaryBufferStart(0x1000, 0);
      if (scenario == 25) {
        std::array<uint32_t, 4> raw = {0x7FC00123, 0x80000000, 1, 0x3F800000};
        for (uint32_t i = 0; i < raw.size(); ++i) {
          const uint32_t value = std::byteswap(raw[i]);
          std::memcpy(memory.data() + 0x1800 + i * 4, &value, 4);
        }
        std::array<uint32_t, 4> block = {gfx::native_gpu_indirect::kHeader, 0x1800, 1023,
                                         uint32_t(raw.size())};
        for (auto& value : block)
          value = std::byteswap(value);
        gfx::PacketDisassembler::SetMemoryBase(memory.data(), uint32_t(memory.size()));
        gfx::PacketInfo info;
        Require(gfx::PacketDisassembler::DisasmPacket(
                    reinterpret_cast<const uint8_t*>(block.data()), &info) &&
                    info.count == 4 && !info.predicated && info.actions.size() == raw.size(),
                "Indirect trace lost its source boundary or actions");
        for (uint32_t i = 0; i < raw.size(); ++i)
          Require(info.actions[i].type == gfx::PacketAction::Type::kRegisterWrite &&
                      info.actions[i].register_write.index == 0x4000 + 1023 + i &&
                      info.actions[i].register_write.value == raw[i],
                  "Indirect trace lost raw source bits or the VS/PS boundary");
        for (uint32_t mask : {1u, 4u, 0x80u, 0x8000u, 0x10000u}) {
          auto invalid = block;
          invalid[0] ^= std::byteswap(mask);
          gfx::PacketInfo rejected;
          Require(!gfx::PacketDisassembler::DisasmPacket(
                      reinterpret_cast<const uint8_t*>(invalid.data()), &rejected),
                  "Trace accepted a predicated or malformed indirect-constant header");
        }
        gfx::PacketDisassembler::SetMemoryBase(nullptr, 0);
        std::memcpy(memory.data() + 0x1000, block.data(), block.size() * 4);
        writer.WritePacketStart(0x1000, uint32_t(block.size()));
        writer.WriteMemoryRead(0x1800, raw.size() * 4, memory.data() + 0x1800);
        writer.WritePacketEnd();
      }
      if (scenario >= 23 && scenario < 25) {
        const bool event = scenario == 24;
        auto coherency = gfx::native_gpu_sync::Encode({0x5000000, 0x1800, 0x2000});
        std::vector<uint32_t> block;
        if (event)
          block = {gfx::native_gpu_sync::kEventHeader, 27, 0x4003, 0x12345678};
        else
          block.assign(coherency.begin(), coherency.end());
        for (auto& value : block)
          value = std::byteswap(value);
        gfx::PacketInfo info;
        Require(gfx::PacketDisassembler::DisasmPacket(
                    reinterpret_cast<const uint8_t*>(block.data()), &info) &&
                    info.count == block.size() && !info.predicated &&
                    info.actions.size() == (event ? 1 : 3),
                "Synchronization trace changed its boundary or ordered actions");
        const std::array<std::pair<uint32_t, uint32_t>, 3> expected =
            event ? std::array<std::pair<uint32_t, uint32_t>, 3>{{{0x21F9, 27}, {}, {}}}
                  : std::array<std::pair<uint32_t, uint32_t>, 3>{
                        {{0xA31, 0}, {0xA2F, 0x1800}, {0xA30, 0x2000}}};
        for (uint32_t i = 0; i < info.actions.size(); ++i)
          Require(info.actions[i].type == gfx::PacketAction::Type::kRegisterWrite &&
                      info.actions[i].register_write.index == expected[i].first &&
                      info.actions[i].register_write.value == expected[i].second,
                  "Synchronization trace changed a register or its final value");
        for (uint32_t mask : {1u, 4u, 0x80u, 0x8000u, 0x10000u}) {
          auto invalid = block;
          invalid[0] ^= std::byteswap(mask);
          gfx::PacketInfo rejected;
          Require(!gfx::PacketDisassembler::DisasmPacket(
                      reinterpret_cast<const uint8_t*>(invalid.data()), &rejected),
                  "Trace accepted a predicated or malformed synchronization header");
        }
        std::memcpy(memory.data() + 0x1000, block.data(), block.size() * 4);
        writer.WritePacketStart(0x1000, uint32_t(block.size()));
        writer.WritePacketEnd();
      }
      if (scenario >= 20 && scenario < 23) {
        const uint32_t count = scenario == 21 ? 8 : 1;
        std::vector<uint32_t> block(count + 1);
        block[0] = scenario == 20   ? gfx::native_gpu_ports::kWaitHeader
                   : scenario == 21 ? gfx::native_gpu_ports::kGprEightHeader
                                    : gfx::native_gpu_ports::kGprHeader;
        for (uint32_t i = 1; i <= count; ++i)
          block[i] = i * 0x10203u;
        for (auto& value : block)
          value = std::byteswap(value);
        gfx::PacketInfo info;
        Require(gfx::PacketDisassembler::DisasmPacket(
                    reinterpret_cast<const uint8_t*>(block.data()), &info) &&
                    info.count == block.size() && !info.predicated &&
                    info.actions.size() == count && info.type_info &&
                    info.type_info->category == gfx::PacketCategory::kGeneric,
                "Initialization trace lost repeated port writes or changed its boundary");
        for (uint32_t i = 0; i < count; ++i)
          Require(info.actions[i].type == gfx::PacketAction::Type::kRegisterWrite &&
                      info.actions[i].register_write.index == (scenario == 20 ? 0x5C8u : 0xD00u) &&
                      info.actions[i].register_write.value == (i + 1) * 0x10203u,
                  "Initialization trace changed port repetition, register or payload");
        for (uint32_t mask : {1u, 4u, 0x80u, 0x8000u, 0x10000u}) {
          auto invalid = block;
          invalid[0] ^= std::byteswap(mask);
          gfx::PacketInfo rejected;
          Require(!gfx::PacketDisassembler::DisasmPacket(
                      reinterpret_cast<const uint8_t*>(invalid.data()), &rejected),
                  "Trace accepted a predicated or malformed initialization port");
        }
        std::memcpy(memory.data() + 0x1000, block.data(), block.size() * 4);
        writer.WritePacketStart(0x1000, uint32_t(block.size()));
        writer.WritePacketEnd();
      }
      if (scenario >= 17 && scenario < 20) {
        const bool shader = scenario < 19;
        std::vector<uint32_t> block(shader ? 18 : 2);
        if (shader) {
          Require(gfx::native_gpu_program::EncodeShaderHeader(15, block[0]),
                  "Could not encode native shader trace input");
          block[1] = scenario - 17;
          block[2] = 15;
          for (uint32_t i = 3; i < block.size(); ++i)
            block[i] = 0x91E10DA5u + i * 0x10203u;
        } else {
          block[0] = gfx::native_gpu_program::kInvalidateHeader;
          block[1] = 0x300;
        }
        for (auto& value : block)
          value = std::byteswap(value);
        gfx::PacketInfo info;
        Require(gfx::PacketDisassembler::DisasmPacket(
                    reinterpret_cast<const uint8_t*>(block.data()), &info) &&
                    info.count == block.size() && !info.predicated && info.actions.empty() &&
                    info.type_info && info.type_info->category == gfx::PacketCategory::kGeneric &&
                    !std::strcmp(info.type_info->name,
                                 shader ? "NATIVE_SHADER_BIND" : "NATIVE_STATE_INVALIDATE"),
                "Native program trace changed its type, length, predication or state actions");
        for (uint32_t mask : {1u, 8u, 0x80u, 0x8000u, 0x10000u}) {
          auto invalid = block;
          invalid[0] ^= std::byteswap(mask);
          gfx::PacketInfo rejected;
          Require(!gfx::PacketDisassembler::DisasmPacket(
                      reinterpret_cast<const uint8_t*>(invalid.data()), &rejected),
                  "Trace disassembly accepted a noncanonical program header");
        }
        if (shader) {
          for (const auto& [offset, value] :
               {std::pair{1u, 2u}, {2u, 0u}, {2u, 14u}, {2u, 0x1000Fu}}) {
            auto invalid = block;
            invalid[offset] = std::byteswap(value);
            gfx::PacketInfo rejected;
            Require(!gfx::PacketDisassembler::DisasmPacket(
                        reinterpret_cast<const uint8_t*>(invalid.data()), &rejected),
                    "Trace disassembly accepted invalid shader stage, size or start");
          }
        }
        std::memcpy(memory.data() + 0x1000, block.data(), block.size() * 4);
        writer.WritePacketStart(0x1000, uint32_t(block.size()));
        writer.WritePacketEnd();
      }
      if (coherency) {
        auto words = gfx::native_gpu_coherency::Encode({0x91E10DA5, 0x10000, 0x1800, 0x2000});
        for (auto& word : words)
          word = std::byteswap(word);
        gfx::PacketInfo info;
        Require(gfx::PacketDisassembler::DisasmPacket(
                    reinterpret_cast<const uint8_t*>(words.data()), &info) &&
                    info.count == 13 && info.actions.size() == 4 && !info.predicated &&
                    info.type_info->category == gfx::PacketCategory::kGeneric,
                "Coherency trace lost its range, type or wait result");
        const std::array<std::pair<uint32_t, uint32_t>, 4> expected = {
            std::pair{0x2007u, 0x91E10DA5u}, {0xA31u, 0u}, {0xA2Fu, 0x1800u}, {0xA30u, 0x2000u}};
        for (uint32_t i = 0; i < expected.size(); ++i)
          Require(info.actions[i].type == gfx::PacketAction::Type::kRegisterWrite &&
                      info.actions[i].register_write.index == expected[i].first &&
                      info.actions[i].register_write.value == expected[i].second,
                  "Coherency trace changed its destination/size/base or completed wait");
        for (uint32_t changed : {0u, 2u, 4u, 7u, 8u, 9u, 10u, 11u, 12u}) {
          auto invalid = words;
          invalid[changed] ^= std::byteswap(1u);
          gfx::PacketInfo rejected;
          Require(!gfx::PacketDisassembler::DisasmPacket(
                      reinterpret_cast<const uint8_t*>(invalid.data()), &rejected),
                  "Trace disassembly accepted an invalid coherency contract");
        }
        std::memcpy(memory.data() + 0x1000, words.data(), sizeof(words));
        writer.WritePacketStart(0x1000, uint32_t(words.size()));
        writer.WritePacketEnd();
      }
      if (state) {
        const uint32_t page = gfx::native_gpu_state::kPages[scenario - 9];
        const uint32_t first = page < 0x4800    ? page + 128
                               : page == 0x4800 ? 0x48BA
                               : page == 0x4900 ? 0x4920
                                                : page;
        const uint32_t count = page < 0x4800 ? 7 : page == 0x4800 ? 6 : page == 0x4900 ? 8 : 3;
        std::vector<uint32_t> block(count + 1);
        Require(gfx::native_gpu_state::EncodeHeader(first, count, block[0]),
                "Could not encode native state trace input");
        for (uint32_t i = 1; i <= count; ++i)
          block[i] = 0x7FC00000u + i;
        for (auto& value : block)
          value = std::byteswap(value);
        gfx::PacketInfo info;
        Require(gfx::PacketDisassembler::DisasmPacket(
                    reinterpret_cast<const uint8_t*>(block.data()), &info) &&
                    info.count == count + 1 && info.actions.size() == count && !info.predicated,
                "Native state trace changed its length or predication");
        for (uint32_t i = 0; i < count; ++i)
          Require(info.actions[i].type == gfx::PacketAction::Type::kRegisterWrite &&
                      info.actions[i].register_write.index == first + i &&
                      info.actions[i].register_write.value == 0x7FC00001u + i,
                  "Native state trace lost register addresses or raw payload bits");
        auto invalid = block;
        invalid[0] |= std::byteswap(1u);
        gfx::PacketInfo rejected;
        Require(!gfx::PacketDisassembler::DisasmPacket(
                    reinterpret_cast<const uint8_t*>(invalid.data()), &rejected),
                "Disassembly accepted a predicated native state range");
        std::memcpy(memory.data() + 0x1000, block.data(), block.size() * 4);
        writer.WritePacketStart(0x1000, uint32_t(block.size()));
        writer.WritePacketEnd();
      }
      if (constants) {
        std::array<uint32_t, 17> block;
        Require(gfx::native_gpu_constants::EncodeHeader(0x4410, 16, block[0]),
                "Could not encode trace shader constants");
        for (uint32_t i = 1; i < block.size(); ++i)
          block[i] = 0x7FC00000u + i;
        for (auto& word : block)
          word = std::byteswap(word);
        gfx::PacketInfo info;
        Require(gfx::PacketDisassembler::DisasmPacket(
                    reinterpret_cast<const uint8_t*>(block.data()), &info) &&
                    info.actions.size() == 16,
                "Native constants were lost from trace state");
        for (uint32_t i = 0; i < 16; ++i)
          Require(info.actions[i].type == gfx::PacketAction::Type::kRegisterWrite &&
                      info.actions[i].register_write.index == 0x4410 + i &&
                      info.actions[i].register_write.value == 0x7FC00001u + i,
                  "Native constant trace actions changed raw payload bits");
        std::memcpy(memory.data() + 0x1000, block.data(), sizeof(block));
        writer.WritePacketStart(0x1000, uint32_t(block.size()));
        writer.WritePacketEnd();
      }
      std::memcpy(memory.data() + 0x1000, draw.data(), draw.size() * 4);
      writer.WritePacketStart(0x1000, uint32_t(draw.size()));
      writer.WritePacketEnd();
      writer.WritePrimaryBufferEnd();
      writer.Close();
      gfx::TraceReader reader;
      Require(reader.Open(path.string()), "Could not read the recorded native trace");
      Require(
          reader.frame_count() == 1 && reader.frame(0)->commands.size() == 1 &&
              reader.frame(0)->commands[0].type == gfx::TraceReader::Frame::Command::Type::kDraw,
          "Native-only trace has no seekable draw without a swap");
      const auto& command = reader.frame(0)->commands[0];
      Require(command.start_ptr < command.end_ptr && command.head_ptr >= command.start_ptr &&
                  command.end_ptr <= reader.frame(0)->end_ptr,
              "Native trace draw has an invalid replay boundary");
      reader.Close();
    }
    std::puts(
        "{\"passed\":true,\"native_only_no_swap_traces\":26,\"constant_actions\":16,"
        "\"state_actions\":45,"
        "\"coherency_actions\":4,"
        "\"shader_bindings\":2,\"state_invalidations\":1,"
        "\"initialization_port_actions\":10,"
        "\"synchronization_actions\":4,"
        "\"indirect_constant_actions\":4,"
        "\"scope\":\"trace indexing and disassembly, not GPU replay\"}");
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
  }
}
