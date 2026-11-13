// Indirect snapshots occur after PacketStart. Check all five register classes
// with the same changing source address, ignored operand bits and a zero bin mask.
#include <rex/graphics/native_gpu_calls.h>
#include <rex/graphics/native_gpu_indirect.h>
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
void Require(bool ok, const char* why) {
  if (!ok)
    throw std::runtime_error(why);
}
int main(int argc, char** argv) {
  try {
    Require(argc == 2, "usage: native_gpu_typed_indirect_trace_check output_directory");
    std::filesystem::path out(argv[1]);
    std::filesystem::create_directories(out);
    rex::InitLogging((out / "runtime.log").string().c_str(), spdlog::level::info);
    const std::array<uint32_t, 5> indices = {1023, 0, 0, 16, 0x180};
    const std::array<std::vector<uint32_t>, 5> inputs = {
        std::vector<uint32_t>{0x7FC00123, 0x80000000, 1, 0x3F800000},
        {2, 0xABCDF000, 0x12345678, 0, 0x80000000, 0x7FC00123},
        {0x7FC00123, 0x80000000, 0, 1, 0x91E10DA5, 0xDEADBEEF, 0xFFFFFFFE, 0x12345678},
        {0x91E10DA5, 0x7FC00123},
        {0x11223344, 0x55667788}};
    for (bool native : {false, true}) {
      std::array<uint8_t, 8192> memory{};
      const auto path = out / (native ? "typed.xtr" : "legacy.xtr");
      gfx::TraceWriter writer(memory.data());
      Require(writer.Open(path, 0x45410809), "Cannot open typed constant trace");
      writer.WritePrimaryBufferStart(0x1000, 0);
      const auto packet = [&](const std::vector<uint32_t>& words,
                              const std::vector<uint32_t>* values = nullptr) {
        for (uint32_t i = 0; i < words.size(); ++i) {
          const auto value = std::byteswap(words[i]);
          std::memcpy(memory.data() + 0x1000 + i * 4, &value, 4);
        }
        if (values)
          for (uint32_t i = 0; i < values->size(); ++i) {
            const auto value = std::byteswap((*values)[i]);
            std::memcpy(memory.data() + 0x1800 + i * 4, &value, 4);
          }
        writer.WritePacketStart(0x1000, uint32_t(words.size()));
        if (values)
          writer.WriteMemoryRead(0x1800, values->size() * 4, memory.data() + 0x1800);
        writer.WritePacketEnd();
      };
      packet({0xC0015000, 0, 0});
      for (uint32_t type = 0; type < inputs.size(); ++type)
        for (uint32_t version = 0; version < 2; ++version) {
          auto values = inputs[type];
          if (version)
            for (auto& value : values)
              value ^= 1;
          if (type == 1)
            values[0] = 2;
          packet({native ? gfx::native_gpu_indirect::kTypedHeader : 0xC0022F00, 0xC0001800,
                  0xA5008800u | (type << 16) | indices[type], 0xF000u | uint32_t(values.size())},
                 &values);
          packet({0xC0000000u | (gfx::native_gpu_calls::kBareDrawOpcode << 8), 0x10081});
        }
      writer.WritePrimaryBufferEnd();
      writer.Close();
      gfx::TraceReader reader;
      Require(reader.Open(path.string()) && reader.frame_count() == 1 &&
                  reader.frame(0)->commands.size() == 10,
              "Typed constant trace lost a draw or swap-free frame");
      reader.Close();
    }
    std::puts(
        "{\"passed\":true,\"typed_source_traces\":2,\"draws\":20,"
        "\"scope\":\"trace writing and indexing; extraction checked separately\"}");
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "Typed constant trace: %s\n", e.what());
    return 1;
  }
}
