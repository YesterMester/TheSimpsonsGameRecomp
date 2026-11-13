#pragma once

#include <array>
#include <rex/graphics/native_gpu_predication.h>
#include <rex/graphics/native_gpu_state.h>
#include <rex/ppc/context.h>

#include "guest_memory.h"
#include "native_gpu_memory.h"

namespace simpsons::native_gpu_state {

struct ConditionalPlan {
  uint32_t device, initial, cursor, count = 0, publications = 0, tail_count = 0;
  uint64_t remaining;
  bool masked_streams = false;
  std::array<uint32_t, 32> words;
  std::array<uint32_t, 453> stream_values;

  uint32_t PublicationWords(uint32_t index) const { return 2 + (index + 1) * 5; }
};
static_assert(sizeof(ConditionalPlan) <= 2048);

// This helper temporarily selects per-stream state, then restores both mask
// halves. Plan all of it before publishing so a late growth/alias rejection
// cannot leave a different selection behind for the following draw.
inline bool PlanConditionalAt(const PPCContext& ctx, const uint8_t* base, uint32_t initial,
                              ConditionalPlan& plan) {
  using namespace native_gpu_memory;
  namespace predication = rex::graphics::native_gpu_predication;
  const uint32_t device = ctx.r3.u32;
  MemoryRange dev, stack, commands;
  if (!base || (device & 3) || (initial & 3) || (ctx.r1.u32 & 15) || ctx.r1.u32 < 192 ||
      !BackingRange(device, 14916, dev) || !BackingRange(ctx.r1.u32 - 192, 192, stack) ||
      Overlaps(dev, stack) || initial > LoadGuestU32(base, device + 56))
    return false;
  plan.device = device;
  plan.initial = initial;
  plan.count = plan.publications = plan.tail_count = 0;
  plan.masked_streams = false;
  plan.remaining = ctx.r4.u64;
  bool fits = true;
  const auto append = [&](uint32_t word) {
    if (plan.tail_count == plan.words.size()) {
      fits = false;
      return;
    }
    plan.words[plan.tail_count++] = word;
    ++plan.count;
  };
  const auto mask = [&](bool high, uint32_t value) {
    append(high ? predication::kMaskHighHeader : predication::kMaskLowHeader);
    append(value);
  };
  const auto state = [&](uint32_t reg, uint32_t value, bool conditional = false) {
    if (conditional) {
      append(predication::kStateHeader);
      append(0x40000 | (reg - 0x2000));
    } else {
      uint32_t header;
      if (!rex::graphics::native_gpu_state::EncodeHeader(reg, 1, header))
        return false;
      append(header);
    }
    append(value);
    return true;
  };
  const auto restore_masks = [&] {
    mask(false, LoadGuestU32(base, device + 12700));
    mask(true, LoadGuestU32(base, device + 12704));
  };
  uint32_t restore_control = UINT32_MAX, restore_surface = UINT32_MAX, restore_index = UINT32_MAX;
  if (plan.remaining & 0x100) {
    const uint8_t flags = *GuestToHost(base, device + 10940);
    const uint8_t stream_flags = *GuestToHost(base, device + 10943);
    bool compatible = bool(flags & 0x10);
    if (!compatible && (flags & 0x20)) {
      compatible = true;
      for (uint32_t i = 0; i < 5; ++i) {
        const uint32_t expected = LoadGuestU32(base, device + 12432 + i * 4);
        compatible &= !expected || expected == LoadGuestU32(base, device + 12720 + i * 4);
      }
    }
    uint32_t control = ctx.r5.u32;
    if ((stream_flags & 0x20) && compatible) {
      const uint32_t raw_count = LoadGuestU32(base, device + 12740);
      // The reference tests count-1 as signed. Zero and its negative range
      // use the existing caller scratch word instead of reading a stream.
      const uint32_t streams = (raw_count - 1) & 0x80000000u ? 0 : raw_count;
      if (streams > plan.stream_values.size())
        return false;
      // Snapshot only the varying values. Headers, masks and publication
      // positions are deterministic, keeping the common draw plan below 2 KiB.
      plan.masked_streams = true;
      plan.count = 2 + streams * 5;
      uint32_t selected = streams ? 0 : LoadGuestU32(base, ctx.r1.u32 - 112);
      const uint32_t mode = (LoadGuestU32(base, device + 11844) >> 17) & 7;
      for (uint32_t i = streams; i--;) {
        control = (control & 0x1FFFF) | (LoadGuestU32(base, device + 13104 + i * 4) << 17);
        selected = !mode || (mode == 2 && (i || !(stream_flags & 0x10))) ? control & ~1u : control;
        plan.stream_values[plan.publications++] = selected;
      }
      if (selected != control && (flags & 0x40)) {
        mask(false, 1);
        if (!state(0x2203, control, true))
          return false;
      }
      restore_masks();
    } else {
      const uint32_t selected =
          LoadGuestU32(base, device + 11844) & 0xE0000 ? control : control & ~1u;
      if (!state(0x2203, selected))
        return false;
      if (selected != control && (flags & 0x40))
        restore_control = control;
    }
    plan.remaining &= ~uint64_t(0x100);
  }
  const uint64_t dirty =
      (uint64_t(LoadGuestU32(base, device + 40)) << 32) | LoadGuestU32(base, device + 44);
  const uint64_t fixups = dirty & plan.remaining;
  if (fixups & (uint64_t(1) << 57)) {
    const uint32_t surface = LoadGuestU32(base, device + 10368);
    if (!state(0x2000, surface))
      return false;
    plan.remaining &= ~(uint64_t(1) << 57);
    restore_surface = (LoadGuestU32(base, device + 13168) & 0x3FFF) | (surface & 0xFFFFC000);
  }
  if (fixups & (uint64_t(1) << 37)) {
    if (!state(0x2104, LoadGuestU32(base, device + 10460)))
      return false;
    plan.remaining &= ~(uint64_t(1) << 37);
    restore_index = 0;
  }
  if ((restore_control & restore_surface & restore_index) != UINT32_MAX) {
    mask(true, 0);
    mask(false, 0x15555555);
    if ((restore_control != UINT32_MAX && !state(0x2203, restore_control, true)) ||
        (restore_surface != UINT32_MAX && !state(0x2000, restore_surface, true)) ||
        (restore_index != UINT32_MAX && !state(0x2104, restore_index, true)))
      return false;
    restore_masks();
  }
  const uint64_t end = uint64_t(initial) + plan.count * 4;
  if (!fits || end > UINT32_MAX || end > LoadGuestU32(base, device + 52) ||
      (plan.publications && uint64_t(initial) + plan.PublicationWords(plan.publications - 1) * 4 >
                                LoadGuestU32(base, device + 56)) ||
      (plan.count && (!BackingRange(initial + 4, plan.count * 4, commands) ||
                      Overlaps(commands, dev) || Overlaps(commands, stack))))
    return false;
  plan.cursor = uint32_t(end);
  return true;
}

inline void ApplyConditional(const ConditionalPlan& plan, uint8_t* base) {
  namespace predication = rex::graphics::native_gpu_predication;
  uint32_t cursor = plan.initial;
  const auto emit = [&](uint32_t value) {
    cursor += 4;
    StoreGuestU32(base, cursor, value);
  };
  if (plan.masked_streams) {
    emit(predication::kMaskHighHeader);
    emit(0);
    for (uint32_t i = 0; i < plan.publications; ++i) {
      const uint32_t shift = ((plan.publications - 1 - i) * 2) & 63;
      emit(predication::kMaskLowHeader);
      emit(shift & 32 ? 0 : 3u << shift);
      emit(predication::kStateHeader);
      emit(0x40203);
      emit(plan.stream_values[i]);
      StoreGuestU32(base, plan.device + 48, cursor);
    }
  }
  for (uint32_t i = 0; i < plan.tail_count; ++i)
    emit(plan.words[i]);
  StoreGuestU32(base, plan.device + 48, plan.cursor);
}

inline bool WriteConditional(PPCContext& ctx, uint8_t* base) {
  native_gpu_memory::MemoryRange device;
  if (!base || !native_gpu_memory::BackingRange(ctx.r3.u32, 60, device))
    return false;
  ConditionalPlan plan;
  if (!PlanConditionalAt(ctx, base, LoadGuestU32(base, ctx.r3.u32 + 48), plan))
    return false;
  ApplyConditional(plan, base);
  ctx.r3.u64 = plan.remaining;
  ctx.lr = uint32_t(ctx.lr);
  return true;
}

}  // namespace simpsons::native_gpu_state
