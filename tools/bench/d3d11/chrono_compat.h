#pragma once
#include <chrono>
#if defined(_LIBCPP_VERSION) && __cpp_lib_chrono < 201907L
namespace std::chrono {
template <class DestinationClock, class SourceClock>
struct clock_time_conversion;
// The runtime only casts between clocks with explicit SDK conversions. This
// lets its Windows code be exercised by LLVM-MinGW without replacing clocks.
template <class DestinationClock, class SourceClock, class Duration>
auto clock_cast(const time_point<SourceClock, Duration>& point) {
  if constexpr (is_same_v<DestinationClock, SourceClock>)
    return point;
  else
    return clock_time_conversion<DestinationClock, SourceClock>{}(point);
}
}  // namespace std::chrono
#endif
