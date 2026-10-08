#include <rex/chrono/clock_ratio.h>

#include <array>
#include <cstdio>
#include <fstream>
#include <random>
#include <stdexcept>
#include <string>

namespace {
// Independent bit-at-a-time product and long division. This needs no 128-bit
// compiler support and checks the native instruction path on Windows too.
uint64_t Reference(uint64_t ticks, uint64_t numerator, uint64_t denominator) {
  uint64_t low = 0, high = 0;
  for (uint32_t bit = 0; bit < 64; ++bit) {
    if (!((numerator >> bit) & 1))
      continue;
    uint64_t part = ticks << bit;
    uint64_t previous = low;
    low += part;
    high += (bit ? ticks >> (64 - bit) : 0) + uint64_t(low < previous);
  }
  uint64_t quotient = 0, remainder = 0;
  for (int bit = 127; bit >= 0; --bit) {
    bool carry = (remainder >> 63) != 0;
    remainder = (remainder << 1) | ((bit >= 64 ? high >> (bit - 64) : low >> bit) & 1);
    bool subtract = carry || remainder >= denominator;
    if (subtract)
      remainder -= denominator;
    quotient = (quotient << 1) | uint64_t(subtract);
  }
  return quotient;
}
}  // namespace

int main(int argc, char** argv) {
  try {
    std::string output;
    if (argc == 3 && std::string(argv[1]) == "--output")
      output = argv[2];
    else if (argc != 1)
      throw std::runtime_error("usage: clock_ratio_check [--output report.json]");
    uint64_t comparisons = 0;
    auto check = [&](uint64_t ticks, uint64_t numerator, uint64_t denominator) {
      uint64_t actual = rex::chrono::ScaleTickDelta(ticks, numerator, denominator);
      uint64_t expected = Reference(ticks, numerator, denominator);
      if (actual != expected)
        throw std::runtime_error("Native clock ratio differs from integer long division");
      ++comparisons;
    };
    constexpr std::array<uint64_t, 10> values = {
        0, 1, 2, 5, 20, 50'000'000, 1'000'000'000, UINT64_MAX / 2, UINT64_MAX - 1, UINT64_MAX};
    for (uint64_t ticks : values)
      for (uint64_t numerator : values)
        for (uint64_t denominator : values)
          if (denominator)
            check(ticks, numerator, denominator);
    std::mt19937_64 random(0x82718710);
    for (uint32_t i = 0; i < 50'000; ++i)
      check(random(), random(), random() | 1);
    std::string report =
        "{\"passed\":true,\"scope\":\"exact native clock ratios\",\"comparisons\":" +
        std::to_string(comparisons) + "}\n";
    std::fputs(report.c_str(), stdout);
    if (!output.empty()) {
      std::ofstream file(output);
      file << report;
      if (!file)
        throw std::runtime_error("Writing clock ratio report failed");
    }
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
  }
}
