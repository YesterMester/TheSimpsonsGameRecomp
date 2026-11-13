#pragma once

#include <algorithm>
#include <cstring>
#include <initializer_list>
#include <rex/ppc/barriers.h>

#include "native_gpu_shader_linkage.h"
#include "native_gpu_shader_resources.h"
#include "native_gpu_shader_setup.h"

namespace simpsons::native_gpu_calls {

namespace inline_shader_detail {
struct Plan {
  Plan() noexcept {}
  ShaderPrefixSpan prefix;
  vertex_declaration_detail::Plan declaration;
  shader_linkage_detail::Plan exports;
  native_gpu_memory::MemoryRange device_range{}, stack{}, object{}, source{};
  uint32_t device = 0, cursor = 0, code = 0, source_address = 0, bytes = 0;
  uint64_t resource_argument = 0;
  bool linkage = false;
};

inline bool BuildPlan(const PPCContext& ctx, const uint8_t* base, Plan& result,
                      const uint32_t* initial = nullptr, const uint32_t* result_value = nullptr) {
  using namespace native_gpu_memory;
  const uint32_t device = ctx.r3.u32, resource = ctx.r5.u32;
  const uint64_t resource_argument = ctx.r5.u64, variant = ctx.r10.u64;
  const bool linkage = ctx.r4.s32 != 0;
  auto& dev = result.device_range;
  auto& stack = result.stack;
  auto& object = result.object;
  auto& source = result.source;
  const uint64_t selector = (variant + 112) * 8;
  if (!base || ((device | resource) & 3) || (ctx.r1.u32 & 15) || ctx.r1.u32 < 824 ||
      variant > UINT32_MAX || uint64_t(resource) + selector + 4 > 0x100000000ull ||
      !BackingRange(device, 22256, dev) || !BackingRange(ctx.r1.u32 - 824, 824, stack) ||
      Overlaps(dev, stack) ||
      !BackingRange(resource, std::max(uint64_t(36), selector + 4), object) ||
      Overlaps(object, dev) || Overlaps(object, stack))
    return false;
  const uint64_t metadata =
      uint64_t(resource) + LoadGuestU32(base, resource + uint32_t(selector)) + 872;
  if (metadata + 8 > 0x100000000ull || !BackingRange(resource, metadata + 8 - resource, object) ||
      Overlaps(object, dev) || Overlaps(object, stack))
    return false;
  const uint32_t bytes = LoadGuestU32(base, uint32_t(metadata) + 4);
  const uint64_t raw_source =
      uint64_t(LoadGuestU32(base, resource + 32)) + LoadGuestU32(base, uint32_t(metadata));
  if ((bytes & 3) || raw_source > UINT32_MAX)
    return false;
  const uint32_t physical =
      (uint32_t(raw_source) & 0x1FFFFFFFu) + (((uint32_t(raw_source) >> 20) + 512) & 0x1000u);
  if (uint64_t(physical) + bytes > 0x20000000ull)
    return false;
  const uint32_t source_address = 0xC0000000u + physical;
  const uint32_t cursor = initial ? *initial : LoadGuestU32(base, device + 48);
  auto& prefix = result.prefix;
  if (!PlanShaderPrefix(base, device, cursor, source_address, bytes / 4, 5, ctx.r1.u32 - 224, 224,
                        prefix) ||
      !BackingRange(source_address, bytes, source) || Overlaps(source, stack) ||
      Overlaps(source, dev) || Overlaps(prefix.commands, stack))
    return false;
  const uint32_t code = cursor + 24;
  auto& declaration = result.declaration;
  if (!vertex_declaration_detail::BuildPlan<false>(
          {resource, ctx.r7.u32, device + 12520, ctx.r1.u32 - 224, code, variant}, base,
          declaration))
    return false;
  for (const auto& input : {declaration.metadata, declaration.attributes, declaration.tables})
    if (Overlaps(input, dev) || Overlaps(input, stack) || Overlaps(input, prefix.commands))
      return false;
  for (uint32_t i = 0; i < declaration.count; ++i)
    if (uint64_t(declaration.records[i] & 0xFFF) * 12 + 12 > bytes)
      return false;

  auto& exports = result.exports;
  if (linkage) {
    if (!shader_linkage_detail::BuildExportPlan(
            {ctx.r8.u32, ctx.r6.u32, ctx.r9.u32, ctx.r1.u32 - 224, code, result_value != nullptr,
             result_value ? *result_value : 0},
            base, exports))
      return false;
    for (const auto& input :
         {exports.resource, exports.descriptor, exports.secondary, exports.result})
      if (Overlaps(input, dev) || Overlaps(input, stack) || Overlaps(input, prefix.commands))
        return false;
    for (const auto& input : {declaration.metadata, declaration.attributes, declaration.stream_map,
                              declaration.tables, source})
      if (Overlaps(exports.result, input))
        return false;
    for (uint32_t i = 0; i < exports.list_count; ++i)
      for (uint32_t j = 0; j < exports.lists[i].count; ++j)
        if (uint64_t(LoadGuestU32(base, exports.lists[i].address + j * 4) & 0xFFF) * 12 + 12 >
            bytes)
          return false;
  }

  result.device = device;
  result.cursor = cursor;
  result.code = code;
  result.source_address = source_address;
  result.bytes = bytes;
  result.resource_argument = resource_argument;
  result.linkage = linkage;
  return true;
}

inline void Apply(const Plan& plan, PPCContext& ctx, uint8_t* base) {
  StoreProgramPrefix(base, plan.cursor, plan.prefix.header, plan.bytes / 4, 0, 0x100);
  REX_PPC_SYNC();
  std::memcpy(GuestToHost(base, plan.code), GuestToHost(base, plan.source_address), plan.bytes);
  REX_PPC_SYNC();
  if (plan.linkage)
    shader_linkage_detail::Apply(plan.exports, ctx, base);
  auto* dirty = GuestToHost(base, plan.device + 10942);
  *dirty = (*dirty & 0x7F) | (plan.linkage ? 0x80 : 0);
  vertex_declaration_detail::Apply(plan.declaration, base);
  shader_resource_detail::Publish(base, plan.device);
  StoreGuestU32(base, plan.device + 48, plan.cursor + 20 + plan.bytes);
}
}  // namespace inline_shader_detail

// 8245F6B0: prove the complete copy/link/declaration sequence before publishing.
// The outer coordinator can reuse the same plan at its actual future cursor.
inline bool WriteInlineResourceShader(PPCContext& ctx, uint8_t* base) {
  inline_shader_detail::Plan plan;
  if (!inline_shader_detail::BuildPlan(ctx, base, plan))
    return false;
  inline_shader_detail::Apply(plan, ctx, base);
  ctx.r3.u64 = plan.declaration.count
                   ? uint64_t(plan.code) +
                         (plan.declaration.records[plan.declaration.count - 1] & 0xFFF) * 12
                   : plan.resource_argument;
  ctx.lr = uint32_t(ctx.lr);
  return true;
}

}  // namespace simpsons::native_gpu_calls
