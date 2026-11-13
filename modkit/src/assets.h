#pragma once

#include "runtime.h"

#include <mutex>
#include <stop_token>
#include <string_view>

namespace simpsons::modkit {

struct AssetLocation {
  std::string stream;
  uint32_t entry = 0, offset = 0, bytes = 0;
  bool container = true;
  std::string resource_id;
};
struct Asset {
  std::string id, name, source, category;
  std::vector<AssetLocation> locations;
  std::string native_name, native_class;
  std::vector<size_t> objects;
};
struct AssetObject {
  std::string guid, model_resource, character_resource, stream;
  uint32_t hash = 0, entry = 0, offset = 0;
  std::string fallback_resource;
};
struct ObjectStream {
  std::string path;
  uint32_t prefix = 0;
  uint64_t allocation = 0;
  size_t chunks = 0;
};
struct AssetSnapshot {
  bool ready = false;
  size_t files = 0, total_files = 0, chunks = 0;
  std::vector<Asset> assets;
  std::map<std::string, size_t> categories;
  std::vector<std::string> errors;
  // Only unambiguous identities may label a live native model. One ID can
  // have several archive locations; distinct source assets remain ambiguous.
  std::map<std::string, size_t> native_resources;
  const Asset* FindNative(std::string_view resource_id) const;
  std::vector<ObjectStream> object_streams;
  std::vector<AssetObject> objects;
};

// Call Get when a creator actually opens the library. The worker never calls
// guest code or changes resource residency. A catalog entry is an asset
// identity, not a live entity template and not permission to spawn it.
class AssetIndex {
 public:
  explicit AssetIndex(std::vector<fs::path> roots);
  ~AssetIndex();
  std::shared_ptr<const AssetSnapshot> Get();
  // Inspect an existing scan without starting disk work from a game query.
  std::shared_ptr<const AssetSnapshot> Peek();

 private:
  void Scan(std::stop_token stop);
  void Publish(std::shared_ptr<const AssetSnapshot> snapshot);
  std::vector<fs::path> roots_;
  std::shared_ptr<const AssetSnapshot> snapshot_;
  std::mutex start_, snapshot_mutex_;
  std::jthread worker_;
};

}  // namespace simpsons::modkit
