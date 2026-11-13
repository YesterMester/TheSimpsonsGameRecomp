/**
 ******************************************************************************
 * ReXGlue - Xbox 360 Static Recompilation Runtime                            *
 ******************************************************************************
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <rex/graphics/util/buffer_hash.h>

#if defined(__x86_64__) || defined(_M_X64)
// Compile the upstream ISA-specific functions with their target attributes;
// the surrounding runtime still supports CPUs without AVX2. Hide the upstream
// C entry points on platforms supporting symbol visibility.
#if defined(__GNUC__)
#pragma GCC visibility push(hidden)
#endif
#define XXH_PRIVATE_API
#define XXH_DISPATCH_DISABLE_REPLACE
#include "../../../thirdparty/xxHash/xxh_x86dispatch.c"
#if defined(__GNUC__)
#pragma GCC visibility pop
#endif
#else
#include <rex/hash.h>
#endif

namespace rex::graphics::draw_util {

uint64_t HashNativeBuffer(const void* bytes, size_t size) {
#if defined(__x86_64__) || defined(_M_X64)
  if (size > 240) {
    // Do not use upstream's mutable, lazily initialized global dispatcher:
    // draws, uploads and shader workers may arrive together on the first call.
    // C++ initializes this private function pointer once, with synchronization.
    // Feature detection includes the OS's enabled vector-register state.
    static const auto hash_long = XXH_kDispatch[XXH_featureTest()].hashLong64_default;
    return hash_long(bytes, size);
  }
#endif
  return XXH3_64bits(bytes, size);
}

}  // namespace rex::graphics::draw_util
