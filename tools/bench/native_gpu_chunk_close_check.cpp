#include <cstdint>
static uint64_t completion_calls = 0, close_calls = 0, unsupported_close_calls = 0;
#define main unused_growth_oracle_main
#include "native_gpu_chunk_growth_check.cpp"
#undef main

#include "native_gpu_chunk_close.h"
#include "native_gpu_recorded.h"

namespace {
constexpr uint32_t kPhysicalHeap = 0xC0010000, kPhysicalSync = 0xC0008000;
constexpr uint32_t kQueue = 0xC0060000;
uint64_t completion_cases = 0, closed_cases = 0, refused = 0, waits = 0, unsupported_paths = 0;
uint64_t closed_growth = 0, closed_descriptors = 0, empty_descriptors = 0, header_checks = 0;
uint32_t sync_view = kPhysicalSync, current_output = 0;

void CommitPhysical(GuestMemoryFixture& memory, uint32_t address, uint32_t bytes) {
  memory.Commit(address, uint64_t(bytes) + (address >= 0xE0000000u ? 4096 : 0));
}
void Clone() {
  std::memcpy(native_memory.data(), original_memory.data(), kMemory);
  std::memcpy(simpsons::GuestToHost(native_memory.data(), kPhysicalHeap),
              simpsons::GuestToHost(original_memory.data(), kPhysicalHeap), kBytes);
  std::memcpy(simpsons::GuestToHost(native_memory.data(), sync_view),
              simpsons::GuestToHost(original_memory.data(), sync_view), 64);
  std::memcpy(simpsons::GuestToHost(native_memory.data(), kQueue),
              simpsons::GuestToHost(original_memory.data(), kQueue), 4224);
}
void MemoryEqual() {
  Require(!std::memcmp(original_memory.data(), native_memory.data(), kMemory) &&
              !std::memcmp(simpsons::GuestToHost(original_memory.data(), kPhysicalHeap),
                           simpsons::GuestToHost(native_memory.data(), kPhysicalHeap), kBytes) &&
              !std::memcmp(simpsons::GuestToHost(original_memory.data(), sync_view),
                           simpsons::GuestToHost(native_memory.data(), sync_view), 64) &&
              !std::memcmp(simpsons::GuestToHost(original_memory.data(), kQueue),
                           simpsons::GuestToHost(native_memory.data(), kQueue), 4224),
          "Chunk close changed guest bytes outside the native headers");
}
void Abi(const PPCContext& a, const PPCContext& b) {
  Require(a.r3.u64 == b.r3.u64 && a.r1.u64 == b.r1.u64 && a.lr == b.lr,
          "Chunk close changed return, stack or link ABI");
  for (auto field : kNonvolatile)
    Require((a.*field).u64 == (b.*field).u64, "Chunk close changed a nonvolatile register");
}
void PhysicalSetup() {
  auto* base = original_memory.data();
  sync_view = kPhysicalSync;
  StoreGuestU32(base, kDevice + 10896, sync_view);
  StoreGuestU32(base, sync_view, LoadGuestU32(base, kSync));
  StoreGuestU32(base, sync_view + 4, LoadGuestU32(base, kSync + 4));
  StoreGuestU32(base, kDevice + 14880, kPhysicalHeap);
  StoreGuestU32(base, kDevice + 14884, kPhysicalHeap + kBytes);
  StoreGuestU32(base, kDevice + 14892, kPhysicalHeap + 0x10004);
  StoreGuestU32(base, kDevice + 14900, kPhysicalHeap);
  StoreGuestU32(base, kObject + 152, kPhysicalHeap);
  StoreGuestU32(base, kObject + 156, kBytes);
}
void Completion(uint32_t sequence, uint32_t version, uint32_t offset, bool mirror, bool callback,
                uint32_t view, bool high_argument) {
  Prepare(0, 0, 0, version, 0, 0);
  PhysicalSetup();
  auto* base = original_memory.data();
  sync_view = view;
  StoreGuestU32(base, kDevice + 10896, view);
  StoreGuestU32(base, view, 0x12345678);
  StoreGuestU32(base, view + 4, 0xAABBCCDD);
  StoreGuestU32(base, kDevice + 10908, sequence);
  StoreGuestU32(base, kDevice + 21508, callback ? 0xDEADBEEFu : 0);
  *simpsons::GuestToHost(base, kDevice + 10941) = mirror ? 0xD2 : 0xD0;
  const uint32_t cursor = kPhysicalHeap + 4092 + offset;
  StoreGuestU32(base, kDevice + 48, cursor);
  StoreGuestU32(base, kDevice + 52, kPhysicalHeap + 0x8000 - 60);
  current_output = cursor + 4;
  auto original = Context(0), native = original;
  original.r4.u64 = native.r4.u64 = (high_argument ? 0xABCD000000000000ull : 0) | current_output;
  Clone();
  const auto before_calls = completion_calls;
  Require(port::WriteChunkCompletion(native, native_memory.data()),
          "Completion unexpectedly fell back");
  Require(before_calls == completion_calls, "Native completion called its original");
  reference_sub_82457210(original, base);
  Abi(original, native);
  for (uint32_t offset : {0u, 8u, 24u}) {
    const uint32_t actual = LoadGuestU32(native_memory.data(), current_output + offset);
    Require(simpsons::native_gpu_calls::RecordedLogicalHeader(actual) ==
                LoadGuestU32(base, current_output + offset),
            "Native completion header lost its original editing view");
    StoreGuestU32(base, current_output + offset, actual);
    ++header_checks;
  }
  MemoryEqual();
  ++completion_cases;
}
void SetupClose(uint32_t kind, bool grow, uint32_t words, uint32_t slot, uint32_t fence,
                uint32_t marker, uint32_t version) {
  Prepare(kind, 0, kind == 0 ? 2 : 0, version, fence, marker);
  PhysicalSetup();
  auto* base = original_memory.data();
  const uint32_t chunk = kind == 3 ? kScratch + 128 : kPhysicalHeap + 4096;
  const uint32_t cursor = chunk + words * 4 - 4;
  StoreGuestU32(base, kDevice + 48, cursor);
  StoreGuestU32(base, kDevice + 14904, chunk);
  StoreGuestU32(base, kDevice + 52, kind == 3 ? kScratch + 4800 : kPhysicalHeap + 0x8000 - 60);
  StoreGuestU32(base, kDevice + 56, grow ? kind == 1 ? 0 : cursor : cursor + 256);
  StoreGuestU32(base, kDevice + 13356, kQueue);
  StoreGuestU32(base, kDevice + 13360, kQueue + 4 + slot * 8);
  StoreGuestU32(base, kDevice + 13364, kQueue + 4220);
  StoreGuestU32(base, kDevice + 13216, kind == 0 ? kPhysicalHeap + 0x18000 : 0);
  if (kind == 1 || kind == 2)
    *simpsons::GuestToHost(base, kDevice + 10940) = 0x80;
}
void Close(bool require_accept = false, bool require_reject = false) {
  original_base = original_memory.data();
  auto input = Context(0), native = input, original = input;
  original_guards.clear();
  native_guards.clear();
  Clone();
  const auto calls =
      std::array{close_calls, growth_calls, recorded_calls, default_calls, marker_calls};
  const bool accepted =
      port::CloseCommandChunkWithGuard(native, native_memory.data(), [](void* ptr, size_t bytes) {
        RecordGuard(native_guards, native_memory.data(), ptr, bytes);
        return simpsons::native_gpu_memory::EnsureCommandStorageWritable(ptr, bytes);
      });
  Require(
      calls == std::array{close_calls, growth_calls, recorded_calls, default_calls, marker_calls},
      "Native close executed an original routine");
  Require(accepted || !require_accept, "Required close fell back");
  Require(!accepted || !require_reject, "Unproved queue alias was accepted");
  if (!accepted) {
    Require(!std::memcmp(&input, &native, sizeof(input)) && native_guards.empty(),
            "Rejected close changed context or called its guard");
    MemoryEqual();
    ++refused;
    try {
      reference_sub_82457B40(original, original_base);
    } catch (const WaitRequired&) {
      ++waits;
    } catch (const std::runtime_error&) {
      ++unsupported_paths;
    }
    return;
  }
  const uint32_t old_descriptor = LoadGuestU32(original_base, kDevice + 13360);
  reference_sub_82457B40(original, original_base);
  Abi(original, native);
  MemoryEqual();
  Require(original_guards == native_guards, "Closing changed growth guard arguments or order");
  closed_descriptors += LoadGuestU32(original_base, kDevice + 13360) != old_descriptor;
  closed_growth +=
      !original_guards.empty() || LoadGuestU32(original_base, kStack - 136) == 0x82457CA8;
  ++closed_cases;
}
void RejectCompletion(PPCContext ctx) {
  Clone();
  const auto input = ctx;
  Require(!port::WriteChunkCompletion(ctx, native_memory.data()),
          "Unproved completion was accepted");
  Require(!std::memcmp(&ctx, &input, sizeof(ctx)), "Rejected completion changed its context");
  MemoryEqual();
  ++refused;
}
}  // namespace

#ifndef NATIVE_GPU_CHUNK_CLOSE_ENTRY
#define NATIVE_GPU_CHUNK_CLOSE_ENTRY main
#endif
int NATIVE_GPU_CHUNK_CLOSE_ENTRY(int argc, char** argv) {
  if (argc > 2 || (argc == 2 && !std::freopen(argv[1], "w", stdout)))
    return 2;
  try {
    for (auto* memory : {&original_memory, &native_memory}) {
      CommitPhysical(*memory, kPhysicalHeap, kBytes);
      CommitPhysical(*memory, kQueue, 4224);
      for (uint32_t view : {0xA0008000u, kPhysicalSync, 0xE0007000u})
        CommitPhysical(*memory, view, 64);
    }
    for (uint32_t sequence : {0u, 16u, 0xFFFFFFFEu, 0xFFFFFFFFu})
      for (uint32_t version : {0u, 1u, 3u, 0xFFFFFFFFu})
        for (uint32_t offset : {0u, 4u, 28u})
          for (bool mirror : {false, true})
            for (bool callback : {false, true})
              for (uint32_t view : {0xA0008000u, kPhysicalSync, 0xE0007000u})
                for (bool high_argument : {false, true})
                  Completion(sequence, version, offset, mirror, callback, view, high_argument);
    for (uint32_t kind = 0; kind < 4; ++kind)
      for (bool grow : {false, true})
        for (uint32_t words : {0u, 1u, 17u, 1024u})
          for (uint32_t slot : {0u, 526u})
            for (uint32_t fence = 0; fence < 4; ++fence)
              for (uint32_t marker = 0; marker < 6; ++marker)
                for (uint32_t version : {0u, 0xFFFFFFFFu}) {
                  SetupClose(kind, grow, words, slot, fence, marker, version);
                  Close();
                }
    // An empty segment does not read malformed queue metadata.
    SetupClose(0, false, 0, 0, 0, 0, 0);
    StoreGuestU32(original_memory.data(), kDevice + 13356, 0xDEADBEEFu);
    Close(true);
    // A full queue takes its original allocation path; never fabricate a block.
    SetupClose(0, false, 17, 527, 0, 0, 0);
    Close();
    // Actual normal ring submission remains outside the native close contract.
    SetupClose(0, false, 17, 0, 0, 0, 0);
    StoreGuestU32(original_memory.data(), kDevice + 13216, 0);
    Close();
    // Queue storage may share the heap while remaining separate from commands.
    SetupClose(0, true, 17, 0, 0, 0, 0);
    StoreGuestU32(original_memory.data(), kDevice + 13356, kPhysicalHeap + 0x30000);
    StoreGuestU32(original_memory.data(), kDevice + 13360, kPhysicalHeap + 0x30004);
    StoreGuestU32(original_memory.data(), kDevice + 13364, kPhysicalHeap + 0x30000 + 4220);
    Close(true);
    // Reject overlap with the newly planned chunk before queue publication.
    SetupClose(0, true, 17, 0, 0, 0, 0);
    StoreGuestU32(original_memory.data(), kDevice + 13356, kPhysicalHeap + 0x10000);
    StoreGuestU32(original_memory.data(), kDevice + 13360, kPhysicalHeap + 0x10004);
    StoreGuestU32(original_memory.data(), kDevice + 13364, kPhysicalHeap + 0x10000 + 4220);
    Close();
    // A malformed ready limit must not publish a new chunk at the queue block.
    SetupClose(0, false, 32, 0, 0, 0, 0);
    const uint32_t next = LoadGuestU32(original_memory.data(), kDevice + 48) + 4;
    StoreGuestU32(original_memory.data(), kDevice + 52, next - 60);
    StoreGuestU32(original_memory.data(), kDevice + 13356, next);
    StoreGuestU32(original_memory.data(), kDevice + 13360, next + 4);
    StoreGuestU32(original_memory.data(), kDevice + 13364, next + 4220);
    Close(false, true);
    for (uint32_t target : {0u, kDevice, kPhysicalHeap + 4096, kStack - 32}) {
      Prepare(0, 0, 0, 0, 0, 0);
      PhysicalSetup();
      const uint32_t cursor = kPhysicalHeap + 4092;
      StoreGuestU32(original_memory.data(), kDevice + 48, cursor);
      StoreGuestU32(original_memory.data(), kDevice + 52, kPhysicalHeap + 0x8000 - 60);
      StoreGuestU32(original_memory.data(), kDevice + 10896, target);
      auto ctx = Context(0);
      ctx.r4.u64 = cursor + 4;
      RejectCompletion(ctx);
    }
    Require(completion_cases == 1152 && closed_cases + refused == 3082 && closed_cases > 2000 &&
                closed_growth && closed_descriptors && waits,
            "Close matrix did not cover its required branches");
    std::printf(
        "{\"passed\":true,\"completion_cases\":%llu,\"close_cases\":%llu,"
        "\"native_header_checks\":%llu,\"closed_descriptors\":%llu,"
        "\"growth_continuations\":%llu,\"unchanged_fallbacks\":%llu,"
        "\"real_wait_fallbacks\":%llu,\"unsupported_fallbacks\":%llu,"
        "\"real_completion_calls\":%llu,\"real_close_calls\":%llu,"
        "\"real_growth_calls\":%llu}\n",
        (unsigned long long)completion_cases, (unsigned long long)closed_cases,
        (unsigned long long)header_checks, (unsigned long long)closed_descriptors,
        (unsigned long long)closed_growth, (unsigned long long)refused, (unsigned long long)waits,
        (unsigned long long)unsupported_paths, (unsigned long long)completion_calls,
        (unsigned long long)close_calls, (unsigned long long)growth_calls);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s (after %llu completion, %llu close cases, %llu refusals)\n",
                 error.what(), (unsigned long long)completion_cases,
                 (unsigned long long)closed_cases, (unsigned long long)refused);
    return 1;
  }
}
