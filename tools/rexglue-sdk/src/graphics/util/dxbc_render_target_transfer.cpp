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

#include <rex/graphics/util/dxbc_render_target_transfer.h>

#include <algorithm>
#include <array>
#include <cstring>

#include <rex/assert.h>
#include <rex/graphics/format/dxbc.h>
#include <rex/graphics/util/dxbc_float.h>
#include <rex/graphics/util/dxbc_portable.h>
#include <rex/math.h>

#include "thirdparty/dxbc/DXBCChecksum.h"

namespace rex::graphics {
namespace {

bool IsIntegerTransferFormat(xenos::ColorRenderTargetFormat format) {
  switch (format) {
    case xenos::ColorRenderTargetFormat::k_16_16:
    case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
    case xenos::ColorRenderTargetFormat::k_16_16_16_16:
    case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT:
    case xenos::ColorRenderTargetFormat::k_32_FLOAT:
    case xenos::ColorRenderTargetFormat::k_32_32_FLOAT:
      return true;
    default:
      return false;
  }
}
bool ValidColorFormat(uint32_t format) {
  switch (xenos::ColorRenderTargetFormat(format)) {
    case xenos::ColorRenderTargetFormat::k_8_8_8_8:
    case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
    case xenos::ColorRenderTargetFormat::k_16_16:
    case xenos::ColorRenderTargetFormat::k_16_16_16_16:
    case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
    case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT:
    case xenos::ColorRenderTargetFormat::k_32_FLOAT:
    case xenos::ColorRenderTargetFormat::k_32_32_FLOAT:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16:
      return true;
    default:
      return false;
  }
}
}  // namespace

const uint32_t DxbcRenderTargetTransferShader::kTransferUsedRootParameters[size_t(
    TransferRootSignatureIndex::kCount)] = {
    // kColor
    kTransferUsedRootParameterColorSRVBit | kTransferUsedRootParameterAddressConstantBit,
    // kDepth
    kTransferUsedRootParameterDepthSRVBit | kTransferUsedRootParameterAddressConstantBit,
    // kDepthStencil
    kTransferUsedRootParameterDepthSRVBit | kTransferUsedRootParameterStencilSRVBit |
        kTransferUsedRootParameterAddressConstantBit,
    // kColorToStencilBit
    kTransferUsedRootParameterStencilMaskConstantBit | kTransferUsedRootParameterColorSRVBit |
        kTransferUsedRootParameterAddressConstantBit,
    // kStencilToStencilBit
    kTransferUsedRootParameterStencilMaskConstantBit | kTransferUsedRootParameterStencilSRVBit |
        kTransferUsedRootParameterAddressConstantBit,
    // kColorAndHostDepth
    kTransferUsedRootParameterColorSRVBit | kTransferUsedRootParameterAddressConstantBit |
        kTransferUsedRootParameterHostDepthSRVBit |
        kTransferUsedRootParameterHostDepthAddressConstantBit,
    // kDepthAndHostDepth
    kTransferUsedRootParameterDepthSRVBit | kTransferUsedRootParameterAddressConstantBit |
        kTransferUsedRootParameterHostDepthSRVBit |
        kTransferUsedRootParameterHostDepthAddressConstantBit,
    // kDepthStencilAndHostDepth
    kTransferUsedRootParameterDepthSRVBit | kTransferUsedRootParameterStencilSRVBit |
        kTransferUsedRootParameterAddressConstantBit | kTransferUsedRootParameterHostDepthSRVBit |
        kTransferUsedRootParameterHostDepthAddressConstantBit,
};

const DxbcRenderTargetTransferShader::TransferModeInfo
    DxbcRenderTargetTransferShader::kTransferModes[size_t(TransferMode::kCount)] = {
        // kColorToDepth
        {TransferOutput::kDepth, TransferRootSignatureIndex::kColor,
         TransferRootSignatureIndex::kColor},
        // kColorToColor
        {TransferOutput::kColor, TransferRootSignatureIndex::kColor,
         TransferRootSignatureIndex::kColor},
        // kDepthToDepth
        {TransferOutput::kDepth, TransferRootSignatureIndex::kDepth,
         TransferRootSignatureIndex::kDepthStencil},
        // kDepthToColor
        {TransferOutput::kColor, TransferRootSignatureIndex::kDepthStencil,
         TransferRootSignatureIndex::kDepthStencil},
        // kColorToStencilBit
        {TransferOutput::kStencilBit, TransferRootSignatureIndex::kColorToStencilBit,
         TransferRootSignatureIndex::kColorToStencilBit},
        // kDepthToStencilBit
        {TransferOutput::kStencilBit, TransferRootSignatureIndex::kStencilToStencilBit,
         TransferRootSignatureIndex::kStencilToStencilBit},
        // kColorAndHostDepthToDepth
        {TransferOutput::kDepth, TransferRootSignatureIndex::kColorAndHostDepth,
         TransferRootSignatureIndex::kColorAndHostDepth},
        // kDepthAndHostDepthToDepth
        {TransferOutput::kDepth, TransferRootSignatureIndex::kDepthAndHostDepth,
         TransferRootSignatureIndex::kDepthStencilAndHostDepth},
};

bool DxbcRenderTargetTransferShader::Create(TransferShaderKey key, const Options& options,
                                            std::vector<uint32_t>& output, std::string& error) {
  output.clear();
  error.clear();
  constexpr uint32_t used_key_bits =
      3 * xenos::kMsaaSamplesBits + 2 * xenos::kRenderTargetFormatBits + 4;
  if (!options.resolution_scale_x || !options.resolution_scale_y ||
      options.resolution_scale_x > 3 || options.resolution_scale_y > 3 ||
      uint32_t(key.mode) >= uint32_t(TransferMode::kCount) || (key.key >> used_key_bits) ||
      uint32_t(key.dest_msaa_samples) > 2 || uint32_t(key.source_msaa_samples) > 2 ||
      uint32_t(key.host_depth_source_msaa_samples) > 2) {
    error = "Invalid native render target transfer key or scale";
    return false;
  }
  const TransferModeInfo& mode = kTransferModes[size_t(key.mode)];
  bool dest_is_stencil_bit = mode.output == TransferOutput::kStencilBit;
  uint32_t rs = kTransferUsedRootParameters[size_t(options.stencil_reference_output
                                                       ? mode.root_signature_with_stencil_ref
                                                       : mode.root_signature_no_stencil_ref)];
  bool source_is_color_for_validation = (rs & kTransferUsedRootParameterColorSRVBit) != 0;
  if ((mode.output == TransferOutput::kColor ? !ValidColorFormat(key.dest_resource_format)
                                             : key.dest_resource_format > 1) ||
      (source_is_color_for_validation ? !ValidColorFormat(key.source_resource_format)
                                      : key.source_resource_format > 1) ||
      (key.host_depth_source_is_copy &&
       key.host_depth_source_msaa_samples != xenos::MsaaSamples::k1X)) {
    error = "Invalid native render target transfer formats";
    return false;
  }
  // If not dest_is_color, it's depth, or stencil bit - 40-sample columns are
  // swapped as opposed to color source.
  bool dest_is_color = (mode.output == TransferOutput::kColor);

  xenos::ColorRenderTargetFormat dest_color_format =
      xenos::ColorRenderTargetFormat(key.dest_resource_format);
  xenos::DepthRenderTargetFormat dest_depth_format =
      xenos::DepthRenderTargetFormat(key.dest_resource_format);
  bool dest_is_64bpp = dest_is_color && xenos::IsColorRenderTargetFormat64bpp(dest_color_format);

  xenos::ColorRenderTargetFormat source_color_format =
      xenos::ColorRenderTargetFormat(key.source_resource_format);
  xenos::DepthRenderTargetFormat source_depth_format =
      xenos::DepthRenderTargetFormat(key.source_resource_format);
  // If not source_is_color, it's depth / stencil - 40-sample columns are
  // swapped as opposed to color destination.
  bool source_is_color = (rs & kTransferUsedRootParameterColorSRVBit) != 0;
  bool source_is_64bpp;
  uint32_t source_color_format_component_count;
  uint32_t source_color_srv_component_mask;
  bool source_color_is_uint;
  if (source_is_color) {
    assert_zero(rs & kTransferUsedRootParameterDepthSRVBit);
    assert_zero(rs & kTransferUsedRootParameterStencilSRVBit);
    source_is_64bpp = xenos::IsColorRenderTargetFormat64bpp(source_color_format);
    source_color_format_component_count =
        xenos::GetColorRenderTargetFormatComponentCount(source_color_format);
    if (dest_is_stencil_bit) {
      if (source_is_64bpp && !dest_is_64bpp) {
        // Need one component, but choosing from the two 32bpp halves of the
        // 64bpp sample.
        source_color_srv_component_mask = 0b1 | (0b1 << (source_color_format_component_count >> 1));
      } else {
        // Red is at least 8 bits per component in all formats.
        source_color_srv_component_mask = 0b1;
      }
    } else {
      source_color_srv_component_mask = (uint32_t(1) << source_color_format_component_count) - 1;
    }
    source_color_is_uint = IsIntegerTransferFormat(source_color_format);
  } else {
    source_is_64bpp = false;
    source_color_format_component_count = 0;
    source_color_srv_component_mask = 0;
    source_color_is_uint = false;
  }

  bool shader_uses_stencil_reference_output =
      mode.output == TransferOutput::kDepth && options.stencil_reference_output;

  // Because of output.resize(), pointers can't be kept persistently
  // here! Resizing also zeroes the memory.

  output.clear();

  // RDEF, ISGN, OSGN, SHEX, optionally SFI0, STAT.
  uint32_t blob_count = 5 + uint32_t(shader_uses_stencil_reference_output);

  // Allocate space for the container header and the blob offsets.
  output.resize(sizeof(dxbc::ContainerHeader) / sizeof(uint32_t) + blob_count);
  uint32_t blob_offset_position_dwords = sizeof(dxbc::ContainerHeader) / sizeof(uint32_t);
  uint32_t blob_position_dwords = uint32_t(output.size());
  constexpr uint32_t kBlobHeaderSizeDwords = sizeof(dxbc::BlobHeader) / sizeof(uint32_t);

  uint32_t name_ptr;

  // ***************************************************************************
  // Resource definition
  // ***************************************************************************

  output[blob_offset_position_dwords] = uint32_t(blob_position_dwords * sizeof(uint32_t));
  uint32_t rdef_position_dwords = blob_position_dwords + kBlobHeaderSizeDwords;
  // Not needed, as the next operation done is resize, to allocate the space for
  // both the blob header and the resource definition header.
  // output.resize(rdef_position_dwords);

  // Allocate space for the RDEF header.
  output.resize(rdef_position_dwords + sizeof(dxbc::RdefHeader) / sizeof(uint32_t));
  // Generator name.
  dxbc::AppendAlignedString(output, "Xenia");

  // Constant types - uint (aka "dword" when it's scalar) only.
  // Names.
  name_ptr = uint32_t((output.size() - rdef_position_dwords) * sizeof(uint32_t));
  uint32_t rdef_dword_name_ptr = name_ptr;
  name_ptr += dxbc::AppendAlignedString(output, "dword");
  // Types.
  uint32_t rdef_type_uint_position_dwords = uint32_t(output.size());
  uint32_t rdef_type_uint_ptr =
      uint32_t((rdef_type_uint_position_dwords - rdef_position_dwords) * sizeof(uint32_t));
  output.resize(rdef_type_uint_position_dwords + sizeof(dxbc::RdefType) / sizeof(uint32_t));
  {
    auto& rdef_type_uint =
        *reinterpret_cast<dxbc::RdefType*>(output.data() + rdef_type_uint_position_dwords);
    rdef_type_uint.variable_class = dxbc::RdefVariableClass::kScalar;
    rdef_type_uint.variable_type = dxbc::RdefVariableType::kUInt;
    rdef_type_uint.row_count = 1;
    rdef_type_uint.column_count = 1;
    rdef_type_uint.name_ptr = rdef_dword_name_ptr;
  }

  // Constants, if needed:
  // - uint xe_transfer_stencil_mask
  // - uint xe_transfer_address
  // - uint xe_transfer_host_depth_address
  uint32_t rdef_constant_count = 0;
  uint32_t rdef_constant_index_stencil_mask =
      (rs & kTransferUsedRootParameterStencilMaskConstantBit) ? rdef_constant_count++ : UINT32_MAX;
  assert_false(dest_is_stencil_bit && rdef_constant_index_stencil_mask == UINT32_MAX);
  uint32_t rdef_constant_index_address =
      (rs & kTransferUsedRootParameterAddressConstantBit) ? rdef_constant_count++ : UINT32_MAX;
  assert_true(rdef_constant_index_address != UINT32_MAX);
  uint32_t rdef_constant_index_host_depth_address =
      (rs & kTransferUsedRootParameterHostDepthAddressConstantBit) ? rdef_constant_count++
                                                                   : UINT32_MAX;
  // Names.
  name_ptr = uint32_t((output.size() - rdef_position_dwords) * sizeof(uint32_t));
  uint32_t rdef_xe_transfer_stencil_mask_name_ptr = name_ptr;
  if (rdef_constant_index_stencil_mask != UINT32_MAX) {
    name_ptr += dxbc::AppendAlignedString(output, "xe_transfer_stencil_mask");
  }
  uint32_t rdef_xe_transfer_address_name_ptr = name_ptr;
  if (rdef_constant_index_address != UINT32_MAX) {
    name_ptr += dxbc::AppendAlignedString(output, "xe_transfer_address");
  }
  uint32_t rdef_xe_transfer_host_depth_address_name_ptr = name_ptr;
  if (rdef_constant_index_host_depth_address != UINT32_MAX) {
    name_ptr += dxbc::AppendAlignedString(output, "xe_transfer_host_depth_address");
  }
  // Constants.
  uint32_t rdef_constants_position_dwords = uint32_t(output.size());
  uint32_t rdef_constants_ptr =
      uint32_t((rdef_constants_position_dwords - rdef_position_dwords) * sizeof(uint32_t));
  output.resize(rdef_constants_position_dwords +
                sizeof(dxbc::RdefVariable) / sizeof(uint32_t) * rdef_constant_count);
  {
    auto rdef_constants =
        reinterpret_cast<dxbc::RdefVariable*>(output.data() + rdef_constants_position_dwords);
    // uint xe_transfer_stencil_mask
    if (rdef_constant_index_stencil_mask != UINT32_MAX) {
      dxbc::RdefVariable& rdef_constant_stencil_mask =
          rdef_constants[rdef_constant_index_stencil_mask];
      rdef_constant_stencil_mask.name_ptr = rdef_xe_transfer_stencil_mask_name_ptr;
      rdef_constant_stencil_mask.size_bytes = sizeof(uint32_t);
      rdef_constant_stencil_mask.flags = dxbc::kRdefVariableFlagUsed;
      rdef_constant_stencil_mask.type_ptr = rdef_type_uint_ptr;
      rdef_constant_stencil_mask.start_texture = UINT32_MAX;
      rdef_constant_stencil_mask.start_sampler = UINT32_MAX;
    }
    // uint xe_transfer_address
    if (rdef_constant_index_address != UINT32_MAX) {
      dxbc::RdefVariable& rdef_constant_address = rdef_constants[rdef_constant_index_address];
      rdef_constant_address.name_ptr = rdef_xe_transfer_address_name_ptr;
      rdef_constant_address.size_bytes = sizeof(uint32_t);
      rdef_constant_address.flags = dxbc::kRdefVariableFlagUsed;
      rdef_constant_address.type_ptr = rdef_type_uint_ptr;
      rdef_constant_address.start_texture = UINT32_MAX;
      rdef_constant_address.start_sampler = UINT32_MAX;
    }
    // uint xe_transfer_host_depth_address
    if (rdef_constant_index_host_depth_address != UINT32_MAX) {
      dxbc::RdefVariable& rdef_constant_host_depth_address =
          rdef_constants[rdef_constant_index_host_depth_address];
      rdef_constant_host_depth_address.name_ptr = rdef_xe_transfer_host_depth_address_name_ptr;
      rdef_constant_host_depth_address.size_bytes = sizeof(uint32_t);
      rdef_constant_host_depth_address.flags = dxbc::kRdefVariableFlagUsed;
      rdef_constant_host_depth_address.type_ptr = rdef_type_uint_ptr;
      rdef_constant_host_depth_address.start_texture = UINT32_MAX;
      rdef_constant_host_depth_address.start_sampler = UINT32_MAX;
    }
  }

  // Constant buffers, if needed:
  // - xe_transfer_stencil_mask { uint xe_transfer_stencil_mask; }
  // - xe_transfer_address { uint xe_transfer_address; }
  // - xe_transfer_host_depth_address { uint xe_transfer_host_depth_address; }
  // Reusing the constant names for constant buffers.
  uint32_t rdef_cbuffer_count = 0;
  uint32_t cbuffer_index_stencil_mask =
      rdef_constant_index_stencil_mask != UINT32_MAX ? rdef_cbuffer_count++ : UINT32_MAX;
  uint32_t cbuffer_index_address =
      rdef_constant_index_address != UINT32_MAX ? rdef_cbuffer_count++ : UINT32_MAX;
  uint32_t cbuffer_index_host_depth_address =
      rdef_constant_index_host_depth_address != UINT32_MAX ? rdef_cbuffer_count++ : UINT32_MAX;
  uint32_t rdef_cbuffer_position_dwords = uint32_t(output.size());
  output.resize(rdef_cbuffer_position_dwords +
                sizeof(dxbc::RdefCbuffer) / sizeof(uint32_t) * rdef_cbuffer_count);
  {
    auto rdef_cbuffers =
        reinterpret_cast<dxbc::RdefCbuffer*>(output.data() + rdef_cbuffer_position_dwords);
    // xe_transfer_stencil_mask
    if (cbuffer_index_stencil_mask != UINT32_MAX) {
      dxbc::RdefCbuffer& rdef_cbuffer_stencil_mask = rdef_cbuffers[cbuffer_index_stencil_mask];
      rdef_cbuffer_stencil_mask.name_ptr = rdef_xe_transfer_stencil_mask_name_ptr;
      rdef_cbuffer_stencil_mask.variable_count = 1;
      rdef_cbuffer_stencil_mask.variables_ptr = uint32_t(
          rdef_constants_ptr + sizeof(dxbc::RdefVariable) * rdef_constant_index_stencil_mask);
      rdef_cbuffer_stencil_mask.size_vector_aligned_bytes = sizeof(uint32_t) * 4;
    }
    // xe_transfer_address
    if (cbuffer_index_address != UINT32_MAX) {
      dxbc::RdefCbuffer& rdef_cbuffer_address = rdef_cbuffers[cbuffer_index_address];
      rdef_cbuffer_address.name_ptr = rdef_xe_transfer_address_name_ptr;
      rdef_cbuffer_address.variable_count = 1;
      rdef_cbuffer_address.variables_ptr =
          uint32_t(rdef_constants_ptr + sizeof(dxbc::RdefVariable) * rdef_constant_index_address);
      rdef_cbuffer_address.size_vector_aligned_bytes = sizeof(uint32_t) * 4;
    }
    // xe_transfer_host_depth_address
    if (cbuffer_index_host_depth_address != UINT32_MAX) {
      dxbc::RdefCbuffer& rdef_cbuffer_host_depth_address =
          rdef_cbuffers[cbuffer_index_host_depth_address];
      rdef_cbuffer_host_depth_address.name_ptr = rdef_xe_transfer_host_depth_address_name_ptr;
      rdef_cbuffer_host_depth_address.variable_count = 1;
      rdef_cbuffer_host_depth_address.variables_ptr = uint32_t(
          rdef_constants_ptr + sizeof(dxbc::RdefVariable) * rdef_constant_index_host_depth_address);
      rdef_cbuffer_host_depth_address.size_vector_aligned_bytes = sizeof(uint32_t) * 4;
    }
  }

  // Bindings.
  // - Texture2D/Texture2DMS<floatN/uintN> xe_transfer_color
  // - Texture2D/Texture2DMS<float> xe_transfer_depth
  // - Texture2D/Texture2DMS<uint2> xe_transfer_stencil
  // - Texture2D<float>/Texture2DMS<float>/Buffer<uint> xe_transfer_host_depth
  // - Constant buffers
  uint32_t rdef_srv_count = 0;
  uint32_t srv_index_color =
      (rs & kTransferUsedRootParameterColorSRVBit) ? rdef_srv_count++ : UINT32_MAX;
  uint32_t srv_index_depth =
      (rs & kTransferUsedRootParameterDepthSRVBit) ? rdef_srv_count++ : UINT32_MAX;
  uint32_t srv_index_stencil =
      (rs & kTransferUsedRootParameterStencilSRVBit) ? rdef_srv_count++ : UINT32_MAX;
  uint32_t srv_index_host_depth =
      (rs & kTransferUsedRootParameterHostDepthSRVBit) ? rdef_srv_count++ : UINT32_MAX;
  uint32_t rdef_binding_count = rdef_srv_count + rdef_cbuffer_count;
  // Names.
  name_ptr = uint32_t((output.size() - rdef_position_dwords) * sizeof(uint32_t));
  uint32_t rdef_xe_transfer_color_name_ptr = name_ptr;
  if (srv_index_color != UINT32_MAX) {
    name_ptr += dxbc::AppendAlignedString(output, "xe_transfer_color");
  }
  uint32_t rdef_xe_transfer_depth_name_ptr = name_ptr;
  if (srv_index_depth != UINT32_MAX) {
    name_ptr += dxbc::AppendAlignedString(output, "xe_transfer_depth");
  }
  uint32_t rdef_xe_transfer_stencil_name_ptr = name_ptr;
  if (srv_index_stencil != UINT32_MAX) {
    name_ptr += dxbc::AppendAlignedString(output, "xe_transfer_stencil");
  }
  uint32_t rdef_xe_transfer_host_depth_name_ptr = name_ptr;
  if (srv_index_host_depth != UINT32_MAX) {
    name_ptr += dxbc::AppendAlignedString(output, "xe_transfer_host_depth");
  }
  // Bindings.
  uint32_t rdef_binding_position_dwords = uint32_t(output.size());
  output.resize(rdef_binding_position_dwords +
                sizeof(dxbc::RdefInputBind) / sizeof(uint32_t) * rdef_binding_count);
  {
    auto rdef_bindings =
        reinterpret_cast<dxbc::RdefInputBind*>(output.data() + rdef_binding_position_dwords);
    uint32_t rdef_binding_index = 0;
    // xe_transfer_color
    if (srv_index_color != UINT32_MAX) {
      dxbc::RdefInputBind& rdef_binding_color = rdef_bindings[rdef_binding_index++];
      rdef_binding_color.name_ptr = rdef_xe_transfer_color_name_ptr;
      rdef_binding_color.type = dxbc::RdefInputType::kTexture;
      rdef_binding_color.return_type =
          source_color_is_uint ? dxbc::ResourceReturnType::kUInt : dxbc::ResourceReturnType::kFloat;
      if (key.source_msaa_samples != xenos::MsaaSamples::k1X) {
        rdef_binding_color.dimension = dxbc::RdefDimension::kSRVTexture2DMS;
      } else {
        rdef_binding_color.dimension = dxbc::RdefDimension::kSRVTexture2D;
        rdef_binding_color.sample_count = UINT32_MAX;
      }
      rdef_binding_color.bind_point = kTransferSRVRegisterColor;
      rdef_binding_color.bind_count = 1;
      assert_not_zero(source_color_srv_component_mask);
      rdef_binding_color.flags = (32 - rex::lzcnt(source_color_srv_component_mask) - 1)
                                 << dxbc::kRdefInputFlagsComponentsShift;
      rdef_binding_color.id = srv_index_color;
    }
    // xe_transfer_depth
    if (srv_index_depth != UINT32_MAX) {
      dxbc::RdefInputBind& rdef_binding_depth = rdef_bindings[rdef_binding_index++];
      rdef_binding_depth.name_ptr = rdef_xe_transfer_depth_name_ptr;
      rdef_binding_depth.type = dxbc::RdefInputType::kTexture;
      rdef_binding_depth.return_type = dxbc::ResourceReturnType::kFloat;
      if (key.source_msaa_samples != xenos::MsaaSamples::k1X) {
        rdef_binding_depth.dimension = dxbc::RdefDimension::kSRVTexture2DMS;
      } else {
        rdef_binding_depth.dimension = dxbc::RdefDimension::kSRVTexture2D;
        rdef_binding_depth.sample_count = UINT32_MAX;
      }
      rdef_binding_depth.bind_point = kTransferSRVRegisterDepth;
      rdef_binding_depth.bind_count = 1;
      rdef_binding_depth.id = srv_index_depth;
    }
    // xe_transfer_stencil
    if (srv_index_stencil != UINT32_MAX) {
      dxbc::RdefInputBind& rdef_binding_stencil = rdef_bindings[rdef_binding_index++];
      rdef_binding_stencil.name_ptr = rdef_xe_transfer_stencil_name_ptr;
      rdef_binding_stencil.type = dxbc::RdefInputType::kTexture;
      rdef_binding_stencil.return_type = dxbc::ResourceReturnType::kUInt;
      if (key.source_msaa_samples != xenos::MsaaSamples::k1X) {
        rdef_binding_stencil.dimension = dxbc::RdefDimension::kSRVTexture2DMS;
      } else {
        rdef_binding_stencil.dimension = dxbc::RdefDimension::kSRVTexture2D;
        rdef_binding_stencil.sample_count = UINT32_MAX;
      }
      rdef_binding_stencil.bind_point = kTransferSRVRegisterStencil;
      rdef_binding_stencil.bind_count = 1;
      rdef_binding_stencil.flags = dxbc::kRdefInputFlags2Component;
      rdef_binding_stencil.id = srv_index_stencil;
    }
    // xe_transfer_host_depth
    if (srv_index_host_depth != UINT32_MAX) {
      dxbc::RdefInputBind& rdef_binding_host_depth = rdef_bindings[rdef_binding_index++];
      rdef_binding_host_depth.name_ptr = rdef_xe_transfer_host_depth_name_ptr;
      rdef_binding_host_depth.type = dxbc::RdefInputType::kTexture;
      if (key.host_depth_source_is_copy) {
        // Float as uint.
        rdef_binding_host_depth.return_type = dxbc::ResourceReturnType::kUInt;
        rdef_binding_host_depth.dimension = dxbc::RdefDimension::kSRVBuffer;
        rdef_binding_host_depth.sample_count = UINT32_MAX;
      } else {
        rdef_binding_host_depth.return_type = dxbc::ResourceReturnType::kFloat;
        if (key.host_depth_source_msaa_samples != xenos::MsaaSamples::k1X) {
          rdef_binding_host_depth.dimension = dxbc::RdefDimension::kSRVTexture2DMS;
        } else {
          rdef_binding_host_depth.dimension = dxbc::RdefDimension::kSRVTexture2D;
          rdef_binding_host_depth.sample_count = UINT32_MAX;
        }
      }
      rdef_binding_host_depth.bind_point = kTransferSRVRegisterHostDepth;
      rdef_binding_host_depth.bind_count = 1;
      rdef_binding_host_depth.id = srv_index_host_depth;
    }
    // xe_transfer_stencil_mask
    if (cbuffer_index_stencil_mask != UINT32_MAX) {
      dxbc::RdefInputBind& rdef_binding_stencil_mask = rdef_bindings[rdef_binding_index++];
      rdef_binding_stencil_mask.name_ptr = rdef_xe_transfer_stencil_mask_name_ptr;
      rdef_binding_stencil_mask.type = dxbc::RdefInputType::kCbuffer;
      rdef_binding_stencil_mask.bind_point = kTransferCBVRegisterStencilMask;
      rdef_binding_stencil_mask.bind_count = 1;
      rdef_binding_stencil_mask.flags = dxbc::kRdefInputFlagUserPacked;
      rdef_binding_stencil_mask.id = cbuffer_index_stencil_mask;
    }
    // xe_transfer_address
    if (cbuffer_index_address != UINT32_MAX) {
      dxbc::RdefInputBind& rdef_binding_address = rdef_bindings[rdef_binding_index++];
      rdef_binding_address.name_ptr = rdef_xe_transfer_address_name_ptr;
      rdef_binding_address.type = dxbc::RdefInputType::kCbuffer;
      rdef_binding_address.bind_point = kTransferCBVRegisterAddress;
      rdef_binding_address.bind_count = 1;
      rdef_binding_address.flags = dxbc::kRdefInputFlagUserPacked;
      rdef_binding_address.id = cbuffer_index_address;
    }
    // xe_transfer_host_depth_address
    if (cbuffer_index_host_depth_address != UINT32_MAX) {
      dxbc::RdefInputBind& rdef_binding_host_depth_address = rdef_bindings[rdef_binding_index++];
      rdef_binding_host_depth_address.name_ptr = rdef_xe_transfer_host_depth_address_name_ptr;
      rdef_binding_host_depth_address.type = dxbc::RdefInputType::kCbuffer;
      rdef_binding_host_depth_address.bind_point = kTransferCBVRegisterHostDepthAddress;
      rdef_binding_host_depth_address.bind_count = 1;
      rdef_binding_host_depth_address.flags = dxbc::kRdefInputFlagUserPacked;
      rdef_binding_host_depth_address.id = cbuffer_index_host_depth_address;
    }
  }

  // Header.
  {
    auto& rdef_header = *reinterpret_cast<dxbc::RdefHeader*>(output.data() + rdef_position_dwords);
    rdef_header.cbuffer_count = rdef_cbuffer_count;
    rdef_header.cbuffers_ptr =
        uint32_t((rdef_cbuffer_position_dwords - rdef_position_dwords) * sizeof(uint32_t));
    rdef_header.input_bind_count = rdef_binding_count;
    rdef_header.input_binds_ptr =
        uint32_t((rdef_binding_position_dwords - rdef_position_dwords) * sizeof(uint32_t));
    rdef_header.shader_model = dxbc::RdefShaderModel::kPixelShader5_1;
    rdef_header.compile_flags =
        dxbc::kCompileFlagNoPreshader | dxbc::kCompileFlagPreferFlowControl |
        dxbc::kCompileFlagIeeeStrictness | dxbc::kCompileFlagAllResourcesBound;
    // Generator name is right after the header.
    rdef_header.generator_name_ptr = sizeof(dxbc::RdefHeader);
    rdef_header.fourcc = dxbc::RdefHeader::FourCC::k5_1;
    rdef_header.InitializeSizes();
  }

  {
    auto& blob_header = *reinterpret_cast<dxbc::BlobHeader*>(output.data() + blob_position_dwords);
    blob_header.fourcc = dxbc::BlobHeader::FourCC::kResourceDefinition;
    blob_position_dwords = uint32_t(output.size());
    blob_header.size_bytes = (blob_position_dwords - kBlobHeaderSizeDwords) * sizeof(uint32_t) -
                             output[blob_offset_position_dwords++];
  }

  // ***************************************************************************
  // Input signature
  // ***************************************************************************

  // Registers for accessing in the shader code - multiple inputs may be packed
  // into the same register.
  enum InputRegister : uint32_t {
    kInputRegisterPosition,
    kInputRegisterSampleIndex,
    kInputRegisterCount,
  };

  // Position, and for multisampled, sample index.
  uint32_t isgn_parameter_count = 1 + uint32_t(key.dest_msaa_samples != xenos::MsaaSamples::k1X);

  // Reserve space for the header and the parameters.
  output[blob_offset_position_dwords] = uint32_t(blob_position_dwords * sizeof(uint32_t));
  uint32_t isgn_position_dwords = blob_position_dwords + kBlobHeaderSizeDwords;
  output.resize(isgn_position_dwords + sizeof(dxbc::Signature) / sizeof(uint32_t) +
                sizeof(dxbc::SignatureParameter) / sizeof(uint32_t) * isgn_parameter_count);

  // Names (after the parameters).
  name_ptr = uint32_t((output.size() - isgn_position_dwords) * sizeof(uint32_t));
  uint32_t isgn_sv_position_name_ptr = name_ptr;
  name_ptr += dxbc::AppendAlignedString(output, "SV_Position");
  uint32_t isgn_sv_sample_index_name_ptr = name_ptr;
  if (key.dest_msaa_samples != xenos::MsaaSamples::k1X) {
    name_ptr += dxbc::AppendAlignedString(output, "SV_SampleIndex");
  }

  // Header and parameters.
  {
    // Header.
    auto& isgn_header = *reinterpret_cast<dxbc::Signature*>(output.data() + isgn_position_dwords);
    isgn_header.parameter_count = isgn_parameter_count;
    isgn_header.parameter_info_ptr = sizeof(dxbc::Signature);
    // Parameters.
    auto isgn_parameters = reinterpret_cast<dxbc::SignatureParameter*>(
        output.data() + isgn_position_dwords + sizeof(dxbc::Signature) / sizeof(uint32_t));
    uint32_t isgn_parameter_index = 0;
    // SV_Position.xy
    dxbc::SignatureParameter& isgn_sv_position = isgn_parameters[isgn_parameter_index++];
    isgn_sv_position.semantic_name_ptr = isgn_sv_position_name_ptr;
    isgn_sv_position.system_value = dxbc::Name::kPosition;
    isgn_sv_position.component_type = dxbc::SignatureRegisterComponentType::kFloat32;
    isgn_sv_position.register_index = kInputRegisterPosition;
    isgn_sv_position.mask = 0b1111;
    isgn_sv_position.always_reads_mask = 0b0011;
    // SV_SampleIndex
    if (key.dest_msaa_samples != xenos::MsaaSamples::k1X) {
      dxbc::SignatureParameter& isgn_sv_sample_index = isgn_parameters[isgn_parameter_index++];
      isgn_sv_sample_index.semantic_name_ptr = isgn_sv_sample_index_name_ptr;
      isgn_sv_sample_index.system_value = dxbc::Name::kSampleIndex;
      isgn_sv_sample_index.component_type = dxbc::SignatureRegisterComponentType::kUInt32;
      isgn_sv_sample_index.register_index = kInputRegisterSampleIndex;
      isgn_sv_sample_index.mask = 0b0001;
      isgn_sv_sample_index.always_reads_mask = 0b0001;
    }
  }

  {
    auto& blob_header = *reinterpret_cast<dxbc::BlobHeader*>(output.data() + blob_position_dwords);
    blob_header.fourcc = dxbc::BlobHeader::FourCC::kInputSignature;
    blob_position_dwords = uint32_t(output.size());
    blob_header.size_bytes = (blob_position_dwords - kBlobHeaderSizeDwords) * sizeof(uint32_t) -
                             output[blob_offset_position_dwords++];
  }

  // ***************************************************************************
  // Output signature
  // ***************************************************************************

  // Color or depth.
  uint32_t osgn_parameter_count = 0;
  uint32_t osgn_parameter_index_sv_target =
      mode.output == TransferOutput::kColor ? osgn_parameter_count++ : UINT32_MAX;
  uint32_t osgn_parameter_index_sv_depth =
      mode.output == TransferOutput::kDepth ? osgn_parameter_count++ : UINT32_MAX;
  uint32_t osgn_parameter_index_sv_stencil_ref =
      shader_uses_stencil_reference_output ? osgn_parameter_count++ : UINT32_MAX;

  // Reserve space for the header and the parameters.
  output[blob_offset_position_dwords] = uint32_t(blob_position_dwords * sizeof(uint32_t));
  uint32_t osgn_position_dwords = blob_position_dwords + kBlobHeaderSizeDwords;
  output.resize(osgn_position_dwords + sizeof(dxbc::Signature) / sizeof(uint32_t) +
                sizeof(dxbc::SignatureParameter) / sizeof(uint32_t) * osgn_parameter_count);

  // Names (after the parameters).
  name_ptr = uint32_t((output.size() - osgn_position_dwords) * sizeof(uint32_t));
  uint32_t osgn_sv_target_name_ptr = name_ptr;
  if (osgn_parameter_index_sv_target != UINT32_MAX) {
    name_ptr += dxbc::AppendAlignedString(output, "SV_Target");
  }
  uint32_t osgn_sv_depth_name_ptr = name_ptr;
  if (osgn_parameter_index_sv_depth != UINT32_MAX) {
    name_ptr += dxbc::AppendAlignedString(output, "SV_Depth");
  }
  uint32_t osgn_sv_stencil_ref_name_ptr = name_ptr;
  if (osgn_parameter_index_sv_stencil_ref != UINT32_MAX) {
    name_ptr += dxbc::AppendAlignedString(output, "SV_StencilRef");
  }

  bool dest_color_is_uint;
  if (mode.output == TransferOutput::kColor) {
    dest_color_is_uint = IsIntegerTransferFormat(dest_color_format);
  } else {
    dest_color_is_uint = false;
  }

  // Header and parameters.
  {
    // Header.
    auto& osgn_header = *reinterpret_cast<dxbc::Signature*>(output.data() + osgn_position_dwords);
    osgn_header.parameter_count = osgn_parameter_count;
    osgn_header.parameter_info_ptr = sizeof(dxbc::Signature);
    // Parameters.
    auto osgn_parameters = reinterpret_cast<dxbc::SignatureParameter*>(
        output.data() + osgn_position_dwords + sizeof(dxbc::Signature) / sizeof(uint32_t));
    // SV_Target
    if (osgn_parameter_index_sv_target != UINT32_MAX) {
      dxbc::SignatureParameter& osgn_sv_target = osgn_parameters[osgn_parameter_index_sv_target];
      osgn_sv_target.semantic_name_ptr = osgn_sv_target_name_ptr;
      osgn_sv_target.component_type = dest_color_is_uint
                                          ? dxbc::SignatureRegisterComponentType::kUInt32
                                          : dxbc::SignatureRegisterComponentType::kFloat32;
      osgn_sv_target.register_index = 0;
      osgn_sv_target.mask = 0b1111;
    }
    // SV_Depth
    if (osgn_parameter_index_sv_depth != UINT32_MAX) {
      dxbc::SignatureParameter& osgn_sv_depth = osgn_parameters[osgn_parameter_index_sv_depth];
      osgn_sv_depth.semantic_name_ptr = osgn_sv_depth_name_ptr;
      osgn_sv_depth.component_type = dxbc::SignatureRegisterComponentType::kFloat32;
      osgn_sv_depth.register_index = UINT32_MAX;
      osgn_sv_depth.mask = 0b0001;
      osgn_sv_depth.never_writes_mask = 0b1110;
    }
    // SV_StencilRef
    if (osgn_parameter_index_sv_stencil_ref != UINT32_MAX) {
      dxbc::SignatureParameter& osgn_sv_stencil_ref =
          osgn_parameters[osgn_parameter_index_sv_stencil_ref];
      osgn_sv_stencil_ref.semantic_name_ptr = osgn_sv_stencil_ref_name_ptr;
      // Older versions of FXC incorrectly expect SV_StencilRef to be float,
      // it's always uint in DXC and also in the latest versions of FXC.
      osgn_sv_stencil_ref.component_type = dxbc::SignatureRegisterComponentType::kUInt32;
      osgn_sv_stencil_ref.register_index = UINT32_MAX;
      osgn_sv_stencil_ref.mask = 0b0001;
      osgn_sv_stencil_ref.never_writes_mask = 0b1110;
    }
  }

  {
    auto& blob_header = *reinterpret_cast<dxbc::BlobHeader*>(output.data() + blob_position_dwords);
    blob_header.fourcc = dxbc::BlobHeader::FourCC::kOutputSignature;
    blob_position_dwords = uint32_t(output.size());
    blob_header.size_bytes = (blob_position_dwords - kBlobHeaderSizeDwords) * sizeof(uint32_t) -
                             output[blob_offset_position_dwords++];
  }

  // ***************************************************************************
  // Shader program
  // ***************************************************************************

  output[blob_offset_position_dwords] = uint32_t(blob_position_dwords * sizeof(uint32_t));
  uint32_t shex_position_dwords = blob_position_dwords + kBlobHeaderSizeDwords;
  output.resize(shex_position_dwords);

  output.push_back(dxbc::VersionToken(dxbc::ProgramType::kPixelShader, 5, 1));
  // Reserve space for the length token.
  output.push_back(0);

  dxbc::Statistics stat;
  std::memset(&stat, 0, sizeof(dxbc::Statistics));
  PortableDxbcAssembler a(output, stat, options.portable_integer_division);

  a.OpDclGlobalFlags(dxbc::kGlobalFlagAllResourcesBound);
  if (cbuffer_index_stencil_mask != UINT32_MAX) {
    a.OpDclConstantBuffer(
        dxbc::Src::CB(dxbc::Src::Dcl, cbuffer_index_stencil_mask, kTransferCBVRegisterStencilMask,
                      kTransferCBVRegisterStencilMask),
        1);
  }
  if (cbuffer_index_address != UINT32_MAX) {
    a.OpDclConstantBuffer(dxbc::Src::CB(dxbc::Src::Dcl, cbuffer_index_address,
                                        kTransferCBVRegisterAddress, kTransferCBVRegisterAddress),
                          1);
  }
  if (cbuffer_index_host_depth_address != UINT32_MAX) {
    a.OpDclConstantBuffer(
        dxbc::Src::CB(dxbc::Src::Dcl, cbuffer_index_host_depth_address,
                      kTransferCBVRegisterHostDepthAddress, kTransferCBVRegisterHostDepthAddress),
        1);
  }
  auto sample_count = [&](xenos::MsaaSamples samples) -> uint32_t {
    return samples == xenos::MsaaSamples::k1X
               ? 0
               : (samples == xenos::MsaaSamples::k2X && options.msaa_2x_supported ? 2 : 4);
  };
  if (srv_index_color != UINT32_MAX) {
    a.OpDclResource(
        key.source_msaa_samples != xenos::MsaaSamples::k1X ? dxbc::ResourceDimension::kTexture2DMS
                                                           : dxbc::ResourceDimension::kTexture2D,
        dxbc::ResourceReturnTypeX4Token(source_color_is_uint ? dxbc::ResourceReturnType::kUInt
                                                             : dxbc::ResourceReturnType::kFloat),
        dxbc::Src::T(dxbc::Src::Dcl, srv_index_color, kTransferSRVRegisterColor,
                     kTransferSRVRegisterColor),
        0, sample_count(key.source_msaa_samples));
  }
  if (srv_index_depth != UINT32_MAX) {
    a.OpDclResource(key.source_msaa_samples != xenos::MsaaSamples::k1X
                        ? dxbc::ResourceDimension::kTexture2DMS
                        : dxbc::ResourceDimension::kTexture2D,
                    dxbc::ResourceReturnTypeX4Token(dxbc::ResourceReturnType::kFloat),
                    dxbc::Src::T(dxbc::Src::Dcl, srv_index_depth, kTransferSRVRegisterDepth,
                                 kTransferSRVRegisterDepth),
                    0, sample_count(key.source_msaa_samples));
  }
  if (srv_index_stencil != UINT32_MAX) {
    a.OpDclResource(key.source_msaa_samples != xenos::MsaaSamples::k1X
                        ? dxbc::ResourceDimension::kTexture2DMS
                        : dxbc::ResourceDimension::kTexture2D,
                    dxbc::ResourceReturnTypeX4Token(dxbc::ResourceReturnType::kUInt),
                    dxbc::Src::T(dxbc::Src::Dcl, srv_index_stencil, kTransferSRVRegisterStencil,
                                 kTransferSRVRegisterStencil),
                    0, sample_count(key.source_msaa_samples));
  }
  if (srv_index_host_depth != UINT32_MAX) {
    a.OpDclResource(
        key.host_depth_source_is_copy
            ? dxbc::ResourceDimension::kBuffer
            : (key.host_depth_source_msaa_samples != xenos::MsaaSamples::k1X
                   ? dxbc::ResourceDimension::kTexture2DMS
                   : dxbc::ResourceDimension::kTexture2D),
        dxbc::ResourceReturnTypeX4Token(dxbc::ResourceReturnType::kFloat),
        dxbc::Src::T(dxbc::Src::Dcl, srv_index_host_depth, kTransferSRVRegisterHostDepth,
                     kTransferSRVRegisterHostDepth),
        0, key.host_depth_source_is_copy ? 0 : sample_count(key.host_depth_source_msaa_samples));
  }
  a.OpDclInputPSSIV(dxbc::InterpolationMode::kLinearNoPerspective,
                    dxbc::Dest::V1D(kInputRegisterPosition, 0b0011), dxbc::Name::kPosition);
  if (key.dest_msaa_samples != xenos::MsaaSamples::k1X) {
    a.OpDclInputPSSGV(dxbc::Dest::V1D(kInputRegisterSampleIndex, 0b0001), dxbc::Name::kSampleIndex);
  }
  if (osgn_parameter_index_sv_target != UINT32_MAX) {
    a.OpDclOutput(dxbc::Dest::O(0));
  }
  if (osgn_parameter_index_sv_depth != UINT32_MAX) {
    a.OpDclOutput(dxbc::Dest::ODepth());
  }
  if (osgn_parameter_index_sv_stencil_ref != UINT32_MAX) {
    a.OpDclOutput(dxbc::Dest::OStencilRef());
  }
  // r0:r2 are involved at least in common addressing code. Texture loads
  // usually can overwrite some of the addressing temps as they are only needed
  // for the coordinates for that load. Currently 3 temps are enough.
  a.OpDclTemps(3);

  uint32_t draw_resolution_scale_x = options.resolution_scale_x;
  uint32_t draw_resolution_scale_y = options.resolution_scale_y;

  uint32_t tile_width_samples = xenos::kEdramTileWidthSamples * draw_resolution_scale_x;
  uint32_t tile_height_samples = xenos::kEdramTileHeightSamples * draw_resolution_scale_y;

  // Split the destination pixel index into 32bpp tile in r0.zw and
  // 32bpp-tile-relative pixel index in r0.xy.
  // r0.xy = pixel XY as uint
  a.OpFToU(dxbc::Dest::R(0, 0b0011), dxbc::Src::V1D(kInputRegisterPosition));
  uint32_t dest_tile_width_pixels =
      tile_width_samples >>
      (uint32_t(dest_is_64bpp) + uint32_t(key.dest_msaa_samples >= xenos::MsaaSamples::k4X));
  uint32_t dest_tile_height_pixels =
      tile_height_samples >> uint32_t(key.dest_msaa_samples >= xenos::MsaaSamples::k2X);
  // r0.xy = destination pixel XY index within the 32bpp tile
  // r0.zw = 32bpp tile XY index
  a.OpUDiv(dxbc::Dest::R(0, 0b1100), dxbc::Dest::R(0, 0b0011), dxbc::Src::R(0, 0b01000100),
           dxbc::Src::LU(dest_tile_width_pixels, dest_tile_height_pixels, dest_tile_width_pixels,
                         dest_tile_height_pixels));

  // r1.x = destination pitch in 32bpp tiles
  a.OpUBFE(dxbc::Dest::R(1, 0b0001), dxbc::Src::LU(xenos::kEdramPitchTilesBits), dxbc::Src::LU(0),
           dxbc::Src::CB(cbuffer_index_address, kTransferCBVRegisterAddress, 0, dxbc::Src::kXXXX));
  // r0.z = 32bpp tile index relative to the destination base
  // r0.w = free
  // r1.x = free
  a.OpUMAd(dxbc::Dest::R(0, 0b0100), dxbc::Src::R(1, dxbc::Src::kXXXX),
           dxbc::Src::R(0, dxbc::Src::kWWWW), dxbc::Src::R(0, dxbc::Src::kZZZZ));

  // Now the tile index doesn't have any dependencies on the destination. The
  // dword index within the source tile, however, is calculated from both the
  // source and the destination pixel size, sample count and color vs. depth.

  // Source can be 64bpp or 32bpp - depth if only depth is available, color in
  // all other cases.

  // Load the source to r1 (or low to r0, high to r1 if need 64bpp color as the
  // result, as the address is loaded to r1).

  // Source pixel and sample index within the 32bpp tile.
  // X to r1.x (or keep r0.x if not modifying).
  // Y to r1.y (or keep r0.y if not modifying).
  // Sample index to r1.z (or use v# if not modifying); r1.z will also be set
  // to 0 before sampling for the LOD of the single-sampled source (needs to
  // be in the register).
  // If 64bpp -> 32bpp, also the needed half in r0.w.

  dxbc::Src dest_sample(dxbc::Src::V1D(kInputRegisterSampleIndex, dxbc::Src::kXXXX));
  dxbc::Src source_sample(dest_sample);
  uint32_t source_tile_pixel_x_reg = 0;
  uint32_t source_tile_pixel_y_reg = 0;

  // First sample bit at 4x in Direct3D 10.1+ - horizontal sample.
  // Second sample bit at 4x in Direct3D 10.1+ - vertical sample.
  // At 2x:
  // - Native 2x: top is 1 in Direct3D 10.1+, bottom is 0.
  // - 2x as 4x: top is 0, bottom is 3.

  if (!source_is_64bpp && dest_is_64bpp) {
    // 32bpp -> 64bpp, need two samples of the source.
    if (key.source_msaa_samples >= xenos::MsaaSamples::k4X) {
      // 32bpp -> 64bpp, 4x ->.
      // Source has 32bpp halves in two adjacent samples.
      if (key.dest_msaa_samples >= xenos::MsaaSamples::k4X) {
        // 32bpp -> 64bpp, 4x -> 4x.
        // 1 destination horizontal sample = 2 source horizontal samples.
        // D p0,0 s0,0 = S p0,0 s0,0 | S p0,0 s1,0
        // D p0,0 s1,0 = S p1,0 s0,0 | S p1,0 s1,0
        // D p0,0 s0,1 = S p0,0 s0,1 | S p0,0 s1,1
        // D p0,0 s1,1 = S p1,0 s0,1 | S p1,0 s1,1
        // Thus destination horizontal sample -> source horizontal pixel,
        // vertical samples are 1:1.
        a.OpAnd(dxbc::Dest::R(1, 0b0100), dest_sample, dxbc::Src::LU(0b10));
        source_sample = dxbc::Src::R(1, dxbc::Src::kZZZZ);
        a.OpBFI(dxbc::Dest::R(1, 0b0001), dxbc::Src::LU(31), dxbc::Src::LU(1),
                dxbc::Src::R(0, dxbc::Src::kXXXX),
                dxbc::Src::V1D(kInputRegisterSampleIndex, dxbc::Src::kXXXX));
        source_tile_pixel_x_reg = 1;
      } else if (key.dest_msaa_samples == xenos::MsaaSamples::k2X) {
        // 32bpp -> 64bpp, 4x -> 2x.
        // 1 destination horizontal pixel = 2 source horizontal samples.
        // D p0,0 s0 = S p0,0 s0,0 | S p0,0 s1,0
        // D p0,0 s1 = S p0,0 s0,1 | S p0,0 s1,1
        // D p1,0 s0 = S p1,0 s0,0 | S p1,0 s1,0
        // D p1,0 s1 = S p1,0 s0,1 | S p1,0 s1,1
        // Pixel index can be reused. Sample 1 (for native 2x) or 0 (for 2x as
        // 4x) should become samples 01, sample 0 or 3 should become samples 23.
        source_sample = dxbc::Src::R(1, dxbc::Src::kZZZZ);
        if (options.msaa_2x_supported) {
          a.OpXOr(dxbc::Dest::R(1, 0b0100), dest_sample, dxbc::Src::LU(1));
          a.OpIShL(dxbc::Dest::R(1, 0b0100), source_sample, dxbc::Src::LU(1));
        } else {
          a.OpAnd(dxbc::Dest::R(1, 0b0100), dest_sample, dxbc::Src::LU(0b10));
        }
      } else {
        // 32bpp -> 64bpp, 4x -> 1x.
        // 1 destination horizontal pixel = 2 source horizontal samples.
        // D p0,0 = S p0,0 s0,0 | S p0,0 s1,0
        // D p0,1 = S p0,0 s0,1 | S p0,0 s1,1
        // Horizontal pixel index can be reused. Vertical pixel 1 should
        // become sample 2.
        a.OpBFI(dxbc::Dest::R(1, 0b0100), dxbc::Src::LU(1), dxbc::Src::LU(1),
                dxbc::Src::R(0, dxbc::Src::kYYYY), dxbc::Src::LU(0));
        source_sample = dxbc::Src::R(1, dxbc::Src::kZZZZ);
        a.OpUShR(dxbc::Dest::R(1, 0b0010), dxbc::Src::R(0, dxbc::Src::kYYYY), dxbc::Src::LU(1));
        source_tile_pixel_y_reg = 1;
      }
    } else {
      // 32bpp -> 64bpp, 1x/2x ->.
      // Source has 32bpp halves in two adjacent pixels.
      if (key.dest_msaa_samples >= xenos::MsaaSamples::k4X) {
        // 32bpp -> 64bpp, 1x/2x -> 4x.
        // The X part.
        // 1 destination horizontal sample = 2 source horizontal pixels.
        a.OpIShL(dxbc::Dest::R(1, 0b0001), dxbc::Src::R(0, dxbc::Src::kXXXX), dxbc::Src::LU(2));
        a.OpBFI(dxbc::Dest::R(1, 0b0001), dxbc::Src::LU(1), dxbc::Src::LU(1),
                dxbc::Src::V1D(kInputRegisterSampleIndex, dxbc::Src::kXXXX),
                dxbc::Src::R(1, dxbc::Src::kXXXX));
        source_tile_pixel_x_reg = 1;
        // Y is handled by common code.
      } else {
        // 32bpp -> 64bpp, 1x/2x -> 1x/2x.
        // The X part.
        // 1 destination horizontal pixel = 2 source horizontal pixels.
        a.OpIShL(dxbc::Dest::R(1, 0b0001), dxbc::Src::R(0, dxbc::Src::kXXXX), dxbc::Src::LU(1));
        source_tile_pixel_x_reg = 1;
        // Y is handled by common code.
      }
    }
  } else if (source_is_64bpp && !dest_is_64bpp) {
    // 64bpp -> 32bpp, also the half to r0.w.
    if (key.dest_msaa_samples >= xenos::MsaaSamples::k4X) {
      // 64bpp -> 32bpp, -> 4x.
      // The needed half is in the destination horizontal sample index.
      if (key.source_msaa_samples >= xenos::MsaaSamples::k4X) {
        // 64bpp -> 32bpp, 4x -> 4x.
        // D p0,0 s0,0 = S s0,0 low
        // D p0,0 s1,0 = S s0,0 high
        // D p1,0 s0,0 = S s1,0 low
        // D p1,0 s1,0 = S s1,0 high
        // Vertical pixel and sample (second bit) addressing is the same.
        // However, 1 horizontal destination pixel = 1 horizontal source sample.
        a.OpBFI(dxbc::Dest::R(1, 0b0100), dxbc::Src::LU(1), dxbc::Src::LU(0),
                dxbc::Src::R(0, dxbc::Src::kXXXX), dest_sample);
        source_sample = dxbc::Src::R(1, dxbc::Src::kZZZZ);
        // 2 destination horizontal samples = 1 source horizontal sample, thus
        // 2 destination horizontal pixels = 1 source horizontal pixel.
        a.OpUShR(dxbc::Dest::R(1, 0b0001), dxbc::Src::R(0, dxbc::Src::kXXXX), dxbc::Src::LU(1));
        source_tile_pixel_x_reg = 1;
      } else {
        // 64bpp -> 32bpp, 1x/2x -> 4x.
        // 2 destination horizontal samples = 1 source horizontal pixel, thus
        // 1 destination horizontal pixel = 1 source horizontal pixel. Can reuse
        // horizontal pixel index.
        // Y is handled by common code.
      }
      // Half in r0.w from the destination horizontal sample index.
      a.OpAnd(dxbc::Dest::R(0, 0b1000), dest_sample, dxbc::Src::LU(1));
    } else {
      // 64bpp -> 32bpp, -> 1x/2x.
      // The needed half is in the destination horizontal pixel index.
      if (key.source_msaa_samples >= xenos::MsaaSamples::k4X) {
        // 64bpp -> 32bpp, 4x -> 1x/2x.
        // (Destination horizontal pixel >> 1) & 1 = source horizontal sample
        // (first bit).
        a.OpUBFE(dxbc::Dest::R(1, 0b0100), dxbc::Src::LU(1), dxbc::Src::LU(1),
                 dxbc::Src::R(0, dxbc::Src::kXXXX));
        source_sample = dxbc::Src::R(1, dxbc::Src::kZZZZ);
        if (key.dest_msaa_samples == xenos::MsaaSamples::k2X) {
          // 64bpp -> 32bpp, 4x -> 2x.
          // Destination vertical samples (1/0 in the first bit for native 2x or
          // 0/1 in the second bit for 2x as 4x) = source vertical samples
          // (second bit).
          if (options.msaa_2x_supported) {
            a.OpBFI(dxbc::Dest::R(1, 0b0100), dxbc::Src::LU(1), dxbc::Src::LU(1), dest_sample,
                    source_sample);
            a.OpXOr(dxbc::Dest::R(1, 0b0100), source_sample, dxbc::Src::LU(1 << 1));
          } else {
            a.OpBFI(dxbc::Dest::R(1, 0b0100), dxbc::Src::LU(1), dxbc::Src::LU(0), source_sample,
                    dest_sample);
          }
        } else {
          // 64bpp -> 32bpp, 4x -> 1x.
          // 1 destination vertical pixel = 1 source vertical sample.
          a.OpBFI(dxbc::Dest::R(1, 0b0100), dxbc::Src::LU(1), dxbc::Src::LU(1),
                  dxbc::Src::R(0, dxbc::Src::kYYYY), source_sample);
          a.OpUShR(dxbc::Dest::R(1, 0b0010), dxbc::Src::R(0, dxbc::Src::kYYYY), dxbc::Src::LU(1));
          source_tile_pixel_y_reg = 1;
        }
        // 2 destination horizontal pixels = 1 source horizontal sample.
        // 4 destination horizontal pixels = 1 source horizontal pixel.
        a.OpUShR(dxbc::Dest::R(1, 0b0001), dxbc::Src::R(0, dxbc::Src::kXXXX), dxbc::Src::LU(2));
        source_tile_pixel_x_reg = 1;
      } else {
        // 64bpp -> 32bpp, 1x/2x -> 1x/2x.
        // The X part.
        // 2 destination horizontal pixels = 1 destination source pixel.
        a.OpUShR(dxbc::Dest::R(1, 0b0001), dxbc::Src::R(0, dxbc::Src::kXXXX), dxbc::Src::LU(1));
        source_tile_pixel_x_reg = 1;
        // Y is handled by common code.
      }
      // Half in r0.w from the destination horizontal pixel index.
      a.OpAnd(dxbc::Dest::R(0, 0b1000), dxbc::Src::R(0, dxbc::Src::kXXXX), dxbc::Src::LU(1));
    }
  } else {
    // Same bit count.
    if (key.source_msaa_samples != key.dest_msaa_samples) {
      if (key.source_msaa_samples >= xenos::MsaaSamples::k4X) {
        // Same BPP, 4x -> 1x/2x.
        if (key.dest_msaa_samples == xenos::MsaaSamples::k2X) {
          // Same BPP, 4x -> 2x.
          // Horizontal pixels to samples. Vertical sample (1/0 in the first bit
          // for native 2x or 0/1 in the second bit for 2x as 4x) to second
          // sample bit.
          source_sample = dxbc::Src::R(1, dxbc::Src::kZZZZ);
          if (options.msaa_2x_supported) {
            a.OpBFI(dxbc::Dest::R(1, 0b0100), dxbc::Src::LU(31), dxbc::Src::LU(1), dest_sample,
                    dxbc::Src::R(0, dxbc::Src::kXXXX));
            a.OpXOr(dxbc::Dest::R(1, 0b0100), source_sample, dxbc::Src::LU(1 << 1));
          } else {
            a.OpBFI(dxbc::Dest::R(1, 0b0100), dxbc::Src::LU(1), dxbc::Src::LU(0),
                    dxbc::Src::R(0, dxbc::Src::kXXXX), dest_sample);
          }
          a.OpUShR(dxbc::Dest::R(1, 0b0001), dxbc::Src::R(0, dxbc::Src::kXXXX), dxbc::Src::LU(1));
          source_tile_pixel_x_reg = 1;
        } else {
          // Same BPP, 4x -> 1x.
          // Pixels to samples.
          a.OpAnd(dxbc::Dest::R(1, 0b0100), dxbc::Src::R(0, dxbc::Src::kXXXX), dxbc::Src::LU(1));
          source_sample = dxbc::Src::R(1, dxbc::Src::kZZZZ);
          a.OpBFI(dxbc::Dest::R(1, 0b0100), dxbc::Src::LU(1), dxbc::Src::LU(1),
                  dxbc::Src::R(0, dxbc::Src::kYYYY), source_sample);
          a.OpUShR(dxbc::Dest::R(1, 0b0011), dxbc::Src::R(0), dxbc::Src::LU(1));
          source_tile_pixel_x_reg = 1;
          source_tile_pixel_y_reg = 1;
        }
      } else {
        // Same BPP, 1x/2x -> 1x/2x/4x (as long as they're different).
        // Only the X part - Y is handled by common code.
        if (key.dest_msaa_samples >= xenos::MsaaSamples::k4X) {
          // Horizontal samples to pixels.
          a.OpBFI(dxbc::Dest::R(1, 0b0001), dxbc::Src::LU(31), dxbc::Src::LU(1),
                  dxbc::Src::R(0, dxbc::Src::kXXXX), dest_sample);
          source_tile_pixel_x_reg = 1;
        }
      }
    }
  }
  // Common source Y and sample index for 1x/2x AA sources, independent of bits
  // per sample.
  if (key.source_msaa_samples < xenos::MsaaSamples::k4X &&
      key.source_msaa_samples != key.dest_msaa_samples) {
    if (key.dest_msaa_samples >= xenos::MsaaSamples::k4X) {
      // 1x/2x -> 4x.
      if (key.source_msaa_samples == xenos::MsaaSamples::k2X) {
        // 2x -> 4x.
        // Vertical samples (second bit) of 4x destination to vertical sample
        // (1, 0 for native 2x, or 0, 3 for 2x as 4x) of 2x source.
        a.OpUShR(dxbc::Dest::R(1, 0b0100), dest_sample, dxbc::Src::LU(1));
        source_sample = dxbc::Src::R(1, dxbc::Src::kZZZZ);
        if (options.msaa_2x_supported) {
          a.OpXOr(dxbc::Dest::R(1, 0b0100), source_sample, dxbc::Src::LU(1));
        } else {
          a.OpBFI(dxbc::Dest::R(1, 0b0100), dxbc::Src::LU(1), dxbc::Src::LU(1), source_sample,
                  source_sample);
        }
      } else {
        // 1x -> 4x.
        // Vertical samples (second bit) to Y pixels.
        a.OpUShR(dxbc::Dest::R(1, 0b0010), dest_sample, dxbc::Src::LU(1));
        a.OpBFI(dxbc::Dest::R(1, 0b0010), dxbc::Src::LU(31), dxbc::Src::LU(1),
                dxbc::Src::R(0, dxbc::Src::kYYYY), dxbc::Src::R(1, dxbc::Src::kYYYY));
        source_tile_pixel_y_reg = 1;
      }
    } else {
      // 1x/2x -> different 1x/2x.
      if (key.source_msaa_samples == xenos::MsaaSamples::k2X) {
        // 2x -> 1x.
        // Vertical pixels of 2x destination to vertical samples (1, 0 for
        // native 2x, or 0, 3 for 2x as 4x) of 1x source.
        a.OpAnd(dxbc::Dest::R(1, 0b0100), dxbc::Src::R(0, dxbc::Src::kYYYY), dxbc::Src::LU(1));
        source_sample = dxbc::Src::R(1, dxbc::Src::kZZZZ);
        if (options.msaa_2x_supported) {
          a.OpXOr(dxbc::Dest::R(1, 0b0100), source_sample, dxbc::Src::LU(1));
        } else {
          a.OpBFI(dxbc::Dest::R(1, 0b0100), dxbc::Src::LU(1), dxbc::Src::LU(1), source_sample,
                  source_sample);
        }
        a.OpUShR(dxbc::Dest::R(1, 0b0010), dxbc::Src::R(0, dxbc::Src::kYYYY), dxbc::Src::LU(1));
        source_tile_pixel_y_reg = 1;
      } else {
        // 1x -> 2x.
        // Vertical samples (1/0 in the first bit for native 2x or 0/1 in the
        // second bit for 2x as 4x) of 2x destination to vertical pixels of 1x
        // source.
        if (options.msaa_2x_supported) {
          a.OpBFI(dxbc::Dest::R(1, 0b0010), dxbc::Src::LU(31), dxbc::Src::LU(1),
                  dxbc::Src::R(0, dxbc::Src::kYYYY), dest_sample);
          a.OpXOr(dxbc::Dest::R(1, 0b0010), dxbc::Src::R(1, dxbc::Src::kYYYY), dxbc::Src::LU(1));
        } else {
          a.OpUShR(dxbc::Dest::R(1, 0b0010), dest_sample, dxbc::Src::LU(1));
          a.OpBFI(dxbc::Dest::R(1, 0b0010), dxbc::Src::LU(31), dxbc::Src::LU(1),
                  dxbc::Src::R(0, dxbc::Src::kYYYY), dxbc::Src::R(1, dxbc::Src::kYYYY));
        }
        source_tile_pixel_y_reg = 1;
      }
    }
  }

  uint32_t source_pixel_width_dwords_log2 =
      uint32_t(key.source_msaa_samples >= xenos::MsaaSamples::k4X) + uint32_t(source_is_64bpp);

  if (source_is_color != dest_is_color) {
    // Copying between color and depth / stencil - swap 40-32bpp-sample columns
    // in the pixel index within the source 32bpp tile using r1.w as temporary.
    uint32_t source_32bpp_tile_half_pixels =
        tile_width_samples >> (1 + source_pixel_width_dwords_log2);
    a.OpULT(dxbc::Dest::R(1, 0b1000), dxbc::Src::R(source_tile_pixel_x_reg, dxbc::Src::kXXXX),
            dxbc::Src::LU(source_32bpp_tile_half_pixels));
    a.OpMovC(dxbc::Dest::R(1, 0b1000), dxbc::Src::R(1, dxbc::Src::kWWWW),
             dxbc::Src::LI(int32_t(source_32bpp_tile_half_pixels)),
             dxbc::Src::LI(-int32_t(source_32bpp_tile_half_pixels)));
    a.OpIAdd(dxbc::Dest::R(1, 0b0001), dxbc::Src::R(source_tile_pixel_x_reg, dxbc::Src::kXXXX),
             dxbc::Src::R(1, dxbc::Src::kWWWW));
    source_tile_pixel_x_reg = 1;
    // r1.w = free
  }

  // Current register allocation:
  // r0.xy = pixel index within the destination 32bpp tile
  // r0.z = 32bpp tile index relative to the destination base
  // r0.w for 64bpp -> 32bpp - needed 32bpp half index of 64bpp data
  // r1.xy = pixel index within the source 32bpp tile
  // r1.z for 2x/4x -> = sample index within the source pixel

  // Apply the source 32bpp tile index.
  // r1.w = destination to source EDRAM tile adjustment
  a.OpIBFE(dxbc::Dest::R(1, 0b1000), dxbc::Src::LU(xenos::kEdramBaseTilesBits + 1),
           dxbc::Src::LU(xenos::kEdramPitchTilesBits * 2),
           dxbc::Src::CB(cbuffer_index_address, kTransferCBVRegisterAddress, 0, dxbc::Src::kXXXX));
  // r1.w = 32bpp tile index within the source, or the tile index within the
  //        source minus the EDRAM tile count if transferring across addressing
  //        wrapping (if negative)
  a.OpIAdd(dxbc::Dest::R(1, 0b1000), dxbc::Src::R(0, dxbc::Src::kZZZZ),
           dxbc::Src::R(1, dxbc::Src::kWWWW));
  // r1.w = 32bpp tile index within the source
  a.OpAnd(dxbc::Dest::R(1, 0b1000), dxbc::Src::R(1, dxbc::Src::kWWWW),
          dxbc::Src::LU(xenos::kEdramTileCount - 1));
  // r2.x = source pitch in 32bpp tiles
  a.OpUBFE(dxbc::Dest::R(2, 0b0001), dxbc::Src::LU(xenos::kEdramPitchTilesBits),
           dxbc::Src::LU(xenos::kEdramPitchTilesBits),
           dxbc::Src::CB(cbuffer_index_address, kTransferCBVRegisterAddress, 0, dxbc::Src::kXXXX));
  // r1.w = source tile row
  // r2.x = source 32bpp tile within the row
  a.OpUDiv(dxbc::Dest::R(1, 0b1000), dxbc::Dest::R(2, 0b0001), dxbc::Src::R(1, dxbc::Src::kWWWW),
           dxbc::Src::R(2, dxbc::Src::kXXXX));
  // r1.x = pixel X within the source texture
  // r2.x = free
  a.OpUMAd(
      dxbc::Dest::R(1, 0b0001), dxbc::Src::LU(tile_width_samples >> source_pixel_width_dwords_log2),
      dxbc::Src::R(2, dxbc::Src::kXXXX), dxbc::Src::R(source_tile_pixel_x_reg, dxbc::Src::kXXXX));
  // r1.y = pixel Y within the source texture
  // r1.w = free
  a.OpUMAd(dxbc::Dest::R(1, 0b0010),
           dxbc::Src::LU(tile_height_samples >>
                         uint32_t(key.source_msaa_samples >= xenos::MsaaSamples::k2X)),
           dxbc::Src::R(1, dxbc::Src::kWWWW),
           dxbc::Src::R(source_tile_pixel_y_reg, dxbc::Src::kYYYY));

  // Load the source to r1, or, for 32bpp | 32bpp -> 64bpp, the first dword to
  // r0 since addressing will not be needed anymore for color, and the second
  // dword to r1.
  // Depth will be loaded to w before loading stencil (so it doesn't overwrite
  // the coordinates needed for stencil loading).
  // Stencil will be loaded to x.
  // Color will be loaded to x...w.
  bool source_load_is_two_dwords = !source_is_64bpp && dest_is_64bpp;
  if (key.source_msaa_samples != xenos::MsaaSamples::k1X) {
    for (uint32_t i = 0; i <= uint32_t(source_load_is_two_dwords); ++i) {
      uint32_t source_load_register = source_load_is_two_dwords ? i : 1;
      if (srv_index_depth != UINT32_MAX) {
        a.OpLdMS(dxbc::Dest::R(source_load_register, 0b1000), dxbc::Src::R(1), 0b0011,
                 dxbc::Src::T(srv_index_depth, kTransferSRVRegisterDepth, dxbc::Src::kXXXX),
                 source_sample);
      }
      if (srv_index_stencil != UINT32_MAX) {
        a.OpLdMS(dxbc::Dest::R(source_load_register, 0b0001), dxbc::Src::R(1), 0b0011,
                 dxbc::Src::T(srv_index_stencil, kTransferSRVRegisterStencil, dxbc::Src::kYYYY),
                 source_sample);
      } else if (srv_index_color != UINT32_MAX) {
        a.OpLdMS(dxbc::Dest::R(source_load_register, source_color_srv_component_mask),
                 dxbc::Src::R(1), 0b0011, dxbc::Src::T(srv_index_color, kTransferSRVRegisterColor),
                 source_sample);
      }
      if (source_load_is_two_dwords && !i) {
        // Go to the next sample or pixel along X if need to load two dwords.
        if (key.source_msaa_samples >= xenos::MsaaSamples::k4X) {
          a.OpOr(dxbc::Dest::R(1, 0b0100), source_sample, dxbc::Src::LU(1));
          source_sample = dxbc::Src::R(1, dxbc::Src::kZZZZ);
        } else {
          a.OpOr(dxbc::Dest::R(1, 0b0001), dxbc::Src::R(1, dxbc::Src::kXXXX), dxbc::Src::LU(1));
        }
      }
    }
  } else {
    // Write zero to the LOD index in r1.z.
    a.OpMov(dxbc::Dest::R(1, 0b0100), dxbc::Src::LU(0));
    dxbc::Src source_coordinates(dxbc::Src::R(1, 0b10000100));
    for (uint32_t i = 0; i <= uint32_t(source_load_is_two_dwords); ++i) {
      uint32_t source_load_register = source_load_is_two_dwords ? i : 1;
      if (srv_index_depth != UINT32_MAX) {
        a.OpLd(dxbc::Dest::R(source_load_register, 0b1000), source_coordinates, 0b1011,
               dxbc::Src::T(srv_index_depth, kTransferSRVRegisterDepth, dxbc::Src::kXXXX));
      }
      if (srv_index_stencil != UINT32_MAX) {
        a.OpLd(dxbc::Dest::R(source_load_register, 0b0001), source_coordinates, 0b1011,
               dxbc::Src::T(srv_index_stencil, kTransferSRVRegisterStencil, dxbc::Src::kYYYY));
      } else if (srv_index_color != UINT32_MAX) {
        a.OpLd(dxbc::Dest::R(source_load_register, source_color_srv_component_mask),
               source_coordinates, 0b1011,
               dxbc::Src::T(srv_index_color, kTransferSRVRegisterColor));
      }
      if (source_load_is_two_dwords && !i) {
        // Go to the next pixel along X if need to load two dwords.
        a.OpOr(dxbc::Dest::R(1, 0b0001), dxbc::Src::R(1, dxbc::Src::kXXXX), dxbc::Src::LU(1));
      }
    }
  }
  // Pick the needed 32bpp half of the 64bpp color based on r0.w.
  if (source_is_64bpp && !dest_is_64bpp) {
    uint32_t source_color_half_component_count = source_color_format_component_count >> 1;
    if (dest_is_stencil_bit) {
      a.OpMovC(dxbc::Dest::R(1, 0b0001), dxbc::Src::R(0, dxbc::Src::kWWWW),
               dxbc::Src::R(1).Select(source_color_half_component_count),
               dxbc::Src::R(1, dxbc::Src::kXXXX));
    } else {
      uint32_t color_high_dword_swizzle =
          (source_color_half_component_count * 0b01010101) &
          ~((uint32_t(1) << (source_color_half_component_count * 2)) - 1);
      for (uint32_t i = 0; i < source_color_half_component_count; ++i) {
        color_high_dword_swizzle |= (source_color_half_component_count + i) << (i * 2);
      }
      a.OpMovC(dxbc::Dest::R(1, (1 << source_color_half_component_count) - 1),
               dxbc::Src::R(0, dxbc::Src::kWWWW), dxbc::Src::R(1, color_high_dword_swizzle),
               dxbc::Src::R(1));
    }
  }

  if (osgn_parameter_index_sv_stencil_ref != UINT32_MAX && srv_index_stencil != UINT32_MAX) {
    // For the depth -> depth case, write the stencil loaded to r1.x directly to
    // the output.
    assert_true(mode.output == TransferOutput::kDepth);
    a.OpMov(dxbc::Dest::OStencilRef(), dxbc::Src::R(1, dxbc::Src::kXXXX));
  }

  if (dest_is_64bpp) {
    // Handle construction of 64bpp color, either from two 32-bit samples in r0
    // and r1, or from one 64bpp sample in r1. Using r2.x as temporary when
    // needed.
    // If color_packed_in_r0x_and_r1x, use the generic path for combining two
    // 32-bit samples - as raw in r0.x and r1.x - into the destination.
    bool color_packed_in_r0x_and_r1x = false;
    if (source_is_color) {
      switch (source_color_format) {
        case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA: {
          // 8_8_8_8_GAMMA is represented by linear stored in
          // R16G16B16A16_UNORM.
          for (uint32_t i = 0; i < 2; ++i) {
            for (uint32_t j = 0; j < 3; ++j) {
              DxbcFloatEmitter::PreSaturatedLinearToPWLGamma(a, i, j, i, j, 2, 0, 2, 1);
            }
          }
        }
          [[fallthrough]];
        case xenos::ColorRenderTargetFormat::k_8_8_8_8: {
          color_packed_in_r0x_and_r1x = true;
          for (uint32_t i = 0; i < 2; ++i) {
            a.OpMAd(dxbc::Dest::R(i), dxbc::Src::R(i), dxbc::Src::LF(255.0f), dxbc::Src::LF(0.5f));
            a.OpFToU(dxbc::Dest::R(i), dxbc::Src::R(i));
            for (uint32_t j = 1; j < 4; ++j) {
              a.OpBFI(dxbc::Dest::R(i, 0b0001), dxbc::Src::LU(8), dxbc::Src::LU(j * 8),
                      dxbc::Src::R(i).Select(j), dxbc::Src::R(i, dxbc::Src::kXXXX));
            }
          }
        } break;
        case xenos::ColorRenderTargetFormat::k_2_10_10_10:
        case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10: {
          color_packed_in_r0x_and_r1x = true;
          for (uint32_t i = 0; i < 2; ++i) {
            a.OpMAd(dxbc::Dest::R(i), dxbc::Src::R(i),
                    dxbc::Src::LF(1023.0f, 1023.0f, 1023.0f, 3.0f), dxbc::Src::LF(0.5f));
            a.OpFToU(dxbc::Dest::R(i), dxbc::Src::R(i));
            for (uint32_t j = 1; j < 4; ++j) {
              a.OpBFI(dxbc::Dest::R(i, 0b0001), dxbc::Src::LU(j == 3 ? 2 : 10),
                      dxbc::Src::LU(j * 10), dxbc::Src::R(i).Select(j),
                      dxbc::Src::R(i, dxbc::Src::kXXXX));
            }
          }
        } break;
        case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
        case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16: {
          color_packed_in_r0x_and_r1x = true;
          for (uint32_t i = 0; i < 2; ++i) {
            // Float16 has a wider range for both color and alpha, also NaNs -
            // clamp and convert.
            for (uint32_t j = 0; j < 3; ++j) {
              DxbcFloatEmitter::UnclampedFloat32To7e3(a, i, j, i, j, 2, 0);
              if (j) {
                a.OpBFI(dxbc::Dest::R(i, 0b0001), dxbc::Src::LU(10), dxbc::Src::LU(j * 10),
                        dxbc::Src::R(i).Select(j), dxbc::Src::R(i, dxbc::Src::kXXXX));
              }
            }
            // Saturate and convert the alpha.
            a.OpMov(dxbc::Dest::R(i, 0b1000), dxbc::Src::R(i, dxbc::Src::kWWWW), true);
            a.OpMAd(dxbc::Dest::R(i, 0b1000), dxbc::Src::R(i, dxbc::Src::kWWWW),
                    dxbc::Src::LF(3.0f), dxbc::Src::LF(0.5f));
            a.OpFToU(dxbc::Dest::R(i, 0b1000), dxbc::Src::R(i, dxbc::Src::kWWWW));
            a.OpBFI(dxbc::Dest::R(i, 0b0001), dxbc::Src::LU(2), dxbc::Src::LU(30),
                    dxbc::Src::R(i, dxbc::Src::kWWWW), dxbc::Src::R(i, dxbc::Src::kXXXX));
          }
        } break;
        // All 64bpp formats, and all 16 bits per component formats, are
        // represented as integers in ownership transfer for safe handling of
        // NaNs and -32768 / -32767.
        case xenos::ColorRenderTargetFormat::k_16_16:
        case xenos::ColorRenderTargetFormat::k_16_16_FLOAT: {
          if (dest_color_format == xenos::ColorRenderTargetFormat::k_32_32_FLOAT) {
            for (uint32_t i = 0; i < 2; ++i) {
              a.OpBFI(dxbc::Dest::O(0, 1 << i), dxbc::Src::LU(16), dxbc::Src::LU(16),
                      dxbc::Src::R(i, dxbc::Src::kYYYY), dxbc::Src::R(i, dxbc::Src::kXXXX));
            }
          } else {
            a.OpMov(dxbc::Dest::O(0, 0b0011), dxbc::Src::R(0));
            a.OpMov(dxbc::Dest::O(0, 0b1100), dxbc::Src::R(1, 0b0100 << 4));
          }
        } break;
        case xenos::ColorRenderTargetFormat::k_16_16_16_16:
        case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT: {
          if (dest_color_format == xenos::ColorRenderTargetFormat::k_32_32_FLOAT) {
            a.OpBFI(dxbc::Dest::O(0, 0b0011), dxbc::Src::LU(16), dxbc::Src::LU(16),
                    dxbc::Src::R(1, 0b1101), dxbc::Src::R(1, 0b1000));
          } else {
            a.OpMov(dxbc::Dest::O(0), dxbc::Src::R(1));
          }
        } break;
        case xenos::ColorRenderTargetFormat::k_32_FLOAT: {
          color_packed_in_r0x_and_r1x = true;
        } break;
        case xenos::ColorRenderTargetFormat::k_32_32_FLOAT: {
          if (dest_color_format == xenos::ColorRenderTargetFormat::k_32_32_FLOAT) {
            a.OpMov(dxbc::Dest::O(0, 0b0011), dxbc::Src::R(1));
          } else {
            a.OpUBFE(dxbc::Dest::O(0), dxbc::Src::LU(16), dxbc::Src::LU(0, 16, 0, 16),
                     dxbc::Src::R(1, 0b01010000));
          }
        } break;
      }
    } else {
      assert_not_zero(rs & kTransferUsedRootParameterDepthSRVBit);
      color_packed_in_r0x_and_r1x = true;
      for (uint32_t i = 0; i < 2; ++i) {
        switch (source_depth_format) {
          case xenos::DepthRenderTargetFormat::kD24S8: {
            // Round to the nearest even integer. This seems to be the correct
            // conversion, adding +0.5 and rounding towards zero results in red
            // instead of black in the 4D5307E6 clear shader.
            a.OpMul(dxbc::Dest::R(i, 0b1000), dxbc::Src::R(i, dxbc::Src::kWWWW),
                    dxbc::Src::LF(float(0xFFFFFF)));
            a.OpRoundNE(dxbc::Dest::R(i, 0b1000), dxbc::Src::R(i, dxbc::Src::kWWWW));
            a.OpFToU(dxbc::Dest::R(i, 0b1000), dxbc::Src::R(i, dxbc::Src::kWWWW));
          } break;
          case xenos::DepthRenderTargetFormat::kD24FS8: {
            // Convert using r1.y as temporary.
            // When converting the depth in pixel shaders, it's always exact,
            // truncating not to insert additional rounding instructions.
            DxbcFloatEmitter::PreClampedDepthTo20e4(
                a, i, 3, i, 3, 1, 1,
                !options.depth_float24_convert_in_pixel_shader && options.depth_float24_round,
                true);
          } break;
        }
        // Merge depth and stencil into r0/r1.x.
        a.OpBFI(dxbc::Dest::R(i, 0b0001), dxbc::Src::LU(24), dxbc::Src::LU(8),
                dxbc::Src::R(i, dxbc::Src::kWWWW), dxbc::Src::R(i, dxbc::Src::kXXXX));
      }
    }
    if (color_packed_in_r0x_and_r1x) {
      if (dest_color_format == xenos::ColorRenderTargetFormat::k_32_32_FLOAT) {
        a.OpMov(dxbc::Dest::O(0, 0b0001), dxbc::Src::R(0, dxbc::Src::kXXXX));
        a.OpMov(dxbc::Dest::O(0, 0b0010), dxbc::Src::R(1, dxbc::Src::kXXXX));
      } else {
        for (uint32_t i = 0; i < 2; ++i) {
          a.OpUBFE(dxbc::Dest::O(0, 0b11 << (i * 2)), dxbc::Src::LU(16),
                   dxbc::Src::LU(0, 16, 0, 16), dxbc::Src::R(i, dxbc::Src::kXXXX));
        }
      }
    }
  } else {
    // Handle a 32bpp destination (32bpp color, or depth / stencil). If
    // color_packed_in_r1x is true, a raw 32bpp color value was written, and
    // common handling will be done.
    bool color_packed_in_r1x = false;
    bool depth_loaded_in_guest_format = false;
    if (source_is_color) {
      switch (source_color_format) {
        case xenos::ColorRenderTargetFormat::k_8_8_8_8:
        case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA: {
          if (dest_is_stencil_bit) {
            if (source_color_format == xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA) {
              DxbcFloatEmitter::PreSaturatedLinearToPWLGamma(a, 1, 0, 1, 0, 2, 0, 2, 1);
            }
            a.OpMAd(dxbc::Dest::R(1, 0b0001), dxbc::Src::R(1, dxbc::Src::kXXXX),
                    dxbc::Src::LF(255.0f), dxbc::Src::LF(0.5f));
            a.OpFToU(dxbc::Dest::R(1, 0b0001), dxbc::Src::R(1, dxbc::Src::kXXXX));
          } else if (dest_is_color &&
                     (dest_color_format == xenos::ColorRenderTargetFormat::k_8_8_8_8 ||
                      dest_color_format == xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA)) {
            if (source_color_format != dest_color_format) {
              // Color space conversion between k_8_8_8_8 and
              // k_8_8_8_8_GAMMA.
              if (dest_color_format != xenos::ColorRenderTargetFormat::k_8_8_8_8) {
                for (uint32_t i = 0; i < 3; ++i) {
                  DxbcFloatEmitter::PreSaturatedLinearToPWLGamma(a, 1, i, 1, i, 2, 0, 2, 1);
                }
              } else {
                for (uint32_t i = 0; i < 3; ++i) {
                  DxbcFloatEmitter::PWLGammaToLinear(a, 1, i, 1, i, true, 2, 0, 2, 1);
                }
              }
            }
            // Same or converted format - passthrough.
            a.OpMov(dxbc::Dest::O(0), dxbc::Src::R(1));
          } else if (mode.output == TransferOutput::kDepth) {
            if (source_color_format == xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA) {
              for (uint32_t i = osgn_parameter_index_sv_stencil_ref != UINT32_MAX ? 0 : 1; i < 3;
                   ++i) {
                DxbcFloatEmitter::PreSaturatedLinearToPWLGamma(a, 1, i, 1, i, 2, 0, 2, 1);
              }
            }
            // When need only depth, not stencil, skip the red component.
            a.OpMAd(dxbc::Dest::R(
                        1, osgn_parameter_index_sv_stencil_ref != UINT32_MAX ? 0b1111 : 0b1110),
                    dxbc::Src::R(1), dxbc::Src::LF(255.0f), dxbc::Src::LF(0.5f));
            a.OpFToU(dxbc::Dest::R(1, 0b1110), dxbc::Src::R(1));
            if (osgn_parameter_index_sv_stencil_ref != UINT32_MAX) {
              // Write the red component to the stencil reference.
              a.OpFToU(dxbc::Dest::OStencilRef(), dxbc::Src::R(1, dxbc::Src::kXXXX));
            }
            // Put depth in 0:23 of r1.w.
            // r1.y = 0xGGBB0000.
            a.OpBFI(dxbc::Dest::R(1, 0b0010), dxbc::Src::LU(8), dxbc::Src::LU(8),
                    dxbc::Src::R(1, dxbc::Src::kZZZZ), dxbc::Src::R(1, dxbc::Src::kYYYY));
            // r1.w = 0xGGBBAA00.
            a.OpBFI(dxbc::Dest::R(1, 0b1000), dxbc::Src::LU(8), dxbc::Src::LU(16),
                    dxbc::Src::R(1, dxbc::Src::kWWWW), dxbc::Src::R(1, dxbc::Src::kYYYY));
          } else {
            if (source_color_format == xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA) {
              for (uint32_t i = 0; i < 3; ++i) {
                DxbcFloatEmitter::PreSaturatedLinearToPWLGamma(a, 1, i, 1, i, 2, 0, 2, 1);
              }
            }
            color_packed_in_r1x = true;
            a.OpMAd(dxbc::Dest::R(1), dxbc::Src::R(1), dxbc::Src::LF(255.0f), dxbc::Src::LF(0.5f));
            a.OpFToU(dxbc::Dest::R(1), dxbc::Src::R(1));
            for (uint32_t i = 1; i < 4; ++i) {
              a.OpBFI(dxbc::Dest::R(1, 0b0001), dxbc::Src::LU(8), dxbc::Src::LU(i * 8),
                      dxbc::Src::R(1).Select(i), dxbc::Src::R(1, dxbc::Src::kXXXX));
            }
          }
        } break;
        case xenos::ColorRenderTargetFormat::k_2_10_10_10:
        case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10: {
          if (dest_is_stencil_bit) {
            a.OpMAd(dxbc::Dest::R(1, 0b0001), dxbc::Src::R(1, dxbc::Src::kXXXX),
                    dxbc::Src::LF(1023.0f), dxbc::Src::LF(0.5f));
            a.OpFToU(dxbc::Dest::R(1, 0b0001), dxbc::Src::R(1, dxbc::Src::kXXXX));
          } else if (dest_is_color &&
                     (dest_color_format == xenos::ColorRenderTargetFormat::k_2_10_10_10 ||
                      dest_color_format ==
                          xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10)) {
            a.OpMov(dxbc::Dest::O(0), dxbc::Src::R(1));
          } else {
            color_packed_in_r1x = true;
            a.OpMAd(dxbc::Dest::R(1), dxbc::Src::R(1),
                    dxbc::Src::LF(1023.0f, 1023.0f, 1023.0f, 3.0f), dxbc::Src::LF(0.5f));
            a.OpFToU(dxbc::Dest::R(1), dxbc::Src::R(1));
            for (uint32_t i = 1; i < 4; ++i) {
              a.OpBFI(dxbc::Dest::R(1, 0b0001), dxbc::Src::LU(i == 3 ? 2 : 10),
                      dxbc::Src::LU(i * 10), dxbc::Src::R(1).Select(i),
                      dxbc::Src::R(1, dxbc::Src::kXXXX));
            }
          }
        } break;
        case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
        case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16: {
          if (dest_is_stencil_bit) {
            DxbcFloatEmitter::UnclampedFloat32To7e3(a, 1, 0, 1, 0, 2, 0);
          } else if (dest_is_color &&
                     (dest_color_format == xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT ||
                      dest_color_format ==
                          xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16)) {
            a.OpMov(dxbc::Dest::O(0), dxbc::Src::R(1));
          } else {
            color_packed_in_r1x = true;
            // Float16 has a wider range for both color and alpha, also NaNs -
            // clamp and convert.
            for (uint32_t i = 0; i < 3; ++i) {
              DxbcFloatEmitter::UnclampedFloat32To7e3(a, 1, i, 1, i, 2, 0);
              if (i) {
                a.OpBFI(dxbc::Dest::R(1, 0b0001), dxbc::Src::LU(10), dxbc::Src::LU(i * 10),
                        dxbc::Src::R(1).Select(i), dxbc::Src::R(1, dxbc::Src::kXXXX));
              }
            }
            // Saturate and convert the alpha.
            a.OpMov(dxbc::Dest::R(1, 0b1000), dxbc::Src::R(1, dxbc::Src::kWWWW), true);
            a.OpMAd(dxbc::Dest::R(1, 0b1000), dxbc::Src::R(1, dxbc::Src::kWWWW),
                    dxbc::Src::LF(3.0f), dxbc::Src::LF(0.5f));
            a.OpFToU(dxbc::Dest::R(1, 0b1000), dxbc::Src::R(1, dxbc::Src::kWWWW));
            a.OpBFI(dxbc::Dest::R(1, 0b0001), dxbc::Src::LU(2), dxbc::Src::LU(30),
                    dxbc::Src::R(1, dxbc::Src::kWWWW), dxbc::Src::R(1, dxbc::Src::kXXXX));
          }
        } break;
        case xenos::ColorRenderTargetFormat::k_16_16:
        case xenos::ColorRenderTargetFormat::k_16_16_16_16:
        case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
        case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT: {
          // All 16 bits per component formats are represented as integers in
          // ownership transfer for safe handling of NaNs and -32768 / -32767.
          if (dest_is_stencil_bit) {
            // High bits are not important for discarding, as only one bit is
            // checked - already loaded to red.
          } else if (dest_is_color &&
                     (dest_color_format == xenos::ColorRenderTargetFormat::k_16_16 ||
                      dest_color_format == xenos::ColorRenderTargetFormat::k_16_16_FLOAT)) {
            a.OpMov(dxbc::Dest::O(0, 0b0011), dxbc::Src::R(1));
          } else {
            color_packed_in_r1x = true;
            a.OpBFI(dxbc::Dest::R(1, 0b0001), dxbc::Src::LU(16), dxbc::Src::LU(16),
                    dxbc::Src::R(1, dxbc::Src::kYYYY), dxbc::Src::R(1, dxbc::Src::kXXXX));
          }
        } break;
        case xenos::ColorRenderTargetFormat::k_32_FLOAT:
        case xenos::ColorRenderTargetFormat::k_32_32_FLOAT: {
          color_packed_in_r1x = true;
        } break;
      }
    } else if (rs & kTransferUsedRootParameterDepthSRVBit) {
      if (dest_is_color || dest_depth_format != source_depth_format) {
        // Need to reinterpret the depth value as color or as a different depth
        // format. Convert the depth within r1.w.
        depth_loaded_in_guest_format = true;
        switch (source_depth_format) {
          case xenos::DepthRenderTargetFormat::kD24S8: {
            // Round to the nearest even integer. This seems to be the correct
            // conversion, adding +0.5 and rounding towards zero results in red
            // instead of black in the 4D5307E6 clear shader.
            a.OpMul(dxbc::Dest::R(1, 0b1000), dxbc::Src::R(1, dxbc::Src::kWWWW),
                    dxbc::Src::LF(float(0xFFFFFF)));
            a.OpRoundNE(dxbc::Dest::R(1, 0b1000), dxbc::Src::R(1, dxbc::Src::kWWWW));
            a.OpFToU(dxbc::Dest::R(1, 0b1000), dxbc::Src::R(1, dxbc::Src::kWWWW));
          } break;
          case xenos::DepthRenderTargetFormat::kD24FS8: {
            // Convert using r1.y as temporary.
            // When converting the depth in pixel shaders, it's always exact,
            // truncating not to insert additional rounding instructions.
            DxbcFloatEmitter::PreClampedDepthTo20e4(
                a, 1, 3, 1, 3, 1, 1,
                !options.depth_float24_convert_in_pixel_shader && options.depth_float24_round,
                true);
          } break;
        }
        if (dest_is_color) {
          // Merge depth and stencil into r1.x for reinterpretation as color.
          color_packed_in_r1x = true;
          a.OpBFI(dxbc::Dest::R(1, 0b0001), dxbc::Src::LU(24), dxbc::Src::LU(8),
                  dxbc::Src::R(1, dxbc::Src::kWWWW), dxbc::Src::R(1, dxbc::Src::kXXXX));
        }
      }
    }
    switch (mode.output) {
      case TransferOutput::kColor:
        // Unless a special path was taken, unpack the raw 32bpp value into the
        // 32bpp color output. Any register can be used as temporary if needed -
        // this is the end of the shader.
        if (color_packed_in_r1x) {
          switch (dest_color_format) {
            case xenos::ColorRenderTargetFormat::k_8_8_8_8: {
              a.OpUBFE(dxbc::Dest::R(1), dxbc::Src::LU(8), dxbc::Src::LU(0, 8, 16, 24),
                       dxbc::Src::R(1, dxbc::Src::kXXXX));
              a.OpUToF(dxbc::Dest::R(1), dxbc::Src::R(1));
              a.OpMul(dxbc::Dest::O(0), dxbc::Src::R(1), dxbc::Src::LF(1.0f / 255.0f));
            } break;
            case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA: {
              // 8_8_8_8_GAMMA is represented by linear stored in
              // R16G16B16A16_UNORM.
              a.OpUBFE(dxbc::Dest::R(1), dxbc::Src::LU(8), dxbc::Src::LU(0, 8, 16, 24),
                       dxbc::Src::R(1, dxbc::Src::kXXXX));
              a.OpUToF(dxbc::Dest::R(1), dxbc::Src::R(1));
              a.OpMul(dxbc::Dest::R(1, 0b0111), dxbc::Src::R(1), dxbc::Src::LF(1.0f / 255.0f));
              a.OpMul(dxbc::Dest::O(0, 0b1000), dxbc::Src::R(1), dxbc::Src::LF(1.0f / 255.0f));
              for (uint32_t i = 0; i < 3; ++i) {
                DxbcFloatEmitter::PWLGammaToLinear(a, 1, i, 1, i, true, 0, 0, 0, 1);
              }
              a.OpMov(dxbc::Dest::O(0, 0b0111), dxbc::Src::R(1));
            } break;
            case xenos::ColorRenderTargetFormat::k_2_10_10_10:
            case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10: {
              a.OpUBFE(dxbc::Dest::R(1), dxbc::Src::LU(10, 10, 10, 2), dxbc::Src::LU(0, 10, 20, 30),
                       dxbc::Src::R(1, dxbc::Src::kXXXX));
              a.OpUToF(dxbc::Dest::R(1), dxbc::Src::R(1));
              a.OpMul(dxbc::Dest::O(0), dxbc::Src::R(1),
                      dxbc::Src::LF(1.0f / 1023.0f, 1.0f / 1023.0f, 1.0f / 1023.0f, 1.0f / 3.0f));
            } break;
            case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
            case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16: {
              // Color using r1.yz as temporary.
              for (uint32_t i = 0; i < 3; ++i) {
                DxbcFloatEmitter::Float7e3To32(a, dxbc::Dest::O(0, 1 << i), 1, 0, i * 10, 1, 1, 1,
                                               2);
              }
              // Alpha.
              a.OpUBFE(dxbc::Dest::R(1, 0b1000), dxbc::Src::LU(2), dxbc::Src::LU(30),
                       dxbc::Src::R(1, dxbc::Src::kXXXX));
              a.OpUToF(dxbc::Dest::R(1, 0b1000), dxbc::Src::R(1, dxbc::Src::kWWWW));
              a.OpMul(dxbc::Dest::O(0, 0b1000), dxbc::Src::R(1, dxbc::Src::kWWWW),
                      dxbc::Src::LF(1.0f / 3.0f));
            } break;
            case xenos::ColorRenderTargetFormat::k_16_16:
            case xenos::ColorRenderTargetFormat::k_16_16_FLOAT: {
              // All 16 bits per component formats are represented as integers
              // in ownership transfer for safe handling of NaNs and
              // -32768 / -32767.
              a.OpUBFE(dxbc::Dest::O(0, 0b0011), dxbc::Src::LU(16), dxbc::Src::LU(0, 16, 0, 0),
                       dxbc::Src::R(1, dxbc::Src::kXXXX));
            } break;
            case xenos::ColorRenderTargetFormat::k_32_FLOAT: {
              // Already as a 32-bit value.
              a.OpMov(dxbc::Dest::O(0, 0b0001), dxbc::Src::R(1, dxbc::Src::kXXXX));
            } break;
            default:
              // A 64bpp format (handled separately) or an invalid one.
              assert_unhandled_case(dest_color_format);
          }
        }
        break;
      case TransferOutput::kDepth:
        if (source_is_color || depth_loaded_in_guest_format) {
          if (color_packed_in_r1x) {
            // Extract the depth bits to r1.w.
            a.OpUBFE(dxbc::Dest::R(1, 0b1000), dxbc::Src::LU(24), dxbc::Src::LU(8),
                     dxbc::Src::R(1, dxbc::Src::kXXXX));
            if (osgn_parameter_index_sv_stencil_ref != UINT32_MAX) {
              // Extract the stencil bits to the stencil reference.
              // The depth -> depth case is handled earlier, not long after
              // loading the stencil, for simplicity.
              a.OpUBFE(dxbc::Dest::OStencilRef(), dxbc::Src::LU(8), dxbc::Src::LU(0),
                       dxbc::Src::R(1, dxbc::Src::kXXXX));
            }
          }
          // r1.w contains the depth in the guest format. If a host depth source
          // is available, need to check if it's up to date - if it is, the host
          // precision value needs to be written. Otherwise, the new guest value
          // needs to be converted to the host format. Using `if` here because
          // it's likely that the values will either be the same - if not
          // modified - or different - if cleared or totally overwritten - in
          // large amounts of samples, usually whole waves, at once.
          if (rs & kTransferUsedRootParameterHostDepthSRVBit) {
            // Load the host float32 depth to r0.x, check if, when converted to
            // the guest format, it's the same as the guest source, thus up to
            // date, and if it is, write host float32 depth to r1.w, otherwise
            // do the guest -> host conversion on the `else` path.

            // Current register allocation:
            // r0.xy = pixel index within the destination 32bpp tile
            // r0.z = 32bpp tile index relative to the destination base
            // r1.w = depth in guest format

            if (key.host_depth_source_is_copy) {
              // Get the address in the EDRAM scratch buffer and load from
              // there.
              // The beginning of the buffer is (0, 0) of the destination.
              // 40-sample columns are not swapped for addressing simplicity
              // (because this is used for depth -> depth transfers, where
              // swapping isn't needed).
              // Convert samples to pixels.
              assert_true(key.host_depth_source_msaa_samples == xenos::MsaaSamples::k1X);
              if (key.dest_msaa_samples >= xenos::MsaaSamples::k2X) {
                if (key.dest_msaa_samples >= xenos::MsaaSamples::k4X) {
                  // Horizontal sample index in bit 0.
                  a.OpBFI(dxbc::Dest::R(0, 0b0001), dxbc::Src::LU(31), dxbc::Src::LU(1),
                          dxbc::Src::R(0, dxbc::Src::kXXXX), dest_sample);
                }
                // Vertical sample index as 1 or 0 in bit 0 for true 2x or as 0
                // or 1 in bit 1 for 4x or for 2x emulated as 4x.
                if (key.dest_msaa_samples == xenos::MsaaSamples::k2X && options.msaa_2x_supported) {
                  a.OpBFI(dxbc::Dest::R(0, 0b0010), dxbc::Src::LU(31), dxbc::Src::LU(1),
                          dxbc::Src::R(0, dxbc::Src::kYYYY), dest_sample);
                  a.OpXOr(dxbc::Dest::R(0, 0b0010), dxbc::Src::R(0, dxbc::Src::kYYYY),
                          dxbc::Src::LU(1));
                } else {
                  // Using r0.w as a temporary.
                  a.OpUShR(dxbc::Dest::R(0, 0b1000), dest_sample, dxbc::Src::LU(1));
                  a.OpBFI(dxbc::Dest::R(0, 0b0010), dxbc::Src::LU(31), dxbc::Src::LU(1),
                          dxbc::Src::R(0, dxbc::Src::kYYYY), dxbc::Src::R(0, dxbc::Src::kWWWW));
                }
              }
              // Combine the tile sample index and the tile index into buffer
              // address to r0.x.
              // The tile index doesn't need to be wrapped, as the host depth is
              // written to the beginning of the buffer, without the base
              // offset.
              a.OpUMAd(dxbc::Dest::R(0, 0b0001), dxbc::Src::LU(tile_width_samples),
                       dxbc::Src::R(0, dxbc::Src::kYYYY), dxbc::Src::R(0, dxbc::Src::kXXXX));
              a.OpUMAd(dxbc::Dest::R(0, 0b0001),
                       dxbc::Src::LU(tile_width_samples * tile_height_samples),
                       dxbc::Src::R(0, dxbc::Src::kZZZZ), dxbc::Src::R(0, dxbc::Src::kXXXX));
              // Load from the buffer.
              a.OpLd(dxbc::Dest::R(0, 0b0001), dxbc::Src::R(0, dxbc::Src::kXXXX), 0b0001,
                     dxbc::Src::T(srv_index_host_depth, kTransferSRVRegisterHostDepth,
                                  dxbc::Src::kXXXX));
            } else {
              // Adjust the tile index from the destination to the host depth
              // source.
              // r0.w = destination to host depth source EDRAM tile adjustment
              a.OpIBFE(dxbc::Dest::R(0, 0b1000), dxbc::Src::LU(xenos::kEdramBaseTilesBits + 1),
                       dxbc::Src::LU(xenos::kEdramPitchTilesBits * 2),
                       dxbc::Src::CB(cbuffer_index_host_depth_address,
                                     kTransferCBVRegisterHostDepthAddress, 0, dxbc::Src::kXXXX));
              // r0.z = tile index relative to the host depth source base, or
              //        the tile index within the host depth source minus the
              //        EDRAM tile count if transferring across addressing
              //        wrapping (if negative)
              // r0.w = free
              a.OpIAdd(dxbc::Dest::R(0, 0b0100), dxbc::Src::R(0, dxbc::Src::kZZZZ),
                       dxbc::Src::R(0, dxbc::Src::kWWWW));
              // r0.z = tile index relative to the host depth source base
              a.OpAnd(dxbc::Dest::R(0, 0b0100), dxbc::Src::R(0, dxbc::Src::kZZZZ),
                      dxbc::Src::LU(xenos::kEdramTileCount - 1));
              // Convert position and sample index from within the destination
              // tile to within the host depth source tile, like for the guest
              // render target, but for 32bpp -> 32bpp only.
              dxbc::Src host_depth_source_sample(dest_sample);
              if (key.host_depth_source_msaa_samples != key.dest_msaa_samples) {
                if (key.host_depth_source_msaa_samples >= xenos::MsaaSamples::k4X) {
                  // 4x -> 1x/2x.
                  if (key.dest_msaa_samples == xenos::MsaaSamples::k2X) {
                    // 4x -> 2x.
                    // Horizontal pixels to samples. Vertical sample (1, 0 in
                    // the first bit for native 2x or 0, 1 in the second bit for
                    // 2x as 4x) to second sample bit.
                    host_depth_source_sample = dxbc::Src::R(0, dxbc::Src::kWWWW);
                    if (options.msaa_2x_supported) {
                      a.OpBFI(dxbc::Dest::R(0, 0b1000), dxbc::Src::LU(31), dxbc::Src::LU(1),
                              dest_sample, dxbc::Src::R(0, dxbc::Src::kXXXX));
                      a.OpXOr(dxbc::Dest::R(0, 0b1000), host_depth_source_sample,
                              dxbc::Src::LU(1 << 1));
                    } else {
                      a.OpBFI(dxbc::Dest::R(0, 0b1000), dxbc::Src::LU(1), dxbc::Src::LU(0),
                              dxbc::Src::R(0, dxbc::Src::kXXXX), dest_sample);
                    }
                    a.OpUShR(dxbc::Dest::R(0, 0b0001), dxbc::Src::R(0, dxbc::Src::kXXXX),
                             dxbc::Src::LU(1));
                  } else {
                    // 4x -> 1x.
                    // Pixels to samples.
                    a.OpAnd(dxbc::Dest::R(0, 0b1000), dxbc::Src::R(0, dxbc::Src::kXXXX),
                            dxbc::Src::LU(1));
                    host_depth_source_sample = dxbc::Src::R(0, dxbc::Src::kWWWW);
                    a.OpBFI(dxbc::Dest::R(0, 0b1000), dxbc::Src::LU(1), dxbc::Src::LU(1),
                            dxbc::Src::R(0, dxbc::Src::kYYYY), host_depth_source_sample);
                    a.OpUShR(dxbc::Dest::R(0, 0b0011), dxbc::Src::R(0), dxbc::Src::LU(1));
                  }
                } else {
                  // 1x/2x -> 1x/2x/4x (as long as they're different).
                  // Only the X part - Y is handled by common code.
                  if (key.dest_msaa_samples >= xenos::MsaaSamples::k4X) {
                    // Horizontal samples to pixels.
                    a.OpBFI(dxbc::Dest::R(0, 0b0001), dxbc::Src::LU(31), dxbc::Src::LU(1),
                            dxbc::Src::R(0, dxbc::Src::kXXXX), dest_sample);
                  }
                }
                // Host depth source Y and sample index for 1x/2x AA sources.
                if (key.host_depth_source_msaa_samples < xenos::MsaaSamples::k4X) {
                  if (key.dest_msaa_samples >= xenos::MsaaSamples::k4X) {
                    // 1x/2x -> 4x.
                    if (key.host_depth_source_msaa_samples == xenos::MsaaSamples::k2X) {
                      // 2x -> 4x.
                      // Vertical samples (second bit) of 4x destination to
                      // vertical sample (1, 0 for native 2x, or 0, 3 for 2x as
                      // 4x) of 2x source.
                      a.OpUShR(dxbc::Dest::R(0, 0b1000), dest_sample, dxbc::Src::LU(1));
                      host_depth_source_sample = dxbc::Src::R(0, dxbc::Src::kWWWW);
                      if (options.msaa_2x_supported) {
                        a.OpXOr(dxbc::Dest::R(0, 0b1000), host_depth_source_sample,
                                dxbc::Src::LU(1));
                      } else {
                        a.OpBFI(dxbc::Dest::R(0, 0b1000), dxbc::Src::LU(1), dxbc::Src::LU(1),
                                host_depth_source_sample, host_depth_source_sample);
                      }
                    } else {
                      // 1x -> 4x.
                      // Vertical samples (second bit) to Y pixels, using r0.w
                      // (not needed without source MSAA) as a temporary.
                      a.OpUShR(dxbc::Dest::R(0, 0b1000), dest_sample, dxbc::Src::LU(1));
                      a.OpBFI(dxbc::Dest::R(0, 0b0010), dxbc::Src::LU(31), dxbc::Src::LU(1),
                              dxbc::Src::R(0, dxbc::Src::kYYYY), dxbc::Src::R(0, dxbc::Src::kWWWW));
                    }
                  } else {
                    // 1x/2x -> different 1x/2x.
                    if (key.host_depth_source_msaa_samples == xenos::MsaaSamples::k2X) {
                      // 2x -> 1x.
                      // Vertical pixels of 2x destination to vertical samples
                      // (1, 0 for native 2x, or 0, 3 for 2x as 4x) of 1x
                      // source.
                      a.OpAnd(dxbc::Dest::R(0, 0b1000), dxbc::Src::R(0, dxbc::Src::kYYYY),
                              dxbc::Src::LU(1));
                      host_depth_source_sample = dxbc::Src::R(0, dxbc::Src::kWWWW);
                      if (options.msaa_2x_supported) {
                        a.OpXOr(dxbc::Dest::R(0, 0b1000), host_depth_source_sample,
                                dxbc::Src::LU(1));
                      } else {
                        a.OpBFI(dxbc::Dest::R(0, 0b1000), dxbc::Src::LU(1), dxbc::Src::LU(1),
                                host_depth_source_sample, host_depth_source_sample);
                      }
                      a.OpUShR(dxbc::Dest::R(0, 0b0010), dxbc::Src::R(0, dxbc::Src::kYYYY),
                               dxbc::Src::LU(1));
                    } else {
                      // 1x -> 2x.
                      // Vertical samples (1, 0 in the first bit for native 2x
                      // or 0, 1 in the second bit for 2x as 4x) of 2x
                      // destination to vertical pixels of 1x source.
                      // Using r0.w (not needed without source MSAA) as a
                      // temporary.
                      if (options.msaa_2x_supported) {
                        a.OpBFI(dxbc::Dest::R(0, 0b0010), dxbc::Src::LU(31), dxbc::Src::LU(1),
                                dxbc::Src::R(0, dxbc::Src::kYYYY), dest_sample);
                        a.OpXOr(dxbc::Dest::R(0, 0b0010), dxbc::Src::R(0, dxbc::Src::kYYYY),
                                dxbc::Src::LU(1));
                      } else {
                        a.OpUShR(dxbc::Dest::R(0, 0b1000), dest_sample, dxbc::Src::LU(1));
                        a.OpBFI(dxbc::Dest::R(0, 0b0010), dxbc::Src::LU(31), dxbc::Src::LU(1),
                                dxbc::Src::R(0, dxbc::Src::kYYYY),
                                dxbc::Src::R(0, dxbc::Src::kWWWW));
                      }
                    }
                  }
                }
              }
              // r1.x = host depth source pitch in tiles
              a.OpUBFE(dxbc::Dest::R(1, 0b0001), dxbc::Src::LU(xenos::kEdramPitchTilesBits),
                       dxbc::Src::LU(xenos::kEdramPitchTilesBits),
                       dxbc::Src::CB(cbuffer_index_host_depth_address,
                                     kTransferCBVRegisterHostDepthAddress, 0, dxbc::Src::kXXXX));
              // r0.z = host depth source tile row
              // r1.x = host depth source tile within the row
              a.OpUDiv(dxbc::Dest::R(0, 0b0100), dxbc::Dest::R(1, 0b0001),
                       dxbc::Src::R(0, dxbc::Src::kZZZZ), dxbc::Src::R(1, dxbc::Src::kXXXX));
              // r0.x = pixel X within the host depth source texture
              // r1.x = free
              a.OpUMAd(
                  dxbc::Dest::R(0, 0b0001),
                  dxbc::Src::LU(tile_width_samples >> uint32_t(key.host_depth_source_msaa_samples >=
                                                               xenos::MsaaSamples::k4X)),
                  dxbc::Src::R(1, dxbc::Src::kXXXX), dxbc::Src::R(0, dxbc::Src::kXXXX));
              // r0.y = pixel Y within the host depth source texture
              // r0.z = free
              a.OpUMAd(dxbc::Dest::R(0, 0b0010),
                       dxbc::Src::LU(
                           tile_height_samples >>
                           uint32_t(key.host_depth_source_msaa_samples >= xenos::MsaaSamples::k2X)),
                       dxbc::Src::R(0, dxbc::Src::kZZZZ), dxbc::Src::R(0, dxbc::Src::kYYYY));
              // Load from the host depth texture.
              if (key.host_depth_source_msaa_samples != xenos::MsaaSamples::k1X) {
                a.OpLdMS(dxbc::Dest::R(0, 0b0001), dxbc::Src::R(0), 0b0011,
                         dxbc::Src::T(srv_index_host_depth, kTransferSRVRegisterHostDepth,
                                      dxbc::Src::kXXXX),
                         host_depth_source_sample);
              } else {
                // Write zero to the LOD index in r0.z.
                a.OpMov(dxbc::Dest::R(0, 0b0100), dxbc::Src::LU(0));
                a.OpLd(dxbc::Dest::R(0, 0b0001), dxbc::Src::R(0, 0b10000100), 0b1011,
                       dxbc::Src::T(srv_index_host_depth, kTransferSRVRegisterHostDepth,
                                    dxbc::Src::kXXXX));
              }
            }
            // Convert the host depth value in r0.x to the guest format in r0.y
            // using r0.z as a temporary and check if it matches the value in
            // the currently owning guest render target.
            switch (dest_depth_format) {
              case xenos::DepthRenderTargetFormat::kD24S8: {
                // Round to the nearest even integer. This seems to be the
                // correct, adding +0.5 and rounding towards zero results in red
                // instead of black in the 4D5307E6 clear shader.
                a.OpMul(dxbc::Dest::R(0, 0b0010), dxbc::Src::R(0, dxbc::Src::kXXXX),
                        dxbc::Src::LF(float(0xFFFFFF)));
                a.OpRoundNE(dxbc::Dest::R(0, 0b0010), dxbc::Src::R(0, dxbc::Src::kYYYY));
                a.OpFToU(dxbc::Dest::R(0, 0b0010), dxbc::Src::R(0, dxbc::Src::kYYYY));
              } break;
              case xenos::DepthRenderTargetFormat::kD24FS8: {
                // When converting the depth in pixel shaders, it's always
                // exact, truncating not to insert additional rounding
                // instructions.
                DxbcFloatEmitter::PreClampedDepthTo20e4(
                    a, 0, 1, 0, 0, 0, 2,
                    !options.depth_float24_convert_in_pixel_shader && options.depth_float24_round,
                    true);
              } break;
            }
            a.OpIEq(dxbc::Dest::R(0, 0b0010), dxbc::Src::R(0, dxbc::Src::kYYYY),
                    dxbc::Src::R(1, dxbc::Src::kWWWW));
            a.OpIf(true, dxbc::Src::R(0, dxbc::Src::kYYYY));
            // If the host depth is up to date, write it to oDepth at the host
            // precision instead of converting the guest depth.
            a.OpMov(dxbc::Dest::R(1, 0b1000), dxbc::Src::R(0, dxbc::Src::kXXXX));
            a.OpElse();
          }
          // Convert using r0.x as a temporary.
          switch (dest_depth_format) {
            case xenos::DepthRenderTargetFormat::kD24S8: {
              // Multiplying by 1.0 / 0xFFFFFF produces an incorrect result (for
              // 0xC00000, for instance - which is 2_10_10_10 clear to 0001) -
              // rescale from 0...0xFFFFFF to 0...0x1000000 doing what true
              // float division followed by multiplication does (on x86-64 MSVC
              // with default SSE rounding) - values starting from 0x800000
              // become bigger by 1; then accurately bias the result's exponent.
              a.OpUShR(dxbc::Dest::R(0, 0b0001), dxbc::Src::R(1, dxbc::Src::kWWWW),
                       dxbc::Src::LU(23));
              a.OpIAdd(dxbc::Dest::R(1, 0b1000), dxbc::Src::R(1, dxbc::Src::kWWWW),
                       dxbc::Src::R(0, dxbc::Src::kXXXX));
              a.OpUToF(dxbc::Dest::R(1, 0b1000), dxbc::Src::R(1, dxbc::Src::kWWWW));
              a.OpMul(dxbc::Dest::R(1, 0b1000), dxbc::Src::R(1, dxbc::Src::kWWWW),
                      dxbc::Src::LF(1.0f / float(1 << 24)));
            } break;
            case xenos::DepthRenderTargetFormat::kD24FS8: {
              DxbcFloatEmitter::Depth20e4To32(a, dxbc::Dest::R(1, 0b1000), 1, 3, 0, 1, 3, 0, 0,
                                              true);
            } break;
          }
          // Host depth is different, or not available - convert the guest depth
          // to the destination format.
          if (rs & kTransferUsedRootParameterHostDepthSRVBit) {
            // Close the conditional for the host / guest depth.
            a.OpEndIf();
          }
        }
        a.OpMov(dxbc::Dest::ODepth(), dxbc::Src::R(1, dxbc::Src::kWWWW));
        break;
      case TransferOutput::kStencilBit:
        // Discard the sample if the needed stencil bit is not set.
        assert_true(cbuffer_index_stencil_mask != UINT32_MAX);
        a.OpAnd(dxbc::Dest::R(0, 0b0001), dxbc::Src::R(1, dxbc::Src::kXXXX),
                dxbc::Src::CB(cbuffer_index_stencil_mask, kTransferCBVRegisterStencilMask, 0,
                              dxbc::Src::kXXXX));
        a.OpDiscard(false, dxbc::Src::R(0, dxbc::Src::kXXXX));
        break;
    }
  }

  if (dest_is_color) {
    // Fill the unused components of the color result.
    uint32_t dest_color_component_count =
        xenos::GetColorRenderTargetFormatComponentCount(dest_color_format);
    uint32_t dest_color_unwritten_mask = 0b1111 & ~uint32_t((1 << dest_color_component_count) - 1);
    if (dest_color_component_count < 4) {
      a.OpMov(dxbc::Dest::O(0, dest_color_unwritten_mask), dxbc::Src::LU(0));
    }
  }

  a.OpRet();

  // Write the shader program length in dwords.
  output[shex_position_dwords + 1] = uint32_t(output.size()) - shex_position_dwords;

  {
    auto& blob_header = *reinterpret_cast<dxbc::BlobHeader*>(output.data() + blob_position_dwords);
    blob_header.fourcc = dxbc::BlobHeader::FourCC::kShaderEx;
    blob_position_dwords = uint32_t(output.size());
    blob_header.size_bytes = (blob_position_dwords - kBlobHeaderSizeDwords) * sizeof(uint32_t) -
                             output[blob_offset_position_dwords++];
  }

  // ***************************************************************************
  // Shader feature info
  // ***************************************************************************

  if (shader_uses_stencil_reference_output) {
    output[blob_offset_position_dwords] = uint32_t(blob_position_dwords * sizeof(uint32_t));
    uint32_t sfi0_position_dwords = blob_position_dwords + kBlobHeaderSizeDwords;
    output.resize(sfi0_position_dwords + sizeof(dxbc::ShaderFeatureInfo) / sizeof(uint32_t));
    auto& shader_feature_info =
        *reinterpret_cast<dxbc::ShaderFeatureInfo*>(output.data() + sfi0_position_dwords);
    shader_feature_info.feature_flags[0] |= dxbc::kShaderFeature0_StencilRef;
    {
      auto& blob_header =
          *reinterpret_cast<dxbc::BlobHeader*>(output.data() + blob_position_dwords);
      blob_header.fourcc = dxbc::BlobHeader::FourCC::kShaderFeatureInfo;
      blob_position_dwords = uint32_t(output.size());
      blob_header.size_bytes = (blob_position_dwords - kBlobHeaderSizeDwords) * sizeof(uint32_t) -
                               output[blob_offset_position_dwords++];
    }
  }

  // ***************************************************************************
  // Statistics
  // ***************************************************************************

  output[blob_offset_position_dwords] = uint32_t(blob_position_dwords * sizeof(uint32_t));
  uint32_t stat_position_dwords = blob_position_dwords + kBlobHeaderSizeDwords;
  output.resize(stat_position_dwords + sizeof(dxbc::Statistics) / sizeof(uint32_t));
  std::memcpy(output.data() + stat_position_dwords, &stat, sizeof(dxbc::Statistics));
  {
    auto& blob_header = *reinterpret_cast<dxbc::BlobHeader*>(output.data() + blob_position_dwords);
    blob_header.fourcc = dxbc::BlobHeader::FourCC::kStatistics;
    blob_position_dwords = uint32_t(output.size());
    blob_header.size_bytes = (blob_position_dwords - kBlobHeaderSizeDwords) * sizeof(uint32_t) -
                             output[blob_offset_position_dwords++];
  }

  // ***************************************************************************
  // Container header
  // ***************************************************************************

  uint32_t output_size_bytes = uint32_t(output.size() * sizeof(uint32_t));
  {
    auto& container_header = *reinterpret_cast<dxbc::ContainerHeader*>(output.data());
    container_header.InitializeIdentification();
    container_header.size_bytes = output_size_bytes;
    container_header.blob_count = blob_count;
    CalculateDXBCChecksum(reinterpret_cast<unsigned char*>(output.data()),
                          static_cast<unsigned int>(output_size_bytes),
                          reinterpret_cast<unsigned int*>(&container_header.hash));
  }

  return true;
}

}  // namespace rex::graphics
