#pragma once

#include <algorithm>

#include "native_gpu_vertices.h"
#include "native_gpu_flush.h"

namespace simpsons::native_gpu_vertices {

struct IndexedBeginPlan {
  uint32_t device, cursor, vertex_dwords, index_dwords, out_vertex, out_index;
};

inline bool PlanIndexedBegin(const PPCContext& ctx, const uint8_t* base, IndexedBeginPlan& plan,
                             const BeginSubmission* submission = nullptr) {
  using namespace native_gpu_memory;
  const uint32_t device = ctx.r3.u32, count = ctx.r6.u32, stride = ctx.r9.u32;
  const uint32_t indices = ctx.r7.u32;
  MemoryRange dev, stack, commands, out_vertex, out_index;
  if (!base || (device & 3) || (ctx.r1.u32 & 15) || ctx.r1.u32 < 608 || !count || count > 65535 ||
      !indices || indices > 65535 || stride < 4 || stride > 1020 || (stride & 3) ||
      !BackingRange(device, 14916, dev) || !BackingRange(ctx.r1.u32 - 608, 696, stack) ||
      Overlaps(dev, stack))
    return false;
  const uint32_t out_v = LoadGuestU32(base, ctx.r1.u32 + 84), out_i = ctx.r10.u32;
  if ((out_v & 3) || (out_i & 3) || !BackingRange(out_v, 4, out_vertex) ||
      !BackingRange(out_i, 4, out_index) || Overlaps(out_vertex, out_index) ||
      Overlaps(out_vertex, dev) || Overlaps(out_index, dev) || Overlaps(out_vertex, stack) ||
      Overlaps(out_index, stack))
    return false;
  if (!submission)
    for (uint32_t offset : {0u, 8u, 16u, 24u, 32u})
      if (Load64(base, device + offset))
        return false;
  if ((*GuestToHost(base, device + 10940) & 1) || LoadGuestU32(base, device + 13216) ||
      (*GuestToHost(base, device + 11824) != stride / 4 && LoadGuestU32(base, device + 12688)))
    return false;
  const uint32_t cursor = submission ? submission->cursor : LoadGuestU32(base, device + 48);
  const uint32_t initial = submission ? submission->initial : cursor;
  if ((cursor & 3) || (initial & 3) || initial > cursor ||
      cursor > LoadGuestU32(base, device + 56) ||
      uint64_t(cursor) + 72 > LoadGuestU32(base, device + 52) ||
      !BackingRange(initial + 4, uint64_t(cursor) + 72 - initial, commands) ||
      Overlaps(commands, dev) || Overlaps(commands, stack) || Overlaps(commands, out_vertex) ||
      Overlaps(commands, out_index))
    return false;
  const uint32_t bytes = count * stride;
  const uint32_t index_dwords = ctx.r8.u32 & 4 ? indices : (indices + 1) / 2;
  plan = {device, cursor, bytes / 4, index_dwords, out_v, out_i};
  if (*GuestToHost(base, device + 10941) & 0x20)
    return true;
  if (LoadGuestU32(base, device + 13492)) {
    const std::array protected_ranges = {dev, stack, out_vertex, out_index};
    const std::array sizes = {plan.vertex_dwords, plan.index_dwords};
    return PlanRecordedAllocations(base, device, cursor, commands, protected_ranges, sizes);
  }
  // Both retained allocations must take the non-wrapping default path. A
  // budget transition after the first may move the ring or reset this heap.
  // Reject that reset before either allocation, rather than replaying it.
  const uint32_t budget = LoadGuestU32(base, device + 14908) + bytes;
  if (budget > LoadGuestU32(base, device + 14912) &&
      LoadGuestU32(base, device + 14900) != LoadGuestU32(base, device + 14880))
    return false;
  uint64_t first = LoadGuestU32(base, device + 14892);
  if (!first)
    first = uint64_t(LoadGuestU32(base, device + 52)) + 60;
  first = (first + 15) & ~uint64_t(15);
  const uint64_t second = (first + bytes + 15) & ~uint64_t(15);
  const uint64_t end = second + uint64_t(index_dwords) * 4;
  MemoryRange vertices, index_data, sync;
  const uint32_t sync_address = LoadGuestU32(base, device + 10896);
  if (end > UINT32_MAX || end > LoadGuestU32(base, device + 14884) ||
      !BackingRange(uint32_t(first), bytes, vertices) ||
      !BackingRange(uint32_t(second), uint64_t(index_dwords) * 4, index_data) ||
      !BackingRange(sync_address, 8, sync))
    return false;
  const std::array<MemoryRange, 8> ranges = {dev,       stack,    commands,   out_vertex,
                                             out_index, vertices, index_data, sync};
  for (uint32_t i = 0; i < ranges.size(); ++i)
    for (uint32_t j = i + 1; j < ranges.size(); ++j)
      if (Overlaps(ranges[i], ranges[j]))
        return false;
  return ((LoadGuestU32(base, device + 14896) - LoadGuestU32(base, sync_address + 4)) & 3) == 0;
}

template <typename Allocate>
bool BeginIndexedVerticesWithPlan(PPCContext& ctx, uint8_t* base, const IndexedBeginPlan& plan,
                                  Allocate allocate) {
  const uint32_t device = plan.device;
  const uint64_t incoming_lr = ctx.lr;
  PPCContext work = ctx;
  work.r1.u32 -= 240;
  StoreGuestU32(base, work.r1.u32, ctx.r1.u32);
  StoreGuestU32(base, ctx.r1.u32 - 8, uint32_t(incoming_lr));
  work.r31.u64 = ctx.r3.u64;
  work.r20.u64 = ctx.r4.u64;
  work.r19.u64 = ctx.r5.u64;
  work.r23.u64 = ctx.r7.u64;
  work.r16.u64 = ctx.r10.u64;
  work.r18.u64 = plan.vertex_dwords;
  work.r22.u64 = ctx.r8.u32 & 4;
  work.r17.u64 = work.r22.u32 ? ctx.r7.u64 : plan.index_dwords;
  work.r26.u64 = ctx.r8.u64;
  work.r25.u64 = *GuestToHost(base, device + 12520);
  work.r21.u64 = work.r27.u64 = work.r28.u64 = work.r29.u64 = 0;
  work.r30.u64 = plan.cursor;
  work.lr = 0x8244CC3C;
  work.r3.u64 = ctx.r3.u64;
  work.r4.u64 = plan.vertex_dwords;
  work.r5.u64 = 16;
  allocate(work, base);
  work.r25.u64 = work.r3.u64;
  const uint32_t vertex = work.r3.u32;
  work.lr = 0x8244CC50;
  work.r3.u64 = ctx.r3.u64;
  work.r4.u64 = work.r17.u64;
  work.r5.u64 = 16;
  allocate(work, base);
  const uint32_t index = work.r3.u32;
  ctx.lr = uint32_t(incoming_lr);
  if (!vertex || !index) {
    ctx.r3.s64 = int32_t(0x8007000Eu);
    StoreGuestU32(base, device + 48, plan.cursor);
    StoreGuestU32(base, plan.out_index, 0);
    StoreGuestU32(base, plan.out_vertex, 0);
    return true;
  }
  const auto physical = [](uint32_t address) {
    return (address & 0x1FFFFFFFu) + (((address >> 20) + 512) & 0x1000u);
  };
  const uint32_t mode = *GuestToHost(base, device + 12179);
  const bool index32 = ctx.r8.u32 & 4;
  rex::graphics::native_gpu_calls::VerticesDrawCall call{
      ctx.r5.u32,
      {0, 0, 0, 0, physical(vertex) | 3, ((plan.vertex_dwords << 2) & 0x03FFFFFCu) | 0x10000002u},
      {},
      uint32_t(*GuestToHost(base, device + 12114 + mode) & 0x3F) | (mode ? 0x100u : 0u),
      (ctx.r4.u32 & 63) | (ctx.r7.u32 << 16) | (index32 ? 0x800u : 0u),
      physical(index),
      (index32 ? ((ctx.r7.u32 << 1) & 0xFFFFFEu) : ctx.r7.u32 & 0xFFFFFFu) |
          ((ctx.r8.u32 & 3) << 30)};
  std::array<uint32_t, 18> words;
  rex::graphics::native_gpu_calls::EncodeVertices(call, words);
  for (uint32_t i = 0; i < words.size(); ++i)
    StoreGuestU32(base, plan.cursor + 4 + i * 4, words[i]);
  Store64(base, device + 24, Load64(base, device + 24) | 1);
  StoreGuestU32(base, device + 13428, plan.cursor + 72);
  StoreGuestU32(base, device + 13432, vertex);
  StoreGuestU32(base, device + 13436, index);
  StoreGuestU32(base, device + 13440, plan.vertex_dwords);
  StoreGuestU32(base, device + 13444, plan.index_dwords);
  StoreGuestU32(base, plan.out_vertex, vertex);
  StoreGuestU32(base, plan.out_index, index);
  ctx.r3.u64 = 0;
  return true;
}

template <typename Allocate>
bool BeginIndexedVertices(PPCContext& ctx, uint8_t* base, Allocate allocate) {
  IndexedBeginPlan plan;
  return PlanIndexedBegin(ctx, base, plan) &&
         BeginIndexedVerticesWithPlan(ctx, base, plan, allocate);
}

// DrawPrimitive with already-bound streams. The optional whole-flush plan
// handles ordinary dirty shadows without allocating or growing the ring.
inline bool DrawRange(PPCContext& ctx, uint8_t* base, bool indexed = false,
                      bool flush_dirty = false, bool samplers = false, bool conditional = false) {
  using namespace native_gpu_memory;
  const uint32_t device = ctx.r3.u32;
  const uint32_t count = indexed ? ctx.r7.u32 : ctx.r6.u32;
  const uint32_t frame = indexed ? 240 : 208, bytes = indexed ? 28 : 20;
  MemoryRange dev, stack, commands;
  if (!base || (device & 3) || (ctx.r1.u32 & 15) || ctx.r1.u32 < frame || count > 65535 ||
      !BackingRange(device, 14916, dev) || !BackingRange(ctx.r1.u32 - frame, frame, stack) ||
      Overlaps(dev, stack))
    return false;
  const auto dirty = native_gpu_flush::DirtyState(base, device);
  const bool needs_flush =
      std::any_of(dirty.begin(), dirty.end(), [](uint64_t mask) { return mask; });
  native_gpu_flush::FlushPlan flush;
  if (needs_flush && (!flush_dirty || !native_gpu_flush::PlanFlush(ctx, base, frame, dirty, flush,
                                                                   samplers, conditional)))
    return false;
  const uint32_t flush_frame = conditional ? 192 : 176;
  if (needs_flush && !BackingRange(ctx.r1.u32 - frame - flush_frame, frame + flush_frame, stack))
    return false;
  const uint32_t initial = LoadGuestU32(base, device + 48);
  const uint32_t cursor = needs_flush ? flush.cursor : initial;
  if ((*GuestToHost(base, device + 10940) & 1) || (cursor & 3) ||
      cursor > LoadGuestU32(base, device + 56) ||
      uint64_t(cursor) + bytes > LoadGuestU32(base, device + 52) ||
      !BackingRange(initial + 4, uint64_t(cursor) + bytes - initial, commands) ||
      Overlaps(commands, dev) || Overlaps(commands, stack))
    return false;
  const uint32_t mode = *GuestToHost(base, device + 12179);
  rex::graphics::native_gpu_calls::RangeDrawCall call{
      ctx.r5.u32, uint32_t(*GuestToHost(base, device + 12114 + mode) & 0x3F) | (mode ? 0x100u : 0u),
      (ctx.r4.u32 & 63) | (count << 16) | 0x80};
  if (indexed) {
    const uint32_t pointer = LoadGuestU32(base, device + 12428);
    MemoryRange metadata;
    if ((pointer & 3) || !BackingRange(pointer, 28, metadata) || Overlaps(metadata, dev) ||
        Overlaps(metadata, stack) || Overlaps(metadata, commands))
      return false;
    const uint32_t format = LoadGuestU32(base, pointer);
    const bool index32 = format & 0x80000000u;
    const uint32_t address = LoadGuestU32(base, pointer + 24) + ctx.r6.u32 * (index32 ? 4 : 2);
    call.initiator = (ctx.r4.u32 & 63) | (count << 16) | (index32 ? 0x800u : 0u);
    call.index_base = (address & 0x1FFFFFFFu) + (((address >> 20) + 512) & 0x1000u);
    call.index_size =
        (index32 ? ((count << 1) & 0xFFFFFEu) : count) | ((format << 1) & 0xC0000000u);
  }
  std::array<uint32_t, 7> words;
  const uint32_t dwords = bytes / 4;
  rex::graphics::native_gpu_calls::EncodeRange(call, {words.data(), dwords});
  if (needs_flush)
    native_gpu_flush::ApplyFlush(flush, base);
  for (uint32_t i = 0; i < dwords; ++i)
    StoreGuestU32(base, cursor + 4 + i * 4, words[i]);
  StoreGuestU32(base, device + 48, cursor + bytes);
  ctx.r3.u64 = cursor + 4;
  ctx.lr = uint32_t(ctx.lr);
  return true;
}

}  // namespace simpsons::native_gpu_vertices
