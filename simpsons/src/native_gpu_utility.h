#pragma once

#include <array>
#include <initializer_list>
#include <rex/graphics/native_gpu_calls.h>
#include <rex/graphics/native_gpu_coherency.h>
#include <rex/graphics/native_gpu_program.h>
#include <rex/graphics/native_gpu_state.h>
#include <rex/ppc/context.h>

#include "guest_memory.h"
#include "native_gpu_memory.h"

namespace simpsons::native_gpu_calls {

// The utility shader is copied into the ordered buffer before the copy draw.
// All reservations, aliases and growth boundaries are checked before writing;
// the unsupported path can safely fall back without publishing half a call.
inline bool WriteUtilityDraw(PPCContext& ctx, uint8_t* base) {
  using namespace native_gpu_memory;
  constexpr uint32_t kShader = 0x8206BF28;
  const uint32_t device = ctx.r3.u32;
  const bool wait = ctx.r4.u32 != 3;
  const uint32_t words = wait ? 73 : 62;
  MemoryRange dev, stack, shader, commands;
  if (!base || (device & 3) || (ctx.r1.u32 & 15) || ctx.r1.u32 < 200 ||
      !BackingRange(device, 14916, dev) || !BackingRange(ctx.r1.u32 - 200, 200, stack) ||
      !BackingRange(kShader, 60, shader) || Overlaps(dev, stack) || Overlaps(dev, shader) ||
      Overlaps(stack, shader))
    return false;
  const uint32_t initial = LoadGuestU32(base, device + 48);
  const uint64_t end = uint64_t(initial) + words * 4;
  if ((initial & 3) || end > UINT32_MAX || end > LoadGuestU32(base, device + 52) ||
      end - 16 > LoadGuestU32(base, device + 56) ||
      !BackingRange(initial + 4, words * 4, commands) || Overlaps(commands, dev) ||
      Overlaps(commands, stack) || Overlaps(commands, shader))
    return false;

  const auto physical = [](uint32_t address) {
    return (address & 0x1FFFFFFFu) + (((address >> 20) + 512) & 0x1000);
  };
  const uint32_t width = (ctx.r6.u32 + 31) & ~31u;
  const uint32_t destination = wait ? physical(ctx.r5.u32) : 0;
  std::array<uint32_t, 73> encoded{};
  uint32_t at = 0;
  const auto state = [&](uint32_t first, std::initializer_list<uint32_t> values) {
    uint32_t header;
    if (!rex::graphics::native_gpu_state::EncodeHeader(first, uint32_t(values.size()), header))
      return false;
    encoded[at++] = header;
    for (uint32_t value : values)
      encoded[at++] = value;
    return true;
  };
  if (!state(0x2318, {(ctx.r4.u32 << 20) & 0x300000}) || !state(0x2319, {destination}) ||
      !state(0x231A, {((ctx.r7.u32 << 16) & 0x3FFF0000) | (width & 0x3FFF)}) ||
      !state(0x231B, {0x01000302}))
    return false;
  encoded[at++] = rex::graphics::native_gpu_program::kInvalidateHeader;
  encoded[at++] = 0x300;
  uint32_t shader_header;
  if (!rex::graphics::native_gpu_program::EncodeShaderHeader(15, shader_header))
    return false;
  encoded[at++] = shader_header;
  encoded[at++] = 0;
  encoded[at++] = 15;
  for (uint32_t i = 0; i < 15; ++i)
    encoded[at++] = LoadGuestU32(base, kShader + i * 4);
  if (!state(0x2180, {0x10002, 0}) || !state(0x2208, {6}) || !state(0x2200, {0}) ||
      !state(0x2203, {0}) || !state(0x2100, {0xFFFF, 0, 0}) ||
      !state(0x2204, {0x10000, 0x10000, 0x300}) || !state(0x2312, {0xFFFF}) ||
      !state(0x48BA, {3, 0, 0, 0, physical(ctx.r8.u32 & ~3u) | 3, 0x1000001A}))
    return false;
  encoded[at++] = 0xC0000001u | (rex::graphics::native_gpu_calls::kBareDrawOpcode << 8);
  encoded[at++] = 0x30088;
  if (wait) {
    const uint32_t size = ((width * ctx.r7.u32 << 2) + 8191) & 0xFFFFF000;
    const auto coherency = rex::graphics::native_gpu_coherency::Encode(
        {LoadGuestU32(base, device + 10396), 0x03000100, size, destination & 0xFFFFF000});
    for (uint32_t value : coherency)
      encoded[at++] = value;
  } else if (!state(0x2007, {LoadGuestU32(base, device + 10396)})) {
    return false;
  }
  if (!state(0x2208, {4}) || !state(0x2206, {0}) || at != words)
    return false;
  for (uint32_t i = 0; i < words; ++i) {
    StoreGuestU32(base, initial + (i + 1) * 4, encoded[i]);
    // Preserve each cursor publication, including the destination write
    // immediately before the coherency wait. Submission remains ordered.
    if (i == 36 || i == 46 || i == 55 || i == 57 || i == words - 5 || i == words - 1)
      StoreGuestU32(base, device + 48, initial + (i + 1) * 4);
  }
  ctx.r3.u64 = wait ? uint64_t(int64_t(INT32_MIN)) : uint64_t(initial + 228);
  ctx.lr = uint32_t(ctx.lr);
  return true;
}

}  // namespace simpsons::native_gpu_calls
