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

#include <rex/graphics/util/dxbc_render_target_dump.h>

#include <rex/graphics/util/draw.h>
#include <rex/graphics/util/dxbc_float.h>
#include <rex/graphics/util/dxbc_portable.h>
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

bool DxbcRenderTargetDumpShader::Create(Key key, const Options& options,
                                        std::vector<uint32_t>& output, std::string& error) {
  output.clear();
  error.clear();
  if ((key.key >> 7) || uint32_t(key.msaa_samples) > 2 ||
      (key.is_depth ? key.resource_format > 1 : !ValidColorFormat(key.resource_format)) ||
      !options.resolution_scale_x || !options.resolution_scale_y ||
      options.resolution_scale_x > 3 || options.resolution_scale_y > 3) {
    error = "Invalid native render target dump key or resolution scale";
    return false;
  }
  // Because of output.resize(), pointers can't be kept persistently
  // here! Resizing also zeroes the memory.

  output.clear();

  // RDEF, ISGN, OSGN, SHEX, STAT.
  constexpr uint32_t kBlobCount = 5;

  // Allocate space for the container header and the blob offsets.
  output.resize(sizeof(dxbc::ContainerHeader) / sizeof(uint32_t) + kBlobCount);
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

  // Constants:
  // - uint xe_edram_dump_offsets
  // - uint xe_edram_dump_pitches
  enum Constant : uint32_t {
    kConstantOffsets,
    kConstantPitches,
    kConstantCount,
  };
  // Names.
  name_ptr = uint32_t((output.size() - rdef_position_dwords) * sizeof(uint32_t));
  uint32_t rdef_xe_edram_dump_offsets_name_ptr = name_ptr;
  name_ptr += dxbc::AppendAlignedString(output, "xe_edram_dump_offsets");
  uint32_t rdef_xe_edram_dump_pitches_name_ptr = name_ptr;
  name_ptr += dxbc::AppendAlignedString(output, "xe_edram_dump_pitches");
  // Constants.
  uint32_t rdef_constants_position_dwords = uint32_t(output.size());
  uint32_t rdef_constants_ptr =
      uint32_t((rdef_constants_position_dwords - rdef_position_dwords) * sizeof(uint32_t));
  output.resize(rdef_constants_position_dwords +
                sizeof(dxbc::RdefVariable) / sizeof(uint32_t) * kConstantCount);
  {
    auto rdef_constants =
        reinterpret_cast<dxbc::RdefVariable*>(output.data() + rdef_constants_position_dwords);
    // uint xe_edram_dump_offsets
    dxbc::RdefVariable& rdef_constant_offsets = rdef_constants[kConstantOffsets];
    rdef_constant_offsets.name_ptr = rdef_xe_edram_dump_offsets_name_ptr;
    rdef_constant_offsets.size_bytes = sizeof(uint32_t);
    rdef_constant_offsets.flags = dxbc::kRdefVariableFlagUsed;
    rdef_constant_offsets.type_ptr = rdef_type_uint_ptr;
    rdef_constant_offsets.start_texture = UINT32_MAX;
    rdef_constant_offsets.start_sampler = UINT32_MAX;
    // uint xe_edram_dump_pitches
    dxbc::RdefVariable& rdef_constant_pitches = rdef_constants[kConstantPitches];
    rdef_constant_pitches.name_ptr = rdef_xe_edram_dump_pitches_name_ptr;
    rdef_constant_pitches.size_bytes = sizeof(uint32_t);
    rdef_constant_pitches.flags = dxbc::kRdefVariableFlagUsed;
    rdef_constant_pitches.type_ptr = rdef_type_uint_ptr;
    rdef_constant_pitches.start_texture = UINT32_MAX;
    rdef_constant_pitches.start_sampler = UINT32_MAX;
  }

  // Constant buffers:
  // - xe_edram_dump_offsets : b0 { uint xe_edram_dump_offsets; }
  // - xe_edram_dump_pitches : b1 { uint xe_edram_dump_pitches; }
  // Reusing the constant names for constant buffers.
  uint32_t rdef_cbuffer_position_dwords = uint32_t(output.size());
  output.resize(rdef_cbuffer_position_dwords +
                sizeof(dxbc::RdefCbuffer) / sizeof(uint32_t) * kDumpCbufferCount);
  {
    auto rdef_cbuffers =
        reinterpret_cast<dxbc::RdefCbuffer*>(output.data() + rdef_cbuffer_position_dwords);
    // xe_edram_dump_offsets
    dxbc::RdefCbuffer& rdef_cbuffer_offsets = rdef_cbuffers[kDumpCbufferOffsets];
    rdef_cbuffer_offsets.name_ptr = rdef_xe_edram_dump_offsets_name_ptr;
    rdef_cbuffer_offsets.variable_count = 1;
    rdef_cbuffer_offsets.variables_ptr =
        uint32_t(rdef_constants_ptr + sizeof(dxbc::RdefVariable) * kConstantOffsets);
    rdef_cbuffer_offsets.size_vector_aligned_bytes = sizeof(uint32_t) * 4;
    // xe_edram_dump_pitches
    dxbc::RdefCbuffer& rdef_cbuffer_pitches = rdef_cbuffers[kDumpCbufferPitches];
    rdef_cbuffer_pitches.name_ptr = rdef_xe_edram_dump_pitches_name_ptr;
    rdef_cbuffer_pitches.variable_count = 1;
    rdef_cbuffer_pitches.variables_ptr =
        uint32_t(rdef_constants_ptr + sizeof(dxbc::RdefVariable) * kConstantPitches);
    rdef_cbuffer_pitches.size_vector_aligned_bytes = sizeof(uint32_t) * 4;
  }

  // Bindings.
  // - Texture2D/Texture2DMS<float4/uint4> xe_edram_dump_source : t0
  // - Optionally, Texture2D/Texture2DMS<uint2> xe_edram_dump_stencil : t1
  // - RWBuffer<uint/uint2> xe_edram : u0
  // - Constant buffers
  uint32_t rdef_binding_count = 1 + key.is_depth + 1 + kDumpCbufferCount;
  // Names.
  name_ptr = uint32_t((output.size() - rdef_position_dwords) * sizeof(uint32_t));
  uint32_t rdef_xe_edram_dump_source_name_ptr = name_ptr;
  name_ptr += dxbc::AppendAlignedString(output, "xe_edram_dump_source");
  uint32_t rdef_xe_edram_dump_stencil_name_ptr = name_ptr;
  if (key.is_depth) {
    name_ptr += dxbc::AppendAlignedString(output, "xe_edram_dump_stencil");
  }
  uint32_t rdef_xe_edram_name_ptr = name_ptr;
  name_ptr += dxbc::AppendAlignedString(output, "xe_edram");
  // Bindings.
  uint32_t rdef_binding_position_dwords = uint32_t(output.size());
  output.resize(rdef_binding_position_dwords +
                sizeof(dxbc::RdefInputBind) / sizeof(uint32_t) * rdef_binding_count);
  bool source_is_uint;
  if (key.is_depth) {
    source_is_uint = false;
  } else {
    source_is_uint = IsIntegerTransferFormat(key.GetColorFormat());
  }
  dxbc::ResourceReturnType source_return_type =
      source_is_uint ? dxbc::ResourceReturnType::kUInt : dxbc::ResourceReturnType::kFloat;
  uint32_t source_component_count =
      key.is_depth ? 1 : xenos::GetColorRenderTargetFormatComponentCount(key.GetColorFormat());
  bool format_is_64bpp =
      !key.is_depth && xenos::IsColorRenderTargetFormat64bpp(key.GetColorFormat());
  {
    auto rdef_bindings =
        reinterpret_cast<dxbc::RdefInputBind*>(output.data() + rdef_binding_position_dwords);
    uint32_t rdef_binding_index = 0;
    // xe_edram_dump_source
    dxbc::RdefInputBind& rdef_binding_source = rdef_bindings[rdef_binding_index++];
    rdef_binding_source.name_ptr = rdef_xe_edram_dump_source_name_ptr;
    rdef_binding_source.type = dxbc::RdefInputType::kTexture;
    rdef_binding_source.return_type = source_return_type;
    if (key.msaa_samples != xenos::MsaaSamples::k1X) {
      rdef_binding_source.dimension = dxbc::RdefDimension::kSRVTexture2DMS;
      // Sample count is dynamic on Shader Model 5.
    } else {
      rdef_binding_source.dimension = dxbc::RdefDimension::kSRVTexture2D;
      rdef_binding_source.sample_count = UINT32_MAX;
    }
    rdef_binding_source.bind_count = 1;
    rdef_binding_source.flags = (source_component_count - 1)
                                << dxbc::kRdefInputFlagsComponentsShift;
    // xe_edram_dump_stencil
    if (key.is_depth) {
      dxbc::RdefInputBind& rdef_binding_stencil = rdef_bindings[rdef_binding_index++];
      rdef_binding_stencil.name_ptr = rdef_xe_edram_dump_stencil_name_ptr;
      rdef_binding_stencil.type = dxbc::RdefInputType::kTexture;
      rdef_binding_stencil.return_type = dxbc::ResourceReturnType::kUInt;
      rdef_binding_stencil.dimension = rdef_binding_source.dimension;
      rdef_binding_stencil.sample_count = rdef_binding_source.sample_count;
      rdef_binding_stencil.bind_point = 1;
      rdef_binding_stencil.bind_count = 1;
      rdef_binding_stencil.flags = dxbc::kRdefInputFlags2Component;
      rdef_binding_stencil.id = 1;
    }
    // xe_edram
    dxbc::RdefInputBind& rdef_binding_edram = rdef_bindings[rdef_binding_index++];
    rdef_binding_edram.name_ptr = rdef_xe_edram_name_ptr;
    rdef_binding_edram.type = dxbc::RdefInputType::kUAVRWTyped;
    rdef_binding_edram.return_type = dxbc::ResourceReturnType::kUInt;
    rdef_binding_edram.dimension = dxbc::RdefDimension::kUAVBuffer;
    rdef_binding_edram.sample_count = UINT32_MAX;
    rdef_binding_edram.bind_count = 1;
    rdef_binding_edram.flags = format_is_64bpp ? dxbc::kRdefInputFlags2Component : 0;
    // xe_edram_dump_offsets
    dxbc::RdefInputBind& rdef_binding_offsets = rdef_bindings[rdef_binding_index++];
    rdef_binding_offsets.name_ptr = rdef_xe_edram_dump_offsets_name_ptr;
    rdef_binding_offsets.type = dxbc::RdefInputType::kCbuffer;
    rdef_binding_offsets.bind_point = kDumpCbufferOffsets;
    rdef_binding_offsets.bind_count = 1;
    rdef_binding_offsets.flags = dxbc::kRdefInputFlagUserPacked;
    rdef_binding_offsets.id = kDumpCbufferOffsets;
    // xe_edram_dump_pitches
    dxbc::RdefInputBind& rdef_binding_pitches = rdef_bindings[rdef_binding_index++];
    rdef_binding_pitches.name_ptr = rdef_xe_edram_dump_pitches_name_ptr;
    rdef_binding_pitches.type = dxbc::RdefInputType::kCbuffer;
    rdef_binding_pitches.bind_point = kDumpCbufferPitches;
    rdef_binding_pitches.bind_count = 1;
    rdef_binding_pitches.flags = dxbc::kRdefInputFlagUserPacked;
    rdef_binding_pitches.id = kDumpCbufferPitches;
  }

  // Header.
  {
    auto& rdef_header = *reinterpret_cast<dxbc::RdefHeader*>(output.data() + rdef_position_dwords);
    rdef_header.cbuffer_count = kDumpCbufferCount;
    rdef_header.cbuffers_ptr =
        uint32_t((rdef_cbuffer_position_dwords - rdef_position_dwords) * sizeof(uint32_t));
    rdef_header.input_bind_count = rdef_binding_count;
    rdef_header.input_binds_ptr =
        uint32_t((rdef_binding_position_dwords - rdef_position_dwords) * sizeof(uint32_t));
    rdef_header.shader_model = dxbc::RdefShaderModel::kComputeShader5_1;
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
  // Input and output signatures (empty)
  // ***************************************************************************

  for (uint32_t i = 0; i < 2; ++i) {
    output[blob_offset_position_dwords] = uint32_t(blob_position_dwords * sizeof(uint32_t));
    uint32_t signature_position_dwords = blob_position_dwords + kBlobHeaderSizeDwords;
    output.resize(signature_position_dwords + sizeof(dxbc::Signature) / sizeof(uint32_t));
    {
      auto& signature =
          *reinterpret_cast<dxbc::Signature*>(output.data() + signature_position_dwords);
      // Empty - just set parameter pointer to the end.
      signature.parameter_info_ptr = sizeof(dxbc::Signature);
    }
    {
      auto& blob_header =
          *reinterpret_cast<dxbc::BlobHeader*>(output.data() + blob_position_dwords);
      blob_header.fourcc = i ? dxbc::BlobHeader::FourCC::kOutputSignature
                             : dxbc::BlobHeader::FourCC::kInputSignature;
      blob_position_dwords = uint32_t(output.size());
      blob_header.size_bytes = (blob_position_dwords - kBlobHeaderSizeDwords) * sizeof(uint32_t) -
                               output[blob_offset_position_dwords++];
    }
  }

  // ***************************************************************************
  // Shader program
  // ***************************************************************************

  output[blob_offset_position_dwords] = uint32_t(blob_position_dwords * sizeof(uint32_t));
  uint32_t shex_position_dwords = blob_position_dwords + kBlobHeaderSizeDwords;
  output.resize(shex_position_dwords);

  output.push_back(dxbc::VersionToken(dxbc::ProgramType::kComputeShader, 5, 1));
  // Reserve space for the length token.
  output.push_back(0);

  dxbc::Statistics stat;
  std::memset(&stat, 0, sizeof(dxbc::Statistics));
  PortableDxbcAssembler a(output, stat, options.portable_integer_division);

  a.OpDclGlobalFlags(dxbc::kGlobalFlagAllResourcesBound);
  a.OpDclConstantBuffer(
      dxbc::Src::CB(dxbc::Src::Dcl, kDumpCbufferOffsets, kDumpCbufferOffsets, kDumpCbufferOffsets),
      1);
  a.OpDclConstantBuffer(
      dxbc::Src::CB(dxbc::Src::Dcl, kDumpCbufferPitches, kDumpCbufferPitches, kDumpCbufferPitches),
      1);
  // Source texture.
  dxbc::ResourceDimension source_dimension = key.msaa_samples != xenos::MsaaSamples::k1X
                                                 ? dxbc::ResourceDimension::kTexture2DMS
                                                 : dxbc::ResourceDimension::kTexture2D;
  a.OpDclResource(
      source_dimension,
      dxbc::ResourceReturnTypeX4Token(source_is_uint ? dxbc::ResourceReturnType::kUInt
                                                     : dxbc::ResourceReturnType::kFloat),
      dxbc::Src::T(dxbc::Src::Dcl, 0, 0, 0));
  // Source stencil texture.
  if (key.is_depth) {
    a.OpDclResource(source_dimension,
                    dxbc::ResourceReturnTypeX4Token(dxbc::ResourceReturnType::kUInt),
                    dxbc::Src::T(dxbc::Src::Dcl, 1, 1, 1));
  }
  // EDRAM buffer.
  a.OpDclUnorderedAccessViewTyped(dxbc::ResourceDimension::kBuffer, 0,
                                  dxbc::ResourceReturnTypeX4Token(dxbc::ResourceReturnType::kUInt),
                                  dxbc::Src::U(dxbc::Src::Dcl, 0, 0, 0));
  a.OpDclInput(dxbc::Dest::VThreadID(0b0011));
  // r0 - addressing before the load, then addressing and conversion scratch
  // r1 - addressing scratch before the load, then data
  a.OpDclTemps(2);
  // There's no strict dependency on the group size here, for simplicity of
  // calculations especially with resolution scaling, dividing manually (as the
  // group size is not unlimited). The only restriction is that an integer
  // multiple of it must be 80x16 samples (and no larger than that) for 32bpp,
  // or 40x16 samples for 64bpp (because only a half of the pair of tiles may
  // need to be dumped). The group size limit in Direct3D 11 is 1024, and 40x16
  // fits in it, while 80x16 doesn't.
  a.OpDclThreadGroup(40, 16, 1);

  uint32_t draw_resolution_scale_x = options.resolution_scale_x;
  uint32_t draw_resolution_scale_y = options.resolution_scale_y;

  // For now, as the exact addressing in 64bpp render targets relatively to
  // 32bpp is unknown, treating 64bpp tiles as storing 40x16 samples rather than
  // 80x16 for simplicity of addressing into the texture.

  uint32_t tile_width =
      (xenos::kEdramTileWidthSamples * draw_resolution_scale_x) >> uint32_t(format_is_64bpp);
  uint32_t tile_height = xenos::kEdramTileHeightSamples * draw_resolution_scale_y;

  // Get the parts of the address - tile row index within the dispatch to r0.zw,
  // sample Y within the tile to r0.xy.
  // r0.x = X sample position within the tile
  // r0.y = Y sample position within the tile
  // r0.z = X tile position
  // r0.w = Y tile position
  a.OpUDiv(dxbc::Dest::R(0, 0b1100), dxbc::Dest::R(0, 0b0011), dxbc::Src::VThreadID(0b01000100),
           dxbc::Src::LU(tile_width, tile_height, tile_width, tile_height));

  // Extract the dump rectangle tile row pitch to r1.x.
  // r0.x = X sample position within the tile
  // r0.y = Y sample position within the tile
  // r0.z = X tile position
  // r0.w = Y tile position
  // r1.x = dump rectangle pitch in tiles
  a.OpUBFE(dxbc::Dest::R(1, 0b0001), dxbc::Src::LU(xenos::kEdramPitchTilesBits), dxbc::Src::LU(0),
           dxbc::Src::CB(kDumpCbufferPitches, kDumpCbufferPitches, 0, dxbc::Src::kXXXX));
  // Get the tile index in the EDRAM relative to the dump rectangle base tile to
  // r0.w.
  // r0.x = X sample position within the tile
  // r0.y = Y sample position within the tile
  // r0.z = free
  // r0.w = tile index relative to the dump rectangle base
  // r1.x = free
  a.OpUMAd(dxbc::Dest::R(0, 0b1000), dxbc::Src::R(0, dxbc::Src::kWWWW),
           dxbc::Src::R(1, dxbc::Src::kXXXX), dxbc::Src::R(0, dxbc::Src::kZZZZ));

  // Extract the index of the first tile (taking EDRAM addressing wrapping into
  // account) of the dispatch in the EDRAM to r0.z.
  // r0.x = X sample position within the tile
  // r0.y = Y sample position within the tile
  // r0.z = first EDRAM tile index in the dispatch
  // r0.w = tile index relative to the dump rectangle base
  a.OpUBFE(dxbc::Dest::R(0, 0b0100), dxbc::Src::LU(xenos::kEdramBaseTilesBits + 1),
           dxbc::Src::LU(0),
           dxbc::Src::CB(kDumpCbufferOffsets, kDumpCbufferOffsets, 0, dxbc::Src::kXXXX));
  // Add the base tile in the dispatch to the dispatch-local tile index to r0.w,
  // not wrapping yet so in case of a wraparound, the address relative to the
  // base in the texture after subtraction of the base won't be negative.
  // r0.x = X sample position within the tile
  // r0.y = Y sample position within the tile
  // r0.z = free
  // r0.w = non-wrapped tile index in the EDRAM
  a.OpIAdd(dxbc::Dest::R(0, 0b1000), dxbc::Src::R(0, dxbc::Src::kWWWW),
           dxbc::Src::R(0, dxbc::Src::kZZZZ));
  // Wrap the address of the tile in the EDRAM to r0.z.
  // r0.x = X sample position within the tile
  // r0.y = Y sample position within the tile
  // r0.z = wrapped tile index in the EDRAM
  // r0.w = non-wrapped tile index in the EDRAM
  a.OpAnd(dxbc::Dest::R(0, 0b0100), dxbc::Src::R(0, dxbc::Src::kWWWW),
          dxbc::Src::LU(xenos::kEdramTileCount - 1));
  // Convert the tile index to samples and add the X sample index to it to r0.z.
  // r0.x = X sample position within the tile
  // r0.y = Y sample position within the tile
  // r0.z = tile sample offset in the EDRAM plus X sample offset
  // r0.w = non-wrapped tile index in the EDRAM
  a.OpUMAd(dxbc::Dest::R(0, 0b0100), dxbc::Src::R(0, dxbc::Src::kZZZZ),
           dxbc::Src::LU(draw_resolution_scale_x * draw_resolution_scale_y *
                         (xenos::kEdramTileWidthSamples >> uint32_t(format_is_64bpp)) *
                         xenos::kEdramTileHeightSamples),
           dxbc::Src::R(0, dxbc::Src::kXXXX));
  // Add the contribution of the Y sample position within the tile to the sample
  // address in the EDRAM to r0.z.
  // r0.x = X sample position within the tile
  // r0.y = Y sample position within the tile
  // r0.z = sample offset in the EDRAM without the depth column swapping
  // r0.w = non-wrapped tile index in the EDRAM
  a.OpUMAd(dxbc::Dest::R(0, 0b0100), dxbc::Src::R(0, dxbc::Src::kYYYY), dxbc::Src::LU(tile_width),
           dxbc::Src::R(0, dxbc::Src::kZZZZ));
  if (key.is_depth) {
    uint32_t tile_width_half = tile_width >> 1;
    // Get which 40-sample half within the tile is being processed to r1.x.
    // r0.x = X sample position within the tile
    // r0.y = Y sample position within the tile
    // r0.z = sample offset in the EDRAM without the depth column swapping
    // r0.w = non-wrapped tile index in the EDRAM
    // r1.x = 0xFFFFFFFF if in the right 40-sample half, 0 otherwise
    a.OpUGE(dxbc::Dest::R(1, 0b0001), dxbc::Src::R(0, dxbc::Src::kXXXX),
            dxbc::Src::LU(tile_width_half));
    // Get the offset needed to swap 40-sample halves for depth.
    // r0.x = X sample position within the tile
    // r0.y = Y sample position within the tile
    // r0.z = sample offset in the EDRAM without the depth column swapping
    // r0.w = non-wrapped tile index in the EDRAM
    // r1.x = depth half-tile flipping offset
    a.OpMovC(dxbc::Dest::R(1, 0b0001), dxbc::Src::R(1, dxbc::Src::kXXXX),
             dxbc::Src::LI(-int32_t(tile_width_half)), dxbc::Src::LI(int32_t(tile_width_half)));
    // Swap 40-sample columns in the depth buffer in the destination address in
    // r0.w to get the final address of the sample in EDRAM.
    // r0.x = X sample position within the tile
    // r0.y = Y sample position within the tile
    // r0.z = sample offset in the EDRAM
    // r0.w = non-wrapped tile index in the EDRAM
    // r1.x = free
    a.OpIAdd(dxbc::Dest::R(0, 0b0100), dxbc::Src::R(0, dxbc::Src::kZZZZ),
             dxbc::Src::R(1, dxbc::Src::kXXXX));
  }

  // Extract the source texture base tile index to r1.x.
  // r0.x = X sample position within the tile
  // r0.y = Y sample position within the tile
  // r0.z = sample offset in the EDRAM
  // r0.w = non-wrapped tile index in the EDRAM
  // r1.x = source texture base tile index
  a.OpUBFE(dxbc::Dest::R(1, 0b0001), dxbc::Src::LU(xenos::kEdramBaseTilesBits),
           dxbc::Src::LU(xenos::kEdramBaseTilesBits + 1),
           dxbc::Src::CB(kDumpCbufferOffsets, kDumpCbufferOffsets, 0, dxbc::Src::kXXXX));
  // Get the linear tile index within the source texture to r0.w.
  // r0.x = X sample position within the tile
  // r0.y = Y sample position within the tile
  // r0.z = sample offset in the EDRAM
  // r0.w = linear tile index in the source texture
  // r1.x = free
  a.OpIAdd(dxbc::Dest::R(0, 0b1000), dxbc::Src::R(0, dxbc::Src::kWWWW),
           -dxbc::Src::R(1, dxbc::Src::kXXXX));
  // Get the source texture pitch in tiles to r1.x.
  // r0.x = X sample position within the tile
  // r0.y = Y sample position within the tile
  // r0.z = sample offset in the EDRAM
  // r0.w = linear tile index in the source texture
  // r1.x = source texture pitch in tiles
  a.OpUBFE(dxbc::Dest::R(1, 0b0001), dxbc::Src::LU(xenos::kEdramPitchTilesBits),
           dxbc::Src::LU(xenos::kEdramPitchTilesBits),
           dxbc::Src::CB(kDumpCbufferPitches, kDumpCbufferPitches, 0, dxbc::Src::kXXXX));
  // Split the linear tile index in the source texture into X and Y in tiles.
  // r0.x = X sample position within the tile
  // r0.y = Y sample position within the tile
  // r0.z = sample offset in the EDRAM
  // r0.w = X tile index within the tile row in the source texture
  // r1.x = Y tile row index within the source texture
  a.OpUDiv(dxbc::Dest::R(1, 0b0001), dxbc::Dest::R(0, 0b1000), dxbc::Src::R(0, dxbc::Src::kWWWW),
           dxbc::Src::R(1, dxbc::Src::kXXXX));
  // Add the source texture tile X offset to the source texture sample X
  // coordinate.
  // r0.x = X sample position within the source texture
  // r0.y = Y sample position within the tile
  // r0.z = sample offset in the EDRAM
  // r0.w = free
  // r1.x = Y tile row index within the source texture
  a.OpUMAd(dxbc::Dest::R(0, 0b0001), dxbc::Src::R(0, dxbc::Src::kWWWW), dxbc::Src::LU(tile_width),
           dxbc::Src::R(0, dxbc::Src::kXXXX));
  // Add the source texture tile Y offset to the source texture sample Y
  // coordinate.
  // r0.x = X sample position within the source texture
  // r0.y = Y sample position within the source texture
  // r0.z = sample offset in the EDRAM
  // r1.x = free
  a.OpUMAd(dxbc::Dest::R(0, 0b0010), dxbc::Src::R(1, dxbc::Src::kXXXX),
           dxbc::Src::LU(xenos::kEdramTileHeightSamples * draw_resolution_scale_y),
           dxbc::Src::R(0, dxbc::Src::kYYYY));
  // Will be using the source texture coordinates from r0.xy, and for
  // single-sampled source, LOD from r0.w.
  dxbc::Src source_address_src(dxbc::Src::R(0, 0b11000100));
  if (key.msaa_samples >= xenos::MsaaSamples::k2X) {
    if (key.msaa_samples >= xenos::MsaaSamples::k4X) {
      // 4x MSAA source texture sample index - bit 0 for horizontal, bit 1 for
      // vertical.
      // Extract the horizontal sample index to r0.w.
      // r0.x = X sample position within the source texture
      // r0.y = Y sample position within the source texture
      // r0.z = sample offset in the EDRAM
      // r0.w = horizontal sample index within the source pixel
      a.OpAnd(dxbc::Dest::R(0, 0b1000), dxbc::Src::R(0, dxbc::Src::kXXXX), dxbc::Src::LU(1));
      // Insert the vertical sample index to r0.w.
      // r0.x = X sample position within the source texture
      // r0.y = Y sample position within the source texture
      // r0.z = sample offset in the EDRAM
      // r0.w = sample index within the source pixel
      a.OpBFI(dxbc::Dest::R(0, 0b1000), dxbc::Src::LU(1), dxbc::Src::LU(1),
              dxbc::Src::R(0, dxbc::Src::kYYYY), dxbc::Src::R(0, dxbc::Src::kWWWW));
      // Convert sample to pixel coordinates in the source texture to r0.xy.
      // r0.x = X pixel position within the source texture
      // r0.y = Y pixel position within the source texture
      // r0.z = sample offset in the EDRAM
      // r0.w = sample index within the source pixel
      a.OpUShR(dxbc::Dest::R(0, 0b0011), dxbc::Src::R(0), dxbc::Src::LU(1));
    } else {
      // 2x MSAA source texture sample index.
      // Extract the vertical sample index to r0.w.
      // r0.x = X pixel position within the source texture
      // r0.y = Y sample position within the source texture
      // r0.z = sample offset in the EDRAM
      // r0.w = vertical sample index within the destination pixel
      a.OpAnd(dxbc::Dest::R(0, 0b1000), dxbc::Src::R(0, dxbc::Src::kYYYY), dxbc::Src::LU(1));
      // Convert the 2x MSAA sample index from the guest to Direct3D 10.1+.
      // r0.x = X pixel position within the source texture
      // r0.y = Y sample position within the source texture
      // r0.z = sample offset in the EDRAM
      // r0.w = sample index within the source pixel
      a.OpMovC(
          dxbc::Dest::R(0, 0b1000), dxbc::Src::R(0, dxbc::Src::kWWWW),
          dxbc::Src::LU(draw_util::GetD3D10SampleIndexForGuest2xMSAA(1, options.msaa_2x_supported)),
          dxbc::Src::LU(
              draw_util::GetD3D10SampleIndexForGuest2xMSAA(0, options.msaa_2x_supported)));
      // Convert sample Y to pixel Y in the source texture to r0.y.
      // r0.x = X pixel position within the source texture
      // r0.y = Y pixel position within the source texture
      // r0.z = sample offset in the EDRAM
      // r0.w = sample index within the source pixel
      a.OpUShR(dxbc::Dest::R(0, 0b0010), dxbc::Src::R(0, dxbc::Src::kYYYY), dxbc::Src::LU(1));
    }
    // Load the source to r1.
    // r0.x = X pixel position within the source texture if stencil is needed
    // r0.y = Y pixel position within the source texture if stencil is needed
    // r0.z = sample offset in the EDRAM
    // r0.w = sample index within the source pixel if stencil is needed
    // r1 = source texel value
    a.OpLdMS(dxbc::Dest::R(1, (1 << source_component_count) - 1), source_address_src, 0b0011,
             dxbc::Src::T(0, 0), dxbc::Src::R(0, dxbc::Src::kWWWW));
    if (key.is_depth) {
      // Load the source stencil to r1.y.
      // r0.x = free
      // r0.y = free
      // r0.z = sample offset in the EDRAM
      // r0.w = free
      // r1.x = source depth value
      // r1.y = source stencil value
      a.OpLdMS(dxbc::Dest::R(1, 0b0010), source_address_src, 0b0011, dxbc::Src::T(1, 1),
               dxbc::Src::R(0, dxbc::Src::kWWWW));
    }
  } else {
    // Write the LOD index (0) to the register with texture coordinates for
    // loading from the single-sampled source texture.
    // r0.x = X pixel position within the source texture
    // r0.y = Y pixel position within the source texture
    // r0.z = sample offset in the EDRAM
    // r0.w = LOD for the texture load (zero)
    a.OpMov(dxbc::Dest::R(0, 0b1000), dxbc::Src::LF(0.0f));
    // Load the source to r1.
    // r0.x = X pixel position within the source texture if stencil is needed
    // r0.y = Y pixel position within the source texture if stencil is needed
    // r0.z = sample offset in the EDRAM
    // r0.w = LOD for the texture load (zero)
    // r1 = source texel value
    a.OpLd(dxbc::Dest::R(1, (1 << source_component_count) - 1), source_address_src, 0b1011,
           dxbc::Src::T(0, 0));
    if (key.is_depth) {
      // Load the source stencil to r1.y.
      // r0.x = free
      // r0.y = free
      // r0.z = sample offset in the EDRAM
      // r0.w = free
      // r1.x = source depth value
      // r1.y = source stencil value
      a.OpLd(dxbc::Dest::R(1, 0b0010), source_address_src, 0b1011, dxbc::Src::T(1, 1));
    }
  }

  // Pack in the needed format, writing the result to r1.x for 32bpp or r1.xy
  // for 64bpp.
  // r0.xyw are usable as temporary storage.
  if (key.is_depth) {
    switch (key.GetDepthFormat()) {
      case xenos::DepthRenderTargetFormat::kD24S8:
        // Round to the nearest even integer. This seems to be the correct
        // conversion, adding +0.5 and rounding towards zero results in red
        // instead of black in the 4D5307E6 clear shader.
        a.OpMul(dxbc::Dest::R(1, 0b0001), dxbc::Src::R(1, dxbc::Src::kXXXX),
                dxbc::Src::LF(float(0xFFFFFF)));
        a.OpRoundNE(dxbc::Dest::R(1, 0b0001), dxbc::Src::R(1, dxbc::Src::kXXXX));
        a.OpFToU(dxbc::Dest::R(1, 0b0001), dxbc::Src::R(1, dxbc::Src::kXXXX));
        break;
      case xenos::DepthRenderTargetFormat::kD24FS8:
        // Convert to [0, 2) float24 from [0, 1) float32, using r0.x as
        // temporary.
        // When converting the depth in pixel shaders, it's always exact,
        // truncating not to insert additional rounding instructions.
        DxbcFloatEmitter::PreClampedDepthTo20e4(
            a, 1, 0, 1, 0, 0, 0,
            !options.depth_float24_convert_in_pixel_shader && options.depth_float24_round, true);
        break;
    }
    // Combine 24-bit depth and stencil into r1.x.
    a.OpBFI(dxbc::Dest::R(1, 0b0001), dxbc::Src::LU(24), dxbc::Src::LU(8),
            dxbc::Src::R(1, dxbc::Src::kXXXX), dxbc::Src::R(1, dxbc::Src::kYYYY));
  } else {
    switch (key.GetColorFormat()) {
      case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA:
        // 8_8_8_8_GAMMA is represented by linear stored in
        // R16G16B16A16_UNORM.
        assert_false(source_is_uint);
        for (uint32_t i = 0; i < 3; ++i) {
          DxbcFloatEmitter::PreSaturatedLinearToPWLGamma(a, 1, i, 1, i, 0, 0, 0, 1);
        }
        a.OpMAd(dxbc::Dest::R(1), dxbc::Src::R(1), dxbc::Src::LF(255.0f), dxbc::Src::LF(0.5f));
        a.OpFToU(dxbc::Dest::R(1), dxbc::Src::R(1));
        for (uint32_t i = 1; i < 4; ++i) {
          a.OpBFI(dxbc::Dest::R(1, 0b0001), dxbc::Src::LU(8), dxbc::Src::LU(i * 8),
                  dxbc::Src::R(1).Select(i), dxbc::Src::R(1, dxbc::Src::kXXXX));
        }
        break;
      case xenos::ColorRenderTargetFormat::k_8_8_8_8:
        if (!source_is_uint) {
          a.OpMAd(dxbc::Dest::R(1), dxbc::Src::R(1), dxbc::Src::LF(255.0f), dxbc::Src::LF(0.5f));
          a.OpFToU(dxbc::Dest::R(1), dxbc::Src::R(1));
        }
        for (uint32_t i = 1; i < 4; ++i) {
          a.OpBFI(dxbc::Dest::R(1, 0b0001), dxbc::Src::LU(8), dxbc::Src::LU(i * 8),
                  dxbc::Src::R(1).Select(i), dxbc::Src::R(1, dxbc::Src::kXXXX));
        }
        break;
      case xenos::ColorRenderTargetFormat::k_2_10_10_10:
      case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10:
        if (!source_is_uint) {
          a.OpMAd(dxbc::Dest::R(1), dxbc::Src::R(1), dxbc::Src::LF(1023.0f, 1023.0f, 1023.0f, 3.0f),
                  dxbc::Src::LF(0.5f));
          a.OpFToU(dxbc::Dest::R(1), dxbc::Src::R(1));
        }
        for (uint32_t i = 1; i < 4; ++i) {
          a.OpBFI(dxbc::Dest::R(1, 0b0001), dxbc::Src::LU(i == 3 ? 2 : 10), dxbc::Src::LU(i * 10),
                  dxbc::Src::R(1).Select(i), dxbc::Src::R(1, dxbc::Src::kXXXX));
        }
        break;
      case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
      case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16:
        // Float16 has a wider range for both color and alpha, also NaNs.
        // Color - clamp and convert.
        // Convert red in r1.x to the result register r1.x - the same, but
        // UnclampedFloat32To7e3 allows that - using r0.x as a temporary.
        DxbcFloatEmitter::UnclampedFloat32To7e3(a, 1, 0, 1, 0, 0, 0);
        for (uint32_t i = 1; i < 3; ++i) {
          // Convert green and blue to a temporary register r0.x using r0.y
          // as an internal temporary, then insert them into the result in
          // r1.x.
          DxbcFloatEmitter::UnclampedFloat32To7e3(a, 0, 0, 1, i, 0, 1);
          a.OpBFI(dxbc::Dest::R(1, 0b0001), dxbc::Src::LU(10), dxbc::Src::LU(i * 10),
                  dxbc::Src::R(0, dxbc::Src::kXXXX), dxbc::Src::R(1, dxbc::Src::kXXXX));
        }
        // Alpha - saturate and convert.
        a.OpMov(dxbc::Dest::R(1, 0b1000), dxbc::Src::R(1, dxbc::Src::kWWWW), true);
        a.OpMAd(dxbc::Dest::R(1, 0b1000), dxbc::Src::R(1, dxbc::Src::kWWWW), dxbc::Src::LF(3.0f),
                dxbc::Src::LF(0.5f));
        a.OpFToU(dxbc::Dest::R(1, 0b1000), dxbc::Src::R(1, dxbc::Src::kWWWW));
        a.OpBFI(dxbc::Dest::R(1, 0b0001), dxbc::Src::LU(2), dxbc::Src::LU(30),
                dxbc::Src::R(1, dxbc::Src::kWWWW), dxbc::Src::R(1, dxbc::Src::kXXXX));
        break;
      case xenos::ColorRenderTargetFormat::k_16_16:
      case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
        assert_true(source_is_uint);
        a.OpBFI(dxbc::Dest::R(1, 0b0001), dxbc::Src::LU(16), dxbc::Src::LU(16),
                dxbc::Src::R(1, dxbc::Src::kYYYY), dxbc::Src::R(1, dxbc::Src::kXXXX));
        break;
      case xenos::ColorRenderTargetFormat::k_16_16_16_16:
      case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT:
        assert_true(source_is_uint);
        a.OpBFI(dxbc::Dest::R(1, 0b0011), dxbc::Src::LU(16), dxbc::Src::LU(16),
                dxbc::Src::R(1, 0b1101), dxbc::Src::R(1, 0b1000));
        break;
      case xenos::ColorRenderTargetFormat::k_32_FLOAT:
      case xenos::ColorRenderTargetFormat::k_32_32_FLOAT:
        assert_true(source_is_uint);
        // Already has the needed representation.
        break;
    }
  }

  // Write the sample to the destination address stored in r0.z.
  a.OpStoreUAVTyped(dxbc::Dest::U(0, 0), dxbc::Src::R(0, dxbc::Src::kZZZZ), 1,
                    dxbc::Src::R(1, format_is_64bpp ? 0b0100 : dxbc::Src::kXXXX));

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
    container_header.blob_count = kBlobCount;
    CalculateDXBCChecksum(reinterpret_cast<unsigned char*>(output.data()),
                          static_cast<unsigned int>(output_size_bytes),
                          reinterpret_cast<unsigned int*>(&container_header.hash));
  }

  return true;
}

}  // namespace rex::graphics
