#pragma once

#include <cstdint>
#include <functional>
#include <initializer_list>
#include <string_view>

struct PPCContext;
namespace rex {
class Runtime;
}

// Work for the game's main thread, run at the start of its frame (the frame
// pacing hook), where guest functions can be called with the thread's own
// context. Tasks restore the registers they change.
using GameTask = std::function<void(PPCContext& ctx, uint8_t* base)>;
void RunOnGameThread(GameTask task);
// frame_pacing.cpp, once a frame on the game thread.
void TickGameThread(PPCContext& ctx, uint8_t* base);
// frame_pacing.cpp: whether the level's physics stepped within this long, so
// gameplay runs (not a loading screen or a menu).
bool LevelStepped(int64_t within_ms);
void InitGameThread(rex::Runtime* runtime);

// Calls the guest function at an address (a vtable entry, say) on the game
// thread with up to eight integer arguments, below the current stack frame.
// Returns r3, and f1 through *float_result. False if no function is there.
bool CallGuest(PPCContext& ctx, uint8_t* base, uint32_t address,
               std::initializer_list<uint32_t> args, uint32_t* result = nullptr,
               double* float_result = nullptr);

// The ScoreKeeper, on the game thread. A name is a score or score event as in
// gamedata/simpsons_scores.lua. GetScore is 0 for a missing score. Setting a
// score runs its watchers, as the game's own scripts do.
namespace scores {
bool Ready(const uint8_t* base);
float Get(PPCContext& ctx, uint8_t* base, std::string_view name);
void Set(PPCContext& ctx, uint8_t* base, std::string_view name, float value);
void Issue(PPCContext& ctx, uint8_t* base, std::string_view event);
uint32_t Hash(std::string_view name);
}  // namespace scores
