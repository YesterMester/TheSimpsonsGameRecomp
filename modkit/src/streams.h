#pragma once

#include "runtime.h"
#include "compiled_objects.h"

#include <array>
#include <span>

namespace simpsons::modkit {

// SToc v7, as read by the game's streaming loader. Resource payloads remain
// opaque: the container reader does not guess at mesh or entity record layouts.
struct StreamEntry {
  uint32_t pool = 0, codec = 0, size = 0, allocation = 0, stored = 0, read_offset = 0;
  uint64_t offset = 0;
};
struct ResourceInfo {
  std::string name, source;
  uint32_t offset = 0, size = 0, type = 0;
  std::string resource_id;
  std::string native_class;
  std::vector<std::string> references;
  std::vector<compiled_objects::Object> objects;
};
struct StreamFolder {
  uint32_t prefix = 0;
  std::string name;
  std::vector<StreamFolder> children;
  std::vector<uint32_t> dependencies;
};
class StreamFile {
 public:
  explicit StreamFile(fs::path path);
  const std::vector<StreamEntry>& entries() const { return entries_; }
  // The original folder loader expands this tree into 32 request slots.
  // Offsets in this metadata are relative to the root, including child names.
  StreamFolder Folder() const;
  std::vector<uint8_t> Read(size_t index, size_t maximum = 512 * 1024 * 1024) const;
  ResourceInfo Describe(size_t index) const;
  // A stream entry may contain many RenderWare resources. Deep indexing
  // decodes one entry at a time and walks the real chunk lengths.
  std::vector<ResourceInfo> DescribeAll(size_t index) const;
  // Untouched entries retain their exact packed bytes, padding and metadata.
  // Replacements are complete decoded resources and keep the original entry's
  // compression protocol, with bounded RefPack matching and in-place headroom.
  void Rebuild(const fs::path& destination,
               const std::map<size_t, std::vector<uint8_t>>& replacements = {}) const;

 private:
  std::vector<uint8_t> Prefix(size_t index, size_t length) const;
  fs::path path_;
  std::array<uint8_t, 2048> header_{};
  uint32_t table_ = 0;
  std::vector<StreamEntry> entries_;
};

// Header and LZ command sizes are checked before copying. Prefix decoding is
// for indexing only; a full Read also requires the stream's terminating command.
std::vector<uint8_t> DecodeRefPack(std::span<const uint8_t> packed, uint32_t expected,
                                   size_t prefix);
// Optional read_offset reports the aligned headroom needed to decode in place.
// The encoder uses the format's three reference commands and literal commands.
std::vector<uint8_t> EncodeRefPack(std::span<const uint8_t> decoded,
                                  uint32_t* read_offset = nullptr);

// The streamer can reserve buffers from StreamTOC before opening a child STR.
// Synchronize those reservations with the winning files of the merged mount.
// Patches apply to reads only; neither game files nor installed mods are edited.
struct StreamPatch {
  uint64_t offset = 0;
  std::vector<uint8_t> bytes;
};
struct StreamCatalogSync {
  size_t catalogs = 0, reservations = 0;
  std::map<std::string, std::vector<StreamPatch>> patches;
};
std::string StreamKey(std::string path);
StreamCatalogSync SynchronizeStreamCatalogs(const std::map<std::string, fs::path>& streams);
void ApplyStreamPatches(std::span<const StreamPatch> patches, std::span<uint8_t> data,
                       uint64_t offset);

}  // namespace simpsons::modkit
