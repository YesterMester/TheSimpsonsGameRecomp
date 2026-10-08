/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <string>

#include <rex/assert.h>
#include <rex/audio/conversion.h>
#include <rex/audio/flags.h>
#include <rex/audio/sdl/sdl_audio_driver.h>
#include <rex/cvar.h>
#include <rex/dbg.h>
#include <rex/logging.h>
#include <rex/perf/counter.h>
#include <rex/platform.h>
#include <SDL3/SDL.h>

#if REX_PLATFORM_WIN32
#include <windows.h>
#endif

REXCVAR_DEFINE_BOOL(audio_mute, false, "Audio", "Mute audio output");
// Diagnostics: every frame the game submits, raw (256 samples x 6 channels,
// big-endian float, channel after channel), and in <file>.ts the
// monotonic nanosecond timestamp of each, for measuring gaps without listening.
REXCVAR_DEFINE_STRING(
    audio_dump_file, "", "Audio",
    "Write every submitted audio frame (raw 6 x 256 big-endian floats) to this file, "
    "and uint64 nanosecond timestamps to <file>.ts; empty = off")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(audio_log_underruns, false, "Audio",
                    "Log how many played frames were silence because no frame was queued, and how "
                    "many frames the game submitted were entirely silent (diagnostic)");

namespace rex::audio::sdl {

SDLAudioDriver::SDLAudioDriver(memory::Memory* memory, rex::thread::Semaphore* semaphore)
    : AudioDriver(memory), semaphore_(semaphore) {}

SDLAudioDriver::~SDLAudioDriver() {
  assert_true(frames_queued_.empty());
  assert_true(frames_unused_.empty());
}

bool SDLAudioDriver::Initialize() {
  // Prevent SDL from interfering with timer resolution (causes FPS drops)
  SDL_SetHintWithPriority(SDL_HINT_TIMER_RESOLUTION, "0", SDL_HINT_OVERRIDE);
#if REX_PLATFORM_WIN32
  // SDL applies the hint by ending the 1 ms period it began when it started,
  // and timeEndPeriod withdraws the process's timer resolution request as a
  // whole, the one the app made at startup included. From here on every sleep
  // and plain waitable timer of the process ran at the default 15.6 ms: the
  // game's 10 ms timers 64 times a second instead of 100, Sleep(1) 15.5 ms,
  // 30 fps vblanks 31 or 47 ms apart. Ask for it again.
  rex::thread::RequestHighResolutionTimer();
#endif

  // Set audio category for proper OS audio handling
  SDL_SetHint(SDL_HINT_AUDIO_CATEGORY, "playback");

#if REX_PLATFORM_LINUX
  // HAND PATCH: SDL's default audio driver auto-probe can land on "jack"
  // before ever trying pipewire/pulseaudio, and if no real JACK server is
  // reachable (common -- SteamOS runs PipeWire, whose JACK-compatibility
  // shim isn't always present/started, especially inside sandboxed compat
  // tool environments like Steam Linux Runtime), SDL_InitSubSystem(AUDIO)
  // fails outright with "Can't open JACK client" instead of continuing on
  // to a driver that actually works. Steer it straight at the drivers that
  // are real, present, and reliable on this platform. Linux only: naming
  // these drivers on Windows/macOS makes SDL fail init entirely ("Audio
  // target 'pipewire,pulseaudio,alsa' not available").
  SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "pipewire,pulseaudio,alsa");
#endif

  // Set app name for audio device identification
  SDL_SetAppMetadataProperty(SDL_PROP_APP_METADATA_NAME_STRING, "rexglue");

  if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
#if REX_PLATFORM_LINUX
    // The steered driver list can itself be wrong on unusual setups (e.g. a
    // bare ALSA-less system). Fall back to SDL's own probe order before
    // giving up on audio entirely.
    SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "");
    if (SDL_InitSubSystem(SDL_INIT_AUDIO)) {
      REXAPU_WARN("preferred audio drivers unavailable; using SDL default probe");
    } else
#endif
    {
      REXAPU_ERROR("SDL_InitSubSystem(SDL_INIT_AUDIO) failed: {}", SDL_GetError());
      return false;
    }
  }
  sdl_initialized_ = true;

  SDL_AudioSpec desired_spec = {};
  SDL_AudioSpec obtained_spec = {};
  desired_spec.freq = frame_frequency_;
  desired_spec.format = SDL_AUDIO_F32LE;
  desired_spec.channels = frame_channels_;
  sdl_device_channels_ = frame_channels_;
  sdl_stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &desired_spec,
                                          SDLCallback, this);
  if (!sdl_stream_) {
    REXAPU_ERROR("SDL_OpenAudioDeviceStream() failed: {}", SDL_GetError());
    return false;
  }

  SDL_AudioDeviceID sdl_device = SDL_GetAudioStreamDevice(sdl_stream_);
  if (!sdl_device) {
    REXAPU_ERROR("SDL_GetAudioStreamDevice() failed: {}", SDL_GetError());
    return false;
  }

  if (!SDL_GetAudioDeviceFormat(sdl_device, &obtained_spec, NULL)) {
    REXAPU_WARN("SDL_GetAudioDeviceFormat() failed: {}", SDL_GetError());
    obtained_spec = desired_spec;
  }

  if (obtained_spec.channels == 2) {
    SDL_DestroyAudioStream(sdl_stream_);
    sdl_stream_ = nullptr;
    desired_spec.channels = 2;
    sdl_device_channels_ = 2;
    sdl_stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &desired_spec,
                                            SDLCallback, this);
    if (!sdl_stream_) {
      REXAPU_ERROR("SDL_OpenAudioDeviceStream() stereo fallback failed: {}", SDL_GetError());
      return false;
    }
    sdl_device = SDL_GetAudioStreamDevice(sdl_stream_);
    if (!sdl_device) {
      REXAPU_ERROR("SDL_GetAudioStreamDevice() failed after stereo fallback: {}", SDL_GetError());
      return false;
    }
  }

  if (!SDL_ResumeAudioDevice(sdl_device)) {
    REXAPU_ERROR("SDL_ResumeAudioDevice() failed: {}", SDL_GetError());
    return false;
  }

  const std::string dump_path = REXCVAR_GET(audio_dump_file);
  if (!dump_path.empty()) {
    dump_ = std::fopen(dump_path.c_str(), "wb");
    dump_timestamps_ = std::fopen((dump_path + ".ts").c_str(), "wb");
    if (!dump_ || !dump_timestamps_) {
      REXAPU_WARN("Could not open audio dump '{}'; dumping disabled", dump_path);
      if (dump_)
        std::fclose(dump_);
      if (dump_timestamps_)
        std::fclose(dump_timestamps_);
      dump_ = dump_timestamps_ = nullptr;
    }
  }
  return true;
}

void SDLAudioDriver::SubmitFrame(uint32_t frame_ptr) {
  const auto input_frame = memory_->TranslateVirtual<float*>(frame_ptr);
  float* output_frame;
  {
    std::unique_lock<std::mutex> guard(frames_mutex_);
    if (frames_unused_.empty()) {
      output_frame = new float[frame_samples_];
    } else {
      output_frame = frames_unused_.top();
      frames_unused_.pop();
    }
  }

  std::memcpy(output_frame, input_frame, frame_samples_ * sizeof(float));

  if (dump_ && dump_timestamps_) {
    const uint64_t now = SDL_GetTicksNS();
    if (std::fwrite(input_frame, sizeof(float), frame_samples_, dump_) != frame_samples_ ||
        std::fwrite(&now, sizeof(now), 1, dump_timestamps_) != 1) {
      REXAPU_WARN("Audio dump write failed; stopping the dump");
      std::fclose(dump_);
      std::fclose(dump_timestamps_);
      dump_ = dump_timestamps_ = nullptr;
    }
  }

  bool log_underruns = REXCVAR_GET(audio_log_underruns);
  bool silent_frame = false;
  if (log_underruns) {
    silent_frame = std::all_of(output_frame, output_frame + frame_samples_,
                               [](float sample) { return sample == 0.0f; });
  }

  std::array<uint32_t, 5> diagnostic = {};
  double queue_average = 0.0;
  uint32_t queue_minimum = 0;
  bool report = false;
  size_t queued_count;
  {
    std::unique_lock<std::mutex> guard(frames_mutex_);
    frames_queued_.push(output_frame);
    queued_count = frames_queued_.size();
    PROFILE_BUFFER_QUEUE_DEPTH(static_cast<int64_t>(queued_count));
    if (log_underruns) {
      diag_silent_submitted_frames_ += uint32_t(silent_frame);
      diag_queue_sum_ += queued_count;
      diag_queue_min_ = std::min<uint32_t>(diag_queue_min_, uint32_t(queued_count));
      // Snapshot under the lock, then log outside it so the realtime
      // callback never waits for a file or terminal write.
      if (++diag_submitted_frames_ >= 960) {
        diagnostic = {diag_played_frames_, diag_underrun_frames_, diag_submitted_frames_,
                      diag_silent_submitted_frames_, uint32_t(queued_count)};
        queue_average = double(diag_queue_sum_) / diag_submitted_frames_;
        queue_minimum = diag_queue_min_;
        diag_queue_sum_ = 0;
        diag_queue_min_ = UINT32_MAX;
        diag_played_frames_ = 0;
        diag_underrun_frames_ = 0;
        diag_submitted_frames_ = 0;
        diag_silent_submitted_frames_ = 0;
        report = true;
      }
    }
  }
  static uint32_t sdl_submit_count = 0;
  if (sdl_submit_count < 10) {
    ++sdl_submit_count;
    REXAPU_DEBUG("SDLAudioDriver::SubmitFrame: frame_ptr={:08X} queued_count={}", frame_ptr,
                 queued_count);
  }
  if (report) {
    REXAPU_INFO(
        "[audio-diag] played {} frames: {} silence (nothing queued); submitted {}: {} all "
        "zero; queued now {}, avg {:.1f}, min {}",
        diagnostic[0], diagnostic[1], diagnostic[2], diagnostic[3], diagnostic[4], queue_average,
        queue_minimum);
  }
}

void SDLAudioDriver::Shutdown() {
  if (dump_)
    std::fclose(dump_);
  if (dump_timestamps_)
    std::fclose(dump_timestamps_);
  dump_ = dump_timestamps_ = nullptr;
  if (sdl_stream_) {
    SDL_DestroyAudioStream(sdl_stream_);
    sdl_stream_ = nullptr;
  }
  if (sdl_initialized_) {
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    sdl_initialized_ = false;
  }
  std::unique_lock<std::mutex> guard(frames_mutex_);
  while (!frames_unused_.empty()) {
    delete[] frames_unused_.top();
    frames_unused_.pop();
  }
  while (!frames_queued_.empty()) {
    delete[] frames_queued_.front();
    frames_queued_.pop();
  }
}

void SDLAudioDriver::SDLCallback(void* userdata, SDL_AudioStream* stream, int additional_amount,
                                 [[maybe_unused]] int total_amount) {
  SCOPE_profile_cpu_f("apu");
  if (!userdata || !stream) {
    REXAPU_ERROR("SDLAudioDriver::SDLCallback called with nullptr.");
    return;
  }
  const auto driver = static_cast<SDLAudioDriver*>(userdata);
  const int sample_count =
      static_cast<int>(channel_samples_ * std::max<uint8_t>(driver->sdl_device_channels_, 1));
  const int len = static_cast<int>(sizeof(float) * sample_count);
  float* data = SDL_stack_alloc(float, sample_count);
  if (!data) {
    REXAPU_ERROR("SDLAudioDriver::SDLCallback failed to allocate {} samples", sample_count);
    return;
  }
  while (additional_amount > 0) {
    float* buffer = nullptr;
    {
      std::unique_lock<std::mutex> guard(driver->frames_mutex_);
      if (!driver->frames_queued_.empty()) {
        buffer = driver->frames_queued_.front();
        driver->frames_queued_.pop();
      } else {
        ++driver->diag_underrun_frames_;
      }
      ++driver->diag_played_frames_;
    }
    // The popped buffer belongs to this callback until it is returned to the
    // pool. Keep conversion, SDL calls and semaphore wakeups outside the
    // queue lock so the submitting thread can fill the next available slot.
    if (!buffer || REXCVAR_GET(audio_mute)) {
      std::memset(data, 0, len);
    } else {
      switch (driver->sdl_device_channels_) {
        case 2:
          conversion::sequential_6_BE_to_interleaved_2_LE(data, buffer, channel_samples_);
          break;
        case 6:
          conversion::sequential_6_BE_to_interleaved_6_LE(data, buffer, channel_samples_);
          break;
        default:
          assert_unhandled_case(driver->sdl_device_channels_);
          break;
      }
    }
    bool submitted = SDL_PutAudioStreamData(stream, data, len);
    if (buffer) {
      {
        std::unique_lock<std::mutex> guard(driver->frames_mutex_);
        driver->frames_unused_.push(buffer);
      }
      // Return a consumed credit even if SDL rejects the write. Otherwise a
      // device error permanently shrinks the queue and can stall the worker.
      auto ret = driver->semaphore_->Release(1, nullptr);
      assert_true(ret);
    }
    if (!submitted) {
      REXAPU_ERROR("SDL_PutAudioStreamData() failed: {}", SDL_GetError());
      break;
    }
    additional_amount -= len;
  }
  SDL_stack_free(data);
}

}  // namespace rex::audio::sdl
