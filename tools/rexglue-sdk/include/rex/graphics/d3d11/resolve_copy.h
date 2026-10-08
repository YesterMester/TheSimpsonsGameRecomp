#pragma once

#include <array>

#include <rex/graphics/d3d11/render_target_encoder.h>
#include <rex/graphics/util/draw.h>

namespace rex::graphics::d3d11 {

// Copies encoded native attachments into guest tiled texture memory. Both
// versions use native compute shaders; the scaled version addresses a buffer
// relative to the resolve base instead of the entire physical address space.
class ResolveCopy {
 public:
  ResolveCopy(ui::d3d11::D3D11Device& device, DrawContext& draws)
      : device_(device), draws_(draws), shaders_(device), buffers_(device, 1024 * 1024) {}
  bool Copy(const RenderTargetEncoder& source, draw_util::ResolveCopyShaderIndex shader,
            const draw_util::ResolveCopyShaderConstants& constants,
            ID3D11UnorderedAccessView* destination, uint32_t groups_x, uint32_t groups_y,
            bool scaled, std::string& error);
  void Clear();

 private:
  const ShaderProgram* Program(draw_util::ResolveCopyShaderIndex shader, bool scaled,
                               std::string& error);
  bool PrepareDepthCopy(ID3D11Buffer* destination, uint32_t first_word, uint32_t word_count,
                        std::string& error);
  ui::d3d11::D3D11Device& device_;
  DrawContext& draws_;
  ShaderCache shaders_;
  BufferCache buffers_;
  std::array<std::array<const ShaderProgram*, size_t(draw_util::ResolveCopyShaderIndex::kCount)>, 2>
      programs_ = {};
  std::array<std::array<bool, size_t(draw_util::ResolveCopyShaderIndex::kCount)>, 2> attempted_ =
      {};
  std::array<std::array<std::string, size_t(draw_util::ResolveCopyShaderIndex::kCount)>, 2> errors_;
  Microsoft::WRL::ComPtr<ID3D11ComputeShader> depth_copy_;
  Microsoft::WRL::ComPtr<ID3D11Buffer> depth_destination_buffer_;
  Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> depth_destination_;
  uint32_t depth_destination_first_word_ = 0, depth_destination_word_count_ = 0;
  bool depth_copy_attempted_ = false;
  std::string depth_copy_error_;
};

}  // namespace rex::graphics::d3d11
