#pragma once

#include <cstdint>
#include "native_ragdoll_driver.h"

struct PPCContext;

namespace native_ragdoll {
// Opt-in, passive family-physics test. Runs for two seconds of simulation,
// then restores animation and controls, preserving the current actor root.
// This is not an animated
// get-up, an NPC controller, or the older whole-character tumbling effect.
// All lifecycle calls run on the game's update thread.
bool StartPlayerPreview(PPCContext& ctx, uint8_t* base, uint32_t input_player = 0);
bool StopPlayerPreviews(PPCContext& ctx, uint8_t* base);
void TickPlayerPreviews(PPCContext& ctx, uint8_t* base);
uint32_t ActivePlayerPreviews();
std::optional<PoseDriverStats> PlayerPreviewStats(uint32_t input_player);
// Apply only to gameplay input, after in-game menus have handled their input.
// Paused/loading menus receive their complete original input. During play,
// keeps pause/back and camera look; suppresses character actions and movement
// for the affected logical player only. The caller supplies 12 gamepad bytes.
void FilterPlayerPreviewInput(const uint8_t* base, uint32_t input_player, uint8_t* gamepad);
}  // namespace native_ragdoll
