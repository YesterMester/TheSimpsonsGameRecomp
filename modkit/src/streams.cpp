#include "streams.h"
#include "refpack.h"
#include "resource_id.h"
#include "models.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <set>

namespace simpsons::modkit {
namespace {
uint32_t Big(const uint8_t* p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
uint32_t Little(const uint8_t* p) {
  return uint32_t(p[3]) << 24 | uint32_t(p[2]) << 16 | uint32_t(p[1]) << 8 | p[0];
}
void Store(uint8_t* p, uint32_t value) {
  for (int i = 3; i >= 0; --i) {
    p[i] = uint8_t(value);
    value >>= 8;
  }
}
bool Packed(std::span<const uint8_t> data) {
  return data.size() >= 2 && (data[0] & 0x3E) == 0x10 && data[1] == 0xFB;
}
std::vector<uint8_t> Bytes(const fs::path& path, uint64_t offset, size_t size) {
  std::ifstream input(path, std::ios::binary);
  if (!input || !input.seekg(static_cast<std::streamoff>(offset)))
    throw Failure("read_failed", "Cannot read the stream resource");
  std::vector<uint8_t> data(size);
  if (size && !input.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(size)))
    throw Failure("truncated_stream", "Stream resource is incomplete");
  return data;
}
ResourceInfo Description(std::span<const uint8_t> data) {
  ResourceInfo info;
  if (data.size() >= 21 && Little(data.data()) == 0x716) {
    size_t end = 20;
    while (end < data.size() && data[end]) {
      if (data[end] < 32 || data[end] >= 127)
        break;
      info.name += char(data[end++]);
    }
    if (end == data.size() || data[end] != 0)
      info.name.clear();
    // sub_82711B28 reads a padded name followed by the resource GUID.
    // A shallow prefix may not contain that header, so do not invent an ID
    // from the payload or the bytes following a malformed name.
    const uint64_t guid = 20ull + Big(data.data() + 16);
    const uint64_t payload = 16ull + Big(data.data() + 12);
    const uint64_t chunk = 12ull + Little(data.data() + 4);
    if (!info.name.empty() && end < guid && guid + 20 <= payload && payload <= chunk &&
        guid + 16 <= data.size()) {
      std::array<uint32_t, 4> words;
      for (size_t i = 0; i < words.size(); ++i)
        words[i] = Big(data.data() + size_t(guid) + i * 4);
      info.resource_id = ResourceId(words);
      size_t name_at = size_t(guid + 20);
      const auto end_at = std::min<uint64_t>(payload, data.size());
      for (; name_at < end_at && data[name_at]; ++name_at) {
        if (data[name_at] < 32 || data[name_at] >= 127)
          break;
        info.native_class += char(data[name_at]);
      }
      if (name_at == end_at || data[name_at])
        info.native_class.clear();
    }
  }
  // Paths are metadata, never output filenames. A prefix can contain the
  // character just before the path, so trim it at the drive letter.
  std::string text;
  for (uint8_t c : data) {
    if (c >= 32 && c < 127)
      text += char(c);
    else {
      if (text.find("\\assets") != std::string::npos) {
        const auto drive = text.find(":\\");
        info.source = drive != std::string::npos && drive ? text.substr(drive - 1) : text;
        break;
      }
      text.clear();
    }
  }
  return info;
}
}  // namespace

std::vector<uint8_t> DecodeRefPack(std::span<const uint8_t> data, uint32_t expected,
                                   size_t prefix) {
  const auto header = simpsons::refpack::Header(data);
  const auto reject = [](simpsons::refpack::Error error) {
    throw Failure(error == simpsons::refpack::Error::truncated ? "truncated_stream" : "invalid_compression",
                  std::string(simpsons::refpack::Describe(error)));
  };
  if (!header) reject(header.error);
  if (header.size != expected)
    throw Failure("invalid_compression", "RefPack size does not match the stream index");
  const auto goal = std::min<size_t>(prefix, expected);
  std::vector<uint8_t> output(goal);
  const auto decoded = simpsons::refpack::Decode(data, output, goal == expected);
  if (!decoded) reject(decoded.error);
  return output;
}

StreamFile::StreamFile(fs::path path) : path_(std::move(path)) {
  const auto length = fs::file_size(path_);
  auto header = Bytes(path_, 0, header_.size());
  std::copy(header.begin(), header.end(), header_.begin());
  if (std::memcmp(header_.data(), "SToc", 4) || Big(header_.data() + 4) != 7)
    throw Failure("invalid_stream", "Expected an SToc version 7 stream");
  table_ = Big(header_.data() + 16);
  const size_t count = header_[8];
  if (table_ < 20 || uint64_t(table_) + count * 24 > header_.size())
    throw Failure("invalid_stream", "Stream index is outside its header");
  uint64_t offset = header_.size();
  for (size_t i = 0; i < count; ++i) {
    const auto* p = header_.data() + table_ + i * 24;
    StreamEntry entry{Big(p),      Big(p + 4),  Big(p + 8), Big(p + 12),
                      Big(p + 16), Big(p + 20), offset};
    if (entry.size > entry.allocation ||
        uint64_t(entry.read_offset) + entry.stored > entry.allocation || entry.stored % 2048 ||
        entry.stored > length - offset)
      throw Failure("invalid_stream", "Stream entry exceeds its allocation or file bounds");
    entries_.push_back(entry);
    offset += entry.stored;
  }
  if (offset != length)
    throw Failure("invalid_stream", "Stream index does not cover its file");
}

StreamFolder StreamFile::Folder() const {
  const uint32_t root = Big(header_.data() + 12);
  if (!root)
    return {};  // Child streams use their episode's retained StreamTOC.
  std::set<uint32_t> visited;
  const auto valid = [&](uint64_t at, uint64_t size) {
    return at >= 20 && at <= header_.size() && size <= header_.size() - at;
  };
  const auto reject = [] { throw Failure("invalid_stream", "Invalid stream folder metadata"); };
  std::function<StreamFolder(uint32_t)> read = [&](uint32_t at) -> StreamFolder {
    if ((at & 3) || !valid(at, 24) || visited.size() >= 32 || !visited.insert(at).second)
      reject();
    const auto* record = header_.data() + at;
    const uint64_t name = uint64_t(root) + Big(record + 4);
    if (!Big(record + 4) || !valid(name, 1))
      reject();
    StreamFolder result;
    result.prefix = Big(record);
    bool terminated = false;
    for (size_t i = size_t(name); i < header_.size() && result.name.size() < 240; ++i) {
      const auto c = header_[i];
      if (!c) { terminated = true; break; }
      if (c < 32 || c >= 127)
        reject();
      result.name += char(c);
    }
    if (!terminated || result.name.empty())
      reject();
    const uint32_t count = uint32_t(record[8]) + record[9];
    const uint64_t references = uint64_t(root) + Big(record + 16);
    if (count && ((references & 3) || !Big(record + 16) || !valid(references, count * 4ull)))
      reject();
    for (uint32_t i = 0; i < count; ++i) {
      const uint64_t target = uint64_t(root) + Big(header_.data() + references + i * 4);
      if ((target & 3) || !valid(target, 24))
        reject();
      if (i < record[8])
        result.children.push_back(read(uint32_t(target)));
      else
        result.dependencies.push_back(Big(header_.data() + target));
    }
    return result;
  };
  return read(root);
}

std::vector<uint8_t> StreamFile::Prefix(size_t index, size_t length) const {
  const auto& entry = entries_.at(index);
  // Even literal-heavy RefPack commands need less input than this to produce
  // a small prefix. Keep full resource reads on the explicit Read path.
  auto packed = Bytes(path_, entry.offset, std::min<size_t>(entry.stored, length * 2 + 32));
  if (Packed(packed))
    return DecodeRefPack(packed, entry.size, length);
  packed.resize(std::min<size_t>({packed.size(), length, entry.size}));
  return packed;
}
std::vector<uint8_t> StreamFile::Read(size_t index, size_t maximum) const {
  const auto& entry = entries_.at(index);
  if (entry.size > maximum || entry.stored > 512 * 1024 * 1024)
    throw Failure("resource_limit", "Stream resource exceeds the read budget");
  auto data = Bytes(path_, entry.offset, entry.stored);
  if (Packed(data))
    return DecodeRefPack(data, entry.size, entry.size);
  if (data.size() < entry.size)
    throw Failure("invalid_stream", "Uncompressed resource is incomplete");
  data.resize(entry.size);
  return data;
}
ResourceInfo StreamFile::Describe(size_t index) const {
  const auto data = Prefix(index, 1024);
  return Description(data);
}
std::vector<ResourceInfo> StreamFile::DescribeAll(size_t index) const {
  const auto data = Read(index);
  std::vector<ResourceInfo> result;
  size_t offset = 0;
  while (offset + 12 <= data.size()) {
    const auto type = Little(data.data() + offset);
    const auto length = Little(data.data() + offset + 4);
    if (uint64_t(offset) + 12 + length > data.size()) {
      if (offset == 0)
        return {};  // Opaque non-RenderWare entry.
      throw Failure("invalid_resource", "RenderWare chunk exceeds its stream entry");
    }
    const auto size = size_t(length) + 12;
    auto info = Description(std::span(data).subspan(offset, std::min<size_t>(size, 1024)));
    info.offset = uint32_t(offset);
    info.size = uint32_t(size);
    info.type = type;
    if (type == 0x722) {
      const auto body = std::span(data).subspan(offset + 12, length);
      struct Memory {
        std::span<const uint8_t> bytes;
        bool Has(uint32_t at, uint32_t count) const {
          return uint64_t(at) + count <= bytes.size();
        }
        uint32_t word(uint32_t at) const { return Big(bytes.data() + at); }
        uint8_t byte(uint32_t at) const { return bytes[at]; }
      } memory{body};
      if (!memory.Has(0, 48) || memory.word(8) != 0x53696D47 || memory.word(12) != 1)
        throw Failure("invalid_object", "Expected a SimG version 1 object layer");
      const auto count = memory.word(24), table = memory.word(40);
      if (count > 16384 || (count && (!table || (table & 3) || !memory.Has(table, count * 4))))
        throw Failure("invalid_object", "Object definition table exceeds its layer");
      for (uint32_t i = 0; i < count; ++i) {
        const auto record = memory.word(table + i * 4);
        const auto object = compiled_objects::Read(memory, record);
        if (!object)
          throw Failure("unsupported_object", "Object layer has an unsupported compiled definition");
        info.objects.push_back(*object);
      }
    } else if (info.native_class == "MetaModel") {
      const ModelResource model(std::span(data).subspan(offset, size));
      const auto description = model.Definition().Describe();
      for (const auto& reference : description.at("references"))
        if (reference.at("native_type") == 0x9E3791CF)
          info.references.push_back(reference.at("resource_id").get<std::string>());
    }
    result.push_back(std::move(info));
    offset += size;
  }
  if (offset != data.size() &&
      !std::all_of(data.begin() + offset, data.end(), [](auto c) { return !c; }))
    throw Failure("invalid_resource", "Unexpected bytes after the final RenderWare chunk");
  return result;
}
void StreamFile::Rebuild(const fs::path& destination,
                         const std::map<size_t, std::vector<uint8_t>>& replacements) const {
  if (fs::weakly_canonical(destination) == fs::canonical(path_))
    throw Failure("invalid_path", "Write rebuilt streams to a new file");
  if (fs::exists(destination))
    throw Failure("invalid_path", "Choose a new output file");
  for (const auto& [index, bytes] : replacements)
    if (index >= entries_.size() || bytes.empty() || bytes.size() > 512 * 1024 * 1024)
      throw Failure("invalid_argument", "Invalid replacement stream resource");
  auto header = header_;
  fs::create_directories(destination.parent_path());
  const auto temporary_folder =
      destination.parent_path() /
      (".modkit-stream-" +
       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  if (!fs::create_directory(temporary_folder))
    throw Failure("write_failed", "Cannot create a private rebuilding folder");
  const auto temporary = temporary_folder / "stream";
  try {
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    std::ifstream input(path_, std::ios::binary);
    if (!input || !output)
      throw Failure("write_failed", "Cannot open stream files for rebuilding");
    output.write(reinterpret_cast<const char*>(header.data()), header.size());
    const std::array<char, 2048> zeros{};
    for (size_t i = 0; i < entries_.size(); ++i) {
      const auto replacement = replacements.find(i);
      if (replacement == replacements.end()) {
        input.seekg(static_cast<std::streamoff>(entries_[i].offset));
        size_t left = entries_[i].stored;
        std::array<char, 65536> buffer;
        while (left) {
          const auto size = std::min(left, buffer.size());
          if (!input.read(buffer.data(), size))
            throw Failure("read_failed", "Stream changed while rebuilding");
          output.write(buffer.data(), size);
          left -= size;
        }
      } else {
        const bool packed = Packed(Bytes(path_, entries_[i].offset,
                                         std::min<size_t>(2, entries_[i].stored)));
        uint32_t read_offset = 0;
        const auto encoded = packed ? EncodeRefPack(replacement->second, &read_offset)
                                    : std::vector<uint8_t>{};
        const auto& bytes = packed ? encoded : replacement->second;
        const auto stored = (bytes.size() + 2047) & ~size_t(2047);
        const auto allocation = (std::max(replacement->second.size(), stored + read_offset) + 255) &
                                ~size_t(255);
        if (stored > 512 * 1024 * 1024 || allocation > 512 * 1024 * 1024)
          throw Failure("resource_limit", "Encoded resource allocation exceeds 512 MiB");
        auto* p = header.data() + table_ + i * 24;
        Store(p + 8, uint32_t(replacement->second.size()));
        Store(p + 12, uint32_t(allocation));
        Store(p + 16, uint32_t(stored));
        Store(p + 20, read_offset);
        output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        const auto padding = (2048 - bytes.size() % 2048) % 2048;
        output.write(zeros.data(), padding);
      }
    }
    output.seekp(0);
    output.write(reinterpret_cast<const char*>(header.data()), header.size());
    if (!output.flush())
      throw Failure("write_failed", "Cannot write the rebuilt stream");
    output.close();
    fs::rename(temporary, destination);
    fs::remove(temporary_folder);
  } catch (...) {
    std::error_code error;
    fs::remove_all(temporary_folder, error);
    throw;
  }
}

}  // namespace simpsons::modkit
