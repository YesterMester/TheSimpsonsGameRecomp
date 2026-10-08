#include <rex/graphics/d3d11/resolve_copy.h>

#include <algorithm>
#include <cstring>

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
      uint32_t(constants.dest_relative.dest_info.copy_dest_endian) > 3 ||
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
  depth_destination_buffer_.Reset();
  depth_destination_.Reset();
  depth_copy_attempted_ = false;
  depth_copy_error_.clear();
}

}  // namespace rex::graphics::d3d11
