#include "simpsons_init.h"
#include "native_gpu_shader_state.h"
#include "guest_memory_fixture.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>

static uint64_t accepted[2]{};
namespace simpsons::native_gpu_calls {
bool TryShaderState(uint32_t kind, PPCContext& ctx, uint8_t* base) {
  const bool result =
      kind == 0 ? WritePixelShaderReference(ctx, base) : WriteConditionalShaderState(ctx, base);
  accepted[kind] += result;
  return result;
}
}  // namespace simpsons::native_gpu_calls
#include "native_gpu_shader_state_reference.inc"

using simpsons::LoadGuestU32;
using simpsons::StoreGuestU32;
namespace port = simpsons::native_gpu_calls;
namespace {
constexpr uint32_t kSize = 1024 * 1024, kDevice = 0x1000, kResource = 0x10000;
constexpr uint32_t kPixel = 0x20000, kStack = 0xF0000;
std::unique_ptr<GuestMemoryFixture> original, native;
std::array<uint8_t, kSize> pristine;
uint64_t cases = 0, records = 0, rejections = 0;
uint32_t current_kind = 0, current_case = 0;
void Require(bool ok, const char* why) {
  if (!ok)
    throw std::runtime_error(why);
}
PPCContext Prepare(uint32_t cursor, uint32_t source, uint32_t words) {
  std::memcpy(original->data(), pristine.data(), kSize);
  StoreGuestU32(original->data(), kDevice + 48, cursor);
  StoreGuestU32(original->data(), kDevice + 52, cursor + 1024);
  StoreGuestU32(original->data(), kDevice + 56, cursor + 512);
  StoreGuestU32(original->data(), kDevice + 10908, 0x91E10DA5);
  for (uint32_t offset : {10372, 10528, 10532, 10580, 12700, 12704})
    StoreGuestU32(original->data(), kDevice + offset, offset * 0x1020305u);
  StoreGuestU32(original->data(), kResource + 32, source);
  StoreGuestU32(original->data(), kPixel + 24, source);
  StoreGuestU32(original->data(), kPixel + 64, 0x1000);
  StoreGuestU32(original->data(), kPixel + 0x1000 + 40, 0);
  StoreGuestU32(original->data(), kPixel + 0x1000 + 44, words * 4);
  for (uint32_t i : {0u, 1u, 15u, 64u}) {
    const uint32_t relative = 0x2000 + i * 1024;
    StoreGuestU32(original->data(), kResource + (i + 112) * 8, relative);
    StoreGuestU32(original->data(), kResource + relative + 872, i * 0x40000u);
    StoreGuestU32(original->data(), kResource + relative + 876, words * 4);
  }
  for (uint32_t i = 0; i < 256; i += 4)
    StoreGuestU32(original->data(), cursor - 16 + i, i * 0x1020305u);
  std::memcpy(native->data(), original->data(), kSize);
  if (cursor >= kSize)
    for (uint32_t i = 0; i < 256; i += 4)
      StoreGuestU32(native->data(), cursor - 16 + i,
                    LoadGuestU32(original->data(), cursor - 16 + i));
  PPCContext ctx{};
  ctx.r1.u64 = 0xABCD000000000000ull | kStack;
  ctx.r3.u64 = 0x1122334455667788;
  ctx.r4.u64 = 0x9988776655443322;
  ctx.r5.u64 = 0x8899776655443322;
  ctx.r6.u64 = 0x2233445566778899;
  ctx.r7.u64 = 0x33445566778899AA;
  ctx.r8.u64 = 0x445566778899AABB;
  ctx.r9.u64 = 0x5566778899AABBCC;
  ctx.r10.u64 = 0x66778899AABBCCDD;
  ctx.r11.u64 = 0xEEEE000000000000ull | cursor;
  ctx.r12.u64 = 0x778899AABBCCDDEE;
  ctx.r14.s64 = -1;
  ctx.r15.s64 = -1;
  ctx.r17.s64 = int32_t(0xC0012700);
  ctx.r19.s64 = -1;
  ctx.r20.u64 = 0xFF11223344556677;
  ctx.r23.s64 = -1;
  ctx.r28.u64 = 0x0123456789ABCDEF;
  ctx.r29.s64 = -1;
  ctx.r30.u64 = 0xFFFF000000000000ull | kDevice;
  ctx.r31.u64 = 0xFFFF000000000000ull | kResource;
  ctx.lr = 0x8245FF04;
  ctx.xer.ca = 1;
  ctx.cr0.compare<int32_t>(-5, 7, ctx.xer);
  ctx.cr6.compare<int32_t>(-9, 2, ctx.xer);
  return ctx;
}
void EqualMemory(uint32_t cursor) {
  Require(!std::memcmp(original->data(), native->data(), kSize),
          "Shader state changed memory, metadata, versions, operands or publication");
  if (cursor >= kSize)
    for (uint32_t i = 0; i < 256; i += 4)
      Require(LoadGuestU32(original->data(), cursor - 16 + i) ==
                  LoadGuestU32(native->data(), cursor - 16 + i),
              "Shader state changed an aliased operand or padding");
}
void Check(uint32_t kind, PPCContext ctx) {
  current_kind = kind;
  current_case = uint32_t(cases);
  const uint32_t cursor = ctx.r11.u32;
  auto expected = ctx, actual = ctx;
  if (kind == 0)
    reference_0(expected, original->data());
  else
    reference_1(expected, original->data());
  const auto before = accepted[kind];
  if (kind == 0)
    patched_0(actual, native->data());
  else
    patched_1(actual, native->data());
  Require(accepted[kind] == before + 1, "Valid shader-state checkpoint fell back");
  if (std::memcmp(&expected, &actual, sizeof(actual))) {
    const auto* a = reinterpret_cast<const uint8_t*>(&expected);
    const auto* b = reinterpret_cast<const uint8_t*>(&actual);
    for (uint32_t i = 0; i < sizeof(actual); ++i)
      if (a[i] != b[i]) {
        std::fprintf(stderr, "Context offset %u expected=%02X actual=%02X\n", i, a[i], b[i]);
        break;
      }
    throw std::runtime_error("Shader state changed the complete continuation context");
  }
  uint32_t position = cursor + 4;
  while (position <= actual.r11.u32) {
    const uint32_t head = LoadGuestU32(native->data(), position);
    const uint32_t count = ((head >> 16) & 0x3FFF) + 1;
    const uint32_t opcode = (head >> 8) & 0x7F;
    uint32_t old = 0;
    if (opcode == rex::graphics::native_gpu_shader_reference::kOpcode) {
      Require(rex::graphics::native_gpu_shader_reference::IsValidHeader(head, count),
              "Invalid native shader reference");
      old = 0xC0012700 | (head & 1);
    } else if (opcode == rex::graphics::native_gpu_predication::kOpcode) {
      namespace pred = rex::graphics::native_gpu_predication;
      Require(pred::IsValidHeader(head, count), "Invalid selected-state metadata");
      old = head == pred::kMaskLowHeader    ? 0xC0006000
            : head == pred::kMaskHighHeader ? 0xC0006100
                                            : (0xC0002D01u | ((count - 1) << 16));
    } else {
      rex::graphics::native_gpu_state::StateRange range;
      Require(rex::graphics::native_gpu_state::DecodeHeader(head, count, range),
              "Original or unknown state header remains in the batch");
      old = ((count - 1) << 16) | range.first_register;
    }
    StoreGuestU32(native->data(), position, old);
    position += (count + 1) * 4;
    ++records;
  }
  Require(position == actual.r11.u32 + 4, "State records changed the final operand boundary");
  EqualMemory(cursor);
  ++cases;
}
void Rejections() {
  for (uint32_t kind : {0u, 1u})
    for (uint32_t probe = 0; probe < 12; ++probe) {
      auto ctx = Prepare(0x40000, 0xA0100000, 9);
      ctx.r29.u64 = kind == 0 ? kPixel : UINT64_MAX;
      ctx.r19.u64 = 0;
      if (probe == 0)
        ctx.r11.u32 += 1;
      if (probe == 1)
        ctx.r1.u32 += 4;
      if (probe == 2)
        ctx.r17.u32 |= 1;
      if (probe == 3)
        ctx.r30.u32 += 1;
      if (probe == 4) {
        if (kind == 0)
          ctx.r29.u32 += 1;
        else
          ctx.r31.u32 += 1;
      }
      if (probe == 5)
        ctx.r11.u32 = kStack;
      if (probe == 6)
        ctx.r11.u32 = kDevice;
      if (probe == 7)
        ctx.r11.u32 = kind == 0 ? kPixel + 24 - 4 : kResource + 32 - 4;
      if (probe == 8)
        ctx.r11.u32 = 0xA0100000;
      const uint32_t bytes_address = kind == 0 ? kPixel + 0x1000 + 44 : kResource + 0x2000 + 876;
      if (probe >= 9) {
        const uint32_t bytes = probe == 9 ? 35 : probe == 10 ? 0 : 0x40000;
        StoreGuestU32(original->data(), bytes_address, bytes);
        StoreGuestU32(native->data(), bytes_address, bytes);
      }
      StoreGuestU32(original->data(), kDevice + 52, ctx.r11.u32 + 1024);
      StoreGuestU32(native->data(), kDevice + 52, ctx.r11.u32 + 1024);
      StoreGuestU32(original->data(), kDevice + 56, ctx.r11.u32 + 512);
      StoreGuestU32(native->data(), kDevice + 56, ctx.r11.u32 + 512);
      const auto saved = ctx;
      const bool ok = kind == 0 ? port::WritePixelShaderReference(ctx, native->data())
                                : port::WriteConditionalShaderState(ctx, native->data());
      Require(!ok && !std::memcmp(&saved, &ctx, sizeof(ctx)),
              "Rejected shader state changed context");
      EqualMemory(0x40000);
      ++rejections;
    }
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
    for (uint32_t cursor : {0x40000u, 0xA0040000u, 0xC0040000u, 0xE003F000u}) {
      for (uint32_t words : {1u, 9u, 525u, 933u, 65535u})
        for (uint32_t source : {0xA0100000u, 0xC0100001u, 0xE00FF000u}) {
          auto ctx = Prepare(cursor, source, words);
          ctx.r29.u64 = 0xFFFF000000000000ull | kPixel;
          Check(0, ctx);
        }
      for (uint32_t bits = 0; bits < 64; ++bits)
        for (int32_t first : {-1, 0, 1, 15, 64})
          for (int32_t second : {-1, 0, 1, 15, 64}) {
            auto ctx = Prepare(cursor, (bits & 1) ? 0xA0100001 : 0xE00FF000, 9 + bits * 4);
            ctx.r20.u64 = (ctx.r20.u64 & ~((1ull << 56) | 0x10008ull)) | (bits & 1 ? 8 : 0) |
                          (bits & 2 ? (1ull << 56) : 0) | (bits & 4 ? 0x10000 : 0);
            ctx.r14.s64 = bits & 8 ? int32_t(0x91E10DA5) : -1;
            ctx.r23.s64 = bits & 16 ? int32_t(0xDEADBEEF) : -1;
            ctx.r29.s64 = bits & 32 ? int32_t(0x87654321) : -1;
            ctx.r15.s64 = first;
            ctx.r19.s64 = second;
            Check(1, ctx);
          }
    }
    Rejections();
    char report[384];
    std::snprintf(
        report, sizeof(report),
        "{\"passed\":true,\"cases\":%llu,\"pixel_references\":%llu,"
        "\"conditional_batches\":%llu,\"native_records\":%llu,\"unchanged_rejections\":%llu}\n",
        (unsigned long long)cases, (unsigned long long)accepted[0], (unsigned long long)accepted[1],
        (unsigned long long)records, (unsigned long long)rejections);
    FILE* out = argc == 2 ? std::fopen(argv[1], "wb") : stdout;
    Require(out, "Cannot open shader state report");
    std::fputs(report, out);
    if (out != stdout)
      Require(!std::fclose(out), "Cannot close shader state report");
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "Shader state kind=%u case=%u: %s\n", current_kind, current_case,
                 e.what());
    return 1;
  }
}
