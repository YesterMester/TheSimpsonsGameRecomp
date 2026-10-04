// Havok physics step at 60 fps.
//
// The game steps its Havok world once per frame in sub_827A55C0: it moves the
// world's frame time marker on by f1 seconds, then steps the simulation until
// it reaches the marker. f1 is the game clock's frame time (0x82CED748, whole
// 59.94 Hz vblanks) smoothed by sub_827A66B8 (s = 0.15 * dt + 0.85 * s). If f1
// is at most T (0x82159238, 1/29.97 s), it takes one step of T; otherwise two
// steps of f1 / 2 (f1 below 2/29.97 s) or steps of 1/59.94 s (0x8215923C).
//
// - original: the shipped code. At 30 fps one 33.4 ms step per frame (two of
//   16.7 ms after a dropped frame); at 60 fps one 33.4 ms step every other
//   frame, so physics objects move at 30 Hz and run up to a frame ahead.
// - legacy: T = 1/59.94 s, the 60 FPS fix this build used to carry as a hand
//   edit in the generated code. It was meant to give one 16.7 ms step per
//   60 Hz frame, but the smoothed frame time settles 3 float ulps above the
//   vblank (0x3C88AB89 against 0x3C88AB86), so every frame takes the other
//   branch: two steps of 8.3 ms, about 120 steps a second, a step size the
//   console never used.
// - steady (the default): T = 1/59.94 s, and once the smoothed frame time has
//   settled within a few ulps of the clock's frame time, the step gets that
//   exact value. At 60 fps that is one 16.7 ms step per frame, at 30 fps two:
//   60 steps of 16.7 ms a second either way, the step size and rate the
//   console used in its usual mode. Frame drops and changes of the time scale
//   take the game's own path until the frame time settles again.
// Only sub_827A55C0 reads the three constants, so T is set in the image when
// the game is loaded.
//
// physics_log reports every 5 seconds how many steps each frame took and how
// long they were, and, for every second with hazard damage, how often the two
// hazard components fired: TouchDetectorHurt (sub_829ED8D8, a touch callback
// that sends its damage on every call) and TriggerHurt (sub_8299EAB8, on
// iMsgTrigger). The same hazard firing twice as often at 60 fps as at 30 would
// show a rate tied to the physics steps.

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/ppc.h>
#include <rex/system/xmemory.h>

REXCVAR_DEFINE_STRING(physics_step, "steady", "GPU",
                      "Havok physics step: steady (one 1/59.94 s step per 60 Hz frame, as the "
                      "console stepped at 30 fps), legacy (two 8.3 ms steps per 60 Hz frame, the "
                      "earlier 60 FPS fix) or original (the shipped code, 30 Hz steps)")
    .allowed({"steady", "legacy", "original"})
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(physics_log, false, "GPU",
                    "Log the physics steps per frame every 5 s and the hazard damage events per "
                    "second");

REX_EXTERN(__imp__sub_827A55C0);
// hkWorld::stepDeltaTime(world, f1 = step).
REX_EXTERN(sub_82AA2070);
REX_EXTERN(__imp__sub_82AA2070);
// TouchDetectorHurt's touch callback: r3 = the component, r4 = what touched it.
REX_EXTERN(sub_829ED8D8);
REX_EXTERN(__imp__sub_829ED8D8);
// TriggerHurt's message handler: r3 = the component, r4 = the message.
REX_EXTERN(sub_8299EAB8);
REX_EXTERN(__imp__sub_8299EAB8);

namespace {

// The step threshold and size T, 1/29.97 s as shipped.
constexpr uint32_t kStepThreshold = 0x82159238;
constexpr uint32_t kStepThresholdOriginal = 0x3D08AB86;
// 1/59.94 s, the game's own vblank period.
constexpr uint32_t kVblankStep = 0x3C88AB86;
// The game clock's frame time, whole vblanks times the time scale.
constexpr uint32_t kFrameDt = 0x82CED748;
// The same without the time scale.
constexpr uint32_t kRawFrameDt = 0x82CED744;
constexpr double kVblankSeconds = 1.0 / 59.94;
// The smoothed frame time settles 2 to 3 ulps from the clock's value.
constexpr int32_t kSettledUlps = 8;
// iMsgTrigger's message id.
constexpr uint32_t kMsgTriggerId = 0x82E2DB64;

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

// Host address of guest memory as the generated code computes it
// (REX_PHYS_HOST_OFFSET): on Windows the physical heaps from 0xE0000000,
// where the game's heap objects live, sit 0x1000 further on in host memory.
const uint8_t* Host(const uint8_t* base, uint32_t address) {
#if REX_PLATFORM_WIN32
  return base + address + (address >= 0xE0000000u ? 0x1000u : 0u);
#else
  return base + address;
#endif
}

uint32_t LoadBE32(const uint8_t* base, uint32_t address) {
  uint32_t value;
  std::memcpy(&value, Host(base, address), sizeof(value));
  return rex::byte_swap(value);
}

float LoadBEFloat(const uint8_t* base, uint32_t address) {
  return std::bit_cast<float>(LoadBE32(base, address));
}

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
  uint32_t settled = 0;
  uint32_t steps_per_frame[4] = {};  // 0, 1, 2, 3 or more
  uint32_t steps = 0;
  uint32_t vblanks_per_frame[4] = {};  // the game clock's frame time: 0, 1, 2, 3 or more
  double game_seconds = 0.0;
  // Real time between two steps: <12, 12-15.5, 15.5-18, 18-21, 21-30, >=30 ms.
  uint32_t intervals[6] = {};
  float min_step = 0.0f;
  float max_step = 0.0f;
  uint32_t last_requested = 0;
  uint32_t last_used = 0;
};
StepStats g_steps;

// Hazard events, counted wherever the game raises them.
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
      "frame time {:08X} -> {:08X}, settled {}/{}, real frame interval <12:{} 12-15.5:{} "
      "15.5-18:{} 18-21:{} 21-30:{} 30+:{} ms",
      ModeName(), seconds, s.frames, s.frames / seconds, s.game_seconds / seconds,
      s.vblanks_per_frame[0], s.vblanks_per_frame[1], s.vblanks_per_frame[2],
      s.vblanks_per_frame[3], s.steps_per_frame[0], s.steps_per_frame[1], s.steps_per_frame[2],
      s.steps_per_frame[3], s.min_step * 1000.0f, s.max_step * 1000.0f, s.last_requested,
      s.last_used, s.settled, s.frames, s.intervals[0], s.intervals[1], s.intervals[2],
      s.intervals[3], s.intervals[4], s.intervals[5]);
  s = StepStats{};
  s.since_ns = now;
}

void LogHazards(int64_t now) {
  const uint32_t touches = g_touch_hurts.exchange(0, std::memory_order_relaxed);
  const uint32_t triggers = g_trigger_hurts.exchange(0, std::memory_order_relaxed);
  if (touches || triggers) {
    const double seconds = double(now - g_hazard_since_ns) * 1e-9;
    REXLOG_INFO(
        "[physics] hazards in {:.2f} s ({} frames, {} steps): TouchDetectorHurt {} (damage {}), "
        "TriggerHurt {} (damage {})",
        seconds, g_hazard_frames, g_hazard_steps, touches,
        std::bit_cast<float>(g_touch_damage_bits.load(std::memory_order_relaxed)), triggers,
        std::bit_cast<float>(g_trigger_damage_bits.load(std::memory_order_relaxed)));
  }
  g_hazard_since_ns = now;
  g_hazard_frames = 0;
  g_hazard_steps = 0;
}

void Record(const uint8_t* base, uint32_t requested, uint32_t used, bool settled) {
  const int64_t now = NowNs();
  StepStats& s = g_steps;
  if (!s.since_ns) {
    s.since_ns = now;
    g_hazard_since_ns = now;
  }
  static int64_t last_ns = 0;
  if (last_ns) {
    const double ms = double(now - last_ns) * 1e-6;
    ++s.intervals[ms < 12.0 ? 0 : ms < 15.5 ? 1 : ms < 18.0 ? 2 : ms < 21.0 ? 3 : ms < 30.0 ? 4 : 5];
  }
  last_ns = now;
  const double raw_dt = LoadBEFloat(base, kRawFrameDt);
  s.game_seconds += raw_dt;
  ++s.vblanks_per_frame[std::clamp<long>(std::lround(raw_dt / kVblankSeconds), 0, 3)];
  ++s.frames;
  s.settled += settled ? 1 : 0;
  ++s.steps_per_frame[std::min<uint32_t>(t_steps, 3)];
  s.last_requested = requested;
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

uint32_t LoadImageBE32(rex::memory::Memory* memory, uint32_t address) {
  uint32_t value;
  std::memcpy(&value, memory->TranslateVirtual(address), sizeof(value));
  return rex::byte_swap(value);
}

// Writes to the loaded image, lifting the read-only protection of .rdata
// pages for the write.
void StoreImageBE32(rex::memory::Memory* memory, uint32_t address, uint32_t value) {
  value = rex::byte_swap(value);
  rex::memory::BaseHeap* heap = memory->LookupHeap(address);
  const uint32_t page = address & ~(heap->page_size() - 1);
  uint32_t old_protect = 0;
  heap->Protect(page, heap->page_size(),
                rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite, &old_protect);
  std::memcpy(memory->TranslateVirtual(address), &value, sizeof(value));
  heap->Protect(page, heap->page_size(), old_protect);
}

}  // namespace

void ApplyPhysicsStepOptions(rex::memory::Memory* memory) {
  const std::string mode = REXCVAR_GET(physics_step);
  g_mode = Mode::kOriginal;
  if (mode == "original") {
    REXLOG_INFO("physics_step: original (30 Hz steps)");
    return;
  }
  // Another release of the game has its data elsewhere: change nothing.
  if (LoadImageBE32(memory, kStepThreshold) != kStepThresholdOriginal) {
    REXLOG_WARN("physics_step: this game image is not the one the option was made for; "
                "physics left as shipped");
    return;
  }
  StoreImageBE32(memory, kStepThreshold, kVblankStep);
  g_mode = mode == "legacy" ? Mode::kLegacy : Mode::kSteady;
  REXLOG_INFO("physics_step: {}", mode);
}

// Called by frame_pacing.cpp's override of sub_827A55C0.
void HavokStep(PPCContext& ctx, uint8_t* base) {
  const uint32_t requested = Bits(ctx.f1.f64);
  bool settled = false;
  if (g_mode == Mode::kSteady) {
    const float frame_dt = LoadBEFloat(base, kFrameDt);
    const uint32_t frame_dt_bits = std::bit_cast<uint32_t>(frame_dt);
    if (frame_dt > 0.0f && float(ctx.f1.f64) > 0.0f &&
        std::abs(int32_t(requested) - int32_t(frame_dt_bits)) <= kSettledUlps) {
      ctx.f1.f64 = double(frame_dt);
      settled = true;
    }
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
  Record(base, requested, used, settled);
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
  if (ctx.r4.u32 && REXCVAR_GET(physics_log)) {
    g_touch_hurts.fetch_add(1, std::memory_order_relaxed);
    g_touch_damage_bits.store(LoadBE32(base, ctx.r3.u32 + 76), std::memory_order_relaxed);
  }
  __imp__sub_829ED8D8(ctx, base);
}

REX_FUNC(sub_8299EAB8) {
  if (ctx.r4.u32 && REXCVAR_GET(physics_log) &&
      LoadBE32(base, ctx.r4.u32) == LoadBE32(base, kMsgTriggerId)) {
    g_trigger_hurts.fetch_add(1, std::memory_order_relaxed);
    g_trigger_damage_bits.store(LoadBE32(base, ctx.r3.u32 + 344), std::memory_order_relaxed);
  }
  __imp__sub_8299EAB8(ctx, base);
}
