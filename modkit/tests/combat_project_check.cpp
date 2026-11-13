// Execute the public workshop source against deterministic host boundaries.
// This checks authoring logic, ownership and reload; original damage routines
// and actual NPC resources have separate native-oracle and live-game checks.
#include "entities.h"
#include "source.h"

#include <iostream>
#include <fstream>

using namespace simpsons::modkit;
namespace {
int checks = 0;
void Require(bool value, const char* message) {
  ++checks;
  if (!value) throw std::runtime_error(message);
}
Json Health(float value) {
  return {{"current", value}, {"maximum", 10}, {"dead", value <= 0}, {"enabled", true}};
}
}  // namespace

int main(int argc, char** argv) {
  const auto root = fs::temp_directory_path() / ("simpsons-combat-" +
      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  try {
    Require(argc == 2 || argc == 3, "Pass the public combat workshop project and optional report");
    const auto source = fs::path(argv[1]);
    fs::create_directories(root / "workshop");
    auto manifest = Json::parse(ReadText(source / "mod.json")); manifest["id"] = "workshop";
    WriteText(root / "workshop/mod.json", manifest.dump());
    WriteText(root / "workshop/main.lua", ReadText(source / "main.lua"));
    OwnedEntities entities;
    std::map<uint64_t, float> health;
    uint64_t next = 0; size_t spawns = 0;
    bool loaded = true, health_supported = true;
    bool include_first = true, renamed = false;
    Json last_position;
    std::string last_template;
    HostServices host;
    host.call = [&](const HostOwner& owner, std::string_view method, const Json& args) -> Json {
      if (method == "world.status") return {{"ready", true}, {"epoch", 2}};
      if (method == "world.catalog") {
        Json result = Json::array();
        if (!loaded) return result;
        for (int i : {1, 2}) {
          if (i == 1 && !include_first) continue;
          result.push_back({{"id", std::string(renamed ? "template:fresh" : "template:") + std::to_string(i)},
              {"category", "Enemies"}, {"class", "NPCMelee"},
              {"model_resource", std::string(31, '0') + std::to_string(i)},
              {"name", renamed ? "Resolved model " + std::to_string(i) : "Brawler"}});
        }
        if (renamed) std::reverse(result.begin(), result.end());
        return result;
      }
      if (method == "players.controlled") return {{"id", "player:1"},
          {"position", {{"x", args.at("player") == 1 ? 100 : 0}, {"y", 2}, {"z", 20}}}};
      if (method == "players.health") return Health(10);
      if (method == "world.spawn") {
        ++spawns; ++next; last_position = args.at("position");
        last_template = args.at("template");
        entities.Add(owner, next, {{"guid", {0x534D4F44, 2, 3, next}},
            {"vtable", 0x82100000}, {"class", "NPCMelee"}});
        health[next] = 10; return "entity:" + std::to_string(next);
      }
      if (method == "world.owned") {
        Json result = Json::array();
        for (const auto& item : entities.List(owner))
          result.push_back({{"id", "entity:" + std::to_string(item.lifetime)}, {"class", "NPCMelee"}});
        return result;
      }
      const auto id = std::stoull(args.at("entity").get<std::string>().substr(7));
      if (method == "world.destroy") return entities.Destroy(owner, id);
      entities.Get(owner, id);
      if (method == "entities.set_visible") {
        entities.SetRenderHidden(owner, id, !args.at("visible").get<bool>());
        return {{"hidden_by_mod", !args.at("visible").get<bool>()},
                {"hide_count", args.at("visible") == true ? 0 : 1},
                {"has_model", true},
                {"scene_attached", args.at("visible")}};
      }
      if (method == "entities.health") return health_supported ? Health(health.at(id)) : Json(nullptr);
      if (method == "entities.damage" || method == "entities.heal") {
        const auto before = Health(health.at(id));
        if (method == "entities.damage") {
          health[id] = std::max(0.0f, health[id] - args.at("amount").get<float>());
          if (!health[id]) entities.Died(id);  // Queue, never reenter Lua during this callback.
        } else health[id] = 10;
        return {{"before", before}, {"after", Health(health[id])}, {"removed", false}};
      }
      throw std::runtime_error("Unexpected workshop host operation");
    };
    host.save = [&](const HostOwner& owner) { return Json{{"entities", entities.Save(owner)}}; };
    host.release = [&](const HostOwner& owner) { entities.Release(owner); };
    host.restore = [&](const HostOwner& owner, const Json& value, bool checkpoint) {
      entities.Restore(owner, value.at("entities"), checkpoint,
          [](const Json& object) { return object.at("guid")[3].get<uint64_t>(); });
    };
    auto runtime = std::make_unique<Runtime>([](auto, const auto&) { return Json(); },
        [](auto) {}, "en", host);
    runtime->EnableInspection(true);
    runtime->Load(root, root / "storage", {"workshop"}); runtime->Tick(true, .01);
    auto mission = [&] { return runtime->Inspect({{"action", "call"}, {"owner", "workshop"},
        {"method", "missions.get"}, {"arguments", {{"id", "brawler-workshop"}}}}); };
    auto drain = [&] {
      for (const auto& item : entities.TakeDeaths())
        runtime->EntityDied(item.owner, {{"entity", "entity:" + std::to_string(item.lifetime)}});
      for (const auto& item : entities.TakePending()) {
        entities.Forget(item.lifetime); health.erase(item.lifetime);
        runtime->EntityRemoved(item.owner, {{"entity", "entity:" + std::to_string(item.lifetime)},
            {"reason", "removed"}});
      }
      Require(!runtime->Status()[0]["failed"].get<bool>(), "The public workshop callback remains healthy");
    };
    auto action = [&](const char* row, uint32_t player = 0) {
      runtime->Activate("workshop:combat", row, nullptr, player); drain();
    };
    action("next"); action("start", 1);
    Require(entities.All().size() == 2 && mission()["status"] == "active" && last_position["x"] == 105,
        "P2 starts the real project beside P2's actor, with two independently owned targets");
    Require(last_template == "template:2",
        "The native menu picker distinguishes models that share a display label");
    action("hide");
    action("hide");
    Require(entities.All()[0].render_hidden && entities.All()[1].render_hidden,
            "The public workshop can hide its targets repeatedly without replacing them");
    action("reveal");
    action("reveal");
    Require(!entities.All()[0].render_hidden && !entities.All()[1].render_hidden,
            "The public workshop reveals the same owned targets without changing its objective");
    action("hit");
    Require(mission()["objectives"][0]["progress"] == 1 && entities.All().size() == 1 &&
        mission()["has_checkpoint"] == true, "A queued native death advances once, checkpoints and removes its body");
    auto item = entities.All()[0]; entities.Destroy(item.owner, item.lifetime); drain(); drain();
    Require(mission()["status"] == "failed" && mission()["objectives"][0]["progress"] == 1 &&
        entities.All().empty(), "Removal without death fails the encounter without awarding a false kill");
    action("previous"); action("retry", 1);
    Require(entities.All().size() == 1 && mission()["status"] == "active" &&
        mission()["objectives"][0]["progress"] == 1,
        "Checkpoint retry recreates only the remaining target and preserves valid progress");
    Require(last_template == "template:2",
        "Browsing another model does not replace this encounter's checkpoint template");
    action("hide");
    const auto before_reload = spawns;
    renamed = true;
    auto prepared = runtime->PrepareReload(PrepareSources(runtime->Sources(), root / "images", true));
    runtime.swap(prepared); prepared.reset(); runtime->NotifyReloaded();
    Require(spawns == before_reload && entities.All().size() == 1 &&
        mission()["objectives"][0]["progress"] == 1,
        "Code reload rebinds this source project's targets without spawning doubles or resetting its checkpoint");
    Require(entities.All()[0].render_hidden,
        "The actual public project and runtime transfer a hidden target through code reload");
    action("reveal");
    Require(!entities.All()[0].render_hidden,
        "The reloaded project can release its transferred visibility request");
    action("hit");
    Require(mission()["status"] == "completed" && mission()["objectives"][0]["progress"] == 2 &&
        entities.All().empty(), "The encounter completes through native deaths and balances owned cleanup");
    action("start"); action("heal");
    Require(last_template == "template:fresh1",
        "Reloaded selection follows resource identity across renamed labels, reordered rows and fresh handles");
    runtime->Tick(true, 90); drain(); drain();
    Require(mission()["status"] == "failed" && entities.All().empty(),
        "The simulation deadline fails and removes the workshop's remaining targets");
    include_first = false; const auto before_missing = spawns; action("retry");
    Require(mission()["status"] == "failed" && entities.All().empty() && spawns == before_missing,
        "A missing checkpoint model fails without silently substituting a different NPC");
    include_first = true;
    loaded = false; action("start");
    Require(mission()["status"] == "failed" && entities.All().empty(),
        "An unavailable loaded template leaves a retryable failure without orphaned objects");
    loaded = true; health_supported = false; action("start");
    Require(mission()["status"] == "failed" && entities.All().empty(),
        "A template without supported health rolls back its partial encounter");
    runtime->Stop(); fs::remove_all(root);
    if (argc == 3) {
      std::ofstream output(argv[2]); output << Json{{"ok", true}, {"checks", checks}}.dump() << '\n';
      if (!output) throw std::runtime_error("Cannot write combat qualification report");
    }
    std::cout << checks << " public combat project checks passed\n";
    return 0;
  } catch (const std::exception& error) {
    fs::remove_all(root); std::cerr << error.what() << '\n'; return 1;
  }
}
