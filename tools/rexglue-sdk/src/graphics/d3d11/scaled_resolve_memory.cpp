#include <rex/graphics/d3d11/scaled_resolve_memory.h>

#include <algorithm>
#include <cstdio>

namespace rex::graphics::d3d11 {
namespace {
constexpr uint32_t kPhysicalSize = 512u << 20;
constexpr uint32_t kMaximumRegionBytes = 512u << 20;
constexpr uint32_t kPageSize = 4096;
constexpr DXGI_FORMAT kFormats[] = {DXGI_FORMAT_R32_UINT, DXGI_FORMAT_R32G32_UINT,
                                    DXGI_FORMAT_R32G32B32A32_UINT};
bool Check(HRESULT result, const char* operation, std::string& error) {
  if (SUCCEEDED(result))
    return true;
  char message[160];
  std::snprintf(message, sizeof(message), "%s failed (0x%08X)", operation, unsigned(result));
  error = message;
  return false;
}
}  // namespace

bool ScaledResolveMemory::Ensure(uint32_t start, uint32_t length, std::string& error) {
  error.clear();
  if (!area_ || area_ > 9 || !length || start >= kPhysicalSize || length > kPhysicalSize - start) {
    error = "Invalid native scaled resolve range";
    return false;
  }
  uint32_t first = start & ~(kPageSize - 1);
  uint32_t end = (start + length + kPageSize - 1) & ~(kPageSize - 1);
  auto begin = regions_.upper_bound(first);
  if (begin != regions_.begin()) {
    auto previous = std::prev(begin);
    if (previous->second.end >= end)
      return true;
    if (previous->second.end > first)
      begin = previous;
  }
  auto after = begin;
  // Merge intersecting allocations, including ones reached by extending the
  // range. Neighbors remain independent, avoiding a growing allocation when
  // separate render textures merely happen to have consecutive addresses.
  while (after != regions_.end() && after->first < end) {
    first = std::min(first, after->first);
    end = std::max(end, after->second.end);
    ++after;
  }
  uint64_t bytes = uint64_t(end - first) * area_;
  if (bytes > kMaximumRegionBytes) {
    error = "Native scaled resolve range exceeds the device buffer view limit";
    return false;
  }
  D3D11_BUFFER_DESC desc = {};
  desc.ByteWidth = uint32_t(bytes);
  desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
  desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
  Region next{};
  next.end = end;
  if (!Check(device_.device()->CreateBuffer(&desc, nullptr, next.buffer.GetAddressOf()),
             "Native scaled resolve storage", error))
    return false;
  D3D11_UNORDERED_ACCESS_VIEW_DESC view = {};
  view.Format = DXGI_FORMAT_R32_TYPELESS;
  view.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
  view.Buffer.NumElements = uint32_t(bytes) / 4;
  view.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
  Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> zero_view;
  if (!Check(device_.device()->CreateUnorderedAccessView(next.buffer.Get(), &view,
                                                         zero_view.GetAddressOf()),
             "Native scaled resolve initialization view", error))
    return false;
  draws_.Invalidate();
  const uint32_t zero[4] = {};
  device_.context()->ClearUnorderedAccessViewUint(zero_view.Get(), zero);
  uint64_t replaced_bytes = 0;
  for (auto i = begin; i != after; ++i) {
    uint32_t length_scaled = (i->second.end - i->first) * area_;
    D3D11_BOX box = {0, 0, 0, length_scaled, 1, 1};
    device_.context()->CopySubresourceRegion(next.buffer.Get(), 0, (i->first - first) * area_, 0, 0,
                                             i->second.buffer.Get(), 0, &box);
    replaced_bytes += length_scaled;
  }
  if (!Check(device_.device()->GetDeviceRemovedReason(), "Native scaled resolve preservation",
             error))
    return false;
  regions_.erase(begin, after);
  regions_.emplace(first, std::move(next));
  resident_bytes_ += bytes - replaced_bytes;
  return true;
}

ScaledResolveMemory::Region* ScaledResolveMemory::Find(uint32_t start, uint32_t length,
                                                       uint32_t& first_byte, std::string& error) {
  if (!Ensure(start, length, error))
    return nullptr;
  auto i = std::prev(regions_.upper_bound(start));
  first_byte = (start - i->first) * area_;
  return &i->second;
}

Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> ScaledResolveMemory::Read(
    uint32_t start, uint32_t length, uint32_t element_size_log2, std::string& error) {
  error.clear();
  if (element_size_log2 < 2 || element_size_log2 > 4 ||
      ((uint64_t(start) * area_) & ((1u << element_size_log2) - 1))) {
    error = "Invalid native scaled resolve read alignment or element size";
    return {};
  }
  uint32_t first_byte;
  auto* region = Find(start, length, first_byte, error);
  if (!region)
    return {};
  D3D11_SHADER_RESOURCE_VIEW_DESC desc = {};
  desc.Format = kFormats[element_size_log2 - 2];
  desc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
  desc.Buffer.FirstElement = first_byte >> element_size_log2;
  desc.Buffer.NumElements = (region->end - start) * area_ >> element_size_log2;
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view;
  if (!Check(device_.device()->CreateShaderResourceView(region->buffer.Get(), &desc,
                                                        view.GetAddressOf()),
             "Native scaled resolve read view", error))
    return {};
  return view;
}

Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> ScaledResolveMemory::Write(
    uint32_t start, uint32_t length, uint32_t element_size_log2, std::string& error) {
  error.clear();
  if (element_size_log2 < 2 || element_size_log2 > 4 ||
      ((uint64_t(start) * area_) & ((1u << element_size_log2) - 1))) {
    error = "Invalid native scaled resolve write alignment or element size";
    return {};
  }
  uint32_t first_byte;
  auto* region = Find(start, length, first_byte, error);
  if (!region)
    return {};
  D3D11_UNORDERED_ACCESS_VIEW_DESC desc = {};
  desc.Format = kFormats[element_size_log2 - 2];
  desc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
  desc.Buffer.FirstElement = first_byte >> element_size_log2;
  desc.Buffer.NumElements = (region->end - start) * area_ >> element_size_log2;
  Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> view;
  if (!Check(device_.device()->CreateUnorderedAccessView(region->buffer.Get(), &desc,
                                                         view.GetAddressOf()),
             "Native scaled resolve write view", error))
    return {};
  return view;
}

}  // namespace rex::graphics::d3d11
