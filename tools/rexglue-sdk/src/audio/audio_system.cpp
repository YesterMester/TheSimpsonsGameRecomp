/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/assert.h>
#include <rex/audio/audio_driver.h>
#include <rex/audio/audio_system.h>
#include <rex/audio/flags.h>
#include <rex/audio/thread_priority.h>
#include <rex/audio/xma/decoder.h>
#include <rex/dbg.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/memory/ring_buffer.h>
#include <rex/stream.h>
#include <rex/string/buffer.h>
#include <rex/system/thread_state.h>
#include <rex/thread.h>
#include <rex/cvar.h>

#include <algorithm>
#include <chrono>
#include <thread>

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_thread.h>
#include <SDL3/SDL_timer.h>
#if REX_PLATFORM_ANDROID
#include <SDL3/SDL_system.h>
#include <jni.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#endif
#if REX_PLATFORM_LINUX
#include <SDL3/SDL_system.h>
#include <sys/syscall.h>
#endif

REXCVAR_DEFINE_INT32(
    audio_maxqframes, 8, "Audio",
    "Max buffered audio frames (range 4-64). Lower reduces latency but may cause stuttering.");

REXCVAR_DECLARE(bool, audio_log_underruns);

REXCVAR_DEFINE_BOOL(audio_callback_pacing, true, "Audio",
                    "Ask the game for audio frames at the hardware's steady rate instead of as "
                    "fast as queue space frees up (bursts make the game submit silent frames)");

// As with normal Microsoft, there are like twelve different ways to access
// the audio APIs. Early games use XMA*() methods almost exclusively to touch
// decoders. Later games use XAudio*() and direct memory writes to the XMA
// structures (as opposed to the XMA* calls), meaning that we have to support
// both.
//
// For ease of implementation, most audio related processing is handled in
// AudioSystem, and the functions here call off to it.
// The XMA*() functions just manipulate the audio system in the guest context
// and let the normal AudioSystem handling take it, to prevent duplicate
// implementations. They can be found in xboxkrnl_audio_xma.cc

#if defined(_WIN32)
#include <windows.h>
#else
#include <pthread.h>
#include <unistd.h>
#include <cerrno>
#endif
#if REX_PLATFORM_GNU_LINUX && !REX_PLATFORM_ANDROID
#include <sched.h>
#include <signal.h>
#include <sys/resource.h>
#include <time.h>

#include <mutex>

namespace {
thread_local uint64_t audio_realtime_budget_ns = 0;
thread_local uint64_t audio_realtime_start_ns = 0;
thread_local uint64_t audio_realtime_check_wall_ns = 0;
thread_local uint32_t audio_realtime_calls = 0;

uint64_t AudioThreadCpuNs() {
  timespec now{};
  if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now) != 0)
    return 0;
  return uint64_t(now.tv_sec) * 1000000000u + uint64_t(now.tv_nsec);
}

// The soft realtime CPU limit (RLIMIT_RTTIME) was reached: a realtime thread
// ran for its whole budget without blocking, for instance spinning on a lock
// that a thread it keeps off the CPU holds. The default action ends the game,
// and at the hard limit RTKit's SIGKILL would. The signal goes to the thread
// that overran: put it back on normal scheduling, which no longer counts the
// limit, and let the game carry on.
void OnRealtimeLimit(int) {
  sched_param param{};
  syscall(SYS_sched_setscheduler, 0, SCHED_OTHER, &param);
  static const char kMessage[] =
      "[audio] a realtime thread overran its CPU budget; it now runs at normal priority\n";
  (void)!write(2, kMessage, sizeof(kMessage) - 1);
}

void StartAudioRealtimeBudget() {
  static std::once_flag handler_once;
  std::call_once(handler_once, [] {
    struct sigaction action{};
    action.sa_handler = OnRealtimeLimit;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART;
    sigaction(SIGXCPU, &action, nullptr);
  });
  // RTKit usually grants 100 ms before SIGXCPU and 200 ms before SIGKILL.
  // Keep its limits and scheduling priority; sleep at safe work boundaries
  // well before that continuous CPU budget. sched_yield does not reset it.
  audio_realtime_budget_ns = 20000000;
  rlimit limit{};
  if (getrlimit(RLIMIT_RTTIME, &limit) == 0 && limit.rlim_cur != RLIM_INFINITY)
    audio_realtime_budget_ns =
        std::min(audio_realtime_budget_ns, std::max<uint64_t>(1, limit.rlim_cur / 4) * 1000);
  audio_realtime_start_ns = AudioThreadCpuNs();
}
}  // namespace
#endif

extern "C" bool rex_audio_yield_realtime_budget() {
#if REX_PLATFORM_GNU_LINUX && !REX_PLATFORM_ANDROID
  if (!audio_realtime_budget_ns)
    return false;
  // Voice-state polling can call this thousands of times per millisecond.
  // Look at the wall clock on every 16th call only (the guest's own timebase
  // reads around it cost a clock read each already), and ask for the more
  // expensive thread CPU clock at most once per millisecond, still well
  // inside the RT budget.
  if (++audio_realtime_calls & 15)
    return false;
  timespec wall{};
  if (clock_gettime(CLOCK_MONOTONIC, &wall) != 0)
    return false;
  const uint64_t wall_ns = uint64_t(wall.tv_sec) * 1000000000u + uint64_t(wall.tv_nsec);
  if (wall_ns < audio_realtime_check_wall_ns)
    return false;
  audio_realtime_check_wall_ns = wall_ns + 1000000;
  const uint64_t now = AudioThreadCpuNs();
  if (!now || now - audio_realtime_start_ns < audio_realtime_budget_ns)
    return false;
  timespec remaining{0, 100000};
  while (nanosleep(&remaining, &remaining) != 0) {
    if (errno != EINTR)
      return false;
  }
  audio_realtime_start_ns = AudioThreadCpuNs();
  return true;
#else
  return false;
#endif
}

extern "C" bool rex_audio_set_thread_priority() {
#if defined(_WIN32)
  return SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL) != 0;
#elif REX_PLATFORM_ANDROID
  // SCHED_FIFO is privileged, and SDL's pthread fallback leaves SCHED_OTHER's
  // nice level unchanged on Android. Use Android's audio priority for both the
  // mixer and XMA decoder, as SDL already does for the device output thread.
  auto* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
  if (!env) {
    return SDL_SetError("Android audio thread could not attach to the Java VM");
  }
  jclass process = env->FindClass("android/os/Process");
  if (!process) {
    env->ExceptionClear();
    return SDL_SetError("Android audio thread could not find Process");
  }
  jmethodID set_priority = env->GetStaticMethodID(process, "setThreadPriority", "(I)V");
  if (!set_priority) {
    env->ExceptionClear();
    env->DeleteLocalRef(process);
    return SDL_SetError("Android audio thread could not find setThreadPriority");
  }
  env->CallStaticVoidMethod(process, set_priority, jint(-16));  // THREAD_PRIORITY_AUDIO
  const bool failed = env->ExceptionCheck();
  if (failed)
    env->ExceptionClear();
  env->DeleteLocalRef(process);
  if (failed)
    return SDL_SetError("Android denied the audio thread priority request");
  REXAPU_INFO("Android audio worker scheduling: nice {}",
              getpriority(PRIO_PROCESS, id_t(syscall(SYS_gettid))));
  return true;
#else
  struct sched_param sp{};
  sp.sched_priority = 10;
  if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) == 0) {
#if REX_PLATFORM_GNU_LINUX
    StartAudioRealtimeBudget();
#endif
    return true;
  }
#if REX_PLATFORM_GNU_LINUX
  if (SDL_SetLinuxThreadPriorityAndPolicy(syscall(SYS_gettid), SDL_THREAD_PRIORITY_TIME_CRITICAL,
                                          SCHED_RR)) {
    StartAudioRealtimeBudget();
    return true;
  }
#endif
  return SDL_SetCurrentThreadPriority(SDL_THREAD_PRIORITY_HIGH);
#endif
}

namespace rex::audio {

AudioSystem::AudioSystem(runtime::FunctionDispatcher* function_dispatcher)
    : memory_(function_dispatcher->memory()),
      function_dispatcher_(function_dispatcher),
      worker_running_(false) {
  std::memset(clients_, 0, sizeof(clients_));

  queued_frames_ =
      uint32_t(std::clamp(REXCVAR_GET(audio_maxqframes), 4, int32_t(kMaximumQueuedFrames)));

  for (size_t i = 0; i < kMaximumClientCount; ++i) {
    client_semaphores_[i] = rex::thread::Semaphore::Create(0, queued_frames_);
    assert_not_null(client_semaphores_[i]);
    wait_handles_[i] = client_semaphores_[i].get();
  }
  shutdown_event_ = rex::thread::Event::CreateAutoResetEvent(false);
  assert_not_null(shutdown_event_);
  wait_handles_[kMaximumClientCount] = shutdown_event_.get();

  xma_decoder_ = std::make_unique<rex::audio::XmaDecoder>(function_dispatcher_);

  resume_event_ = rex::thread::Event::CreateAutoResetEvent(false);
  assert_not_null(resume_event_);
}

AudioSystem::~AudioSystem() {
  if (xma_decoder_) {
    xma_decoder_->Shutdown();
  }
}

X_STATUS AudioSystem::Setup(system::KernelState* kernel_state) {
  X_STATUS result = xma_decoder_->Setup(kernel_state);
  if (result) {
    return result;
  }

  worker_running_ = true;
  worker_thread_ = system::object_ref<system::XHostThread>(
      new system::XHostThread(kernel_state, 128 * 1024, 0, [this]() {
        WorkerThreadMain();
        return 0;
      }));

  worker_thread_->set_name("Audio Worker");
  worker_thread_->Create();

  return X_STATUS_SUCCESS;
}

void AudioSystem::WorkerThreadMain() {
  // HAND PATCH: raise the audio worker to realtime-ish priority so guest
  // audio callbacks aren't starved by GPU/streaming threads (audible
  // stutter under load). SCHED_FIFO needs privileges we may not have, so
  // use SDL's native scheduling helper, including the desktop priority broker.
  if (!rex_audio_set_thread_priority()) {
    REXAPU_WARN("AudioWorker: cannot raise thread priority: {}", SDL_GetError());
  }
  // Initialize driver and ringbuffer.
  Initialize();

  // The hardware asks for one frame (256 samples) every 5.33 ms of playback,
  // and the game's mixer renders at that pace: called again right away, its
  // callback submits a frame of silence. But the audio driver frees queue
  // slots in bursts (a PipeWire period is four frames), so callbacks are
  // spread out to just under the frame duration, which keeps the queue full.
  constexpr auto kCallbackInterval = std::chrono::microseconds(5333 * 39 / 40);
  std::chrono::steady_clock::time_point next_callback[kMaximumClientCount] = {};
  std::chrono::steady_clock::time_point last_callback[kMaximumClientCount] = {};

  // audio_log_underruns: callbacks per second, how long the game's callback
  // runs and how late the paced wait wakes (a coarse timer shows here).
  struct PacingDiag {
    uint32_t callbacks = 0, waits = 0, unpaced = 0;
    double callback_sum_us = 0, callback_max_us = 0, late_sum_us = 0, late_max_us = 0;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point last_callback = {};
  } pacing_diag;

  // Main run loop.
  uint32_t diag_pump_count = 0;
  while (worker_running_) {
    // Client semaphores have lower wait indices and can stay ready while the
    // device consumes audio. Acknowledge pause before selecting another client
    // so a busy queue cannot starve it. A callback already selected below is
    // allowed to finish, preserving its queue permit and guest synchronization.
    if (paused_) {
      pause_fence_.Signal();
      thread::Wait(resume_event_.get(), false);
      continue;
    }
    rex_audio_yield_realtime_budget();
    // These handles signify the number of submitted samples. Once we reach
    // 64 samples, we wait until our audio backend releases a semaphore
    // (signaling a sample has finished playing)
    auto result = rex::thread::WaitAny(wait_handles_, rex::countof(wait_handles_), true,
                                       std::chrono::milliseconds(500));
    if (result.first == rex::thread::WaitResult::kFailed) {
      REXAPU_WARN("AudioWorker: WaitAny failed");
      continue;
    }

    if (result.first == rex::thread::WaitResult::kTimeout) {
      if (diag_pump_count < 5) {
        REXAPU_NOISY_DEBUG("AudioWorker: WaitAny timed out (no semaphore signals)");
      }
    }

    if (result.first == thread::WaitResult::kSuccess && result.second == kMaximumClientCount) {
      // Wake for shutdown or pause. The next loop checks these requests before
      // consuming another client's queue capacity.
      continue;
    }

    // Number of clients pumped
    bool pumped = false;
    if (result.first == rex::thread::WaitResult::kSuccess) {
      auto index = result.second;

      auto global_lock = global_critical_region_.Acquire();
      uint32_t client_callback = clients_[index].callback;
      uint32_t client_callback_arg = clients_[index].wrapped_callback_arg;
      global_lock.unlock();

      if (client_callback) {
        if (REXCVAR_GET(audio_callback_pacing)) {
          auto now = std::chrono::steady_clock::now();
          // Keep the playback deadline separate from the minimum spacing.
          // Moving the deadline forward after every late wake permanently
          // loses those samples, even when the worker has time to recover.
          const auto wake =
              std::max(next_callback[index], last_callback[index] + kCallbackInterval / 2);
          if (now < wake) {
            // SDL uses a high-resolution timer on Windows. Keep the existing
            // schedule and measure the actual wake rather than the deadline.
            SDL_DelayNS(
                uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(wake - now).count()));
            now = std::chrono::steady_clock::now();
            if (REXCVAR_GET(audio_log_underruns)) {
              const double late = std::chrono::duration<double, std::micro>(now - wake).count();
              pacing_diag.late_sum_us += std::max(0.0, late);
              pacing_diag.late_max_us = std::max(pacing_diag.late_max_us, late);
              ++pacing_diag.waits;
            }
          } else if (REXCVAR_GET(audio_log_underruns)) {
            ++pacing_diag.unpaced;
          }
          // Catch up gradually after scheduling jitter, with at least half a
          // frame between callback starts. A pause longer than the entire
          // queue starts a new schedule rather than retaining stale deadlines.
          if (next_callback[index] == std::chrono::steady_clock::time_point{} ||
              now - next_callback[index] > kCallbackInterval * queued_frames_) {
            next_callback[index] = now;
          }
          next_callback[index] += kCallbackInterval;
          last_callback[index] = now;
        }
        if (!worker_running_) {
          break;
        }
        if (diag_pump_count < 10) {
          REXAPU_DEBUG("AudioWorker: dispatching callback {:08X} with arg {:08X} for client {}",
                       client_callback, client_callback_arg, index);
        }
        SCOPE_profile_cpu_i("apu", "rex::audio::AudioSystem->client_callback");
        uint64_t args[] = {client_callback_arg};
        const bool log_pacing = REXCVAR_GET(audio_log_underruns);
        const auto callback_start =
            log_pacing ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        function_dispatcher_->Execute(worker_thread_->thread_state(), client_callback, args,
                                      rex::countof(args));
        if (log_pacing) {
          auto end = std::chrono::steady_clock::now();
          // Do not average a pause, loading gap or disabled diagnostic period
          // into the callback rate reported for active playback.
          if (pacing_diag.last_callback == std::chrono::steady_clock::time_point{} ||
              callback_start - pacing_diag.last_callback > std::chrono::milliseconds(250)) {
            pacing_diag = {};
            pacing_diag.start = callback_start;
          }
          pacing_diag.last_callback = end;
          double took = std::chrono::duration<double, std::micro>(end - callback_start).count();
          pacing_diag.callback_sum_us += took;
          pacing_diag.callback_max_us = std::max(pacing_diag.callback_max_us, took);
          ++pacing_diag.callbacks;
          double elapsed = std::chrono::duration<double>(end - pacing_diag.start).count();
          if (elapsed >= 5.0) {
            REXAPU_INFO(
                "[audio-pacing] {:.1f} callbacks/s (hardware: 187.5), callback avg {:.0f} us "
                "max {:.0f} us; paced wait woke late avg {:.0f} us max {:.0f} us over {} waits, "
                "{} callbacks already late",
                pacing_diag.callbacks / elapsed,
                pacing_diag.callback_sum_us / pacing_diag.callbacks, pacing_diag.callback_max_us,
                pacing_diag.waits ? pacing_diag.late_sum_us / pacing_diag.waits : 0.0,
                pacing_diag.late_max_us, pacing_diag.waits, pacing_diag.unpaced);
            pacing_diag = {};
            pacing_diag.start = pacing_diag.last_callback = end;
          }
        }
        if (diag_pump_count < 10) {
          REXAPU_DEBUG("AudioWorker: callback returned for client {}", index);
        }
        diag_pump_count++;
      } else {
        REXAPU_DEBUG("AudioWorker: semaphore signaled for client {} but callback is 0", index);
      }

      pumped = true;
    }

    if (!worker_running_) {
      break;
    }

    if (!pumped) {
      SCOPE_profile_cpu_i("apu", "Sleep");
      rex::thread::Sleep(std::chrono::milliseconds(500));
    }
  }
  worker_running_ = false;

  // TODO(benvanik): call module API to kill?
}

int AudioSystem::FindFreeClient() {
  for (size_t i = 0; i < kMaximumClientCount; i++) {
    auto& client = clients_[i];
    if (!client.in_use) {
      return i;
    }
  }

  return -1;
}

void AudioSystem::Initialize() {}

void AudioSystem::Shutdown() {
  if (!worker_thread_ && !worker_running_) {
    return;
  }

  worker_running_ = false;
  shutdown_event_->Set();
  // A paused mixer may already be waiting here instead of on shutdown_event_.
  resume_event_->Set();
  if (worker_thread_) {
    // Keep the decoder alive while the current callback finishes. Cancelling
    // normal native work asynchronously can unwind through noexcept functions
    // and abort the process. Only a callback blocked on stopped guest threads
    // needs the existing forced termination path.
    auto result =
        rex::thread::Wait(worker_thread_->thread(), false, std::chrono::milliseconds(2000));
    if (result != rex::thread::WaitResult::kSuccess) {
      REXAPU_WARN("Audio worker did not stop cooperatively; terminating the blocked callback");
      worker_thread_->Terminate(0);
    }
    worker_thread_.reset();
  }

  if (xma_decoder_) {
    xma_decoder_->Shutdown();
  }

  // Destroy all active client drivers (closes SDL audio devices, stopping
  // callback threads) before the semaphores they reference are destroyed.
  for (size_t i = 0; i < kMaximumClientCount; i++) {
    if (clients_[i].in_use) {
      DestroyDriver(clients_[i].driver);
      if (clients_[i].wrapped_callback_arg) {
        memory()->SystemHeapFree(clients_[i].wrapped_callback_arg);
      }
      clients_[i] = {nullptr, 0, 0, 0, false};
    }
  }
}

X_STATUS AudioSystem::RegisterClient(uint32_t callback, uint32_t callback_arg, size_t* out_index) {
  REXAPU_DEBUG("AudioSystem::RegisterClient: callback={:08X} callback_arg={:08X}", callback,
               callback_arg);
  auto global_lock = global_critical_region_.Acquire();

  auto index = FindFreeClient();
  assert_true(index >= 0);
  REXAPU_DEBUG("AudioSystem::RegisterClient: using client index={} queued_frames={}", index,
               queued_frames_);

  auto client_semaphore = client_semaphores_[index].get();
  auto ret = client_semaphore->Release(queued_frames_, nullptr);
  assert_true(ret);

  AudioDriver* driver;
  auto result = CreateDriver(index, client_semaphore, &driver);
  if (XFAILED(result)) {
    return result;
  }
  assert_not_null(driver);

  uint32_t ptr = memory()->SystemHeapAlloc(0x4);
  memory::store_and_swap<uint32_t>(memory()->TranslateVirtual(ptr), callback_arg);

  clients_[index] = {driver, callback, callback_arg, ptr, true};

  if (out_index) {
    *out_index = index;
  }

  return X_STATUS_SUCCESS;
}

void AudioSystem::SubmitFrame(size_t index, uint32_t samples_ptr) {
  SCOPE_profile_cpu_f("apu");
  rex_audio_yield_realtime_budget();

  static uint32_t submit_count = 0;
  if (submit_count < 10) {
    REXAPU_DEBUG("AudioSystem::SubmitFrame called: index={} samples_ptr={:08X}", index,
                 samples_ptr);
    submit_count++;
  }

  auto global_lock = global_critical_region_.Acquire();
  assert_true(index < kMaximumClientCount);
  assert_true(clients_[index].driver != NULL);
  (clients_[index].driver)->SubmitFrame(samples_ptr);
}

void AudioSystem::UnregisterClient(size_t index) {
  SCOPE_profile_cpu_f("apu");

  auto global_lock = global_critical_region_.Acquire();
  assert_true(index < kMaximumClientCount);
  DestroyDriver(clients_[index].driver);
  memory()->SystemHeapFree(clients_[index].wrapped_callback_arg);
  clients_[index] = {nullptr, 0, 0, 0, false};

  // Drain the semaphore of its count.
  auto client_semaphore = client_semaphores_[index].get();
  rex::thread::WaitResult wait_result;
  do {
    wait_result = rex::thread::Wait(client_semaphore, false, std::chrono::milliseconds(0));
  } while (wait_result == rex::thread::WaitResult::kSuccess);
  assert_true(wait_result == rex::thread::WaitResult::kTimeout);
}

bool AudioSystem::Save(stream::ByteStream* stream) {
  stream->Write(kAudioSaveSignature);

  // Count the number of used clients first.
  // Any gaps should be handled gracefully.
  uint32_t used_clients = 0;
  for (size_t i = 0; i < kMaximumClientCount; i++) {
    if (clients_[i].in_use) {
      used_clients++;
    }
  }

  stream->Write(used_clients);
  for (uint32_t i = 0; i < kMaximumClientCount; i++) {
    auto& client = clients_[i];
    if (!client.in_use) {
      continue;
    }

    stream->Write(i);
    stream->Write(client.callback);
    stream->Write(client.callback_arg);
    stream->Write(client.wrapped_callback_arg);
  }

  return true;
}

bool AudioSystem::Restore(stream::ByteStream* stream) {
  if (stream->Read<uint32_t>() != kAudioSaveSignature) {
    REXAPU_ERROR("AudioSystem::Restore - Invalid magic value!");
    return false;
  }

  uint32_t num_clients = stream->Read<uint32_t>();
  for (uint32_t i = 0; i < num_clients; i++) {
    auto id = stream->Read<uint32_t>();
    assert_true(id < kMaximumClientCount);

    auto& client = clients_[id];

    // Reset the semaphore and recreate the driver ourselves.
    if (client.driver) {
      UnregisterClient(id);
    }

    client.callback = stream->Read<uint32_t>();
    client.callback_arg = stream->Read<uint32_t>();
    client.wrapped_callback_arg = stream->Read<uint32_t>();

    client.in_use = true;

    auto client_semaphore = client_semaphores_[id].get();
    auto ret = client_semaphore->Release(queued_frames_, nullptr);
    assert_true(ret);

    AudioDriver* driver = nullptr;
    auto status = CreateDriver(id, client_semaphore, &driver);
    if (XFAILED(status)) {
      REXAPU_ERROR(
          "AudioSystem::Restore - Call to CreateDriver failed with status "
          "{:08X}",
          status);
      return false;
    }

    assert_not_null(driver);
    client.driver = driver;
  }

  return true;
}

void AudioSystem::Pause() {
  if (paused_ || !worker_running_) {
    return;
  }
  paused_ = true;

  // Kind of a hack, but it works.
  shutdown_event_->Set();
  pause_fence_.Wait();

  xma_decoder_->Pause();
}

void AudioSystem::Resume() {
  if (!paused_) {
    return;
  }
  paused_ = false;

  resume_event_->Set();

  xma_decoder_->Resume();
}

}  // namespace rex::audio
