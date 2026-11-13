#pragma once

#include "native_gpu_chunk_close.h"

namespace simpsons::native_gpu_vertices {
namespace chunk_dispatch_detail {
using namespace native_gpu_memory;
using chunk_close_detail::Load64;
using recorded_allocator_detail::Store64;

constexpr uint32_t kSynchronizeChunks = 0x82D51550;

struct Plan {
  chunk_close_detail::ClosePlan close;
  uint32_t device = 0, stack = 0, cursor = 0, capacity = 0;
  bool synchronize = false;
};

inline bool BuildArguments(uint32_t device, uint32_t sp, const uint8_t* base, Plan& plan) {
  MemoryRange dev, frame, completed{}, object{}, owner, global{};
  if (!base || (device & 3) || (sp & 15) || sp < 624 || !BackingRange(device, 22268, dev) ||
      !BackingRange(sp - 624, 624, frame) || Overlaps(dev, frame))
    return false;
  const bool recording = (*GuestToHost(base, device + 10940) & 0x80) != 0;
  // The normal path synthesizes state and submits to the real ring first.
  if ((!recording && !LoadGuestU32(base, device + 13216)) ||
      !chunk_close_detail::PlanCloseArguments(device, sp - 112, base, plan.close))
    return false;
  const uint32_t sync = LoadGuestU32(base, device + 10896);
  if (sync && ((sync & 3) || !BackingRange(sync, 8, completed) || Overlaps(completed, frame) ||
               Overlaps(completed, dev)))
    return false;
  plan.device = device;
  plan.stack = sp;
  plan.cursor = plan.close.grow ? plan.close.growth.cursor : plan.close.aligned - 4;
  plan.capacity = plan.close.grow ? plan.close.growth.capacity : LoadGuestU32(base, device + 52);
  if (!chunk_close_detail::Owner(base, device, (*GuestToHost(base, device + 10941) & 0x20) != 0,
                                 dev, frame, owner, object) ||
      Overlaps(completed, owner) || Overlaps(completed, object))
    return false;
  if (plan.close.descriptor) {
    MemoryRange descriptors;
    if (!BackingRange(LoadGuestU32(base, device + 13356), 4224, descriptors) ||
        Overlaps(frame, descriptors) || Overlaps(completed, descriptors))
      return false;
  }
  if (plan.close.grow) {
    const auto& growth = plan.close.growth;
    MemoryRange storage;
    if (!BackingRange(growth.success ? growth.first : growth.cursor,
                      growth.success ? growth.bytes : 4800, storage) ||
        Overlaps(frame, storage) || Overlaps(dev, storage) || Overlaps(object, storage) ||
        Overlaps(completed, storage))
      return false;
  }
  if (recording)
    return true;
  // Closure does not change this global, the sync words, or the submitted
  // sequence. Prove that before using their preflight values after publication.
  if (!BackingRange(kSynchronizeChunks, 4, global) || Overlaps(global, dev) ||
      Overlaps(global, frame) || Overlaps(global, owner) || Overlaps(global, object))
    return false;
  if (plan.close.descriptor) {
    MemoryRange descriptors;
    if (!BackingRange(LoadGuestU32(base, device + 13356), 4224, descriptors) ||
        Overlaps(global, descriptors))
      return false;
  }
  if (plan.close.grow) {
    const auto& growth = plan.close.growth;
    MemoryRange storage;
    if (!BackingRange(growth.success ? growth.first : growth.cursor,
                      growth.success ? growth.bytes : 4800, storage) ||
        Overlaps(global, storage))
      return false;
  }
  plan.synchronize =
      LoadGuestU32(base, kSynchronizeChunks) && !(*GuestToHost(base, device + 10941) & 2);
  if (plan.synchronize) {
    const uint32_t submitted = LoadGuestU32(base, device + 10908);
    const uint32_t target = submitted - 2;
    if (target && (!sync || 2u < submitted - LoadGuestU32(base, sync)))
      return false;  // Preserve real blocking waits; never manufacture completion.
  }
  return true;
}

template <typename EnsureWritable>
inline void Apply(PPCContext& ctx, uint8_t* base, const Plan& plan, EnsureWritable ensure) {
  const uint32_t sp = plan.stack, device = plan.device;
  StoreGuestU32(base, sp - 8, uint32_t(ctx.lr));
  Store64(base, sp - 24, ctx.r30.u64);
  Store64(base, sp - 16, ctx.r31.u64);
  StoreGuestU32(base, sp - 112, sp);
  ctx.r1.u32 = sp - 112;
  ctx.r31.u64 = ctx.r3.u64;
  ctx.lr = 0x82457D5C;
  chunk_close_detail::ApplyClose(ctx, base, plan.close, ensure);
  if (plan.synchronize) {
    // The real marker routine's nonblocking return still leaves its save area.
    Store64(base, sp - 144, ctx.r29.u64);
    Store64(base, sp - 136, ctx.r30.u64);
    Store64(base, sp - 128, ctx.r31.u64);
    StoreGuestU32(base, sp - 120, 0x82457D9C);
    StoreGuestU32(base, sp - 256, sp - 112);
    *GuestToHost(base, device + 10941) |= 2;
  }
  ctx.r3.u64 = plan.cursor;
  if (plan.cursor)
    ensure(base + plan.cursor, 8192);
  ctx.r1.u32 = sp;
  ctx.r30.u64 = Load64(base, sp - 24);
  ctx.r31.u64 = Load64(base, sp - 16);
  ctx.lr = LoadGuestU32(base, sp - 8);
}

struct ReservePlan {
  Plan dispatch;
  uint32_t device = 0, stack = 0, bytes = 0;
  bool close = false;
};

inline bool PlanReserve(const PPCContext& ctx, const uint8_t* base, ReservePlan& plan) {
  const uint32_t device = ctx.r3.u32, sp = ctx.r1.u32;
  MemoryRange dev, frame, owner, object{}, writes;
  if (!base || (device & 3) || (sp & 15) || sp < 736 || !BackingRange(device, 22268, dev) ||
      !BackingRange(sp - 736, 736, frame) || Overlaps(dev, frame) || ctx.r4.u32 > 0x3FFFFFFF ||
      !chunk_close_detail::Owner(base, device, (*GuestToHost(base, device + 10941) & 0x20) != 0,
                                 dev, frame, owner, object))
    return false;
  plan.device = device;
  plan.stack = sp;
  plan.bytes = ctx.r4.u32 << 2;
  const uint32_t cursor = LoadGuestU32(base, device + 48);
  const uint32_t capacity = LoadGuestU32(base, device + 52);
  if (((cursor | capacity) & 3) || uint64_t(cursor) + plan.bytes > UINT32_MAX)
    return false;
  plan.close = cursor + plan.bytes > capacity;
  const uint64_t bytes = std::max(4u, plan.bytes);
  if (plan.close) {
    if (!BuildArguments(device, sp - 112, base, plan.dispatch) ||
        uint64_t(plan.dispatch.cursor) + plan.bytes > plan.dispatch.capacity)
      return false;  // A second allocation needs a separate complete proof.
    if (plan.dispatch.close.grow) {
      const auto& growth = plan.dispatch.close.growth;
      if (!BackingRange(growth.success ? growth.first : growth.cursor,
                        growth.success ? growth.bytes : 4800, owner) ||
          Overlaps(frame, owner) || Overlaps(dev, owner) || Overlaps(object, owner))
        return false;
    }
  }
  const uint32_t result = plan.close ? plan.dispatch.cursor : cursor;
  return uint64_t(result) + 4 + bytes <= UINT32_MAX && BackingRange(result + 4, bytes, writes) &&
         writes.first >= owner.first && writes.end <= owner.end && !Overlaps(writes, dev) &&
         !Overlaps(writes, frame) && !Overlaps(writes, object);
}

template <typename EnsureWritable>
inline void ApplyReserve(PPCContext& ctx, uint8_t* base, const ReservePlan& plan,
                         EnsureWritable ensure) {
  const uint32_t sp = plan.stack;
  Store64(base, sp - 32, ctx.r29.u64);
  Store64(base, sp - 24, ctx.r30.u64);
  Store64(base, sp - 16, ctx.r31.u64);
  StoreGuestU32(base, sp - 8, uint32_t(ctx.lr));
  StoreGuestU32(base, sp - 112, sp);
  ctx.r1.u32 = sp - 112;
  ctx.r31.u64 = ctx.r3.u64;
  ctx.r29.u64 = ctx.r4.u64;
  ctx.r30.u64 = plan.bytes;
  if (plan.close) {
    ctx.lr = 0x82457F80;
    Apply(ctx, base, plan.dispatch, ensure);
  }
  ctx.r3.u64 = LoadGuestU32(base, plan.device + 48);
  ctx.r1.u32 = sp;
  ctx.r29.u64 = Load64(base, sp - 32);
  ctx.r30.u64 = Load64(base, sp - 24);
  ctx.r31.u64 = Load64(base, sp - 16);
  ctx.lr = LoadGuestU32(base, sp - 8);
}
}  // namespace chunk_dispatch_detail

template <typename EnsureWritable>
inline bool DispatchCommandChunkWithGuard(PPCContext& ctx, uint8_t* base, EnsureWritable ensure) {
  chunk_dispatch_detail::Plan plan;
  if (!chunk_dispatch_detail::BuildArguments(ctx.r3.u32, ctx.r1.u32, base, plan))
    return false;
  chunk_dispatch_detail::Apply(ctx, base, plan, ensure);
  return true;
}

inline bool DispatchCommandChunk(PPCContext& ctx, uint8_t* base) {
  return DispatchCommandChunkWithGuard(ctx, base, native_gpu_memory::EnsureCommandStorageWritable);
}

template <typename EnsureWritable>
inline bool ReserveCommandWordsWithGuard(PPCContext& ctx, uint8_t* base, EnsureWritable ensure) {
  chunk_dispatch_detail::ReservePlan plan;
  if (!chunk_dispatch_detail::PlanReserve(ctx, base, plan))
    return false;
  chunk_dispatch_detail::ApplyReserve(ctx, base, plan, ensure);
  return true;
}

inline bool ReserveCommandWords(PPCContext& ctx, uint8_t* base) {
  return ReserveCommandWordsWithGuard(ctx, base, native_gpu_memory::EnsureCommandStorageWritable);
}
}  // namespace simpsons::native_gpu_vertices
