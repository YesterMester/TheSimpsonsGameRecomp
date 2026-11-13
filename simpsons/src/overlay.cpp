// Achievement notifications drawn over the game with the runtime's ImGui
// drawer.
//
// The drawer is attached only while a notification shows: with a UI drawer
// the presenter paints from the UI thread instead of presenting the guest
// output directly, so an idle overlay would cost frame pacing. Notifications
// take no input.

#include "overlay.h"
#include "overlay_draw.h"

#include <algorithm>
#include <atomic>
#include <map>
#include <memory>

#include <imgui.h>

#include <rex/logging.h>
#include <rex/ui/guest_output_viewport.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/immediate_drawer.h>
#include <rex/ui/window.h>
#include <rex/ui/windowed_app_context.h>

#include "LuckiestGuyRegular.h"
#include "RobotoBold.h"
#include "RobotoRegular.h"

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "../../tools/rexglue-sdk/thirdparty/tracy/profiler/src/stb_image.h"
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace overlay {
namespace {

rex::ui::Window* g_window = nullptr;
rex::ui::ImGuiDrawer* g_drawer = nullptr;
rex::ui::ImmediateDrawer* g_immediate = nullptr;
ImFont* g_display = nullptr;
ImFont* g_body = nullptr;
ImFont* g_bold = nullptr;
std::atomic<bool> g_alive{false};

// UI thread.
class OverlayDialog;
OverlayDialog* g_dialog = nullptr;
std::map<uint32_t, std::unique_ptr<rex::ui::ImmediateTexture>> g_pictures;
float g_scale = 1, g_frame_time = 0;
ImVec2 g_game_min{0, 0}, g_game_max{1280, 720};

class OverlayDialog final : public rex::ui::ImGuiDialog {
 public:
  explicit OverlayDialog(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}
  // ImGuiDialog deletes itself after closing through its own non-virtual
  // destructor, so the cleanup belongs in OnClose. Shutdown deletes it
  // directly.
  ~OverlayDialog() { Release(); }

 protected:
  void OnClose() override { Release(); }

  void OnDraw(ImGuiIO& io) override {
    g_frame_time = std::clamp(io.DeltaTime, 0.0f, 0.1f);
    // Lay out over the game image, not the window's letterbox borders.
    const auto viewport = rex::ui::GetGuestOutputViewport();
    g_game_min = ImVec2(std::max(0.0f, viewport.x * io.DisplaySize.x),
                        std::max(0.0f, viewport.y * io.DisplaySize.y));
    g_game_max = ImVec2(std::min(io.DisplaySize.x, (viewport.x + viewport.width) * io.DisplaySize.x),
                        std::min(io.DisplaySize.y, (viewport.y + viewport.height) * io.DisplaySize.y));
    if (g_game_max.x - g_game_min.x < 64 || g_game_max.y - g_game_min.y < 64) {
      g_game_min = ImVec2(0, 0);
      g_game_max = io.DisplaySize;
    }
    g_scale = std::max(std::min((g_game_max.x - g_game_min.x) / 1280.0f,
                                (g_game_max.y - g_game_min.y) / 720.0f),
                       0.35f);
    if (!DrawNotifications())
      Close();
  }

 private:
  void Release() {
    if (g_dialog == this)
      g_dialog = nullptr;
  }
};

// UI thread: show the overlay if there is something to show.
void EnsureDialog() {
  if (!g_alive.load() || !g_drawer || g_dialog)
    return;
  g_dialog = new OverlayDialog(g_drawer);
}

}  // namespace

ImFont* DisplayFont() {
  return g_display ? g_display : ImGui::GetFont();
}
ImFont* BodyFont() {
  return g_body ? g_body : ImGui::GetFont();
}
ImFont* BoldFont() {
  return g_bold ? g_bold : BodyFont();
}
float Scale() {
  return g_scale;
}
ImVec2 GameMin() {
  return g_game_min;
}
ImVec2 GameMax() {
  return g_game_max;
}
float FrameTime() {
  return g_frame_time;
}

ImVec2 MeasureText(ImFont* font, float size, std::string_view text, float wrap) {
  return font->CalcTextSizeA(size, FLT_MAX, wrap, text.data(), text.data() + text.size());
}

void Text(ImDrawList* draw, ImFont* font, float size, ImVec2 position, ImU32 color,
          std::string_view text, float wrap) {
  draw->AddText(font, size, position, color, text.data(), text.data() + text.size(), wrap);
}

std::vector<uint8_t> DecodePicture(const std::vector<uint8_t>& png, int* width, int* height) {
  int channels = 0;
  stbi_uc* pixels =
      png.empty() ? nullptr
                  : stbi_load_from_memory(png.data(), int(png.size()), width, height, &channels, 4);
  if (!pixels)
    return {};
  std::vector<uint8_t> rgba(pixels, pixels + size_t(*width) * size_t(*height) * 4);
  stbi_image_free(pixels);
  return rgba;
}

ImTextureID Picture(uint32_t key, const std::vector<uint8_t>& png) {
  auto& texture = g_pictures[key];
  if (!texture && g_immediate) {
    int width = 0, height = 0;
    const auto rgba = DecodePicture(png, &width, &height);
    if (!rgba.empty())
      texture = g_immediate->CreateTexture(uint32_t(width), uint32_t(height),
                                           rex::ui::ImmediateTextureFilter::kLinear, false,
                                           rgba.data());
  }
  return texture ? reinterpret_cast<ImTextureID>(texture.get()) : ImTextureID{};
}

void ConfigureFonts(ImFontAtlas* atlas) {
  // Baked large: without texture updates from the renderer, other sizes are
  // scaled from these.
  static const ImWchar kAscii[] = {0x0020, 0x007E, 0};
  static const ImWchar kLatin[] = {0x0020, 0x00FF, 0x2013, 0x2014, 0x2018, 0x201D, 0x2022,
                                   0x2026, 0};
  ImFontConfig config;
  config.FontDataOwnedByAtlas = false;
  config.OversampleH = 2;
  config.OversampleV = 1;
  g_display = atlas->AddFontFromMemoryTTF(const_cast<unsigned char*>(kLuckiestGuyRegular),
                                          int(sizeof(kLuckiestGuyRegular)), 64.0f, &config, kAscii);
  g_body = atlas->AddFontFromMemoryTTF(const_cast<unsigned char*>(kRobotoRegular),
                                       int(sizeof(kRobotoRegular)), 40.0f, &config, kLatin);
  g_bold = atlas->AddFontFromMemoryTTF(const_cast<unsigned char*>(kRobotoBold),
                                       int(sizeof(kRobotoBold)), 40.0f, &config, kLatin);
}

void Init(rex::ui::Window* window, rex::ui::ImGuiDrawer* drawer,
          rex::ui::ImmediateDrawer* immediate_drawer) {
  g_window = window;
  g_drawer = drawer;
  g_immediate = immediate_drawer;
  g_alive = true;
  // Notifications waiting from before the drawer existed.
  Wake();
}

void Shutdown() {
  if (!g_alive.exchange(false))
    return;
  delete g_dialog;
  g_dialog = nullptr;
  g_pictures.clear();
}

void Wake() {
  if (!g_alive.load() || !g_window)
    return;
  g_window->app_context().CallInUIThread([] { EnsureDialog(); });
}

}  // namespace overlay
