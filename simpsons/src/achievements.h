#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

struct PPCContext;
namespace rex {
class Runtime;
}

// The game's 44 Xbox 360 achievements, kept with the save data.
namespace achievements {

struct Achievement {
  uint32_t id = 0;
  std::string name, description, locked_description;
  uint32_t gamerscore = 0;
  // The achievement's picture from the game's own title data (PNG).
  std::vector<uint8_t> icon_png;
  bool unlocked = false;
  int64_t unlock_time = 0;  // Unix seconds, 0 if unknown
  bool with_cheats = false;  // unlocked while the mod menu's cheats were on
  // How far the loaded save is towards it, when the game counts it.
  float progress = 0, goal = 0;
};

void Init(rex::Runtime* runtime);
void Shutdown();

// The game thread, once a frame. Unlocks what the loaded save has earned,
// including progress from before achievements were kept.
void Tick(PPCContext& ctx, uint8_t* base);

// Every achievement in the game's order. Empty until the title data is read.
std::vector<Achievement> List();
// Changes whenever an achievement is unlocked or locked.
uint32_t Generation();

// Unlocks an achievement; false if it was already unlocked or is unknown.
bool Unlock(uint32_t id, std::string_view reason);
bool Lock(uint32_t id);
void LockAll();

// Unlocks not yet shown, oldest first.
std::vector<uint32_t> TakeNotifications();

}  // namespace achievements
