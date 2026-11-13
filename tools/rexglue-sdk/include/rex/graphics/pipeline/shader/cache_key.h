#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>

#include <rex/graphics/xenos.h>

namespace rex::graphics {

// Microcode does not encode its shader stage. Keep the raw data hash for
// storage/AOT identities, but include the stage in in-memory cache equality.
struct ShaderCacheKey {
  uint64_t hash;
  xenos::ShaderType type;

  ShaderCacheKey(uint64_t data_hash, xenos::ShaderType shader_type)
      : hash(data_hash), type(shader_type) {}
  auto operator<=>(const ShaderCacheKey&) const = default;
};

struct ShaderCacheKeyHasher {
  std::size_t operator()(const ShaderCacheKey& key) const noexcept {
    return std::size_t(key.hash ^ (uint64_t(key.type) * 0x9E3779B97F4A7C15ull));
  }
};

}  // namespace rex::graphics
