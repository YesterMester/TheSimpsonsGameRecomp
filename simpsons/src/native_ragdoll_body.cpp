#include "native_ragdoll_body.h"

#include "game_thread.h"
#include "guest_memory.h"
#include "native_ragdoll_joint.h"
#include "spawner.h"

#include <bit>
#include <cmath>
#include <cstring>
#include <limits>

#include <rex/ppc/context.h>

namespace native_ragdoll {
namespace {
constexpr uint32_t kCapsuleConstruct = 0x82AFB470, kInfoConstruct = 0x82AB0978;
constexpr uint32_t kCapsuleTable = 0x8219F0A4;
constexpr uint32_t kBodyConstruct = 0x82AB0288, kBodyTable = 0x8219AE44;
constexpr uint32_t kRouterTlsSlot = 0x82E29800, kTlsGetValue = 0x82CC2EF4;
constexpr uint32_t kAllocate = 0x82B640F8, kFree = 0x82B64190, kRelease = 0x82AAC0A0;
constexpr uint32_t kInstanceConstruct = 0x82AB3940, kInstanceTable = 0x8219BEDC;
constexpr uint32_t kInstanceBytes = 44;
constexpr uint32_t kActorGroup = 0x8269BAF8, kFilterConstruct = 0x827A4D28;
constexpr uint32_t kFilterEncode = 0x827A4A90, kFilterBytes = 20;

bool FilterValid(const uint8_t* base, uint32_t token) {
  if (!token)
    return true;
  if (!base || (token >> 30) != 1)
    return false;
  const uint32_t at = token << 2;
  if (!at || !spawner::Readable(at, kFilterBytes))
    return false;
  const auto bits = simpsons::LoadGuestU32(base, at);
  return (bits & 0xF000FFFFu) == 992 && (bits & 0x0FFF0000u) &&
         simpsons::LoadGuestU32(base, at + 4) == UINT32_MAX &&
         simpsons::LoadGuestU32(base, at + 8) == UINT32_MAX &&
         simpsons::LoadGuestU32(base, at + 12) == 0 && simpsons::LoadGuestU32(base, at + 16) == 0;
}

bool UnworldedBody(const uint8_t* base, uint32_t body) {
  if (!base || !body || (body & 15) || !spawner::Readable(body, kBodyBytes) ||
      simpsons::LoadGuestU32(base, body) != kBodyTable ||
      simpsons::LoadGuestU16(base, body + 4) != kBodyBytes ||
      simpsons::LoadGuestU32(base, body + 8))
    return false;
  const auto references = simpsons::LoadGuestU16(base, body + 6);
  return references && references < 32767;
}

void AllocationSize(uint8_t* base, uint32_t object, uint16_t bytes) {
  const auto big_endian = std::byteswap(bytes);
  std::memcpy(simpsons::GuestToHost(base, object + 4), &big_endian, sizeof(big_endian));
}

uint32_t MemoryRouter(PPCContext& ctx, uint8_t* base) {
  uint32_t router = 0;
  if (!base || !spawner::Readable(kRouterTlsSlot, 4) ||
      !CallGuest(ctx, base, kTlsGetValue, {simpsons::LoadGuestU32(base, kRouterTlsSlot)},
                 &router) ||
      !router || !spawner::Readable(router, 1024))
    return 0;
  return router;
}

uint32_t Allocate(PPCContext& ctx, uint8_t* base, uint32_t router, uint32_t bytes,
                  uint32_t category) {
  uint32_t at = 0;
  if (!CallGuest(ctx, base, kAllocate, {router, bytes, category}, &at))
    return 0;
  // The native allocator promises aligned, writable memory. A violation is
  // an engine contract failure, not storage which another heap may repair.
  if (!at || (at & 15) || !spawner::Readable(at, bytes))
    return 0;
  return at;
}

bool Free(PPCContext& ctx, uint8_t* base, uint32_t router, uint32_t at, uint32_t bytes,
          uint32_t category) {
  return !at || CallGuest(ctx, base, kFree, {router, at, bytes, category});
}

bool RegionAvailable(const PPCContext& ctx, uint32_t at, uint32_t bytes) {
  if (!at || (at & 15) || !spawner::Readable(at, bytes) || ctx.r1.u32 < 0x1000 ||
      (ctx.r1.u32 & 15) || !spawner::Readable(ctx.r1.u32 - 0x800, 0x840))
    return false;
  return uint64_t(at) + bytes <= ctx.r1.u32 - 0x800 || uint64_t(at) >= uint64_t(ctx.r1.u32) + 0x40;
}

bool CapsuleInertia(const CapsuleBodyInfo& info, std::array<float, 3>& diagonal) {
  if (!std::isfinite(info.radius) || info.radius <= 0 || !std::isfinite(info.half_length) ||
      info.half_length < 0 || !std::isfinite(info.mass) || info.mass <= 0)
    return false;
  double norm = 0;
  for (const auto value : info.rotation) {
    if (!std::isfinite(value))
      return false;
    norm += double(value) * value;
  }
  if (std::abs(norm - 1) > 1e-4)
    return false;
  for (const auto& vector : {info.position, info.linear_velocity, info.angular_velocity})
    for (const auto value : vector)
      if (!std::isfinite(value))
        return false;

  // Integrate a uniform-density cylinder and its two solid hemispheres about
  // the capsule center. The 3*length*radius/8 term includes the hemispheres'
  // displaced centers of mass; treating them as a sphere at each endpoint
  // gives the wrong transverse inertia.
  const double radius = info.radius, length = 2.0 * info.half_length;
  const double extent = radius + info.half_length;
  // Havok computes AABB diagonals and reciprocal mass/inertia in float32.
  // Finite inputs alone do not prevent overflow or flush-to-zero there.
  if (4 * (2 * radius * radius + extent * extent) > std::numeric_limits<float>::max() ||
      radius * radius < std::numeric_limits<float>::min())
    return false;
  for (const auto value : info.position)
    if (std::abs(double(value)) + extent > std::numeric_limits<float>::max())
      return false;
  const double cylinder_mass = info.mass * length / (length + 4.0 * radius / 3.0);
  const double caps_mass = info.mass - cylinder_mass;
  const double axial = (0.5 * cylinder_mass + 0.4 * caps_mass) * radius * radius;
  const double transverse =
      cylinder_mass * (3 * radius * radius + length * length) / 12 +
      caps_mass * (0.4 * radius * radius + 3 * length * radius / 8 + length * length / 4);
  diagonal = {float(transverse), float(axial), float(transverse)};
  for (const auto value : diagonal)
    if (!std::isfinite(value) || value < std::numeric_limits<float>::min() ||
        !std::isfinite(1.0f / value) || 1.0f / value < std::numeric_limits<float>::min())
      return false;
  return std::isfinite(1.0f / info.mass) && 1.0f / info.mass >= std::numeric_limits<float>::min();
}

template <size_t Count>
void StoreVector(uint8_t* base, uint32_t at, const std::array<float, Count>& vector) {
  for (uint32_t index = 0; index < 4; ++index)
    simpsons::StoreGuestFloat(base, at + index * 4, index < Count ? vector[index] : 0);
}
}  // namespace

std::optional<ActorCollisionFilter> CreateActorCollisionFilter(PPCContext& ctx, uint8_t* base,
                                                               uint64_t actor) {
  const auto entity = spawner::Address(actor);
  if (!base || !actor || !entity || !spawner::Readable(entity, 24))
    return {};
  const auto router = MemoryRouter(ctx, base);
  uint32_t group = 0;
  if (!router || !CallGuest(ctx, base, kActorGroup, {entity}, &group) || !group || group > 4095)
    return {};
  const auto storage = Allocate(ctx, base, router, kFilterBytes, 21);
  if (!storage)
    return {};
  uint32_t token = 0;
  if (!CallGuest(ctx, base, kFilterConstruct, {storage, 0, group, UINT32_MAX, UINT32_MAX, 0, 0}) ||
      !CallGuest(ctx, base, kFilterEncode, {storage}, &token) || !FilterValid(base, token) ||
      spawner::Address(actor) != entity) {
    Free(ctx, base, router, storage, kFilterBytes, 21);
    return {};
  }
  return ActorCollisionFilter{storage, token, actor};
}

bool ReleaseActorCollisionFilter(PPCContext& ctx, uint8_t* base, ActorCollisionFilter& filter) {
  if (!base || !filter.storage || (filter.storage & 15) ||
      !spawner::Readable(filter.storage, kFilterBytes) ||
      filter.token != ((filter.storage >> 2) | 0x40000000u))
    return false;
  const auto router = MemoryRouter(ctx, base);
  if (!router || !Free(ctx, base, router, filter.storage, kFilterBytes, 21))
    return false;
  filter = {};
  return true;
}

bool InitializeCapsuleBodyInfo(PPCContext& ctx, uint8_t* base, uint32_t shape, uint32_t config,
                               const CapsuleBodyInfo& info) {
  std::array<float, 3> inertia;
  if (!base || !FilterValid(base, info.collision_filter) ||
      !RegionAvailable(ctx, shape, kCapsuleBytes) ||
      !RegionAvailable(ctx, config, kBodyInfoBytes) ||
      (uint64_t(shape) < uint64_t(config) + kBodyInfoBytes &&
       uint64_t(config) < uint64_t(shape) + kCapsuleBytes) ||
      !CapsuleInertia(info, inertia))
    return false;
  std::array<uint8_t, kCapsuleBytes> previous_shape;
  std::array<uint8_t, kBodyInfoBytes> previous_config;
  std::memcpy(previous_shape.data(), simpsons::GuestToHost(base, shape), kCapsuleBytes);
  std::memcpy(previous_config.data(), simpsons::GuestToHost(base, config), kBodyInfoBytes);
  const auto restore = [&] {
    std::memcpy(simpsons::GuestToHost(base, shape), previous_shape.data(), kCapsuleBytes);
    std::memcpy(simpsons::GuestToHost(base, config), previous_config.data(), kBodyInfoBytes);
  };
  try {
    std::memset(simpsons::GuestToHost(base, shape), 0, kCapsuleBytes);
    std::memset(simpsons::GuestToHost(base, config), 0, kBodyInfoBytes);
    // Reuse the unpublished configuration as aligned endpoint scratch; the
    // original Cinfo constructor initializes it after the capsule has read it.
    StoreVector(base, config, std::array<float, 3>{0, -info.half_length, 0});
    StoreVector(base, config + 16, std::array<float, 3>{0, info.half_length, 0});
    PPCContext call = ctx;
    call.f1.f64 = info.radius;
    if (CallGuest(call, base, kCapsuleConstruct, {shape, config, config + 16}) &&
        simpsons::LoadGuestU32(base, shape) == kCapsuleTable &&
        CallGuest(ctx, base, kInfoConstruct, {config})) {
      simpsons::StoreGuestU32(base, config + 4, shape);
      simpsons::StoreGuestU32(base, config, info.collision_filter);
      StoreVector(base, config + 16, info.position);
      StoreVector(base, config + 32, info.rotation);
      StoreVector(base, config + 48, info.linear_velocity);
      StoreVector(base, config + 64, info.angular_velocity);
      for (uint32_t column = 0; column < 3; ++column) {
        std::array<float, 3> axis{};
        axis[column] = inertia[column];
        StoreVector(base, config + 80 + column * 16, axis);
      }
      StoreVector(base, config + 128, std::array<float, 3>{});
      simpsons::StoreGuestFloat(base, config + 144, info.mass);
      return true;
    }
  } catch (...) {
    restore();
    throw;
  }
  restore();
  return false;
}

uint32_t CreateUnworldedCapsuleBody(PPCContext& ctx, uint8_t* base, const CapsuleBodyInfo& info) {
  std::array<float, 3> inertia;
  if (!FilterValid(base, info.collision_filter) || !CapsuleInertia(info, inertia) ||
      ctx.r1.u32 < 0x1000 || (ctx.r1.u32 & 15) || !spawner::Readable(ctx.r1.u32 - 0x800, 0x840))
    return 0;
  const auto router = MemoryRouter(ctx, base);
  if (!router)
    return 0;
  const auto shape = Allocate(ctx, base, router, kCapsuleBytes, 37);
  if (!shape)
    return 0;
  const auto config = Allocate(ctx, base, router, kBodyInfoBytes, 21);
  if (!config) {
    Free(ctx, base, router, shape, kCapsuleBytes, 37);
    return 0;
  }
  if (!InitializeCapsuleBodyInfo(ctx, base, shape, config, info)) {
    Free(ctx, base, router, config, kBodyInfoBytes, 21);
    Free(ctx, base, router, shape, kCapsuleBytes, 37);
    return 0;
  }
  // Native reference counting is enabled only for actual allocator-owned
  // objects. The body constructor retains the shape; release our temporary
  // shape reference after construction, leaving its lifetime with the body.
  AllocationSize(base, shape, kCapsuleBytes);
  const auto body = Allocate(ctx, base, router, kBodyBytes, 43);
  bool constructed = false;
  if (body) {
    std::memset(simpsons::GuestToHost(base, body), 0, kBodyBytes);
    AllocationSize(base, body, kBodyBytes);
    constructed = CallGuest(ctx, base, kBodyConstruct, {body, config});
    if (!constructed)
      Free(ctx, base, router, body, kBodyBytes, 43);
  }
  Free(ctx, base, router, config, kBodyInfoBytes, 21);
  CallGuest(ctx, base, kRelease, {shape});
  return constructed ? body : 0;
}

bool ReleaseUnworldedCapsuleBody(PPCContext& ctx, uint8_t* base, uint32_t& body) {
  if (!UnworldedBody(base, body) || simpsons::LoadGuestU16(base, body + 6) != 1)
    return false;
  if (!CallGuest(ctx, base, kRelease, {body}))
    return false;
  body = 0;
  return true;
}

uint32_t CreateUnworldedRagdollJoint(PPCContext& ctx, uint8_t* base, uint32_t body_a,
                                     uint32_t body_b, const entity_transform::Transform& frame_a,
                                     const entity_transform::Transform& frame_b,
                                     const JointLimits* limits) {
  if (body_a == body_b || !UnworldedBody(base, body_a) || !UnworldedBody(base, body_b) ||
      (limits && !ValidJointLimits(*limits)))
    return 0;
  const auto router = MemoryRouter(ctx, base);
  if (!router)
    return 0;
  const auto data = Allocate(ctx, base, router, kJointBytes, 42);
  if (!data)
    return 0;
  if (!InitializeJoint(ctx, base, data, frame_a, frame_b, limits)) {
    Free(ctx, base, router, data, kJointBytes, 42);
    return 0;
  }
  AllocationSize(base, data, kJointBytes);
  const auto instance = Allocate(ctx, base, router, kInstanceBytes, 42);
  bool constructed = false;
  if (instance) {
    std::memset(simpsons::GuestToHost(base, instance), 0, kInstanceBytes);
    AllocationSize(base, instance, kInstanceBytes);
    constructed = CallGuest(ctx, base, kInstanceConstruct, {instance, body_a, body_b, data, 1});
    if (!constructed)
      Free(ctx, base, router, instance, kInstanceBytes, 42);
  }
  CallGuest(ctx, base, kRelease, {data});
  return constructed ? instance : 0;
}

bool ReleaseUnworldedRagdollJoint(PPCContext& ctx, uint8_t* base, uint32_t& joint) {
  if (!base || !joint || (joint & 15) || !spawner::Readable(joint, kInstanceBytes) ||
      simpsons::LoadGuestU32(base, joint) != kInstanceTable ||
      simpsons::LoadGuestU16(base, joint + 4) != kInstanceBytes ||
      simpsons::LoadGuestU16(base, joint + 6) != 1 || simpsons::LoadGuestU32(base, joint + 8))
    return false;
  if (!CallGuest(ctx, base, kRelease, {joint}))
    return false;
  joint = 0;
  return true;
}

}  // namespace native_ragdoll
