#pragma once

#include <cstdint>

struct PPCContext;
namespace rex {
class Runtime;
}

// Achievements and the mod menu in the pause menu (pause_menu.cpp).
void InitPauseMenu(rex::Runtime* runtime);
// The game thread, once a frame.
void TickPauseMenu(PPCContext& ctx, uint8_t* base);

// The pause menu is up and taking input (not a screen opened from it), as of
// the game's last frame.
bool PauseMenuShowing();
