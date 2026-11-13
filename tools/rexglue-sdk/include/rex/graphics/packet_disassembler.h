#pragma once
/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2015 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <vector>

#include <rex/graphics/register_file.h>
#include <rex/graphics/trace_protocol.h>
#include <rex/graphics/trace_reader.h>
#include <rex/memory.h>

namespace rex::graphics {

enum class PacketCategory {
  kGeneric,
  kDraw,
  kSwap,
};

struct PacketTypeInfo {
  PacketCategory category;
  const char* name;
};

struct PacketAction {
  enum class Type {
    kRegisterWrite,
    kSetBinMask,
    kSetBinSelect,
    kSetBinMaskLow,
    kSetBinMaskHigh,
    kSetBinSelectLow,
    kSetBinSelectHigh,
  };
  Type type;

  union {
    struct {
      uint32_t index;
      uint32_t value;
    } register_write;
    struct {
      uint64_t value;
    } set_bin_mask;
    struct {
      uint64_t value;
    } set_bin_select;
  };

  static PacketAction RegisterWrite(uint32_t index, uint32_t value) {
    PacketAction action;
    action.type = Type::kRegisterWrite;
    action.register_write.index = index;
    action.register_write.value = value;
    return action;
  }

  static PacketAction SetBinMask(uint64_t value) {
    PacketAction action;
    action.type = Type::kSetBinMask;
    action.set_bin_mask.value = value;
    return action;
  }

  static PacketAction SetBinSelect(uint64_t value) {
    PacketAction action;
    action.type = Type::kSetBinSelect;
    action.set_bin_select.value = value;
    return action;
  }

  static PacketAction SetBinHalf(Type type, uint32_t value) {
    PacketAction action;
    action.type = type;
    action.set_bin_mask.value = value;
    return action;
  }
};

struct PacketInfo {
  const PacketTypeInfo* type_info = nullptr;
  bool predicated = false;
  uint32_t count = 0;
  std::vector<PacketAction> actions;
};

class PacketDisassembler {
 public:
  // Optional guest memory base (512 MB physical space). When set,
  // memory-indirect packets (PM4_LOAD_ALU_CONSTANT) resolve their register
  // data from it instead of emitting placeholder values.
  static void SetMemoryBase(const uint8_t* membase, uint32_t size);

  static PacketCategory GetPacketCategory(const uint8_t* base_ptr);

  static bool DisasmPacketType0(const uint8_t* base_ptr, uint32_t packet, PacketInfo* out_info);
  static bool DisasmPacketType1(const uint8_t* base_ptr, uint32_t packet, PacketInfo* out_info);
  static bool DisasmPacketType2(const uint8_t* base_ptr, uint32_t packet, PacketInfo* out_info);
  static bool DisasmPacketType3(const uint8_t* base_ptr, uint32_t packet, PacketInfo* out_info);
  static bool DisasmPacket(const uint8_t* base_ptr, PacketInfo* out_info);
};

}  // namespace rex::graphics
