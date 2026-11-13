#pragma once

#include "models.h"

#include <array>

namespace simpsons::modkit {

// EA's preinstanced Xbox 360 dictionaries retain native texture, raster and
// fetch headers. Image edits preserve those records and the original allocation.
// DDS carries linear BC1/BC2/BC3 blocks or RGBA8 pixels, including every mip.
class TextureResource : public NamedResource {
 public:
  struct Level {
    uint32_t width = 0, height = 0, offset = 0, pitch = 0, x = 0, y = 0;
  };
  struct Texture {
    std::string id, name, mask, reason;
    uint32_t record = 0, raster = 0, pixels = 0, allocation = 0;
    uint32_t width = 0, height = 0, format = 0, endian = 0, block = 0, bytes = 0;
    std::array<uint32_t, 6> fetch{};
    std::vector<Level> levels;
    bool tiled = false, editable = false;
  };
  explicit TextureResource(std::span<const uint8_t> bytes);
  Json Describe() const;
  std::vector<uint8_t> Dds(size_t index) const;
  Json Import(size_t index, std::span<const uint8_t> dds) const;
  std::vector<uint8_t> Apply(const Json& document) const;
  const std::vector<Texture>& textures() const { return textures_; }

 private:
  uint32_t Word(uint32_t at) const;
  void Range(uint64_t at, uint64_t bytes) const;
  void Layout(Texture& texture);
  uint32_t Address(const Texture& texture, size_t level, uint32_t x, uint32_t y) const;
  std::vector<uint8_t> Linear(const Texture& texture, size_t level) const;
  std::vector<Texture> textures_;
};

Json ValidateTextureVariants(const Json& catalog);
class TextureVariants {
 public:
  explicit TextureVariants(const std::vector<Manifest>& manifests);
  bool Contains(std::string_view id) const;
  size_t size() const { return textures_.size(); }
  struct Result {
    std::string owner;
    std::vector<uint8_t> bytes;
  };
  std::optional<Result> Apply(std::span<const uint8_t> bytes) const;

 private:
  struct Variant {
    std::string owner;
    Json document;
  };
  std::map<std::string, Variant, std::less<>> textures_;
};

}  // namespace simpsons::modkit
