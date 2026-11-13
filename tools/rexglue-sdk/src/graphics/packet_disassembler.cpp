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

#include <cstdio>
#include <cstdlib>
#include <rex/graphics/packet_disassembler.h>
#include <rex/graphics/native_gpu_calls.h>
#include <rex/graphics/native_gpu_constants.h>
#include <rex/graphics/native_gpu_state.h>
#include <rex/graphics/native_gpu_coherency.h>
#include <rex/graphics/native_gpu_program.h>
#include <rex/graphics/native_gpu_predication.h>
#include <rex/graphics/native_gpu_ports.h>
#include <rex/graphics/native_gpu_sync.h>
#include <rex/graphics/native_gpu_indirect.h>
#include <rex/graphics/native_gpu_shader_reference.h>
#include <rex/graphics/native_gpu_direct.h>
#include <rex/graphics/native_gpu_presentation.h>
#include <rex/graphics/xenos.h>

namespace rex::graphics {

using namespace rex::graphics::xenos;

namespace {
const uint8_t* disasm_membase = nullptr;
uint32_t disasm_memsize = 0;
}  // namespace

void PacketDisassembler::SetMemoryBase(const uint8_t* membase, uint32_t size) {
  disasm_membase = membase;
  disasm_memsize = size;
}

PacketCategory PacketDisassembler::GetPacketCategory(const uint8_t* base_ptr) {
  const uint32_t packet = memory::load_and_swap<uint32_t>(base_ptr);
  const uint32_t packet_type = packet >> 30;
  switch (packet_type) {
    case 0x00:
    case 0x01:
    case 0x02: {
      return PacketCategory::kGeneric;
    }
    case 0x03: {
      uint32_t opcode = (packet >> 8) & 0x7F;
      switch (opcode) {
        case PM4_DRAW_INDX:
        case PM4_DRAW_INDX_2:
        case native_gpu_calls::kOpcode:
        case native_gpu_calls::kBareDrawOpcode:
        case native_gpu_direct::kOpcode:
        case native_gpu_calls::kVerticesDrawOpcode:
        case native_gpu_calls::kRangeDrawOpcode:
          return PacketCategory::kDraw;
        case PM4_XE_SWAP:
        case native_gpu_presentation::kOpcode:
          return PacketCategory::kSwap;
        default:
          return PacketCategory::kGeneric;
      }
    }
    default: {
      assert_unhandled_case(packet_type);
      return PacketCategory::kGeneric;
    }
  }
}

bool PacketDisassembler::DisasmPacketType0(const uint8_t* base_ptr, uint32_t packet,
                                           PacketInfo* out_info) {
  static const PacketTypeInfo type_0_info = {PacketCategory::kGeneric, "PM4_TYPE0"};
  out_info->type_info = &type_0_info;

  uint32_t count = ((packet >> 16) & 0x3FFF) + 1;
  out_info->count = 1 + count;
  auto ptr = base_ptr + 4;

  uint32_t base_index = (packet & 0x7FFF);
  uint32_t write_one_reg = (packet >> 15) & 0x1;
  for (uint32_t m = 0; m < count; m++) {
    uint32_t reg_data = memory::load_and_swap<uint32_t>(ptr);
    uint32_t target_index = write_one_reg ? base_index : base_index + m;
    out_info->actions.emplace_back(PacketAction::RegisterWrite(target_index, reg_data));
    ptr += 4;
  }

  return true;
}

bool PacketDisassembler::DisasmPacketType1(const uint8_t* base_ptr, uint32_t packet,
                                           PacketInfo* out_info) {
  static const PacketTypeInfo type_1_info = {PacketCategory::kGeneric, "PM4_TYPE1"};
  out_info->type_info = &type_1_info;

  out_info->count = 1 + 2;
  auto ptr = base_ptr + 4;

  uint32_t reg_index_1 = packet & 0x7FF;
  uint32_t reg_index_2 = (packet >> 11) & 0x7FF;
  uint32_t reg_data_1 = memory::load_and_swap<uint32_t>(ptr);
  uint32_t reg_data_2 = memory::load_and_swap<uint32_t>(ptr + 4);
  out_info->actions.emplace_back(PacketAction::RegisterWrite(reg_index_1, reg_data_1));
  out_info->actions.emplace_back(PacketAction::RegisterWrite(reg_index_2, reg_data_2));

  return true;
}

bool PacketDisassembler::DisasmPacketType2(const uint8_t* base_ptr, uint32_t packet,
                                           PacketInfo* out_info) {
  static const PacketTypeInfo type_2_info = {PacketCategory::kGeneric, "PM4_TYPE2"};
  out_info->type_info = &type_2_info;

  out_info->count = 1;

  return true;
}

bool PacketDisassembler::DisasmPacketType3(const uint8_t* base_ptr, uint32_t packet,
                                           PacketInfo* out_info) {
  static const PacketTypeInfo type_3_unknown_info = {PacketCategory::kGeneric, "PM4_TYPE3_UNKNOWN"};
  out_info->type_info = &type_3_unknown_info;

  uint32_t opcode = (packet >> 8) & 0x7F;
  uint32_t count = ((packet >> 16) & 0x3FFF) + 1;
  out_info->count = 1 + count;
  auto ptr = base_ptr + 4;

  if (packet & 1) {
    out_info->predicated = true;
  }

  bool result = true;
  switch (opcode) {
    case native_gpu_direct::kOpcode: {
      if (!native_gpu_direct::IsValidHeader(packet, count))
        return false;
      std::array<uint32_t, 4> payload{};
      for (uint32_t i = 0; i < count; ++i)
        payload[i] = memory::load_and_swap<uint32_t>(ptr + i * 4);
      native_gpu_direct::Call call;
      if (!native_gpu_direct::Decode(packet, {payload.data(), count}, call))
        return false;
      static const PacketTypeInfo info = {PacketCategory::kDraw, "NATIVE_DIRECT_DRAW"};
      out_info->type_info = &info;
      out_info->actions.emplace_back(
          PacketAction::RegisterWrite(XE_GPU_REG_VGT_DRAW_INITIATOR, call.control));
      if (((call.control >> 6) & 3) == 0) {
        out_info->actions.emplace_back(
            PacketAction::RegisterWrite(XE_GPU_REG_VGT_DMA_BASE, call.index_base));
        out_info->actions.emplace_back(
            PacketAction::RegisterWrite(XE_GPU_REG_VGT_DMA_SIZE, call.index_size));
      }
      return true;
    }
    case native_gpu_shader_reference::kOpcode: {
      if (!native_gpu_shader_reference::IsValidHeader(packet, count) ||
          !native_gpu_shader_reference::Validate(
              {memory::load_and_swap<uint32_t>(ptr), memory::load_and_swap<uint32_t>(ptr + 4)}))
        return false;
      static const PacketTypeInfo info = {PacketCategory::kGeneric, "NATIVE_SHADER_REFERENCE"};
      out_info->type_info = &info;
      return true;
    }
    case native_gpu_indirect::kOpcode: {
      if (!native_gpu_indirect::IsValidHeader(packet, count))
        return false;
      if (packet == native_gpu_indirect::kBufferHeader) {
        const native_gpu_indirect::BufferCall call{memory::load_and_swap<uint32_t>(ptr),
                                                  memory::load_and_swap<uint32_t>(ptr + 4)};
        if (!native_gpu_indirect::ValidateBuffer(call))
          return false;
        static const PacketTypeInfo info = {PacketCategory::kGeneric, "NATIVE_COMMAND_BUFFER"};
        out_info->type_info = &info;
        return true;
      }
      const native_gpu_indirect::Call call{memory::load_and_swap<uint32_t>(ptr),
                                           memory::load_and_swap<uint32_t>(ptr + 4),
                                           memory::load_and_swap<uint32_t>(ptr + 8)};
      native_gpu_indirect::TypedRange range{call.address, 0x4000 + call.index, call.count};
      const bool typed = packet == native_gpu_indirect::kTypedHeader;
      if (typed ? !native_gpu_indirect::DecodeTyped({call.address, call.index, call.count}, range)
                : !native_gpu_indirect::Validate(call))
        return false;
      static const PacketTypeInfo info = {PacketCategory::kGeneric,
                                          "NATIVE_INDIRECT_ALU_CONSTANTS"};
      static const PacketTypeInfo typed_info = {PacketCategory::kGeneric,
                                                "NATIVE_INDIRECT_TYPED_CONSTANTS"};
      out_info->type_info = typed ? &typed_info : &info;
      for (uint32_t i = 0; i < range.count; ++i) {
        uint32_t value = 0xDEADBEEF;
        if (disasm_membase) {
          const uint32_t source = (range.address + i * 4) & (disasm_memsize - 1);
          if (uint64_t(source) + 4 <= disasm_memsize)
            value = memory::load_and_swap<uint32_t>(disasm_membase + source);
        }
        out_info->actions.emplace_back(
            PacketAction::RegisterWrite(range.first_register + i, value));
      }
      return true;
    }
    case native_gpu_sync::kOpcode: {
      if (!native_gpu_sync::IsValidHeader(packet, count))
        return false;
      if (packet == native_gpu_sync::kEventHeader) {
        static const PacketTypeInfo info = {PacketCategory::kGeneric, "NATIVE_SHADER_EVENT"};
        out_info->type_info = &info;
        out_info->actions.emplace_back(PacketAction::RegisterWrite(
            XE_GPU_REG_VGT_EVENT_INITIATOR, memory::load_and_swap<uint32_t>(ptr) & 0x3F));
      } else {
        std::array<uint32_t, 10> payload;
        for (uint32_t i = 0; i < payload.size(); ++i)
          payload[i] = memory::load_and_swap<uint32_t>(ptr + i * 4);
        native_gpu_sync::Coherency call;
        if (!native_gpu_sync::Decode(payload, call))
          return false;
        static const PacketTypeInfo info = {PacketCategory::kGeneric,
                                            "NATIVE_RETIREMENT_COHERENCY"};
        out_info->type_info = &info;
        out_info->actions.emplace_back(
            PacketAction::RegisterWrite(XE_GPU_REG_COHER_STATUS_HOST, 0));
        out_info->actions.emplace_back(
            PacketAction::RegisterWrite(XE_GPU_REG_COHER_SIZE_HOST, call.size));
        out_info->actions.emplace_back(
            PacketAction::RegisterWrite(XE_GPU_REG_COHER_BASE_HOST, call.base));
      }
      return true;
    }
    case native_gpu_ports::kOpcode: {
      uint32_t reg;
      if (!native_gpu_ports::DecodeHeader(packet, count, reg))
        return false;
      static const PacketTypeInfo info = {PacketCategory::kGeneric, "NATIVE_INITIALIZATION_PORT"};
      out_info->type_info = &info;
      for (uint32_t i = 0; i < count; ++i)
        out_info->actions.emplace_back(
            PacketAction::RegisterWrite(reg, memory::load_and_swap<uint32_t>(ptr + i * 4)));
      return true;
    }
    case native_gpu_predication::kOpcode: {
      if (!native_gpu_predication::IsValidHeader(packet, count))
        return false;
      if (native_gpu_predication::IsStateHeader(packet, count)) {
        uint32_t first;
        if (!native_gpu_predication::DecodeRegister(memory::load_and_swap<uint32_t>(ptr), first,
                                                    count - 1))
          return false;
        static const PacketTypeInfo info = {PacketCategory::kGeneric, "NATIVE_CONDITIONAL_STATE"};
        out_info->type_info = &info;
        for (uint32_t i = 0; i < count - 1; ++i)
          out_info->actions.emplace_back(PacketAction::RegisterWrite(
              first + i, memory::load_and_swap<uint32_t>(ptr + 4 + i * 4)));
      } else {
        const bool high = packet == native_gpu_predication::kMaskHighHeader;
        static const PacketTypeInfo low_info = {PacketCategory::kGeneric, "NATIVE_BIN_MASK_LOW"};
        static const PacketTypeInfo high_info = {PacketCategory::kGeneric, "NATIVE_BIN_MASK_HIGH"};
        out_info->type_info = high ? &high_info : &low_info;
        out_info->actions.emplace_back(PacketAction::SetBinHalf(
            high ? PacketAction::Type::kSetBinMaskHigh : PacketAction::Type::kSetBinMaskLow,
            memory::load_and_swap<uint32_t>(ptr)));
      }
      return true;
    }
    case native_gpu_program::kOpcode: {
      if (!native_gpu_program::IsValidHeader(packet, count))
        return false;
      if (packet == native_gpu_program::kInvalidateHeader) {
        static const PacketTypeInfo info = {PacketCategory::kGeneric, "NATIVE_STATE_INVALIDATE"};
        out_info->type_info = &info;
      } else {
        const uint32_t stage = memory::load_and_swap<uint32_t>(ptr);
        const uint32_t start_size = memory::load_and_swap<uint32_t>(ptr + 4);
        if (!native_gpu_program::ValidateShader(stage, start_size, count))
          return false;
        static const PacketTypeInfo info = {PacketCategory::kGeneric, "NATIVE_SHADER_BIND"};
        out_info->type_info = &info;
      }
      return true;
    }
    case native_gpu_coherency::kOpcode: {
      if (packet != native_gpu_coherency::kHeader)
        return false;
      std::array<uint32_t, 12> payload;
      for (uint32_t i = 0; i < payload.size(); ++i)
        payload[i] = memory::load_and_swap<uint32_t>(ptr + i * 4);
      native_gpu_coherency::Call call;
      if (!native_gpu_coherency::Decode(payload, call))
        return false;
      static const PacketTypeInfo info = {PacketCategory::kGeneric, "NATIVE_COHERENCY"};
      out_info->type_info = &info;
      out_info->actions.emplace_back(
          PacketAction::RegisterWrite(XE_GPU_REG_COHER_DEST_BASE_1, call.destination_base));
      out_info->actions.emplace_back(PacketAction::RegisterWrite(XE_GPU_REG_COHER_STATUS_HOST, 0));
      out_info->actions.emplace_back(
          PacketAction::RegisterWrite(XE_GPU_REG_COHER_SIZE_HOST, call.size));
      out_info->actions.emplace_back(
          PacketAction::RegisterWrite(XE_GPU_REG_COHER_BASE_HOST, call.base));
      return true;
    }
    case 0x72:
    case 0x73:
    case 0x74:
    case 0x75:
    case 0x76:
    case 0x77:
    case 0x78: {
      native_gpu_state::StateRange range;
      if (!native_gpu_state::DecodeHeader(packet, count, range))
        return false;
      static const PacketTypeInfo info = {PacketCategory::kGeneric, "NATIVE_STATE_RANGE"};
      out_info->type_info = &info;
      for (uint32_t i = 0; i < count; ++i)
        out_info->actions.emplace_back(PacketAction::RegisterWrite(
            range.first_register + i, memory::load_and_swap<uint32_t>(ptr + i * 4)));
      return true;
    }
    case native_gpu_calls::kVerticesDrawOpcode: {
      if ((count != 15 && count != 17) || (packet & 0x80FFu))
        return false;
      std::array<uint32_t, 17> payload;
      for (uint32_t i = 0; i < count; ++i)
        payload[i] = memory::load_and_swap<uint32_t>(ptr + i * 4);
      native_gpu_calls::VerticesDrawCall call;
      if (!native_gpu_calls::DecodeVertices({payload.data(), count}, call))
        return false;
      static const PacketTypeInfo info = {PacketCategory::kDraw, "NATIVE_VERTICES_DRAW"};
      out_info->type_info = &info;
      // Keep the packet unpredicated: only its contained draw uses the bin
      // condition. The preceding state writes always apply.
      out_info->actions.emplace_back(
          PacketAction::RegisterWrite(XE_GPU_REG_VGT_INDX_OFFSET, call.first_vertex));
      for (uint32_t i = 0; i < call.fetch.size(); ++i)
        out_info->actions.emplace_back(PacketAction::RegisterWrite(0x48BA + i, call.fetch[i]));
      for (uint32_t i = 0; i < call.constant_flush.size(); ++i)
        out_info->actions.emplace_back(PacketAction::RegisterWrite(0x5000 + i, call.constant_flush[i]));
      out_info->actions.emplace_back(
          PacketAction::RegisterWrite(XE_GPU_REG_VGT_DRAW_INITIATOR, call.initiator));
      if (((call.initiator >> 6) & 3) == 0) {
        out_info->actions.emplace_back(
            PacketAction::RegisterWrite(XE_GPU_REG_VGT_DMA_BASE, call.index_base));
        out_info->actions.emplace_back(
            PacketAction::RegisterWrite(XE_GPU_REG_VGT_DMA_SIZE, call.index_size));
      }
      return true;
    }
    case native_gpu_calls::kRangeDrawOpcode: {
      if ((count != 4 && count != 6) || (packet & 0x80FFu))
        return false;
      std::array<uint32_t, 6> payload;
      for (uint32_t i = 0; i < count; ++i)
        payload[i] = memory::load_and_swap<uint32_t>(ptr + i * 4);
      native_gpu_calls::RangeDrawCall call;
      if (!native_gpu_calls::DecodeRange({payload.data(), count}, call))
        return false;
      static const PacketTypeInfo info = {PacketCategory::kDraw, "NATIVE_RANGE_DRAW"};
      out_info->type_info = &info;
      out_info->actions.emplace_back(
          PacketAction::RegisterWrite(XE_GPU_REG_VGT_INDX_OFFSET, call.first_vertex));
      out_info->actions.emplace_back(
          PacketAction::RegisterWrite(XE_GPU_REG_VGT_DRAW_INITIATOR, call.initiator));
      if (((call.initiator >> 6) & 3) == 0) {
        out_info->actions.emplace_back(
            PacketAction::RegisterWrite(XE_GPU_REG_VGT_DMA_BASE, call.index_base));
        out_info->actions.emplace_back(
            PacketAction::RegisterWrite(XE_GPU_REG_VGT_DMA_SIZE, call.index_size));
      }
      return true;
    }
    case native_gpu_calls::kOpcode: {
      if (count != 23 && count != 26)
        return false;
      std::array<uint32_t, 26> payload;
      for (uint32_t i = 0; i < count; ++i)
        payload[i] = memory::load_and_swap<uint32_t>(ptr + i * 4);
      native_gpu_calls::DrawCall call;
      if (!native_gpu_calls::DecodeDraw({payload.data(), count}, call))
        return false;
      static const PacketTypeInfo info = {PacketCategory::kDraw, "NATIVE_DRAW"};
      out_info->type_info = &info;
      for (uint32_t i = 0; i < call.fetch.size(); ++i)
        out_info->actions.emplace_back(PacketAction::RegisterWrite(0x48BA + i, call.fetch[i]));
      out_info->actions.emplace_back(PacketAction::RegisterWrite(XE_GPU_REG_COHER_STATUS_HOST, 0));
      out_info->actions.emplace_back(
          PacketAction::RegisterWrite(XE_GPU_REG_COHER_SIZE_HOST, call.coherent_size));
      out_info->actions.emplace_back(
          PacketAction::RegisterWrite(XE_GPU_REG_COHER_BASE_HOST, call.coherent_base));
      for (uint32_t i = 0; i < call.constant_flush.size(); ++i)
        out_info->actions.emplace_back(
            PacketAction::RegisterWrite(0x5000 + i, call.constant_flush[i]));
      out_info->actions.emplace_back(
          PacketAction::RegisterWrite(XE_GPU_REG_VGT_DRAW_INITIATOR, call.initiator));
      if (((call.initiator >> 6) & 3) == 0) {
        out_info->actions.emplace_back(
            PacketAction::RegisterWrite(XE_GPU_REG_VGT_DMA_BASE, call.index_base));
        out_info->actions.emplace_back(
            PacketAction::RegisterWrite(XE_GPU_REG_VGT_DMA_SIZE, call.index_size));
      }
      return true;
    }
    case native_gpu_calls::kBareDrawOpcode: {
      if (count != 1)
        return false;
      uint32_t control = memory::load_and_swap<uint32_t>(ptr);
      if (((control >> 6) & 3) != 2)
        return false;
      static const PacketTypeInfo info = {PacketCategory::kDraw, "NATIVE_BARE_DRAW"};
      out_info->type_info = &info;
      out_info->actions.emplace_back(PacketAction::RegisterWrite(XE_GPU_REG_VGT_DRAW_INITIATOR, control));
      return true;
    }
    case native_gpu_constants::kOpcode: {
      native_gpu_constants::ConstantRange range;
      if (!native_gpu_constants::DecodeHeader(packet, count, range))
        return false;
      static const PacketTypeInfo info = {PacketCategory::kGeneric, "NATIVE_SHADER_CONSTANTS"};
      out_info->type_info = &info;
      for (uint32_t i = 0; i < range.register_count; ++i)
        out_info->actions.emplace_back(PacketAction::RegisterWrite(
            range.first_register + i, memory::load_and_swap<uint32_t>(ptr + i * 4)));
      return true;
    }
    case PM4_ME_INIT: {
      // initialize CP's micro-engine
      static const PacketTypeInfo op_info = {PacketCategory::kGeneric, "PM4_ME_INIT"};
      out_info->type_info = &op_info;
      break;
    }
    case PM4_NOP: {
      // skip N 32-bit words to get to the next packet
      // No-op, ignore some data.
      static const PacketTypeInfo op_info = {PacketCategory::kGeneric, "PM4_NOP"};
      out_info->type_info = &op_info;
      break;
    }
    case PM4_INTERRUPT: {
      // generate interrupt from the command stream
      static const PacketTypeInfo op_info = {PacketCategory::kGeneric, "PM4_INTERRUPT"};
      out_info->type_info = &op_info;
      uint32_t cpu_mask = memory::load_and_swap<uint32_t>(ptr + 0);
      for (int n = 0; n < 6; n++) {
        if (cpu_mask & (1 << n)) {
          // graphics_system_->DispatchInterruptCallback(1, n);
        }
      }
      break;
    }
    case native_gpu_presentation::kOpcode: {
      if (!native_gpu_presentation::IsValidHeader(packet, count))
        return false;
      std::array<uint32_t, 4> payload;
      for (uint32_t i = 0; i < payload.size(); ++i)
        payload[i] = memory::load_and_swap<uint32_t>(ptr + i * 4);
      native_gpu_presentation::Call call;
      if (!native_gpu_presentation::Decode(payload, call))
        return false;
      static const PacketTypeInfo info = {PacketCategory::kSwap, "NATIVE_PRESENT"};
      out_info->type_info = &info;
      break;
    }
    case PM4_XE_SWAP: {
      // Xenia-specific VdSwap hook.
      // VdSwap will post this to tell us we need to swap the screen/fire an
      // interrupt.
      // 63 words here, but only the first has any data.
      static const PacketTypeInfo op_info = {PacketCategory::kSwap, "PM4_XE_SWAP"};
      out_info->type_info = &op_info;
      uint32_t frontbuffer_ptr = memory::load_and_swap<uint32_t>(ptr + 0);
      break;
    }
    case PM4_INDIRECT_BUFFER:
    case PM4_INDIRECT_BUFFER_PFD: {
      // indirect buffer dispatch
      static const PacketTypeInfo op_info = {PacketCategory::kGeneric, "PM4_INDIRECT_BUFFER"};
      out_info->type_info = &op_info;
      uint32_t list_ptr = memory::load_and_swap<uint32_t>(ptr + 0);
      uint32_t list_length = memory::load_and_swap<uint32_t>(ptr + 4);
      break;
    }
    case PM4_WAIT_REG_MEM: {
      // wait until a register or memory location is a specific value
      static const PacketTypeInfo op_info = {PacketCategory::kGeneric, "PM4_WAIT_REG_MEM"};
      out_info->type_info = &op_info;
      uint32_t wait_info = memory::load_and_swap<uint32_t>(ptr + 0);
      uint32_t poll_reg_addr = memory::load_and_swap<uint32_t>(ptr + 4);
      uint32_t ref = memory::load_and_swap<uint32_t>(ptr + 8);
      uint32_t mask = memory::load_and_swap<uint32_t>(ptr + 12);
      uint32_t wait = memory::load_and_swap<uint32_t>(ptr + 16);
      break;
    }
    case PM4_REG_RMW: {
      // register read/modify/write
      // ? (used during shader upload and edram setup)
      static const PacketTypeInfo op_info = {PacketCategory::kGeneric, "PM4_REG_RMW"};
      out_info->type_info = &op_info;
      uint32_t rmw_info = memory::load_and_swap<uint32_t>(ptr + 0);
      uint32_t and_mask = memory::load_and_swap<uint32_t>(ptr + 4);
      uint32_t or_mask = memory::load_and_swap<uint32_t>(ptr + 8);
      break;
    }
    case PM4_COND_WRITE: {
      // conditional write to memory or register
      static const PacketTypeInfo op_info = {PacketCategory::kGeneric, "PM4_COND_WRITE"};
      out_info->type_info = &op_info;
      uint32_t wait_info = memory::load_and_swap<uint32_t>(ptr + 0);
      uint32_t poll_reg_addr = memory::load_and_swap<uint32_t>(ptr + 4);
      uint32_t ref = memory::load_and_swap<uint32_t>(ptr + 8);
      uint32_t mask = memory::load_and_swap<uint32_t>(ptr + 12);
      uint32_t write_reg_addr = memory::load_and_swap<uint32_t>(ptr + 16);
      uint32_t write_data = memory::load_and_swap<uint32_t>(ptr + 20);
      break;
    }
    case PM4_EVENT_WRITE: {
      // generate an event that creates a write to memory when completed
      static const PacketTypeInfo op_info = {PacketCategory::kGeneric, "PM4_EVENT_WRITE"};
      out_info->type_info = &op_info;
      uint32_t initiator = memory::load_and_swap<uint32_t>(ptr + 0);
      break;
    }
    case PM4_EVENT_WRITE_SHD: {
      // generate a VS|PS_done event
      static const PacketTypeInfo op_info = {PacketCategory::kGeneric, "PM4_EVENT_WRITE_SHD"};
      out_info->type_info = &op_info;
      uint32_t initiator = memory::load_and_swap<uint32_t>(ptr + 0);
      uint32_t address = memory::load_and_swap<uint32_t>(ptr + 4);
      uint32_t value = memory::load_and_swap<uint32_t>(ptr + 8);
      break;
    }
    case PM4_EVENT_WRITE_EXT: {
      // generate a screen extent event
      static const PacketTypeInfo op_info = {PacketCategory::kGeneric, "PM4_EVENT_WRITE_EXT"};
      out_info->type_info = &op_info;
      uint32_t unk0 = memory::load_and_swap<uint32_t>(ptr + 0);
      uint32_t unk1 = memory::load_and_swap<uint32_t>(ptr + 4);
      break;
    }
    case PM4_DRAW_INDX:
    case PM4_DRAW_INDX_2: {
      const uint32_t prefix = opcode == PM4_DRAW_INDX ? 1 : 0;
      if (count <= prefix)
        return false;
      const uint32_t control = memory::load_and_swap<uint32_t>(ptr + prefix * 4);
      const uint32_t source = (control >> 6) & 3;
      if ((source != 0 && source != 2) || (source == 0 && count - prefix < 3))
        return false;
      static const PacketTypeInfo query_info = {PacketCategory::kDraw, "PM4_DRAW_INDX"};
      static const PacketTypeInfo auto_info = {PacketCategory::kDraw, "PM4_DRAW_INDX_2"};
      out_info->type_info = prefix ? &query_info : &auto_info;
      out_info->actions.emplace_back(
          PacketAction::RegisterWrite(XE_GPU_REG_VGT_DRAW_INITIATOR, control));
      if (source == 0) {
        out_info->actions.emplace_back(PacketAction::RegisterWrite(
            XE_GPU_REG_VGT_DMA_BASE, memory::load_and_swap<uint32_t>(ptr + (prefix + 1) * 4)));
        out_info->actions.emplace_back(PacketAction::RegisterWrite(
            XE_GPU_REG_VGT_DMA_SIZE, memory::load_and_swap<uint32_t>(ptr + (prefix + 2) * 4)));
      }
      return true;
    }
    case PM4_SET_CONSTANT: {
      // load constant into chip and to memory
      // PM4_REG(reg) ((0x4 << 16) | (GSL_HAL_SUBBLOCK_OFFSET(reg)))
      //                                     reg - 0x2000
      static const PacketTypeInfo op_info = {PacketCategory::kGeneric, "PM4_SET_CONSTANT"};
      out_info->type_info = &op_info;
      uint32_t offset_type = memory::load_and_swap<uint32_t>(ptr + 0);
      uint32_t index = offset_type & 0x7FF;
      uint32_t type = (offset_type >> 16) & 0xFF;
      switch (type) {
        case 0:  // ALU
          index += 0x4000;
          break;
        case 1:  // FETCH
          index += 0x4800;
          break;
        case 2:  // BOOL
          index += 0x4900;
          break;
        case 3:  // LOOP
          index += 0x4908;
          break;
        case 4:  // REGISTERS
          index += 0x2000;
          break;
        default:
          assert_always();
          result = false;
          break;
      }
      for (uint32_t n = 0; n < count - 1; n++, index++) {
        uint32_t data = memory::load_and_swap<uint32_t>(ptr + 4 + n * 4);
        out_info->actions.emplace_back(PacketAction::RegisterWrite(index, data));
      }
      break;
    }
    case PM4_SET_CONSTANT2: {
      static const PacketTypeInfo op_info = {PacketCategory::kGeneric, "PM4_SET_CONSTANT2"};
      out_info->type_info = &op_info;
      uint32_t offset_type = memory::load_and_swap<uint32_t>(ptr + 0);
      uint32_t index = offset_type & 0xFFFF;
      for (uint32_t n = 0; n < count - 1; n++, index++) {
        uint32_t data = memory::load_and_swap<uint32_t>(ptr + 4 + n * 4);
        out_info->actions.emplace_back(PacketAction::RegisterWrite(index, data));
      }
      return true;
      break;
    }
    case PM4_LOAD_ALU_CONSTANT: {
      // load constants from memory
      static const PacketTypeInfo op_info = {PacketCategory::kGeneric, "PM4_LOAD_ALU_CONSTANT"};
      out_info->type_info = &op_info;
      uint32_t address = memory::load_and_swap<uint32_t>(ptr + 0);
      address &= 0x3FFFFFFF;
      uint32_t offset_type = memory::load_and_swap<uint32_t>(ptr + 4);
      uint32_t index = offset_type & 0x7FF;
      uint32_t size_dwords = memory::load_and_swap<uint32_t>(ptr + 8);
      size_dwords &= 0xFFF;
      uint32_t type = (offset_type >> 16) & 0xFF;
      switch (type) {
        case 0:  // ALU
          index += 0x4000;
          break;
        case 1:  // FETCH
          index += 0x4800;
          break;
        case 2:  // BOOL
          index += 0x4900;
          break;
        case 3:  // LOOP
          index += 0x4908;
          break;
        case 4:  // REGISTERS
          index += 0x2000;
          break;
        default:
          assert_always();
          return true;
      }
      if (getenv("REPLAY_ALU_LOG")) {
        uint32_t first = 0;
        if (disasm_membase && (address & (disasm_memsize - 1)) + 4 <= disasm_memsize)
          first = memory::load_and_swap<uint32_t>(disasm_membase +
                                                  (address & (disasm_memsize - 1)));
        std::fprintf(stderr, "LOAD_ALU idx=%04X n=%u src=%08X first=%08X\n", index,
                     size_dwords, address, first);
      }
      for (uint32_t n = 0; n < size_dwords; n++, index++) {
        uint32_t data = 0xDEADBEEF;
        if (disasm_membase) {
          uint32_t src = (address + n * 4) & (disasm_memsize - 1);
          if (src + 4 <= disasm_memsize)
            data = memory::load_and_swap<uint32_t>(disasm_membase + src);
        }
        out_info->actions.emplace_back(PacketAction::RegisterWrite(index, data));
      }
      break;
    }
    case PM4_SET_SHADER_CONSTANTS: {
      static const PacketTypeInfo op_info = {PacketCategory::kGeneric, "PM4_SET_SHADER_CONSTANTS"};
      out_info->type_info = &op_info;
      uint32_t offset_type = memory::load_and_swap<uint32_t>(ptr + 0);
      uint32_t index = offset_type & 0xFFFF;
      for (uint32_t n = 0; n < count - 1; n++, index++) {
        uint32_t data = memory::load_and_swap<uint32_t>(ptr + 4 + n * 4);
        out_info->actions.emplace_back(PacketAction::RegisterWrite(index, data));
      }
      return true;
    }
    case PM4_IM_LOAD: {
      // load sequencer instruction memory (pointer-based)
      static const PacketTypeInfo op_info = {PacketCategory::kGeneric, "PM4_IM_LOAD"};
      out_info->type_info = &op_info;
      uint32_t addr_type = memory::load_and_swap<uint32_t>(ptr + 0);
      auto shader_type = static_cast<xenos::ShaderType>(addr_type & 0x3);
      uint32_t addr = addr_type & ~0x3;
      uint32_t start_size = memory::load_and_swap<uint32_t>(ptr + 4);
      uint32_t start = start_size >> 16;
      uint32_t size_dwords = start_size & 0xFFFF;  // dwords
      assert_true(start == 0);
      break;
    }
    case PM4_IM_LOAD_IMMEDIATE: {
      // load sequencer instruction memory (code embedded in packet)
      static const PacketTypeInfo op_info = {PacketCategory::kGeneric, "PM4_IM_LOAD_IMMEDIATE"};
      out_info->type_info = &op_info;
      uint32_t dword0 = memory::load_and_swap<uint32_t>(ptr + 0);
      uint32_t dword1 = memory::load_and_swap<uint32_t>(ptr + 4);
      auto shader_type = static_cast<xenos::ShaderType>(dword0);
      uint32_t start_size = dword1;
      uint32_t start = start_size >> 16;
      uint32_t size_dwords = start_size & 0xFFFF;  // dwords
      assert_true(start == 0);
      break;
    }
    case PM4_INVALIDATE_STATE: {
      // selective invalidation of state pointers
      static const PacketTypeInfo op_info = {PacketCategory::kGeneric, "PM4_INVALIDATE_STATE"};
      out_info->type_info = &op_info;
      uint32_t mask = memory::load_and_swap<uint32_t>(ptr + 0);
      break;
    }
    case PM4_SET_BIN_MASK_LO: {
      static const PacketTypeInfo op_info = {PacketCategory::kGeneric, "PM4_SET_BIN_MASK_LO"};
      out_info->type_info = &op_info;
      uint32_t value = memory::load_and_swap<uint32_t>(ptr);
      // bin_mask_ = (bin_mask_ & 0xFFFFFFFF00000000ull) | value;
      out_info->actions.emplace_back(
          PacketAction::SetBinHalf(PacketAction::Type::kSetBinMaskLow, value));
      break;
    }
    case PM4_SET_BIN_MASK_HI: {
      static const PacketTypeInfo op_info = {PacketCategory::kGeneric, "PM4_SET_BIN_MASK_HI"};
      out_info->type_info = &op_info;
      uint32_t value = memory::load_and_swap<uint32_t>(ptr);
      // bin_mask_ =
      //  (bin_mask_ & 0xFFFFFFFFull) | (static_cast<uint64_t>(value) << 32);
      out_info->actions.emplace_back(
          PacketAction::SetBinHalf(PacketAction::Type::kSetBinMaskHigh, value));
      break;
    }
    case PM4_SET_BIN_SELECT_LO: {
      static const PacketTypeInfo op_info = {PacketCategory::kGeneric, "PM4_SET_BIN_SELECT_LO"};
      out_info->type_info = &op_info;
      uint32_t value = memory::load_and_swap<uint32_t>(ptr);
      // bin_select_ = (bin_select_ & 0xFFFFFFFF00000000ull) | value;
      out_info->actions.emplace_back(
          PacketAction::SetBinHalf(PacketAction::Type::kSetBinSelectLow, value));
      break;
    }
    case PM4_SET_BIN_SELECT_HI: {
      static const PacketTypeInfo op_info = {PacketCategory::kGeneric, "PM4_SET_BIN_SELECT_HI"};
      out_info->type_info = &op_info;
      uint32_t value = memory::load_and_swap<uint32_t>(ptr);
      // bin_select_ =
      //  (bin_select_ & 0xFFFFFFFFull) | (static_cast<uint64_t>(value) <<
      //  32);
      out_info->actions.emplace_back(
          PacketAction::SetBinHalf(PacketAction::Type::kSetBinSelectHigh, value));
      break;
    }

    // Ignored packets - useful if breaking on the default handler below.
    case PM4_SET_BIN_MASK: {
      if (count != 2)
        return false;
      static const PacketTypeInfo op_info = {PacketCategory::kGeneric, "PM4_SET_BIN_MASK"};
      out_info->type_info = &op_info;
      out_info->actions.emplace_back(
          PacketAction::SetBinMask((uint64_t(memory::load_and_swap<uint32_t>(ptr)) << 32) |
                                   memory::load_and_swap<uint32_t>(ptr + 4)));
      break;
    }
    case PM4_SET_BIN_SELECT: {
      if (count != 2)
        return false;
      static const PacketTypeInfo op_info = {PacketCategory::kGeneric, "PM4_SET_BIN_SELECT"};
      out_info->type_info = &op_info;
      out_info->actions.emplace_back(
          PacketAction::SetBinSelect((uint64_t(memory::load_and_swap<uint32_t>(ptr)) << 32) |
                                     memory::load_and_swap<uint32_t>(ptr + 4)));
      break;
    }
    default: {
      result = false;
      break;
    }
  }

  return result;
}

bool PacketDisassembler::DisasmPacket(const uint8_t* base_ptr, PacketInfo* out_info) {
  *out_info = {};
  const uint32_t packet = memory::load_and_swap<uint32_t>(base_ptr);
  const uint32_t packet_type = packet >> 30;
  switch (packet_type) {
    case 0x00:
      return DisasmPacketType0(base_ptr, packet, out_info);
    case 0x01:
      return DisasmPacketType1(base_ptr, packet, out_info);
    case 0x02:
      return DisasmPacketType2(base_ptr, packet, out_info);
    case 0x03:
      return DisasmPacketType3(base_ptr, packet, out_info);
    default:
      assert_unhandled_case(packet_type);
      return false;
  }
}

}  // namespace rex::graphics
