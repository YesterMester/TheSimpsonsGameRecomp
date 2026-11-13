#pragma once

#include <cstdint>

struct PPCContext;
namespace rex {
class Runtime;
namespace ui {
class Window;
}
}  // namespace rex

void InitGameMenuPointer(rex::Runtime* runtime, rex::ui::Window* window);
void ShutdownGameMenuPointer();
void TickGameMenuPointer(PPCContext& ctx, uint8_t* base);
void RecordGameMenuPointerText(const uint8_t* base, uint32_t text);
void RecordGameMenuPointerButton(float x, float y, float width, float height, int key);
void RecordGameMenuPointerSaveRow(float x, float y, float width, float height, int row);
void RecordGameMenuPointerSaveKey(float x, float y, float width, float height, int row, int column);
void GameMenuPointerScreenClosed(uint32_t screen);
void InvalidateGameMenuPointer();
bool GameMenuPointerActive();
bool GameMenuPointerUsesKeyboard();
void GameMenuPointerInputSource(int source);
bool GameMenuPointerUsesTouch();
// The deepest of the game's screens that is showing (game thread), or 0.
uint32_t GameMenuPointerScreen();
