#pragma once

#include <cstdint>
#include <list>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/ui/d3d11/d3d11_device.h>

namespace rex::graphics::d3d11 {

enum class BufferKind { kRaw, kIndex16, kIndex32, kConstant };

class BufferVersion {
 public:
  ID3D11Buffer* buffer() const { return buffer_.Get(); }
  ID3D11ShaderResourceView* raw_view() const { return raw_view_.Get(); }
  BufferKind kind() const { return kind_; }
  size_t size() const { return gpu_source_size_ ? gpu_source_size_ : source_.size(); }
  uint32_t allocation_size() const { return allocation_size_; }

 private:
  friend class BufferCache;
  BufferKind kind_;
  uint32_t allocation_size_ = 0;
  uint32_t gpu_source_size_ = 0;
  std::vector<uint8_t> source_;
  Microsoft::WRL::ComPtr<ID3D11Buffer> buffer_;
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> raw_view_;
};

// Immutable native vertex/index snapshots and constant buffers. All source
// bytes are checked before reuse. The caller pins versions needed by a frame;
// cache eviction cannot modify those versions or recycle their GPU storage.
class BufferCache {
 public:
  struct Statistics {
    uint64_t hits = 0;
    uint64_t creations = 0;
    uint64_t evictions = 0;
    uint64_t retained_bytes = 0;
    uint64_t uploaded_bytes = 0;
  };

  explicit BufferCache(ui::d3d11::D3D11Device& device, uint64_t budget = 8 * 1024 * 1024)
      : device_(device), budget_(budget) {}
  std::shared_ptr<const BufferVersion> GetOrCreate(std::span<const uint8_t> source, BufferKind kind,
                                                   std::string& error);
  // Caller unbinds the immediate context first. GPU-owned inputs receive a
  // fresh retained copy without a CPU download or reuse by guest address.
  std::shared_ptr<const BufferVersion> SnapshotGpuRange(ID3D11Buffer* source, uint32_t offset,
                                                        uint32_t length, BufferKind kind,
                                                        std::string& error);
  void Clear();
  const Statistics& statistics() const { return statistics_; }

 private:
  struct Entry {
    uint64_t hash;
    uint64_t cost;
    std::shared_ptr<const BufferVersion> version;
  };
  using Entries = std::list<Entry>;
  void Trim();

  ui::d3d11::D3D11Device& device_;
  // Counts cache-owned GPU bytes and source snapshots. Frames can pin evicted
  // versions until their work completes, independently of this cache budget.
  uint64_t budget_;
  Entries entries_;
  std::unordered_multimap<uint64_t, Entries::iterator> lookup_;
  Statistics statistics_;
};

}  // namespace rex::graphics::d3d11
