#pragma once

#include <cstdint>

namespace rex::graphics::native_gpu_program {

// Inline bindings retain the stage, size and code offsets of their source
// buffer. The low-byte tag is unpredicated, so a skipped draw still binds its
// shader. Invalidation retains the mask and the existing backend semantics.
constexpr uint32_t kOpcode = 0x70;
constexpr uint32_t kShaderTag = 0xC0007002;
constexpr uint32_t kInvalidateHeader = 0xC0007004;
constexpr uint32_t kCapability = 1u << 5;

inline constexpr bool EncodeShaderHeader(uint32_t code_words, uint32_t& header) {
  if (!code_words || code_words > 16382)
    return false;
  header = kShaderTag | ((code_words + 1) << 16);
  return true;
}

inline constexpr bool IsShaderHeader(uint32_t header, uint32_t payload_words) {
  uint32_t canonical;
  return payload_words >= 3 && EncodeShaderHeader(payload_words - 2, canonical) &&
         header == canonical;
}

inline constexpr bool IsValidHeader(uint32_t header, uint32_t payload_words) {
  return IsShaderHeader(header, payload_words) ||
         (header == kInvalidateHeader && payload_words == 1);
}

inline constexpr bool ValidateShader(uint32_t stage, uint32_t start_size, uint32_t payload_words) {
  return stage <= 1 && !(start_size >> 16) && start_size && start_size <= 16382 &&
         payload_words == start_size + 2;
}

void RecordShader();
void RecordInvalidation();
uint64_t ShaderCount();
uint64_t InvalidationCount();
void ResetCounts();

}  // namespace rex::graphics::native_gpu_program
