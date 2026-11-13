#pragma once

#include <array>
#include <bit>

#include <rex/graphics/native_gpu_indirect.h>
#include <rex/ppc/barriers.h>

#include "simpsons_init.h"
#include "native_gpu_chunk_close.h"

namespace simpsons::native_gpu_vertices {
namespace ring_submission_detail {
using namespace native_gpu_memory;
using chunk_close_detail::Load64;
using recorded_allocator_detail::Store64;
using rex::graphics::native_gpu_indirect::BufferCall;

constexpr uint32_t kDoorbell = 0x7FC80714;
constexpr uint32_t kMaxDescriptors = 527;
constexpr std::array<PPCRegister PPCContext::*, 11> kSaved = {
    &PPCContext::r21, &PPCContext::r22, &PPCContext::r23, &PPCContext::r24,
    &PPCContext::r25, &PPCContext::r26, &PPCContext::r27, &PPCContext::r28,
    &PPCContext::r29, &PPCContext::r30, &PPCContext::r31};

inline void Save(const PPCContext& ctx, uint8_t* base, uint32_t sp, uint32_t first) {
  for (uint32_t i = first; i < kSaved.size(); ++i)
    Store64(base, sp - 96 + i * 8, (ctx.*kSaved[i]).u64);
  StoreGuestU32(base, sp - 8, uint32_t(ctx.lr));
}
inline void Restore(PPCContext& ctx, const uint8_t* base, uint32_t sp, uint32_t first) {
  for (uint32_t i = first; i < kSaved.size(); ++i)
    (ctx.*kSaved[i]).u64 = Load64(base, sp - 96 + i * 8);
  ctx.lr = LoadGuestU32(base, sp - 8);
}

inline bool RingMask(uint32_t mask) {
  return mask >= 3 && mask <= 0xFFFFF && std::has_single_bit(mask + 1);
}

struct ReadyPlan {
  uint32_t stack = 0, end = 0, completed = 0;
};

inline bool PlanReady(uint32_t device, uint32_t sp, uint64_t start, uint64_t words,
                      const uint8_t* base, ReadyPlan& plan) {
  MemoryRange dev, frame, sync;
  if (!base || (device & 3) || (sp & 15) || sp < 144 || !BackingRange(device, 22268, dev) ||
      !BackingRange(sp - 144, 144, frame) || Overlaps(dev, frame))
    return false;
  const uint32_t mask = LoadGuestU32(base, device + 14876);
  const uint32_t completed = LoadGuestU32(base, device + 10896);
  if (!RingMask(mask) || uint32_t(start) > mask || uint32_t(words) > mask || !completed ||
      (completed & 3) || !BackingRange(completed, 96, sync) || Overlaps(sync, dev) ||
      Overlaps(sync, frame))
    return false;
  const uint32_t end = uint32_t(start + words) & mask;
  const uint32_t read = LoadGuestU32(base, completed + 60);
  if (read > mask)
    return false;
  // Exactly the original unsigned circular-space predicate. In particular,
  // a zero-sized request is not a manufactured successful wait.
  const bool ready = uint32_t(start) < end ? uint32_t(start) >= read || read > end
                                           : uint32_t(start) >= read && read > end;
  if (!ready)
    return false;
  plan = {sp, end, completed};
  return true;
}

inline void ApplyReady(PPCContext& ctx, uint8_t* base, const ReadyPlan& plan) {
  Save(ctx, base, plan.stack, 8);
  StoreGuestU32(base, plan.stack - 144, plan.stack);
  ctx.r3.u64 = plan.end;
  ctx.lr = uint32_t(ctx.lr);
}

inline bool OwnedSpan(const uint8_t* base, uint32_t device, const BufferCall& call,
                      const MemoryRange& dev, const MemoryRange& frame, const MemoryRange& parent,
                      MemoryRange& data) {
  if (!rex::graphics::native_gpu_indirect::ValidateBuffer(call))
    return false;
  data = {0x100000000ull + call.address, 0x100000000ull + call.address + uint64_t(call.count) * 4};
  const auto contains = [&](uint32_t first, uint32_t bytes, const MemoryRange& object) {
    MemoryRange owner;
    return first && !((first | bytes) & 3) &&
           chunk_close_detail::PhysicalSpan(first, bytes, owner) && !Overlaps(owner, dev) &&
           !Overlaps(owner, frame) && !Overlaps(owner, parent) && !Overlaps(owner, object) &&
           data.first >= owner.first && data.end <= owner.end;
  };
  const uint32_t heap = LoadGuestU32(base, device + 14880);
  const uint32_t end = LoadGuestU32(base, device + 14884);
  if (end > heap && end - heap <= INT32_MAX && contains(heap, end - heap, {}))
    return true;
  const uint32_t recording = LoadGuestU32(base, device + 13492);
  MemoryRange object;
  return recording && !(recording & 3) && BackingRange(recording, 184, object) &&
         !Overlaps(object, dev) && !Overlaps(object, frame) && !Overlaps(object, parent) &&
         (LoadGuestU32(base, recording + 108) & 15) == 2 &&
         contains(LoadGuestU32(base, recording + 152), LoadGuestU32(base, recording + 156), object);
}

struct RingPlan {
  ReadyPlan ready;
  std::array<BufferCall, kMaxDescriptors> calls;
  uint32_t device = 0, stack = 0, ring = 0, mask = 0, start = 0, count = 0;
  bool cpu_mode = false;
};

inline bool PlanRing(uint32_t device, uint32_t sp, uint32_t descriptors, uint64_t count,
                     const uint8_t* base, RingPlan& plan, const BufferCall* single = nullptr,
                     const MemoryRange& parent = {}) {
  MemoryRange dev, frame, ring, inputs, sync;
  if (!base || (device & 3) || (sp & 15) || sp < 320 || !BackingRange(device, 22268, dev) ||
      !BackingRange(sp - 320, 320, frame) || Overlaps(dev, frame) || Overlaps(dev, parent))
    return false;
  // Preserve callback execution and its lifetime through the whole original.
  if (LoadGuestU32(base, device + 21508))
    return false;
  plan.device = device;
  plan.stack = sp;
  plan.cpu_mode = (*GuestToHost(base, device + 10941) & 2) != 0;
  if (plan.cpu_mode)
    return true;  // No callback means no descriptor/ring/wait access in this mode.
  if (LoadGuestU32(base, device + 21648) || !uint32_t(count) || uint32_t(count) > kMaxDescriptors ||
      (descriptors & 3) || !BackingRange(descriptors, uint64_t(uint32_t(count)) * 8, inputs) ||
      Overlaps(inputs, dev) || Overlaps(inputs, frame) || (!single && Overlaps(inputs, parent)))
    return false;
  plan.count = uint32_t(count);
  plan.ring = LoadGuestU32(base, device + 14872);
  plan.mask = LoadGuestU32(base, device + 14876);
  plan.start = LoadGuestU32(base, device + 10952);
  if (!RingMask(plan.mask) || uint64_t(plan.count) * 3 > plan.mask || !plan.ring ||
      (plan.ring & 3) ||
      !chunk_close_detail::PhysicalSpan(plan.ring, uint64_t(plan.mask + 1) * 4, ring) ||
      Overlaps(ring, dev) || Overlaps(ring, frame) || Overlaps(ring, parent) ||
      Overlaps(ring, inputs) ||
      !PlanReady(device, sp - 176, plan.start, count * 3, base, plan.ready) ||
      !BackingRange(plan.ready.completed, 96, sync) || Overlaps(sync, ring) ||
      Overlaps(sync, inputs) || Overlaps(sync, frame) || Overlaps(sync, parent))
    return false;
  for (uint32_t i = 0; i < plan.count; ++i) {
    const BufferCall call = single ? *single
                                   : BufferCall{LoadGuestU32(base, descriptors + i * 8 + 4),
                                                LoadGuestU32(base, descriptors + i * 8) & 0xFFFFFF};
    MemoryRange data;
    if (!OwnedSpan(base, device, call, dev, frame, parent, data) || Overlaps(data, ring) ||
        Overlaps(data, inputs) || Overlaps(data, sync))
      return false;
    plan.calls[i] = call;
  }
  return !single || plan.count == 1;
}

template <typename Doorbell>
inline void ApplyRing(PPCContext& ctx, uint8_t* base, const RingPlan& plan, Doorbell doorbell) {
  const uint32_t sp = plan.stack, device = plan.device;
  Save(ctx, base, sp, 0);
  StoreGuestU32(base, sp - 176, sp);
  ctx.r1.u32 -= 176;
  ctx.r26.u64 = ctx.r3.u64;
  ctx.r30.u64 = ctx.r4.u64;
  ctx.r31.u64 = ctx.r5.u64;
  if (!plan.cpu_mode) {
    ctx.r25.u64 = plan.mask;
    ctx.r24.u64 = plan.ring;
    ctx.r29.u64 = plan.start;
    ctx.r4.u64 = plan.start;
    ctx.r5.u64 = ctx.r31.u64 * 3;
    ctx.r3.u64 = ctx.r26.u64;
    ctx.lr = 0x82456F20;
    ApplyReady(ctx, base, plan.ready);
    uint32_t at = plan.start;
    for (uint32_t i = 0; i < plan.count; ++i) {
      for (uint32_t value : {rex::graphics::native_gpu_indirect::kBufferHeader,
                             plan.calls[i].address, plan.calls[i].count}) {
        StoreGuestU32(base, plan.ring + at * 4, value);
        at = (at + 1) & plan.mask;
      }
    }
    StoreGuestU32(base, device + 10952, at);
    REX_PPC_SYNC();
    doorbell(kDoorbell, at);
    REX_PPC_EIEIO();
    REX_PPC_SYNC();
    ctx.r3.u64 = 0;
  }
  ctx.r1.u32 += 176;
  Restore(ctx, base, sp, 0);
}

struct SubmitPlan {
  RingPlan ring;
  uint32_t device = 0, stack = 0, address = 0, words = 0;
};

inline bool PlanSubmit(const PPCContext& ctx, const uint8_t* base, SubmitPlan& plan) {
  const uint32_t device = ctx.r3.u32, sp = ctx.r1.u32;
  MemoryRange dev, frame;
  if (!base || (device & 3) || (sp & 15) || sp < 496 || !BackingRange(device, 22268, dev) ||
      !BackingRange(sp - 496, 496, frame) || Overlaps(dev, frame) ||
      LoadGuestU32(base, device + 11000))
    return false;  // Queued work requires the real spin lock and list operations.
  plan.device = device;
  plan.stack = sp;
  plan.address = ctx.r5.u32;
  plan.words = ctx.r6.u32 & 0xFFFFFF;
  const BufferCall call{plan.address, plan.words};
  return PlanRing(device, sp - 176, sp - 96, 1, base, plan.ring, &call, frame);
}

template <typename Doorbell>
inline void ApplySubmit(PPCContext& ctx, uint8_t* base, const SubmitPlan& plan, Doorbell doorbell) {
  const uint32_t sp = plan.stack, device = plan.device;
  Save(ctx, base, sp, 2);
  StoreGuestU32(base, sp - 176, sp);
  ctx.r1.u32 -= 176;
  ctx.r30.u64 = ctx.r3.u64;
  ctx.r31.u64 = ctx.r4.u64;
  ctx.r28.u64 = ctx.r5.u64;
  ctx.r27.u64 = ctx.r6.u64;
  ctx.r26.u64 = ctx.r7.u64;
  ctx.r25.u64 = ctx.r8.u64;
  ctx.r29.u64 = 0;
  StoreGuestU32(base, sp - 92, plan.address);
  StoreGuestU32(base, device + 11000, LoadGuestU32(base, device + 11000) + ctx.r26.u32);
  StoreGuestU32(base, sp - 96, 0x81000000 | plan.words);
  ctx.r4.u64 = ctx.r1.u64 + 80;
  ctx.r5.u64 = 1;
  ctx.r3.u64 = ctx.r30.u64;
  ctx.lr = 0x824573C0;
  ApplyRing(ctx, base, plan.ring, doorbell);
  ctx.r3.u64 = ctx.r31.u64;
  ctx.r1.u32 += 176;
  Restore(ctx, base, sp, 2);
}
}  // namespace ring_submission_detail

inline bool ReadySubmissionRing(PPCContext& ctx, uint8_t* base) {
  ring_submission_detail::ReadyPlan plan;
  if (!ring_submission_detail::PlanReady(ctx.r3.u32, ctx.r1.u32, ctx.r4.u64, ctx.r5.u64, base,
                                         plan))
    return false;
  ring_submission_detail::ApplyReady(ctx, base, plan);
  return true;
}

template <typename Doorbell>
inline bool PublishSubmissionRing(PPCContext& ctx, uint8_t* base, Doorbell doorbell) {
  ring_submission_detail::RingPlan plan;
  if (!ring_submission_detail::PlanRing(ctx.r3.u32, ctx.r1.u32, ctx.r4.u32, ctx.r5.u64, base, plan))
    return false;
  ring_submission_detail::ApplyRing(ctx, base, plan, doorbell);
  return true;
}

template <typename Doorbell>
inline bool SubmitCommandBuffer(PPCContext& ctx, uint8_t* base, Doorbell doorbell) {
  ring_submission_detail::SubmitPlan plan;
  if (!ring_submission_detail::PlanSubmit(ctx, base, plan))
    return false;
  ring_submission_detail::ApplySubmit(ctx, base, plan, doorbell);
  return true;
}
}  // namespace simpsons::native_gpu_vertices
