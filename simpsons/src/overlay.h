#pragma once

#include <cstdint>
#include <vector>

struct ImFontAtlas;
namespace rex {
namespace ui {
class ImGuiDrawer;
class ImmediateDrawer;
class Window;
}  // namespace ui
}  // namespace rex

// Achievement notifications, drawn over the game with the runtime's ImGui
// drawer on every platform. Everything else (the debug menu, the
// achievements) is drawn in the game itself (game_ui.h).
namespace overlay {

// ReXApp::OnConfigureFonts and ReXApp::OnCreateDialogs.
void ConfigureFonts(ImFontAtlas* atlas);
void Init(rex::ui::Window* window, rex::ui::ImGuiDrawer* drawer,
          rex::ui::ImmediateDrawer* immediate_drawer);
void Shutdown();

// A notification may be waiting (any thread).
void Wake();

// A PNG's pixels as RGBA, empty if it cannot be read.
std::vector<uint8_t> DecodePicture(const std::vector<uint8_t>& png, int* width, int* height);

}  // namespace overlay
