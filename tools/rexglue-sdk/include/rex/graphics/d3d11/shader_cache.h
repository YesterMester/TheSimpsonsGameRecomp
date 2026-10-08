#pragma once

#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/graphics/d3d11/shader_bytecode.h>
#include <rex/ui/d3d11/d3d11_device.h>

namespace rex::graphics::d3d11 {

// Immutable native shader objects. The device must outlive its shader cache.
class ShaderProgram {
 public:
  const ShaderBytecode& bytecode() const { return bytecode_; }
  ID3D11VertexShader* vertex() const { return vertex_.Get(); }
  ID3D11PixelShader* pixel() const { return pixel_.Get(); }
  ID3D11GeometryShader* geometry() const { return geometry_.Get(); }
  ID3D11HullShader* hull() const { return hull_.Get(); }
  ID3D11DomainShader* domain() const { return domain_.Get(); }
  ID3D11ComputeShader* compute() const { return compute_.Get(); }

 private:
  friend class ShaderCache;
  bool shared_memory_read_only_ = false;
  std::vector<TextureSwizzle> texture_swizzles_;
  std::vector<uint8_t> source_;
  ShaderBytecode bytecode_;
  std::string error_;
  Microsoft::WRL::ComPtr<ID3D11VertexShader> vertex_;
  Microsoft::WRL::ComPtr<ID3D11PixelShader> pixel_;
  Microsoft::WRL::ComPtr<ID3D11GeometryShader> geometry_;
  Microsoft::WRL::ComPtr<ID3D11HullShader> hull_;
  Microsoft::WRL::ComPtr<ID3D11DomainShader> domain_;
  Microsoft::WRL::ComPtr<ID3D11ComputeShader> compute_;
};

// Owned by the command processor thread. Failed conversions and native shader
// creation are retained too, so a bad draw cannot compile again every frame.
class ShaderCache {
 public:
  struct Statistics {
    uint64_t hits = 0;
    uint64_t conversions = 0;
    uint64_t creations = 0;
    uint64_t failures = 0;
  };

  explicit ShaderCache(ui::d3d11::D3D11Device& device) : device_(device) {}

  // A read-only pipeline may route shared-memory UAV loads to the raw SRV.
  // This promise applies to BOTH graphics stages, not only to this shader.
  // Writes in an aliased shader are independently rejected by conversion.
  const ShaderProgram* GetOrCreate(std::span<const uint8_t> shader_model_51,
                                   bool shared_memory_read_only, std::string& error);
  const ShaderProgram* GetOrCreate(std::span<const uint8_t> shader_model_51,
                                   bool shared_memory_read_only,
                                   std::span<const TextureSwizzle> texture_swizzles,
                                   std::string& error);
  void Clear();
  const Statistics& statistics() const { return statistics_; }

 private:
  ShaderBytecodeOptions GetOptions(bool shared_memory_read_only) const;
  HRESULT CreateNative(ShaderProgram& program);

  ui::d3d11::D3D11Device& device_;
  // Hashes narrow the lookup; exact source bytes and the pipeline promise are
  // compared before reuse. A hash collision never selects another shader.
  std::unordered_multimap<uint64_t, std::unique_ptr<ShaderProgram>> programs_;
  Statistics statistics_;
};

}  // namespace rex::graphics::d3d11
