#pragma once

#include <rex/input/input_driver.h>

namespace rex::input::touch {

// Called by the Android touch view. A whole pad snapshot keeps combinations
// (move, camera and attacks at once) consistent across the UI and game threads.
void UpdateState(uint16_t buttons, uint8_t lt, uint8_t rt, int16_t lx, int16_t ly,
                 int16_t rx, int16_t ry);
void ResetState();

class TouchInputDriver final : public InputDriver {
 public:
  TouchInputDriver() : InputDriver(nullptr, 0) {}
  X_STATUS Setup() override { return X_STATUS_SUCCESS; }
  std::vector<InputDevice> GetDevices() override {
    return {{"touch", "Touch controls", DeviceKind::kTouch, PromptStyle::kXbox, 0}};
  }
  X_RESULT GetCapabilities(uint32_t user_index, uint32_t flags,
                           X_INPUT_CAPABILITIES* out_caps) override;
  X_RESULT GetState(uint32_t user_index, X_INPUT_STATE* out_state) override;
  X_RESULT SetState(uint32_t user_index, X_INPUT_VIBRATION* vibration) override;
  X_RESULT GetKeystroke(uint32_t user_index, uint32_t flags,
                        X_INPUT_KEYSTROKE* out_keystroke) override;
};

}  // namespace rex::input::touch
