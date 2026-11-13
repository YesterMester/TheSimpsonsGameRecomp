#pragma once

#include <array>
#include <bit>
#include <cstdint>
#include <cstring>

#include <rex/graphics/native_gpu_calls.h>
#include <rex/ppc/context.h>

#include "guest_memory.h"
#include "native_gpu_memory.h"
#include "native_gpu_recorded_allocation.h"

namespace simpsons::native_gpu_vertices {

inline uint64_t Load64(const uint8_t* base, uint32_t address) {
  uint64_t value;
  std::memcpy(&value, GuestToHost(base, address), sizeof(value));
  return std::byteswap(value);
}

inline void Store64(uint8_t* base, uint32_t address, uint64_t value) {
  value = std::byteswap(value);
  std::memcpy(GuestToHost(base, address), &value, sizeof(value));
}

struct BeginPlan {
  uint32_t device, cursor, dwords;
};

// A caller may preflight a state flush and the deferred draw as one span.
// It must apply that verified flush before using the allocation/draw plan.
struct BeginSubmission {
  uint32_t initial, cursor;
};

// Plan before touching either the device or the allocator. This first branch
// handles prepared state or a fully planned state flush with the default,
// non-wrapping allocator or a proved fixed recording. Custom heaps, bin/extent
// records and growth retain the original call before any flush or allocation.
inline bool PlanBegin(const PPCContext& ctx, const uint8_t* base, BeginPlan& plan,
                      const BeginSubmission* submission = nullptr) {
  using namespace native_gpu_memory;
  const uint32_t device = ctx.r3.u32, count = ctx.r5.u32, stride = ctx.r6.u32;
  MemoryRange dev, stack, commands, vertices, sync;
  if (!base || (device & 3) || (ctx.r1.u32 & 15) || ctx.r1.u32 < 544 || !count || count > 65535 ||
      stride < 4 || stride > 1020 || (stride & 3) || !BackingRange(device, 14916, dev) ||
      !BackingRange(ctx.r1.u32 - 544, 544, stack) || Overlaps(dev, stack))
    return false;
  if ((!submission &&
       (Load64(base, device) || Load64(base, device + 8) || Load64(base, device + 16) ||
        (Load64(base, device + 24) & ~uint64_t(1)) || Load64(base, device + 32))) ||
      (*GuestToHost(base, device + 10940) & 1) || LoadGuestU32(base, device + 13216))
    return false;
  if (*GuestToHost(base, device + 11824) != stride / 4 && LoadGuestU32(base, device + 12688))
    return false;
  const uint32_t cursor = submission ? submission->cursor : LoadGuestU32(base, device + 48);
  const uint32_t initial = submission ? submission->initial : cursor;
  if ((cursor & 3) || (initial & 3) || initial > cursor ||
      cursor > LoadGuestU32(base, device + 56) ||
      uint64_t(cursor) + 64 > LoadGuestU32(base, device + 52) ||
      !BackingRange(initial + 4, uint64_t(cursor) + 64 - initial, commands) ||
      Overlaps(commands, dev) || Overlaps(commands, stack))
    return false;
  const uint32_t bytes = count * stride;
  plan = {device, cursor, bytes / 4};
  // The real allocator's existing failure flag returns zero without touching
  // a heap. Preserve that failure rather than fabricating a successful draw.
  if (*GuestToHost(base, device + 10941) & 0x20)
    return true;
  if (LoadGuestU32(base, device + 13492)) {
    const std::array protected_ranges = {dev, stack};
    const std::array sizes = {plan.dwords};
    return PlanRecordedAllocations(base, device, cursor, commands, protected_ranges, sizes);
  }
  uint64_t first = LoadGuestU32(base, device + 14892);
  if (!first)
    first = uint64_t(LoadGuestU32(base, device + 52)) + 60;
  first = (first + 15) & ~uint64_t(15);
  const uint32_t sync_address = LoadGuestU32(base, device + 10896);
  if (first + bytes > LoadGuestU32(base, device + 14884) || first + bytes > UINT32_MAX ||
      !BackingRange(uint32_t(first), bytes, vertices) || !BackingRange(sync_address, 8, sync) ||
      Overlaps(vertices, dev) || Overlaps(vertices, commands) || Overlaps(vertices, stack) ||
      Overlaps(sync, dev) || Overlaps(sync, commands) || Overlaps(sync, vertices) ||
      Overlaps(sync, stack))
    return false;
  const uint32_t version = LoadGuestU32(base, device + 14896);
  return ((version - LoadGuestU32(base, sync_address + 4)) & 3) == 0;
}

// Allocate is the actual sub_82456D60 in the game and the independently
// extracted, executed allocator body in the CPU oracle. It is never a mock.
template <typename Allocate>
bool BeginVerticesWithPlan(PPCContext& ctx, uint8_t* base, const BeginPlan& plan,
                           Allocate allocate) {
  const uint32_t device = plan.device;
  const uint64_t incoming_lr = ctx.lr;
  Store64(base, device + 24, 0);
  PPCContext work = ctx;
  // Give the retained allocator its original guest call frame and arguments.
  work.r1.u32 -= 176;
  StoreGuestU32(base, work.r1.u32, ctx.r1.u32);
  StoreGuestU32(base, ctx.r1.u32 - 8, uint32_t(incoming_lr));
  work.lr = 0x8244C704;
  work.r3.u64 = ctx.r3.u64;
  work.r4.u64 = plan.dwords;
  work.r5.u64 = 16;
  work.r22.u64 = plan.dwords;
  work.r23.u64 = ctx.r4.u64;
  work.r24.u64 = ctx.r5.u64;
  work.r25.u64 = *GuestToHost(base, device + 12520);
  work.r26.u64 = work.r27.u64 = work.r28.u64 = work.r29.u64 = 0;
  work.r30.u64 = plan.cursor;
  work.r31.u64 = ctx.r3.u64;
  allocate(work, base);
  ctx.r3.u64 = work.r3.u64;
  ctx.lr = uint32_t(incoming_lr);
  if (!work.r3.u32) {
    ctx.r3.u64 = 0;
    StoreGuestU32(base, device + 48, plan.cursor);
    return true;
  }
  const uint32_t vertex = work.r3.u32;
  const uint32_t physical = (vertex & 0x1FFFFFFFu) + (((vertex >> 20) + 512) & 0x1000u);
  const uint32_t mode = *GuestToHost(base, device + 12179);
  rex::graphics::native_gpu_calls::VerticesDrawCall call{
      0,
      {1, 0, 1, 0, physical | 3, ((plan.dwords << 2) & 0x03FFFFFCu) | 0x10000002u},
      {},
      uint32_t(*GuestToHost(base, device + 12114 + mode) & 0x3F) | (mode ? 0x100u : 0u),
      (work.r23.u32 & 63) | (work.r24.u32 << 16) | 0x80};
  std::array<uint32_t, 16> words;
  // Its source is always auto-index and the fixed span cannot fail encoding.
  rex::graphics::native_gpu_calls::EncodeVertices(call, words);
  for (uint32_t i = 0; i < words.size(); ++i)
    StoreGuestU32(base, plan.cursor + 4 + i * 4, words[i]);
  Store64(base, device + 24, 1);
  StoreGuestU32(base, device + 13428, plan.cursor + 64);
  StoreGuestU32(base, device + 13432, vertex);
  StoreGuestU32(base, device + 13440, plan.dwords);
  // device+48 stays at the old cursor. The caller must fill the returned
  // vertices and call EndVertices before the draw becomes visible.
  return true;
}

template <typename Allocate>
bool BeginVertices(PPCContext& ctx, uint8_t* base, Allocate allocate) {
  BeginPlan plan;
  return PlanBegin(ctx, base, plan) && BeginVerticesWithPlan(ctx, base, plan, allocate);
}

inline bool EndVertices(PPCContext& ctx, uint8_t* base) {
  native_gpu_memory::MemoryRange device;
  if (!base || (ctx.r3.u32 & 3) || !native_gpu_memory::BackingRange(ctx.r3.u32, 13432, device))
    return false;
  ctx.r10.u64 = *GuestToHost(base, ctx.r3.u32 + 10940) & 0x80;
  ctx.r11.u64 = LoadGuestU32(base, ctx.r3.u32 + 13428);
  StoreGuestU32(base, ctx.r3.u32 + 48, ctx.r11.u32);
  ctx.cr0.compare<int32_t>(ctx.r10.s32, 0, ctx.xer);
  // The original tail call sub_824605C0 is an empty return.
  return true;
}

}  // namespace simpsons::native_gpu_vertices
