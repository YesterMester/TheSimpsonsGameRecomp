// Menu frame pacing.
//
// The game shipped at 30 fps: its frame scheduler waits two vblanks per frame.
// The 60 FPS patch (the "li r4,1" hand patch in sub_82867A48) makes that one
// vblank everywhere. Gameplay copes, since its logic runs on real time and
// physics has its own fix, but the front end runs its logic once per frame:
// at 60 fps menus scroll and repeat inputs twice as fast as they were made
// for, and the title screen, which costs a little more than one 60 Hz frame,
// alternates 17 and 33 ms frames. While no level is simulating, this puts the
// frame scheduler back on the console's 30 fps cadence.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/ppc.h>

REXCVAR_DEFINE_INT32(menu_frame_rate, 30, "GPU",
                     "Frame rate of menus, the title screen and loading screens, where the game "
                     "runs its logic once per frame and was made for 30 fps (0 = same as gameplay)")
    .range(0, 240);

// Steps the Havok world by f1 seconds of frame time. Runs every frame while a
// level is live and never in the front end.
REX_EXTERN(__imp__sub_827A55C0);
REX_EXTERN(sub_827A55C0);

// Returns the time until the frame scheduler's next deadline. r3 = the
// scheduler, which holds the frame's start timebase at +136 and its periods
// per frame at +144, read on every call. The period is the game's own fixed
// refresh rate (59.94 Hz), whatever rate the guest vblank runs at.
REX_EXTERN(__imp__sub_82718710);
REX_EXTERN(sub_82718710);

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

// Host address of guest memory as the generated code computes it
// (REX_PHYS_HOST_OFFSET): on Windows the physical heaps from 0xE0000000,
// where the game's heap objects live, sit 0x1000 further on in host memory.
const uint8_t* GuestToHost(const uint8_t* base, uint32_t address) {
#if REX_PLATFORM_WIN32
  return base + address + (address >= 0xE0000000u ? 0x1000u : 0u);
#else
  return base + address;
#endif
}

uint32_t LoadBE32(const uint8_t* base, uint32_t address) {
  uint32_t value;
  std::memcpy(&value, GuestToHost(base, address), sizeof(value));
  return rex::byte_swap(value);
}

void StoreBE32(uint8_t* base, uint32_t address, uint32_t value) {
  value = rex::byte_swap(value);
  std::memcpy(const_cast<uint8_t*>(GuestToHost(base, address)), &value, sizeof(value));
}

}  // namespace

REX_FUNC(sub_827A55C0) {
  g_last_level_step_ns.store(NowNs(), std::memory_order_relaxed);
  __imp__sub_827A55C0(ctx, base);
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
    uint32_t current = LoadBE32(base, address);
    // Whatever the game itself set, unless it is still the menu value.
    if (!game_periods || current != applied_periods) {
      game_periods = std::max<uint32_t>(current, 1);
    }
    int64_t last_step = g_last_level_step_ns.load(std::memory_order_relaxed);
    bool menu = !last_step || NowNs() - last_step > kMenuAfterNs;
    uint32_t refresh_bits = LoadBE32(base, kSchedulerRefreshHz);
    float refresh_hz;
    std::memcpy(&refresh_hz, &refresh_bits, sizeof(refresh_hz));
    if (!(refresh_hz >= 20.0f && refresh_hz <= 240.0f)) {
      refresh_hz = 59.94f;
    }
    uint32_t menu_periods =
        std::max(game_periods, uint32_t(std::lround(double(refresh_hz) / double(menu_rate))));
    uint32_t wanted = menu ? menu_periods : game_periods;
    if (wanted != current) {
      StoreBE32(base, address, wanted);
    }
    applied_periods = wanted;
    if (menu != in_menu) {
      in_menu = menu;
      REXLOG_INFO("Frame pacing: {:.2f} fps, {}", double(refresh_hz) / double(wanted),
                  menu ? "no level running (menus)" : "level running");
    }
  }
  __imp__sub_82718710(ctx, base);
}
