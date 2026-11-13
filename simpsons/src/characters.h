#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Other characters for players 1 and 2 (characters.cpp, from frankyfife's
// fork), for the mod menu.
namespace characters {

struct Choice {
  const char* name;   // character template, "" = the level's own
  const char* label;
};

const std::vector<Choice>& Choices();
// The choice in effect for a player (0 or 1), "" for the level's own.
std::string Current(int player);
// What the level itself spawned for a player last, as a label.
std::string LevelCharacter(int player);
// Sets both players' characters and restarts from the last checkpoint once the
// level runs with nothing in front of it.
void Apply(const std::string& player1, const std::string& player2);
// Whether a level is running that a restart can apply to.
bool LevelReady();

}  // namespace characters
