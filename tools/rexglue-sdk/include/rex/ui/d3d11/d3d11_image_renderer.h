#pragma once

#include <span>
#include <rex/graphics/d3d11/shader_cache.h>
#include <rex/ui/d3d11/d3d11_device.h>

namespace rex::ui::d3d11 {

// Native frame composition. Callers own context serialization and invalidate
// their draw bindings after this changes the immediate context.
class D3D11ImageRenderer {
 public:
  struct Rectangle {
    int32_t x, y;
    uint32_t width, height;
  };
  explicit D3D11ImageRenderer(D3D11Device& device) : device_(device), shaders_(device) {}
  bool Initialize(std::string& error);
  bool Draw(ID3D11ShaderResourceView* source, ID3D11RenderTargetView* target, uint32_t width,
            uint32_t height, Rectangle rectangle, uint32_t swizzle, uint32_t gamma_mode,
            ID3D11ShaderResourceView* gamma, std::string& error, bool fxaa_luma = false);
  bool ApplyFxaa(ID3D11ShaderResourceView* source, ID3D11UnorderedAccessView* target,
                 uint32_t width, uint32_t height, bool extreme, std::string& error);
  bool DrawEffect(ID3D11ShaderResourceView* source, ID3D11RenderTargetView* target, uint32_t width,
                  uint32_t height, Rectangle rectangle, std::span<const uint8_t> pixel_shader,
                  std::span<const uint8_t> parameters, std::string& error);
  bool DrawBatch(ID3D11ShaderResourceView* vertices, ID3D11Buffer* indices,
                 ID3D11ShaderResourceView* texture, ID3D11SamplerState* sampler,
                 ID3D11RenderTargetView* target, uint32_t width, uint32_t height,
                 float coordinate_width, float coordinate_height, D3D11_RECT scissor,
                 D3D11_PRIMITIVE_TOPOLOGY topology, uint32_t count, uint32_t first,
                 int32_t base_vertex, std::string& error);

 private:
  bool Bind(ID3D11RenderTargetView* target, uint32_t width, uint32_t height,
            const D3D11_RECT& scissor, bool blend, std::string& error);
  bool Constants(std::span<const uint8_t> bytes, std::string& error);
  D3D11Device& device_;
  graphics::d3d11::ShaderCache shaders_;
  Microsoft::WRL::ComPtr<ID3D11VertexShader> fullscreen_, immediate_;
  Microsoft::WRL::ComPtr<ID3D11PixelShader> image_, ui_;
  Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterizer_;
  Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depth_;
  Microsoft::WRL::ComPtr<ID3D11BlendState> blend_, opaque_;
  Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;
  Microsoft::WRL::ComPtr<ID3D11Buffer> constants_;
};

}  // namespace rex::ui::d3d11
