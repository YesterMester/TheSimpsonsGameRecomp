#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

#include <rex/graphics/d3d11/draw_context.h>

namespace rex::graphics::d3d11 {

// Copies the texture converter's linear GPU buffer into a native image. D3D11
// cannot copy a buffer directly to an image; a typed integer compute store
// preserves the texel bits without a GPU -> CPU -> GPU round trip.
class TextureUpload {
 public:
  enum class PackedSource : uint32_t { kNone, kB5G6R5, kB5G5R5A1, kB4G4R4A4 };
  struct Layout {
    uint32_t offset = 0;
    uint32_t row_pitch = 0;
    uint32_t slice_pitch = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t depth_or_layers = 1;
    PackedSource packed_source = PackedSource::kNone;
  };
  TextureUpload(ui::d3d11::D3D11Device& device, DrawContext& draws)
      : device_(device), draws_(draws) {}
  // Destination is an integer UAV of a typeless native image. Normalized,
  // signed and float views of that image retain the exact source bit pattern.
  // Supported families: R8/RG8/RGBA8, RGB10A2, R16/RG16/RGBA16 and R32/RG32/RGBA32.
  bool Upload(ID3D11ShaderResourceView* source, ID3D11UnorderedAccessView* destination,
              const Layout& layout, std::string& error);
  void Clear();

 private:
  struct Kernel {
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> shader;
    std::string error;
  };
  ui::d3d11::D3D11Device& device_;
  DrawContext& draws_;
  std::unordered_map<uint64_t, Kernel> kernels_;
  Microsoft::WRL::ComPtr<ID3D11Buffer> packed_normalization_;
};

}  // namespace rex::graphics::d3d11
