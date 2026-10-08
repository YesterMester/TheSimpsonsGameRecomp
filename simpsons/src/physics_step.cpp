// Havok physics step at 60 fps.
//
// The game steps its Havok world once per frame in sub_827A55C0: it moves the
// world's frame time marker on by f1 seconds, then steps the simulation until
// it reaches the marker. f1 is the game clock's frame time (0x82CED748, whole
// 59.94 Hz vblanks times the time scale) smoothed by sub_827A66B8 (s = 0.15 *
// dt + 0.85 * s). If f1 is at most T (0x82159238, 1/29.97 s), it takes one
// step of T; otherwise two steps of f1 / 2 (f1 below 2/29.97 s) or steps of
// 1/59.94 s (0x8215923C). In float the smoothing settles a few ulps off the
// frame time, on the side it comes from: above it after any longer frame (a
// level's first frame, a dropped frame), below it only when it rises from
// shorter ones.
//
// - original: the shipped code. At 30 fps two 16.7 ms steps per frame once the
//   smoothed time has come down from a longer frame (0x3D08AB89 against
//   T = 0x3D08AB86), one 33.4 ms step if it rose from below; at 60 fps one
//   33.4 ms step every other frame, so physics objects move at 30 Hz and run
//   up to a frame ahead.
// - legacy: T = 1/59.94 s, the 60 FPS fix this build used to carry as a hand
//   edit in the generated code. It was meant to give one 16.7 ms step per
//   60 Hz frame, but the smoothed frame time settles at 0x3C88AB89, above
//   T = 0x3C88AB86, so every frame takes the other branch: two steps of
//   8.3 ms, about 120 a second, a step size the console never used.
// - steady (the default): T = 1/59.94 s, and the step gets the clock's frame
//   time itself instead of its smoothed copy: one 16.7 ms step per vblank the
//   frame took (two of 25 ms for a three-vblank frame). That is 60 steps of
//   16.7 ms a second at 60 and at 30 fps, the step size and rate of the
//   console's usual mode, and physics time follows the game clock exactly.
//   While the time scale is 0 the game's own path runs.
//   Frames counted in fractional vblanks (game_clock.cpp) can be any length.
//   One no longer than T gets a step of T when the simulation is behind the
//   marker, so above 60 fps physics still steps 60 times a second. One longer
//   than T but shorter than H (a static of the step, set from 0x82159240,
//   4/59.94 s, on its first call) would be stepped in halves: at 40-60 fps,
//   two steps of 8.3-12.5 ms every frame, up to 120 steps a second. While
//   frames are fractional H is 0, so every step is T.
// Only sub_827A55C0 reads the three constants, so T is set in the image when
// the game is loaded.
//
// physics_log reports every 5 seconds the steps per frame and their length,
// the game clock's rate and the real time between frames, and for every
// second in which a hazard dealt damage, how many damage messages
// TouchDetectorHurt (sub_829ED8D8, a touch callback) and TriggerHurt
// (sub_8299EAB8, on iMsgTrigger) sent. Both send one only when what they touch
// can take damage. The same hazard sending twice as often at 60 fps as at 30
// would show a rate tied to the physics steps.

#include "image_patch.h"
#include "guest_memory.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/ppc.h>
#include <rex/system/xmemory.h>

REXCVAR_DEFINE_STRING(physics_step, "steady", "GPU",
                      "Havok physics step: steady (one 1/59.94 s step per vblank, as the console "
                      "stepped at 30 fps), legacy (two 8.3 ms steps per 60 Hz frame, the earlier "
                      "60 FPS fix) or original (the shipped code, 30 Hz steps at 60 fps)")
    .allowed({"steady", "legacy", "original"})
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(physics_log, false, "GPU",
                    "Log the physics steps per frame every 5 s and the hazard damage messages per "
                    "second");

// frame_pacing.cpp: whether frames are counted in fractional vblanks.
bool FractionalVblanks();

REX_EXTERN(__imp__sub_827A55C0);
// hkWorld::stepDeltaTime(world, f1 = step).
REX_EXTERN(sub_82AA2070);
REX_EXTERN(__imp__sub_82AA2070);
// TouchDetectorHurt's touch callback: r3 = the component, r4 = what touched it.
// Its frame is 208 bytes; the damage message sits at +80 in it.
REX_EXTERN(sub_829ED8D8);
REX_EXTERN(__imp__sub_829ED8D8);
// TriggerHurt's message handler: r3 = the component, r4 = the message. Its
// frame is 192 bytes; the damage message sits at +80 in it.
REX_EXTERN(sub_8299EAB8);
REX_EXTERN(__imp__sub_8299EAB8);
// Posts a message: r3 = the dispatcher, r4 = the receiver, r5 = the message.
// A damage message holds the damage at +44.
REX_EXTERN(sub_8268FAA8);
REX_EXTERN(__imp__sub_8268FAA8);

namespace {

// The step threshold and size T, 1/29.97 s as shipped.
constexpr uint32_t kStepThreshold = 0x82159238;
constexpr uint32_t kStepThresholdOriginal = 0x3D08AB86;
// 1/59.94 s, the game's own vblank period.
constexpr uint32_t kVblankStep = 0x3C88AB86;
// The step's half-step limit H, a function static: its guard word (bit 0 set
// once H has been initialized) and the constant it is initialized from.
constexpr uint32_t kHalfStepLimit = 0x82DFFA58;
constexpr uint32_t kHalfStepLimitGuard = 0x82DFFA5C;
constexpr uint32_t kHalfStepLimitInitial = 0x82159240;
constexpr uint32_t kHalfStepLimitOriginal = 0x3D88AB86;  // 4/59.94 s
// The game clock's frame time, whole vblanks times the time scale.
constexpr uint32_t kFrameDt = 0x82CED748;
// The same without the time scale.
constexpr uint32_t kRawFrameDt = 0x82CED744;
constexpr double kVblankSeconds = 1.0 / 59.94;
// A longer gap between two steps means no level ran (loading, menus).
constexpr int64_t kGapNs = 250'000'000;

enum class Mode { kSteady, kLegacy, kOriginal };
Mode g_mode = Mode::kOriginal;

const char* ModeName() {
  switch (g_mode) {
    case Mode::kSteady:
      return "steady";
    case Mode::kLegacy:
      return "legacy";
    default:
      return "original";
  }
}

using simpsons::LoadGuestFloat;
using simpsons::LoadGuestU32;
using simpsons::StoreGuestU32;

uint32_t Bits(double value) {
  return std::bit_cast<uint32_t>(float(value));
}

int64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// Step statistics, kept by the game's main thread, which runs the step.
thread_local bool t_in_step = false;
thread_local uint32_t t_steps = 0;
struct StepStats {
  int64_t since_ns = 0;
  uint32_t frames = 0;
  uint32_t clock_frames = 0;         // frames stepped by the clock's own frame time
  uint32_t steps_per_frame[4] = {};  // 0, 1, 2, 3 or more
  uint32_t steps = 0;
  uint32_t vblanks_per_frame[4] = {};  // the game clock's frame time: 0, 1, 2, 3 or more
  double game_seconds = 0.0;
  // Real time between two steps: <12, 12-15.5, 15.5-18, 18-21, 21-30, >=30 ms.
  uint32_t intervals[6] = {};
  float min_step = 0.0f;
  float max_step = 0.0f;
  uint32_t last_smoothed = 0;
  uint32_t last_used = 0;
};
StepStats g_steps;
int64_t g_last_step_ns = 0;

// Damage messages of the two hazards. While one of their handlers runs, its
// damage message is the one at this guest address.
enum class Hazard : uint8_t { kNone, kTouch, kTrigger };
thread_local Hazard t_hazard = Hazard::kNone;
thread_local uint32_t t_hazard_message = 0;
std::atomic<uint32_t> g_touch_hurts{0};
std::atomic<uint32_t> g_trigger_hurts{0};
std::atomic<uint32_t> g_touch_damage_bits{0};
std::atomic<uint32_t> g_trigger_damage_bits{0};
int64_t g_hazard_since_ns = 0;
uint32_t g_hazard_frames = 0;
uint32_t g_hazard_steps = 0;

void LogSteps(int64_t now) {
  StepStats& s = g_steps;
  const double seconds = double(now - s.since_ns) * 1e-9;
  REXLOG_INFO(
      "[physics] {}: {:.1f} s, {} frames ({:.1f}/s), game clock {:.3f}x real time, vblanks per "
      "frame 0:{} 1:{} 2:{} 3+:{}, steps per frame 0:{} 1:{} 2:{} 3+:{}, step {:.3f}-{:.3f} ms, "
      "frame time smoothed {:08X} -> used {:08X}, clock frame time used {}/{}, real frame "
      "interval <12:{} 12-15.5:{} 15.5-18:{} 18-21:{} 21-30:{} 30+:{} ms",
      ModeName(), seconds, s.frames, s.frames / seconds, s.game_seconds / seconds,
      s.vblanks_per_frame[0], s.vblanks_per_frame[1], s.vblanks_per_frame[2],
      s.vblanks_per_frame[3], s.steps_per_frame[0], s.steps_per_frame[1], s.steps_per_frame[2],
      s.steps_per_frame[3], s.min_step * 1000.0f, s.max_step * 1000.0f, s.last_smoothed,
      s.last_used, s.clock_frames, s.frames, s.intervals[0], s.intervals[1], s.intervals[2],
      s.intervals[3], s.intervals[4], s.intervals[5]);
  s = StepStats{};
  s.since_ns = now;
}

void ResetHazards(int64_t now) {
  g_touch_hurts.store(0, std::memory_order_relaxed);
  g_trigger_hurts.store(0, std::memory_order_relaxed);
  g_hazard_since_ns = now;
  g_hazard_frames = 0;
  g_hazard_steps = 0;
}

void LogHazards(int64_t now) {
  const uint32_t touches = g_touch_hurts.exchange(0, std::memory_order_relaxed);
  const uint32_t triggers = g_trigger_hurts.exchange(0, std::memory_order_relaxed);
  if (touches || triggers) {
    const double seconds = double(now - g_hazard_since_ns) * 1e-9;
    REXLOG_INFO(
        "[physics] hazards in {:.2f} s ({} frames, {} steps): TouchDetectorHurt {} damage "
        "messages (last {}), TriggerHurt {} (last {})",
        seconds, g_hazard_frames, g_hazard_steps, touches,
        std::bit_cast<float>(g_touch_damage_bits.load(std::memory_order_relaxed)), triggers,
        std::bit_cast<float>(g_trigger_damage_bits.load(std::memory_order_relaxed)));
  }
  ResetHazards(now);
}

void Record(const uint8_t* base, uint32_t smoothed, uint32_t used, bool clock_frame) {
  const int64_t now = NowNs();
  StepStats& s = g_steps;
  // After loading or a menu, start new windows instead of averaging the gap.
  if (!g_last_step_ns || now - g_last_step_ns > kGapNs) {
    if (g_last_step_ns) {
      REXLOG_INFO("[physics] stepping again after {:.1f} s", double(now - g_last_step_ns) * 1e-9);
    }
    s = StepStats{};
    s.since_ns = now;
    ResetHazards(now);
  } else {
    const double ms = double(now - g_last_step_ns) * 1e-6;
    ++s.intervals[ms < 12.0   ? 0
                  : ms < 15.5 ? 1
                  : ms < 18.0 ? 2
                  : ms < 21.0 ? 3
                  : ms < 30.0 ? 4
                              : 5];
  }
  g_last_step_ns = now;
  const double raw_dt = LoadGuestFloat(base, kRawFrameDt);
  s.game_seconds += raw_dt;
  ++s.vblanks_per_frame[std::clamp<long>(std::lround(raw_dt / kVblankSeconds), 0, 3)];
  ++s.frames;
  s.clock_frames += clock_frame ? 1 : 0;
  ++s.steps_per_frame[std::min<uint32_t>(t_steps, 3)];
  s.last_smoothed = smoothed;
  s.last_used = used;
  ++g_hazard_frames;
  g_hazard_steps += t_steps;
  if (now - g_hazard_since_ns >= 1'000'000'000) {
    LogHazards(now);
  }
  if (now - s.since_ns >= 5'000'000'000) {
    LogSteps(now);
  }
}

// Steps of T only while frames are fractional; the shipped H otherwise.
void SetHalfStepLimit(uint8_t* base, bool fractional) {
  const uint32_t guard = LoadGuestU32(base, kHalfStepLimitGuard);
  if (!fractional && !(guard & 1)) {
    return;  // not initialized yet: the step sets the shipped value itself
  }
  StoreGuestU32(base, kHalfStepLimitGuard, guard | 1);
  StoreGuestU32(base, kHalfStepLimit, fractional ? 0 : kHalfStepLimitOriginal);
}

// Runs a hazard handler, noting where its damage message will be: the
// handler's stack frame (frame_size below the caller's r1) plus 80.
void RunHazard(PPCContext& ctx, uint8_t* base, Hazard hazard, uint32_t frame_size,
               void (*handler)(PPCContext&, uint8_t*)) {
  const Hazard outer = t_hazard;
  const uint32_t outer_message = t_hazard_message;
  t_hazard = hazard;
  t_hazard_message = ctx.r1.u32 - frame_size + 80;
  handler(ctx, base);
  t_hazard = outer;
  t_hazard_message = outer_message;
}

}  // namespace

void ApplyPhysicsStepOptions(rex::memory::Memory* memory) {
  const std::string mode = REXCVAR_GET(physics_step);
  g_mode = Mode::kOriginal;
  if (mode == "original") {
    REXLOG_INFO("physics_step: original");
    return;
  }
  // Another release of the game has its data elsewhere: change nothing.
  const simpsons::ImageWordPatch patch[] = {
      {kStepThreshold, kStepThresholdOriginal, kVblankStep},
      {kHalfStepLimitInitial, kHalfStepLimitOriginal, kHalfStepLimitOriginal}};
  if (!simpsons::ApplyImagePatch(memory, patch)) {
    REXLOG_WARN(
        "physics_step: this game image is not the one the option was made for; "
        "physics left as shipped");
    return;
  }
  g_mode = mode == "legacy" ? Mode::kLegacy : Mode::kSteady;
  REXLOG_INFO("physics_step: {}", mode);
}

// Called by frame_pacing.cpp's override of sub_827A55C0.
void HavokStep(PPCContext& ctx, uint8_t* base) {
  const uint32_t smoothed = Bits(ctx.f1.f64);
  bool clock_frame = false;
  if (g_mode == Mode::kSteady) {
    const float frame_dt = LoadGuestFloat(base, kFrameDt);
    if (frame_dt > 0.0f && std::isfinite(frame_dt)) {
      ctx.f1.f64 = double(frame_dt);
      clock_frame = true;
    }
    SetHalfStepLimit(base, FractionalVblanks());
  }
  if (!REXCVAR_GET(physics_log)) {
    __imp__sub_827A55C0(ctx, base);
    return;
  }
  const uint32_t used = Bits(ctx.f1.f64);
  t_in_step = true;
  t_steps = 0;
  __imp__sub_827A55C0(ctx, base);
  t_in_step = false;
  Record(base, smoothed, used, clock_frame);
}

REX_FUNC(sub_82AA2070) {
  if (t_in_step) {
    const float step = float(ctx.f1.f64);
    StepStats& s = g_steps;
    s.min_step = s.steps ? std::min(s.min_step, step) : step;
    s.max_step = s.steps ? std::max(s.max_step, step) : step;
    ++s.steps;
    ++t_steps;
  }
  __imp__sub_82AA2070(ctx, base);
}

REX_FUNC(sub_829ED8D8) {
  if (!REXCVAR_GET(physics_log)) {
    __imp__sub_829ED8D8(ctx, base);
    return;
  }
  RunHazard(ctx, base, Hazard::kTouch, 208, __imp__sub_829ED8D8);
}

REX_FUNC(sub_8299EAB8) {
  if (!REXCVAR_GET(physics_log)) {
    __imp__sub_8299EAB8(ctx, base);
    return;
  }
  RunHazard(ctx, base, Hazard::kTrigger, 192, __imp__sub_8299EAB8);
}

REX_FUNC(sub_8268FAA8) {
  if (t_hazard != Hazard::kNone && ctx.r5.u32 == t_hazard_message) {
    const uint32_t damage = LoadGuestU32(base, ctx.r5.u32 + 44);
    if (t_hazard == Hazard::kTouch) {
      g_touch_hurts.fetch_add(1, std::memory_order_relaxed);
      g_touch_damage_bits.store(damage, std::memory_order_relaxed);
    } else {
      g_trigger_hurts.fetch_add(1, std::memory_order_relaxed);
      g_trigger_damage_bits.store(damage, std::memory_order_relaxed);
    }
  }
  __imp__sub_8268FAA8(ctx, base);
}
