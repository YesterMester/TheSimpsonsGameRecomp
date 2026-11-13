#pragma once

#include "guest_memory.h"
#include "players.h"

#include <span>

namespace players {

// sub_8288DBE0 tests +820 against -1 for human control. The input binding
// installed by sub_828844F0 is at +2076 (6 means unbound); the original
// button readers, including sub_82884788, pass it to the input dispatcher.
// +2068 is a family-list identity and +2088 is a viewport, not an input port.
// Input can be temporarily blocked by menus/cinematics without losing this
// assignment, so do not infer ownership from held buttons or input flags.
inline int32_t ReadInputPlayer(const uint8_t* base, uint32_t entity) {
  if (simpsons::LoadGuestU32(base, entity + 820) == UINT32_MAX)
    return -1;
  const auto input = simpsons::LoadGuestU32(base, entity + 2076);
  return input < 2 ? int32_t(input) : -1;
}

// During a handover, more than one actor can temporarily retain a binding.
// Reject every claimant instead of making callback order select a character.
inline void ClearAmbiguousInputs(std::span<Player> players) {
  std::array<uint32_t, 2> counts{};
  for (const auto& player : players)
    if (player.input_player >= 0 && player.input_player < 2)
      ++counts[size_t(player.input_player)];
  for (auto& player : players)
    if (player.input_player >= 0 && player.input_player < 2 &&
        counts[size_t(player.input_player)] != 1)
      player.input_player = -1;
}

}  // namespace players
