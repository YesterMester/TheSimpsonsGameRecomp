#pragma once

#include "models.h"

#include <array>

namespace simpsons::modkit {

// The reviewed declaration uses signed, normalized 10-bit XYZ. Preserve W and
// exact source encodings when a component's decoded value has not changed.
std::array<float, 3> DecodeMeshNormal(uint32_t packed);
uint32_t EncodeMeshNormal(uint32_t original, const std::array<float, 3>& normal);

// EA's preinstanced geometry plugin (0xEA33). Its little-endian outer header
// contains big-endian main/secondary blocks. Original sub_82831280 relocates
// the explicit field table; sub_828312D0 copies and rebases both blocks.
class NativeMeshData {
 public:
  explicit NativeMeshData(std::span<const uint8_t> plugin);
  struct Relocation {
    uint32_t field, block;
  };
  struct Section {
    uint32_t type, size, offset;
  };
  const std::vector<uint8_t>& main() const { return main_; }
  const std::vector<uint8_t>& secondary() const { return secondary_; }
  const std::vector<Relocation>& relocations() const { return relocations_; }
  const std::vector<Section>& sections() const { return sections_; }
  std::vector<uint8_t> Relocated(uint32_t main_base, uint32_t secondary_base) const;
  uint32_t Word(uint32_t at) const;

 private:
  std::vector<uint8_t> main_, secondary_;
  std::vector<Relocation> relocations_;
  std::vector<Section> sections_;
};

// Typed mesh inspection preserves the complete named chunk. The first writer
// edits static float3 vertices/normals and independent float2 UVs, preserving
// topology and bounds. UV-only edits can use reviewed skinned streams while
// positions, weights and normal data remain untouched.
class MeshResource : public NamedResource {
 public:
  explicit MeshResource(std::span<const uint8_t> bytes);
  Json Describe(bool include_geometry = true) const;
  std::vector<uint8_t> Apply(const Json& document) const;
  struct Geometry {
    std::string id, reason, normals_reason, uvs_reason;
    uint32_t structure = 0, plugin = 0, vertices = 0, source_vertices = 0, stride = 0,
             positions = 0, normal_positions = 0, uv_positions = 0;
    std::vector<uint32_t> triangles;
    std::vector<std::array<float, 3>> coordinates;
    std::vector<std::array<float, 3>> normals;
    std::vector<std::array<float, 2>> uvs;
    std::array<float, 4> sphere{};
    std::array<float, 6> bounds{};
    bool has_bounds = false, skinned = false, editable = false, normals_editable = false,
         uvs_editable = false;
  };
  const std::vector<Geometry>& geometries() const { return geometries_; }

 private:
  struct Chunk {
    uint32_t type, offset, size;
  };
  std::vector<Chunk> Children(uint32_t at, uint32_t bytes) const;
  void GeometryAt(const Chunk& chunk, size_t index);
  std::vector<Geometry> geometries_;
};

Json ValidateMeshVariants(const Json& catalog);
class MeshVariants {
 public:
  explicit MeshVariants(const std::vector<Manifest>& manifests);
  bool Contains(std::string_view id) const;
  size_t size() const { return meshes_.size(); }
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
  std::map<std::string, Variant, std::less<>> meshes_;
};

}  // namespace simpsons::modkit
