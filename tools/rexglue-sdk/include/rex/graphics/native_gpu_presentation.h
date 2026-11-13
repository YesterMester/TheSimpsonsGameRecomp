#pragma once

#include <array>
#include <cstdint>
#include <span>

namespace rex::graphics::native_gpu_presentation {

constexpr uint32_t kOpcode = 0x68;
constexpr uint32_t kHeader = 0xC0036800u;
constexpr uint32_t kVersion = 0x50525301u;
constexpr uint32_t kCapability = 1u << 15;
constexpr uint32_t kFetchHeader = 0xC0057600u;

struct Call {
  uint32_t address, width, height;
};

inline constexpr bool IsValidHeader(uint32_t header, uint32_t count) {
  return header == kHeader && count == 4;
}

inline bool Decode(std::span<const uint32_t> payload, Call& call) {
  if (payload.size() != 4 || payload[0] != kVersion)
    return false;
  call = {payload[1], payload[2], payload[3]};
  return true;
}

inline bool DimensionsIndependent(const void* output, const void* width, const void* height) {
  const auto overlaps = [first = reinterpret_cast<uintptr_t>(output)](const void* pointer) {
    const uintptr_t address = reinterpret_cast<uintptr_t>(pointer);
    return address >= first ? address - first < 256 : first - address < 4;
  };
  return !overlaps(width) && !overlaps(height);
}

// VdSwap owns a 64-word primary-ring reservation. Keep the fetch payload, swap
// operand positions and padding, including the order of the two dimension
// reads. The output may be the kernel's endian-aware mapped array.
template <typename Output, typename ReadWidth, typename ReadHeight>
void WriteBuffer(Output&& output, const std::array<uint32_t, 6>& fetch, uint32_t address,
                 ReadWidth read_width, ReadHeight read_height, uint32_t legacy_signature,
                 bool native) {
  output[0] = native ? kFetchHeader : 0x00054800u;
  for (uint32_t i = 0; i < fetch.size(); ++i)
    output[i + 1] = fetch[i];
  output[7] = native ? kHeader : 0xC0036400u;
  output[8] = native ? kVersion : legacy_signature;
  output[9] = address;
  output[10] = read_width();
  output[11] = read_height();
  for (uint32_t i = 12; i < 64; ++i)
    output[i] = 0x80000000u;
}

}  // namespace rex::graphics::native_gpu_presentation
