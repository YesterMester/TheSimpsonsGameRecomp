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

// Preserve both integer-division results when implementations require matching
// output masks. The caller's original temporary registers retain their values.
class PortableDxbcAssembler : public dxbc::Assembler {
 public:
  PortableDxbcAssembler(std::vector<uint32_t>& code, dxbc::Statistics& statistics, bool portable)
      : dxbc::Assembler(code, statistics), portable_(portable) {}
  void OpDclTemps(uint32_t count) {
    scratch_base_ = count;
    dxbc::Assembler::OpDclTemps(count + (portable_ ? 2 : 0));
  }
  void OpUDiv(const dxbc::Dest& quotient, const dxbc::Dest& remainder, const dxbc::Src& numerator,
              const dxbc::Src& denominator) {
    if (!portable_ || !quotient.GetMask() || !remainder.GetMask() ||
        quotient.GetMask() == remainder.GetMask()) {
      dxbc::Assembler::OpUDiv(quotient, remainder, numerator, denominator);
      return;
    }
    dxbc::Assembler::OpUDiv(dxbc::Dest::R(scratch_base_), dxbc::Dest::R(scratch_base_ + 1),
                            numerator, denominator);
    OpMov(quotient, dxbc::Src::R(scratch_base_));
    OpMov(remainder, dxbc::Src::R(scratch_base_ + 1));
  }
  void OpLdMS(const dxbc::Dest& dest, const dxbc::Src& address, uint32_t address_mask,
              const dxbc::Src& resource, const dxbc::Src& sample_index, int32_t aoffimmi_u = 0,
              int32_t aoffimmi_v = 0) {
    if (!portable_) {
      dxbc::Assembler::OpLdMS(dest, address, address_mask, resource, sample_index, aoffimmi_u,
                              aoffimmi_v);
      return;
    }
    // These helpers read non-array Texture2DMS views. Match FXC's SM 5.0
    // address with zero in both unused components instead of repeating X.
    assert_true(address_mask == 0b0011);
    OpMov(dxbc::Dest::R(scratch_base_, 0b0011), address);
    OpMov(dxbc::Dest::R(scratch_base_, 0b1100), dxbc::Src::LU(0));
    dxbc::Assembler::OpLdMS(dest, dxbc::Src::R(scratch_base_), 0b1111, resource, sample_index,
                            aoffimmi_u, aoffimmi_v);
  }

 private:
  bool portable_;
  uint32_t scratch_base_ = 0;
};

}  // namespace rex::graphics
