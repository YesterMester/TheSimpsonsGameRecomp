#include "source_state.h"

#include <fstream>
#include <iostream>

using namespace simpsons::modkit;
namespace {
unsigned checks = 0;
void Require(bool condition, const char* text) {
  ++checks;
  if (!condition) throw std::runtime_error(text);
}
template<class Function> void Reject(Function function, const char* text) {
  bool rejected = false;
  try { function(); } catch (const std::exception&) { rejected = true; }
  Require(rejected, text);
}
Json Source() {
  return {{"api", 1}, {"path", "brt/brt/challenge_mode/challenge_mode_design.str"},
          {"world", 0xE0000000u}, {"manager", 0xE1000000u}, {"definitions", 343u},
          {"debug_claimed", false}, {"before", {4u, 5u, 6u}},
          {"files", Json::array({{{"key", 18u}, {"address", 0xE2000000u}, {"name", "brt\\brt_global.str"}},
                                {{"key", 19u}, {"address", 0xE2000400u}, {"name", "brt\\brt.str"}},
                                {{"key", 20u}, {"address", 0xE2000800u},
                                 {"name", "brt\\brt\\challenge_mode\\challenge_mode_design.str"}}})},
          {"bodies", Json::array({{{"address", 0xE3000000u}, {"key", 20u}}})}};
}
Json Profile() {
  return {{"api", 1}, {"spawn_source", Source()},
          {"mods", Json::array({{{"id", "creator.first"},
                                 {"host", {{"source_stream", Source()["path"]}}}},
                                {{"id", "creator.second"},
                                 {"host", {{"source_stream", Source()["path"]}}}}})}};
}
Json Session() {
  auto source = Source();
  const std::string second = "nvr/nvr/zone01/zone01_design.str";
  source["api"] = 2;
  source["streams"] = Json::array({source["path"], second});
  for (auto& file : source["files"]) file["source"] = source["path"];
  source["files"].push_back({{"key", 21u}, {"address", 0xE2000C00u},
      {"name", "nvr\\nvr\\zone01\\zone01_design.str"}, {"source", second}});
  source["bodies"].push_back({{"address", 0xE3000400u}, {"key", 21u}});
  source["definitions"] = 355u;
  return source;
}
}  // namespace

int main(int argc, char** argv) {
  try {
    struct File { uint32_t address; std::string name; };
    const std::map<uint32_t, File> owned{{20, {0xE2000800u, "brt\\source.str"}}};
    Require(source_state::OwnsFile(owned, 20, 0xE2000800u, "brt\\source.str"),
            "The exact native stream identity belongs to the source");
    Require(!source_state::OwnsFile(owned, 4, 0xE2000000u, "level\\base.str") &&
            !source_state::OwnsFile(owned, 99, 0xE2000C00u, "level\\later.str"),
            "Borrowed files remain outside ownership even when they streamed later");
    Require(!source_state::OwnsFile(owned, 20, 0xE2000C00u, "brt\\source.str") &&
            !source_state::OwnsFile(owned, 20, 0xE2000800u, "level\\replacement.str"),
            "Reused keys and pool nodes cannot retire a replacement's resources");
    Require(source_state::Path("BRT/BRT.str") == "brt/brt.str", "Guest paths compare case-insensitively");
    Require(source_state::Path("BRT\\BRT.STR", true) == "brt/brt.str", "Native stream filenames use backslashes");
    for (const auto* path : {"", "/brt.str", "../brt.str", "brt/../brt.str", "brt/./brt.str",
                             "brt//brt.str", "C:/brt.str", "brt\\brt.str", "brt/brt.bin"})
      Require(!source_state::Path(path), "Invalid public paths cannot acquire an existing group");
    source_state::Validate(Source());
    source_state::ValidateClaims(Profile());
    ++checks;
    for (const Json version : {Json(4294967298ULL), Json(-4294967294LL), Json(2.0), Json(true)}) {
      auto invalid = Session();
      invalid["api"] = version;
      Reject([&] { source_state::Validate(invalid); },
             "A malformed or overflowing version cannot be truncated into a supported session");
    }
    auto profile = Profile();
    profile["mods"][0]["host"]["source_stream"] = "dolphins/dolphins.str";
    Reject([&] { source_state::ValidateClaims(profile); },
           "A valid path for the wrong group rejects before native memory restore");
    profile = Profile();
    profile["spawn_source"] = nullptr;
    Reject([&] { source_state::ValidateClaims(profile); }, "A claim cannot rebind without its native group");
    profile = Profile();
    profile["mods"] = Json::array();
    Reject([&] { source_state::ValidateClaims(profile); }, "An ownerless group cannot survive a restore");
    profile["spawn_source"]["debug_claimed"] = true;
    source_state::ValidateClaims(profile);
    ++checks;
    profile["spawn_source"].erase("debug_claimed");
    source_state::ValidateClaims(profile);
    ++checks;  // Legacy debug-only snapshots predate the explicit claim field.
    profile = Profile();
    profile["mods"][0]["host"]["source_stream"] = "BRT/BRT/CHALLENGE_MODE/CHALLENGE_MODE_DESIGN.str";
    source_state::ValidateClaims(profile);
    ++checks;
    for (const auto* path : {"", "../escape.str", "brt/other.str"}) {
      auto source = Source();
      source["path"] = path;
      Reject([&] { source_state::Validate(source); }, "The target must be a valid owned native file");
    }
    for (const auto* field : {"world", "manager"}) {
      auto source = Source();
      source[field] = 0xE0000001u;
      Reject([&] { source_state::Validate(source); }, "Misaligned native roots reject before restore");
    }
    auto source = Source();
    source["files"][0]["key"] = 4u;
    Reject([&] { source_state::Validate(source); }, "A borrowed level file cannot become owned on restore");
    source = Source();
    source["files"][1]["address"] = source["files"][0]["address"];
    Reject([&] { source_state::Validate(source); }, "Two source keys cannot alias one file-pool node");
    source = Source();
    source["bodies"][0]["key"] = 99u;
    Reject([&] { source_state::Validate(source); }, "A definition body needs a retained native owner");
    source = Source();
    source["definitions"] = 0u;
    Reject([&] { source_state::Validate(source); }, "A ready source contains registered definitions");
    Json empty = {{"api", 1}, {"mods", Json::array()}};
    source_state::ValidateClaims(empty);
    ++checks;
    source = Session();
    profile = Profile(); profile["spawn_source"] = source;
    profile["mods"][1]["host"]["source_stream"] = source["streams"][1];
    source_state::ValidateClaims(profile);
    Require(source_state::Paths(source).size() == 2,
            "A saved session retains two independently selected source roots");
    source["files"][3]["source"] = source["path"];
    Reject([&] { source_state::Validate(source); }, "Each target stream needs its own original load identity");
    source = Session(); source["streams"].push_back(source["path"]);
    Reject([&] { source_state::Validate(source); }, "Repeated session paths cannot hide a missing source");
    source = Session(); source["streams"][1] = "../outside.str";
    Reject([&] { source_state::Validate(source); }, "Additional source paths retain the same containment rules");
    source = Session(); source["files"][3]["source"] = "missing/missing.str";
    Reject([&] { source_state::Validate(source); }, "A file cannot be assigned to an unretained source");
    source = Session(); source["files"].erase(3);
    Reject([&] { source_state::Validate(source); }, "Every retained archive must have a saved native target file");
    source = Session(); source["debug_claimed"] = true;
    Reject([&] { source_state::Validate(source); }, "A new-format debug owner needs its actual selected source");
    source["debug_stream"] = source["streams"][1]; source_state::Validate(source); ++checks;
    source["debug_stream"] = "missing/missing.str";
    Reject([&] { source_state::Validate(source); }, "The debug owner cannot claim an unknown archive");
    source = Session(); source["debug_stream"] = source["path"];
    Reject([&] { source_state::Validate(source); }, "An unclaimed debug path cannot retain a session");
    source = Session(); source["streams"] = Json::array();
    for (size_t i = 0; i <= source_state::kMaxSources; ++i)
      source["streams"].push_back("episode/source" + std::to_string(i) + ".str");
    Reject([&] { source_state::Validate(source); }, "Snapshot source counts retain the native session's bound");
    profile = Profile(); profile["spawn_source"] = Session();
    profile["mods"][0]["host"]["source_stream"] = "missing/missing.str";
    Reject([&] { source_state::ValidateClaims(profile); }, "Every owner must claim a committed root in the session");
    profile = Profile(); profile["spawn_source"]["streams"] = Session()["streams"];
    profile["mods"][1]["host"]["source_stream"] = Session()["streams"][1];
    Reject([&] { source_state::ValidateClaims(profile); }, "Legacy metadata cannot acquire undeclared multi-source ownership");
    for (int i = 1; i < argc; ++i) {
      std::ifstream file(argv[i]);
      Require(bool(file), "A captured native profile must be readable");
      source_state::ValidateClaims(Json::parse(file));
      ++checks;
    }
    std::cout << "{\"ok\":true,\"checks\":" << checks << "}\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
