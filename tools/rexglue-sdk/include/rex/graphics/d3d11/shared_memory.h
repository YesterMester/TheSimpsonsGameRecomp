#pragma once

#include <array>

#include <rex/graphics/d3d11/draw_context.h>
#include <rex/graphics/shared_memory.h>
#include <rex/graphics/trace_writer.h>

namespace rex::graphics::d3d11 {

// Physical-address buffer for texture conversion, memexport and remaining
// guest-address fetches. Native vertex/index snapshots use BufferCache instead.
// CPU page validity and GPU-written ownership remain in the common tracker.
class D3D11SharedMemory final : public SharedMemory {
 public:
  D3D11SharedMemory(ui::d3d11::D3D11Device& device, DrawContext& draw_context,
                    memory::Memory& memory, TraceWriter& trace_writer)
      : SharedMemory(memory),
        device_(device),
        draw_context_(draw_context),
        trace_writer_(trace_writer) {}
  ~D3D11SharedMemory() override;
  bool Initialize(std::string& error);
  void Shutdown();
  ID3D11Buffer* buffer() const { return buffer_.Get(); }
  ID3D11ShaderResourceView* raw_srv() const { return srvs_[0].Get(); }
  ID3D11UnorderedAccessView* raw_uav() const { return uavs_[0].Get(); }
  ID3D11ShaderResourceView* typed_srv(uint32_t element_size_log2) const;
  ID3D11UnorderedAccessView* typed_uav(uint32_t element_size_log2) const;
  bool InitializeTraceSubmitDownloads();
  bool InitializeTraceCompleteDownloads();
  bool ReadbackCpuRange(uint32_t address, uint32_t length, std::string& error);

 protected:
  bool UploadRanges(const std::vector<std::pair<uint32_t, uint32_t>>& page_ranges) override;

 private:
  void ResetTraceDownload();
  ui::d3d11::D3D11Device& device_;
  DrawContext& draw_context_;
  TraceWriter& trace_writer_;
  bool common_initialized_ = false;
  Microsoft::WRL::ComPtr<ID3D11Buffer> buffer_;
  std::array<Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>, 4> srvs_;
  std::array<Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView>, 4> uavs_;
  std::vector<uint8_t> upload_snapshot_;
  Microsoft::WRL::ComPtr<ID3D11Buffer> trace_download_;
};

}  // namespace rex::graphics::d3d11
