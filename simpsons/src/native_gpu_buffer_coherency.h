#pragma once

#include <array>
#include <cstring>

#include <rex/graphics/native_gpu_sync.h>
#include <rex/ppc/context.h>

#include "guest_memory.h"
#include "native_gpu_memory.h"

namespace simpsons::native_gpu_calls {
namespace buffer_coherency_detail {
using namespace native_gpu_memory;

struct Plan {
  uint32_t device, cursor, stack, physical, first_page, end_page;
};

inline bool BuildPlan(const PPCContext& ctx, const uint8_t* base, bool writeback, Plan& plan) {
  const uint32_t device = ctx.r3.u32, stack = ctx.r1.u32;
  const uint32_t frame_bytes = writeback ? 128 : 112;
  MemoryRange dev, frame, commands;
  if (!base || (device & 3) || (stack & 15) || stack < frame_bytes ||
      !BackingRange(device, 22256, dev) || !BackingRange(stack - frame_bytes, frame_bytes, frame) ||
      Overlaps(dev, frame))
    return false;
  const uint32_t cursor = LoadGuestU32(base, device + 48);
  const uint32_t limit = LoadGuestU32(base, device + 52);
  const uint32_t guard = LoadGuestU32(base, device + 56);
  // Growth changes the command owner. Retain that call before touching the
  // saved frame or publishing any part of a command into the old owner.
  if (!cursor || ((cursor | limit | guard) & 3) || cursor > guard ||
      uint64_t(cursor) + 44 > limit || !BackingRange(cursor, 48, commands) ||
      Overlaps(commands, dev) || Overlaps(commands, frame))
    return false;
  // These are addresses sent to the GPU, not CPU source pointers. Preserve
  // the original physical-view offset and 32-bit page arithmetic, including
  // empty ranges and wrapping lengths; do not dereference the range here.
  const uint32_t physical =
      (ctx.r4.u32 & 0x1FFFFFFFu) + (((ctx.r4.u32 >> 20) + 512) & 0x1000u);
  plan = {device, cursor, stack, physical, physical & ~4095u,
          uint32_t(uint64_t(physical) + ctx.r5.u64 + 4095) & ~4095u};
  return true;
}

inline void Store64(uint8_t* base, uint32_t address, uint64_t value) {
  value = std::byteswap(value);
  std::memcpy(GuestToHost(base, address), &value, sizeof(value));
}

inline void Apply(PPCContext& ctx, uint8_t* base, const Plan& plan, bool writeback) {
  // Keep the original save area and back chain visible to guest stack walkers.
  // The preserved registers stay in the context throughout this leaf writer.
  if (writeback)
    Store64(base, plan.stack - 40, ctx.r28.u64);
  Store64(base, plan.stack - 32, ctx.r29.u64);
  Store64(base, plan.stack - 24, ctx.r30.u64);
  Store64(base, plan.stack - 16, ctx.r31.u64);
  StoreGuestU32(base, plan.stack - 8, uint32_t(ctx.lr));
  StoreGuestU32(base, plan.stack - (writeback ? 128 : 112), plan.stack);

  const uint64_t size = uint64_t(plan.end_page) - plan.first_page;
  const uint32_t status = writeback ? 0x03000200u : 0x03000000u;
  const auto words = rex::graphics::native_gpu_sync::Encode(
      {status, uint32_t(size), plan.first_page});
  for (uint32_t i = 0; i < words.size(); ++i)
    StoreGuestU32(base, plan.cursor + 4 + i * 4, words[i]);
  StoreGuestU32(base, plan.device + 48, plan.cursor + 44);

  // Preserve the original return and argument registers as well as the saved
  // ABI. The shared consumer performs the ordered coherency operation and
  // polls its real status register; this writer does not complete a fence.
  ctx.r3.u64 = writeback ? 0 : plan.cursor + 44;
  ctx.r4.u64 = writeback ? 0xA31 : 0xFFFFFFFF80000000ull;
  ctx.r5.u64 = writeback ? 3 : 0;
  ctx.r6.u64 = writeback ? 0x10A2F : 0xA31;
  ctx.r7.u64 = writeback ? 0xFFFFFFFFC0043C00ull : 3;
  ctx.r8.u64 = writeback ? status : 0xFFFFFFFFC0043C00ull;
  ctx.r9.u64 = writeback ? size : 0x10A2F;
  ctx.r10.u64 = writeback ? plan.first_page : status;
  ctx.r11.u64 = writeback ? plan.cursor + 44 : size;
  ctx.r12.u64 = ctx.lr = uint32_t(ctx.lr);
}
}  // namespace buffer_coherency_detail

// 824415D0 and 8244E208: flush the caller's byte range in the current command
// owner. Both retain the eleven-word span used by recorded-buffer patchers.
inline bool WriteBufferCoherency(PPCContext& ctx, uint8_t* base, bool writeback) {
  buffer_coherency_detail::Plan plan;
  if (!buffer_coherency_detail::BuildPlan(ctx, base, writeback, plan))
    return false;
  buffer_coherency_detail::Apply(ctx, base, plan, writeback);
  return true;
}

// 82441238, after its real resource wait, submitting-thread check and optional
// chunk growth. Replace just the flush writer; all resource updates and the
// atomic unlock that follow still run in their original order.
inline bool WriteUnlockCoherency(PPCContext& ctx, uint8_t* base) {
  using namespace native_gpu_memory;
  const uint32_t device = ctx.r31.u32, stack = ctx.r1.u32, cursor = ctx.r11.u32;
  MemoryRange dev, frame, commands;
  if (!base || (device & 3) || (stack & 15) || !cursor || (cursor & 3) ||
      !BackingRange(device, 22256, dev) || !BackingRange(stack, 264, frame) ||
      Overlaps(dev, frame) || !BackingRange(cursor, 48, commands) ||
      Overlaps(commands, dev) || Overlaps(commands, frame) ||
      cursor != LoadGuestU32(base, device + 48) ||
      uint64_t(cursor) + 44 > LoadGuestU32(base, device + 52))
    return false;
  const uint32_t status = LoadGuestU32(base, stack + 260);
  const uint32_t first_page = ctx.r30.u32 & ~4095u;
  const uint32_t end_page = (ctx.r29.u32 + 4095u) & ~4095u;
  const uint64_t size = uint64_t(end_page) - first_page;
  const auto words = rex::graphics::native_gpu_sync::Encode({status, uint32_t(size), first_page});
  for (uint32_t i = 0; i < words.size(); ++i)
    StoreGuestU32(base, cursor + 4 + i * 4, words[i]);
  StoreGuestU32(base, device + 48, cursor + 44);
  ctx.r3.u64 = 0;
  ctx.r4.u64 = 0xA31;
  ctx.r5.u64 = 3;
  ctx.r6.u64 = 0xFFFFFFFFC0043C00ull;
  ctx.r7.u64 = 0x10A2F;
  ctx.r8.u64 = status;
  ctx.r9.u64 = size;
  ctx.r10.u64 = first_page;
  ctx.r11.u32 = cursor + 44;
  ctx.r29.u64 = 8;
  ctx.r30.u64 = 0xFFFFFFFF80000000ull;
  return true;
}
}  // namespace simpsons::native_gpu_calls
