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

#include "thirdparty/dxbc/DXBCChecksum.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <d3dcompiler.h>
#include <cstring>
#include <iterator>
#include <memory>
#include <string>
#include <stdexcept>
#include <tuple>
#include <utility>

#include <rex/assert.h>
#include <rex/cvar.h>
#include <rex/graphics/d3d12/command_processor.h>
#include <rex/graphics/d3d12/deferred_command_list.h>
#include <rex/graphics/d3d12/render_target_cache.h>
#include <rex/graphics/d3d12/texture_cache.h>
#include <rex/graphics/flags.h>
#include <rex/graphics/format/dxbc.h>
#include <rex/graphics/pipeline/shader/dxbc_translator.h>
#include <rex/graphics/trace_writer.h>
#include <rex/graphics/util/draw.h>
#include <rex/graphics/util/dxbc_render_target_transfer.h>
#include <rex/graphics/util/native_color_resolve.h>
#include <rex/graphics/util/native_resolve_source.h>
#include <rex/graphics/util/native_depth_resolve.h>

#include <rex/graphics/xenos.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/string.h>
#include <rex/ui/d3d12/d3d12_provider.h>
#include <rex/ui/d3d12/d3d12_util.h>

REXCVAR_DEFINE_BOOL(d3d12_native_resolve_memory, true, "GPU/D3D12",
                    "Keep supported original-resolution resolves in resource-local GPU "
                    "buffers, materializing compatibility memory only when requested")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(d3d12_native_resolve_debug, false, "GPU/D3D12",
                    "Log the remaining native resolve planning failures");
REXCVAR_DEFINE_BOOL(d3d12_native_depth_resolves, true, "GPU/D3D12",
                    "Pack supported depth and stencil directly from native attachments");

REXCVAR_DEFINE_STRING(render_target_path_d3d12, "", "GPU/D3D12",
                      "D3D12 render target implementation path")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace rex::graphics::d3d12 {

// Generated with `xb buildshaders`.
namespace shaders {
#include "../shaders/bytecode/d3d12_5_1/clear_uint2_ps.h"
#include "../shaders/bytecode/d3d12_5_1/fullscreen_cw_vs.h"
#include "../shaders/bytecode/d3d12_5_1/host_depth_store_1xmsaa_cs.h"
#include "../shaders/bytecode/d3d12_5_1/host_depth_store_2xmsaa_cs.h"
#include "../shaders/bytecode/d3d12_5_1/host_depth_store_4xmsaa_cs.h"
#include "../shaders/bytecode/d3d12_5_1/passthrough_position_xy_vs.h"
#include "../shaders/bytecode/d3d12_5_1/resolve_clear_32bpp_cs.h"
#include "../shaders/bytecode/d3d12_5_1/resolve_clear_32bpp_scaled_cs.h"
#include "../shaders/bytecode/d3d12_5_1/resolve_clear_64bpp_cs.h"
#include "../shaders/bytecode/d3d12_5_1/resolve_clear_64bpp_scaled_cs.h"
#include "../shaders/bytecode/d3d12_5_1/resolve_fast_32bpp_1x2xmsaa_cs.h"
#include "../shaders/bytecode/d3d12_5_1/resolve_fast_32bpp_1x2xmsaa_scaled_cs.h"
#include "../shaders/bytecode/d3d12_5_1/resolve_fast_32bpp_4xmsaa_cs.h"
#include "../shaders/bytecode/d3d12_5_1/resolve_fast_32bpp_4xmsaa_scaled_cs.h"
#include "../shaders/bytecode/d3d12_5_1/resolve_fast_64bpp_1x2xmsaa_cs.h"
#include "../shaders/bytecode/d3d12_5_1/resolve_fast_64bpp_1x2xmsaa_scaled_cs.h"
#include "../shaders/bytecode/d3d12_5_1/resolve_fast_64bpp_4xmsaa_cs.h"
#include "../shaders/bytecode/d3d12_5_1/resolve_fast_64bpp_4xmsaa_scaled_cs.h"
#include "../shaders/bytecode/d3d12_5_1/resolve_full_128bpp_cs.h"
#include "../shaders/bytecode/d3d12_5_1/resolve_full_128bpp_scaled_cs.h"
#include "../shaders/bytecode/d3d12_5_1/resolve_full_16bpp_cs.h"
#include "../shaders/bytecode/d3d12_5_1/resolve_full_16bpp_scaled_cs.h"
#include "../shaders/bytecode/d3d12_5_1/resolve_full_32bpp_cs.h"
#include "../shaders/bytecode/d3d12_5_1/resolve_full_32bpp_scaled_cs.h"
#include "../shaders/bytecode/d3d12_5_1/resolve_full_64bpp_cs.h"
#include "../shaders/bytecode/d3d12_5_1/resolve_full_64bpp_scaled_cs.h"
#include "../shaders/bytecode/d3d12_5_1/resolve_full_8bpp_cs.h"
#include "../shaders/bytecode/d3d12_5_1/resolve_full_8bpp_scaled_cs.h"
}  // namespace shaders

const D3D12RenderTargetCache::ResolveCopyShaderCode
    D3D12RenderTargetCache::kResolveCopyShaders[size_t(
        draw_util::ResolveCopyShaderIndex::kCount)] = {
        {shaders::resolve_fast_32bpp_1x2xmsaa_cs, sizeof(shaders::resolve_fast_32bpp_1x2xmsaa_cs),
         shaders::resolve_fast_32bpp_1x2xmsaa_scaled_cs,
         sizeof(shaders::resolve_fast_32bpp_1x2xmsaa_scaled_cs)},
        {shaders::resolve_fast_32bpp_4xmsaa_cs, sizeof(shaders::resolve_fast_32bpp_4xmsaa_cs),
         shaders::resolve_fast_32bpp_4xmsaa_scaled_cs,
         sizeof(shaders::resolve_fast_32bpp_4xmsaa_scaled_cs)},
        {shaders::resolve_fast_64bpp_1x2xmsaa_cs, sizeof(shaders::resolve_fast_64bpp_1x2xmsaa_cs),
         shaders::resolve_fast_64bpp_1x2xmsaa_scaled_cs,
         sizeof(shaders::resolve_fast_64bpp_1x2xmsaa_scaled_cs)},
        {shaders::resolve_fast_64bpp_4xmsaa_cs, sizeof(shaders::resolve_fast_64bpp_4xmsaa_cs),
         shaders::resolve_fast_64bpp_4xmsaa_scaled_cs,
         sizeof(shaders::resolve_fast_64bpp_4xmsaa_scaled_cs)},
        {shaders::resolve_full_8bpp_cs, sizeof(shaders::resolve_full_8bpp_cs),
         shaders::resolve_full_8bpp_scaled_cs, sizeof(shaders::resolve_full_8bpp_scaled_cs)},
        {shaders::resolve_full_16bpp_cs, sizeof(shaders::resolve_full_16bpp_cs),
         shaders::resolve_full_16bpp_scaled_cs, sizeof(shaders::resolve_full_16bpp_scaled_cs)},
        {shaders::resolve_full_32bpp_cs, sizeof(shaders::resolve_full_32bpp_cs),
         shaders::resolve_full_32bpp_scaled_cs, sizeof(shaders::resolve_full_32bpp_scaled_cs)},
        {shaders::resolve_full_64bpp_cs, sizeof(shaders::resolve_full_64bpp_cs),
         shaders::resolve_full_64bpp_scaled_cs, sizeof(shaders::resolve_full_64bpp_scaled_cs)},
        {shaders::resolve_full_128bpp_cs, sizeof(shaders::resolve_full_128bpp_cs),
         shaders::resolve_full_128bpp_scaled_cs, sizeof(shaders::resolve_full_128bpp_scaled_cs)},
};

const uint32_t D3D12RenderTargetCache::kTransferUsedRootParameters[size_t(
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

const D3D12RenderTargetCache::TransferModeInfo
    D3D12RenderTargetCache::kTransferModes[size_t(TransferMode::kCount)] = {
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

D3D12RenderTargetCache::~D3D12RenderTargetCache() {
  Shutdown(true);
}

bool D3D12RenderTargetCache::Initialize() {
  const ui::d3d12::D3D12Provider& provider = command_processor_.GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();

  if (REXCVAR_GET(render_target_path_d3d12) == "rtv") {
    path_ = Path::kHostRenderTargets;
  } else if (REXCVAR_GET(render_target_path_d3d12) == "rov") {
    path_ = Path::kPixelShaderInterlock;
  } else {
    // As of April 2021 (driver version 27.20.0100.9316), on Intel (tested on
    // UHD Graphics 630), the "always" stencil comparison function isn't working
    // properly, so clears in the Xbox 360's Direct3D 9 don't work. Forcing ROV
    // there.
#if 1
    // The ROV path is currently much slower generally.
    // TODO(Triang3l): Make ROV the default when it's optimized better (for
    // instance, using static shader modifications to pass render target
    // parameters).
    path_ = provider.GetAdapterVendorID() == ui::GraphicsProvider::GpuVendorID::kIntel
                ? Path::kPixelShaderInterlock
                : Path::kHostRenderTargets;
#else
    // The AMD shader compiler crashes very often with Xenia's custom
    // output-merger code as of March 2021.
    path_ = provider.GetAdapterVendorID() == ui::GraphicsProvider::GpuVendorID::kAMD
                ? Path::kHostRenderTargets
                : Path::kPixelShaderInterlock;
#endif
  }
  if (path_ == Path::kPixelShaderInterlock && !provider.AreRasterizerOrderedViewsSupported()) {
    path_ = Path::kHostRenderTargets;
  }

  // Create non-shader-visible descriptors of the EDRAM buffer for copying.
  D3D12_DESCRIPTOR_HEAP_DESC edram_buffer_descriptor_heap_desc;
  edram_buffer_descriptor_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  edram_buffer_descriptor_heap_desc.NumDescriptors = uint32_t(EdramBufferDescriptorIndex::kCount);
  edram_buffer_descriptor_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
  edram_buffer_descriptor_heap_desc.NodeMask = 0;
  if (FAILED(device->CreateDescriptorHeap(&edram_buffer_descriptor_heap_desc,
                                          IID_PPV_ARGS(&edram_buffer_descriptor_heap_)))) {
    REXGPU_ERROR(
        "D3D12RenderTargetCache: Failed to create the descriptor heap for "
        "EDRAM buffer views");
    Shutdown();
    return false;
  }
  edram_buffer_descriptor_heap_start_ =
      edram_buffer_descriptor_heap_->GetCPUDescriptorHandleForHeapStart();
  if (path_ == Path::kPixelShaderInterlock) {
    if (!EnsureEdramBuffer()) {
      Shutdown();
      return false;
    }
  } else {
    UpdateEdramBufferDescriptors();
  }

  bool draw_resolution_scaled = IsDrawResolutionScaled();

  // Create the resolve copying root signature.
  std::array<D3D12_ROOT_PARAMETER, 3> resolve_copy_root_parameters;
  // Parameter 0 is constants.
  resolve_copy_root_parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  resolve_copy_root_parameters[0].Constants.ShaderRegister = 0;
  resolve_copy_root_parameters[0].Constants.RegisterSpace = 0;
  // Binding all of the shared memory at 1x resolution, portions with scaled
  // resolution.
  resolve_copy_root_parameters[0].Constants.Num32BitValues =
      (draw_resolution_scaled ? sizeof(draw_util::ResolveCopyShaderConstants::DestRelative)
                              : sizeof(draw_util::ResolveCopyShaderConstants)) /
      sizeof(uint32_t);
  resolve_copy_root_parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  // Parameter 1 is the destination (shared memory).
  D3D12_DESCRIPTOR_RANGE resolve_copy_dest_range;
  resolve_copy_dest_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
  resolve_copy_dest_range.NumDescriptors = 1;
  resolve_copy_dest_range.BaseShaderRegister = 0;
  resolve_copy_dest_range.RegisterSpace = 0;
  resolve_copy_dest_range.OffsetInDescriptorsFromTableStart = 0;
  resolve_copy_root_parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  resolve_copy_root_parameters[1].DescriptorTable.NumDescriptorRanges = 1;
  resolve_copy_root_parameters[1].DescriptorTable.pDescriptorRanges = &resolve_copy_dest_range;
  resolve_copy_root_parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  // Parameter 2 is the source (EDRAM).
  D3D12_DESCRIPTOR_RANGE resolve_copy_source_range;
  resolve_copy_source_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  resolve_copy_source_range.NumDescriptors = 1;
  resolve_copy_source_range.BaseShaderRegister = 0;
  resolve_copy_source_range.RegisterSpace = 0;
  resolve_copy_source_range.OffsetInDescriptorsFromTableStart = 0;
  resolve_copy_root_parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  resolve_copy_root_parameters[2].DescriptorTable.NumDescriptorRanges = 1;
  resolve_copy_root_parameters[2].DescriptorTable.pDescriptorRanges = &resolve_copy_source_range;
  resolve_copy_root_parameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  D3D12_ROOT_SIGNATURE_DESC resolve_copy_root_signature_desc;
  resolve_copy_root_signature_desc.NumParameters = UINT(resolve_copy_root_parameters.size());
  resolve_copy_root_signature_desc.pParameters = resolve_copy_root_parameters.data();
  resolve_copy_root_signature_desc.NumStaticSamplers = 0;
  resolve_copy_root_signature_desc.pStaticSamplers = nullptr;
  resolve_copy_root_signature_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
  resolve_copy_root_signature_ =
      ui::d3d12::util::CreateRootSignature(provider, resolve_copy_root_signature_desc);
  if (resolve_copy_root_signature_ == nullptr) {
    REXGPU_ERROR(
        "D3D12RenderTargetCache: Failed to create the resolve copy root "
        "signature");
    Shutdown();
    return false;
  }
  // Create the resolve copying pipelines.
  for (size_t i = 0; i < size_t(draw_util::ResolveCopyShaderIndex::kCount); ++i) {
    const draw_util::ResolveCopyShaderInfo& resolve_copy_shader_info =
        draw_util::resolve_copy_shader_info[i];
    const ResolveCopyShaderCode& resolve_copy_shader_code = kResolveCopyShaders[i];
    // Somewhat verification whether resolve_copy_shaders_ is up to date.
    assert_true(resolve_copy_shader_code.unscaled && resolve_copy_shader_code.unscaled_size &&
                resolve_copy_shader_code.scaled && resolve_copy_shader_code.scaled_size);
    ID3D12PipelineState* resolve_copy_pipeline = ui::d3d12::util::CreateComputePipeline(
        device,
        draw_resolution_scaled ? resolve_copy_shader_code.scaled
                               : resolve_copy_shader_code.unscaled,
        draw_resolution_scaled ? resolve_copy_shader_code.scaled_size
                               : resolve_copy_shader_code.unscaled_size,
        resolve_copy_root_signature_);
    if (resolve_copy_pipeline == nullptr) {
      REXGPU_ERROR("D3D12RenderTargetCache: Failed to create {} resolve copy pipeline",
                   resolve_copy_shader_info.debug_name);
      Shutdown();
      return false;
    }
    std::u16string resolve_copy_pipeline_name =
        rex::string::to_utf16(resolve_copy_shader_info.debug_name);
    resolve_copy_pipeline->SetName(reinterpret_cast<LPCWSTR>(resolve_copy_pipeline_name.c_str()));
    resolve_copy_pipelines_[i] = resolve_copy_pipeline;
  }

  // Using the cvar on emulator initialization so used pipelines are consistent
  // across different titles launched in one emulator instance.
  use_stencil_reference_output_ =
      REXCVAR_GET(native_stencil_value_output) &&
      provider.IsPSSpecifiedStencilReferenceSupported() &&
      (REXCVAR_GET(native_stencil_value_output_d3d12_intel) ||
       provider.GetAdapterVendorID() != ui::GraphicsProvider::GpuVendorID::kIntel);

  if (path_ == Path::kHostRenderTargets) {
    // Host render targets.

    gamma_render_target_as_unorm16_ = REXCVAR_GET(gamma_render_target_as_unorm16);

    depth_float24_round_ = REXCVAR_GET(depth_float24_round);
    depth_float24_convert_in_pixel_shader_ = REXCVAR_GET(depth_float24_convert_in_pixel_shader);

    // Check if 2x MSAA is supported or needs to be emulated with 4x MSAA
    // instead.
    if (REXCVAR_GET(native_2x_msaa)) {
      msaa_2x_supported_ = true;
      static const DXGI_FORMAT kRenderTargetDXGIFormats[] = {
          DXGI_FORMAT_R16G16B16A16_FLOAT,
          DXGI_FORMAT_R16G16B16A16_SNORM,
          DXGI_FORMAT_R32G32_FLOAT,
          DXGI_FORMAT_D32_FLOAT_S8X24_UINT,
          DXGI_FORMAT_R10G10B10A2_UNORM,
          DXGI_FORMAT_R8G8B8A8_UNORM,
          DXGI_FORMAT_R16G16_FLOAT,
          DXGI_FORMAT_R16G16_SNORM,
          DXGI_FORMAT_R32_FLOAT,
          DXGI_FORMAT_D24_UNORM_S8_UINT,
          // For ownership transfer.
          DXGI_FORMAT_R16G16B16A16_UINT,
          DXGI_FORMAT_R32G32_UINT,
          DXGI_FORMAT_R16G16_UINT,
          DXGI_FORMAT_R32_UINT,
      };
      D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS multisample_quality_levels;
      multisample_quality_levels.SampleCount = 2;
      multisample_quality_levels.Flags = D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE;
      for (size_t i = 0; i < rex::countof(kRenderTargetDXGIFormats); ++i) {
        multisample_quality_levels.Format = kRenderTargetDXGIFormats[i];
        multisample_quality_levels.NumQualityLevels = 0;
        if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS,
                                               &multisample_quality_levels,
                                               sizeof(multisample_quality_levels))) ||
            !multisample_quality_levels.NumQualityLevels) {
          msaa_2x_supported_ = false;
          break;
        }
      }
    } else {
      msaa_2x_supported_ = false;
    }
    if (msaa_2x_supported_ && gamma_render_target_as_unorm16_) {
      D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS multisample_quality_levels;
      multisample_quality_levels.SampleCount = 2;
      multisample_quality_levels.Flags = D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE;
      multisample_quality_levels.Format = DXGI_FORMAT_R16G16B16A16_UNORM;
      multisample_quality_levels.NumQualityLevels = 0;
      if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS,
                                             &multisample_quality_levels,
                                             sizeof(multisample_quality_levels))) ||
          !multisample_quality_levels.NumQualityLevels) {
        msaa_2x_supported_ = false;
      }
    }
    if (!msaa_2x_supported_) {
      REXGPU_WARN(
          "2x MSAA is not supported, emulated via top-left and bottom-right "
          "samples of 4x MSAA");
    }

    descriptor_pool_color_ = std::make_unique<ui::d3d12::D3D12CpuDescriptorPool>(
        provider, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 11);
    descriptor_pool_depth_ = std::make_unique<ui::d3d12::D3D12CpuDescriptorPool>(
        provider, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 11);
    descriptor_pool_srv_ = std::make_unique<ui::d3d12::D3D12CpuDescriptorPool>(
        provider, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 11);

    // Create null render target descriptors for gaps, must be fully typed
    // (though in pipeline states, DXGI_FORMAT_UNKNOWN must be used instead -
    // this would also cause a mismatching format error in the debug layer, but
    // it's a bug in the debug layer itself - needs to be suppressed, and
    // already fixed in some version of Windows).
    null_rtv_descriptor_ss_ = descriptor_pool_color_->AllocateDescriptor();
    null_rtv_descriptor_ms_ = descriptor_pool_color_->AllocateDescriptor();
    if (!null_rtv_descriptor_ss_ || !null_rtv_descriptor_ms_) {
      Shutdown();
      return false;
    }
    D3D12_RENDER_TARGET_VIEW_DESC null_rtv_desc;
    // The format doesn't matter, but it must be bindable as a render target,
    // not DXGI_FORMAT_UNKNOWN.
    null_rtv_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    null_rtv_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    null_rtv_desc.Texture2D.MipSlice = 0;
    null_rtv_desc.Texture2D.PlaneSlice = 0;
    device->CreateRenderTargetView(nullptr, &null_rtv_desc, null_rtv_descriptor_ss_.GetHandle());
    null_rtv_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMS;
    device->CreateRenderTargetView(nullptr, &null_rtv_desc, null_rtv_descriptor_ms_.GetHandle());

    // For host depth -> same depth transfers, host depth storing root signature
    // and pipelines.
    D3D12_ROOT_PARAMETER
    host_depth_store_root_parameters[kHostDepthStoreRootParameterCount];
    // Constants.
    D3D12_ROOT_PARAMETER& host_depth_store_root_constants =
        host_depth_store_root_parameters[kHostDepthStoreRootParameterConstants];
    host_depth_store_root_constants.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    host_depth_store_root_constants.Constants.ShaderRegister = 0;
    host_depth_store_root_constants.Constants.RegisterSpace = 0;
    host_depth_store_root_constants.Constants.Num32BitValues =
        sizeof(HostDepthStoreConstants) / sizeof(uint32_t);
    host_depth_store_root_constants.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    // Source.
    D3D12_DESCRIPTOR_RANGE host_depth_store_root_source_range;
    host_depth_store_root_source_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    host_depth_store_root_source_range.NumDescriptors = 1;
    host_depth_store_root_source_range.BaseShaderRegister = 0;
    host_depth_store_root_source_range.RegisterSpace = 0;
    host_depth_store_root_source_range.OffsetInDescriptorsFromTableStart = 0;
    D3D12_ROOT_PARAMETER& host_depth_store_root_source =
        host_depth_store_root_parameters[kHostDepthStoreRootParameterSource];
    host_depth_store_root_source.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    host_depth_store_root_source.DescriptorTable.NumDescriptorRanges = 1;
    host_depth_store_root_source.DescriptorTable.pDescriptorRanges =
        &host_depth_store_root_source_range;
    host_depth_store_root_source.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    // Destination.
    D3D12_DESCRIPTOR_RANGE host_depth_store_root_dest_range;
    host_depth_store_root_dest_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    host_depth_store_root_dest_range.NumDescriptors = 1;
    host_depth_store_root_dest_range.BaseShaderRegister = 0;
    host_depth_store_root_dest_range.RegisterSpace = 0;
    host_depth_store_root_dest_range.OffsetInDescriptorsFromTableStart = 0;
    D3D12_ROOT_PARAMETER& host_depth_store_root_dest =
        host_depth_store_root_parameters[kHostDepthStoreRootParameterDest];
    host_depth_store_root_dest.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    host_depth_store_root_dest.DescriptorTable.NumDescriptorRanges = 1;
    host_depth_store_root_dest.DescriptorTable.pDescriptorRanges =
        &host_depth_store_root_dest_range;
    host_depth_store_root_dest.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    // Root signature.
    D3D12_ROOT_SIGNATURE_DESC host_depth_store_root_desc;
    host_depth_store_root_desc.NumParameters = UINT(rex::countof(host_depth_store_root_parameters));
    host_depth_store_root_desc.pParameters = host_depth_store_root_parameters;
    host_depth_store_root_desc.NumStaticSamplers = 0;
    host_depth_store_root_desc.pStaticSamplers = nullptr;
    host_depth_store_root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    host_depth_store_root_signature_ =
        ui::d3d12::util::CreateRootSignature(provider, host_depth_store_root_desc);
    if (!host_depth_store_root_signature_) {
      REXGPU_ERROR(
          "D3D12RenderTargetCache: Failed to create the host depth storing "
          "root signature");
      Shutdown();
      return false;
    }
    // Pipelines.
    // 1 sample.
    host_depth_store_pipelines_[size_t(xenos::MsaaSamples::k1X)] =
        ui::d3d12::util::CreateComputePipeline(device, shaders::host_depth_store_1xmsaa_cs,
                                               sizeof(shaders::host_depth_store_1xmsaa_cs),
                                               host_depth_store_root_signature_);
    if (!host_depth_store_pipelines_[size_t(xenos::MsaaSamples::k1X)]) {
      REXGPU_ERROR(
          "D3D12RenderTargetCache: Failed to create the 1-sample host depth "
          "storing pipeline");
      Shutdown();
      return false;
    }
    host_depth_store_pipelines_[size_t(xenos::MsaaSamples::k1X)]->SetName(
        L"Host Depth Store 1xMSAA");
    // 2 samples.
    host_depth_store_pipelines_[size_t(xenos::MsaaSamples::k2X)] =
        ui::d3d12::util::CreateComputePipeline(device, shaders::host_depth_store_2xmsaa_cs,
                                               sizeof(shaders::host_depth_store_2xmsaa_cs),
                                               host_depth_store_root_signature_);
    if (!host_depth_store_pipelines_[size_t(xenos::MsaaSamples::k2X)]) {
      REXGPU_ERROR(
          "D3D12RenderTargetCache: Failed to create the 2-sample host depth "
          "storing pipeline");
      Shutdown();
      return false;
    }
    host_depth_store_pipelines_[size_t(xenos::MsaaSamples::k2X)]->SetName(
        L"Host Depth Store 2xMSAA");
    // 4 samples.
    host_depth_store_pipelines_[size_t(xenos::MsaaSamples::k4X)] =
        ui::d3d12::util::CreateComputePipeline(device, shaders::host_depth_store_4xmsaa_cs,
                                               sizeof(shaders::host_depth_store_4xmsaa_cs),
                                               host_depth_store_root_signature_);
    if (!host_depth_store_pipelines_[size_t(xenos::MsaaSamples::k4X)]) {
      REXGPU_ERROR(
          "D3D12RenderTargetCache: Failed to create the 4-sample host depth "
          "storing pipeline");
      Shutdown();
      return false;
    }
    host_depth_store_pipelines_[size_t(xenos::MsaaSamples::k4X)]->SetName(
        L"Host Depth Store 4xMSAA");

    // Transfer and clear vertex buffer, for quads of up to tile granularity.
    transfer_vertex_buffer_pool_ = std::make_unique<ui::d3d12::D3D12UploadBufferPool>(
        provider, std::max(ui::d3d12::D3D12UploadBufferPool::kDefaultPageSize,
                           sizeof(float) * 2 * 6 * Transfer::kMaxCutoutBorderRectangles *
                               xenos::kEdramTileCount));

    // Transfer root signatures.
    D3D12_DESCRIPTOR_RANGE transfer_root_color_srv_range;
    transfer_root_color_srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    transfer_root_color_srv_range.NumDescriptors = 1;
    transfer_root_color_srv_range.BaseShaderRegister = kTransferSRVRegisterColor;
    transfer_root_color_srv_range.RegisterSpace = 0;
    transfer_root_color_srv_range.OffsetInDescriptorsFromTableStart = 0;
    D3D12_DESCRIPTOR_RANGE transfer_root_depth_srv_range;
    transfer_root_depth_srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    transfer_root_depth_srv_range.NumDescriptors = 1;
    transfer_root_depth_srv_range.BaseShaderRegister = kTransferSRVRegisterDepth;
    transfer_root_depth_srv_range.RegisterSpace = 0;
    transfer_root_depth_srv_range.OffsetInDescriptorsFromTableStart = 0;
    D3D12_DESCRIPTOR_RANGE transfer_root_stencil_srv_range;
    transfer_root_stencil_srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    transfer_root_stencil_srv_range.NumDescriptors = 1;
    transfer_root_stencil_srv_range.BaseShaderRegister = kTransferSRVRegisterStencil;
    transfer_root_stencil_srv_range.RegisterSpace = 0;
    transfer_root_stencil_srv_range.OffsetInDescriptorsFromTableStart = 0;
    D3D12_DESCRIPTOR_RANGE transfer_root_host_depth_srv_range;
    transfer_root_host_depth_srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    transfer_root_host_depth_srv_range.NumDescriptors = 1;
    transfer_root_host_depth_srv_range.BaseShaderRegister = kTransferSRVRegisterHostDepth;
    transfer_root_host_depth_srv_range.RegisterSpace = 0;
    transfer_root_host_depth_srv_range.OffsetInDescriptorsFromTableStart = 0;
    D3D12_ROOT_PARAMETER
    transfer_root_parameters[kTransferUsedRootParameterCount];
    D3D12_ROOT_SIGNATURE_DESC transfer_root_desc;
    transfer_root_desc.pParameters = transfer_root_parameters;
    transfer_root_desc.NumStaticSamplers = 0;
    transfer_root_desc.pStaticSamplers = nullptr;
    transfer_root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    for (size_t i = 0; i < size_t(TransferRootSignatureIndex::kCount); ++i) {
      uint32_t transfer_root_mask = kTransferUsedRootParameters[i];
      // Stencil mask constant.
      if (transfer_root_mask & kTransferUsedRootParameterStencilMaskConstantBit) {
        D3D12_ROOT_PARAMETER& transfer_root_stencil_mask_constant =
            transfer_root_parameters[rex::bit_count(
                transfer_root_mask & (kTransferUsedRootParameterStencilMaskConstantBit - 1))];
        transfer_root_stencil_mask_constant.ParameterType =
            D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        transfer_root_stencil_mask_constant.Constants.ShaderRegister =
            kTransferCBVRegisterStencilMask;
        transfer_root_stencil_mask_constant.Constants.RegisterSpace = 0;
        transfer_root_stencil_mask_constant.Constants.Num32BitValues = 1;
        transfer_root_stencil_mask_constant.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
      }
      // Color SRV.
      if (transfer_root_mask & kTransferUsedRootParameterColorSRVBit) {
        D3D12_ROOT_PARAMETER& transfer_root_color_srv = transfer_root_parameters[rex::bit_count(
            transfer_root_mask & (kTransferUsedRootParameterColorSRVBit - 1))];
        transfer_root_color_srv.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        transfer_root_color_srv.DescriptorTable.NumDescriptorRanges = 1;
        transfer_root_color_srv.DescriptorTable.pDescriptorRanges = &transfer_root_color_srv_range;
        transfer_root_color_srv.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
      }
      // Depth SRV.
      if (transfer_root_mask & kTransferUsedRootParameterDepthSRVBit) {
        D3D12_ROOT_PARAMETER& transfer_root_depth_srv = transfer_root_parameters[rex::bit_count(
            transfer_root_mask & (kTransferUsedRootParameterDepthSRVBit - 1))];
        transfer_root_depth_srv.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        transfer_root_depth_srv.DescriptorTable.NumDescriptorRanges = 1;
        transfer_root_depth_srv.DescriptorTable.pDescriptorRanges = &transfer_root_depth_srv_range;
        transfer_root_depth_srv.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
      }
      // Stencil SRV.
      if (transfer_root_mask & kTransferUsedRootParameterStencilSRVBit) {
        D3D12_ROOT_PARAMETER& transfer_root_stencil_srv = transfer_root_parameters[rex::bit_count(
            transfer_root_mask & (kTransferUsedRootParameterStencilSRVBit - 1))];
        transfer_root_stencil_srv.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        transfer_root_stencil_srv.DescriptorTable.NumDescriptorRanges = 1;
        transfer_root_stencil_srv.DescriptorTable.pDescriptorRanges =
            &transfer_root_stencil_srv_range;
        transfer_root_stencil_srv.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
      }
      // Address constant.
      if (transfer_root_mask & kTransferUsedRootParameterAddressConstantBit) {
        D3D12_ROOT_PARAMETER& transfer_root_address_constant =
            transfer_root_parameters[rex::bit_count(
                transfer_root_mask & (kTransferUsedRootParameterAddressConstantBit - 1))];
        transfer_root_address_constant.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        transfer_root_address_constant.Constants.ShaderRegister = kTransferCBVRegisterAddress;
        transfer_root_address_constant.Constants.RegisterSpace = 0;
        transfer_root_address_constant.Constants.Num32BitValues =
            sizeof(TransferAddressConstant) / sizeof(uint32_t);
        transfer_root_address_constant.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
      }
      // Host depth SRV.
      if (transfer_root_mask & kTransferUsedRootParameterHostDepthSRVBit) {
        D3D12_ROOT_PARAMETER& transfer_root_host_depth_srv =
            transfer_root_parameters[rex::bit_count(
                transfer_root_mask & (kTransferUsedRootParameterHostDepthSRVBit - 1))];
        transfer_root_host_depth_srv.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        transfer_root_host_depth_srv.DescriptorTable.NumDescriptorRanges = 1;
        transfer_root_host_depth_srv.DescriptorTable.pDescriptorRanges =
            &transfer_root_host_depth_srv_range;
        transfer_root_host_depth_srv.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
      }
      // Host depth address constant.
      if (transfer_root_mask & kTransferUsedRootParameterHostDepthAddressConstantBit) {
        D3D12_ROOT_PARAMETER& transfer_root_host_address_constant =
            transfer_root_parameters[rex::bit_count(
                transfer_root_mask & (kTransferUsedRootParameterHostDepthAddressConstantBit - 1))];
        transfer_root_host_address_constant.ParameterType =
            D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        transfer_root_host_address_constant.Constants.ShaderRegister =
            kTransferCBVRegisterHostDepthAddress;
        transfer_root_host_address_constant.Constants.RegisterSpace = 0;
        transfer_root_host_address_constant.Constants.Num32BitValues =
            sizeof(TransferAddressConstant) / sizeof(uint32_t);
        transfer_root_host_address_constant.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
      }
      transfer_root_desc.NumParameters = rex::bit_count(transfer_root_mask);
      assert_true(transfer_root_desc.NumParameters <= kTransferUsedRootParameterCount);
      transfer_root_signatures_[i] =
          ui::d3d12::util::CreateRootSignature(provider, transfer_root_desc);
      if (!transfer_root_signatures_[i]) {
        REXGPU_ERROR(
            "D3D12RenderTargetCache: Failed to create the render target "
            "ownership transfer root signature {:X}",
            transfer_root_mask);
        Shutdown();
        return false;
      }
    }

    // Dumping root signatures.
    D3D12_DESCRIPTOR_RANGE dump_root_source_range;
    dump_root_source_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    dump_root_source_range.NumDescriptors = 1;
    dump_root_source_range.BaseShaderRegister = 0;
    dump_root_source_range.RegisterSpace = 0;
    dump_root_source_range.OffsetInDescriptorsFromTableStart = 0;
    D3D12_DESCRIPTOR_RANGE dump_root_stencil_range;
    dump_root_stencil_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    dump_root_stencil_range.NumDescriptors = 1;
    dump_root_stencil_range.BaseShaderRegister = 1;
    dump_root_stencil_range.RegisterSpace = 0;
    dump_root_stencil_range.OffsetInDescriptorsFromTableStart = 0;
    D3D12_DESCRIPTOR_RANGE dump_root_edram_range;
    dump_root_edram_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    dump_root_edram_range.NumDescriptors = 1;
    dump_root_edram_range.BaseShaderRegister = 0;
    dump_root_edram_range.RegisterSpace = 0;
    dump_root_edram_range.OffsetInDescriptorsFromTableStart = 0;
    D3D12_ROOT_PARAMETER
    dump_root_color_parameters[kDumpRootParameterColorCount];
    D3D12_ROOT_PARAMETER
    dump_root_depth_parameters[kDumpRootParameterDepthCount];
    for (uint32_t i = 0; i < 2; ++i) {
      // Offsets.
      D3D12_ROOT_PARAMETER& dump_root_offsets =
          i ? dump_root_depth_parameters[kDumpRootParameterOffsets]
            : dump_root_color_parameters[kDumpRootParameterOffsets];
      dump_root_offsets.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
      dump_root_offsets.Constants.ShaderRegister = kDumpCbufferOffsets;
      dump_root_offsets.Constants.RegisterSpace = 0;
      dump_root_offsets.Constants.Num32BitValues = sizeof(DumpOffsets) / sizeof(uint32_t);
      dump_root_offsets.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
      // Source.
      D3D12_ROOT_PARAMETER& dump_root_source =
          i ? dump_root_depth_parameters[kDumpRootParameterSource]
            : dump_root_color_parameters[kDumpRootParameterSource];
      dump_root_source.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
      dump_root_source.DescriptorTable.NumDescriptorRanges = 1;
      dump_root_source.DescriptorTable.pDescriptorRanges = &dump_root_source_range;
      dump_root_source.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
      // Stencil.
      if (i) {
        D3D12_ROOT_PARAMETER& dump_root_stencil =
            dump_root_depth_parameters[kDumpRootParameterDepthStencil];
        dump_root_stencil.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        dump_root_stencil.DescriptorTable.NumDescriptorRanges = 1;
        dump_root_stencil.DescriptorTable.pDescriptorRanges = &dump_root_stencil_range;
        dump_root_stencil.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
      }
      // Pitches.
      D3D12_ROOT_PARAMETER& dump_root_pitches =
          i ? dump_root_depth_parameters[kDumpRootParameterDepthPitches]
            : dump_root_color_parameters[kDumpRootParameterColorPitches];
      dump_root_pitches.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
      dump_root_pitches.Constants.ShaderRegister = kDumpCbufferPitches;
      dump_root_pitches.Constants.RegisterSpace = 0;
      dump_root_pitches.Constants.Num32BitValues = sizeof(DumpPitches) / sizeof(uint32_t);
      dump_root_pitches.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
      // EDRAM.
      D3D12_ROOT_PARAMETER& dump_root_edram =
          i ? dump_root_depth_parameters[kDumpRootParameterDepthEdram]
            : dump_root_color_parameters[kDumpRootParameterColorEdram];
      dump_root_edram.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
      dump_root_edram.DescriptorTable.NumDescriptorRanges = 1;
      dump_root_edram.DescriptorTable.pDescriptorRanges = &dump_root_edram_range;
      dump_root_edram.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    D3D12_ROOT_SIGNATURE_DESC dump_root_desc;
    dump_root_desc.NumParameters = UINT(rex::countof(dump_root_color_parameters));
    dump_root_desc.pParameters = dump_root_color_parameters;
    dump_root_desc.NumStaticSamplers = 0;
    dump_root_desc.pStaticSamplers = nullptr;
    dump_root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    dump_root_signature_color_ = ui::d3d12::util::CreateRootSignature(provider, dump_root_desc);
    if (!dump_root_signature_color_) {
      REXGPU_ERROR(
          "D3D12RenderTargetCache: Failed to create the color render target "
          "dumping root signature");
      Shutdown();
      return false;
    }
    dump_root_desc.NumParameters = UINT(rex::countof(dump_root_depth_parameters));
    dump_root_desc.pParameters = dump_root_depth_parameters;
    dump_root_signature_depth_ = ui::d3d12::util::CreateRootSignature(provider, dump_root_desc);
    if (!dump_root_signature_depth_) {
      REXGPU_ERROR(
          "D3D12RenderTargetCache: Failed to create the depth render target "
          "dumping root signature");
      Shutdown();
      return false;
    }

    // k_32_FLOAT and k_32_32_FLOAT clear root signature and pipelines.
    D3D12_ROOT_PARAMETER uint32_rtv_clear_root_constants;
    uint32_rtv_clear_root_constants.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    uint32_rtv_clear_root_constants.Constants.ShaderRegister = 0;
    uint32_rtv_clear_root_constants.Constants.RegisterSpace = 0;
    uint32_rtv_clear_root_constants.Constants.Num32BitValues = 2;
    uint32_rtv_clear_root_constants.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC uint32_rtv_clear_root_desc;
    uint32_rtv_clear_root_desc.NumParameters = 1;
    uint32_rtv_clear_root_desc.pParameters = &uint32_rtv_clear_root_constants;
    uint32_rtv_clear_root_desc.NumStaticSamplers = 0;
    uint32_rtv_clear_root_desc.pStaticSamplers = nullptr;
    uint32_rtv_clear_root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    uint32_rtv_clear_root_signature_ =
        ui::d3d12::util::CreateRootSignature(provider, uint32_rtv_clear_root_desc);
    if (!uint32_rtv_clear_root_signature_) {
      REXGPU_ERROR(
          "D3D12RenderTargetCache: Failed to create the k_32_FLOAT / "
          "k_32_32_FLOAT render target clearing root signature");
      Shutdown();
      return false;
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC uint32_rtv_clear_pipeline_desc = {};
    uint32_rtv_clear_pipeline_desc.pRootSignature = uint32_rtv_clear_root_signature_;
    uint32_rtv_clear_pipeline_desc.VS.pShaderBytecode = shaders::fullscreen_cw_vs;
    uint32_rtv_clear_pipeline_desc.VS.BytecodeLength = sizeof(shaders::fullscreen_cw_vs);
    uint32_rtv_clear_pipeline_desc.PS.pShaderBytecode = shaders::clear_uint2_ps;
    uint32_rtv_clear_pipeline_desc.PS.BytecodeLength = sizeof(shaders::clear_uint2_ps);
    uint32_rtv_clear_pipeline_desc.BlendState.RenderTarget[0].RenderTargetWriteMask =
        D3D12_COLOR_WRITE_ENABLE_ALL;
    uint32_rtv_clear_pipeline_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    uint32_rtv_clear_pipeline_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    uint32_rtv_clear_pipeline_desc.RasterizerState.DepthClipEnable = TRUE;
    uint32_rtv_clear_pipeline_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    uint32_rtv_clear_pipeline_desc.NumRenderTargets = 1;
    for (size_t i = 0; i < 2; ++i) {
      uint32_rtv_clear_pipeline_desc.RTVFormats[0] =
          GetColorOwnershipTransferDXGIFormat(i ? xenos::ColorRenderTargetFormat::k_32_32_FLOAT
                                                : xenos::ColorRenderTargetFormat::k_32_FLOAT);
      for (size_t j = size_t(xenos::MsaaSamples::k1X); j <= size_t(xenos::MsaaSamples::k4X); ++j) {
        if (xenos::MsaaSamples(j) == xenos::MsaaSamples::k2X && !msaa_2x_supported_) {
          // Using sample 0 as 0 and 3 as 1 for 2x instead.
          uint32_rtv_clear_pipeline_desc.SampleMask = 0b1001;
          uint32_rtv_clear_pipeline_desc.SampleDesc.Count = 4;
        } else {
          uint32_rtv_clear_pipeline_desc.SampleMask = UINT_MAX;
          uint32_rtv_clear_pipeline_desc.SampleDesc.Count = 1 << j;
        }
        ID3D12PipelineState* uint32_rtv_clear_pipeline;
        if (FAILED(device->CreateGraphicsPipelineState(&uint32_rtv_clear_pipeline_desc,
                                                       IID_PPV_ARGS(&uint32_rtv_clear_pipeline)))) {
          REXGPU_ERROR(
              "D3D12RenderTargetCache: Failed to create the {} {}-sample "
              "render target clearing pipeline",
              i ? "k_32_32_FLOAT" : "k_32_FLOAT", uint32_t(1) << j);
          Shutdown();
          return false;
        }
        uint32_rtv_clear_pipelines_[i][j] = uint32_rtv_clear_pipeline;
        auto uint32_rtv_clear_pipeline_name = rex::string::to_utf16(fmt::format(
            "Resolve Clear {} {}xMSAA", i ? "k_32_32_FLOAT" : "k_32_FLOAT", uint32_t(1) << j));
        uint32_rtv_clear_pipeline->SetName(
            reinterpret_cast<LPCWSTR>(uint32_rtv_clear_pipeline_name.c_str()));
      }
    }

    // FXC-compiled depth / stencil dumping shader is ~2 KB, reserve 4 KB for
    // some additional space.
    built_shader_.reserve(1024);
  } else if (path_ == Path::kPixelShaderInterlock) {
    // Pixel shader interlock (rasterizer-ordered view).

    // Blending is done in linear space directly in shaders.
    gamma_render_target_as_unorm16_ = false;

    // Always true float24 depth rounded to the nearest even.
    depth_float24_round_ = true;
    depth_float24_convert_in_pixel_shader_ = true;

    // Only ForcedSampleCount, which doesn't support 2x.
    msaa_2x_supported_ = false;

    // Create the resolve EDRAM buffer clearing root signature.
    std::array<D3D12_ROOT_PARAMETER, 2> resolve_rov_clear_root_parameters;
    // Parameter 0 is constants.
    resolve_rov_clear_root_parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    resolve_rov_clear_root_parameters[0].Constants.ShaderRegister = 0;
    resolve_rov_clear_root_parameters[0].Constants.RegisterSpace = 0;
    // Binding all of the shared memory at 1x resolution, portions with scaled
    // resolution.
    resolve_rov_clear_root_parameters[0].Constants.Num32BitValues =
        sizeof(draw_util::ResolveClearShaderConstants) / sizeof(uint32_t);
    resolve_rov_clear_root_parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    // Parameter 1 is the destination (EDRAM).
    D3D12_DESCRIPTOR_RANGE resolve_rov_clear_dest_range;
    resolve_rov_clear_dest_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    resolve_rov_clear_dest_range.NumDescriptors = 1;
    resolve_rov_clear_dest_range.BaseShaderRegister = 0;
    resolve_rov_clear_dest_range.RegisterSpace = 0;
    resolve_rov_clear_dest_range.OffsetInDescriptorsFromTableStart = 0;
    resolve_rov_clear_root_parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    resolve_rov_clear_root_parameters[1].DescriptorTable.NumDescriptorRanges = 1;
    resolve_rov_clear_root_parameters[1].DescriptorTable.pDescriptorRanges =
        &resolve_rov_clear_dest_range;
    resolve_rov_clear_root_parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC resolve_rov_clear_root_signature_desc;
    resolve_rov_clear_root_signature_desc.NumParameters =
        UINT(resolve_rov_clear_root_parameters.size());
    resolve_rov_clear_root_signature_desc.pParameters = resolve_rov_clear_root_parameters.data();
    resolve_rov_clear_root_signature_desc.NumStaticSamplers = 0;
    resolve_rov_clear_root_signature_desc.pStaticSamplers = nullptr;
    resolve_rov_clear_root_signature_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    resolve_rov_clear_root_signature_ =
        ui::d3d12::util::CreateRootSignature(provider, resolve_rov_clear_root_signature_desc);
    if (resolve_rov_clear_root_signature_ == nullptr) {
      REXGPU_ERROR(
          "D3D12RenderTargetCache: Failed to create the resolve EDRAM buffer "
          "clear root signature");
      Shutdown();
      return false;
    }

    // Create the resolve EDRAM buffer clearing pipelines.
    resolve_rov_clear_32bpp_pipeline_ = ui::d3d12::util::CreateComputePipeline(
        device,
        draw_resolution_scaled ? shaders::resolve_clear_32bpp_scaled_cs
                               : shaders::resolve_clear_32bpp_cs,
        draw_resolution_scaled ? sizeof(shaders::resolve_clear_32bpp_scaled_cs)
                               : sizeof(shaders::resolve_clear_32bpp_cs),
        resolve_rov_clear_root_signature_);
    if (resolve_rov_clear_32bpp_pipeline_ == nullptr) {
      REXGPU_ERROR(
          "D3D12RenderTargetCache: Failed to create the 32bpp resolve EDRAM "
          "buffer clear pipeline");
      Shutdown();
      return false;
    }
    resolve_rov_clear_32bpp_pipeline_->SetName(L"Resolve Clear 32bpp");
    resolve_rov_clear_64bpp_pipeline_ = ui::d3d12::util::CreateComputePipeline(
        device,
        draw_resolution_scaled ? shaders::resolve_clear_64bpp_scaled_cs
                               : shaders::resolve_clear_64bpp_cs,
        draw_resolution_scaled ? sizeof(shaders::resolve_clear_64bpp_scaled_cs)
                               : sizeof(shaders::resolve_clear_64bpp_cs),
        resolve_rov_clear_root_signature_);
    if (resolve_rov_clear_64bpp_pipeline_ == nullptr) {
      REXGPU_ERROR(
          "D3D12RenderTargetCache: Failed to create the 64bpp resolve EDRAM "
          "buffer clear pipeline");
      Shutdown();
      return false;
    }
    resolve_rov_clear_64bpp_pipeline_->SetName(L"Resolve Clear 64bpp");
  } else {
    assert_unhandled_case(path_);
    Shutdown();
    return false;
  }

  InitializeCommon();

  return true;
}

void D3D12RenderTargetCache::Shutdown(bool from_destructor) {
  native_depth_snapshots_.clear();
  ui::d3d12::util::ReleaseAndNull(resolve_rov_clear_64bpp_pipeline_);
  ui::d3d12::util::ReleaseAndNull(resolve_rov_clear_32bpp_pipeline_);
  ui::d3d12::util::ReleaseAndNull(resolve_rov_clear_root_signature_);

  for (size_t i = 0; i < 2; ++i) {
    for (size_t j = size_t(xenos::MsaaSamples::k1X); j <= size_t(xenos::MsaaSamples::k4X); ++j) {
      ui::d3d12::util::ReleaseAndNull(uint32_rtv_clear_pipelines_[i][j]);
    }
  }
  ui::d3d12::util::ReleaseAndNull(uint32_rtv_clear_root_signature_);

  for (const auto& dump_pipeline_pair : dump_pipelines_) {
    if (dump_pipeline_pair.second) {
      dump_pipeline_pair.second->Release();
    }
  }
  dump_pipelines_.clear();
  ui::d3d12::util::ReleaseAndNull(dump_root_signature_depth_);
  ui::d3d12::util::ReleaseAndNull(dump_root_signature_color_);

  for (const auto& transfer_pipeline_array_pair : transfer_stencil_bit_pipelines_) {
    for (ID3D12PipelineState* transfer_pipeline : transfer_pipeline_array_pair.second) {
      if (transfer_pipeline) {
        transfer_pipeline->Release();
      }
    }
  }
  transfer_stencil_bit_pipelines_.clear();
  for (const auto& transfer_pipeline_pair : transfer_pipelines_) {
    if (transfer_pipeline_pair.second) {
      transfer_pipeline_pair.second->Release();
    }
  }
  transfer_pipelines_.clear();
  for (size_t i = 0; i < rex::countof(transfer_root_signatures_); ++i) {
    ui::d3d12::util::ReleaseAndNull(transfer_root_signatures_[i]);
  }

  transfer_vertex_buffer_pool_.reset();

  for (size_t i = 0; i < rex::countof(host_depth_store_pipelines_); ++i) {
    ui::d3d12::util::ReleaseAndNull(host_depth_store_pipelines_[i]);
  }
  ui::d3d12::util::ReleaseAndNull(host_depth_store_root_signature_);

  null_rtv_descriptor_ms_.Free();
  null_rtv_descriptor_ss_.Free();
  descriptor_pool_srv_.reset();
  descriptor_pool_depth_.reset();
  descriptor_pool_color_.reset();

  for (size_t i = 0; i < rex::countof(resolve_copy_pipelines_); ++i) {
    ui::d3d12::util::ReleaseAndNull(resolve_copy_pipelines_[i]);
  }
  ui::d3d12::util::ReleaseAndNull(resolve_copy_root_signature_);
  for (auto& pipeline : native_color_copy_pipeline_)
    ui::d3d12::util::ReleaseAndNull(pipeline);
  ui::d3d12::util::ReleaseAndNull(native_color_copy_root_signature_);
  native_color_copy_attempted_ = {};
  for (auto& pipeline : native_depth_copy_pipeline_)
    ui::d3d12::util::ReleaseAndNull(pipeline);
  ui::d3d12::util::ReleaseAndNull(native_depth_copy_root_signature_);
  native_depth_copy_attempted_ = {};

  edram_snapshot_restore_pool_.reset();
  ui::d3d12::util::ReleaseAndNull(edram_snapshot_download_buffer_);

  ui::d3d12::util::ReleaseAndNull(edram_buffer_descriptor_heap_);
  ui::d3d12::util::ReleaseAndNull(edram_buffer_);

  if (!from_destructor) {
    ShutdownCommon();
  }
}

void D3D12RenderTargetCache::ClearCache() {
  // Cache clearing is performed with the GPU idle.
  native_depth_snapshots_.clear();
  RenderTargetCache::ClearCache();
}

void D3D12RenderTargetCache::CompletedSubmissionUpdated() {
  while (!native_depth_snapshots_.empty() &&
         native_depth_snapshots_.front().first <= command_processor_.GetCompletedSubmission())
    native_depth_snapshots_.pop_front();
  if (edram_snapshot_restore_pool_) {
    edram_snapshot_restore_pool_->Reclaim(command_processor_.GetCompletedSubmission());
  }
  if (transfer_vertex_buffer_pool_) {
    transfer_vertex_buffer_pool_->Reclaim(command_processor_.GetCompletedSubmission());
  }
}

void D3D12RenderTargetCache::BeginSubmission() {
  // New command list - render targets not bound.
  InvalidateCommandListRenderTargets();
  // ExecuteCommandLists is a full UAV barrier.
  if (edram_buffer_modification_status_ != EdramBufferModificationStatus::kUnmodified) {
    assert_true(edram_buffer_state_ == D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    edram_buffer_modification_status_ = EdramBufferModificationStatus::kUnmodified;
    PixelShaderInterlockFullEdramBarrierPlaced();
  }
}

bool D3D12RenderTargetCache::Update(bool is_rasterization_done,
                                    reg::RB_DEPTHCONTROL normalized_depth_control,
                                    uint32_t normalized_color_mask, const Shader& vertex_shader) {
  if (!RenderTargetCache::Update(is_rasterization_done, normalized_depth_control,
                                 normalized_color_mask, vertex_shader)) {
    return false;
  }
  switch (GetPath()) {
    case Path::kHostRenderTargets: {
      RenderTarget* const* depth_and_color_render_targets =
          last_update_accumulated_render_targets();
      PerformTransfersAndResolveClears(1 + xenos::kMaxColorRenderTargets,
                                       depth_and_color_render_targets, last_update_transfers());
      SetCommandListRenderTargets(depth_and_color_render_targets);
    } break;
    case Path::kPixelShaderInterlock: {
      // For ROV, only the barrier is needed - already scheduled if required.
      // But the buffer will be used for ROV drawing now.
      TransitionEdramBuffer(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      // Commit preceding UAV (but not ROV) writes like clears as they aren't
      // synchronized with ROV accesses.
      CommitEdramBufferUAVWrites(EdramBufferModificationStatus::kAsUAV);
      // TODO(Triang3l): Check if this draw call modifies color or depth /
      // stencil, at least coarsely, to prevent useless barriers.
      MarkEdramBufferModified(EdramBufferModificationStatus::kAsROV);
    } break;
    default:
      assert_unhandled_case(GetPath());
      return false;
  }
  return true;
}

bool D3D12RenderTargetCache::EnsureEdramBuffer() {
  if (edram_buffer_) {
    return true;
  }
  const auto& provider = command_processor_.GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();
  // Create the buffer for reinterpreting EDRAM contents.
  uint32_t edram_buffer_size =
      xenos::kEdramSizeBytes * (draw_resolution_scale_x() * draw_resolution_scale_y());
  D3D12_RESOURCE_DESC edram_buffer_desc;
  ui::d3d12::util::FillBufferResourceDesc(edram_buffer_desc, edram_buffer_size,
                                          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  // The first operation will likely be depth self-comparison with host render
  // targets or drawing with ROV.
  edram_buffer_state_ = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  // Creating zeroed for stable initial value with ROV (though on a real
  // console it has to be cleared anyway probably) and not to leak irrelevant
  // data to trace dumps when not covered by host render targets entirely.
  if (FAILED(device->CreateCommittedResource(
          &ui::d3d12::util::kHeapPropertiesDefault, D3D12_HEAP_FLAG_NONE, &edram_buffer_desc,
          edram_buffer_state_, nullptr, IID_PPV_ARGS(&edram_buffer_)))) {
    REXGPU_ERROR("D3D12RenderTargetCache: Failed to create the EDRAM buffer");
    return false;
  }
  edram_buffer_->SetName(L"EDRAM Buffer");
  command_processor_.native_gpu_diagnostics().RecordAllocation(
      NativeGpuDiagnostics::Allocation::kEdram, edram_buffer_size, edram_buffer_size);
  edram_buffer_modification_status_ = EdramBufferModificationStatus::kUnmodified;

  UpdateEdramBufferDescriptors();
  return true;
}

void D3D12RenderTargetCache::UpdateEdramBufferDescriptors() {
  const auto& provider = command_processor_.GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();
  uint32_t edram_buffer_size =
      xenos::kEdramSizeBytes * (draw_resolution_scale_x() * draw_resolution_scale_y());
  ui::d3d12::util::CreateBufferRawSRV(
      device,
      provider.OffsetViewDescriptor(edram_buffer_descriptor_heap_start_,
                                    uint32_t(EdramBufferDescriptorIndex::kRawSRV)),
      edram_buffer_, edram_buffer_size);
  ui::d3d12::util::CreateBufferTypedSRV(
      device,
      provider.OffsetViewDescriptor(edram_buffer_descriptor_heap_start_,
                                    uint32_t(EdramBufferDescriptorIndex::kR32UintSRV)),
      edram_buffer_, DXGI_FORMAT_R32_UINT, edram_buffer_size >> 2);
  ui::d3d12::util::CreateBufferTypedSRV(
      device,
      provider.OffsetViewDescriptor(edram_buffer_descriptor_heap_start_,
                                    uint32_t(EdramBufferDescriptorIndex::kR32G32UintSRV)),
      edram_buffer_, DXGI_FORMAT_R32G32_UINT, edram_buffer_size >> 3);
  ui::d3d12::util::CreateBufferTypedSRV(
      device,
      provider.OffsetViewDescriptor(edram_buffer_descriptor_heap_start_,
                                    uint32_t(EdramBufferDescriptorIndex::kR32G32B32A32UintSRV)),
      edram_buffer_, DXGI_FORMAT_R32G32B32A32_UINT, edram_buffer_size >> 4);
  ui::d3d12::util::CreateBufferRawUAV(
      device,
      provider.OffsetViewDescriptor(edram_buffer_descriptor_heap_start_,
                                    uint32_t(EdramBufferDescriptorIndex::kRawUAV)),
      edram_buffer_, edram_buffer_size);
  ui::d3d12::util::CreateBufferTypedUAV(
      device,
      provider.OffsetViewDescriptor(edram_buffer_descriptor_heap_start_,
                                    uint32_t(EdramBufferDescriptorIndex::kR32UintUAV)),
      edram_buffer_, DXGI_FORMAT_R32_UINT, edram_buffer_size >> 2);
  ui::d3d12::util::CreateBufferTypedUAV(
      device,
      provider.OffsetViewDescriptor(edram_buffer_descriptor_heap_start_,
                                    uint32_t(EdramBufferDescriptorIndex::kR32G32UintUAV)),
      edram_buffer_, DXGI_FORMAT_R32G32_UINT, edram_buffer_size >> 3);
  ui::d3d12::util::CreateBufferTypedUAV(
      device,
      provider.OffsetViewDescriptor(edram_buffer_descriptor_heap_start_,
                                    uint32_t(EdramBufferDescriptorIndex::kR32G32B32A32UintUAV)),
      edram_buffer_, DXGI_FORMAT_R32G32B32A32_UINT, edram_buffer_size >> 4);
}

void D3D12RenderTargetCache::WriteEdramRawSRVDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE handle) {
  const ui::d3d12::D3D12Provider& provider = command_processor_.GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();
  device->CopyDescriptorsSimple(
      1, handle,
      provider.OffsetViewDescriptor(edram_buffer_descriptor_heap_start_,
                                    uint32_t(EdramBufferDescriptorIndex::kRawSRV)),
      D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

void D3D12RenderTargetCache::WriteEdramRawUAVDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE handle) {
  const ui::d3d12::D3D12Provider& provider = command_processor_.GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();
  device->CopyDescriptorsSimple(
      1, handle,
      provider.OffsetViewDescriptor(edram_buffer_descriptor_heap_start_,
                                    uint32_t(EdramBufferDescriptorIndex::kRawUAV)),
      D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

void D3D12RenderTargetCache::WriteEdramUintPow2SRVDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE handle,
                                                             uint32_t element_size_bytes_pow2) {
  EdramBufferDescriptorIndex descriptor_index;
  switch (element_size_bytes_pow2) {
    case 2:
      descriptor_index = EdramBufferDescriptorIndex::kR32UintSRV;
      break;
    case 3:
      descriptor_index = EdramBufferDescriptorIndex::kR32G32UintSRV;
      break;
    case 4:
      descriptor_index = EdramBufferDescriptorIndex::kR32G32B32A32UintSRV;
      break;
    default:
      assert_unhandled_case(element_size_bytes_pow2);
      return;
  }
  const ui::d3d12::D3D12Provider& provider = command_processor_.GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();
  device->CopyDescriptorsSimple(1, handle,
                                provider.OffsetViewDescriptor(edram_buffer_descriptor_heap_start_,
                                                              uint32_t(descriptor_index)),
                                D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

void D3D12RenderTargetCache::WriteEdramUintPow2UAVDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE handle,
                                                             uint32_t element_size_bytes_pow2) {
  EdramBufferDescriptorIndex descriptor_index;
  switch (element_size_bytes_pow2) {
    case 2:
      descriptor_index = EdramBufferDescriptorIndex::kR32UintUAV;
      break;
    case 3:
      descriptor_index = EdramBufferDescriptorIndex::kR32G32UintUAV;
      break;
    case 4:
      descriptor_index = EdramBufferDescriptorIndex::kR32G32B32A32UintUAV;
      break;
    default:
      assert_unhandled_case(element_size_bytes_pow2);
      return;
  }
  const ui::d3d12::D3D12Provider& provider = command_processor_.GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();
  device->CopyDescriptorsSimple(1, handle,
                                provider.OffsetViewDescriptor(edram_buffer_descriptor_heap_start_,
                                                              uint32_t(descriptor_index)),
                                D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

bool D3D12RenderTargetCache::Resolve(const memory::Memory& memory, D3D12SharedMemory& shared_memory,
                                     D3D12TextureCache& texture_cache,
                                     uint32_t& written_address_out, uint32_t& written_length_out) {
  written_address_out = 0;
  written_length_out = 0;

  bool draw_resolution_scaled = IsDrawResolutionScaled();

  draw_util::ResolveInfo resolve_info;
  bool fixed_16_truncated_to_minus_1_to_1 = IsFixed16TruncatedToMinus1To1();
  if (!draw_util::GetResolveInfo(register_file(), memory, trace_writer_, draw_resolution_scale_x(),
                                 draw_resolution_scale_y(), fixed_16_truncated_to_minus_1_to_1,
                                 fixed_16_truncated_to_minus_1_to_1, resolve_info)) {
    return false;
  }

  const bool source_original_resolution =
      draw_resolution_scaled && GetPath() == Path::kHostRenderTargets &&
      resolve_info.IsCopyingDepth() &&
      IsOriginalResolutionRenderTarget(resolve_info.depth_original_base,
                                       resolve_info.depth_edram_info.pitch_tiles, true);
  if (source_original_resolution) {
    draw_resolution_scaled = false;
    if (!draw_util::GetResolveInfo(register_file(), memory, trace_writer_, 1, 1,
                                   fixed_16_truncated_to_minus_1_to_1,
                                   fixed_16_truncated_to_minus_1_to_1, resolve_info))
      return false;
  }

  // Nothing to copy/clear.
  if (!resolve_info.coordinate_info.width_div_8 || !resolve_info.height_div_8) {
    return true;
  }

  DeferredCommandList& command_list = command_processor_.GetDeferredCommandList();

  // Copying.
  bool copied = false;
  if (resolve_info.copy_dest_extent_length) {
    draw_util::ResolveCopyShaderConstants copy_shader_constants;
    uint32_t copy_group_count_x, copy_group_count_y;
    draw_util::ResolveCopyShaderIndex copy_shader =
        resolve_info.GetCopyShader(draw_resolution_scaled ? draw_resolution_scale_x() : 1u,
                                   draw_resolution_scaled ? draw_resolution_scale_y() : 1u,
                                   copy_shader_constants, copy_group_count_x, copy_group_count_y);
    assert_true(copy_group_count_x && copy_group_count_y);
    if (copy_shader != draw_util::ResolveCopyShaderIndex::kUnknown) {
      const draw_util::ResolveCopyShaderInfo& copy_shader_info =
          draw_util::resolve_copy_shader_info[size_t(copy_shader)];
      NativeResolveCopy native_copy;
      bool native_surface_copy = false, native_depth_copy = false;
      if (GetPath() == Path::kHostRenderTargets) {
        if (REXCVAR_GET(direct_host_resolve)) {
          PlanNativeResolveCopy(resolve_info, texture_cache, native_copy);
          native_surface_copy =
              native_copy.source &&
              GetOrCreateNativeResolvePipeline(
                  resolve_info.IsCopyingDepth(), native_copy.integer_source,
                  native_copy.source->key().msaa_samples != xenos::MsaaSamples::k1X);
          native_depth_copy = native_surface_copy && resolve_info.IsCopyingDepth();
        }
        // Keep the guest-memory copy coherent for consumers outside the native
        // texture path, including CPU readback and differently shaped aliases.
        uint32_t dump_base, dump_row_length_used, dump_rows, dump_pitch;
        resolve_info.GetCopyEdramTileSpan(dump_base, dump_row_length_used, dump_rows, dump_pitch);
        if (!native_surface_copy &&
            !DumpRenderTargets(dump_base, dump_row_length_used, dump_rows, dump_pitch)) {
          REXGPU_ERROR("D3D12RenderTargetCache: Failed to dump host render targets for resolve");
          return false;
        }
      }

      if (!native_surface_copy) {
        command_processor_.native_gpu_diagnostics().RecordDependency(
            NativeGpuDiagnostics::Dependency::kResolveFallback,
            resolve_info.copy_dest_extent_length,
            "D3D12 resolve uses encoded render target samples");
      }
      D3D12SharedMemory::NativeResolveHandle native_destination;
      if (native_surface_copy && !draw_resolution_scaled &&
          REXCVAR_GET(d3d12_native_resolve_memory)) {
        native_destination = shared_memory.PrepareNativeResolve(
            resolve_info.copy_dest_extent_start, resolve_info.copy_dest_extent_length);
      }

      // Make sure there is memory to write to.
      bool copy_dest_committed;
      if (draw_resolution_scaled) {
        // Committing starting with the beginning of the potentially written
        // extent, but making the buffer containing the base current as the
        // beginning of the bound buffer is the base.
        copy_dest_committed =
            texture_cache.EnsureScaledResolveMemoryCommitted(
                resolve_info.copy_dest_extent_start, resolve_info.copy_dest_extent_length) &&
            texture_cache.MakeScaledResolveRangeCurrent(resolve_info.copy_dest_base,
                                                        resolve_info.copy_dest_extent_start -
                                                            resolve_info.copy_dest_base +
                                                            resolve_info.copy_dest_extent_length);
      } else {
        copy_dest_committed =
            native_destination || shared_memory.RequestRange(resolve_info.copy_dest_extent_start,
                                                             resolve_info.copy_dest_extent_length);
      }
      if (copy_dest_committed) {
        // Write the descriptors and transition the resources.
        // Full shared memory without resolution scaling, range of the scaled
        // resolve buffer with scaling because only at least 128 * 2^20 R32
        // elements must be addressable
        // (D3D12_REQ_BUFFER_RESOURCE_TEXEL_COUNT_2_TO_EXP).
        ui::d3d12::util::DescriptorCpuGpuHandlePair descriptor_dest;
        ui::d3d12::util::DescriptorCpuGpuHandlePair descriptor_source;
        ui::d3d12::util::DescriptorCpuGpuHandlePair descriptor_stencil;
        ui::d3d12::util::DescriptorCpuGpuHandlePair descriptors[3];
        uint32_t dest_bpe_log2 = native_surface_copy ? 2 : copy_shader_info.dest_bpe_log2;
        bool resource_local_destination = draw_resolution_scaled || native_destination;
        uint32_t descriptor_count = bindless_resources_used_
                                        ? uint32_t(resource_local_destination) +
                                              uint32_t(native_surface_copy) +
                                              uint32_t(native_depth_copy)
                                        : 2 + uint32_t(native_depth_copy);
        if (command_processor_.RequestOneUseSingleViewDescriptors(descriptor_count, descriptors)) {
          if (bindless_resources_used_) {
            if (resource_local_destination) {
              descriptor_dest = descriptors[0];
            } else {
              descriptor_dest =
                  command_processor_.GetSharedMemoryUintPow2BindlessUAVHandlePair(dest_bpe_log2);
            }
            if (native_surface_copy) {
              descriptor_source = descriptors[uint32_t(resource_local_destination)];
              if (native_depth_copy) {
                descriptor_stencil = descriptors[uint32_t(resource_local_destination) + 1];
              }
            } else if (copy_shader_info.source_is_raw) {
              descriptor_source = command_processor_.GetSystemBindlessViewHandlePair(
                  D3D12CommandProcessor::SystemBindlessView::kEdramRawSRV);
            } else {
              descriptor_source = command_processor_.GetEdramUintPow2BindlessSRVHandlePair(
                  copy_shader_info.source_bpe_log2);
            }
          } else {
            descriptor_dest = descriptors[0];
            if (!draw_resolution_scaled && !native_destination) {
              shared_memory.WriteUintPow2UAVDescriptor(descriptor_dest.first, dest_bpe_log2);
            }
            descriptor_source = descriptors[1];
            if (native_depth_copy) {
              descriptor_stencil = descriptors[2];
            }
            if (native_surface_copy) {
              // Written below using the host image SRV.
            } else if (copy_shader_info.source_is_raw) {
              WriteEdramRawSRVDescriptor(descriptor_source.first);
            } else {
              WriteEdramUintPow2SRVDescriptor(descriptor_source.first,
                                              copy_shader_info.source_bpe_log2);
            }
          }
          if (draw_resolution_scaled) {
            texture_cache.CreateCurrentScaledResolveRangeUintPow2UAV(descriptor_dest.first,
                                                                     dest_bpe_log2);
            texture_cache.TransitionCurrentScaledResolveRange(
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
          } else if (native_destination) {
            shared_memory.WriteNativeResolveUAV(native_destination, descriptor_dest.first);
          } else {
            shared_memory.UseForWriting();
          }
          if (native_surface_copy) {
            command_processor_.GetD3D12Provider().GetDevice()->CopyDescriptorsSimple(
                1, descriptor_source.first, native_copy.source->descriptor_srv().GetHandle(),
                D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            if (native_depth_copy) {
              command_processor_.GetD3D12Provider().GetDevice()->CopyDescriptorsSimple(
                  1, descriptor_stencil.first,
                  native_copy.source->descriptor_srv_stencil().GetHandle(),
                  D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            }
            command_processor_.PushTransitionBarrier(
                native_copy.source->resource(),
                native_copy.source->SetResourceState(
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
          } else {
            TransitionEdramBuffer(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
          }

          // Submit the resolve.
          command_list.D3DSetComputeRootSignature(
              native_surface_copy ? (native_depth_copy ? native_depth_copy_root_signature_
                                                       : native_color_copy_root_signature_)
                                  : resolve_copy_root_signature_);
          command_list.D3DSetComputeRootDescriptorTable(2, descriptor_source.second);
          command_list.D3DSetComputeRootDescriptorTable(1, descriptor_dest.second);
          if (native_depth_copy) {
            command_list.D3DSetComputeRootDescriptorTable(3, descriptor_stencil.second);
          }
          if (native_surface_copy) {
            uint32_t width = uint32_t(resolve_info.coordinate_info.width_div_8) << 3;
            uint32_t height = resolve_info.height_div_8 << 3;
            auto samples = native_copy.source->key().msaa_samples;
            uint32_t sample_map =
                samples == xenos::MsaaSamples::k4X
                    ? 0xD8u
                    : draw_util::GetD3D10SampleIndexForGuest2xMSAA(0, msaa_2x_supported_) |
                          (draw_util::GetD3D10SampleIndexForGuest2xMSAA(1, msaa_2x_supported_)
                           << 2);
            uint32_t values[20] = {
                width,
                height,
                uint32_t(resolve_info.copy_dest_coordinate_info.pitch_aligned_div_32) << 5,
                draw_resolution_scaled
                    ? (resolve_info.copy_dest_base_raw - resolve_info.copy_dest_base) *
                          native_copy.scale_x * native_copy.scale_y
                    : resolve_info.copy_dest_base_raw -
                          (native_destination ? native_destination->address : 0),
                native_copy.scale_x,
                native_copy.scale_y,
                native_depth_copy ? uint32_t(native_copy.source->key().GetDepthFormat() ==
                                             xenos::DepthRenderTargetFormat::kD24FS8)
                                  : native_copy.color_packing,
                uint32_t(resolve_info.copy_dest_info.copy_dest_endian),
                native_depth_copy
                    ? uint32_t(!depth_float24_convert_in_pixel_shader() && depth_float24_round())
                    : uint32_t(native_copy.red_blue_swapped),
                uint32_t(!draw_resolution_scaled),
                resolve_info.rect_x0,
                resolve_info.rect_y0,
                uint32_t(resolve_info.copy_dest_coordinate_info.height_aligned_div_32) << 5,
                uint32_t(resolve_info.copy_dest_info.copy_dest_slice),
                uint32_t(resolve_info.copy_dest_info.copy_dest_array),
                uint32_t(127 + int32_t(resolve_info.copy_dest_info.copy_dest_exp_bias)) << 23,
                uint32_t(resolve_info.copy_dest_coordinate_info.copy_sample_select),
                sample_map,
                native_depth_copy
                    ? 0u
                    : uint32_t(draw_util::GetNativeColorResolvePacking(
                          xenos::ColorFormat(resolve_info.copy_dest_info.copy_dest_format))),
                native_copy.source_layout};
            command_list.D3DSetComputeRoot32BitConstants(0, 20, values, 0);
            copy_group_count_x =
                (width * (draw_resolution_scaled ? native_copy.scale_x : 1) + 7) / 8;
            copy_group_count_y =
                (height * (draw_resolution_scaled ? native_copy.scale_y : 1) + 7) / 8;
          } else if (draw_resolution_scaled) {
            command_list.D3DSetComputeRoot32BitConstants(
                0, sizeof(copy_shader_constants.dest_relative) / sizeof(uint32_t),
                &copy_shader_constants.dest_relative, 0);
          } else {
            command_list.D3DSetComputeRoot32BitConstants(
                0, sizeof(copy_shader_constants) / sizeof(uint32_t), &copy_shader_constants, 0);
          }
          command_processor_.SetExternalPipeline(
              native_surface_copy
                  ? (native_depth_copy
                         ? native_depth_copy_pipeline_[native_copy.source->key().msaa_samples !=
                                                       xenos::MsaaSamples::k1X]
                         : native_color_copy_pipeline_[uint32_t(native_copy.integer_source) |
                                                       (uint32_t(native_copy.source->key()
                                                                     .msaa_samples !=
                                                                 xenos::MsaaSamples::k1X)
                                                        << 1)])
                  : resolve_copy_pipelines_[size_t(copy_shader)]);
          command_processor_.SubmitBarriers();
          command_list.D3DDispatch(copy_group_count_x, copy_group_count_y, 1);
          if (native_surface_copy) {
            command_processor_.native_gpu_diagnostics().RecordOperation(
                NativeGpuDiagnostics::Operation::kResolve);
            if (++native_color_pack_count_ <= 8 || !(native_color_pack_count_ & 4095)) {
              REXGPU_INFO("[d3d12-native-resolve] {} attachment resolves packed from host images",
                          native_color_pack_count_);
            }
          }

          // Order the resolve with other work using the destination as a UAV.
          if (draw_resolution_scaled) {
            texture_cache.MarkCurrentScaledResolveRangeUAVWritesCommitNeeded();
          } else if (native_destination) {
            shared_memory.MarkNativeResolveUAVWrites(native_destination);
          } else {
            shared_memory.MarkUAVWritesCommitNeeded();
          }

          // Invalidate textures and mark the range as scaled if needed.
          if (native_destination)
            shared_memory.BeginNativeResolveWrite(native_destination);
          texture_cache.MarkRangeAsResolved(resolve_info.copy_dest_extent_start,
                                            resolve_info.copy_dest_extent_length,
                                            source_original_resolution);
          if (native_destination && !shared_memory.EndNativeResolveWrite())
            return false;
          PerformNativeResolveCopy(native_copy, texture_cache);
          written_address_out = resolve_info.copy_dest_extent_start;
          written_length_out = resolve_info.copy_dest_extent_length;
          copied = true;
        }
      } else {
        REXGPU_ERROR(
            "D3D12RenderTargetCache: Failed to obtain the resolve destination "
            "memory region");
      }
    }
  } else {
    copied = true;
  }

  // Clearing.
  bool cleared = false;
  bool clear_depth = resolve_info.IsClearingDepth();
  bool clear_color = resolve_info.IsClearingColor();
  if (clear_depth || clear_color) {
    switch (GetPath()) {
      case Path::kHostRenderTargets: {
        Transfer::Rectangle clear_rectangle;
        RenderTarget* clear_render_targets[2];
        // If PrepareHostRenderTargetsResolveClear returns false, may be just an
        // empty region (success) or an error - don't care.
        if (PrepareHostRenderTargetsResolveClear(resolve_info, clear_rectangle,
                                                 clear_render_targets[0], clear_transfers_[0],
                                                 clear_render_targets[1], clear_transfers_[1])) {
          uint64_t clear_values[2];
          clear_values[0] = resolve_info.rb_depth_clear;
          clear_values[1] =
              resolve_info.rb_color_clear | (uint64_t(resolve_info.rb_color_clear_lo) << 32);
          PerformTransfersAndResolveClears(2, clear_render_targets, clear_transfers_, clear_values,
                                           &clear_rectangle);
        }
        cleared = true;
      } break;
      case Path::kPixelShaderInterlock: {
        ui::d3d12::util::DescriptorCpuGpuHandlePair descriptor_edram;
        bool descriptor_edram_obtained;
        if (bindless_resources_used_) {
          descriptor_edram = command_processor_.GetSystemBindlessViewHandlePair(
              D3D12CommandProcessor::SystemBindlessView ::kEdramR32G32B32A32UintUAV);
          descriptor_edram_obtained = true;
        } else {
          descriptor_edram_obtained =
              command_processor_.RequestOneUseSingleViewDescriptors(1, &descriptor_edram);
          if (descriptor_edram_obtained) {
            WriteEdramUintPow2UAVDescriptor(descriptor_edram.first, 4);
          }
        }
        if (descriptor_edram_obtained) {
          TransitionEdramBuffer(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
          // Should be safe to only commit once (if was UAV / ROV previously -
          // if there was nothing to copy, only to clear, for some reason, for
          // instance), overlap of the depth and the color ranges is highly
          // unlikely.
          CommitEdramBufferUAVWrites();
          command_list.D3DSetComputeRootSignature(resolve_rov_clear_root_signature_);
          command_list.D3DSetComputeRootDescriptorTable(1, descriptor_edram.second);
          std::pair<uint32_t, uint32_t> clear_group_count = resolve_info.GetClearShaderGroupCount(
              draw_resolution_scale_x(), draw_resolution_scale_y());
          assert_true(clear_group_count.first && clear_group_count.second);
          if (clear_depth) {
            draw_util::ResolveClearShaderConstants depth_clear_constants;
            resolve_info.GetDepthClearShaderConstants(depth_clear_constants);
            command_list.D3DSetComputeRoot32BitConstants(
                0, sizeof(depth_clear_constants) / sizeof(uint32_t), &depth_clear_constants, 0);
            command_processor_.SetExternalPipeline(resolve_rov_clear_32bpp_pipeline_);
            command_processor_.SubmitBarriers();
            command_list.D3DDispatch(clear_group_count.first, clear_group_count.second, 1);
          }
          if (clear_color) {
            draw_util::ResolveClearShaderConstants color_clear_constants;
            resolve_info.GetColorClearShaderConstants(color_clear_constants);
            if (clear_depth) {
              // Non-RT-specific constants have already been set.
              command_list.D3DSetComputeRoot32BitConstants(
                  0, sizeof(color_clear_constants.rt_specific) / sizeof(uint32_t),
                  &color_clear_constants.rt_specific,
                  offsetof(draw_util::ResolveClearShaderConstants, rt_specific) / sizeof(uint32_t));
            } else {
              command_list.D3DSetComputeRoot32BitConstants(
                  0, sizeof(color_clear_constants) / sizeof(uint32_t), &color_clear_constants, 0);
            }
            command_processor_.SetExternalPipeline(resolve_info.color_edram_info.format_is_64bpp
                                                       ? resolve_rov_clear_64bpp_pipeline_
                                                       : resolve_rov_clear_32bpp_pipeline_);
            command_processor_.SubmitBarriers();
            command_list.D3DDispatch(clear_group_count.first, clear_group_count.second, 1);
          }
          MarkEdramBufferModified();
          cleared = true;
        }
      } break;
      default:
        assert_unhandled_case(GetPath());
    }
  } else {
    cleared = true;
  }

  return copied && cleared;
}

bool D3D12RenderTargetCache::InitializeTraceSubmitDownloads() {
  if (IsDrawResolutionScaled()) {
    // No 1:1 mapping.
    return false;
  }
  if (!edram_snapshot_download_buffer_) {
    D3D12_RESOURCE_DESC edram_snapshot_download_buffer_desc;
    ui::d3d12::util::FillBufferResourceDesc(edram_snapshot_download_buffer_desc,
                                            xenos::kEdramSizeBytes, D3D12_RESOURCE_FLAG_NONE);
    const ui::d3d12::D3D12Provider& provider = command_processor_.GetD3D12Provider();
    ID3D12Device* device = provider.GetDevice();
    if (FAILED(device->CreateCommittedResource(
            &ui::d3d12::util::kHeapPropertiesReadback, provider.GetHeapFlagCreateNotZeroed(),
            &edram_snapshot_download_buffer_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&edram_snapshot_download_buffer_)))) {
      REXGPU_ERROR(
          "D3D12RenderTargetCache: Failed to create a EDRAM snapshot download "
          "buffer");
      return false;
    }
  }
  if (GetPath() == Path::kHostRenderTargets) {
    // Dump all host render targets to edram_buffer_.
    if (!DumpRenderTargets(0, xenos::kEdramTileCount, 1, xenos::kEdramTileCount)) {
      REXGPU_ERROR("D3D12RenderTargetCache: Failed to dump host render targets for trace");
      return false;
    }
  }
  TransitionEdramBuffer(D3D12_RESOURCE_STATE_COPY_SOURCE);
  command_processor_.SubmitBarriers();
  command_processor_.GetDeferredCommandList().D3DCopyBufferRegion(
      edram_snapshot_download_buffer_, 0, edram_buffer_, 0, xenos::kEdramSizeBytes);
  return true;
}

void D3D12RenderTargetCache::InitializeTraceCompleteDownloads() {
  if (!edram_snapshot_download_buffer_) {
    return;
  }
  void* download_mapping;
  if (SUCCEEDED(edram_snapshot_download_buffer_->Map(0, nullptr, &download_mapping))) {
    trace_writer_.WriteEdramSnapshot(download_mapping);
    D3D12_RANGE download_write_range = {};
    edram_snapshot_download_buffer_->Unmap(0, &download_write_range);
  } else {
    REXGPU_ERROR(
        "D3D12RenderTargetCache: Failed to map the EDRAM snapshot download "
        "buffer");
  }
  edram_snapshot_download_buffer_->Release();
  edram_snapshot_download_buffer_ = nullptr;
}

void D3D12RenderTargetCache::RestoreEdramSnapshot(const void* snapshot) {
  if (IsDrawResolutionScaled()) {
    // No 1:1 mapping.
    return;
  }

  // Create the buffer - will be used for copying to either a 32-bit 1280x2048
  // render target or the EDRAM buffer.
  const ui::d3d12::D3D12Provider& provider = command_processor_.GetD3D12Provider();
  if (!edram_snapshot_restore_pool_) {
    edram_snapshot_restore_pool_ =
        std::make_unique<ui::d3d12::D3D12UploadBufferPool>(provider, xenos::kEdramSizeBytes);
  }
  ID3D12Resource* upload_buffer;
  size_t upload_buffer_offset;
  void* upload_buffer_mapping = edram_snapshot_restore_pool_->Request(
      command_processor_.GetCurrentSubmission(), xenos::kEdramSizeBytes, 1, &upload_buffer,
      &upload_buffer_offset, nullptr);
  if (!upload_buffer_mapping) {
    REXGPU_ERROR(
        "D3D12RenderTargetCache: Failed to get a buffer for restoring a EDRAM "
        "snapshot");
    return;
  }

  DeferredCommandList& command_list = command_processor_.GetDeferredCommandList();

  switch (GetPath()) {
    case Path::kHostRenderTargets: {
      // k_32_FLOAT because it's unambiguous (not effected by something like
      // DXGI_FORMAT_R8G8B8A8 vs. DXGI_FORMAT_B8G8R8A8).
      D3D12RenderTarget* full_edram_render_target =
          static_cast<D3D12RenderTarget*>(PrepareFullEdram1280xRenderTargetForSnapshotRestoration(
              xenos::ColorRenderTargetFormat::k_32_FLOAT));
      if (!full_edram_render_target) {
        return;
      }
      D3D12_TEXTURE_COPY_LOCATION copy_source_location;
      copy_source_location.pResource = upload_buffer;
      copy_source_location.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      UINT64 copy_total_bytes;
      D3D12_RESOURCE_DESC full_edram_render_target_desc =
          full_edram_render_target->resource()->GetDesc();
      provider.GetDevice()->GetCopyableFootprints(&full_edram_render_target_desc, 0, 1, 0,
                                                  &copy_source_location.PlacedFootprint, nullptr,
                                                  nullptr, &copy_total_bytes);
      // 1280 width * sizeof(uint32_t) is aligned to
      // D3D12_TEXTURE_DATA_PITCH_ALIGNMENT (256).
      assert_true(copy_total_bytes <= xenos::kEdramSizeBytes);
      assert_false(full_edram_render_target->key().Is64bpp());
      uint32_t pitch_tiles = full_edram_render_target->key().pitch_tiles_at_32bpp;
      uint32_t tile_rows = xenos::kEdramTileCount / pitch_tiles;
      assert_true(pitch_tiles * tile_rows == xenos::kEdramTileCount);
      const uint8_t* snapshot_sample_row = reinterpret_cast<const uint8_t*>(snapshot);
      for (uint32_t y_tile = 0; y_tile < tile_rows; ++y_tile) {
        uint8_t* upload_buffer_tile_row_origin =
            reinterpret_cast<uint8_t*>(upload_buffer_mapping) +
            copy_source_location.PlacedFootprint.Offset +
            xenos::kEdramTileHeightSamples * y_tile *
                copy_source_location.PlacedFootprint.Footprint.RowPitch;
        for (uint32_t x_tile = 0; x_tile < pitch_tiles; ++x_tile) {
          uint8_t* upload_buffer_sample_row =
              upload_buffer_tile_row_origin +
              sizeof(uint32_t) * xenos::kEdramTileWidthSamples * x_tile;
          for (uint32_t sample_row = 0; sample_row < xenos::kEdramTileHeightSamples; ++sample_row) {
            std::memcpy(upload_buffer_sample_row, snapshot_sample_row,
                        sizeof(uint32_t) * xenos::kEdramTileWidthSamples);
            snapshot_sample_row += sizeof(uint32_t) * xenos::kEdramTileWidthSamples;
            upload_buffer_sample_row += copy_source_location.PlacedFootprint.Footprint.RowPitch;
          }
        }
      }
      command_processor_.PushTransitionBarrier(
          full_edram_render_target->resource(),
          full_edram_render_target->SetResourceState(D3D12_RESOURCE_STATE_COPY_DEST),
          D3D12_RESOURCE_STATE_COPY_DEST);
      command_processor_.SubmitBarriers();
      D3D12_TEXTURE_COPY_LOCATION copy_dest_location;
      copy_dest_location.pResource = full_edram_render_target->resource();
      copy_dest_location.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      copy_dest_location.SubresourceIndex = 0;
      command_list.D3DCopyTextureRegion(&copy_dest_location, 0, 0, 0, &copy_source_location,
                                        nullptr);
    } break;

    case Path::kPixelShaderInterlock: {
      std::memcpy(upload_buffer_mapping, snapshot, xenos::kEdramSizeBytes);
      TransitionEdramBuffer(D3D12_RESOURCE_STATE_COPY_DEST);
      command_processor_.SubmitBarriers();
      command_list.D3DCopyBufferRegion(edram_buffer_, 0, upload_buffer,
                                       UINT64(upload_buffer_offset), xenos::kEdramSizeBytes);
    } break;

    default:
      assert_unhandled_case(GetPath());
  }
}

DXGI_FORMAT D3D12RenderTargetCache::GetColorResourceDXGIFormat(
    xenos::ColorRenderTargetFormat format) const {
  // Typed should be preferred over typeless so there are more opportunities for
  // compression.
  switch (format) {
    case xenos::ColorRenderTargetFormat::k_8_8_8_8:
      return DXGI_FORMAT_R8G8B8A8_UNORM;
    case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA:
      return gamma_render_target_as_unorm16_ ? DXGI_FORMAT_R16G16B16A16_UNORM
                                             : DXGI_FORMAT_R8G8B8A8_UNORM;
    case xenos::ColorRenderTargetFormat::k_2_10_10_10:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10:
      return DXGI_FORMAT_R10G10B10A2_UNORM;
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16:
      return DXGI_FORMAT_R16G16B16A16_FLOAT;
    // SNORM has two representations of -1.
    case xenos::ColorRenderTargetFormat::k_16_16:
      return DXGI_FORMAT_R16G16_TYPELESS;
    case xenos::ColorRenderTargetFormat::k_16_16_16_16:
      return DXGI_FORMAT_R16G16B16A16_TYPELESS;
    // Floating-point - ensure NaN propagation during ownership transfer for
    // unmodified data.
    case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
      return DXGI_FORMAT_R16G16_TYPELESS;
    case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT:
      return DXGI_FORMAT_R16G16B16A16_TYPELESS;
    // TODO(Triang3l): Check if NaN propagation defined in the D3D11.3
    // specification can be relied on for 32-bit float render targets.
    case xenos::ColorRenderTargetFormat::k_32_FLOAT:
      return DXGI_FORMAT_R32_TYPELESS;
    case xenos::ColorRenderTargetFormat::k_32_32_FLOAT:
      return DXGI_FORMAT_R32G32_TYPELESS;
    default:
      assert_unhandled_case(format);
      return DXGI_FORMAT_UNKNOWN;
  }
}

DXGI_FORMAT D3D12RenderTargetCache::GetColorDrawDXGIFormat(
    xenos::ColorRenderTargetFormat format) const {
  switch (format) {
    case xenos::ColorRenderTargetFormat::k_16_16:
      return DXGI_FORMAT_R16G16_SNORM;
    case xenos::ColorRenderTargetFormat::k_16_16_16_16:
      return DXGI_FORMAT_R16G16B16A16_SNORM;
    case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
      return DXGI_FORMAT_R16G16_FLOAT;
    case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT:
      return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case xenos::ColorRenderTargetFormat::k_32_FLOAT:
      return DXGI_FORMAT_R32_FLOAT;
    case xenos::ColorRenderTargetFormat::k_32_32_FLOAT:
      return DXGI_FORMAT_R32G32_FLOAT;
    default:
      return GetColorResourceDXGIFormat(format);
  }
}

DXGI_FORMAT D3D12RenderTargetCache::GetColorOwnershipTransferDXGIFormat(
    xenos::ColorRenderTargetFormat format, bool* is_integer_out) const {
  if (is_integer_out) {
    *is_integer_out = true;
  }
  switch (format) {
    case xenos::ColorRenderTargetFormat::k_16_16:
    case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
      return DXGI_FORMAT_R16G16_UINT;
    case xenos::ColorRenderTargetFormat::k_16_16_16_16:
    case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT:
      return DXGI_FORMAT_R16G16B16A16_UINT;
    case xenos::ColorRenderTargetFormat::k_32_FLOAT:
      return DXGI_FORMAT_R32_UINT;
    case xenos::ColorRenderTargetFormat::k_32_32_FLOAT:
      return DXGI_FORMAT_R32G32_UINT;
    default:
      if (is_integer_out) {
        *is_integer_out = false;
      }
      return GetColorDrawDXGIFormat(format);
  }
}

DXGI_FORMAT D3D12RenderTargetCache::GetDepthResourceDXGIFormat(
    xenos::DepthRenderTargetFormat format) {
  switch (format) {
    case xenos::DepthRenderTargetFormat::kD24S8:
      return DXGI_FORMAT_R24G8_TYPELESS;
    case xenos::DepthRenderTargetFormat::kD24FS8:
      return DXGI_FORMAT_R32G8X24_TYPELESS;
    default:
      assert_unhandled_case(format);
      return DXGI_FORMAT_UNKNOWN;
  }
}

DXGI_FORMAT D3D12RenderTargetCache::GetDepthDSVDXGIFormat(xenos::DepthRenderTargetFormat format) {
  switch (format) {
    case xenos::DepthRenderTargetFormat::kD24S8:
      return DXGI_FORMAT_D24_UNORM_S8_UINT;
    case xenos::DepthRenderTargetFormat::kD24FS8:
      return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    default:
      assert_unhandled_case(format);
      return DXGI_FORMAT_UNKNOWN;
  }
}

DXGI_FORMAT D3D12RenderTargetCache::GetDepthSRVDepthDXGIFormat(
    xenos::DepthRenderTargetFormat format) {
  switch (format) {
    case xenos::DepthRenderTargetFormat::kD24S8:
      return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case xenos::DepthRenderTargetFormat::kD24FS8:
      return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    default:
      assert_unhandled_case(format);
      return DXGI_FORMAT_UNKNOWN;
  }
}

DXGI_FORMAT D3D12RenderTargetCache::GetDepthSRVStencilDXGIFormat(
    xenos::DepthRenderTargetFormat format) {
  switch (format) {
    case xenos::DepthRenderTargetFormat::kD24S8:
      return DXGI_FORMAT_X24_TYPELESS_G8_UINT;
    case xenos::DepthRenderTargetFormat::kD24FS8:
      return DXGI_FORMAT_X32_TYPELESS_G8X24_UINT;
    default:
      assert_unhandled_case(format);
      return DXGI_FORMAT_UNKNOWN;
  }
}

RenderTargetCache::RenderTarget* D3D12RenderTargetCache::CreateRenderTarget(RenderTargetKey key) {
  ID3D12Device* device = command_processor_.GetD3D12Provider().GetDevice();

  D3D12_RESOURCE_DESC resource_desc;
  resource_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  resource_desc.Alignment = 0;
  resource_desc.Width = key.GetWidth() * GetRenderTargetScaleX(key);
  resource_desc.Height = GetRenderTargetHeight(key.pitch_tiles_at_32bpp, key.msaa_samples,
                                               GetRenderTargetScaleY(key)) *
                         GetRenderTargetScaleY(key);
  resource_desc.DepthOrArraySize = 1;
  resource_desc.MipLevels = 1;
  if (key.is_depth) {
    resource_desc.Format = GetDepthResourceDXGIFormat(key.GetDepthFormat());
  } else {
    resource_desc.Format = GetColorResourceDXGIFormat(key.GetColorFormat());
  }
  assert_true(resource_desc.Format != DXGI_FORMAT_UNKNOWN);
  if (resource_desc.Format == DXGI_FORMAT_UNKNOWN) {
    REXGPU_ERROR("D3D12RenderTargetCache: Unknown {} render target format {}",
                 key.is_depth ? "depth" : "color", uint32_t(key.resource_format));
    return nullptr;
  }
  if (key.msaa_samples == xenos::MsaaSamples::k2X && !msaa_2x_supported()) {
    resource_desc.SampleDesc.Count = 4;
  } else {
    resource_desc.SampleDesc.Count = UINT(1) << UINT(key.msaa_samples);
  }
  resource_desc.SampleDesc.Quality = 0;
  resource_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  resource_desc.Flags = key.is_depth ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL
                                     : D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  // The first access will be ownership transfer into this render target or
  // starting to draw directly.
  D3D12_RESOURCE_STATES resource_state =
      key.is_depth ? D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_RENDER_TARGET;
  D3D12_CLEAR_VALUE optimized_clear_value;
  if (key.is_depth) {
    optimized_clear_value.Format = GetDepthDSVDXGIFormat(key.GetDepthFormat());
    // Fixed-point depth is generally direct (1 being the farthest),
    // floating-point is used for more uniform precision across the range (0
    // being the farthest).
    optimized_clear_value.DepthStencil.Depth =
        key.GetDepthFormat() == xenos::DepthRenderTargetFormat::kD24S8 ? 1.0f : 0.0f;
    optimized_clear_value.DepthStencil.Stencil = 0;
  } else {
    optimized_clear_value.Format = GetColorDrawDXGIFormat(key.GetColorFormat());
    optimized_clear_value.Color[0] = 0.0f;
    optimized_clear_value.Color[1] = 0.0f;
    optimized_clear_value.Color[2] = 0.0f;
    optimized_clear_value.Color[3] = 0.0f;
  }
  // Create zeroed for more determinism, primarily with respect to compression
  // and depth float24 / float32 mirroring.
  Microsoft::WRL::ComPtr<ID3D12Resource> resource;
  if (FAILED(device->CreateCommittedResource(&ui::d3d12::util::kHeapPropertiesDefault,
                                             D3D12_HEAP_FLAG_NONE, &resource_desc, resource_state,
                                             &optimized_clear_value, IID_PPV_ARGS(&resource)))) {
    return nullptr;
  }
  {
    std::u16string resource_name = rex::string::to_utf16(key.GetDebugName());
    resource->SetName(reinterpret_cast<LPCWSTR>(resource_name.c_str()));
  }

  ui::d3d12::D3D12CpuDescriptorPool& descriptor_pool =
      key.is_depth ? *descriptor_pool_depth_ : *descriptor_pool_color_;
  ui::d3d12::D3D12CpuDescriptorPool::Descriptor descriptor_draw =
      descriptor_pool.AllocateDescriptor();
  ui::d3d12::D3D12CpuDescriptorPool::Descriptor descriptor_srv =
      descriptor_pool_srv_->AllocateDescriptor();
  if (!descriptor_draw.IsValid() || !descriptor_srv.IsValid()) {
    return nullptr;
  }
  D3D12_CPU_DESCRIPTOR_HANDLE descriptor_draw_handle = descriptor_draw.GetHandle();
  ui::d3d12::D3D12CpuDescriptorPool::Descriptor descriptor_load_separate;
  ui::d3d12::D3D12CpuDescriptorPool::Descriptor descriptor_srv_stencil;
  D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc;
  srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  if (resource_desc.SampleDesc.Count > 1) {
    srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
  } else {
    srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Texture2D.MostDetailedMip = 0;
    srv_desc.Texture2D.MipLevels = 1;
    srv_desc.Texture2D.PlaneSlice = 0;
    srv_desc.Texture2D.ResourceMinLODClamp = 0.0f;
  }
  if (key.is_depth) {
    // DSV and stencil SRV.
    descriptor_srv_stencil = descriptor_pool_srv_->AllocateDescriptor();
    if (!descriptor_srv_stencil.IsValid()) {
      return nullptr;
    }
    D3D12_DEPTH_STENCIL_VIEW_DESC dsv_desc;
    dsv_desc.Format = optimized_clear_value.Format;
    dsv_desc.Flags = D3D12_DSV_FLAG_NONE;
    D3D12_SHADER_RESOURCE_VIEW_DESC stencil_srv_desc;
    stencil_srv_desc.Format = GetDepthSRVStencilDXGIFormat(key.GetDepthFormat());
    stencil_srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    if (resource_desc.SampleDesc.Count > 1) {
      dsv_desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMS;
      stencil_srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
    } else {
      dsv_desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
      dsv_desc.Texture2D.MipSlice = 0;
      stencil_srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
      stencil_srv_desc.Texture2D.MostDetailedMip = 0;
      stencil_srv_desc.Texture2D.MipLevels = 1;
      stencil_srv_desc.Texture2D.PlaneSlice = 1;
      stencil_srv_desc.Texture2D.ResourceMinLODClamp = 0.0f;
    }
    device->CreateDepthStencilView(resource.Get(), &dsv_desc, descriptor_draw_handle);
    device->CreateShaderResourceView(resource.Get(), &stencil_srv_desc,
                                     descriptor_srv_stencil.GetHandle());
    // Depth SRV.
    srv_desc.Format = GetDepthSRVDepthDXGIFormat(key.GetDepthFormat());
  } else {
    // Drawing RTV.
    D3D12_RENDER_TARGET_VIEW_DESC rtv_desc;
    rtv_desc.Format = optimized_clear_value.Format;
    if (resource_desc.SampleDesc.Count > 1) {
      rtv_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMS;
    } else {
      rtv_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
      rtv_desc.Texture2D.MipSlice = 0;
      rtv_desc.Texture2D.PlaneSlice = 0;
    }
    device->CreateRenderTargetView(resource.Get(), &rtv_desc, descriptor_draw_handle);
    // Ownership transfer RTV.
    DXGI_FORMAT load_format = GetColorOwnershipTransferDXGIFormat(key.GetColorFormat());
    if (rtv_desc.Format != load_format) {
      descriptor_load_separate = descriptor_pool.AllocateDescriptor();
      if (!descriptor_load_separate.IsValid()) {
        return nullptr;
      }
      rtv_desc.Format = load_format;
      device->CreateRenderTargetView(resource.Get(), &rtv_desc,
                                     descriptor_load_separate.GetHandle());
    }
    // SRV for ownership transfer and dumping.
    srv_desc.Format = load_format;
  }
  device->CreateShaderResourceView(resource.Get(), &srv_desc, descriptor_srv.GetHandle());

  return new D3D12RenderTarget(key, resource.Get(), std::move(descriptor_draw),
                               std::move(descriptor_load_separate), std::move(descriptor_srv),
                               std::move(descriptor_srv_stencil), resource_state);
}

bool D3D12RenderTargetCache::IsHostDepthEncodingDifferent(
    xenos::DepthRenderTargetFormat format) const {
  if (format == xenos::DepthRenderTargetFormat::kD24FS8) {
    return !depth_float24_convert_in_pixel_shader_;
  }
  return false;
}

bool D3D12RenderTargetCache::IsGammaFormatHostStorageSeparate() const {
  return gamma_render_target_as_unorm16_;
}

void D3D12RenderTargetCache::RequestPixelShaderInterlockBarrier() {
  CommitEdramBufferUAVWrites();
}

void D3D12RenderTargetCache::TransitionEdramBuffer(D3D12_RESOURCE_STATES new_state) {
  if (!EnsureEdramBuffer()) {
    throw std::runtime_error("Could not allocate D3D12 compatibility render target storage");
  }
  command_processor_.native_gpu_diagnostics().RecordDependency(
      NativeGpuDiagnostics::Dependency::kEdram, 0, "use D3D12 EDRAM-layout buffer");
  if (command_processor_.PushTransitionBarrier(edram_buffer_, edram_buffer_state_, new_state)) {
    // Resetting edram_buffer_modification_status_ only if the barrier has been
    // truly inserted - in particular, not resetting it for UAV > UAV as
    // barriers are dropped if the state hasn't been changed.
    edram_buffer_modification_status_ = EdramBufferModificationStatus::kUnmodified;
  }
  edram_buffer_state_ = new_state;
}

void D3D12RenderTargetCache::MarkEdramBufferModified(
    EdramBufferModificationStatus modification_status) {
  assert_true(modification_status != EdramBufferModificationStatus::kUnmodified);
  assert_true(edram_buffer_state_ == D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  if (edram_buffer_state_ != D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
    return;
  }
  // max because being modified as a UAV requires stricter synchronization than
  // as ROV.
  edram_buffer_modification_status_ =
      std::max(edram_buffer_modification_status_, modification_status);
}

void D3D12RenderTargetCache::CommitEdramBufferUAVWrites(
    EdramBufferModificationStatus commit_status) {
  assert_true(commit_status != EdramBufferModificationStatus::kUnmodified);
  if (edram_buffer_modification_status_ < commit_status) {
    return;
  }
  assert_true(edram_buffer_state_ == D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  if (edram_buffer_state_ == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
    command_processor_.PushUAVBarrier(edram_buffer_);
  }
  edram_buffer_modification_status_ = EdramBufferModificationStatus::kUnmodified;
  PixelShaderInterlockFullEdramBarrierPlaced();
}

ID3D12PipelineState* const* D3D12RenderTargetCache::GetOrCreateTransferPipelines(
    TransferShaderKey key) {
  const TransferModeInfo& mode = kTransferModes[size_t(key.mode)];
  bool dest_is_stencil_bit = (mode.output == TransferOutput::kStencilBit);

  if (dest_is_stencil_bit) {
    auto pipelines_it = transfer_stencil_bit_pipelines_.find(key);
    if (pipelines_it != transfer_stencil_bit_pipelines_.end()) {
      return pipelines_it->second[0] ? pipelines_it->second.data() : nullptr;
    }
  } else {
    auto pipeline_it = transfer_pipelines_.find(key);
    if (pipeline_it != transfer_pipelines_.end()) {
      return pipeline_it->second ? &pipeline_it->second : nullptr;
    }
  }

  uint32_t rs = kTransferUsedRootParameters[size_t(use_stencil_reference_output_
                                                       ? mode.root_signature_with_stencil_ref
                                                       : mode.root_signature_no_stencil_ref)];

  const bool dest_is_color = mode.output == TransferOutput::kColor;
  const auto dest_color_format = xenos::ColorRenderTargetFormat(key.dest_resource_format);
  const auto dest_depth_format = xenos::DepthRenderTargetFormat(key.dest_resource_format);
  const auto source_color_format = xenos::ColorRenderTargetFormat(key.source_resource_format);
  const auto source_depth_format = xenos::DepthRenderTargetFormat(key.source_resource_format);

  // DX11 and DX12 share the sample-address and format-preservation contract.
  // Generate it once so fixes to aliases and resolution transitions agree.
  using Generator = DxbcRenderTargetTransferShader;
  Generator::TransferShaderKey shader_key;
  shader_key.dest_msaa_samples = key.dest_msaa_samples;
  shader_key.dest_resource_format = key.dest_resource_format;
  shader_key.source_msaa_samples = key.source_msaa_samples;
  shader_key.host_depth_source_msaa_samples = key.host_depth_source_msaa_samples;
  shader_key.source_resource_format = key.source_resource_format;
  shader_key.host_depth_source_is_copy = key.host_depth_source_is_copy;
  shader_key.dest_original_resolution = key.dest_original_resolution;
  shader_key.source_original_resolution = key.source_original_resolution;
  shader_key.host_depth_source_original_resolution = key.host_depth_source_original_resolution;
  shader_key.mode = Generator::TransferMode(uint32_t(key.mode));
  Generator::Options options;
  options.resolution_scale_x = draw_resolution_scale_x();
  options.resolution_scale_y = draw_resolution_scale_y();
  options.msaa_2x_supported = msaa_2x_supported_;
  options.stencil_reference_output = use_stencil_reference_output_;
  options.depth_float24_convert_in_pixel_shader = depth_float24_convert_in_pixel_shader();
  options.depth_float24_round = depth_float24_round();
  options.portable_integer_division = true;
  std::string error;
  if (!Generator::Create(shader_key, options, built_shader_, error)) {
    REXGPU_ERROR("D3D12 render target transfer shader: {}", error);
    return nullptr;
  }
  const uint32_t built_shader_size_bytes = uint32_t(built_shader_.size() * sizeof(uint32_t));

  // ***************************************************************************
  // Pipeline
  // ***************************************************************************

  ID3D12PipelineState* const* pipelines;
  ID3D12Device* device = command_processor_.GetD3D12Provider().GetDevice();
  D3D12_INPUT_ELEMENT_DESC pipeline_input_element_desc;
  pipeline_input_element_desc.SemanticName = "POSITION";
  pipeline_input_element_desc.SemanticIndex = 0;
  pipeline_input_element_desc.Format = DXGI_FORMAT_R32G32_FLOAT;
  pipeline_input_element_desc.InputSlot = 0;
  pipeline_input_element_desc.AlignedByteOffset = 0;
  pipeline_input_element_desc.InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
  pipeline_input_element_desc.InstanceDataStepRate = 0;
  D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline_desc = {};
  pipeline_desc.pRootSignature = transfer_root_signatures_[size_t(
      use_stencil_reference_output_ ? mode.root_signature_with_stencil_ref
                                    : mode.root_signature_no_stencil_ref)];
  pipeline_desc.VS.pShaderBytecode = shaders::passthrough_position_xy_vs;
  pipeline_desc.VS.BytecodeLength = sizeof(shaders::passthrough_position_xy_vs);
  pipeline_desc.PS.pShaderBytecode = built_shader_.data();
  pipeline_desc.PS.BytecodeLength = built_shader_size_bytes;
  if (key.dest_msaa_samples == xenos::MsaaSamples::k2X && !msaa_2x_supported_) {
    // Using sample 0 as 0 and 3 as 1 for 2x instead.
    pipeline_desc.SampleMask = 0b1001;
    pipeline_desc.SampleDesc.Count = 4;
  } else {
    pipeline_desc.SampleMask = UINT_MAX;
    pipeline_desc.SampleDesc.Count = UINT(1) << UINT(key.dest_msaa_samples);
  }
  pipeline_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  pipeline_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  pipeline_desc.RasterizerState.DepthClipEnable = TRUE;
  pipeline_desc.InputLayout.pInputElementDescs = &pipeline_input_element_desc;
  pipeline_desc.InputLayout.NumElements = 1;
  pipeline_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  if (dest_is_stencil_bit) {
    pipeline_desc.DepthStencilState.StencilEnable = TRUE;
    pipeline_desc.DepthStencilState.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
    pipeline_desc.DepthStencilState.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
    pipeline_desc.DepthStencilState.FrontFace.StencilPassOp = D3D12_STENCIL_OP_REPLACE;
    pipeline_desc.DepthStencilState.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    pipeline_desc.DepthStencilState.BackFace = pipeline_desc.DepthStencilState.FrontFace;
    pipeline_desc.DSVFormat = GetDepthDSVDXGIFormat(dest_depth_format);
    // Even if creation fails, still store the null pointers not to try to
    // create again.
    std::array<ID3D12PipelineState*, 8>& stencil_bit_pipelines =
        transfer_stencil_bit_pipelines_
            .emplace(std::piecewise_construct, std::make_tuple(key), std::make_tuple())
            .first->second;
    bool stencil_pipelines_created = true;
    for (uint32_t i = 0; i < 8; ++i) {
      pipeline_desc.DepthStencilState.StencilWriteMask = UINT8(1) << i;
      if (SUCCEEDED(device->CreateGraphicsPipelineState(&pipeline_desc,
                                                        IID_PPV_ARGS(&stencil_bit_pipelines[i])))) {
        continue;
      }
      stencil_pipelines_created = false;
      for (uint32_t j = 0; j < i; ++j) {
        stencil_bit_pipelines[j]->Release();
        stencil_bit_pipelines[j] = nullptr;
      }
      break;
    }
    pipelines = stencil_pipelines_created ? stencil_bit_pipelines.data() : nullptr;
  } else {
    if (dest_is_color) {
      pipeline_desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
      pipeline_desc.NumRenderTargets = 1;
      pipeline_desc.RTVFormats[0] = GetColorOwnershipTransferDXGIFormat(dest_color_format);
    } else {
      pipeline_desc.DepthStencilState.DepthEnable = TRUE;
      pipeline_desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
      pipeline_desc.DepthStencilState.DepthFunc = REXCVAR_GET(depth_transfer_not_equal_test)
                                                      ? D3D12_COMPARISON_FUNC_NOT_EQUAL
                                                      : D3D12_COMPARISON_FUNC_ALWAYS;
      if (use_stencil_reference_output_) {
        pipeline_desc.DepthStencilState.StencilEnable = TRUE;
        pipeline_desc.DepthStencilState.StencilWriteMask = UINT8_MAX;
        pipeline_desc.DepthStencilState.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
        pipeline_desc.DepthStencilState.FrontFace.StencilDepthFailOp =
            REXCVAR_GET(depth_transfer_not_equal_test) ? D3D12_STENCIL_OP_REPLACE
                                                       : D3D12_STENCIL_OP_KEEP;
        pipeline_desc.DepthStencilState.FrontFace.StencilPassOp = D3D12_STENCIL_OP_REPLACE;
        // Using ALWAYS, not NOT_EQUAL, so depth writing is unaffected by
        // stencil being different.
        pipeline_desc.DepthStencilState.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        pipeline_desc.DepthStencilState.BackFace = pipeline_desc.DepthStencilState.FrontFace;
      }
      pipeline_desc.DSVFormat = GetDepthDSVDXGIFormat(dest_depth_format);
    }
    ID3D12PipelineState* pipeline;
    if (FAILED(device->CreateGraphicsPipelineState(&pipeline_desc, IID_PPV_ARGS(&pipeline)))) {
      pipeline = nullptr;
    }
    // Even if creation fails, still store the null pointer not to try to create
    // again.
    // Return a pointer to the persistent location.
    ID3D12PipelineState*& inserted_pipeline =
        transfer_pipelines_.emplace(key, pipeline).first->second;
    pipelines = inserted_pipeline ? &inserted_pipeline : nullptr;
  }
  // TODO(Triang3l): Pipeline state name debug names (lots of variables - but
  // not very important since everything can be derived from the bindings and
  // outputs in a debugger).

  if (!pipelines) {
    // Stencil bit copying uses only the stencil SRV for depth / stencil source,
    // can't use srv_index_depth for checking.
    const char* source_format_name =
        (rs & kTransferUsedRootParameterColorSRVBit)
            ? xenos::GetColorRenderTargetFormatName(source_color_format)
            : xenos::GetDepthRenderTargetFormatName(source_depth_format);
    const char* dest_format_name = mode.output == TransferOutput::kColor
                                       ? xenos::GetColorRenderTargetFormatName(dest_color_format)
                                       : xenos::GetDepthRenderTargetFormatName(dest_depth_format);
    if (rs & kTransferUsedRootParameterHostDepthSRVBit) {
      REXGPU_ERROR(
          "D3D12RenderTargetCache: Failed to create a render target ownership "
          "transfer pipeline for {}-sample {} + {}-sample host depth{} -> "
          "{}-sample {} for mode {}",
          uint32_t(1) << uint32_t(key.source_msaa_samples), source_format_name,
          uint32_t(1) << uint32_t(key.host_depth_source_msaa_samples),
          key.host_depth_source_is_copy ? " copy" : "",
          uint32_t(1) << uint32_t(key.dest_msaa_samples), dest_format_name, uint32_t(key.mode));
    } else {
      REXGPU_ERROR(
          "D3D12RenderTargetCache: Failed to create a render target ownership "
          "transfer pipeline for {}-sample {} -> {}-sample {} for mode {}",
          uint32_t(1) << uint32_t(key.source_msaa_samples), source_format_name,
          uint32_t(1) << uint32_t(key.dest_msaa_samples), dest_format_name, uint32_t(key.mode));
    }
  }
  return pipelines;
}

D3D12RenderTargetCache::D3D12RenderTarget* D3D12RenderTargetCache::SnapshotNativeHostDepth(
    D3D12RenderTarget& source) {
  auto snapshot = std::unique_ptr<D3D12RenderTarget>(
      static_cast<D3D12RenderTarget*>(CreateRenderTarget(source.key())));
  if (!snapshot)
    return nullptr;
  command_processor_.PushTransitionBarrier(
      source.resource(), source.SetResourceState(D3D12_RESOURCE_STATE_COPY_SOURCE),
      D3D12_RESOURCE_STATE_COPY_SOURCE);
  command_processor_.PushTransitionBarrier(
      snapshot->resource(), snapshot->SetResourceState(D3D12_RESOURCE_STATE_COPY_DEST),
      D3D12_RESOURCE_STATE_COPY_DEST);
  command_processor_.SubmitBarriers();
  command_processor_.GetDeferredCommandList().D3DCopyResource(snapshot->resource(),
                                                              source.resource());
  auto* result = snapshot.get();
  native_depth_snapshots_.emplace_back(command_processor_.GetCurrentSubmission(),
                                       std::move(snapshot));
  if (++native_depth_snapshot_count_ <= 8 || !(native_depth_snapshot_count_ & 255))
    REXGPU_INFO("[d3d12-native-depth-snapshot] {} in-place depth versions retained as images",
                native_depth_snapshot_count_);
  return result;
}

void D3D12RenderTargetCache::PerformTransfersAndResolveClears(
    uint32_t render_target_count, RenderTarget* const* render_targets,
    const std::vector<Transfer>* render_target_transfers,
    const uint64_t* render_target_resolve_clear_values,
    const Transfer::Rectangle* resolve_clear_rectangle) {
  assert_true(GetPath() == Path::kHostRenderTargets);

  const ui::d3d12::D3D12Provider& provider = command_processor_.GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();
  uint64_t current_submission = command_processor_.GetCurrentSubmission();
  DeferredCommandList& command_list = command_processor_.GetDeferredCommandList();

  // An aliased colour writer can leave this depth attachment as the retained
  // high-precision depth owner. Freeze its image before writing it again, so
  // the transfer samples a separate native attachment rather than EDRAM.
  std::array<std::vector<Transfer>, 1 + xenos::kMaxColorRenderTargets> native_transfers;
  bool needs_depth_snapshot = false;
  if (render_target_transfers) {
    for (uint32_t i = 0; i < render_target_count && !needs_depth_snapshot; ++i)
      needs_depth_snapshot =
          render_targets[i] &&
          std::any_of(render_target_transfers[i].begin(), render_target_transfers[i].end(),
                      [&](const Transfer& transfer) {
                        return transfer.host_depth_source == render_targets[i];
                      });
  }
  if (needs_depth_snapshot) {
    assert_true(render_target_count <= native_transfers.size());
    bool replaced = false;
    for (uint32_t i = 0; i < render_target_count; ++i) {
      native_transfers[i] = render_target_transfers[i];
      RenderTarget* snapshot = nullptr;
      for (auto& transfer : native_transfers[i]) {
        if (!render_targets[i] || transfer.host_depth_source != render_targets[i])
          continue;
        if (!snapshot)
          snapshot = SnapshotNativeHostDepth(*static_cast<D3D12RenderTarget*>(render_targets[i]));
        if (snapshot) {
          transfer.host_depth_source = snapshot;
          replaced = true;
        }
      }
    }
    if (replaced)
      render_target_transfers = native_transfers.data();
  }

  bool resolve_clear_needed = render_target_resolve_clear_values && resolve_clear_rectangle;
  D3D12_RECT clear_rect;
  if (resolve_clear_needed) {
    // Assuming the rectangle is already clamped by the setup function from the
    // common render target cache.
    clear_rect.left = LONG(resolve_clear_rectangle->x_pixels * draw_resolution_scale_x());
    clear_rect.top = LONG(resolve_clear_rectangle->y_pixels * draw_resolution_scale_y());
    clear_rect.right =
        LONG((resolve_clear_rectangle->x_pixels + resolve_clear_rectangle->width_pixels) *
             draw_resolution_scale_x());
    clear_rect.bottom =
        LONG((resolve_clear_rectangle->y_pixels + resolve_clear_rectangle->height_pixels) *
             draw_resolution_scale_y());
  }

  // Do host depth storing for the depth destination (assuming there can be only
  // one depth destination) where depth destination == host depth source.
  bool host_depth_store_set_up = false;
  for (uint32_t i = 0; i < render_target_count; ++i) {
    RenderTarget* dest_rt = render_targets[i];
    if (!dest_rt) {
      continue;
    }
    auto& dest_d3d12_rt = *static_cast<D3D12RenderTarget*>(dest_rt);
    RenderTargetKey dest_rt_key = dest_d3d12_rt.key();
    if (!dest_rt_key.is_depth) {
      continue;
    }
    const std::vector<Transfer>& depth_transfers = render_target_transfers[i];
    for (const Transfer& transfer : depth_transfers) {
      if (transfer.host_depth_source != dest_rt) {
        continue;
      }
      if (!host_depth_store_set_up) {
        if (!EnsureEdramBuffer()) {
          return;
        }
        // Bindings.
        // 0 - source.
        // 1 - EDRAM if bindful.
        ui::d3d12::util::DescriptorCpuGpuHandlePair host_depth_store_descriptors[2];
        if (!command_processor_.RequestOneUseSingleViewDescriptors(
                1 + uint32_t(!bindless_resources_used_), host_depth_store_descriptors)) {
          continue;
        }
        command_list.D3DSetComputeRootSignature(host_depth_store_root_signature_);
        // Destination (EDRAM uint4 buffer).
        if (bindless_resources_used_) {
          command_list.D3DSetComputeRootDescriptorTable(
              kHostDepthStoreRootParameterDest,
              command_processor_.GetEdramUintPow2BindlessUAVHandlePair(4).second);
        } else {
          const ui::d3d12::util::DescriptorCpuGpuHandlePair& host_depth_store_descriptor_dest =
              host_depth_store_descriptors[1];
          WriteEdramUintPow2UAVDescriptor(host_depth_store_descriptor_dest.first, 4);
          command_list.D3DSetComputeRootDescriptorTable(kHostDepthStoreRootParameterDest,
                                                        host_depth_store_descriptor_dest.second);
        }
        // Depth source texture.
        const ui::d3d12::util::DescriptorCpuGpuHandlePair& host_depth_store_descriptor_source =
            host_depth_store_descriptors[0];
        device->CopyDescriptorsSimple(1, host_depth_store_descriptor_source.first,
                                      dest_d3d12_rt.descriptor_srv().GetHandle(),
                                      D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        command_list.D3DSetComputeRootDescriptorTable(kHostDepthStoreRootParameterSource,
                                                      host_depth_store_descriptor_source.second);
        // Render target constant.
        HostDepthStoreRenderTargetConstant host_depth_store_render_target_constant =
            GetHostDepthStoreRenderTargetConstant(dest_rt_key.pitch_tiles_at_32bpp,
                                                  msaa_2x_supported_);
        command_list.D3DSetComputeRoot32BitConstants(
            kHostDepthStoreRootParameterConstants,
            sizeof(host_depth_store_render_target_constant) / sizeof(uint32_t),
            &host_depth_store_render_target_constant,
            offsetof(HostDepthStoreConstants, render_target) / sizeof(uint32_t));
        // Barriers - don't need to try to combine them with the rest of
        // render target transfer barriers now - if this happens, after host
        // depth storing, NON_PIXEL_SHADER_RESOURCE -> DEPTH_WRITE will be done
        // anyway even in the best case, so it's not possible to have all the
        // barriers in one place here.
        TransitionEdramBuffer(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        command_processor_.PushTransitionBarrier(
            dest_d3d12_rt.resource(),
            dest_d3d12_rt.SetResourceState(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        // Pipeline.
        command_processor_.SetExternalPipeline(
            host_depth_store_pipelines_[size_t(dest_rt_key.msaa_samples)]);
        host_depth_store_set_up = true;
      }
      Transfer::Rectangle transfer_rectangles[Transfer::kMaxRectanglesWithCutout];
      uint32_t transfer_rectangle_count = transfer.GetRectangles(
          dest_rt_key.base_tiles, dest_rt_key.pitch_tiles_at_32bpp, dest_rt_key.msaa_samples, false,
          transfer_rectangles, resolve_clear_rectangle);
      assert_not_zero(transfer_rectangle_count);
      HostDepthStoreRectangleConstant host_depth_store_rectangle_constant;
      for (uint32_t j = 0; j < transfer_rectangle_count; ++j) {
        uint32_t group_count_x, group_count_y;
        GetHostDepthStoreRectangleInfo(transfer_rectangles[j], dest_rt_key.msaa_samples,
                                       host_depth_store_rectangle_constant, group_count_x,
                                       group_count_y);
        command_list.D3DSetComputeRoot32BitConstants(
            kHostDepthStoreRootParameterConstants,
            sizeof(host_depth_store_rectangle_constant) / sizeof(uint32_t),
            &host_depth_store_rectangle_constant,
            offsetof(HostDepthStoreConstants, rectangle) / sizeof(uint32_t));
        command_processor_.SubmitBarriers();
        command_list.D3DDispatch(group_count_x, group_count_y, 1);
        MarkEdramBufferModified();
      }
    }
    break;
  }

  // Try to insert as many barriers as possible in one place, hoping that in the
  // best case (no cross-copying between current render targets), barriers will
  // need to be only inserted here, not between transfers. In case of
  // cross-copying, if the destination use is going to happen before the source
  // use, choose the destination state, otherwise the source state - to match
  // the order in which transfers will actually happen (otherwise there will be
  // just a useless switch back and forth).
  for (uint32_t i = 0; i < render_target_count; ++i) {
    RenderTarget* dest_rt = render_targets[i];
    if (!dest_rt) {
      continue;
    }
    auto& dest_d3d12_rt = *static_cast<D3D12RenderTarget*>(dest_rt);
    const std::vector<Transfer>& dest_transfers = render_target_transfers[i];
    if (!resolve_clear_needed && dest_transfers.empty()) {
      continue;
    }
    // Transition the sources, only if not going to be used as destinations
    // earlier.
    for (const Transfer& transfer : render_target_transfers[i]) {
      bool source_previously_used_as_dest = false;
      bool host_depth_source_previously_used_as_dest = false;
      for (uint32_t j = 0; j < i; ++j) {
        if (render_target_transfers[j].empty()) {
          continue;
        }
        const RenderTarget* previous_rt = render_targets[j];
        if (transfer.source == previous_rt) {
          source_previously_used_as_dest = true;
        }
        if (transfer.host_depth_source == previous_rt) {
          host_depth_source_previously_used_as_dest = true;
        }
      }
      if (!source_previously_used_as_dest) {
        auto& source_d3d12_rt = *static_cast<D3D12RenderTarget*>(transfer.source);
        command_processor_.PushTransitionBarrier(
            source_d3d12_rt.resource(),
            source_d3d12_rt.SetResourceState(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
      }
      // transfer.host_depth_source == dest_rt means the EDRAM buffer will be
      // used instead, no need to transition.
      if (transfer.host_depth_source && transfer.host_depth_source != dest_rt &&
          !host_depth_source_previously_used_as_dest) {
        auto& host_depth_source_d3d12_rt =
            *static_cast<D3D12RenderTarget*>(transfer.host_depth_source);
        command_processor_.PushTransitionBarrier(
            host_depth_source_d3d12_rt.resource(),
            host_depth_source_d3d12_rt.SetResourceState(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
      }
    }
    // Transition the destination, only if not going to be used as a source
    // earlier.
    bool dest_used_previously_as_source = false;
    for (uint32_t j = 0; j < i; ++j) {
      for (const Transfer& previous_transfer : render_target_transfers[j]) {
        if (previous_transfer.source == dest_rt || previous_transfer.host_depth_source == dest_rt) {
          dest_used_previously_as_source = true;
          break;
        }
      }
    }
    if (!dest_used_previously_as_source) {
      D3D12_RESOURCE_STATES dest_state = dest_d3d12_rt.key().is_depth
                                             ? D3D12_RESOURCE_STATE_DEPTH_WRITE
                                             : D3D12_RESOURCE_STATE_RENDER_TARGET;
      command_processor_.PushTransitionBarrier(
          dest_d3d12_rt.resource(), dest_d3d12_rt.SetResourceState(dest_state), dest_state);
    }
  }
  if (host_depth_store_set_up) {
    // Will be reading copied host depth from the EDRAM buffer.
    TransitionEdramBuffer(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  }

  // Copy source descriptors to the shader-visible heap.
  // Clear previously set shader-visible descriptor indices.
  for (uint32_t i = 0; i < render_target_count; ++i) {
    RenderTarget* dest_rt = render_targets[i];
    if (!dest_rt) {
      continue;
    }
    for (const Transfer& transfer : render_target_transfers[i]) {
      assert_not_null(transfer.source);
      auto& source_d3d12_rt = *static_cast<D3D12RenderTarget*>(transfer.source);
      source_d3d12_rt.SetTemporarySRVDescriptorIndex(UINT32_MAX);
      source_d3d12_rt.SetTemporarySRVDescriptorIndexStencil(UINT32_MAX);
      auto* host_depth_source_d3d12_rt =
          static_cast<D3D12RenderTarget*>(transfer.host_depth_source);
      if (host_depth_source_d3d12_rt) {
        host_depth_source_d3d12_rt->SetTemporarySRVDescriptorIndex(UINT32_MAX);
        host_depth_source_d3d12_rt->SetTemporarySRVDescriptorIndexStencil(UINT32_MAX);
      }
    }
  }
  current_temporary_descriptors_cpu_.clear();
  uint32_t host_depth_copy_srv_index;
  if (host_depth_store_set_up && !bindless_resources_used_) {
    host_depth_copy_srv_index = uint32_t(current_temporary_descriptors_cpu_.size());
    current_temporary_descriptors_cpu_.push_back(provider.OffsetViewDescriptor(
        edram_buffer_descriptor_heap_start_, uint32_t(EdramBufferDescriptorIndex::kR32UintSRV)));
  } else {
    host_depth_copy_srv_index = UINT32_MAX;
  }
  for (uint32_t i = 0; i < render_target_count; ++i) {
    RenderTarget* dest_rt = render_targets[i];
    if (!dest_rt) {
      continue;
    }
    bool dest_is_depth = dest_rt->key().is_depth;
    for (const Transfer& transfer : render_target_transfers[i]) {
      assert_not_null(transfer.source);
      auto& source_d3d12_rt = *static_cast<D3D12RenderTarget*>(transfer.source);
      if (source_d3d12_rt.temporary_srv_descriptor_index() == UINT32_MAX) {
        source_d3d12_rt.SetTemporarySRVDescriptorIndex(
            uint32_t(current_temporary_descriptors_cpu_.size()));
        current_temporary_descriptors_cpu_.push_back(source_d3d12_rt.descriptor_srv().GetHandle());
      }
      if (source_d3d12_rt.key().is_depth &&
          source_d3d12_rt.temporary_srv_descriptor_index_stencil() == UINT32_MAX) {
        source_d3d12_rt.SetTemporarySRVDescriptorIndexStencil(
            uint32_t(current_temporary_descriptors_cpu_.size()));
        current_temporary_descriptors_cpu_.push_back(
            source_d3d12_rt.descriptor_srv_stencil().GetHandle());
      }
      bool source_is_depth = source_d3d12_rt.key().is_depth;
      auto* host_depth_source_d3d12_rt =
          static_cast<D3D12RenderTarget*>(transfer.host_depth_source);
      // The host_depth_source_d3d12_rt == dest_rt case would use the EDRAM
      // buffer instead.
      if (host_depth_source_d3d12_rt && host_depth_source_d3d12_rt != dest_rt &&
          host_depth_source_d3d12_rt->temporary_srv_descriptor_index() == UINT32_MAX) {
        host_depth_source_d3d12_rt->SetTemporarySRVDescriptorIndex(
            uint32_t(current_temporary_descriptors_cpu_.size()));
        current_temporary_descriptors_cpu_.push_back(
            host_depth_source_d3d12_rt->descriptor_srv().GetHandle());
      }
    }
  }
  uint32_t descriptor_count = uint32_t(current_temporary_descriptors_cpu_.size());
  current_temporary_descriptors_gpu_.resize(descriptor_count);
  if (!command_processor_.RequestOneUseSingleViewDescriptors(
          descriptor_count, current_temporary_descriptors_gpu_.data())) {
    return;
  }
  for (uint32_t i = 0; i < descriptor_count; ++i) {
    device->CopyDescriptorsSimple(1, current_temporary_descriptors_gpu_[i].first,
                                  current_temporary_descriptors_cpu_[i],
                                  D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  }

  // Perform the transfers and clears.

  bool transfer_viewport_set = false;
  float pixels_to_ndc_unscaled = 2.0f / float(D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION);

  TransferRootSignatureIndex last_transfer_root_signature_index =
      TransferRootSignatureIndex::kCount;
  uint32_t transfer_root_parameters_set = 0;
  uint32_t last_descriptor_index_color = UINT32_MAX;
  uint32_t last_descriptor_index_depth = UINT32_MAX;
  uint32_t last_descriptor_index_stencil = UINT32_MAX;
  uint32_t last_descriptor_index_host_depth_non_copy = UINT32_MAX;
  bool last_descriptor_host_depth_is_copy = false;
  TransferAddressConstant last_address_constant;
  TransferAddressConstant last_host_depth_address_constant;

  for (uint32_t i = 0; i < render_target_count; ++i) {
    RenderTarget* dest_rt = render_targets[i];
    if (!dest_rt) {
      continue;
    }

    const std::vector<Transfer>& current_transfers = render_target_transfers[i];
    if (current_transfers.empty() && !resolve_clear_needed) {
      continue;
    }

    auto& dest_d3d12_rt = *static_cast<D3D12RenderTarget*>(dest_rt);
    RenderTargetKey dest_rt_key = dest_d3d12_rt.key();
    float pixels_to_ndc_x = pixels_to_ndc_unscaled * GetRenderTargetScaleX(dest_rt_key);
    float pixels_to_ndc_y = pixels_to_ndc_unscaled * GetRenderTargetScaleY(dest_rt_key);

    // Late barrier in case there was cross-copying that prevented merging of
    // barriers.
    D3D12_RESOURCE_STATES dest_state = dest_rt_key.is_depth ? D3D12_RESOURCE_STATE_DEPTH_WRITE
                                                            : D3D12_RESOURCE_STATE_RENDER_TARGET;
    command_processor_.PushTransitionBarrier(
        dest_d3d12_rt.resource(), dest_d3d12_rt.SetResourceState(dest_state), dest_state);

    if (!current_transfers.empty()) {
      are_current_command_list_render_targets_valid_ = false;
      if (dest_rt_key.is_depth) {
        D3D12_CPU_DESCRIPTOR_HANDLE dsv_handle = dest_d3d12_rt.descriptor_draw().GetHandle();
        command_list.D3DOMSetRenderTargets(0, nullptr, FALSE, &dsv_handle);
        if (!use_stencil_reference_output_) {
          command_processor_.SetStencilReference(UINT8_MAX);
        }
      } else {
        D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle =
            dest_d3d12_rt.descriptor_load_separate().IsValid()
                ? dest_d3d12_rt.descriptor_load_separate().GetHandle()
                : dest_d3d12_rt.descriptor_draw().GetHandle();
        command_list.D3DOMSetRenderTargets(1, &rtv_handle, FALSE, nullptr);
      }

      uint32_t dest_pitch_tiles = dest_rt_key.GetPitchTiles();
      bool dest_is_64bpp = dest_rt_key.Is64bpp();

      // Gather shader keys and sort to reduce pipeline state and binding
      // switches. Also gather stencil rectangles to clear if needed.
      bool need_stencil_bit_draws = dest_rt_key.is_depth && !use_stencil_reference_output_;
      current_transfer_invocations_.clear();
      current_transfer_invocations_.reserve(current_transfers.size()
                                            << uint32_t(need_stencil_bit_draws));
      uint32_t rt_sort_index = 0;
      TransferShaderKey new_transfer_shader_key;
      new_transfer_shader_key.dest_msaa_samples = dest_rt_key.msaa_samples;
      new_transfer_shader_key.dest_resource_format = dest_rt_key.resource_format;
      new_transfer_shader_key.dest_original_resolution = dest_rt_key.original_resolution;
      uint32_t stencil_clear_rectangle_count = 0;
      for (uint32_t j = 0; j <= uint32_t(need_stencil_bit_draws); ++j) {
        // j == 0 - color or depth.
        // j == 1 - stencil bits.
        // Stencil bit writing always requires a different root signature,
        // handle these separately. Stencil never has a host depth source.
        // Clear previously set sort indices.
        for (const Transfer& transfer : current_transfers) {
          auto* host_depth_source_d3d12_rt =
              static_cast<D3D12RenderTarget*>(transfer.host_depth_source);
          if (host_depth_source_d3d12_rt) {
            host_depth_source_d3d12_rt->SetTemporarySortIndex(UINT32_MAX);
          }
          assert_not_null(transfer.source);
          auto& source_d3d12_rt = *static_cast<D3D12RenderTarget*>(transfer.source);
          source_d3d12_rt.SetTemporarySortIndex(UINT32_MAX);
        }
        for (const Transfer& transfer : current_transfers) {
          assert_not_null(transfer.source);
          auto& source_d3d12_rt = *static_cast<D3D12RenderTarget*>(transfer.source);
          D3D12RenderTarget* host_depth_source_d3d12_rt =
              j ? nullptr : static_cast<D3D12RenderTarget*>(transfer.host_depth_source);
          if (host_depth_source_d3d12_rt &&
              host_depth_source_d3d12_rt->temporary_sort_index() == UINT32_MAX) {
            host_depth_source_d3d12_rt->SetTemporarySortIndex(rt_sort_index++);
          }
          if (source_d3d12_rt.temporary_sort_index() == UINT32_MAX) {
            source_d3d12_rt.SetTemporarySortIndex(rt_sort_index++);
          }
          RenderTargetKey source_rt_key = source_d3d12_rt.key();
          new_transfer_shader_key.source_msaa_samples = source_rt_key.msaa_samples;
          new_transfer_shader_key.source_resource_format = source_rt_key.resource_format;
          new_transfer_shader_key.source_original_resolution = source_rt_key.original_resolution;
          bool host_depth_source_is_copy = host_depth_source_d3d12_rt == &dest_d3d12_rt;
          new_transfer_shader_key.host_depth_source_is_copy = host_depth_source_is_copy;
          new_transfer_shader_key.host_depth_source_original_resolution =
              host_depth_source_d3d12_rt && !host_depth_source_is_copy
                  ? host_depth_source_d3d12_rt->key().original_resolution
                  : 0;
          // The host depth copy buffer has only raw samples.
          new_transfer_shader_key.host_depth_source_msaa_samples =
              (host_depth_source_d3d12_rt && !host_depth_source_is_copy)
                  ? host_depth_source_d3d12_rt->key().msaa_samples
                  : xenos::MsaaSamples::k1X;
          if (j) {
            new_transfer_shader_key.mode = source_rt_key.is_depth
                                               ? TransferMode::kDepthToStencilBit
                                               : TransferMode::kColorToStencilBit;
            stencil_clear_rectangle_count += transfer.GetRectangles(
                dest_rt_key.base_tiles, dest_pitch_tiles, dest_rt_key.msaa_samples, dest_is_64bpp,
                nullptr, resolve_clear_rectangle);
          } else {
            if (dest_rt_key.is_depth) {
              if (host_depth_source_d3d12_rt) {
                new_transfer_shader_key.mode = source_rt_key.is_depth
                                                   ? TransferMode::kDepthAndHostDepthToDepth
                                                   : TransferMode::kColorAndHostDepthToDepth;
              } else {
                new_transfer_shader_key.mode = source_rt_key.is_depth ? TransferMode::kDepthToDepth
                                                                      : TransferMode::kColorToDepth;
              }
            } else {
              new_transfer_shader_key.mode = source_rt_key.is_depth ? TransferMode::kDepthToColor
                                                                    : TransferMode::kColorToColor;
            }
          }
          current_transfer_invocations_.emplace_back(transfer, new_transfer_shader_key);
          if (j) {
            current_transfer_invocations_.back().transfer.host_depth_source = nullptr;
          }
        }
      }
      std::sort(current_transfer_invocations_.begin(), current_transfer_invocations_.end());

      // Clear the stencil to 0 where it will be loaded - will be setting the
      // bits that need to be 1 by discarding samples. Clearing everything here
      // to reduce context switches internally in the driver if clear causes
      // them.
      if (stencil_clear_rectangle_count) {
        command_processor_.SubmitBarriers();
        D3D12_RECT* stencil_clear_rect_write_ptr = command_list.ClearDepthStencilViewAllocatedRects(
            dest_d3d12_rt.descriptor_draw().GetHandle(), D3D12_CLEAR_FLAG_STENCIL, 0.0f, 0,
            stencil_clear_rectangle_count);
        assert_not_null(stencil_clear_rect_write_ptr);
        for (const Transfer& transfer : current_transfers) {
          Transfer::Rectangle transfer_stencil_clear_rectangles[Transfer::kMaxRectanglesWithCutout];
          uint32_t transfer_stencil_clear_rectangle_count = transfer.GetRectangles(
              dest_rt_key.base_tiles, dest_pitch_tiles, dest_rt_key.msaa_samples, dest_is_64bpp,
              transfer_stencil_clear_rectangles, resolve_clear_rectangle);
          for (uint32_t j = 0; j < transfer_stencil_clear_rectangle_count; ++j) {
            const Transfer::Rectangle& stencil_clear_rectangle =
                transfer_stencil_clear_rectangles[j];
            stencil_clear_rect_write_ptr->left =
                LONG(stencil_clear_rectangle.x_pixels * GetRenderTargetScaleX(dest_rt_key));
            stencil_clear_rect_write_ptr->top =
                LONG(stencil_clear_rectangle.y_pixels * GetRenderTargetScaleY(dest_rt_key));
            stencil_clear_rect_write_ptr->right =
                LONG((stencil_clear_rectangle.x_pixels + stencil_clear_rectangle.width_pixels) *
                     GetRenderTargetScaleX(dest_rt_key));
            stencil_clear_rect_write_ptr->bottom =
                LONG((stencil_clear_rectangle.y_pixels + stencil_clear_rectangle.height_pixels) *
                     GetRenderTargetScaleY(dest_rt_key));
            ++stencil_clear_rect_write_ptr;
          }
        }
      }

      // Perform the transfers for the render target.

      if (!transfer_viewport_set) {
        transfer_viewport_set = true;
        // Will be passing NDC directly, set the viewport to the maximum host
        // render target size for simplicity. Using a power-of-two scale for
        // exact pixel coordinates.
        D3D12_VIEWPORT transfer_viewport;
        transfer_viewport.TopLeftX = 0.0f;
        transfer_viewport.TopLeftY = 0.0f;
        transfer_viewport.Width = float(D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION);
        transfer_viewport.Height = float(D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION);
        transfer_viewport.MinDepth = 0.0f;
        transfer_viewport.MaxDepth = 1.0f;
        command_processor_.SetViewport(transfer_viewport);
        // TODO(Triang3l): Reduce scissor to the smallest transfer region for
        // more tiling friendliness.
        D3D12_RECT transfer_scissor;
        transfer_scissor.left = 0;
        transfer_scissor.top = 0;
        transfer_scissor.right = LONG(D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION);
        transfer_scissor.bottom = LONG(D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION);
        command_processor_.SetScissorRect(transfer_scissor);
      }

      for (auto it = current_transfer_invocations_.cbegin();
           it != current_transfer_invocations_.cend(); ++it) {
        const TransferInvocation& transfer_invocation_first = *it;
        // Will be merging transfers from the same source into one mesh.
        auto it_merged_first = it, it_merged_last = it;
        uint32_t transfer_rectangle_count = transfer_invocation_first.transfer.GetRectangles(
            dest_rt_key.base_tiles, dest_pitch_tiles, dest_rt_key.msaa_samples, dest_is_64bpp,
            nullptr, resolve_clear_rectangle);
        for (auto it_merge = std::next(it_merged_first);
             it_merge != current_transfer_invocations_.cend(); ++it_merge) {
          if (!transfer_invocation_first.CanBeMergedIntoOneDraw(*it_merge)) {
            break;
          }
          transfer_rectangle_count += it_merge->transfer.GetRectangles(
              dest_rt_key.base_tiles, dest_pitch_tiles, dest_rt_key.msaa_samples, dest_is_64bpp,
              nullptr, resolve_clear_rectangle);
          it_merged_last = it_merge;
        }
        assert_not_zero(transfer_rectangle_count);
        // Skip the merged transfers in the subsequent iterations.
        it = it_merged_last;

        assert_not_null(it->transfer.source);
        auto& source_d3d12_rt = *static_cast<D3D12RenderTarget*>(it->transfer.source);
        auto* host_depth_source_d3d12_rt =
            static_cast<D3D12RenderTarget*>(it->transfer.host_depth_source);
        TransferShaderKey transfer_shader_key = it->shader_key;
        const TransferModeInfo& transfer_mode_info =
            kTransferModes[size_t(transfer_shader_key.mode)];
        TransferRootSignatureIndex transfer_root_signature_index =
            use_stencil_reference_output_ ? transfer_mode_info.root_signature_with_stencil_ref
                                          : transfer_mode_info.root_signature_no_stencil_ref;
        uint32_t transfer_root_parameters_used =
            kTransferUsedRootParameters[size_t(transfer_root_signature_index)];
        bool is_stencil_bit =
            (transfer_root_parameters_used & kTransferUsedRootParameterStencilMaskConstantBit) != 0;

        // Late barriers in case there was cross-copying that prevented merging
        // of barriers.
        command_processor_.PushTransitionBarrier(
            source_d3d12_rt.resource(),
            source_d3d12_rt.SetResourceState(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        if (host_depth_source_d3d12_rt) {
          if (transfer_shader_key.host_depth_source_is_copy) {
            // Reading copied host depth from the EDRAM buffer.
            TransitionEdramBuffer(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
          } else {
            // Reading host depth from the texture.
            command_processor_.PushTransitionBarrier(
                host_depth_source_d3d12_rt->resource(),
                host_depth_source_d3d12_rt->SetResourceState(
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
          }
        }

        uint32_t transfer_vertex_count = 6 * transfer_rectangle_count;
        D3D12_VERTEX_BUFFER_VIEW transfer_rectangle_buffer_view;
        transfer_rectangle_buffer_view.StrideInBytes = sizeof(float) * 2;
        transfer_rectangle_buffer_view.SizeInBytes =
            transfer_rectangle_buffer_view.StrideInBytes * transfer_vertex_count;
        float* transfer_rectangle_write_ptr =
            reinterpret_cast<float*>(transfer_vertex_buffer_pool_->Request(
                current_submission, transfer_rectangle_buffer_view.SizeInBytes, sizeof(float),
                nullptr, nullptr, &transfer_rectangle_buffer_view.BufferLocation));
        if (!transfer_rectangle_write_ptr) {
          continue;
        }
        for (auto it_merged = it_merged_first; it_merged <= it_merged_last; ++it_merged) {
          Transfer::Rectangle transfer_invocation_rectangles[Transfer::kMaxRectanglesWithCutout];
          uint32_t transfer_invocation_rectangle_count = it_merged->transfer.GetRectangles(
              dest_rt_key.base_tiles, dest_pitch_tiles, dest_rt_key.msaa_samples, dest_is_64bpp,
              transfer_invocation_rectangles, resolve_clear_rectangle);
          assert_not_zero(transfer_invocation_rectangle_count);
          for (uint32_t j = 0; j < transfer_invocation_rectangle_count; ++j) {
            const Transfer::Rectangle& transfer_rectangle = transfer_invocation_rectangles[j];
            float transfer_rectangle_x0 = -1.0f + transfer_rectangle.x_pixels * pixels_to_ndc_x;
            float transfer_rectangle_y0 = 1.0f - transfer_rectangle.y_pixels * pixels_to_ndc_y;
            float transfer_rectangle_x1 =
                transfer_rectangle_x0 + transfer_rectangle.width_pixels * pixels_to_ndc_x;
            float transfer_rectangle_y1 =
                transfer_rectangle_y0 - transfer_rectangle.height_pixels * pixels_to_ndc_y;
            // O-*
            // |/
            // *
            *(transfer_rectangle_write_ptr++) = transfer_rectangle_x0;
            *(transfer_rectangle_write_ptr++) = transfer_rectangle_y0;
            // *-O
            // |/
            // *
            *(transfer_rectangle_write_ptr++) = transfer_rectangle_x1;
            *(transfer_rectangle_write_ptr++) = transfer_rectangle_y0;
            // *-*
            // |/
            // O
            *(transfer_rectangle_write_ptr++) = transfer_rectangle_x0;
            *(transfer_rectangle_write_ptr++) = transfer_rectangle_y1;
            //   *
            //  /|
            // O-*
            *(transfer_rectangle_write_ptr++) = transfer_rectangle_x0;
            *(transfer_rectangle_write_ptr++) = transfer_rectangle_y1;
            //   O
            //  /|
            // *-*
            *(transfer_rectangle_write_ptr++) = transfer_rectangle_x1;
            *(transfer_rectangle_write_ptr++) = transfer_rectangle_y0;
            //   *
            //  /|
            // *-O
            *(transfer_rectangle_write_ptr++) = transfer_rectangle_x1;
            *(transfer_rectangle_write_ptr++) = transfer_rectangle_y1;
          }
        }
        command_processor_.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        command_list.D3DIASetVertexBuffers(0, 1, &transfer_rectangle_buffer_view);

        ID3D12PipelineState* const* transfer_pipelines =
            GetOrCreateTransferPipelines(transfer_shader_key);
        if (!transfer_pipelines) {
          continue;
        }
        if (last_transfer_root_signature_index != transfer_root_signature_index) {
          last_transfer_root_signature_index = transfer_root_signature_index;
          command_processor_.SetExternalGraphicsRootSignature(
              transfer_root_signatures_[size_t(transfer_root_signature_index)]);
          transfer_root_parameters_set = 0;
        }

        // Invalidate outdated bindings.
        if (transfer_root_parameters_used & kTransferUsedRootParameterColorSRVBit) {
          uint32_t descriptor_index_color = source_d3d12_rt.temporary_srv_descriptor_index();
          assert_true(descriptor_index_color != UINT32_MAX);
          if (last_descriptor_index_color != descriptor_index_color) {
            last_descriptor_index_color = descriptor_index_color;
            transfer_root_parameters_set &= ~kTransferUsedRootParameterColorSRVBit;
          }
        }
        if (transfer_root_parameters_used & kTransferUsedRootParameterDepthSRVBit) {
          uint32_t descriptor_index_depth = source_d3d12_rt.temporary_srv_descriptor_index();
          assert_true(descriptor_index_depth != UINT32_MAX);
          if (last_descriptor_index_depth != descriptor_index_depth) {
            last_descriptor_index_depth = descriptor_index_depth;
            transfer_root_parameters_set &= ~kTransferUsedRootParameterDepthSRVBit;
          }
        }
        if (transfer_root_parameters_used & kTransferUsedRootParameterStencilSRVBit) {
          uint32_t descriptor_index_stencil =
              source_d3d12_rt.temporary_srv_descriptor_index_stencil();
          assert_true(descriptor_index_stencil != UINT32_MAX);
          if (last_descriptor_index_stencil != descriptor_index_stencil) {
            last_descriptor_index_stencil = descriptor_index_stencil;
            transfer_root_parameters_set &= ~kTransferUsedRootParameterStencilSRVBit;
          }
        }
        if (transfer_root_parameters_used & kTransferUsedRootParameterHostDepthSRVBit) {
          if (transfer_shader_key.host_depth_source_is_copy) {
            if (!last_descriptor_host_depth_is_copy) {
              last_descriptor_host_depth_is_copy = true;
              transfer_root_parameters_set &= ~kTransferUsedRootParameterHostDepthSRVBit;
            }
          } else {
            assert_not_null(host_depth_source_d3d12_rt);
            uint32_t descriptor_index_host_depth =
                host_depth_source_d3d12_rt->temporary_srv_descriptor_index();
            assert_true(descriptor_index_host_depth != UINT32_MAX);
            if (last_descriptor_host_depth_is_copy ||
                last_descriptor_index_host_depth_non_copy != descriptor_index_host_depth) {
              transfer_root_parameters_set &= ~kTransferUsedRootParameterHostDepthSRVBit;
            }
            last_descriptor_host_depth_is_copy = false;
            last_descriptor_index_host_depth_non_copy = descriptor_index_host_depth;
          }
        }
        if (transfer_root_parameters_used & kTransferUsedRootParameterAddressConstantBit) {
          RenderTargetKey source_rt_key = source_d3d12_rt.key();
          TransferAddressConstant address_constant;
          address_constant.dest_pitch = dest_pitch_tiles;
          address_constant.source_pitch = source_rt_key.GetPitchTiles();
          address_constant.source_to_dest =
              int32_t(dest_rt_key.base_tiles) - int32_t(source_rt_key.base_tiles);
          if (last_address_constant != address_constant) {
            last_address_constant = address_constant;
            transfer_root_parameters_set &= ~kTransferUsedRootParameterAddressConstantBit;
          }
        }
        if (transfer_root_parameters_used & kTransferUsedRootParameterHostDepthAddressConstantBit) {
          assert_not_null(host_depth_source_d3d12_rt);
          RenderTargetKey host_depth_source_rt_key = host_depth_source_d3d12_rt->key();
          TransferAddressConstant host_depth_address_constant;
          host_depth_address_constant.dest_pitch = dest_pitch_tiles;
          host_depth_address_constant.source_pitch = host_depth_source_rt_key.GetPitchTiles();
          host_depth_address_constant.source_to_dest =
              int32_t(dest_rt_key.base_tiles) - int32_t(host_depth_source_rt_key.base_tiles);
          if (last_host_depth_address_constant != host_depth_address_constant) {
            last_host_depth_address_constant = host_depth_address_constant;
            transfer_root_parameters_set &= ~kTransferUsedRootParameterHostDepthAddressConstantBit;
          }
        }

        // Apply the new bindings.
        uint32_t transfer_root_parameters_unset =
            transfer_root_parameters_used & ~transfer_root_parameters_set;
        if (transfer_root_parameters_unset &
            kTransferUsedRootParameterHostDepthAddressConstantBit) {
          command_list.D3DSetGraphicsRoot32BitConstants(
              rex::bit_count(transfer_root_parameters_used &
                             (kTransferUsedRootParameterHostDepthAddressConstantBit - 1)),
              sizeof(last_host_depth_address_constant) / sizeof(uint32_t),
              &last_host_depth_address_constant, 0);
          transfer_root_parameters_set |= kTransferUsedRootParameterHostDepthAddressConstantBit;
        }
        if (transfer_root_parameters_unset & kTransferUsedRootParameterHostDepthSRVBit) {
          D3D12_GPU_DESCRIPTOR_HANDLE descriptor_gpu_handle;
          if (last_descriptor_host_depth_is_copy) {
            if (bindless_resources_used_) {
              descriptor_gpu_handle =
                  command_processor_
                      .GetSystemBindlessViewHandlePair(
                          D3D12CommandProcessor::SystemBindlessView ::kEdramR32UintSRV)
                      .second;
            } else {
              assert_true(host_depth_copy_srv_index != UINT32_MAX);
              descriptor_gpu_handle =
                  current_temporary_descriptors_gpu_[host_depth_copy_srv_index].second;
            }
          } else {
            assert_true(last_descriptor_index_host_depth_non_copy != UINT32_MAX);
            descriptor_gpu_handle =
                current_temporary_descriptors_gpu_[last_descriptor_index_host_depth_non_copy]
                    .second;
          }
          command_list.D3DSetGraphicsRootDescriptorTable(
              rex::bit_count(transfer_root_parameters_used &
                             (kTransferUsedRootParameterHostDepthSRVBit - 1)),
              descriptor_gpu_handle);
          transfer_root_parameters_set |= kTransferUsedRootParameterHostDepthSRVBit;
        }
        if (transfer_root_parameters_unset & kTransferUsedRootParameterAddressConstantBit) {
          command_list.D3DSetGraphicsRoot32BitConstants(
              rex::bit_count(transfer_root_parameters_used &
                             (kTransferUsedRootParameterAddressConstantBit - 1)),
              sizeof(last_address_constant) / sizeof(uint32_t), &last_address_constant, 0);
          transfer_root_parameters_set |= kTransferUsedRootParameterAddressConstantBit;
        }
        if (transfer_root_parameters_unset & kTransferUsedRootParameterStencilSRVBit) {
          assert_true(last_descriptor_index_stencil != UINT32_MAX);
          command_list.D3DSetGraphicsRootDescriptorTable(
              rex::bit_count(transfer_root_parameters_used &
                             (kTransferUsedRootParameterStencilSRVBit - 1)),
              current_temporary_descriptors_gpu_[last_descriptor_index_stencil].second);
          transfer_root_parameters_set |= kTransferUsedRootParameterStencilSRVBit;
        }
        if (transfer_root_parameters_unset & kTransferUsedRootParameterDepthSRVBit) {
          assert_true(last_descriptor_index_depth != UINT32_MAX);
          command_list.D3DSetGraphicsRootDescriptorTable(
              rex::bit_count(transfer_root_parameters_used &
                             (kTransferUsedRootParameterDepthSRVBit - 1)),
              current_temporary_descriptors_gpu_[last_descriptor_index_depth].second);
          transfer_root_parameters_set |= kTransferUsedRootParameterDepthSRVBit;
        }
        if (transfer_root_parameters_unset & kTransferUsedRootParameterColorSRVBit) {
          assert_true(last_descriptor_index_color != UINT32_MAX);
          command_list.D3DSetGraphicsRootDescriptorTable(
              rex::bit_count(transfer_root_parameters_used &
                             (kTransferUsedRootParameterColorSRVBit - 1)),
              current_temporary_descriptors_gpu_[last_descriptor_index_color].second);
          transfer_root_parameters_set |= kTransferUsedRootParameterColorSRVBit;
        }

        // Draw the transfer rectangles.
        command_processor_.SubmitBarriers();
        for (uint32_t j = 0; j <= uint32_t(is_stencil_bit) * 7; ++j) {
          if (is_stencil_bit) {
            uint32_t transfer_stencil_bit = uint32_t(1) << j;
            command_list.D3DSetGraphicsRoot32BitConstants(
                rex::bit_count(transfer_root_parameters_used &
                               (kTransferUsedRootParameterStencilMaskConstantBit - 1)),
                sizeof(transfer_stencil_bit) / sizeof(uint32_t), &transfer_stencil_bit, 0);
          }
          command_processor_.SetExternalPipeline(transfer_pipelines[j]);
          command_list.D3DDrawInstanced(transfer_vertex_count, 1, 0, 0);
        }
      }
    }

    // Perform the clear.
    if (resolve_clear_needed) {
      clear_rect.left =
          LONG(resolve_clear_rectangle->x_pixels * GetRenderTargetScaleX(dest_rt_key));
      clear_rect.top = LONG(resolve_clear_rectangle->y_pixels * GetRenderTargetScaleY(dest_rt_key));
      clear_rect.right =
          LONG((resolve_clear_rectangle->x_pixels + resolve_clear_rectangle->width_pixels) *
               GetRenderTargetScaleX(dest_rt_key));
      clear_rect.bottom =
          LONG((resolve_clear_rectangle->y_pixels + resolve_clear_rectangle->height_pixels) *
               GetRenderTargetScaleY(dest_rt_key));
      uint64_t clear_value = render_target_resolve_clear_values[i];
      if (dest_rt_key.is_depth) {
        uint32_t depth_guest_clear_value = (uint32_t(clear_value) >> 8) & 0xFFFFFF;
        float depth_host_clear_value = 0.0f;
        switch (dest_rt_key.GetDepthFormat()) {
          case xenos::DepthRenderTargetFormat::kD24S8:
            depth_host_clear_value = xenos::UNorm24To32(depth_guest_clear_value);
            break;
          case xenos::DepthRenderTargetFormat::kD24FS8:
            // Taking [0, 2) -> [0, 1) remapping into account.
            depth_host_clear_value = xenos::Float20e4To32(depth_guest_clear_value) * 0.5f;
            break;
        }
        command_processor_.PushTransitionBarrier(
            dest_d3d12_rt.resource(),
            dest_d3d12_rt.SetResourceState(D3D12_RESOURCE_STATE_DEPTH_WRITE),
            D3D12_RESOURCE_STATE_DEPTH_WRITE);
        command_processor_.SubmitBarriers();
        command_list.D3DClearDepthStencilView(dest_d3d12_rt.descriptor_draw().GetHandle(),
                                              D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
                                              depth_host_clear_value, UINT(clear_value) & 0xFF, 1,
                                              &clear_rect);
      } else {
        float color_clear_value[4] = {};
        bool clear_via_drawing = false;
        switch (dest_rt_key.GetColorFormat()) {
          case xenos::ColorRenderTargetFormat::k_8_8_8_8: {
            for (uint32_t j = 0; j < 4; ++j) {
              color_clear_value[j] = ((clear_value >> (j * 8)) & 0xFF) * (1.0f / 0xFF);
            }
          } break;
          case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA: {
            // 8_8_8_8_GAMMA is represented by linear stored in
            // R16G16B16A16_UNORM.
            for (uint32_t j = 0; j < 4; ++j) {
              color_clear_value[j] = ((clear_value >> (j * 8)) & 0xFF) * (1.0f / 0xFF);
            }
            for (uint32_t j = 0; j < 3; ++j) {
              color_clear_value[j] = xenos::PWLGammaToLinear(color_clear_value[j]);
            }
          } break;
          case xenos::ColorRenderTargetFormat::k_2_10_10_10:
          case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10: {
            for (uint32_t j = 0; j < 3; ++j) {
              color_clear_value[j] = ((clear_value >> (j * 10)) & 0x3FF) * (1.0f / 0x3FF);
            }
            color_clear_value[3] = ((clear_value >> 30) & 0x3) * (1.0f / 0x3);
          } break;
          case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
          case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16: {
            for (uint32_t j = 0; j < 3; ++j) {
              color_clear_value[j] = xenos::Float7e3To32((clear_value >> (j * 10)) & 0x3FF);
            }
            color_clear_value[3] = ((clear_value >> 30) & 0x3) * (1.0f / 0x3);
          } break;
          case xenos::ColorRenderTargetFormat::k_16_16:
          case xenos::ColorRenderTargetFormat::k_16_16_FLOAT: {
            // Using uint for loading both. Disregarding the current -32...32
            // vs. -1...1 settings for consistency with color clear via depth
            // aliasing.
            for (uint32_t j = 0; j < 2; ++j) {
              color_clear_value[j] = float((clear_value >> (j * 16)) & 0xFFFF);
            }
          } break;
          case xenos::ColorRenderTargetFormat::k_16_16_16_16:
          case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT: {
            // Using uint for loading both. Disregarding the current -32...32
            // vs. -1...1 settings for consistency with color clear via depth
            // aliasing.
            for (uint32_t j = 0; j < 4; ++j) {
              color_clear_value[j] = float((clear_value >> (j * 16)) & 0xFFFF);
            }
          } break;
          case xenos::ColorRenderTargetFormat::k_32_FLOAT: {
            // Using uint for proper denormal and NaN handling.
            color_clear_value[0] = float(uint32_t(clear_value));
            // Numbers > 2^24 can't be represented with a step of 1 as floats,
            // need to clear by drawing a uint rectangle.
            if (uint64_t(color_clear_value[0]) != uint32_t(clear_value)) {
              clear_via_drawing = true;
            }
          } break;
          case xenos::ColorRenderTargetFormat::k_32_32_FLOAT: {
            // Using uint for proper denormal and NaN handling.
            color_clear_value[0] = float(uint32_t(clear_value));
            color_clear_value[1] = float(uint32_t(clear_value >> 32));
            // Numbers > 2^24 can't be represented with a step of 1 as floats,
            // need to clear by drawing a uint rectangle.
            if (uint64_t(color_clear_value[0]) != uint32_t(clear_value) ||
                uint64_t(color_clear_value[1]) != uint32_t(clear_value >> 32)) {
              clear_via_drawing = true;
            }
          } break;
        }
        command_processor_.PushTransitionBarrier(
            dest_d3d12_rt.resource(),
            dest_d3d12_rt.SetResourceState(D3D12_RESOURCE_STATE_RENDER_TARGET),
            D3D12_RESOURCE_STATE_RENDER_TARGET);
        if (clear_via_drawing) {
          D3D12_CPU_DESCRIPTOR_HANDLE clear_rtv_handle =
              dest_d3d12_rt.descriptor_load_separate().IsValid()
                  ? dest_d3d12_rt.descriptor_load_separate().GetHandle()
                  : dest_d3d12_rt.descriptor_draw().GetHandle();
          command_list.D3DOMSetRenderTargets(1, &clear_rtv_handle, FALSE, nullptr);
          are_current_command_list_render_targets_valid_ = true;
          D3D12_VIEWPORT clear_viewport;
          clear_viewport.TopLeftX = float(clear_rect.left);
          clear_viewport.TopLeftY = float(clear_rect.top);
          clear_viewport.Width = float(clear_rect.right - clear_rect.left);
          clear_viewport.Height = float(clear_rect.bottom - clear_rect.top);
          clear_viewport.MinDepth = 0.0f;
          clear_viewport.MaxDepth = 1.0f;
          command_processor_.SetViewport(clear_viewport);
          command_processor_.SetScissorRect(clear_rect);
          command_processor_.SetExternalGraphicsRootSignature(uint32_rtv_clear_root_signature_);
          uint32_t clear_via_drawing_value[2] = {uint32_t(clear_value),
                                                 uint32_t(clear_value >> 32)};
          command_list.D3DSetGraphicsRoot32BitConstants(0, 2, clear_via_drawing_value, 0);
          command_processor_.SetExternalPipeline(uint32_rtv_clear_pipelines_[size_t(
              dest_rt_key.GetColorFormat() ==
              xenos::ColorRenderTargetFormat::k_32_32_FLOAT)][size_t(dest_rt_key.msaa_samples)]);
          command_processor_.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
          command_list.D3DDrawInstanced(3, 1, 0, 0);
        } else {
          command_processor_.SubmitBarriers();
          command_list.D3DClearRenderTargetView(
              dest_d3d12_rt.descriptor_load_separate().IsValid()
                  ? dest_d3d12_rt.descriptor_load_separate().GetHandle()
                  : dest_d3d12_rt.descriptor_draw().GetHandle(),
              color_clear_value, 1, &clear_rect);
        }
      }
    }
  }
}

void D3D12RenderTargetCache::SetCommandListRenderTargets(
    RenderTarget* const* depth_and_color_render_targets) {
  assert_true(GetPath() == Path::kHostRenderTargets);

  // Ensure the render targets are in the needed resource state.
  if (depth_and_color_render_targets[0]) {
    auto& d3d12_rt = *static_cast<D3D12RenderTarget*>(depth_and_color_render_targets[0]);
    command_processor_.PushTransitionBarrier(
        d3d12_rt.resource(), d3d12_rt.SetResourceState(D3D12_RESOURCE_STATE_DEPTH_WRITE),
        D3D12_RESOURCE_STATE_DEPTH_WRITE);
  }
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    RenderTarget* render_target = depth_and_color_render_targets[1 + i];
    if (!render_target) {
      continue;
    }
    auto& d3d12_rt = *static_cast<D3D12RenderTarget*>(render_target);
    command_processor_.PushTransitionBarrier(
        d3d12_rt.resource(), d3d12_rt.SetResourceState(D3D12_RESOURCE_STATE_RENDER_TARGET),
        D3D12_RESOURCE_STATE_RENDER_TARGET);
  }

  // Bind the render targets.
  if (are_current_command_list_render_targets_valid_ &&
      std::memcmp(current_command_list_render_targets_, depth_and_color_render_targets,
                  sizeof(current_command_list_render_targets_))) {
    are_current_command_list_render_targets_valid_ = false;
  }
  if (!are_current_command_list_render_targets_valid_) {
    std::memcpy(current_command_list_render_targets_, depth_and_color_render_targets,
                sizeof(current_command_list_render_targets_));
    D3D12_CPU_DESCRIPTOR_HANDLE dsv_handle;
    if (depth_and_color_render_targets[0]) {
      dsv_handle = static_cast<const D3D12RenderTarget*>(depth_and_color_render_targets[0])
                       ->descriptor_draw()
                       .GetHandle();
    }
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_handles[xenos::kMaxColorRenderTargets];
    uint32_t rtv_count = 0;
    for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
      const RenderTarget* render_target = depth_and_color_render_targets[1 + i];
      if (!render_target) {
        continue;
      }
      // Fill the gaps with a null descriptor.
      while (rtv_count < i) {
        rtv_handles[rtv_count++] = render_target->key().msaa_samples != xenos::MsaaSamples::k1X
                                       ? null_rtv_descriptor_ms_.GetHandle()
                                       : null_rtv_descriptor_ss_.GetHandle();
      }
      auto& d3d12_rt = *static_cast<const D3D12RenderTarget*>(render_target);
      rtv_handles[rtv_count++] = d3d12_rt.descriptor_draw().GetHandle();
    }
    command_processor_.GetDeferredCommandList().D3DOMSetRenderTargets(
        rtv_count, rtv_handles, FALSE, depth_and_color_render_targets[0] ? &dsv_handle : nullptr);
    are_current_command_list_render_targets_valid_ = true;
  }
}

ID3D12PipelineState* D3D12RenderTargetCache::GetOrCreateDumpPipeline(DumpPipelineKey key) {
  auto pipeline_it = dump_pipelines_.find(key);
  if (pipeline_it != dump_pipelines_.end()) {
    return pipeline_it->second;
  }

  // Because of built_shader_.resize(), pointers can't be kept persistently
  // here! Resizing also zeroes the memory.

  built_shader_.clear();

  // RDEF, ISGN, OSGN, SHEX, STAT.
  constexpr uint32_t kBlobCount = 5;

  // Allocate space for the container header and the blob offsets.
  built_shader_.resize(sizeof(dxbc::ContainerHeader) / sizeof(uint32_t) + kBlobCount);
  uint32_t blob_offset_position_dwords = sizeof(dxbc::ContainerHeader) / sizeof(uint32_t);
  uint32_t blob_position_dwords = uint32_t(built_shader_.size());
  constexpr uint32_t kBlobHeaderSizeDwords = sizeof(dxbc::BlobHeader) / sizeof(uint32_t);

  uint32_t name_ptr;

  // ***************************************************************************
  // Resource definition
  // ***************************************************************************

  built_shader_[blob_offset_position_dwords] = uint32_t(blob_position_dwords * sizeof(uint32_t));
  uint32_t rdef_position_dwords = blob_position_dwords + kBlobHeaderSizeDwords;
  // Not needed, as the next operation done is resize, to allocate the space for
  // both the blob header and the resource definition header.
  // built_shader_.resize(rdef_position_dwords);

  // Allocate space for the RDEF header.
  built_shader_.resize(rdef_position_dwords + sizeof(dxbc::RdefHeader) / sizeof(uint32_t));
  // Generator name.
  dxbc::AppendAlignedString(built_shader_, "Xenia");

  // Constant types - uint (aka "dword" when it's scalar) only.
  // Names.
  name_ptr = uint32_t((built_shader_.size() - rdef_position_dwords) * sizeof(uint32_t));
  uint32_t rdef_dword_name_ptr = name_ptr;
  name_ptr += dxbc::AppendAlignedString(built_shader_, "dword");
  // Types.
  uint32_t rdef_type_uint_position_dwords = uint32_t(built_shader_.size());
  uint32_t rdef_type_uint_ptr =
      uint32_t((rdef_type_uint_position_dwords - rdef_position_dwords) * sizeof(uint32_t));
  built_shader_.resize(rdef_type_uint_position_dwords + sizeof(dxbc::RdefType) / sizeof(uint32_t));
  {
    auto& rdef_type_uint =
        *reinterpret_cast<dxbc::RdefType*>(built_shader_.data() + rdef_type_uint_position_dwords);
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
  name_ptr = uint32_t((built_shader_.size() - rdef_position_dwords) * sizeof(uint32_t));
  uint32_t rdef_xe_edram_dump_offsets_name_ptr = name_ptr;
  name_ptr += dxbc::AppendAlignedString(built_shader_, "xe_edram_dump_offsets");
  uint32_t rdef_xe_edram_dump_pitches_name_ptr = name_ptr;
  name_ptr += dxbc::AppendAlignedString(built_shader_, "xe_edram_dump_pitches");
  // Constants.
  uint32_t rdef_constants_position_dwords = uint32_t(built_shader_.size());
  uint32_t rdef_constants_ptr =
      uint32_t((rdef_constants_position_dwords - rdef_position_dwords) * sizeof(uint32_t));
  built_shader_.resize(rdef_constants_position_dwords +
                       sizeof(dxbc::RdefVariable) / sizeof(uint32_t) * kConstantCount);
  {
    auto rdef_constants = reinterpret_cast<dxbc::RdefVariable*>(built_shader_.data() +
                                                                rdef_constants_position_dwords);
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
  uint32_t rdef_cbuffer_position_dwords = uint32_t(built_shader_.size());
  built_shader_.resize(rdef_cbuffer_position_dwords +
                       sizeof(dxbc::RdefCbuffer) / sizeof(uint32_t) * kDumpCbufferCount);
  {
    auto rdef_cbuffers =
        reinterpret_cast<dxbc::RdefCbuffer*>(built_shader_.data() + rdef_cbuffer_position_dwords);
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
  name_ptr = uint32_t((built_shader_.size() - rdef_position_dwords) * sizeof(uint32_t));
  uint32_t rdef_xe_edram_dump_source_name_ptr = name_ptr;
  name_ptr += dxbc::AppendAlignedString(built_shader_, "xe_edram_dump_source");
  uint32_t rdef_xe_edram_dump_stencil_name_ptr = name_ptr;
  if (key.is_depth) {
    name_ptr += dxbc::AppendAlignedString(built_shader_, "xe_edram_dump_stencil");
  }
  uint32_t rdef_xe_edram_name_ptr = name_ptr;
  name_ptr += dxbc::AppendAlignedString(built_shader_, "xe_edram");
  // Bindings.
  uint32_t rdef_binding_position_dwords = uint32_t(built_shader_.size());
  built_shader_.resize(rdef_binding_position_dwords +
                       sizeof(dxbc::RdefInputBind) / sizeof(uint32_t) * rdef_binding_count);
  bool source_is_uint;
  if (key.is_depth) {
    source_is_uint = false;
  } else {
    GetColorOwnershipTransferDXGIFormat(key.GetColorFormat(), &source_is_uint);
  }
  dxbc::ResourceReturnType source_return_type =
      source_is_uint ? dxbc::ResourceReturnType::kUInt : dxbc::ResourceReturnType::kFloat;
  uint32_t source_component_count =
      key.is_depth ? 1 : xenos::GetColorRenderTargetFormatComponentCount(key.GetColorFormat());
  bool format_is_64bpp =
      !key.is_depth && xenos::IsColorRenderTargetFormat64bpp(key.GetColorFormat());
  {
    auto rdef_bindings =
        reinterpret_cast<dxbc::RdefInputBind*>(built_shader_.data() + rdef_binding_position_dwords);
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
    auto& rdef_header =
        *reinterpret_cast<dxbc::RdefHeader*>(built_shader_.data() + rdef_position_dwords);
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
    auto& blob_header =
        *reinterpret_cast<dxbc::BlobHeader*>(built_shader_.data() + blob_position_dwords);
    blob_header.fourcc = dxbc::BlobHeader::FourCC::kResourceDefinition;
    blob_position_dwords = uint32_t(built_shader_.size());
    blob_header.size_bytes = (blob_position_dwords - kBlobHeaderSizeDwords) * sizeof(uint32_t) -
                             built_shader_[blob_offset_position_dwords++];
  }

  // ***************************************************************************
  // Input and output signatures (empty)
  // ***************************************************************************

  for (uint32_t i = 0; i < 2; ++i) {
    built_shader_[blob_offset_position_dwords] = uint32_t(blob_position_dwords * sizeof(uint32_t));
    uint32_t signature_position_dwords = blob_position_dwords + kBlobHeaderSizeDwords;
    built_shader_.resize(signature_position_dwords + sizeof(dxbc::Signature) / sizeof(uint32_t));
    {
      auto& signature =
          *reinterpret_cast<dxbc::Signature*>(built_shader_.data() + signature_position_dwords);
      // Empty - just set parameter pointer to the end.
      signature.parameter_info_ptr = sizeof(dxbc::Signature);
    }
    {
      auto& blob_header =
          *reinterpret_cast<dxbc::BlobHeader*>(built_shader_.data() + blob_position_dwords);
      blob_header.fourcc = i ? dxbc::BlobHeader::FourCC::kOutputSignature
                             : dxbc::BlobHeader::FourCC::kInputSignature;
      blob_position_dwords = uint32_t(built_shader_.size());
      blob_header.size_bytes = (blob_position_dwords - kBlobHeaderSizeDwords) * sizeof(uint32_t) -
                               built_shader_[blob_offset_position_dwords++];
    }
  }

  // ***************************************************************************
  // Shader program
  // ***************************************************************************

  built_shader_[blob_offset_position_dwords] = uint32_t(blob_position_dwords * sizeof(uint32_t));
  uint32_t shex_position_dwords = blob_position_dwords + kBlobHeaderSizeDwords;
  built_shader_.resize(shex_position_dwords);

  built_shader_.push_back(dxbc::VersionToken(dxbc::ProgramType::kComputeShader, 5, 1));
  // Reserve space for the length token.
  built_shader_.push_back(0);

  dxbc::Statistics stat;
  std::memset(&stat, 0, sizeof(dxbc::Statistics));
  dxbc::Assembler a(built_shader_, stat);

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
  stat.temp_register_count = 2;
  a.OpDclTemps(stat.temp_register_count);
  // There's no strict dependency on the group size here, for simplicity of
  // calculations especially with resolution scaling, dividing manually (as the
  // group size is not unlimited). The only restriction is that an integer
  // multiple of it must be 80x16 samples (and no larger than that) for 32bpp,
  // or 40x16 samples for 64bpp (because only a half of the pair of tiles may
  // need to be dumped). The group size limit in Direct3D 11 is 1024, and 40x16
  // fits in it, while 80x16 doesn't.
  a.OpDclThreadGroup(40, 16, 1);

  uint32_t draw_resolution_scale_x = this->draw_resolution_scale_x();
  uint32_t draw_resolution_scale_y = this->draw_resolution_scale_y();

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
      a.OpMovC(dxbc::Dest::R(0, 0b1000), dxbc::Src::R(0, dxbc::Src::kWWWW),
               dxbc::Src::LU(draw_util::GetD3D10SampleIndexForGuest2xMSAA(1, msaa_2x_supported_)),
               dxbc::Src::LU(draw_util::GetD3D10SampleIndexForGuest2xMSAA(0, msaa_2x_supported_)));
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
        DxbcShaderTranslator::PreClampedDepthTo20e4(
            a, 1, 0, 1, 0, 0, 0, !depth_float24_convert_in_pixel_shader() && depth_float24_round(),
            true);
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
          DxbcShaderTranslator::PreSaturatedLinearToPWLGamma(a, 1, i, 1, i, 0, 0, 0, 1);
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
        DxbcShaderTranslator::UnclampedFloat32To7e3(a, 1, 0, 1, 0, 0, 0);
        for (uint32_t i = 1; i < 3; ++i) {
          // Convert green and blue to a temporary register r0.x using r0.y
          // as an internal temporary, then insert them into the result in
          // r1.x.
          DxbcShaderTranslator::UnclampedFloat32To7e3(a, 0, 0, 1, i, 0, 1);
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
  built_shader_[shex_position_dwords + 1] = uint32_t(built_shader_.size()) - shex_position_dwords;

  {
    auto& blob_header =
        *reinterpret_cast<dxbc::BlobHeader*>(built_shader_.data() + blob_position_dwords);
    blob_header.fourcc = dxbc::BlobHeader::FourCC::kShaderEx;
    blob_position_dwords = uint32_t(built_shader_.size());
    blob_header.size_bytes = (blob_position_dwords - kBlobHeaderSizeDwords) * sizeof(uint32_t) -
                             built_shader_[blob_offset_position_dwords++];
  }

  // ***************************************************************************
  // Statistics
  // ***************************************************************************

  built_shader_[blob_offset_position_dwords] = uint32_t(blob_position_dwords * sizeof(uint32_t));
  uint32_t stat_position_dwords = blob_position_dwords + kBlobHeaderSizeDwords;
  built_shader_.resize(stat_position_dwords + sizeof(dxbc::Statistics) / sizeof(uint32_t));
  std::memcpy(built_shader_.data() + stat_position_dwords, &stat, sizeof(dxbc::Statistics));
  {
    auto& blob_header =
        *reinterpret_cast<dxbc::BlobHeader*>(built_shader_.data() + blob_position_dwords);
    blob_header.fourcc = dxbc::BlobHeader::FourCC::kStatistics;
    blob_position_dwords = uint32_t(built_shader_.size());
    blob_header.size_bytes = (blob_position_dwords - kBlobHeaderSizeDwords) * sizeof(uint32_t) -
                             built_shader_[blob_offset_position_dwords++];
  }

  // ***************************************************************************
  // Container header
  // ***************************************************************************

  uint32_t built_shader_size_bytes = uint32_t(built_shader_.size() * sizeof(uint32_t));
  {
    auto& container_header = *reinterpret_cast<dxbc::ContainerHeader*>(built_shader_.data());
    container_header.InitializeIdentification();
    container_header.size_bytes = built_shader_size_bytes;
    container_header.blob_count = kBlobCount;
    CalculateDXBCChecksum(reinterpret_cast<unsigned char*>(built_shader_.data()),
                          static_cast<unsigned int>(built_shader_size_bytes),
                          reinterpret_cast<unsigned int*>(&container_header.hash));
  }

  // ***************************************************************************
  // Pipeline
  // ***************************************************************************
  ID3D12PipelineState* pipeline = ui::d3d12::util::CreateComputePipeline(
      command_processor_.GetD3D12Provider().GetDevice(), built_shader_.data(),
      built_shader_size_bytes,
      key.is_depth ? dump_root_signature_depth_ : dump_root_signature_color_);
  const char* format_name = key.is_depth
                                ? xenos::GetDepthRenderTargetFormatName(key.GetDepthFormat())
                                : xenos::GetColorRenderTargetFormatName(key.GetColorFormat());
  if (pipeline) {
    std::u16string pipeline_name = rex::string::to_utf16(
        fmt::format("RT Dump {} {}xMSAA", format_name, uint32_t(1) << uint32_t(key.msaa_samples)));
    pipeline->SetName(reinterpret_cast<LPCWSTR>(pipeline_name.c_str()));
  } else {
    REXGPU_ERROR(
        "D3D12RenderTargetCache: Failed to create a render target dumping "
        "pipeline for {}-sample render targets with format {}",
        uint32_t(1) << uint32_t(key.msaa_samples), format_name);
  }
  // Even if creation fails, still store the null pointer not to try to create
  // again.
  dump_pipelines_.emplace(key, pipeline);
  return pipeline;
}

bool D3D12RenderTargetCache::GetOrCreateNativeResolvePipeline(bool depth, bool integer_source,
                                                              bool multisampled) {
  uint32_t variant = uint32_t(integer_source) | (uint32_t(multisampled) << 1);
  auto& attempted =
      depth ? native_depth_copy_attempted_[multisampled] : native_color_copy_attempted_[variant];
  auto& root_signature =
      depth ? native_depth_copy_root_signature_ : native_color_copy_root_signature_;
  auto& pipeline =
      depth ? native_depth_copy_pipeline_[multisampled] : native_color_copy_pipeline_[variant];
  if (attempted) {
    return pipeline != nullptr;
  }
  attempted = true;

  // The compiler is optional at runtime. Keep the existing precompiled resolve
  // path available when this shader cannot be compiled or created.
  HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
  auto compile =
      compiler ? reinterpret_cast<decltype(&D3DCompile)>(GetProcAddress(compiler, "D3DCompile"))
               : nullptr;
  ID3DBlob* code = nullptr;
  ID3DBlob* errors = nullptr;
  std::string source = depth ? draw_util::kNativeDepthResolve : draw_util::kNativeColorResolve;
  if (!depth && integer_source)
    source.insert(0, "#define NATIVE_COLOR_UINT\n");
  if (multisampled)
    source.insert(0, "#define NATIVE_RESOLVE_MSAA\n");
  HRESULT result = compile ? compile(source.data(), source.size(),
                                     depth ? "native_depth_resolve" : "native_color_resolve",
                                     nullptr, nullptr, "main", "cs_5_0",
                                     D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3,
                                     0, &code, &errors)
                           : E_NOINTERFACE;
  if (FAILED(result)) {
    REXGPU_WARN("D3D12 native {} resolve shader unavailable: {:08X} {}", depth ? "depth" : "color",
                uint32_t(result),
                errors ? std::string(static_cast<const char*>(errors->GetBufferPointer()),
                                     errors->GetBufferSize())
                       : "D3DCompiler_47.dll is unavailable");
  } else {
    auto& provider = command_processor_.GetD3D12Provider();
    D3D12_DESCRIPTOR_RANGE ranges[3] = {};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[0].NumDescriptors = 1;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[1].NumDescriptors = 1;
    // One-use bindless descriptors are recycled independently, so the depth
    // and stencil views are not necessarily adjacent. Give each its own table
    // instead of overwriting the descriptor following the depth view.
    ranges[2].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[2].NumDescriptors = 1;
    ranges[2].BaseShaderRegister = 1;
    D3D12_ROOT_PARAMETER parameters[4] = {};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants.Num32BitValues = 20;
    for (uint32_t i = 0; i < (depth ? 3u : 2u); ++i) {
      parameters[i + 1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
      parameters[i + 1].DescriptorTable.NumDescriptorRanges = 1;
      parameters[i + 1].DescriptorTable.pDescriptorRanges = &ranges[i];
    }
    D3D12_ROOT_SIGNATURE_DESC description = {};
    description.NumParameters = depth ? 4 : 3;
    description.pParameters = parameters;
    if (!root_signature)
      root_signature = ui::d3d12::util::CreateRootSignature(provider, description);
    if (root_signature) {
      pipeline = ui::d3d12::util::CreateComputePipeline(
          provider.GetDevice(), code->GetBufferPointer(), code->GetBufferSize(), root_signature);
    }
    if (!pipeline) {
      REXGPU_WARN("D3D12 native {} resolve pipeline unavailable; using the general resolve",
                  depth ? "depth" : "color");
    }
  }
  ui::d3d12::util::ReleaseAndNull(errors);
  ui::d3d12::util::ReleaseAndNull(code);
  if (compiler) {
    FreeLibrary(compiler);
  }
  return pipeline != nullptr;
}

RenderTargetCache::RenderTarget* D3D12RenderTargetCache::PrepareNativeResolveAlias(
    const draw_util::ResolveInfo& resolve, uint32_t base, uint32_t row_length, uint32_t rows,
    uint32_t pitch) {
  NativeResolveOwnershipPlan plan;
  if (!PlanNativeResolveOwnership(resolve, base, row_length, rows, pitch, plan, true))
    return nullptr;
  const auto dest_key = plan.destination;
  const bool depth = dest_key.is_depth;
  auto* dest = static_cast<D3D12RenderTarget*>(GetOrCreateRenderTarget(dest_key));
  bool integer = !depth && draw_util::NativeColorResolveUsesIntegerSource(
                               draw_util::GetNativeColorResolvePacking(dest_key.GetColorFormat()));
  if (!dest || !GetOrCreateNativeResolvePipeline(depth, integer,
                                                 dest_key.msaa_samples != xenos::MsaaSamples::k1X))
    return nullptr;
  // Preflight every source before decoding the temporary view. No source is decoded
  // through the EDRAM buffer, including mixed format/base/pitch owners.
  for (const auto& transfer : plan.transfers) {
    const auto source_key = transfer.source->key();
    TransferShaderKey shader_key;
    shader_key.dest_msaa_samples = dest_key.msaa_samples;
    shader_key.dest_resource_format = dest_key.resource_format;
    shader_key.source_msaa_samples = source_key.msaa_samples;
    shader_key.source_resource_format = source_key.resource_format;
    shader_key.dest_original_resolution = dest_key.original_resolution;
    shader_key.source_original_resolution = source_key.original_resolution;
    shader_key.host_depth_source_original_resolution =
        transfer.host_depth_source ? transfer.host_depth_source->key().original_resolution : 0;
    shader_key.mode =
        GetTransferMode(false, depth, source_key.is_depth, transfer.host_depth_source != nullptr);
    if (transfer.host_depth_source)
      shader_key.host_depth_source_msaa_samples = transfer.host_depth_source->key().msaa_samples;
    if (!GetOrCreateTransferPipelines(shader_key))
      return nullptr;
    if (depth && !use_stencil_reference_output_) {
      shader_key.mode = GetTransferMode(true, true, source_key.is_depth, false);
      shader_key.host_depth_source_msaa_samples = xenos::MsaaSamples::k1X;
      if (!GetOrCreateTransferPipelines(shader_key))
        return nullptr;
    }
  }
  // Resolving is a read. Keep the original owners and host depth values;
  // publishing this decoded view would accumulate format-rounding losses.
  const auto& transfers = plan.transfers;
  RenderTarget* targets[] = {dest};
  PerformTransfersAndResolveClears(1, targets, &transfers);
  GetResolveCopyRectanglesToDump(base, row_length, rows, pitch, dump_rectangles_);
  command_processor_.native_gpu_diagnostics().RecordOperation(
      NativeGpuDiagnostics::Operation::kAliasTransfer);
  return dest;
}

void D3D12RenderTargetCache::PlanNativeResolveCopy(const draw_util::ResolveInfo& resolve,
                                                   D3D12TextureCache& textures,
                                                   NativeResolveCopy& copy) {
  // Native packing preserves matching single-sampled attachment formats. A
  // partial rectangle updates compact resolve storage; whole compatible
  // textures can additionally receive an image copy.
  bool depth = resolve.IsCopyingDepth();
  const draw_util::ResolveEdramInfo& edram =
      depth ? resolve.depth_edram_info : resolve.color_edram_info;
  auto reject = [&](const char* reason) {
    if (REXCVAR_GET(d3d12_native_resolve_debug)) {
      static uint32_t logged = 0;
      if (logged++ < 64) {
        const auto& info = resolve.IsCopyingDepth() ? resolve.depth_edram_info : edram;
        REXGPU_INFO(
            "[d3d12-resolve-fallback] {}: depth {}, source format {}, destination format {}, "
            "MSAA {}, origin {}x{}, size {}x{}, destination {:08X}",
            reason, resolve.IsCopyingDepth(), uint32_t(info.format),
            uint32_t(resolve.copy_dest_info.copy_dest_format), uint32_t(info.msaa_samples),
            resolve.rect_x0, resolve.rect_y0, uint32_t(resolve.coordinate_info.width_div_8) << 3,
            resolve.height_div_8 << 3, resolve.copy_dest_base);
      }
    }
  };
  if ((depth &&
       (!REXCVAR_GET(d3d12_native_depth_resolves) || resolve.copy_dest_info.copy_dest_swap)) ||
      edram.msaa_samples > xenos::MsaaSamples::k4X ||
      uint32_t(resolve.copy_dest_info.copy_dest_endian) > uint32_t(xenos::Endian128::k8in128)) {
    reject("copy parameters");
    return;
  }
  auto guest_format = xenos::ColorRenderTargetFormat(edram.format);
  auto depth_format = xenos::DepthRenderTargetFormat(edram.format);
  if (depth ? (uint32_t(depth_format) > uint32_t(xenos::DepthRenderTargetFormat::kD24FS8) ||
               uint32_t(resolve.copy_dest_info.copy_dest_format) !=
                   (depth_format == xenos::DepthRenderTargetFormat::kD24FS8
                        ? uint32_t(xenos::TextureFormat::k_24_8_FLOAT)
                        : uint32_t(xenos::TextureFormat::k_24_8)))
            : (draw_util::GetNativeColorResolvePacking(
                   xenos::ColorFormat(resolve.copy_dest_info.copy_dest_format)) ==
               draw_util::NativeColorResolveDestination::kUnsupported)) {
    reject("format conversion");
    return;
  }
  uint32_t base, row_length, rows, pitch;
  resolve.GetCopyEdramTileSpan(base, row_length, rows, pitch);
  GetResolveCopyRectanglesToDump(base, row_length, rows, pitch, dump_rectangles_);
  auto* native_source = PrepareNativeResolveAlias(resolve, base, row_length, rows, pitch);
  if (!native_source)
    native_source = FindNativeResolveSource(base, row_length, rows, pitch);
  if (!native_source) {
    reject("source tiles missing or have different owners");
    return;
  }
  auto& source = *static_cast<D3D12RenderTarget*>(native_source);
  RenderTargetKey key = source.key();
  xenos::ColorRenderTargetFormat source_format =
      key.is_depth ? xenos::ColorRenderTargetFormat::k_8_8_8_8 : key.GetColorFormat();
  if (bool(key.is_depth) != depth ||
      key.base_tiles != (depth ? resolve.depth_original_base : resolve.color_original_base) ||
      key.msaa_samples != edram.msaa_samples || key.GetPitchTiles() != edram.pitch_tiles ||
      (depth ? key.GetDepthFormat() != depth_format
             : draw_util::GetNativeColorResolvePacking(source_format) ==
                   draw_util::NativeColorResolvePacking::kUnsupported)) {
    reject("source format or layout");
    if (REXCVAR_GET(d3d12_native_resolve_debug)) {
      REXGPU_INFO(
          "[d3d12-resolve-source] depth {}, format {}, base {}, pitch {}, MSAA {}; requested "
          "depth {}, format {}, base {}, pitch {}, MSAA {}",
          bool(key.is_depth),
          key.is_depth ? uint32_t(key.GetDepthFormat()) : uint32_t(key.GetColorFormat()),
          uint32_t(key.base_tiles), key.GetPitchTiles(), uint32_t(key.msaa_samples), depth,
          uint32_t(edram.format), depth ? resolve.depth_original_base : resolve.color_original_base,
          uint32_t(edram.pitch_tiles), uint32_t(edram.msaa_samples));
    }
    return;
  }
  D3D12_RESOURCE_DESC source_desc = source.resource()->GetDesc();
  auto packing = draw_util::GetNativeColorResolvePacking(source_format);
  uint32_t scale_x = GetRenderTargetScaleX(key), scale_y = GetRenderTargetScaleY(key);
  uint32_t x1 = resolve.rect_x0 + (uint32_t(resolve.coordinate_info.width_div_8)
                                   << xenos::kResolveAlignmentPixelsLog2);
  uint32_t y1 = resolve.rect_y0 + (resolve.height_div_8 << xenos::kResolveAlignmentPixelsLog2);
  uint32_t dest_pitch = uint32_t(resolve.copy_dest_coordinate_info.pitch_aligned_div_32)
                        << xenos::kTextureTileWidthHeightLog2;
  auto source_layout = draw_util::GetNativeResolveSourceLayout(
      resolve.rect_x0, resolve.rect_y0, x1 - resolve.rect_x0, y1 - resolve.rect_y0, scale_x,
      scale_y, uint32_t(source_desc.Width), source_desc.Height, edram.msaa_samples,
      key.msaa_samples, !depth && key.Is64bpp(), edram.pitch_tiles);
  if (!x1 || !y1 || x1 > dest_pitch || !source_layout ||
      (depth ? (source_desc.Format != DXGI_FORMAT_R32G8X24_TYPELESS &&
                source_desc.Format != DXGI_FORMAT_R24G8_TYPELESS)
             : source_desc.Format != GetColorResourceDXGIFormat(source_format))) {
    reject("host extent or format");
    return;
  }
  copy.source = &source;
  copy.scale_x = scale_x;
  copy.scale_y = scale_y;
  copy.color_packing = uint32_t(packing);
  copy.source_layout = source_layout.packed;
  copy.integer_source = !depth && draw_util::NativeColorResolveUsesIntegerSource(packing);
  copy.red_blue_swapped = resolve.copy_dest_info.copy_dest_swap != 0;
  if (depth || (source_layout.packed & 0x80000000u) ||
      key.msaa_samples != xenos::MsaaSamples::k1X ||
      !xenos::IsColorResolveFormatBitwiseEquivalent(
          source_format, xenos::ColorFormat(resolve.copy_dest_info.copy_dest_format)) ||
      resolve.rect_x0 || resolve.rect_y0 || resolve.copy_dest_info.copy_dest_array ||
      resolve.copy_dest_info.copy_dest_exp_bias || copy.integer_source ||
      packing >= draw_util::NativeColorResolvePacking::kSint16x2 ||
      uint32_t(resolve.copy_dest_info.copy_dest_endian) > uint32_t(xenos::Endian128::k16in32))
    return;
  D3D12TextureCache::NativeResolveTarget found[D3D12TextureCache::kMaxNativeResolveTargets];
  uint32_t count = textures.FindNativeResolveTargets(
      resolve.copy_dest_base_raw, dest_pitch,
      xenos::TextureFormat(resolve.copy_dest_info.copy_dest_format),
      xenos::Endian(resolve.copy_dest_info.copy_dest_endian),
      IsDrawResolutionScaled() && !key.original_resolution, found);
  for (uint32_t i = 0; i < count; ++i) {
    const auto& target = found[i];
    const D3D12_RESOURCE_DESC target_desc = target.resource->GetDesc();
    bool copy_compatible = (source_desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM &&
                            target_desc.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS) ||
                           (source_desc.Format == DXGI_FORMAT_R10G10B10A2_UNORM &&
                            target_desc.Format == DXGI_FORMAT_R10G10B10A2_TYPELESS);
    if (target.width > x1 || target.height > y1 ||
        uint64_t(target.width) * scale_x > source_desc.Width ||
        target.height * scale_y > source_desc.Height || !copy_compatible ||
        source_desc.SampleDesc.Count != 1 ||
        target_desc.Width != uint64_t(target.width) * scale_x ||
        target_desc.Height != target.height * scale_y) {
      continue;
    }
    copy.targets[copy.target_count++] = target;
  }
  copy.source = &source;
  copy.scale_x = scale_x;
  copy.scale_y = scale_y;
  copy.red_blue_swapped = resolve.copy_dest_info.copy_dest_swap != 0;
}

void D3D12RenderTargetCache::PerformNativeResolveCopy(const NativeResolveCopy& copy,
                                                      D3D12TextureCache& textures) {
  if (!copy.target_count) {
    return;
  }
  command_processor_.PushTransitionBarrier(
      copy.source->resource(), copy.source->SetResourceState(D3D12_RESOURCE_STATE_COPY_SOURCE),
      D3D12_RESOURCE_STATE_COPY_SOURCE);
  for (uint32_t i = 0; i < copy.target_count; ++i) {
    textures.BeginNativeResolveWrite(copy.targets[i]);
  }
  command_processor_.SubmitBarriers();
  D3D12_TEXTURE_COPY_LOCATION source = {};
  source.pResource = copy.source->resource();
  source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  for (uint32_t i = 0; i < copy.target_count; ++i) {
    const auto& target = copy.targets[i];
    D3D12_TEXTURE_COPY_LOCATION destination = {};
    destination.pResource = target.resource;
    destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_BOX box = {0, 0, 0, target.width * copy.scale_x, target.height * copy.scale_y, 1};
    command_processor_.GetDeferredCommandList().D3DCopyTextureRegion(&destination, 0, 0, 0, &source,
                                                                     &box);
    textures.EndNativeResolveWrite(target, copy.red_blue_swapped);
    command_processor_.native_gpu_diagnostics().RecordOperation(
        NativeGpuDiagnostics::Operation::kResolve);
    if (++direct_resolve_success_count_ <= 8 || !(direct_resolve_success_count_ & 4095)) {
      REXGPU_INFO("[d3d12-native-resolve] {} resolves copied into the textures sampling them",
                  direct_resolve_success_count_);
    }
  }
}

bool D3D12RenderTargetCache::DumpRenderTargets(uint32_t dump_base, uint32_t dump_row_length_used,
                                               uint32_t dump_rows, uint32_t dump_pitch) {
  if (!EnsureEdramBuffer()) {
    return false;
  }
  assert_true(GetPath() == Path::kHostRenderTargets);

  GetResolveCopyRectanglesToDump(dump_base, dump_row_length_used, dump_rows, dump_pitch,
                                 dump_rectangles_);
  if (dump_rectangles_.empty()) {
    return true;
  }

  // Clear previously set temporary indices.
  for (const ResolveCopyDumpRectangle& rectangle : dump_rectangles_) {
    auto& d3d12_rt = *static_cast<D3D12RenderTarget*>(rectangle.render_target);
    d3d12_rt.SetTemporarySortIndex(UINT32_MAX);
    d3d12_rt.SetTemporarySRVDescriptorIndex(UINT32_MAX);
    d3d12_rt.SetTemporarySRVDescriptorIndexStencil(UINT32_MAX);
  }
  // Gather all needed barriers and info needed to create descriptors and to
  // sort the invocations.
  TransitionEdramBuffer(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  dump_invocations_.clear();
  dump_invocations_.reserve(dump_rectangles_.size());
  current_temporary_descriptors_cpu_.clear();
  bool any_sources_32bpp_64bpp[2] = {};
  uint32_t rt_sort_index = 0;
  for (const ResolveCopyDumpRectangle& rectangle : dump_rectangles_) {
    auto& d3d12_rt = *static_cast<D3D12RenderTarget*>(rectangle.render_target);
    command_processor_.PushTransitionBarrier(
        d3d12_rt.resource(),
        d3d12_rt.SetResourceState(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (d3d12_rt.temporary_sort_index() == UINT32_MAX) {
      d3d12_rt.SetTemporarySortIndex(rt_sort_index++);
    }
    if (d3d12_rt.temporary_srv_descriptor_index() == UINT32_MAX) {
      d3d12_rt.SetTemporarySRVDescriptorIndex(uint32_t(current_temporary_descriptors_cpu_.size()));
      current_temporary_descriptors_cpu_.push_back(d3d12_rt.descriptor_srv().GetHandle());
    }
    RenderTargetKey rt_key = d3d12_rt.key();
    if (rt_key.is_depth && d3d12_rt.temporary_srv_descriptor_index_stencil() == UINT32_MAX) {
      d3d12_rt.SetTemporarySRVDescriptorIndexStencil(
          uint32_t(current_temporary_descriptors_cpu_.size()));
      current_temporary_descriptors_cpu_.push_back(d3d12_rt.descriptor_srv_stencil().GetHandle());
    }
    any_sources_32bpp_64bpp[size_t(rt_key.Is64bpp())] = true;
    DumpPipelineKey pipeline_key;
    pipeline_key.msaa_samples = rt_key.msaa_samples;
    pipeline_key.resource_format = rt_key.resource_format;
    pipeline_key.is_depth = rt_key.is_depth;
    dump_invocations_.emplace_back(rectangle, pipeline_key);
  }
  // 32bpp and 64bpp.
  size_t edram_uav_indices[2] = {SIZE_MAX, SIZE_MAX};
  const ui::d3d12::D3D12Provider& provider = command_processor_.GetD3D12Provider();
  if (!bindless_resources_used_) {
    if (any_sources_32bpp_64bpp[0]) {
      edram_uav_indices[0] = current_temporary_descriptors_cpu_.size();
      current_temporary_descriptors_cpu_.push_back(provider.OffsetViewDescriptor(
          edram_buffer_descriptor_heap_start_, uint32_t(EdramBufferDescriptorIndex::kR32UintUAV)));
    }
    if (any_sources_32bpp_64bpp[1]) {
      edram_uav_indices[1] = current_temporary_descriptors_cpu_.size();
      current_temporary_descriptors_cpu_.push_back(
          provider.OffsetViewDescriptor(edram_buffer_descriptor_heap_start_,
                                        uint32_t(EdramBufferDescriptorIndex::kR32G32UintUAV)));
    }
  }

  // Copy source descriptors to a shader-visible heap.
  ID3D12Device* device = provider.GetDevice();
  uint32_t descriptor_count = uint32_t(current_temporary_descriptors_cpu_.size());
  current_temporary_descriptors_gpu_.resize(descriptor_count);
  if (!command_processor_.RequestOneUseSingleViewDescriptors(
          descriptor_count, current_temporary_descriptors_gpu_.data())) {
    return false;
  }
  for (uint32_t i = 0; i < descriptor_count; ++i) {
    device->CopyDescriptorsSimple(1, current_temporary_descriptors_gpu_[i].first,
                                  current_temporary_descriptors_cpu_[i],
                                  D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  }

  // Sort the invocations to reduce context and binding switches.
  std::sort(dump_invocations_.begin(), dump_invocations_.end());

  // Dump the render targets.
  DeferredCommandList& command_list = command_processor_.GetDeferredCommandList();
  ID3D12RootSignature* last_root_signature = nullptr;
  uint32_t root_parameters_set = 0;
  uint32_t last_descriptor_index_source = UINT32_MAX;
  uint32_t last_descriptor_index_stencil = UINT32_MAX;
  bool last_edram_uav_is_64bpp = false;
  DumpOffsets last_offsets;
  DumpPitches last_pitches;
  bool all_pipelines_available = true;
  for (const DumpInvocation& invocation : dump_invocations_) {
    const ResolveCopyDumpRectangle& rectangle = invocation.rectangle;
    auto& d3d12_rt = *static_cast<D3D12RenderTarget*>(rectangle.render_target);
    RenderTargetKey rt_key = d3d12_rt.key();
    DumpPipelineKey pipeline_key = invocation.pipeline_key;
    ID3D12PipelineState* pipeline = GetOrCreateDumpPipeline(pipeline_key);
    if (!pipeline) {
      all_pipelines_available = false;
      continue;
    }
    command_processor_.SetExternalPipeline(pipeline);

    ID3D12RootSignature* root_signature =
        pipeline_key.is_depth ? dump_root_signature_depth_ : dump_root_signature_color_;
    if (last_root_signature != root_signature) {
      last_root_signature = root_signature;
      command_list.D3DSetComputeRootSignature(root_signature);
      root_parameters_set = 0;
    }

    DumpRootParameter root_parameter_edram =
        pipeline_key.is_depth ? kDumpRootParameterDepthEdram : kDumpRootParameterColorEdram;
    uint32_t root_parameter_edram_bit = uint32_t(1) << root_parameter_edram;
    bool format_is_64bpp = rt_key.Is64bpp();
    if (last_edram_uav_is_64bpp != format_is_64bpp) {
      last_edram_uav_is_64bpp = format_is_64bpp;
      root_parameters_set &= ~root_parameter_edram_bit;
    }
    if (!(root_parameters_set & root_parameter_edram_bit)) {
      D3D12_GPU_DESCRIPTOR_HANDLE descriptor_handle_edram;
      if (bindless_resources_used_) {
        descriptor_handle_edram =
            command_processor_
                .GetEdramUintPow2BindlessUAVHandlePair(2 + uint32_t(last_edram_uav_is_64bpp))
                .second;
      } else {
        assert_true(edram_uav_indices[size_t(last_edram_uav_is_64bpp)] != SIZE_MAX);
        descriptor_handle_edram =
            current_temporary_descriptors_gpu_[edram_uav_indices[size_t(last_edram_uav_is_64bpp)]]
                .second;
      }
      command_list.D3DSetComputeRootDescriptorTable(root_parameter_edram, descriptor_handle_edram);
      root_parameters_set |= root_parameter_edram_bit;
    }

    DumpRootParameter root_parameter_pitches =
        pipeline_key.is_depth ? kDumpRootParameterDepthPitches : kDumpRootParameterColorPitches;
    uint32_t root_parameter_pitches_bit = uint32_t(1) << root_parameter_pitches;
    DumpPitches pitches;
    pitches.dest_pitch = dump_pitch;
    pitches.source_pitch = rt_key.GetPitchTiles();
    if (last_pitches != pitches) {
      last_pitches = pitches;
      root_parameters_set &= ~root_parameter_pitches_bit;
    }
    if (!(root_parameters_set & root_parameter_pitches_bit)) {
      command_list.D3DSetComputeRoot32BitConstants(
          root_parameter_pitches, sizeof(last_pitches) / sizeof(uint32_t), &last_pitches, 0);
      root_parameters_set |= root_parameter_pitches_bit;
    }

    if (pipeline_key.is_depth) {
      constexpr uint32_t kDumpRootParameterDepthStencilBit = uint32_t(1)
                                                             << kDumpRootParameterDepthStencil;
      uint32_t descriptor_index_stencil = d3d12_rt.temporary_srv_descriptor_index_stencil();
      assert_true(descriptor_index_stencil != UINT32_MAX);
      if (last_descriptor_index_stencil != descriptor_index_stencil) {
        last_descriptor_index_stencil = descriptor_index_stencil;
        root_parameters_set &= ~kDumpRootParameterDepthStencilBit;
      }
      if (!(root_parameters_set & kDumpRootParameterDepthStencilBit)) {
        command_list.D3DSetComputeRootDescriptorTable(
            kDumpRootParameterDepthStencil,
            current_temporary_descriptors_gpu_[last_descriptor_index_stencil].second);
        root_parameters_set |= kDumpRootParameterDepthStencilBit;
      }
    }

    constexpr uint32_t kDumpRootParameterSourceBit = uint32_t(1) << kDumpRootParameterSource;
    uint32_t descriptor_index_source = d3d12_rt.temporary_srv_descriptor_index();
    assert_true(descriptor_index_source != UINT32_MAX);
    if (last_descriptor_index_source != descriptor_index_source) {
      last_descriptor_index_source = descriptor_index_source;
      root_parameters_set &= ~kDumpRootParameterSourceBit;
    }
    if (!(root_parameters_set & kDumpRootParameterSourceBit)) {
      command_list.D3DSetComputeRootDescriptorTable(
          kDumpRootParameterSource,
          current_temporary_descriptors_gpu_[last_descriptor_index_source].second);
      root_parameters_set |= kDumpRootParameterSourceBit;
    }

    constexpr uint32_t kDumpRootParameterOffsetsBit = uint32_t(1) << kDumpRootParameterOffsets;
    DumpOffsets offsets;
    offsets.source_base_tiles = rt_key.base_tiles;
    ResolveCopyDumpRectangle::Dispatch dispatches[ResolveCopyDumpRectangle::kMaxDispatches];
    uint32_t dispatch_count = rectangle.GetDispatches(dump_pitch, dump_row_length_used, dispatches);
    for (uint32_t i = 0; i < dispatch_count; ++i) {
      const ResolveCopyDumpRectangle::Dispatch& dispatch = dispatches[i];
      offsets.dispatch_first_tile = dump_base + dispatch.offset;
      if (last_offsets != offsets) {
        last_offsets = offsets;
        root_parameters_set &= ~kDumpRootParameterOffsetsBit;
      }
      if (!(root_parameters_set & kDumpRootParameterOffsetsBit)) {
        command_list.D3DSetComputeRoot32BitConstants(
            kDumpRootParameterOffsets, sizeof(last_offsets) / sizeof(uint32_t), &last_offsets, 0);
        root_parameters_set |= kDumpRootParameterOffsetsBit;
      }
      command_processor_.SubmitBarriers();
      // Processing 40 x 16 x scale samples per dispatch (a 32bpp tile in two
      // dispatches at 1x1 scale, 64bpp in one dispatch).
      command_list.D3DDispatch((dispatch.width_tiles * draw_resolution_scale_x())
                                   << uint32_t(!format_is_64bpp),
                               dispatch.height_tiles * draw_resolution_scale_y(), 1);
    }
    MarkEdramBufferModified();
  }
  return all_pipelines_available;
}

}  // namespace rex::graphics::d3d12
