#include <atomic>
static void ObserveQueueAcquire(struct PPCContext&, unsigned char*);
static void ObserveQueueRelease(struct PPCContext&, unsigned char*);
static void ObserveOriginalQueueStore(unsigned char*, unsigned int, unsigned int);
static uint64_t linked_append_calls = 0, queue_cache_calls = 0;
#define NATIVE_GPU_RING_SUBMISSION_ENTRY unused_queued_ring_main
#include "native_gpu_ring_submission_check.cpp"
#include "native_gpu_queued_submission.h"

namespace {
constexpr uint32_t kPcr = 0xD000, kList = kDevice + 13352;
uint32_t queue_node = kPhysicalHeap + 0x1000, queue_tail = kPhysicalHeap + 0x8000;
bool drain_at_lock = false, queue_test = false;
uint64_t append_cases = 0, queued_cases = 0, drained_cases = 0, rejected_queued = 0;
uint64_t append_fallbacks = 0, publish_fallbacks = 0, wait_boundaries = 0, callback_boundaries = 0;
uint64_t model_acquires = 0, model_releases = 0;

struct QueueEvent {
  uint32_t phase, address, value, count, lock_owner, irql, current, last, first, payload, source, end,
      tail_value;
  bool operator==(const QueueEvent&) const = default;
};
std::vector<QueueEvent> original_events, native_events;

void Capture(std::vector<QueueEvent>& events, uint8_t* base, uint32_t phase, uint32_t address,
             uint32_t value) {
  events.push_back({phase, address, value, LoadGuestU32(base, kDevice + 11000),
                    LoadGuestU32(base, kDevice + 11004), *simpsons::GuestToHost(base, kPcr + 24),
                    LoadGuestU32(base, kList + 4), LoadGuestU32(base, kList + 8),
                    LoadGuestU32(base, queue_node), LoadGuestU32(base, queue_node + 4),
                    LoadGuestU32(base, queue_node + 8), LoadGuestU32(base, queue_node + 12),
                    LoadGuestU32(base, queue_tail)});
}

void BoundaryStore(std::vector<QueueEvent>& events, uint8_t* base, uint32_t address, uint32_t value) {
  Capture(events, base, 2, address, value);
  if (address != port::ring_submission_detail::kDoorbell)
    *reinterpret_cast<volatile uint32_t*>(simpsons::GuestToHost(base, address)) = std::byteswap(value);
}

// A deterministic CPU boundary model for differential comparisons, including
// an actual atomic owner word. The separate runtime fixture exercises the real
// SDK kernel implementation and contention; this model does not claim either.
void BoundaryAcquire(std::vector<QueueEvent>& events, PPCContext& ctx, uint8_t* base) {
  auto* irql = simpsons::GuestToHost(base, ctx.r13.u32 + 24);
  const uint32_t old = *irql;
  *irql = 2;
  auto* word = reinterpret_cast<uint32_t*>(simpsons::GuestToHost(base, ctx.r3.u32));
  uint32_t expected = 0;
  Require(std::atomic_ref<uint32_t>(*word).compare_exchange_strong(expected, std::byteswap(ctx.r13.u32)),
          "Boundary model encountered real contention; use the SDK fixture");
  if (drain_at_lock)
    StoreGuestU32(base, kDevice + 11000, 0);
  Capture(events, base, 0, ctx.r3.u32, old);
  ctx.r3.u64 = old;
  ++model_acquires;
}

void BoundaryRelease(std::vector<QueueEvent>& events, PPCContext& ctx, uint8_t* base) {
  Require(LoadGuestU32(base, ctx.r3.u32) == ctx.r13.u32, "Released a lock owned by another PCR");
  Capture(events, base, 3, ctx.r3.u32, ctx.r4.u32);
  auto* word = reinterpret_cast<uint32_t*>(simpsons::GuestToHost(base, ctx.r3.u32));
  std::atomic_ref<uint32_t>(*word).store(0, std::memory_order_release);
  if (ctx.r4.u32 < 2)
    *simpsons::GuestToHost(base, ctx.r13.u32 + 24) = uint8_t(ctx.r4.u32);
  ++model_releases;
}

struct ObservedOps {
  static void Acquire(PPCContext& ctx, uint8_t* base) { BoundaryAcquire(native_events, ctx, base); }
  static void Release(PPCContext& ctx, uint8_t* base) { BoundaryRelease(native_events, ctx, base); }
  static void Store(uint8_t* base, uint32_t address, uint32_t value) {
    BoundaryStore(native_events, base, address, value);
    if (address == port::ring_submission_detail::kDoorbell)
      Observe(native_doorbells, base, address, value);
  }
  static void AppendOriginal(PPCContext& ctx, uint8_t* base) {
    ++append_fallbacks;
    reference_sub_82456A90(ctx, base);
  }
  static void PublishOriginal(PPCContext& ctx, uint8_t* base) {
    ++publish_fallbacks;
    reference_sub_82456E68(ctx, base);
  }
};

void SetupQueue(uint32_t count, bool drain, bool cpu, uint32_t read, uint32_t start,
                uint32_t irql, uint32_t callbacks, uint32_t tail, uint32_t node) {
  SetupRing(31, start, read, 1, 0, callbacks, cpu, kRing);
  auto* base = original_memory.data();
  drain_at_lock = drain;
  queue_tail = tail;
  queue_node = node;
  StoreGuestU32(base, kDevice + 11000, count);
  StoreGuestU32(base, kDevice + 11004, 0);
  StoreGuestU32(base, kList, kPhysicalHeap + 0x4000);
  StoreGuestU32(base, kList + 4, kPhysicalHeap + 0x4000);
  StoreGuestU32(base, kList + 8, tail);
  StoreGuestU32(base, kList + 16, kDevice);
  *simpsons::GuestToHost(base, kPcr + 24) = uint8_t(irql);
  for (uint32_t i = 0; i < 4; ++i)
    StoreGuestU32(base, node + i * 4, 0x12340000 + i);
  StoreGuestU32(base, tail, 0xC0000000);
}

void CheckQueue(PPCContext input, bool required_reject = false, bool unsafe_reference = false) {
  CloneRing();
  // Extra A/E pages are independent sparse fixture views, not alias mappings.
  for (uint32_t view : {0xA0010000u, 0xE000F000u})
    std::memcpy(simpsons::GuestToHost(native_memory.data(), view),
                simpsons::GuestToHost(original_memory.data(), view), kBytes);
  const auto equal = [] {
    EqualRing();
    for (uint32_t view : {0xA0010000u, 0xE000F000u})
      Require(!std::memcmp(simpsons::GuestToHost(native_memory.data(), view),
                           simpsons::GuestToHost(original_memory.data(), view), kBytes),
              "Queued submission changed a physical view");
  };
  original_events.clear();
  native_events.clear();
  original_doorbells.clear();
  native_doorbells.clear();
  const auto calls = std::array{linked_append_calls, queue_cache_calls, ring_ready_calls,
                                ring_publish_calls, buffer_submit_calls};
  const auto old_append_fallbacks = append_fallbacks, old_publish_fallbacks = publish_fallbacks;
  auto native = input, original = input;
  bool native_wait = false, native_callback = false;
  bool accepted = true;
  try {
    accepted = queue_test ? port::SubmitQueuedCommandBuffer(native, native_memory.data(), ObservedOps{})
                         : port::AppendLinkedCommandBuffer(native, native_memory.data(),
                               [](uint32_t address, uint32_t value) {
                                 ObservedOps::Store(native_memory.data(), address, value);
                               });
  } catch (const WaitRequired&) {
    native_wait = true;
  } catch (const CallbackRequired&) {
    native_callback = true;
  }
  Require(!required_reject || !accepted, "Unproved queue entry was accepted");
  if (old_append_fallbacks == append_fallbacks && old_publish_fallbacks == publish_fallbacks)
    Require(calls == std::array{linked_append_calls, queue_cache_calls, ring_ready_calls,
                                ring_publish_calls, buffer_submit_calls},
            "Native queue path called its original without a declared leaf fallback");
  if (!accepted) {
    Require(!std::memcmp(&input, &native, sizeof(input)) && native_events.empty(),
            "Rejected queue entry changed context, lock or publication");
    equal();
    ++rejected_queued;
    if (unsafe_reference)
      return;
  }
  bool original_wait = false, original_callback = false;
  try {
    if (queue_test)
      reference_sub_824572F8(original, original_memory.data());
    else
      reference_sub_82456A90(original, original_memory.data());
  } catch (const WaitRequired&) {
    original_wait = true;
  } catch (const CallbackRequired&) {
    original_callback = true;
  }
  if (!accepted)
    return;
  Require(native_wait == original_wait && native_callback == original_callback,
          "Queued submission fabricated a completed wait or callback");
  Require(native_events == original_events && native_doorbells == original_doorbells,
          "Queued submission changed lock, tail publication, release order or visible state");
  if (!native_wait && !native_callback)
    Abi(original, native);
  for (uint32_t i = 0; i <= current_mask; ++i) {
    const uint32_t value = LoadGuestU32(native_memory.data(), current_ring + i * 4);
    const uint32_t before = LoadGuestU32(original_memory.data(), current_ring + i * 4);
    Require(simpsons::native_gpu_calls::RecordedLogicalHeader(value) ==
                simpsons::native_gpu_calls::RecordedLogicalHeader(before),
            "Queued ring record lost its logical editing header");
    if (value != before) {
      Require(value == rex::graphics::native_gpu_indirect::kBufferHeader,
              "Queued ring payload changed");
      StoreGuestU32(original_memory.data(), current_ring + i * 4, value);
    }
  }
  equal();
  if (native_wait)
    ++wait_boundaries;
  else if (native_callback)
    ++callback_boundaries;
  else if (queue_test) {
    ++queued_cases;
    drained_cases += drain_at_lock;
  } else {
    ++append_cases;
  }
}
}  // namespace

static void ObserveQueueAcquire(PPCContext& ctx, uint8_t* base) { BoundaryAcquire(original_events, ctx, base); }
static void ObserveQueueRelease(PPCContext& ctx, uint8_t* base) { BoundaryRelease(original_events, ctx, base); }
static void ObserveOriginalQueueStore(uint8_t* base, uint32_t address, uint32_t value) {
  // A retained original leaf can run inside the native orchestration. Route
  // its observations to the memory image it actually operates on.
  const bool native = base == native_memory.data();
  BoundaryStore(native ? native_events : original_events, base, address, value);
  if (address == port::ring_submission_detail::kDoorbell) {
    if (native)
      Observe(native_doorbells, base, address, value);
    else
      ObserveOriginalDoorbell(base, address, value);
  }
}

int main(int argc, char** argv) {
  if (argc > 2 || (argc == 2 && !std::freopen(argv[1], "w", stdout)))
    return 2;
  try {
    for (auto* memory : {&original_memory, &native_memory}) {
      for (uint32_t view : {kPhysicalHeap, 0xA0010000u, 0xE000F000u})
        CommitPhysical(*memory, view, kBytes);
      CommitPhysical(*memory, kQueue, 4224);
      CommitPhysical(*memory, sync_view, 96);
      CommitPhysical(*memory, kRing, kRingBytes);
    }
    for (uint32_t view : {kPhysicalHeap, 0xA0010000u, 0xE000F000u})
      for (uint32_t words : {1u, 4u, 33u, 1024u})
        for (uint32_t offset : {0u, 4u, 124u}) {
          SetupQueue(1, false, false, 0, 0, 0, 0, view + 0x8000, view + 4096 + offset);
          auto ctx = Context(0);
          ctx.r3.u64 = 0xABCD000000000000ull | kList;
          ctx.r4.u64 = 0xBCDE000000000000ull | queue_node;
          ctx.r5.u64 = 0xCDEF000000000000ull | (queue_node + words * 4);
          CheckQueue(ctx);
        }
    queue_test = true;
    for (uint32_t queued : {1u, 0xFFFFFFFFu})
      for (uint32_t increment : {0u, 1u, 0xFFFFFFFFu})
        for (uint32_t words : {0u, 17u, 0x100000u})
          for (bool drain : {false, true})
            for (bool cpu : {false, true})
              for (uint32_t irql : {0u, 1u, 2u, 3u})
                for (uint32_t start : {0u, 30u})
                  for (uint32_t read : {0u, 4u})
                    for (uint32_t callback : {0u, 1u, 2u}) {
                      SetupQueue(queued, drain, cpu, read, start, irql, callback,
                                 kPhysicalHeap + 0x8000, kPhysicalHeap + 4096);
                      auto ctx = Context(0);
                      ctx.r4.u64 = 0xABCD000000000000ull | queue_node;
                      ctx.r5.u64 = 0xBCDE000000012000ull;
                      ctx.r6.u64 = 0xCDEF000000000000ull | words;
                      ctx.r7.u64 = 0xDEF0000000000000ull | increment;
                      ctx.r8.u64 = 0xEF01000000000000ull | kList;
                      ctx.r13.u64 = 0xF012000000000000ull | kPcr;
                      CheckQueue(ctx);
                    }
    // A valid custom tail keeps the actual original append leaf under the lock.
    SetupQueue(1, false, false, 0, 0, 0, 0, kQueue, kPhysicalHeap + 4096);
    auto ctx = Context(0);
    ctx.r4.u64 = queue_node;
    ctx.r5.u64 = 0x12000;
    ctx.r6.u64 = 17;
    ctx.r7.u64 = 1;
    ctx.r8.u64 = kList;
    ctx.r13.u64 = kPcr;
    CheckQueue(ctx);
    for (uint32_t pcr : {0u, kDevice, kStack - 24, kPhysicalHeap}) {
      SetupQueue(1, false, false, 0, 0, 0, 0, kPhysicalHeap + 0x8000, kPhysicalHeap + 4096);
      ctx.r13.u64 = pcr;
      CheckQueue(ctx, true, true);
    }
    Require(append_cases && queued_cases && drained_cases && wait_boundaries && callback_boundaries &&
                append_fallbacks && publish_fallbacks && rejected_queued && model_acquires == model_releases,
            "Queued matrix missed append, lock recheck, original fallback or real boundary");
    std::printf("{\"passed\":true,\"append_cases\":%llu,\"queued_cases\":%llu,"
                "\"drained_cases\":%llu,\"unchanged_rejections\":%llu,\"wait_boundaries\":%llu,"
                "\"callback_boundaries\":%llu,\"original_append_fallbacks\":%llu,"
                "\"original_publish_fallbacks\":%llu,\"observed_lock_pairs\":%llu,"
                "\"actual_sdk_lock_executed\":false,\"hardware_mmio_executed\":false}\n",
                (unsigned long long)append_cases, (unsigned long long)queued_cases,
                (unsigned long long)drained_cases, (unsigned long long)rejected_queued,
                (unsigned long long)wait_boundaries, (unsigned long long)callback_boundaries,
                (unsigned long long)append_fallbacks, (unsigned long long)publish_fallbacks,
                (unsigned long long)model_acquires);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s (queue=%u drain=%u; %llu append, %llu queued)\n", error.what(),
                 queue_test, drain_at_lock, (unsigned long long)append_cases,
                 (unsigned long long)queued_cases);
    return 1;
  }
}
