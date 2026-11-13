#pragma once

#include <cstdint>
#include <optional>

struct PPCContext;

namespace entity_render {
// Native visibility requests and scene membership, not a camera/occlusion test.
struct State {
  uint8_t hide_count = 0;
  bool has_model = false;
  bool scene_attached = false;
};
std::optional<State> Get(const uint8_t* base, uint32_t entity);
// Acquire or release one native hide request. The host owns the balancing
// lease; this never uses the game's force-show/reset operation.
bool Change(PPCContext& ctx, uint8_t* base, uint32_t entity, bool hide);
}  // namespace entity_render
