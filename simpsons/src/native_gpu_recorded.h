#pragma once

#include <rex/graphics/native_gpu_constants.h>
#include <rex/graphics/native_gpu_direct.h>
#include <rex/graphics/native_gpu_indirect.h>
#include <rex/graphics/native_gpu_ports.h>
#include <rex/graphics/native_gpu_predication.h>
#include <rex/graphics/native_gpu_program.h>
#include <rex/graphics/native_gpu_shader_reference.h>
#include <rex/graphics/native_gpu_state.h>
#include <rex/graphics/native_gpu_sync.h>

namespace simpsons::native_gpu_calls {

// The original buffer iterator is also the game's address/patch-table editor.
// Give it a logical view of one-to-one native records without changing a byte
// in the command buffer. Payload pointers, lengths, predicates and patch offsets
// still belong to that buffer; execution continues through the native consumer.
// Compound records need a separate editing contract and stay opaque here.
inline constexpr uint32_t RecordedLogicalHeader(uint32_t header) {
  if ((header >> 30) != 3)
    return header;
  using namespace rex::graphics;
  const uint32_t count = ((header >> 16) & 0x3FFF) + 1;
  const uint32_t opcode = (header >> 8) & 127;
  const uint32_t length = (count - 1) << 16;
  if (opcode == native_gpu_direct::kOpcode && native_gpu_direct::IsValidHeader(header, count))
    return 0xC0000000u | length | ((header & 2) ? 0x3600 : 0x2200) | (header & 1);
  if (opcode == native_gpu_shader_reference::kOpcode &&
      native_gpu_shader_reference::IsValidHeader(header, count))
    return 0xC0012700u | (header & 1);
  if (header == native_gpu_indirect::kBufferHeader)
    return 0xC0013F00u;
  if (opcode == native_gpu_indirect::kOpcode && native_gpu_indirect::IsValidHeader(header, count))
    return 0xC0022F00u;
  if (header == 0xC0007B00u || header == 0xC0007B01u)
    return 0xC0003600u | (header & 1);
  if (opcode == native_gpu_constants::kOpcode) {
    native_gpu_constants::ConstantRange range{};
    if (native_gpu_constants::DecodeHeader(header, count, range))
      return length | range.first_register;
  }
  if (native_gpu_state::IsOpcode(opcode)) {
    native_gpu_state::StateRange range{};
    if (native_gpu_state::DecodeHeader(header, count, range)) {
      // This iterator groups fetch descriptors in sixes and the 0x5000 flush
      // in threes. A partial descriptor is not one of its supported views.
      if ((range.first_register >= 0x4800 && range.first_register < 0x48C0 &&
           ((range.first_register - 0x4800) % 6 || count % 6)) ||
          (range.first_register == 0x5000 && count != 3))
        return header;
      return length | range.first_register;
    }
  }
  if (opcode == native_gpu_program::kOpcode) {
    if (native_gpu_program::IsShaderHeader(header, count))
      return 0xC0002B00u | length;
    if (header == native_gpu_program::kInvalidateHeader)
      return 0xC0003B00u;
  }
  if (opcode == native_gpu_predication::kOpcode &&
      native_gpu_predication::IsValidHeader(header, count)) {
    if (header == native_gpu_predication::kMaskLowHeader)
      return 0xC0006000u;
    if (header == native_gpu_predication::kMaskHighHeader)
      return 0xC0006100u;
    return 0xC0002D01u | length;
  }
  if (opcode == native_gpu_ports::kOpcode) {
    uint32_t reg{};
    if (native_gpu_ports::DecodeHeader(header, count, reg))
      return length | reg | (count == 8 ? 0x8000 : 0);
  }
  if (header == native_gpu_sync::kEventHeader)
    return 0xC0025800u;
  return header;
}

}  // namespace simpsons::native_gpu_calls
