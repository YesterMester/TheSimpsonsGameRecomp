#include "meshes.h"
#include "sha256.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <set>

namespace simpsons::modkit {
namespace {
uint32_t Read(std::span<const uint8_t> data, uint32_t at, bool little = false) {
  if (uint64_t(at) + 4 > data.size())
    throw Failure("invalid_mesh", "Mesh field exceeds its data");
  const uint32_t value = uint32_t(data[at]) << 24 | uint32_t(data[at + 1]) << 16 |
                         uint32_t(data[at + 2]) << 8 | data[at + 3];
  if (!little)
    return value;
  return uint32_t(data[at]) | uint32_t(data[at + 1]) << 8 | uint32_t(data[at + 2]) << 16 |
         uint32_t(data[at + 3]) << 24;
}
void Range(std::span<const uint8_t> data, uint32_t at, uint64_t count) {
  if (uint64_t(at) + count > data.size())
    throw Failure("invalid_mesh", "Mesh data exceeds its block");
}
void Put(std::span<uint8_t> data, uint32_t at, uint32_t value) {
  Range(data, at, 4);
  for (uint32_t i = 0; i < 4; ++i)
    data[at + i] = uint8_t(value >> (24 - i * 8));
}
float Float(std::span<const uint8_t> data, uint32_t at, bool little = false) {
  const auto value = std::bit_cast<float>(Read(data, at, little));
  if (!std::isfinite(value))
    throw Failure("invalid_mesh", "Mesh coordinates must be finite");
  return value;
}
std::string Digest(std::span<const uint8_t> bytes) {
  return sha256::SHA256{}(bytes.data(), bytes.size());
}
bool UnitNormal(const std::array<float, 3>& normal) {
  double squared = 0;
  for (const float component : normal) {
    if (!std::isfinite(component) || component < -1 || component > 1)
      return false;
    squared += double(component) * component;
  }
  return squared >= .995 * .995 && squared <= 1.005 * 1.005;
}
uint32_t AttributeBytes(uint32_t type) {
  // The declaration's low six bits select the Xenos vertex format. Keep the
  // authoring core independent of the graphics SDK; these are its stored sizes.
  switch (type & 63) {
    case 6:
    case 7:
    case 16:
    case 17:
    case 25:
    case 31:
    case 33:
    case 36:
      return 4;
    case 26:
    case 32:
    case 34:
    case 37:
      return 8;
    case 57:
      return 12;
    case 35:
    case 38:
      return 16;
    default:
      return 0;
  }
}
bool ExclusiveAttribute(const NativeMeshData& block, uint32_t declaration, uint32_t count,
                        uint32_t stride, uint32_t offset, uint32_t size, uint32_t semantic) {
  if (uint64_t(offset) + size > stride)
    return false;
  for (uint32_t i = 0; i < count; ++i) {
    const auto entry = declaration + i * 12, location = block.Word(entry);
    if (location >> 16)
      continue;
    const auto other = location & 65535;
    if (other == offset && block.Word(entry + 8) == semantic)
      continue;
    const auto width = AttributeBytes(block.Word(entry + 4));
    if (!width || uint64_t(other) + width > stride ||
        (other < uint64_t(offset) + size && offset < uint64_t(other) + width))
      return false;
  }
  return true;
}
void ValidateMeshChange(const Json& change) {
  if (!change.is_object() || change.size() < 2 || change.size() > 4 ||
      !change.contains("geometry") || !change.at("geometry").is_string())
    throw Failure("invalid_mesh", "Mesh changes need a geometry and bounded vertex attributes");
  for (auto it = change.begin(); it != change.end(); ++it)
    if (it.key() != "geometry" && it.key() != "positions" && it.key() != "normals" &&
        it.key() != "uvs")
      throw Failure("invalid_mesh", "Unknown mesh attribute");
  const auto key = change.at("geometry").get<std::string>();
  const auto number = std::string_view(key).substr(std::min(size_t(9), key.size()));
  if (!key.starts_with("geometry:") || number.empty() || number.size() > 4 ||
      (number.size() > 1 && number.front() == '0') ||
      number.find_first_not_of("0123456789") != std::string_view::npos)
    throw Failure("invalid_mesh", "Mesh geometry IDs must identify source records");
  std::optional<size_t> count;
  for (const auto* field : {"positions", "normals", "uvs"}) {
    if (!change.contains(field))
      continue;
    const auto& values = change.at(field);
    if (!values.is_array() || values.size() > 1000000 || (count && *count != values.size()))
      throw Failure("invalid_mesh", "Edited vertex attributes must have matching bounded counts");
    count = values.size();
    const bool normal = std::string_view(field) == "normals";
    const uint32_t dimensions = std::string_view(field) == "uvs" ? 2 : 3;
    for (const auto& value : values) {
      if (!value.is_array() || value.size() != dimensions)
        throw Failure("invalid_mesh", "Vertex attribute has the wrong component count");
      std::array<float, 3> direction{};
      for (uint32_t axis = 0; axis < dimensions; ++axis) {
        const auto& component = value.at(axis);
        if (!component.is_number() || !std::isfinite(component.get<double>()) ||
            std::abs(component.get<double>()) >
                (normal ? 1.0 : double(std::numeric_limits<float>::max())))
          throw Failure("invalid_mesh", "Vertex attribute components must be finite floats");
        if (normal)
          direction[axis] = component.get<float>();
      }
      if (normal)
        EncodeMeshNormal(0, direction);
    }
  }
}
}  // namespace

std::array<float, 3> DecodeMeshNormal(uint32_t packed) {
  std::array<float, 3> result{};
  for (uint32_t i = 0; i < 3; ++i) {
    const auto bits = (packed >> (i * 10)) & 1023;
    const int32_t value = bits & 512 ? int32_t(bits) - 1024 : int32_t(bits);
    result[i] = std::max(-1.0f, float(value) / 511.0f);
  }
  return result;
}
uint32_t EncodeMeshNormal(uint32_t original, const std::array<float, 3>& normal) {
  if (!UnitNormal(normal))
    throw Failure("invalid_mesh", "Mesh normals must be finite unit vectors");
  const auto previous = DecodeMeshNormal(original);
  uint32_t result = original & 0xC0000000;
  for (uint32_t i = 0; i < 3; ++i) {
    const uint32_t component = normal[i] == previous[i]
                                   ? (original >> (i * 10)) & 1023
                                   : uint32_t(int32_t(std::lround(double(normal[i]) * 511))) & 1023;
    result |= component << (i * 10);
  }
  return result;
}

NativeMeshData::NativeMeshData(std::span<const uint8_t> plugin) {
  if (plugin.size() < 40 || plugin.size() > 16 * 1024 * 1024)
    throw Failure("resource_limit", "Native mesh block is outside its size budget");
  const auto alignment = Read(plugin, 0, true), primary = Read(plugin, 4, true),
             secondary = Read(plugin, 8, true);
  if (!alignment || alignment > 4096 || (alignment & (alignment - 1)) || primary < 28 ||
      uint64_t(primary) + secondary + 12 != plugin.size())
    throw Failure("invalid_mesh", "Invalid native mesh allocation header");
  main_.assign(plugin.begin() + 12, plugin.begin() + 12 + primary);
  secondary_.assign(plugin.begin() + 12 + primary, plugin.end());
  if (Word(0) != 0xBFBFBFBF || Word(4) != 0x01000000 || Word(8) != main_.size() ||
      Word(12) != secondary_.size())
    throw Failure("unsupported_mesh", "Unsupported native mesh block header");
  const auto count = Word(20), sections = Word(24);
  if (count > 65536 || sections > 4096 ||
      uint64_t(count) * 8 + uint64_t(sections) * 12 + 28 > main_.size())
    throw Failure("resource_limit", "Native mesh tables exceed their block");
  const uint32_t metadata_end = 28 + count * 8 + sections * 12;
  std::set<uint32_t> seen;
  for (uint32_t i = 0; i < count; ++i) {
    const auto field = Word(28 + i * 8), block = Word(32 + i * 8);
    if (field < metadata_end || field % 4 || block > 1 || !seen.insert(field).second)
      throw Failure("invalid_mesh",
                    "Native mesh relocations overlap metadata or use an unknown block");
    Range(main_, field, 4);
    const auto value = Word(field);
    const auto size = block ? secondary_.size() : main_.size();
    // The original relocation adds a base even to zero offsets. Preserve that
    // behavior rather than interpreting zero as an absent pointer.
    if (value > size || (!size && value))
      throw Failure("invalid_mesh", "Native mesh relocation exceeds its target block");
    relocations_.push_back({field, block});
  }
  for (uint32_t i = 0; i < sections; ++i) {
    const auto at = 28 + count * 8 + i * 12;
    Section section{Word(at), Word(at + 4), Word(at + 8)};
    if (section.offset < metadata_end)
      throw Failure("invalid_mesh", "Native mesh section overlaps metadata");
    Range(main_, section.offset, section.size);
    sections_.push_back(section);
  }
}
uint32_t NativeMeshData::Word(uint32_t at) const {
  return Read(main_, at);
}
std::vector<uint8_t> NativeMeshData::Relocated(uint32_t main_base, uint32_t secondary_base) const {
  if (uint64_t(main_base) + main_.size() > 0x100000000ull ||
      uint64_t(secondary_base) + secondary_.size() > 0x100000000ull || main_base % 4 ||
      secondary_base % 4)
    throw Failure("invalid_argument", "Native mesh relocation bases must fit a 32-bit address");
  auto result = main_;
  for (const auto& entry : relocations_) {
    const uint64_t pointer =
        uint64_t(Word(entry.field)) + (entry.block ? secondary_base : main_base);
    if (pointer > UINT32_MAX)
      throw Failure("invalid_mesh", "Native mesh pointer wraps its address");
    Put(result, entry.field, uint32_t(pointer));
  }
  return result;
}

std::vector<MeshResource::Chunk> MeshResource::Children(uint32_t at, uint32_t bytes) const {
  Range(data, at, bytes);
  std::vector<Chunk> result;
  const uint64_t end = uint64_t(at) + bytes;
  while (at < end) {
    if (end - at < 12 || result.size() >= 65536)
      throw Failure("invalid_mesh", "Mesh chunk table is truncated or exceeds its budget");
    Chunk chunk{Read(data, at, true), at, Read(data, at + 4, true)};
    if (chunk.size > end - at - 12)
      throw Failure("invalid_mesh", "Mesh chunk exceeds its parent");
    result.push_back(chunk);
    at += 12 + chunk.size;
  }
  return result;
}
MeshResource::MeshResource(std::span<const uint8_t> bytes) : NamedResource(bytes) {
  if (native_class != "EARS_MESH" || body_size < 12 || Read(data, body_offset, true) != 0x10 ||
      uint64_t(Read(data, body_offset + 4, true)) + 12 != body_size)
    throw Failure("unsupported_mesh", "Expected an EARS_MESH RenderWare clump");
  const auto clump = Children(body_offset + 12, body_size - 12);
  size_t lists = 0;
  for (const auto& child : clump) {
    if (child.type != 0x1A)
      continue;
    if (++lists != 1)
      throw Failure("invalid_mesh", "Clump has more than one geometry list");
    const auto children = Children(child.offset + 12, child.size);
    if (children.empty() || children[0].type != 1 || children[0].size != 4)
      throw Failure("invalid_mesh", "Geometry list has no count");
    const auto expected = Read(data, children[0].offset + 12, true);
    if (expected > 4096)
      throw Failure("resource_limit", "Geometry count exceeds its authoring budget");
    for (const auto& item : children)
      if (item.type == 0xF)
        GeometryAt(item, geometries_.size());
    if (geometries_.size() != expected)
      throw Failure("invalid_mesh", "Geometry count does not match its list");
  }
  if (lists != 1)
    throw Failure("invalid_mesh", "Clump has no geometry list");
}

void MeshResource::GeometryAt(const Chunk& chunk, size_t index) {
  Geometry geometry;
  geometry.id = "geometry:" + std::to_string(index);
  const auto children = Children(chunk.offset + 12, chunk.size);
  for (const auto& child : children) {
    if (child.type == 1) {
      if (geometry.structure || child.size != 40)
        throw Failure("unsupported_mesh", "Geometry uses an unreviewed structure layout");
      geometry.structure = child.offset + 12;
      geometry.vertices = Read(data, geometry.structure + 8, true);
      geometry.source_vertices = geometry.vertices;
      for (uint32_t i = 0; i < 4; ++i)
        geometry.sphere[i] = Float(data, geometry.structure + 16 + i * 4, true);
      if (geometry.sphere[3] < 0 || geometry.vertices > 1000000)
        throw Failure("invalid_mesh", "Invalid geometry sphere or vertex count");
    }
    if (child.type != 3)
      continue;
    for (const auto& extension : Children(child.offset + 12, child.size)) {
      if (extension.type == 0x116)
        geometry.skinned = true;
      if (extension.type == 0xEA16) {
        if (geometry.has_bounds || extension.size != 36)
          throw Failure("unsupported_mesh", "Geometry uses an unreviewed bounds layout");
        geometry.has_bounds = true;
        for (uint32_t i = 0; i < 6; ++i)
          geometry.bounds[i] = Float(data, extension.offset + 24 + i * 4, true);
      }
      if (extension.type == 0xEA33) {
        if (geometry.plugin)
          throw Failure("invalid_mesh", "Geometry has repeated native blocks");
        // Keep the descriptor's value, not a pointer into a temporary list.
        geometry.plugin = extension.offset;
      }
    }
  }
  if (!geometry.structure || !geometry.plugin) {
    geometry.reason = "Geometry has no reviewed preinstanced vertex data";
    geometries_.push_back(std::move(geometry));
    return;
  }
  const auto native_size = Read(data, geometry.plugin + 4, true);
  const NativeMeshData block{
      std::span<const uint8_t>(data).subspan(geometry.plugin + 12, native_size)};
  auto unsupported = [&](std::string message) {
    geometry.reason = std::move(message);
    geometries_.push_back(std::move(geometry));
  };
  if (block.sections().size() != 1 || block.sections()[0].type != 0x3C43A23D ||
      block.sections()[0].size != 56) {
    unsupported("Native geometry uses an unreviewed section layout");
    return;
  }
  const auto& bytes = block.main();
  const auto root = block.sections()[0].offset;
  // sub_8273B760 initializes one combined vertex/index allocation through
  // root +12. The adjacent words are preserved buffer modes, not array counts.
  // Both shipped modes use that same record layout and declaration stream.
  if (block.Word(root) != 0x00030002 ||
      (block.Word(root + 16) != 1 && block.Word(root + 16) != 2) ||
      (block.Word(root + 24) != 1 && block.Word(root + 24) != 2)) {
    unsupported("Native buffer mode needs a separately reviewed writer");
    return;
  }
  const auto record = block.Word(root + 12);
  Range(bytes, record, 48);
  const auto vertex_bytes = block.Word(record), stride = block.Word(record + 4);
  const auto count = block.Word(record + 8), declaration = block.Word(record + 12);
  const auto vertices = block.Word(record + 16), index_bytes = block.Word(record + 20);
  const auto index_type = block.Word(record + 24), indices = block.Word(record + 28);
  if (!stride || stride > 4096 || count > 256 || !count || vertex_bytes % stride ||
      vertex_bytes / stride > 1000000)
    throw Failure("invalid_mesh", "Native vertex allocation does not match the geometry");
  // Preinstancing may split or compact vertices. Draw indices address this
  // native allocation, rather than the source Geometry structure's old count.
  geometry.vertices = vertex_bytes / stride;
  Range(bytes, declaration, uint64_t(count) * 12);
  Range(bytes, vertices, vertex_bytes);
  Range(bytes, indices, index_bytes);
  uint32_t position = UINT32_MAX, normal = UINT32_MAX, uv = UINT32_MAX;
  bool normal_layout = true, uv_layout = true;
  for (uint32_t i = 0; i < count; ++i) {
    const auto entry = declaration + i * 12, stream_offset = block.Word(entry);
    if (stream_offset >> 16)
      continue;
    if (block.Word(entry + 8) == 0x00030000) {
      if (normal != UINT32_MAX || block.Word(entry + 4) != 0x002A2187)
        normal_layout = false;
      normal = stream_offset & 65535;
    }
    if (block.Word(entry + 8) == 0x00050000) {
      if (uv != UINT32_MAX || block.Word(entry + 4) != 0x002C23A5)
        uv_layout = false;
      uv = stream_offset & 65535;
    }
    if (block.Word(entry + 8) != 0)
      continue;
    if (position != UINT32_MAX || block.Word(entry + 4) != 0x002A23B9) {
      unsupported("Position declaration needs a separately reviewed decoder");
      return;
    }
    position = stream_offset & 65535;
  }
  if (position == UINT32_MAX || uint64_t(position) + 12 > stride) {
    unsupported("No bounded float3 position declaration");
    return;
  }
  geometry.stride = stride;
  geometry.positions = geometry.plugin + 24 + vertices + position;
  for (uint32_t i = 0; i < geometry.vertices; ++i) {
    const uint32_t at = vertices + i * stride + position;
    geometry.coordinates.push_back({Float(bytes, at), Float(bytes, at + 4), Float(bytes, at + 8)});
  }
  if (normal_layout && normal != UINT32_MAX && uint64_t(normal) + 4 <= stride &&
      (uint64_t(normal) + 4 <= position || normal >= uint64_t(position) + 12)) {
    // A second declaration at the same slot could give these bits another
    // meaning. Preserve that layout instead of changing two attributes at once.
    for (uint32_t i = 0; i < count; ++i) {
      const auto entry = declaration + i * 12;
      if (block.Word(entry) == normal && block.Word(entry + 8) != 0x00030000)
        normal_layout = false;
    }
    if (normal_layout) {
      geometry.normal_positions = geometry.plugin + 24 + vertices + normal;
      for (uint32_t i = 0; i < geometry.vertices; ++i)
        geometry.normals.push_back(DecodeMeshNormal(block.Word(vertices + i * stride + normal)));
    }
  }
  if (uv_layout && uv != UINT32_MAX &&
      ExclusiveAttribute(block, declaration, count, stride, uv, 8, 0x00050000)) {
    geometry.uv_positions = geometry.plugin + 24 + vertices + uv;
    bool finite = true;
    for (uint32_t i = 0; i < geometry.vertices; ++i) {
      const auto at = vertices + i * stride + uv;
      const std::array<float, 2> value{std::bit_cast<float>(block.Word(at)),
                                       std::bit_cast<float>(block.Word(at + 4))};
      finite &= std::isfinite(value[0]) && std::isfinite(value[1]);
      geometry.uvs.push_back(value);
    }
    // Bad unused UV data must not break an otherwise supported position edit.
    if (!finite) {
      geometry.uv_positions = 0;
      geometry.uvs.clear();
      geometry.uvs_reason = "Source texture coordinates are not finite";
    }
  }
  if (index_type != 1 || index_bytes % 2) {
    unsupported("Index format needs a separately reviewed decoder");
    return;
  }
  const auto index_record = block.Word(root + 20);
  Range(bytes, index_record, 32);
  if (block.Word(index_record + 12) != 6 || block.Word(index_record + 24) != index_bytes / 2) {
    unsupported("Primitive topology needs a separately reviewed decoder");
    return;
  }
  uint32_t a = 0, b = 0, length = 0;
  for (uint32_t i = 0; i < index_bytes; i += 2) {
    const auto value = uint32_t(bytes[indices + i]) << 8 | bytes[indices + i + 1];
    if (value == 65535) {
      length = 0;
      continue;
    }
    if (value >= geometry.vertices)
      throw Failure("invalid_mesh", "Native index exceeds the vertex buffer");
    if (length >= 2 && a != b && a != value && b != value) {
      geometry.triangles.push_back(length % 2 ? b : a);
      geometry.triangles.push_back(length % 2 ? a : b);
      geometry.triangles.push_back(value);
    }
    a = b;
    b = value;
    ++length;
  }
  geometry.editable = !geometry.skinned && geometry.has_bounds;
  if (!geometry.editable)
    geometry.reason = geometry.skinned ? "Skinned bounds and normals need a qualified writer"
                                       : "Geometry has no reviewed bounds";
  if (geometry.editable) {
    for (uint32_t axis = 0; axis < 3; ++axis)
      if (geometry.bounds[axis] > geometry.bounds[axis + 3])
        throw Failure("invalid_mesh", "Geometry bounds are inverted");
    const double tolerance = 0.00001 * std::max(1.0, double(geometry.sphere[3]));
    for (const auto& coordinate : geometry.coordinates) {
      double distance = 0;
      for (uint32_t axis = 0; axis < 3; ++axis) {
        const double delta = coordinate[axis] - geometry.sphere[axis];
        distance += delta * delta;
        if (coordinate[axis] < geometry.bounds[axis] - tolerance ||
            coordinate[axis] > geometry.bounds[axis + 3] + tolerance)
          geometry.editable = false;
      }
      if (std::sqrt(distance) > geometry.sphere[3] + tolerance)
        geometry.editable = false;
    }
    if (!geometry.editable)
      geometry.reason = "Native positions do not share the reviewed geometry bounds";
  }
  geometry.normals_editable =
      geometry.editable && geometry.normal_positions &&
      std::all_of(geometry.normals.begin(), geometry.normals.end(), UnitNormal);
  if (!geometry.normals_editable)
    geometry.normals_reason = !geometry.editable ? "Geometry is read-only"
                              : !geometry.normal_positions
                                  ? "No reviewed packed normal stream"
                                  : "Source normals are not finite unit vectors";
  // UV-only changes do not touch positions, skinning, normals or culling data.
  // The same reviewed stream is usable on both static and skinned geometry.
  geometry.uvs_editable = geometry.uv_positions != 0;
  if (!geometry.uvs_editable && geometry.uvs_reason.empty())
    geometry.uvs_reason = "No independent reviewed float2 texture-coordinate stream";
  geometries_.push_back(std::move(geometry));
}

Json MeshResource::Describe(bool include_geometry) const {
  Json result = {{"format", "EARS_MESH"},
                 {"version", 1},
                 {"name", name},
                 {"resource_id", resource_id},
                 {"source_sha256", Digest(data)},
                 {"changes", Json::array()}};
  if (!include_geometry)
    return result;
  Json geometries = Json::array();
  for (const auto& geometry : geometries_) {
    geometries.push_back({{"id", geometry.id},
                          {"editable", geometry.editable},
                          {"reason", geometry.reason},
                          {"skinned", geometry.skinned},
                          {"vertex_count", geometry.vertices},
                          {"source_vertex_count", geometry.source_vertices},
                          {"positions", geometry.coordinates},
                          {"normals", geometry.normals},
                          {"normals_editable", geometry.normals_editable},
                          {"normals_reason", geometry.normals_reason},
                          {"uvs", geometry.uvs},
                          {"uvs_editable", geometry.uvs_editable},
                          {"uvs_reason", geometry.uvs_reason},
                          {"triangles", geometry.triangles},
                          {"bounds", geometry.bounds},
                          {"sphere", geometry.sphere}});
  }
  result["geometries"] = std::move(geometries);
  return result;
}
std::vector<uint8_t> MeshResource::Apply(const Json& document) const {
  if (!document.is_object() || document.value("format", Json()) != "EARS_MESH" ||
      document.value("version", Json()) != 1 || !document.at("version").is_number_integer() ||
      document.value("resource_id", Json()) != resource_id ||
      document.value("source_sha256", Json()) != Digest(data))
    throw Failure("source_changed", "Mesh document does not match this exact resource");
  if (document.contains("geometries")) {
    auto description = Describe().at("geometries");
    const auto& inspected = document.at("geometries");
    // Position-only projects created before normal inspection remain valid.
    if (inspected.is_array() && inspected.size() == description.size())
      for (size_t i = 0; i < inspected.size(); ++i)
        for (const auto* key :
             {"normals", "normals_editable", "normals_reason", "uvs", "uvs_editable", "uvs_reason"})
          if (!inspected.at(i).contains(key))
            description.at(i).erase(key);
    if (inspected != description)
      throw Failure("invalid_argument",
                    "Mesh inspection metadata is read-only; edit the changes list");
  }
  const auto& changes = document.at("changes");
  if (!changes.is_array() || changes.size() > geometries_.size())
    throw Failure("invalid_argument", "Mesh changes need a bounded geometry array");
  if (changes.empty())
    return data;
  auto result = data;
  std::set<std::string> seen;
  for (const auto& change : changes) {
    ValidateMeshChange(change);
    const auto id = change.at("geometry").get<std::string>();
    const auto found = std::find_if(geometries_.begin(), geometries_.end(),
                                    [&](const auto& geometry) { return geometry.id == id; });
    if (!seen.insert(id).second || found == geometries_.end())
      throw Failure("unsupported_mesh", "Unknown or repeated mesh geometry");
    if (change.contains("positions")) {
      const auto& positions = change.at("positions");
      if (!found->editable || positions.size() != found->vertices)
        throw Failure("unsupported_mesh",
                      "Mesh edits must preserve the original vertex count and order");
      for (uint32_t i = 0; i < found->vertices; ++i) {
        if (!positions.at(i).is_array() || positions.at(i).size() != 3)
          throw Failure("invalid_argument", "Each vertex needs three finite coordinates");
        double distance = 0;
        for (uint32_t axis = 0; axis < 3; ++axis) {
          if (!positions.at(i).at(axis).is_number())
            throw Failure("invalid_argument", "Vertex coordinates must be numeric");
          const double value = positions.at(i).at(axis).get<double>();
          const double tolerance = 0.00001 * std::max(1.0, double(found->sphere[3]));
          if (!std::isfinite(value) || value < found->bounds[axis] - tolerance ||
              value > found->bounds[axis + 3] + tolerance)
            throw Failure("unsupported_mesh", "Vertex edits must stay inside the original bounds");
          const double delta = value - found->sphere[axis];
          distance += delta * delta;
          Put(result, found->positions + i * found->stride + axis * 4,
              std::bit_cast<uint32_t>(float(value)));
        }
        if (std::sqrt(distance) >
            found->sphere[3] + 0.00001 * std::max(1.0, double(found->sphere[3])))
          throw Failure("unsupported_mesh", "Vertex edits must stay inside the original sphere");
      }
    }
    if (change.contains("normals")) {
      const auto& normals = change.at("normals");
      if (!found->normals_editable || !normals.is_array() || normals.size() != found->vertices)
        throw Failure("unsupported_mesh", "Mesh normals need the reviewed static normal stream");
      for (uint32_t i = 0; i < found->vertices; ++i) {
        if (!normals.at(i).is_array() || normals.at(i).size() != 3)
          throw Failure("invalid_mesh", "Each normal needs three finite components");
        std::array<float, 3> value{};
        for (uint32_t axis = 0; axis < 3; ++axis) {
          const auto& component = normals.at(i).at(axis);
          if (!component.is_number() || !std::isfinite(component.get<double>()) ||
              std::abs(component.get<double>()) > 1)
            throw Failure("invalid_mesh", "Mesh normals must be finite unit vectors");
          value[axis] = component.get<float>();
        }
        const auto at = found->normal_positions + i * found->stride;
        Put(result, at, EncodeMeshNormal(Read(data, at), value));
      }
    }
    if (change.contains("uvs")) {
      const auto& uvs = change.at("uvs");
      if (!found->uvs_editable || uvs.size() != found->vertices)
        throw Failure("unsupported_mesh", "UV edits need the reviewed texture-coordinate stream");
      for (uint32_t i = 0; i < found->vertices; ++i)
        for (uint32_t axis = 0; axis < 2; ++axis)
          Put(result, found->uv_positions + i * found->stride + axis * 4,
              std::bit_cast<uint32_t>(uvs.at(i).at(axis).get<float>()));
    }
  }
  MeshResource check(result);
  return result;
}

Json ValidateMeshVariants(const Json& catalog) {
  if (!catalog.is_object() || catalog.size() != 2 || !catalog.contains("version") ||
      !catalog.at("version").is_number_integer() || catalog.at("version") != 1 ||
      !catalog.contains("meshes") || !catalog.at("meshes").is_array() ||
      catalog.at("meshes").size() > 64)
    throw Failure("invalid_mesh", "A mesh catalog needs version 1 and at most 64 resources");
  std::set<std::string> seen;
  for (const auto& document : catalog.at("meshes")) {
    if (!document.is_object() || document.value("format", Json()) != "EARS_MESH" ||
        !document.contains("version") || !document.at("version").is_number_integer() ||
        document.at("version") != 1 || !document.contains("resource_id") ||
        !document.at("resource_id").is_string() || !document.contains("source_sha256") ||
        !document.at("source_sha256").is_string() || !document.contains("changes") ||
        !document.at("changes").is_array() || document.at("changes").empty() ||
        document.at("changes").size() > 4096)
      throw Failure("invalid_mesh",
                    "Mesh edits need a native identity, source hash and geometry changes");
    const auto id = document.at("resource_id").get<std::string>(),
               hash = document.at("source_sha256").get<std::string>();
    if (id.size() != 32 || id.find_first_not_of("0123456789ABCDEF") != std::string::npos ||
        id == std::string(32, '0') || hash.size() != 64 ||
        hash.find_first_not_of("0123456789abcdef") != std::string::npos || !seen.insert(id).second)
      throw Failure("invalid_mesh",
                    "Mesh edits need unique identities and lowercase source SHA-256");
    std::set<std::string> changed;
    for (const auto& change : document.at("changes")) {
      ValidateMeshChange(change);
      if (!changed.insert(change.at("geometry").get<std::string>()).second)
        throw Failure("invalid_mesh", "Mesh geometry IDs must be unique source records");
    }
  }
  return catalog;
}
MeshVariants::MeshVariants(const std::vector<Manifest>& manifests) {
  size_t total = 0, bytes = 0;
  for (const auto& mod : manifests) {
    if (!mod.mesh_edits)
      continue;
    const auto catalog = ValidateMeshVariants(*mod.mesh_edits);
    const auto size = catalog.dump().size();
    if (size > 64 * 1024 * 1024 - bytes)
      throw Failure("resource_limit", "Prepared mesh edits exceed 64 MiB");
    bytes += size;
    for (const auto& document : catalog.at("meshes")) {
      if (++total > 1024)
        throw Failure("resource_limit", "A profile supports at most 1024 edited mesh resources");
      meshes_[document.at("resource_id").get<std::string>()] = {mod.id, document};
    }
  }
}
bool MeshVariants::Contains(std::string_view id) const {
  return meshes_.contains(id);
}
std::optional<MeshVariants::Result> MeshVariants::Apply(std::span<const uint8_t> bytes) const {
  const NamedResource resource(bytes);
  const auto found = meshes_.find(resource.resource_id);
  if (found == meshes_.end())
    return {};
  try {
    return Result{found->second.owner, MeshResource(bytes).Apply(found->second.document)};
  } catch (const std::exception& error) {
    throw Failure("invalid_mesh",
                  found->second.owner + " mesh " + found->first + ": " + error.what());
  }
}

}  // namespace simpsons::modkit
