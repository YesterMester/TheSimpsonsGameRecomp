#pragma once

#include <array>
#include <cstdint>
#include <span>

namespace rex::graphics::native_gpu_coherency {

// Sampler flushes retain their original 13-word span and operand offsets.
// Only the old state/wait headers become versioned metadata. The fixed wait
// means register equality on COHER_STATUS_HOST's dirty bit, never guest memory.
constexpr uint32_t kOpcode = 0x71;
constexpr uint32_t kHeader = 0xC00B7100;
constexpr uint32_t kVersion = 0x434F4801;  // COH, layout version 1.
constexpr uint32_t kCapability = 1u << 4;

struct Call {
  uint32_t destination_base, status, size, base;
};

inline std::array<uint32_t, 13> Encode(const Call& call) {
  return {
      kHeader, call.destination_base, kVersion, call.status, 0, call.size, call.base, 0, 3, 0xA31,
      0,       0x80000000u,           8};
}

inline bool Decode(std::span<const uint32_t> payload, Call& call) {
  if (payload.size() != 12 || payload[1] != kVersion || payload[3] || payload[6] ||
      payload[7] != 3 || payload[8] != 0xA31 || payload[9] || payload[10] != 0x80000000u ||
      payload[11] != 8)
    return false;
  call = {payload[0], payload[2], payload[4], payload[5]};
  return true;
}

void RecordCall();
uint64_t CallCount();
void ResetCounts();

}  // namespace rex::graphics::native_gpu_coherency
