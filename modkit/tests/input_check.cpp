#include "input_state.h"

#include <iostream>

using namespace simpsons::modkit;

namespace {
int checks = 0;
void Require(bool value, const char* message) {
  ++checks;
  if (!value)
    throw Failure("fixture", message);
}
std::array<uint8_t, 16> Sample(uint16_t buttons = 0, uint8_t left = 0, uint8_t right = 0,
                               int16_t lx = 0, int16_t ly = 0, int16_t rx = 0, int16_t ry = 0) {
  std::array<uint8_t, 16> bytes{};
  const uint16_t words[] = {buttons,      uint16_t(uint16_t(left) << 8 | right),
                            uint16_t(lx), uint16_t(ly),
                            uint16_t(rx), uint16_t(ry)};
  for (size_t i = 0; i < std::size(words); ++i) {
    bytes[4 + i * 2] = uint8_t(words[i] >> 8);
    bytes[5 + i * 2] = uint8_t(words[i]);
  }
  return bytes;
}
void Observe(InputSnapshots& input, uint32_t player, const std::array<uint8_t, 16>& bytes,
             bool available = true) {
  input.Observe(player, input.Epoch(), available, bytes);
}
}  // namespace

int main(int argc, char** argv) {
  const auto root = fs::temp_directory_path() /
                    ("simpsons-input-check-" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  try {
    InputSnapshots input;
    input.BeginFrame();
    Require(!input.Read(0).at("sampled") && !input.Read(1).at("available"),
            "Unpolled players have neutral snapshots, not invented device connections");
    Observe(input, 0, Sample(0x1000));
    Observe(input, 1, Sample(0x4000));
    input.BeginFrame();
    Require(input.Read(0)["buttons"]["a"] && !input.Read(0)["pressed"]["a"] &&
                input.Read(1)["buttons"]["x"] && !input.Read(1)["pressed"]["x"],
            "The first observation establishes independent baselines without phantom presses");
    Observe(input, 0, Sample());
    Observe(input, 1, Sample());
    input.BeginFrame();
    Require(input.Read(0)["released"]["a"] && input.Read(1)["released"]["x"],
            "Releases belong to the logical player that held each control");
    Observe(input, 0, Sample(0x1000, 30, 255, -32768, 32767, 0, -16384));
    Observe(input, 1, Sample(0x4000, 31));
    input.BeginFrame();
    const auto first = input.Read(0), second = input.Read(1);
    Require(first["pressed"]["a"] && !first["pressed"]["x"] && second["pressed"]["x"] &&
                !second["pressed"]["a"],
            "Logical-player button edges do not merge between players");
    Require(
        first["left_stick"]["x"] == -1 && first["left_stick"]["y"] == 1 &&
            first["right_stick"]["x"] == 0 && first["right_stick"]["y"] == -0.5 &&
            first["right_trigger"] == 1 && !first["buttons"]["left_trigger"] &&
            second["pressed"]["left_trigger"],
        "Big-endian axes normalize at both endpoints and triggers retain the runtime threshold");
    Observe(input, 0, Sample());
    Require(input.Read(0) == first && input.Read(1) == second,
            "Device polls between callbacks cannot change the shared frame snapshot");
    input.BeginFrame();
    Require(input.Read(0)["released"]["a"] && !input.Read(1).at("updated") &&
                input.Read(1)["buttons"]["x"] && !input.Read(1)["pressed"]["x"],
            "An unpolled channel retains its last sample without repeating its edges");
    Observe(input, 0, Sample(0x2000));
    Observe(input, 0, Sample());
    input.BeginFrame();
    Require(input.Read(0)["pressed"]["b"] && input.Read(0)["released"]["b"] &&
                !input.Read(0)["buttons"]["b"],
            "A quick press and release between host updates survives as both transitions");
    Observe(input, 1, Sample(0xFFFF), false);
    input.BeginFrame();
    Require(!input.Read(1).at("available") && !input.Read(1)["buttons"]["x"] &&
                input.Read(1)["released"]["x"] && input.Read(1)["released"]["left_trigger"],
            "Disconnect releases held controls without reading stale output bytes");
    Observe(input, 1, Sample(0x8000));
    input.BeginFrame();
    Require(input.Read(1)["buttons"]["y"] && !input.Read(1)["pressed"]["y"],
            "Reconnecting with a held button does not synthesize a press");
    const auto retired = input.Epoch();
    const auto previous_frame = input.Read(0).at("frame");
    input.Reset();
    input.Observe(0, retired, true, Sample(0xFFFF));
    input.BeginFrame();
    Require(!input.Read(0).at("sampled") && input.Read(0).at("frame") > previous_frame,
            "An in-flight poll from a retired world cannot repopulate restored input");
    Observe(input, 0, Sample(0x1000));
    input.BeginFrame();
    Require(!input.Read(0)["pressed"]["a"] && !input.Read(0)["released"]["a"],
            "Guest rewinds reestablish a baseline without old input edges");
    bool rejected = false;
    try {
      input.Read(2);
    } catch (const Failure& error) {
      rejected = error.code == "invalid_argument";
    }
    Require(rejected, "Only the two supported logical players are accepted");

    // Two independent Lua mods must see the same edges. Their reads do not
    // consume a queue, depend on callback order or poll a physical device.
    for (const auto id : {"one", "two"}) {
      WriteText(root / id / "mod.json", Json{{"api", 1},
                                             {"id", id},
                                             {"name", id},
                                             {"version", "1.0.0"},
                                             {"kind", "lua"},
                                             {"entry", "main.lua"},
                                             {"save_mode", "state"},
                                             {"requires_operations", {"input.state"}}}
                                            .dump());
      WriteText(root / id / "main.lua",
                "game.state.p1=0; game.state.p2=0\n"
                "game.on('simulation',function()\n"
                " local a=game.input.state(0); local b=game.input.state(1)\n"
                " assert(a.frame==b.frame and game.input.state(0).pressed.a==a.pressed.a)\n"
                " if a.pressed.a then game.state.p1=game.state.p1+1 end\n"
                " if b.pressed.x then game.state.p2=game.state.p2+1 end\n"
                "end)\n");
    }
    input.Reset();
    Observe(input, 0, Sample());
    Observe(input, 1, Sample());
    input.BeginFrame();
    Runtime runtime(
        [&](std::string_view method, const Json& args) {
          if (method != "input.state")
            throw Failure("fixture", "Unexpected game operation");
          return input.Read(args.at("player").get<uint32_t>());
        },
        [](std::string_view message) { std::cerr << message << '\n'; });
    runtime.Load(root, root / "storage", {"one", "two"});
    runtime.EnableInspection(true);
    runtime.Tick(true, 1.0 / 60);
    Observe(input, 0, Sample(0x1000));
    Observe(input, 1, Sample(0x4000));
    input.BeginFrame();
    runtime.Tick(true, 1.0 / 60);
    input.BeginFrame();
    runtime.Tick(true, 1.0 / 60);
    for (const auto id : {"one", "two"}) {
      const auto state = runtime.Inspect({{"action", "state"}, {"owner", id}});
      Require(state["p1"] == 1 && state["p2"] == 1,
              "Each mod sees each player's single edge exactly once");
    }
    runtime.Stop();

    Require(argc == 2, "The public input example is required for qualification");
    fs::copy(Utf8Path(argv[1]), root / "observer", fs::copy_options::recursive);
    input.Reset();
    Observe(input, 0, Sample());
    Observe(input, 1, Sample());
    input.BeginFrame();
    std::array<Json, 2> actors = {Json{{"character", "Bart"}}, Json{{"character", "Homer"}}};
    Runtime example(
        [&](std::string_view method, const Json& args) {
          if (method == "players.controlled") return actors.at(args.at("player").get<size_t>());
          if (method != "input.state")
            throw Failure("fixture", "Unexpected example operation");
          return input.Read(args.at("player").get<uint32_t>());
        },
        [](std::string_view message) { std::cerr << message << '\n'; });
    example.Load(root, root / "example-storage", {"examples.input-observer"});
    example.Tick(true, 0.25);
    Observe(input, 0, Sample(0x4000));
    Observe(input, 1, Sample(0x8000));
    input.BeginFrame();
    example.Tick(true, 0.25);
    auto pages = example.Pages();
    Require(pages.size() == 1 && pages[0].rows[0].label == "P1 presses: 1; last: x" &&
                pages[0].rows[1].label == "P2 presses: 1; last: y" &&
                pages[0].rows[2].label == "P1 character: Bart" &&
                pages[0].rows[3].label == "P2 character: Homer",
            "The public example reports independent logical-player presses in its native page");
    actors[0] = Json{{"character", "Lisa"}};
    actors[1] = nullptr;
    input.BeginFrame();
    example.Tick(true, 0.25);
    Require(example.Pages()[0].rows[0].label == pages[0].rows[0].label,
            "The example does not count a held button twice");
    Require(example.Pages()[0].rows[2].label == "P1 character: Lisa" &&
                example.Pages()[0].rows[3].label == "P2 character: Unassigned",
            "Character switching and dropout refresh ownership independently of held controls");
    example.Activate("examples.input-observer:inputs", "record", false, 1);
    Observe(input, 0, Sample(0x8000));
    input.BeginFrame();
    example.Tick(true, 0.25);
    Require(example.Pages()[0].rows[0].label == pages[0].rows[0].label,
            "P2 can stop recording through its native menu without changing gameplay input");
    example.Activate("examples.input-observer:inputs", "clear", nullptr, 1);
    Require(example.Pages()[0].rows[0].label == "P1 presses: 0; last: none" &&
                !example.Status()[0].at("failed"),
            "The public example's reset action is valid and leaves the mod running");
    example.Tick(false, 0.25);
    Require(example.Pages()[0].rows[2].label == "P1 character: Unassigned" &&
                example.Pages()[0].rows[3].label == "P2 character: Unassigned",
            "Unloading the world clears displayed actor ownership");
    example.Stop();
    fs::remove_all(root);
    std::cout << checks << " input snapshot checks passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << "\nEvidence: " << root << '\n';
    return 1;
  }
}
