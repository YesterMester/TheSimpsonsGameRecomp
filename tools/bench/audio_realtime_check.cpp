// Exercise Linux audio scheduling with real CPU work and the host's RT limit.
// --skip-yield is a negative control: it should terminate with SIGXCPU.
#include <rex/audio/thread_priority.h>
#include <SDL3/SDL.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#if defined(__linux__) && !defined(__ANDROID__)
#include <csignal>
#include <pthread.h>
#include <sys/resource.h>
#include <time.h>

uint64_t CpuNs() {
  timespec now{};
  if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now) != 0)
    return 0;
  return uint64_t(now.tv_sec) * 1000000000u + uint64_t(now.tv_nsec);
}

int main(int argc, char** argv) {
  if (argc > 2 || (argc == 2 && std::strcmp(argv[1], "--skip-yield")))
    return 2;
  rlimit limit{100000, 200000};
  if (setrlimit(RLIMIT_RTTIME, &limit) != 0 || !SDL_Init(0))
    return 2;
  std::signal(SIGXCPU, SIG_DFL);
  sigset_t signals;
  sigemptyset(&signals);
  sigaddset(&signals, SIGXCPU);
  pthread_sigmask(SIG_UNBLOCK, &signals, nullptr);
  if (!rex_audio_set_thread_priority()) {
    std::fprintf(stderr, "Real-time audio scheduling unavailable: %s\n", SDL_GetError());
    return 77;
  }
  int policy = 0;
  sched_param priority{};
  if (pthread_getschedparam(pthread_self(), &policy, &priority) != 0 ||
      ((policy & ~SCHED_RESET_ON_FORK) != SCHED_RR &&
       (policy & ~SCHED_RESET_ON_FORK) != SCHED_FIFO))
    return 77;
  rlimit granted{};
  if (getrlimit(RLIMIT_RTTIME, &granted) != 0 || granted.rlim_cur > 100000 ||
      granted.rlim_max > 200000)
    return 2;
  const uint64_t started = CpuNs();
  uint32_t yields = 0;
  while (CpuNs() - started < 300000000) {
    const uint64_t chunk = CpuNs();
    while (CpuNs() - chunk < 1000000)
      std::atomic_signal_fence(std::memory_order_seq_cst);
    if (argc == 1)
      yields += rex_audio_yield_realtime_budget();
  }
  int final_policy = 0;
  sched_param final_priority{};
  rlimit final_limit{};
  const bool passed =
      yields > 0 && pthread_getschedparam(pthread_self(), &final_policy, &final_priority) == 0 &&
      getrlimit(RLIMIT_RTTIME, &final_limit) == 0 && final_policy == policy &&
      final_priority.sched_priority == priority.sched_priority &&
      final_limit.rlim_cur == granted.rlim_cur && final_limit.rlim_max == granted.rlim_max;
  sched_param normal{};
  pthread_setschedparam(pthread_self(), SCHED_OTHER, &normal);
  std::printf(
      "{\"passed\":%s,\"cpu_ms\":300,\"yields\":%u,\"priority\":%d,"
      "\"rttime_soft_us\":%llu,\"rttime_hard_us\":%llu}\n",
      passed ? "true" : "false", yields, priority.sched_priority,
      (unsigned long long)granted.rlim_cur, (unsigned long long)granted.rlim_max);
  SDL_Quit();
  return passed ? 0 : 1;
}
#else
int main() {
  std::fprintf(stderr, "This check requires Linux real-time scheduling.\n");
  return 77;
}
#endif
