#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

namespace rex::graphics {

struct NativeSurfaceCopyRegion {
  uint32_t source_x, source_y;
  uint32_t dest_x, dest_y;
  uint32_t width, height;
};

// A same-format, single-sampled surface can retain its pixels across a pitch
// change with image copies. Split the rectangle where either surface's tile
// row ends; no format conversion or depth rounding is needed.
inline bool BuildNativeSurfaceCopyRegions(uint32_t source_base, uint32_t source_pitch,
                                          uint32_t source_rows, uint32_t dest_base,
                                          uint32_t dest_pitch, uint32_t dest_rows,
                                          uint32_t tile_width, uint32_t x, uint32_t y,
                                          uint32_t width, uint32_t height,
                                          std::vector<NativeSurfaceCopyRegion>& regions) {
  regions.clear();
  constexpr uint32_t kTileCount = 2048;
  constexpr uint32_t kTileHeight = 16;
  if (source_base >= kTileCount || dest_base >= kTileCount || !source_pitch || !dest_pitch ||
      source_pitch > kTileCount || dest_pitch > kTileCount || !source_rows || !dest_rows ||
      source_rows > kTileCount || dest_rows > kTileCount ||
      (tile_width != 40 && tile_width != 80) || !width || !height ||
      uint64_t(x) + width > uint64_t(dest_pitch) * tile_width ||
      uint64_t(y) + height > uint64_t(dest_rows) * kTileHeight) {
    return false;
  }
  uint32_t x_end = x + width;
  uint32_t y_end = y + height;
  for (uint32_t dest_y = y; dest_y < y_end;) {
    uint32_t row_height = std::min(y_end - dest_y, kTileHeight - dest_y % kTileHeight);
    for (uint32_t dest_x = x; dest_x < x_end;) {
      uint32_t tile = uint32_t((uint64_t(dest_base) + uint64_t(dest_y / kTileHeight) * dest_pitch +
                                dest_x / tile_width) &
                               (kTileCount - 1));
      uint32_t source_tile = (tile + kTileCount - source_base) & (kTileCount - 1);
      uint32_t source_row = source_tile / source_pitch;
      if (source_row >= source_rows) {
        regions.clear();
        return false;
      }
      uint32_t source_x = (source_tile % source_pitch) * tile_width + dest_x % tile_width;
      uint32_t source_y = source_row * kTileHeight + dest_y % kTileHeight;
      uint32_t column_width = std::min(x_end - dest_x, tile_width - dest_x % tile_width);
      if (!regions.empty()) {
        auto& previous = regions.back();
        if (previous.source_y == source_y && previous.dest_y == dest_y &&
            previous.height == row_height && previous.source_x + previous.width == source_x &&
            previous.dest_x + previous.width == dest_x) {
          previous.width += column_width;
          dest_x += column_width;
          continue;
        }
      }
      regions.push_back({source_x, source_y, dest_x, dest_y, column_width, row_height});
      dest_x += column_width;
    }
    dest_y += row_height;
  }
  return true;
}

}  // namespace rex::graphics
