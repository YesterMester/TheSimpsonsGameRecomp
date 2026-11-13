// Check partial masks and conditional snapshots through the production
// disassembler and shared trace-state helper, without substituting a GPU.
#include <rex/graphics/native_gpu_predication.h>
#include <rex/graphics/native_gpu_calls.h>
#include <rex/graphics/packet_state.h>
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
namespace pred = gfx::native_gpu_predication;

void Require(bool ok, const char* why) {
  if (!ok)
    throw std::runtime_error(why);
}

int main(int argc, char** argv) {
  try {
    Require(argc == 2, "usage: native_gpu_predication_trace_check output_directory");
    std::filesystem::path directory(argv[1]);
    std::filesystem::create_directories(directory);
    rex::InitLogging((directory / "runtime.log").string().c_str(), spdlog::level::info);
    gfx::PacketState state;
    std::vector<uint32_t> registers(0x5004);
    std::array<uint8_t, 8192> memory{};
    gfx::TraceWriter writer(memory.data());
    Require(writer.Open(directory / "predication.xtr", 0x45410809),
            "Cannot open predication trace");
    writer.WritePrimaryBufferStart(0x1000, 0);
    uint32_t packets = 0;
    const auto submit = [&](std::vector<uint32_t> words) {
      const uint32_t header = words[0];
      for (auto& word : words)
        word = std::byteswap(word);
      gfx::PacketInfo info;
      Require(
          gfx::PacketDisassembler::DisasmPacket(reinterpret_cast<uint8_t*>(words.data()), &info),
          "Trace rejected a valid conditional operation");
      const auto result = state.Apply(info, header, registers);
      std::memcpy(memory.data() + 0x1000, words.data(), words.size() * 4);
      writer.WritePacketStart(0x1000, uint32_t(words.size()));
      writer.WritePacketEnd();
      ++packets;
      return result;
    };
    submit({0xC0015000, 0xFFFF1111, 0xAAAA5555});
    submit({pred::kMaskLowHeader, 0x12345678});
    if (state.bin_mask != 0xFFFF111112345678ull)
      std::fprintf(stderr, "Observed bin mask %016llX\n", (unsigned long long)state.bin_mask);
    Require(state.bin_mask == 0xFFFF111112345678ull, "Low mask update lost the high half");
    submit({pred::kMaskHighHeader, 0xDEADBEEF});
    Require(state.bin_mask == 0xDEADBEEF12345678ull, "High mask update lost the low half");
    submit({0xC0006200, 0xA5A50102});
    submit({0xC0006300, 0x91E10DA5});
    Require(state.bin_select == 0x91E10DA5A5A50102ull, "Partial selection lost a half");
    submit({0xC0015100, 1, 0});
    submit({pred::kMaskHighHeader, 1});
    submit({pred::kMaskLowHeader, 0});
    const auto selected = submit({pred::kStateHeader, 0x40203, 0x11223344});
    Require(selected.executed && selected.register_writes == 1 && registers[0x2203] == 0x11223344,
            "High-half selection did not apply the state");
    submit({pred::kMaskLowHeader, 1});
    Require(submit({pred::kStateHeader, 0x40000, 0x55667788}).executed &&
                registers[0x2000] == 0x55667788,
            "Changing low mask lost a high-half selection");
    submit({pred::kMaskHighHeader, 0});
    const auto rejected = submit({pred::kStateHeader, 0x40104, 0x99AABBCC});
    Require(!rejected.executed && !rejected.register_writes && !registers[0x2104],
            "Rejected conditional state changed registers");

    gfx::native_gpu_calls::VerticesDrawCall vertices{7,          {1, 2, 3, 4, 5, 6}, {7, 8, 9}, 0,
                                                     0x00030006, 0x123402,           0x40000003};
    std::array<uint32_t, 18> encoded;
    Require(gfx::native_gpu_calls::EncodeVertices(vertices, encoded),
            "Cannot encode composite probe");
    const auto composite = submit({encoded.begin(), encoded.end()});
    Require(composite.executed && !composite.draw && registers[0x2102] == 7 &&
                registers[0x48BA] == 1 && registers[0x48BF] == 6 && registers[0x5002] == 9 &&
                !registers[0x21FC] && !registers[0x21FA] && !registers[0x21FB],
            "Rejected composite lost unconditional state or applied draw/DMA state");
    gfx::native_gpu_calls::RangeDrawCall range{19, 0, 0x00030006, 0x200002, 0x40000003};
    std::array<uint32_t, 7> range_words;
    Require(gfx::native_gpu_calls::EncodeRange(range, range_words), "Cannot encode range probe");
    const auto range_result = submit({range_words.begin(), range_words.end()});
    Require(range_result.executed && !range_result.draw && registers[0x2102] == 19 &&
                !registers[0x21FC] && !registers[0x21FA] && !registers[0x21FB],
            "Rejected range applied its contained draw/DMA state");
    Require(!submit({0xC0000001u | (gfx::native_gpu_calls::kBareDrawOpcode << 8), 0x10081}).draw,
            "Rejected whole-call draw remained visible in extraction");
    submit({pred::kMaskLowHeader, UINT32_MAX});
    submit({pred::kMaskHighHeader, 1});
    const auto selected_range = submit({0xC0026F05, 0x40180, 0x91E10DA5, 0x7FC00123});
    Require(selected_range.executed && selected_range.register_writes == 2 &&
                registers[0x2180] == 0x91E10DA5 && registers[0x2181] == 0x7FC00123,
            "Selected range lost its raw values or sequential registers");
    Require(submit({0xC0000000u | (gfx::native_gpu_calls::kBareDrawOpcode << 8), 0x10081}).draw,
            "Unpredicated initialization draw was lost");
    writer.WritePrimaryBufferEnd();
    writer.Close();

    uint32_t malformed = 0;
    const auto reject = [&](std::vector<uint32_t> words) {
      for (auto& word : words)
        word = std::byteswap(word);
      gfx::PacketInfo info;
      Require(
          !gfx::PacketDisassembler::DisasmPacket(reinterpret_cast<uint8_t*>(words.data()), &info),
          "Trace accepted invalid predication metadata");
      ++malformed;
    };
    for (uint32_t header : {pred::kMaskLowHeader, pred::kMaskHighHeader, pred::kStateHeader})
      for (uint32_t mask : {1u, 8u, 0x80u, 0x8000u, 0x10000u})
        reject({header ^ mask, 0x40203, 0x12345678});
    for (uint32_t operand : {0u, 0x30000u, 0x40400u, 0x40800u, UINT32_MAX})
      reject({pred::kStateHeader, operand, 0x12345678});
    for (uint32_t operand : {0x400FFu, 0x401FFu, 0x402FFu, 0x403FFu})
      reject({0xC0026F05, operand, 0x12345678, 0x87654321});
    // Reusing an output object must not retain a prior predicate or actions.
    std::array<uint32_t, 3> conditional = {std::byteswap(pred::kStateHeader),
                                           std::byteswap(0x40203u), std::byteswap(1u)};
    std::array<uint32_t, 2> mask = {std::byteswap(pred::kMaskLowHeader), std::byteswap(1u)};
    gfx::PacketInfo reused;
    Require(gfx::PacketDisassembler::DisasmPacket(reinterpret_cast<uint8_t*>(conditional.data()),
                                                  &reused) &&
                reused.predicated && reused.actions.size() == 1 &&
                gfx::PacketDisassembler::DisasmPacket(reinterpret_cast<uint8_t*>(mask.data()),
                                                      &reused) &&
                !reused.predicated && reused.actions.size() == 1 && reused.count == 2,
            "Reused disassembly retained stale predicates or actions");
    std::printf(
        "{\"passed\":true,\"recorded_packets\":%u,\"expected_extracted_draws\":1,"
        "\"malformed_rejections\":%u,\"expected_hiz_control\":\"11223344\","
        "\"expected_surface_info\":\"55667788\"}\n",
        packets, malformed);
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "Predication trace: %s\n", e.what());
    return 1;
  }
}
