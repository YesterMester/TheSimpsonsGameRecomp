#pragma once

#include "entity_transform.h"

#include <cstdint>

struct PPCContext;

namespace native_ragdoll {

// hkRagdollConstraintData in this executable: two local joint frames, twist,
// cone/plane limits and optional motors. Storage is 16-byte aligned and belongs
// to the caller. This adapter does not allocate bodies, add constraints to the
// world, drive a skeleton or provide character ragdoll/recovery behavior.
inline constexpr uint32_t kJointBytes = 320;

// Angles are radians in the native joint frame: X is twist; the native cone
// and plane atoms bound swing. These
// are authored limits, not recovered anatomical data. Leave the pointer null
// to keep the original constructor's defaults. Motors remain disabled.
struct JointLimits {
  float twist_min = -0.5235988f, twist_max = 0.5235988f;
  float cone_max = 1.0f;
  float plane_min = -0.025f, plane_max = 0.025f;
  float tau = 0.8f;
};
bool ValidJointLimits(const JointLimits& limits);

// Only fresh, unpublished storage may be initialized. Both transforms are
// rigid body-local joint frames; scaled or reflected model matrices are not
// Havok rotations. Rejection leaves the storage untouched.
bool InitializeJoint(PPCContext& ctx, uint8_t* base, uint32_t storage,
                     const entity_transform::Transform& body_a,
                     const entity_transform::Transform& body_b,
                     const JointLimits* limits = nullptr);
// Runs the game's real joint validator, including its native limit checks.
bool JointValid(PPCContext& ctx, uint8_t* base, uint32_t storage);

}  // namespace native_ragdoll
