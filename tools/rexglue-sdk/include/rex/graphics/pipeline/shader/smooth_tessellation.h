#pragma once

#include <cstdint>
#include <string_view>

namespace rex::graphics {

// Where a vertex shader keeps what PN-triangle tessellation needs: the vertex's
// world-space position and normal, written to interpolators, and the float
// constants that turn the same model-space vector into the clip position and
// into the world position.
struct SmoothTessellationLayout {
  // c[clip_constant + i] gives clip position component i (i = 0...3).
  uint32_t clip_constant = 0;
  // c[world_constant + i] gives world position and normal component i
  // (i = 0...2).
  uint32_t world_constant = 0;
  uint32_t position_interpolator = 0;
  uint32_t normal_interpolator = 0;
  // Reads float constants relatively, as the skinned meshes' bone matrices.
  bool skinned = false;
};

// Recognizes such a vertex shader from its microcode disassembly: four dp4
// writing oPos from c[n...n+3] and one register, three dp4 writing an
// interpolator's xyz from c[m...m+2] and the same register with the same
// swizzle, and three dp3 writing another interpolator's xyz from the same
// world constants. Everything around them is checked to run unconditionally
// and not to change the register in between.
bool FindSmoothTessellationLayout(std::string_view ucode_disassembly,
                                  SmoothTessellationLayout& layout_out);

}  // namespace rex::graphics
