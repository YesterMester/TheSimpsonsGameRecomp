#pragma once

#include <array>
#include <cstdint>
#include <optional>

struct PPCContext;

namespace entity_transform {

using Vec3 = std::array<float, 3>;
// World-space RenderWare basis vectors include scale. They are not Euler
// angles or a promise that the frame has an orthonormal rotation.
struct Transform {
  Vec3 right, up, forward, position;
};

// Authoring requires a nonsingular basis whose vector lengths can be computed
// by the original float32 routines without overflow or a flushed zero scale.
bool Valid(const Transform& transform);
// Animation may intentionally collapse a helper/facial bone to hide geometry.
// Such an affine matrix is finite even though it cannot be inverted.
bool Finite(const Transform& transform);

// Checked affine composition and conversion back to a parent frame. These
// preserve nonuniform scale and shear; neither operation assumes a rotation.
// No game memory is read or changed. The parent must be nonsingular; a local
// bone/world result may have zero scale. Nonfinite/overflowing results fail.
std::optional<Transform> Compose(const Transform& parent, const Transform& local);
std::optional<Transform> Relative(const Transform& parent, const Transform& world);

// Game thread only. The original frame getter resolves pending parent/frame
// updates before reading; an invalid or unsupported frame returns no result.
std::optional<Transform> Get(PPCContext& ctx, uint8_t* base, uint32_t entity);
// World position, preserving the existing basis. Dispatches the class's own
// setter (including controller/physics overrides), never scans object memory.
bool Move(PPCContext& ctx, uint8_t* base, uint32_t entity, const Vec3& position);
// World position, orientation and scale through the class's full matrix setter.
// Converts all four vectors through a parent before dispatching; cached scale,
// model notifications and controller overrides stay with the original class.
bool Set(PPCContext& ctx, uint8_t* base, uint32_t entity, const Transform& transform);

}  // namespace entity_transform
