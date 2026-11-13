#pragma once

#include "native_gpu_cache_range.h"
#include "native_gpu_chunk_close.h"

namespace simpsons::native_gpu_vertices {
namespace descriptor_list_detail {
using namespace native_gpu_memory;

struct Plan {
  default_allocator_detail::TransientPlan normal;
  recorded_allocator_detail::TransientPlan recording;
  cache_range_detail::Plan cache;
  uint32_t list = 0, device = 0, stack = 0, previous = 0, cursor = 0, result = 0;
  bool fixed = false, success = false;
};

inline bool OwnedBlock(const uint8_t* base, uint32_t device, uint32_t address, uint32_t bytes,
                       const MemoryRange& dev, const MemoryRange& frame, const MemoryRange& list,
                       MemoryRange& block) {
  MemoryRange owner, object{};
  return chunk_close_detail::PhysicalSpan(address, bytes, block) &&
         chunk_close_detail::Owner(base, device, false, dev, frame, owner, object) &&
         block.first >= owner.first && block.end <= owner.end && !Overlaps(block, list);
}

inline bool Build(const PPCContext& ctx, const uint8_t* base, Plan& plan) {
  const uint32_t list = ctx.r3.u32, sp = ctx.r1.u32;
  MemoryRange metadata, frame, dev, previous{}, cursor{}, next{};
  if (!base || (list & 3) || (sp & 15) || sp < 480 || !BackingRange(list, 20, metadata) ||
      !BackingRange(sp - 480, 480, frame) || Overlaps(metadata, frame))
    return false;
  const uint32_t device = LoadGuestU32(base, list + 16);
  if ((device & 3) || !BackingRange(device, 22268, dev) || Overlaps(dev, frame))
    return false;
  // These seven lists are embedded in the real device; other retained list
  // objects must be separate from allocator metadata.
  const uint32_t offset = list - device;
  if (Overlaps(metadata, dev) &&
      (offset < 13232 || offset > 13352 || (offset - 13232) % 20))
    return false;
  plan.list = list;
  plan.device = device;
  plan.stack = sp;
  plan.previous = LoadGuestU32(base, list + 4);
  plan.cursor = LoadGuestU32(base, list + 8);
  if (plan.previous || plan.cursor) {
    if (!plan.previous || (plan.previous & 127) || plan.cursor < uint64_t(plan.previous) + 4 ||
        (plan.cursor & 3) || uint64_t(plan.cursor) + 4 > uint64_t(plan.previous) + 4224 ||
        uint64_t(plan.previous) + 4220 != LoadGuestU32(base, list + 12) ||
        !OwnedBlock(base, device, plan.previous, 4224, dev, frame, metadata, previous) ||
        !chunk_close_detail::PhysicalSpan(plan.cursor, 4, cursor))
      return false;
  }
  const uint8_t flags = *GuestToHost(base, device + 10941);
  plan.fixed = (flags & 0x20) || LoadGuestU32(base, device + 13492);
  if (plan.fixed) {
    if (!recorded_allocator_detail::PlanTransient(base, device, sp - 112, 4224, 128,
                                                  plan.recording))
      return false;
    plan.result = plan.recording.allocation.result;
  } else {
    if (!default_allocator_detail::PlanTransient(base, device, sp - 112, 4224, 128, plan.normal))
      return false;
    plan.result = plan.normal.allocation.failed ? 0 : plan.normal.allocation.first;
  }
  plan.success = plan.result != 0;
  if (plan.success) {
    // 82456D60 takes words: retain the original 16,896-byte reservation even
    // though the descriptor block itself uses 4,224 bytes.
    if ((plan.result & 127) ||
        !OwnedBlock(base, device, plan.result, 4224 * 4, dev, frame, metadata, next) ||
        Overlaps(next, previous) || Overlaps(next, cursor))
      return false;
    const uint32_t first = chunk_close_detail::Physical(plan.result) - 0x40000000u;
    if (!cache_range_detail::Build(first, first + 4224, sp - 112, base, plan.cache))
      return false;
  } else {
    plan.result = LoadGuestU32(base, device + 16704);
    if (!plan.result || (plan.result & 3) ||
        !BackingRange(plan.result, 4800, next) || Overlaps(next, dev) ||
        Overlaps(next, frame) || Overlaps(next, metadata) || Overlaps(next, previous))
      return false;
  }
  if (plan.previous) {
    // The old link is written before allocation. Protect every value the
    // allocator reads, including its completed-fence storage and recording.
    const uint32_t sync = LoadGuestU32(base, device + 10896);
    MemoryRange completed, object;
    if (sync && ((sync & 3) || !BackingRange(sync, 8, completed) ||
                 Overlaps(completed, previous) || Overlaps(completed, next)))
      return false;
    const uint32_t recording = LoadGuestU32(base, device + 13492);
    if (recording && ((recording & 3) || !BackingRange(recording, 184, object) ||
                      Overlaps(object, previous) || Overlaps(object, next)))
      return false;
  }
  return true;
}

inline void Apply(PPCContext& ctx, uint8_t* base, const Plan& plan) {
  using recorded_allocator_detail::Store64;
  const uint32_t sp = plan.stack, list = plan.list;
  Store64(base, sp - 32, ctx.r29.u64);
  Store64(base, sp - 24, ctx.r30.u64);
  Store64(base, sp - 16, ctx.r31.u64);
  StoreGuestU32(base, sp - 8, uint32_t(ctx.lr));
  StoreGuestU32(base, sp - 112, sp);
  ctx.r1.u32 -= 112;
  ctx.r30.u64 = ctx.r3.u64;
  ctx.r29.u64 = 0x40000000;
  if (plan.previous)
    StoreGuestU32(base, plan.previous, chunk_close_detail::Physical(plan.cursor) - 0x40000000u);
  ctx.r3.u64 = plan.device;
  ctx.r4.u64 = 4224;
  ctx.r5.u64 = 128;
  ctx.lr = 0x82457798;
  if (plan.fixed)
    recorded_allocator_detail::ApplyTransient(ctx, base, plan.recording);
  else
    default_allocator_detail::ApplyTransient(ctx, base, plan.normal);
  ctx.r31.u64 = plan.result;
  if (plan.success) {
    StoreGuestU32(base, plan.result,
                  chunk_close_detail::Physical(plan.result + 4) - 0x40000000u);
    if (plan.cursor)
      StoreGuestU32(base, plan.cursor, chunk_close_detail::Physical(plan.result) - 0x40000000u);
    else
      StoreGuestU32(base, list, plan.result);
    ctx.r3.u64 = uint64_t(chunk_close_detail::Physical(plan.result)) - 0x40000000u;
    ctx.r4.u64 = ctx.r3.u64 + 4224;
    ctx.lr = 0x82457824;
    cache_range_detail::Apply(ctx, base, plan.cache);
  } else {
    StoreGuestU32(base, list, 0);
  }
  ctx.r3.u64 = ctx.r31.u64 + 4;
  StoreGuestU32(base, list + 4, plan.result);
  StoreGuestU32(base, list + 8, ctx.r3.u32);
  StoreGuestU32(base, list + 12, plan.result + 4220);
  ctx.r1.u32 += 112;
  ctx.r29.u64 = chunk_close_detail::Load64(base, sp - 32);
  ctx.r30.u64 = chunk_close_detail::Load64(base, sp - 24);
  ctx.r31.u64 = chunk_close_detail::Load64(base, sp - 16);
  ctx.lr = LoadGuestU32(base, sp - 8);
}
}  // namespace descriptor_list_detail

inline bool ExpandDescriptorList(PPCContext& ctx, uint8_t* base) {
  descriptor_list_detail::Plan plan;
  if (!descriptor_list_detail::Build(ctx, base, plan))
    return false;
  descriptor_list_detail::Apply(ctx, base, plan);
  return true;
}
}  // namespace simpsons::native_gpu_vertices
