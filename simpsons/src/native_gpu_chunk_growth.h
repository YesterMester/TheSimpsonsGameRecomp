#pragma once

#include <algorithm>

#include "native_gpu_command_storage.h"
#include "native_gpu_default_allocator.h"

namespace simpsons::native_gpu_vertices {
namespace chunk_growth_detail {
using namespace native_gpu_memory;

struct Plan {
  default_allocator_detail::Plan normal;
  recorded_allocator_detail::Plan recording;
  uint32_t device = 0, stack = 0, requested = 0, bytes = 0, first = 0;
  uint32_t cursor = 0, capacity = 0, ready = 0;
  bool already_failed = false, fixed = false, success = false, marker = false;
};

inline bool Scratch(const uint8_t* base, uint32_t device, const MemoryRange& dev,
                    const MemoryRange& frame, const MemoryRange& object, const MemoryRange& owner,
                    Plan& plan) {
  const uint32_t scratch = LoadGuestU32(base, device + 16704);
  MemoryRange span;
  // 82466EC0 retains a separate 4,800-byte heap allocation at device+16704.
  if (!scratch || (scratch & 3) || uint64_t(scratch) + 4800 > UINT32_MAX ||
      !BackingRange(scratch, 4800, span) || Overlaps(span, dev) || Overlaps(span, frame) ||
      Overlaps(span, object) || Overlaps(span, owner))
    return false;
  plan.cursor = scratch;
  plan.capacity = scratch + 4800;
  plan.ready = scratch + 4640;
  return true;
}

inline bool BuildArguments(uint32_t device, uint32_t sp, uint32_t request, const uint8_t* base,
                           Plan& plan) {
  const uint32_t parameter = sp - 48;
  MemoryRange dev, frame, caller, object{}, owner{};
  if (!base || (device & 3) || (sp & 15) || sp < 384 || !BackingRange(device, 22268, dev) ||
      !BackingRange(sp - 384, 384, frame) || !BackingRange(sp - 128, 128, caller) ||
      Overlaps(dev, frame))
    return false;
  plan.device = device;
  plan.stack = sp;
  plan.already_failed = (*GuestToHost(base, device + 10941) & 0x20) != 0;
  if (plan.already_failed)
    return Scratch(base, device, dev, frame, object, owner, plan);
  plan.requested = request ? (request + 14) << 2 : 0;
  const uint32_t recording = LoadGuestU32(base, device + 13492);
  if (recording) {
    if ((recording & 3) || !BackingRange(recording, 184, object) || Overlaps(object, dev) ||
        Overlaps(object, frame))
      return false;
    if (LoadGuestU32(base, recording + 172))
      plan.requested = std::max(plan.requested, LoadGuestU32(base, device + 14888));
    if (!LoadGuestU32(base, recording + 152))
      return false;  // Custom allocation callbacks retain their original path.
    plan.fixed = true;
    if (!recorded_allocator_detail::BuildPlan(base, device, parameter, plan.requested, false, 32,
                                              frame, plan.recording))
      return false;
    plan.first = plan.recording.result;
    plan.bytes = plan.first ? plan.recording.bytes : plan.requested;
  } else {
    if (!default_allocator_detail::BuildPlan(base, device, parameter, plan.requested, false, 32,
                                             sp - 128, caller, plan.normal))
      return false;
    plan.first = plan.normal.failed ? 0 : plan.normal.first;
    plan.bytes = plan.first ? (plan.normal.end - plan.normal.first) & ~3u : plan.requested;
  }
  plan.success = plan.first != 0;
  if (plan.success) {
    const uint32_t owner_first =
        plan.fixed ? LoadGuestU32(base, recording + 152) : LoadGuestU32(base, device + 14880);
    const uint32_t owner_bytes = plan.fixed ? LoadGuestU32(base, recording + 156)
                                            : LoadGuestU32(base, device + 14884) - owner_first;
    MemoryRange data;
    if (plan.first < 4 || plan.bytes < 216 || uint64_t(plan.first) + plan.bytes > UINT32_MAX ||
        !BackingRange(owner_first, owner_bytes, owner) ||
        !BackingRange(plan.first, plan.bytes, data) || data.first < owner.first ||
        data.end > owner.end || Overlaps(owner, dev) || Overlaps(owner, frame) ||
        Overlaps(owner, object))
      return false;
    plan.cursor = plan.first - 4;
    plan.capacity = plan.first + plan.bytes - 60;
    plan.ready = plan.capacity - 160;
    // Keep the current 64 KiB ready-limit patch, even for a larger owned chunk.
    if (plan.ready - plan.cursor > 65536)
      plan.ready = plan.cursor + 65536;
  } else if (!Scratch(base, device, dev, frame, object, owner, plan)) {
    return false;
  }
  if (!(LoadGuestU32(base, device + 22264) & 1) && LoadGuestU32(base, device + 14916)) {
    MemoryRange sync, scratch;
    const uint32_t completed = LoadGuestU32(base, device + 10896);
    if (!completed || (completed & 3) || !BackingRange(completed, 8, sync) || Overlaps(sync, dev) ||
        Overlaps(sync, frame) || Overlaps(sync, object) || Overlaps(sync, owner))
      return false;
    if (!plan.success && (!BackingRange(plan.cursor, 4800, scratch) || Overlaps(sync, scratch)))
      return false;
    const uint32_t submitted = LoadGuestU32(base, device + 10908);
    const uint32_t marker = LoadGuestU32(base, device + 14916);
    const uint32_t finished = LoadGuestU32(base, completed);
    if (submitted - marker < submitted - finished &&
        !(marker == submitted && LoadGuestU32(base, device + 13216)))
      return false;  // A real marker wait or recursive grow is still required.
    plan.marker = true;
  }
  return true;
}

inline bool BuildPlan(const PPCContext& ctx, const uint8_t* base, Plan& plan) {
  return BuildArguments(ctx.r3.u32, ctx.r1.u32, ctx.r4.u32, base, plan);
}

template <typename EnsureWritable>
inline void Apply(const PPCContext& ctx, uint8_t* base, const Plan& plan, EnsureWritable ensure) {
  using recorded_allocator_detail::Store64;
  const uint32_t sp = plan.stack, device = plan.device, parameter = sp - 48;
  Store64(base, sp - 32, ctx.r29.u64);
  Store64(base, sp - 24, ctx.r30.u64);
  Store64(base, sp - 16, ctx.r31.u64);
  StoreGuestU32(base, sp - 8, uint32_t(ctx.lr));
  StoreGuestU32(base, sp - 128, sp);
  if (!plan.already_failed) {
    StoreGuestU32(base, parameter, plan.requested);
    if (plan.fixed) {
      recorded_allocator_detail::Apply(plan.recording, base, device, parameter);
    } else {
      default_allocator_detail::ApplySaved(base, plan.normal, ctx.r1.u64 - 48, 0, ctx.r30.u64,
                                           ctx.r3.u64, 0x82457968);
    }
    if (plan.success) {
      ensure(base + plan.first, 128 * 1024);
      StoreGuestU32(base, device + 14904, plan.first);
      StoreGuestU32(base, device + 14892, 0);
      StoreGuestU32(base, device + 14908, 0);
    }
  }
  if (!plan.success)
    *GuestToHost(base, device + 10941) |= 0x20;
  StoreGuestU32(base, device + 48, plan.cursor);
  StoreGuestU32(base, device + 52, plan.capacity);
  StoreGuestU32(base, device + 56, plan.ready);
  if (plan.marker) {
    // 824574B8's proven early return still writes its original save area.
    Store64(base, sp - 160, 0);
    Store64(base, sp - 152, plan.first);
    Store64(base, sp - 144, ctx.r3.u64);
    StoreGuestU32(base, sp - 136, 0x82457A08);
    StoreGuestU32(base, sp - 272, sp - 128);
    StoreGuestU32(base, device + 14916, 0);
  }
}
}  // namespace chunk_growth_detail

template <typename EnsureWritable>
inline bool GrowCommandChunkWithGuard(PPCContext& ctx, uint8_t* base, EnsureWritable ensure) {
  chunk_growth_detail::Plan plan;
  if (!chunk_growth_detail::BuildPlan(ctx, base, plan))
    return false;
  chunk_growth_detail::Apply(ctx, base, plan, ensure);
  ctx.r3.u64 = plan.first;
  ctx.lr = uint32_t(ctx.lr);
  return true;
}

inline bool GrowCommandChunk(PPCContext& ctx, uint8_t* base) {
  return GrowCommandChunkWithGuard(ctx, base, native_gpu_memory::EnsureCommandStorageWritable);
}

}  // namespace simpsons::native_gpu_vertices
