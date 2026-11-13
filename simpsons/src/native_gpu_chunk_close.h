#pragma once

#include <array>
#include <bit>
#include <cstring>

#include <rex/graphics/native_gpu_program.h>
#include <rex/graphics/native_gpu_sync.h>

#include "native_gpu_chunk_growth.h"

namespace simpsons::native_gpu_vertices {
namespace chunk_close_detail {
using namespace native_gpu_memory;
using recorded_allocator_detail::Store64;

inline uint64_t Load64(const uint8_t* base, uint32_t address) {
  uint64_t value;
  std::memcpy(&value, GuestToHost(base, address), sizeof(value));
  return std::byteswap(value);
}

inline uint32_t Physical(uint32_t address) {
  return (address & 0x1FFFFFFFu) + (((address >> 20) + 512) & 0x1000u);
}

inline bool PhysicalSpan(uint32_t address, uint64_t bytes, MemoryRange& span) {
  return BackingRange(address, bytes, span) && span.first == 0x100000000ull + Physical(address) &&
         uint64_t(Physical(address)) + bytes <= 0x20000000ull;
}

inline bool Owner(const uint8_t* base, uint32_t device, bool failed, const MemoryRange& dev,
                  const MemoryRange& frame, MemoryRange& owner, MemoryRange& object) {
  uint32_t first, bytes;
  if (failed) {
    first = LoadGuestU32(base, device + 16704);
    bytes = 4800;
  } else {
    const uint32_t recording = LoadGuestU32(base, device + 13492);
    if (recording) {
      if ((recording & 3) || !BackingRange(recording, 184, object) || Overlaps(object, dev) ||
          Overlaps(object, frame) || (LoadGuestU32(base, recording + 108) & 15) != 2)
        return false;
      first = LoadGuestU32(base, recording + 152);
      bytes = LoadGuestU32(base, recording + 156);
    } else {
      first = LoadGuestU32(base, device + 14880);
      const uint32_t end = LoadGuestU32(base, device + 14884);
      if (end <= first || end - first > INT32_MAX)
        return false;
      bytes = end - first;
    }
  }
  return first && !((first | bytes) & 3) && uint64_t(first) + bytes <= UINT32_MAX &&
         BackingRange(first, bytes, owner) && !Overlaps(owner, dev) && !Overlaps(owner, frame) &&
         !Overlaps(owner, object);
}

struct CompletionPlan {
  uint32_t device = 0, output = 0, sync = 0, cursor = 0, version = 0, sequence = 0;
  bool mirror = false;
};

inline bool PlanCompletion(const PPCContext& ctx, const uint8_t* base, CompletionPlan& plan) {
  const uint32_t device = ctx.r3.u32, output = ctx.r4.u32, sp = ctx.r1.u32;
  MemoryRange dev, frame, owner, object{}, commands, sync;
  if (!base || ((device | output) & 3) || (sp & 15) || sp < 40 ||
      !BackingRange(device, 22268, dev) || !BackingRange(sp - 40, 40, frame) ||
      Overlaps(dev, frame) || (*GuestToHost(base, device + 10940) & 0x80) ||
      LoadGuestU32(base, device + 13216) || LoadGuestU32(base, device + 13492) ||
      !Owner(base, device, false, dev, frame, owner, object))
    return false;
  const uint32_t cursor = LoadGuestU32(base, device + 48);
  const uint32_t capacity = LoadGuestU32(base, device + 52);
  if ((cursor | capacity) & 3 || uint64_t(cursor) + 4 != output ||
      uint64_t(output) + 40 > uint64_t(capacity) + 4 || !PhysicalSpan(output, 40, commands) ||
      commands.first < owner.first || commands.end > owner.end || Overlaps(commands, dev) ||
      Overlaps(commands, frame))
    return false;
  plan.sync = LoadGuestU32(base, device + 10896);
  if (!plan.sync || (plan.sync & 3) || !PhysicalSpan(plan.sync, 8, sync) ||
      Overlaps(sync, commands) || Overlaps(sync, dev) || Overlaps(sync, frame))
    return false;
  plan.device = device;
  plan.output = output;
  plan.cursor = cursor;
  plan.version = LoadGuestU32(base, device + 14896);
  plan.sequence = LoadGuestU32(base, device + 10908);
  plan.mirror = !LoadGuestU32(base, device + 21508) && (*GuestToHost(base, device + 10941) & 2);
  return true;
}

inline void ApplyCompletion(const PPCContext& ctx, uint8_t* base, const CompletionPlan& plan) {
  using rex::graphics::native_gpu_program::kInvalidateHeader;
  using rex::graphics::native_gpu_sync::kEventHeader;
  const uint32_t sp = ctx.r1.u32;
  Store64(base, sp - 40, ctx.r28.u64);
  Store64(base, sp - 32, ctx.r29.u64);
  Store64(base, sp - 24, ctx.r30.u64);
  Store64(base, sp - 16, ctx.r31.u64);
  StoreGuestU32(base, sp - 8, uint32_t(ctx.lr));
  const uint32_t packed = plan.cursor | (plan.version & 3);
  StoreGuestU32(base, plan.device + 13228, plan.version);
  StoreGuestU32(base, plan.device + 13224, plan.cursor);
  const std::array<uint32_t, 10> words = {
      kInvalidateHeader,           0x7FFF,       kEventHeader, 3,
      Physical(plan.sync + 4) | 2, packed,       kEventHeader, 3,
      Physical(plan.sync) | 2,     plan.sequence};
  for (uint32_t i = 0; i < words.size(); ++i)
    StoreGuestU32(base, plan.output + i * 4, words[i]);
  // Retain the original CPU-mirror mode; this is not a new completion signal.
  if (plan.mirror) {
    StoreGuestU32(base, plan.sync, plan.sequence);
    StoreGuestU32(base, plan.sync + 4, packed);
  }
  StoreGuestU32(base, plan.device + 10908, plan.sequence + 2);
}

struct ClosePlan {
  chunk_growth_detail::Plan growth;
  uint32_t device = 0, stack = 0, chunk = 0, next = 0, aligned = 0;
  uint32_t descriptor = 0, words = 0;
  bool reserved = false, grow = false;
};

inline bool PlanCloseArguments(uint32_t device, uint32_t sp, const uint8_t* base,
                               ClosePlan& plan) {
  MemoryRange dev, frame, owner, object{}, data{}, descriptors{};
  if (!base || (device & 3) || (sp & 15) || sp < 512 || !BackingRange(device, 22268, dev) ||
      !BackingRange(sp - 512, 512, frame) || Overlaps(dev, frame))
    return false;
  const bool failed = (*GuestToHost(base, device + 10941) & 0x20) != 0;
  const bool recording = !failed && (*GuestToHost(base, device + 10940) & 0x80);
  plan.reserved = !failed && !recording && LoadGuestU32(base, device + 13216);
  // Normal ring submission remains in the real driver. Its separate completion
  // writer can still use the native formats through its public entry point.
  if ((!failed && !recording && !plan.reserved) ||
      !Owner(base, device, failed, dev, frame, owner, object))
    return false;
  const uint32_t cursor = LoadGuestU32(base, device + 48);
  if ((cursor & 3) || uint64_t(cursor) + 35 > UINT32_MAX)
    return false;
  plan.device = device;
  plan.stack = sp;
  plan.chunk = LoadGuestU32(base, device + 14904);
  plan.next = cursor + 4;
  plan.aligned = (plan.next + 31) & ~31u;
  MemoryRange next_word;
  if (!BackingRange(plan.next, 4, next_word) || next_word.first < owner.first ||
      next_word.end > owner.end)
    return false;
  if (plan.reserved) {
    if ((plan.chunk & 3) || plan.next < plan.chunk || (plan.next - plan.chunk) / 4 > 0xFFFFFF)
      return false;
    plan.words = (plan.next - plan.chunk) / 4;
    if (plan.words) {
      const uint32_t block = LoadGuestU32(base, device + 13356);
      plan.descriptor = LoadGuestU32(base, device + 13360);
      const uint32_t limit = LoadGuestU32(base, device + 13364);
      const uint64_t command_end = uint64_t(LoadGuestU32(base, device + 52)) + 60;
      if (!block || (block & 127) || uint64_t(block) + 4220 != limit ||
          plan.descriptor < uint64_t(block) + 4 || (uint64_t(plan.descriptor) - block - 4) % 8 ||
          uint64_t(plan.descriptor) + 8 > limit || !BackingRange(block, 4224, descriptors) ||
          command_end < plan.next || command_end > UINT32_MAX ||
          !PhysicalSpan(plan.chunk, command_end - plan.chunk, data) || data.first < owner.first ||
          data.end > owner.end || Overlaps(descriptors, data) || Overlaps(descriptors, dev) ||
          Overlaps(descriptors, frame) || Overlaps(descriptors, object))
        return false;
      const uint32_t completed = LoadGuestU32(base, device + 10896);
      MemoryRange sync;
      if (completed && !(completed & 3) && BackingRange(completed, 8, sync) &&
          Overlaps(descriptors, sync))
        return false;
    }
  }
  const uint32_t capacity = LoadGuestU32(base, device + 52);
  const uint32_t ready = LoadGuestU32(base, device + 56);
  if (((capacity | ready) & 3) || ready > capacity)
    return false;
  plan.grow = plan.aligned > ready;
  if (plan.grow) {
    if (!chunk_growth_detail::BuildArguments(device, sp - 128, 0, base, plan.growth))
      return false;
    MemoryRange next_owner;
    const uint32_t first = plan.growth.success ? plan.growth.first : plan.growth.cursor;
    const uint32_t bytes = plan.growth.success ? plan.growth.bytes : 4800;
    if (!BackingRange(first, bytes, next_owner) || Overlaps(next_owner, descriptors))
      return false;
  } else {
    MemoryRange aligned_word;
    if (!BackingRange(plan.aligned, 4, aligned_word) || aligned_word.first < owner.first ||
        aligned_word.end > owner.end || Overlaps(aligned_word, descriptors))
      return false;
  }
  return true;
}

inline bool PlanClose(const PPCContext& ctx, const uint8_t* base, ClosePlan& plan) {
  return PlanCloseArguments(ctx.r3.u32, ctx.r1.u32, base, plan);
}

template <typename EnsureWritable>
inline void ApplyClose(PPCContext& ctx, uint8_t* base, const ClosePlan& plan,
                       EnsureWritable ensure) {
  const uint32_t sp = plan.stack, device = plan.device;
  Store64(base, sp - 40, ctx.r28.u64);
  Store64(base, sp - 32, ctx.r29.u64);
  Store64(base, sp - 24, ctx.r30.u64);
  Store64(base, sp - 16, ctx.r31.u64);
  StoreGuestU32(base, sp - 8, uint32_t(ctx.lr));
  StoreGuestU32(base, sp - 128, sp);
  ctx.r1.u32 -= 128;
  ctx.r31.u64 = ctx.r3.u64;
  ctx.r30.u64 = plan.chunk;
  ctx.r28.u64 = plan.next;
  if (plan.reserved) {
    ctx.r29.u64 = plan.words;
    ctx.r30.u64 = Physical(plan.chunk);
    if (plan.words) {
      ctx.r3.u64 = ctx.r31.u64 + 13352;
      StoreGuestU32(base, plan.descriptor + 4, ctx.r30.u32);
      StoreGuestU32(base, plan.descriptor, plan.words | 0x81000000u);
      StoreGuestU32(base, device + 13360, plan.descriptor + 8);
    }
  }
  if (plan.grow) {
    StoreGuestU32(base, device + 48, plan.next - 4);
    ctx.r4.u64 = 0;
    ctx.r3.u64 = ctx.r31.u64;
    ctx.lr = 0x82457CA8;
    chunk_growth_detail::Apply(ctx, base, plan.growth, ensure);
    ctx.r3.u64 = plan.growth.first;
  } else {
    StoreGuestU32(base, device + 14904, plan.aligned);
    StoreGuestU32(base, device + 48, plan.aligned - 4);
  }
  ctx.r1.s64 += 128;
  ctx.r28.u64 = Load64(base, sp - 40);
  ctx.r29.u64 = Load64(base, sp - 32);
  ctx.r30.u64 = Load64(base, sp - 24);
  ctx.r31.u64 = Load64(base, sp - 16);
  ctx.lr = LoadGuestU32(base, sp - 8);
}
}  // namespace chunk_close_detail

inline bool WriteChunkCompletion(PPCContext& ctx, uint8_t* base) {
  chunk_close_detail::CompletionPlan plan;
  if (!chunk_close_detail::PlanCompletion(ctx, base, plan))
    return false;
  chunk_close_detail::ApplyCompletion(ctx, base, plan);
  ctx.r3.u64 = ctx.r4.u64 + 40;
  ctx.lr = uint32_t(ctx.lr);
  return true;
}

template <typename EnsureWritable>
inline bool CloseCommandChunkWithGuard(PPCContext& ctx, uint8_t* base, EnsureWritable ensure) {
  chunk_close_detail::ClosePlan plan;
  if (!chunk_close_detail::PlanClose(ctx, base, plan))
    return false;
  chunk_close_detail::ApplyClose(ctx, base, plan, ensure);
  return true;
}

inline bool CloseCommandChunk(PPCContext& ctx, uint8_t* base) {
  return CloseCommandChunkWithGuard(ctx, base, native_gpu_memory::EnsureCommandStorageWritable);
}

}  // namespace simpsons::native_gpu_vertices
