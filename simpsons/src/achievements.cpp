// The game's Xbox 360 achievements.
//
// The game awards them through its ScoreKeeper. In gamedata/simpsons_scores.lua
// each achievement is an unlock watcher on a score,
// score:AddUnlockWatcher(ACHIEVEMENT_AWARD, threshold, ACHIEVEMENT_X), and when
// the score reaches the threshold the game writes the achievement with
// XUserWriteAchievements (sub_824316A8, XGI message 0xB0008), which the runtime
// passes to Unlock. Press START to Play is written by the front end.
//
// Watchers fire when a score changes, never when a save is loaded, so progress
// from before achievements were kept would not count. Twice a second the
// loaded save's scores are read through the game's own GetScore (sub_828A3100,
// behind the Lua binding sk:GetScore) and compared with the same thresholds.
// That also keeps the progress shown for the counted achievements.
//
// Unlocks are kept for the profile next to its save slots, in
// <user data>/<XUID>/45410809/achievements.txt, so the launcher's save backups
// include them. Names, descriptions, gamerscore and pictures come from the
// title data in the player's own default.xex.

#include "achievements.h"
#include "game_thread.h"
#include "debug_menu.h"
#include "overlay.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <sstream>

#include <fmt/format.h>

#include <rex/kernel/xam/achievements.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/flags.h>
#include <rex/system/kernel_state.h>
#include <rex/system/util/xdbf_utils.h>
#include <rex/system/xam/user_profile.h>

namespace achievements {
namespace {

struct Rule {
  uint32_t id;
  const char* score;
  float threshold;
};

// The unlock watchers of simpsons_scores.lua, by achievement id.
constexpr Rule kRules[] = {
    // EpisodeHelper: SCORE_<episode>_COMPLETE, then the time challenge.
    {33, "SCORE_LAND_OF_CHOCOLATE_COMPLETE", 1},
    {34, "SCORE_BARTMAN_BEGINS_COMPLETE", 1},
    {35, "SCORE_EIGHTY_BITES_COMPLETE", 1},
    {36, "SCORE_TREEHUGGER_COMPLETE", 1},
    {37, "SCORE_MOB_RULES_COMPLETE", 1},
    {38, "SCORE_CHEATER_COMPLETE", 1},
    {39, "SCORE_SPRINGFIELD_STOOD_STILL_COMPLETE", 1},
    {40, "SCORE_DOLPHINS_COMPLETE", 1},
    {41, "SCORE_COLOSSAL_DONUT_COMPLETE", 1},
    {42, "SCORE_BARGAIN_BIN_COMPLETE", 1},
    {43, "SCORE_MEDAL_OF_HOMER_COMPLETE", 1},
    {44, "SCORE_NEVERQUEST_COMPLETE", 1},
    {45, "SCORE_GRAND_THEFT_SCRATCHY_COMPLETE", 1},
    {46, "SCORE_BIG_SUPER_COMPLETE", 1},
    {47, "SCORE_RHYMES_WITH_COMPLAINING_COMPLETE", 1},
    {48, "SCORE_MEET_THY_PLAYER_COMPLETE", 1},
    {61, "SCORE_LAND_OF_CHOCOLATE_CHALLENGE_COMPLETE", 1},
    {62, "SCORE_BARTMAN_BEGINS_CHALLENGE_COMPLETE", 1},
    {63, "SCORE_EIGHTY_BITES_CHALLENGE_COMPLETE", 1},
    {64, "SCORE_TREEHUGGER_CHALLENGE_COMPLETE", 1},
    {65, "SCORE_MOB_RULES_CHALLENGE_COMPLETE", 1},
    {66, "SCORE_CHEATER_CHALLENGE_COMPLETE", 1},
    {67, "SCORE_SPRINGFIELD_STOOD_STILL_CHALLENGE_COMPLETE", 1},
    {68, "SCORE_COLOSSAL_DONUT_CHALLENGE_COMPLETE", 1},
    {69, "SCORE_DOLPHINS_CHALLENGE_COMPLETE", 1},
    {70, "SCORE_BARGAIN_BIN_CHALLENGE_COMPLETE", 1},
    {71, "SCORE_NEVERQUEST_CHALLENGE_COMPLETE", 1},
    {73, "SCORE_MEDAL_OF_HOMER_CHALLENGE_COMPLETE", 1},
    {74, "SCORE_BIG_SUPER_CHALLENGE_COMPLETE", 1},
    {75, "SCORE_GRAND_THEFT_SCRATCHY_CHALLENGE_COMPLETE", 1},
    {76, "SCORE_RHYMES_WITH_COMPLAINING_CHALLENGE_COMPLETE", 1},
    {77, "SCORE_MEET_THY_PLAYER_CHALLENGE_COMPLETE", 1},
    // SetupGeneric, SetupSprHub and SetupMeetThyPlayer.
    {49, "SCORE_SPR_HUB_TROPHIES_AWARDED", 4},
    {50, "SCORE_TOTAL_BEERCAP_COLLECTED", 154},
    {51, "SCORE_TOTAL_PRODUCT_COLLECTED", 110},
    {52, "SCORE_TOTAL_DOLL_COLLECTED", 124},
    {54, "SCORE_TOTAL_KOUPON_COLLECTED", 151},
    {55, "SCORE_CLICHE_31", 1},
    {56, "SCORE_TASKS_COMPLETED", 3},
    {57, "SCORE_TOTAL_EPISODE_TARGET_TIMES_BEATEN", 16},
    {58, "SCORE_MEET_THY_PLAYER_GOD_BATTLE_PERFECT", 1},
    {59, "SCORE_TOTAL_PLAYER_DEATHS", 10},
    {60, "SCORE_ALL_CHALLENGES_COMPLETE", 1},
};
// Any of these scores means a save with progress is loaded, so START was
// pressed at some point on that profile.
constexpr uint32_t kPressStart = 32;

struct State {
  rex::Runtime* runtime = nullptr;
  std::mutex mutex;
  bool loaded = false;
  std::vector<Achievement> list;
  std::filesystem::path path;
  std::deque<uint32_t> notifications;
  uint32_t tick = 0;
};
State* g_state = nullptr;
std::atomic<uint32_t> g_generation{0};

int64_t Now() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

Achievement* Find(uint32_t id) {
  for (auto& achievement : g_state->list) {
    if (achievement.id == id)
      return &achievement;
  }
  return nullptr;
}

// Next to the profile's save slots: <user data>/<XUID>/<title id>/, where the
// content manager keeps the 00000001 (saved game) folder.
std::filesystem::path StorePath() {
  auto* kernel_state = g_state->runtime->kernel_state();
  if (!kernel_state || !kernel_state->user_profile() || g_state->runtime->user_data_root().empty())
    return {};
  return g_state->runtime->user_data_root() /
         fmt::format("{:016X}", kernel_state->user_profile()->xuid()) /
         fmt::format("{:08X}", kernel_state->title_id()) / "achievements.txt";
}

void Write() {
  if (g_state->path.empty())
    return;
  std::ostringstream text;
  text << "# Achievements earned in The Simpsons Game.\n"
          "# achievement id, unlock time (Unix seconds, 0 if unknown), 1 if cheats were on\n";
  for (const auto& achievement : g_state->list) {
    if (achievement.unlocked)
      text << achievement.id << ' ' << achievement.unlock_time << ' '
           << (achievement.with_cheats ? 1 : 0) << '\n';
  }
  std::error_code error;
  std::filesystem::create_directories(g_state->path.parent_path(), error);
  auto temporary = g_state->path;
  temporary += ".tmp";
  {
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    file << text.str();
    if (!file.flush()) {
      REXLOG_ERROR("[achievements] Could not write {}", temporary.string());
      return;
    }
  }
  std::filesystem::rename(temporary, g_state->path, error);
  if (error)
    REXLOG_ERROR("[achievements] Could not save {}: {}", g_state->path.string(), error.message());
}

// The title data and the saved unlocks, once the game is running.
void Load() {
  if (g_state->loaded)
    return;
  auto* kernel_state = g_state->runtime->kernel_state();
  if (!kernel_state)
    return;
  g_state->loaded = true;
  const auto db = kernel_state->title_xdbf();
  if (!db.is_valid()) {
    REXLOG_WARN("[achievements] The game has no achievement data");
    return;
  }
  const auto language =
      db.GetExistingLanguage(static_cast<rex::system::XLanguage>(REXCVAR_GET(user_language)));
  for (const auto& entry : db.GetAchievements()) {
    Achievement achievement;
    achievement.id = entry.id;
    achievement.name = db.GetStringTableEntry(language, entry.label_id);
    achievement.description = db.GetStringTableEntry(language, entry.description_id);
    achievement.locked_description = db.GetStringTableEntry(language, entry.unachieved_id);
    achievement.gamerscore = entry.gamerscore;
    const auto image = db.GetEntry(rex::system::util::XdbfSection::kImage, entry.image_id);
    if (image)
      achievement.icon_png.assign(image.buffer, image.buffer + image.size);
    for (const auto& rule : kRules) {
      if (rule.id == achievement.id && rule.threshold > 1)
        achievement.goal = rule.threshold;
    }
    g_state->list.push_back(std::move(achievement));
  }

  g_state->path = StorePath();
  std::ifstream file(g_state->path);
  size_t restored = 0;
  for (std::string line; std::getline(file, line);) {
    if (line.empty() || line[0] == '#')
      continue;
    unsigned id = 0, cheats = 0;
    long long time = 0;
    if (std::sscanf(line.c_str(), "%u %lld %u", &id, &time, &cheats) < 1)
      continue;
    if (auto* achievement = Find(id); achievement && !achievement->unlocked) {
      achievement->unlocked = true;
      achievement->unlock_time = time;
      achievement->with_cheats = cheats != 0;
      ++restored;
    }
  }
  REXLOG_INFO("[achievements] {} achievements, {} unlocked, kept in {}", g_state->list.size(),
              restored, g_state->path.string());
  g_generation.fetch_add(1);
}

bool UnlockLocked(uint32_t id, std::string_view reason) {
  Load();
  auto* achievement = Find(id);
  if (!achievement || achievement->unlocked)
    return false;
  achievement->unlocked = true;
  achievement->unlock_time = Now();
  achievement->with_cheats = debug_menu::CheatsEnabled();
  g_state->notifications.push_back(id);
  REXLOG_INFO("[achievements] Unlocked {} \"{}\" ({}G): {}", id, achievement->name,
              achievement->gamerscore, reason);
  Write();
  g_generation.fetch_add(1);
  overlay::Wake();
  return true;
}

}  // namespace

void Init(rex::Runtime* runtime) {
  g_state = new State;
  g_state->runtime = runtime;
  rex::kernel::xam::AchievementHost host;
  host.award = [](uint32_t user_index, uint32_t id) {
    Unlock(id, "awarded by the game");
    (void)user_index;
  };
  host.unlocked = [](uint32_t, uint32_t id, uint64_t* filetime) {
    if (!g_state)
      return false;
    std::lock_guard lock(g_state->mutex);
    Load();
    const auto* achievement = Find(id);
    if (!achievement || !achievement->unlocked)
      return false;
    // Unix seconds to a FILETIME.
    *filetime = achievement->unlock_time > 0
                    ? (uint64_t(achievement->unlock_time) + 11644473600ull) * 10000000ull
                    : 0;
    return true;
  };
  rex::kernel::xam::SetAchievementHost(std::move(host));
}

void Shutdown() {
  // Guest threads may still award achievements while the runtime stops.
  rex::kernel::xam::SetAchievementHost({});
}

void Tick(PPCContext& ctx, uint8_t* base) {
  if (!g_state || ++g_state->tick % 30 != 0 || !scores::Ready(base))
    return;
  std::array<float, std::size(kRules)> values{};
  for (size_t i = 0; i < std::size(kRules); ++i)
    values[i] = scores::Get(ctx, base, kRules[i].score);

  std::lock_guard lock(g_state->mutex);
  Load();
  bool progress = false, changed = false;
  for (size_t i = 0; i < std::size(kRules); ++i) {
    const auto& rule = kRules[i];
    // Scores are whole numbers stored as floats; a time can be FLT_MAX.
    if (!(values[i] >= 0) || values[i] > 1e7f)
      continue;
    progress |= values[i] > 0;
    if (auto* achievement = Find(rule.id)) {
      const float value = std::min(values[i], rule.threshold);
      changed |= achievement->progress != value;
      achievement->progress = value;
    }
    if (values[i] >= rule.threshold)
      UnlockLocked(rule.id, "earned by the loaded save");
  }
  if (changed)
    g_generation.fetch_add(1);
  if (progress)
    UnlockLocked(kPressStart, "a save with progress is loaded");
}

std::vector<Achievement> List() {
  if (!g_state)
    return {};
  std::lock_guard lock(g_state->mutex);
  Load();
  return g_state->list;
}

uint32_t Generation() {
  return g_generation.load();
}

bool Unlock(uint32_t id, std::string_view reason) {
  if (!g_state)
    return false;
  std::lock_guard lock(g_state->mutex);
  return UnlockLocked(id, reason);
}

bool Lock(uint32_t id) {
  if (!g_state)
    return false;
  std::lock_guard lock(g_state->mutex);
  Load();
  auto* achievement = Find(id);
  if (!achievement || !achievement->unlocked)
    return false;
  achievement->unlocked = false;
  achievement->unlock_time = 0;
  achievement->with_cheats = false;
  REXLOG_INFO("[achievements] Locked {} \"{}\"", id, achievement->name);
  Write();
  g_generation.fetch_add(1);
  return true;
}

void LockAll() {
  if (!g_state)
    return;
  std::lock_guard lock(g_state->mutex);
  Load();
  for (auto& achievement : g_state->list) {
    achievement.unlocked = false;
    achievement.unlock_time = 0;
    achievement.with_cheats = false;
  }
  g_state->notifications.clear();
  REXLOG_INFO("[achievements] Locked every achievement");
  Write();
  g_generation.fetch_add(1);
}

std::vector<uint32_t> TakeNotifications() {
  if (!g_state)
    return {};
  std::lock_guard lock(g_state->mutex);
  std::vector<uint32_t> ids(g_state->notifications.begin(), g_state->notifications.end());
  g_state->notifications.clear();
  return ids;
}

}  // namespace achievements
