// Pointer input for the game's own screens: the front end, the pause menu and
// in-game screens like the notepad and the bus stop. Hit regions come from
// their rendered text and clips; actions go through the native screen handler.

#include "game_menu_pointer.h"
#include "game_ui.h"
#include "game_menu.h"
#include "game_menu_apt.h"
#include "guest_memory.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <chrono>
#include <cmath>
#include <deque>
#include <memory>
#include <map>
#include <mutex>
#include <numbers>
#include <string>
#include <string_view>
#include <set>
#include <vector>

#include <rex/input/input_system.h>
#include <rex/input/menu_input.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/ppc.h>
#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>
#include <rex/ui/guest_output_viewport.h>
#include <rex/ui/overlay_input.h>
#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>

REX_EXTERN(__imp__sub_827F2DC8);
REX_EXTERN(sub_827F2DC8);

REXCVAR_DEFINE_BOOL(menu_pointer_log, false, "Input", "Log native menu pointer targets");
REXCVAR_DEFINE_STRING(menu_pointer_test_file, "", "Input",
                      "Development: taps and clicks read from this file, one per line as "
                      "'tap X Y', 'click X Y' or 'swipe X Y X2 Y2' in game image coordinates "
                      "(0 to 1)");

namespace {
using rex::X_RESULT;
using simpsons::GuestToHost;
using simpsons::LoadGuestFloat;
using simpsons::LoadGuestU32;
using simpsons::StoreGuestU32;
using Clock = std::chrono::steady_clock;
std::atomic<bool> g_native_menu_active{false};

// Pictures on the bus stop screens that a tap picks: a stop on the bus stop
// map, a character on the phone. A step is a picture on a carousel (Movies,
// Clichés), reached by moving the carousel `index` places (left if negative).
enum class Pick { kNone, kBusStop, kCharacter, kStep, kSaveKey };
struct Hit {
  int row = -1, key = 0;
  float x = 0, y = 0, width = 0, height = 0;
  uint64_t identity = 0;
  Clock::time_point seen{};
  Pick pick = Pick::kNone;
  int index = -1;
};
struct Command {
  int row, key;
  uint64_t generation;
  Pick pick = Pick::kNone;
  int index = -1;
};
struct Screen {
  uint32_t address, table;
};

// The screens' input methods take the game's UI events: 0 pause, 1 select
// (Back), 2-5 the D-pad, 6 accept, 7 cancel, 8/9 LT/RT, 10/11 LB/RB,
// 12 option (X) and 13 help (Y). Hit regions use 1 for Start and 8/9 for X/Y.
int ScreenKey(int key) {
  return key == 1 ? 0 : key == 8 ? 12 : key == 9 ? 13 : key;
}
// The bus stop screen's input method; its pages are APT clips.
constexpr uint32_t kBusStopInput = 0x82391E88;
// The front end's carousels: Clichés (cliche_summary) and Movies (fmv_browser).
constexpr uint32_t kClichesInput = 0x82392960;
constexpr uint32_t kMoviesInput = 0x8239A7A0;
// The front end's own screen. Replay Episode is its EpisodeSelect page.
constexpr uint32_t kFrontEndInput = 0x8239F330;
// The screens' movies are laid out on a 512x448 stage stretched over the frame.
constexpr float kStageWidth = 512, kStageHeight = 448;
// A drag this far across the frame, mostly sideways, turns a carousel.
constexpr float kSwipe = .06f;
struct MenuRow {
  std::string text, variable;
  int action = 6;
  bool save_slot = false;
  Hit hit;
};

class MenuPointer final : public rex::ui::WindowInputListener, public rex::ui::WindowListener {
 public:
  MenuPointer(rex::Runtime* runtime, rex::ui::Window* window)
      : runtime_(runtime),
        window_(window),
        apt_(runtime),
        input_(dynamic_cast<rex::input::InputSystem*>(runtime->input_system())) {
    if (window_) {
      window_->AddInputListener(this, 2050);
      window_->AddListener(this);
    }
  }
  void Shutdown() {
    alive_ = active_ = false;
    g_native_menu_active = false;
    rex::input::ClearMenuButtons();
    if (input_)
      input_->SetMenuPointerActive(false);
    if (window_) {
      window_->RemoveInputListener(this);
      window_->RemoveListener(this);
    }
  }
  void Shown(const uint8_t* base, uint32_t screen) {
    Closed(screen);
    // Not every screen reports its close. Its memory is reused later, so a
    // screen only counts while it still has the class it was shown with.
    const auto table = LoadGuestU32(base, screen);
    screens_.push_back({screen, table});
    Invalidate();
    if (REXCVAR_GET(menu_pointer_log))
      REXLOG_INFO("[menu-pointer] shown {:08X} input {:08X} parent {:08X}", screen,
                  table ? LoadGuestU32(base, table + 100) : 0, LoadGuestU32(base, screen + 32));
  }
  void Closed(uint32_t screen) {
    std::erase_if(screens_, [screen](const Screen& s) { return s.address == screen; });
    if (screen_ == screen) {
      screen_ = 0;
      Invalidate();
    }
  }
  void Invalidate() {
    rex::input::ClearMenuButtons();
    carousel_ = false;
    std::lock_guard lock(mutex_);
    ++generation_;
    logged_texts_.clear();
    rows_.clear();
    footers_.clear();
    picks_.clear();
    labels_.clear();
    commands_.clear();
    pressed_ = {};
    pressed_.key = -1;
    refresh_after_ = {};
  }
  bool active() const { return alive_ && active_; }
  uint32_t screen() const { return screen_; }
  bool keyboard() const { return active() && source_ == 1; }
  bool touch() const { return active() && source_ == 2; }

  void Tick(PPCContext& ctx, uint8_t* base) {
    if (!alive_)
      return;
    TestInput();
    const bool menu = !game_ui::LevelRunning(LoadGuestU32(base, 0x82D08C98)) ||
                      LoadGuestU32(base, 0x82D572DC) == 1;
    uint32_t screen = 0;
    std::erase_if(screens_,
                  [base](const Screen& s) { return LoadGuestU32(base, s.address) != s.table; });
    if (menu) {
      unsigned deepest = 0;
      for (auto it = screens_.rbegin(); it != screens_.rend(); ++it) {
        if (LoadGuestU32(base, it->address + 28) == 1) {
          uint32_t parent = it->address;
          unsigned depth = 0;
          while (parent && depth < 32) {
            parent = LoadGuestU32(base, parent + 32);
            ++depth;
          }
          if (!screen || depth > deepest) {
            screen = it->address;
            deepest = depth;
          }
        }
      }
    }
    if (screen != screen_) {
      if (REXCVAR_GET(menu_pointer_log)) {
        const auto table = screen ? LoadGuestU32(base, screen) : 0;
        REXLOG_INFO("[menu-pointer] screen {:08X} input {:08X}, state {} mode {}", screen,
                    table ? LoadGuestU32(base, table + 100) : 0, LoadGuestU32(base, 0x82D08C98),
                    LoadGuestU32(base, 0x82D572DC));
      }
      Invalidate();
      screen_ = screen;
      logged_texts_.clear();
    }
    active_ = window_ && screen_ != 0;
    g_native_menu_active = active_.load();
    if (input_)
      input_->SetMenuPointerActive(active_);
    {
      // The title screen (input method 0x8239F330) takes a tap anywhere as Start.
      const auto table = active_ ? LoadGuestU32(base, screen_) : 0;
      title_ = game_ui::Current() != game_ui::Screen::kSaves && table &&
               LoadGuestU32(base, 0x82D08C98) <= 6 && LoadGuestU32(base, table + 100) == 0x8239F330;
    }
    if (!active_)
      return;
    const auto handle = LoadGuestU32(base, screen_ + 96);
    const auto path = handle ? LoadGuestU32(base, handle) : 0;
    if (!path)
      return;
    const std::string movie(reinterpret_cast<const char*>(GuestToHost(base, path)));
    apt_.Bind(ctx, base, movie);
    if (probe_) {
      probe_ = false;
      Probe(base, movie);
    }
    if (Clock::now() >= refresh_after_) {
      Refresh(base);
      refresh_after_ = Clock::now() + std::chrono::milliseconds(120);
    }
    std::deque<Command> commands;
    uint64_t generation;
    {
      std::lock_guard lock(mutex_);
      generation = generation_;
      commands.swap(commands_);
    }
    for (const auto& command : commands) {
      if (command.generation != generation || !screen_ || LoadGuestU32(base, screen_ + 28) != 1)
        continue;
      if (command.row >= 0 && apt_.Number("ActiveMenu.currentSelection") != command.row) {
        for (unsigned moves = 0; moves < 32; ++moves) {
          const int current = apt_.Number("ActiveMenu.currentSelection");
          if (current == command.row)
            break;
          apt_.Invoke(current < command.row ? "menuMoveDown" : "menuMoveUp");
          if (apt_.Number("ActiveMenu.currentSelection") == current)
            break;
        }
        if (apt_.Number("ActiveMenu.currentSelection") != command.row)
          continue;
      }
      if (command.pick != Pick::kNone)
        Picked(ctx, base, command);
      else if (command.key)
        Send(ctx, base, command.row, command.key);
      // An action may replace a page or close a screen. Never send the rest
      // of an old pointer gesture into the newly visible page.
      {
        std::lock_guard lock(mutex_);
        if (generation_ != generation)
          break;
      }
    }
    StepCharacter(ctx, base, generation);
    StepCarousel(ctx, base, generation);
    if (input_) {
      for (const auto& device : input_->GetDevices()) {
        if (device.kind == rex::input::DeviceKind::kKeyboard)
          continue;
        rex::input::X_INPUT_STATE state{};
        if (input_->GetDeviceState(device.id, &state) != X_ERROR_SUCCESS)
          continue;
        auto& before = buttons_[device.id];
        if (uint16_t(state.gamepad.buttons) & ~before) {
          source_ = 0;
          GameMenuSetKeyboardInput(false);
        }
        before = state.gamepad.buttons;
      }
    }
  }

  void Record(const uint8_t* base, uint32_t text) {
    if (!active() || !text)
      return;
    {
      // Button glyphs drawn next (RecordButton) belong to this text.
      const auto name = LoadGuestU32(base, text + 84);
      drawing_ = name ? reinterpret_cast<const char*>(GuestToHost(base, name)) : "";
    }
    const auto transform = LoadGuestU32(base, 0x82D08BF4);
    const auto matrix = transform ? LoadGuestU32(base, transform + 4) : 0;
    if (!matrix)
      return;
    const float x = LoadGuestFloat(base, matrix), y = LoadGuestFloat(base, matrix + 4);
    const float scale = x == 0 ? y : y == 0 ? x : std::hypot(x, y);
    const auto manager = LoadGuestU32(base, 0x82D08EA8);
    const auto camera = manager ? LoadGuestU32(base, manager + 44) : 0;
    const auto raster = camera ? LoadGuestU32(base, camera + 96) : 0;
    const auto canvas = [base, raster](uint32_t global, uint32_t raster_offset) {
      const auto value = LoadGuestU32(base, global);
      return float(value >= 320 && value <= 16384 ? value
                   : raster                       ? LoadGuestU32(base, raster + raster_offset)
                                                  : 0);
    };
    const float canvas_width = canvas(0x82E06DD4, 12);
    const float canvas_height = canvas(0x82E06DD8, 16);
    if (REXCVAR_GET(menu_pointer_log) && logged_texts_.size() < 32) {
      const auto raw = LoadGuestU32(base, text + 84);
      const std::string value = raw ? reinterpret_cast<const char*>(GuestToHost(base, raw)) : "";
      if (logged_texts_.insert(value).second)
        REXLOG_INFO(
            "[menu-pointer] text {} matrix {}, {} scale {}, canvas {}, {}, bounds {}, {}, {}, {}",
            value, x, y, scale, canvas_width, canvas_height, LoadGuestFloat(base, text + 4),
            LoadGuestFloat(base, text + 8), LoadGuestFloat(base, text + 12),
            LoadGuestFloat(base, text + 16));
    }
    if (!(scale > 0 && canvas_width > 0 && canvas_height > 0))
      return;
    canvas_width_ = canvas_width;
    canvas_height_ = canvas_height;
    Hit hit;
    hit.identity = text;
    hit.seen = Clock::now();
    hit.x =
        (LoadGuestFloat(base, matrix + 48) + LoadGuestFloat(base, text + 4) * scale) / canvas_width;
    hit.y = (LoadGuestFloat(base, matrix + 52) + LoadGuestFloat(base, text + 8) * scale) /
            canvas_height;
    hit.width =
        (LoadGuestFloat(base, text + 12) - LoadGuestFloat(base, text + 4)) * scale / canvas_width;
    hit.height =
        (LoadGuestFloat(base, text + 16) - LoadGuestFloat(base, text + 8)) * scale / canvas_height;
    if (!(hit.width > 0 && hit.height > 0) ||
        !std::isfinite(hit.x + hit.y + hit.width + hit.height))
      return;
    const auto string = [base, text](uint32_t offset) -> std::string_view {
      const auto pointer = LoadGuestU32(base, text + offset);
      return pointer ? reinterpret_cast<const char*>(GuestToHost(base, pointer)) : "";
    };
    const auto raw = string(84), localized = string(116);
    std::lock_guard lock(mutex_);
    for (size_t i = 0; i < rows_.size(); ++i) {
      const auto& row = rows_[i];
      if ((!row.text.empty() && (row.text == raw || row.text == localized)) ||
          (!row.variable.empty() && (row.variable == raw || row.variable == localized))) {
        hit.row = int(i);
        hit.key = row.action;
        // Leave a usable finger target around the rendered text.
        hit.x -= .012f;
        hit.width += .024f;
        hit.y -= .008f;
        hit.height += .016f;
        if (row.action == 3)
          hit.height = std::max(hit.height, .11f);
        if (row.save_slot)
          hit.width = std::max(hit.width, .5f);
        if (REXCVAR_GET(menu_pointer_log) && rows_[i].hit.width == 0)
          REXLOG_INFO("[menu-pointer] row {} {} at {}, {} size {}, {} (canvas {}, {})", i, row.text,
                      hit.x, hit.y, hit.width, hit.height, canvas_width, canvas_height);
        rows_[i].hit = hit;
        return;
      }
    }
    int key = 0;
    const auto value = localized.empty() ? raw : localized;
    if (value == "[RDDown]" || value == "[A]" || value == "[b=A]")
      key = 6;
    if (value == "[RDRight]" || value == "[B]" || value == "[b=B]")
      key = 7;
    if (value == "[RDLeft]" || value == "[X]" || value == "[b=X]")
      key = 8;
    if (value == "[RDUp]" || value == "[Y]" || value == "[b=Y]")
      key = 9;
    if (raw == "Press START" || raw == "PRESS START" || raw == "Press Start")
      key = 1;
    if (!key) {
      // A prompt's label ("Back", "Select") is its own text beside the button.
      if (hit.width < .3f && hit.height < .1f)
        AddLabel(hit);
      return;
    }
    hit.key = key;
    hit.x -= .035f;
    hit.width += .07f;
    hit.y -= .012f;
    hit.height += .024f;
    AddFooter(hit);
  }
  void RecordButton(float x, float y, float width, float height, int key) {
    if (!active() || !key || !(canvas_width_ > 0 && canvas_height_ > 0))
      return;
    // The bus stop map's "Navigate" shows the whole D-pad, not one direction.
    if (drawing_ == "$FE_Dpad")
      return;
    Hit hit{-1,
            key,
            x / canvas_width_ - .02f,
            y / canvas_height_ - .012f,
            width / canvas_width_ + .04f,
            height / canvas_height_ + .024f};
    if (drawing_ == "$FE_Button_EPGoalsMOHMap") {
      // The notepad's compass opens the bus stop map with Y. Its button sits
      // in the middle of the compass, and the whole compass takes the tap.
      const float cx = hit.x + hit.width * .5f, cy = hit.y + hit.height * .5f;
      hit.x = cx - .065f;
      hit.y = cy - .115f;
      hit.width = .13f;
      hit.height = .23f;
    }
    std::lock_guard lock(mutex_);
    AddFooter(hit);
  }

  void OnKeyDown(rex::ui::KeyEvent& e) override {
    if (!Accept() || e.is_handled() || GameMenuIsVisible())
      return;
    using K = rex::ui::VirtualKey;
    int key = 0;
    switch (e.virtual_key()) {
      case K::kLeft:
        key = 2;
        break;
      case K::kRight:
        key = 3;
        break;
      case K::kUp:
        key = 4;
        break;
      case K::kDown:
        key = 5;
        break;
      case K::kReturn:
      case K::kSpace:
        key = 6;
        break;
      case K::kBack:
      case K::kEscape:
        key = 7;
        break;
      default:
        break;
    }
    if (key) {
      source_ = 1;
      std::lock_guard lock(mutex_);
      commands_.push_back({-1, key, generation_});
      e.set_handled(true);
    }
  }
  void OnMouseDown(rex::ui::MouseEvent& e) override {
    if (!Accept() || e.is_handled())
      return;
    source_ = 1;
    if (e.button() == rex::ui::MouseEvent::Button::kRight) {
      std::lock_guard lock(mutex_);
      commands_.push_back({-1, 7, generation_});
    } else if (e.button() == rex::ui::MouseEvent::Button::kLeft)
      Down(float(e.x()), float(e.y()), 0);
    else
      return;
    e.set_handled(true);
  }
  void OnMouseMove(rex::ui::MouseEvent& e) override {
    if (!Accept() || e.is_handled())
      return;
    source_ = 1;
    Move(float(e.x()), float(e.y()));
    e.set_handled(true);
  }
  void OnMouseUp(rex::ui::MouseEvent& e) override {
    if (!Accept() || e.is_handled() || e.button() != rex::ui::MouseEvent::Button::kLeft)
      return;
    Up(float(e.x()), float(e.y()), 0, false);
    e.set_handled(true);
  }
  void OnTouchEvent(rex::ui::TouchEvent& e) override {
    if (!active() || e.is_handled() || rex::ui::IsOverlayInputActive())
      return;
    source_ = 2;
    using A = rex::ui::TouchEvent::Action;
    if (GameMenuCapturingInput() && e.action() == A::kDown) {
      std::lock_guard lock(mutex_);
      if (HitAt(e.x(), e.y()).key != 7) {
        e.set_handled(true);
        return;
      }
    }
    if (e.action() == A::kDown)
      Down(e.x(), e.y(), e.pointer_id());
    else if (e.action() == A::kMove)
      Move(e.x(), e.y());
    else
      Up(e.x(), e.y(), e.pointer_id(), e.action() == A::kCancel);
    e.set_handled(true);
  }
  void OnMouseWheel(rex::ui::MouseEvent& e) override {
    if (!Accept() || e.is_handled() || !e.scroll_y())
      return;
    source_ = 1;
    std::lock_guard lock(mutex_);
    const auto hit = HitAt(float(e.x()), float(e.y()));
    if (carousel_)
      commands_.push_back({-1, e.scroll_y() > 0 ? 2 : 3, generation_});
    else if (hit.row >= 0)
      commands_.push_back({hit.row, e.scroll_y() > 0 ? 3 : 2, generation_});
    else
      commands_.push_back({-1, e.scroll_y() > 0 ? 4 : 5, generation_});
    e.set_handled(true);
  }
  void OnLostFocus(rex::ui::UISetupEvent&) override {
    std::lock_guard lock(mutex_);
    pressed_.key = -1;
    commands_.clear();
  }

 private:
  // Test runs have no input device that can tap; the file stands in for one.
  void TestInput() {
    const auto& path = REXCVAR_GET(menu_pointer_test_file);
    if (path.empty() || !window_)
      return;
    FILE* file = std::fopen(path.c_str(), "r");
    if (!file)
      return;
    char op[16] = {};
    float gx = 0, gy = 0, gx2 = 0, gy2 = 0;
    const int fields = std::fscanf(file, "%15s %f %f %f %f", op, &gx, &gy, &gx2, &gy2);
    std::fclose(file);
    std::remove(path.c_str());
    if (std::string_view(op) == "probe") {
      probe_ = true;
      return;
    }
    if (fields < 3)
      return;
    const auto viewport = rex::ui::GetGuestOutputViewport();
    const auto window_x = [&](float frame) {
      return (viewport.x + frame * viewport.width) * window_->GetActualPhysicalWidth();
    };
    const auto window_y = [&](float frame) {
      return (viewport.y + frame * viewport.height) * window_->GetActualPhysicalHeight();
    };
    const float x = window_x(gx), y = window_y(gy);
    REXLOG_INFO("[menu-pointer] test {} at {}, {} (window {}, {}) active {}", op, gx, gy, x, y,
                active());
    using A = rex::ui::TouchEvent::Action;
    using B = rex::ui::MouseEvent::Button;
    if (std::string_view(op) == "swipe" && fields == 5) {
      // 'swipe X Y X2 Y2': a touch dragged from one point to the other.
      rex::ui::TouchEvent down(window_, 7, A::kDown, x, y);
      rex::ui::TouchEvent move(window_, 7, A::kMove, window_x((gx + gx2) * .5f),
                               window_y((gy + gy2) * .5f));
      rex::ui::TouchEvent up(window_, 7, A::kUp, window_x(gx2), window_y(gy2));
      OnTouchEvent(down);
      OnTouchEvent(move);
      OnTouchEvent(up);
    } else if (std::string_view(op) == "tap") {
      rex::ui::TouchEvent down(window_, 7, A::kDown, x, y), up(window_, 7, A::kUp, x, y);
      OnTouchEvent(down);
      OnTouchEvent(up);
    } else if (std::string_view(op) == "click") {
      rex::ui::MouseEvent down(window_, B::kLeft, int32_t(x), int32_t(y));
      rex::ui::MouseEvent up(window_, B::kLeft, int32_t(x), int32_t(y));
      OnMouseDown(down);
      OnMouseUp(up);
    }
  }
  // Development: logs the APT values of the paths listed next to the test
  // file, one per line ("!name" calls a function instead).
  void Probe(const uint8_t* base, const std::string& movie) {
    FILE* file = std::fopen((REXCVAR_GET(menu_pointer_test_file) + ".probe").c_str(), "r");
    if (!file)
      return;
    char line[4096];
    while (std::fgets(line, sizeof(line), file)) {
      std::string name(line);
      while (!name.empty() && std::isspace(uint8_t(name.back())))
        name.pop_back();
      if (!name.empty() && (name[0] == '!' || name[0] == '=' || name[0] == '#' || name[0] == '+')) {
        const char operation = name[0];
        const size_t divider = name.find('|');
        const std::string path =
            name.substr(1, divider == std::string::npos ? divider : divider - 1);
        std::vector<std::string> args;
        size_t start = divider;
        while (start != std::string::npos) {
          const size_t end = name.find('|', start + 1);
          args.push_back(name.substr(start + 1, end == std::string::npos ? end : end - start - 1));
          start = end;
        }
        if (operation == '!')
          apt_.Invoke(path, args);
        else if (operation == '+')
          apt_.SetArray(path, args);
        else if (operation == '=' && args.size() == 1)
          apt_.SetText(path, args[0]);
        else if (operation == '#' && args.size() == 1) {
          char* end = nullptr;
          const float value = std::strtof(args[0].c_str(), &end);
          if (end != args[0].c_str() && *end == '\0' && std::isfinite(value))
            apt_.SetReal(path, value);
        }
        REXLOG_INFO("[menu-pointer] probe {}: called {}", movie, name.substr(1));
      } else if (!name.empty()) {
        const auto value = apt_.Get(name);
        REXLOG_INFO("[menu-pointer] probe {}.{} = type {} text '{}' real {}", movie, name,
                    value ? LoadGuestU32(base, value + 4) & 0x7F : 0, apt_.Text(name),
                    apt_.Real(name));
      }
    }
    std::fclose(file);
  }

 public:
  void InputSource(int source) { source_ = source; }
  void SaveRow(float x, float y, float width, float height, int row) {
    if (!active() || !(canvas_width_ > 0 && canvas_height_ > 0))
      return;
    Hit hit;
    hit.x = x / canvas_width_;
    hit.y = y / canvas_height_;
    hit.width = width / canvas_width_;
    hit.height = height / canvas_height_;
    hit.row = row;
    hit.key = 6;
    hit.identity = 0x53570000u + uint32_t(row);
    hit.seen = Clock::now();
    std::lock_guard lock(mutex_);
    if (row >= 0 && row < int(rows_.size()))
      rows_[size_t(row)].hit = hit;
  }
  void SaveKey(float x, float y, float width, float height, int row, int column) {
    if (!active() || !game_ui::SavesKeyboard())
      return;
    // The widget supplies each whole cell, including its touch margin.
    const auto* base = runtime_->virtual_membase();
    const float cw = float(LoadGuestU32(base, 0x82E06DD4));
    const float ch = float(LoadGuestU32(base, 0x82E06DD8));
    if (!(cw > 0 && ch > 0))
      return;
    Hit hit;
    hit.x = x / cw;
    hit.y = y / ch;
    hit.width = width / cw;
    hit.height = height / ch;
    hit.row = row;
    hit.pick = Pick::kSaveKey;
    hit.index = row * 10 + column;
    hit.identity = 0x53560000u + uint32_t(hit.index);
    hit.seen = Clock::now();
    std::lock_guard lock(mutex_);
    const auto found = std::find_if(picks_.begin(), picks_.end(),
                                    [&](const Hit& old) { return old.identity == hit.identity; });
    if (found == picks_.end())
      picks_.push_back(hit);
    else
      *found = hit;
  }

 private:
  // Sends a key to the screen the way its controller would.
  void Send(PPCContext& ctx, uint8_t* base, int row, int key) {
    const auto table = LoadGuestU32(base, screen_);
    // Options installs vtable 0x82002858; its input method 0x823A5768
    // occupies +100. The neighbouring entries are screen lifecycle calls.
    const auto address = table ? LoadGuestU32(base, table + 100) : 0;
    // The title polls Start directly. Startup storage notices also run
    // through that original polling loop, outside ordinary menu input, and
    // so do the movie browser's movies.
    if (game_ui::Current() != game_ui::Screen::kSaves &&
        (LoadGuestU32(base, 0x82D08C98) <= 6 || movie_playing_)) {
      const bool title = address == 0x8239F330;
      uint16_t button = 0;
      if (key == 1 || (title && (key == 6 || key == 7)))
        button = rex::input::X_INPUT_GAMEPAD_START;
      else if (key == 6)
        button = rex::input::X_INPUT_GAMEPAD_A;
      else if (key == 7)
        button = rex::input::X_INPUT_GAMEPAD_B;
      // Saved Games polls X (Delete) and Y (Change Storage Device) too.
      else if (key == 8)
        button = rex::input::X_INPUT_GAMEPAD_X;
      else if (key == 9)
        button = rex::input::X_INPUT_GAMEPAD_Y;
      if (button) {
        rex::input::QueueMenuButton(0, button);
        return;
      }
    }
    auto* handler = runtime_->function_dispatcher()->GetFunction(address);
    if (REXCVAR_GET(menu_pointer_log))
      REXLOG_INFO("[menu-pointer] dispatch row {} key {} screen {:08X} handler {:08X}", row,
                  ScreenKey(key), screen_, address);
    if (!handler)
      return;
    const PPCContext saved = ctx;
    ctx.r1.u64 -= 512;
    StoreGuestU32(base, ctx.r1.u32, saved.r1.u32);
    ctx.r3.u64 = screen_;
    ctx.r4.u64 = input_ ? input_->GetMenuPlayer() : 0;
    ctx.r5.u64 = uint32_t(ScreenKey(key));
    handler(ctx, base);
    ctx = saved;
    GameMenuSetKeyboardInput(source_ == 1);
  }

  // The character the phone shows for the player being chosen (main or buddy).
  int ShownCharacter() {
    return apt_.Number(apt_.Number("ActiveMenu.curChar") == 1 ? "ActiveMenu.char2"
                                                              : "ActiveMenu.char1");
  }
  void Picked(PPCContext& ctx, uint8_t* base, const Command& command) {
    if (command.pick == Pick::kSaveKey) {
      const int row = command.index / 10, column = command.index % 10;
      game_ui::SavesKeyboardTap(ctx, base, row, (column + .5f) / (row == 3 ? 5.f : 10.f));
    } else if (command.pick == Pick::kBusStop) {
      // A second tap on the chosen stop takes the bus, like Select.
      if (apt_.Number("ActiveMenu.curBusStop") == command.index) {
        Send(ctx, base, -1, 6);
        return;
      }
      // The map's own D-pad step (findNextBusStop) sets curBusStop, marks
      // it with updateBusStops and plays this sound.
      apt_.SetNumber("BusStopMap.curBusStop", command.index);
      apt_.Invoke("BusStopMap.updateBusStops");
      apt_.Invoke("_level0.ui_thumbnail_swap.start");
    } else if (command.pick == Pick::kCharacter) {
      if (ShownCharacter() == command.index) {
        Send(ctx, base, -1, 6);
        return;
      }
      // The phone turns one step per Right press, skipping characters that
      // can't be chosen. StepCharacter presses until it gets there.
      const int shown = ShownCharacter();
      character_ = {command.index, shown, shown, 0, 0, {}, command.generation};
    } else if (command.pick == Pick::kStep) {
      steps_ = {command.index, 0, false, Clock::now() + std::chrono::milliseconds(800),
                command.generation};
    }
  }
  // Moves a carousel one place at a time: the next move waits until the
  // last one has shown (its position changed and its slide has stopped).
  void StepCarousel(PPCContext& ctx, uint8_t* base, uint64_t generation) {
    auto& s = steps_;
    if (!s.remaining)
      return;
    if (s.generation != generation || !screen_ || LoadGuestU32(base, screen_ + 28) != 1 ||
        carousel_position_.empty()) {
      s.remaining = 0;
      return;
    }
    const auto now = Clock::now();
    const int position = apt_.Number(carousel_position_);
    const bool still = carousel_still_.empty() || apt_.Number(carousel_still_);
    if (s.pressed && position == s.last) {
      // Nothing moved: the end of the list, or a move the screen ignored.
      if (now >= s.retry)
        s.remaining = 0;
      return;
    }
    if (!still && now < s.retry)
      return;
    Send(ctx, base, -1, s.remaining > 0 ? 3 : 2);
    s.remaining += s.remaining > 0 ? -1 : 1;
    s.pressed = true;
    s.last = position;
    s.retry = now + std::chrono::milliseconds(800);
  }
  void StepCharacter(PPCContext& ctx, uint8_t* base, uint64_t generation) {
    auto& c = character_;
    if (c.target < 0)
      return;
    if (c.generation != generation || !screen_ || LoadGuestU32(base, screen_ + 28) != 1 ||
        apt_.Number("ActiveMenu.Characters.length") != 4) {
      c.target = -1;
      return;
    }
    const int shown = ShownCharacter();
    const auto now = Clock::now();
    if (c.presses && shown == c.last && now < c.retry)
      return;  // the phone is still turning
    if (shown != c.last)
      ++c.moves;
    // Back where it started: that character can't be chosen now.
    if (shown == c.target || c.presses >= 4 || (c.moves && shown == c.start)) {
      c.target = -1;
      return;
    }
    Send(ctx, base, -1, 3);
    ++c.presses;
    c.last = shown;
    c.retry = now + std::chrono::milliseconds(400);
  }

  // A target centred on a point of the stage, sized in fractions of the frame.
  static void AddPick(std::vector<Hit>& picks, float x, float y, float width, float height,
                      Pick pick, int index, int key) {
    Hit hit;
    hit.pick = pick;
    hit.index = index;
    hit.key = key;
    hit.x = x / kStageWidth - width * .5f;
    hit.y = y / kStageHeight - height * .5f;
    hit.width = width;
    hit.height = height;
    hit.seen = Clock::now();
    // Apart from the footers' numbering.
    hit.identity = 1ull << 63 | (uint64_t(pick) << 32 | uint32_t(index + 1)) << 4 | uint32_t(key);
    if (std::isfinite(hit.x + hit.y + hit.width + hit.height) && width > 0 && height > 0)
      picks.push_back(hit);
  }

  // The bus stop map's stops and the phone's characters, from their clips.
  void BusStopPicks(std::vector<Hit>& picks) {
    const float page_x = apt_.Real("ActiveMenu._x"), page_y = apt_.Real("ActiveMenu._y");
    const float page_sx = apt_.Real("ActiveMenu._xscale") / 100;
    const float page_sy = apt_.Real("ActiveMenu._yscale") / 100;
    // Positions are in the page clip; hit sizes are fractions of the frame.
    const auto add = [&](float x, float y, float width, float height, Pick pick, int index,
                         int key) {
      AddPick(picks, page_x + x * page_sx, page_y + y * page_sy, width, height, pick, index, key);
    };
    const int stops = apt_.Number("ActiveMenu.numBusStops");
    if (stops > 0 && stops <= 64) {
      for (int i = 0; i < stops; ++i) {
        const auto marker = "ActiveMenu.marker" + std::to_string(i);
        add(apt_.Real(marker + "._x"), apt_.Real(marker + "._y"), .05f, .085f, Pick::kBusStop, i,
            0);
      }
      return;
    }
    // One player chooses a main character and a buddy (1PCharSelect). Each
    // character's clip holds its icon on the phone's arc (Still_mc) and its
    // pictures in the main and buddy circles.
    if (apt_.Number("ActiveMenu.Characters.length") != 4 ||
        !apt_.Number("ActiveMenu.bSinglePlayer"))
      return;
    float circle_x = 0, circle_y = 0;
    const bool buddy = apt_.Number("ActiveMenu.curChar") == 1;
    for (int i = 0; i < 4; ++i) {
      const auto clip = "ActiveMenu." + apt_.Text("ActiveMenu.Characters." + std::to_string(i));
      const float x = apt_.Real(clip + "._x"), y = apt_.Real(clip + "._y");
      const float scale = apt_.Real(clip + "._xscale") / 100;
      const float angle = apt_.Real(clip + "._rotation") * std::numbers::pi_v<float> / 180;
      const auto place = [&](const std::string& child, float& out_x, float& out_y) {
        const float cx = apt_.Real(clip + "." + child + "._x") * scale;
        const float cy = apt_.Real(clip + "." + child + "._y") * scale;
        out_x = x + cx * std::cos(angle) - cy * std::sin(angle);
        out_y = y + cx * std::sin(angle) + cy * std::cos(angle);
      };
      float still_x, still_y;
      place("Still_mc", still_x, still_y);
      add(still_x, still_y, .075f, .13f, Pick::kCharacter, i, 0);
      if (i == 0)
        place(buddy ? "Char_mc_blue" : "Char_mc_red", circle_x, circle_y);
    }
    // The circle of the player being chosen confirms, like Select.
    add(circle_x, circle_y, .12f, .2f, Pick::kNone, -1, 6);
  }

  // A carousel keeps its pictures in slot clips obj1... of whichever of its
  // forward and backward slides is showing. The slot drawn largest, in the
  // middle, is the current item: a tap on it sends `select` (if any), a tap
  // on another slot moves the carousel there. The faded slots at the ends lie
  // under its arrows, which are clips of the screen (`arrows`) or drawn over
  // the third slot on each side.
  void CarouselPicks(std::vector<Hit>& picks, const std::string& screen, std::string_view forward,
                     std::string_view backward, int slots, int select, bool arrows) {
    const float page_x = apt_.Real(screen + "._x"), page_y = apt_.Real(screen + "._y");
    const float page_sx = apt_.Real(screen + "._xscale") / 100;
    const float page_sy = apt_.Real(screen + "._yscale") / 100;
    if (!(page_sx > 0 && page_sy > 0))
      return;
    const auto strip =
        screen + "." +
        std::string(apt_.Number(screen + "." + std::string(forward) + "._visible") ? forward
                                                                                    : backward);
    const float strip_x = apt_.Real(strip + "._x"), strip_y = apt_.Real(strip + "._y");
    struct Slot {
      float x, y, width, height, alpha;
    };
    // Empty slots (before the first item, after the last) have no size.
    std::vector<Slot> all;
    for (int i = 1; i <= slots; ++i) {
      const auto clip = strip + ".obj" + std::to_string(i);
      const float width = apt_.Real(clip + "._width"), height = apt_.Real(clip + "._height");
      if (width > 0 && height > 0)
        all.push_back({strip_x + apt_.Real(clip + "._x"), strip_y + apt_.Real(clip + "._y"), width,
                       height,
                       apt_.Number(clip + "._visible") ? apt_.Real(clip + "._alpha") : 0.0f});
    }
    if (all.empty())
      return;
    std::sort(all.begin(), all.end(), [](const Slot& a, const Slot& b) { return a.x < b.x; });
    const int middle = int(std::max_element(all.begin(), all.end(),
                                            [](const Slot& a, const Slot& b) {
                                              return a.width * a.height < b.width * b.height;
                                            }) -
                           all.begin());
    const auto place = [&](float x, float y, float width, float height, Pick pick, int index,
                           int key) {
      AddPick(picks, page_x + x * page_sx, page_y + y * page_sy, width * page_sx / kStageWidth,
              height * page_sy / kStageHeight, pick, index, key);
    };
    for (int i = 0; i < int(all.size()); ++i) {
      const auto& slot = all[i];
      const int offset = i - middle;
      if (std::abs(offset) == 3 && !arrows)
        place(slot.x, slot.y, slot.width, slot.height, Pick::kNone, -1, offset < 0 ? 2 : 3);
      else if (slot.alpha >= 40 && (offset || select))
        place(slot.x, slot.y, slot.width, slot.height, offset ? Pick::kStep : Pick::kNone, offset,
              offset ? 0 : select);
    }
    if (arrows) {
      for (const auto& [name, key] : {std::pair{"leftArrow", 2}, std::pair{"rightArrow", 3}}) {
        const auto clip = screen + "." + name;
        if (apt_.Number(clip + "._visible"))
          AddPick(picks, page_x + apt_.Real(clip + "._x") * page_sx,
                  page_y + apt_.Real(clip + "._y") * page_sy, .07f, .14f, Pick::kNone, -1, key);
      }
    }
  }

  // Replay Episode (the front end's EpisodeSelect page) shows one episode's
  // picture between two arrows. They are unnamed shapes of its Episodes clip;
  // their places in it were measured on screen.
  void EpisodePicks(std::vector<Hit>& picks) {
    if (apt_.Number("Episodes.bLeaderboardMode"))
      return;
    const float x = apt_.Real("Episodes._x"), y = apt_.Real("Episodes._y");
    const float sx = apt_.Real("Episodes._xscale") / 100, sy = apt_.Real("Episodes._yscale") / 100;
    if (!(sx > 0 && sy > 0))
      return;
    AddPick(picks, x - 145 * sx, y - 50 * sy, .06f, .14f, Pick::kNone, -1, 2);
    AddPick(picks, x + 140 * sx, y - 49 * sy, .06f, .14f, Pick::kNone, -1, 3);
    AddPick(picks, x - 1 * sx, y - 46 * sy, 246 * sx / kStageWidth, 121 * sy / kStageHeight,
            Pick::kNone, -1, 6);
  }

  // Callers hold mutex_, as for AddFooter.
  void AddLabel(Hit hit) {
    const auto now = Clock::now();
    std::erase_if(
        labels_, [now](const Hit& old) { return now - old.seen > std::chrono::milliseconds(500); });
    hit.seen = now;
    const auto found = std::find_if(labels_.begin(), labels_.end(),
                                    [&](const Hit& old) { return old.identity == hit.identity; });
    if (found != labels_.end())
      *found = hit;
    else if (labels_.size() < 64)
      labels_.push_back(hit);
  }
  void AddFooter(Hit hit) {
    // A modal and the menu behind it can draw the same button. Keep both
    // locations and expire fields that are no longer drawn.
    const auto now = Clock::now();
    std::erase_if(footers_, [now](const Hit& old) {
      return now - old.seen > std::chrono::milliseconds(500);
    });
    const auto found = std::find_if(footers_.begin(), footers_.end(), [&](const Hit& old) {
      return old.key == hit.key &&
             std::abs(old.x + old.width * .5f - hit.x - hit.width * .5f) < .04f &&
             std::abs(old.y + old.height * .5f - hit.y - hit.height * .5f) < .04f;
    });
    hit.seen = now;
    if (found != footers_.end()) {
      hit.identity = found->identity;
      *found = hit;
    } else if (footers_.size() < 64) {
      hit.identity = ++next_hit_identity_;
      footers_.push_back(hit);
    }
  }
  bool Accept() const {
    return active() && !GameMenuCapturingInput() && !rex::ui::IsOverlayInputActive();
  }
  void Refresh(const uint8_t* base) {
    std::vector<MenuRow> rows;
    const auto table = LoadGuestU32(base, screen_);
    const auto handler = table ? LoadGuestU32(base, table + 100) : 0;
    // The common confirmation popup shares its movie with the saved-game
    // menu. Its own handler owns Yes/No; the rows underneath are inactive.
    const int count = handler == 0x823A7FC0 ? 0 : apt_.Number("ActiveMenu.MenuItemButtons.length");
    for (int i = 0; i < std::clamp(count, 0, 32); ++i) {
      const auto button = apt_.Text("ActiveMenu.MenuItemButtons." + std::to_string(i));
      const auto object = "ActiveMenu." + button;
      MenuRow row;
      row.text = apt_.Text(object + ".text_str");
      row.variable = apt_.Text(object + ".variable_text");
      const auto gizmo = apt_.Text("ActiveMenu.GizmoTypes." + std::to_string(i));
      if (gizmo == "selector" || gizmo == "slider")
        row.action = 3;
      if (row.text.empty()) {
        const auto ref = apt_.Text("ActiveMenu.TextRefs." + std::to_string(i));
        row.text = apt_.Text("ActiveMenu." + ref + ".text_entry.text");
      }
      // The save menu keeps its original placeholder in text_str while
      // replacing the displayed time. Its separate A/B/C/D fields are stable.
      if (count == 4 && row.text.starts_with("-99:") && i < 4) {
        row.variable = std::string(1, char('A' + i));
        row.save_slot = true;
      }
      rows.push_back(std::move(row));
    }
    std::vector<Hit> picks;
    // A carousel's position and whether it has stopped sliding, for steps.
    std::string position, still;
    movie_playing_ = handler == kMoviesInput && apt_.Number("FMVScreen.bMoviePlaying");
    if (handler == kBusStopInput) {
      BusStopPicks(picks);
    } else if (handler == kMoviesInput) {
      if (movie_playing_) {
        // The movie hides the touch pad; a tap anywhere stops it, like B.
        AddPick(picks, kStageWidth * .5f, kStageHeight * .5f, 1, 1, Pick::kNone, -1, 7);
      } else {
        CarouselPicks(picks, "FMVScreen", "MoviesFwd", "MoviesBkwd", 6, 6, true);
        position = "FMVScreen.FirstClipNum";
        still = "FMVScreen.bStopped";
      }
    } else if (handler == kClichesInput) {
      CarouselPicks(picks, "ClicheScreen", "ClicheFwd", "ClicheBkwd", 10, 0, false);
      position = "ClicheScreen.FirstClipNum";
      still = "ClicheScreen.bStopped";
    } else if (handler == kFrontEndInput && apt_.Text("ActiveMenu._name") == "EpisodeSelect") {
      EpisodePicks(picks);
      position = "Episodes.currentEpisode";
    }
    carousel_position_ = position;
    carousel_still_ = still;
    carousel_ = !position.empty();
    std::lock_guard lock(mutex_);
    picks_ = std::move(picks);
    const bool same =
        rows.size() == rows_.size() &&
        std::equal(rows.begin(), rows.end(), rows_.begin(), [](const MenuRow& a, const MenuRow& b) {
          return a.text == b.text && a.variable == b.variable && a.action == b.action &&
                 a.save_slot == b.save_slot;
        });
    if (!same) {
      ++generation_;
      rows_ = std::move(rows);
      pressed_.key = -1;
      footers_.clear();
      if (REXCVAR_GET(menu_pointer_log))
        REXLOG_INFO("[menu-pointer] screen {:08X}, {} rows", screen_, rows_.size());
      if (REXCVAR_GET(menu_pointer_log))
        for (const auto& row : rows_)
          REXLOG_INFO("[menu-pointer] label '{}' variable '{}'", row.text, row.variable);
    }
  }
  // Window pixels to fractions of the game image.
  bool ToFrame(float& x, float& y) const {
    const auto viewport = rex::ui::GetGuestOutputViewport();
    const float width = float(window_->GetActualPhysicalWidth());
    const float height = float(window_->GetActualPhysicalHeight());
    if (!(width > 0 && height > 0 && viewport.width > 0 && viewport.height > 0))
      return false;
    x = (x / width - viewport.x) / viewport.width;
    y = (y / height - viewport.y) / viewport.height;
    return true;
  }
  Hit HitAt(float x, float y) const {
    if (!ToFrame(x, y))
      return {};
    const auto now = Clock::now();
    const auto fresh = [now](const Hit& hit) {
      return now - hit.seen <= std::chrono::milliseconds(500) && hit.width > 0 && hit.height > 0;
    };
    const auto inside = [x, y, &fresh](const Hit& hit) {
      return fresh(hit) && x >= hit.x && y >= hit.y && x < hit.x + hit.width &&
             y < hit.y + hit.height;
    };
    // Targets can overlap, like rows drawn close together or stops on the
    // map. The one whose middle is nearest wins.
    const auto nearer = [x, y](const Hit* best, const Hit& hit) {
      const auto distance = [x, y](const Hit& h) {
        const float dx = x - h.x - h.width * .5f, dy = y - h.y - h.height * .5f;
        return dx * dx + dy * dy;
      };
      return !best || distance(hit) < distance(*best);
    };
    if (game_ui::SavesKeyboard()) {
      const Hit* key = nullptr;
      for (const auto& hit : picks_)
        if (hit.pick == Pick::kSaveKey && inside(hit) && nearer(key, hit))
          key = &hit;
      if (key)
        return *key;
    }
    const MenuRow* row = nullptr;
    for (const auto& candidate : rows_)
      if (!game_ui::SavesKeyboard() && inside(candidate.hit) &&
          nearer(row ? &row->hit : nullptr, candidate.hit))
        row = &candidate;
    if (row) {
      auto hit = row->hit;
      if (row->action == 3)
        hit.key = x < hit.x + hit.width * .5f ? 2 : 3;
      return hit;
    }
    const Hit* pick = nullptr;
    for (const auto& hit : picks_)
      if (inside(hit) && nearer(pick, hit))
        pick = &hit;
    if (pick)
      return *pick;
    for (const auto& hit : footers_)
      if (inside(hit))
        return hit;
    // A prompt's label takes the tap for the button beside it.
    for (const auto& label : labels_) {
      if (!inside(label))
        continue;
      const float middle = label.y + label.height * .5f;
      for (const auto& hit : footers_) {
        const float gap = std::max(hit.x - label.x - label.width, label.x - hit.x - hit.width);
        if (fresh(hit) && std::abs(hit.y + hit.height * .5f - middle) < .03f && gap < .05f)
          return hit;
      }
    }
    return {};
  }
  void Down(float x, float y, uint32_t id) {
    std::lock_guard lock(mutex_);
    if (pressed_.key != -1)
      return;
    pressed_ = HitAt(x, y);
    pressed_id_ = id;
    pressed_generation_ = generation_;
    down_x_ = x;
    down_y_ = y;
    ToFrame(down_x_, down_y_);
    if (REXCVAR_GET(menu_pointer_log)) {
      const auto viewport = rex::ui::GetGuestOutputViewport();
      REXLOG_INFO(
          "[menu-pointer] press {}, {} -> row {} key {} pick {} {}, window {}x{}, viewport {}, {}, "
          "{}, {}",
          x, y, pressed_.row, pressed_.key, int(pressed_.pick), pressed_.index,
          window_->GetActualPhysicalWidth(), window_->GetActualPhysicalHeight(), viewport.x,
          viewport.y, viewport.width, viewport.height);
      for (const auto& row : rows_)
        REXLOG_INFO("[menu-pointer] target row {} key {} at {}, {} size {}, {}", row.hit.row,
                    row.hit.key, row.hit.x, row.hit.y, row.hit.width, row.hit.height);
      for (const auto& hit : footers_)
        REXLOG_INFO("[menu-pointer] target key {} at {}, {} size {}, {}", hit.key, hit.x, hit.y,
                    hit.width, hit.height);
      for (const auto& hit : picks_)
        REXLOG_INFO("[menu-pointer] target pick {} {} key {} at {}, {} size {}, {}", int(hit.pick),
                    hit.index, hit.key, hit.x, hit.y, hit.width, hit.height);
    }
    if (pressed_.row >= 0)
      commands_.push_back({pressed_.row, 0, generation_});
  }
  void Move(float x, float y) {
    std::lock_guard lock(mutex_);
    if (pressed_.key != -1)
      return;
    const auto hit = HitAt(x, y);
    if (hit.row >= 0) {
      if (!commands_.empty() && commands_.back().key == 0)
        commands_.back() = {hit.row, 0, generation_};
      else
        commands_.push_back({hit.row, 0, generation_});
    }
  }
  void Up(float x, float y, uint32_t id, bool cancel) {
    std::lock_guard lock(mutex_);
    if (pressed_.key == -1 || pressed_id_ != id)
      return;
    // A sideways swipe turns a carousel; swiping left brings in the next item.
    float frame_x = x, frame_y = y;
    if (carousel_ && !cancel && pressed_generation_ == generation_ && ToFrame(frame_x, frame_y) &&
        std::abs(frame_x - down_x_) > kSwipe &&
        std::abs(frame_x - down_x_) > 2 * std::abs(frame_y - down_y_)) {
      commands_.push_back({-1, frame_x < down_x_ ? 3 : 2, generation_});
      pressed_.key = -1;
      return;
    }
    const auto hit = HitAt(x, y);
    if (!cancel && pressed_generation_ == generation_ && hit.identity == pressed_.identity &&
        hit.row == pressed_.row && hit.key == pressed_.key && (hit.key || hit.pick != Pick::kNone))
      commands_.push_back({hit.row, hit.key, generation_, hit.pick, hit.index});
    else if (!cancel && title_ && source_ == 2 && pressed_generation_ == generation_ &&
             !pressed_.key && pressed_.row < 0 && pressed_.pick == Pick::kNone && !hit.key &&
             hit.row < 0)
      commands_.push_back({-1, 1, generation_});
    pressed_.key = -1;
  }

  rex::Runtime* runtime_;
  rex::ui::Window* window_;
  simpsons::GameMenuApt apt_;
  rex::input::InputSystem* input_;
  std::atomic<bool> alive_{true}, active_{false}, title_{false};
  bool probe_ = false;
  std::atomic<int> source_{0};
  std::vector<Screen> screens_;
  uint32_t screen_ = 0;
  float canvas_width_ = 1280, canvas_height_ = 720;
  // The text being drawn (its untranslated name), for its button glyphs.
  std::string_view drawing_;
  // A character picked on the phone, reached one Right press at a time.
  struct {
    int target = -1, start = -1, last = -1, presses = 0, moves = 0;
    Clock::time_point retry{};
    uint64_t generation = 0;
  } character_;
  // The carousel on screen (game thread): APT paths of its position and of
  // whether it has stopped sliding.
  std::string carousel_position_, carousel_still_;
  std::atomic<bool> carousel_{false};
  // A movie of the movie browser is playing; it reads the controller itself.
  bool movie_playing_ = false;
  // Moves still to make towards a tapped picture (negative: left).
  struct {
    int remaining = 0, last = 0;
    bool pressed = false;
    Clock::time_point retry{};
    uint64_t generation = 0;
  } steps_;
  std::mutex mutex_;
  std::vector<MenuRow> rows_;
  std::vector<Hit> footers_, picks_, labels_;
  std::deque<Command> commands_;
  std::map<std::string, uint16_t> buttons_;
  std::set<std::string> logged_texts_;
  uint64_t generation_ = 0, pressed_generation_ = 0;
  uint64_t next_hit_identity_ = 0;
  uint32_t pressed_id_ = 0;
  Hit pressed_{-1, -1};
  // Where the press began, in fractions of the game image.
  float down_x_ = 0, down_y_ = 0;
  Clock::time_point refresh_after_{};
};

std::unique_ptr<MenuPointer> g_pointer;
}  // namespace

void InitGameMenuPointer(rex::Runtime* runtime, rex::ui::Window* window) {
  g_pointer = std::make_unique<MenuPointer>(runtime, window);
}
void ShutdownGameMenuPointer() {
  if (g_pointer)
    g_pointer->Shutdown();
}
void TickGameMenuPointer(PPCContext& ctx, uint8_t* base) {
  if (g_pointer)
    g_pointer->Tick(ctx, base);
}
void RecordGameMenuPointerSaveRow(float x, float y, float width, float height, int row) {
  if (g_pointer)
    g_pointer->SaveRow(x, y, width, height, row);
}
void RecordGameMenuPointerSaveKey(float x, float y, float width, float height, int row,
                                  int column) {
  if (g_pointer)
    g_pointer->SaveKey(x, y, width, height, row, column);
}
void RecordGameMenuPointerText(const uint8_t* base, uint32_t text) {
  if (g_pointer)
    g_pointer->Record(base, text);
}
void RecordGameMenuPointerButton(float x, float y, float width, float height, int key) {
  if (g_pointer)
    g_pointer->RecordButton(x, y, width, height, key);
}
void GameMenuPointerScreenClosed(uint32_t screen) {
  if (g_pointer)
    g_pointer->Closed(screen);
}
void InvalidateGameMenuPointer() {
  if (g_pointer)
    g_pointer->Invalidate();
}
bool GameMenuPointerActive() {
  return g_native_menu_active.load();
}
void GameMenuPointerInputSource(int source) {
  if (g_pointer)
    g_pointer->InputSource(source);
}
bool GameMenuPointerUsesKeyboard() {
  return g_pointer && g_pointer->keyboard();
}
bool GameMenuPointerUsesTouch() {
  return g_pointer && g_pointer->touch();
}
uint32_t GameMenuPointerScreen() {
  return g_pointer ? g_pointer->screen() : 0;
}

REX_FUNC(sub_827F2DC8) {
  const auto screen = ctx.r3.u32;
  __imp__sub_827F2DC8(ctx, base);
  if (g_pointer && LoadGuestU32(base, screen + 28) == 1)
    g_pointer->Shown(base, screen);
}
