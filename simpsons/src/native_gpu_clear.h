#pragma once

#include <array>
#include <bit>
#include <cstdint>

#include <rex/graphics/native_gpu_state.h>

#include "native_gpu_vertices.h"

namespace simpsons::native_gpu_clear {

struct ClearPlan {
  uint32_t device, cursor, count, half_pixel;
};

inline bool PlanClear(const PPCContext& ctx, const uint8_t* base, ClearPlan& plan) {
  using namespace native_gpu_memory;
  const uint32_t device = ctx.r3.u32, count = ctx.r4.u32;
  MemoryRange dev, stack, commands, half;
  if (!base || !count || count > 21845 || (device & 3) || (ctx.r1.u32 & 15) || ctx.r1.u32 < 592 ||
      !BackingRange(device, 14916, dev) || !BackingRange(ctx.r1.u32 - 592, 680, stack) ||
      Overlaps(dev, stack) || LoadGuestU32(base, device + 13492) ||
      LoadGuestU32(base, device + 13216))
    return false;
  const uint32_t cursor = LoadGuestU32(base, device + 48);
  const uint32_t half_address = LoadGuestU32(base, device + 10688) & 1 ? 0x821DD0D8u : 0x82000FB8u;
  if ((cursor & 3) || cursor > LoadGuestU32(base, device + 56) ||
      uint64_t(cursor) + 88 > LoadGuestU32(base, device + 52) ||
      !BackingRange(cursor + 4, 88, commands) || !BackingRange(half_address, 4, half) ||
      Overlaps(commands, dev) || Overlaps(commands, stack) || Overlaps(commands, half) ||
      Overlaps(half, dev) || Overlaps(half, stack))
    return false;
  plan = {device, cursor, count, half_address};
  // The original reads its half-pixel constant before allocation, but reads
  // neither rectangles nor colour when the retained allocator returns zero.
  if (*GuestToHost(base, device + 10941) & 0x20)
    return true;
  const uint32_t bytes = count * 84;
  uint64_t first = LoadGuestU32(base, device + 14892);
  if (!first)
    first = uint64_t(LoadGuestU32(base, device + 52)) + 60;
  first = (first + 3) & ~uint64_t(3);
  const uint64_t end = first + bytes;
  MemoryRange vertices, rectangles, colour, sync;
  const uint32_t sync_address = LoadGuestU32(base, device + 10896);
  if (end > UINT32_MAX || end > LoadGuestU32(base, device + 14884) ||
      !BackingRange(uint32_t(first), bytes, vertices) ||
      !BackingRange(ctx.r5.u32, uint64_t(count) * 16, rectangles) ||
      !BackingRange(ctx.r6.u32, 16, colour) || !BackingRange(sync_address, 8, sync) ||
      Overlaps(vertices, dev) || Overlaps(vertices, stack) || Overlaps(vertices, commands) ||
      Overlaps(vertices, rectangles) || Overlaps(vertices, colour) || Overlaps(vertices, half) ||
      Overlaps(sync, dev) || Overlaps(sync, stack) || Overlaps(sync, commands) ||
      Overlaps(sync, vertices) || Overlaps(rectangles, stack) || Overlaps(rectangles, commands) ||
      Overlaps(colour, stack) || Overlaps(colour, commands))
    return false;
  return ((LoadGuestU32(base, device + 14896) - LoadGuestU32(base, sync_address + 4)) & 3) == 0;
}

// Clear's utility geometry uses three position/depth/colour vertices per
// rectangle. Keep its float rounding steps, actual allocator and dirty flags.
template <typename Allocate>
bool DrawClear(PPCContext& ctx, uint8_t* base, Allocate allocate) {
  ClearPlan plan;
  if (!PlanClear(ctx, base, plan))
    return false;
  const uint64_t incoming_lr = ctx.lr;
  ctx.fpscr.disableFlushMode();
  const double half = double(LoadGuestFloat(base, plan.half_pixel));
  PPCContext work = ctx;
  work.r1.u32 -= 224;
  StoreGuestU32(base, work.r1.u32, ctx.r1.u32);
  StoreGuestU32(base, ctx.r1.u32 - 8, uint32_t(incoming_lr));
  work.r31.u64 = ctx.r3.u64;
  work.f30.f64 = ctx.f1.f64;
  work.f31.f64 = half;
  work.r25.u64 = ctx.r4.u64;
  work.r24.u64 = ctx.r5.u64;
  work.r28.u64 = ctx.r6.u64;
  work.r29.u64 = ctx.r8.u64;
  work.r26.u64 = ctx.r9.u64;
  work.r23.u64 = ctx.r10.u64;
  work.r30.u64 = plan.cursor;
  work.r27.u64 = ctx.r4.u64 * 21;
  work.r3.u64 = ctx.r3.u64;
  work.r4.u64 = work.r27.u64;
  work.r5.u64 = 4;
  work.lr = 0x82452A44;
  allocate(work, base);
  const uint32_t vertex = work.r3.u32;
  ctx.r3.u64 = 0;
  ctx.lr = uint32_t(incoming_lr);
  if (!vertex) {
    StoreGuestU32(base, plan.device + 48, plan.cursor);
    return true;
  }
  std::array<double, 4> colour;
  for (uint32_t i = 0; i < colour.size(); ++i)
    colour[i] = double(LoadGuestFloat(base, ctx.r6.u32 + i * 4));
  for (uint32_t rectangle = 0; rectangle < plan.count; ++rectangle) {
    std::array<double, 4> edges;
    for (uint32_t i = 0; i < edges.size(); ++i) {
      const int32_t value = int32_t(LoadGuestU32(base, ctx.r5.u32 + rectangle * 16 + i * 4));
      edges[i] = double(float(double(value)));
    }
    const std::array<float, 4> positions = {float(edges[0] - half), float(edges[1] - half),
                                            float(edges[2] - half), float(edges[3] - half)};
    for (uint32_t corner = 0; corner < 3; ++corner) {
      const uint32_t output = vertex + rectangle * 84 + corner * 28;
      StoreGuestFloat(base, output, positions[corner ? 2 : 0]);
      StoreGuestFloat(base, output + 4, positions[corner == 2 ? 3 : 1]);
      StoreGuestFloat(base, output + 8, float(ctx.f1.f64));
      for (uint32_t i = 0; i < colour.size(); ++i)
        StoreGuestFloat(base, output + 12 + i * 4, float(colour[i]));
    }
  }
  uint32_t cursor = plan.cursor;
  const auto state = [&](uint32_t first, const auto& values) {
    uint32_t header;
    rex::graphics::native_gpu_state::EncodeHeader(first, uint32_t(values.size()), header);
    StoreGuestU32(base, cursor += 4, header);
    for (uint32_t value : values)
      StoreGuestU32(base, cursor += 4, value);
  };
  const uint32_t physical = (vertex & 0x1FFFFFFFu) + (((vertex >> 20) + 512) & 0x1000u);
  state(0x4800, std::array<uint32_t, 6>{
                    physical | 3, ((plan.count * 84) & 0x03FFFFFCu) | 0x10000002u, 0, 0, 0, 0});
  state(0x5000, std::array<uint32_t, 3>{0, 0, 0});
  state(0x2000, std::array<uint32_t, 2>{ctx.r8.u32, ctx.r9.u32});
  const uint32_t samples = (ctx.r8.u32 >> 16) & 3;
  state(0x2301, std::array<uint32_t, 1>{samples ? (samples == 1 ? 0x8001u : 0xC003u) : 0u});
  state(0x2200, std::array<uint32_t, 1>{ctx.r10.u32});
  state(0x2208, std::array<uint32_t, 1>{LoadGuestU32(base, ctx.r1.u32 + 84)});
  StoreGuestU32(base, cursor += 4,
                0xC0000001u | (rex::graphics::native_gpu_calls::kBareDrawOpcode << 8));
  StoreGuestU32(base, cursor += 4, (plan.count * 0x30000u) | 0x88u);
  StoreGuestU32(base, plan.device + 48, cursor);
  using native_gpu_vertices::Load64;
  using native_gpu_vertices::Store64;
  Store64(base, plan.device + 24, Load64(base, plan.device + 24) | 0x80000000u);
  uint64_t dirty = Load64(base, plan.device + 16) | (uint64_t(1) << 57);
  Store64(base, plan.device + 16, dirty);
  dirty |= uint64_t(1) << 56;
  Store64(base, plan.device + 16, dirty);
  Store64(base, plan.device + 32, Load64(base, plan.device + 32) | (uint64_t(1) << 36));
  dirty |= 0x800;
  Store64(base, plan.device + 16, dirty);
  Store64(base, plan.device + 16, dirty | 8);
  return true;
}

}  // namespace simpsons::native_gpu_clear
