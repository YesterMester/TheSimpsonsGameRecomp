#include "native_ragdoll_world.h"

#include "game_thread.h"
#include "guest_memory.h"
#include "host_memory.h"
#include "spawner.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>

#include <rex/hook.h>
#include <rex/logging.h>

namespace native_ragdoll {
namespace {
constexpr uint32_t kWorldTable = 0x8219A87C, kBodyTable = 0x8219AE44;
constexpr uint32_t kJointTable = 0x8219BEDC;
constexpr uint32_t kAddBody = 0x82AA2BB8, kRemoveBody = 0x82AA2DC0;
constexpr uint32_t kAddJoint = 0x82AA75F0, kRemoveJoint = 0x82AA77B0;
constexpr uint32_t kBodyInfo = 0x82AAFE30;

struct World {
  uint32_t address = 0;
  uint64_t generation = 0;
  bool destroying = false;
};
struct OwnedRig {
  uint64_t actor = 0, epoch = 0;
  World world;
  ActorCollisionFilter filter;
  std::vector<uint32_t> bodies, joints;
  bool active = false;
};

// Native hooks may re-enter through destruction callbacks. The lock protects
// host ownership; actual world mutations still require the game's unlocked
// update boundary and must never be queued into Havok's deferred-operation list.
std::recursive_mutex g_mutex;
std::map<uint32_t, World> g_worlds;
std::map<uint64_t, OwnedRig> g_rigs;
World g_current;
uint64_t g_generation = 0, g_next_rig = 0;
uint32_t g_scratch = 0;

bool WorldReadable(const uint8_t* base, uint32_t world) {
  return base && world && !(world & 15) && spawner::Readable(world, 1024) &&
         simpsons::LoadGuestU32(base, world) == kWorldTable;
}

bool Unlocked(const uint8_t* base, const World& world) {
  const auto found = g_worlds.find(world.address);
  return found != g_worlds.end() && found->second.generation == world.generation &&
         !found->second.destroying && WorldReadable(base, world.address) &&
         simpsons::LoadGuestU32(base, world.address + 128) == 0;
}

bool Scratch() {
  if (!g_scratch)
    g_scratch = host_memory::Alloc(kBodyInfoBytes);
  return g_scratch && spawner::Readable(g_scratch, kBodyInfoBytes);
}

bool BodyValid(const uint8_t* base, uint32_t body) {
  return base && body && !(body & 15) && spawner::Readable(body, kBodyBytes) &&
         simpsons::LoadGuestU32(base, body) == kBodyTable &&
         simpsons::LoadGuestU16(base, body + 4) == kBodyBytes &&
         simpsons::LoadGuestU16(base, body + 6) > 0;
}

bool JointValid(const uint8_t* base, uint32_t joint) {
  return base && joint && !(joint & 15) && spawner::Readable(joint, 44) &&
         simpsons::LoadGuestU32(base, joint) == kJointTable &&
         simpsons::LoadGuestU16(base, joint + 4) == 44 &&
         simpsons::LoadGuestU16(base, joint + 6) > 0;
}

bool Remove(PPCContext& ctx, uint8_t* base, const World& world, uint32_t routine, uint32_t object) {
  if (!Unlocked(base, world) || !Scratch())
    return false;
  simpsons::GuestToHost(base, g_scratch)[0] = 0;
  // Both removals return hkBool through a hidden first argument. Treating r3
  // as the world would corrupt the world, not merely fail the removal.
  return CallGuest(ctx, base, routine, {g_scratch, world.address, object}) &&
         simpsons::GuestToHost(base, g_scratch)[0] == 1;
}

bool Cleanup(PPCContext& ctx, uint8_t* base, OwnedRig& rig) {
  rig.active = false;
  // Constraints retain their endpoint bodies and data. Release them first,
  // even when construction or insertion failed partway through.
  for (auto& joint : rig.joints) {
    if (!joint)
      continue;
    if (!JointValid(base, joint))
      return false;
    if (simpsons::LoadGuestU32(base, joint + 8) &&
        !Remove(ctx, base, rig.world, kRemoveJoint, joint))
      return false;
    if (!ReleaseUnworldedRagdollJoint(ctx, base, joint))
      return false;
  }
  for (auto& body : rig.bodies) {
    if (!body)
      continue;
    if (!BodyValid(base, body))
      return false;
    const auto world = simpsons::LoadGuestU32(base, body + 8);
    if (world && (world != rig.world.address || !Remove(ctx, base, rig.world, kRemoveBody, body)))
      return false;
    if (!ReleaseUnworldedCapsuleBody(ctx, base, body))
      return false;
  }
  // Native bodies do not retain filter data. It must be the last allocation
  // freed, after every endpoint, reference and world membership is gone.
  return !rig.filter.storage || ReleaseActorCollisionFilter(ctx, base, rig.filter);
}

bool SourceAlive(const OwnedRig& rig) {
  return rig.actor && rig.epoch == spawner::WorldEpoch() && spawner::Address(rig.actor);
}

void Collect(PPCContext& ctx, uint8_t* base) {
  for (auto it = g_rigs.begin(); it != g_rigs.end();) {
    auto& rig = it->second;
    if ((!rig.active || !SourceAlive(rig)) && Cleanup(ctx, base, rig))
      it = g_rigs.erase(it);
    else
      ++it;
  }
}

bool Published(const uint8_t* base, const OwnedRig& rig) {
  if (!rig.active || !SourceAlive(rig) || !Unlocked(base, rig.world))
    return false;
  for (const auto body : rig.bodies)
    if (!BodyValid(base, body) || simpsons::LoadGuestU32(base, body + 8) != rig.world.address)
      return false;
  for (const auto joint : rig.joints)
    if (!JointValid(base, joint) || !simpsons::LoadGuestU32(base, joint + 8))
      return false;
  return true;
}

bool DefinitionValid(std::span<const CapsuleBodyInfo> bodies,
                     std::span<const JointDefinition> joints) {
  if (bodies.empty() || bodies.size() > 32 || joints.size() > 31 ||
      joints.size() + 1 != bodies.size())
    return false;
  // Require a connected tree. This excludes self-joints, duplicate links,
  // disconnected limbs and cycles before allocating any native object.
  std::vector<uint32_t> roots(bodies.size());
  for (uint32_t i = 0; i < roots.size(); ++i)
    roots[i] = i;
  auto root = [&](uint32_t node) {
    while (roots[node] != node)
      node = roots[node];
    return node;
  };
  for (const auto& body : bodies)
    if (body.collision_filter)
      return false;
  for (const auto& joint : joints) {
    if (joint.body_a >= bodies.size() || joint.body_b >= bodies.size() ||
        (joint.limits && !ValidJointLimits(*joint.limits)))
      return false;
    const auto a = root(joint.body_a), b = root(joint.body_b);
    if (a == b)
      return false;
    roots[a] = b;
  }
  return true;
}
}  // namespace

uint64_t CreateRig(PPCContext& ctx, uint8_t* base, uint64_t actor,
                   std::span<const CapsuleBodyInfo> bodies,
                   std::span<const JointDefinition> joints) {
  std::lock_guard lock(g_mutex);
  Collect(ctx, base);
  if (!base || !actor || !spawner::Address(actor) || !Unlocked(base, g_current) ||
      g_rigs.size() >= 8 || !DefinitionValid(bodies, joints) || !Scratch() ||
      std::any_of(g_rigs.begin(), g_rigs.end(),
                  [&](const auto& entry) { return entry.second.actor == actor; }))
    return 0;
  const auto id = ++g_next_rig;
  auto& rig = g_rigs[id];
  rig.actor = actor;
  rig.epoch = spawner::WorldEpoch();
  rig.world = g_current;
  rig.bodies.reserve(bodies.size());
  rig.joints.reserve(joints.size());
  const auto rollback = [&] {
    if (Cleanup(ctx, base, rig))
      g_rigs.erase(id);
    else
      REXLOG_ERROR("[ragdoll] Incomplete rig retained for safe native cleanup");
    return uint64_t(0);
  };
  try {
    const auto filter = CreateActorCollisionFilter(ctx, base, actor);
    if (!filter)
      return rollback();
    rig.filter = *filter;
    for (auto info : bodies) {
      info.collision_filter = rig.filter.token;
      const auto body = CreateUnworldedCapsuleBody(ctx, base, info);
      if (!body)
        return rollback();
      rig.bodies.push_back(body);
    }
    for (const auto& definition : joints) {
      const auto joint = CreateUnworldedRagdollJoint(ctx, base, rig.bodies[definition.body_a],
                                                     rig.bodies[definition.body_b],
                                                     definition.frame_a, definition.frame_b,
                                                     definition.limits ? &*definition.limits : nullptr);
      if (!joint)
        return rollback();
      rig.joints.push_back(joint);
    }
    if (!SourceAlive(rig))
      return rollback();
    for (const auto body : rig.bodies) {
      uint32_t result = 0;
      if (!Unlocked(base, rig.world) ||
          !CallGuest(ctx, base, kAddBody, {rig.world.address, body, 1}, &result) ||
          result != body || simpsons::LoadGuestU32(base, body + 8) != rig.world.address)
        return rollback();
    }
    for (const auto joint : rig.joints) {
      uint32_t result = 0;
      if (!Unlocked(base, rig.world) ||
          !CallGuest(ctx, base, kAddJoint, {rig.world.address, joint}, &result) || !result ||
          !simpsons::LoadGuestU32(base, joint + 8))
        return rollback();
    }
    rig.active = true;
    return id;
  } catch (...) {
    rollback();
    throw;
  }
}

std::optional<std::vector<SimulatedBody>> ReadRig(PPCContext& ctx, uint8_t* base, uint64_t id,
                                                bool verify_native) {
  std::lock_guard lock(g_mutex);
  const auto found = g_rigs.find(id);
  if (found == g_rigs.end() || !Published(base, found->second) || (verify_native && !Scratch()))
    return {};
  std::vector<SimulatedBody> result;
  result.reserve(found->second.bodies.size());
  for (const auto body : found->second.bodies) {
    // The native body transform contains a rigid matrix at +176, verified
    // against the Cinfo readback. Avoid converting its quaternion a second time.
    SimulatedBody pose;
    uint32_t at = body + 176;
    for (auto* column : {&pose.transform.right, &pose.transform.up, &pose.transform.forward,
                         &pose.transform.position}) {
      for (uint32_t axis = 0; axis < 3; ++axis) {
        (*column)[axis] = simpsons::LoadGuestFloat(base, at + axis * 4);
        if (!std::isfinite((*column)[axis]))
          return {};
      }
      at += 16;
    }
    for (uint32_t axis = 0; axis < 3; ++axis) {
      // 82AAFE30 copies these exact native vectors (+384/+400) into Cinfo
      // +48/+64. Avoid its shape/inertia virtual calls and a guest-call frame
      // per limb when an animation callback only needs current pose values.
      pose.linear_velocity[axis] = simpsons::LoadGuestFloat(base, body + 384 + axis * 4);
      pose.angular_velocity[axis] = simpsons::LoadGuestFloat(base, body + 400 + axis * 4);
      if (!std::isfinite(pose.linear_velocity[axis]) || !std::isfinite(pose.angular_velocity[axis]))
        return {};
    }
    if (verify_native &&
        (!CallGuest(ctx, base, kBodyInfo, {body, g_scratch}) ||
         std::memcmp(simpsons::GuestToHost(base, body + 224),
                     simpsons::GuestToHost(base, g_scratch + 16), 12) ||
         std::memcmp(simpsons::GuestToHost(base, body + 384),
                     simpsons::GuestToHost(base, g_scratch + 48), 12) ||
         std::memcmp(simpsons::GuestToHost(base, body + 400),
                     simpsons::GuestToHost(base, g_scratch + 64), 12)))
      return {};
    result.push_back(pose);
  }
  if (!Published(base, found->second))
    return {};
  return result;
}

bool ReleaseRig(PPCContext& ctx, uint8_t* base, uint64_t& id) {
  std::lock_guard lock(g_mutex);
  const auto found = g_rigs.find(id);
  if (found != g_rigs.end() && !Cleanup(ctx, base, found->second))
    return false;
  if (found != g_rigs.end())
    g_rigs.erase(found);
  id = 0;
  return true;
}

void BeforeWorldStep(PPCContext& ctx, uint8_t* base, uint32_t world) {
  std::lock_guard lock(g_mutex);
  if (!WorldReadable(base, world))
    return;
  auto [found, added] = g_worlds.try_emplace(world);
  if (added)
    found->second = {world, ++g_generation, false};
  if (found->second.destroying)
    return;
  g_current = found->second;
  Collect(ctx, base);
}

uint64_t RigActor(uint64_t id) {
  std::lock_guard lock(g_mutex);
  const auto found = g_rigs.find(id);
  return found != g_rigs.end() && found->second.active && SourceAlive(found->second)
      ? found->second.actor : 0;
}

bool SnapshotReady() {
  std::lock_guard lock(g_mutex);
  return g_rigs.empty();
}

void MemoryRestored() {
  std::lock_guard lock(g_mutex);
  // The caller rejected the rewind if any native rig still existed. No
  // destructor may be called on allocations from the discarded guest heaps.
  g_worlds.clear();
  g_current = {};
  ++g_generation;
}

void WorldDestroying(uint32_t world) {
  std::lock_guard lock(g_mutex);
  const auto found = g_worlds.find(world);
  if (found != g_worlds.end())
    found->second.destroying = true;
  if (g_current.address == world)
    g_current = {};
  for (auto& [id, rig] : g_rigs)
    if (rig.world.address == world)
      rig.active = false;
}

void WorldDestroyed(PPCContext& ctx, uint8_t* base, uint32_t world) {
  std::lock_guard lock(g_mutex);
  // The original destructor removed world references. Our references kept
  // bodies/joints/filter alive until then; now release in dependency order.
  g_worlds.erase(world);
  Collect(ctx, base);
}
}  // namespace native_ragdoll

REX_EXTERN(__imp__sub_82AA43F0);
REX_HOOK_RAW(sub_82AA43F0) {
  // This is the full hkWorld destructor, reached by its deleting destructor
  // 82AABF80. The vtable's second entry (82AA2250) only reports memory usage.
  const auto world = ctx.r3.u32;
  native_ragdoll::WorldDestroying(world);
  __imp__sub_82AA43F0(ctx, base);
  native_ragdoll::WorldDestroyed(ctx, base, world);
}
