#include <rex/graphics/util/native_resolve_image_copy.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>

namespace {
void Require(bool condition) {
  if (!condition)
    std::abort();
}
uint32_t Swap(uint32_t pixel, bool swapped) {
  return swapped ? (pixel & 0xFF00FF00u) | ((pixel & 0xFFu) << 16) |
                       ((pixel >> 16) & 0xFFu)
                 : pixel;
}
uint32_t Pixel(uint32_t x, uint32_t y) {
  return 0xFF000000u | (((x * 7 + y * 13) & 255) << 16) | ((x * 3 + 19) << 8) |
         ((y * 11 + 29) & 255);
}
}  // namespace

int main(int argc, char** argv) {
  Require(argc == 1 || (argc == 3 && std::strcmp(argv[1], "--output") == 0));
  using rex::graphics::draw_util::GetNativeResolveImageCopyRegion;
  uint32_t cases = 0, partial = 0, rejected = 0;
  constexpr uint32_t width = 5, height = 3;
  for (uint32_t scale = 1; scale <= 3; ++scale) {
    for (bool old_swapped : {false, true}) {
      for (bool copy_swapped : {false, true}) {
        for (uint32_t x0 = 0; x0 <= 6; ++x0) {
          for (uint32_t y0 = 0; y0 <= 4; ++y0) {
            for (uint32_t x1 = 0; x1 <= 7; ++x1) {
              for (uint32_t y1 = 0; y1 <= 5; ++y1) {
                uint32_t covered = 0;
                for (uint32_t y = 0; y < height; ++y)
                  for (uint32_t x = 0; x < width; ++x)
                    covered += x >= x0 && x < x1 && y >= y0 && y < y1;
                const bool expected = covered &&
                                      (old_swapped == copy_swapped || covered == width * height);
                const auto region = GetNativeResolveImageCopyRegion(
                    x0, y0, x1, y1, width, height, 8, 6, scale, scale, old_swapped, copy_swapped);
                Require(bool(region) == expected);
                ++cases;
                if (!region) {
                  ++rejected;
                  continue;
                }
                Require(region.replaces_all == (covered == width * height));
                partial += !region.replaces_all;
                for (uint32_t y = 0; y < height * scale; ++y) {
                  for (uint32_t x = 0; x < width * scale; ++x) {
                    const uint32_t before = Pixel(x + 29, y + 7);
                    uint32_t actual = before;
                    if (x >= region.x && x < region.x + region.width && y >= region.y &&
                        y < region.y + region.height)
                      actual = Pixel(x, y);
                    actual = Swap(actual, region.replaces_all ? copy_swapped : old_swapped);
                    const bool written = x / scale >= x0 && x / scale < x1 && y / scale >= y0 &&
                                         y / scale < y1;
                    const uint32_t reference = written ? Swap(Pixel(x, y), copy_swapped)
                                                       : Swap(before, old_swapped);
                    Require(actual == reference);
                  }
                }
              }
            }
          }
        }
      }
    }
  }
  Require(!GetNativeResolveImageCopyRegion(0, 0, 5, 3, 5, 3, 4, 3, 1, 1, false, false));
  Require(!GetNativeResolveImageCopyRegion(0, 0, 5, 3, 5, 3, 5, 2, 1, 1, false, false));
  Require(!GetNativeResolveImageCopyRegion(0, 0, 5, 3, 5, 3, 5, 3, 0, 1, false, false));
  Require(!GetNativeResolveImageCopyRegion(0, 0, 5, 3, 5, 3, 5, 3, 1, UINT32_MAX, false, false));
  Require(!GetNativeResolveImageCopyRegion(0, 0, UINT32_MAX, 3, UINT32_MAX, 3, UINT32_MAX, 3,
                                         2, 1, false, false));
  char report[192];
  std::snprintf(report, sizeof(report),
                "{\"passed\":true,\"cases\":%u,\"partial_copies\":%u,\"rejections\":%u}\n",
                cases + 5, partial, rejected + 5);
  if (argc == 3) {
    std::ofstream output(argv[2]);
    output << report;
    output.flush();
    Require(bool(output));
  }
  std::fputs(report, stdout);
}
