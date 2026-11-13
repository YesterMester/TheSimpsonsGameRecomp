#pragma once

#include <cstdint>
#include <filesystem>

struct PPCContext;
namespace rex {
class Runtime;
namespace ui {
class Window;
}
}  // namespace rex

void InitGameMenu(rex::Runtime* runtime, rex::ui::Window* window,
                  const std::filesystem::path& config);
void ShutdownGameMenu();
void TickGameMenu(PPCContext& ctx, uint8_t* base);

// A title replacement for the original Options text field, if this page owns it.
uint32_t GameMenuTitleForText(const uint8_t* base, uint32_t text);

bool GameMenuIsVisible();
uint32_t GameMenuPromptPlayer();
bool GameMenuUsesKeyboard();
bool GameMenuCapturingInput();
void GameMenuSetKeyboardInput(bool keyboard);
