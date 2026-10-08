#include <rex/graphics/d3d11/render_target_encoder.h>

#include <cstdio>

namespace rex::graphics::d3d11 {
namespace {
bool Failed(HRESULT result, const char* operation, std::string& error) {
  if (SUCCEEDED(result))
    return false;
  char message[160];
  std::snprintf(message, sizeof(message), "%s failed (0x%08X)", operation, unsigned(result));
  error = message;
  return true;
}
}  // namespace

bool RenderTargetEncoder::Initialize(const DxbcRenderTargetDumpShader::Options& options,
                                     std::string& error) {
  error.clear();
  if (buffer_ || !options.resolution_scale_x || !options.resolution_scale_y ||
      options.resolution_scale_x > 3 || options.resolution_scale_y > 3) {
    error = "Invalid native resolve encoder initialization or resolution scale";
    return false;
  }
  options_ = options;
  options_.portable_integer_division = true;
  byte_size_ = xenos::kEdramTileCount * xenos::kEdramTileWidthSamples *
               xenos::kEdramTileHeightSamples * 4 * options.resolution_scale_x *
               options.resolution_scale_y;
  D3D11_BUFFER_DESC description = {};
  description.ByteWidth = byte_size_;
  description.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
  description.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
  HRESULT result = device_.device()->CreateBuffer(&description, nullptr, buffer_.GetAddressOf());
  constexpr std::array<DXGI_FORMAT, 4> formats = {DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_R32_UINT,
                                                  DXGI_FORMAT_R32G32_UINT,
                                                  DXGI_FORMAT_R32G32B32A32_UINT};
  for (uint32_t i = 0; i < formats.size() && SUCCEEDED(result); ++i) {
    uint32_t bytes = i ? 1u << (i + 1) : 4;
    D3D11_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.Format = formats[i];
    srv.ViewDimension = i ? D3D11_SRV_DIMENSION_BUFFER : D3D11_SRV_DIMENSION_BUFFEREX;
    srv.BufferEx.NumElements = byte_size_ / bytes;
    srv.BufferEx.Flags = i ? 0 : D3D11_BUFFEREX_SRV_FLAG_RAW;
    result =
        device_.device()->CreateShaderResourceView(buffer_.Get(), &srv, srvs_[i].GetAddressOf());
    if (FAILED(result))
      break;
    D3D11_UNORDERED_ACCESS_VIEW_DESC uav = {};
    uav.Format = formats[i];
    uav.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    uav.Buffer.NumElements = byte_size_ / bytes;
    uav.Buffer.Flags = i ? 0 : D3D11_BUFFER_UAV_FLAG_RAW;
    result =
        device_.device()->CreateUnorderedAccessView(buffer_.Get(), &uav, uavs_[i].GetAddressOf());
  }
  if (Failed(result, "Native render target encoding buffer", error)) {
    srvs_ = {};
    uavs_ = {};
    buffer_.Reset();
    byte_size_ = 0;
    return false;
  }
  draws_.Invalidate();
  constexpr std::array<uint32_t, 4> zero = {};
  device_.context()->ClearUnorderedAccessViewUint(uavs_[0].Get(), zero.data());
  return true;
}

ID3D11ShaderResourceView* RenderTargetEncoder::typed_srv(uint32_t element_size_log2) const {
  return element_size_log2 >= 2 && element_size_log2 <= 4 ? srvs_[element_size_log2 - 1].Get()
                                                          : nullptr;
}

bool RenderTargetEncoder::Encode(const RenderTargetSurface& source, uint32_t first_tile,
                                 uint32_t source_base, uint32_t destination_pitch,
                                 uint32_t source_pitch, uint32_t width_tiles, uint32_t height_tiles,
                                 std::string& error) {
  error.clear();
  if (!buffer_ || first_tile >= 2 * xenos::kEdramTileCount ||
      source_base >= xenos::kEdramTileCount || !destination_pitch ||
      destination_pitch >= (1u << xenos::kEdramPitchTilesBits) || !source_pitch ||
      source_pitch >= (1u << xenos::kEdramPitchTilesBits) || !width_tiles ||
      width_tiles > destination_pitch || !height_tiles || height_tiles > xenos::kEdramTileCount ||
      (source.description().samples == xenos::MsaaSamples::k2X &&
       source.description().msaa_2x_supported != options_.msaa_2x_supported)) {
    error = "Invalid native render target encoding range or sample convention";
    return false;
  }
  DxbcRenderTargetDumpShader::Key key;
  key.resource_format = source.description().guest_format;
  key.is_depth = source.description().depth;
  key.msaa_samples = source.description().samples;
  auto existing = programs_.find(key.key);
  const ShaderProgram* program;
  if (existing == programs_.end()) {
    std::vector<uint32_t> words;
    program = nullptr;
    if (DxbcRenderTargetDumpShader::Create(key, options_, words, error))
      program = shaders_.GetOrCreate(
          {reinterpret_cast<const uint8_t*>(words.data()), words.size() * 4}, false, error);
    programs_.emplace(key.key, program);
    if (!program)
      errors_.emplace(key.key, error);
  } else {
    program = existing->second;
    if (!program)
      error = errors_.at(key.key);
  }
  if (!program)
    return false;
  DxbcRenderTargetDumpShader::Offsets offsets;
  offsets.dispatch_first_tile = first_tile;
  offsets.source_base_tiles = source_base;
  DxbcRenderTargetDumpShader::Pitches pitches;
  pitches.dest_pitch = destination_pitch;
  pitches.source_pitch = source_pitch;
  std::array<uint32_t, 4> offset_values = {offsets.offsets, 0, 0, 0};
  std::array<uint32_t, 4> pitch_values = {pitches.pitches, 0, 0, 0};
  auto offset_buffer = buffers_.GetOrCreate(
      {reinterpret_cast<const uint8_t*>(offset_values.data()), sizeof(offset_values)},
      BufferKind::kConstant, error);
  auto pitch_buffer = buffers_.GetOrCreate(
      {reinterpret_cast<const uint8_t*>(pitch_values.data()), sizeof(pitch_values)},
      BufferKind::kConstant, error);
  if (!offset_buffer || !pitch_buffer)
    return false;
  std::array<ID3D11Buffer*, 2> constants = {offset_buffer->buffer(), pitch_buffer->buffer()};
  std::array<ID3D11ShaderResourceView*, 2> reads = {source.read_view(), source.stencil_view()};
  bool wide = !key.is_depth && xenos::IsColorRenderTargetFormat64bpp(key.GetColorFormat());
  ID3D11UnorderedAccessView* destination = uavs_[wide ? 2 : 1].Get();
  draws_.Invalidate();
  device_.context()->CSSetShader(program->compute(), nullptr, 0);
  device_.context()->CSSetConstantBuffers(0, uint32_t(constants.size()), constants.data());
  device_.context()->CSSetShaderResources(0, key.is_depth ? 2 : 1, reads.data());
  device_.context()->CSSetUnorderedAccessViews(0, 1, &destination, nullptr);
  device_.context()->Dispatch((width_tiles * options_.resolution_scale_x) << uint32_t(!wide),
                              height_tiles * options_.resolution_scale_y, 1);
  // Release the output before a resolve shader reads its encoded data. The
  // context orders these native dispatches without a CPU wait or readback.
  draws_.Invalidate();
  return !Failed(device_.device()->GetDeviceRemovedReason(), "Native render target encoding",
                 error);
}

void RenderTargetEncoder::Clear() {
  draws_.Invalidate();
  programs_.clear();
  errors_.clear();
  shaders_.Clear();
  buffers_.Clear();
  downsample_.Reset();
  downsample_attempted_ = false;
  downsample_error_.clear();
  if (buffer_) {
    constexpr std::array<uint32_t, 4> zero = {};
    device_.context()->ClearUnorderedAccessViewUint(uavs_[0].Get(), zero.data());
  }
}

bool RenderTargetEncoder::DownsampleFrom(const RenderTargetEncoder& source, uint32_t first_tile,
                                         uint32_t width_tiles, uint32_t height_tiles,
                                         uint32_t pitch_tiles, bool wide,
                                         xenos::MsaaSamples samples, std::string& error) {
  error.clear();
  uint64_t words = uint64_t(width_tiles) * height_tiles * 1280;
  if (!buffer_ || !source.buffer_ || buffer_.Get() == source.buffer_.Get() ||
      options_.resolution_scale_x != 1 || options_.resolution_scale_y != 1 || !width_tiles ||
      !height_tiles || width_tiles > pitch_tiles ||
      pitch_tiles >= (1u << xenos::kEdramPitchTilesBits) ||
      first_tile >= 2 * xenos::kEdramTileCount || uint32_t(samples) > 2 ||
      (words + 63) / 64 > D3D11_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION) {
    error = "Invalid native render target downsample range";
    return false;
  }
  if (!downsample_attempted_) {
    downsample_attempted_ = true;
    constexpr std::string_view code = R"(
ByteAddressBuffer source : register(t0);
RWByteAddressBuffer destination : register(u0);
cbuffer parameters : register(b0) {
  uint first_tile; uint width_tiles; uint height_tiles; uint pitch_tiles;
  uint scale_x; uint scale_y; uint wide; uint samples;
};
[numthreads(64,1,1)] void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= width_tiles * height_tiles * 1280) return;
  uint relative_tile = id.x / 1280;
  uint tile = (first_tile + relative_tile % width_tiles +
              (relative_tile / width_tiles) * pitch_tiles) & 2047;
  uint word = id.x % 1280;
  uint words_per_pixel = wide ? 2 : 1;
  uint tile_width = wide ? 40 : 80;
  uint pixel = word / words_per_pixel;
  uint x = pixel % tile_width, y = pixel / tile_width;
  uint hss = samples == 2, vss = samples != 0;
  // Scale the pixel coordinate while retaining the guest sample within it.
  // The center sample compensates for the original half-pixel convention.
  uint sx = (((x >> hss) * scale_x + (scale_x >> 1)) << hss) | (x & hss);
  uint sy = (((y >> vss) * scale_y + (scale_y >> 1)) << vss) | (y & vss);
  uint source_word = tile * 1280 * scale_x * scale_y +
                     (sy * tile_width * scale_x + sx) * words_per_pixel +
                     word % words_per_pixel;
  destination.Store((tile * 1280 + word) * 4, source.Load(source_word * 4));
})";
    Microsoft::WRL::ComPtr<ID3DBlob> compiled;
    HRESULT result = ui::d3d11::D3D11Device::CompileShader(
        code, "main", ui::d3d11::D3D11Device::ShaderStage::kCompute, compiled.GetAddressOf(),
        downsample_error_);
    if (SUCCEEDED(result))
      result = device_.device()->CreateComputeShader(compiled->GetBufferPointer(),
                                                     compiled->GetBufferSize(), nullptr,
                                                     downsample_.GetAddressOf());
    if (FAILED(result) && downsample_error_.empty())
      downsample_error_ = "Native render target downsample shader creation failed";
  }
  if (!downsample_) {
    error = downsample_error_;
    return false;
  }
  std::array<uint32_t, 8> values = {first_tile,
                                    width_tiles,
                                    height_tiles,
                                    pitch_tiles,
                                    source.options_.resolution_scale_x,
                                    source.options_.resolution_scale_y,
                                    uint32_t(wide),
                                    uint32_t(samples)};
  auto constants =
      buffers_.GetOrCreate({reinterpret_cast<const uint8_t*>(values.data()), sizeof(values)},
                           BufferKind::kConstant, error);
  if (!constants)
    return false;
  auto* cb = constants->buffer();
  auto* input = source.raw_srv();
  auto* output = uavs_[0].Get();
  draws_.Invalidate();
  device_.context()->CSSetShader(downsample_.Get(), nullptr, 0);
  device_.context()->CSSetConstantBuffers(0, 1, &cb);
  device_.context()->CSSetShaderResources(0, 1, &input);
  device_.context()->CSSetUnorderedAccessViews(0, 1, &output, nullptr);
  device_.context()->Dispatch(uint32_t((words + 63) / 64), 1, 1);
  draws_.Invalidate();
  return !Failed(device_.device()->GetDeviceRemovedReason(), "Native render target downsample",
                 error);
}

}  // namespace rex::graphics::d3d11
