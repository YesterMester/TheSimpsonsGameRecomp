#pragma once

#include <rex/graphics/native_gpu_sync.h>
#include <rex/ppc/context.h>

#include "guest_memory.h"
#include "native_gpu_memory.h"

namespace simpsons::native_gpu_calls {
namespace counter_event_detail {
using namespace native_gpu_memory;

inline bool Device(const PPCContext& ctx, const uint8_t* base, MemoryRange& device,
                   MemoryRange& stack) {
  return base && !(ctx.r3.u32 & 3) && !(ctx.r1.u32 & 15) && ctx.r1.u32 >= 112 &&
         BackingRange(ctx.r3.u32, 22256, device) && BackingRange(ctx.r1.u32 - 112, 112, stack) &&
         !Overlaps(device, stack);
}

inline uint32_t Physical(uint32_t address) {
  return (address & 0x1FFFFFFFu) + (((address >> 20) + 512) & 0x1000u);
}

inline bool Write(PPCContext& ctx, uint8_t* base, uint32_t target, const MemoryRange& device,
                  const MemoryRange& stack) {
  const uint32_t dev = ctx.r3.u32, cursor = LoadGuestU32(base, dev + 48);
  const uint32_t capacity = LoadGuestU32(base, dev + 52);
  MemoryRange commands, destination;
  if ((*GuestToHost(base, dev + 10940) & 1) || LoadGuestU32(base, dev + 13216) ||
      LoadGuestU32(base, dev + 13492) || ((cursor | capacity) & 3) ||
      cursor > LoadGuestU32(base, dev + 56) || uint64_t(cursor) + 16 > capacity ||
      !BackingRange(cursor + 4, 16, commands) || Overlaps(commands, device) ||
      Overlaps(commands, stack) || uint64_t(target & ~3u) + 4 > 0x20000000ull ||
      !BackingRange(0xC0000000u + (target & ~3u), 4, destination) ||
      Overlaps(destination, commands) || Overlaps(destination, device) ||
      Overlaps(destination, stack))
    return false;
  // Keep the original counter selector, poison value, endian bits and span.
  // The native consumer writes the counter on the command thread, in order.
  StoreGuestU32(base, cursor + 4, rex::graphics::native_gpu_sync::kEventHeader);
  StoreGuestU32(base, cursor + 8, 0x80000003u);
  StoreGuestU32(base, cursor + 12, target);
  StoreGuestU32(base, cursor + 16, 0xDEADBEEFu);
  StoreGuestU32(base, dev + 48, cursor + 16);
  ctx.lr = uint32_t(ctx.lr);
  return true;
}
}  // namespace counter_event_detail

inline bool WriteCounterEvent(PPCContext& ctx, uint8_t* base) {
  using namespace counter_event_detail;
  MemoryRange device, stack;
  return Device(ctx, base, device, stack) && Write(ctx, base, Physical(ctx.r4.u32), device, stack);
}

inline bool WriteCounterSlotEvent(PPCContext& ctx, uint8_t* base) {
  using namespace counter_event_detail;
  MemoryRange device, stack;
  if (!Device(ctx, base, device, stack))
    return false;
  const uint32_t dev = ctx.r3.u32;
  if (LoadGuestU32(base, dev + 21624) != 2) {
    ctx.lr = uint32_t(ctx.lr);
    return true;
  }
  const uint32_t current = LoadGuestU32(base, dev + 21632);
  const uint32_t next = current == 3 ? 0 : current + 1;
  if (next == LoadGuestU32(base, dev + 21628)) {
    ctx.lr = uint32_t(ctx.lr);
    return true;
  }
  const uint32_t slot = next * 2 + uint32_t(ctx.r4.u32 == 0);
  const uint32_t address = LoadGuestU32(base, dev + 21620) + slot * 4;
  return Write(ctx, base, Physical(address) | 2, device, stack);
}

}  // namespace simpsons::native_gpu_calls
