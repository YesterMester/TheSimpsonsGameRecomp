#pragma once

#include <bit>
#include <cstring>

#include <rex/ppc.h>

#include "guest_memory.h"
#include "native_gpu_memory.h"

namespace simpsons::native_gpu_vertices {
namespace recorded_allocator_detail {
using namespace native_gpu_memory;

enum class Operation { exhausted, initialize, allocate };
struct Plan {
  Operation operation = Operation::exhausted;
  uint32_t storage = 0, bytes = 0, result = 0, capacity = 0, limit = 0;
};

inline void Store64(uint8_t* base, uint32_t address, uint64_t value) {
  value = std::byteswap(value);
  std::memcpy(GuestToHost(base, address), &value, sizeof(value));
}

// The recording object retains this allocation. Do not replace an unknown
// heap, a wrapped address or an overlapping physical view with guessed storage.
// Planning never changes guest state, including the caller's size parameter.
inline bool BuildPlan(const uint8_t* base, uint32_t device, uint32_t parameter, uint32_t bytes,
                      bool temporary, uint64_t alignment, const MemoryRange& protected_range,
                      Plan& plan) {
  MemoryRange whole_device, object, owner, size_parameter;
  if (!base || (device & 3) || !BackingRange(device, 22256, whole_device) ||
      Overlaps(whole_device, protected_range))
    return false;
  const uint32_t limit = LoadGuestU32(base, device + 56);
  // The original early failure does not dereference the recording or parameter.
  if (!temporary && limit)
    return true;
  const uint32_t recording = LoadGuestU32(base, device + 13492);
  if (!recording || (recording & 3) || !BackingRange(recording, 184, object) ||
      Overlaps(object, whole_device) || Overlaps(object, protected_range) || (parameter & 3) ||
      !BackingRange(parameter, 4, size_parameter) || Overlaps(size_parameter, whole_device) ||
      Overlaps(size_parameter, object) || (LoadGuestU32(base, recording + 108) & 15) != 2)
    return false;
  plan.storage = LoadGuestU32(base, recording + 152);
  plan.bytes = LoadGuestU32(base, recording + 156);
  const uint64_t end = uint64_t(plan.storage) + plan.bytes;
  if (!plan.storage || !plan.bytes || ((plan.storage | plan.bytes) & 3) || end > UINT32_MAX ||
      !BackingRange(plan.storage, plan.bytes, owner) || Overlaps(owner, object) ||
      Overlaps(owner, whole_device) || Overlaps(owner, size_parameter) ||
      Overlaps(owner, protected_range))
    return false;
  if (!temporary) {
    plan.operation = Operation::initialize;
    plan.result = plan.storage;
    return true;
  }
  if (!alignment || alignment > UINT32_MAX || !std::has_single_bit(alignment))
    return false;
  uint32_t tail = LoadGuestU32(base, device + 14892);
  if (!tail)
    tail = uint32_t(end);
  const uint32_t cursor = LoadGuestU32(base, device + 48);
  const uint32_t requested = bytes & ~3u;
  if ((tail | cursor) & 3 || tail < plan.storage || tail > end || requested > tail ||
      uint64_t(cursor) + 4 < plan.storage || uint64_t(cursor) + 164 > end)
    return false;
  const uint32_t first = uint32_t((uint64_t(tail) - requested) & ~(alignment - 1));
  if (first < cursor + 164)
    return true;
  const uint32_t consumed = tail - first;
  const uint32_t capacity = LoadGuestU32(base, device + 52);
  if (first < plan.storage || ((capacity | limit) & 3) || consumed > capacity || consumed > limit)
    return false;
  plan.operation = Operation::allocate;
  plan.result = first;
  plan.capacity = capacity - consumed;
  plan.limit = limit - consumed;
  return true;
}

inline void Apply(const Plan& plan, uint8_t* base, uint32_t device, uint32_t parameter) {
  if (plan.operation == Operation::initialize) {
    StoreGuestU32(base, device + 14904, plan.storage);
    StoreGuestU32(base, parameter, plan.bytes);
  } else if (plan.operation == Operation::allocate) {
    StoreGuestU32(base, device + 14892, plan.result);
    StoreGuestU32(base, device + 52, plan.capacity);
    StoreGuestU32(base, device + 56, plan.limit);
  }
}
}  // namespace recorded_allocator_detail

// 82458E60: command storage is handed out once; temporary data grows down from
// its retained end, leaving the same 164-byte guard above command emission.
inline bool AllocateRecordedStorage(PPCContext& ctx, uint8_t* base) {
  using namespace recorded_allocator_detail;
  const uint32_t device = ctx.r3.u32, parameter = ctx.r5.u32;
  const bool temporary = (ctx.r4.u32 & 1) != 0;
  MemoryRange input;
  if (!base || (temporary && ((parameter & 3) || !BackingRange(parameter, 4, input))))
    return false;
  const uint32_t bytes = temporary ? LoadGuestU32(base, parameter) : 0;
  Plan plan;
  if (!BuildPlan(base, device, parameter, bytes, temporary, ctx.r6.u64, {}, plan))
    return false;
  Apply(plan, base, device, parameter);
  ctx.r3.u64 = plan.result;
  return true;
}

// 82456D60's fixed-owner branch, including its original stack writes, wrapped
// byte accounting, budget throttling and sticky failure flag. Preflight covers
// all effects; there is no fallback after an allocation has been published.
namespace recorded_allocator_detail {
struct TransientPlan {
  Plan allocation;
  uint32_t device = 0, stack = 0, bytes = 0, budget = 0, tail = 0, cursor = 0;
  uint8_t flags = 0;
  bool throttle = false, reset_tail = false;
};

inline bool PlanTransient(const uint8_t* base, uint32_t device, uint32_t sp, uint32_t dwords,
                          uint64_t alignment, TransientPlan& plan) {
  MemoryRange whole_device, frame;
  if (!base || (device & 3) || (sp & 15) || sp < 112 ||
      !BackingRange(device, 22256, whole_device) || !BackingRange(sp - 112, 112, frame) ||
      Overlaps(whole_device, frame))
    return false;
  plan.device = device;
  plan.stack = sp;
  plan.flags = *GuestToHost(base, device + 10941);
  plan.bytes = dwords << 2;
  if (!(plan.flags & 0x20)) {
    if (!BuildPlan(base, device, sp - 32, plan.bytes, true, alignment, frame, plan.allocation))
      return false;
    plan.budget = LoadGuestU32(base, device + 14908) + plan.bytes;
    plan.throttle =
        plan.budget > LoadGuestU32(base, device + 14912) && !LoadGuestU32(base, device + 13216);
    if (plan.throttle) {
      plan.cursor = LoadGuestU32(base, device + 48);
      plan.reset_tail = LoadGuestU32(base, device + 14900) != LoadGuestU32(base, device + 14880);
      if (plan.reset_tail)
        plan.tail = LoadGuestU32(base, device + 14884);
    }
  }
  return true;
}

inline void ApplyTransient(PPCContext& ctx, uint8_t* base, const TransientPlan& plan) {
  const uint32_t sp = plan.stack, device = plan.device, parameter = sp - 32;
  StoreGuestU32(base, sp - 8, uint32_t(ctx.lr));
  recorded_allocator_detail::Store64(base, sp - 16, ctx.r31.u64);
  StoreGuestU32(base, sp - 112, sp);
  if (!(plan.flags & 0x20)) {
    StoreGuestU32(base, parameter, plan.bytes);
    Apply(plan.allocation, base, device, parameter);
    StoreGuestU32(base, device + 14908, plan.budget);
    if (plan.throttle) {
      StoreGuestU32(base, device + 56, plan.cursor);
      StoreGuestU32(base, device + 52, plan.cursor + 160);
      if (plan.reset_tail)
        StoreGuestU32(base, device + 14892, plan.tail);
    }
    if (!plan.allocation.result)
      *GuestToHost(base, device + 10941) = plan.flags | 0x20;
  }
  ctx.r3.u64 = plan.allocation.result;
  ctx.lr = uint32_t(ctx.lr);
}
}  // namespace recorded_allocator_detail

inline bool AllocateRecordedTransient(PPCContext& ctx, uint8_t* base) {
  recorded_allocator_detail::TransientPlan plan;
  if (!recorded_allocator_detail::PlanTransient(base, ctx.r3.u32, ctx.r1.u32, ctx.r4.u32,
                                                ctx.r5.u64, plan))
    return false;
  recorded_allocator_detail::ApplyTransient(ctx, base, plan);
  return true;
}

}  // namespace simpsons::native_gpu_vertices
