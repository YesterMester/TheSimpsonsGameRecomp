#pragma once

#include <array>
#include <memory>
#include <span>

#include <rex/graphics/d3d11/draw_context.h>
#include <rex/graphics/xenos.h>

namespace rex::graphics::d3d11 {

// Native storage and exact transfer views for one guest render target.
// Tile ownership stays in the common render target cache.
class RenderTargetSurface {
 public:
  struct Description {
    uint32_t width = 0, height = 0;
    uint32_t guest_format = 0;
    xenos::MsaaSamples samples = xenos::MsaaSamples::k1X;
    bool depth = false;
    bool msaa_2x_supported = true;
    bool gamma_as_unorm16 = true;
  };
  static std::unique_ptr<RenderTargetSurface> Create(ui::d3d11::D3D11Device& device,
                                                     const Description& description,
                                                     std::string& error);
  const Description& description() const { return description_; }
  uint32_t sample_count() const { return sample_count_; }
  ID3D11Texture2D* image() const { return image_.Get(); }
  ID3D11RenderTargetView* draw_view() const { return draw_.Get(); }
  ID3D11RenderTargetView* transfer_view() const { return transfer_.Get(); }
  ID3D11DepthStencilView* depth_view() const { return depth_.Get(); }
  ID3D11ShaderResourceView* read_view() const { return read_.Get(); }
  ID3D11ShaderResourceView* stencil_view() const { return stencil_.Get(); }
  bool integer_transfer() const { return integer_transfer_; }
  // Retain a separate native image for a depth source that is also the writable
  // destination. No source pixels are downloaded or encoded in a CPU buffer.
  std::unique_ptr<RenderTargetSurface> Snapshot(ui::d3d11::D3D11Device& device, DrawContext& draws,
                                                std::string& error) const;

 private:
  Description description_;
  uint32_t sample_count_ = 1;
  bool integer_transfer_ = false;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> image_;
  Microsoft::WRL::ComPtr<ID3D11RenderTargetView> draw_, transfer_;
  Microsoft::WRL::ComPtr<ID3D11DepthStencilView> depth_;
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> read_, stencil_;
};

// Native rectangle submission for ownership transfers and resolve clears.
// Pixel programs use the shared transfer generator's b0-b2/t0-t3 contract.
class RenderTargetTransfer {
 public:
  enum class Output { kColor, kDepth, kStencilBit };
  struct Constants {
    uint32_t address = 0, host_depth_address = 0;
    uint8_t stencil_bit = 0;
    Output output = Output::kColor;
    bool write_stencil_reference = false;
  };
  RenderTargetTransfer(ui::d3d11::D3D11Device& device, DrawContext& draws)
      : draws_(draws), shaders_(device), buffers_(device, 1024 * 1024) {}
  bool Transfer(RenderTargetSurface& destination, const RenderTargetSurface& source,
                const RenderTargetSurface* host_depth, const ShaderProgram& pixel,
                const Constants& constants, std::span<const D3D11_RECT> rectangles,
                std::string& error);
  bool ClearColor(RenderTargetSurface& destination, const std::array<float, 4>& value,
                  std::span<const D3D11_RECT> rectangles, std::string& error);
  bool ClearColorInteger(RenderTargetSurface& destination, const std::array<uint32_t, 4>& value,
                         std::span<const D3D11_RECT> rectangles, std::string& error);
  bool ClearDepthStencil(RenderTargetSurface& destination, bool clear_depth, float depth,
                         bool clear_stencil, uint8_t stencil,
                         std::span<const D3D11_RECT> rectangles, std::string& error);
  void Clear();

 private:
  const ShaderProgram* Helper(const char* source, const char* profile, std::string& error);
  bool Prepare(DrawCommand& command, const RenderTargetSurface& destination, std::string& error);
  bool Rectangles(DrawCommand& command, std::span<const D3D11_RECT> rectangles, std::string& error);
  std::shared_ptr<const BufferVersion> Constant(const void* data, size_t size, std::string& error);
  DrawContext& draws_;
  ShaderCache shaders_;
  BufferCache buffers_;
  const ShaderProgram* vertex_ = nullptr;
  const ShaderProgram* color_float_ = nullptr;
  const ShaderProgram* color_integer_ = nullptr;
  const ShaderProgram* depth_ = nullptr;
  const ShaderProgram* stencil_ = nullptr;
};

}  // namespace rex::graphics::d3d11
