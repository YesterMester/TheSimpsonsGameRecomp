#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <rex/ppc/context.h>

#include "guest_memory.h"
#include "native_gpu_memory.h"

namespace simpsons::native_gpu_calls {
namespace vertex_declaration_detail {
using namespace native_gpu_memory;
constexpr uint32_t kSwizzleTable = 0x8206B038, kFormatSizes = 0x8206B048;

struct Instruction {
  uint32_t first, second, third;
};

inline uint32_t Stream(const Instruction& value) {
  return (std::rotl(value.first, 14) & 0x7C) | (std::rotl(value.first, 7) & 3);
}

inline int32_t Offset(const Instruction& value) {
  return int32_t(value.third << 1) >> 9;
}

inline uint32_t Size(const uint8_t* base, const Instruction& value) {
  return *GuestToHost(base, kFormatSizes + ((value.second >> 16) & 63));
}

inline uint32_t Swizzle(uint32_t value, uint32_t code) {
  value = (value | 14) & code & 0xFFFF;
  // The original reduces the selected swizzle bits with four three-bit folds.
  uint32_t reduced = value;
  for (uint32_t i = 0; i < 4; ++i)
    reduced = (reduced >> 3) | value;
  return (reduced >> 1) & 7;
}

inline void Patch(Instruction& value, const uint8_t* base, uint32_t attribute, uint32_t streams,
                  uint32_t swizzle_seed) {
  uint32_t swizzle;
  if (!attribute) {
    value.first = (value.first & ~0x3FF00000u) | (95u << 20);
    value.second = (value.second & 0xBFC6CFFFu) | 0x60000;
    value.third = (value.third & 0x80000000u) | *GuestToHost(base, streams);
    swizzle = 37456 | swizzle_seed;
  } else {
    const uint32_t format = LoadGuestU32(base, attribute + 4);
    uint32_t packed = (std::rotl(format, 16) & 7) | (std::rotl(format, 22) & 0x38);
    packed = ((packed << 3) | (std::rotl(format, 13) & 7)) << 1;
    packed |= format & 0xFC00;
    swizzle = ((packed << 3) | swizzle_seed) & 0xFFFF;
    if ((format & 0xC0) == 64) {
      for (auto [mask, limit, flip] : {std::array<uint32_t, 3>{0xE000, 0x6000, 0x2000},
                                       {0x1C00, 0xC00, 0x400},
                                       {0x380, 0x180, 0x80},
                                       {0x70, 0x30, 0x10}})
        if ((swizzle & mask) <= limit)
          swizzle ^= flip;
    }
    const uint32_t stream = LoadGuestU16(base, attribute);
    const uint32_t offset = LoadGuestU16(base, attribute + 2);
    value.second = (value.second & 0xBFC0CFFFu) |
                   (((std::rotl(format, 12) & 0x3F000) | (format & 0x300)) << 4);
    const uint32_t reciprocal = uint32_t((int64_t(95) - stream) * 0x01560056);
    const uint32_t adjusted = (reciprocal & ~0xF80000u) | (std::rotl(reciprocal, 11) & 0xF80000);
    value.first =
        ((value.first & ~0x07F00000u) | (std::rotl(adjusted, 1) & 0x07F00000)) & ~0x38000000u;
    value.third = (value.third & 0x80000000u) | ((offset << 6) & 0x7FFFFF00u) |
                  *GuestToHost(base, streams + stream);
  }
  uint32_t result = value.second;
  for (uint32_t component = 0; component < 4; ++component) {
    const uint32_t selector = (result >> (component * 3)) & 7;
    const uint32_t mapped = Swizzle(swizzle, LoadGuestU16(base, kSwizzleTable + selector * 2));
    result = (result & ~(7u << (component * 3))) | (mapped << (component * 3));
  }
  value.second = result;
}

inline void OrderAndMerge(std::array<Instruction, 32>& values,
                          const std::array<uint32_t, 32>& records, uint32_t count,
                          const uint8_t* base) {
  uint32_t scan = 0;
  while (scan < count) {
    while (scan < count && !(records[scan] & 0x300000))
      ++scan;
    const uint32_t first = scan++;
    while (scan < count && !(records[scan] & 0x300000))
      ++scan;
    if (scan >= count)
      break;
    const uint32_t last = scan++;
    const bool fixed_last = (records[last] & 0x300000) == 0x300000;
    const uint32_t special = fixed_last ? Stream(values[last]) : UINT32_MAX;
    const uint32_t sort_end = last + !fixed_last;
    // Preserve the original pairwise order, including equal-key instructions.
    for (uint32_t i = first; i + 1 < sort_end; ++i)
      for (uint32_t j = i + 1; j < sort_end; ++j) {
        const uint32_t a = Stream(values[i]), b = Stream(values[j]);
        bool swap;
        if (a != b)
          swap = a == special || (b != special && int32_t(b - a) > 0);
        else if (Offset(values[i]) != Offset(values[j]))
          swap = int32_t(uint32_t(Offset(values[i])) - uint32_t(Offset(values[j]))) > 0;
        else
          swap = int32_t(Size(base, values[i]) - Size(base, values[j])) > 0;
        if (swap)
          std::swap(values[i], values[j]);
      }
    uint32_t current = first;
    for (uint32_t next = first + 1; next <= last; ++next) {
      const auto key = [](uint32_t word) {
        return ((word & ~0xC0000u) | (std::rotl(word, 25) & 0xC0000)) & 0x1FC0000;
      };
      const uint32_t a = uint32_t(Offset(values[current])), b = uint32_t(Offset(values[next]));
      const uint32_t distance = Size(base, values[next]) - a + b;
      if (key(values[current].first) == key(values[next].first) && a <= b && distance <= 8) {
        const uint32_t old_span = std::rotl(values[current].first, 5) & 7;
        const uint32_t span = std::max(old_span, distance - 1);
        values[current].first =
            (values[current].first & ~0x38000000u) | ((span << 27) & 0x38000000);
        values[next].second |= 0x40000000;
      } else {
        current = next;
      }
    }
  }
}
struct Arguments {
  uint32_t resource, declaration, streams, stack;
  uint64_t code, variant;
};

struct Plan {
  // Only populated slots are read. Keep unused instruction/record slots out of
  // the cached-resource path, including value construction by std::optional.
  Plan() noexcept {}
  std::array<Instruction, 32> instructions;
  std::array<uint32_t, 32> records;
  std::array<uint64_t, 32> backings;
  MemoryRange metadata{}, stack{}, attributes{}, stream_map{}, tables{}, writes{};
  uint64_t code = 0;
  uint32_t count = 0;
  bool writes_tracked = false;
};

// Separating planning from publication lets the resource binder prove all its
// extra output state before committing any instruction or advancing a version.
template <bool TrackWrites = true>
inline bool BuildPlan(const Arguments& args, const uint8_t* base, Plan& plan) {
  const uint32_t resource = args.resource, declaration = args.declaration, streams = args.streams;
  const uint64_t variant = args.variant, selector = (variant + 112) * 8;
  auto& [instructions, records, backings, metadata, stack, attributes, stream_map, tables, writes,
         code, count, writes_tracked] = plan;
  writes_tracked = TrackWrites;
  if (!base || (resource & 3) || (declaration & 3) || (uint32_t(args.code) & 3) ||
      (args.stack & 15) || args.stack < 600 || variant > UINT32_MAX ||
      uint64_t(resource) + selector + 4 > 0x100000000ull ||
      !BackingRange(resource, selector + 4, metadata) ||
      !BackingRange(args.stack - 600, 600, stack) || Overlaps(metadata, stack))
    return false;
  code = args.code;
  const uint64_t selected =
      uint64_t(resource) + LoadGuestU32(base, resource + uint32_t(selector)) + 872;
  const uint64_t templates = uint64_t(resource) + variant * 416 + 68;
  uint64_t end = std::max(uint64_t(resource) + selector + 4, selected + 32);
  if (end > 0x100000000ull || !BackingRange(resource, end - resource, metadata) ||
      Overlaps(metadata, stack))
    return false;
  count = LoadGuestU32(base, uint32_t(selected) + 28);
  if (!count)
    return true;
  if (count > 32 || !BackingRange(declaration, 52, attributes) || Overlaps(attributes, stack))
    return false;
  const uint32_t attribute_count = LoadGuestU32(base, declaration + 24);
  const uint64_t records_address =
      selected + (uint64_t(LoadGuestU32(base, uint32_t(selected) + 24)) + 9) * 4;
  end = std::max(end, std::max(templates + count * 12, records_address + count * 4));
  if (attribute_count > 256 || end > 0x100000000ull ||
      !BackingRange(resource, end - resource, metadata) ||
      !BackingRange(declaration, 52 + attribute_count * 12, attributes) ||
      !BackingRange(streams, 96, stream_map) || !BackingRange(kSwizzleTable, 80, tables) ||
      Overlaps(metadata, stack) || Overlaps(attributes, stack) || Overlaps(stream_map, stack) ||
      Overlaps(tables, stack))
    return false;
  // The original low bit comes from its private frame. Preserve it rather than
  // assuming that a fresh or reused guest stack is zero-initialized.
  const uint32_t swizzle_seed = LoadGuestU16(base, args.stack - 512) & 1;
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t source = uint32_t(templates) + i * 12;
    instructions[i] = {LoadGuestU32(base, source), LoadGuestU32(base, source + 4),
                       LoadGuestU32(base, source + 8)};
    records[i] = LoadGuestU32(base, uint32_t(records_address) + i * 4);
    uint32_t attribute = 0;
    for (uint32_t j = 0; j < attribute_count; ++j) {
      const uint32_t entry = declaration + 52 + j * 12;
      if (*GuestToHost(base, entry + 9) == ((records[i] >> 12) & 15) &&
          *GuestToHost(base, entry + 10) == ((records[i] >> 16) & 15)) {
        attribute = entry;
        if (LoadGuestU16(base, entry) >= 96)
          return false;
        break;
      }
    }
    Patch(instructions[i], base, attribute, streams, swizzle_seed);
    const uint64_t output = uint64_t(uint32_t(code)) + (records[i] & 0xFFF) * 12;
    MemoryRange instruction;
    if (output > UINT32_MAX || !BackingRange(uint32_t(output), 12, instruction) ||
        Overlaps(instruction, metadata) || Overlaps(instruction, attributes) ||
        Overlaps(instruction, stream_map) || Overlaps(instruction, tables) ||
        Overlaps(instruction, stack))
      return false;
    if constexpr (TrackWrites) {
      backings[i] = instruction.first;
      if (i == 0) {
        writes = instruction;
      } else {
        writes.first = std::min(writes.first, instruction.first);
        writes.end = std::max(writes.end, instruction.end);
      }
    }
  }
  OrderAndMerge(instructions, records, count, base);
  return true;
}

inline bool WritesOverlap(const Plan& plan, const MemoryRange& range) {
  if (!plan.count)
    return false;
  if (!plan.writes_tracked) {
    for (uint32_t i = 0; i < plan.count; ++i) {
      MemoryRange instruction;
      if (!BackingRange(uint32_t(plan.code) + (plan.records[i] & 0xFFF) * 12, 12, instruction) ||
          Overlaps(instruction, range))
        return true;
    }
    return false;
  }
  if (!Overlaps(plan.writes, range))
    return false;
  // BuildPlan already proved each twelve-byte output and captured its backing.
  // Only ranges touching the enclosing span need the exact per-slot check.
  for (uint32_t i = 0; i < plan.count; ++i)
    if (Overlaps({plan.backings[i], plan.backings[i] + 12}, range))
      return true;
  return false;
}

inline void Apply(const Plan& plan, uint8_t* base) {
  for (uint32_t i = 0; i < plan.count; ++i) {
    const uint32_t output = uint32_t(plan.code) + (plan.records[i] & 0xFFF) * 12;
    StoreGuestU32(base, output, plan.instructions[i].first);
    StoreGuestU32(base, output + 4, plan.instructions[i].second);
    StoreGuestU32(base, output + 8, plan.instructions[i].third);
  }
}
}  // namespace vertex_declaration_detail

// 8245EDC8: build, order and coalesce the declaration-specific fetch instructions
// before publishing them at the original code indices. Shader binding still
// happens in the caller, after its original fence/version checks.
inline bool PatchVertexDeclaration(PPCContext& ctx, uint8_t* base) {
  using namespace vertex_declaration_detail;
  Plan plan;
  if (!BuildPlan<false>({ctx.r3.u32, ctx.r5.u32, ctx.r6.u32, ctx.r1.u32, ctx.r4.u64, ctx.r7.u64},
                        base, plan))
    return false;
  Apply(plan, base);
  if (plan.count)
    ctx.r3.u64 = plan.code + (plan.records[plan.count - 1] & 0xFFF) * 12;
  ctx.lr = uint32_t(ctx.lr);
  return true;
}

}  // namespace simpsons::native_gpu_calls
