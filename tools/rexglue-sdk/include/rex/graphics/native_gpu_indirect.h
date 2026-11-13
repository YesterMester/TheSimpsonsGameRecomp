#pragma once

#include <array>
#include <cstdint>

namespace rex::graphics::native_gpu_indirect {

// Three raw operands retain their pointer/index/length patch offsets. Source
// bytes are fetched by the command thread, not captured by the game thread.
constexpr uint32_t kOpcode = 0x6C;
constexpr uint32_t kHeader = 0xC0026C00;
constexpr uint32_t kCapability = 1u << 9;
constexpr uint32_t kTypedHeader = 0xC0026C02;
constexpr uint32_t kTypedCapability = 1u << 12;
// A retained command buffer is read by the command thread after publication.
// Keep the ring entry's original address and word-count offsets.
constexpr uint32_t kBufferHeader = 0xC0016C04;
constexpr uint32_t kBufferCapability = 1u << 14;

struct BufferCall {
  uint32_t address, count;
};

inline constexpr bool ValidateBuffer(const BufferCall& call) {
  return !(call.address & 3) && call.address < 0x20000000 && call.count && call.count <= 0xFFFFF &&
         uint64_t(call.address) + uint64_t(call.count) * 4 <= 0x20000000;
}

struct Call {
  uint32_t address, index, count;
};

inline constexpr bool Validate(const Call& call) {
  return !(call.address & 3) && call.address < 0x20000000 && call.count && call.index < 2048 &&
         call.count <= 2048 - call.index &&
         uint64_t(call.address) + uint64_t(call.count) * 4 <= 0x20000000;
}

struct TypedCall {
  uint32_t address, offset_type, size;
};
struct TypedRange {
  uint32_t address, first_register, count;
};

inline constexpr bool DecodeTyped(const TypedCall& call, TypedRange& range) {
  constexpr std::array<uint32_t, 5> bases = {0x4000, 0x4800, 0x4900, 0x4908, 0x2000};
  constexpr std::array<uint32_t, 5> limits = {2048, 192, 8, 32, 1024};
  const uint32_t address = call.address & 0x3FFFFFFFu;
  const uint32_t type = (call.offset_type >> 16) & 255;
  const uint32_t index = call.offset_type & 0x7FF, count = call.size & 0xFFF;
  if ((address & 3) || address >= 0x20000000 || type >= bases.size() || index > limits[type] ||
      count > limits[type] - index || uint64_t(address) + uint64_t(count) * 4 > 0x20000000)
    return false;
  range = {address, bases[type] + index, count};
  return true;
}

inline constexpr bool IsValidHeader(uint32_t header, uint32_t payload_words) {
  return (payload_words == 3 && (header == kHeader || header == kTypedHeader)) ||
         (payload_words == 2 && header == kBufferHeader);
}

void RecordCall(uint32_t words);
void RecordTypedCall(uint32_t words);
void RecordBufferCall(uint32_t words);
uint64_t CallCount();
uint64_t WordCount();
uint64_t TypedCallCount();
uint64_t BufferCallCount();
uint64_t BufferWordCount();
void ResetCounts();

}  // namespace rex::graphics::native_gpu_indirect
