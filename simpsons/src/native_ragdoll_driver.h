#pragma once

#include "native_ragdoll_rig.h"

namespace native_ragdoll {

struct PoseDriverStats {
  bool enabled = false;
  uint64_t callbacks = 0, foreign_thread_callbacks = 0, writes = 0, failures = 0;
  uint64_t total_nanoseconds = 0, maximum_nanoseconds = 0;
};

// Game thread only. Attach the checked family skeleton to an already owned
// native biped. The caller retains the rig's ownership and must detach before
// releasing it. A lost rig fails closed without dereferencing old bone buffers.
// This drives bones after native animation generation; controller/collision
// handover and character recovery belong to the caller, not this adapter.
// Starts disabled. Enable only at a qualified game-thread update boundary.
uint64_t AttachFamilyPose(PPCContext& ctx, uint8_t* base, uint64_t rig,
                          const BipedRig& definition);
bool SetFamilyPoseEnabled(uint64_t driver, bool enabled);
bool DetachFamilyPose(uint64_t& driver);
std::optional<PoseDriverStats> FamilyPoseStats(uint64_t driver);

}  // namespace native_ragdoll
