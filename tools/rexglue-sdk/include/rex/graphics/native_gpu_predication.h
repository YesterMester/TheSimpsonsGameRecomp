#pragma once

#include <cstdint>
#include <rex/graphics/native_gpu_state.h>

namespace rex::graphics::native_gpu_predication {

// Mask halves are unconditional. The selected state update keeps the old
// operand layout and applies only when the current selection passes. Ranges
// use the additional capability so older consumers never receive new forms.
constexpr uint32_t kOpcode = 0x6F;
constexpr uint32_t kMaskLowHeader = 0xC0006F00;
constexpr uint32_t kMaskHighHeader = 0xC0006F02;
constexpr uint32_t kStateHeader = 0xC0016F05;
constexpr uint32_t kCapability = 1u << 6;
constexpr uint32_t kRangeCapability = 1u << 11;

inline constexpr bool IsStateHeader(uint32_t header, uint32_t payload_words) {
  return payload_words >= 2 && payload_words <= 257 &&
         header == (0xC0006F05u | ((payload_words - 1) << 16));
}

inline constexpr bool IsValidHeader(uint32_t header, uint32_t payload_words) {
  return ((header == kMaskLowHeader || header == kMaskHighHeader) && payload_words == 1) ||
         IsStateHeader(header, payload_words);
}

inline constexpr bool DecodeRegister(uint32_t operand, uint32_t& first, uint32_t count = 1) {
  if ((operand & 0xFFFFF800u) != 0x40000)
    return false;
  const uint32_t decoded = 0x2000 + (operand & 0x7FF);
  uint32_t header;
  if (!native_gpu_state::EncodeHeader(decoded, count, header))
    return false;
  first = decoded;
  return true;
}

void RecordMask();
void RecordState(bool applied);
uint64_t MaskCount();
uint64_t StateCount();
uint64_t SkippedStateCount();
void ResetCounts();

}  // namespace rex::graphics::native_gpu_predication
