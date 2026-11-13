// Execute the full original allocation/ready-fence chain independently.
#include "simpsons_init.h"
#include "native_gpu_default_allocator.h"
#include "guest_memory_fixture.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <utility>

struct WaitRequired : std::runtime_error {
  WaitRequired() : std::runtime_error("Original allocator requires a live GPU wait") {}
};
static uint64_t default_calls = 0, dispatch_calls = 0, recorded_calls = 0, ready_calls = 0;
static uint64_t blocked_calls = 0;
#include "native_gpu_default_allocator_reference.inc"

namespace port = simpsons::native_gpu_vertices;
using simpsons::LoadGuestU32;
using simpsons::StoreGuestU32;

namespace {
constexpr uint32_t kDevice = 0x1000, kSync = 0x8000, kParameter = 0x9000, kObject = 0xA000;
constexpr uint32_t kHeap = 0x10000, kBytes = 0x40000, kStack = 0x7F000, kMemory = 0x80000;
GuestMemoryFixture original_memory(kMemory), native_memory(kMemory);
constexpr std::array<PPCRegister PPCContext::*, 18> kNonvolatile = {
    &PPCContext::r14, &PPCContext::r15, &PPCContext::r16, &PPCContext::r17, &PPCContext::r18,
    &PPCContext::r19, &PPCContext::r20, &PPCContext::r21, &PPCContext::r22, &PPCContext::r23,
    &PPCContext::r24, &PPCContext::r25, &PPCContext::r26, &PPCContext::r27, &PPCContext::r28,
    &PPCContext::r29, &PPCContext::r30, &PPCContext::r31};
uint64_t cases = 0, failed = 0, rejections = 0, waiting = 0, conservative = 0;
uint64_t temporary = 0, chunks = 0, dispatches = 0, command_wraps = 0, temporary_wraps = 0;
uint64_t partial_wrap_failures = 0, physical_views = 0, recorded = 0;
int active_mode = 0;
uint32_t active_pattern = 0, active_bytes = 0;

void Require(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}
void Prepare(uint32_t pattern, uint32_t version, uint32_t reservation, uint32_t fence) {
  auto* base = original_memory.data();
  std::memset(base, 0xCD, kMemory);
  std::memset(base + kDevice, 0, 22256);
  StoreGuestU32(base, kParameter, 16);
  StoreGuestU32(base, kDevice + 10896, kSync);
  StoreGuestU32(base, kDevice + 48, kHeap + 4096 - 4);
  StoreGuestU32(base, kDevice + 52, kHeap + 0x10000 - 60);
  StoreGuestU32(base, kDevice + 56, kHeap + 0x10000 - 220);
  StoreGuestU32(base, kDevice + 14880, kHeap);
  StoreGuestU32(base, kDevice + 14884, kHeap + kBytes);
  StoreGuestU32(base, kDevice + 14888, pattern & 2 ? 4096 : 216);
  StoreGuestU32(base, kDevice + 14892,
                pattern & 1   ? kHeap + kBytes - 16
                : pattern & 2 ? 0
                              : kHeap + 0x10004);
  StoreGuestU32(base, kDevice + 14896, version);
  StoreGuestU32(base, kDevice + 14900, kHeap + (pattern & 4 ? 260 : 0));
  StoreGuestU32(base, kDevice + 14904, kHeap + (pattern & 8 ? 1024 : 0x20000));
  StoreGuestU32(base, kDevice + 14908, pattern & 4 ? 0xFFFFFFF0u : 12);
  StoreGuestU32(base, kDevice + 14912, pattern & 8 ? 16 : 0x7FFFFFFFu);
  *simpsons::GuestToHost(base, kDevice + 10941) = 0xD0;
  *simpsons::GuestToHost(base, kDevice + 10942) = 0xA1;
  if (reservation) {
    StoreGuestU32(base, kDevice + 13216, kHeap + (reservation & 1 ? 0x100 : 0x18000));
    const uint32_t delta = reservation == 1   ? UINT32_MAX
                           : reservation == 2 ? 0
                           : reservation == 3 ? 1
                                              : 2;
    StoreGuestU32(base, kDevice + 13220, version + delta);
  }
  StoreGuestU32(base, kSync + 4, (kHeap + (pattern & 4 ? 0x100 : 0x30000)) | fence);
}
PPCContext Context(int mode, uint32_t bytes, uint64_t alignment) {
  PPCContext ctx{};
  ctx.r1.u64 = 0xA5A5000000000000ull | kStack;
  ctx.r3.u64 = 0xB6B6000000000000ull | kDevice;
  ctx.r4.u64 =
      mode >= 2 ? 0xC7C7000000000000ull | bytes : 0xD8D8000000000000ull | (mode == 0 ? 1 : 2);
  ctx.r5.u64 = mode >= 2 ? alignment : 0xE9E9000000000000ull | kParameter;
  ctx.r6.u64 = alignment;
  ctx.lr = 0xFAFA000082345678ull;
  for (uint32_t i = 0; i < kNonvolatile.size(); ++i)
    (ctx.*kNonvolatile[i]).u64 = 0xABCD000000000000ull | i;
  return ctx;
}
bool Native(PPCContext& ctx, int mode) {
  return mode >= 2 ? port::AllocateTransient(ctx, native_memory.data())
                   : port::AllocateDefaultStorage(ctx, native_memory.data());
}
void Original(PPCContext& ctx, int mode) {
  if (mode >= 2)
    reference_sub_82456D60(ctx, original_memory.data());
  else
    reference_sub_82456B58(ctx, original_memory.data());
}
void Compare(PPCContext input, int mode, bool require_accept = false) {
  active_mode = mode;
  const auto* base = original_memory.data();
  const uint32_t old_version = LoadGuestU32(base, kDevice + 14896);
  const uint32_t old_tail = LoadGuestU32(base, kDevice + 14900);
  const uint32_t old_cursor = LoadGuestU32(base, kDevice + 14892);
  std::memcpy(native_memory.data(), original_memory.data(), kMemory);
  auto native = input, original = input;
  const auto calls = std::array{default_calls, dispatch_calls, recorded_calls, ready_calls};
  const bool accepted = Native(native, mode);
  Require(calls == std::array{default_calls, dispatch_calls, recorded_calls, ready_calls},
          "Native allocation called an original implementation");
  Require(accepted || !require_accept, "Required native allocation fell back");
  if (!accepted) {
    Require(!std::memcmp(&input, &native, sizeof(input)) &&
                !std::memcmp(original_memory.data(), native_memory.data(), kMemory),
            "Rejected allocation changed guest memory or context");
    ++rejections;
    try {
      Original(original, mode);
      ++conservative;
    } catch (const WaitRequired&) {
      ++waiting;
    }
    return;
  }
  Original(original, mode);  // Any live wait here fails the accepted-case check.
  Require(original.r3.u64 == native.r3.u64 && original.lr == native.lr &&
              original.r1.u64 == native.r1.u64,
          "Default allocation changed return, link or stack ABI");
  for (auto field : kNonvolatile)
    Require((original.*field).u64 == (native.*field).u64,
            "Default allocation changed a nonvolatile GPR");
  Require(!std::memcmp(original_memory.data(), native_memory.data(), kMemory),
          "Default allocation changed storage accounting, frames or neighboring bytes");
  ++cases;
  failed += !original.r3.u32;
  temporary += mode == 0;
  chunks += mode == 1;
  dispatches += mode == 2;
  recorded += mode == 3;
  const bool command_wrap = mode == 1 && LoadGuestU32(base, kDevice + 14896) != old_version;
  command_wraps += command_wrap;
  partial_wrap_failures += command_wrap && !original.r3.u32;
  temporary_wraps +=
      mode != 1 && mode != 3 && LoadGuestU32(base, kDevice + 14900) != old_tail && original.r3.u32;
  if (!original.r3.u32 && mode == 0)
    Require(LoadGuestU32(base, kDevice + 14892) == old_cursor,
            "Failed temporary allocation published a cursor");
}
void Reject(PPCContext ctx, int mode) {
  active_mode = mode;
  std::memcpy(native_memory.data(), original_memory.data(), kMemory);
  const auto before = ctx;
  Require(!Native(ctx, mode), "Unproved allocator geometry was accepted");
  Require(!std::memcmp(&before, &ctx, sizeof(ctx)) &&
              !std::memcmp(original_memory.data(), native_memory.data(), kMemory),
          "Unproved allocator changed state before fallback");
  ++rejections;
}
void CommitView(GuestMemoryFixture& memory, uint32_t address, uint32_t bytes) {
  memory.Commit(address, uint64_t(bytes) + (address >= 0xE0000000u ? 4096 : 0));
}
}  // namespace

int main(int argc, char** argv) {
  if (argc > 2 || (argc == 2 && !std::freopen(argv[1], "w", stdout)))
    return 2;
  try {
    for (int mode = 0; mode < 3; ++mode)
      for (uint32_t pattern = 0; pattern < 16; ++pattern)
        for (uint32_t reservation = 0; reservation < 5; ++reservation)
          for (uint32_t version : {0u, 1u, 3u, 0x7FFFFFFFu, 0xFFFFFFFEu, 0xFFFFFFFFu})
            for (uint32_t bytes : {0u, 1u, 3u, 16u, 4096u, 65536u})
              for (uint32_t fence = 0; fence < 4; ++fence) {
                active_pattern = pattern;
                active_bytes = bytes;
                Prepare(pattern, version, reservation, fence);
                StoreGuestU32(original_memory.data(), kParameter, bytes);
                const uint32_t alignment = std::array{
                    1u, 4u, 16u, 32u, 256u, 4096u}[(pattern + reservation + version) % 6];
                Compare(Context(mode, bytes, alignment), mode);
              }
    // Fixed recordings still route to the previously qualified allocator.
    for (uint32_t pattern = 0; pattern < 16; ++pattern)
      for (uint32_t bytes : {0u, 4u, 4096u, 65536u}) {
        Prepare(pattern, 0, 0, 0);
        auto* base = original_memory.data();
        StoreGuestU32(base, kDevice + 13492, kObject);
        StoreGuestU32(base, kObject + 108, 2);
        StoreGuestU32(base, kObject + 152, kHeap);
        StoreGuestU32(base, kObject + 156, kBytes);
        StoreGuestU32(base, kDevice + 14892, 0);
        StoreGuestU32(base, kDevice + 52, kHeap + kBytes - 60);
        StoreGuestU32(base, kDevice + 56, kHeap + kBytes - 220);
        Compare(Context(3, bytes, 16), 3, true);
      }
    // Sticky failure must not inspect an invalid heap, count or alignment.
    for (uint32_t alignment : {0u, 3u, 0xFFFFFFFFu}) {
      Prepare(0, 0, 0, 0);
      *simpsons::GuestToHost(original_memory.data(), kDevice + 10941) |= 0x20;
      StoreGuestU32(original_memory.data(), kDevice + 14880, 0xDEADBEEFu);
      Compare(Context(2, 0xFFFFFFFFu, alignment), 2, true);
    }
    // A reservation failure never reads the completed-fence pointer. Keep
    // this early failure native even when the unused pointer is invalid.
    for (int mode = 0; mode < 3; ++mode) {
      Prepare(0, 0, 1, 0);
      StoreGuestU32(original_memory.data(), kDevice + 10896, 0xDEADBEEFu);
      Compare(Context(mode, 16, 16), mode, true);
    }
    // Command chunks ignore the temporary-data alignment argument entirely.
    for (uint64_t alignment : {0ull, 3ull, 0x100000010ull}) {
      Prepare(0, 0, 0, 0);
      Compare(Context(1, 16, alignment), 1, true);
    }
    for (uint32_t dwords : {0x40000000u, 0x40000001u, 0x80000004u}) {
      Prepare(4, 0, 0, 0);
      Compare(Context(2, dwords, 16), 2, true);
    }
    for (int mode = 0; mode < 3; ++mode) {
      for (uint32_t field : {14880u, 14884u, 13492u}) {
        Prepare(0, 0, 0, 0);
        StoreGuestU32(original_memory.data(), kDevice + field, 1);
        Reject(Context(mode, 16, 16), mode);
      }
      for (uint32_t heap : {kDevice, mode >= 2 ? kStack - 32 : kParameter, kStack - 128,
                            0xBFFFF000u, 0xFFFFF000u}) {
        Prepare(0, 0, 0, 0);
        StoreGuestU32(original_memory.data(), kDevice + 14880, heap);
        StoreGuestU32(original_memory.data(), kDevice + 14884, heap + kBytes);
        Reject(Context(mode, 16, 16), mode);
      }
      for (uint32_t sp : {kStack + 4, 128u, kDevice + 512}) {
        Prepare(0, 0, 0, 0);
        auto ctx = Context(mode, 16, 16);
        ctx.r1.u64 = sp;
        Reject(ctx, mode);
      }
      for (uint32_t sync : {0u, kSync + 1, kDevice, mode >= 2 ? kStack - 32 : kParameter,
                            kHeap + 1024, kStack - 256}) {
        Prepare(0, 0, 0, 0);
        StoreGuestU32(original_memory.data(), kDevice + 10896, sync);
        Reject(Context(mode, 16, 16), mode);
      }
    }
    for (int mode : {0, 2})
      for (uint64_t alignment : {0ull, 3ull, 0x100000010ull}) {
        Prepare(0, 0, 0, 0);
        Reject(Context(mode, 16, alignment), mode);
      }
    // Each view uses the actual guest-to-host offset on its target platform.
    for (uint32_t heap : {0xA0100000u, 0xC0100000u, 0xE0100000u, 0x91000000u})
      for (int mode = 0; mode < 3; ++mode) {
        Prepare(0, 0, 0, 0);
        for (auto* memory : {&original_memory, &native_memory})
          CommitView(*memory, heap, kBytes);
        auto* base = original_memory.data();
        StoreGuestU32(base, kDevice + 14880, heap);
        StoreGuestU32(base, kDevice + 14884, heap + kBytes);
        StoreGuestU32(base, kDevice + 14892, heap + 0x10004);
        StoreGuestU32(base, kParameter, 16);
        Compare(Context(mode, 4, 16), mode, true);
        ++physical_views;
      }
    Require(cases > 10000 && temporary > 1000 && chunks > 1000 && dispatches > 1000 &&
                command_wraps && temporary_wraps && partial_wrap_failures && waiting,
            "Allocator matrix did not cover its promised branches");
    std::printf(
        "{\"passed\":true,\"cases\":%llu,\"failed_allocations\":%llu,"
        "\"unchanged_fallbacks\":%llu,\"real_wait_fallbacks\":%llu,"
        "\"conservative_fallbacks\":%llu,\"temporary_cases\":%llu,"
        "\"command_chunks\":%llu,\"dispatch_cases\":%llu,"
        "\"recorded_dispatch_cases\":%llu,\"command_wraps\":%llu,"
        "\"temporary_wraps\":%llu,\"partial_wrap_failures\":%llu,"
        "\"physical_views\":%llu,\"real_default_calls\":%llu,"
        "\"real_dispatch_calls\":%llu,\"real_recorded_calls\":%llu,"
        "\"real_ready_calls\":%llu,\"real_wait_branches\":%llu}\n",
        (unsigned long long)cases, (unsigned long long)failed, (unsigned long long)rejections,
        (unsigned long long)waiting, (unsigned long long)conservative,
        (unsigned long long)temporary, (unsigned long long)chunks, (unsigned long long)dispatches,
        (unsigned long long)recorded, (unsigned long long)command_wraps,
        (unsigned long long)temporary_wraps, (unsigned long long)partial_wrap_failures,
        (unsigned long long)physical_views, (unsigned long long)default_calls,
        (unsigned long long)dispatch_calls, (unsigned long long)recorded_calls,
        (unsigned long long)ready_calls, (unsigned long long)blocked_calls);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s (mode %d, pattern %u, bytes %u; %llu comparisons, %llu rejections)\n",
                 error.what(), active_mode, active_pattern, active_bytes, (unsigned long long)cases,
                 (unsigned long long)rejections);
    return 1;
  }
}
