#include "scenes.h"

#include "source_state.h"

#include <array>
#include <cmath>
#include <limits>
#include <set>

namespace simpsons::modkit {
namespace {
void Check(bool valid, const char* text) {
  if (!valid) throw Failure("invalid_scene", text);
}
void Keys(const Json& value, std::initializer_list<std::string_view> allowed) {
  Check(value.is_object(), "Scene records must be objects");
  for (auto field = value.begin(); field != value.end(); ++field) {
    bool found = false;
    for (const auto key : allowed) found |= field.key() == key;
    Check(found, "A scene record has an unsupported field");
  }
}
std::string Text(const Json& value, size_t maximum, bool empty = false) {
  Check(value.is_string(), "Scene names and identities must be text");
  const auto text = value.get<std::string>();
  Check((empty || !text.empty()) && text.size() <= maximum, "Scene text exceeds its bounds");
  for (const unsigned char c : text) Check(c >= 32 && c != 127, "Scene text contains a control character");
  return text;
}
std::string Guid(const Json& value) {
  auto result = Text(value, 32);
  Check(result.size() == 32, "A scene source needs its original 32-digit definition GUID");
  bool nonzero = false;
  for (auto& c : result) {
    if (c >= 'a' && c <= 'f') c -= 'a' - 'A';
    Check((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'), "Invalid scene definition GUID");
    nonzero |= c != '0';
  }
  Check(nonzero, "A scene source cannot use a null GUID");
  return result;
}
std::array<float, 3> Vector(const Json& value) {
  Keys(value, {"x", "y", "z"});
  std::array<float, 3> result;
  size_t index = 0;
  for (const auto* axis : {"x", "y", "z"}) {
    Check(value.contains(axis) && value.at(axis).is_number(), "Scene coordinates must be numbers");
    const auto coordinate = value.at(axis).get<double>();
    Check(std::isfinite(coordinate) && std::abs(coordinate) <= 1000000,
          "Scene coordinates must be finite and within one million units");
    result[index++] = float(coordinate);
  }
  return result;
}
Json Record(const std::array<float, 3>& value) {
  return {{"x", value[0]}, {"y", value[1]}, {"z", value[2]}};
}
Json Pose(const Json& value) {
  Keys(value, {"position", "right", "up", "forward"});
  for (const auto* field : {"position", "right", "up", "forward"})
    Check(value.contains(field), "A scene pose needs position and all three basis vectors");
  const auto r = Vector(value.at("right")), u = Vector(value.at("up")), f = Vector(value.at("forward"));
  double lengths[3];
  const std::array<float, 3> axes[] = {r, u, f};
  for (size_t i = 0; i < 3; ++i) {
    double squared = 0;
    for (float component : axes[i]) squared += double(component) * component;
    Check(squared >= double(std::numeric_limits<float>::min()) * 4 &&
              squared <= double(std::numeric_limits<float>::max()) / 4,
          "Scene basis lengths are outside the native float32 range");
    lengths[i] = std::sqrt(squared);
  }
  const double determinant = double(r[0]) * (double(u[1]) * f[2] - double(u[2]) * f[1]) -
      double(r[1]) * (double(u[0]) * f[2] - double(u[2]) * f[0]) +
      double(r[2]) * (double(u[0]) * f[1] - double(u[1]) * f[0]);
  Check(std::abs(determinant) > lengths[0] * lengths[1] * lengths[2] * 1e-8,
        "Scene basis vectors must form a nonsingular native pose");
  return {{"position", Record(Vector(value.at("position")))}, {"right", Record(r)},
          {"up", Record(u)}, {"forward", Record(f)}};
}
}  // namespace

bool ObjectTag(std::string_view tag) {
  if (tag.empty() || tag.size() > 64) return false;
  for (const unsigned char c : tag)
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.'))
      return false;
  return true;
}

Json EmptyScene() {
  return {{"schema", 1}, {"name", "My scene"}, {"level_root", ""},
          {"autoload", false}, {"objects", Json::array()}};
}

Json ValidateScene(const Json& document) {
  Keys(document, {"schema", "name", "level_root", "autoload", "objects"});
  Check(document.contains("schema") && document.at("schema").is_number_integer() &&
            document.at("schema") == 1, "Unsupported scene document version");
  Check(document.contains("name") && document.contains("level_root") && document.contains("objects"),
        "A scene needs its name, level root and object list");
  auto root = Text(document.at("level_root"), 200, true);
  if (!root.empty()) {
    const auto path = source_state::Path(root);
    Check(bool(path), "A scene level root must be an installed relative .str path");
    const auto slash = path->find('/');
    Check(slash != std::string::npos && path->find('/', slash + 1) == std::string::npos &&
              path->substr(0, slash) == path->substr(slash + 1, path->size() - slash - 5),
          "A scene level root must name its episode catalog");
    root = *path;
  }
  const auto& objects = document.at("objects");
  // Lua represents an empty JSON array as an empty table.
  Check(objects.is_array() || (objects.is_object() && objects.empty()), "Scene objects must be an array");
  Check(objects.size() <= 64 && (objects.empty() || !root.empty()),
        "A scene needs a level root and supports at most 64 objects");
  Check(!document.contains("autoload") || document.at("autoload").is_boolean(),
        "Scene autoload must be boolean");
  Json result = {{"schema", 1}, {"name", Text(document.at("name"), 160)}, {"level_root", root},
      {"autoload", document.value("autoload", false)}, {"objects", Json::array()}};
  std::set<std::string> ids;
  for (const auto& object : objects) {
    Keys(object, {"id", "name", "source", "transform", "radius"});
    Check(object.contains("id") && object.contains("name") && object.contains("source") &&
              object.contains("transform"), "A scene object is missing an identity, source or pose");
    const auto id = Text(object.at("id"), 64);
    Check(ObjectTag(id) && ids.insert(id).second, "Scene object ids must be unique lowercase tags");
    const auto& source = object.at("source");
    Keys(source, {"guid", "class", "stream"});
    Check(source.contains("guid") && source.contains("class") && source.contains("stream"),
          "A scene object needs its definition GUID, native class and source stream");
    const auto path = source_state::Path(Text(source.at("stream"), 200));
    Check(bool(path), "Invalid scene object source path");
    const auto radius = object.value("radius", Json(1));
    Check(radius.is_number() && std::isfinite(radius.get<double>()) &&
              float(radius.get<double>()) > 0 && radius.get<double>() <= 1000, "Invalid scene object spawn radius");
    result["objects"].push_back({{"id", id}, {"name", Text(object.at("name"), 160)},
        {"source", {{"guid", Guid(source.at("guid"))}, {"class", Text(source.at("class"), 160)},
                    {"stream", *path}}}, {"transform", Pose(object.at("transform"))},
        {"radius", float(radius.get<double>())}});
  }
  Check(result.dump().size() <= 128 * 1024, "Scene documents are limited to 128 KiB");
  return result;
}

}  // namespace simpsons::modkit
