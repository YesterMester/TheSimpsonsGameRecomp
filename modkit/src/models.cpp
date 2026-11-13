#include "models.h"

#include "resource_id.h"
#include "sha256.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <set>

namespace simpsons::modkit {
namespace {
constexpr uint32_t kMaximum = 16 * 1024 * 1024;
uint32_t Big(std::span<const uint8_t> bytes, uint32_t at) {
  if (uint64_t(at) + 4 > bytes.size())
    throw Failure("invalid_model", "Model field exceeds its data");
  return uint32_t(bytes[at]) << 24 | uint32_t(bytes[at + 1]) << 16 | uint32_t(bytes[at + 2]) << 8 |
         bytes[at + 3];
}
void Put(std::span<uint8_t> bytes, uint32_t at, uint32_t value) {
  if (uint64_t(at) + 4 > bytes.size())
    throw Failure("invalid_model", "Model edit exceeds its data");
  for (uint32_t i = 0; i < 4; ++i)
    bytes[at + i] = uint8_t(value >> (24 - i * 8));
}
uint32_t Little(std::span<const uint8_t> bytes, uint32_t at) {
  if (uint64_t(at) + 4 > bytes.size())
    throw Failure("invalid_model", "Model field exceeds its data");
  return uint32_t(bytes[at]) | uint32_t(bytes[at + 1]) << 8 | uint32_t(bytes[at + 2]) << 16 |
         uint32_t(bytes[at + 3]) << 24;
}
std::string Guid(std::span<const uint8_t> bytes, uint32_t at) {
  return ResourceId({Big(bytes, at), Big(bytes, at + 4), Big(bytes, at + 8), Big(bytes, at + 12)});
}
std::string Name(std::span<const uint8_t> bytes, uint32_t at, uint32_t limit) {
  if (!at)
    return {};
  if (at >= limit || limit > bytes.size())
    throw Failure("invalid_model", "Model string exceeds its data");
  std::string result;
  for (uint32_t end = std::min<uint64_t>(limit, uint64_t(at) + 4096); at < end; ++at) {
    const auto c = bytes[at];
    if (!c)
      return result;
    if (c < 32 || c > 126)
      throw Failure("unsupported_model", "Model names need a separately reviewed encoding");
    result.push_back(char(c));
  }
  throw Failure("invalid_model", "Model string is not terminated within its bounds");
}
bool Contains(const std::vector<uint32_t>& values, uint32_t value) {
  return std::binary_search(values.begin(), values.end(), value);
}
std::string Key(std::string_view kind, size_t index) {
  return std::string(kind) + ":" + std::to_string(index);
}
float Float(const Json& value) {
  if (!value.is_number())
    throw Failure("invalid_argument", "Model float edits need a number");
  const auto number = value.get<double>();
  if (!std::isfinite(number) || std::abs(number) > std::numeric_limits<float>::max())
    throw Failure("invalid_argument", "Model float edits must be finite 32-bit values");
  return float(number);
}
std::vector<uint8_t> Copy(std::span<const uint8_t> bytes, size_t minimum) {
  if (bytes.size() < minimum || bytes.size() > kMaximum)
    throw Failure("resource_limit", "Model data is outside its supported size budget");
  return {bytes.begin(), bytes.end()};
}
}  // namespace

uint32_t ModelDefinition::Word(uint32_t at) const {
  return Big(bytes_, at);
}
uint16_t ModelDefinition::Half(uint32_t at) const {
  Range(at, 2);
  return uint16_t(uint16_t(bytes_[at]) << 8 | bytes_[at + 1]);
}
void ModelDefinition::Range(uint32_t at, uint64_t size) const {
  if (uint64_t(at) + size > bytes_.size())
    throw Failure("invalid_model", "Model table exceeds its payload");
}
uint32_t ModelDefinition::Pointer(uint32_t at, uint32_t alignment) {
  const auto value = Word(at);
  if (value > bytes_.size() || (value && value % alignment))
    throw Failure("invalid_model", "Model relocation is outside its payload or misaligned");
  if (relocations_.size() >= 262144)
    throw Failure("resource_limit", "Model relocation count exceeds its authoring budget");
  if (!relocation_fields_.insert(at).second)
    throw Failure("invalid_model", "Model relocation fields overlap");
  relocations_.push_back(at);
  return value;
}
std::string ModelDefinition::String(uint32_t at) {
  const auto value = Name(bytes_, at, uint32_t(bytes_.size()));
  if (at)
    names_.emplace_back(at, at + uint32_t(value.size()) + 1);
  return value;
}
void ModelDefinition::List(uint32_t at, uint32_t count, const std::vector<uint32_t>& targets) {
  if (count && !at)
    throw Failure("invalid_model", "Model list has no storage");
  Range(at, uint64_t(count) * 4);
  if (count)
    lists_.emplace_back(at, at + count * 4);
  for (uint32_t i = 0; i < count; ++i) {
    const auto value = Pointer(at + i * 4);
    if (value && !Contains(targets, value))
      throw Failure("invalid_model", "Model list refers to a foreign record");
  }
}

ModelDefinition::ModelDefinition(std::span<const uint8_t> bytes) : bytes_(Copy(bytes, 88)) {
  if (bytes_.size() < 88 || bytes_.size() > kMaximum || Word(0) != 0x4D4D646C || Word(4) != 10 ||
      Word(8) != bytes_.size())
    throw Failure("unsupported_model", "Expected a bounded MMdl version 10 payload");
  if (Word(12) & 4)
    throw Failure("invalid_model", "A loaded model is not an editable on-disk definition");
  if (Word(72))
    throw Failure("unsupported_model", "This model uses the unreviewed final header pointer");
  for (uint32_t at = 44; at <= 72; at += 4)
    Pointer(at, at == 68 ? 1 : 4);
  struct Section {
    uint32_t begin, end;
  };
  std::vector<Section> sections{{0, 88}};
  auto table = [&](uint32_t pointer, uint32_t count_at, uint32_t stride) {
    std::vector<uint32_t> result;
    const uint32_t count = Half(count_at), at = Word(pointer);
    if (count > 4096)
      throw Failure("resource_limit", "Model record count exceeds its authoring budget");
    if (count && (!at || at % 4))
      throw Failure("invalid_model", "Model table has no aligned storage");
    Range(at, uint64_t(count) * stride);
    if (count)
      sections.push_back({at, at + uint32_t(count) * stride});
    for (uint32_t i = 0; i < count; ++i)
      result.push_back(at + i * stride);
    return result;
  };
  references_ = table(44, 76, 32);
  const auto states = table(48, 78, 32);
  variables_ = table(52, 80, 12);
  properties_ = table(56, 82, 24);
  conditions_ = table(60, 84, 12);
  parts_ = table(64, 86, 20);
  std::sort(sections.begin(), sections.end(), [](auto a, auto b) { return a.begin < b.begin; });
  for (size_t i = 1; i < sections.size(); ++i)
    if (sections[i].begin < sections[i - 1].end)
      throw Failure("invalid_model", "Model record tables overlap");
  for (size_t i = 0; i < references_.size(); ++i) {
    const auto at = references_[i];
    if (Word(at + 28))
      throw Failure("invalid_model", "Model reference retains a runtime binding");
    reference_info_.push_back({{"id", Key("reference", i)},
                               {"resource_id", Guid(bytes_, at + 12)},
                               {"native_type", Word(at + 8)}});
  }
  for (const auto at : variables_)
    String(Pointer(at, 1));
  for (size_t i = 0; i < properties_.size(); ++i) {
    const auto at = properties_[i];
    Field field{
        Key("property", i), String(Pointer(at + 4, 1)), String(Pointer(at + 8, 1)), {}, at, 4,
        Word(at + 20)};
    if (field.type & 0x1B8)
      Pointer(at, field.type == 16 || field.type == 32 || field.type == 128 ? 4 : 1);
    Value(field);
    fields_.push_back(std::move(field));
  }
  for (size_t i = 0; i < conditions_.size(); ++i) {
    const auto at = conditions_[i];
    const auto variable = Pointer(at + 4);
    if (!Contains(variables_, variable))
      throw Failure("invalid_model", "Model condition refers to a foreign variable");
    Field field{Key("condition", i), String(Word(variable)), "condition", {}, at, 4,
                Half(variable + 8)};
    if (field.type == 2 || field.type == 4)
      Value(field);
    else {
      field.kind = "preserved";
      field.value = Word(at);
    }
    fields_.push_back(std::move(field));
  }
  for (size_t i = 0; i < parts_.size(); ++i) {
    const auto at = parts_[i];
    Json attributes = Json::array();
    const auto list = Pointer(at + 12);
    List(list, Half(at + 16), properties_);
    for (uint32_t j = 0; j < Half(at + 16); ++j) {
      const auto value = Word(list + j * 4);
      if (value)
        attributes.push_back(Key("property", (value - Word(56)) / 24));
    }
    part_info_.push_back({{"id", Key("part", i)},
                          {"type", String(Pointer(at + 4, 1))},
                          {"name", String(Pointer(at + 8, 1))},
                          {"properties", std::move(attributes)}});
  }
  for (size_t i = 0; i < states.size(); ++i) {
    const auto at = states[i];
    const auto properties = Pointer(at + 12), parts = Pointer(at + 20),
               conditions = Pointer(at + 24);
    List(properties, Half(at + 16), properties_);
    List(parts, Half(at + 28), parts_);
    List(conditions, Half(at + 30), conditions_);
    auto ids = [&](uint32_t list, uint32_t count, uint32_t first, uint32_t stride,
                   const char* prefix) {
      Json result = Json::array();
      for (uint32_t j = 0; j < count; ++j)
        if (const auto value = Word(list + j * 4))
          result.push_back(Key(prefix, (value - first) / stride));
      return result;
    };
    states_.push_back({{"id", Key("state", i)},
                       {"type", String(Pointer(at + 4, 1))},
                       {"name", String(Pointer(at + 8, 1))},
                       {"properties", ids(properties, Half(at + 16), Word(56), 24, "property")},
                       {"parts", ids(parts, Half(at + 28), Word(64), 20, "part")},
                       {"conditions", ids(conditions, Half(at + 30), Word(60), 12, "condition")}});
  }
  std::sort(relocations_.begin(), relocations_.end());
  if (std::adjacent_find(relocations_.begin(), relocations_.end()) != relocations_.end())
    throw Failure("invalid_model", "Model relocation fields overlap");
  for (const auto [begin, end] : lists_)
    for (const auto& section : sections)
      if (begin < section.end && end > section.begin)
        throw Failure("invalid_model", "Model pointer lists overlap record tables");
  for (const auto [begin, end] : names_) {
    for (const auto& section : sections)
      if (begin < section.end && end > section.begin)
        throw Failure("invalid_model", "Model strings overlap record tables");
    for (const auto [list_begin, list_end] : lists_)
      if (begin < list_end && end > list_begin)
        throw Failure("invalid_model", "Model strings overlap pointer lists");
  }
  const bool opaque_values =
      Word(68) || std::any_of(fields_.begin(), fields_.end(), [&](const auto& field) {
        return field.kind == "preserved" && (field.type & 0x1B8) && Word(field.offset);
      });
  // Shared out-of-line values stay read-only: changing one field must not
  // silently change another part or overwrite structural/relocation data.
  for (auto& field : fields_) {
    if (!field.editable || field.offset < 88) {
      field.editable = false;
      continue;
    }
    if (field.type != 16 && (field.type & 0x1B8)) {
      if (opaque_values)
        field.editable = false;
      for (const auto& section : sections)
        if (field.offset < section.end && uint64_t(field.offset) + field.size > section.begin)
          field.editable = false;
      for (const auto* ranges : {&lists_, &names_})
        for (const auto [begin, end] : *ranges)
          if (field.offset < end && uint64_t(field.offset) + field.size > begin)
            field.editable = false;
      for (const auto& other : fields_)
        if (&other != &field && field.offset < uint64_t(other.offset) + other.size &&
            uint64_t(field.offset) + field.size > other.offset)
          field.editable = false;
    }
  }
}

void ModelDefinition::Value(Field& field) {
  const auto at = field.offset;
  switch (field.type) {
    case 2:
      field.kind = "uint32";
      field.value = Word(at);
      field.editable = true;
      return;
    case 4: {
      field.kind = "float";
      const auto number = std::bit_cast<float>(Word(at));
      if (std::isfinite(number)) {
        field.value = number;
        field.editable = true;
      }
      return;
    }
    case 8:
      field.kind = "string";
      field.value = String(Word(at));
      return;
    case 16: {
      field.kind = "resource";
      const auto value = Word(at);
      if (!value)
        return;
      if (!Contains(references_, value))
        throw Failure("invalid_model", "Model property refers to a foreign resource record");
      field.value = Key("reference", (value - Word(44)) / 32);
      field.editable = true;
      return;
    }
    case 32:
      field.kind = "matrix";
      field.size = 64;
      break;
    case 128:
      field.kind = "vector3";
      field.size = 12;
      break;
    default:
      field.kind = "preserved";
      field.value = Word(at);
      return;
  }
  field.offset = Word(at);
  if (!field.offset)
    return;
  Range(field.offset, field.size);
  field.value = Json::array();
  field.editable = true;
  for (uint32_t i = 0; i < field.size; i += 4) {
    const auto number = std::bit_cast<float>(Word(field.offset + i));
    if (!std::isfinite(number)) {
      field.value = nullptr;
      field.editable = false;
      return;
    }
    field.value.push_back(number);
  }
}

std::string ModelDefinition::Id() const {
  return Guid(bytes_, 16);
}
std::string ModelDefinition::Sha256() const {
  return sha256::SHA256{}(bytes_.data(), bytes_.size());
}
Json ModelDefinition::Describe() const {
  Json fields = Json::array();
  for (const auto& field : fields_)
    fields.push_back({{"id", field.id},
                      {"name", field.name},
                      {"owner", field.owner},
                      {"kind", field.kind},
                      {"native_type", field.type},
                      {"editable", field.editable},
                      {"value", field.value}});
  return {{"format", "MMdl"},
          {"version", 10},
          {"resource_id", Id()},
          {"bytes", bytes_.size()},
          {"source_sha256", Sha256()},
          {"references", reference_info_},
          {"parts", part_info_},
          {"states", states_},
          {"fields", std::move(fields)},
          {"changes", Json::array()}};
}

std::vector<uint8_t> ModelDefinition::Edit(const Json& changes) const {
  if (!changes.is_array() || changes.size() > 4096)
    throw Failure("invalid_argument", "Model changes must be a bounded array of named fields");
  auto result = bytes_;
  std::set<std::string> seen;
  for (const auto& change : changes) {
    if (!change.is_object() || change.size() != 2 || !change.contains("field") ||
        !change.at("field").is_string() || !change.contains("value"))
      throw Failure("invalid_argument", "Each model change needs field and value");
    const auto id = change.at("field").get<std::string>();
    const auto found = std::find_if(fields_.begin(), fields_.end(),
                                    [&](const auto& field) { return field.id == id; });
    if (found == fields_.end() || !found->editable || !seen.insert(id).second)
      throw Failure("invalid_argument", "Unknown, read-only or repeated model field: " + id);
    const auto& field = *found;
    const auto& value = change.at("value");
    if (field.kind == "uint32") {
      if (!value.is_number_integer() || (!value.is_number_unsigned() && value.get<int64_t>() < 0) ||
          value.get<uint64_t>() > UINT32_MAX)
        throw Failure("invalid_argument", "Model integers must be unsigned 32-bit values");
      Put(result, field.offset, value.get<uint32_t>());
    } else if (field.kind == "float") {
      Put(result, field.offset, std::bit_cast<uint32_t>(Float(value)));
    } else if (field.kind == "resource") {
      const auto next = std::find_if(reference_info_.begin(), reference_info_.end(),
                                     [&](const auto& ref) { return ref.at("id") == value; });
      if (next == reference_info_.end())
        throw Failure("invalid_argument", "Choose an existing model reference");
      if (next->at("resource_id") == "")
        throw Failure("invalid_argument", "Choose a nonzero resource identity");
      const auto replacement = references_[size_t(next - reference_info_.begin())];
      const auto original = Word(field.offset);
      if (Word(original + 8) != Word(replacement + 8))
        throw Failure("invalid_argument", "Model references must keep their native resource type");
      Put(result, field.offset, replacement);
    } else {
      if (!value.is_array() || value.size() != field.size / 4)
        throw Failure("invalid_argument", "Model vector/matrix edit has the wrong dimensions");
      for (size_t i = 0; i < value.size(); ++i)
        Put(result, field.offset + uint32_t(i) * 4, std::bit_cast<uint32_t>(Float(value.at(i))));
    }
  }
  ModelDefinition check(result);
  if (check.Id() != Id() || check.bytes().size() != bytes_.size())
    throw Failure("invalid_model", "Model edit changed its resource identity or layout");
  return result;
}
std::vector<uint8_t> ModelDefinition::Apply(const Json& document) const {
  if (!document.is_object() || !document.contains("format") || document.at("format") != "MMdl" ||
      !document.contains("version") || !document.at("version").is_number_integer() ||
      document.at("version") != 10 || !document.contains("resource_id") ||
      document.at("resource_id") != Id() || !document.contains("source_sha256") ||
      document.at("source_sha256") != Sha256() || !document.contains("changes"))
    throw Failure("source_changed", "Model document does not match this exact native resource");
  const auto description = Describe();
  for (const auto* name : {"bytes", "references", "parts", "states", "fields"})
    if (document.contains(name) && document.at(name) != description.at(name))
      throw Failure("invalid_argument",
                    "Model inspection metadata is read-only; edit the changes list");
  return Edit(document.at("changes"));
}
std::vector<uint8_t> ModelDefinition::ApplyVariant(const Json& document) const {
  auto result = Apply(document);
  for (const auto& change : document.at("changes")) {
    const auto id = change.at("field").get<std::string>();
    const auto found = std::find_if(fields_.begin(), fields_.end(),
                                    [&](const auto& field) { return field.id == id; });
    if (found == fields_.end() || found->owner != "ModelPart" || found->name != "Model" ||
        found->type != 16 || Word(Word(found->offset) + 8) != 0x9E3791CF)
      throw Failure("unsupported_model",
                    "Live variants currently support ModelPart.Model DFF selections only");
  }
  return result;
}
std::vector<uint8_t> ModelDefinition::Relocated(uint32_t base) const {
  if (uint64_t(base) + bytes_.size() > UINT32_MAX || base % 4)
    throw Failure("invalid_argument", "Model relocation base must fit a 32-bit address");
  auto result = bytes_;
  for (const auto at : relocations_)
    if (const auto value = Word(at))
      Put(result, at, value + base);
  return result;
}

NamedResource::NamedResource(std::span<const uint8_t> bytes) : data(Copy(bytes, 64)) {
  if (bytes.size() < 64 || bytes.size() > kMaximum || Little(bytes, 0) != 0x716 ||
      uint64_t(Little(bytes, 4)) + 12 != bytes.size())
    throw Failure("unsupported_model", "Expected one bounded named RenderWare resource");
  const auto header = Big(bytes, 12);
  if (uint64_t(header) + 16 > bytes.size())
    throw Failure("invalid_model", "Model wrapper header exceeds its resource");
  body_offset = header + 16;
  auto word = [&](uint64_t at) {
    if (at + 4 > body_offset)
      throw Failure("invalid_model", "Model wrapper pointer exceeds its header");
    return Big(bytes, uint32_t(at));
  };
  const uint64_t padded = word(16), guid = 20 + padded, class_name = guid + 20;
  if (!padded || guid + 20 > body_offset)
    throw Failure("invalid_model", "Model name exceeds its wrapper");
  name = Name(bytes, 20, uint32_t(guid));
  resource_id = Guid(bytes, uint32_t(guid));
  const uint64_t metadata = class_name + word(guid + 16);
  const uint64_t indirect = metadata + word(metadata) + 4;
  const uint64_t allocation = indirect + word(indirect);
  native_class = Name(bytes, uint32_t(class_name), body_offset);
  body_size = word(allocation + 8);
  if (uint64_t(body_offset) + body_size > bytes.size())
    throw Failure("invalid_model", "MMdl body exceeds its wrapper");
}
ModelResource::ModelResource(std::span<const uint8_t> bytes) : NamedResource(bytes) {
  if (native_class != "MetaModel")
    throw Failure("unsupported_model", "Named resource is not a MetaModel");
  const auto model = Definition();
  if (model.Id() != resource_id)
    throw Failure("invalid_model", "Model wrapper and payload identities disagree");
}
ModelDefinition ModelResource::Definition() const {
  return ModelDefinition(std::span(data).subspan(body_offset, body_size));
}
std::vector<uint8_t> ModelResource::Edit(const Json& changes) const {
  auto result = data;
  const auto body = Definition().Edit(changes);
  std::copy(body.begin(), body.end(), result.begin() + body_offset);
  ModelResource check(result);
  return result;
}

Json ValidateModelVariants(const Json& catalog) {
  if (!catalog.is_object() || catalog.size() != 2 || !catalog.contains("version") ||
      !catalog.at("version").is_number_integer() || catalog.at("version") != 1 ||
      !catalog.contains("models") || !catalog.at("models").is_array() ||
      catalog.at("models").size() > 256)
    throw Failure("invalid_model",
                  "A model variant catalog needs version 1 and at most 256 models");
  Json models = Json::array();
  std::set<std::string> seen;
  auto key = [](const Json& value, std::string_view prefix) {
    if (!value.is_string())
      return false;
    const auto text = value.get<std::string>();
    const auto number = std::string_view(text).substr(std::min(prefix.size(), text.size()));
    return text.starts_with(prefix) && !number.empty() && number.size() <= 5 &&
           (number.size() == 1 || number.front() != '0') &&
           number.find_first_not_of("0123456789") == std::string_view::npos;
  };
  for (const auto& document : catalog.at("models")) {
    if (!document.is_object() || document.value("format", Json()) != "MMdl" ||
        !document.contains("version") || !document.at("version").is_number_integer() ||
        document.at("version") != 10 || !document.contains("resource_id") ||
        !document.at("resource_id").is_string() || !document.contains("source_sha256") ||
        !document.at("source_sha256").is_string() || !document.contains("changes") ||
        !document.at("changes").is_array() || document.at("changes").empty() ||
        document.at("changes").size() > 4096)
      throw Failure("invalid_model",
                    "Model variants need an identity, source SHA-256 and named changes");
    const auto id = document.at("resource_id").get<std::string>();
    const auto hash = document.at("source_sha256").get<std::string>();
    if (id.size() != 32 || id.find_first_not_of("0123456789ABCDEF") != std::string::npos ||
        id == std::string(32, '0') || hash.size() != 64 ||
        hash.find_first_not_of("0123456789abcdef") != std::string::npos || !seen.insert(id).second)
      throw Failure("invalid_model",
                    "Model variants need unique native identities and lowercase source SHA-256");
    std::set<std::string> changed;
    for (const auto& change : document.at("changes")) {
      if (!change.is_object() || change.size() != 2 || !change.contains("field") ||
          !change.contains("value") || !key(change.at("field"), "property:") ||
          !key(change.at("value"), "reference:") ||
          !changed.insert(change.at("field").get<std::string>()).second)
        throw Failure("unsupported_model",
                      "Live model variants select unique properties and existing references");
    }
    // Keep inspection metadata when present, so accidental edits to those
    // read-only fields cannot disappear silently during preparation.
    models.push_back(document);
  }
  return {{"version", 1}, {"models", std::move(models)}};
}

ModelVariants::ModelVariants(const std::vector<Manifest>& manifests) {
  size_t total = 0;
  for (const auto& mod : manifests) {
    if (!mod.model_edits)
      continue;
    const auto catalog = ValidateModelVariants(*mod.model_edits);
    for (const auto& document : catalog.at("models")) {
      if (++total > 4096)
        throw Failure("resource_limit", "A profile supports at most 4096 model variants");
      models_[document.at("resource_id").get<std::string>()] = {mod.id, document};
    }
  }
}
std::optional<ModelVariants::Result> ModelVariants::Apply(std::span<const uint8_t> bytes) const {
  if (bytes.size() < 88)
    return {};
  const auto found = models_.find(Guid(bytes, 16));
  if (found == models_.end())
    return {};
  try {
    return Result{found->second.owner, ModelDefinition(bytes).ApplyVariant(found->second.document)};
  } catch (const std::exception& error) {
    throw Failure("invalid_model",
                  found->second.owner + " model " + found->first + ": " + error.what());
  }
}

}  // namespace simpsons::modkit
