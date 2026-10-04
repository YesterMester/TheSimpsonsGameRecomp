// The game's Direct3D code waits for the GPU to consume its command ring before
// reusing command memory (sub_824574B8, looping on the read pointer the GPU
// writes back). Each loop iteration calls sub_82452018, which on the Xbox 360
// paused the hardware thread with db16cyc hints before checking the GPU hang
// timeout. Recompiled, the hints do nothing and the loop spins a host core flat
// out, which on a power-limited APU (Steam Deck: 15 W for CPU and GPU together)
// takes power from the GPU. With ring_wait_sleep, the iteration instead blocks
// until the command processor next writes progress back to guest memory (the
// word waited on is a fence in the scratch register writeback area) or 1 ms
// passes.

#include <cstdint>
#include <cstring>

#include <rex/cvar.h>
#include <rex/graphics/ring_progress.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/ppc.h>

REXCVAR_DEFINE_BOOL(ring_wait_sleep, true, "GPU",
                    "Sleep until the GPU command processor moves on while the game waits for it "
                    "to consume its command ring, instead of spinning a CPU core (leaves power to "
                    "the GPU on power-limited devices)");

// One iteration of the ring wait: r3 = the wait state, whose first word is the
// Direct3D device. Returns r3 = 1 to keep waiting, 0 to stop (the device's
// abort flag, or the GPU hang timeout, after which it handles the hang).
REX_EXTERN(__imp__sub_82452018);
REX_EXTERN(sub_82452018);

namespace {

// Direct3D device field: pointer to the word the read pointer is written back to.
constexpr uint32_t kDeviceReadPointerAddress = 10896;
constexpr uint32_t kWaitTimeoutUs = 1000;

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

bool SleepEnabled() {
  static const bool enabled = REXCVAR_GET(ring_wait_sleep);
  return enabled;
}

}  // namespace

REX_FUNC(sub_82452018) {
  if (!SleepEnabled()) {
    __imp__sub_82452018(ctx, base);
    return;
  }
  static bool logged = false;
  if (!logged) {
    logged = true;
    uint32_t device = LoadBE32(base, ctx.r3.u32);
    REXLOG_INFO(
        "ring_wait_sleep: the game waits on the word at guest {:08X}; the command processor "
        "writes the read pointer back to physical {:08X}",
        LoadBE32(base, device + kDeviceReadPointerAddress),
        rex::graphics::GetReadPointerWritebackAddress());
  }
  // Read before the check, so progress between the check and the wait still
  // wakes it.
  uint32_t seen = rex::graphics::GetRingProgressCount();
  __imp__sub_82452018(ctx, base);
  if (ctx.r3.u32 == 1) {
    rex::graphics::WaitForRingProgress(seen, kWaitTimeoutUs);
  }
}
