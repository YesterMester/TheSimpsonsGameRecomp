#pragma once

#include <array>
#include <rex/graphics/native_gpu_indirect.h>
#include <rex/ppc/context.h>

#include "guest_memory.h"
#include "native_gpu_memory.h"

namespace simpsons::native_gpu_calls {

// Shader-resource descriptors have already supplied the source, vector index
// and word count. Keep the real outer descriptor loop and ring-growth call.
inline bool WriteIndirectConstants(PPCContext& ctx, uint8_t* base, bool typed = false) {
  using namespace native_gpu_memory;
  const uint32_t device = ctx.r29.u32, cursor = ctx.r3.u32, source = ctx.r30.u32;
  const uint32_t physical = (source & 0x1FFFFFFFu) + (((source >> 20) + 512) & 0x1000u);
  if (ctx.r26.u64 > (typed ? 65535u : 511u) || ctx.r28.u64 > (typed ? 65535u : 2048u))
    return false;
  const rex::graphics::native_gpu_indirect::Call call{physical, ctx.r26.u32 * 4, ctx.r28.u32};
  rex::graphics::native_gpu_indirect::TypedRange range{physical, 0x4000 + call.index, call.count};
  if (typed ? !rex::graphics::native_gpu_indirect::DecodeTyped(
                  {call.address, call.index, call.count}, range)
            : !rex::graphics::native_gpu_indirect::Validate(call))
    return false;
  MemoryRange dev, stack, commands, data{}, gpu_data{}, descriptors;
  if (!base || (device & 3) || (cursor & 3) || (ctx.r1.u32 & 15) || ctx.r31.u32 < 8 ||
      ctx.r27.u32 < ctx.r31.u32 || !BackingRange(device, 22256, dev) ||
      !BackingRange(ctx.r1.u32, 144, stack) || Overlaps(dev, stack) ||
      cursor > LoadGuestU32(base, device + 56) ||
      uint64_t(cursor) + 16 > LoadGuestU32(base, device + 52) ||
      !BackingRange(cursor + 4, 16, commands) ||
      (range.count && (!BackingRange(source, uint64_t(range.count) * 4, data) ||
                       !BackingRange(0xA0000000u + physical, uint64_t(range.count) * 4, gpu_data) ||
                       data.first != gpu_data.first || data.end != gpu_data.end)) ||
      !BackingRange(ctx.r31.u32 - 8, uint64_t(ctx.r27.u32) - ctx.r31.u32 + 8, descriptors) ||
      Overlaps(commands, dev) || Overlaps(commands, stack) || Overlaps(commands, data) ||
      Overlaps(commands, descriptors) || Overlaps(data, stack))
    return false;
  const std::array<uint32_t, 4> words = {typed ? rex::graphics::native_gpu_indirect::kTypedHeader
                                               : rex::graphics::native_gpu_indirect::kHeader,
                                         call.address, call.index, call.count};
  for (uint32_t i = 0; i < words.size(); ++i)
    StoreGuestU32(base, cursor + 4 + i * 4, words[i]);
  ctx.r3.u32 = cursor + 16;
  ctx.r9.s64 = int32_t(0xC0022F00);
  ctx.r10.u64 = source & 0x1FFFFFFFu;
  ctx.r11.u64 = physical;
  ctx.r8.u64 = call.index;
  StoreGuestU32(base, device + 48, ctx.r3.u32);
  return true;
}

}  // namespace simpsons::native_gpu_calls
