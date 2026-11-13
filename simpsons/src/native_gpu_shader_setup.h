#pragma once

#include <algorithm>
#include <array>
#include <rex/graphics/native_gpu_program.h>
#include <rex/ppc/context.h>

#include "guest_memory.h"
#include "native_gpu_memory.h"

namespace simpsons::native_gpu_calls {

struct ShaderPrefixSpan {
  native_gpu_memory::MemoryRange commands;
  uint32_t header;
};

inline bool PlanShaderPrefix(const uint8_t* base, uint32_t device, uint32_t cursor, uint32_t source,
                             uint32_t words, uint32_t prefix, uint32_t stack_pointer,
                             uint32_t frame, ShaderPrefixSpan& span) {
  using namespace native_gpu_memory;
  MemoryRange dev, stack, code, commands;
  uint32_t header;
  if (!base || (device & 3) || !cursor || (cursor & 3) || (source & 3) || (stack_pointer & 15) ||
      stack_pointer < 8 || !rex::graphics::native_gpu_program::EncodeShaderHeader(words, header) ||
      !BackingRange(device, 22256, dev) ||
      !BackingRange(stack_pointer - 8, uint64_t(frame) + 8, stack) || Overlaps(dev, stack) ||
      uint64_t(cursor) + uint64_t(words + prefix) * 4 > LoadGuestU32(base, device + 52) ||
      !BackingRange(cursor + 4, uint64_t(words + prefix) * 4, commands) ||
      !BackingRange(source, uint64_t(words) * 4, code) || Overlaps(commands, dev) ||
      Overlaps(commands, stack) || Overlaps(commands, code) || Overlaps(code, stack))
    return false;
  span = {commands, header};
  return true;
}

inline void StoreProgramPrefix(uint8_t* base, uint32_t cursor, uint32_t header, uint32_t words,
                               uint32_t stage, uint32_t invalidate) {
  if (invalidate) {
    StoreGuestU32(base, cursor += 4, rex::graphics::native_gpu_program::kInvalidateHeader);
    StoreGuestU32(base, cursor += 4, invalidate);
  }
  StoreGuestU32(base, cursor += 4, header);
  StoreGuestU32(base, cursor += 4, stage);
  StoreGuestU32(base, cursor + 4, words);
}

inline bool WriteResourceShaderPrefix(PPCContext& ctx, uint8_t* base) {
  using namespace native_gpu_memory;
  const uint32_t resource = ctx.r31.u32, cursor = ctx.r3.u32;
  if (!base || (resource & 3) || (ctx.r30.u32 & 7) || ctx.r30.u32 > UINT32_MAX - 4 ||
      ctx.r28.u64 > 16382 || !ctx.r28.u32 || ctx.r27.u64 != ctx.r28.u64 * 4 ||
      uint64_t(resource) + ctx.r30.u32 + 4 > 0x100000000ull)
    return false;
  MemoryRange header, selected;
  const uint32_t header_bytes = std::max(36u, ctx.r30.u32 + 4);
  if (!BackingRange(resource, header_bytes, header))
    return false;
  const uint32_t relative = LoadGuestU32(base, resource + ctx.r30.u32);
  if (uint64_t(resource) + relative + 880 > 0x100000000ull ||
      !BackingRange(resource + relative, 880, selected))
    return false;
  const uint64_t raw_source =
      uint64_t(LoadGuestU32(base, resource + 32)) + LoadGuestU32(base, resource + relative + 872);
  if (raw_source > UINT32_MAX)
    return false;
  const uint32_t physical =
      (uint32_t(raw_source) & 0x1FFFFFFFu) + (((uint32_t(raw_source) >> 20) + 512) & 0x1000u);
  if (uint64_t(physical) + ctx.r27.u32 > 0x20000000)
    return false;
  ShaderPrefixSpan span;
  if (!PlanShaderPrefix(base, ctx.r29.u32, cursor, 0xC0000000u + physical, ctx.r28.u32, 5,
                        ctx.r1.u32, 224, span) ||
      Overlaps(span.commands, header) || Overlaps(span.commands, selected))
    return false;
  StoreProgramPrefix(base, cursor, span.header, ctx.r28.u32, 0, 0x100);
  ctx.r3.u32 = cursor + 20;
  ctx.r7.u64 = 0;
  ctx.r8.u64 = 0xFFFFFFFFC0002B00ull | ((ctx.r28.u64 + 1) << 16);
  ctx.r9.u64 = ctx.r28.u64 + 1;
  ctx.r10.u64 = 0x100;
  ctx.r11.u64 = ctx.r28.u32;
  return true;
}

inline bool WriteCopyVertexShaderPrefix(PPCContext& ctx, uint8_t* base) {
  if (ctx.r29.u64 > 16382)
    return false;
  const uint32_t words = ctx.r29.u32, cursor = ctx.r3.u32;
  ShaderPrefixSpan span;
  if (!PlanShaderPrefix(base, ctx.r31.u32, cursor, ctx.r27.u32, words, 5, ctx.r1.u32, 192, span))
    return false;
  StoreProgramPrefix(base, cursor, span.header, words, 0, 0x300);
  ctx.r3.u32 = cursor + 20;
  ctx.r4.u64 = ctx.r27.u64;
  ctx.r5.u64 = words * 4;
  ctx.r7.u64 = 0;
  ctx.r8.u64 = 0xFFFFFFFFC0002B00ull | (uint64_t(words + 1) << 16);
  ctx.r9.u64 = words;
  ctx.r10.u64 = 0x300;
  ctx.r11.s64 = int32_t(0xC0003B00);
  ctx.r22.u64 = 0;
  ctx.r29.u64 = words * 4;
  return true;
}

inline bool WriteCopyPixelShaderPrefix(PPCContext& ctx, uint8_t* base) {
  const uint32_t cursor = ctx.r3.u32;
  if (uint64_t(ctx.r26.u32) + 2160 > 0x100000000ull)
    return false;
  ShaderPrefixSpan span;
  if (!PlanShaderPrefix(base, ctx.r31.u32, cursor, ctx.r26.u32 + 2100, 15, 3, ctx.r1.u32, 192,
                        span))
    return false;
  StoreProgramPrefix(base, cursor, span.header, 15, 1, 0);
  ctx.r3.u32 = cursor + 12;
  ctx.r4.u64 = ctx.r26.u64 + 2100;
  ctx.r5.u64 = 60;
  ctx.r9.u64 = 15;
  ctx.r10.u64 = 1;
  ctx.r11.s64 = int32_t(0xC0102B00);
  return true;
}

}  // namespace simpsons::native_gpu_calls
