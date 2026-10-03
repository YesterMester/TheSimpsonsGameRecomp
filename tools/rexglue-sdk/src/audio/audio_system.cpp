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

namespace rex::audio {

AudioSystem::AudioSystem(runtime::FunctionDispatcher* function_dispatcher)
    : memory_(function_dispatcher->memory()),
      function_dispatcher_(function_dispatcher),
      worker_running_(false) {
  std::memset(clients_, 0, sizeof(clients_));

  queued_frames_ = std::min(
      static_cast<uint32_t>(kMaximumQueuedFrames),
      std::max(static_cast<uint32_t>(REXCVAR_GET(audio_maxqframes)), static_cast<uint32_t>(4)));

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
  // fall back to the highest nice level for this thread.
  {
#if defined(_WIN32)
    // Windows equivalent of the POSIX realtime-ish bump: time-critical priority
    // keeps the audio worker ahead of GPU/streaming threads.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
#else
    struct sched_param sp {};
    sp.sched_priority = 10;
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != 0) {
      // Not privileged for SCHED_FIFO: use nice -10 for this thread.
      errno = 0;
      (void)nice(-10);
    }
#endif
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

#if REX_PLATFORM_WIN32
  // On Windows, std::this_thread::sleep_until wakes on the system timer tick,
  // 15.6 ms unless the process timer resolution is raised, and Windows does
  // not always grant that (#36). The callbacks then came about 128 times a
  // second instead of 187.5: the queue stayed at one or two frames and a
  // third of the output was silence, choppy and slowed down. A
  // high-resolution timer (Windows 10 1803 and newer) wakes within about
  // 0.5 ms of a relative due time whatever the timer resolution.
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
  HANDLE pacing_timer = CreateWaitableTimerExW(
      nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
#endif
  auto pace_until = [&](std::chrono::steady_clock::time_point due) {
#if REX_PLATFORM_WIN32
    if (pacing_timer) {
      LARGE_INTEGER relative;
      relative.QuadPart = -std::chrono::duration_cast<rex::chrono::hundrednanoseconds>(
                               due - std::chrono::steady_clock::now())
                               .count();
      if (relative.QuadPart < 0 &&
          SetWaitableTimer(pacing_timer, &relative, 0, nullptr, nullptr, FALSE)) {
        WaitForSingleObject(pacing_timer, INFINITE);
      }
      return;
    }
#endif
    std::this_thread::sleep_until(due);
  };

  // audio_log_underruns: callbacks per second, how long the game's callback
  // runs and how late the paced wait wakes (a coarse timer shows here).
  struct PacingDiag {
    uint32_t callbacks = 0, waits = 0, unpaced = 0;
    double callback_sum_us = 0, callback_max_us = 0, late_sum_us = 0, late_max_us = 0;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
  } pacing_diag;

  // Main run loop.
  uint32_t diag_pump_count = 0;
  while (worker_running_) {
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
      // Shutdown event signaled.
      if (paused_) {
        pause_fence_.Signal();
        thread::Wait(resume_event_.get(), false);
      }

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
          if (now < next_callback[index]) {
            pace_until(next_callback[index]);
            if (REXCVAR_GET(audio_log_underruns)) {
              double late = std::chrono::duration<double, std::micro>(
                                std::chrono::steady_clock::now() - next_callback[index])
                                .count();
              pacing_diag.late_sum_us += late;
              pacing_diag.late_max_us = std::max(pacing_diag.late_max_us, late);
              ++pacing_diag.waits;
            }
            now = next_callback[index];
          } else {
            ++pacing_diag.unpaced;
          }
          next_callback[index] = now + kCallbackInterval;
        }
        if (diag_pump_count < 10) {
          REXAPU_DEBUG("AudioWorker: dispatching callback {:08X} with arg {:08X} for client {}",
                       client_callback, client_callback_arg, index);
        }
        SCOPE_profile_cpu_i("apu", "rex::audio::AudioSystem->client_callback");
        uint64_t args[] = {client_callback_arg};
        auto callback_start = std::chrono::steady_clock::now();
        function_dispatcher_->Execute(worker_thread_->thread_state(), client_callback, args,
                                      rex::countof(args));
        if (REXCVAR_GET(audio_log_underruns)) {
          auto end = std::chrono::steady_clock::now();
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
                pacing_diag.callbacks / elapsed, pacing_diag.callback_sum_us / pacing_diag.callbacks,
                pacing_diag.callback_max_us,
                pacing_diag.waits ? pacing_diag.late_sum_us / pacing_diag.waits : 0.0,
                pacing_diag.late_max_us, pacing_diag.waits, pacing_diag.unpaced);
            pacing_diag = {};
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
#if REX_PLATFORM_WIN32
  if (pacing_timer) {
    CloseHandle(pacing_timer);
  }
#endif

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
  if (!worker_running_) {
    return;
  }

  // Shut down XMA decoder first - its worker can stall in FFmpeg
  if (xma_decoder_) {
    xma_decoder_->Shutdown();
  }

  worker_running_ = false;
  shutdown_event_->Set();
  if (worker_thread_) {
    // The worker may be stuck inside a guest callback that is itself blocked
    // on guest objects (e.g. KeWaitForMultipleObjects).
    // Terminate the thread to break the deadlock.
    worker_thread_->Terminate(0);
    worker_thread_.reset();
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
  if (paused_) {
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
