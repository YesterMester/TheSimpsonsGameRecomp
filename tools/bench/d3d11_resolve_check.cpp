#include <rex/graphics/d3d11/resolve_copy.h>
#include <rex/graphics/pipeline/texture/info.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <cfenv>
#include <fstream>
#include <stdexcept>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace rex::graphics::d3d11;
namespace draw = rex::graphics::draw_util;
namespace xenos = rex::graphics::xenos;
using rex::ui::d3d11::D3D11Device;

namespace {
void Require(bool condition, const std::string& message) {
  if (!condition)
    throw std::runtime_error(message);
}
void Check(HRESULT result, const char* operation) {
  if (FAILED(result)) {
    char message[160];
    std::snprintf(message, sizeof(message), "%s failed (0x%08X)", operation, unsigned(result));
    throw std::runtime_error(message);
  }
}

// CPU address reference for the guest's 32x32 texture tiles. The production
// path executes the SDK's compute bytecode and does not call this function.
uint32_t TextureAddress(uint32_t x, uint32_t y, uint32_t pitch, uint32_t size_log2) {
  uint32_t macro = ((x / 32) + (y / 32) * (pitch / 32)) << (size_log2 + 7);
  uint32_t micro = ((x % 8) + ((y & 14) * 4)) << size_log2;
  uint32_t offset = macro + (micro / 16) * 32 + (micro % 16) + (y % 2) * 16;
  return (offset / 512) * 4096 + (y & 16) * 128 + (offset & 448) * 4 +
         (((y / 4 & 2) + x / 8) % 4) * 64 + (offset % 64);
}
uint32_t TextureAddress3D(uint32_t x, uint32_t y, uint32_t z, uint32_t pitch, uint32_t height,
                          uint32_t size_log2) {
  uint32_t outer = (((z / 4 * (height / 16) + y / 16) * (pitch / 32)) + x / 32) * 128;
  uint32_t inner = (z % 4) * 32 + ((y / 2) % 4) * 8 + x % 8;
  uint32_t bytes = (outer | inner) << size_log2;
  uint32_t bank = (y / 8 ^ z / 4) & 1;
  uint32_t pipe = (x / 8 & 3) ^ (bank * 2);
  return (y % 2) * 16 | pipe * 64 | bank * 2048 | (bytes & 15) | ((bytes / 16 & 1) * 32) |
         ((bytes / 32 & 7) * 256) | (bytes / 256 * 4096);
}
uint32_t SourceWord(uint32_t index) {
  return std::rotl(index * 0x9E3779B9u, 13) ^ 0xA7682F19u;
}
uint32_t EndianWord(uint32_t value, uint32_t endian) {
  std::array<uint8_t, 4> input, output;
  std::memcpy(input.data(), &value, 4);
  for (uint32_t i = 0; i < 4; ++i)
    output[i] = input[i ^ (endian == 1 ? 1 : endian == 2 ? 3 : endian == 3 ? 2 : 0)];
  std::memcpy(&value, output.data(), 4);
  return value;
}

struct Results {
  uint64_t compared_bytes = 0, written_bytes = 0;
  uint32_t dispatches = 0, rejected = 0, offset_dispatches = 0, mirrored_dispatches = 0;
  uint32_t native_color_dispatches = 0, native_depth_dispatches = 0;
  uint32_t native_float_dispatches = 0, native_partial_dispatches = 0;
  uint32_t native_array_dispatches = 0, native_bias_dispatches = 0, general_surface_comparisons = 0;
  uint32_t native_msaa_dispatches = 0, native_conversion_dispatches = 0;
};
struct DepthCase {
  bool floating, round;
  float value;
  uint8_t stencil;
};
uint32_t FloatDepthReference(float host_depth, bool round) {
  double value = double(host_depth) * 2.0;
  auto integral = [&](double number) {
    return uint32_t(round ? std::nearbyint(number) : std::floor(number));
  };
  if (value < std::ldexp(1.0, -14))
    return integral(std::ldexp(value, 34));
  int exponent;
  double mantissa = std::frexp(value, &exponent) * 2.0 - 1.0;
  return (uint32_t(exponent + 14) * (1u << 20) + integral(std::ldexp(mantissa, 20))) & 0xFFFFFFu;
}
struct Fixture {
  D3D11Device& device;
  DrawContext draws;
  ResolveCopy copy;
  Results results;
  explicit Fixture(D3D11Device& owner) : device(owner), draws(owner), copy(owner, draws) {}

  void DrawSamplePattern(RenderTargetSurface& surface) {
    static constexpr char vertex_source[] = R"(
      float4 main(uint id : SV_VertexID) : SV_Position {
        return float4(id == 2u ? 3.0 : -1.0, id == 1u ? -3.0 : 1.0, 0.0, 1.0);
      })";
    static constexpr char color_source[] = R"(
      float4 main(float4 p : SV_Position, uint s : SV_SampleIndex) : SV_Target {
        return float4(frac(p.x * 0.03125) + float(s) * 0.0625,
                      frac(p.y * 0.0625) - 0.25, float(s) * 0.375, float(s + 1u) * 0.125);
      })";
    static constexpr char depth_source[] = R"(
      float main(float4 p : SV_Position, uint s : SV_SampleIndex) : SV_Depth {
        return float(s + 1u) * 0.0625 + frac(p.x * 0.03125) * 0.03125;
      })";
    std::string error;
    ComPtr<ID3DBlob> vertex_code, pixel_code;
    Check(D3D11Device::CompileShader(vertex_source, "main", D3D11Device::ShaderStage::kVertex,
                                     vertex_code.GetAddressOf(), error),
          "Sample vertex shader");
    Check(D3D11Device::CompileShader(surface.description().depth ? depth_source : color_source,
                                     "main", D3D11Device::ShaderStage::kPixel,
                                     pixel_code.GetAddressOf(), error),
          "Sample pixel shader");
    ComPtr<ID3D11VertexShader> vertex;
    ComPtr<ID3D11PixelShader> pixel;
    Check(device.device()->CreateVertexShader(vertex_code->GetBufferPointer(),
                                              vertex_code->GetBufferSize(), nullptr,
                                              vertex.GetAddressOf()),
          "Sample vertex program");
    Check(device.device()->CreatePixelShader(pixel_code->GetBufferPointer(),
                                             pixel_code->GetBufferSize(), nullptr,
                                             pixel.GetAddressOf()),
          "Sample pixel program");
    D3D11_RASTERIZER_DESC raster = {};
    raster.FillMode = D3D11_FILL_SOLID;
    raster.CullMode = D3D11_CULL_NONE;
    raster.DepthClipEnable = raster.MultisampleEnable = true;
    ComPtr<ID3D11RasterizerState> raster_state;
    Check(device.device()->CreateRasterizerState(&raster, raster_state.GetAddressOf()),
          "Sample rasterizer");
    D3D11_DEPTH_STENCIL_DESC depth = {};
    depth.DepthEnable = surface.description().depth;
    depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    depth.DepthFunc = D3D11_COMPARISON_ALWAYS;
    depth.StencilEnable = surface.description().depth;
    depth.StencilReadMask = depth.StencilWriteMask = 255;
    depth.FrontFace.StencilFailOp = depth.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
    depth.FrontFace.StencilPassOp = D3D11_STENCIL_OP_REPLACE;
    depth.FrontFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
    depth.BackFace = depth.FrontFace;
    ComPtr<ID3D11DepthStencilState> depth_state;
    Check(device.device()->CreateDepthStencilState(&depth, depth_state.GetAddressOf()),
          "Sample depth state");
    auto* context = device.context();
    context->ClearState();
    context->VSSetShader(vertex.Get(), nullptr, 0);
    context->PSSetShader(pixel.Get(), nullptr, 0);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->RSSetState(raster_state.Get());
    D3D11_VIEWPORT viewport = {
        0, 0, float(surface.description().width), float(surface.description().height), 0, 1};
    context->RSSetViewports(1, &viewport);
    auto* rtv = surface.draw_view();
    context->OMSetRenderTargets(surface.description().depth ? 0 : 1, &rtv, surface.depth_view());
    for (uint32_t sample = 0; sample < (surface.description().depth ? surface.sample_count() : 1u);
         ++sample) {
      context->OMSetDepthStencilState(depth_state.Get(), 0x71u + 17u * sample);
      context->OMSetBlendState(nullptr, nullptr,
                               surface.description().depth ? (1u << sample) : UINT32_MAX);
      context->Draw(3, 0);
    }
    context->ClearState();
    draws.Invalidate();
  }

  std::vector<uint8_t> GeneralSurfaceReference(const RenderTargetSurface& source,
                                               draw::ResolveInfo resolve, uint32_t scale_x,
                                               uint32_t scale_y, bool scaled,
                                               uint32_t destination_address,
                                               const D3D11_UNORDERED_ACCESS_VIEW_DESC& view_desc,
                                               const std::vector<uint8_t>& initial,
                                               bool round_float24 = false) {
    std::string error;
    const auto& description = source.description();
    bool wide = !description.depth && xenos::IsColorRenderTargetFormat64bpp(
                                          xenos::ColorRenderTargetFormat(description.guest_format));
    uint32_t tile_width = 80u >> uint32_t(description.samples == xenos::MsaaSamples::k4X);
    uint32_t tile_height = 16u >> uint32_t(description.samples != xenos::MsaaSamples::k1X);
    uint32_t source_pitch = ((description.width / scale_x + tile_width - 1) / tile_width)
                            << uint32_t(wide);
    uint32_t rows = (description.height / scale_y + tile_height - 1) / tile_height;
    rex::graphics::DxbcRenderTargetDumpShader::Options options;
    options.resolution_scale_x = scale_x;
    options.resolution_scale_y = scale_y;
    options.portable_integer_division = true;
    options.msaa_2x_supported = description.msaa_2x_supported;
    options.depth_float24_convert_in_pixel_shader = false;
    options.depth_float24_round = round_float24;
    RenderTargetEncoder encoded(device, draws), original(device, draws);
    Require(encoded.Initialize(options, error), error);
    Require(encoded.Encode(source, 0, 0, source_pitch, source_pitch, source_pitch, rows, error),
            error);
    const RenderTargetEncoder* input = &encoded;
    if (!scaled && (scale_x != 1 || scale_y != 1)) {
      options.resolution_scale_x = options.resolution_scale_y = 1;
      Require(original.Initialize(options, error), error);
      Require(original.DownsampleFrom(encoded, 0, source_pitch, rows, source_pitch, wide,
                                      description.samples, error),
              error);
      input = &original;
    }
    auto& edram = description.depth ? resolve.depth_edram_info : resolve.color_edram_info;
    edram.pitch_tiles = source_pitch;
    edram.format = description.guest_format;
    edram.format_is_64bpp = wide;
    edram.msaa_samples = description.samples;
    edram.is_depth = description.depth;
    edram.base_tiles = (resolve.rect_y0 / tile_height) * source_pitch +
                       (resolve.rect_x0 / tile_width) * (wide ? 2 : 1);
    resolve.coordinate_info.edram_offset_x_div_8 = (resolve.rect_x0 % tile_width) / 8;
    resolve.coordinate_info.edram_offset_y_div_8 = (resolve.rect_y0 % tile_height) / 8;
    uint32_t dest_size_log2 =
        std::countr_zero(rex::graphics::FormatInfo::Get(
                             xenos::TextureFormat(resolve.copy_dest_info.copy_dest_format))
                             ->bits_per_pixel /
                         8);
    uint32_t x_mask = (1u << xenos::GetTextureTiledXBaseGranularityLog2(
                           resolve.copy_dest_info.copy_dest_array, dest_size_log2)) -
                      1u;
    uint32_t y_mask = (1u << xenos::GetTextureTiledYBaseGranularityLog2(
                           resolve.copy_dest_info.copy_dest_array, dest_size_log2)) -
                      1u;
    resolve.copy_dest_coordinate_info.offset_x_div_8 = (resolve.rect_x0 & x_mask) / 8;
    resolve.copy_dest_coordinate_info.offset_y_div_8 = (resolve.rect_y0 & y_mask) / 8;
    bool copy_scaled = scaled && (scale_x != 1 || scale_y != 1);
    draw::ResolveCopyShaderConstants constants = {};
    constants.dest_relative.edram_info = edram;
    constants.dest_relative.coordinate_info = resolve.coordinate_info;
    constants.dest_relative.coordinate_info.draw_resolution_scale_x = scaled ? scale_x : 1;
    constants.dest_relative.coordinate_info.draw_resolution_scale_y = scaled ? scale_y : 1;
    constants.dest_relative.dest_coordinate_info = resolve.copy_dest_coordinate_info;
    constants.dest_relative.dest_info = resolve.copy_dest_info;
    constants.dest_base = scaled && !copy_scaled ? view_desc.Buffer.FirstElement * 16
                                                 : resolve.copy_dest_base - destination_address;
    bool full =
        !description.depth &&
        (resolve.copy_dest_info.copy_dest_exp_bias != 0 ||
         !xenos::IsSingleCopySampleSelected(resolve.copy_dest_coordinate_info.copy_sample_select) ||
         !xenos::IsColorResolveFormatBitwiseEquivalent(
             xenos::ColorRenderTargetFormat(description.guest_format),
             xenos::ColorFormat(resolve.copy_dest_info.copy_dest_format)));
    auto shader = full ? (dest_size_log2 == 0u   ? draw::ResolveCopyShaderIndex::kFull8bpp
                          : dest_size_log2 == 1u ? draw::ResolveCopyShaderIndex::kFull16bpp
                          : dest_size_log2 == 2u ? draw::ResolveCopyShaderIndex::kFull32bpp
                          : dest_size_log2 == 3u ? draw::ResolveCopyShaderIndex::kFull64bpp
                                                 : draw::ResolveCopyShaderIndex::kFull128bpp)
                  : description.samples == xenos::MsaaSamples::k4X
                      ? (wide ? draw::ResolveCopyShaderIndex::kFast64bpp4xMSAA
                              : draw::ResolveCopyShaderIndex::kFast32bpp4xMSAA)
                      : (wide ? draw::ResolveCopyShaderIndex::kFast64bpp1x2xMSAA
                              : draw::ResolveCopyShaderIndex::kFast32bpp1x2xMSAA);
    uint32_t group_width = full   ? (dest_size_log2 == 0   ? 64
                                     : dest_size_log2 == 4 ? 16
                                                           : 32)
                           : wide ? 32
                                  : 64;
    uint32_t groups_x =
        ((resolve.coordinate_info.width_div_8 << 3) * (scaled ? scale_x : 1) + group_width - 1) /
        group_width;
    uint32_t groups_y = resolve.height_div_8 * (scaled ? scale_y : 1);
    D3D11_BUFFER_DESC description_buffer = {};
    description_buffer.ByteWidth = uint32_t(initial.size());
    description_buffer.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    D3D11_SUBRESOURCE_DATA data = {initial.data(), 0, 0};
    ComPtr<ID3D11Buffer> buffer, staging;
    Check(device.device()->CreateBuffer(&description_buffer, &data, buffer.GetAddressOf()),
          "General surface reference buffer");
    ComPtr<ID3D11UnorderedAccessView> view;
    auto reference_view = view_desc;
    uint32_t view_bytes = view_desc.Buffer.FirstElement * 16;
    uint32_t element_bytes = dest_size_log2 < 2u ? 8u : 16u;
    reference_view.Format =
        element_bytes == 8 ? DXGI_FORMAT_R32G32_UINT : DXGI_FORMAT_R32G32B32A32_UINT;
    reference_view.Buffer.FirstElement = view_bytes / element_bytes;
    reference_view.Buffer.NumElements = (uint32_t(initial.size()) - view_bytes) / element_bytes;
    if (scaled && !copy_scaled) {
      reference_view.Buffer.FirstElement = 0;
      reference_view.Buffer.NumElements = uint32_t(initial.size()) / element_bytes;
    }
    Check(device.device()->CreateUnorderedAccessView(buffer.Get(), &reference_view,
                                                     view.GetAddressOf()),
          "General surface reference view");
    bool copied =
        copy.Copy(*input, shader, constants, view.Get(), groups_x, groups_y, copy_scaled, error);
    Require(
        copied,
        error + " (general oracle: source " + std::to_string(description.guest_format) +
            ", destination " + std::to_string(uint32_t(resolve.copy_dest_info.copy_dest_format)) +
            ", samples " + std::to_string(1u << uint32_t(description.samples)) + ", selection " +
            std::to_string(uint32_t(resolve.copy_dest_coordinate_info.copy_sample_select)) + ")");
    description_buffer.BindFlags = 0;
    description_buffer.Usage = D3D11_USAGE_STAGING;
    description_buffer.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    Check(device.device()->CreateBuffer(&description_buffer, nullptr, staging.GetAddressOf()),
          "General surface reference staging");
    draws.Invalidate();
    device.context()->CopyResource(staging.Get(), buffer.Get());
    D3D11_MAPPED_SUBRESOURCE mapping = {};
    Check(device.context()->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapping),
          "General surface reference readback");
    const auto* bytes = static_cast<const uint8_t*>(mapping.pData);
    std::vector<uint8_t> result(bytes, bytes + initial.size());
    device.context()->Unmap(staging.Get(), 0);
    ++results.general_surface_comparisons;
    return result;
  }

  void NativeSurfaceCase(
      uint32_t scale_x, uint32_t scale_y, bool rgb10a2, uint32_t endian, bool swap, bool scaled,
      bool compact = false, const DepthCase* depth = nullptr,
      xenos::ColorRenderTargetFormat color_format = xenos::ColorRenderTargetFormat::k_8_8_8_8,
      uint32_t origin_x = 0, uint32_t origin_y = 0, bool array = false, uint32_t slice = 0,
      int32_t exp_bias = 0, xenos::MsaaSamples samples = xenos::MsaaSamples::k1X,
      xenos::CopySampleSelect selection = xenos::CopySampleSelect::k0,
      xenos::ColorFormat destination_format = xenos::ColorFormat(UINT32_MAX),
      bool native_2x = true) {
    constexpr uint32_t width = 64, height = 24, pitch = 128, destination_height = 64;
    const uint32_t bytes = array ? 4 * 1024 * 1024 : 256 * 1024;
    const uint32_t view_offset = scaled ? 4096 : 0, base = scaled ? 0 : 8192;
    const uint32_t destination_address = compact ? 4096 : 0;
    RenderTargetSurface::Description description;
    description.width = (origin_x + width) * scale_x;
    description.height = (origin_y + height) * scale_y;
    description.samples = samples;
    description.msaa_2x_supported = native_2x;
    if (rgb10a2)
      color_format = xenos::ColorRenderTargetFormat::k_2_10_10_10;
    rgb10a2 = color_format == xenos::ColorRenderTargetFormat::k_2_10_10_10;
    description.guest_format = uint32_t(color_format);
    if (depth) {
      description.depth = true;
      description.guest_format = uint32_t(depth->floating ? xenos::DepthRenderTargetFormat::kD24FS8
                                                          : xenos::DepthRenderTargetFormat::kD24S8);
    }
    std::string error;
    auto surface = RenderTargetSurface::Create(device, description, error);
    Require(bool(surface), error);
    bool source_wide = !depth && xenos::IsColorRenderTargetFormat64bpp(color_format);
    auto resolved_format = xenos::ColorFormat::k_8_8_8_8;
    switch (color_format) {
      case xenos::ColorRenderTargetFormat::k_2_10_10_10:
        resolved_format = xenos::ColorFormat::k_2_10_10_10;
        break;
      case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
        resolved_format = xenos::ColorFormat::k_16_16_FLOAT;
        break;
      case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT:
        resolved_format = xenos::ColorFormat::k_16_16_16_16_FLOAT;
        break;
      case xenos::ColorRenderTargetFormat::k_32_FLOAT:
        resolved_format = xenos::ColorFormat::k_32_FLOAT;
        break;
      case xenos::ColorRenderTargetFormat::k_32_32_FLOAT:
        resolved_format = xenos::ColorFormat::k_32_32_FLOAT;
        break;
      default:
        break;
    }
    bool override_format = uint32_t(destination_format) != UINT32_MAX;
    if (override_format)
      resolved_format = destination_format;
    bool converted = !depth && (override_format || !xenos::IsColorResolveFormatBitwiseEquivalent(
                                                       color_format, resolved_format));
    bool special_source = color_format == xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA ||
                          xenos::GetStorageColorFormat(color_format) ==
                              xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT;
    bool general_only = converted || special_source || samples != xenos::MsaaSamples::k1X;
    uint32_t dest_size_log2 = std::countr_zero(
        rex::graphics::FormatInfo::Get(xenos::TextureFormat(resolved_format))->bits_per_pixel / 8);
    bool wide = dest_size_log2 == 3u;
    uint32_t words = wide ? 2 : 1;
    uint32_t source_words = source_wide ? 2 : 1;
    auto tile = [&](uint32_t x, uint32_t y) {
      return array ? TextureAddress3D(x, y, slice, pitch, destination_height, dest_size_log2)
                   : TextureAddress(x, y, pitch, dest_size_log2);
    };
    std::vector<uint32_t> source(description.width * description.height * source_words);
    draws.Invalidate();
    if (samples != xenos::MsaaSamples::k1X || special_source) {
      DrawSamplePattern(*surface);
    } else if (depth) {
      device.context()->ClearDepthStencilView(surface->depth_view(),
                                              D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, depth->value,
                                              depth->stencil);
      D3D11_TEXTURE2D_DESC read_desc;
      surface->image()->GetDesc(&read_desc);
      read_desc.Usage = D3D11_USAGE_STAGING;
      read_desc.BindFlags = read_desc.MiscFlags = 0;
      read_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      ComPtr<ID3D11Texture2D> readback;
      Check(device.device()->CreateTexture2D(&read_desc, nullptr, readback.GetAddressOf()),
            "Depth source staging");
      device.context()->CopyResource(readback.Get(), surface->image());
      D3D11_MAPPED_SUBRESOURCE mapped = {};
      Check(device.context()->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped),
            "Depth source readback");
      for (uint32_t y = 0; y < description.height; ++y) {
        for (uint32_t x = 0; x < description.width; ++x) {
          const auto* pixel = static_cast<const uint8_t*>(mapped.pData) + y * mapped.RowPitch +
                              x * (depth->floating ? 8 : 4);
          uint32_t bits, stencil;
          std::memcpy(&bits, pixel, 4);
          if (depth->floating) {
            std::memcpy(&stencil, pixel + 4, 4);
            bits = FloatDepthReference(std::bit_cast<float>(bits), depth->round);
          } else {
            stencil = bits >> 24;
            bits &= 0xFFFFFFu;
          }
          source[y * description.width + x] = (bits << 8) | (stencil & 255);
        }
      }
      device.context()->Unmap(readback.Get(), 0);
    } else {
      for (uint32_t i = 0; i < source.size(); ++i)
        source[i] = SourceWord(i);
      device.context()->UpdateSubresource(surface->image(), 0, nullptr, source.data(),
                                          description.width * source_words * 4, 0);
    }

    D3D11_BUFFER_DESC desc = {};
    desc.ByteWidth = bytes;
    desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    std::vector<uint8_t> initial(bytes, 0xCD), expected = initial;
    D3D11_SUBRESOURCE_DATA data = {initial.data(), 0, 0};
    ComPtr<ID3D11Buffer> destination, staging;
    Check(device.device()->CreateBuffer(&desc, &data, destination.GetAddressOf()),
          "Native color resolve destination");
    D3D11_UNORDERED_ACCESS_VIEW_DESC view_desc = {};
    view_desc.Format = DXGI_FORMAT_R32G32B32A32_UINT;
    view_desc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    view_desc.Buffer.FirstElement = view_offset / 16;
    view_desc.Buffer.NumElements = (bytes - view_offset) / 16;
    ComPtr<ID3D11UnorderedAccessView> view;
    Check(device.device()->CreateUnorderedAccessView(destination.Get(), &view_desc,
                                                     view.GetAddressOf()),
          "Native color resolve view");

    draw::ResolveInfo resolve = {};
    resolve.coordinate_info.width_div_8 = width / 8;
    resolve.coordinate_info.draw_resolution_scale_x = scale_x;
    resolve.coordinate_info.draw_resolution_scale_y = scale_y;
    resolve.height_div_8 = height / 8;
    resolve.rect_x0 = origin_x;
    resolve.rect_y0 = origin_y;
    resolve.copy_dest_base_raw = base;
    uint32_t x_mask =
        (1u << xenos::GetTextureTiledXBaseGranularityLog2(array, dest_size_log2)) - 1u;
    uint32_t y_mask =
        (1u << xenos::GetTextureTiledYBaseGranularityLog2(array, dest_size_log2)) - 1u;
    resolve.copy_dest_base =
        base +
        (array ? TextureAddress3D(origin_x & ~x_mask, origin_y & ~y_mask, 0, pitch,
                                  destination_height, dest_size_log2)
               : TextureAddress(origin_x & ~x_mask, origin_y & ~y_mask, pitch, dest_size_log2));
    uint32_t first = UINT32_MAX, last = 0;
    for (uint32_t y = origin_y; y < origin_y + height; ++y)
      for (uint32_t x = origin_x; x < origin_x + width; ++x) {
        first = std::min(first, tile(x, y));
        last = std::max(last, tile(x, y) + (1u << dest_size_log2));
      }
    resolve.copy_dest_extent_start = base + first;
    resolve.copy_dest_extent_length = base + ((last + 15) & ~15u) - resolve.copy_dest_extent_start;
    resolve.copy_dest_coordinate_info.pitch_aligned_div_32 = pitch / 32;
    resolve.copy_dest_coordinate_info.height_aligned_div_32 = 2;
    resolve.copy_dest_coordinate_info.copy_sample_select = selection;
    resolve.copy_dest_info.copy_dest_endian = xenos::Endian128(endian);
    resolve.copy_dest_info.copy_dest_swap = swap;
    resolve.copy_dest_info.copy_dest_array = array;
    resolve.copy_dest_info.copy_dest_slice = slice;
    resolve.copy_dest_info.copy_dest_exp_bias = exp_bias;
    resolve.copy_dest_info.copy_dest_format =
        rgb10a2 ? xenos::ColorFormat::k_2_10_10_10 : xenos::ColorFormat::k_8_8_8_8;
    if (!depth) {
      switch (color_format) {
        case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
          resolve.copy_dest_info.copy_dest_format = xenos::ColorFormat::k_16_16_FLOAT;
          break;
        case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT:
          resolve.copy_dest_info.copy_dest_format = xenos::ColorFormat::k_16_16_16_16_FLOAT;
          break;
        case xenos::ColorRenderTargetFormat::k_32_FLOAT:
          resolve.copy_dest_info.copy_dest_format = xenos::ColorFormat::k_32_FLOAT;
          break;
        case xenos::ColorRenderTargetFormat::k_32_32_FLOAT:
          resolve.copy_dest_info.copy_dest_format = xenos::ColorFormat::k_32_32_FLOAT;
          break;
        default:
          break;
      }
    }
    if (depth) {
      resolve.rb_copy_control.copy_src_select = 4;
      resolve.copy_dest_info.copy_dest_format = xenos::ColorFormat(uint32_t(
          depth->floating ? xenos::TextureFormat::k_24_8_FLOAT : xenos::TextureFormat::k_24_8));
    }
    if (!depth)
      resolve.copy_dest_info.copy_dest_format = resolved_format;
    auto perform = [&](uint32_t address) {
      return depth ? copy.CopyDepthSurface(*surface, resolve, view.Get(), scaled, error, address,
                                           depth->round)
                   : copy.CopyColorSurface(*surface, resolve, view.Get(), scaled, error, address);
    };
    bool copied = perform(destination_address);
    Require(copied, error + " (format " + std::to_string(description.guest_format) + ", slice " +
                        std::to_string(slice) + ", bias " + std::to_string(exp_bias) + ", scale " +
                        std::to_string(scale_x) + "x" + std::to_string(scale_y) + ")");
    if (depth)
      ++results.native_depth_dispatches;
    else
      ++results.native_color_dispatches;
    if (!depth && surface->integer_transfer())
      ++results.native_float_dispatches;
    if (origin_x || origin_y)
      ++results.native_partial_dispatches;
    if (array)
      ++results.native_array_dispatches;
    if (exp_bias)
      ++results.native_bias_dispatches;
    if (samples != xenos::MsaaSamples::k1X)
      ++results.native_msaa_dispatches;
    if (converted)
      ++results.native_conversion_dispatches;
    ++results.dispatches;
    uint32_t output_scale_x = scaled ? scale_x : 1, output_scale_y = scaled ? scale_y : 1;
    if (!general_only)
      for (uint32_t y = 0; y < height * output_scale_y; ++y) {
        for (uint32_t x = 0; x < width * output_scale_x; ++x) {
          uint32_t sx = (scaled ? x : x * scale_x + scale_x / 2) + origin_x * scale_x;
          uint32_t sy = (scaled ? y : y * scale_y + scale_y / 2) + origin_y * scale_y;
          uint32_t word = source[(sy * description.width + sx) * words];
          if (swap && !surface->integer_transfer()) {
            word = rgb10a2 ? (word & 0xC00FFC00u) | ((word & 1023) << 20) | ((word >> 20) & 1023)
                           : (word & 0xFF00FF00u) | ((word & 255) << 16) | ((word >> 16) & 255);
          }
          uint32_t lanes = wide ? 2 : 4;
          uint32_t address =
              view_offset +
              (scaled ? (base - resolve.copy_dest_base) * output_scale_x * output_scale_y
                      : base - destination_address) +
              tile((x / lanes) / output_scale_x * lanes + origin_x, y / output_scale_y + origin_y) *
                  output_scale_x * output_scale_y +
              (((x / lanes) % output_scale_x) * output_scale_y + y % output_scale_y) * 16 +
              (x % lanes) * words * 4;
          for (uint32_t i = 0; i < words; ++i) {
            uint32_t value = i ? source[(sy * description.width + sx) * words + i] : word;
            if (swap && color_format == xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT) {
              uint32_t other = source[(sy * description.width + sx) * words + (i ^ 1)];
              value = (value & 0xFFFF0000u) | (other & 0xFFFFu);
            }
            // Swaps apply within a texel; the 8-in-64 mode is only meaningful
            // for 64-bit destinations. Permute individual bytes as a reference.
            for (uint32_t byte = 0; byte < 4; ++byte) {
              uint32_t target = (address + i * 4 + byte) ^ (endian == 1           ? 1
                                                            : endian == 2         ? 3
                                                            : endian == 3         ? 2
                                                            : endian == 4 && wide ? 7
                                                                                  : 0);
              Require(target < expected.size(), "Native color reference exceeds its buffer");
              expected[target] = uint8_t(value >> (byte * 8));
            }
            results.written_bytes += 4;
          }
        }
      }
    if (general_only || (!depth && (array || exp_bias || (swap && surface->integer_transfer())))) {
      auto reference =
          GeneralSurfaceReference(*surface, resolve, scale_x, scale_y, scaled, destination_address,
                                  view_desc, initial, depth && depth->round);
      if (!exp_bias && !general_only)
        Require(expected == reference, "Array address reference differs from the general resolve");
      expected = std::move(reference);
    }
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    Check(device.device()->CreateBuffer(&desc, nullptr, staging.GetAddressOf()),
          "Native color resolve staging");
    device.context()->CopyResource(staging.Get(), destination.Get());
    D3D11_MAPPED_SUBRESOURCE mapping = {};
    Check(device.context()->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapping),
          "Native color resolve readback");
    const auto* actual = static_cast<const uint8_t*>(mapping.pData);
    for (uint32_t i = 0; i < bytes; ++i) {
      if (actual[i] != expected[i]) {
        char message[400];
        std::snprintf(message, sizeof(message),
                      "Native color resolve mismatch at %u: %02X != %02X (scale %ux%u, "
                      "source %u, destination %u, endian %u, swap %u, scaled %u, origin %ux%u, "
                      "array %u, slice %u, bias %d, MSAA %u, selection %u)",
                      i, actual[i], expected[i], scale_x, scale_y, uint32_t(color_format),
                      uint32_t(resolve.copy_dest_info.copy_dest_format), endian, swap, scaled,
                      origin_x, origin_y, array, slice, exp_bias, 1u << uint32_t(samples),
                      uint32_t(selection));
        throw std::runtime_error(message);
      }
    }
    device.context()->Unmap(staging.Get(), 0);
    results.compared_bytes += bytes;
    resolve.rect_x0 = origin_x + 8;
    Require(!perform(0), "A native resolve beyond its source attachment was accepted");
    ++results.rejected;
    if (compact) {
      resolve.rect_x0 = origin_x;
      Require(!perform(resolve.copy_dest_extent_start + 16),
              "A compact resolve destination after its first written byte was accepted");
      ++results.rejected;
    }
  }

  void Case(RenderTargetEncoder& encoder, uint32_t scale_x, uint32_t scale_y, bool wide, bool depth,
            uint32_t samples_log2, uint32_t sample, uint32_t endian, bool swap,
            int full_shader = -1, uint32_t reference_scale_x = 1, uint32_t reference_scale_y = 1) {
    constexpr uint32_t destination_bytes = 1024 * 1024, width = 64, height = 16, pitch = 128;
    uint32_t size_log2 = wide ? 3 : 2;
    if (full_shader >= 0)
      size_log2 = uint32_t(full_shader);
    const bool scaled = scale_x != 1 || scale_y != 1;
    const uint32_t view_offset = scaled ? 4096 : 0;
    draw::ResolveCopyShaderConstants constants = {};
    auto& edram = constants.dest_relative.edram_info;
    edram.pitch_tiles = 11;
    edram.base_tiles = 2044;
    edram.msaa_samples = xenos::MsaaSamples(samples_log2);
    edram.is_depth = depth;
    edram.format = wide ? 7 : 0;
    edram.format_is_64bpp = wide;
    auto& coordinate = constants.dest_relative.coordinate_info;
    coordinate.edram_offset_x_div_8 = 3;
    coordinate.edram_offset_y_div_8 = 1;
    coordinate.width_div_8 = width / 8;
    coordinate.draw_resolution_scale_x = scale_x;
    coordinate.draw_resolution_scale_y = scale_y;
    auto& target = constants.dest_relative.dest_coordinate_info;
    target.pitch_aligned_div_32 = pitch / 32;
    target.height_aligned_div_32 = 64 / 32;
    target.offset_x_div_8 = 2;
    target.offset_y_div_8 = 3;
    target.copy_sample_select = xenos::CopySampleSelect(sample);
    constants.dest_relative.dest_info.value = 0;
    constants.dest_relative.dest_info.copy_dest_endian = xenos::Endian128(endian);
    constants.dest_relative.dest_info.copy_dest_swap = swap;
    constexpr std::array<xenos::ColorFormat, 5> full_formats = {
        xenos::ColorFormat::k_8, xenos::ColorFormat::k_16, xenos::ColorFormat::k_8_8_8_8,
        xenos::ColorFormat::k_16_16_16_16, xenos::ColorFormat::k_32_32_32_32_FLOAT};
    constants.dest_relative.dest_info.copy_dest_format =
        full_shader >= 0 ? full_formats[size_log2]
        : wide           ? xenos::ColorFormat::k_16_16_16_16_FLOAT
                         : xenos::ColorFormat::k_8_8_8_8;
    constants.dest_base = 4096;
    auto shader = full_shader >= 0
                      ? draw::ResolveCopyShaderIndex(
                            uint32_t(draw::ResolveCopyShaderIndex::kFull8bpp) + size_log2)
                  : wide ? (samples_log2 == 2 ? draw::ResolveCopyShaderIndex::kFast64bpp4xMSAA
                                              : draw::ResolveCopyShaderIndex::kFast64bpp1x2xMSAA)
                         : (samples_log2 == 2 ? draw::ResolveCopyShaderIndex::kFast32bpp4xMSAA
                                              : draw::ResolveCopyShaderIndex::kFast32bpp1x2xMSAA);
    uint32_t group_size = full_shader == 0           ? 64
                          : full_shader == 4         ? 16
                          : full_shader >= 0 || wide ? 32
                                                     : 64;
    uint32_t groups_x = (width * scale_x + group_size - 1) / group_size;
    uint32_t groups_y = height * scale_y / 8;
    D3D11_BUFFER_DESC desc = {};
    desc.ByteWidth = destination_bytes;
    desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    std::vector<uint8_t> initial(destination_bytes, 0xCD), expected = initial;
    D3D11_SUBRESOURCE_DATA initial_data = {initial.data(), 0, 0};
    ComPtr<ID3D11Buffer> destination, staging;
    Check(device.device()->CreateBuffer(&desc, &initial_data, destination.GetAddressOf()),
          "Resolve destination buffer");
    D3D11_UNORDERED_ACCESS_VIEW_DESC view_desc = {};
    view_desc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    uint32_t view_size_log2 = full_shader == 0 || full_shader == 1 ? 3 : 4;
    view_desc.Format =
        view_size_log2 == 3 ? DXGI_FORMAT_R32G32_UINT : DXGI_FORMAT_R32G32B32A32_UINT;
    view_desc.Buffer.FirstElement = view_offset >> view_size_log2;
    view_desc.Buffer.NumElements = (destination_bytes - view_offset) >> view_size_log2;
    ComPtr<ID3D11UnorderedAccessView> view;
    Check(device.device()->CreateUnorderedAccessView(destination.Get(), &view_desc,
                                                     view.GetAddressOf()),
          "Resolve destination view");
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    Check(device.device()->CreateBuffer(&desc, nullptr, staging.GetAddressOf()),
          "Resolve destination staging");
    for (uint32_t y = 0; y < height * scale_y; ++y)
      for (uint32_t x = 0; x < width * scale_x; ++x) {
        uint32_t sx = (coordinate.edram_offset_x_div_8 * 8 * scale_x + x)
                      << uint32_t(samples_log2 == 2);
        uint32_t sy = (coordinate.edram_offset_y_div_8 * 8 * scale_y + y)
                      << uint32_t(samples_log2 != 0);
        sx += sample >> 1;
        sy += sample & 1;
        uint32_t tile_width = (wide ? 40 : 80) * scale_x, tile_height = 16 * scale_y;
        uint32_t tile =
            (edram.base_tiles + (sy / tile_height) * edram.pitch_tiles + sx / tile_width) % 2048;
        uint32_t sample_x = sx % tile_width;
        if (depth)
          sample_x = (sample_x + tile_width / 2) % tile_width;
        uint32_t source_y = sy % tile_height;
        if (reference_scale_x != 1 || reference_scale_y != 1) {
          // The mirror keeps each original MSAA sample and chooses the center
          // host pixel in its scaled footprint. Project after depth's tile swap.
          uint32_t sample_mask_x = uint32_t(samples_log2 == 2);
          uint32_t sample_mask_y = uint32_t(samples_log2 != 0);
          sample_x = (((sample_x >> sample_mask_x) * reference_scale_x + reference_scale_x / 2)
                      << sample_mask_x) +
                     (sample_x & sample_mask_x);
          source_y = (((source_y >> sample_mask_y) * reference_scale_y + reference_scale_y / 2)
                      << sample_mask_y) +
                     (source_y & sample_mask_y);
          tile_width *= reference_scale_x;
          tile_height *= reference_scale_y;
        }
        uint32_t word_index =
            (tile * tile_width * tile_height + source_y * tile_width + sample_x) * (wide ? 2 : 1);
        std::array<uint32_t, 4> value = {SourceWord(word_index), SourceWord(word_index + 1), 0, 0};
        if (swap) {
          if (wide) {
            uint32_t r = value[0] & 65535, b = value[1] & 65535;
            value[0] = (value[0] & 0xFFFF0000u) | b;
            value[1] = (value[1] & 0xFFFF0000u) | r;
          } else {
            value[0] =
                (value[0] & 0xFF00FF00u) | ((value[0] & 255) << 16) | ((value[0] >> 16) & 255);
          }
        }
        if (full_shader >= 0) {
          uint32_t rgba = value[0];
          if (size_log2 == 0)
            value[0] = rgba & 255;
          else if (size_log2 == 1)
            value[0] = (rgba & 255) * 257;
          else if (size_log2 == 3) {
            value[0] = (rgba & 255) * 257 | (((rgba >> 8) & 255) * 257 << 16);
            value[1] = ((rgba >> 16) & 255) * 257 | ((rgba >> 24) * 257 << 16);
          } else if (size_log2 == 4) {
            for (uint32_t c = 0; c < 4; ++c)
              value[c] = std::bit_cast<uint32_t>(float((rgba >> (c * 8)) & 255) * (1.0f / 255));
          }
        }
        for (auto& word : value)
          word = EndianWord(word, endian);
        // Scaled guest storage expands a typed buffer element, rather than
        // each individual texel. Its X subelement precedes Y in that block.
        uint32_t block_size = size_log2 == 0 ? 8 : 16;
        uint32_t pixels_per_block = block_size >> size_log2;
        uint32_t host_x = x + target.offset_x_div_8 * 8 * scale_x;
        uint32_t block_x = host_x / pixels_per_block;
        uint32_t guest_x = block_x / scale_x * pixels_per_block;
        uint32_t address =
            TextureAddress(guest_x, y / scale_y + target.offset_y_div_8 * 8, pitch, size_log2);
        address = address * scale_x * scale_y +
                  ((block_x % scale_x) * scale_y + y % scale_y) * block_size +
                  (host_x % pixels_per_block) * (1u << size_log2);
        if (!scaled)
          address += constants.dest_base;
        address += view_offset;
        Require(address + (1u << size_log2) <= expected.size(), "CPU resolve reference extent");
        std::memcpy(expected.data() + address, value.data(), 1u << size_log2);
        results.written_bytes += 1u << size_log2;
      }
    std::string error;
    bool copied =
        copy.Copy(encoder, shader, constants, view.Get(), groups_x, groups_y, scaled, error);
    Require(copied, "Resolve shader " + std::to_string(unsigned(shader)) + ": " + error);
    ++results.dispatches;
    results.offset_dispatches += scaled;
    auto invalid = constants;
    invalid.dest_relative.coordinate_info.draw_resolution_scale_x = 0;
    Require(!copy.Copy(encoder, shader, invalid, view.Get(), groups_x, groups_y, scaled, error),
            "Zero resolution scale was accepted");
    Require(!copy.Copy(encoder, draw::ResolveCopyShaderIndex::kUnknown, constants, view.Get(),
                       groups_x, groups_y, scaled, error),
            "Invalid resolve shader was accepted");
    Require(
        !copy.Copy(encoder, shader, constants, view.Get(), groups_x + 1, groups_y, scaled, error),
        "Invalid resolve dispatch width was accepted");
    results.rejected += 3;
    draws.Invalidate();
    device.context()->CopyResource(staging.Get(), destination.Get());
    Check(device.WaitForCompletion(), "Native resolve completion");
    D3D11_MAPPED_SUBRESOURCE mapping = {};
    Check(device.context()->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapping),
          "Native resolve map");
    const auto* actual = static_cast<const uint8_t*>(mapping.pData);
    size_t mismatch = expected.size();
    uint32_t actual_byte = 0;
    for (size_t i = 0; i < expected.size(); ++i)
      if (actual[i] != expected[i]) {
        mismatch = i;
        actual_byte = actual[i];
        break;
      }
    device.context()->Unmap(staging.Get(), 0);
    if (mismatch != expected.size()) {
      char message[256];
      std::snprintf(message, sizeof(message),
                    "Resolve mismatch shader=%u scale=%ux%u wide=%u depth=%u msaa=%u sample=%u "
                    "endian=%u swap=%u byte=%llu actual=%02X expected=%02X",
                    unsigned(shader), scale_x, scale_y, unsigned(wide), unsigned(depth),
                    samples_log2, sample, endian, unsigned(swap),
                    static_cast<unsigned long long>(mismatch), actual_byte, expected[mismatch]);
      throw std::runtime_error(message);
    }
    results.compared_bytes += expected.size();
    results.mirrored_dispatches += reference_scale_x != 1 || reference_scale_y != 1;
  }
};
}  // namespace

int main(int argc, char** argv) {
  std::string output;
  try {
    D3D11Device::Options options;
    options.debug = true;
    options.maximum_feature_level = D3D_FEATURE_LEVEL_11_0;
    bool native_colors_only = false, native_depth_only = false;
    std::fesetround(FE_TONEAREST);
    for (int i = 1; i < argc; ++i) {
      std::string arg(argv[i]);
      if (arg == "--warp")
        options.warp = true;
      else if (arg == "--output" && i + 1 < argc)
        output = argv[++i];
      else if (arg == "--native-colors-only")
        native_colors_only = true;
      else if (arg == "--native-depth-only")
        native_depth_only = true;
      else
        throw std::runtime_error(
            "usage: d3d11_resolve_check [--warp] [--native-colors-only | --native-depth-only] "
            "[--output report.json]");
    }
    std::string error;
    auto device = D3D11Device::Create(options, error);
    Require(bool(device), error);
    Fixture fixture(*device);
    if (!native_depth_only)
      for (const auto& scale :
           std::array<std::array<uint32_t, 2>, 6>{{{1, 1}, {2, 2}, {2, 1}, {1, 2}, {3, 1}, {3, 3}}})
        for (bool rgb10a2 : {false, true})
          for (uint32_t endian = 0; endian < 4; ++endian)
            for (bool swap : {false, true}) {
              for (bool scaled : {false, true})
                fixture.NativeSurfaceCase(scale[0], scale[1], rgb10a2, endian, swap, scaled);
              fixture.NativeSurfaceCase(scale[0], scale[1], rgb10a2, endian, swap, false, true);
            }
    if (!native_depth_only) {
      for (auto format :
           {xenos::ColorRenderTargetFormat::k_8_8_8_8, xenos::ColorRenderTargetFormat::k_2_10_10_10,
            xenos::ColorRenderTargetFormat::k_16_16_FLOAT,
            xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT,
            xenos::ColorRenderTargetFormat::k_32_FLOAT,
            xenos::ColorRenderTargetFormat::k_32_32_FLOAT})
        for (uint32_t scale : {1u, 2u, 3u})
          for (uint32_t endian = 0; endian < 6; ++endian)
            for (const auto& origin :
                 std::array<std::array<uint32_t, 2>, 3>{{{0, 0}, {8, 8}, {40, 40}}})
              for (bool scaled : {false, true})
                fixture.NativeSurfaceCase(
                    scale, scale, format == xenos::ColorRenderTargetFormat::k_2_10_10_10, endian,
                    false, scaled, !scaled, nullptr, format, origin[0], origin[1]);
    }
    if (!native_depth_only) {
      for (auto format : {xenos::ColorRenderTargetFormat::k_8_8_8_8,
                          xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT,
                          xenos::ColorRenderTargetFormat::k_32_32_FLOAT})
        for (uint32_t scale : {1u, 2u})
          for (uint32_t slice : {0u, 1u, 4u, 7u})
            for (uint32_t endian : {2u, 4u})
              for (bool scaled : {false, true})
                fixture.NativeSurfaceCase(scale, scale, false, endian, false, scaled, !scaled,
                                          nullptr, format, 40, 40, true, slice);
      for (auto format :
           {xenos::ColorRenderTargetFormat::k_8_8_8_8, xenos::ColorRenderTargetFormat::k_2_10_10_10,
            xenos::ColorRenderTargetFormat::k_16_16_FLOAT,
            xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT,
            xenos::ColorRenderTargetFormat::k_32_FLOAT,
            xenos::ColorRenderTargetFormat::k_32_32_FLOAT})
        for (uint32_t scale : {1u, 2u})
          for (int32_t bias : {-8, -1, 1, 8})
            for (bool scaled : {false, true})
              fixture.NativeSurfaceCase(scale, scale, false, 2, false, scaled, !scaled, nullptr,
                                        format, 8, 8, false, 0, bias);
      for (auto format : {xenos::ColorRenderTargetFormat::k_16_16_FLOAT,
                          xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT,
                          xenos::ColorRenderTargetFormat::k_32_FLOAT,
                          xenos::ColorRenderTargetFormat::k_32_32_FLOAT})
        for (uint32_t scale : {1u, 2u})
          for (int32_t bias : {-1, 0, 1})
            for (bool scaled : {false, true})
              fixture.NativeSurfaceCase(scale, scale, false, 4, true, scaled, !scaled, nullptr,
                                        format, 40, 40, false, 0, bias);
      for (auto format : {xenos::ColorRenderTargetFormat::k_8_8_8_8,
                          xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT})
        for (uint32_t slice : {1u, 7u})
          fixture.NativeSurfaceCase(2, 2, false, 2, false, true, false, nullptr, format, 40, 40,
                                    true, slice, -1);
    }
    if (!native_colors_only) {
      for (const auto& scale :
           std::array<std::array<uint32_t, 2>, 6>{{{1, 1}, {2, 2}, {2, 1}, {1, 2}, {3, 1}, {3, 3}}})
        for (bool floating : {false, true})
          for (bool round : {false, true}) {
            if (!floating && round)
              continue;
            for (uint32_t endian = 0; endian < 4; ++endian)
              for (bool scaled : {false, true}) {
                DepthCase depth{floating, round, std::bit_cast<float>(0x3E23456Bu),
                                uint8_t(0xA7 ^ (scale[0] << 4) ^ endian)};
                fixture.NativeSurfaceCase(scale[0], scale[1], false, endian, false, scaled, !scaled,
                                          &depth);
              }
          }
      const std::array values = {0.0f,
                                 std::ldexp(1.0f, -36),
                                 std::ldexp(1.0f, -35),
                                 std::bit_cast<float>(0x37FFFFFFu),
                                 std::ldexp(1.0f, -15),
                                 0.5f,
                                 std::bit_cast<float>(0x3F7FFFFFu),
                                 1.0f};
      for (bool round : {false, true})
        for (uint32_t i = 0; i < values.size(); ++i) {
          DepthCase depth{true, round, values[i], uint8_t(i == 0 ? 0 : i == 1 ? 255 : i * 17)};
          fixture.NativeSurfaceCase(1, 1, false, 2, false, false, true, &depth);
        }
      for (bool floating : {false, true})
        for (uint32_t scale : {1u, 2u})
          for (uint32_t endian = 0; endian < 6; ++endian)
            for (bool scaled : {false, true}) {
              DepthCase depth{floating, true, 0.3125f, 0xB3};
              fixture.NativeSurfaceCase(scale, scale, false, endian, false, scaled, !scaled, &depth,
                                        xenos::ColorRenderTargetFormat::k_8_8_8_8, 40, 40);
            }
      for (bool floating : {false, true})
        for (uint32_t scale : {1u, 2u})
          for (uint32_t slice : {1u, 4u, 7u})
            for (bool scaled : {false, true}) {
              DepthCase depth{floating, true, 0.3125f, 0xD3};
              fixture.NativeSurfaceCase(scale, scale, false, 2, false, scaled, !scaled, &depth,
                                        xenos::ColorRenderTargetFormat::k_8_8_8_8, 40, 40, true,
                                        slice);
            }
    }
    const std::array<xenos::ColorRenderTargetFormat, 10> native_sources = {
        xenos::ColorRenderTargetFormat::k_8_8_8_8,
        xenos::ColorRenderTargetFormat::k_2_10_10_10,
        xenos::ColorRenderTargetFormat::k_16_16_FLOAT,
        xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT,
        xenos::ColorRenderTargetFormat::k_32_FLOAT,
        xenos::ColorRenderTargetFormat::k_32_32_FLOAT,
        xenos::ColorRenderTargetFormat::k_16_16,
        xenos::ColorRenderTargetFormat::k_16_16_16_16,
        xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA,
        xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT};
    const std::array<xenos::ColorFormat, 21> native_destinations = {
        xenos::ColorFormat::k_8_8_8_8,
        xenos::ColorFormat::k_2_10_10_10,
        xenos::ColorFormat::k_16_16_FLOAT,
        xenos::ColorFormat::k_16_16_16_16_FLOAT,
        xenos::ColorFormat::k_32_FLOAT,
        xenos::ColorFormat::k_32_32_FLOAT,
        xenos::ColorFormat::k_10_11_11,
        xenos::ColorFormat::k_11_11_10,
        xenos::ColorFormat::k_16_16,
        xenos::ColorFormat::k_16_16_16_16,
        xenos::ColorFormat::k_8,
        xenos::ColorFormat::k_8_A,
        xenos::ColorFormat::k_8_B,
        xenos::ColorFormat::k_8_8,
        xenos::ColorFormat::k_1_5_5_5,
        xenos::ColorFormat::k_5_6_5,
        xenos::ColorFormat::k_6_5_5,
        xenos::ColorFormat::k_4_4_4_4,
        xenos::ColorFormat::k_16,
        xenos::ColorFormat::k_16_FLOAT,
        xenos::ColorFormat::k_32_32_32_32_FLOAT};
    if (!native_depth_only) {
      // Full 3x and asymmetric scales exercise both axes of native addressing,
      // including integer sources, array slices and multisample conversion.
      for (auto format : native_sources)
        for (const auto& scale : std::array<std::array<uint32_t, 2>, 3>{{{3, 3}, {2, 3}, {3, 2}}})
          for (bool scaled : {false, true})
            fixture.NativeSurfaceCase(scale[0], scale[1], false, 2, false, scaled, !scaled, nullptr,
                                      format, 40, 40, true, 7, -1, xenos::MsaaSamples::k4X,
                                      xenos::CopySampleSelect::k0123);
      for (auto format : native_sources)
        for (auto samples : {xenos::MsaaSamples::k2X, xenos::MsaaSamples::k4X})
          for (bool native_2x : {false, true}) {
            if (samples == xenos::MsaaSamples::k4X && !native_2x)
              continue;
            for (uint32_t sample = 0; sample <= 6; ++sample) {
              if (samples == xenos::MsaaSamples::k2X && sample != 0 && sample != 1 && sample != 4)
                continue;
              for (uint32_t scale : {1u, 2u})
                for (bool scaled : {false, true})
                  fixture.NativeSurfaceCase(
                      scale, scale, false, 2, sample & 1u, scaled, !scaled, nullptr, format, 40, 40,
                      sample == 5u, 7u, sample == 6u ? -1 : 0, samples,
                      xenos::CopySampleSelect(sample), xenos::ColorFormat(UINT32_MAX), native_2x);
            }
          }
      for (auto source : native_sources)
        for (auto destination : native_destinations) {
          if (xenos::IsColorResolveFormatBitwiseEquivalent(source, destination))
            continue;
          for (uint32_t scale : {1u, 2u})
            for (bool scaled : {false, true})
              fixture.NativeSurfaceCase(scale, scale, false, 4, true, scaled, !scaled, nullptr,
                                        source, 8, 8, false, 0, 0, xenos::MsaaSamples::k1X,
                                        xenos::CopySampleSelect::k0, destination);
          fixture.NativeSurfaceCase(2, 2, false, 2, true, true, false, nullptr, source, 40, 40,
                                    true, 7, -1, xenos::MsaaSamples::k4X,
                                    xenos::CopySampleSelect::k0123, destination);
        }
    }
    if (!native_colors_only)
      for (bool floating : {false, true})
        for (bool round : {false, true}) {
          if (!floating && round)
            continue;
          DepthCase depth{floating, round, 0.3125f, 0xB3};
          for (auto samples : {xenos::MsaaSamples::k2X, xenos::MsaaSamples::k4X})
            for (bool native_2x : {false, true}) {
              if (samples == xenos::MsaaSamples::k4X && !native_2x)
                continue;
              for (uint32_t sample = 0; sample < (1u << uint32_t(samples)); ++sample)
                for (uint32_t scale : {1u, 2u})
                  for (bool scaled : {false, true})
                    fixture.NativeSurfaceCase(scale, scale, false, 2, false, scaled, !scaled,
                                              &depth, xenos::ColorRenderTargetFormat::k_8_8_8_8, 40,
                                              40, sample == 3u, 4u, 0, samples,
                                              xenos::CopySampleSelect(sample),
                                              xenos::ColorFormat(UINT32_MAX), native_2x);
            }
        }
    for (const auto& scale :
         std::array<std::array<uint32_t, 2>, 5>{{{1, 1}, {2, 2}, {2, 1}, {1, 2}, {3, 1}}}) {
      if (native_colors_only || native_depth_only)
        break;
      RenderTargetEncoder encoder(*device, fixture.draws);
      rex::graphics::DxbcRenderTargetDumpShader::Options encoder_options;
      encoder_options.resolution_scale_x = scale[0];
      encoder_options.resolution_scale_y = scale[1];
      Require(encoder.Initialize(encoder_options, error), error);
      std::vector<uint32_t> input(encoder.byte_size() / 4);
      for (uint32_t i = 0; i < input.size(); ++i)
        input[i] = SourceWord(i);
      fixture.draws.Invalidate();
      device->context()->UpdateSubresource(encoder.buffer(), 0, nullptr, input.data(), 0, 0);
      for (uint32_t msaa = 0; msaa < 3; ++msaa)
        for (uint32_t sample = 0; sample < (1u << msaa); ++sample)
          for (uint32_t endian = 0; endian < 4; ++endian) {
            for (bool wide : {false, true})
              for (bool swap : {false, true})
                fixture.Case(encoder, scale[0], scale[1], wide, false, msaa, sample, endian, swap);
            fixture.Case(encoder, scale[0], scale[1], false, true, msaa, sample, endian, false);
          }
      for (int size = 0; size < 5; ++size)
        fixture.Case(encoder, scale[0], scale[1], false, false, 0, 0, 0, false, size);
      if (scale[0] != 1 || scale[1] != 1) {
        RenderTargetEncoder mirror(*device, fixture.draws);
        rex::graphics::DxbcRenderTargetDumpShader::Options mirror_options;
        Require(mirror.Initialize(mirror_options, error), error);
        for (uint32_t msaa = 0; msaa < 3; ++msaa)
          for (uint32_t sample = 0; sample < (1u << msaa); ++sample)
            for (uint32_t endian = 0; endian < 4; ++endian) {
              for (bool wide : {false, true}) {
                Require(mirror.DownsampleFrom(encoder, 2044, 11, 4, 11, wide,
                                              xenos::MsaaSamples(msaa), error),
                        error);
                for (bool swap : {false, true})
                  fixture.Case(mirror, 1, 1, wide, false, msaa, sample, endian, swap, -1, scale[0],
                               scale[1]);
              }
              Require(mirror.DownsampleFrom(encoder, 2044, 11, 4, 11, false,
                                            xenos::MsaaSamples(msaa), error),
                      error);
              fixture.Case(mirror, 1, 1, false, true, msaa, sample, endian, false, -1, scale[0],
                           scale[1]);
            }
        Require(
            mirror.DownsampleFrom(encoder, 2044, 11, 4, 11, false, xenos::MsaaSamples::k1X, error),
            error);
        for (int size = 0; size < 5; ++size)
          fixture.Case(mirror, 1, 1, false, false, 0, 0, 0, false, size, scale[0], scale[1]);
        Require(
            !mirror.DownsampleFrom(encoder, 2044, 0, 4, 11, false, xenos::MsaaSamples::k1X, error),
            "An empty mirror span was accepted");
        Require(
            !mirror.DownsampleFrom(mirror, 2044, 11, 4, 11, false, xenos::MsaaSamples::k1X, error),
            "An aliased mirror buffer was accepted");
        fixture.results.rejected += 2;
      }
    }
    fixture.draws.Invalidate();
    Check(device->WaitForCompletion(), "Resolve fixture completion");
    ComPtr<ID3D11InfoQueue> messages;
    if (SUCCEEDED(device->device()->QueryInterface(IID_PPV_ARGS(messages.GetAddressOf())))) {
      for (uint64_t i = 0; i < messages->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
        SIZE_T size = 0;
        Check(messages->GetMessage(i, nullptr, &size), "Resolve debug message size");
        std::vector<uint8_t> data(size);
        auto* message = reinterpret_cast<D3D11_MESSAGE*>(data.data());
        Check(messages->GetMessage(i, message, &size), "Resolve debug message");
        if (message->Severity <= D3D11_MESSAGE_SEVERITY_WARNING)
          throw std::runtime_error(message->pDescription);
      }
    }
    const auto& result = fixture.results;
    std::string report =
        "{\"passed\":true,\"scope\":\"native tiled resolve compute copies\",\"feature_level\":" +
        std::to_string(device->features().level) +
        ",\"software\":" + (device->features().software ? "true" : "false") +
        ",\"dispatches\":" + std::to_string(result.dispatches) +
        ",\"offset_view_dispatches\":" + std::to_string(result.offset_dispatches) +
        ",\"mirrored_dispatches\":" + std::to_string(result.mirrored_dispatches) +
        ",\"native_color_dispatches\":" + std::to_string(result.native_color_dispatches) +
        ",\"native_depth_dispatches\":" + std::to_string(result.native_depth_dispatches) +
        ",\"native_float_dispatches\":" + std::to_string(result.native_float_dispatches) +
        ",\"native_partial_dispatches\":" + std::to_string(result.native_partial_dispatches) +
        ",\"native_array_dispatches\":" + std::to_string(result.native_array_dispatches) +
        ",\"native_bias_dispatches\":" + std::to_string(result.native_bias_dispatches) +
        ",\"native_msaa_dispatches\":" + std::to_string(result.native_msaa_dispatches) +
        ",\"native_conversion_dispatches\":" + std::to_string(result.native_conversion_dispatches) +
        ",\"general_surface_comparisons\":" + std::to_string(result.general_surface_comparisons) +
        ",\"compared_bytes\":" + std::to_string(result.compared_bytes) +
        ",\"written_bytes\":" + std::to_string(result.written_bytes) +
        ",\"rejected_dispatches\":" + std::to_string(result.rejected) + "}\n";
    std::fputs(report.c_str(), stdout);
    if (!output.empty()) {
      std::ofstream file(output);
      file << report;
      Require(bool(file), "Could not write resolve qualification");
    }
    return 0;
  } catch (const std::exception& failure) {
    std::fprintf(stderr, "FAIL: %s\n", failure.what());
    if (!output.empty()) {
      std::string message;
      for (char c : std::string(failure.what())) {
        if (c == '\\' || c == '"')
          message += '\\';
        message += c == '\n' || c == '\r' ? ' ' : c;
      }
      std::ofstream file(output);
      file << "{\"passed\":false,\"error\":\"" << message << "\"}\n";
    }
    return 1;
  }
}
