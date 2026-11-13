#pragma once

#include <array>
#include <bit>
#include <cstdint>
#include <cstring>

#include <rex/graphics/native_gpu_constants.h>
#include <rex/ppc/context.h>

#include "guest_memory.h"
#include "native_gpu_memory.h"

namespace simpsons::native_gpu_constants {

namespace detail {

using native_gpu_memory::BackingRange;
using native_gpu_memory::MemoryRange;
using native_gpu_memory::Overlaps;

struct Run {
  uint32_t cursor;
  uint32_t padding;
  uint32_t source;
  uint32_t register_count;
  uint32_t header;
};

}  // namespace detail

struct ConstantsPlan {
  uint32_t device, cursor, register_end, run_count;
  std::array<detail::Run, 32> runs;
};

// A caller planning several flushes may supply the cursor without publishing
// it. Planning does not modify either guest memory or the context.
inline bool PlanConstantsAt(const PPCContext& ctx, const uint8_t* base, uint32_t initial_cursor,
                            ConstantsPlan& plan) {
  const uint32_t device = ctx.r3.u32;
  const uint64_t dirty = ctx.r4.u64;
  const uint32_t first_register = ctx.r5.u32;
  const uint32_t shadow = ctx.r6.u32;
  if (!base || !dirty || (device & 3) || (shadow & 15) ||
      (first_register != 0x4000 && first_register != 0x4400))
    return false;
  detail::MemoryRange device_range;
  if (!detail::BackingRange(device, 56, device_range))
    return false;
  const uint32_t limit = LoadGuestU32(base, device + 52);
  if (initial_cursor & 3)
    return false;

  auto& runs = plan.runs;
  uint32_t run_count = 0, group = 0, cursor = initial_cursor, register_end = first_register;
  uint64_t remaining = dirty;
  while (remaining) {
    const uint32_t gap = uint32_t(std::countl_zero(remaining));
    remaining <<= gap;
    group += gap;
    const uint32_t groups = uint32_t(std::countl_one(remaining));
    const uint32_t count = groups * 16;
    // This is the generated writer's exact per-run slow-path predicate. Its
    // capacity limit includes the XDK reservation convention, not a byte end.
    if (uint64_t(cursor) + uint64_t(count + 3) * 4 >= limit)
      return false;
    const uint32_t padding = ((8u - (cursor & 15u)) & 15u) / 4;
    const uint64_t source = uint64_t(shadow) + uint64_t(group) * 64;
    const uint64_t end = uint64_t(cursor) + uint64_t(padding + 1 + count) * 4;
    if (source + uint64_t(count) * 4 > 0x100000000ull || end > UINT32_MAX)
      return false;
    uint32_t header;
    if (!rex::graphics::native_gpu_constants::EncodeHeader(first_register + group * 16, count,
                                                           header))
      return false;
    runs[run_count++] = {cursor, padding, uint32_t(source), count, header};
    cursor = uint32_t(end);
    register_end = first_register + (group + groups) * 16;
    group += groups;
    remaining = groups == 64 ? 0 : remaining << groups;
  }

  detail::MemoryRange destination;
  if (!detail::BackingRange(initial_cursor + 4, uint64_t(cursor) - initial_cursor, destination) ||
      detail::Overlaps(destination, device_range))
    return false;
  for (uint32_t i = 0; i < run_count; ++i) {
    detail::MemoryRange source;
    if (!detail::BackingRange(runs[i].source, uint64_t(runs[i].register_count) * 4, source) ||
        detail::Overlaps(source, destination) || detail::Overlaps(source, device_range))
      return false;
  }

  plan.device = device;
  plan.cursor = cursor;
  plan.register_end = register_end;
  plan.run_count = run_count;
  return true;
}

inline void ApplyConstants(const ConstantsPlan& plan, uint8_t* base) {
  for (uint32_t i = 0; i < plan.run_count; ++i) {
    const auto& run = plan.runs[i];
    uint32_t write = run.cursor;
    for (uint32_t pad = 0; pad < run.padding; ++pad) {
      write += 4;
      StoreGuestU32(base, write, 0x80000000u);
    }
    write += 4;
    StoreGuestU32(base, write, run.header);
    std::memcpy(GuestToHost(base, write + 4), GuestToHost(base, run.source),
                size_t(run.register_count) * 4);
  }
  StoreGuestU32(base, plan.device + 48, plan.cursor);
}

// Pure bounded replacement for sub_8245EC68. All runs are planned before the
// first write. Unsupported cases leave memory and the complete context intact,
// so the strong override can execute the original function from its beginning.
inline bool WriteConstants(PPCContext& ctx, uint8_t* base) {
  detail::MemoryRange device;
  if (!base || !detail::BackingRange(ctx.r3.u32, 56, device))
    return false;
  ConstantsPlan plan;
  if (!PlanConstantsAt(ctx, base, LoadGuestU32(base, ctx.r3.u32 + 48), plan))
    return false;
  ApplyConstants(plan, base);
  // The XDK fast path returns these consumed cursors in volatile registers.
  // r3 and r6, the stack/link registers, and all nonvolatile fields are intact.
  ctx.r4.u64 = plan.cursor;
  ctx.r5.u64 += plan.register_end - ctx.r5.u32;
  return true;
}

bool TryConstants(PPCContext& ctx, uint8_t* base);

}  // namespace simpsons::native_gpu_constants
