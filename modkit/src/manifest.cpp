#include "runtime.h"
#include "models.h"
#include "meshes.h"
#include "textures.h"
#include "scenes.h"
#include "project_documents.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <fstream>
#include <set>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "sha256.h"
#include "api_catalog.h"

namespace simpsons::modkit {
namespace {
bool Identifier(std::string_view s) {
  return !s.empty() && s.size() <= 96 &&
         ((s.front() >= 'a' && s.front() <= 'z') || (s.front() >= '0' && s.front() <= '9')) &&
         std::all_of(s.begin(), s.end(), [](unsigned char c) {
           return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                  c == '_' || c == '-' || c == '.';
         });
}
std::array<uint64_t, 3> Version(std::string_view text) {
  std::array<uint64_t, 3> result{};
  for (unsigned i = 0; i < 3; ++i) {
    const auto end = text.find('.');
    const auto part = text.substr(0, end);
    const auto parsed = std::from_chars(part.data(), part.data() + part.size(), result[i]);
    if (part.empty() || (part.size() > 1 && part.front() == '0') ||
        parsed.ec != std::errc() || parsed.ptr != part.data() + part.size() || result[i] > 0xFFFFFFFFull)
      throw Failure("invalid_version", "Use numeric versions such as 1.0.0");
    if (end == std::string_view::npos) {
      if (i != 2)
        throw Failure("invalid_version", "Versions need major.minor.patch");
      return result;
    }
    text.remove_prefix(end + 1);
  }
  throw Failure("invalid_version", "Versions need major.minor.patch");
}
std::string HashFile(const fs::path& path, std::stop_token stop) {
  sha256::SHA256 hash;
  std::ifstream input(path, std::ios::binary);
  if (!input)
    throw Failure("read_failed", "Cannot read " + path.string());
  std::array<char, 65536> buffer;
  while (input.read(buffer.data(), buffer.size()) || input.gcount()) {
    if (stop.stop_requested()) throw Failure("cancelled", "Mod source preparation cancelled");
    hash.add(buffer.data(), static_cast<size_t>(input.gcount()));
  }
  if (!input.eof())
    throw Failure("read_failed", "Cannot finish reading " + path.string());
  return hash.getHash();
}
}  // namespace

fs::path Utf8Path(std::string_view text) {
  return fs::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
}
std::string Utf8(const fs::path& path) {
  const auto value = path.generic_u8string();
  return std::string(value.begin(), value.end());
}
fs::path RelativePath(std::string_view relative) {
  if (relative.empty() || relative.size() > 512 || relative.front() == '/' ||
      relative.find_first_of("\\:<>\"|?*") != std::string_view::npos ||
      std::any_of(relative.begin(), relative.end(), [](unsigned char c) { return c < 32; }))
    throw Failure("invalid_path", "Use a relative path with forward slashes");
  size_t first = 0, depth = 0;
  while (first <= relative.size()) {
    const auto end = relative.find('/', first);
    const auto part = relative.substr(first, end == std::string_view::npos ? end : end - first);
    if (part.empty() || part.back() == '.' || part.back() == ' ' || ++depth > 64)
      throw Failure("invalid_path", "A mod path has an invalid component");
    std::string stem(part.substr(0, part.find('.')));
    for (char& c : stem) if (c >= 'a' && c <= 'z') c -= 'a' - 'A';
    if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL" ||
        (stem.size() == 4 && (stem.starts_with("COM") || stem.starts_with("LPT")) &&
         stem[3] >= '1' && stem[3] <= '9'))
      throw Failure("invalid_path", "A mod path uses a reserved Windows device name");
    if (end == std::string_view::npos) break;
    first = end + 1;
  }
  const auto path = Utf8Path(relative);
  if (path.is_absolute() || path.has_root_name())
    throw Failure("invalid_path", "An absolute path is not a mod path");
  for (const auto& part : path)
    if (part == ".." || part == "." || part.empty())
      throw Failure("invalid_path", "A mod path cannot contain dot segments");
  return path;
}
fs::path Inside(const fs::path& root, std::string_view relative) {
  const auto path = RelativePath(relative);
  const auto parent = fs::weakly_canonical(root);
  const auto result = fs::weakly_canonical(parent / path);
  const auto tail = result.lexically_relative(parent);
  if (tail.empty() || tail.is_absolute() || *tail.begin() == "..")
    throw Failure("invalid_path", "A mod path escaped its folder");
  return result;
}

std::string ReadText(const fs::path& path, size_t maximum) {
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  if (!input)
    throw Failure("read_failed", "Cannot read " + path.string());
  const auto size = input.tellg();
  if (size < 0 || static_cast<uint64_t>(size) > maximum)
    throw Failure("file_too_large", "Text file exceeds its size limit");
  std::string result(static_cast<size_t>(size), '\0');
  input.seekg(0);
  if (!result.empty() && !input.read(result.data(), result.size()))
    throw Failure("read_failed", "Cannot finish reading " + path.string());
  return result;
}

void WriteText(const fs::path& path, std::string_view text) {
  if (!path.parent_path().empty())
    fs::create_directories(path.parent_path());
  auto temporary = path;
  temporary += ".part";
  {
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output || !output.write(text.data(), text.size()) || !output.flush())
      throw Failure("write_failed", "Cannot write " + path.string());
  }
#if defined(_WIN32)
  if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    throw Failure("write_failed", "Cannot replace the mod storage file");
#else
  fs::rename(temporary, path);
#endif
}

std::string Fingerprint(const fs::path& folder, Json* inventory, std::stop_token stop) {
  std::vector<std::pair<std::string, fs::path>> files;
  std::set<std::string> keys;
  uint64_t bytes = 0;
  for (auto it = fs::recursive_directory_iterator(folder); it != fs::recursive_directory_iterator(); ++it) {
    if (stop.stop_requested()) throw Failure("cancelled", "Mod source preparation cancelled");
    const auto filename = Utf8(it->path().filename());
    if (it->is_symlink())
      throw Failure("invalid_path", "Mod packages cannot contain symbolic links");
    if (it->is_directory()) {
      if (filename.starts_with('.') || filename == "build" || filename == "__pycache__")
        it.disable_recursion_pending();
      continue;
    }
    if (!it->is_regular_file())
      throw Failure("invalid_path", "Mod packages must contain regular files");
    if (it->path().lexically_relative(folder) == "checksums.json") continue;
    if (it->file_size() > 8ull * 1024 * 1024 * 1024 - bytes || files.size() >= 100000)
      throw Failure("resource_limit", "Mod package exceeds its file or size limit");
    bytes += it->file_size();
    const auto relative = it->path().lexically_relative(folder).generic_u8string();
    std::string name(relative.begin(), relative.end());
    if (name == "checksums.json")
      continue;
    std::string key = name;
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) {
      return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
    });
    if (key == "default.xex" || key.ends_with("/default.xex"))
      throw Failure("invalid_manifest", "A mod cannot contain the game's executable");
    Inside(folder, name);
    if (!keys.insert(key).second)
      throw Failure("path_conflict", "Mod paths differ only in letter case: " + name);
    files.emplace_back(std::move(name), it->path());
  }
  std::sort(files.begin(), files.end());
  sha256::SHA256 result;
  if (inventory) *inventory = Json::object();
  for (const auto& [name, file] : files) {
    const auto digest = HashFile(file, stop);
    if (inventory) (*inventory)[name] = digest;
    const std::string record = name + '\0' + digest + '\n';
    result.add(record.data(), record.size());
  }
  return result.getHash();
}

bool VersionMatches(std::string_view version, std::string_view requirement) {
  const auto value = Version(version);
  if (requirement == "*")
    return true;
  if (requirement.starts_with('^')) {
    const auto lower = Version(requirement.substr(1));
    auto upper = lower;
    if (lower[0]) { ++upper[0]; upper[1] = upper[2] = 0; }
    else if (lower[1]) { ++upper[1]; upper[2] = 0; }
    else ++upper[2];
    return value >= lower && value < upper;
  }
  return value == Version(requirement);
}

Manifest Manifest::Read(const fs::path& folder, bool contents, std::stop_token stop) {
  Manifest m;
  m.folder = fs::canonical(folder);
  m.source = Json::parse(ReadText(m.folder / "mod.json", 256 * 1024));
  if (!m.source.is_object())
    throw Failure("invalid_manifest", "mod.json must be an object");
  m.name = m.source.at("name").get<std::string>();
  m.id = m.source.value("id", "");
  // Earlier file-only projects remain importable. Assign a deterministic id.
  if (!m.source.contains("id")) {
    sha256::SHA256 hash;
    m.id = "legacy." + hash(m.name).substr(0, 16);
  }
  if (!Identifier(m.id) || m.name.empty() || m.name.size() > 160)
    throw Failure("invalid_manifest", "A mod needs a valid id and name");
  m.version = m.source.value("version", "1.0.0");
  if (m.version == "1.0" && !m.source.contains("api")) m.version = "1.0.0";
  Version(m.version);
  if (m.source.contains("api") && (!m.source.at("api").is_number_integer() ||
      m.source.at("api") != SIM_MOD_ABI_VERSION))
    throw Failure("abi_mismatch", "This mod needs a different Modkit API version");
  m.author = m.source.value("author", "");
  m.description = m.source.value("description", "");
  m.kind = m.source.value("kind", "data");
  static const auto catalog = Json::parse(kModkitApiCatalog);
  for (const auto* field : {"requires_operations", "requires_events"}) {
    const auto required = m.source.value(field, Json::array());
    if (!required.is_array() || required.size() > 128)
      throw Failure("invalid_manifest", std::string(field) + " needs at most 128 unique API names");
    std::set<std::string> seen;
    for (const auto& value : required) {
      if (!value.is_string())
        throw Failure("invalid_manifest", std::string(field) + " needs API names");
      const auto name = value.get<std::string>();
      if (!Identifier(name) || name.size() > 80 || !seen.insert(name).second)
        throw Failure("invalid_manifest", std::string(field) + " needs unique API names");
      const bool available = std::string_view(field) == "requires_events"
          ? catalog.at("events").contains(name)
          : std::any_of(catalog.at("operations").begin(), catalog.at("operations").end(),
                        [&](const auto& operation) { return operation.at("name") == name; });
      if (!available)
        throw Failure("capability_missing", m.name + " needs a newer game build with " + name);
    }
  }
  m.entry = m.source.value("entry", "");
  m.save_mode = m.source.value("save_mode", m.kind == "data" ? "stateless" : "checkpoint");
  if (m.kind != "lua" && m.kind != "native" && m.kind != "data")
    throw Failure("invalid_manifest", "Mod kind must be data, lua or native");
  if (m.save_mode != "stateless" && m.save_mode != "state" && m.save_mode != "checkpoint")
    throw Failure("invalid_manifest", "save_mode must be stateless, state or checkpoint");
  if (m.source.contains("dependencies"))
    m.dependencies = m.source.at("dependencies").get<decltype(m.dependencies)>();
  for (const auto& [id, version] : m.dependencies) {
    if (!Identifier(id)) throw Failure("invalid_manifest", "Invalid dependency id");
    VersionMatches("1.0.0", version);
  }
  m.conflicts = m.source.value("conflicts", std::vector<std::string>{});
  for (const auto& id : m.conflicts)
    if (!Identifier(id)) throw Failure("invalid_manifest", "Invalid conflicting mod id");
  if (m.source.contains("binaries"))
    m.binaries = m.source.at("binaries").get<decltype(m.binaries)>();
  if (m.kind == "lua") {
    const auto entry = Inside(m.folder, m.entry);
    if (contents && !fs::is_regular_file(entry))
      throw Failure("missing_entry", "Cannot find this mod's Lua entrypoint");
  }
  for (const auto& [target, path] : m.binaries) {
    if (target != "windows-x64" && target != "linux-x64" && target != "android-arm64")
      throw Failure("invalid_manifest", "Unknown native target: " + target);
    if (contents && !fs::is_regular_file(Inside(m.folder, path)))
      throw Failure("missing_entry", "Cannot find this mod's native binary");
  }
  if (contents && m.kind == "native" && m.binaries.empty())
    throw Failure("missing_entry", "A native mod needs at least one platform binary");
  if (m.source.contains("scene")) ValidateScene(m.source.at("scene"));
  if (m.source.contains("mission")) ValidateProjectDocument("mission", m.source.at("mission"));
  if (m.source.contains("model_edits")) {
    const auto path = Inside(m.folder, m.source.at("model_edits").get<std::string>());
    if (contents && !fs::is_regular_file(path))
      throw Failure("missing_entry", "Cannot find this mod's model variant catalog");
  }
  if (m.source.contains("mesh_edits")) {
    const auto path = Inside(m.folder, m.source.at("mesh_edits").get<std::string>());
    if (contents && !fs::is_regular_file(path))
      throw Failure("missing_entry", "Cannot find this mod's mesh edit catalog");
  }
  if (m.source.contains("texture_edits")) {
    const auto path = Inside(m.folder, m.source.at("texture_edits").get<std::string>());
    if (contents && !fs::is_regular_file(path))
      throw Failure("missing_entry", "Cannot find this mod's texture edit catalog");
  }
  if (contents) {
    Json inventory;
    m.fingerprint = Fingerprint(m.folder, &inventory, stop);
    m.files = std::make_shared<const std::map<std::string, std::string>>(
        inventory.get<std::map<std::string, std::string>>());
    if (m.kind == "lua")
      for (auto file = inventory.begin(); file != inventory.end(); ++file)
        if (file.key() == m.entry || file.key().ends_with(".lua"))
          m.script_hashes.emplace(file.key(), file.value().get<std::string>());
    if (fs::is_regular_file(m.folder / "checksums.json") &&
        Json::parse(ReadText(m.folder / "checksums.json", 64 * 1024 * 1024)) != inventory)
      throw Failure("checksum_mismatch", "The mod package's files do not match its checksums");
    for (const auto* field : {"model_edits", "mesh_edits", "texture_edits"}) {
      if (!m.source.contains(field))
        continue;
      const auto relative = m.source.at(field).get<std::string>();
      const bool mesh = std::string_view(field) == "mesh_edits";
      const bool texture = std::string_view(field) == "texture_edits";
      const auto text =
          ReadText(Inside(m.folder, relative), (mesh || texture ? 16 : 1) * 1024 * 1024);
      const auto found = m.files->find(relative);
      if (found == m.files->end() || sha256::SHA256{}(text.data(), text.size()) != found->second)
        throw Failure("source_changed", std::string(texture ? "Texture edits"
                                                    : mesh  ? "Mesh edits"
                                                            : "Model variants") +
                                            " changed during package validation");
      // Android package import uses this same validation entrypoint. Reject
      // malformed catalogs there, before a user can enable the package.
      if (texture)
        ValidateTextureVariants(Json::parse(text));
      else if (mesh)
        ValidateMeshVariants(Json::parse(text));
      else
        ValidateModelVariants(Json::parse(text));
    }
  }
  return m;
}

std::string Platform() {
#if defined(__ANDROID__)
  return "android-arm64";
#elif defined(_WIN32)
  return "windows-x64";
#else
  return "linux-x64";
#endif
}

std::vector<Manifest> Resolve(std::vector<Manifest> manifests,
                              const std::vector<std::string>& enabled) {
  std::map<std::string, Manifest> all;
  for (auto& m : manifests) {
    const auto id = m.id;
    if (!all.emplace(id, std::move(m)).second)
      throw Failure("duplicate_mod", "More than one installed mod has id " + id);
  }
  std::map<std::string, int> visited;
  std::set<std::string> requested;
  for (const auto& id : enabled)
    if (!Identifier(id) || !requested.insert(id).second)
      throw Failure("invalid_profile", "A mod profile needs unique valid ids");
  std::vector<std::string> order;
  std::function<void(const std::string&)> visit = [&](const auto& id) {
    if (!all.contains(id)) throw Failure("missing_dependency", "Mod is not installed: " + id);
    if (visited[id] == 1) throw Failure("dependency_cycle", "Dependency cycle at " + id);
    if (visited[id] == 2) return;
    if (std::count_if(visited.begin(), visited.end(), [](const auto& p) { return p.second == 1; }) >= 64 ||
        visited.size() > 256)
      throw Failure("resource_limit", "A mod profile is limited to 256 mods and 64 dependency levels");
    visited[id] = 1;
    for (const auto& [dependency, requirement] : all.at(id).dependencies) {
      if (!all.contains(dependency) || !VersionMatches(all.at(dependency).version, requirement))
        throw Failure("dependency_version", id + " needs " + dependency + " " + requirement);
      visit(dependency);
    }
    visited[id] = 2;
    order.push_back(id);
  };
  // First in the user's list has highest asset priority. Dependencies are
  // initialized before dependents and therefore have lower asset priority.
  for (auto it = enabled.rbegin(); it != enabled.rend(); ++it) visit(*it);
  std::vector<Manifest> result;
  for (const auto& id : order) {
    for (const auto& conflict : all.at(id).conflicts)
      if (visited[conflict] == 2)
        throw Failure("mod_conflict", id + " conflicts with " + conflict);
    result.push_back(std::move(all.at(id)));
  }
  return result;
}

Mod::Mod(Runtime& runtime, Manifest m) : manifest(std::move(m)), runtime_(runtime) {}
Json Mod::Call(std::string_view method, const Json& args) {
  ++calls;
  return runtime_.Call(*this, method, args);
}
void Mod::Message(std::string_view message) { runtime_.Message(manifest.id, message); }

}  // namespace simpsons::modkit
