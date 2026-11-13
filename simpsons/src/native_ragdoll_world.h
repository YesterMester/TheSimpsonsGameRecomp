#pragma once

#include "native_ragdoll_body.h"
#include "native_ragdoll_joint.h"

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace native_ragdoll {

struct JointDefinition {
  uint32_t body_a = 0, body_b = 0;
  entity_transform::Transform frame_a, frame_b;
  std::optional<JointLimits> limits;
};

struct SimulatedBody {
  entity_transform::Transform transform;
  entity_transform::Vec3 linear_velocity, angular_velocity;
};

// Game thread only. The host handle owns the entire native rig, including the
// raw game collision filter. No body/world pointers escape to scripts. Bodies
// and joints are assembled outside the world before publication. An incomplete
// insertion remains privately owned for cleanup; zero is never a partial rig.
// All bodies use the source actor's native collision group. The caller's body
// definitions must have collision_filter == 0. Limits are 8 rigs, 32 bodies and
// 31 joints per rig; an actor can have only one owned rig at a time.
uint64_t CreateRig(PPCContext& ctx, uint8_t* base, uint64_t actor,
                   std::span<const CapsuleBodyInfo> bodies,
                   std::span<const JointDefinition> joints);
// The native motion already holds the current rigid transform and velocities.
// verify_native also compares them with the original Cinfo getter; use that
// slower diagnostic path for qualification, not every animation callback.
std::optional<std::vector<SimulatedBody>> ReadRig(PPCContext& ctx, uint8_t* base, uint64_t rig,
                                               bool verify_native = false);
// Source identity for checked internal pose binding; zero after retirement.
uint64_t RigActor(uint64_t rig);
// Marks the rig inactive immediately. Returns true and clears the handle only
// after joints, bodies and filter are all released. A locked world defers the
// cleanup to its next safe step. Releasing an already retired handle is safe.
bool ReleaseRig(PPCContext& ctx, uint8_t* base, uint64_t& rig);

// Called from the existing hkWorld step hook, before the original step. This
// is also the cleanup boundary for lost actors and source/world changes.
void BeforeWorldStep(PPCContext& ctx, uint8_t* base, uint32_t world);

// Exact memory snapshots cannot include native objects whose ownership lives
// outside the restored guest heaps. Refuse capture/restore until rigs retire;
// never free stale guest handles after a memory rewind.
bool SnapshotReady();
void MemoryRestored();

}  // namespace native_ragdoll
