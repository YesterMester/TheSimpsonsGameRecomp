// Direct calls keep both legacy layouts, whole-call predication, unused auto
// operands and the original halfword index address. Extract against manual state.
#include <rex/graphics/native_gpu_direct.h>
#include <rex/graphics/trace_writer.h>
#include <rex/logging.h>

#include <array>
#include <bit>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace gfx = rex::graphics;
void Require(bool ok, const char* why) {
  if (!ok)
    throw std::runtime_error(why);
}
int main(int argc, char** argv) {
  try {
    Require(argc == 2, "usage: native_gpu_direct_trace_check output_directory");
    std::filesystem::path out(argv[1]);
    std::filesystem::create_directories(out);
    rex::InitLogging((out / "runtime.log").string().c_str(), spdlog::level::info);
    constexpr std::array<uint32_t, 8> headers = {0xC0012200, 0xC0012201, 0xC0032200, 0xC0032201,
                                                 0xC0003600, 0xC0003601, 0xC0023600, 0xC0023601};
    for (bool native : {false, true}) {
      std::array<uint8_t, 8192> memory{};
      gfx::TraceWriter writer(memory.data());
      const std::string label = native ? "direct" : "legacy";
      Require(writer.Open(out / (label + ".xtr"), 0x45410809), "Cannot open direct trace");
      std::ofstream expected(out / (label + "-expected.jsonl"));
      writer.WritePrimaryBufferStart(0x1000, 0);
      const auto packet = [&](const std::vector<uint32_t>& words) {
        for (uint32_t i = 0; i < words.size(); ++i) {
          const uint32_t value = std::byteswap(words[i]);
          std::memcpy(memory.data() + 0x1000 + i * 4, &value, 4);
        }
        writer.WritePacketStart(0x1000, uint32_t(words.size()));
        writer.WritePacketEnd();
      };
      uint32_t dma_base = 0, dma_size = 0, draws = 0;
      for (uint32_t old : headers) {
        const uint32_t count = ((old >> 16) & 0x3FFF) + 1;
        const bool query = ((old >> 8) & 127) == 0x22;
        for (uint32_t source : {2u, 0u}) {
          if (source == 0 && count - query < 3)
            continue;
          for (bool wide : {false, true}) {
            if (source == 2 && wide)
              continue;
            for (bool selected : {false, true}) {
              packet({0xC0015000, 0, selected ? UINT32_MAX : 0});
              uint32_t head = old;
              if (native)
                Require(gfx::native_gpu_direct::EncodeLegacyHeader(old, head),
                        "Cannot encode direct trace header");
              const uint32_t control = 0x00030006u | (source << 6) | (wide ? 0x800 : 0);
              std::vector<uint32_t> words = {head};
              if (query)
                words.push_back(0xA5A50100);
              words.push_back(control);
              if (count - query == 3) {
                words.push_back(0x120002);
                words.push_back(0x80000006);
              }
              Require(words.size() == count + 1, "Trace changed the original operand count");
              packet(words);
              if ((old & 1) && !selected)
                continue;
              if (source == 0) {
                dma_base = 0x120002;
                dma_size = 0x80000006;
              }
              char row[256];
              std::snprintf(row, sizeof(row),
                            "{\"n\":%u,\"initiator\":\"%08X\",\"vgt_dma_base\":\"%08X\",\"vgt_dma_"
                            "size\":\"%08X\"}\n",
                            ++draws, control, dma_base, dma_size);
              expected << row;
            }
          }
        }
      }
      Require(draws == 24 && bool(expected), "Direct trace coverage or expected output changed");
      writer.WritePrimaryBufferEnd();
      writer.Close();
    }
    std::puts("{\"passed\":true,\"traces\":2,\"expected_draws\":48}");
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "Direct trace: %s\n", e.what());
    return 1;
  }
}
