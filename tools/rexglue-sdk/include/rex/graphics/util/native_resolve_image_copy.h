#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>

namespace rex::graphics::draw_util {

struct NativeResolveImageCopyRegion {
  uint32_t x = 0, y = 0, width = 0, height = 0;
  bool replaces_all = false;
  explicit operator bool() const { return width && height; }
};

// Source and target have the same unorm format and scale. A partial copy may
// keep an existing channel swizzle, but cannot change it for untouched pixels.
inline NativeResolveImageCopyRegion GetNativeResolveImageCopyRegion(
    uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, uint32_t target_width,
    uint32_t target_height, uint32_t source_width, uint32_t source_height, uint32_t scale_x,
    uint32_t scale_y, bool target_swapped, bool copy_swapped) {
  if (x0 >= x1 || y0 >= y1 || target_width <= x0 || target_height <= y0 || !scale_x ||
      !scale_y)
    return {};
  x1 = std::min(x1, target_width);
  y1 = std::min(y1, target_height);
  const bool replaces_all = !x0 && !y0 && x1 == target_width && y1 == target_height;
  if (x1 > source_width || y1 > source_height ||
      (!replaces_all && target_swapped != copy_swapped) ||
      uint64_t(x1) * scale_x > uint32_t(std::numeric_limits<int32_t>::max()) ||
      uint64_t(y1) * scale_y > uint32_t(std::numeric_limits<int32_t>::max()))
    return {};
  return {x0 * scale_x, y0 * scale_y, (x1 - x0) * scale_x, (y1 - y0) * scale_y, replaces_all};
}

}  // namespace rex::graphics::draw_util
