#include <rex/graphics/d3d11/sampler_cache.h>

#include <cstdio>
#include <cstring>

namespace rex::graphics::d3d11 {

Microsoft::WRL::ComPtr<ID3D11SamplerState> SamplerCache::GetOrCreate(
    const D3D11_SAMPLER_DESC& description, std::string& error) {
  error.clear();
  for (auto entry = entries_.begin(); entry != entries_.end(); ++entry) {
    if (!std::memcmp(&entry->description, &description, sizeof(description))) {
      auto sampler = entry->sampler;
      entries_.splice(entries_.begin(), entries_, entry);
      ++statistics_.hits;
      return sampler;
    }
  }
  Entry entry = {};
  entry.description = description;
  HRESULT result = device_.device()->CreateSamplerState(&description, entry.sampler.GetAddressOf());
  if (FAILED(result)) {
    char message[100];
    std::snprintf(message, sizeof(message), "DX11 sampler creation failed (0x%08X)",
                  unsigned(result));
    error = message;
    return {};
  }
  auto sampler = entry.sampler;
  ++statistics_.creations;
  entries_.push_front(std::move(entry));
  while (entries_.size() > limit_) {
    entries_.pop_back();
    ++statistics_.evictions;
  }
  return sampler;
}

void SamplerCache::Clear() {
  entries_.clear();
  statistics_ = {};
}

}  // namespace rex::graphics::d3d11
