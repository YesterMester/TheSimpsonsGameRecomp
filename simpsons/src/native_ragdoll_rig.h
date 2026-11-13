#pragma once

#include "native_ragdoll_pose.h"
#include "native_ragdoll_world.h"

#include <optional>
#include <span>
#include <vector>

namespace native_ragdoll {

struct BoneBinding {
  uint32_t body = 0;
  // The entire authored affine bone frame, relative to a rigid physics body.
  // Keeping the basis, not just its position, preserves scale and helper bones.
  entity_transform::Transform body_to_bone;
};

struct BipedRig {
  SkeletonPose source;
  std::vector<CapsuleBodyInfo> bodies;
  std::vector<JointDefinition> joints;
  // One binding per animation track. Model-only nodes are never overwritten.
  std::vector<BoneBinding> bindings;
};

// Build a 13-body connected biped from the native named skeleton snapshot.
// Dimensions come from the current pose in world units. Mass fractions and
// capsule proportions are authored defaults, not extracted collision assets.
// Missing/mismatched anatomy and degenerate frames fail without allocations in
// the game. This does not suspend a controller or publish anything to Havok.
std::optional<BipedRig> BuildBipedRig(const SkeletonPose& pose, float total_mass,
                                     const entity_transform::Vec3& velocity = {});

// Convert actual simulated bodies back into the render root's model space.
// The actor may have moved or changed scale since construction. Caller must
// still verify the actor/world/model identity before publishing the palette.
std::optional<std::vector<BonePose>> EvaluateBipedRig(
    const BipedRig& rig, std::span<const SimulatedBody> bodies,
    const entity_transform::Transform& render_root);

}  // namespace native_ragdoll
