#include "entity_health.h"

#include "entity_transform.h"
#include "game_thread.h"
#include "guest_memory.h"
#include "spawner.h"

#include <cmath>

#include <rex/ppc/context.h>

namespace entity_health {
namespace {
constexpr uint32_t kHealthTable = 0x8218AB98;
constexpr uint32_t kSendMessage = 0x8268FAA8;
constexpr uint32_t kDamageMessage = 0x82E07CC4, kHealMessage = 0x82E07CD4;
constexpr uint32_t kDamageInfoInit = 0x82937650, kDamageInfoDestroy = 0x82881400;

bool Readable(uint32_t address, uint32_t bytes) {
  return address && !(address & 3) && spawner::Readable(address, bytes);
}

uint32_t Find(const uint8_t* base, uint32_t entity) {
  if (!Readable(entity, 56)) return 0;
  const auto bank = simpsons::LoadGuestU32(base, entity + 48);
  const auto items = simpsons::LoadGuestU32(base, entity + 52);
  if (!Readable(bank, 16)) return 0;
  // sub_8269AD58 walks items[1..bank.count]. Entry zero is the absent
  // component sentinel. The bank's numeric type map is not a name hash.
  const auto count = simpsons::LoadGuestU16(base, bank + 12);
  if (count > 256 || !Readable(items, (uint32_t(count) + 1) * 4)) return 0;
  uint32_t found = 0;
  for (uint32_t i = 1; i <= count; ++i) {
    const auto component = simpsons::LoadGuestU32(base, items + i * 4);
    if (!Readable(component, 12) || simpsons::LoadGuestU32(base, component) != kHealthTable)
      continue;
    if (found || !Readable(component, 216) ||
        simpsons::LoadGuestU32(base, component + 8) != entity ||
        simpsons::LoadGuestU32(base, component + 40) != entity) return 0;
    found = component;
  }
  return found;
}

std::optional<Health> Read(PPCContext& ctx, uint8_t* base, uint32_t component) {
  double current = 0, maximum = 0;
  uint32_t dead = 0, enabled = 0;
  if (!component || !CallGuest(ctx, base, 0x82956FF0, {component}, nullptr, &current) ||
      !CallGuest(ctx, base, 0x82956FF8, {component}, nullptr, &maximum) ||
      !CallGuest(ctx, base, 0x82957018, {component}, &dead) ||
      !CallGuest(ctx, base, 0x82956FC0, {component}, &enabled) ||
      !std::isfinite(current) || !std::isfinite(maximum) || current < 0 || maximum < 0)
    return {};
  return Health{float(current), float(maximum), bool(dead & 0xFF), bool(enabled & 0xFF)};
}
}  // namespace

std::optional<Health> Get(PPCContext& ctx, uint8_t* base, uint32_t entity) {
  return Read(ctx, base, Find(base, entity));
}

uint32_t ComponentOwner(const uint8_t* base, uint32_t component) {
  if (!Readable(component, 216) || simpsons::LoadGuestU32(base, component) != kHealthTable)
    return 0;
  const auto owner = simpsons::LoadGuestU32(base, component + 8);
  return owner && owner == simpsons::LoadGuestU32(base, component + 40) &&
          Find(base, owner) == component ? owner : 0;
}

uint32_t DyingEntity(const uint8_t* base, uint32_t component) {
  if (!Readable(component, 216) || simpsons::LoadGuestU32(base, component) != kHealthTable)
    return 0;
  const auto owner = simpsons::LoadGuestU32(base, component + 8);
  const auto current = simpsons::LoadGuestFloat(base, component + 16);
  return std::isfinite(current) && current <= 0 &&
      !(simpsons::LoadGuestU32(base, component + 28) & 2) &&
      owner == simpsons::LoadGuestU32(base, component + 40) ? owner : 0;
}

std::optional<Change> Apply(PPCContext& ctx, uint8_t* base, uint32_t entity,
                            Action action, float amount) {
  if (!std::isfinite(amount) || amount < 0 || amount > 1000000) return {};
  const auto component = Find(base, entity);
  const auto before = Read(ctx, base, component);
  if (!before) return {};
  Change result{*before, before, false};
  if (!amount) return result;
  const auto transform = entity_transform::Get(ctx, base, entity);
  if (action == Action::kDamage && !transform) return {};
  const auto lifetime = spawner::Observe(entity);
  PPCContext scratch = ctx;
  if (scratch.r1.u32 < 0x600) return {};
  scratch.r1.u64 -= 0x100;
  const auto info = scratch.r1.u32 + 0x50;
  const auto message = action == Action::kDamage ? kDamageMessage : kHealMessage;
  if (!Readable(entity, 72) || !simpsons::LoadGuestU32(base, message) ||
      !CallGuest(scratch, base, kDamageInfoInit, {info})) return {};
  simpsons::StoreGuestFloat(base, info + 44, amount);
  // Original type 13 accepts script-authored hits without an attacker. Do
  // not impersonate another entity or bypass the original eligibility hook.
  simpsons::StoreGuestU32(base, info + 52, 13);
  // Match the original hit producers, including the simulation timestamp
  // used by character cooldowns. A constructor's zero timestamp is stale.
  simpsons::StoreGuestU32(base, info + 64, simpsons::LoadGuestU32(base, 0x82D572BC));
  if (transform)
    for (uint32_t axis = 0; axis < 3; ++axis) {
      simpsons::StoreGuestFloat(base, info + 20 + axis * 4, transform->position[axis]);
      simpsons::StoreGuestFloat(base, info + 32 + axis * 4, transform->position[axis]);
    }
  // Match the original synchronous hit producers. Dispatch to the entity's
  // subscriber, preserving activation checks and every listening component,
  // rather than entering just its health handler or forcing an inactive target.
  const bool applied = CallGuest(scratch, base, kSendMessage, {message, entity + 56, info, 0});
  // Own the temporary exactly like the original callers, even if a callback
  // removed the target. Its references are independent of the target's life.
  const bool released = CallGuest(scratch, base, kDamageInfoDestroy, {info});
  if (!applied || !released) return {};
  result.removed = spawner::Lifetime(entity) != lifetime;
  result.after = result.removed ? std::nullopt : Get(ctx, base, entity);
  return result;
}

}  // namespace entity_health
