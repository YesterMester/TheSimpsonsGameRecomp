#include <rex/graphics/native_gpu_presentation.h>
#include <rex/graphics/native_gpu_state.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/xenos.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <stdexcept>
#include <vector>

namespace {
namespace presentation = rex::graphics::native_gpu_presentation;
namespace xenos = rex::graphics::xenos;
using Buffer = std::array<uint32_t, 64>;

void Require(bool value, const char* message) {
  if (!value)
    throw std::runtime_error(message);
}

struct Output {
  struct Word {
    uint32_t& word;
    bool swapped;
    void operator=(uint32_t value) { word = swapped ? std::byteswap(value) : value; }
  };
  Buffer& words;
  bool swapped;
  Word operator[](uint32_t i) { return {words[i], swapped}; }
};

// Original VdSwap format, kept independent of the native formatter. Use the
// SDK's original packet helpers and the original order of mapped writes/reads.
template <typename ReadWidth, typename ReadHeight>
void Original(Output output, const std::array<uint32_t, 6>& fetch, uint32_t address,
              ReadWidth width, ReadHeight height) {
  uint32_t offset = 0;
  output[offset++] = xenos::MakePacketType0(rex::graphics::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0, 6);
  output[offset++] = fetch[0];
  output[offset++] = fetch[1];
  output[offset++] = fetch[2];
  output[offset++] = fetch[3];
  output[offset++] = fetch[4];
  output[offset++] = fetch[5];
  output[offset++] = xenos::MakePacketType3(xenos::PM4_XE_SWAP, 4);
  output[offset++] = uint32_t(xenos::kSwapSignature);
  output[offset++] = address;
  output[offset++] = width();
  output[offset++] = height();
  for (uint32_t i = offset; i < 64; ++i)
    output[i] = xenos::MakePacketType2();
}

uint64_t BufferChecks() {
  uint64_t checks = 0;
  for (bool swapped : {false, true})
    for (uint32_t seed : {0u, 1u, 0xFFFFFFFFu, 0xA5A591E1u})
      for (uint32_t address : {0u, 0x0314C000u, 0x1FFFF000u, 0xFFFFFFFFu})
        for (uint32_t width : {0u, 1u, 640u, 1280u, 3840u, 7680u, UINT32_MAX})
          for (uint32_t height : {0u, 1u, 480u, 720u, 2160u, UINT32_MAX}) {
            std::array<uint32_t, 6> fetch;
            for (uint32_t i = 0; i < fetch.size(); ++i)
              fetch[i] = std::rotl(seed, int(i * 3)) ^ (i * 0x91E10DA5u);
            Buffer reference{}, legacy{}, native{};
            std::vector<int> original_order, legacy_order, native_order;
            const auto read = [](uint32_t value, int channel, auto& order) {
              order.push_back(channel);
              return value;
            };
            Original(Output{reference, swapped}, fetch, address,
                      [&] { return read(width, 0, original_order); },
                      [&] { return read(height, 1, original_order); });
            presentation::WriteBuffer(Output{legacy, swapped}, fetch, address,
                                       [&] { return read(width, 0, legacy_order); },
                                       [&] { return read(height, 1, legacy_order); },
                                       uint32_t(xenos::kSwapSignature), false);
            presentation::WriteBuffer(Output{native, swapped}, fetch, address,
                                       [&] { return read(width, 0, native_order); },
                                       [&] { return read(height, 1, native_order); },
                                       uint32_t(xenos::kSwapSignature), true);
            Require(reference == legacy, "Opt-out changed the original mapped swap buffer");
            Require(original_order == std::vector<int>({0, 1}) && original_order == legacy_order &&
                        original_order == native_order, "Mapped dimension reads changed order");
            const auto load = [&](uint32_t i) {
              return swapped ? std::byteswap(native[i]) : native[i];
            };
            Require(load(0) == presentation::kFetchHeader && load(7) == presentation::kHeader,
                    "Native fetch/presentation headers changed their positions");
            const std::array<uint32_t, 4> payload = {load(8), load(9), load(10), load(11)};
            presentation::Call call;
            Require(presentation::Decode(payload, call) && call.address == address &&
                        call.width == width && call.height == height,
                    "Native presentation changed a host-swap argument");
            for (uint32_t i : {0u, 7u, 8u})
              native[i] = reference[i];
            Require(native == reference, "Native presentation changed payload or padding bytes");
            ++checks;
          }
  // When a dimension aliases an output word, the production producer keeps
  // the legacy format. Check the actual sequential read effects in that case.
  for (uint32_t width_slot = 0; width_slot < 64; ++width_slot)
    for (uint32_t height_slot = 0; height_slot < 64; ++height_slot) {
      Buffer reference{}, legacy{};
      const std::array<uint32_t, 6> fetch = {1, 2, 3, 4, 5, 6};
      Require(!presentation::DimensionsIndependent(legacy.data(), &legacy[width_slot],
                                                    &legacy[height_slot]),
              "An aliased dimension used the native formatter");
      Original(Output{reference, false}, fetch, 0x0314C000,
                [&] { return reference[width_slot]; }, [&] { return reference[height_slot]; });
      presentation::WriteBuffer(Output{legacy, false}, fetch, 0x0314C000,
                                 [&] { return legacy[width_slot]; },
                                 [&] { return legacy[height_slot]; },
                                 uint32_t(xenos::kSwapSignature), false);
      Require(reference == legacy, "Aliased legacy dimension reads changed their results");
      ++checks;
    }
  return checks;
}

uint64_t SchemaChecks() {
  uint64_t checks = 0;
  uint32_t header = 0;
  Require(rex::graphics::native_gpu_state::EncodeHeader(0x4800, 6, header) &&
              header == presentation::kFetchHeader, "Native presentation lost fetch slot zero");
  Require(presentation::IsValidHeader(presentation::kHeader, 4), "Valid presentation header rejected");
  for (uint32_t bit = 0; bit < 32; ++bit) {
    Require(!presentation::IsValidHeader(presentation::kHeader ^ (1u << bit), 4),
            "Malformed presentation header accepted");
    ++checks;
  }
  for (uint32_t count = 0; count < 32; ++count) {
    Require(presentation::IsValidHeader(presentation::kHeader, count) == (count == 4),
            "Presentation payload size was not enforced");
    ++checks;
  }
  std::array<uint32_t, 8> payload = {presentation::kVersion, 0, 1280, 720};
  for (uint32_t count = 0; count <= payload.size(); ++count) {
    presentation::Call call{1, 2, 3};
    Require(presentation::Decode({payload.data(), count}, call) == (count == 4),
            "Presentation schema accepted a different operand span");
    if (count != 4)
      Require(call.address == 1 && call.width == 2 && call.height == 3,
              "Rejected presentation payload changed the output");
    ++checks;
  }
  payload[0] ^= 1;
  presentation::Call call{1, 2, 3};
  Require(!presentation::Decode({payload.data(), 4}, call) && call.address == 1 &&
              call.width == 2 && call.height == 3, "Wrong presentation version accepted");
  return checks + 1;
}
}  // namespace

int main(int argc, char** argv) {
  try {
    const auto buffers = BufferChecks(), schemas = SchemaChecks();
    char report[256];
    std::snprintf(report, sizeof(report),
                  "{\"passed\":true,\"buffer_cases\":%llu,\"schema_cases\":%llu,\"rendered_frames\":0}\n",
                  static_cast<unsigned long long>(buffers), static_cast<unsigned long long>(schemas));
    FILE* file = argc > 1 ? std::fopen(argv[1], "wb") : stdout;
    Require(file != nullptr, "Cannot open presentation check report");
    std::fputs(report, file);
    if (file != stdout)
      Require(!std::fclose(file), "Cannot close presentation check report");
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
