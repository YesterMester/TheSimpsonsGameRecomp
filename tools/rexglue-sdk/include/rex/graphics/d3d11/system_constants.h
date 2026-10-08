#pragma once

#include <rex/graphics/pipeline/shader/dxbc_translator.h>
#include <rex/graphics/pipeline/texture/cache.h>
#include <rex/graphics/util/draw.h>

namespace rex::graphics::d3d11 {

// Shared DXBC layouts with the host-render-target semantics used by the game.
void BuildGuestSystemConstants(
    DxbcShaderTranslator::SystemConstants& system_constants, const RegisterFile& regs,
    TextureCache& textures, bool gamma_as_unorm16, bool shared_memory_is_uav,
    bool primitive_polygonal, uint32_t line_loop_closing_index, xenos::Endian index_endian,
    const draw_util::ViewportInfo& viewport_info, uint32_t used_texture_mask,
    reg::RB_DEPTHCONTROL normalized_depth_control, uint32_t normalized_color_mask);

}  // namespace rex::graphics::d3d11
