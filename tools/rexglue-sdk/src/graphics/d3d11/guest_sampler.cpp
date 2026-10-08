/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * Native Direct3D 11 sampler state for the ReXGlue runtime.
 */

#include <rex/graphics/d3d11/guest_sampler.h>

#include <algorithm>
#include <cfloat>

#include <rex/graphics/pipeline/texture/util.h>

namespace rex::graphics::d3d11 {

D3D11_SAMPLER_DESC BuildGuestSamplerDescription(const RegisterFile& regs,
                                                const DxbcShader::SamplerBinding& binding,
                                                int32_t anisotropic_override) {
  auto fetch = regs.GetTextureFetch(binding.fetch_constant);
  D3D11_SAMPLER_DESC desc = {};
  xenos::ClampMode clamp_x, clamp_y, clamp_z;
  texture_util::GetClampModesForDimension(fetch, clamp_x, clamp_y, clamp_z);
  static constexpr D3D11_TEXTURE_ADDRESS_MODE address[] = {
      D3D11_TEXTURE_ADDRESS_WRAP,   D3D11_TEXTURE_ADDRESS_MIRROR,
      D3D11_TEXTURE_ADDRESS_CLAMP,  D3D11_TEXTURE_ADDRESS_MIRROR_ONCE,
      D3D11_TEXTURE_ADDRESS_CLAMP,  D3D11_TEXTURE_ADDRESS_MIRROR_ONCE,
      D3D11_TEXTURE_ADDRESS_BORDER, D3D11_TEXTURE_ADDRESS_MIRROR_ONCE};
  desc.AddressU = address[uint32_t(clamp_x)];
  desc.AddressV = address[uint32_t(clamp_y)];
  desc.AddressW = address[uint32_t(clamp_z)];
  xenos::BorderColor border = xenos::ClampModeUsesBorder(clamp_x) ||
                                      xenos::ClampModeUsesBorder(clamp_y) ||
                                      xenos::ClampModeUsesBorder(clamp_z)
                                  ? fetch.border_color
                                  : xenos::BorderColor::k_ABGR_Black;
  switch (border) {
    case xenos::BorderColor::k_ABGR_White:
      std::fill(std::begin(desc.BorderColor), std::end(desc.BorderColor), 1.0f);
      break;
    case xenos::BorderColor::k_ACBYCR_Black:
      desc.BorderColor[0] = desc.BorderColor[2] = 0.5f;
      break;
    case xenos::BorderColor::k_ACBCRY_Black:
      desc.BorderColor[1] = desc.BorderColor[2] = 0.5f;
      break;
    default:
      break;
  }
  uint32_t min_mip, max_mip;
  texture_util::GetSubresourcesFromFetchConstant(fetch, nullptr, nullptr, nullptr, nullptr, nullptr,
                                                 &min_mip, &max_mip);
  auto mag = binding.mag_filter == xenos::TextureFilter::kUseFetchConst ? fetch.mag_filter
                                                                        : binding.mag_filter;
  auto min = binding.min_filter == xenos::TextureFilter::kUseFetchConst ? fetch.min_filter
                                                                        : binding.min_filter;
  auto mip = binding.mip_filter == xenos::TextureFilter::kUseFetchConst ? fetch.mip_filter
                                                                        : binding.mip_filter;
  auto aniso = binding.aniso_filter == xenos::AnisoFilter::kUseFetchConst ? fetch.aniso_filter
                                                                          : binding.aniso_filter;
  if (anisotropic_override >= 0 && anisotropic_override < 6 && max_mip > min_mip &&
      mag == xenos::TextureFilter::kLinear && min == xenos::TextureFilter::kLinear &&
      (mip == xenos::TextureFilter::kPoint || mip == xenos::TextureFilter::kLinear))
    aniso = xenos::AnisoFilter(anisotropic_override);
  aniso = std::min(aniso, xenos::AnisoFilter::kMax_16_1);
  if (aniso != xenos::AnisoFilter::kDisabled) {
    desc.Filter = D3D11_FILTER_ANISOTROPIC;
    desc.MaxAnisotropy = 1u << (uint32_t(aniso) - 1);
  } else {
    desc.Filter = D3D11_ENCODE_BASIC_FILTER(
        min == xenos::TextureFilter::kLinear ? D3D11_FILTER_TYPE_LINEAR : D3D11_FILTER_TYPE_POINT,
        mag == xenos::TextureFilter::kLinear ? D3D11_FILTER_TYPE_LINEAR : D3D11_FILTER_TYPE_POINT,
        mip == xenos::TextureFilter::kLinear ? D3D11_FILTER_TYPE_LINEAR : D3D11_FILTER_TYPE_POINT,
        D3D11_FILTER_REDUCTION_TYPE_STANDARD);
    desc.MaxAnisotropy = 1;
  }
  desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
  desc.MinLOD = float(min_mip);
  desc.MaxLOD = mip == xenos::TextureFilter::kBaseMap
                    ? desc.MinLOD + (aniso == xenos::AnisoFilter::kDisabled ? 0.25f : 0.0f)
                    : FLT_MAX;
  return desc;
}

}  // namespace rex::graphics::d3d11
