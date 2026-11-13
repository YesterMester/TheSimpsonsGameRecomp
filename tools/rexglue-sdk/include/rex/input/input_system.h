#pragma once
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

#include <memory>
#include <array>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <rex/input/input.h>
#include <rex/input/input_driver.h>
#include <rex/system/interfaces/input.h>

namespace rex::ui {
class Window;
}

namespace rex::input {

class InputSystem : public system::IInputSystem {
 public:
  explicit InputSystem(rex::ui::Window* window);
  ~InputSystem() override;

  rex::ui::Window* window() const { return window_; }

  X_STATUS Setup() override;
  void Shutdown() override;

  void AddDriver(std::unique_ptr<InputDriver> driver);
  void AttachWindow(rex::ui::Window* window);
  void SetActiveCallback(std::function<bool()> callback);

  X_RESULT GetCapabilities(uint32_t user_index, uint32_t flags, X_INPUT_CAPABILITIES* out_caps);
  X_RESULT GetState(uint32_t user_index, X_INPUT_STATE* out_state);
  X_RESULT SetState(uint32_t user_index, X_INPUT_VIBRATION* vibration);
  X_RESULT GetKeystroke(uint32_t user_index, uint32_t flags, X_INPUT_KEYSTROKE* out_keystroke);

  std::vector<InputDevice> GetDevices();
  // Assigning an input already owned by another player exchanges their
  // assignments. A device never controls both players.
  bool AssignDevice(uint32_t player, const std::string& id);
  bool SetInputSetting(const std::string& name, const std::string& value);
  std::string GetAssignment(uint32_t player) const;
  InputDevice GetActiveDevice(uint32_t player);
  ButtonPrompt GetButtonPrompt(uint32_t player, PromptButton button);
  uint32_t GetMenuPlayer();
  uint32_t GetLastActivePlayer();
  X_RESULT GetDeviceState(const std::string& id, X_INPUT_STATE* out_state);
  // Menus read devices directly while game input is captured. On closing,
  // held buttons must be released before they can reach the game again.
  void SetMenuInputCapture(bool capture);
  // A native menu needs a cursor while retaining keyboard/controller input.
  void SetMenuPointerActive(bool active);

 private:
  struct DeviceSource {
    InputDevice device;
    InputDriver* driver;
  };
  struct Activity {
    X_INPUT_GAMEPAD previous{};
    uint64_t order = 0;
  };
  std::vector<DeviceSource> GetSources();
  std::vector<DeviceSource> GetPlayerSources(uint32_t player,
                                             const std::vector<DeviceSource>& sources);
  X_RESULT ReadPlayer(uint32_t player, X_INPUT_STATE* out_state,
                       const std::vector<DeviceSource>& sources);
  std::string AssignmentFlag(uint32_t player) const;

  rex::ui::Window* window_ = nullptr;

  std::vector<std::unique_ptr<InputDriver>> drivers_;
  std::mutex routing_mutex_;
  std::unordered_map<std::string, Activity> activity_;
  std::array<InputDevice, 4> active_devices_{};
  // Automatic controller ownership survives another controller unplugging.
  std::array<std::string, 4> automatic_gamepads_{};
  std::array<X_INPUT_STATE, 4> player_states_{};
  std::array<uint16_t, 4> release_buttons_{};
  std::array<uint8_t, 4> release_triggers_{};
  uint64_t activity_order_ = 0;
  bool menu_input_capture_ = false;
  uint32_t menu_player_ = 0;
  uint32_t last_active_player_ = 0;
};

/// Create a default InputSystem with SDL + NOP drivers.
/// In tool mode, only the NOP driver is added.
std::unique_ptr<InputSystem> CreateDefaultInputSystem(bool tool_mode);

}  // namespace rex::input
