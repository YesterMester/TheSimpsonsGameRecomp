// The game clock in fractional vblanks, for frame rates above 60.
//
// The game clock (sub_82690D60, a method of the clock object in r3, whose
// +12 holds the previous tick count) measures each frame with the
// millisecond tick count and counts it in whole 59.94 Hz vblanks:
//   n = trunc(ms / ms_per_vblank - carry + 0.5), clamped to 1..max_vblanks,
//   carry = n - ms / ms_per_vblank, reset to 0 beyond +-0.2.
// Everything a frame's logic, animation and physics advance by comes from n:
// the frame time (raw and scaled by the time scale), the game time in
// microseconds, milliseconds and seconds, and the whole-vblank totals. A
// frame shorter than a vblank still counts as one, so above 60 fps the game
// ran faster than real time.
//
// While FractionalVblanks(), a frame counts the vblanks it actually took,
// measured on the precise guest clock instead of the millisecond tick count:
// x = seconds / vblank, clamped to 1/20 .. max_vblanks. Frame times follow
// real time, and the whole-vblank total (which the game turns into seconds and
// milliseconds) advances by the whole vblanks the fractions add up to. Every
// value is otherwise computed as the game computes it.
//
// physics_log also reports every 5 s how the frames the clock counted compare
// with real time, in either mode.

#include "guest_memory.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc.h>

REXCVAR_DECLARE(bool, physics_log);

// frame_pacing.cpp: whether frames are counted in fractional vblanks.
bool FractionalVblanks();

REX_EXTERN(__imp__sub_82690D60);
REX_EXTERN(sub_82690D60);
// The game's millisecond tick count (GetTickCount).
REX_EXTERN(sub_8270CE10);

namespace {

using simpsons::LoadGuestFloat;
using simpsons::LoadGuestU32;
using simpsons::StoreGuestFloat;
using simpsons::StoreGuestU32;

constexpr uint32_t kClockRunning = 0x82CED760;
constexpr uint32_t kClockPaused = 0x82D572DC;
constexpr uint32_t kCarry = 0x82D572AC;
constexpr uint32_t kMsPerVblank = 0x82D6CA9C;
constexpr uint32_t kMaxVblanks = 0x82CED740;
constexpr uint32_t kTotalVblanks = 0x82D572A8;
constexpr uint32_t kRawFrameDt = 0x82CED744;
constexpr uint32_t kTotalMs = 0x82D572B0;
constexpr uint32_t kTotalSeconds = 0x82D572B4;
constexpr uint32_t kRefreshHz = 0x82CF0394;
constexpr uint32_t kTimeScale = 0x82CED750;
constexpr uint32_t kGameTimeUs = 0x82D572C8;
constexpr uint32_t kGameTimeMs = 0x82D572BC;
constexpr uint32_t kGameTimeSeconds = 0x82D572C0;
constexpr uint32_t kFrameMs = 0x82CED74C;
constexpr uint32_t kFrameDt = 0x82CED748;
constexpr uint32_t kFrameCount = 0x82D572B8;
// A frame counts at least this many vblanks (0.83 ms), so its time is never 0.
constexpr float kMinVblanks = 0.05f;

uint64_t LoadGuestU64(const uint8_t* base, uint32_t address) {
  return (uint64_t(LoadGuestU32(base, address)) << 32) | LoadGuestU32(base, address + 4);
}

void StoreGuestU64(uint8_t* base, uint32_t address, uint64_t value) {
  StoreGuestU32(base, address, uint32_t(value >> 32));
  StoreGuestU32(base, address + 4, uint32_t(value));
}

// fctidz followed by stfiwx: truncate, keep the low 32 bits.
uint32_t Truncate32(double value) {
  if (std::isnan(value)) {
    return 0;
  }
  return uint32_t(int64_t(std::clamp(value, -9.2e18, 9.2e18)));
}

// Only the game's main thread runs the clock.
uint32_t g_clock = 0;
uint64_t g_last_guest_ticks = 0;
double g_vblank_fraction = 0.0;

// The clock's frames against real time, for physics_log.
struct ClockStats {
  uint64_t since_ticks = 0;
  uint64_t since_game_us = 0;
  bool fractional = false;
  uint32_t frames = 0;
  uint32_t clocks = 0;  // calls on another clock object than the last one
  double frame_seconds = 0.0;
};
ClockStats g_stats;

void RecordClock(const uint8_t* base, uint32_t clock, uint64_t now, bool fractional) {
  static uint32_t last_clock = 0;
  ClockStats& s = g_stats;
  const uint64_t game_us = LoadGuestU64(base, kGameTimeUs);
  const bool other_clock = last_clock && clock != last_clock;
  last_clock = clock;
  if (!LoadGuestU32(base, kClockRunning) || !s.since_ticks || s.fractional != fractional) {
    s = ClockStats{};
    s.since_ticks = now;
    s.since_game_us = game_us;
    s.fractional = fractional;
    return;
  }
  ++s.frames;
  s.clocks += other_clock ? 1 : 0;
  s.frame_seconds += LoadGuestFloat(base, kRawFrameDt);
  const double seconds =
      double(now - s.since_ticks) / double(rex::chrono::Clock::guest_tick_frequency());
  if (seconds < 5.0) {
    return;
  }
  REXLOG_INFO(
      "[clock] {} vblanks: {:.1f} s, {} frames ({:.1f}/s), frame time {:.3f}x real time, game "
      "time {:.3f}x, {} switches between clock objects",
      fractional ? "fractional" : "whole", seconds, s.frames, s.frames / seconds,
      s.frame_seconds / seconds, double(game_us - s.since_game_us) * 1e-6 / seconds, s.clocks);
  s = ClockStats{};
  s.since_ticks = now;
  s.since_game_us = game_us;
  s.fractional = fractional;
}

}  // namespace

REX_FUNC(sub_82690D60) {
  if (!FractionalVblanks()) {
    g_clock = 0;
    const uint32_t clock = ctx.r3.u32;
    __imp__sub_82690D60(ctx, base);
    if (REXCVAR_GET(physics_log)) {
      RecordClock(base, clock, rex::chrono::Clock::QueryGuestTickCount(), false);
    }
    return;
  }
  const uint32_t clock = ctx.r3.u32;
  // The tick count the game keeps in the clock, for the whole-vblank path.
  sub_8270CE10(ctx, base);
  const uint32_t ticks = ctx.r3.u32;
  const uint64_t now = rex::chrono::Clock::QueryGuestTickCount();
  if (LoadGuestU32(base, kClockRunning)) {
    const float ms_per_vblank = LoadGuestFloat(base, kMsPerVblank);
    double seconds;
    if (g_clock == clock && now > g_last_guest_ticks) {
      seconds =
          double(now - g_last_guest_ticks) / double(rex::chrono::Clock::guest_tick_frequency());
    } else {
      // The first frame, or after switching from whole vblanks.
      seconds = double(ticks - LoadGuestU32(base, clock + 12)) * 0.001;
    }
    const float max_vblanks = float(std::max<uint32_t>(LoadGuestU32(base, kMaxVblanks), 1));
    float vblanks = float(seconds * 1000.0 / double(ms_per_vblank));
    vblanks = std::clamp(vblanks, kMinVblanks, max_vblanks);
    StoreGuestFloat(base, kCarry, 0.0f);
    // The whole vblanks the fractions have added up to.
    g_vblank_fraction += double(vblanks);
    const uint32_t whole = uint32_t(g_vblank_fraction);
    g_vblank_fraction -= double(whole);
    const uint32_t total = LoadGuestU32(base, kTotalVblanks) + whole;
    StoreGuestU32(base, kTotalVblanks, total);
    const float raw_dt = float(vblanks * ms_per_vblank) * 0.001f;
    StoreGuestFloat(base, kRawFrameDt, raw_dt);
    StoreGuestU32(base, kTotalMs, Truncate32(std::fma(double(total), double(ms_per_vblank), 0.5)));
    StoreGuestFloat(base, kTotalSeconds, float(total) / LoadGuestFloat(base, kRefreshHz));
    if (LoadGuestU32(base, kClockPaused) != 1) {
      const float scaled_dt = LoadGuestFloat(base, kTimeScale) * raw_dt;
      const uint64_t game_us =
          LoadGuestU64(base, kGameTimeUs) +
          uint64_t(int64_t(std::trunc(std::fma(scaled_dt, 1000000.0f, 0.5f))));
      StoreGuestU64(base, kGameTimeUs, game_us);
      const double game_us_double = double(game_us);
      StoreGuestU32(base, kGameTimeMs,
                    Truncate32(float(std::fma(float(game_us_double), 0.001f, 0.5f))));
      StoreGuestU32(base, kFrameMs, Truncate32(float(std::fma(vblanks, ms_per_vblank, 0.5f))));
      StoreGuestFloat(base, kFrameDt, scaled_dt);
      StoreGuestU32(base, kFrameCount, LoadGuestU32(base, kFrameCount) + 1);
      StoreGuestFloat(base, kGameTimeSeconds, float(game_us_double * 1e-6));
    }
  }
  g_clock = clock;
  g_last_guest_ticks = now;
  StoreGuestU32(base, clock + 12, ticks);
  if (REXCVAR_GET(physics_log)) {
    RecordClock(base, clock, now, true);
  }
}
