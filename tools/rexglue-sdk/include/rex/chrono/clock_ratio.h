#pragma once

#include <cstdint>

#include <rex/platform.h>

namespace rex::chrono {

// Exact tick scaling, returning the low 64 bits like a native clock counter.
// The reduced ratios used by nanosecond clocks and Windows QPC usually need
// only a divide or a multiply. The general case must keep the full product.
inline uint64_t ScaleTickDelta(uint64_t ticks, uint64_t numerator, uint64_t denominator) {
  if (denominator == 1)
    return ticks * numerator;
  if (numerator == 1)
    return ticks / denominator;
#if REX_ARCH_AMD64
  uint64_t low, high;
#if REX_COMPILER_MSVC
  low = _umul128(ticks, numerator, &high);
#else
  unsigned __int128 product = static_cast<unsigned __int128>(ticks) * numerator;
  low = uint64_t(product);
  high = uint64_t(product >> 64);
#endif
  // DIV requires a quotient that fits in 64 bits. Reducing the high word
  // removes only whole multiples of 2^64 from the final quotient.
  if (high >= denominator)
    high %= denominator;
  uint64_t remainder;
#if REX_COMPILER_MSVC
  return _udiv128(high, low, denominator, &remainder);
#else
  // Clang's Windows MSVC ABI does not provide the __udivti3 runtime helper.
  // Use the native instruction, retaining the exact integer result.
  uint64_t quotient;
  __asm__("divq %2"
          : "=a"(quotient), "=d"(remainder)
          : "r"(denominator), "a"(low), "d"(high)
          : "cc");
  return quotient;
#endif
#else
  return uint64_t(static_cast<unsigned __int128>(ticks) * numerator / denominator);
#endif
}

}  // namespace rex::chrono
