#pragma once

#include <array>
#include <cstdint>
#include <span>

namespace rex::graphics::native_gpu_sync {

// Retirement buffers do not update COHER_DEST_BASE_1. Keep their shorter
// coherency span and the following shader-event operands at their old offsets.
constexpr uint32_t kOpcode = 0x6D;
constexpr uint32_t kCoherencyHeader = 0xC0096D00;
constexpr uint32_t kEventHeader = 0xC0026D02;
constexpr uint32_t kVersion = 0x53594E01;
constexpr uint32_t kCapability = 1u << 8;

struct Coherency {
  uint32_t status, size, base;
};

inline std::array<uint32_t, 11> Encode(const Coherency& call) {
  return {kCoherencyHeader,
          call.status,
          kVersion,
          call.size,
          call.base,
          0,
          3,
          0xA31,
          0,
          0x80000000u,
          8};
}

inline bool Decode(std::span<const uint32_t> payload, Coherency& call) {
  if (payload.size() != 10 || payload[1] != kVersion || payload[4] || payload[5] != 3 ||
      payload[6] != 0xA31 || payload[7] || payload[8] != 0x80000000u || payload[9] != 8)
    return false;
  call = {payload[0], payload[2], payload[3]};
  return true;
}

inline constexpr bool IsValidHeader(uint32_t header, uint32_t payload_words) {
  return (header == kCoherencyHeader && payload_words == 10) ||
         (header == kEventHeader && payload_words == 3);
}

void RecordCoherency();
void RecordEvent();
uint64_t CoherencyCount();
uint64_t EventCount();
void ResetCounts();

}  // namespace rex::graphics::native_gpu_sync
