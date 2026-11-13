// Compare complete original allocator bodies, not a second allocator model.
#include "simpsons_init.h"
#include "native_gpu_recorded_allocator.h"
#include "guest_memory_fixture.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <utility>

static uint64_t storage_calls = 0, dispatch_calls = 0;
#include "native_gpu_recorded_allocator_reference.inc"

namespace port = simpsons::native_gpu_vertices;
using simpsons::LoadGuestU32;
using simpsons::StoreGuestU32;

namespace {
constexpr uint32_t kDevice = 0x1000, kObject = 0x8000, kStorage = 0x10000;
constexpr uint32_t kBytes = 0x40000, kParameter = 0x9000, kStack = 0x7F000;
constexpr uint32_t kMemory = 0x80000;
GuestMemoryFixture original_memory(kMemory), native_memory(kMemory);
constexpr std::array<PPCRegister PPCContext::*, 18> kNonvolatile = {
    &PPCContext::r14, &PPCContext::r15, &PPCContext::r16, &PPCContext::r17, &PPCContext::r18,
    &PPCContext::r19, &PPCContext::r20, &PPCContext::r21, &PPCContext::r22, &PPCContext::r23,
    &PPCContext::r24, &PPCContext::r25, &PPCContext::r26, &PPCContext::r27, &PPCContext::r28,
    &PPCContext::r29, &PPCContext::r30, &PPCContext::r31};
uint64_t cases = 0, rejections = 0, failures = 0, initializations = 0;
uint64_t throttles = 0, wraps = 0, physical_views = 0, sequence_calls = 0;

void Require(bool pass, const char* message) {
  if (!pass)
    throw std::runtime_error(message);
}
void Prepare(uint32_t pattern = 0) {
  auto* base = original_memory.data();
  std::memset(base, 0xCD, kMemory);
  std::memset(base + kDevice, 0, 22256);
  std::memset(base + kObject, 0, 184);
  StoreGuestU32(base, kDevice + 13492, kObject);
  StoreGuestU32(base, kObject + 108, 0xA5000002);
  StoreGuestU32(base, kObject + 152, kStorage);
  StoreGuestU32(base, kObject + 156, kBytes);
  StoreGuestU32(base, kDevice + 48, kStorage + (pattern & 1 ? 0 : 1024));
  StoreGuestU32(base, kDevice + 52, kStorage + kBytes - 60);
  StoreGuestU32(base, kDevice + 56, kStorage + kBytes - 220);
  StoreGuestU32(base, kDevice + 14892, pattern & 2 ? kStorage + kBytes - 12 : 0);
  StoreGuestU32(base, kDevice + 14880, 0x60000);
  StoreGuestU32(base, kDevice + 14884, 0x70000);
  StoreGuestU32(base, kDevice + 14900, pattern & 4 ? 0x60004 : 0x60000);
  StoreGuestU32(base, kDevice + 14908, pattern & 8 ? 0xFFFFFFF0u : 0);
  StoreGuestU32(base, kDevice + 14912, pattern & 16 ? 16 : 0x7FFFFFFFu);
  StoreGuestU32(base, kDevice + 13216, pattern & 32 ? 0x1234 : 0);
  *simpsons::GuestToHost(base, kDevice + 10941) = 0xD0;
  std::memcpy(native_memory.data(), base, kMemory);
}
PPCContext Context(bool dispatch, uint32_t count, uint64_t alignment) {
  PPCContext ctx{};
  ctx.r1.u64 = 0xA5A5000000000000ull | kStack;
  ctx.r3.u64 = 0xB6B6000000000000ull | kDevice;
  ctx.r4.u64 = dispatch ? (0xC7C7000000000000ull | count) : 0xD8D8000000000001ull;
  ctx.r5.u64 = dispatch ? alignment : 0xE9E9000000000000ull | kParameter;
  ctx.r6.u64 = alignment;
  ctx.lr = 0xFAFA000082345678ull;
  for (uint32_t i = 0; i < kNonvolatile.size(); ++i)
    (ctx.*kNonvolatile[i]).u64 = 0xABCD000000000000ull | i;
  return ctx;
}
void Compare(PPCContext ctx, bool dispatch) {
  auto native = ctx;
  const uint64_t old_storage = storage_calls, old_dispatch = dispatch_calls;
  const bool was_failed =
      (*simpsons::GuestToHost(original_memory.data(), kDevice + 10941) & 0x20) != 0;
  const uint32_t old_budget = LoadGuestU32(original_memory.data(), kDevice + 14908);
  const uint32_t bytes = ctx.r4.u32 << 2;
  const uint32_t budget = old_budget + bytes;
  const bool throttle = budget > LoadGuestU32(original_memory.data(), kDevice + 14912) &&
                        !LoadGuestU32(original_memory.data(), kDevice + 13216);
  if (dispatch)
    reference_sub_82456D60(ctx, original_memory.data());
  else
    reference_sub_82458E60(ctx, original_memory.data());
  const auto real_storage = storage_calls, real_dispatch = dispatch_calls;
  Require(dispatch ? port::AllocateRecordedTransient(native, native_memory.data())
                   : port::AllocateRecordedStorage(native, native_memory.data()),
          "Proved recorded allocator unexpectedly fell back");
  Require(real_storage == storage_calls && real_dispatch == dispatch_calls,
          "Native allocation executed the original body");
  Require(dispatch_calls == old_dispatch + dispatch &&
              storage_calls == old_storage + (!dispatch || !was_failed),
          "Reference did not execute the real fixed allocator");
  Require(ctx.r3.u64 == native.r3.u64 && ctx.r1.u64 == native.r1.u64 && ctx.lr == native.lr,
          "Recorded allocator changed its return, stack or link ABI");
  for (auto field : kNonvolatile)
    Require((ctx.*field).u64 == (native.*field).u64,
            "Recorded allocator changed a nonvolatile GPR");
  Require(!std::memcmp(original_memory.data(), native_memory.data(), kMemory),
          "Recorded allocator changed memory, accounting or neighboring bytes");
  if (!ctx.r3.u32)
    ++failures;
  if (dispatch && !was_failed) {
    throttles += throttle;
    wraps += uint64_t(old_budget) + bytes > UINT32_MAX;
  }
  ++cases;
}
void Reject(PPCContext ctx, bool dispatch) {
  const auto before = ctx;
  std::memcpy(native_memory.data(), original_memory.data(), kMemory);
  Require(!(dispatch ? port::AllocateRecordedTransient(ctx, native_memory.data())
                     : port::AllocateRecordedStorage(ctx, native_memory.data())),
          "Unproved allocation was accepted");
  Require(!std::memcmp(&before, &ctx, sizeof(ctx)) &&
              !std::memcmp(original_memory.data(), native_memory.data(), kMemory),
          "Rejected allocation changed memory or CPU context");
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
    for (uint32_t pattern = 0; pattern < 4; ++pattern)
      for (uint32_t alignment : {1u, 2u, 4u, 8u, 16u, 32u, 256u, 4096u, 0x80000000u})
        for (uint32_t bytes : {0u, 1u, 3u, 4u, 5u, 15u, 16u, 17u, 1023u, 4096u, kBytes - 2048,
                               kBytes - 164, kBytes}) {
          Prepare(pattern);
          StoreGuestU32(original_memory.data(), kParameter, bytes);
          std::memcpy(native_memory.data(), original_memory.data(), kMemory);
          Compare(Context(false, 0, alignment), false);
        }
    for (uint32_t pattern = 0; pattern < 64; ++pattern)
      for (uint32_t alignment : {4u, 16u, 32u, 256u})
        for (uint32_t dwords : {0u, 1u, 4u, 257u, 1024u, (kBytes - 164) / 4, kBytes / 4,
                                0x40000000u, 0x40000001u, 0x80000004u}) {
          Prepare(pattern);
          Compare(Context(true, dwords, alignment), true);
        }
    // One-time command storage and its already-initialized failure. Unused
    // parameters/recording pointers must not make the early failure dereference.
    for (uint32_t flags : {0u, 2u, 0xFFFFFFFEu}) {
      Prepare();
      auto ctx = Context(false, 0, 0);
      ctx.r4.u64 = flags;
      StoreGuestU32(original_memory.data(), kDevice + 56, 0);
      std::memcpy(native_memory.data(), original_memory.data(), kMemory);
      Compare(ctx, false);
      ++initializations;
      ctx.r5.u64 = 0xDEADC0DE;
      StoreGuestU32(original_memory.data(), kDevice + 56, 1);
      StoreGuestU32(original_memory.data(), kDevice + 13492, 0xDEADBEEFu);
      std::memcpy(native_memory.data(), original_memory.data(), kMemory);
      Compare(ctx, false);
    }
    // Once failed, dispatch still saves its frame but does not read allocator
    // metadata, the requested count or alignment, and does not account bytes.
    for (uint32_t alignment : {0u, 3u, 0xFFFFFFFFu}) {
      Prepare();
      *simpsons::GuestToHost(original_memory.data(), kDevice + 10941) |= 0x20;
      StoreGuestU32(original_memory.data(), kDevice + 13492, 0xDEADBEEFu);
      std::memcpy(native_memory.data(), original_memory.data(), kMemory);
      Compare(Context(true, 0xFFFFFFFFu, alignment), true);
    }
    // Run a real recording lifecycle: initialize, allocate repeatedly, fail,
    // then ensure the sticky failure suppresses later requests and accounting.
    Prepare();
    StoreGuestU32(original_memory.data(), kDevice + 56, 0);
    std::memcpy(native_memory.data(), original_memory.data(), kMemory);
    auto command = Context(false, 0, 32);
    command.r4.u64 = 2;
    Compare(command, false);
    for (auto* base : {original_memory.data(), native_memory.data()}) {
      StoreGuestU32(base, kDevice + 48, kStorage - 4);
      StoreGuestU32(base, kDevice + 52, kStorage + kBytes - 60);
      StoreGuestU32(base, kDevice + 56, kStorage + kBytes - 220);
    }
    for (uint32_t i = 0; i < 68; ++i) {
      Compare(Context(true, 1024, 16), true);
      ++sequence_calls;
    }
    // The four backing views use the platform's actual physical offset rules.
    for (uint32_t storage : {0xA0100000u, 0xC0100000u, 0xE0100000u, 0x91000000u}) {
      Prepare();
      for (auto* memory : {&original_memory, &native_memory}) {
        CommitView(*memory, storage, kBytes);
        auto* base = memory->data();
        std::memset(simpsons::GuestToHost(base, storage), 0xB6, kBytes);
        StoreGuestU32(base, kObject + 152, storage);
        StoreGuestU32(base, kDevice + 48, storage + 1024);
        StoreGuestU32(base, kDevice + 52, storage + kBytes - 60);
        StoreGuestU32(base, kDevice + 56, storage + kBytes - 220);
      }
      Compare(Context(true, 257, 16), true);
      Require(!std::memcmp(simpsons::GuestToHost(original_memory.data(), storage),
                           simpsons::GuestToHost(native_memory.data(), storage), kBytes),
              "Physical storage was unexpectedly overwritten");
      ++physical_views;
    }
    for (bool dispatch : {false, true}) {
      for (uint64_t alignment : {0ull, 3ull, 24ull, 0x100000010ull}) {
        Prepare();
        Reject(Context(dispatch, 4, alignment), dispatch);
      }
      for (uint32_t field : {108u, 152u, 156u}) {
        Prepare();
        StoreGuestU32(original_memory.data(), kObject + field, field == 108 ? 3 : 1);
        Reject(Context(dispatch, 4, 16), dispatch);
      }
      for (uint32_t recording : {0u, kObject + 1, kDevice + 4, kStack - 128}) {
        Prepare();
        if (recording == kStack - 128) {
          std::memcpy(original_memory.data() + recording, original_memory.data() + kObject, 184);
          if (!dispatch)
            continue;  // The leaf has no private frame.
        }
        StoreGuestU32(original_memory.data(), kDevice + 13492, recording);
        Reject(Context(dispatch, 4, 16), dispatch);
      }
      for (uint32_t tail : {kStorage - 4, kStorage + 1, kStorage + kBytes + 4}) {
        Prepare();
        StoreGuestU32(original_memory.data(), kDevice + 14892, tail);
        Reject(Context(dispatch, 4, 16), dispatch);
      }
      for (uint32_t storage :
           {kDevice, kObject, dispatch ? kStack - 32 : kParameter, 0xBFFFF000u, 0xFFFFF000u}) {
        Prepare();
        StoreGuestU32(original_memory.data(), kObject + 152, storage);
        Reject(Context(dispatch, 4, 16), dispatch);
      }
    }
    for (uint32_t parameter : {kParameter + 1, kDevice + 4, kObject + 4, kStorage + 4}) {
      Prepare();
      auto ctx = Context(false, 0, 16);
      ctx.r5.u64 = parameter;
      Reject(ctx, false);
    }
    for (uint32_t sp : {kStack + 4, 96u, kDevice + 128}) {
      Prepare();
      auto ctx = Context(true, 4, 16);
      ctx.r1.u64 = sp;
      Reject(ctx, true);
    }
    // Normalize aliases before overlap checks, rather than comparing only the
    // guest addresses. These must fail without touching either mapped view.
    for (auto [storage, alias] :
         std::array<std::pair<uint32_t, uint32_t>, 3>{{{0xA0100000u, 0xC0100000u},
                                                       {0xA0101000u, 0xE0100000u},
                                                       {0x81000000u, 0x91000000u}}}) {
      Prepare();
      CommitView(original_memory, alias, 184);
      CommitView(native_memory, alias, 184);
      auto* base = original_memory.data();
      std::memcpy(simpsons::GuestToHost(base, alias), base + kObject, 184);
      StoreGuestU32(base, alias + 152, storage);
      StoreGuestU32(base, kDevice + 13492, alias);
      std::memcpy(simpsons::GuestToHost(native_memory.data(), alias),
                  simpsons::GuestToHost(base, alias), 184);
      Reject(Context(true, 4, 16), true);
      Require(!std::memcmp(simpsons::GuestToHost(base, alias),
                           simpsons::GuestToHost(native_memory.data(), alias), 184),
              "Rejected physical alias changed recording metadata");
    }
    std::printf(
        "{\"passed\":true,\"cases\":%llu,\"unchanged_fallbacks\":%llu,"
        "\"failed_allocations\":%llu,\"initializations\":%llu,"
        "\"budget_throttles\":%llu,\"wrapped_budgets\":%llu,"
        "\"physical_views\":%llu,\"lifecycle_calls\":%llu,"
        "\"real_storage_calls\":%llu,\"real_dispatch_calls\":%llu}\n",
        (unsigned long long)cases, (unsigned long long)rejections, (unsigned long long)failures,
        (unsigned long long)initializations, (unsigned long long)throttles,
        (unsigned long long)wraps, (unsigned long long)physical_views,
        (unsigned long long)sequence_calls, (unsigned long long)storage_calls,
        (unsigned long long)dispatch_calls);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s (after %llu comparisons, %llu rejections)\n", error.what(),
                 (unsigned long long)cases, (unsigned long long)rejections);
    return 1;
  }
}
