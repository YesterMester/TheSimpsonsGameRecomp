// Menu frame pacing.
//
// The game shipped at 30 fps: its frame scheduler waits two vblanks per frame.
// The 60 FPS patch (the "li r4,1" hand patch in sub_82867A48) makes that one
// vblank everywhere. Gameplay copes, since its logic runs on real time and
// physics_step.cpp evens out the physics step, but the front end runs its
// logic once per frame:
// at 60 fps menus scroll and repeat inputs twice as fast as they were made
// for, and the title screen, which costs a little more than one 60 Hz frame,
// alternates 17 and 33 ms frames. While no level is simulating, this puts the
// frame scheduler back on the console's 30 fps cadence.

#include "guest_memory.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>

#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/ppc.h>

REXCVAR_DEFINE_INT32(menu_frame_rate, 30, "GPU",
                     "Frame rate of menus, the title screen and loading screens, where the game "
                     "runs its logic once per frame and was made for 30 fps (0 = same as gameplay)")
    .range(0, 240);

REXCVAR_DEFINE_BOOL(frame_pacing_vblank_lock, false, "GPU",
                    "Put the game's frame deadlines on the vblank grid. The game times its frames "
                    "on its own 59.94 Hz clock, which drifts against the 60 Hz vblank its swaps "
                    "wait for, so for seconds at a time frames finished right at a vblank and "
                    "reached the screen at uneven times");

REXCVAR_DEFINE_INT32(frame_pacing_vblank_offset_us, 0, "GPU",
                     "With frame_pacing_vblank_lock, where frame deadlines go relative to the "
                     "vblank, in microseconds (negative = before it)")
    .range(-16000, 16000);

REXCVAR_DEFINE_BOOL(present_late_frames_immediately, true, "GPU",
                    "Show a frame that is finished just after its vblank right away instead of "
                    "holding it until the next vblank. The game only allowed that within 0% of a "
                    "frame (on the console it tears), so a frame late by a fraction of a "
                    "millisecond was shown a whole frame late: a stutter. The host presents "
                    "without tearing either way");

// freecam.cpp: whether the photo mode has paused the level.
bool FreecamWorldFrozen();

// Steps the Havok world by f1 seconds of frame time. Runs every frame while a
// level is live and never in the front end.
REX_EXTERN(sub_827A55C0);
// physics_step.cpp: the step itself, through the shipped code.
void HavokStep(PPCContext& ctx, uint8_t* base);

// The XDK's swap callback (D3D's swap queue), called by the GPU interrupt the
// command stream raises when it reaches a Swap, right before the GPU waits
// for the swap to be released. r3 = front buffer address | present interval
// << 8 | immediate threshold: if the target vblank has already passed, the
// swap is released now when less than this percentage of a frame has passed
// since the last vblank, otherwise at the next vblank. The game passes 0.
REX_EXTERN(__imp__sub_82453EB0);
REX_EXTERN(sub_82453EB0);

// Returns the time until the frame scheduler's next deadline. r3 = the
// scheduler, which holds the frame's start timebase at +136 and its periods
// per frame at +144, read on every call. The period is the game's own fixed
// refresh rate (59.94 Hz), whatever rate the guest vblank runs at.
REX_EXTERN(__imp__sub_82718710);
REX_EXTERN(sub_82718710);

// Marks the start of a frame: the scheduler's start timebase (+136) = now.
REX_EXTERN(__imp__sub_82718788);
REX_EXTERN(sub_82718788);

// From the runtime, referenced weakly so this still runs with runtimes that
// don't have them.
extern "C" bool rex_graphics_get_vblank_schedule(uint64_t* last_vblank_ticks,
                                                 uint64_t* interval_ticks) __attribute__((weak));
namespace rex::perf {
void TraceEvent(const char* name, uint64_t arg) __attribute__((weak));
}  // namespace rex::perf

namespace {

constexpr uint32_t kSchedulerPeriodsPerFrame = 144;
// The refresh rate (float) the scheduler divides the timebase frequency by.
constexpr uint32_t kSchedulerRefreshHz = 0x82CF0404;

// Without a physics step for this long, no level is running.
constexpr int64_t kMenuAfterNs = 750'000'000;

std::atomic<int64_t> g_last_level_step_ns{0};

int64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

using simpsons::LoadGuestU32;
using simpsons::StoreGuestU32;

}  // namespace

REX_FUNC(sub_82453EB0) {
  if (REXCVAR_GET(present_late_frames_immediately)) {
    ctx.r3.u64 = (ctx.r3.u32 & ~uint32_t(0xFF)) | 100;
  }
  __imp__sub_82453EB0(ctx, base);
}

REX_FUNC(sub_827A55C0) {
  g_last_level_step_ns.store(NowNs(), std::memory_order_relaxed);
  HavokStep(ctx, base);
}

REX_FUNC(sub_82718710) {
  // Only the game's main thread runs the scheduler.
  static uint32_t game_periods = 0;
  static uint32_t applied_periods = 0;
  static bool in_menu = false;

  int32_t menu_rate = REXCVAR_GET(menu_frame_rate);
  uint32_t scheduler = ctx.r3.u32;
  if (menu_rate > 0 && scheduler) {
    uint32_t address = scheduler + kSchedulerPeriodsPerFrame;
    uint32_t current = LoadGuestU32(base, address);
    // Whatever the game itself set, unless it is still the menu value.
    if (!game_periods || current != applied_periods) {
      game_periods = std::max<uint32_t>(current, 1);
    }
    int64_t last_step = g_last_level_step_ns.load(std::memory_order_relaxed);
    // The photo mode pauses the level, which stops the physics step too; it is
    // still a level and keeps the gameplay frame rate.
    bool menu = (!last_step || NowNs() - last_step > kMenuAfterNs) && !FreecamWorldFrozen();
    uint32_t refresh_bits = LoadGuestU32(base, kSchedulerRefreshHz);
    float refresh_hz;
    std::memcpy(&refresh_hz, &refresh_bits, sizeof(refresh_hz));
    if (!(refresh_hz >= 20.0f && refresh_hz <= 240.0f)) {
      refresh_hz = 59.94f;
    }
    uint32_t menu_periods =
        std::max(game_periods, uint32_t(std::lround(double(refresh_hz) / double(menu_rate))));
    uint32_t wanted = menu ? menu_periods : game_periods;
    if (wanted != current) {
      StoreGuestU32(base, address, wanted);
    }
    applied_periods = wanted;
    if (menu != in_menu) {
      in_menu = menu;
      REXLOG_INFO("Frame pacing: {:.2f} fps, {}", double(refresh_hz) / double(wanted),
                  menu ? "no level running (menus)" : "level running");
    }
  }
  __imp__sub_82718710(ctx, base);
  // r3 = timebase ticks until the frame's deadline. Move the deadline to the
  // nearest vblank (plus the offset), so frames start in step with the
  // vblanks that release their swaps.
  uint64_t last_vblank = 0, vblank_interval = 0;
  if (REXCVAR_GET(frame_pacing_vblank_lock) && rex_graphics_get_vblank_schedule &&
      rex_graphics_get_vblank_schedule(&last_vblank, &vblank_interval) && vblank_interval) {
    uint64_t now = rex::chrono::Clock::QueryGuestTickCount();
    int64_t offset_ticks = int64_t(REXCVAR_GET(frame_pacing_vblank_offset_us)) *
                           int64_t(rex::chrono::Clock::guest_tick_frequency()) / 1000000;
    int64_t interval = int64_t(vblank_interval);
    int64_t grid_base = int64_t(last_vblank) + offset_ticks;
    int64_t deadline = int64_t(now) + int64_t(ctx.r3.u32);
    // Nearest grid point to the game's deadline, then not in the past.
    int64_t from_base = deadline - grid_base;
    int64_t steps = from_base >= 0 ? (from_base + interval / 2) / interval
                                   : -((-from_base + interval / 2) / interval);
    int64_t snapped = grid_base + steps * interval;
    while (snapped <= int64_t(now)) {
      snapped += interval;
    }
    ctx.r3.u64 = uint32_t(snapped - int64_t(now));
  }
}

REX_FUNC(sub_82718788) {
  __imp__sub_82718788(ctx, base);
  if (rex::perf::TraceEvent) {
    rex::perf::TraceEvent("frame_start", 0);
  }
}
