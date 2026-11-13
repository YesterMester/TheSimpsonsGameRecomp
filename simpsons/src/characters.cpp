// Other characters for players 1 and 2, from frankyfife's fork (its in-game
// character menu, without the menu: the mod menu chooses).
//
// A level spawns its playable characters from four player slots. Every time
// the level loads, including a restart from a checkpoint, sub_8289E428
// resolves the slots' overrides into one character template GUID per slot at
// GameFlowManager (0x82D08BA8) +0x354 + slot * 16, whose first word is the
// template's stream ID in the character StreamTOC. After it, the chosen
// template's GUID replaces the resolved one; the game's own lists are left
// alone, so the level's own character comes back with the next load once the
// choice is cleared. A change takes effect through a restart from the last
// checkpoint (sub_8289BEA8 with the GameFlowManager's restart-pending flag set,
// as the pause menu's restart and a death do), which loses the progress since
// that checkpoint.
//
// Levels are scripted for their own characters: with others, triggers that
// wait for a particular character or power, objectives and cutscenes may not
// fire, so a level can get stuck. The mod menu says so.

#include "characters.h"

#include "guest_memory.h"
#include "game_ui.h"

#include <array>
#include <atomic>
#include <mutex>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>

REXCVAR_DEFINE_STRING(characters_test, "", "Game",
                      "Development: character templates for players 1 and 2, as \"one,two\", "
                      "chosen once the first level runs");

// ResolvePlayers: r3 = the GameFlowManager.
REX_EXTERN(sub_8289E428);
REX_EXTERN(__imp__sub_8289E428);
// Restarts the main loop from the last checkpoint snapshot: r3 = the
// GameFlowManager, r4 = 1.
REX_EXTERN(sub_8289BEA8);

namespace {

using simpsons::GuestToHost;
using simpsons::LoadGuestU32;
using simpsons::StoreGuestU32;

struct CharacterTemplate {
  const char* name;
  uint32_t stream_id;
  uint32_t words[3];
};

constexpr CharacterTemplate kTemplates[] = {
    {"bart_bc0_grp0_ss1_h0", 0x41927909, {0x4B8036DD, 0xB12AD298, 0xE1EA669F}},
    {"bart_bc1_grp0_ss1_h0", 0xE068E7B3, {0x4FBD4A07, 0x21660D9A, 0x2C34855A}},
    {"bart_bc1_grp1_ss1_h0", 0xF9296E99, {0x4C9AC4D9, 0xD871579B, 0xA0B22EF5}},
    {"bart_bc1_grp1_ss1_h1", 0x83FDB761, {0x4A48D75F, 0xA4F93AB9, 0x92349FE0}},
    {"bart_bc1_grp1_ss1_h2", 0x8E0BCC61, {0x42243136, 0x6558E7AB, 0x8A7C0B19}},
    {"bart_bc2_grp1_ss2_h2", 0xE663B19D, {0x4EC14F65, 0x74F798BA, 0x90438F13}},
    {"bart_bc2_grp1_ss2_h2_moh", 0xEA5831BB, {0x439964FC, 0x32BC6CAA, 0xAE0EE5B6}},
    {"bart_bc2_grp1_ss2_h2_moh_drk", 0xFE561E14, {0x45816CE5, 0x53026B99, 0x64693FB1}},
    {"bart_bc2_grp1_ss2_h2_mtp", 0xA0AF582E, {0x4AAC4CFC, 0x1A517DB0, 0x128E6219}},
    {"homer_fh0_gh0_hh0_h0", 0xDA4E8185, {0x446C0842, 0x240343A5, 0x00085096}},
    {"homer_fh1_gh0_hh0_h0", 0x9CF7AFAD, {0x46135447, 0x91CA0CB9, 0x5EA46840}},
    {"homer_fh1_gh0_hh1_h1", 0xD9B67B21, {0x411743DB, 0x4E311C80, 0x6BFF16C1}},
    {"homer_fh1_gh0_hh1_h1_sotcd", 0xB78227F4, {0x4F697FFE, 0x6C8C67A0, 0xAF8F5E47}},
    {"homer_fh1_gh0_hh1_h2", 0x41E4761E, {0x409E1D32, 0x4B652BAE, 0x648CD1F4}},
    {"homer_fh1_gh1_hh1_h2", 0x08010E79, {0x4E93A21D, 0x716EE39F, 0x09D7A091}},
    {"homer_fh1_gh1_hh1_h2_bshff", 0x70194DB4, {0x453E6B97, 0x085B0089, 0xF748F9A9}},
    {"homer_fh1_gh1_hh1_h2_moh", 0xE4B61F47, {0x4E9B3042, 0xF6553D8D, 0x411F2AE1}},
    {"homer_fh1_gh1_hh1_h2_moh_drk", 0xFC230B54, {0x4C10227B, 0xCDE9E981, 0x1B7D7B38}},
    {"homer_fh1_gh1_hh1_h2_mtp", 0x2B042AED, {0x401D9209, 0x12F48686, 0xBC6602EB}},
    {"homer_fh1_gh1_hh1_h2_nq", 0x1AD3CC79, {0x46DFC91B, 0xAC2FCAB9, 0xD50A45D1}},
    {"lisa_sx0_hog0_h0", 0x6A0511F7, {0x4EC9F49E, 0x5E3342BC, 0x3FF10592}},
    {"lisa_sx1_hog1_h0", 0x7A2C63B2, {0x4AF53FF1, 0xB01EDDB0, 0x0960AD47}},
    {"lisa_sx2_hog1_h0", 0xE5C1CE15, {0x4605E810, 0x1A01DDB0, 0xAEE0D021}},
    {"lisa_sx2_hog2_h0", 0x206EF848, {0x4B13969A, 0x69280482, 0x1B4D3600}},
    {"lisa_sx2_hog2_h1", 0x109897D5, {0x4283F857, 0x98BC1ABC, 0x4031529B}},
    {"lisa_sx2_hog2_h2", 0xCCF0B7F8, {0x4399973B, 0x7E0FC79C, 0x6E74B86B}},
    {"lisa_sx2_hog3_h2", 0x341862F1, {0x4D2D5318, 0x0C5F69AE, 0x8A7525ED}},
    {"lisa_sx2_hog3_h2_bshff", 0xCD1BCDD9, {0x4991877C, 0x9E7C25B5, 0xF8D29B1A}},
    {"lisa_sx2_hog3_h2_mtp", 0x0560B1DD, {0x44E45C30, 0x5A713DBA, 0xAB80319C}},
    {"marge_mm0_h0", 0x1A9F39F8, {0x4C688BEC, 0xFAE3EFA2, 0x8EB67053}},
    {"marge_mm1_h0", 0xC0CAC4A2, {0x4C775154, 0x1EB8089D, 0x12BCC752}},
    {"marge_mm1_h1", 0xEBB0DDE3, {0x463DE9E5, 0x27A5E68E, 0xE02B8370}},
    {"marge_mm1_h2", 0x67A63D86, {0x469F9E30, 0x560CE5AE, 0x23708863}},
    {"marge_mm2_h2", 0xCFC173AC, {0x4463A2AC, 0xA6C646BD, 0x9F506FE5}},
    {"marge_mm2_h2_gts", 0x3ADD3EF5, {0x4F7A1D79, 0x1838CA89, 0x2BE350F6}},
    {"marge_mm2_h2_mtp", 0x1DBE1259, {0x4B254C87, 0xC520D581, 0x7BA7FDDB}},
    {"marge_mm2_h2_nq", 0xEBA72200, {0x44A79575, 0xC786AA81, 0x391BFDED}},
};

// The choices, in the fork's order: each character with all
// powers, the episode costumes, then the earlier power stages of the story.
const std::vector<characters::Choice> kChoices = {
    {"", "The level's character"},
    {"homer_fh1_gh1_hh1_h2", "Homer"},
    {"homer_fh1_gh1_hh1_h2_bshff", "Homer - Big Super Happy Fun Fun"},
    {"homer_fh1_gh1_hh1_h2_moh", "Homer - Medal of Homer"},
    {"homer_fh1_gh1_hh1_h2_moh_drk", "Homer - Medal of Homer (dark)"},
    {"homer_fh1_gh1_hh1_h2_nq", "Homer - NeverQuest"},
    {"homer_fh1_gh1_hh1_h2_mtp", "Homer - Meet Thy Player"},
    {"homer_fh1_gh0_hh1_h1_sotcd", "Homer - The Colossal Donut"},
    {"homer_fh0_gh0_hh0_h0", "Homer, stage 1 of 5"},
    {"homer_fh1_gh0_hh0_h0", "Homer, stage 2 of 5"},
    {"homer_fh1_gh0_hh1_h1", "Homer, stage 3 of 5"},
    {"homer_fh1_gh0_hh1_h2", "Homer, stage 4 of 5"},
    {"marge_mm2_h2", "Marge"},
    {"marge_mm2_h2_gts", "Marge - Grand Theft Scratchy"},
    {"marge_mm2_h2_nq", "Marge - NeverQuest"},
    {"marge_mm2_h2_mtp", "Marge - Meet Thy Player"},
    {"marge_mm0_h0", "Marge, stage 1 of 5"},
    {"marge_mm1_h0", "Marge, stage 2 of 5"},
    {"marge_mm1_h1", "Marge, stage 3 of 5"},
    {"marge_mm1_h2", "Marge, stage 4 of 5"},
    {"bart_bc2_grp1_ss2_h2", "Bart"},
    {"bart_bc2_grp1_ss2_h2_moh", "Bart - Medal of Homer"},
    {"bart_bc2_grp1_ss2_h2_moh_drk", "Bart - Medal of Homer (dark)"},
    {"bart_bc2_grp1_ss2_h2_mtp", "Bart - Meet Thy Player"},
    {"bart_bc0_grp0_ss1_h0", "Bart, stage 1 of 6"},
    {"bart_bc1_grp0_ss1_h0", "Bart, stage 2 of 6"},
    {"bart_bc1_grp1_ss1_h0", "Bart, stage 3 of 6"},
    {"bart_bc1_grp1_ss1_h1", "Bart, stage 4 of 6"},
    {"bart_bc1_grp1_ss1_h2", "Bart, stage 5 of 6"},
    {"lisa_sx2_hog3_h2", "Lisa"},
    {"lisa_sx2_hog3_h2_bshff", "Lisa - Big Super Happy Fun Fun"},
    {"lisa_sx2_hog3_h2_mtp", "Lisa - Meet Thy Player"},
    {"lisa_sx0_hog0_h0", "Lisa, stage 1 of 7"},
    {"lisa_sx1_hog1_h0", "Lisa, stage 2 of 7"},
    {"lisa_sx2_hog1_h0", "Lisa, stage 3 of 7"},
    {"lisa_sx2_hog2_h0", "Lisa, stage 4 of 7"},
    {"lisa_sx2_hog2_h1", "Lisa, stage 5 of 7"},
    {"lisa_sx2_hog2_h2", "Lisa, stage 6 of 7"},
};


constexpr uint32_t kResolvedPlayers = 0x354;  // 4 slots x 16-byte GUID
constexpr int kAllSlots = 4;
constexpr int kSlots = 2;  // players 1 and 2

constexpr uint32_t kGameFlowManager = 0x82D08BA8;
constexpr uint32_t kFlowEpisode = 160;
constexpr uint32_t kFlowMap = 172;
constexpr uint32_t kFlowFlags = 1200;
constexpr uint32_t kFlowRestartPending = 0x1000;
constexpr uint32_t kFlowBusy = 0x1000 | 0x4000 | 0x10000;  // restart, map reload, delayed restart
constexpr uint32_t kLevelState = 0x82D08C34;  // state at +12 (0 = running), started at +17
constexpr uint32_t kSimEnabled = 0x82CED760;
constexpr uint32_t kPauseReasons = 0x82D572E4;  // 1 load, 2 movie, 4 restart, 8 FMV
constexpr uint32_t kUiPauseCount = 0x82D57368;  // the pause menu and other UI screens

std::mutex g_mutex;
std::array<std::string, kSlots> g_choice;        // "" = the level's
std::array<std::string, kSlots> g_level_choice;  // what the level spawned last
std::atomic<bool> g_restart_requested{false};
std::atomic<bool> g_level_ready{false};

const CharacterTemplate* FindTemplate(std::string_view name) {
  for (const CharacterTemplate& t : kTemplates) {
    if (name == t.name)
      return &t;
  }
  return nullptr;
}

const CharacterTemplate* TemplateByStream(uint32_t stream_id) {
  for (const CharacterTemplate& t : kTemplates) {
    if (t.stream_id == stream_id)
      return &t;
  }
  return nullptr;
}

void ApplyChoices(uint8_t* base, uint32_t game_flow) {
  std::array<std::string, kSlots> choice;
  {
    std::lock_guard lock(g_mutex);
    choice = g_choice;
  }
  const CharacterTemplate* level[kAllSlots] = {};
  for (int slot = 0; slot < kAllSlots; ++slot)
    level[slot] = TemplateByStream(LoadGuestU32(base, game_flow + kResolvedPlayers + slot * 16));
  const CharacterTemplate* result[kAllSlots] = {level[0], level[1], level[2], level[3]};
  for (int slot = 0; slot < kSlots; ++slot) {
    if (choice[slot].empty())
      continue;
    const CharacterTemplate* chosen = FindTemplate(choice[slot]);
    if (!chosen)
      continue;
    // One template spawns one character: a second slot would get the same one.
    bool taken = false;
    for (int other = 0; other < kAllSlots; ++other)
      taken |= other != slot && result[other] == chosen;
    if (taken) {
      REXLOG_WARN("[characters] player {}: {} is already in the level, keeping the level's",
                  slot + 1, chosen->name);
      continue;
    }
    result[slot] = chosen;
  }
  for (int slot = 0; slot < kSlots; ++slot) {
    const uint32_t guid = game_flow + kResolvedPlayers + slot * 16;
    if (result[slot] != level[slot] && result[slot]) {
      StoreGuestU32(base, guid + 0, result[slot]->stream_id);
      StoreGuestU32(base, guid + 4, result[slot]->words[0]);
      StoreGuestU32(base, guid + 8, result[slot]->words[1]);
      StoreGuestU32(base, guid + 12, result[slot]->words[2]);
      REXLOG_INFO("[characters] player {}: {} instead of {}", slot + 1, result[slot]->name,
                  level[slot] ? level[slot]->name : "none");
    }
  }
  std::lock_guard lock(g_mutex);
  for (int slot = 0; slot < kSlots; ++slot)
    g_level_choice[slot] = level[slot] ? level[slot]->name : "";
}

// Whether the level runs with nothing in front of it.
bool LevelRunning(uint8_t* base, uint32_t game_flow, bool allow_ui) {
  const uint32_t level = LoadGuestU32(base, kLevelState);
  if (!game_flow || !level)
    return false;
  return LoadGuestU32(base, level + 12) == 0 && GuestToHost(base, level + 17)[0] == 1 &&
         LoadGuestU32(base, game_flow + kFlowMap) != 0 &&
         LoadGuestU32(base, game_flow + kFlowEpisode) != 0 &&
         (LoadGuestU32(base, game_flow + kFlowFlags) & kFlowBusy) == 0 &&
         LoadGuestU32(base, kSimEnabled) != 0 && LoadGuestU32(base, kPauseReasons) == 0 &&
         (allow_ui || LoadGuestU32(base, kUiPauseCount) == 0);
}

}  // namespace

namespace characters {

const std::vector<Choice>& Choices() {
  return kChoices;
}

std::string Current(int player) {
  std::lock_guard lock(g_mutex);
  return player >= 0 && player < kSlots ? g_choice[size_t(player)] : std::string();
}

std::string LevelCharacter(int player) {
  std::lock_guard lock(g_mutex);
  if (player < 0 || player >= kSlots)
    return {};
  for (const auto& choice : kChoices) {
    if (*choice.name && g_level_choice[size_t(player)] == choice.name)
      return choice.label;
  }
  return g_level_choice[size_t(player)];
}

void Apply(const std::string& player1, const std::string& player2) {
  {
    std::lock_guard lock(g_mutex);
    g_choice = {player1, player2};
  }
  REXLOG_INFO("[characters] player 1 {}, player 2 {}; restarting from the last checkpoint",
              player1.empty() ? "the level's" : player1, player2.empty() ? "the level's" : player2);
  g_restart_requested = true;
}

bool LevelReady() {
  return g_level_ready.load();
}

}  // namespace characters

// The simulation tick (freecam.cpp's hook), on the game thread: the restart a
// change asked for, once the level runs without a menu in front of it.
void CharactersTick(PPCContext& ctx, uint8_t* base) {
  const uint32_t game_flow = LoadGuestU32(base, kGameFlowManager);
  g_level_ready = LevelRunning(base, game_flow, true) || LoadGuestU32(base, kUiPauseCount) != 0;
  static bool tested = false;
  if (!tested && !REXCVAR_GET(characters_test).empty() && LevelRunning(base, game_flow, false)) {
    tested = true;
    const std::string& test = REXCVAR_GET(characters_test);
    const size_t comma = test.find(',');
    characters::Apply(test.substr(0, comma),
                      comma == std::string::npos ? std::string() : test.substr(comma + 1));
  }
  if (!g_restart_requested.load() || game_ui::IsOpen() || !LevelRunning(base, game_flow, false))
    return;
  g_restart_requested = false;
  StoreGuestU32(base, game_flow + kFlowFlags,
                LoadGuestU32(base, game_flow + kFlowFlags) | kFlowRestartPending);
  REXLOG_INFO("[characters] restarting from the last checkpoint");
  rex::CallFrame frame(ctx);
  frame.ctx.r3.u64 = game_flow;
  frame.ctx.r4.u64 = 1;
  sub_8289BEA8(frame, base);
}

REX_FUNC(sub_8289E428) {
  const uint32_t game_flow = ctx.r3.u32;
  __imp__sub_8289E428(ctx, base);
  if (game_flow)
    ApplyChoices(base, game_flow);
}
