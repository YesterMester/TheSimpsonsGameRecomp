#include <rex/graphics/d3d11/buffer_cache.h>

#include <rex/graphics/d3d11/profile.h>
#include <rex/graphics/util/bytes_equal.h>
#include <rex/hash.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace rex::graphics::d3d11 {

std::shared_ptr<const BufferVersion> BufferCache::GetOrCreate(std::span<const uint8_t> source,
                                                              BufferKind kind, std::string& error) {
  error.clear();
  if (source.empty() || source.size() > 128 * 1024 * 1024 ||
      (kind == BufferKind::kConstant && source.size() > 65536) ||
      (kind == BufferKind::kIndex16 && source.size() % 2) ||
      (kind == BufferKind::kIndex32 && source.size() % 4) ||
      uint32_t(kind) > uint32_t(BufferKind::kConstant)) {
    error = "Invalid native buffer size or kind";
    return nullptr;
  }
  uint64_t hash = XXH3_64bits_withSeed(source.data(), source.size(), uint64_t(kind));
  auto [begin, end] = lookup_.equal_range(hash);
  for (auto it = begin; it != end; ++it) {
    auto entry = it->second;
    const auto& version = *entry->version;
    if (version.kind_ == kind && version.source_.size() == source.size() &&
        draw_util::BytesEqual(version.source_.data(), source.data(), source.size())) {
      auto result = entry->version;
      entries_.splice(entries_.begin(), entries_, entry);
      ++statistics_.hits;
      g_profile.buffer_hits.fetch_add(1, std::memory_order_relaxed);
      return result;
    }
  }

  auto version = std::shared_ptr<BufferVersion>(new BufferVersion());
  version->kind_ = kind;
  version->source_.assign(source.begin(), source.end());
  uint32_t alignment = kind == BufferKind::kConstant ? 16 : 4;
  version->allocation_size_ = (uint32_t(source.size()) + alignment - 1) & ~(alignment - 1);
  std::vector<uint8_t> padded(version->allocation_size_, 0);
  std::memcpy(padded.data(), version->source_.data(), version->source_.size());
  D3D11_BUFFER_DESC desc = {};
  desc.ByteWidth = version->allocation_size_;
  desc.Usage = D3D11_USAGE_IMMUTABLE;
  if (kind == BufferKind::kConstant) {
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  } else {
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (kind == BufferKind::kIndex16 || kind == BufferKind::kIndex32) {
      desc.BindFlags |= D3D11_BIND_INDEX_BUFFER;
    }
    desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
  }
  D3D11_SUBRESOURCE_DATA data = {padded.data(), 0, 0};
  HRESULT result = device_.device()->CreateBuffer(&desc, &data, version->buffer_.GetAddressOf());
  if (SUCCEEDED(result) && kind != BufferKind::kConstant) {
    D3D11_SHADER_RESOURCE_VIEW_DESC view = {};
    view.Format = DXGI_FORMAT_R32_TYPELESS;
    view.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
    view.BufferEx.NumElements = desc.ByteWidth / 4;
    view.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
    result = device_.device()->CreateShaderResourceView(version->buffer_.Get(), &view,
                                                        version->raw_view_.GetAddressOf());
  }
  if (FAILED(result)) {
    char message[128];
    std::snprintf(message, sizeof(message), "Native buffer creation failed (0x%08X)",
                  unsigned(result));
    error = message;
    return nullptr;
  }
  ++statistics_.creations;
  statistics_.uploaded_bytes += desc.ByteWidth;
  g_profile.buffer_creations.fetch_add(1, std::memory_order_relaxed);
  g_profile.buffer_creation_bytes.fetch_add(desc.ByteWidth, std::memory_order_relaxed);
  uint64_t cost = uint64_t(source.size()) + desc.ByteWidth;
  statistics_.retained_bytes += cost;
  entries_.push_front({hash, cost, version});
  lookup_.emplace(hash, entries_.begin());
  Trim();
  return version;
}

std::shared_ptr<const BufferVersion> BufferCache::SnapshotGpuRange(ID3D11Buffer* source,
                                                                   uint32_t offset, uint32_t length,
                                                                   BufferKind kind,
                                                                   std::string& error) {
  error.clear();
  if (!source || !length || length > 128 * 1024 * 1024 || kind == BufferKind::kConstant ||
      uint32_t(kind) > uint32_t(BufferKind::kConstant) ||
      (kind == BufferKind::kIndex16 && ((offset | length) & 1)) ||
      (kind == BufferKind::kIndex32 && ((offset | length) & 3))) {
    error = "Invalid native GPU buffer snapshot range or kind";
    return nullptr;
  }
  D3D11_BUFFER_DESC source_desc = {};
  source->GetDesc(&source_desc);
  if (offset > source_desc.ByteWidth || length > source_desc.ByteWidth - offset) {
    error = "Native GPU buffer snapshot exceeds the source";
    return nullptr;
  }
  auto version = std::shared_ptr<BufferVersion>(new BufferVersion());
  version->kind_ = kind;
  version->gpu_source_size_ = length;
  version->allocation_size_ = (length + 3) & ~3u;
  D3D11_BUFFER_DESC desc = {};
  desc.ByteWidth = version->allocation_size_;
  desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  if (kind == BufferKind::kIndex16 || kind == BufferKind::kIndex32)
    desc.BindFlags |= D3D11_BIND_INDEX_BUFFER;
  desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
  HRESULT result = device_.device()->CreateBuffer(&desc, nullptr, version->buffer_.GetAddressOf());
  if (SUCCEEDED(result)) {
    D3D11_SHADER_RESOURCE_VIEW_DESC view = {};
    view.Format = DXGI_FORMAT_R32_TYPELESS;
    view.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
    view.BufferEx.NumElements = desc.ByteWidth / 4;
    view.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
    result = device_.device()->CreateShaderResourceView(version->buffer_.Get(), &view,
                                                        version->raw_view_.GetAddressOf());
  }
  if (FAILED(result)) {
    error = "Unable to create a retained native GPU buffer snapshot";
    return nullptr;
  }
  D3D11_BOX range = {offset, 0, 0, offset + length, 1, 1};
  device_.context()->CopySubresourceRegion(version->buffer_.Get(), 0, 0, 0, 0, source, 0, &range);
  if (FAILED(device_.device()->GetDeviceRemovedReason())) {
    error = "Direct3D device was lost during a native buffer snapshot";
    return nullptr;
  }
  // This snapshot is owned by the caller. Its GPU storage is never placed in
  // the CPU byte-comparison cache or reused for another guest write.
  return version;
}

void BufferCache::Trim() {
  while (!entries_.empty() && statistics_.retained_bytes > budget_) {
    auto last = std::prev(entries_.end());
    auto [begin, end] = lookup_.equal_range(last->hash);
    for (auto it = begin; it != end; ++it) {
      if (it->second == last) {
        lookup_.erase(it);
        break;
      }
    }
    statistics_.retained_bytes -= last->cost;
    ++statistics_.evictions;
    entries_.erase(last);
  }
}

void BufferCache::Clear() {
  lookup_.clear();
  entries_.clear();
  statistics_ = {};
}

}  // namespace rex::graphics::d3d11
