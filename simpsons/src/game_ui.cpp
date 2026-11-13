// Screens drawn inside the game's frame (game_ui.h).
//
// The game draws two 2D passes every frame, each with its camera, render
// states and fonts set up for text: the HUD (sub_826D5050, which ends with
// sub_826D4840) and then the front end's APT render (sub_827BCAB8, in
// sub_827F58D8). In play only the HUD's reaches the screen; with a menu up
// (the front end, the pause menu, the notepad and the bus stop: 0x82D572DC is
// 1 then) only the front end's does. The screens are drawn at the end of the
// one that shows. The game reads the controllers (XInputGetState), updates
// and draws on one thread, so screen actions can change the game directly.

#include "game_ui.h"
#include "cutscene_skip.h"
#include "debug_menu.h"
#include "freecam.h"
#include "game_canvas.h"
#include "game_menu.h"
#include "game_menu_apt.h"
#include "game_menu_pointer.h"
#include "game_thread.h"
#include "guest_memory.h"
#include "pause_menu.h"
#include "players.h"
#include "spawner.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cmath>
#include <deque>
#include <memory>
#include <mutex>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/runtime.h>
#include <rex/ui/guest_output_viewport.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/overlay_input.h>
#include <rex/ui/ui_event.h>
#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>

REXCVAR_DEFINE_BOOL(game_ui_sounds, false, "Game",
                    "Play the front end's sounds in the debug menu and the achievements");
REXCVAR_DEFINE_BOOL(game_ui_log_input, false, "Game",
                    "Log the controller presses the debug menu and the achievements read "
                    "(for development)");
REXCVAR_DEFINE_STRING(game_ui_test_file, "", "Game",
                      "Development: clicks and taps read from this file as 'click X Y', "
                      "'move X Y' or 'tap X Y' in game image coordinates (0 to 1), or "
                      "'key VKEY' and 'char CODEPOINT' through the screens' input listener");

REX_EXTERN(__imp__sub_827BCAB8);
REX_EXTERN(sub_827BCAB8);
REX_EXTERN(__imp__sub_826D4840);
REX_EXTERN(sub_826D4840);

namespace game_ui {
namespace {

using simpsons::LoadGuestU32;
using Clock = std::chrono::steady_clock;

constexpr uint32_t kFrontendState = 0x82D08C98;  // 7 or 9 in play (LevelRunning)
constexpr uint16_t kUp = 0x0001, kDown = 0x0002, kLeft = 0x0004, kRight = 0x0008;
constexpr uint16_t kStart = 0x0010, kBack = 0x0020, kLB = 0x0100, kRB = 0x0200;
constexpr uint16_t kA = 0x1000, kB = 0x2000, kX = 0x4000, kY = 0x8000;
// What a screen keeps from the game; the debug menu leaves the sticks,
// triggers, stick clicks, Start and Back to it.
constexpr uint16_t kMenuButtons = kUp | kDown | kLeft | kRight | kLB | kRB | kA | kB | kX | kY;
constexpr float kRepeatDelay = 0.36f, kRepeatRate = 0.07f;

struct Pad {
  uint16_t buttons = 0;
  uint8_t lt = 0, rt = 0;
  int16_t lx = 0, ly = 0, rx = 0, ry = 0;
};

// Window events, queued by the UI thread for the game's.
struct Event {
  enum Kind { kKey, kPress, kRelease, kMove, kWheel, kChar } kind;
  rex::ui::VirtualKey key = rex::ui::VirtualKey::kNone;
  float x = 0, y = 0;  // the game image, 0 to 1
  float wheel = 0;
  bool touch = false;
  uint32_t character = 0;  // kChar: a Unicode character typed
};

rex::Runtime* g_runtime = nullptr;
rex::ui::Window* g_window = nullptr;
std::atomic<Screen> g_screen{Screen::kNone};
std::atomic<bool> g_alive{false};

std::mutex g_event_mutex;
std::deque<Event> g_events;

// Game thread.
std::array<Pad, 4> g_pads;
std::array<bool, 4> g_hold{};  // keep the game off the buttons until they are let go
std::array<int, 4> g_inject{};  // polls left of a B that leaves the pause menu
std::atomic<bool> g_resume{false};
std::atomic<uint32_t> g_pause_owner{0};
Screen g_after_resume = Screen::kNone;
Screen g_shown = Screen::kNone;
uint16_t g_held = 0;
std::array<float, 4> g_repeat{};
Clock::time_point g_last_frame{};
uint32_t g_frame = 0, g_drawn_frame = ~0u;  // frames started, and the last one drawn
bool g_pointer_mode = false;
enum class Controls { kController, kKeyboard, kTouch };
Controls g_controls = Controls::kController;
bool g_overlay_input = false;
float g_hover_x = -1, g_hover_y = -1;
bool g_touching = false;
float g_touch_x = 0, g_touch_y = 0, g_touch_start_y = 0, g_touch_moved = 0;

std::mutex g_notify_mutex;
std::string g_notify;
float g_notify_time = 0;

std::mutex g_sound_mutex;
std::vector<std::string> g_sounds;
std::unique_ptr<simpsons::GameMenuApt> g_apt;
std::atomic<bool> g_take_text{false};
std::atomic<bool> g_edit_text{false};

// Presses made for the game (PressForGame): each held for a few polls of the
// controller and a moment, then let go as long.
std::mutex g_press_mutex;
std::deque<uint16_t> g_presses;
uint16_t g_pressing = 0;
int g_press_phase = 0, g_press_polls = 0;  // phase 1 held, 2 let go
Clock::time_point g_press_since{};
uint32_t g_press_packet = 0;
std::atomic<bool> g_block_game{false};

// The buttons the game gets from its first controller while presses are made.
uint16_t NextPress() {
  std::lock_guard lock(g_press_mutex);
  const auto now = Clock::now();
  if (g_press_phase == 0 && !g_presses.empty()) {
    g_pressing = g_presses.front();
    g_presses.pop_front();
    g_press_phase = 1;
    g_press_polls = 0;
    g_press_since = now;
  }
  if (g_press_phase == 0)
    return 0;
  const bool long_enough =
      ++g_press_polls >= 3 && now - g_press_since >= std::chrono::milliseconds(90);
  const uint16_t buttons = g_press_phase == 1 ? g_pressing : 0;
  if (long_enough) {
    g_press_phase = g_press_phase == 1 ? 2 : 0;
    g_press_polls = 0;
    g_press_since = now;
  }
  return buttons;
}

// Keys, the mouse and touch while a screen is open. Ahead of the game's menu
// pointer (2050) and its menus.
class Listener final : public rex::ui::WindowInputListener {
 public:
  void OnKeyDown(rex::ui::KeyEvent& e) override {
    if (!IsOpen() || e.is_handled())
      return;
    // Menu input stops here before the system binds run. Keep the menu's
    // own shortcut available so the same key can close it again.
    if (e.virtual_key() == rex::ui::ParseVirtualKey(rex::cvar::GetFlagByName("bind_debug_menu"))) {
      if (!e.prev_state())
        Toggle(Screen::kDebugMenu);
      e.set_handled(true);
      return;
    }
    Push({Event::kKey, e.virtual_key()});
    e.set_handled(true);
  }
  void OnKeyUp(rex::ui::KeyEvent& e) override {
    if (IsOpen())
      e.set_handled(true);
  }
  void OnKeyChar(rex::ui::KeyEvent& e) override {
    if (!IsOpen() || e.is_handled())
      return;
    const uint32_t character = uint32_t(e.virtual_key());
    if (g_take_text.load() && character >= 32 && character != 127 && !e.is_ctrl_pressed()) {
      Event event{Event::kChar};
      event.character = character;
      Push(event);
    }
    e.set_handled(true);
  }
  void OnMouseDown(rex::ui::MouseEvent& e) override { Pointer(e, Event::kPress); }
  void OnMouseUp(rex::ui::MouseEvent& e) override { Pointer(e, Event::kRelease); }
  void OnMouseMove(rex::ui::MouseEvent& e) override {
    if (!IsOpen() || Current() == Screen::kSaves || e.is_handled())
      return;
    float x, y;
    if (ToGame(float(e.x()), float(e.y()), &x, &y))
      Push({Event::kMove, {}, x, y});
  }
  void OnMouseWheel(rex::ui::MouseEvent& e) override {
    if (!IsOpen() || e.is_handled() || !e.scroll_y())
      return;
    Event event{Event::kWheel};
    event.wheel = float(e.scroll_y()) / float(rex::ui::MouseEvent::kScrollPerDetent);
    Push(event);
    e.set_handled(true);
  }
  void OnTouchEvent(rex::ui::TouchEvent& e) override {
    if (e.is_handled() || !IsOpen() || Current() == Screen::kSaves)
      return;
    float x, y;
    if (!ToGame(e.x(), e.y(), &x, &y))
      return;
    using A = rex::ui::TouchEvent::Action;
    Event event{e.action() == A::kDown   ? Event::kPress
                : e.action() == A::kMove ? Event::kMove
                                         : Event::kRelease,
                {}, x, y, 0, true};
    if (e.action() == A::kCancel)
      event.x = event.y = -1;
    Push(event);
    e.set_handled(true);
  }

 private:
  void Pointer(rex::ui::MouseEvent& e, Event::Kind kind) {
    if (Current() == Screen::kSaves)
      return;
    if (e.is_handled() || e.button() != rex::ui::MouseEvent::Button::kLeft) {
      if (IsOpen() && e.button() == rex::ui::MouseEvent::Button::kRight && kind == Event::kPress) {
        Push({Event::kKey, rex::ui::VirtualKey::kEscape});
        e.set_handled(true);
      }
      return;
    }
    float x, y;
    if (!IsOpen() || !ToGame(float(e.x()), float(e.y()), &x, &y))
      return;
    Push({kind, {}, x, y});
    e.set_handled(true);
  }
  static bool ToGame(float wx, float wy, float* x, float* y) {
    if (!g_window)
      return false;
    const auto viewport = rex::ui::GetGuestOutputViewport();
    const float width = float(g_window->GetActualPhysicalWidth());
    const float height = float(g_window->GetActualPhysicalHeight());
    if (!(width > 0 && height > 0 && viewport.width > 0 && viewport.height > 0))
      return false;
    *x = (wx / width - viewport.x) / viewport.width;
    *y = (wy / height - viewport.y) / viewport.height;
    return true;
  }
  static void Push(Event event) {
    std::lock_guard lock(g_event_mutex);
    if (g_events.size() < 256)
      g_events.push_back(event);
  }
};
Listener g_listener;

void SetOverlayInput(bool on) {
  if (on == g_overlay_input)
    return;
  g_overlay_input = on;
  if (on)
    rex::ui::AcquireOverlayInput();
  else
    rex::ui::ReleaseOverlayInput();
}

// The fonts: titles in the largest of the game's proportional fonts, the rest
// in the one closest to body text's size.
void PickFonts(canvas::Canvas& c, uint32_t* title, uint32_t* body, const uint8_t* base) {
  uint32_t best_title = 0, best_body = 0;
  float title_line = 0, body_distance = 1e9f;
  static bool logged = false;
  for (const uint32_t font : c.Fonts()) {
    c.UseFont(font);
    const float line = float(LoadGuestU32(base, font + 24));
    const bool proportional = c.Measure(line, "i") < c.Measure(line, "W") * 0.6f;
    if (!logged)
      REXLOG_INFO("[game-ui] font {:08X}: line {}, {}", font, line,
                  proportional ? "proportional" : "fixed");
    if (!proportional)
      continue;
    if (line > title_line) {
      title_line = line;
      best_title = font;
    }
    if (std::abs(line - 40) < body_distance) {
      body_distance = std::abs(line - 40);
      best_body = font;
    }
  }
  logged = true;
  *title = best_title ? best_title : c.font();
  *body = best_body ? best_body : *title;
}

void DrawNotify(Frame& f) {
  std::string text;
  float t;
  {
    std::lock_guard lock(g_notify_mutex);
    if (g_notify.empty())
      return;
    g_notify_time += f.dt;
    if (g_notify_time > 3.2f) {
      g_notify.clear();
      return;
    }
    text = g_notify;
    t = g_notify_time;
  }
  const float alpha = std::clamp(std::min(t * 6.0f, (3.2f - t) * 3.0f), 0.0f, 1.0f);
  const auto a = [alpha](uint32_t color) {
    return (uint32_t(float(color >> 24) * alpha) << 24) | (color & 0xFFFFFF);
  };
  // As the game's TV menus draw their messages: a dark band between light
  // blue wavy lines, white text with a dark edge.
  auto& c = f.c;
  const float s = f.s, size = 28 * s;
  c.UseFont(f.title_font);
  const bool in_saves = Current() == Screen::kSaves;
  const auto shown = c.Fit(size, text, in_saves ? 760 * s : c.width() - 120 * s);
  const float width = c.Measure(size, shown) + 80 * s, height = 52 * s;
  const float x = (c.width() - width) * 0.5f;
  const float y = in_saves ? 8 * s : c.height() * 0.09f;
  c.Rect(x, y, width, height, a(0xD80E0A30));
  for (const float line : {y, y + height}) {
    float px = x, py = line;
    const int steps = std::max(8, int(width / (12 * s)));
    for (int i = 1; i <= steps; ++i) {
      const float t = float(i) / steps;
      const float wx = x + width * t, wy = line + std::sin(t * 6.2831853f * 3.5f) * 1.5f * s;
      c.Line(px, py, wx, wy, 2.5f * s, a(0xFFA8DCFF));
      px = wx;
      py = wy;
    }
  }
  const float o = std::max(1.5f, size * 0.07f);
  const float tx = c.width() * 0.5f, ty = y + (height - size) * 0.5f;
  for (int i = 0; i < 12; ++i) {
    const float angle = i * 6.2831853f / 12;
    c.Text(tx + std::cos(angle) * o, ty + std::sin(angle) * o, size, a(0xFF15103A), shown,
           canvas::Align::kCenter);
  }
  c.Text(tx, ty, size, a(0xFFFFFFFF), shown, canvas::Align::kCenter);
}

void DrawNpcAvatarHud(Frame& f) {
  const auto avatar = players::ActiveNpcAvatar();
  if (!avatar.active) return;
  // Keep the real family portrait and dynamic ability prompts. A generic
  // silhouette and hardcoded B badge cannot represent this NPC's abilities.
  auto& c = f.c;
  const float s = f.s;
  c.UseFont(f.body_font);
  const auto label = c.Fit(18.0f * s, "Costume preview: " + avatar.name, 420.0f * s);
  c.RoundRect(24.0f * s, 88.0f * s, c.Measure(18.0f * s, label) + 24.0f * s,
              32.0f * s, 8.0f * s, 0xD8101828);
  c.Text(36.0f * s, 93.0f * s, 18.0f * s, 0xFFFFD90F, label);
}

void DrawSpoonerHud(Frame& f) {
  if (!spawner::SpoonerEnabled())
    return;
  auto& c = f.c;
  const float s = f.s;
  const float cx = c.width() * 0.5f;
  const float cy = c.height() * 0.5f;
  const bool grabbing = spawner::SpoonerGrabbing();
  const auto selected = spawner::SpoonerSelectedInfo(f.ctx, f.base);
  const auto hovered = spawner::SpoonerHoveredInfo(f.ctx, f.base);
  const uint32_t ret_color = grabbing  ? 0xFFFFD90F
                             : hovered ? 0xFF6ED65C
                                       : 0xE0FFFFFF;

  // Crosshair reticle in the center of the screen.
  c.Circle(cx, cy, 12.0f * s, 0x55000000);
  c.Circle(cx, cy, 3.5f * s, ret_color);
  const float gap = 7.0f * s, arm = 16.0f * s;
  c.Line(cx - arm, cy, cx - gap, cy, 2.5f * s, ret_color);
  c.Line(cx + gap, cy, cx + arm, cy, 2.5f * s, ret_color);
  c.Line(cx, cy - arm, cx, cy - gap, 2.5f * s, ret_color);
  c.Line(cx, cy + gap, cx, cy + arm, 2.5f * s, ret_color);

  // Top status banner when Debug Menu isn't covering it.
  if (Current() == Screen::kNone) {
    const float w = 780.0f * s;
    const float h = 64.0f * s;
    const float x = (c.width() - w) * 0.5f;
    const float y = 18.0f * s;
    c.RoundRect(x, y, w, h, 8.0f * s, 0xE60A1020);
    c.Rect(x, y, w, 3.0f * s, ret_color);

    std::string top_line = "SPOONER MODE  |  Aim at any map object or NPC";
    const auto& active = grabbing && selected ? selected : (hovered ? hovered : selected);
    if (active) {
      char buf[256];
      std::snprintf(buf, sizeof(buf), "SPOONER %s: %s (%s)  [%.1fm]",
                    grabbing ? "GRABBING" : (hovered ? "TARGET" : "SELECTED"),
                    active->name.c_str(),
                    !active->category.empty() ? active->category.c_str() : active->class_name.c_str(),
                    active->distance_to_camera);
      top_line = buf;
    }
    c.UseFont(f.title_font);
    c.Text(x + 16.0f * s, y + 8.0f * s, 21.0f * s, ret_color, c.Fit(21.0f * s, top_line, w - 32.0f * s));
    c.UseFont(f.body_font);
    c.Text(x + 16.0f * s, y + 35.0f * s, 17.5f * s, 0xFFDDE6F5,
           "[F / Pad A] Grab/Drop   [C / Pad Y] Clone   [Del / Pad X] Delete   [Z/X] Rotate   [R/T] Distance   [F2] Menu");
  }
}

void OpenFromGameThread(Screen screen) {
  if (screen == Screen::kNone)
    return;
  if (screen == Screen::kDebugMenu && PauseMenuShowing()) {
    // The debug menu works over the game: leave the pause menu first.
    g_resume = true;
    g_after_resume = screen;
    return;
  }
  g_screen = screen;
}

// This frame's input for the open screen, from the controllers and the
// window's events.
Input Gather(float dt, float width, float height) {
  Input in;
  Pad pad;
  for (const Pad& user : g_pads) {
    pad.buttons |= user.buttons;
    pad.lt = std::max(pad.lt, user.lt);
    pad.rt = std::max(pad.rt, user.rt);
    const auto stronger = [](int16_t a, int16_t b) {
      return std::abs(int(b)) > std::abs(int(a)) ? b : a;
    };
    pad.lx = stronger(pad.lx, user.lx);
    pad.ly = stronger(pad.ly, user.ly);
    pad.rx = stronger(pad.rx, user.rx);
    pad.ry = stronger(pad.ry, user.ry);
  }
  uint16_t buttons = pad.buttons;
  // The left stick moves through the saves and achievements; in the debug
  // menu it moves the family.
  if (g_screen.load() == Screen::kAchievements || g_screen.load() == Screen::kSaves) {
    if (pad.ly > 16000)
      buttons |= kUp;
    if (pad.ly < -16000)
      buttons |= kDown;
    if (pad.lx < -16000)
      buttons |= kLeft;
    if (pad.lx > 16000)
      buttons |= kRight;
  }
  const uint16_t pressed = buttons & ~g_held;
  g_held = buttons;
  if (pressed || std::abs(int(pad.lx)) > 16000 || std::abs(int(pad.ly)) > 16000 ||
      std::abs(int(pad.rx)) > 16000 || std::abs(int(pad.ry)) > 16000 || pad.lt > 64 ||
      pad.rt > 64) {
    g_pointer_mode = false;
    g_controls = Controls::kController;
    if (Current() == Screen::kSaves)
      GameMenuPointerInputSource(0);
  }
  if (pressed && REXCVAR_GET(game_ui_log_input))
    REXLOG_INFO("[game-ui] frame {} buttons {:04X} pressed {:04X}", g_frame, buttons, pressed);
  const auto repeat = [&](uint16_t mask, float& timer) {
    if (pressed & mask) {
      timer = kRepeatDelay;
      return true;
    }
    if (!(buttons & mask))
      return false;
    timer -= dt;
    if (timer > 0)
      return false;
    timer += kRepeatRate;
    return true;
  };
  in.up = repeat(kUp, g_repeat[0]);
  in.down = repeat(kDown, g_repeat[1]);
  in.left = repeat(kLeft, g_repeat[2]);
  in.right = repeat(kRight, g_repeat[3]);
  in.accept = pressed & kA;
  in.back = pressed & kB;
  in.previous = pressed & kLB;
  in.next = pressed & kRB;
  in.x = pressed & kX;
  in.y = pressed & kY;
  in.start = pressed & kStart;
  const float stick = -float(pad.ry) / 32767.0f;
  if (std::abs(stick) > 0.25f)
    in.scroll += stick * dt * 12.0f;
  if (g_screen.load() == Screen::kAchievements)
    in.scroll += (float(pad.rt) - float(pad.lt)) / 255.0f * dt * 12.0f;

  std::deque<Event> events;
  {
    std::lock_guard lock(g_event_mutex);
    events.swap(g_events);
  }
  using K = rex::ui::VirtualKey;
  const bool typing = g_take_text.load();
  for (const Event& e : events) {
    switch (e.kind) {
      case Event::kChar: {
        g_pointer_mode = false;
        g_controls = Controls::kKeyboard;
        if (Current() == Screen::kSaves)
          GameMenuPointerInputSource(1);
        const uint32_t c = e.character;
        if (c < 0x80) {
          in.text += char(c);
        } else if (c < 0x800) {
          in.text += char(0xC0 | c >> 6);
          in.text += char(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
          in.text += char(0xE0 | c >> 12);
          in.text += char(0x80 | (c >> 6 & 0x3F));
          in.text += char(0x80 | (c & 0x3F));
        }
        break;
      }
      case Event::kKey:
        g_pointer_mode = false;
        g_controls = Controls::kKeyboard;
        if (Current() == Screen::kSaves)
          GameMenuPointerInputSource(1);
        if (typing) {
          // Letters, digits and space come as characters.
          switch (e.key) {
            case K::kUp:
              in.up = true;
              break;
            case K::kDown:
              in.down = true;
              break;
            case K::kLeft:
              in.left = true;
              break;
            case K::kRight:
              in.right = true;
              break;
            case K::kReturn:
              in.enter = true;
              break;
            case K::kEscape:
              in.back = true;
              break;
            case K::kBack:
              in.erase = true;
              break;
            case K::kDelete:
              if (g_edit_text.load())
                in.erase = true;
              else if (Current() == Screen::kSaves)
                in.remove = true;
              else
                in.y = true;
              break;
            case K::kPrior:
              in.previous = true;
              break;
            case K::kShift:
              in.previous = g_edit_text.load();
              break;
            case K::kNext:
              in.next = true;
              break;
            case K::kF3:
              in.find = true;
              break;
            case K::kF2:
              in.x = true;
              break;
            default:
              break;
          }
          break;
        }
        switch (e.key) {
          case K::kUp:
            in.up = true;
            break;
          case K::kDown:
            in.down = true;
            break;
          case K::kLeft:
            in.left = true;
            break;
          case K::kRight:
            in.right = true;
            break;
          case K::kReturn:
          case K::kSpace:
            in.accept = true;
            break;
          case K::kEscape:
          case K::kBack:
            in.back = true;
            break;
          case K::kPrior:
          case K::kQ:
            in.previous = true;
            break;
          case K::kNext:
          case K::kE:
          case K::kTab:
            in.next = true;
            break;
          case K::kF3:
            in.find = true;
            break;
          case K::kF2:
            in.x = true;
            break;
          case K::kDelete:
            if (Current() == Screen::kSaves)
              in.remove = true;
            else
              in.y = true;
            break;
          default:
            break;
        }
        break;
      case Event::kMove:
        g_pointer_mode = true;
        g_controls = e.touch ? Controls::kTouch : Controls::kKeyboard;
        if (e.touch && g_touching) {
          const float dy = (e.y - g_touch_y) * height;
          g_touch_moved += std::abs(dy) + std::abs((e.x - g_touch_x) * width);
          in.scroll -= dy / (36.0f * height / 720.0f);
          g_touch_x = e.x;
          g_touch_y = e.y;
        } else if (!e.touch) {
          g_hover_x = e.x * width;
          g_hover_y = e.y * height;
          in.hover_moved = true;
        }
        break;
      case Event::kPress:
        g_pointer_mode = true;
        g_controls = e.touch ? Controls::kTouch : Controls::kKeyboard;
        if (e.touch) {
          g_touching = true;
          g_touch_x = e.x;
          g_touch_y = g_touch_start_y = e.y;
          g_touch_moved = 0;
        }
        break;
      case Event::kRelease:
        g_pointer_mode = true;
        g_controls = e.touch ? Controls::kTouch : Controls::kKeyboard;
        if (e.touch) {
          const bool tap = g_touching && g_touch_moved < 24.0f * height / 720.0f && e.x >= 0;
          g_touching = false;
          if (!tap)
            break;
        }
        in.tapped = true;
        in.tap_x = e.x * width;
        in.tap_y = e.y * height;
        break;
      case Event::kWheel:
        g_pointer_mode = true;
        g_controls = Controls::kKeyboard;
        in.scroll -= e.wheel * 3;
        break;
    }
  }
  in.pointer = g_pointer_mode;
  in.touch = g_controls == Controls::kTouch;
  in.hovering = g_hover_x >= 0 && !g_touching;
  in.hover_x = g_hover_x;
  in.hover_y = g_hover_y;
  return in;
}

// Whether the front end's pass is the one that shows.
bool FrontEndShows(const uint8_t* base) {
  return !LevelRunning(LoadGuestU32(base, kFrontendState)) || LoadGuestU32(base, 0x82D572DC) == 1;
}

void FrameScreens(PPCContext& ctx, uint8_t* base) {
  if (g_drawn_frame == g_frame)
    return;
  g_drawn_frame = g_frame;
  const auto now = Clock::now();
  const float dt =
      g_last_frame == Clock::time_point{}
          ? 1.0f / 60
          : std::clamp(std::chrono::duration<float>(now - g_last_frame).count(), 0.0f, 0.1f);
  g_last_frame = now;
  bool notifying;
  {
    std::lock_guard lock(g_notify_mutex);
    notifying = !g_notify.empty();
  }
  const Screen screen = g_screen.load();
  const bool npc_hud = !FrontEndShows(base) && !PhotoModeActive() && !debug_menu::HideHud() &&
                       players::ActiveNpcAvatar().active;
  const bool spooner_hud = !FrontEndShows(base) && !PhotoModeActive() && spawner::SpoonerEnabled();
  SetOverlayInput(screen != Screen::kNone && screen != Screen::kSaves);
  if (screen == Screen::kSaves && !SavesCovering())
    return;
  if (screen == Screen::kNone && !notifying && !SavesCovering() && !npc_hud && !spooner_hud) {
    g_shown = Screen::kNone;
    std::lock_guard lock(g_event_mutex);
    g_events.clear();
    return;
  }
  const PPCContext saved = ctx;
  {
    canvas::Canvas c(ctx, base);
    if (!c.valid()) {
      ctx = saved;
      return;
    }
    uint32_t title_font = 0, body_font = 0;
    PickFonts(c, &title_font, &body_font, base);
    if (screen != g_shown) {
      // Buttons already down as a screen opens (the press that opened it)
      // aren't presses on it.
      uint16_t held = 0;
      for (const Pad& pad : g_pads)
        held |= pad.buttons;
      g_held = held;
    }
    const Input input = Gather(dt, c.width(), c.height());
    Frame f{ctx, base, c, input, c.height() / 720.0f, dt, title_font, body_font};
    if (npc_hud)
      DrawNpcAvatarHud(f);
    if (spooner_hud)
      DrawSpoonerHud(f);
    if (screen != Screen::kNone) {
      const bool appearing = screen != g_shown;
      g_shown = screen;
      bool keep = true;
      if (screen == Screen::kDebugMenu)
        keep = DrawDebugMenu(f, appearing);
      else if (screen == Screen::kAchievements)
        keep = DrawAchievements(f, appearing);
      if (!keep) {
        Screen expected = screen;
        g_screen.compare_exchange_strong(expected, Screen::kNone);
        Sound("ui_back_b_button");
        for (bool& hold : g_hold)
          hold = true;
      }
    } else {
      g_shown = Screen::kNone;
    }
    DrawSavesCover(f);
    DrawNotify(f);
  }
  ctx = saved;
}

}  // namespace

void Init(rex::Runtime* runtime, rex::ui::Window* window) {
  g_runtime = runtime;
  g_window = window;
  g_apt = std::make_unique<simpsons::GameMenuApt>(runtime);
  InitSaves(runtime);
  g_alive = true;
  if (window)
    window->AddInputListener(&g_listener, 2200);
  rex::ui::RegisterBind("bind_debug_menu", "F2", "Open the debug menu",
                        [] { Toggle(Screen::kDebugMenu); });
  rex::ui::RegisterBind("bind_achievements", "F5", "Show the achievements",
                        [] { Toggle(Screen::kAchievements); });
}

void Shutdown() {
  if (!g_alive.exchange(false))
    return;
  rex::ui::UnregisterBind("bind_debug_menu");
  rex::ui::UnregisterBind("bind_achievements");
  if (g_window)
    g_window->RemoveInputListener(&g_listener);
  g_screen = Screen::kNone;
  SetOverlayInput(false);
}

void Open(Screen screen) {
  if (screen == Screen::kAchievements && !InGame()) {
    Notify("Load a save to see your achievements");
    return;
  }
  REXLOG_INFO("[game-ui] open {}", screen == Screen::kAchievements ? "achievements"
                                   : screen == Screen::kSaves      ? "saves"
                                                                   : "debug menu");
  if (screen == Screen::kDebugMenu && PauseMenuShowing()) {
    g_after_resume = screen;
    g_resume = true;
    return;
  }
  if (screen == Screen::kSaves) {
    g_held = 0;
    for (const Pad& pad : g_pads)
      g_held |= pad.buttons;
    TakeText(false);
    SetOverlayInput(false);
  }
  g_screen = screen;
}

void Toggle(Screen screen) {
  Screen expected = screen;
  if (!g_screen.compare_exchange_strong(expected, Screen::kNone))
    Open(screen);
}

void Close() {
  g_screen = Screen::kNone;
}

Input ReadSavesInput() {
  static auto last = Clock::now();
  const auto now = Clock::now();
  const float dt = std::clamp(std::chrono::duration<float>(now - last).count(), 0.0f, .1f);
  last = now;
  return Gather(dt, 1280, 720);
}

void TakeText(bool on, bool editing) {
  g_edit_text = on && editing;
  g_take_text = on;
}

void PressForGame(uint16_t button) {
  std::lock_guard lock(g_press_mutex);
  if (g_presses.size() < 64)
    g_presses.push_back(button);
}

bool PressingForGame() {
  std::lock_guard lock(g_press_mutex);
  return g_press_phase != 0 || !g_presses.empty();
}

void CancelPressesForGame() {
  std::lock_guard lock(g_press_mutex);
  g_presses.clear();
  g_press_phase = 0;
}

void BlockGameInput(bool on) {
  g_block_game = on;
}

bool InjectedButtons(uint32_t user, uint16_t* buttons) {
  if (user != 0 || !PressingForGame())
    return false;
  *buttons = NextPress();
  return true;
}

Screen Current() {
  return g_screen.load();
}

bool IsOpen() {
  return g_screen.load() != Screen::kNone;
}

bool InGame() {
  if (!g_runtime)
    return false;
  const auto* base = g_runtime->virtual_membase();
  return base && LevelRunning(LoadGuestU32(base, kFrontendState));
}

void Notify(std::string text) {
  std::lock_guard lock(g_notify_mutex);
  g_notify = std::move(text);
  g_notify_time = 0;
}

void Sound(const char* name) {
  std::lock_guard lock(g_sound_mutex);
  if (g_sounds.size() < 8)
    g_sounds.push_back(name);
}

// The game thread at the start of its frame (game_thread.cpp): the front
// end's sound objects play from there, outside the APT render.
void Tick(PPCContext& ctx, uint8_t* base) {
  ++g_frame;
  if (const auto& path = REXCVAR_GET(game_ui_test_file); !path.empty()) {
    if (FILE* file = std::fopen(path.c_str(), "r")) {
      char op[16] = {};
      float x = 0, y = 0;
      const int fields = std::fscanf(file, "%15s %f %f", op, &x, &y);
      std::fclose(file);
      std::remove(path.c_str());
      if (fields >= 2 && g_window && std::isfinite(x) && x >= 0 && x <= 65535 &&
          x == std::floor(x) && (std::string_view(op) == "key" || std::string_view(op) == "char")) {
        rex::ui::KeyEvent event(g_window, static_cast<rex::ui::VirtualKey>(uint32_t(x)), 1, false,
                                false, false, false, false);
        if (std::string_view(op) == "key") {
          g_listener.OnKeyDown(event);
          g_listener.OnKeyUp(event);
        } else {
          g_listener.OnKeyChar(event);
        }
        REXLOG_INFO("[game-ui] test {} {}: {}", op, uint32_t(x),
                    event.is_handled() ? "handled" : "ignored");
      } else if (fields == 3 && std::string_view(op) == "tap" && g_window) {
        // A finger: touch events through the listener, in window pixels.
        REXLOG_INFO("[game-ui] test tap at {}, {}", x, y);
        using A = rex::ui::TouchEvent::Action;
        const auto viewport = rex::ui::GetGuestOutputViewport();
        const float wx = (viewport.x + x * viewport.width) * g_window->GetActualPhysicalWidth();
        const float wy = (viewport.y + y * viewport.height) * g_window->GetActualPhysicalHeight();
        rex::ui::TouchEvent down(g_window, 9, A::kDown, wx, wy), up(g_window, 9, A::kUp, wx, wy);
        g_listener.OnTouchEvent(down);
        g_listener.OnTouchEvent(up);
      } else if (fields == 3) {
        REXLOG_INFO("[game-ui] test {} at {}, {}", op, x, y);
        std::lock_guard lock(g_event_mutex);
        Event move{Event::kMove, {}, x, y};
        g_events.push_back(move);
        if (std::string_view(op) == "click")
          g_events.push_back({Event::kRelease, {}, x, y});
      }
    }
  }
  std::vector<std::string> sounds;
  {
    std::lock_guard lock(g_sound_mutex);
    sounds.swap(g_sounds);
  }
  if (sounds.empty() || !g_apt || !g_apt->ready() || !REXCVAR_GET(game_ui_sounds))
    return;
  g_apt->Bind(ctx, base, "_level0");
  for (const auto& sound : sounds) {
    // Only sounds the loaded movies have: calling into a missing object can
    // hang the interpreter.
    const std::string object = "_level0." + sound;
    const uint32_t value = g_apt->Get(object);
    static int logged = 0;
    if (logged++ < 8)
      REXLOG_INFO("[game-ui] sound {}: {}", sound, value ? "found" : "missing");
    if (value)
      g_apt->Invoke(object + ".start");
  }
}

bool FilterController(uint32_t user, uint16_t buttons, uint8_t left_trigger,
                      uint8_t right_trigger, int16_t lx, int16_t ly, int16_t rx, int16_t ry,
                      uint16_t* game_buttons, bool* keep_sticks) {
  *game_buttons = 0;
  *keep_sticks = false;
  if (user >= g_pads.size())
    return IsOpen() || g_block_game.load() || PressingForGame();
  const uint16_t pressed = buttons & ~g_pads[user].buttons;
  g_pads[user] = {buttons, left_trigger, right_trigger, lx, ly, rx, ry};
  // Presses made for the game, on its first controller; nothing else reaches
  // the game meanwhile.
  if (PressingForGame() || g_block_game.load()) {
    if (user == 0)
      *game_buttons = NextPress();
    g_hold[user] = true;
    return true;
  }
  if ((pressed & kStart) && !PauseMenuShowing() && !IsOpen())
    g_pause_owner = user;
  // Leaving the pause menu: B down for a few polls, then up, on the
  // controller that paused.
  if (g_inject[user] > 0) {
    *game_buttons = g_inject[user] > 3 ? kB : 0;
    if (--g_inject[user] == 0 && g_after_resume != Screen::kNone) {
      g_screen = g_after_resume;
      g_after_resume = Screen::kNone;
    }
    return true;
  }
  if (g_resume.load() && !IsOpen() && PauseMenuShowing() && user == g_pause_owner.load()) {
    g_resume = false;
    g_inject[user] = 6;
    *game_buttons = kB;
    return true;
  }
  if (g_resume.load() && !PauseMenuShowing()) {
    g_resume = false;
    if (g_after_resume != Screen::kNone) {
      g_screen = g_after_resume;
      g_after_resume = Screen::kNone;
    }
  }
  const Screen screen = g_screen.load();
  if (screen == Screen::kNone) {
    if (PauseMenuShowing() && !GameMenuIsVisible() && InGame() && (pressed & (kY | kX))) {
      // In the pause menu, Y opens the achievements and X the debug menu; the
      // game never sees the press.
      REXLOG_INFO("[game-ui] pause menu shortcut {}", pressed & kY ? "Y" : "X");
      Sound("ui_select_a_button");
      OpenFromGameThread(pressed & kY ? Screen::kAchievements : Screen::kDebugMenu);
      g_hold[user] = true;
      *game_buttons = buttons & ~(kY | kX);
      *keep_sticks = true;
      return true;
    }
    // The debug menu in play: hold RB and press Left.
    if (InGame() && !PauseMenuShowing() && !GameMenuPointerActive() && (buttons & kRB) &&
        (pressed & kLeft)) {
      g_screen = Screen::kDebugMenu;
      g_hold[user] = true;
      *game_buttons = buttons & ~(kRB | kLeft);
      *keep_sticks = true;
      return true;
    }
  }
  if (screen != Screen::kNone) {
    g_hold[user] = true;
    if (screen == Screen::kDebugMenu) {
      *game_buttons = buttons & ~kMenuButtons;
      *keep_sticks = true;
    }
    return true;
  }
  if (g_hold[user]) {
    if ((buttons & kMenuButtons) == 0) {
      g_hold[user] = false;
      return false;
    }
    *game_buttons = buttons & ~kMenuButtons;
    *keep_sticks = true;
    return true;
  }
  return false;
}

// --- Drawing helpers ----------------------------------------------------

namespace {
// How a button looks on the player's controller.
struct Look {
  rex::input::PromptStyle style = rex::input::PromptStyle::kXbox;
  rex::input::PromptButton prompt = rex::input::PromptButton::kA;
  std::string label;
  bool face = false;  // A, B, X or Y
};
Look LookOf(std::string_view button) {
  using rex::input::PromptButton;
  using rex::input::PromptStyle;
  // The keys the screens use on a keyboard.
  struct Name {
    std::string_view button, key;
    PromptButton prompt;
  };
  constexpr Name names[] = {
      {"A", "Enter", PromptButton::kA},
      {"B", "Esc", PromptButton::kB},
      {"X", "F2", PromptButton::kX},
      {"Y", "Del", PromptButton::kY},
      {"LB", "PgUp", PromptButton::kLeftShoulder},
      {"RB", "PgDn", PromptButton::kRightShoulder},
      {"Start", "Enter", PromptButton::kStart},
      {"Erase", "Backspace", PromptButton::kX},
      {"Space", "Space", PromptButton::kY},
  };
  Look look;
  look.label = std::string(button);
  for (const auto& n : names) {
    if (n.button != button)
      continue;
    look.prompt = n.prompt;
    look.face = n.prompt <= PromptButton::kY;
    auto* input =
        g_runtime ? dynamic_cast<rex::input::InputSystem*>(g_runtime->input_system()) : nullptr;
    if (input) {
      uint32_t cutscene_player = 0;
      bool cutscene_keyboard = false;
      if (CutsceneSkipPromptPlayer(&cutscene_player, &cutscene_keyboard)) {
        const auto prompt = cutscene_keyboard ?
            rex::input::MakeButtonPrompt(PromptStyle::kKeyboard, n.prompt) :
            input->GetButtonPrompt(cutscene_player, n.prompt);
        look.style = prompt.style;
        look.label = prompt.label;
        return look;
      }
      const auto prompt = input->GetButtonPrompt(input->GetLastActivePlayer(), n.prompt);
      look.style = prompt.style;
      look.label = prompt.style == PromptStyle::kKeyboard ? std::string(n.key) : prompt.label;
    }
    if (g_controls == Controls::kKeyboard) {
      look.style = PromptStyle::kKeyboard;
      look.label = std::string(n.key);
    }
  }
  return look;
}
}  // namespace

void ScreenFonts(canvas::Canvas& c, uint32_t* title, uint32_t* body, const uint8_t* base) {
  PickFonts(c, title, body, base);
}

bool KeyboardPrompts() {
  return LookOf("A").style == rex::input::PromptStyle::kKeyboard;
}

float GlyphWidth(Frame& f, float size, std::string_view button) {
  const Look look = LookOf(button);
  if (look.style == rex::input::PromptStyle::kPlayStation && look.face)
    return size;
  f.c.UseFont(f.title_font);
  const float text = size * (look.label.size() > 1 ? 0.5f : 0.7f);
  return std::max(size, f.c.Measure(text, look.label) + size * 0.6f);
}

float Glyph(Frame& f, float x, float y, float size, std::string_view button) {
  using rex::input::PromptButton;
  using rex::input::PromptStyle;
  auto& c = f.c;
  const Look look = LookOf(button);
  const PromptStyle style = look.style;
  const std::string& label = look.label;
  const bool face = look.face;
  const Look* name = &look;
  if (style == PromptStyle::kPlayStation && face) {
    // A dark disc with the shape in its colour.
    const float r = size * 0.5f, cx = x + r, cy = y + r, k = r * 0.42f;
    const float thick = std::max(1.5f, size * 0.09f);
    c.Circle(cx, cy, r + 1.5f * f.s, 0xC0FFFFFF);
    c.Circle(cx, cy, r, 0xFF101726);
    if (name->prompt == PromptButton::kA) {
      c.Line(cx - k, cy - k, cx + k, cy + k, thick, 0xFF82B9FF);
      c.Line(cx - k, cy + k, cx + k, cy - k, thick, 0xFF82B9FF);
    } else if (name->prompt == PromptButton::kB) {
      for (int i = 0; i < 16; ++i) {
        const float a = i * 6.2831853f / 16, b = (i + 1) * 6.2831853f / 16;
        c.Line(cx + std::cos(a) * k, cy + std::sin(a) * k, cx + std::cos(b) * k,
               cy + std::sin(b) * k, thick, 0xFFFF7885);
      }
    } else if (name->prompt == PromptButton::kX) {
      c.Frame(cx - k, cy - k, k * 2, k * 2, thick, 0xFFF4A0D8);
    } else {
      c.Line(cx, cy - k, cx + k, cy + k * 0.8f, thick, 0xFF7CE7B4);
      c.Line(cx + k, cy + k * 0.8f, cx - k, cy + k * 0.8f, thick, 0xFF7CE7B4);
      c.Line(cx - k, cy + k * 0.8f, cx, cy - k, thick, 0xFF7CE7B4);
    }
    return size;
  }
  uint32_t fill = 0xFF3C465A;
  if (style == PromptStyle::kXbox && face) {
    if (look.prompt == PromptButton::kA)
      fill = 0xFF60B228;
    else if (look.prompt == PromptButton::kB)
      fill = 0xFFD63031;
    else if (look.prompt == PromptButton::kX)
      fill = 0xFF2078D6;
    else
      fill = 0xFFE6B214;
  }
  c.UseFont(f.title_font);
  const float text = size * (label.size() > 1 ? 0.5f : 0.7f);
  const float width = std::max(size, c.Measure(text, label) + size * 0.6f);
  const float radius = style == PromptStyle::kKeyboard ? size * 0.2f : size * 0.5f;
  c.RoundRect(x - 1.5f * f.s, y - 1.5f * f.s, width + 3 * f.s, size + 3 * f.s,
              radius + 1.5f * f.s, 0xC0FFFFFF);
  c.RoundRect(x, y, width, size, radius, fill);
  c.Text(x + width * 0.5f, y + (size - text) * 0.5f, text, 0xFFFFFFFF, label,
         canvas::Align::kCenter);
  return width;
}

void Padlock(Frame& f, float cx, float cy, float size, uint32_t color) {
  auto& c = f.c;
  const float body_w = size * 0.8f, body_h = size * 0.55f, thick = std::max(1.5f, size * 0.13f);
  const float top = cy - body_h * 0.15f, r = body_w * 0.3f;
  // The shackle: a half circle of short lines.
  for (int i = 0; i < 8; ++i) {
    const float a0 = 3.14159265f + i * 3.14159265f / 8, a1 = a0 + 3.14159265f / 8;
    c.Line(cx + std::cos(a0) * r, top + std::sin(a0) * r, cx + std::cos(a1) * r,
           top + std::sin(a1) * r, thick, color);
  }
  c.RoundRect(cx - body_w * 0.5f, top, body_w, body_h, size * 0.1f, color);
}

float ShadowText(Frame& f, float x, float y, float size, uint32_t color, std::string_view text,
                 int align) {
  const auto a = canvas::Align(align);
  const float offset = std::max(1.0f, size * 0.07f);
  const uint32_t shadow = (uint32_t((color >> 24) * 0.55f) << 24);
  f.c.Text(x + offset, y + offset, size, shadow, text, a);
  return f.c.Text(x, y, size, color, text, a);
}

// --- The list -----------------------------------------------------------

namespace {
constexpr float kRowHeight = 38, kHeaderHeight = 96, kSubtitleHeight = 36, kFooterHeight = 26;
constexpr int kVisibleRows = 11;
}  // namespace

List::List(Frame& f, State& state, std::string_view page, bool cheats)
    : f_(f), state_(state), page_(page), cheats_(cheats) {
  const Input& in = f.input;
  const int count = int(state.selectable.size());
  const auto step = [&](int direction) {
    for (int i = 1; i <= count; ++i) {
      const int j = ((state.selected + direction * i) % count + count) % count;
      if (state.selectable[size_t(j)]) {
        state.selected = j;
        return;
      }
    }
  };
  if (count) {
    if (in.up) {
      step(-1);
      Sound("ui_move_up_down");
    }
    if (in.down) {
      step(1);
      Sound("ui_move_up_down");
    }
  }
  // The rows where they were drawn last frame: hovering selects, a tap uses.
  const float row_h = kRowHeight * f.s;
  const auto row_at = [&](float x, float y) {
    if (state.row_y.size() != state.selectable.size() || x < state.left ||
        x >= state.left + state.width)
      return -1;
    for (size_t i = 0; i < state.row_y.size(); ++i)
      if (state.selectable[i] && state.row_y[i] >= 0 && y >= state.row_y[i] &&
          y < state.row_y[i] + row_h)
        return int(i);
    return -1;
  };
  if (in.hovering && in.hover_moved && !in.tapped) {
    const int row = row_at(in.hover_x, in.hover_y);
    if (row >= 0)
      state.selected = row;
  }
  if (in.tapped) {
    tapped_row_ = row_at(in.tap_x, in.tap_y);
    if (tapped_row_ >= 0) {
      state.selected = tapped_row_;
      tap_side_ = in.tap_x < state.left + state.width * 0.62f ? 0.0f
                  : in.tap_x < state.left + state.width * 0.81f ? -1.0f
                                                                 : 1.0f;
    }
  }
  accept_ = in.accept || tapped_row_ >= 0;
  back_ = in.back;
  change_ = in.left ? -1 : in.right ? 1 : 0;
}

bool List::Activated(size_t index, bool locked) {
  if (!Selected(index) || !accept_)
    return false;
  if (locked) {
    locked_use_ = true;
    Sound("ui_back_b_button");
    return false;
  }
  Sound("ui_select_a_button");
  return true;
}

bool List::Action(std::string_view label, std::string_view help, bool cheat,
                  std::string_view detail) {
  const size_t i = rows_.size();
  const bool locked = cheat && !cheats_;
  rows_.push_back(
      {Kind::kAction, std::string(label), std::string(detail), std::string(help), false, locked});
  return Activated(i, locked);
}

bool List::Toggle(std::string_view label, bool on, std::string_view help, bool cheat) {
  const size_t i = rows_.size();
  const bool locked = cheat && !cheats_;
  rows_.push_back({Kind::kToggle, std::string(label), {}, std::string(help), on, locked});
  if (Selected(i) && change_ && !locked && ((change_ > 0) != on)) {
    Sound("ui_toggle_on_off");
    return true;
  }
  return Activated(i, locked);
}

bool List::Submenu(std::string_view label, std::string_view help, bool cheat) {
  const size_t i = rows_.size();
  const bool locked = cheat && !cheats_;
  rows_.push_back({Kind::kSubmenu, std::string(label), {}, std::string(help), false, locked});
  if (Selected(i) && change_ > 0 && !locked) {
    Sound("ui_select_a_button");
    return true;
  }
  return Activated(i, locked);
}

int List::Value(std::string_view label, std::string_view value, std::string_view help,
                bool cheat) {
  const size_t i = rows_.size();
  const bool locked = cheat && !cheats_;
  rows_.push_back({Kind::kValue, std::string(label), std::string(value), std::string(help), false,
                   locked});
  if (!Selected(i))
    return 0;
  int direction = change_;
  if (!direction && accept_)
    direction = tapped_row_ == int(i) && tap_side_ < 0 ? -1 : 1;
  if (!direction)
    return 0;
  if (locked) {
    if (accept_)
      locked_use_ = true;
    return 0;
  }
  Sound("ui_move_up_down");
  return direction;
}

void List::Divider(std::string_view label) {
  rows_.push_back({Kind::kDivider, std::string(label)});
}

void List::Info(std::string_view label, std::string_view value) {
  rows_.push_back({Kind::kInfo, std::string(label), std::string(value)});
}

void List::Draw(float x, float y, float width, std::string_view status, float status_alpha) {
  auto& c = f_.c;
  const float s = f_.s;
  // What can be selected, for the next frame's movement.
  state_.selectable.assign(rows_.size(), false);
  for (size_t i = 0; i < rows_.size(); ++i)
    state_.selectable[i] = rows_[i].kind != Kind::kDivider && rows_[i].kind != Kind::kInfo;
  if (state_.selected >= int(rows_.size()) || state_.selected < 0 ||
      !state_.selectable[size_t(state_.selected)]) {
    const auto first = std::find(state_.selectable.begin(), state_.selectable.end(), true);
    state_.selected = first == state_.selectable.end() ? 0 : int(first - state_.selectable.begin());
  }
  const int count = int(rows_.size());
  const int visible = std::min(count, kVisibleRows);
  // Scrolling: the wheel, the right stick or a drag, and the selection kept
  // in view.
  if (f_.input.scroll != 0) {
    state_.scroll += f_.input.scroll;
    const int rows = int(state_.scroll);
    state_.scroll -= float(rows);
    state_.top += rows;
  }
  if (!f_.input.pointer || f_.input.up || f_.input.down) {
    if (state_.selected < state_.top)
      state_.top = state_.selected;
    if (state_.selected >= state_.top + visible)
      state_.top = state_.selected - visible + 1;
  }
  state_.top = std::clamp(state_.top, 0, std::max(0, count - visible));
  state_.left = x;
  state_.width = width;

  // The banner.
  const float header = kHeaderHeight * s;
  c.Gradient(x, y, width, header, 0xFFFFE34A, 0xFFF2AE00);
  c.Rect(x, y + header - 5 * s, width, 5 * s, 0xFFB57A00);
  // Sprinkles across the banner, like the donut's.
  {
    uint32_t seed = 0x5EED;
    const uint32_t colors[] = {0xE6FFFFFF, 0xFF70D1FE, 0xFFF06292, 0xFF6ED65C, 0xFF8C5ADC};
    for (int i = 0; i < 26; ++i) {
      seed = seed * 1664525u + 1013904223u;
      const float px = x + 10 * s + float(seed >> 8 & 0xFFFF) / 65535.0f * (width - 20 * s);
      seed = seed * 1664525u + 1013904223u;
      const float py = y + 8 * s + float(seed >> 8 & 0xFFFF) / 65535.0f * (header - 22 * s);
      const float angle = float(seed & 0xFF) / 255.0f * 3.14159265f;
      const float len = 6 * s;
      c.Line(px - std::cos(angle) * len, py - std::sin(angle) * len, px + std::cos(angle) * len,
             py + std::sin(angle) * len, 3.2f * s, colors[i % 5] & 0x80FFFFFF);
    }
  }
  // The donut.
  {
    const float cx = x + width - 104 * s, cy = y + header * 0.47f, r = 30 * s;
    c.Circle(cx + 3 * s, cy + 4 * s, r, 0x50000000);
    c.Circle(cx, cy, r, 0xFFE8A33D);
    c.Circle(cx, cy - 2 * s, r * 0.9f, 0xFFF06292);
    const uint32_t colors[] = {0xFFFFFFFF, 0xFF70D1FE, 0xFFFFD90F, 0xFF6ED65C, 0xFF8C5ADC};
    for (int i = 0; i < 14; ++i) {
      const float a = i * 2.399963f, d = r * (0.48f + 0.35f * float((i * 37) % 10) / 10);
      const float px = cx + std::cos(a) * d, py = cy - 2 * s + std::sin(a) * d;
      c.Line(px - std::cos(a * 3) * 3.5f * s, py - std::sin(a * 3) * 3.5f * s,
             px + std::cos(a * 3) * 3.5f * s, py + std::sin(a * 3) * 3.5f * s, 2.6f * s,
             colors[i % 5]);
    }
    c.Circle(cx, cy - 2 * s, r * 0.32f, 0xFFF2BE20);
  }
  // Close: a red button in the banner's corner, for the mouse and touch.
  {
    const float cx = x + width - 34 * s, cy = y + 34 * s, r = 20 * s;
    c.Circle(cx + 2 * s, cy + 3 * s, r, 0x50000000);
    c.Circle(cx, cy, r + 2.5f * s, 0xFFFFFFFF);
    c.Circle(cx, cy, r, 0xFFD63031);
    c.Line(cx - 7 * s, cy - 7 * s, cx + 7 * s, cy + 7 * s, 4 * s, 0xFFFFFFFF);
    c.Line(cx - 7 * s, cy + 7 * s, cx + 7 * s, cy - 7 * s, 4 * s, 0xFFFFFFFF);
    const Input& in = f_.input;
    if (in.tapped && std::hypot(in.tap_x - cx, in.tap_y - cy) < r * 1.6f)
      close_ = true;
  }
  c.UseFont(f_.title_font);
  {
    const float size = 50 * s;
    const float tx = x + 22 * s, ty = y + (header - 5 * s - size) * 0.5f;
    for (int dx = -1; dx <= 1; ++dx)
      for (int dy = -1; dy <= 1; ++dy)
        if (dx || dy)
          c.Text(tx + dx * 2.5f * s, ty + dy * 2.5f * s, size, 0xFF2A1A00, "DEBUG MENU");
    c.Text(tx, ty, size, 0xFFFFFFFF, "DEBUG MENU");
  }
  y += header;

  // The page and where the selection is.
  const float subtitle = kSubtitleHeight * s;
  c.Rect(x, y, width, subtitle, 0xF5101828);
  c.UseFont(f_.title_font);
  float title_x = x + 16 * s;
  if (allow_back_) {
    // Back, for the mouse and touch: the arrow and the page's name.
    const float cy = y + subtitle * 0.5f;
    c.Line(x + 26 * s, cy - 8 * s, x + 17 * s, cy, 4 * s, 0xFFFFD90F);
    c.Line(x + 17 * s, cy, x + 26 * s, cy + 8 * s, 4 * s, 0xFFFFD90F);
    title_x = x + 38 * s;
    const Input& in = f_.input;
    if (in.tapped && in.tap_x >= x && in.tap_x < x + width * 0.6f && in.tap_y >= y &&
        in.tap_y < y + subtitle)
      back_ = true;
  }
  c.Text(title_x, y + (subtitle - 24 * s) * 0.5f, 24 * s, 0xFFFFD90F, page_);
  {
    int position = 0, total = 0;
    for (int i = 0; i < count; ++i)
      if (state_.selectable[size_t(i)]) {
        ++total;
        if (i <= state_.selected)
          ++position;
      }
    char counter[32];
    std::snprintf(counter, sizeof(counter), "%d / %d", total ? position : 0, total);
    c.UseFont(f_.body_font);
    c.Text(x + width - 16 * s, y + (subtitle - 22 * s) * 0.5f, 22 * s, 0xFFDDE6F5, counter,
           canvas::Align::kRight);
  }
  y += subtitle;

  // The rows.
  const float row_h = kRowHeight * s, text = 25 * s;
  state_.row_y.assign(rows_.size(), -1.0f);
  for (int i = state_.top; i < state_.top + visible; ++i) {
    const Row& row = rows_[size_t(i)];
    const bool selected = i == state_.selected;
    state_.row_y[size_t(i)] = y;
    if (row.kind == Kind::kDivider) {
      c.Rect(x, y, width, row_h, 0xE60A1020);
      c.UseFont(f_.title_font);
      const float w = c.Text(x + 16 * s, y + (row_h - 21 * s) * 0.5f, 21 * s, 0xFFD9B23A, row.label);
      c.Rect(x + 28 * s + w, y + row_h * 0.5f, width - 44 * s - w, 2 * s, 0x60D9B23A);
      y += row_h;
      continue;
    }
    c.Rect(x, y, width, row_h, selected ? 0xFFFFD90F : 0xD60A1020);
    if (selected)
      c.Rect(x, y, 6 * s, row_h, 0xFFF06292);
    const uint32_t ink = selected ? 0xFF121A30 : row.locked ? 0xFF7C8AA6 : 0xFFF2F5FA;
    const uint32_t dim = selected ? 0xFF3A4660 : 0xFFA9B6CC;
    c.UseFont(f_.body_font);
    float right = x + width - 16 * s;
    const float ty = y + (row_h - text) * 0.5f;
    if (row.locked) {
      Padlock(f_, right - 9 * s, y + row_h * 0.5f, 18 * s, ink);
      right -= 28 * s;
    }
    switch (row.kind) {
      case Kind::kToggle: {
        const float box = 22 * s, bx = right - box, by = y + (row_h - box) * 0.5f;
        if (row.on) {
          c.RoundRect(bx, by, box, box, 4 * s, selected ? 0xFF2E7D32 : 0xFF6ED65C);
          c.Line(bx + box * 0.22f, by + box * 0.52f, bx + box * 0.43f, by + box * 0.74f, 3.2f * s,
                 0xFFFFFFFF);
          c.Line(bx + box * 0.41f, by + box * 0.74f, bx + box * 0.8f, by + box * 0.26f, 3.2f * s,
                 0xFFFFFFFF);
        } else {
          c.RoundRect(bx, by, box, box, 4 * s, selected ? 0xFF121A30 : 0xB0FFFFFF);
          c.RoundRect(bx + 3 * s, by + 3 * s, box - 6 * s, box - 6 * s, 2 * s,
                      selected ? 0xFFFFD90F : 0xFF0A1020);
        }
        right = bx - 12 * s;
        break;
      }
      case Kind::kSubmenu: {
        const float cx = right - 6 * s, cy = y + row_h * 0.5f;
        c.Line(cx - 6 * s, cy - 7 * s, cx, cy, 3 * s, ink);
        c.Line(cx, cy, cx - 6 * s, cy + 7 * s, 3 * s, ink);
        right = cx - 18 * s;
        break;
      }
      case Kind::kValue: {
        const float vw = c.Measure(text * 0.92f, row.value);
        if (selected) {
          const float cy = y + row_h * 0.5f;
          c.Triangle(right, cy, right - 9 * s, cy - 7 * s, right - 9 * s, cy + 7 * s, ink);
          right -= 16 * s;
          c.Text(right, y + (row_h - text * 0.92f) * 0.5f, text * 0.92f, ink, row.value,
                 canvas::Align::kRight);
          right -= vw + 8 * s;
          c.Triangle(right - 9 * s, cy, right, cy - 7 * s, right, cy + 7 * s, ink);
          right -= 18 * s;
        } else {
          c.Text(right, y + (row_h - text * 0.92f) * 0.5f, text * 0.92f, dim, row.value,
                 canvas::Align::kRight);
          right -= vw + 14 * s;
        }
        break;
      }
      case Kind::kInfo: {
        // Resource paths can be longer than the entire panel. Reserve room
        // for the label and fit the value before right-aligning it.
        const float label_width = std::min(c.Measure(text, row.label), width * 0.4f);
        const auto value = c.Fit(text * 0.92f, row.value, right - x - 34 * s - label_width);
        c.Text(right, y + (row_h - text * 0.92f) * 0.5f, text * 0.92f, 0xFFFFE57A, value,
               canvas::Align::kRight);
        right -= c.Measure(text * 0.92f, value) + 14 * s;
        break;
      }
      case Kind::kAction:
        if (!row.value.empty()) {
          c.Text(right, y + (row_h - text * 0.8f) * 0.5f, text * 0.8f, dim, row.value,
                 canvas::Align::kRight);
          right -= c.Measure(text * 0.8f, row.value) + 14 * s;
        }
        break;
      default:
        break;
    }
    const uint32_t label_ink = row.kind == Kind::kInfo ? dim : ink;
    c.Text(x + 18 * s, ty, text, label_ink, c.Fit(text, row.label, right - x - 26 * s));
    y += row_h;
  }

  // The footer: arrows when there is more.
  const float footer = kFooterHeight * s;
  c.Rect(x, y, width, footer, 0xF5101828);
  if (count > visible) {
    const float cx = x + width * 0.5f, cy = y + footer * 0.5f;
    const uint32_t up = state_.top > 0 ? 0xFFFFFFFF : 0x40FFFFFF;
    const uint32_t down = state_.top + visible < count ? 0xFFFFFFFF : 0x40FFFFFF;
    c.Triangle(cx - 16 * s, cy + 1 * s, cx - 8 * s, cy - 6 * s, cx, cy + 1 * s, up);
    c.Triangle(cx + 4 * s, cy - 3 * s, cx + 20 * s, cy - 3 * s, cx + 12 * s, cy + 5 * s, down);
  }
  y += footer + 8 * s;

  // The selected row's help, and the last thing that happened.
  const Row* selected = state_.selected >= 0 && state_.selected < count
                            ? &rows_[size_t(state_.selected)]
                            : nullptr;
  std::string help = selected ? selected->help : std::string();
  if (selected && selected->locked)
    help = "Needs cheats: turn Cheats on at the top of the main page." +
           (help.empty() ? std::string() : " " + help);
  c.UseFont(f_.body_font);
  const float help_size = 21 * s;
  const auto lines = c.Wrap(help_size, help, width - 32 * s);
  const bool has_status = !status.empty() && status_alpha > 0;
  const float box_h = 14 * s + float(std::min<size_t>(lines.size(), 5)) * help_size * 1.2f +
                      (has_status ? help_size * 1.35f : 0);
  if (!lines.empty() || has_status) {
    c.Rect(x, y, width, box_h + 6 * s, 0xE60A1020);
    c.Rect(x, y, width, 3 * s, 0xFFFFD90F);
    float ty = y + 10 * s;
    if (has_status) {
      const uint32_t a = uint32_t(255 * std::clamp(status_alpha, 0.0f, 1.0f)) << 24;
      c.UseFont(f_.title_font);
      c.Text(x + 16 * s, ty, help_size, a | 0x6ED65C, status);
      c.UseFont(f_.body_font);
      ty += help_size * 1.35f;
    }
    for (size_t i = 0; i < lines.size() && i < 5; ++i) {
      c.Text(x + 16 * s, ty, help_size, 0xFFE8EDF5, lines[i]);
      ty += help_size * 1.2f;
    }
  }
}

}  // namespace game_ui

// The front end's APT render, the last 2D pass of the frame.
REX_FUNC(sub_827BCAB8) {
  __imp__sub_827BCAB8(ctx, base);
  if (game_ui::g_alive.load() && game_ui::FrontEndShows(base))
    game_ui::FrameScreens(ctx, base);
}

// The end of the HUD pass, before it puts its render states back.
REX_FUNC(sub_826D4840) {
  // Story videos keep this native HUD/font pass even while their front-end
  // screens are unloaded. Draw before its camera and render states are reset.
  static uint64_t cutscene_frame = ~uint64_t{0};
  if (game_ui::g_alive.load() && cutscene_frame != game_ui::g_frame) {
    cutscene_frame = game_ui::g_frame;
    DrawCutsceneSkip(ctx, base);
  }
  if (game_ui::g_alive.load() && !game_ui::FrontEndShows(base))
    game_ui::FrameScreens(ctx, base);
  __imp__sub_826D4840(ctx, base);
}
