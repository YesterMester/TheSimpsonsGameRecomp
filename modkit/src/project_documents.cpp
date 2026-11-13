#include "project_documents.h"

#include "missions.h"
#include "scenes.h"
#include "source_state.h"

#include <algorithm>
#include <cmath>
#include <set>

namespace simpsons::modkit {
namespace {
void Check(bool condition, const char* message) {
  if (!condition) throw Failure("invalid_document", message);
}
void Keys(const Json& value, std::initializer_list<std::string_view> allowed) {
  Check(value.is_object(), "A mission document needs object records");
  for (auto it = value.begin(); it != value.end(); ++it)
    Check(std::find(allowed.begin(), allowed.end(), it.key()) != allowed.end(),
          "Unsupported mission document field");
}
std::string Text(const Json& value, size_t limit, bool empty = false) {
  Check(value.is_string(), "Mission names and ids must be text");
  auto text = value.get<std::string>();
  Check((empty || !text.empty()) && text.size() <= limit, "Mission text exceeds its bounds");
  for (const unsigned char c : text) Check(c >= 32 && c != 127, "Mission text contains a control character");
  return text;
}
double Number(const Json& value, double low, double high) {
  Check(value.is_number(), "Mission coordinates and durations must be numbers");
  const auto n = value.get<double>();
  Check(std::isfinite(n) && n >= low && n <= high, "Mission number exceeds its bounds");
  return n;
}
Json Position(const Json& value) {
  Keys(value, {"x", "y", "z"});
  Json result = Json::object();
  for (const auto* axis : {"x", "y", "z"}) {
    Check(value.contains(axis), "A mission point needs x, y and z");
    result[axis] = float(Number(value.at(axis), -1000000, 1000000));
  }
  return result;
}
bool Boolean(const Json& value) {
  Check(value.is_boolean(), "Mission switches must be boolean");
  return value.get<bool>();
}
Json MissionDocument(const Json& value) {
  Keys(value, {"schema", "name", "description", "level_root", "revision", "time_limit",
               "fail_on_defeat", "auto_start", "objectives"});
  Check(value.contains("schema") && value.at("schema").is_number_integer() && value.at("schema") == 1,
        "Unsupported mission document version");
  Check(value.contains("name") && value.contains("level_root") && value.contains("objectives"),
        "A mission document needs a name, episode and objectives");
  auto root = Text(value.at("level_root"), 200, true);
  if (!root.empty()) {
    const auto path = source_state::Path(root);
    Check(bool(path), "A mission episode must be a relative .str path");
    const auto slash = path->find('/');
    Check(slash != std::string::npos && path->find('/', slash + 1) == std::string::npos &&
              path->substr(slash + 1) == path->substr(0, slash) + ".str",
          "A mission episode must name its original episode catalog");
    root = *path;
  }
  const auto revision = value.value("revision", Json(1));
  Check(revision.is_number_integer() && Number(revision, 1, UINT32_MAX) >= 1,
        "Mission document revision must be a positive 32-bit integer");
  auto objects = value.at("objectives");
  if (objects.is_object() && objects.empty()) objects = Json::array();
  Check(objects.is_array() && objects.size() <= 32 && (objects.empty() || !root.empty()),
        "A mission has up to 32 objectives in one episode");
  Json result = {{"schema", 1}, {"name", Text(value.at("name"), 160)},
      {"description", Text(value.value("description", Json("")), 1024, true)}, {"level_root", root},
      {"revision", revision}, {"time_limit", Number(value.value("time_limit", Json(0)), 0, 86400)},
      {"fail_on_defeat", Boolean(value.value("fail_on_defeat", Json(true)))},
      {"auto_start", Boolean(value.value("auto_start", Json(false)))}, {"objectives", Json::array()}};
  for (const auto& item : objects) {
    Keys(item, {"id", "title", "kind", "position", "radius", "seconds", "requires", "optional", "checkpoint"});
    Check(item.contains("id") && item.contains("title") && item.contains("kind"),
          "A mission objective needs a stable id, title and kind");
    const auto kind = Text(item.at("kind"), 16);
    Check(kind == "reach" || kind == "stay" || kind == "wait", "Unsupported mission objective kind");
    Check(kind == "wait" || item.contains("position"), "A reach or stay objective needs its authored point");
    const float radius = float(Number(item.value("radius", Json(3)), 0, 1000));
    Check(radius > 0, "A mission radius cannot be zero or underflow float32");
    const auto seconds = Number(item.value("seconds", Json(2)), .1, 86400);
    auto dependencies = item.value("requires", Json::array());
    if (dependencies.is_object() && dependencies.empty()) dependencies = Json::array();
    result["objectives"].push_back({{"id", Text(item.at("id"), 80)},
        {"title", Text(item.at("title"), 240)}, {"kind", kind},
        {"position", Position(item.value("position", Json{{"x", 0}, {"y", 0}, {"z", 0}}))},
        {"radius", radius}, {"seconds", seconds}, {"requires", dependencies},
        {"optional", Boolean(item.value("optional", Json(false)))},
        {"checkpoint", Boolean(item.value("checkpoint", Json(false)))}});
  }
  // The native mission service remains the objective graph/contract oracle.
  // Validate in a temporary owner, without registering anything in the game.
  if (!result.at("objectives").empty()) {
    Missions temporary;
    auto native = temporary.Call("document", "missions.register",
                                 {{"definition", MissionProjectDefinition(result)}});
    for (size_t i = 0; i < result["objectives"].size(); ++i)
      result["objectives"][i]["requires"] = native.at("objectives")[i].at("requires");
  }
  Check(result.dump().size() <= 64 * 1024, "Mission documents must fit 64 KiB");
  return result;
}
}  // namespace

Json EmptyProjectDocument(std::string_view kind) {
  if (kind == "scene") return EmptyScene();
  if (kind == "mission") return {{"schema", 1}, {"name", "My mission"}, {"description", ""},
      {"level_root", ""}, {"revision", 1}, {"time_limit", 0.0}, {"fail_on_defeat", true},
      {"auto_start", false}, {"objectives", Json::array()}};
  throw Failure("unsupported", "Choose a scene or mission project document");
}
Json ValidateProjectDocument(std::string_view kind, const Json& document) {
  if (kind == "scene") return ValidateScene(document);
  if (kind == "mission") return MissionDocument(document);
  throw Failure("unsupported", "Choose a scene or mission project document");
}
Json MissionProjectDefinition(const Json& document) {
  Json result = {{"id", "authored-mission"}, {"title", document.at("name")},
      {"description", document.at("description")}, {"revision", document.at("revision")},
      {"objectives", Json::array()}};
  for (const auto& item : document.at("objectives"))
    result["objectives"].push_back({{"id", item.at("id")}, {"title", item.at("title")},
        {"goal", item.at("kind") == "reach" ? Json(1) : item.at("seconds")},
        {"optional", item.at("optional")}, {"requires", item.at("requires")}});
  return result;
}

}  // namespace simpsons::modkit
