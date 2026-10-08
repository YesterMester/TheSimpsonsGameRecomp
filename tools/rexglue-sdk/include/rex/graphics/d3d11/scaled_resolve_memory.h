#pragma once

#include <map>
#include <unordered_map>
#include <rex/graphics/d3d11/draw_context.h>

namespace rex::graphics::d3d11 {

// Scaled guest texture storage allocates only the address ranges actually
// used by resolves. Expanding a range preserves its old contents on the GPU;
// outstanding views keep the previous buffer alive until its draws finish.
class ScaledResolveMemory {
 public:
  ScaledResolveMemory(ui::d3d11::D3D11Device& device, DrawContext& draws, uint32_t scale_x,
                      uint32_t scale_y)
      : device_(device), draws_(draws), area_(scale_x * scale_y) {}
  bool Ensure(uint32_t start, uint32_t length, std::string& error);
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> Read(uint32_t start, uint32_t length,
                                                        uint32_t element_size_log2,
                                                        std::string& error);
  Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> Write(uint32_t start, uint32_t length,
                                                          uint32_t element_size_log2,
                                                          std::string& error);
  uint64_t resident_bytes() const { return resident_bytes_; }

 private:
  struct Region {
    uint32_t end;
    Microsoft::WRL::ComPtr<ID3D11Buffer> buffer;
    // Views cover the remainder of this immutable allocation. Extending or
    // merging storage starts a new cache; retained old views keep their data.
    std::unordered_map<uint32_t, Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>> read_views;
    std::unordered_map<uint32_t, Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView>> write_views;
  };
  Region* Find(uint32_t start, uint32_t length, uint32_t& first_byte, std::string& error);
  ui::d3d11::D3D11Device& device_;
  DrawContext& draws_;
  uint32_t area_;
  uint64_t resident_bytes_ = 0;
  std::map<uint32_t, Region> regions_;
};

}  // namespace rex::graphics::d3d11
