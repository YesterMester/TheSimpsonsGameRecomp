#include "native_gpu_constants.h"

#include <rex/cvar.h>
#include <rex/graphics/native_gpu_calls.h>
#include <rex/ppc.h>

REXCVAR_DECLARE(bool, native_gpu_calls);
REX_EXTERN(sub_8245EC68);
REX_EXTERN(__imp__sub_8245EC68);

namespace simpsons::native_gpu_constants {

bool TryConstants(PPCContext& ctx, uint8_t* base) {
  static const bool enabled = REXCVAR_GET(native_gpu_calls);
  return enabled && rex::graphics::native_gpu_calls::Enabled() && WriteConstants(ctx, base);
}

}  // namespace simpsons::native_gpu_constants

REX_FUNC(sub_8245EC68) {
  if (!simpsons::native_gpu_constants::TryConstants(ctx, base))
    __imp__sub_8245EC68(ctx, base);
}
