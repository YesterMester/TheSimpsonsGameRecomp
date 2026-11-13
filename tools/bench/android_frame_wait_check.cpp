// Exercise the Android limiter's wake-up policy without changing game time.
#include "../../simpsons/src/frame_wait.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <limits>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
void Require(bool value, const char* message) {
  if (!value) {
    throw std::runtime_error(message);
  }
}

uint64_t CheckDeadlines() {
  uint64_t cases = 0;
  constexpr std::array<int64_t, 8> wake_errors{0, 10'000, 75'000, 200'000,
                                             1'500'000, 10'000'000, 0, 0};
  for (int rate : {24, 25, 30, 45, 60, 90, 120, 144, 240, 1000}) {
    simpsons::AdaptiveFrameWait wait;
    const int64_t period = 1'000'000'000 / rate;
    for (int frame = 0; frame < 2048; ++frame) {
      const int64_t start = int64_t(frame) * period;
      const int64_t deadline = start + period;
      int64_t now = start + (frame % 4) * period / 5;
      const int64_t wake_error = wake_errors[(frame / 16) % wake_errors.size()];
      while (now < deadline) {
        const int64_t remaining = deadline - now;
        const int64_t sleep = wait.SleepDuration(remaining);
        Require(sleep >= 0 && sleep < remaining, "Sleep crossed the frame deadline");
        if (sleep) {
          wait.ObserveSleep(sleep, sleep + wake_error);
          now += sleep + wake_error;
        } else {
          now += 1'000;  // A simulated yield, not an advance to the deadline.
        }
        ++cases;
      }
      Require(now >= deadline && now - deadline <= wake_error + 1'000,
              "Limiter returned early or added another frame");
      ++cases;
    }
  }
  simpsons::AdaptiveFrameWait wait;
  Require(wait.SleepDuration(0) == 0 && wait.SleepDuration(-1) == 0,
          "An expired deadline requested sleep");
  const int64_t initial = wait.SleepDuration(10'000'000);
  wait.ObserveSleep(0, 100);
  wait.ObserveSleep(100, -1);
  Require(wait.SleepDuration(10'000'000) == initial, "Invalid observation changed the guard");
  wait.ObserveSleep(1, std::numeric_limits<int64_t>::max());
  Require(wait.SleepDuration(10'000'000) == 8'000'000, "Delayed wake-up exceeded the guard cap");
  for (int i = 0; i < 256; ++i) {
    wait.ObserveSleep(1'000'000, 1'000'000);
  }
  Require(wait.SleepDuration(10'000'000) == 9'950'000,
          "Guard did not recover after a temporary delay");
  return cases + 6;
}

void MeasureWait(bool adaptive) {
  using Clock = std::chrono::steady_clock;
  simpsons::AdaptiveFrameWait wait;
  constexpr int kFrames = 120;
  constexpr int64_t kPeriod = 8'333'333;
  std::vector<int64_t> late;
  const auto started = Clock::now();
  const auto cpu_started = std::clock();
  for (int frame = 0; frame < kFrames; ++frame) {
    const auto deadline = started + std::chrono::nanoseconds((frame + 1) * kPeriod);
    auto now = Clock::now();
    while (now < deadline) {
      const auto remaining = std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - now).count();
      const int64_t sleep = adaptive ? wait.SleepDuration(remaining)
                                    : std::max<int64_t>(0, remaining - 2'000'000);
      if (sleep) {
        const auto before = Clock::now();
        std::this_thread::sleep_for(std::chrono::nanoseconds(sleep));
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - before).count();
        if (adaptive) {
          wait.ObserveSleep(sleep, elapsed);
        }
      } else {
        std::this_thread::yield();
      }
      now = Clock::now();
    }
    late.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(now - deadline).count());
  }
  const double cpu_ms = double(std::clock() - cpu_started) * 1000 / CLOCKS_PER_SEC;
  std::sort(late.begin(), late.end());
  std::printf("{\"policy\":\"%s\",\"frames\":%d,\"thread_cpu_ms\":%.3f,"
              "\"late_p99_us\":%.3f,\"late_max_us\":%.3f}\n",
              adaptive ? "adaptive" : "fixed-2ms", kFrames, cpu_ms,
              double(late[118]) / 1000, double(late.back()) / 1000);
}
}  // namespace

int main(int argc, char**) {
  try {
    std::printf("{\"passed\":true,\"checks\":%llu}\n",
                static_cast<unsigned long long>(CheckDeadlines()));
    if (argc > 1) {
      MeasureWait(false);
      MeasureWait(true);
    }
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
