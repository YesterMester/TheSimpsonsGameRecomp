#include "runtime.h"
#include "source.h"
#include "project_documents.h"

#include <iostream>

using namespace simpsons::modkit;
namespace {
unsigned checks = 0;
void Require(bool value, const char* message) { ++checks; if (!value) throw Failure("fixture", message); }
PreparedSources Sources(Runtime& runtime, const fs::path& root) {
  SourceJob job(runtime.Sources(), root / "cache", true);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (!job.ready() && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  Require(job.ready(), "Frozen module preparation completes");
  return job.Take();
}
}
int main(int argc, char** argv) {
  const auto root = fs::temp_directory_path() / ("simpsons-route-editor-" +
      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  try {
    Require(argc == 2, "The shared launcher template bundle is required");
    const auto bundle = Json::parse(ReadText(Utf8Path(argv[1]), 4 * 1024 * 1024));
    const auto found = std::find_if(bundle.at("templates").begin(), bundle.at("templates").end(),
        [](const auto& value) { return value.at("id") == "mission-editor"; });
    Require(found != bundle.at("templates").end(), "Mission editor is a public creator project");
    auto manifest = found->at("manifest");
    const auto owner = manifest.at("id").get<std::string>();
    WriteText(root / "project/mod.json", manifest.dump());
    for (const auto& [path, content] : found->at("files").items())
      WriteText(Inside(root / "project", path), content.get<std::string>());
    double positions[2] = {0, 20};
    bool present[2] = {true, true};
    GameCall game = [&](std::string_view method, const Json& args) -> Json {
      if (method == "world.status") return {{"ready", true}, {"streaming", false}, {"level_root", "gamehub/gamehub.str"}};
      if (method == "world.catalog" || method == "world.owned") return Json::array();
      if (method == "players.controlled") {
        const auto player = args.at("player").get<unsigned>();
        Require(player < 2, "Only logical P1/P2 are evaluated");
        if (!present[player]) return nullptr;
        return {{"position", {{"x", positions[player]}, {"y", 2}, {"z", 3}}}, {"health", 100}, {"max_health", 100}};
      }
      throw Failure("fixture", "Unexpected game adapter: " + std::string(method));
    };
    auto runtime = std::make_unique<Runtime>(game, [](std::string_view text) { std::cerr << text << '\n'; });
    runtime->EnableInspection(true);
    runtime->Load(root, root / "storage", {owner});
    runtime->Tick(true, 0);
    auto call = [&](const char* method, Json args = Json::object()) {
      return runtime->Inspect({{"action", "call"}, {"owner", owner}, {"method", method}, {"arguments", args}});
    };
    auto state = [&] { return runtime->Save()["mods"][0]["state"]; };
    auto mission = [&] { return call("missions.get", {{"id", "authored-mission"}}); };
    auto action = [&](const char* row, Json value = nullptr, uint32_t player = 0) {
      runtime->Activate(owner + ":mission", row, value, player);
      Require(!runtime->Status()[0]["failed"].get<bool>(), "Public Lua editor remains active");
    };
    action("add", nullptr, 1);
    Require(state()["mission_document"]["objectives"][0]["position"]["x"] == 20,
            "The menu stamps the invoking logical player's point");
    action("retry-point", true);
    action("add"); action("position-x", 30); action("kind", "stay"); action("seconds", .4);
    action("add"); action("kind", "wait"); action("seconds", .3);
    Require(state()["mission_document"]["objectives"][2]["requires"] == Json::array({"point-2"}),
            "New objectives keep stable prerequisite ids");
    action("start");
    positions[1] = 50;
    runtime->Tick(true, .11);
    Require(mission()["objectives"][0]["progress"] == 0,
            "A departed author or inactive family member cannot complete the first point");
    positions[0] = 20; runtime->Tick(true, .11);
    Require(mission()["objectives"][0]["state"] == "completed" && mission()["has_checkpoint"] == true,
            "A controlled visit completes the point and records its retry state");
    positions[0] = 30; runtime->Tick(true, .11);
    Require(state()["holds"]["point-2"]["player"] == 0, "A stay objective remembers its logical player");
    positions[0] = 20; positions[1] = 30; runtime->Tick(true, .11);
    Require(state()["holds"]["point-2"]["player"] == 1 && state()["holds"]["point-2"]["seconds"] == .11,
            "Another player's handoff starts a fresh continuous stay");
    positions[1] = 40; runtime->Tick(true, .11);
    Require(!state()["holds"].contains("point-2"), "Leaving a point resets continuous stay progress");
    positions[0] = 30; runtime->Tick(true, .11);
    runtime->Tick(true, .04);
    const auto paused = state();
    for (unsigned i = 0; i < 35; ++i) runtime->Tick(true, 0);
    Require(state()["elapsed"] == paused["elapsed"] && state()["holds"] == paused["holds"] &&
                state()["sample_seconds"] == paused["sample_seconds"], "Pause does not consume simulation time or lose a partial sample");
    const auto saved = runtime->Save();
    auto restored = runtime->PrepareRestore(saved);
    runtime->Stop(); runtime = std::move(restored); runtime->NotifyRestored(); runtime->Tick(true, 0);
    Require(state()["holds"] == paused["holds"] && state()["sample_seconds"] == .04 &&
                mission()["has_checkpoint"] == true, "Restore preserves stay ownership, partial time and native retry state");
    auto reloaded = runtime->PrepareReload(Sources(*runtime, root));
    runtime->Stop(); runtime = std::move(reloaded); runtime->NotifyReloaded(); runtime->Tick(true, 0);
    Require(state()["holds"] == paused["holds"] && mission()["objectives"][0]["state"] == "completed",
            "Frozen scene-module reload preserves the authored mission contract and progress");
    for (unsigned i = 0; i < 10; ++i) runtime->Tick(true, .11);
    Require(mission()["status"] == "completed", "The continuous stay and dependent wait complete the route");
    action("retry");
    Require(mission()["objectives"][0]["state"] == "completed" && mission()["objectives"][1]["progress"] == 0 &&
                state()["holds"].empty(), "Retry restores native progress and the matching scripted retry state");
    action("stop"); action("objective", "point-1"); action("parent", "point-3");
    const auto before_cycle = state()["mission_document"];
    action("link");
    Require(state()["mission_document"] == before_cycle, "A cyclic menu edit preserves the previous valid route");
    action("time-limit", .2); action("start");
    for (unsigned i = 0; i < 3; ++i) runtime->Tick(true, .11);
    Require(mission()["status"] == "failed" && mission()["reason"] == "Time limit reached",
            "The authored simulation-time limit fails the preview");
    action("stop"); action("undo");
    Require(state()["mission_document"]["time_limit"] == 0, "Undo restores the previous authored rule");
    action("save");
    for (unsigned i = 0; i < 100; ++i) {
      runtime->Tick(true, 0);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const auto draft = Json::parse(ReadText(root / "storage" / owner / "project-drafts.json", 1024 * 1024));
    Require(draft.contains("fingerprint") && draft["documents"].contains("scene") && draft["documents"].contains("mission"),
            "Both saved documents share one atomic storage replacement and immutable source revision");
    Require(draft["documents"]["mission"] == ValidateProjectDocument("mission", state()["mission_document"]),
            "A saved draft matches the canonical Lua document, including empty prerequisite arrays");
    action("new");
    std::string previous;
    for (unsigned i = 0; i < 32; ++i) {
      action("add"); action("kind", "wait");
      if (!previous.empty()) { action("parent", previous); action("link"); }
      previous = state()["mission_document"]["objectives"].back()["id"].get<std::string>();
    }
    action("start"); runtime->Tick(true, .11);
    const auto wide = mission();
    Require(wide["objectives"].size() == 32 && std::all_of(wide["objectives"].begin(), wide["objectives"].end(),
                [](const auto& point) { return point.at("progress") == .11; }),
            "All 32 simultaneous timed goals update in one sample without exhausting the callback budget");
    Require(!runtime->Status()[0]["failed"].get<bool>(), "The complete editor fixture has no script fault");
    runtime->Stop(); fs::remove_all(root);
    std::cout << "{\"ok\":true,\"checks\":" << checks << "}\n";
  } catch (const std::exception& error) {
    fs::remove_all(root); std::cerr << error.what() << '\n'; return 1;
  }
}
