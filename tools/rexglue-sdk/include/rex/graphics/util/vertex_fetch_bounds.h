#pragma once

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>

namespace rex::graphics::draw_util {

struct VertexFetchIndexBounds {
  int64_t first, last;
  const char* failure = nullptr;
  explicit operator bool() const { return failure == nullptr; }
};

struct VertexFetchByteRange {
  uint32_t address = 0, size = 0;
  explicit operator bool() const { return size != 0; }
};

// A negative word offset can still point to valid guest memory before the
// declared stream. Validate the actual physical range before doing arithmetic;
// fetch.size itself is not a bound imposed by the translated shader.
inline VertexFetchByteRange GetVertexFetchByteRange(uint32_t base_words, int64_t first_word,
                                                    int64_t end_word, uint32_t memory_size) {
  if (base_words > (UINT32_MAX >> 2) || first_word >= end_word ||
      first_word < -int64_t(base_words) ||
      end_word > int64_t(memory_size >> 2) - int64_t(base_words))
    return {};
  return {uint32_t((int64_t(base_words) + first_word) * 4), uint32_t((end_word - first_word) * 4)};
}

// Prove the float operations the translated fetch performs before converting
// its index to an integer. Every operation must preserve an exact binary grid;
// expressions requiring float rounding retain their compatibility path.
template <typename ReadConstant>
VertexFetchIndexBounds GetExactVertexFetchIndexBounds(uint32_t first, uint32_t last, bool rounded,
                                                      uint32_t offset_constant,
                                                      uint32_t scale_constant,
                                                      ReadConstant read_constant) {
  VertexFetchIndexBounds result{first, last};
  auto fail = [&](const char* reason) {
    result.failure = reason;
    return result;
  };
  if (first > last || last > (1u << 24))
    return fail("float vertex index");
  if (offset_constant == UINT32_MAX && scale_constant == UINT32_MAX &&
      (!rounded || last < (1u << 23)))
    return result;
  float first_index = float(first), last_index = float(last);
  int quantum_exponent = 0;
  if (offset_constant != UINT32_MAX) {
    if (offset_constant >= 256u * 4)
      return fail("vertex index offset register");
    float offset = read_constant(offset_constant);
    if (!std::isfinite(offset) || (offset != 0.0f && !std::isnormal(offset)))
      return fail("vertex index offset");
    if (offset != 0.0f) {
      uint32_t bits = std::bit_cast<uint32_t>(offset);
      uint32_t mantissa = (bits & 0x7FFFFFu) | 0x800000u;
      quantum_exponent =
          std::min(0, int((bits >> 23) & 255) - 127 - 23 + int(std::countr_zero(mantissa)));
    }
    double first_sum = double(first) + double(offset);
    double last_sum = double(last) + double(offset);
    if (quantum_exponent < -20 ||
        std::max(std::abs(first_sum), std::abs(last_sum)) >= std::ldexp(1.0, 23 + quantum_exponent))
      return fail("vertex index addition precision");
    first_index = float(first_sum);
    last_index = float(last_sum);
  }
  if (scale_constant != UINT32_MAX) {
    if (scale_constant >= 256u * 4)
      return fail("vertex index scale register");
    float scale = read_constant(scale_constant);
    int exponent;
    std::frexp(std::abs(scale), &exponent);
    if (!std::isfinite(scale) || (scale != 0.0f && !std::isnormal(scale)) || exponent < -20 ||
        exponent > 20)
      return fail("vertex index scale");
    if (scale == 0.0f) {
      first_index = last_index = 0.0f;
      quantum_exponent = 0;
    } else {
      const uint32_t bits = std::bit_cast<uint32_t>(scale);
      const uint32_t mantissa = (bits & 0x7FFFFFu) | 0x800000u;
      quantum_exponent += exponent - 24 + int(std::countr_zero(mantissa));
      // A binary32 product is exact in binary64. Bound all products on the
      // resulting grid before converting back to binary32. This also covers
      // non-power-of-two and negative scales without rounding assumptions.
      const double first_product = double(first_index) * double(scale);
      const double last_product = double(last_index) * double(scale);
      if (std::max(std::abs(first_product), std::abs(last_product)) >=
          std::ldexp(1.0, 23 + quantum_exponent))
        return fail("vertex index multiplication precision");
      first_index = float(std::min(first_product, last_product));
      last_index = float(std::max(first_product, last_product));
    }
  }
  if (double(first_index) < double(INT32_MIN) || double(last_index) >= 0x1p31)
    return fail("vertex index multiplication precision");
  float rounding = rounded ? 0.5f : 0.0f;
  if (rounded &&
      std::max(std::abs(double(first_index) + 0.5), std::abs(double(last_index) + 0.5)) >=
          std::ldexp(1.0, 23 + std::min(quantum_exponent, -1)))
    return fail("vertex fetch rounding precision");
  result.first = int64_t(std::floor(first_index + rounding));
  result.last = int64_t(std::floor(last_index + rounding));
  return result;
}

// Enclose both adjacent binary32 results permitted by the GPU rounding mode.
// Addition may also round in binary64 on the CPU, so widen that intermediate
// by one binary64 step before bracketing it with binary32 endpoints. A product
// of binary32 endpoints is exact in binary64. Subnormal results include flush
// to zero and use normal outer endpoints, independent of the CPU's FTZ mode.
inline double BoundVertexIndexOperation(double a, double b, bool multiply, bool lower) {
  if (multiply) {
    if (a == 0.0 || b == 0.0)
      return 0.0;
    if (b == 1.0)
      return a;
    if (b == -1.0)
      return -a;
  } else {
    if (a == 0.0)
      return b;
    if (b == 0.0)
      return a;
    if (a == -b)
      return 0.0;
  }
  double value = multiply ? a * b : a + b;
  if (!multiply)
    value = std::nextafter(value, lower ? -std::numeric_limits<double>::infinity()
                                        : std::numeric_limits<double>::infinity());
  if (value != 0.0 && std::abs(value) < double(std::numeric_limits<float>::min()))
    return lower ? (value < 0.0 ? -double(std::numeric_limits<float>::min()) : 0.0)
                 : (value > 0.0 ? double(std::numeric_limits<float>::min()) : 0.0);
  float endpoint = float(value);
  if (lower ? double(endpoint) > value : double(endpoint) < value)
    endpoint = std::nextafter(endpoint, lower ? -std::numeric_limits<float>::infinity()
                                              : std::numeric_limits<float>::infinity());
  return double(endpoint);
}

template <typename ReadConstant>
VertexFetchIndexBounds GetVertexFetchIndexBounds(uint32_t first, uint32_t last, bool rounded,
                                                 uint32_t offset_constant, uint32_t scale_constant,
                                                 ReadConstant read_constant) {
  auto exact = GetExactVertexFetchIndexBounds(first, last, rounded, offset_constant, scale_constant,
                                              read_constant);
  if (exact || first > last)
    return exact;
  auto constant = [&](uint32_t index, float fallback) {
    return index == UINT32_MAX ? fallback
           : index < 256u * 4  ? read_constant(index)
                               : std::numeric_limits<float>::quiet_NaN();
  };
  const float offset = constant(offset_constant, 0.0f);
  const float scale = constant(scale_constant, 1.0f);
  if (!std::isfinite(offset) || !std::isfinite(scale))
    return exact;
  // The guest mask normally produces 24-bit indices. An unsigned clamp may
  // raise those further; include both adjacent binary32 conversions rather
  // than requiring the integer itself to be exactly representable.
  double lower = float(first), upper = float(last);
  if (lower > double(first))
    lower = std::nextafter(float(lower), -std::numeric_limits<float>::infinity());
  if (upper < double(last))
    upper = std::nextafter(float(upper), std::numeric_limits<float>::infinity());
  if (offset_constant != UINT32_MAX) {
    const double original_lower = lower, original_upper = upper;
    lower = BoundVertexIndexOperation(lower, offset, false, true);
    upper = BoundVertexIndexOperation(upper, offset, false, false);
    if (offset != 0.0f && !std::isnormal(offset)) {
      lower = std::min(lower, original_lower);
      upper = std::max(upper, original_upper);
    }
    if (!std::isfinite(lower) || !std::isfinite(upper))
      return exact;
  }
  if (scale_constant != UINT32_MAX) {
    if (scale < 0.0f)
      std::swap(lower, upper);
    lower = BoundVertexIndexOperation(lower, scale, true, true);
    upper = BoundVertexIndexOperation(upper, scale, true, false);
    // A device may flush a subnormal constant operand before multiplication.
    if (scale != 0.0f && !std::isnormal(scale)) {
      lower = std::min(lower, 0.0);
      upper = std::max(upper, 0.0);
    }
  }
  if (rounded) {
    lower = BoundVertexIndexOperation(lower, 0.5, false, true);
    upper = BoundVertexIndexOperation(upper, 0.5, false, false);
  }
  if (!std::isfinite(lower) || !std::isfinite(upper) || lower > upper ||
      lower < double(INT32_MIN) || upper >= 0x1p31)
    return exact;
  return {int64_t(std::floor(lower)), int64_t(std::floor(upper))};
}

}  // namespace rex::graphics::draw_util
