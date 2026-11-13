#include <cstdint>
static uint64_t wait_begin_calls = 0, wait_poll_calls = 0, wait_end_calls = 0;
static uint64_t WaitTimebase(unsigned char*);
static void WaitPollStep(struct PPCContext&, unsigned char*);
static void WaitBeginObserved(struct PPCContext&, unsigned char*);
static void WaitEndObserved(struct PPCContext&, unsigned char*);
static void WaitDispatchBoundary(struct PPCContext&, unsigned char*);
static void WaitHangBoundary(struct PPCContext&, unsigned char*);
#define main unused_fence_growth_main
#include "native_gpu_chunk_growth_check.cpp"
#undef main
#include "native_gpu_fence_waits.h"

struct DispatchRequired : std::runtime_error { DispatchRequired() : std::runtime_error("Real dispatch required") {} };
struct HangRequired : std::runtime_error { HangRequired() : std::runtime_error("Real GPU hang handling required") {} };
struct WaitCallbackRequired : std::runtime_error { WaitCallbackRequired() : std::runtime_error("Real profiling callback required") {} };

namespace {
constexpr uint32_t kWaitPcr = 0xB000, kWaitThread = 0xE000;
constexpr std::array<uint32_t, 3> kFenceViews = {0xC0008000u, 0xA0008000u, 0xE0007000u};
uint32_t wait_mode = 0, wait_after = 1, wait_steps = 0, wait_clocks = 0;
uint32_t initial_fence_view = kFenceViews[0], moved_fence_view = kFenceViews[1];
uint32_t completion_value = 0;
bool marker_wait = false, real_wait_runtime = false;
uint64_t allocation_cases = 0, marker_cases = 0, waiting_cases = 0, abort_cases = 0;
uint64_t dispatch_boundaries = 0, hang_boundaries = 0, wait_callback_boundaries = 0, refused_waits = 0;
uint64_t moved_owners = 0, first_poll_differences = 0;
struct WaitEvent {
  uint32_t phase, state, fence, submitted, initial_owner_word;
  uint64_t r3, r4, r5, r29, r30, r31, stack, link;
  bool operator==(const WaitEvent&) const = default;
};
std::vector<WaitEvent> native_wait_events, original_wait_events;

auto& WaitEvents(uint8_t* base) {
  return base == native_memory.data() ? native_wait_events : original_wait_events;
}
void WaitObserve(uint8_t* base, PPCContext& ctx, uint32_t phase) {
  const uint32_t sync = LoadGuestU32(base, kDevice + 10896);
  WaitEvents(base).push_back({phase, ctx.r3.u32, LoadGuestU32(base, sync + (marker_wait ? 0 : 4)),
      LoadGuestU32(base, kDevice + 10908),
      LoadGuestU32(base, initial_fence_view + (marker_wait ? 0 : 4)), ctx.r3.u64, ctx.r4.u64, ctx.r5.u64,
      ctx.r29.u64, ctx.r30.u64, ctx.r31.u64, ctx.r1.u64, ctx.lr});
}
void CompleteWait(uint8_t* base) {
  const uint32_t sync = LoadGuestU32(base, kDevice + 10896);
  StoreGuestU32(base, sync + (marker_wait ? 0 : 4), completion_value);
}
struct WaitOps {
  static void Begin(PPCContext& ctx, uint8_t* base) { wait_begin_entry(ctx, base); }
  static void Poll(PPCContext& ctx, uint8_t* base) { wait_poll_entry(ctx, base); }
  static void End(PPCContext& ctx, uint8_t* base) { wait_end_entry(ctx, base); }
  static void Dispatch(PPCContext& ctx, uint8_t* base) { WaitDispatchBoundary(ctx, base); }
};
void CloneWait() {
  std::memcpy(native_memory.data(), original_memory.data(), kMemory);
  for (uint32_t view : kFenceViews)
    std::memcpy(simpsons::GuestToHost(native_memory.data(), view),
                simpsons::GuestToHost(original_memory.data(), view), 96);
  std::memcpy(native_memory.data() + 0x821DD000u, original_memory.data() + 0x821DD000u, 4096);
}
void EqualWait() {
  Require(!std::memcmp(native_memory.data(), original_memory.data(), kMemory),
          "Native wait changed guest frames, wait state, timing statistics or device bytes");
  for (uint32_t view : kFenceViews)
    Require(!std::memcmp(simpsons::GuestToHost(native_memory.data(), view),
                         simpsons::GuestToHost(original_memory.data(), view), 96),
            "Native wait changed a completion word or its neighboring bytes");
}
void SetupWait(uint32_t view, uint32_t mode, uint32_t after, uint32_t submitted,
               uint32_t completed, uint32_t packed, bool reserved, bool callback) {
  Prepare(0, 0, 0, 0, 0, 0);
  auto* base = original_memory.data();
  initial_fence_view = view;
  moved_fence_view = view == kFenceViews[0] ? kFenceViews[1] : kFenceViews[0];
  wait_mode = mode;
  wait_after = after;
  StoreGuestU32(base, kDevice + 10896, view);
  StoreGuestU32(base, kDevice + 10908, submitted);
  StoreGuestU32(base, kDevice + 13216, reserved ? 0x12340000u : 0);
  StoreGuestU32(base, kDevice + 13420, callback ? 0x81234560u : 0);
  StoreGuestU32(base, kDevice + 21564, std::bit_cast<uint32_t>(1.0f));
  StoreGuestU32(base, kDevice + 10888, 9);
  StoreGuestU32(base, kWaitPcr + 256, kWaitThread);
  StoreGuestU32(base, kWaitThread + 88, 0);
  StoreGuestU32(base, kWaitThread + 332, 9);
  for (uint32_t sync : kFenceViews) {
    std::memset(simpsons::GuestToHost(base, sync), 0, 96);
    StoreGuestU32(base, sync, completed);
    StoreGuestU32(base, sync + 4, packed);
  }
  StoreGuestU32(base, 0x821DD324u, std::bit_cast<uint32_t>(1.0f));
}
void CheckWait(PPCContext input, bool must_reject = false) {
  CloneWait();
  native_wait_events.clear();
  original_wait_events.clear();
  const auto execute = [&](PPCContext& ctx, uint8_t* base, bool native) -> int {
    wait_steps = wait_clocks = 0;
    try {
      if (native)
        return marker_wait ? port::WaitForGpuMarker(ctx, base, WaitOps{})
                           : port::WaitForAllocationFence(ctx, base, WaitOps{});
      if (marker_wait)
        reference_sub_824574B8(ctx, base);
      else
        reference_sub_824564D0(ctx, base);
      return true;
    } catch (const DispatchRequired&) {
      return 2;
    } catch (const HangRequired&) {
      return 3;
    } catch (const WaitCallbackRequired&) {
      return 4;
    }
  };
  auto native = input, original = input;
  const auto calls = std::array{ready_calls, marker_calls};
  const auto helpers_before = std::array{wait_begin_calls, wait_poll_calls, wait_end_calls};
  const int accepted = execute(native, native_memory.data(), true);
  const auto helpers_after = std::array{wait_begin_calls, wait_poll_calls, wait_end_calls};
  Require(calls == std::array{ready_calls, marker_calls}, "Native wait called the original wait root");
  Require(!must_reject || !accepted, "Unproved wait was accepted");
  if (!accepted) {
    Require(!std::memcmp(&native, &input, sizeof(input)) && native_wait_events.empty(),
            "Rejected wait changed its context or called a helper");
    Require(helpers_before == helpers_after, "Rejected wait executed an original timing helper");
    EqualWait();
    ++refused_waits;
    return;
  }
  const uint32_t polls = wait_steps;
  const int expected = execute(original, original_memory.data(), false);
  const auto helpers_original = std::array{wait_begin_calls, wait_poll_calls, wait_end_calls};
  for (size_t i = 0; i < helpers_before.size(); ++i)
    Require(helpers_after[i] - helpers_before[i] == helpers_original[i] - helpers_after[i],
            "Native wait changed the number of original constructor, poll or cleanup calls");
  Require(accepted == expected && native_wait_events == original_wait_events,
          "Native wait changed helper arguments, ordering or a real boundary");
  EqualWait();
  if (accepted != 1) {
    dispatch_boundaries += accepted == 2;
    hang_boundaries += accepted == 3;
    wait_callback_boundaries += accepted == 4;
    return;
  }
  Require(native.r3.u64 == original.r3.u64 && native.r1.u64 == original.r1.u64 && native.lr == original.lr,
          "Native wait changed its full-width return, stack or link");
  for (auto field : kNonvolatile)
    Require((native.*field).u64 == (original.*field).u64, "Native wait changed a nonvolatile register");
  const bool waited = !native_wait_events.empty();
  waiting_cases += waited;
  abort_cases += waited && wait_mode == 1;
  moved_owners += waited && wait_mode == 6 && polls;
  if (waited && wait_mode == 2) {
    Require(polls == (marker_wait ? 0u : 1u), "Native wait lost the marker/allocation first-poll distinction");
    ++first_poll_differences;
  }
  if (marker_wait)
    ++marker_cases;
  else
    ++allocation_cases;
}
}  // namespace

static uint64_t WaitTimebase(uint8_t* base) {
  if (real_wait_runtime)
    return REX_QUERY_TIMEBASE();
  if (wait_mode == 2 && !wait_clocks)
    CompleteWait(base);
  return 1000 + uint64_t(wait_clocks++) * 57;
}
static void WaitPollStep(PPCContext& ctx, uint8_t* base) {
  WaitObserve(base, ctx, 1);
  ++wait_steps;
  Require(wait_steps < 32, "Wait fixture exceeded its bounded progress script");
  const uint32_t tls = LoadGuestU32(base, kWaitPcr + 256);
  StoreGuestU32(base, tls + 88, wait_mode == 3 ? 5000 : wait_steps * 100);
  if (wait_mode == 6 && wait_steps == 1)
    StoreGuestU32(base, kDevice + 10896, moved_fence_view);
  if (wait_steps >= wait_after) {
    if (wait_mode == 1)
      *simpsons::GuestToHost(base, kDevice + 10941) |= 2;
    else if (wait_mode != 3)
      CompleteWait(base);
  }
}
static void WaitBeginObserved(PPCContext& ctx, uint8_t* base) {
  if (!real_wait_runtime)
    WaitObserve(base, ctx, 0);
}
static void WaitEndObserved(PPCContext& ctx, uint8_t* base) {
  if (!real_wait_runtime)
    WaitObserve(base, ctx, 2);
}
static void WaitDispatchBoundary(PPCContext& ctx, uint8_t* base) {
  WaitObserve(base, ctx, 3);
  throw DispatchRequired();
}
static void WaitHangBoundary(PPCContext& ctx, uint8_t* base) {
  if (real_wait_runtime)
    throw std::runtime_error("Real receiver test reached GPU hang handling");
  WaitObserve(base, ctx, 4);
  throw HangRequired();
}
#ifndef NATIVE_GPU_REAL_WAIT_RUNTIME
namespace rex::runtime {
PPCFunc* ResolveIndirectFunction(uint32_t) { throw WaitCallbackRequired(); }
}  // namespace rex::runtime
#endif

#ifndef NATIVE_GPU_FENCE_WAITS_ENTRY
#define NATIVE_GPU_FENCE_WAITS_ENTRY main
#endif
int NATIVE_GPU_FENCE_WAITS_ENTRY(int argc, char** argv) {
  if (argc > 2 || (argc == 2 && !std::freopen(argv[1], "w", stdout)))
    return 2;
  try {
    for (auto* memory : {&original_memory, &native_memory}) {
      for (uint32_t sync : kFenceViews)
        memory->Commit(sync, 96 + (sync >= 0xE0000000u ? 4096 : 0));
      memory->Commit(0x821DD000u, 4096);
    }
    for (uint32_t view : kFenceViews)
      for (uint32_t version : {0u, 1u, 3u, 0xFFFFFFFFu})
        for (uint32_t bits : {0u, 1u, 2u, 3u})
          for (int32_t offset : {-4, 0, 4})
            for (uint32_t mode : {0u, 1u, 2u, 3u, 6u})
              for (uint32_t after : {1u, 3u}) {
                marker_wait = false;
                SetupWait(view, mode, after, 4, 2, 0xC0012000u | bits, false, false);
                completion_value = 0xC0012000u | (version & 3);
                auto ctx = Context(0);
                ctx.r4.u64 = 0xABCD000000000000ull | uint32_t(0xC0012000u + offset);
                ctx.r5.u64 = 0xBCDE000000000000ull | version;
                ctx.r13.u64 = 0xCDEF000000000000ull | kWaitPcr;
                CheckWait(ctx);
              }
    marker_wait = true;
    for (uint32_t submitted : {0u, 4u, 0xFFFFFFFEu})
      for (uint32_t target : {0u, 1u, 4u, 0xFFFFFFFFu})
        for (uint32_t completed : {0u, 1u, 3u, 0xFFFFFFFDu})
          for (bool reserved : {false, true})
            for (uint32_t kind : {0u, 2u, 3u})
              for (uint32_t mode : {0u, 1u, 2u, 3u, 6u}) {
                SetupWait(kFenceViews[kind == 0 ? 0 : kind - 1], mode, 3, submitted, completed, 0,
                          reserved, false);
                completion_value = target;
                auto ctx = Context(0);
                ctx.r4.u64 = 0xABCD000000000000ull | target;
                ctx.r5.u64 = 0xBCDE000000000000ull | kind;
                ctx.r13.u64 = 0xCDEF000000000000ull | kWaitPcr;
                CheckWait(ctx);
              }
    for (bool marker : {false, true}) {
      marker_wait = marker;
      SetupWait(kFenceViews[0], 0, 1, 4, 0, 0, false, true);
      completion_value = marker ? 2 : 1;
      auto ctx = Context(0);
      ctx.r4.u64 = marker ? 2 : 0xC0012000u;
      ctx.r5.u64 = marker ? 2 : 1;
      ctx.r13.u64 = kWaitPcr;
      CheckWait(ctx);
    }
    marker_wait = true;
    SetupWait(kFenceViews[0], 0, 1, 0, 0, 0, false, false);
    auto empty = Context(0);
    empty.r3.u64 = 0xABCD0000F1234567ull;
    empty.r4.u64 = 0;
    CheckWait(empty);
    marker_wait = false;
    for (uint32_t alias : {0u, kDevice, kStack - 24}) {
      SetupWait(kFenceViews[0], 0, 1, 4, 0, 0, false, false);
      StoreGuestU32(original_memory.data(), kDevice + 10896, alias);
      auto ctx = Context(0);
      ctx.r4.u64 = 0xC0012000;
      ctx.r5.u64 = 1;
      CheckWait(ctx, true);
    }
    Require(allocation_cases && marker_cases && waiting_cases && abort_cases && dispatch_boundaries &&
                hang_boundaries && wait_callback_boundaries && moved_owners && first_poll_differences,
            "Wait matrix missed completion, abort, dispatch, hang, callback or live owner paths");
    std::printf("{\"passed\":true,\"allocation_cases\":%llu,\"marker_cases\":%llu,"
                "\"waited_cases\":%llu,\"abort_exits\":%llu,\"dispatch_boundaries\":%llu,"
                "\"hang_boundaries\":%llu,\"callback_boundaries\":%llu,\"moved_owners\":%llu,"
                "\"first_poll_checks\":%llu,\"unchanged_rejections\":%llu,"
                "\"scripted_progress_model\":true,\"hardware_gpu_executed\":false}\n",
                (unsigned long long)allocation_cases, (unsigned long long)marker_cases,
                (unsigned long long)waiting_cases, (unsigned long long)abort_cases,
                (unsigned long long)dispatch_boundaries, (unsigned long long)hang_boundaries,
                (unsigned long long)wait_callback_boundaries, (unsigned long long)moved_owners,
                (unsigned long long)first_poll_differences, (unsigned long long)refused_waits);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s (marker=%u mode=%u step=%u; %llu allocation, %llu marker)\n", error.what(),
                 marker_wait, wait_mode, wait_steps, (unsigned long long)allocation_cases,
                 (unsigned long long)marker_cases);
    return 1;
  }
}
