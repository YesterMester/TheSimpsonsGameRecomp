/**
 * @file        perf/event_trace.cpp
 * @brief       Debugging timeline of frame events
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/perf/event_trace.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <vector>

#include <rex/platform.h>

#if REX_PLATFORM_LINUX
#include <unistd.h>
#endif

namespace rex::perf {

namespace {

struct Event {
  int64_t time_us;
  int32_t tid;
  const char* name;
  uint64_t arg;
};

const char* TracePath() {
  static const char* path = std::getenv("REX_EVENT_TRACE");
  return path;
}

std::mutex g_event_mutex;
std::vector<Event> g_events;
thread_local bool t_swap_thread = false;

int64_t NowUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

int32_t ThreadId() {
#if REX_PLATFORM_LINUX
  thread_local int32_t tid = int32_t(gettid());
  return tid;
#else
  return 0;
#endif
}

}  // namespace

bool EventTraceEnabled() {
  return TracePath() != nullptr;
}

void TraceEvent(const char* name, uint64_t arg) {
  if (!TracePath()) {
    return;
  }
  Event event{NowUs(), ThreadId(), name, arg};
  std::lock_guard<std::mutex> lock(g_event_mutex);
  g_events.push_back(event);
}

void MarkSwapThread() {
  t_swap_thread = true;
}

void TraceSwapThreadEvent(const char* name, uint64_t arg) {
  if (t_swap_thread) {
    TraceEvent(name, arg);
  }
}

void FlushEventTrace() {
  const char* path = TracePath();
  if (!path) {
    return;
  }
  static std::vector<Event> writing;
  {
    std::lock_guard<std::mutex> lock(g_event_mutex);
    writing.swap(g_events);
  }
  if (writing.empty()) {
    return;
  }
  static FILE* file = std::fopen(path, "w");
  if (file) {
    for (const Event& event : writing) {
      std::fprintf(file, "%lld %d %s %llu\n", (long long)event.time_us, event.tid, event.name,
                   (unsigned long long)event.arg);
    }
    std::fflush(file);
  }
  writing.clear();
}

}  // namespace rex::perf
