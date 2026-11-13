// The save library in the game's own menus (game_ui.h, saves.h).
//
// Saved Games offers Continue, New Game, Load Game, the playthrough's game
// menu and the original slots. Save Game in the pause menu saves or loads
// anywhere. Pages reuse the shipped APT text references and buttons; leaving
// restores the original menu before the game's loading/saving flow runs.
//
// A selected save is copied into slot A, then the original menus select that
// slot, answer the load question and Continue. Exact snapshots go in after
// the level starts, while paused; the original pause menu then closes.

#include "game_ui.h"

#include "game_canvas.h"
#include "game_menu_apt.h"
#include "game_menu_pointer.h"
#include "game_thread.h"
#include "guest_memory.h"
#include "host_memory.h"
#include "modkit_game.h"
#include "pause_menu.h"
#include "saves.h"
#include "snapshot.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstring>
#include <cmath>
#include <ctime>
#include <memory>
#include <map>
#include <string>
#include <vector>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/hook.h>
#include <rex/ppc.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/runtime.h>

REXCVAR_DEFINE_BOOL(saves_screen, false, "Game",
                    "Use the game's Saved Games menu for Continue, New Game and "
                    "the save library");

REX_EXTERN(__imp__sub_8239F330);
REX_EXTERN(sub_8239F330);
REX_EXTERN(sub_826BFFC8);

namespace game_ui {
namespace {

using simpsons::LoadGuestU32;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

constexpr uint32_t kFrontendState = 0x82D08C98;  // 9 while a level runs
constexpr uint32_t kFrontEndInput = 0x8239F330;
constexpr uint32_t kPopupInput = 0x823A7FC0;
constexpr size_t kNameLength = 40;

rex::Runtime* g_runtime = nullptr;
std::unique_ptr<simpsons::GameMenuApt> g_apt;

std::tm LocalTime(int64_t time) {
  const std::time_t t = std::time_t(time);
  std::tm local{};
#if REX_PLATFORM_WIN32
  localtime_s(&local, &t);
#else
  localtime_r(&t, &local);
#endif
  return local;
}

// "Today 21:40", "Yesterday 09:12" or "12 Oct 2026, 21:40".
std::string When(int64_t time) {
  const std::tm local = LocalTime(time), today = LocalTime(int64_t(std::time(nullptr)));
  char clock[16], day[32];
  std::strftime(clock, sizeof(clock), "%H:%M", &local);
  if (local.tm_year == today.tm_year && local.tm_yday == today.tm_yday)
    return fmt::format("Today {}", clock);
  if (local.tm_year == today.tm_year && local.tm_yday + 1 == today.tm_yday)
    return fmt::format("Yesterday {}", clock);
  std::strftime(day, sizeof(day), "%d %b %Y", &local);
  return fmt::format("{}, {}", day, clock);
}

// --- The game's pages --------------------------------------------------------

struct GamePage {
  uint32_t screen = 0, input = 0;
  std::string name;
  int rows = 0, selection = -1;
  bool slots = false;  // Saved Games
};

GamePage ReadPage(PPCContext& ctx, uint8_t* base) {
  GamePage page;
  page.screen = GameMenuPointerScreen();
  if (!page.screen || LoadGuestU32(base, page.screen + 28) != 1)
    return {};
  const uint32_t table = LoadGuestU32(base, page.screen);
  page.input = table ? LoadGuestU32(base, table + 100) : 0;
  const uint32_t handle = LoadGuestU32(base, page.screen + 96);
  const uint32_t path = handle ? LoadGuestU32(base, handle) : 0;
  if (!path || !g_apt || !g_apt->ready())
    return page;
  const PPCContext saved = ctx;
  g_apt->Bind(ctx, base, reinterpret_cast<const char*>(simpsons::GuestToHost(base, path)));
  page.name = g_apt->Text("ActiveMenu._name");
  page.rows = g_apt->Number("ActiveMenu.MenuItemButtons.length");
  page.selection = g_apt->Number("ActiveMenu.currentSelection");
  page.slots = page.input == kFrontEndInput && page.name == "PickSlot";
  ctx = saved;
  return page;
}

// The index of one of a menu's rows by its id (MenuItemIds), or -1.
int PauseRow(PPCContext& ctx, uint8_t* base, uint32_t screen, std::string_view id) {
  const uint32_t handle = LoadGuestU32(base, screen + 96);
  const uint32_t path = handle ? LoadGuestU32(base, handle) : 0;
  if (!path || !g_apt || !g_apt->ready())
    return -1;
  const PPCContext saved = ctx;
  g_apt->Bind(ctx, base, reinterpret_cast<const char*>(simpsons::GuestToHost(base, path)));
  int found = -1;
  const int count = g_apt->Number("ActiveMenu.MenuItemIds.length");
  for (int i = 0; i < count && i < 16 && found < 0; ++i)
    if (g_apt->Text("ActiveMenu.MenuItemIds." + std::to_string(i)) == id)
      found = i;
  ctx = saved;
  return found;
}

// --- Working the game's menus ------------------------------------------------

enum class Flow { kNone, kLoad, kNewGame, kGameMenu, kLoadHere, kLoadExit };
enum class Stage {
  kSources,
  kExitRow,
  kExitAsk,
  kFrontEnd,
  kSlots,
  kDeleteAsk,
  kSlotAsk,
  kMainMenu,
  kLevel,
  kPause,
  kPut,
  kResume
};

struct Drive {
  Flow flow = Flow::kNone;
  Stage stage = Stage::kSlots;
  saves::Save save;
  bool checkpoint = false;
  bool deleted = false;
  int tries = 0;
  Clock::time_point stage_start{}, wait_until{}, deadline{};
  std::string seen;
};
Drive g_drive;

// The black the game turns into an exact save's moment under: how much shows
// (0 to 1) and where it's going.
float g_black = 0, g_black_target = 0;

const char* StageName(Stage stage) {
  switch (stage) {
    case Stage::kSources:
      return "checking mods";
    case Stage::kExitRow:
      return "exit game";
    case Stage::kExitAsk:
      return "exit question";
    case Stage::kFrontEnd:
      return "front end";
    case Stage::kSlots:
      return "slots";
    case Stage::kDeleteAsk:
      return "delete question";
    case Stage::kSlotAsk:
      return "load question";
    case Stage::kMainMenu:
      return "main menu";
    case Stage::kLevel:
      return "level";
    case Stage::kPause:
      return "pause";
    case Stage::kPut:
      return "put";
    case Stage::kResume:
      return "resume";
  }
  return "?";
}

void Next(Stage stage, std::chrono::milliseconds limit, std::chrono::milliseconds wait = 0ms) {
  const auto now = Clock::now();
  g_drive.stage = stage;
  g_drive.tries = 0;
  g_drive.stage_start = now;
  g_drive.wait_until = now + wait;
  g_drive.deadline = now + limit;
  REXLOG_INFO("[saves-ui] -> {}", StageName(stage));
}

void Notice(std::string text, bool load_error = false);

void Finish(bool ok, std::string message) {
  REXLOG_INFO("[saves-ui] {} '{}': {}", ok ? "done" : "FAILED", g_drive.save.name, message);
  if (!ok) {
    modkit_game::CancelRestore();
    saves::DropRead();
    CancelPressesForGame();
  }
  snapshot::Release();
  g_drive.flow = Flow::kNone;
  g_black_target = 0;
  BlockGameInput(false);
  if (Current() == Screen::kSaves)
    Close();
  if (!message.empty()) {
    Notice(message, !ok);
    Notify(std::move(message));
  }
}

// Restore the original menu before driving its slot/load/Continue sequence.
void Begin(Flow flow, const saves::Save& save) {
  Notice({}, false);
  Close();
  g_drive = {};
  g_drive.flow = flow;
  g_drive.save = save;
  BlockGameInput(true);
  if (flow == Flow::kLoadHere) {
    Close();
    g_black_target = 1;
  } else if (flow == Flow::kLoadExit) {
    Close();
  }
  REXLOG_INFO("[saves-ui] {} '{}' ({})",
              flow == Flow::kNewGame    ? "new game"
              : flow == Flow::kGameMenu ? "game menu"
                                        : "load",
              save.name, save.id);
}

// Reset the library state on returning to Saved Games.
void ShowOnSavedGames();

void Work(PPCContext& ctx, uint8_t* base) {
  auto& d = g_drive;
  if (d.flow == Flow::kNone)
    return;
  const auto now = Clock::now();
  if (now < d.wait_until || PressingForGame())
    return;
  const uint32_t state = LoadGuestU32(base, kFrontendState);
  const GamePage page = ReadPage(ctx, base);
  const std::string seen = fmt::format("state {} screen {:08X} input {:08X} page '{}' rows {} at {}",
                                       state, page.screen, page.input, page.name, page.rows,
                                       page.selection);
  if (seen != d.seen) {
    d.seen = seen;
    REXLOG_INFO("[saves-ui] {}: {}", StageName(d.stage), seen);
  }
  if (now > d.deadline) {
    Finish(false, "Couldn't load: the game didn't get there");
    return;
  }
  const bool exact = d.save.exact && d.flow == Flow::kLoad;
  switch (d.stage) {
    case Stage::kSources: {
      if (d.checkpoint && !modkit_game::CheckpointReady())
        return;
      if (!saves::PrepareSlot(d.save, d.checkpoint)) {
        const auto error = d.checkpoint ? modkit_game::CheckpointError() : std::string{};
        Finish(false, error.empty() ? "Couldn't prepare that save" : "Couldn't load: " + error);
        return;
      }
      if (d.flow == Flow::kLoadExit) {
        d.flow = Flow::kLoad;
        d.save.exact = false;
        ShowOnSavedGames();
      }
      if (d.flow == Flow::kLoad && d.save.exact && !saves::Read(d.save.id)) {
        Finish(false, "Couldn't read that save");
        return;
      }
      saves::Track(true);
      Next(Stage::kSlots, 20s);
      return;
    }
    case Stage::kExitRow: {
      // The pause menu's Exit Game, found by its id (the rows differ in and
      // out of episodes).
      if (!PauseMenuShowing() || page.screen == 0)
        return;
      const int row = PauseRow(ctx, base, page.screen, "Exit_game");
      if (row < 0) {
        Finish(false, "Couldn't find Exit Game");
        return;
      }
      if (page.selection != row) {
        PressForGame(page.selection < row ? kPadDown : kPadUp);
        d.wait_until = now + 250ms;
        return;
      }
      PressForGame(kPadA);
      Next(Stage::kExitAsk, 10s, 500ms);
      return;
    }
    case Stage::kExitAsk:
      // Yes (A) to leaving: progress since the checkpoint goes.
      if (page.input == kPopupInput) {
        PressForGame(kPadA);
        Next(Stage::kFrontEnd, 120s, 800ms);
      } else if (now - d.stage_start > 3s) {
        Next(Stage::kFrontEnd, 120s);
      }
      return;
    case Stage::kFrontEnd:
      // Out of the level: Saved Games, or the main menu one step from it.
      if (page.slots) {
        if (!modkit_game::BeginCheckpoint(d.save.folder)) {
          Finish(false, "Couldn't start checking that save's mods");
          return;
        }
        d.checkpoint = true;
        Next(Stage::kSources, 60s);
        return;
      }
      if (page.input == kFrontEndInput && page.name == "MainMenu") {
        PressForGame(kPadB);
        d.wait_until = now + 900ms;
      }
      return;
    case Stage::kSlots:
      // Saved Games, with slot A chosen. A question there is answered: Yes
      // just after deleting (it says so), otherwise No.
      if (page.input == kPopupInput) {
        PressForGame(d.deleted && now - d.stage_start < 5s ? kPadA : kPadB);
        d.wait_until = now + 500ms;
        return;
      }
      if (!page.slots)
        return;
      if (page.selection != 0) {
        PressForGame(kPadUp);
        d.wait_until = now + 250ms;
        return;
      }
      if (d.flow == Flow::kNewGame && !d.deleted) {
        PressForGame(kPadX);
        Next(Stage::kDeleteAsk, 8s, 300ms);
        return;
      }
      if (d.flow == Flow::kNewGame && !saves::SlotAEmpty()) {
        // The game may still be writing the emptied slot; once more if not.
        if (now - d.stage_start < 3s)
          return;
        if (d.tries++ < 1) {
          d.deleted = false;
          return;
        }
        Finish(false, "Couldn't clear slot A for the new game");
        return;
      }
      PressForGame(kPadA);
      Next(Stage::kSlotAsk, 10s, 300ms);
      return;
    case Stage::kDeleteAsk:
      // The game's question (Kent Brockman's) lists No, then Yes, with No
      // chosen. No question: the slot was empty already.
      if (page.input == kPopupInput) {
        PressForGame(kPadDown);
        PressForGame(kPadA);
        d.deleted = true;
        Next(Stage::kSlots, 15s, 1000ms);
      } else if (now - d.stage_start > 2500ms) {
        d.deleted = true;
        Next(Stage::kSlots, 15s);
      }
      return;
    case Stage::kSlotAsk:
      if (page.input == kPopupInput) {
        PressForGame(kPadA);
        Next(Stage::kMainMenu, 60s, 500ms);
      } else if (page.slots && now - d.stage_start > 2s) {
        Next(Stage::kSlots, 10s);
      } else if (!page.slots && state != 6) {
        Next(Stage::kMainMenu, 60s);
      }
      return;
    case Stage::kMainMenu:
      if (state == 9) {
        Next(Stage::kLevel, 180s);
        return;
      }
      if (d.flow == Flow::kNewGame && ((state != 6 && state != 7 && state != 8) ||
                                       (!page.screen && now - d.stage_start > 1500ms))) {
        // The new game's opening has started: the game's menus have gone.
        Finish(true, {});
        return;
      }
      if (page.input == kPopupInput) {
        // More questions on the way (a new game's).
        PressForGame(kPadA);
        d.wait_until = now + 600ms;
        return;
      }
      if (page.input != kFrontEndInput || page.slots || page.rows <= 0)
        return;
      if (d.flow == Flow::kGameMenu) {
        Finish(true, {});
        return;
      }
      if (page.selection != 0) {
        PressForGame(kPadUp);
        d.wait_until = now + 250ms;
        return;
      }
      // Continue: the original TV turns off and its loading screen shows.
      PressForGame(kPadA);
      if (Current() == Screen::kSaves)
        Close();
      Next(Stage::kLevel, 180s, 1s);
      return;
    case Stage::kLevel:
      if (state != 9) {
        if (d.flow == Flow::kNewGame && state != 6 && state != 7 && state != 8) {
          Finish(true, {});
          return;
        }
        if (page.input == kPopupInput) {
          PressForGame(kPadA);
          d.wait_until = now + 600ms;
        }
        return;
      }
      if (!exact) {
        Finish(true, {});
        return;
      }
      if (saves::ReadStatus() == saves::ReadState::kOlder) {
        saves::DropRead();
        Finish(true, "Loaded at its checkpoint: an older version made that save");
        return;
      }
      // The loading screen is part of the level too: the save goes in once
      // the level plays (its physics steps).
      if (!LevelStepped(250))
        return;
      // The level runs: black at once, and the saved moment goes in under
      // it while the game is paused, as it was saved (the game's audio is
      // paused then, which freezing it needs).
      g_black = g_black_target = 1;
      PressForGame(kPadStart);
      Next(Stage::kPause, 12s, 400ms);
      return;
    case Stage::kPause:
      if (!PauseMenuShowing()) {
        if (now - d.stage_start > 3s && d.tries++ < 3) {
          PressForGame(kPadStart);
          d.stage_start = now;
        }
        return;
      }
      Next(Stage::kPut, 60s);
      return;
    case Stage::kPut: {
      // Loading from the pause menu waits for the black.
      if (g_black < 1)
        return;
      const auto status = saves::ReadStatus();
      if (status == saves::ReadState::kOlder) {
        Finish(false, d.flow == Flow::kLoadHere
                          ? "An older version made that save: load it from Saved Games"
                          : "Loaded at its checkpoint: an older version made that save");
        return;
      }
      if (status == saves::ReadState::kFailed || status == saves::ReadState::kNone) {
        Finish(false, "Couldn't read that save");
        return;
      }
      if (status != saves::ReadState::kReady || !snapshot::Ready())
        return;
      if (!saves::PutRead(ctx, base)) {
        if (++d.tries > 120)
          Finish(false, d.flow == Flow::kLoadHere
                            ? "The game wasn't ready to load that save here"
                            : "Loaded at its checkpoint: the exact moment didn't fit");
        return;
      }
      if (d.flow == Flow::kLoadHere) {
        // The save's slot A and playthrough, now that the game is the save.
        saves::PrepareSlot(d.save, false);
        saves::Track(true);
      }
      // The game carries on from the save now: ctx is the save's. It was
      // paused when saved; the pause menu goes once it shows.
      Next(Stage::kResume, 10s, 400ms);
      return;
    }
    case Stage::kResume:
      // The save was made paused: its pause menu closes into the game, as
      // after any pause, while the black lifts.
      g_black_target = 0;
      if (PauseMenuShowing() && now - d.stage_start < 8s) {
        PressForGame(kPadB);
        d.wait_until = now + 700ms;
        return;
      }
      Finish(true, {});
      return;
  }
}

// --- The screen --------------------------------------------------------------

enum class Mode { kFrontEnd, kPause };
enum class Page { kMain, kBrowse, kKeyboard, kConfirm, kDetails, kLibraryOptions };
enum class Typing { kNone, kNewSave, kNewGame, kRename, kSearch };
enum class Ask { kNone, kOverwrite, kLoad, kDelete, kNewGame };

struct Ui {
  Mode mode = Mode::kFrontEnd;
  Page page = Page::kMain;
  int main_selected = 0;
  // The library, read again now and then.
  std::vector<saves::Save> saves;  // every save
  bool dirty = true;
  bool reload = true;
  uint64_t catalog_version = ~uint64_t(0);
  saves::Playthrough current;
  // The browser.
  std::vector<saves::Save> list;  // the saves shown
  std::string tapped;              // the save tapped last
  std::string search;
  bool all = true;  // every playthrough's
  int selected = 0, top = 0;     // -1: the search box
  float scroll = 0;
  // The keyboard.
  Typing typing = Typing::kNone;
  Page typing_from = Page::kMain;
  std::string text;
  bool fresh = false, shift = true;
  int key_row = 0, key_column = 0;
  std::string target;  // the save renamed
  // The question.
  Ask ask = Ask::kNone;
  Page ask_from = Page::kMain;
  std::string question;
  bool yes = false;
  saves::Save subject;
  // Asked of the game thread's next frame.
  std::string save_name, save_replace;
  bool save_now = false;
  // Where the game is (pause mode).
  std::string location;
  bool can_save = false;
  bool classic = false;  // the classic slots for this visit to Saved Games
};
Ui g_ui;

struct Catalog {
  std::vector<saves::Save> saves;
  Clock::time_point read{};
  uint64_t version = 0;
  bool pending = false, ready = false;
};
Catalog g_catalog;

void ShowOnSavedGames() {
  g_ui = Ui{};
  g_ui.mode = Mode::kFrontEnd;
  Open(Screen::kSaves);
}

void Refresh() {
  const auto now = Clock::now();
  if (!g_catalog.pending && (g_ui.reload || !g_catalog.ready || now - g_catalog.read >= 3s)) {
    g_ui.reload = false;
    g_catalog.pending = true;
    saves::Scan([](std::vector<saves::Save> list) {
      g_catalog.saves = std::move(list);
      g_catalog.read = Clock::now();
      g_catalog.pending = false;
      g_catalog.ready = true;
      ++g_catalog.version;
    });
  }
  const bool changed = g_ui.catalog_version != g_catalog.version;
  if (!g_ui.dirty && !changed)
    return;
  std::string selected;
  if (!g_ui.dirty && g_ui.selected >= 0 && g_ui.selected < int(g_ui.list.size()))
    selected = g_ui.list[size_t(g_ui.selected)].id;
  g_ui.dirty = false;
  if (changed) {
    g_ui.catalog_version = g_catalog.version;
    g_ui.saves = g_catalog.saves;
    g_ui.current = saves::Current();
  }
  g_ui.list.clear();
  std::vector<std::string> words;
  {
    std::string lower;
    for (const char c : g_ui.search)
      lower += char(std::tolower(uint8_t(c)));
    size_t start = 0;
    while (start < lower.size()) {
      size_t end = lower.find(' ', start);
      if (end == std::string::npos)
        end = lower.size();
      if (end > start)
        words.push_back(lower.substr(start, end - start));
      start = end + 1;
    }
  }
  for (const auto& save : g_ui.saves) {
    if (!g_ui.all && save.playthrough != g_ui.current.id)
      continue;
    std::string text = save.name + " " + save.playthrough_name + " " + save.location;
    for (char& c : text)
      c = char(std::tolower(uint8_t(c)));
    if (std::all_of(words.begin(), words.end(),
                    [&](const std::string& w) { return text.find(w) != std::string::npos; }))
      g_ui.list.push_back(save);
  }
  if (!selected.empty()) {
    const auto found = std::find_if(g_ui.list.begin(), g_ui.list.end(),
                                    [&](const saves::Save& save) { return save.id == selected; });
    if (found != g_ui.list.end())
      g_ui.selected = int(found - g_ui.list.begin());
  }
  g_ui.selected = std::clamp(g_ui.selected, -1, int(g_ui.list.size()) - 1);
}

const saves::Save* Latest() {
  for (const auto& save : g_ui.saves)
    if (g_ui.current.id.empty() || save.playthrough == g_ui.current.id)
      return &save;
  return g_ui.saves.empty() ? nullptr : &g_ui.saves.front();
}

// The newest save made with Save in the current playthrough: what Save over
// replaces.
const saves::Save* LatestManual() {
  for (const auto& save : g_ui.saves)
    if (save.playthrough == g_ui.current.id && save.kind == "manual")
      return &save;
  return nullptr;
}

void Type(Typing typing, std::string text, Page from) {
  g_ui.typing = typing;
  g_ui.typing_from = from;
  g_ui.text = std::move(text);
  g_ui.fresh = true;
  g_ui.shift = g_ui.text.empty();
  g_ui.key_row = g_ui.key_column = 0;
  g_ui.page = Page::kKeyboard;
  TakeText(true, true);
}

void Question(Ask ask, std::string question, const saves::Save& subject, Page from) {
  g_ui.ask = ask;
  g_ui.ask_from = from;
  g_ui.question = std::move(question);
  g_ui.subject = subject;
  // Deleting starts on No.
  g_ui.yes = ask != Ask::kDelete;
  g_ui.page = Page::kConfirm;
  TakeText(false);
}

void Browse() {
  g_ui.page = Page::kBrowse;
  g_ui.all = g_ui.mode == Mode::kFrontEnd;
  g_ui.selected = 0;
  g_ui.top = 0;
  g_ui.dirty = true;
  TakeText(true);
}

// Loads a save, from wherever the screen is.
void Load(const saves::Save& save) {
  Sound("ui_select_a_button");
  const bool exact = saves::Exact(save);
  if (g_ui.mode == Mode::kPause) {
    if (!exact) {
      // Checkpoint saves load through the game's own Saved Games: out of the
      // level with Exit Game first.
      saves::Track(true);
      Begin(Flow::kLoadExit, save);
      Next(Stage::kExitRow, 15s);
      return;
    }
    if (!saves::Read(save.id)) {
      Notify("Couldn't read that save");
      return;
    }
    Begin(Flow::kLoadHere, save);
    Next(Stage::kPut, 60s);
    return;
  }
  Begin(Flow::kLoad, save);
  g_drive.checkpoint = !exact;
  if (g_drive.checkpoint && !modkit_game::BeginCheckpoint(save.folder)) {
    Finish(false, "Couldn't start checking that save's mods");
    return;
  }
  Next(Stage::kSources, 60s);
}

void NewGame(const std::string& name) {
  modkit_game::CancelRestore();
  saves::BackupSlot();
  const auto playthrough = saves::NewPlaythrough(name);
  saves::Track(true);
  saves::Save save;
  save.name = name;
  save.playthrough = playthrough.id;
  save.playthrough_name = playthrough.name;
  Begin(Flow::kNewGame, save);
  Next(Stage::kSlots, 20s);
}

void GameMenu(const saves::Save& save) {
  Begin(Flow::kGameMenu, save);
  g_drive.checkpoint = true;
  if (!modkit_game::BeginCheckpoint(save.folder)) {
    Finish(false, "Couldn't start checking that save's mods");
    return;
  }
  Next(Stage::kSources, 60s);
}

// --- The game's own menu ----------------------------------------------------
// Keep the original movie, TV, text children and selection animation. APT's
// duplicateMovieClip cannot reproduce these native text children, so pages use
// the shipped references and restore them before handing control to the game.

enum class Action {
  kNone,
  kContinue,
  kNewGame,
  kBrowse,
  kGameMenu,
  kClassic,
  kNewSave,
  kOverwrite,
  kMore,
  kSave,
  kBack,
  kLoad,
  kRename,
  kDelete,
  kSearch,
  kFilter,
  kClear,
  kCancel,
  kConfirm,
  kKey
};
struct Row {
  std::string label, help;
  Action action = Action::kNone;
  int index = 0;
};
struct Reference {
  std::string name, text, variable;
  float x = 0, y = 0, sx = 100, sy = 100;
  bool visible = false;
};
struct NativeMenu {
  uint32_t screen = 0, strings = 0;
  std::string movie, name, signature, initial;
  std::vector<std::string> buttons, ids, removed;
  std::vector<Reference> refs;
  std::vector<Row> rows;
  std::array<float, 4> row_y{};
  std::map<uint32_t, int> text_rows;
  std::map<uint32_t, uint32_t> fonts;
  int focus = 0;
  uint64_t frame = 0, drawn_frame = ~uint64_t(0);
  std::array<float, 6> title_transform{}, widget_relative{};
  float title_center = 0;
  uint64_t title_frame = ~uint64_t(0);
  bool widget_anchored = false;
  int selection = 0, main_offset = 0, browse_offset = 0;
  bool numbers = false;
  std::string notice;
  Clock::time_point notice_until{};
  bool load_error = false;
};
NativeMenu g_menu;
std::atomic<bool> g_keyboard{false};
constexpr size_t kRows = 5, kStringSize = 512;
constexpr int kPageRows = int(kRows) - 1;
constexpr const char* kLetters[] = {"qwertyuiop", "asdfghjkl'", "zxcvbnm,.-"};
constexpr const char* kNumbers[] = {"1234567890", "!@#$%&*()'", "+=/?_:,.-\""};

void TextBuffer(size_t index, std::string_view text) {
  if (!g_menu.strings)
    return;
  const size_t length = std::min(text.size(), kStringSize - 1);
  auto* target = simpsons::GuestToHost(g_runtime->virtual_membase(),
                                       g_menu.strings + uint32_t(index * kStringSize));
  std::memcpy(target, text.data(), length);
  target[length] = 0;
}

std::vector<std::string> Array(std::string_view path) {
  std::vector<std::string> result;
  const int count = std::clamp(g_apt->Number(std::string(path) + ".length"), 0, 32);
  for (int i = 0; i < count; ++i)
    result.push_back(g_apt->Text(std::string(path) + "." + std::to_string(i)));
  return result;
}

void RestoreNativeMenu(PPCContext& ctx, uint8_t* base) {
  if (!g_menu.screen)
    return;
  g_keyboard = false;
  const uint32_t strings = g_menu.strings;
  if (LoadGuestU32(base, g_menu.screen + 28) == 1) {
    g_apt->Bind(ctx, base, g_menu.movie);
    g_apt->Invoke("stopKeyListener");
    g_apt->Invoke("destroyActiveMenuButtons");
    for (size_t i = 0; i < kRows; ++i)
      g_apt->Invoke(g_menu.name + ".nativeSaveButton" + std::to_string(i) + ".removeMovieClip");
    for (const auto& ref : g_menu.refs) {
      const std::string path = g_menu.name + "." + ref.name;
      g_apt->SetText(path + ".text_entry.variable", ref.variable);
      g_apt->SetText(path + ".text_entry.text", ref.text);
    }
    std::vector<std::string> references;
    for (const auto& ref : g_menu.refs)
      references.push_back(ref.name);
    g_apt->SetArray(g_menu.name + ".TextRefs", references);
    g_apt->SetArray(g_menu.name + ".MenuItemButtons", g_menu.buttons);
    g_apt->SetArray(g_menu.name + ".MenuItemIds", g_menu.ids);
    g_apt->SetArray(g_menu.name + ".RemoveItems", g_menu.removed);
    g_apt->SetText("InitialSelection", g_menu.initial);
    g_apt->Invoke("initializeButtons");
    const std::array<std::string, 1> select{std::to_string(g_menu.selection)};
    g_apt->Invoke("activateMenuButtons", select);
    g_apt->SetNumber(g_menu.name + ".currentSelection", g_menu.selection);
    // initializeButtons lays out the rows again. Restore their captured
    // geometry afterwards, including the pause menu's tighter spacing.
    for (const auto& ref : g_menu.refs) {
      const std::string path = g_menu.name + "." + ref.name;
      g_apt->SetReal(path + "._x", ref.x);
      g_apt->SetReal(path + "._y", ref.y);
      g_apt->SetReal(path + "._xscale", ref.sx);
      g_apt->SetReal(path + "._yscale", ref.sy);
      g_apt->SetNumber(path + "._visible", ref.visible);
    }
    g_apt->Invoke("startKeyListener");
  }
  REXLOG_INFO("[saves-ui] restored native menu {}", g_menu.name);
  auto notice = std::move(g_menu.notice);
  const auto notice_until = g_menu.notice_until;
  const bool load_error = g_menu.load_error && Clock::now() < notice_until;
  g_menu = {};
  if (load_error) {
    g_menu.notice = std::move(notice);
    g_menu.notice_until = notice_until;
    g_menu.load_error = true;
  }
  g_menu.strings = strings;
  TakeText(false);
  InvalidateGameMenuPointer();
}

bool BindNativeMenu(PPCContext& ctx, uint8_t* base) {
  const GamePage page = ReadPage(ctx, base);
  if (g_menu.screen) {
    if (page.screen == g_menu.screen && page.name == g_menu.name) {
      g_apt->Bind(ctx, base, g_menu.movie);
      return true;
    }
    RestoreNativeMenu(ctx, base);
    Close();
    return false;
  }
  if (!page.screen || page.name.empty() || page.input == kPopupInput)
    return false;
  const uint32_t handle = LoadGuestU32(base, page.screen + 96);
  const uint32_t path = handle ? LoadGuestU32(base, handle) : 0;
  if (!path)
    return false;
  const std::string movie(reinterpret_cast<const char*>(simpsons::GuestToHost(base, path)));
  g_apt->Bind(ctx, base, movie);
  const auto names = Array(page.name + ".TextRefs");
  if (names.size() < 4)
    return false;
  for (const auto& name : names)
    if (!g_apt->Get(page.name + "." + name + ".text_entry"))
      return false;
  g_menu.screen = page.screen;
  g_menu.movie = movie;
  g_menu.name = page.name;
  g_menu.selection = page.selection;
  g_menu.buttons = Array(page.name + ".MenuItemButtons");
  g_menu.ids = Array(page.name + ".MenuItemIds");
  g_menu.removed = Array(page.name + ".RemoveItems");
  g_menu.initial = g_apt->Text("InitialSelection");
  for (const auto& name : names) {
    const std::string ref = page.name + "." + name;
    g_menu.refs.push_back({name, g_apt->Text(ref + ".text_entry.text"),
                           g_apt->Text(ref + ".text_entry.variable"), g_apt->Real(ref + "._x"),
                           g_apt->Real(ref + "._y"), g_apt->Real(ref + "._xscale"),
                           g_apt->Real(ref + "._yscale"), g_apt->Number(ref + "._visible") != 0});
  }
  if (!g_menu.strings)
    g_menu.strings = host_memory::Alloc(uint32_t(kStringSize * (8 + kRows)));
  TextBuffer(2, "[RDRight]");
  TextBuffer(3, "Back");
  TextBuffer(5, "[RDUp]");
  TextBuffer(7, "");
  g_apt->SetNumber(g_menu.name + ".currentSelection", 0);
  REXLOG_INFO("[saves-ui] native menu {} in {}, {} text references", page.name, movie,
              g_menu.refs.size());
  return true;
}

const saves::Save* SelectedSave() {
  return g_ui.selected >= 0 && g_ui.selected < int(g_ui.list.size())
             ? &g_ui.list[size_t(g_ui.selected)]
             : nullptr;
}

void Notice(std::string text, bool load_error) {
  g_menu.notice = std::move(text);
  g_menu.notice_until = Clock::now() + (load_error ? 6s : 3s);
  g_menu.load_error = load_error;
  g_menu.signature.clear();
}

void DoneTyping() {
  std::string text = g_ui.text;
  while (!text.empty() && text.back() == ' ')
    text.pop_back();
  while (!text.empty() && text.front() == ' ')
    text.erase(text.begin());
  if (text.empty() && g_ui.typing != Typing::kSearch) {
    Notice("Enter a name first");
    return;
  }
  const Typing typing = g_ui.typing;
  g_ui.typing = Typing::kNone;
  g_ui.page = g_ui.typing_from;
  TakeText(g_ui.page == Page::kBrowse);
  switch (typing) {
    case Typing::kNewGame:
      if (!g_ui.saves.empty()) {
        saves::Save save;
        save.name = text;
        Question(Ask::kNewGame, "Start " + text + "? Other playthroughs stay saved.", save,
                 Page::kMain);
      } else {
        NewGame(text);
      }
      break;
    case Typing::kNewSave:
      g_ui.save_name = text;
      g_ui.save_replace.clear();
      g_ui.save_now = true;
      break;
    case Typing::kRename:
      Notice(saves::Rename(g_ui.target, text) ? "Renamed to " + text : "Couldn't rename that save");
      g_ui.dirty = g_ui.reload = true;
      break;
    case Typing::kSearch:
      g_ui.search = text;
      g_ui.dirty = true;
      g_ui.selected = g_menu.browse_offset = 0;
      break;
    case Typing::kNone:
      break;
  }
}

void AddText(std::string_view text) {
  if (g_ui.fresh) {
    g_ui.text.clear();
    g_ui.fresh = false;
  }
  for (const char c : text)
    if (c >= 32 && c < 127 && g_ui.text.size() < kNameLength)
      g_ui.text += c;
}
void EraseText() {
  if (g_ui.fresh)
    g_ui.text.clear();
  else if (!g_ui.text.empty())
    g_ui.text.pop_back();
  g_ui.fresh = false;
}

void Confirm() {
  const Ask ask = g_ui.ask;
  g_ui.ask = Ask::kNone;
  g_ui.page = g_ui.ask_from;
  TakeText(g_ui.page == Page::kBrowse);
  switch (ask) {
    case Ask::kOverwrite:
      g_ui.save_name = g_ui.subject.name;
      g_ui.save_replace = g_ui.subject.id;
      g_ui.save_now = true;
      break;
    case Ask::kLoad:
      Load(g_ui.subject);
      break;
    case Ask::kDelete:
      Notice(saves::Remove(g_ui.subject.id) ? "Deleted " + g_ui.subject.name
                                            : "Couldn't delete that save");
      g_ui.page = Page::kBrowse;
      g_ui.dirty = g_ui.reload = true;
      break;
    case Ask::kNewGame:
      NewGame(g_ui.subject.name);
      break;
    case Ask::kNone:
      break;
  }
}

void Back() {
  Sound("ui_back_b_button");
  switch (g_ui.page) {
    case Page::kKeyboard:
      g_ui.page = g_ui.typing_from;
      g_ui.typing = Typing::kNone;
      break;
    case Page::kConfirm:
      g_ui.page = g_ui.ask_from;
      g_ui.ask = Ask::kNone;
      break;
    case Page::kDetails:
    case Page::kLibraryOptions:
      g_ui.page = Page::kBrowse;
      break;
    case Page::kBrowse:
      g_ui.page = Page::kMain;
      g_ui.search.clear();
      g_ui.dirty = true;
      break;
    case Page::kMain:
      if (g_menu.main_offset) {
        g_menu.main_offset = 0;
      } else {
        // The front-end's Back returns to its original slots for this visit;
        // pause/Options returns to the screen that opened the library.
        g_ui.classic = g_ui.mode == Mode::kFrontEnd;
        Close();
      }
      break;
  }
  TakeText(g_ui.page == Page::kBrowse);
  g_menu.signature.clear();
}

void Key(int row, int column) {
  g_ui.key_row = std::clamp(row, 0, 3);
  g_ui.key_column = std::clamp(column, 0, row == 3 ? 4 : 9);
  if (row == 3) {
    switch (column) {
      case 0:
        AddText(" ");
        break;
      case 1:
        g_menu.numbers = !g_menu.numbers;
        break;
      case 2:
        g_ui.shift = !g_ui.shift;
        break;
      case 3:
        EraseText();
        break;
      case 4:
        DoneTyping();
        break;
    }
  } else {
    char c = (g_menu.numbers ? kNumbers : kLetters)[g_ui.key_row][g_ui.key_column];
    if (g_ui.shift)
      c = char(std::toupper(uint8_t(c)));
    AddText(std::string_view(&c, 1));
    if (std::isalpha(uint8_t(c)))
      g_ui.shift = false;
  }
}

void Activate(const Row& row) {
  Sound("ui_select_a_button");
  const saves::Save* latest = Latest();
  const saves::Save* save = SelectedSave();
  switch (row.action) {
    case Action::kContinue:
      if (latest)
        Load(*latest);
      break;
    case Action::kNewGame:
      Type(Typing::kNewGame, fmt::format("Playthrough {}", saves::Playthroughs().size() + 1),
           Page::kMain);
      break;
    case Action::kBrowse:
      Browse();
      g_menu.browse_offset = 0;
      break;
    case Action::kGameMenu:
      if (latest)
        GameMenu(*latest);
      break;
    case Action::kClassic:
      g_ui.classic = true;
      saves::Track(false);
      Close();
      break;
    case Action::kNewSave: {
      const std::tm now = LocalTime(int64_t(std::time(nullptr)));
      char clock[16];
      std::strftime(clock, sizeof(clock), "%H:%M", &now);
      Type(Typing::kNewSave,
           g_ui.location.empty() ? fmt::format("Save {}", clock)
                                 : fmt::format("{} {}", g_ui.location, clock),
           Page::kMain);
      break;
    }
    case Action::kOverwrite:
      if (const auto* manual = LatestManual())
        Question(Ask::kOverwrite, "Save over " + manual->name + "?", *manual, Page::kMain);
      break;
    case Action::kMore:
      if (g_ui.page == Page::kMain)
        g_menu.main_offset = row.index;
      else
        g_menu.browse_offset = row.index;
      break;
    case Action::kSave:
      g_ui.selected = row.index;
      g_ui.page = Page::kDetails;
      TakeText(false);
      break;
    case Action::kBack:
    case Action::kCancel:
      Back();
      break;
    case Action::kLoad:
      if (save) {
        if (g_ui.mode == Mode::kPause)
          Question(Ask::kLoad, "Load " + save->name + "? Unsaved progress will be lost.", *save,
                   Page::kDetails);
        else
          Load(*save);
      }
      break;
    case Action::kRename:
      if (save) {
        g_ui.target = save->id;
        Type(Typing::kRename, save->name, Page::kDetails);
      }
      break;
    case Action::kDelete:
      if (save)
        Question(Ask::kDelete, "Delete " + save->name + "? This cannot be undone.", *save,
                 Page::kDetails);
      break;
    case Action::kSearch:
      Type(Typing::kSearch, g_ui.search, Page::kBrowse);
      break;
    case Action::kFilter:
      g_ui.all = !g_ui.all;
      g_ui.dirty = true;
      g_menu.browse_offset = 0;
      break;
    case Action::kClear:
      g_ui.search.clear();
      g_ui.dirty = true;
      g_menu.browse_offset = 0;
      break;
    case Action::kConfirm:
      Confirm();
      break;
    case Action::kKey:
      Key(row.index, g_ui.key_column);
      break;
    case Action::kNone:
      break;
  }
  g_menu.signature.clear();
}

// Keep a save's picture while it is read again, but never show another
// save's picture in its place.
std::shared_ptr<const canvas::Picture> g_last_picture;
std::string g_last_picture_save;
std::shared_ptr<const canvas::Picture> Steady(const saves::Save* save) {
  const std::string id = save ? save->id : std::string{};
  if (id != g_last_picture_save) {
    g_last_picture_save = id;
    g_last_picture.reset();
  }
  auto picture = save ? saves::Picture(*save) : nullptr;
  if (picture)
    return g_last_picture = picture;
  return save ? g_last_picture : nullptr;
}

std::string Detail(const saves::Save& save) {
  return fmt::format("{}  -  {}  -  {}%", save.location, When(save.created), int(save.completion));
}

std::vector<Row> Rows() {
  std::vector<Row> rows;
  switch (g_ui.page) {
    case Page::kMain: {
      if (!g_catalog.ready)
        return {{"Reading saves...", "Reading the save library", Action::kNone}};
      const auto* latest = Latest();
      if (g_ui.mode == Mode::kFrontEnd) {
        if (latest)
          rows.push_back({"Continue", Detail(*latest), Action::kContinue});
        rows.push_back(
            {"New Game", "Name a new playthrough; your other saves stay saved", Action::kNewGame});
        if (!g_ui.saves.empty())
          rows.push_back({"Load Game", "Choose a save from your library", Action::kBrowse});
        if (latest)
          rows.push_back({"Game Menu", "Episodes, movies, extras and options", Action::kGameMenu});
        rows.push_back({"Classic Slots", "The original slots A to D", Action::kClassic});
      } else {
        if (g_ui.can_save) {
          rows.push_back({"New Save", "Save this moment in " + g_ui.location, Action::kNewSave});
          if (const auto* manual = LatestManual())
            rows.push_back({"Save Over", "Replace " + manual->name, Action::kOverwrite});
        } else {
          rows.push_back({"Saving...", "Wait for the current save to finish", Action::kNone});
        }
        rows.push_back({"Load Game", "Choose a save from your library", Action::kBrowse});
        rows.push_back({"Back", "Return to the game menu", Action::kBack});
      }
      if (rows.size() > kRows) {
        const int offset = std::clamp(g_menu.main_offset, 0, int(rows.size()) - 1);
        const int end = std::min(offset + 3, int(rows.size()));
        std::vector<Row> shown(rows.begin() + offset, rows.begin() + end);
        shown.push_back({end == int(rows.size()) ? "First Page" : "More", "More save options",
                         Action::kMore, end == int(rows.size()) ? 0 : end});
        return shown;
      }
      break;
    }
    case Page::kBrowse: {
      const int count = int(g_ui.list.size());
      if (!count)
        return {{g_ui.search.empty() ? "No Saves Yet" : "No Matching Saves", "Use Search or Back",
                 Action::kNone},
                {"Search", "Find a save by name, playthrough or level", Action::kSearch},
                {"Back", "Return to save options", Action::kBack}};
      g_menu.browse_offset = std::clamp(g_menu.browse_offset, 0, std::max(0, count - 1));
      const int end =
          std::min(g_menu.browse_offset + (count > int(kRows) ? kPageRows : int(kRows)), count);
      for (int i = g_menu.browse_offset; i < end; ++i) {
        const auto& save = g_ui.list[size_t(i)];
        rows.push_back({save.name, Detail(save), Action::kSave, i});
      }
      if (count > int(kRows))
        rows.push_back({fmt::format("More Saves  {}/{}", g_menu.browse_offset / kPageRows + 1,
                                    (count + kPageRows - 1) / kPageRows),
                        "Next page of saves", Action::kMore, end == count ? 0 : end});
      break;
    }
    case Page::kDetails:
      if (const auto* save = SelectedSave()) {
        rows = {{"Load", save->name + " - " + Detail(*save), Action::kLoad},
                {"Rename", "Change the name of " + save->name, Action::kRename},
                {"Delete", "Delete " + save->name + "; confirmation required", Action::kDelete},
                {"Back", "Return to the save library", Action::kBack}};
      } else {
        g_ui.page = Page::kBrowse;
        return Rows();
      }
      break;
    case Page::kLibraryOptions:
      rows = {
          {"Search", "Find a save by name, playthrough or level", Action::kSearch},
          {g_ui.all ? "All Playthroughs" : "This Playthrough",
           "Select to change the playthrough filter", Action::kFilter},
          {"Clear Search", g_ui.search.empty() ? "No search filter" : "Searching: " + g_ui.search,
           Action::kClear},
          {"Back", "Return to the save library", Action::kBack}};
      break;
    case Page::kConfirm:
      rows = {{"Cancel", g_ui.question, Action::kCancel},
              {g_ui.ask == Ask::kDelete      ? "Delete"
               : g_ui.ask == Ask::kOverwrite ? "Overwrite"
               : g_ui.ask == Ask::kNewGame   ? "Start New Game"
                                             : "Load",
               g_ui.question, Action::kConfirm}};
      break;
    case Page::kKeyboard:
      for (int row = 0; row < 3; ++row) {
        std::string label;
        for (int column = 0; column < 10; ++column) {
          char c = (g_menu.numbers ? kNumbers : kLetters)[row][column];
          if (g_ui.shift)
            c = char(std::toupper(uint8_t(c)));
          if (column)
            label += "  ";
          label += c;
        }
        rows.push_back({std::move(label), g_ui.text + "_", Action::kKey, row});
      }
      {
        std::string label;
        const std::array<std::string, 5> keys{"Space", g_menu.numbers ? "ABC" : "123", "Shift",
                                              "Erase", "Done"};
        for (int i = 0; i < 5; ++i) {
          if (i)
            label += "   ";
          label += keys[size_t(i)];
        }
        rows.push_back({std::move(label), g_ui.text + "_", Action::kKey, 3});
      }
      break;
  }
  return rows;
}

std::string Title() {
  switch (g_ui.page) {
    case Page::kMain:
      return g_ui.mode == Mode::kFrontEnd ? "Saved Games" : "Save Game";
    case Page::kBrowse:
      return "Load Game";
    case Page::kDetails:
      return "Saved Game";
    case Page::kLibraryOptions:
      return "Find Saves";
    case Page::kConfirm:
      return g_ui.ask == Ask::kDelete      ? "Delete Save"
             : g_ui.ask == Ask::kOverwrite ? "Overwrite Save"
             : g_ui.ask == Ask::kNewGame   ? "New Game"
                                           : "Load Game";
    case Page::kKeyboard:
      return g_ui.typing == Typing::kRename    ? "Rename Save"
             : g_ui.typing == Typing::kSearch  ? "Search Saves"
             : g_ui.typing == Typing::kNewSave ? "Name Your Save"
                                               : "Name Your Playthrough";
  }
  return "Saved Games";
}

void RebuildNativeMenu(int selection = 0) {
  g_keyboard = g_ui.page == Page::kKeyboard;
  g_menu.rows = Rows();
  for (size_t i = 0; i < kRows; ++i)
    TextBuffer(8 + i, i < g_menu.rows.size() ? g_menu.rows[i].label : "");
  std::string signature = std::to_string(int(g_ui.page));
  for (const auto& row : g_menu.rows)
    signature += "\n" + row.label;
  const auto title = Title();
  TextBuffer(0, title.size() > 36 ? title.substr(0, 33) + "..." : title);
  TextBuffer(4, g_ui.page == Page::kKeyboard ? "Type" : "Select");
  TextBuffer(6, g_ui.page == Page::kKeyboard ? "Done"
                : g_ui.page == Page::kBrowse ? "Find Saves"
                                             : "");
  if (signature == g_menu.signature) {
    if (g_ui.page == Page::kKeyboard)
      g_apt->SetNumber(g_menu.name + ".currentSelection", g_ui.key_row);
    return;
  }
  g_menu.signature = signature;
  g_menu.text_rows.clear();
  InvalidateGameMenuPointer();
  g_apt->Invoke("stopKeyListener");
  g_apt->Invoke("destroyActiveMenuButtons");
  for (size_t i = 0; i < std::max(g_menu.buttons.size(), kRows); ++i) {
    if (i < g_menu.buttons.size())
      g_apt->Invoke(g_menu.name + "." + g_menu.buttons[i] + ".removeMovieClip");
    g_apt->Invoke(g_menu.name + ".nativeSaveButton" + std::to_string(i) + ".removeMovieClip");
  }
  std::vector<std::string> refs, buttons, ids;
  for (size_t i = 0; i < std::max(g_menu.refs.size(), g_menu.rows.size()); ++i) {
    const auto& ref = g_menu.refs[std::min(i, g_menu.refs.size() - 1)];
    const std::string path = g_menu.name + "." + ref.name;
    g_apt->SetNumber(path + "._visible", 0);
    if (i >= g_menu.rows.size())
      continue;
    const std::string variable = g_menu.movie + ".NativeSaveLabel" + std::to_string(i);
    const std::string& label = g_menu.rows[i].label;
    g_apt->SetText(variable, label);
    g_apt->SetText(path + ".text_entry.variable", variable);
    g_apt->SetText(path + ".text_entry.text", label);
    // PickSlot's four text fields already have the TV's correct placement.
    // Other entry points use their first four shipped rows, spaced normally.
    g_apt->SetReal(path + "._xscale", ref.sx);
    g_apt->SetReal(path + "._yscale", ref.sy);
    g_apt->SetReal(path + "._y", -83.15f + std::min(i, size_t(3)) * 45.3f);
    refs.push_back(ref.name);
    buttons.push_back("nativeSaveButton" + std::to_string(i));
    ids.push_back("NativeSave" + std::to_string(i));
  }
  g_apt->SetArray(g_menu.name + ".TextRefs", refs);
  g_apt->SetArray(g_menu.name + ".MenuItemButtons", buttons);
  g_apt->SetArray(g_menu.name + ".MenuItemIds", ids);
  g_apt->SetArray(g_menu.name + ".RemoveItems", {});
  g_apt->SetText("InitialSelection", "");
  g_apt->Invoke("initializeButtons");
  selection = std::clamp(selection, 0, int(g_menu.rows.size()) - 1);
  const std::array<std::string, 1> args{std::to_string(selection)};
  g_apt->Invoke("activateMenuButtons", args);
  g_apt->SetNumber(g_menu.name + ".currentSelection", selection);
  g_apt->Invoke("startKeyListener");
  REXLOG_INFO("[saves-ui] native page {}: {} rows", int(g_ui.page), g_menu.rows.size());
}

void Move(int direction) {
  g_apt->Invoke(direction < 0 ? "menuMoveUp" : "menuMoveDown");
  Sound("ui_move_up_down");
}

void HandleNativeInput(const Input& in) {
  const int selection =
      std::clamp(g_apt->Number(g_menu.name + ".currentSelection"), 0, int(g_menu.rows.size()) - 1);
  if (in.back) {
    Back();
    return;
  }
  if (g_ui.page == Page::kKeyboard) {
    if (!in.text.empty())
      AddText(in.text);
    if (in.erase || in.x)
      EraseText();
    if (in.y || in.enter || in.start) {
      DoneTyping();
      return;
    }
    if (in.previous)
      g_ui.shift = !g_ui.shift;
    if (in.up)
      g_ui.key_row = (g_ui.key_row + 3) % 4;
    if (in.down)
      g_ui.key_row = (g_ui.key_row + 1) % 4;
    const int columns = g_ui.key_row == 3 ? 5 : 10;
    g_ui.key_column = std::clamp(g_ui.key_column, 0, columns - 1);
    if (in.left)
      g_ui.key_column = (g_ui.key_column + columns - 1) % columns;
    if (in.right)
      g_ui.key_column = (g_ui.key_column + 1) % columns;
    if (in.accept)
      Key(g_ui.key_row, g_ui.key_column);
    RebuildNativeMenu(g_ui.key_row);
    return;
  }
  if (g_ui.page == Page::kConfirm && (in.left || in.right))
    g_apt->SetNumber(g_menu.name + ".currentSelection", in.right ? 1 : 0);
  if (in.up || in.scroll < -.5f)
    Move(-1);
  if (in.down || in.scroll > .5f)
    Move(1);
  if (g_ui.page == Page::kBrowse) {
    if (!in.text.empty() || in.erase) {
      if (in.erase && !g_ui.search.empty())
        g_ui.search.pop_back();
      for (char c : in.text)
        if (c >= 32 && c < 127 && g_ui.search.size() < kNameLength)
          g_ui.search += c;
      g_ui.dirty = true;
      g_menu.browse_offset = 0;
    }
    if (in.y || in.find) {
      g_ui.page = Page::kLibraryOptions;
      TakeText(false);
    }
    if (in.previous || in.next) {
      const int pages = std::max(1, (int(g_ui.list.size()) + kPageRows - 1) / kPageRows);
      g_menu.browse_offset =
          ((g_menu.browse_offset / kPageRows + (in.next ? 1 : pages - 1)) % pages) * kPageRows;
    }
    if ((in.x || in.remove) && selection < int(g_menu.rows.size())) {
      const Row& row = g_menu.rows[size_t(selection)];
      if (row.action == Action::kSave) {
        g_ui.selected = row.index;
        Activate({{}, {}, in.remove ? Action::kDelete : Action::kRename});
      }
    }
  }
  if ((in.accept || in.enter) && selection < int(g_menu.rows.size()))
    Activate(g_menu.rows[size_t(selection)]);
}

void TickNativeMenu(PPCContext& ctx, uint8_t* base) {
  static Clock::time_point waiting{};
  if (!BindNativeMenu(ctx, base)) {
    if (waiting == Clock::time_point{})
      waiting = Clock::now();
    if (ReadSavesInput().back || Clock::now() - waiting > 2s) {
      g_ui.classic = g_ui.mode == Mode::kFrontEnd;
      Close();
      Notify("Couldn't open the save library here");
      waiting = {};
    }
    return;
  }
  waiting = {};
  ++g_menu.frame;
  const auto version = g_ui.catalog_version;
  Refresh();
  int selection = g_apt->Number(g_menu.name + ".currentSelection");
  if (g_ui.page == Page::kBrowse && version != g_ui.catalog_version && SelectedSave()) {
    g_menu.browse_offset =
        int(g_ui.list.size()) <= int(kRows) ? 0 : g_ui.selected / kPageRows * kPageRows;
    selection = g_ui.selected - g_menu.browse_offset;
  }
  g_ui.can_save = !saves::Busy() && !g_ui.save_now;
  const Page before = g_ui.page;
  RebuildNativeMenu(selection);
  const Input in = ReadSavesInput();
  HandleNativeInput(in);
  if (Current() != Screen::kSaves || g_drive.flow != Flow::kNone) {
    RestoreNativeMenu(ctx, base);
    return;
  }
  Refresh();
  RebuildNativeMenu(g_ui.page == Page::kKeyboard ? g_ui.key_row
                    : before != g_ui.page        ? 0
                                          : g_apt->Number(g_menu.name + ".currentSelection"));
  selection =
      std::clamp(g_apt->Number(g_menu.name + ".currentSelection"), 0, int(g_menu.rows.size()) - 1);
  g_menu.focus = selection;
  if (g_ui.page == Page::kBrowse && g_menu.rows[size_t(selection)].action == Action::kSave)
    g_ui.selected = g_menu.rows[size_t(selection)].index;
  std::string detail = Clock::now() < g_menu.notice_until ? g_menu.notice
                       : g_ui.page == Page::kKeyboard     ? g_ui.text + "_"
                                                          : g_menu.rows[size_t(selection)].help;
  if (detail.size() > 76)
    detail = detail.substr(0, 73) + "...";
  TextBuffer(1, detail);
}

}  // namespace

// --- game_ui.h ---------------------------------------------------------------

uint32_t SavesTextForText(const uint8_t* base, uint32_t text) {
  if (Current() != Screen::kSaves || !g_menu.screen || !g_menu.strings ||
      GameMenuPointerScreen() != g_menu.screen)
    return 0;
  const auto address = LoadGuestU32(base, text + 84);
  if (!address)
    return 0;
  const std::string_view raw(reinterpret_cast<const char*>(simpsons::GuestToHost(base, address)));
  int index = -1;
  // The four slot-letter fields and their matching statistics share their
  // native row centres. Use those centres to retain the real text children;
  // slot polling writes the statistic strings every frame, independently of
  // the menu button's ActionScript label.
  const auto transform = LoadGuestU32(base, 0x82D08BF4);
  const auto matrix = transform ? LoadGuestU32(base, transform + 4) : 0;
  float y = 0;
  if (matrix) {
    const float x_scale = simpsons::LoadGuestFloat(base, matrix);
    const float y_scale = simpsons::LoadGuestFloat(base, matrix + 4);
    const float scale = x_scale == 0   ? y_scale
                        : y_scale == 0 ? x_scale
                                       : std::hypot(x_scale, y_scale);
    y = simpsons::LoadGuestFloat(base, matrix + 52) +
        scale *
            (simpsons::LoadGuestFloat(base, text + 8) + simpsons::LoadGuestFloat(base, text + 16)) *
            .5f;
  }
  if (g_menu.name == "PickSlot" && raw.size() == 1 && raw[0] >= 'A' && raw[0] <= 'D')
    g_menu.row_y[size_t(raw[0] - 'A')] = y;
  const float width =
      simpsons::LoadGuestFloat(base, text + 12) - simpsons::LoadGuestFloat(base, text + 4);
  const float height =
      simpsons::LoadGuestFloat(base, text + 16) - simpsons::LoadGuestFloat(base, text + 8);
  int row = -1;
  const bool slot_text = width > 300 && height > 50;
  if (slot_text) {
    for (size_t i = 0; i < g_menu.rows.size(); ++i)
      if (raw == g_menu.rows[i].label ||
          raw == g_menu.movie + ".NativeSaveLabel" + std::to_string(i))
        row = int(i);
  }
  if (row < 0 && g_menu.name == "PickSlot" && matrix && width > 800 && height > 55 && height < 65 &&
      raw.find(':') != std::string_view::npos && raw.find('%') != std::string_view::npos) {
    float distance = 10000;
    for (int i = 0; i < 4; ++i) {
      const float gap = std::abs(y - g_menu.row_y[size_t(i)]);
      if (g_menu.row_y[size_t(i)] && gap < distance) {
        row = i;
        distance = gap;
      }
    }
    if (distance > height)
      row = -1;
  } else if (row < 0 && g_menu.name != "PickSlot") {
    for (size_t i = 0; i < g_menu.refs.size() && i < kRows; ++i)
      if (raw == g_menu.refs[i].text ||
          raw == g_menu.movie + ".NativeSaveLabel" + std::to_string(i))
        row = int(i);
  }
  if (row >= 0) {
    g_menu.text_rows[text] = row;
    return g_menu.strings + uint32_t((8 + row) * kStringSize);
  }
  if (raw == "$FE_MenuTitle_PauseMenu" || raw == "$FE_Title_ProfileChooser" ||
      raw == "$FE_Title_Pause" || raw == "$FE_Pause" || raw == "Pause" || raw == "Options" ||
      raw == "$FE_Options")
    index = 0;
  else if (g_menu.name == "PickSlot" && (raw == "A" || raw == "B" || raw == "C" || raw == "D"))
    index = 7;
  else if (raw == "$FE_Button_Delete" || raw == "$FE_Back_button")
    index = 2;
  else if (raw == "$FE_Button_DeleteText" || raw == "$FE_Back_text")
    index = 3;
  else if (g_ui.page == Page::kKeyboard && !GameMenuPointerUsesTouch() &&
           (KeyboardPrompts() || GameMenuPointerUsesKeyboard()) &&
           (raw == "$FE_Select_button" || raw == "$FE_Select_text"))
    index = 7;  // Enter finishes the name; it does not type the highlighted key.
  else if (raw == "$FE_Select_text")
    index = 4;
  else if (raw == "$FE_Button_ChangeStorageDevice")
    index = g_ui.page == Page::kKeyboard || g_ui.page == Page::kBrowse ? 5 : 7;
  else if (raw == "$FE_Button_ChangeStorageDeviceText")
    index = 6;
  else if (g_menu.name == "PickSlot" && !raw.starts_with("$") &&
           simpsons::LoadGuestFloat(base, text + 12) - simpsons::LoadGuestFloat(base, text + 4) >
               750 &&
           simpsons::LoadGuestFloat(base, text + 16) - simpsons::LoadGuestFloat(base, text + 8) <
               45)
    index = 7;  // The attached widget owns the details band.
  if (index == 0 && matrix) {
    const std::array<float, 6> anchor{
        simpsons::LoadGuestFloat(base, matrix),      simpsons::LoadGuestFloat(base, matrix + 4),
        simpsons::LoadGuestFloat(base, matrix + 16), simpsons::LoadGuestFloat(base, matrix + 20),
        simpsons::LoadGuestFloat(base, matrix + 48), simpsons::LoadGuestFloat(base, matrix + 52)};
    if (std::all_of(anchor.begin(), anchor.end(),
                    [](float value) { return std::isfinite(value); }) &&
        std::abs(anchor[0] * anchor[3] - anchor[1] * anchor[2]) > .0001f) {
      g_menu.title_transform = anchor;
      g_menu.title_center =
          (simpsons::LoadGuestFloat(base, text + 4) + simpsons::LoadGuestFloat(base, text + 12)) *
          .5f;
      g_menu.title_frame = g_menu.frame;
    }
  }
  return index < 0 ? 0 : g_menu.strings + uint32_t(index * kStringSize);
}

// A custom widget in the original APT menu's display list. Its vertices and
// hit targets use that text child's transform, rather than the final frame's
// screen coordinates. The TV, titles, footers and transitions remain the game.
bool DrawNativeSavesWidget(PPCContext& ctx, uint8_t* base, uint32_t text) {
  if (Current() != Screen::kSaves || !g_menu.screen)
    return false;
  const auto found = g_menu.text_rows.find(text);
  if (found == g_menu.text_rows.end())
    return false;
  if (found->second != 0 || g_menu.drawn_frame == g_menu.frame)
    return true;
  const auto transform = LoadGuestU32(base, 0x82D08BF4);
  const auto matrix = transform ? LoadGuestU32(base, transform + 4) : 0;
  if (!matrix)
    return false;
  const float width =
      simpsons::LoadGuestFloat(base, text + 12) - simpsons::LoadGuestFloat(base, text + 4);
  const float unit = width / 840.f;
  if (!(unit > 0 && std::isfinite(unit)))
    return false;
  const float left = simpsons::LoadGuestFloat(base, text + 4),
              top = simpsons::LoadGuestFloat(base, text + 8);
  const float a = simpsons::LoadGuestFloat(base, matrix),
              b = simpsons::LoadGuestFloat(base, matrix + 4);
  const float c = simpsons::LoadGuestFloat(base, matrix + 16),
              d = simpsons::LoadGuestFloat(base, matrix + 20);
  // Like native text, keep square pixels while inheriting the APT rotation.
  // The movie stage itself is stretched vertically for the television.
  std::array<float, 6> pose{a * unit,
                            b * unit,
                            -b * unit,
                            a * unit,
                            simpsons::LoadGuestFloat(base, matrix + 48) + a * left + c * top,
                            simpsons::LoadGuestFloat(base, matrix + 52) + b * left + d * top};
  for (float value : pose)
    if (!std::isfinite(value))
      return false;
  // Only the selected text child should pulse. Anchor the complete layout to
  // the title's APT transform so the photo, panels and keys stay steady, while
  // the screen still follows its original transition.
  if (g_menu.title_frame == g_menu.frame) {
    const auto& anchor = g_menu.title_transform;
    if (!g_menu.widget_anchored) {
      const float det = anchor[0] * anchor[3] - anchor[1] * anchor[2];
      const auto local = [&](float x, float y) {
        return std::array<float, 2>{(anchor[3] * x - anchor[2] * y) / det,
                                    (anchor[0] * y - anchor[1] * x) / det};
      };
      const auto right = local(pose[0], pose[1]), up = local(pose[2], pose[3]),
                 origin = local(pose[4] - anchor[4], pose[5] - anchor[5]);
      g_menu.widget_relative = {right[0], right[1], up[0], up[1], origin[0], origin[1]};
      // Centre the complete 840-unit layout on the title, rather than
      // inheriting the stock slot label's left margin.
      g_menu.widget_relative[4] = g_menu.title_center - right[0] * 420.f;
      g_menu.widget_anchored = true;
    }
    const auto& relative = g_menu.widget_relative;
    pose = {anchor[0] * relative[0] + anchor[2] * relative[1],
            anchor[1] * relative[0] + anchor[3] * relative[1],
            anchor[0] * relative[2] + anchor[2] * relative[3],
            anchor[1] * relative[2] + anchor[3] * relative[3],
            anchor[0] * relative[4] + anchor[2] * relative[5] + anchor[4],
            anchor[1] * relative[4] + anchor[3] * relative[5] + anchor[5]};
  }
  const PPCContext saved = ctx;
  bool painted = false;
  {
    canvas::Canvas paint(ctx, base);
    if (paint.valid()) {
      painted = true;
      canvas::WidgetScope scope(pose, {-28, -14, 856, 380});
      uint32_t title_font = paint.font(), body_font = paint.font();
      float largest = 0, nearest = 1e9f;
      for (uint32_t font : paint.Fonts()) {
        paint.UseFont(font);
        const float line = float(LoadGuestU32(base, font + 24));
        if (paint.Measure(line, "i") >= paint.Measure(line, "W") * .6f)
          continue;
        if (line > largest) {
          largest = line;
          title_font = font;
        }
        if (std::abs(line - 40) < nearest) {
          nearest = std::abs(line - 40);
          body_font = font;
        }
      }
      const auto outlined = [&](float x, float y, float size, uint32_t colour,
                                std::string_view label,
                                canvas::Align align = canvas::Align::kLeft) {
        for (const auto offset : {std::array<float, 2>{-1.5f, 0}, {1.5f, 0}, {0, -1.5f}, {0, 1.5f}})
          paint.Text(x + offset[0], y + offset[1], size, 0xFF15103A, label, align);
        paint.Text(x, y, size, colour, label, align);
      };
      const auto world = [&](float x, float y) {
        return std::array<float, 2>{pose[0] * x + pose[2] * y + pose[4],
                                    pose[1] * x + pose[3] * y + pose[5]};
      };
      const auto target = [&](float x, float y, float w, float h, int row, int column = -1,
                              int key = 0) {
        const auto p0 = world(x, y), p1 = world(x + w, y), p2 = world(x, y + h),
                   p3 = world(x + w, y + h);
        const float x0 = std::min({p0[0], p1[0], p2[0], p3[0]}),
                    y0 = std::min({p0[1], p1[1], p2[1], p3[1]});
        const float x1 = std::max({p0[0], p1[0], p2[0], p3[0]}),
                    y1 = std::max({p0[1], p1[1], p2[1], p3[1]});
        if (column >= 0)
          RecordGameMenuPointerSaveKey(x0, y0, x1 - x0, y1 - y0, row, column);
        else if (key)
          RecordGameMenuPointerButton(x0, y0, x1 - x0, y1 - y0, key);
        else
          RecordGameMenuPointerSaveRow(x0, y0, x1 - x0, y1 - y0, row);
      };
      constexpr uint32_t ink = 0xFF15103A, yellow = 0xFFFFD90F, wave = 0xFFA8DCFF;
      const int focus = std::clamp(g_menu.focus, 0, int(g_menu.rows.size()) - 1);
      if (g_ui.page == Page::kKeyboard) {
        constexpr float gap = 8, key_w = (840 - gap * 9) / 10, key_h = 52;
        for (int row = 0; row < 4; ++row) {
          const int columns = row == 3 ? 5 : 10;
          const float w = row == 3 ? (840 - gap * 4) / 5 : key_w;
          for (int column = 0; column < columns; ++column) {
            const float x = column * (w + gap), y = 48 + row * 64.f;
            const bool on = row == g_ui.key_row && column == g_ui.key_column;
            std::string label;
            if (row == 3) {
              const std::array<std::string, 5> words{"Space", g_menu.numbers ? "ABC" : "123",
                                                     "Shift", "Erase", "Done"};
              label = words[size_t(column)];
            } else {
              char ch = (g_menu.numbers ? kNumbers : kLetters)[row][column];
              label = char(g_ui.shift ? std::toupper(uint8_t(ch)) : ch);
            }
            paint.RoundRect(x - 2, y - 2, w + 4, key_h + 4, 11, on ? yellow : ink);
            paint.RoundRect(x, y, w, key_h, 9, on ? 0xFF34599E : 0xEC14244F);
            paint.UseFont(title_font);
            outlined(x + w * .5f, y + 11, 29, on ? yellow : 0xFFFFFFFF, label,
                     canvas::Align::kCenter);
            target(x - 3, y - 3, w + 6, key_h + 6, row, column);
          }
        }
        paint.RoundRect(0, -12, 840, 48, 10, 0xF0F7F8FF);
        paint.UseFont(body_font);
        std::string editing = g_ui.text;
        if (!g_ui.fresh) {
          bool clipped = false;
          while (!editing.empty() &&
                 paint.Measure(25, (clipped ? "..." : "") + editing + "_") > 740) {
            editing.erase(editing.begin());
            clipped = true;
          }
          if (clipped)
            editing = "..." + editing;
        }
        if (g_ui.fresh && !editing.empty())
          paint.RoundRect(12, -3, std::min(paint.Measure(25, editing), 740.f) + 8, 31, 5,
                          0xFFCAE6FF);
        paint.Text(16, -1, 25, 0xFF1B2440, paint.Fit(25, editing + (g_ui.fresh ? "" : "_"), 740));
        paint.Text(824, 4, 17, 0xFF506080, fmt::format("{}/{}", g_ui.text.size(), kNameLength),
                   canvas::Align::kRight);
        if (Clock::now() < g_menu.notice_until)
          paint.Text(420, 324, 21, yellow, paint.Fit(21, g_menu.notice, 820),
                     canvas::Align::kCenter);
      } else if (g_ui.page == Page::kConfirm) {
        paint.RoundRect(-12, 4, 864, 262, 18, 0xF00E1639);
        paint.Frame(-6, 10, 852, 250, 2, wave);
        paint.UseFont(title_font);
        const auto lines = paint.Wrap(30, g_ui.question, 760);
        for (size_t i = 0; i < lines.size() && i < 3; ++i)
          outlined(420, 34 + i * 36.f, 30, 0xFFFFFFFF, lines[i], canvas::Align::kCenter);
        for (int i = 0; i < 2; ++i) {
          const float x = i ? 452 : 92;
          paint.RoundRect(x - 2, 169, 300, 62, 12, focus == i ? yellow : ink);
          paint.RoundRect(x, 171, 296, 58, 10, focus == i ? 0xFF34599E : 0xFF17254E);
          outlined(x + 148, 183, 31, focus == i ? yellow : 0xFFFFFFFF, g_menu.rows[size_t(i)].label,
                   canvas::Align::kCenter);
          target(x - 6, 165, 308, 70, i);
        }
      } else {
        const bool browse = g_ui.page == Page::kBrowse;
        const float start_y = browse ? 42.f : 8.f;
        if (browse) {
          paint.RoundRect(-12, -7, 864, 38, 12, 0xEE14244F);
          paint.UseFont(body_font);
          outlined(
              8, 2, 22, 0xFFFFFFFF,
              paint.Fit(22, g_ui.search.empty() ? "Find saves" : "Search: " + g_ui.search, 570));
          paint.Text(830, 5, 18, wave, g_ui.all ? "All playthroughs" : "This playthrough",
                     canvas::Align::kRight);
          target(-12, -7, 864, 38, -1, -1, 9);
        }
        const float row_h = browse ? 52.f : 260.f / float(std::max<size_t>(g_menu.rows.size(), 1));
        for (size_t i = 0; i < g_menu.rows.size(); ++i) {
          const Row& row = g_menu.rows[i];
          const float y = start_y + row_h * i;
          const bool selected = int(i) == focus;
          paint.RoundRect(-14, y - 2, 420, row_h - 6, 10, selected ? 0xE627427E : 0xB012244D);
          if (selected) {
            paint.Circle(-1, y + row_h * .42f, 9, 0xFFE8A33D);
            paint.Circle(-1, y + row_h * .42f - 1, 7, 0xFFF06292);
            paint.Circle(-1, y + row_h * .42f - 1, 2.5f, 0xFF2A4F9B);
          }
          paint.UseFont(title_font);
          const float size = browse ? 28.f : g_menu.rows.size() > 4 ? 30.f : 34.f;
          outlined(18, y + 1, size, selected ? yellow : 0xFFE2C93A,
                   paint.Fit(size, row.label, 370));
          std::string detail = row.help;
          if (row.action == Action::kSave) {
            const auto& save = g_ui.list[size_t(row.index)];
            detail = When(save.created) + " - " + save.location;
          }
          paint.UseFont(body_font);
          paint.Text(18, y + row_h - 22, 16, 0xFFC9CCF5, paint.Fit(16, detail, 370));
          paint.Line(-6, y + row_h - 5, 402, y + row_h - 5, 1.5f, wave);
          target(-20, y - 3, 430, row_h - 1, int(i));
        }
        const saves::Save* preview =
            g_ui.page == Page::kBrowse || g_ui.page == Page::kDetails ? SelectedSave() : Latest();
        if (g_ui.page == Page::kMain && g_menu.rows[size_t(focus)].action == Action::kOverwrite)
          preview = LatestManual();
        const Action selected_action = g_menu.rows[size_t(focus)].action;
        if (g_ui.page == Page::kMain &&
            (selected_action == Action::kNewGame || selected_action == Action::kClassic))
          preview = nullptr;
        const float photo_offset = browse ? 30.f : 0.f;
        std::shared_ptr<const canvas::Picture> picture =
            g_ui.mode == Mode::kPause && g_ui.page == Page::kMain &&
                    g_menu.rows[size_t(focus)].action == Action::kNewSave
                ? saves::PausePicture()
                : Steady(preview);
        paint.RoundRect(432, 14 + photo_offset, 410, 256, 12, 0xC012244D);
        paint.Rect(447, 27 + photo_offset, 384, 219, 0x70000000);
        paint.Rect(441, 21 + photo_offset, 384, 219, ink);
        paint.Rect(444, 24 + photo_offset, 378, 213, 0xFFFFFFFF);
        paint.Rect(451, 31 + photo_offset, 364, 199, ink);
        bool image = false;
        if (picture && picture->width && picture->height) {
          const float fit = std::min(364.f / picture->width, 199.f / picture->height);
          const float w = picture->width * fit, h = picture->height * fit;
          image = paint.Image(*picture, 451 + (364 - w) * .5f, 31 + photo_offset + (199 - h) * .5f,
                              w, h);
        }
        if (!image) {
          paint.Rect(451, 31 + photo_offset, 364, 199, 0xFF284D8B);
          paint.Circle(633, 124 + photo_offset, 24, 0xFFE8A33D);
          paint.Circle(633, 122 + photo_offset, 20, 0xFFF06292);
          paint.Circle(633, 122 + photo_offset, 7, 0xFF284D8B);
        }
        paint.UseFont(body_font);
        paint.Text(636, 245 + photo_offset, 18, 0xFFFFFFFF,
                   paint.Fit(18,
                             preview                               ? preview->name
                             : selected_action == Action::kNewGame ? "Start a new playthrough"
                             : selected_action == Action::kClassic ? "The original slots A to D"
                                                                   : "Save this moment",
                             380),
                   canvas::Align::kCenter);
      }
      if (g_ui.page != Page::kKeyboard) {
        const float band_y = g_ui.page == Page::kBrowse ? 324.f : 294.f;
        paint.Rect(-18, band_y, 876, 50, 0xE00E1639);
        paint.Line(-18, band_y, 858, band_y, 2, wave);
        paint.Line(-18, band_y + 50, 858, band_y + 50, 2, wave);
        paint.UseFont(body_font);
        const auto* selected = SelectedSave();
        const std::string detail = Clock::now() < g_menu.notice_until ? g_menu.notice
                                   : g_ui.page == Page::kConfirm      ? "Choose an option"
                                                                 : g_menu.rows[size_t(focus)].help;
        paint.Text(420, band_y + 6, 22, 0xFFFFFFFF, paint.Fit(22, detail, 840),
                   canvas::Align::kCenter);
        paint.Text(420, band_y + 31, 16, wave,
                   selected && (g_ui.page == Page::kBrowse || g_ui.page == Page::kDetails)
                       ? (saves::Exact(*selected) ? "Carries on from the saved moment"
                                                  : "Carries on from its checkpoint")
                       : "Your other saves stay saved",
                   canvas::Align::kCenter);
      }
    }
  }
  ctx = saved;
  if (painted)
    g_menu.drawn_frame = g_menu.frame;
  return painted;
}

bool SavesKeyboard() {
  return Current() == Screen::kSaves && g_keyboard.load();
}

bool SavesMenuEvent(PPCContext& ctx, uint8_t* base, uint32_t screen, int key) {
  if (Current() != Screen::kSaves || screen != g_menu.screen)
    return false;
  modkit_game::Context context(ctx, base);
  g_apt->Bind(ctx, base, g_menu.movie);
  Input in;
  in.up = key == 4;
  in.down = key == 5;
  in.left = key == 2;
  in.right = key == 3;
  in.accept = key == 6;
  in.back = key == 7;
  in.previous = key == 10;
  in.next = key == 11;
  in.x = key == 12;
  in.y = key == 13;
  in.start = key == 0;
  HandleNativeInput(in);
  if (Current() != Screen::kSaves)
    RestoreNativeMenu(ctx, base);
  else
    RebuildNativeMenu(g_ui.page == Page::kKeyboard ? g_ui.key_row : 0);
  return true;
}

int SavesKeyboardRow(uint32_t text) {
  if (!SavesKeyboard())
    return -1;
  const auto found = g_menu.text_rows.find(text);
  return found == g_menu.text_rows.end() ? -1 : found->second;
}

bool SavesTextStyle(PPCContext& ctx, const uint8_t* base, uint32_t text, uint32_t replacement,
                    float* scale, uint32_t* colour) {
  if (Current() != Screen::kSaves || !replacement || !g_menu.strings)
    return false;
  const int field = int((replacement - g_menu.strings) / kStringSize);
  if (replacement < g_menu.strings || field < 0 || field >= int(8 + kRows) ||
      (field > 1 && field < 8))
    return false;
  const int row = field - 8;
  if (row >= int(g_menu.rows.size()))
    return false;
  std::string label = field == 0 ? Title()
                      : field == 1
                          ? reinterpret_cast<const char*>(simpsons::GuestToHost(base, replacement))
                          : g_menu.rows[size_t(row)].label;
  *scale = row >= 0 && g_ui.page == Page::kKeyboard ? .8f : 1.f;
  if (row >= 0)
    *colour = row == (g_ui.page == Page::kKeyboard ? g_ui.key_row : g_menu.focus) ? 0xFFFFFFFF
                                                                                  : 0xFFE2C93A;
  // Resolve the same font as the native text renderer, then use its glyph
  // advances. APT's textWidth/textHeight are not valid for these text children.
  const uint32_t name = LoadGuestU32(base, text + 112);
  auto& font = g_menu.fonts[name];
  if (!font) {
    const auto manager = LoadGuestU32(base, 0x82D08EA8);
    const PPCContext saved = ctx;
    ctx.r1.u64 -= 512;
    simpsons::StoreGuestU32(const_cast<uint8_t*>(base), ctx.r1.u32, saved.r1.u32);
    ctx.r3.u64 = manager;
    ctx.r4.u64 = name;
    sub_826BFFC8(ctx, const_cast<uint8_t*>(base));
    ctx = saved;
    font = manager ? LoadGuestU32(base, manager + 28) : 0;
  }
  const uint32_t table = font ? LoadGuestU32(base, font + 36) : 0;
  const float line = font ? float(LoadGuestU32(base, font + 24)) : 0;
  if (table && line > 0) {
    const auto measure = [&](std::string_view value) {
      float advance = 0;
      for (const unsigned char c : value) {
        const uint32_t glyph = LoadGuestU32(base, table + uint32_t(c) * 4);
        if (glyph) {
          const auto* bytes = simpsons::GuestToHost(base, glyph + 6);
          advance += float(uint16_t(bytes[0]) << 8 | bytes[1]);
        }
      }
      return advance * simpsons::LoadGuestFloat(base, text + 72) * *scale / line;
    };
    const float available =
        (simpsons::LoadGuestFloat(base, text + 12) - simpsons::LoadGuestFloat(base, text + 4)) *
        .92f;
    if (row >= 0 && g_ui.page == Page::kKeyboard) {
      const float width = measure(label);
      if (width > available)
        *scale *= available / width;
    } else if (field == 0) {
      const float width = measure(label);
      if (width > available)
        *scale *= available / width;
    } else if (measure(label) > available) {
      while (!label.empty() && measure(label + "...") > available)
        label.pop_back();
      label += "...";
    }
  }
  TextBuffer(size_t(field), label);
  return true;
}

void SavesKeyboardTap(PPCContext& ctx, uint8_t* base, int row, float position) {
  if (!SavesKeyboard() || !g_menu.screen)
    return;
  g_apt->Bind(ctx, base, g_menu.movie);
  Key(row, std::clamp(int(position * (row == 3 ? 5 : 10)), 0, row == 3 ? 4 : 9));
  RebuildNativeMenu(g_ui.key_row);
}

void InitSaves(rex::Runtime* runtime) {
  g_runtime = runtime;
  g_apt = std::make_unique<simpsons::GameMenuApt>(runtime);
}

void OpenSavesFromPause() {
  if (!saves::kLibraryEnabled)
    return;
  g_ui = Ui{};
  g_ui.mode = Mode::kPause;
  g_ui.can_save = !saves::Busy();
  g_ui.dirty = true;
  Refresh();
  Open(Screen::kSaves);
}

// Plain black, faded in a quarter of a second either way.
bool DrawSavesCover(Frame& f) {
  // An exact save loading: black from the level's first playing frame.
  if (g_drive.flow == Flow::kLoad && g_drive.stage == Stage::kLevel && g_drive.save.exact &&
      LevelStepped(250))
    g_black = g_black_target = 1;
  const float step = std::max(f.dt, 0.001f) / 0.25f;
  g_black = g_black < g_black_target ? std::min(g_black_target, g_black + step)
                                     : std::max(g_black_target, g_black - step);
  if (g_black <= 0)
    return false;
  f.c.Rect(0, 0, f.c.width(), f.c.height(), uint32_t(std::clamp(g_black, 0.0f, 1.0f) * 255) << 24);
  return true;
}

bool SavesCovering() {
  return g_black > 0 || g_black_target > 0;
}

void TickSaves(PPCContext& ctx, uint8_t* base) {
  if (!saves::kLibraryEnabled)
    return;
  // The game thread's half of the screen: saving needs the start of a frame.
  // Saving, at the start of a frame with the pause menu up: the game's audio
  // is paused then, which freezing it for the snapshot needs (in play, its
  // audio worker can keep the freeze waiting).
  static Clock::time_point save_asked{};
  if (g_ui.save_now && save_asked == Clock::time_point{})
    save_asked = Clock::now();
  // The game gets ready to be frozen first (a frame or two).
  if (g_ui.save_now && !snapshot::Ready() && Clock::now() - save_asked < 3s)
    ;
  else if (g_ui.save_now) {
    g_ui.save_now = false;
    save_asked = {};
    if (LoadGuestU32(base, kFrontendState) != 9) {
      snapshot::Release();
      Notify("Can't save here");
    } else if (saves::Busy()) {
      snapshot::Release();
      Notify("Still saving the last one");
    } else {
      Close();
      RestoreNativeMenu(ctx, base);
      const bool ok = saves::Create(
          ctx, base, g_ui.save_name, "manual",
          [](bool ok, const saves::Save& save) {
            Notify(ok ? (save.exact ? "Saved \"" + save.name + "\""
                                    : "Saved \"" + save.name + "\" (checkpoint only)")
                      : std::string("Couldn't save"));
            g_ui.dirty = true;
            g_ui.reload = true;
          },
          g_ui.save_replace);
      Notify(ok ? "Saving..." : "Couldn't save");
      if (ok)
        saves::Track(true);
    }
  }
  if (Current() == Screen::kSaves && g_ui.mode == Mode::kPause && g_ui.location.empty())
    g_ui.location = saves::Where(ctx, base);
  if (Current() == Screen::kSaves && g_drive.flow == Flow::kNone)
    TickNativeMenu(ctx, base);
  else
    RestoreNativeMenu(ctx, base);
  Work(ctx, base);
  if (g_drive.flow != Flow::kNone || !REXCVAR_GET(saves_screen))
    return;
  // Use the native Saved Games menu as soon as its page is ready.
  if (LoadGuestU32(base, kFrontendState) == 9)
    return;
  const GamePage page = ReadPage(ctx, base);
  if (!page.slots) {
    // The classic slots last while Saved Games and its questions show.
    if (page.input != kPopupInput)
      g_ui.classic = false;
    if (Current() == Screen::kSaves && g_ui.mode == Mode::kFrontEnd)
      Close();
    return;
  }
  if (g_ui.classic || IsOpen())
    return;
  REXLOG_INFO("[saves-ui] Saved Games: the save library (page '{}')", page.name);
  g_ui = Ui{};
  g_ui.mode = Mode::kFrontEnd;
  Refresh();
  Open(Screen::kSaves);
  TickNativeMenu(ctx, base);
}

}  // namespace game_ui

// Front-end controller and native pointer events share this handler.
REX_FUNC(sub_8239F330) {
  if (game_ui::SavesMenuEvent(ctx, base, ctx.r3.u32, int(ctx.r5.u32)))
    return;
  __imp__sub_8239F330(ctx, base);
}
