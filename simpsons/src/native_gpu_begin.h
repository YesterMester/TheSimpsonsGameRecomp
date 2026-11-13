#pragma once

#include "native_gpu_indexed.h"

namespace simpsons::native_gpu_vertices {

// Keep the prepared path short. A rejected prepared plan has no side effects;
// only then try the complete dirty-state/allocator/draw preflight. An actual
// allocation failure is handled once and is never retried through a fallback.
template <typename Allocate>
bool BeginWithState(PPCContext& ctx, uint8_t* base, Allocate allocate, bool indexed,
                    bool state_enabled, bool samplers, bool conditional = false) {
  if (indexed ? BeginIndexedVertices(ctx, base, allocate) : BeginVertices(ctx, base, allocate))
    return true;
  native_gpu_memory::MemoryRange device;
  if (!state_enabled || !base || (ctx.r3.u32 & 3) ||
      !native_gpu_memory::BackingRange(ctx.r3.u32, 14916, device))
    return false;
  auto dirty = native_gpu_flush::DirtyState(base, ctx.r3.u32);
  // Auto Begin discards the previous temporary vertex fetch bit. Indexed
  // Begin flushes it along with the other fetch shadows instead.
  if (!indexed)
    dirty[3] &= ~uint64_t(1);
  native_gpu_flush::FlushPlan flush;
  if (!native_gpu_flush::PlanFlush(ctx, base, indexed ? 240 : 176, dirty, flush, samplers,
                                   conditional))
    return false;
  const BeginSubmission submission{flush.initial, flush.cursor};
  BeginPlan auto_plan;
  IndexedBeginPlan indexed_plan;
  if (indexed ? !PlanIndexedBegin(ctx, base, indexed_plan, &submission)
              : !PlanBegin(ctx, base, auto_plan, &submission))
    return false;
  if (!indexed)
    Store64(base, ctx.r3.u32 + 24, dirty[3]);
  native_gpu_flush::ApplyFlush(flush, base);
  return indexed ? BeginIndexedVerticesWithPlan(ctx, base, indexed_plan, allocate)
                 : BeginVerticesWithPlan(ctx, base, auto_plan, allocate);
}

}  // namespace simpsons::native_gpu_vertices
