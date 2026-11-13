#pragma once

#include <array>
#include <cstdint>

namespace rex::graphics::draw_util {

// Private Vulkan fetch constants have an aligned snapshot byte offset, a valid
// flag, a signed 31-bit original word offset and the original endian mode.
// Every physically valid fetch range fits this signed offset: the original
// base is at most 30 bits in words, and guest physical memory is 512 MiB.
// This replaces size-field rebasing without padding a compact buffer up to a
// large or negative original index. Guest fetch constants are never changed.
inline constexpr uint32_t kNativeVertexFetchValidBit = 1;
inline constexpr uint32_t kNativeVertexFetchOffsetHighBit = 2;
inline constexpr uint32_t kNativeVertexFetchOffsetBits = 31;

constexpr std::array<uint32_t, 2> PackNativeVertexFetch(uint32_t snapshot_offset_bytes,
                                                        int64_t first_word, uint32_t endian) {
  const uint32_t bits = uint32_t(first_word);
  return {snapshot_offset_bytes | kNativeVertexFetchValidBit |
              ((bits >> 29) & kNativeVertexFetchOffsetHighBit),
          (bits << 2) | (endian & 3u)};
}

constexpr int32_t UnpackNativeVertexFetchOffset(const std::array<uint32_t, 2>& fetch) {
  const uint32_t bits = (fetch[1] >> 2) | ((fetch[0] & kNativeVertexFetchOffsetHighBit) << 29);
  return int32_t(int64_t(bits) - ((bits & (1u << 30)) ? (int64_t(1) << 31) : 0));
}

}  // namespace rex::graphics::draw_util
