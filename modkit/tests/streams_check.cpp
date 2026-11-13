#include "assets.h"
#include "streams.h"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <random>

using namespace simpsons::modkit;
namespace {
int checks = 0;
void Require(bool ok, const char* message) {
  ++checks;
  if (!ok)
    throw std::runtime_error(message);
}
template <typename Function>
void Reject(Function function, const char* message) {
  bool failed = false;
  try {
    function();
  } catch (const std::exception&) {
    failed = true;
  }
  Require(failed, message);
}
void Big(std::vector<uint8_t>& data, size_t at, uint32_t value) {
  for (size_t i = 0; i < 4; ++i)
    data[at + i] = uint8_t(value >> ((3 - i) * 8));
}
void Little(std::vector<uint8_t>& data, size_t at, uint32_t value) {
  for (size_t i = 0; i < 4; ++i)
    data[at + i] = uint8_t(value >> (i * 8));
}
std::vector<uint8_t> Resource(std::string_view name) {
  std::vector<uint8_t> data(20 + name.size() + 1);
  Little(data, 0, 0x716);
  Little(data, 4, uint32_t(data.size() - 12));
  std::memcpy(data.data() + 20, name.data(), name.size());
  return data;
}
std::vector<uint8_t> NamedResource(std::string_view name, std::array<uint32_t, 4> guid) {
  auto data = Resource(name);
  const size_t padded = (name.size() + 4) & ~size_t(3);
  const size_t at = 20 + padded, payload = at + 20;
  data.resize(payload + 8);
  Little(data, 4, uint32_t(data.size() - 12));
  Big(data, 12, uint32_t(payload - 16));
  Big(data, 16, uint32_t(padded));
  for (size_t i = 0; i < guid.size(); ++i)
    Big(data, at + i * 4, guid[i]);
  return data;
}
std::vector<uint8_t> Container(std::span<const uint8_t> payload) {
  std::vector<uint8_t> data(4096);
  std::memcpy(data.data(), "SToc", 4);
  Big(data, 4, 7);
  data[8] = 1;
  data[9] = 4;
  Big(data, 16, 32);
  Big(data, 32, 0x5393AC01);
  Big(data, 36, 0x0EAC15C8);
  Big(data, 40, uint32_t(payload.size()));
  Big(data, 44, 2048);
  Big(data, 48, 2048);
  std::copy(payload.begin(), payload.end(), data.begin() + 2048);
  return data;
}
void Write(const fs::path& path, const std::vector<uint8_t>& data) {
  WriteText(path, {reinterpret_cast<const char*>(data.data()), data.size()});
}
std::vector<uint8_t> CatalogResource() {
  auto data = Resource("StreamTOC");
  data.resize(64 + 128);
  Little(data, 4, uint32_t(data.size() - 12));
  Big(data, 12, 48);  // Named payload starts at +16 + header length.
  Big(data, 64, 0x9CBA7B28);
  Big(data, 68, 9);
  Big(data, 72, 64);  // Prefix.
  data[81] = 1;      // One file record.
  Big(data, 84, 1);  // One allocation pool.
  Big(data, 88, 56);
  Big(data, 96, 69);  // File name, at record +4.
  data[102] = 1;     // One preallocated block, at record +10.
  Big(data, 112, 60);
  Big(data, 120, 0x5393AC01);
  data[125] = 8;     // Two-byte allocation units of 256, then pool index.
  std::memcpy(data.data() + 128, "zone", 5);
  std::memcpy(data.data() + 133, "changed", 8);
  return data;
}
}  // namespace

int main() {
  const auto root = fs::temp_directory_path() /
                    ("simpsons-stream-check-" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  try {
    const std::vector<uint8_t> packed{0x10, 0xFB, 0, 0, 7, 0xE0, 'a', 'b', 'c', 'd', 0, 0, 0xFC};
    const auto decoded = DecodeRefPack(packed, 7, 7);
    const auto encoded = EncodeRefPack(decoded);
    Require(DecodeRefPack(encoded, 7, 7) == decoded, "RefPack encoding round-trips");
    Require(std::string(decoded.begin(), decoded.end()) == "abcdddd",
            "RefPack overlapping references");
    Require(DecodeRefPack(std::span(packed).first(packed.size() - 1), 7, 4) ==
                std::vector<uint8_t>({'a', 'b', 'c', 'd'}),
            "Prefix indexing need not decode the remainder");
    Reject([&] { DecodeRefPack(std::span(packed).first(packed.size() - 1), 7, 7); },
           "Full decode requires termination");
    Reject([&] { DecodeRefPack(packed, 8, 8); }, "Header and index sizes must agree");
    Reject([&] { DecodeRefPack(std::vector<uint8_t>{0x10, 0xFB, 0, 0, 3, 0, 0, 0xFC}, 3, 3); },
           "References cannot precede output");
    Reject([&] { DecodeRefPack(std::vector<uint8_t>{0x10, 0xFB, 0, 0, 3, 0xFC}, 3, 3); },
           "Early stop is invalid");
    const std::vector<uint8_t> medium{0x10, 0xFB, 0,   0,    8, 0xE0, 'a',
                                      'b',  'c',  'd', 0x80, 0, 3,    0xFC};
    const auto doubled = DecodeRefPack(medium, 8, 8);
    Require(std::string(doubled.begin(), doubled.end()) == "abcdabcd",
            "Three-byte RefPack command");
    const std::vector<uint8_t> long_copy{0x90, 0xFB, 0,   0,    0, 13, 0xE0, 'a',
                                         'b',  'c',  'd', 0xC0, 0, 3,  4,    0xFC};
    const auto repeated = DecodeRefPack(long_copy, 13, 13);
    Require(std::string(repeated.begin(), repeated.end()) == "abcdabcdabcda",
            "Four-byte size and long RefPack reference");

    std::mt19937 random(0x514A);
    bool boundaries = true;
    for (size_t size : {0, 1, 2, 3, 4, 111, 112, 113, 114, 115, 1027, 1028, 1029,
                        131071, 131072, 131073}) {
      std::vector<uint8_t> input(size);
      for (auto& byte : input)
        byte = uint8_t(random());
      uint32_t offset = 0;
      const auto compressed = EncodeRefPack(input, &offset);
      boundaries &= DecodeRefPack(compressed, uint32_t(size), size) == input && offset >= 32 &&
                    offset % 32 == 0;
    }
    Require(boundaries, "Literal tails, command lengths and reference-window edges round-trip");
    std::vector<uint8_t> compressible(200000);
    for (size_t i = 0; i < compressible.size(); ++i)
      compressible[i] = uint8_t(i % 241);
    uint32_t headroom = 0;
    const auto compressed = EncodeRefPack(compressible, &headroom);
    Require(compressed.size() < compressible.size() / 8 &&
                DecodeRefPack(compressed, uint32_t(compressible.size()), compressible.size()) == compressible,
            "Repeated assets use references instead of expanding into literal-only data");
    Require(headroom > compressible.size() - compressed.size(),
            "In-place headroom accounts for expanded references");

    auto payload = Resource("one.dff");
    const auto two = Resource("two.cha");
    payload.insert(payload.end(), two.begin(), two.end());
    const auto original = Container(payload);
    Write(root / "game/test.str", original);
    StreamFile stream(root / "game/test.str");
    Require(stream.Read(0) == payload, "Read uncompressed stream resources");
    const auto resources = stream.DescribeAll(0);
    Require(resources.size() == 2 && resources[1].name == "two.cha",
            "Index every named resource in an entry");
    Require(resources[1].offset == Resource("one.dff").size(), "Deep index uses chunk lengths");
    Require(resources[0].resource_id.empty(), "An incomplete named header has no invented GUID");
    constexpr std::array<uint32_t, 4> native_guid{0x12345678, 0x90ABCDEF, 1, 0xFEDCBA98};
    const auto native_resource = NamedResource("replacement.dff", native_guid);
    constexpr std::string_view native_id = "1234567890ABCDEF00000001FEDCBA98";
    Write(root / "native-id.str", Container(native_resource));
    Require(StreamFile(root / "native-id.str").DescribeAll(0)[0].resource_id == native_id &&
                StreamFile(root / "native-id.str").Describe(0).resource_id == native_id,
            "Shallow and deep readers retain all sixteen big-endian resource GUID bytes");
    for (int damage = 0; damage < 3; ++damage) {
      auto malformed = native_resource;
      if (damage == 0)
        Big(malformed, 16, UINT32_MAX);
      if (damage == 1)
        Big(malformed, 12, 4);
      if (damage == 2)
        Big(malformed, 16, 1);
      Write(root / "malformed-id.str", Container(malformed));
      Require(StreamFile(root / "malformed-id.str").DescribeAll(0)[0].resource_id.empty(),
              "Invalid name and header bounds cannot lend payload bytes a resource identity");
    }
    stream.Rebuild(root / "copy.str");
    const auto copy = ReadText(root / "copy.str");
    Require(std::vector<uint8_t>(copy.begin(), copy.end()) == original,
            "An unchanged stream rebuild is byte-identical");
    const auto changed = Resource("replacement.dff");
    stream.Rebuild(root / "changed.str", {{0, changed}});
    Require(StreamFile(root / "changed.str").Read(0) == changed,
            "Replaced resources reparse with their new sizes");
    auto packed_container = Container(packed);
    Big(packed_container, 40, 7);
    Write(root / "packed.str", packed_container);
    StreamFile(root / "packed.str").Rebuild(root / "packed-replacement.str", {{0, changed}});
    StreamFile packed_replacement(root / "packed-replacement.str");
    const auto bytes = ReadText(root / "packed-replacement.str");
    Require(uint8_t(bytes[2048]) == 0x10 && uint8_t(bytes[2049]) == 0xFB,
            "Compressed game streams retain their RefPack protocol after replacement");
    Require(packed_replacement.Read(0) == changed && packed_replacement.entries()[0].read_offset >= 32,
            "Replacement allocation leaves room for in-place decompression");
    const auto& allocation = packed_replacement.entries()[0];
    Require(allocation.allocation % 256 == 0 &&
                allocation.allocation - allocation.read_offset - allocation.stored < 256,
            "Rebuilt stream allocations retain sector reads and compact aligned padding");

    const auto toc = CatalogResource();
    Write(root / "game/zone/catalog.str", Container(toc));
    auto child = Container(Resource("changed.dff"));
    Write(root / "game/zone/changed.str", child);
    const std::map<std::string, fs::path> catalog_files{
        {"zone/catalog.str", root / "game/zone/catalog.str"},
        {"zone/changed.str", root / "game/zone/changed.str"}};
    const auto unchanged_catalog = SynchronizeStreamCatalogs(catalog_files);
    Require(unchanged_catalog.catalogs == 1 && unchanged_catalog.reservations == 1 &&
                unchanged_catalog.patches.empty(),
            "Matching StreamTOC reservations require no changes");
    Big(child, 44, 4352);
    Write(root / "game/zone/changed.str", child);
    const auto sync = SynchronizeStreamCatalogs(catalog_files);
    Require(sync.patches.size() == 1 && sync.patches.at("zone/catalog.str").size() == 1,
            "Changed stream allocation updates its separate preallocation catalog");
    const auto& fields = sync.patches.at("zone/catalog.str");
    Require(fields[0].offset == 2048 + 124 && fields[0].bytes == std::vector<uint8_t>({0, 17, 0}),
            "StreamTOC allocation uses two big-endian bytes shifted by eight and its pool");
    auto read = Container(toc);
    ApplyStreamPatches(fields, read, 0);
    Require(read[2048 + 125] == 17 &&
                ReadText(root / "game/zone/catalog.str")[2048 + 125] == 8,
            "Catalog read correction preserves original host files");
    std::array<uint8_t, 1> partial{8};
    ApplyStreamPatches(fields, partial, fields[0].offset + 1);
    Require(partial[0] == 17, "Partial reads intersect a catalog patch correctly");
    partial[0] = 42;
    ApplyStreamPatches(fields, partial, fields[0].offset + 3);
    Require(partial[0] == 42, "Reads after a catalog patch keep their bytes");
    Reject([&] { ApplyStreamPatches(fields, partial, UINT64_MAX); },
           "Catalog read offsets cannot wrap");
    auto missing_stream = catalog_files;
    missing_stream.erase("zone/changed.str");
    Reject([&] { SynchronizeStreamCatalogs(missing_stream); },
           "Missing catalog dependencies fail before game startup");
    Big(child, 32, 0x12345678);
    Write(root / "game/zone/changed.str", child);
    Reject([&] { SynchronizeStreamCatalogs(catalog_files); },
           "Unregistered replacement allocation pools are rejected");
    Big(child, 32, 0x5393AC01);
    Big(child, 44, 0x1000000);
    Write(root / "game/zone/changed.str", child);
    Reject([&] { SynchronizeStreamCatalogs(catalog_files); },
           "Catalog allocation fields cannot truncate large replacements");
    auto bad_toc = toc;
    Big(bad_toc, 112, UINT32_MAX);
    Write(root / "game/zone/catalog.str", Container(bad_toc));
    Reject([&] { SynchronizeStreamCatalogs(catalog_files); },
           "Catalog pointers cannot escape the decoded resource");
    Reject([&] { StreamKey("../outside.str"); }, "Catalog paths cannot escape game data");
    Require(StreamKey("ZONE\\Changed.STR") == "zone/changed.str",
            "Catalog paths match guest case-insensitive lookup");
    fs::remove_all(root / "game/zone");
    Reject([&] { stream.Rebuild(root / "game/test.str"); },
           "Rebuilds cannot overwrite original game files");
    auto malformed = original;
    Big(malformed, 16, 2040);
    Write(root / "bad.str", malformed);
    Reject([&] { StreamFile bad(root / "bad.str"); }, "Reject a table outside the header");

    auto folder_bytes = Container(Resource("folder.dff"));
    Big(folder_bytes, 12, 80);
    Big(folder_bytes, 80, 0x12345678);
    Big(folder_bytes, 84, 80);  // Root name at 160.
    folder_bytes[88] = 1;
    Big(folder_bytes, 96, 64);  // Root-relative reference array at 144.
    Big(folder_bytes, 144, 24); // Child record at 104.
    Big(folder_bytes, 108, 100); // Child name at 180, still root-relative.
    std::memcpy(folder_bytes.data() + 160, "episode\\episode", 16);
    std::memcpy(folder_bytes.data() + 180, "shared_global", 14);
    Write(root / "folder.str", folder_bytes);
    const auto folder = StreamFile(root / "folder.str").Folder();
    Require(folder.prefix == 0x12345678 && folder.name == "episode\\episode" &&
                folder.children.size() == 1 && folder.children.front().prefix == 0 &&
                folder.children.front().name == "shared_global",
            "Folder metadata keeps zero-prefix children and uses root-relative offsets");
    auto invalid_folder = folder_bytes;
    Big(invalid_folder, 144, 0);
    Write(root / "bad-folder.str", invalid_folder);
    Reject([&] { StreamFile(root / "bad-folder.str").Folder(); },
           "Cyclic folders cannot overflow the original native request stack");
    invalid_folder = folder_bytes;
    Big(invalid_folder, 108, UINT32_MAX);
    Write(root / "bad-folder.str", invalid_folder);
    Reject([&] { StreamFile(root / "bad-folder.str").Folder(); },
           "Relative folder names cannot wrap outside the stream header");
    Require(stream.Folder().name.empty(), "Zone streams may borrow their episode catalog");
    for (uint32_t nodes : {32u, 33u}) {
      auto bounded = Container(Resource("folder.dff"));
      Big(bounded, 12, 80);
      for (uint32_t i = 0; i < nodes; ++i) {
        const auto at = 80 + i * 24, name = 1400 + i * 7, reference = 1000 + i * 4;
        Big(bounded, at + 4, name - 80);
        std::memcpy(bounded.data() + name, "folder", 7);
        if (i + 1 < nodes) {
          bounded[at + 8] = 1;
          Big(bounded, at + 16, reference - 80);
          Big(bounded, reference, (i + 1) * 24);
        }
      }
      Write(root / "bounded-folder.str", bounded);
      if (nodes == 32) {
        const auto tree = StreamFile(root / "bounded-folder.str").Folder();
        const auto* node = &tree;
        size_t depth = 1;
        while (!node->children.empty()) { node = &node->children.front(); ++depth; }
        Require(depth == 32, "All 32 native folder request slots are usable");
      } else {
        Reject([&] { StreamFile(root / "bounded-folder.str").Folder(); },
               "A 33rd folder request cannot overflow the original stack");
      }
    }
    fs::remove(root / "bounded-folder.str");
    fs::remove(root / "folder.str");
    fs::remove(root / "bad-folder.str");

    // A higher-priority mod replaces the whole stream, including its catalog.
    Write(root / "mod/test.str", Container(native_resource));
    WriteText(root / "game/voice.snu", "synthetic audio");
    AssetIndex index({root / "mod", root / "game"});
    Require(!index.Peek()->ready && index.Peek()->files == 0,
            "Peeking from a game query does not start disk indexing");
    std::shared_ptr<const AssetSnapshot> snapshot;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    do {
      snapshot = index.Get();
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    } while (!snapshot->ready && std::chrono::steady_clock::now() < deadline);
    Require(snapshot->ready && snapshot->errors.empty(), "Asset indexing completes asynchronously");
    Require(snapshot->assets.size() == 2, "Whole-file mod priority and loose audio are respected");
    Require(snapshot->categories.at("Models") == 1 && snapshot->categories.at("Audio") == 1,
            "The game-wide library groups actual asset kinds");
    Require(std::none_of(snapshot->assets.begin(), snapshot->assets.end(),
                         [](const auto& asset) {
                           return asset.name == "one.dff" || asset.name == "two.cha";
                         }),
            "An overridden stream cannot leave stale library entries");
    const auto* native = snapshot->FindNative(native_id);
    Require(native && native->name == "Replacement" && native->native_name == "replacement.dff" &&
                native->locations[0].resource_id == native_id && !snapshot->FindNative(""),
            "Friendly labels retain the exact winning native resource name and identity");
    Write(root / "game/ambiguous.str", Container(NamedResource("different.cha", native_guid)));
    AssetIndex ambiguous({root / "mod", root / "game"});
    do {
      snapshot = ambiguous.Get();
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    } while (!snapshot->ready && std::chrono::steady_clock::now() < deadline);
    Require(
        snapshot->ready && snapshot->errors.empty() && !snapshot->FindNative(native_id),
        "One GUID naming distinct source assets remains ambiguous instead of mislabelling a model");
    fs::remove_all(root);
    std::cout << checks << " stream and asset checks passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    std::error_code ignored;
    fs::remove_all(root, ignored);
    return 1;
  }
}
