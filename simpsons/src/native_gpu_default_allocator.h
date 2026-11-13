#pragma once

#include <algorithm>
#include <bit>

#include "native_gpu_recorded_allocator.h"

namespace simpsons::native_gpu_vertices {
namespace default_allocator_detail {
using namespace native_gpu_memory;
using recorded_allocator_detail::Store64;

struct Plan {
  uint32_t device = 0, parameter = 0, stack = 0;
  uint32_t first = 0, end = 0, heap = 0, version = 0;
  bool temporary = false, wrapped = false, failed = false;
};

inline bool BuildPlan(const uint8_t* base, uint32_t device, uint32_t parameter, uint32_t bytes,
                      bool temporary, uint64_t alignment, uint32_t stack,
                      const MemoryRange& caller_frame, Plan& plan) {
  MemoryRange dev, frame, input, owner;
  if (!base || ((device | parameter) & 3) || (stack & 15) || stack < 256 ||
      !BackingRange(device, 22256, dev) || !BackingRange(stack - 256, 256, frame) ||
      !BackingRange(parameter, 4, input) || Overlaps(dev, frame) || Overlaps(dev, input) ||
      Overlaps(input, frame) || Overlaps(dev, caller_frame) || LoadGuestU32(base, device + 13492))
    return false;
  const uint32_t heap = LoadGuestU32(base, device + 14880);
  const uint32_t limit = LoadGuestU32(base, device + 14884);
  if (!heap || ((heap | limit) & 3) || limit <= heap || limit - heap > INT32_MAX ||
      !BackingRange(heap, uint64_t(limit) - heap, owner) || Overlaps(owner, dev) ||
      Overlaps(owner, frame) || Overlaps(owner, input) || Overlaps(owner, caller_frame))
    return false;
  if (temporary && (!alignment || alignment > UINT32_MAX || !std::has_single_bit(alignment)))
    return false;
  const uint32_t reservation = LoadGuestU32(base, device + 13216);
  if (reservation && ((reservation & 3) || reservation < heap || reservation > limit))
    return false;
  const uint32_t reserved_version = reservation ? LoadGuestU32(base, device + 13220) : 0;
  const uint32_t version = LoadGuestU32(base, device + 14896);
  const uint32_t tail = LoadGuestU32(base, device + 14892);
  plan.device = device;
  plan.parameter = parameter;
  plan.stack = stack;
  plan.heap = heap;
  plan.version = version;
  plan.temporary = temporary;
  uint64_t first, end;
  if (temporary) {
    const uint64_t cursor = tail ? tail : uint64_t(LoadGuestU32(base, device + 52)) + 60;
    first = (cursor + alignment - 1) & ~(alignment - 1);
    end = first + (bytes & ~3u);
    if (first > UINT32_MAX || end > UINT32_MAX)
      return false;
    if (end > limit) {
      const uint32_t wrap = LoadGuestU32(base, device + 14900);
      first = (uint64_t(wrap) + alignment - 1) & ~(alignment - 1);
      end = first + (bytes & ~3u);
      if (first > UINT32_MAX || end > UINT32_MAX)
        return false;
      plan.wrapped = true;
      const uint32_t delta = reserved_version - version - 1;
      plan.failed = reservation && (delta == UINT32_MAX || (!delta && reservation < end));
      plan.failed |= end > LoadGuestU32(base, device + 14904);
    } else {
      const uint32_t delta = reserved_version - version;
      plan.failed = reservation && (delta == UINT32_MAX || (!delta && reservation < end));
    }
  } else {
    first = ((tail ? uint64_t(tail) : uint64_t(LoadGuestU32(base, device + 48)) + 4) + 31) &
            ~uint64_t(31);
    uint32_t words = bytes >> 2;
    if (!words)
      words = 54;
    const uint32_t chunk = std::max(words, LoadGuestU32(base, device + 14888) >> 2) * 4;
    if (first > limit)
      return false;
    end = std::min(first + chunk, uint64_t(limit));
    if ((end - first) / 4 < words) {
      first = (uint64_t(LoadGuestU32(base, device + 14900)) + 31) & ~uint64_t(31);
      end = first + chunk;
      plan.wrapped = true;
      ++plan.version;
    }
    if (first > UINT32_MAX || end > UINT32_MAX)
      return false;
    const uint32_t delta = reserved_version - plan.version;
    if (reservation && (delta == UINT32_MAX || (!delta && reservation < end))) {
      end = reservation;
      plan.failed = plan.version != reserved_version || end < first || (end - first) / 4 < words;
    }
  }
  // A failed command wrap still publishes its generation and wrap flag. A
  // failed temporary allocation publishes neither cursor; preserve both cases.
  plan.first = uint32_t(first);
  plan.end = uint32_t(end);
  if (plan.failed)
    return true;
  if (first < heap || end < first || end > limit)
    return false;
  MemoryRange sync;
  const uint32_t completed = LoadGuestU32(base, device + 10896);
  if (!completed || (completed & 3) || !BackingRange(completed, 8, sync) || Overlaps(sync, dev) ||
      Overlaps(sync, frame) || Overlaps(sync, input) || Overlaps(sync, owner) ||
      Overlaps(sync, caller_frame))
    return false;
  const uint32_t fence = LoadGuestU32(base, completed + 4);
  const uint32_t expected = temporary && plan.wrapped ? version + 1 : plan.version;
  const uint32_t distance = (expected - fence) & 3;
  // Exactly 824564D0's non-waiting predicate. A live wait retains the original
  // allocator before any allocation, generation or guest-frame write occurs.
  return !distance || (distance == 1 && end <= (fence & ~3u));
}

inline void ApplySaved(uint8_t* base, const Plan& plan, uint64_t parameter, uint64_t incoming_r29,
                       uint64_t incoming_r30, uint64_t incoming_r31, uint32_t incoming_lr) {
  const uint32_t sp = plan.stack, device = plan.device;
  Store64(base, sp - 32, incoming_r29);
  Store64(base, sp - 24, incoming_r30);
  Store64(base, sp - 16, incoming_r31);
  StoreGuestU32(base, sp - 8, incoming_lr);
  StoreGuestU32(base, sp - 112, sp);
  if (!plan.temporary && plan.wrapped) {
    StoreGuestU32(base, device + 14900, plan.heap);
    StoreGuestU32(base, device + 14896, plan.version);
    *GuestToHost(base, device + 10942) |= 4;
  }
  if (plan.failed)
    return;
  if (plan.temporary)
    StoreGuestU32(base, device + (plan.wrapped ? 14900 : 14892), plan.end);
  // Retain the real ready child call's saved registers and private frame.
  // Its original body is independently executed by the reference fixture.
  Store64(base, sp - 144, parameter);
  Store64(base, sp - 136, plan.temporary ? incoming_r30 : plan.first);
  Store64(base, sp - 128, plan.temporary ? plan.first : plan.end);
  StoreGuestU32(base, sp - 120, plan.temporary ? 0x82456C50 : 0x82456D40);
  StoreGuestU32(base, sp - 256, sp - 112);
  if (!plan.temporary)
    StoreGuestU32(base, plan.parameter, (plan.end - plan.first) & ~3u);
}

inline void Apply(const PPCContext& ctx, uint8_t* base, const Plan& plan, uint64_t parameter,
                  uint64_t incoming_r31, uint32_t incoming_lr) {
  ApplySaved(base, plan, parameter, ctx.r29.u64, ctx.r30.u64, incoming_r31, incoming_lr);
}
}  // namespace default_allocator_detail

inline bool AllocateDefaultStorage(PPCContext& ctx, uint8_t* base) {
  using namespace default_allocator_detail;
  MemoryRange parameter;
  if (!base || (ctx.r5.u32 & 3) || !BackingRange(ctx.r5.u32, 4, parameter))
    return false;
  Plan plan;
  if (!BuildPlan(base, ctx.r3.u32, ctx.r5.u32, LoadGuestU32(base, ctx.r5.u32),
                 (ctx.r4.u32 & 1) != 0, ctx.r6.u64, ctx.r1.u32, {}, plan))
    return false;
  Apply(ctx, base, plan, ctx.r5.u64, ctx.r31.u64, uint32_t(ctx.lr));
  ctx.r3.u64 = plan.failed ? 0 : plan.first;
  ctx.lr = uint32_t(ctx.lr);
  return true;
}

namespace default_allocator_detail {
struct TransientPlan {
  Plan allocation;
  uint32_t device = 0, stack = 0, bytes = 0, budget = 0, cursor = 0;
  uint8_t flags = 0;
  bool throttle = false, reset_tail = false;
};

inline bool PlanTransient(const uint8_t* base, uint32_t device, uint32_t sp, uint32_t dwords,
                          uint64_t alignment, TransientPlan& plan) {
  MemoryRange dev, frame;
  if (!base || (device & 3) || !BackingRange(device, 22256, dev))
    return false;
  plan.flags = *GuestToHost(base, device + 10941);
  if ((plan.flags & 0x20) || LoadGuestU32(base, device + 13492))
    return false;
  if ((sp & 15) || sp < 368 || !BackingRange(sp - 112, 112, frame) || Overlaps(frame, dev))
    return false;
  plan.device = device;
  plan.stack = sp;
  plan.bytes = dwords << 2;
  if (!BuildPlan(base, device, sp - 32, plan.bytes, true, alignment, sp - 112, frame,
                 plan.allocation))
    return false;
  plan.budget = LoadGuestU32(base, device + 14908) + plan.bytes;
  plan.throttle =
      plan.budget > LoadGuestU32(base, device + 14912) && !LoadGuestU32(base, device + 13216);
  plan.cursor = LoadGuestU32(base, device + 48);
  const uint32_t wrap = plan.allocation.wrapped && !plan.allocation.failed
                            ? plan.allocation.end
                            : LoadGuestU32(base, device + 14900);
  plan.reset_tail = wrap != plan.allocation.heap;
  return !plan.throttle || uint64_t(plan.cursor) + 160 <= UINT32_MAX;
}

inline void ApplyTransient(PPCContext& ctx, uint8_t* base, const TransientPlan& plan) {
  const uint32_t sp = plan.stack, device = plan.device, parameter = sp - 32;
  StoreGuestU32(base, sp - 8, uint32_t(ctx.lr));
  recorded_allocator_detail::Store64(base, sp - 16, ctx.r31.u64);
  StoreGuestU32(base, sp - 112, sp);
  StoreGuestU32(base, parameter, plan.bytes);
  Apply(ctx, base, plan.allocation, ctx.r1.u64 - 32, ctx.r3.u64, 0x82456DB0);
  StoreGuestU32(base, device + 14908, plan.budget);
  if (plan.throttle) {
    StoreGuestU32(base, device + 56, plan.cursor);
    StoreGuestU32(base, device + 52, plan.cursor + 160);
    if (plan.reset_tail)
      StoreGuestU32(base, device + 14892, LoadGuestU32(base, device + 14884));
  }
  if (plan.allocation.failed)
    *GuestToHost(base, device + 10941) = plan.flags | 0x20;
  ctx.r3.u64 = plan.allocation.failed ? 0 : plan.allocation.first;
  ctx.lr = uint32_t(ctx.lr);
}
}  // namespace default_allocator_detail

inline bool AllocateTransient(PPCContext& ctx, uint8_t* base) {
  using namespace default_allocator_detail;
  MemoryRange device;
  if (!base || (ctx.r3.u32 & 3) || !BackingRange(ctx.r3.u32, 22256, device))
    return false;
  if ((*GuestToHost(base, ctx.r3.u32 + 10941) & 0x20) || LoadGuestU32(base, ctx.r3.u32 + 13492))
    return AllocateRecordedTransient(ctx, base);
  TransientPlan plan;
  if (!PlanTransient(base, ctx.r3.u32, ctx.r1.u32, ctx.r4.u32, ctx.r5.u64, plan))
    return false;
  ApplyTransient(ctx, base, plan);
  return true;
}

}  // namespace simpsons::native_gpu_vertices
