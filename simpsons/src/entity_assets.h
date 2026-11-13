#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace entity_assets {
// Read-only identities of resources already retained by the native model.
// These do not load an asset or make a template usable in another level.
struct Model {
  std::array<uint32_t, 4> guid{};
  std::string name;
  std::string Id() const;
};
std::optional<Model> ModelOf(const uint8_t* base, uint32_t entity);
// Observe the original named-resource reader without retaining stream memory.
// Complete only after that reader returns; failed loads add no model name.
std::optional<Model> ReadStreamName(const uint8_t* base, uint32_t stream);
void Loaded(const uint8_t* base, const Model& pending);
void Reset();
// Bounded ASCII resource/class names. Missing mappings and unterminated or
// non-text data return an empty name, including across a page boundary.
std::string ReadName(const uint8_t* base, uint32_t address, size_t limit = 256);
}  // namespace entity_assets
