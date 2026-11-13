#pragma once

#include <cstdint>

namespace rex::graphics::native_gpu_shader_reference {

constexpr uint32_t kOpcode = 0x6B;
constexpr uint32_t kHeader = 0xC0016B00;
constexpr uint32_t kCapability = 1u << 10;

struct Call {
  uint32_t address_type, words;
};

inline constexpr bool IsValidHeader(uint32_t header, uint32_t count) {
  return count == 2 && (header == kHeader || header == (kHeader | 1));
}

inline constexpr bool Validate(const Call& call) {
  const uint32_t address = call.address_type & ~3u;
  return (call.address_type & 3) <= 1 && address < 0x20000000 && call.words &&
         call.words <= 0xFFFF && uint64_t(address) + uint64_t(call.words) * 4 <= 0x20000000;
}

void RecordCall();
void RecordSkip();
uint64_t CallCount();
uint64_t SkipCount();
void ResetCounts();

}  // namespace rex::graphics::native_gpu_shader_reference
