#pragma once

#include <algorithm>
#include <array>
#include <cstring>

#include "guest_memory.h"
#include "native_gpu_memory.h"

namespace simpsons::native_gpu_calls {

// 82445240: the leaf initializer deliberately preserves the full link register.
inline bool InitializePixelResource(PPCContext& ctx, uint8_t* base) {
  using namespace native_gpu_memory;
  MemoryRange output;
  const uint32_t resource = ctx.r3.u32;
  if (!base || (resource & 3) || !BackingRange(resource, 40, output))
    return false;
  std::memset(GuestToHost(base, resource), 0, 40);
  StoreGuestU32(base, resource, 7);
  StoreGuestU32(base, resource + 4, 1);
  StoreGuestU32(base, resource + 20, 0xFFFF0000);
  StoreGuestU32(base, resource + 24, ctx.r4.u32);
  return true;
}

// 824457F0: the real initializer's private stream mask contains sixteen bytes.
// Prove the terminated input and the entire output before clearing or copying.
inline bool InitializeVertexDeclaration(PPCContext& ctx, uint8_t* base) {
  using namespace native_gpu_memory;
  const uint32_t input = ctx.r3.u32, output = ctx.r4.u32;
  MemoryRange source, destination, stack;
  if (!base || ((input | output) & 3) || (ctx.r1.u32 & 15) || ctx.r1.u32 < 144 ||
      !BackingRange(ctx.r1.u32 - 144, 144, stack))
    return false;
  std::array<uint8_t, 16> streams{};
  uint32_t count = 0, maximum = 0;
  for (;;) {
    if (uint64_t(input) + uint64_t(count) * 12 + 2 > 0x100000000ull ||
        !BackingRange(input, uint64_t(count) * 12 + 2, source) || Overlaps(source, stack))
      return false;
    const uint32_t stream = LoadGuestU16(base, input + count * 12);
    if (stream == 255)
      break;
    if (stream >= streams.size() || count == 256)
      return false;
    streams[stream] = 255;
    maximum = std::max(maximum, stream);
    ++count;
  }
  const uint32_t bytes = count * 12 + 56;
  if (!BackingRange(output, bytes, destination) || Overlaps(source, destination) ||
      Overlaps(destination, stack))
    return false;
  std::memset(GuestToHost(base, output), 0, bytes);
  StoreGuestU32(base, output + 24, count);
  StoreGuestU32(base, output + 28, maximum);
  StoreGuestU32(base, output + 4, 1);
  StoreGuestU32(base, output, 0x100005);
  StoreGuestU32(base, output + 20, 0xFFFF0000);
  std::memcpy(GuestToHost(base, output + 32), streams.data(), streams.size());
  for (uint32_t i = 0; i < count; ++i)
    for (uint32_t j = 0; j < 3; ++j)
      StoreGuestU32(base, output + 52 + i * 12 + j * 4, LoadGuestU32(base, input + i * 12 + j * 4));
  ctx.r3.u64 = ctx.r4.u64;
  ctx.lr = uint32_t(ctx.lr);
  return true;
}

// 82445480: capture each variant's original fetch instructions as templates for
// the native declaration patcher. Metadata and source code must survive the
// original 872-byte clear; copies publish at the original twelve-byte offsets.
inline bool InitializeVertexShaderTemplates(PPCContext& ctx, uint8_t* base) {
  using namespace native_gpu_memory;
  struct Copy {
    uint32_t source, destination;
  };
  std::array<Copy, 64> copies;
  uint32_t copy_count = 0;
  const uint32_t resource = ctx.r3.u32;
  MemoryRange output, stack, flags;
  if (!base || (resource & 3) || (ctx.r1.u32 & 15) || ctx.r1.u32 < 168 ||
      !BackingRange(resource, 872, output) || !BackingRange(ctx.r1.u32 - 168, 168, stack) ||
      uint64_t(resource) + 876 > 0x100000000ull || !BackingRange(resource + 872, 4, flags) ||
      Overlaps(output, stack) || Overlaps(flags, stack))
    return false;
  const uint32_t variants = 1 + uint32_t((LoadGuestU32(base, resource + 872) & 32) != 0);
  for (uint32_t variant = 0; variant < variants; ++variant) {
    const uint64_t selector = uint64_t(resource) + 896 + variant * 8;
    MemoryRange selected_word, metadata;
    if (selector + 4 > 0x100000000ull || !BackingRange(uint32_t(selector), 4, selected_word) ||
        Overlaps(selected_word, stack) || Overlaps(selected_word, output))
      return false;
    const uint64_t header = uint64_t(resource) + LoadGuestU32(base, uint32_t(selector)) + 872;
    if (header + 32 > 0x100000000ull || !BackingRange(uint32_t(header), 32, metadata) ||
        Overlaps(metadata, stack) || Overlaps(metadata, output))
      return false;
    const uint32_t count = LoadGuestU32(base, uint32_t(header) + 28);
    const uint64_t table = header + (uint64_t(LoadGuestU32(base, uint32_t(header) + 24)) + 9) * 4;
    const uint64_t code = uint64_t(ctx.r4.u32) + LoadGuestU32(base, uint32_t(header));
    MemoryRange records{};
    if (count > 32 || (count && (table + count * 4 > 0x100000000ull ||
                                 !BackingRange(uint32_t(table), count * 4, records) ||
                                 Overlaps(records, stack) || Overlaps(records, output))))
      return false;
    for (uint32_t i = 0; i < count; ++i) {
      const uint64_t address = code + (LoadGuestU32(base, uint32_t(table) + i * 4) & 0xFFF) * 12;
      MemoryRange instruction;
      if (address > UINT32_MAX || !BackingRange(uint32_t(address), 12, instruction) ||
          Overlaps(instruction, output) || Overlaps(instruction, stack))
        return false;
      copies[copy_count++] = {uint32_t(address), 68 + variant * 416 + i * 12};
    }
  }
  uint64_t result = ctx.r3.u64;
  std::memset(GuestToHost(base, resource), 0, 872);
  StoreGuestU32(base, resource + 32, ctx.r4.u32);
  StoreGuestU32(base, resource, 6);
  StoreGuestU32(base, resource + 4, 1);
  StoreGuestU32(base, resource + 20, 0xFFFF0000);
  uint32_t next = 0;
  for (uint32_t variant = 0; variant < variants; ++variant) {
    while (next < copy_count && copies[next].destination < 68 + (variant + 1) * 416) {
      const auto& copy = copies[next++];
      std::memcpy(GuestToHost(base, resource + copy.destination), GuestToHost(base, copy.source),
                  12);
      result = ctx.r3.u64 + copy.destination;
    }
    StoreGuestU32(base, resource + 40 + variant * 416, UINT32_MAX);
  }
  ctx.r3.u64 = result;
  ctx.lr = uint32_t(ctx.lr);
  return true;
}

}  // namespace simpsons::native_gpu_calls
