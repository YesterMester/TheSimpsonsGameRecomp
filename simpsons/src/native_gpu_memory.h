#pragma once

#include <array>
#include <cstdint>

namespace simpsons::native_gpu_memory {

struct MemoryRange {
  uint64_t first;
  uint64_t end;
};

namespace backing_detail {
// One entry per 16 MiB: every view boundary below is a multiple of 16 MiB, so
// the address's top byte selects its view. backing = address + delta (mod 2^64).
struct View {
  uint64_t end, delta, backing_limit;
};
constexpr std::array<View, 256> MakeViews() {
  std::array<View, 256> views{};
  for (uint32_t top = 0; top < 256; ++top) {
    const uint64_t address = uint64_t(top) << 24;
    if (address < 0x7F000000ull)
      views[top] = {0x7F000000ull, 0, UINT64_MAX};
    else if (address < 0x80000000ull)
      views[top] = {0x80000000ull, 0x100000000ull - 0x7F000000ull, UINT64_MAX};
    else if (address < 0x90000000ull)
      views[top] = {0x90000000ull, 0, UINT64_MAX};
    else if (address < 0xA0000000ull)
      views[top] = {0xA0000000ull, 0x80000000ull - 0x90000000ull, UINT64_MAX};
    else if (address < 0xC0000000ull)
      views[top] = {0xC0000000ull, 0x100000000ull - 0xA0000000ull, UINT64_MAX};
    else if (address < 0xE0000000ull)
      views[top] = {0xE0000000ull, 0x100000000ull - 0xC0000000ull, UINT64_MAX};
    else
      views[top] = {0x100000000ull, 0x100001000ull - 0xE0000000ull, 0x120000000ull};
  }
  return views;
}
inline constexpr std::array<View, 256> kViews = MakeViews();
}  // namespace backing_detail

// Match the backing views in system/xmemory.cpp, including the 0x7F writeback
// view, the two XEX views, A/C physical aliases, and E's physical-page offset.
// A range crossing a view boundary stays on the original path.
// Every native GPU call validates its guest ranges with this, tens of thousands
// of times per frame, so it is a table lookup that inlines. It returns exactly
// what the original if-chain did (79M-input equivalence test:
// ~/simpsons-test/harness/bench/backing_range_equiv.cpp).
inline bool BackingRange(uint32_t address, uint64_t size, MemoryRange& range) {
  const backing_detail::View& view = backing_detail::kViews[address >> 24];
  const uint64_t backing = uint64_t(address) + view.delta;
  if (!size || uint64_t(address) + size > view.end || backing + size > view.backing_limit)
    return false;
  range = {backing, backing + size};
  return true;
}

inline bool Overlaps(const MemoryRange& a, const MemoryRange& b) {
  return a.first < b.end && b.first < a.end;
}

}  // namespace simpsons::native_gpu_memory
