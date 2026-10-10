#pragma once

#include <array>
#include <unordered_map>

#include <rex/graphics/d3d11/render_target_encoder.h>
#include <rex/graphics/d3d11/resolve_copy.h>
#include <rex/graphics/d3d11/texture_cache.h>
#include <rex/graphics/pipeline/render_target/cache.h>
#include <rex/graphics/util/dxbc_render_target_transfer.h>

namespace rex::graphics::d3d11 {

class D3D11SharedMemory;
class D3D11TextureCache;

// Native color and depth attachments with the common guest tile ownership
// tracker. All image work runs on the command processor's immediate context.
class D3D11RenderTargetCache final : public RenderTargetCache {
 public:
  struct Options {
    bool native_2x_msaa = true;
    bool gamma_as_unorm16 = true;
    bool stencil_reference_output = false;
    bool depth_float24_convert_in_pixel_shader = true;
    bool depth_float24_round = false;
  };
  D3D11RenderTargetCache(const RegisterFile& registers, const memory::Memory& memory,
                         TraceWriter* trace, ui::d3d11::D3D11Device& device, DrawContext& draws,
                         uint32_t resolution_scale_x, uint32_t resolution_scale_y);
  ~D3D11RenderTargetCache() override;
  bool Initialize(const Options& options);
  void ClearCache() override;
  Path GetPath() const override { return Path::kHostRenderTargets; }
  bool Update(bool is_rasterization_done, reg::RB_DEPTHCONTROL normalized_depth_control,
              uint32_t normalized_color_mask, const Shader& vertex_shader) override;
  void BindRenderTargets(DrawCommand& command) const;
  bool Resolve(D3D11SharedMemory& shared_memory, D3D11TextureCache& textures,
               const memory::Memory& memory, TraceWriter& trace, uint32_t& written_address,
               uint32_t& written_length);
  bool ResolveClear(const draw_util::ResolveInfo& resolve);
  bool DumpRenderTargets(uint32_t base, uint32_t row_length, uint32_t rows, uint32_t pitch);
  RenderTargetEncoder& encoder() { return encoder_; }
  bool msaa_2x_supported() const { return msaa_2x_supported_; }
  bool gamma_as_unorm16() const { return options_.gamma_as_unorm16; }
  bool depth_float24_convert_in_pixel_shader() const {
    return options_.depth_float24_convert_in_pixel_shader;
  }
  bool depth_float24_round() const { return options_.depth_float24_round; }
  const std::string& last_error() const { return last_error_; }

 protected:
  bool IsGammaFormatHostStorageSeparate() const override { return options_.gamma_as_unorm16; }
  uint32_t GetMaxRenderTargetWidth() const override { return D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION; }
  uint32_t GetMaxRenderTargetHeight() const override {
    return D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION;
  }
  RenderTarget* CreateRenderTarget(RenderTargetKey key) override;
  bool IsHostDepthEncodingDifferent(xenos::DepthRenderTargetFormat format) const override;

 private:
  class NativeRenderTarget final : public RenderTarget {
   public:
    NativeRenderTarget(RenderTargetKey key, std::unique_ptr<RenderTargetSurface> surface)
        : RenderTarget(key), surface(std::move(surface)) {}
    std::unique_ptr<RenderTargetSurface> surface;
  };
  // A resolve whose destination textures get the render target copied in
  // directly (d3d11_native_resolve_textures).
  struct NativeResolveCopy {
    ID3D11Texture2D* source = nullptr;
    D3D11TextureCache::NativeResolveTarget targets[D3D11TextureCache::kMaxNativeResolveTargets];
    uint32_t target_count = 0;
    uint32_t scale_x = 1;
    uint32_t scale_y = 1;
    bool red_blue_swapped = false;
  };
  void PlanNativeResolveCopy(const draw_util::ResolveInfo& resolve, D3D11TextureCache& textures,
                             NativeResolveCopy& copy);
  void PerformNativeResolveCopy(const NativeResolveCopy& copy, D3D11TextureCache& textures);
  uint64_t native_resolve_copies_ = 0;
  const ShaderProgram* TransferProgram(DxbcRenderTargetTransferShader::TransferShaderKey key,
                                       RenderTargetKey destination);
  bool PerformTransfers(std::span<RenderTarget* const> destinations,
                        const std::vector<Transfer>* transfers,
                        const Transfer::Rectangle* clear_cutout = nullptr);
  bool ClearTarget(NativeRenderTarget& target, const Transfer::Rectangle& rectangle,
                   uint64_t guest_value);
  void RefreshBindings();
  bool Fail(const char* message);

  ui::d3d11::D3D11Device& device_;
  DrawContext& draws_;
  RenderTargetTransfer transfer_;
  RenderTargetEncoder encoder_;
  RenderTargetEncoder unscaled_encoder_;
  ResolveCopy resolve_copy_;
  ShaderCache shaders_;
  std::unordered_map<uint64_t, const ShaderProgram*> transfer_programs_;
  Options options_;
  bool initialized_ = false, failed_ = false, msaa_2x_supported_ = false;
  std::array<ID3D11RenderTargetView*, xenos::kMaxColorRenderTargets> color_views_ = {};
  uint32_t color_view_count_ = 0;
  ID3D11DepthStencilView* depth_view_ = nullptr;
  std::string last_error_;
};

}  // namespace rex::graphics::d3d11
