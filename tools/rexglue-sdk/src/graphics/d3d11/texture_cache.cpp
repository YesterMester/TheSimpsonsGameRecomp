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

#include <rex/graphics/d3d11/texture_cache.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>

#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/logging.h>
#include <rex/math.h>

namespace rex::graphics::d3d11 {
using Microsoft::WRL::ComPtr;

namespace {
constexpr bool AreDimensionsCompatible(xenos::FetchOpDimension binding_dimension,
                                       xenos::DataDimension resource_dimension) {
  switch (binding_dimension) {
    case xenos::FetchOpDimension::k1D:
    case xenos::FetchOpDimension::k2D:
      return resource_dimension == xenos::DataDimension::k1D ||
             resource_dimension == xenos::DataDimension::k2DOrStacked ||
             resource_dimension == xenos::DataDimension::k3D;
    case xenos::FetchOpDimension::k3DOrStacked:
      return resource_dimension == xenos::DataDimension::k3D;
    case xenos::FetchOpDimension::kCube:
      return resource_dimension == xenos::DataDimension::kCube;
    default:
      return false;
  }
}
}  // namespace

namespace shaders {
#include "../shaders/bytecode/d3d12_5_1/texture_load_128bpb_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_16bpb_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_32bpb_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_64bpb_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_8bpb_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_bgrg8_rgb8_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_bgrg8_rgbg8_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_ctx1_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_depth_float_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_depth_unorm_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_dxn_rg8_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_dxt1_rgba8_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_dxt3_rgba8_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_dxt3a_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_dxt3aas1111_bgra4_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_dxt5_rgba8_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_dxt5a_r8_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_gbgr8_grgb8_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_gbgr8_rgb8_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_r10g11b11_rgba16_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_r10g11b11_rgba16_snorm_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_r11g11b10_rgba16_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_r11g11b10_rgba16_snorm_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_r4g4b4a4_b4g4r4a4_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_r5g5b5a1_b5g5r5a1_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_r5g5b6_b5g6r5_swizzle_rbga_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_r5g6b5_b5g6r5_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_128bpb_scaled_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_16bpb_scaled_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_32bpb_scaled_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_64bpb_scaled_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_8bpb_scaled_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_depth_float_scaled_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_depth_unorm_scaled_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_r10g11b11_rgba16_scaled_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_r10g11b11_rgba16_snorm_scaled_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_r11g11b10_rgba16_scaled_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_r11g11b10_rgba16_snorm_scaled_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_r4g4b4a4_b4g4r4a4_scaled_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_r5g5b5a1_b5g5r5a1_scaled_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_r5g5b6_b5g6r5_swizzle_rbga_scaled_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_r5g6b5_b5g6r5_scaled_cs.h"
}  // namespace shaders

const D3D11TextureCache::HostFormat D3D11TextureCache::host_formats_[64] = {
    // k_1_REVERSE
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_1
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_8
    {DXGI_FORMAT_R8_TYPELESS, DXGI_FORMAT_R8_UNORM, kLoadShaderIndex8bpb, DXGI_FORMAT_R8_SNORM,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_1_5_5_5
    // Red and blue swapped in the load shader for simplicity.
    {DXGI_FORMAT_B5G5R5A1_UNORM, DXGI_FORMAT_B5G5R5A1_UNORM, kLoadShaderIndexR5G5B5A1ToB5G5R5A1,
     DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_5_6_5
    // Red and blue swapped in the load shader for simplicity.
    {DXGI_FORMAT_B5G6R5_UNORM, DXGI_FORMAT_B5G6R5_UNORM, kLoadShaderIndexR5G6B5ToB5G6R5,
     DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBB},
    // k_6_5_5
    // On the host, green bits in blue, blue bits in green.
    {DXGI_FORMAT_B5G6R5_UNORM, DXGI_FORMAT_B5G6R5_UNORM,
     kLoadShaderIndexR5G5B6ToB5G6R5WithRBGASwizzle, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, XE_GPU_MAKE_TEXTURE_SWIZZLE(R, B, G, G)},
    // k_8_8_8_8
    {DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM, kLoadShaderIndex32bpb,
     DXGI_FORMAT_R8G8B8A8_SNORM, kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_2_10_10_10
    {DXGI_FORMAT_R10G10B10A2_TYPELESS, DXGI_FORMAT_R10G10B10A2_UNORM, kLoadShaderIndex32bpb,
     DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_8_A
    {DXGI_FORMAT_R8_TYPELESS, DXGI_FORMAT_R8_UNORM, kLoadShaderIndex8bpb, DXGI_FORMAT_R8_SNORM,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_8_B
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_8_8
    {DXGI_FORMAT_R8G8_TYPELESS, DXGI_FORMAT_R8G8_UNORM, kLoadShaderIndex16bpb,
     DXGI_FORMAT_R8G8_SNORM, kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG},
    // k_Cr_Y1_Cb_Y0_REP
    // Red and blue swapped in the load shader for simplicity.
    // TODO(Triang3l): The DXGI_FORMAT_R8G8B8A8_U/SNORM conversion is usable for
    // the signed version, separate unsigned and signed load shaders completely
    // (as one doesn't need decompression for this format, while another does).
    {DXGI_FORMAT_G8R8_G8B8_UNORM, DXGI_FORMAT_G8R8_G8B8_UNORM, kLoadShaderIndexGBGR8ToGRGB8,
     DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, true, DXGI_FORMAT_R8G8B8A8_UNORM,
     kLoadShaderIndexGBGR8ToRGB8, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBB},
    // k_Y1_Cr_Y0_Cb_REP
    // Red and blue swapped in the load shader for simplicity.
    // TODO(Triang3l): The DXGI_FORMAT_R8G8B8A8_U/SNORM conversion is usable for
    // the signed version, separate unsigned and signed load shaders completely
    // (as one doesn't need decompression for this format, while another does).
    {DXGI_FORMAT_R8G8_B8G8_UNORM, DXGI_FORMAT_R8G8_B8G8_UNORM, kLoadShaderIndexBGRG8ToRGBG8,
     DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, true, DXGI_FORMAT_R8G8B8A8_UNORM,
     kLoadShaderIndexBGRG8ToRGB8, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBB},
    // k_16_16_EDRAM
    // Not usable as a texture, also has -32...32 range.
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG},
    // k_8_8_8_8_A
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_4_4_4_4
    // Red and blue swapped in the load shader for simplicity.
    {DXGI_FORMAT_B4G4R4A4_UNORM, DXGI_FORMAT_B4G4R4A4_UNORM, kLoadShaderIndexRGBA4ToBGRA4,
     DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_10_11_11
    {DXGI_FORMAT_R16G16B16A16_TYPELESS, DXGI_FORMAT_R16G16B16A16_UNORM,
     kLoadShaderIndexR11G11B10ToRGBA16, DXGI_FORMAT_R16G16B16A16_SNORM,
     kLoadShaderIndexR11G11B10ToRGBA16SNorm, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBB},
    // k_11_11_10
    {DXGI_FORMAT_R16G16B16A16_TYPELESS, DXGI_FORMAT_R16G16B16A16_UNORM,
     kLoadShaderIndexR10G11B11ToRGBA16, DXGI_FORMAT_R16G16B16A16_SNORM,
     kLoadShaderIndexR10G11B11ToRGBA16SNorm, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBB},
    // k_DXT1
    {DXGI_FORMAT_BC1_UNORM, DXGI_FORMAT_BC1_UNORM, kLoadShaderIndex64bpb, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, true, DXGI_FORMAT_R8G8B8A8_UNORM, kLoadShaderIndexDXT1ToRGBA8,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_DXT2_3
    {DXGI_FORMAT_BC2_UNORM, DXGI_FORMAT_BC2_UNORM, kLoadShaderIndex128bpb, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, true, DXGI_FORMAT_R8G8B8A8_UNORM, kLoadShaderIndexDXT3ToRGBA8,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_DXT4_5
    {DXGI_FORMAT_BC3_UNORM, DXGI_FORMAT_BC3_UNORM, kLoadShaderIndex128bpb, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, true, DXGI_FORMAT_R8G8B8A8_UNORM, kLoadShaderIndexDXT5ToRGBA8,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_16_16_16_16_EDRAM
    // Not usable as a texture, also has -32...32 range.
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // R32_FLOAT for depth because shaders would require an additional SRV to
    // sample stencil, which we don't provide.
    // k_24_8
    {DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_FLOAT, kLoadShaderIndexDepthUnorm,
     DXGI_FORMAT_R32_FLOAT, kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_24_8_FLOAT
    {DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_FLOAT, kLoadShaderIndexDepthFloat,
     DXGI_FORMAT_R32_FLOAT, kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_16
    {DXGI_FORMAT_R16_TYPELESS, DXGI_FORMAT_R16_UNORM, kLoadShaderIndex16bpb, DXGI_FORMAT_R16_SNORM,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_16_16
    {DXGI_FORMAT_R16G16_TYPELESS, DXGI_FORMAT_R16G16_UNORM, kLoadShaderIndex32bpb,
     DXGI_FORMAT_R16G16_SNORM, kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG},
    // k_16_16_16_16
    {DXGI_FORMAT_R16G16B16A16_TYPELESS, DXGI_FORMAT_R16G16B16A16_UNORM, kLoadShaderIndex64bpb,
     DXGI_FORMAT_R16G16B16A16_SNORM, kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_16_EXPAND
    {DXGI_FORMAT_R16_FLOAT, DXGI_FORMAT_R16_FLOAT, kLoadShaderIndex16bpb, DXGI_FORMAT_R16_FLOAT,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_16_16_EXPAND
    {DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R16G16_FLOAT, kLoadShaderIndex32bpb,
     DXGI_FORMAT_R16G16_FLOAT, kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG},
    // k_16_16_16_16_EXPAND
    {DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT, kLoadShaderIndex64bpb,
     DXGI_FORMAT_R16G16B16A16_FLOAT, kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_16_FLOAT
    {DXGI_FORMAT_R16_FLOAT, DXGI_FORMAT_R16_FLOAT, kLoadShaderIndex16bpb, DXGI_FORMAT_R16_FLOAT,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_16_16_FLOAT
    {DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R16G16_FLOAT, kLoadShaderIndex32bpb,
     DXGI_FORMAT_R16G16_FLOAT, kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG},
    // k_16_16_16_16_FLOAT
    {DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT, kLoadShaderIndex64bpb,
     DXGI_FORMAT_R16G16B16A16_FLOAT, kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_32
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_32_32
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG},
    // k_32_32_32_32
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_32_FLOAT
    {DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_FLOAT, kLoadShaderIndex32bpb, DXGI_FORMAT_R32_FLOAT,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_32_32_FLOAT
    {DXGI_FORMAT_R32G32_FLOAT, DXGI_FORMAT_R32G32_FLOAT, kLoadShaderIndex64bpb,
     DXGI_FORMAT_R32G32_FLOAT, kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG},
    // k_32_32_32_32_FLOAT
    {DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT, kLoadShaderIndex128bpb,
     DXGI_FORMAT_R32G32B32A32_FLOAT, kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_32_AS_8
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_32_AS_8_8
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG},
    // k_16_MPEG
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_16_16_MPEG
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG},
    // k_8_INTERLACED
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_32_AS_8_INTERLACED
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_32_AS_8_8_INTERLACED
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG},
    // k_16_INTERLACED
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_16_MPEG_INTERLACED
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_16_16_MPEG_INTERLACED
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG},
    // k_DXN
    {DXGI_FORMAT_BC5_UNORM, DXGI_FORMAT_BC5_UNORM, kLoadShaderIndex128bpb, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, true, DXGI_FORMAT_R8G8_UNORM, kLoadShaderIndexDXNToRG8,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG},
    // k_8_8_8_8_AS_16_16_16_16
    {DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM, kLoadShaderIndex32bpb,
     DXGI_FORMAT_R8G8B8A8_SNORM, kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_DXT1_AS_16_16_16_16
    {DXGI_FORMAT_BC1_UNORM, DXGI_FORMAT_BC1_UNORM, kLoadShaderIndex64bpb, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, true, DXGI_FORMAT_R8G8B8A8_UNORM, kLoadShaderIndexDXT1ToRGBA8,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_DXT2_3_AS_16_16_16_16
    {DXGI_FORMAT_BC2_UNORM, DXGI_FORMAT_BC2_UNORM, kLoadShaderIndex128bpb, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, true, DXGI_FORMAT_R8G8B8A8_UNORM, kLoadShaderIndexDXT3ToRGBA8,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_DXT4_5_AS_16_16_16_16
    {DXGI_FORMAT_BC3_UNORM, DXGI_FORMAT_BC3_UNORM, kLoadShaderIndex128bpb, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, true, DXGI_FORMAT_R8G8B8A8_UNORM, kLoadShaderIndexDXT5ToRGBA8,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_2_10_10_10_AS_16_16_16_16
    {DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_R10G10B10A2_UNORM, kLoadShaderIndex32bpb,
     DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_10_11_11_AS_16_16_16_16
    {DXGI_FORMAT_R16G16B16A16_TYPELESS, DXGI_FORMAT_R16G16B16A16_UNORM,
     kLoadShaderIndexR11G11B10ToRGBA16, DXGI_FORMAT_R16G16B16A16_SNORM,
     kLoadShaderIndexR11G11B10ToRGBA16SNorm, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBB},
    // k_11_11_10_AS_16_16_16_16
    {DXGI_FORMAT_R16G16B16A16_TYPELESS, DXGI_FORMAT_R16G16B16A16_UNORM,
     kLoadShaderIndexR10G11B11ToRGBA16, DXGI_FORMAT_R16G16B16A16_SNORM,
     kLoadShaderIndexR10G11B11ToRGBA16SNorm, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBB},
    // k_32_32_32_FLOAT
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBB},
    // k_DXT3A
    // R8_UNORM has the same size as BC2, but doesn't have the 4x4 size
    // alignment requirement.
    {DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8_UNORM, kLoadShaderIndexDXT3A, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_DXT5A
    {DXGI_FORMAT_BC4_UNORM, DXGI_FORMAT_BC4_UNORM, kLoadShaderIndex64bpb, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, true, DXGI_FORMAT_R8_UNORM, kLoadShaderIndexDXT5AToR8,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_CTX1
    {DXGI_FORMAT_R8G8_UNORM, DXGI_FORMAT_R8G8_UNORM, kLoadShaderIndexCTX1, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG},
    // k_DXT3A_AS_1_1_1_1
    {DXGI_FORMAT_B4G4R4A4_UNORM, DXGI_FORMAT_B4G4R4A4_UNORM, kLoadShaderIndexDXT3AAs1111ToBGRA4,
     DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_8_8_8_8_GAMMA_EDRAM
    // Not usable as a texture.
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_2_10_10_10_FLOAT_EDRAM
    // Not usable as a texture.
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
};

D3D11TextureCache::D3D11TextureCache(const RegisterFile& registers, D3D11SharedMemory& memory,
                                     ui::d3d11::D3D11Device& device, DrawContext& draws,
                                     uint32_t resolution_scale_x, uint32_t resolution_scale_y)
    : TextureCache(registers, memory, resolution_scale_x, resolution_scale_y),
      native_memory_(memory),
      device_(device),
      draws_(draws),
      shaders_(device),
      uploader_(device, draws),
      scaled_memory_(device, draws, resolution_scale_x, resolution_scale_y) {}

bool D3D11TextureCache::EnsureScaledResolveMemoryCommitted(uint32_t start, uint32_t length,
                                                           uint32_t alignment_log2) {
  if (alignment_log2 > 4) {
    last_error_ = "Unsupported native scaled resolve alignment";
    return false;
  }
  // Page-aligned allocations also satisfy the common decoder's 16-byte
  // padding requirement. Preserve their contents when only image caches clear.
  return scaled_memory_.Ensure(start, length, last_error_);
}

ComPtr<ID3D11UnorderedAccessView> D3D11TextureCache::ScaledResolveDestination(
    uint32_t start, uint32_t length, uint32_t element_size_log2) {
  return scaled_memory_.Write(start, length, element_size_log2, last_error_);
}

Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> D3D11TextureCache::RequestSwapTexture(
    xenos::TextureFormat& format, uint32_t& swizzle, uint32_t& width, uint32_t& height,
    bool& scaled) {
  last_error_.clear();
  auto fetch = register_file().GetTextureFetch(0);
  TextureKey key;
  BindingInfoFromFetchConstant(fetch, key, nullptr);
  if (!key.is_valid || !key.base_page || key.dimension != xenos::DataDimension::k2DOrStacked ||
      key.GetDepthOrArraySize() != 1) {
    last_error_ = "Native swap fetch is not a valid single 2D front buffer";
    return {};
  }
  auto* texture = static_cast<D3D11Texture*>(FindOrCreateTexture(key));
  if (!texture || !LoadTextureData(*texture))
    return {};
  D3D11_SHADER_RESOURCE_VIEW_DESC desc = {};
  desc.Format = texture->format.unsigned_view;
  desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
  desc.Texture2D.MipLevels = 1;
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view;
  if (Failed(device_.device()->CreateShaderResourceView(texture->resource.Get(), &desc,
                                                        view.GetAddressOf()),
             "Native swap image view"))
    return {};
  texture->MarkAsUsed();
  format = texture->key().format;
  swizzle = GuestToHostSwizzle(fetch.swizzle, GetHostFormatSwizzle(texture->key()));
  if (texture->content_red_blue_swapped)
    swizzle = SwapRedBlueSwizzle(swizzle);
  scaled = texture->key().scaled_resolve;
  width = texture->key().GetWidth() * (scaled ? draw_resolution_scale_x() : 1);
  height = texture->key().GetHeight() * (scaled ? draw_resolution_scale_y() : 1);
  return view;
}

D3D11TextureCache::~D3D11TextureCache() {
  ClearCache();
}

void D3D11TextureCache::ClearCache() {
  draws_.Invalidate();
  TextureCache::ClearCache();
  uploader_.Clear();
  shaders_.Clear();
  load_programs_ = {};
  load_attempted_ = {};
  load_errors_ = {};
  last_error_.clear();
}

void D3D11TextureCache::RequestTextures(uint32_t used_texture_mask) {
  last_error_.clear();
  TextureCache::RequestTextures(used_texture_mask);
}

bool D3D11TextureCache::Failed(HRESULT result, const char* operation) {
  if (SUCCEEDED(result))
    return false;
  char message[192];
  std::snprintf(message, sizeof(message), "%s failed (0x%08X)", operation, unsigned(result));
  last_error_ = message;
  return true;
}

D3D11TextureCache::NativeFormat D3D11TextureCache::GetNativeFormat(TextureKey key) {
  const HostFormat& host = host_formats_[uint32_t(key.format)];
  NativeFormat result;
  result.unsigned_view = host.unsigned_view;
  result.signed_view = host.signed_view;
  result.load_shader = key.signed_separate ? host.signed_load_shader : host.load_shader;
  DXGI_FORMAT family = host.resource;
  // D3D11 cannot copy a decoded GPU buffer into an image. Decode compressed
  // and subsampled textures before the native typed-UAV image transfer.
  if (!key.signed_separate && host.decompressed_view != DXGI_FORMAT_UNKNOWN) {
    family = result.unsigned_view = host.decompressed_view;
    result.signed_view = DXGI_FORMAT_UNKNOWN;
    result.load_shader = host.decompress_shader;
  }
  switch (family) {
    case DXGI_FORMAT_B5G6R5_UNORM:
    case DXGI_FORMAT_B5G5R5A1_UNORM:
    case DXGI_FORMAT_B4G4R4A4_UNORM:
      result.resource = DXGI_FORMAT_R32G32B32A32_TYPELESS;
      result.unsigned_view = DXGI_FORMAT_R32G32B32A32_FLOAT;
      result.signed_view = DXGI_FORMAT_UNKNOWN;
      result.storage_view = DXGI_FORMAT_R32G32B32A32_UINT;
      result.packed_source =
          family == DXGI_FORMAT_B5G6R5_UNORM     ? TextureUpload::PackedSource::kB5G6R5
          : family == DXGI_FORMAT_B5G5R5A1_UNORM ? TextureUpload::PackedSource::kB5G5R5A1
                                                 : TextureUpload::PackedSource::kB4G4R4A4;
      break;
    case DXGI_FORMAT_R8_TYPELESS:
    case DXGI_FORMAT_R8_UNORM:
      result.resource = DXGI_FORMAT_R8_TYPELESS;
      result.storage_view = DXGI_FORMAT_R8_UINT;
      break;
    case DXGI_FORMAT_R8G8_TYPELESS:
    case DXGI_FORMAT_R8G8_UNORM:
      result.resource = DXGI_FORMAT_R8G8_TYPELESS;
      result.storage_view = DXGI_FORMAT_R8G8_UINT;
      break;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
      result.resource = DXGI_FORMAT_R8G8B8A8_TYPELESS;
      result.storage_view = DXGI_FORMAT_R8G8B8A8_UINT;
      break;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
      result.resource = DXGI_FORMAT_R10G10B10A2_TYPELESS;
      result.storage_view = DXGI_FORMAT_R10G10B10A2_UINT;
      break;
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_R16_UNORM:
    case DXGI_FORMAT_R16_FLOAT:
      result.resource = DXGI_FORMAT_R16_TYPELESS;
      result.storage_view = DXGI_FORMAT_R16_UINT;
      break;
    case DXGI_FORMAT_R16G16_TYPELESS:
    case DXGI_FORMAT_R16G16_FLOAT:
      result.resource = DXGI_FORMAT_R16G16_TYPELESS;
      result.storage_view = DXGI_FORMAT_R16G16_UINT;
      break;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
      result.resource = DXGI_FORMAT_R16G16B16A16_TYPELESS;
      result.storage_view = DXGI_FORMAT_R16G16B16A16_UINT;
      break;
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_R32_FLOAT:
      result.resource = DXGI_FORMAT_R32_TYPELESS;
      result.storage_view = DXGI_FORMAT_R32_UINT;
      break;
    case DXGI_FORMAT_R32G32_TYPELESS:
    case DXGI_FORMAT_R32G32_FLOAT:
      result.resource = DXGI_FORMAT_R32G32_TYPELESS;
      result.storage_view = DXGI_FORMAT_R32G32_UINT;
      break;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
      result.resource = DXGI_FORMAT_R32G32B32A32_TYPELESS;
      result.storage_view = DXGI_FORMAT_R32G32B32A32_UINT;
      break;
    default:
      result.load_shader = kLoadShaderIndexUnknown;
      break;
  }
  return result;
}

bool D3D11TextureCache::IsSignedVersionSeparateForFormat(TextureKey key) const {
  const HostFormat& host = host_formats_[uint32_t(key.format)];
  return host.signed_load_shader != kLoadShaderIndexUnknown &&
         host.signed_load_shader != host.load_shader;
}

bool D3D11TextureCache::HasScaledLoadProgram(LoadShaderIndex index) {
  switch (index) {
    case kLoadShaderIndex8bpb:
    case kLoadShaderIndex16bpb:
    case kLoadShaderIndex32bpb:
    case kLoadShaderIndex64bpb:
    case kLoadShaderIndex128bpb:
    case kLoadShaderIndexR5G6B5ToB5G6R5:
    case kLoadShaderIndexR5G5B5A1ToB5G5R5A1:
    case kLoadShaderIndexR5G5B6ToB5G6R5WithRBGASwizzle:
    case kLoadShaderIndexRGBA4ToBGRA4:
    case kLoadShaderIndexR10G11B11ToRGBA16:
    case kLoadShaderIndexR10G11B11ToRGBA16SNorm:
    case kLoadShaderIndexR11G11B10ToRGBA16:
    case kLoadShaderIndexR11G11B10ToRGBA16SNorm:
    case kLoadShaderIndexDepthUnorm:
    case kLoadShaderIndexDepthFloat:
      return true;
    default:
      return false;
  }
}

bool D3D11TextureCache::IsScaledResolveSupportedForFormat(TextureKey key) const {
  return HasScaledLoadProgram(GetNativeFormat(key).load_shader);
}

uint32_t D3D11TextureCache::GetHostFormatSwizzle(TextureKey key) const {
  return host_formats_[uint32_t(key.format)].swizzle;
}

uint32_t D3D11TextureCache::GetMaxHostTextureWidthHeight(xenos::DataDimension dimension) const {
  return dimension == xenos::DataDimension::k3D ? D3D11_REQ_TEXTURE3D_U_V_OR_W_DIMENSION
                                                : D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION;
}

uint32_t D3D11TextureCache::GetMaxHostTextureDepthOrArraySize(
    xenos::DataDimension dimension) const {
  if (dimension == xenos::DataDimension::k3D)
    return D3D11_REQ_TEXTURE3D_U_V_OR_W_DIMENSION;
  if (dimension == xenos::DataDimension::kCube)
    return D3D11_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION / 6 * 6;
  return D3D11_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION;
}

D3D11TextureCache::D3D11Texture::D3D11Texture(D3D11TextureCache& cache, TextureKey key,
                                              NativeFormat native_format,
                                              ComPtr<ID3D11Resource> native_resource)
    : Texture(cache, key), format(native_format), resource(std::move(native_resource)) {
  uint64_t size = 0;
  bool volume = key.dimension == xenos::DataDimension::k3D;
  for (uint32_t mip = 0; mip <= key.mip_max_level; ++mip) {
    size += uint64_t(std::max(key.GetWidth() >> mip, 1u)) * std::max(key.GetHeight() >> mip, 1u) *
            (volume ? std::max(key.GetDepthOrArraySize() >> mip, 1u) : key.GetDepthOrArraySize()) *
            (format.packed_source == TextureUpload::PackedSource::kNone
                 ? GetLoadShaderInfo(format.load_shader).bytes_per_host_block
                 : 16);
  }
  // D3D11 needs a separate image for the guest's 2D view of a volume texture.
  if (volume) {
    size += uint64_t(key.GetWidth()) * key.GetHeight() *
            (format.packed_source == TextureUpload::PackedSource::kNone
                 ? GetLoadShaderInfo(format.load_shader).bytes_per_host_block
                 : 16);
  }
  if (key.scaled_resolve)
    size *= cache.draw_resolution_scale_x() * cache.draw_resolution_scale_y();
  SetHostMemoryUsage(size);
}

std::unique_ptr<TextureCache::Texture> D3D11TextureCache::CreateTexture(TextureKey key) {
  NativeFormat format = GetNativeFormat(key);
  if (format.resource == DXGI_FORMAT_UNKNOWN || format.load_shader == kLoadShaderIndexUnknown ||
      (key.scaled_resolve && !HasScaledLoadProgram(format.load_shader))) {
    last_error_ = "The DX11 guest texture has an unsupported storage format or scaled source";
    return nullptr;
  }
  ComPtr<ID3D11Resource> resource;
  bool volume = key.dimension == xenos::DataDimension::k3D;
  uint32_t levels = key.mip_max_level + 1;
  uint32_t width = key.GetWidth() * (key.scaled_resolve ? draw_resolution_scale_x() : 1);
  uint32_t height = key.GetHeight() * (key.scaled_resolve ? draw_resolution_scale_y() : 1);
  if (volume) {
    D3D11_TEXTURE3D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.Depth = key.GetDepthOrArraySize();
    desc.MipLevels = levels;
    desc.Format = format.resource;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    ComPtr<ID3D11Texture3D> texture;
    if (Failed(device_.device()->CreateTexture3D(&desc, nullptr, texture.GetAddressOf()),
               "Native guest volume creation"))
      return nullptr;
    resource = texture;
  } else {
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.ArraySize = key.GetDepthOrArraySize();
    desc.MipLevels = levels;
    desc.Format = format.resource;
    desc.SampleDesc.Count = 1;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    if (key.dimension == xenos::DataDimension::kCube)
      desc.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;
    ComPtr<ID3D11Texture2D> texture;
    if (Failed(device_.device()->CreateTexture2D(&desc, nullptr, texture.GetAddressOf()),
               "Native guest image creation"))
      return nullptr;
    resource = texture;
  }
  auto texture = std::make_unique<D3D11Texture>(*this, key, format, resource);
  if (volume) {
    // The guest's 1D/2D view of a volume is its first base-level slice.
    // D3D11 can't reinterpret a Texture3D as Texture2DArray, so retain a
    // separately owned image populated by the same GPU decode buffer.
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = format.resource;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    if (Failed(
            device_.device()->CreateTexture2D(&desc, nullptr, texture->volume_slice.GetAddressOf()),
            "Native volume slice image"))
      return nullptr;
    D3D11_UNORDERED_ACCESS_VIEW_DESC view = {};
    view.Format = format.storage_view;
    view.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
    view.Texture2DArray.ArraySize = 1;
    if (Failed(device_.device()->CreateUnorderedAccessView(
                   texture->volume_slice.Get(), &view, texture->volume_slice_write.GetAddressOf()),
               "Native volume slice writes"))
      return nullptr;
  }
  texture->mip_views.resize(levels);
  draws_.Invalidate();
  const uint32_t zero[4] = {};
  if (texture->volume_slice_write)
    device_.context()->ClearUnorderedAccessViewUint(texture->volume_slice_write.Get(), zero);
  for (uint32_t mip = 0; mip < levels; ++mip) {
    D3D11_UNORDERED_ACCESS_VIEW_DESC desc = {};
    desc.Format = format.storage_view;
    if (volume) {
      desc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE3D;
      desc.Texture3D.MipSlice = mip;
      desc.Texture3D.WSize = std::max(key.GetDepthOrArraySize() >> mip, 1u);
    } else {
      desc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
      desc.Texture2DArray.MipSlice = mip;
      desc.Texture2DArray.ArraySize = key.GetDepthOrArraySize();
    }
    if (Failed(device_.device()->CreateUnorderedAccessView(resource.Get(), &desc,
                                                           texture->mip_views[mip].GetAddressOf()),
               "Native guest mip storage view"))
      return nullptr;
    device_.context()->ClearUnorderedAccessViewUint(texture->mip_views[mip].Get(), zero);
  }
  return texture;
}

const ShaderProgram* D3D11TextureCache::GetLoadProgram(LoadShaderIndex index, bool scaled) {
  if (load_attempted_[scaled][index]) {
    last_error_ = load_errors_[scaled][index];
    return load_programs_[scaled][index];
  }
  load_attempted_[scaled][index] = true;
  std::span<const uint8_t> code;
#define REX_D3D11_TEXTURE_LOAD(index_name, shader_name)          \
  case index_name:                                               \
    code = {shaders::shader_name, sizeof(shaders::shader_name)}; \
    break
  if (scaled) {
    switch (index) {
      REX_D3D11_TEXTURE_LOAD(kLoadShaderIndex8bpb, texture_load_8bpb_scaled_cs);
      REX_D3D11_TEXTURE_LOAD(kLoadShaderIndex16bpb, texture_load_16bpb_scaled_cs);
      REX_D3D11_TEXTURE_LOAD(kLoadShaderIndex32bpb, texture_load_32bpb_scaled_cs);
      REX_D3D11_TEXTURE_LOAD(kLoadShaderIndex64bpb, texture_load_64bpb_scaled_cs);
      REX_D3D11_TEXTURE_LOAD(kLoadShaderIndex128bpb, texture_load_128bpb_scaled_cs);
      REX_D3D11_TEXTURE_LOAD(kLoadShaderIndexR5G6B5ToB5G6R5, texture_load_r5g6b5_b5g6r5_scaled_cs);
      REX_D3D11_TEXTURE_LOAD(kLoadShaderIndexR5G5B5A1ToB5G5R5A1,
                             texture_load_r5g5b5a1_b5g5r5a1_scaled_cs);
      REX_D3D11_TEXTURE_LOAD(kLoadShaderIndexR5G5B6ToB5G6R5WithRBGASwizzle,
                             texture_load_r5g5b6_b5g6r5_swizzle_rbga_scaled_cs);
      REX_D3D11_TEXTURE_LOAD(kLoadShaderIndexRGBA4ToBGRA4,
                             texture_load_r4g4b4a4_b4g4r4a4_scaled_cs);
      REX_D3D11_TEXTURE_LOAD(kLoadShaderIndexR10G11B11ToRGBA16,
                             texture_load_r10g11b11_rgba16_scaled_cs);
      REX_D3D11_TEXTURE_LOAD(kLoadShaderIndexR10G11B11ToRGBA16SNorm,
                             texture_load_r10g11b11_rgba16_snorm_scaled_cs);
      REX_D3D11_TEXTURE_LOAD(kLoadShaderIndexR11G11B10ToRGBA16,
                             texture_load_r11g11b10_rgba16_scaled_cs);
      REX_D3D11_TEXTURE_LOAD(kLoadShaderIndexR11G11B10ToRGBA16SNorm,
                             texture_load_r11g11b10_rgba16_snorm_scaled_cs);
      REX_D3D11_TEXTURE_LOAD(kLoadShaderIndexDepthUnorm, texture_load_depth_unorm_scaled_cs);
      REX_D3D11_TEXTURE_LOAD(kLoadShaderIndexDepthFloat, texture_load_depth_float_scaled_cs);
      default:
        break;
    }
  } else {
    switch (index) {
      REX_D3D11_TEXTURE_LOAD(kLoadShaderIndex8bpb, texture_load_8bpb_cs);
      REX_D3D11_TEXTURE_LOAD(kLoadShaderIndex16bpb, texture_load_16bpb_cs);
      REX_D3D11_TEXTURE_LOAD(kLoadShaderIndex32bpb, texture_load_32bpb_cs);
      REX_D3D11_TEXTURE_LOAD(kLoadShaderIndex64bpb, texture_load_64bpb_cs);
      REX_D3D11_TEXTURE_LOAD(kLoadShaderIndex128bpb, texture_load_128bpb_cs);
      REX_D3D11_TEXTURE_LOAD(kLoadShaderIndexR5G6B5ToB5G6R5, texture_load_r5g6b5_b5g6r5_cs);
      REX_D3D11_TEXTURE_LOAD(kLoadShaderIndexR5G5B5A1ToB5G5R5A1, texture_load_r5g5b5a1_b5g5r5a1_cs);
      REX_D3D11_TEXTURE_LOAD(kLoadShaderIndexR5G5B6ToB5G6R5WithRBGASwizzle,
                             texture_load_r5g5b6_b5g6r5_swizzle_rbga_cs);
      REX_D3D11_TEXTURE_LOAD(kLoadShaderIndexRGBA4ToBGRA4, texture_load_r4g4b4a4_b4g4r4a4_cs);
      REX_D3D11_TEXTURE_LOAD(kLoadShaderIndexDXT3AAs1111ToBGRA4, texture_load_dxt3aas1111_bgra4_cs);
      REX_D3D11_TEXTURE_LOAD(kLoadShaderIndexR10G11B11ToRGBA16, texture_load_r10g11b11_rgba16_cs);
      REX_D3D11_TEXTURE_LOAD(kLoadShaderIndexR10G11B11ToRGBA16SNorm,
                             texture_load_r10g11b11_rgba16_snorm_cs);
      REX_D3D11_TEXTURE_LOAD(kLoadShaderIndexR11G11B10ToRGBA16, texture_load_r11g11b10_rgba16_cs);
      REX_D3D11_TEXTURE_LOAD(kLoadShaderIndexR11G11B10ToRGBA16SNorm,
                             texture_load_r11g11b10_rgba16_snorm_cs);
      REX_D3D11_TEXTURE_LOAD(kLoadShaderIndexGBGR8ToRGB8, texture_load_gbgr8_rgb8_cs);
      REX_D3D11_TEXTURE_LOAD(kLoadShaderIndexBGRG8ToRGB8, texture_load_bgrg8_rgb8_cs);
      REX_D3D11_TEXTURE_LOAD(kLoadShaderIndexDXT1ToRGBA8, texture_load_dxt1_rgba8_cs);
      REX_D3D11_TEXTURE_LOAD(kLoadShaderIndexDXT3ToRGBA8, texture_load_dxt3_rgba8_cs);
      REX_D3D11_TEXTURE_LOAD(kLoadShaderIndexDXT5ToRGBA8, texture_load_dxt5_rgba8_cs);
      REX_D3D11_TEXTURE_LOAD(kLoadShaderIndexDXNToRG8, texture_load_dxn_rg8_cs);
      REX_D3D11_TEXTURE_LOAD(kLoadShaderIndexDXT3A, texture_load_dxt3a_cs);
      REX_D3D11_TEXTURE_LOAD(kLoadShaderIndexDXT5AToR8, texture_load_dxt5a_r8_cs);
      REX_D3D11_TEXTURE_LOAD(kLoadShaderIndexCTX1, texture_load_ctx1_cs);
      REX_D3D11_TEXTURE_LOAD(kLoadShaderIndexDepthUnorm, texture_load_depth_unorm_cs);
      REX_D3D11_TEXTURE_LOAD(kLoadShaderIndexDepthFloat, texture_load_depth_float_cs);
      default:
        break;
    }
  }
#undef REX_D3D11_TEXTURE_LOAD
  if (code.empty()) {
    load_errors_[scaled][index] = "The DX11 texture decoder is unavailable for this guest format";
  } else {
    load_programs_[scaled][index] = shaders_.GetOrCreate(code, false, load_errors_[scaled][index]);
  }
  last_error_ = load_errors_[scaled][index];
  return load_programs_[scaled][index];
}

bool D3D11TextureCache::LoadTextureDataFromResidentMemoryImpl(Texture& common_texture,
                                                              bool load_base, bool load_mips) {
  auto& texture = static_cast<D3D11Texture&>(common_texture);
  auto key = texture.key();
  bool scaled = key.scaled_resolve;
  uint32_t scale_x = scaled ? draw_resolution_scale_x() : 1;
  uint32_t scale_y = scaled ? draw_resolution_scale_y() : 1;
  uint32_t scale_area = scale_x * scale_y;
  const auto* program = GetLoadProgram(texture.format.load_shader, scaled);
  if (!program || !program->compute() || (!load_base && !load_mips))
    return false;
  const auto& info = GetLoadShaderInfo(texture.format.load_shader);
  const auto& guest = texture.guest_layout();
  const auto* guest_format = FormatInfo::Get(key.format);
  bool volume = key.dimension == xenos::DataDimension::k3D;
  uint32_t width = key.GetWidth(), height = key.GetHeight();
  uint32_t depth = volume ? key.GetDepthOrArraySize() : 1;
  uint32_t layers = volume ? 1 : key.GetDepthOrArraySize();
  uint32_t first = load_base ? 0 : 1, last = load_mips ? key.mip_max_level : 0;
  uint32_t packed = guest.packed_level;
  uint32_t loop_first = packed ? std::min(first, packed) : uint32_t(first != 0);
  uint32_t loop_last = packed ? std::min(last, packed) : uint32_t(last != 0);
  struct Slice {
    uint32_t offset = 0, pitch = 0, plane_pitch = 0, size = 0;
  };
  std::array<Slice, xenos::kTextureMaxMips + 1> layouts = {};
  uint64_t buffer_size = 0;
  uint32_t pixels_per_thread =
      (1u << info.guest_x_blocks_per_thread_log2) * guest_format->block_width;
  for (uint32_t loop = loop_first; loop <= loop_last; ++loop) {
    uint32_t level = packed ? loop : 0;
    const auto& stored = loop ? guest.mips[level] : guest.base;
    uint32_t w = level == packed ? stored.x_extent_blocks * guest_format->block_width
                                 : std::max(width >> level, 1u);
    uint32_t h = level == packed ? stored.y_extent_blocks * guest_format->block_height
                                 : std::max(height >> level, 1u);
    uint32_t d = level == packed ? stored.z_extent : std::max(depth >> level, 1u);
    uint64_t pitch = rex::align(
        uint64_t(rex::round_up(w * scale_x, pixels_per_thread)) * info.bytes_per_host_block,
        uint64_t(256));
    uint64_t plane = pitch * h * scale_y, size = rex::align(plane * d, uint64_t(512));
    if (buffer_size + size * layers > UINT32_MAX || plane > UINT32_MAX) {
      last_error_ = "The DX11 decoded texture exceeds the native buffer address range";
      return false;
    }
    layouts[loop] = {uint32_t(buffer_size), uint32_t(pitch), uint32_t(plane), uint32_t(size)};
    buffer_size += size * layers;
  }
  D3D11_BUFFER_DESC desc = {};
  desc.ByteWidth = uint32_t(buffer_size);
  desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
  desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
  ComPtr<ID3D11Buffer> buffer;
  if (Failed(device_.device()->CreateBuffer(&desc, nullptr, buffer.GetAddressOf()),
             "Native decode buffer"))
    return false;
  D3D11_UNORDERED_ACCESS_VIEW_DESC output_desc = {};
  output_desc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
  output_desc.Format = info.dest_bpe_log2 == 2   ? DXGI_FORMAT_R32_UINT
                       : info.dest_bpe_log2 == 3 ? DXGI_FORMAT_R32G32_UINT
                                                 : DXGI_FORMAT_R32G32B32A32_UINT;
  output_desc.Buffer.NumElements = uint32_t(buffer_size) >> info.dest_bpe_log2;
  ComPtr<ID3D11UnorderedAccessView> output;
  if (Failed(device_.device()->CreateUnorderedAccessView(buffer.Get(), &output_desc,
                                                         output.GetAddressOf()),
             "Native decode buffer view"))
    return false;
  D3D11_SHADER_RESOURCE_VIEW_DESC input_desc = {};
  input_desc.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
  input_desc.Format = DXGI_FORMAT_R32_TYPELESS;
  input_desc.BufferEx.NumElements = uint32_t(buffer_size) / 4;
  input_desc.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
  ComPtr<ID3D11ShaderResourceView> decoded;
  if (Failed(device_.device()->CreateShaderResourceView(buffer.Get(), &input_desc,
                                                        decoded.GetAddressOf()),
             "Native decoded buffer reads"))
    return false;
  draws_.Invalidate();
  auto* destination = output.Get();
  for (uint32_t loop = loop_first; loop <= loop_last; ++loop) {
    uint32_t address = (loop ? key.mip_page : key.base_page) << 12;
    ComPtr<ID3D11ShaderResourceView> scaled_source;
    if (scaled) {
      scaled_source = scaled_memory_.Read(
          address, loop ? texture.GetGuestMipsSize() : texture.GetGuestBaseSize(),
          info.source_bpe_log2, last_error_);
      if (!scaled_source)
        return false;
    }
    // Expanding scaled storage may unbind the compute state to preserve old
    // ranges. Bind the decoder only after the source view has been acquired.
    auto* source = scaled ? scaled_source.Get() : native_memory_.typed_srv(info.source_bpe_log2);
    device_.context()->CSSetShader(program->compute(), nullptr, 0);
    device_.context()->CSSetShaderResources(0, 1, &source);
    device_.context()->CSSetUnorderedAccessViews(0, 1, &destination, nullptr);
    uint32_t level = packed ? loop : 0;
    const auto& stored = loop ? guest.mips[level] : guest.base;
    uint32_t w = level == packed ? stored.x_extent_blocks * guest_format->block_width
                                 : std::max(width >> level, 1u);
    uint32_t h = level == packed ? stored.y_extent_blocks * guest_format->block_height
                                 : std::max(height >> level, 1u);
    uint32_t d = level == packed ? stored.z_extent : std::max(depth >> level, 1u);
    LoadConstants constants = {};
    constants.is_tiled_3d_endian_scale =
        uint32_t(key.tiled) | (uint32_t(volume || texture.force_load_3d_tiling()) << 1) |
        (uint32_t(key.endianness) << 2) | (scale_x << 4) | (scale_y << 7);
    constants.guest_offset = scaled ? 0 : address;
    if (loop)
      constants.guest_offset += guest.mip_offsets_bytes[level] * scale_area;
    constants.guest_pitch_aligned = key.tiled
                                        ? stored.row_pitch_bytes / guest_format->bytes_per_block()
                                        : stored.row_pitch_bytes;
    constants.guest_z_stride_block_rows_aligned = stored.z_slice_stride_block_rows;
    constants.size_blocks[0] =
        (w + guest_format->block_width - 1) / guest_format->block_width * scale_x;
    constants.size_blocks[1] =
        (h + guest_format->block_height - 1) / guest_format->block_height * scale_y;
    constants.size_blocks[2] = d;
    constants.host_offset = layouts[loop].offset;
    constants.host_pitch = layouts[loop].pitch;
    constants.height_texels = h;
    uint32_t groups_x =
        (constants.size_blocks[0] + (1u << info.GetGuestXBlocksPerGroupLog2()) - 1) >>
        info.GetGuestXBlocksPerGroupLog2();
    uint32_t groups_y = (constants.size_blocks[1] + (1u << kLoadGuestYBlocksPerGroupLog2) - 1) >>
                        kLoadGuestYBlocksPerGroupLog2;
    for (uint32_t layer = 0; layer < layers; ++layer) {
      std::array<uint32_t, (sizeof(LoadConstants) + 15) / 16 * 4> padded = {};
      std::memcpy(padded.data(), &constants, sizeof(constants));
      D3D11_BUFFER_DESC cb_desc = {};
      cb_desc.ByteWidth = sizeof(padded);
      cb_desc.Usage = D3D11_USAGE_IMMUTABLE;
      cb_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
      D3D11_SUBRESOURCE_DATA cb_data = {padded.data(), 0, 0};
      ComPtr<ID3D11Buffer> cb;
      if (Failed(device_.device()->CreateBuffer(&cb_desc, &cb_data, cb.GetAddressOf()),
                 "Native decode constants")) {
        draws_.Invalidate();
        return false;
      }
      auto* cb_pointer = cb.Get();
      device_.context()->CSSetConstantBuffers(0, 1, &cb_pointer);
      device_.context()->Dispatch(groups_x, groups_y, d);
      constants.guest_offset += stored.array_slice_stride_bytes * scale_area;
      constants.host_offset += layouts[loop].size;
    }
  }
  // The next stage reads the decoded buffer as an SRV and writes an image.
  draws_.Invalidate();
  for (uint32_t level = first; level <= last; ++level) {
    uint32_t loop = level ? (packed ? std::min(level, packed) : 1) : 0;
    const auto& slice = layouts[loop];
    uint32_t x = 0, y = 0, z = 0;
    if (level >= packed)
      texture_util::GetPackedMipOffset(width, height, depth, key.format, level, x, y, z);
    TextureUpload::Layout transfer;
    transfer.packed_source = texture.format.packed_source;
    transfer.offset =
        slice.offset + x * scale_x * guest_format->block_width * info.bytes_per_host_block +
        y * scale_y * guest_format->block_height * slice.pitch + z * slice.plane_pitch;
    transfer.row_pitch = slice.pitch;
    transfer.slice_pitch = volume ? slice.plane_pitch : slice.size;
    transfer.width = std::max((width * scale_x) >> level, 1u);
    transfer.height = std::max((height * scale_y) >> level, 1u);
    transfer.depth_or_layers = volume ? std::max(depth >> level, 1u) : layers;
    if (!uploader_.Upload(decoded.Get(), texture.mip_views[level].Get(), transfer, last_error_))
      return false;
    if (volume && !level) {
      transfer.depth_or_layers = 1;
      if (!uploader_.Upload(decoded.Get(), texture.volume_slice_write.Get(), transfer, last_error_))
        return false;
    }
  }
  if (load_base)
    texture.content_red_blue_swapped = false;
  texture.MarkAsUsed();
  return !Failed(device_.device()->GetDeviceRemovedReason(), "Native guest texture decode");
}

uint32_t D3D11TextureCache::SwapRedBlueSwizzle(uint32_t swizzle) {
  uint32_t swapped = 0;
  for (uint32_t i = 0; i < 4; ++i) {
    uint32_t component = (swizzle >> (3 * i)) & 0b111;
    if (component == xenos::XE_GPU_TEXTURE_SWIZZLE_R) {
      component = xenos::XE_GPU_TEXTURE_SWIZZLE_B;
    } else if (component == xenos::XE_GPU_TEXTURE_SWIZZLE_B) {
      component = xenos::XE_GPU_TEXTURE_SWIZZLE_R;
    }
    swapped |= component << (3 * i);
  }
  return swapped;
}

uint32_t D3D11TextureCache::FindNativeResolveTargets(uint32_t dest_base,
                                                     uint32_t dest_pitch_texels,
                                                     xenos::TextureFormat format,
                                                     xenos::Endian endian, bool scaled,
                                                     NativeResolveTarget* targets_out) {
  dest_base &= 0x1FFFFFFF;
  if (dest_base & 0xFFF)
    return 0;
  uint32_t count = 0;
  ForEachTextureWithBasePage(dest_base >> 12, [&](Texture& common) {
    const TextureKey& key = common.key();
    if (count >= kMaxNativeResolveTargets || key.format != format || key.endianness != endian ||
        !key.tiled || key.dimension != xenos::DataDimension::k2DOrStacked ||
        key.depth_or_array_size_minus_1 || key.signed_separate ||
        bool(key.scaled_resolve) != scaled || (uint32_t(key.pitch) << 5) != dest_pitch_texels ||
        key.mip_max_level || common.GetGuestMipsSize() || common.outdated_mask())
      return;
    auto& texture = static_cast<D3D11Texture&>(common);
    NativeResolveTarget& target = targets_out[count++];
    target.texture = &texture;
    target.resource = texture.resource.Get();
    target.width = key.GetWidth();
    target.height = key.GetHeight();
  });
  return count;
}

void D3D11TextureCache::EndNativeResolveWrite(const NativeResolveTarget& target,
                                              bool red_blue_swapped) {
  auto& texture = *static_cast<D3D11Texture*>(target.texture);
  texture.content_red_blue_swapped = red_blue_swapped;
  texture.MarkAsUsed();
  MarkTextureBaseWrittenByGpu(texture);
}

ID3D11ShaderResourceView* D3D11TextureCache::GetOrCreateView(
    D3D11Texture& texture, const DxbcShader::TextureBinding& binding) {
  auto key = texture.key();
  bool volume = binding.dimension == xenos::FetchOpDimension::k3DOrStacked;
  bool cube = binding.dimension == xenos::FetchOpDimension::kCube;
  if (!AreDimensionsCompatible(binding.dimension, key.dimension)) {
    last_error_ = "The DX11 texture binding needs a different native image dimension";
    return nullptr;
  }
  uint32_t view_key = uint32_t(binding.dimension) | (uint32_t(binding.is_signed) << 3);
  auto existing = texture.views.find(view_key);
  if (existing != texture.views.end())
    return existing->second.Get();
  D3D11_SHADER_RESOURCE_VIEW_DESC desc = {};
  ID3D11Resource* resource_for_view = texture.resource.Get();
  desc.Format = binding.is_signed ? texture.format.signed_view : texture.format.unsigned_view;
  if (desc.Format == DXGI_FORMAT_UNKNOWN) {
    last_error_ = "The DX11 guest texture has no native view for this signedness";
    return nullptr;
  }
  if (volume) {
    desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D;
    desc.Texture3D.MipLevels = key.mip_max_level + 1;
  } else if (cube) {
    desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
    desc.TextureCube.MipLevels = key.mip_max_level + 1;
  } else {
    desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
    desc.Texture2DArray.MipLevels = key.mip_max_level + 1;
    desc.Texture2DArray.ArraySize = key.GetDepthOrArraySize();
    if (key.dimension == xenos::DataDimension::k3D) {
      resource_for_view = texture.volume_slice.Get();
      desc.Texture2DArray.MipLevels = desc.Texture2DArray.ArraySize = 1;
    }
  }
  ComPtr<ID3D11ShaderResourceView> view;
  if (Failed(
          device_.device()->CreateShaderResourceView(resource_for_view, &desc, view.GetAddressOf()),
          "Native guest texture sampling view"))
    return nullptr;
  auto* result = view.Get();
  texture.views.emplace(view_key, std::move(view));
  return result;
}

bool D3D11TextureCache::BuildShaderBindings(std::span<const DxbcShader::TextureBinding> bindings,
                                            std::vector<ID3D11ShaderResourceView*>& resources,
                                            std::vector<TextureSwizzle>& swizzles,
                                            std::string& error) {
  error = last_error_;
  resources.clear();
  swizzles.clear();
  if (!error.empty())
    return false;
  if (bindings.size() > D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - 1) {
    error = "The DX11 guest shader exceeds the native texture register limit";
    return false;
  }
  resources.resize(bindings.size() + 1);
  resources[0] = native_memory_.raw_srv();
  for (uint32_t i = 0; i < bindings.size(); ++i) {
    const auto& shader_binding = bindings[i];
    if (shader_binding.fetch_constant >= 32) {
      error = "The DX11 shader has an invalid texture fetch register";
      return false;
    }
    const auto* binding = GetValidTextureBinding(shader_binding.fetch_constant);
    uint32_t swizzle = xenos::XE_GPU_TEXTURE_SWIZZLE_0000;
    if (binding && AreDimensionsCompatible(shader_binding.dimension, binding->key.dimension) &&
        (shader_binding.is_signed ? texture_util::IsAnySignSigned(binding->swizzled_signs)
                                  : texture_util::IsAnySignNotSigned(binding->swizzled_signs))) {
      auto* common = shader_binding.is_signed && IsSignedVersionSeparateForFormat(binding->key)
                         ? binding->texture_signed
                         : binding->texture;
      if (!common) {
        error = "The DX11 guest texture could not be created or loaded";
        return false;
      }
      auto& texture = static_cast<D3D11Texture&>(*common);
      resources[i + 1] = GetOrCreateView(texture, shader_binding);
      if (!resources[i + 1]) {
        error = last_error_;
        return false;
      }
      texture.MarkAsUsed();
      swizzle = binding->host_swizzle;
      if (texture.content_red_blue_swapped)
        swizzle = SwapRedBlueSwizzle(swizzle);
    }
    if (swizzle != kIdentityTextureSwizzle)
      swizzles.push_back({i + 1, swizzle});
  }
  return true;
}

}  // namespace rex::graphics::d3d11
