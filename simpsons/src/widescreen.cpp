// Widescreen camera framing.
//
// The camera's view window is calculated by sub_82714588 and then passed to
// RenderWare's CameraSetViewWindow (sub_823F1C98). Changing the window here
// updates the rendered projection and RenderWare's culling together. The
// game's other caller, sub_82961FC0, uses the same calculation when fitting
// a camera to a bounding box.
//
// This is only the camera part of widescreen support. The front end still
// opens a 1280x720 RenderWare mode, and changing that mode also requires the
// render targets, post effects, HUD and movies to agree on their dimensions.
// Keep this development option off until that work is qualified. Stretching
// presentation does not change the camera raster and does not enable it.

#include "guest_memory.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include <rex/cvar.h>
#include <rex/ppc.h>

REXCVAR_DEFINE_BOOL(widescreen_camera, false, "GPU",
                    "Experimental camera framing for a changed gameplay raster aspect; "
                    "does not resize render targets, the HUD or movies")
    .debug_only();

REX_EXTERN(__imp__sub_82714588);
REX_EXTERN(sub_82714588);

namespace {

constexpr uint32_t kCameraManager = 0x82D08B10;
constexpr uint32_t kManagerCamera0 = 20;
constexpr uint32_t kManagerViewportCount = 300;
constexpr uint32_t kCameraRaster = 96;
constexpr uint32_t kRasterWidth = 12;
constexpr uint32_t kRasterHeight = 16;

// The factors used by the shipped projection helper for the widescreen and
// 4:3 modes. Read the image's values rather than rounding their reciprocals.
constexpr uint32_t kWideVerticalFactor = 0x8214E7B8;
constexpr uint32_t kStandardVerticalFactor = 0x821DD368;

bool IsSingleGameplayCamera(const uint8_t* base, uint32_t camera) {
  const uint32_t manager = simpsons::LoadGuestU32(base, kCameraManager);
  return manager && simpsons::LoadGuestU32(base, manager + kManagerViewportCount) == 1 &&
         simpsons::LoadGuestU32(base, manager + kManagerCamera0) == camera;
}

}  // namespace

REX_FUNC(sub_82714588) {
  // r4 = RwCamera, r5 = widescreen flag, r6 = vertical-FOV flag,
  // r7 = output RwV2d, f1 = tan(field of view / 2). The original function
  // truncates the flags to their low byte.
  const uint32_t camera = ctx.r4.u32;
  const uint32_t output = ctx.r7.u32;
  const bool wide = (ctx.r5.u32 & 0xFF) != 0;
  const bool vertical_fov = (ctx.r6.u32 & 0xFF) != 0;
  const bool enabled = REXCVAR_GET(widescreen_camera);

  __imp__sub_82714588(ctx, base);

  if (!enabled || !wide || !IsSingleGameplayCamera(base, camera)) {
    return;
  }
  const uint32_t raster = simpsons::LoadGuestU32(base, camera + kCameraRaster);
  if (!raster) {
    return;
  }
  const int32_t width = int32_t(simpsons::LoadGuestU32(base, raster + kRasterWidth));
  const int32_t height = int32_t(simpsons::LoadGuestU32(base, raster + kRasterHeight));
  const float factor = simpsons::LoadGuestFloat(
      base, wide ? kWideVerticalFactor : kStandardVerticalFactor);
  const float original_x = simpsons::LoadGuestFloat(base, output);
  const float original_y = simpsons::LoadGuestFloat(base, output + 4);
  if (width <= 0 || height <= 0 || !std::isfinite(factor) || factor <= 0.0f ||
      !std::isfinite(original_x) || !std::isfinite(original_y) || original_x <= 0.0f ||
      original_y <= 0.0f) {
    return;
  }

  // The shipped helper already accounts for interlaced raster heights.
  // Its output ratio is therefore the effective raster aspect, even when
  // the raw height is the height of one field.
  const float aspect = original_x / original_y;
  const float reference_aspect = 1.0f / factor;
  if (aspect < 0.75f || aspect > 4.0f ||
      std::abs(aspect - 16.0f / 9.0f) < 0.00001f ||
      std::abs(aspect - 4.0f / 3.0f) < 0.00001f ||
      std::abs(aspect - reference_aspect) < 0.00001f) {
    // In particular, the stock 16:9 and 4:3 paths remain byte-for-byte the
    // game's own calculation, regardless of the director's FOV flags.
    // Split-screen and auxiliary cameras also keep their original framing.
    return;
  }
  if ((vertical_fov && aspect >= reference_aspect) ||
      (!vertical_fov && aspect <= reference_aspect)) {
    // These conventions already expose more of the scene on the changed
    // axis. Preserve the game's exact result, including its float rounding.
    return;
  }

  // Recover the shipped mode's horizontal view from the original helper,
  // regardless of which FOV convention the current director requested.
  const float reference_x = vertical_fov ? original_y / factor : original_x;
  const float reference_y = reference_x * factor;
  // Wider screens show more horizontally; taller screens show more
  // vertically. Neither axis loses any of the original framing.
  const float x = std::max(reference_x, reference_y * aspect);
  const float y = std::max(reference_y, reference_x / aspect);
  if (!std::isfinite(x) || !std::isfinite(y)) {
    return;
  }
  simpsons::StoreGuestFloat(base, output, x);
  simpsons::StoreGuestFloat(base, output + 4, y);
}
