// Check real audio workers and native callbacks during shutdown, without game data.
#include <rex/audio/audio_system.h>
#include <rex/audio/audio_driver.h>
#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>
#include <rex/logging.h>
#include <rex/cvar.h>
#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <thread>
#include <string>

using namespace std::chrono_literals;
std::atomic<bool> callback_started{false}, callback_finished{false};
std::atomic<uint32_t> destroyed{0};
std::atomic<uint32_t> callback_count{0};
std::atomic<uint32_t> finished_count{0};
std::atomic<bool> recycle_slots{false};
std::atomic<rex::thread::Semaphore*> output_slots{nullptr};
void callback(PPCContext&, uint8_t*) noexcept {
  callback_started.store(true, std::memory_order_release);
  ++callback_count;
  std::this_thread::sleep_for(80ms);
  if (recycle_slots) {
    // Model a device that keeps consuming frames while Pause waits. A ready
    // lower-index client must not starve the worker's pause request.
    auto* slots = output_slots.load(std::memory_order_acquire);
    if (slots)
      slots->Release(1, nullptr);
  }
  callback_finished.store(true, std::memory_order_release);
  ++finished_count;
}
struct Driver : rex::audio::AudioDriver {
  explicit Driver(rex::memory::Memory* m) : AudioDriver(m) {}
  void SubmitFrame(uint32_t) override {}
};
struct Audio : rex::audio::AudioSystem {
  explicit Audio(rex::runtime::FunctionDispatcher* d) : AudioSystem(d) {}
  rex::X_STATUS CreateDriver(size_t, rex::thread::Semaphore* slots,
                             rex::audio::AudioDriver** out) override {
    *out = new Driver(memory());
    output_slots.store(slots, std::memory_order_release);
    return 0;
  }
  void DestroyDriver(rex::audio::AudioDriver* d) override {
    delete d;
    output_slots.store(nullptr, std::memory_order_release);
    ++destroyed;
  }
};
void require(bool b, const char* message) {
  if (!b)
    throw std::runtime_error(message);
}
int main(int argc, char** argv) {
  try {
    require(argc == 2 || argc == 3,
            "usage: audio_shutdown_check idle|active|paused|busy [report.json]");
    std::string mode = argv[1];
    require(mode == "idle" || mode == "active" || mode == "paused" || mode == "busy",
            "Unknown shutdown scenario");
    recycle_slots = mode == "busy";
    rex::InitLogging(nullptr, spdlog::level::info);
    require(rex::cvar::SetFlagByName("audio_callback_pacing", "true"), "pacing flag");
    require(rex::cvar::SetFlagByName("audio_maxqframes", "8"), "queue flag");
    rex::Runtime runtime("", "");
    rex::RuntimeConfig config;
    config.audio_factory = [](rex::runtime::FunctionDispatcher* d) {
      return std::make_unique<Audio>(d);
    };
    require(runtime.Setup(std::move(config)) == 0, "runtime setup failed");
    auto* audio = dynamic_cast<Audio*>(runtime.audio_system());
    require(audio, "audio factory failed");
    uint32_t pause_cycles = 0;
    int64_t max_pause_ms = 0;
    if (mode != "idle") {
      require(runtime.function_dispatcher()->InitializeFunctionTable(0x82000000, 0x1000, 0x82000000,
                                                                     0x10000, true),
              "callback module table failed");
      require(runtime.function_dispatcher()->SetFunction(0x82000000, callback),
              "callback mapping failed");
      size_t client;
      require(audio->RegisterClient(0x82000000, 0, &client) == 0, "register client failed");
      const auto limit = std::chrono::steady_clock::now() + 3s;
      while (!callback_started.load(std::memory_order_acquire) &&
             std::chrono::steady_clock::now() < limit)
        std::this_thread::sleep_for(1ms);
      require(callback_started, "callback did not start");
      if (mode == "paused")
        audio->Pause();
      if (mode == "busy") {
        std::puts("busy pause: callback started, device slots remain ready");
        std::fflush(stdout);
        // More cycles than the initial queue capacity also catch a pause path
        // that consumes a semaphore permit without returning or using it.
        for (uint32_t i = 0; i < 20; ++i) {
          const auto pause_start = std::chrono::steady_clock::now();
          audio->Pause();
          const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - pause_start)
                                .count();
          max_pause_ms = std::max<int64_t>(max_pause_ms, took);
          require(took < 500, "ready audio buffers starved a pause request");
          ++pause_cycles;
          const auto count = callback_count.load();
          audio->Resume();
          const auto deadline = std::chrono::steady_clock::now() + 500ms;
          while (callback_count.load() == count && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(1ms);
          require(callback_count.load() > count, "pause/resume lost audio queue capacity");
        }
      }
    }
    auto start = std::chrono::steady_clock::now();
    audio->Shutdown();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - start)
                       .count();
    if (mode != "idle")
      require(callback_finished, "in-flight native callback was cancelled before returning");
    require(finished_count.load() == callback_count.load(),
            "an in-flight callback did not finish before driver destruction");
    require(destroyed.load() == uint32_t(mode != "idle"), "driver was not destroyed exactly once");
    audio->Shutdown();
    audio->Pause();  // A partially initialized or stopped worker must not wait on a fence.
    runtime.Shutdown();
    require(destroyed.load() == uint32_t(mode != "idle"),
            "repeated shutdown destroyed driver again");
    require(elapsed < 2000, "cooperative shutdown exceeded its deadline");
    if (argc == 3) {
      std::ofstream report(argv[2]);
      require(bool(report), "cannot open shutdown report");
      report << "{\"passed\":true,\"mode\":\"" << mode << "\",\"shutdown_ms\":" << elapsed
             << ",\"callback_finished\":" << (callback_finished ? "true" : "false")
             << ",\"pause_cycles\":" << pause_cycles << ",\"max_pause_ms\":" << max_pause_ms
             << ",\"destroyed_drivers\":" << destroyed.load() << "}\n";
      report.close();
      require(bool(report), "cannot write shutdown report");
    }
    std::printf("PASS %s shutdown %lld ms\n", mode.c_str(), (long long)elapsed);
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "FAIL: %s\n", e.what());
    return 1;
  }
}
