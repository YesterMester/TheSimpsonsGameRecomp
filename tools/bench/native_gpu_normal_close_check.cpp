static void ObserveOriginalCloseDoorbell(unsigned char*, unsigned int, unsigned int);
#define NATIVE_GPU_RING_SUBMISSION_ENTRY unused_ring_oracle_main
#include "native_gpu_ring_submission_check.cpp"
#include "native_gpu_normal_close.h"

namespace {
struct Publication {
  uint32_t sequence, completed, cursor, generation;
  std::array<uint32_t, 10> completion;
  bool operator==(const Publication&) const = default;
};
std::vector<Publication> original_publications, native_publications;
uint32_t active_close_words = 0, active_close_tail = 0;
bool active_close_grow = false, active_close_cpu = false;
uint64_t normal_cases = 0, normal_growth = 0, normal_wraps = 0, normal_cpu = 0;
uint64_t normal_rejected = 0, normal_waits = 0, normal_queues = 0, normal_callbacks = 0;
uint64_t normal_conservative = 0, normal_unsupported = 0, normal_headers = 0;

void CapturePublication(std::vector<Publication>& list, uint8_t* base) {
  Publication record{LoadGuestU32(base, kDevice + 10908), LoadGuestU32(base, sync_view),
                     LoadGuestU32(base, kDevice + 13224), LoadGuestU32(base, kDevice + 13228), {}};
  const uint32_t output = kPhysicalHeap + 4096 + active_close_words * 4;
  for (uint32_t i = 0; i < 10; ++i)
    record.completion[i] = simpsons::native_gpu_calls::RecordedLogicalHeader(
        LoadGuestU32(base, output + i * 4));
  list.push_back(record);
}

void SetupNormal(uint32_t words, bool grow, bool cpu, uint32_t version, uint32_t fence,
                 uint32_t tail, uint32_t marker, uint32_t start, uint32_t read,
                 uint32_t queued, uint32_t callback, uint32_t sequence, uint32_t ring) {
  SetupRing(31, start, read, 1, 0, callback, cpu, ring);
  active_close_words = words;
  active_close_grow = grow;
  active_close_cpu = cpu;
  active_close_tail = tail;
  auto* base = original_memory.data();
  const uint32_t cursor = kPhysicalHeap + 4096 + words * 4 - 4;
  StoreGuestU32(base, kDevice + 48, cursor);
  StoreGuestU32(base, kDevice + 14904, kPhysicalHeap + 4096);
  StoreGuestU32(base, kDevice + 56, grow ? cursor : kPhysicalHeap + 0x7000);
  StoreGuestU32(base, kDevice + 52, kPhysicalHeap + 0x8000 - 60);
  StoreGuestU32(base, kDevice + 14896, version);
  StoreGuestU32(base, kDevice + 14892, tail == 0 ? kPhysicalHeap + 0x10004
                                               : tail == 1 ? kPhysicalHeap + kBytes - 4 : 0);
  StoreGuestU32(base, kDevice + 14916, marker);
  StoreGuestU32(base, kDevice + 11000, queued);
  StoreGuestU32(base, kDevice + 10908, sequence);
  StoreGuestU32(base, sync_view, 14);
  StoreGuestU32(base, sync_view + 4, (kPhysicalHeap + 0x30000) | fence);
}

void CheckNormal(bool require_accept = false, bool require_reject = false, bool run_reference = true) {
  CloneRing();
  auto input = Context(0), native = input, original = input;
  original_base = original_memory.data();
  original_doorbells.clear();
  native_doorbells.clear();
  original_publications.clear();
  native_publications.clear();
  original_guards.clear();
  native_guards.clear();
  const auto calls = std::array{completion_calls, close_calls, growth_calls, marker_calls,
                                default_calls, dispatch_calls, recorded_calls, ready_calls,
                                blocked_calls, recursive_calls, ring_ready_calls, ring_publish_calls,
                                buffer_submit_calls, unproved_queue_calls, callback_calls};
  const bool accepted = port::CloseNormalCommandChunkWithGuard(
      native, native_memory.data(),
      [](uint32_t address, uint32_t value) {
        Observe(native_doorbells, native_memory.data(), address, value);
        CapturePublication(native_publications, native_memory.data());
      },
      [](void* ptr, size_t bytes) {
        RecordGuard(native_guards, native_memory.data(), ptr, bytes);
        return simpsons::native_gpu_memory::EnsureCommandStorageWritable(ptr, bytes);
      });
  Require(calls == std::array{completion_calls, close_calls, growth_calls, marker_calls,
                              default_calls, dispatch_calls, recorded_calls, ready_calls,
                              blocked_calls, recursive_calls, ring_ready_calls, ring_publish_calls,
                              buffer_submit_calls, unproved_queue_calls, callback_calls},
          "Native close called an original routine or external callback");
  Require(accepted || !require_accept, "Required normal closure fell back");
  Require(!accepted || !require_reject, "Unproved normal closure was accepted");
  if (!accepted) {
    Require(!std::memcmp(&native, &input, sizeof(input)) && native_doorbells.empty() &&
                native_guards.empty(), "Rejected normal closure changed context or published effects");
    EqualRing();
    ++normal_rejected;
    if (!run_reference)
      return;
    try {
      reference_sub_82457B40(original, original_memory.data());
      ++normal_conservative;
    } catch (const WaitRequired&) {
      ++normal_waits;
    } catch (const QueueRequired&) {
      ++normal_queues;
    } catch (const CallbackRequired&) {
      ++normal_callbacks;
    } catch (const std::runtime_error&) {
      ++normal_unsupported;
    }
    return;
  }
  reference_sub_82457B40(original, original_memory.data());
  Abi(original, native);
  Require(original_guards == native_guards, "Normal closure changed storage guard order or arguments");
  Require(original_doorbells == native_doorbells && original_publications == native_publications,
          "Normal closure published before completion or changed MMIO-visible state");
  const uint32_t output = kPhysicalHeap + 4096 + active_close_words * 4;
  for (uint32_t offset : {0u, 8u, 24u}) {
    const uint32_t value = LoadGuestU32(native_memory.data(), output + offset);
    Require(simpsons::native_gpu_calls::RecordedLogicalHeader(value) ==
                LoadGuestU32(original_memory.data(), output + offset),
            "Normal close completion lost its logical header");
    StoreGuestU32(original_memory.data(), output + offset, value);
    ++normal_headers;
  }
  if (!native_doorbells.empty()) {
    const uint32_t start = (native_doorbells[0].value - 3) & current_mask;
    const uint32_t address = current_ring + start * 4;
    const uint32_t value = LoadGuestU32(native_memory.data(), address);
    Require(value == rex::graphics::native_gpu_indirect::kBufferHeader &&
                simpsons::native_gpu_calls::RecordedLogicalHeader(value) ==
                    LoadGuestU32(original_memory.data(), address),
            "Normal close submission lost its logical header");
    StoreGuestU32(original_memory.data(), address, value);
    ++normal_headers;
  }
  EqualRing();
  ++normal_cases;
  normal_cpu += active_close_cpu;
  normal_growth += active_close_grow;
  normal_wraps += active_close_grow && active_close_tail == 1;
}
}  // namespace

static void ObserveOriginalCloseDoorbell(uint8_t* base, uint32_t address, uint32_t value) {
  ObserveOriginalDoorbell(base, address, value);
  CapturePublication(original_publications, base);
}

int main(int argc, char** argv) {
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
    for (uint32_t words : {0u, 1u, 7u, 257u})
      for (bool grow : {false, true})
        for (bool cpu : {false, true})
          for (uint32_t version : {0u, 3u, 0xFFFFFFFFu})
            for (uint32_t fence : {0u, 1u, 3u})
              for (uint32_t tail : {0u, 1u, 2u})
                for (uint32_t marker : {0u, 15u})
                  for (uint32_t start : {0u, 30u, 31u})
                    for (uint32_t read : {0u, 4u}) {
                      SetupNormal(words, grow, cpu, version, fence, tail, marker, start, read,
                                  0, 0, version == 3 ? 0xFFFFFFFFu : 16,
                                  std::array{kRing, 0xA0070000u, 0xE006F000u}[tail]);
                      CheckNormal();
                    }
    for (uint32_t queued : {0u, 1u, 0xFFFFFFFFu})
      for (uint32_t callback : {0u, 1u, 2u})
        for (bool grow : {false, true})
          for (bool cpu : {false, true}) {
            SetupNormal(1, grow, cpu, 0, 0, 0, 0, 0, 0, queued, callback, 16, kRing);
            CheckNormal(queued == 0 && callback == 0 && !cpu, queued != 0 || callback == 1 ||
                            (callback == 2 && !cpu));
          }
    // The parent close save area must be protected in addition to child frames.
    for (uint32_t address : {kDevice, kStack - 24, kPhysicalHeap + 4096}) {
      SetupNormal(1, false, false, 0, 0, 0, 0, 0, 0, 0, 0, 16, kRing);
      StoreGuestU32(original_memory.data(), kDevice + 10896, address);
      CheckNormal(false, true, false);
    }
    Require(normal_cases && normal_growth && normal_wraps && normal_cpu && normal_waits &&
                normal_queues && normal_callbacks && normal_conservative && normal_headers,
            "Normal closure matrix missed growth, wrapping, CPU mode or real fallbacks");
    std::printf("{\"passed\":true,\"cases\":%llu,\"growth_cases\":%llu,\"wrapped_growth\":%llu,"
                "\"cpu_mode_cases\":%llu,\"unchanged_fallbacks\":%llu,\"real_wait_fallbacks\":%llu,"
                "\"real_queue_fallbacks\":%llu,\"real_callback_fallbacks\":%llu,"
                "\"conservative_fallbacks\":%llu,\"unsupported_fallbacks\":%llu,"
                "\"logical_headers\":%llu,\"hardware_mmio_executed\":false}\n",
                (unsigned long long)normal_cases, (unsigned long long)normal_growth,
                (unsigned long long)normal_wraps, (unsigned long long)normal_cpu,
                (unsigned long long)normal_rejected, (unsigned long long)normal_waits,
                (unsigned long long)normal_queues, (unsigned long long)normal_callbacks,
                (unsigned long long)normal_conservative, (unsigned long long)normal_unsupported,
                (unsigned long long)normal_headers);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s (words=%u grow=%u cpu=%u tail=%u; %llu accepted, %llu rejected)\n",
                 error.what(), active_close_words, active_close_grow, active_close_cpu,
                 active_close_tail, (unsigned long long)normal_cases,
                 (unsigned long long)normal_rejected);
    return 1;
  }
}
