#pragma once

#include <array>
#include <unordered_map>

#include <rex/graphics/d3d11/shared_memory.h>
#include <rex/graphics/d3d11/scaled_resolve_memory.h>
#include <rex/graphics/d3d11/texture_upload.h>
#include <rex/graphics/pipeline/shader/dxbc.h>
#include <rex/graphics/pipeline/texture/cache.h>

namespace rex::graphics::d3d11 {

// Native images backed by the common guest layout, watch and residency tracker.
// Texture decoding and buffer-to-image transfers remain on the GPU. All context
// access belongs to the command processor thread.
class D3D11TextureCache final : public TextureCache {
 public:
  D3D11TextureCache(const RegisterFile& registers, D3D11SharedMemory& memory,
                    ui::d3d11::D3D11Device& device, DrawContext& draws, uint32_t resolution_scale_x,
                    uint32_t resolution_scale_y);
  ~D3D11TextureCache() override;
  void ClearCache() override;
  void RequestTextures(uint32_t used_texture_mask) override;
  bool EnsureScaledResolveMemoryCommitted(uint32_t start, uint32_t length,
                                          uint32_t alignment_log2 = 0) override;
  Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> ScaledResolveDestination(
      uint32_t start, uint32_t length, uint32_t element_size_log2);
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> RequestSwapTexture(xenos::TextureFormat& format,
                                                                      uint32_t& swizzle,
                                                                      uint32_t& width,
                                                                      uint32_t& height,
                                                                      bool& scaled);
  // Resources include shared memory at t0 and guest textures from t1. Channel
  // mappings specialize the native VS/PS variants prepared for these views.
  // Call after RequestTextures and before preparing the final native pipeline.
  bool BuildShaderBindings(std::span<const DxbcShader::TextureBinding> bindings,
                           std::vector<ID3D11ShaderResourceView*>& resources,
                           std::vector<TextureSwizzle>& swizzles, std::string& error);
  const std::string& last_error() const { return last_error_; }

 protected:
  bool IsSignedVersionSeparateForFormat(TextureKey key) const override;
  bool IsScaledResolveSupportedForFormat(TextureKey key) const override;
  uint32_t GetHostFormatSwizzle(TextureKey key) const override;
  uint32_t GetMaxHostTextureWidthHeight(xenos::DataDimension dimension) const override;
  uint32_t GetMaxHostTextureDepthOrArraySize(xenos::DataDimension dimension) const override;
  std::unique_ptr<Texture> CreateTexture(TextureKey key) override;
  bool LoadTextureDataFromResidentMemoryImpl(Texture& texture, bool load_base,
                                             bool load_mips) override;

 private:
  // Kept in guest format order, including formats currently unsupported by the
  // common DXBC decode shaders. Native storage families are selected separately.
  struct HostFormat {
    DXGI_FORMAT resource;
    DXGI_FORMAT unsigned_view;
    LoadShaderIndex load_shader;
    DXGI_FORMAT signed_view;
    LoadShaderIndex signed_load_shader;
    bool block_compressed;
    DXGI_FORMAT decompressed_view;
    LoadShaderIndex decompress_shader;
    uint32_t swizzle;
  };
  struct NativeFormat {
    DXGI_FORMAT resource = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT unsigned_view = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT signed_view = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT storage_view = DXGI_FORMAT_UNKNOWN;
    LoadShaderIndex load_shader = kLoadShaderIndexUnknown;
    TextureUpload::PackedSource packed_source = TextureUpload::PackedSource::kNone;
  };
  class D3D11Texture final : public Texture {
   public:
    D3D11Texture(D3D11TextureCache& cache, TextureKey key, NativeFormat format,
                 Microsoft::WRL::ComPtr<ID3D11Resource> resource);
    NativeFormat format;
    Microsoft::WRL::ComPtr<ID3D11Resource> resource;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> volume_slice;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> volume_slice_write;
    std::vector<Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView>> mip_views;
    std::unordered_map<uint32_t, Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>> views;
  };
  static const HostFormat host_formats_[64];
  static NativeFormat GetNativeFormat(TextureKey key);
  const ShaderProgram* GetLoadProgram(LoadShaderIndex index, bool scaled);
  static bool HasScaledLoadProgram(LoadShaderIndex index);
  ID3D11ShaderResourceView* GetOrCreateView(D3D11Texture& texture,
                                            const DxbcShader::TextureBinding& binding);
  bool Failed(HRESULT result, const char* operation);

  D3D11SharedMemory& native_memory_;
  ui::d3d11::D3D11Device& device_;
  DrawContext& draws_;
  ShaderCache shaders_;
  TextureUpload uploader_;
  ScaledResolveMemory scaled_memory_;
  std::array<std::array<const ShaderProgram*, kLoadShaderCount>, 2> load_programs_ = {};
  std::array<std::array<bool, kLoadShaderCount>, 2> load_attempted_ = {};
  std::array<std::array<std::string, kLoadShaderCount>, 2> load_errors_;
  std::string last_error_;
};

}  // namespace rex::graphics::d3d11
