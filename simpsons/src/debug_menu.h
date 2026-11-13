#pragma once

#include <cstdint>
#include <filesystem>

struct PPCContext;

// The debug menu's switches and the effects it keeps up in the game
// (debug_menu.cpp; its pages are drawn in the game by game_ui.cpp).
namespace debug_menu {

// The settings file, for the switches that are kept.
void Init(const std::filesystem::path& config);
// The game thread, once a frame.
void Tick(PPCContext& ctx, uint8_t* base);
// The UI scene's draw callback is skipped (freecam.cpp).
bool HideHud();
// Cheats are switched on: the debug menu's cheats and the free camera's
// "bring the family here" work, and achievements unlocked meanwhile are marked.
bool CheatsEnabled();
void SetCheats(bool on);

}  // namespace debug_menu
