#include "modkit_models.h"

#include <rex/ppc/context.h>

REX_EXTERN(__imp__sub_82719770);

extern "C" REX_FUNC(sub_82719770) {
  try {
    modkit_models::Apply(base, ctx.r3.u32, ctx.r4.u32);
  } catch (...) {
    // Creator diagnostics must never suppress or retry the original parser.
  }
  __imp__sub_82719770(ctx, base);
}
