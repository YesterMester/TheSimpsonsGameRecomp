#pragma once

#include <cstdint>
#include <optional>

struct PPCContext;

namespace entity_health {

struct Health {
  float current = 0, maximum = 0;
  bool dead = false, enabled = false;
};
struct Change {
  Health before;
  // A native hit can remove its target. Never read that allocation again.
  std::optional<Health> after;
  bool removed = false;
};
enum class Action { kDamage, kHeal };

// Game thread only. These recognize the original health component in the
// entity's bounded component bank; a generic entity getter returning 1 does
// not establish that an object actually has health.
std::optional<Health> Get(PPCContext& ctx, uint8_t* base, uint32_t entity);
// Resolve only a recognized component whose live owner's bank still contains
// this exact component. Does not invoke guest code or infer a family layout.
uint32_t ComponentOwner(const uint8_t* base, uint32_t component);
// Original death-handler hook: identify a recognized component that currently
// satisfies its native death predicate, without retaining its allocation.
uint32_t DyingEntity(const uint8_t* base, uint32_t component);
// Scripted damage/healing, through the original entity dispatcher. Amount is
// finite, nonnegative and at most 1,000,000. Damage protection and the game's
// own filters/callbacks still apply. Healing does not resurrect a dead actor.
std::optional<Change> Apply(PPCContext& ctx, uint8_t* base, uint32_t entity,
                            Action action, float amount);

}  // namespace entity_health
