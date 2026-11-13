#include "entity_render.h"

#include "game_thread.h"
#include "guest_memory.h"
#include "spawner.h"

namespace entity_render {
namespace {
constexpr uint32_t kHide = 0x82697188, kShow = 0x826971C0, kForceShow = 0x82697200;

bool Readable(uint32_t at, uint32_t bytes) {
  return at && !(at & 3) && spawner::Readable(at, bytes);
}
}  // namespace

std::optional<State> Get(const uint8_t* base, uint32_t entity) {
  if (!Readable(entity, 208))
    return {};
  const auto table = simpsons::LoadGuestU32(base, entity);
  // Recognize the original shared layout before reading its hide counter.
  // A class with different visibility methods needs its own reviewed adapter.
  if (!Readable(table, 264) || simpsons::LoadGuestU32(base, table + 96) != kHide ||
      simpsons::LoadGuestU32(base, table + 100) != kShow ||
      simpsons::LoadGuestU32(base, table + 104) != kForceShow ||
      simpsons::LoadGuestU32(base, table + 256) != 0x826993F0 ||
      simpsons::LoadGuestU32(base, table + 260) != 0x826994E0)
    return {};
  const auto model = simpsons::LoadGuestU32(base, entity + 136);
  const auto node = simpsons::LoadGuestU32(base, entity + 156);
  if (model) {
    if (!Readable(model, 4) || !Readable(simpsons::LoadGuestU32(base, model), 64))
      return {};
  }
  if (node && (!Readable(node, 32) || !Readable(simpsons::LoadGuestU32(base, entity + 160), 164)))
    return {};
  return State{simpsons::GuestToHost(base, entity + 206)[0], bool(model),
               node && bool(simpsons::GuestToHost(base, node + 26)[0] & 4)};
}

bool Change(PPCContext& ctx, uint8_t* base, uint32_t entity, bool hide) {
  const auto state = Get(base, entity);
  if (!state || (hide && state->hide_count == 255))
    return false;
  // A game force-show may already have cleared every request. Do not call
  // Show at zero: the original method would attempt scene insertion again.
  if (!hide && !state->hide_count)
    return true;
  return CallGuest(ctx, base, hide ? kHide : kShow, {entity});
}
}  // namespace entity_render
