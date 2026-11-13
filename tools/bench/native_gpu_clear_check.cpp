#include "simpsons_init.h"
#include "native_gpu_direct_draws.h"
#include "native_gpu_clear.h"
#include "guest_memory_fixture.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <vector>

static uint64_t reference_begin_calls = 0, allocator_body_calls = 0;
#include "native_gpu_clear_reference.inc"

namespace port = simpsons::native_gpu_clear;
namespace state = rex::graphics::native_gpu_state;
namespace calls = rex::graphics::native_gpu_calls;
using simpsons::LoadGuestU32;
using simpsons::StoreGuestU32;

namespace {
constexpr uint32_t kDevice = 0x1000, kSync = 0x8000, kColour = 0x9000, kCursor = 0x10000;
constexpr uint32_t kRects = 0x100000, kStack = 0x3F0000, kLowSize = 4 * 1024 * 1024;
std::unique_ptr<GuestMemoryFixture> reference_memory, native_memory;
constexpr std::array<PPCRegister PPCContext::*, 18> kNonvolatile = {
    &PPCContext::r14, &PPCContext::r15, &PPCContext::r16, &PPCContext::r17, &PPCContext::r18,
    &PPCContext::r19, &PPCContext::r20, &PPCContext::r21, &PPCContext::r22, &PPCContext::r23,
    &PPCContext::r24, &PPCContext::r25, &PPCContext::r26, &PPCContext::r27, &PPCContext::r28,
    &PPCContext::r29, &PPCContext::r30, &PPCContext::r31};
constexpr std::array<uint32_t, 8> kFloats = {0,          0x3F000000, 0x80000000, 1,
                                             0x7F7FFFFF, 0x7FC12345, 0x7F800001, 0x7F800000};
constexpr std::array<uint64_t, 8> kDepth = {
    0, 0x3FF0000000000000ull, 0x8000000000000000ull, 0x7FF0000000000000ull, 0x3FD5555555555555ull,
    1, 0x7FF8000123456789ull, 0x7FF0000000000001ull};
void Require(bool ok, const char* why) {
  if (!ok)
    throw std::runtime_error(why);
}
void Prepare(uint32_t count, uint32_t pixel_mode, uint32_t pattern, uint32_t allocation) {
  auto* base = reference_memory->data();
  std::memset(base, 0xCD, kLowSize);
  std::memset(base + kDevice, 0, 14916);
  StoreGuestU32(base, kDevice + 48, kCursor);
  StoreGuestU32(base, kDevice + 52, 0x20000);
  StoreGuestU32(base, kDevice + 56, kCursor + 512);
  StoreGuestU32(base, kDevice + 10688, pixel_mode);
  StoreGuestU32(base, kDevice + 10896, kSync);
  StoreGuestU32(base, kDevice + 14884, 0x3C0000);
  StoreGuestU32(base, kDevice + 14892, allocation == 2 ? 0 : 0x180001);
  StoreGuestU32(base, kDevice + 14896, pattern * 0x55555555u);
  StoreGuestU32(base, kSync + 4, pattern * 0x55555555u + 4);
  StoreGuestU32(base, kDevice + 14908, allocation == 2 ? 0xFFFFFFF0u : 0);
  StoreGuestU32(base, kDevice + 14912, allocation == 1 ? 1 : UINT32_MAX);
  StoreGuestU32(base, kDevice + 14900, allocation == 1 ? 0x30004 : 0x30000);
  StoreGuestU32(base, kDevice + 14880, 0x30000);
  if (allocation == 3)
    *simpsons::GuestToHost(base, kDevice + 10941) = 0x20;
  for (uint32_t offset : {16u, 24u, 32u})
    simpsons::native_gpu_vertices::Store64(base, kDevice + offset, 0xA5A5123400000000ull | pattern);
  StoreGuestU32(base, kStack + 84, 0x7FC01234u ^ (pattern * 0x10203u));
  for (uint32_t i = 0; i < 4; ++i)
    StoreGuestU32(base, kColour + i * 4, kFloats[(pattern + i) % 8]);
  for (uint32_t i = 0; i < count * 4; ++i) {
    constexpr std::array<uint32_t, 12> edges = {
        0,   1,    UINT32_MAX, 17, 0x7FFFFFFF, 0x80000000, 16777217, uint32_t(-16777217),
        640, 1280, 800,        720};
    StoreGuestU32(base, kRects + i * 4, edges[(i + pattern) % edges.size()]);
  }
  StoreGuestU32(base, 0x82000FB8, kFloats[pattern]);
  StoreGuestU32(base, 0x821DD0D8, kFloats[(pattern + 1) % 8]);
  std::memcpy(native_memory->data(), base, kLowSize);
  for (uint32_t address : {0x82000FB8u, 0x821DD0D8u})
    StoreGuestU32(native_memory->data(), address, LoadGuestU32(base, address));
}
PPCContext Context(uint32_t count, uint32_t samples, uint32_t pattern) {
  PPCContext ctx{};
  ctx.r1.u64 = kStack;
  ctx.r3.u64 = 0xA5A5000000000000ull | kDevice;
  ctx.r4.u64 = 0xABCD000000000000ull | count;
  ctx.r5.u64 = 0xABCD000000000000ull | kRects;
  ctx.r6.u64 = 0xFFFF000000000000ull | kColour;
  ctx.r8.u64 = 0xFFFF000000000027ull | (samples << 16);
  ctx.r9.u64 = 0xABCD000012341234ull;
  ctx.r10.u64 = 0xA5A50000AA55AA55ull;
  ctx.f1.u64 = kDepth[pattern];
  ctx.f30.u64 = 0x7FF0000000000001ull;
  ctx.f31.u64 = 0x8000000000000000ull;
  ctx.fpscr.InitHost();
  ctx.fpscr.storeFromGuest(pattern & 3);
  if (pattern & 4)
    ctx.fpscr.enableFlushModeUnconditional();
  ctx.lr = 0xABCD000082123450ull;
  for (uint32_t i = 0; i < kNonvolatile.size(); ++i)
    (ctx.*kNonvolatile[i]).u64 = 0xDEADBEEF00000000ull + i;
  return ctx;
}
void Abi(const PPCContext& a, const PPCContext& b) {
  Require(a.r1.u64 == b.r1.u64 && a.r3.u64 == b.r3.u64 && a.lr == b.lr && a.f30.u64 == b.f30.u64 &&
              a.f31.u64 == b.f31.u64 && a.fpscr.csr == b.fpscr.csr,
          "Native clear changed its stack/link/return/FPU ABI");
  for (auto field : kNonvolatile)
    Require((a.*field).u64 == (b.*field).u64, "Native clear changed a nonvolatile GPR");
}
void EqualMemory(std::vector<uint8_t>& normalized) {
  const auto* reference = reference_memory->data();
  for (const auto& [start, end] : {std::pair{0u, kStack - 592}, std::pair{kStack + 88, kLowSize}}) {
    if (std::memcmp(reference + start, normalized.data() + start, end - start)) {
      for (uint32_t i = start; i < end; ++i)
        if (reference[i] != normalized[i]) {
          std::fprintf(stderr, "Memory mismatch at %08X: original=%02X native=%02X\n", i,
                       reference[i], normalized[i]);
          break;
        }
      throw std::runtime_error("Native clear changed guest data, state or allocator results");
    }
  }
}
}  // namespace

int main(int argc, char** argv) {
  if (argc > 2 || (argc == 2 && !std::freopen(argv[1], "w", stdout)))
    return 2;
  try {
    reference_memory = std::make_unique<GuestMemoryFixture>(kLowSize);
    native_memory = std::make_unique<GuestMemoryFixture>(kLowSize);
    for (auto* memory : {reference_memory.get(), native_memory.get()})
      for (uint32_t address : {0x82000FB8u, 0x821DD0D8u})
        memory->Commit(address, 4);
    uint64_t cases = 0, failures = 0, vertices = 0, records = 0, rejected = 0;
    for (uint32_t count : {1u, 2u, 9u, 31u, 128u, 21845u})
      for (uint32_t pixel_mode : {0u, 1u})
        for (uint32_t samples = 0; samples < 4; ++samples)
          for (uint32_t pattern = 0; pattern < 8; ++pattern)
            for (uint32_t allocation = 0; allocation < 4; ++allocation) {
              if (count == 21845 && (pixel_mode || samples || pattern || allocation))
                continue;
              Prepare(count, pixel_mode, pattern, allocation);
              auto original = Context(count, samples, pattern), native = original;
              reference_sub_824529C0(original, reference_memory->data());
              const auto calls_before = reference_begin_calls, allocs = allocator_body_calls;
              Require(port::DrawClear(native, native_memory->data(), reference_sub_82456D60),
                      "Prepared clear unexpectedly fell back");
              Require(reference_begin_calls == calls_before && allocator_body_calls == allocs + 1,
                      "Native clear called the old builder or bypassed its allocator");
              Abi(original, native);
              auto normalized =
                  std::vector<uint8_t>(native_memory->data(), native_memory->data() + kLowSize);
              const uint32_t end = LoadGuestU32(reference_memory->data(), kDevice + 48);
              if (end == kCursor) {
                ++failures;
              } else {
                Require(
                    end == kCursor + 88 && end == LoadGuestU32(native_memory->data(), kDevice + 48),
                    "Clear command span/publication changed");
                uint32_t cursor = kCursor + 4;
                while (cursor <= end) {
                  const uint32_t header = LoadGuestU32(reference_memory->data(), cursor);
                  const uint32_t native_header = LoadGuestU32(native_memory->data(), cursor);
                  const uint32_t count = ((header >> 16) & 0x3FFF) + 1;
                  if ((header >> 30) == 0) {
                    state::StateRange decoded;
                    Require(state::DecodeHeader(native_header, count, decoded) &&
                                decoded.first_register == (header & 0x7FFF) &&
                                decoded.register_count == count,
                            "Clear state range differs from the generated record");
                  } else {
                    Require(header == 0xC0003601u &&
                                native_header == (0xC0000001u | (calls::kBareDrawOpcode << 8)),
                            "Clear lost its predicated auto draw");
                  }
                  StoreGuestU32(normalized.data(), cursor, header);
                  cursor += (count + 1) * 4;
                  ++records;
                }
                Require(cursor == end + 4, "Clear transport changed its ordinary packet lengths");
                vertices += count * 3;
              }
              EqualMemory(normalized);
              ++cases;
            }
    for (uint32_t problem = 0; problem < 16; ++problem) {
      Prepare(2, 0, 0, 0);
      auto ctx = Context(2, 0, 0);
      auto* base = native_memory->data();
      if (problem == 0)
        ctx.r4.u64 = 0;
      if (problem == 1)
        ctx.r4.u64 = 21846;
      if (problem == 2)
        StoreGuestU32(base, kDevice + 56, kCursor - 4);
      if (problem == 3)
        StoreGuestU32(base, kDevice + 52, kCursor + 84);
      if (problem == 4)
        StoreGuestU32(base, kDevice + 13492, 0x8000);
      if (problem == 5)
        StoreGuestU32(base, kDevice + 13216, 1);
      if (problem == 6)
        StoreGuestU32(base, kSync + 4, 1);
      if (problem == 7)
        StoreGuestU32(base, kDevice + 14884, 0x180004);
      if (problem == 8)
        ctx.r5.u64 = 0x180004;
      if (problem == 9)
        ctx.r6.u64 = kCursor + 16;
      if (problem == 10)
        ctx.r6.u64 = kStack - 128;
      if (problem == 11)
        ctx.r5.u64 = kCursor + 8;
      if (problem == 12)
        StoreGuestU32(base, kDevice + 14892, kDevice);
      if (problem == 13)
        StoreGuestU32(base, kDevice + 48, kStack - 256);
      if (problem == 14)
        StoreGuestU32(base, kDevice + 10896, kCursor + 4);
      if (problem == 15)
        ctx.r5.u64 = 0xFFFFFFF0u;
      const auto before = std::vector<uint8_t>(base, base + kLowSize);
      const auto saved = ctx;
      const auto allocs = allocator_body_calls;
      Require(!port::DrawClear(ctx, base, reference_sub_82456D60) &&
                  allocs == allocator_body_calls && !std::memcmp(&ctx, &saved, sizeof(ctx)) &&
                  std::equal(before.begin(), before.end(), base),
              "Unsupported clear modified memory, context or allocation state");
      ++rejected;
    }
    std::printf(
        "{\"passed\":true,\"cases\":%llu,\"real_allocation_failures\":%llu,"
        "\"vertices_compared\":%llu,\"native_records\":%llu,\"unchanged_fallbacks\":%llu,"
        "\"native_original_builder_calls\":0,\"scope\":\"CPU clear ABI/data/command contract\"}\n",
        (unsigned long long)cases, (unsigned long long)failures, (unsigned long long)vertices,
        (unsigned long long)records, (unsigned long long)rejected);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
  }
}
