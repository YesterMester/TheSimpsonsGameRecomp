#pragma once

#include <array>
#include <bit>
#include <cstdint>
#include <cstring>

#include <rex/graphics/native_gpu_state.h>
#include <rex/ppc/context.h>

#include "guest_memory.h"
#include "native_gpu_memory.h"

namespace simpsons::native_gpu_state {

struct StateRun {
  uint32_t cursor, padding, source, count, header;
};
struct StatePlan {
  uint32_t device, initial, cursor, run_count, register_end;
  std::array<StateRun, 32> runs;
};

// Plan every run before writing. Header spans/lengths and payload locations
// match the original state/fetch writers, including alignment and the flush.
inline bool PlanStateAt(const PPCContext& ctx, const uint8_t* base, bool fetch, uint32_t initial,
                        StatePlan& plan) {
  using namespace native_gpu_memory;
  const uint32_t device = ctx.r3.u32;
  const uint64_t dirty = ctx.r4.u64;
  MemoryRange dev, stack, destination;
  if (!base || !dirty || (device & 3) || (ctx.r1.u32 & 15) || ctx.r1.u32 < 144 ||
      !BackingRange(device, 56, dev) || !BackingRange(ctx.r1.u32 - 144, 144, stack) ||
      Overlaps(dev, stack))
    return false;
  const uint32_t limit = LoadGuestU32(base, device + 52);
  if ((initial & 3) || (!fetch && (ctx.r6.u32 & 3)))
    return false;
  const uint32_t first_register = fetch ? 0x4800 : ctx.r5.u32;
  const uint64_t shadow = fetch ? uint64_t(device) + 1152 : ctx.r6.u32;
  const uint32_t per_bit = fetch ? 6 : 1;
  plan = {device, initial, initial, 0, first_register, {}};
  uint32_t bit = 0;
  uint64_t remaining = dirty;
  while (remaining) {
    const uint32_t gap = uint32_t(std::countl_zero(remaining));
    remaining <<= gap;
    bit += gap;
    const uint32_t groups = uint32_t(std::countl_one(remaining)), count = groups * per_bit;
    // These are the original fast-path capacity comparisons. Preserve their
    // strict boundary and reject all slow allocation/ring-growth paths first.
    if (uint64_t(plan.cursor) + uint64_t(count + (fetch ? 5 : 0)) * 4 >= limit)
      return false;
    const uint32_t padding = fetch ? ((plan.cursor & 7) / 4) : 0;
    const uint64_t source = shadow + uint64_t(bit) * per_bit * 4;
    const uint64_t end = uint64_t(plan.cursor) + uint64_t(padding + 1 + count) * 4;
    uint32_t header;
    if (source + uint64_t(count) * 4 > 0x100000000ull || end > UINT32_MAX ||
        !rex::graphics::native_gpu_state::EncodeHeader(first_register + bit * per_bit, count,
                                                       header))
      return false;
    plan.runs[plan.run_count++] = {plan.cursor, padding, uint32_t(source), count, header};
    plan.cursor = uint32_t(end);
    bit += groups;
    plan.register_end = first_register + bit * per_bit;
    remaining = groups == 64 ? 0 : remaining << groups;
  }
  const uint64_t end = uint64_t(plan.cursor) + (fetch ? 16 : 0);
  if (end > UINT32_MAX || !BackingRange(initial + 4, end - initial, destination) ||
      Overlaps(destination, dev) || Overlaps(destination, stack))
    return false;
  for (uint32_t i = 0; i < plan.run_count; ++i) {
    const auto& run = plan.runs[i];
    MemoryRange source;
    if (!BackingRange(run.source, uint64_t(run.count) * 4, source) ||
        Overlaps(source, destination) || Overlaps(source, dev) || Overlaps(source, stack))
      return false;
  }
  return true;
}

inline bool PlanState(const PPCContext& ctx, const uint8_t* base, bool fetch, StatePlan& plan) {
  native_gpu_memory::MemoryRange device;
  if (!base || !native_gpu_memory::BackingRange(ctx.r3.u32, 56, device))
    return false;
  return PlanStateAt(ctx, base, fetch, LoadGuestU32(base, ctx.r3.u32 + 48), plan);
}

inline void ApplyState(const StatePlan& plan, uint8_t* base, bool fetch) {
  for (uint32_t i = 0; i < plan.run_count; ++i) {
    const auto& run = plan.runs[i];
    uint32_t cursor = run.cursor;
    if (run.padding) {
      StoreGuestU32(base, cursor + 4, 0x80000000u);
      cursor += 4;
    }
    StoreGuestU32(base, cursor + 4, run.header);
    std::memcpy(GuestToHost(base, cursor + 8), GuestToHost(base, run.source),
                size_t(run.count) * 4);
  }
  if (fetch) {
    uint32_t header;
    rex::graphics::native_gpu_state::EncodeHeader(0x5000, 3, header);
    StoreGuestU32(base, plan.cursor + 4, header);
    // The actual fast path writes two identical 64-bit values. The second
    // 0x25000 is payload, not another packet header. Keep those raw bits.
    StoreGuestU32(base, plan.cursor + 8, 0);
    StoreGuestU32(base, plan.cursor + 12, 0x25000);
    StoreGuestU32(base, plan.cursor + 16, 0);
  }
  StoreGuestU32(base, plan.device + 48, plan.cursor + (fetch ? 16 : 0));
}

inline bool WriteState(PPCContext& ctx, uint8_t* base, bool fetch) {
  StatePlan plan;
  if (!PlanState(ctx, base, fetch, plan))
    return false;
  ApplyState(plan, base, fetch);
  ctx.r4.u64 = plan.cursor;
  if (!fetch)
    ctx.r5.u64 += plan.register_end - ctx.r5.u32;
  ctx.lr = uint32_t(ctx.lr);
  return true;
}

}  // namespace simpsons::native_gpu_state
