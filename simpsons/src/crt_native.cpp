// Native memset and memcpy for the game's C runtime.
//
// The game's own memset (sub_82A3C440) and memcpy (sub_82A3CD80, and the
// simpler sub_82A3D1D8) run as recompiled loops of byte-swapped 4-byte
// stores and loads, which made them a few percent of the main thread. A byte
// fill or copy is the same in either byte order, so these do the work with
// the host's routines. All three take r3 = destination, r4 = fill byte or
// source, r5 = size, and return the destination in r3; sub_82A3CD80 also
// leaves it at r1 - 8, below the stack pointer.
//
// The game's copies run forward. For a destination above an overlapping
// source that repeats the copied bytes, which the host's memcpy and memmove
// don't, so such a copy keeps the original routine.

#include "guest_memory.h"

#include <cstdint>
#include <cstring>

#include <rex/ppc.h>

REX_EXTERN(__imp__sub_82A3C440);
REX_EXTERN(sub_82A3C440);
REX_EXTERN(__imp__sub_82A3CD80);
REX_EXTERN(sub_82A3CD80);
REX_EXTERN(__imp__sub_82A3D1D8);
REX_EXTERN(sub_82A3D1D8);

namespace {

using simpsons::GuestToHost;

// A forward copy of size bytes from source to destination gives the same
// result as memmove unless the destination starts inside the source.
bool ForwardCopyIsPlain(uint32_t destination, uint32_t source, uint32_t size) {
  return destination <= source || destination - source >= size;
}

// Whether [address, address + size) stays within one side of the physical
// address boundary, so that one host pointer covers it on every platform.
bool OneHostRange(uint32_t address, uint32_t size) {
  const uint64_t end = uint64_t(address) + size;
  return end <= 0x100000000ull && (address >= 0xE0000000u || end <= 0xE0000000ull);
}

}  // namespace

REX_FUNC(sub_82A3C440) {
  const uint32_t destination = ctx.r3.u32;
  const uint32_t size = ctx.r5.u32;
  if (!OneHostRange(destination, size)) {
    __imp__sub_82A3C440(ctx, base);
    return;
  }
  std::memset(GuestToHost(base, destination), int(ctx.r4.u32 & 0xFF), size);
}

REX_FUNC(sub_82A3CD80) {
  const uint32_t destination = ctx.r3.u32;
  const uint32_t source = ctx.r4.u32;
  const uint32_t size = ctx.r5.u32;
  if (!ForwardCopyIsPlain(destination, source, size) || !OneHostRange(destination, size) ||
      !OneHostRange(source, size)) {
    __imp__sub_82A3CD80(ctx, base);
    return;
  }
  // std r3,-8(r1)
  const uint64_t saved = std::byteswap(uint64_t(destination));
  std::memcpy(GuestToHost(base, ctx.r1.u32 - 8), &saved, sizeof(saved));
  std::memmove(GuestToHost(base, destination), GuestToHost(base, source), size);
}

REX_FUNC(sub_82A3D1D8) {
  const uint32_t destination = ctx.r3.u32;
  const uint32_t source = ctx.r4.u32;
  const uint32_t size = ctx.r5.u32;
  if (!ForwardCopyIsPlain(destination, source, size) || !OneHostRange(destination, size) ||
      !OneHostRange(source, size)) {
    __imp__sub_82A3D1D8(ctx, base);
    return;
  }
  std::memmove(GuestToHost(base, destination), GuestToHost(base, source), size);
}
