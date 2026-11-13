#include "native_ragdoll_joint.h"

#include "game_thread.h"
#include "guest_memory.h"
#include "spawner.h"

#include <array>
#include <cmath>
#include <cstring>
#include <utility>

#include <rex/ppc/context.h>

namespace native_ragdoll {
namespace {
constexpr uint32_t kConstruct = 0x82B20270, kValidate = 0x82B1FF18;
constexpr uint32_t kJointTable = 0x821A07D4;

bool ScratchAvailable(const PPCContext& ctx, uint32_t storage) {
  if (ctx.r1.u32 < 0x1000 || !spawner::Readable(ctx.r1.u32 - 0x400, 0x440)) return false;
  return uint64_t(storage) + kJointBytes <= ctx.r1.u32 - 0x400 ||
         uint64_t(storage) >= uint64_t(ctx.r1.u32) + 0x40;
}

bool Rigid(const entity_transform::Transform& frame) {
  const auto dot = [](const auto& a, const auto& b) {
    return double(a[0]) * b[0] + double(a[1]) * b[1] + double(a[2]) * b[2];
  };
  for (const auto& column : {frame.right, frame.up, frame.forward, frame.position})
    for (const auto value : column)
      if (!std::isfinite(value)) return false;
  if (std::abs(dot(frame.right, frame.right) - 1) > 1e-4 ||
      std::abs(dot(frame.up, frame.up) - 1) > 1e-4 ||
      std::abs(dot(frame.forward, frame.forward) - 1) > 1e-4 ||
      std::abs(dot(frame.right, frame.up)) > 1e-4 ||
      std::abs(dot(frame.right, frame.forward)) > 1e-4 ||
      std::abs(dot(frame.up, frame.forward)) > 1e-4) return false;
  const std::array<double, 3> cross{
      double(frame.right[1]) * frame.up[2] - double(frame.right[2]) * frame.up[1],
      double(frame.right[2]) * frame.up[0] - double(frame.right[0]) * frame.up[2],
      double(frame.right[0]) * frame.up[1] - double(frame.right[1]) * frame.up[0]};
  return dot(cross, frame.forward) > 0.9999;
}

void StoreFrame(uint8_t* base, uint32_t at, const entity_transform::Transform& frame) {
  for (const auto& column : {frame.right, frame.up, frame.forward, frame.position}) {
    for (uint32_t i = 0; i < 3; ++i) simpsons::StoreGuestFloat(base, at + i * 4, column[i]);
    simpsons::StoreGuestU32(base, at + 12, 0);
    at += 16;
  }
}

entity_transform::Transform ReadFrame(const uint8_t* base, uint32_t at) {
  entity_transform::Transform frame;
  for (auto* column : {&frame.right, &frame.up, &frame.forward, &frame.position}) {
    for (uint32_t i = 0; i < 3; ++i) (*column)[i] = simpsons::LoadGuestFloat(base, at + i * 4);
    at += 16;
  }
  return frame;
}
}  // namespace

bool ValidJointLimits(const JointLimits& limits) {
  constexpr float pi = 3.14159265358979323846f;
  for (const auto value : {limits.twist_min, limits.twist_max, limits.cone_max,
                          limits.plane_min, limits.plane_max, limits.tau})
    if (!std::isfinite(value)) return false;
  return limits.twist_min >= -pi && limits.twist_max <= pi &&
      limits.twist_min <= limits.twist_max && limits.cone_max >= 0 && limits.cone_max < pi &&
      limits.plane_min >= -pi * .5f && limits.plane_max <= pi * .5f &&
      limits.plane_min <= limits.plane_max && limits.tau > 0 && limits.tau <= 1;
}

bool JointValid(PPCContext& ctx, uint8_t* base, uint32_t storage) {
  if (!storage || (storage & 15) || !spawner::Readable(storage, kJointBytes) ||
      simpsons::LoadGuestU32(base, storage) != kJointTable || !ScratchAvailable(ctx, storage))
    return false;
  if (!Rigid(ReadFrame(base, storage + 32)) || !Rigid(ReadFrame(base, storage + 96))) return false;
  for (const auto offset : {260u, 264u, 268u, 280u, 284u, 288u, 300u, 304u, 308u})
    if (!std::isfinite(simpsons::LoadGuestFloat(base, storage + offset))) return false;
  PPCContext scratch = ctx;
  scratch.r1.u64 -= 0x100;
  const uint32_t result = scratch.r1.u32 + 0x50;
  if (!spawner::Readable(result, 4) ||
      (uint64_t(result) < uint64_t(storage) + kJointBytes && uint64_t(result) + 4 > storage))
    return false;
  simpsons::GuestToHost(base, result)[0] = 0;
  // hkBool is returned through a hidden output pointer (r3); this is r4.
  // Passing the joint as r3 would overwrite its vtable with the boolean.
  return CallGuest(scratch, base, kValidate, {result, storage}) &&
         simpsons::GuestToHost(base, result)[0] == 1;
}

bool InitializeJoint(PPCContext& ctx, uint8_t* base, uint32_t storage,
                     const entity_transform::Transform& body_a,
                     const entity_transform::Transform& body_b, const JointLimits* limits) {
  if (!storage || (storage & 15) || !spawner::Readable(storage, kJointBytes) ||
      !ScratchAvailable(ctx, storage) || !Rigid(body_a) || !Rigid(body_b) ||
      (limits && !ValidJointLimits(*limits))) return false;
  // This is a definition, not a published physics object. Roll back if the
  // original constructor/validator cannot complete; no world owns it yet.
  std::array<uint8_t, kJointBytes> previous;
  std::memcpy(previous.data(), simpsons::GuestToHost(base, storage), kJointBytes);
  try {
    std::memset(simpsons::GuestToHost(base, storage), 0, kJointBytes);
    if (CallGuest(ctx, base, kConstruct, {storage})) {
      StoreFrame(base, storage + 32, body_a);
      StoreFrame(base, storage + 96, body_b);
      if (limits) {
        // 82B20270 initializes these native atoms; 82B1FF18 validates their
        // limits. 82B1F9C0 independently confirms cone_max at +284. Preserve
        // the native cone-min sentinel (+280), axis selectors and motor data.
        // Only fresh, unpublished definitions are edited, never a live joint.
        for (const auto [offset, value] : std::array<std::pair<uint32_t, float>, 8>{{
                 {260, limits->twist_min}, {264, limits->twist_max}, {268, limits->tau},
                 {284, limits->cone_max}, {288, limits->tau}, {300, limits->plane_min},
                 {304, limits->plane_max}, {308, limits->tau}}})
          simpsons::StoreGuestFloat(base, storage + offset, value);
      }
      if (JointValid(ctx, base, storage)) return true;
    }
  } catch (...) {
    std::memcpy(simpsons::GuestToHost(base, storage), previous.data(), kJointBytes);
    throw;
  }
  std::memcpy(simpsons::GuestToHost(base, storage), previous.data(), kJointBytes);
  return false;
}

}  // namespace native_ragdoll
