#pragma once

#include <cstdint>

namespace rex::graphics::native_gpu_constants {

// The opcode fixes this transport's layout. Constants remain at their original
// command-buffer offsets; the header replaces exactly one type-0 header.
constexpr uint32_t kOpcode = 0x7C;

struct ConstantRange {
  uint32_t first_register;
  uint32_t register_count;
};

inline constexpr bool EncodeHeader(uint32_t first_register, uint32_t register_count,
                                   uint32_t& header) {
  if (!register_count || register_count > 1024 || (register_count & 15) ||
      first_register < 0x4000 || first_register >= 0x4800 || (first_register & 15))
    return false;
  const uint32_t stage = (first_register - 0x4000) >> 10;
  const uint32_t offset = first_register & 0x3FF;
  if (offset + register_count > 1024)
    return false;
  // bit 0 is the common type-3 predicate, not a stage/group bit.
  header = 0xC0000000u | ((register_count - 1) << 16) | (kOpcode << 8) |
           (stage << 7) | ((offset >> 4) << 1);
  return true;
}

inline constexpr bool DecodeHeader(uint32_t header, uint32_t payload_count,
                                   ConstantRange& range) {
  if ((header >> 30) != 3 || ((header >> 8) & 0x7F) != kOpcode ||
      (header & 0x8001) || (((header >> 16) & 0x3FFF) + 1) != payload_count)
    return false;
  const uint32_t first_register =
      0x4000u + ((header & 0x80) ? 0x400u : 0u) + ((header >> 1) & 63u) * 16u;
  uint32_t canonical;
  if (!EncodeHeader(first_register, payload_count, canonical) || canonical != header)
    return false;
  range = {first_register, payload_count};
  return true;
}

}  // namespace rex::graphics::native_gpu_constants
