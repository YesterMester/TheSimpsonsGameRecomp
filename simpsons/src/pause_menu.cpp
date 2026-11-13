// Achievements and the debug menu in the pause menu (drawn by game_ui.cpp).
//
// The pause screen (singleton 0x82D08E30, state 0x82D08E34: 1 while its menu
// is up) is shown by sub_823A5C50, which starts its key listener, and takes
// input in sub_823A6C78(screen, player, key). Accept (key 6) reads the
// selected item's id with the movie's getCurrentMenuSelection and compares it
// with Resume_game, Save_game, Quit_episode and the rest.

#include "pause_menu.h"
#include "saves.h"

#include "game_menu.h"
#include "game_menu_apt.h"
#include "guest_memory.h"
#include "game_ui.h"

#include <array>
#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/runtime.h>

REXCVAR_DEFINE_BOOL(pause_menu_probe, false, "Game",
                    "Log the pause menu's movie variables when it opens (for development)");
REXCVAR_DEFINE_BOOL(save_anywhere, false, "Game",
                    "The pause menu's Save Game is in every level and saves (or loads) right "
                    "there with the save library");
REXCVAR_DEFINE_STRING(pause_menu_test_screen, "", "Game",
                      "Open screens with the pause menu, one per pause, from a comma separated "
                      "list of achievements and debug_menu (for testing with controller input "
                      "only)");

REX_EXTERN(__imp__sub_823A5C50);
REX_EXTERN(sub_823A5C50);
REX_EXTERN(__imp__sub_823A62F0);
REX_EXTERN(sub_823A62F0);
REX_EXTERN(__imp__sub_823A6C78);
REX_EXTERN(sub_823A6C78);
REX_EXTERN(__imp__sub_82741C38);
REX_EXTERN(sub_82741C38);
REX_EXTERN(sub_827C6108);
REX_EXTERN(__imp__sub_823AC620);
REX_EXTERN(sub_823AC620);

namespace {

using simpsons::GuestToHost;
using simpsons::LoadGuestU32;

constexpr uint32_t kPauseScreen = 0x82D08E30;
constexpr uint32_t kPauseState = 0x82D08E34;
constexpr uint32_t kFrontendState = 0x82D08C98;  // 9 while a level runs
// The pause menu's Save Game row's id, "Save_game", and what it becomes with
// Save anywhere.
constexpr uint32_t kSaveGameId = 0x82002CB0;
constexpr const char* kSaveAnywhereId = "Save_anywhere";

// Set from the pause menu's showing until its script has asked IsPlayingDemo.
std::atomic<bool> g_pause_opening{false};

// Set while the pause menu takes Accept, to see whether Save Game was chosen.
std::atomic<bool> g_watch_save_row{false};
bool g_save_row_chosen = false;

rex::Runtime* g_runtime = nullptr;
std::unique_ptr<simpsons::GameMenuApt> g_apt;
int g_probe_after = 0;
std::atomic<bool> g_showing{false};

std::string MoviePath(const uint8_t* base, uint32_t screen) {
  const auto handle = LoadGuestU32(base, screen + 96);
  const auto path = handle ? LoadGuestU32(base, handle) : 0;
  return path ? reinterpret_cast<const char*>(GuestToHost(base, path)) : std::string();
}

void Probe(PPCContext& ctx, uint8_t* base) {
  const uint32_t screen = LoadGuestU32(base, kPauseScreen);
  if (!screen)
    return;
  const std::string movie = MoviePath(base, screen);
  REXLOG_INFO("[pause-probe] screen {:08X} state {} movie '{}'", screen,
              LoadGuestU32(base, kPauseState), movie);
  if (movie.empty())
    return;
  if (!g_apt)
    g_apt = std::make_unique<simpsons::GameMenuApt>(g_runtime);
  auto& apt = *g_apt;
  apt.Bind(ctx, base, movie);
  // The menu clip, as the menu pointer reaches it.
  for (const char* array : {"MenuItemIds", "TextRefs", "MenuItemButtons", "RemoveItems"}) {
    const std::string path = std::string("ActiveMenu.") + array;
    const int32_t length = apt.Number(path + ".length");
    std::string items;
    for (int32_t i = 0; i < length && i < 16; ++i)
      items += (i ? ", " : "") + apt.Text(path + "." + std::to_string(i));
    REXLOG_INFO("[pause-probe] {} ({}): {}", path, length, items);
  }
  REXLOG_INFO("[pause-probe] ActiveMenu.currentSelection {}",
              apt.Number("ActiveMenu.currentSelection"));
  for (const char* clip : {"text_resume_game", "text_restart_challenge", "text_options",
                           "text_controller", "text_exit_game", "btn_Resume_Game"}) {
    const std::string path = std::string("ActiveMenu.") + clip;
    if (apt.Get(path))
      REXLOG_INFO("[pause-probe] {} exists, _visible {}, _y {}", path,
                  apt.Real(path + "._visible"), apt.Real(path + "._y"));
  }
}

}  // namespace

void InitPauseMenu(rex::Runtime* runtime) {
  g_runtime = runtime;
  g_apt = std::make_unique<simpsons::GameMenuApt>(runtime);
}

namespace {
}  // namespace

// The game thread, once a frame (game_thread.cpp).
void TickPauseMenu(PPCContext& ctx, uint8_t* base) {
  const uint32_t screen = LoadGuestU32(base, kPauseScreen);
  // Options opens over the pause menu without changing its state.
  const bool showing = screen && LoadGuestU32(base, screen + 28) == 1 &&
                       LoadGuestU32(base, kPauseState) == 1 && !GameMenuIsVisible();
  g_showing = showing;
  if (g_probe_after > 0 && --g_probe_after == 0 && LoadGuestU32(base, kPauseState) == 1)
    Probe(ctx, base);
}

bool PauseMenuShowing() {
  return g_showing.load(std::memory_order_relaxed);
}

REX_FUNC(sub_823A5C50) {
  // The game as it was, before the pause menu covers it: the next save's
  // picture.
  if (saves::kLibraryEnabled)
    saves::RememberFrame();
  g_pause_opening = (saves::kLibraryEnabled && REXCVAR_GET(save_anywhere)) && LoadGuestU32(base, kFrontendState) == 9;
  __imp__sub_823A5C50(ctx, base);
  if (REXCVAR_GET(pause_menu_probe))
    g_probe_after = 5;
  static size_t test_index = 0;
  const std::string tests = REXCVAR_GET(pause_menu_test_screen);
  size_t start = 0;
  for (size_t i = 0; i < test_index && start != std::string::npos; ++i) {
    start = tests.find(',', start);
    start = start == std::string::npos ? start : start + 1;
  }
  if (start == std::string::npos || start >= tests.size())
    return;
  ++test_index;
  const std::string test = tests.substr(start, tests.find(',', start) - start);
  if (test == "achievements")
    game_ui::Open(game_ui::Screen::kAchievements);
  else if (test == "debug_menu")
    game_ui::Open(game_ui::Screen::kDebugMenu);
}

// CanSaveGameInPause, asked by the pause menu's script as it lays out its
// rows. The game allows its Save Game in Springfield only; with Save anywhere
// it's in every level and opens the save library's screen.
REX_FUNC(sub_823A62F0) {
  REXLOG_INFO("[pause] CanSaveGameInPause asked (state {})", LoadGuestU32(base, kFrontendState));
  if (!(saves::kLibraryEnabled && REXCVAR_GET(save_anywhere)) || LoadGuestU32(base, kFrontendState) != 9) {
    __imp__sub_823A62F0(ctx, base);
    return;
  }
  // The script's answer, as the original gives it.
  ctx.r3.u64 = 1;
  sub_827C6108(ctx, base);
}

// The pause menu's input (screen, player, key). Accept (key 6) reads the
// chosen row's id and compares it with each row's (sub_82741C38); on Save Game
// the saves screen opens instead of the game's own save.
REX_FUNC(sub_823A6C78) {
  if (game_ui::SavesMenuEvent(ctx, base, ctx.r3.u32, int(ctx.r5.u32)))
    return;
  const bool watch = (saves::kLibraryEnabled && REXCVAR_GET(save_anywhere)) && ctx.r5.u32 == 6 &&
                     LoadGuestU32(base, kPauseState) == 1 &&
                     LoadGuestU32(base, kFrontendState) == 9;
  if (watch && g_apt && g_apt->ready()) {
    // The row kept by Save anywhere: the saves screen, not the game.
    const std::string movie = MoviePath(base, ctx.r3.u32);
    std::string id;
    if (!movie.empty()) {
      const PPCContext saved = ctx;
      g_apt->Bind(ctx, base, movie);
      id = g_apt->Text("ActiveMenu.MenuItemIds." +
                       std::to_string(g_apt->Number("ActiveMenu.currentSelection")));
      ctx = saved;
    }
    if (id == kSaveAnywhereId) {
      REXLOG_INFO("[pause] Save Game: the save library");
      game_ui::OpenSavesFromPause();
      return;
    }
  }
  g_save_row_chosen = false;
  g_watch_save_row = watch;
  __imp__sub_823A6C78(ctx, base);
  g_watch_save_row = false;
  if (g_save_row_chosen) {
    REXLOG_INFO("[pause] Save Game: the save library");
    game_ui::OpenSavesFromPause();
  }
}

// Compares a script string with text: 0 when they're the same.
REX_FUNC(sub_82741C38) {
  const uint32_t text = ctx.r4.u32;
  __imp__sub_82741C38(ctx, base);
  if (g_watch_save_row.load(std::memory_order_relaxed) && text == kSaveGameId &&
      ctx.r3.u32 == 0) {
    // Not Save Game, as far as the pause menu goes: it stays as it is.
    g_save_row_chosen = true;
    ctx.r3.u64 = 1;
  }
}

// IsPlayingDemo, which the pause menu's script asks just before it lays out
// its rows (initializeButtons, which then leaves out those in RemoveItems,
// Save_game always among them). Its row list (MenuItemIds) is still whole
// then, and when returning from Options: Save Game's id there becomes
// Save_anywhere, which nothing leaves out, so the row stays with the game's
// own button and text. Choosing it opens the saves screen (sub_823A6C78).
REX_FUNC(sub_823AC620) {
  __imp__sub_823AC620(ctx, base);
  if (!g_pause_opening.exchange(false) &&
      !((saves::kLibraryEnabled && REXCVAR_GET(save_anywhere)) && LoadGuestU32(base, kFrontendState) == 9 &&
        LoadGuestU32(base, kPauseState) == 1))
    return;
  const uint32_t screen = LoadGuestU32(base, kPauseScreen);
  const std::string movie = screen ? MoviePath(base, screen) : std::string();
  if (movie.empty() || !g_apt || !g_apt->ready())
    return;
  const PPCContext saved = ctx;
  auto& apt = *g_apt;
  apt.Bind(ctx, base, movie);
  const int count = apt.Number("PauseMenu.MenuItemIds.length");
  std::string items;
  for (int i = 0; i < count && i < 16; ++i) {
    const std::string path = "PauseMenu.MenuItemIds." + std::to_string(i);
    const std::string item = apt.Text(path);
    items += (i ? ", " : "") + item;
    if (item == "Save_game")
      apt.SetText(path, kSaveAnywhereId);
  }
  REXLOG_INFO("[pause] rows as the menu opens: {}", items);
  ctx = saved;
}
