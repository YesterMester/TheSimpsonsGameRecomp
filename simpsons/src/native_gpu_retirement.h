#pragma once

#include <array>
#include <bit>
#include <rex/graphics/native_gpu_calls.h>
#include <rex/graphics/native_gpu_state.h>
#include <rex/graphics/native_gpu_sync.h>
#include <rex/ppc/context.h>

#include "guest_memory.h"
#include "native_gpu_memory.h"

namespace simpsons::native_gpu_calls {

// Called after the original ring wait and before its lock/submission. Only
// command/data construction is replaced: scheduling and synchronization stay
// in the original function, including retries and all real helper calls.
inline bool WriteRetirementChunk(PPCContext& ctx, uint8_t* base) {
  using namespace native_gpu_memory;
  const uint32_t block = ctx.r31.u32, descriptor = ctx.r29.u32;
  MemoryRange output, desc, owner, stack, global, root;
  if (!base || (block & 3) || (descriptor & 3) || (ctx.r22.u32 & 3) || (ctx.r1.u32 & 15) ||
      ctx.r30.u64 || ctx.r25.u64 != 0x41000000 || ctx.r23.u64 != 0xA31 ||
      ctx.r24.s64 != -2113929216 || !BackingRange(block, 384, output) ||
      !BackingRange(descriptor, 40, desc) || !BackingRange(ctx.r22.u32, 40, owner) ||
      !BackingRange(ctx.r1.u32, 240, stack) || !BackingRange(0x82000710, 4, global) ||
      Overlaps(output, desc) || Overlaps(output, owner) || Overlaps(output, stack) ||
      Overlaps(output, global) || Overlaps(stack, desc) || Overlaps(stack, owner) ||
      Overlaps(stack, global))
    return false;
  const uint32_t global_root = LoadGuestU32(base, 0x82000710);
  if ((global_root & 3) || !BackingRange(global_root, 4, root) || Overlaps(output, root) ||
      Overlaps(stack, root))
    return false;
  const uint32_t device = LoadGuestU32(base, global_root);
  const uint32_t end = LoadGuestU32(base, descriptor + 20);
  const uint32_t data = block + 256 + 0x41000000u;
  const uint32_t coherent_base = data & 0xFFFFF000u;
  const uint64_t coherent_size = uint64_t(end) - coherent_base + 0x41000000ull;
  const uint64_t sequence = (ctx.r26.u32 & 0xFFFFFFFCu) | (ctx.r28.u64 & 0xFFFFFFFF00000003ull);
  const uint64_t fetch =
      (std::rotl(0x2100000021ull, 28) & 0xFFFFFFFFFC000003ull) | (coherent_size & 0x3FFFFFC);
  uint32_t fetch_header, flush_header;
  if (!rex::graphics::native_gpu_state::EncodeHeader(0x48BA, 6, fetch_header) ||
      !rex::graphics::native_gpu_state::EncodeHeader(0x5000, 3, flush_header))
    return false;
  std::array<uint32_t, 28> words{};
  words[0] = fetch_header;
  words[5] = data | 3;
  words[6] = uint32_t(fetch);
  const auto coherency =
      rex::graphics::native_gpu_sync::Encode({0x05000000, uint32_t(coherent_size), coherent_base});
  for (uint32_t i = 0; i < coherency.size(); ++i)
    words[7 + i] = coherency[i];
  words[18] = flush_header;
  words[22] = 0xC0000000u | (rex::graphics::native_gpu_calls::kBareDrawOpcode << 8);
  words[23] = 0x50081;
  words[24] = rex::graphics::native_gpu_sync::kEventHeader;
  words[25] = 27;
  words[26] = ctx.r21.u32;
  words[27] = uint32_t(sequence);
  for (uint32_t i = 0; i < words.size(); ++i)
    StoreGuestU32(base, block + i * 4, words[i]);
  for (uint32_t i = 0; i < 5; ++i)
    StoreGuestU32(base, block + 256 + i * 4, std::bit_cast<uint32_t>(float(i)));
  // Preserve the continuation's complete context, including the private
  // conversion spill and the helper arguments surviving the lock loop.
  StoreGuestU32(base, ctx.r1.u32 + 88, 0);
  StoreGuestU32(base, ctx.r1.u32 + 92, 4);
  ctx.fpscr.disableFlushMode();
  ctx.f0.f64 = 4;
  ctx.cr6.compare<uint32_t>(5, 5, ctx.xer);
  ctx.r3.u64 = device;
  ctx.r4.s64 = int32_t(0xC0025800);
  ctx.r5.u64 = 0x50081;
  ctx.r6.u64 = sequence;
  ctx.r7.u64 = 33;
  ctx.r8.u64 = end;
  ctx.r9.u64 = fetch;
  ctx.r10.u64 = global_root;
  ctx.r11.u64 = ctx.r31.u64 + 112;
  ctx.r27.u64 = 27;
  return true;
}

}  // namespace simpsons::native_gpu_calls
