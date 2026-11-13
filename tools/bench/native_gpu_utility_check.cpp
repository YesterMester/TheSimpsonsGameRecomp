// Compare the bounded utility builder with the actual generated routine,
// including its real memcpy and register save/restore helpers. No game runs.
#include "simpsons_init.h"
#include "native_gpu_utility.h"
#include "guest_memory_fixture.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <vector>

static uint64_t reference_calls = 0, copy_calls = 0;
#include "native_gpu_utility_reference.inc"

namespace program = rex::graphics::native_gpu_program;
namespace state = rex::graphics::native_gpu_state;
namespace coherency = rex::graphics::native_gpu_coherency;
namespace calls = rex::graphics::native_gpu_calls;
using simpsons::LoadGuestU32;
using simpsons::StoreGuestU32;

namespace {
constexpr uint32_t kSize = 1024 * 1024, kDevice = 0x1000, kCursor = 0x20000;
constexpr uint32_t kStack = 0xF0000, kShader = 0x8206BF28;
std::unique_ptr<GuestMemoryFixture> original, native;
std::array<uint8_t, kSize> pristine;
constexpr std::array<PPCRegister PPCContext::*, 18> kNonvolatile = {
    &PPCContext::r14, &PPCContext::r15, &PPCContext::r16, &PPCContext::r17, &PPCContext::r18,
    &PPCContext::r19, &PPCContext::r20, &PPCContext::r21, &PPCContext::r22, &PPCContext::r23,
    &PPCContext::r24, &PPCContext::r25, &PPCContext::r26, &PPCContext::r27, &PPCContext::r28,
    &PPCContext::r29, &PPCContext::r30, &PPCContext::r31};
uint64_t cases = 0, waits = 0, rejections = 0, state_ranges = 0, patched_copies = 0;
uint32_t current_mode = 0, current_width = 0, current_height = 0, current_alignment = 0;

void Require(bool ok, const char* why) {
  if (!ok)
    throw std::runtime_error(why);
}

std::vector<uint32_t> Normalize(std::vector<uint32_t> data, bool count) {
  uint32_t shaders = 0, invalidations = 0, draws = 0, coherent = 0;
  for (size_t at = 0; at < data.size();) {
    const uint32_t header = data[at];
    Require((header >> 30) == 3, "Utility builder retained a legacy state packet");
    const uint32_t size = ((header >> 16) & 0x3FFF) + 1;
    Require(at + size < data.size(), "Native utility record crosses the published end");
    const uint32_t opcode = (header >> 8) & 127;
    if (state::IsOpcode(opcode)) {
      state::StateRange range;
      Require(state::DecodeHeader(header, size, range), "Malformed native utility state");
      data[at] = ((size - 1) << 16) | range.first_register;
      state_ranges += count;
    } else if (opcode == program::kOpcode) {
      Require(program::IsValidHeader(header, size), "Malformed native utility program");
      if (header == program::kInvalidateHeader) {
        data[at] = 0xC0003B00;
        ++invalidations;
      } else {
        Require(program::ValidateShader(data[at + 1], data[at + 2], size),
                "Malformed native utility shader");
        data[at] = 0xC0002B00 | ((size - 1) << 16);
        ++shaders;
      }
    } else if (opcode == coherency::kOpcode) {
      coherency::Call call;
      Require(header == coherency::kHeader && coherency::Decode({data.data() + at + 1, size}, call),
              "Malformed native utility coherency");
      const std::array<uint32_t, 13> legacy = {0x2007,    call.destination_base,
                                               0xA31,     call.status,
                                               0x10A2F,   call.size,
                                               call.base, 0xC0043C00,
                                               3,         0xA31,
                                               0,         0x80000000u,
                                               8};
      std::copy(legacy.begin(), legacy.end(), data.begin() + at);
      ++coherent;
    } else {
      Require(opcode == calls::kBareDrawOpcode && size == 1 && (header & 0x80FFu) == 1,
              "Unexpected native utility operation");
      data[at] = 0xC0003601;
      ++draws;
    }
    at += size + 1;
  }
  Require(shaders == 1 && invalidations == 1 && draws == 1 && coherent == (current_mode != 3),
          "Utility operation coverage changed");
  return data;
}

PPCContext Prepare(uint32_t mode, uint32_t width, uint32_t height, uint32_t alignment,
                   uint32_t variant) {
  auto* base = original->data();
  std::memcpy(base, pristine.data(), kSize);
  StoreGuestU32(base, kDevice + 48, kCursor + alignment);
  const uint32_t bytes = mode == 3 ? 248 : 292;
  // Exact end and exact pre-final growth limits are accepted.
  StoreGuestU32(base, kDevice + 52, kCursor + alignment + bytes);
  StoreGuestU32(base, kDevice + 56, kCursor + alignment + bytes - 16);
  StoreGuestU32(base, kDevice + 10396, variant * 0x9E3779B9u);
  for (uint32_t i = 0; i < 15; ++i)
    StoreGuestU32(base, kShader + i * 4, (variant + i) * 0x1020305u);
  std::memcpy(native->data(), base, kSize);
  std::memcpy(native->data() + kShader, base + kShader, 60);
  PPCContext ctx{};
  ctx.r1.u64 = kStack;
  ctx.r3.u64 = kDevice;
  ctx.r4.u64 = 0xABCD000000000000ull | mode;
  constexpr std::array<uint32_t, 8> addresses = {0,          0x100004,   0xA0123456, 0xC0987654,
                                                 0xE0123456, 0x7F123457, 0x90123457, UINT32_MAX};
  ctx.r5.u64 = addresses[variant % addresses.size()];
  ctx.r6.u64 = 0xFFFF000000000000ull | width;
  ctx.r7.u64 = 0xDEAD000000000000ull | height;
  ctx.r8.u64 = addresses[(variant / addresses.size()) % addresses.size()];
  ctx.lr = 0xABCD000082123450ull;
  for (uint32_t i = 0; i < kNonvolatile.size(); ++i)
    (ctx.*kNonvolatile[i]).u64 = 0xDEADBEEF00000000ull + i;
  return ctx;
}

void Check(uint32_t mode, uint32_t width, uint32_t height, uint32_t alignment, uint32_t variant) {
  current_mode = mode;
  current_width = width;
  current_height = height;
  current_alignment = alignment;
  auto expected = Prepare(mode, width, height, alignment, variant), actual = expected;
  reference_sub_8246AF58(expected, original->data());
  const uint64_t before = reference_calls, copies = copy_calls;
  Require(simpsons::native_gpu_calls::WriteUtilityDraw(actual, native->data()),
          "Supported native utility rejected");
  Require(reference_calls == before && copy_calls == copies,
          "Native utility called original helpers");
  Require(actual.r1.u64 == expected.r1.u64 && actual.r3.u64 == expected.r3.u64 &&
              actual.lr == expected.lr && actual.fpscr.csr == expected.fpscr.csr,
          "Native utility changed its return, stack, link or FP state");
  for (auto reg : kNonvolatile)
    Require((actual.*reg).u64 == (expected.*reg).u64, "Native utility changed a saved register");
  const uint32_t initial = kCursor + alignment;
  const uint32_t end = LoadGuestU32(original->data(), kDevice + 48);
  Require(
      end == initial + (mode == 3 ? 248 : 292) && end == LoadGuestU32(native->data(), kDevice + 48),
      "Native utility changed its cursor advance");
  std::vector<uint32_t> source, encoded;
  for (uint32_t p = initial + 4; p <= end; p += 4) {
    source.push_back(LoadGuestU32(original->data(), p));
    encoded.push_back(LoadGuestU32(native->data(), p));
  }
  const auto normalized = Normalize(encoded, true);
  Require(source == normalized, "Native utility changed an operand or original patch offset");
  for (uint32_t i = 0; i < normalized.size(); ++i)
    StoreGuestU32(native->data(), initial + 4 + i * 4, normalized[i]);
  Require(!std::memcmp(original->data(), native->data(), kStack - 200) &&
              !std::memcmp(original->data() + kStack, native->data() + kStack, kSize - kStack),
          "Native utility changed memory outside its commands or scratch stack");
  Require(!std::memcmp(original->data() + kShader, native->data() + kShader, 60),
          "Native utility changed its shader source");
  // Copy the record, then patch its inline shader and destination operands
  // through exactly the offsets used by the original sequence.
  auto patched = encoded;
  auto patched_source = source;
  for (uint32_t offset : {13u + variant % 15, 57u}) {
    patched[offset] ^= 0x1020304u;
    patched_source[offset] ^= 0x1020304u;
  }
  Require(Normalize(patched, false) == patched_source, "Copied utility lost its patch operands");
  ++patched_copies;
  ++cases;
  waits += mode != 3;
}

void NegativeChecks() {
  for (uint32_t probe = 0; probe < 17; ++probe) {
    auto ctx = Prepare(probe & 1 ? 3 : 0, 1280, 720, 0, probe);
    auto* base = native->data();
    const uint32_t bytes = ctx.r4.u32 == 3 ? 248 : 292;
    switch (probe) {
      case 0:
        StoreGuestU32(base, kDevice + 56, kCursor - 4);
        break;
      case 1:
        StoreGuestU32(base, kDevice + 56, kCursor + 148 - 4);
        break;
      case 2:
        StoreGuestU32(base, kDevice + 56, kCursor + 188 - 4);
        break;
      case 3:
        StoreGuestU32(base, kDevice + 56, kCursor + 224 - 4);
        break;
      case 4:
        StoreGuestU32(base, kDevice + 56, kCursor + 232 - 4);
        break;
      case 5:
        StoreGuestU32(base, kDevice + 56, kCursor + bytes - 20);
        break;
      case 6:
        StoreGuestU32(base, kDevice + 52, kCursor + bytes - 4);
        break;
      case 7:
        StoreGuestU32(base, kDevice + 48, kCursor + 1);
        break;
      case 8:
        ctx.r3.u32 += 1;
        break;
      case 9:
        ctx.r1.u32 += 4;
        break;
      case 10:
        ctx.r1.u32 = 192;
        break;
      case 11:
        StoreGuestU32(base, kDevice + 48, UINT32_MAX - 3);
        break;
      case 12:
        StoreGuestU32(base, kDevice + 48, kDevice + 256);
        break;
      case 13:
        StoreGuestU32(base, kDevice + 48, kStack - 204);
        break;
      case 14:
        StoreGuestU32(base, kDevice + 48, 0x9206BF24);
        break;
      case 15:
        ctx.r1.u32 = kDevice + 256;
        break;
      case 16:
        ctx.r1.u32 = 0x9206BF30;
        break;
    }
    if (probe >= 11) {
      StoreGuestU32(base, kDevice + 52, UINT32_MAX);
      StoreGuestU32(base, kDevice + 56, UINT32_MAX);
    }
    const auto saved = ctx;
    std::vector<uint8_t> saved_memory(base, base + kSize);
    const uint64_t helpers = reference_calls + copy_calls;
    Require(!simpsons::native_gpu_calls::WriteUtilityDraw(ctx, base) &&
                !std::memcmp(&ctx, &saved, sizeof(ctx)) &&
                !std::memcmp(base, saved_memory.data(), kSize) &&
                helpers == reference_calls + copy_calls,
            "Rejected utility changed memory/context or entered a helper");
    ++rejections;
  }
  uint32_t header;
  Require(!program::EncodeShaderHeader(0, header) && !program::EncodeShaderHeader(16383, header),
          "Invalid program length accepted");
  for (uint32_t size : {1u, 15u, 192u, 16382u}) {
    Require(program::EncodeShaderHeader(size, header) &&
                program::IsShaderHeader(header, size + 2) &&
                program::ValidateShader(0, size, size + 2) &&
                program::ValidateShader(1, size, size + 2),
            "Valid program boundary rejected");
    for (uint32_t mask : {1u, 4u, 8u, 0x80u, 0x8000u, 0x10000u, 0x40000000u})
      Require(!program::IsShaderHeader(header ^ mask, size + 2),
              "Noncanonical shader header accepted");
    Require(!program::ValidateShader(2, size, size + 2) &&
                !program::ValidateShader(0, size | 0x10000, size + 2) &&
                !program::ValidateShader(0, size, size + 1),
            "Invalid shader arguments accepted");
  }
  Require(program::IsValidHeader(program::kInvalidateHeader, 1) &&
              !program::IsValidHeader(program::kInvalidateHeader | 1, 1) &&
              !program::IsValidHeader(program::kInvalidateHeader, 2),
          "Invalid invalidation header accepted");
}
}  // namespace

int main(int argc, char** argv) {
  try {
    original = std::make_unique<GuestMemoryFixture>(kSize);
    native = std::make_unique<GuestMemoryFixture>(kSize);
    original->Commit(kShader, 60);
    native->Commit(kShader, 60);
    for (uint32_t i = 0; i < kSize; ++i)
      pristine[i] = uint8_t(i * 37 + i / 7);
    uint32_t variant = 0;
    for (uint32_t mode : {0u, 1u, 2u, 3u, 4u, UINT32_MAX})
      for (uint32_t width : {0u, 1u, 31u, 32u, 33u, 640u, 1280u, 16383u, 16384u, 0xFFFFFFE0u,
                             0xFFFFFFEFu, UINT32_MAX})
        for (uint32_t height : {0u, 1u, 720u, 0x3FFFu, 0x4000u, UINT32_MAX})
          for (uint32_t alignment : {0u, 4u, 8u, 12u})
            Check(mode, width, height, alignment, variant++);
    NegativeChecks();
    Require(reference_calls == cases && copy_calls == cases, "Oracle skipped its real helpers");
    char report[768];
    std::snprintf(
        report, sizeof(report),
        "{\"passed\":true,\"cases\":%llu,\"coherency_waits\":%llu,"
        "\"state_ranges\":%llu,\"copied_patched_records\":%llu,"
        "\"unchanged_rejections\":%llu,\"real_reference_calls\":%llu,\"real_memcpy_calls\":%llu}\n",
        (unsigned long long)cases, (unsigned long long)waits, (unsigned long long)state_ranges,
        (unsigned long long)patched_copies, (unsigned long long)rejections,
        (unsigned long long)reference_calls, (unsigned long long)copy_calls);
    FILE* output = argc == 2 ? std::fopen(argv[1], "wb") : stdout;
    Require(output, "Cannot open oracle output");
    std::fputs(report, output);
    if (output != stdout)
      Require(!std::fclose(output), "Cannot close oracle output");
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "Utility case mode=%u width=%u height=%u alignment=%u: %s\n", current_mode,
                 current_width, current_height, current_alignment, e.what());
    return 1;
  }
}
