/**
 * @file        input/mnk/mnk_input_driver.cpp
 * @brief       Keyboard/mouse input driver implementation.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <rex/input/mnk/mnk_input_driver.h>

#include <rex/cvar.h>
#include <rex/input/input.h>
#include <rex/logging.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/overlay_input.h>
#include <rex/ui/virtual_key.h>
#include <rex/ui/window.h>

#include <algorithm>
#include <cstdlib>
#include <cmath>
#include <cstring>

#if REX_PLATFORM_WIN32
#include <rex/ui/window_win.h>
#include <windows.h>
#endif

REXCVAR_DEFINE_BOOL(mnk_mode, false, "Input", "Enable keyboard/mouse controller emulation");
REXCVAR_DEFINE_INT32(mnk_user_index, 0, "Input", "Controller slot (0-3) for MnK").range(0, 3);
REXCVAR_DEFINE_DOUBLE(mnk_sensitivity, 1.0, "Input", "Mouse sensitivity for right stick")
    .range(0.01, 10.0);
REXCVAR_DEFINE_BOOL(mnk_invert_y, false, "Input", "Invert vertical mouse look");
REXCVAR_DEFINE_DOUBLE(mnk_stick_deadzone, 0.24, "Input",
                      "Right stick deflection the slowest mouse movement maps to, so it gets past "
                      "the game's own stick dead zone")
    .range(0.0, 0.9);

// A binding is a key name or several separated by commas ("Space,Enter"). Mouse:
// LMB, RMB, MMB, Mouse4, Mouse5, WheelUp, WheelDown.
REXCVAR_DEFINE_STRING(keybind_a, "Space,Enter", "Input/Keybinds/Controller", "A button (jump)");
REXCVAR_DEFINE_STRING(keybind_b, "RMB,Backspace", "Input/Keybinds/Controller",
                      "B button (special attack, back in menus)");
REXCVAR_DEFINE_STRING(keybind_x, "LMB", "Input/Keybinds/Controller", "X button (attack)");
REXCVAR_DEFINE_STRING(keybind_y, "E", "Input/Keybinds/Controller", "Y button (action)");
REXCVAR_DEFINE_STRING(keybind_left_trigger, "Shift", "Input/Keybinds/Controller",
                      "Left trigger (target)");
REXCVAR_DEFINE_STRING(keybind_right_trigger, "F", "Input/Keybinds/Controller", "Right trigger");
REXCVAR_DEFINE_STRING(keybind_left_shoulder, "Q", "Input/Keybinds/Controller", "Left shoulder");
REXCVAR_DEFINE_STRING(keybind_right_shoulder, "R", "Input/Keybinds/Controller", "Right shoulder");
REXCVAR_DEFINE_STRING(keybind_lstick_up, "W", "Input/Keybinds/Controller", "Left stick up");
REXCVAR_DEFINE_STRING(keybind_lstick_down, "S", "Input/Keybinds/Controller", "Left stick down");
REXCVAR_DEFINE_STRING(keybind_lstick_left, "A", "Input/Keybinds/Controller", "Left stick left");
REXCVAR_DEFINE_STRING(keybind_lstick_right, "D", "Input/Keybinds/Controller", "Left stick right");
REXCVAR_DEFINE_STRING(keybind_lstick_press, "C", "Input/Keybinds/Controller", "Left stick press");
REXCVAR_DEFINE_STRING(keybind_walk, "Control", "Input/Keybinds/Controller",
                      "Walk (half the left stick while held)");
REXCVAR_DEFINE_STRING(keybind_rstick_press, "MMB", "Input/Keybinds/Controller",
                      "Right stick press");
// Camera on keys, besides the mouse (#33). Unbound by default.
REXCVAR_DEFINE_STRING(keybind_rstick_up, "", "Input/Keybinds/Controller",
                      "Right stick up (camera)");
REXCVAR_DEFINE_STRING(keybind_rstick_down, "", "Input/Keybinds/Controller",
                      "Right stick down (camera)");
REXCVAR_DEFINE_STRING(keybind_rstick_left, "", "Input/Keybinds/Controller",
                      "Right stick left (camera)");
REXCVAR_DEFINE_STRING(keybind_rstick_right, "", "Input/Keybinds/Controller",
                      "Right stick right (camera)");
REXCVAR_DEFINE_STRING(keybind_dpad_up, "1,Up", "Input/Keybinds/Controller",
                      "D-pad up (switch character)");
REXCVAR_DEFINE_STRING(keybind_dpad_down, "3,Down", "Input/Keybinds/Controller",
                      "D-pad down (switch character)");
REXCVAR_DEFINE_STRING(keybind_dpad_left, "4,Left", "Input/Keybinds/Controller",
                      "D-pad left (switch character)");
REXCVAR_DEFINE_STRING(keybind_dpad_right, "2,Right", "Input/Keybinds/Controller",
                      "D-pad right (switch character)");
REXCVAR_DEFINE_STRING(keybind_back, "Tab", "Input/Keybinds/Controller", "Back button (to-do list)");
REXCVAR_DEFINE_STRING(keybind_start, "Escape", "Input/Keybinds/Controller",
                      "Start button (pause)");
REXCVAR_DEFINE_STRING(keybind_guide, "", "Input/Keybinds/Controller", "Guide button");

namespace rex::input::mnk {

using rex::ui::VirtualKey;

MnkInputDriver::MnkInputDriver(rex::ui::Window* window, size_t window_z_order)
    : InputDriver(window, window_z_order) {}

MnkInputDriver::~MnkInputDriver() {
  alive_->store(false);
  // Detach handled by OnClosing; if window outlives the driver, clean up here.
  if (attached_window_) {
    attached_window_->RemoveInputListener(this);
    attached_window_->RemoveListener(this);
    attached_window_ = nullptr;
  }
}

X_STATUS MnkInputDriver::Setup() {
  REXLOG_INFO("MnK input driver initialized");
  return X_STATUS_SUCCESS;
}

void MnkInputDriver::OnWindowAvailable(rex::ui::Window* window) {
  if (window) {
    attached_window_ = window;
    has_focus_ = window->HasFocus();
    window->AddInputListener(this, window_z_order());
    window->AddListener(this);
  }
}

void MnkInputDriver::OnClosing(rex::ui::UIEvent&) {
  if (attached_window_) {
    if (mouse_captured_) {
      mouse_captured_ = false;
      attached_window_->SetCursorVisibility(rex::ui::Window::CursorVisibility::kVisible);
      attached_window_->ReleaseMouse();
    }
    attached_window_->RemoveInputListener(this);
    attached_window_->RemoveListener(this);
    attached_window_ = nullptr;
  }
}

uint32_t MnkInputDriver::UserIndex() const {
  return static_cast<uint32_t>(REXCVAR_GET(mnk_user_index));
}

bool MnkInputDriver::IsEnabled() const {
  return REXCVAR_GET(mnk_mode);
}

static bool IsBindPressed(const bool (&key_down)[256], const std::string& cvar_val) {
  // Any of the comma-separated keys.
  size_t name_start = 0;
  while (name_start <= cvar_val.size()) {
    size_t name_end = cvar_val.find(',', name_start);
    if (name_end == std::string::npos) {
      name_end = cvar_val.size();
    }
    size_t first = name_start, last = name_end;
    while (first < last && cvar_val[first] == ' ') {
      ++first;
    }
    while (last > first && cvar_val[last - 1] == ' ') {
      --last;
    }
    if (first < last) {
      VirtualKey vk =
          rex::ui::ParseVirtualKey(std::string_view(cvar_val).substr(first, last - first));
      uint16_t idx = static_cast<uint16_t>(vk);
      if (vk != VirtualKey::kNone && idx < 256 && key_down[idx]) {
        return true;
      }
    }
    name_start = name_end + 1;
  }
  return false;
}

// Pseudo virtual keys of the mouse wheel (unassigned in Windows, see
// keybinds.cpp).
constexpr uint16_t kVirtualKeyWheelUp = 0x0A;
constexpr uint16_t kVirtualKeyWheelDown = 0x0B;
// How long a wheel detent holds its "key", so the game sees it for a few
// frames.
constexpr std::chrono::milliseconds kWheelPressDuration(70);
// The shortest a key or mouse button press is reported for.
constexpr std::chrono::milliseconds kMinPressDuration(50);

X_RESULT MnkInputDriver::GetCapabilities(uint32_t user_index, uint32_t flags,
                                         X_INPUT_CAPABILITIES* out_caps) {
  if (!IsEnabled() || user_index != UserIndex()) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  if (out_caps) {
    std::memset(out_caps, 0, sizeof(*out_caps));
    out_caps->type = 0x01;
    out_caps->sub_type = 0x01;
    out_caps->flags = 0;
    out_caps->gamepad.buttons = 0xFFFF;
    out_caps->gamepad.left_trigger = 0xFF;
    out_caps->gamepad.right_trigger = 0xFF;
    out_caps->gamepad.thumb_lx = static_cast<int16_t>(0x7FFF);
    out_caps->gamepad.thumb_ly = static_cast<int16_t>(0x7FFF);
    out_caps->gamepad.thumb_rx = static_cast<int16_t>(0x7FFF);
    out_caps->gamepad.thumb_ry = static_cast<int16_t>(0x7FFF);
    out_caps->vibration.left_motor_speed = 0xFFFF;
    out_caps->vibration.right_motor_speed = 0xFFFF;
  }
  return X_ERROR_SUCCESS;
}

X_RESULT MnkInputDriver::GetState(uint32_t user_index, X_INPUT_STATE* out_state) {
  if (!IsEnabled() || user_index != UserIndex()) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  UpdateMouseCapture();

  // Nothing reaches the game while an overlay (settings, console) has the
  // keyboard and mouse.
  if (!is_active() || !has_focus_ || rex::ui::IsOverlayInputActive()) {
    if (out_state) {
      std::memset(out_state, 0, sizeof(*out_state));
      out_state->packet_number = packet_number_;
    }
    return X_ERROR_SUCCESS;
  }

  std::lock_guard lock(state_mutex_);

  auto now = std::chrono::steady_clock::now();
  // Keys held now, or released but pressed too briefly to have been seen yet.
  bool key_down[256];
  for (size_t i = 0; i < 256; ++i) {
    key_down[i] = key_down_[i] || now < key_hold_until_[i];
  }
  key_down[kVirtualKeyWheelUp] = now < wheel_up_until_;
  key_down[kVirtualKeyWheelDown] = now < wheel_down_until_;

  uint16_t buttons = 0;
  if (IsBindPressed(key_down, REXCVAR_GET(keybind_a)))
    buttons |= X_INPUT_GAMEPAD_A;
  if (IsBindPressed(key_down, REXCVAR_GET(keybind_b)))
    buttons |= X_INPUT_GAMEPAD_B;
  if (IsBindPressed(key_down, REXCVAR_GET(keybind_x)))
    buttons |= X_INPUT_GAMEPAD_X;
  if (IsBindPressed(key_down, REXCVAR_GET(keybind_y)))
    buttons |= X_INPUT_GAMEPAD_Y;
  if (IsBindPressed(key_down, REXCVAR_GET(keybind_left_shoulder)))
    buttons |= X_INPUT_GAMEPAD_LEFT_SHOULDER;
  if (IsBindPressed(key_down, REXCVAR_GET(keybind_right_shoulder)))
    buttons |= X_INPUT_GAMEPAD_RIGHT_SHOULDER;
  if (IsBindPressed(key_down, REXCVAR_GET(keybind_lstick_press)))
    buttons |= X_INPUT_GAMEPAD_LEFT_THUMB;
  if (IsBindPressed(key_down, REXCVAR_GET(keybind_rstick_press)))
    buttons |= X_INPUT_GAMEPAD_RIGHT_THUMB;
  if (IsBindPressed(key_down, REXCVAR_GET(keybind_back)))
    buttons |= X_INPUT_GAMEPAD_BACK;
  if (IsBindPressed(key_down, REXCVAR_GET(keybind_start)))
    buttons |= X_INPUT_GAMEPAD_START;
  if (IsBindPressed(key_down, REXCVAR_GET(keybind_guide)))
    buttons |= X_INPUT_GAMEPAD_GUIDE;
  if (IsBindPressed(key_down, REXCVAR_GET(keybind_dpad_up)))
    buttons |= X_INPUT_GAMEPAD_DPAD_UP;
  if (IsBindPressed(key_down, REXCVAR_GET(keybind_dpad_down)))
    buttons |= X_INPUT_GAMEPAD_DPAD_DOWN;
  if (IsBindPressed(key_down, REXCVAR_GET(keybind_dpad_left)))
    buttons |= X_INPUT_GAMEPAD_DPAD_LEFT;
  if (IsBindPressed(key_down, REXCVAR_GET(keybind_dpad_right)))
    buttons |= X_INPUT_GAMEPAD_DPAD_RIGHT;

  uint8_t lt = IsBindPressed(key_down, REXCVAR_GET(keybind_left_trigger)) ? 0xFF : 0;
  uint8_t rt = IsBindPressed(key_down, REXCVAR_GET(keybind_right_trigger)) ? 0xFF : 0;

  int32_t lx = 0;
  int32_t ly = 0;
  if (IsBindPressed(key_down, REXCVAR_GET(keybind_lstick_left)))
    lx -= INT16_MAX;
  if (IsBindPressed(key_down, REXCVAR_GET(keybind_lstick_right)))
    lx += INT16_MAX;
  if (IsBindPressed(key_down, REXCVAR_GET(keybind_lstick_up)))
    ly += INT16_MAX;
  if (IsBindPressed(key_down, REXCVAR_GET(keybind_lstick_down)))
    ly -= INT16_MAX;
  if (IsBindPressed(key_down, REXCVAR_GET(keybind_walk))) {
    lx /= 2;
    ly /= 2;
  }

  // The right stick follows the mouse velocity: camera turn speed rises with
  // how fast the mouse moves. Recomputed at most every few milliseconds so
  // several polls in one frame get the same value instead of the first one
  // taking all the movement, and averaged over two updates against jitter.
  double interval =
      std::chrono::duration<double>(now - stick_update_time_).count();
  if (interval >= 0.004) {
    interval = std::min(interval, 0.1);
    double velocity_x = mouse_dx_ / interval;
    double velocity_y = mouse_dy_ / interval;
    mouse_dx_ = 0;
    mouse_dy_ = 0;
    stick_update_time_ = now;
    double smoothed_x = 0.5 * (velocity_x + prev_velocity_x_);
    double smoothed_y = 0.5 * (velocity_y + prev_velocity_y_);
    prev_velocity_x_ = velocity_x;
    prev_velocity_y_ = velocity_y;
    stick_rx_ = MouseVelocityToStick(smoothed_x);
    stick_ry_ = MouseVelocityToStick(REXCVAR_GET(mnk_invert_y) ? smoothed_y : -smoothed_y);
  }
  int32_t rx = stick_rx_;
  int32_t ry = stick_ry_;
  // Camera keys push the stick all the way, over the mouse on that axis.
  int32_t key_rx = 0;
  int32_t key_ry = 0;
  if (IsBindPressed(key_down, REXCVAR_GET(keybind_rstick_left)))
    key_rx -= INT16_MAX;
  if (IsBindPressed(key_down, REXCVAR_GET(keybind_rstick_right)))
    key_rx += INT16_MAX;
  if (IsBindPressed(key_down, REXCVAR_GET(keybind_rstick_up)))
    key_ry += INT16_MAX;
  if (IsBindPressed(key_down, REXCVAR_GET(keybind_rstick_down)))
    key_ry -= INT16_MAX;
  if (key_rx) {
    rx = key_rx;
  }
  if (key_ry) {
    ry = key_ry;
  }

  auto clamp16 = [](int32_t v) -> int16_t {
    return static_cast<int16_t>(std::clamp(v, (int32_t)INT16_MIN, (int32_t)INT16_MAX));
  };

  packet_number_++;

  // Debugging (REX_MNK_LOG): log changes of the emulated controller state.
  static const bool state_log = std::getenv("REX_MNK_LOG") != nullptr;
  if (state_log) {
    static uint16_t logged_buttons = 0;
    static uint8_t logged_lt = 0, logged_rt = 0;
    static int32_t logged_lx = 0, logged_ly = 0, logged_rx_sign = 0;
    int32_t rx_sign = (rx > 0) - (rx < 0);
    if (buttons != logged_buttons || lt != logged_lt || rt != logged_rt || lx != logged_lx ||
        ly != logged_ly || rx_sign != logged_rx_sign) {
      REXLOG_INFO("[mnk] buttons {:04X} lt {} rt {} left stick {},{} right stick x {}", buttons,
                  lt, rt, lx, ly, rx);
      logged_buttons = buttons;
      logged_lt = lt;
      logged_rt = rt;
      logged_lx = lx;
      logged_ly = ly;
      logged_rx_sign = rx_sign;
    }
  }

  if (out_state) {
    out_state->packet_number = packet_number_;
    out_state->gamepad.buttons = buttons;
    out_state->gamepad.left_trigger = lt;
    out_state->gamepad.right_trigger = rt;
    out_state->gamepad.thumb_lx = clamp16(lx);
    out_state->gamepad.thumb_ly = clamp16(ly);
    out_state->gamepad.thumb_rx = clamp16(rx);
    out_state->gamepad.thumb_ry = clamp16(ry);
  }
  return X_ERROR_SUCCESS;
}

X_RESULT MnkInputDriver::SetState(uint32_t user_index, X_INPUT_VIBRATION* vibration) {
  if (!IsEnabled() || user_index != UserIndex()) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  return X_ERROR_SUCCESS;
}

X_RESULT MnkInputDriver::GetKeystroke(uint32_t user_index, uint32_t flags,
                                      X_INPUT_KEYSTROKE* out_keystroke) {
  if (!IsEnabled() || user_index != UserIndex()) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  std::lock_guard lock(state_mutex_);
  if (keystroke_queue_.empty()) {
    return X_ERROR_EMPTY;
  }
  if (out_keystroke) {
    *out_keystroke = keystroke_queue_.front();
  }
  keystroke_queue_.pop();
  return X_ERROR_SUCCESS;
}

void MnkInputDriver::EnqueueKeystroke(uint16_t vk_pad, bool down) {
  X_INPUT_KEYSTROKE ks = {};
  ks.virtual_key = vk_pad;
  ks.unicode = 0;
  ks.flags = down ? X_INPUT_KEYSTROKE_KEYDOWN : X_INPUT_KEYSTROKE_KEYUP;
  ks.user_index = static_cast<uint8_t>(UserIndex());
  ks.hid_code = 0;
  keystroke_queue_.push(ks);
}

void MnkInputDriver::CenterCursor() {
  if (!attached_window_)
    return;
  int32_t cx = static_cast<int32_t>(attached_window_->GetActualLogicalWidth() / 2);
  int32_t cy = static_cast<int32_t>(attached_window_->GetActualLogicalHeight() / 2);
  prev_mouse_x_ = cx;
  prev_mouse_y_ = cy;
#if REX_PLATFORM_WIN32
  auto* win32_window = dynamic_cast<rex::ui::Win32Window*>(attached_window_);
  if (win32_window && win32_window->hwnd()) {
    POINT pt = {static_cast<LONG>(cx), static_cast<LONG>(cy)};
    ClientToScreen(win32_window->hwnd(), &pt);
    SetCursorPos(pt.x, pt.y);
  }
#endif
}

int32_t MnkInputDriver::MouseVelocityToStick(double velocity) const {
  // Mouse counts per second for a full stick deflection at sensitivity 1.
  constexpr double kFullDeflectionVelocity = 4000.0;
  // Slower is sensor noise.
  constexpr double kMinVelocity = 20.0;
  double magnitude = std::abs(velocity);
  if (magnitude < kMinVelocity) {
    return 0;
  }
  double deflection =
      std::min(1.0, magnitude * REXCVAR_GET(mnk_sensitivity) / kFullDeflectionVelocity);
  double deadzone = REXCVAR_GET(mnk_stick_deadzone);
  double value = (deadzone + (1.0 - deadzone) * deflection) * double(INT16_MAX);
  return int32_t(velocity < 0.0 ? -value : value);
}

void MnkInputDriver::UpdateMouseCapture() {
  if (!attached_window_)
    return;

  bool should_capture =
      IsEnabled() && has_focus_ && is_active() && !rex::ui::IsOverlayInputActive();
  if (should_capture != capture_requested_) {
    capture_requested_ = should_capture;
    // Window mouse capture and cursor changes belong to the UI thread (GTK
    // isn't thread-safe, and Win32 capture is per thread).
    std::shared_ptr<std::atomic<bool>> alive = alive_;
    attached_window_->app_context().CallInUIThreadDeferred([this, alive, should_capture] {
      if (alive->load()) {
        ApplyMouseCapture(should_capture);
      }
    });
  }

  // Without relative motion events, keep the cursor centered so it doesn't
  // stop at the edges.
  if (mouse_captured_ && !attached_window_->ReportsRelativeMouseMotion()) {
    CenterCursor();
  }
}

void MnkInputDriver::ApplyMouseCapture(bool capture) {
  if (!attached_window_)
    return;
  if (capture && !mouse_captured_ && has_focus_) {
    {
      // No spike from movement before the capture.
      std::lock_guard lock(state_mutex_);
      mouse_dx_ = 0;
      mouse_dy_ = 0;
    }
    mouse_captured_ = true;
    attached_window_->SetCursorVisibility(rex::ui::Window::CursorVisibility::kHidden);
    attached_window_->CaptureMouse();
    if (std::getenv("REX_MNK_LOG")) {
      REXLOG_INFO("MnK: mouse captured");
    }
  } else if (!capture && mouse_captured_) {
    mouse_captured_ = false;
    attached_window_->SetCursorVisibility(rex::ui::Window::CursorVisibility::kVisible);
    attached_window_->ReleaseMouse();
    if (std::getenv("REX_MNK_LOG")) {
      REXLOG_INFO("MnK: mouse released");
    }
  }
}

void MnkInputDriver::SetKeyState(uint16_t vk, bool down) {
  if (vk < 256) {
    if (down && !key_down_[vk]) {
      key_hold_until_[vk] = std::chrono::steady_clock::now() + kMinPressDuration;
    }
    key_down_[vk] = down;
  }
}

void MnkInputDriver::OnKeyDown(rex::ui::KeyEvent& e) {
  // Releases still count while an overlay has input, so nothing stays held.
  if (!IsEnabled() || !has_focus_ || rex::ui::IsOverlayInputActive())
    return;
  std::lock_guard lock(state_mutex_);
  uint16_t vk = static_cast<uint16_t>(e.virtual_key());
  SetKeyState(vk, true);
}

void MnkInputDriver::OnKeyUp(rex::ui::KeyEvent& e) {
  if (!IsEnabled())
    return;
  std::lock_guard lock(state_mutex_);
  uint16_t vk = static_cast<uint16_t>(e.virtual_key());
  SetKeyState(vk, false);
}

void MnkInputDriver::OnMouseDown(rex::ui::MouseEvent& e) {
  if (!IsEnabled() || !has_focus_ || rex::ui::IsOverlayInputActive())
    return;
  std::lock_guard lock(state_mutex_);
  switch (e.button()) {
    case rex::ui::MouseEvent::Button::kLeft:
      SetKeyState(static_cast<uint16_t>(VirtualKey::kLButton), true);
      break;
    case rex::ui::MouseEvent::Button::kRight:
      SetKeyState(static_cast<uint16_t>(VirtualKey::kRButton), true);
      break;
    case rex::ui::MouseEvent::Button::kMiddle:
      SetKeyState(static_cast<uint16_t>(VirtualKey::kMButton), true);
      break;
    case rex::ui::MouseEvent::Button::kX1:
      SetKeyState(static_cast<uint16_t>(VirtualKey::kXButton1), true);
      break;
    case rex::ui::MouseEvent::Button::kX2:
      SetKeyState(static_cast<uint16_t>(VirtualKey::kXButton2), true);
      break;
    default:
      break;
  }
}

void MnkInputDriver::OnMouseUp(rex::ui::MouseEvent& e) {
  if (!IsEnabled())
    return;
  std::lock_guard lock(state_mutex_);
  switch (e.button()) {
    case rex::ui::MouseEvent::Button::kLeft:
      SetKeyState(static_cast<uint16_t>(VirtualKey::kLButton), false);
      break;
    case rex::ui::MouseEvent::Button::kRight:
      SetKeyState(static_cast<uint16_t>(VirtualKey::kRButton), false);
      break;
    case rex::ui::MouseEvent::Button::kMiddle:
      SetKeyState(static_cast<uint16_t>(VirtualKey::kMButton), false);
      break;
    case rex::ui::MouseEvent::Button::kX1:
      SetKeyState(static_cast<uint16_t>(VirtualKey::kXButton1), false);
      break;
    case rex::ui::MouseEvent::Button::kX2:
      SetKeyState(static_cast<uint16_t>(VirtualKey::kXButton2), false);
      break;
    default:
      break;
  }
}

void MnkInputDriver::OnMouseMove(rex::ui::MouseEvent& e) {
  if (!IsEnabled() || !has_focus_)
    return;
  std::lock_guard lock(state_mutex_);
  int32_t x = e.x();
  int32_t y = e.y();
  // Windows reporting relative motion send it separately (positions then only
  // reflect the window keeping the hidden cursor inside).
  if (!attached_window_ || !attached_window_->ReportsRelativeMouseMotion()) {
    mouse_dx_ += x - prev_mouse_x_;
    mouse_dy_ += y - prev_mouse_y_;
  }
  prev_mouse_x_ = x;
  prev_mouse_y_ = y;
}

void MnkInputDriver::OnMouseRelativeMove(rex::ui::MouseEvent& e) {
  if (!IsEnabled() || !has_focus_ || !mouse_captured_)
    return;
  std::lock_guard lock(state_mutex_);
  mouse_dx_ += e.x();
  mouse_dy_ += e.y();
}

void MnkInputDriver::OnMouseWheel(rex::ui::MouseEvent& e) {
  if (!IsEnabled() || !has_focus_ || rex::ui::IsOverlayInputActive())
    return;
  std::lock_guard lock(state_mutex_);
  auto until = std::chrono::steady_clock::now() + kWheelPressDuration;
  if (e.scroll_y() > 0) {
    wheel_up_until_ = until;
  } else if (e.scroll_y() < 0) {
    wheel_down_until_ = until;
  }
}

void MnkInputDriver::OnLostFocus(rex::ui::UISetupEvent&) {
  std::lock_guard lock(state_mutex_);
  has_focus_ = false;
  std::memset(key_down_, 0, sizeof(key_down_));
  mouse_dx_ = 0;
  mouse_dy_ = 0;
  if (mouse_captured_ && attached_window_) {
    mouse_captured_ = false;
    attached_window_->SetCursorVisibility(rex::ui::Window::CursorVisibility::kVisible);
    attached_window_->ReleaseMouse();
  }
}

void MnkInputDriver::OnGotFocus(rex::ui::UISetupEvent&) {
  std::lock_guard lock(state_mutex_);
  has_focus_ = true;
}

}  // namespace rex::input::mnk
