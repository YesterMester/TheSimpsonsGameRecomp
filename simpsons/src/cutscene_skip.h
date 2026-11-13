#pragma once

#include <cstdint>

struct PPCContext;
namespace rex {
class Runtime;
namespace ui {
class Window;
}
}  // namespace rex

// Controls drawn by the movie player's native font pass, with its normal
// pause, resume and stop/finish sequence.
void InitCutsceneSkip(rex::Runtime* runtime, rex::ui::Window* window);
void ShutdownCutsceneSkip();
void TickCutsceneSkip(PPCContext& ctx, uint8_t* base);
void DrawCutsceneSkip(PPCContext& ctx, uint8_t* base);
bool CutsceneSkipPromptPlayer(uint32_t* player, bool* keyboard);
