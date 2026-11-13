#include "entities.h"
#include "source.h"

#include <iostream>
#include <thread>

using namespace simpsons::modkit;
namespace {
int checks = 0;
void Require(bool value, const char* message) {
  ++checks;
  if (!value) throw std::runtime_error(message);
}
template<class Function> void Reject(Function function, const char* message) {
  bool rejected = false;
  try { function(); } catch (const std::exception&) { rejected = true; }
  Require(rejected, message);
}
Json Object(uint32_t id) {
  return {{"guid", {0x534D4F44, 2, 3, id}}, {"vtable", 0x82001000}, {"class", "Prop"}};
}
void Project(const fs::path& root, std::string id, bool dependent = false) {
  Json manifest = {{"api", 1}, {"id", id}, {"name", id}, {"version", "1.0.0"},
      {"kind", "lua"}, {"entry", "main.lua"}, {"save_mode", "state"}};
  if (dependent) manifest["dependencies"] = {{"first", "*"}};
  WriteText(root / id / "mod.json", manifest.dump());
  WriteText(root / id / "main.lua", R"(
game.on('initialize', function()
  game.state.count = 0
  game.ui.register_page({id='objects',title='Objects',rows={
    {id='spawn',label='Spawn'}, {id='fail',label='Fail'}}})
end)
game.on('ui.action', function(e)
  if e.row == 'fail' then error('intentional lifecycle failure') end
  game.world.spawn('template:1', {x=0,y=0,z=0})
  game.state.count = game.state.count + 1
end)
game.on('entity.removed', function(e) game.state.reason = e.reason end)
game.on('entity.died', function(e) game.state.deaths = (game.state.deaths or 0) + 1 end)
)");
}
}  // namespace

int main() {
  const auto root = fs::temp_directory_path() / ("simpsons-entities-" +
      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  try {
    OwnedEntities entities;
    const HostOwner a{1, "first"}, b{1, "second"}, replacement{2, "first"};
    entities.Add(a, 10, Object(1));
    entities.Add(b, 20, Object(2));
    Require(entities.Lifetimes() == std::vector<uint64_t>{10, 20},
        "Frame liveness tokens are sorted and include every owner without descriptor copies");
    Require(entities.Get(a, 10).description == Object(1), "The active owner can access its object");
    entities.SetRenderHidden(a, 10, true);
    entities.SetRenderHidden(a, 10, true);
    Require(entities.Get(a, 10).render_hidden && entities.Save(a)[0]["render_hidden"] == true &&
                !entities.Get(b, 20).render_hidden,
            "An idempotent visibility lease is scoped to its owning object");
    Reject([&] { entities.SetRenderHidden(b, 10, false); },
           "Another owner cannot reveal a hidden object");
    Reject([&] { entities.SetRenderHidden(replacement, 10, false); },
           "An old generation cannot change visibility");
    entities.ResetRenderHidden(999);
    entities.ResetRenderHidden(10);
    Require(!entities.Get(a, 10).render_hidden && !entities.Save(a)[0].contains("render_hidden"),
            "A native force-show retires the lease before a new authored hide request");
    Reject([&] { entities.Get(b, 10); }, "Foreign owners cannot read or move another mod's object");
    Reject([&] { entities.Get(replacement, 10); }, "A different runtime generation cannot move an old object");
    Reject([&] { entities.Destroy(b, 10); }, "Foreign owners cannot destroy an entity");
    Reject([&] { entities.Add(b, 10, Object(1)); }, "An entity cannot have two owners");
    Require(entities.Destroy(a, 10) && !entities.Destroy(a, 10),
        "Removal is deferred and idempotent before the drain");
    Reject([&] { entities.Get(a, 10); }, "Queued removal rejects access before the destructor drains");
    Reject([&] { entities.SetRenderHidden(a, 10, true); },
           "Queued removal rejects visibility changes");
    Require(entities.List(a).empty() && entities.Save(a).empty() && entities.List(b).size() == 1,
        "Pending removal disappears from live queries and saves without affecting another mod");
    auto pending = entities.TakePending(1);
    Require(pending.size() == 1 && pending[0].lifetime == 10 && entities.All().size() == 2 &&
        entities.TakePending().empty(),
        "The bounded game-thread drain takes only pending objects");
    Require(entities.Forget(10).has_value(), "An asynchronous destructor remains tracked until it completes");
    Require(!entities.Forget(10), "External destruction after draining is harmless");
    Reject([&] { entities.Get(a, 10); }, "Destroyed lifetimes reject access without a guest read");
    entities.Add(a, 11, Object(3));
    entities.SetRenderHidden(a, 11, true);
    const auto state = entities.Save(a);
    std::thread worker([&] { entities.Release(a); });
    worker.join();
    Require(entities.List(a).empty() && entities.All().size() == 2,
        "Worker retirement performs host bookkeeping without guest destruction");
    entities.Restore(replacement, state, false, [](const Json&) { return 11; });
    entities.Release(a);
    Require(entities.TakePending().empty() && entities.List(replacement).size() == 1,
        "Reload transfers an object and an old generation cannot retire its replacement");
    Require(entities.Get(replacement, 11).render_hidden,
            "Reload transfers the existing hide lease without applying another native hide");
    Reject([&] { entities.Restore(b, state, false, [](const Json&) { return 11; }); },
        "A restored active object cannot be stolen");
    entities.Release(replacement);
    Reject([&] { entities.Restore(b, state, false, [](const Json&) { return 11; }); },
        "A retiring object cannot be transferred to another mod");
    Require(entities.All().size() == 2 && entities.List(b).size() == 1,
        "A rejected restore leaves all owners unchanged");
    Reject([&] { OwnedEntities::Validate({state, state}); }, "Saved owners cannot duplicate a GUID");
    Reject([&] { OwnedEntities::Validate({Json::array({{{"guid", {1, 2, 3, 4}},
        {"vtable", 1}, {"class", "Prop"}}})}); }, "Stock GUIDs cannot be adopted from a save");
    auto broken = state;
    broken[0]["guid"][3] = -1;
    Reject([&] { OwnedEntities::Validate({broken}); }, "Negative GUID words reject before restore");
    broken = state; broken[0]["guid"][3] = uint64_t(UINT32_MAX) + 1;
    Reject([&] { OwnedEntities::Validate({broken}); }, "Oversized GUID words reject before restore");
    broken = state; broken[0]["address"] = 0x82000000;
    Reject([&] { OwnedEntities::Validate({broken}); }, "Saved descriptors accept no arbitrary addresses");
    entities.Clear();

    entities.Add(a, 21, Object(21)); entities.Add(b, 22, Object(22));
    entities.SetRenderHidden(a, 21, true);
    entities.Died(999); entities.Died(21); entities.Died(21); entities.Died(22);
    const auto queued_death = entities.Save(a);
    Require(entities.Get(a, 21).Save() == queued_death[0] &&
        queued_death[0]["health_death"] == 1 && queued_death[0]["render_hidden"] == true,
        "Filtered game snapshots use the same serializer for death and visibility metadata");
    Require(queued_death[0]["health_death"] == 1,
        "An undelivered native death is saved with its owned object");
    auto notices = entities.TakeDeaths(1);
    Require(notices.size() == 1 && notices[0].owner == a && notices[0].lifetime == 21 &&
        entities.Save(a)[0]["health_death"] == 2,
        "Native death delivery is bounded, deduplicated and scoped to the real owner");
    Require(entities.TakeDeaths(1).at(0).owner == b && entities.TakeDeaths().empty(),
        "Draining one owner preserves another owner's pending death");
    const auto delivered_death = entities.Save(b);
    entities.Release(a);
    entities.Restore(replacement, queued_death, false, [](const Json& value) {
      Require(!value.contains("health_death") && !value.contains("render_hidden"),
              "Guest resolution receives only the immutable object identity");
      return 21;
    });
    Require(entities.TakeDeaths().at(0).owner == replacement && entities.TakeDeaths().empty(),
        "Reload transfers a queued death to the replacement generation exactly once");
    entities.Clear();
    entities.Restore(replacement, delivered_death, false, [](const Json&) { return 23; });
    entities.Died(23);
    Require(entities.TakeDeaths().empty(), "Restoring an already delivered native death cannot count the kill twice");
    for (auto invalid : {Json(0), Json(3), Json(-1), Json(true), Json(1.5)}) {
      auto malformed = queued_death; malformed[0]["health_death"] = invalid;
      Reject([&] { OwnedEntities::Validate({malformed}); }, "Invalid saved death state rejects before guest resolution");
    }
    for (auto invalid : {Json(false), Json(0), Json(1), Json("true"), Json(nullptr)}) {
      auto malformed = queued_death;
      malformed[0]["render_hidden"] = invalid;
      Reject([&] { OwnedEntities::Validate({malformed}); },
             "Invalid visibility leases reject before guest resolution");
    }
    entities.Clear();
    Require(entities.TakePending().empty(), "World rewind drops stale retirement without destroying new allocations");
    entities.Restore(replacement, state, true, [](const Json&) -> uint64_t {
      throw std::runtime_error("Checkpoint must not resolve old entities");
    });
    Require(entities.All().empty(), "A checkpoint never adopts an old-world object");
    entities.Restore(replacement, state, false, [](const Json&) { return 111; });
    Require(entities.List(replacement).at(0).lifetime == 111,
        "An exact snapshot rebinds saved GUIDs to new host lifetime identities");
    Require(entities.Get(replacement, 111).render_hidden,
            "Exact restore preserves the saved visibility lease on the newly resolved lifetime");
    entities.Forget(111);
    entities.Add(b, 112, Object(3));
    entities.Release(replacement);
    Require(entities.List(b).size() == 1 && entities.TakePending().empty(),
        "An allocation with a new lifetime survives an old owner's cleanup");
    entities.Clear();
    for (uint32_t i = 1; i <= 64; ++i) entities.Add(a, i, Object(i));
    Reject([&] { entities.CheckCapacity(a); }, "Capacity is checked before calling the guest factory");
    Reject([&] { entities.Add(a, 65, Object(65)); }, "An owner has a bounded number of entities");
    entities.Release(a);
    pending = entities.TakePending(7);
    for (const auto& entry : pending) entities.Forget(entry.lifetime);
    Require(pending.size() == 7 && entities.All().size() == 57,
        "A large retirement drains in bounded batches");
    entities.Clear();

    Project(root, "first"); Project(root, "second"); Project(root, "dependent", true);
    uint64_t next = 1000;
    HostServices host;
    host.call = [&](const HostOwner& owner, std::string_view method, const Json&) -> Json {
      if (method != "world.spawn") throw std::runtime_error("Unexpected game operation");
      ++next;
      entities.Add(owner, next, Object(uint32_t(next)));
      return "entity:" + std::to_string(next);
    };
    host.save = [&](const HostOwner& owner) { return Json{{"entities", entities.Save(owner)}}; };
    host.release = [&](const HostOwner& owner) { entities.Release(owner); };
    host.restore = [&](const HostOwner& owner, const Json& saved, bool checkpoint) {
      entities.Restore(owner, saved.at("entities"), checkpoint,
          [](const Json& description) { return description.at("guid").at(3).get<uint64_t>(); });
    };
    auto runtime = std::make_unique<Runtime>([](auto, const auto&) { return Json(); },
        [](auto) {}, "en", host);
    runtime->Load(root, root / "storage", {"first", "dependent", "second"});
    runtime->Tick(true, .016);
    runtime->Activate("first:objects", "spawn", nullptr, 0);
    runtime->Activate("second:objects", "spawn", nullptr, 1);
    runtime->Activate("dependent:objects", "spawn", nullptr, 0);
    auto sources = PrepareSources(runtime->Sources(), root / "images", true);
    auto prepared = runtime->PrepareReload(sources);
    Require(entities.All().size() == 3 && entities.TakePending().empty(),
        "Preparing a replacement does not acquire or destroy live objects");
    prepared.reset();
    Require(entities.TakePending().empty(), "Discarding prepared code leaves live owners intact");
    prepared = runtime->PrepareReload(sources);
    runtime.swap(prepared); prepared.reset();
    runtime->NotifyReloaded();
    Require(entities.All().size() == 3 && entities.TakePending().empty(),
        "Real runtime reload transfers all objects without a despawn/respawn cycle");
    HostOwner live_owner;
    for (const auto& entry : entities.All()) if (entry.owner.mod == "second") live_owner = entry.owner;
    runtime->EntityRemoved(a, {{"entity", "old"}, {"reason", "removed"}});
    runtime->EntityRemoved(live_owner, {{"entity", "current"}, {"reason", "game"}});
    runtime->EntityDied(a, {{"entity", "old"}});
    runtime->EntityDied(live_owner, {{"entity", "current"}});
    const auto saved = runtime->Save(false);
    for (const auto& mod : saved.at("mods"))
      if (mod.at("id") == "second") Require(mod.at("state").at("reason") == "game" &&
          mod.at("state").at("deaths") == 1,
          "Lifecycle notifications target the owning runtime generation");
    runtime->Activate("first:objects", "fail", nullptr, 0);
    runtime->Tick(true, 0);  // Dependency failures propagate on the next dispatch.
    pending = entities.TakePending();
    for (const auto& entry : pending) entities.Forget(entry.lifetime);
    Require(pending.size() == 2 && entities.All().size() == 1 &&
        entities.All().at(0).owner.mod == "second",
        "A callback fault retires its objects and dependent objects, preserving an unrelated mod");
    std::thread shutdown([&] { runtime.reset(); }); shutdown.join();
    Require(entities.TakePending().size() == 1,
        "Runtime shutdown on another thread queues remaining objects without guest calls");
    fs::remove_all(root);
    std::cout << checks << " entity lifecycle checks passed\n";
    return 0;
  } catch (const std::exception& error) {
    fs::remove_all(root);
    std::cerr << error.what() << '\n';
    return 1;
  }
}
