#pragma once

#include <unordered_map>

#include <rex/graphics/d3d11/guest_sampler.h>
#include <rex/graphics/d3d11/pipeline_cache.h>
#include <rex/graphics/d3d11/primitive_processor.h>
#include <rex/graphics/d3d11/render_target_cache.h>
#include <rex/graphics/d3d11/sampler_cache.h>
#include <rex/graphics/d3d11/system_constants.h>
#include <rex/graphics/d3d11/texture_cache.h>

namespace rex::graphics::d3d11 {

// Connects common guest draw state to the native shader, image and draw caches.
// Owned by the game command processor, which serializes the immediate context
// with presentation and invokes EndFrame at its real swap boundary.
class GuestDraw {
 public:
  GuestDraw(const RegisterFile& registers, ui::d3d11::D3D11Device& device, DrawContext& draws,
            D3D11SharedMemory& memory, D3D11TextureCache& textures,
            D3D11RenderTargetCache& render_targets, D3D11PrimitiveProcessor& primitives,
            PipelineCache& pipelines)
      : registers_(registers),
        device_(device),
        draws_(draws),
        memory_(memory),
        textures_(textures),
        render_targets_(render_targets),
        primitives_(primitives),
        pipelines_(pipelines),
        buffers_(device),
        samplers_(device) {}
  bool Submit(D3D11Shader* vertex, D3D11Shader* pixel, std::string& error);
  void ClearCache();

 private:
  struct Bindings {
    std::array<ID3D11Buffer*, 4> constant_views;
    std::vector<ID3D11ShaderResourceView*> resources;
    std::vector<TextureSwizzle> swizzles;
    std::vector<Microsoft::WRL::ComPtr<ID3D11SamplerState>> samplers;
    std::vector<ID3D11SamplerState*> sampler_views;
  };
  bool PrepareBindings(const D3D11Shader* shader, bool pixel_stage, bool shared_memory_is_uav,
                       const DxbcShaderTranslator::SystemConstants& system_constants,
                       Bindings& output, std::string& error);
  bool PrepareIndices(const PrimitiveProcessor::ProcessingResult& primitive, DrawCommand& command,
                      std::string& error);
  // Constant blocks: system, vertex floats, pixel floats, bool and loop, and
  // fetch. Each exact size has its own dynamic buffer, so reads past a block
  // still return zero. A changed block is rewritten with WRITE_DISCARD, which
  // leaves queued draws their own copy; an unchanged one keeps its buffer.
  enum ConstantBlock { kSystemConstants, kVertexFloats, kPixelFloats, kBoolLoop, kFetch, kCount };
  struct ConstantBuffer {
    Microsoft::WRL::ComPtr<ID3D11Buffer> buffer;
    std::vector<uint8_t> contents;
  };
  ID3D11Buffer* UpdateConstants(ConstantBlock block, std::span<const uint8_t> data,
                                std::string& error);
  // Finds the words of each vertex fetch constant the draw can read, when the
  // shader analysis proves that its attributes fetch at the vertex index.
  void GetVertexRanges(const D3D11Shader& vertex, const D3D11Shader* pixel,
                       const PrimitiveProcessor::ProcessingResult& primitive);
  const RegisterFile& registers_;
  ui::d3d11::D3D11Device& device_;
  DrawContext& draws_;
  D3D11SharedMemory& memory_;
  D3D11TextureCache& textures_;
  D3D11RenderTargetCache& render_targets_;
  D3D11PrimitiveProcessor& primitives_;
  PipelineCache& pipelines_;
  BufferCache buffers_;
  SamplerCache samplers_;
  std::array<std::unordered_map<uint32_t, ConstantBuffer>, kCount> constants_;
  // Storage reused by every draw.
  Bindings vertex_bindings_, pixel_bindings_;
  std::vector<uint32_t> vertex_floats_, pixel_floats_;
  std::vector<draw_util::MemExportRange> exports_;
  // Fetch constants with a proven range, and their first and end words.
  std::array<uint64_t, 2> vertex_ranges_valid_ = {};
  std::array<std::pair<uint32_t, uint32_t>, 96> vertex_ranges_ = {};
};

}  // namespace rex::graphics::d3d11
