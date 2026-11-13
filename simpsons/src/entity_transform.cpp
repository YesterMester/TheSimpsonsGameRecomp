#include "entity_transform.h"

#include "game_thread.h"
#include "guest_memory.h"
#include "spawner.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include <rex/ppc/context.h>

namespace entity_transform {
namespace {
constexpr uint32_t kFrame = 140, kParent = 4, kRoot = 160;
constexpr uint32_t kGetWorldMatrix = 0x823F2540, kSetPositionSlot = 28, kSetMatrixSlot = 40;

bool Finite(const Vec3& value) {
  return std::all_of(value.begin(), value.end(), [](float axis) { return std::isfinite(axis); });
}

// The original getter follows parents recursively and reads the root's dirty
// flag. Validate that bounded chain before giving it control, including cycles
// and inconsistent roots. Game matrices themselves stay owned by RenderWare.
bool FrameReadable(const uint8_t* base, uint32_t frame) {
  std::array<uint32_t, 128> visited{};
  uint32_t expected_root = 0;
  for (size_t depth = 0; depth < visited.size(); ++depth) {
    if (!frame || (frame & 3) || !spawner::Readable(frame, kRoot + 4) ||
        std::find(visited.begin(), visited.begin() + depth, frame) != visited.begin() + depth)
      return false;
    visited[depth] = frame;
    const auto root = simpsons::LoadGuestU32(base, frame + kRoot);
    if (!depth) expected_root = root;
    if (root != expected_root) return false;
    if (root == frame) return true;
    frame = simpsons::LoadGuestU32(base, frame + kParent);
  }
  return false;
}

std::optional<Transform> FrameTransform(PPCContext& ctx, uint8_t* base, uint32_t frame) {
  if (!FrameReadable(base, frame)) return {};
  uint32_t matrix = 0;
  if (!CallGuest(ctx, base, kGetWorldMatrix, {frame}, &matrix) ||
      !spawner::Readable(matrix, 64)) return {};
  const auto vector = [&](uint32_t offset) {
    Vec3 result;
    for (size_t i = 0; i < result.size(); ++i)
      result[i] = simpsons::LoadGuestFloat(base, matrix + offset + uint32_t(i) * 4);
    return result;
  };
  Transform result{vector(0), vector(16), vector(32), vector(48)};
  if (!Finite(result.right) || !Finite(result.up) || !Finite(result.forward) ||
      !Finite(result.position)) return {};
  return result;
}

// RenderWare composes local * parent using row-vector affine matrices. Invert
// just the parent's 3x3 basis, in double precision, to accept world coordinates
// under rotated, nonuniformly scaled and sheared parents. Reject singular or
// nearly dependent axes before changing anything in the entity.
std::optional<Vec3> ToLocal(const Transform& parent, const Vec3& world, bool point) {
  using D3 = std::array<double, 3>;
  const auto cross = [](const Vec3& a, const Vec3& b) -> D3 {
    return {double(a[1]) * b[2] - double(a[2]) * b[1],
            double(a[2]) * b[0] - double(a[0]) * b[2],
            double(a[0]) * b[1] - double(a[1]) * b[0]};
  };
  const auto dot = [](const auto& a, const auto& b) {
    return double(a[0]) * b[0] + double(a[1]) * b[1] + double(a[2]) * b[2];
  };
  const D3 cofactors[] = {cross(parent.up, parent.forward),
                          cross(parent.forward, parent.right), cross(parent.right, parent.up)};
  const double determinant = dot(parent.right, cofactors[0]);
  const double volume = std::sqrt(dot(parent.right, parent.right)) *
      std::sqrt(dot(parent.up, parent.up)) * std::sqrt(dot(parent.forward, parent.forward));
  if (!std::isfinite(determinant) || !std::isfinite(volume) || !volume ||
      std::abs(determinant) <= volume * 1e-8) return {};
  const D3 delta{double(world[0]) - (point ? parent.position[0] : 0),
                 double(world[1]) - (point ? parent.position[1] : 0),
                 double(world[2]) - (point ? parent.position[2] : 0)};
  Vec3 local;
  for (size_t i = 0; i < local.size(); ++i) local[i] = float(dot(delta, cofactors[i]) / determinant);
  return Finite(local) ? std::optional<Vec3>(local) : std::nullopt;
}
}  // namespace

bool Finite(const Transform& transform) {
  return Finite(transform.right) && Finite(transform.up) &&
      Finite(transform.forward) && Finite(transform.position);
}

bool Valid(const Transform& transform) {
  if (!Finite(transform.position)) return false;
  double lengths[3];
  const Vec3* axes[] = {&transform.right, &transform.up, &transform.forward};
  for (size_t i = 0; i < 3; ++i) {
    if (!Finite(*axes[i])) return false;
    double squared = 0;
    for (const float v : *axes[i]) squared += double(v) * v;
    // Leave rounding margin for the native multiply/add length calculation.
    if (squared < double(std::numeric_limits<float>::min()) * 4 ||
        squared > double(std::numeric_limits<float>::max()) / 4) return false;
    lengths[i] = std::sqrt(squared);
  }
  const auto& r = transform.right;
  const auto& u = transform.up;
  const auto& f = transform.forward;
  const double determinant = double(r[0]) * (double(u[1]) * f[2] - double(u[2]) * f[1]) -
      double(r[1]) * (double(u[0]) * f[2] - double(u[2]) * f[0]) +
      double(r[2]) * (double(u[0]) * f[1] - double(u[1]) * f[0]);
  return std::abs(determinant) > lengths[0] * lengths[1] * lengths[2] * 1e-8;
}

std::optional<Transform> Compose(const Transform& parent, const Transform& local) {
  if (!Valid(parent) || !Finite(local)) return {};
  const auto convert = [&](const Vec3& value, bool point) {
    Vec3 result;
    for (size_t axis = 0; axis < 3; ++axis)
      result[axis] = float(double(parent.right[axis]) * value[0] +
          double(parent.up[axis]) * value[1] + double(parent.forward[axis]) * value[2] +
          (point ? double(parent.position[axis]) : 0.0));
    return result;
  };
  Transform result{convert(local.right, false), convert(local.up, false),
                   convert(local.forward, false), convert(local.position, true)};
  return Finite(result) ? std::optional<Transform>(result) : std::nullopt;
}

std::optional<Transform> Relative(const Transform& parent, const Transform& world) {
  if (!Valid(parent) || !Finite(world)) return {};
  const auto r = ToLocal(parent, world.right, false);
  const auto u = ToLocal(parent, world.up, false);
  const auto f = ToLocal(parent, world.forward, false);
  const auto p = ToLocal(parent, world.position, true);
  if (!r || !u || !f || !p) return {};
  Transform result{*r, *u, *f, *p};
  return Finite(result) ? std::optional<Transform>(result) : std::nullopt;
}

std::optional<Transform> Get(PPCContext& ctx, uint8_t* base, uint32_t entity) {
  if (!entity || (entity & 3) || !spawner::Readable(entity, kFrame + 4)) return {};
  return FrameTransform(ctx, base, simpsons::LoadGuestU32(base, entity + kFrame));
}

bool Move(PPCContext& ctx, uint8_t* base, uint32_t entity, const Vec3& position) {
  if (!Finite(position) || !entity || (entity & 3) ||
      !spawner::Readable(entity, kFrame + 4)) return false;
  const auto frame = simpsons::LoadGuestU32(base, entity + kFrame);
  const auto table = simpsons::LoadGuestU32(base, entity);
  if (!FrameReadable(base, frame) || !table || (table & 3) ||
      !spawner::Readable(table, 96)) return false;
  Vec3 local = position;
  if (simpsons::LoadGuestU32(base, frame + kRoot) != frame) {
    const auto parent = FrameTransform(ctx, base, simpsons::LoadGuestU32(base, frame + kParent));
    const auto converted = parent ? ToLocal(*parent, position, true) : std::nullopt;
    if (!converted) return false;
    local = *converted;
  }
  PPCContext scratch = ctx;
  scratch.r1.u64 -= 0x80;
  const auto vector = scratch.r1.u32 + 0x60;
  for (size_t i = 0; i < local.size(); ++i)
    simpsons::StoreGuestFloat(base, vector + uint32_t(i) * 4, local[i]);
  return CallGuest(scratch, base, simpsons::LoadGuestU32(base, table + kSetPositionSlot),
                   {entity, vector});
}

bool Set(PPCContext& ctx, uint8_t* base, uint32_t entity, const Transform& transform) {
  if (!Valid(transform) || !entity || (entity & 3) ||
      !spawner::Readable(entity, 204)) return false;
  const auto frame = simpsons::LoadGuestU32(base, entity + kFrame);
  const auto table = simpsons::LoadGuestU32(base, entity);
  if (!FrameReadable(base, frame) || !table || (table & 3) ||
      !spawner::Readable(table, 96)) return false;
  const auto setter = simpsons::LoadGuestU32(base, table + kSetMatrixSlot);
  if (!setter || ctx.r1.u32 < 0x500 || (ctx.r1.u32 & 15) ||
      !spawner::Readable(ctx.r1.u32 - 0x500, 0x500)) return false;
  PPCContext scratch = ctx;
  scratch.r1.u64 -= 0x100;
  const auto matrix = scratch.r1.u32 + 0x80;
  if (!spawner::Readable(matrix, 64)) return false;
  Transform local = transform;
  if (simpsons::LoadGuestU32(base, frame + kRoot) != frame) {
    const auto parent = FrameTransform(ctx, base, simpsons::LoadGuestU32(base, frame + kParent));
    if (!parent) return false;
    const auto r = ToLocal(*parent, transform.right, false);
    const auto u = ToLocal(*parent, transform.up, false);
    const auto f = ToLocal(*parent, transform.forward, false);
    const auto p = ToLocal(*parent, transform.position, true);
    if (!r || !u || !f || !p) return false;
    local = {*r, *u, *f, *p};
    if (!Valid(local)) return false;
  }
  const Vec3* vectors[] = {&local.right, &local.up, &local.forward, &local.position};
  for (size_t row = 0; row < 4; ++row) {
    for (size_t axis = 0; axis < 3; ++axis)
      simpsons::StoreGuestFloat(base, matrix + uint32_t(row * 16 + axis * 4), (*vectors[row])[axis]);
    // General affine basis: never claim orthonormal/identity optimization flags.
    simpsons::StoreGuestFloat(base, matrix + uint32_t(row * 16 + 12), row == 3 ? 1.0f : 0.0f);
  }
  return CallGuest(scratch, base, setter, {entity, matrix});
}

}  // namespace entity_transform
