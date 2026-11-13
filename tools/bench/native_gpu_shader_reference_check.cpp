#include "simpsons_init.h"
#include "native_gpu_shader_reference.h"
#include "guest_memory_fixture.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>

static uint64_t accepted = 0;
namespace simpsons::native_gpu_calls {
bool TryPrimaryShaderReference(PPCContext& ctx, uint8_t* base) {
  const bool result = WritePrimaryShaderReference(ctx, base);
  accepted += result;
  return result;
}
}  // namespace simpsons::native_gpu_calls
#include "native_gpu_shader_reference_reference.inc"

namespace port = simpsons::native_gpu_calls;
namespace reference_call = rex::graphics::native_gpu_shader_reference;
using simpsons::LoadGuestU32;
using simpsons::StoreGuestU32;
namespace {
constexpr uint32_t kSize = 1024 * 1024, kDevice = 0x1000, kResource = 0x10000;
constexpr uint32_t kStack = 0xF0000;
std::unique_ptr<GuestMemoryFixture> original, native;
std::array<uint8_t, kSize> pristine;
uint64_t cases = 0, rejections = 0;
uint32_t current_variant = 0, current_words = 0;
void Require(bool ok, const char* why) {
  if (!ok)
    throw std::runtime_error(why);
}
PPCContext Prepare(uint32_t cursor, uint32_t source, uint32_t words, uint32_t variant) {
  std::memcpy(original->data(), pristine.data(), kSize);
  StoreGuestU32(original->data(), kDevice + 48, cursor);
  StoreGuestU32(original->data(), kDevice + 52, cursor + 1024);
  StoreGuestU32(original->data(), kDevice + 56, cursor + 512);
  StoreGuestU32(original->data(), kDevice + 10908, 0x91E10DA5u ^ variant);
  StoreGuestU32(original->data(), kResource + 32, source);
  StoreGuestU32(original->data(), kResource + (variant + 112) * 8, 0x2000);
  StoreGuestU32(original->data(), kResource + 0x2000 + 872, 0);
  StoreGuestU32(original->data(), kResource + 0x2000 + 876, words * 4);
  for (uint32_t i = 0; i < 64; i += 4)
    StoreGuestU32(original->data(), cursor - 16 + i, i * 0x1020305u);
  std::memcpy(native->data(), original->data(), kSize);
  for (uint32_t i = 0; i < 64; i += 4)
    StoreGuestU32(native->data(), cursor - 16 + i, LoadGuestU32(original->data(), cursor - 16 + i));
  PPCContext ctx{};
  ctx.r1.u64 = 0xABCD000000000000ull | kStack;
  ctx.r3.u64 = 0x1122334455667788;
  ctx.r6.u64 = 0x2233445566778899;
  ctx.r7.u64 = 0x33445566778899AA;
  ctx.r8.u64 = 0x445566778899AABB;
  ctx.r9.u64 = 0x5566778899AABBCC;
  ctx.r10.u64 = 0x66778899AABBCCDD;
  ctx.r11.u64 = 0xEEEE000000000000ull | cursor;
  ctx.r17.s64 = int32_t(0xC0012700);
  ctx.r19.u64 = variant;
  ctx.r30.u64 = 0xFFFF000000000000ull | kDevice;
  ctx.r31.u64 = 0xFFFF000000000000ull | kResource;
  ctx.lr = 0x824601C0;
  return ctx;
}
void EqualMemory(uint32_t cursor) {
  Require(!std::memcmp(original->data(), native->data(), kSize),
          "Shader reference changed memory, metadata, version or publication");
  if (cursor >= kSize)
    for (uint32_t i = 0; i < 64; i += 4)
      Require(LoadGuestU32(original->data(), cursor - 16 + i) ==
                  LoadGuestU32(native->data(), cursor - 16 + i),
              "Shader reference changed an aliased operand or padding");
}
void Check(uint32_t cursor, uint32_t source, uint32_t words, uint32_t variant) {
  current_variant = variant;
  current_words = words;
  auto expected = Prepare(cursor, source, words, variant), actual = expected;
  reference(expected, original->data());
  const auto before = accepted;
  patched(actual, native->data());
  Require(accepted == before + 1 && !std::memcmp(&expected, &actual, sizeof(actual)),
          "Shader reference changed the complete continuation context");
  Require(LoadGuestU32(native->data(), cursor + 4) == reference_call::kHeader &&
              reference_call::Validate({LoadGuestU32(native->data(), cursor + 8),
                                        LoadGuestU32(native->data(), cursor + 12)}),
          "Shader reference emitted an original or invalid header");
  StoreGuestU32(native->data(), cursor + 4, 0xC0012700);
  EqualMemory(cursor);
  ++cases;
}
void Rejections() {
  for (uint32_t probe = 0; probe < 13; ++probe) {
    auto ctx = Prepare(0x20000, 0xA0100000, 9, 0);
    switch (probe) {
      case 0:
        ctx.r11.u32 += 1;
        break;
      case 1:
        ctx.r19.u64 = UINT64_MAX;
        break;
      case 2:
        ctx.r17.u32 |= 1;
        break;
      case 3:
        ctx.r1.u32 += 4;
        break;
      case 4:
        ctx.r31.u32 += 1;
        break;
      case 5:
        ctx.r30.u32 += 1;
        break;
      case 6:
        ctx.r11.u32 = kStack;
        break;
      case 7:
        ctx.r11.u32 = kResource + 896 - 4;
        break;
      case 8:
        ctx.r11.u32 = 0xA0100000;
        break;
      case 9:
        StoreGuestU32(original->data(), kResource + 0x2000 + 876, 35);
        StoreGuestU32(native->data(), kResource + 0x2000 + 876, 35);
        break;
      case 10:
        StoreGuestU32(original->data(), kResource + 32, 0xA0100002);
        StoreGuestU32(native->data(), kResource + 32, 0xA0100002);
        break;
      case 11:
        StoreGuestU32(original->data(), kResource + 0x2000 + 876, 0x40000);
        StoreGuestU32(native->data(), kResource + 0x2000 + 876, 0x40000);
        break;
      case 12:
        ctx.r11.u32 = kDevice;
        break;
    }
    const uint32_t capacity = ctx.r11.u32 + 1024;
    StoreGuestU32(original->data(), kDevice + 52, capacity);
    StoreGuestU32(native->data(), kDevice + 52, capacity);
    StoreGuestU32(original->data(), kDevice + 56, capacity - 512);
    StoreGuestU32(native->data(), kDevice + 56, capacity - 512);
    const auto saved = ctx;
    Require(!port::WritePrimaryShaderReference(ctx, native->data()) &&
                !std::memcmp(&saved, &ctx, sizeof(ctx)),
            "Rejected shader reference changed context");
    EqualMemory(0x20000);
    ++rejections;
  }
  for (uint32_t mask : {2u, 4u, 0x80u, 0x8000u, 0x10000u})
    Require(!reference_call::IsValidHeader(reference_call::kHeader ^ mask, 2),
            "Malformed shader reference header accepted");
  for (const reference_call::Call c :
       {reference_call::Call{2, 9}, {3, 9}, {0, 0}, {0, 0x10000}, {0x1FFFFFFC, 9}, {0x20000000, 1}})
    Require(!reference_call::Validate(c), "Invalid shader reference arguments accepted");
}
}  // namespace
int main(int argc, char** argv) {
  try {
    original = std::make_unique<GuestMemoryFixture>(kSize);
    native = std::make_unique<GuestMemoryFixture>(kSize);
    for (auto* memory : {original.get(), native.get()})
      for (uint32_t address : {0xA0040000u, 0xC0040000u, 0xE003F000u})
        memory->Commit(address - 16, 8192);
    for (uint32_t i = 0; i < kSize; ++i)
      pristine[i] = uint8_t(i * 37 + i / 7);
    for (uint32_t cursor : {0x20000u, 0xA0040000u, 0xC0040000u, 0xE003F000u})
      for (uint32_t source : {0xA0100000u, 0xC0100000u, 0xE00FF000u})
        for (uint32_t stage : {0u, 1u})
          for (uint32_t words : {1u, 9u, 15u, 525u, 933u, 65535u})
            for (uint32_t variant : {0u, 15u, 64u})
              Check(cursor + stage * 4, source | stage, words, variant);
    Rejections();
    char report[384];
    std::snprintf(report, sizeof(report),
                  "{\"passed\":true,\"cases\":%llu,\"native_references\":%llu,"
                  "\"unchanged_rejections\":%llu,\"gpu_code_read_by_cpu_fixture\":false}\n",
                  (unsigned long long)cases, (unsigned long long)accepted,
                  (unsigned long long)rejections);
    FILE* out = argc == 2 ? std::fopen(argv[1], "wb") : stdout;
    Require(out, "Cannot open shader reference report");
    std::fputs(report, out);
    if (out != stdout)
      Require(!std::fclose(out), "Cannot close shader reference report");
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "Shader reference variant=%u words=%u: %s\n", current_variant,
                 current_words, e.what());
    return 1;
  }
}
