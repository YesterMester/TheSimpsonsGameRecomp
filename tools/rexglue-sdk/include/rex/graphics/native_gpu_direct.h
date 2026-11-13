#pragma once

#include <cstdint>
#include <span>

namespace rex::graphics::native_gpu_direct {

// Direct arguments retain the original draw's word count and patch offsets.
// Bit 1 omits the otherwise-present query operand; bit 0 predicates the whole call.
constexpr uint32_t kOpcode = 0x6A;
constexpr uint32_t kCapability = 1u << 13;

inline constexpr bool IsValidHeader(uint32_t header, uint32_t count) {
  const bool query = !(header & 2);
  return (count == (query ? 2u : 1u) || count == (query ? 4u : 3u)) &&
         header == (0xC0000000u | ((count - 1) << 16) | (kOpcode << 8) | (header & 3));
}

inline constexpr bool EncodeLegacyHeader(uint32_t legacy, uint32_t& header) {
  const uint32_t opcode = (legacy >> 8) & 127, count = ((legacy >> 16) & 0x3FFF) + 1;
  if ((opcode != 0x22 && opcode != 0x36) ||
      legacy != (0xC0000000u | ((count - 1) << 16) | (opcode << 8) | (legacy & 1)))
    return false;
  const uint32_t result =
      0xC0000000u | ((count - 1) << 16) | (kOpcode << 8) | (opcode == 0x36 ? 2 : 0) | (legacy & 1);
  if (!IsValidHeader(result, count))
    return false;
  header = result;
  return true;
}

struct Call {
  uint32_t query, control, index_base, index_size;
};
inline bool Decode(uint32_t header, std::span<const uint32_t> payload, Call& call) {
  if (!IsValidHeader(header, uint32_t(payload.size())))
    return false;
  const uint32_t prefix = (header & 2) ? 0 : 1;
  const uint32_t control = payload[prefix], source = (control >> 6) & 3;
  if ((source != 0 && source != 2) || (source == 0 && payload.size() - prefix < 3))
    return false;
  call = {prefix ? payload[0] : 0, control, source == 0 ? payload[prefix + 1] : 0,
          source == 0 ? payload[prefix + 2] : 0};
  return true;
}

void RecordCall();
void RecordSkip();
uint64_t CallCount();
uint64_t SkipCount();
void ResetCounts();

}  // namespace rex::graphics::native_gpu_direct
