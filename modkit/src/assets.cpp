#include "assets.h"
#include "streams.h"
#include "asset_names.h"
#include "resource_id.h"

#include <algorithm>
#include <limits>

namespace simpsons::modkit {
namespace {
std::string Lower(std::string text) {
  for (char& c : text)
    if (c >= 'A' && c <= 'Z')
      c += 'a' - 'A';
  return text;
}
std::string Category(const std::string& name) {
  const auto dot = name.rfind('.');
  const auto extension = dot == std::string::npos ? "" : Lower(name.substr(dot + 1));
  if (extension == "cha")
    return "Characters";
  if (extension == "dff")
    return "Models";
  if (extension == "bnk" || extension == "amx")
    return "Animations";
  if (extension == "vfx")
    return "Effects";
  if (extension == "itxd")
    return "Textures";
  if (extension == "rws" || extension == "bsp")
    return "Worlds";
  if (extension == "hkt" || extension == "hko")
    return "Physics";
  if (extension == "auc" || extension == "mix" || extension == "snu" || extension == "mus")
    return "Audio";
  if (extension == "lh2")
    return "Text";
  if (extension == "vp6")
    return "Movies";
  if (extension == "xml" || extension == "lua")
    return "Scripts and events";
  return "Other resources";
}
}  // namespace

const Asset* AssetSnapshot::FindNative(std::string_view resource_id) const {
  const auto found = native_resources.find(std::string(resource_id));
  return found != native_resources.end() && found->second < assets.size() ? &assets[found->second]
                                                                          : nullptr;
}

AssetIndex::AssetIndex(std::vector<fs::path> roots) : roots_(std::move(roots)) {
  snapshot_ = std::make_shared<AssetSnapshot>();
}
AssetIndex::~AssetIndex() {
  if (worker_.joinable())
    worker_.request_stop();
  // jthread joins before the state used by Scan is destroyed.
}
std::shared_ptr<const AssetSnapshot> AssetIndex::Get() {
  std::lock_guard lock(start_);
  if (!worker_.joinable())
    worker_ = std::jthread([this](std::stop_token stop) { Scan(stop); });
  return Peek();
}
std::shared_ptr<const AssetSnapshot> AssetIndex::Peek() {
  std::lock_guard snapshot_lock(snapshot_mutex_);
  return snapshot_;
}
void AssetIndex::Publish(std::shared_ptr<const AssetSnapshot> snapshot) {
  std::lock_guard lock(snapshot_mutex_);
  snapshot_ = std::move(snapshot);
}
void AssetIndex::Scan(std::stop_token stop) {
  auto state = std::make_shared<AssetSnapshot>();
  const auto progress = [&] {
    // Partial catalogs are not usable yet. Publishing their growing object
    // vectors for every eight loose files can copy gigabytes of strings.
    auto update = std::make_shared<AssetSnapshot>();
    update->files = state->files;
    update->total_files = state->total_files;
    update->chunks = state->chunks;
    update->errors = state->errors;
    Publish(std::move(update));
  };
  const auto error = [&](const std::string& text) {
    if (state->errors.size() < 128)
      state->errors.push_back(text);
  };
  try {
    // Roots have the same high-to-low priority as the game's merged mount.
    std::map<std::string, std::pair<fs::path, std::string>> files;
    for (const auto& root : roots_) {
      if (!fs::is_directory(root))
        continue;
      for (auto it = fs::recursive_directory_iterator(root);
           it != fs::recursive_directory_iterator(); ++it) {
        if (stop.stop_requested())
          return;
        if (it->is_symlink()) {
          if (it->is_directory())
            it.disable_recursion_pending();
          continue;
        }
        if (!it->is_regular_file())
          continue;
        const auto relative = Utf8(it->path().lexically_relative(root));
        if (Lower(Utf8(it->path().filename())) == "default.xex")
          continue;
        files.try_emplace(Lower(relative), it->path(), relative);
        if (files.size() > 100000)
          throw Failure("resource_limit", "The asset catalog exceeds 100000 streams");
      }
    }
    state->total_files = files.size();
    progress();
    std::map<std::string, Asset> assets;
    std::map<std::string, std::vector<std::string>> model_meshes;
    for (const auto& [key, file] : files) {
      if (stop.stop_requested())
        return;
      try {
        if (Lower(Utf8(file.first.extension())) != ".str") {
          Asset asset;
          asset.name = Utf8(file.first.filename());
          asset.source = file.second;
          asset.category = Category(asset.name);
          const auto size = fs::file_size(file.first);
          asset.locations.push_back(
              {file.second, 0, 0, uint32_t(std::min<uintmax_t>(size, UINT32_MAX)), false});
          assets.emplace("file:" + key, std::move(asset));
          ++state->files;
          continue;
        }
        StreamFile stream(file.first);
        ObjectStream objects{file.second};
        for (const auto& entry : stream.entries())
          objects.allocation += entry.allocation;
        for (size_t entry = 0; entry < stream.entries().size(); ++entry) {
          if (stop.stop_requested())
            return;
          for (const auto& resource : stream.DescribeAll(entry)) {
            ++state->chunks;
            if (resource.type == 0x722)
              ++objects.chunks;
            const auto id = [](const auto& guid) {
              return std::any_of(guid.begin(), guid.end(), [](auto word) { return word != 0; })
                         ? ResourceId(guid) : std::string();
            };
            for (const auto& definition : resource.objects) {
              if (state->objects.size() >= 400000)
                throw Failure("resource_limit", "The object catalog exceeds 400000 definitions");
              state->objects.push_back({ResourceId(definition.guid), id(definition.model),
                                         id(definition.character), file.second, definition.hash,
                                         uint32_t(entry), resource.offset + 12 + definition.offset,
                                         id(definition.fallback_model)});
            }
            if (resource.native_class == "MetaModel") {
              const auto [found, inserted] = model_meshes.try_emplace(resource.resource_id, resource.references);
              if (!inserted && found->second != resource.references)
                found->second.clear();
            }
            if (resource.name.empty())
              continue;
            const auto identity = Lower(resource.name + '\n' + resource.source);
            auto& asset = assets[identity];
            if (asset.name.empty()) {
              asset.name = resource.name;
              asset.source = resource.source;
              asset.category = Category(resource.name);
              asset.native_name = resource.name;
              asset.native_class = resource.native_class;
              if (resource.native_class == "MetaModel")
                asset.category = "Objects and characters";
              if (asset.category == "Objects and characters" || asset.category == "Models" ||
                  asset.category == "Characters")
                if (auto name = FriendlyAssetName(resource.name); !name.empty())
                  asset.name = std::move(name);
            }
            asset.locations.push_back({file.second, uint32_t(entry), resource.offset, resource.size,
                                       true, resource.resource_id});
            if (state->chunks > 1000000)
              throw Failure("resource_limit", "The asset catalog exceeds one million chunks");
          }
          std::this_thread::yield();
        }
        if (objects.chunks) {
          objects.prefix = stream.Folder().prefix;
          state->object_streams.push_back(std::move(objects));
        }
      } catch (const Failure& failure) {
        if (failure.code == "resource_limit")
          throw;
        error(file.second + ": " + failure.what());
      } catch (const std::exception& exception) {
        error(file.second + ": " + exception.what());
      }
      ++state->files;
      if (state->files % 8 == 0)
        progress();
    }
    std::map<std::string, std::vector<size_t>> resource_objects;
    for (auto& object : state->objects)
      if (object.model_resource.empty() && model_meshes.contains(object.fallback_resource))
        object.model_resource = object.fallback_resource;
    for (size_t i = 0; i < state->objects.size(); ++i) {
      const auto& object = state->objects[i];
      if (!object.model_resource.empty()) {
        resource_objects[object.model_resource].push_back(i);
        if (const auto meshes = model_meshes.find(object.model_resource); meshes != model_meshes.end())
          for (const auto& mesh : meshes->second)
            resource_objects[mesh].push_back(i);
      }
      if (!object.character_resource.empty())
        resource_objects[object.character_resource].push_back(i);
    }
    for (auto& [identity, asset] : assets) {
      asset.id = "asset:" + std::to_string(state->assets.size());
      for (const auto& location : asset.locations) {
        if (location.resource_id.empty())
          continue;
        auto [found, inserted] =
            state->native_resources.try_emplace(location.resource_id, state->assets.size());
        if (!inserted && found->second != state->assets.size())
          found->second = std::numeric_limits<size_t>::max();
        if (const auto objects = resource_objects.find(location.resource_id); objects != resource_objects.end())
          asset.objects.insert(asset.objects.end(), objects->second.begin(), objects->second.end());
      }
      std::sort(asset.objects.begin(), asset.objects.end());
      asset.objects.erase(std::unique(asset.objects.begin(), asset.objects.end()), asset.objects.end());
      ++state->categories[asset.category];
      state->assets.push_back(std::move(asset));
    }
  } catch (const std::exception& exception) {
    error(exception.what());
  }
  state->ready = true;
  Publish(std::move(state));
}

}  // namespace simpsons::modkit
