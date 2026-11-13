#pragma once

#include <array>
#include <bit>
#include <initializer_list>
#include <rex/graphics/native_gpu_ports.h>
#include <rex/graphics/native_gpu_program.h>
#include <rex/graphics/native_gpu_state.h>
#include <rex/ppc/context.h>

#include "guest_memory.h"
#include "native_gpu_memory.h"

namespace simpsons::native_gpu_calls {

// Setup copies the original adjacent VS/PS code blocks into its caller's
// reserved buffer. It neither allocates nor changes the device's shadows.
inline bool WriteInitializationSetup(PPCContext& ctx, uint8_t* base) {
  using namespace native_gpu_memory;
  constexpr uint32_t kShader = 0x8206A0C8, kWords = 74;
  const uint32_t output = ctx.r4.u32;
  MemoryRange commands, source, stack;
  if (!base || (output & 3) || (ctx.r1.u32 & 15) || ctx.r1.u32 < 200 ||
      !BackingRange(output, kWords * 4, commands) || !BackingRange(kShader, 132, source) ||
      !BackingRange(ctx.r1.u32 - 200, 200, stack) || Overlaps(commands, source) ||
      Overlaps(commands, stack) || Overlaps(source, stack))
    return false;
  std::array<uint32_t, kWords> words;
  uint32_t at = 0;
  const auto shader = [&](uint32_t stage, uint32_t offset, uint32_t count) {
    uint32_t header;
    if (!rex::graphics::native_gpu_program::EncodeShaderHeader(count, header))
      return false;
    words[at++] = header;
    words[at++] = stage;
    words[at++] = count;
    for (uint32_t i = 0; i < count; ++i)
      words[at++] = LoadGuestU32(base, kShader + offset + i * 4);
    return true;
  };
  const auto state = [&](uint32_t first, std::initializer_list<uint32_t> values) {
    uint32_t header;
    if (!rex::graphics::native_gpu_state::EncodeHeader(first, uint32_t(values.size()), header))
      return false;
    words[at++] = header;
    for (uint32_t value : values)
      words[at++] = value;
    return true;
  };
  words[at++] = rex::graphics::native_gpu_program::kInvalidateHeader;
  words[at++] = 0x300;
  if (!shader(0, 0, 24) || !shader(1, 96, 9) || !state(0x2180, {0x1000000E, 0}) ||
      !state(0x2100, {0xFFFF, 0, 0}) || !state(0x2293, {0}) ||
      !state(0x2204, {0x10000, 0x10000, 0x300}) || !state(0x2312, {0xFFFF}) ||
      !state(0x200D, {0}) || !state(0x2200, {0}) || !state(0x2203, {0}) || !state(0x2208, {4}) ||
      !state(0x2104, {0}) || !state(0x2280, {0x80008}) || !state(0x2302, {4}) ||
      !state(0x2080, {0, 0, 0x100010}) || at != kWords)
    return false;
  for (uint32_t i = 0; i < kWords; ++i)
    StoreGuestU32(base, output + i * 4, words[i]);
  ctx.r3.u64 = kWords;
  ctx.lr = uint32_t(ctx.lr);
  return true;
}

inline bool WriteInitializationGpr(PPCContext& ctx, uint8_t* base) {
  native_gpu_memory::MemoryRange commands;
  const uint32_t output = ctx.r5.u32, index = ctx.r4.u32;
  if (!base || (output & 3) || !native_gpu_memory::BackingRange(output, 52, commands))
    return false;
  const uint32_t bits = ((0u - index) & ~0x7F00u) | ((index << 8) & 0x7F00);
  const uint32_t value = (std::rotl(bits, 4) & 0x7FFF0) & ~0x800u;
  using namespace rex::graphics::native_gpu_ports;
  const std::array<uint32_t, 13> words = {
      kWaitHeader, 0x20000,   kGprEightHeader, value | 1, value | 1,  value | 1, value | 1,
      value | 1,   value | 1, value | 1,       value | 1, kGprHeader, value};
  for (uint32_t i = 0; i < words.size(); ++i)
    StoreGuestU32(base, output + i * 4, words[i]);
  ctx.r3.u64 = 13;
  return true;
}

}  // namespace simpsons::native_gpu_calls
