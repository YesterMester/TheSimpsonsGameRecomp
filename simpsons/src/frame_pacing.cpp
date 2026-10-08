// Frame pacing.
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
//
// Frame rates other than whole fractions of 60 (and 0, unlimited) need more:
// the game measures frames in whole 59.94 Hz vblanks (at least one), its frame
// limiter (sub_826B7B70) waits whole vblank periods after the frame started,
// and swaps wait for the next 60 Hz guest vblank. For those rates, frames are
// counted in fractional vblanks (game_clock.cpp), the limiter waits for the
// target rate's frame period instead, the scheduler's deadline for background
// work (loading) moves with it, though never before the game's own frame
// period, and swaps are released at once. Physics keeps stepping 1/59.94 s
// (physics_step.cpp): the Havok step only steps when its time marker has
// passed the simulation, so it still steps 60 times a second at any frame
// rate.
//
// The menus count frames: a held direction repeats every few ticks, screens
// wait a few frames, the credits scroll a step per frame. Menu rates above 30
// run that logic on its 30 fps cadence and only draw the frames in between
// (see sub_82867D60). Their UI (APT) movies advance by time, but the time
// they were given lost up to a millisecond per frame (see sub_827C0F78).

#include "guest_memory.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>

#include <thread>

#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/ppc.h>

REXCVAR_DEFINE_INT32(frame_rate, 60, "GPU",
                     "Frame rate while a level runs (0 = unlimited). 60 and whole fractions of it "
                     "use the game's own vblank timing; other rates measure frames precisely")
    .range(0, 1000);

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

// The frame limiter: waits until r4 vblank periods (the scheduler's +144) have
// passed since r3, the frame's start timebase (low 32 bits).
REX_EXTERN(__imp__sub_826B7B70);
REX_EXTERN(sub_826B7B70);

// The frame body, once per frame in the front end and in levels: the update
// broadcast (sub_8269E130: the game clock, the menus' per-frame components),
// the tick message to the UI, the simulation tick (sub_82691540: the tick
// messages, the UI's among them), the frame itself (sub_826B8068: drawing,
// the swap, background work and the limiter; r3 = a flag the body computes)
// and sub_82862D50, which runs timers of its own.
REX_EXTERN(__imp__sub_82867D60);
REX_EXTERN(sub_82867D60);
REX_EXTERN(__imp__sub_826B8068);
REX_EXTERN(sub_826B8068);

// Advances the UI's (APT's) movies by r3 milliseconds (sub_827C0E38): it adds
// them up and plays a movie frame per frame period.
REX_EXTERN(__imp__sub_827C0F78);
REX_EXTERN(sub_827C0F78);

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
// Whether the last scheduled frame was in the front end. Main thread only.
bool g_in_menu = true;
// The clock's running flag (sub_826917A0).
constexpr uint32_t kClockRunning = 0x82CED760;
// The game clock's frame time: scaled, raw (seconds) and in milliseconds.
constexpr uint32_t kFrameDt = 0x82CED748;
constexpr uint32_t kRawFrameDt = 0x82CED744;
constexpr uint32_t kFrameMs = 0x82CED74C;
// Where the UI's tick handler (sub_827F74A8) calls sub_827C0F78 from.
constexpr uint32_t kUiTickReturn = 0x827F75B4;
// The menus' logic rate: 30 fps, two vblanks.
constexpr double kMenuLogicHz = 59.94 / 2.0;

// The frame rate in effect now: the menu rate in the front end, unless menus
// follow the gameplay rate.
int32_t ActiveFrameRate() {
  int32_t menu_rate = REXCVAR_GET(menu_frame_rate);
  return g_in_menu && menu_rate > 0 ? menu_rate : REXCVAR_GET(frame_rate);
}

// Rates the game's whole-vblank timing reaches by itself: 60 and its whole
// fractions.
bool WholeVblankRate(int32_t rate) {
  return rate > 0 && rate <= 60 && 60 % rate == 0;
}

// Guest timebase ticks per frame at the active rate, or 0 for unlimited.
uint64_t FramePeriodTicks() {
  int32_t rate = ActiveFrameRate();
  if (rate <= 0) {
    return 0;
  }
  return rex::chrono::Clock::guest_tick_frequency() / uint64_t(rate);
}

int64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

using simpsons::LoadGuestU32;
using simpsons::StoreGuestU32;

}  // namespace

bool FractionalVblanks() {
  return !WholeVblankRate(ActiveFrameRate());
}

namespace {

// Whether menu logic runs on its own cadence: menu rates above 30 fps.
bool MenuLogicCadence() {
  if (!g_in_menu || FreecamWorldFrozen()) {
    return false;
  }
  const int32_t rate = ActiveFrameRate();
  return rate <= 0 || rate > 30;
}

// The flag of the last frame the body ran itself. Main thread only.
uint32_t g_frame_flag = 0;
// When the menu logic last ran, in timebase ticks; 0 while it runs every
// frame.
uint64_t g_menu_logic_ticks = 0;

}  // namespace

REX_FUNC(sub_82453EB0) {
  if (FractionalVblanks()) {
    // Present interval 0 (bits 8-11): frames are paced by the limiter, not the
    // 60 Hz guest vblank, so the swap is released now.
    ctx.r3.u64 = ctx.r3.u32 & ~uint32_t(0xF00);
  } else if (REXCVAR_GET(present_late_frames_immediately)) {
    ctx.r3.u64 = (ctx.r3.u32 & ~uint32_t(0xFF)) | 100;
  }
  __imp__sub_82453EB0(ctx, base);
}

REX_FUNC(sub_826B7B70) {
  if (!FractionalVblanks()) {
    __imp__sub_826B7B70(ctx, base);
    return;
  }
  if (!LoadGuestU32(base, kClockRunning)) {
    return;
  }
  const uint64_t period = FramePeriodTicks();
  if (!period) {
    return;
  }
  // Frames end on a grid of the target period, not a period after the frame
  // started: the game marks the start a little after the previous frame's
  // limiter returned, which would add that time to every frame. A frame late
  // by more than a period starts the grid over instead of being made up for.
  static uint64_t next = 0;
  uint64_t now = rex::chrono::Clock::QueryGuestTickCount();
  if (!next || now > next + period || next > now + 2 * period) {
    next = now;
  }
  const double ticks_per_second = double(rex::chrono::Clock::guest_tick_frequency());
  while (now < next) {
    // Sleep while more than 2 ms remain, then yield until the deadline, so a
    // coarse system timer cannot make the frame late.
    const double remaining = double(next - now) / ticks_per_second;
    if (remaining > 0.002) {
      std::this_thread::sleep_for(std::chrono::duration<double>(remaining - 0.002));
    } else {
      std::this_thread::yield();
    }
    now = rex::chrono::Clock::QueryGuestTickCount();
  }
  next += period;
}

REX_FUNC(sub_827A55C0) {
  g_last_level_step_ns.store(NowNs(), std::memory_order_relaxed);
  HavokStep(ctx, base);
}

REX_FUNC(sub_82718710) {
  // Only the game's main thread runs the scheduler.
  static uint32_t game_periods = 0;
  static uint32_t applied_periods = 0;
  static int32_t logged_rate = -1;
  static bool logged_menu = false;
  // The game's own frame period in timebase ticks.
  static uint64_t own_period = 0;

  uint32_t scheduler = ctx.r3.u32;
  if (scheduler) {
    uint32_t address = scheduler + kSchedulerPeriodsPerFrame;
    uint32_t current = LoadGuestU32(base, address);
    // Whatever the game itself set, unless it is still the value set here.
    if (!game_periods || current != applied_periods) {
      game_periods = std::max<uint32_t>(current, 1);
    }
    int64_t last_step = g_last_level_step_ns.load(std::memory_order_relaxed);
    // The photo mode pauses the level, which stops the physics step too; it is
    // still a level and keeps the gameplay frame rate.
    g_in_menu = (!last_step || NowNs() - last_step > kMenuAfterNs) && !FreecamWorldFrozen();
    uint32_t refresh_bits = LoadGuestU32(base, kSchedulerRefreshHz);
    float refresh_hz;
    std::memcpy(&refresh_hz, &refresh_bits, sizeof(refresh_hz));
    if (!(refresh_hz >= 20.0f && refresh_hz <= 240.0f)) {
      refresh_hz = 59.94f;
    }
    own_period = uint64_t(double(game_periods) *
                          double(rex::chrono::Clock::guest_tick_frequency()) / double(refresh_hz));
    // 60 and its whole fractions are whole vblank periods per frame; other
    // rates are paced by the limiter and leave the game's own period.
    int32_t rate = ActiveFrameRate();
    uint32_t wanted = game_periods;
    if (WholeVblankRate(rate)) {
      wanted = std::max(game_periods, uint32_t(60 / rate));
    }
    if (wanted != current) {
      StoreGuestU32(base, address, wanted);
    }
    applied_periods = wanted;
    if (rate != logged_rate || g_in_menu != logged_menu) {
      logged_rate = rate;
      logged_menu = g_in_menu;
      const char* state = g_in_menu ? "no level running (menus)" : "level running";
      if (WholeVblankRate(rate)) {
        REXLOG_INFO("Frame pacing: {:.2f} fps, {}", double(refresh_hz) / double(wanted), state);
      } else {
        REXLOG_INFO("Frame pacing: {}, {}, frames measured in fractional vblanks",
                    rate > 0 ? fmt::format("{} fps", rate) : std::string("unlimited"), state);
      }
    }
  }
  __imp__sub_82718710(ctx, base);
  if (scheduler && FractionalVblanks()) {
    // The time left is what the scheduler's background tasks (sub_82718B70:
    // loading, which runs on this thread in slices after each frame) may use.
    // They get it until the limiter's deadline, but never less than the
    // game's own frame period: with no limit, or a frame that took longer
    // than the limit, they would get nothing and a loading screen would never
    // end. Tasks with nothing to do return at once, so frames only last that
    // long while something loads.
    const uint64_t period = std::max(FramePeriodTicks(), own_period);
    const uint32_t elapsed =
        uint32_t(rex::chrono::Clock::QueryGuestTickCount()) - LoadGuestU32(base, scheduler + 136);
    ctx.r3.u64 = elapsed < period ? uint32_t(period - elapsed) : 0;
    return;
  }
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

REX_FUNC(sub_826B8068) {
  g_frame_flag = ctx.r3.u32;
  __imp__sub_826B8068(ctx, base);
}

REX_FUNC(sub_82867D60) {
  const uint64_t now = rex::chrono::Clock::QueryGuestTickCount();
  const uint64_t period =
      uint64_t(double(rex::chrono::Clock::guest_tick_frequency()) / kMenuLogicHz);
  if (!MenuLogicCadence()) {
    g_menu_logic_ticks = 0;
    __imp__sub_82867D60(ctx, base);
    return;
  }
  if (!g_menu_logic_ticks || now - g_menu_logic_ticks >= period) {
    // A logic frame. Keep the cadence, unless frames are so slow that it
    // would have to catch up.
    g_menu_logic_ticks =
        g_menu_logic_ticks && now - g_menu_logic_ticks < 2 * period ? g_menu_logic_ticks + period
                                                                    : now;
    __imp__sub_82867D60(ctx, base);
    return;
  }
  // Only draw. The clock did not run, so nothing that advances by the frame
  // time while drawing may advance again.
  const uint32_t frame_dt = LoadGuestU32(base, kFrameDt);
  const uint32_t raw_dt = LoadGuestU32(base, kRawFrameDt);
  const uint32_t frame_ms = LoadGuestU32(base, kFrameMs);
  StoreGuestU32(base, kFrameDt, 0);
  StoreGuestU32(base, kRawFrameDt, 0);
  StoreGuestU32(base, kFrameMs, 0);
  ctx.r3.u64 = g_frame_flag;
  sub_826B8068(ctx, base);
  StoreGuestU32(base, kFrameDt, frame_dt);
  StoreGuestU32(base, kRawFrameDt, raw_dt);
  StoreGuestU32(base, kFrameMs, frame_ms);
}

REX_FUNC(sub_827C0F78) {
  // The UI tick handler passes the clock's raw frame time * 1000 truncated to
  // whole milliseconds: movies lost up to 1 ms a frame (1% slow at 30 fps, 4%
  // at 60 and 120) and stood still above 1000 fps. Carry the fraction.
  if (uint32_t(ctx.lr) == kUiTickReturn) {
    static double carry = 0.0;
    float raw_dt;
    const uint32_t raw_bits = LoadGuestU32(base, kRawFrameDt);
    std::memcpy(&raw_dt, &raw_bits, sizeof(raw_dt));
    const double ms = std::max(double(raw_dt * 1000.0f), 0.0) + carry;
    const double whole = std::floor(std::min(ms, 1e6));
    carry = std::clamp(ms - whole, 0.0, 1.0);
    ctx.r3.u64 = uint32_t(whole);
  }
  __imp__sub_827C0F78(ctx, base);
}
