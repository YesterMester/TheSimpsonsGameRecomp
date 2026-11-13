/**
 ******************************************************************************
 * ReXGlue - Xbox 360 Static Recompilation Runtime                            *
 ******************************************************************************
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#pragma once

#include <algorithm>
#include <cstdint>
#include <numeric>

#include <rex/graphics/xenos.h>

namespace rex::graphics::draw_util {

struct NativeResolveSourceLayout {
  uint32_t packed = 0;
  bool valid = false;
  explicit operator bool() const { return valid; }
};

// Image coordinates are relative to the surface's original tile base. A
// resolve may read beyond one EDRAM period, including across its last row.
// Check every folded tile before permitting a native image load there.
inline NativeResolveSourceLayout GetNativeResolveSourceLayout(
    uint32_t x0, uint32_t y0, uint32_t width, uint32_t height, uint32_t scale_x, uint32_t scale_y,
    uint32_t image_width, uint32_t image_height, xenos::MsaaSamples guest_samples,
    xenos::MsaaSamples image_samples, bool source_wide, uint32_t pitch_tiles) {
  NativeResolveSourceLayout result;
  if (!width || !height || !scale_x || !scale_y || pitch_tiles > 65535u ||
      guest_samples > xenos::MsaaSamples::k4X || image_samples > xenos::MsaaSamples::k4X ||
      (image_samples != guest_samples && image_samples != xenos::MsaaSamples::k1X))
    return result;
  bool virtual_samples = image_samples != guest_samples;
  uint32_t grid_x = virtual_samples && guest_samples == xenos::MsaaSamples::k4X ? 2u : 1u;
  uint32_t grid_y = virtual_samples && guest_samples != xenos::MsaaSamples::k1X ? 2u : 1u;
  uint64_t px0 = uint64_t(x0) * grid_x, py0 = uint64_t(y0) * grid_y;
  uint64_t px1 = (uint64_t(x0) + width) * grid_x;
  uint64_t py1 = (uint64_t(y0) + height) * grid_y;
  uint32_t tile_width =
      xenos::kEdramTileWidthSamples >>
      (uint32_t(source_wide) + uint32_t(image_samples == xenos::MsaaSamples::k4X));
  uint32_t tile_height =
      xenos::kEdramTileHeightSamples >> uint32_t(image_samples != xenos::MsaaSamples::k1X);
  uint64_t last_tile = ((py1 - 1u) / tile_height) * pitch_tiles + (px1 - 1u) / tile_width;
  bool wrapped = px1 * scale_x > image_width || py1 * scale_y > image_height ||
                 (pitch_tiles && last_tile >= xenos::kEdramTileCount);
  if (wrapped) {
    if (!pitch_tiles)
      return result;
    uint64_t row_first = py0 / tile_height, column_first = px0 / tile_width;
    uint64_t rows = (py1 - 1u) / tile_height - row_first + 1u;
    uint64_t columns = (px1 - 1u) / tile_width - column_first + 1u;
    // Further periods repeat the same image locations.
    rows = std::min(
        rows, uint64_t(xenos::kEdramTileCount / std::gcd(pitch_tiles, xenos::kEdramTileCount)));
    columns = std::min(columns, uint64_t(xenos::kEdramTileCount));
    for (uint64_t row = 0; row < rows; ++row)
      for (uint64_t column = 0; column < columns; ++column) {
        uint32_t tile = uint32_t(((row_first + row) * pitch_tiles + column_first + column) &
                                 (xenos::kEdramTileCount - 1u));
        if (uint64_t(tile % pitch_tiles + 1u) * tile_width * scale_x > image_width ||
            uint64_t(tile / pitch_tiles + 1u) * tile_height * scale_y > image_height)
          return result;
      }
  }
  result.packed = uint32_t(guest_samples) | (virtual_samples ? 4u : 0u) |
                  (uint32_t(image_samples) << 4) | (uint32_t(source_wide) << 6) |
                  (pitch_tiles << 8) | (wrapped ? 0x80000000u : 0u);
  result.valid = true;
  return result;
}

}  // namespace rex::graphics::draw_util
