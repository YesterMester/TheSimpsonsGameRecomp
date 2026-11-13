#include "textures.h"
#include "sha256.h"

#include <algorithm>
#include <bit>
#include <set>

namespace simpsons::modkit {
namespace {
constexpr uint32_t kLimit = 16 * 1024 * 1024;
uint32_t Read(std::span<const uint8_t> data, size_t at, bool little = false) {
  if (at > data.size() || data.size() - at < 4)
    throw Failure("invalid_texture", "Texture field exceeds its data");
  uint32_t value = 0;
  for (size_t i = 0; i < 4; ++i)
    value |= uint32_t(data[at + i]) << ((little ? i : 3 - i) * 8);
  return value;
}
void Little(std::vector<uint8_t>& data, size_t at, uint32_t value) {
  for (size_t i = 0; i < 4; ++i)
    data.at(at + i) = uint8_t(value >> (i * 8));
}
void Big(std::vector<uint8_t>& data, size_t at, uint32_t value) {
  for (size_t i = 0; i < 4; ++i)
    data.at(at + i) = uint8_t(value >> ((3 - i) * 8));
}
uint32_t Align(uint32_t value, uint32_t alignment) {
  return (value + alignment - 1) & ~(alignment - 1);
}
uint32_t CeilLog(uint32_t value) {
  return std::bit_width(value - 1);
}
uint32_t FourCC(uint32_t format) {
  return format == 18 ? 0x31545844 : format == 19 ? 0x33545844 : 0x35545844;
}
std::string Hex(std::span<const uint8_t> bytes) {
  constexpr char digits[] = "0123456789abcdef";
  std::string result(bytes.size() * 2, '0');
  for (size_t i = 0; i < bytes.size(); ++i) {
    result[i * 2] = digits[bytes[i] >> 4];
    result[i * 2 + 1] = digits[bytes[i] & 15];
  }
  return result;
}
std::vector<uint8_t> Unhex(const std::string& text) {
  if (text.empty() || text.size() > kLimit || text.size() % 2 ||
      text.find_first_not_of("0123456789abcdef") != std::string::npos)
    throw Failure("invalid_texture", "Texture blocks need bounded lowercase hexadecimal bytes");
  auto digit = [](char c) { return uint8_t(c <= '9' ? c - '0' : c - 'a' + 10); };
  std::vector<uint8_t> result(text.size() / 2);
  for (size_t i = 0; i < result.size(); ++i)
    result[i] = digit(text[2 * i]) * 16 + digit(text[2 * i + 1]);
  return result;
}
void ValidateChange(const Json& change) {
  if (!change.is_object() || change.size() != 2 || !change.contains("texture") ||
      !change.at("texture").is_string() || !change.contains("levels") ||
      !change.at("levels").is_array() || change.at("levels").empty() ||
      change.at("levels").size() > 14)
    throw Failure("invalid_texture", "Texture changes need an identity and a complete mip chain");
  const auto id = change.at("texture").get<std::string>();
  const auto suffix = std::string_view(id).substr(std::min(size_t(8), id.size()));
  if (!id.starts_with("texture:") || suffix.empty() || suffix.size() > 4 ||
      (suffix.size() > 1 && suffix[0] == '0') ||
      suffix.find_first_not_of("0123456789") != std::string_view::npos)
    throw Failure("invalid_texture", "Texture identities must identify original source records");
  for (const auto& level : change.at("levels")) {
    if (!level.is_string())
      throw Failure("invalid_texture", "Mip blocks must be hexadecimal strings");
    Unhex(level.get_ref<const std::string&>());
  }
}
// Xenos 32x32-block address permutation. The runtime's texture_util::GetTiledOffset2D
// and Xenia's BSD-licensed texture research provide the independent layout oracle.
uint32_t Tiled(uint32_t x, uint32_t y, uint32_t pitch, uint32_t log) {
  const uint32_t macro = ((x >> 5) + (y >> 5) * (pitch >> 5)) << (log + 7);
  const uint32_t micro = ((x & 7) + ((y & 14) << 2)) << log;
  const uint32_t offset = macro + ((micro & ~15u) << 1) + (micro & 15) + ((y & 1) << 4);
  return ((offset & ~511u) << 3) + ((y & 16) << 7) + ((offset & 448) << 2) +
         (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 63);
}
std::pair<uint32_t, uint32_t> Packed(uint32_t width, uint32_t height, uint32_t level) {
  const auto w = CeilLog(width), h = CeilLog(height),
             base = std::min(w, h) > 4 ? std::min(w, h) - 4 : 0;
  if (level < base)
    return {};
  const auto relative = level - base;
  if (relative < 3)
    return w > h ? std::pair(0u, 16u >> relative) : std::pair(16u >> relative, 0u);
  const uint32_t offset = (1u << (std::max(w, h) - base)) >> (relative - 2);
  return w > h ? std::pair(offset, 0u) : std::pair(0u, offset);
}
}  // namespace

void TextureResource::Range(uint64_t at, uint64_t count) const {
  if (at > body_size || count > body_size - at)
    throw Failure("invalid_texture", "Texture data exceeds its dictionary");
}
uint32_t TextureResource::Word(uint32_t at) const {
  Range(at, 4);
  return Read(data, body_offset + at);
}
TextureResource::TextureResource(std::span<const uint8_t> bytes) : NamedResource(bytes) {
  if (native_class != "EARS_ITXD" || body_size < 32 || Word(0) != 0x757A0003)
    throw Failure("unsupported_texture", "Expected a reviewed preinstanced EARS_ITXD dictionary");
  const uint32_t counts = Word(4), plugins = (counts >> 16) + (counts & 65535);
  if (plugins > 256)
    throw Failure("invalid_texture", "Too many native texture plugins");
  const uint32_t object = (plugins + 1) * 8, sentinel = object + 8;
  Range(object, 24);
  if (data[body_offset + object] != 6)
    throw Failure("unsupported_texture", "Expected a native texture dictionary object");
  bool image_identity = false;
  for (uint32_t i = 0; i < plugins; ++i)
    image_identity |= Word(8 + i * 8) == 0xEA2F && Word(12 + i * 8) == 0x00580020;
  uint32_t previous = sentinel, node = Word(sentinel), metadata_end = object + 24;
  std::set<uint32_t> seen;
  while (node != sentinel) {
    if (node < 8 || node % 4 || textures_.size() >= 1024 || !seen.insert(node).second)
      throw Failure("invalid_texture", "Broken or oversized native texture list");
    const uint32_t record = node - 8;
    Range(record, 88);
    if (Word(node + 4) != previous)
      throw Failure("invalid_texture", "Native texture list links disagree");
    auto name = [&](uint32_t at) {
      std::string text;
      for (uint32_t i = 0; i < 32; ++i) {
        const uint8_t c = data.at(body_offset + at + i);
        if (!c)
          return text;
        if (c < 32 || c > 126)
          throw Failure("unsupported_texture", "Texture names need a reviewed text encoding");
        text += char(c);
      }
      throw Failure("invalid_texture", "Unterminated native texture name");
    };
    Texture t;
    t.record = record;
    t.raster = Word(record);
    t.id = "texture:" + std::to_string(textures_.size());
    t.name = name(record + 16);
    t.mask = name(record + 48);
    Range(t.raster, 136);
    t.width = Word(t.raster + 12);
    t.height = Word(t.raster + 16);
    t.pixels = Word(t.raster + 72);
    t.allocation = Word(t.raster + 68);
    Range(t.pixels, t.allocation);
    metadata_end = std::max({metadata_end, record + 88, t.raster + 136});
    for (size_t i = 0; i < 6; ++i)
      t.fetch[i] = Word(t.raster + 112 + uint32_t(i) * 4);
    t.format = t.fetch[1] & 63;
    t.endian = (t.fetch[1] >> 6) & 3;
    t.tiled = (t.fetch[0] >> 31) != 0;
    if (!image_identity || Word(t.raster) != t.raster || Word(t.raster + 52) != t.raster + 84 ||
        t.raster != record + 120 || t.pixels % 4096 || !t.allocation)
      t.reason = "Native raster ownership or allocation needs a reviewed layout";
    else {
      try {
        Layout(t);
      } catch (const Failure& error) {
        t.reason = error.what();
        t.levels.clear();
      }
    }
    t.editable = t.reason.empty();
    textures_.push_back(std::move(t));
    previous = node;
    node = Word(node);
  }
  if (Word(sentinel + 4) != previous)
    throw Failure("invalid_texture", "Native texture dictionary tail disagrees");
  for (size_t i = 0; i < textures_.size(); ++i) {
    auto& a = textures_[i];
    if (a.pixels < metadata_end) {
      a.editable = false;
      a.reason = "Pixel allocation overlaps native metadata";
    }
    for (size_t j = 0; j < i; ++j) {
      auto& b = textures_[j];
      if (a.pixels < uint64_t(b.pixels) + b.allocation &&
          b.pixels < uint64_t(a.pixels) + a.allocation) {
        a.editable = b.editable = false;
        a.reason = b.reason = "Shared pixel allocations need a reviewed alias policy";
      }
    }
  }
}
void TextureResource::Layout(Texture& t) {
  t.block = t.format == 6 ? 1 : 4;
  t.bytes = t.format == 6 ? 4 : t.format == 18 ? 8 : 16;
  if (t.format != 6 && t.format != 18 && t.format != 19 && t.format != 20)
    throw Failure("unsupported_texture", "DDS editing supports RGBA8, BC1, BC2 and BC3");
  if ((t.fetch[0] & 3) != 2 || ((t.fetch[5] >> 9) & 3) != 1 || (t.fetch[1] & 1024) ||
      (t.fetch[2] >> 26) || ((t.fetch[3] >> 1) & 4095) != 0x688 || (t.fetch[3] & 0x80000000))
    throw Failure("unsupported_texture",
                  "Texture needs a reviewed 2D, single-layer, identity-swizzle layout");
  if (!t.width || !t.height || t.width > 8192 || t.height > 8192 ||
      t.width != (t.fetch[2] & 8191) + 1 || t.height != ((t.fetch[2] >> 13) & 8191) + 1)
    throw Failure("invalid_texture", "Raster dimensions disagree with the texture fetch header");
  const uint32_t max_level = (t.fetch[4] >> 6) & 15;
  if (max_level > std::bit_width(std::max(t.width, t.height)) - 1 || max_level > 13)
    throw Failure("invalid_texture", "Mip chain exceeds its source dimensions");
  const uint32_t packed =
      t.fetch[5] & 2048 ? std::max(CeilLog(std::min(t.width, t.height)), 4u) - 4 : UINT32_MAX;
  uint32_t mip_offset = 0, storage_previous = 0;
  std::vector<bool> touched((t.allocation + t.bytes - 1) / t.bytes);
  for (uint32_t level = 0; level <= max_level; ++level) {
    const uint32_t storage = std::min(level, packed);
    const uint32_t width = std::max(t.width >> level, 1u), height = std::max(t.height >> level, 1u);
    const uint32_t pitch_texels = level == 0 ? ((t.fetch[0] >> 22) & 511) * 32
                                             : std::max((1u << CeilLog(t.width)) >> storage, 1u);
    uint32_t pitch = Align((pitch_texels + t.block - 1) / t.block, 32) * t.bytes;
    if (!t.tiled && level)
      pitch = Align(pitch, 256);
    if (!pitch)
      throw Failure("invalid_texture", "Texture pitch is zero");
    if (level > 1 && storage != storage_previous) {
      const auto previous_height = std::max((1u << CeilLog(t.height)) >> storage_previous, 1u);
      const auto& before = t.levels.back();
      mip_offset +=
          Align(before.pitch * Align((previous_height + t.block - 1) / t.block, 32), 4096);
    }
    auto [x, y] = level >= packed ? Packed(t.width, t.height, level) : std::pair(0u, 0u);
    const uint64_t offset =
        uint64_t((level ? t.fetch[5] : t.fetch[1]) & ~4095u) + (level ? mip_offset : 0);
    if (offset >= t.allocation)
      throw Failure("invalid_texture", "Mip address exceeds its allocation");
    Level current{width, height, uint32_t(offset), pitch, x / t.block, y / t.block};
    t.levels.push_back(current);
    storage_previous = storage;
    const uint32_t columns = (width + t.block - 1) / t.block,
                   rows = (height + t.block - 1) / t.block;
    for (uint32_t by = 0; by < rows; ++by)
      for (uint32_t bx = 0; bx < columns; ++bx) {
        const auto at = Address(t, level, bx, by) - t.pixels;
        if (at % t.bytes || at > t.allocation || t.bytes > t.allocation - at ||
            touched.at(at / t.bytes))
          throw Failure("invalid_texture",
                        "Mip storage overlaps or exceeds its original allocation");
        touched[at / t.bytes] = true;
      }
  }
}
uint32_t TextureResource::Address(const Texture& t, size_t level, uint32_t x, uint32_t y) const {
  const auto& l = t.levels.at(level);
  x += l.x;
  y += l.y;
  const uint32_t offset = t.tiled ? Tiled(x, y, l.pitch / t.bytes, std::countr_zero(t.bytes))
                                  : y * l.pitch + x * t.bytes;
  const uint64_t relative = uint64_t(l.offset) + offset;
  if (relative >= t.allocation || t.bytes > t.allocation - relative)
    throw Failure("invalid_texture", "Mip block exceeds its original allocation");
  return t.pixels + uint32_t(relative);
}
std::vector<uint8_t> TextureResource::Linear(const Texture& t, size_t level) const {
  if (!t.editable)
    throw Failure("unsupported_texture", t.reason);
  const auto& l = t.levels.at(level);
  const uint32_t columns = (l.width + t.block - 1) / t.block,
                 rows = (l.height + t.block - 1) / t.block;
  std::vector<uint8_t> result(size_t(columns) * rows * t.bytes);
  const uint32_t swap[] = {0, 1, 3, 2};
  for (uint32_t y = 0; y < rows; ++y)
    for (uint32_t x = 0; x < columns; ++x) {
      const auto source = body_offset + Address(t, level, x, y), dest = (y * columns + x) * t.bytes;
      for (uint32_t i = 0; i < t.bytes; ++i)
        result[dest + i] = data.at(source + (i ^ swap[t.endian]));
    }
  return result;
}
Json TextureResource::Describe() const {
  Json textures = Json::array();
  for (const auto& t : textures_)
    textures.push_back({{"id", t.id},
                        {"name", t.name},
                        {"mask", t.mask},
                        {"width", t.width},
                        {"height", t.height},
                        {"format", t.format},
                        {"mips", ((t.fetch[4] >> 6) & 15) + 1},
                        {"tiled", t.tiled},
                        {"editable", t.editable},
                        {"reason", t.reason}});
  return {{"format", "EARS_ITXD"},
          {"version", 1},
          {"resource_id", resource_id},
          {"source_sha256", sha256::SHA256{}(data.data(), data.size())},
          {"name", name},
          {"textures", textures},
          {"changes", Json::array()}};
}
std::vector<uint8_t> TextureResource::Dds(size_t index) const {
  const auto& t = textures_.at(index);
  if (!t.editable)
    throw Failure("unsupported_texture", t.reason);
  std::vector<uint8_t> result(128);
  Little(result, 0, 0x20534444);
  Little(result, 4, 124);
  Little(result, 8, 0x1007 | (t.format == 6 ? 8 : 0x80000) | (t.levels.size() > 1 ? 0x20000 : 0));
  Little(result, 12, t.height);
  Little(result, 16, t.width);
  Little(result, 20, t.format == 6 ? t.width * 4 : uint32_t(Linear(t, 0).size()));
  Little(result, 28, uint32_t(t.levels.size()));
  Little(result, 76, 32);
  Little(result, 80, t.format == 6 ? 0x41 : 4);
  if (t.format == 6) {
    Little(result, 88, 32);
    Little(result, 92, 0xff);
    Little(result, 96, 0xff00);
    Little(result, 100, 0xff0000);
    Little(result, 104, 0xff000000);
  } else
    Little(result, 84, FourCC(t.format));
  Little(result, 108, 0x1000 | (t.levels.size() > 1 ? 0x400008 : 0));
  for (size_t i = 0; i < t.levels.size(); ++i) {
    const auto bytes = Linear(t, i);
    result.insert(result.end(), bytes.begin(), bytes.end());
  }
  return result;
}
Json TextureResource::Import(size_t index, std::span<const uint8_t> dds) const {
  const auto& t = textures_.at(index);
  if (!t.editable)
    throw Failure("unsupported_texture", t.reason);
  auto word = [&](size_t at) { return Read(dds, at, true); };
  if (dds.size() < 128 || dds.size() > kLimit || word(0) != 0x20534444 || word(4) != 124 ||
      word(12) != t.height || word(16) != t.width || word(76) != 32 || word(24) || word(112) ||
      word(116) || word(120) || word(124) || (word(8) & 0x1007) != 0x1007 ||
      (word(108) & 0x1000) != 0x1000 || std::max(word(28), 1u) != t.levels.size())
    throw Failure("invalid_texture",
                  "DDS must preserve dimensions, format, 2D shape and every source mip");
  if (t.format == 6 ? (word(80) != 0x41 || word(84) || word(88) != 32 || word(92) != 0xff ||
                       word(96) != 0xff00 || word(100) != 0xff0000 || word(104) != 0xff000000)
                    : (word(80) != 4 || word(84) != FourCC(t.format)))
    throw Failure("invalid_texture", "DDS pixel format must match the original native texture");
  Json levels = Json::array();
  size_t cursor = 128;
  for (const auto& l : t.levels) {
    const size_t size =
        size_t((l.width + t.block - 1) / t.block) * ((l.height + t.block - 1) / t.block) * t.bytes;
    if (cursor > dds.size() || size > dds.size() - cursor)
      throw Failure("invalid_texture", "DDS mip chain is truncated");
    levels.push_back(Hex(dds.subspan(cursor, size)));
    cursor += size;
  }
  if (cursor != dds.size())
    throw Failure("invalid_texture", "DDS has unexpected trailing data");
  return {{"texture", t.id}, {"levels", std::move(levels)}};
}
std::vector<uint8_t> TextureResource::Apply(const Json& document) const {
  const auto description = Describe();
  for (const auto* key : {"format", "version", "resource_id", "source_sha256"})
    if (!document.contains(key) || document.at(key) != description.at(key))
      throw Failure("source_changed", "Texture document does not match its original resource");
  if (document.contains("textures") && document.at("textures") != description.at("textures"))
    throw Failure("invalid_texture", "Texture inspection metadata is read-only");
  if (!document.contains("changes") || !document.at("changes").is_array() ||
      document.at("changes").size() > textures_.size())
    throw Failure("invalid_texture", "Texture changes need a bounded array");
  auto result = data;
  std::set<std::string> seen;
  const uint32_t swap[] = {0, 1, 3, 2};
  for (const auto& change : document.at("changes")) {
    ValidateChange(change);
    const auto id = change.at("texture").get<std::string>();
    const auto found =
        std::find_if(textures_.begin(), textures_.end(), [&](const auto& t) { return t.id == id; });
    if (found == textures_.end() || !seen.insert(id).second || !found->editable)
      throw Failure("unsupported_texture", "Choose a unique, editable original texture");
    const auto& t = *found;
    if (change.at("levels").size() != t.levels.size())
      throw Failure("invalid_texture", "Include every original mip level");
    bool changed = false;
    sha256::SHA256 identity;
    const std::string seed =
        "simpsons.modkit.image.v1:" + description.at("source_sha256").get<std::string>() + ":" +
        t.id;
    identity.add(seed.data(), seed.size());
    for (size_t level = 0; level < t.levels.size(); ++level) {
      const auto bytes = Unhex(change.at("levels").at(level).get_ref<const std::string&>());
      const auto& l = t.levels[level];
      const uint32_t columns = (l.width + t.block - 1) / t.block,
                     rows = (l.height + t.block - 1) / t.block;
      if (bytes.size() != size_t(columns) * rows * t.bytes)
        throw Failure("invalid_texture", "Mip block count does not match its native image");
      identity.add(bytes.data(), bytes.size());
      for (uint32_t y = 0; y < rows; ++y)
        for (uint32_t x = 0; x < columns; ++x) {
          const auto target = body_offset + Address(t, level, x, y),
                     source = (y * columns + x) * t.bytes;
          for (uint32_t i = 0; i < t.bytes; ++i) {
            changed |= result.at(target + (i ^ swap[t.endian])) != bytes[source + i];
            result.at(target + (i ^ swap[t.endian])) = bytes[source + i];
          }
        }
    }
    if (changed) {
      // The original loader shares images by the EA2F plugin's second word.
      // Keep the name hash, but give authored pixels their own cache identity.
      // A no-op retains the original key and every byte of its resource.
      std::array<uint8_t, 32> digest{};
      identity.getHash(digest.data());
      uint32_t key = Read(digest, 0);
      auto used = [&](uint32_t value) {
        return !value || std::any_of(textures_.begin(), textures_.end(), [&](const auto& source) {
          return Read(result, body_offset + source.record + 92) == value;
        });
      };
      while (used(key))
        ++key;
      Big(result, body_offset + t.record + 92, key);
    }
  }
  return result;
}
Json ValidateTextureVariants(const Json& catalog) {
  if (!catalog.is_object() || catalog.size() != 2 || catalog.value("version", Json()) != 1 ||
      !catalog.at("version").is_number_integer() || !catalog.contains("textures") ||
      !catalog.at("textures").is_array() || catalog.at("textures").size() > 64 ||
      catalog.dump().size() > kLimit)
    throw Failure("invalid_texture",
                  "A texture catalog needs version 1 and at most 64 bounded resources");
  std::set<std::string> seen;
  for (const auto& d : catalog.at("textures")) {
    if (!d.is_object() || d.value("format", Json()) != "EARS_ITXD" ||
        d.value("version", Json()) != 1 || !d.at("version").is_number_integer() ||
        !d.contains("resource_id") || !d.at("resource_id").is_string() ||
        !d.contains("source_sha256") || !d.at("source_sha256").is_string() ||
        !d.contains("changes") || !d.at("changes").is_array() || d.at("changes").empty() ||
        d.at("changes").size() > 1024)
      throw Failure("invalid_texture",
                    "Texture edits need a source identity, hash and image changes");
    const auto id = d.at("resource_id").get<std::string>(),
               hash = d.at("source_sha256").get<std::string>();
    if (id.size() != 32 || id.find_first_not_of("0123456789ABCDEF") != std::string::npos ||
        id == std::string(32, '0') || hash.size() != 64 ||
        hash.find_first_not_of("0123456789abcdef") != std::string::npos || !seen.insert(id).second)
      throw Failure("invalid_texture",
                    "Texture edits need unique identities and lowercase source SHA-256");
    std::set<std::string> changed;
    for (const auto& c : d.at("changes")) {
      ValidateChange(c);
      if (!changed.insert(c.at("texture").get<std::string>()).second)
        throw Failure("invalid_texture", "Repeated texture edit");
    }
  }
  return catalog;
}
TextureVariants::TextureVariants(const std::vector<Manifest>& manifests) {
  size_t count = 0, bytes = 0;
  for (const auto& mod : manifests) {
    if (!mod.texture_edits)
      continue;
    const auto catalog = ValidateTextureVariants(*mod.texture_edits);
    const auto size = catalog.dump().size();
    if (size > 64 * 1024 * 1024 - bytes)
      throw Failure("resource_limit", "Prepared texture edits exceed 64 MiB");
    bytes += size;
    for (const auto& d : catalog.at("textures")) {
      if (++count > 1024)
        throw Failure("resource_limit",
                      "A profile supports at most 1024 edited texture dictionaries");
      textures_[d.at("resource_id").get<std::string>()] = {mod.id, d};
    }
  }
}
bool TextureVariants::Contains(std::string_view id) const {
  return textures_.contains(id);
}
std::optional<TextureVariants::Result> TextureVariants::Apply(
    std::span<const uint8_t> bytes) const {
  const NamedResource resource(bytes);
  const auto found = textures_.find(resource.resource_id);
  if (found == textures_.end())
    return {};
  try {
    return Result{found->second.owner, TextureResource(bytes).Apply(found->second.document)};
  } catch (const std::exception& e) {
    throw Failure("invalid_texture",
                  found->second.owner + " texture " + found->first + ": " + e.what());
  }
}
}  // namespace simpsons::modkit
