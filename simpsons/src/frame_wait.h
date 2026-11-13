#pragma once

#include <algorithm>
#include <cstdint>

namespace simpsons {

// Keep a short margin for the scheduler, learning from its actual wake-ups.
// The caller still checks the original frame deadline after every wait.
class AdaptiveFrameWait {
 public:
  int64_t SleepDuration(int64_t remaining_ns) const {
    return remaining_ns > guard_ns_ ? remaining_ns - guard_ns_ : 0;
  }

  void ObserveSleep(int64_t requested_ns, int64_t elapsed_ns) {
    if (requested_ns <= 0 || elapsed_ns < 0) {
      return;
    }
    const int64_t oversleep = elapsed_ns > requested_ns ? elapsed_ns - requested_ns : 0;
    const int64_t wanted = std::min(oversleep, kMaximumGuard - kMinimumGuard) + kMinimumGuard;
    // React immediately to a delayed wake-up and give the margin back slowly.
    guard_ns_ = wanted >= guard_ns_ ? wanted : guard_ns_ - (guard_ns_ - wanted + 15) / 16;
  }

 private:
  static constexpr int64_t kMinimumGuard = 50'000;
  static constexpr int64_t kMaximumGuard = 2'000'000;
  int64_t guard_ns_ = 100'000;
};

}  // namespace simpsons
