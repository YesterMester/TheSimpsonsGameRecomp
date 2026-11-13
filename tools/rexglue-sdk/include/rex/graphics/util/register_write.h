/**
 ******************************************************************************
 * ReXGlue - Xbox 360 Static Recompilation Runtime                            *
 ******************************************************************************
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>

#include <simde/x86/sse4.1.h>

namespace rex::graphics::draw_util {

// Constant registers have no write side effects. Convert their guest byte
// order while detecting changes, so identical packets keep their current
// GPU constants and texture bindings. Other register banks still use their
// normal write handlers.
inline bool CopyAndSwapRegisters(uint32_t* destination, const uint32_t* source, size_t count) {
  const auto shuffle = simde_mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);
  auto difference = simde_mm_setzero_si128();
  size_t i = 0;
  for (; count - i >= 4; i += 4) {
    const auto value = simde_mm_shuffle_epi8(
        simde_mm_loadu_si128(reinterpret_cast<const simde__m128i*>(source + i)), shuffle);
    const auto previous =
        simde_mm_loadu_si128(reinterpret_cast<const simde__m128i*>(destination + i));
    difference = simde_mm_or_si128(difference, simde_mm_xor_si128(previous, value));
    simde_mm_storeu_si128(reinterpret_cast<simde__m128i*>(destination + i), value);
  }
  uint32_t tail_difference = 0;
  for (; i < count; ++i) {
    const uint32_t value = std::byteswap(source[i]);
    tail_difference |= destination[i] ^ value;
    destination[i] = value;
  }
  return tail_difference || !simde_mm_testz_si128(difference, difference);
}

}  // namespace rex::graphics::draw_util
