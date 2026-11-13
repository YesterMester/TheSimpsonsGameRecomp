#pragma once

#include <optional>

#include "native_gpu_inline_shader.h"
#include "native_gpu_shader_constants.h"
#include "native_gpu_shader_control.h"
#include "native_gpu_shader_state.h"

namespace simpsons::native_gpu_calls {
namespace shader_coordinator_detail {
using namespace native_gpu_memory;

// No guest writes, version allocations or command publication during planning.
// A rejected late child must leave the original coordinator free to run once.
struct ResourceWrites {
  MemoryRange versions, declaration, counter;
};
struct Plan {
  shader_control_detail::Plan control;
  std::array<shader_constants_detail::Plan, 2> constants;
  std::array<std::optional<shader_resource_detail::Plan>, 2> resources;
  std::array<ResourceWrites, 2> resource_writes;
  std::array<uint32_t, 2> mutators;
  std::optional<inline_shader_detail::Plan> inline_shader;
  std::optional<shader_state_detail::ConditionalPlan> conditional;
  std::optional<shader_state_detail::ConditionalPlan> conditional_cached;
  shader_state_detail::ShaderView primary;
  std::array<MemoryRange, 64> inputs;
  std::array<MemoryRange, 3> vertex_holes{};
  MemoryRange device_range{}, stack{}, commands{}, pixel_code{};
  uint64_t flags = 0;
  uint32_t device = 0, vertex = 0, declaration = 0, initial = 0, cursor = 0;
  uint32_t program = 0, depth = 0, pixel_input = 0, mode = 0, loop_low = 0, loop_high = 0;
  uint32_t result_address = 0, vertex_mask = 0, input_count = 0, resource_count = 0;
  uint32_t mutator_count = 0;
  uint32_t pending_busy_slot = UINT32_MAX;
  uint32_t pixel_reference_cursor = 0, pixel_address_type = 0, pixel_words = 0;
  uint8_t shader_flags = 0;
  bool control_used = false, pixel_used = false, program_used = false, depth_used = false;
  bool mode_used = false, loops_used = false, inline_used = false, conditional_used = false;
  bool primary_used = false, vertex_used = false;
  bool holes_ready = false;
};

inline bool AddInput(Plan& plan, MemoryRange range) {
  if (range.first == range.end)
    return true;
  if (plan.input_count == plan.inputs.size() || Overlaps(range, plan.device_range) ||
      Overlaps(range, plan.stack))
    return false;
  plan.inputs[plan.input_count++] = range;
  return true;
}

inline bool Input(Plan& plan, uint64_t address, uint64_t bytes) {
  MemoryRange range;
  return address <= UINT32_MAX && BackingRange(uint32_t(address), bytes, range) &&
         AddInput(plan, range);
}

// A declaration version and the two VS version/snapshot records are intentional
// outputs. Carve only those fields from an aggregate VS/declaration input; PS,
// literal and other owners always retain their entire input ranges.
inline bool AddVertexInput(Plan& plan, MemoryRange range) {
  if (!plan.holes_ready) {
    auto& holes = plan.vertex_holes;
    BackingRange(plan.vertex + 40, 28, holes[0]);
    BackingRange(plan.vertex + 456, 28, holes[1]);
    if (plan.declaration && uint64_t(plan.declaration) + 52 <= 0x100000000ull)
      BackingRange(plan.declaration + 48, 4, holes[2]);
    std::sort(holes.begin(), holes.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    plan.holes_ready = true;
  }
  for (const auto& hole : plan.vertex_holes) {
    if (!Overlaps(range, hole))
      continue;
    if (range.first < hole.first && !AddInput(plan, {range.first, hole.first}))
      return false;
    range.first = std::min(hole.end, range.end);
  }
  return AddInput(plan, range);
}

inline bool Constants(Plan& plan, PPCContext& work, const uint8_t* base, uint32_t slot,
                      uint32_t metadata, uint32_t source) {
  work.r3.u64 = plan.device;
  work.r4.u64 = metadata;
  work.r5.u64 = source;
  auto& constants = plan.constants[slot];
  if (!shader_constants_detail::BuildPlan(work, base, constants, &plan.cursor) ||
      !AddInput(plan, constants.metadata))
    return false;
  if (constants.count)
    plan.cursor = constants.cursor;
  return true;
}

inline bool Resource(Plan& plan, PPCContext& work, const uint8_t* base, uint32_t variant,
                     bool& ready) {
  if (plan.resource_count == plan.resources.size())
    return false;
  const uint32_t slot = plan.resource_count++;
  auto& resource = plan.resources[slot].emplace();
  work.r3.u64 = plan.device;
  work.r4.u64 = plan.vertex;
  work.r5.u64 = plan.declaration;
  work.r6.u64 = variant;
  const auto* preceding =
      slot && plan.resources[slot - 1]->kind == shader_resource_detail::Kind::kRebuild &&
              !plan.resources[slot - 1]->version
          ? &*plan.resources[slot - 1]
          : nullptr;
  if (!shader_resource_detail::BuildPlan(work, base, resource, preceding) ||
      !AddVertexInput(plan, resource.object) || !AddVertexInput(plan, resource.attributes) ||
      !AddInput(plan, resource.fence_word))
    return false;
  if (resource.kind == shader_resource_detail::Kind::kRebuild &&
      (!AddVertexInput(plan, resource.patch.metadata) || !AddInput(plan, resource.patch.tables)))
    return false;
  if (resource.kind == shader_resource_detail::Kind::kRebuild) {
    plan.mutators[plan.mutator_count++] = slot;
    auto& writes = plan.resource_writes[slot];
    if (!BackingRange(resource.record + 40, 24, writes.versions))
      return false;
    if (!resource.version && !resource.version_pending) {
      writes.counter = resource.counter;
      if (!BackingRange(resource.declaration + 48, 4, writes.declaration))
        return false;
    }
  }
  if (resource.version_pending && resource.cache_possible &&
      resource.kind == shader_resource_detail::Kind::kBusy)
    plan.pending_busy_slot = slot;
  ready = resource.kind != shader_resource_detail::Kind::kBusy;
  return true;
}

inline bool Inline(Plan& plan, PPCContext& work, const uint8_t* base, uint32_t variant,
                   uint32_t metadata, uint32_t pixel_metadata, bool linkage) {
  work.r3.u64 = plan.device;
  work.r4.u64 = linkage;
  work.r5.u64 = plan.vertex;
  work.r6.u64 = plan.result_address;
  work.r7.u64 = plan.declaration;
  work.r8.u64 = metadata;
  work.r9.u64 = pixel_metadata;
  work.r10.u64 = variant;
  auto& value = plan.inline_shader.emplace();
  if (!inline_shader_detail::BuildPlan(work, base, value, &plan.cursor, &plan.vertex_mask) ||
      !AddVertexInput(plan, value.object) || !AddVertexInput(plan, value.declaration.metadata) ||
      !AddVertexInput(plan, value.declaration.attributes) ||
      !AddInput(plan, value.declaration.tables))
    return false;
  if (linkage &&
      (!AddInput(plan, value.exports.resource) || !AddInput(plan, value.exports.descriptor) ||
       !AddInput(plan, value.exports.secondary)))
    return false;
  if (value.exports.write_result)
    plan.vertex_mask = value.exports.result_value;
  plan.shader_flags = (plan.shader_flags & 0x7F) | (linkage ? 0x80 : 0);
  plan.cursor += 20 + value.bytes;
  plan.inline_used = true;
  return true;
}

inline bool ViewInputs(Plan& plan, const shader_state_detail::ShaderView& view) {
  return AddInput(plan, view.base_word) && AddInput(plan, view.selector_word) &&
         AddInput(plan, view.code_words) && !Overlaps(view.code, plan.device_range) &&
         !Overlaps(view.code, plan.stack);
}

inline bool MutableOverlaps(const Plan& plan, uint32_t slot, const MemoryRange& input) {
  const auto& writes = plan.resource_writes[slot];
  if (Overlaps(input, writes.versions) ||
      (!plan.resources[slot]->version && !plan.resources[slot]->version_pending &&
       (Overlaps(input, writes.declaration) || Overlaps(input, writes.counter))))
    return true;
  return vertex_declaration_detail::WritesOverlap(plan.resources[slot]->patch, input);
}

inline bool Ownership(const Plan& plan) {
  const auto output_safe = [&](const MemoryRange& range) {
    return !Overlaps(range, plan.commands) && !Overlaps(range, plan.device_range) &&
           !Overlaps(range, plan.stack);
  };
  for (uint32_t i = 0; i < plan.input_count; ++i) {
    const auto& input = plan.inputs[i];
    if (Overlaps(input, plan.commands))
      return false;
    for (uint32_t j = 0; j < plan.mutator_count; ++j)
      if (MutableOverlaps(plan, plan.mutators[j], input))
        return false;
  }
  const auto source_safe = [&](const MemoryRange& range) {
    if (!output_safe(range))
      return false;
    for (uint32_t i = 0; i < plan.mutator_count; ++i)
      if (MutableOverlaps(plan, plan.mutators[i], range))
        return false;
    return true;
  };
  for (const auto& constants : plan.constants)
    for (uint32_t i = 0; i < constants.count; ++i) {
      const auto& record = constants.records[i];
      MemoryRange source;
      if (!BackingRange(record.source, uint64_t(record.count) * 4, source) || !source_safe(source))
        return false;
    }
  for (uint32_t i = 0; i < plan.resource_count; ++i) {
    const auto& resource = *plan.resources[i];
    if (resource.kind == shader_resource_detail::Kind::kRebuild &&
        (MutableOverlaps(plan, i, plan.commands) || MutableOverlaps(plan, i, plan.stack)))
      return false;
    if (!resource.version && !resource.version_pending &&
        resource.kind == shader_resource_detail::Kind::kRebuild &&
        (!output_safe(resource.counter) || Overlaps(resource.counter, plan.pixel_code) ||
         (plan.inline_used && Overlaps(resource.counter, plan.inline_shader->source))))
      return false;
    // The second decision must be planned against the preceding allocation.
    // A possible busy/cache hit carries both proved final command sequences.
    if (i + 1 < plan.resource_count && !resource.version &&
        resource.kind == shader_resource_detail::Kind::kRebuild &&
        !plan.resources[i + 1]->version_pending)
      return false;
  }
  if (plan.pixel_used && !output_safe(plan.pixel_code))
    return false;
  if (plan.inline_used && !output_safe(plan.inline_shader->source))
    return false;
  const auto view_safe = [&](const shader_state_detail::ShaderView& view) {
    if (!output_safe(view.code))
      return false;
    for (uint32_t i = 0; i < plan.input_count; ++i)
      if (Overlaps(view.version, plan.inputs[i]))
        return false;
    if (Overlaps(view.version, plan.pixel_code) || Overlaps(view.version, view.code) ||
        (plan.inline_used && Overlaps(view.version, plan.inline_shader->source)))
      return false;
    for (const auto& constants : plan.constants)
      for (uint32_t i = 0; i < constants.count; ++i) {
        const auto& record = constants.records[i];
        MemoryRange source;
        if (!BackingRange(record.source, uint64_t(record.count) * 4, source) ||
            Overlaps(view.version, source))
          return false;
      }
    // Raw CPU patching and the physical code consumed by the GPU must name the
    // same owner. Unproved virtual-to-physical arrangements stay on the guest path.
    for (uint32_t i = 0; i < plan.resource_count; ++i) {
      const auto& resource = *plan.resources[i];
      if (resource.kind != shader_resource_detail::Kind::kRebuild ||
          resource.record != plan.vertex + view.variant * 416 || !resource.patch.count)
        continue;
      MemoryRange raw;
      if (!BackingRange(uint32_t(resource.patch.code), uint64_t(view.call.words) * 4, raw) ||
          raw.first != view.code.first || raw.end != view.code.end)
        return false;
    }
    return true;
  };
  if (plan.primary_used && !view_safe(plan.primary))
    return false;
  for (uint32_t i = 0; i < (plan.conditional_used ? plan.conditional->view_count : 0); ++i)
    if (!view_safe(plan.conditional->views[i]))
      return false;
  if (plan.conditional_cached)
    for (uint32_t i = 0; i < plan.conditional_cached->view_count; ++i)
      if (!view_safe(plan.conditional_cached->views[i]))
        return false;
  return true;
}

inline bool BuildPlan(const PPCContext& ctx, const uint8_t* base, Plan& plan) {
  const uint32_t device = ctx.r3.u32, sp = ctx.r1.u32;
  if (!base || (device & 3) || (sp & 15) || sp < 1080 ||
      !BackingRange(device, 22256, plan.device_range) ||
      !BackingRange(sp - 1080, 1080, plan.stack) || Overlaps(plan.device_range, plan.stack))
    return false;
  plan.device = device;
  plan.flags = ctx.r4.u64;
  plan.initial = plan.cursor = LoadGuestU32(base, device + 48);
  plan.program = LoadGuestU32(base, device + 10580);
  plan.depth = LoadGuestU32(base, device + 10372);
  plan.loop_low = LoadGuestU32(base, device + 10528);
  plan.loop_high = LoadGuestU32(base, device + 10532);
  // Child planners read only their documented argument registers. Keep the
  // large vector/FP context out of this host-only planning frame.
  PPCContext work;
  work.r1.u64 = sp - 256;
  work.r3.u64 = device;
  if (plan.flags & 0x20000) {
    if (!shader_control_detail::BuildPlan(work, base, plan.control) ||
        !AddInput(plan, plan.control.shader))
      return false;
    plan.control_used = true;
    plan.flags |= 0x100;
    if (plan.control.emit)
      plan.cursor += 8;
  }
  plan.vertex = LoadGuestU32(base, device + 12688);
  plan.declaration = LoadGuestU32(base, device + 11812);
  if (!plan.vertex) {
    if (plan.cursor != plan.initial &&
        !BackingRange(plan.initial + 4, plan.cursor - plan.initial, plan.commands))
      return false;
    return Ownership(plan);
  }
  plan.vertex_used = true;
  const uint32_t vertex = plan.vertex, pixel = LoadGuestU32(base, device + 12684);
  if ((vertex & 3) || !Input(plan, vertex, 36) || !Input(plan, uint64_t(vertex) + 872, 4) ||
      !Input(plan, uint64_t(vertex) + 896, 4))
    return false;
  const uint32_t vertex_flags = LoadGuestU32(base, vertex + 872);
  uint32_t variant = !pixel && (vertex_flags & 0x20) ? 1 : 0;
  if (variant && !Input(plan, uint64_t(vertex) + 904, 4))
    return false;
  const uint64_t metadata_address =
      uint64_t(vertex) + LoadGuestU32(base, vertex + 896 + variant * 8) + 872;
  if ((metadata_address & 3) || !Input(plan, metadata_address, 32))
    return false;
  const uint32_t metadata = uint32_t(metadata_address);
  uint32_t pixel_metadata = 0, pixel_low = 0, pixel_high = 0;
  plan.shader_flags = *GuestToHost(base, device + 10942);
  bool mismatch;
  if (!pixel) {
    if ((plan.program & 7) != 5) {
      plan.program = (plan.program & ~7u) | 5;
      plan.program_used = true;
      plan.flags |= 0x80008;
      if (*GuestToHost(base, device + 10943) & 4) {
        plan.depth &= ~0xF0000u;
        plan.depth_used = true;
        plan.flags |= 1ull << 56;
      }
    }
    mismatch = (LoadGuestU32(base, metadata + 20) & 31) != 0;
  } else {
    if ((pixel & 3) || !Input(plan, pixel, 68))
      return false;
    const uint64_t address = uint64_t(pixel) + LoadGuestU32(base, pixel + 64) + 40;
    if ((address & 3) || !Input(plan, address, 32))
      return false;
    pixel_metadata = uint32_t(address);
    pixel_low = LoadGuestU32(base, pixel_metadata + 8);
    pixel_high = LoadGuestU32(base, pixel_metadata + 12);
    // This is an OR into the existing bit, including a previously set bit.
    if ((pixel_low & 0x70000000) > 0x10000000)
      plan.shader_flags |= 8;
    if (plan.flags & 0x100000) {
      if (!Constants(plan, work, base, 0, pixel + 40, LoadGuestU32(base, pixel + 24)))
        return false;
      MemoryRange output, dev, stack;
      work.r30.u64 = device;
      work.r11.u64 = plan.cursor;
      if (!shader_state_detail::Output(base, work, 3, output, dev, stack))
        return false;
      const uint64_t raw =
          uint64_t(LoadGuestU32(base, pixel + 24)) + LoadGuestU32(base, pixel_metadata);
      const uint32_t bytes = LoadGuestU32(base, pixel_metadata + 4);
      if (raw > UINT32_MAX || (bytes & 3))
        return false;
      const rex::graphics::native_gpu_shader_reference::Call call{
          ((uint32_t(raw) & 0x1FFFFFFEu) + (((uint32_t(raw) >> 20) + 512) & 0x1000u)) | 1u,
          bytes / 4};
      if (!rex::graphics::native_gpu_shader_reference::Validate(call) ||
          !BackingRange(0xC0000000u + (call.address_type & ~3u), bytes, plan.pixel_code))
        return false;
      plan.pixel_reference_cursor = plan.cursor;
      plan.pixel_address_type = call.address_type;
      plan.pixel_words = call.words;
      plan.cursor += 12;
      plan.pixel_used = true;
      plan.flags |= 0x4000;
      plan.pixel_input = LoadGuestU32(base, pixel_metadata + 24);
      if ((plan.program & 7) != 4) {
        plan.program = (plan.program & ~7u) | 4;
        plan.program_used = true;
        plan.flags |= 0x80008;
        if (*GuestToHost(base, device + 10943) & 4) {
          const uint64_t depth = uint64_t(LoadGuestU32(base, device + 12432)) + 28;
          if ((depth & 3) || !Input(plan, depth, 4))
            return false;
          plan.depth = LoadGuestU32(base, uint32_t(depth));
          plan.depth_used = true;
          plan.flags |= 1ull << 56;
        }
      }
    }
    mismatch = ((LoadGuestU32(base, pixel_metadata + 20) ^ LoadGuestU32(base, metadata + 20)) &
                0x7FFFF) != 0;
  }
  if (!LoadGuestU32(base, device + 13488) &&
      ((vertex_flags & 0x10) || (pixel && (LoadGuestU32(base, pixel + 40) & 0x10)))) {
    if (!Input(plan, 0x82D5155C, 4) || !LoadGuestU32(base, 0x82D5155C))
      return false;
  }
  plan.vertex_mask = LoadGuestU32(base, metadata + 8);
  plan.result_address = sp - 176;
  const uint32_t vertex_high = LoadGuestU32(base, metadata + 12);
  uint32_t effective = plan.program, mode_predicate = UINT32_MAX, depth_predicate = UINT32_MAX;
  const uint8_t device_flags = *GuestToHost(base, device + 10940);
  if (device_flags & 0x40) {
    if (!pixel || (!(LoadGuestU32(base, device + 10556) & 8) &&
                   !(LoadGuestU32(base, pixel_metadata + 28) & 0x30)))
      effective = (effective & ~7u) | 5;
    if (effective != LoadGuestU32(base, device + 12692) || (plan.flags & 8)) {
      plan.mode = mode_predicate = effective;
      plan.mode_used = true;
      plan.flags |= 8;
      if (*GuestToHost(base, device + 10943) & 4)
        depth_predicate = effective % 8 == 5 ? plan.depth & ~0xF0000u : plan.depth;
    }
  }
  const bool mode_changed = ((effective ^ plan.program) & 7) != 0;
  uint32_t primary = UINT32_MAX, alternate = UINT32_MAX;
  if (!(plan.shader_flags & 0x80) && !mismatch) {
    if (plan.flags & 0x80000) {
      if (!Constants(plan, work, base, 1, vertex + 872, LoadGuestU32(base, vertex + 32)))
        return false;
      plan.shader_flags &= ~0x40;
      bool alternate_allowed = true;
      if (plan.declaration && !(vertex_flags & 0x40)) {
        if (device_flags & 0x80) {
          if (!Inline(plan, work, base, variant, metadata, pixel_metadata, false))
            return false;
          alternate_allowed = false;
        } else {
          bool ready;
          if (!Resource(plan, work, base, variant, ready))
            return false;
          if (ready) {
            primary = variant;
          } else if (!Inline(plan, work, base, variant, metadata, pixel_metadata, false)) {
            return false;
          }
        }
      } else {
        primary = variant;
      }
      if (alternate_allowed && mode_changed && (vertex_flags & 0x20)) {
        bool ready = true;
        if (plan.declaration && !(vertex_flags & 0x40) && !Resource(plan, work, base, 1, ready))
          return false;
        if (ready) {
          alternate = 1;
          plan.shader_flags |= 0x40;
        }
      }
    }
  } else if (plan.flags & 0x180000) {
    plan.shader_flags &= ~0x40;
    if (!Constants(plan, work, base, 1, vertex + 872, LoadGuestU32(base, vertex + 32)) ||
        !Inline(plan, work, base, variant, metadata, pixel_metadata, mismatch))
      return false;
  }
  uint32_t alternate_low = UINT32_MAX, alternate_high = UINT32_MAX;
  uint32_t cached_low = UINT32_MAX, cached_high = UINT32_MAX;
  if (plan.flags & 0x180000) {
    plan.loop_low = plan.vertex_mask | pixel_low;
    plan.loop_high = vertex_high | pixel_high;
    plan.loops_used = true;
    plan.flags |= 0x18000;
    if ((plan.shader_flags & 0x40) || plan.pending_busy_slot != UINT32_MAX) {
      if (!Input(plan, uint64_t(vertex) + 904, 4))
        return false;
      const uint64_t extra = uint64_t(vertex) + LoadGuestU32(base, vertex + 904) + 880;
      if ((extra & 3) || !Input(plan, extra, 8))
        return false;
      cached_low = LoadGuestU32(base, uint32_t(extra)) | pixel_low;
      cached_high = LoadGuestU32(base, uint32_t(extra) + 4) | pixel_high;
      if (plan.shader_flags & 0x40) {
        alternate_low = cached_low;
        alternate_high = cached_high;
      }
    }
  }
  work.r30.u64 = device;
  work.r31.u64 = vertex;
  work.r11.u64 = plan.cursor;
  if (device_flags & 0x40) {
    const uint32_t final_initial = plan.cursor;
    const uint64_t pending_flags = plan.flags;
    work.r17.s64 = int32_t(0xC0012700);
    work.r19.s64 = int32_t(primary);
    work.r14.s64 = int32_t(mode_predicate);
    work.r23.s64 = int32_t(depth_predicate);
    const shader_state_detail::Values values{plan.program, plan.depth, plan.loop_low,
                                             plan.loop_high};
    const auto final_plan = [&](uint32_t alt, uint32_t low, uint32_t high,
                                shader_state_detail::ConditionalPlan& final) {
      work.r11.u64 = final_initial;
      work.r20.u64 = pending_flags;
      work.r15.s64 = int32_t(alt);
      work.r29.s64 = int32_t(low);
      work.r28.u64 = high;
      if (!shader_state_detail::BuildConditionalPlan<false>(work, base, final, &values))
        return false;
      for (uint32_t i = 0; i < final.view_count; ++i)
        if (!ViewInputs(plan, final.views[i]))
          return false;
      return true;
    };
    if (!final_plan(alternate, alternate_low, alternate_high, plan.conditional.emplace()))
      return false;
    if (plan.pending_busy_slot != UINT32_MAX) {
      if (plan.pending_busy_slot != 1 ||
          !final_plan(1, cached_low, cached_high, plan.conditional_cached.emplace()))
        return false;
    }
    plan.cursor = plan.conditional->next.r11.u32;
    plan.flags = plan.conditional->next.r20.u64;
    plan.conditional_used = true;
  } else if (primary != UINT32_MAX) {
    MemoryRange output, dev, stack;
    if (!shader_state_detail::Output(base, work, 3, output, dev, stack) ||
        !shader_state_detail::PrepareView(base, vertex, primary, output, dev, stack,
                                          plan.primary) ||
        !ViewInputs(plan, plan.primary))
      return false;
    plan.primary_used = true;
    plan.cursor += 12;
  }
  if (plan.pending_busy_slot != UINT32_MAX && !plan.conditional_cached)
    return false;
  const uint32_t maximum = plan.conditional_cached
                               ? std::max(plan.cursor, plan.conditional_cached->next.r11.u32)
                               : plan.cursor;
  if (maximum != plan.initial &&
      (maximum < plan.initial ||
       !BackingRange(plan.initial + 4, maximum - plan.initial, plan.commands) ||
       Overlaps(plan.commands, plan.device_range) || Overlaps(plan.commands, plan.stack)))
    return false;
  return Ownership(plan);
}

inline uint64_t Apply(const Plan& plan, PPCContext& ctx, uint8_t* base) {
  const uint32_t device = plan.device;
  if (plan.control_used)
    shader_control_detail::Apply(plan.control, base);
  if (!plan.vertex_used)
    return plan.flags;
  shader_constants_detail::Apply(plan.constants[0], base);
  if (plan.pixel_used) {
    StoreGuestU32(base, plan.pixel_reference_cursor + 4,
                  rex::graphics::native_gpu_shader_reference::kHeader);
    StoreGuestU32(base, plan.pixel_reference_cursor + 8, plan.pixel_address_type);
    StoreGuestU32(base, plan.pixel_reference_cursor + 12, plan.pixel_words);
    StoreGuestU32(base, device + 48, plan.pixel_reference_cursor + 12);
    StoreGuestU32(base, device + 10536, plan.pixel_input);
  }
  if (plan.program_used)
    StoreGuestU32(base, device + 10580, plan.program);
  if (plan.depth_used)
    StoreGuestU32(base, device + 10372, plan.depth);
  if (plan.mode_used)
    StoreGuestU32(base, device + 12692, plan.mode);
  StoreGuestU32(base, plan.result_address, plan.vertex_mask);
  shader_constants_detail::Apply(plan.constants[1], base);
  // A primary busy result falls back to inline before testing the second variant.
  if (plan.resource_count)
    shader_resource_detail::Apply(*plan.resources[0], ctx, base);
  if (plan.inline_used)
    inline_shader_detail::Apply(*plan.inline_shader, ctx, base);
  bool cached_final = false;
  if (plan.resource_count > 1) {
    const auto kind = shader_resource_detail::Apply(*plan.resources[1], ctx, base);
    cached_final = plan.conditional_cached && kind == shader_resource_detail::Kind::kCached;
  }
  *GuestToHost(base, device + 10942) = plan.shader_flags | (cached_final ? 0x40 : 0);
  if (plan.loops_used) {
    StoreGuestU32(base, device + 10532, plan.loop_high);
    StoreGuestU32(base, device + 10528, plan.loop_low);
  }
  if (plan.conditional_used) {
    const auto& final = cached_final ? *plan.conditional_cached : *plan.conditional;
    shader_state_detail::ApplyMemory(final, base);
    StoreGuestU32(base, device + 48, final.next.r11.u32);
    return final.next.r20.u64;
  } else if (plan.primary_used) {
    StoreGuestU32(base, plan.cursor - 8, rex::graphics::native_gpu_shader_reference::kHeader);
    StoreGuestU32(base, plan.cursor - 4, plan.primary.call.address_type);
    StoreGuestU32(base, plan.cursor, plan.primary.call.words);
    StoreGuestU32(base, plan.primary.version_address, LoadGuestU32(base, device + 10908));
    StoreGuestU32(base, device + 48, plan.cursor);
  }
  return plan.flags;
}
}  // namespace shader_coordinator_detail

// 8245FA00: the complete bounded, ready-buffer shader/state coordinator. Ring
// growth, debug traps and unproved alternate paths retain the original.
inline bool CoordinateShaders(PPCContext& ctx, uint8_t* base) {
  shader_coordinator_detail::Plan plan;
  if (!shader_coordinator_detail::BuildPlan(ctx, base, plan))
    return false;
  ctx.r3.u64 = shader_coordinator_detail::Apply(plan, ctx, base);
  ctx.lr = uint32_t(ctx.lr);
  return true;
}
}  // namespace simpsons::native_gpu_calls
