#include "streams.h"

#include <algorithm>
#include <cstring>
#include <set>

namespace simpsons::modkit {
namespace {
class Catalog {
 public:
  explicit Catalog(std::span<const uint8_t> entry) : data_(entry) {
    if (data_.size() < 16 || Word(0) != 0x16070000 ||
        (uint32_t(data_[4]) | uint32_t(data_[5]) << 8 | uint32_t(data_[6]) << 16 |
         uint32_t(data_[7]) << 24) != data_.size() - 12)
      throw Failure("invalid_stream_catalog", "Invalid StreamTOC resource wrapper");
    const uint64_t payload = uint64_t(Word(12)) + 16;
    if (payload > data_.size())
      throw Failure("invalid_stream_catalog", "StreamTOC payload is outside its resource");
    base_ = size_t(payload);
    if (Word(base_) != 0x9CBA7B28 || Word(base_ + 4) != 9)
      throw Failure("invalid_stream_catalog", "Expected a version 9 StreamTOC");
    Range(base_, 28);
    records = size_t(data_[base_ + 16]) << 8 | data_[base_ + 17];
    Range(base_ + 28, records * 24);
    prefix = String(Word(base_ + 8));
    pool_count_ = Word(base_ + 20);
    if (pool_count_ > 256)
      throw Failure("invalid_stream_catalog", "StreamTOC has too many allocation pools");
    pools_ = Pointer(base_ + 24, pool_count_ * 4);
  }

  size_t Record(size_t index) const { return base_ + 28 + index * 24; }
  std::string Name(size_t record) const { return String(Word(record + 4)); }
  size_t Count(size_t record) const { return data_[record + 10]; }
  size_t Blocks(size_t record) const { return Pointer(record + 20, Count(record) * 3); }
  std::array<uint8_t, 3> Reservation(const StreamEntry& entry) const {
    // sub_82728398 reconstructs two big-endian bytes, then shifts by 8.
    const uint64_t allocation = (uint64_t(entry.allocation) + 255) & ~uint64_t(255);
    if (allocation > 0xFFFF00)
      throw Failure("resource_limit", "StreamTOC reservations are limited to 16776960 bytes");
    for (size_t pool = 0; pool < pool_count_; ++pool)
      if (Word(pools_ + pool * 4) == entry.pool)
        return {uint8_t(allocation >> 16), uint8_t(allocation >> 8), uint8_t(pool)};
    throw Failure("invalid_stream_catalog", "The replacement uses a pool absent from StreamTOC");
  }

  size_t records = 0;
  std::string prefix;

 private:
  void Range(size_t offset, size_t length) const {
    if (offset > data_.size() || length > data_.size() - offset)
      throw Failure("invalid_stream_catalog", "StreamTOC data is outside its resource");
  }
  uint32_t Word(size_t offset) const {
    Range(offset, 4);
    const auto* p = data_.data() + offset;
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
  }
  size_t Pointer(size_t field, size_t length) const {
    const uint64_t offset = uint64_t(base_) + Word(field);
    if (offset > data_.size())
      throw Failure("invalid_stream_catalog", "StreamTOC pointer is outside its resource");
    Range(size_t(offset), length);
    return size_t(offset);
  }
  std::string String(uint32_t relative) const {
    const uint64_t offset = uint64_t(base_) + relative;
    if (!relative || offset >= data_.size())
      throw Failure("invalid_stream_catalog", "StreamTOC name is outside its resource");
    size_t end = size_t(offset);
    while (end < data_.size() && data_[end] && end - offset <= 1024)
      ++end;
    if (end == data_.size() || end - offset > 1024)
      throw Failure("invalid_stream_catalog", "StreamTOC name is not terminated");
    return {reinterpret_cast<const char*>(data_.data() + offset), end - size_t(offset)};
  }
  std::span<const uint8_t> data_;
  size_t base_ = 0, pools_ = 0, pool_count_ = 0;
};
}  // namespace

std::string StreamKey(std::string path) {
  for (char& c : path) {
    if (c == '\\') c = '/';
    if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
  }
  return Utf8(RelativePath(path));
}

StreamCatalogSync SynchronizeStreamCatalogs(const std::map<std::string, fs::path>& files) {
  if (files.size() > 100000)
    throw Failure("resource_limit", "The stream mount exceeds 100000 files");
  StreamCatalogSync result;
  std::map<std::string, StreamFile> streams;
  const auto stream = [&](const std::string& key) -> const StreamFile& {
    const auto file = files.find(key);
    if (file == files.end())
      throw Failure("invalid_stream_catalog", "StreamTOC refers to a missing stream: " + key);
    const auto found = streams.find(key);
    if (found != streams.end()) return found->second;
    return streams.emplace(key, StreamFile(file->second)).first->second;
  };
  for (const auto& [key, path] : files) {
    if (StreamKey(key) != key)
      throw Failure("invalid_argument", "Stream keys must use lowercase portable relative paths");
    const auto& source = stream(key);
    if (source.entries().empty() || source.Describe(0).name != "StreamTOC") continue;
    const auto entry = source.Read(0, 4 * 1024 * 1024);
    const Catalog catalog(entry);
    ++result.catalogs;
    std::set<std::string> names;
    std::map<size_t, std::array<uint8_t, 3>> patches;
    for (size_t i = 0; i < catalog.records; ++i) {
      const auto record = catalog.Record(i);
      const auto child = StreamKey(catalog.prefix + '/' + catalog.Name(record) + ".str");
      if (!names.insert(child).second)
        throw Failure("invalid_stream_catalog", "StreamTOC contains duplicate stream paths");
      const auto count = catalog.Count(record);
      // The catalog's own stream is loaded before its reservations exist.
      // It deliberately has zero preallocated blocks in the stock catalog.
      if (!count) continue;
      const auto& target = stream(child).entries();
      if (count != target.size())
        throw Failure("invalid_stream_catalog", "Replacement entry count differs from StreamTOC: " + child);
      const auto blocks = catalog.Blocks(record);
      for (size_t block = 0; block < count; ++block) {
        ++result.reservations;
        const auto wanted = catalog.Reservation(target[block]);
        const auto at = blocks + block * 3;
        const auto [it, inserted] = patches.try_emplace(at, wanted);
        if (!inserted && it->second != wanted)
          throw Failure("invalid_stream_catalog", "Aliased StreamTOC reservations disagree");
      }
    }
    for (const auto& [at, wanted] : patches) {
      if (std::equal(wanted.begin(), wanted.end(), entry.begin() + at)) continue;
      const auto& layout = source.entries()[0];
      if (layout.codec != 0x0EAC15C8 || layout.read_offset != 0)
        throw Failure("unsupported_stream_catalog", "Changed compressed StreamTOC needs a catalog rebuilder");
      auto& output = result.patches[key];
      const uint64_t offset = layout.offset + at;
      if (!output.empty() && output.back().offset + output.back().bytes.size() > offset)
        throw Failure("invalid_stream_catalog", "StreamTOC reservation fields overlap");
      output.push_back({offset, {wanted.begin(), wanted.end()}});
    }
  }
  return result;
}

void ApplyStreamPatches(std::span<const StreamPatch> patches, std::span<uint8_t> data,
                       uint64_t offset) {
  if (data.size() > UINT64_MAX - offset)
    throw Failure("invalid_argument", "Stream read exceeds the file offset range");
  const uint64_t end = offset + data.size();
  for (const auto& patch : patches) {
    if (patch.bytes.size() > UINT64_MAX - patch.offset)
      throw Failure("invalid_argument", "Stream patch exceeds the file offset range");
    const uint64_t patch_end = patch.offset + patch.bytes.size();
    const auto first = std::max(offset, patch.offset), last = std::min(end, patch_end);
    if (first >= last) continue;
    std::copy_n(patch.bytes.begin() + size_t(first - patch.offset), size_t(last - first),
                data.begin() + size_t(first - offset));
  }
}

}  // namespace simpsons::modkit
