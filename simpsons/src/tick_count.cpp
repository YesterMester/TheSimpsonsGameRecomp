// The game's millisecond tick count.
//
// The game clock (sub_82690D60) measures each frame with the tick count in
// milliseconds and turns it into whole 59.94 Hz vblanks: n = round(ms /
// 16.683 - carry), 1 to 5, with the carry dropped once it exceeds 0.2.
// The tick count comes from sub_824324A8, the XDK's GetTickCount and the only
// reader of the kernel's KeTimeStampBundle, which a 1 ms repeating timer of
// the runtime keeps up to date.
//
// On Windows that timer can only be as punctual as the system timer lets its
// thread wake: with the default 15.6 ms resolution its callbacks come in
// bursts (REX_TIMER_STATS: 1000 a second, 7.3 ms late on average, up to
// 15.3 ms), so the tick count moves in steps of about 15.6 ms. Frames 16.7 ms
// apart then measure 15.6 or 31.2 ms; the clock rounds the first up to one
// vblank and counts the second as two, and in a 60 fps session it ran 1.075x
// real time, with all game logic, timers, animation and physics 7.5% fast.
//
// tick_count_precise answers GetTickCount from the runtime's clock at the
// moment of the call, the same clock the timer copies, so the tick count no
// longer depends on the system timer resolution.

#include <cstdint>

#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/ppc.h>

REXCVAR_DEFINE_BOOL(tick_count_precise, true, "Clock",
                    "Read the game's millisecond tick count from the clock at every call instead "
                    "of the kernel's copy, which a coarse system timer lets lag by up to 15 ms");

// GetTickCount: r3 = KeTimeStampBundle.TickCount.
REX_EXTERN(sub_824324A8);
REX_EXTERN(__imp__sub_824324A8);

REX_FUNC(sub_824324A8) {
  if (!REXCVAR_GET(tick_count_precise)) {
    __imp__sub_824324A8(ctx, base);
    return;
  }
  ctx.r3.u64 = rex::chrono::Clock::QueryGuestUptimeMillis();
}
