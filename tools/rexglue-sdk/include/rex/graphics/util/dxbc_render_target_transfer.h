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

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <rex/graphics/xenos.h>

namespace rex::graphics {

// Shared ownership-transfer pixel bytecode for native Direct3D backends.
// Register numbers and keys retain the existing D3D12 draw contract.
class DxbcRenderTargetTransferShader {
 public:
  enum TransferCBVRegister : uint32_t {
    kTransferCBVRegisterStencilMask,
    kTransferCBVRegisterAddress,
    kTransferCBVRegisterHostDepthAddress,
  };
  enum TransferSRVRegister : uint32_t {
    kTransferSRVRegisterColor,
    kTransferSRVRegisterDepth,
    kTransferSRVRegisterStencil,
    kTransferSRVRegisterHostDepth,
    kTransferSRVRegisterCount,
  };
  enum TransferUsedRootParameter : uint32_t {
    // Changed 8 times per transfer.
    kTransferUsedRootParameterStencilMaskConstant,
    kTransferUsedRootParameterColorSRV,
    // Mutually exclusive with ColorSRV.
    kTransferUsedRootParameterDepthSRV,
    // Mutually exclusive with ColorSRV.
    kTransferUsedRootParameterStencilSRV,
    // May happen to be the same for different sources.
    kTransferUsedRootParameterAddressConstant,
    kTransferUsedRootParameterHostDepthSRV,
    kTransferUsedRootParameterHostDepthAddressConstant,
    kTransferUsedRootParameterCount,

    kTransferUsedRootParameterStencilMaskConstantBit =
        uint32_t(1) << kTransferUsedRootParameterStencilMaskConstant,
    kTransferUsedRootParameterColorSRVBit = uint32_t(1) << kTransferUsedRootParameterColorSRV,
    kTransferUsedRootParameterDepthSRVBit = uint32_t(1) << kTransferUsedRootParameterDepthSRV,
    kTransferUsedRootParameterStencilSRVBit = uint32_t(1) << kTransferUsedRootParameterStencilSRV,
    kTransferUsedRootParameterAddressConstantBit = uint32_t(1)
                                                   << kTransferUsedRootParameterAddressConstant,
    kTransferUsedRootParameterHostDepthSRVBit = uint32_t(1)
                                                << kTransferUsedRootParameterHostDepthSRV,
    kTransferUsedRootParameterHostDepthAddressConstantBit =
        uint32_t(1) << kTransferUsedRootParameterHostDepthAddressConstant,

    kTransferUsedRootParametersDescriptorMask =
        kTransferUsedRootParameterColorSRVBit | kTransferUsedRootParameterDepthSRVBit |
        kTransferUsedRootParameterStencilSRVBit | kTransferUsedRootParameterHostDepthSRVBit,
  };
  enum class TransferRootSignatureIndex {
    kColor,
    kDepth,
    kDepthStencil,
    kColorToStencilBit,
    kStencilToStencilBit,
    kColorAndHostDepth,
    kDepthAndHostDepth,
    kDepthStencilAndHostDepth,
    kCount,
  };
  static const uint32_t kTransferUsedRootParameters[size_t(TransferRootSignatureIndex::kCount)];
  enum class TransferMode : uint32_t {
    // 1 SRV (color texture), source constant.
    kColorToDepth,
    // 1 SRV (color texture), source constant.
    kColorToColor,

    // 1 or 2 SRVs (depth texture, stencil texture if SV_StencilRef is
    // supported), source constant.
    kDepthToDepth,
    // 2 SRVs (depth texture, stencil texture), source constant.
    kDepthToColor,

    // 1 SRV (color texture), mask constant (most frequently changed, 8 times
    // per transfer), source constant.
    kColorToStencilBit,
    // 1 SRV (stencil texture), mask constant, source constant.
    kDepthToStencilBit,

    // Two-source modes, using the host depth if it, when converted to the guest
    // format, matches what's in the owner source (not modified, keep host
    // precision), or the guest data otherwise (significantly modified, possibly
    // cleared). Stencil for SV_StencilRef is always taken from the guest
    // source.

    // 2 SRVs (color texture, host depth texture or buffer), source constant,
    // host depth source constant.
    kColorAndHostDepthToDepth,
    // When using different source and destination depth formats. 2 or 3 SRVs
    // (depth texture, stencil texture if SV_StencilRef is supported, host depth
    // texture or buffer), source constant, host depth source constant.
    kDepthAndHostDepthToDepth,

    kCount,
  };
  enum class TransferOutput {
    kColor,
    kDepth,
    // With this output, kTransferCBVRegisterStencilMask is used.
    kStencilBit,
  };
  struct TransferModeInfo {
    TransferOutput output;
    TransferRootSignatureIndex root_signature_no_stencil_ref;
    TransferRootSignatureIndex root_signature_with_stencil_ref;
  };
  static const TransferModeInfo kTransferModes[size_t(TransferMode::kCount)];

  union TransferAddressConstant {
    uint32_t constant;
    struct {
      uint32_t dest_pitch : xenos::kEdramPitchTilesBits;
      uint32_t source_pitch : xenos::kEdramPitchTilesBits;
      // Destination base minus source base, in guest EDRAM tiles.
      int32_t source_to_dest : xenos::kEdramBaseTilesBits + 1;
    };
    TransferAddressConstant() : constant(0) { static_assert(sizeof(*this) == 4); }
  };

  union TransferShaderKey {
    uint32_t key;
    struct {
      xenos::MsaaSamples dest_msaa_samples : xenos::kMsaaSamplesBits;
      uint32_t dest_resource_format : xenos::kRenderTargetFormatBits;
      xenos::MsaaSamples source_msaa_samples : xenos::kMsaaSamplesBits;
      // Always 1x when host_depth_source_is_copy is true not to create the same
      // pipeline for different MSAA sample counts as it doesn't matter in this
      // case.
      xenos::MsaaSamples host_depth_source_msaa_samples : xenos::kMsaaSamplesBits;
      uint32_t source_resource_format : xenos::kRenderTargetFormatBits;
      // If host depth is also fetched, whether it's pre-copied to the EDRAM
      // buffer (but since it's just a scratch buffer, with tiles laid out
      // linearly with the same pitch as in the original render target; also no
      // swapping of 40-sample columns as opposed to the host render target -
      // this is done only for the color source).
      uint32_t host_depth_source_is_copy : 1;

      // Last bits because this affects the root signature - after sorting, only
      // change it as fewer times as possible. Depth buffers have an additional
      // stencil SRV.
      static_assert(size_t(TransferMode::kCount) <= (size_t(1) << 3));
      TransferMode mode : 3;
    };

    TransferShaderKey() : key(0) { static_assert(sizeof(*this) == sizeof(key)); }

    struct Hasher {
      size_t operator()(const TransferShaderKey& key) const {
        return std::hash<uint32_t>{}(key.key);
      }
    };
    bool operator==(const TransferShaderKey& other_key) const { return key == other_key.key; }
    bool operator!=(const TransferShaderKey& other_key) const { return !(*this == other_key); }
    bool operator<(const TransferShaderKey& other_key) const { return key < other_key.key; }
  };

  struct Options {
    uint32_t resolution_scale_x = 1;
    uint32_t resolution_scale_y = 1;
    bool msaa_2x_supported = true;
    bool stencil_reference_output = false;
    bool depth_float24_convert_in_pixel_shader = true;
    bool depth_float24_round = false;
    // Normalize dual-output division to matching masks for native Direct3D
    // implementations that do not implement differing quotient/remainder masks.
    bool portable_integer_division = false;
  };
  static bool Create(TransferShaderKey key, const Options& options, std::vector<uint32_t>& output,
                     std::string& error);
};

}  // namespace rex::graphics
