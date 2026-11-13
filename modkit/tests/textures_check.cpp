#include "textures.h"
#include "streams.h"
#include "source.h"
#include <rex/ppc/context.h>

#include <algorithm>
#include <cstring>
#include <iostream>
#include <set>

using namespace simpsons::modkit;
namespace {
uint64_t checks = 0;
std::vector<uint8_t> memory;
std::vector<std::pair<uint32_t, uint32_t>> regions;
void Require(bool value, const char* message) {
  ++checks;
  if (!value)
    throw std::runtime_error(message);
}
void Bounded(uint32_t at, uint32_t size) {
  Require(std::any_of(regions.begin(), regions.end(),
                      [&](auto r) {
                        return at >= r.first && uint64_t(at) + size <= uint64_t(r.first) + r.second;
                      }),
          "Original texture operations stay within declared metadata and stack");
}
uint32_t Read(uint32_t at) {
  if (at == 0x82E3DC94)
    return 52;  // Native driver's reviewed raster-extension offset.
  Bounded(at, 4);
  uint32_t value = 0;
  for (size_t i = 0; i < 4; ++i)
    value = (value << 8) | memory.at(at + i);
  return value;
}
void Write(uint32_t at, uint32_t value) {
  Bounded(at, 4);
  for (size_t i = 0; i < 4; ++i)
    memory.at(at + i) = uint8_t(value >> (24 - i * 8));
}
uint64_t Read64(uint32_t at) {
  return uint64_t(Read(at)) << 32 | Read(at + 4);
}
void Write64(uint32_t at, uint64_t value) {
  Write(at, uint32_t(value >> 32));
  Write(at + 4, uint32_t(value));
}
template <class F>
void Reject(F f, const char* message) {
  bool rejected = false;
  try {
    f();
  } catch (const std::exception&) {
    rejected = true;
  }
  Require(rejected, message);
}
void Put(std::vector<uint8_t>& data, size_t at, uint32_t value, bool little = false) {
  for (size_t i = 0; i < 4; ++i)
    data.at(at + i) = uint8_t(value >> ((little ? i : 3 - i) * 8));
}
}  // namespace
#undef REX_LOAD_U32
#define REX_LOAD_U32(at) Read(uint32_t(at))
#undef REX_STORE_U32
#define REX_STORE_U32(at, value) Write(uint32_t(at), uint32_t(value))
#undef REX_LOAD_U64
#define REX_LOAD_U64(at) Read64(uint32_t(at))
#undef REX_STORE_U64
#define REX_STORE_U64(at, value) Write64(uint32_t(at), uint64_t(value))
#define REX_FUNC_PROLOGUE() ((void)0)
#include "native_texture.h"

namespace {
void Oracle(const TextureResource& source, const TextureResource::Texture& t) {
  const uint32_t base = 4096, extent = t.raster + 136, stack = base + extent + 512;
  memory.assign(stack + 64, 0xD3);
  regions = {{base, extent}, {stack - 256, 256}};
  std::copy_n(source.data.begin() + source.body_offset, extent, memory.begin() + base);
  auto expected = memory;
  const uint32_t raster = base + t.raster, header = raster + 84;
  Put(expected, base + t.record, raster);
  Put(expected, raster, raster);
  Put(expected, raster + 52, header);
  const uint32_t palette = Read(raster + 56);
  if (palette)
    Put(expected, raster + 56, base + palette);
  Put(expected, header + 32, ((t.fetch[1] & ~4095u) + base + t.pixels) | (t.fetch[1] & 4095));
  if (((t.fetch[4] >> 6) & 15) != 0)
    Put(expected, header + 48, ((t.fetch[5] & ~4095u) + base + t.pixels) | (t.fetch[5] & 4095));
  PPCContext context{};
  context.r1.u64 = stack;
  context.r3.u64 = base + t.record;
  context.r4.u64 = base;
  context.lr = 0x12345678;
  context.r31.u64 = 0xABCDEF01;
  GameTextureRelocate(context, memory.data());
  Require(context.r1.u32 == stack && context.r3.u32 == base + t.record &&
              context.lr == 0x12345678 && context.r31.u32 == 0xABCDEF01,
          "Native relocation preserves its ABI and returns the original texture");
  Require(std::equal(expected.begin(), expected.begin() + base + extent, memory.begin()),
          "Raster, header and physical image pointers agree byte-for-byte with original routines");
  Require(std::equal(expected.begin() + base + extent, expected.begin() + stack - 256,
                     memory.begin() + base + extent) &&
              std::equal(expected.begin() + stack, expected.end(), memory.begin() + stack),
          "Native texture relocation preserves allocation guards");
}
std::vector<uint8_t> Fixture(uint32_t format = 20, bool tiled = true, bool packed = true,
                             uint32_t endian = 1) {
  constexpr uint32_t size = 0x21000;
  std::vector<uint8_t> bytes(96 + size);
  Put(bytes, 0, 0x716, true);
  Put(bytes, 4, uint32_t(bytes.size() - 12), true);
  Put(bytes, 8, 0x1802FFFF, true);
  Put(bytes, 12, 80);
  Put(bytes, 16, 12);
  std::copy_n("test.itxd", 9, bytes.begin() + 20);
  for (uint32_t i = 0; i < 4; ++i)
    Put(bytes, 32 + i * 4, i + 1);
  Put(bytes, 48, 12);
  std::copy_n("EARS_ITXD", 9, bytes.begin() + 52);
  Put(bytes, 68, 4);
  Put(bytes, 80, size);
  auto w = [&](uint32_t at, uint32_t value) { Put(bytes, 96 + at, value); };
  w(0, 0x757A0003);
  w(4, 1);
  w(8, 0xEA2F);
  w(12, 0x00580020);
  w(16, 0x06000000);
  w(24, 48);
  w(28, 48);
  w(40, 160);
  w(48, 24);
  w(52, 24);
  std::copy_n("test", 4, bytes.begin() + 96 + 56);
  w(160, 160);
  w(172, 64);
  w(176, 64);
  w(212, 244);
  w(228, 0x20000);
  w(232, 4096);
  w(272, 2 | (2 << 22) | (tiled ? 0x80000000u : 0));
  w(276, format | (endian << 6));
  w(280, 63 | (63 << 13));
  w(284, 0xD10);
  w(288, 6 << 6);
  w(292, 0x4000 | 0x200 | (packed ? 2048 : 0));
  for (size_t i = 96 + 4096; i < bytes.size(); ++i)
    bytes[i] = uint8_t(i * 13 + (i >> 9));
  return bytes;
}
void Writer() {
  for (uint32_t format : {6u, 18u, 19u, 20u})
    for (bool tiled : {false, true})
      for (bool packed : {false, true})
        for (uint32_t endian = 0; endian < 4; ++endian) {
          auto bytes = Fixture(format, tiled, packed, endian);
          const TextureResource resource(bytes);
          Require(resource.textures().size() == 1 && resource.textures()[0].editable,
                  "Reviewed synthetic image is editable");
          const auto dds = resource.Dds(0);
          auto document = resource.Describe();
          document["changes"] = {resource.Import(0, dds)};
          Require(resource.Apply(document) == bytes,
                  "DDS round-trip preserves every header, mip, pixel and padding byte");
          Oracle(resource, resource.textures()[0]);
        }
  const auto bytes = Fixture();
  const TextureResource resource(bytes);
  const auto dds = resource.Dds(0);
  auto document = resource.Describe();
  auto changed = dds;
  changed.at(128) ^= 0x5A;
  document["changes"] = {resource.Import(0, changed)};
  const TextureResource edited(resource.Apply(document));
  Require(edited.Dds(0) == changed,
          "Authored DDS edit reaches exactly the selected native image bytes");
  const auto image_key = resource.body_offset + resource.textures()[0].record + 92;
  Require(!std::equal(bytes.begin() + image_key, bytes.begin() + image_key + 4,
                      edited.data.begin() + image_key) &&
              std::equal(bytes.begin() + image_key - 4, bytes.begin() + image_key,
                         edited.data.begin() + image_key - 4),
          "Authored pixels get a separate content key while the original name hash stays intact");
  Require(resource.Apply(document) == edited.data, "Authored image identities are reproducible");
  Oracle(edited, edited.textures()[0]);
  for (size_t offset : {size_t(12), size_t(16), size_t(28), size_t(76), size_t(84), size_t(112)}) {
    auto bad = dds;
    bad.at(offset) ^= 1;
    Reject([&] { resource.Import(0, bad); }, "Incompatible DDS headers reject before mutation");
  }
  auto bad = dds;
  bad.pop_back();
  Reject([&] { resource.Import(0, bad); }, "Truncated mip chain rejects");
  bad = dds;
  bad.push_back(0);
  Reject([&] { resource.Import(0, bad); }, "Unexpected DDS data rejects");
  auto change = document["changes"][0];
  change["levels"][0] = "a";
  document["changes"] = {change};
  Reject([&] { resource.Apply(document); }, "Malformed hexadecimal rejects");
  change["levels"][0] = "00";
  document["changes"] = {change};
  Reject([&] { resource.Apply(document); }, "Incorrect mip byte count rejects");
  auto corrupt = bytes;
  Put(corrupt, 96 + 52, 48);
  Reject([&] { TextureResource r(corrupt); }, "Invalid dictionary back-link rejects");
  corrupt = bytes;
  Put(corrupt, 96 + 48, 48);
  Reject([&] { TextureResource r(corrupt); }, "Circular lists must close at their sentinel");
  corrupt = bytes;
  Put(corrupt, 96 + 292, 0xFFFFFA00);
  Require(!TextureResource(corrupt).textures()[0].editable,
          "Overflowing mip addresses stay read-only");
  corrupt = bytes;
  Put(corrupt, 96 + 276, 59);
  Require(!TextureResource(corrupt).textures()[0].editable, "Unsupported formats stay read-only");
  auto catalog = Json{{"version", 1}, {"textures", Json::array({resource.Describe()})}};
  catalog["textures"][0]["changes"] = {resource.Import(0, dds)};
  ValidateTextureVariants(catalog);
  catalog["textures"][0]["changes"][0]["arbitrary_offset"] = 0;
  Reject([&] { ValidateTextureVariants(catalog); },
         "Image edits cannot introduce arbitrary offsets");

  auto first = resource.Describe(), second = first;
  first["changes"] = {resource.Import(0, dds)};
  second["changes"] = {resource.Import(0, changed)};
  Manifest low, high;
  low.id = "texture.low";
  high.id = "texture.high";
  low.texture_edits = std::make_shared<const Json>(Json{{"version", 1}, {"textures", {first}}});
  high.texture_edits = std::make_shared<const Json>(Json{{"version", 1}, {"textures", {second}}});
  const TextureVariants variants({low, high});
  const auto selected = variants.Apply(bytes);
  Require(selected && selected->owner == high.id && selected->bytes == edited.data,
          "Requested resource priority keeps the winning creator and complete native image edit");
  high.texture_edits.reset();
  Require(variants.Apply(bytes)->bytes == edited.data,
          "Prepared texture changes retain immutable source data");
  auto drift = bytes;
  drift.back() ^= 1;
  Reject([&] { variants.Apply(drift); }, "Changed native sources reject the entire image edit");
}
}  // namespace
int main(int argc, char** argv) {
  Json report{{"ok", false}};
  size_t resources = 0, textures = 0, editable = 0, mips = 0;
  uint64_t pixels = 0;
  try {
    Writer();
    if (argc > 1)
      for (const auto& item : fs::recursive_directory_iterator(Utf8Path(argv[1]))) {
        if (!item.is_regular_file() || item.path().extension() != ".str")
          continue;
        const StreamFile file(item.path());
        for (size_t entry = 0; entry < file.entries().size(); ++entry) {
          const auto infos = file.DescribeAll(entry);
          std::vector<uint8_t> decoded;
          for (const auto& info : infos) {
            if (!info.name.ends_with(".itxd"))
              continue;
            if (decoded.empty())
              decoded = file.Read(entry);
            const TextureResource source(
                std::span<const uint8_t>(decoded).subspan(info.offset, info.size));
            ++resources;
            auto document = source.Describe();
            for (size_t i = 0; i < source.textures().size(); ++i) {
              const auto& texture = source.textures()[i];
              ++textures;
              Oracle(source, texture);
              if (!texture.editable)
                continue;
              ++editable;
              mips += texture.levels.size();
              pixels += uint64_t(texture.width) * texture.height;
              document["changes"].push_back(source.Import(i, source.Dds(i)));
            }
            Require(source.Apply(document) == source.data,
                    "All stock DDS writer round-trips preserve the complete source resource");
          }
        }
      }
    report = {{"ok", true},
              {"checks", checks},
              {"resources", resources},
              {"textures", textures},
              {"editable", editable},
              {"mips", mips},
              {"base_pixels", pixels},
              {"native_relocation_sha256", kGameTextureRelocateSource},
              {"native_address_sha256", ksub_82C20E80Source},
              {"native_mip_count_sha256", ksub_8243FE90Source}};
  } catch (const std::exception& error) {
    report["error"] = error.what();
    report["checks"] = checks;
    report["resources"] = resources;
  }
  std::cout << report.dump(2) << '\n';
  return report.at("ok") == true ? 0 : 1;
}
