/**
 ******************************************************************************
 * ReXGlue - Xbox 360 Static Recompilation Runtime                            *
 ******************************************************************************
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>

#include <simde/x86/sse4.1.h>
#include <rex/graphics/xenos.h>

namespace rex::graphics::draw_util {

struct VertexIndexBounds {
  uint32_t first = UINT32_MAX;
  uint32_t last = 0;
};

// Scan the CPU copy of the uploaded indices. Restart is tested on the raw
// index, before the DMA endian conversion. Match the vertex shader's unsigned
// add, 24-bit mask and unsigned clamp, including wrap at UINT32_MAX.
inline VertexIndexBounds GetVertexIndexBounds(const void* indices, uint32_t count, bool index_16,
                                              xenos::Endian endian, bool restart, uint32_t base,
                                              uint32_t clamp_min, uint32_t clamp_max) {
  simde__m128i shuffle;
  switch (endian) {
    case xenos::Endian::k8in16:
      shuffle = simde_mm_setr_epi8(1, 0, 3, 2, 5, 4, 7, 6, 9, 8, 11, 10, 13, 12, 15, 14);
      break;
    case xenos::Endian::k8in32:
      shuffle = simde_mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);
      break;
    case xenos::Endian::k16in32:
      shuffle = simde_mm_setr_epi8(2, 3, 0, 1, 6, 7, 4, 5, 10, 11, 8, 9, 14, 15, 12, 13);
      break;
    default:
      shuffle = simde_mm_setr_epi8(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
      break;
  }
  const simde__m128i base_vector = simde_mm_set1_epi32(int32_t(base));
  const simde__m128i mask_vector = simde_mm_set1_epi32(xenos::kVertexIndexMask);
  const simde__m128i clamp_min_vector = simde_mm_set1_epi32(int32_t(clamp_min));
  const simde__m128i clamp_max_vector = simde_mm_set1_epi32(int32_t(clamp_max));
  const simde__m128i restart_vector = simde_mm_set1_epi32(index_16 ? UINT16_MAX : -1);
  const simde__m128i all_ones = simde_mm_set1_epi32(-1);
  const simde__m128i zero = simde_mm_setzero_si128();
  simde__m128i minimum = all_ones;
  simde__m128i maximum = zero;
  auto include = [&](simde__m128i raw) {
    simde__m128i value = simde_mm_shuffle_epi8(raw, shuffle);
    value = simde_mm_and_si128(simde_mm_add_epi32(value, base_vector), mask_vector);
    value = simde_mm_min_epu32(simde_mm_max_epu32(value, clamp_min_vector), clamp_max_vector);
    if (restart) {
      simde__m128i invalid = simde_mm_cmpeq_epi32(raw, restart_vector);
      minimum = simde_mm_min_epu32(minimum, simde_mm_or_si128(value, invalid));
      maximum = simde_mm_max_epu32(maximum, simde_mm_andnot_si128(invalid, value));
    } else {
      minimum = simde_mm_min_epu32(minimum, value);
      maximum = simde_mm_max_epu32(maximum, value);
    }
  };
  const auto* bytes = static_cast<const uint8_t*>(indices);
  uint32_t i = 0;
  if (index_16) {
    for (; count - i >= 8; i += 8) {
      simde__m128i raw = simde_mm_loadu_si128(
          reinterpret_cast<const simde__m128i*>(bytes + size_t(i) * sizeof(uint16_t)));
      include(simde_mm_cvtepu16_epi32(raw));
      include(simde_mm_cvtepu16_epi32(simde_mm_srli_si128(raw, 8)));
    }
  } else {
    for (; count - i >= 4; i += 4) {
      include(simde_mm_loadu_si128(
          reinterpret_cast<const simde__m128i*>(bytes + size_t(i) * sizeof(uint32_t))));
    }
  }
  uint32_t minimum_lanes[4], maximum_lanes[4];
  simde_mm_storeu_si128(reinterpret_cast<simde__m128i*>(minimum_lanes), minimum);
  simde_mm_storeu_si128(reinterpret_cast<simde__m128i*>(maximum_lanes), maximum);
  VertexIndexBounds bounds;
  for (uint32_t lane = 0; lane < 4; ++lane) {
    bounds.first = std::min(bounds.first, minimum_lanes[lane]);
    bounds.last = std::max(bounds.last, maximum_lanes[lane]);
  }
  for (; i < count; ++i) {
    uint32_t raw;
    if (index_16) {
      uint16_t raw_16;
      std::memcpy(&raw_16, bytes + size_t(i) * sizeof(uint16_t), sizeof(raw_16));
      raw = raw_16;
    } else {
      std::memcpy(&raw, bytes + size_t(i) * sizeof(uint32_t), sizeof(raw));
    }
    if (restart && raw == (index_16 ? UINT16_MAX : UINT32_MAX)) {
      continue;
    }
    uint32_t value = std::clamp((xenos::GpuSwap(raw, endian) + base) & xenos::kVertexIndexMask,
                                clamp_min, clamp_max);
    bounds.first = std::min(bounds.first, value);
    bounds.last = std::max(bounds.last, value);
  }
  return bounds;
}

}  // namespace rex::graphics::draw_util
