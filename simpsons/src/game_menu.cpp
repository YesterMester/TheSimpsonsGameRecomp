// Recomp settings and player assignment in the original Options movie. The front
// end and pause menu share it, including its fonts, buttons and key listener.

#include "game_menu.h"
#include "host_memory.h"
#include "game_menu_apt.h"
#include "game_menu_pointer.h"
#include "guest_memory.h"
#include "game_ui.h"
#include "modkit_game.h"
#include "localization.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <rex/cvar.h>
#include <rex/graphics/command_processor.h>
#include <rex/graphics/graphics_system.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/ppc.h>
#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/overlay_input.h>
#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>

REXCVAR_DEFINE_BOOL(in_game_settings, true, "Game", "Recomp settings and player assignment in Options")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REX_EXTERN(__imp__sub_823A4890);
REX_EXTERN(sub_823A4890);
REX_EXTERN(__imp__sub_823A4850);
REX_EXTERN(sub_823A4850);
REX_EXTERN(__imp__sub_823A4C90);
REX_EXTERN(sub_823A4C90);
REX_EXTERN(__imp__sub_823A5768);
REX_EXTERN(sub_823A5768);
REX_EXTERN(__imp__sub_827F3780);
REX_EXTERN(sub_827F3780);

namespace {
using rex::X_RESULT;
using rex::input::InputDevice;
using rex::input::InputSystem;
using simpsons::GuestToHost;
using simpsons::LoadGuestU32;
using simpsons::StoreGuestU32;
using Clock = std::chrono::steady_clock;

constexpr uint32_t kOptionsScreen = 0x82D08DDC;
constexpr uint32_t kOptionsState = 0x82D08DE0;
constexpr size_t kRowsPerPage = 4;

enum class Apply { kRestart, kGame, kInput, kPresenter, kSwap, kGpu, kWindow, kAudio };
enum class Page {
  kRoot,
  kPC,
  kGraphics,
  kDisplay,
  kAudio,
  kControls,
  kBindings,
  kPlayers,
  kPlayer1,
  kPlayer2,
  kCharacters,
  kColour,
  kExtras,
  kSaves,
  kMods,
  kModPage
};
enum class Action { kNone, kPage, kNative, kAssign, kListen, kBind, kMore, kScreen, kModOpen, kModAction };
struct Choice {
  std::string value, label;
  bool operator==(const Choice&) const = default;
};
struct Row {
  std::string label, name;
  std::vector<Choice> choices;
  Apply apply = Apply::kRestart;
  Action action = Action::kNone;
  Page page = Page::kRoot;
  game_ui::Screen screen = game_ui::Screen::kNone;
  float low = 0, high = 0, step = 0;
  std::string mod_page, mod_row, mod_type;
  simpsons::modkit::Json mod_value;
  bool operator==(const Row&) const = default;
};

std::vector<Choice> Choices(std::string_view list) {
  std::vector<Choice> choices;
  while (!list.empty()) {
    const size_t end = list.find(';');
    const auto item = list.substr(0, end);
    const size_t divider = item.find('|');
    choices.push_back(
        {std::string(item.substr(0, divider)),
         std::string(divider == std::string_view::npos ? item : item.substr(divider + 1))});
    if (end == std::string_view::npos)
      break;
    list.remove_prefix(end + 1);
  }
  return choices;
}

class GameMenu final : public rex::ui::WindowInputListener {
 public:
  GameMenu(rex::Runtime* runtime, rex::ui::Window* window, std::filesystem::path config)
      : runtime_(runtime),
        window_(window),
        config_(std::move(config)),
        apt_(runtime),
        input_(dynamic_cast<InputSystem*>(runtime->input_system())),
        graphics_(dynamic_cast<rex::graphics::GraphicsSystem*>(runtime->graphics_system())) {
    title_ = host_memory::Alloc(256);
    if (window_)
      window_->AddInputListener(this, 2048);
  }

  void Shutdown() {
    if (!alive_->exchange(false))
      return;
    visible_ = false;
    if (input_)
      input_->SetMenuInputCapture(false);
    if (window_)
      window_->RemoveInputListener(this);
  }

  void Enter(std::string movie) {
    if (!alive_->load())
      return;
    movie_ = std::move(movie);
    page_ = Page::kRoot;
    controller_player_ = input_ ? input_->GetLastActivePlayer() : 0;
    offset_ = 0;
    // The Options timeline creates its references on the next APT update.
    rebuild_after_ = 2;
    visible_ = true;
  }
  void Leave() {
    if (!alive_->load())
      return;
    visible_ = false;
    CancelCapture();
    Save();
    rebuild_after_ = 0;
  }

  void Tick(PPCContext& ctx, uint8_t* base) {
    if (!alive_->load())
      return;
    if (!visible_ || !REXCVAR_GET(in_game_settings))
      return;
    apt_.Bind(ctx, base, movie_);
    const auto screen = LoadGuestU32(base, kOptionsScreen);
    const auto state = LoadGuestU32(base, kOptionsState);
    if (!screen || LoadGuestU32(base, screen + 28) != 1) {
      Leave();
      return;
    }
    if (state == 0)
      return;
    if (state == 1 && rebuild_after_ && --rebuild_after_ == 0)
      Rebuild(apt_.Number("Options.currentSelection"));
    std::deque<int> keys;
    std::string binding;
    {
      std::lock_guard lock(key_mutex_);
      keys.swap(keys_);
      binding.swap(pending_binding_);
    }
    if (!binding.empty()) {
      if (binding == "Escape")
        CancelCapture();
      else if (listening_player_ >= 0) {
        controller_player_ = uint32_t(listening_player_);
        Assign(controller_player_, "keyboard");
      } else if (!capturing_.empty()) {
        Change(capturing_, binding, Apply::kInput);
        CancelCapture();
      }
      Rebuild();
    }
    if (listening_player_ >= 0)
      PollAssignment();
    for (const int key : keys) {
      if (Event(ctx, base, key))
        continue;
      const PPCContext saved = ctx;
      ctx.r3.u64 = screen;
      ctx.r4.u64 = input_ ? input_->GetMenuPlayer() : 0;
      ctx.r5.u64 = uint32_t(key);
      __imp__sub_823A5768(ctx, base);
      ctx = saved;
    }
    if (state == 1 && !rebuild_after_ && (page_ == Page::kMods || page_ == Page::kModPage) &&
        Clock::now() >= next_mod_refresh_) {
      // Source loading, storage and mod callbacks can finish without another
      // button press. Check changed rows at a modest rate; rebuilding an
      // unchanged APT page every frame would restart its native animations.
      next_mod_refresh_ = Clock::now() + std::chrono::milliseconds(250);
      const auto selection = apt_.Number("Options.currentSelection");
      auto previous = std::move(rows_);
      const auto previous_title = mod_title_;
      MakeRows();
      if (rows_ != previous || mod_title_ != previous_title)
        Rebuild(selection);
    }
    if (unsaved_ && Clock::now() - last_change_ > std::chrono::milliseconds(600))
      Save();
  }

  bool Event(PPCContext& ctx, uint8_t* base, uint32_t key) {
    if (!alive_->load() || !visible_ || !REXCVAR_GET(in_game_settings) ||
        LoadGuestU32(base, kOptionsState) != 1)
      return false;
    apt_.Bind(ctx, base, movie_);
    if (rebuild_after_)
      return true;
    if (listening_player_ >= 0 || !capturing_.empty()) {
      if (key == 7) {
        CancelCapture();
        Rebuild();
      }
      return true;
    }
    const int selection = apt_.Number("Options.currentSelection");
    if (key == 4 || key == 5) {
      apt_.Invoke(key == 4 ? "menuMoveUp" : "menuMoveDown");
      return true;
    }
    if (key == 7) {
      if (page_ == Page::kRoot) {
        Leave();
        return false;
      }
      Open(Parent(page_));
      return true;
    }
    if (selection < 0 || size_t(selection) >= shown_.size())
      return true;
    const Row row = shown_[size_t(selection)];
    if (key == 6 && row.action == Action::kNative)
      return false;
    if (key != 2 && key != 3 && key != 6)
      return true;
    if (row.action == Action::kPage) {
      if (key == 6)
        Open(row.page);
    } else if (row.action == Action::kScreen) {
      if (key == 6)
        game_ui::Open(row.screen);
    } else if (row.action == Action::kMore) {
      if (key == 6) {
        offset_ = offset_ + kRowsPerPage >= rows_.size() ? 0 : offset_ + kRowsPerPage;
        Rebuild();
      }
    } else if (row.action == Action::kModOpen && key == 6) {
      mod_page_ = row.mod_page;
      Open(Page::kModPage);
    } else if (row.action == Action::kModAction) {
      auto value = row.mod_value;
      if (row.mod_type == "action") {
        if (key != 6) return true;
        value = nullptr;
      } else if (row.mod_type == "toggle") {
        value = !value.get<bool>();
      } else if (row.mod_type == "slider") {
        value = std::clamp(value.get<double>() + (key == 2 ? -row.step : row.step),
                           double(row.low), double(row.high));
      } else if (row.mod_type == "select") {
        const auto selected = std::find_if(row.choices.begin(), row.choices.end(), [&](const auto& c) {
          return c.value == value.dump();
        });
        int index = selected == row.choices.end() ? 0 : int(selected - row.choices.begin());
        index = (index + (key == 2 ? -1 : 1) + int(row.choices.size())) % int(row.choices.size());
        value = simpsons::modkit::Json::parse(row.choices[size_t(index)].value);
      } else return true;
      modkit_game::Activate(row.mod_page, row.mod_row, value, controller_player_);
      // The callback runs at the next frame boundary and may replace its page.
      rebuild_after_ = 2;
    } else if (row.action == Action::kListen && key == 6) {
      listening_player_ = page_ == Page::kPlayer2 ? 1 : 0;
      listening_ready_ = false;
      listening_buttons_.clear();
      input_->SetMenuInputCapture(true);
      capture_active_ = true;
      Rebuild();
    } else if (row.action == Action::kBind && key == 6) {
      capturing_ = row.name;
      input_->SetMenuInputCapture(true);
      capture_active_ = true;
      Rebuild();
    } else if (row.action == Action::kAssign) {
      const uint32_t player = page_ == Page::kPlayer2 ? 1 : 0;
      const auto value = Next(row, input_->GetAssignment(player), key == 2 ? -1 : 1);
      Assign(player, value);
      Rebuild(selection);
    } else if (!row.name.empty()) {
      std::string value;
      if (row.step > 0) {
        const float current = std::strtof(Value(row.name).c_str(), nullptr);
        const float next =
            std::clamp(current + (key == 2 ? -row.step : row.step), row.low, row.high);
        value = std::to_string(next);
      } else
        value = Next(row, Value(row.name), key == 2 ? -1 : 1);
      Change(row.name, value, row.apply);
      Rebuild(selection);
    }
    return true;
  }

  uint32_t TitleForText(const uint8_t* base, uint32_t text) const {
    if (!alive_->load() || !visible_ || page_ == Page::kRoot || !title_ || !text ||
        LoadGuestU32(base, kOptionsState) != 1)
      return 0;
    const auto raw = LoadGuestU32(base, text + 84);
    if (!raw)
      return 0;
    const auto* original = reinterpret_cast<const char*>(GuestToHost(base, raw));
    return std::strcmp(original, "Options") == 0 || std::strcmp(original, "$FE_Options") == 0 ||
                   std::strcmp(original, "-Options-") == 0
               ? title_
               : 0;
  }

  bool UsesKeyboard() const { return alive_->load() && visible_ && keyboard_used_; }
  bool CapturingInput() const { return alive_->load() && visible_ && capture_active_; }
  void SetKeyboardInput(bool keyboard) { keyboard_used_ = keyboard; }
  void ControllerActivity(uint32_t player) {
    keyboard_used_ = false;
    controller_player_ = std::min(player, 1u);
  }
  bool visible() const { return alive_->load() && visible_; }
  uint32_t prompt_player() const { return controller_player_; }

  void OnKeyDown(rex::ui::KeyEvent& e) override {
    if (!visible_ || !alive_->load() || e.is_handled() || rex::ui::IsOverlayInputActive())
      return;
    keyboard_used_ = true;
    std::lock_guard lock(key_mutex_);
    if (capture_active_) {
      if (!e.prev_state())
        pending_binding_ = rex::ui::VirtualKeyToString(e.virtual_key());
      e.set_handled(true);
      return;
    }
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
      case K::kSpace:
      case K::kReturn:
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
      keys_.push_back(key);
      e.set_handled(true);
    }
  }
  void OnMouseDown(rex::ui::MouseEvent& e) override {
    if (!visible_ || !alive_->load() || e.is_handled() || rex::ui::IsOverlayInputActive())
      return;
    using B = rex::ui::MouseEvent::Button;
    if (!capture_active_) {
      if (e.button() == B::kRight) {
        keyboard_used_ = true;
        std::lock_guard lock(key_mutex_);
        keys_.push_back(7);
        e.set_handled(true);
      }
      return;
    }
    keyboard_used_ = true;
    const char* key = e.button() == B::kLeft     ? "LMB"
                      : e.button() == B::kRight  ? "RMB"
                      : e.button() == B::kMiddle ? "MMB"
                      : e.button() == B::kX1     ? "Mouse4"
                                                 : "Mouse5";
    std::lock_guard lock(key_mutex_);
    pending_binding_ = key;
    e.set_handled(true);
  }
  void OnMouseWheel(rex::ui::MouseEvent& e) override {
    if (!capture_active_ || !visible_ || !alive_->load() || !e.scroll_y() ||
        rex::ui::IsOverlayInputActive())
      return;
    std::lock_guard lock(key_mutex_);
    pending_binding_ = e.scroll_y() > 0 ? "WheelUp" : "WheelDown";
    e.set_handled(true);
  }

 private:
  std::string Value(const std::string& name) const {
    const auto saved = changed_.find(name);
    return saved == changed_.end() ? rex::cvar::GetFlagByName(name) : saved->second;
  }
  void Save() {
    if (!unsaved_)
      return;
    if (rex::cvar::SaveConfigPatch(config_, pending_save_)) {
      pending_save_.clear();
      unsaved_ = false;
    } else
      REXLOG_ERROR("[game-menu] Could not save settings to {}", config_.string());
  }
  void Change(const std::string& name, const std::string& value, Apply apply) {
    bool accepted = true;
    if (apply == Apply::kInput && input_)
      accepted = input_->SetInputSetting(name, value);
    else if (apply == Apply::kGpu && graphics_) {
      graphics_->command_processor()->CallInThread(
          [name, value] { rex::cvar::SetFlagByName(name, value); });
    } else if (apply == Apply::kSwap && graphics_) {
      accepted = rex::cvar::SetFlagByName(name, value);
      using CP = rex::graphics::CommandProcessor;
      graphics_->command_processor()->SetDesiredSwapPostEffect(
          value == "fxaa"           ? CP::SwapPostEffect::kFxaa
          : value == "fxaa_extreme" ? CP::SwapPostEffect::kFxaaExtreme
                                    : CP::SwapPostEffect::kNone);
    } else if (apply == Apply::kPresenter && graphics_) {
      window_->app_context().CallInUIThread([graphics = graphics_, alive = alive_, name, value] {
        if (alive->load() && rex::cvar::SetFlagByName(name, value))
          graphics->presenter()->ApplyGuestOutputSettingsFromUIThread();
      });
    } else if (apply == Apply::kWindow) {
      window_->app_context().CallInUIThread([window = window_, alive = alive_, name, value] {
        if (!alive->load())
          return;
        rex::cvar::SetFlagByName(name, value);
        window->SetFullscreen(value == "true");
      });
    } else if (apply != Apply::kRestart)
      accepted = rex::cvar::SetFlagByName(name, value);
    if (!accepted)
      return;
    changed_[name] = pending_save_[name] = value;
    if (name == "vsync") {
      // Match the launcher's fallback policy on the next swapchain creation.
      const std::string allow_tearing = value == "true" ? "false" : "true";
      changed_["vulkan_allow_present_mode_immediate"] =
          pending_save_["vulkan_allow_present_mode_immediate"] = allow_tearing;
      changed_["vulkan_allow_present_mode_fifo_relaxed"] =
          pending_save_["vulkan_allow_present_mode_fifo_relaxed"] = allow_tearing;
    }
    unsaved_ = true;
    last_change_ = Clock::now();
    Save();
    REXLOG_INFO("[game-menu] {} = {}{}", name, value,
                apply == Apply::kRestart ? " (restart required)" : "");
  }
  void Assign(uint32_t player, const std::string& id) {
    if (!input_->AssignDevice(player, id))
      return;
    for (uint32_t i = 0; i < 4; ++i) {
      const std::string name = "player" + std::to_string(i + 1) + "_device";
      changed_[name] = pending_save_[name] = input_->GetAssignment(i);
    }
    for (const auto* name : {"mnk_mode", "mnk_user_index"})
      changed_[name] = pending_save_[name] = rex::cvar::GetFlagByName(name);
    unsaved_ = true;
    last_change_ = Clock::now();
    Save();
    CancelCapture();
  }
  void CancelCapture() {
    capturing_.clear();
    listening_player_ = -1;
    capture_active_ = false;
    if (input_)
      input_->SetMenuInputCapture(false);
  }
  void PollAssignment() {
    const auto devices = input_->GetDevices();
    bool neutral = true;
    for (const auto& device : devices) {
      rex::input::X_INPUT_STATE state{};
      if (input_->GetDeviceState(device.id, &state) != X_ERROR_SUCCESS)
        continue;
      const uint16_t buttons = state.gamepad.buttons;
      neutral &= buttons == 0;
      auto& before = listening_buttons_[device.id];
      const bool pressed = (buttons & ~before) != 0;
      before = buttons;
      if (listening_ready_ && pressed) {
        controller_player_ = uint32_t(listening_player_);
        keyboard_used_ = device.kind == rex::input::DeviceKind::kKeyboard;
        Assign(controller_player_, device.id);
        Rebuild();
        return;
      }
    }
    if (neutral)
      listening_ready_ = true;
  }
  static std::string Next(const Row& row, const std::string& value, int direction) {
    if (row.choices.empty())
      return value;
    auto found = std::find_if(row.choices.begin(), row.choices.end(),
                              [&](const Choice& choice) { return choice.value == value; });
    int index = found == row.choices.end() ? 0 : int(found - row.choices.begin());
    index = (index + direction + int(row.choices.size())) % int(row.choices.size());
    return row.choices[size_t(index)].value;
  }
  static Page Parent(Page page) {
    if (page == Page::kMods) return Page::kExtras;
    if (page == Page::kModPage) return Page::kMods;
    if (page == Page::kPC || page == Page::kPlayers || page == Page::kExtras)
      return Page::kRoot;
    if (page == Page::kPlayer1 || page == Page::kPlayer2)
      return Page::kPlayers;
    if (page == Page::kBindings)
      return Page::kControls;
    if (page == Page::kCharacters || page == Page::kColour)
      return Page::kGraphics;
    return Page::kPC;
  }
  void Open(Page page) {
    page_ = page;
    offset_ = 0;
    Rebuild();
  }
  void Link(const char* label, Page page) {
    Row row;
    row.label = label;
    row.action = Action::kPage;
    row.page = page;
    rows_.push_back(std::move(row));
  }
  void ScreenLink(const char* label, game_ui::Screen screen) {
    Row row;
    row.label = label;
    row.action = Action::kScreen;
    row.screen = screen;
    rows_.push_back(std::move(row));
  }
  void Native(const char* label) {
    Row row;
    row.label = row.name = label;
    row.action = Action::kNative;
    rows_.push_back(std::move(row));
  }
  void Select(const char* label, const char* name, std::string_view choices, Apply apply) {
    Row row;
    row.label = label;
    row.name = name;
    row.choices = Choices(choices);
    row.apply = apply;
    rows_.push_back(std::move(row));
  }
  void Toggle(const char* label, const char* name, Apply apply) {
    Select(label, name, "false|Off;true|On", apply);
  }
  void Slider(const char* label, const char* name, float low, float high, float step, Apply apply) {
    Row row;
    row.label = label;
    row.name = name;
    row.low = low;
    row.high = high;
    row.step = step;
    row.apply = apply;
    rows_.push_back(std::move(row));
  }
  void MakeRows() {
    rows_.clear();
    switch (page_) {
      case Page::kRoot:
        Native("Audio");
        Link("Recomp settings", Page::kPC);
        Native("Controls");
        Link("Players", Page::kPlayers);
        Link("Extras", Page::kExtras);
        break;
      case Page::kExtras:
        if (modkit_game::Active()) Link("Mods", Page::kMods);
        Toggle("Skip cutscenes", "cutscene_skip", Apply::kGame);
        // Both are about the save being played, so only in the game.
        if (game_ui::InGame()) {
          ScreenLink("Achievements", game_ui::Screen::kAchievements);
          ScreenLink("Debug menu", game_ui::Screen::kDebugMenu);
        }
        Native("Downloads");
        Native("Credits");
        break;
      case Page::kMods:
        for (const auto& page : modkit_game::Pages()) {
          Row row;
          row.label = simpsons::modkit::MenuText(page.title);
          row.action = Action::kModOpen;
          row.mod_page = page.owner + ":" + page.id;
          rows_.push_back(std::move(row));
        }
        if (rows_.empty()) {
          Row row;
          row.label = "No mod settings pages";
          rows_.push_back(std::move(row));
        }
        break;
      case Page::kModPage:
        mod_title_ = "Mod settings";
        for (const auto& page : modkit_game::Pages()) {
          if (page.owner + ":" + page.id != mod_page_) continue;
          mod_title_ = simpsons::modkit::MenuText(page.title);
          for (const auto& item : page.rows) {
            Row row;
            row.label = simpsons::modkit::MenuText(item.label);
            row.mod_page = mod_page_;
            row.mod_row = item.id;
            row.mod_type = item.type;
            row.mod_value = item.value;
            row.action = item.type == "text" ? Action::kNone : Action::kModAction;
            row.low = float(item.minimum); row.high = float(item.maximum); row.step = float(item.step);
            for (const auto& choice : item.choices)
              row.choices.push_back({choice.at("value").dump(), simpsons::modkit::MenuText(choice.at("label").get<std::string>())});
            rows_.push_back(std::move(row));
          }
        }
        if (rows_.empty()) {
          Row row;
          row.label = "This page has closed";
          rows_.push_back(std::move(row));
        }
        break;
      case Page::kPC:
        Link("Graphics", Page::kGraphics);
        Link("Display", Page::kDisplay);
        Link("Sound", Page::kAudio);
#if !REX_PLATFORM_ANDROID
        Link("Keyboard and mouse", Page::kControls);
#endif
        Link("Players", Page::kPlayers);
        break;
      case Page::kSaves:
        break;
      case Page::kGraphics:
#if REX_PLATFORM_WIN32
        Select("Renderer", "gpu",
               "|Automatic;vulkan|Vulkan;d3d12|Direct3D 12;d3d11|Direct3D 11 (experimental)",
               Apply::kRestart);
#endif
        Select("Render resolution", "resolution_scale", "1|1x;2|2x;3|3x;4|4x", Apply::kRestart);
        Toggle("GPU API calls", "native_gpu_calls", Apply::kRestart);
        Select("Texture filtering", "anisotropic_override",
               "-1|Original;0|Off;2|2x;3|4x;4|8x;5|16x", Apply::kGpu);
        Select("Anti-aliasing", "swap_post_effect", "none|Off;fxaa|FXAA;fxaa_extreme|FXAA extreme",
               Apply::kSwap);
        Select("Image scaling", "present_effect", "bilinear|Bilinear;cas|CAS;fsr|FSR 1",
               Apply::kPresenter);
        Slider("CAS sharpness", "present_cas_additional_sharpness", 0, 1, .05f, Apply::kPresenter);
        Slider("FSR softness", "present_fsr_sharpness_reduction", 0, 2, .05f, Apply::kPresenter);
        if (graphics_ && graphics_->name().find("Vulkan") != std::string::npos) {
          Link("Characters", Page::kCharacters);
          Link("Colour", Page::kColour);
        }
        break;
      case Page::kDisplay:
#if !REX_PLATFORM_ANDROID
        Toggle("Fullscreen", "fullscreen", Apply::kWindow);
#endif
        Select("Frame rate", "frame_rate",
               "30|30;60|60;90|90;120|120;144|144;165|165;240|240;0|Unlimited", Apply::kGame);
        Select("Menu frame rate", "menu_frame_rate", "30|30;60|60;90|90;120|120;0|Match gameplay",
               Apply::kGame);
        Toggle("V-sync", "vsync", Apply::kRestart);
        Toggle("Keep image proportions", "present_letterbox", Apply::kPresenter);
        break;
      case Page::kAudio:
        Toggle("Mute audio", "audio_mute", Apply::kAudio);
        Select("Audio buffer", "audio_maxqframes", "16|Low latency;32|Normal;64|Large",
               Apply::kRestart);
        Toggle("Always show subtitles", "subtitles", Apply::kGame);
        break;
      case Page::kCharacters:
        Select("Character smoothing", "character_tessellation",
               "off|Off;low|Low;medium|Medium;high|High", Apply::kRestart);
        Select("Ink outlines", "ink_outlines", "original|Original;soft|Soft;off|Off",
               Apply::kRestart);
        Slider("Outline strength", "ink_outline_strength", 0, 1, .05f, Apply::kRestart);
        Select("Eye shading", "eye_shading", "clean|Clean;original|Original", Apply::kRestart);
        break;
      case Page::kColour:
        Slider("Saturation", "color_saturation", 0, 2, .05f, Apply::kRestart);
        Slider("Vibrance", "color_vibrance", -1, 1, .05f, Apply::kRestart);
        Slider("Contrast", "color_contrast", .5f, 1.5f, .05f, Apply::kRestart);
        Slider("Brightness", "color_brightness", -.25f, .25f, .025f, Apply::kRestart);
        break;
      case Page::kControls:
        Toggle("Keyboard and mouse", "mnk_mode", Apply::kInput);
        Slider("Mouse sensitivity", "mnk_sensitivity", .05f, 5, .05f, Apply::kInput);
        Toggle("Invert mouse Y", "mnk_invert_y", Apply::kInput);
        Link("Key bindings", Page::kBindings);
        break;
      case Page::kBindings: {
        constexpr std::pair<const char*, const char*> bindings[] = {
            {"Move forward", "keybind_lstick_up"},
            {"Move back", "keybind_lstick_down"},
            {"Move left", "keybind_lstick_left"},
            {"Move right", "keybind_lstick_right"},
            {"Walk", "keybind_walk"},
            {"Jump / confirm", "keybind_a"},
            {"Special / back", "keybind_b"},
            {"Attack", "keybind_x"},
            {"Action", "keybind_y"},
            {"Left trigger", "keybind_left_trigger"},
            {"Right trigger", "keybind_right_trigger"},
            {"Left shoulder", "keybind_left_shoulder"},
            {"Right shoulder", "keybind_right_shoulder"},
            {"To-do list", "keybind_back"},
            {"Pause", "keybind_start"},
            {"D-pad up", "keybind_dpad_up"},
            {"D-pad down", "keybind_dpad_down"},
            {"D-pad left", "keybind_dpad_left"},
            {"D-pad right", "keybind_dpad_right"},
            {"Left stick click", "keybind_lstick_press"},
            {"Right stick click", "keybind_rstick_press"},
            {"Look up", "keybind_rstick_up"},
            {"Look down", "keybind_rstick_down"},
            {"Look left", "keybind_rstick_left"},
            {"Look right", "keybind_rstick_right"},
        };
        for (const auto& [label, name] : bindings) {
          Row row;
          row.label = label;
          row.name = name;
          row.action = Action::kBind;
          rows_.push_back(std::move(row));
        }
        break;
      }
      case Page::kPlayers:
        Link("Player 1", Page::kPlayer1);
        Link("Player 2", Page::kPlayer2);
        Toggle("Two players in Springfield", "freeroam_coop", Apply::kRestart);
        break;
      case Page::kPlayer1:
      case Page::kPlayer2: {
        const uint32_t player = page_ == Page::kPlayer2 ? 1 : 0;
        Row row;
        row.label = "Input";
        row.name = "player" + std::to_string(player + 1) + "_device";
        row.action = Action::kAssign;
        row.choices = {{"auto", "Automatic"}, {"none", "Not assigned"}};
        for (const auto& device : input_->GetDevices())
          row.choices.push_back({device.id, device.name});
        rows_.push_back(std::move(row));
        Row listen;
        listen.label = "Assign by pressing a button";
        listen.action = Action::kListen;
        rows_.push_back(std::move(listen));
        const std::string flag = "player" + std::to_string(player + 1) + "_prompts";
        Select("Button prompts", flag.c_str(),
               "auto|Automatic;xbox|Xbox;playstation|PlayStation;nintendo|Nintendo;steamdeck|Steam "
               "Deck;keyboard|Keyboard",
               Apply::kInput);
        break;
      }
    }
  }
  std::string Label(const Row& row) const {
    if (row.action == Action::kModAction) {
      if (row.mod_type == "toggle") return row.label + (row.mod_value.get<bool>() ? ": On" : ": Off");
      if (row.mod_type == "slider") {
        char value[48];
        std::snprintf(value, sizeof(value), "%.2f", row.mod_value.get<double>());
        return row.label + ": " + value;
      }
      if (row.mod_type == "select")
        for (const auto& choice : row.choices)
          if (choice.value == row.mod_value.dump()) return row.label + ": " + choice.label;
      return row.label;
    }
    if (row.action == Action::kListen && listening_player_ >= 0)
      return "Press a button on your input...";
    if (row.name.empty() || row.action == Action::kNative)
      return row.label;
    if (capturing_ == row.name)
      return row.label + ": press a key...";
    const auto value = Value(row.name);
    const auto found = std::find_if(row.choices.begin(), row.choices.end(),
                                    [&](const Choice& choice) { return choice.value == value; });
    std::string text = found == row.choices.end() ? value : found->label;
    if (row.step > 0) {
      char number[32];
      std::snprintf(number, sizeof(number), "%.2f", std::strtof(value.c_str(), nullptr));
      text = number;
    }
    if (text.empty())
      text = "Unbound";
    if (text.size() > 32)
      text = text.substr(0, 29) + "...";
    return row.label + ": " + text +
           (row.apply == Apply::kRestart && row.action == Action::kNone ? " (restart)" : "");
  }
  void Rebuild(int selection = 0) {
    if (!apt_.ready())
      return;
    InvalidateGameMenuPointer();
    MakeRows();
    constexpr const char* titles[] = {"Options",  "Recomp settings",    "Graphics",     "Display",
                                      "Sound",    "Keyboard and mouse", "Key bindings", "Players",
                                      "Player 1", "Player 2",           "Characters",   "Colour",
                                      "Extras",   "Saves", "Mods", "Mod settings"};
    static_assert(std::size(titles) == size_t(Page::kModPage) + 1);
    if (title_) {
      std::string title = titles[size_t(page_)];
      if (page_ == Page::kModPage)
        title = mod_title_;
      std::memcpy(runtime_->memory()->TranslateVirtual(title_), title.c_str(), title.size() + 1);
    }
    shown_.clear();
    const bool paginate = rows_.size() > 5;
    // A callback or code reload may shorten a mod page while it is open.
    // Normalize the old offset before subtraction and indexing.
    if (!paginate || offset_ >= rows_.size())
      offset_ = 0;
    const size_t count = paginate ? std::min(kRowsPerPage, rows_.size() - offset_) : rows_.size();
    for (size_t i = 0; i < count; ++i)
      shown_.push_back(rows_[(paginate ? offset_ : 0) + i]);
    if (paginate) {
      Row more;
      more.label = "More settings  " + std::to_string(offset_ / kRowsPerPage + 1) + "/" +
                   std::to_string((rows_.size() + kRowsPerPage - 1) / kRowsPerPage);
      more.action = Action::kMore;
      shown_.push_back(std::move(more));
    }
    // The shipped reference clips already contain the game's font and text
    // format. Reuse them; duplicateMovieClip does not reproduce their native
    // text children in this APT implementation.
    constexpr const char* text_refs[] = {"text_audio", "text_video", "text_controls",
                                         "text_downloads", "text_credits"};
    if (!apt_.Get("Options.text_audio.text_entry")) {
      rebuild_after_ = 1;
      return;
    }
    if (!reference_scales_ready_) {
      for (size_t i = 0; i < std::size(text_refs); ++i) {
        const std::string ref = std::string("Options.") + text_refs[i];
        auto& scale = reference_scales_[i];
        scale = {apt_.Real(ref + "._xscale"), apt_.Real(ref + "._yscale")};
        for (auto& component : scale) {
          if (!std::isfinite(component) || component <= 0)
            component = 100;
        }
        REXLOG_INFO("[game-menu] Native text scale {}: {}, {}", text_refs[i], scale[0], scale[1]);
      }
      reference_scales_ready_ = true;
    }
    apt_.Invoke("stopKeyListener");
    apt_.Invoke("destroyActiveMenuButtons");
    // attachMovie replaces occupied depths, but a shorter page must also
    // remove the buttons at the remaining depths. Hide every reference too.
    for (size_t i = 0; i < std::size(text_refs); ++i) {
      apt_.SetNumber(std::string("Options.") + text_refs[i] + "._visible", 0);
      const std::string button = "recompButton" + std::to_string(i);
      apt_.Invoke("Options." + button + ".removeMovieClip");
      apt_.Invoke(button + ".removeMovieClip");
    }
    std::vector<std::string> references, buttons, ids;
    for (size_t i = 0; i < shown_.size(); ++i) {
      const std::string name = text_refs[i], ref = "Options." + name;
      const std::string variable = movie_ + ".RecompLabel" + std::to_string(i);
      apt_.SetText(variable, Label(shown_[i]));
      apt_.SetText(ref + ".text_entry.variable", variable);
      apt_.SetText(ref + ".text_entry.text", Label(shown_[i]));
      apt_.SetNumber(ref + "._y", 2 - int(shown_.size() - 1) * 20 + int(i) * 40);
      const float size = page_ == Page::kRoot || page_ == Page::kPC || page_ == Page::kPlayers ||
                                 page_ == Page::kExtras
                             ? 1.0f
                             : 0.8f;
      apt_.SetReal(ref + "._xscale", reference_scales_[i][0] * size);
      apt_.SetReal(ref + "._yscale", reference_scales_[i][1] * size);
      references.push_back(name);
      buttons.push_back("recompButton" + std::to_string(i));
      ids.push_back(shown_[i].action == Action::kNative ? shown_[i].name
                                                        : "Recomp" + std::to_string(i));
    }
    apt_.SetArray("Options.TextRefs", references);
    apt_.SetArray("Options.MenuItemButtons", buttons);
    apt_.SetArray("Options.MenuItemIds", ids);
    apt_.SetArray("Options.RemoveItems", {});
    apt_.SetText("InitialSelection", "");
    apt_.Invoke("initializeButtons");
    const std::array<std::string, 1> args{
        std::to_string(std::clamp(selection, 0, int(shown_.size()) - 1))};
    apt_.Invoke("activateMenuButtons", args);
    apt_.SetNumber("Options.currentSelection", std::clamp(selection, 0, int(shown_.size()) - 1));
    apt_.Invoke("startKeyListener");
    next_mod_refresh_ = Clock::now() + std::chrono::milliseconds(250);
    REXLOG_INFO("[game-menu] Native page {}, {} rows", int(page_), shown_.size());
  }

  rex::Runtime* runtime_;
  rex::ui::Window* window_;
  std::filesystem::path config_;
  simpsons::GameMenuApt apt_;
  InputSystem* input_;
  rex::graphics::GraphicsSystem* graphics_;
  std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
  std::atomic<bool> visible_{false}, capture_active_{false}, keyboard_used_{false};
  std::mutex key_mutex_;
  std::deque<int> keys_;
  std::string pending_binding_;
  std::map<std::string, std::string> changed_, pending_save_;
  std::map<std::string, uint16_t> listening_buttons_;
  std::vector<Row> rows_, shown_;
  std::array<std::array<float, 2>, 5> reference_scales_{};
  bool reference_scales_ready_ = false;
  std::string movie_, capturing_, mod_page_, mod_title_;
  Page page_ = Page::kRoot;
  uint32_t title_ = 0, controller_player_ = 0;
  size_t offset_ = 0;
  unsigned rebuild_after_ = 0;
  int listening_player_ = -1;
  bool listening_ready_ = false, unsaved_ = false;
  Clock::time_point last_change_{}, next_mod_refresh_{};
};

// OnShutdown runs before the guest threads are stopped. Keep the object and
// its arena alive after removing its window listener and disabling callbacks.
std::unique_ptr<GameMenu> g_menu;
}  // namespace

void InitGameMenu(rex::Runtime* runtime, rex::ui::Window* window,
                  const std::filesystem::path& config) {
  g_menu = std::make_unique<GameMenu>(runtime, window, config);
  InitGameMenuPointer(runtime, window);
}
void ShutdownGameMenu() {
  ShutdownGameMenuPointer();
  if (g_menu)
    g_menu->Shutdown();
}
void TickGameMenu(PPCContext& ctx, uint8_t* base) {
  TickGameMenuPointer(ctx, base);
  if (g_menu)
    g_menu->Tick(ctx, base);
}
bool GameMenuIsVisible() {
  return g_menu && g_menu->visible();
}
uint32_t GameMenuPromptPlayer() {
  return g_menu ? g_menu->prompt_player() : 0;
}
bool GameMenuUsesKeyboard() {
  return g_menu && g_menu->UsesKeyboard();
}
bool GameMenuCapturingInput() {
  return g_menu && g_menu->CapturingInput();
}
void GameMenuSetKeyboardInput(bool keyboard) {
  if (g_menu)
    g_menu->SetKeyboardInput(keyboard);
}

uint32_t GameMenuTitleForText(const uint8_t* base, uint32_t text) {
  return g_menu ? g_menu->TitleForText(base, text) : 0;
}

namespace {
void EnterNativeOptions(uint8_t* base) {
  if (!g_menu || !REXCVAR_GET(in_game_settings) || LoadGuestU32(base, kOptionsState) != 1)
    return;
  const auto screen = LoadGuestU32(base, kOptionsScreen);
  if (!screen || LoadGuestU32(base, screen + 28) != 1)
    return;
  const auto handle = LoadGuestU32(base, screen + 96);
  const auto path = handle ? LoadGuestU32(base, handle) : 0;
  if (path)
    g_menu->Enter(reinterpret_cast<const char*>(GuestToHost(base, path)));
}
}  // namespace

// Options starts on its own root timeline when the screen is shown. Returning
// from Audio or Controls goes through the state transition instead.
REX_FUNC(sub_823A4890) {
  __imp__sub_823A4890(ctx, base);
  EnterNativeOptions(base);
}

REX_FUNC(sub_823A4C90) {
  __imp__sub_823A4C90(ctx, base);
  EnterNativeOptions(base);
}

REX_FUNC(sub_823A4850) {
  if (g_menu)
    g_menu->Leave();
  __imp__sub_823A4850(ctx, base);
}

REX_FUNC(sub_823A5768) {
  if (game_ui::SavesMenuEvent(ctx, base, ctx.r3.u32, int(ctx.r5.u32)))
    return;
  if (g_menu && ctx.r3.u32 == LoadGuestU32(base, kOptionsScreen)) {
    g_menu->ControllerActivity(ctx.r4.u32);
    if (g_menu->Event(ctx, base, ctx.r5.u32))
      return;
  }
  __imp__sub_823A5768(ctx, base);
}

// The base screen closes without always visiting the Options suspend hook.
// Match its active-screen flag so keyboard capture ends before gameplay resumes.
REX_FUNC(sub_827F3780) {
  GameMenuPointerScreenClosed(ctx.r3.u32);
  if (g_menu && ctx.r3.u32 == LoadGuestU32(base, kOptionsScreen) &&
      LoadGuestU32(base, ctx.r3.u32 + 28) == 1)
    g_menu->Leave();
  __imp__sub_827F3780(ctx, base);
}
