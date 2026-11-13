#include <rex/ui/surface_android.h>

#include <algorithm>

namespace rex::ui {

bool AndroidNativeWindowSurface::GetSizeImpl(uint32_t& width_out,
                                            uint32_t& height_out) const {
  width_out = uint32_t(std::max(0, ANativeWindow_getWidth(window_)));
  height_out = uint32_t(std::max(0, ANativeWindow_getHeight(window_)));
  return width_out && height_out;
}

}  // namespace rex::ui
