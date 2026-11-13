#pragma once

#include <rex/ppc/context.h>

#include "guest_memory.h"
#include "native_gpu_memory.h"
#include <rex/graphics/native_gpu_calls.h>

namespace simpsons::native_gpu_calls {

// The initialization helper emits 24 identical, unpredicated one-vertex
// draws. It neither submits them nor changes any device state.
inline bool WriteWarmupDraws(PPCContext& ctx, uint8_t* base) {
  native_gpu_memory::MemoryRange output;
  const uint32_t write = ctx.r4.u32;
  if (!base || (write & 3) || !native_gpu_memory::BackingRange(write, 192, output))
    return false;
  for (uint32_t i = 0; i < 24; ++i) {
    StoreGuestU32(base, write + i * 8,
                  0xC0000000u | (rex::graphics::native_gpu_calls::kBareDrawOpcode << 8));
    StoreGuestU32(base, write + i * 8 + 4, 0x10081);
  }
  ctx.r3.u64 = 48;
  return true;
}

// A prepared triangle-list draw, predicated as a whole. Unlike BeginVertices,
// there are no unconditional state writes to apply when the bin rejects it.
inline bool WriteBareTriangleDraw(PPCContext& ctx, uint8_t* base) {
  using namespace native_gpu_memory;
  const uint32_t device = ctx.r3.u32;
  MemoryRange dev, stack, commands;
  if (!base || (device & 3) || (ctx.r1.u32 & 15) || ctx.r1.u32 < 112 ||
      !BackingRange(device, 60, dev) || !BackingRange(ctx.r1.u32 - 112, 112, stack) ||
      Overlaps(dev, stack))
    return false;
  const uint32_t cursor = LoadGuestU32(base, device + 48);
  if ((cursor & 3) || cursor > LoadGuestU32(base, device + 56) ||
      uint64_t(cursor) + 8 > LoadGuestU32(base, device + 52) ||
      !BackingRange(cursor + 4, 8, commands) || Overlaps(commands, dev) ||
      Overlaps(commands, stack))
    return false;
  StoreGuestU32(base, cursor + 4,
                0xC0000001u | (rex::graphics::native_gpu_calls::kBareDrawOpcode << 8));
  StoreGuestU32(base, cursor + 8, (ctx.r4.u32 << 16) | 0x86);
  StoreGuestU32(base, device + 48, cursor + 8);
  ctx.r3.u64 = cursor + 8;
  ctx.lr = uint32_t(ctx.lr);
  return true;
}

}  // namespace simpsons::native_gpu_calls
