#pragma once

#include <array>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/graphics/d3d11/draw_state.h>
#include <rex/graphics/d3d11/shader_cache.h>
#include <rex/graphics/pipeline/shader/dxbc.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/util/dxbc_geometry.h>
#include <rex/string/buffer.h>

namespace rex::graphics::d3d11 {

class PipelineCache;

class D3D11Shader final : public DxbcShader {
 public:
  using DxbcShader::DxbcShader;

  class D3D11Translation final : public DxbcTranslation {
   public:
    D3D11Translation(D3D11Shader& shader, uint64_t modification)
        : DxbcTranslation(shader, modification) {}

   private:
    friend class PipelineCache;
    bool translation_attempted_ = false;
    std::string translation_error_;
    // Native programs also specialize texture channel mappings. Keep the two
    // shared-memory access modes separate, and retain failed variants too.
    struct NativeVariant {
      std::vector<TextureSwizzle> texture_swizzles;
      std::array<bool, 2> creation_attempted = {};
      std::array<const ShaderProgram*, 2> programs = {};
      std::array<std::string, 2> creation_errors;
    };
    std::vector<NativeVariant> native_variants_;
  };

 protected:
  Translation* CreateTranslationInstance(uint64_t modification) override {
    return new D3D11Translation(*this, modification);
  }
};

// Shader preparation for the native draw path, owned by the command processor.
// Resource binding and fixed-function state are submitted by the draw context.
class PipelineCache {
 public:
  struct Options {
    uint32_t resolution_scale_x = 1;
    uint32_t resolution_scale_y = 1;
    bool gamma_render_target_as_unorm8 = false;
    bool msaa_2x_supported = true;
    bool depth_float24_convert_in_pixel_shader = true;
    bool depth_float24_round = false;
  };

  struct PreparedShaders {
    const ShaderProgram* vertex = nullptr;
    const ShaderProgram* pixel = nullptr;
    const ShaderProgram* geometry = nullptr;
    uint64_t vertex_modification = 0;
    uint64_t pixel_modification = 0;
    uint32_t interpolator_mask = 0;
    bool shared_memory_read_only = false;
  };

  PipelineCache(ui::d3d11::D3D11Device& device, const RegisterFile& registers,
                const Options& options);
  D3D11Shader* LoadShader(xenos::ShaderType type, std::span<const uint32_t> guest_words);
  void AnalyzeShader(D3D11Shader& shader);
  // The caller removes a pixel shader with no rasterization side effects first.
  // Both stages are analyzed before the shared-memory read-only promise is made.
  bool PrepareShaders(D3D11Shader& vertex, D3D11Shader* pixel,
                      Shader::HostVertexShaderType vertex_type, xenos::PrimitiveType primitive_type,
                      reg::RB_DEPTHCONTROL normalized_depth_control, PreparedShaders& output,
                      std::string& error,
                      std::span<const TextureSwizzle> vertex_texture_swizzles = {},
                      std::span<const TextureSwizzle> pixel_texture_swizzles = {});
  // Adds normalized guest raster, depth/stencil and blend state to a draw.
  // The command processor supplies resources, topology and auxiliary tessellation
  // shaders, and uploads the returned viewport's NDC values in system constants.
  bool PrepareDraw(D3D11Shader& vertex, D3D11Shader* pixel,
                   Shader::HostVertexShaderType vertex_type, xenos::PrimitiveType primitive_type,
                   reg::RB_DEPTHCONTROL normalized_depth_control, uint32_t normalized_color_mask,
                   uint32_t bound_depth_and_color_mask, DrawCommand& command,
                   draw_util::ViewportInfo& viewport, PreparedShaders& shaders, std::string& error,
                   std::span<const TextureSwizzle> vertex_texture_swizzles = {},
                   std::span<const TextureSwizzle> pixel_texture_swizzles = {});
  // Drop active shader and prepared pipeline references before clearing.
  void Clear();
  bool PrepareTessellation(Shader::HostVertexShaderType type, xenos::TessellationMode mode,
                           DrawCommand& command, std::string& error);
  const ShaderCache::Statistics& shader_statistics() const { return native_shaders_.statistics(); }

 private:
  struct LoadedShader {
    std::vector<uint32_t> source;
    std::unique_ptr<D3D11Shader> shader;
  };
  DxbcShaderTranslator::Modification VertexModification(const D3D11Shader& shader,
                                                        Shader::HostVertexShaderType type,
                                                        uint32_t interpolators) const;
  DxbcShaderTranslator::Modification PixelModification(const D3D11Shader& shader,
                                                       uint32_t interpolators,
                                                       uint32_t param_gen_position,
                                                       reg::RB_DEPTHCONTROL depth) const;
  const ShaderProgram* PrepareTranslation(D3D11Shader& shader, uint64_t modification,
                                          bool shared_memory_read_only,
                                          std::span<const TextureSwizzle> texture_swizzles,
                                          std::string& error);
  bool PrepareGeometry(xenos::PrimitiveType primitive_type, PreparedShaders& output,
                       std::string& error);
  struct GeometryProgram {
    const ShaderProgram* program;
    std::string error;
  };

  const RegisterFile& registers_;
  Options options_;
  DxbcShaderTranslator translator_;
  ShaderCache native_shaders_;
  rex::string::StringBuffer disassembly_;
  std::unordered_multimap<uint64_t, LoadedShader> shaders_;
  std::unordered_map<uint32_t, GeometryProgram> geometry_programs_;
};

}  // namespace rex::graphics::d3d11
