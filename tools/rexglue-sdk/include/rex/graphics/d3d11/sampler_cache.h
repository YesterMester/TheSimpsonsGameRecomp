#pragma once

#include <list>
#include <string>

#include <rex/ui/d3d11/d3d11_device.h>

namespace rex::graphics::d3d11 {

// Native sampler objects with bounded cache ownership. Callers retain the
// returned reference through binding, so eviction cannot invalidate a draw.
class SamplerCache {
 public:
  struct Statistics {
    uint64_t hits = 0;
    uint64_t creations = 0;
    uint64_t evictions = 0;
  };
  explicit SamplerCache(ui::d3d11::D3D11Device& device, size_t limit = 1024)
      : device_(device), limit_(limit) {}
  Microsoft::WRL::ComPtr<ID3D11SamplerState> GetOrCreate(const D3D11_SAMPLER_DESC& description,
                                                         std::string& error);
  void Clear();
  const Statistics& statistics() const { return statistics_; }

 private:
  struct Entry {
    D3D11_SAMPLER_DESC description;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler;
  };
  ui::d3d11::D3D11Device& device_;
  size_t limit_;
  std::list<Entry> entries_;
  Statistics statistics_;
};

}  // namespace rex::graphics::d3d11
