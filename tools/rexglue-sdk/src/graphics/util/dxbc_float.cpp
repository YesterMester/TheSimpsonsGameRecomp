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

#include <rex/graphics/util/dxbc_float.h>

#include <rex/assert.h>

namespace rex::graphics {

void DxbcFloatEmitter::PWLGammaToLinear(dxbc::Assembler& a, uint32_t target_temp,
                                        uint32_t target_temp_component, uint32_t source_temp,
                                        uint32_t source_temp_component, bool source_pre_saturated,
                                        uint32_t temp1, uint32_t temp1_component, uint32_t temp2,
                                        uint32_t temp2_component) {
  // The source is needed only once to begin building the result, so it can be
  // the same as the destination.
  assert_true(temp1 != target_temp || temp1_component != target_temp_component);
  assert_true(temp1 != source_temp || temp1_component != source_temp_component);
  assert_true(temp2 != target_temp || temp2_component != target_temp_component);
  assert_true(temp2 != source_temp || temp2_component != source_temp_component);
  assert_true(temp1 != temp2 || temp1_component != temp2_component);
  dxbc::Dest target_dest(dxbc::Dest::R(target_temp, UINT32_C(1) << target_temp_component));
  dxbc::Src target_src(dxbc::Src::R(target_temp).Select(target_temp_component));
  dxbc::Src source_src(dxbc::Src::R(source_temp).Select(source_temp_component));
  dxbc::Dest temp1_dest(dxbc::Dest::R(temp1, UINT32_C(1) << temp1_component));
  dxbc::Src temp1_src(dxbc::Src::R(temp1).Select(temp1_component));
  dxbc::Dest temp2_dest(dxbc::Dest::R(temp2, UINT32_C(1) << temp2_component));
  dxbc::Src temp2_src(dxbc::Src::R(temp2).Select(temp2_component));

  // Get the scale (into temp1) and the offset (into temp2) for the piece.
  // Using `source >= threshold` comparisons because the input might have not
  // been saturated yet, and thus it may be NaN - since it will be saturated to
  // 0 later, the 0...64/255 case should be selected for it.
  a.OpGE(temp2_dest, source_src, dxbc::Src::LF(96.0f / 255.0f));
  a.OpIf(true, temp2_src);
  // [96/255 ... 1
  a.OpGE(temp2_dest, source_src, dxbc::Src::LF(192.0f / 255.0f));
  a.OpMovC(temp1_dest, temp2_src, dxbc::Src::LF(8.0f / 1024.0f), dxbc::Src::LF(4.0f / 1024.0f));
  a.OpMovC(temp2_dest, temp2_src, dxbc::Src::LF(-1024.0f), dxbc::Src::LF(-256.0f));
  a.OpElse();
  // 0 ... 96/255)
  a.OpGE(temp2_dest, source_src, dxbc::Src::LF(64.0f / 255.0f));
  a.OpMovC(temp1_dest, temp2_src, dxbc::Src::LF(2.0f / 1024.0f), dxbc::Src::LF(1.0f / 1024.0f));
  a.OpMovC(temp2_dest, temp2_src, dxbc::Src::LF(-64.0f), dxbc::Src::LF(0.0f));
  a.OpEndIf();

  if (!source_pre_saturated) {
    // Saturate the input, and flush NaN to 0.
    a.OpMov(target_dest, source_src, true);
  }
  // linear = gamma * (255 * 1024) * scale + offset
  // As both 1024 and the scale are powers of 2, and 1024 * scale is not smaller
  // than 1, it's not important if it's (gamma * 255) * 1024 * scale,
  // (gamma * 255 * 1024) * scale, gamma * 255 * (1024 * scale), or
  // gamma * (255 * 1024 * scale) - or the option chosen here, as long as
  // 1024 is applied before the scale since the scale is < 1 (specifically at
  // least 1/1024), and it may make very small values denormal.
  a.OpMul(target_dest, source_pre_saturated ? source_src : target_src,
          dxbc::Src::LF(255.0f * 1024.0f));
  a.OpMAd(target_dest, target_src, temp1_src, temp2_src);
  // linear += trunc(linear * scale)
  a.OpMul(temp1_dest, target_src, temp1_src);
  a.OpRoundZ(temp1_dest, temp1_src);
  a.OpAdd(target_dest, target_src, temp1_src);
  // linear *= 1/1023
  a.OpMul(target_dest, target_src, dxbc::Src::LF(1.0f / 1023.0f));
}

void DxbcFloatEmitter::PreSaturatedLinearToPWLGamma(
    dxbc::Assembler& a, uint32_t target_temp, uint32_t target_temp_component, uint32_t source_temp,
    uint32_t source_temp_component, uint32_t temp_or_target, uint32_t temp_or_target_component,
    uint32_t temp_non_target, uint32_t temp_non_target_component) {
  // The source may be the same as the target, but in this case it can't also be
  // used as a temporary variable.
  assert_true(target_temp != source_temp || target_temp_component != source_temp_component ||
              target_temp != temp_or_target || target_temp_component != temp_or_target_component);
  assert_true(temp_or_target != source_temp || temp_or_target_component != source_temp_component);
  assert_true(temp_non_target != target_temp || temp_non_target_component != target_temp_component);
  assert_true(temp_non_target != source_temp || temp_non_target_component != source_temp_component);
  assert_true(temp_or_target != temp_non_target ||
              temp_or_target_component != temp_non_target_component);
  dxbc::Dest target_dest(dxbc::Dest::R(target_temp, UINT32_C(1) << target_temp_component));
  dxbc::Src target_src(dxbc::Src::R(target_temp).Select(target_temp_component));
  dxbc::Src source_src(dxbc::Src::R(source_temp).Select(source_temp_component));
  dxbc::Dest temp_or_target_dest(
      dxbc::Dest::R(temp_or_target, UINT32_C(1) << temp_or_target_component));
  dxbc::Src temp_or_target_src(dxbc::Src::R(temp_or_target).Select(temp_or_target_component));
  dxbc::Dest temp_non_target_dest(
      dxbc::Dest::R(temp_non_target, UINT32_C(1) << temp_non_target_component));
  dxbc::Src temp_non_target_src(dxbc::Src::R(temp_non_target).Select(temp_non_target_component));

  // Get the scale (into temp_or_target) and the offset (into temp_non_target)
  // for the piece.
  a.OpGE(temp_non_target_dest, source_src, dxbc::Src::LF(128.0f / 1023.0f));
  a.OpIf(true, temp_non_target_src);
  // [128/1023 ... 1
  a.OpGE(temp_non_target_dest, source_src, dxbc::Src::LF(512.0f / 1023.0f));
  a.OpMovC(temp_or_target_dest, temp_non_target_src, dxbc::Src::LF(1023.0f / 8.0f),
           dxbc::Src::LF(1023.0f / 4.0f));
  a.OpMovC(temp_non_target_dest, temp_non_target_src, dxbc::Src::LF(128.0f / 255.0f),
           dxbc::Src::LF(64.0f / 255.0f));
  a.OpElse();
  // 0 ... 128/1023)
  a.OpGE(temp_non_target_dest, source_src, dxbc::Src::LF(64.0f / 1023.0f));
  a.OpMovC(temp_or_target_dest, temp_non_target_src, dxbc::Src::LF(1023.0f / 2.0f),
           dxbc::Src::LF(1023.0f));
  a.OpMovC(temp_non_target_dest, temp_non_target_src, dxbc::Src::LF(32.0f / 255.0f),
           dxbc::Src::LF(0.0f));
  a.OpEndIf();

  // gamma = trunc(linear * scale) * (1.0 / 255.0) + offset
  a.OpMul(target_dest, source_src, temp_or_target_src);
  a.OpRoundZ(target_dest, target_src);
  a.OpMAd(target_dest, target_src, dxbc::Src::LF(1.0f / 255.0f), temp_non_target_src);
}

void DxbcFloatEmitter::PreClampedFloat32To7e3(dxbc::Assembler& a, uint32_t f10_temp,
                                              uint32_t f10_temp_component, uint32_t f32_temp,
                                              uint32_t f32_temp_component, uint32_t temp_temp,
                                              uint32_t temp_temp_component) {
  assert_true(temp_temp != f10_temp || temp_temp_component != f10_temp_component);
  assert_true(temp_temp != f32_temp || temp_temp_component != f32_temp_component);
  // Source and destination may be the same.
  dxbc::Dest f10_dest(dxbc::Dest::R(f10_temp, 1 << f10_temp_component));
  dxbc::Src f10_src(dxbc::Src::R(f10_temp).Select(f10_temp_component));
  dxbc::Src f32_src(dxbc::Src::R(f32_temp).Select(f32_temp_component));
  dxbc::Dest temp_dest(dxbc::Dest::R(temp_temp, 1 << temp_temp_component));
  dxbc::Src temp_src(dxbc::Src::R(temp_temp).Select(temp_temp_component));

  // https://github.com/Microsoft/DirectXTex/blob/master/DirectXTex/DirectXTexConvert.cpp
  // Assuming the color is already clamped to [0, 31.875].

  // Check if the number is too small to be represented as normalized 7e3.
  // temp = f32 < 2^-2
  a.OpULT(temp_dest, f32_src, dxbc::Src::LU(0x3E800000));
  // Handle denormalized numbers separately.
  a.OpIf(true, temp_src);
  {
    // temp = f32 >> 23
    a.OpUShR(temp_dest, f32_src, dxbc::Src::LU(23));
    // temp = 125 - (f32 >> 23)
    a.OpIAdd(temp_dest, dxbc::Src::LI(125), -temp_src);
    // Don't allow the shift to overflow, since in DXBC the lower 5 bits of the
    // shift amount are used.
    // temp = min(125 - (f32 >> 23), 24)
    a.OpUMin(temp_dest, temp_src, dxbc::Src::LU(24));
    // biased_f32 = (f32 & 0x7FFFFF) | 0x800000
    a.OpBFI(f10_dest, dxbc::Src::LU(9), dxbc::Src::LU(23), dxbc::Src::LU(1), f32_src);
    // biased_f32 = ((f32 & 0x7FFFFF) | 0x800000) >> min(125 - (f32 >> 23), 24)
    a.OpUShR(f10_dest, f10_src, temp_src);
  }
  // Not denormalized?
  a.OpElse();
  {
    // Bias the exponent.
    // biased_f32 = f32 + (-124 << 23)
    // (left shift of a negative value is undefined behavior)
    a.OpIAdd(f10_dest, f32_src, dxbc::Src::LU(0xC2000000u));
  }
  // Close the denormal check.
  a.OpEndIf();
  // Build the 7e3 number.
  // temp = (biased_f32 >> 16) & 1
  a.OpUBFE(temp_dest, dxbc::Src::LU(1), dxbc::Src::LU(16), f10_src);
  // f10 = biased_f32 + 0x7FFF
  a.OpIAdd(f10_dest, f10_src, dxbc::Src::LU(0x7FFF));
  // f10 = biased_f32 + 0x7FFF + ((biased_f32 >> 16) & 1)
  a.OpIAdd(f10_dest, f10_src, temp_src);
  // f24 = ((biased_f32 + 0x7FFF + ((biased_f32 >> 16) & 1)) >> 16) & 0x3FF
  a.OpUBFE(f10_dest, dxbc::Src::LU(10), dxbc::Src::LU(16), f10_src);
}

void DxbcFloatEmitter::UnclampedFloat32To7e3(dxbc::Assembler& a, uint32_t f10_temp,
                                             uint32_t f10_temp_component, uint32_t f32_temp,
                                             uint32_t f32_temp_component, uint32_t temp_temp,
                                             uint32_t temp_temp_component) {
  // Source and destination might be the same or different, just like in
  // PreClampedFloat32To7e3 - clamp to the destination and use it as source.
  a.OpMax(dxbc::Dest::R(f10_temp, 1 << f10_temp_component),
          dxbc::Src::R(f32_temp).Select(f32_temp_component), dxbc::Src::LF(0.0f));
  a.OpMin(dxbc::Dest::R(f10_temp, 1 << f10_temp_component),
          dxbc::Src::R(f10_temp).Select(f10_temp_component), dxbc::Src::LF(31.875f));
  PreClampedFloat32To7e3(a, f10_temp, f10_temp_component, f10_temp, f10_temp_component, temp_temp,
                         temp_temp_component);
}

void DxbcFloatEmitter::Float7e3To32(dxbc::Assembler& a, const dxbc::Dest& f32, uint32_t f10_temp,
                                    uint32_t f10_temp_component, uint32_t f10_shift,
                                    uint32_t temp1_temp, uint32_t temp1_temp_component,
                                    uint32_t temp2_temp, uint32_t temp2_temp_component) {
  assert_true(f10_shift <= (32 - 10));
  assert_true(temp1_temp != temp2_temp || temp1_temp_component != temp2_temp_component);
  // Source may be the same as temp1 or temp2.
  dxbc::Dest exponent_dest(dxbc::Dest::R(temp1_temp, 1 << temp1_temp_component));
  dxbc::Src exponent_src(dxbc::Src::R(temp1_temp).Select(temp1_temp_component));
  dxbc::Dest mantissa_dest(dxbc::Dest::R(temp2_temp, 1 << temp2_temp_component));
  dxbc::Src mantissa_src(dxbc::Src::R(temp2_temp).Select(temp2_temp_component));

  // https://github.com/Microsoft/DirectXTex/blob/master/DirectXTex/DirectXTexConvert.cpp

  if (!(f10_temp == temp1_temp && f10_temp_component == temp1_temp_component)) {
    // Unpack the exponent before the mantissa if that doesn't overwrite the
    // source.
    a.OpUBFE(exponent_dest, dxbc::Src::LU(3), dxbc::Src::LU(f10_shift + 7),
             dxbc::Src::R(f10_temp).Select(f10_temp_component));
  }
  // Unpack the mantissa.
  a.OpUBFE(mantissa_dest, dxbc::Src::LU(7), dxbc::Src::LU(f10_shift),
           dxbc::Src::R(f10_temp).Select(f10_temp_component));
  if (f10_temp == temp1_temp && f10_temp_component == temp1_temp_component) {
    // Unpack the exponent after the mantissa if doing that before the mantissa
    // would overwrite the source.
    a.OpUBFE(exponent_dest, dxbc::Src::LU(3), dxbc::Src::LU(f10_shift + 7),
             dxbc::Src::R(f10_temp).Select(f10_temp_component));
  }
  // Check if the number is denormalized.
  a.OpIf(false, exponent_src);
  {
    // Check if the number is non-zero (if the mantissa isn't zero - the
    // exponent is known to be zero at this point).
    a.OpIf(true, mantissa_src);
    {
      // Normalize the mantissa.
      // Note that HLSL firstbithigh(x) is compiled to DXBC like:
      // `x ? 31 - firstbit_hi(x) : -1`
      // (returns the index from the LSB, not the MSB, but -1 for zero too).
      // exponent = firstbit_hi(mantissa)
      a.OpFirstBitHi(exponent_dest, mantissa_src);
      // exponent = 7 - firstbithigh(mantissa)
      // Or:
      // exponent = 7 - (31 - firstbit_hi(mantissa))
      a.OpIAdd(exponent_dest, exponent_src, dxbc::Src::LI(7 - 31));
      // mantissa = mantissa << (7 - firstbithigh(mantissa))
      // AND 0x7F not needed after this - BFI will do it.
      a.OpIShL(mantissa_dest, mantissa_src, exponent_src);
      // Get the normalized exponent.
      // exponent = 1 - (7 - firstbithigh(mantissa))
      a.OpIAdd(exponent_dest, dxbc::Src::LI(1), -exponent_src);
    }
    // The number is zero.
    a.OpElse();
    {
      // Set the unbiased exponent to -124 for zero - 124 will be added later,
      // resulting in zero float32.
      a.OpMov(exponent_dest, dxbc::Src::LI(-124));
    }
    // Close the non-zero check.
    a.OpEndIf();
  }
  // Close the denormal check.
  a.OpEndIf();
  // Bias the exponent and move it to the correct location in f32.
  a.OpIMAd(exponent_dest, exponent_src, dxbc::Src::LI(1 << 23), dxbc::Src::LI(124 << 23));
  // Combine the mantissa and the exponent.
  a.OpBFI(f32, dxbc::Src::LU(7), dxbc::Src::LU(23 - 7), mantissa_src, exponent_src);
}

void DxbcFloatEmitter::PreClampedDepthTo20e4(dxbc::Assembler& a, uint32_t f24_temp,
                                             uint32_t f24_temp_component, uint32_t f32_temp,
                                             uint32_t f32_temp_component, uint32_t temp_temp,
                                             uint32_t temp_temp_component,
                                             bool round_to_nearest_even, bool remap_from_0_to_0_5) {
  assert_true(temp_temp != f24_temp || temp_temp_component != f24_temp_component);
  assert_true(temp_temp != f32_temp || temp_temp_component != f32_temp_component);
  // Source and destination may be the same.
  dxbc::Dest f24_dest(dxbc::Dest::R(f24_temp, 1 << f24_temp_component));
  dxbc::Src f24_src(dxbc::Src::R(f24_temp).Select(f24_temp_component));
  dxbc::Src f32_src(dxbc::Src::R(f32_temp).Select(f32_temp_component));
  dxbc::Dest temp_dest(dxbc::Dest::R(temp_temp, 1 << temp_temp_component));
  dxbc::Src temp_src(dxbc::Src::R(temp_temp).Select(temp_temp_component));

  // CFloat24 from d3dref9.dll +
  // https://github.com/Microsoft/DirectXTex/blob/master/DirectXTex/DirectXTexConvert.cpp
  // Assuming the depth is already clamped to [0, 2) (in all places, the depth
  // is written with the saturate flag set).

  uint32_t remap_bias = uint32_t(remap_from_0_to_0_5);

  // Check if the number is too small to be represented as normalized 20e4.
  // temp = f32 < 2^-14
  a.OpULT(temp_dest, f32_src, dxbc::Src::LU(0x38800000 - (remap_bias << 23)));
  // Handle denormalized numbers separately.
  a.OpIf(true, temp_src);
  {
    // temp = f32 >> 23
    a.OpUShR(temp_dest, f32_src, dxbc::Src::LU(23));
    // temp = 113 - (f32 >> 23)
    a.OpIAdd(temp_dest, dxbc::Src::LI(113 - remap_bias), -temp_src);
    // Don't allow the shift to overflow, since in DXBC the lower 5 bits of the
    // shift amount are used (otherwise 0 becomes 8).
    // temp = min(113 - (f32 >> 23), 24)
    a.OpUMin(temp_dest, temp_src, dxbc::Src::LU(24));
    // biased_f32 = (f32 & 0x7FFFFF) | 0x800000
    a.OpBFI(f24_dest, dxbc::Src::LU(9), dxbc::Src::LU(23), dxbc::Src::LU(1), f32_src);
    // biased_f32 = ((f32 & 0x7FFFFF) | 0x800000) >> min(113 - (f32 >> 23), 24)
    a.OpUShR(f24_dest, f24_src, temp_src);
  }
  // Not denormalized?
  a.OpElse();
  {
    // Bias the exponent.
    // biased_f32 = f32 + (-112 << 23)
    // (left shift of a negative value is undefined behavior)
    a.OpIAdd(f24_dest, f32_src, dxbc::Src::LU(0xC8000000u + (remap_bias << 23)));
  }
  // Close the denormal check.
  a.OpEndIf();
  // Build the 20e4 number.
  if (round_to_nearest_even) {
    // temp = (biased_f32 >> 3) & 1
    a.OpUBFE(temp_dest, dxbc::Src::LU(1), dxbc::Src::LU(3), f24_src);
    // f24 = biased_f32 + 3
    a.OpIAdd(f24_dest, f24_src, dxbc::Src::LU(3));
    // f24 = biased_f32 + 3 + ((biased_f32 >> 3) & 1)
    a.OpIAdd(f24_dest, f24_src, temp_src);
  }
  // For rounding to the nearest even:
  // f24 = ((biased_f32 + 3 + ((biased_f32 >> 3) & 1)) >> 3) & 0xFFFFFF
  // For rounding towards zero:
  // f24 = (biased_f32 >> 3) & 0xFFFFFF
  a.OpUBFE(f24_dest, dxbc::Src::LU(24), dxbc::Src::LU(3), f24_src);
}

void DxbcFloatEmitter::Depth20e4To32(dxbc::Assembler& a, const dxbc::Dest& f32, uint32_t f24_temp,
                                     uint32_t f24_temp_component, uint32_t f24_shift,
                                     uint32_t temp1_temp, uint32_t temp1_temp_component,
                                     uint32_t temp2_temp, uint32_t temp2_temp_component,
                                     bool remap_to_0_to_0_5) {
  assert_true(f24_shift <= (32 - 24));
  assert_true(temp1_temp != temp2_temp || temp1_temp_component != temp2_temp_component);
  // Source may be the same as temp1 or temp2.
  dxbc::Dest exponent_dest(dxbc::Dest::R(temp1_temp, 1 << temp1_temp_component));
  dxbc::Src exponent_src(dxbc::Src::R(temp1_temp).Select(temp1_temp_component));
  dxbc::Dest mantissa_dest(dxbc::Dest::R(temp2_temp, 1 << temp2_temp_component));
  dxbc::Src mantissa_src(dxbc::Src::R(temp2_temp).Select(temp2_temp_component));

  // CFloat24 from d3dref9.dll +
  // https://github.com/Microsoft/DirectXTex/blob/master/DirectXTex/DirectXTexConvert.cpp

  uint32_t remap_bias = uint32_t(remap_to_0_to_0_5);

  if (!(f24_temp == temp1_temp && f24_temp_component == temp1_temp_component)) {
    // Unpack the exponent before the mantissa if that doesn't overwrite the
    // source.
    a.OpUBFE(exponent_dest, dxbc::Src::LU(4), dxbc::Src::LU(f24_shift + 20),
             dxbc::Src::R(f24_temp).Select(f24_temp_component));
  }
  // Unpack the mantissa.
  a.OpUBFE(mantissa_dest, dxbc::Src::LU(20), dxbc::Src::LU(f24_shift),
           dxbc::Src::R(f24_temp).Select(f24_temp_component));
  if (f24_temp == temp1_temp && f24_temp_component == temp1_temp_component) {
    // Unpack the exponent after the mantissa if doing that before the mantissa
    // would overwrite the source.
    a.OpUBFE(exponent_dest, dxbc::Src::LU(4), dxbc::Src::LU(f24_shift + 20),
             dxbc::Src::R(f24_temp).Select(f24_temp_component));
  }
  // Check if the number is denormalized.
  a.OpIf(false, exponent_src);
  {
    // Check if the number is non-zero (if the mantissa isn't zero - the
    // exponent is known to be zero at this point).
    a.OpIf(true, mantissa_src);
    {
      // Normalize the mantissa.
      // Note that HLSL firstbithigh(x) is compiled to DXBC like:
      // `x ? 31 - firstbit_hi(x) : -1`
      // (returns the index from the LSB, not the MSB, but -1 for zero too).
      // exponent = firstbit_hi(mantissa)
      a.OpFirstBitHi(exponent_dest, mantissa_src);
      // exponent = 20 - firstbithigh(mantissa)
      // Or:
      // exponent = 20 - (31 - firstbit_hi(mantissa))
      a.OpIAdd(exponent_dest, exponent_src, dxbc::Src::LI(20 - 31));
      // mantissa = mantissa << (20 - firstbithigh(mantissa))
      // AND 0xFFFFF not needed after this - BFI will do it.
      a.OpIShL(mantissa_dest, mantissa_src, exponent_src);
      // Get the normalized exponent.
      // exponent = 1 - (20 - firstbithigh(mantissa))
      a.OpIAdd(exponent_dest, dxbc::Src::LI(1), -exponent_src);
    }
    // The number is zero.
    a.OpElse();
    {
      // Set the unbiased exponent to -112 for zero - 112 will be added later
      // (taking the range remap bias into account), resulting in zero float32.
      a.OpMov(exponent_dest, dxbc::Src::LI(-int32_t(112 - remap_bias)));
    }
    // Close the non-zero check.
    a.OpEndIf();
  }
  // Close the denormal check.
  a.OpEndIf();
  // Bias the exponent and move it to the correct location in f32, and also
  // remap from guest 0...1 to host 0...0.5 if needed.
  a.OpIMAd(exponent_dest, exponent_src, dxbc::Src::LI(1 << 23),
           dxbc::Src::LI((112 - remap_bias) << 23));
  // Combine the mantissa and the exponent.
  a.OpBFI(f32, dxbc::Src::LU(20), dxbc::Src::LU(23 - 20), mantissa_src, exponent_src);
}

}  // namespace rex::graphics
