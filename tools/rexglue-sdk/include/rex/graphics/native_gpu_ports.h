#pragma once

#include <cstdint>

namespace rex::graphics::native_gpu_ports {

// Initialization writes WAIT_UNTIL once, then the same GPR-management port
// eight times and once more. Keep every ordered callback and raw operand.
constexpr uint32_t kOpcode = 0x6E;
constexpr uint32_t kWaitHeader = 0xC0006E00;
constexpr uint32_t kGprHeader = 0xC0006E02;
constexpr uint32_t kGprEightHeader = 0xC0076E02;
constexpr uint32_t kCapability = 1u << 7;

inline constexpr bool DecodeHeader(uint32_t header, uint32_t payload_words, uint32_t& reg) {
  if (header == kWaitHeader && payload_words == 1) {
    reg = 0x5C8;
    return true;
  }
  if ((header == kGprHeader && payload_words == 1) ||
      (header == kGprEightHeader && payload_words == 8)) {
    reg = 0xD00;
    return true;
  }
  return false;
}

void RecordCall(uint32_t writes);
uint64_t CallCount();
uint64_t WriteCount();
void ResetCounts();

}  // namespace rex::graphics::native_gpu_ports
