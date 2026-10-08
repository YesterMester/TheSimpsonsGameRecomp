#include <rex/graphics/d3d11/shader_cache.h>

#include <algorithm>
#include <cstdio>

#include <rex/graphics/format/dxbc.h>

namespace rex::graphics::d3d11 {

namespace {
uint64_t HashSource(std::span<const uint8_t> source, bool read_only,
                    std::span<const TextureSwizzle> swizzles) {
  uint64_t hash = 0xCBF29CE484222325ull;
  for (uint8_t byte : source) {
    hash = (hash ^ byte) * 0x100000001B3ull;
  }
  hash = (hash ^ uint64_t(read_only)) * 0x100000001B3ull;
  for (const auto& swizzle : swizzles) {
    hash = (hash ^ swizzle.srv_register) * 0x100000001B3ull;
    hash = (hash ^ swizzle.swizzle) * 0x100000001B3ull;
  }
  return hash;
}
}  // namespace

ShaderBytecodeOptions ShaderCache::GetOptions(bool shared_memory_read_only) const {
  ShaderBytecodeOptions options;
  const auto& features = device_.features();
  if (features.level >= D3D_FEATURE_LEVEL_11_1) {
    options.uav_register_count = D3D11_1_UAV_SLOT_COUNT;
    options.supported_feature_flags |=
        dxbc::kShaderFeature0_UAVsAtEveryStage | dxbc::kShaderFeature0_64UAVs;
  }
  if (features.rasterizer_ordered_views) {
    options.supported_feature_flags |= dxbc::kShaderFeature0_ROVs;
  }
  if (features.pixel_shader_stencil_reference) {
    options.supported_feature_flags |= dxbc::kShaderFeature0_StencilRef;
  }
  if (features.typed_uav_load_additional_formats) {
    options.supported_feature_flags |= dxbc::kShaderFeature0_TypedUAVLoadAdditionalFormats;
  }
  if (shared_memory_read_only) {
    // The translator's shared-memory SRV and UAV both use register zero.
    options.read_only_uav_aliases.push_back({0, 0});
  }
  return options;
}

HRESULT ShaderCache::CreateNative(ShaderProgram& program) {
  const auto& code = program.bytecode_;
  auto* device = device_.device();
  switch (code.program_type) {
    case uint32_t(dxbc::ProgramType::kPixelShader):
      return device->CreatePixelShader(code.data.data(), code.data.size(), nullptr,
                                       program.pixel_.GetAddressOf());
    case uint32_t(dxbc::ProgramType::kVertexShader):
      return device->CreateVertexShader(code.data.data(), code.data.size(), nullptr,
                                        program.vertex_.GetAddressOf());
    case uint32_t(dxbc::ProgramType::kGeometryShader):
      return device->CreateGeometryShader(code.data.data(), code.data.size(), nullptr,
                                          program.geometry_.GetAddressOf());
    case uint32_t(dxbc::ProgramType::kHullShader):
      return device->CreateHullShader(code.data.data(), code.data.size(), nullptr,
                                      program.hull_.GetAddressOf());
    case uint32_t(dxbc::ProgramType::kDomainShader):
      return device->CreateDomainShader(code.data.data(), code.data.size(), nullptr,
                                        program.domain_.GetAddressOf());
    case uint32_t(dxbc::ProgramType::kComputeShader):
      return device->CreateComputeShader(code.data.data(), code.data.size(), nullptr,
                                         program.compute_.GetAddressOf());
    default:
      return E_INVALIDARG;
  }
}

const ShaderProgram* ShaderCache::GetOrCreate(std::span<const uint8_t> shader_model_51,
                                              bool shared_memory_read_only, std::string& error) {
  return GetOrCreate(shader_model_51, shared_memory_read_only, {}, error);
}

const ShaderProgram* ShaderCache::GetOrCreate(std::span<const uint8_t> shader_model_51,
                                              bool shared_memory_read_only,
                                              std::span<const TextureSwizzle> texture_swizzles,
                                              std::string& error) {
  error.clear();
  // Bound the retained input independently of the bytecode parser. Callers
  // may pass trace or shader-cache data as well as trusted translations.
  if (shader_model_51.empty() || shader_model_51.size() > 16 * 1024 * 1024) {
    error = "Invalid native shader input size";
    return nullptr;
  }
  if (texture_swizzles.size() > D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT) {
    error = "DX11 shader has too many texture channel mappings";
    return nullptr;
  }
  std::vector<TextureSwizzle> mappings(texture_swizzles.begin(), texture_swizzles.end());
  std::sort(mappings.begin(), mappings.end(),
            [](const auto& a, const auto& b) { return a.srv_register < b.srv_register; });
  for (size_t i = 0; i < mappings.size(); ++i) {
    if ((i && mappings[i - 1].srv_register == mappings[i].srv_register) ||
        mappings[i].srv_register >= D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT ||
        mappings[i].swizzle > 0xFFF) {
      error = "Invalid or duplicate DX11 texture channel mapping";
      return nullptr;
    }
    for (uint32_t component = 0; component < 4; ++component) {
      if (((mappings[i].swizzle >> (component * 3)) & 7) > 5) {
        error = "Invalid DX11 texture channel selector";
        return nullptr;
      }
    }
  }
  std::erase_if(mappings,
                [](const auto& mapping) { return mapping.swizzle == kIdentityTextureSwizzle; });
  uint64_t hash = HashSource(shader_model_51, shared_memory_read_only, mappings);
  auto [begin, end] = programs_.equal_range(hash);
  for (auto it = begin; it != end; ++it) {
    const auto& program = *it->second;
    if (program.shared_memory_read_only_ == shared_memory_read_only &&
        program.texture_swizzles_ == mappings &&
        std::equal(program.source_.begin(), program.source_.end(), shader_model_51.begin(),
                   shader_model_51.end())) {
      ++statistics_.hits;
      error = program.error_;
      return error.empty() ? &program : nullptr;
    }
  }
  auto program = std::unique_ptr<ShaderProgram>(new ShaderProgram());
  program->source_.assign(shader_model_51.begin(), shader_model_51.end());
  program->shared_memory_read_only_ = shared_memory_read_only;
  program->texture_swizzles_ = mappings;
  ++statistics_.conversions;
  ShaderBytecodeOptions options = GetOptions(shared_memory_read_only);
  options.texture_swizzles = std::move(mappings);
  if (ConvertShaderBytecode(shader_model_51, options, program->bytecode_, program->error_)) {
    HRESULT result = CreateNative(*program);
    if (SUCCEEDED(result)) {
      ++statistics_.creations;
    } else {
      char message[128];
      std::snprintf(message, sizeof(message), "Native shader creation failed (0x%08X)",
                    unsigned(result));
      program->error_ = message;
    }
  }
  if (!program->error_.empty()) {
    ++statistics_.failures;
  }
  error = program->error_;
  const ShaderProgram* result = error.empty() ? program.get() : nullptr;
  programs_.emplace(hash, std::move(program));
  return result;
}

void ShaderCache::Clear() {
  programs_.clear();
  statistics_ = {};
}

}  // namespace rex::graphics::d3d11
