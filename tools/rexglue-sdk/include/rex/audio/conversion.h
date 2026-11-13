/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <cstdint>

#include <rex/platform.h>
#include <rex/types.h>

namespace rex::audio::conversion {

// Stereo downmix of the game's 5.1 output (fl, fr, fc, lfe, bl, br) as the
// Xbox 360 makes it for a stereo TV: the centre and the surrounds at -3 dB on
// both sides, the LFE dropped, nothing attenuated overall. Dialogue is on the
// centre channel; the previous (fl + bl + fc / 2) / 2.5 left voices 14 dB down
// and everything else 8 dB down. Peaks above the knee bend smoothly toward
// full scale instead of clipping.
constexpr float kStereoCenterGain = 0.70710678f;
constexpr float kStereoSurroundGain = 0.70710678f;
constexpr float kStereoSoftKnee = 0.75f;

inline float StereoSoftLimit(float x) {
  float magnitude = x < 0.0f ? -x : x;
  if (magnitude <= kStereoSoftKnee) {
    return x;
  }
  // Slope 1 at the knee, approaching 1.0 for any louder input.
  float over = (magnitude - kStereoSoftKnee) / (1.0f - kStereoSoftKnee);
  float limited = kStereoSoftKnee + (1.0f - kStereoSoftKnee) * over / (1.0f + over);
  return x < 0.0f ? -limited : limited;
}

#if REX_ARCH_AMD64
inline void sequential_6_BE_to_interleaved_6_LE(float* output, const float* input,
                                                size_t ch_sample_count) {
  const uint32_t* in = reinterpret_cast<const uint32_t*>(input);
  uint32_t* out = reinterpret_cast<uint32_t*>(output);
  const __m128i byte_swap_shuffle =
      _mm_set_epi8(12, 13, 14, 15, 8, 9, 10, 11, 4, 5, 6, 7, 0, 1, 2, 3);
  for (size_t sample = 0; sample < ch_sample_count; sample++) {
    __m128i sample0 =
        _mm_set_epi32(in[3 * ch_sample_count + sample], in[2 * ch_sample_count + sample],
                      in[1 * ch_sample_count + sample], in[0 * ch_sample_count + sample]);
    uint32_t sample1 = in[4 * ch_sample_count + sample];
    uint32_t sample2 = in[5 * ch_sample_count + sample];
    sample0 = _mm_shuffle_epi8(sample0, byte_swap_shuffle);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(&out[sample * 6]), sample0);
    sample1 = rex::byte_swap(sample1);
    out[sample * 6 + 4] = sample1;
    sample2 = rex::byte_swap(sample2);
    out[sample * 6 + 5] = sample2;
  }
}

inline __m128 StereoSoftLimit(__m128 x) {
  const __m128 sign_mask = _mm_set1_ps(-0.0f);
  const __m128 knee = _mm_set1_ps(kStereoSoftKnee);
  const __m128 range = _mm_set1_ps(1.0f - kStereoSoftKnee);
  const __m128 one = _mm_set1_ps(1.0f);
  __m128 magnitude = _mm_andnot_ps(sign_mask, x);
  __m128 over = _mm_div_ps(_mm_max_ps(_mm_sub_ps(magnitude, knee), _mm_setzero_ps()), range);
  __m128 limited = _mm_add_ps(knee, _mm_mul_ps(range, _mm_div_ps(over, _mm_add_ps(one, over))));
  limited = _mm_blendv_ps(magnitude, limited, _mm_cmpgt_ps(magnitude, knee));
  return _mm_or_ps(limited, _mm_and_ps(x, sign_mask));
}

inline void sequential_6_BE_to_interleaved_2_LE(float* output, const float* input,
                                                size_t ch_sample_count,
                                                float center_gain = kStereoCenterGain) {
  assert_true(ch_sample_count % 4 == 0);

  const __m128i byte_swap_shuffle =
      _mm_set_epi8(12, 13, 14, 15, 8, 9, 10, 11, 4, 5, 6, 7, 0, 1, 2, 3);
  const __m128 center_scale = _mm_set1_ps(center_gain);
  const __m128 surround_scale = _mm_set1_ps(kStereoSurroundGain);

  for (size_t sample = 0; sample < ch_sample_count; sample += 4) {
    // load 4 samples from 6 channels each
    __m128 fl = _mm_loadu_ps(&input[0 * ch_sample_count + sample]);
    __m128 fr = _mm_loadu_ps(&input[1 * ch_sample_count + sample]);
    __m128 fc = _mm_loadu_ps(&input[2 * ch_sample_count + sample]);
    __m128 bl = _mm_loadu_ps(&input[4 * ch_sample_count + sample]);
    __m128 br = _mm_loadu_ps(&input[5 * ch_sample_count + sample]);
    // byte swap
    fl = _mm_castsi128_ps(_mm_shuffle_epi8(_mm_castps_si128(fl), byte_swap_shuffle));
    fr = _mm_castsi128_ps(_mm_shuffle_epi8(_mm_castps_si128(fr), byte_swap_shuffle));
    fc = _mm_castsi128_ps(_mm_shuffle_epi8(_mm_castps_si128(fc), byte_swap_shuffle));
    bl = _mm_castsi128_ps(_mm_shuffle_epi8(_mm_castps_si128(bl), byte_swap_shuffle));
    br = _mm_castsi128_ps(_mm_shuffle_epi8(_mm_castps_si128(br), byte_swap_shuffle));

    __m128 center = _mm_mul_ps(fc, center_scale);
    __m128 left = _mm_add_ps(_mm_add_ps(fl, center), _mm_mul_ps(bl, surround_scale));
    __m128 right = _mm_add_ps(_mm_add_ps(fr, center), _mm_mul_ps(br, surround_scale));
    left = StereoSoftLimit(left);
    right = StereoSoftLimit(right);
    _mm_storeu_ps(&output[sample * 2], _mm_unpacklo_ps(left, right));
    _mm_storeu_ps(&output[(sample + 2) * 2], _mm_unpackhi_ps(left, right));
  }
}
#else
inline void sequential_6_BE_to_interleaved_6_LE(float* output, const float* input,
                                                size_t ch_sample_count) {
  for (size_t sample = 0; sample < ch_sample_count; sample++) {
    for (size_t channel = 0; channel < 6; channel++) {
      output[sample * 6 + channel] = rex::byte_swap(input[channel * ch_sample_count + sample]);
    }
  }
}
inline void sequential_6_BE_to_interleaved_2_LE(float* output, const float* input,
                                                size_t ch_sample_count,
                                                float center_gain = kStereoCenterGain) {
  // Default 5.1 channel mapping is fl, fr, fc, lf, bl, br
  // https://docs.microsoft.com/en-us/windows/win32/xaudio2/xaudio2-default-channel-mapping
  for (size_t sample = 0; sample < ch_sample_count; sample++) {
    float fl = rex::byte_swap(input[0 * ch_sample_count + sample]);
    float fr = rex::byte_swap(input[1 * ch_sample_count + sample]);
    float fc = rex::byte_swap(input[2 * ch_sample_count + sample]);
    float bl = rex::byte_swap(input[4 * ch_sample_count + sample]);
    float br = rex::byte_swap(input[5 * ch_sample_count + sample]);
    float center = fc * center_gain;
    output[sample * 2] = StereoSoftLimit(fl + center + bl * kStereoSurroundGain);
    output[sample * 2 + 1] = StereoSoftLimit(fr + center + br * kStereoSurroundGain);
  }
}
#endif

}  // namespace rex::audio::conversion
