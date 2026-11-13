// The debug menu: toys and cheats for the running game, drawn in the game's
// own frame like an open-world game's trainer (game_ui.cpp draws its list).
//
// The game reads its controllers, updates and draws on one thread, which also
// runs the menu; changes to the world are queued for the start of the next
// frame (game_thread.h) rather than made while the game is drawing. Progress
// changes go through the ScoreKeeper like the game's own scripts, and avoid
// the scores that achievements watch: episodes are unlocked through their
// _LOCKED scores, never by completing them.

#include "debug_menu.h"

#include "achievements.h"
#include "characters.h"
#include "freecam.h"
#include "game_canvas.h"
#include "game_thread.h"
#include "game_ui.h"
#include "guest_memory.h"
#include "players.h"
#include "native_ragdoll_preview.h"
#include "spawner.h"
#include "spawn_sources.h"
#include "assets.h"
#include "resource_id.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <chrono>
#include <deque>
#include <optional>
#include <string>
#include <vector>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DECLARE(bool, achievement_notifications);
REXCVAR_DECLARE(bool, ragdoll_mod_enabled);
REXCVAR_DECLARE(bool, ragdoll_run_impact);
REXCVAR_DECLARE(bool, ragdoll_punch_impact);
REXCVAR_DECLARE(bool, ragdoll_push_downed);
REXCVAR_DECLARE(bool, ragdoll_player_enabled);
REXCVAR_DECLARE(double, ragdoll_force_scale);
REXCVAR_DECLARE(std::string, quick_launch_level);
REXCVAR_DEFINE_BOOL(cheats, false, "Game",
                    "Turn on the debug menu's cheats (god mode, spawning, speed, unlocks, "
                    "character swaps); achievements unlocked meanwhile are marked");

// physics_step.cpp: the Havok world the game last stepped.
uint32_t HavokWorld();
namespace game_ui {
void TickLibrarySpawn(PPCContext& ctx, uint8_t* base);
}

namespace debug_menu {
namespace {

using simpsons::LoadGuestFloat;
using simpsons::StoreGuestFloat;

// The game clock's time scale (game_clock.cpp): 1 normally, 0 while paused.
constexpr uint32_t kTimeScale = 0x82CED750;
// The Havok world's gravity vector (hkpWorld +16), as the level set it.
constexpr uint32_t kWorldGravity = 16;

std::filesystem::path g_config;
std::atomic<float> g_speed{1.0f};
std::atomic<float> g_gravity{1.0f};
std::atomic<bool> g_hide_hud{false};
float g_written_speed = 1.0f;  // game thread
uint32_t g_gravity_world = 0;
std::array<float, 3> g_level_gravity{};
float g_written_gravity = 1.0f;

}  // namespace

void Init(const std::filesystem::path& config) {
  g_config = config;
}

void Tick(PPCContext& ctx, uint8_t* base) {
  game_ui::TickLibrarySpawn(ctx, base);
  spawner::TickSpooner(ctx, base);
  native_ragdoll::TickPlayerPreviews(ctx, base);
  players::TickFeatures(ctx, base);
  players::Log(ctx, base);
  // Game speed scales the game's normal speed. Pauses (0) and other values the
  // game sets itself are left alone.
  const float speed = g_speed.load();
  const float current = LoadGuestFloat(base, kTimeScale);
  if ((std::abs(current - 1.0f) < 1e-4f || current == g_written_speed) && current != speed) {
    StoreGuestFloat(base, kTimeScale, speed);
    if (speed != g_written_speed)
      REXLOG_INFO("[debug-menu] game speed {:.2f}x", speed);
    g_written_speed = speed;
  }
  // Gravity scales the level's own, which is kept for each world.
  const uint32_t world = HavokWorld();
  if (world) {
    if (world != g_gravity_world) {
      g_gravity_world = world;
      for (int axis = 0; axis < 3; ++axis)
        g_level_gravity[axis] = LoadGuestFloat(base, world + kWorldGravity + axis * 4);
      g_written_gravity = 1.0f;
    }
    const float gravity = g_gravity.load();
    if (gravity != g_written_gravity) {
      for (int axis = 0; axis < 3; ++axis)
        StoreGuestFloat(base, world + kWorldGravity + axis * 4, g_level_gravity[axis] * gravity);
      g_written_gravity = gravity;
      REXLOG_INFO("[debug-menu] gravity {:.2f}x", gravity);
    }
  }
}

bool HideHud() {
  return g_hide_hud.load(std::memory_order_relaxed);
}

bool CheatsEnabled() {
  return REXCVAR_GET(cheats);
}

// Switches the cheats; switching them off also ends the ones still running.
void SetCheats(bool on) {
  rex::cvar::SetFlagByName("cheats", on ? "true" : "false");
  if (!g_config.empty())
    rex::cvar::SaveConfigPatch(g_config, {{"cheats", on ? "true" : "false"}});
  if (!on) {
    players::SetGodMode(false);
    g_speed = 1.0f;
    g_gravity = 1.0f;
  }
  REXLOG_INFO("[debug-menu] cheats {}", on ? "on" : "off");
}

}  // namespace debug_menu

namespace game_ui {
namespace {

using debug_menu::g_gravity;
using debug_menu::g_hide_hud;
using debug_menu::g_speed;

constexpr const char* kEpisodes[] = {
    "LAND_OF_CHOCOLATE",   "BARTMAN_BEGINS",          "EIGHTY_BITES",
    "TREEHUGGER",          "MOB_RULES",               "CHEATER",
    "DOLPHINS",            "COLOSSAL_DONUT",          "SPRINGFIELD_STOOD_STILL",
    "BARGAIN_BIN",         "NEVERQUEST",              "GRAND_THEFT_SCRATCHY",
    "MEDAL_OF_HOMER",      "BIG_SUPER",               "RHYMES_WITH_COMPLAINING",
    "MEET_THY_PLAYER",     "SPR_HUB",                 "GAME_HUB"};
constexpr const char* kCostumes[] = {
    "SCORE_COSTUME_LISA_BIG_SUPER_LOCKED",  "SCORE_COSTUME_HOMER_BIG_SUPER_LOCKED",
    "SCORE_COSTUME_HOMER_MEDAL_OF_HOMER_LOCKED", "SCORE_COSTUME_BART_MEDAL_OF_HOMER_LOCKED",
    "SCORE_COSTUME_HOMER_NEVERQUEST_LOCKED", "SCORE_COSTUME_MARGE_NEVERQUEST_LOCKED",
    "SCORE_BIG_SUPER_COSTUME_LOCKED",        "SCORE_MEDAL_OF_HOMER_COSTUME_LOCKED",
    "SCORE_NEVERQUEST_COSTUME_LOCKED"};
constexpr const char* kCharacters[] = {"BART", "HOMER", "MARGE", "LISA"};

// What the Progress page shows.
struct Readout {
  const char* label;
  const char* score;
  const char* total;  // a score holding the total, or null
  float fixed_total;
};
constexpr Readout kReadouts[] = {
    {"Episodes finished", "SCORE_TOTAL_EPISODES_COMPLETE", nullptr, 16},
    {"Time challenges won", "SCORE_TOTAL_CHALLENGES_COMPLETE", nullptr, 16},
    {"Target times beaten", "SCORE_TOTAL_EPISODE_TARGET_TIMES_BEATEN", nullptr, 16},
    {"Collectibles", "SCORE_TOTAL_COLLECTIBLES_COLLECTED", "SCORE_MAX_COLLECTIBLES", 0},
    {"Video game cliches", "SCORE_CLICHE_COLLECTED", "SCORE_CLICHE_TOTAL", 0},
    {"Times defeated", "SCORE_TOTAL_PLAYER_DEATHS", nullptr, 0},
    {"Bart's power", "SCORE_POWER_MAX_BART", nullptr, 0},
    {"Homer's power", "SCORE_POWER_MAX_HOMER", nullptr, 0},
    {"Marge's power", "SCORE_POWER_MAX_MARGE", nullptr, 0},
    {"Lisa's power", "SCORE_POWER_MAX_LISA", nullptr, 0},
};

constexpr float kSpeeds[] = {0.1f, 0.25f, 0.5f, 0.75f, 1.0f, 1.25f, 1.5f, 2.0f, 3.0f};
constexpr std::pair<float, const char*> kGravities[] = {
    {0.17f, "Moon"}, {0.4f, "Floaty"}, {0.7f, "Light"}, {1.0f, "Normal"},
    {1.5f, "Heavy"}, {2.0f, "Crushing"}};
constexpr int kCounts[] = {1, 2, 3, 5, 10};
constexpr const char* kCategories[] = {"Vehicles", "People", "Enemies", "Props",
                                       "Toys",     "Weapons", "Pickups", "Collectibles"};

enum class Page {
  kMain,
  kPlayer,
  kCharacters,
  kPlayAsNpc,
  kTeleport,
  kSpooner,
  kSpoonerEntities,
  kSpoonerSwap,
  kAnimations,
  kRagdoll,
  kQuickLaunch,
  kSpawner,
  kSpawnCategory,
  kSourceGroups,
  kSourceGroup,
  kSourceArchive,
  kLibrary,
  kLibraryCategory,
  kLibraryAsset,
  kWorld,
  kCamera,
  kProgress,
  kAchievements,
  kEachAchievement,
  kSettings,
};
struct Opened {
  Page page;
  std::string arg;
  List::State state;
};
// A deque: rows open pages while the current one is still being built.
std::deque<Opened> g_stack;
std::string g_status;
float g_status_time = 0;
bool g_right = false;
int g_count = 0;  // index in kCounts
uint32_t g_spawned = 0;
int g_spooner_page = 0;
int g_spooner_swap_page = 0;
int g_npc_library_page = 0;
int g_anim_clip_page = 0;
int g_anim_library_page = 0;
int g_anim_npc_pick = 0;
std::shared_ptr<const simpsons::modkit::AssetSnapshot> g_assets;
std::vector<size_t> g_asset_matches;
std::string g_asset_filter, g_asset_search, g_asset_match_search;
int g_asset_page = 0, g_asset_initial = 0, g_asset_match_initial = -1;
int g_source_page = 0;
int g_library_template_page = 0;
struct LibrarySpawn {
  simpsons::modkit::AssetObject object;
  std::string label;
  int count = 1;
  uint32_t world = 0;
  uint64_t epoch = 0;
  std::chrono::steady_clock::time_point started;
};
std::optional<LibrarySpawn> g_library_spawn;
bool g_asset_edit = false;
std::array<int, 2> g_character_pick{-1, -1};
struct Spot {
  bool saved = false;
  std::array<float, 3> position{};
};
std::array<Spot, 3> g_spots;
float g_lock_all_confirm = 0;
uint32_t g_readout_frame = 0;
std::array<std::array<float, 2>, std::size(kReadouts)> g_readouts{};
float g_percent = 0, g_infinite_power = 0;
bool g_scores_ready = false;

void Status(std::string text) {
  g_status = std::move(text);
  g_status_time = 0;
}

void Push(Page page, std::string arg = {}) {
  g_stack.push_back({page, std::move(arg), {}});
}

// Copies of a record in rows in front of the controlled player, each clear of
// what was spawned before and is still there.
void Spawn(const spawner::Record& record, int count, bool vehicle, std::string label) {
  RunOnGameThread([record, count, vehicle, label = std::move(label)](PPCContext& ctx,
                                                                     uint8_t* base) {
    spawn_sources::RetainDebug(record.source_layer);
    const auto family = players::List(base);
    if (family.empty()) {
      Status("Load a level before spawning");
      return;
    }
    auto controlled = std::find_if(family.begin(), family.end(),
                                   [](const auto& player) { return player.input_player == 0; });
    if (controlled == family.end())
      controlled = std::find_if(family.begin(), family.end(),
                                [](const auto& player) { return player.input_player >= 0; });
    const auto& lead = controlled == family.end() ? family.front() : *controlled;
    std::array<float, 3> forward{0, 0, 1}, right{1, 0, 0};
    if (lead.frame) {
      for (int axis = 0; axis < 3; ++axis) {
        right[axis] = simpsons::LoadGuestFloat(base, lead.frame + 16 + axis * 4);
        forward[axis] = simpsons::LoadGuestFloat(base, lead.frame + 48 + axis * 4);
      }
    }
    // Room on the ground: a car takes about 4.5 by 2 metres.
    const float radius = vehicle ? 2.6f : 0.9f, gap = radius * 2 + 0.4f;
    const float first = vehicle ? 5.0f : 3.0f;
    auto placed = spawner::Spawned(ctx, base);
    const auto clear = [&](const std::array<float, 3>& at) {
      for (const auto& other : placed) {
        const float dx = at[0] - other.position[0], dz = at[2] - other.position[2];
        if (dx * dx + dz * dz < (radius + other.radius) * (radius + other.radius))
          return false;
      }
      return true;
    };
    // Rows of five, the middle first, then out to the sides; later rows
    // further out.
    constexpr int kAcross[] = {0, 1, -1, 2, -2};
    int place = 0, created = 0;
    for (int i = 0; i < count; ++i) {
      std::array<float, 3> position;
      do {
        position = lead.position;
        const float across = float(kAcross[place % 5]) * gap;
        const float ahead = first + float(place / 5) * gap;
        for (int axis = 0; axis < 3; ++axis)
          position[axis] += ahead * forward[axis] + across * right[axis];
      } while (!clear(position) && ++place < 40);
      ++place;
      if (spawner::Clone(ctx, base, record, position, radius)) {
        ++g_spawned;
        ++created;
        placed.push_back({position, radius});
      }
    }
    if (created)
      Status(fmt::format("Spawned {}: {}", created, label));
    else
      Status("Could not spawn " + label);
  });
}

std::string Speed(float value) {
  return fmt::format("{:.2f}x", value);
}

void MainPage(Frame& f, List& list) {
  const bool cheats = debug_menu::CheatsEnabled();
  if (list.Toggle("Cheats", cheats,
                  "Turns on everything marked with a padlock. Achievements still unlock with "
                  "cheats on; the achievements screen then says the game was modified.")) {
    debug_menu::SetCheats(!cheats);
    Status(!cheats ? "Cheats on" : "Cheats off");
  }
  if (list.Submenu("Player", "God mode, health, power, play as any NPC, ragdolls and teleports"))
    Push(Page::kPlayer);
  if (list.Submenu("NPC costume preview",
                   "Preview a loaded NPC costume; family controls and abilities stay unchanged",
                   true))
    Push(Page::kPlayAsNpc);
  if (list.Submenu("Spooner (map editor)",
                   "Use freecam to edit supported loaded entities; authored scripts remain active (F7)",
                   true))
    Push(Page::kSpooner);
  if (list.Submenu("Animations & poses",
                   "Play supported clips from a loaded character bank",
                   true))
    Push(Page::kAnimations);
  if (list.Submenu("Ragdolls & knockdowns",
                   "Test native family ragdolls, or configure the older tumbling prototype",
                   true))
    Push(Page::kRagdoll);
  if (list.Submenu("Spawner",
                   "Make more of what this level has loaded: cars, props, people, weapons and "
                   "toys, right in front of you",
                   true))
    Push(Page::kSpawner);
  if (list.Submenu("Game-wide library", "Browse assets from every episode and enabled mod, grouped by type"))
    Push(Page::kLibrary);
  if (list.Submenu("Quick launch level (dev)",
                   "Configure direct boot into any episode or hub, skipping menus and cutscenes"))
    Push(Page::kQuickLaunch);
  if (list.Submenu("World", "Game speed, gravity and the HUD"))
    Push(Page::kWorld);
  if (list.Submenu("Camera", "The free camera, Spooner mode and photo mode"))
    Push(Page::kCamera);
  if (list.Submenu("Progress", "What your save has done, and unlocks"))
    Push(Page::kProgress);
  if (list.Submenu("Achievements", "Look at them, switch the pop-ups, or unlock and lock them"))
    Push(Page::kAchievements);
  if (list.Submenu("Settings", "Where the menu sits, and how to open it"))
    Push(Page::kSettings);
  (void)f;
}

void PlayerPage(Frame& f, List& list) {
  const auto family = players::List(f.base);
  if (family.empty())
    list.Info("Nobody is in play right now");
  for (const auto& player : family)
    list.Info(players::CharacterName(player.character),
              fmt::format("{:.0f} / {:.0f}", player.hit_points, player.max_hit_points));
  const auto avatar = players::ActiveNpcAvatar();
  if (avatar.active)
    list.Info("Active NPC avatar", avatar.name);
  const bool god = players::GodMode();
  if (list.Toggle("God mode", god, "The family takes no damage", true)) {
    players::SetGodMode(!god);
    Status(!god ? "God mode on. Nothing can hurt you now." : "God mode off");
  }
  if (list.Action("Refill health", "Everyone back to full health", true)) {
    RunOnGameThread([](PPCContext& ctx, uint8_t* base) { players::RefillHealth(ctx, base); });
    Status("Health refilled");
  }
  const bool infinite = g_infinite_power >= 1;
  if (list.Toggle("Infinite power", infinite,
                  "Powers never run out. The game saves this with your progress.", true)) {
    RunOnGameThread([infinite](PPCContext& ctx, uint8_t* base) {
      scores::Set(ctx, base, "SCORE_POWER_INFINITE", infinite ? 0.0f : 1.0f);
    });
    Status(infinite ? "Infinite power off" : "Infinite power!");
  }
  if (list.Action("Power upgrade", "+500 power for the whole family, as the game's upgrades give",
                  true)) {
    RunOnGameThread([](PPCContext& ctx, uint8_t* base) {
      scores::Issue(ctx, base, "SCORE_EVENT_POWER_INCREASE");
    });
    Status("Power upgraded");
  }
  if (list.Submenu("NPC costume preview",
                   "Preview a loaded NPC costume; NPC abilities are not transferred",
                   true))
    Push(Page::kPlayAsNpc);
  if (avatar.active) {
    list.Info("NPC abilities", "Not supported by the costume preview");
    if (list.Action("Restore default Simpsons character",
                    "Return to your normal Simpsons family character and HUD portrait", true)) {
      RunOnGameThread([](PPCContext& ctx, uint8_t* base) {
        players::RestoreDefaultCharacter(ctx, base);
      });
      Status("Restored default Simpsons character");
    }
  }
  if (list.Submenu("Simpsons family & costumes", "Play as Homer, Bart, Marge or Lisa, costumes included", true))
    Push(Page::kCharacters);
  if (list.Submenu("Animations & poses", "Play any animation on your character or an NPC", true))
    Push(Page::kAnimations);
  if (list.Submenu("Ragdolls & knockdowns", "Native player physics tests and experimental knockdowns", true))
    Push(Page::kRagdoll);
  if (list.Action("Experimental player knockdown", "Launch the controlled character into an active ragdoll tumble", true)) {
    RunOnGameThread([](PPCContext& ctx, uint8_t* base) {
      Status(players::TriggerPlayerRagdoll(ctx, base)
          ? "Experimental knockdown started" : "Enable the prototype and load a controlled player");
    });
  }
  if (list.Submenu("Teleport", "Saved spots, and getting the family together", true))
    Push(Page::kTeleport);
}

void CharactersPage(Frame& f, List& list) {
  (void)f;
  const auto& choices = characters::Choices();
  for (int player = 0; player < 2; ++player) {
    int& pick = g_character_pick[size_t(player)];
    if (pick < 0) {
      const std::string current = characters::Current(player);
      pick = 0;
      for (size_t i = 0; i < choices.size(); ++i)
        if (current == choices[i].name)
          pick = int(i);
    }
    std::string help = "Left and right choose.";
    if (pick == 0) {
      const std::string level = characters::LevelCharacter(player);
      if (!level.empty())
        help = "The level's own: " + level + ". Left and right choose.";
    }
    if (const int change = list.Value(player ? "Player 2" : "Player 1", choices[size_t(pick)].label,
                                      help, true))
      pick = (pick + change + int(choices.size())) % int(choices.size());
  }
  if (list.Action("Change characters",
                  "Restarts from the last checkpoint with them. Levels are made for their own "
                  "characters: story triggers can wait for the right one, and \"The level's "
                  "character\" brings them back.",
                  true)) {
    characters::Apply(choices[size_t(g_character_pick[0])].name,
                      choices[size_t(g_character_pick[1])].name);
    Status("Characters change when the game goes on");
    Close();
  }
}

void TeleportPage(Frame& f, List& list) {
  (void)f;
  if (list.Action("Bring the family together", "Everyone next to the first player", true)) {
    RunOnGameThread([](PPCContext& ctx, uint8_t* base) {
      const auto list = players::List(base);
      for (size_t i = 1; i < list.size(); ++i) {
        auto position = list.front().position;
        position[0] += 1.5f * float(i);
        players::MoveTo(ctx, base, list[i], position);
      }
    });
    Status("Everyone is here");
  }
  if (FreecamActive()) {
    if (list.Action("To the free camera", "The family goes where the camera is", true)) {
      BringFamilyToCamera();
      Close();
    }
  }
  for (size_t i = 0; i < g_spots.size(); ++i) {
    list.Divider(fmt::format("Spot {}", i + 1));
    if (list.Action(fmt::format("Save spot {}", i + 1), "Remembers where the family stands",
                    true)) {
      RunOnGameThread([i](PPCContext&, uint8_t* base) {
        const auto list = players::List(base);
        if (list.empty())
          return;
        g_spots[i].position = list.front().position;
        g_spots[i].saved = true;
      });
      Status("Spot saved");
    }
    if (g_spots[i].saved) {
      if (list.Action(fmt::format("Go to spot {}", i + 1), "Whoosh!", true)) {
        RunOnGameThread([i](PPCContext& ctx, uint8_t* base) {
          const auto list = players::List(base);
          float offset = 0;
          for (const auto& player : list) {
            auto position = g_spots[i].position;
            position[0] += offset;
            players::MoveTo(ctx, base, player, position);
            offset += 1.5f;
          }
        });
        Status("Whoosh!");
      }
    }
  }
}

void SpawnerPage(Frame& f, List& list) {
  const auto& catalog = spawner::Catalog(f.base);
  if (const int change = list.Value("How many", std::to_string(kCounts[g_count]),
                                    "How many copies each spawn makes", true))
    g_count = std::clamp(g_count + change, 0, int(std::size(kCounts)) - 1);
  list.Info("NPC AI", "Native behavior; controller suspension is not supported yet");
  if (list.Submenu("Spooner (freecam map editor)",
                   "Select, move, rotate, clone, swap or delete any spawned or pre-existing map "
                   "entity",
                   true))
    Push(Page::kSpooner);
  if (list.Submenu("Entire game", "All episodes and mod assets, including resources that are not loaded here"))
    Push(Page::kLibrary);
  if (list.Submenu("Objects from other levels",
                   "Load an object's native source and dependencies without starting its level",
                   true))
    Push(Page::kSourceGroups);
  list.Divider("Resident object templates");
  bool any = false;
  std::vector<std::string> categories(std::begin(kCategories), std::end(kCategories));
  for (const auto& item : catalog)
    if (std::find(categories.begin(), categories.end(), item.category) == categories.end())
      categories.push_back(item.category);
  for (const auto& category : categories) {
    int count = 0;
    for (const auto& spawnable : catalog)
      count += spawnable.category == category;
    if (!count)
      continue;
    any = true;
    if (list.Submenu(fmt::format("{} ({})", category, count),
                     "Native objects and retained definitions, by name", true))
      Push(Page::kSpawnCategory, category);
  }
  if (!any)
    list.Info("Nothing to spawn here yet");
  list.Info("Spawned this session", std::to_string(g_spawned));
}

void SourceStatus(List& list) {
  const auto state = spawn_sources::Get();
  if (state.phase == spawn_sources::Phase::idle)
    return;
  list.Info("Object source", state.path);
  list.Info(state.message);
  if (state.files)
    list.Info("Streams loaded", fmt::format("{} / {}", state.loaded, state.files));
  if (state.definitions)
    list.Info("Retained definitions", std::to_string(state.definitions));
  if (!state.streams.empty())
    list.Info("Retained archives", std::to_string(state.streams.size()));
  if (state.owners > 1 || (state.owners && !state.debug_claimed))
    list.Info("Shared source", fmt::format("{} active owners", state.owners));
  if (!state.streams.empty() &&
      list.Submenu("Open spawner",
                   "The loaded source's object templates are now in their usual categories", true))
    Push(Page::kSpawner);
  const auto debug = spawn_sources::DebugStatus();
  const bool cancel = debug.phase == spawn_sources::Phase::preparing ||
                      debug.phase == spawn_sources::Phase::loading;
  if (state.phase != spawn_sources::Phase::releasing &&
      (state.phase != spawn_sources::Phase::failed || state.files) &&
      list.Action(cancel
                      ? "Cancel source load"
                      : state.owners > 1 || (state.owners && !state.debug_claimed)
                            ? "Remove the menu's copies"
                            : "Remove sources and their objects",
                  cancel ? "Cancel this addition while keeping earlier sources and objects"
                         : "Remove menu-owned copies and release its session claim; active mods keep their resources",
                  true))
    RunOnGameThread([cancel](PPCContext& ctx, uint8_t* base) {
      g_library_spawn.reset();
      if (cancel) spawn_sources::Cancel();
      else spawn_sources::Release(ctx, base);
    });
}

void SourceGroupsPage(Frame& f, List& list) {
  (void)f;
  SourceStatus(list);
  g_assets = spawner::Library();
  if (!g_assets->ready) {
    list.Info("Finding object sources",
              fmt::format("{} / {} files", g_assets->files, g_assets->total_files));
    return;
  }
  std::map<std::string, size_t> groups;
  for (const auto& stream : g_assets->object_streams)
    ++groups[stream.path.substr(0, stream.path.find('/'))];
  list.Divider("Installed object archives");
  for (const auto& [name, count] : groups)
    if (list.Submenu(fmt::format("{} ({})", name, count),
                     "Browse this episode's native object archives")) {
      g_source_page = 0;
      Push(Page::kSourceGroup, name);
    }
}

void SourceGroupPage(Frame& f, List& list, const std::string& group) {
  (void)f;
  if (!g_assets || !g_assets->ready)
    return;
  std::vector<const simpsons::modkit::ObjectStream*> streams;
  for (const auto& stream : g_assets->object_streams)
    if (stream.path.substr(0, stream.path.find('/')) == group)
      streams.push_back(&stream);
  constexpr size_t per_page = 40;
  const int pages = std::max(1, int((streams.size() + per_page - 1) / per_page));
  if (const auto change = list.Value("Page", fmt::format("{} / {}", g_source_page + 1, pages)))
    g_source_page = std::clamp(g_source_page + change, 0, pages - 1);
  list.Divider(group);
  const auto start = size_t(g_source_page) * per_page;
  for (auto i = start; i < std::min(start + per_page, streams.size()); ++i) {
    const auto& stream = *streams[i];
    auto name = stream.path.substr(group.size() + 1);
    if (name.ends_with(".str"))
      name.resize(name.size() - 4);
    if (list.Submenu(name, fmt::format("Native source data: {:.1f} MiB, before dependencies",
                                       stream.allocation / 1048576.0)))
      Push(Page::kSourceArchive, stream.path);
  }
}

void SourceArchivePage(Frame& f, List& list, const std::string& path) {
  (void)f;
  SourceStatus(list);
  const auto state = spawn_sources::Get();
  if (state.phase == spawn_sources::Phase::idle || state.path != path) {
    list.Divider("Source archive");
    list.Info(path);
    list.Info("Existing source objects stay loaded");
  }
  const bool retained = spawn_sources::IsReady(path);
  if ((retained || state.phase == spawn_sources::Phase::idle || state.phase == spawn_sources::Phase::failed ||
       state.phase == spawn_sources::Phase::ready) &&
      list.Action(retained ? "Use retained object templates" : "Load additional object templates",
                  "Load resources and retain definitions; the source level's actors and scripts "
                  "stay inactive",
                  true))
    RunOnGameThread([path](PPCContext&, uint8_t*) {
      if (!spawn_sources::Request(path))
        Status("The source loader is busy or the session is full");
    });
}

void LibraryPage(Frame& f, List& list) {
  (void)f;
  g_assets = spawner::Library();
  if (!g_assets->ready) {
    list.Info("Building game-wide library", fmt::format("{} / {} files", g_assets->files, g_assets->total_files));
    list.Info("You can close this menu while it scans");
    return;
  }
  list.Info("Across all episodes", fmt::format("{} assets", g_assets->assets.size()));
  for (const auto& [category, count] : g_assets->categories)
    if (list.Submenu(fmt::format("{} ({})", category, count), "Browse the complete installed game and mod library")) {
      g_asset_filter.clear();
      g_asset_search.clear();
      g_asset_page = g_asset_initial = 0;
      Push(Page::kLibraryCategory, category);
    }
  if (!g_assets->errors.empty()) list.Info("Files needing inspection", std::to_string(g_assets->errors.size()));
}

void LibraryCategoryPage(Frame& f, List& list, const std::string& category) {
  if (!g_assets || !g_assets->ready) { list.Info("Open the library again"); return; }
  if (list.Action(g_asset_edit ? "Finish typing" : "Search names and paths",
                  "Type with a keyboard; Return finishes. The letter and page controls also work with touch and controllers.",
                  false, g_asset_search.empty() ? "All names" : g_asset_search)) g_asset_edit = !g_asset_edit;
  if (f.input.find) g_asset_edit = true;
  if (g_asset_edit) {
    if (g_asset_search.size() + f.input.text.size() <= 160) g_asset_search += f.input.text;
    if (f.input.erase && !g_asset_search.empty()) {
      auto start = g_asset_search.size() - 1;
      while (start && (uint8_t(g_asset_search[start]) & 0xC0) == 0x80) --start;
      g_asset_search.erase(start);
    }
    if (f.input.enter) g_asset_edit = false;
  }
  TakeText(g_asset_edit, g_asset_edit);
  constexpr std::string_view letters = "*ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
  if (const auto change = list.Value("Names beginning with", g_asset_initial ? std::string(1, letters[g_asset_initial]) : "Any",
                                     "Filter without typing; arrows and taps change the letter"))
    g_asset_initial = std::clamp(g_asset_initial + change, 0, int(letters.size()) - 1);
  if (!g_asset_search.empty() && list.Action("Clear search")) { g_asset_search.clear(); g_asset_edit = false; }
  if (g_asset_filter != category || g_asset_match_search != g_asset_search || g_asset_match_initial != g_asset_initial) {
    g_asset_filter = category;
    g_asset_match_search = g_asset_search;
    g_asset_match_initial = g_asset_initial;
    g_asset_page = 0;
    g_asset_matches.clear();
    auto lower = [](std::string text) {
      for (char& c : text) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
      return text;
    };
    const auto needle = lower(g_asset_search);
    for (size_t i = 0; i < g_assets->assets.size(); ++i) {
      const auto& asset = g_assets->assets[i];
      if (asset.category != category) continue;
      if (g_asset_initial && (asset.name.empty() || lower(asset.name.substr(0, 1))[0] != char(std::tolower(letters[g_asset_initial])))) continue;
      if (!needle.empty() && lower(asset.name + " " + asset.native_name + " " + asset.source).find(needle) == std::string::npos) continue;
      g_asset_matches.push_back(i);
    }
  }
  constexpr size_t per_page = 40;
  const int pages = std::max(1, int((g_asset_matches.size() + per_page - 1) / per_page));
  if (const auto change = list.Value("Page", fmt::format("{} / {}", g_asset_page + 1, pages), "Arrows or taps browse the results"))
    g_asset_page = std::clamp(g_asset_page + change, 0, pages - 1);
  list.Divider(fmt::format("{} matches", g_asset_matches.size()));
  const size_t start = size_t(g_asset_page) * per_page;
  for (size_t row = start; row < std::min(start + per_page, g_asset_matches.size()); ++row) {
    const auto index = g_asset_matches[row];
    const auto& asset = g_assets->assets[index];
    if (list.Submenu(asset.name, asset.source.empty() ? asset.locations.front().stream : asset.source))
      Push(Page::kLibraryAsset, std::to_string(index));
  }
}

void LibraryAssetPage(Frame& f, List& list, const std::string& index) {
  if (!g_assets)
    return;
  const auto position = std::stoul(index);
  if (position >= g_assets->assets.size()) return;
  const auto& asset = g_assets->assets[position];
  list.Info(asset.category, asset.name);
  if (!asset.native_name.empty() && asset.native_name != asset.name)
    list.Info("Resource name", asset.native_name);
  if (!asset.source.empty()) list.Info("Source asset", asset.source);
  list.Info("Used in", fmt::format("{} stream locations", asset.locations.size()));
  for (size_t i = 0; i < std::min<size_t>(asset.locations.size(), 12); ++i) {
    const auto& location = asset.locations[i];
    list.Info(location.stream, location.container ? fmt::format("Entry {} + {:X}", location.entry, location.offset) : "Game file");
  }
  list.Divider("Spawning");
  list.Info("NPC AI", "Native behavior; controller suspension is not supported yet");
  bool available = false;
  const auto classes = spawner::PlaceableClasses(f.base);
  std::map<std::pair<std::string, uint32_t>, const spawner::Spawnable*> resident_templates;
  for (const auto* item : spawner::CurrentCatalog(f.base))
    resident_templates.emplace(std::make_pair(simpsons::modkit::ResourceId(item->record.source_guid),
                                               item->record.hash), item);
  std::vector<size_t> definitions;
  for (const auto candidate : asset.objects)
    if (candidate < g_assets->objects.size() && classes.contains(g_assets->objects[candidate].hash))
      definitions.push_back(candidate);
  if (definitions.empty() && !asset.locations.empty()) {
    const std::string& primary_stream = asset.locations.front().stream;
    for (size_t i = 0; i < g_assets->objects.size() && definitions.size() < 24; ++i) {
      const auto& obj = g_assets->objects[i];
      if (obj.stream == primary_stream && classes.contains(obj.hash))
        definitions.push_back(i);
    }
  }
  if (!definitions.empty()) {
    constexpr size_t page_size = 20;
    const int pages = std::max(1, int((definitions.size() + page_size - 1) / page_size));
    g_library_template_page = std::clamp(g_library_template_page, 0, pages - 1);
    if (pages > 1)
      if (const auto change = list.Value("Object variants",
                                         fmt::format("{} / {}", g_library_template_page + 1, pages)))
        g_library_template_page = std::clamp(g_library_template_page + change, 0, pages - 1);
    const auto first = size_t(g_library_template_page) * page_size;
    for (auto row = first; row < std::min(first + page_size, definitions.size()); ++row) {
      const auto& object = g_assets->objects[definitions[row]];
      const auto existing = resident_templates.find({object.guid, object.hash});
      const auto* resident = existing != resident_templates.end() ? existing->second : nullptr;
      if (resident && !object.model_resource.empty() && resident->model_resource != object.model_resource)
        resident = nullptr;
      const auto* model = g_assets->FindNative(object.model_resource);
      const auto label = model ? model->name : asset.name;
      const auto& type = classes.at(object.hash);
      const auto title = fmt::format("{} {}", resident ? "Spawn" : "Load and spawn", label);
      const auto help = fmt::format("{} — {}. Definition {}. Uses its original object data and "
                                     "native resource dependencies.", type, object.stream, object.guid);
      if (g_library_spawn) {
        list.Info("Preparing object", g_library_spawn->label);
        break;
      }
      if (list.Action(title, help, true, type)) {
        if (resident) {
          Spawn(resident->record, kCounts[g_count], resident->category == "Vehicles", label);
        } else {
          RunOnGameThread([object, label, count = kCounts[g_count]](PPCContext&, uint8_t* base) {
            if (g_library_spawn)
              return;
            if (!spawn_sources::Request(object.stream)) {
              Status("The source loader is busy or the session is full");
              return;
            }
            g_library_spawn = LibrarySpawn{object, label, count,
                simpsons::LoadGuestU32(base, 0x82D08BEC), spawner::WorldEpoch(),
                std::chrono::steady_clock::now()};
            Status("Loading " + label);
          });
        }
      }
    }
    available = true;
  }
  const auto& catalog = spawner::Catalog(f.base);
  for (const auto& spawnable : catalog) {
    if (!definitions.empty())
      break;
    // Source identity is decisive; equal names can describe unrelated models.
    if (spawnable.model_resource.empty() ||
        g_assets->FindNative(spawnable.model_resource) != &asset)
      continue;
    available = true;
    if (list.Action("Spawn " + spawnable.name, "Create this object in front of the player", true,
                    spawnable.record.name))
      Spawn(spawnable.record, kCounts[g_count], spawnable.category == "Vehicles", spawnable.name);
  }
  if (available) {
    if (const auto change = list.Value("How many", std::to_string(kCounts[g_count]),
                                       "How many copies each spawn makes", true))
      g_count = std::clamp(g_count + change, 0, int(std::size(kCounts)) - 1);
  } else {
    list.Info(asset.objects.empty() ? "No placeable object definition uses this resource"
                                    : "This resource's object classes need a native spawn adapter");
  }
  if (asset.category == "Characters" || asset.category == "Objects and characters" ||
      !definitions.empty()) {
    if (list.Action("Play as " + asset.name,
                    "Switch into this NPC as your playable character with animations, abilities "
                    "and corner HUD portrait",
                    true)) {
      RunOnGameThread([position, name = asset.name](PPCContext& ctx, uint8_t* base) {
        if (players::PlayAsLibraryNpc(ctx, base, position))
          Status("Playing as " + name);
        else
          Status("Loading NPC stream for " + name + "...");
      });
    }
  }
  if (asset.category == "Animations" || asset.category == "Characters" ||
      asset.category == "Objects and characters") {
    list.Divider("Animation Playback");
    constexpr const char* kTargetLabels[] = {
        "Current Character", "Spawn Selected NPC", "Original NPC Tied to Animation"};
    const int mode_idx = int(spawner::GetAnimTargetMode());
    if (const int change = list.Value(
            "Play target", kTargetLabels[std::clamp(mode_idx, 0, 2)],
            "Play on your current character, spawn an NPC, or spawn the original NPC tied to this "
            "animation",
            true)) {
      const int next = (mode_idx + change + 3) % 3;
      spawner::SetAnimTargetMode(static_cast<spawner::AnimTargetMode>(next));
    }
    const bool loop_anim = spawner::AnimLoopEnabled();
    if (list.Toggle("Loop animation", loop_anim, "Repeat the animation continuously on loop", true))
      spawner::SetAnimLoopEnabled(!loop_anim);
    if (list.Action("Play animation: " + asset.name,
                    "Plays this animation on the selected target (loads its source stream if "
                    "needed)",
                    true)) {
      RunOnGameThread([position, name = asset.name](PPCContext& ctx, uint8_t* base) {
        if (spawner::PlayAnimationFromLibrary(ctx, base, position, spawner::GetAnimTargetMode(),
                                              spawner::AnimLoopEnabled()))
          Status("Playing animation: " + name);
        else
          Status("Could not play animation " + name);
      });
    }
  }
  if (!definitions.empty()) {
    const auto source = spawn_sources::Get();
    if (source.phase == spawn_sources::Phase::ready)
      list.Info("Replacing a source removes its spawned objects");
    if (source.phase != spawn_sources::Phase::idle)
      list.Info("Source status", source.message);
  }
  if (list.Submenu("Open resident spawner",
                   "Object templates whose native source stream is loaded"))
    Push(Page::kSpawner);
}

void SpawnCategoryPage(Frame& f, List& list, const std::string& category) {
  const auto& catalog = spawner::Catalog(f.base);
  if (category == "People" || category == "Enemies")
    list.Info("NPC AI", "Native behavior; controller suspension is not supported yet");
  bool any = false;
  for (const auto& spawnable : catalog) {
    if (spawnable.category != category)
      continue;
    any = true;
    const std::string help = fmt::format(
        "Spawns {} in front of you. {} Spawned things go when their source "
        "stream unloads.",
        kCounts[g_count] == 1 ? std::string("one") : std::to_string(kCounts[g_count]),
        spawnable.seen ? fmt::format("{} live source objects.", spawnable.seen)
                       : "Retained native definition; no live source object needed.");
    if (list.Action(spawnable.name, help, true,
                    spawnable.seen ? fmt::format("x{}", spawnable.seen) : "Resident")) {
      Spawn(spawnable.record, kCounts[g_count], category == "Vehicles", spawnable.name);
    }
  }
  if (!any)
    list.Info("None of these are loaded here now");
}

void PlayAsNpcPage(Frame& f, List& list) {
  const auto avatar = players::ActiveNpcAvatar();
  if (avatar.active) {
    list.Info("Currently playing as", avatar.name);
    list.Info("NPC abilities", "Not supported by the costume preview");
    if (list.Action("Restore default Simpsons character",
                    "Switch back to your normal Simpsons character and HUD icon", true)) {
      RunOnGameThread([](PPCContext& ctx, uint8_t* base) {
        players::RestoreDefaultCharacter(ctx, base);
      });
      Status("Restored default Simpsons character");
    }
  } else {
    list.Info("Current character", "Default Simpsons Family");
  }

  list.Divider("Loaded NPCs & Enemies in This Level");
  const auto& catalog = spawner::Catalog(f.base);
  bool any_resident = false;
  for (const auto& spawnable : catalog) {
    if (spawnable.category != "People" && spawnable.category != "Enemies")
      continue;
    any_resident = true;
    if (list.Action("Play as " + spawnable.name,
                    "Experimental costume clone over the controlled family character",
                    true, spawnable.category)) {
      RunOnGameThread([spawnable](PPCContext& ctx, uint8_t* base) {
        if (players::PlayAsNpc(ctx, base, spawnable))
          Status("Now playing as " + spawnable.name);
        else
          Status("Could not switch to " + spawnable.name);
      });
    }
  }
  if (!any_resident)
    list.Info("No NPC templates resident yet — choose from the game-wide catalog below");

  list.Divider("All NPCs Across the Game Library");
  g_assets = spawner::Library();
  if (!g_assets || !g_assets->ready) {
    list.Info("Scanning game library...",
              g_assets ? fmt::format("{} / {} files", g_assets->files, g_assets->total_files) : "");
    return;
  }
  std::vector<size_t> npc_assets;
  for (size_t i = 0; i < g_assets->assets.size(); ++i) {
    const auto& a = g_assets->assets[i];
    if (a.category == "Characters" || a.category == "Objects and characters")
      npc_assets.push_back(i);
  }
  constexpr size_t per_page = 30;
  const int pages = std::max(1, int((npc_assets.size() + per_page - 1) / per_page));
  g_npc_library_page = std::clamp(g_npc_library_page, 0, pages - 1);
  if (pages > 1) {
    if (const int change = list.Value("Library NPC page",
                                      fmt::format("{} / {}", g_npc_library_page + 1, pages),
                                      "Browse all NPCs across every episode"))
      g_npc_library_page = std::clamp(g_npc_library_page + change, 0, pages - 1);
  }
  const size_t start = size_t(g_npc_library_page) * per_page;
  for (size_t row = start; row < std::min(start + per_page, npc_assets.size()); ++row) {
    const size_t idx = npc_assets[row];
    const auto& asset = g_assets->assets[idx];
    if (list.Action("Play as " + asset.name,
                    "Loads this NPC's stream if needed and switches you into them", true,
                    asset.native_name)) {
      RunOnGameThread([idx, name = asset.name](PPCContext& ctx, uint8_t* base) {
        if (players::PlayAsLibraryNpc(ctx, base, idx))
          Status("Playing as " + name);
        else
          Status("Loading NPC stream for " + name + "...");
      });
    }
  }
}

void SpoonerPage(Frame& f, List& list) {
  const bool enabled = spawner::SpoonerEnabled();
  if (list.Toggle("Spooner mode (freecam editor)", enabled,
                  "Fly with the free camera and aim at any map object or NPC to select, grab, "
                  "move, rotate, scale, clone, swap or delete it (F7)",
                  true)) {
    const bool next = !enabled;
    if (next)
      SetFreecam(true);
    spawner::SetSpoonerEnabled(next);
    Status(next ? "Spooner mode on — aim at any object and press F or A to grab"
                : "Spooner mode off");
  }
  const auto hovered = spawner::SpoonerHoveredInfo(f.ctx, f.base);
  const auto selected = spawner::SpoonerSelectedInfo(f.ctx, f.base);
  if (hovered)
    list.Info("Crosshair target",
              fmt::format("{} ({}, {:.1f}m)", hovered->name,
                          !hovered->category.empty() ? hovered->category : hovered->class_name,
                          hovered->distance_to_camera));
  else
    list.Info("Crosshair target", "None (aim camera at any object)");

  if (hovered && list.Action("Select crosshair target", "Lock selection onto the aimed entity", true)) {
    spawner::SpoonerSelectHovered();
    Status("Selected " + hovered->name);
  }
  if (list.Submenu("Browse all map entities",
                   "Inspect and select from every NPC, vehicle, prop and map object in the level",
                   true)) {
    g_spooner_page = 0;
    Push(Page::kSpoonerEntities);
  }

  const bool grabbing = spawner::SpoonerGrabbing();
  if (list.Toggle("Grab & move with camera", grabbing,
                  "Attaches the selected entity in front of the free camera as you fly (F / Pad A)",
                  true)) {
    RunOnGameThread([](PPCContext& ctx, uint8_t* base) {
      spawner::SpoonerToggleGrab(ctx, base);
    });
  }
  if (const int change = list.Value("Grab distance",
                                    fmt::format("{:.1f} m", spawner::SpoonerGrabDistance()),
                                    "How far in front of the free camera the grabbed object sits",
                                    true)) {
    spawner::AdjustSpoonerGrabDistance(float(change) * 0.5f);
  }

  if (selected) {
    list.Divider("Selected: " + selected->name);
    list.Info("Class & origin",
              fmt::format("{} ({})", selected->class_name,
                          selected->is_spawned_clone ? "Spawned clone" : "Map entity"));
    if (const int dx = list.Value("Position X", fmt::format("{:.2f}", selected->position[0]),
                                  "Nudge East / West by 0.5m", true)) {
      RunOnGameThread([dx](PPCContext& ctx, uint8_t* base) {
        spawner::SpoonerMoveSelected(ctx, base, {float(dx) * 0.5f, 0.0f, 0.0f});
      });
    }
    if (const int dy = list.Value("Position Y (Height)", fmt::format("{:.2f}", selected->position[1]),
                                  "Move Up / Down by 0.5m", true)) {
      RunOnGameThread([dy](PPCContext& ctx, uint8_t* base) {
        spawner::SpoonerMoveSelected(ctx, base, {0.0f, float(dy) * 0.5f, 0.0f});
      });
    }
    if (const int dz = list.Value("Position Z", fmt::format("{:.2f}", selected->position[2]),
                                  "Nudge North / South by 0.5m", true)) {
      RunOnGameThread([dz](PPCContext& ctx, uint8_t* base) {
        spawner::SpoonerMoveSelected(ctx, base, {0.0f, 0.0f, float(dz) * 0.5f});
      });
    }
    if (const int dyaw = list.Value("Rotate yaw", fmt::format("{:.0f} deg", selected->yaw_degrees),
                                    "Rotate left / right by 15 degrees (Z/X or D-Pad Left/Right)",
                                    true)) {
      RunOnGameThread([dyaw](PPCContext& ctx, uint8_t* base) {
        spawner::SpoonerRotateSelected(ctx, base, float(dyaw) * 15.0f);
      });
    }
    if (const int dscale = list.Value("Scale", fmt::format("{:.2f}x", selected->scale[0]),
                                      "Scale the model down or up by 10%", true)) {
      RunOnGameThread([dscale](PPCContext& ctx, uint8_t* base) {
        spawner::SpoonerScaleSelected(ctx, base, dscale > 0 ? 1.1f : 0.9f);
      });
    }
    list.Info("AI suspension", "Not supported for this controller");
    if (list.Submenu("Change / swap model...",
                     "Replace this entity in-place with another character, vehicle or prop", true)) {
      g_spooner_swap_page = 0;
      Push(Page::kSpoonerSwap);
    }
    if (list.Action("Clone selected entity",
                    "Spawn an identical copy right next to it (C / Pad Y)", true)) {
      RunOnGameThread([](PPCContext& ctx, uint8_t* base) {
        if (spawner::SpoonerCloneSelected(ctx, base))
          Status("Cloned selected entity");
      });
    }
    if (list.Action("Experimental knockdown on selected NPC",
                    "Apply experimental root motion to a supported character", true)) {
      const uint64_t life = selected->lifetime;
      RunOnGameThread([life](PPCContext& ctx, uint8_t* base) {
        Status(players::TriggerEntityRagdoll(ctx, base, life, {0.0f, 5.5f, 4.0f}, false)
            ? "Experimental knockdown started" : "Enable the prototype and select a supported character");
      });
    }
    if (list.Action("Reset rotation & scale", "Restore upright 1x transform", true)) {
      RunOnGameThread([](PPCContext& ctx, uint8_t* base) {
        spawner::SpoonerResetSelectedTransform(ctx, base);
      });
      Status("Reset transform");
    }
    if (list.Action("Delete / remove entity",
                    "Destroys spawned clones or removes pre-placed map objects (Del / Pad X)",
                    true)) {
      RunOnGameThread([](PPCContext& ctx, uint8_t* base) {
        if (spawner::SpoonerDeleteSelected(ctx, base))
          Status("Deleted selected entity");
      });
    }
  }

  const size_t removed = spawner::SpoonerRemovedMapEntityCount();
  if (removed > 0) {
    if (list.Action(fmt::format("Restore removed map entities ({})", removed),
                    "Bring back pre-placed map objects deleted during this session", true)) {
      RunOnGameThread([](PPCContext& ctx, uint8_t* base) {
        const size_t count = spawner::SpoonerRestoreRemovedMapEntities(ctx, base);
        Status(fmt::format("Restored {} map entities", count));
      });
    }
  }
}

void SpoonerEntitiesPage(Frame& f, List& list) {
  const auto entities = spawner::ListMapEntities(f.ctx, f.base);
  if (entities.empty()) {
    list.Info("No map entities found");
    return;
  }
  constexpr size_t per_page = 30;
  const int pages = std::max(1, int((entities.size() + per_page - 1) / per_page));
  g_spooner_page = std::clamp(g_spooner_page, 0, pages - 1);
  if (pages > 1) {
    if (const int change = list.Value("Page", fmt::format("{} / {}", g_spooner_page + 1, pages)))
      g_spooner_page = std::clamp(g_spooner_page + change, 0, pages - 1);
  }
  list.Divider(fmt::format("{} entities on map", entities.size()));
  const size_t start = size_t(g_spooner_page) * per_page;
  for (size_t i = start; i < std::min(start + per_page, entities.size()); ++i) {
    const auto& item = entities[i];
    const std::string badge = fmt::format(
        "{} • {:.1f}m", !item.category.empty() ? item.category : item.class_name,
        item.distance_to_camera);
    if (list.Action(item.name,
                    fmt::format("Select {} ({}) in Spooner", item.name, item.class_name), true,
                    badge)) {
      spawner::SetSpoonerSelectedLifetime(item.lifetime);
      Status("Selected " + item.name);
      if (g_stack.size() > 1)
        g_stack.pop_back();
    }
  }
}

void SpoonerSwapPage(Frame& f, List& list) {
  const auto& catalog = spawner::Catalog(f.base);
  if (catalog.empty()) {
    list.Info("No resident templates available to swap with");
    return;
  }
  constexpr size_t per_page = 30;
  const int pages = std::max(1, int((catalog.size() + per_page - 1) / per_page));
  g_spooner_swap_page = std::clamp(g_spooner_swap_page, 0, pages - 1);
  if (pages > 1) {
    if (const int change =
            list.Value("Page", fmt::format("{} / {}", g_spooner_swap_page + 1, pages)))
      g_spooner_swap_page = std::clamp(g_spooner_swap_page + change, 0, pages - 1);
  }
  const size_t start = size_t(g_spooner_swap_page) * per_page;
  for (size_t i = start; i < std::min(start + per_page, catalog.size()); ++i) {
    const auto& item = catalog[i];
    if (list.Action("Swap to " + item.name,
                    "Replace the selected map entity with this model at the same position", true,
                    item.category)) {
      RunOnGameThread([record = item.record, name = item.name](PPCContext& ctx, uint8_t* base) {
        if (spawner::SpoonerSwapSelected(ctx, base, record))
          Status("Swapped entity to " + name);
        else
          Status("Could not swap to " + name);
      });
      if (g_stack.size() > 1)
        g_stack.pop_back();
    }
  }
}

void AnimationsPage(Frame& f, List& list) {
  constexpr const char* kTargetLabels[] = {
      "Current Character", "Spawn Selected NPC", "Original NPC Tied to Animation"};
  const int mode_idx = int(spawner::GetAnimTargetMode());
  if (const int change = list.Value(
          "Play target", kTargetLabels[std::clamp(mode_idx, 0, 2)],
          "Play on your current character, spawn a chosen NPC, or spawn the original NPC tied to "
          "the animation",
          true)) {
    const int next = (mode_idx + change + 3) % 3;
    spawner::SetAnimTargetMode(static_cast<spawner::AnimTargetMode>(next));
  }
  const bool loop = spawner::AnimLoopEnabled();
  if (list.Toggle("Loop animation", loop, "Repeat played animations continuously on loop", true))
    spawner::SetAnimLoopEnabled(!loop);

  // Determine target entity for clip inspection
  uint32_t target_entity = 0;
  std::string target_label = "Controlled Player";
  if (const auto sel = spawner::SpoonerSelectedInfo(f.ctx, f.base); sel && sel->entity) {
    target_entity = sel->entity;
    target_label = "Spooner: " + sel->name;
  } else if (players::ActiveNpcAvatar().active && players::ActiveNpcAvatar().entity) {
    target_entity = players::ActiveNpcAvatar().entity;
    target_label = "NPC Avatar: " + players::ActiveNpcAvatar().name;
  } else {
    const auto fam = players::List(f.base);
    if (!fam.empty())
      target_entity = fam.front().entity;
  }

  if (target_entity) {
    const bool paused = spawner::IsEntityAnimationPaused(f.base, target_entity);
    if (list.Toggle("Pause animation (" + target_label + ")", paused,
                    "Freeze or resume the character's current animation pose", true)) {
      const auto lifetime = spawner::Observe(target_entity);
      RunOnGameThread([lifetime, paused](PPCContext& ctx, uint8_t* base) {
        const auto entity = spawner::Address(lifetime);
        if (!entity || !spawner::SetEntityAnimationPaused(ctx, base, entity, !paused))
          Status("This actor's animation cannot be paused");
      });
    }
    if (list.Action("Stop looping animation", "Return character to normal locomotion", true)) {
      const auto lifetime = spawner::Observe(target_entity);
      RunOnGameThread([lifetime](PPCContext& ctx, uint8_t* base) {
        const auto entity = spawner::Address(lifetime);
        Status(entity && spawner::StopEntityAnimation(ctx, base, entity)
            ? "Released custom animation" : "This actor's animation cannot be resumed");
      });
    }

    const auto clips = spawner::ListEntityAnimations(f.base, target_entity);
    if (!clips.empty()) {
      list.Divider(fmt::format("Loaded clips on {} ({})", target_label, clips.size()));
      constexpr size_t per_page = 20;
      const int pages = std::max(1, int((clips.size() + per_page - 1) / per_page));
      g_anim_clip_page = std::clamp(g_anim_clip_page, 0, pages - 1);
      if (pages > 1) {
        if (const int change = list.Value(
                "Clip page", fmt::format("{} / {}", g_anim_clip_page + 1, pages)))
          g_anim_clip_page = std::clamp(g_anim_clip_page + change, 0, pages - 1);
      }
      const size_t start = size_t(g_anim_clip_page) * per_page;
      for (size_t i = start; i < std::min(start + per_page, clips.size()); ++i) {
        const auto& clip = clips[i];
        if (list.Action("Play " + clip.label,
                        "Play this animation clip on the current target or a newly spawned NPC",
                        true, fmt::format("{:#010x}", clip.clip_key))) {
          const uint32_t key = clip.clip_key;
          const bool do_loop = spawner::AnimLoopEnabled();
          const auto lifetime = spawner::Observe(target_entity);
          RunOnGameThread([lifetime, key, do_loop, label = clip.label](PPCContext& ctx, uint8_t* base) {
            const auto entity = spawner::Address(lifetime);
            Status(entity && spawner::PlayEntityAnimation(ctx, base, entity, key, do_loop)
                ? "Playing " + label : "This actor cannot play that clip");
          });
        }
      }
    }
  }

  list.Divider("Game-Wide Animation Library");
  g_assets = spawner::Library();
  if (!g_assets || !g_assets->ready) {
    list.Info("Scanning animation library...");
    return;
  }
  std::vector<size_t> anim_assets;
  for (size_t i = 0; i < g_assets->assets.size(); ++i)
    if (g_assets->assets[i].category == "Animations")
      anim_assets.push_back(i);
  constexpr size_t per_page = 25;
  const int pages = std::max(1, int((anim_assets.size() + per_page - 1) / per_page));
  g_anim_library_page = std::clamp(g_anim_library_page, 0, pages - 1);
  if (pages > 1) {
    if (const int change = list.Value(
            "Library page", fmt::format("{} / {}", g_anim_library_page + 1, pages)))
      g_anim_library_page = std::clamp(g_anim_library_page + change, 0, pages - 1);
  }
  const size_t start = size_t(g_anim_library_page) * per_page;
  for (size_t row = start; row < std::min(start + per_page, anim_assets.size()); ++row) {
    const size_t idx = anim_assets[row];
    const auto& asset = g_assets->assets[idx];
    if (list.Action(asset.name,
                    "Play this library animation on the selected target (loads stream if needed)",
                    true, asset.locations.empty() ? "" : asset.locations.front().stream)) {
      RunOnGameThread([idx, name = asset.name](PPCContext& ctx, uint8_t* base) {
        if (spawner::PlayAnimationFromLibrary(ctx, base, idx, spawner::GetAnimTargetMode(),
                                              spawner::AnimLoopEnabled()))
          Status("Playing animation: " + name);
      });
    }
  }
}

void RagdollPage(Frame& f, List& list) {
  (void)f;
  list.Divider("Native family ragdoll test");
  list.Info("Two-second physics test", "Starting closes the menu. Normal animation and controls return afterward.");
  list.Info("Experimental", "Family characters only. Keep the other family member away: the same physics froze the game when they touched the body.");
  for (uint32_t slot = 0; slot < 2; ++slot) {
    if (list.Action(fmt::format("Test player {} ragdoll", slot + 1),
                    "Real jointed physics. Disable the older knockdown prototype first.", true)) {
      RunOnGameThread([slot](PPCContext& ctx, uint8_t* base) {
        if (native_ragdoll::StartPlayerPreview(ctx, base, slot)) {
          game_ui::Close();
          Status("Native ragdoll started");
        } else {
          Status("Load a living, controlled family character and disable the old prototype");
        }
      });
    }
  }
  if (list.Action("Restore native ragdolls", "End the tests and restore normal controls", true)) {
    RunOnGameThread([](PPCContext& ctx, uint8_t* base) {
      Status(native_ragdoll::StopPlayerPreviews(ctx, base)
          ? "Native ragdolls restored" : "Waiting for a safe physics update to restore");
    });
  }
  list.Divider("Older root-motion prototype");
  const bool enabled = REXCVAR_GET(ragdoll_mod_enabled);
  if (list.Toggle("Experimental root-motion knockdowns", enabled,
                  "Development prototype without jointed bodies or terrain collisions")) {
    players::SetRagdollModEnabled(!enabled);
    Status(!enabled ? "Experimental knockdowns enabled" : "Experimental knockdowns disabled");
  }
  const bool run_impact = REXCVAR_GET(ragdoll_run_impact);
  if (list.Toggle("Run-into-NPC knockdown", run_impact,
                  "Experimental sprint knockdowns on recognized NPCs")) {
    players::SetRagdollRunImpactEnabled(!run_impact);
  }
  const bool punch_impact = REXCVAR_GET(ragdoll_punch_impact);
  if (list.Toggle("Punch & attack ragdolls", punch_impact,
                  "Punching or hitting NPCs launches them into physical ragdoll reactions")) {
    players::SetRagdollPunchImpactEnabled(!punch_impact);
  }
  const bool push_downed = REXCVAR_GET(ragdoll_push_downed);
  if (list.Toggle("Push downed ragdolls with player body", push_downed,
                  "Walk or run into downed NPCs and defeated enemies to roll and push their bodies "
                  "until they get up or fade away")) {
    players::SetRagdollPushDownedEnabled(!push_downed);
  }
  const bool player_rag = REXCVAR_GET(ragdoll_player_enabled);
  if (list.Toggle("Allow player characters to ragdoll", player_rag,
                  "Allow experimental knockdown motion on family characters")) {
    players::SetRagdollPlayerEnabled(!player_rag);
  }
  constexpr float kForces[] = {0.5f, 0.75f, 1.0f, 1.25f, 1.5f, 2.0f, 3.0f};
  const float cur_force = float(REXCVAR_GET(ragdoll_force_scale));
  size_t force_idx = 2;
  for (size_t i = 0; i < std::size(kForces); ++i)
    if (std::abs(kForces[i] - cur_force) < 0.05f)
      force_idx = i;
  if (const int change = list.Value("Impact force multiplier",
                                    fmt::format("{:.2f}x", kForces[force_idx]),
                                    "Scales knockback and ragdoll launch velocity")) {
    force_idx = size_t(std::clamp(int(force_idx) + change, 0, int(std::size(kForces)) - 1));
    players::SetRagdollForceScale(kForces[force_idx]);
  }
  list.Divider("Triggers");
  if (list.Action("Experimental player knockdown",
                  "Apply experimental whole-character tumbling", true)) {
    RunOnGameThread([](PPCContext& ctx, uint8_t* base) {
      Status(players::TriggerPlayerRagdoll(ctx, base)
          ? "Experimental knockdown started" : "Enable the prototype and load a controlled player");
    });
  }
  if (list.Action("Ragdoll all nearby NPCs & enemies",
                  "Shockwave that knocks every nearby NPC and enemy into a ragdoll", true)) {
    RunOnGameThread([](PPCContext& ctx, uint8_t* base) {
      const size_t hit = players::TriggerNearbyRagdolls(ctx, base, 20.0f);
      Status(fmt::format("Ragdolled {} nearby characters", hit));
    });
  }
}

void QuickLaunchPage(Frame& f, List& list) {
  (void)f;
  const std::string current = REXCVAR_GET(quick_launch_level);
  list.Info("Startup quick-launch target", current.empty() ? "Off (Normal Title Screen)" : current);
  if (!current.empty()) {
    if (list.Action("Disable startup quick-launch",
                    "Return to normal boot (title screen and main menu)")) {
      rex::cvar::SetFlagByName("quick_launch_level", "");
      if (!debug_menu::g_config.empty())
        rex::cvar::SaveConfigPatch(debug_menu::g_config, {{"quick_launch_level", ""}});
      Status("Startup quick-launch disabled");
    }
  }
  list.Divider("Select Level for Direct Quick-Launch on Boot");
  struct LevelEntry {
    const char* id;
    const char* title;
  };
  constexpr LevelEntry kLevels[] = {
      {"SPR_HUB", "Springfield Hub (Overworld)"},
      {"GAME_HUB", "Simpsons House / Game Hub"},
      {"LAND_OF_CHOCOLATE", "01. The Land of Chocolate"},
      {"BARTMAN_BEGINS", "02. Bartman Begins"},
      {"EIGHTY_BITES", "03. Around the World in 80 Bites"},
      {"TREEHUGGER", "04. Lisa the Tree Hugger"},
      {"MOB_RULES", "05. Mob Rules"},
      {"CHEATER", "06. Enter the Cheatrix"},
      {"DOLPHINS", "07. The Day of the Dolphin"},
      {"COLOSSAL_DONUT", "08. Shadow of the Colossal Donut"},
      {"SPRINGFIELD_STOOD_STILL", "09. Invasion of the Yokel-Snatchers"},
      {"BARGAIN_BIN", "10. Bargain Bin"},
      {"NEVERQUEST", "11. NeverQuest"},
      {"GRAND_THEFT_SCRATCHY", "12. Grand Theft Scratchy"},
      {"MEDAL_OF_HOMER", "13. Medal of Homer"},
      {"BIG_SUPER", "14. Big Super Happy Fun Fun Game"},
      {"RHYMES_WITH_COMPLAINING", "15. Five Characters in Search of an Author"},
      {"MEET_THY_PLAYER", "16. Game Over"},
  };
  for (const auto& lvl : kLevels) {
    const bool active = current == lvl.id;
    if (list.Action(lvl.title,
                    "Set as the direct quick-launch level (skips logos, menus and cutscenes on boot)",
                    false, active ? "ACTIVE" : lvl.id)) {
      rex::cvar::SetFlagByName("quick_launch_level", lvl.id);
      if (!debug_menu::g_config.empty())
        rex::cvar::SaveConfigPatch(debug_menu::g_config, {{"quick_launch_level", lvl.id}});
      Status(std::string("Quick-launch set to ") + lvl.title);
    }
  }
}

void WorldPage(Frame& f, List& list) {
  (void)f;
  {
    const float speed = g_speed.load();
    size_t index = 4;
    for (size_t i = 0; i < std::size(kSpeeds); ++i)
      if (std::abs(kSpeeds[i] - speed) < 0.01f)
        index = i;
    if (const int change = list.Value("Game speed", Speed(speed),
                                      "Everything moves at this speed: Springfield, enemies, "
                                      "physics and cutscenes. Pausing still pauses.",
                                      true)) {
      index = size_t(std::clamp(int(index) + change, 0, int(std::size(kSpeeds)) - 1));
      g_speed = kSpeeds[index];
      Status("Game speed " + Speed(kSpeeds[index]));
    }
  }
  {
    const float gravity = g_gravity.load();
    size_t index = 3;
    for (size_t i = 0; i < std::size(kGravities); ++i)
      if (std::abs(kGravities[i].first - gravity) < 0.01f)
        index = i;
    if (const int change =
            list.Value("Gravity",
                       fmt::format("{} {:.2f}x", kGravities[index].second, kGravities[index].first),
                       "Everything physics moves follows it: debris, smashed props, thrown and "
                       "knocked-over things. The family's own jumps keep their weight.",
                       true)) {
      index = size_t(std::clamp(int(index) + change, 0, int(std::size(kGravities)) - 1));
      g_gravity = kGravities[index].first;
      Status(std::string("Gravity: ") + kGravities[index].second);
    }
  }
  const bool hide = g_hide_hud.load();
  if (list.Toggle("Hide the HUD", hide, "For clean screenshots and videos"))
    g_hide_hud = !hide;
  if (list.Submenu("Spooner (freecam map editor)",
                   "Select, move, rotate, clone, swap or delete any map object or NPC", true))
    Push(Page::kSpooner);
  if (list.Submenu("Ragdolls & knockdowns",
                   "Native player physics tests and experimental knockdowns", true))
    Push(Page::kRagdoll);
  if (list.Submenu("Quick launch level (dev)",
                   "Configure direct startup into any episode or hub"))
    Push(Page::kQuickLaunch);
}

void CameraPage(Frame& f, List& list) {
  (void)f;
  const bool freecam = FreecamActive();
  if (list.Toggle("Free camera", freecam,
                  "Fly anywhere: left stick moves, right stick looks, triggers go down and up, "
                  "bumpers zoom. L3 + R3 or F6 switch it too.")) {
    SetFreecam(!freecam);
    Status(!freecam ? "Free camera on" : "Free camera off");
  }
  const bool spooner = spawner::SpoonerEnabled();
  if (list.Toggle("Spooner mode (world editor)", spooner,
                  "Aim with the free camera crosshair to grab, move, clone or delete map entities "
                  "(F7)",
                  true)) {
    const bool next = !spooner;
    if (next)
      SetFreecam(true);
    spawner::SetSpoonerEnabled(next);
    Status(next ? "Spooner mode on" : "Spooner mode off");
  }
  if (list.Submenu("Open Spooner controls...",
                   "Full transform, model swap, clone and map entity browser", true))
    Push(Page::kSpooner);
  const bool photo = PhotoModeActive();
  if (list.Toggle("Photo mode", photo, "Freezes the world and hides the HUD"))
    SetPhotoMode(!photo);
  if (freecam && list.Action("Bring the family to the camera",
                             "B in the free camera does it too, with cheats on", true)) {
    BringFamilyToCamera();
    Close();
  }
}

void ProgressPage(Frame& f, List& list) {
  (void)f;
  if (!g_scores_ready) {
    list.Info("Load a save to see its progress");
  } else {
    list.Info("Game complete", fmt::format("{:.1f}%", g_percent));
    for (size_t i = 0; i < std::size(kReadouts); ++i)
      list.Info(kReadouts[i].label,
                g_readouts[i][1] > 0
                    ? fmt::format("{} / {}", int(g_readouts[i][0]), int(g_readouts[i][1]))
                    : fmt::format("{}", int(g_readouts[i][0])));
  }
  list.Divider("Unlocks");
  const auto set = [](auto scores, float value) {
    RunOnGameThread([scores, value](PPCContext& ctx, uint8_t* base) {
      for (const std::string& score : scores)
        scores::Set(ctx, base, score, value);
    });
  };
  if (list.Action("Unlock every episode",
                  "Open until the game restarts. They never count as finished, so they don't "
                  "award achievements.",
                  true)) {
    std::vector<std::string> scores;
    for (const char* episode : kEpisodes)
      scores.push_back(std::string("SCORE_") + episode + "_LOCKED");
    set(scores, 0);
    Status("Every episode is open");
  }
  if (list.Action("Unlock every time challenge", "Open until the game restarts", true)) {
    std::vector<std::string> scores;
    for (const char* episode : kEpisodes)
      scores.push_back(std::string("SCORE_") + episode + "_CHALLENGE_LOCKED");
    set(scores, 0);
    Status("Every time challenge is open");
  }
  if (list.Action("Unlock every costume", "Wear them at the bus stops", true)) {
    set(std::vector<std::string>(std::begin(kCostumes), std::end(kCostumes)), 0);
    Status("Every costume is unlocked");
  }
  if (list.Action("Everyone at the bus stops", "Pick any of the family at a bus stop", true)) {
    std::vector<std::string> scores;
    for (const char* character : kCharacters)
      scores.push_back(std::string("SCORE_") + character + "_BUS_STOP_SELECTABLE");
    set(scores, 1);
    Status("Every character can be picked at the bus stops");
  }
}

void AchievementsPage(Frame& f, List& list) {
  const auto all = achievements::List();
  uint32_t unlocked = 0, score = 0, total = 0;
  for (const auto& achievement : all) {
    unlocked += achievement.unlocked;
    total += achievement.gamerscore;
    if (achievement.unlocked)
      score += achievement.gamerscore;
  }
  list.Info("Unlocked", fmt::format("{} / {}", unlocked, all.size()));
  list.Info("Gamerscore", fmt::format("{} / {}G", score, total));
  if (list.Action("View achievements", "Every achievement, its picture's colours and your dates")) {
    Close();
    Open(Screen::kAchievements);
  }
  const bool notify = REXCVAR_GET(achievement_notifications);
  if (list.Toggle("Pop up when one unlocks", notify, "The notification in the corner")) {
    const char* value = notify ? "false" : "true";
    rex::cvar::SetFlagByName("achievement_notifications", value);
    if (!debug_menu::g_config.empty())
      rex::cvar::SaveConfigPatch(debug_menu::g_config, {{"achievement_notifications", value}});
  }
  if (list.Action("Unlock all", "Every achievement, marked as unlocked with cheats", true)) {
    for (const auto& achievement : all)
      achievements::Unlock(achievement.id, "debug menu");
    Status("Every achievement unlocked");
  }
  g_lock_all_confirm = std::max(0.0f, g_lock_all_confirm - f.dt);
  if (list.Action(g_lock_all_confirm > 0 ? "Lock all: press again to confirm" : "Lock all",
                  "Saves are not changed, and the game unlocks again what a loaded save has "
                  "earned.",
                  true)) {
    if (g_lock_all_confirm > 0) {
      achievements::LockAll();
      g_lock_all_confirm = 0;
      Status("Every achievement locked");
    } else {
      g_lock_all_confirm = 3;
    }
  }
  if (list.Submenu("Each achievement", "Unlock or lock them one at a time", true))
    Push(Page::kEachAchievement);
}

void EachAchievementPage(Frame& f, List& list) {
  (void)f;
  for (const auto& achievement : achievements::List()) {
    const std::string help = fmt::format(
        "{} ({}G)", achievement.unlocked || achievement.locked_description.empty()
                        ? achievement.description
                        : achievement.locked_description,
        achievement.gamerscore);
    if (list.Toggle(achievement.name, achievement.unlocked, help, true)) {
      if (achievement.unlocked)
        achievements::Lock(achievement.id);
      else
        achievements::Unlock(achievement.id, "debug menu");
    }
  }
}

void SettingsPage(Frame& f, List& list) {
  (void)f;
  if (list.Toggle("Menu on the right", g_right, "Which side of the screen the menu sits on"))
    g_right = !g_right;
  list.Info("Open or close", "RB + Left");
  list.Info("Keyboard", rex::cvar::GetFlagByName("bind_debug_menu"));
  list.Info("Pause menu", "X");
}

const char* Title(Page page) {
  switch (page) {
    case Page::kMain:
      return "MAIN MENU";
    case Page::kPlayer:
      return "PLAYER";
    case Page::kCharacters:
      return "CHARACTERS";
    case Page::kPlayAsNpc:
      return "PLAY AS ANY NPC";
    case Page::kTeleport:
      return "TELEPORT";
    case Page::kSpooner:
      return "SPOONER MAP EDITOR";
    case Page::kSpoonerEntities:
      return "MAP ENTITIES";
    case Page::kSpoonerSwap:
      return "SWAP ENTITY MODEL";
    case Page::kAnimations:
      return "ANIMATIONS & POSES";
    case Page::kRagdoll:
      return "EUPHORIA RAGDOLL MOD";
    case Page::kQuickLaunch:
      return "QUICK LAUNCH LEVEL";
    case Page::kSpawner:
      return "SPAWNER";
    case Page::kSpawnCategory:
      return "SPAWN";
    case Page::kSourceGroups:
      return "OBJECT SOURCES";
    case Page::kSourceGroup:
      return "EPISODE SOURCES";
    case Page::kSourceArchive:
      return "LOAD OBJECT SOURCE";
    case Page::kLibrary:
      return "ENTIRE GAME";
    case Page::kLibraryCategory:
      return "ASSET LIBRARY";
    case Page::kLibraryAsset:
      return "ASSET DETAILS";
    case Page::kWorld:
      return "WORLD";
    case Page::kCamera:
      return "CAMERA";
    case Page::kProgress:
      return "PROGRESS";
    case Page::kAchievements:
      return "ACHIEVEMENTS";
    case Page::kEachAchievement:
      return "EACH ACHIEVEMENT";
    case Page::kSettings:
      return "SETTINGS";
  }
  return "";
}

// Scores shown on the Player and Progress pages, read now and then.
void ReadScores(Frame& f) {
  if (++g_readout_frame % 20 != 1)
    return;
  g_scores_ready = scores::Ready(f.base);
  if (!g_scores_ready)
    return;
  g_infinite_power = scores::Get(f.ctx, f.base, "SCORE_POWER_INFINITE");
  if (g_stack.empty() || g_stack.back().page != Page::kProgress)
    return;
  for (size_t i = 0; i < std::size(kReadouts); ++i) {
    g_readouts[i][0] = scores::Get(f.ctx, f.base, kReadouts[i].score);
    g_readouts[i][1] =
        kReadouts[i].total ? scores::Get(f.ctx, f.base, kReadouts[i].total) : kReadouts[i].fixed_total;
  }
  const float scale = scores::Get(f.ctx, f.base, "SCORE_GAME_PERCENT_COMPLETE_SCALE");
  g_percent = scale > 0 ? 100.0f * scores::Get(f.ctx, f.base, "SCORE_GAME_PERCENT_COMPLETE") / scale
                        : 0;
}

}  // namespace

void TickLibrarySpawn(PPCContext& ctx, uint8_t* base) {
  (void)ctx;
  if (!g_library_spawn)
    return;
  if (!debug_menu::CheatsEnabled() ||
      spawner::WorldEpoch() != g_library_spawn->epoch ||
      simpsons::LoadGuestU32(base, 0x82D08BEC) != g_library_spawn->world) {
    g_library_spawn.reset();
    Status("Library spawn canceled: the level or cheats changed");
    return;
  }
  const auto state = spawn_sources::DebugStatus();
  if (state.phase == spawn_sources::Phase::failed ||
      std::chrono::steady_clock::now() - g_library_spawn->started > std::chrono::seconds(90)) {
    g_library_spawn.reset();
    Status(state.phase == spawn_sources::Phase::failed ? state.message : "Object source load timed out");
    return;
  }
  if (state.phase != spawn_sources::Phase::ready)
    return;
  const auto request = *g_library_spawn;
  if (!spawn_sources::IsReady(request.object.stream))
    return;
  g_library_spawn.reset();
  const auto item = spawner::FindTemplate(base, request.object.guid, request.object.hash);
  if (!item || (!request.object.model_resource.empty() &&
                request.object.model_resource != item->model_resource)) {
    Status("The selected object's definition is not available after loading");
    return;
  }
  Spawn(item->record, request.count, item->category == "Vehicles", request.label);
}

bool DrawDebugMenu(Frame& f, bool appearing) {
  spawner::WarmCatalog(f.base);
  if (appearing) {
    if (g_stack.empty())
      Push(Page::kMain);
    g_character_pick = {-1, -1};
    REXLOG_INFO("[debug-menu] opened");
  }
  if (g_stack.empty())
    Push(Page::kMain);
  ReadScores(f);
  g_status_time += f.dt;
  Opened& current = g_stack.back();
  const Page page = current.page;
  if (page != Page::kLibraryCategory) { g_asset_edit = false; TakeText(false); }
  const std::string arg = current.arg;
  const std::string title = page == Page::kSpawnCategory ? fmt::format("SPAWN: {}", arg)
                                                         : std::string(Title(page));
  List list(f, current.state, title, debug_menu::CheatsEnabled());
  switch (page) {
    case Page::kMain:
      MainPage(f, list);
      break;
    case Page::kPlayer:
      PlayerPage(f, list);
      break;
    case Page::kCharacters:
      CharactersPage(f, list);
      break;
    case Page::kPlayAsNpc:
      PlayAsNpcPage(f, list);
      break;
    case Page::kTeleport:
      TeleportPage(f, list);
      break;
    case Page::kSpooner:
      SpoonerPage(f, list);
      break;
    case Page::kSpoonerEntities:
      SpoonerEntitiesPage(f, list);
      break;
    case Page::kSpoonerSwap:
      SpoonerSwapPage(f, list);
      break;
    case Page::kAnimations:
      AnimationsPage(f, list);
      break;
    case Page::kRagdoll:
      RagdollPage(f, list);
      break;
    case Page::kQuickLaunch:
      QuickLaunchPage(f, list);
      break;
    case Page::kSpawner:
      SpawnerPage(f, list);
      break;
    case Page::kSpawnCategory:
      SpawnCategoryPage(f, list, arg);
      break;
    case Page::kSourceGroups:
      SourceGroupsPage(f, list);
      break;
    case Page::kSourceGroup:
      SourceGroupPage(f, list, arg);
      break;
    case Page::kSourceArchive:
      SourceArchivePage(f, list, arg);
      break;
    case Page::kLibrary:
      LibraryPage(f, list);
      break;
    case Page::kLibraryCategory:
      LibraryCategoryPage(f, list, arg);
      break;
    case Page::kLibraryAsset:
      LibraryAssetPage(f, list, arg);
      break;
    case Page::kWorld:
      WorldPage(f, list);
      break;
    case Page::kCamera:
      CameraPage(f, list);
      break;
    case Page::kProgress:
      ProgressPage(f, list);
      break;
    case Page::kAchievements:
      AchievementsPage(f, list);
      break;
    case Page::kEachAchievement:
      EachAchievementPage(f, list);
      break;
    case Page::kSettings:
      SettingsPage(f, list);
      break;
  }
  if (list.locked_use())
    Status("That needs cheats: switch Cheats on in the main menu");
  const float width = 470 * f.s;
  const float x = g_right ? f.c.width() - width - 40 * f.s : 40 * f.s;
  const float status_alpha = g_status_time < 3.0f ? std::min(1.0f, (3.0f - g_status_time) * 2) : 0;
  list.AllowBack(g_stack.size() > 1);
  list.Draw(x, 40 * f.s, width, g_status, status_alpha);
  if (list.close())
    return false;
  if (list.back()) {
    if (g_asset_edit) { g_asset_edit = false; TakeText(false); return true; }
    if (g_stack.size() > 1) {
      // The page drawn is the one below anything its rows opened.
      while (g_stack.size() > 1 && &g_stack.back() != &current)
        g_stack.pop_back();
      g_stack.pop_back();
      Sound("ui_back_b_button");
    } else {
      return false;
    }
  }
  return Current() == Screen::kDebugMenu;
}

}  // namespace game_ui
