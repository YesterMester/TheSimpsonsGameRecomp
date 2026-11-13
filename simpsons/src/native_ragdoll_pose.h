#pragma once

#include "entity_transform.h"

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

struct PPCContext;

namespace native_ragdoll {

struct BonePose {
  uint32_t id = 0, model_index = 0;
  int32_t parent = -1;
  // Current animated model-local pose, including authored scale. This is
  // neither a bind pose nor a ready-made Havok rigid transform.
  entity_transform::Transform model_transform;
};

struct SkeletonPose {
  uint64_t actor = 0, world_epoch = 0;
  uint32_t character = 0, model_nodes = 0;
  std::array<uint32_t, 4> model_guid{};
  entity_transform::Transform world_transform;
  // Native animation-track order. Parents precede children; model-only nodes
  // with no animation track remain outside this list and must be preserved.
  std::vector<BonePose> bones;
};

// Game-thread snapshot of a currently enumerated family actor. Resolves its
// native skinned clump, hierarchy and animation mapping instead of guessing
// bone names/order. Copies values only; no guest buffer ownership escapes.
// Unsupported/incomplete rigs or a changed actor/resource return no result.
// NPC controller/rig qualification is separate; this does not enable ragdolls.
std::optional<SkeletonPose> ReadFamilySkeleton(PPCContext& ctx, uint8_t* base, uint64_t actor);

// Game thread only, after native animation generation. Revalidates the actor,
// world, model, hierarchy and complete track mapping before writing anything.
// pose.world_transform must match the current render root. Only the spatial
// components of mapped animated bones are changed; native padding and model-
// only nodes stay byte-identical. A rejected write leaves the palette intact.
// This is a pose writer, not controller suspension or a get-up animation.
bool WriteFamilySkeleton(PPCContext& ctx, uint8_t* base, const SkeletonPose& pose);

}  // namespace native_ragdoll
