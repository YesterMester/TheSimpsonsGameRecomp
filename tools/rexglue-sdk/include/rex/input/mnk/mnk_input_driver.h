/**
 * @file        rex/input/mnk/mnk_input_driver.h
 * @brief       Keyboard/mouse input driver - maps MnK to Xbox 360 controller.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <rex/input/input_driver.h>
#include <rex/ui/window_listener.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <queue>
#include <string>

namespace rex::input::mnk {

class MnkInputDriver final : public InputDriver,
                             public rex::ui::WindowInputListener,
                             public rex::ui::WindowListener {
 public:
  explicit MnkInputDriver(rex::ui::Window* window, size_t window_z_order);
  ~MnkInputDriver() override;

  X_STATUS Setup() override;

  X_RESULT GetCapabilities(uint32_t user_index, uint32_t flags,
                           X_INPUT_CAPABILITIES* out_caps) override;
  X_RESULT GetState(uint32_t user_index, X_INPUT_STATE* out_state) override;
  X_RESULT SetState(uint32_t user_index, X_INPUT_VIBRATION* vibration) override;
  X_RESULT GetKeystroke(uint32_t user_index, uint32_t flags,
                        X_INPUT_KEYSTROKE* out_keystroke) override;

  void OnWindowAvailable(rex::ui::Window* window) override;
  std::vector<InputDevice> GetDevices() override;
  void SetMenuPointerActive(bool active);

  // WindowInputListener
  void OnKeyDown(rex::ui::KeyEvent& e) override;
  void OnKeyUp(rex::ui::KeyEvent& e) override;
  void OnMouseDown(rex::ui::MouseEvent& e) override;
  void OnMouseUp(rex::ui::MouseEvent& e) override;
  void OnMouseMove(rex::ui::MouseEvent& e) override;
  void OnMouseWheel(rex::ui::MouseEvent& e) override;
  void OnMouseRelativeMove(rex::ui::MouseEvent& e) override;

  // WindowListener
  void OnClosing(rex::ui::UIEvent& e) override;
  void OnLostFocus(rex::ui::UISetupEvent& e) override;
  void OnGotFocus(rex::ui::UISetupEvent& e) override;

 private:
  uint32_t UserIndex() const;
  bool IsEnabled() const;
  void CenterCursor();
  // From GetState (the game's thread): requests capturing or releasing the
  // mouse, which the windowing systems only allow on the UI thread.
  void UpdateMouseCapture();
  // UI thread.
  void ApplyMouseCapture(bool capture);
  void SetKeyState(uint16_t vk, bool down);
  void EnqueueKeystroke(uint16_t vk_pad, bool down);
  // A right stick axis value for mouse movement at the velocity (counts per
  // second).
  int32_t MouseVelocityToStick(double velocity) const;

  rex::ui::Window* attached_window_ = nullptr;
  // Cleared in the destructor, for calls queued for the UI thread.
  std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);

  std::mutex state_mutex_;
  bool key_down_[256] = {};
  // Every press is reported for at least a moment, so taps shorter than the
  // interval between the game's polls aren't lost.
  std::chrono::steady_clock::time_point key_hold_until_[256] = {};
  // Mouse wheel "keys" stay pressed until these times.
  std::chrono::steady_clock::time_point wheel_up_until_{};
  std::chrono::steady_clock::time_point wheel_down_until_{};

  // Mouse movement since the right stick was last updated.
  int32_t mouse_dx_ = 0;
  int32_t mouse_dy_ = 0;
  int32_t prev_mouse_x_ = 0;
  int32_t prev_mouse_y_ = 0;
  // The right stick from mouse movement, recomputed at most every few
  // milliseconds (the game may poll several times per frame), from the
  // velocity over the last two updates.
  std::chrono::steady_clock::time_point stick_update_time_{};
  double prev_velocity_x_ = 0.0;
  double prev_velocity_y_ = 0.0;
  int32_t stick_rx_ = 0;
  int32_t stick_ry_ = 0;
  // Game thread: the capture state last requested from the UI thread.
  bool capture_requested_ = false;
  std::atomic<bool> mouse_captured_{false};
  std::atomic<bool> menu_pointer_active_{false};
  std::atomic<bool> has_focus_{true};

  // Keystroke queue
  std::queue<X_INPUT_KEYSTROKE> keystroke_queue_;

  // Packet number incremented on state change
  uint32_t packet_number_ = 0;
};

}  // namespace rex::input::mnk
