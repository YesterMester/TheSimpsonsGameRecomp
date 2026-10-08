#pragma once

#include <rex/graphics/d3d11/draw_context.h>
#include <rex/graphics/util/draw.h>

namespace rex::graphics::d3d11 {

// Host-render-target state from the same normalized registers and viewport
// conversion used by the shader compiler. Returned NDC values must also be
// uploaded in the draw's system constants.
void BuildGuestDrawState(const RegisterFile& registers,
                         reg::RB_DEPTHCONTROL normalized_depth_control,
                         uint32_t normalized_color_mask, uint32_t bound_depth_and_color_mask,
                         uint32_t scale_x, uint32_t scale_y, bool pixel_shader_writes_depth,
                         bool convert_depth_to_float24, bool msaa_2x_supported,
                         draw_util::ViewportInfo& viewport, DrawState& state);

}  // namespace rex::graphics::d3d11
