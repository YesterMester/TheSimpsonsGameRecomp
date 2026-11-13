#include <rex/graphics/d3d11/buffer_cache.h>

#include <rex/graphics/d3d11/profile.h>
#include <rex/graphics/util/bytes_equal.h>
#include <rex/hash.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>

namespace rex::graphics::d3d11 {
namespace {
bool ValidBufferSize(uint64_t size, BufferKind kind) {
  return size && size <= (128u << 20) && (kind != BufferKind::kConstant || size <= 65536) &&
         (kind != BufferKind::kIndex16 || !(size & 1)) &&
         (kind != BufferKind::kIndex32 || !(size & 3)) &&
         uint32_t(kind) <= uint32_t(BufferKind::kConstant);
}
constexpr std::array<uint8_t, 1024> kZeroPadding = {};
void HashPadding(XXH3_state_t& state, uint32_t length) {
  while (length) {
    uint32_t count = std::min(length, uint32_t(kZeroPadding.size()));
    XXH3_64bits_update(&state, kZeroPadding.data(), count);
    length -= count;
  }
}
bool ZeroPadding(const uint8_t* data, uint32_t length) {
  while (length) {
    uint32_t count = std::min(length, uint32_t(kZeroPadding.size()));
    if (!draw_util::BytesEqual(data, kZeroPadding.data(), count))
      return false;
    data += count;
    length -= count;
  }
  return true;
}
}  // namespace

std::shared_ptr<const BufferVersion> BufferCache::GetOrCreate(std::span<const uint8_t> source,
                                                              BufferKind kind, std::string& error) {
  error.clear();
  if (!ValidBufferSize(source.size(), kind)) {
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
  return Create(std::move(version), hash, error);
}

std::shared_ptr<const BufferVersion> BufferCache::GetOrCreate(std::span<const CpuRange> ranges,
                                                              uint32_t size, BufferKind kind,
                                                              std::string& error) {
  error.clear();
  if (!ValidBufferSize(size, kind)) {
    error = "Invalid native buffer size or kind";
    return {};
  }
  uint32_t end = 0;
  for (const auto& range : ranges) {
    if (!range.data || !range.length || range.offset < end || range.offset > size ||
        range.length > size - range.offset) {
      error = "Invalid native CPU buffer ranges";
      return {};
    }
    end = range.offset + range.length;
  }
  if (ranges.size() == 1 && ranges.front().offset == 0 && ranges.front().length == size)
    return GetOrCreate(std::span<const uint8_t>(ranges.front().data, size), kind, error);

  XXH3_state_t state;
  XXH3_INITSTATE(&state);
  XXH3_64bits_reset_withSeed(&state, uint64_t(kind));
  end = 0;
  for (const auto& range : ranges) {
    HashPadding(state, range.offset - end);
    XXH3_64bits_update(&state, range.data, range.length);
    end = range.offset + range.length;
  }
  HashPadding(state, size - end);
  uint64_t hash = XXH3_64bits_digest(&state);
  auto [begin, last] = lookup_.equal_range(hash);
  for (auto it = begin; it != last; ++it) {
    auto entry = it->second;
    const auto& version = *entry->version;
    if (version.kind_ != kind || version.source_.size() != size)
      continue;
    bool equal = true;
    end = 0;
    for (const auto& range : ranges) {
      if (!ZeroPadding(version.source_.data() + end, range.offset - end) ||
          !draw_util::BytesEqual(version.source_.data() + range.offset, range.data, range.length)) {
        equal = false;
        break;
      }
      end = range.offset + range.length;
    }
    if (!equal || !ZeroPadding(version.source_.data() + end, size - end))
      continue;
    auto result = entry->version;
    entries_.splice(entries_.begin(), entries_, entry);
    ++statistics_.hits;
    g_profile.buffer_hits.fetch_add(1, std::memory_order_relaxed);
    return result;
  }

  auto version = std::shared_ptr<BufferVersion>(new BufferVersion());
  version->kind_ = kind;
  version->source_.reserve(size);
  for (const auto& range : ranges) {
    // Only padding needs initialization before appending the real input.
    version->source_.resize(range.offset, 0);
    version->source_.insert(version->source_.end(), range.data, range.data + range.length);
  }
  version->source_.resize(size, 0);
  return Create(std::move(version), hash, error);
}

std::shared_ptr<const BufferVersion> BufferCache::Create(std::shared_ptr<BufferVersion> version,
                                                         uint64_t hash, std::string& error) {
  auto kind = version->kind_;
  uint32_t alignment = kind == BufferKind::kConstant ? 16 : 4;
  version->allocation_size_ =
      (uint32_t(version->source_.size()) + alignment - 1) & ~(alignment - 1);
  std::vector<uint8_t> padded;
  const uint8_t* initial = version->source_.data();
  if (version->allocation_size_ != version->source_.size()) {
    padded.resize(version->allocation_size_, 0);
    std::memcpy(padded.data(), initial, version->source_.size());
    initial = padded.data();
  }
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
  D3D11_SUBRESOURCE_DATA data = {initial, 0, 0};
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
  uint64_t cost = uint64_t(version->source_.size()) + desc.ByteWidth;
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

std::shared_ptr<const BufferVersion> BufferCache::SnapshotMixedRanges(
    std::span<const uint8_t> cpu_bytes, std::span<const GpuCopy> copies, std::string& error) {
  error.clear();
  if (cpu_bytes.empty() || cpu_bytes.size() > 128 * 1024 * 1024 || (cpu_bytes.size() & 3)) {
    error = "Invalid mixed native buffer snapshot size";
    return {};
  }
  for (const auto& copy : copies) {
    D3D11_BUFFER_DESC source = {};
    if (copy.source)
      copy.source->GetDesc(&source);
    if (!copy.source || !copy.length ||
        ((copy.source_offset | copy.destination_offset | copy.length) & 3) ||
        copy.source_offset > source.ByteWidth ||
        copy.length > source.ByteWidth - copy.source_offset ||
        copy.destination_offset > cpu_bytes.size() ||
        copy.length > cpu_bytes.size() - copy.destination_offset) {
      error = "Mixed native buffer snapshot exceeds a source or destination";
      return {};
    }
  }
  auto version = std::shared_ptr<BufferVersion>(new BufferVersion());
  version->kind_ = BufferKind::kRaw;
  version->allocation_size_ = version->gpu_source_size_ = uint32_t(cpu_bytes.size());
  D3D11_BUFFER_DESC desc = {};
  desc.ByteWidth = version->allocation_size_;
  desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
  D3D11_SUBRESOURCE_DATA initial = {cpu_bytes.data(), 0, 0};
  HRESULT result = device_.device()->CreateBuffer(&desc, &initial, version->buffer_.GetAddressOf());
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
    error = "Unable to allocate a mixed native buffer snapshot";
    return {};
  }
  for (const auto& copy : copies) {
    D3D11_BOX box = {copy.source_offset, 0, 0, copy.source_offset + copy.length, 1, 1};
    device_.context()->CopySubresourceRegion(version->buffer_.Get(), 0, copy.destination_offset, 0,
                                             0, copy.source, 0, &box);
  }
  if (FAILED(device_.device()->GetDeviceRemovedReason())) {
    error = "The Direct3D 11 device was lost while assembling native streams";
    return {};
  }
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
