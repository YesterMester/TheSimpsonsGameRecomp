// The movie player's own input and drawing passes. Story videos unload the
// front end, so their controls use the game's font batch, like subtitles.
// The decoder's pause, resume and stop sequence still owns movie playback.

#include "cutscene_skip.h"

#include "game_canvas.h"
#include "game_thread.h"
#include "game_ui.h"
#include "guest_memory.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <string>
#include <string_view>

#include <rex/cvar.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/runtime.h>
#include <rex/ui/guest_output_viewport.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>

REXCVAR_DEFINE_BOOL(cutscene_skip, true, "Game", "Skip cutscenes with a confirmation");
REXCVAR_DEFINE_BOOL(cutscene_skip_log, false, "Game", "Log cutscene skip transitions");
REXCVAR_DECLARE(bool, mods_editor_preview);
REXCVAR_DECLARE(std::string, quick_launch_level);

REX_EXTERN(__imp__sub_826B8B70);
REX_EXTERN(sub_826B8B70);
REX_EXTERN(__imp__sub_826B8AD8);
REX_EXTERN(sub_826B8AD8);
REX_EXTERN(__imp__sub_828A8968);
REX_EXTERN(sub_828A8968);
REX_EXTERN(__imp__sub_8239A7A0);
REX_EXTERN(sub_8239A7A0);

namespace {
using simpsons::LoadGuestU32;
using Clock = std::chrono::steady_clock;
constexpr uint32_t kInputTick = 0x82D57390, kInputChanged = 0x82D573A0;
constexpr uint32_t kReveal = game_ui::kPadX;
enum class Phase { kHidden, kHint, kConfirm };

rex::ui::Window* g_window = nullptr;
rex::input::InputSystem* g_input = nullptr;
std::atomic<uint32_t> g_movie{0}, g_player{0};
std::atomic<bool> g_alive{false}, g_ready{false}, g_keyboard{false}, g_touch{false};
std::atomic<Phase> g_phase{Phase::kHidden};
std::atomic<uint16_t> g_keys{0};
std::array<uint16_t, 4> g_held{};
std::array<uint8_t, 4> g_triggers{};
bool g_armed = false, g_paused = false, g_pointer = false;
bool g_drawn = false, g_no_canvas = false;
Clock::time_point g_hint_until{};

struct Rect {
  float x = 0, y = 0, w = 0, h = 0;
  bool Contains(float px, float py) const {
    return w > 0 && h > 0 && px >= x && py >= y && px < x + w && py < y + h;
  }
};
std::mutex g_hits_mutex;
Rect g_hint_hit, g_yes_hit, g_no_hit;  // normalized game-image coordinates

bool Enabled() {
  return g_alive && g_movie && REXCVAR_GET(cutscene_skip);
}

void SetPhase(Phase phase) {
  if (g_phase.exchange(phase) != phase && REXCVAR_GET(cutscene_skip_log))
    REXLOG_INFO("[cutscene-skip] {}", phase == Phase::kHidden ? "hidden"
                                      : phase == Phase::kHint ? "hint"
                                                              : "confirm");
}

void Resume(PPCContext& ctx, uint8_t* base) {
  const uint32_t movie = g_movie;
  if (g_paused && movie && LoadGuestU32(base, movie + 20) == 6)
    CallGuest(ctx, base, 0x826B8D68, {movie});
  g_paused = false;
}

void ReleasePointer() {
  if (g_pointer && g_input)
    g_input->SetMenuPointerActive(false);
  g_pointer = false;
}

void Press(uint16_t button, bool keyboard, bool touch = false) {
  g_player = g_input ? g_input->GetLastActivePlayer() : 0;
  g_keyboard = keyboard;
  g_touch = touch;
  g_keys.fetch_or(button);
}

bool BoundKey(rex::ui::VirtualKey key, const char* name) {
  const std::string saved = rex::cvar::GetFlagByName(name);
  std::string_view bindings(saved);
  while (!bindings.empty()) {
    const auto end = bindings.find(',');
    auto token = bindings.substr(0, end);
    while (!token.empty() && token.front() == ' ')
      token.remove_prefix(1);
    while (!token.empty() && token.back() == ' ')
      token.remove_suffix(1);
    if (!token.empty() && rex::ui::ParseVirtualKey(token) == key)
      return true;
    if (end == std::string_view::npos)
      break;
    bindings.remove_prefix(end + 1);
  }
  return false;
}

uint16_t KeyButton(rex::ui::VirtualKey key) {
  if (BoundKey(key, "keybind_b") || key == rex::ui::VirtualKey::kEscape)
    return game_ui::kPadB;
  if (BoundKey(key, "keybind_a"))
    return game_ui::kPadA;
  return kReveal;
}

class Listener final : public rex::ui::WindowInputListener {
 public:
  void OnKeyDown(rex::ui::KeyEvent& e) override {
    if (!Enabled() || !g_ready || e.is_handled() || e.is_alt_pressed())
      return;
    if (!e.prev_state())
      Press(KeyButton(e.virtual_key()), true);
    e.set_handled(true);
  }
  void OnKeyUp(rex::ui::KeyEvent& e) override {
    if (Enabled() && g_ready && !e.is_alt_pressed())
      e.set_handled(true);
  }
  void OnMouseDown(rex::ui::MouseEvent& e) override {
    if (!Enabled() || !g_ready || e.is_handled())
      return;
    using B = rex::ui::MouseEvent::Button;
    using K = rex::ui::VirtualKey;
    if (e.button() == B::kLeft) {
      Down(float(e.x()), float(e.y()), false);
    } else {
      const K key = e.button() == B::kRight    ? K::kRButton
                    : e.button() == B::kMiddle ? K::kMButton
                    : e.button() == B::kX1     ? K::kXButton1
                                               : K::kXButton2;
      Press(KeyButton(key), true);
    }
    e.set_handled(true);
  }
  void OnMouseUp(rex::ui::MouseEvent& e) override {
    if (!Enabled() || !g_ready || e.is_handled())
      return;
    if (e.button() == rex::ui::MouseEvent::Button::kLeft)
      Up(float(e.x()), float(e.y()), false);
    e.set_handled(true);
  }
  void OnMouseWheel(rex::ui::MouseEvent& e) override {
    if (!Enabled() || !g_ready || e.is_handled() || !e.scroll_y())
      return;
    Press(KeyButton(static_cast<rex::ui::VirtualKey>(e.scroll_y() > 0 ? 0x0A : 0x0B)), true);
    e.set_handled(true);
  }
  void OnTouchEvent(rex::ui::TouchEvent& e) override {
    if (!Enabled() || !g_ready || e.is_handled())
      return;
    using A = rex::ui::TouchEvent::Action;
    if (e.action() == A::kDown)
      Down(e.x(), e.y(), true);
    else if (e.action() == A::kUp)
      Up(e.x(), e.y(), true);
    else if (e.action() == A::kCancel)
      down_ = false;
    e.set_handled(true);
  }

 private:
  bool Position(float wx, float wy, float* x, float* y) const {
    if (!g_window)
      return false;
    const auto v = rex::ui::GetGuestOutputViewport();
    if (v.width <= 0 || v.height <= 0)
      return false;
    const float width = float(g_window->GetActualPhysicalWidth());
    const float height = float(g_window->GetActualPhysicalHeight());
    if (width <= 0 || height <= 0)
      return false;
    *x = (wx / width - v.x) / v.width;
    *y = (wy / height - v.y) / v.height;
    return *x >= 0 && *x <= 1 && *y >= 0 && *y <= 1;
  }
  void Down(float x, float y, bool touch) {
    down_ = Position(x, y, &down_x_, &down_y_);
    down_phase_ = g_phase;
    down_touch_ = touch;
  }
  void Up(float wx, float wy, bool touch) {
    float x = 0, y = 0;
    const bool valid = down_ && down_touch_ == touch && down_phase_ == g_phase &&
                       Position(wx, wy, &x, &y) && std::hypot(x - down_x_, y - down_y_) < .025f;
    down_ = false;
    if (!valid)
      return;
    uint16_t button = 0;
    {
      std::lock_guard lock(g_hits_mutex);
      if (down_phase_ == Phase::kHidden)
        button = kReveal;
      else if (down_phase_ == Phase::kHint && g_hint_hit.Contains(x, y) &&
               g_hint_hit.Contains(down_x_, down_y_))
        button = game_ui::kPadB;
      else if (down_phase_ == Phase::kConfirm) {
        if (g_yes_hit.Contains(x, y) && g_yes_hit.Contains(down_x_, down_y_))
          button = game_ui::kPadA;
        else if (g_no_hit.Contains(x, y) && g_no_hit.Contains(down_x_, down_y_))
          button = game_ui::kPadB;
      }
    }
    if (button)
      Press(button, !touch, touch);
  }
  bool down_ = false, down_touch_ = false;
  float down_x_ = 0, down_y_ = 0;
  Phase down_phase_ = Phase::kHidden;
};
Listener g_listener;

void Draw(PPCContext& ctx, uint8_t* base) {
  if (!Enabled() || !g_ready || g_phase == Phase::kHidden)
    return;
  const PPCContext saved = ctx;
  {
    canvas::Canvas c(ctx, base);
    if (!c.valid()) {
      if (!g_no_canvas && REXCVAR_GET(cutscene_skip_log))
        REXLOG_INFO("[cutscene-skip] waiting for the movie's font camera");
      g_no_canvas = true;
      ctx = saved;
      return;
    }
    uint32_t title = 0, body = 0;
    game_ui::ScreenFonts(c, &title, &body, base);
    const game_ui::Input input{};
    game_ui::Frame f{ctx, base, c, input, c.height() / 720, 0, title, body};
    const float s = f.s, width = c.width(), height = c.height();
    const auto hit = [&](Rect r) {
      return Rect{r.x / width, r.y / height, r.w / width, r.h / height};
    };
    if (g_phase == Phase::kHint) {
      c.UseFont(body);
      const float text = 26 * s, size = 30 * s;
      const std::string_view press = g_touch ? "Tap " : "Press ";
      const float prefix = c.Measure(text, press), suffix = c.Measure(text, " to skip");
      const float glyph = game_ui::GlyphWidth(f, size, "B");
      const float x = width - 32 * s - prefix - glyph - suffix - 12 * s;
      const float y = height - 60 * s;
      c.UseFont(body);
      game_ui::ShadowText(f, x, y, text, 0xFFFFFFFF, press);
      game_ui::Glyph(f, x + prefix + 6 * s, y - 2 * s, size, "B");
      c.UseFont(body);
      game_ui::ShadowText(f, x + prefix + glyph + 12 * s, y, text, 0xFFFFFFFF, " to skip");
      std::lock_guard lock(g_hits_mutex);
      g_hint_hit = hit({x - 12 * s, y - 12 * s, width - x - 16 * s, 52 * s});
    } else if (g_phase == Phase::kConfirm) {
      const float w = std::min(700 * s, width - 64 * s), h = 254 * s;
      const float x = (width - w) * .5f, y = (height - h) * .5f;
      // The blue paper, pale edges and lettering used by the game's own
      // questions. Drawn into the movie frame, with its font atlas and states.
      c.Rect(0, 0, width, height, 0x50000000);
      const float xx[4]{x + 3 * s, x - 5 * s, x + w - 3 * s, x + w + 5 * s};
      const float yy[4]{y, y + h, y + 2 * s, y + h - 2 * s};
      c.Shape(xx, yy, 0xF0142147);
      c.Line(x - 7 * s, y, x + w + 7 * s, y + 2 * s, 3 * s, 0xFFA2D8F4);
      c.Line(x - 10 * s, y + h, x + w + 8 * s, y + h - 2 * s, 3 * s, 0xFFA2D8F4);
      c.UseFont(title);
      game_ui::ShadowText(f, width * .5f, y + 24 * s, 36 * s, 0xFFFFD90F, "Skip cutscene?", 1);
      c.UseFont(body);
      game_ui::ShadowText(f, width * .5f, y + 89 * s, 29 * s, 0xFFFFFFFF,
                          "Are you sure you want to skip?", 1);
      const float button_y = y + h - 67 * s, size = 32 * s;
      c.UseFont(body);
      const float yes_label = c.Measure(28 * s, "Skip");
      const float yes_width = game_ui::GlyphWidth(f, size, "A");
      const float no_x = x + 34 * s;
      const float yes_x = x + w - 34 * s - yes_width - 12 * s - yes_label;
      const float no_glyph = game_ui::Glyph(f, no_x, button_y, size, "B");
      c.UseFont(body);
      game_ui::ShadowText(f, no_x + no_glyph + 12 * s, button_y, 28 * s, 0xFFFFFFFF,
                          "Keep watching");
      const float yes_glyph = game_ui::Glyph(f, yes_x, button_y, size, "A");
      c.UseFont(body);
      game_ui::ShadowText(f, yes_x + yes_glyph + 12 * s, button_y, 28 * s, 0xFFFFFFFF, "Skip");
      std::lock_guard lock(g_hits_mutex);
      g_no_hit = hit({no_x - 14 * s, button_y - 14 * s, w * .57f, 60 * s});
      g_yes_hit = hit({yes_x - 14 * s, button_y - 14 * s, x + w - yes_x, 60 * s});
    }
    if (!g_drawn && REXCVAR_GET(cutscene_skip_log))
      REXLOG_INFO("[cutscene-skip] native movie UI {}x{}, fonts {:08X}/{:08X}", width, height,
                  title, body);
    g_drawn = true;
  }
  ctx = saved;
}

}  // namespace

void InitCutsceneSkip(rex::Runtime* runtime, rex::ui::Window* window) {
  g_window = window;
  g_input = dynamic_cast<rex::input::InputSystem*>(runtime->input_system());
  g_alive = true;
  if (window)
    window->AddInputListener(&g_listener, 2300);
}

void DrawCutsceneSkip(PPCContext& ctx, uint8_t* base) {
  Draw(ctx, base);
}

void ShutdownCutsceneSkip() {
  g_alive = false;
  g_ready = false;
  ReleasePointer();
  if (g_window)
    g_window->RemoveInputListener(&g_listener);
}

bool CutsceneSkipPromptPlayer(uint32_t* player, bool* keyboard) {
  if (!Enabled() || !g_ready || g_phase == Phase::kHidden)
    return false;
  *player = g_player;
  *keyboard = g_keyboard;
  return true;
}

void TickCutsceneSkip(PPCContext& ctx, uint8_t* base) {
  if (!Enabled()) {
    g_ready = false;
    ReleasePointer();
    SetPhase(Phase::kHidden);
    Resume(ctx, base);
    g_keys = 0;
    return;
  }
  const uint32_t movie = g_movie;
  const auto state = LoadGuestU32(base, movie + 20);
  g_ready = state == 3 || state == 6;
  if (!g_ready) {
    ReleasePointer();
    SetPhase(Phase::kHidden);
    g_keys = 0;
    g_armed = false;
    return;
  }
  if (g_input) {
    g_input->SetMenuPointerActive(true);
    g_pointer = true;
  }
  uint16_t pressed = g_keys.exchange(0);
  for (uint32_t player = 0; g_input && player < g_held.size(); ++player) {
    rex::input::X_INPUT_STATE pad{};
    const uint16_t buttons =
        g_input->GetState(player, &pad) == 0 ? uint16_t(pad.gamepad.buttons) : 0;
    const uint16_t edge = buttons & ~g_held[player];
    g_held[player] = buttons;
    const uint8_t triggers =
        uint8_t(pad.gamepad.left_trigger > 30) | uint8_t(pad.gamepad.right_trigger > 30) << 1;
    const auto trigger_edge = triggers & ~g_triggers[player];
    g_triggers[player] = triggers;
    if (edge || trigger_edge) {
      pressed |= edge;
      if (trigger_edge)
        pressed |= kReveal;
      g_player = player;
      g_keyboard = g_input->GetButtonPrompt(player, rex::input::PromptButton::kB).style ==
                   rex::input::PromptStyle::kKeyboard;
      g_touch = g_input->GetActiveDevice(player).kind == rex::input::DeviceKind::kTouch;
    }
  }
  // Ignore buttons held through loading and require another press after B
  // opens the question. Both are edges, independent for each controller.
  if (!g_armed) {
    g_armed = true;
    return;
  }
  if (g_phase == Phase::kConfirm) {
    if (pressed & (game_ui::kPadA | game_ui::kPadB)) {
      const bool skip = !(pressed & game_ui::kPadB);
      SetPhase(Phase::kHidden);
      if (skip) {
        // The same cleanup and MovieHasEnded event as the player's original
        // stop request: no early return out of the decoder or story logic.
        const auto controls = LoadGuestU32(base, 0x82D08DAC);
        for (uint32_t player = 0; controls && player < 4; ++player)
          CallGuest(ctx, base, 0x826BBF88, {controls, player});
        CallGuest(ctx, base, 0x8238F130, {0x82D61DF0, 0});
        if (REXCVAR_GET(cutscene_skip_log))
          REXLOG_INFO("[cutscene-skip] stop request {}", g_movie ? "pending" : "finished");
      } else {
        Resume(ctx, base);
        if (REXCVAR_GET(cutscene_skip_log))
          REXLOG_INFO("[cutscene-skip] keep watching");
      }
    }
  } else if (pressed & game_ui::kPadB) {
    if (state == 3) {
      CallGuest(ctx, base, 0x826B8D28, {movie});
      g_paused = LoadGuestU32(base, movie + 20) == 6;
    }
    SetPhase(Phase::kConfirm);
  } else if (pressed) {
    SetPhase(Phase::kHint);
    g_hint_until = Clock::now() + std::chrono::seconds(4);
  } else if (g_phase == Phase::kHint && Clock::now() >= g_hint_until) {
    SetPhase(Phase::kHidden);
  }
}

REX_FUNC(sub_826B8B70) {
  const uint32_t movie = ctx.r3.u32;
  const uint32_t descriptor = ctx.r4.u32;
  const bool quick_boot =
      REXCVAR_GET(mods_editor_preview) || !REXCVAR_GET(quick_launch_level).empty();
  if (quick_boot && descriptor) {
    const auto name = LoadGuestU32(base, descriptor);
    std::string_view path =
        name ? reinterpret_cast<const char*>(simpsons::GuestToHost(base, name)) : "";
    const auto slash = path.find_last_of("/\\");
    const auto file = path.substr(slash == std::string_view::npos ? 0 : slash + 1);
    const bool is_logo =
        file.starts_with("ealogo") || file.starts_with("foxlogo") || file.starts_with("gracielogo");
    if (is_logo || LoadGuestU32(base, 0x82D08C98) < 9) {
      REXLOG_INFO("[cutscene-skip] quick-launch skipping movie {}", path);
      ctx.r4.u64 = 0;
    }
  }
  __imp__sub_826B8B70(ctx, base);
  if (ctx.r3.u32) {
    const auto name = descriptor ? LoadGuestU32(base, descriptor) : 0;
    std::string_view path =
        name ? reinterpret_cast<const char*>(simpsons::GuestToHost(base, name)) : "";
    const auto slash = path.find_last_of("/\\");
    const auto file = path.substr(slash == std::string_view::npos ? 0 : slash + 1);
    // Logos run before the regular UI fonts are loaded. Their existing boot
    // controls and the separate logo-skipping patch keep handling them.
    if (file.starts_with("ealogo") || file.starts_with("foxlogo") || file.starts_with("gracielogo"))
      return;
    g_movie = movie;
    g_keys = 0;
    g_held = {};
    g_triggers = {};
    g_armed = g_paused = g_drawn = g_no_canvas = false;
    g_ready = false;
    SetPhase(Phase::kHidden);
    if (REXCVAR_GET(cutscene_skip_log)) {
      REXLOG_INFO("[cutscene-skip] movie {:08X} {}", movie, path);
    }
  }
}

REX_FUNC(sub_826B8AD8) {
  if (ctx.r3.u32 == g_movie) {
    SetPhase(Phase::kHidden);
    g_ready = false;
    g_movie = 0;
    g_paused = false;
    ReleasePointer();
  }
  __imp__sub_826B8AD8(ctx, base);
}

REX_FUNC(sub_828A8968) {
  if (Enabled() && ctx.r4.u32) {
    const auto type = LoadGuestU32(base, ctx.r4.u32);
    if (type == LoadGuestU32(base, kInputTick) || type == LoadGuestU32(base, kInputChanged)) {
      TickCutsceneSkip(ctx, base);
      return;
    }
  }
  __imp__sub_828A8968(ctx, base);
}

REX_FUNC(sub_8239A7A0) {
  if (Enabled())
    return;
  __imp__sub_8239A7A0(ctx, base);
}
