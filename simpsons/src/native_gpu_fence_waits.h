#pragma once

#include "native_gpu_ring_submission.h"

namespace simpsons::native_gpu_vertices {
namespace fence_wait_detail {
using namespace native_gpu_memory;

struct Plan {
  uint32_t device = 0, stack = 0;
};

inline bool Build(const PPCContext& ctx, const uint8_t* base, bool marker, Plan& plan) {
  const uint32_t device = ctx.r3.u32, sp = ctx.r1.u32;
  const bool empty = marker && !ctx.r4.u32;
  const uint32_t bytes = empty ? 144 : 272;
  MemoryRange dev, frame, sync;
  if (!base || (sp & 15) || sp < bytes || !BackingRange(sp - bytes, bytes, frame))
    return false;
  plan = {device, sp};
  if (empty)
    return true;  // The original zero-target path never dereferences the device.
  if ((device & 3) || !BackingRange(device, 22268, dev) || Overlaps(dev, frame))
    return false;
  const uint32_t completed = LoadGuestU32(base, device + 10896);
  return completed && !(completed & 3) && BackingRange(completed, 8, sync) &&
         sync.first >= 0x100000000ull && sync.end <= 0x120000000ull &&
         !Overlaps(sync, dev) && !Overlaps(sync, frame);
}

inline bool AllocationReady(const uint8_t* base, uint32_t device, uint32_t end, uint32_t version) {
  const uint32_t sync = LoadGuestU32(base, device + 10896);
  const uint32_t fence = LoadGuestU32(base, sync + 4);
  const uint32_t distance = (version - fence) & 3;
  return !distance || (distance == 1 && end <= (fence & ~3u));
}

inline bool MarkerReady(const uint8_t* base, uint32_t device, uint32_t target) {
  const uint32_t sync = LoadGuestU32(base, device + 10896);
  const uint32_t submitted = LoadGuestU32(base, device + 10908);
  const uint32_t completed = LoadGuestU32(base, sync);
  return submitted - target >= submitted - completed;
}

inline void Enter(PPCContext& ctx, uint8_t* base, const Plan& plan, bool marker) {
  ring_submission_detail::Save(ctx, base, plan.stack, 8);
  StoreGuestU32(base, plan.stack - 144, plan.stack);
  ctx.r1.u32 -= 144;
  ctx.r31.u64 = ctx.r3.u64;
  ctx.r30.u64 = marker ? ctx.r4.u64 : ctx.r5.u64;
  ctx.r29.u64 = marker ? ctx.r5.u64 : ctx.r4.u64;
}

inline void Leave(PPCContext& ctx, uint8_t* base, const Plan& plan) {
  ctx.r1.s64 += 144;
  ring_submission_detail::Restore(ctx, base, plan.stack, 8);
}

template <typename Ops>
inline void Allocation(PPCContext& ctx, uint8_t* base, const Plan& plan, Ops ops) {
  Enter(ctx, base, plan, false);
  if (!AllocationReady(base, plan.device, ctx.r29.u32, ctx.r30.u32)) {
    ctx.r5.u64 = 2;
    ctx.r4.u64 = ctx.r31.u64;
    ctx.r3.u64 = ctx.r1.u64 + 80;
    ctx.lr = 0x82456520;
    ops.Begin(ctx, base);
    do {
      ctx.r3.u64 = ctx.r1.u64 + 80;
      ctx.lr = 0x82456528;
      ops.Poll(ctx, base);
      if (!ctx.r3.u32)
        break;  // Preserve the real abort/hang outcome; never mark it complete.
    } while (!AllocationReady(base, plan.device, ctx.r29.u32, ctx.r30.u32));
    ctx.r3.u64 = ctx.r1.u64 + 80;
    ctx.lr = 0x82456560;
    ops.End(ctx, base);
  }
  Leave(ctx, base, plan);
}

template <typename Ops>
inline void Marker(PPCContext& ctx, uint8_t* base, const Plan& plan, Ops ops) {
  Enter(ctx, base, plan, true);
  if (ctx.r30.u32 && !MarkerReady(base, plan.device, ctx.r30.u32)) {
    const uint32_t submitted = LoadGuestU32(base, plan.device + 10908);
    if (ctx.r30.u32 == submitted) {
      if (LoadGuestU32(base, plan.device + 13216)) {
        Leave(ctx, base, plan);
        return;
      }
      ctx.lr = 0x82457510;
      ops.Dispatch(ctx, base);
    }
    if (!MarkerReady(base, plan.device, ctx.r30.u32)) {
      ctx.r5.u64 = ctx.r29.u64;
      ctx.r4.u64 = ctx.r31.u64;
      ctx.r3.u64 = ctx.r1.u64 + 80;
      ctx.lr = 0x8245753C;
      ops.Begin(ctx, base);
      // Unlike allocation waits, the marker path rechecks after construction
      // before its first poll. Both predicates reload the live owner/sequence.
      while (!MarkerReady(base, plan.device, ctx.r30.u32)) {
        ctx.r3.u64 = ctx.r1.u64 + 80;
        ctx.lr = 0x82457548;
        ops.Poll(ctx, base);
        if (!ctx.r3.u32)
          break;
      }
      ctx.r3.u64 = ctx.r1.u64 + 80;
      ctx.lr = 0x82457574;
      ops.End(ctx, base);
    }
  }
  Leave(ctx, base, plan);
}
}  // namespace fence_wait_detail

template <typename Ops>
inline bool WaitForAllocationFence(PPCContext& ctx, uint8_t* base, Ops ops) {
  fence_wait_detail::Plan plan;
  if (!fence_wait_detail::Build(ctx, base, false, plan))
    return false;
  fence_wait_detail::Allocation(ctx, base, plan, ops);
  return true;
}

template <typename Ops>
inline bool WaitForGpuMarker(PPCContext& ctx, uint8_t* base, Ops ops) {
  fence_wait_detail::Plan plan;
  if (!fence_wait_detail::Build(ctx, base, true, plan))
    return false;
  fence_wait_detail::Marker(ctx, base, plan, ops);
  return true;
}
}  // namespace simpsons::native_gpu_vertices
