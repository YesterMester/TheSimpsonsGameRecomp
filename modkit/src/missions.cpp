#include "missions.h"

#include <algorithm>
#include <cmath>
#include <set>

namespace simpsons::modkit {
namespace {
constexpr size_t kOwnerBytes = 256 * 1024;
bool Id(const Json& value) {
  if (!value.is_string())
    return false;
  const auto& id = value.get_ref<const std::string&>();
  return !id.empty() && id.size() <= 80 && std::all_of(id.begin(), id.end(), [](unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
  });
}
void Require(bool condition, std::string message) {
  if (!condition)
    throw Failure("invalid_mission", std::move(message));
}
bool Number(const Json& value, double minimum, double maximum) {
  return value.is_number() && std::isfinite(value.get<double>()) &&
         value.get<double>() >= minimum && value.get<double>() <= maximum;
}
void Fields(const Json& object, std::initializer_list<std::string_view> names) {
  Require(object.is_object(), "Mission descriptions and saved records must be objects");
  for (auto it = object.begin(); it != object.end(); ++it)
    Require(std::find(names.begin(), names.end(), it.key()) != names.end(),
            "Unknown mission field: " + it.key());
}
std::string Text(const Json& object, std::string_view name, size_t maximum, bool optional = false) {
  const auto key = std::string(name);
  if (optional && !object.contains(key))
    return "";
  Require(object.contains(key) && object.at(key).is_string(), "Missing mission text: " + key);
  const auto result = object.at(key).get<std::string>();
  Require(result.size() <= maximum && (optional || !result.empty()),
          "Mission text does not fit: " + key);
  Require(result.find('\0') == std::string::npos, "Mission text cannot contain null characters");
  return result;
}
}  // namespace

Json Missions::Definition(const Json& value) {
  Fields(value, {"id", "title", "description", "revision", "objectives"});
  Require(value.contains("id") && Id(value.at("id")), "Missions need a stable local id");
  const auto revision = value.value("revision", Json(1));
  Require(revision.is_number_integer() && Number(revision, 1, UINT32_MAX),
          "Mission revision must be a positive integer");
  Json result = {{"id", value.at("id")},
                 {"revision", revision},
                 {"title", Text(value, "title", 160)},
                 {"description", Text(value, "description", 1024, true)},
                 {"objectives", Json::array()}};
  Require(value.contains("objectives") && value.at("objectives").is_array() &&
              !value.at("objectives").empty() && value.at("objectives").size() <= 32,
          "A mission needs 1 to 32 objectives");
  std::set<std::string> ids;
  bool required = false;
  for (const auto& source : value.at("objectives")) {
    Fields(source, {"id", "title", "goal", "optional", "requires"});
    Require(source.contains("id") && Id(source.at("id")), "Objectives need stable local ids");
    const auto id = source.at("id").get<std::string>();
    Require(ids.insert(id).second, "Duplicate objective id: " + id);
    const auto goal = source.value("goal", Json(1));
    Require(Number(goal, 0, 1e9) && goal.get<double>() > 0,
            "Objective goal must be finite, positive and at most one billion");
    const auto optional = source.value("optional", Json(false));
    Require(optional.is_boolean(), "Objective optional must be boolean");
    auto dependencies = source.value("requires", Json::array());
    // Lua's empty table represents an empty object.
    if (dependencies.is_object() && dependencies.empty())
      dependencies = Json::array();
    Require(dependencies.is_array() && dependencies.size() <= 32,
            "Objective prerequisites must be an array of ids");
    std::set<std::string> parents;
    for (const auto& parent : dependencies) {
      Require(Id(parent), "Objective prerequisites need valid ids");
      Require(parent != id && parents.insert(parent.get<std::string>()).second,
              "Objective prerequisites cannot repeat or refer to themselves: " + id);
    }
    result["objectives"].push_back({{"id", id},
                                    {"title", Text(source, "title", 240)},
                                    {"goal", goal},
                                    {"optional", optional},
                                    {"requires", parents}});
    required |= !optional.get<bool>();
  }
  Require(required, "A mission needs at least one required objective");
  std::map<std::string, int> visited;
  std::function<void(const std::string&)> visit = [&](const std::string& id) {
    Require(visited[id] != 1, "Objective prerequisites contain a cycle at " + id);
    if (visited[id] == 2)
      return;
    visited[id] = 1;
    const auto& objectives = result.at("objectives");
    const auto found = std::find_if(objectives.begin(), objectives.end(),
                                    [&](const auto& o) { return o.at("id") == id; });
    for (const auto& parent : found->at("requires")) {
      Require(ids.contains(parent.get<std::string>()),
              "Unknown objective prerequisite: " + parent.get<std::string>());
      visit(parent.get<std::string>());
    }
    visited[id] = 2;
  };
  for (const auto& id : ids)
    visit(id);
  Require(result.dump().size() <= 65536, "Mission definitions must fit 64 KiB");
  return result;
}

Json Missions::Contract(const Json& definition) {
  Json objectives = Json::object();
  for (auto objective : definition.at("objectives")) {
    const auto id = objective.at("id").get<std::string>();
    objective.erase("id");
    objective.erase("title");
    objectives[id] = std::move(objective);
  }
  return {{"revision", definition.at("revision")}, {"objectives", std::move(objectives)}};
}
Missions::Mission Missions::Initial(Json definition) {
  Mission mission;
  mission.definition = std::move(definition);
  mission.progress = Json::object();
  for (const auto& objective : mission.definition.at("objectives"))
    mission.progress[objective.at("id").get<std::string>()] = 0.0;
  return mission;
}
bool Missions::Ready(const Mission& mission, const Json& objective) {
  for (const auto& id : objective.at("requires")) {
    const auto& all = mission.definition.at("objectives");
    const auto parent =
        std::find_if(all.begin(), all.end(), [&](const auto& o) { return o.at("id") == id; });
    if (mission.progress.at(id.get<std::string>()).get<double>() < parent->at("goal").get<double>())
      return false;
  }
  return true;
}
bool Missions::Complete(const Mission& mission) {
  for (const auto& objective : mission.definition.at("objectives"))
    if (!objective.at("optional").get<bool>() &&
        mission.progress.at(objective.at("id").get<std::string>()).get<double>() <
            objective.at("goal").get<double>())
      return false;
  return true;
}
Json Missions::View(const Mission& mission) {
  Json result = mission.definition;
  result["status"] = mission.status;
  result["reason"] = mission.reason;
  result["has_checkpoint"] = !mission.checkpoint.is_null();
  for (auto& objective : result["objectives"]) {
    const auto progress = mission.progress.at(objective.at("id").get<std::string>()).get<double>();
    objective["progress"] = progress;
    objective["state"] = progress >= objective.at("goal").get<double>() ? "completed"
                         : mission.status == "idle"                     ? "pending"
                         : !Ready(mission, objective)                   ? "locked"
                         : mission.status == "active"                   ? "active"
                                                                        : "inactive";
  }
  return result;
}
Json Missions::Stored(const Mission& mission) {
  return {{"definition", mission.definition},
          {"status", mission.status},
          {"progress", mission.progress},
          {"reason", mission.reason},
          {"checkpoint", mission.checkpoint}};
}
Json Missions::Empty() {
  return {{"schema", 1}, {"missions", Json::array()}, {"pending", Json::array()}};
}
Json Missions::Save(std::string_view owner) const {
  auto result = Empty();
  for (const auto& [key, mission] : missions_)
    if (key.first == owner)
      result["missions"].push_back(Stored(mission));
  for (const auto& [key, change] : changes_)
    if (key.first == owner)
      result["pending"].push_back({{"id", key.second}, {"change", change.reason}});
  return result;
}
std::vector<Missions::Mission> Missions::ReadSaved(const Json& state) {
  try {
    Fields(state, {"schema", "missions", "pending"});
    Require(state.value("schema", 0) == 1 && state.contains("missions") &&
                state.at("missions").is_array() && state.at("missions").size() <= 16 &&
                state.dump().size() <= kOwnerBytes,
            "Invalid saved mission collection");
    std::vector<Mission> result;
    std::set<std::string> ids;
    for (const auto& row : state.at("missions")) {
      Fields(row, {"definition", "status", "progress", "reason", "checkpoint"});
      auto mission = Initial(Definition(row.at("definition")));
      Require(ids.insert(mission.definition.at("id").get<std::string>()).second,
              "Duplicate saved mission");
      mission.status = row.at("status").get<std::string>();
      Require(mission.status == "idle" || mission.status == "active" ||
                  mission.status == "failed" || mission.status == "completed",
              "Unknown saved mission status");
      mission.reason = Text(row, "reason", 512, true);
      Require((mission.status == "failed") == !mission.reason.empty(),
              "Only failed missions have a failure reason");
      const auto validate_progress = [&](const Json& progress, std::string_view status) {
        Require(progress.is_object() && progress.size() == mission.progress.size(),
                "Saved objective set changed");
        auto check = mission;
        check.progress = progress;
        for (const auto& objective : check.definition.at("objectives")) {
          const auto id = objective.at("id").get<std::string>();
          Require(progress.contains(id) &&
                      Number(progress.at(id), 0, objective.at("goal").get<double>()),
                  "Invalid saved progress for " + id);
          Require(status != "idle" || progress.at(id) == 0, "Idle missions cannot have progress");
          Require(progress.at(id) == 0 || Ready(check, objective),
                  "Locked objectives cannot have progress");
        }
        Require((status == "completed") == Complete(check),
                "Saved completion does not match objective progress");
      };
      validate_progress(row.at("progress"), mission.status);
      mission.progress = row.at("progress");
      // Lua omits nil-valued object fields when a state.loaded callback
      // round-trips its snapshot through missions.restore.
      mission.checkpoint = row.value("checkpoint", Json());
      if (!mission.checkpoint.is_null())
        validate_progress(mission.checkpoint, "active");
      result.push_back(std::move(mission));
    }
    Require(state.contains("pending") && state.at("pending").is_array() &&
                state.at("pending").size() <= 16,
            "Saved mission changes must fit a bounded array");
    std::set<std::string> pending;
    for (const auto& event : state.at("pending")) {
      Fields(event, {"id", "change"});
      const auto id = event.at("id").get<std::string>();
      const auto reason = event.at("change").get<std::string>();
      Require(ids.contains(id) && pending.insert(id).second, "Invalid saved mission change owner");
      Require(reason == "start" || reason == "restart" || reason == "advance" ||
                  reason == "checkpoint" || reason == "fail",
              "Unknown saved mission change");
    }
    return result;
  } catch (const std::exception& error) {
    throw Failure("save_incompatible", "Saved mission state: " + std::string(error.what()));
  }
}
void Missions::ValidateSaved(const Json& state) {
  ReadSaved(state);
}
void Missions::Restore(std::string_view owner, const Json& state) {
  auto saved = ReadSaved(state);
  // Resolve every contract before replacing any state. Titles and display
  // order may change; goals, prerequisite graphs and revisions need migration.
  for (auto& mission : saved) {
    const Key key{std::string(owner), mission.definition.at("id").get<std::string>()};
    const auto current = missions_.find(key);
    if (current == missions_.end() ||
        Contract(current->second.definition) != Contract(mission.definition))
      throw Failure("save_incompatible", "Mission definition changed: " + key.second +
                                             "; migrate it in state.loaded or restart the project");
    mission.definition = current->second.definition;
  }
  for (auto& [key, mission] : missions_)
    if (key.first == owner)
      mission = Initial(mission.definition);
  for (auto& mission : saved) {
    const Key key{std::string(owner), mission.definition.at("id").get<std::string>()};
    missions_.at(key) = std::move(mission);
  }
  std::erase_if(changes_, [&](const auto& entry) { return entry.first.first == owner; });
  for (const auto& event : state.at("pending"))
    Changed({std::string(owner), event.at("id").get<std::string>()},
            event.at("change").get<std::string>());
}
void Missions::Release(std::string_view owner) {
  std::erase_if(missions_, [&](const auto& entry) { return entry.first.first == owner; });
  std::erase_if(changes_, [&](const auto& entry) { return entry.first.first == owner; });
}
void Missions::Changed(const Key& key, std::string reason) {
  changes_[key] = {++revision_, std::move(reason)};
}
std::vector<Missions::Pending> Missions::Events() const {
  std::vector<Pending> result;
  for (const auto& [key, change] : changes_)
    result.emplace_back(key, change.revision);
  return result;
}
std::optional<Json> Missions::TakeEvent(const Pending& pending) {
  const auto found = changes_.find(pending.first);
  if (found == changes_.end() || found->second.revision != pending.second)
    return std::nullopt;
  Json result = {{"mission", View(missions_.at(pending.first))},
                 {"change", found->second.reason},
                 {"revision", found->second.revision}};
  changes_.erase(found);
  return result;
}

Json Missions::Call(std::string_view owner, std::string_view method, const Json& args) {
  if (method == "missions.list") {
    Json result = Json::array();
    for (const auto& [key, mission] : missions_)
      if (key.first == owner)
        result.push_back(View(mission));
    return result;
  }
  if (method == "missions.register") {
    auto definition = Definition(args.at("definition"));
    const Key key{std::string(owner), definition.at("id").get<std::string>()};
    const auto found = missions_.find(key);
    if (found == missions_.end() &&
        (missions_.size() >= 128 || Call(owner, "missions.list", {}).size() >= 16))
      throw Failure("resource_limit", "Missions are limited to 16 per mod and 128 overall");
    auto next = Initial(std::move(definition));
    if (found != missions_.end() && found->second.status != "idle") {
      if (Contract(found->second.definition) != Contract(next.definition))
        throw Failure("mission_in_use",
                      "A running mission's goals cannot be replaced; restart the project or reload "
                      "with a state migration");
      next.progress = found->second.progress;
      next.checkpoint = found->second.checkpoint;
      next.status = found->second.status;
      next.reason = found->second.reason;
    }
    auto prospective = Save(owner);
    std::erase_if(prospective["missions"].get_ref<Json::array_t&>(),
                  [&](const auto& row) { return row.at("definition").at("id") == key.second; });
    prospective["missions"].push_back(Stored(next));
    // Reserve the largest future numeric progress, checkpoint maps, escaped
    // failure reasons and pending events before accepting a definition.
    // A mission that was accepted must not later outgrow its save quota.
    auto maximum = prospective;
    maximum["pending"] = Json::array();
    for (auto& row : maximum["missions"]) {
      for (auto& progress : row["progress"].items())
        progress.value() = std::string(32, '0');
      row["checkpoint"] = row["progress"];
      row["reason"] = std::string(512, '\1');
      row["status"] = "completed";
      maximum["pending"].push_back(
          {{"id", row.at("definition").at("id")}, {"change", "checkpoint"}});
    }
    if (maximum.dump().size() > kOwnerBytes)
      throw Failure("resource_limit",
                    "A mod's definitions and maximum saved mission state must fit 256 KiB");
    missions_[key] = std::move(next);
    return View(missions_.at(key));
  }
  const Key key{std::string(owner), args.at("id").get<std::string>()};
  if (method == "missions.remove") {
    changes_.erase(key);
    return missions_.erase(key) != 0;
  }
  const auto found = missions_.find(key);
  if (found == missions_.end())
    throw Failure("unknown_mission", "This mod has no mission named " + key.second);
  auto& mission = found->second;
  if (method == "missions.get")
    return View(mission);
  if (method == "missions.advance_batch") {
    if (mission.status != "active")
      throw Failure("mission_inactive", "Start or retry this mission before advancing objectives");
    auto updates = args.at("updates");
    if (updates.is_object() && updates.empty()) updates = Json::array();
    Require(updates.is_array() && updates.size() <= 32, "Advance at most 32 distinct objectives at once");
    auto next = mission;
    std::set<std::string> seen;
    bool changed = false;
    for (const auto& update : updates) {
      Fields(update, {"objective", "amount"});
      Require(update.contains("objective") && update.contains("amount") && Id(update.at("objective")),
              "An update needs its objective id and amount");
      const auto objective_id = update.at("objective").get<std::string>();
      Require(seen.insert(objective_id).second, "An objective cannot appear twice in one batch");
      const auto& definitions = mission.definition.at("objectives");
      const auto objective = std::find_if(definitions.begin(), definitions.end(),
          [&](const auto& value) { return value.at("id") == objective_id; });
      if (objective == definitions.end()) throw Failure("unknown_objective", "Unknown mission objective: " + objective_id);
      if (!Ready(mission, *objective))
        throw Failure("objective_locked", "Batch objectives must be unlocked before the update");
      const auto& amount = update.at("amount");
      if (!Number(amount, 0, 1e9))
        throw Failure("invalid_argument", "Objective progress must be finite and nonnegative");
      const auto before = mission.progress.at(objective_id).get<double>();
      const auto after = std::min(objective->at("goal").get<double>(), before + amount.get<double>());
      next.progress[objective_id] = after;
      changed |= before != after;
    }
    if (!changed) return View(mission);
    if (Complete(next)) next.status = "completed";
    mission = std::move(next);
    Changed(key, "advance");
    return View(mission);
  }
  if (method == "missions.start" || method == "missions.restart") {
    if (method == "missions.start" && mission.status != "idle")
      throw Failure("mission_started", "Use missions.restart to replay or retry a mission");
    const auto checkpoint = mission.checkpoint;
    auto next = Initial(mission.definition);
    if (method == "missions.restart" && args.value("from_checkpoint", true) &&
        !checkpoint.is_null()) {
      next.progress = checkpoint;
      next.checkpoint = checkpoint;
    }
    next.status = "active";
    mission = std::move(next);
  } else if (method == "missions.advance") {
    if (mission.status != "active")
      throw Failure("mission_inactive", "Start or retry this mission before advancing objectives");
    const auto id = args.at("objective").get<std::string>();
    const auto& objectives = mission.definition.at("objectives");
    const auto objective = std::find_if(objectives.begin(), objectives.end(),
                                        [&](const auto& o) { return o.at("id") == id; });
    if (objective == objectives.end())
      throw Failure("unknown_objective", "Unknown mission objective: " + id);
    if (!Ready(mission, *objective))
      throw Failure("objective_locked", "Complete this objective's prerequisites first");
    const auto amount = args.at("amount");
    if (!Number(amount, 0, 1e9))
      throw Failure("invalid_argument",
                    "Objective progress must be finite, nonnegative and at most one billion");
    const double progress = mission.progress.at(id).get<double>();
    const double next =
        std::min(objective->at("goal").get<double>(), progress + amount.get<double>());
    if (progress == next)
      return View(mission);
    mission.progress[id] = next;
    if (Complete(mission))
      mission.status = "completed";
  } else if (method == "missions.checkpoint") {
    if (mission.status != "active")
      throw Failure("mission_inactive", "Only active missions can record a retry point");
    mission.checkpoint = mission.progress;
  } else if (method == "missions.fail") {
    if (mission.status != "active")
      throw Failure("mission_inactive", "Only active missions can fail");
    mission.reason = Text(args, "reason", 512);
    mission.status = "failed";
  } else
    throw Failure("unsupported", "Unknown mission operation");
  Changed(key, std::string(method.substr(9)));
  return View(mission);
}
}  // namespace simpsons::modkit
