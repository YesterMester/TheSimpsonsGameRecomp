#include "source.h"
#include "models.h"
#include "meshes.h"
#include "textures.h"

#include "sha256.h"

#include <algorithm>
#include <array>
#include <exception>
#include <fstream>
#include <set>

namespace simpsons::modkit {
namespace {
void CheckCancelled(std::stop_token stop) {
  if (stop.stop_requested())
    throw Failure("cancelled", "Mod source preparation cancelled");
}

bool Library(std::string name) {
  std::transform(name.begin(), name.end(), name.begin(),
                 [](unsigned char c) { return c >= 'A' && c <= 'Z' ? c + 'a' - 'A' : c; });
  return name.ends_with(".dll") || name.ends_with(".so") || name.find(".so.") != std::string::npos;
}

bool CodeFile(const Manifest& manifest, std::string_view path) {
  // Game scripts under the mounted asset tree belong to the game's own Lua
  // VM, not this mod's VM. They cannot be replaced by a mod-code reload.
  std::string lower(path);
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return c >= 'A' && c <= 'Z' ? c + 'a' - 'A' : c; });
  if (lower.starts_with("gamedata/") || lower.starts_with("locales/"))
    return false;
  if (manifest.kind == "lua")
    return path == manifest.entry || path.ends_with(".lua");
  if (manifest.kind != "native")
    return false;
  for (const auto& [platform, binary] : manifest.binaries)
    if (path == binary || (Utf8Path(path).parent_path() == Utf8Path(binary).parent_path() &&
                           Library(Utf8(Utf8Path(path).filename()))))
      return true;
  // Creator projects keep source with the package. Rebuilding these files is
  // safe; changing game data, locales or metadata still requires a restart.
  return path.ends_with(".cpp") || path.ends_with(".cc") || path.ends_with(".cxx") ||
         path.ends_with(".c") || path.ends_with(".h") || path.ends_with(".hpp") ||
         path == "CMakeLists.txt";
}

void CheckReload(const Manifest& old, const Manifest& next) {
  if (old.source != next.source || !old.files || !next.files)
    throw Failure("restart_required", "Mod metadata changed; restart to load " + old.name);
  for (const auto* files : {old.files.get(), next.files.get()})
    for (const auto& [path, hash] : *files) {
      if (CodeFile(old, path))
        continue;
      const auto before = old.files->find(path), after = next.files->find(path);
      if (before == old.files->end() || after == next.files->end() ||
          before->second != after->second)
        throw Failure("restart_required",
                      "Game data or translations changed; restart to load " + path);
    }
}

void CopyVerified(const fs::path& source, const fs::path& destination, std::string_view expected,
                  std::stop_token stop, uint64_t& total) {
  std::ifstream input(source, std::ios::binary);
  std::ofstream output(destination, std::ios::binary | std::ios::trunc);
  if (!input || !output)
    throw Failure("read_failed", "Cannot prepare native code: " + Utf8(source));
  sha256::SHA256 hash;
  std::array<char, 65536> buffer;
  uint64_t bytes = 0;
  while (input.read(buffer.data(), buffer.size()) || input.gcount()) {
    CheckCancelled(stop);
    const auto size = static_cast<size_t>(input.gcount());
    if (size > 64 * 1024 * 1024 - bytes || size > 256 * 1024 * 1024 - total)
      throw Failure("resource_limit",
                    "Native code is limited to 64 MiB per library and 256 MiB per mod");
    bytes += size;
    total += size;
    hash.add(buffer.data(), size);
    if (!output.write(buffer.data(), size))
      throw Failure("write_failed", "Cannot cache native code");
  }
  if (!input.eof() || !output.flush())
    throw Failure("write_failed", "Cannot finish preparing native code");
  if (hash.getHash() != expected)
    throw Failure("source_changed",
                  "Native code changed while preparing " + Utf8(source.filename()));
  output.close();
#ifndef _WIN32
  fs::permissions(destination, fs::perms::owner_read);
#endif
}

Manifest PrepareOne(const Manifest& old, const fs::path& cache, bool reload, std::stop_token stop) {
  CheckCancelled(stop);
  auto next = Manifest::Read(old.folder, true, stop);
  if (reload)
    CheckReload(old, next);
  else if (old.fingerprint != next.fingerprint)
    throw Failure("save_incompatible", "Mod source changed since launch: " + old.name);
  if (next.fingerprint == old.fingerprint) {
    next.scripts = old.scripts;
    next.native_image = old.native_image;
    next.model_edits = old.model_edits;
    next.mesh_edits = old.mesh_edits;
    next.texture_edits = old.texture_edits;
  }
  PrepareCode(next, cache, stop);
  return next;
}
}  // namespace

NativeImage::~NativeImage() {
  if (folder.empty())
    return;
  std::error_code ignored;
  fs::remove_all(folder, ignored);
}

void PrepareCode(Manifest& manifest, const fs::path& cache, std::stop_token stop) {
  CheckCancelled(stop);
  if (manifest.source.contains("texture_edits") && !manifest.texture_edits) {
    const auto path = manifest.source.at("texture_edits").get<std::string>();
    if (!manifest.files || !manifest.files->contains(path))
      throw Failure("not_prepared", "Texture edits need a verified package inventory");
    const auto text = ReadText(Inside(manifest.folder, path), 16 * 1024 * 1024);
    if (sha256::SHA256{}(text.data(), text.size()) != manifest.files->at(path))
      throw Failure("source_changed", "Texture edits changed while preparing " + path);
    manifest.texture_edits =
        std::make_shared<const Json>(ValidateTextureVariants(Json::parse(text)));
  }
  if (manifest.source.contains("mesh_edits") && !manifest.mesh_edits) {
    const auto path = manifest.source.at("mesh_edits").get<std::string>();
    if (!manifest.files || !manifest.files->contains(path))
      throw Failure("not_prepared", "Mesh edits need a verified package inventory");
    const auto text = ReadText(Inside(manifest.folder, path), 16 * 1024 * 1024);
    if (sha256::SHA256{}(text.data(), text.size()) != manifest.files->at(path))
      throw Failure("source_changed", "Mesh edits changed while preparing " + path);
    manifest.mesh_edits = std::make_shared<const Json>(ValidateMeshVariants(Json::parse(text)));
  }
  if (manifest.source.contains("model_edits") && !manifest.model_edits) {
    const auto path = manifest.source.at("model_edits").get<std::string>();
    if (!manifest.files || !manifest.files->contains(path))
      throw Failure("not_prepared", "Model variants need a verified package inventory");
    const auto text = ReadText(Inside(manifest.folder, path), 1024 * 1024);
    if (sha256::SHA256{}(text.data(), text.size()) != manifest.files->at(path))
      throw Failure("source_changed", "Model variants changed while preparing " + path);
    manifest.model_edits = std::make_shared<const Json>(ValidateModelVariants(Json::parse(text)));
  }
  if (manifest.kind == "lua" && !manifest.scripts) {
    auto sources = std::make_shared<std::map<std::string, std::string>>();
    size_t bytes = 0;
    for (const auto& [path, hash] : manifest.script_hashes) {
      CheckCancelled(stop);
      auto text = ReadText(Inside(manifest.folder, path));
      if (text.size() > 32 * 1024 * 1024 - bytes)
        throw Failure("resource_limit", "A mod's prepared Lua source is limited to 32 MiB");
      bytes += text.size();
      if (sha256::SHA256{}(text.data(), text.size()) != hash)
        throw Failure("source_changed", "Mod source changed while preparing " + path);
      sources->emplace(path, std::move(text));
    }
    if (!sources->contains(manifest.entry))
      throw Failure("missing_entry", "Lua entrypoint is absent from the verified package");
    manifest.scripts = std::move(sources);
  }
  if (manifest.kind != "native" || manifest.native_image)
    return;
  const auto found = manifest.binaries.find(Platform());
  if (found == manifest.binaries.end())
    throw Failure("unsupported_platform", manifest.name + " has no binary for " + Platform());
  if (!manifest.files || cache.empty())
    throw Failure("not_prepared", "Native code needs a verified cache");
  fs::create_directories(cache);
  auto image = std::make_shared<NativeImage>();
  static std::atomic<uint64_t> sequence{0};
  for (int attempt = 0; attempt < 32; ++attempt) {
    const auto nonce = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                       "-" + std::to_string(++sequence);
    const auto folder = cache / (manifest.id + "-" + nonce);
    if (fs::create_directory(folder)) {
      image->folder = folder;
      break;
    }
  }
  if (image->folder.empty())
    throw Failure("write_failed", "Cannot allocate a private native code folder");
#ifndef _WIN32
  fs::permissions(image->folder, fs::perms::owner_all);
#endif
  const auto entry = Utf8Path(found->second);
  uint64_t bytes = 0;
  size_t count = 0;
  for (const auto& [path, hash] : *manifest.files) {
    const auto relative = Utf8Path(path);
    if (path != found->second &&
        (relative.parent_path() != entry.parent_path() || !Library(Utf8(relative.filename()))))
      continue;
    if (++count > 256)
      throw Failure("resource_limit", "A native mod has too many sibling libraries");
    CopyVerified(Inside(manifest.folder, path), image->folder / relative.filename(), hash, stop,
                 bytes);
  }
  image->entry = image->folder / entry.filename();
  if (!fs::is_regular_file(image->entry))
    throw Failure("missing_entry", "The verified native entrypoint is missing");
  manifest.native_image = std::move(image);
}

void PreparedSources::Validate(const std::vector<Manifest>& current, bool for_reload) const {
  if (reload != for_reload || original.size() != current.size() || mods.size() != current.size())
    throw Failure("stale_preparation",
                  "The prepared mod profile no longer matches the running game");
  for (size_t i = 0; i < current.size(); ++i)
    if (original[i].id != current[i].id || original[i].folder != current[i].folder ||
        original[i].fingerprint != current[i].fingerprint || mods[i].id != current[i].id ||
        (mods[i].kind == "lua" && !mods[i].scripts) ||
        (mods[i].kind == "native" && !mods[i].native_image))
      throw Failure("stale_preparation", "Prepared mod code is stale: " + current[i].name);
}

PreparedSources PrepareSources(const std::vector<Manifest>& original, const fs::path& cache,
                               bool reload, std::stop_token stop) {
  PreparedSources result{original, {}, reload};
  for (const auto& mod : original)
    result.mods.push_back(PrepareOne(mod, cache, reload, stop));
  return result;
}

SourceJob::SourceJob(std::vector<Manifest> original, fs::path cache, bool reload,
                     fs::path checkpoint_state)
    : total_(original.size()),
      worker_([this, original = std::move(original), cache = std::move(cache),
               reload, checkpoint_state = std::move(checkpoint_state)](std::stop_token stop) {
        try {
          PreparedSources result{original, {}, reload};
          for (const auto& mod : original) {
            result.mods.push_back(PrepareOne(mod, cache, reload, stop));
            std::lock_guard lock(mutex_);
            ++completed_;
          }
          if (!checkpoint_state.empty()) {
            CheckCancelled(stop);
            result.checkpoint_prepared = true;
            if (fs::exists(checkpoint_state))
              result.checkpoint_state = Json::parse(ReadText(checkpoint_state, 16 * 1024 * 1024));
          }
          CheckCancelled(stop);
          std::lock_guard lock(mutex_);
          result_ = std::move(result);
          done_ = true;
        } catch (...) {
          std::lock_guard lock(mutex_);
          cancelled_ = stop.stop_requested();
          error_ = std::current_exception();
          done_ = true;
        }
      }) {}
SourceJob::~SourceJob() {
  Cancel();
}
void SourceJob::Cancel() {
  worker_.request_stop();
}
bool SourceJob::ready() const {
  std::lock_guard lock(mutex_);
  return done_;
}
Json SourceJob::Status() const {
  std::lock_guard lock(mutex_);
  Json state = {{"state", !done_       ? "preparing"
                          : cancelled_ ? "cancelled"
                          : error_     ? "failed"
                                       : "ready"},
                {"completed", completed_},
                {"total", total_}};
  if (error_) {
    try {
      std::rethrow_exception(error_);
    } catch (const Failure& error) {
      state["error"] = error.what();
      state["code"] = error.code;
    } catch (const std::exception& error) {
      state["error"] = error.what();
      state["code"] = "prepare_failed";
    } catch (...) {
      state["error"] = "Unknown source preparation error";
      state["code"] = "prepare_failed";
    }
  }
  return state;
}
PreparedSources SourceJob::Take() {
  std::lock_guard lock(mutex_);
  if (!done_)
    throw Failure("not_ready", "Mod source preparation is still running");
  if (error_)
    std::rethrow_exception(error_);
  if (!result_)
    throw Failure("not_ready", "The prepared code was already consumed");
  auto result = std::move(*result_);
  result_.reset();
  return result;
}
}  // namespace simpsons::modkit
