#pragma once

#include <cstdint>

namespace rex::input {

// Native menu gestures that must pass through the game's input polling,
// including title screens that poll Start instead of a menu callback.
void QueueMenuButton(uint32_t player, uint16_t button);
void ClearMenuButtons();

}  // namespace rex::input
