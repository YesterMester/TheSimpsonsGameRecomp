#pragma once

#include "native_gpu_flush.h"

namespace simpsons::native_gpu_vertices {

// The vertex-ID families use the same shadow flush as ordinary ranges, but
// first select tessellation/vertex-ID words from the game's own lookup tables.
// All table reads, flushes and output ranges are checked before those writes.
inline bool DrawVertexIdRange(PPCContext& ctx, uint8_t* base, bool indexed, bool samplers = false,
                              bool conditional = false) {
  using namespace native_gpu_memory;
  const uint32_t device = ctx.r3.u32, primitive = ctx.r4.u32;
  const uint32_t count = indexed ? ctx.r7.u32 : ctx.r6.u32;
  const uint32_t frame = indexed ? 240 : 208, bytes = indexed ? 28 : 20;
  const uint32_t flush_frame = conditional ? 192 : 176;
  MemoryRange dev, stack, commands, metadata;
  if (!base || (device & 3) || (ctx.r1.u32 & 15) || ctx.r1.u32 < frame + flush_frame ||
      primitive > 31 || count > 65535 || !BackingRange(device, 14916, dev) ||
      !BackingRange(ctx.r1.u32 - frame - flush_frame, frame + flush_frame, stack) ||
      Overlaps(dev, stack) || (*GuestToHost(base, device + 10940) & 1))
    return false;
  uint32_t format = 0, index_address = 0;
  if (indexed) {
    const uint32_t pointer = LoadGuestU32(base, device + 12428);
    if ((pointer & 3) || !BackingRange(pointer, 28, metadata) || Overlaps(metadata, dev) ||
        Overlaps(metadata, stack))
      return false;
    format = LoadGuestU32(base, pointer);
    index_address = LoadGuestU32(base, pointer + 24);
  }
  const bool expanded = indexed && (LoadGuestU32(base, device + 10616) & 3) == 2;
  if (expanded && primitive < 16)
    return false;
  const uint32_t lookup = expanded ? primitive - 16 : primitive;
  const uint32_t first = (expanded ? 0x8206A44Cu : 0x8206A368u) + lookup * 12;
  const uint32_t second = (expanded                             ? 0x8206A5A0u
                           : !indexed || (format & 0x80000000u) ? 0x8206A508u
                                                                : 0x8206A470u) +
                          lookup * 8;
  MemoryRange table_a, table_b;
  if (!BackingRange(first, 12, table_a) || !BackingRange(second, 8, table_b) ||
      Overlaps(table_a, dev) || Overlaps(table_b, dev) || Overlaps(table_a, stack) ||
      Overlaps(table_b, stack))
    return false;
  std::array<uint32_t, 7> shadow;
  for (uint32_t i = 0; i < 3; ++i)
    shadow[i] = LoadGuestU32(base, first + i * 4);
  shadow[3] = LoadGuestU32(base, second);
  shadow[4] = LoadGuestU32(base, second + 4);
  shadow[5] = expanded                             ? 0x61412107u
              : !indexed || (format & 0x80000000u) ? 0x61412101u
                                                   : 0x30201000u;
  shadow[6] = expanded ? 0x66462606u : shadow[5];
  if (expanded && uint64_t(count) * shadow[2] > 65535)
    return false;

  auto dirty = native_gpu_flush::DirtyState(base, device);
  const uint64_t selected = dirty[3] | (uint64_t(1) << 50);
  dirty[3] = selected | (uint64_t(127) << 39);
  native_gpu_flush::FlushPlan flush;
  if (!native_gpu_flush::PlanFlush(ctx, base, frame, dirty, flush, samplers, conditional))
    return false;
  const uint32_t cursor = flush.cursor;
  if (cursor > LoadGuestU32(base, device + 56) ||
      uint64_t(cursor) + bytes > LoadGuestU32(base, device + 52) ||
      !BackingRange(flush.initial + 4, uint64_t(cursor) + bytes - flush.initial, commands) ||
      Overlaps(commands, dev) || Overlaps(commands, stack) || Overlaps(commands, table_a) ||
      Overlaps(commands, table_b) || (indexed && Overlaps(commands, metadata)))
    return false;
  const uint32_t mode = *GuestToHost(base, device + 12179);
  rex::graphics::native_gpu_calls::RangeDrawCall call{
      ctx.r5.u32, uint32_t(*GuestToHost(base, device + 12114 + mode) & 0x3F) | (mode ? 0x100u : 0u),
      (primitive & 63) | (count << 16) | 0x180};
  if (indexed) {
    const bool index32 = expanded || (format & 0x80000000u);
    const uint32_t indices = expanded ? count * shadow[2] : count;
    const uint32_t address = index_address + ctx.r6.u32 * (index32 ? 4 : 2);
    call.initiator = (primitive & 63) | (indices << 16) | 0x100 | (index32 ? 0x800u : 0u);
    call.index_base = (address & 0x1FFFFFFFu) + (((address >> 20) + 512) & 0x1000u);
    call.index_size =
        (index32 ? ((indices << 1) & 0xFFFFFEu) : indices) | ((format << 1) & 0xC0000000u);
  }
  std::array<uint32_t, 7> words;
  if (!rex::graphics::native_gpu_calls::EncodeRange(call, {words.data(), bytes / 4}))
    return false;

  StoreGuestU32(base, device + 10612, (LoadGuestU32(base, device + 10612) & ~3u) | 1);
  Store64(base, device + 24, selected);
  for (uint32_t i : {0u, 1u, 2u, 3u, 5u, 6u, 4u})
    StoreGuestU32(base, device + 10632 + i * 4, shadow[i]);
  Store64(base, device + 24, dirty[3]);
  native_gpu_flush::ApplyFlush(flush, base);
  for (uint32_t i = 0; i < bytes / 4; ++i)
    StoreGuestU32(base, cursor + 4 + i * 4, words[i]);
  StoreGuestU32(base, device + 48, cursor + bytes);
  ctx.r3.u64 = cursor + 4;
  ctx.lr = uint32_t(ctx.lr);
  return true;
}

}  // namespace simpsons::native_gpu_vertices
