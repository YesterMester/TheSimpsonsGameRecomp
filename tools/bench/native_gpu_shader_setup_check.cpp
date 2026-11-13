// Compare each actual setup segment through its real memcpy. Shader patchers,
// allocation and submission remain in the original caller, outside this check.
#include "simpsons_init.h"
#include "native_gpu_shader_setup.h"
#include "guest_memory_fixture.h"
#include <rex/ppc/barriers.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>

static uint64_t reference_calls = 0, copy_calls = 0, accepted = 0;
namespace simpsons::native_gpu_calls {
bool TryShaderSetup(uint32_t kind, PPCContext& ctx, uint8_t* base) {
  const bool result = kind == 0   ? WriteResourceShaderPrefix(ctx, base)
                      : kind == 1 ? WriteCopyVertexShaderPrefix(ctx, base)
                                  : WriteCopyPixelShaderPrefix(ctx, base);
  accepted += result;
  return result;
}
}  // namespace simpsons::native_gpu_calls
#include "native_gpu_shader_setup_reference.inc"

namespace port = simpsons::native_gpu_calls;
namespace program = rex::graphics::native_gpu_program;
using simpsons::LoadGuestU32;
using simpsons::StoreGuestU32;
namespace {
constexpr uint32_t kSize = 1024 * 1024, kDevice = 0x1000, kResource = 0x10000;
constexpr uint32_t kStack = 0xF0000, kSource = 0xC0030000, kTable = 0x82010000;
std::unique_ptr<GuestMemoryFixture> original, native;
std::array<uint8_t, kSize> pristine;
uint64_t cases = 0, rejections = 0;
uint32_t current_kind = 0, current_words = 0;

void Require(bool ok, const char* why) {
  if (!ok)
    throw std::runtime_error(why);
}
PPCContext Prepare(uint32_t kind, uint32_t cursor, uint32_t words, uint32_t selector) {
  std::memcpy(original->data(), pristine.data(), kSize);
  StoreGuestU32(original->data(), kDevice + 52, cursor + 0x20000);
  StoreGuestU32(original->data(), kDevice + 56, cursor + 0x1F000);
  const uint32_t offset = (selector + 112) * 8;
  StoreGuestU32(original->data(), kResource + offset, 0x400);
  StoreGuestU32(original->data(), kResource + 32, 0xA0030000);
  StoreGuestU32(original->data(), kResource + 0x400 + 872, 0);
  StoreGuestU32(original->data(), kResource + 0x400 + 876, words * 4);
  for (uint32_t i = 0; i < 65584; i += 4)
    StoreGuestU32(original->data(), cursor - 16 + i, i * 0x91E10DA5u + words);
  const uint32_t source = kind == 0 ? kSource : kind == 1 ? kTable : kTable + 2100;
  for (uint32_t i = 0; i < words; ++i)
    StoreGuestU32(original->data(), source + i * 4, i * 0x1020305u ^ (words * 0x91E10DA5u));
  std::memcpy(native->data(), original->data(), kSize);
  for (uint32_t i = 0; i < 65584; i += 4)
    StoreGuestU32(native->data(), cursor - 16 + i, LoadGuestU32(original->data(), cursor - 16 + i));
  for (uint32_t i = 0; i < words; ++i)
    StoreGuestU32(native->data(), source + i * 4, LoadGuestU32(original->data(), source + i * 4));
  PPCContext ctx{};
  ctx.r1.u64 = 0xABCD000000000000ull | kStack;
  ctx.r3.u64 = 0xEEEE000000000000ull | cursor;
  ctx.r4.u64 = 0x1122334455667788;
  ctx.r5.u64 = 0x2233445566778899;
  ctx.r7.u64 = 0x33445566778899AA;
  ctx.r8.u64 = 0x445566778899AABB;
  ctx.r9.u64 = 0x5566778899AABBCC;
  ctx.r10.u64 = 0x66778899AABBCCDD;
  ctx.r11.u64 = 0x778899AABBCCDDEE;
  ctx.lr = 0x8246C960;
  if (kind == 0) {
    ctx.r27.u64 = words * 4;
    ctx.r28.u64 = words;
    ctx.r29.u64 = 0xFFFF000000000000ull | kDevice;
    ctx.r30.u64 = offset;
    ctx.r31.u64 = 0xFFFF000000000000ull | kResource;
  } else {
    ctx.r26.u64 = 0xFFFFFFFF00000000ull | kTable;
    ctx.r27.u64 = 0xFFFFFFFF00000000ull | source;
    ctx.r29.u64 = words;
    ctx.r31.u64 = 0xFFFF000000000000ull | kDevice;
  }
  return ctx;
}
void EqualMemory(uint32_t cursor, uint32_t bytes) {
  Require(!std::memcmp(original->data(), native->data(), kSize),
          "Shader setup changed memory, source metadata or its private frame");
  if (cursor >= kSize)
    for (uint32_t i = 0; i < bytes + 32; i += 4)
      Require(LoadGuestU32(original->data(), cursor - 16 + i) ==
                  LoadGuestU32(native->data(), cursor - 16 + i),
              "Shader setup changed aliased code, operand positions or padding");
}
void Check(uint32_t kind, uint32_t cursor, uint32_t words, uint32_t selector) {
  current_kind = kind;
  current_words = words;
  auto expected = Prepare(kind, cursor, words, selector), actual = expected;
  const auto before = accepted, copies = copy_calls;
  const std::array reference = {reference_0, reference_1, reference_2};
  const std::array patched = {patched_0, patched_1, patched_2};
  reference[kind](expected, original->data());
  patched[kind](actual, native->data());
  Require(accepted == before + 1 && copy_calls == copies + 2,
          "Shader setup bypassed native metadata or its real memcpy");
  Require(!std::memcmp(&expected, &actual, sizeof(actual)),
          "Shader setup changed the complete memcpy/continuation context");
  uint32_t at = cursor + 4;
  if (kind != 2) {
    Require(LoadGuestU32(native->data(), at) == program::kInvalidateHeader,
            "Shader setup retained an original invalidation header");
    StoreGuestU32(native->data(), at, 0xC0003B00);
    at += 8;
  }
  uint32_t header;
  Require(program::EncodeShaderHeader(words, header) &&
              LoadGuestU32(native->data(), at) == header &&
              LoadGuestU32(native->data(), at + 4) == (kind == 2 ? 1u : 0u) &&
              LoadGuestU32(native->data(), at + 8) == words,
          "Shader setup changed its stage, size or native header");
  StoreGuestU32(native->data(), at, 0xC0002B00 | ((words + 1) << 16));
  EqualMemory(cursor, (words + (kind == 2 ? 3 : 5)) * 4);
  const uint32_t source = kind == 0 ? kSource : kind == 1 ? kTable : kTable + 2100;
  for (uint32_t i = 0; i < words; ++i)
    Require(LoadGuestU32(original->data(), source + i * 4) ==
                LoadGuestU32(native->data(), source + i * 4),
            "Shader setup changed its copied source");
  ++cases;
}
void Rejections() {
  for (uint32_t kind = 0; kind < 3; ++kind)
    for (uint32_t probe = 0; probe < 9; ++probe) {
      const uint32_t words = kind == 2 ? 15 : 525, prefix = kind == 2 ? 3 : 5;
      auto ctx = Prepare(kind, 0x20000, words, 0);
      switch (probe) {
        case 0:
          ctx.r3.u32 += 1;
          break;
        case 1:
          ctx.r3.u32 = 0;
          break;
        case 2:
          if (kind == 0)
            ctx.r28.u64 = 0;
          else if (kind == 1)
            ctx.r29.u64 = 0;
          else
            ctx.r26.u32 += 1;
          break;
        case 3:
          ctx.r3.u32 = (kind == 0 ? kSource : kind == 1 ? kTable : kTable + 2100) - 4;
          break;
        case 4:
          ctx.r1.u32 += 4;
          break;
        case 5:
          if (kind == 0)
            ctx.r31.u32 += 1;
          else if (kind == 1)
            ctx.r27.u32 += 1;
          else
            ctx.r26.u32 += 1;
          break;
        case 6:
          break;
        case 7:
          ctx.r3.u32 = kStack - (words + prefix) * 4 - 4;
          break;
        case 8:
          if (kind == 0)
            ctx.r3.u32 = kResource + (112 * 8) - 4;
          else
            ctx.r3.u32 = kDevice;
          break;
      }
      const uint32_t capacity = ctx.r3.u32 + (words + prefix) * 4 + (probe == 6 ? -4u : 512);
      StoreGuestU32(original->data(), kDevice + 52, capacity);
      StoreGuestU32(native->data(), kDevice + 52, capacity);
      const auto saved = ctx;
      const bool result = kind == 0   ? port::WriteResourceShaderPrefix(ctx, native->data())
                          : kind == 1 ? port::WriteCopyVertexShaderPrefix(ctx, native->data())
                                      : port::WriteCopyPixelShaderPrefix(ctx, native->data());
      Require(!result && !std::memcmp(&saved, &ctx, sizeof(ctx)),
              "Rejected shader prefix changed context");
      EqualMemory(0x20000, (words + prefix) * 4);
      ++rejections;
    }
}
}  // namespace
int main(int argc, char** argv) {
  try {
    original = std::make_unique<GuestMemoryFixture>(kSize);
    native = std::make_unique<GuestMemoryFixture>(kSize);
    for (auto* memory : {original.get(), native.get()}) {
      memory->Commit(kSource, 0x11000);
      memory->Commit(kTable, 0x11000);
      for (uint32_t address : {0xA0040000u, 0xC0040000u, 0xE003F000u})
        memory->Commit(address - 16, 0x12000);
    }
    for (uint32_t i = 0; i < kSize; ++i)
      pristine[i] = uint8_t(i * 37 + i / 7);
    for (uint32_t cursor : {0x20000u, 0xA0040000u, 0xC0040000u, 0xE003F000u})
      for (uint32_t alignment : {0u, 4u, 8u, 12u}) {
        for (uint32_t words : {1u, 2u, 3u, 15u, 24u, 525u, 933u, 16382u}) {
          for (uint32_t selector : {0u, 15u})
            Check(0, cursor + alignment, words, selector);
          Check(1, cursor + alignment, words, 0);
        }
        Check(2, cursor + alignment, 15, 0);
      }
    Rejections();
    char report[384];
    std::snprintf(report, sizeof(report),
                  "{\"passed\":true,\"cases\":%llu,\"real_memcpy_calls\":%llu,"
                  "\"native_prefixes\":%llu,\"unchanged_rejections\":%llu,"
                  "\"allocator_shader_patchers_executed\":false}\n",
                  (unsigned long long)cases, (unsigned long long)copy_calls,
                  (unsigned long long)accepted, (unsigned long long)rejections);
    FILE* out = argc == 2 ? std::fopen(argv[1], "wb") : stdout;
    Require(out, "Cannot open shader setup report");
    std::fputs(report, out);
    if (out != stdout)
      Require(!std::fclose(out), "Cannot close shader setup report");
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "Shader setup kind=%u words=%u: %s\n", current_kind, current_words,
                 e.what());
    return 1;
  }
}
