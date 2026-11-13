#pragma once

#include "native_gpu_cache_range.h"
#include "native_gpu_ring_submission.h"

namespace simpsons::native_gpu_vertices {
namespace linked_append_detail {
using namespace native_gpu_memory;

struct Plan {
  cache_range_detail::Plan first_cache, tail_cache;
  uint32_t stack = 0, list = 0, node = 0, end = 0, tail = 0;
};

inline bool Build(const PPCContext& ctx, const uint8_t* base, Plan& plan,
                  const MemoryRange& parent = {}, const MemoryRange& pcr = {}) {
  const uint32_t list = ctx.r3.u32, node = ctx.r4.u32, end = ctx.r5.u32, sp = ctx.r1.u32;
  MemoryRange metadata, frame, dev, data, tail;
  if (!base || ((list | node | end) & 3) || (sp & 15) || sp < 144 || end <= node ||
      !BackingRange(list, 20, metadata) || !BackingRange(sp - 144, 144, frame) ||
      Overlaps(metadata, frame) || Overlaps(metadata, parent) || Overlaps(metadata, pcr))
    return false;
  const uint32_t device = LoadGuestU32(base, list + 16);
  if ((device & 3) || !BackingRange(device, 22268, dev) || Overlaps(dev, frame) ||
      Overlaps(dev, parent) || Overlaps(dev, pcr))
    return false;
  const uint32_t offset = list - device;
  if (Overlaps(metadata, dev) &&
      (offset < 13232 || offset > 13352 || (offset - 13232) % 20))
    return false;
  const uint32_t old_tail = LoadGuestU32(base, list + 8);
  if (!old_tail || (old_tail & 3) ||
      !chunk_close_detail::PhysicalSpan(node, uint64_t(end) - node, data) ||
      !ring_submission_detail::OwnedSpan(base, device,
          {chunk_close_detail::Physical(node), (end - node) / 4}, dev, frame, parent, data) ||
      !chunk_close_detail::PhysicalSpan(old_tail, 4, tail) ||
      !ring_submission_detail::OwnedSpan(base, device,
          {chunk_close_detail::Physical(old_tail), 1}, dev, frame, parent, tail) ||
      Overlaps(data, tail) || Overlaps(data, metadata) || Overlaps(tail, metadata) ||
      Overlaps(data, pcr) || Overlaps(tail, pcr))
    return false;
  const uint32_t first = chunk_close_detail::Physical(node) - 0x40000000u;
  const uint32_t last = chunk_close_detail::Physical(end) - 0x40000000u;
  const uint32_t tail_first = chunk_close_detail::Physical(old_tail) - 0x40000000u;
  if (!cache_range_detail::Build(first, last, sp - 112, base, plan.first_cache) ||
      !cache_range_detail::Build(tail_first, tail_first + 16, sp - 112, base, plan.tail_cache))
    return false;
  plan.stack = sp;
  plan.list = list;
  plan.node = node;
  plan.end = end;
  plan.tail = old_tail;
  return true;
}

template <typename Store>
inline void Apply(PPCContext& ctx, uint8_t* base, const Plan& plan, Store store) {
  const uint32_t sp = plan.stack;
  ring_submission_detail::Save(ctx, base, sp, 8);
  StoreGuestU32(base, sp - 112, sp);
  ctx.r1.u32 -= 112;
  ctx.r31.u64 = plan.tail;
  StoreGuestU32(base, plan.node, chunk_close_detail::Physical(plan.end) - 0x40000000u);
  StoreGuestU32(base, plan.list + 4, plan.node);
  ctx.r30.u64 = uint64_t(chunk_close_detail::Physical(plan.node)) - 0x40000000u;
  StoreGuestU32(base, plan.list + 8, plan.end - 4);
  ctx.r29.u64 = uint64_t(chunk_close_detail::Physical(plan.tail)) - 0x40000000u;
  REX_PPC_EIEIO();
  ctx.r4.u64 = uint64_t(chunk_close_detail::Physical(plan.end)) - 0x40000000u;
  ctx.r3.u64 = ctx.r30.u64;
  ctx.lr = 0x82456B10;
  cache_range_detail::Apply(ctx, base, plan.first_cache);
  store(plan.tail, ctx.r30.u32);
  REX_PPC_EIEIO();
  ctx.r4.u64 = ctx.r29.u64 + 16;
  ctx.r3.u64 = ctx.r29.u64;
  ctx.lr = 0x82456B24;
  cache_range_detail::Apply(ctx, base, plan.tail_cache);
  ctx.r1.u32 += 112;
  ring_submission_detail::Restore(ctx, base, sp, 8);
}
}  // namespace linked_append_detail

template <typename Store>
inline bool AppendLinkedCommandBuffer(PPCContext& ctx, uint8_t* base, Store store) {
  linked_append_detail::Plan plan;
  if (!linked_append_detail::Build(ctx, base, plan))
    return false;
  linked_append_detail::Apply(ctx, base, plan, store);
  return true;
}
}  // namespace simpsons::native_gpu_vertices
