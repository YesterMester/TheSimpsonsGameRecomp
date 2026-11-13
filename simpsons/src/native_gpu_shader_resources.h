#pragma once

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstring>

// Share the generated callers' interrupt nesting counter and the runtime's
// global critical region. A separate lock would not coordinate with them.
#include "simpsons_init.h"
#include "guest_memory.h"
#include "native_gpu_memory.h"
#include "native_gpu_vertex_declaration.h"

namespace simpsons::native_gpu_calls {
namespace shader_resource_detail {
using namespace native_gpu_memory;
constexpr uint32_t kVersionCounter = 0x82D51560;

inline uint64_t Load64(const uint8_t* base, uint32_t address) {
  uint64_t value;
  std::memcpy(&value, GuestToHost(base, address), sizeof(value));
  return std::byteswap(value);
}

inline void Store64(uint8_t* base, uint32_t address, uint64_t value) {
  value = std::byteswap(value);
  std::memcpy(GuestToHost(base, address), &value, sizeof(value));
}

inline uint32_t ReserveVersion(PPCContext& ctx, uint8_t* base) {
  auto& storage = *reinterpret_cast<uint32_t*>(GuestToHost(base, kVersionCounter));
  std::atomic_ref<uint32_t> counter(storage);
  for (;;) {
    // Keep the same critical region, interrupt-state restoration and sequential
    // ordering as guest atomic writers. The host word contains big-endian data.
    std::atomic_thread_fence(std::memory_order_seq_cst);
    const uint64_t interrupt_state = REX_CHECK_GLOBAL_LOCK();
    std::atomic_thread_fence(std::memory_order_seq_cst);
    ctx.msr = (ctx.r13.u32 & 0x8020) | (ctx.msr & ~0x8020);
    REX_ENTER_GLOBAL_LOCK();
    uint32_t observed = counter.load(std::memory_order_relaxed);
    const uint32_t next = std::byteswap(observed) + 1;
    ctx.reserved.u32 = observed;
    const bool exchanged =
        counter.compare_exchange_strong(observed, std::byteswap(next), std::memory_order_seq_cst);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    ctx.msr = (interrupt_state & 0x8020) | (ctx.msr & ~0x8020);
    REX_LEAVE_GLOBAL_LOCK();
    if (exchanged && uint32_t(next + 1) >= 2)
      return next;
  }
}

inline void Publish(uint8_t* base, uint32_t device) {
  Store64(base, device + 11824, Load64(base, device + 12520));
  Store64(base, device + 11832, Load64(base, device + 12528));
}

inline void Return(PPCContext& ctx, uint32_t value) {
  ctx.r3.u64 = value;
  ctx.lr = uint32_t(ctx.lr);
}
}  // namespace shader_resource_detail

namespace shader_resource_detail {
enum class Kind { kCached, kBusy, kRebuild };
struct Plan {
  Plan() noexcept {}
  vertex_declaration_detail::Plan patch;
  MemoryRange device_range{}, object{}, attributes{}, stack{}, fence_word{}, counter{};
  uint32_t device = 0, record = 0, declaration = 0, version = 0, saved_version = 0;
  Kind kind = Kind::kBusy;
  bool version_pending = false;
  bool cache_possible = false;
};

inline bool BuildPlan(const PPCContext& ctx, const uint8_t* base, Plan& result,
                      const Plan* preceding = nullptr) {
  const uint32_t device = ctx.r3.u32, resource = ctx.r4.u32, declaration = ctx.r5.u32;
  const uint64_t variant = ctx.r6.u64, record_offset = variant * 416;
  auto& dev = result.device_range;
  auto& object = result.object;
  auto& attributes = result.attributes;
  auto& stack = result.stack;
  auto& fence_word = result.fence_word;
  auto& counter = result.counter;
  if (!base || ((device | resource | declaration) & 3) || (ctx.r1.u32 & 15) || ctx.r1.u32 < 728 ||
      variant > UINT32_MAX || record_offset + 68 > 0x100000000ull - resource ||
      !BackingRange(device, 22256, dev) || !BackingRange(resource, record_offset + 68, object) ||
      !BackingRange(declaration, 52, attributes) || !BackingRange(ctx.r1.u32 - 728, 728, stack) ||
      Overlaps(dev, object) || Overlaps(dev, attributes) || Overlaps(object, attributes) ||
      Overlaps(dev, stack) || Overlaps(object, stack) || Overlaps(attributes, stack))
    return false;
  const uint32_t record = resource + uint32_t(record_offset);
  const uint32_t version = LoadGuestU32(base, declaration + 48);
  result.device = device;
  result.record = record;
  result.declaration = declaration;
  result.version = version;
  result.version_pending = preceding != nullptr;
  result.cache_possible = false;
  const uint32_t saved_version = LoadGuestU32(base, record + 40);
  if (preceding) {
    MemoryRange prior_record, current_record;
    // The first real allocation supplies the ID. Plan the miss now, retain a
    // possible cache hit, and decide from the published declaration during Apply.
    if (preceding->kind != Kind::kRebuild || preceding->version || preceding->version_pending ||
        preceding->declaration != declaration || preceding->device != device || version ||
        !BackingRange(preceding->record, 68, prior_record) ||
        !BackingRange(record, 68, current_record) || Overlaps(prior_record, current_record))
      return false;
    result.saved_version = saved_version;
  }
  const bool version_can_match = result.version_pending
                                     ? saved_version && saved_version != UINT32_MAX
                                     : saved_version == version;
  const bool cached =
      version_can_match && !(((Load64(base, record + 48) ^ Load64(base, device + 12520)) &
                              Load64(base, declaration + 32)) |
                             ((Load64(base, record + 56) ^ Load64(base, device + 12528)) &
                              Load64(base, declaration + 40)));
  if (result.version_pending) {
    result.cache_possible = cached;
  } else if (cached) {
    result.kind = Kind::kCached;
    return true;
  }
  const uint32_t resource_fence = LoadGuestU32(base, record + 64);
  if (resource_fence) {
    const uint32_t completed_address = LoadGuestU32(base, device + 10896);
    if ((completed_address & 3) || !BackingRange(completed_address, 4, fence_word) ||
        Overlaps(fence_word, stack))
      return false;
    const uint32_t current = LoadGuestU32(base, device + 10908);
    const uint32_t completed = LoadGuestU32(base, completed_address);
    if (uint32_t(current - resource_fence) < uint32_t(current - completed)) {
      result.kind = Kind::kBusy;
      return true;
    }
  }
  const uint64_t selector = (variant + 112) * 8;
  if (uint64_t(resource) + selector + 4 > 0x100000000ull ||
      !BackingRange(resource, std::max(record_offset + 68, selector + 4), object) ||
      Overlaps(object, dev) || Overlaps(object, attributes) || Overlaps(object, stack))
    return false;
  const uint64_t selected = uint64_t(resource) + LoadGuestU32(base, resource + uint32_t(selector));
  if (selected + 876 > 0x100000000ull ||
      !BackingRange(resource, std::max(record_offset + 68, selected + 876 - resource), object) ||
      Overlaps(object, dev) || Overlaps(object, attributes) || Overlaps(object, stack))
    return false;
  const uint64_t code =
      uint64_t(LoadGuestU32(base, resource + 32)) + LoadGuestU32(base, uint32_t(selected) + 872);
  auto& plan = result.patch;
  if (code > UINT32_MAX ||
      !vertex_declaration_detail::BuildPlan(
          {resource, declaration, device + 12520, ctx.r1.u32 - 128, code, variant}, base, plan))
    return false;
  object.end = std::max(object.end, plan.metadata.end);
  if (plan.count)
    attributes = plan.attributes;
  if (Overlaps(object, dev) || Overlaps(object, attributes) || Overlaps(object, stack) ||
      Overlaps(attributes, dev) || Overlaps(attributes, stack) ||
      vertex_declaration_detail::WritesOverlap(plan, dev) ||
      vertex_declaration_detail::WritesOverlap(plan, stack) ||
      vertex_declaration_detail::WritesOverlap(plan, fence_word))
    return false;
  if (!version && !result.version_pending) {
    if (!BackingRange(kVersionCounter, 4, counter) || Overlaps(counter, dev) ||
        Overlaps(counter, object) || Overlaps(counter, attributes) || Overlaps(counter, stack) ||
        Overlaps(counter, fence_word) ||
        reinterpret_cast<uintptr_t>(GuestToHost(base, kVersionCounter)) %
            std::atomic_ref<uint32_t>::required_alignment ||
        vertex_declaration_detail::WritesOverlap(plan, counter))
      return false;
  }
  result.kind = Kind::kRebuild;
  return true;
}

inline bool MutableOverlaps(const Plan& plan, const MemoryRange& range) {
  if (plan.kind != Kind::kRebuild)
    return false;
  MemoryRange versions;
  if (!BackingRange(plan.record + 40, 24, versions) || Overlaps(versions, range) ||
      vertex_declaration_detail::WritesOverlap(plan.patch, range))
    return true;
  if (!plan.version && !plan.version_pending) {
    MemoryRange declaration_version;
    if (!BackingRange(plan.declaration + 48, 4, declaration_version) ||
        Overlaps(declaration_version, range) || Overlaps(plan.counter, range))
      return true;
  }
  return false;
}

inline Kind Apply(const Plan& plan, PPCContext& ctx, uint8_t* base) {
  const Kind kind = plan.version_pending && plan.cache_possible &&
                            LoadGuestU32(base, plan.declaration + 48) == plan.saved_version
                        ? Kind::kCached
                        : plan.kind;
  if (kind == Kind::kBusy)
    return kind;
  if (kind == Kind::kRebuild) {
    vertex_declaration_detail::Apply(plan.patch, base);
    if (!plan.version && !plan.version_pending)
      StoreGuestU32(base, plan.declaration + 48, ReserveVersion(ctx, base));
    StoreGuestU32(base, plan.record + 40, LoadGuestU32(base, plan.declaration + 48));
    Store64(base, plan.record + 48, Load64(base, plan.device + 12520));
    Store64(base, plan.record + 56, Load64(base, plan.device + 12528));
  }
  Publish(base, plan.device);
  return kind;
}
}  // namespace shader_resource_detail

// 8245F8C0: planning is separate so a coordinator can prove later writes before
// publishing code, reserving a shared version or updating any snapshots.
inline bool PrepareShaderResource(PPCContext& ctx, uint8_t* base) {
  shader_resource_detail::Plan plan;
  if (!shader_resource_detail::BuildPlan(ctx, base, plan))
    return false;
  const auto kind = shader_resource_detail::Apply(plan, ctx, base);
  shader_resource_detail::Return(ctx, kind == shader_resource_detail::Kind::kBusy ? 0 : 1);
  return true;
}

}  // namespace simpsons::native_gpu_calls
