#include "modkit_filesystem.h"
#include "streams.h"

#include <algorithm>
#include <map>
#include <memory>
#include <stdexcept>

#include <rex/filesystem.h>
#include <rex/filesystem/devices/host_path_device.h>
#include <rex/filesystem/devices/host_path_entry.h>
#include <rex/filesystem/vfs.h>
#include <rex/string/utf8.h>

namespace {
namespace fs = std::filesystem;
using rex::filesystem::HostPathEntry;
using rex::X_STATUS;
using simpsons::modkit::StreamPatch;

class CatalogFile final : public rex::filesystem::File {
 public:
  CatalogFile(File* source, std::shared_ptr<const std::vector<StreamPatch>> patches)
      : File(source->file_access(), source->entry()), source_(source), patches_(std::move(patches)) {}
  ~CatalogFile() override { source_->Destroy(); }
  void Destroy() override { delete this; }
  X_STATUS ReadSync(std::span<uint8_t> buffer, size_t offset, size_t* read) override {
    *read = 0;
    const auto status = source_->ReadSync(buffer, offset, read);
    if (*read <= buffer.size())
      simpsons::modkit::ApplyStreamPatches(*patches_, buffer.first(*read), offset);
    return status;
  }
  X_STATUS WriteSync(std::span<const uint8_t>, size_t, size_t* written) override {
    *written = 0;
    return X_STATUS_ACCESS_DENIED;
  }
  X_STATUS SetLength(size_t) override { return X_STATUS_ACCESS_DENIED; }

 private:
  File* source_;
  std::shared_ptr<const std::vector<StreamPatch>> patches_;
};

class ModFileEntry final : public HostPathEntry {
 public:
  ModFileEntry(rex::filesystem::Device* device, rex::filesystem::Entry* parent, std::string_view path,
           const fs::path& host)
      : HostPathEntry(device, parent, path, host) {
    rex::filesystem::FileInfo info;
    if (!rex::filesystem::GetInfo(host, &info))
      throw std::runtime_error("Cannot read mod filesystem metadata");
    const bool directory = info.type == rex::filesystem::FileInfo::Type::kDirectory;
    attributes_ = (directory ? rex::filesystem::kFileAttributeDirectory
                             : rex::filesystem::kFileAttributeNormal) |
                  rex::filesystem::kFileAttributeReadOnly;
    size_ = directory ? 0 : info.total_size;
    allocation_size_ = (size_ + 511) & ~size_t(511);
    create_timestamp_ = info.create_timestamp;
    access_timestamp_ = info.access_timestamp;
    write_timestamp_ = info.write_timestamp;
  }
  void Patch(std::vector<StreamPatch> patches) {
    patches_ = std::make_shared<const std::vector<StreamPatch>>(std::move(patches));
  }
  X_STATUS Open(uint32_t access, rex::filesystem::File** out) override {
    const auto status = HostPathEntry::Open(access, out);
    if (status == X_STATUS_SUCCESS && patches_) {
      try {
        *out = new CatalogFile(*out, patches_);
      } catch (...) {
        (*out)->Destroy();
        *out = nullptr;
        throw;
      }
    }
    return status;
  }
  bool can_map() const override { return !patches_; }
  std::unique_ptr<rex::memory::MappedMemory> OpenMapped(rex::memory::MappedMemory::Mode mode,
                                                      size_t offset, size_t length) override {
    // Catalogs are read through File. Returning the unmodified host mapping
    // here would expose the old reservations through a second access path.
    return patches_ ? nullptr : HostPathEntry::OpenMapped(mode, offset, length);
  }

 private:
  std::shared_ptr<const std::vector<StreamPatch>> patches_;
};

class DirectoryFile final : public rex::filesystem::File {
 public:
  using File::File;
  void Destroy() override { delete this; }
  X_STATUS ReadSync(std::span<uint8_t>, size_t, size_t* read) override {
    *read = 0;
    return X_STATUS_FILE_IS_A_DIRECTORY;
  }
  X_STATUS WriteSync(std::span<const uint8_t>, size_t, size_t* written) override {
    *written = 0;
    return X_STATUS_ACCESS_DENIED;
  }
  X_STATUS SetLength(size_t) override { return X_STATUS_FILE_IS_A_DIRECTORY; }
};

// A merged directory has no single host path. HostPathEntry parents trigger
// VFS's stale-file check against parent.host_path()/child.name(), which would
// delete valid base-game children absent from the winning mod's directory.
class ModDirectory final : public rex::filesystem::Entry {
 public:
  ModDirectory(rex::filesystem::Device* device, ModDirectory* parent, std::string_view path,
               const fs::path& metadata)
      : Entry(device, parent, path) {
    rex::filesystem::FileInfo info;
    if (!rex::filesystem::GetInfo(metadata, &info))
      throw std::runtime_error("Cannot read mod directory metadata");
    attributes_ = rex::filesystem::kFileAttributeDirectory | rex::filesystem::kFileAttributeReadOnly;
    create_timestamp_ = info.create_timestamp;
    access_timestamp_ = info.access_timestamp;
    write_timestamp_ = info.write_timestamp;
  }
  void Add(std::unique_ptr<Entry> entry) { children_.push_back(std::move(entry)); }
  X_STATUS Open(uint32_t access, rex::filesystem::File** out) override {
    *out = new DirectoryFile(access, this);
    return X_STATUS_SUCCESS;
  }
};

class ModDevice final : public rex::filesystem::HostPathDevice {
 public:
  ModDevice(std::string_view mount, const fs::path& game, std::vector<fs::path> roots)
      : HostPathDevice(mount, game, true), roots_(std::move(roots)) {
    roots_.push_back(game);
  }
  bool Initialize() override {
    root_ = std::make_unique<ModDirectory>(this, nullptr, "", host_path());
    Populate(*root_, roots_, 0);
    auto catalogs = simpsons::modkit::SynchronizeStreamCatalogs(streams_);
    for (auto& [path, patches] : catalogs.patches) entries_.at(path)->Patch(std::move(patches));
    return true;
  }
  rex::filesystem::Entry* ResolvePath(std::string_view path) override {
    // Match the original guest filesystem's case-insensitive lookup.
    return root_->ResolvePath(path);
  }
  void Dump(rex::string::StringBuffer* text) override { root_->Dump(text, 0); }

 private:
  void Populate(ModDirectory& parent, const std::vector<fs::path>& directories, unsigned depth) {
    if (depth > 64) throw std::runtime_error("Mod directory tree is too deep");
    struct Candidate { std::string name; fs::path winner; bool directory; std::vector<fs::path> layers; };
    std::map<std::string, Candidate> entries;
    for (const auto& directory : directories) {
      std::vector<fs::directory_entry> sorted;
      for (const auto& entry : fs::directory_iterator(directory)) sorted.push_back(entry);
      std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.path() < b.path(); });
      for (const auto& entry : sorted) {
        if (!entry.is_directory() && !entry.is_regular_file()) continue;
        const std::string name = rex::path_to_utf8(entry.path().filename());
        const std::string key = rex::string::utf8_lower_ascii(name);
        // The executable must always match the statically recompiled code.
        if (depth == 0 && key == "default.xex" && directory != host_path())
          throw std::runtime_error("A mod cannot replace default.xex");
        auto [it, inserted] = entries.try_emplace(key, Candidate{name, entry.path(), entry.is_directory(), {}});
        if (it->second.directory && entry.is_directory()) it->second.layers.push_back(entry.path());
      }
    }
    for (auto& [key, candidate] : entries) {
      const auto path = rex::string::utf8_join_guest_paths(parent.path(), candidate.name);
      if (candidate.directory) {
        auto child = std::make_unique<ModDirectory>(this, &parent, path, candidate.winner);
        Populate(*child, candidate.layers, depth + 1);
        parent.Add(std::move(child));
      } else {
        auto child = std::make_unique<ModFileEntry>(this, &parent, path, candidate.winner);
        if (rex::string::utf8_lower_ascii(rex::path_to_utf8(candidate.winner.extension())) == ".str") {
          const auto key = simpsons::modkit::StreamKey(std::string(path));
          streams_.emplace(key, candidate.winner);
          entries_.emplace(key, child.get());
        }
        parent.Add(std::move(child));
      }
    }
  }
  std::vector<fs::path> roots_;
  std::map<std::string, fs::path> streams_;
  std::map<std::string, ModFileEntry*> entries_;
  std::unique_ptr<ModDirectory> root_;
};
}  // namespace

void MountModFiles(rex::filesystem::VirtualFileSystem* vfs, const std::filesystem::path& game,
                   const std::vector<std::filesystem::path>& roots) {
  if (roots.empty()) return;
  constexpr std::string_view mount = "\\Device\\SimpsonsModkitContent";
  auto device = std::make_unique<ModDevice>(mount, std::filesystem::absolute(game), roots);
  device->Initialize();
  // Keep the original device alive: kernel services can retain its entries.
  // The VFS takes the first matching device prefix, so appending a Partition1
  // replacement behind the Harddisk0 null device would hide it. A distinct
  // mount plus a device-path alias covers both absolute opens and game:/d:.
  if (!vfs->RegisterDevice(std::move(device)) ||
      !vfs->RegisterSymbolicLink("\\Device\\Harddisk0\\Partition1", mount))
    throw std::runtime_error("Cannot mount the mod filesystem");
  auto* executable = vfs->ResolvePath("game:\\default.xex");
  if (!executable || !executable->size())
    throw std::runtime_error("The merged mod filesystem cannot find the original game executable");
}
