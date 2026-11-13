/**
 ******************************************************************************
 * ReXGlue - Xbox 360 Static Recompilation Runtime                            *
 ******************************************************************************
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <rex/platform/performance_hint.h>

#include <dlfcn.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(android_performance_hints, true, "Android",
                    "Report each frame's work and deadline to Android's performance hint "
                    "session, which gives the game's threads the cores and clocks to keep up")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace rex::platform {

namespace {

struct APerformanceHintManager;
struct APerformanceHintSession;

// The NDK functions (Android 13, setThreads Android 14), looked up at run time
// as the app also supports older versions.
struct PerformanceHintApi {
  APerformanceHintManager* (*get_manager)();
  APerformanceHintSession* (*create_session)(APerformanceHintManager*, const int32_t*, size_t,
                                             int64_t);
  int (*update_target)(APerformanceHintSession*, int64_t);
  int (*report_actual)(APerformanceHintSession*, int64_t);
  int (*set_threads)(APerformanceHintSession*, const pid_t*, size_t);
  void (*close_session)(APerformanceHintSession*);
};

std::mutex threads_mutex;
std::vector<int32_t> threads;
bool threads_changed = false;
// Each transition increments the generation; its low bit says whether the
// activity is active. Read them together so a resume cannot report stale work.
std::atomic<uint64_t> activity_generation{1};

void RemoveThread(int32_t tid) {
  std::lock_guard<std::mutex> lock(threads_mutex);
  auto found = std::find(threads.begin(), threads.end(), tid);
  if (found != threads.end()) {
    threads.erase(found);
    threads_changed = true;
  }
}

struct ThreadRegistration {
  int32_t tid = int32_t(gettid());
  bool registered = false;
  ~ThreadRegistration() {
    if (registered) {
      RemoveThread(tid);
    }
  }
};

bool LoadApi(PerformanceHintApi& api) {
  void* library = dlopen("libandroid.so", RTLD_NOW);
  if (!library) {
    return false;
  }
  api.get_manager = reinterpret_cast<decltype(api.get_manager)>(
      dlsym(library, "APerformanceHint_getManager"));
  api.create_session = reinterpret_cast<decltype(api.create_session)>(
      dlsym(library, "APerformanceHint_createSession"));
  api.update_target = reinterpret_cast<decltype(api.update_target)>(
      dlsym(library, "APerformanceHint_updateTargetWorkDuration"));
  api.report_actual = reinterpret_cast<decltype(api.report_actual)>(
      dlsym(library, "APerformanceHint_reportActualWorkDuration"));
  api.set_threads =
      reinterpret_cast<decltype(api.set_threads)>(dlsym(library, "APerformanceHint_setThreads"));
  api.close_session = reinterpret_cast<decltype(api.close_session)>(
      dlsym(library, "APerformanceHint_closeSession"));
  return api.get_manager && api.create_session && api.update_target && api.report_actual &&
         api.close_session;
}

}  // namespace

void AddPerformanceHintThread() {
  thread_local ThreadRegistration registration;
  if (registration.registered) {
    return;
  }
  std::lock_guard<std::mutex> lock(threads_mutex);
  const int32_t tid = registration.tid;
  if (std::find(threads.begin(), threads.end(), tid) == threads.end()) {
    threads.push_back(tid);
    threads_changed = true;
  }
  registration.registered = true;
}

void SetPerformanceHintActive(bool active) {
  uint64_t generation = activity_generation.load(std::memory_order_relaxed);
  while (bool(generation & 1) != active) {
    if (activity_generation.compare_exchange_weak(generation, generation + 1,
                                                  std::memory_order_release,
                                                  std::memory_order_relaxed)) {
      return;
    }
  }
}

void ReportFrameWork(int64_t work_ns, int64_t target_ns) {
  static PerformanceHintApi api;
  static const bool unavailable = !REXCVAR_GET(android_performance_hints) || !LoadApi(api);
  static APerformanceHintManager* manager = nullptr;
  static APerformanceHintSession* session = nullptr;
  static int64_t session_target_ns = 0;
  static int64_t retry_after_ns = 0;
  static uint64_t last_generation = 1;
  static bool warned_no_session = false;
  if (unavailable || work_ns <= 0 || target_ns <= 0) {
    return;
  }
  auto reset_session = [&] {
    if (session) {
      api.close_session(session);
      session = nullptr;
    }
    session_target_ns = 0;
    manager = nullptr;
    std::lock_guard<std::mutex> lock(threads_mutex);
    threads_changed = true;
  };
  const uint64_t generation = activity_generation.load(std::memory_order_acquire);
  if (generation != last_generation) {
    last_generation = generation;
    reset_session();
    retry_after_ns = 0;
    // The first sample after a pause may include time spent in the background.
    return;
  }
  if (!(generation & 1)) {
    return;
  }
  const int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::steady_clock::now().time_since_epoch())
                             .count();
  if (now_ns < retry_after_ns) {
    return;
  }
  if (!manager) {
    manager = api.get_manager();
    if (!manager) {
      retry_after_ns = now_ns + 1'000'000'000;
      return;
    }
  }
  {
    std::lock_guard<std::mutex> lock(threads_mutex);
    if (threads_changed && !threads.empty()) {
      // Android 14 changes the threads of a session; 13 needs a new one.
      if (!session || !api.set_threads ||
          api.set_threads(session, threads.data(), threads.size()) != 0) {
        if (session) {
          api.close_session(session);
        }
        session = api.create_session(manager, threads.data(), threads.size(), target_ns);
        session_target_ns = target_ns;
        if (!session) {
          if (!warned_no_session) {
            REXLOG_WARN("Android performance hints unavailable: no session");
            warned_no_session = true;
          }
          manager = nullptr;
          retry_after_ns = now_ns + 1'000'000'000;
          return;
        }
        warned_no_session = false;
        REXLOG_INFO("Android performance hints: {} threads", threads.size());
      }
      threads_changed = false;
    }
  }
  if (!session) {
    return;
  }
  if (target_ns != session_target_ns) {
    if (api.update_target(session, target_ns) != 0) {
      reset_session();
      retry_after_ns = now_ns + 1'000'000'000;
      return;
    }
    session_target_ns = target_ns;
  }
  if (api.report_actual(session, work_ns) != 0) {
    reset_session();
    retry_after_ns = now_ns + 1'000'000'000;
  }
}

}  // namespace rex::platform
