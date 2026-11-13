// The family members in play.
//
// The player manager (0x82D08BA4) keeps its players in a list at +8: an array
// of pointers at +4 and a count at +8. Each pointer is to +2032 in a player
// entity, which keeps its character id at +3460 (sub_82895130 finds a player
// by character, as the bus stop map does) and its RenderWare frame at +140,
// whose modelling matrix puts the position at +64.
//
// The health check that sends iMsgPlayerHealthLow (sub_82886AF0) reads the
// entity's health through its vtable at +176 and the maximum through the
// component at +344 (vtable +40). The health component keeps the hit points
// at +16 and the maximum the game compares with at +24. Its iMsgDamage handler
// (sub_82957478) asks sub_82957208(component, damage) whether the hit counts;
// 2 means it is ignored, as for a component that is already down.

#include "players.h"
#include "player_control.h"
#include "feature_clock.h"
#include "entity_transform.h"
#include "entity_render.h"
#include "entity_health.h"
#include "spawner.h"
#include "spawn_sources.h"
#include "assets.h"

#include "game_thread.h"
#include "guest_memory.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <map>
#include <mutex>
#include <string>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc.h>

REXCVAR_DEFINE_BOOL(debug_menu_log_players, false, "Game",
                    "Log the players' fields every few seconds (for development)");
REXCVAR_DEFINE_INT32(debug_menu_log_players_frames, 150, "Game",
                     "Frames between the players' log lines (for development)")
    .range(1, 3600);
REXCVAR_DEFINE_BOOL(ragdoll_mod_enabled, false, "Mods",
                    "Development: enable experimental root-motion knockdowns");
REXCVAR_DEFINE_BOOL(ragdoll_run_impact, true, "Mods",
                    "Trigger experimental knockdowns when running into supported NPCs");
REXCVAR_DEFINE_BOOL(ragdoll_punch_impact, true, "Mods",
                    "Trigger experimental knockdowns after accepted damage");
REXCVAR_DEFINE_BOOL(ragdoll_push_downed, true, "Mods",
                    "Allow moving downed ragdolls and defeated enemies with the player body");
REXCVAR_DEFINE_BOOL(ragdoll_player_enabled, true, "Mods",
                    "Allow player characters to ragdoll on heavy impacts or manual trigger");
REXCVAR_DEFINE_DOUBLE(ragdoll_force_scale, 1.0, "Mods",
                      "Force multiplier for experimental knockdown motion")
    .range(0.25, 4.0);

REX_EXTERN(__imp__sub_82957208);
REX_EXTERN(sub_82957208);

namespace players {
namespace {

using simpsons::LoadGuestFloat;
using simpsons::LoadGuestU32;

constexpr uint32_t kPlayerManager = 0x82D08BA4;
constexpr uint32_t kListEntryOffset = 2032;
constexpr uint32_t kCharacter = 3460;
constexpr uint32_t kFrame = 140;
constexpr uint32_t kFramePosition = 64;
constexpr uint32_t kHealthComponent = 344;
constexpr uint32_t kHitPoints = 16;
constexpr uint32_t kMaxHitPoints = 24;
constexpr uint32_t kDamageIgnored = 2;

std::atomic<bool> g_god_mode{false};
std::atomic<uint64_t> g_god_mode_revision{0};
std::recursive_mutex g_features_mutex;
std::atomic<uint64_t> g_ragdoll_settings_revision{0};

NpcAvatarInfo g_npc_avatar;
uint64_t g_avatar_player_lifetime = 0;
bool g_avatar_hid_player = false;
struct PendingLibraryNpc {
  bool active = false;
  size_t asset_index = 0;
  std::string stream;
};
PendingLibraryNpc g_pending_npc;

struct PlayerMotion {
  std::array<float, 3> prev_pos{};
  std::array<float, 3> velocity{};
  bool valid = false;
};
std::map<uint32_t, PlayerMotion> g_player_motion;
FeatureClock g_feature_clock;
double g_feature_time = 0;

struct RagdollBody {
  uint64_t lifetime = 0;
  std::array<float, 3> velocity{};
  std::array<float, 3> angular_vel{};
  float pitch = 0.0f;
  float yaw = 0.0f;
  float roll = 0.0f;
  float ground_y = 0.0f;
  float timer = 0.0f;
  float writhe_phase = 0.0f;
  bool down_for_count = false;
  bool is_player = false;
  entity_transform::Transform upright_basis{};
  double last_impact = 0;
};
std::map<uint64_t, RagdollBody> g_ragdolls;
std::vector<uint64_t> g_pending_hit_entities;

uint32_t ThemeColorForName(std::string_view raw_name) {
  std::string lower(raw_name);
  for (char& c : lower) c = char(std::tolower(uint8_t(c)));
  if (lower.find("krusty") != std::string::npos || lower.find("clown") != std::string::npos)
    return 0x32D6B8;
  if (lower.find("wiggum") != std::string::npos || lower.find("cop") != std::string::npos ||
      lower.find("lou") != std::string::npos || lower.find("eddie") != std::string::npos)
    return 0x3D8BFF;
  if (lower.find("burns") != std::string::npos || lower.find("kang") != std::string::npos ||
      lower.find("kodos") != std::string::npos || lower.find("alien") != std::string::npos)
    return 0x68E348;
  if (lower.find("apu") != std::string::npos || lower.find("flanders") != std::string::npos)
    return 0x4CC26B;
  if (lower.find("frink") != std::string::npos || lower.find("smithers") != std::string::npos)
    return 0xC978FF;
  if (lower.find("willie") != std::string::npos || lower.find("bob") != std::string::npos ||
      lower.find("duff") != std::string::npos)
    return 0xFF6B4A;
  if (lower.find("nelson") != std::string::npos || lower.find("milhouse") != std::string::npos ||
      lower.find("skinner") != std::string::npos)
    return 0x5CA8FF;
  if (lower.find("moe") != std::string::npos || lower.find("barney") != std::string::npos ||
      lower.find("comic") != std::string::npos)
    return 0xFFA940;
  uint32_t h = 2166136261u;
  for (char c : lower) h = (h ^ uint8_t(c)) * 16777619u;
  const uint32_t r = 110 + (h & 0x7F);
  const uint32_t g = 110 + ((h >> 8) & 0x7F);
  const uint32_t b = 110 + ((h >> 16) & 0x7F);
  return (r << 16) | (g << 8) | b;
}

// The game's own pointers are either null or valid; anything else means the
// layout is not what is assumed here.
bool Plausible(uint32_t address) {
  return address >= 0x10000u && address < 0xFFFF0000u && !(address & 3);
}

uint32_t log_tick = 0;

}  // namespace

std::vector<Player> List(const uint8_t* base) {
  std::vector<Player> result;
  const uint32_t manager = LoadGuestU32(base, kPlayerManager);
  if (!Plausible(manager))
    return result;
  const uint32_t list = LoadGuestU32(base, manager + 8);
  if (!Plausible(list))
    return result;
  const uint32_t items = LoadGuestU32(base, list + 4);
  const uint32_t count = LoadGuestU32(base, list + 8);
  if (!Plausible(items) || count > 8)
    return result;
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t item = LoadGuestU32(base, items + i * 4);
    if (!Plausible(item))
      continue;
    Player player;
    player.entity = item - kListEntryOffset;
    player.character = LoadGuestU32(base, player.entity + kCharacter);
    player.input_player = ReadInputPlayer(base, player.entity);
    player.frame = LoadGuestU32(base, player.entity + kFrame);
    if (Plausible(player.frame)) {
      for (int axis = 0; axis < 3; ++axis)
        player.position[axis] = LoadGuestFloat(base, player.frame + kFramePosition + axis * 4);
    }
    player.health = LoadGuestU32(base, player.entity + kHealthComponent);
    if (Plausible(player.health)) {
      player.hit_points = LoadGuestFloat(base, player.health + kHitPoints);
      player.max_hit_points = LoadGuestFloat(base, player.health + kMaxHitPoints);
    } else {
      player.health = 0;
    }
    result.push_back(player);
  }
  ClearAmbiguousInputs(result);
  return result;
}

const char* CharacterName(uint32_t character) {
  // Stored character IDs are Homer, Bart, Marge, Lisa.
  constexpr const char* kNames[] = {"Homer", "Bart", "Marge", "Lisa"};
  return character < std::size(kNames) ? kNames[character] : "Player";
}

void SetGodMode(bool on) {
  g_god_mode = on;
  g_god_mode_revision.fetch_add(1, std::memory_order_release);
  REXLOG_INFO("[players] god mode {}", on ? "on" : "off");
}

bool GodMode() {
  return g_god_mode.load(std::memory_order_relaxed);
}

uint64_t GodModeRevision() {
  return g_god_mode_revision.load(std::memory_order_acquire);
}

void RefillHealth(PPCContext& ctx, uint8_t* base) {
  for (const auto& player : List(base)) {
    if (const auto health = entity_health::Get(ctx, base, player.entity))
      entity_health::Apply(ctx, base, player.entity, entity_health::Action::kHeal,
          std::max(0.0f, health->maximum - health->current));
  }
}

bool MoveTo(PPCContext& ctx, uint8_t* base, const Player& player,
            const std::array<float, 3>& position) {
  if (!entity_transform::Move(ctx, base, player.entity, position)) return false;
  REXLOG_INFO("[players] moved {} to ({:.2f}, {:.2f}, {:.2f})", CharacterName(player.character),
              position[0], position[1], position[2]);
  return true;
}

void Log(PPCContext& ctx, uint8_t* base) {
  if (!REXCVAR_GET(debug_menu_log_players) ||
      ++log_tick % uint32_t(REXCVAR_GET(debug_menu_log_players_frames)) != 0)
    return;
  for (const auto& player : List(base)) {
    const uint32_t vtable = LoadGuestU32(base, player.entity);
    const uint32_t health_fn = Plausible(vtable) ? LoadGuestU32(base, vtable + 176) : 0;
    double health = -1, maximum = -1;
    CallGuest(ctx, base, health_fn, {player.entity}, nullptr, &health);
    const uint32_t component = LoadGuestU32(base, player.entity + 344);
    uint32_t component_vtable = 0;
    if (Plausible(component)) {
      component_vtable = LoadGuestU32(base, component);
      if (Plausible(component_vtable))
        CallGuest(ctx, base, LoadGuestU32(base, component_vtable + 40), {component}, nullptr,
                  &maximum);
    }
    REXLOG_INFO("[players] entity {:08X} char {} frame {:08X} pos ({:.2f}, {:.2f}, {:.2f}) vtable "
                "{:08X} health fn {:08X} = {} / {} (component {:08X} vtable {:08X})",
                player.entity, player.character, player.frame, player.position[0],
                player.position[1], player.position[2], vtable, health_fn, health, maximum,
                component, component_vtable);
    if (Plausible(component_vtable)) {
      std::string slots;
      for (uint32_t i = 0; i < 24; ++i)
        slots += fmt::format(" {:08X}", LoadGuestU32(base, component_vtable + i * 4));
      REXLOG_INFO("[players] health component vtable:{}", slots);
    }
  }
}

NpcAvatarInfo ActiveNpcAvatar() {
  NpcAvatarInfo out;
  {
    std::lock_guard lock(g_features_mutex);
    out = g_npc_avatar;
  }
  // Do not nest the feature lock inside the spawner's lifetime lock.
  if (out.active) {
    out.entity = spawner::Address(out.npc_lifetime);
    if (!out.entity) out.active = false;
  }
  return out;
}

void VisibilityReset(uint64_t lifetime) {
  std::lock_guard lock(g_features_mutex);
  if (lifetime && lifetime == g_avatar_player_lifetime) g_avatar_hid_player = false;
}

bool RestoreDefaultCharacter(PPCContext& ctx, uint8_t* base) {
  uint64_t player_lifetime = 0, npc_lifetime = 0;
  bool hid_player = false;
  {
    std::lock_guard lock(g_features_mutex);
    g_pending_npc = {};
    if (!g_npc_avatar.active) return false;
    player_lifetime = g_avatar_player_lifetime;
    npc_lifetime = g_npc_avatar.npc_lifetime;
    hid_player = g_avatar_hid_player;
    g_npc_avatar = {};
    g_avatar_player_lifetime = 0;
    g_avatar_hid_player = false;
  }
  if (const auto player = spawner::Address(player_lifetime); player && hid_player)
    entity_render::Change(ctx, base, player, false);
  if (npc_lifetime) spawner::Destroy(ctx, base, npc_lifetime);
  return true;
}

bool PlayAsNpc(PPCContext& ctx, uint8_t* base, const spawner::Spawnable& target) {
  if (target.category != "People" && target.category != "Enemies" && target.category != "Bosses")
    return false;
  Player controlled{};
  for (const auto& player : List(base))
    if (player.input_player == 0) { controlled = player; break; }
  if (!controlled.entity) return false;
  const auto player_lifetime = spawner::Observe(controlled.entity);
  const auto transform = entity_transform::Get(ctx, base, controlled.entity);
  const auto render = entity_render::Get(base, controlled.entity);
  if (!player_lifetime || !transform || !render || render->hide_count == 255) return false;
  const auto entity = spawner::Clone(ctx, base, target.record, controlled.position, 1.0f);
  if (!entity) return false;
  const auto lifetime = spawner::Lifetime(entity);
  if (!lifetime || !entity_transform::Set(ctx, base, entity, *transform) ||
      !entity_render::Change(ctx, base, controlled.entity, true)) {
    if (lifetime) spawner::Destroy(ctx, base, lifetime);
    return false;
  }
  // Commit only after the replacement and this preview's own hide lease work.
  // A failed selection leaves the previous preview intact.
  RestoreDefaultCharacter(ctx, base);
  spawn_sources::RetainDebug(target.record.source_layer);
  {
    std::lock_guard lock(g_features_mutex);
    g_npc_avatar.active = true;
    g_npc_avatar.name = target.name;
    g_npc_avatar.class_name = target.record.name;
    g_npc_avatar.category = target.category;
    g_npc_avatar.npc_lifetime = lifetime;
    g_npc_avatar.player_entity = controlled.entity;
    g_npc_avatar.theme_color = ThemeColorForName(target.name);
    g_avatar_player_lifetime = player_lifetime;
    g_avatar_hid_player = true;
  }
  REXLOG_INFO("[players] NPC costume preview '{}' (native NPC AI is unchanged)", target.name);
  return true;
}

bool PlayAsLibraryNpc(PPCContext& ctx, uint8_t* base, size_t asset_index) {
  const auto target = spawner::LibraryTemplate(base, asset_index);
  return target && PlayAsNpc(ctx, base, *target);
}

bool TriggerNpcAvatarAbility(PPCContext&, uint8_t*) {
  // A costume clone does not possess the source NPC's ability controller.
  // Never choose a random animation or damage nearby actors as a substitute.
  return false;
}

RagdollSettings ReadRagdollSettings() {
  std::lock_guard lock(g_features_mutex);
  return {REXCVAR_GET(ragdoll_mod_enabled), REXCVAR_GET(ragdoll_run_impact),
          REXCVAR_GET(ragdoll_punch_impact), REXCVAR_GET(ragdoll_push_downed),
          REXCVAR_GET(ragdoll_player_enabled), float(REXCVAR_GET(ragdoll_force_scale))};
}
void ApplyRagdollSettings(const RagdollSettings& settings) {
  if (!std::isfinite(settings.force_scale) || settings.force_scale < 0.25f ||
      settings.force_scale > 4.0f) return;
  std::lock_guard lock(g_features_mutex);
  REXCVAR_SET(ragdoll_mod_enabled, settings.enabled);
  REXCVAR_SET(ragdoll_run_impact, settings.run_impact);
  REXCVAR_SET(ragdoll_punch_impact, settings.punch_impact);
  REXCVAR_SET(ragdoll_push_downed, settings.push_downed);
  REXCVAR_SET(ragdoll_player_enabled, settings.player_enabled);
  REXCVAR_SET(ragdoll_force_scale, double(settings.force_scale));
  g_ragdoll_settings_revision.fetch_add(1, std::memory_order_release);
}
uint64_t RagdollSettingsRevision() {
  return g_ragdoll_settings_revision.load(std::memory_order_acquire);
}

bool RagdollModEnabled() { return REXCVAR_GET(ragdoll_mod_enabled); }
void SetRagdollModEnabled(bool enabled) {
  auto settings = ReadRagdollSettings();
  settings.enabled = enabled;
  ApplyRagdollSettings(settings);
}
bool RagdollRunImpactEnabled() { return REXCVAR_GET(ragdoll_run_impact); }
void SetRagdollRunImpactEnabled(bool enabled) {
  auto settings = ReadRagdollSettings();
  settings.run_impact = enabled;
  ApplyRagdollSettings(settings);
}
bool RagdollPunchImpactEnabled() { return REXCVAR_GET(ragdoll_punch_impact); }
void SetRagdollPunchImpactEnabled(bool enabled) {
  auto settings = ReadRagdollSettings();
  settings.punch_impact = enabled;
  ApplyRagdollSettings(settings);
}
bool RagdollPushDownedEnabled() { return REXCVAR_GET(ragdoll_push_downed); }
void SetRagdollPushDownedEnabled(bool enabled) {
  auto settings = ReadRagdollSettings();
  settings.push_downed = enabled;
  ApplyRagdollSettings(settings);
}
bool RagdollPlayerEnabled() { return REXCVAR_GET(ragdoll_player_enabled); }
void SetRagdollPlayerEnabled(bool enabled) {
  auto settings = ReadRagdollSettings();
  settings.player_enabled = enabled;
  ApplyRagdollSettings(settings);
}
float RagdollForceScale() { return float(REXCVAR_GET(ragdoll_force_scale)); }
void SetRagdollForceScale(float scale) {
  if (!std::isfinite(scale)) return;
  auto settings = ReadRagdollSettings();
  settings.force_scale = std::clamp(scale, 0.25f, 4.0f);
  ApplyRagdollSettings(settings);
}

namespace {
entity_transform::Transform BuildRagdollTransform(const entity_transform::Transform& upright,
                                                  const std::array<float, 3>& pos, float pitch,
                                                  float yaw, float roll) {
  const float cy = std::cos(yaw), sy = std::sin(yaw);
  const float cp = std::cos(pitch), sp = std::sin(pitch);
  const float cr = std::cos(roll), sr = std::sin(roll);

  // Preserve original scale from upright basis
  const auto len3 = [](const entity_transform::Vec3& v) {
    const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    return (l > 0.05f && l < 20.0f) ? l : 1.0f;
  };
  const float sx = len3(upright.right);
  const float sy_scale = len3(upright.up);
  const float sz = len3(upright.forward);

  entity_transform::Transform tf{};
  tf.right = {
      (cy * cr + sy * sp * sr) * sx,
      (cp * sr) * sx,
      (-sy * cr + cy * sp * sr) * sx,
  };
  tf.up = {
      (-cy * sr + sy * sp * cr) * sy_scale,
      (cp * cr) * sy_scale,
      (sy * sr + cy * sp * cr) * sy_scale,
  };
  tf.forward = {
      (sy * cp) * sz,
      (-sp) * sz,
      (cy * cp) * sz,
  };
  tf.position = pos;
  return tf;
}
}  // namespace

bool TriggerEntityRagdoll(PPCContext& ctx, uint8_t* base, uint64_t lifetime,
                          const std::array<float, 3>& impulse, bool down_for_count) {
  if (!lifetime || !RagdollModEnabled() ||
      !std::all_of(impulse.begin(), impulse.end(), [](float v) { return std::isfinite(v); }))
    return false;
  const uint32_t entity = spawner::Address(lifetime);
  if (!entity || !spawner::Readable(entity, 160)) return false;
  const auto family = List(base);
  const bool player = std::any_of(family.begin(), family.end(),
      [&](const Player& item) { return item.entity == entity; });
  if ((player && !RagdollPlayerEnabled()) ||
      (!player && !entity_health::Get(ctx, base, entity))) return false;
  const auto tf = entity_transform::Get(ctx, base, entity);
  if (!tf) return false;

  const float scale = RagdollForceScale();
  std::lock_guard lock(g_features_mutex);
  if (!g_ragdolls.contains(lifetime) && g_ragdolls.size() >= 64) return false;
  auto& body = g_ragdolls[lifetime];
  if (body.lifetime == 0) {
    body.lifetime = lifetime;
    body.upright_basis = *tf;
    body.is_player = player;
    body.ground_y = tf->position[1];
    body.yaw = std::atan2(tf->forward[0], tf->forward[2]);
    body.pitch = 0.25f;
    body.roll = 0.0f;
  }
  body.velocity[0] += impulse[0] * scale;
  body.velocity[1] = std::max(body.velocity[1], std::max(1.8f, impulse[1] * scale));
  body.velocity[2] += impulse[2] * scale;
  body.angular_vel = {4.2f * scale, (impulse[0] - impulse[2]) * 0.8f, 2.6f * scale};
  body.timer = std::max(body.timer, down_for_count ? 30.0f : 3.2f);
  body.down_for_count = body.down_for_count || down_for_count;
  body.last_impact = g_feature_time;

  return true;
}

bool TriggerPlayerRagdoll(PPCContext& ctx, uint8_t* base, uint32_t player_slot) {
  const auto list = List(base);
  if (list.empty() || player_slot > 1 || !RagdollPlayerEnabled()) return false;
  const Player* chosen = nullptr;
  for (const auto& p : list) {
    if (p.input_player == int32_t(player_slot)) {
      chosen = &p;
      break;
    }
  }
  if (!chosen) return false;
  const uint64_t lt = spawner::Observe(chosen->entity);
  if (!lt) return false;
  const auto tf = entity_transform::Get(ctx, base, chosen->entity);
  const std::array<float, 3> impulse =
      tf ? std::array<float, 3>{-tf->forward[0] * 4.5f, 3.2f, -tf->forward[2] * 4.5f}
         : std::array<float, 3>{0.0f, 3.2f, -4.5f};
  const bool ok = TriggerEntityRagdoll(ctx, base, lt, impulse, false);
  if (ok) {
    std::lock_guard lock(g_features_mutex);
    g_ragdolls[lt].is_player = true;
    g_ragdolls[lt].timer = 1.85f;
  }
  return ok;
}

size_t TriggerNearbyRagdolls(PPCContext& ctx, uint8_t* base, float radius) {
  const auto list = List(base);
  if (list.empty()) return 0;
  const auto origin = list.front().position;
  size_t count = 0;
  const uint64_t avatar_lt = ActiveNpcAvatar().npc_lifetime;
  for (const auto& rec : spawner::Instances(base)) {
    if (rec.lifetime == avatar_lt) continue;
    const auto info = spawner::DescribeEntity(ctx, base, rec.lifetime);
    if (!info || !info->has_transform) continue;
    if (info->category != "People" && info->category != "Enemies" && info->category != "Bosses" &&
        !rec.name.starts_with("NPC"))
      continue;
    const float dx = info->position[0] - origin[0];
    const float dy = info->position[1] - origin[1];
    const float dz = info->position[2] - origin[2];
    const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (dist <= radius) {
      const float inv = dist > 0.1f ? (1.0f / dist) : 1.0f;
      if (TriggerEntityRagdoll(ctx, base, rec.lifetime,
                           {dx * inv * 5.2f, 2.8f, dz * inv * 5.2f}, false)) ++count;
    }
  }
  return count;
}

void ResetSessionState() {
  std::lock_guard lock(g_features_mutex);
  g_npc_avatar = {};
  g_pending_npc = {};
  g_player_motion.clear();
  g_avatar_player_lifetime = 0;
  g_avatar_hid_player = false;
  g_feature_clock.Reset();
  g_feature_time = 0;
  g_ragdolls.clear();
  g_pending_hit_entities.clear();
}

void TickFeatures(PPCContext& ctx, uint8_t* base) {
  const uint64_t game_us = (uint64_t(LoadGuestU32(base, 0x82D572C8)) << 32) |
                           LoadGuestU32(base, 0x82D572CC);
  const bool running = LoadGuestU32(base, 0x82CED760) && LoadGuestU32(base, 0x82D572DC) != 1;
  const float dt = g_feature_clock.Sample(game_us, running);
  const auto settings = ReadRagdollSettings();

  // Releasing a mod or disabling the effect restores live actors on the game
  // thread, even while paused. Never touch a recycled lifetime.
  std::map<uint64_t, RagdollBody> restore;
  {
    std::lock_guard lock(g_features_mutex);
    for (auto it = g_ragdolls.begin(); it != g_ragdolls.end();) {
      if (!settings.enabled || (it->second.is_player && !settings.player_enabled)) {
        restore.emplace(*it);
        it = g_ragdolls.erase(it);
      } else ++it;
    }
    if (!settings.enabled) {
      g_pending_hit_entities.clear();
      g_player_motion.clear();
    }
  }
  for (const auto& [lifetime, body] : restore) {
    const auto entity = spawner::Address(lifetime);
    if (!entity) continue;
    if (const auto current = entity_transform::Get(ctx, base, entity)) {
      auto upright = body.upright_basis;
      upright.position = current->position;
      entity_transform::Set(ctx, base, entity, upright);
    }
  }
  if (dt <= 0) return;
  g_feature_time += dt;
  {
    std::lock_guard lock(g_features_mutex);
    if (!settings.enabled && !g_npc_avatar.active && !g_pending_npc.active) return;
  }

  // 1. Check pending library NPC play-as request
  {
    PendingLibraryNpc pending;
    {
      std::lock_guard lock(g_features_mutex);
      if (g_pending_npc.active &&
          (g_pending_npc.stream.empty() || spawn_sources::IsReady(g_pending_npc.stream))) {
        pending = g_pending_npc;
        g_pending_npc.active = false;
      } else if (g_pending_npc.active &&
                 spawn_sources::DebugStatus().phase == spawn_sources::Phase::failed) {
        g_pending_npc.active = false;
      }
    }
    if (pending.active) {
      spawner::WarmCatalog(base);
      PlayAsLibraryNpc(ctx, base, pending.asset_index);
    }
  }

  // 2. Track player positions and velocities
  const auto player_list = List(base);
  std::erase_if(g_player_motion, [&](const auto& entry) {
    return std::none_of(player_list.begin(), player_list.end(),
                       [&](const Player& player) { return player.entity == entry.first; });
  });
  for (const auto& p : player_list) {
    auto& m = g_player_motion[p.entity];
    if (m.valid) {
      m.velocity = {
          (p.position[0] - m.prev_pos[0]) / dt,
          (p.position[1] - m.prev_pos[1]) / dt,
          (p.position[2] - m.prev_pos[2]) / dt,
      };
    }
    m.prev_pos = p.position;
    m.valid = true;
  }

  // 3. A costume preview follows exactly the family actor it hid. Restore
  // on a handover, native force-show, destruction or unsupported transform.
  const auto avatar = ActiveNpcAvatar();
  if (avatar.active) {
    uint64_t player_lifetime = 0;
    bool hid_player = false;
    {
      std::lock_guard lock(g_features_mutex);
      player_lifetime = g_avatar_player_lifetime;
      hid_player = g_avatar_hid_player;
    }
    const auto player = spawner::Address(player_lifetime);
    const auto npc = spawner::Address(avatar.npc_lifetime);
    const bool controlled = player && std::any_of(player_list.begin(), player_list.end(),
        [&](const Player& item) { return item.entity == player && item.input_player == 0; });
    const auto transform = controlled ? entity_transform::Get(ctx, base, player) : std::nullopt;
    if (!npc || !hid_player || !transform || !entity_transform::Set(ctx, base, npc, *transform))
      RestoreDefaultCharacter(ctx, base);
  }

  // 4. Experimental root-motion knockdowns
  if (!RagdollModEnabled()) return;

  std::vector<uint64_t> hit_entities;
  {
    std::lock_guard lock(g_features_mutex);
    hit_entities.swap(g_pending_hit_entities);
  }

  for (const auto& player : player_list) {
    const auto lifetime = spawner::Lifetime(player.entity);
    if (settings.player_enabled && lifetime &&
        std::find(hit_entities.begin(), hit_entities.end(), lifetime) != hit_entities.end())
      TriggerPlayerRagdoll(ctx, base, uint32_t(player.input_player));
  }
  const uint64_t avatar_lt = ActiveNpcAvatar().npc_lifetime;
  bool any_sprinting = false;
  if (RagdollRunImpactEnabled()) {
    for (const auto& p : player_list) {
      const auto& pm = g_player_motion[p.entity];
      if (std::hypot(pm.velocity[0], pm.velocity[2]) >= 2.9f) {
        any_sprinting = true;
        break;
      }
    }
  }

  if ((!hit_entities.empty() && RagdollPunchImpactEnabled() && !player_list.empty()) ||
      any_sprinting) {
    const auto instances = spawner::Instances(base);
    const auto ppos = player_list.empty() ? std::array<float, 3>{} : player_list.front().position;

    for (const auto& rec : instances) {
      if (rec.lifetime == avatar_lt || !spawner::Readable(rec.entity, 360)) continue;
      const uint32_t frame = LoadGuestU32(base, rec.entity + kFrame);
      if (!frame || !spawner::Readable(frame, kFramePosition + 12)) continue;
      const float ex = LoadGuestFloat(base, frame + kFramePosition + 0);
      const float ey = LoadGuestFloat(base, frame + kFramePosition + 4);
      const float ez = LoadGuestFloat(base, frame + kFramePosition + 8);
      if (!std::isfinite(ex) || !std::isfinite(ey) || !std::isfinite(ez)) continue;

      const auto health = entity_health::Get(ctx, base, rec.entity);
      const bool has_health = health.has_value();
      const bool dead = health && (health->dead || health->current <= 0);
      const bool is_player = std::any_of(player_list.begin(), player_list.end(),
          [&](const Player& player) { return player.entity == rec.entity; });
      if (is_player) continue;  // Player hit reactions are queued separately below.

      // 4a. Punch / damage impact ragdoll
      if (!hit_entities.empty() && RagdollPunchImpactEnabled() &&
          std::find(hit_entities.begin(), hit_entities.end(), rec.lifetime) != hit_entities.end()) {
        const float dx = ex - ppos[0];
        const float dz = ez - ppos[2];
        const float dist = std::max(0.2f, std::hypot(dx, dz));
        TriggerEntityRagdoll(ctx, base, rec.lifetime,
                             {(dx / dist) * 5.8f, 2.9f, (dz / dist) * 5.8f}, dead);
        continue;
      }

      // 4b. Sprint-into-NPC knockdown ragdoll
      if (any_sprinting && has_health && rec.name.starts_with("NPC")) {
        for (const auto& p : player_list) {
          const auto& pm = g_player_motion[p.entity];
          const float pspeed = std::hypot(pm.velocity[0], pm.velocity[2]);
          if (pspeed < 2.9f) continue;
          const float dx = ex - p.position[0];
          const float dy = std::abs(ey - p.position[1]);
          const float dz = ez - p.position[2];
          const float hdist = std::hypot(dx, dz);
          if (hdist < 1.25f && dy < 1.8f) {
            std::unique_lock lock(g_features_mutex);
            const auto it = g_ragdolls.find(rec.lifetime);
            if (it != g_ragdolls.end() &&
                g_feature_time - it->second.last_impact < 0.45)
              continue;
            lock.unlock();
            const float nx = hdist > 0.05f ? (dx / hdist) : 0.0f;
            const float nz = hdist > 0.05f ? (dz / hdist) : 1.0f;
            TriggerEntityRagdoll(
                ctx, base, rec.lifetime,
                {pm.velocity[0] * 0.75f + nx * 3.8f, 2.4f, pm.velocity[2] * 0.75f + nz * 3.8f},
                dead);
          }
        }
      }
    }
  }

  // Step bounded root motion. There is no skeleton or terrain collision solver here.
  std::vector<uint64_t> active_lts;
  {
    std::lock_guard lock(g_features_mutex);
    for (const auto& [lt, body] : g_ragdolls) active_lts.push_back(lt);
  }
  const float force_scale = RagdollForceScale();
  for (const uint64_t lt : active_lts) {
    const uint32_t entity = spawner::Address(lt);
    if (!entity || !spawner::Readable(entity, 160)) {
      std::lock_guard lock(g_features_mutex);
      g_ragdolls.erase(lt);
      continue;
    }
    const auto cur_tf = entity_transform::Get(ctx, base, entity);
    if (!cur_tf) {
      std::lock_guard lock(g_features_mutex);
      g_ragdolls.erase(lt);
      continue;
    }

    std::unique_lock lock(g_features_mutex);
    auto it = g_ragdolls.find(lt);
    if (it == g_ragdolls.end()) continue;
    RagdollBody& body = it->second;

    if (!body.is_player) {
      if (const auto h = entity_health::Get(ctx, base, entity); h && (h->dead || h->current <= 0.0f))
        body.down_for_count = true;
    }

    // Push downed ragdoll bodies (and defeated enemies down for the count) with player body!
    if (RagdollPushDownedEnabled() && !body.is_player) {
      for (const auto& p : player_list) {
        const float dx = cur_tf->position[0] - p.position[0];
        const float dy = std::abs(cur_tf->position[1] - p.position[1]);
        const float dz = cur_tf->position[2] - p.position[2];
        const float hdist = std::hypot(dx, dz);
        if (hdist < 1.35f && dy < 1.85f) {
          const auto& pm = g_player_motion[p.entity];
          const float nx = hdist > 0.05f ? (dx / hdist) : 1.0f;
          const float nz = hdist > 0.05f ? (dz / hdist) : 0.0f;
          const float overlap = 1.35f - hdist;
          body.velocity[0] = pm.velocity[0] * 0.85f + nx * (3.2f + overlap * 4.0f) * force_scale;
          body.velocity[2] = pm.velocity[2] * 0.85f + nz * (3.2f + overlap * 4.0f) * force_scale;
          body.velocity[1] = std::max(body.velocity[1], 0.85f);
          body.angular_vel[0] += 3.5f * force_scale;
          body.angular_vel[2] += (nx - nz) * 3.0f * force_scale;
          if (!body.down_for_count)
            body.timer = std::max(body.timer, 1.6f);
        }
      }
    }

    // Even defeated actors have a bound if their native despawn never arrives.
    body.timer -= dt;

    if (body.timer <= 0.0f) {
      // Recover and stand back up smoothly!
      entity_transform::Transform recovered = body.upright_basis;
      recovered.position = cur_tf->position;
      recovered.position[1] = std::max(recovered.position[1], body.ground_y);
      const auto copy_rec = recovered;
      g_ragdolls.erase(it);
      lock.unlock();
      if (entity_transform::Valid(copy_rec))
        entity_transform::Set(ctx, base, entity, copy_rec);
      continue;
    }

    // Integrate experimental whole-character tumbling.
    std::array<float, 3> pos = cur_tf->position;
    pos[0] += body.velocity[0] * dt;
    pos[1] += body.velocity[1] * dt;
    pos[2] += body.velocity[2] * dt;

    body.velocity[1] -= 18.0f * dt;
    if (pos[1] <= body.ground_y) {
      pos[1] = body.ground_y;
      if (body.velocity[1] < -2.0f)
        body.velocity[1] = -body.velocity[1] * 0.28f;
      else
        body.velocity[1] = 0.0f;
    }
    const float damping = std::exp(-4.5f * dt);
    body.velocity[0] *= damping;
    body.velocity[2] *= damping;

    body.writhe_phase += dt * 7.5f;
    const float writhe = body.down_for_count ? 0.0f : (0.12f * std::sin(body.writhe_phase));
    body.pitch = std::clamp(body.pitch + body.angular_vel[0] * dt, -1.48f, 1.48f);
    body.yaw += body.angular_vel[1] * dt;
    body.roll = std::clamp(body.roll + body.angular_vel[2] * dt, -1.2f, 1.2f);
    body.angular_vel[0] *= damping;
    body.angular_vel[1] *= damping;
    body.angular_vel[2] *= damping;

    // Settle toward ground prone/supine angle (~1.38 rad)
    body.pitch += (1.38f + writhe - body.pitch) * std::min(1.0f, dt * 6.0f);
    body.roll += (writhe * 0.7f - body.roll) * std::min(1.0f, dt * 4.0f);

    const auto ragdoll_tf =
        BuildRagdollTransform(body.upright_basis, pos, body.pitch, body.yaw, body.roll);
    lock.unlock();

    if (entity_transform::Valid(ragdoll_tf))
      entity_transform::Set(ctx, base, entity, ragdoll_tf);
  }
}

}  // namespace players

// Damage against the family is ignored while god mode is on.
REX_FUNC(sub_82957208) {
  const uint32_t component = ctx.r3.u32;
  bool is_player_health = false;
  for (const auto& player : players::List(base)) {
    if (player.health && player.health == component) {
      is_player_health = true;
      if (players::GodMode()) {
        ctx.r3.u64 = players::kDamageIgnored;
        return;
      }
      break;
    }
  }
  const auto target = players::RagdollModEnabled() && players::RagdollPunchImpactEnabled() &&
          (!is_player_health || players::RagdollPlayerEnabled())
      ? entity_health::ComponentOwner(base, component) : 0;
  const auto lifetime = target ? spawner::Observe(target) : 0;
  __imp__sub_82957208(ctx, base);
  if (ctx.r3.u32 == 0 && lifetime && spawner::Address(lifetime) == target) {
    std::lock_guard lock(players::g_features_mutex);
    if (players::g_pending_hit_entities.size() < 32 &&
        std::find(players::g_pending_hit_entities.begin(), players::g_pending_hit_entities.end(),
                  lifetime) == players::g_pending_hit_entities.end())
      players::g_pending_hit_entities.push_back(lifetime);
  }
}
