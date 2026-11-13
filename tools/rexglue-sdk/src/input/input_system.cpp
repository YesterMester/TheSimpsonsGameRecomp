/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#include <rex/dbg.h>
#include <rex/input/flags.h>
#include <rex/input/input_driver.h>
#include <rex/input/input_system.h>
#include <rex/input/menu_input.h>
#include <rex/input/mnk/mnk_input_driver.h>
#include <rex/input/nop/nop_input_driver.h>
#include <rex/input/sdl/sdl_input_driver.h>
#include <rex/input/xinput/xinput_input_driver.h>
#if REX_PLATFORM_ANDROID
#include <rex/input/touch/touch_input_driver.h>
#endif
#include <rex/logging.h>
#include <rex/ui/virtual_key.h>

REXCVAR_DEFINE_STRING(input_backend, "sdl", "Input", "Input backend: sdl, xinput")
    .allowed({"sdl", "xinput"});

REXCVAR_DEFINE_BOOL(guide_button, false, "Input", "Enable guide button pass-through");
REXCVAR_DEFINE_STRING(player1_device, "auto", "Input/Players", "Player 1 input device");
REXCVAR_DEFINE_STRING(player2_device, "auto", "Input/Players", "Player 2 input device");
REXCVAR_DEFINE_STRING(player3_device, "auto", "Input/Players", "Player 3 input device");
REXCVAR_DEFINE_STRING(player4_device, "auto", "Input/Players", "Player 4 input device");
REXCVAR_DEFINE_STRING(player1_prompts, "auto", "Input/Players", "Player 1 button prompts")
    .allowed({"auto", "xbox", "playstation", "nintendo", "steamdeck", "keyboard"});
REXCVAR_DEFINE_STRING(player2_prompts, "auto", "Input/Players", "Player 2 button prompts")
    .allowed({"auto", "xbox", "playstation", "nintendo", "steamdeck", "keyboard"});
REXCVAR_DEFINE_STRING(player3_prompts, "auto", "Input/Players", "Player 3 button prompts")
    .allowed({"auto", "xbox", "playstation", "nintendo", "steamdeck", "keyboard"});
REXCVAR_DEFINE_STRING(player4_prompts, "auto", "Input/Players", "Player 4 button prompts")
    .allowed({"auto", "xbox", "playstation", "nintendo", "steamdeck", "keyboard"});
REXCVAR_DEFINE_STRING(input_inject_file, "", "Input",
                      "Test automation: file polled for injected player 1 controller input, "
                      "one command per write: '<control> <hold_seconds>'");
namespace rex::input {

namespace {

struct MenuButton {
  uint16_t buttons = 0;
  std::chrono::steady_clock::time_point until{};
  uint16_t down = 0, up = 0;
};
std::mutex menu_button_mutex;
std::array<MenuButton, 4> menu_buttons;

uint16_t ReadMenuButtons(uint32_t player) {
  std::lock_guard lock(menu_button_mutex);
  const auto& pulse = menu_buttons[player];
  return std::chrono::steady_clock::now() < pulse.until ? pulse.buttons : 0;
}

bool ReadMenuKeystroke(uint32_t player, X_INPUT_KEYSTROKE* out) {
  using V = rex::ui::VirtualKey;
  constexpr std::array<V, 16> keys = {V::kXInputPadDpadUp,
                                      V::kXInputPadDpadDown,
                                      V::kXInputPadDpadLeft,
                                      V::kXInputPadDpadRight,
                                      V::kXInputPadStart,
                                      V::kXInputPadBack,
                                      V::kXInputPadLThumbPress,
                                      V::kXInputPadRThumbPress,
                                      V::kXInputPadLShoulder,
                                      V::kXInputPadRShoulder,
                                      V::kNone,
                                      V::kNone,
                                      V::kXInputPadA,
                                      V::kXInputPadB,
                                      V::kXInputPadX,
                                      V::kXInputPadY};
  std::lock_guard lock(menu_button_mutex);
  auto& pulse = menu_buttons[player];
  const bool released = std::chrono::steady_clock::now() >= pulse.until;
  for (unsigned edge = 0; edge < (released ? 2u : 1u); ++edge) {
    auto& pending = edge ? pulse.up : pulse.down;
    for (unsigned bit = 0; bit < keys.size(); ++bit) {
      if (!(pending & (1u << bit)))
        continue;
      pending &= uint16_t(~(1u << bit));
      if (keys[bit] == V::kNone)
        continue;
      *out = {};
      out->virtual_key = uint16_t(keys[bit]);
      out->flags = edge ? X_INPUT_KEYSTROKE_KEYUP : X_INPUT_KEYSTROKE_KEYDOWN;
      out->user_index = uint8_t(player);
      return true;
    }
  }
  return false;
}

// Injected controller input for unattended test runs. The harness writes a
// command such as "A 0.15" to the file; it is consumed on the next poll and
// the control is held for that long.
class InputInjector {
 public:
  bool enabled() const { return !REXCVAR_GET(input_inject_file).empty(); }

  // Adds the active injected control to the state; returns true if enabled.
  bool Apply(X_INPUT_STATE& state) {
    if (!enabled()) {
      return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    auto now = std::chrono::steady_clock::now();
    if (now >= next_poll_) {
      next_poll_ = now + std::chrono::milliseconds(20);
      Poll(now);
    }
    uint16_t buttons = 0;
    int16_t lx = 0, ly = 0, rx = 0, ry = 0;
    uint8_t lt = 0, rt = 0;
    if (now < release_at_) {
      buttons = buttons_;
      lx = lx_;
      ly = ly_;
      rx = rx_;
      ry = ry_;
      lt = lt_;
      rt = rt_;
    }
    if (buttons != last_buttons_ || lx != last_lx_ || ly != last_ly_ || rx != last_rx_ ||
        ry != last_ry_ || lt != last_lt_ || rt != last_rt_) {
      ++packet_;
      last_buttons_ = buttons;
      last_lx_ = lx;
      last_ly_ = ly;
      last_rx_ = rx;
      last_ry_ = ry;
      last_lt_ = lt;
      last_rt_ = rt;
    }
    state.packet_number = uint32_t(state.packet_number) + packet_;
    state.gamepad.buttons = uint16_t(state.gamepad.buttons) | buttons;
    state.gamepad.left_trigger = std::max(state.gamepad.left_trigger, lt);
    state.gamepad.right_trigger = std::max(state.gamepad.right_trigger, rt);
    if (lx || ly) {
      state.gamepad.thumb_lx = lx;
      state.gamepad.thumb_ly = ly;
    }
    if (rx || ry) {
      state.gamepad.thumb_rx = rx;
      state.gamepad.thumb_ry = ry;
    }
    return true;
  }

 private:
  void Poll(std::chrono::steady_clock::time_point now) {
    const std::string& path = REXCVAR_GET(input_inject_file);
    FILE* file = std::fopen(path.c_str(), "r");
    if (!file) {
      return;
    }
    char control[32] = {};
    double hold = 0.15;
    int fields = std::fscanf(file, "%31s %lf", control, &hold);
    std::fclose(file);
    std::remove(path.c_str());
    if (fields < 1 || !std::isfinite(hold) || hold < 0.0 || hold > 60.0) {
      return;
    }
    buttons_ = 0;
    lx_ = ly_ = rx_ = ry_ = 0;
    lt_ = rt_ = 0;
    // One control, or several joined by '+' and held together (e.g. LS+RS).
    std::string controls(control);
    static const std::pair<const char*, uint16_t> kButtons[] = {
        {"A", X_INPUT_GAMEPAD_A},
        {"B", X_INPUT_GAMEPAD_B},
        {"X", X_INPUT_GAMEPAD_X},
        {"Y", X_INPUT_GAMEPAD_Y},
        {"START", X_INPUT_GAMEPAD_START},
        {"BACK", X_INPUT_GAMEPAD_BACK},
        {"LB", X_INPUT_GAMEPAD_LEFT_SHOULDER},
        {"RB", X_INPUT_GAMEPAD_RIGHT_SHOULDER},
        {"LS", X_INPUT_GAMEPAD_LEFT_THUMB},
        {"RS", X_INPUT_GAMEPAD_RIGHT_THUMB},
        {"UP", X_INPUT_GAMEPAD_DPAD_UP},
        {"DOWN", X_INPUT_GAMEPAD_DPAD_DOWN},
        {"LEFT", X_INPUT_GAMEPAD_DPAD_LEFT},
        {"RIGHT", X_INPUT_GAMEPAD_DPAD_RIGHT},
    };
    constexpr int16_t kFull = 32767;
    size_t start = 0;
    while (start <= controls.size()) {
      size_t end = controls.find('+', start);
      if (end == std::string::npos) {
        end = controls.size();
      }
      const std::string name = controls.substr(start, end - start);
      start = end + 1;
      bool known = false;
      for (const auto& [button_name, mask] : kButtons) {
        if (name == button_name) {
          buttons_ |= mask;
          known = true;
        }
      }
      if (name == "LT") {
        lt_ = 0xFF;
        known = true;
      }
      if (name == "RT") {
        rt_ = 0xFF;
        known = true;
      }
      if (name == "L_UP") {
        ly_ = kFull;
        known = true;
      }
      if (name == "L_DOWN") {
        ly_ = -kFull;
        known = true;
      }
      if (name == "L_LEFT") {
        lx_ = -kFull;
        known = true;
      }
      if (name == "L_RIGHT") {
        lx_ = kFull;
        known = true;
      }
      if (name == "R_UP") {
        ry_ = kFull;
        known = true;
      }
      if (name == "R_DOWN") {
        ry_ = -kFull;
        known = true;
      }
      if (name == "R_LEFT") {
        rx_ = -kFull;
        known = true;
      }
      if (name == "R_RIGHT") {
        rx_ = kFull;
        known = true;
      }
      if (!known) {
        REXLOG_WARN("input_inject_file: unknown control '{}'", name);
        buttons_ = 0;
        lx_ = ly_ = rx_ = ry_ = 0;
        lt_ = rt_ = 0;
        return;
      }
    }
    const std::string& name = controls;
    release_at_ = now + std::chrono::microseconds(int64_t(hold * 1e6));
    REXLOG_INFO("input_inject_file: {} for {:.2f}s", name, hold);
  }

  std::mutex mutex_;
  std::chrono::steady_clock::time_point next_poll_{};
  std::chrono::steady_clock::time_point release_at_{};
  uint16_t buttons_ = 0;
  int16_t lx_ = 0, ly_ = 0, rx_ = 0, ry_ = 0;
  uint8_t lt_ = 0, rt_ = 0;
  uint16_t last_buttons_ = 0;
  int16_t last_lx_ = 0, last_ly_ = 0, last_rx_ = 0, last_ry_ = 0;
  uint8_t last_lt_ = 0, last_rt_ = 0;
  uint32_t packet_ = 0;
};

InputInjector g_input_injector;

void FillInjectedCapabilities(X_INPUT_CAPABILITIES* out_caps) {
  if (!out_caps) {
    return;
  }
  std::memset(out_caps, 0, sizeof(*out_caps));
  out_caps->type = 0x01;
  out_caps->sub_type = 0x01;
  out_caps->gamepad.buttons = 0xFFFF;
  out_caps->gamepad.left_trigger = 0xFF;
  out_caps->gamepad.right_trigger = 0xFF;
  out_caps->gamepad.thumb_lx = static_cast<int16_t>(0x7FFF);
  out_caps->gamepad.thumb_ly = static_cast<int16_t>(0x7FFF);
  out_caps->gamepad.thumb_rx = static_cast<int16_t>(0x7FFF);
  out_caps->gamepad.thumb_ry = static_cast<int16_t>(0x7FFF);
}

}  // namespace

void QueueMenuButton(uint32_t player, uint16_t button) {
  if (player >= menu_buttons.size() || !button)
    return;
  std::lock_guard lock(menu_button_mutex);
  menu_buttons[player] = {button, std::chrono::steady_clock::now() + std::chrono::milliseconds(150),
                          button, button};
}

void ClearMenuButtons() {
  std::lock_guard lock(menu_button_mutex);
  for (auto& pulse : menu_buttons) {
    const uint16_t release = pulse.up & ~pulse.down;
    pulse = {0, {}, 0, release};
  }
}

InputSystem::InputSystem(rex::ui::Window* window) : window_(window) {}

InputSystem::~InputSystem() = default;

X_STATUS InputSystem::Setup() {
  return X_STATUS_SUCCESS;
}

void InputSystem::Shutdown() {
  drivers_.clear();
}

void InputSystem::AddDriver(std::unique_ptr<InputDriver> driver) {
  drivers_.push_back(std::move(driver));
}

void InputSystem::AttachWindow(rex::ui::Window* window) {
  window_ = window;
  for (auto& driver : drivers_) {
    driver->OnWindowAvailable(window);
  }
}

void InputSystem::SetActiveCallback(std::function<bool()> callback) {
  for (auto& driver : drivers_) {
    driver->set_is_active_callback(callback);
  }
}

std::string InputSystem::AssignmentFlag(uint32_t player) const {
  return "player" + std::to_string(player + 1) + "_device";
}

std::string InputSystem::GetAssignment(uint32_t player) const {
  return player < 4 ? cvar::GetFlagByName(AssignmentFlag(player)) : "none";
}

std::vector<InputSystem::DeviceSource> InputSystem::GetSources() {
  std::vector<DeviceSource> sources;
  for (auto& driver : drivers_) {
    for (auto& device : driver->GetDevices()) {
      sources.push_back({std::move(device), driver.get()});
    }
  }
  return sources;
}

std::vector<InputDevice> InputSystem::GetDevices() {
  std::lock_guard lock(routing_mutex_);
  std::vector<InputDevice> devices;
  for (auto& source : GetSources()) devices.push_back(std::move(source.device));
  return devices;
}

bool InputSystem::AssignDevice(uint32_t player, const std::string& id) {
  if (player >= 4 || id.empty()) return false;
  std::lock_guard lock(routing_mutex_);
  if (id != "auto" && id != "none") {
    auto sources = GetSources();
    if (std::none_of(sources.begin(), sources.end(),
                     [&](const auto& source) { return source.device.id == id; })) return false;
    const std::string previous = GetAssignment(player);
    for (uint32_t other = 0; other < 4; ++other) {
      if (other != player && GetAssignment(other) == id) {
        cvar::SetFlagByName(AssignmentFlag(other), previous);
      }
    }
  }
  if (!cvar::SetFlagByName(AssignmentFlag(player), id)) return false;
  if (id == "keyboard") {
    cvar::SetFlagByName("mnk_user_index", std::to_string(player));
    cvar::SetFlagByName("mnk_mode", "true");
  }
  // Reassignment cannot carry a held confirm/attack into the other player.
  for (uint32_t index = 0; index < 4; ++index) {
    release_buttons_[index] = 0xFFFF;
    release_triggers_[index] = 3;
    active_devices_[index] = {};
  }
  REXLOG_INFO("Input: player {} assigned to {}", player + 1, id);
  return true;
}

std::vector<InputSystem::DeviceSource> InputSystem::GetPlayerSources(
    uint32_t player, const std::vector<DeviceSource>& sources) {
  std::vector<DeviceSource> result;
  const std::string assignment = GetAssignment(player);
  if (assignment == "none")
    return result;
  if (assignment != "auto") {
    for (const auto& source : sources) {
      if (source.device.id == assignment)
        result.push_back(source);
    }
    return result;
  }
  std::array<std::string, 4> assignments;
  for (uint32_t index = 0; index < 4; ++index)
    assignments[index] = GetAssignment(index);
  auto claimed = [&](const std::string& id) {
    return std::find(assignments.begin(), assignments.end(), id) != assignments.end();
  };
  // Do not renumber the remaining controllers when one disconnects. Preserve
  // each automatic player's reservation, then fill vacant slots from inputs
  // not explicitly assigned or reserved by another automatic player.
  for (uint32_t index = 0; index < 4; ++index) {
    if (assignments[index] != "auto" || claimed(automatic_gamepads_[index]))
      automatic_gamepads_[index].clear();
  }
  auto connected = [&](const std::string& id) {
    return std::any_of(sources.begin(), sources.end(), [&](const auto& source) {
      return source.device.kind == DeviceKind::kGamepad && source.device.id == id;
    });
  };
  for (uint32_t index = 0; index < 4; ++index) {
    if (assignments[index] != "auto" || connected(automatic_gamepads_[index]))
      continue;
    for (const auto& source : sources) {
      if (source.device.kind != DeviceKind::kGamepad || claimed(source.device.id) ||
          std::find(automatic_gamepads_.begin(), automatic_gamepads_.end(), source.device.id) !=
              automatic_gamepads_.end())
        continue;
      automatic_gamepads_[index] = source.device.id;
      break;
    }
  }
  for (const auto& source : sources) {
    if (claimed(source.device.id))
      continue;
    if (source.device.kind == DeviceKind::kGamepad) {
      if (source.device.id == automatic_gamepads_[player])
        result.push_back(source);
    } else if (source.device.kind == DeviceKind::kTouch && player == 0) {
      result.push_back(source);
    } else if (source.device.kind == DeviceKind::kKeyboard &&
               source.device.driver_index == player && cvar::Query<bool>("mnk_mode")) {
      result.push_back(source);
    }
  }
  return result;
}

bool InputSystem::SetInputSetting(const std::string& name, const std::string& value) {
  std::lock_guard lock(routing_mutex_);
  return cvar::SetFlagByName(name, value);
}

X_RESULT InputSystem::ReadPlayer(uint32_t player, X_INPUT_STATE* out_state,
                                 const std::vector<DeviceSource>& sources) {
  X_INPUT_STATE selected{};
  const auto candidates = GetPlayerSources(player, sources);
  InputDevice chosen;
  uint64_t newest = 0;
  bool found = false;
  for (const auto& source : candidates) {
    X_INPUT_STATE state{};
    if (source.driver->GetState(source.device.driver_index, &state) != X_ERROR_SUCCESS)
      continue;
    auto& activity = activity_[source.device.id];
    const auto& pad = state.gamepad;
    const auto& previous = activity.previous;
    auto moved = [](int16_t value, int16_t before) {
      return std::abs(int(value)) > 6000 && std::abs(int(value) - int(before)) > 3000;
    };
    if ((uint16_t(pad.buttons) & ~uint16_t(previous.buttons)) ||
        (pad.left_trigger > 30 && int(pad.left_trigger) - int(previous.left_trigger) > 8) ||
        (pad.right_trigger > 30 && int(pad.right_trigger) - int(previous.right_trigger) > 8) ||
        moved(pad.thumb_lx, previous.thumb_lx) || moved(pad.thumb_ly, previous.thumb_ly) ||
        moved(pad.thumb_rx, previous.thumb_rx) || moved(pad.thumb_ry, previous.thumb_ry)) {
      activity.order = ++activity_order_;
      last_active_player_ = player;
      if (uint16_t(pad.buttons) & ~uint16_t(previous.buttons) & (X_INPUT_GAMEPAD_START | X_INPUT_GAMEPAD_BACK)) menu_player_ = player;
    }
    activity.previous = pad;
    if (!found || activity.order > newest ||
        (activity.order == newest && source.device.id == active_devices_[player].id)) {
      selected = state;
      chosen = source.device;
      newest = activity.order;
      found = true;
    }
  }
  if (found && active_devices_[player].id != chosen.id) {
    // Stop rumble on the device that has just given up this player slot.
    for (const auto& source : sources) {
      if (source.device.id == active_devices_[player].id) {
        X_INPUT_VIBRATION stop{};
        source.driver->SetState(source.device.driver_index, &stop);
      }
    }
    active_devices_[player] = chosen;
  }
  if (player == 0 && g_input_injector.Apply(selected)) found = true;
  if (const auto buttons = ReadMenuButtons(player)) {
    selected.gamepad.buttons = uint16_t(selected.gamepad.buttons) | buttons;
    found = true;
  }
  if (!found) {
    active_devices_[player] = {};
    if (player != 0) {
      if (out_state) *out_state = {};
      return X_ERROR_DEVICE_NOT_CONNECTED;
    }
  }
  auto& pad = selected.gamepad;
  release_buttons_[player] &= uint16_t(pad.buttons);
  if (pad.left_trigger <= 30) release_triggers_[player] &= ~1u;
  if (pad.right_trigger <= 30) release_triggers_[player] &= ~2u;
  if (menu_input_capture_) {
    release_buttons_[player] = pad.buttons;
    release_triggers_[player] = (pad.left_trigger > 30 ? 1 : 0) |
                                (pad.right_trigger > 30 ? 2 : 0);
    pad = {};
  } else {
    pad.buttons = uint16_t(pad.buttons) & ~release_buttons_[player];
    if (release_triggers_[player] & 1) pad.left_trigger = 0;
    if (release_triggers_[player] & 2) pad.right_trigger = 0;
  }
  auto& previous = player_states_[player];
  if (std::memcmp(&previous.gamepad, &pad, sizeof(pad))) {
    previous.gamepad = pad;
    previous.packet_number = uint32_t(previous.packet_number) + 1;
  }
  if (out_state) *out_state = previous;
  return X_ERROR_SUCCESS;
}

InputDevice InputSystem::GetActiveDevice(uint32_t player) {
  if (player >= 4) return {};
  std::lock_guard lock(routing_mutex_);
  auto device = active_devices_[player];
  if (device.id.empty()) {
    const auto sources = GetPlayerSources(player, GetSources());
    if (!sources.empty()) device = sources.front().device;
  }
  return device;
}

ButtonPrompt InputSystem::GetButtonPrompt(uint32_t player, PromptButton button) {
  auto device = GetActiveDevice(player);
  auto style = ParsePromptStyle(cvar::GetFlagByName(
      "player" + std::to_string(player + 1) + "_prompts"), device.prompts);
  auto prompt = MakeButtonPrompt(style, button);
  if (style == device.prompts) {
    if (button == PromptButton::kStart && !device.start_label.empty()) prompt.label = device.start_label;
    if (button == PromptButton::kBack && !device.back_label.empty()) prompt.label = device.back_label;
  }
  return prompt;
}

uint32_t InputSystem::GetMenuPlayer() {
  std::lock_guard lock(routing_mutex_);
  return menu_player_;
}

uint32_t InputSystem::GetLastActivePlayer() {
  std::lock_guard lock(routing_mutex_);
  return last_active_player_;
}

X_RESULT InputSystem::GetDeviceState(const std::string& id, X_INPUT_STATE* out_state) {
  std::lock_guard lock(routing_mutex_);
  for (const auto& source : GetSources()) {
    if (source.device.id == id) return source.driver->GetState(source.device.driver_index, out_state);
  }
  return X_ERROR_DEVICE_NOT_CONNECTED;
}

void InputSystem::SetMenuInputCapture(bool capture) {
  std::lock_guard lock(routing_mutex_);
  if (capture != menu_input_capture_) {
    release_buttons_.fill(0xFFFF);
    release_triggers_.fill(3);
    menu_input_capture_ = capture;
  }
}

void InputSystem::SetMenuPointerActive(bool active) {
  for (const auto& driver : drivers_)
    if (auto* keyboard = dynamic_cast<mnk::MnkInputDriver*>(driver.get()))
      keyboard->SetMenuPointerActive(active);
}

X_RESULT InputSystem::GetCapabilities(uint32_t user_index, uint32_t flags,
                                      X_INPUT_CAPABILITIES* out_caps) {
  SCOPE_profile_cpu_f("hid");
  if (user_index >= 4) return X_ERROR_BAD_ARGUMENTS;
  std::lock_guard lock(routing_mutex_);
  for (const auto& source : GetPlayerSources(user_index, GetSources())) {
    const auto result = source.driver->GetCapabilities(source.device.driver_index, flags, out_caps);
    if (result == X_ERROR_SUCCESS) return result;
  }
  if (user_index == 0) {
    FillInjectedCapabilities(out_caps);
    return X_ERROR_SUCCESS;
  }
  return X_ERROR_DEVICE_NOT_CONNECTED;
}

X_RESULT InputSystem::GetState(uint32_t user_index, X_INPUT_STATE* out_state) {
  SCOPE_profile_cpu_f("hid");
  if (user_index >= 4) return X_ERROR_BAD_ARGUMENTS;
  std::lock_guard lock(routing_mutex_);
  return ReadPlayer(user_index, out_state, GetSources());
}

X_RESULT InputSystem::SetState(uint32_t user_index, X_INPUT_VIBRATION* vibration) {
  SCOPE_profile_cpu_f("hid");
  if (user_index >= 4 || !vibration) return X_ERROR_BAD_ARGUMENTS;
  std::lock_guard lock(routing_mutex_);
  for (const auto& source : GetPlayerSources(user_index, GetSources())) {
    if (source.device.id == active_devices_[user_index].id || active_devices_[user_index].id.empty()) {
      return source.driver->SetState(source.device.driver_index, vibration);
    }
  }
  return user_index == 0 ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED;
}

X_RESULT InputSystem::GetKeystroke(uint32_t user_index, uint32_t flags,
                                   X_INPUT_KEYSTROKE* out_keystroke) {
  SCOPE_profile_cpu_f("hid");
  if (!out_keystroke || (user_index >= 4 && user_index != 0xFF)) return X_ERROR_BAD_ARGUMENTS;
  std::lock_guard lock(routing_mutex_);
  const auto sources = GetSources();
  auto discard_keys = [&](const DeviceSource& source) {
    X_INPUT_KEYSTROKE discarded{};
    for (uint32_t count = 0; count < 32; ++count) {
      if (source.driver->GetKeystroke(source.device.driver_index, flags, &discarded) != X_ERROR_SUCCESS) break;
    }
  };
  if (menu_input_capture_) {
    for (const auto& source : sources) discard_keys(source);
    return X_ERROR_EMPTY;
  }
  for (uint32_t player = user_index == 0xFF ? 0 : user_index;
       player < (user_index == 0xFF ? 4 : user_index + 1); ++player) {
    if (ReadMenuKeystroke(player, out_keystroke))
      return X_ERROR_SUCCESS;
    for (const auto& source : GetPlayerSources(player, sources)) {
      if (!active_devices_[player].id.empty() && source.device.id != active_devices_[player].id) continue;
      if (release_buttons_[player] || release_triggers_[player]) {
        X_INPUT_STATE state{};
        if (source.driver->GetState(source.device.driver_index, &state) == X_ERROR_SUCCESS) {
          release_buttons_[player] &= uint16_t(state.gamepad.buttons);
          if (state.gamepad.left_trigger <= 30) release_triggers_[player] &= ~1u;
          if (state.gamepad.right_trigger <= 30) release_triggers_[player] &= ~2u;
        }
        if (release_buttons_[player] || release_triggers_[player]) {
          discard_keys(source);
          continue;
        }
      }
      if (source.driver->GetKeystroke(source.device.driver_index, flags, out_keystroke) == X_ERROR_SUCCESS) {
        out_keystroke->user_index = player;
        return X_ERROR_SUCCESS;
      }
    }
  }
  return X_ERROR_EMPTY;
}

std::unique_ptr<InputSystem> CreateDefaultInputSystem(bool tool_mode) {
  auto input = std::make_unique<InputSystem>(nullptr);

  if (!tool_mode) {
#if REX_PLATFORM_WIN32
    if (REXCVAR_GET(input_backend) == "xinput") {
      auto xinput_driver = std::make_unique<xinput::XinputInputDriver>(nullptr, 0);
      if (xinput_driver->Setup() == X_STATUS_SUCCESS) {
        input->AddDriver(std::move(xinput_driver));
      }
    }
#endif

    if (REXCVAR_GET(input_backend) == "sdl") {
      auto sdl_driver = std::make_unique<sdl::SDLInputDriver>(nullptr, 0);
      if (sdl_driver->Setup() == X_STATUS_SUCCESS) {
        input->AddDriver(std::move(sdl_driver));
      }
    }

    // MnK driver (keyboard/mouse -> controller emulation)
#if REX_PLATFORM_ANDROID
    input->AddDriver(std::make_unique<touch::TouchInputDriver>());
#else
    auto mnk_driver = std::make_unique<mnk::MnkInputDriver>(nullptr, 0);
    if (mnk_driver->Setup() == X_STATUS_SUCCESS) {
      input->AddDriver(std::move(mnk_driver));
    }
#endif
  }

  // NOP driver (primary in tool mode, fallback otherwise)
  uint8_t nop_index = tool_mode ? 0 : 1;
  input->AddDriver(std::make_unique<nop::NopInputDriver>(nullptr, nop_index));
  return input;
}

}  // namespace rex::input
