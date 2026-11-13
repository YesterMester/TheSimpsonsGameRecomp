#include "entity_assets.h"
#include "modkit_meshes.h"

#include <rex/ppc/context.h>

REX_EXTERN(__imp__sub_82711B28);

extern "C" REX_FUNC(sub_82711B28) {
  try {
    modkit_meshes::Apply(base, ctx.r4.u32);
  } catch (...) {
    // Optional mesh edits cannot suppress the game's normal resource reader.
  }
  std::optional<entity_assets::Model> pending;
  try {
    pending = entity_assets::ReadStreamName(base, ctx.r4.u32);
  } catch (...) {
    // Optional creator metadata must not prevent the original asset load.
  }
  __imp__sub_82711B28(ctx, base);
  if (pending) {
    try {
      entity_assets::Loaded(base, *pending);
    } catch (...) {
      // Native loading and its register/exception behavior remain unchanged.
    }
  }
}
