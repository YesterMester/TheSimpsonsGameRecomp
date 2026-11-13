// Trace source snapshots follow PacketStart. Reusing one address must retain
// each binding's own bytes, constants and bin-predicate behavior in the tools.
#include <rex/graphics/native_gpu_calls.h>
#include <rex/graphics/native_gpu_indirect.h>
#include <rex/graphics/native_gpu_shader_reference.h>
#include <rex/graphics/trace_reader.h>
#include <rex/graphics/trace_writer.h>
#include <rex/graphics/xenos.h>
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
    Require(argc == 2, "usage: native_gpu_shader_reference_trace_check output_directory");
    std::filesystem::path out(argv[1]);
    std::filesystem::create_directories(out);
    rex::InitLogging((out / "runtime.log").string().c_str(), spdlog::level::info);
    for (bool native : {false, true}) {
      std::array<uint8_t, 8192> memory{};
      const auto path = out / (native ? "native.xtr" : "legacy.xtr");
      gfx::TraceWriter writer(memory.data());
      Require(writer.Open(path, 0x45410809), "Cannot open changing-source trace");
      writer.WritePrimaryBufferStart(0x1000, 0);
      const auto packet = [&](const std::vector<uint32_t>& words, uint32_t source = 0,
                              const std::array<uint32_t, 4>* values = nullptr) {
        for (uint32_t i = 0; i < words.size(); ++i) {
          const auto value = std::byteswap(words[i]);
          std::memcpy(memory.data() + 0x1000 + i * 4, &value, 4);
        }
        if (values)
          for (uint32_t i = 0; i < values->size(); ++i) {
            const auto value = std::byteswap((*values)[i]);
            std::memcpy(memory.data() + source + i * 4, &value, 4);
          }
        writer.WritePacketStart(0x1000, uint32_t(words.size()));
        if (values)
          writer.WriteMemoryRead(source, values->size() * 4, memory.data() + source);
        writer.WritePacketEnd();
      };
      const std::array<uint32_t, 4> shader_a = {0x91E10DA5, 0x10203040, 0x3F800000, 0x7FC00123};
      const std::array<uint32_t, 4> shader_b = {0x91E10DA4, 0x10203041, 0x3F800001, 0x7FC00122};
      const std::array<uint32_t, 4> constants_a = {0x7FC00123, 0x80000000, 1, 0x3F800000};
      const std::array<uint32_t, 4> constants_b = {0x7FC00122, 0x80000001, 0, 0x3F800001};
      const uint32_t shader_header =
          native ? gfx::native_gpu_shader_reference::kHeader : 0xC0012700;
      const uint32_t constants_header = native ? gfx::native_gpu_indirect::kHeader : 0xC0022F00;
      const uint32_t draw = 0xC0000000u | (gfx::native_gpu_calls::kBareDrawOpcode << 8);
      for (uint32_t version = 0; version < 2; ++version) {
        packet({shader_header, 0x1800, 4}, 0x1800, version ? &shader_b : &shader_a);
        packet({constants_header, 0x1C00, 1023, 4}, 0x1C00, version ? &constants_b : &constants_a);
        packet({draw, 0x10081});
      }
      packet({gfx::xenos::MakePacketType3(gfx::xenos::PM4_SET_BIN_MASK, 2), 0, 0});
      packet({shader_header | 1, 0x1801, 4});
      packet({draw, 0x10081});
      writer.WritePrimaryBufferEnd();
      writer.Close();
      gfx::TraceReader reader;
      Require(reader.Open(path.string()), "Cannot read changing-source trace");
      Require(reader.frame_count() == 1 && reader.frame(0)->commands.size() == 3,
              "Changing-source trace lost an unpredicated draw or swap-free frame");
      reader.Close();
    }
    std::puts(
        "{\"passed\":true,\"changing_source_traces\":2,\"draws\":6,"
        "\"scope\":\"trace writing and indexing; extraction checked separately\"}");
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "Shader-reference trace: %s\n", e.what());
    return 1;
  }
}
