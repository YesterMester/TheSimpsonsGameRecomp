#pragma once

#include <rex/graphics/native_gpu_direct.h>
#include <rex/graphics/native_gpu_state.h>
#include "guest_memory.h"
#include "native_gpu_memory.h"

namespace simpsons::native_gpu_calls {

// Refreshed by the existing API entry hook on this submitting thread. Keeping
// this policy local avoids an additional cvar/capability call at every store.
inline thread_local bool direct_draw_headers_enabled = false;
inline thread_local bool direct_state_headers_enabled = false;

inline bool OwnsDirectRingSpan(uint8_t* base, uint32_t device, uint32_t destination,
                              uint32_t words) {
  if (!base || (device & 3) || (destination & 3))
    return false;
  const uint64_t bytes = uint64_t(words) * 4;
  native_gpu_memory::MemoryRange dev, commands;
  if (!native_gpu_memory::BackingRange(device, 22256, dev) ||
      !native_gpu_memory::BackingRange(destination, bytes, commands) ||
      native_gpu_memory::Overlaps(dev, commands))
    return false;
  if ((*GuestToHost(base, device + 10940) & 1) || LoadGuestU32(base, device + 13216))
    return false;
  // Keep the original setup, allocation and publication. The header store can
  // lower only a complete packet in the reserved, unpublished command span.
  const uint32_t published = LoadGuestU32(base, device + 48);
  const uint32_t capacity = LoadGuestU32(base, device + 52);
  native_gpu_memory::MemoryRange ring;
  if (((published | capacity) & 3) || destination <= published ||
      uint64_t(destination) + bytes > capacity)
    return false;
  const uint32_t recorded = LoadGuestU32(base, device + 13492);
  if (recorded) {
    native_gpu_memory::MemoryRange object, owner;
    if ((recorded & 3) || !native_gpu_memory::BackingRange(recorded, 184, object) ||
        native_gpu_memory::Overlaps(object, dev) || native_gpu_memory::Overlaps(object, commands) ||
        (LoadGuestU32(base, recorded + 108) & 15) != 2)
      return false;
    const uint32_t storage = LoadGuestU32(base, recorded + 152);
    const uint32_t bytes = LoadGuestU32(base, recorded + 156);
    if (!storage || ((storage | bytes) & 3) ||
        !native_gpu_memory::BackingRange(storage, bytes, owner) ||
        native_gpu_memory::Overlaps(owner, dev) || native_gpu_memory::Overlaps(owner, object) ||
        commands.first < owner.first || commands.end > owner.end)
      return false;
    // A newly grown recording has its last published word at storage - 4.
    // Check the reserved words, allowing that cursor at a backing-view edge.
    if (!native_gpu_memory::BackingRange(published + 4, uint64_t(capacity) - published - 4, ring))
      return false;
  } else if (!native_gpu_memory::BackingRange(published, uint64_t(capacity) - published, ring)) {
    return false;
  }
  if (commands.first < ring.first || commands.end > ring.end)
    return false;
  return true;
}

inline uint32_t DirectRingDrawHeader(uint8_t* base, uint32_t device, uint32_t destination,
                                     uint32_t legacy) {
  if (!direct_draw_headers_enabled)
    return legacy;
  uint32_t header;
  if (!rex::graphics::native_gpu_direct::EncodeLegacyHeader(legacy, header) ||
      !OwnsDirectRingSpan(base, device, destination, ((legacy >> 16) & 0x3FFF) + 2))
    return legacy;
  return header;
}

// These seven writer sites emit the scalar vertex-index offset separately
// from their draw. Retain that order and operand; never absorb it into a
// predicated draw or let a skipped draw suppress the unconditional state write.
inline uint32_t DirectRingStateHeader(uint8_t* base, uint32_t device, uint32_t destination,
                                      uint32_t legacy) {
  uint32_t header;
  if (!direct_state_headers_enabled || legacy != 0x2102 ||
      !rex::graphics::native_gpu_state::EncodeHeader(0x2102, 1, header) ||
      !OwnsDirectRingSpan(base, device, destination, 2))
    return legacy;
  return header;
}

}  // namespace simpsons::native_gpu_calls
