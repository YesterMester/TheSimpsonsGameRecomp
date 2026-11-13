// Springfield uses the same players, cameras and drop-in handling as the
// episodes. Its gameflow mode explicitly limits it to one player. Lift that
// limit when the mode is attached to Springfield, keeping the original join
// and leave paths and each episode's own player limit.

#include "guest_memory.h"

#include <cstring>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc.h>

REXCVAR_DEFINE_BOOL(freeroam_coop, true, "Game", "Allow two players to explore Springfield")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REX_EXTERN(__imp__sub_828A6900);
REX_EXTERN(sub_828A6900);

// EpisodePkg::AddEntry(episode handle, mode handle). Each handle's +4 is its
// reference-counted data. Episode data has kind 5 and an owned name at +12;
// mode data has kind 3 and its player limit at +56 (SetNumPlayersAllowed).
REX_FUNC(sub_828A6900) {
  using simpsons::GuestToHost;
  using simpsons::LoadGuestU32;
  using simpsons::StoreGuestU32;

  if (REXCVAR_GET(freeroam_coop) && ctx.r3.u32 && ctx.r4.u32) {
    const auto episode = LoadGuestU32(base, ctx.r3.u32 + 4);
    const auto mode = LoadGuestU32(base, ctx.r4.u32 + 4);
    if (episode && mode && LoadGuestU32(base, episode + 8) == 5 &&
        LoadGuestU32(base, mode + 8) == 3 && LoadGuestU32(base, mode + 56) == 1 &&
        (LoadGuestU32(base, episode + 16) >> 16) == 7) {
      const auto name = LoadGuestU32(base, episode + 12);
      if (name && !std::memcmp(GuestToHost(base, name), "SPR_HUB", 8)) {
        StoreGuestU32(base, mode + 56, 2);
        REXLOG_INFO("[freeroam] Springfield supports two players");
      }
    }
  }
  __imp__sub_828A6900(ctx, base);
}
