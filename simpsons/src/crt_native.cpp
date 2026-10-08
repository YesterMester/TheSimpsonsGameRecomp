// Native memset and memcpy for the game's C runtime.
//
// The game's own memset (sub_82A3C440) and copies run as recompiled loops of
// byte-swapped loads and stores, which made them several percent of the main
// thread. A byte fill or copy is the same in either byte order, so these do
// the work with the host's routines. All take r3 = destination, r4 = fill byte
// or source, r5 = size, and leave the destination in r3:
// - sub_82A3CD80, memcpy, which also leaves it at r1 - 8, below the stack
//   pointer, and sub_82A3D1D8, a simpler one;
// - sub_8270A590, a copy of whole 64-byte blocks (the shader constants the
//   renderer writes into command buffers);
// - sub_82B74110, the XDK's copy for write-combined memory (vector stores,
//   cache lines zeroed ahead of being overwritten), whose tail copies bytes
//   while subtracting a global that is 1.
//
// The game's copies run forward. For a destination above an overlapping
// source that repeats the copied bytes, which the host's memcpy and memmove
// don't, so such a copy keeps the original routine, as does anything else
// outside the cases above.

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
REX_EXTERN(__imp__sub_8270A590);
REX_EXTERN(sub_8270A590);
REX_EXTERN(__imp__sub_82B74110);
REX_EXTERN(sub_82B74110);

namespace {

using simpsons::GuestToHost;
using simpsons::LoadGuestU32;

// The step sub_82B74110's byte loop subtracts from the count.
constexpr uint32_t kCopyByteStep = 0x82CFE4C0;

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

REX_FUNC(sub_8270A590) {
  const uint32_t destination = ctx.r3.u32;
  const uint32_t source = ctx.r4.u32;
  const uint32_t size = ctx.r5.u32;
  if ((size & 63) || !ForwardCopyIsPlain(destination, source, size) ||
      !OneHostRange(destination, size) || !OneHostRange(source, size)) {
    __imp__sub_8270A590(ctx, base);
    return;
  }
  std::memmove(GuestToHost(base, destination), GuestToHost(base, source), size);
}

REX_FUNC(sub_82B74110) {
  const uint32_t destination = ctx.r3.u32;
  const uint32_t source = ctx.r4.u32;
  const uint32_t size = ctx.r5.u32;
  if (LoadGuestU32(base, kCopyByteStep) != 1 ||
      !ForwardCopyIsPlain(destination, source, size) || !OneHostRange(destination, size) ||
      !OneHostRange(source, size)) {
    __imp__sub_82B74110(ctx, base);
    return;
  }
  std::memmove(GuestToHost(base, destination), GuestToHost(base, source), size);
}
