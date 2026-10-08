#pragma once

#include <rex/graphics/command_processor.h>
#include <rex/graphics/d3d11/guest_draw.h>
#include <rex/graphics/d3d11/gpu_profiler.h>
#include <rex/ui/d3d11/d3d11_image_renderer.h>
#include <rex/ui/d3d11/d3d11_provider.h>

namespace rex::graphics::d3d11 {
class D3D11GraphicsSystem;
class D3D11CommandProcessor final : public CommandProcessor {
 public:
  D3D11CommandProcessor(D3D11GraphicsSystem* graphics, system::KernelState* kernel);
  void ClearCaches() override;
  void InvalidateGpuMemory() override;
  void IssueSwap(uint32_t pointer, uint32_t width, uint32_t height) override;
  void TracePlaybackWroteMemory(uint32_t address, uint32_t length) override;
  void RestoreEdramSnapshot(const void* snapshot) override;

 protected:
  bool SetupContext() override;
  void ShutdownContext() override;
  void WriteRegister(uint32_t index, uint32_t value) override;
  void WriteRegistersFromMem(uint32_t start_index, uint32_t* base, uint32_t num_registers) override;
  Shader* LoadShader(xenos::ShaderType type, uint32_t address, const uint32_t* words,
                     uint32_t count) override;
  bool IssueDraw(xenos::PrimitiveType type, uint32_t count, IndexBufferInfo* indices,
                 bool explicit_mode) override;
  bool IssueCopy() override;
  void OnPrimaryBufferEnd() override;

 private:
  bool Fail(std::string error);
  ui::d3d11::D3D11Device& Device() const;
  bool PrepareFxaaImage(uint32_t width, uint32_t height, std::string& error);
  std::unique_ptr<DrawContext> draws_;
  std::unique_ptr<D3D11SharedMemory> shared_memory_;
  std::unique_ptr<D3D11TextureCache> textures_;
  std::unique_ptr<D3D11RenderTargetCache> targets_;
  std::unique_ptr<D3D11PrimitiveProcessor> primitives_;
  std::unique_ptr<PipelineCache> pipelines_;
  std::unique_ptr<GuestDraw> guest_draw_;
  std::unique_ptr<ui::d3d11::D3D11ImageRenderer> output_;
  std::unique_ptr<BufferCache> gamma_buffers_;
  std::unique_ptr<GpuProfiler> gpu_profiler_;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> fxaa_image_;
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> fxaa_read_;
  Microsoft::WRL::ComPtr<ID3D11RenderTargetView> fxaa_write_;
  std::string failure_;
};
}  // namespace rex::graphics::d3d11
