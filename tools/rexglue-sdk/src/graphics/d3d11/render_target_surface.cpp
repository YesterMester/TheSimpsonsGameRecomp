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

#include <rex/graphics/d3d11/render_target_surface.h>

#include <rex/graphics/d3d11/profile.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include <rex/assert.h>

namespace rex::graphics::d3d11 {
namespace {
using Microsoft::WRL::ComPtr;
DXGI_FORMAT GetColorResourceDXGIFormat(xenos::ColorRenderTargetFormat format,
                                       bool gamma_as_unorm16) {
  // Typed should be preferred over typeless so there are more opportunities for
  // compression.
  switch (format) {
    case xenos::ColorRenderTargetFormat::k_8_8_8_8:
      return DXGI_FORMAT_R8G8B8A8_UNORM;
    case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA:
      return gamma_as_unorm16 ? DXGI_FORMAT_R16G16B16A16_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
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

DXGI_FORMAT GetColorDrawDXGIFormat(xenos::ColorRenderTargetFormat format, bool gamma_as_unorm16) {
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
      return GetColorResourceDXGIFormat(format, gamma_as_unorm16);
  }
}

DXGI_FORMAT GetColorOwnershipTransferDXGIFormat(xenos::ColorRenderTargetFormat format,
                                                bool gamma_as_unorm16, bool* is_integer_out) {
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
      return GetColorDrawDXGIFormat(format, gamma_as_unorm16);
  }
}

DXGI_FORMAT GetDepthResourceDXGIFormat(xenos::DepthRenderTargetFormat format) {
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

DXGI_FORMAT GetDepthDSVDXGIFormat(xenos::DepthRenderTargetFormat format) {
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

DXGI_FORMAT GetDepthSRVDepthDXGIFormat(xenos::DepthRenderTargetFormat format) {
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

DXGI_FORMAT GetDepthSRVStencilDXGIFormat(xenos::DepthRenderTargetFormat format) {
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

bool Failed(HRESULT result, const char* operation, std::string& error) {
  if (SUCCEEDED(result))
    return false;
  char message[160];
  std::snprintf(message, sizeof(message), "%s failed (0x%08X)", operation, unsigned(result));
  error = message;
  return true;
}
}  // namespace

std::unique_ptr<RenderTargetSurface> RenderTargetSurface::Create(ui::d3d11::D3D11Device& owner,
                                                                 const Description& description,
                                                                 std::string& error) {
  error.clear();
  uint32_t format = description.guest_format;
  bool valid_color = format <= 7 || format == 10 || format == 12 || format == 14 || format == 15;
  if (!description.width || !description.height ||
      description.width > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
      description.height > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
      uint32_t(description.samples) > 2 || (description.depth ? format > 1 : !valid_color)) {
    error = "Invalid native render target extent, format or sample count";
    return nullptr;
  }
  auto surface = std::unique_ptr<RenderTargetSurface>(new RenderTargetSurface());
  surface->description_ = description;
  surface->sample_count_ = 1u << uint32_t(description.samples);
  if (description.samples == xenos::MsaaSamples::k2X && !description.msaa_2x_supported)
    surface->sample_count_ = 4;
  DXGI_FORMAT resource_format, draw_format, transfer_format;
  if (description.depth) {
    auto depth_format = xenos::DepthRenderTargetFormat(format);
    resource_format = GetDepthResourceDXGIFormat(depth_format);
    draw_format = transfer_format = GetDepthDSVDXGIFormat(depth_format);
  } else {
    auto color_format = xenos::ColorRenderTargetFormat(format);
    resource_format = GetColorResourceDXGIFormat(color_format, description.gamma_as_unorm16);
    draw_format = GetColorDrawDXGIFormat(color_format, description.gamma_as_unorm16);
    transfer_format = GetColorOwnershipTransferDXGIFormat(
        color_format, description.gamma_as_unorm16, &surface->integer_transfer_);
  }
  uint32_t quality_levels = 0;
  if (Failed(owner.device()->CheckMultisampleQualityLevels(draw_format, surface->sample_count_,
                                                           &quality_levels),
             "Native render target sample query", error) ||
      !quality_levels) {
    if (error.empty())
      error = "The native render target format does not support this sample count";
    return nullptr;
  }
  D3D11_TEXTURE2D_DESC desc = {};
  desc.Width = description.width;
  desc.Height = description.height;
  desc.MipLevels = desc.ArraySize = 1;
  desc.Format = resource_format;
  desc.SampleDesc.Count = surface->sample_count_;
  desc.BindFlags = D3D11_BIND_SHADER_RESOURCE |
                   (description.depth ? D3D11_BIND_DEPTH_STENCIL : D3D11_BIND_RENDER_TARGET);
  if (Failed(owner.device()->CreateTexture2D(&desc, nullptr, surface->image_.GetAddressOf()),
             "Native render target image", error))
    return nullptr;
  D3D11_SHADER_RESOURCE_VIEW_DESC srv = {};
  srv.ViewDimension =
      surface->sample_count_ > 1 ? D3D11_SRV_DIMENSION_TEXTURE2DMS : D3D11_SRV_DIMENSION_TEXTURE2D;
  if (surface->sample_count_ == 1)
    srv.Texture2D.MipLevels = 1;
  if (description.depth) {
    D3D11_DEPTH_STENCIL_VIEW_DESC dsv = {};
    dsv.Format = draw_format;
    dsv.ViewDimension = surface->sample_count_ > 1 ? D3D11_DSV_DIMENSION_TEXTURE2DMS
                                                   : D3D11_DSV_DIMENSION_TEXTURE2D;
    if (Failed(owner.device()->CreateDepthStencilView(surface->image_.Get(), &dsv,
                                                      surface->depth_.GetAddressOf()),
               "Native depth target view", error))
      return nullptr;
    auto depth_format = xenos::DepthRenderTargetFormat(format);
    srv.Format = GetDepthSRVDepthDXGIFormat(depth_format);
    if (Failed(owner.device()->CreateShaderResourceView(surface->image_.Get(), &srv,
                                                        surface->read_.GetAddressOf()),
               "Native depth source view", error))
      return nullptr;
    srv.Format = GetDepthSRVStencilDXGIFormat(depth_format);
    if (Failed(owner.device()->CreateShaderResourceView(surface->image_.Get(), &srv,
                                                        surface->stencil_.GetAddressOf()),
               "Native stencil source view", error))
      return nullptr;
  } else {
    D3D11_RENDER_TARGET_VIEW_DESC rtv = {};
    rtv.ViewDimension = surface->sample_count_ > 1 ? D3D11_RTV_DIMENSION_TEXTURE2DMS
                                                   : D3D11_RTV_DIMENSION_TEXTURE2D;
    rtv.Format = draw_format;
    if (Failed(owner.device()->CreateRenderTargetView(surface->image_.Get(), &rtv,
                                                      surface->draw_.GetAddressOf()),
               "Native draw target view", error))
      return nullptr;
    if (transfer_format == draw_format) {
      surface->transfer_ = surface->draw_;
    } else {
      rtv.Format = transfer_format;
      if (Failed(owner.device()->CreateRenderTargetView(surface->image_.Get(), &rtv,
                                                        surface->transfer_.GetAddressOf()),
                 "Native exact transfer target view", error))
        return nullptr;
    }
    srv.Format = transfer_format;
    if (Failed(owner.device()->CreateShaderResourceView(surface->image_.Get(), &srv,
                                                        surface->read_.GetAddressOf()),
               "Native render target source view", error))
      return nullptr;
  }
  return surface;
}

std::unique_ptr<RenderTargetSurface> RenderTargetSurface::Snapshot(ui::d3d11::D3D11Device& device,
                                                                   DrawContext& draws,
                                                                   std::string& error) const {
  auto copy = Create(device, description_, error);
  if (!copy)
    return nullptr;
  draws.Invalidate();
  device.context()->CopyResource(copy->image(), image());
  if (Failed(device.device()->GetDeviceRemovedReason(), "Native render target snapshot", error))
    return nullptr;
  return copy;
}

const ShaderProgram* RenderTargetTransfer::Helper(const char* source, const char* profile,
                                                  std::string& error) {
  Microsoft::WRL::ComPtr<ID3DBlob> code, diagnostics;
  if (Failed(D3DCompile(source, std::strlen(source), "native_target_transfer", nullptr, nullptr,
                        "main", profile, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, code.GetAddressOf(),
                        diagnostics.GetAddressOf()),
             "Native render target helper compilation", error)) {
    if (diagnostics)
      error.assign(static_cast<const char*>(diagnostics->GetBufferPointer()),
                   diagnostics->GetBufferSize());
    return nullptr;
  }
  return shaders_.GetOrCreate(
      {static_cast<const uint8_t*>(code->GetBufferPointer()), code->GetBufferSize()}, false, error);
}

bool RenderTargetTransfer::Prepare(DrawCommand& command, const RenderTargetSurface& destination,
                                   std::string& error) {
  if (!vertex_) {
    vertex_ = Helper(
        "float4 main(uint id:SV_VertexID):SV_Position {"
        "float2 p=float2((id<<1)&2,id&2);"
        "return float4(p*float2(2,-2)+float2(-1,1),0,1); }",
        "vs_5_1", error);
    if (!vertex_)
      return false;
  }
  command = {};
  command.programs[size_t(DrawStage::kVertex)] = vertex_;
  command.topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
  command.count = 3;
  command.state =
      DrawState::Default(destination.description().width, destination.description().height);
  if (destination.description().samples == xenos::MsaaSamples::k2X &&
      !destination.description().msaa_2x_supported)
    command.state.sample_mask = 9;
  return true;
}

std::shared_ptr<const BufferVersion> RenderTargetTransfer::Constant(const void* data, size_t size,
                                                                    std::string& error) {
  return buffers_.GetOrCreate({static_cast<const uint8_t*>(data), size}, BufferKind::kConstant,
                              error);
}

bool RenderTargetTransfer::Rectangles(DrawCommand& command, std::span<const D3D11_RECT> rectangles,
                                      std::string& error) {
  // Validate every rectangle before changing output state or writing any pixels.
  for (const auto& rect : rectangles) {
    if (rect.left < 0 || rect.top < 0 || rect.right < rect.left || rect.bottom < rect.top ||
        uint32_t(rect.right) > uint32_t(command.state.viewport.Width) ||
        uint32_t(rect.bottom) > uint32_t(command.state.viewport.Height)) {
      error = "Native render target rectangle is outside the destination";
      return false;
    }
  }
  for (const auto& rect : rectangles) {
    if (rect.left == rect.right || rect.top == rect.bottom)
      continue;
    command.state.scissor = rect;
    if (!draws_.Draw(command, error))
      return false;
  }
  return true;
}

bool RenderTargetTransfer::Transfer(RenderTargetSurface& destination,
                                    const RenderTargetSurface& source,
                                    const RenderTargetSurface* host_depth,
                                    const ShaderProgram& pixel, const Constants& constants,
                                    std::span<const D3D11_RECT> rectangles, std::string& error) {
  error.clear();
  if (!pixel.pixel() || destination.image() == source.image() ||
      (host_depth &&
       (host_depth->image() == destination.image() || !host_depth->description().depth)) ||
      (constants.output == Output::kColor) == destination.description().depth ||
      (constants.output == Output::kStencilBit &&
       (!constants.stencil_bit || (constants.stencil_bit & (constants.stencil_bit - 1))))) {
    error = "Invalid native render target transfer sources, output or stencil bit";
    return false;
  }
  g_profile.transfer_draws.fetch_add(1, std::memory_order_relaxed);
  DrawCommand command;
  if (!Prepare(command, destination, error))
    return false;
  command.programs[size_t(DrawStage::kPixel)] = &pixel;
  std::array<ID3D11ShaderResourceView*, 4> reads = {
      source.description().depth ? nullptr : source.read_view(),
      source.description().depth ? source.read_view() : nullptr,
      source.description().depth ? source.stencil_view() : nullptr,
      host_depth ? host_depth->read_view() : nullptr};
  command.bindings[size_t(DrawStage::kPixel)].resources = reads;
  std::array<uint32_t, 4> mask = {constants.stencil_bit, 0, 0, 0};
  std::array<uint32_t, 4> address = {constants.address, 0, 0, 0};
  std::array<uint32_t, 4> host_address = {constants.host_depth_address, 0, 0, 0};
  auto mask_buffer = Constant(mask.data(), sizeof(mask), error);
  auto address_buffer = Constant(address.data(), sizeof(address), error);
  auto host_buffer = Constant(host_address.data(), sizeof(host_address), error);
  if (!mask_buffer || !address_buffer || !host_buffer)
    return false;
  std::array<ID3D11Buffer*, 3> cb = {mask_buffer->buffer(), address_buffer->buffer(),
                                     host_buffer->buffer()};
  command.bindings[size_t(DrawStage::kPixel)].constants = cb;
  std::array<ID3D11RenderTargetView*, 1> targets = {destination.transfer_view()};
  if (constants.output == Output::kColor) {
    command.render_targets = targets;
  } else {
    command.depth_stencil = destination.depth_view();
    if (constants.output == Output::kDepth) {
      command.state.depth_stencil.DepthEnable = TRUE;
      command.state.depth_stencil.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
      command.state.depth_stencil.DepthFunc = D3D11_COMPARISON_ALWAYS;
    }
    if (constants.output == Output::kStencilBit || constants.write_stencil_reference) {
      command.state.depth_stencil.StencilEnable = TRUE;
      command.state.depth_stencil.StencilWriteMask =
          constants.output == Output::kStencilBit ? constants.stencil_bit : 255;
      command.state.depth_stencil.FrontFace.StencilPassOp = D3D11_STENCIL_OP_REPLACE;
      command.state.depth_stencil.FrontFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
      command.state.depth_stencil.BackFace = command.state.depth_stencil.FrontFace;
      command.state.stencil_reference = 255;
    }
  }
  return Rectangles(command, rectangles, error);
}

bool RenderTargetTransfer::ClearColor(RenderTargetSurface& destination,
                                      const std::array<float, 4>& value,
                                      std::span<const D3D11_RECT> rectangles, std::string& error) {
  error.clear();
  if (destination.description().depth || destination.integer_transfer()) {
    error = "Floating-point clear used with an exact integer or depth target";
    return false;
  }
  if (!color_float_)
    color_float_ = Helper(
        "cbuffer Clear:b0 { float4 value; };"
        "float4 main():SV_Target { return value; }",
        "ps_5_1", error);
  if (!color_float_)
    return false;
  DrawCommand command;
  if (!Prepare(command, destination, error))
    return false;
  command.programs[size_t(DrawStage::kPixel)] = color_float_;
  auto cb = Constant(value.data(), sizeof(value), error);
  if (!cb)
    return false;
  std::array<ID3D11Buffer*, 1> constants = {cb->buffer()};
  std::array<ID3D11RenderTargetView*, 1> targets = {destination.transfer_view()};
  command.bindings[size_t(DrawStage::kPixel)].constants = constants;
  command.render_targets = targets;
  return Rectangles(command, rectangles, error);
}

bool RenderTargetTransfer::ClearColorInteger(RenderTargetSurface& destination,
                                             const std::array<uint32_t, 4>& value,
                                             std::span<const D3D11_RECT> rectangles,
                                             std::string& error) {
  error.clear();
  if (destination.description().depth || !destination.integer_transfer()) {
    error = "Exact integer clear used with a normalized or depth target";
    return false;
  }
  if (!color_integer_)
    color_integer_ = Helper(
        "cbuffer Clear:b0 { uint4 value; };"
        "uint4 main():SV_Target { return value; }",
        "ps_5_1", error);
  if (!color_integer_)
    return false;
  DrawCommand command;
  if (!Prepare(command, destination, error))
    return false;
  command.programs[size_t(DrawStage::kPixel)] = color_integer_;
  auto stored_value = value;
  D3D11_RENDER_TARGET_VIEW_DESC view_description;
  destination.transfer_view()->GetDesc(&view_description);
  if (view_description.Format == DXGI_FORMAT_R16G16_UINT ||
      view_description.Format == DXGI_FORMAT_R16G16B16A16_UINT) {
    // Integer attachment conversion saturates instead of selecting low bits.
    // The clear values describe raw guest components, not numeric conversion.
    for (auto& component : stored_value)
      component &= 0xFFFF;
  }
  auto cb = Constant(stored_value.data(), sizeof(stored_value), error);
  if (!cb)
    return false;
  std::array<ID3D11Buffer*, 1> constants = {cb->buffer()};
  std::array<ID3D11RenderTargetView*, 1> targets = {destination.transfer_view()};
  command.bindings[size_t(DrawStage::kPixel)].constants = constants;
  command.render_targets = targets;
  return Rectangles(command, rectangles, error);
}

bool RenderTargetTransfer::ClearDepthStencil(RenderTargetSurface& destination, bool clear_depth,
                                             float depth, bool clear_stencil, uint8_t stencil,
                                             std::span<const D3D11_RECT> rectangles,
                                             std::string& error) {
  error.clear();
  if (!destination.description().depth || (!clear_depth && !clear_stencil) ||
      !std::isfinite(depth) || depth < 0 || depth > 1) {
    error = "Invalid native partial depth or stencil clear";
    return false;
  }
  auto*& pixel = clear_depth ? depth_ : stencil_;
  if (!pixel)
    pixel = Helper(clear_depth ? "cbuffer Clear:b0 { float4 value; };"
                                 "float main():SV_Depth { return value.x; }"
                               : "void main() {}",
                   "ps_5_1", error);
  if (!pixel)
    return false;
  DrawCommand command;
  if (!Prepare(command, destination, error))
    return false;
  command.programs[size_t(DrawStage::kPixel)] = pixel;
  command.depth_stencil = destination.depth_view();
  std::array<float, 4> value = {depth, 0, 0, 0};
  auto cb = Constant(value.data(), sizeof(value), error);
  if (!cb)
    return false;
  std::array<ID3D11Buffer*, 1> constants = {cb->buffer()};
  command.bindings[size_t(DrawStage::kPixel)].constants = constants;
  command.state.depth_stencil.DepthEnable = clear_depth;
  command.state.depth_stencil.DepthWriteMask =
      clear_depth ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
  command.state.depth_stencil.DepthFunc = D3D11_COMPARISON_ALWAYS;
  if (clear_stencil) {
    command.state.depth_stencil.StencilEnable = TRUE;
    command.state.depth_stencil.StencilWriteMask = 255;
    command.state.depth_stencil.FrontFace.StencilPassOp = D3D11_STENCIL_OP_REPLACE;
    command.state.depth_stencil.FrontFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
    command.state.depth_stencil.BackFace = command.state.depth_stencil.FrontFace;
    command.state.stencil_reference = stencil;
  }
  return Rectangles(command, rectangles, error);
}

void RenderTargetTransfer::Clear() {
  draws_.Invalidate();
  shaders_.Clear();
  buffers_.Clear();
  vertex_ = color_float_ = color_integer_ = depth_ = stencil_ = nullptr;
}

}  // namespace rex::graphics::d3d11
