// Hint texts that would show before the level's text is loaded (from
// frankyfife's fork).
//
// The tutorial hints in the cloud at the top of the screen come from the
// level's scripts: an EventText component calls the hint manager's show
// function (sub_828B3E30) with the text's hash, which looks the hash up in
// the text database (the singleton at 0x82D08B0C) and shows the string it
// finds, or an empty one. The database searches the text tables loaded at the
// time (sub_8282AC20).
//
// Starting a new game in Springfield, the house's first hint ("You have
// unlocked Bart! Press D-pad left to switch characters") fires about a second
// before the level runs, while only 2 of the 44 text tables are loaded: the
// lookup fails and the cloud stays empty, so the player does not learn that
// the switch opens the front door. A hint whose text is not loaded yet is now
// held back and shown once it is, checked every simulation tick; one whose
// text never appears still shows after a few seconds, empty as before.

#include "guest_memory.h"

#include <chrono>
#include <cstdint>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>

// Shows a hint: r3 = the hint manager, r4 = the text hash, f1 = seconds (0 =
// until hidden).
REX_EXTERN(sub_828B3E30);
REX_EXTERN(__imp__sub_828B3E30);
// Looks a text hash up in the loaded tables: r3 = the text database, r4 = the
// hash, r5 = out string, r6 = out flag; returns whether it was found.
REX_EXTERN(sub_8282AC20);

namespace {

constexpr uint32_t kTextDatabase = 0x82D08B0C;
constexpr auto kGiveUpAfter = std::chrono::seconds(5);

struct PendingHint {
  bool active = false;
  uint32_t manager = 0;
  uint32_t text = 0;
  double seconds = 0;
  std::chrono::steady_clock::time_point since;
};
PendingHint g_pending;  // game thread only

using simpsons::LoadGuestU32;

bool TextLoaded(PPCContext& ctx, uint8_t* base, uint32_t text) {
  const uint32_t database = LoadGuestU32(base, kTextDatabase);
  if (!database) {
    return false;
  }
  rex::CallFrame frame(ctx);
  // Room for the two results above the callee's frame.
  frame.ctx.r1.u32 -= 0x80;
  const uint32_t out = frame.ctx.r1.u32 + 0x60;
  frame.ctx.r3.u64 = database;
  frame.ctx.r4.u64 = text;
  frame.ctx.r5.u64 = out;
  frame.ctx.r6.u64 = out + 4;
  sub_8282AC20(frame, base);
  return (frame.ctx.r3.u32 & 0xFF) != 0;
}

void ShowNow(PPCContext& ctx, uint8_t* base, const PendingHint& hint) {
  rex::CallFrame frame(ctx);
  frame.ctx.r3.u64 = hint.manager;
  frame.ctx.r4.u64 = hint.text;
  frame.ctx.f1.f64 = hint.seconds;
  __imp__sub_828B3E30(frame, base);
}

}  // namespace

REX_FUNC(sub_828B3E30) {
  if (TextLoaded(ctx, base, ctx.r4.u32)) {
    g_pending.active = false;  // a later hint replaces one still waiting
    __imp__sub_828B3E30(ctx, base);
    return;
  }
  g_pending = {true, ctx.r3.u32, ctx.r4.u32, ctx.f1.f64, std::chrono::steady_clock::now()};
  REXLOG_INFO("[hint] text {:08X} not loaded yet, holding the hint back", ctx.r4.u32);
}

// Called by freecam.cpp's override of the simulation tick (sub_82691540) on
// the game thread, which runs once the level does.
void HintTextTick(PPCContext& ctx, uint8_t* base) {
  if (!g_pending.active) {
    return;
  }
  const bool loaded = TextLoaded(ctx, base, g_pending.text);
  if (!loaded && std::chrono::steady_clock::now() - g_pending.since < kGiveUpAfter) {
    return;
  }
  g_pending.active = false;
  REXLOG_INFO("[hint] showing text {:08X}{}", g_pending.text, loaded ? "" : " (still not loaded)");
  ShowNow(ctx, base, g_pending);
}
