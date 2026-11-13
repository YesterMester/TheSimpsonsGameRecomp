#pragma once

#include <array>
#include <atomic>
#include <bit>

#include <rex/graphics/native_gpu_sync.h>

#include "simpsons_init.h"
#include "native_gpu_chunk_close.h"

namespace simpsons::native_gpu_vertices {
namespace submission_state_detail {
using namespace native_gpu_memory;
using chunk_close_detail::Load64;
using recorded_allocator_detail::Store64;

constexpr uint64_t kEmptyRange = 0x00000000FFFFFFFFull;
constexpr std::array<PPCRegister PPCContext::*, 12> kSaved = {
    &PPCContext::r20, &PPCContext::r21, &PPCContext::r22, &PPCContext::r23,
    &PPCContext::r24, &PPCContext::r25, &PPCContext::r26, &PPCContext::r27,
    &PPCContext::r28, &PPCContext::r29, &PPCContext::r30, &PPCContext::r31};

inline std::atomic_ref<uint64_t> RangeWord(const uint8_t* base, uint32_t device) {
  return std::atomic_ref<uint64_t>(
      *reinterpret_cast<uint64_t*>(GuestToHost(const_cast<uint8_t*>(base), device + 11816)));
}

// Use the same global critical region and interrupt nesting as the generated
// range producers. The exchange consumes every range published before it;
// later merges remain pending for the next submission.
inline uint64_t DrainRange(PPCContext& ctx, uint8_t* base, uint32_t device) {
  std::atomic_thread_fence(std::memory_order_seq_cst);
  const uint64_t interrupts = REX_CHECK_GLOBAL_LOCK();
  std::atomic_thread_fence(std::memory_order_seq_cst);
  ctx.msr = (ctx.r13.u32 & 0x8020) | (ctx.msr & ~0x8020);
  REX_ENTER_GLOBAL_LOCK();
  const uint64_t previous =
      RangeWord(base, device).exchange(std::byteswap(kEmptyRange), std::memory_order_seq_cst);
  ctx.reserved.u64 = previous;
  std::atomic_thread_fence(std::memory_order_seq_cst);
  ctx.msr = (interrupts & 0x8020) | (ctx.msr & ~0x8020);
  REX_LEAVE_GLOBAL_LOCK();
  return std::byteswap(previous);
}

struct Plan {
  default_allocator_detail::TransientPlan allocation;
  recorded_allocator_detail::TransientPlan recording;
  uint32_t device = 0, stack = 0, address_output = 0, count_output = 0;
  uint32_t words = 0, heap_first = 0, heap_end = 0;
  bool dirty = false, wrap = false, already_failed = false, fixed = false;
};

inline bool BuildPlan(const PPCContext& ctx, const uint8_t* base, Plan& plan) {
  const uint32_t device = ctx.r3.u32, sp = ctx.r1.u32;
  MemoryRange dev, frame, count, address{}, object{}, owner, data, sync;
  if (!base || (device & 7) || (sp & 15) || sp < 560 || !BackingRange(device, 22268, dev) ||
      !BackingRange(sp - 560, 560, frame) || Overlaps(dev, frame) || (ctx.r5.u32 & 3) ||
      !BackingRange(ctx.r5.u32, 4, count) || Overlaps(count, dev) || Overlaps(count, frame) ||
      reinterpret_cast<uintptr_t>(GuestToHost(base, device + 11816)) %
          std::atomic_ref<uint64_t>::required_alignment)
    return false;
  plan.device = device;
  plan.stack = sp;
  plan.address_output = ctx.r4.u32;
  plan.count_output = ctx.r5.u32;
  plan.dirty =
      std::byteswap(RangeWord(base, device).load(std::memory_order_seq_cst)) != kEmptyRange;
  plan.wrap = (*GuestToHost(base, device + 10942) & 4) != 0;
  plan.words = (uint32_t(plan.dirty) + uint32_t(plan.wrap)) * 11;
  if (!plan.words)
    return true;  // The unused address output and heap are not dereferenced.
  plan.already_failed = (*GuestToHost(base, device + 10941) & 0x20) != 0;
  if (plan.already_failed)
    return true;
  // Keep the existing hand patch: 128 dwords, not a reduced byte allocation.
  const uint32_t recording = LoadGuestU32(base, device + 13492);
  plan.fixed = recording != 0;
  uint32_t first, owner_first, owner_bytes;
  if (plan.fixed) {
    if ((recording & 3) || !BackingRange(recording, 184, object) || Overlaps(object, dev) ||
        Overlaps(object, frame) || Overlaps(object, count) ||
        !recorded_allocator_detail::PlanTransient(base, device, sp - 192, 128, 32, plan.recording))
      return false;
    first = plan.recording.allocation.result;
    owner_first = plan.recording.allocation.storage;
    owner_bytes = plan.recording.allocation.bytes;
  } else {
    if (!default_allocator_detail::PlanTransient(base, device, sp - 192, 128, 32, plan.allocation))
      return false;
    first = plan.allocation.allocation.failed ? 0 : plan.allocation.allocation.first;
    owner_first = LoadGuestU32(base, device + 14880);
    owner_bytes = LoadGuestU32(base, device + 14884) - owner_first;
  }
  if (!BackingRange(owner_first, owner_bytes, owner) || Overlaps(owner, dev) ||
      Overlaps(owner, frame) || Overlaps(owner, object))
    return false;
  if (!first)
    return true;
  if (!chunk_close_detail::PhysicalSpan(first, 512, data) || data.first < owner.first ||
      data.end > owner.end || (plan.address_output & 3) ||
      !BackingRange(plan.address_output, 4, address) || Overlaps(address, dev) ||
      Overlaps(address, frame) || Overlaps(address, count) || Overlaps(data, count) ||
      Overlaps(data, address) || Overlaps(address, object))
    return false;
  if (!plan.fixed && (!BackingRange(LoadGuestU32(base, device + 10896), 8, sync) ||
                      Overlaps(sync, count) || Overlaps(sync, address)))
    return false;
  if (plan.wrap) {
    plan.heap_first = LoadGuestU32(base, device + 14880);
    plan.heap_end = LoadGuestU32(base, device + 14884);
    MemoryRange heap;
    if (plan.heap_end <= plan.heap_first ||
        !chunk_close_detail::PhysicalSpan(plan.heap_first,
                                          uint64_t(plan.heap_end) - plan.heap_first, heap) ||
        Overlaps(heap, dev) || Overlaps(heap, frame) || Overlaps(heap, object) ||
        uint64_t(chunk_close_detail::Physical(plan.heap_first)) + plan.heap_end - plan.heap_first !=
            chunk_close_detail::Physical(plan.heap_end))
      return false;
  }
  return true;
}

template <typename EnsureWritable>
inline void Apply(PPCContext& ctx, uint8_t* base, const Plan& plan, EnsureWritable ensure) {
  const uint32_t sp = plan.stack, device = plan.device;
  for (uint32_t i = 0; i < kSaved.size(); ++i)
    Store64(base, sp - 104 + i * 8, (ctx.*kSaved[i]).u64);
  StoreGuestU32(base, sp - 8, uint32_t(ctx.lr));
  StoreGuestU32(base, sp - 192, sp);
  ctx.r1.u32 -= 192;
  ctx.r29.u64 = ctx.r3.u64;
  ctx.r27.u64 = ctx.r29.u64 + 11816;
  ctx.r21.u64 = ctx.r4.u64;
  ctx.r22.u64 = ctx.r5.u64;
  ctx.r26.u64 = 0;
  ctx.r28.u64 = plan.dirty;
  ctx.r24.u64 = plan.wrap;
  ctx.r23.u64 = plan.words;
  if (plan.words) {
    ctx.r4.u64 = 128;
    ctx.r5.u64 = 32;
    ctx.lr = 0x824570BC;
    if (plan.already_failed) {
      StoreGuestU32(base, sp - 200, uint32_t(ctx.lr));
      Store64(base, sp - 208, ctx.r31.u64);
      StoreGuestU32(base, sp - 304, sp - 192);
      ctx.r3.u64 = 0;
    } else if (plan.fixed) {
      recorded_allocator_detail::ApplyTransient(ctx, base, plan.recording);
    } else {
      default_allocator_detail::ApplyTransient(ctx, base, plan.allocation);
    }
    if (ctx.r3.u32) {
      const uint32_t first = ctx.r3.u32;
      ensure(base + first - 4, 256);
      uint32_t at = first;
      const auto write = [&](const rex::graphics::native_gpu_sync::Coherency& call) {
        const auto words = rex::graphics::native_gpu_sync::Encode(call);
        for (uint32_t value : words) {
          StoreGuestU32(base, at, value);
          at += 4;
        }
      };
      if (plan.dirty) {
        const uint64_t range = DrainRange(ctx, base, device);
        StoreGuestU32(base, sp - 112, uint32_t(range));
        StoreGuestU32(base, sp - 108, uint32_t(range >> 32));
        const uint32_t first_page = uint32_t(range) & ~4095u;
        const uint32_t end_page = (uint32_t(range >> 32) + 4095) & ~4095u;
        write({0x03000000, end_page - first_page, first_page});
      }
      if (plan.wrap) {
        const uint32_t first_page = chunk_close_detail::Physical(plan.heap_first) & ~4095u;
        const uint32_t end_page = (chunk_close_detail::Physical(plan.heap_end) + 4095) & ~4095u;
        write({0x01000000, end_page - first_page, first_page});
      }
      StoreGuestU32(base, plan.address_output, chunk_close_detail::Physical(first));
      StoreGuestU32(base, plan.count_output, plan.words);
      ctx.r3.u64 = 0xA31;
    } else {
      StoreGuestU32(base, plan.count_output, 0);
    }
  } else {
    StoreGuestU32(base, plan.count_output, 0);
  }
  ctx.r1.u32 += 192;
  for (uint32_t i = 0; i < kSaved.size(); ++i)
    (ctx.*kSaved[i]).u64 = Load64(base, sp - 104 + i * 8);
  ctx.lr = LoadGuestU32(base, sp - 8);
}
}  // namespace submission_state_detail

template <typename EnsureWritable>
inline bool PrepareSubmissionStateWithGuard(PPCContext& ctx, uint8_t* base, EnsureWritable ensure) {
  submission_state_detail::Plan plan;
  if (!submission_state_detail::BuildPlan(ctx, base, plan))
    return false;
  submission_state_detail::Apply(ctx, base, plan, ensure);
  return true;
}

inline bool PrepareSubmissionState(PPCContext& ctx, uint8_t* base) {
  return PrepareSubmissionStateWithGuard(ctx, base,
                                         native_gpu_memory::EnsureCommandStorageWritable);
}
}  // namespace simpsons::native_gpu_vertices
