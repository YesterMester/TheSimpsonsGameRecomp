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

 private:
  bool portable_;
  uint32_t scratch_base_ = 0;
};

}  // namespace rex::graphics
