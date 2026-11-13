// Keep the camera's input cutoff independent of frame rate.
//
// Both camera input handlers multiply the stick by their 60 Hz frame factor
// (+72) and turn speed before comparing it with a fixed 0.02 cutoff. The
// usual vertical speed is 0.03: at 90 FPS it lands on the cutoff, and above
// that even a fully held stick is discarded. Scale the cutoff on short
// frames, leaving the game's turn speeds, limits and 30/60 FPS input intact.

#include "guest_memory.h"

#include <cmath>
#include <cstdint>

#include <rex/ppc.h>

REX_EXTERN(__imp__sub_8295C9C0);
REX_EXTERN(__imp__sub_82960870);
REX_EXTERN(sub_8295C9C0);
REX_EXTERN(sub_82960870);

namespace {

class FrameInputCutoff {
 public:
  FrameInputCutoff(uint8_t* base, uint32_t camera, uint32_t address)
      : base_(base), address_(address), saved_(simpsons::LoadGuestU32(base, address)) {
    const float factor = simpsons::LoadGuestFloat(base, camera + 72);
    const float cutoff = simpsons::LoadGuestFloat(base, address);
    if (factor > 0.0f && factor < 1.0f && std::isfinite(cutoff) && cutoff > 0.0f) {
      simpsons::StoreGuestFloat(base, address, cutoff * factor);
    }
  }

  ~FrameInputCutoff() { simpsons::StoreGuestU32(base_, address_, saved_); }

 private:
  uint8_t* base_;
  uint32_t address_, saved_;
};

}  // namespace

// The input handlers run on the simulation thread. Restore the shared tuning
// word before returning so the next camera keeps its own frame factor.
REX_FUNC(sub_8295C9C0) {
  const FrameInputCutoff cutoff(base, ctx.r3.u32, 0x82CF8EDC);
  __imp__sub_8295C9C0(ctx, base);
}

REX_FUNC(sub_82960870) {
  const FrameInputCutoff cutoff(base, ctx.r3.u32, 0x82CF8F80);
  __imp__sub_82960870(ctx, base);
}
