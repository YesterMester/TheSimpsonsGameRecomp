#pragma once

#include <algorithm>
#include <cstdint>

namespace players {

// Editor character effects follow simulation time, not menu draws or the
// host's wall clock. Repeated draws, pauses and clock resets advance nothing.
class FeatureClock {
 public:
  float Sample(uint64_t microseconds, bool running) {
    const auto previous = previous_;
    const bool advance = initialized_ && running && was_running_ && microseconds > previous;
    previous_ = microseconds;
    initialized_ = true;
    was_running_ = running;
    return advance ? float(std::min<uint64_t>(microseconds - previous, 50000)) / 1000000.0f
                   : 0.0f;
  }
  void Reset() { *this = {}; }

 private:
  uint64_t previous_ = 0;
  bool initialized_ = false, was_running_ = false;
};

}  // namespace players
