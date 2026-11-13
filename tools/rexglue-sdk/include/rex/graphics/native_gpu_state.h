#pragma once

#include <array>
#include <cstdint>

namespace rex::graphics::native_gpu_state {

// Each opcode identifies a register page. The offset fits in header bits
// 1..7 and 15; bit 0 remains the common predicate and must be clear. The
// ordinary type-3 length and every original payload/patch offset are retained.
constexpr uint32_t kFirstOpcode = 0x72;
constexpr uint32_t kLastOpcode = 0x78;
constexpr uint32_t kCapability = 1u << 3;
constexpr std::array<uint32_t, 7> kPages = {0x2000, 0x2100, 0x2200, 0x2300, 0x4800, 0x4900, 0x5000};

struct StateRange {
  uint32_t first_register, register_count;
};

inline constexpr bool IsOpcode(uint32_t opcode) {
  return opcode >= kFirstOpcode && opcode <= kLastOpcode;
}

inline constexpr bool EncodeHeader(uint32_t first, uint32_t count, uint32_t& header) {
  if (!count || count > 256 || (first & 255u) + count > 256)
    return false;
  uint32_t page = 0;
  for (; page < kPages.size(); ++page)
    if ((first & ~255u) == kPages[page])
      break;
  if (page == kPages.size() || (first >= 0x4800 && first + count > 0x48C0 && first < 0x4900) ||
      (first >= 0x4900 && first + count > 0x4928 && first < 0x5000) ||
      (first >= 0x5000 && first + count > 0x5003))
    return false;
  header = 0xC0000000u | ((count - 1) << 16) | ((kFirstOpcode + page) << 8) |
           ((first & 127u) << 1) | ((first & 128u) << 8);
  return true;
}

inline constexpr bool DecodeHeader(uint32_t header, uint32_t count, StateRange& range) {
  const uint32_t opcode = (header >> 8) & 127u;
  if ((header >> 30) != 3 || !IsOpcode(opcode) || (header & 1) ||
      (((header >> 16) & 0x3FFF) + 1) != count)
    return false;
  const uint32_t first =
      kPages[opcode - kFirstOpcode] + ((header >> 1) & 127u) + ((header >> 8) & 128u);
  uint32_t canonical;
  if (!EncodeHeader(first, count, canonical) || canonical != header)
    return false;
  range = {first, count};
  return true;
}

// Diagnostic counts are external to CommandProcessor, preserving its ABI.
// A newly enabled runtime starts a new interval; shutdown keeps the result.
void RecordCall(uint32_t register_count);
uint64_t CallCount();
uint64_t RegisterCount();
void ResetCounts();

}  // namespace rex::graphics::native_gpu_state
