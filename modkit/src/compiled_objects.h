#pragma once

#include <array>
#include <cstdint>
#include <optional>

namespace simpsons::modkit::compiled_objects {

using Guid = std::array<uint32_t, 4>;
struct Object {
  Guid guid{}, model{}, character{};
  uint32_t hash = 0, offset = 0;
  Guid fallback_model{};
};

// Compiled property groups are shared by the native entity reader and SimG
// files. All displacements are signed and relative to the field containing
// them. A default bit omits the property's word from the packed value table.
// Memory supplies Has(address, bytes), word(address), and byte(address).
template <typename Memory>
std::optional<uint32_t> Relative(const Memory& memory, uint32_t field, uint32_t bytes) {
  if (!memory.Has(field, 4))
    return {};
  const auto displacement = int32_t(memory.word(field));
  const auto at = int64_t(field) + displacement;
  if (!displacement || at <= 0 || at > UINT32_MAX || (at & 3) ||
      !memory.Has(uint32_t(at), bytes))
    return {};
  return uint32_t(at);
}

template <typename Memory>
std::optional<Guid> GuidAt(const Memory& memory, uint32_t field) {
  const auto target = Relative(memory, field, 16);
  if (!target)
    return {};
  Guid result{};
  for (uint32_t i = 0; i < 4; ++i)
    result[i] = memory.word(*target + i * 4);
  return result;
}

// sub_8269B588 selects a namespace; sub_8269B428 selects the property within
// it. Zero denotes the native default, not a pointer into another record.
template <typename Memory>
std::optional<uint32_t> Property(const Memory& memory, uint32_t record, uint32_t name_space,
                                 uint32_t index) {
  if (!memory.Has(record, 32) || !(memory.byte(record + 16) & 1))
    return {};
  const uint64_t table = uint64_t(record) + 32 + uint32_t(memory.byte(record + 18)) * 4;
  const uint32_t groups = memory.byte(record + 19);
  if (table > UINT32_MAX || (groups && !memory.Has(uint32_t(table), groups * 4)))
    return {};
  std::optional<uint32_t> result;
  bool matched = false;
  for (uint32_t i = 0; i < groups; ++i) {
    const auto group = Relative(memory, uint32_t(table) + i * 4, 6);
    if (!group)
      return {};
    if (memory.word(*group) != name_space)
      continue;
    if (matched)
      return {};  // Ambiguous/corrupt namespaces must never supply an identity.
    matched = true;
    const uint32_t count = uint32_t(memory.byte(*group + 4)) * 256 + memory.byte(*group + 5);
    const uint32_t bit_bytes = (count + 7) / 8;
    if (count > 4096 || index >= count || !memory.Has(*group, 6 + bit_bytes))
      return {};
    const uint64_t values = (uint64_t(*group) + 6 + bit_bytes + 3) & ~uint64_t(3);
    uint32_t present = 0;
    for (uint32_t field = 0; field < count; ++field) {
      const bool defaulted = memory.byte(*group + 6 + field / 8) & (1u << (field % 8));
      if (field == index) {
        const auto at = values + uint64_t(present) * 4;
        if (at > UINT32_MAX || (!defaulted && !memory.Has(uint32_t(at), 4)))
          return {};
        result = defaulted ? 0 : uint32_t(at);
      }
      present += !defaulted;
    }
    if (values > UINT32_MAX || (present && !memory.Has(uint32_t(values), present * 4)))
      return {};
  }
  return result;
}

template <typename Memory>
std::optional<Object> Read(const Memory& memory, uint32_t record) {
  if (!memory.Has(record, 32) || !(memory.byte(record + 16) & 1))
    return {};
  const auto guid = GuidAt(memory, record + 24);
  if (!guid)
    return {};
  Object result{*guid, {}, {}, memory.word(record + 28), record};
  // Entity's property 1 is the MetaModel GUID (sub_82699DF0).
  if (const auto field = Property(memory, record, 0x38523FC3, 1); field && *field)
    if (const auto model = GuidAt(memory, *field))
      result.model = *model;
  // Animated's property 1 is the character bank GUID (sub_82694218).
  if (const auto field = Property(memory, record, 0xAE986323, 1); field && *field)
    if (const auto character = GuidAt(memory, *field))
      result.character = *character;
  // Entity's fallback (sub_8270DCA0) uses the first resource enumerated by
  // sub_8269A7E8/sub_8269B6B8. Its native type must be checked by the catalog;
  // a resource reference alone does not establish that it is a MetaModel.
  if (memory.byte(record + 18))
    if (const auto resource = GuidAt(memory, record + 32))
      result.fallback_model = *resource;
  return result;
}

}  // namespace simpsons::modkit::compiled_objects
