#include <cstdint>
static uint64_t orchestration_calls = 0, reservation_calls = 0;
#define NATIVE_GPU_CHUNK_CLOSE_ENTRY unused_close_oracle_main
#include "native_gpu_chunk_close_check.cpp"

#include "native_gpu_chunk_dispatch.h"

namespace {
constexpr uint32_t kGlobal = port::chunk_dispatch_detail::kSynchronizeChunks;
uint64_t dispatch_cases = 0, reserve_cases = 0, dispatch_refusals = 0;
uint64_t dispatch_waits = 0, dispatch_unsupported = 0, dispatch_growth = 0;
uint64_t post_close_reserves = 0, synchronized_closures = 0;
bool reserve_call = false;
uint32_t case_kind = 0, case_words = 0, case_request = 0, case_marker = 0;

void CompareDispatch(uint32_t request, bool required = false, bool required_reject = false) {
  original_base = original_memory.data();
  auto input = Context(request), native = input, original = input;
  original_guards.clear();
  native_guards.clear();
  Clone();
  StoreGuestU32(native_memory.data(), kGlobal, LoadGuestU32(original_base, kGlobal));
  const auto before = std::array{orchestration_calls, reservation_calls, close_calls, growth_calls,
                                 recorded_calls,      default_calls,     marker_calls};
  auto guard = [](void* pointer, size_t bytes) {
    RecordGuard(native_guards, native_memory.data(), pointer, bytes);
    return simpsons::native_gpu_memory::EnsureCommandStorageWritable(pointer, bytes);
  };
  const bool accepted =
      reserve_call ? port::ReserveCommandWordsWithGuard(native, native_memory.data(), guard)
                   : port::DispatchCommandChunkWithGuard(native, native_memory.data(), guard);
  Require(before == std::array{orchestration_calls, reservation_calls, close_calls, growth_calls,
                               recorded_calls, default_calls, marker_calls},
          "Native dispatch executed an original routine");
  Require(accepted || !required, "Required shared dispatch fell back");
  Require(!accepted || !required_reject, "Unproved shared dispatch was accepted");
  Require(LoadGuestU32(native_memory.data(), kGlobal) == LoadGuestU32(original_base, kGlobal),
          "Shared dispatch changed the global synchronization flag");
  auto reference = [&] {
    if (reserve_call)
      reference_sub_82457F50(original, original_base);
    else
      reference_sub_82457CC8(original, original_base);
  };
  if (!accepted) {
    Require(!std::memcmp(&input, &native, sizeof(input)) && native_guards.empty(),
            "Rejected dispatch changed context or guard state");
    MemoryEqual();
    ++dispatch_refusals;
    try {
      reference();
    } catch (const WaitRequired&) {
      ++dispatch_waits;
    } catch (const std::runtime_error&) {
      ++dispatch_unsupported;
    }
    return;
  }
  const uint32_t initial_cursor = LoadGuestU32(original_base, kDevice + 48);
  reference();
  Abi(original, native);
  MemoryEqual();
  Require(original_guards == native_guards,
          "Shared dispatch changed guard ordering, publication state or arguments");
  dispatch_growth += native_guards.size() > 1;
  post_close_reserves +=
      reserve_call && LoadGuestU32(original_base, kDevice + 48) != initial_cursor;
  synchronized_closures +=
      !(*simpsons::GuestToHost(native_memory.data(), kDevice + 10940) & 0x80) &&
      LoadGuestU32(native_memory.data(), kGlobal) && !native_guards.empty() &&
      (*simpsons::GuestToHost(native_memory.data(), kDevice + 10941) & 2);
  reserve_call ? ++reserve_cases : ++dispatch_cases;
}

void SetupDispatch(uint32_t kind, bool grow, uint32_t words, uint32_t slot, uint32_t marker,
                   bool global, bool mirror) {
  SetupClose(kind, grow, words, slot, 0, marker, marker == 5 ? 0xFFFFFFFFu : 0);
  if (kind == 3)
    *simpsons::GuestToHost(original_memory.data(), kDevice + 10940) |= 0x80;
  if (mirror)
    *simpsons::GuestToHost(original_memory.data(), kDevice + 10941) |= 2;
  StoreGuestU32(original_memory.data(), kGlobal, global);
  if (reserve_call && grow) {
    const uint32_t cursor = LoadGuestU32(original_memory.data(), kDevice + 48);
    StoreGuestU32(original_memory.data(), kDevice + 52, cursor + 16);
  }
}
}  // namespace

int main(int argc, char** argv) {
  if (argc > 2 || (argc == 2 && !std::freopen(argv[1], "w", stdout)))
    return 2;
  try {
    for (auto* memory : {&original_memory, &native_memory}) {
      CommitPhysical(*memory, kPhysicalHeap, kBytes);
      CommitPhysical(*memory, kQueue, 4224);
      for (uint32_t view : {0xA0008000u, kPhysicalSync, 0xE0007000u})
        CommitPhysical(*memory, view, 64);
      memory->Commit(kGlobal, 4);
    }
    for (bool reserve : {false, true}) {
      reserve_call = reserve;
      for (case_kind = 0; case_kind < 4; ++case_kind)
        for (bool grow : {false, true})
          for (uint32_t words : {0u, 17u, 1024u})
            for (uint32_t slot : {0u, 526u})
              for (case_marker = 0; case_marker < 6; ++case_marker)
                for (bool global : {false, true})
                  for (bool mirror : {false, true})
                    for (uint32_t request : {0u, 4u, 32u, 1024u, 40000u, 0x40000000u}) {
                      if (!reserve && request)
                        continue;
                      case_words = words;
                      case_request = request;
                      SetupDispatch(case_kind, grow, words, slot, case_marker, global, mirror);
                      CompareDispatch(request);
                    }
    }
    for (bool reserve : {false, true}) {
      reserve_call = reserve;
      // The sync branch preserves its unsigned comparison, including wrap and
      // target zero. A pending completion must decline before descriptor effects.
      for (uint32_t submitted : {0u, 1u, 2u, 16u, 0xFFFFFFFFu})
        for (uint32_t finished : {0u, 1u, 14u, 15u, 16u, 0xFFFFFFFEu}) {
          SetupDispatch(0, false, 17, 0, 0, true, false);
          StoreGuestU32(original_memory.data(), kDevice + 10908, submitted);
          StoreGuestU32(original_memory.data(), sync_view, finished);
          CompareDispatch(4);
        }
      // A full descriptor block keeps the real allocator path.
      SetupDispatch(0, true, 17, 527, 0, false, false);
      CompareDispatch(reserve ? 32 : 0, false, true);
      // Custom fixed-recording storage cannot be guessed from a pointer.
      SetupDispatch(1, true, 17, 0, 0, false, false);
      StoreGuestU32(original_memory.data(), kObject + 152, 0);
      CompareDispatch(reserve ? 32 : 0, false, true);
      // The normal ring path keeps its original state synthesis/submission.
      SetupDispatch(0, true, 17, 0, 0, false, false);
      StoreGuestU32(original_memory.data(), kDevice + 13216, 0);
      CompareDispatch(reserve ? 32 : 0, false, true);
      // Descriptor storage may share a heap, separately from both chunks.
      SetupDispatch(0, true, 17, 0, 0, false, false);
      StoreGuestU32(original_memory.data(), kDevice + 13356, kPhysicalHeap + 0x30000);
      StoreGuestU32(original_memory.data(), kDevice + 13360, kPhysicalHeap + 0x30004);
      StoreGuestU32(original_memory.data(), kDevice + 13364, kPhysicalHeap + 0x30000 + 4220);
      CompareDispatch(reserve ? 32 : 0, true);
      // Bad sync aliases and outer saved-frame aliases decline before effects.
      for (uint32_t sync : {kDevice, kPhysicalHeap + 4096, kStack - 24}) {
        SetupDispatch(0, true, 17, 0, 0, true, false);
        StoreGuestU32(original_memory.data(), kDevice + 10896, sync);
        CompareDispatch(reserve ? 32 : 0, false, true);
      }
    }
    reserve_call = true;
    SetupDispatch(0, false, 17, 0, 0, false, false);
    StoreGuestU32(original_memory.data(), kDevice + 13216, 0);
    CompareDispatch(4, true);  // Owned no-growth reservation also covers normal mode.
    // A scratch end sentinel cannot promise one more writable word beyond its owner.
    SetupDispatch(3, false, 17, 0, 0, false, false);
    StoreGuestU32(original_memory.data(), kDevice + 48, kScratch + 4796);
    CompareDispatch(1, false, true);
    Require(dispatch_cases > 1000 && reserve_cases > 1000 && dispatch_refusals > 1000 &&
                dispatch_growth && post_close_reserves && synchronized_closures && dispatch_waits,
            "Shared dispatch matrix missed required coverage");
    std::printf(
        "{\"passed\":true,\"dispatch_cases\":%llu,\"reserve_cases\":%llu,"
        "\"unchanged_fallbacks\":%llu,\"real_wait_fallbacks\":%llu,"
        "\"unsupported_fallbacks\":%llu,\"growth_continuations\":%llu,"
        "\"post_close_reservations\":%llu,\"synchronized_closures\":%llu,"
        "\"real_dispatch_calls\":%llu,\"real_reserve_calls\":%llu}\n",
        (unsigned long long)dispatch_cases, (unsigned long long)reserve_cases,
        (unsigned long long)dispatch_refusals, (unsigned long long)dispatch_waits,
        (unsigned long long)dispatch_unsupported, (unsigned long long)dispatch_growth,
        (unsigned long long)post_close_reserves, (unsigned long long)synchronized_closures,
        (unsigned long long)orchestration_calls, (unsigned long long)reservation_calls);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr,
                 "%s (reserve=%d kind=%u words=%u request=%u marker=%u; "
                 "%llu dispatch, %llu reserve, %llu refusals)\n",
                 error.what(), int(reserve_call), case_kind, case_words, case_request, case_marker,
                 (unsigned long long)dispatch_cases, (unsigned long long)reserve_cases,
                 (unsigned long long)dispatch_refusals);
    return 1;
  }
}
