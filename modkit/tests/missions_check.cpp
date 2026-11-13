#include "missions.h"
#include "source.h"

#include <iostream>
#include <limits>

using namespace simpsons::modkit;

namespace {
int checks = 0;
void Require(bool value, const char* message) {
  ++checks;
  if (!value)
    throw Failure("fixture", message);
}
template <typename Function>
void Reject(Function function, const char* message) {
  bool rejected = false;
  try {
    function();
  } catch (const std::exception&) {
    rejected = true;
  }
  Require(rejected, message);
}
Json Definition(double goal = 2) {
  return {{"id", "route"},
          {"title", "Route"},
          {"objectives",
           Json::array({{{"id", "out"}, {"title", "Go out"}, {"goal", goal}},
                        {{"id", "back"}, {"title", "Come back"}, {"requires", {"out"}}}})}};
}
Json Call(Runtime& runtime, std::string method, Json arguments = Json::object()) {
  return runtime.Inspect(
      {{"action", "call"}, {"owner", "mission"}, {"method", method}, {"arguments", arguments}});
}
std::string Script(int goal = 2, bool migration = false) {
  return "game.state.changes=0\n"
         "game.on('initialize',function() game.missions.register({id='route',title='Route',"
         "objectives={{id='out',title='Go out',goal=" +
         std::to_string(goal) +
         "},{id='back',title='Come back',requires={'out'}}}}) end)\n"
         "game.on('mission.changed',function() game.state.changes=game.state.changes+1 end)\n" +
         (migration ? "game.on('state.loaded',function(e) "
                      "e.missions.missions[1].definition.objectives[1].goal=" +
                          std::to_string(goal) + "; game.missions.restore(e.missions) end)\n"
                    : "");
}
void Project(const fs::path& root, std::string script) {
  WriteText(root / "mission/mod.json", Json{{"api", 1},
                                            {"id", "mission"},
                                            {"name", "Mission fixture"},
                                            {"version", "1.0.0"},
                                            {"kind", "lua"},
                                            {"entry", "main.lua"},
                                            {"save_mode", "state"}}
                                           .dump());
  WriteText(root / "mission/main.lua", script);
}
PreparedSources Sources(Runtime& runtime, const fs::path& root) {
  SourceJob job(runtime.Sources(), root / "cache", true);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (!job.ready() && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  Require(job.ready(), "Mission reload preparation completes");
  return job.Take();
}
}  // namespace

int main(int argc, char** argv) {
  const auto root = fs::temp_directory_path() /
                    ("simpsons-mission-check-" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  try {
    Missions missions;
    const auto register_route = [&](std::string owner, Json definition) {
      return missions.Call(owner, "missions.register", {{"definition", definition}});
    };
    register_route("one", Definition());
    register_route("two", Definition());
    missions.Call("one", "missions.start", {{"id", "route"}});
    Reject(
        [&] {
          missions.Call("one", "missions.advance",
                        {{"id", "route"}, {"objective", "back"}, {"amount", 1}});
        },
        "Prerequisites keep later objectives locked");
    missions.Call("one", "missions.advance",
                  {{"id", "route"}, {"objective", "out"}, {"amount", 99}});
    auto state = missions.Call("one", "missions.get", {{"id", "route"}});
    Require(state["objectives"][0]["progress"] == 2 && state["objectives"][1]["state"] == "active",
            "Progress clamps and unlocks dependent objectives");
    Require(missions.Call("two", "missions.get", {{"id", "route"}})["status"] == "idle",
            "Mods may use the same local mission id without sharing progress");
    missions.Call("one", "missions.checkpoint", {{"id", "route"}});
    missions.Call("one", "missions.fail", {{"id", "route"}, {"reason", "Try again"}});
    state = missions.Call("one", "missions.restart", {{"id", "route"}});
    Require(state["status"] == "active" && state["objectives"][0]["progress"] == 2 &&
                state["has_checkpoint"] == true,
            "Retry preserves its objective checkpoint");
    state = missions.Call("one", "missions.advance",
                          {{"id", "route"}, {"objective", "back"}, {"amount", 1}});
    Require(state["status"] == "completed",
            "All required objectives complete the authored mission");
    Reject(
        [&] {
          missions.Call("one", "missions.advance",
                        {{"id", "route"}, {"objective", "out"}, {"amount", 1}});
        },
        "Completed missions cannot continue consuming gameplay events");
    const auto saved = missions.Save("one");
    Missions::ValidateSaved(saved);
    Require(saved["pending"].size() == 1,
            "Changes coalesce to one current notification per mission");
    Missions restored;
    auto renamed = Definition();
    renamed["title"] = "Translated route";
    std::reverse(renamed["objectives"].begin(), renamed["objectives"].end());
    restored.Call("one", "missions.register", {{"definition", renamed}});
    restored.Restore("one", saved);
    Require(restored.Call("one", "missions.get", {{"id", "route"}})["title"] == "Translated route",
            "Restores preserve current display text and allow reordered objectives");
    auto pending = restored.Events();
    Require(pending.size() == 1 &&
                restored.TakeEvent(pending[0])->at("mission").at("status") == "completed" &&
                restored.Events().empty(),
            "An undelivered completion survives restore and delivers once");
    auto bad = saved;
    bad["missions"][0]["progress"]["back"] = -1;
    const auto before = restored.Save("one");
    Reject([&] { restored.Restore("one", bad); },
           "Malformed progress is rejected before replacement");
    Require(restored.Save("one") == before, "Failed restores keep all current mission state");
    bad = Definition();
    bad["objectives"][0]["requires"] = {"back"};
    Reject([&] { register_route("two", bad); }, "Cyclic objectives cannot enter a live definition");
    bad = Definition();
    bad["objectives"][1]["requires"] = {"missing"};
    Reject([&] { register_route("two", bad); }, "Unresolved prerequisites fail with a named error");
    bad = Definition();
    bad["objectives"][0]["goal"] = std::numeric_limits<double>::infinity();
    Reject([&] { register_route("two", bad); }, "Non-finite goals are rejected");
    state = missions.Call("one", "missions.restart", {{"id", "route"}, {"from_checkpoint", false}});
    Require(state["objectives"][0]["progress"] == 0 && state["has_checkpoint"] == false,
            "A fresh replay clears progress and the previous retry point");
    const auto revision = missions.Events()[0];
    missions.Call("one", "missions.advance",
                  {{"id", "route"}, {"objective", "out"}, {"amount", 1}});
    Require(!missions.TakeEvent(revision),
            "A newer change supersedes a captured notification without recursive dispatch");
    missions.Release("one");
    Require(missions.Call("one", "missions.list", {}).empty() &&
                missions.Call("two", "missions.list", {}).size() == 1,
            "Retirement releases only its owner's services");
    Missions bounded;
    std::vector<std::string> accepted;
    bool limited = false;
    for (int index = 0; index < 16; ++index) {
      Json wide = {{"id", "large_" + std::to_string(index)},
                   {"title", "Large project"},
                   {"description", std::string(1024, 'd')},
                   {"objectives", Json::array()}};
      for (int objective = 0; objective < 32; ++objective)
        wide["objectives"].push_back(
            {{"id", "objective_" + std::to_string(objective) + std::string(60, 'x')},
             {"title", std::string(240, 't')},
             {"goal", 2}});
      try {
        bounded.Call("budget", "missions.register", {{"definition", wide}});
        accepted.push_back(wide.at("id"));
      } catch (const Failure& error) {
        Require(error.code == "resource_limit", "Oversized projects report a service quota");
        limited = true;
        break;
      }
    }
    Require(limited && !accepted.empty(),
            "Save headroom is reserved before a large project is accepted");
    for (const auto& id : accepted) {
      bounded.Call("budget", "missions.start", {{"id", id}});
      const auto mission = bounded.Call("budget", "missions.get", {{"id", id}});
      for (const auto& objective : mission.at("objectives"))
        bounded.Call(
            "budget", "missions.advance",
            {{"id", id}, {"objective", objective.at("id")}, {"amount", .12345678901234567}});
      bounded.Call("budget", "missions.checkpoint", {{"id", id}});
      bounded.Call("budget", "missions.fail", {{"id", id}, {"reason", std::string(512, '\1')}});
    }
    const auto largest = bounded.Save("budget");
    Missions::ValidateSaved(largest);
    Require(largest.dump().size() <= 256 * 1024,
            "Accepted definitions still fit saves with fractional progress, checkpoints and fully "
            "escaped failure text");

    Project(root, Script());
    auto required = Json::parse(ReadText(root / "mission/mod.json"));
    required["requires_operations"] = {"missions.register", "missions.advance"};
    required["requires_events"] = {"mission.changed", "state.loaded"};
    WriteText(root / "mission/mod.json", required.dump());
    Require(Manifest::Read(root / "mission").id == "mission",
            "A project can declare the public operations and events it needs");
    required["requires_operations"] = {"missions.not_implemented"};
    WriteText(root / "mission/mod.json", required.dump());
    Reject([&] { Manifest::Read(root / "mission"); },
           "Unsupported operations reject the project before callbacks run");
    required["requires_operations"] = {"missions.register"};
    required["requires_events"] = {"not.an.event"};
    WriteText(root / "mission/mod.json", required.dump());
    Reject([&] { Manifest::Read(root / "mission"); },
           "Unsupported event requirements cannot silently leave a mission inactive");
    // The reload checks below rewrite this fixture's script with Project().
    // Keep their starting metadata identical; metadata changes require restart.
    Project(root, Script());
    const auto logger = [](std::string_view message) { std::cerr << message << "\n"; };
    Runtime runtime([](std::string_view, const Json&) -> Json { return nullptr; }, logger);
    runtime.Load(root, root / "storage", {"mission"});
    runtime.EnableInspection(true);
    runtime.Tick(false, 0);
    Reject([&] { Call(runtime, "missions.start", {{"id", "route"}}); },
           "Starting a playable mission needs a loaded level");
    runtime.Tick(true, 0.01);
    Call(runtime, "missions.start", {{"id", "route"}});
    const auto pending_save = runtime.Save();
    auto replacement = runtime.PrepareRestore(pending_save);
    runtime.Stop();
    replacement->NotifyRestored();
    replacement->Tick(true, 0.01);
    replacement->Tick(true, 0.01);
    Require(replacement->Save()["mods"][0]["state"]["changes"] == 1,
            "Runtime restore delivers a pending mission change once");
    Reject([&] { Call(*replacement, "missions.restore", {{"state", Missions::Empty()}}); },
           "Mission migration cannot run outside restore preflight");
    Call(*replacement, "missions.advance", {{"id", "route"}, {"objective", "out"}, {"amount", 1}});
    replacement->Tick(true, 0.01);
    const auto old = replacement->Save();
    Project(root, Script(4));
    Reject([&] { replacement->PrepareReload(Sources(*replacement, root)); },
           "Changed goals cannot silently reuse an old mission contract");
    Require(replacement->Save() == old, "Rejected mission reload leaves the running mod unchanged");
    Project(root, Script(4, true));
    auto migrated = replacement->PrepareReload(Sources(*replacement, root));
    replacement->Stop();
    migrated->NotifyReloaded();
    state = Call(*migrated, "missions.get", {{"id", "route"}});
    Require(state["objectives"][0]["goal"] == 4 && state["objectives"][0]["progress"] == 1,
            "A validated state.loaded migration preserves progress across code reload");
    const auto checkpoint = migrated->Save(false);
    auto checkpoint_runtime = migrated->PrepareRestore(checkpoint, true);
    migrated->Stop();
    checkpoint_runtime->NotifyRestored(true);
    checkpoint_runtime->Tick(true, 0.01);
    Require(Call(*checkpoint_runtime, "missions.get",
                 {{"id", "route"}})["objectives"][0]["progress"] == 1,
            "Checkpoint sidecars preserve authored mission progress");
    checkpoint_runtime->Stop();

    Project(root, Script() +
                      "game.on('mission.changed',function(e) if e.change=='start' then "
                      "game.missions.advance('route','out',1) end end)\n");
    Runtime deferred([](std::string_view, const Json&) -> Json { return nullptr; }, {});
    deferred.Load(root, root / "deferred-storage", {"mission"});
    deferred.EnableInspection(true);
    deferred.Tick(true, .01);
    Call(deferred, "missions.start", {{"id", "route"}});
    deferred.Tick(true, .01);
    Require(deferred.Save()["mods"][0]["state"]["changes"] == 1,
            "A mission callback cannot recursively deliver its new change in the same tick");
    deferred.Tick(true, .01);
    Require(deferred.Save()["mods"][0]["state"]["changes"] == 2,
            "The deferred mission change arrives on the next tick");
    deferred.Stop();

    if (argc > 1) {
      fs::copy(Utf8Path(argv[1]), root / "walk", fs::copy_options::recursive);
      auto manifest = Json::parse(ReadText(root / "walk/mod.json"));
      manifest["id"] = "mission";
      WriteText(root / "walk/mod.json", manifest.dump());
      // Keep only the actual template enabled, with two independently moving
      // family members supplied by the same named API used by the game.
      fs::remove_all(root / "mission");
      double first = 0, second = 1000;
      Runtime walk(
          [&](std::string_view name, const Json&) -> Json {
            if (name == "players.list")
              return Json::array({{{"id", "player:1"},
                                   {"character", "Homer"},
                                   {"health", 100},
                                   {"max_health", 100},
                                   {"position", {{"x", first}, {"y", 0}, {"z", 0}}}},
                                  {{"id", "player:2"},
                                   {"character", "Bart"},
                                   {"health", 100},
                                   {"max_health", 100},
                                   {"position", {{"x", second}, {"y", 0}, {"z", 0}}}}});
            throw Failure("unsupported", "Unexpected template host call");
          },
          {});
      walk.Load(root, root / "walk-storage", {"mission"});
      walk.EnableInspection(true);
      walk.Tick(true, 0.01);
      walk.Activate("mission:route", "start", nullptr, 1);
      first = 15;
      walk.Tick(true, 0.1);
      state = Call(walk, "missions.get", {{"id", "there-and-back"}});
      Require(state["status"] == "active" && state["has_checkpoint"] == true,
              "The commented template advances through a real player-position query");
      walk.Tick(true, 0.1);
      Require(Call(walk, "missions.get", {{"id", "there-and-back"}})["status"] == "active",
              "An idle family member at home cannot complete another member's return");
      first = 0;
      walk.Tick(true, 0.1);
      Require(Call(walk, "missions.get", {{"id", "there-and-back"}})["status"] == "completed",
              "A family member who went out can complete the return objective");
      walk.Activate("mission:route", "start", nullptr, 1);
      first = 15;
      second = 1015;
      walk.Tick(true, .1);
      second = 1000;
      walk.Tick(true, .1);
      Require(Call(walk, "missions.get", {{"id", "there-and-back"}})["status"] == "completed",
              "A second independently moving family member can complete the shared route");
      walk.Activate("mission:route", "start", nullptr, 0);
      walk.Tick(true, 90);
      Require(Call(walk, "missions.get", {{"id", "there-and-back"}})["status"] == "failed",
              "The template fails by simulation time, independent of display frames");
      walk.Activate("mission:route", "start", nullptr, 0);
      walk.Tick(false, 0);
      Require(Call(walk, "missions.get", {{"id", "there-and-back"}})["status"] == "failed",
              "Leaving a level retires an active route through its native lifecycle event");
      walk.Stop();
    }
    fs::remove_all(root);
    std::cout << "{\"ok\":true,\"checks\":" << checks << "}\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << "\n";
    fs::remove_all(root);
    return 1;
  }
}
