#pragma once

#include "runtime.h"

#include <set>

namespace simpsons::modkit::source_state {

inline constexpr size_t kMaxSources = 8;

template<class Files>
bool OwnsFile(const Files& files, uint32_t key, uint32_t address, std::string_view name) {
  const auto found = files.find(key);
  return key && address && !name.empty() && found != files.end() &&
         found->second.address == address && found->second.name == name;
}

// The guest filesystem is case-insensitive. Public paths use '/', while the
// original file manager records '\\'. Never normalize an escaping path into
// something that appears to be an installed source.
inline std::optional<std::string> Path(std::string_view text, bool native = false) {
  if (text.empty() || text.size() > 255)
    return {};
  std::string result;
  for (const unsigned char c : text) {
    if (c < 32 || c == 127 || c == ':' || (!native && c == '\\'))
      return {};
    result += c == '\\' ? '/' : c >= 'A' && c <= 'Z' ? char(c + 'a' - 'A') : char(c);
  }
  if (result.front() == '/' || !result.ends_with(".str"))
    return {};
  for (size_t start = 0; start < result.size();) {
    const auto slash = result.find('/', start);
    const auto part = std::string_view(result).substr(start, slash == result.npos ? result.size() - start : slash - start);
    if (part.empty() || part == "." || part == "..")
      return {};
    if (slash == result.npos)
      break;
    start = slash + 1;
  }
  return result;
}

inline std::map<std::string, std::string> Paths(const Json& state) {
  if (state.is_null()) return {};
  if (!state.is_object() || !state.contains("api") || !state.at("api").is_number_integer())
    throw Failure("save_incompatible", "Invalid saved object source version");
  const auto& version = state.at("api");
  if (version != 1 && version != 2)
    throw Failure("save_incompatible", "Unsupported saved object source version");
  const Json paths = version == 2 ? state.at("streams") : Json::array({state.at("path")});
  if (!paths.is_array() || paths.empty() || paths.size() > kMaxSources)
    throw Failure("save_incompatible", "Invalid saved source session paths");
  std::map<std::string, std::string> result;
  for (const auto& value : paths) {
    if (!value.is_string() || value.get_ref<const std::string&>().size() > 200)
      throw Failure("save_incompatible", "Invalid saved source session path");
    const auto path = Path(value.get_ref<const std::string&>());
    if (!path || !result.emplace(*path, value.get<std::string>()).second)
      throw Failure("save_incompatible", "Invalid or repeated saved source session path");
  }
  return result;
}

inline void Validate(const Json& state) {
  if (state.is_null())
    return;
  const auto fail = [](const char* text) { throw Failure("save_incompatible", text); };
  const auto word = [&](const Json& value) {
    if (!value.is_number_unsigned() || value.get<uint64_t>() > UINT32_MAX)
      fail("Invalid saved object source address");
    return value.get<uint32_t>();
  };
  if (!state.is_object() ||
      !state.contains("path") || !state.at("path").is_string() ||
      state.at("path").get_ref<const std::string&>().size() > 200 ||
      !Path(state.at("path").get_ref<const std::string&>()))
    fail("Invalid saved object source path");
  const auto paths = Paths(state);
  const auto version = state.at("api").get<int>();
  if (!paths.contains(*Path(state.at("path").get_ref<const std::string&>())))
    fail("The saved primary source is outside its session");
  if (state.contains("debug_claimed") && !state.at("debug_claimed").is_boolean())
    fail("Invalid saved debug source claim");
  if (version == 2) {
    if (!state.contains("debug_claimed")) fail("The saved source session has no debug ownership flag");
    if (state.at("debug_claimed").get<bool>()) {
      if (!state.contains("debug_stream") || !state.at("debug_stream").is_string())
        fail("The saved debug source claim has no path");
      const auto claim = Path(state.at("debug_stream").get_ref<const std::string&>());
      if (!claim || !paths.contains(*claim)) fail("The debug source claim is outside its session");
    } else if (state.contains("debug_stream")) {
      fail("An unclaimed source session cannot retain a debug path");
    }
  }
  if (!state.at("files").is_array() || state.at("files").empty() || state.at("files").size() > 1024 ||
      !state.at("before").is_array() || state.at("before").size() > 1024 ||
      !state.at("bodies").is_array() || state.at("bodies").size() > 1024 ||
      !word(state.at("world")) || (word(state.at("world")) & 3) ||
      !word(state.at("manager")) || (word(state.at("manager")) & 3) ||
      !word(state.at("definitions")) || word(state.at("definitions")) > 16384)
    fail("Invalid saved object source");
  std::set<uint32_t> before, keys, addresses, bodies;
  for (const auto& value : state.at("before"))
    if (!word(value) || !before.insert(word(value)).second)
      fail("Invalid saved stream ownership");
  std::set<std::string> targets;
  for (const auto& file : state.at("files")) {
    const auto key = word(file.at("key")), address = word(file.at("address"));
    if (!key || !address || (address & 3) || before.contains(key) || !keys.insert(key).second ||
        !addresses.insert(address).second || !file.at("name").is_string())
      fail("Invalid saved source file identity");
    const auto path = Path(file.at("name").get_ref<const std::string&>(), true);
    if (!path)
      fail("Invalid saved source filename");
    const Json owner = version == 2 ? file.at("source") : state.at("path");
    if (!owner.is_string()) fail("Invalid saved file source");
    const auto source = Path(owner.get_ref<const std::string&>());
    if (!source || !paths.contains(*source)) fail("A saved file belongs to an unknown source");
    if (*path == *source) targets.insert(*path);
  }
  if (targets.size() != paths.size())
    fail("The saved source session does not own every requested stream");
  for (const auto& body : state.at("bodies")) {
    const auto address = word(body.at("address")), key = word(body.at("key"));
    if (!address || (address & 3) || !bodies.insert(address).second || !keys.contains(key))
      fail("Invalid saved source definition owner");
  }
}

// Run before guest memory is replaced, not from an individual mod's restore
// notification. A valid relative path can still name the wrong saved group.
inline void ValidateClaims(const Json& profile) {
  const auto source = profile.value("spawn_source", Json());
  Validate(source);
  const auto expected = Paths(source);
  bool retained = !source.is_null() && source.value("debug_claimed", true);
  for (const auto& mod : profile.at("mods")) {
    const auto host = mod.value("host", Json::object());
    if (!host.contains("source_stream"))
      continue;
    if (!host.at("source_stream").is_string())
      throw Failure("save_incompatible", "Invalid saved mod source claim");
    const auto claim = Path(host.at("source_stream").get_ref<const std::string&>());
    if (!claim || !expected.contains(*claim))
      throw Failure("save_incompatible", "A mod's source claim does not match the saved native source");
    retained = true;
  }
  if (!source.is_null() && !retained)
    throw Failure("save_incompatible", "The saved source has no remaining owner");
}

}  // namespace simpsons::modkit::source_state
