#include <rex/input/device.h>

#include <array>
#include <rex/cvar.h>

namespace rex::input {

PromptStyle ParsePromptStyle(const std::string& name, PromptStyle detected) {
  if (name == "xbox") return PromptStyle::kXbox;
  if (name == "playstation") return PromptStyle::kPlayStation;
  if (name == "nintendo") return PromptStyle::kNintendo;
  if (name == "steamdeck") return PromptStyle::kSteamDeck;
  if (name == "keyboard") return PromptStyle::kKeyboard;
  return detected;
}

ButtonPrompt MakeButtonPrompt(PromptStyle style, PromptButton button) {
  constexpr std::array<const char*, 18> xbox = {
      "A", "B", "X", "Y", "LB", "RB", "LT", "RT", "Menu", "View", "LS", "RS",
      "Up", "Down", "Left", "Right", "Left stick", "Right stick"};
  constexpr std::array<const char*, 18> playstation = {
      "Cross", "Circle", "Square", "Triangle", "L1", "R1", "L2", "R2", "Options",
      "Share", "L3", "R3", "Up", "Down", "Left", "Right", "Left stick", "Right stick"};
  constexpr std::array<const char*, 18> nintendo = {
      "B", "A", "Y", "X", "L", "R", "ZL", "ZR", "+", "-", "LS", "RS",
      "Up", "Down", "Left", "Right", "Left stick", "Right stick"};
  constexpr std::array<const char*, 18> bindings = {
      "keybind_a", "keybind_b", "keybind_x", "keybind_y", "keybind_left_shoulder",
      "keybind_right_shoulder", "keybind_left_trigger", "keybind_right_trigger",
      "keybind_start", "keybind_back", "keybind_lstick_press", "keybind_rstick_press",
      "keybind_dpad_up", "keybind_dpad_down", "keybind_dpad_left", "keybind_dpad_right",
      "keybind_lstick_up", "keybind_rstick_up"};
  const auto index = static_cast<size_t>(button);
  if (index >= xbox.size()) return {style, button, "?"};
  std::string label = xbox[index];
  if (style == PromptStyle::kPlayStation) label = playstation[index];
  if (style == PromptStyle::kNintendo) label = nintendo[index];
  if (style == PromptStyle::kSteamDeck && button == PromptButton::kBack) label = "View";
  if (style == PromptStyle::kKeyboard) {
    label = cvar::GetFlagByName(bindings[index]);
    label = label.substr(0, label.find(','));
    if (button == PromptButton::kLeftStick) label = "Move";
    if (button == PromptButton::kRightStick) label = "Mouse";
    if (label.empty()) label = "Unbound";
    if (label == "Return") label = "Enter";
    if (label == "Control") label = "Ctrl";
    if (label == "Escape") label = "Esc";
  }
  return {style, button, std::move(label)};
}

}  // namespace rex::input
