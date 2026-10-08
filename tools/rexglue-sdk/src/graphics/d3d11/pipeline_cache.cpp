/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * Native Direct3D 11 shader preparation for the ReXGlue runtime.
 */

#include <rex/graphics/d3d11/pipeline_cache.h>

#include <algorithm>
#include <stdexcept>

#include <rex/graphics/util/draw.h>
#include <rex/hash.h>

namespace rex::graphics::d3d11 {

namespace shaders {
#include "../shaders/bytecode/d3d12_5_1/float24_round_ps.h"
#include "../shaders/bytecode/d3d12_5_1/float24_truncate_ps.h"
#include "../shaders/bytecode/d3d12_5_1/tessellation_adaptive_vs.h"
#include "../shaders/bytecode/d3d12_5_1/tessellation_indexed_vs.h"
#include "../shaders/bytecode/d3d12_5_1/adaptive_triangle_hs.h"
#include "../shaders/bytecode/d3d12_5_1/adaptive_quad_hs.h"
#include "../shaders/bytecode/d3d12_5_1/discrete_triangle_3cp_hs.h"
#include "../shaders/bytecode/d3d12_5_1/discrete_triangle_1cp_hs.h"
#include "../shaders/bytecode/d3d12_5_1/discrete_quad_4cp_hs.h"
#include "../shaders/bytecode/d3d12_5_1/discrete_quad_1cp_hs.h"
#include "../shaders/bytecode/d3d12_5_1/continuous_triangle_3cp_hs.h"
#include "../shaders/bytecode/d3d12_5_1/continuous_triangle_1cp_hs.h"
#include "../shaders/bytecode/d3d12_5_1/continuous_quad_4cp_hs.h"
#include "../shaders/bytecode/d3d12_5_1/continuous_quad_1cp_hs.h"
}  // namespace shaders

PipelineCache::PipelineCache(ui::d3d11::D3D11Device& device, const RegisterFile& registers,
                             const Options& options)
    : registers_(registers),
      options_(options),
      translator_(static_cast<ui::GraphicsProvider::GpuVendorID>(device.features().vendor_id),
                  false, false, options.gamma_render_target_as_unorm8, options.msaa_2x_supported,
                  options.resolution_scale_x, options.resolution_scale_y),
      native_shaders_(device) {
  if (!options.resolution_scale_x || !options.resolution_scale_y) {
    throw std::invalid_argument("The DX11 draw resolution scale must be positive");
  }
  static_assert(uint32_t(DxbcShaderTranslator::SRVMainRegister::kSharedMemory) == 0);
  static_assert(uint32_t(DxbcShaderTranslator::UAVRegister::kSharedMemory) == 0);
}

D3D11Shader* PipelineCache::LoadShader(xenos::ShaderType type,
                                       std::span<const uint32_t> guest_words) {
  if (guest_words.empty() || guest_words.size() > 256 * 1024 ||
      (type != xenos::ShaderType::kVertex && type != xenos::ShaderType::kPixel)) {
    return nullptr;
  }
  uint64_t hash = XXH3_64bits(guest_words.data(), guest_words.size_bytes());
  auto [begin, end] = shaders_.equal_range(hash);
  for (auto it = begin; it != end; ++it) {
    const auto& loaded = it->second;
    if (loaded.shader->type() == type && std::equal(loaded.source.begin(), loaded.source.end(),
                                                    guest_words.begin(), guest_words.end())) {
      return loaded.shader.get();
    }
  }
  LoadedShader loaded;
  loaded.source.assign(guest_words.begin(), guest_words.end());
  loaded.shader = std::make_unique<D3D11Shader>(type, hash, guest_words.data(), guest_words.size());
  auto* shader = loaded.shader.get();
  shaders_.emplace(hash, std::move(loaded));
  return shader;
}

void PipelineCache::AnalyzeShader(D3D11Shader& shader) {
  if (!shader.is_ucode_analyzed()) {
    shader.AnalyzeUcode(disassembly_);
  }
}

DxbcShaderTranslator::Modification PipelineCache::VertexModification(
    const D3D11Shader& shader, Shader::HostVertexShaderType type, uint32_t interpolators) const {
  const auto& regs = registers_;
  DxbcShaderTranslator::Modification modification(translator_.GetDefaultVertexShaderModification(
      shader.GetDynamicAddressableRegisterCount(regs.Get<reg::SQ_PROGRAM_CNTL>().vs_num_reg),
      type));
  modification.vertex.interpolator_mask = interpolators;
  auto clip = regs.Get<reg::PA_CL_CLIP_CNTL>();
  uint32_t user_clip_planes = clip.clip_disable ? 0 : clip.ucp_ena;
  modification.vertex.user_clip_plane_count = rex::bit_count(user_clip_planes);
  modification.vertex.user_clip_plane_cull = uint32_t(user_clip_planes && clip.ucp_cull_only_ena);
  modification.vertex.point_ps_ucp_mode = clip.ps_ucp_mode;
  modification.vertex.vertex_kill_and =
      uint32_t((shader.writes_point_size_edge_flag_kill_vertex() & 0b100) && !clip.vtx_kill_or);
  modification.vertex.output_point_size =
      uint32_t((shader.writes_point_size_edge_flag_kill_vertex() & 0b001) &&
               regs.Get<reg::VGT_DRAW_INITIATOR>().prim_type == xenos::PrimitiveType::kPointList);
  return modification;
}

DxbcShaderTranslator::Modification PipelineCache::PixelModification(
    const D3D11Shader& shader, uint32_t interpolators, uint32_t param_gen_position,
    reg::RB_DEPTHCONTROL depth) const {
  const auto& regs = registers_;
  DxbcShaderTranslator::Modification modification(translator_.GetDefaultPixelShaderModification(
      shader.GetDynamicAddressableRegisterCount(regs.Get<reg::SQ_PROGRAM_CNTL>().ps_num_reg)));
  modification.pixel.interpolator_mask = interpolators;
  modification.pixel.interpolators_centroid =
      interpolators & ~xenos::GetInterpolatorSamplingPattern(
                          regs.Get<reg::RB_SURFACE_INFO>().msaa_samples,
                          regs.Get<reg::SQ_CONTEXT_MISC>().sc_sample_cntl,
                          regs.Get<reg::SQ_INTERPOLATOR_CNTL>().sampling_pattern);
  if (param_gen_position < xenos::kMaxInterpolators) {
    modification.pixel.param_gen_enable = 1;
    modification.pixel.param_gen_interpolator = param_gen_position;
    modification.pixel.param_gen_point =
        uint32_t(regs.Get<reg::VGT_DRAW_INITIATOR>().prim_type == xenos::PrimitiveType::kPointList);
  }
  using DepthStencilMode = DxbcShaderTranslator::Modification::DepthStencilMode;
  if (options_.depth_float24_convert_in_pixel_shader && depth.z_enable &&
      regs.Get<reg::RB_DEPTH_INFO>().depth_format == xenos::DepthRenderTargetFormat::kD24FS8) {
    modification.pixel.depth_stencil_mode = options_.depth_float24_round
                                                ? DepthStencilMode::kFloat24Rounding
                                                : DepthStencilMode::kFloat24Truncating;
  } else if (shader.implicit_early_z_write_allowed() &&
             (!shader.writes_color_target(0) ||
              !draw_util::DoesCoverageDependOnAlpha(regs.Get<reg::RB_COLORCONTROL>()))) {
    modification.pixel.depth_stencil_mode = DepthStencilMode::kEarlyHint;
  } else {
    modification.pixel.depth_stencil_mode = DepthStencilMode::kNoModifiers;
  }
  return modification;
}

const ShaderProgram* PipelineCache::PrepareTranslation(
    D3D11Shader& shader, uint64_t modification, bool shared_memory_read_only,
    std::span<const TextureSwizzle> texture_swizzles, std::string& error) {
  auto* translation =
      static_cast<D3D11Shader::D3D11Translation*>(shader.GetOrCreateTranslation(modification));
  if (!translation->translation_attempted_) {
    translation->translation_attempted_ = true;
    if (!translator_.TranslateAnalyzedShader(*translation) || !translation->is_valid()) {
      translation->translation_error_ = "The guest shader could not be compiled for DX11";
    }
  }
  if (!translation->translation_error_.empty()) {
    error = translation->translation_error_;
    return nullptr;
  }
  size_t access = size_t(shared_memory_read_only);
  auto variant =
      std::find_if(translation->native_variants_.begin(), translation->native_variants_.end(),
                   [&](const auto& entry) {
                     return std::equal(entry.texture_swizzles.begin(), entry.texture_swizzles.end(),
                                       texture_swizzles.begin(), texture_swizzles.end());
                   });
  if (variant == translation->native_variants_.end()) {
    variant = translation->native_variants_.emplace(translation->native_variants_.end());
    variant->texture_swizzles.assign(texture_swizzles.begin(), texture_swizzles.end());
  }
  if (!variant->creation_attempted[access]) {
    variant->creation_attempted[access] = true;
    variant->programs[access] =
        native_shaders_.GetOrCreate(translation->translated_binary(), shared_memory_read_only,
                                    texture_swizzles, variant->creation_errors[access]);
  }
  error = variant->creation_errors[access];
  return variant->programs[access];
}

bool PipelineCache::PrepareShaders(D3D11Shader& vertex, D3D11Shader* pixel,
                                   Shader::HostVertexShaderType vertex_type,
                                   xenos::PrimitiveType primitive_type,
                                   reg::RB_DEPTHCONTROL normalized_depth_control,
                                   PreparedShaders& output, std::string& error,
                                   std::span<const TextureSwizzle> vertex_texture_swizzles,
                                   std::span<const TextureSwizzle> pixel_texture_swizzles) {
  output = {};
  error.clear();
  if (vertex.type() != xenos::ShaderType::kVertex ||
      (pixel && pixel->type() != xenos::ShaderType::kPixel)) {
    error = "The DX11 draw has shaders in the wrong stages";
    return false;
  }
  AnalyzeShader(vertex);
  if (pixel) {
    AnalyzeShader(*pixel);
  }
  PreparedShaders prepared;
  prepared.shared_memory_read_only =
      !vertex.memexport_eM_written() && !(pixel && pixel->memexport_eM_written());
  uint32_t param_gen_position = UINT32_MAX;
  prepared.interpolator_mask =
      pixel ? vertex.writes_interpolators() &
                  pixel->GetInterpolatorInputMask(registers_.Get<reg::SQ_PROGRAM_CNTL>(),
                                                  registers_.Get<reg::SQ_CONTEXT_MISC>(),
                                                  param_gen_position)
            : 0;
  prepared.vertex_modification =
      VertexModification(vertex, vertex_type, prepared.interpolator_mask).value;
  prepared.vertex =
      PrepareTranslation(vertex, prepared.vertex_modification, prepared.shared_memory_read_only,
                         vertex_texture_swizzles, error);
  if (!prepared.vertex) {
    return false;
  }
  if (pixel) {
    prepared.pixel_modification = PixelModification(*pixel, prepared.interpolator_mask,
                                                    param_gen_position, normalized_depth_control)
                                      .value;
    prepared.pixel =
        PrepareTranslation(*pixel, prepared.pixel_modification, prepared.shared_memory_read_only,
                           pixel_texture_swizzles, error);
    if (!prepared.pixel) {
      return false;
    }
  } else if (options_.depth_float24_convert_in_pixel_shader && normalized_depth_control.z_enable &&
             (normalized_depth_control.zfunc != xenos::CompareFunction::kAlways ||
              normalized_depth_control.z_write_enable) &&
             registers_.Get<reg::RB_DEPTH_INFO>().depth_format ==
                 xenos::DepthRenderTargetFormat::kD24FS8) {
    // Depth prepasses have no guest pixel shader. They must store the same
    // float24 depth as the following color pass, or an EQUAL test rejects
    // most of the environment even though both passes use the same vertices.
    std::span<const uint8_t> code = options_.depth_float24_round
                                        ? std::span<const uint8_t>(shaders::float24_round_ps)
                                        : std::span<const uint8_t>(shaders::float24_truncate_ps);
    prepared.pixel = native_shaders_.GetOrCreate(code, false, error);
    if (!prepared.pixel)
      return false;
  }
  if (vertex_type == Shader::HostVertexShaderType::kVertex &&
      !PrepareGeometry(primitive_type, prepared, error)) {
    return false;
  }
  output = prepared;
  return true;
}

bool PipelineCache::PrepareDraw(D3D11Shader& vertex, D3D11Shader* pixel,
                                Shader::HostVertexShaderType vertex_type,
                                xenos::PrimitiveType primitive_type,
                                reg::RB_DEPTHCONTROL normalized_depth_control,
                                uint32_t normalized_color_mask, uint32_t bound_depth_and_color_mask,
                                DrawCommand& command, draw_util::ViewportInfo& viewport,
                                PreparedShaders& shaders, std::string& error,
                                std::span<const TextureSwizzle> vertex_texture_swizzles,
                                std::span<const TextureSwizzle> pixel_texture_swizzles) {
  if (!PrepareShaders(vertex, pixel, vertex_type, primitive_type, normalized_depth_control, shaders,
                      error, vertex_texture_swizzles, pixel_texture_swizzles))
    return false;
  if (Shader::IsHostVertexShaderTypeDomain(vertex_type)) {
    command.programs[size_t(DrawStage::kDomain)] = shaders.vertex;
  } else {
    command.programs[size_t(DrawStage::kVertex)] = shaders.vertex;
    command.programs[size_t(DrawStage::kHull)] = nullptr;
    command.programs[size_t(DrawStage::kDomain)] = nullptr;
  }
  command.programs[size_t(DrawStage::kGeometry)] = shaders.geometry;
  command.programs[size_t(DrawStage::kPixel)] = shaders.pixel;
  BuildGuestDrawState(registers_, normalized_depth_control, normalized_color_mask,
                      bound_depth_and_color_mask, options_.resolution_scale_x,
                      options_.resolution_scale_y, pixel && pixel->writes_depth(),
                      options_.depth_float24_convert_in_pixel_shader, options_.msaa_2x_supported,
                      viewport, command.state);
  return true;
}

bool PipelineCache::PrepareGeometry(xenos::PrimitiveType primitive_type, PreparedShaders& output,
                                    std::string& error) {
  DxbcGeometryShaderKey key;
  switch (primitive_type) {
    case xenos::PrimitiveType::kPointList:
      key.type = DxbcGeometryShaderType::kPointList;
      break;
    case xenos::PrimitiveType::kRectangleList:
      key.type = DxbcGeometryShaderType::kRectangleList;
      break;
    case xenos::PrimitiveType::kQuadList:
      key.type = DxbcGeometryShaderType::kQuadList;
      break;
    default:
      output.geometry = nullptr;
      return true;
  }
  DxbcShaderTranslator::Modification vertex(output.vertex_modification);
  DxbcShaderTranslator::Modification pixel(output.pixel_modification);
  key.interpolator_count = rex::bit_count(vertex.vertex.interpolator_mask);
  key.user_clip_plane_count = vertex.vertex.user_clip_plane_count;
  key.user_clip_plane_cull = vertex.vertex.user_clip_plane_cull;
  key.has_vertex_kill_and = vertex.vertex.vertex_kill_and;
  key.has_point_size = vertex.vertex.output_point_size;
  key.has_point_coordinates = pixel.pixel.param_gen_point;
  key.point_ps_ucp_mode = vertex.vertex.point_ps_ucp_mode;
  auto found = geometry_programs_.find(key.key);
  if (found == geometry_programs_.end()) {
    std::vector<uint32_t> code;
    CreateDxbcGeometryShader(key, code);
    GeometryProgram geometry = {};
    geometry.program = native_shaders_.GetOrCreate(
        {reinterpret_cast<const uint8_t*>(code.data()), code.size() * sizeof(uint32_t)}, false,
        geometry.error);
    found = geometry_programs_.emplace(key.key, std::move(geometry)).first;
  }
  error = found->second.error;
  output.geometry = found->second.program;
  return output.geometry != nullptr;
}

bool PipelineCache::PrepareTessellation(Shader::HostVertexShaderType type,
                                        xenos::TessellationMode mode, DrawCommand& command,
                                        std::string& error) {
  using Type = Shader::HostVertexShaderType;
  bool triangle =
      type == Type::kTriangleDomainCPIndexed || type == Type::kTriangleDomainPatchIndexed;
  bool quad = type == Type::kQuadDomainCPIndexed || type == Type::kQuadDomainPatchIndexed;
  bool cp_indexed = type == Type::kTriangleDomainCPIndexed || type == Type::kQuadDomainCPIndexed;
  if ((!triangle && !quad) || (cp_indexed && mode == xenos::TessellationMode::kAdaptive)) {
    error = "Unsupported native tessellation domain or control points";
    return false;
  }
  std::span<const uint8_t> vertex, hull;
  auto bytes = [](const auto& code) { return std::span<const uint8_t>(code, sizeof(code)); };
  vertex = mode == xenos::TessellationMode::kAdaptive ? bytes(shaders::tessellation_adaptive_vs)
                                                      : bytes(shaders::tessellation_indexed_vs);
  switch (mode) {
    case xenos::TessellationMode::kDiscrete:
      hull = triangle ? (cp_indexed ? bytes(shaders::discrete_triangle_3cp_hs)
                                    : bytes(shaders::discrete_triangle_1cp_hs))
                      : (cp_indexed ? bytes(shaders::discrete_quad_4cp_hs)
                                    : bytes(shaders::discrete_quad_1cp_hs));
      break;
    case xenos::TessellationMode::kContinuous:
      hull = triangle ? (cp_indexed ? bytes(shaders::continuous_triangle_3cp_hs)
                                    : bytes(shaders::continuous_triangle_1cp_hs))
                      : (cp_indexed ? bytes(shaders::continuous_quad_4cp_hs)
                                    : bytes(shaders::continuous_quad_1cp_hs));
      break;
    case xenos::TessellationMode::kAdaptive:
      hull = triangle ? bytes(shaders::adaptive_triangle_hs) : bytes(shaders::adaptive_quad_hs);
      break;
    default:
      error = "Unknown native tessellation mode";
      return false;
  }
  auto* vertex_program = native_shaders_.GetOrCreate(vertex, true, error);
  if (!vertex_program)
    return false;
  auto* hull_program = native_shaders_.GetOrCreate(hull, true, error);
  if (!hull_program)
    return false;
  command.programs[size_t(DrawStage::kVertex)] = vertex_program;
  command.programs[size_t(DrawStage::kHull)] = hull_program;
  command.topology = (cp_indexed || mode == xenos::TessellationMode::kAdaptive)
                         ? (triangle ? D3D11_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST
                                     : D3D11_PRIMITIVE_TOPOLOGY_4_CONTROL_POINT_PATCHLIST)
                         : D3D11_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST;
  return true;
}

void PipelineCache::Clear() {
  shaders_.clear();
  geometry_programs_.clear();
  native_shaders_.Clear();
  disassembly_.Reset();
}

}  // namespace rex::graphics::d3d11
