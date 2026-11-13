#pragma once

#include <array>
#include <span>

#include "guest_memory.h"
#include "native_gpu_memory.h"

namespace simpsons::native_gpu_vertices {

// Fixed recordings allocate temporary vertices down from the end of their
// owned storage, shrinking command capacity by the same amount. Plan every
// allocation before flushing state; the real allocator still does the work.
inline bool PlanRecordedAllocations(
    const uint8_t* base, uint32_t device, uint32_t cursor,
    const native_gpu_memory::MemoryRange& commands,
    std::span<const native_gpu_memory::MemoryRange> protected_ranges,
    std::span<const uint32_t> dwords) {
  using namespace native_gpu_memory;
  const uint32_t recorded = LoadGuestU32(base, device + 13492);
  MemoryRange object, owner, whole_device;
  if (!recorded || (recorded & 3) || !BackingRange(device, 22256, whole_device) ||
      !BackingRange(recorded, 184, object) || Overlaps(object, commands) ||
      Overlaps(object, whole_device))
    return false;
  for (const auto& range : protected_ranges)
    if (Overlaps(object, range))
      return false;
  if ((LoadGuestU32(base, recorded + 108) & 15) != 2)
    return false;
  const uint32_t storage = LoadGuestU32(base, recorded + 152);
  const uint32_t bytes = LoadGuestU32(base, recorded + 156);
  if (!storage || ((storage | bytes) & 3) || uint64_t(storage) + bytes > UINT32_MAX ||
      !BackingRange(storage, bytes, owner) || Overlaps(owner, object) ||
      Overlaps(owner, whole_device) || commands.first < owner.first || commands.end > owner.end)
    return false;
  for (const auto& range : protected_ranges)
    if (Overlaps(owner, range))
      return false;
  uint32_t tail = LoadGuestU32(base, device + 14892);
  if (!tail)
    tail = storage + bytes;
  MemoryRange tail_word;
  if ((tail & 3) || tail < 4 || !BackingRange(tail - 4, 4, tail_word) ||
      tail_word.first < owner.first || tail_word.end > owner.end ||
      uint64_t(cursor) + 164 > UINT32_MAX)
    return false;
  uint32_t capacity = LoadGuestU32(base, device + 52);
  uint32_t limit = LoadGuestU32(base, device + 56);
  uint32_t budget = LoadGuestU32(base, device + 14908);
  if ((capacity | limit) & 3)
    return false;
  bool failed = false;
  for (uint32_t i = 0; i < dwords.size(); ++i) {
    if (failed)
      break;  // The real failure flag makes subsequent allocations return zero.
    const uint64_t size = uint64_t(dwords[i]) * 4;
    if (!size || size > tail)
      return false;
    const uint32_t first = uint32_t((tail - size) & ~uint64_t(15));
    if (first < cursor + 164) {
      failed = true;
    } else {
      const uint32_t consumed = tail - first;
      MemoryRange data;
      if (consumed > capacity || consumed > limit || !BackingRange(first, consumed, data) ||
          data.first < owner.first || data.end > owner.end || Overlaps(data, commands) ||
          Overlaps(data, object))
        return false;
      for (const auto& range : protected_ranges)
        if (Overlaps(data, range))
          return false;
      tail = first;
      capacity -= consumed;
      limit -= consumed;
    }
    // The original accounts the requested size even if storage ran out. Its
    // 32-bit budget can reset ring limits and, for a different heap, its tail.
    budget += uint32_t(size);
    if (budget > LoadGuestU32(base, device + 14912)) {
      capacity = cursor + 160;
      limit = cursor;
      if (LoadGuestU32(base, device + 14900) != LoadGuestU32(base, device + 14880) && !failed &&
          i + 1 < dwords.size())
        return false;
    }
  }
  if (failed)
    return true;
  MemoryRange reserved;
  // A planned flush precedes cursor. Its words were checked against the
  // owner and every allocation above; the deferred draw must still fit the
  // reduced capacity after the last allocation.
  return capacity > cursor && BackingRange(cursor + 4, uint64_t(capacity) - cursor, reserved) &&
         commands.end <= reserved.end;
}

}  // namespace simpsons::native_gpu_vertices
