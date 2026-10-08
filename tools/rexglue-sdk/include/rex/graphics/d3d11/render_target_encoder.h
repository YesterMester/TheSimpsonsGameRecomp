#pragma once

#include <array>
#include <unordered_map>

#include <rex/graphics/d3d11/render_target_surface.h>
#include <rex/graphics/util/dxbc_render_target_dump.h>

namespace rex::graphics::d3d11 {

// Native GPU buffer encoding for guest resolve copies and frame trace snapshots.
// Images, constants and encoded data stay on the GPU during game rendering.
class RenderTargetEncoder {
 public:
  RenderTargetEncoder(ui::d3d11::D3D11Device& device, DrawContext& draws)
      : device_(device), draws_(draws), shaders_(device), buffers_(device, 1024 * 1024) {}
  bool Initialize(const DxbcRenderTargetDumpShader::Options& options, std::string& error);
  bool Encode(const RenderTargetSurface& source, uint32_t first_tile, uint32_t source_base,
              uint32_t destination_pitch, uint32_t source_pitch, uint32_t width_tiles,
              uint32_t height_tiles, std::string& error);
  // Produce the original-resolution samples for CPU-visible resolves. This
  // preserves MSAA sample identities and runs entirely on the native GPU.
  bool DownsampleFrom(const RenderTargetEncoder& source, uint32_t first_tile, uint32_t width_tiles,
                      uint32_t height_tiles, uint32_t pitch_tiles, bool wide,
                      xenos::MsaaSamples samples, std::string& error);
  ID3D11Buffer* buffer() const { return buffer_.Get(); }
  ID3D11ShaderResourceView* raw_srv() const { return srvs_[0].Get(); }
  ID3D11ShaderResourceView* typed_srv(uint32_t element_size_log2) const;
  uint32_t byte_size() const { return byte_size_; }
  void Clear();

 private:
  ui::d3d11::D3D11Device& device_;
  DrawContext& draws_;
  ShaderCache shaders_;
  BufferCache buffers_;
  DxbcRenderTargetDumpShader::Options options_;
  Microsoft::WRL::ComPtr<ID3D11Buffer> buffer_;
  std::array<Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>, 4> srvs_;
  std::array<Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView>, 4> uavs_;
  std::unordered_map<uint32_t, const ShaderProgram*> programs_;
  std::unordered_map<uint32_t, std::string> errors_;
  Microsoft::WRL::ComPtr<ID3D11ComputeShader> downsample_;
  bool downsample_attempted_ = false;
  std::string downsample_error_;
  uint32_t byte_size_ = 0;
};

}  // namespace rex::graphics::d3d11
