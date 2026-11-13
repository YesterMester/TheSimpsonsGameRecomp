#pragma once

#include <algorithm>
#include <array>
#include <rex/graphics/native_gpu_indirect.h>
#include <rex/ppc/context.h>

#include "guest_memory.h"
#include "native_gpu_memory.h"

namespace simpsons::native_gpu_calls {
namespace shader_constants_detail {
using namespace native_gpu_memory;

struct Record {
  uint32_t physical, index, count, source;
};
struct Plan {
  std::array<Record, 128> records;
  uint32_t device = 0, initial = 0, cursor = 0, count = 0;
  MemoryRange metadata{};
};

// 8245F810's entire descriptor loop. Sources remain deferred command-thread
// reads; planning never copies their data onto the game thread.
inline bool BuildPlan(const PPCContext& ctx, const uint8_t* base, Plan& plan,
                      const uint32_t* initial = nullptr) {
  const uint32_t device = ctx.r3.u32, resource = ctx.r4.u32;
  MemoryRange dev, stack, commands;
  auto& metadata = plan.metadata;
  if (!base || ((device | resource) & 3) || (ctx.r1.u32 & 15) || ctx.r1.u32 < 144 ||
      !BackingRange(device, 22256, dev) || !BackingRange(ctx.r1.u32 - 144, 144, stack) ||
      !BackingRange(resource, 24, metadata) || Overlaps(dev, stack) || Overlaps(metadata, dev) ||
      Overlaps(metadata, stack))
    return false;
  plan.device = device;
  plan.count = 0;
  const uint32_t relative = LoadGuestU32(base, resource + 20);
  if (!relative)
    return true;
  const uint64_t header = uint64_t(resource) + relative;
  if ((relative & 3) || header + 20 > 0x100000000ull ||
      !BackingRange(resource, std::max(uint64_t(24), header + 20 - resource), metadata) ||
      Overlaps(metadata, dev) || Overlaps(metadata, stack))
    return false;
  const uint32_t bytes = LoadGuestU32(base, uint32_t(header) + 16);
  const uint64_t end = header + 20 + bytes;
  if ((bytes & 3) || bytes > 65536 || end > 0x100000000ull ||
      !BackingRange(resource, std::max(uint64_t(24), end - resource), metadata) ||
      Overlaps(metadata, dev) || Overlaps(metadata, stack))
    return false;
  for (uint64_t position = header + 20; position < end; position += 8) {
    if (position + 4 > end)
      return false;
    const uint32_t count = LoadGuestU16(base, uint32_t(position) + 2);
    if (!count)
      break;
    if (position + 8 > end || plan.count == plan.records.size())
      return false;
    const uint32_t index = uint32_t(LoadGuestU16(base, uint32_t(position))) * 4;
    const uint32_t source = uint32_t(ctx.r5.u64 + LoadGuestU32(base, uint32_t(position) + 4));
    const uint32_t physical = (source & 0x1FFFFFFFu) + (((source >> 20) + 512) & 0x1000u);
    if (!rex::graphics::native_gpu_indirect::Validate({physical, index, count}))
      return false;
    plan.records[plan.count++] = {physical, index, count, source};
  }
  if (!plan.count)
    return true;
  plan.initial = plan.cursor = initial ? *initial : LoadGuestU32(base, device + 48);
  const uint32_t limit = LoadGuestU32(base, device + 52);
  const uint32_t ready = LoadGuestU32(base, device + 56);
  if (!plan.initial || (plan.initial & 3) || uint64_t(plan.initial) + plan.count * 16 > limit ||
      uint64_t(plan.initial) + (plan.count - 1) * 16 > ready ||
      !BackingRange(plan.initial + 4, plan.count * 16, commands) || Overlaps(commands, dev) ||
      Overlaps(commands, stack) || Overlaps(commands, metadata))
    return false;
  for (uint32_t i = 0; i < plan.count; ++i) {
    const auto& record = plan.records[i];
    MemoryRange data;
    const uint64_t physical = 0x100000000ull + record.physical;
    if (!BackingRange(record.source, uint64_t(record.count) * 4, data) || data.first != physical ||
        data.end != physical + uint64_t(record.count) * 4 || Overlaps(data, commands) ||
        Overlaps(data, stack))
      return false;
  }
  plan.cursor += plan.count * 16;
  return true;
}

inline void Apply(const Plan& plan, uint8_t* base) {
  uint32_t cursor = plan.initial;
  for (uint32_t i = 0; i < plan.count; ++i) {
    const auto& record = plan.records[i];
    StoreGuestU32(base, cursor + 4, rex::graphics::native_gpu_indirect::kHeader);
    StoreGuestU32(base, cursor + 8, record.physical);
    StoreGuestU32(base, cursor + 12, record.index);
    StoreGuestU32(base, cursor + 16, record.count);
    cursor += 16;
    StoreGuestU32(base, plan.device + 48, cursor);
  }
}
}  // namespace shader_constants_detail

inline bool LoadShaderConstants(PPCContext& ctx, uint8_t* base) {
  shader_constants_detail::Plan plan;
  if (!shader_constants_detail::BuildPlan(ctx, base, plan))
    return false;
  shader_constants_detail::Apply(plan, base);
  if (plan.count)
    ctx.r3.u64 = plan.cursor;
  ctx.lr = uint32_t(ctx.lr);
  return true;
}

}  // namespace simpsons::native_gpu_calls
