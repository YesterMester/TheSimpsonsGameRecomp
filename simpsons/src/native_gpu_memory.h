#pragma once

#include <cstdint>

namespace simpsons::native_gpu_memory {

struct MemoryRange {
  uint64_t first;
  uint64_t end;
};

// Match the backing views in system/xmemory.cpp, including the 0x7F writeback
// view, the two XEX views, A/C physical aliases, and E's physical-page offset.
// A range crossing a view boundary stays on the original path.
inline bool BackingRange(uint32_t address, uint64_t size, MemoryRange& range) {
  if (!size || uint64_t(address) + size > 0x100000000ull)
    return false;
  uint64_t view_end, backing;
  if (address < 0x7F000000u) {
    view_end = 0x7F000000ull;
    backing = address;
  } else if (address < 0x80000000u) {
    view_end = 0x80000000ull;
    backing = 0x100000000ull + address - 0x7F000000u;
  } else if (address < 0x90000000u) {
    view_end = 0x90000000ull;
    backing = address;
  } else if (address < 0xA0000000u) {
    view_end = 0xA0000000ull;
    backing = 0x80000000ull + address - 0x90000000u;
  } else if (address < 0xC0000000u) {
    view_end = 0xC0000000ull;
    backing = 0x100000000ull + address - 0xA0000000u;
  } else if (address < 0xE0000000u) {
    view_end = 0xE0000000ull;
    backing = 0x100000000ull + address - 0xC0000000u;
  } else {
    view_end = 0x100000000ull;
    backing = 0x100001000ull + address - 0xE0000000u;
    if (backing + size > 0x120000000ull)
      return false;
  }
  if (uint64_t(address) + size > view_end)
    return false;
  range = {backing, backing + size};
  return true;
}

inline bool Overlaps(const MemoryRange& a, const MemoryRange& b) {
  return a.first < b.end && b.first < a.end;
}

}  // namespace simpsons::native_gpu_memory
