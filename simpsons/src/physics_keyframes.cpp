// Keep animated collision bodies with their models at high frame rates.
// The original hard-keyframe helper converts a pose error to velocity with
// the render frame's inverse duration. Havok may step less often, so bound
// that correction by the simulation step while retaining the original pose,
// centre of mass, rotation and activation handling.

#include "guest_memory.h"

#include <cmath>
#include <cstdint>

#include <rex/ppc.h>

float HavokKeyframeStep(uint32_t world);

REX_EXTERN(sub_82A93FE0);
REX_EXTERN(__imp__sub_82A93FE0);

REX_FUNC(sub_82A93FE0) {
  const double inverse = ctx.f1.f64;
  const float step = HavokKeyframeStep(simpsons::LoadGuestU32(base, ctx.r6.u32 + 8));
  if (inverse > 0.0 && std::isfinite(inverse) && step > 0.0f && std::isfinite(step)) {
    const double maximum = 1.0 / double(step);
    if (inverse > maximum) {
      ctx.f1.f64 = maximum;
    }
  }
  __imp__sub_82A93FE0(ctx, base);
}
