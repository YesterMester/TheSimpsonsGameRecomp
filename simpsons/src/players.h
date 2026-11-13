#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct PPCContext;
namespace spawner {
struct Spawnable;
}

// The family members in play, for the debug menu (players.cpp). Game thread.
namespace players {

struct Player {
  uint32_t entity = 0;     // the player entity
  uint32_t character = 0;  // its character id (+3460)
  uint32_t frame = 0;      // its RenderWare frame (+140)
  uint32_t health = 0;     // its health component (+344)
  std::array<float, 3> position{};
  float hit_points = 0, max_hit_points = 0;
  int32_t input_player = -1;  // logical port 0/1; -1 for AI/unbound/ambiguous
};

struct NpcAvatarInfo {
  bool active = false;
  std::string name;
  std::string class_name;
  std::string category;
  std::string stream;
  uint64_t npc_lifetime = 0;
  uint32_t entity = 0;
  uint32_t player_entity = 0;
  uint32_t theme_color = 0xFFD54A;
};

std::vector<Player> List(const uint8_t* base);
// Homer, Bart, Lisa or Marge.
const char* CharacterName(uint32_t character);

// Any thread.
void SetGodMode(bool on);
bool GodMode();
uint64_t GodModeRevision();

// Game thread.
void RefillHealth(PPCContext& ctx, uint8_t* base);
bool MoveTo(PPCContext& ctx, uint8_t* base, const Player& player,
            const std::array<float, 3>& position);

// Experimental NPC costume preview over the controlled family character.
// This does not transfer NPC AI, attacks or abilities to the player.
NpcAvatarInfo ActiveNpcAvatar();
bool PlayAsNpc(PPCContext& ctx, uint8_t* base, const spawner::Spawnable& target);
bool PlayAsLibraryNpc(PPCContext& ctx, uint8_t* base, size_t asset_index);
bool RestoreDefaultCharacter(PPCContext& ctx, uint8_t* base);
bool TriggerNpcAvatarAbility(PPCContext& ctx, uint8_t* base);

// Experimental root-motion knockdowns. These are not jointed Havok ragdolls.
// A mod leases these settings through the host resource service. Without a
// mod or an explicit debug request, normal gameplay never enables the effect.
struct RagdollSettings {
  bool enabled = false, run_impact = true, punch_impact = true;
  bool push_downed = true, player_enabled = true;
  float force_scale = 1.0f;
};
RagdollSettings ReadRagdollSettings();
void ApplyRagdollSettings(const RagdollSettings& settings);
uint64_t RagdollSettingsRevision();
bool RagdollModEnabled();
void SetRagdollModEnabled(bool enabled);
bool RagdollRunImpactEnabled();
void SetRagdollRunImpactEnabled(bool enabled);
bool RagdollPunchImpactEnabled();
void SetRagdollPunchImpactEnabled(bool enabled);
bool RagdollPushDownedEnabled();
void SetRagdollPushDownedEnabled(bool enabled);
bool RagdollPlayerEnabled();
void SetRagdollPlayerEnabled(bool enabled);
float RagdollForceScale();
void SetRagdollForceScale(float scale);
bool TriggerEntityRagdoll(PPCContext& ctx, uint8_t* base, uint64_t lifetime,
                          const std::array<float, 3>& impulse, bool down_for_count = false);
bool TriggerPlayerRagdoll(PPCContext& ctx, uint8_t* base, uint32_t player_slot = 0);
size_t TriggerNearbyRagdolls(PPCContext& ctx, uint8_t* base, float radius = 8.0f);
// The original force-show explicitly cancels every outstanding hide request.
void VisibilityReset(uint64_t lifetime);
void ResetSessionState();
void TickFeatures(PPCContext& ctx, uint8_t* base);

// Development: logs the players' fields every few seconds (debug_menu_log_players).
void Log(PPCContext& ctx, uint8_t* base);

}  // namespace players
