#include "resources.h"

#include <algorithm>
#include <iostream>
#include <limits>

using namespace simpsons::modkit;

namespace {
int checks = 0;
void Require(bool value, const char* message) {
  ++checks;
  if (!value)
    throw std::runtime_error(message);
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
struct Switch {
  bool value = false;
  uint64_t revision = 0;
  void Set(bool next) {
    value = next;
    ++revision;
  }
  Resources::Binding Binding(bool world = false) {
    return {[this] { return Json(value); }, [this](const Json& next) { Set(next.get<bool>()); },
            [this] { return revision; },
            [](const Json& next) {
              if (!next.is_boolean())
                throw Failure("invalid_argument", "Expected a boolean");
            },
            world};
  }
};
void Project(const fs::path& root, std::string id) {
  WriteText(root / id / "mod.json", Json{{"api", 1},
                                         {"id", id},
                                         {"name", id},
                                         {"version", "1.0.0"},
                                         {"kind", "lua"},
                                         {"entry", "main.lua"},
                                         {"save_mode", "state"}}
                                        .dump());
  WriteText(root / id / "main.lua", R"(
game.on('initialize', function()
  game.ui.register_page({id='controls', title='Resource checks', rows={
    {id='enabled', label='Enabled', type='toggle', value=false},
    {id='fail', label='Fail this mod'}}})
end)
game.on('ui.action', function(event)
  if event.row == 'fail' then error('intentional callback failure') end
  game.players.god_mode(event.value)
end)
)");
}
}  // namespace

int main(int argc, char** argv) {
  const auto root = fs::temp_directory_path() /
                    ("simpsons-resource-check-" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  try {
    Resources resources;
    Switch protection, camera;
    resources.Bind("protection", protection.Binding());
    resources.Bind("camera", camera.Binding(true));
    const HostOwner a{1, "first"}, b{1, "second"}, replacement{2, "first"};
    resources.Set(a, "protection", true);
    resources.Set(b, "protection", false);
    Require(!protection.value, "The latest active owner wins");
    resources.Release(a);
    Require(!protection.value && !resources.Save(b).empty(),
            "Removing another owner preserves the winner");
    resources.Release(b);
    Require(!protection.value && resources.Save(b).empty(), "Last release restores the baseline");
    protection.Set(true);
    resources.Set(a, "protection", false);
    resources.Release(a);
    Require(protection.value, "A preexisting manual value survives resource retirement");
    resources.Set(a, "protection", false);
    protection.Set(false);
    resources.Release(a);
    Require(!protection.value,
            "Manual requests with the same value supersede a mod's old ownership");
    resources.Set(a, "protection", true);
    resources.Set(b, "protection", false);
    resources.Set(a, "protection", true);
    const auto saved_a = resources.Save(a), saved_b = resources.Save(b);
    resources.Validate({saved_a, saved_b});
    resources.Release(a);
    Require(!protection.value, "Releasing a winner reveals the preceding owner's value");
    resources.Release(b);
    resources.Restore(b, saved_b, false);
    resources.Restore(replacement, saved_a, false);
    Reject([&] { resources.Restore(replacement, saved_a, false); },
           "Restoring an already active owner cannot overwrite its leases");
    Require(protection.value, "Saved request order survives replacement runtime generations");
    resources.Release(a);
    Require(protection.value, "Retiring an old generation cannot release its replacement");
    resources.Release(replacement);
    resources.Release(b);
    resources.Restore(replacement, saved_a, false);
    resources.Restore(b, saved_b, false);
    Require(protection.value, "Restoring in another mod order does not change the winner");
    resources.Release(replacement);
    resources.Release(b);
    resources.Set(a, "protection", true);
    resources.Set(a, "camera", true);
    const auto checkpoint = resources.Save(a);
    resources.Release(a);
    resources.Restore(replacement, checkpoint, true);
    Require(protection.value && !camera.value,
            "Checkpoints retain global resources and discard old-world camera state");
    resources.Release(replacement);
    auto bad = saved_a;
    bad["protection"]["value"] = "yes";
    Reject([&] { resources.Validate({bad}); }, "Saved resource values are typed");
    bad = saved_b;
    bad["protection"]["baseline"] = true;
    Reject([&] { resources.Validate({saved_a, bad}); },
           "Owners cannot disagree about a saved baseline");
    Reject([&] { resources.Validate({saved_a, saved_a}); },
           "Saved ownership order cannot be ambiguous");
    bad = saved_a;
    bad["protection"]["order"] = -1;
    Reject([&] { resources.Validate({bad}); }, "Negative saved resource order is invalid");
    bad["protection"]["order"] = std::numeric_limits<uint64_t>::max();
    Reject([&] { resources.Validate({bad}); },
           "Saved resource order cannot overflow the next request");
    Reject([&] { resources.Set({}, "protection", true); }, "A mutation cannot be unowned");
    Reject([&] { resources.Set(a, "missing", true); }, "Unknown host resources are rejected");
    resources.Set(a, "protection", true);
    std::thread cleanup([&] { resources.Release(a); });
    cleanup.join();
    Require(!protection.value, "Host-only resource retirement works from the shutdown thread");

    Resources cleanup_failure;
    Switch first_binding, second_binding;
    bool reject_write = false;
    auto binding = first_binding.Binding();
    binding.write = [&](const Json& value) {
      if (reject_write)
        throw Failure("fixture", "Unavailable host binding");
      first_binding.Set(value.get<bool>());
    };
    cleanup_failure.Bind("first", std::move(binding));
    cleanup_failure.Bind("second", second_binding.Binding());
    cleanup_failure.Set(a, "first", true);
    cleanup_failure.Set(a, "second", true);
    reject_write = true;
    Reject([&] { cleanup_failure.Release(a); }, "A failed cleanup reports its host error");
    Require(!second_binding.value && cleanup_failure.Save(a).empty(),
            "A broken binding cannot strand another resource or retain its retired owner");

    // Exercise the actual Lua callback, fault, save and replacement lifecycle.
    std::map<HostOwner, int> retired;
    HostServices host;
    host.call = [&](const HostOwner& owner, std::string_view method, const Json& args) -> Json {
      if (method != "players.god_mode")
        throw Failure("unsupported", "Unexpected host call");
      resources.Set(owner, "protection", args.at("enabled"));
      return nullptr;
    };
    host.save = [&](const HostOwner& owner) { return resources.Save(owner); };
    host.validate = [&](const std::vector<Json>& state) { resources.Validate(state); };
    host.restore = [&](const HostOwner& owner, const Json& state, bool checkpoint) {
      resources.Restore(owner, state, checkpoint);
    };
    host.release = [&](const HostOwner& owner) {
      ++retired[owner];
      resources.Release(owner);
    };
    Project(root, "first");
    Project(root, "second");
    Runtime runtime({}, {}, "en", host);
    runtime.Load(root, root / "storage", {"first", "second"});
    runtime.Tick(true, 0.01);
    runtime.Activate("first:controls", "enabled", true, 0);
    runtime.Activate("second:controls", "enabled", false, 0);
    runtime.Activate("first:controls", "enabled", true, 0);
    const auto saved = runtime.Save();
    Require(!saved.at("mods").at(0).at("host").empty(), "Snapshots include owned host state");
    runtime.EnableInspection(true);
    Require(!runtime.Inspect({{"action", "inspect"}}).at("mods").at(0).at("resources").empty(),
            "Creator inspection exposes each mod's owned host resources");
    auto staged = runtime.PrepareRestore(saved);
    Require(protection.value, "Preflight does not change live resources");
    staged.reset();
    Require(protection.value, "Discarded preflight state cannot retire live mod resources");
    staged = runtime.PrepareRestore(saved);
    runtime.Activate("first:controls", "fail", nullptr, 0);
    Require(!protection.value && runtime.Pages().size() == 1,
            "A callback failure removes only its owner's resource and page");
    runtime.Tick(true, 0.01);
    runtime.Stop();
    staged->NotifyRestored();
    Require(protection.value && staged->Pages().size() == 2,
            "Committing a replacement restores saved host ownership");
    staged->Stop();
    Require(!protection.value, "Stopping the replacement restores the baseline");
    Require(std::all_of(retired.begin(), retired.end(),
                        [](const auto& item) { return item.second == 1; }),
            "Each runtime generation retires an owner exactly once");
    if (argc > 1) {
      const auto folder = root / "native-fault";
      fs::create_directories(folder);
      const auto binary = fs::path(argv[1]).filename();
      fs::copy_file(argv[1], folder / binary);
      WriteText(folder / "mod.json", Json{{"api", 1},
                                          {"id", "native-fault"},
                                          {"name", "Native fault fixture"},
                                          {"version", "1.0.0"},
                                          {"kind", "native"},
                                          {"save_mode", "stateless"},
                                          {"binaries", {{Platform(), binary.string()}}}}
                                         .dump());
      std::vector<std::string> messages;
      Runtime native(
          {}, [&](std::string_view message) { messages.emplace_back(message); }, "en", host);
      native.Load(root, root / "storage", {"native-fault"});
      native.Tick(true, 0.01);
      Require(native.Status().at(0).at("failed").get<bool>() && !protection.value,
              "A throwing native callback retires its owned resources");
      native.Stop();
      native.Stop();
      Require(std::count_if(messages.begin(), messages.end(),
                            [](const auto& message) {
                              return message.find("fault-fixture-shutdown-marker") !=
                                     std::string::npos;
                            }) == 1,
              "A throwing native shutdown callback runs exactly once");
      Require(std::any_of(messages.begin(), messages.end(),
                          [](const auto& message) {
                            return message.find("fixture shutdown failure") != std::string::npos;
                          }),
              "A native shutdown exception survives unloading as a host diagnostic");
    }
    fs::remove_all(root);
    std::cout << checks << " resource ownership checks passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    std::error_code ignored;
    fs::remove_all(root, ignored);
    return 1;
  }
}
