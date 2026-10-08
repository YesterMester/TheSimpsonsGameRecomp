#pragma once

#include <rex/graphics/d3d11/buffer_cache.h>
#include <rex/ui/d3d11/d3d11_image_renderer.h>
#include <rex/ui/immediate_drawer.h>

namespace rex::ui::d3d11 {
class D3D11ImmediateDrawer final : public ImmediateDrawer {
 public:
  static std::unique_ptr<D3D11ImmediateDrawer> Create(D3D11Device& device);
  std::unique_ptr<ImmediateTexture> CreateTexture(uint32_t width, uint32_t height,
                                                  ImmediateTextureFilter filter, bool repeated,
                                                  const uint8_t* data) override;
  void Begin(UIDrawContext& context, float width, float height) override;
  void BeginDrawBatch(const ImmediateDrawBatch& batch) override;
  void Draw(const ImmediateDraw& draw) override;
  void EndDrawBatch() override;
  void End() override;

 private:
  class Texture final : public ImmediateTexture {
   public:
    Texture(uint32_t width, uint32_t height) : ImmediateTexture(width, height) {}
    Microsoft::WRL::ComPtr<ID3D11Texture2D> resource;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler;
  };
  explicit D3D11ImmediateDrawer(D3D11Device& device)
      : device_(device), renderer_(device), buffers_(device, 8 * 1024 * 1024) {}
  D3D11Device& device_;
  D3D11ImageRenderer renderer_;
  graphics::d3d11::BufferCache buffers_;
  std::unique_ptr<ImmediateTexture> white_;
  std::unique_lock<ContextMutex> context_lock_;
  std::shared_ptr<const graphics::d3d11::BufferVersion> vertices_, indices_;
  std::vector<uint16_t> index_data_;
  uint32_t vertex_count_ = 0;
};
}  // namespace rex::ui::d3d11
