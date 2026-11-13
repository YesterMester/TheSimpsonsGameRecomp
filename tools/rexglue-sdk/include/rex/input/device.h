#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace rex::input {

enum class DeviceKind { kGamepad, kKeyboard, kTouch };
enum class PromptStyle { kXbox, kPlayStation, kNintendo, kSteamDeck, kKeyboard };

// The id identifies a physical input, rather than its current player slot.
// Serial numbers are preferred; devices without one use their SDL path.
struct InputDevice {
  std::string id;
  std::string name;
  DeviceKind kind = DeviceKind::kGamepad;
  PromptStyle prompts = PromptStyle::kXbox;
  uint32_t driver_index = 0;
  std::string start_label;
  std::string back_label;
};

enum class PromptButton {
  kA, kB, kX, kY, kLeftShoulder, kRightShoulder, kLeftTrigger, kRightTrigger,
  kStart, kBack, kLeftStickPress, kRightStickPress, kDpadUp, kDpadDown,
  kDpadLeft, kDpadRight, kLeftStick, kRightStick,
};

struct ButtonPrompt {
  PromptStyle style = PromptStyle::kXbox;
  PromptButton button = PromptButton::kA;
  std::string label;
};

ButtonPrompt MakeButtonPrompt(PromptStyle style, PromptButton button);
PromptStyle ParsePromptStyle(const std::string& name, PromptStyle detected);

}  // namespace rex::input
