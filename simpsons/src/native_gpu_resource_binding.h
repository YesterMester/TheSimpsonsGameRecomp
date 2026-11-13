#pragma once

#include <algorithm>
#include <array>
#include <cstring>

#include "guest_memory.h"
#include "native_gpu_memory.h"
#include "native_gpu_shader_resources.h"

namespace simpsons::native_gpu_calls {
namespace resource_binding_detail {
using namespace native_gpu_memory;
using shader_resource_detail::Load64;
using shader_resource_detail::Store64;

struct Retirement {
  MemoryRange resource{}, owner{};
  uint32_t address = 0, fence = 0, cursor = 0, packed = 0;
  bool enqueue = false;
};

inline bool Frame(const PPCContext& ctx, const uint8_t* base, uint32_t bytes, MemoryRange& device,
                  MemoryRange& stack) {
  return base && !(ctx.r3.u32 & 3) && !(ctx.r1.u32 & 15) && ctx.r1.u32 >= bytes &&
         BackingRange(ctx.r3.u32, 22256, device) &&
         BackingRange(ctx.r1.u32 - bytes, bytes, stack) && !Overlaps(device, stack);
}

inline bool PlanRetirement(const PPCContext& ctx, const uint8_t* base, uint32_t pointer_offset,
                           uint32_t frame_bytes, const MemoryRange& device,
                           const MemoryRange& stack, Retirement& plan) {
  const uint32_t dev = ctx.r3.u32;
  plan.address = LoadGuestU32(base, dev + pointer_offset);
  if (!plan.address)
    return true;
  if ((plan.address & 3) || !BackingRange(plan.address, 12, plan.resource) ||
      Overlaps(plan.resource, device) || Overlaps(plan.resource, stack))
    return false;
  plan.fence = LoadGuestU32(base, dev + 10908);
  if (plan.fence || !(LoadGuestU32(base, dev + 10912) & LoadGuestU32(base, plan.address)))
    return true;
  if (*GuestToHost(base, dev + 10941) & 0x20)
    return false;
  const uint32_t first = LoadGuestU32(base, dev + 13496);
  const uint32_t limit = LoadGuestU32(base, dev + 13504);
  plan.cursor = LoadGuestU32(base, dev + 13500);
  if (!first || ((first | limit | plan.cursor) & 7) || uint64_t(first) + 2008 != limit ||
      plan.cursor < uint64_t(first) + 8 || uint64_t(plan.cursor) + 8 > limit ||
      !BackingRange(first, 2008, plan.owner) || Overlaps(plan.owner, device) ||
      Overlaps(plan.owner, stack) || Overlaps(plan.owner, plan.resource))
    return false;
  // The capture bit is taken from the original private frame, not invented or
  // cleared. Retain the same two words and eight-byte queue publication.
  plan.packed =
      (LoadGuestU32(base, ctx.r1.u32 - frame_bytes + 80) & 0x80000000u) | (plan.address >> 2);
  plan.enqueue = true;
  return true;
}

inline bool SeparateInput(const Retirement& retirement, const MemoryRange& input,
                          uint32_t address) {
  return !Overlaps(retirement.owner, input) &&
         (retirement.address == address || !Overlaps(retirement.resource, input));
}

inline void Retire(const Retirement& plan, uint8_t* base, uint32_t device) {
  if (plan.address && plan.fence)
    StoreGuestU32(base, plan.address + 8, plan.fence);
  if (plan.enqueue) {
    Store64(base, plan.cursor, (uint64_t(plan.packed) << 32) | UINT32_MAX);
    StoreGuestU32(base, device + 13500, plan.cursor + 8);
  }
}

struct ConstantPatch {
  uint32_t source, offset, words;
  bool masked;
};
struct ShaderBinding {
  std::array<ConstantPatch, 128> patches;
  MemoryRange resource{};
  Retirement retirement;
  uint64_t clear_mask = 0;
  uint32_t count = 0, last_copy_offset = 0;
  bool metadata = false, misc_dirty = false, copied = false;
};

inline bool ShadowSpan(uint32_t offset, uint32_t bytes) {
  // The flushers' fetch/ALU/bool/loop/render shadow precedes the completed-fence
  // pointer at +10896. Binding and lifetime fields are outside this interval.
  return uint64_t(offset) + bytes <= 10896 - 1152;
}

inline bool PlanShader(const PPCContext& ctx, const uint8_t* base, bool vertex,
                       ShaderBinding& plan) {
  const uint32_t dev = ctx.r3.u32, incoming = ctx.r4.u32, prefix = vertex ? 872 : 40;
  MemoryRange device, stack;
  if (!Frame(ctx, base, 136, device, stack) ||
      !PlanRetirement(ctx, base, vertex ? 12688 : 12684, 128, device, stack, plan.retirement))
    return false;
  if (!incoming)
    return true;
  if ((incoming & 3) || !BackingRange(incoming, prefix + 24, plan.resource) ||
      Overlaps(plan.resource, device) || Overlaps(plan.resource, stack) ||
      !SeparateInput(plan.retirement, plan.resource, incoming))
    return false;
  const uint32_t relative = LoadGuestU32(base, incoming + prefix + 20);
  if (!relative)
    return true;
  const uint64_t header = uint64_t(incoming) + prefix + relative;
  if ((relative & 3) || header + 20 > 0x100000000ull ||
      !BackingRange(incoming, header + 20 - incoming, plan.resource) ||
      Overlaps(plan.resource, device) || Overlaps(plan.resource, stack) ||
      !SeparateInput(plan.retirement, plan.resource, incoming))
    return false;
  const uint32_t bytes = LoadGuestU32(base, uint32_t(header) + 16);
  const uint64_t end = header + 20 + bytes;
  if ((bytes & 3) || bytes > 65536 || end > 0x100000000ull ||
      !BackingRange(incoming, end - incoming, plan.resource) || Overlaps(plan.resource, device) ||
      Overlaps(plan.resource, stack) || !SeparateInput(plan.retirement, plan.resource, incoming))
    return false;
  plan.metadata = true;
  plan.clear_mask = Load64(base, uint32_t(header));
  plan.misc_dirty = Load64(base, uint32_t(header) + 8) != 0;
  uint64_t cursor = header + 20;
  while (cursor < end) {
    const uint32_t words = LoadGuestU16(base, uint32_t(cursor) + 2);
    cursor += 4;
    if (!words)
      break;
    // The skipped descriptor's value is never read here. A trailing header may
    // advance beyond the declared table without causing another guest access.
    cursor += 4;
  }
  bool masked = false;
  while (cursor < end) {
    const uint32_t offset = LoadGuestU16(base, uint32_t(cursor));
    const uint32_t words = LoadGuestU16(base, uint32_t(cursor) + 2);
    cursor += 4;
    if (!words) {
      if (masked)
        break;
      masked = true;
      continue;
    }
    const uint32_t data_bytes = words * 4, output_bytes = masked ? words * 2 : data_bytes;
    if ((masked && (words & 1)) || cursor + data_bytes > end || !ShadowSpan(offset, output_bytes) ||
        plan.count == plan.patches.size())
      return false;
    plan.patches[plan.count++] = {uint32_t(cursor), offset, words, masked};
    if (!masked) {
      plan.copied = true;
      plan.last_copy_offset = offset;
    }
    cursor += data_bytes;
  }
  return true;
}

inline void PatchConstants(const ShaderBinding& plan, uint8_t* base, uint32_t device, bool vertex) {
  if (!plan.metadata)
    return;
  const uint32_t mask_address = device + (vertex ? 0 : 8);
  Store64(base, mask_address, Load64(base, mask_address) & ~plan.clear_mask);
  if (plan.misc_dirty)
    Store64(base, device + 32, Load64(base, device + 32) | (1ull << 56));
  for (uint32_t i = 0; i < plan.count; ++i) {
    const auto& patch = plan.patches[i];
    const uint32_t output = device + 1152 + patch.offset;
    if (!patch.masked) {
      std::memcpy(GuestToHost(base, output), GuestToHost(base, patch.source), patch.words * 4);
    } else {
      for (uint32_t j = 0; j < patch.words / 2; ++j) {
        const uint32_t mask = LoadGuestU32(base, patch.source + j * 8);
        const uint32_t value = LoadGuestU32(base, patch.source + j * 8 + 4);
        StoreGuestU32(base, output + j * 4, (LoadGuestU32(base, output + j * 4) & mask) | value);
      }
    }
  }
}
}  // namespace resource_binding_detail

inline bool BindShaderResource(PPCContext& ctx, uint8_t* base, bool vertex) {
  using namespace resource_binding_detail;
  ShaderBinding plan;
  if (!PlanShader(ctx, base, vertex, plan))
    return false;
  const uint32_t device = ctx.r3.u32, incoming = ctx.r4.u32;
  uint64_t result = plan.retirement.enqueue ? plan.retirement.cursor : ctx.r3.u64;
  if (vertex && incoming)
    Store64(base, device + 16, Load64(base, device + 16) | 0x80000);
  Retire(plan.retirement, base, device);
  StoreGuestU32(base, device + (vertex ? 12688 : 12684), incoming);
  if (!vertex) {
    Store64(base, device + 16, Load64(base, device + 16) | 0x100000);
    Store64(base, device + 16, Load64(base, device + 16) | 0x20000);
  } else {
    *GuestToHost(base, device + 10942) &= 0x7F;
  }
  PatchConstants(plan, base, device, vertex);
  if (plan.copied)
    result = ctx.r3.u64 + 1152 + plan.last_copy_offset;
  ctx.r3.u64 = result;
  ctx.lr = uint32_t(ctx.lr);
  return true;
}

inline bool BindTextureResource(PPCContext& ctx, uint8_t* base) {
  using namespace resource_binding_detail;
  const uint32_t device = ctx.r3.u32, slot = ctx.r4.u32, incoming = ctx.r5.u32;
  MemoryRange dev, stack, texture{};
  Retirement retirement;
  if (slot > 31 || !Frame(ctx, base, 160, dev, stack) ||
      !PlanRetirement(ctx, base, 12536 + slot * 4, 160, dev, stack, retirement) ||
      (incoming &&
       ((incoming & 3) || !BackingRange(incoming, 52, texture) || Overlaps(texture, dev) ||
        Overlaps(texture, stack) || !SeparateInput(retirement, texture, incoming))))
    return false;
  uint64_t result = ctx.r3.u64;
  if (incoming) {
    const uint32_t output = device + (slot + 48) * 24;
    const uint32_t source0 = LoadGuestU32(base, incoming + 28);
    const uint32_t raw1 = LoadGuestU32(base, incoming + 32);
    const uint32_t source2 = LoadGuestU32(base, incoming + 36);
    const uint32_t source3 = LoadGuestU32(base, incoming + 40);
    const uint32_t source4 = LoadGuestU32(base, incoming + 44);
    const uint32_t raw5 = LoadGuestU32(base, incoming + 48);
    const auto physical = [](uint32_t address) {
      return (address & 0x1FFFFFFFu) + (((address >> 20) + 512) & 0x1000u);
    };
    const uint32_t first = (source0 & ~0x3FFC00u) | (LoadGuestU32(base, output) & 0x3FFC00);
    const uint32_t second = (physical(raw1) & ~0x800u) | (LoadGuestU32(base, output + 4) & 0x800);
    const uint32_t fourth =
        (source3 & ~0x7FF80000u) | (LoadGuestU32(base, output + 12) & 0x7FF80000);
    const uint32_t fifth = (source4 & 0x3FC) | (LoadGuestU32(base, output + 16) & ~0x3FCu);
    const uint32_t sixth = (physical(raw5) & ~0x1FFu) | (LoadGuestU32(base, output + 20) & 0x1FF);
    const uint32_t low =
        std::max((source4 >> 2) & 15u, uint32_t(*GuestToHost(base, device + slot + 11942)));
    const uint32_t high =
        std::min((source4 >> 6) & 15u, uint32_t(*GuestToHost(base, device + slot + 11968)));
    const uint32_t low_clamped = (fifth & ~0x3Cu) | ((low << 2) & 0x3C);
    StoreGuestU32(base, output + 8, source2);
    StoreGuestU32(base, output, first);
    StoreGuestU32(base, output + 4, second);
    StoreGuestU32(base, output + 16, fifth);
    StoreGuestU32(base, output + 12, fourth);
    StoreGuestU32(base, output + 20, sixth);
    StoreGuestU32(base, output + 16, low_clamped);
    StoreGuestU32(base, output + 16, (low_clamped & ~0x3C0u) | ((high << 6) & 0x3C0));
    Store64(base, device + 24, Load64(base, device + 24) | ctx.r6.u64);
    result = second;
  }
  StoreGuestU32(base, device + 12536 + slot * 4, incoming);
  Retire(retirement, base, device);
  ctx.r3.u64 = retirement.enqueue ? retirement.cursor : result;
  ctx.lr = uint32_t(ctx.lr);
  return true;
}

inline bool BindVertexDeclaration(PPCContext& ctx, uint8_t* base) {
  using namespace resource_binding_detail;
  MemoryRange device;
  if (!base || (ctx.r3.u32 & 3) || !BackingRange(ctx.r3.u32, 22256, device))
    return false;
  StoreGuestU32(base, ctx.r3.u32 + 11812, ctx.r4.u32);
  Store64(base, ctx.r3.u32 + 16, Load64(base, ctx.r3.u32 + 16) | 0x80000);
  return true;
}

}  // namespace simpsons::native_gpu_calls
