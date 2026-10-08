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
#include <cstring>

#include <simde/x86/sse4.1.h>

namespace rex::graphics::draw_util {

// Compares two byte ranges for equality 64 bytes at a time. Per-draw checks of
// unchanged data compare a lot of memory; some C runtimes, including Wine's,
// implement memcmp one byte at a time.
inline bool BytesEqual(const void* a, const void* b, size_t size) {
  const auto* x = static_cast<const uint8_t*>(a);
  const auto* y = static_cast<const uint8_t*>(b);
  auto difference = [&](size_t offset) {
    return simde_mm_xor_si128(
        simde_mm_loadu_si128(reinterpret_cast<const simde__m128i*>(x + offset)),
        simde_mm_loadu_si128(reinterpret_cast<const simde__m128i*>(y + offset)));
  };
  size_t i = 0;
  for (; size - i >= 64; i += 64) {
    simde__m128i any = simde_mm_or_si128(simde_mm_or_si128(difference(i), difference(i + 16)),
                                         simde_mm_or_si128(difference(i + 32), difference(i + 48)));
    if (!simde_mm_testz_si128(any, any)) {
      return false;
    }
  }
  for (; size - i >= 16; i += 16) {
    simde__m128i any = difference(i);
    if (!simde_mm_testz_si128(any, any)) {
      return false;
    }
  }
  return !std::memcmp(x + i, y + i, size - i);
}

}  // namespace rex::graphics::draw_util
