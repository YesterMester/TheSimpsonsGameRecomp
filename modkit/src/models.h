#pragma once

#include "runtime.h"

#include <span>
#include <unordered_set>

namespace simpsons::modkit {

// MMdl version 10, in its on-disk, big-endian form. The reader validates every
// relocation used by the original loader. It never retains a guest pointer.
class ModelDefinition {
 public:
  explicit ModelDefinition(std::span<const uint8_t> bytes);
  Json Describe() const;
  // Edits address named fields in Describe(), not arbitrary byte offsets.
  // Counts, native classes and dependencies stay intact. Resource properties
  // may select another existing reference of the same native resource type.
  std::vector<uint8_t> Edit(const Json& changes) const;
  std::vector<uint8_t> Apply(const Json& document) const;
  // The live loader currently qualifies ModelPart's existing DFF choices.
  // Other typed edits remain available to the offline writer for research.
  std::vector<uint8_t> ApplyVariant(const Json& document) const;
  std::vector<uint8_t> Relocated(uint32_t base) const;
  const std::vector<uint32_t>& relocations() const { return relocations_; }
  const std::vector<uint8_t>& bytes() const { return bytes_; }
  std::string Id() const;
  std::string Sha256() const;

 private:
  struct Field {
    std::string id, name, owner, kind;
    uint32_t offset = 0, size = 0, type = 0;
    Json value;
    bool editable = false;
  };
  uint32_t Word(uint32_t at) const;
  uint16_t Half(uint32_t at) const;
  void Range(uint32_t at, uint64_t size) const;
  uint32_t Pointer(uint32_t at, uint32_t alignment = 4);
  std::string String(uint32_t at);
  void List(uint32_t at, uint32_t count, const std::vector<uint32_t>& targets);
  void Value(Field& field);
  std::vector<uint8_t> bytes_;
  std::vector<uint32_t> relocations_, references_, properties_, conditions_, parts_, variables_;
  std::unordered_set<uint32_t> relocation_fields_;
  std::vector<Field> fields_;
  std::vector<std::pair<uint32_t, uint32_t>> lists_, names_;
  Json states_ = Json::array(), part_info_ = Json::array(), reference_info_ = Json::array();
};

// A named RenderWare 0x716 wrapper can contain alignment padding after MMdl.
// Preserve that wrapper and padding exactly when replacing this fixed-size body.
struct NamedResource {
  explicit NamedResource(std::span<const uint8_t> bytes);
  std::string name, resource_id, native_class;
  uint32_t body_offset = 0, body_size = 0;
  std::vector<uint8_t> data;
};
struct ModelResource : NamedResource {
  explicit ModelResource(std::span<const uint8_t> bytes);
  ModelDefinition Definition() const;
  std::vector<uint8_t> Edit(const Json& changes) const;
};

// Source documents are verified with the rest of the package before play.
// Selection follows the same whole-resource priority as file replacements.
Json ValidateModelVariants(const Json& catalog);
class ModelVariants {
 public:
  explicit ModelVariants(const std::vector<Manifest>& manifests);
  struct Result {
    std::string owner;
    std::vector<uint8_t> bytes;
  };
  std::optional<Result> Apply(std::span<const uint8_t> bytes) const;
  size_t size() const { return models_.size(); }

 private:
  struct Variant {
    std::string owner;
    Json document;
  };
  std::map<std::string, Variant> models_;
};

}  // namespace simpsons::modkit
