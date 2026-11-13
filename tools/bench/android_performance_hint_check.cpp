// Run the production Android session code on Linux with a controlled service.
// The fake service checks recovery and lifetimes, not a phone's scheduling.
#include <rex/platform.h>
#undef REX_PLATFORM_ANDROID
#define REX_PLATFORM_ANDROID 1
#include "../rexglue-sdk/src/core/performance_hint_android.cpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <set>
#include <stdexcept>
#include <thread>

namespace {
using Manager = rex::platform::APerformanceHintManager;
using Session = rex::platform::APerformanceHintSession;
uint64_t checks = 0;
int creates = 0, closes = 0, updates = 0, reports = 0, thread_updates = 0, managers = 0;
int fail_create = 0, fail_update = 0, fail_report = 0, fail_threads = 0, fail_manager = 0;
bool old_api = false, missing_api = false;
size_t last_thread_count = 0;
std::set<Session*> live;

void Require(bool value, const char* message) {
  ++checks;
  if (!value) {
    throw std::runtime_error(message);
  }
}

void CheckThreads(const int32_t* ids, size_t size) {
  Require(size != 0, "Session has no threads");
  std::set<int32_t> unique;
  for (size_t i = 0; i < size; ++i) {
    Require(unique.insert(ids[i]).second, "Duplicate thread passed to service");
    Require(std::filesystem::exists("/proc/self/task/" + std::to_string(ids[i])),
            "Retired thread passed to service");
  }
  last_thread_count = size;
}

Manager* GetManager() {
  ++managers;
  if (fail_manager) {
    --fail_manager;
    return nullptr;
  }
  return reinterpret_cast<Manager*>(uintptr_t(1));
}

Session* Create(Manager* manager, const int32_t* ids, size_t size, int64_t target) {
  ++creates;
  Require(manager != nullptr && target > 0, "Invalid session request");
  CheckThreads(ids, size);
  if (fail_create) {
    --fail_create;
    return nullptr;
  }
  auto* session = reinterpret_cast<Session*>(uintptr_t(creates + 1) * 16);
  Require(live.insert(session).second, "Session handle reused while live");
  return session;
}

int Update(Session* session, int64_t target) {
  Require(live.count(session) && target > 0, "Target updated on a closed session");
  ++updates;
  if (fail_update) {
    --fail_update;
    return EPIPE;
  }
  return 0;
}

int Report(Session* session, int64_t actual) {
  Require(live.count(session) && actual > 0, "Work reported on a closed session");
  Require(actual < 1'000'000'000, "Background interval reported as frame work");
  ++reports;
  if (fail_report) {
    --fail_report;
    return EPIPE;
  }
  return 0;
}

int SetThreads(Session* session, const pid_t* ids, size_t size) {
  Require(live.count(session), "Threads updated on a closed session");
  ++thread_updates;
  CheckThreads(ids, size);
  if (fail_threads) {
    --fail_threads;
    return EPIPE;
  }
  return 0;
}

void Close(Session* session) {
  Require(live.erase(session) == 1, "Session closed twice");
  ++closes;
}

void Frame(int64_t target = 16'666'667) {
  rex::platform::ReportFrameWork(5'000'000, target);
}

void Resume() {
  rex::platform::SetPerformanceHintActive(false);
  rex::platform::SetPerformanceHintActive(true);
  const int previous = reports;
  rex::platform::ReportFrameWork(60'000'000'000, 16'666'667);
  Require(reports == previous && live.empty(), "Pause/resume interval was not discarded");
}

void Retry() {
  std::this_thread::sleep_for(std::chrono::milliseconds(1050));
  Frame();
  Require(live.size() == 1, "Session did not recover from temporary service failure");
}
}  // namespace

extern "C" void* __real_dlopen(const char*, int);
extern "C" void* __real_dlsym(void*, const char*);
extern "C" void* __wrap_dlopen(const char* name, int flags) {
  if (name && std::strcmp(name, "libandroid.so") == 0) {
    return missing_api ? nullptr : reinterpret_cast<void*>(uintptr_t(1));
  }
  return __real_dlopen(name, flags);
}
extern "C" void* __wrap_dlsym(void* handle, const char* name) {
  if (handle != reinterpret_cast<void*>(uintptr_t(1))) {
    return __real_dlsym(handle, name);
  }
  if (!std::strcmp(name, "APerformanceHint_getManager")) return reinterpret_cast<void*>(&GetManager);
  if (!std::strcmp(name, "APerformanceHint_createSession")) return reinterpret_cast<void*>(&Create);
  if (!std::strcmp(name, "APerformanceHint_updateTargetWorkDuration")) return reinterpret_cast<void*>(&Update);
  if (!std::strcmp(name, "APerformanceHint_reportActualWorkDuration")) return reinterpret_cast<void*>(&Report);
  if (!std::strcmp(name, "APerformanceHint_setThreads")) return old_api ? nullptr : reinterpret_cast<void*>(&SetThreads);
  if (!std::strcmp(name, "APerformanceHint_closeSession")) return reinterpret_cast<void*>(&Close);
  return nullptr;
}

int main(int argc, char** argv) {
  try {
    old_api = argc > 1 && !std::strcmp(argv[1], "--android13");
    missing_api = argc > 1 && !std::strcmp(argv[1], "--unavailable");
    const bool disabled = argc > 1 && !std::strcmp(argv[1], "--disabled");
    rex::InitLogging(nullptr, spdlog::level::off);
    if (disabled) {
      rex::cvar::SetFlagByName("android_performance_hints", "false");
    }
    rex::platform::AddPerformanceHintThread();
    rex::platform::AddPerformanceHintThread();
    if (missing_api || disabled) {
      Frame();
      Frame();
      Require(creates == 0 && reports == 0, "Unavailable hints were required");
    } else {
      fail_manager = 1;
      Frame();
      Frame();
      Require(managers == 1 && creates == 0, "Missing manager was retried every frame");
      Retry();
      Require(last_thread_count == 1, "Repeated registration duplicated the thread");
      const int initial_reports = reports;
      Frame(33'333'333);
      Require(updates == 1 && reports == initial_reports + 1, "Frame target change was lost");

      std::atomic<bool> ready{false}, release{false};
      std::thread worker([&] {
        rex::platform::AddPerformanceHintThread();
        ready.store(true, std::memory_order_release);
        ready.notify_one();
        release.wait(false, std::memory_order_acquire);
      });
      struct JoinWorker {
        std::atomic<bool>& release;
        std::thread& worker;
        ~JoinWorker() {
          release.store(true, std::memory_order_release);
          release.notify_one();
          if (worker.joinable()) worker.join();
        }
      } join_worker{release, worker};
      ready.wait(false, std::memory_order_acquire);
      fail_threads = old_api ? 0 : 1;
      Frame();
      const bool added = last_thread_count == 2 && live.size() == 1;
      release.store(true, std::memory_order_release);
      release.notify_one();
      worker.join();
      Require(added, "New worker was not added after a rejected thread update");
      Frame();
      Require(last_thread_count == 1, "Retired worker remained in the session");

      Resume();
      fail_create = 1;
      const int before_create = creates;
      Frame();
      Frame();
      Require(creates == before_create + 1 && live.empty(), "Session creation failure was not backed off");
      Retry();
      fail_update = 1;
      const int before_report = reports;
      Frame(33'333'333);
      Require(live.empty() && reports == before_report, "Work used a failed target update");
      Retry();
      fail_report = 1;
      Frame();
      Require(live.empty(), "Broken reporting session was kept");
      Retry();
      rex::platform::SetPerformanceHintActive(false);
      const int previous = reports;
      Frame();
      Frame();
      Require(reports == previous && live.empty(), "Background activity kept reporting work");
      rex::platform::SetPerformanceHintActive(true);
      Frame();
      Frame();
      Require(live.size() == 1, "Foreground activity did not recover");
      Resume();
      Require(creates == closes + 1, "Session lifecycle count disagrees");
      // One failed create returned no handle and needs no close.
    }
    std::printf("{\"passed\":true,\"checks\":%llu,\"android13_api\":%s,\"creates\":%d,\"closes\":%d}\n",
                static_cast<unsigned long long>(checks), old_api ? "true" : "false", creates, closes);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
