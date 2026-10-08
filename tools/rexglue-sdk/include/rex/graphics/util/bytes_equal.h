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
#if defined(__AVX2__)
#include <simde/x86/avx2.h>
#endif

namespace rex::graphics::draw_util {

// Compares two byte ranges for equality 64 bytes at a time. Per-draw checks of
// unchanged data compare a lot of memory; some C runtimes, including Wine's,
// implement memcmp one byte at a time.
inline bool BytesEqual(const void* a, const void* b, size_t size) {
  if (a == b || !size) {
    return true;
  }
  const auto* x = static_cast<const uint8_t*>(a);
  const auto* y = static_cast<const uint8_t*>(b);
  size_t i = 0;
#if defined(__AVX2__)
  if (size >= 512) {
    auto difference_wide = [&](size_t offset) {
      return simde_mm256_xor_si256(
          simde_mm256_loadu_si256(reinterpret_cast<const simde__m256i*>(x + offset)),
          simde_mm256_loadu_si256(reinterpret_cast<const simde__m256i*>(y + offset)));
    };
    for (; size - i >= 64; i += 64) {
      simde__m256i any = simde_mm256_or_si256(difference_wide(i), difference_wide(i + 32));
      if (!simde_mm256_testz_si256(any, any)) {
        return false;
      }
    }
    if (size - i >= 32) {
      simde__m256i any = difference_wide(i);
      if (!simde_mm256_testz_si256(any, any)) {
        return false;
      }
      i += 32;
    }
  }
#endif
  auto difference = [&](size_t offset) {
    return simde_mm_xor_si128(
        simde_mm_loadu_si128(reinterpret_cast<const simde__m128i*>(x + offset)),
        simde_mm_loadu_si128(reinterpret_cast<const simde__m128i*>(y + offset)));
  };
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
