#include <atomic>
#include <cstdint>
static uint64_t submission_calls = 0;
static uint64_t reference_reservation = 0;
static std::atomic<uint64_t> range_drain_calls{0}, range_merge_calls{0};
static bool inject_range = false;
static uint64_t guard_range = 0;
static void InjectRange(uint8_t* base);
#define NATIVE_GPU_CHUNK_CLOSE_ENTRY unused_close_oracle_main
#include "native_gpu_chunk_close_check.cpp"

#include "native_gpu_submission_state.h"
#include "native_gpu_recorded_view.h"

#include <algorithm>
#include <thread>

static void InjectRange(uint8_t* base) {
  if (inject_range)
    port::submission_state_detail::RangeWord(base, kDevice)
        .store(std::byteswap(guard_range), std::memory_order_seq_cst);
}

namespace {
constexpr uint32_t kAddress = 0x9000, kCount = 0x9004, kIterator = 0x9200;
constexpr uint32_t kTag = 0x9300, kMetadata = 0x9400;
uint64_t comparisons = 0, refusals = 0, wait_fallbacks = 0, fixed_fallbacks = 0;
uint64_t empty_cases = 0, failed_cases = 0, native_flushes = 0, atomic_drains = 0;
uint64_t fixed_cases = 0, unsupported_fallbacks = 0;
uint64_t wrapped_allocations = 0, throttled_allocations = 0, editing_headers = 0;
uint64_t concurrent_updates = 0, raced_ranges = 0;
uint32_t active_heap = kPhysicalHeap, case_kind = 0, case_pattern = 0;
uint64_t case_range = 0;

void CloneSubmission() {
  Clone();
  std::memcpy(simpsons::GuestToHost(native_memory.data(), active_heap),
              simpsons::GuestToHost(original_memory.data(), active_heap), kBytes);
}
void EqualSubmission() {
  MemoryEqual();
  Require(!std::memcmp(simpsons::GuestToHost(original_memory.data(), active_heap),
                       simpsons::GuestToHost(native_memory.data(), active_heap), kBytes),
          "Submission changed an aliased heap, operand, padding or neighboring byte");
}
void BeforeGuard(std::vector<GuardCall>& calls, uint8_t* base, void* pointer, size_t bytes) {
  RecordGuard(calls, base, pointer, bytes);
  InjectRange(base);
}

void Setup(uint32_t kind, uint32_t pattern, uint32_t reservation, uint32_t version, uint32_t fence,
           uint64_t range, bool wrap, uint32_t heap) {
  case_kind = kind;
  case_pattern = pattern;
  case_range = range;
  active_heap = heap;
  Prepare(kind, pattern, reservation, version, fence, 0);
  PhysicalSetup();
  auto* base = original_memory.data();
  StoreGuestU32(base, kDevice + 14880, heap);
  StoreGuestU32(base, kDevice + 14884, heap + kBytes);
  StoreGuestU32(base, kDevice + 14892,
                (pattern & 3) == 0   ? heap + 0x10004
                : (pattern & 3) == 1 ? heap + kBytes - 128
                : (pattern & 3) == 2 ? 0
                                     : heap + kBytes - 16);
  StoreGuestU32(base, kDevice + 14900, heap + (pattern & 4 ? 260 : 0));
  StoreGuestU32(base, kDevice + 14904, heap + 0x20000);
  StoreGuestU32(base, kDevice + 48, heap + 4092);
  StoreGuestU32(base, kDevice + 52, heap + 0x8000 - 60);
  StoreGuestU32(base, kDevice + 56, heap + 0x8000 - 220);
  StoreGuestU32(base, kDevice + 14908, pattern & 4 ? 0xFFFFFFF0u : 128);
  StoreGuestU32(base, kDevice + 14912, pattern & 2 ? 256 : 0x7FFFFFFFu);
  if (reservation) {
    StoreGuestU32(base, kDevice + 13216, heap + (reservation & 1 ? 0x100 : 0x18000));
    StoreGuestU32(base, kDevice + 13220,
                  version + (reservation == 1 ? UINT32_MAX : reservation - 2));
  }
  StoreGuestU32(base, sync_view + 4, (heap + (pattern & 4 ? 0x100 : 0x30000)) | fence);
  *simpsons::GuestToHost(base, kDevice + 10942) = wrap ? 0xA5 : 0xA1;
  port::submission_state_detail::RangeWord(base, kDevice)
      .store(std::byteswap(range), std::memory_order_seq_cst);
  StoreGuestU32(base, kObject + 152, heap);
  StoreGuestU32(base, kObject + 156, kBytes);
}

void Check(PPCContext input, bool require_accept = false, bool require_reject = false) {
  original_base = original_memory.data();
  CloneSubmission();
  auto original = input, native = input;
  original_guards.clear();
  native_guards.clear();
  const int32_t locks = ppc_global_lock_count_().load();
  const auto calls = std::array{submission_calls,         default_calls,  dispatch_calls,
                                range_drain_calls.load(), recorded_calls, ready_calls};
  const uint32_t old_tail = LoadGuestU32(original_base, kDevice + 14900);
  const bool accepted = port::PrepareSubmissionStateWithGuard(
      native, native_memory.data(), [](void* pointer, size_t bytes) {
        BeforeGuard(native_guards, native_memory.data(), pointer, bytes);
        return simpsons::native_gpu_memory::EnsureCommandStorageWritable(pointer, bytes);
      });
  Require(calls == std::array{submission_calls, default_calls, dispatch_calls,
                              range_drain_calls.load(), recorded_calls, ready_calls},
          "Native state preparation called an original routine");
  Require(accepted || !require_accept, "Required state preparation fell back");
  Require(!accepted || !require_reject, "Unproved state preparation was accepted");
  Require(ppc_global_lock_count_().load() == locks, "State preparation leaked interrupt nesting");
  if (!accepted) {
    Require(!std::memcmp(&input, &native, sizeof(input)) && native_guards.empty(),
            "Rejected state preparation changed context or guard state");
    EqualSubmission();
    ++refusals;
    try {
      reference_sub_82457050(original, original_base);
      fixed_fallbacks += case_kind == 1;
    } catch (const WaitRequired&) {
      ++wait_fallbacks;
    } catch (const std::runtime_error&) {
      ++unsupported_fallbacks;
    }
    return;
  }
  // The reference guard has the same optional concurrent publication as the
  // native guard. It retains the actual original allocation and atomic drain.
  reference_reservation = input.reserved.u64;
  reference_sub_82457050(original, original_base);
  Abi(original, native);
  Require(original.msr == native.msr && original.r13.u64 == native.r13.u64 &&
              reference_reservation == native.reserved.u64,
          "State preparation changed interrupt restoration or the atomic reservation");
  Require(ppc_global_lock_count_().load() == locks, "Original state preparation leaked nesting");
  Require(original_guards == native_guards, "State preparation changed guard order or arguments");
  const uint32_t words = LoadGuestU32(native_memory.data(), kCount);
  if (words) {
    Require(words == 11 || words == 22, "State preparation changed the command span");
    const uint32_t first = native_guards.at(0).address + 4;
    for (uint32_t packet = 0; packet < words / 11; ++packet) {
      const uint32_t address = first + packet * 44;
      std::array<uint32_t, 10> payload;
      for (uint32_t i = 0; i < payload.size(); ++i)
        payload[i] = LoadGuestU32(native_memory.data(), address + (i + 1) * 4);
      rex::graphics::native_gpu_sync::Coherency call;
      Require(LoadGuestU32(native_memory.data(), address) ==
                      rex::graphics::native_gpu_sync::kCoherencyHeader &&
                  rex::graphics::native_gpu_sync::Decode(payload, call),
              "Submission lost its native coherency record");
      std::array<uint8_t, 28> iterator_before;
      std::memcpy(iterator_before.data(), native_memory.data() + kIterator, iterator_before.size());
      StoreGuestU32(native_memory.data(), kIterator, address);
      StoreGuestU32(native_memory.data(), kIterator + 4, address + 44);
      StoreGuestU32(native_memory.data(), kIterator + 8, address);
      StoreGuestU32(native_memory.data(), kIterator + 24, 0);
      {
        simpsons::native_gpu_calls::RecordedIteratorView view(native_memory.data(), kIterator, kTag,
                                                              kMetadata, kStack - 1024);
        for (uint32_t offset : {0u, 8u, 20u}) {
          const uint32_t actual = LoadGuestU32(native_memory.data(), address + offset);
          Require(view.Header(actual, address + offset) ==
                      LoadGuestU32(original_base, address + offset),
                  "Submission lost its original recorded editing view");
          StoreGuestU32(original_base, address + offset, actual);
          ++editing_headers;
        }
      }
      std::memcpy(native_memory.data() + kIterator, iterator_before.data(), iterator_before.size());
      ++native_flushes;
    }
    wrapped_allocations += LoadGuestU32(original_base, kDevice + 14900) != old_tail;
    throttled_allocations +=
        LoadGuestU32(original_base, kDevice + 56) == LoadGuestU32(original_base, kDevice + 48);
    atomic_drains += case_range != port::submission_state_detail::kEmptyRange;
    fixed_cases += case_kind == 1;
  } else {
    empty_cases += input.r3.u64 == native.r3.u64;
    failed_cases += native.r3.u64 == 0;
  }
  EqualSubmission();
  ++comparisons;
}

PPCContext Input() {
  auto ctx = Context(0);
  ctx.r4.u64 = 0xABCD000000000000ull | kAddress;
  ctx.r5.u64 = 0xBCDE000000000000ull | kCount;
  ctx.r13.u64 = case_pattern & 1 ? 0x8020 : 0;
  ctx.msr = 0xA5A58020;
  return ctx;
}

void Contention() {
  constexpr uint32_t writers = 4, writes = 2048, total = writers * writes;
  auto* base = native_memory.data();
  port::submission_state_detail::RangeWord(base, kDevice)
      .store(std::byteswap(port::submission_state_detail::kEmptyRange));
  std::atomic<uint32_t> running{writers};
  std::atomic<bool> start{false};
  std::array<std::thread, writers> threads;
  std::array<bool, total> covered{};
  for (uint32_t worker = 0; worker < writers; ++worker)
    threads[worker] = std::thread([&, worker] {
      while (!start.load(std::memory_order_acquire))
        std::this_thread::yield();
      for (uint32_t i = 0; i < writes; ++i) {
        PPCContext ctx{};
        ctx.r3.u64 = kDevice + 11816;
        ctx.r4.u64 = 0x10000 + (worker * writes + i) * 64;
        ctx.r5.u64 = ctx.r4.u64 + 32;
        reference_sub_82456768(ctx, base);
      }
      running.fetch_sub(1, std::memory_order_release);
    });
  auto collect = [&] {
    PPCContext ctx{};
    const uint64_t range = port::submission_state_detail::DrainRange(ctx, base, kDevice);
    if (range == port::submission_state_detail::kEmptyRange)
      return;
    const uint32_t first = uint32_t(range), end = uint32_t(range >> 32);
    Require(first >= 0x10000 && end > first && end <= 0x10000 + total * 64,
            "Contended range was torn or outside its published bounds");
    for (uint32_t i = (first - 0x10000) / 64; i <= (end - 1 - 0x10000) / 64; ++i)
      covered[i] = true;
  };
  start.store(true, std::memory_order_release);
  while (running.load(std::memory_order_acquire))
    collect();
  for (auto& thread : threads)
    thread.join();
  collect();
  Require(std::all_of(covered.begin(), covered.end(), [](bool value) { return value; }) &&
              std::byteswap(port::submission_state_detail::RangeWord(base, kDevice).load()) ==
                  port::submission_state_detail::kEmptyRange &&
              !ppc_global_lock_count_().load(),
          "Native range drain lost a published write or leaked a critical region");
  concurrent_updates += total;
}
}  // namespace

int main(int argc, char** argv) {
  if (argc > 2 || (argc == 2 && !std::freopen(argv[1], "w", stdout)))
    return 2;
  try {
    for (auto* memory : {&original_memory, &native_memory}) {
      CommitPhysical(*memory, kPhysicalHeap, kBytes);
      CommitPhysical(*memory, kQueue, 4224);
      CommitPhysical(*memory, 0xA0010000, kBytes);
      CommitPhysical(*memory, 0xE000F000, kBytes);
      CommitPhysical(*memory, sync_view, 64);
    }
    for (uint32_t kind : {0u, 1u, 3u})
      for (uint32_t pattern = 0; pattern < 8; ++pattern)
        for (uint32_t reservation = 0; reservation < 4; ++reservation)
          for (uint32_t version : {0u, 0xFFFFFFFFu})
            for (uint32_t fence = 0; fence < 4; ++fence)
              for (uint64_t range : std::array<uint64_t, 5>{
                       port::submission_state_detail::kEmptyRange, 0x0002400000012000ull,
                       0x0002400100012345ull, 0xFFFFFFFFFFFFFFFEull, 0ull})
                for (bool wrap : {false, true})
                  for (uint32_t heap : {kPhysicalHeap, 0xA0010000u, 0xE000F000u}) {
                    Setup(kind, pattern, reservation, version, fence, range, wrap, heap);
                    Check(Input());
                  }
    // Empty and sticky-failure paths ignore the unused address and heap.
    for (uint32_t kind : {0u, 3u}) {
      Setup(kind, 0, 0, 0, 0, port::submission_state_detail::kEmptyRange, false, kPhysicalHeap);
      StoreGuestU32(original_memory.data(), kDevice + 14880, 0xDEADBEEF);
      auto ctx = Input();
      ctx.r4.u64 = 0xDEADBEEF;
      Check(ctx, true);
      if (kind == 3) {
        Setup(3, 0, 0, 0, 0, 0x0002400000012000ull, true, kPhysicalHeap);
        StoreGuestU32(original_memory.data(), kDevice + 14880, 0xDEADBEEF);
        ctx = Input();
        ctx.r4.u64 = 0xDEADBEEF;
        Check(ctx, true);
      }
    }
    // A producer may publish after allocation but before the atomic drain.
    // A previously empty range stays pending if this call only flushes a wrap.
    for (uint64_t range : std::array<uint64_t, 2>{port::submission_state_detail::kEmptyRange,
                                                  0x0002400000012000ull}) {
      Setup(0, 0, 0, 0, 0, range, true, kPhysicalHeap);
      inject_range = true;
      guard_range = 0x0003600100023456ull;
      Check(Input(), true);
      inject_range = false;
      ++raced_ranges;
    }
    // Interrupt nesting uses the real runtime lock, shared with original producers.
    Setup(0, 0, 0, 0, 0, 0x0002400000012000ull, true, kPhysicalHeap);
    REX_ENTER_GLOBAL_LOCK();
    try {
      Check(Input(), true);
    } catch (...) {
      REX_LEAVE_GLOBAL_LOCK();
      throw;
    }
    REX_LEAVE_GLOBAL_LOCK();
    // Output aliases are refused before allocation or dirty-range consumption.
    for (uint32_t address :
         {kDevice, kStack - 24, kCount, kPhysicalHeap + 0x10020, kPhysicalSync}) {
      Setup(0, 0, 0, 0, 0, 0x0002400000012000ull, true, kPhysicalHeap);
      auto ctx = Input();
      ctx.r4.u64 = address;
      Check(ctx, false, true);
    }
    // Unknown recording allocation callbacks keep the original path.
    Setup(1, 0, 0, 0, 0, 0x0002400000012000ull, true, kPhysicalHeap);
    StoreGuestU32(original_memory.data(), kObject + 152, 0);
    Check(Input(), false, true);
    // A fixed owner's unused default heap and fence need not be dereferenced.
    Setup(1, 0, 0, 0, 0, 0x0002400000012000ull, false, kPhysicalHeap);
    StoreGuestU32(original_memory.data(), kDevice + 14880, 0xDEADBEEF);
    StoreGuestU32(original_memory.data(), kDevice + 10896, 0xDEADBEEF);
    Check(Input(), true);
    Contention();
    Require(comparisons > 10000 && refusals && wait_fallbacks && fixed_cases && empty_cases &&
                failed_cases && native_flushes && atomic_drains && wrapped_allocations &&
                throttled_allocations && editing_headers == native_flushes * 3,
            "Submission matrix missed a required allocation, drain or logical-editing branch");
    std::printf(
        "{\"passed\":true,\"cases\":%llu,\"unchanged_fallbacks\":%llu,"
        "\"real_wait_fallbacks\":%llu,\"fixed_owner_fallbacks\":%llu,"
        "\"fixed_owner_cases\":%llu,\"unsupported_fallbacks\":%llu,"
        "\"empty_cases\":%llu,\"failed_cases\":%llu,\"native_flushes\":%llu,"
        "\"atomic_drains\":%llu,\"wrapped_allocations\":%llu,"
        "\"throttled_allocations\":%llu,\"logical_headers\":%llu,"
        "\"concurrent_updates\":%llu,\"raced_ranges\":%llu}\n",
        (unsigned long long)comparisons, (unsigned long long)refusals,
        (unsigned long long)wait_fallbacks, (unsigned long long)fixed_fallbacks,
        (unsigned long long)fixed_cases, (unsigned long long)unsupported_fallbacks,
        (unsigned long long)empty_cases, (unsigned long long)failed_cases,
        (unsigned long long)native_flushes, (unsigned long long)atomic_drains,
        (unsigned long long)wrapped_allocations, (unsigned long long)throttled_allocations,
        (unsigned long long)editing_headers, (unsigned long long)concurrent_updates,
        (unsigned long long)raced_ranges);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s (kind=%u pattern=%u range=%016llX; %llu cases, %llu refusals)\n",
                 error.what(), case_kind, case_pattern, (unsigned long long)case_range,
                 (unsigned long long)comparisons, (unsigned long long)refusals);
    return 1;
  }
}
