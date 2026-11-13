#pragma once

#include <array>
#include <mutex>
#include <unordered_map>

#include <rex/graphics/d3d11/draw_context.h>
#include <rex/graphics/native_gpu_diagnostics.h>
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
  void ClearCache() override;
  ID3D11Buffer* buffer() const { return buffer_.Get(); }
  ID3D11ShaderResourceView* raw_srv() const { return srvs_[0].Get(); }
  ID3D11UnorderedAccessView* raw_uav() const { return uavs_[0].Get(); }
  ID3D11ShaderResourceView* typed_srv(uint32_t element_size_log2) const;
  ID3D11UnorderedAccessView* typed_uav(uint32_t element_size_log2) const;
  bool InitializeTraceSubmitDownloads();
  bool InitializeTraceCompleteDownloads();
  bool ReadbackCpuRange(uint32_t address, uint32_t length, std::string& error);
  // Native uploads capture CPU-owned bytes without declaring the guest-address
  // buffer current. GPU-owned ranges must keep their authoritative GPU source.
  bool CopyCpuRange(uint32_t address, std::span<uint8_t> destination);
  // Borrowed CPU inputs for direct immutable uploads and content checks.
  // GPU-owned ranges have no CPU view; tracing uses the selected snapshot.
  std::span<const uint8_t> BorrowCpuRange(uint32_t address, uint32_t length);
  void TraceCpuRangeRead(uint32_t address, std::span<const uint8_t> snapshot);
  // Original-resolution resolve data stays in resource-local buffers until a
  // compatibility consumer requests its pages. Padding is captured before the
  // resolve; CPU writes invalidate only the pages they actually touch.
  struct NativeResolveBuffer {
    uint32_t address = 0, length = 0;
    Microsoft::WRL::ComPtr<ID3D11Buffer> buffer;
    std::array<Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView>, 3> writes;
    std::unordered_map<uint32_t, Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>> reads;
    // Protected by native_resolve_mutex_, including in the memory callback.
    std::vector<uint8_t> valid_pages, materialized_pages;
  };
  using NativeResolveHandle = std::shared_ptr<NativeResolveBuffer>;
  bool CanRetainNativeResolve(uint32_t address, uint32_t length) const;
  NativeResolveHandle PrepareNativeResolve(uint32_t address, uint32_t length,
                                           uint32_t element_size_log2, std::string& error);
  void BeginNativeResolveWrite(const NativeResolveHandle& buffer);
  bool EndNativeResolveWrite();
  bool HasNativeResolveRange(uint32_t address, uint32_t length) const;
  // Several retained GPU owners and CPU-only gaps can also supply a temporary
  // texture input. Its assembly does not publish a new GPU-written version.
  bool CanReadNativeResolveRange(uint32_t address, uint32_t length);
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> ReadNativeResolveRange(
      uint32_t address, uint32_t length, uint32_t element_size_log2, std::string& error);
  void SetNativeDiagnostics(NativeGpuDiagnostics* diagnostics) { diagnostics_ = diagnostics; }
  NativeGpuDiagnostics* native_diagnostics() const { return diagnostics_; }

 protected:
  bool DownloadNativeGpuIndices(uint32_t start, std::span<uint8_t> bytes) override;
  bool EnsureHostBuffer() override;
  bool FlushGpuWrittenRange(uint32_t start, uint32_t length, bool writing = false) override;
  bool UploadRanges(const std::vector<std::pair<uint32_t, uint32_t>>& page_ranges) override;

 private:
  void ResetTraceDownload();
  NativeResolveHandle FindNativeResolveRange(uint32_t address, uint32_t length) const;
  bool PopulateNativeResolve(const NativeResolveHandle& buffer, std::string& error);
  void CompactNativeResolves(uint64_t target_bytes);
  static void NativeResolveInvalidation(const std::unique_lock<std::recursive_mutex>& global_lock,
                                        void* context, uint32_t first, uint32_t last, bool gpu);
  ui::d3d11::D3D11Device& device_;
  DrawContext& draw_context_;
  TraceWriter& trace_writer_;
  bool common_initialized_ = false;
  NativeGpuDiagnostics* diagnostics_ = nullptr;
  Microsoft::WRL::ComPtr<ID3D11Buffer> buffer_;
  std::array<Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>, 4> srvs_;
  std::array<Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView>, 4> uavs_;
  std::vector<uint8_t> upload_snapshot_;
  Microsoft::WRL::ComPtr<ID3D11Buffer> trace_download_;
  GlobalWatchHandle native_resolve_watch_ = nullptr;
  mutable std::mutex native_resolve_mutex_;
  std::vector<NativeResolveHandle> native_resolve_buffers_;
  NativeResolveHandle native_resolve_pending_;
  bool native_resolve_clearing_cache_ = false;
  uint64_t native_resolve_bytes_ = 0;
  uint64_t native_resolve_writes_ = 0;
  uint64_t native_resolve_preimages_ = 0;
  uint64_t native_resolve_compactions_ = 0;
};

}  // namespace rex::graphics::d3d11
