#pragma once

#include <array>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <rex/graphics/d3d11/buffer_cache.h>
#include <rex/graphics/d3d11/shader_cache.h>

namespace rex::graphics::d3d11 {

enum class DrawStage : uint32_t { kVertex, kHull, kDomain, kGeometry, kPixel, kCount };

struct StageBindings {
  // Fixed native registers, starting at zero. Missing trailing slots are null.
  std::span<ID3D11Buffer* const> constants;
  std::span<ID3D11ShaderResourceView* const> resources;
  std::span<ID3D11SamplerState* const> samplers;
};

struct DrawState {
  D3D11_RASTERIZER_DESC rasterizer = {};
  D3D11_DEPTH_STENCIL_DESC depth_stencil = {};
  D3D11_BLEND_DESC blend = {};
  D3D11_VIEWPORT viewport = {};
  D3D11_RECT scissor = {};
  std::array<float, 4> blend_factor = {};
  uint32_t stencil_reference = 0;
  uint32_t sample_mask = UINT32_MAX;

  // Fully initialized native defaults; guest register state replaces these.
  static DrawState Default(uint32_t width, uint32_t height);
};

struct DrawCommand {
  std::array<const ShaderProgram*, size_t(DrawStage::kCount)> programs = {};
  std::array<StageBindings, size_t(DrawStage::kCount)> bindings = {};
  std::span<ID3D11RenderTargetView* const> render_targets;
  ID3D11DepthStencilView* depth_stencil = nullptr;
  // The shader adapter reserves u0-u3 for the four guest render targets.
  // These views start at u4, including for VS/DS memexport on FL 11_1.
  std::span<ID3D11UnorderedAccessView* const> unordered_access;
  DrawState state;
  D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
  std::shared_ptr<const BufferVersion> indices;
  uint32_t count = 0;
  uint32_t first = 0;
  int32_t base_vertex = 0;
  bool discard_rasterization = false;
};

// The subresources a view covers, for hazard checks.
struct DrawViewRange {
  static constexpr uint32_t kColor = 1;
  Microsoft::WRL::ComPtr<ID3D11Resource> resource;
  uint32_t mip = 0;
  uint32_t mips = 1;
  uint32_t layer = 0;
  uint32_t layers = 1;
  uint32_t aspects = kColor;
  uint32_t width = 1;
  uint32_t height = 1;
  uint32_t samples = 1;
  uint32_t quality = 0;
};

// Command-processor-owned native draw submission. Guest shaders pull vertex
// data from SRVs, so there is no host input layout or vertex stream. External
// compute or presentation work on the command processor thread must call
// Invalidate before changing immediate-context bindings; plain copies and
// uploads do not change them. Use of the context by another thread is detected
// through the device's context lock. The owning device outlives this object.
class DrawContext {
 public:
  struct Statistics {
    uint64_t draws = 0;
    uint64_t indexed_draws = 0;
    uint64_t rejected_draws = 0;
    uint64_t state_creations = 0;
    uint64_t binding_updates = 0;
    uint64_t explicit_resource_unbinds = 0;
    uint64_t output_changes = 0;
  };

  explicit DrawContext(ui::d3d11::D3D11Device& device) : device_(device) {}
  bool Draw(const DrawCommand& command, std::string& error);
  // Releases context references and invalidates all cached bindings. It does
  // not wait for queued GPU commands or discard cached immutable state objects.
  void Invalidate();
  void Clear();
  const Statistics& statistics() const { return statistics_; }

 private:
  template <typename Description, typename State>
  struct CachedState {
    Description description;
    Microsoft::WRL::ComPtr<State> state;
  };
  struct CachedBindings {
    std::array<Microsoft::WRL::ComPtr<ID3D11Buffer>,
               D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT>
        constants;
    std::array<Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>,
               D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT>
        resources;
    std::array<Microsoft::WRL::ComPtr<ID3D11SamplerState>, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT>
        samplers;
    // Slots at and above these counts are null.
    uint32_t constant_count = 0;
    uint32_t resource_count = 0;
    uint32_t sampler_count = 0;
  };
  struct CachedOutputs {
    std::array<Microsoft::WRL::ComPtr<ID3D11RenderTargetView>, 4> render_targets;
    uint32_t render_target_count = 0;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> depth_stencil;
    std::array<Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView>, D3D11_1_UAV_SLOT_COUNT>
        unordered_access;
    uint32_t unordered_access_count = 0;
    bool valid = false;
  };
  bool OutputsMatch(const DrawCommand& command) const;
  void BindStage(DrawStage stage, const StageBindings& bindings);
  void SetResources(DrawStage stage, uint32_t first, uint32_t count,
                    ID3D11ShaderResourceView* const* views);
  void ReleaseOutputs();
  ID3D11GeometryShader* DiscardShader(const ShaderProgram& source, std::string& error);
  struct CachedDiscardShader {
    std::vector<uint8_t> source;
    Microsoft::WRL::ComPtr<ID3D11GeometryShader> shader;
    std::string error;
  };

  ui::d3d11::D3D11Device& device_;
  bool initialized_ = false;
  uint64_t context_epoch_ = 0;
  std::array<CachedBindings, size_t(DrawStage::kCount)> bindings_;
  // Views are retained, so a cached pointer always names the same view.
  CachedOutputs outputs_;
  std::vector<CachedState<D3D11_RASTERIZER_DESC, ID3D11RasterizerState>> rasterizers_;
  std::vector<CachedState<D3D11_DEPTH_STENCIL_DESC, ID3D11DepthStencilState>> depths_;
  std::vector<CachedState<D3D11_BLEND_DESC, ID3D11BlendState>> blends_;
  std::vector<CachedDiscardShader> discard_shaders_;
  // Reused by every draw.
  std::vector<DrawViewRange> writes_;
  Statistics statistics_;
};

}  // namespace rex::graphics::d3d11
