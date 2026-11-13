#pragma once

#include <array>
#include <rex/graphics/native_gpu_calls.h>
#include <rex/graphics/native_gpu_state.h>
#include <rex/ppc/context.h>

#include "guest_memory.h"
#include "native_gpu_memory.h"

namespace simpsons::native_gpu_calls {

// These checkpoints run after the real geometry/allocation or ring-growth
// steps. A rejected span resumes the original writer at the same checkpoint;
// no allocation, flush or outer iteration is repeated.
inline bool WriteClearResolveDraw(PPCContext& ctx, uint8_t* base) {
  using namespace native_gpu_memory;
  const uint32_t device = ctx.r31.u32, cursor = ctx.r30.u32;
  MemoryRange dev, stack, commands, vertices;
  if (!base || (device & 3) || (cursor & 3) || (ctx.r1.u32 & 15) ||
      !BackingRange(device, 22256, dev) || !BackingRange(ctx.r1.u32, 384, stack) ||
      Overlaps(dev, stack) || ctx.r10.u64 != 0x54800 || ctx.r7.u64 != 0x1000001A ||
      ctx.r26.u64 != 0x25000 || ctx.r4.u64 || ctx.r5.u64 || ctx.r6.u64 || ctx.r8.u64 ||
      ctx.r29.u64 || ctx.r27.u64 || ctx.r25.u64 || cursor > LoadGuestU32(base, device + 56) ||
      uint64_t(cursor) + 52 > LoadGuestU32(base, device + 52) ||
      !BackingRange(cursor + 4, 52, commands) || !BackingRange(ctx.r3.u32, 24, vertices) ||
      Overlaps(commands, dev) || Overlaps(commands, stack) || Overlaps(commands, vertices))
    return false;
  const uint32_t flags = LoadGuestU32(base, device + 12708);
  if (flags & ~1u)
    return false;
  uint32_t fetch, flush;
  if (!rex::graphics::native_gpu_state::EncodeHeader(0x4800, 6, fetch) ||
      !rex::graphics::native_gpu_state::EncodeHeader(0x5000, 3, flush))
    return false;
  const std::array<uint32_t, 13> words = {
      fetch,      ctx.r9.u32,
      0x1000001A, 0,
      0,          0,
      0,          flush,
      0,          0,
      0,          0xC0000000u | (rex::graphics::native_gpu_calls::kBareDrawOpcode << 8) | flags,
      0x30088};
  for (uint32_t i = 0; i < words.size(); ++i)
    StoreGuestU32(base, cursor + 4 + i * 4, words[i]);
  ctx.r30.u32 = cursor + 4;
  ctx.r11.u64 = ctx.r30.u64;
  ctx.r11.u32 = cursor + 52;
  ctx.r9.u64 = 0x30088;
  // The old header value is a volatile continuation register, never emitted.
  ctx.r10.u64 = 0xC0003600u | flags;
  return true;
}

inline bool WriteCopyResolveDraw(PPCContext& ctx, uint8_t* base) {
  using namespace native_gpu_memory;
  const uint32_t device = ctx.r31.u32, cursor = ctx.r11.u32, remaining = ctx.r29.u32;
  MemoryRange dev, stack, commands;
  if (!base || (device & 3) || (cursor & 3) || (ctx.r1.u32 & 15) ||
      !BackingRange(device, 22256, dev) || !BackingRange(ctx.r1.u32, 192, stack) ||
      Overlaps(dev, stack) || cursor > LoadGuestU32(base, device + 56) ||
      uint64_t(cursor) + 16 > LoadGuestU32(base, device + 52) ||
      !BackingRange(cursor + 4, 16, commands) || Overlaps(commands, dev) ||
      Overlaps(commands, stack))
    return false;
  const uint64_t count = remaining > 65535 ? 65534 : ctx.r29.u64;
  uint32_t offset;
  if (!rex::graphics::native_gpu_state::EncodeHeader(0x2102, 1, offset))
    return false;
  const uint32_t initiator = (uint32_t(count) << 16) | 0x81;
  const std::array<uint32_t, 4> words = {
      offset, ctx.r28.u32, 0xC0000001u | (rex::graphics::native_gpu_calls::kBareDrawOpcode << 8),
      initiator};
  for (uint32_t i = 0; i < words.size(); ++i)
    StoreGuestU32(base, cursor + 4 + i * 4, words[i]);
  ctx.cr6.compare<uint32_t>(remaining, 65535, ctx.xer);
  ctx.r10.u64 = count;
  ctx.r29.u64 -= count;
  ctx.cr0.compare<int32_t>(ctx.r29.s32, 0, ctx.xer);
  ctx.r9.s64 = int32_t(0xC0003601);
  ctx.r8.u64 = initiator;
  ctx.r11.u32 = cursor + 16;
  StoreGuestU32(base, device + 48, ctx.r11.u32);
  return true;
}

}  // namespace simpsons::native_gpu_calls
