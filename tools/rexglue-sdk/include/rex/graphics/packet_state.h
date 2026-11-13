#pragma once

#include <cstdint>
#include <span>

#include <rex/graphics/native_gpu_calls.h>
#include <rex/graphics/packet_disassembler.h>

namespace rex::graphics {

// Trace tools share the runtime's initial bin state and partial-update rules.
// Applying a rejected vertices/range record still updates its unconditional
// state, but not the contained draw's control or DMA descriptors.
struct PacketState {
  uint64_t bin_mask = 0xFFFFFFFFull, bin_select = 0xFFFFFFFFull;
  struct Result {
    uint32_t register_writes = 0;
    bool executed = false, draw = false;
  };

  Result Apply(const PacketInfo& packet, uint32_t header, std::span<uint32_t> registers) {
    const bool selected = (bin_mask & bin_select) != 0;
    Result result;
    if (packet.predicated && !selected)
      return result;
    result.executed = true;
    const uint32_t opcode = (header >> 8) & 127;
    const bool composite =
        (header >> 30) == 3 && (opcode == native_gpu_calls::kVerticesDrawOpcode ||
                                opcode == native_gpu_calls::kRangeDrawOpcode);
    result.draw = packet.type_info && packet.type_info->category == PacketCategory::kDraw &&
                  (!composite || selected);
    for (const auto& action : packet.actions) {
      using Type = PacketAction::Type;
      switch (action.type) {
        case Type::kRegisterWrite: {
          const auto& reg = action.register_write;
          if (reg.index < registers.size() &&
              !(composite && !selected && reg.index >= 0x21FA && reg.index <= 0x21FC)) {
            registers[reg.index] = reg.value;
            ++result.register_writes;
          }
          break;
        }
        case Type::kSetBinMask:
          bin_mask = action.set_bin_mask.value;
          break;
        case Type::kSetBinSelect:
          bin_select = action.set_bin_select.value;
          break;
        case Type::kSetBinMaskLow:
          bin_mask = (bin_mask & 0xFFFFFFFF00000000ull) | uint32_t(action.set_bin_mask.value);
          break;
        case Type::kSetBinMaskHigh:
          bin_mask =
              (bin_mask & 0xFFFFFFFFull) | (uint64_t(uint32_t(action.set_bin_mask.value)) << 32);
          break;
        case Type::kSetBinSelectLow:
          bin_select = (bin_select & 0xFFFFFFFF00000000ull) | uint32_t(action.set_bin_mask.value);
          break;
        case Type::kSetBinSelectHigh:
          bin_select =
              (bin_select & 0xFFFFFFFFull) | (uint64_t(uint32_t(action.set_bin_mask.value)) << 32);
          break;
      }
    }
    return result;
  }
};

}  // namespace rex::graphics
