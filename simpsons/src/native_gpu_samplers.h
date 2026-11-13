#pragma once

#include <rex/graphics/native_gpu_coherency.h>

#include "native_gpu_state.h"

namespace simpsons::native_gpu_samplers {

struct SamplerPlan {
  uint32_t device, initial;
  std::array<uint32_t, 13> coherency;
  native_gpu_state::StatePlan state;
};

// This helper's wait precedes the aligned four-word sampler ranges. Keep
// both in the ordered buffer; sampler copies alone would lose that boundary.
inline bool PlanSamplersAt(const PPCContext& ctx, const uint8_t* base, uint32_t initial,
                           SamplerPlan& plan) {
  using namespace native_gpu_memory;
  const uint32_t device = ctx.r3.u32;
  const uint64_t dirty = ctx.r4.u64;
  MemoryRange dev, stack, commands;
  if (!base || !dirty || (dirty & 0x3FFFFFFFFull) || (device & 3) || (initial & 3) ||
      (ctx.r1.u32 & 15) || ctx.r1.u32 < 160 || !BackingRange(device, 14916, dev) ||
      !BackingRange(ctx.r1.u32 - 160, 160, stack) || Overlaps(dev, stack) ||
      initial > LoadGuestU32(base, device + 56) || uint64_t(initial) + 52 > UINT32_MAX)
    return false;
  plan.device = device;
  plan.initial = initial;
  plan.coherency = rex::graphics::native_gpu_coherency::Encode(
      {LoadGuestU32(base, device + 10396), 0x10000, 0, 0x1000});
  auto& state = plan.state;
  state = {device, initial + 52, initial + 52, 0, 0x2388, {}};
  const uint32_t limit = LoadGuestU32(base, device + 52);
  uint32_t bit = 0;
  uint64_t remaining = dirty;
  while (remaining) {
    const uint32_t gap = uint32_t(std::countl_zero(remaining));
    remaining <<= gap;
    bit += gap;
    const uint32_t groups = uint32_t(std::countl_one(remaining)), count = groups * 4;
    if (uint64_t(state.cursor) + uint64_t(count) * 4 + 4 >= limit)
      return false;
    const uint32_t padding = (state.cursor & 7) / 4;
    const uint64_t source = uint64_t(device) + 10272 + bit * 16;
    const uint64_t end = uint64_t(state.cursor) + uint64_t(padding + 1 + count) * 4;
    uint32_t header;
    if (source + uint64_t(count) * 4 > 0x100000000ull || end > UINT32_MAX ||
        !rex::graphics::native_gpu_state::EncodeHeader(0x2388 + bit * 4, count, header))
      return false;
    state.runs[state.run_count++] = {state.cursor, padding, uint32_t(source), count, header};
    state.cursor = uint32_t(end);
    bit += groups;
    state.register_end = 0x2388 + bit * 4;
    remaining = groups == 64 ? 0 : remaining << groups;
  }
  return BackingRange(initial + 4, uint64_t(state.cursor) - initial, commands) &&
         !Overlaps(commands, dev) && !Overlaps(commands, stack);
}

inline void ApplySamplers(const SamplerPlan& plan, uint8_t* base) {
  for (uint32_t i = 0; i < plan.coherency.size(); ++i)
    StoreGuestU32(base, plan.initial + 4 + i * 4, plan.coherency[i]);
  StoreGuestU32(base, plan.device + 48, plan.initial + 52);
  native_gpu_state::ApplyState(plan.state, base, false);
}

inline bool WriteSamplers(PPCContext& ctx, uint8_t* base) {
  native_gpu_memory::MemoryRange device;
  if (!base || !native_gpu_memory::BackingRange(ctx.r3.u32, 56, device))
    return false;
  SamplerPlan plan;
  if (!PlanSamplersAt(ctx, base, LoadGuestU32(base, ctx.r3.u32 + 48), plan))
    return false;
  ApplySamplers(plan, base);
  ctx.r3.u64 = 3;
  ctx.r4.u64 = plan.state.cursor;
  ctx.lr = uint32_t(ctx.lr);
  return true;
}

}  // namespace simpsons::native_gpu_samplers
