#include "runtime.h"
#include <iostream>
#include <set>

using namespace simpsons::modkit;

int main(int argc, char** argv) {
  const auto root = fs::temp_directory_path() /
      ("simpsons-library-check-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  try {
    if (argc != 2) throw Failure("fixture", "Public library example required");
    fs::create_directories(root);
    fs::copy(Utf8Path(argv[1]), root / "example", fs::copy_options::recursive);
    bool index_ready = false, level_ready = false, source_ready = false;
    std::string claim, prior_claim, pending;
    std::set<std::string> streams;
    Json objects = Json::array();
    unsigned spawns = 0, loads = 0, checks = 0;
    const auto require = [&](bool value, const char* message) {
      ++checks;
      if (!value) throw Failure("fixture", message);
    };
    Runtime runtime([&](std::string_view method, const Json& args) -> Json {
      if (method == "world.status") return {{"ready", level_ready}};
      if (method == "assets.search")
        return {{"ready", index_ready}, {"files", 1}, {"total_files", 2},
                {"items", Json::array({{{"id", "asset:1"}, {"name", "Jet engine updraft"}}})}};
      if (method == "assets.templates") {
        Json items = Json::array();
        for (const auto* stream : {"brt/first.str", "brt/second.str"})
          items.push_back({{"supported", true}, {"name", "Jet engine updraft"},
                           {"class", "ScriptableProp"}, {"stream", stream},
                           {"resident_template", streams.contains(stream) ? "template:current" : ""}});
        return {{"ready", true}, {"total", 2}, {"items", items}};
      }
      if (method == "assets.load_source") {
        const auto path = args.at("stream").get<std::string>();
        if (!pending.empty() && pending != path) throw Failure("busy", "Another addition is pending");
        prior_claim = claim;
        claim = path;
        pending = path;
        source_ready = false;
        ++loads;
        return true;
      }
      if (method == "assets.source_streams") return streams;
      if (method == "assets.cancel_source") {
        if (pending.empty()) return false;
        pending.clear();
        claim = prior_claim;
        source_ready = streams.contains(claim);
        return true;
      }
      if (method == "assets.source_status") {
        if (source_ready && !pending.empty()) { streams.insert(pending); pending.clear(); }
        return {{"phase", source_ready ? "ready" : "loading"}, {"owned", !claim.empty()},
                {"ready", source_ready}, {"message", "Native source progress"}};
      }
      if (method == "players.controlled") {
        require(args.at("player") == 1, "Spawn follows the menu's logical player");
        return {{"position", {{"x", 10}, {"y", 3}, {"z", 8}}}};
      }
      if (method == "world.spawn") {
        require(source_ready && args.at("template") == "template:current",
                "Example reacquires a current resident handle before spawning");
        require(args.at("position") == Json{{"x", 14}, {"y", 4}, {"z", 8}},
                "Example uses native coordinates beside the chosen player");
        ++spawns;
        const auto id = "entity:owned" + std::to_string(spawns);
        objects.push_back({{"id", id}});
        return id;
      }
      if (method == "world.owned") return objects;
      if (method == "world.destroy") {
        const auto before = objects.size();
        for (auto it = objects.begin(); it != objects.end();)
          if (it->at("id") == args.at("entity")) it = objects.erase(it);
          else ++it;
        require(objects.size() + 1 == before, "Only the example's owned object is removed");
        return nullptr;
      }
      if (method == "assets.release_source") {
        claim.clear(); pending.clear(); streams.clear(); source_ready = false; return true;
      }
      throw Failure("fixture", "Unexpected operation: " + std::string(method));
    }, [](std::string_view text) { std::cerr << text << '\n'; });
    runtime.Load(root, root / "storage", {"examples.library-objects"});
    const auto action = [&](std::string_view row) {
      runtime.Activate("examples.library-objects:library", row, nullptr, 1);
    };
    const auto frames = [&](unsigned count) {
      for (unsigned i = 0; i < count; ++i) runtime.Tick(level_ready, 0);
    };
    frames(1);
    action("inspect");
    index_ready = true;
    frames(30);
    require(spawns == 0 && loads == 0, "Indexing never loads sources or spawns automatically");
    action("load");
    require(loads == 0, "Frontend actions cannot mutate an unloaded world");
    level_ready = true;
    frames(1);
    action("inspect");
    action("spawn");
    require(spawns == 0, "Disk metadata is never used as a spawn handle");
    action("load");
    frames(30);
    require(loads == 1 && spawns == 0, "Async loading waits without automatic spawning");
    source_ready = true;
    frames(30);
    action("spawn");
    require(spawns == 1 && objects.size() == 1, "The example's explicit Spawn action works");
    action("next");
    action("load");
    require(claim == "brt/second.str" && loads == 2 && objects.size() == 1,
            "A source addition preserves the example's earlier object");
    action("cancel");
    require(claim == "brt/first.str" && pending.empty() && objects.size() == 1,
            "Cancel withdraws the pending addition without clearing the earlier lease or object");
    action("load");
    source_ready = true;
    frames(30);
    action("spawn");
    require(spawns == 2 && objects.size() == 2 && streams.size() == 2,
            "A completed addition permits another explicit spawn without replacing earlier objects");
    action("clear");
    require(objects.empty() && claim.empty(), "Clear removes owned objects and releases only its claim");
    frames(60);
    require(spawns == 2, "Completion and further frames do not duplicate objects");
    runtime.Stop();
    fs::remove_all(root);
    std::cout << "{\"ok\":true,\"checks\":" << checks << "}\n";
  } catch (const std::exception& error) {
    fs::remove_all(root);
    std::cerr << error.what() << '\n';
    return 1;
  }
}
