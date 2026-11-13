#pragma once

#include "native_gpu_ring_submission.h"

namespace simpsons::native_gpu_vertices {
namespace normal_close_detail {
using namespace native_gpu_memory;

struct Plan {
  chunk_close_detail::CompletionPlan completion;
  ring_submission_detail::SubmitPlan submission;
  chunk_growth_detail::Plan growth;
  uint32_t stack = 0, device = 0, chunk = 0, end = 0, aligned = 0;
  bool grow = false;
};

inline bool Build(const PPCContext& ctx, const uint8_t* base, Plan& plan) {
  const uint32_t device = ctx.r3.u32, sp = ctx.r1.u32;
  MemoryRange dev, frame, owner, object{}, commands, sync;
  if (!base || (device & 3) || (sp & 15) || sp < 624 ||
      !BackingRange(device, 22268, dev) || !BackingRange(sp - 624, 624, frame) ||
      Overlaps(dev, frame) || (*GuestToHost(base, device + 10941) & 0x20) ||
      (*GuestToHost(base, device + 10940) & 0x80) || LoadGuestU32(base, device + 13216) ||
      LoadGuestU32(base, device + 13492) ||
      !chunk_close_detail::Owner(base, device, false, dev, frame, owner, object))
    return false;
  const uint32_t cursor = LoadGuestU32(base, device + 48);
  const uint32_t chunk = LoadGuestU32(base, device + 14904);
  if ((cursor | chunk) & 3 || uint64_t(cursor) + 75 > UINT32_MAX || cursor + 4 < chunk)
    return false;
  const uint32_t end = cursor + 44;
  if (!chunk_close_detail::PhysicalSpan(chunk, end - chunk, commands) ||
      commands.first < owner.first || commands.end > owner.end)
    return false;
  PPCContext child = ctx;
  child.r1.u32 -= 128;
  child.r4.u64 = cursor + 4;
  if (!chunk_close_detail::PlanCompletion(child, base, plan.completion) ||
      !BackingRange(plan.completion.sync, 8, sync) || Overlaps(sync, frame))
    return false;
  child.r4.u64 = end;
  child.r5.u64 = chunk_close_detail::Physical(chunk);
  child.r6.u64 = (end - chunk) / 4;
  child.r7.u64 = 0;
  child.r8.u64 = ctx.r3.u64 + 13352;
  if (child.r6.u64 > 0xFFFFF || !ring_submission_detail::PlanSubmit(child, base, plan.submission))
    return false;
  if (!plan.submission.ring.cpu_mode) {
    MemoryRange ring;
    if (!chunk_close_detail::PhysicalSpan(plan.submission.ring.ring,
                                        uint64_t(plan.submission.ring.mask + 1) * 4, ring) ||
        Overlaps(ring, frame) || Overlaps(ring, owner) ||
        !BackingRange(plan.submission.ring.ready.completed, 96, sync) || Overlaps(sync, frame))
      return false;
  }
  const uint32_t ready = LoadGuestU32(base, device + 56);
  const uint32_t capacity = LoadGuestU32(base, device + 52);
  if ((ready | capacity) & 3 || ready > capacity)
    return false;
  plan.stack = sp;
  plan.device = device;
  plan.chunk = chunk;
  plan.end = end;
  plan.aligned = (end + 31) & ~31u;
  plan.grow = plan.aligned > ready;
  if (plan.grow) {
    // Completion changes the sequence and can mirror the fence. Closing also
    // changes the cursor. Only plan against the old snapshot when growth reads
    // none of those changed fields: an existing tail selects the next chunk,
    // no pending marker needs the sequence, and hardware owns the fence.
    if (plan.completion.mirror || !LoadGuestU32(base, device + 14892) ||
        LoadGuestU32(base, device + 14916) ||
        !chunk_growth_detail::BuildArguments(device, sp - 128, 0, base, plan.growth))
      return false;
  } else {
    MemoryRange next;
    if (!BackingRange(plan.aligned, 4, next) || next.first < owner.first || next.end > owner.end)
      return false;
  }
  return true;
}

template <typename Doorbell, typename EnsureWritable>
inline void Apply(PPCContext& ctx, uint8_t* base, const Plan& plan, Doorbell doorbell,
                  EnsureWritable ensure) {
  const uint32_t sp = plan.stack, device = plan.device;
  ring_submission_detail::Save(ctx, base, sp, 7);
  StoreGuestU32(base, sp - 128, sp);
  ctx.r1.u32 -= 128;
  ctx.r31.u64 = ctx.r3.u64;
  ctx.r30.u64 = plan.chunk;
  ctx.r28.u64 = plan.completion.output;
  ctx.r4.u64 = ctx.r28.u64;
  ctx.lr = 0x82457C4C;
  chunk_close_detail::ApplyCompletion(ctx, base, plan.completion);
  ctx.r3.u64 = plan.end;
  ctx.r4.u64 = ctx.r3.u64;
  ctx.r8.u64 = ctx.r31.u64 + 13352;
  ctx.r5.u64 = chunk_close_detail::Physical(plan.chunk);
  ctx.r7.u64 = 0;
  ctx.r6.u64 = (plan.end - plan.chunk) / 4;
  ctx.r3.u64 = ctx.r31.u64;
  ctx.lr = 0x82457C7C;
  ring_submission_detail::ApplySubmit(ctx, base, plan.submission, doorbell);
  ctx.r28.u64 = ctx.r3.u64;
  if (plan.grow) {
    StoreGuestU32(base, device + 48, plan.end - 4);
    ctx.r4.u64 = 0;
    ctx.r3.u64 = ctx.r31.u64;
    ctx.lr = 0x82457CA8;
    chunk_growth_detail::Apply(ctx, base, plan.growth, ensure);
    ctx.r3.u64 = plan.growth.first;
  } else {
    StoreGuestU32(base, device + 14904, plan.aligned);
    StoreGuestU32(base, device + 48, plan.aligned - 4);
  }
  ctx.r1.s64 += 128;
  ring_submission_detail::Restore(ctx, base, sp, 7);
}
}  // namespace normal_close_detail

template <typename Doorbell, typename EnsureWritable>
inline bool CloseNormalCommandChunkWithGuard(PPCContext& ctx, uint8_t* base, Doorbell doorbell,
                                             EnsureWritable ensure) {
  normal_close_detail::Plan plan;
  if (!normal_close_detail::Build(ctx, base, plan))
    return false;
  normal_close_detail::Apply(ctx, base, plan, doorbell, ensure);
  return true;
}

template <typename Doorbell>
inline bool CloseNormalCommandChunk(PPCContext& ctx, uint8_t* base, Doorbell doorbell) {
  return CloseNormalCommandChunkWithGuard(ctx, base, doorbell,
                                         native_gpu_memory::EnsureCommandStorageWritable);
}
}  // namespace simpsons::native_gpu_vertices
