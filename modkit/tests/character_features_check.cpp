// Exercise the shipped Lua package with the real runtime and resource leases.
#include "resources.h"
#include "asset_selection.h"
#include "../../simpsons/src/feature_clock.h"
#include "../../simpsons/src/ragdoll_settings.h"

#include <cmath>
#include <iostream>
#include <limits>

using namespace simpsons::modkit;
namespace {
int checks = 0;
void Require(bool ok, const char* message) {
  ++checks;
  if (!ok) throw std::runtime_error(message);
}
template <typename Function> void Reject(Function function, const char* message) {
  bool rejected = false;
  try { function(); } catch (const std::exception&) { rejected = true; }
  Require(rejected, message);
}
}

int main(int argc, char** argv) {
  const auto root = fs::temp_directory_path() /
      ("simpsons-character-features-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  try {
    if (argc != 2) throw std::runtime_error("Pass the shipped knockdown package path");
    players::FeatureClock clock;
    Require(clock.Sample(100, true) == 0, "First sample does not manufacture a frame");
    Require(clock.Sample(100, true) == 0, "Repeated render draws do not advance effects");
    Require(std::abs(clock.Sample(1100, true) - 0.001f) < 1e-7f,
            "1000 FPS keeps the real delta without a 240 FPS minimum");
    Require(clock.Sample(100000, false) == 0, "Pause advances no effects");
    Require(clock.Sample(5000000, false) == 0, "Long pause remains frozen");
    Require(clock.Sample(5000000, true) == 0, "Resume does not catch up paused time");
    Require(std::abs(clock.Sample(5001000, true) - 0.001f) < 1e-7f, "Resume uses the next real step");
    Require(clock.Sample(1, true) == 0, "Save restore or world reset cannot underflow time");
    Require(clock.Sample(1000001, true) == 0.05f, "Slow-frame effect work is bounded");
    clock.Reset();
    Require(clock.Sample(2000000, true) == 0, "A new session primes the clock again");

    AssetSnapshot snapshot;
    snapshot.ready = true;
    Asset asset; asset.name = "Wiggum"; asset.objects = {0};
    snapshot.assets.push_back(asset);
    snapshot.objects.push_back({"actor-guid", "model-guid", "rig-guid", "test.str", 42});
    Require(LibraryTemplateMatches(snapshot, 0, "actor-guid", 42, "model-guid"),
            "An authored object matches its own class, GUID and model");
    Require(!LibraryTemplateMatches(snapshot, 0, "different-guid", 42, "model-guid"),
            "The same name and class cannot select an unrelated actor");
    Require(!LibraryTemplateMatches(snapshot, 0, "actor-guid", 99, "model-guid"),
            "A different factory is rejected");
    Require(!LibraryTemplateMatches(snapshot, 0, "actor-guid", 42, "wrong-rig-model"),
            "A mismatched model cannot substitute a different NPC");
    Require(!LibraryTemplateMatches(snapshot, 1, "actor-guid", 42, "model-guid"),
            "A stale asset index is rejected");
    snapshot.assets[0].objects = {10};
    Require(!LibraryTemplateMatches(snapshot, 0, "actor-guid", 42, "model-guid"),
            "Invalid library object references fail safely");
    snapshot.ready = false;
    Require(!LibraryTemplateMatches(snapshot, 0, "actor-guid", 42, "model-guid"),
            "An unfinished index is not a spawn capability");

    auto current = players::SaveRagdollSettings({});
    const auto baseline = current;
    uint64_t revision = 0;
    Resources resources;
    resources.Bind("knockdowns", {
        [&] { return current; },
        [&](const Json& value) { current = players::SaveRagdollSettings(players::ParseRagdollSettings(value)); ++revision; },
        [&] { return revision; },
        [](const Json& value) { players::ParseRagdollSettings(value); }});
    for (auto bad : {Json(nullptr), Json::object(), Json{{"enabled", true}}})
      Reject([&] { players::ParseRagdollSettings(bad); }, "Incomplete settings are rejected");
    auto invalid = baseline; invalid["force_scale"] = std::numeric_limits<double>::infinity();
    Reject([&] { players::ParseRagdollSettings(invalid); }, "Non-finite force is rejected");
    invalid = baseline; invalid["enabled"] = "true";
    Reject([&] { players::ParseRagdollSettings(invalid); }, "Switch strings are rejected");
    invalid = baseline; invalid["force_scale"] = 4.01;
    Reject([&] { players::ParseRagdollSettings(invalid); }, "Out-of-range force is rejected");

    fs::create_directories(root);
    fs::copy(Utf8Path(argv[1]), root / "euphoria.ragdolls", fs::copy_options::recursive);
    HostServices host;
    host.call = [&](const HostOwner& owner, std::string_view operation, const Json& args) -> Json {
      if (operation != "physics.knockdowns") throw Failure("fixture", "Unexpected host operation");
      resources.Set(owner, "knockdowns", players::SaveRagdollSettings(
          players::ParseRagdollSettings(args.at("options"))));
      return nullptr;
    };
    host.save = [&](const HostOwner& owner) { return resources.Save(owner); };
    host.validate = [&](const std::vector<Json>& states) { resources.Validate(states); };
    host.restore = [&](const HostOwner& owner, const Json& state, bool checkpoint) {
      resources.Restore(owner, state, checkpoint);
    };
    host.release = [&](const HostOwner& owner) { resources.Release(owner); };
    {
      Runtime runtime([](std::string_view, const Json&) -> Json {
        throw Failure("fixture", "Unexpected unowned operation");
      }, [](std::string_view message) { std::cerr << message << '\n'; }, "en", host);
      runtime.Load(root, root / "storage", {"euphoria.ragdolls"});
      runtime.Tick(true, 0);
      Require(current.at("enabled") == true, "Loading the actual Lua package acquires its settings");
      Require(!runtime.Pages().empty(), "The package registers an in-game options page");
      runtime.Activate("euphoria.ragdolls:euphoria_ragdolls", "enabled", false, 0);
      Require(current.at("enabled") == false, "The Lua toggle changes the owned host request");
      runtime.Activate("euphoria.ragdolls:euphoria_ragdolls", "enabled", true, 0);
      runtime.Activate("euphoria.ragdolls:euphoria_ragdolls", "punch_impact", false, 0);
      Require(current.at("enabled") == true && current.at("punch_impact") == false,
              "Independent options preserve the other switches");
    }
    Require(current == baseline, "Unloading the package restores the disabled baseline");
    fs::remove_all(root);
    std::cout << "character features: " << checks << " checks passed\n";
    return 0;
  } catch (const std::exception& error) {
    fs::remove_all(root);
    std::cerr << error.what() << '\n';
    return 1;
  }
}
