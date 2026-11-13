// Execute the real descriptor helper with its save/restore bodies, including
// release register-locals variants. The source GPU data is not captured here.
#include "simpsons_init.h"
#include "native_gpu_indirect.h"
#include "guest_memory_fixture.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>

static uint64_t accepted = 0, attempts = 0, growth = 0;
struct GrowthBoundary {};
REX_FUNC(sub_82457CC8) {
  ++growth;
  throw GrowthBoundary{};
}
namespace simpsons::native_gpu_calls {
bool TryIndirectConstants(PPCContext& ctx, uint8_t* base) {
  ++attempts;
  const bool result = WriteIndirectConstants(ctx, base);
  accepted += result;
  return result;
}
}  // namespace simpsons::native_gpu_calls
#include "native_gpu_indirect_reference.inc"

namespace port = simpsons::native_gpu_calls;
namespace indirect = rex::graphics::native_gpu_indirect;
using simpsons::LoadGuestU32;
using simpsons::StoreGuestU32;
namespace {
constexpr uint32_t kSize = 1024 * 1024, kDevice = 0x1000, kTable = 0x10000;
constexpr uint32_t kStack = 0xF0000, kCursor = 0x20000;
std::unique_ptr<GuestMemoryFixture> original, native;
std::array<uint8_t, kSize> pristine;
uint64_t cases = 0, local_cases = 0, native_records = 0, legacy_records = 0, rejects = 0;
uint32_t current_index = 0, current_words = 0;

void Require(bool ok, const char* why) {
  if (!ok)
    throw std::runtime_error(why);
}
PPCContext Prepare(uint32_t cursor, uint32_t source, uint32_t index, uint32_t words,
                   uint32_t records) {
  std::memcpy(original->data(), pristine.data(), kSize);
  StoreGuestU32(original->data(), kDevice + 48, cursor);
  StoreGuestU32(original->data(), kDevice + 52, cursor + 1024);
  StoreGuestU32(original->data(), kDevice + 56, cursor + 512);
  StoreGuestU32(original->data(), kTable + 20, records ? 32 : 0);
  StoreGuestU32(original->data(), kTable + 48, records * 8);
  for (uint32_t i = 0; i < records; ++i) {
    StoreGuestU32(original->data(), kTable + 52 + i * 8, (index << 16) | words);
    StoreGuestU32(original->data(), kTable + 56 + i * 8, i * 16);
  }
  for (uint32_t i = 0; i < 128; i += 4)
    StoreGuestU32(original->data(), cursor - 32 + i, i * 0x10203);
  std::memcpy(native->data(), original->data(), kSize);
  for (uint32_t i = 0; i < 128; i += 4)
    StoreGuestU32(native->data(), cursor - 32 + i, LoadGuestU32(original->data(), cursor - 32 + i));
  PPCContext ctx{};
  ctx.r1.u64 = 0xABCD000000000000ull | kStack;
  ctx.r3.u64 = 0xFFFF000000000000ull | kDevice;
  ctx.r4.u64 = 0xFFFF000000000000ull | kTable;
  ctx.r5.u64 = 0xABCD000000000000ull | source;
  ctx.r25.u64 = 0x1122334455667788;
  ctx.r26.u64 = 0x1122334455667789;
  ctx.r27.u64 = 0x112233445566778A;
  ctx.r28.u64 = 0x112233445566778B;
  ctx.r29.u64 = 0x112233445566778C;
  ctx.r30.u64 = 0x112233445566778D;
  ctx.r31.u64 = 0x112233445566778E;
  ctx.lr = 0xFFFF000082123450;
  return ctx;
}
void EqualMemory(uint32_t cursor) {
  Require(!std::memcmp(original->data(), native->data(), kSize),
          "Indirect constants changed metadata, stack, operands or padding");
  if (cursor >= kSize)
    for (uint32_t i = 0; i < 128; i += 4)
      Require(LoadGuestU32(original->data(), cursor - 32 + i) ==
                  LoadGuestU32(native->data(), cursor - 32 + i),
              "Indirect constants changed aliased command payloads or padding");
}
void Check(bool locals, uint32_t cursor, uint32_t source, uint32_t index, uint32_t words,
           uint32_t records) {
  current_index = index;
  current_words = words;
  auto expected = Prepare(cursor, source, index, words, records), actual = expected;
  const auto before = accepted, growth_before = growth;
  (locals ? reference_locals : reference)(expected, original->data());
  (locals ? patched_locals : patched)(actual, native->data());
  Require(!std::memcmp(&expected, &actual, sizeof(actual)) && growth == growth_before,
          "Indirect constants changed the complete helper context or entered growth");
  uint32_t native_count = 0, legacy_count = 0;
  for (uint32_t i = 0; i < (words ? records : 0); ++i) {
    const uint32_t address = cursor + 4 + i * 16;
    const uint32_t header = LoadGuestU32(native->data(), address);
    if (header == indirect::kHeader) {
      indirect::Call call{LoadGuestU32(native->data(), address + 4),
                          LoadGuestU32(native->data(), address + 8),
                          LoadGuestU32(native->data(), address + 12)};
      Require(indirect::Validate(call), "Producer emitted invalid indirect constants");
      StoreGuestU32(native->data(), address, 0xC0022F00);
      ++native_count;
    } else {
      Require(header == 0xC0022F00, "Indirect fallback emitted an unknown record");
      ++legacy_count;
    }
  }
  const uint32_t count = records && words ? records : 0;
  const bool supported = index < 512 && words <= 2048 - index * 4;
  Require(accepted - before == native_count && native_count == (supported ? count : 0),
          "Indirect constants took an unexpected native or compatibility branch");
  EqualMemory(cursor);
  native_records += native_count;
  legacy_records += legacy_count;
  ++cases;
  local_cases += locals;
}
void Rejections() {
  for (uint32_t probe = 0; probe < 12; ++probe) {
    auto ctx = Prepare(kCursor, 0xA0020000, 0, 4, 1);
    ctx.r1.u32 -= 144;
    ctx.r29.u64 = kDevice;
    ctx.r3.u64 = kCursor;
    ctx.r30.u64 = 0xA0020000;
    ctx.r26.u64 = 0;
    ctx.r28.u64 = 4;
    ctx.r31.u64 = kTable + 60;
    ctx.r27.u64 = kTable + 60;
    switch (probe) {
      case 0:
        ctx.r3.u32 += 1;
        break;
      case 1:
        ctx.r30.u32 += 1;
        break;
      case 2:
        ctx.r30.u32 = 0x7F020000;
        break;
      case 3:
        ctx.r30.u32 = kCursor;
        break;
      case 4:
        ctx.r26.u64 = 512;
        break;
      case 5:
        ctx.r28.u64 = 2049;
        break;
      case 6:
        ctx.r31.u32 = 4;
        break;
      case 7:
        ctx.r27.u32 -= 1;
        break;
      case 8:
        ctx.r3.u32 = kDevice;
        break;
      case 9:
        ctx.r3.u32 = kStack - 144;
        break;
      case 10:
        ctx.r1.u32 += 4;
        break;
      case 11:
        ctx.r29.u32 += 1;
        break;
    }
    const auto saved = ctx;
    Require(!port::WriteIndirectConstants(ctx, native->data()) &&
                !std::memcmp(&saved, &ctx, sizeof(ctx)),
            "Rejected indirect constants changed context");
    EqualMemory(kCursor);
    ++rejects;
  }
  for (const indirect::Call call : {indirect::Call{1, 0, 4},
                                    {0x20000000, 0, 4},
                                    {0x1FFFFFFC, 0, 2},
                                    {0, 2048, 1},
                                    {0, 2047, 2},
                                    {0, 0, 0},
                                    {0, 0, 2049}})
    Require(!indirect::Validate(call), "Invalid indirect consumer arguments accepted");
}
void GrowthBoundaries() {
  for (bool locals : {false, true}) {
    auto expected = Prepare(kCursor, 0xA0020000, 0, 4, 1), actual = expected;
    StoreGuestU32(original->data(), kDevice + 56, kCursor - 4);
    StoreGuestU32(native->data(), kDevice + 56, kCursor - 4);
    const auto before = growth, checkpoint = attempts;
    try {
      (locals ? reference_locals : reference)(expected, original->data());
      Require(false, "Reference missed growth");
    } catch (const GrowthBoundary&) {}
    try {
      (locals ? patched_locals : patched)(actual, native->data());
      Require(false, "Native helper missed growth");
    } catch (const GrowthBoundary&) {}
    Require(growth == before + 2 && checkpoint == attempts &&
                !std::memcmp(&expected, &actual, sizeof(actual)),
            "Indirect constants changed the original growth boundary");
    EqualMemory(kCursor);
  }
}
}  // namespace

int main(int argc, char** argv) {
  try {
    original = std::make_unique<GuestMemoryFixture>(kSize);
    native = std::make_unique<GuestMemoryFixture>(kSize);
    for (auto* memory : {original.get(), native.get()})
      for (uint32_t address : {0xA0040000u, 0xC0040000u, 0xE003F000u})
        memory->Commit(address - 32, 8192);
    for (uint32_t i = 0; i < kSize; ++i)
      pristine[i] = uint8_t(i * 37 + i / 7);
    for (bool locals : {false, true})
      for (uint32_t cursor : {kCursor, 0xA0040000u, 0xC0040000u, 0xE003F000u})
        for (uint32_t source : {0xA0020000u, 0xC0020000u, 0xE001F000u}) {
          for (uint32_t index : {0u, 1u, 255u, 256u, 511u, 512u, 0x4000u})
            for (uint32_t words : {0u, 1u, 4u, 16u, 1024u, 2048u})
              Check(locals, cursor, source, index, words, 1);
          Check(locals, cursor, source, 256, 16, 3);
          Check(locals, cursor, source, 0, 0, 0);
        }
    Rejections();
    GrowthBoundaries();
    char report[512];
    std::snprintf(report, sizeof(report),
                  "{\"passed\":true,\"cases\":%llu,\"register_locals_cases\":%llu,"
                  "\"native_records\":%llu,\"unchanged_legacy_records\":%llu,"
                  "\"unchanged_rejections\":%llu,\"growth_boundary_calls\":%llu,"
                  "\"gpu_source_read_by_cpu_fixture\":false}\n",
                  (unsigned long long)cases, (unsigned long long)local_cases,
                  (unsigned long long)native_records, (unsigned long long)legacy_records,
                  (unsigned long long)rejects, (unsigned long long)growth);
    FILE* out = argc == 2 ? std::fopen(argv[1], "wb") : stdout;
    Require(out, "Cannot open indirect constant report");
    std::fputs(report, out);
    if (out != stdout)
      Require(!std::fclose(out), "Cannot close indirect constant report");
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "Indirect index=%u words=%u: %s\n", current_index, current_words,
                 e.what());
    return 1;
  }
}
