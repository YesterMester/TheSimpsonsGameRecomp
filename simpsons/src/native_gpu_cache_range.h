#pragma once

#include <rex/ppc/barriers.h>

#include "guest_memory.h"
#include "native_gpu_memory.h"

namespace simpsons::native_gpu_vertices {
namespace cache_range_detail {
struct Plan {
  uint32_t stack = 0, first = 0, last = 0, lines = 0;
  bool active = false;
};

inline bool Build(uint32_t first, uint32_t last, uint32_t stack, const uint8_t* base, Plan& plan) {
  plan.active = first - 0x7F100000u > 0x07EFFFFFu;
  if (!plan.active)
    return true;  // This original early return does not touch the stack.
  native_gpu_memory::MemoryRange frame;
  plan.stack = stack;
  plan.first = first & ~127u;
  plan.last = (last + 127) & ~127u;
  const uint32_t bytes = plan.last - plan.first;
  if (!base || (stack & 15) || stack < 32 ||
      !native_gpu_memory::BackingRange(stack - 32, 32, frame) || bytes > INT32_MAX)
    return false;  // Reversed or ambiguous ranges retain the original routine.
  plan.lines = bytes >> 7;
  return true;
}

inline void Apply(PPCContext& ctx, uint8_t* base, const Plan& plan) {
  // dcbf is already a no-op in the recompiled reference. Keep the final guest
  // stack values and the real host fence, without looping over cache lines.
  ctx.r10.u64 = ctx.r3.u64 - 0x7F100000u;
  ctx.r11.u64 = 0x07EFFFFF;
  ctx.cr6.compare<uint32_t>(ctx.r10.u32, ctx.r11.u32, ctx.xer);
  if (!plan.active)
    return;
  const uint32_t groups = plan.lines >> 3, remainder = plan.lines & 7;
  StoreGuestU32(base, plan.stack - 20, plan.first);
  StoreGuestU32(base, plan.stack - 16, plan.last);
  StoreGuestU32(base, plan.stack - 32, plan.last);
  StoreGuestU32(base, plan.stack - 28, groups);
  StoreGuestU32(base, plan.stack - 12, plan.lines);
  StoreGuestU32(base, plan.stack - 8, groups);
  StoreGuestU32(base, plan.stack - 4, remainder);
  StoreGuestU32(base, plan.stack - 24, remainder);
  ctx.r8.u64 = groups;
  ctx.r9.u64 = ctx.r10.u64 = remainder;
  ctx.r11.u64 = uint64_t(plan.first) + uint64_t(plan.lines) * 128;
  if (groups)
    ctx.r7.u64 = 896;
  ctx.xer.ca = 0;
  ctx.cr0.compare<int32_t>(groups, 0, ctx.xer);
  ctx.cr6.compare<uint32_t>(remainder, remainder, ctx.xer);
  REX_PPC_SYNC();
}
}  // namespace cache_range_detail

inline bool FlushCpuCommandRange(PPCContext& ctx, uint8_t* base) {
  cache_range_detail::Plan plan;
  if (!cache_range_detail::Build(ctx.r3.u32, ctx.r4.u32, ctx.r1.u32, base, plan))
    return false;
  cache_range_detail::Apply(ctx, base, plan);
  return true;
}
}  // namespace simpsons::native_gpu_vertices
