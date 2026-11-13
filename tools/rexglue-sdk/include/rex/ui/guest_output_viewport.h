#pragma once

namespace rex::ui {

// The full guest image in normalized window coordinates, before clipping.
// Negative offsets describe overscan; unused window borders lie outside it.
struct GuestOutputViewport {
  float x = 0, y = 0, width = 1, height = 1;
};

GuestOutputViewport GetGuestOutputViewport();

}  // namespace rex::ui
