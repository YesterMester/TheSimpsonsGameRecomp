#include <rex/graphics/d3d11/resolve_copy.h>

#include <algorithm>
#include <cstring>

#include <rex/graphics/util/native_color_resolve.h>
#include <rex/graphics/util/native_depth_resolve.h>
#include <rex/graphics/util/native_resolve_source.h>

namespace rex::graphics::d3d11 {
namespace {
namespace shaders {
#include "../shaders/bytecode/d3d12_5_1/resolve_fast_32bpp_1x2xmsaa_cs.h"
#include "../shaders/bytecode/d3d12_5_1/resolve_fast_32bpp_4xmsaa_cs.h"
#include "../shaders/bytecode/d3d12_5_1/resolve_fast_64bpp_1x2xmsaa_cs.h"
#include "../shaders/bytecode/d3d12_5_1/resolve_fast_64bpp_4xmsaa_cs.h"
#include "../shaders/bytecode/d3d12_5_1/resolve_full_8bpp_cs.h"
#include "../shaders/bytecode/d3d12_5_1/resolve_full_16bpp_cs.h"
#include "../shaders/bytecode/d3d12_5_1/resolve_full_32bpp_cs.h"
#include "../shaders/bytecode/d3d12_5_1/resolve_full_64bpp_cs.h"
#include "../shaders/bytecode/d3d12_5_1/resolve_full_128bpp_cs.h"
#include "../shaders/bytecode/d3d12_5_1/resolve_fast_32bpp_1x2xmsaa_scaled_cs.h"
#include "../shaders/bytecode/d3d12_5_1/resolve_fast_32bpp_4xmsaa_scaled_cs.h"
#include "../shaders/bytecode/d3d12_5_1/resolve_fast_64bpp_1x2xmsaa_scaled_cs.h"
#include "../shaders/bytecode/d3d12_5_1/resolve_fast_64bpp_4xmsaa_scaled_cs.h"
#include "../shaders/bytecode/d3d12_5_1/resolve_full_8bpp_scaled_cs.h"
#include "../shaders/bytecode/d3d12_5_1/resolve_full_16bpp_scaled_cs.h"
#include "../shaders/bytecode/d3d12_5_1/resolve_full_32bpp_scaled_cs.h"
#include "../shaders/bytecode/d3d12_5_1/resolve_full_64bpp_scaled_cs.h"
#include "../shaders/bytecode/d3d12_5_1/resolve_full_128bpp_scaled_cs.h"
}  // namespace shaders

struct CopyProgram {
  std::span<const uint8_t> code;
  bool raw_source;
  uint32_t source_bpe_log2, destination_bpe_log2, group_x_log2;
};
#define COPY_PROGRAM(name, raw, source, destination, group_x) \
  {shaders::name, raw, source, destination, group_x}
const std::array<std::array<CopyProgram, 9>, 2> kPrograms = {{
    {{COPY_PROGRAM(resolve_fast_32bpp_1x2xmsaa_cs, false, 4, 4, 6),
      COPY_PROGRAM(resolve_fast_32bpp_4xmsaa_cs, false, 4, 4, 6),
      COPY_PROGRAM(resolve_fast_64bpp_1x2xmsaa_cs, false, 4, 4, 5),
      COPY_PROGRAM(resolve_fast_64bpp_4xmsaa_cs, false, 3, 4, 5),
      COPY_PROGRAM(resolve_full_8bpp_cs, true, 2, 3, 6),
      COPY_PROGRAM(resolve_full_16bpp_cs, true, 2, 3, 5),
      COPY_PROGRAM(resolve_full_32bpp_cs, true, 2, 4, 5),
      COPY_PROGRAM(resolve_full_64bpp_cs, true, 2, 4, 5),
      COPY_PROGRAM(resolve_full_128bpp_cs, true, 2, 4, 4)}},
    {{COPY_PROGRAM(resolve_fast_32bpp_1x2xmsaa_scaled_cs, false, 4, 4, 6),
      COPY_PROGRAM(resolve_fast_32bpp_4xmsaa_scaled_cs, false, 4, 4, 6),
      COPY_PROGRAM(resolve_fast_64bpp_1x2xmsaa_scaled_cs, false, 4, 4, 5),
      COPY_PROGRAM(resolve_fast_64bpp_4xmsaa_scaled_cs, false, 3, 4, 5),
      COPY_PROGRAM(resolve_full_8bpp_scaled_cs, true, 2, 3, 6),
      COPY_PROGRAM(resolve_full_16bpp_scaled_cs, true, 2, 3, 5),
      COPY_PROGRAM(resolve_full_32bpp_scaled_cs, true, 2, 4, 5),
      COPY_PROGRAM(resolve_full_64bpp_scaled_cs, true, 2, 4, 5),
      COPY_PROGRAM(resolve_full_128bpp_scaled_cs, true, 2, 4, 4)}},
}};
#undef COPY_PROGRAM
static_assert(kPrograms[0].size() == size_t(draw_util::ResolveCopyShaderIndex::kCount));

// Four-sample depth tiles swap their 40-sample halves. A vector load across
// that boundary cannot use consecutive source words: the logical continuation
// is in the other half of the same tile, rather than the following tile/row.
// Address each selected sample separately, including with resolution scaling.
constexpr char kDepthCopy[] = R"(
cbuffer Resolve : register(b0) { uint edram; uint coordinates; uint dest_info;
  uint dest_coordinates; uint dest_base; uint scaled; };
ByteAddressBuffer source : register(t0);
RWBuffer<uint> destination : register(u0);
uint TiledAddress(uint x, uint y, uint pitch) {
  uint macro = ((x >> 5) + (y >> 5) * (pitch >> 5)) << 9;
  uint micro = ((x & 7) + ((y & 14) << 2)) << 2;
  uint offset = macro + ((micro & ~15u) << 1) + (micro & 15) + ((y & 1) << 4);
  return ((offset & ~511u) << 3) + ((y & 16) << 7) + ((offset & 448) << 2)
    + (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 63);
}
uint TiledAddress3D(uint x, uint y, uint z, uint pitch, uint height) {
  uint outer = ((y >> 4) + (z >> 2) * (height >> 4)) * (pitch >> 5);
  uint macro = ((((x >> 5) + outer) << 8) & 0x0FFFFFFF) << 1;
  uint micro = ((x & 7) + ((y & 6) << 2)) << 2;
  uint odd = ((y >> 3) + (z >> 2)) & 1;
  uint part1 = odd + ((((x >> 3) + (odd << 1)) & 3) << 1);
  uint part2 = ((macro + (micro & ~15u)) << 1) + (micro & 15)
      + ((z & 3) << 8) + ((y & 1) << 4);
  uint address = (((part1 & 1) << 3) + ((part2 >> 6) & 7)) << 3;
  address = (address + (part1 & ~1u)) << 2;
  return ((address + (part2 & ~511u)) << 3) + (part2 & 63);
}
[numthreads(8,8,1)] void main(uint3 id : SV_DispatchThreadID) {
  uint sx_scale = (coordinates >> 16) & 7, sy_scale = (coordinates >> 19) & 7;
  uint width = ((coordinates >> 5) & 2047) * 8 * sx_scale;
  uint tile_width = 80 * sx_scale, tile_height = 16 * sy_scale;
  uint pitch = edram & 1023, base = (edram >> 13) & 2047;
  uint sample = (dest_coordinates >> 28) & 7;
  if (sample > 3) sample = sample == 5 ? 2 : 0;
  for (uint j = 0; j < 8; ++j) {
    uint x = id.x * 8 + j, y = id.y;
    if (x >= width) continue;
    uint source_x = x, source_y = y;
    if ((edram >> 29) & 1) {
      source_x = max(source_x, sx_scale >> 1);
      source_y = max(source_y, sy_scale >> 1);
    }
    source_x += (coordinates & 15) * 8 * sx_scale;
    source_y += ((coordinates >> 4) & 1) * 8 * sy_scale;
    source_x = source_x * 2 + (sample >> 1);
    source_y = source_y * 2 + (sample & 1);
    uint tile = (base + (source_y / tile_height) * pitch + source_x / tile_width) & 2047;
    uint column = source_x % tile_width;
    column = (column + tile_width / 2) % tile_width;
    uint value = source.Load((tile * tile_width * tile_height
        + (source_y % tile_height) * tile_width + column) * 4);
    uint endian = dest_info & 7;
    if (endian == 1 || endian == 2)
      value = ((value & 0x00FF00FF) << 8) | ((value & 0xFF00FF00) >> 8);
    if (endian == 2 || endian == 3) value = (value << 16) | (value >> 16);
    uint host_x = x + ((dest_coordinates >> 20) & 15) * 8 * sx_scale;
    uint guest_x = (host_x / 4) / sx_scale * 4;
    uint guest_y = y / sy_scale + ((dest_coordinates >> 24) & 15) * 8;
    uint pitch_dest = (dest_coordinates & 1023) * 32;
    uint address = (dest_info & 8)
        ? TiledAddress3D(guest_x, guest_y, (dest_info >> 4) & 7, pitch_dest,
                         ((dest_coordinates >> 10) & 1023) * 32)
        : TiledAddress(guest_x, guest_y, pitch_dest);
    address = address * sx_scale * sy_scale
        + (((host_x / 4) % sx_scale) * sy_scale + y % sy_scale) * 16 + (host_x % 4) * 4;
    if (!scaled) address += dest_base;
    destination[address >> 2] = value;
  }
})";

}  // namespace

bool ResolveCopy::PrepareDepthCopy(ID3D11Buffer* destination, uint32_t first_word,
                                   uint32_t word_count, std::string& error) {
  if (!depth_copy_attempted_) {
    depth_copy_attempted_ = true;
    Microsoft::WRL::ComPtr<ID3DBlob> code;
    HRESULT result = ui::d3d11::D3D11Device::CompileShader(
        kDepthCopy, "main", ui::d3d11::D3D11Device::ShaderStage::kCompute, code.GetAddressOf(),
        depth_copy_error_);
    if (SUCCEEDED(result)) {
      result = device_.device()->CreateComputeShader(
          code->GetBufferPointer(), code->GetBufferSize(), nullptr, depth_copy_.GetAddressOf());
      if (FAILED(result))
        depth_copy_error_ = "Unable to create the native depth resolve shader";
    }
  }
  if (!depth_copy_) {
    error = depth_copy_error_;
    return false;
  }
  return PrepareWordDestination(destination, first_word, word_count, error);
}

bool ResolveCopy::PrepareWordDestination(ID3D11Buffer* destination, uint32_t first_word,
                                         uint32_t word_count, std::string& error) {
  if (depth_destination_buffer_.Get() != destination ||
      depth_destination_first_word_ != first_word || depth_destination_word_count_ != word_count) {
    D3D11_UNORDERED_ACCESS_VIEW_DESC view = {};
    view.Format = DXGI_FORMAT_R32_UINT;
    view.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    view.Buffer.FirstElement = first_word;
    view.Buffer.NumElements = word_count;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> replacement;
    if (FAILED(device_.device()->CreateUnorderedAccessView(destination, &view,
                                                           replacement.GetAddressOf()))) {
      error = "Unable to create the native depth resolve destination view";
      return false;
    }
    depth_destination_ = std::move(replacement);
    depth_destination_buffer_ = destination;
    depth_destination_first_word_ = first_word;
    depth_destination_word_count_ = word_count;
  }
  return true;
}

bool ResolveCopy::CopyColorSurface(const RenderTargetSurface& source,
                                   const draw_util::ResolveInfo& resolve,
                                   ID3D11UnorderedAccessView* destination, bool scaled,
                                   std::string& error, uint32_t destination_address) {
  return CopySurface(source, resolve, destination, scaled, error, destination_address, false,
                     false);
}

bool ResolveCopy::CopyDepthSurface(const RenderTargetSurface& source,
                                   const draw_util::ResolveInfo& resolve,
                                   ID3D11UnorderedAccessView* destination, bool scaled,
                                   std::string& error, uint32_t destination_address,
                                   bool round_float24) {
  return CopySurface(source, resolve, destination, scaled, error, destination_address, true,
                     round_float24);
}

bool ResolveCopy::CopySurface(const RenderTargetSurface& source,
                              const draw_util::ResolveInfo& resolve,
                              ID3D11UnorderedAccessView* destination, bool scaled,
                              std::string& error, uint32_t destination_address, bool depth,
                              bool round_float24) {
  error.clear();
  const auto& description = source.description();
  const auto depth_format = xenos::DepthRenderTargetFormat(description.guest_format);
  auto format = xenos::ColorRenderTargetFormat(description.guest_format);
  auto packing = draw_util::GetNativeColorResolvePacking(format);
  if (format == xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA && !description.gamma_as_unorm16)
    packing = draw_util::NativeColorResolvePacking::k8888;
  auto destination_packing = draw_util::GetNativeColorResolvePacking(
      xenos::ColorFormat(resolve.copy_dest_info.copy_dest_format));
  bool integer = !depth && draw_util::NativeColorResolveUsesIntegerSource(packing);
  bool multisampled = description.samples != xenos::MsaaSamples::k1X;
  uint32_t variant = uint32_t(integer) | (uint32_t(multisampled) << 1);
  auto& program = depth ? surface_depth_copy_[multisampled] : color_copy_[variant];
  auto& attempted =
      depth ? surface_depth_copy_attempted_[multisampled] : color_copy_attempted_[variant];
  auto& program_error =
      depth ? surface_depth_copy_error_[multisampled] : color_copy_error_[variant];
  uint32_t width = uint32_t(resolve.coordinate_info.width_div_8) << 3;
  uint32_t height = resolve.height_div_8 << 3;
  uint32_t scale_x = resolve.coordinate_info.draw_resolution_scale_x;
  uint32_t scale_y = resolve.coordinate_info.draw_resolution_scale_y;
  uint32_t pitch = uint32_t(resolve.copy_dest_coordinate_info.pitch_aligned_div_32) << 5;
  uint64_t x1 = uint64_t(resolve.rect_x0) + width;
  uint64_t y1 = uint64_t(resolve.rect_y0) + height;
  const auto& edram = depth ? resolve.depth_edram_info : resolve.color_edram_info;
  auto source_layout = draw_util::GetNativeResolveSourceLayout(
      resolve.rect_x0, resolve.rect_y0, width, height, scale_x, scale_y, description.width,
      description.height, description.samples, description.samples,
      !depth && xenos::IsColorRenderTargetFormat64bpp(format), edram.pitch_tiles);
  if (!destination || !source.read_view() || description.depth != depth ||
      (depth && !source.stencil_view()) ||
      !draw_util::NativeResolveSampleSelectValid(
          description.samples, resolve.copy_dest_coordinate_info.copy_sample_select, depth) ||
      (depth ? (uint32_t(depth_format) > uint32_t(xenos::DepthRenderTargetFormat::kD24FS8))
             : packing == draw_util::NativeColorResolvePacking::kUnsupported) ||
      resolve.IsCopyingDepth() != depth || (depth && resolve.copy_dest_info.copy_dest_swap) ||
      (depth && resolve.copy_dest_info.copy_dest_exp_bias) ||
      uint32_t(resolve.copy_dest_info.copy_dest_endian) > uint32_t(xenos::Endian128::k8in128) ||
      ((resolve.rect_x0 | resolve.rect_y0) & 7) ||
      (depth ? uint32_t(resolve.copy_dest_info.copy_dest_format) !=
                   uint32_t(depth_format == xenos::DepthRenderTargetFormat::kD24FS8
                                ? xenos::TextureFormat::k_24_8_FLOAT
                                : xenos::TextureFormat::k_24_8)
             : destination_packing == draw_util::NativeColorResolveDestination::kUnsupported) ||
      !width || !height || !scale_x || !scale_y || scale_x > 3 || scale_y > 3 || x1 > pitch ||
      (resolve.copy_dest_info.copy_dest_array &&
       y1 > (uint32_t(resolve.copy_dest_coordinate_info.height_aligned_div_32) << 5)) ||
      !source_layout) {
    error = "Invalid native attachment resolve source or layout";
    return false;
  }
  Microsoft::WRL::ComPtr<ID3D11Resource> resource;
  destination->GetResource(resource.GetAddressOf());
  Microsoft::WRL::ComPtr<ID3D11Buffer> buffer;
  D3D11_UNORDERED_ACCESS_VIEW_DESC view = {};
  destination->GetDesc(&view);
  uint32_t bytes_per_element = view.Format == DXGI_FORMAT_R32_UINT            ? 4
                               : view.Format == DXGI_FORMAT_R32G32_UINT       ? 8
                               : view.Format == DXGI_FORMAT_R32G32B32A32_UINT ? 16
                                                                              : 0;
  if (!bytes_per_element || view.ViewDimension != D3D11_UAV_DIMENSION_BUFFER || view.Buffer.Flags ||
      FAILED(resource.As(&buffer))) {
    error = "Native color resolve requires a typed buffer destination";
    return false;
  }
  uint32_t base = scaled ? (resolve.copy_dest_base_raw - resolve.copy_dest_base) * scale_x * scale_y
                         : resolve.copy_dest_base_raw - destination_address;
  uint64_t end = uint64_t(resolve.copy_dest_extent_start) + resolve.copy_dest_extent_length;
  uint64_t required =
      scaled ? (end - resolve.copy_dest_base) * scale_x * scale_y : end - destination_address;
  if (end < resolve.copy_dest_base ||
      (!scaled && destination_address > resolve.copy_dest_extent_start) ||
      (scaled && destination_address) ||
      required > uint64_t(view.Buffer.NumElements) * bytes_per_element ||
      (!scaled && view.Buffer.FirstElement)) {
    error = "Native color resolve exceeds its destination buffer view";
    return false;
  }
  if (!attempted) {
    attempted = true;
    Microsoft::WRL::ComPtr<ID3DBlob> code;
    std::string shader = depth ? draw_util::kNativeDepthResolve : draw_util::kNativeColorResolve;
    if (integer)
      shader.insert(0, "#define NATIVE_COLOR_UINT\n");
    if (multisampled)
      shader.insert(0, "#define NATIVE_RESOLVE_MSAA\n");
    HRESULT result = ui::d3d11::D3D11Device::CompileShader(
        shader, "main", ui::d3d11::D3D11Device::ShaderStage::kCompute, code.GetAddressOf(),
        program_error);
    if (SUCCEEDED(result))
      result = device_.device()->CreateComputeShader(
          code->GetBufferPointer(), code->GetBufferSize(), nullptr, program.GetAddressOf());
    if (FAILED(result) && program_error.empty())
      program_error = "Unable to create the native attachment resolve shader";
  }
  if (!program ||
      !PrepareWordDestination(buffer.Get(), view.Buffer.FirstElement * (bytes_per_element / 4),
                              view.Buffer.NumElements * (bytes_per_element / 4), error)) {
    if (!program)
      error = program_error;
    return false;
  }
  uint32_t sample_map =
      description.samples == xenos::MsaaSamples::k4X
          ? 0xD8u
          : draw_util::GetD3D10SampleIndexForGuest2xMSAA(0, description.msaa_2x_supported) |
                (draw_util::GetD3D10SampleIndexForGuest2xMSAA(1, description.msaa_2x_supported)
                 << 2);
  std::array<uint32_t, 20> values = {
      width,
      height,
      pitch,
      base,
      scale_x,
      scale_y,
      depth ? uint32_t(depth_format == xenos::DepthRenderTargetFormat::kD24FS8) : uint32_t(packing),
      uint32_t(resolve.copy_dest_info.copy_dest_endian),
      depth ? uint32_t(round_float24) : uint32_t(resolve.copy_dest_info.copy_dest_swap),
      uint32_t(!scaled),
      resolve.rect_x0,
      resolve.rect_y0,
      uint32_t(resolve.copy_dest_coordinate_info.height_aligned_div_32) << 5,
      uint32_t(resolve.copy_dest_info.copy_dest_slice),
      uint32_t(resolve.copy_dest_info.copy_dest_array),
      uint32_t(127 + int32_t(resolve.copy_dest_info.copy_dest_exp_bias)) << 23,
      uint32_t(resolve.copy_dest_coordinate_info.copy_sample_select),
      sample_map,
      depth ? 0u : uint32_t(destination_packing),
      source_layout.packed};
  auto constants =
      buffers_.GetOrCreate({reinterpret_cast<const uint8_t*>(values.data()), sizeof(values)},
                           BufferKind::kConstant, error);
  if (!constants)
    return false;
  auto* cb = constants->buffer();
  ID3D11ShaderResourceView* inputs[] = {source.read_view(),
                                        depth ? source.stencil_view() : nullptr};
  auto* output = depth_destination_.Get();
  draws_.Invalidate();
  device_.context()->CSSetShader(program.Get(), nullptr, 0);
  device_.context()->CSSetConstantBuffers(0, 1, &cb);
  device_.context()->CSSetShaderResources(0, 2, inputs);
  device_.context()->CSSetUnorderedAccessViews(0, 1, &output, nullptr);
  device_.context()->Dispatch((width * (scaled ? scale_x : 1) + 7) / 8,
                              (height * (scaled ? scale_y : 1) + 7) / 8, 1);
  draws_.Invalidate();
  if (FAILED(device_.device()->GetDeviceRemovedReason())) {
    error = "Direct3D device was lost during a native attachment resolve";
    return false;
  }
  return true;
}

const ShaderProgram* ResolveCopy::Program(draw_util::ResolveCopyShaderIndex shader, bool scaled,
                                          std::string& error) {
  size_t index = size_t(shader);
  if (!attempted_[scaled][index]) {
    attempted_[scaled][index] = true;
    programs_[scaled][index] =
        shaders_.GetOrCreate(kPrograms[scaled][index].code, false, errors_[scaled][index]);
  }
  error = errors_[scaled][index];
  return programs_[scaled][index];
}

bool ResolveCopy::Copy(const RenderTargetEncoder& source, draw_util::ResolveCopyShaderIndex shader,
                       const draw_util::ResolveCopyShaderConstants& constants,
                       ID3D11UnorderedAccessView* destination, uint32_t groups_x, uint32_t groups_y,
                       bool scaled, std::string& error) {
  error.clear();
  const auto& coordinate = constants.dest_relative.coordinate_info;
  const auto& target = constants.dest_relative.dest_coordinate_info;
  size_t index = size_t(shader);
  uint32_t scale_x = coordinate.draw_resolution_scale_x;
  uint32_t scale_y = coordinate.draw_resolution_scale_y;
  if (index >= kPrograms[0].size() || !destination || !source.buffer() || !scale_x || !scale_y ||
      scale_x > 3 || scale_y > 3 || scaled != (scale_x != 1 || scale_y != 1) ||
      source.byte_size() != xenos::kEdramSizeBytes * scale_x * scale_y ||
      !constants.dest_relative.edram_info.pitch_tiles ||
      uint32_t(constants.dest_relative.dest_info.copy_dest_endian) >
          uint32_t(xenos::Endian128::k8in128) ||
      uint32_t(constants.dest_relative.edram_info.msaa_samples) >
          uint32_t(xenos::MsaaSamples::k4X) ||
      !coordinate.width_div_8 || !groups_x || !groups_y ||
      groups_x > D3D11_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION ||
      groups_y > D3D11_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION ||
      !target.pitch_aligned_div_32 || !target.height_aligned_div_32) {
    error = "Invalid native resolve copy shader, dimensions or resolution scale";
    return false;
  }
  const auto& info = kPrograms[scaled][index];
  uint32_t width = (coordinate.width_div_8 << 3) * scale_x;
  if (groups_x != (width + (1u << info.group_x_log2) - 1) >> info.group_x_log2 ||
      (uint32_t(target.offset_x_div_8) << 3) + (coordinate.width_div_8 << 3) >
          (uint32_t(target.pitch_aligned_div_32) << 5) ||
      (uint32_t(target.offset_y_div_8) << 3) * scale_y + groups_y * 8 >
          (uint32_t(target.height_aligned_div_32) << 5) * scale_y) {
    error = "Native resolve copy extends beyond the destination layout";
    return false;
  }
  Microsoft::WRL::ComPtr<ID3D11Resource> resource;
  destination->GetResource(resource.GetAddressOf());
  Microsoft::WRL::ComPtr<ID3D11Buffer> destination_buffer;
  D3D11_UNORDERED_ACCESS_VIEW_DESC view = {};
  destination->GetDesc(&view);
  DXGI_FORMAT format =
      info.destination_bpe_log2 == 3 ? DXGI_FORMAT_R32G32_UINT : DXGI_FORMAT_R32G32B32A32_UINT;
  if (FAILED(resource.As(&destination_buffer)) || resource.Get() == source.buffer() ||
      view.ViewDimension != D3D11_UAV_DIMENSION_BUFFER || view.Format != format ||
      (!scaled && view.Buffer.FirstElement) || view.Buffer.Flags) {
    error = "Native resolve copy requires a separate typed destination buffer range";
    return false;
  }
  D3D11_BUFFER_DESC buffer = {};
  destination_buffer->GetDesc(&buffer);
  if (!view.Buffer.NumElements ||
      (uint64_t(view.Buffer.FirstElement) + view.Buffer.NumElements) << info.destination_bpe_log2 >
          buffer.ByteWidth ||
      (!scaled &&
       (uint64_t(view.Buffer.NumElements) << info.destination_bpe_log2) != buffer.ByteWidth) ||
      (!scaled && (constants.dest_base >= buffer.ByteWidth || (constants.dest_base & 15)))) {
    error = "Native resolve copy destination range is invalid";
    return false;
  }
  bool depth_copy = shader == draw_util::ResolveCopyShaderIndex::kFast32bpp4xMSAA &&
                    constants.dest_relative.edram_info.is_depth;
  ID3D11ShaderResourceView* input =
      info.raw_source || depth_copy ? source.raw_srv() : source.typed_srv(info.source_bpe_log2);
  const ShaderProgram* program = depth_copy ? nullptr : Program(shader, scaled, error);
  if (!input || (!depth_copy && (!program || !program->compute())))
    return false;
  if (depth_copy &&
      !PrepareDepthCopy(destination_buffer.Get(),
                        view.Buffer.FirstElement << (info.destination_bpe_log2 - 2),
                        view.Buffer.NumElements << (info.destination_bpe_log2 - 2), error))
    return false;
  std::array<uint32_t, 8> words = {};
  std::memcpy(words.data(), &constants,
              scaled && !depth_copy ? sizeof(constants.dest_relative) : sizeof(constants));
  if (depth_copy)
    words[5] = scaled;
  auto values =
      buffers_.GetOrCreate({reinterpret_cast<const uint8_t*>(words.data()), sizeof(words)},
                           BufferKind::kConstant, error);
  if (!values)
    return false;
  draws_.Invalidate();
  auto* context = device_.context();
  auto* constant_buffer = values->buffer();
  context->CSSetShader(depth_copy ? depth_copy_.Get() : program->compute(), nullptr, 0);
  context->CSSetConstantBuffers(0, 1, &constant_buffer);
  context->CSSetShaderResources(0, 1, &input);
  ID3D11UnorderedAccessView* output = depth_copy ? depth_destination_.Get() : destination;
  context->CSSetUnorderedAccessViews(0, 1, &output, nullptr);
  context->Dispatch(groups_x, groups_y, 1);
  draws_.Invalidate();
  if (FAILED(device_.device()->GetDeviceRemovedReason())) {
    error = "Direct3D device was lost during a native resolve copy";
    return false;
  }
  return true;
}

void ResolveCopy::Clear() {
  draws_.Invalidate();
  programs_ = {};
  attempted_ = {};
  errors_ = {};
  shaders_.Clear();
  buffers_.Clear();
  depth_copy_.Reset();
  for (auto& program : color_copy_)
    program.Reset();
  for (auto& program : surface_depth_copy_)
    program.Reset();
  surface_depth_copy_attempted_ = {};
  for (auto& error : surface_depth_copy_error_)
    error.clear();
  depth_destination_buffer_.Reset();
  depth_destination_.Reset();
  depth_copy_attempted_ = false;
  depth_copy_error_.clear();
  color_copy_attempted_ = {};
  color_copy_error_ = {};
}

}  // namespace rex::graphics::d3d11
