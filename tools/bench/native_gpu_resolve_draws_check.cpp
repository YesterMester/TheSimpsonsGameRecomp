// Execute the original and patched checkpoints, including the actual original
// split-loop control and the register-locals rewrite used by release builds.
#include "simpsons_init.h"
#include "native_gpu_resolve_draws.h"
#include "guest_memory_fixture.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>

static uint64_t native_checkpoints = 0, growth_boundaries = 0;
struct GrowthBoundary {};
REX_FUNC(sub_82457CC8) {
  ++growth_boundaries;
  throw GrowthBoundary{};
}
namespace simpsons::native_gpu_calls {
bool TryClearResolveDraw(PPCContext& ctx, uint8_t* base) {
  ++native_checkpoints;
  return WriteClearResolveDraw(ctx, base);
}
bool TryCopyResolveDraw(PPCContext& ctx, uint8_t* base) {
  ++native_checkpoints;
  return WriteCopyResolveDraw(ctx, base);
}
}  // namespace simpsons::native_gpu_calls
#include "native_gpu_resolve_draws_reference.inc"

namespace port = simpsons::native_gpu_calls;
namespace state = rex::graphics::native_gpu_state;
using simpsons::LoadGuestU32;
using simpsons::StoreGuestU32;

namespace {
constexpr uint32_t kSize = 4 * 1024 * 1024, kDevice = 0x1000, kStack = 0x3F0000;
constexpr uint32_t kVertices = 0x180000, kMaxBytes = 1048624;
std::unique_ptr<GuestMemoryFixture> original, native;
std::array<uint8_t, kSize> pristine;
uint64_t clears = 0, copies = 0, local_cases = 0, split_records = 0, rejections = 0;
uint32_t current_buffer = 0, current_count = 0;
bool current_copy = false, current_locals = false;

void Require(bool ok, const char* why) {
  if (!ok)
    throw std::runtime_error(why);
}

PPCContext Prepare(bool copy, uint32_t cursor, uint32_t count, uint32_t flags) {
  std::memcpy(original->data(), pristine.data(), kSize);
  for (uint32_t i = 0; i < kMaxBytes + 128; i += 4)
    StoreGuestU32(original->data(), cursor - 64 + i, count * 0x91E10DA5u + i);
  StoreGuestU32(original->data(), kDevice + 48, cursor);
  StoreGuestU32(original->data(), kDevice + 52, cursor + 0x180000);
  StoreGuestU32(original->data(), kDevice + 56, cursor + 0x17F000);
  StoreGuestU32(original->data(), kDevice + 12708, flags);
  std::memcpy(native->data(), original->data(), kSize);
  for (uint32_t i = 0; i < kMaxBytes + 128; i += 4)
    StoreGuestU32(native->data(), cursor - 64 + i, LoadGuestU32(original->data(), cursor - 64 + i));
  PPCContext ctx{};
  ctx.r1.u64 = 0xABCD000000000000ull | kStack;
  ctx.r3.u64 = 0xFFFF000000000000ull | kVertices;
  ctx.r9.u64 = 0x80020003u;
  ctx.r10.u64 = 0x54800;
  ctx.r11.u64 = 0x1111222233334444;
  ctx.r26.u64 = 0x25000;
  ctx.r28.u64 = 0xFFFF0000FFFFFFF0ull | flags;
  ctx.r29.u64 = copy ? count : 0;
  ctx.r30.u64 = 0xCCCC000000000000ull | cursor;
  ctx.r31.u64 = 0xFFFF000000000000ull | kDevice;
  ctx.r7.u64 = 0x1000001A;
  ctx.lr = 0x8246CD58;
  ctx.xer.so = count & 1;
  return ctx;
}

void EqualMemory(uint32_t cursor, uint32_t span) {
  Require(!std::memcmp(original->data(), native->data(), kSize),
          "Resolve draw changed its records, device, vertices, stack or padding");
  if (cursor >= kSize)
    for (uint32_t i = 0; i < span + 128; i += 4)
      Require(LoadGuestU32(original->data(), cursor - 64 + i) ==
                  LoadGuestU32(native->data(), cursor - 64 + i),
              "Resolve draw changed an aliased output, payload or padding");
}

void CompareContext(const PPCContext& expected, PPCContext actual, bool locals) {
  // External helpers may clobber volatile CR0/CR6. Their local counterparts
  // remain private in the original rewritten function. The real loop below
  // still has to terminate correctly and preserve every other context field.
  if (locals) {
    actual.cr0 = expected.cr0;
    actual.cr6 = expected.cr6;
  }
  Require(!std::memcmp(&expected, &actual, sizeof(actual)),
          "Resolve draw changed its continuation context");
}

void Check(bool copy, bool locals, uint32_t cursor, uint32_t count, uint32_t flags) {
  current_copy = copy;
  current_locals = locals;
  current_buffer = cursor;
  current_count = count;
  auto expected = Prepare(copy, cursor, count, flags), actual = expected;
  const uint64_t checkpoints = native_checkpoints, growth = growth_boundaries;
  if (copy) {
    (locals ? reference_copy_locals : reference_copy)(expected, original->data());
    (locals ? patched_copy_locals : patched_copy)(actual, native->data());
  } else {
    (locals ? reference_clear_locals : reference_clear)(expected, original->data());
    (locals ? patched_clear_locals : patched_clear)(actual, native->data());
  }
  const uint32_t iterations = copy ? count <= 65535 ? 1 : 1 + (uint64_t(count) - 2) / 65534 : 1;
  Require(growth == growth_boundaries && native_checkpoints - checkpoints == iterations,
          "Resolve draw skipped an iteration or unexpectedly entered ring growth");
  CompareContext(expected, actual, locals);
  const uint32_t span = copy ? iterations * 16 : 52;
  for (uint32_t i = 0; i < (copy ? iterations : 1); ++i) {
    const uint32_t first = cursor + 4 + (copy ? i * 16 : 0);
    uint32_t header;
    Require(state::EncodeHeader(copy ? 0x2102 : 0x4800, copy ? 1 : 6, header) &&
                LoadGuestU32(native->data(), first) == header,
            "Resolve draw lost its unconditional native state");
    StoreGuestU32(native->data(), first, copy ? 0x2102 : 0x54800);
    if (!copy) {
      Require(state::EncodeHeader(0x5000, 3, header) &&
                  LoadGuestU32(native->data(), first + 28) == header,
              "Clear draw lost its native flush range");
      StoreGuestU32(native->data(), first + 28, 0x25000);
    }
    const uint32_t draw = first + (copy ? 8 : 44);
    Require(LoadGuestU32(native->data(), draw) ==
                (0xC0000000u | (rex::graphics::native_gpu_calls::kBareDrawOpcode << 8) |
                 (copy ? 1 : flags)),
            "Resolve draw emitted an original header or changed draw predication");
    StoreGuestU32(native->data(), draw, 0xC0003600u | (copy ? 1 : flags));
  }
  EqualMemory(cursor, span);
  (copy ? copies : clears)++;
  local_cases += locals;
  split_records += copy ? iterations : 0;
}

void Rejections() {
  for (bool copy : {false, true}) {
    for (uint32_t probe = 0; probe < (copy ? 8u : 12u); ++probe) {
      auto ctx = Prepare(copy, 0x20000, 1, 0);
      auto& output = copy ? ctx.r11 : ctx.r30;
      if (copy)
        output.u64 = 0x20000;
      switch (probe) {
        case 0:
          output.u32 += 1;
          break;
        case 1:
          output.u32 = 0x7EFFFFFC;
          break;
        case 2:
          output.u32 = kDevice;
          break;
        case 3:
          output.u32 = kStack;
          break;
        case 4:
          ctx.r1.u32 += 4;
          break;
        case 5:
          ctx.r31.u32 += 1;
          break;
        case 6:
          output.u32 = 0xFFFFFFFC;
          break;
        case 7:
          ctx.r31.u32 = kStack;
          break;
        case 8:
          output.u32 = kVertices;
          break;
        case 9:
          ctx.r4.u64 = 1;
          break;
        case 10:
          ctx.r10.u64 = 0x54801;
          break;
        case 11:
          StoreGuestU32(original->data(), kDevice + 12708, 4);
          StoreGuestU32(native->data(), kDevice + 12708, 4);
          break;
      }
      const auto saved = ctx;
      Require(!(copy ? port::WriteCopyResolveDraw(ctx, native->data())
                     : port::WriteClearResolveDraw(ctx, native->data())) &&
                  !std::memcmp(&saved, &ctx, sizeof(ctx)),
              "Rejected resolve draw changed its context");
      EqualMemory(0x20000, 52);
      ++rejections;
    }
  }
}

void GrowthBoundaries() {
  for (bool locals : {false, true}) {
    auto expected = Prepare(true, 0x20000, 65536, 0), actual = expected;
    StoreGuestU32(original->data(), kDevice + 56, 0x1FFFC);
    StoreGuestU32(native->data(), kDevice + 56, 0x1FFFC);
    const auto before = growth_boundaries, checkpoint = native_checkpoints;
    try {
      (locals ? reference_copy_locals : reference_copy)(expected, original->data());
      Require(false, "Original copy loop missed its growth boundary");
    } catch (const GrowthBoundary&) {}
    try {
      (locals ? patched_copy_locals : patched_copy)(actual, native->data());
      Require(false, "Patched copy loop missed its growth boundary");
    } catch (const GrowthBoundary&) {}
    Require(growth_boundaries == before + 2 && checkpoint == native_checkpoints,
            "Native checkpoint ran before original ring growth");
    CompareContext(expected, actual, locals);
    EqualMemory(0x20000, 52);
  }
}
}  // namespace

int main(int argc, char** argv) {
  try {
    original = std::make_unique<GuestMemoryFixture>(kSize);
    native = std::make_unique<GuestMemoryFixture>(kSize);
    for (auto* memory : {original.get(), native.get()})
      for (uint32_t address : {0xA0020000u, 0xC0020000u, 0xE001F000u})
        memory->Commit(address - 64, kMaxBytes + 8192);
    for (uint32_t i = 0; i < kSize; ++i)
      pristine[i] = uint8_t(i * 37 + i / 7);
    for (bool locals : {false, true}) {
      for (uint32_t address : {0x20000u, 0xA0020000u, 0xC0020000u, 0xE001F000u}) {
        for (uint32_t alignment : {0u, 4u, 8u, 12u})
          for (uint32_t flags : {0u, 1u})
            Check(false, locals, address + alignment, 0, flags);
        for (uint32_t count : {0u, 1u, 2u, 65534u, 65535u, 65536u, 131068u, 131069u, 131070u,
                               131071u, 262141u, 0xFFFFFFFFu})
          Check(true, locals, address + 4, count, 1);
      }
    }
    Rejections();
    GrowthBoundaries();
    char report[512];
    std::snprintf(report, sizeof(report),
                  "{\"passed\":true,\"cases\":%llu,\"clear_cases\":%llu,\"copy_cases\":%llu,"
                  "\"register_locals_cases\":%llu,\"split_draw_records\":%llu,"
                  "\"unchanged_rejections\":%llu,\"ring_growth_boundary_calls\":%llu,"
                  "\"allocator_geometry_resource_setup_executed\":false}\n",
                  (unsigned long long)(clears + copies), (unsigned long long)clears,
                  (unsigned long long)copies, (unsigned long long)local_cases,
                  (unsigned long long)split_records, (unsigned long long)rejections,
                  (unsigned long long)growth_boundaries);
    FILE* out = argc == 2 ? std::fopen(argv[1], "wb") : stdout;
    Require(out, "Cannot open resolve-draw report");
    std::fputs(report, out);
    if (out != stdout)
      Require(!std::fclose(out), "Cannot close resolve-draw report");
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "Resolve draw copy=%d locals=%d buffer=%08X count=%u: %s\n", current_copy,
                 current_locals, current_buffer, current_count, e.what());
    return 1;
  }
}
