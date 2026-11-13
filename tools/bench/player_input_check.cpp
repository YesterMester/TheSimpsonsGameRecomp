#include <rex/cvar.h>
#include <rex/input/input_system.h>
#include <rex/input/menu_input.h>
#include <rex/perf/counter.h>
#include <rex/ui/virtual_key.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace rex;
using namespace rex::input;

REXCVAR_DEFINE_STRING(input_check_string, "original", "Test", "Config round trip");
REXCVAR_DEFINE_INT32(input_check_integer, 10, "Test", "Config round trip").range(0, 100);

namespace {
void Require(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}

class Devices final : public InputDriver {
 public:
  Devices() : InputDriver(nullptr, 0) {
    devices = {
        {"pad-a",
         "PlayStation controller",
         DeviceKind::kGamepad,
         PromptStyle::kPlayStation,
         0,
         {},
         {}},
        {"pad-b", "Nintendo controller", DeviceKind::kGamepad, PromptStyle::kNintendo, 1, {}, {}},
        {"keyboard", "Keyboard & mouse", DeviceKind::kKeyboard, PromptStyle::kKeyboard, 2, {}, {}}};
  }
  X_STATUS Setup() override { return X_STATUS_SUCCESS; }
  std::vector<InputDevice> GetDevices() override { return devices; }
  X_RESULT GetCapabilities(uint32_t index, uint32_t, X_INPUT_CAPABILITIES* caps) override {
    if (index >= 3)
      return X_ERROR_DEVICE_NOT_CONNECTED;
    if (caps)
      *caps = {};
    return X_ERROR_SUCCESS;
  }
  X_RESULT GetState(uint32_t index, X_INPUT_STATE* state) override {
    if (index >= 3)
      return X_ERROR_DEVICE_NOT_CONNECTED;
    if (state)
      *state = states[index];
    return X_ERROR_SUCCESS;
  }
  X_RESULT SetState(uint32_t index, X_INPUT_VIBRATION*) override {
    rumble_index = index;
    return X_ERROR_SUCCESS;
  }
  X_RESULT GetKeystroke(uint32_t index, uint32_t, X_INPUT_KEYSTROKE* key) override {
    if (index != key_index)
      return X_ERROR_EMPTY;
    key_index = 99;
    *key = {};
    key->user_index = index;
    key->flags = X_INPUT_KEYSTROKE_KEYDOWN;
    return X_ERROR_SUCCESS;
  }
  std::vector<InputDevice> devices;
  X_INPUT_STATE states[3]{};
  uint32_t key_index = 99, rumble_index = 99;
};
}  // namespace

int main(int argc, char** argv) {
  rex::perf::Profiler::Startup();
  struct ProfilerLifetime {
    ~ProfilerLifetime() { rex::perf::Profiler::Shutdown(); }
  } profiler;
  try {
    InputSystem input(nullptr);
    auto devices = std::make_unique<Devices>();
    auto* driver = devices.get();
    input.AddDriver(std::move(devices));
    Require(input.AssignDevice(0, "keyboard") && input.AssignDevice(1, "pad-a"),
            "keyboard and pad assignment");
    X_INPUT_STATE first{}, second{};
    input.GetState(0, &first);
    input.GetState(1, &second);
    driver->states[2].gamepad.buttons = X_INPUT_GAMEPAD_A;
    driver->states[0].gamepad.buttons = X_INPUT_GAMEPAD_X;
    input.GetState(0, &first);
    input.GetState(1, &second);
    Require(
        first.gamepad.buttons == X_INPUT_GAMEPAD_A && second.gamepad.buttons == X_INPUT_GAMEPAD_X,
        "keyboard and controller must not merge or control the other player");
    Require(input.GetButtonPrompt(0, PromptButton::kA).style == PromptStyle::kKeyboard &&
                input.GetButtonPrompt(1, PromptButton::kA).style == PromptStyle::kPlayStation,
            "independent automatic prompt families");
    Require(input.AssignDevice(1, "keyboard"), "move keyboard to player two");
    Require(input.GetAssignment(0) == "pad-a" && input.GetAssignment(1) == "keyboard",
            "swap assigned inputs");
    driver->states[0] = {};
    driver->states[2] = {};
    input.GetState(0, &first);
    input.GetState(1, &second);
    driver->states[0].gamepad.buttons = X_INPUT_GAMEPAD_Y;
    driver->states[2].gamepad.buttons = X_INPUT_GAMEPAD_B;
    input.GetState(0, &first);
    input.GetState(1, &second);
    Require(
        first.gamepad.buttons == X_INPUT_GAMEPAD_Y && second.gamepad.buttons == X_INPUT_GAMEPAD_B,
        "swapped inputs must remain independent");
    driver->states[2].gamepad.buttons = X_INPUT_GAMEPAD_BACK;
    input.GetState(1, &second);
    Require(input.GetMenuPlayer() == 1, "to-do list prompts follow the player opening it");
    driver->states[0].gamepad.buttons = X_INPUT_GAMEPAD_START;
    input.GetState(0, &first);
    Require(input.GetMenuPlayer() == 0, "pause prompts follow the player opening them");
    X_INPUT_VIBRATION vibration{};
    input.SetState(0, &vibration);
    Require(driver->rumble_index == 0, "rumble follows the assigned physical controller");
    driver->key_index = 2;
    X_INPUT_KEYSTROKE key{};
    Require(input.GetKeystroke(1, 0, &key) == X_ERROR_SUCCESS && key.user_index == 1,
            "keyboard keystrokes carry their logical player index");
    driver->key_index = 2;
    Require(input.GetKeystroke(0xFF, 0, &key) == X_ERROR_SUCCESS && key.user_index == 1,
            "any-user polling must deliver player two's event");
    driver->key_index = 0;
    Require(input.GetKeystroke(0xFF, 0, &key) == X_ERROR_SUCCESS && key.user_index == 0,
            "any-user polling keeps player one's event independent");
    input.SetMenuInputCapture(true);
    input.GetState(0, &first);
    input.GetState(1, &second);
    Require(!first.gamepad.buttons && !second.gamepad.buttons, "menus capture both players");
    input.SetMenuInputCapture(false);
    input.GetState(0, &first);
    input.GetState(1, &second);
    Require(!first.gamepad.buttons && !second.gamepad.buttons,
            "closing a menu must not leak held buttons");
    driver->states[0] = {};
    driver->states[2] = {};
    input.GetState(0, &first);
    input.GetState(1, &second);
    driver->states[0].gamepad.buttons = X_INPUT_GAMEPAD_A;
    input.GetState(0, &first);
    Require(first.gamepad.buttons == X_INPUT_GAMEPAD_A, "buttons work again after release");
    Require(input.AssignDevice(1, "pad-b"), "second physical controller");
    Require(input.GetButtonPrompt(1, PromptButton::kA).label == "B",
            "Nintendo positional button labels");
    input.SetInputSetting("player1_prompts", "steamdeck");
    Require(input.GetButtonPrompt(0, PromptButton::kBack).style == PromptStyle::kSteamDeck &&
                input.GetButtonPrompt(1, PromptButton::kA).style == PromptStyle::kNintendo,
            "prompt override affects only its player");
    driver->devices.erase(driver->devices.begin() + 1);
    Require(input.GetState(1, &second) == X_ERROR_DEVICE_NOT_CONNECTED,
            "unplugging a player must not steal another player's device");
    driver->devices.insert(
        driver->devices.begin() + 1,
        {"pad-b", "Nintendo controller", DeviceKind::kGamepad, PromptStyle::kNintendo, 1, {}, {}});
    Require(input.GetState(1, &second) == X_ERROR_SUCCESS,
            "reconnect restores the reserved assignment");
    Require(!input.AssignDevice(0, "missing-device"), "unknown device rejected");
    cvar::SetFlagByName("player1_device", "auto");
    cvar::SetFlagByName("player2_device", "auto");
    cvar::SetFlagByName("mnk_mode", "true");
    // The fake keyboard's physical driver slot is 2, so reserve it explicitly
    // for Player 3 while exercising two automatic physical controllers.
    cvar::SetFlagByName("player3_device", "keyboard");
    driver->states[0] = {};
    driver->states[1] = {};
    input.GetState(0, &first);
    input.GetState(1, &second);
    driver->states[0].gamepad.buttons = X_INPUT_GAMEPAD_X;
    driver->states[1].gamepad.buttons = X_INPUT_GAMEPAD_Y;
    input.GetState(0, &first);
    input.GetState(1, &second);
    Require(
        first.gamepad.buttons == X_INPUT_GAMEPAD_X && second.gamepad.buttons == X_INPUT_GAMEPAD_Y,
        "automatic controllers stay in separate player slots");
    const auto first_controller = driver->devices.front();
    driver->devices.erase(driver->devices.begin());
    first = {};
    second = {};
    input.GetState(0, &first);
    input.GetState(1, &second);
    Require(first.gamepad.buttons == 0 && second.gamepad.buttons == X_INPUT_GAMEPAD_Y,
            "unplugging automatic player one must not steal player two's controller");
    driver->devices.insert(driver->devices.begin(), first_controller);
    input.GetState(0, &first);
    input.GetState(1, &second);
    Require(
        first.gamepad.buttons == X_INPUT_GAMEPAD_X && second.gamepad.buttons == X_INPUT_GAMEPAD_Y,
        "automatic reconnect restores each player's own controller");

    driver->states[0] = {};
    driver->states[1] = {};
    input.GetState(0, &first);
    input.GetState(1, &second);
    const auto menu_device = input.GetActiveDevice(0).id;
    QueueMenuButton(0, X_INPUT_GAMEPAD_START);
    input.GetState(0, &first);
    input.GetState(1, &second);
    Require(first.gamepad.buttons == X_INPUT_GAMEPAD_START && second.gamepad.buttons == 0,
            "native menu gestures reach only their requested player");
    Require(input.GetActiveDevice(0).id == menu_device,
            "native menu gestures preserve the assigned input and its prompts");
    X_INPUT_KEYSTROKE menu_key{};
    Require(input.GetKeystroke(0, 0, &menu_key) == X_ERROR_SUCCESS &&
                menu_key.virtual_key == uint16_t(ui::VirtualKey::kXInputPadStart) &&
                menu_key.flags == X_INPUT_KEYSTROKE_KEYDOWN && menu_key.user_index == 0,
            "native menu gestures also reach games that read keystrokes");
    ClearMenuButtons();
    input.GetState(0, &first);
    Require(first.gamepad.buttons == 0, "closing a menu cancels its pending button gesture");
    Require(input.GetKeystroke(0, 0, &menu_key) == X_ERROR_SUCCESS &&
                menu_key.flags == X_INPUT_KEYSTROKE_KEYUP,
            "cancelling a delivered menu gesture releases its key");

    if (argc > 1) {
      const std::filesystem::path path = argv[1];
      const std::string original =
          "# launcher marker\ninput_check_integer = 10\n# settings version: 7\n"
          "unknown_future_setting = 'keep me'\n[unrelated]\nvalue = 9\n";
      {
        std::ofstream file(path);
        file << original;
      }
      Require(cvar::SaveConfigPatch(path, {{"input_check_integer", "25"},
                                           {"input_check_string",
                                            "A controller with \\\"quotes\\\" and a newline\n"}}),
              "save selected settings");
      std::ifstream file(path);
      const std::string saved((std::istreambuf_iterator<char>(file)), {});
      Require(saved.find("# settings version: 7") != std::string::npos &&
                  saved.find("# launcher marker") != std::string::npos &&
                  saved.find("unknown_future_setting = 'keep me'") != std::string::npos &&
                  saved.find("[unrelated]\nvalue = 9") != std::string::npos,
              "preserve launcher markers and unknown settings");
      cvar::LoadConfig(path);
      Require(cvar::GetFlagByName("input_check_integer") == "25", "saved number round trip");
      Require(!cvar::SaveConfigPatch(path, {{"input_check_integer", "1000"}}),
              "invalid settings must not overwrite config");
      std::ifstream unchanged_file(path);
      Require(std::string((std::istreambuf_iterator<char>(unchanged_file)), {}) == saved,
              "failed save leaves config untouched");
    }
    std::cout
        << "Player assignment, prompts, hotplug, menu capture and settings persistence passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
