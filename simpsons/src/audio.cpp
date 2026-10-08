// The RenderWare DAC thread mixes the game's audio and submits XMA work.
// Decoding runs synchronously on this thread, so raising only the runtime's
// audio worker and idle decoder thread leaves the actual producer behind
// under CPU load. Request native audio scheduling once at its entry point.

#include <rex/audio/thread_priority.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc.h>

REXCVAR_DEFINE_BOOL(native_audio_mixer_priority, true, "Audio",
                    "Give the game's native audio mixer thread audio scheduling priority");

// The DAC-specific callback at 0x823462F8 enters the mix/submit loop at
// 0x823460D0. The RenderWare wrappers at 0x82CAC720 and 0x8232A868 also
// launch non-audio workers. Hook only this callback and retain the original
// mixer, decode, timing and shutdown behavior.
REX_EXTERN(__imp__sub_823462F8);
REX_EXTERN(sub_823462F8);

REX_FUNC(sub_823462F8) {
  if (REXCVAR_GET(native_audio_mixer_priority)) {
    if (rex_audio_set_thread_priority()) {
      REXLOG_INFO("native_audio_mixer_priority: raised the native mixer thread priority");
    } else {
      REXLOG_WARN("native_audio_mixer_priority: native audio scheduling unavailable");
    }
  }
  __imp__sub_823462F8(ctx, base);
}
