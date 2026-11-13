#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <vector>

#include <rex/graphics/util/bytes_equal.h>
#include <rex/ui/d3d11/d3d11_device.h>

namespace rex::graphics::d3d11 {

// Queue reads before changing this buffer again. Discard preserves queued GPU
// versions; append maps only touch bytes not used since the previous discard.
class ConstantBuffer {
 public:
  enum class Update { kFailed, kUnchanged, kDiscard, kAppend };

  static bool SupportsRanges(ui::d3d11::D3D11Device& device) {
    D3D11_FEATURE_DATA_D3D11_OPTIONS options = {};
    Microsoft::WRL::ComPtr<ID3D11DeviceContext1> context;
    return device.features().constant_buffer_ranges &&
           SUCCEEDED(device.device()->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &options,
                                                          sizeof(options))) &&
           options.MapNoOverwriteOnDynamicConstantBuffer &&
           SUCCEEDED(device.context()->QueryInterface(IID_PPV_ARGS(context.GetAddressOf())));
  }

  Update Upload(ui::d3d11::D3D11Device& device, std::span<const uint8_t> data,
                uint32_t range_capacity, std::string& error) {
    error.clear();
    if (data.empty() || data.size() > D3D11_REQ_CONSTANT_BUFFER_ELEMENT_COUNT * 16) {
      error = "Invalid native constant buffer size";
      return Update::kFailed;
    }
    uint32_t size = (uint32_t(data.size()) + 15) & ~uint32_t(15);
    if (buffer_ && size != size_) {
      error = "A native constant buffer changed its allocation size";
      return Update::kFailed;
    }
    if (buffer_ && contents_.size() == data.size() &&
        draw_util::BytesEqual(contents_.data(), data.data(), data.size()))
      return Update::kUnchanged;

    uint32_t bound_size = (size + 255) & ~uint32_t(255);
    if (!buffer_) {
      size_ = size;
      if (range_capacity && !(range_capacity & 255) && range_capacity <= 65536 &&
          bound_size <= range_capacity / 2 && SupportsRanges(device))
        capacity_ = range_capacity;
      D3D11_BUFFER_DESC desc = {};
      desc.ByteWidth = capacity_ ? capacity_ : size;
      desc.Usage = D3D11_USAGE_DYNAMIC;
      desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
      desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
      if (FAILED(device.device()->CreateBuffer(&desc, nullptr, buffer_.GetAddressOf()))) {
        capacity_ = 0;
        error = "Unable to create a native constant buffer";
        return Update::kFailed;
      }
    }

    uint32_t written = capacity_ ? bound_size : size;
    uint32_t offset = capacity_ ? next_offset_ : 0;
    bool append = offset && written <= capacity_ - offset;
    if (!append)
      offset = 0;
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(device.context()->Map(
            buffer_.Get(), 0, append ? D3D11_MAP_WRITE_NO_OVERWRITE : D3D11_MAP_WRITE_DISCARD, 0,
            &mapped))) {
      error = "Unable to update a native constant buffer";
      return Update::kFailed;
    }
    auto* destination = static_cast<uint8_t*>(mapped.pData) + offset;
    std::memcpy(destination, data.data(), data.size());
    // Rounded views must return zero just like reads past an exact-size buffer.
    std::memset(destination + data.size(), 0, written - data.size());
    device.context()->Unmap(buffer_.Get(), 0);
    contents_.assign(data.begin(), data.end());
    first_ = offset >> 4;
    count_ = capacity_ ? bound_size >> 4 : D3D11_REQ_CONSTANT_BUFFER_ELEMENT_COUNT;
    next_offset_ = offset + written;
    upload_bytes_ = written;
    return append ? Update::kAppend : Update::kDiscard;
  }

  ID3D11Buffer* buffer() const { return buffer_.Get(); }
  uint32_t first() const { return first_; }
  uint32_t count() const { return count_; }
  uint32_t capacity() const { return capacity_; }
  uint32_t upload_bytes() const { return upload_bytes_; }

 private:
  Microsoft::WRL::ComPtr<ID3D11Buffer> buffer_;
  std::vector<uint8_t> contents_;
  uint32_t size_ = 0, capacity_ = 0;
  uint32_t first_ = 0, count_ = 0, next_offset_ = 0, upload_bytes_ = 0;
};

}  // namespace rex::graphics::d3d11
