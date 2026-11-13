#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <rex/ppc/context.h>

#include "guest_memory.h"
#include "native_gpu_memory.h"

namespace simpsons::native_gpu_calls {
namespace shader_linkage_detail {
using namespace native_gpu_memory;

struct PatchList {
  uint32_t address;
  uint32_t count;
  uint32_t nibble;
  bool neutral;
};

// Plan the complete linkage before writing code. Metadata must stay unchanged
// throughout all lists, including duplicate instruction indices and lists shared
// by multiple descriptors. No side queue or cached guest pointers survive a call.
struct Plan {
  std::array<PatchList, 31> lists{};
  MemoryRange resource{}, stack{}, descriptor{}, secondary{}, result{};
  uint32_t resource_address = 0, vertex_exports = 0, list_count = 0;
  uint32_t result_address = 0, result_value = 0;
  uint64_t code_address = 0, resource_end = 0;
  bool write_result = false;
};

inline bool Begin(uint32_t stack_pointer, const uint8_t* base, uint32_t resource, uint64_t code,
                  uint32_t frame, Plan& plan) {
  if (!base || (resource & 3) || (uint32_t(code) & 3) || (stack_pointer & 15) ||
      stack_pointer < frame || !BackingRange(resource, 32, plan.resource) ||
      !BackingRange(stack_pointer - frame, frame, plan.stack) ||
      Overlaps(plan.resource, plan.stack))
    return false;
  plan.resource_address = resource;
  plan.resource_end = uint64_t(resource) + 32;
  plan.code_address = code;
  plan.vertex_exports = (LoadGuestU32(base, resource + 20) >> 5) & 31;
  return true;
}

inline bool ExtendResource(Plan& plan, uint64_t end) {
  end = std::max(end, plan.resource_end);
  if (end > 0x100000000ull ||
      !BackingRange(plan.resource_address, end - plan.resource_address, plan.resource) ||
      Overlaps(plan.resource, plan.stack))
    return false;
  plan.resource_end = end;
  return true;
}

inline bool AddList(Plan& plan, const uint8_t* base, uint32_t descriptor, bool neutral,
                    uint32_t nibble = 0) {
  if (plan.list_count == plan.lists.size())
    return false;
  const uint32_t resource = plan.resource_address;
  const uint64_t index = uint64_t(LoadGuestU16(base, descriptor) & 0xFFF) + plan.vertex_exports +
                         LoadGuestU32(base, resource + 24) + LoadGuestU32(base, resource + 28) + 9;
  const uint64_t address = uint64_t(resource) + index * 4;
  if (address > UINT32_MAX)
    return false;
  // A malformed list must not turn a native call into an unbounded scan. Longer
  // chains keep the original implementation; 4096 is the instruction-index space.
  for (uint32_t count = 1; count <= 4096; ++count) {
    const uint64_t end = address + count * 4;
    if (!ExtendResource(plan, end))
      return false;
    if (LoadGuestU32(base, uint32_t(end - 4)) & 0x1000) {
      plan.lists[plan.list_count++] = {uint32_t(address), count, nibble, neutral};
      return true;
    }
  }
  return false;
}

inline bool Validate(const Plan& plan, const uint8_t* base) {
  if (Overlaps(plan.descriptor, plan.stack) || Overlaps(plan.secondary, plan.stack) ||
      Overlaps(plan.result, plan.stack) || Overlaps(plan.result, plan.resource) ||
      Overlaps(plan.result, plan.descriptor) || Overlaps(plan.result, plan.secondary))
    return false;
  for (uint32_t i = 0; i < plan.list_count; ++i) {
    const auto& list = plan.lists[i];
    for (uint32_t j = 0; j < list.count; ++j) {
      const uint64_t address = uint64_t(uint32_t(plan.code_address)) +
                               (LoadGuestU32(base, list.address + j * 4) & 0xFFF) * 12;
      MemoryRange instruction;
      if (address > UINT32_MAX || !BackingRange(uint32_t(address), 12, instruction) ||
          Overlaps(instruction, plan.resource) || Overlaps(instruction, plan.descriptor) ||
          Overlaps(instruction, plan.secondary) || Overlaps(instruction, plan.result) ||
          Overlaps(instruction, plan.stack))
        return false;
    }
  }
  return true;
}

inline void Apply(const Plan& plan, PPCContext& ctx, uint8_t* base) {
  for (uint32_t i = 0; i < plan.list_count; ++i) {
    const auto& list = plan.lists[i];
    for (uint32_t j = 0; j < list.count; ++j) {
      const uint32_t offset = (LoadGuestU32(base, list.address + j * 4) & 0xFFF) * 12;
      const uint32_t address = uint32_t(plan.code_address) + offset;
      const uint32_t first =
          list.neutral ? 0xC8000000u : (LoadGuestU32(base, address) & ~15u) | list.nibble;
      const uint32_t second = list.neutral ? 0 : LoadGuestU32(base, address + 4);
      const uint32_t third = list.neutral ? 0x02000000u : LoadGuestU32(base, address + 8);
      StoreGuestU32(base, address, first);
      StoreGuestU32(base, address + 4, second);
      StoreGuestU32(base, address + 8, third);
      // The original twelve-byte copy returns its full destination register.
      ctx.r3.u64 = plan.code_address + offset;
    }
  }
  if (plan.write_result)
    StoreGuestU32(base, plan.result_address, plan.result_value);
  ctx.lr = uint32_t(ctx.lr);
}
}  // namespace shader_linkage_detail

// 8245F3D8 and 8245F4A8: neutralize an unused export or relink its component.
inline bool PatchShaderLinkage(PPCContext& ctx, uint8_t* base, bool neutral) {
  using namespace shader_linkage_detail;
  Plan plan;
  if (!Begin(ctx.r1.u32, base, ctx.r3.u32, ctx.r4.u64, neutral ? 136 : 152, plan) ||
      (ctx.r5.u32 & 3) || !BackingRange(ctx.r5.u32, 4, plan.descriptor) ||
      Overlaps(plan.descriptor, plan.stack))
    return false;
  uint32_t nibble = 0;
  if (!neutral) {
    if ((ctx.r6.u32 & 3) || !BackingRange(ctx.r6.u32, 4, plan.secondary) ||
        Overlaps(plan.secondary, plan.stack))
      return false;
    nibble = (LoadGuestU32(base, ctx.r6.u32) >> 8) & 15;
  }
  if (!AddList(plan, base, ctx.r5.u32, neutral, nibble) || !Validate(plan, base))
    return false;
  Apply(plan, ctx, base);
  return true;
}

namespace shader_linkage_detail {
struct ExportArguments {
  uint32_t resource, result, secondary, stack;
  uint64_t code;
  bool override_result = false;
  uint32_t result_value = 0;
};

// The inline shader caller must prove the merge alongside its allocation, copy
// and declaration patch before any of them writes. Planning has no side effects.
inline bool BuildExportPlan(const ExportArguments& args, const uint8_t* base, Plan& plan) {
  if (!Begin(args.stack, base, args.resource, args.code, 328, plan))
    return false;
  const uint32_t resource = args.resource, flags = LoadGuestU32(base, resource + 20);
  if ((*GuestToHost(base, resource + 8) & 7) == 7 || (flags & 0x40000))
    return true;
  uint32_t secondary_count = 0, export_count = 0;
  const uint32_t secondary = args.secondary;
  if (secondary) {
    if ((secondary & 3) || !BackingRange(secondary, 32, plan.secondary) ||
        Overlaps(plan.secondary, plan.stack))
      return false;
    const uint32_t secondary_flags = LoadGuestU32(base, secondary + 20);
    if (secondary_flags & 0x20000)
      return true;
    secondary_count = (secondary_flags >> 5) & 31;
    export_count = (secondary_flags & 31) ? (secondary_flags & 31) - 1 : 0;
    if (!BackingRange(secondary, 32 + secondary_count * 4, plan.secondary))
      return false;
  }
  const uint64_t table =
      uint64_t(resource) +
      (uint64_t(LoadGuestU32(base, resource + 24)) + LoadGuestU32(base, resource + 28) + 9) * 4;
  if (!ExtendResource(plan, table + plan.vertex_exports * 4) || (args.result & 3) ||
      !BackingRange(args.result, 4, plan.result))
    return false;
  uint32_t current = 0;
  for (uint32_t i = 0; i < secondary_count; ++i) {
    const uint32_t selected = secondary + 32 + i * 4;
    const uint32_t semantic = *GuestToHost(base, selected + 3);
    while (current < plan.vertex_exports &&
           *GuestToHost(base, uint32_t(table) + current * 4 + 3) < semantic) {
      if (!AddList(plan, base, uint32_t(table) + current++ * 4, true))
        return false;
    }
    // The guest may read a sentinel past its declared table in this case.
    // Preserve that branch through the original path until it has an owner.
    if (current == plan.vertex_exports)
      return false;
    const uint32_t descriptor = uint32_t(table) + current++ * 4;
    if ((LoadGuestU32(base, descriptor) ^ LoadGuestU32(base, selected)) & 0xF00)
      if (!AddList(plan, base, descriptor, false, (LoadGuestU32(base, selected) >> 8) & 15))
        return false;
  }
  while (current < plan.vertex_exports)
    if (!AddList(plan, base, uint32_t(table) + current++ * 4, true))
      return false;
  plan.write_result = true;
  plan.result_address = args.result;
  if (!Validate(plan, base))
    return false;
  const uint32_t original =
      args.override_result ? args.result_value : LoadGuestU32(base, args.result);
  plan.result_value = (original & 0xFF0FFFFFu) | ((export_count & 15) << 20);
  return true;
}
}  // namespace shader_linkage_detail

// 8245F560: retain the original descriptor merge order and export-count bits.
// The more general vertex-declaration patcher (8245EDC8) is a separate contract.
inline bool LinkShaderExports(PPCContext& ctx, uint8_t* base) {
  shader_linkage_detail::Plan plan;
  if (!shader_linkage_detail::BuildExportPlan(
          {ctx.r4.u32, ctx.r6.u32, ctx.r7.u32, ctx.r1.u32, ctx.r5.u64}, base, plan))
    return false;
  shader_linkage_detail::Apply(plan, ctx, base);
  return true;
}

}  // namespace simpsons::native_gpu_calls
