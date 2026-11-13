#include "runtime.h"
#include "storage.h"
#include "localization.h"
#include "text_table.h"

#include <cstdlib>
#include <iostream>
#include <thread>

using namespace simpsons::modkit;

namespace {
int checks = 0;
void Require(bool value, const char* message) {
  ++checks;
  if (!value) throw std::runtime_error(message);
}
template <typename Function>
void Reject(Function function, const char* message) {
  bool failed = false;
  try { function(); }
  catch (const std::exception&) { failed = true; }
  Require(failed, message);
}
void Project(const fs::path& root, std::string id, const std::string& code,
             Json extra = Json::object()) {
  Json manifest = {{"id", id}, {"name", id}, {"version", "1.0.0"}, {"api", 1},
                   {"kind", "lua"}, {"entry", "main.lua"}, {"save_mode", "state"}};
  manifest.update(extra);
  WriteText(root / id / "mod.json", manifest.dump());
  WriteText(root / id / "main.lua", code);
}
}

int main(int argc, char** argv) {
  if (argc == 3 && std::string_view(argv[1]) == "--validate-package") {
    try {
      std::cout << Manifest::Read(fs::path(argv[2])).fingerprint << '\n';
      return 0;
    } catch (const std::exception& error) {
      std::cerr << error.what() << '\n';
      return 1;
    }
  }
  if (argc == 3 && std::string_view(argv[1]) == "--fingerprint") {
    try { std::cout << Fingerprint(fs::path(argv[2])) << '\n'; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
  }
  const auto root = fs::temp_directory_path() / ("simpsons-modkit-check-" +
      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  const auto report = argc > 2 ? fs::path(argv[2]) : fs::path();
  try {
    fs::create_directories(root);
    Require(MenuText("Sant\xC3\xA9") == std::string("Sant\xE9"),
            "UTF-8 translated menu text uses the original font encoding");
    Reject([&] {MenuText("\xE6\x97\xA5");}, "Unsupported menu glyphs need a font import");
    Reject([&] {MenuText("\xC0\x80");}, "Malformed UTF-8 cannot enter a native menu");
    std::vector<uint8_t> table(48);
    const auto word = [&](size_t offset, uint32_t value) {
      for (int i = 3; i >= 0; --i) { table[offset + i] = uint8_t(value); value >>= 8; }
    };
    std::copy_n("2HCL", 4, table.begin());
    word(4, 48); word(8, 1); word(16, 1); word(20, 1); word(32, 1); word(36, 40);
    std::copy_n("Options", 8, table.begin() + 40);
    const auto table_digest = RelocatedTextTableHash(table, 0);
    word(24, 0xE5000020); word(28, 0xE5000024); word(36, 0xE5000028);
    Require(RelocatedTextTableHash(table, 0xE5000000) == table_digest,
            "A native text table retains its source identity after pointer relocation");
    word(36, 0xE5000001);
    Reject([&] {RelocatedTextTableHash(table, 0xE5000000);}, "Invalid native text pointers are rejected");
    Project(root, "game-language", "", {{"kind", "data"}, {"save_mode", "stateless"}, {"game_text", "locales/game.json"}});
    WriteText(root / "game-language/locales/game.json", Json{{"version", 1}, {"tables", {
        {table_digest, {{"00000001", {{"source", "Options"}, {"text", "Options FR"}}}}}}}}.dump());
    Runtime game_language({}, {});
    game_language.Load(root, root / "game-language-storage", {"game-language"});
    Require(game_language.GameTexts().at(table_digest).at(1).text == "Options FR",
            "Data-only language packs prepare native text overrides without archive replacements");
    game_language.Stop();
    {
      PersistentStorage storage(root / "persistent", {"first", "second"});
      for (int i = 0; i < 100; ++i) storage.Write("first", "settings.json", {{"volume", i}});
      Require(storage.Read("first", "settings.json")["volume"] == 99,
              "Queued writes must be immediately readable");
      Require(storage.Read("second", "settings.json").is_null(), "Mod storage must be isolated");
      Reject([&] {storage.Write("first", "../escape", 1);}, "Storage cannot escape a mod namespace");
      storage.Flush();
      Require(Json::parse(ReadText(root / "persistent/first/settings.json"))["volume"] == 99,
              "Draining coalesced writes must persist the newest value");
      Require(storage.Status("first")["pending"] == 0 && storage.Status("first")["errors"].empty(),
              "Storage reports completed writes");
    }
    {
      PersistentStorage storage(root / "persistent", {"first"});
      Require(storage.Read("first", "settings.json")["volume"] == 99, "Storage survives a restart");
    }
    {
      WriteText(root / "unwritable", "a file cannot be a storage directory");
      PersistentStorage storage(root / "unwritable", {"first"});
      storage.Write("first", "value.json", 1);
      storage.Flush();
      Require(!storage.Status("first")["errors"].empty(), "Background I/O errors must be visible");
    }
    int heals = 0;
    std::vector<std::string> log;
    auto call = [&](std::string_view name, const Json&) -> Json {
      if (name == "players.list") return Json::array({{{"id", "player:1"}, {"slot", 0}, {"character", "Homer"},
          {"position", {{"x", 1}, {"y", 2}, {"z", 3}}}, {"health", 1}, {"max_health", 10}}});
      if (name == "players.refill_health") { ++heals; return nullptr; }
      throw Failure("unsupported", std::string(name));
    };
    auto logger = [&](std::string_view text) { log.emplace_back(text); };
    Project(root, "health", R"(
      game.state.frames = 0
      game.on("level.ready", function()
        for _, player in ipairs(game.players.list()) do game.players.refill_health(player.id) end
      end)
      game.on("frame", function() game.state.frames = game.state.frames + 1 end)
      game.on("initialize", function()
        game.ui.register_page({id="tools",title="Tools",rows={
          {id="heal",label="Refill health"},
          {id="enabled",label="Enabled",type="toggle",value=true},
          {id="power",label="Power",type="slider",value=1,min=0,max=2,step=0.1},
          {id="choice",label="Choice",type="select",value="a",choices={{label="A",value="a"},{label="B",value="b"}}}
        }})
      end)
      game.on("ui.action", function(event)
        game.state.last_player = event.player
        game.state.last_value = event.value
        if event.row == "heal" then game.players.refill_health(game.players.list()[1].id) end
      end)
    )");
    Runtime runtime(call, logger);
    runtime.Load(root, root / "storage", {"health"});
    runtime.Tick(true, 1.0 / 30);
    Require(heals == 1, "Lua must call the real host bridge");
    Require(runtime.Pages().size() == 1, "Lua page registration");
    runtime.Activate("health:tools", "heal", nullptr, 1);
    Require(heals == 2, "Native menu action dispatch");
    runtime.Activate("health:tools", "enabled", false, 1);
    Require(runtime.Pages()[0].rows[1].value == false, "Toggle keeps its selected value");
    Reject([&] {runtime.Activate("health:tools", "power", 9, 0);}, "Invalid slider value must fail");
    Reject([&] {runtime.Activate("health:tools", "choice", "c", 0);}, "Invalid selector value must fail");
    auto saved = runtime.Save();
    Require(saved["mods"][0]["state"]["frames"] == 1, "Lua state is serialized");
    Require(saved["mods"][0]["state"]["last_player"] == 1, "Menu player ownership");
    runtime.Tick(true, 1.0 / 30);
    auto restored = runtime.PrepareRestore(saved);
    Require(restored->Save()["mods"][0]["state"]["frames"] == 1, "Restore returns to saved mod state");
    Require(runtime.Save()["mods"][0]["state"]["frames"] == 2, "Preflight must leave the running state alone");
    restored->NotifyRestored();
    restored->Stop();
    auto wrong = saved;
    wrong["mods"][0]["fingerprint"] = "changed";
    Reject([&] {runtime.ValidateSave(wrong);}, "A changed code profile cannot restore a snapshot");
    bool wrong_thread = false;
    std::thread other([&] { try { runtime.Activate("health:tools", "heal", nullptr, 0); }
                           catch (const Failure& e) { wrong_thread = e.code == "wrong_thread"; } });
    other.join();
    Require(wrong_thread, "Other threads cannot mutate the game");
    runtime.Stop();

    Project(root, "timed", R"(
      game.state.count=0
      game.on('initialize',function()
        game.timers.schedule('repeat', 1, 1, {name='Homer'})
      end)
      game.on('timer',function(event)
        game.state.count=game.state.count+1
        game.state.skipped=event.skipped
        game.state.name=event.data.name
      end)
    )");
    Runtime timed(call, logger);
    timed.EnableInspection(true);
    timed.Load(root, root / "timer-storage", {"timed"});
    timed.Tick(true, 0.5);
    timed.Tick(true, 0);
    timed.Tick(false, 100);
    Require(timed.Save()["mods"][0]["state"]["count"] == 0,
            "Pauses, menus and redraws must not advance simulation timers");
    const auto timer_save = timed.Save();
    timed.Tick(true, 0.5);
    Require(timed.Save()["mods"][0]["state"]["count"] == 1,
            "A timer fires at its remaining simulation deadline");
    auto timed_restore = timed.PrepareRestore(timer_save);
    timed_restore->Tick(true, 0.49);
    Require(timed_restore->Save()["mods"][0]["state"]["count"] == 0,
            "An exact restore preserves the unfinished timer delay");
    timed_restore->Tick(true, 0.02);
    Require(timed_restore->Save()["mods"][0]["state"]["count"] == 1,
            "A restored timer resumes without a duplicate event");
    timed.Tick(true, 10);
    const auto timer_state = timed.Save()["mods"][0]["state"];
    Require(timer_state["count"] == 2 && timer_state["skipped"] == 9 && timer_state["name"] == "Homer",
            "A slow frame coalesces repeat events and keeps their JSON payload");
    const auto inspection = timed.Inspect({{"action", "inspect"}});
    Require(inspection["mods"].size() == 1 && inspection["mods"][0]["timers"].size() == 1 &&
            !inspection["trace"].empty(), "Inspection exposes ownership, timers and recent lifecycle events");
    Reject([&] {timed.Inspect({{"action", "call"}, {"method", "players.refill_health"},
        {"arguments", {{"player", "player:1"}}}});}, "Creator mutations require a real mod owner");
    timed.Inspect({{"action", "call"}, {"method", "timers.cancel"}, {"owner", "timed"},
                   {"arguments", {{"id", "repeat"}}}});
    timed.Tick(true, 5);
    Require(timed.Save()["mods"][0]["state"]["count"] == 2,
            "Cancel removes a pending repeating timer");
    Reject([&] {timed.Inspect({{"action", "call"}, {"method", "timers.schedule"}, {"owner", "timed"},
        {"arguments", {{"id", "bad"}, {"seconds", 0}, {"interval", 0.0001}}}});},
           "Timer validation rejects unbounded repeat rates");
    timed_restore->Stop(); timed.Stop();

    Project(root, "replace-timers", R"(
      game.state.ids=''
      game.on('initialize',function()
        game.timers.schedule('a',0)
        game.timers.schedule('b',0)
      end)
      game.on('timer',function(event)
        game.state.ids=game.state.ids..event.id
        if event.id=='a' then game.timers.schedule('b',0) end
      end)
    )");
    Runtime replace_timers(call, logger);
    replace_timers.Load(root, root / "timer-storage", {"replace-timers"});
    replace_timers.Tick(true, 0.01);
    Require(replace_timers.Save()["mods"][0]["state"]["ids"] == "a",
            "A callback replacing another due timer cannot deliver its retired event");
    replace_timers.Tick(true, 0.01);
    Require(replace_timers.Save()["mods"][0]["state"]["ids"] == "ab",
            "A newly scheduled zero-delay timer waits until the next simulation step");
    replace_timers.Stop();
    Runtime no_mods(call, logger);
    no_mods.EnableInspection(true);
    no_mods.Load(root, root / "empty-storage", {});
    no_mods.Tick(false, 0);
    Require(no_mods.Inspect({{"action", "call"}, {"method", "system.describe"}}).contains("operations"),
            "Creator host inspection works before any mod is loaded");
    no_mods.Stop();

    Project(root, "early-mutation", "game.on('initialize',function() game.players.refill_health('player:1') end)");
    Runtime early_mutation(call, logger);
    early_mutation.Load(root, root / "storage", {"early-mutation"});
    const auto before_initialize = heals;
    early_mutation.Tick(false, 0);
    Require(early_mutation.Status()[0]["failed"] == true && heals == before_initialize,
            "Initial module registration must not mutate gameplay");
    Reject([&] {early_mutation.Inspect({{"action", "inspect"}});}, "Creator inspection is disabled by default");
    early_mutation.Stop();

    Project(root, "frozen-source", "local helper=require('helpers.value');game.state.value=helper.value");
    WriteText(root / "frozen-source/helpers/value.lua", "return {value=3}");
    Runtime frozen_source(call, logger);
    frozen_source.Load(root, root / "frozen-storage", {"frozen-source"});
    WriteText(root / "frozen-source/helpers/value.lua", "return {value=4}");
    frozen_source.Tick(false, 0);
    Require(frozen_source.Save()["mods"][0]["state"]["value"] == 3,
            "The first game-thread callback uses one immutable verified set of Lua modules");
    Reject([&] {frozen_source.PrepareRestore(frozen_source.Save());},
           "Keeping immutable scripts must not weaken changed-source restore checks");
    WriteText(root / "frozen-source/helpers/value.lua", "return {value=3}");
    auto frozen_restore = frozen_source.PrepareRestore(frozen_source.Save());
    Require(frozen_restore->Save()["mods"][0]["state"]["value"] == 3,
            "A verified replacement VM reuses the prepared script bytes");
    frozen_restore->Stop(); frozen_source.Stop();

    Project(root, "runaway", "game.on('frame',function() while true do end end)");
    Runtime runaway(call, logger);
    runaway.Load(root, root / "storage", {"runaway"});
    runaway.Tick(true, 0.03);
    Require(runaway.Status()[0]["failed"] == true, "A runaway callback must be interrupted");
    runaway.Stop();
    Project(root, "allocation", "game.on('frame',function() local a=string.rep('x',64*1024*1024) end)");
    Runtime allocation(call, logger);
    allocation.Load(root, root / "storage", {"allocation"});
    allocation.Tick(true, 0.03);
    Require(allocation.Status()[0]["failed"] == true, "Allocation limits must report an error, not terminate the host");
    allocation.Stop();
    Project(root, "sandbox", "game.on('initialize',function() assert(io==nil and os==nil and debug==nil and package==nil and load==nil and pcall==nil) end)");
    Runtime sandbox(call, logger);
    sandbox.Load(root, root / "storage", {"sandbox"});
    sandbox.Tick(false, 0);
    Require(sandbox.Status()[0]["failed"] == false, "Sandbox libraries");
    sandbox.Stop();
    Project(root, "host-errors", R"(
      game.on('initialize', function()
        local rejected = game.system.try_call('players.refill_health', {player='player:1'})
        assert(not rejected.ok and rejected.code == 'restore_preflight')
        local pose = {position={x=0,y=0,z=0},right={x=1,y=0,z=0},up={x=0,y=1,z=0},forward={x=0,y=0,z=1}}
        local placement = game.system.try_call('entities.set_transform', {entity='entity:1',transform=pose})
        assert(not placement.ok and placement.code == 'restore_preflight')
        local invalid = game.system.try_call('players.refill_health', {wrong='player:1'})
        assert(not invalid.ok and invalid.code == 'invalid_argument')
        local recursive = game.system.try_call('system.try_call', {method='system.describe',arguments={}})
        assert(not recursive.ok and recursive.code == 'invalid_argument')
      end)
      game.on('level.ready', function()
        local ok = game.system.try_call('players.list', {})
        assert(ok.ok and ok.value[1].id == 'player:1')
        local rejected = game.system.try_call('world.spawn', {template='template:1',position={x=1,y=2,z=3}})
        assert(not rejected.ok and rejected.code == 'unsupported')
        local unknown = game.system.try_call('missing.operation', {})
        assert(not unknown.ok and unknown.code == 'unsupported')
        local invalid = game.system.try_call('entities.move', {entity='entity:1',position={x=1,y='bad',z=3}})
        assert(not invalid.ok and invalid.code == 'invalid_argument')
        local invalid_pose = game.system.try_call('entities.set_transform', {entity='entity:1',transform={}})
        assert(not invalid_pose.ok and invalid_pose.code == 'invalid_argument')
        local oversized_pose = game.system.try_call('entities.set_transform', {entity='entity:1',transform={
          position={x=0,y=0,z=0},right={x=1e100,y=0,z=0},up={x=0,y=1,z=0},forward={x=0,y=0,z=1}}})
        assert(not oversized_pose.ok and oversized_pose.code == 'invalid_argument')
        local empty = game.system.try_call('log.info', {message='Recoverable host calls are active'})
        assert(empty.ok and empty.value == nil)
        game.state.alive = true
        -- Reading game.state during serialization can invoke a Lua metamethod.
        -- Even that path must retain the host's save-time mutation guard.
        local saved = game.state
        game.state = nil
        setmetatable(game, {__index=function(_, key)
          if key == 'state' then
            local rejected = game.system.try_call('players.refill_health', {player='player:1'})
            assert(not rejected.ok and rejected.code == 'read_only')
            local placement = game.system.try_call('entities.set_transform', {entity='entity:1',transform={
              position={x=0,y=0,z=0},right={x=1,y=0,z=0},up={x=0,y=1,z=0},forward={x=0,y=0,z=1}}})
            assert(not placement.ok and placement.code == 'read_only')
            return saved
          end
        end})
      end)
    )");
    const auto heals_before = heals;
    Runtime host_errors(call, logger);
    host_errors.Load(root, root / "storage", {"host-errors"});
    host_errors.Tick(true, 0.03);
    Require(host_errors.Status()[0]["failed"] == false && host_errors.Save()["mods"][0]["state"]["alive"] == true,
        "Expected host failures are recoverable without disabling Lua or losing its state");
    Require(heals == heals_before, "Recoverable calls cannot bypass preflight or serialization mutation checks");
    host_errors.Stop();
    Project(root, "host-errors-runaway", R"(
      game.on('frame', function() while true do game.system.try_call('players.list', {}) end end)
    )");
    Runtime host_runaway(call, logger);
    host_runaway.Load(root, root / "storage", {"host-errors-runaway"});
    host_runaway.Tick(true, 0.03);
    Require(host_runaway.Status()[0]["failed"] == true,
        "Recoverable host calls cannot catch or bypass the Lua callback budget");
    host_runaway.Stop();
    Project(root, "finalizer", "local guard=setmetatable({}, {__gc=function() while true do end end}); game.on('frame',function() local keep=guard end)");
    Runtime finalizer(call, logger);
    finalizer.Load(root, root / "storage", {"finalizer"});
    finalizer.Tick(false, 0);
    Require(finalizer.Status()[0]["failed"] == true, "Unbounded finalizer registration must fail");
    finalizer.Stop();
    Require(true, "Shutdown must interrupt a runaway Lua finalizer");
    Project(root, "checkpoint", "game.state.visits=5; game.on('level.ready',function() game.state.visits=game.state.visits+1 end)", {{"save_mode", "checkpoint"}});
    Runtime checkpoint(call, logger);
    checkpoint.Load(root, root / "storage", {"checkpoint"});
    checkpoint.Tick(true, 0.03);
    Require(!checkpoint.SupportsExact(), "Checkpoint-only mods must not advertise snapshot support");
    Reject([&] {checkpoint.Save();}, "Exact capture rejects checkpoint-only mods");
    const auto checkpoint_state = checkpoint.Save(false);
    auto checkpoint_next = checkpoint.PrepareRestore(checkpoint_state, true);
    Require(checkpoint_next->Save(false)["mods"][0]["state"]["visits"] == 6,
            "Checkpoint restores serialized mod progress");
    checkpoint_next->Tick(true, 0.03);
    Require(checkpoint_next->Save(false)["mods"][0]["state"]["visits"] == 7,
            "A checkpoint restore dispatches level.ready only when its new level begins");
    auto initial = checkpoint.PrepareInitialState();
    Require(initial->Save(false)["mods"][0]["state"]["visits"] == 5,
            "Adopting a vanilla checkpoint uses mod defaults, not an empty table");
    Reject([&] {checkpoint.PrepareRestore(checkpoint_state);}, "Checkpoint state cannot load as a snapshot");
    initial->Stop(); checkpoint_next->Stop(); checkpoint.Stop();
    Reject([&] {Inside(root, "../escape");}, "Reject parent traversal");
    Reject([&] {Inside(root, "/absolute");}, "Reject absolute paths");

    Project(root, "localized", R"(
      game.on('initialize', function()
        game.state.language=game.locale.current()
        game.state.title=game.locale.text('title')
        game.state.fallback=game.locale.text('fallback')
        game.state.greeting=game.locale.text('greeting',{name='Lisa'})
        game.state.unknown=game.locale.text('missing.key')
      end)
    )", {{"default_locale", "en"}, {"locales", {{"en", "locales/en.json"}, {"fr", "locales/fr.json"}}}});
    WriteText(root / "localized/locales/en.json", Json{{"title", "Tools"}, {"fallback", "Original"},
        {"greeting", "Hello {name} [RDDown] %02d %1$s %%"}}.dump());
    WriteText(root / "localized/locales/fr.json", Json{{"title", "Outils"}}.dump());
    Project(root, "language-pack", "", {{"kind", "data"}, {"save_mode", "stateless"},
        {"dependencies", {{"localized", "1.0.0"}}},
        {"translations", {{"localized", {{"fr", "locales/fr.json"}}}}}});
    WriteText(root / "language-pack/locales/fr.json", Json{{"greeting", "Bonjour {name} [RDDown] %02d %1$s %%"}}.dump());
    Runtime localized(call, logger, "fr-CA");
    localized.Load(root, root / "localized-storage", {"language-pack"});
    localized.Tick(false, 0);
    const auto words = localized.Save()["mods"][0]["state"];
    Require(words["language"] == "fr-ca", "Locale codes normalize consistently");
    Require(words["title"] == "Outils", "A regional locale falls back to its language");
    Require(words["fallback"] == "Original", "Missing translated keys use the source catalog");
    Require(words["greeting"] == "Bonjour Lisa [RDDown] %02d %1$s %%", "Language packs preserve dynamic button and format tags");
    Require(words["unknown"] == "missing.key", "An unknown mod text key is visible");
    auto localized_restore = localized.PrepareRestore(localized.Save());
    Require(localized_restore->Save()["mods"][0]["state"] == words, "Save preflight retains the loaded locale catalogs");
    localized_restore->Stop(); localized.Stop();
    WriteText(root / "language-pack/locales/fr.json", Json{{"greeting", "Bonjour {name}"}}.dump());
    Runtime broken_translation(call, logger, "fr");
    Reject([&] {broken_translation.Load(root, root / "localized-storage", {"language-pack"});},
           "A language pack cannot silently remove dynamic prompts or format values");
    Reject([&] {Inside(root, "C:\\file");}, "Reject Windows paths on every platform");
    Require(VersionMatches("1.2.0", "^1.0.0") && !VersionMatches("2.0.0", "^1.0.0"), "Dependency version ranges");
    auto a = Manifest::Read(root / "health"), b = Manifest::Read(root / "sandbox");
    a.dependencies["sandbox"] = "^1.0.0";
    auto order = Resolve({a, b}, {"health"});
    Require(order.size() == 2 && order[0].id == "sandbox", "Dependencies load before dependents");
    b.dependencies["health"] = "*";
    Reject([&] {Resolve({a, b}, {"health"});}, "Reject dependency cycles");
    b.dependencies.clear(); b.conflicts.push_back("health");
    Reject([&] {Resolve({a, b}, {"health"});}, "Reject declared conflicts");
    if (argc > 1) {
      const auto binary = fs::absolute(argv[1]);
      fs::create_directories(root / "native");
      fs::copy_file(binary, root / "native" / binary.filename());
      WriteText(root / "native/mod.json", Json{{"id", "native"}, {"name", "Native"},
          {"version", "1.0.0"}, {"kind", "native"}, {"save_mode", "state"},
          {"binaries", {{Platform(), binary.filename().string()}}}}.dump());
      Runtime native(call, logger);
      native.Load(root, root / "storage", {"native"});
      const int before = heals;
      native.Tick(true, 0.03);
      Require(heals == before + 1, "C++ SDK must call the host through the C ABI");
      auto state = native.Save();
      Require(state["mods"][0]["state"]["frames"] == 1, "Native plugin serialization");
      native.Tick(true, 0.03);
      auto restored_native = native.PrepareRestore(state);
      Require(restored_native->Save()["mods"][0]["state"]["frames"] == 1, "Native plugin state restoration");
      restored_native->NotifyRestored();
      restored_native->Stop();
      native.Stop();
    }
    fs::remove_all(root);
    if (!report.empty()) WriteText(report, Json{{"ok", true}, {"checks", checks}, {"platform", Platform()}}.dump());
    std::cout << checks << " modkit checks passed\n";
    return 0;
  } catch (const std::exception& error) {
    if (!report.empty()) WriteText(report, Json{{"ok", false}, {"error", error.what()}, {"checks", checks}}.dump());
    std::cerr << error.what() << "\nEvidence: " << root << '\n';
    return 1;
  }
}
