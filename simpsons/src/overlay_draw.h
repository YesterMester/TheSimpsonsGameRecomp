#pragma once

// Drawing shared by the overlay's notifications (overlay.cpp).

#include <cstdint>
#include <string_view>
#include <vector>

#include <imgui.h>

namespace overlay {

// The show's colours.
namespace color {
constexpr ImU32 kYellow = IM_COL32(254, 217, 15, 255);
constexpr ImU32 kText = IM_COL32(245, 247, 250, 255);
}  // namespace color

ImFont* DisplayFont();  // Luckiest Guy: titles
ImFont* BodyFont();     // Roboto
ImFont* BoldFont();     // Roboto Bold

// One unit of a 1280x720 layout, in display units, fitted to the game image.
float Scale();
// The game image in the window (display units), without letterbox borders.
ImVec2 GameMin();
ImVec2 GameMax();
// Seconds since the last frame.
float FrameTime();

ImVec2 MeasureText(ImFont* font, float size, std::string_view text, float wrap = 0);
void Text(ImDrawList* draw, ImFont* font, float size, ImVec2 position, ImU32 color,
          std::string_view text, float wrap = 0);

// An achievement picture (PNG) as a texture. Null when it cannot be decoded.
ImTextureID Picture(uint32_t key, const std::vector<uint8_t>& png);

// Achievement notifications (achievements_ui.cpp); true while one is showing
// or waiting.
bool DrawNotifications();

}  // namespace overlay
