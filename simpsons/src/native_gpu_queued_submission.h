#pragma once

#include <rex/kernel/xboxkrnl/threading.h>

#include "native_gpu_linked_append.h"

namespace simpsons::native_gpu_vertices {

// Use the same SDK implementations as KfAcquire/ReleaseSpinLock. They preserve
// the PCR owner, real contention, IRQL transitions and release ordering.
struct QueuedKernelLock {
  static void Acquire(PPCContext& ctx, uint8_t* base) {
    ctx.r3.u64 = rex::kernel::xboxkrnl::xeKeKfAcquireSpinLock(
        &ctx, reinterpret_cast<rex::X_KSPINLOCK*>(GuestToHost(base, ctx.r3.u32)));
  }
  static void Release(PPCContext& ctx, uint8_t* base) {
    rex::kernel::xboxkrnl::xeKeKfReleaseSpinLock(
        &ctx, reinterpret_cast<rex::X_KSPINLOCK*>(GuestToHost(base, ctx.r3.u32)), ctx.r4.u32);
  }
};

namespace queued_submission_detail {
using namespace native_gpu_memory;

struct Plan {
  uint32_t device = 0, stack = 0;
  MemoryRange frame{}, pcr{};
};

inline bool Build(const PPCContext& ctx, const uint8_t* base, Plan& plan) {
  const uint32_t device = ctx.r3.u32, sp = ctx.r1.u32, pcr = ctx.r13.u32;
  MemoryRange dev, node;
  if (!base || ((device | pcr | ctx.r4.u32) & 3) || (sp & 15) || sp < 496 || !pcr ||
      !BackingRange(device, 22268, dev) || !BackingRange(sp - 496, 496, plan.frame) ||
      !BackingRange(pcr, 28, plan.pcr) || Overlaps(dev, plan.frame) ||
      Overlaps(dev, plan.pcr) || Overlaps(plan.pcr, plan.frame) ||
      !LoadGuestU32(base, device + 11000) ||
      !chunk_close_detail::PhysicalSpan(ctx.r4.u32, 16, node) ||
      !ring_submission_detail::OwnedSpan(base, device,
          {chunk_close_detail::Physical(ctx.r4.u32), 4}, dev, plan.frame, plan.pcr, node))
    return false;
  plan.device = device;
  plan.stack = sp;
  return true;
}

template <typename Ops>
inline void Apply(PPCContext& ctx, uint8_t* base, const Plan& plan, Ops ops) {
  const uint32_t sp = plan.stack, device = plan.device;
  ring_submission_detail::Save(ctx, base, sp, 2);
  StoreGuestU32(base, sp - 176, sp);
  ctx.r1.u32 -= 176;
  ctx.r30.u64 = ctx.r3.u64;
  ctx.r31.u64 = ctx.r4.u64;
  ctx.r28.u64 = ctx.r5.u64;
  ctx.r27.u64 = ctx.r6.u64;
  ctx.r26.u64 = ctx.r7.u64;
  ctx.r25.u64 = ctx.r8.u64;
  ctx.r29.u64 = 0;
  ctx.r24.u64 = ctx.r30.u64 + 11004;
  ctx.r3.u64 = ctx.r24.u64;
  ctx.lr = 0x82457338;
  ops.Acquire(ctx, base);
  ctx.r23.u64 = ctx.r3.u64;
  if (LoadGuestU32(base, device + 11000)) {
    StoreGuestU32(base, ctx.r31.u32 + 4, ctx.r27.u32 | 0x81000000u);
    StoreGuestU32(base, ctx.r31.u32 + 8, ctx.r28.u32);
    StoreGuestU32(base, ctx.r31.u32 + 12, 0xC0000000);
    ctx.r4.u64 = ctx.r31.u64;
    ctx.r3.u64 = ctx.r25.u64;
    ctx.r31.u64 += 16;
    ctx.r5.u64 = ctx.r31.u64;
    ctx.lr = 0x82457370;
    linked_append_detail::Plan append;
    if (linked_append_detail::Build(ctx, base, append, plan.frame, plan.pcr))
      linked_append_detail::Apply(ctx, base, append,
                                 [&](uint32_t address, uint32_t value) { ops.Store(base, address, value); });
    else
      ops.AppendOriginal(ctx, base);
    StoreGuestU32(base, device + 11000, LoadGuestU32(base, device + 11000) + ctx.r26.u32);
    ctx.r29.u64 = 1;
  }
  ctx.r4.u64 = ctx.r23.u64;
  ctx.r3.u64 = ctx.r24.u64;
  ctx.lr = 0x8245738C;
  ops.Release(ctx, base);
  if (!ctx.r29.u32) {
    // A concurrent drain won the lock. Keep the original inline descriptor
    // and publish after release, including its real wait/callback fallback.
    StoreGuestU32(base, sp - 92, ctx.r28.u32);
    StoreGuestU32(base, device + 11000, LoadGuestU32(base, device + 11000) + ctx.r26.u32);
    StoreGuestU32(base, sp - 96, 0x81000000u | (ctx.r27.u32 & 0xFFFFFF));
    ctx.r5.u64 = 1;
    ctx.r4.u64 = ctx.r1.u64 + 80;
    ctx.r3.u64 = ctx.r30.u64;
    ctx.lr = 0x824573C0;
    const rex::graphics::native_gpu_indirect::BufferCall call{ctx.r28.u32, ctx.r27.u32 & 0xFFFFFF};
    ring_submission_detail::RingPlan ring;
    if (ring_submission_detail::PlanRing(device, ctx.r1.u32, ctx.r4.u32, 1, base, ring, &call, plan.frame))
      ring_submission_detail::ApplyRing(ctx, base, ring,
                                       [&](uint32_t address, uint32_t value) { ops.Store(base, address, value); });
    else
      ops.PublishOriginal(ctx, base);
  }
  ctx.r3.u64 = ctx.r31.u64;
  ctx.r1.u32 += 176;
  ring_submission_detail::Restore(ctx, base, sp, 2);
}
}  // namespace queued_submission_detail

template <typename Ops>
inline bool SubmitQueuedCommandBuffer(PPCContext& ctx, uint8_t* base, Ops ops) {
  queued_submission_detail::Plan plan;
  if (!queued_submission_detail::Build(ctx, base, plan))
    return false;
  queued_submission_detail::Apply(ctx, base, plan, ops);
  return true;
}
}  // namespace simpsons::native_gpu_vertices
