// Compare the constant-time address proof with every index executed by the
// shader's separate float add, multiply, optional round and floor operations.
#include <rex/graphics/util/vertex_fetch_bounds.h>
#include <rex/graphics/util/vertex_index_bounds.h>
#include <rex/graphics/util/native_vertex_fetch.h>

#include <array>
#include <cmath>
#include <cfenv>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <utility>

#if defined(__clang__)
#pragma STDC FENV_ACCESS ON
#endif

namespace {
void Require(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}
}  // namespace

int main(int argc, char** argv) {
  try {
    Require(argc == 1 || (argc == 3 && std::strcmp(argv[1], "--output") == 0),
            "usage: vertex_fetch_bounds_check [--output report.json]");
    using rex::graphics::draw_util::GetVertexFetchByteRange;
    auto prefix = GetVertexFetchByteRange(1024, -16, -4, 16384);
    Require(prefix && prefix.address == 4032 && prefix.size == 48,
            "A valid prefix before the stream was not preserved");
    auto start = GetVertexFetchByteRange(1024, -1024, -1023, 16384);
    Require(start && start.address == 0 && start.size == 4, "Physical address zero was lost");
    auto end = GetVertexFetchByteRange(4096, -1, 0, 16384);
    Require(end && end.address == 16380 && end.size == 4, "The final physical word was lost");
    Require(!GetVertexFetchByteRange(0, -1, 1, 16384) &&
                !GetVertexFetchByteRange(4096, 0, 1, 16384) &&
                !GetVertexFetchByteRange(0, 1, 1, 16384) &&
                !GetVertexFetchByteRange(0, INT64_MIN, INT64_MAX, 16384) &&
                !GetVertexFetchByteRange(UINT32_MAX, 0, 1, 16384),
            "An invalid or overflowing physical vertex range was accepted");
    uint64_t packed_cases = 0;
    for (int64_t first_word : {-0x3FFFFFFFll, -0x1000000ll, -128ll, -1ll, 0ll, 1ll, 0xFFFFFEll,
                               0xFFFFFFll, 0x1000000ll, 0x7FFFFFFll}) {
      for (uint32_t offset : {0u, 4u, 128u, 0x7FFFFFCu}) {
        for (uint32_t endian = 0; endian < 4; ++endian) {
          auto packed = rex::graphics::draw_util::PackNativeVertexFetch(offset, first_word, endian);
          Require((packed[0] & ~3u) == offset && (packed[0] & 1u) && (packed[1] & 3u) == endian,
                  "Compact vertex metadata changed its address, valid flag or endian mode");
          Require(rex::graphics::draw_util::UnpackNativeVertexFetchOffset(packed) == first_word,
                  "A large or negative vertex rebase did not round-trip");
          for (uint32_t word = 0; word < 4; ++word) {
            uint32_t address = (packed[0] >> 2) + uint32_t(first_word + word);
            address -= uint32_t(rex::graphics::draw_util::UnpackNativeVertexFetchOffset(packed));
            Require(address == (offset >> 2) + word,
                    "Compact vertex arithmetic differs from the original physical word");
          }
          ++packed_cases;
        }
      }
    }
    uint64_t cases = 0, indices = 0, rejected = 0;
    uint64_t auto_cases = 0, auto_indices = 0;
    for (uint32_t base : {0u, 1u, 127u, 0xFFFFE0u, 0xFFFFFEu, 0xFFFFFFu, 0x7FFFFFFFu, UINT32_MAX}) {
      for (uint32_t count : {0u, 1u, 2u, 3u, 9u, 257u, 65535u}) {
        for (auto clamp : {std::pair{0u, 0xFFFFFFu}, std::pair{2u, 50u}, std::pair{0u, 0u},
                           std::pair{127u, 255u}, std::pair{0xFFFFF0u, UINT32_MAX},
                           std::pair{0x1000000u, UINT32_MAX}}) {
          auto bounds = rex::graphics::draw_util::GetAutoVertexIndexBounds(count, base, clamp.first,
                                                                           clamp.second);
          uint32_t minimum = UINT32_MAX, maximum = 0;
          for (uint32_t index = 0; index < count; ++index) {
            uint32_t fetched = std::clamp((base + index) & 0xFFFFFFu, clamp.first, clamp.second);
            minimum = std::min(minimum, fetched);
            maximum = std::max(maximum, fetched);
            ++auto_indices;
          }
          Require(bounds.first == minimum && bounds.last == maximum,
                  "Auto-index bounds differ from the executed guest indices");
          ++auto_cases;
        }
      }
    }
    constexpr uint32_t absent = UINT32_MAX;
    const std::array offsets = {0.0f, 0.25f, 0.5f, 1.0f, -0.25f, -1.0f, 16.25f};
    const std::array scales = {0.0f, -0.0f, 0.25f, 0.5f,   0.75f,  1.0f,  1.5f,  2.0f,
                               3.0f, 4.0f,  5.0f,  -0.25f, -0.75f, -1.0f, -1.5f, -3.0f};
    for (uint32_t first : {0u, 1u, 2u, 3u, 127u}) {
      for (uint32_t count : {1u, 8u, 1024u}) {
        for (uint32_t offset_id = 0; offset_id <= offsets.size(); ++offset_id) {
          for (uint32_t scale_id = 0; scale_id <= scales.size(); ++scale_id) {
            for (bool rounded : {false, true}) {
              bool has_offset = offset_id < offsets.size(), has_scale = scale_id < scales.size();
              float offset = has_offset ? offsets[offset_id] : 0.0f;
              float scale = has_scale ? scales[scale_id] : 1.0f;
              auto bounds = rex::graphics::draw_util::GetVertexFetchIndexBounds(
                  first, first + count - 1, rounded, has_offset ? 0u : absent,
                  has_scale ? 1u : absent, [&](uint32_t id) { return id ? scale : offset; });
              Require(bool(bounds), "An exact shader index grid was rejected");
              int64_t minimum = INT64_MAX, maximum = INT64_MIN;
              for (uint32_t index = first; index < first + count; ++index) {
                volatile float value = float(index);
                if (has_offset)
                  value = value + offset;
                if (has_scale)
                  value = value * scale;
                if (rounded)
                  value = value + 0.5f;
                int64_t fetched = int64_t(std::floor(value));
                minimum = std::min(minimum, fetched);
                maximum = std::max(maximum, fetched);
                Require(fetched >= bounds.first && fetched <= bounds.last,
                        "The proof omitted an actual shader fetch");
                ++indices;
              }
              Require(bounds.first == minimum && bounds.last == maximum,
                      "The proven endpoints differ from the shader's fetched endpoints");
              ++cases;
            }
          }
        }
      }
    }
    uint64_t rounded_cases = 0, rounded_indices = 0;
    for (uint32_t first : {0u, 1u, 7u, 255u, (1u << 21) - 1}) {
      for (uint32_t count : {1u, 8u, 127u}) {
        for (float offset : {0.0f, 0.1f, -0.1f, 0.25f, -0.25f, 0x1p-40f, -0x1p-40f}) {
          for (float scale :
               {0.3f, -0.3f, 1.1f, -1.1f, std::nextafter(1.0f, 2.0f),
                std::numeric_limits<float>::min(), -std::numeric_limits<float>::min()}) {
            for (bool rounded : {false, true}) {
              auto bounds = rex::graphics::draw_util::GetVertexFetchIndexBounds(
                  first, first + count - 1, rounded, 0, 1,
                  [&](uint32_t id) { return id ? scale : offset; });
              Require(bool(bounds), "A bounded float index expression was rejected");
              for (int mode : {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO}) {
                Require(std::fesetround(mode) == 0, "Could not select CPU reference rounding");
                for (uint32_t index = first; index < first + count; ++index) {
                  volatile float value = float(index);
                  value = value + offset;
                  value = value * scale;
                  if (rounded)
                    value = value + 0.5f;
                  int64_t fetched = int64_t(std::floor(value));
                  Require(fetched >= bounds.first && fetched <= bounds.last,
                          "The proof omitted a fetch under an allowed GPU rounding direction");
                  ++rounded_indices;
                }
              }
              Require(std::fesetround(FE_TONEAREST) == 0, "Could not restore reference rounding");
              ++rounded_cases;
            }
          }
        }
      }
    }
    auto reject = [&](uint32_t first, uint32_t last, uint32_t offset_id, uint32_t scale_id,
                      float offset, float scale) {
      auto bounds = rex::graphics::draw_util::GetVertexFetchIndexBounds(
          first, last, true, offset_id, scale_id, [&](uint32_t id) { return id ? scale : offset; });
      Require(!bounds, "An unproved shader index expression was accepted");
      ++rejected;
    };
    uint64_t wide_cases = 0, wide_indices = 0;
    for (uint32_t first :
         {0u, (1u << 22) - 1, 1u << 22, (1u << 23) - 1, (1u << 23) + 1, (1u << 24) - 16,
          (1u << 24) + 1, 1u << 30, 0x7FFFFFFFu, 0x80000000u, 0xFFFFFFF0u}) {
      for (uint32_t count : {1u, 8u}) {
        for (float offset : {0.0f, -float(first), 0.1f, -0.1f, float(1u << 22), -float(1u << 22),
                             std::numeric_limits<float>::denorm_min(),
                             -std::numeric_limits<float>::denorm_min()}) {
          if (first >= (1u << 30) && offset != -float(first))
            continue;
          for (float scale : {0.0f, 1.0f, -1.0f, 0.3f, -0.3f, std::numeric_limits<float>::min(),
                              std::numeric_limits<float>::denorm_min(),
                              -std::numeric_limits<float>::denorm_min()}) {
            for (bool rounded : {false, true}) {
              auto bounds = rex::graphics::draw_util::GetVertexFetchIndexBounds(
                  first, first + count - 1, rounded, 0, 1,
                  [&](uint32_t id) { return id ? scale : offset; });
              Require(bool(bounds), "A bounded large or subnormal index expression was rejected");
              for (bool flush : {false, true}) {
                auto flush_value = [&](float value) {
                  return flush && value != 0.0f && !std::isnormal(value)
                             ? std::copysign(0.0f, value)
                             : value;
                };
                for (int mode : {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO}) {
                  Require(std::fesetround(mode) == 0, "Could not select wide reference rounding");
                  for (uint32_t i = 0; i < count; ++i) {
                    volatile float value = float(first + i);
                    value = flush_value(value + flush_value(offset));
                    value = flush_value(value * flush_value(scale));
                    if (rounded)
                      value = flush_value(value + 0.5f);
                    const double fetched = std::floor(double(value));
                    Require(
                        std::isfinite(fetched) && fetched >= bounds.first && fetched <= bounds.last,
                        "Wide index bounds omitted an executed rounding/flush result");
                    ++wide_indices;
                  }
                }
              }
              Require(std::fesetround(FE_TONEAREST) == 0,
                      "Could not restore wide reference rounding");
              ++wide_cases;
            }
          }
        }
      }
    }
    // Rounding without ALU constants still rounds large odd integer indices.
    for (uint32_t first : {(1u << 23) + 1, (1u << 24) - 3}) {
      auto bounds = rex::graphics::draw_util::GetVertexFetchIndexBounds(
          first, first + 2, true, absent, absent, [](uint32_t) { return 0.0f; });
      Require(bool(bounds), "A large rounded identity fetch was rejected");
      for (int mode : {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO}) {
        Require(std::fesetround(mode) == 0, "Could not select identity reference rounding");
        for (uint32_t i = first; i < first + 3; ++i) {
          volatile float value = float(i);
          value = value + 0.5f;
          double fetched = std::floor(double(value));
          Require(fetched >= bounds.first && fetched <= bounds.last,
                  "Rounded identity bounds omitted a large odd index");
          ++wide_indices;
        }
      }
      Require(std::fesetround(FE_TONEAREST) == 0, "Could not restore identity reference rounding");
      ++wide_cases;
    }
    for (float offset :
         {std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(),
          std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::max()})
      reject(0, 127, 0, absent, offset, 1);
    for (float scale : {std::numeric_limits<float>::infinity(),
                        std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::max()})
      reject(0, 127, absent, 1, 0, scale);
    reject(128, 127, absent, absent, 0, 1);
    reject(0, UINT32_MAX, absent, absent, 0, 1);
    reject(0, 127, 1024, absent, 0, 1);
    reject(0, 127, absent, 1024, 0, 1);
    char report[768];
    std::snprintf(
        report, sizeof(report),
        "{\"passed\":true,\"cases\":%llu,\"indices\":%llu,\"rejected\":%llu,"
        "\"auto_cases\":%llu,\"auto_indices\":%llu,"
        "\"rounded_cases\":%llu,\"rounded_indices\":%llu,"
        "\"wide_cases\":%llu,\"wide_indices\":%llu,\"packed_cases\":%llu}\n",
        static_cast<unsigned long long>(cases), static_cast<unsigned long long>(indices),
        static_cast<unsigned long long>(rejected), static_cast<unsigned long long>(auto_cases),
        static_cast<unsigned long long>(auto_indices),
        static_cast<unsigned long long>(rounded_cases),
        static_cast<unsigned long long>(rounded_indices),
        static_cast<unsigned long long>(wide_cases), static_cast<unsigned long long>(wide_indices),
        static_cast<unsigned long long>(packed_cases));
    if (argc == 3) {
      std::ofstream file(argv[2]);
      file << report;
      file.flush();
      Require(bool(file), "Could not write address proof report");
    }
    std::fputs(report, stdout);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
  }
}
