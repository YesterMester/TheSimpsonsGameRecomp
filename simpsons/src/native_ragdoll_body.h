#pragma once

#include "entity_transform.h"

#include <array>
#include <cstdint>
#include <optional>

struct PPCContext;

namespace native_ragdoll {

inline constexpr uint32_t kCapsuleBytes = 48;
inline constexpr uint32_t kBodyInfoBytes = 192;
inline constexpr uint32_t kBodyBytes = 512;

// A solid capsule along its local Y axis. The hemispheres' centers are at
// +/-half_length; position and rotation place its center in the physics world.
// Units match the game's Havok world. Rotation is a unit quaternion (x,y,z,w).
struct CapsuleBodyInfo {
  float radius = 0.15f, half_length = 0.25f, mass = 1.0f;
  std::array<float, 3> position{}, linear_velocity{}, angular_velocity{};
  std::array<float, 4> rotation{0, 0, 0, 1};
  // Zero, or an owned ActorCollisionFilter's token. The game uses a tagged
  // filter format; stock Havok group-filter bitfields are not compatible.
  uint32_t collision_filter = 0;
};

struct ActorCollisionFilter {
  uint32_t storage = 0, token = 0;
  uint64_t actor = 0;
};
struct JointLimits;
// Game thread only. Native allocation/group identity and native filter-data
// constructors. Bodies sharing the actor's group ignore each other and the
// original actor's colliders while respecting other native collision groups.
// This raw filter data is not reference counted by hkRigidBody. Keep it alive
// until all bodies using it have left their world and have been destroyed.
std::optional<ActorCollisionFilter> CreateActorCollisionFilter(PPCContext& ctx, uint8_t* base,
                                                               uint64_t actor);
bool ReleaseActorCollisionFilter(PPCContext& ctx, uint8_t* base, ActorCollisionFilter& filter);

// Construct the real capsule shape and hkRigidBodyCinfo in fresh, unpublished,
// 16-byte-aligned storage. Both regions belong to the caller and must be
// separate. No object is allocated or added to a world by this operation.
// The shape is embedded (allocation size zero); a native-heap owner must set
// its real allocation size before giving the shape to a reference-counted body.
// Failed validation/construction leaves both regions unchanged.
bool InitializeCapsuleBodyInfo(PPCContext& ctx, uint8_t* base, uint32_t shape_storage,
                               uint32_t info_storage, const CapsuleBodyInfo& info);

// Game thread only: construct a native-heap hkRigidBody with one owned
// reference. It is deliberately outside the world until its rig, collision
// filters and constraints are ready. The caller must release that reference
// before world/source teardown; this is not a script-visible entity handle.
uint32_t CreateUnworldedCapsuleBody(PPCContext& ctx, uint8_t* base, const CapsuleBodyInfo& info);
// Accept only an unpublished body with its sole reference. Never remove a
// live world object or manually free the shape retained by its constructor.
bool ReleaseUnworldedCapsuleBody(PPCContext& ctx, uint8_t* base, uint32_t& body);

// Build an actual hkConstraintInstance joining two distinct, unpublished
// bodies. The instance owns references to both bodies and the joint data;
// release it before releasing the bodies. Frames are rigid body-local frames.
uint32_t CreateUnworldedRagdollJoint(PPCContext& ctx, uint8_t* base, uint32_t body_a,
                                     uint32_t body_b, const entity_transform::Transform& frame_a,
                                     const entity_transform::Transform& frame_b,
                                     const JointLimits* limits = nullptr);
bool ReleaseUnworldedRagdollJoint(PPCContext& ctx, uint8_t* base, uint32_t& joint);

}  // namespace native_ragdoll
