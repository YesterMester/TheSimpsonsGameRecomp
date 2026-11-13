#pragma once

#include "native_gpu_constants.h"
#include "native_gpu_state.h"
#include "native_gpu_samplers.h"
#include "native_gpu_vertices.h"
#include "native_gpu_conditional.h"

namespace simpsons::native_gpu_flush {

struct FlushPlan {
  uint32_t device, initial, cursor;
  std::array<uint64_t, 5> dirty;
  std::array<native_gpu_constants::ConstantsPlan, 2> constants;
  std::array<native_gpu_state::StatePlan, 9> state;
  std::array<bool, 9> used{};
  native_gpu_samplers::SamplerPlan samplers;
  bool use_samplers = false;
  native_gpu_state::ConditionalPlan conditional;
  bool use_conditional = false;
};

// The same ordered shadow-state flush precedes the normal and vertex-ID draw
// families. Shader/declaration fixups, resource-dependent rewrites
// and ring growth are separate contracts. Reject them before the first write.
inline bool PlanFlush(const PPCContext& ctx, const uint8_t* base, uint32_t caller_frame,
                      const std::array<uint64_t, 5>& dirty, FlushPlan& plan, bool samplers = false,
                      bool conditional = false) {
  using namespace native_gpu_memory;
  const uint32_t device = ctx.r3.u32, frame = caller_frame + (conditional ? 192 : 176);
  constexpr uint64_t render_mask = ((uint64_t(1) << 58) - 1) & ~uint64_t(0x1E0000);
  constexpr uint64_t fetch_mask = 0x007FFFFCFFFFFFFFull;
  const uint64_t misc_mask =
      ((uint64_t(1) << 46) - 1) | (uint64_t(1) << 56) | (samplers ? (uint64_t(63) << 49) : 0);
  MemoryRange dev, stack, commands;
  if (!base || (device & 3) || (ctx.r1.u32 & 15) || ctx.r1.u32 < frame ||
      !BackingRange(device, 14916, dev) || !BackingRange(ctx.r1.u32 - frame, frame, stack) ||
      Overlaps(dev, stack) || (dirty[2] & ~render_mask) || (dirty[3] & ~fetch_mask) ||
      (dirty[4] & ~misc_mask) ||
      (!conditional && (dirty[2] & native_gpu_vertices::Load64(base, device + 40))))
    return false;
  const uint32_t initial = LoadGuestU32(base, device + 48);
  if ((initial & 3) || initial > LoadGuestU32(base, device + 56))
    return false;
  plan.device = device;
  plan.initial = plan.cursor = initial;
  plan.dirty = dirty;
  plan.used.fill(false);
  plan.use_samplers = false;
  plan.use_conditional = false;
  PPCContext work = ctx;
  work.r1.u32 -= caller_frame;
  for (uint32_t bank = 0; bank < 2; ++bank) {
    if (!dirty[bank])
      continue;
    work.r4.u64 = dirty[bank];
    work.r5.u64 = bank ? 0x4400 : 0x4000;
    work.r6.u64 = device + (bank ? 6016 : 1920);
    if (!native_gpu_constants::PlanConstantsAt(work, base, plan.cursor, plan.constants[bank]))
      return false;
    plan.cursor = plan.constants[bank].cursor;
  }
  uint64_t render = dirty[2];
  if (render & native_gpu_vertices::Load64(base, device + 40)) {
    work.r4.u64 = render;
    work.r5.u64 = LoadGuestU32(base, device + 10560);
    if (!native_gpu_state::PlanConditionalAt(work, base, plan.cursor, plan.conditional))
      return false;
    plan.use_conditional = true;
    plan.cursor = plan.conditional.cursor;
    render = plan.conditional.remaining;
  }
  const auto state = [&](uint32_t slot, uint64_t mask, uint32_t first, uint32_t offset,
                         bool fetch = false) {
    if (!mask)
      return true;
    work.r4.u64 = mask;
    work.r5.u64 = first;
    work.r6.u64 = device + offset;
    if (!native_gpu_state::PlanStateAt(work, base, fetch, plan.cursor, plan.state[slot]))
      return false;
    plan.used[slot] = true;
    plan.cursor = plan.state[slot].cursor + (fetch ? 16 : 0);
    return true;
  };
  // Keep the generated call order, including the nonascending register pages.
  if (!state(0, std::rotl(render, 52) & 0xFFF0000000000000ull, 0x2200, 10548) ||
      !state(1, std::rotl(render, 47) & 0xF800000000000000ull, 0x2180, 10528) ||
      !state(2, std::rotl(render, 6) & 0xFFFF000000000000ull, 0x2000, 10368) ||
      !state(3, std::rotl(render, 22) & 0xFFFFF80000000000ull, 0x2100, 10444) ||
      !state(4, std::rotl(dirty[3], 32) & 0xFFFFFFFF00000000ull, 0x4800, 1152, true) ||
      !state(5, std::rotl(dirty[3], 9) & 0xFFFFF80000000000ull, 0x2280, 10596) ||
      !state(6, std::rotl(dirty[4], 26) & 0xFFFFFFFFFC000000ull, 0x2300, 10680) ||
      !state(7, std::rotl(dirty[4], 18) & 0xFF00000000000000ull, 0x2380, 10832))
    return false;
  if (dirty[4] & (uint64_t(63) << 49)) {
    work.r4.u64 = std::rotl(dirty[4], 9) & 0xFC00000000000000ull;
    if (!native_gpu_samplers::PlanSamplersAt(work, base, plan.cursor, plan.samplers))
      return false;
    plan.use_samplers = true;
    plan.cursor = plan.samplers.state.cursor;
  }
  if (!state(8, dirty[4] & (uint64_t(1) << 56) ? 0xFFFFFFFFFF000000ull : 0, 0x4900, 10112))
    return false;
  // The complete output must be separate from every device shadow, not just
  // the cursor fields protected by an individual helper's local preflight.
  return plan.cursor == initial ||
         (BackingRange(initial + 4, uint64_t(plan.cursor) - initial, commands) &&
          !Overlaps(commands, dev) && !Overlaps(commands, stack));
}

inline std::array<uint64_t, 5> DirtyState(const uint8_t* base, uint32_t device) {
  std::array<uint64_t, 5> dirty;
  for (uint32_t i = 0; i < dirty.size(); ++i)
    dirty[i] = native_gpu_vertices::Load64(base, device + i * 8);
  return dirty;
}

inline void ApplyFlush(const FlushPlan& plan, uint8_t* base) {
  const auto clear = [&](uint32_t group) {
    if (plan.dirty[group])
      native_gpu_vertices::Store64(base, plan.device + group * 8, 0);
  };
  for (uint32_t bank = 0; bank < 2; ++bank) {
    if (plan.dirty[bank])
      native_gpu_constants::ApplyConstants(plan.constants[bank], base);
    clear(bank);
  }
  if (plan.use_conditional)
    native_gpu_state::ApplyConditional(plan.conditional, base);
  for (uint32_t i = 0; i < plan.state.size(); ++i) {
    if (i == 8 && plan.use_samplers)
      native_gpu_samplers::ApplySamplers(plan.samplers, base);
    if (plan.used[i])
      native_gpu_state::ApplyState(plan.state[i], base, i == 4);
    if (i == 3)
      clear(2);
    if (i == 5)
      clear(3);
  }
  clear(4);
}

}  // namespace simpsons::native_gpu_flush
