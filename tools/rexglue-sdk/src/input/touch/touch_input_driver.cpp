#include <rex/input/touch/touch_input_driver.h>

#include <cstring>
#include <mutex>

namespace rex::input::touch {
namespace {
std::mutex state_mutex;
X_INPUT_STATE state{};
}  // namespace

void UpdateState(uint16_t buttons, uint8_t lt, uint8_t rt, int16_t lx, int16_t ly,
                 int16_t rx, int16_t ry) {
  std::lock_guard lock(state_mutex);
  X_INPUT_GAMEPAD pad{};
  pad.buttons = buttons;
  pad.left_trigger = lt;
  pad.right_trigger = rt;
  pad.thumb_lx = lx;
  pad.thumb_ly = ly;
  pad.thumb_rx = rx;
  pad.thumb_ry = ry;
  if (std::memcmp(&state.gamepad, &pad, sizeof(pad))) {
    state.gamepad = pad;
    state.packet_number = uint32_t(state.packet_number) + 1;
  }
}

void ResetState() { UpdateState(0, 0, 0, 0, 0, 0, 0); }

X_RESULT TouchInputDriver::GetCapabilities(uint32_t user_index, uint32_t,
                                          X_INPUT_CAPABILITIES* out_caps) {
  if (user_index) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  if (!out_caps) {
    return X_ERROR_BAD_ARGUMENTS;
  }
  *out_caps = {};
  out_caps->type = XINPUT_DEVTYPE_GAMEPAD;
  out_caps->sub_type = 1;
  out_caps->gamepad.buttons = 0xF3FF;
  out_caps->gamepad.left_trigger = out_caps->gamepad.right_trigger = 255;
  out_caps->gamepad.thumb_lx = out_caps->gamepad.thumb_ly = 32767;
  out_caps->gamepad.thumb_rx = out_caps->gamepad.thumb_ry = 32767;
  return X_ERROR_SUCCESS;
}

X_RESULT TouchInputDriver::GetState(uint32_t user_index, X_INPUT_STATE* out_state) {
  if (user_index) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  if (!out_state) {
    return X_ERROR_BAD_ARGUMENTS;
  }
  std::lock_guard lock(state_mutex);
  *out_state = state;
  if (!is_active()) {
    out_state->gamepad = {};
  }
  return X_ERROR_SUCCESS;
}

X_RESULT TouchInputDriver::SetState(uint32_t user_index, X_INPUT_VIBRATION*) {
  // Button haptics are handled by the Android view. No motor is advertised.
  return user_index ? X_ERROR_DEVICE_NOT_CONNECTED : X_ERROR_SUCCESS;
}

X_RESULT TouchInputDriver::GetKeystroke(uint32_t user_index, uint32_t,
                                       X_INPUT_KEYSTROKE*) {
  return user_index && user_index != 0xFF ? X_ERROR_DEVICE_NOT_CONNECTED : X_ERROR_EMPTY;
}

}  // namespace rex::input::touch
