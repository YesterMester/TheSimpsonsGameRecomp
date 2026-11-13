#include "meshes.h"
#include "streams.h"
#include "source.h"
#include "../../simpsons/src/guest_memory.h"
#include <rex/ppc/context.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstring>
#include <iostream>
#include <limits>

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
  if (!size)
    return;
  Require(std::any_of(regions.begin(), regions.end(),
                      [&](auto region) {
                        return at >= region.first &&
                               uint64_t(at) + size <= uint64_t(region.first) + region.second;
                      }),
          "Original mesh operations stay within their declared buffers or stack");
}
uint32_t Read(uint32_t at) {
  Bounded(at, 4);
  return simpsons::LoadGuestU32(memory.data(), at);
}
void Write(uint32_t at, uint32_t value) {
  Bounded(at, 4);
  simpsons::StoreGuestU32(memory.data(), at, value);
}
uint64_t Read64(uint32_t at) {
  Bounded(at, 8);
  return uint64_t(Read(at)) << 32 | Read(at + 4);
}
void Write64(uint32_t at, uint64_t value) {
  Bounded(at, 8);
  Write(at, uint32_t(value >> 32));
  Write(at + 4, uint32_t(value));
}
template <class Function>
void Reject(Function function, const char* message) {
  bool rejected = false;
  try {
    function();
  } catch (const std::exception&) {
    rejected = true;
  }
  Require(rejected, message);
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
static void sub_82A3CD80(PPCContext& context, uint8_t*) {
  const auto dst = context.r3.u32, src = context.r4.u32, count = context.r5.u32;
  Bounded(dst, count);
  Bounded(src, count);
  if (count)
    std::memmove(memory.data() + dst, memory.data() + src, count);
}
#include "native_mesh.h"

namespace {
void Oracle(const NativeMeshData& block) {
  const uint32_t size = uint32_t(block.main().size()),
                 secondary_size = uint32_t(block.secondary().size());
  const uint32_t source = 4096, secondary = source + size + 64,
                 target = secondary + secondary_size + 64;
  const uint32_t target_secondary = target + size + 64,
                 stack = target_secondary + secondary_size + 512;
  memory.assign(stack + 128, 0xD3);
  regions = {{source, size},
             {secondary, secondary_size},
             {target, size},
             {target_secondary, secondary_size},
             {stack - 256, 256}};
  std::copy(block.main().begin(), block.main().end(), memory.begin() + source);
  std::copy(block.secondary().begin(), block.secondary().end(), memory.begin() + secondary);
  PPCContext context{};
  context.r3.u64 = source;
  context.r4.u64 = secondary;
  GameMeshRelocate(context, memory.data());
  const auto expected = block.Relocated(source, secondary);
  Require(std::equal(expected.begin(), expected.end(), memory.begin() + source),
          "Host mesh relocations agree byte-for-byte with the unchanged game routine");
  context.r1.u64 = stack;
  context.r3.u64 = target;
  context.r4.u64 = source;
  context.r5.u64 = target_secondary;
  context.r6.u64 = secondary;
  context.lr = 0x12345678;
  const auto lr = context.lr;
  const auto r28 = context.r28.u64, r29 = context.r29.u64, r30 = context.r30.u64,
             r31 = context.r31.u64;
  GameMeshCopy(context, memory.data());
  const auto copy = block.Relocated(target, target_secondary);
  Require(std::equal(copy.begin(), copy.end(), memory.begin() + target) &&
              std::equal(block.secondary().begin(), block.secondary().end(),
                         memory.begin() + target_secondary),
          "The unchanged original copy/rebase produces identical primary and secondary bytes");
  Require(context.r1.u32 == stack && context.lr == lr && context.r28.u64 == r28 &&
              context.r29.u64 == r29 && context.r30.u64 == r30 && context.r31.u64 == r31,
          "Original copy/rebase preserves its stack and nonvolatile registers");
  for (const auto [at, length] :
       std::vector<std::pair<uint32_t, uint32_t>>{{source - 64, 64},
                                                  {source + size, 64},
                                                  {secondary + secondary_size, 64},
                                                  {target + size, 64},
                                                  {target_secondary + secondary_size, 64}})
    Require(std::all_of(memory.begin() + at, memory.begin() + at + length,
                        [](auto value) { return value == 0xD3; }),
            "Original mesh operations preserve surrounding guard bytes");
}
std::vector<uint8_t> Fixture() {
  std::vector<uint8_t> bytes(12 + 128 + 16);
  auto big = [&](uint32_t at, uint32_t value) { simpsons::StoreGuestU32(bytes.data(), at, value); };
  auto little = [&](uint32_t at, uint32_t value) {
    for (uint32_t i = 0; i < 4; ++i)
      bytes[at + i] = uint8_t(value >> (i * 8));
  };
  little(0, 16);
  little(4, 128);
  little(8, 16);
  big(12, 0xBFBFBFBF);
  big(16, 0x01000000);
  big(20, 128);
  big(24, 16);
  big(28, 16);
  big(32, 2);
  big(36, 1);
  big(40, 64);
  big(44, 0);
  big(48, 68);
  big(52, 1);
  big(56, 0x3C43A23D);
  big(60, 8);
  big(64, 64);
  big(12 + 64, 88);
  big(12 + 68, 8);
  return bytes;
}

void Put(std::vector<uint8_t>& bytes, uint32_t at, uint32_t value, bool little = false) {
  for (uint32_t i = 0; i < 4; ++i)
    bytes.at(at + i) = uint8_t(value >> (little ? i * 8 : 24 - i * 8));
}
std::vector<uint8_t> Chunk(uint32_t type, const std::vector<uint8_t>& payload) {
  std::vector<uint8_t> bytes(12 + payload.size());
  Put(bytes, 0, type, true);
  Put(bytes, 4, uint32_t(payload.size()), true);
  Put(bytes, 8, 0x1C02002D, true);
  std::copy(payload.begin(), payload.end(), bytes.begin() + 12);
  return bytes;
}
void Append(std::vector<uint8_t>& bytes, const std::vector<uint8_t>& tail) {
  bytes.insert(bytes.end(), tail.begin(), tail.end());
}
std::vector<uint8_t> StaticFixture(bool skinned = false, uint32_t mode = 1, bool normals = false,
                                   bool uvs = false) {
  // A synthetic named clump: no original asset bytes, textures or game data.
  const uint32_t declarations = 1 + uint32_t(normals) + uint32_t(uvs);
  const uint32_t stride = 12 + (normals ? 4 : 0) + (uvs ? 8 : 0);
  const uint32_t index_data = 232 + declarations * 12, vertex_data = index_data + 8;
  const uint32_t size = vertex_data + 3 * stride;
  std::vector<uint8_t> plugin(size + 12);
  Put(plugin, 0, 16, true);
  Put(plugin, 4, size, true);
  auto word = [&](uint32_t at, uint32_t value) { Put(plugin, 12 + at, value); };
  word(0, 0xBFBFBFBF);
  word(4, 0x01000000);
  word(8, size);
  word(16, 16);
  word(20, 5);
  word(24, 1);
  const uint32_t fields[]{108, 116, 164, 168, 180},
      targets[]{152, 200, 232, vertex_data, index_data};
  for (uint32_t i = 0; i < 5; ++i) {
    word(28 + i * 8, fields[i]);
    word(fields[i], targets[i]);
  }
  word(68, 0x3C43A23D);
  word(72, 56);
  word(76, 96);
  word(96, 0x00030002);
  word(96 + 16, mode);
  word(96 + 24, mode);
  word(152, 3 * stride);
  word(156, stride);
  word(160, declarations);
  word(172, 6);
  word(176, 1);
  word(200 + 12, 6);
  word(200 + 24, 3);
  word(236, 0x002A23B9);
  if (normals) {
    word(244, 12);
    word(248, 0x002A2187);
    word(252, 0x00030000);
  }
  const uint32_t uv_offset = 12 + (normals ? 4 : 0);
  if (uvs) {
    const auto declaration = 232 + (declarations - 1) * 12;
    word(declaration, uv_offset);
    word(declaration + 4, 0x002C23A5);
    word(declaration + 8, 0x00050000);
  }
  plugin[12 + index_data + 3] = 1;
  plugin[12 + index_data + 5] = 2;
  const float positions[]{-1, -1, 0, 1, -1, 0, 0, 1, 0};
  const float texture_coordinates[]{-.25f, 1.5f, -0.0f, 2, .5f, -.75f};
  for (uint32_t i = 0; i < 3; ++i) {
    for (uint32_t axis = 0; axis < 3; ++axis)
      word(vertex_data + i * stride + axis * 4, std::bit_cast<uint32_t>(positions[i * 3 + axis]));
    if (normals)
      word(vertex_data + i * stride + 12, (511u << 20) | (i << 30));
    if (uvs)
      for (uint32_t axis = 0; axis < 2; ++axis)
        word(vertex_data + i * stride + uv_offset + axis * 4,
             std::bit_cast<uint32_t>(texture_coordinates[i * 2 + axis]));
  }
  std::vector<uint8_t> structure(40), bounds(36);
  Put(structure, 0, 0x0801003F, true);
  Put(structure, 4, 1, true);
  Put(structure, 8, 3, true);
  Put(structure, 12, 1, true);
  Put(structure, 28, std::bit_cast<uint32_t>(2.0f), true);
  const float box[]{-1, -1, 0, 1, 1, 0};
  for (uint32_t i = 0; i < 6; ++i)
    Put(bounds, 12 + i * 4, std::bit_cast<uint32_t>(box[i]), true);
  auto extension = Chunk(0xEA33, plugin);
  Append(extension, Chunk(0xEA16, bounds));
  if (skinned)
    Append(extension, Chunk(0x116, {}));
  auto geometry = Chunk(1, structure);
  Append(geometry, Chunk(3, extension));
  std::vector<uint8_t> count(4);
  Put(count, 0, 1, true);
  auto list = Chunk(1, count);
  Append(list, Chunk(0xF, geometry));
  const auto clump = Chunk(0x10, Chunk(0x1A, list));
  std::vector<uint8_t> resource(96 + clump.size());
  Put(resource, 0, 0x716, true);
  Put(resource, 4, uint32_t(resource.size() - 12), true);
  Put(resource, 8, 0x1802FFFF, true);
  Put(resource, 12, 80);
  Put(resource, 16, 12);
  std::copy_n("test.rws", 8, resource.begin() + 20);
  for (uint32_t i = 0; i < 4; ++i)
    Put(resource, 32 + i * 4, i + 1);
  Put(resource, 48, 12);
  std::copy_n("EARS_MESH", 9, resource.begin() + 52);
  Put(resource, 68, 4);
  Put(resource, 80, uint32_t(clump.size()));
  std::copy(clump.begin(), clump.end(), resource.begin() + 96);
  return resource;
}

void Writer() {
  const auto source = StaticFixture();
  const MeshResource mesh(source);
  auto document = mesh.Describe();
  Require(mesh.geometries().size() == 1 && mesh.geometries()[0].editable &&
              mesh.geometries()[0].triangles == std::vector<uint32_t>{0, 1, 2},
          "Typed static geometry reads positions and the original triangle-strip order");
  Require(mesh.Apply(document) == source, "No-op static edits preserve the entire named resource");
  Require(MeshResource(StaticFixture(false, 2)).geometries()[0].editable,
          "Both native buffer modes use the original initializer's single combined allocation");
  const auto points =
      Json::array({Json::array({-.5, -.5, 0}), Json::array({.5, -.5, 0}), Json::array({0, .5, 0})});
  document["changes"] = Json::array({{{"geometry", "geometry:0"}, {"positions", points}}});
  const auto edited = mesh.Apply(document);
  const MeshResource changed(edited);
  Require(changed.Describe()["geometries"][0]["positions"] == points,
          "Static position edits round-trip through the actual writer");
  const auto& g = mesh.geometries()[0];
  for (size_t i = 0; i < source.size(); ++i) {
    bool position_byte = false;
    for (uint32_t v = 0; v < g.vertices; ++v)
      if (i >= g.positions + v * g.stride && i < g.positions + v * g.stride + 12)
        position_byte = true;
    Require(position_byte || source[i] == edited[i],
            "Vertex edits preserve every non-position byte, including GPU metadata and indices");
  }
  auto stale = document;
  stale["source_sha256"] = std::string(64, '0');
  Reject([&] { mesh.Apply(stale); }, "Mesh source drift rejects before writes");
  auto bad = document;
  bad["changes"][0]["positions"][0][0] = 100;
  Reject([&] { mesh.Apply(bad); }, "Vertices cannot invalidate original culling bounds");
  bad = document;
  bad["geometries"][0]["triangles"] = {2, 1, 0};
  Reject([&] { mesh.Apply(bad); }, "Inspection topology cannot be silently changed");
  const MeshResource skin(StaticFixture(true));
  auto skin_doc = skin.Describe(false);
  skin_doc["changes"] = document["changes"];
  Reject([&] { skin.Apply(skin_doc); }, "Unqualified skinning layouts remain read-only");
  const auto catalog = ValidateMeshVariants({{"version", 1}, {"meshes", Json::array({document})}});
  Manifest a, b;
  a.id = "low";
  b.id = "high";
  a.mesh_edits = b.mesh_edits = std::make_shared<const Json>(catalog);
  const MeshVariants selected({a, b});
  Require(selected.Apply(source)->owner == "high" && selected.Apply(source)->bytes == edited,
          "Mesh selection follows whole-resource profile priority and uses owned prepared data");
  Reject([&] { selected.Apply(edited); },
         "A loaded variant still needs the exact original source identity");

  const auto temporary =
      fs::temp_directory_path() /
      ("simpsons-mesh-check-" +
       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  struct Cleanup {
    fs::path folder;
    ~Cleanup() {
      std::error_code ignored;
      fs::remove_all(folder, ignored);
    }
  } cleanup{temporary};
  const auto project = temporary / "static-prop";
  WriteText(project / "mod.json", Json{{"id", "test.mesh"},
                                       {"name", "Static prop"},
                                       {"version", "1.0.0"},
                                       {"kind", "data"},
                                       {"mesh_edits", "assets/meshes.json"}}
                                      .dump());
  WriteText(project / "assets/meshes.json", "{\"version\":true,\"meshes\":[]}");
  Reject([&] { Manifest::Read(project); },
         "Shared package validation rejects malformed mesh catalogs before Android import");
  WriteText(project / "assets/meshes.json", catalog.dump());
  auto captured = Manifest::Read(project);
  WriteText(project / "assets/meshes.json", catalog.dump(2));
  Reject([&] { PrepareCode(captured, temporary / "cache"); },
         "Mesh source drift cannot bypass its captured fingerprint");
  WriteText(project / "assets/meshes.json", catalog.dump());
  captured = Manifest::Read(project);
  PrepareCode(captured, temporary / "cache");
  fs::rename(project / "assets", project / "unavailable");
  Require(MeshVariants({captured}).Apply(source)->bytes == edited,
          "The live mesh path uses immutable prepared documents without reading package files");
  fs::rename(project / "unavailable", project / "assets");
  Runtime runtime({}, {});
  runtime.Load(temporary, temporary / "storage", {"test.mesh"});
  runtime.Tick(true, .01);
  auto replacement = document;
  replacement["changes"][0]["positions"][0][0] = -.25;
  WriteText(project / "assets/meshes.json",
            Json{{"version", 1}, {"meshes", Json::array({replacement})}}.dump());
  Reject([&] { PrepareSources(runtime.Sources(), temporary / "cache", true); },
         "Mesh changes require restart rather than an unsafe asset reload");
  Require(runtime.MeshEdits()->Apply(source)->bytes == edited,
          "Rejected mesh reload leaves the active prepared selection intact");
  runtime.Stop();
  Require(!runtime.MeshEdits(), "Stopping releases prepared mesh documents");

  const auto normal_source = StaticFixture(false, 1, true);
  const MeshResource normal_mesh(normal_source);
  auto normal_document = normal_mesh.Describe();
  const auto& ng = normal_mesh.geometries()[0];
  Require(ng.normals_editable && ng.normals.size() == 3 &&
              ng.normals[0] == std::array<float, 3>{0, 0, 1},
          "The reviewed signed packed normal declaration decodes XYZ");
  normal_document["changes"] = Json::array(
      {{{"geometry", "geometry:0"}, {"positions", ng.coordinates}, {"normals", ng.normals}}});
  Require(normal_mesh.Apply(normal_document) == normal_source,
          "Unchanged authored normals preserve every source bit, including W");
  auto legacy = normal_document;
  for (const auto* key : {"normals", "normals_editable", "normals_reason"})
    legacy["geometries"][0].erase(key);
  Require(normal_mesh.Apply(legacy) == normal_source,
          "Earlier position-only inspection documents remain compatible");
  normal_document["changes"][0]["normals"] = {{1, 0, 0}, {1, 0, 0}, {1, 0, 0}};
  const auto normal_edited = normal_mesh.Apply(normal_document);
  Require(MeshResource(normal_edited).geometries()[0].normals ==
              std::vector<std::array<float, 3>>(3, {1, 0, 0}),
          "Authored normal directions survive native packing and re-reading");
  for (size_t i = 0; i < normal_source.size(); ++i) {
    bool normal_byte = false;
    for (uint32_t v = 0; v < 3; ++v)
      if (i >= ng.normal_positions + v * ng.stride && i < ng.normal_positions + v * ng.stride + 4)
        normal_byte = true;
    Require(normal_byte || normal_source[i] == normal_edited[i],
            "Normal-only edits preserve all positions and other attributes");
  }
  for (uint32_t i = 0; i < 3; ++i)
    Require((simpsons::LoadGuestU32(normal_source.data(), ng.normal_positions + i * ng.stride) &
             0xC0000000) ==
                (simpsons::LoadGuestU32(normal_edited.data(), ng.normal_positions + i * ng.stride) &
                 0xC0000000),
            "Packed normal edits preserve the original fourth component");
  for (const auto& normal : std::vector<std::array<float, 3>>{{0, 0, 0}, {2, 0, 0}, {0, .1f, .1f}})
    Reject([&] { EncodeMeshNormal(0, normal); },
           "Invalid directions reject before modifying packed normals");
  Require(EncodeMeshNormal(0xC0000200, {-1, 0, 0}) == 0xC0000200,
          "The alternate negative-one source encoding is preserved exactly");
  auto invalid_normal = normal_document;
  invalid_normal["changes"][0]["normals"][0] = {0, 0, 0};
  Reject([&] { normal_mesh.Apply(invalid_normal); }, "Zero authored normals reject");
  Reject([&] { ValidateMeshVariants({{"version", 1}, {"meshes", Json::array({invalid_normal})}}); },
         "Package validation rejects malformed authored directions");
  auto absent_stream = document;
  absent_stream["changes"][0]["normals"] = {{1, 0, 0}, {1, 0, 0}, {1, 0, 0}};
  Reject([&] { mesh.Apply(absent_stream); },
         "Normal edits cannot target an absent or unreviewed stream");

  const auto uv_source = StaticFixture(true, 1, true, true);
  const MeshResource uv_mesh(uv_source);
  const auto& ug = uv_mesh.geometries()[0];
  Require(ug.skinned && !ug.editable && !ug.normals_editable && ug.uvs_editable &&
              ug.uvs == std::vector<std::array<float, 2>>{{-.25f, 1.5f}, {-0.0f, 2}, {.5f, -.75f}},
          "Independent float2 UVs are editable without exposing skinned positions or normals");
  auto uv_document = uv_mesh.Describe();
  uv_document["changes"] = Json::array({{{"geometry", "geometry:0"}, {"uvs", ug.uvs}}});
  Require(uv_mesh.Apply(uv_document) == uv_source,
          "UV-only no-op writes preserve signed zero and every original byte");
  const Json coordinates = {{.25, 1.25}, {-.5, 2.5}, {1, -.5}};
  uv_document["changes"][0]["uvs"] = coordinates;
  const auto uv_edited = uv_mesh.Apply(uv_document);
  Require(MeshResource(uv_edited).Describe()["geometries"][0]["uvs"] == coordinates,
          "Authored UVs round-trip without clamping repeats or flipping their orientation");
  for (size_t i = 0; i < uv_source.size(); ++i) {
    bool uv_byte = false;
    for (uint32_t v = 0; v < ug.vertices; ++v)
      if (i >= ug.uv_positions + v * ug.stride && i < ug.uv_positions + v * ug.stride + 8)
        uv_byte = true;
    Require(uv_byte || uv_source[i] == uv_edited[i],
            "UV-only edits preserve skin data, positions, normals, indices and all metadata");
  }
  auto old_inspection = uv_document;
  for (const auto* key : {"uvs", "uvs_editable", "uvs_reason"})
    old_inspection["geometries"][0].erase(key);
  Require(uv_mesh.Apply(old_inspection) == uv_edited,
          "The added UV inspection fields do not invalidate earlier projects");
  ValidateMeshVariants({{"version", 1}, {"meshes", Json::array({uv_document})}});
  auto invalid_uv = uv_document;
  invalid_uv["changes"][0]["positions"] = ug.coordinates;
  Reject([&] { uv_mesh.Apply(invalid_uv); },
         "A UV-only geometry cannot acquire position editing through its document");
  for (const Json bad :
       std::vector<Json>{Json::array({0}), Json::array({true, 0}),
                         Json::array({0, std::numeric_limits<double>::infinity()})}) {
    invalid_uv = uv_document;
    invalid_uv["changes"][0]["uvs"][0] = bad;
    Reject([&] { uv_mesh.Apply(invalid_uv); }, "Malformed UV values reject before native writes");
    Reject([&] { ValidateMeshVariants({{"version", 1}, {"meshes", Json::array({invalid_uv})}}); },
           "Shared package validation rejects malformed texture coordinates");
  }
  invalid_uv = uv_document;
  invalid_uv["changes"][0]["uvs"].erase(0);
  Reject([&] { uv_mesh.Apply(invalid_uv); }, "UV edits preserve the native vertex count");
  invalid_uv["changes"][0]["positions"] = ug.coordinates;
  Reject([&] { ValidateMeshVariants({{"version", 1}, {"meshes", Json::array({invalid_uv})}}); },
         "Attribute counts agree before package import");
  auto aliased = uv_source;
  // Replace the reviewed UV slot with an overlap inside the position stream.
  simpsons::StoreGuestU32(aliased.data(), ug.plugin + 24 + 256, 8);
  const MeshResource aliased_mesh(aliased);
  Require(!aliased_mesh.geometries()[0].uvs_editable,
          "Overlapping declaration slots remain read-only");
  auto nonfinite = uv_source;
  simpsons::StoreGuestU32(nonfinite.data(), ug.uv_positions, 0x7FC00000);
  const MeshResource nonfinite_mesh(nonfinite);
  Require(!nonfinite_mesh.geometries()[0].uvs_editable &&
              nonfinite_mesh.Apply(nonfinite_mesh.Describe(false)) == nonfinite,
          "Nonfinite unused UV data cannot break unchanged source preservation");
}
}  // namespace

int main(int argc, char** argv) {
  try {
    const auto fixture = Fixture();
    Oracle(NativeMeshData(fixture));
    Writer();
    for (const auto& mutation : std::vector<std::pair<uint32_t, uint32_t>>{{12, 0},
                                                                           {16, 0},
                                                                           {20, 0},
                                                                           {32, UINT32_MAX},
                                                                           {40, 0},
                                                                           {48, 64},
                                                                           {44, 2},
                                                                           {12 + 64, 129},
                                                                           {64, 0}}) {
      auto bad = fixture;
      simpsons::StoreGuestU32(bad.data(), mutation.first, mutation.second);
      Reject([&] { NativeMeshData ignored(bad); },
             "Malformed native mesh blocks reject before native execution");
    }
    Json failures = Json::array();
    uint64_t resources = 0, blocks = 0, skipped = 0, editable = 0, normals = 0, normal_vertices = 0;
    uint64_t uv_geometries = 0, uv_vertices = 0, skinned_uvs = 0, uv_resources = 0;
    if (argc == 5 && std::string_view(argv[1]) == "--stock") {
      const auto index = Json::parse(ReadText(fs::path(argv[3]), 64 * 1024 * 1024));
      std::map<std::pair<std::string, size_t>, std::vector<Json>> entries;
      for (const auto& item : index.at("resources")) {
        const auto name = item.at("name").get<std::string>();
        if (name.ends_with(".dff") || name.ends_with(".rws"))
          entries[{item.at("stream").get<std::string>(), item.at("entry").get<size_t>()}].push_back(
              item);
      }
      for (const auto& [key, items] : entries) {
        const auto data = StreamFile(fs::path(argv[2]) / Utf8Path(key.first)).Read(key.second);
        for (const auto& item : items) {
          try {
            const auto start = item.at("chunk_offset").get<size_t>(),
                       size = item.at("chunk_size").get<size_t>();
            if (start > data.size() || size > data.size() - start)
              throw Failure("source_changed", "Stock inventory no longer matches the stream");
            const auto bytes = std::span(data).subspan(start, size);
            const NamedResource named(bytes);
            if (named.native_class != "EARS_MESH") {
              ++skipped;
              continue;
            }
            const MeshResource mesh(bytes);
            ++resources;
            Require(mesh.Apply(mesh.Describe(false)) == mesh.data,
                    "A no-op mesh edit preserves the complete resource byte-for-byte");
            auto uv_round_trip = mesh.Describe(false);
            for (const auto& geometry : mesh.geometries()) {
              if (geometry.uvs_editable) {
                ++uv_geometries;
                uv_vertices += geometry.vertices;
                skinned_uvs += geometry.skinned;
                uv_round_trip["changes"].push_back(
                    {{"geometry", geometry.id}, {"uvs", geometry.uvs}});
              }
              if (geometry.editable)
                ++editable;
              if (geometry.normals_editable) {
                ++normals;
                for (uint32_t i = 0; i < geometry.vertices; ++i) {
                  const auto packed = simpsons::LoadGuestU32(
                      mesh.data.data(), geometry.normal_positions + i * geometry.stride);
                  Require(
                      EncodeMeshNormal(packed, geometry.normals[i]) == packed,
                      "All reviewed original normals round-trip without changing any packed bit");
                  ++normal_vertices;
                }
              }
              if (!geometry.plugin)
                continue;
              const auto at = geometry.plugin;
              const uint32_t length =
                  uint32_t(mesh.data[at + 4]) | uint32_t(mesh.data[at + 5]) << 8 |
                  uint32_t(mesh.data[at + 6]) << 16 | uint32_t(mesh.data[at + 7]) << 24;
              Oracle(NativeMeshData(std::span(mesh.data).subspan(at + 12, length)));
              ++blocks;
            }
            if (!uv_round_trip["changes"].empty()) {
              Require(
                  mesh.Apply(uv_round_trip) == mesh.data,
                  "Every reviewed static/skinned UV stream round-trips through the real writer");
              ++uv_resources;
            }
          } catch (const std::exception& error) {
            if (failures.size() < 32)
              failures.push_back({{"stream", key.first},
                                  {"entry", key.second},
                                  {"name", item.at("name")},
                                  {"error", error.what()}});
          }
        }
      }
    }
    const Json report{{"ok", failures.empty()},
                      {"checks", checks},
                      {"resources", resources},
                      {"native_blocks", blocks},
                      {"editable_geometries", editable},
                      {"normal_geometries", normals},
                      {"normal_vertices", normal_vertices},
                      {"uv_geometries", uv_geometries},
                      {"uv_vertices", uv_vertices},
                      {"skinned_uv_geometries", skinned_uvs},
                      {"uv_round_trip_resources", uv_resources},
                      {"non_mesh_resources", skipped},
                      {"failures", failures}};
    if (argc == 5)
      WriteText(fs::path(argv[4]), report.dump(2));
    if (argc == 3 && std::string_view(argv[1]) == "--report")
      WriteText(fs::path(argv[2]), report.dump(2));
    std::cout << report.dump() << '\n';
    return failures.empty() ? 0 : 1;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
