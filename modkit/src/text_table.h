#pragma once

#include "runtime.h"

#include <span>

namespace simpsons::modkit {

struct GameTextLine {
  std::string source, text;  // UTF-8 in packages, converted for each native font path.
};
using GameTextTables = std::map<std::string, std::map<uint32_t, GameTextLine>>;
GameTextTables ReadGameTextTables(const std::vector<Manifest>& manifests);
// The game fixes up table pointers in place. Normalize those runtime fields
// before hashing, so a pack can identify its exact source table after loading.
std::string RelocatedTextTableHash(std::span<const uint8_t> bytes, uint32_t address);

}  // namespace simpsons::modkit
