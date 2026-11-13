#include "simpsons_init.h"
#include "native_gpu_chunk_growth.h"
#include "guest_memory_fixture.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <utility>
#include <vector>

struct WaitRequired : std::runtime_error {
  WaitRequired() : std::runtime_error("Original chunk growth needs a live wait") {}
};
static uint64_t default_calls = 0, dispatch_calls = 0, recorded_calls = 0, ready_calls = 0;
static uint64_t blocked_calls = 0, growth_calls = 0, marker_calls = 0, recursive_calls = 0;
struct GuardCall {
  uint32_t address, bytes, chunk, tail, budget, cursor, capacity, ready;
  bool operator==(const GuardCall&) const = default;
};
static std::vector<GuardCall> original_guards, native_guards;
static uint8_t* original_base;
static constexpr uint32_t kDevice = 0x1000;
static void RecordGuard(std::vector<GuardCall>& calls, uint8_t* base, void* address, size_t bytes) {
  calls.push_back(
      {uint32_t(static_cast<uint8_t*>(address) - base), uint32_t(bytes),
       simpsons::LoadGuestU32(base, kDevice + 14904), simpsons::LoadGuestU32(base, kDevice + 14892),
       simpsons::LoadGuestU32(base, kDevice + 14908), simpsons::LoadGuestU32(base, kDevice + 48),
       simpsons::LoadGuestU32(base, kDevice + 52), simpsons::LoadGuestU32(base, kDevice + 56)});
}
#include "native_gpu_chunk_growth_reference.inc"

namespace port = simpsons::native_gpu_vertices;
using simpsons::LoadGuestU32;
using simpsons::StoreGuestU32;

namespace {
constexpr uint32_t kSync = 0x8000, kObject = 0xA000, kHeap = 0x10000, kBytes = 0x40000;
constexpr uint32_t kScratch = 0x60000, kStack = 0x7F000, kMemory = 0x80000;
GuestMemoryFixture original_memory(kMemory), native_memory(kMemory);
constexpr std::array<PPCRegister PPCContext::*, 18> kNonvolatile = {
    &PPCContext::r14, &PPCContext::r15, &PPCContext::r16, &PPCContext::r17, &PPCContext::r18,
    &PPCContext::r19, &PPCContext::r20, &PPCContext::r21, &PPCContext::r22, &PPCContext::r23,
    &PPCContext::r24, &PPCContext::r25, &PPCContext::r26, &PPCContext::r27, &PPCContext::r28,
    &PPCContext::r29, &PPCContext::r30, &PPCContext::r31};
uint64_t cases = 0, rejected = 0, waiting = 0, unsupported = 0, conservative = 0;
uint64_t successful = 0, scratch = 0, already_failed = 0, clamped = 0, fixed = 0, markers = 0;
uint32_t active_kind = 0, active_pattern = 0, active_request = 0;

void Require(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}
void Prepare(uint32_t kind, uint32_t pattern, uint32_t reservation, uint32_t version,
             uint32_t fence, uint32_t marker, uint32_t storage_bytes = 131072) {
  active_kind = kind;
  active_pattern = pattern;
  auto* base = original_memory.data();
  std::memset(base, 0xCD, kMemory);
  std::memset(base + kDevice, 0, 22268);
  std::memset(base + kObject, 0, 184);
  StoreGuestU32(base, kDevice + 10896, kSync);
  StoreGuestU32(base, kDevice + 10908, 16);
  StoreGuestU32(base, kSync, marker == 5 ? 0xFFFFFFFEu : 14);
  StoreGuestU32(base, kSync + 4, (kHeap + 0x30000) | fence);
  StoreGuestU32(base, kDevice + 48, kHeap + 4096 - 4);
  StoreGuestU32(base, kDevice + 52, kHeap + 0x10000 - 60);
  StoreGuestU32(base, kDevice + 56, kHeap + 0x10000 - 220);
  StoreGuestU32(base, kDevice + 14880, kHeap);
  StoreGuestU32(base, kDevice + 14884, kHeap + kBytes);
  StoreGuestU32(base, kDevice + 14888, pattern & 4 ? 0 : pattern & 2 ? 131072 : 4096);
  StoreGuestU32(base, kDevice + 14892, pattern & 1 ? kHeap + kBytes - 4 : kHeap + 0x10004);
  StoreGuestU32(base, kDevice + 14896, version);
  StoreGuestU32(base, kDevice + 14900, kHeap + (pattern & 2 ? 260 : 0));
  StoreGuestU32(base, kDevice + 14904, kHeap + 0x20000);
  StoreGuestU32(base, kDevice + 14908, 0xABCD1234u);
  StoreGuestU32(base, kDevice + 16704, kScratch);
  *simpsons::GuestToHost(base, kDevice + 10941) = kind == 3 ? 0xF0 : 0xD0;
  *simpsons::GuestToHost(base, kDevice + 10942) = 0xA1;
  if (reservation) {
    StoreGuestU32(base, kDevice + 13216, kHeap + (reservation & 1 ? 0x100 : 0x18000));
    StoreGuestU32(base, kDevice + 13220,
                  version + (reservation == 1   ? UINT32_MAX
                             : reservation == 2 ? 0
                                                : 1));
  }
  if (kind == 1 || kind == 2) {
    StoreGuestU32(base, kDevice + 13492, kObject);
    StoreGuestU32(base, kObject + 108, 2);
    StoreGuestU32(base, kObject + 152, kHeap);
    StoreGuestU32(base, kObject + 156, storage_bytes);
    StoreGuestU32(base, kObject + 172, pattern & 2);
    if (kind == 1)
      StoreGuestU32(base, kDevice + 56, 0);
  }
  if (marker) {
    StoreGuestU32(base, kDevice + 14916,
                  marker == 1   ? 14
                  : marker == 3 ? 16
                  : marker == 5 ? 0xFFFFFFFEu
                                : 15);
    if (marker == 4)
      StoreGuestU32(base, kDevice + 22264, 1);
    if (marker == 5)
      StoreGuestU32(base, kDevice + 10908, 1);
  }
}
PPCContext Context(uint32_t request) {
  PPCContext ctx{};
  active_request = request;
  ctx.r1.u64 = 0xA5A5000000000000ull | kStack;
  ctx.r3.u64 = 0xB6B6000000000000ull | kDevice;
  ctx.r4.u64 = 0xC7C7000000000000ull | request;
  ctx.lr = 0xFAFA000082345678ull;
  for (uint32_t i = 0; i < kNonvolatile.size(); ++i)
    (ctx.*kNonvolatile[i]).u64 = 0xABCD000000000000ull | i;
  return ctx;
}
bool Native(PPCContext& ctx) {
  return port::GrowCommandChunkWithGuard(ctx, native_memory.data(), [](void* ptr, size_t bytes) {
    RecordGuard(native_guards, native_memory.data(), ptr, bytes);
    return simpsons::native_gpu_memory::EnsureCommandStorageWritable(ptr, bytes);
  });
}
void Compare(PPCContext input, bool require_accept = false) {
  original_base = original_memory.data();
  std::memcpy(native_memory.data(), original_base, kMemory);
  original_guards.clear();
  native_guards.clear();
  auto native = input, original = input;
  const uint64_t call_count =
      growth_calls + default_calls + ready_calls + recorded_calls + marker_calls;
  const bool accepted = Native(native);
  Require(call_count == growth_calls + default_calls + ready_calls + recorded_calls + marker_calls,
          "Native growth executed an original routine");
  Require(accepted || !require_accept, "Required chunk growth fell back");
  if (!accepted) {
    Require(!std::memcmp(&input, &native, sizeof(input)) && native_guards.empty() &&
                !std::memcmp(original_base, native_memory.data(), kMemory),
            "Rejected growth changed memory, context or guard state");
    ++rejected;
    try {
      reference_sub_824578C0(original, original_base);
      ++conservative;
    } catch (const WaitRequired&) {
      ++waiting;
    } catch (const std::runtime_error&) {
      ++unsupported;
    }
    return;
  }
  reference_sub_824578C0(original, original_base);
  Require(original.r3.u64 == native.r3.u64 && original.r1.u64 == native.r1.u64 &&
              original.lr == native.lr,
          "Growth changed return, stack or link ABI");
  for (auto field : kNonvolatile)
    Require((original.*field).u64 == (native.*field).u64, "Growth changed a nonvolatile GPR");
  Require(original_guards == native_guards, "Growth changed guard arguments or publication order");
  Require(!std::memcmp(original_base, native_memory.data(), kMemory),
          "Growth changed allocation, private frames, marker or publication bytes");
  ++cases;
  successful += original.r3.u32 != 0;
  scratch += original.r3.u32 == 0;
  already_failed += active_kind == 3;
  fixed += active_kind == 1 || active_kind == 2;
  markers += LoadGuestU32(original_base, kDevice + 14916) == 0 &&
             LoadGuestU32(original_base, kStack - 136) == 0x82457A08;
  clamped += original.r3.u32 && LoadGuestU32(original_base, kDevice + 56) !=
                                    LoadGuestU32(original_base, kDevice + 52) - 160;
}
void Reject(PPCContext ctx) {
  std::memcpy(native_memory.data(), original_memory.data(), kMemory);
  native_guards.clear();
  const auto before = ctx;
  Require(!Native(ctx), "Unproved chunk-growth span was accepted");
  Require(!std::memcmp(&before, &ctx, sizeof(ctx)) && native_guards.empty() &&
              !std::memcmp(original_memory.data(), native_memory.data(), kMemory),
          "Unproved chunk growth changed state before fallback");
  ++rejected;
}
void CheckGuardPolicy() {
  constexpr uint32_t page = 0x30000000;
  original_memory.Commit(page, 4096);
  void* address = original_memory.data() + page;
#if defined(_WIN32)
  DWORD previous;
  Require(VirtualProtect(address, 4096, PAGE_READONLY, &previous), "Cannot protect fixture page");
  Require(simpsons::native_gpu_memory::EnsureCommandStorageWritable(address, 131072),
          "Guard failed");
  MEMORY_BASIC_INFORMATION info{};
  Require(VirtualQuery(address, &info, sizeof(info)) && info.Protect == PAGE_READONLY,
          "Shared guard removed host protection");
  VirtualProtect(address, 4096, previous, &previous);
#else
  Require(!mprotect(address, 4096, PROT_READ), "Cannot protect fixture page");
  Require(simpsons::native_gpu_memory::EnsureCommandStorageWritable(address, 131072),
          "Guard failed");
  FILE* maps = std::fopen("/proc/self/maps", "r");
  Require(maps != nullptr, "Cannot inspect fixture page protection");
  char line[1024], permissions[5];
  unsigned long first, end;
  bool found = false;
  while (std::fgets(line, sizeof(line), maps))
    if (std::sscanf(line, "%lx-%lx %4s", &first, &end, permissions) == 3 &&
        uintptr_t(address) >= first && uintptr_t(address) < end) {
      Require(permissions[0] == 'r' && permissions[1] != 'w',
              "Shared guard removed host protection");
      found = true;
      break;
    }
  std::fclose(maps);
  Require(found, "Fixture page was missing from the host map");
  Require(!mprotect(address, 4096, PROT_READ | PROT_WRITE), "Cannot restore fixture page");
#endif
}
}  // namespace

int main(int argc, char** argv) {
  if (argc > 2 || (argc == 2 && !std::freopen(argv[1], "w", stdout)))
    return 2;
  try {
    CheckGuardPolicy();
    for (uint32_t kind = 0; kind < 4; ++kind)
      for (uint32_t pattern = 0; pattern < 8; ++pattern)
        for (uint32_t reservation = 0; reservation < 4; ++reservation)
          for (uint32_t version : {0u, 1u, 0xFFFFFFFFu})
            for (uint32_t fence = 0; fence < 4; ++fence)
              for (uint32_t marker = 0; marker < 6; ++marker)
                for (uint32_t request : {0u, 1u, 20000u, 0xFFFFFFF3u}) {
                  Prepare(kind, pattern, reservation, version, fence, marker);
                  Compare(Context(request));
                }
    for (uint32_t bytes : {216u, 220u, 4096u, 65536u, 65752u, 65756u, 131072u}) {
      Prepare(1, 0, 0, 0, 0, 1, bytes);
      Compare(Context(0), true);
    }
    for (uint32_t bytes : {4u, 56u, 212u}) {
      Prepare(1, 0, 0, 0, 0, 0, bytes);
      Compare(Context(0));
    }
    // Wrapped request sizes, already-failed metadata and unused bad pointers.
    for (uint32_t request : {0x40000000u, 0xFFFFFFF2u, 0xFFFFFFFFu}) {
      Prepare(0, 0, 0, 0, 0, 0);
      Compare(Context(request), true);
    }
    Prepare(3, 0, 0, 0, 0, 2);
    StoreGuestU32(original_memory.data(), kDevice + 13492, 0xDEADBEEFu);
    StoreGuestU32(original_memory.data(), kDevice + 10896, 0xDEADBEEFu);
    Compare(Context(0xFFFFFFFFu), true);
    Prepare(1, 0, 0, 0, 0, 4);
    StoreGuestU32(original_memory.data(), kDevice + 10896, 0xDEADBEEFu);
    StoreGuestU32(original_memory.data(), kDevice + 16704, 0xDEADBEEFu);
    Compare(Context(0), true);
    for (uint32_t heap : {0xA0100000u, 0xC0100000u, 0xE0100000u, 0x91000000u})
      for (uint32_t kind : {0u, 1u}) {
        Prepare(kind, 0, 0, 0, 0, 0, 4096);
        StoreGuestU32(original_memory.data(), kDevice + 14880, heap);
        StoreGuestU32(original_memory.data(), kDevice + 14884, heap + kBytes);
        StoreGuestU32(original_memory.data(), kDevice + 14892, heap + 0x10004);
        StoreGuestU32(original_memory.data(), kObject + 152, heap);
        Compare(Context(0), true);
      }
    for (uint32_t sp : {kStack + 4, 368u, kDevice + 512}) {
      Prepare(1, 0, 0, 0, 0, 0);
      auto ctx = Context(0);
      ctx.r1.u64 = sp;
      Reject(ctx);
    }
    for (uint32_t scratch_address :
         {0u, kScratch + 1, kDevice, kObject, kStack - 128, 0xFFFFF000u}) {
      Prepare(2, 0, 0, 0, 0, 0);
      StoreGuestU32(original_memory.data(), kDevice + 16704, scratch_address);
      Reject(Context(0));
    }
    for (uint32_t completed : {0u, kSync + 1, kDevice, kObject, kHeap, kStack - 128}) {
      Prepare(1, 0, 0, 0, 0, 1);
      StoreGuestU32(original_memory.data(), kDevice + 10896, completed);
      Reject(Context(0));
    }
    // Aliased completed-fence storage must be rejected before dereferencing.
    for (auto [heap, completed] :
         std::array<std::pair<uint32_t, uint32_t>, 3>{{{0xA0100000u, 0xC0100000u},
                                                       {0xA0101000u, 0xE0100000u},
                                                       {0x81000000u, 0x91000000u}}}) {
      Prepare(1, 0, 0, 0, 0, 1);
      StoreGuestU32(original_memory.data(), kObject + 152, heap);
      StoreGuestU32(original_memory.data(), kDevice + 10896, completed);
      Reject(Context(0));
    }
    Require(cases > 10000 && clamped && successful && scratch && fixed && markers && waiting,
            "Growth matrix missed a required branch");
    std::printf(
        "{\"passed\":true,\"cases\":%llu,\"successful_chunks\":%llu,"
        "\"scratch_fallbacks\":%llu,\"already_failed\":%llu,"
        "\"fixed_cases\":%llu,\"ready_markers\":%llu,\"clamped_limits\":%llu,"
        "\"unchanged_fallbacks\":%llu,\"real_wait_fallbacks\":%llu,"
        "\"unsupported_fallbacks\":%llu,\"conservative_fallbacks\":%llu,"
        "\"real_growth_calls\":%llu,\"real_default_calls\":%llu,"
        "\"real_recorded_calls\":%llu,\"real_ready_calls\":%llu,"
        "\"real_marker_calls\":%llu,\"real_wait_branches\":%llu,"
        "\"recursive_growth_branches\":%llu,\"guard_policy_checks\":1}\n",
        (unsigned long long)cases, (unsigned long long)successful, (unsigned long long)scratch,
        (unsigned long long)already_failed, (unsigned long long)fixed, (unsigned long long)markers,
        (unsigned long long)clamped, (unsigned long long)rejected, (unsigned long long)waiting,
        (unsigned long long)unsupported, (unsigned long long)conservative,
        (unsigned long long)growth_calls, (unsigned long long)default_calls,
        (unsigned long long)recorded_calls, (unsigned long long)ready_calls,
        (unsigned long long)marker_calls, (unsigned long long)blocked_calls,
        (unsigned long long)recursive_calls);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s (kind %u, pattern %u, request %u; %llu comparisons, %llu rejects)\n",
                 error.what(), active_kind, active_pattern, active_request,
                 (unsigned long long)cases, (unsigned long long)rejected);
    return 1;
  }
}
