#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <string_view>

#include <rex/graphics/native_gpu_calls.h>

#include "guest_memory.h"
#include "native_gpu_memory.h"
#include "native_gpu_direct_draws.h"

struct PPCContext;

namespace simpsons::native_gpu_calls {

consteval uint32_t FunctionAddress(std::string_view hex) {
  uint32_t result = 0;
  for (char c : hex)
    result = (result << 4) | uint32_t(c <= '9' ? c - '0' : c - 'A' + 10);
  return result;
}

bool TrySimpleDraw(uint32_t address, PPCContext& ctx, uint8_t* base);
bool TryRetirementChunk(PPCContext& ctx, uint8_t* base);
bool TryClearResolveDraw(PPCContext& ctx, uint8_t* base);
bool TryCopyResolveDraw(PPCContext& ctx, uint8_t* base);
bool TryIndirectConstants(PPCContext& ctx, uint8_t* base);
bool TryShaderSetup(uint32_t kind, PPCContext& ctx, uint8_t* base);
bool TryPrimaryShaderReference(PPCContext& ctx, uint8_t* base);
bool TryShaderState(uint32_t kind, PPCContext& ctx, uint8_t* base);

// Command-builder handlers 17/18 and 38/39 have the same bounded contract:
// r3 is the builder, r4 topology, r5 count, r6 indices, r7 index format and
// r8 coherency start for indexed draws (r6 for non-indexed draws). The native
// replacement preserves the builder's byte advance and all non-command data.
inline bool IsSimpleDraw(uint32_t address) {
  return address == 0x824626B8 || address == 0x82462810 || address == 0x82464590 ||
         address == 0x824646E8;
}

inline bool WriteSimpleDraw(uint8_t* base, uint32_t address, uint32_t builder, uint32_t primitive,
                            uint32_t count, uint32_t indices, uint32_t index_format,
                            uint32_t coherent_start) {
  if (!base || !IsSimpleDraw(address) || (builder & 3))
    return false;
  using namespace native_gpu_memory;
  MemoryRange builder_range, device_range, output_range;
  if (!BackingRange(builder, 32, builder_range))
    return false;
  const bool indexed = address == 0x82462810 || address == 0x824646E8;
  const uint32_t dwords = indexed ? 27 : 24;
  const uint32_t write = LoadGuestU32(base, builder + 16);
  if ((write & 3) || write > UINT32_MAX - dwords * 4)
    return false;
  const uint32_t device = LoadGuestU32(base, builder);
  if ((device & 3) || !BackingRange(device, 24, device_range) ||
      !BackingRange(write, dwords * 4, output_range) || Overlaps(output_range, builder_range) ||
      Overlaps(output_range, device_range))
    return false;
  const uint32_t end = LoadGuestU32(base, device + 20);
  const uint32_t start = (indexed ? coherent_start : indices) + 0x41000000u;
  const uint32_t coherent_base = start & 0xFFFFF000u;
  const uint32_t coherent_size = end - coherent_base + 0x41000000u;
  uint32_t index_base = 0, index_size = 0;
  uint32_t initiator = (primitive & 63) | (count << 16);
  if (indexed) {
    index_size = (count & 0xFFFFFFu) | 0x40000000u;
    if (index_format == 6) {
      initiator |= 0x800;
      index_size = ((count << 1) & 0xFFFFFEu) | 0x80000000u;
    }
    if (indices - 0x7F100000u <= 0x07EFFFFFu)
      index_base = indices + 0x41000000u;
    else
      index_base = (indices & 0x1FFFFFFFu) + (((indices >> 20) + 512) & 0x1000u);
  } else {
    initiator |= 0x80;
  }
  // Preserve operand offsets used by command-buffer patch tables, while
  // replacing every old packet header with native metadata or reserved space.
  std::array<uint32_t, 27> words{};
  using namespace rex::graphics::native_gpu_calls;
  DrawCall call{{0, 0, 0, 0, start | 3, (coherent_size & 0x03FFFFFCu) | 0x10000002u},
                coherent_size, coherent_base, 0x05000000, {}, 0, initiator, index_base, index_size};
  if (!EncodeDraw(call, indexed, {words.data(), dwords}))
    return false;
  for (uint32_t i = 0; i < dwords; ++i)
    StoreGuestU32(base, write + i * 4, words[i]);
  StoreGuestU32(base, builder + 16, write + dwords * 4);
  return true;
}

}  // namespace simpsons::native_gpu_calls
