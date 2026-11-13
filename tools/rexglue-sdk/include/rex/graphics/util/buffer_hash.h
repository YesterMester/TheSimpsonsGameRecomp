/**
 ******************************************************************************
 * ReXGlue - Xbox 360 Static Recompilation Runtime                            *
 ******************************************************************************
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace rex::graphics::draw_util {

// The same XXH3 content hash on every host, with a CPU-selected implementation
// for retained buffers. The selection is immutable after first use.
uint64_t HashNativeBuffer(const void* bytes, size_t size);

}  // namespace rex::graphics::draw_util
