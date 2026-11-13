#include "native_ragdoll_rig.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <set>

namespace native_ragdoll {
namespace {
using entity_transform::Transform;
using entity_transform::Vec3;

constexpr JointLimits Limits(float twist, float cone, float plane) {
  constexpr float radians = 3.14159265358979323846f / 180;
  return {-twist * radians, twist * radians, cone * radians,
          -plane * radians, plane * radians, .8f};
}

// The native hierarchy uses case-sensitive 65599 hashes of these exact names.
constexpr uint32_t kPelvis = 0x4387877B, kChest = 0x6CB25065, kNeck = 0x26ABEA80;
constexpr uint32_t kLeftEye = 0x1DF4C75E, kRightEye = 0xB57F0164;
constexpr std::array<uint32_t, 4> kLeftLeg{0x1F5EF1FC, 0x38594A76, 0x842EFC24, 0x8F3C2BF2};
constexpr std::array<uint32_t, 4> kRightLeg{0xB6E92C02, 0xBD6391F0, 0x8930932A, 0x1446736C};
constexpr std::array<uint32_t, 3> kLeftArm{0xE4540033, 0x32674BF0, 0x13A54BFC};
constexpr std::array<uint32_t, 3> kRightArm{0x0C5C8DAD, 0x3768E2F6, 0x18A6E302};

double Dot(const Vec3& a, const Vec3& b) {
  return double(a[0]) * b[0] + double(a[1]) * b[1] + double(a[2]) * b[2];
}
Vec3 Cross(const Vec3& a, const Vec3& b) {
  return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2],
          a[0] * b[1] - a[1] * b[0]};
}
Vec3 Difference(const Vec3& a, const Vec3& b) {
  return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}
Vec3 Normalize(const Vec3& value) {
  const auto length = std::sqrt(Dot(value, value));
  if (!std::isfinite(length) || length < 1e-8) return {};
  return {float(value[0] / length), float(value[1] / length), float(value[2] / length)};
}

std::optional<Transform> LimbFrame(const Vec3& begin, const Vec3& end, const Vec3& facing) {
  const auto up = Normalize(Difference(end, begin));
  if (Dot(up, up) < .99) return {};
  auto right = Normalize(Cross(up, Normalize(facing)));
  if (Dot(right, right) < .99) {
    // A limb can point along the character's facing direction in an animation.
    // Pick the least parallel world axis rather than collapsing its rotation.
    size_t least = 0;
    for (size_t i = 1; i < 3; ++i)
      if (std::abs(up[i]) < std::abs(up[least])) least = i;
    Vec3 reference{};
    reference[least] = 1;
    right = Normalize(Cross(up, reference));
  }
  Vec3 center;
  for (size_t i = 0; i < 3; ++i) center[i] = float((double(begin[i]) + end[i]) * .5);
  Transform result{right, up, Normalize(Cross(right, up)), center};
  return entity_transform::Valid(result) ? std::optional<Transform>(result) : std::nullopt;
}

std::array<float, 4> Quaternion(const Transform& frame) {
  // Columns are the images of the body's local X/Y/Z axes in world space.
  const double matrix[3][3] = {
      {frame.right[0], frame.up[0], frame.forward[0]},
      {frame.right[1], frame.up[1], frame.forward[1]},
      {frame.right[2], frame.up[2], frame.forward[2]}};
  std::array<double, 4> q{};
  const double trace = matrix[0][0] + matrix[1][1] + matrix[2][2];
  if (trace > 0) {
    const double scale = std::sqrt(trace + 1) * 2;
    q = {(matrix[2][1] - matrix[1][2]) / scale,
         (matrix[0][2] - matrix[2][0]) / scale,
         (matrix[1][0] - matrix[0][1]) / scale, scale * .25};
  } else {
    size_t axis = 0;
    if (matrix[1][1] > matrix[axis][axis]) axis = 1;
    if (matrix[2][2] > matrix[axis][axis]) axis = 2;
    const size_t next = (axis + 1) % 3, last = (axis + 2) % 3;
    const double scale = std::sqrt(1 + matrix[axis][axis] - matrix[next][next] - matrix[last][last]) * 2;
    q[axis] = scale * .25;
    q[next] = (matrix[axis][next] + matrix[next][axis]) / scale;
    q[last] = (matrix[axis][last] + matrix[last][axis]) / scale;
    q[3] = (matrix[last][next] - matrix[next][last]) / scale;
  }
  double length = 0;
  for (const auto value : q) length += value * value;
  const double sign = q[3] < 0 ? -1 : 1;
  std::array<float, 4> result;
  for (size_t i = 0; i < 4; ++i) result[i] = float(sign * q[i] / std::sqrt(length));
  return result;
}

bool Rigid(const Transform& frame) {
  if (!entity_transform::Valid(frame)) return false;
  return std::abs(Dot(frame.right, frame.right) - 1) < 1e-4 &&
      std::abs(Dot(frame.up, frame.up) - 1) < 1e-4 &&
      std::abs(Dot(frame.forward, frame.forward) - 1) < 1e-4 &&
      std::abs(Dot(frame.right, frame.up)) < 1e-4 &&
      std::abs(Dot(frame.right, frame.forward)) < 1e-4 &&
      std::abs(Dot(frame.up, frame.forward)) < 1e-4 &&
      Dot(Cross(frame.right, frame.up), frame.forward) > .9999;
}
}  // namespace

std::optional<BipedRig> BuildBipedRig(const SkeletonPose& pose, float total_mass,
                                     const Vec3& velocity) {
  if (!pose.actor || pose.bones.empty() || pose.bones.size() > 256 ||
      pose.model_nodes < pose.bones.size() || pose.model_nodes > 256 ||
      !entity_transform::Valid(pose.world_transform) || !std::isfinite(total_mass) ||
      total_mass < 1 || total_mass > 1000)
    return {};
  for (const auto value : velocity)
    if (!std::isfinite(value) || std::abs(value) > 1000) return {};
  std::map<uint32_t, size_t> names;
  std::set<uint32_t> model_indices;
  std::vector<Transform> bone_world;
  for (size_t i = 0; i < pose.bones.size(); ++i) {
    const auto& bone = pose.bones[i];
    if ((i ? bone.parent < 0 || size_t(bone.parent) >= i : bone.parent != -1) ||
        bone.model_index >= 256 || !names.emplace(bone.id, i).second ||
        !model_indices.insert(bone.model_index).second)
      return {};
    const auto world = entity_transform::Compose(pose.world_transform, bone.model_transform);
    if (!world) return {};
    bone_world.push_back(*world);
  }
  const auto exists = [&](uint32_t id) { return names.contains(id); };
  for (const auto id : {kPelvis, kChest, kNeck, kLeftEye, kRightEye})
    if (!exists(id)) return {};
  for (const auto& chain : {std::span<const uint32_t>(kLeftLeg), std::span<const uint32_t>(kRightLeg),
                             std::span<const uint32_t>(kLeftArm), std::span<const uint32_t>(kRightArm)})
    for (const auto id : chain)
      if (!exists(id)) return {};
  const auto descendant = [&](uint32_t child, uint32_t ancestor) {
    int32_t current = int32_t(names.at(child));
    while (current >= 0) {
      if (size_t(current) == names.at(ancestor)) return true;
      current = pose.bones[size_t(current)].parent;
    }
    return false;
  };
  if (!descendant(kChest, kPelvis) || !descendant(kNeck, kChest) ||
      !descendant(kLeftEye, kNeck) || !descendant(kRightEye, kNeck)) return {};
  for (const auto& chain : {std::span<const uint32_t>(kLeftLeg), std::span<const uint32_t>(kRightLeg),
                             std::span<const uint32_t>(kLeftArm), std::span<const uint32_t>(kRightArm)}) {
    if (!descendant(chain[0], chain.size() == 4 ? kPelvis : kChest)) return {};
    for (size_t i = 1; i < chain.size(); ++i)
      if (!descendant(chain[i], chain[i - 1])) return {};
  }
  const auto position = [&](uint32_t id) { return bone_world[names.at(id)].position; };
  const auto distance = [&](uint32_t a, uint32_t b) {
    const auto delta = Difference(position(a), position(b));
    return std::sqrt(Dot(delta, delta));
  };
  const auto neck = position(kNeck), left_eye = position(kLeftEye), right_eye = position(kRightEye);
  Vec3 head;
  for (size_t i = 0; i < 3; ++i)
    head[i] = float(double(neck[i]) + ((double(left_eye[i]) + right_eye[i]) * .5 - neck[i]) * 1.5);
  const auto head_axis = Difference(head, neck);
  const double height = distance(kLeftLeg[0], kLeftLeg[1]) + distance(kLeftLeg[1], kLeftLeg[2]) +
      distance(kPelvis, kChest) + distance(kChest, kNeck) + std::sqrt(Dot(head_axis, head_axis));
  if (!std::isfinite(height) || height < .2 || height > 10) return {};

  BipedRig rig;
  rig.source = pose;
  std::vector<Transform> body_world;
  std::map<uint32_t, uint32_t> anchors;
  bool valid = true;
  const auto add = [&](uint32_t begin_id, const Vec3& end, float radius_ratio,
                        float mass_ratio, uint32_t parent, const JointLimits& limits = {}) {
    if (!valid) return;
    const auto begin = position(begin_id);
    const auto delta = Difference(end, begin);
    const double length = std::sqrt(Dot(delta, delta));
    const auto frame = LimbFrame(begin, end, pose.world_transform.forward);
    if (!frame || !Rigid(*frame) || !std::isfinite(length) || length < height * .005) {
      valid = false;
      return;
    }
    const auto index = uint32_t(rig.bodies.size());
    CapsuleBodyInfo body;
    body.radius = float(height * radius_ratio);
    body.half_length = float(std::max(length * .5 - body.radius, height * .005));
    body.mass = total_mass * mass_ratio;
    body.position = frame->position;
    body.rotation = Quaternion(*frame);
    body.linear_velocity = velocity;
    rig.bodies.push_back(body);
    body_world.push_back(*frame);
    anchors[begin_id] = index;
    if (parent == UINT32_MAX) return;
    // Native ragdoll limits use the joint's X axis as the twist direction.
    Transform joint_world{frame->up, frame->forward, frame->right, begin};
    const auto a = entity_transform::Relative(body_world[parent], joint_world);
    const auto b = entity_transform::Relative(*frame, joint_world);
    if (!a || !b || !Rigid(*a) || !Rigid(*b)) { valid = false; return; }
    rig.joints.push_back({parent, index, *a, *b, limits});
  };
  // These passive profiles are relative to the entry pose. Long limb axes
  // retain the native X twist convention; narrow plane/twist limits keep
  // elbows and knees from flopping sideways. This is not yet a bind-pose
  // calibrated, one-way knee/elbow or a powered get-up controller.
  add(kPelvis, position(kChest), .10f, .16f, UINT32_MAX);
  add(kChest, neck, .12f, .28f, 0, Limits(25, 35, 25));
  add(kNeck, head, .10f, .08f, 1, Limits(60, 55, 35));
  for (const auto& chain : {kLeftLeg, kRightLeg}) {
    const auto first = uint32_t(rig.bodies.size());
    add(chain[0], position(chain[1]), .055f, .105f, 0, Limits(35, 100, 45));
    add(chain[1], position(chain[2]), .04f, .055f, first, Limits(5, 135, 5));
    add(chain[2], position(chain[3]), .04f, .015f, first + 1, Limits(15, 40, 25));
    anchors[chain[3]] = first + 2;
  }
  for (const auto& chain : {kLeftArm, kRightArm}) {
    const auto first = uint32_t(rig.bodies.size());
    add(chain[0], position(chain[1]), .04f, .04f, 1, Limits(80, 130, 85));
    add(chain[1], position(chain[2]), .03f, .025f, first, Limits(5, 145, 5));
    anchors[chain[2]] = first + 1;
  }
  if (!valid || rig.bodies.size() != 13 || rig.joints.size() != 12) return {};
  anchors[kLeftEye] = anchors[kRightEye] = 2;
  // neck2 and face/helper tracks inherit their nearest physical ancestor.
  rig.bindings.reserve(pose.bones.size());
  for (size_t i = 0; i < pose.bones.size(); ++i) {
    const auto& bone = pose.bones[i];
    const auto found = anchors.find(bone.id);
    const auto body = found != anchors.end() ? found->second :
        (bone.parent < 0 ? 0 : rig.bindings[size_t(bone.parent)].body);
    const auto offset = entity_transform::Relative(body_world[body], bone_world[i]);
    if (!offset) return {};
    rig.bindings.push_back({body, *offset});
  }
  return rig;
}

std::optional<std::vector<BonePose>> EvaluateBipedRig(
    const BipedRig& rig, std::span<const SimulatedBody> bodies, const Transform& render_root) {
  if (bodies.size() != rig.bodies.size() || bodies.empty() || bodies.size() > 32 ||
      rig.bindings.size() != rig.source.bones.size() || !entity_transform::Valid(render_root))
    return {};
  for (const auto& body : bodies)
    if (!Rigid(body.transform)) return {};
  auto result = rig.source.bones;
  for (size_t i = 0; i < result.size(); ++i) {
    const auto& binding = rig.bindings[i];
    if (binding.body >= bodies.size()) return {};
    const auto world = entity_transform::Compose(bodies[binding.body].transform, binding.body_to_bone);
    const auto local = world ? entity_transform::Relative(render_root, *world) : std::nullopt;
    if (!local) return {};
    result[i].model_transform = *local;
  }
  return result;
}

}  // namespace native_ragdoll
