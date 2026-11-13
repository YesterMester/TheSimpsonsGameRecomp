#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct PPCContext;
namespace rex {
class Runtime;
}
namespace canvas {
struct Picture;
}

// The save library: any number of named saves, grouped by playthrough, each
// with a picture of the moment and a copy of the game's save slot A as it was.
// An exact save also holds a snapshot of the whole game (snapshot.h), so it
// loads at that exact moment, mid-level included; the others load at the
// game's last checkpoint.
//
// The game only ever plays slot A (the working slot): loading a save puts its
// copy back as slot A first. Whenever the game writes slot A itself (its
// checkpoints and autosaves) the copy becomes an autosave of the playthrough
// being played, and slots the game changed while the library wasn't watching
// (the classic Saved Games screen, an older version) are imported.
//
// <user data>/saves/<id>/save.toml   name, playthrough, location, time, ...
//                       /thumb.png   the game just before it was paused
//                       /state.snap  the snapshot (memory in ../chunks)
//                       /slots/      the game's slot A (data and header)
// <user data>/saves/chunks/          snapshot memory shared by every save
// <user data>/saves/slots.toml       what the game's slots held last
namespace saves {

// The save library is shelved while the Modkit's other workflows are built.
// Keep its storage and public checkpoint services for existing projects; the
// original game owns the visible save menus, even with an older saved config.
inline constexpr bool kLibraryEnabled = false;

struct Save {
  std::string id;  // the folder's name
  std::string name;
  std::string playthrough;  // id
  std::string playthrough_name;
  std::string kind;  // "manual", "auto" or "imported"
  int64_t created = 0;  // Unix time
  std::string location;  // where it was made, as the game names it
  double play_time = 0;  // seconds
  float completion = 0;  // percent
  bool exact = false;    // has a snapshot
  // The build's own memory layout when it was made (host_memory.h): an exact
  // save only loads exactly under the same one (Exact).
  std::string layout;
  uint64_t bytes = 0;    // what it added on disk
  std::filesystem::path folder;
};

struct Playthrough {
  std::string id, name;
};

void Init(rex::Runtime* runtime);
void Shutdown();
// Game thread, at the start of a frame.
void Tick(PPCContext& ctx, uint8_t* base);

// Where saves live.
std::filesystem::path Root();
// Saves, newest first: every one, or one playthrough's, matching a search
// (each word somewhere in the name, playthrough or location; any case).
std::vector<Save> List(const std::string& playthrough = {}, const std::string& search = {});
// Reads the library without blocking the menus. Done runs on the game thread.
void Scan(std::function<void(std::vector<Save>)> done);
// The newest save of a playthrough, or of all when it's empty (an empty id if
// there is none).
Save Latest(const std::string& playthrough = {});
// Playthroughs that have saves, most recently saved first.
std::vector<Playthrough> Playthroughs();
bool Rename(const std::string& id, const std::string& name);
// Whether a save loads at its exact moment here (else at its checkpoint).
bool Exact(const Save& save);
// Removes a save, then (in the background) the snapshot memory only it used.
bool Remove(const std::string& id);

// The playthrough new saves belong to.
Playthrough Current();
void SetCurrent(const Playthrough& playthrough);
Playthrough NewPlaythrough(const std::string& name);

// Game thread, at the start of a frame while a level runs: saves now. The
// game freezes for about half a second; the rest is written in the
// background, then done runs (on the game thread, from Tick). With replace,
// that save is removed once the new one is written (overwriting it).
using Done = std::function<void(bool ok, const Save& save)>;
bool Create(PPCContext& ctx, uint8_t* base, const std::string& name, const std::string& kind,
            Done done, const std::string& replace = {});
// Checkpoint-only capture: keep the game's current slot and mod state without
// freezing or copying guest memory. Completion uses the same worker and callback.
bool CreateCheckpoint(PPCContext& ctx, uint8_t* base, const std::string& name,
                      const std::string& kind, Done done, const std::string& replace = {});
// Whether a save is being written or read.
bool Busy();

// Puts a save's copy of slot A back as the game's slot A, keeping a backup
// of the slot it replaces, and makes its playthrough the current one.
// Restore checkpoint mod state when its new level is ready. Exact-load paths
// pass false: their host state is committed with the guest snapshot instead.
// Checkpoint callers first use modkit_game::BeginCheckpoint and wait for
// CheckpointReady; this function does not hash packages on the game thread.
bool PrepareSlot(const Save& save, bool restore_checkpoint = true);
// Keeps a backup of the game's slot A (before the game deletes it).
void BackupSlot();
// Whether the game's slot A holds no progress (or isn't there).
bool SlotAEmpty();
// Where the game is, as it names it: the current episode, else where slot A
// was last saved (game thread).
std::string Where(PPCContext& ctx, uint8_t* base);
// Whether the game's slot A changes become the current playthrough's
// autosaves: on while a playthrough started from the library is played.
void Track(bool on);
bool Tracking();

// Reads a save's snapshot in the background, to put under the game later.
bool Read(const std::string& id);
// kOlder: made by a build whose own memory is elsewhere; it only loads at its
// checkpoint.
enum class ReadState { kNone, kReading, kReady, kFailed, kOlder };
ReadState ReadStatus();
// Game thread, at the start of a frame while a level runs: puts the snapshot
// read under the game, which carries on from the save's moment. False, with
// nothing changed, if the game's threads aren't parked where the snapshot's
// were; the snapshot stays ready for another try.
bool PutRead(PPCContext& ctx, uint8_t* base);
void DropRead();

// A save's picture, read in the background: null until it has been read, or
// if it has none.
std::shared_ptr<const canvas::Picture> Picture(const Save& save);
// The game just before the pause menu opened, for the next save's picture.
std::shared_ptr<const canvas::Picture> PausePicture();
// Takes that picture (pause_menu.cpp, as the pause menu opens).
void RememberFrame();

}  // namespace saves
