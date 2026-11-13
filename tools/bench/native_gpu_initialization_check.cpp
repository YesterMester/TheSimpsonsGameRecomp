// Compare initialization buffers with the original shader-setup and GPR-port
// helpers, including real memcpy/save/restore bodies. No game is launched.
#include "simpsons_init.h"
#include "native_gpu_initialization.h"
#include "guest_memory_fixture.h"
#include <rex/graphics/pipeline/shader/cache_key.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <set>
#include <type_traits>
#include <unordered_map>
#include <vector>

static uint64_t reference_calls = 0, copy_calls = 0;
#include "native_gpu_initialization_reference.inc"

namespace port = simpsons::native_gpu_calls;
namespace state = rex::graphics::native_gpu_state;
namespace program = rex::graphics::native_gpu_program;
namespace ports = rex::graphics::native_gpu_ports;
using simpsons::LoadGuestU32;
using simpsons::StoreGuestU32;

namespace {
constexpr uint32_t kSize = 1024 * 1024, kShader = 0x8206A0C8, kOutput = 0x20000, kStack = 0xF0000;
std::unique_ptr<GuestMemoryFixture> original, native;
std::array<uint8_t, kSize> pristine;
uint64_t setup_cases = 0, gpr_cases = 0, ranges = 0, shaders = 0, port_calls = 0, port_writes = 0;
uint64_t rejected = 0, patched_records = 0;
uint32_t current_index = 0, current_alignment = 0;
bool current_gpr = false;

void Require(bool ok, const char* why) {
  if (!ok)
    throw std::runtime_error(why);
}

PPCContext Prepare(bool gpr, uint32_t alignment, uint32_t index) {
  std::memcpy(original->data(), pristine.data(), kSize);
  for (uint32_t i = 0; i < 33; ++i)
    StoreGuestU32(original->data(), kShader + i * 4, index * 0x91E10DA5u + i * 0x1020305u);
  std::memcpy(native->data(), original->data(), kSize);
  std::memcpy(native->data() + kShader, original->data() + kShader, 132);
  PPCContext ctx{};
  ctx.r1.u64 = kStack;
  ctx.r3.u64 = 0xABCD000000001000ull;
  ctx.r4.u64 = 0xABCD000000000000ull | (gpr ? index : kOutput + alignment);
  ctx.r5.u64 = 0xFFFF000000000000ull | (gpr ? kOutput + alignment : kOutput + 8192);
  ctx.lr = 0xABCD000082123450ull;
  ctx.r20.u64 = 0xDEADBEEF00000020ull;
  ctx.r21.u64 = 0xDEADBEEF00000021ull;
  ctx.r22.u64 = 0xDEADBEEF00000022ull;
  ctx.r23.u64 = 0xDEADBEEF00000023ull;
  ctx.r24.u64 = 0xDEADBEEF00000024ull;
  ctx.r25.u64 = 0xDEADBEEF00000025ull;
  ctx.r26.u64 = 0xDEADBEEF00000026ull;
  ctx.r27.u64 = 0xDEADBEEF00000027ull;
  ctx.r28.u64 = 0xDEADBEEF00000028ull;
  ctx.r29.u64 = 0xDEADBEEF00000029ull;
  ctx.r30.u64 = 0xDEADBEEF00000030ull;
  ctx.r31.u64 = 0xDEADBEEF00000031ull;
  return ctx;
}

std::vector<uint32_t> Normalize(std::vector<uint32_t> words, bool count) {
  for (uint32_t at = 0; at < words.size();) {
    const uint32_t header = words[at], size = ((header >> 16) & 0x3FFF) + 1;
    Require(uint64_t(at) + size < words.size(), "Initialization record crosses its buffer");
    const uint32_t opcode = (header >> 8) & 127;
    if (state::IsOpcode(opcode)) {
      state::StateRange range;
      Require(state::DecodeHeader(header, size, range), "Invalid initialization state header");
      words[at] = ((size - 1) << 16) | range.first_register;
      ranges += count;
    } else if (opcode == program::kOpcode) {
      Require(program::IsValidHeader(header, size), "Invalid initialization program header");
      if (header == program::kInvalidateHeader) {
        words[at] = 0xC0003B00;
      } else {
        Require(program::ValidateShader(words[at + 1], words[at + 2], size),
                "Invalid initialization shader stage or size");
        words[at] = 0xC0002B00 | ((size - 1) << 16);
        shaders += count;
      }
    } else {
      uint32_t reg;
      Require(opcode == ports::kOpcode && ports::DecodeHeader(header, size, reg),
              "Invalid initialization register port");
      words[at] = ((size - 1) << 16) | reg | (size > 1 ? 0x8000 : 0);
      port_calls += count;
      port_writes += count ? size : 0;
    }
    at += size + 1;
  }
  return words;
}

void Check(bool gpr, uint32_t alignment, uint32_t index) {
  current_index = index;
  current_alignment = alignment;
  current_gpr = gpr;
  auto expected = Prepare(gpr, alignment, index), actual = expected;
  if (gpr)
    reference_sub_824449E8(expected, original->data());
  else
    reference_sub_82444700(expected, original->data());
  const auto calls_before = reference_calls, copies_before = copy_calls;
  Require(gpr ? port::WriteInitializationGpr(actual, native->data())
              : port::WriteInitializationSetup(actual, native->data()),
          "Valid initialization buffer fell back");
  Require(calls_before == reference_calls && copies_before == copy_calls,
          "Native initialization entered original helpers");
  Require(actual.r1.u64 == expected.r1.u64 && actual.r3.u64 == expected.r3.u64 &&
              actual.r3.u64 == (gpr ? 13 : 74) && actual.lr == expected.lr &&
              actual.fpscr.csr == expected.fpscr.csr,
          "Initialization changed its size, stack, link or FP state");
  for (auto reg : {&PPCContext::r20, &PPCContext::r21, &PPCContext::r22, &PPCContext::r23,
                   &PPCContext::r24, &PPCContext::r25, &PPCContext::r26, &PPCContext::r27,
                   &PPCContext::r28, &PPCContext::r29, &PPCContext::r30, &PPCContext::r31})
    Require((actual.*reg).u64 == (expected.*reg).u64, "Initialization changed a saved register");
  if (gpr)
    Require(actual.r4.u64 == expected.r4.u64 && actual.r5.u64 == expected.r5.u64,
            "GPR helper changed caller loop arguments");
  std::vector<uint32_t> old, encoded;
  for (uint32_t i = 0; i < actual.r3.u32; ++i) {
    old.push_back(LoadGuestU32(original->data(), kOutput + alignment + i * 4));
    encoded.push_back(LoadGuestU32(native->data(), kOutput + alignment + i * 4));
  }
  const auto normalized = Normalize(encoded, true);
  Require(old == normalized, "Initialization changed payloads or patch positions");
  for (uint32_t i = 0; i < normalized.size(); ++i)
    StoreGuestU32(native->data(), kOutput + alignment + i * 4, normalized[i]);
  const uint32_t frame = gpr ? 0 : 200;
  Require(!std::memcmp(original->data(), native->data(), kStack - frame) &&
              !std::memcmp(original->data() + kStack, native->data() + kStack, kSize - kStack) &&
              !std::memcmp(original->data() + kShader, native->data() + kShader, 132),
          "Initialization changed memory outside its output or private frame");
  for (uint32_t offset : gpr ? std::array{1u, 3u, 12u} : std::array{5u, 32u, 73u}) {
    auto copied = encoded, expected_copy = old;
    copied[offset] ^= 0x1020305u;
    expected_copy[offset] ^= 0x1020305u;
    Require(Normalize(copied, false) == expected_copy,
            "Copied initialization lost a patch operand");
    ++patched_records;
  }
  (gpr ? gpr_cases : setup_cases)++;
}

void NegativeChecks() {
  for (bool gpr : {false, true}) {
    for (uint32_t probe = 0; probe < (gpr ? 3u : 7u); ++probe) {
      auto ctx = Prepare(gpr, 0, 16);
      auto& output = gpr ? ctx.r5 : ctx.r4;
      switch (probe) {
        case 0:
          output.u32 += 1;
          break;
        case 1:
          output.u32 = UINT32_MAX - 3;
          break;
        case 2:
          output.u32 = 0x8FFFFFF0;
          break;
        case 3:
          ctx.r1.u32 += 4;
          break;
        case 4:
          ctx.r1.u32 = 192;
          break;
        case 5:
          output.u32 = 0x9206A0C8;
          break;
        case 6:
          output.u32 = kStack - 204;
          break;
      }
      const auto saved = ctx;
      std::vector<uint8_t> bytes(native->data(), native->data() + kSize);
      Require(!(gpr ? port::WriteInitializationGpr(ctx, native->data())
                    : port::WriteInitializationSetup(ctx, native->data())) &&
                  !std::memcmp(&ctx, &saved, sizeof(ctx)) &&
                  !std::memcmp(bytes.data(), native->data(), kSize),
              "Rejected initialization modified context or memory");
      ++rejected;
    }
  }
  for (uint32_t header : {ports::kWaitHeader, ports::kGprHeader, ports::kGprEightHeader}) {
    const uint32_t size = ((header >> 16) & 0x3FFF) + 1;
    for (uint32_t mask : {1u, 4u, 0x80u, 0x8000u, 0x10000u}) {
      uint32_t reg = 0x91E10DA5;
      Require(!ports::DecodeHeader(header ^ mask, size, reg) && reg == 0x91E10DA5,
              "Invalid initialization port accepted or altered decode output");
    }
  }
}

void ShaderKeyChecks() {
  using rex::graphics::ShaderCacheKey;
  using rex::graphics::ShaderCacheKeyHasher;
  using rex::graphics::xenos::ShaderType;
  static_assert(!std::is_constructible_v<ShaderCacheKey, uint64_t>);
  const ShaderCacheKey vertex{0x91E10DA5, ShaderType::kVertex};
  const ShaderCacheKey pixel{vertex.hash, ShaderType::kPixel};
  std::unordered_map<ShaderCacheKey, uint32_t, ShaderCacheKeyHasher> cache;
  cache.emplace(vertex, 11);
  cache.emplace(pixel, 22);
  Require(cache.size() == 2 && cache.at(vertex) == 11 && cache.at(pixel) == 22 &&
              vertex.hash == pixel.hash,
          "Shader cache merged stages or changed the raw storage identity");
  std::set<std::pair<ShaderCacheKey, uint64_t>> warmup;
  warmup.emplace(vertex, 0);
  warmup.emplace(pixel, 0);
  Require(warmup.size() == 2, "Precompile requests merged identical stage code/modifications");
  const ShaderCacheKey collision{vertex.hash ^ 0x9E3779B97F4A7C15ull, ShaderType::kPixel};
  Require(ShaderCacheKeyHasher{}(vertex) == ShaderCacheKeyHasher{}(collision),
          "Shader key collision probe does not reach the same hash bucket");
  cache.emplace(collision, 33);
  Require(cache.size() == 3 && cache.at(vertex) == 11 && cache.at(pixel) == 22 &&
              cache.at(collision) == 33,
          "Shader cache used the bucket hash instead of stage/key equality");
}
}  // namespace

int main(int argc, char** argv) {
  try {
    original = std::make_unique<GuestMemoryFixture>(kSize);
    native = std::make_unique<GuestMemoryFixture>(kSize);
    original->Commit(kShader, 132);
    native->Commit(kShader, 132);
    for (uint32_t i = 0; i < kSize; ++i)
      pristine[i] = uint8_t(i * 37 + i / 7);
    for (uint32_t alignment : {0u, 4u, 8u, 12u}) {
      for (uint32_t index = 0; index < 256; ++index) {
        Check(true, alignment, index);
        if (index < 32)
          Check(false, alignment, index);
      }
      for (uint32_t index : {0x7FFFu, 0x8000u, 0x80000000u, 0x80000001u, 0xFFFFFFFEu, UINT32_MAX})
        Check(true, alignment, index);
    }
    NegativeChecks();
    ShaderKeyChecks();
    Require(reference_calls == setup_cases + gpr_cases && copy_calls == setup_cases * 2,
            "Initialization oracle skipped its real bodies");
    char report[768];
    std::snprintf(
        report, sizeof(report),
        "{\"passed\":true,\"shader_key_checks\":4,\"cases\":%llu,\"setup_cases\":%llu,\"gpr_"
        "cases\":%llu,"
        "\"state_ranges\":%llu,\"shader_bindings\":%llu,\"port_calls\":%llu,\"port_writes\":%llu,"
        "\"patched_records\":%llu,\"unchanged_rejections\":%llu,\"real_memcpy_calls\":%llu}\n",
        (unsigned long long)(setup_cases + gpr_cases), (unsigned long long)setup_cases,
        (unsigned long long)gpr_cases, (unsigned long long)ranges, (unsigned long long)shaders,
        (unsigned long long)port_calls, (unsigned long long)port_writes,
        (unsigned long long)patched_records, (unsigned long long)rejected,
        (unsigned long long)copy_calls);
    FILE* out = argc == 2 ? std::fopen(argv[1], "wb") : stdout;
    Require(out, "Cannot open initialization report");
    std::fputs(report, out);
    if (out != stdout)
      Require(!std::fclose(out), "Cannot close initialization report");
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "Initialization gpr=%d index=%08X alignment=%u: %s\n", current_gpr,
                 current_index, current_alignment, e.what());
    return 1;
  }
}
