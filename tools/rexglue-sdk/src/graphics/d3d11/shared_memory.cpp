#include <rex/graphics/d3d11/shared_memory.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include <rex/graphics/d3d11/profile.h>
#include <rex/logging.h>

namespace rex::graphics::d3d11 {

D3D11SharedMemory::~D3D11SharedMemory() {
  // Common watches are released by the base destructor after texture and
  // primitive caches have unregistered their callbacks.
  ResetTraceDownload();
}

bool D3D11SharedMemory::Initialize(std::string& error) {
  error.clear();
  if (buffer_ || common_initialized_) {
    error = "DX11 shared memory is already initialized";
    return false;
  }
  D3D11_BUFFER_DESC desc = {};
  desc.ByteWidth = kBufferSize;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags =
      D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_INDEX_BUFFER;
  desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
  auto* device = device_.device();
  HRESULT result = device->CreateBuffer(&desc, nullptr, buffer_.GetAddressOf());
  static constexpr DXGI_FORMAT formats[] = {DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_R32_UINT,
                                            DXGI_FORMAT_R32G32_UINT, DXGI_FORMAT_R32G32B32A32_UINT};
  for (uint32_t i = 0; i < 4 && SUCCEEDED(result); ++i) {
    D3D11_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.Format = formats[i];
    srv.ViewDimension = i ? D3D11_SRV_DIMENSION_BUFFER : D3D11_SRV_DIMENSION_BUFFEREX;
    uint32_t element_size = i ? 1u << (i + 1) : 4;
    srv.BufferEx.NumElements = kBufferSize / element_size;
    srv.BufferEx.Flags = i ? 0 : D3D11_BUFFEREX_SRV_FLAG_RAW;
    result = device->CreateShaderResourceView(buffer_.Get(), &srv, srvs_[i].GetAddressOf());
    if (FAILED(result))
      break;
    D3D11_UNORDERED_ACCESS_VIEW_DESC uav = {};
    uav.Format = formats[i];
    uav.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    uav.Buffer.NumElements = kBufferSize / element_size;
    uav.Buffer.Flags = i ? 0 : D3D11_BUFFER_UAV_FLAG_RAW;
    result = device->CreateUnorderedAccessView(buffer_.Get(), &uav, uavs_[i].GetAddressOf());
  }
  if (FAILED(result)) {
    char message[128];
    std::snprintf(message, sizeof(message), "DX11 physical-address buffer creation failed (0x%08X)",
                  unsigned(result));
    error = message;
    srvs_ = {};
    uavs_ = {};
    buffer_.Reset();
    return false;
  }
  draw_context_.Invalidate();
  const uint32_t zero[4] = {};
  device_.context()->ClearUnorderedAccessViewUint(uavs_[0].Get(), zero);
  InitializeCommon();
  common_initialized_ = true;
  streamed_page_shadows_supported_ = true;
  upload_snapshot_.resize(std::max(4u * 1024 * 1024, 1u << page_size_log2()));
  return true;
}

void D3D11SharedMemory::Shutdown() {
  draw_context_.Invalidate();
  ResetTraceDownload();
  srvs_ = {};
  uavs_ = {};
  buffer_.Reset();
  upload_snapshot_.clear();
  if (common_initialized_) {
    ShutdownCommon();
    common_initialized_ = false;
  }
}

ID3D11ShaderResourceView* D3D11SharedMemory::typed_srv(uint32_t element_size_log2) const {
  return element_size_log2 >= 2 && element_size_log2 <= 4 ? srvs_[element_size_log2 - 1].Get()
                                                          : nullptr;
}
ID3D11UnorderedAccessView* D3D11SharedMemory::typed_uav(uint32_t element_size_log2) const {
  return element_size_log2 >= 2 && element_size_log2 <= 4 ? uavs_[element_size_log2 - 1].Get()
                                                          : nullptr;
}

bool D3D11SharedMemory::UploadRanges(
    const std::vector<std::pair<uint32_t, uint32_t>>& page_ranges) {
  if (page_ranges.empty())
    return true;
  if (!buffer_ || upload_snapshot_.empty())
    return false;
  // Updates are ordered with the bound draws and do not change any binding.
  for (auto [first, count] : page_ranges) {
    trace_writer_.WriteMemoryRead(first << page_size_log2(), count << page_size_log2());
    while (count) {
      uint32_t pages = std::min(count, uint32_t(upload_snapshot_.size() >> page_size_log2()));
      uint32_t address = first << page_size_log2(), length = pages << page_size_log2();
      // Protect before copying, so a concurrent CPU write invalidates this
      // upload rather than falsely leaving the page marked current.
      MakeRangeValid(address, length, false);
      CopyPagesForUpload(first, pages, upload_snapshot_.data());
      D3D11_BOX box = {address, 0, 0, address + length, 1, 1};
      device_.context()->UpdateSubresource(buffer_.Get(), 0, &box, upload_snapshot_.data(), 0, 0);
      g_profile.memory_uploads.fetch_add(1, std::memory_order_relaxed);
      g_profile.memory_upload_bytes.fetch_add(length, std::memory_order_relaxed);
      first += pages;
      count -= pages;
    }
  }
  return SUCCEEDED(device_.device()->GetDeviceRemovedReason());
}

bool D3D11SharedMemory::InitializeTraceSubmitDownloads() {
  ResetTraceDownload();
  PrepareForTraceDownload();
  uint32_t pages = trace_download_page_count();
  if (!pages)
    return false;
  D3D11_BUFFER_DESC desc = {};
  desc.ByteWidth = pages << page_size_log2();
  desc.Usage = D3D11_USAGE_STAGING;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  HRESULT result = device_.device()->CreateBuffer(&desc, nullptr, trace_download_.GetAddressOf());
  if (FAILED(result)) {
    REXGPU_ERROR("DX11 shared memory: Failed to create GPU-written trace readback storage");
    ResetTraceDownload();
    return false;
  }
  draw_context_.Invalidate();
  uint32_t offset = 0;
  for (auto [address, length] : trace_download_ranges()) {
    D3D11_BOX box = {address, 0, 0, address + length, 1, 1};
    device_.context()->CopySubresourceRegion(trace_download_.Get(), 0, offset, 0, 0, buffer_.Get(),
                                             0, &box);
    offset += length;
  }
  return true;
}

bool D3D11SharedMemory::InitializeTraceCompleteDownloads() {
  if (!trace_download_)
    return false;
  HRESULT result = device_.WaitForCompletion();
  D3D11_MAPPED_SUBRESOURCE mapping = {};
  if (SUCCEEDED(result))
    result = device_.context()->Map(trace_download_.Get(), 0, D3D11_MAP_READ, 0, &mapping);
  if (FAILED(result)) {
    REXGPU_ERROR("DX11 shared memory: Failed to read GPU-written trace bytes");
    ResetTraceDownload();
    return false;
  }
  uint32_t offset = 0;
  for (auto [address, length] : trace_download_ranges()) {
    trace_writer_.WriteMemoryRead(address, length,
                                  static_cast<const uint8_t*>(mapping.pData) + offset);
    offset += length;
  }
  device_.context()->Unmap(trace_download_.Get(), 0);
  ResetTraceDownload();
  return true;
}

bool D3D11SharedMemory::ReadbackCpuRange(uint32_t address, uint32_t length, std::string& error) {
  error.clear();
  if (!length)
    return true;
  if (!buffer_ || address >= kBufferSize || length > kBufferSize - address) {
    error = "Invalid native CPU readback range";
    return false;
  }
  // A CPU write invalidates an entire tracked page. Publish its preserved
  // neighbors too, so subsequent partial writes cannot discard GPU data.
  uint32_t first = address & ~((1u << page_size_log2()) - 1);
  uint32_t end =
      (address + length + (1u << page_size_log2()) - 1) & ~((1u << page_size_log2()) - 1);
  D3D11_BUFFER_DESC desc = {};
  desc.ByteWidth = end - first;
  desc.Usage = D3D11_USAGE_STAGING;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  Microsoft::WRL::ComPtr<ID3D11Buffer> staging;
  HRESULT result = device_.device()->CreateBuffer(&desc, nullptr, staging.GetAddressOf());
  if (SUCCEEDED(result)) {
    draw_context_.Invalidate();
    D3D11_BOX box = {first, 0, 0, end, 1, 1};
    device_.context()->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, buffer_.Get(), 0, &box);
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    result = device_.context()->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (SUCCEEDED(result)) {
      std::memcpy(memory().TranslatePhysical(first), mapped.pData, end - first);
      device_.context()->Unmap(staging.Get(), 0);
      // Readback is an exact publication of the same GPU version. Re-arm
      // ownership and watches after the physical-memory write fault callback.
      RangeWrittenByGpu(first, end - first);
    }
  }
  if (FAILED(result)) {
    error = "Native CPU readback failed";
    return false;
  }
  return true;
}

void D3D11SharedMemory::ResetTraceDownload() {
  trace_download_.Reset();
  ReleaseTraceDownloadRanges();
}

}  // namespace rex::graphics::d3d11
