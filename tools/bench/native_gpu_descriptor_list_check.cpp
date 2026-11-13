#include <cstdint>
#include <rex/ppc/barriers.h>
static uint64_t descriptor_calls = 0, cache_calls = 0;
#define NATIVE_GPU_CHUNK_CLOSE_ENTRY unused_descriptor_close_main
#include "native_gpu_chunk_close_check.cpp"
#include "native_gpu_descriptor_list.h"

namespace {
constexpr uint32_t kList = 0xB000;
uint64_t descriptor_cases = 0, descriptor_rejections = 0, descriptor_waits = 0;
uint64_t descriptor_success = 0, descriptor_scratch = 0, descriptor_fixed = 0;
uint64_t descriptor_links = 0, descriptor_throttles = 0, descriptor_wraps = 0;
uint64_t cache_cases = 0, cache_early = 0, cache_rejections = 0;
uint32_t active_list_kind = 0, active_list_tail = 0, active_list = kList;
uint32_t active_heap_view = kPhysicalHeap;

void SetupList(uint32_t kind, uint32_t tail, uint32_t version, uint32_t fence,
               uint32_t previous, uint32_t budget, uint32_t view, uint32_t list) {
  active_list_kind = kind;
  active_list_tail = tail;
  active_list = list;
  active_heap_view = view;
  Prepare(kind, 0, 0, version, fence, 0);
  PhysicalSetup();
  auto* base = original_memory.data();
  StoreGuestU32(base, kDevice + 14880, view);
  StoreGuestU32(base, kDevice + 14884, view + kBytes);
  StoreGuestU32(base, kObject + 108, 2);
  StoreGuestU32(base, kObject + 152, view);
  StoreGuestU32(base, kObject + 156, kBytes);
  const uint32_t chunk = view + (tail == 2 ? 0x20000 : 4096);
  StoreGuestU32(base, kDevice + 14904, chunk);
  StoreGuestU32(base, kDevice + 48, chunk + 4092);
  const uint32_t capacity = view + (tail == 2 ? 0x30000 : 0x10000) - 60;
  StoreGuestU32(base, kDevice + 52, capacity);
  StoreGuestU32(base, kDevice + 56, capacity - 160);
  StoreGuestU32(base, kDevice + 14892,
                tail == 0 ? 0 : tail == 1 ? view + 0x38000 :
                tail == 2 ? view + kBytes - 4 : view + 0x3000);
  StoreGuestU32(base, kDevice + 14900, view);
  StoreGuestU32(base, kDevice + 14908, budget == 2 ? 0xFFFFF000u : 0);
  StoreGuestU32(base, kDevice + 14912, budget == 1 ? 128 : 0xFFFFFFFFu);
  StoreGuestU32(base, list, 0x12345678);
  StoreGuestU32(base, list + 4, previous ? view + 0x8000 : 0);
  StoreGuestU32(base, list + 8, previous ? view + 0x8000 + previous : 0);
  StoreGuestU32(base, list + 12, previous ? view + 0x8000 + 4220 : 0);
  StoreGuestU32(base, list + 16, kDevice);
}

void CheckList(bool must_reject = false, bool unsafe_reference = false) {
  Clone();
  // The sparse fixture reserves independent virtual pages. Commit and compare
  // each exercised physical view explicitly; it does not alias-map them.
  std::memcpy(simpsons::GuestToHost(native_memory.data(), active_heap_view),
              simpsons::GuestToHost(original_memory.data(), active_heap_view), kBytes);
  const auto equal = [] {
    MemoryEqual();
    Require(!std::memcmp(simpsons::GuestToHost(original_memory.data(), active_heap_view),
                         simpsons::GuestToHost(native_memory.data(), active_heap_view), kBytes),
            "Descriptor expansion changed its physical-view bytes");
  };
  auto input = Context(0), native = input, original = input;
  input.r3.u64 = native.r3.u64 = original.r3.u64 = 0xABCD000000000000ull | active_list;
  original_base = original_memory.data();
  original_guards.clear();
  native_guards.clear();
  const auto calls = std::array{descriptor_calls, cache_calls, default_calls, dispatch_calls,
                                recorded_calls, ready_calls, blocked_calls};
  const bool accepted = port::ExpandDescriptorList(native, native_memory.data());
  Require(calls == std::array{descriptor_calls, cache_calls, default_calls, dispatch_calls,
                              recorded_calls, ready_calls, blocked_calls},
          "Native expansion called an original driver, allocator or wait");
  Require(!must_reject || !accepted, "Unproved descriptor list was accepted");
  if (!accepted) {
    Require(!std::memcmp(&input, &native, sizeof(input)), "Rejected expansion changed context");
    equal();
    ++descriptor_rejections;
    if (!unsafe_reference) {
      try {
        reference_sub_82457748(original, original_memory.data());
      } catch (const WaitRequired&) {
        ++descriptor_waits;
      }
    }
    return;
  }
  const uint32_t previous = LoadGuestU32(original_memory.data(), active_list + 4);
  reference_sub_82457748(original, original_memory.data());
  Abi(original, native);
  equal();
  ++descriptor_cases;
  descriptor_success += cache_calls != calls[1];
  descriptor_scratch += cache_calls == calls[1];
  descriptor_fixed += active_list_kind == 1;
  descriptor_links += previous != 0;
  descriptor_wraps += active_list_tail == 2 && active_list_kind == 0 && cache_calls != calls[1];
  descriptor_throttles += LoadGuestU32(native_memory.data(), kDevice + 56) ==
                         LoadGuestU32(native_memory.data(), kDevice + 48);
}

void CheckCache(uint32_t first, uint32_t last, bool high) {
  Prepare(0, 0, 0, 0, 0, 0);
  auto input = Context(0);
  input.r3.u64 = (high ? 0xABCD000000000000ull : 0) | first;
  input.r4.u64 = (high ? 0xBCDE000000000000ull : 0) | last;
  input.xer.ca = 1;
  input.xer.so = 1;
  auto native = input, original = input;
  Clone();
  const auto before = cache_calls;
  const bool accepted = port::FlushCpuCommandRange(native, native_memory.data());
  Require(cache_calls == before, "Native cache range called the original");
  if (!accepted) {
    Require(!std::memcmp(&native, &input, sizeof(input)), "Rejected cache range changed context");
    MemoryEqual();
    ++cache_rejections;
    return;  // Reversed ranges can take billions of original no-op iterations.
  }
  reference_sub_8245DA50(original, original_memory.data());
  auto visible = native;
#if defined(NATIVE_GPU_REFERENCE_REGISTER_LOCALS)
  // register_locals.py deliberately keeps these volatile condition registers
  // private. Compare every returned general/vector register and all other
  // context bytes; the context-mode oracle also compares the flags themselves.
  visible.xer = original.xer;
  visible.cr0 = original.cr0;
  visible.cr6 = original.cr6;
#endif
  Require(!std::memcmp(&visible, &original, sizeof(visible)),
          "Native cache range changed register, condition, stack or link state");
  MemoryEqual();
  ++cache_cases;
  cache_early += first - 0x7F100000u <= 0x07EFFFFFu;
}
}  // namespace

int main(int argc, char** argv) {
  if (argc > 2 || (argc == 2 && !std::freopen(argv[1], "w", stdout)))
    return 2;
  try {
    for (auto* memory : {&original_memory, &native_memory}) {
      for (uint32_t view : {kPhysicalHeap, 0xA0010000u, 0xE000F000u})
        CommitPhysical(*memory, view, kBytes);
      CommitPhysical(*memory, kQueue, 4224);
      CommitPhysical(*memory, sync_view, 96);
    }
    for (uint32_t first : {0u, 1u, 127u, 0x7F0FFFFFu, 0x7F100000u, 0x7FFFFFFFu,
                           0x80000000u, 0x86FFFFFFu, 0x87000000u, 0xC0010000u, 0xFFFFF000u})
      for (uint32_t bytes : {0u, 1u, 127u, 128u, 129u, 1023u, 1024u, 1025u, 4224u, 65536u})
        for (bool high : {false, true})
          CheckCache(first, first + bytes, high);
    for (uint32_t first : {0x10000u, 0xC0010000u})
      CheckCache(first, first - 256, true);
    for (uint32_t kind : {0u, 1u, 3u})
      for (uint32_t tail : {0u, 1u, 2u, 3u})
        for (uint32_t version : {0u, 3u, 0xFFFFFFFFu})
          for (uint32_t fence : {0u, 1u, 3u})
            for (uint32_t previous : {0u, 4u, 100u, 4220u})
              for (uint32_t budget : {0u, 1u, 2u})
                for (uint32_t list : {kList, kDevice + 13232, kDevice + 13352}) {
                  const uint32_t view = std::array{kPhysicalHeap, 0xA0010000u, 0xE000F000u}[budget];
                  SetupList(kind, tail, version, fence, previous, budget, view, list);
                  CheckList();
                }
    for (uint32_t list : {kDevice, kStack - 32}) {
      SetupList(0, 0, 0, 0, 0, 0, kPhysicalHeap, list);
      CheckList(true, true);
    }
    for (uint32_t alias : {kDevice, kStack - 128, kPhysicalSync}) {
      SetupList(0, 0, 0, 0, 4, 0, kPhysicalHeap, kList);
      StoreGuestU32(original_memory.data(), kList + 4, alias);
      CheckList(true, true);
    }
    Require(descriptor_cases && descriptor_success && descriptor_scratch && descriptor_fixed &&
                descriptor_links && descriptor_throttles && descriptor_wraps && descriptor_waits &&
                cache_cases && cache_early && cache_rejections,
            "Descriptor matrix missed allocation, failure, wrap, wait or cache branches");
    std::printf("{\"passed\":true,\"descriptor_cases\":%llu,\"successful_allocations\":%llu,"
                "\"scratch_fallbacks\":%llu,\"fixed_owner_cases\":%llu,\"linked_blocks\":%llu,"
                "\"throttled_allocations\":%llu,\"wrapped_allocations\":%llu,"
                "\"unchanged_fallbacks\":%llu,\"real_wait_fallbacks\":%llu,"
                "\"cache_cases\":%llu,\"cache_early_returns\":%llu,\"cache_rejections\":%llu}\n",
                (unsigned long long)descriptor_cases, (unsigned long long)descriptor_success,
                (unsigned long long)descriptor_scratch, (unsigned long long)descriptor_fixed,
                (unsigned long long)descriptor_links, (unsigned long long)descriptor_throttles,
                (unsigned long long)descriptor_wraps, (unsigned long long)descriptor_rejections,
                (unsigned long long)descriptor_waits, (unsigned long long)cache_cases,
                (unsigned long long)cache_early, (unsigned long long)cache_rejections);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s (kind=%u tail=%u list=%08x; %llu descriptor, %llu rejected, %llu cache)\n",
                 error.what(), active_list_kind, active_list_tail, active_list,
                 (unsigned long long)descriptor_cases, (unsigned long long)descriptor_rejections,
                 (unsigned long long)cache_cases);
    return 1;
  }
}
