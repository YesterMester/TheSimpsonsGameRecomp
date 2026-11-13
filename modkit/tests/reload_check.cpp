#include "source.h"

#include <iostream>

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
PreparedSources Finish(SourceJob& job) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (!job.ready() && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  Require(job.ready(), "Source preparation completes within the fixture deadline");
  return job.Take();
}
std::string Script(int version, bool invalid_initialize = false) {
  return "game.state.frames=0\n"
         "game.on('initialize',function()\n"
         "game.ui.register_page({id='tools',title='Version " +
         std::to_string(version) +
         "',"
         "rows={{id='fail',label='Fail'}}})\n"
         "game.timers.schedule('long',600,0,{value=7})\n" +
         (invalid_initialize ? "game.players.god_mode(true)\n" : "") +
         "end)\n"
         "game.on('frame',function() game.state.frames=game.state.frames+" +
         std::to_string(version) +
         " end)\n"
         "game.on('ui.action',function() error('fixture callback failure') end)\n"
         "game.on('code.reloaded',function() game.state.reloaded=" +
         std::to_string(version) + " end)\n";
}
void Project(const fs::path& root, int version) {
  WriteText(root / "lua/mod.json", Json{{"api", 1},
                                        {"id", "lua"},
                                        {"name", "Lua reload"},
                                        {"version", "1.0.0"},
                                        {"kind", "lua"},
                                        {"entry", "main.lua"},
                                        {"save_mode", "state"}}
                                       .dump());
  WriteText(root / "lua/main.lua", Script(version));
}
}  // namespace

int main(int argc, char** argv) {
  fs::path report;
  if (argc > 2 && std::string_view(argv[argc - 2]) == "--report") {
    report = fs::path(argv[argc - 1]);
    argc -= 2;
  }
  const auto root = fs::temp_directory_path() /
                    ("simpsons-reload-check-" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  try {
    Project(root, 1);
    int mutations = 0;
    Runtime runtime(
        [&](std::string_view, const Json&) -> Json {
          ++mutations;
          return nullptr;
        },
        {});
    runtime.Load(root, root / "storage", {"lua"});
    runtime.EnableInspection(true);
    runtime.Tick(true, .01);
    const auto saved = runtime.Save();
    SourceJob restore_job(runtime.Sources(), root / "cache", false);
    auto restore_sources = Finish(restore_job);
    fs::rename(root / "lua", root / "moved-lua");
    auto prepared_restore = runtime.PrepareRestore(saved, false, restore_sources);
    fs::rename(root / "moved-lua", root / "lua");
    Require(
        prepared_restore->Save().at("mods").at(0).at("state") == saved.at("mods").at(0).at("state"),
        "Prepared restore does not reread Lua files on the game thread");
    prepared_restore.reset();

    // Checkpoint preparation also captures its JSON and verified source on the
    // worker. Both restore and vanilla initialization must work with all source
    // files unavailable to the game thread after that capture.
    const auto checkpoint = runtime.Save(false);
    WriteText(root / "checkpoint/mods.json", checkpoint.dump());
    SourceJob checkpoint_job(runtime.Sources(), root / "cache", false, root / "checkpoint/mods.json");
    auto checkpoint_sources = Finish(checkpoint_job);
    Require(checkpoint_sources.checkpoint_prepared &&
                checkpoint_sources.checkpoint_state == checkpoint,
            "Checkpoint preparation captures the complete immutable mod state");
    fs::rename(root / "lua", root / "moved-lua");
    fs::rename(root / "checkpoint", root / "moved-checkpoint");
    auto restored_checkpoint = runtime.PrepareRestore(*checkpoint_sources.checkpoint_state, true,
                                                     checkpoint_sources);
    auto initialized_checkpoint = runtime.PrepareInitialState(checkpoint_sources);
    Require(restored_checkpoint->Save(false).at("mods").at(0).at("state") ==
                checkpoint.at("mods").at(0).at("state") &&
                initialized_checkpoint->Save(false).at("mods").at(0).at("state").at("frames") == 0,
            "Checkpoint restore and vanilla initialization do not reread removed code or sidecars");
    restored_checkpoint.reset(); initialized_checkpoint.reset();
    fs::rename(root / "moved-lua", root / "lua");
    fs::rename(root / "moved-checkpoint", root / "checkpoint");
    SourceJob vanilla_job(runtime.Sources(), root / "cache", false, root / "vanilla/mods.json");
    auto vanilla_sources = Finish(vanilla_job);
    Require(vanilla_sources.checkpoint_prepared && !vanilla_sources.checkpoint_state,
            "A missing checkpoint sidecar explicitly selects vanilla initialization");
    fs::rename(root / "lua", root / "moved-lua");
    auto vanilla_checkpoint = runtime.PrepareInitialState(vanilla_sources);
    fs::rename(root / "moved-lua", root / "lua");
    Require(vanilla_checkpoint->Pages().at(0).title == "Version 1",
            "Vanilla checkpoint initialization uses the worker's frozen code");
    vanilla_checkpoint.reset();
    WriteText(root / "checkpoint/mods.json", "{");
    SourceJob bad_checkpoint(runtime.Sources(), root / "cache", false, root / "checkpoint/mods.json");
    Reject([&] { Finish(bad_checkpoint); }, "Malformed checkpoint JSON rejects during background preparation");
    WriteText(root / "checkpoint/mods.json", checkpoint.dump());
    WriteText(root / "lua/main.lua", Script(99));
    SourceJob changed_checkpoint(runtime.Sources(), root / "cache", false, root / "checkpoint/mods.json");
    Reject([&] { Finish(changed_checkpoint); }, "Changed checkpoint sources reject before any callbacks are replaced");
    WriteText(root / "lua/main.lua", Script(1));
    Require(runtime.Save().at("mods").at(0).at("state") == saved.at("mods").at(0).at("state") &&
                runtime.Pages().at(0).title == "Version 1" && mutations == 0,
            "Failed checkpoint preparation preserves live state, menus and gameplay");
    WriteText(root / "lua/main.lua", Script(2));
    SourceJob reload_job(runtime.Sources(), root / "cache", true);
    runtime.Tick(true, .01);
    auto sources = Finish(reload_job);
    Require(reload_job.Status().at("completed") == 1, "Preparation reports completed mods");
    auto replacement = runtime.PrepareReload(sources);
    Require(runtime.Pages().at(0).title == "Version 1" &&
                replacement->Pages().at(0).title == "Version 2",
            "Replacement menus are staged separately from live menus");
    const auto before_commit = runtime.Save();
    Require(before_commit.at("mods").at(0).at("state").at("frames") == 2 && mutations == 0,
            "Source verification and staging keep the original code running");
    Require(replacement->Save().at("mods").at(0).at("timers") ==
                before_commit.at("mods").at(0).at("timers"),
            "Reload preserves timer delays and data");
    runtime.Stop();
    replacement->NotifyReloaded();
    replacement->Tick(true, .01);
    const auto reloaded = replacement->Save();
    Require(reloaded.at("mods").at(0).at("state").at("frames") == 4 &&
                reloaded.at("mods").at(0).at("state").at("reloaded") == 2,
            "The new code runs with preserved state and receives code.reloaded");
    Reject([&] { replacement->ValidateSave(saved); },
           "An exact save cannot silently use changed mod code");
    Reject([&] { replacement->PrepareReload(sources); },
           "A preparation cannot replace a newer generation's source");
    WriteText(root / "lua/main.lua", Script(3, true));
    SourceJob bad_job(replacement->Sources(), root / "cache", true);
    auto bad_sources = Finish(bad_job);
    Reject([&] { replacement->PrepareReload(bad_sources); },
           "Gameplay mutations during staged initialization are rejected");
    Require(replacement->Pages().at(0).title == "Version 2" && mutations == 0,
            "Failed preflight leaves the old code, pages and host state in place");
    WriteText(root / "lua/main.lua", Script(3) +
                                         "game.on('state.loaded',function() game.state.frames=123; "
                                         "error('incompatible schema') end)\n");
    SourceJob schema_job(replacement->Sources(), root / "cache", true);
    auto schema_sources = Finish(schema_job);
    Reject([&] { replacement->PrepareReload(schema_sources); },
           "A state migration can reject incompatible JSON before commit");
    Require(replacement->Save().at("mods").at(0).at("state").at("frames") == 4 &&
                replacement->Pages().at(0).title == "Version 2",
            "Failed migration cannot change the running state or menu");
    replacement->Activate("lua:tools", "fail", nullptr, 0);
    Require(replacement->Status().at(0).at("failed").get<bool>(),
            "The fixture mod fails before recovery");
    WriteText(root / "lua/main.lua",
              Script(3) +
                  "game.on('state.loaded',function(e) assert(e.kind=='reload'); "
                  "game.state.migrated=true end)\n");
    SourceJob recovery_job(replacement->Sources(), root / "cache", true);
    auto recovered = replacement->PrepareReload(Finish(recovery_job));
    replacement->Stop();
    recovered->NotifyReloaded();
    recovered->Tick(true, .01);
    Require(!recovered->Status().at(0).at("failed").get<bool>() &&
                recovered->Save().at("mods").at(0).at("state").at("frames") == 7 &&
                recovered->Save().at("mods").at(0).at("state").at("migrated") == true,
            "Corrected code can recover a failed mod's captured JSON state");
    WriteText(root / "lua/gamedata/asset.lua", "changed game script");
    SourceJob asset_job(recovered->Sources(), root / "cache", true);
    Reject([&] { Finish(asset_job); },
           "Game-data changes require restart instead of unsafe live remounts");
    fs::remove_all(root / "lua/gamedata");
    auto metadata = Json::parse(ReadText(root / "lua/mod.json"));
    metadata["dependencies"] = {{"other", "*"}};
    WriteText(root / "lua/mod.json", metadata.dump());
    SourceJob metadata_job(recovered->Sources(), root / "cache", true);
    Reject([&] { Finish(metadata_job); }, "Dependency changes require a newly resolved profile");
    std::stop_source cancellation;
    cancellation.request_stop();
    Reject(
        [&] {
          PrepareSources(recovered->Sources(), root / "cache", true, cancellation.get_token());
        },
        "Cancelled source preparation does not hash or initialize code");
    recovered->Stop();

    if (argc > 2) {
      const auto one = fs::absolute(argv[1]), two = fs::absolute(argv[2]);
      const auto binary = root / "native" / one.filename();
      fs::create_directories(binary.parent_path());
      fs::copy_file(one, binary);
      WriteText(root / "native/mod.json",
                Json{{"api", 1},
                     {"id", "native"},
                     {"name", "Native reload"},
                     {"version", "1.0.0"},
                     {"kind", "native"},
                     {"save_mode", "state"},
                     {"binaries", {{Platform(), Utf8(binary.filename())}}}}
                    .dump());
      Runtime native({}, {});
      native.Load(root, root / "native-storage", {"native"});
      native.Tick(true, .01);
      Require(native.Save().at("mods").at(0).at("state") == Json{{"frames", 1}, {"version", 1}},
              "The original native module is running");
      // The source binary can be replaced while its private image is loaded,
      // including on Windows where loaded DLL files would otherwise be locked.
      fs::copy_file(two, binary, fs::copy_options::overwrite_existing);
      SourceJob native_job(native.Sources(), root / "cache", true);
      auto next_native = native.PrepareReload(Finish(native_job));
      Require(native.Save().at("mods").at(0).at("state").at("version") == 1 &&
                  next_native->Save().at("mods").at(0).at("state").at("version") == 2,
              "Native preflight loads the new DLL beside the still-running original");
      native.Stop();
      next_native->NotifyReloaded();
      next_native->Tick(true, .01);
      Require(
          next_native->Save().at("mods").at(0).at("state") == Json{{"frames", 3}, {"version", 2}},
          "Native reload preserves instance state and runs replacement machine code");
      WriteText(binary, "invalid module");
      SourceJob broken_native(next_native->Sources(), root / "cache", true);
      auto broken_sources = Finish(broken_native);
      Reject([&] { next_native->PrepareReload(broken_sources); },
             "An invalid native image cannot replace the running module");
      next_native->Tick(true, .01);
      Require(next_native->Save().at("mods").at(0).at("state").at("frames") == 5,
              "Failed native preflight leaves the current DLL usable");
      next_native->Stop();
      if (argc > 3) {
        fs::copy_file(argv[3], binary, fs::copy_options::overwrite_existing);
        Runtime unsafe_save(
            [&](std::string_view, const Json&) -> Json {
              ++mutations;
              return nullptr;
            },
            {});
        unsafe_save.Load(root, root / "unsafe-storage", {"native"});
        unsafe_save.Tick(true, .01);
        Reject([&] { unsafe_save.Save(); }, "Native serialization cannot change gameplay");
        unsafe_save.EnableInspection(true);
        Reject([&] { unsafe_save.Inspect({{"action", "state"}, {"owner", "native"}}); },
               "Creator state inspection uses the same read-only serialization guard");
        SourceJob unsafe_job(unsafe_save.Sources(), root / "cache", true);
        const auto prepared = Finish(unsafe_job);
        Reject([&] { unsafe_save.PrepareReload(prepared); },
               "A native serializer's side effects reject reload before retiring its instance");
        Require(mutations == 0 && !unsafe_save.Status().at(0).at("failed").get<bool>(),
                "Rejected serialization preserves the running game and instance");
        unsafe_save.Stop();
      }
    }
    fs::remove_all(root);
    if (!report.empty()) WriteText(report, Json{{"ok", true}, {"checks", checks}}.dump(2));
    std::cout << checks << " source preparation and reload checks passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << "\nEvidence: " << root << '\n';
    return 1;
  }
}
