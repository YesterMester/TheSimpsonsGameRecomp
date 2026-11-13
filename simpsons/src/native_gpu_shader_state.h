#pragma once

#include <array>
#include <initializer_list>
#include <rex/graphics/native_gpu_predication.h>
#include <rex/graphics/native_gpu_shader_reference.h>
#include <rex/ppc/context.h>

#include "guest_memory.h"
#include "native_gpu_memory.h"

namespace simpsons::native_gpu_calls {
namespace shader_state_detail {
using namespace native_gpu_memory;

struct ShaderView {
  rex::graphics::native_gpu_shader_reference::Call call;
  uint32_t source, variant, version_address;
  MemoryRange base_word, selector_word, code_words, version, code;
};

inline bool PrepareView(const uint8_t* base, uint32_t resource, uint64_t variant,
                        const MemoryRange& output, const MemoryRange& device,
                        const MemoryRange& stack, ShaderView& view) {
  const uint64_t selector = (variant + 112) * 8, version = variant * 416 + 64;
  if (variant > UINT32_MAX || uint64_t(resource) + selector + 4 > 0x100000000ull ||
      uint64_t(resource) + version + 4 > 0x100000000ull ||
      !BackingRange(resource + 32, 4, view.base_word) ||
      !BackingRange(resource + uint32_t(selector), 4, view.selector_word) ||
      !BackingRange(resource + uint32_t(version), 4, view.version))
    return false;
  const uint32_t relative = LoadGuestU32(base, resource + uint32_t(selector));
  if (uint64_t(resource) + relative + 880 > 0x100000000ull ||
      !BackingRange(resource + relative + 872, 8, view.code_words))
    return false;
  const uint64_t source =
      uint64_t(LoadGuestU32(base, resource + 32)) + LoadGuestU32(base, resource + relative + 872);
  const uint32_t bytes = LoadGuestU32(base, resource + relative + 876);
  if (source > UINT32_MAX || (bytes & 3))
    return false;
  view.source = uint32_t(source);
  view.variant = uint32_t(variant);
  view.version_address = resource + uint32_t(version);
  view.call = {(view.source & 0x1FFFFFFFu) + (((view.source >> 20) + 512) & 0x1000u), bytes / 4};
  if (!rex::graphics::native_gpu_shader_reference::Validate(view.call) ||
      !BackingRange(0xC0000000u + (view.call.address_type & ~3u), bytes, view.code) ||
      Overlaps(view.version, device) || Overlaps(view.version, stack))
    return false;
  for (const auto& input :
       {view.base_word, view.selector_word, view.code_words, view.version, view.code})
    if (Overlaps(output, input))
      return false;
  return true;
}

inline bool Output(const uint8_t* base, const PPCContext& ctx, uint32_t words, MemoryRange& output,
                   MemoryRange& device, MemoryRange& stack) {
  const uint32_t address = ctx.r30.u32, cursor = ctx.r11.u32;
  return base && !(address & 3) && !(cursor & 3) && !(ctx.r1.u32 & 15) &&
         BackingRange(address, 22256, device) && BackingRange(ctx.r1.u32, 256, stack) &&
         !Overlaps(device, stack) && cursor <= LoadGuestU32(base, address + 56) &&
         uint64_t(cursor) + words * 4 <= LoadGuestU32(base, address + 52) &&
         uint64_t(cursor) + 4 + words * 4 <= 0x100000000ull &&
         BackingRange(cursor + 4, words * 4, output) && !Overlaps(output, device) &&
         !Overlaps(output, stack);
}
}  // namespace shader_state_detail

// Pixel program after the declaration's constant fixups and real ring growth.
// The caller retains its dirty bits, state publication and remaining fixups.
inline bool WritePixelShaderReference(PPCContext& ctx, uint8_t* base) {
  using namespace shader_state_detail;
  const uint32_t resource = ctx.r29.u32;
  MemoryRange output, device, stack, header, selected, code;
  if (ctx.r17.u32 != 0xC0012700 || (resource & 3) || !Output(base, ctx, 3, output, device, stack) ||
      !BackingRange(resource, 68, header) || Overlaps(output, header))
    return false;
  const uint32_t relative = LoadGuestU32(base, resource + 64);
  if (uint64_t(resource) + relative + 48 > 0x100000000ull ||
      !BackingRange(resource + relative, 48, selected) || Overlaps(output, selected))
    return false;
  const uint64_t source =
      uint64_t(LoadGuestU32(base, resource + 24)) + LoadGuestU32(base, resource + relative + 40);
  const uint32_t bytes = LoadGuestU32(base, resource + relative + 44);
  if (source > UINT32_MAX || (bytes & 3))
    return false;
  const uint32_t raw = uint32_t(source);
  const rex::graphics::native_gpu_shader_reference::Call call{
      ((raw & 0x1FFFFFFEu) + (((raw >> 20) + 512) & 0x1000u)) | 1u, bytes / 4};
  if (!rex::graphics::native_gpu_shader_reference::Validate(call) ||
      !BackingRange(0xC0000000u + (call.address_type & ~3u), bytes, code) || Overlaps(output, code))
    return false;
  const uint32_t cursor = ctx.r11.u32;
  StoreGuestU32(base, cursor + 4, rex::graphics::native_gpu_shader_reference::kHeader);
  StoreGuestU32(base, cursor + 8, call.address_type);
  StoreGuestU32(base, cursor + 12, call.words);
  ctx.r20.u64 |= 16384;
  ctx.r8.u64 = raw >> 20;
  ctx.r9.u64 = raw & 0x1FFFFFFEu;
  ctx.r10.u64 = call.words;
  ctx.r11.u32 = cursor + 12;
  return true;
}

namespace shader_state_detail {
struct Values {
  uint32_t program, depth, loop_low, loop_high;
};
struct Continuation {
  PPCRegister r3, r4, r5, r6, r7, r8, r9, r10, r11, r12, r20;
  PPCCRRegister cr6;
};
struct ConditionalPlan {
  Continuation next;
  std::array<ShaderView, 2> views{};
  std::array<uint32_t, 33> words{};
  std::array<uint32_t, 2> version_after{};
  uint32_t initial = 0, count = 0, view_count = 0, fence = 0;
};

template <bool KeepContinuation = true>
inline bool BuildConditionalPlan(const PPCContext& ctx, const uint8_t* base, ConditionalPlan& plan,
                                 const Values* values = nullptr) {
  namespace pred = rex::graphics::native_gpu_predication;
  const bool first = ctx.r15.s32 != -1, second = ctx.r19.s32 != -1;
  const bool program = ctx.r20.u64 & 8, depth = ctx.r20.u64 & (1ull << 56);
  const bool loops = ctx.r20.u64 & 0x10000;
  const uint32_t count = 8 + (program ? 2 : 0) + (ctx.r14.s32 != -1 ? 3 : 0) + (depth ? 2 : 0) +
                         (ctx.r23.s32 != -1 ? 3 : 0) + (loops ? 3 : 0) +
                         (ctx.r29.s32 != -1 ? 4 : 0) + (first ? 3 : 0) +
                         (second ? (first ? 5 : 3) : 0);
  MemoryRange output, device, stack;
  if (ctx.r17.u32 != 0xC0012700 || (ctx.r31.u32 & 3) ||
      !Output(base, ctx, count, output, device, stack))
    return false;
  auto& views = plan.views;
  auto& view_count = plan.view_count;
  view_count = 0;
  if (first &&
      !PrepareView(base, ctx.r31.u32, ctx.r15.u64, output, device, stack, views[view_count++]))
    return false;
  if (second &&
      !PrepareView(base, ctx.r31.u32, ctx.r19.u64, output, device, stack, views[view_count++]))
    return false;
  for (uint32_t i = 0; i < view_count; ++i)
    for (uint32_t j = 0; j < view_count; ++j)
      for (const auto& input :
           {views[j].base_word, views[j].selector_word, views[j].code_words, views[j].code})
        if (Overlaps(views[i].version, input))
          return false;

  auto& words = plan.words;
  auto& version_after = plan.version_after;
  uint32_t n = 0, version_count = 0;
  const auto put = [&](uint32_t value) { words[n++] = value; };
  const auto state = [&](uint32_t reg, std::initializer_list<uint32_t> values) {
    uint32_t header;
    if (!rex::graphics::native_gpu_state::EncodeHeader(reg, uint32_t(values.size()), header))
      return false;
    put(header);
    for (auto value : values)
      put(value);
    return true;
  };
  auto& next = plan.next;
  if constexpr (KeepContinuation) {
    next = {ctx.r3, ctx.r4,  ctx.r5,  ctx.r6,  ctx.r7,  ctx.r8,
            ctx.r9, ctx.r10, ctx.r11, ctx.r12, ctx.r20, ctx.cr6};
  } else {
    next = {};
    next.r11 = ctx.r11;
    next.r20 = ctx.r20;
  }
  next.r4.s64 = int32_t(0xC0006100);
  next.r5.s64 = int32_t(0xC0006000);
  next.r7.u64 = program ? 8 : 0;
  next.r8.u64 = next.r4.u64;
  next.r9.u64 = 0x15555555;
  next.r10.s64 = 0;
  put(pred::kMaskLowHeader);
  put(0x15555555);
  put(pred::kMaskHighHeader);
  put(0);
  if (program) {
    next.r10.u64 = (values ? values->program : LoadGuestU32(base, ctx.r30.u32 + 10580));
    next.r12.s64 = -9;
    next.r20.u64 &= ~8ull;
    if (!state(0x2208, {next.r10.u32}))
      return false;
  }
  next.r10.s64 = int32_t(0xC0012D01);
  if (ctx.r14.s32 != -1) {
    next.r9.u64 = next.r10.u64;
    next.r8.u64 = 0x40208;
    put(pred::kStateHeader);
    put(0x40208);
    put(ctx.r14.u32);
  }
  next.r12.u64 = 1ull << 56;
  next.r9.u64 = depth ? (1ull << 56) : 0;
  if (depth) {
    next.r12.u64 = ~(1ull << 56);
    next.r20.u64 &= next.r12.u64;
    next.r9.u64 = (values ? values->depth : LoadGuestU32(base, ctx.r30.u32 + 10372));
    if (!state(0x2001, {next.r9.u32}))
      return false;
  }
  if (ctx.r23.s32 != -1) {
    next.r9.u64 = 0x40001;
    put(pred::kStateHeader);
    put(0x40001);
    put(ctx.r23.u32);
  }
  next.r10.u64 = loops ? 0x10000 : 0;
  if (loops) {
    next.r10.u64 = 0x12180;
    next.r12.s64 = int32_t(0xFFFE7FFF);
    next.r20.u64 &= next.r12.u64;
    next.r10.u64 = (values ? values->loop_low : LoadGuestU32(base, ctx.r30.u32 + 10528));
    const uint32_t low = next.r10.u32;
    next.r10.u64 = (values ? values->loop_high : LoadGuestU32(base, ctx.r30.u32 + 10532));
    if (!state(0x2180, {low, next.r10.u32}))
      return false;
  }
  if (ctx.r29.s32 != -1) {
    next.r10.s64 = int32_t(0xC0022D01);
    next.r9.u64 = 0x40180;
    put(0xC0026F05);
    put(0x40180);
    put(ctx.r29.u32);
    put(ctx.r28.u32);
  }
  if constexpr (KeepContinuation)
    next.cr6.compare<int32_t>(ctx.r19.s32, -1, ctx.xer);
  plan.fence = view_count ? LoadGuestU32(base, ctx.r30.u32 + 10908) : 0;
  const uint32_t fence = plan.fence;
  if (first) {
    const auto& v = views[0];
    next.r8.u64 = (uint64_t(v.variant) + 112) * 8;
    next.r3.u64 = ctx.r31.u64 + uint64_t(v.variant) * 416;
    next.r10.s64 = int32_t(0xC0012701);
    next.r7.u64 = v.source & 0x1FFFFFFFu;
    next.r6.u64 = v.source >> 20;
    next.r9.u64 = fence;
    put(rex::graphics::native_gpu_shader_reference::kHeader | 1);
    put(v.call.address_type);
    put(v.call.words);
    version_after[version_count++] = n;
    if (second) {
      put(pred::kMaskLowHeader);
      put(0x2AAAAAAA);
      next.r6.u64 = 0x2AAAAAAA;
      next.r8.u64 = next.r10.u64;
    }
  }
  if (second) {
    const auto& v = views[first ? 1 : 0];
    next.r9.u64 = (uint64_t(v.variant) + 112) * 8;
    next.r7.u64 = v.source >> 20;
    next.r8.u64 = v.source & 0x1FFFFFFFu;
    if (first) {
      next.r3.u64 = ctx.r31.u64 + uint64_t(v.variant) * 416;
    } else {
      next.r6.u64 = ctx.r31.u64 + uint64_t(v.variant) * 416;
    }
    put(rex::graphics::native_gpu_shader_reference::kHeader | (first ? 1 : 0));
    put(v.call.address_type);
    put(v.call.words);
    version_after[version_count++] = n;
  }
  put(pred::kMaskLowHeader);
  put(LoadGuestU32(base, ctx.r30.u32 + 12700));
  put(pred::kMaskHighHeader);
  put(LoadGuestU32(base, ctx.r30.u32 + 12704));
  next.r10.u64 = words[n - 1];
  if (n != count || n > words.size())
    return false;
  next.r11.u32 += n * 4;
  plan.initial = ctx.r11.u32;
  plan.count = n;
  return true;
}

inline void ApplyMemory(const ConditionalPlan& plan, uint8_t* base) {
  for (uint32_t i = 0; i < plan.count; ++i) {
    StoreGuestU32(base, plan.initial + 4 + i * 4, plan.words[i]);
    for (uint32_t v = 0; v < plan.view_count; ++v)
      if (plan.version_after[v] == i + 1)
        StoreGuestU32(base, plan.views[v].version_address, plan.fence);
  }
}

inline void Apply(const ConditionalPlan& plan, PPCContext& ctx, uint8_t* base) {
  ApplyMemory(plan, base);
  const auto& next = plan.next;
  ctx.r3 = next.r3;
  ctx.r4 = next.r4;
  ctx.r5 = next.r5;
  ctx.r6 = next.r6;
  ctx.r7 = next.r7;
  ctx.r8 = next.r8;
  ctx.r9 = next.r9;
  ctx.r10 = next.r10;
  ctx.r11 = next.r11;
  ctx.r12 = next.r12;
  ctx.r20 = next.r20;
  ctx.cr6 = next.cr6;
}
}  // namespace shader_state_detail

// Conditional shader/render state after real ring growth. Keep every operand
// position, mask restoration, version write and original CPU continuation.
inline bool WriteConditionalShaderState(PPCContext& ctx, uint8_t* base) {
  shader_state_detail::ConditionalPlan plan;
  if (!shader_state_detail::BuildConditionalPlan(ctx, base, plan))
    return false;
  shader_state_detail::Apply(plan, ctx, base);
  return true;
}

}  // namespace simpsons::native_gpu_calls
