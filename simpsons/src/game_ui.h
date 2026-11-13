#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

struct PPCContext;
namespace rex {
class Runtime;
namespace ui {
class Window;
}
}  // namespace rex
namespace canvas {
class Canvas;
}

// Screens drawn inside the game's own frame, with its renderer and fonts
// (game_canvas.h): the debug menu and the achievements. They are drawn after
// the game's own menus every frame, read the controllers through the
// XInputGetState thunk (freecam.cpp) and the keyboard, mouse and touch through
// the window, and run on the game's thread, which reads input, updates and
// draws the game.
namespace game_ui {

enum class Screen { kNone, kDebugMenu, kAchievements, kSaves };

// XInput's buttons.
constexpr uint16_t kPadUp = 0x0001, kPadDown = 0x0002, kPadLeft = 0x0004, kPadRight = 0x0008;
constexpr uint16_t kPadStart = 0x0010, kPadBack = 0x0020, kPadLB = 0x0100, kPadRB = 0x0200;
constexpr uint16_t kPadA = 0x1000, kPadB = 0x2000, kPadX = 0x4000, kPadY = 0x8000;

void Init(rex::Runtime* runtime, rex::ui::Window* window);
void Shutdown();

// Any thread.
void Open(Screen screen);
void Toggle(Screen screen);
void Close();
Screen Current();
bool IsOpen();
// A short message at the top of the screen.
void Notify(std::string text);
// The achievements can be looked at: a save is loaded.
bool InGame();
// The front end's state (0x82D08C98) while a level runs. It is 9 once a save
// has been loaded, but 7 after starting a New Game, until a later load; menus
// over the level show through 0x82D572DC instead (1 while one is up).
inline bool LevelRunning(uint32_t front_end_state) {
  return front_end_state == 7 || front_end_state == 9;
}

// The XInputGetState thunk. True when the game must see *game_buttons instead
// of the buttons pressed; with *keep_sticks it still gets the sticks and
// triggers (the debug menu lets the family move).
bool FilterController(uint32_t user, uint16_t buttons, uint8_t left_trigger,
                      uint8_t right_trigger, int16_t lx, int16_t ly, int16_t rx, int16_t ry,
                      uint16_t* game_buttons, bool* keep_sticks);
// The thunk again, for a controller that isn't connected: true, with the
// buttons, while a press for the game (PressForGame) is made on it.
bool InjectedButtons(uint32_t user, uint16_t* buttons);

// Presses a button for the game on its first controller, after any presses
// queued before (each is held a moment, then let go). The saves screen works
// the game's own menus with them.
void PressForGame(uint16_t button);
// Whether presses for the game are still being made.
bool PressingForGame();
void CancelPressesForGame();
// While on, the game gets no input but the presses made for it.
void BlockGameInput(bool on);

// --- For the screens -------------------------------------------------------

// This frame's input for the open screen.
struct Input {
  bool up = false, down = false, left = false, right = false;
  bool accept = false, back = false, previous = false, next = false, x = false, y = false;
  // Right stick, triggers, mouse wheel or a touch drag, in rows.
  float scroll = 0;
  // A click or tap this frame, in the game image's pixels.
  bool tapped = false;
  float tap_x = 0, tap_y = 0;
  // Where the mouse is, while it moves over the game, and whether it moved.
  bool hovering = false, hover_moved = false;
  float hover_x = 0, hover_y = 0;
  // The last input was a mouse or touch rather than a controller or keys.
  bool pointer = false;
  bool touch = false;
  // Start pressed.
  bool start = false;
  // Typing, while a screen takes text (TakeText): what was typed this frame,
  // Backspace and Return.
  std::string text;
  bool erase = false, enter = false;
  bool find = false, remove = false;
};

// The open screen takes typed text: the keyboard's letters, space, Return and
// Backspace type instead of moving through the screen.
void TakeText(bool on, bool editing = false);

// What a screen draws with.
struct Frame {
  PPCContext& ctx;
  uint8_t* base;
  canvas::Canvas& c;
  const Input& input;
  float s;   // pixels per unit of a 1280x720 layout
  float dt;  // seconds since the last frame
  uint32_t title_font, body_font;
};

// The game thread at the start of its frame (game_thread.cpp).
void Tick(PPCContext& ctx, uint8_t* base);

// The front end's sounds: "ui_move_up_down", "ui_select_a_button",
// "ui_back_b_button", "ui_toggle_on_off".
void Sound(const char* name);

// A controller button as the player's controller names it: A, B, X, Y, LB,
// RB or Start (the keys the screens use on a keyboard). Returns its width.
float Glyph(Frame& f, float x, float y, float size, std::string_view button);
// The width Glyph draws a button at.
float GlyphWidth(Frame& f, float size, std::string_view button);
// The same loaded game fonts for native movie controls and game screens.
void ScreenFonts(canvas::Canvas& c, uint32_t* title, uint32_t* body, const uint8_t* base);
// Whether the current menu is being driven by keyboard or mouse.
bool KeyboardPrompts();
// A padlock.
void Padlock(Frame& f, float cx, float cy, float size, uint32_t color);
// Text with a drop shadow.
float ShadowText(Frame& f, float x, float y, float size, uint32_t color, std::string_view text,
                 int align = 0);

// The screens (debug_menu.cpp, achievements_screen.cpp, saves_screen.cpp):
// false to close.
bool DrawDebugMenu(Frame& f, bool appearing);
bool DrawAchievements(Frame& f, bool appearing);
// Save screens use the original APT menu, without a canvas or overlay capture.
Input ReadSavesInput();
uint32_t SavesTextForText(const uint8_t* base, uint32_t text);
bool SavesMenuEvent(PPCContext& ctx, uint8_t* base, uint32_t screen, int key);
bool DrawNativeSavesWidget(PPCContext& ctx, uint8_t* base, uint32_t text);
bool SavesKeyboard();
int SavesKeyboardRow(uint32_t text);
bool SavesTextStyle(PPCContext& ctx, const uint8_t* base, uint32_t text, uint32_t replacement,
                    float* scale, uint32_t* colour);
void SavesKeyboardTap(PPCContext& ctx, uint8_t* base, int row, float position);
// Drawn over everything, screen or not, while the saves screen loads a save:
// false when there's nothing to draw.
bool DrawSavesCover(Frame& f);
// The native save menu's work at the start of a frame (saves_screen.cpp),
// including the original game's slot/load/Continue sequence.
void TickSaves(PPCContext& ctx, uint8_t* base);
// Whether the saves screen covers the game (loading a save).
bool SavesCovering();
void InitSaves(rex::Runtime* runtime);
// The pause menu's Save Game chosen (pause_menu.cpp, with Save anywhere).
void OpenSavesFromPause();

// The debug menu's list, in the style of the classic trainers: a banner, the
// page and its position, then rows; the selected row's help goes underneath.
// Immediate mode: each row is declared every frame and reports its own input.
class List {
 public:
  struct State {
    int selected = 0, top = 0;
    float scroll = 0;
    // Last frame's layout, for the mouse and touch.
    std::vector<float> row_y;
    std::vector<bool> selectable;
    float left = 0, width = 0;
  };
  List(Frame& f, State& state, std::string_view page, bool cheats);

  // detail: a little text on the right, such as a count.
  bool Action(std::string_view label, std::string_view help = {}, bool cheat = false,
              std::string_view detail = {});
  // True when it should be switched.
  bool Toggle(std::string_view label, bool on, std::string_view help = {}, bool cheat = false);
  bool Submenu(std::string_view label, std::string_view help = {}, bool cheat = false);
  // -1 or +1 when it should change (A and taps on its right go forward).
  int Value(std::string_view label, std::string_view value, std::string_view help = {},
            bool cheat = false);
  void Divider(std::string_view label);
  // A line of information, not selectable.
  void Info(std::string_view label, std::string_view value = {});

  // Draws the list with its top-left corner at x, y.
  void Draw(float x, float y, float width, std::string_view status, float status_alpha);
  // The page can go back: a touch has the arrow in its title bar for it.
  void AllowBack(bool allow) { allow_back_ = allow; }
  // Back was pressed or tapped on this page, or the close button tapped.
  bool back() const { return back_; }
  bool close() const { return close_; }
  // A row that needs the cheats was used with them off.
  bool locked_use() const { return locked_use_; }

 private:
  enum class Kind { kAction, kToggle, kSubmenu, kValue, kDivider, kInfo };
  struct Row {
    Kind kind;
    std::string label, value, help;
    bool on = false, locked = false;
  };
  bool Selected(size_t index) const { return int(index) == state_.selected; }
  // Whether the row being added is the one acted on this frame.
  bool Activated(size_t index, bool locked);
  Frame& f_;
  State& state_;
  std::string page_;
  bool cheats_;
  std::vector<Row> rows_;
  bool accept_ = false, back_ = false, close_ = false, locked_use_ = false, allow_back_ = false;
  int change_ = 0;
  int tapped_row_ = -1;
  float tap_side_ = 0;  // -1 left half, +1 right half
};

}  // namespace game_ui
