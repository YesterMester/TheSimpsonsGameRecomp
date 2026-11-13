#include "native_ragdoll_preview.h"

#include "feature_clock.h"
#include "game_thread.h"
#include "guest_memory.h"
#include "native_ragdoll_driver.h"
#include "players.h"
#include "spawner.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <vector>

#include <rex/logging.h>

uint32_t HavokWorld();

namespace native_ragdoll {
namespace {
struct CollisionState {
  uint32_t wrappers = 0;
  std::vector<uint32_t> wrapper_ids, objects, worlds;
};
struct Preview {
  uint64_t rig = 0, driver = 0;
  SkeletonPose source;
  CollisionState collision;
  uint32_t world = 0;
  float remaining = 2;
  bool suspended = false, stopping = false;
};
std::array<Preview, 2> g_previews;
std::atomic<uint32_t> g_input_mask{0};
players::FeatureClock g_clock;

bool Unlocked(const uint8_t* base, uint32_t world) {
  return world && !(world & 15) && spawner::Readable(world, 1024) &&
      simpsons::LoadGuestU32(base, world) == 0x8219A87C &&
      simpsons::LoadGuestU32(base, world + 128) == 0;
}

// Follow the same wrapper/record traversal as 827A8560 and 827A9418. Do not
// guess character-controller offsets or retain an unvalidated native object.
std::optional<CollisionState> Collision(PPCContext& ctx, uint8_t* base, uint32_t entity) {
  if (!spawner::Readable(entity, 260)) return {};
  CollisionState state;
  state.wrappers = simpsons::LoadGuestU32(base, entity + 252);
  const auto count = simpsons::LoadGuestU32(base, entity + 256);
  if (count > 8 || (count && !spawner::Readable(state.wrappers, count * 8))) return {};
  const auto add = [&](uint32_t object) {
    if (!object) return true;
    if (!spawner::Readable(object, 16)) return false;
    state.objects.push_back(object);
    state.worlds.push_back(simpsons::LoadGuestU32(base, object + 8));
    return true;
  };
  for (uint32_t i = 0; i < count; ++i) {
    const auto wrapper = simpsons::LoadGuestU32(base, state.wrappers + i * 8);
    if (!wrapper || !spawner::Readable(wrapper, 44)) return {};
    state.wrapper_ids.push_back(wrapper);
    if (!add(simpsons::LoadGuestU32(base, wrapper + 28))) return {};
    const auto collection = simpsons::LoadGuestU32(base, wrapper + 4);
    if (!collection || !spawner::Readable(collection, 8)) return {};
    const auto records = simpsons::LoadGuestU32(base, collection);
    const auto n = simpsons::LoadGuestU32(base, collection + 4);
    if (n > 128 || (n && !spawner::Readable(records, n * 112))) return {};
    for (uint32_t j = 0; j < n; ++j) {
      uint32_t object = 0;
      if (!CallGuest(ctx, base, 0x827AA3A0, {records + j * 112}, &object)) return {};
      if (!object && !CallGuest(ctx, base, 0x827AA338, {records + j * 112}, &object)) return {};
      if (!add(object)) return {};
    }
  }
  return state;
}

bool SameObjects(const CollisionState& a, const CollisionState& b) {
  return a.wrappers == b.wrappers && a.wrapper_ids == b.wrapper_ids && a.objects == b.objects;
}

void PublishInputMask() {
  uint32_t mask = 0;
  for (uint32_t i = 0; i < g_previews.size(); ++i)
    if (g_previews[i].driver || g_previews[i].suspended) mask |= 1u << i;
  g_input_mask.store(mask, std::memory_order_release);
}

bool Stop(PPCContext& ctx, uint8_t* base, Preview& preview) {
  preview.stopping = true;
  if (preview.driver && !DetachFamilyPose(preview.driver)) return false;
  const auto entity = preview.source.world_epoch == spawner::WorldEpoch()
      ? spawner::Address(preview.source.actor) : 0;
  if (preview.suspended && entity) {
    if (!Unlocked(base, preview.world)) return false;
    const auto current = Collision(ctx, base, entity);
    if (!current || !SameObjects(*current, preview.collision)) return false;
    // This passive test owns the palette and collision handoff, not the actor
    // root. Preserve a teleport/moving-platform/cinematic transform applied by
    // the game while it was active. Never restore an obsolete world position.
    if (const auto root = entity_transform::Get(ctx, base, entity)) {
      auto restored = preview.source;
      restored.world_transform = *root;
      WriteFamilySkeleton(ctx, base, restored);
    }
    if ((simpsons::LoadGuestU32(base, entity + 220) & 2) &&
        !CallGuest(ctx, base, 0x82695260, {entity})) return false;
    const auto restored = Collision(ctx, base, entity);
    if (!restored || !SameObjects(*restored, preview.collision) ||
        restored->worlds != preview.collision.worlds ||
        (simpsons::LoadGuestU32(base, entity + 220) & 2)) return false;
  }
  preview.suspended = false;
  if (!ReleaseRig(ctx, base, preview.rig)) return false;
  preview = {};
  return true;
}
}  // namespace

bool StartPlayerPreview(PPCContext& ctx, uint8_t* base, uint32_t slot) {
  if (!base || slot >= g_previews.size() || g_previews[slot].rig ||
      !simpsons::LoadGuestU32(base, 0x82CED760) || !LevelStepped(500) ||
      players::ActiveNpcAvatar().active || players::RagdollModEnabled()) return false;
  const auto family = players::List(base);
  const auto player = std::find_if(family.begin(), family.end(),
      [&](const auto& p) { return p.input_player == int32_t(slot); });
  if (player == family.end() || player->hit_points <= 0 || player->character > 3) return false;
  const auto source = ReadFamilySkeleton(ctx, base, spawner::Observe(player->entity));
  const auto collision = Collision(ctx, base, player->entity);
  const auto world = HavokWorld();
  if (!source || !collision || !Unlocked(base, world) ||
      (simpsons::LoadGuestU32(base, player->entity + 220) & 2) ||
      std::any_of(collision->worlds.begin(), collision->worlds.end(),
          [&](uint32_t value) { return value != world; })) return false;
  constexpr float masses[] = {110, 32, 65, 28};
  const auto definition = BuildBipedRig(*source, masses[player->character]);
  if (!definition) return false;
  auto& preview = g_previews[slot];
  preview.rig = CreateRig(ctx, base, source->actor, definition->bodies, definition->joints);
  if (!preview.rig) return false;
  preview.source = *source;
  preview.collision = *collision;
  preview.world = world;
  const auto rollback = [&] { Stop(ctx, base, preview); PublishInputMask(); return false; };
  try {
    preview.driver = AttachFamilyPose(ctx, base, preview.rig, *definition);
    if (!preview.driver) return rollback();
    // The native routine owns flag 0x2 AND removal of its wrappers. Never
    // substitute a raw flag write for the native remove/re-add operations.
    preview.suspended = true;
    if (!CallGuest(ctx, base, 0x82695208, {player->entity})) return rollback();
    const auto removed = Collision(ctx, base, player->entity);
    if (!removed || !SameObjects(*removed, preview.collision) ||
        std::any_of(removed->worlds.begin(), removed->worlds.end(), [](auto value) { return value; }) ||
        !(simpsons::LoadGuestU32(base, player->entity + 220) & 2) ||
        !SetFamilyPoseEnabled(preview.driver, true)) return rollback();
  } catch (...) {
    rollback();
    throw;
  }
  PublishInputMask();
  REXLOG_INFO("[ragdoll] Native player {} preview started (13 bodies, 12 joints)", slot + 1);
  return true;
}

bool StopPlayerPreviews(PPCContext& ctx, uint8_t* base) {
  bool complete = true;
  for (auto& preview : g_previews)
    if (preview.rig || preview.driver || preview.suspended) complete = Stop(ctx, base, preview) && complete;
  PublishInputMask();
  return complete;
}

void TickPlayerPreviews(PPCContext& ctx, uint8_t* base) {
  const auto us = (uint64_t(simpsons::LoadGuestU32(base, 0x82D572C8)) << 32) |
      simpsons::LoadGuestU32(base, 0x82D572CC);
  const bool running = simpsons::LoadGuestU32(base, 0x82CED760) &&
      simpsons::LoadGuestU32(base, 0x82D572DC) != 1;
  const auto dt = g_clock.Sample(us, running);
  const auto family = ActivePlayerPreviews() ? players::List(base) : std::vector<players::Player>{};
  for (uint32_t slot = 0; slot < g_previews.size(); ++slot) {
    auto& preview = g_previews[slot];
    if (!preview.rig && !preview.driver && !preview.suspended) continue;
    const auto stats = FamilyPoseStats(preview.driver);
    const auto entity = preview.source.world_epoch == spawner::WorldEpoch()
        ? spawner::Address(preview.source.actor) : 0;
    const bool controlled = std::any_of(family.begin(), family.end(), [&](const auto& player) {
      return player.entity == entity && player.input_player == int32_t(slot);
    });
    preview.remaining -= dt;
    if (preview.stopping || !entity || !controlled || !RigActor(preview.rig) || !stats || !stats->enabled ||
        stats->foreign_thread_callbacks || stats->failures || preview.remaining <= 0 ||
        players::RagdollModEnabled()) {
      if (Stop(ctx, base, preview)) REXLOG_INFO("[ragdoll] Native player preview restored");
    }
  }
  PublishInputMask();
}

uint32_t ActivePlayerPreviews() { return g_input_mask.load(std::memory_order_acquire); }

std::optional<PoseDriverStats> PlayerPreviewStats(uint32_t player) {
  return player < g_previews.size() ? FamilyPoseStats(g_previews[player].driver) : std::nullopt;
}

void FilterPlayerPreviewInput(const uint8_t* base, uint32_t player, uint8_t* gamepad) {
  if ((player & 0xFF) == 0xFF) player = 0;
  if (!base || !gamepad || player > 1 || !(ActivePlayerPreviews() & (1u << player)) ||
      !simpsons::LoadGuestU32(base, 0x82CED760) ||
      simpsons::LoadGuestU32(base, 0x82D572DC) == 1) return;
  gamepad[0] = 0;
  gamepad[1] &= 0x30;  // Keep Start and Back; the player can always open a menu.
  std::memset(gamepad + 2, 0, 6);  // Triggers and movement; right stick still looks.
}
}  // namespace native_ragdoll
