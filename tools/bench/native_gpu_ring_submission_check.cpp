#include <cstdint>
#include <stdexcept>
#include <rex/ppc/barriers.h>
struct QueueRequired : std::runtime_error {
  QueueRequired() : std::runtime_error("Original submission requires its locked queue") {}
};
struct CallbackRequired : std::runtime_error {
  CallbackRequired() : std::runtime_error("Original submission requires a real callback") {}
};
static uint64_t ring_ready_calls = 0, ring_publish_calls = 0, buffer_submit_calls = 0;
static uint64_t unproved_queue_calls = 0, callback_calls = 0;
static void ObserveOriginalDoorbell(uint8_t*, uint32_t, uint32_t);
static uint32_t ObserveCallbackWord(uint8_t*, uint32_t);
#define NATIVE_GPU_CHUNK_CLOSE_ENTRY unused_close_oracle_main
#include "native_gpu_chunk_close_check.cpp"
#include "native_gpu_ring_submission.h"

namespace rex::runtime {
PPCFunc* ResolveIndirectFunction(uint32_t) {
  ++callback_calls;
  throw CallbackRequired();
}
}  // namespace rex::runtime

namespace {
constexpr uint32_t kRing = 0xC0070000, kRingBytes = 32768, kDescriptors = 0xB000;
constexpr uint32_t kCallback = 0xE000, kVtable = 0xE100;
uint32_t current_ring = kRing, current_mask = 31, current_mode = 0, current_count = 0;
uint64_t ready_cases = 0, ring_cases = 0, submit_cases = 0, rejected_cases = 0;
uint64_t actual_waits = 0, actual_queues = 0, actual_callbacks = 0, conservative_cases = 0;
uint64_t cpu_cases = 0, published_buffers = 0, wrapped_publications = 0, logical_headers = 0;

struct DoorbellCall {
  uint32_t address, value, published_index, queued;
  std::vector<uint32_t> ring;
  bool operator==(const DoorbellCall&) const = default;
};
std::vector<DoorbellCall> original_doorbells, native_doorbells;

void Observe(std::vector<DoorbellCall>& calls, uint8_t* base, uint32_t address, uint32_t value) {
  DoorbellCall call{
      address, value, LoadGuestU32(base, kDevice + 10952), LoadGuestU32(base, kDevice + 11000), {}};
  for (uint32_t i = 0; i <= current_mask; ++i)
    call.ring.push_back(simpsons::native_gpu_calls::RecordedLogicalHeader(
        LoadGuestU32(base, current_ring + i * 4)));
  calls.push_back(std::move(call));
}

void SetupRing(uint32_t mask, uint32_t start, uint32_t read, uint32_t count, uint32_t owner,
               uint32_t callbacks, bool cpu, uint32_t ring) {
  current_ring = ring;
  current_mask = mask;
  current_count = count;
  Prepare(0, 0, 0, 0, 0, 0);
  PhysicalSetup();
  auto* base = original_memory.data();
  std::memset(simpsons::GuestToHost(base, ring), 0xCD, kRingBytes);
  StoreGuestU32(base, kDevice + 14872, ring);
  StoreGuestU32(base, kDevice + 14876, mask);
  StoreGuestU32(base, kDevice + 10952, start);
  StoreGuestU32(base, sync_view + 60, read);
  StoreGuestU32(base, kDevice + 11000, 0);
  StoreGuestU32(base, kDevice + 21508, callbacks == 1 ? kCallback : 0);
  StoreGuestU32(base, kDevice + 21648, callbacks == 2 ? 0x81234560 : 0);
  StoreGuestU32(base, kCallback, kVtable);
  StoreGuestU32(base, kVtable + 24, 0x81234560);
  StoreGuestU32(base, kVtable + 28, 0x81234580);
  if (cpu)
    *simpsons::GuestToHost(base, kDevice + 10941) |= 2;
  if (owner) {
    StoreGuestU32(base, kDevice + 14880, 0);
    StoreGuestU32(base, kDevice + 14884, 0);
  }
  if (owner == 1) {
    StoreGuestU32(base, kDevice + 13492, kObject);
    StoreGuestU32(base, kObject + 108, 2);
    StoreGuestU32(base, kObject + 152, kPhysicalHeap);
    StoreGuestU32(base, kObject + 156, kBytes);
  }
  for (uint32_t i = 0; i < std::min(count, 528u); ++i) {
    StoreGuestU32(base, kDescriptors + i * 8, 0x81000000 | (1 + i % 17));
    StoreGuestU32(base, kDescriptors + i * 8 + 4, 0x12000 + i * 128);
  }
}

void CloneRing() {
  Clone();
  std::memcpy(simpsons::GuestToHost(native_memory.data(), sync_view),
              simpsons::GuestToHost(original_memory.data(), sync_view), 96);
  std::memcpy(simpsons::GuestToHost(native_memory.data(), current_ring),
              simpsons::GuestToHost(original_memory.data(), current_ring), kRingBytes);
}

void EqualRing() {
  MemoryEqual();
  Require(!std::memcmp(simpsons::GuestToHost(original_memory.data(), current_ring),
                       simpsons::GuestToHost(native_memory.data(), current_ring), kRingBytes) &&
              !std::memcmp(simpsons::GuestToHost(original_memory.data(), sync_view),
                           simpsons::GuestToHost(native_memory.data(), sync_view), 96),
          "Ring publication changed storage, writeback, padding or neighboring bytes");
}

PPCContext RingInput() {
  auto ctx = Context(0);
  ctx.r4.u64 = 0xABCD000000000000ull | kDescriptors;
  ctx.r5.u64 = 0xBCDE000000000000ull | current_count;
  return ctx;
}

void CheckRing(PPCContext input, bool require_accept = false, bool require_reject = false) {
  CloneRing();
  original_doorbells.clear();
  native_doorbells.clear();
  auto native = input, original = input;
  const auto calls = std::array{ring_ready_calls, ring_publish_calls, buffer_submit_calls,
                                unproved_queue_calls, callback_calls};
  const auto doorbell = [](uint32_t address, uint32_t value) {
    Observe(native_doorbells, native_memory.data(), address, value);
  };
  const bool accepted = current_mode == 0 ? port::ReadySubmissionRing(native, native_memory.data())
                        : current_mode == 1
                            ? port::PublishSubmissionRing(native, native_memory.data(), doorbell)
                            : port::SubmitCommandBuffer(native, native_memory.data(), doorbell);
  Require(calls == std::array{ring_ready_calls, ring_publish_calls, buffer_submit_calls,
                              unproved_queue_calls, callback_calls},
          "Native publication executed an original driver or external callback");
  Require(accepted || !require_accept, "Required ring publication fell back");
  Require(!accepted || !require_reject, "Unproved ring publication was accepted");
  const auto reference = [&] {
    if (current_mode == 0)
      reference_sub_824565A8(original, original_memory.data());
    else if (current_mode == 1)
      reference_sub_82456E68(original, original_memory.data());
    else
      reference_sub_824572F8(original, original_memory.data());
  };
  if (!accepted) {
    Require(!std::memcmp(&native, &input, sizeof(input)) && native_doorbells.empty(),
            "Rejected publication changed context or published MMIO");
    EqualRing();
    ++rejected_cases;
    try {
      reference();
      ++conservative_cases;
    } catch (const WaitRequired&) {
      ++actual_waits;
    } catch (const QueueRequired&) {
      ++actual_queues;
    } catch (const CallbackRequired&) {
      ++actual_callbacks;
    }
    return;
  }
  const uint32_t start = LoadGuestU32(original_memory.data(), kDevice + 10952);
  reference();
  Abi(original, native);
  Require(original_doorbells == native_doorbells,
          "Native submission changed publication arguments, order or visible ring state");
  if (!native_doorbells.empty()) {
    Require(native_doorbells.size() == 1 &&
                native_doorbells[0].address == port::ring_submission_detail::kDoorbell &&
                native_doorbells[0].value == native_doorbells[0].published_index,
            "Native submission published an incorrect doorbell or stale index");
    const uint32_t count = current_mode == 2 ? 1 : input.r5.u32;
    for (uint32_t i = 0; i < count; ++i) {
      const uint32_t address = current_ring + ((start + i * 3) & current_mask) * 4;
      const uint32_t actual = LoadGuestU32(native_memory.data(), address);
      Require(actual == rex::graphics::native_gpu_indirect::kBufferHeader &&
                  simpsons::native_gpu_calls::RecordedLogicalHeader(actual) ==
                      LoadGuestU32(original_memory.data(), address),
              "Native buffer record lost its original ring/editing view");
      StoreGuestU32(original_memory.data(), address, actual);
      ++logical_headers;
    }
    published_buffers += count;
    wrapped_publications += uint64_t(start) + count * 3 > current_mask;
  } else {
    cpu_cases += current_mode != 0;
  }
  EqualRing();
  if (current_mode == 0)
    ++ready_cases;
  else if (current_mode == 1)
    ++ring_cases;
  else
    ++submit_cases;
}
}  // namespace

static void ObserveOriginalDoorbell(uint8_t* base, uint32_t address, uint32_t value) {
  Observe(original_doorbells, base, address, value);
}

static uint32_t ObserveCallbackWord(uint8_t* base, uint32_t address) {
  // Callback paths are never native successes. Read their real fixture vtable
  // word, refusing actual MMIO rather than simulating a successful device read.
  if ((address & 3) || uint64_t(address) + 4 > kMemory)
    throw CallbackRequired();
  return LoadGuestU32(base, address);
}

#ifndef NATIVE_GPU_RING_SUBMISSION_ENTRY
#define NATIVE_GPU_RING_SUBMISSION_ENTRY main
#endif
int NATIVE_GPU_RING_SUBMISSION_ENTRY(int argc, char** argv) {
  if (argc > 2 || (argc == 2 && !std::freopen(argv[1], "w", stdout)))
    return 2;
  try {
    for (auto* memory : {&original_memory, &native_memory}) {
      CommitPhysical(*memory, kPhysicalHeap, kBytes);
      CommitPhysical(*memory, kQueue, 4224);
      CommitPhysical(*memory, sync_view, 96);
      for (uint32_t ring : {kRing, 0xA0070000u, 0xE006F000u})
        CommitPhysical(*memory, ring, kRingBytes);
    }
    current_mode = 0;
    for (uint32_t mask : {3u, 7u, 15u, 31u})
      for (uint32_t start : {0u, 1u, mask / 2, mask - 1, mask})
        for (uint32_t words : {0u, 1u, 2u, 3u, mask / 2, mask})
          for (uint32_t read = 0; read <= mask; ++read) {
            SetupRing(mask, start, read, 1, 0, 0, false, kRing);
            auto ctx = RingInput();
            ctx.r4.u64 = 0xABCD000000000000ull | start;
            ctx.r5.u64 = 0xBCDE000000000000ull | words;
            CheckRing(ctx);
          }
    current_mode = 1;
    for (uint32_t mask : {7u, 31u, 255u, 8191u})
      for (uint32_t start : {0u, mask - 1, mask})
        for (uint32_t count : {0u, 1u, 2u, 5u, 527u})
          for (uint32_t read :
               {0u, 1u, start, (start + count * 3) & mask, (start + count * 3 + 1) & mask, mask})
            for (uint32_t owner = 0; owner < 3; ++owner)
              for (uint32_t callbacks = 0; callbacks < 3; ++callbacks)
                for (bool cpu : {false, true}) {
                  const uint32_t ring = std::array{kRing, 0xA0070000u, 0xE006F000u}[owner];
                  SetupRing(mask, start, read, count, owner, callbacks, cpu, ring);
                  CheckRing(RingInput());
                }
    current_mode = 2;
    for (uint32_t mask : {7u, 31u, 8191u})
      for (uint32_t start : {0u, mask - 1, mask})
        for (uint32_t read : {0u, 1u, start, (start + 3) & mask, (start + 4) & mask, mask})
          for (uint32_t queued : {0u, 1u, 0xFFFFFFFFu})
            for (uint32_t increment : {0u, 1u, 0xFFFFFFFFu})
              for (uint32_t words : {0u, 1u, 17u, 0x100000u})
                for (bool cpu : {false, true}) {
                  SetupRing(mask, start, read, 1, 0, 0, cpu, kRing);
                  StoreGuestU32(original_memory.data(), kDevice + 11000, queued);
                  auto ctx = RingInput();
                  ctx.r4.u64 = 0xABCDDEADBEEF1234ull;
                  ctx.r5.u64 = 0xBCDE000000012000ull;
                  ctx.r6.u64 = 0xCDEF000000000000ull | words;
                  ctx.r7.u64 = 0xDEF0000000000000ull | increment;
                  ctx.r8.u64 = 0xEF01000000000000ull | (kDevice + 13352);
                  CheckRing(ctx);
                }
    // A descriptor or physical alias may not overwrite the ring or writeback.
    current_mode = 1;
    for (uint32_t descriptors : {kDevice, kStack - 24, 0xA0070000u, kPhysicalSync}) {
      SetupRing(31, 0, 0, 1, 0, 0, false, kRing);
      auto ctx = RingInput();
      ctx.r4.u64 = descriptors;
      // These malformed inputs are rejected before an original unsafe dereference.
      CloneRing();
      const auto input = ctx;
      Require(
          !port::PublishSubmissionRing(ctx, native_memory.data(),
                                       [](uint32_t, uint32_t) {
                                         throw std::runtime_error("Rejected alias published MMIO");
                                       }) &&
              !std::memcmp(&ctx, &input, sizeof(ctx)),
          "Aliased ring input was accepted");
      EqualRing();
      ++rejected_cases;
    }
    Require(ready_cases && ring_cases && submit_cases && actual_waits && actual_queues &&
                actual_callbacks && cpu_cases && wrapped_publications && published_buffers &&
                logical_headers == published_buffers,
            "Ring matrix missed readiness, ownership, wrapping, queue or callback branches");
    std::printf(
        "{\"passed\":true,\"ready_cases\":%llu,\"ring_cases\":%llu,"
        "\"submit_cases\":%llu,\"unchanged_fallbacks\":%llu,"
        "\"real_wait_fallbacks\":%llu,\"real_queue_fallbacks\":%llu,"
        "\"real_callback_fallbacks\":%llu,\"conservative_fallbacks\":%llu,"
        "\"cpu_mode_cases\":%llu,\"published_buffers\":%llu,"
        "\"wrapped_publications\":%llu,\"logical_headers\":%llu,"
        "\"hardware_mmio_executed\":false}\n",
        (unsigned long long)ready_cases, (unsigned long long)ring_cases,
        (unsigned long long)submit_cases, (unsigned long long)rejected_cases,
        (unsigned long long)actual_waits, (unsigned long long)actual_queues,
        (unsigned long long)actual_callbacks, (unsigned long long)conservative_cases,
        (unsigned long long)cpu_cases, (unsigned long long)published_buffers,
        (unsigned long long)wrapped_publications, (unsigned long long)logical_headers);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr,
                 "%s (mode=%u mask=%u count=%u; %llu ready, %llu ring, %llu submit, "
                 "%llu rejected)\n",
                 error.what(), current_mode, current_mask, current_count,
                 (unsigned long long)ready_cases, (unsigned long long)ring_cases,
                 (unsigned long long)submit_cases, (unsigned long long)rejected_cases);
    return 1;
  }
}
