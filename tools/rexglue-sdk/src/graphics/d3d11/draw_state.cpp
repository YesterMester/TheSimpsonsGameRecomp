/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * Native Direct3D 11 host render-target state for the ReXGlue runtime.
 */

#include <rex/graphics/d3d11/draw_state.h>

#include <algorithm>

namespace rex::graphics::d3d11 {

namespace {
D3D11_BLEND BlendFactor(xenos::BlendFactor factor, bool alpha) {
  switch (factor) {
    case xenos::BlendFactor::kOne:
      return D3D11_BLEND_ONE;
    case xenos::BlendFactor::kSrcColor:
      return alpha ? D3D11_BLEND_SRC_ALPHA : D3D11_BLEND_SRC_COLOR;
    case xenos::BlendFactor::kOneMinusSrcColor:
      return alpha ? D3D11_BLEND_INV_SRC_ALPHA : D3D11_BLEND_INV_SRC_COLOR;
    case xenos::BlendFactor::kSrcAlpha:
      return D3D11_BLEND_SRC_ALPHA;
    case xenos::BlendFactor::kOneMinusSrcAlpha:
      return D3D11_BLEND_INV_SRC_ALPHA;
    case xenos::BlendFactor::kDstColor:
      return alpha ? D3D11_BLEND_DEST_ALPHA : D3D11_BLEND_DEST_COLOR;
    case xenos::BlendFactor::kOneMinusDstColor:
      return alpha ? D3D11_BLEND_INV_DEST_ALPHA : D3D11_BLEND_INV_DEST_COLOR;
    case xenos::BlendFactor::kDstAlpha:
      return D3D11_BLEND_DEST_ALPHA;
    case xenos::BlendFactor::kOneMinusDstAlpha:
      return D3D11_BLEND_INV_DEST_ALPHA;
    case xenos::BlendFactor::kConstantColor:
    case xenos::BlendFactor::kConstantAlpha:
      return D3D11_BLEND_BLEND_FACTOR;
    case xenos::BlendFactor::kOneMinusConstantColor:
    case xenos::BlendFactor::kOneMinusConstantAlpha:
      return D3D11_BLEND_INV_BLEND_FACTOR;
    case xenos::BlendFactor::kSrcAlphaSaturate:
      return D3D11_BLEND_SRC_ALPHA_SAT;
    default:
      return D3D11_BLEND_ZERO;
  }
}
D3D11_BLEND_OP BlendOp(xenos::BlendOp operation) {
  switch (operation) {
    case xenos::BlendOp::kSubtract:
      return D3D11_BLEND_OP_SUBTRACT;
    case xenos::BlendOp::kMin:
      return D3D11_BLEND_OP_MIN;
    case xenos::BlendOp::kMax:
      return D3D11_BLEND_OP_MAX;
    case xenos::BlendOp::kRevSubtract:
      return D3D11_BLEND_OP_REV_SUBTRACT;
    default:
      return D3D11_BLEND_OP_ADD;
  }
}
}  // namespace

void BuildGuestDrawState(const RegisterFile& regs, reg::RB_DEPTHCONTROL depth,
                         uint32_t normalized_color_mask, uint32_t bound_mask, uint32_t scale_x,
                         uint32_t scale_y, bool pixel_shader_writes_depth,
                         bool convert_depth_to_float24, bool msaa_2x_supported,
                         draw_util::ViewportInfo& viewport, DrawState& state) {
  draw_util::GetHostViewportInfo(regs, scale_x, scale_y, true, D3D11_VIEWPORT_BOUNDS_MAX,
                                 D3D11_VIEWPORT_BOUNDS_MAX, false, depth, convert_depth_to_float24,
                                 true, pixel_shader_writes_depth, viewport);
  state = DrawState::Default(viewport.xy_extent[0], viewport.xy_extent[1]);
  state.viewport = {float(viewport.xy_offset[0]),
                    float(viewport.xy_offset[1]),
                    float(viewport.xy_extent[0]),
                    float(viewport.xy_extent[1]),
                    viewport.z_min,
                    viewport.z_max};
  draw_util::Scissor scissor;
  draw_util::GetScissor(regs, scissor);
  state.scissor = {LONG(scissor.offset[0] * scale_x), LONG(scissor.offset[1] * scale_y),
                   LONG((scissor.offset[0] + scissor.extent[0]) * scale_x),
                   LONG((scissor.offset[1] + scissor.extent[1]) * scale_y)};
  bool polygonal = draw_util::IsPrimitivePolygonal(regs);
  bool rasterization = draw_util::IsRasterizationPotentiallyDone(regs, polygonal);
  auto mode = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
  bool cull_front = polygonal && mode.cull_front;
  bool cull_back = polygonal && mode.cull_back;
  auto& raster = state.rasterizer;
  if (polygonal) {
    raster.FrontCounterClockwise = mode.face == 0;
    raster.CullMode = cull_front ? D3D11_CULL_FRONT : cull_back ? D3D11_CULL_BACK : D3D11_CULL_NONE;
    if (mode.poly_mode == xenos::PolygonModeEnable::kDualMode &&
        ((!cull_front && mode.polymode_front_ptype != xenos::PolygonType::kTriangles) ||
         (!cull_back && mode.polymode_back_ptype != xenos::PolygonType::kTriangles)))
      raster.FillMode = D3D11_FILL_WIREFRAME;
  }
  float bias, slope;
  draw_util::GetPreferredFacePolygonOffset(regs, polygonal, slope, bias);
  raster.DepthBias =
      draw_util::GetD3D10IntegerPolygonOffset(regs.Get<reg::RB_DEPTH_INFO>().depth_format, bias);
  raster.SlopeScaledDepthBias =
      slope * xenos::kPolygonOffsetScaleSubpixelUnit * float(std::max(scale_x, scale_y));
  raster.DepthClipEnable = !regs.Get<reg::PA_CL_CLIP_CNTL>().clip_disable;
  auto& native_depth = state.depth_stencil;
  if (rasterization && (bound_mask & 1)) {
    if (depth.z_enable) {
      native_depth.DepthEnable = TRUE;
      native_depth.DepthWriteMask =
          depth.z_write_enable ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
      native_depth.DepthFunc = D3D11_COMPARISON_FUNC(uint32_t(depth.zfunc) + 1);
    }
    if (depth.stencil_enable) {
      native_depth.StencilEnable = TRUE;
      bool backface = polygonal && depth.backface_enable;
      auto reference = regs.Get<reg::RB_STENCILREFMASK>(
          backface && cull_front ? XE_GPU_REG_RB_STENCILREFMASK_BF : XE_GPU_REG_RB_STENCILREFMASK);
      native_depth.StencilReadMask = reference.stencilmask;
      native_depth.StencilWriteMask = reference.stencilwritemask;
      state.stencil_reference = reference.stencilref;
      auto& front = native_depth.FrontFace;
      front.StencilFailOp = D3D11_STENCIL_OP(uint32_t(depth.stencilfail) + 1);
      front.StencilDepthFailOp = D3D11_STENCIL_OP(uint32_t(depth.stencilzfail) + 1);
      front.StencilPassOp = D3D11_STENCIL_OP(uint32_t(depth.stencilzpass) + 1);
      front.StencilFunc = D3D11_COMPARISON_FUNC(uint32_t(depth.stencilfunc) + 1);
      if (backface) {
        auto& back = native_depth.BackFace;
        back.StencilFailOp = D3D11_STENCIL_OP(uint32_t(depth.stencilfail_bf) + 1);
        back.StencilDepthFailOp = D3D11_STENCIL_OP(uint32_t(depth.stencilzfail_bf) + 1);
        back.StencilPassOp = D3D11_STENCIL_OP(uint32_t(depth.stencilzpass_bf) + 1);
        back.StencilFunc = D3D11_COMPARISON_FUNC(uint32_t(depth.stencilfunc_bf) + 1);
      } else {
        native_depth.BackFace = front;
      }
    }
  }
  for (uint32_t i = 0; i < 4; ++i) {
    auto& blend = state.blend.RenderTarget[i];
    blend.RenderTargetWriteMask = rasterization && (bound_mask & (2u << i))
                                      ? uint8_t((normalized_color_mask >> (4 * i)) & 15)
                                      : 0;
    if (!blend.RenderTargetWriteMask)
      continue;
    auto guest = regs.Get<reg::RB_BLENDCONTROL>(reg::RB_BLENDCONTROL::rt_register_indices[i]);
    blend.SrcBlend = BlendFactor(guest.color_srcblend, false);
    blend.DestBlend = BlendFactor(guest.color_destblend, false);
    blend.BlendOp = BlendOp(guest.color_comb_fcn);
    blend.SrcBlendAlpha = BlendFactor(guest.alpha_srcblend, true);
    blend.DestBlendAlpha = BlendFactor(guest.alpha_destblend, true);
    blend.BlendOpAlpha = BlendOp(guest.alpha_comb_fcn);
    blend.BlendEnable =
        blend.SrcBlend != D3D11_BLEND_ONE || blend.DestBlend != D3D11_BLEND_ZERO ||
        blend.BlendOp != D3D11_BLEND_OP_ADD || blend.SrcBlendAlpha != D3D11_BLEND_ONE ||
        blend.DestBlendAlpha != D3D11_BLEND_ZERO || blend.BlendOpAlpha != D3D11_BLEND_OP_ADD;
  }
  state.blend_factor = {
      regs.Get<float>(XE_GPU_REG_RB_BLEND_RED), regs.Get<float>(XE_GPU_REG_RB_BLEND_GREEN),
      regs.Get<float>(XE_GPU_REG_RB_BLEND_BLUE), regs.Get<float>(XE_GPU_REG_RB_BLEND_ALPHA)};
  if (!msaa_2x_supported &&
      regs.Get<reg::RB_SURFACE_INFO>().msaa_samples == xenos::MsaaSamples::k2X)
    state.sample_mask = 0b1001;
}

}  // namespace rex::graphics::d3d11
