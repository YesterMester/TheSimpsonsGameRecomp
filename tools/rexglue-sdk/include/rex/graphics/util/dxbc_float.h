/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <rex/graphics/format/dxbc.h>

namespace rex::graphics {

// Guest color and depth conversions emitted as native Direct3D instructions.
// Kept independent of the game shader translator for render target programs.
class DxbcFloatEmitter {
 public:
  static void PWLGammaToLinear(dxbc::Assembler& a, uint32_t target_temp,
                               uint32_t target_temp_component, uint32_t source_temp,
                               uint32_t source_temp_component, bool source_pre_saturated,
                               uint32_t temp1, uint32_t temp1_component, uint32_t temp2,
                               uint32_t temp2_component);
  static void PreSaturatedLinearToPWLGamma(dxbc::Assembler& a, uint32_t target_temp,
                                           uint32_t target_temp_component, uint32_t source_temp,
                                           uint32_t source_temp_component, uint32_t temp_or_target,
                                           uint32_t temp_or_target_component,
                                           uint32_t temp_non_target,
                                           uint32_t temp_non_target_component);
  static void PreClampedFloat32To7e3(dxbc::Assembler& a, uint32_t f10_temp,
                                     uint32_t f10_temp_component, uint32_t f32_temp,
                                     uint32_t f32_temp_component, uint32_t temp_temp,
                                     uint32_t temp_temp_component);
  static void UnclampedFloat32To7e3(dxbc::Assembler& a, uint32_t f10_temp,
                                    uint32_t f10_temp_component, uint32_t f32_temp,
                                    uint32_t f32_temp_component, uint32_t temp_temp,
                                    uint32_t temp_temp_component);
  static void Float7e3To32(dxbc::Assembler& a, const dxbc::Dest& f32, uint32_t f10_temp,
                           uint32_t f10_temp_component, uint32_t f10_shift, uint32_t temp1_temp,
                           uint32_t temp1_temp_component, uint32_t temp2_temp,
                           uint32_t temp2_temp_component);
  static void PreClampedDepthTo20e4(dxbc::Assembler& a, uint32_t f24_temp,
                                    uint32_t f24_temp_component, uint32_t f32_temp,
                                    uint32_t f32_temp_component, uint32_t temp_temp,
                                    uint32_t temp_temp_component, bool round_to_nearest_even,
                                    bool remap_from_0_to_0_5);
  static void Depth20e4To32(dxbc::Assembler& a, const dxbc::Dest& f32, uint32_t f24_temp,
                            uint32_t f24_temp_component, uint32_t f24_shift, uint32_t temp1_temp,
                            uint32_t temp1_temp_component, uint32_t temp2_temp,
                            uint32_t temp2_temp_component, bool remap_to_0_to_0_5);
};

}  // namespace rex::graphics
