// Device-specific button prompts, drawn through the game's font batch. This
// keeps prompts in the same frame, viewport and effects as the original UI.

#include "game_prompts.h"
#include "game_ui.h"
#include "cutscene_skip.h"
#include "host_memory.h"
#include "game_menu.h"
#include "game_menu_pointer.h"
#include "guest_memory.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <rex/cvar.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>

REXCVAR_DEFINE_BOOL(native_button_prompts, true, "Input",
                    "Use the assigned input's button prompts");
REXCVAR_DEFINE_BOOL(button_prompt_log, false, "Input", "Log button tag and viewport coverage");

REX_EXTERN(__imp__sub_826BEEE0);
REX_EXTERN(sub_826BEEE0);
REX_EXTERN(__imp__sub_82728138);
REX_EXTERN(sub_82728138);
REX_EXTERN(__imp__sub_826BE930);
REX_EXTERN(sub_826BE930);
REX_EXTERN(sub_826BE8C0);
REX_EXTERN(__imp__sub_826C00D8);
REX_EXTERN(sub_826C00D8);
REX_EXTERN(__imp__sub_827FA580);
REX_EXTERN(sub_827FA580);
REX_EXTERN(__imp__sub_82734200);
REX_EXTERN(sub_82734200);

namespace {

using rex::input::ButtonPrompt;
using rex::input::PromptButton;
using rex::input::PromptStyle;
using simpsons::GuestToHost;
using simpsons::LoadGuestFloat;
using simpsons::LoadGuestU32;
using simpsons::StoreGuestFloat;
using simpsons::StoreGuestU32;

constexpr uint32_t kFontManager = 0x82D08EA8;
constexpr uint32_t kCameraManager = 0x82D08B10;
constexpr uint32_t kFrontendState = 0x82D08C98;
constexpr uint32_t kClockPaused = 0x82D572DC;
constexpr uint32_t kRenderDevice = 0x82D0CA68;
constexpr uint32_t kFontColours = 0x82D61DFC;
constexpr uint32_t kAptTransform = 0x82D08BF4;
constexpr uint32_t kHudRenderContext = 0x82D097AC;

rex::input::InputSystem* g_input = nullptr;
rex::memory::Memory* g_memory = nullptr;
rex::runtime::FunctionDispatcher* g_dispatcher = nullptr;
uint32_t g_metrics = 0, g_start_prompt = 0;
std::atomic<bool> g_active{false};
std::atomic<uint32_t> g_metrics_offset{0};
std::map<std::pair<uint32_t, uint8_t>, PromptButton> g_glyph_buttons;
std::set<std::string> g_logged_tags;
std::set<std::pair<uint32_t, PromptStyle>> g_logged_metrics;
std::set<std::pair<uint32_t, PromptStyle>> g_logged_players;
thread_local uint32_t g_text_player = 0;
thread_local bool g_text_player_valid = false;
thread_local std::optional<PromptButton> g_standalone_button;
// The bus stop map's "Navigate" draws the whole D-pad with the [LDLeft]
// glyph. It keeps that picture instead of becoming a "Left" key.
thread_local bool g_whole_dpad = false;
thread_local std::optional<uint32_t> g_hud_pass_player;

uint16_t U16(const uint8_t* base, uint32_t address) {
  const auto* p = GuestToHost(base, address);
  return uint16_t(p[0]) << 8 | p[1];
}
void Put16(uint8_t* base, uint32_t address, uint16_t value) {
  auto* p = GuestToHost(base, address);
  p[0] = uint8_t(value >> 8);
  p[1] = uint8_t(value);
}

std::optional<PromptButton> ParseButton(std::string tag) {
  for (auto& c : tag)
    if (c >= 'a' && c <= 'z')
      c -= 'a' - 'A';
  if (tag.starts_with("B="))
    tag.erase(0, 2);
  constexpr std::pair<std::string_view, PromptButton> tags[] = {
      {"A", PromptButton::kA},
      {"B", PromptButton::kB},
      {"X", PromptButton::kX},
      {"Y", PromptButton::kY},
      {"LB", PromptButton::kLeftShoulder},
      {"RB", PromptButton::kRightShoulder},
      {"LT", PromptButton::kLeftTrigger},
      {"RT", PromptButton::kRightTrigger},
      {"START", PromptButton::kStart},
      {"BACK", PromptButton::kBack},
      {"SELECT", PromptButton::kBack},
      {"RDDOWN", PromptButton::kA},
      {"RDRIGHT", PromptButton::kB},
      {"RDLEFT", PromptButton::kX},
      {"RDUP", PromptButton::kY},
      {"LSTOP", PromptButton::kLeftShoulder},
      {"LSBOTTOM", PromptButton::kLeftTrigger},
      {"RSTOP", PromptButton::kRightShoulder},
      {"RSBOTTOM", PromptButton::kRightTrigger},
      {"LDUP", PromptButton::kDpadUp},
      {"LDDOWN", PromptButton::kDpadDown},
      {"LDLEFT", PromptButton::kDpadLeft},
      {"LDRIGHT", PromptButton::kDpadRight},
      {"UP", PromptButton::kDpadUp},
      {"DOWN", PromptButton::kDpadDown},
      {"LEFT", PromptButton::kDpadLeft},
      {"RIGHT", PromptButton::kDpadRight},
      {"LS", PromptButton::kLeftStick},
      {"RS", PromptButton::kRightStick},
      {"LSTICK", PromptButton::kLeftStick},
      {"RSTICK", PromptButton::kRightStick},
      {"L3", PromptButton::kLeftStickPress},
      {"R3", PromptButton::kRightStickPress},
      {"DPADUP", PromptButton::kDpadUp},
      {"DPADDOWN", PromptButton::kDpadDown},
      {"DPADLEFT", PromptButton::kDpadLeft},
      {"DPADRIGHT", PromptButton::kDpadRight},
  };
  for (const auto& [name, button] : tags)
    if (tag == name)
      return button;
  return std::nullopt;
}

struct Point {
  float x, y;
};
struct Box {
  float x, y, w, h;
};

Box RasterBox(const uint8_t* base, uint32_t raster) {
  if (!raster)
    return {};
  return {float(int16_t(U16(base, raster + 28))), float(int16_t(U16(base, raster + 30))),
          float(LoadGuestU32(base, raster + 12)), float(LoadGuestU32(base, raster + 16))};
}

uint32_t PlayerAt(const uint8_t* base, uint32_t manager, Point point) {
  if (!g_input)
    return 0;
  if (GameMenuIsVisible())
    return GameMenuPromptPlayer();
  if (!game_ui::LevelRunning(LoadGuestU32(base, kFrontendState)))
    return g_input->GetLastActivePlayer();
  if (LoadGuestU32(base, kClockPaused) == 1)
    return g_input->GetMenuPlayer();
  const auto cameras = LoadGuestU32(base, kCameraManager);
  if (!cameras)
    return 0;
  // Slot 0 is the full-screen view; slots 1 and 2 are the two players'
  // split views. Director ids identify characters, not controller slots.
  if (LoadGuestU32(base, cameras + 300) < 3 || LoadGuestU32(base, cameras + 304) != 0xFFFFFFFF ||
      LoadGuestU32(base, cameras + 308) == 0xFFFFFFFF ||
      LoadGuestU32(base, cameras + 312) == 0xFFFFFFFF)
    return 0;
  const auto ui_camera = LoadGuestU32(base, manager + 44);
  const auto ui_raster = ui_camera ? LoadGuestU32(base, ui_camera + 96) : 0;
  const auto ui_box = RasterBox(base, ui_raster);
  const float sx = LoadGuestFloat(base, manager + 80), sy = LoadGuestFloat(base, manager + 92);
  if (!(sx > 0 && sy > 0))
    return 0;
  point = {point.x / sx + ui_box.x, point.y / sy + ui_box.y};
  int owner = -1;
  for (uint32_t player = 0; player < 2; ++player) {
    // The raster itself can be a separate half-size target with offset 0.
    // The manager's integer bounds locate it in the final guest frame.
    const uint32_t bounds = cameras + 36 + (player + 1) * 16;
    const float left = float(int32_t(LoadGuestU32(base, bounds)));
    const float top = float(int32_t(LoadGuestU32(base, bounds + 4)));
    const Box box{left, top, float(int32_t(LoadGuestU32(base, bounds + 8))) - left,
                  float(int32_t(LoadGuestU32(base, bounds + 12))) - top};
    if (box.w > 0 && box.h > 0 && point.x >= box.x && point.y >= box.y && point.x < box.x + box.w &&
        point.y < box.y + box.h) {
      if (owner >= 0)
        return g_text_player_valid ? g_text_player : 0;
      owner = int(player);
    }
  }
  return owner >= 0 ? uint32_t(owner) : g_text_player_valid ? g_text_player : 0;
}

ButtonPrompt CurrentPrompt(uint32_t player, PromptButton button) {
  bool cutscene_keyboard = false;
  if (CutsceneSkipPromptPlayer(&player, &cutscene_keyboard))
    return cutscene_keyboard ? rex::input::MakeButtonPrompt(PromptStyle::kKeyboard, button)
                             : g_input->GetButtonPrompt(player, button);
  const bool touch = GameMenuPointerUsesTouch();
  const bool saves = game_ui::Current() == game_ui::Screen::kSaves;
  if (!(saves ? game_ui::KeyboardPrompts() || GameMenuPointerUsesKeyboard()
              : GameMenuUsesKeyboard() || GameMenuPointerUsesKeyboard()) &&
      !touch)
    return g_input->GetButtonPrompt(player, button);
  // With touch, every button the screen offers is a chip you tap (the menu
  // pointer records it as a target); directions keep their names.
  const bool tap = button == PromptButton::kA || button == PromptButton::kX ||
                   button == PromptButton::kY || button == PromptButton::kStart ||
                   button == PromptButton::kBack;
  const bool direction = button == PromptButton::kDpadUp || button == PromptButton::kDpadDown ||
                         button == PromptButton::kDpadLeft || button == PromptButton::kDpadRight;
  if (touch && !tap && !direction && button != PromptButton::kB)
    return g_input->GetButtonPrompt(player, button);
  auto prompt = rex::input::MakeButtonPrompt(PromptStyle::kKeyboard, button);
  // Navigation inside Options uses these menu keys independently of gameplay
  // bindings, including when keyboard gameplay has not been enabled yet.
  switch (button) {
    case PromptButton::kA:
      prompt.label = touch ? "Tap" : "Enter";
      break;
    case PromptButton::kX:
      if (saves && !touch) {
        prompt.label = game_ui::SavesKeyboard() ? "Backspace" : "F2";
        break;
      }
      [[fallthrough]];
    case PromptButton::kY:
      if (saves && !touch) {
        prompt.label = game_ui::SavesKeyboard() ? "Enter" : "F3";
        break;
      }
    case PromptButton::kStart:
    case PromptButton::kBack:
      if (touch)
        prompt.label = "Tap";
      break;
    case PromptButton::kB:
      prompt.label = touch ? "Back" : "Esc";
      break;
    case PromptButton::kDpadUp:
      prompt.label = "Up";
      break;
    case PromptButton::kDpadDown:
      prompt.label = "Down";
      break;
    case PromptButton::kDpadLeft:
      prompt.label = "Left";
      break;
    case PromptButton::kDpadRight:
      prompt.label = "Right";
      break;
    default:
      break;
  }
  return prompt;
}

bool OriginalPrompt(const ButtonPrompt& prompt) {
  return (prompt.style == PromptStyle::kXbox || prompt.style == PromptStyle::kSteamDeck) &&
         prompt.button != PromptButton::kStart && prompt.button != PromptButton::kBack;
}

uint32_t Glyph(const uint8_t* base, uint32_t font, uint8_t character) {
  const auto table = font ? LoadGuestU32(base, font + 36) : 0;
  return table ? LoadGuestU32(base, table + character * 4) : 0;
}

std::optional<PromptButton> ButtonGlyph(uint32_t font, uint8_t character) {
  const auto found = g_glyph_buttons.find({font, character});
  if (found != g_glyph_buttons.end())
    return found->second;
  // The game's resolver has this fixed twenty-entry button font table.
  // Some HUD labels supply a glyph directly instead of parsing a tag first.
  constexpr PromptButton buttons[] = {PromptButton::kStart,
                                      PromptButton::kBack,
                                      PromptButton::kLeftTrigger,
                                      PromptButton::kLeftShoulder,
                                      PromptButton::kRightTrigger,
                                      PromptButton::kRightShoulder,
                                      PromptButton::kDpadLeft,
                                      PromptButton::kDpadRight,
                                      PromptButton::kDpadUp,
                                      PromptButton::kDpadDown,
                                      PromptButton::kX,
                                      PromptButton::kB,
                                      PromptButton::kY,
                                      PromptButton::kA,
                                      PromptButton::kLeftStick,
                                      PromptButton::kRightStick,
                                      PromptButton::kDpadDown,
                                      PromptButton::kDpadUp,
                                      PromptButton::kDpadLeft,
                                      PromptButton::kDpadRight};
  return character < std::size(buttons) ? std::optional(buttons[character]) : std::nullopt;
}

uint32_t PromptFont(const uint8_t* base, uint32_t manager) {
  const auto usable = [base](uint32_t font) {
    const auto letter = Glyph(base, font, 'A'), white = Glyph(base, font, '_');
    return letter && white && U16(base, letter + 2) > 1 && U16(base, letter + 4) > 1 &&
           U16(base, white + 2) > 1 && U16(base, white + 4) > 1;
  };
  const auto current = LoadGuestU32(base, manager + 28);
  if (usable(current))
    return current;
  // Icon-only HUD labels select a metric-only font whose atlas is transparent
  // and whose glyphs are 1x1. Use another live font owned by the font manager;
  // keeping a front-end font pointer across a level load would outlive it.
  const auto fonts = LoadGuestU32(base, manager + 16);
  const auto count = LoadGuestU32(base, manager + 20);
  for (uint32_t i = 0; fonts && i < std::min(count, 128u); ++i) {
    const auto font = LoadGuestU32(base, fonts + i * 4);
    if (usable(font))
      return font;
  }
  return 0;
}

class PromptBatch {
 public:
  PromptBatch(PPCContext& ctx, uint8_t* base, uint32_t manager, uint32_t origin, uint32_t rotation)
      : ctx_(ctx), base_(base), manager_(manager), origin_(origin), rotation_(rotation) {
    scale_x_ = LoadGuestFloat(base, manager + 80);
    scale_y_ = LoadGuestFloat(base, manager + 92);
    if (!(scale_x_ > 0 && scale_y_ > 0) || !std::isfinite(scale_x_) || !std::isfinite(scale_y_))
      return;
    std::memcpy(colours_.data(), GuestToHost(base, kFontColours), 16);
    font_ = PromptFont(base, manager);
    const auto texture = font_ ? LoadGuestU32(base, font_ + 1060) : 0;
    raster_ = texture ? LoadGuestU32(base, texture) : 0;
    if (raster_) {
      width_ = float(LoadGuestU32(base, raster_ + 12));
      height_ = float(LoadGuestU32(base, raster_ + 16));
    }
    const auto glyph = Glyph(base, font_, '_');
    if (!glyph || width_ <= 0 || height_ <= 0 || LoadGuestU32(base, manager + 76))
      return;
    // Font atlas coordinates have four fractional bits. The stock text
    // renderer applies the same 1/16 conversion before normalizing the UVs.
    white_u_ = (U16(base, glyph + 12) + U16(base, glyph + 16)) * 0.5f / (16.0f * width_);
    white_v_ = (U16(base, glyph + 14) + U16(base, glyph + 18)) * 0.5f / (16.0f * height_);
    Flush();
    Bind(raster_);
    valid_ = true;
  }
  ~PromptBatch() {
    if (!valid_)
      return;
    Flush();
    const auto font = LoadGuestU32(base_, manager_ + 32);
    const auto texture = font ? LoadGuestU32(base_, font + 1060) : 0;
    if (texture)
      Bind(LoadGuestU32(base_, texture));
  }
  bool valid() const { return valid_; }
  float x_scale() const { return scale_x_; }
  Box ToPixels(Box box) const {
    return {box.x / scale_x_, box.y / scale_y_, box.w / scale_x_, box.h / scale_y_};
  }

  void Rect(Box box, uint32_t rgb) {
    Quad({Point{box.x, box.y},
          {box.x, box.y + box.h},
          {box.x + box.w, box.y},
          {box.x + box.w, box.y + box.h}},
         {white_u_, white_v_, white_u_, white_v_}, rgb);
  }
  void Line(Point a, Point b, float thickness, uint32_t rgb) {
    const float dx = b.x - a.x, dy = b.y - a.y, length = std::hypot(dx, dy);
    if (length <= 0)
      return;
    const float nx = -dy / length * thickness * .5f, ny = dx / length * thickness * .5f;
    Quad({Point{a.x + nx, a.y + ny},
          {a.x - nx, a.y - ny},
          {b.x + nx, b.y + ny},
          {b.x - nx, b.y - ny}},
         {white_u_, white_v_, white_u_, white_v_}, rgb);
  }
  void Circle(Point center, float radius, uint32_t rgb) {
    const int strips = std::clamp(int(radius * 2), 12, 32);
    const float step = radius * 2 / strips;
    for (int i = 0; i < strips; ++i) {
      const float y = -radius + (i + .5f) * step;
      const float half = std::sqrt(std::max(0.0f, radius * radius - y * y));
      Rect({center.x - half, center.y - radius + i * step, half * 2, step + .05f}, rgb);
    }
  }
  float LabelWidth(const std::string& label, float height) const {
    float advance = 0;
    for (unsigned char c : label) {
      const auto glyph = Glyph(base_, font_, c);
      if (glyph)
        advance += U16(base_, glyph + 6);
    }
    const float font_height = float(LoadGuestU32(base_, font_ + 24));
    return font_height > 0 ? advance * height * .7f / font_height / .8f : 0;
  }
  void Label(Box box, const std::string& label) {
    float advance = 0;
    for (unsigned char c : label) {
      const auto glyph = Glyph(base_, font_, c);
      if (glyph)
        advance += U16(base_, glyph + 6);
    }
    const float font_height = float(LoadGuestU32(base_, font_ + 24));
    if (advance <= 0 || font_height <= 0)
      return;
    const float scale = std::min(box.w * .8f / advance, box.h * .7f / font_height);
    float x = box.x + (box.w - advance * scale) * .5f;
    const float y = box.y + (box.h - font_height * scale) * .5f;
    for (unsigned char c : label) {
      const auto glyph = Glyph(base_, font_, c);
      if (!glyph)
        continue;
      const float left = x + int16_t(U16(base_, glyph + 8)) * scale;
      const float top = y + int16_t(U16(base_, glyph + 10)) * scale;
      const float w = U16(base_, glyph + 2) * scale, h = U16(base_, glyph + 4) * scale;
      Quad({Point{left, top}, {left, top + h}, {left + w, top}, {left + w, top + h}},
           {U16(base_, glyph + 12) / (16.0f * width_), U16(base_, glyph + 14) / (16.0f * height_),
            U16(base_, glyph + 16) / (16.0f * width_), U16(base_, glyph + 18) / (16.0f * height_)},
           0xFFFFFF);
      x += U16(base_, glyph + 6) * scale;
    }
  }

 private:
  void Flush() {
    PPCContext saved = ctx_;
    ctx_.r3.u64 = manager_;
    sub_826BE8C0(ctx_, base_);
    ctx_ = saved;
  }
  void Bind(uint32_t raster) {
    const auto device = LoadGuestU32(base_, kRenderDevice);
    if (!device)
      return;
    const auto function = LoadGuestU32(base_, device + 32);
    PPCContext saved = ctx_;
    ctx_.r3.u64 = 1;
    ctx_.r4.u64 = raster;
    ctx_.ctr.u64 = function;
    if (auto* target = g_dispatcher->GetFunction(function))
      target(ctx_, base_);
    ctx_ = saved;
  }
  void Quad(const std::array<Point, 4>& points, const std::array<float, 4>& uv, uint32_t rgb) {
    uint16_t count = U16(base_, manager_ + 72), capacity = U16(base_, manager_ + 74);
    if (count + 6 > capacity) {
      Flush();
      count = 0;
    }
    if (capacity < 6)
      return;
    const auto vertices = LoadGuestU32(base_, manager_ + 68);
    if (!vertices)
      return;
    const auto camera = LoadGuestU32(base_, manager_ + 44);
    const float z = LoadGuestFloat(base_, manager_ + 52);
    const float depth =
        (z - LoadGuestFloat(base_, camera + 144)) / LoadGuestFloat(base_, camera + 140);
    Point origin{};
    if (origin_)
      origin = {LoadGuestFloat(base_, origin_) / scale_x_,
                LoadGuestFloat(base_, origin_ + 4) / scale_y_};
    float cosine = 1, sine = 0;
    if (rotation_) {
      cosine = LoadGuestFloat(base_, rotation_);
      sine = LoadGuestFloat(base_, rotation_ + 4);
    }
    constexpr uint32_t indices[] = {0, 1, 2, 3, 2, 1};
    for (uint32_t i = 0; i < 6; ++i) {
      const auto corner = indices[i];
      float x = points[corner].x, y = points[corner].y;
      if (rotation_) {
        x -= origin.x;
        y -= origin.y;
        const float rotated_x = std::fma(cosine, x, -sine * y) + origin.x;
        y = std::fma(sine, x, cosine * y) + origin.y;
        x = rotated_x;
      }
      const uint32_t address = vertices + (count + i) * 28;
      StoreGuestFloat(base_, address, x);
      StoreGuestFloat(base_, address + 4, y);
      StoreGuestFloat(base_, address + 8, z);
      StoreGuestFloat(base_, address + 12, depth);
      StoreGuestU32(base_, address + 16, (uint32_t(colours_[corner * 4 + 3]) << 24) | rgb);
      StoreGuestFloat(base_, address + 20, uv[corner >= 2 ? 2 : 0]);
      StoreGuestFloat(base_, address + 24, uv[corner == 1 || corner == 3 ? 3 : 1]);
    }
    Put16(base_, manager_ + 72, count + 6);
  }
  PPCContext& ctx_;
  uint8_t* base_;
  uint32_t manager_, origin_, rotation_, font_ = 0, raster_ = 0;
  float width_ = 0, height_ = 0, white_u_ = 0, white_v_ = 0;
  float scale_x_ = 1, scale_y_ = 1;
  std::array<uint8_t, 16> colours_{};
  bool valid_ = false;
};

void DrawPrompt(PromptBatch& batch, Box box, const ButtonPrompt& prompt) {
  const bool face = prompt.button <= PromptButton::kY && prompt.style != PromptStyle::kKeyboard;
  const Point center{box.x + box.w * .5f, box.y + box.h * .5f};
  if (face) {
    const float radius = std::min(box.w, box.h) * .47f;
    batch.Circle(center, radius, 0x080C14);
    batch.Circle(center, radius * .88f, 0xF1F5FF);
    batch.Circle(center, radius * .77f, 0x101726);
    if (prompt.style == PromptStyle::kPlayStation) {
      const float r = radius * .42f, thickness = std::max(1.2f, radius * .11f);
      if (prompt.button == PromptButton::kA) {
        batch.Line({center.x - r, center.y - r}, {center.x + r, center.y + r}, thickness, 0x82B9FF);
        batch.Line({center.x - r, center.y + r}, {center.x + r, center.y - r}, thickness, 0x82B9FF);
      } else if (prompt.button == PromptButton::kB) {
        for (int i = 0; i < 20; ++i) {
          const float a = i * 6.283185307f / 20, b = (i + 1) * 6.283185307f / 20;
          batch.Line({center.x + std::cos(a) * r, center.y + std::sin(a) * r},
                     {center.x + std::cos(b) * r, center.y + std::sin(b) * r}, thickness, 0xFF7885);
        }
      } else if (prompt.button == PromptButton::kX) {
        batch.Line({center.x - r, center.y - r}, {center.x + r, center.y - r}, thickness, 0xF4A0D8);
        batch.Line({center.x + r, center.y - r}, {center.x + r, center.y + r}, thickness, 0xF4A0D8);
        batch.Line({center.x + r, center.y + r}, {center.x - r, center.y + r}, thickness, 0xF4A0D8);
        batch.Line({center.x - r, center.y + r}, {center.x - r, center.y - r}, thickness, 0xF4A0D8);
      } else {
        batch.Line({center.x, center.y - r}, {center.x + r, center.y + r}, thickness, 0x7CE7B4);
        batch.Line({center.x + r, center.y + r}, {center.x - r, center.y + r}, thickness, 0x7CE7B4);
        batch.Line({center.x - r, center.y + r}, {center.x, center.y - r}, thickness, 0x7CE7B4);
      }
      return;
    }
  } else {
    batch.Rect(box, 0x080C14);
    const float border = std::max(1.0f, box.h * .055f);
    batch.Rect({box.x + border, box.y + border, box.w - border * 2, box.h - border * 2}, 0xF1F5FF);
    batch.Rect({box.x + border * 2, box.y + border * 2, box.w - border * 4, box.h - border * 4},
               0x101726);
  }
  batch.Label(box, prompt.label);
}

}  // namespace

void InitGamePrompts(rex::Runtime* runtime) {
  g_input = dynamic_cast<rex::input::InputSystem*>(runtime->input_system());
  g_memory = runtime->memory();
  g_dispatcher = runtime->function_dispatcher();
  g_metrics = host_memory::Alloc(65536);
  g_start_prompt = host_memory::Alloc(256);
  if (g_start_prompt)
    std::memcpy(g_memory->TranslateVirtual(g_start_prompt), "Press [Start]", 14);
  g_active = true;
}

void ShutdownGamePrompts() {
  // Runtime owns the metrics arena. Keep it alive until its game threads have
  // stopped; OnShutdown runs before title termination.
  g_active = false;
}

REX_FUNC(sub_826BEEE0) {
  const auto manager = ctx.r3.u32, text = ctx.r4.u32, index = ctx.r6.u32;
  __imp__sub_826BEEE0(ctx, base);
  if (!g_active || (!REXCVAR_GET(native_button_prompts) && !REXCVAR_GET(button_prompt_log)) ||
      ctx.r3.s32 != 0)
    return;
  std::string tag;
  const auto* string = GuestToHost(base, text);
  if (string[0] != '[')
    return;
  for (uint32_t i = 1; i < 32 && string[i] && string[i] != ']'; ++i)
    tag += char(string[i]);
  const auto glyph = uint8_t(LoadGuestU32(base, index));
  const auto font = LoadGuestU32(base, manager + 32);
  const auto button = ParseButton(tag);
  if (button)
    g_glyph_buttons[{font, glyph}] = *button;
  if (REXCVAR_GET(button_prompt_log) && g_logged_tags.insert(tag).second) {
    REXLOG_INFO("[button-prompts] tag [{}] glyph {} font {:08X} {}", tag, glyph, font,
                button ? "mapped" : "unmapped");
  }
}

REX_FUNC(sub_827FA580) {
  const auto text_block = ctx.r3.u32;
  auto replacement = game_ui::SavesTextForText(base, text_block);
  if (game_ui::DrawNativeSavesWidget(ctx, base, text_block))
    return;
  if (!replacement)
    replacement = GameMenuTitleForText(base, text_block);
  // The English title screen spells START literally instead of using its
  // button tag. Route that instruction through the same native prompt path.
  if (!replacement && g_active && g_start_prompt && REXCVAR_GET(native_button_prompts) &&
      LoadGuestU32(base, kFrontendState) == 6) {
    const auto localized = LoadGuestU32(base, text_block + 116);
    const auto string = localized ? localized : LoadGuestU32(base, text_block + 84);
    if (string) {
      const std::string_view text(reinterpret_cast<const char*>(GuestToHost(base, string)));
      for (const auto name : {"START", "Start"}) {
        const auto start = text.find(name);
        if (start != std::string_view::npos && text.size() + 3 < 256) {
          std::string title(text);
          title.replace(start, 5, "[Start]");
          std::memcpy(g_memory->TranslateVirtual(g_start_prompt), title.c_str(), title.size() + 1);
          replacement = g_start_prompt;
          break;
        }
      }
    }
  }
  float save_scale = 1;
  const auto original_colour = LoadGuestU32(base, text_block + 44);
  auto save_colour = original_colour;
  const bool save_row =
      game_ui::SavesTextStyle(ctx, base, text_block, replacement, &save_scale, &save_colour);
  const float original_size = save_row ? LoadGuestFloat(base, text_block + 72) : 0;
  if (save_row) {
    StoreGuestFloat(base, text_block + 72, original_size * save_scale);
    StoreGuestU32(base, text_block + 44, save_colour);
  }
  const auto original_title = replacement ? LoadGuestU32(base, text_block + 116) : 0;
  if (replacement)
    StoreGuestU32(base, text_block + 116, replacement);
  RecordGameMenuPointerText(base, text_block);
  const auto previous = g_text_player;
  const bool previous_valid = g_text_player_valid;
  const auto previous_button = g_standalone_button;
  const bool previous_whole_dpad = g_whole_dpad;
  g_standalone_button.reset();
  {
    const auto name = text_block ? LoadGuestU32(base, text_block + 84) : 0;
    g_whole_dpad =
        name &&
        std::string_view(reinterpret_cast<const char*>(GuestToHost(base, name))) == "$FE_Dpad";
  }
  if (g_active && text_block) {
    const auto localized = LoadGuestU32(base, text_block + 116);
    const auto string = localized ? localized : LoadGuestU32(base, text_block + 84);
    if (string) {
      const auto* text = reinterpret_cast<const char*>(GuestToHost(base, string));
      if (text[0] == '[') {
        const size_t length = std::strlen(text);
        if (length > 2 && length < 32 && text[length - 1] == ']')
          g_standalone_button = ParseButton(std::string(text + 1, length - 2));
      }
    }
  }
  if (g_active && g_input && REXCVAR_GET(native_button_prompts)) {
    const auto manager = LoadGuestU32(base, kFontManager),
               transform = LoadGuestU32(base, kAptTransform);
    const auto matrix = transform ? LoadGuestU32(base, transform + 4) : 0;
    if (manager && matrix) {
      const auto text = ctx.r3.u32;
      Point center{(LoadGuestFloat(base, text + 4) + LoadGuestFloat(base, text + 12)) * .5f,
                   (LoadGuestFloat(base, text + 8) + LoadGuestFloat(base, text + 16)) * .5f};
      center = {
          std::fma(center.x, LoadGuestFloat(base, matrix), LoadGuestFloat(base, matrix + 48)),
          std::fma(center.y, LoadGuestFloat(base, matrix + 4), LoadGuestFloat(base, matrix + 52))};
      g_text_player = PlayerAt(base, manager, center);
      g_text_player_valid = true;
    }
  }
  __imp__sub_827FA580(ctx, base);
  if (replacement)
    StoreGuestU32(base, text_block + 116, original_title);
  if (save_row) {
    StoreGuestFloat(base, text_block + 72, original_size);
    StoreGuestU32(base, text_block + 44, original_colour);
  }
  g_text_player = previous;
  g_text_player_valid = previous_valid;
  g_standalone_button = previous_button;
  g_whole_dpad = previous_whole_dpad;
}

REX_FUNC(sub_82728138) {
  const auto font = ctx.r3.u32, out = ctx.r5.u32;
  const auto character = uint8_t(ctx.r4.u32);
  __imp__sub_82728138(ctx, base);
  if (!ctx.r3.u32 || !g_active || !g_input || !g_metrics || !REXCVAR_GET(native_button_prompts))
    return;
  const auto manager = LoadGuestU32(base, kFontManager);
  if (!manager || font != LoadGuestU32(base, manager + 32))
    return;
  const auto button = ButtonGlyph(font, character);
  if (!button)
    return;
  // Footer icons have their own text fields next to the action label. Their
  // keycap grows away from that label at draw time, rather than changing the
  // field's advance and overlapping the neighbouring field.
  if (g_standalone_button == button)
    return;
  const auto prompt = CurrentPrompt(g_text_player_valid ? g_text_player : 0, *button);
  if (OriginalPrompt(prompt) ||
      (prompt.style == PromptStyle::kPlayStation && prompt.button <= PromptButton::kY))
    return;
  const auto original = LoadGuestU32(base, out);
  const auto old_width = U16(base, original + 2), height = U16(base, original + 4);
  const uint32_t new_width = std::max(
      uint32_t(old_width), uint32_t(std::ceil(height * (.55f * prompt.label.size() + .45f))));
  if (REXCVAR_GET(button_prompt_log) && g_logged_metrics.insert({font, prompt.style}).second)
    REXLOG_INFO("[button-prompts] metrics font {:08X} glyph {} style {}: {}x{} -> {}, advance {}",
                font, character, int(prompt.style), old_width, height, new_width,
                U16(base, original + 6));
  if (new_width == old_width || new_width > 2048)
    return;
  const auto copied = g_metrics + (g_metrics_offset.fetch_add(32) % 65536);
  std::memcpy(GuestToHost(base, copied), GuestToHost(base, original), 20);
  Put16(base, copied + 2, uint16_t(new_width));
  Put16(base, copied + 6,
        uint16_t(std::min(65535u, uint32_t(U16(base, original + 6)) + new_width - old_width)));
  StoreGuestU32(base, out, copied);
}

REX_FUNC(sub_826BE930) {
  if (!g_active || !g_input || !REXCVAR_GET(native_button_prompts) || ctx.r7.u32 != 0) {
    __imp__sub_826BE930(ctx, base);
    return;
  }
  const auto manager = ctx.r3.u32, rectangle = ctx.r4.u32;
  const auto font = LoadGuestU32(base, manager + 32);
  const auto button = ButtonGlyph(font, uint8_t(ctx.r29.u32));
  if (!button || g_whole_dpad) {
    __imp__sub_826BE930(ctx, base);
    return;
  }
  Box box{LoadGuestFloat(base, rectangle), LoadGuestFloat(base, rectangle + 4),
          LoadGuestFloat(base, rectangle + 8), LoadGuestFloat(base, rectangle + 12)};
  if (GameMenuPointerActive()) {
    int key = 0;
    switch (*button) {
      case PromptButton::kA:
        key = 6;
        break;
      case PromptButton::kB:
        key = 7;
        break;
      case PromptButton::kX:
        key = 8;
        break;
      case PromptButton::kY:
        key = 9;
        break;
      case PromptButton::kStart:
        key = 1;
        break;
      case PromptButton::kDpadLeft:
        key = 2;
        break;
      case PromptButton::kDpadRight:
        key = 3;
        break;
      case PromptButton::kDpadUp:
        key = 4;
        break;
      case PromptButton::kDpadDown:
        key = 5;
        break;
      default:
        break;
    }
    const float sx = LoadGuestFloat(base, manager + 80), sy = LoadGuestFloat(base, manager + 92);
    if (key && sx > 0 && sy > 0)
      RecordGameMenuPointerButton(box.x / sx, box.y / sy, box.w / sx, box.h / sy, key);
  }
  // Native save footers already have large touchable action labels. Keep
  // their targets, without drawing an extra keyboard keycap saying "Tap".
  if (game_ui::Current() == game_ui::Screen::kSaves && GameMenuPointerUsesTouch() &&
      g_standalone_button)
    return;
  const auto position_player = PlayerAt(base, manager, {box.x + box.w * .5f, box.y + box.h * .5f});
  const bool gameplay =
      game_ui::LevelRunning(LoadGuestU32(base, kFrontendState)) && LoadGuestU32(base, kClockPaused) != 1;
  const auto player = gameplay              ? g_hud_pass_player.value_or(position_player)
                      : g_text_player_valid ? g_text_player
                                            : position_player;
  const auto prompt = CurrentPrompt(player, *button);
  if (REXCVAR_GET(button_prompt_log) && g_logged_players.insert({player, prompt.style}).second)
    REXLOG_INFO(
        "[button-prompts] player {} style {} at {}, {} size {}, {} (scale {}, {}), "
        "manager {:08X}, font {:08X}, colours {:08X}, callback {:08X}, capacity {}",
        player + 1, int(prompt.style), box.x, box.y, box.w, box.h,
        LoadGuestFloat(base, manager + 80), LoadGuestFloat(base, manager + 92), manager,
        LoadGuestU32(base, manager + 28), LoadGuestU32(base, kFontColours),
        LoadGuestU32(base, manager + 76), U16(base, manager + 74));
  if (OriginalPrompt(prompt) || !(box.w > 0 && box.h > 0)) {
    __imp__sub_826BE930(ctx, base);
    return;
  }
  PPCContext saved = ctx;
  {
    PromptBatch batch(ctx, base, manager, ctx.r8.u32, ctx.r9.u32);
    if (!batch.valid()) {
      ctx = saved;
      __imp__sub_826BE930(ctx, base);
      return;
    }
    // Shape sizes and stroke widths are pixels. HUD text can use normalized
    // coordinates, unlike the menus; convert once before building geometry.
    box = batch.ToPixels(box);
    if (g_standalone_button == button &&
        !(prompt.style == PromptStyle::kPlayStation && prompt.button <= PromptButton::kY)) {
      const float width = std::max(box.w, batch.LabelWidth(prompt.label, box.h));
      // The native footer puts action labels inward of their icon fields.
      // This also covers Delete and the other save/episode screen actions.
      const float center =
          (LoadGuestFloat(base, manager + 104) + LoadGuestFloat(base, manager + 112) * .5f) /
          batch.x_scale();
      if (box.x + box.w * .5f < center)
        box.x -= width - box.w;
      box.w = width;
    }
    DrawPrompt(batch, box, prompt);
  }
  ctx = saved;
}

// Direct HUD text uses the same draw function without an APT text block.
// Scope its owner before measurement so key widths and drawn icons agree.
REX_FUNC(sub_826C00D8) {
  const uint32_t previous = g_text_player;
  const bool previous_valid = g_text_player_valid;
  const auto previous_button = g_standalone_button;
  if (!g_standalone_button && g_active && ctx.r7.u32) {
    const auto* text = reinterpret_cast<const char*>(GuestToHost(base, ctx.r7.u32));
    const size_t length = std::strlen(text);
    if (length > 2 && length < 32 && text[0] == '[' && text[length - 1] == ']')
      g_standalone_button = ParseButton(std::string(text + 1, length - 2));
  }
  if (!previous_valid && g_active && g_input && REXCVAR_GET(native_button_prompts) && ctx.r5.u32) {
    const uint32_t manager = ctx.r3.u32, rect = ctx.r5.u32;
    Point anchor{LoadGuestFloat(base, rect), LoadGuestFloat(base, rect + 4)};
    if (ctx.r8.u32 == 2)
      anchor.x += LoadGuestFloat(base, rect + 8) - LoadGuestFloat(base, manager + 80);
    else if (ctx.r8.u32 == 1)
      anchor.x += LoadGuestFloat(base, rect + 8) * .5f;
    g_text_player = PlayerAt(base, manager, anchor);
    g_text_player_valid = true;
  }
  __imp__sub_826C00D8(ctx, base);
  g_text_player = previous;
  g_text_player_valid = previous_valid;
  g_standalone_button = previous_button;
}

// HUD text is drawn in separate player passes. Its coordinates can be local
// to a half-size target, so its screen position cannot identify the player.
// Use the same split-pass context as the game's own text placement code.
REX_FUNC(sub_82734200) {
  const auto previous = g_text_player;
  const bool previous_valid = g_text_player_valid;
  const auto previous_pass = g_hud_pass_player;
  if (g_active && game_ui::LevelRunning(LoadGuestU32(base, kFrontendState)) &&
      LoadGuestU32(base, kClockPaused) != 1) {
    const auto render = LoadGuestU32(base, kHudRenderContext);
    const auto split = render ? LoadGuestU32(base, render + 112) : 0;
    const auto pass = render ? LoadGuestU32(base, render + 116) : 0;
    // HUD drawn into the full guest image already includes its final position.
    // Only separate split passes need an explicit owner; otherwise let the
    // font quad's position select its viewport instead of pinning it to P1.
    g_text_player_valid = split && pass >= 1 && pass <= 2;
    g_hud_pass_player.reset();
    if (g_text_player_valid) {
      g_text_player = pass - 1;
      g_hud_pass_player = pass - 1;
    }
  }
  __imp__sub_82734200(ctx, base);
  g_text_player = previous;
  g_text_player_valid = previous_valid;
  g_hud_pass_player = previous_pass;
}
