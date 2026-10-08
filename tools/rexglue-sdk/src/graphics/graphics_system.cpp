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

#include <rex/graphics/graphics_system.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include <rex/cvar.h>
#include <rex/platform.h>
#include <rex/graphics/command_processor.h>
#include <rex/graphics/flags.h>
#include <rex/perf/event_trace.h>
#include <rex/kernel/xboxkrnl/video.h>
#include <rex/logging.h>
#include <rex/stream.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xthread.h>
#include <rex/ui/graphics_provider.h>
#include <rex/ui/window.h>
#include <rex/ui/windowed_app_context.h>

#if REX_PLATFORM_LINUX
#include <sys/prctl.h>
#endif

namespace {
// The vblank schedule: the guest tick count of the last vblank raised and the
// interval, shared by the threads that can raise them (DeliverDueVblanks).
// There is one graphics system.
std::mutex g_vblank_mutex;
std::atomic<uint64_t> g_vblank_last_frame_time{0};
std::atomic<uint64_t> g_vblank_interval_ticks{0};
}  // namespace

REXCVAR_DEFINE_STRING(trace_gpu_prefix, "", "GPU", "GPU trace file prefix");

REXCVAR_DEFINE_BOOL(trace_gpu_stream, false, "GPU", "Enable GPU trace streaming");

// Guest vblank rate used when vsync is off. 0 = follow the guest video mode's
// refresh rate (the launcher's FPS target). The old hardcoded 1000 Hz costs a
// vblank interrupt per guest handler run ~16x per displayed frame and measured
// as the dominant GPU cost on Van Gogh; only set it high deliberately.
REXCVAR_DEFINE_INT32(unlocked_vblank_hz, 0, "GPU",
                     "Guest vblank rate in Hz when vsync is off (0 = follow video mode)");

// Stays "none" until the FXAA path actually works. Defaulting it to "fxaa"
// produced a completely black screen on RADV/Van Gogh: the game ran fine
// (2100+ frames, ~48fps, draws issued normally) but nothing was presented.
// That path had never been exercised before, because it has always shipped
// disabled -- so enabling it by default shipped an untested code path as the
// default experience. Do not flip this again without confirming a real frame
// reaches the screen on the target hardware.
REXCVAR_DEFINE_STRING(swap_post_effect, "none", "GPU", "Swap post effect: none, fxaa, fxaa_extreme")
    .allowed({"none", "fxaa", "fxaa_extreme"})
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(store_shaders, true, "GPU",
                    "Store shaders persistently and load them when loading games to avoid "
                    "runtime spikes and freezes when playing the game not for the first time.");

namespace {

rex::graphics::CommandProcessor::SwapPostEffect ParseSwapPostEffect(
    const std::string& effect_name) {
  std::string lowered = effect_name;
  std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) {
    c = static_cast<unsigned char>(std::tolower(c));
    return c == '-' ? '_' : char(c);
  });
  if (lowered == "fxaa") {
    return rex::graphics::CommandProcessor::SwapPostEffect::kFxaa;
  }
  if (lowered == "fxaa_extreme" || lowered == "extreme") {
    return rex::graphics::CommandProcessor::SwapPostEffect::kFxaaExtreme;
  }
  return rex::graphics::CommandProcessor::SwapPostEffect::kNone;
}
}  // namespace

namespace rex::graphics {

// Nvidia Optimus/AMD PowerXpress support.
// These exports force the process to trigger the discrete GPU in multi-GPU
// systems.
// https://developer.download.nvidia.com/devzone/devcenter/gamegraphics/files/OptimusRenderingPolicies.pdf
// https://stackoverflow.com/questions/17458803/amd-equivalent-to-nvoptimusenablement
#if REX_PLATFORM_WIN32
extern "C" {
__declspec(dllexport) uint32_t NvOptimusEnablement = 0x00000001;
__declspec(dllexport) uint32_t AmdPowerXpressRequestHighPerformance = 1;
}  // extern "C"
#endif  // REX_PLATFORM_WIN32

GraphicsSystem::GraphicsSystem() : vsync_worker_running_(false) {}

GraphicsSystem::~GraphicsSystem() = default;

X_STATUS GraphicsSystem::SetupPresentation(ui::WindowedAppContext* app_context) {
  if (presenter_) {
    return X_STATUS_SUCCESS;
  }

  if (!provider_) {
    CreateProvider(true);
    if (!provider_) {
      REXGPU_ERROR("Unable to create graphics provider");
      return X_STATUS_UNSUCCESSFUL;
    }
    provider_supports_presentation_ = true;
  } else if (!provider_supports_presentation_) {
    // A prior SetupGuestGpu built a headless provider; backends like Vulkan
    // need swapchain support baked in at provider creation time.
    REXGPU_ERROR("SetupPresentation called after headless SetupGuestGpu; call order is reversed");
    return X_STATUS_UNSUCCESSFUL;
  }

  app_context_ = app_context;
  auto loss_cb = [this](bool is_responsible, bool statically_from_ui_thread) {
    OnHostGpuLossFromAnyThread(is_responsible);
  };
  if (app_context_) {
    // Presenter creation must happen on the UI thread.
    app_context_->CallInUIThreadSynchronous(
        [this, loss_cb]() { presenter_ = provider_->CreatePresenter(loss_cb); });
  } else {
    // Offscreen path (e.g. capturing guest output without a window).
    presenter_ = provider_->CreatePresenter(loss_cb);
  }

  if (!presenter_) {
    REXGPU_ERROR("Unable to create presenter");
    return X_STATUS_UNSUCCESSFUL;
  }
  return X_STATUS_SUCCESS;
}

X_STATUS GraphicsSystem::SetupGuestGpu(runtime::FunctionDispatcher* function_dispatcher,
                                       system::KernelState* kernel_state) {
  memory_ = function_dispatcher->memory();
  function_dispatcher_ = function_dispatcher;
  kernel_state_ = kernel_state;

  // Headless path: no one set up presentation, so build a no-presentation
  // provider just for the command processor.
  if (!provider_) {
    CreateProvider(false);
    provider_supports_presentation_ = false;
  }

  // Create command processor. This will spin up a thread to process all
  // incoming ringbuffer packets.
  command_processor_ = CreateCommandProcessor();
  if (!command_processor_->Initialize()) {
    REXGPU_ERROR("Unable to initialize command processor");
    return X_STATUS_UNSUCCESSFUL;
  }
  command_processor_->SetDesiredSwapPostEffect(ParseSwapPostEffect(REXCVAR_GET(swap_post_effect)));

  // Register GPU MMIO handlers
  // GPU registers are at 0x7FC80000-0x7FCFFFFF
  memory_->AddVirtualMappedRange(0x7FC80000,  // base address
                                 0xFFFF0000,  // mask
                                 0x0000FFFF,  // size (64KB)
                                 this,        // context (GraphicsSystem*)
                                 reinterpret_cast<runtime::MMIOReadCallback>(ReadRegisterThunk),
                                 reinterpret_cast<runtime::MMIOWriteCallback>(WriteRegisterThunk));

  // Guest vblank timer based on the configured guest video mode.
  vsync_worker_running_ = true;
  vsync_worker_thread_ = system::object_ref<system::XHostThread>(
      new system::XHostThread(kernel_state_, 128 * 1024, 0, [this]() {
        system::X_VIDEO_MODE video_mode;
        kernel::xboxkrnl::VdQueryVideoMode(&video_mode);
        double refresh_rate_hz = std::max(1.0, double(float(video_mode.refresh_rate)));
        uint64_t guest_tick_frequency = chrono::Clock::guest_tick_frequency();
        uint64_t vsync_interval_ticks =
            std::max(uint64_t(1), uint64_t(double(guest_tick_frequency) / refresh_rate_hz));
        // Unlocked (vsync off) guest vblank rate. This used to be hardcoded to
        // 1000 Hz, which "unlocks" the frame rate by telling the guest a vblank
        // is always available - but it costs a vblank interrupt and its guest
        // handler roughly 16x per displayed frame, and measured on Van Gogh it
        // pinned the GPU at 99-100% busy even on a menu drawing ~25 draws and
        // 31 vertices. Dropping to a sane rate freed about half the GPU.
        // Default 0 follows the guest video mode's own refresh rate, which is
        // what the launcher's FPS target setting drives, so 60/90/120 there now
        // actually mean something. Set it explicitly to restore a fixed rate
        // (1000 reproduces the historical behavior).
        double unlocked_hz = double(REXCVAR_GET(unlocked_vblank_hz));
        if (unlocked_hz <= 0.0) {
          unlocked_hz = refresh_rate_hz;
        }
        uint64_t no_vsync_interval_ticks =
            std::max(uint64_t(1), uint64_t(double(guest_tick_frequency) / unlocked_hz));
        {
          std::lock_guard<std::mutex> vblank_lock(g_vblank_mutex);
          g_vblank_last_frame_time.store(chrono::Clock::QueryGuestTickCount(),
                                         std::memory_order_relaxed);
        }
        // File-based frame trace trigger, checked about once a second. A
        // keybind can be swallowed by whatever sits between the compositor
        // and the game (and compact keyboards hide the F-row), but touching
        // a file next to the trace prefix works from any terminal or from
        // the launcher UI: drop <trace_gpu_prefix>.request and the next
        // frame is traced.
        auto next_trace_trigger_check = std::chrono::steady_clock::now();
#if REX_PLATFORM_LINUX
        // Wake up for vblanks within microseconds rather than the default
        // 50 us timer slack.
        prctl(PR_SET_TIMERSLACK, 1000UL, 0, 0, 0);
#endif
        while (vsync_worker_running_) {
          uint64_t interval_ticks =
              REXCVAR_GET(vsync) ? vsync_interval_ticks : no_vsync_interval_ticks;
          g_vblank_interval_ticks.store(interval_ticks, std::memory_order_relaxed);
          DeliverDueVblanks();
          uint64_t last_frame_time = g_vblank_last_frame_time.load(std::memory_order_relaxed);
          auto checks_start = std::chrono::steady_clock::now();
          if (checks_start >= next_trace_trigger_check) {
            next_trace_trigger_check = checks_start + std::chrono::seconds(1);
            rex::perf::FlushEventTrace();
            const std::string& trace_prefix = REXCVAR_GET(trace_gpu_prefix);
            if (!trace_prefix.empty()) {
              std::filesystem::path request_path(trace_prefix + ".request");
              std::error_code trace_request_ec;
              if (std::filesystem::exists(request_path, trace_request_ec)) {
                std::filesystem::remove(request_path, trace_request_ec);
                RequestFrameTrace();
                REXLOG_INFO("Frame trace triggered by {}", request_path.string());
              }
              // Same mechanism for a screenshot of the presented guest output:
              // <trace_gpu_prefix>.shot writes <trace_gpu_prefix>_shot_N.ppm.
              std::filesystem::path shot_request_path(trace_prefix + ".shot");
              if (std::filesystem::exists(shot_request_path, trace_request_ec)) {
                std::filesystem::remove(shot_request_path, trace_request_ec);
                // On its own thread: capturing and writing the screenshot
                // takes about 50 ms, which held back vblanks - and with them
                // the game's swaps - every time the test harness took one.
                // Only requested while the game runs, never at shutdown.
                std::thread([this, trace_prefix]() {
                  SaveGuestOutputScreenshot(trace_prefix);
                }).detach();
              }
            }
          }
          {
            // Debugging: REX_VBLANK_STALL_LOG=1 logs vblank thread sleeps
            // that took much longer than asked, with how long the thread
            // waited for a CPU meanwhile (from /proc schedstat).
            static const bool stall_log = std::getenv("REX_VBLANK_STALL_LOG") != nullptr;
            auto read_run_delay_ns = []() -> uint64_t {
              uint64_t on_cpu = 0, run_delay = 0;
              FILE* f = std::fopen("/proc/thread-self/schedstat", "r");
              if (f) {
                if (std::fscanf(f, "%llu %llu", (unsigned long long*)&on_cpu,
                                (unsigned long long*)&run_delay) != 2) {
                  run_delay = 0;
                }
                std::fclose(f);
              }
              return run_delay;
            };
            if (stall_log) {
              auto checks_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                   std::chrono::steady_clock::now() - checks_start)
                                   .count();
              if (checks_us > 8000) {
                REXGPU_WARN("[vblank-stall] trace checks took {:.2f} ms",
                            double(checks_us) / 1000.0);
              }
            }
            uint64_t delay_before = stall_log ? read_run_delay_ns() : 0;
            auto sleep_start = std::chrono::steady_clock::now();
            // Sleep until the next vblank is due, at most 1 ms at a time so
            // vsync and rate changes are picked up soon. Sleeping whole
            // milliseconds made vblanks up to about 1.1 ms late.
            uint64_t now_ticks = chrono::Clock::QueryGuestTickCount();
            uint64_t due_ticks = last_frame_time + interval_ticks;
            uint64_t remaining_us = due_ticks > now_ticks
                                        ? (due_ticks - now_ticks) * 1000000 /
                                              std::max(guest_tick_frequency, uint64_t(1))
                                        : 0;
            if (remaining_us >= 20) {
              rex::thread::Sleep(std::chrono::microseconds(std::min<uint64_t>(remaining_us, 1000)));
            } else {
              rex::thread::MaybeYield();
            }
            if (stall_log) {
              auto slept_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                  std::chrono::steady_clock::now() - sleep_start)
                                  .count();
              if (slept_us > 8000) {
                uint64_t delay_after = read_run_delay_ns();
                REXGPU_WARN(
                    "[vblank-stall] 1 ms sleep took {:.2f} ms, {:.2f} ms of it waiting "
                    "for a CPU",
                    double(slept_us) / 1000.0, double(delay_after - delay_before) / 1e6);
              }
            }
          }
        }
        return 0;
      }));
  // TODO: set_can_debugger_suspend not yet ported
  // vsync_worker_thread_->set_can_debugger_suspend(true);
  vsync_worker_thread_->set_name("GPU VSync");
  vsync_worker_thread_->Create();

  if (REXCVAR_GET(trace_gpu_stream)) {
    BeginTracing();
  }

  return X_STATUS_SUCCESS;
}

void GraphicsSystem::Shutdown() {
  if (command_processor_) {
    EndTracing();
    command_processor_->Shutdown();
    command_processor_.reset();
  }

  if (vsync_worker_thread_) {
    REXGPU_INFO("Shutdown: vsync thread");
    vsync_worker_running_ = false;
    vsync_worker_thread_->Wait(0, 0, 0, nullptr);
    vsync_worker_thread_.reset();
  }

  if (presenter_) {
    REXGPU_INFO("Shutdown: presenter");
    if (app_context_) {
      app_context_->CallInUIThreadSynchronous([this]() { presenter_.reset(); });
    }
    // If there's no app context (thus the presenter is owned by the thread that
    // initialized the GraphicsSystem) or can't be queueing UI thread calls
    // anymore, shutdown anyway.
    presenter_.reset();
  }

  provider_.reset();
}

void GraphicsSystem::OnHostGpuLossFromAnyThread([[maybe_unused]] bool is_responsible) {
  // TODO(Triang3l): Somehow gain exclusive ownership of the Provider (may be
  // used by the command processor, the presenter, and possibly anything else,
  // it's considered free-threaded, except for lifetime management which will be
  // involved in this case) and reset it so a new host GPU API device is
  // created. Then ask the command processor to reset itself in its thread, and
  // ask the UI thread to reset the Presenter (the UI thread manages its
  // lifetime - but if there's no WindowedAppContext, either don't reset it as
  // in this case there's no user who needs uninterrupted gameplay, or somehow
  // protect it with a mutex so any thread can be considered a UI thread and
  // reset).
  if (host_gpu_loss_reported_.test_and_set(std::memory_order_relaxed)) {
    return;
  }
  rex::FatalError("Graphics device lost (probably due to an internal error)");
}

uint32_t GraphicsSystem::ReadRegisterThunk(void* ppc_context, GraphicsSystem* gs, uint32_t addr) {
  return gs->ReadRegister(addr);
}

void GraphicsSystem::WriteRegisterThunk(void* ppc_context, GraphicsSystem* gs, uint32_t addr,
                                        uint32_t value) {
  gs->WriteRegister(addr, value);
}

uint32_t GraphicsSystem::ReadRegister(uint32_t addr) {
  uint32_t r = (addr & 0xFFFF) / 4;

  switch (r) {
    case 0x0F00:  // RB_EDRAM_TIMING
      return 0x08100748;
    case 0x0F01:  // RB_BC_CONTROL
      return 0x0000200E;
    case 0x194C: {  // R500_D1MODE_V_COUNTER
      system::X_VIDEO_MODE video_mode;
      kernel::xboxkrnl::VdQueryVideoMode(&video_mode);
      return std::min(uint32_t(video_mode.display_height), uint32_t(0x0FFF));
    }
    case 0x1951:    // interrupt status
      return 1;     // vblank
    case 0x1961: {  // AVIVO_D1MODE_VIEWPORT_SIZE
      // Maximum [width(0x0FFF), height(0x0FFF)].
      system::X_VIDEO_MODE video_mode;
      kernel::xboxkrnl::VdQueryVideoMode(&video_mode);
      uint32_t viewport_width = std::min(uint32_t(video_mode.display_width), uint32_t(0x0FFF));
      uint32_t viewport_height = std::min(uint32_t(video_mode.display_height), uint32_t(0x0FFF));
      return (viewport_width << 16) | viewport_height;
    }
    default:
      if (!register_file_.GetRegisterInfo(r)) {
        REXGPU_DEBUG("GPU: Read from unknown register ({:04X})", r);
      }
  }

  assert_true(r < RegisterFile::kRegisterCount);
  return register_file_.values[r];
}

void GraphicsSystem::WriteRegister(uint32_t addr, uint32_t value) {
  uint32_t r = (addr & 0xFFFF) / 4;

  switch (r) {
    case 0x01C5:  // CP_RB_WPTR
      command_processor_->UpdateWritePointer(value);
      break;
    case 0x1844:  // AVIVO_D1GRPH_PRIMARY_SURFACE_ADDRESS
      break;
    default:
      REXGPU_WARN("Unknown GPU register {:04X} write: {:08X}", r, value);
      break;
  }

  assert_true(r < RegisterFile::kRegisterCount);
  register_file_.values[r] = value;
}

void GraphicsSystem::InitializeRingBuffer(uint32_t ptr, uint32_t size_log2) {
  command_processor_->InitializeRingBuffer(ptr, size_log2);
}

void GraphicsSystem::EnableReadPointerWriteBack(uint32_t ptr, uint32_t block_size_log2) {
  command_processor_->EnableReadPointerWriteBack(ptr, block_size_log2);
}

void GraphicsSystem::SetInterruptCallback(uint32_t callback, uint32_t user_data) {
  interrupt_callback_ = callback;
  interrupt_callback_data_ = user_data;
  REXGPU_INFO("SetInterruptCallback({:08X}, {:08X})", callback, user_data);
}

void GraphicsSystem::DispatchInterruptCallback(uint32_t source, uint32_t cpu) {
  if (!interrupt_callback_) {
    return;
  }

  auto thread = system::XThread::GetCurrentThread();
  assert_not_null(thread);

  // Pick a CPU, if needed. We're going to guess 2. Because.
  if (cpu == 0xFFFFFFFF) {
    cpu = 2;
  }
  thread->SetActiveCpu(cpu);

  // REXGPU_INFO("Dispatching GPU interrupt at {:08X} w/ mode {} on cpu {}",
  //          interrupt_callback_, source, cpu);

  // Debugging: REX_SWAP_QUEUE_LOG=1 logs the XDK swap callback's argument
  // (front buffer | present interval << 8 | immediate threshold percent) on
  // command buffer interrupts.
  static const bool swap_queue_log = std::getenv("REX_SWAP_QUEUE_LOG") != nullptr;
  if (swap_queue_log && source == 1 && interrupt_callback_data_) {
    static uint32_t swap_queue_log_count = 0;
    if (swap_queue_log_count++ % 600 < 3) {
      auto load = [this](uint32_t address) -> uint32_t {
        return rex::memory::load_and_swap<uint32_t>(memory_->TranslateVirtual(address));
      };
      uint32_t swap_state = load(interrupt_callback_data_ + 10900);
      if (swap_state) {
        REXGPU_WARN(
            "[swap-queue] state {:08X}: word0 {:08X} word1 {:08X} callback {:08X} arg "
            "{:08X} | vblanks {} last target {} swaps {} presented {}",
            swap_state, load(swap_state), load(swap_state + 4), load(swap_state + 16),
            load(swap_state + 20), load(interrupt_callback_data_ + 16524),
            load(interrupt_callback_data_ + 16532), load(interrupt_callback_data_ + 16540),
            load(interrupt_callback_data_ + 16544));
      }
    }
  }
  uint64_t args[] = {source, interrupt_callback_data_};
  function_dispatcher_->ExecuteInterrupt(thread->thread_state(), interrupt_callback_, args,
                                         rex::countof(args));
}

void GraphicsSystem::DeliverDueVblanks() {
  std::unique_lock<std::mutex> lock(g_vblank_mutex, std::try_to_lock);
  if (!lock.owns_lock()) {
    return;
  }
  uint64_t interval_ticks = g_vblank_interval_ticks.load(std::memory_order_relaxed);
  if (!interval_ticks) {
    return;
  }
  uint64_t now = chrono::Clock::QueryGuestTickCount();
  uint64_t last_frame_time = g_vblank_last_frame_time.load(std::memory_order_relaxed);
  while (now - last_frame_time >= interval_ticks) {
    MarkVblank();
    last_frame_time += interval_ticks;
    g_vblank_last_frame_time.store(last_frame_time, std::memory_order_relaxed);
  }
}

void GraphicsSystem::MarkVblank() {
  // TODO: Enable profiling once ported
  // SCOPE_profile_cpu_f("gpu");

  // Increment vblank counter (so the game sees us making progress).
  if (command_processor_) {
    command_processor_->increment_counter();
  }

  // TODO(benvanik): we shouldn't need to do the dispatch here, but there's
  //     something wrong and the CP will block waiting for code that
  //     needs to be run in the interrupt.
  rex::perf::TraceEvent("vblank");
  DispatchInterruptCallback(0, 2);
  rex::perf::TraceEvent("vblank_done");
}

void GraphicsSystem::ClearCaches() {
  command_processor_->CallInThread([&]() { command_processor_->ClearCaches(); });
}

void GraphicsSystem::InvalidateGpuMemory() {
  command_processor_->CallInThread([&]() { command_processor_->InvalidateGpuMemory(); });
}

void GraphicsSystem::InitializeShaderStorage(const std::filesystem::path& cache_root,
                                             uint32_t title_id, bool blocking) {
  if (!REXCVAR_GET(store_shaders)) {
    return;
  }
  if (blocking) {
    if (command_processor_->is_paused()) {
      // Safe to run on any thread while the command processor is paused, no
      // race condition.
      command_processor_->InitializeShaderStorage(cache_root, title_id, true);
    } else {
      rex::thread::Fence fence;
      command_processor_->CallInThread([this, cache_root, title_id, &fence]() {
        command_processor_->InitializeShaderStorage(cache_root, title_id, true);
        fence.Signal();
      });
      fence.Wait();
    }
  } else {
    command_processor_->CallInThread([this, cache_root, title_id]() {
      command_processor_->InitializeShaderStorage(cache_root, title_id, false);
    });
  }
}

void GraphicsSystem::SaveGuestOutputScreenshot(const std::string& prefix) {
  ui::RawImage image;
  if (!presenter_ || !presenter_->CaptureGuestOutput(image)) {
    REXGPU_WARN("Screenshot requested but no guest output is available yet");
    return;
  }
  static std::atomic<uint32_t> screenshot_index{0};
  std::string path = fmt::format("{}_shot_{}.ppm", prefix, screenshot_index++);
  FILE* file = std::fopen(path.c_str(), "wb");
  if (!file) {
    REXGPU_WARN("Unable to open {} for the screenshot", path);
    return;
  }
  std::fprintf(file, "P6\n%u %u\n255\n", image.width, image.height);
  std::vector<uint8_t> row(size_t(image.width) * 3);
  for (uint32_t y = 0; y < image.height; ++y) {
    const uint8_t* source = image.data.data() + image.stride * y;
    for (uint32_t x = 0; x < image.width; ++x) {
      row[x * 3 + 0] = source[x * 4 + 0];
      row[x * 3 + 1] = source[x * 4 + 1];
      row[x * 3 + 2] = source[x * 4 + 2];
    }
    std::fwrite(row.data(), 1, row.size(), file);
  }
  std::fclose(file);
  REXLOG_INFO("Screenshot saved to {} ({}x{})", path, image.width, image.height);
}

void GraphicsSystem::RequestFrameTrace() {
  command_processor_->RequestFrameTrace(REXCVAR_GET(trace_gpu_prefix));
}

void GraphicsSystem::BeginTracing() {
  command_processor_->BeginTracing(REXCVAR_GET(trace_gpu_prefix));
}

void GraphicsSystem::EndTracing() {
  command_processor_->EndTracing();
}

void GraphicsSystem::Pause() {
  paused_ = true;
  command_processor_->Pause();
}

void GraphicsSystem::Resume() {
  paused_ = false;
  command_processor_->Resume();
}

bool GraphicsSystem::Save(::rex::stream::ByteStream* stream) {
  stream->Write<uint32_t>(interrupt_callback_);
  stream->Write<uint32_t>(interrupt_callback_data_);
  return command_processor_->Save(stream);
}

bool GraphicsSystem::Restore(::rex::stream::ByteStream* stream) {
  interrupt_callback_ = stream->Read<uint32_t>();
  interrupt_callback_data_ = stream->Read<uint32_t>();
  return command_processor_->Restore(stream);
}

}  // namespace rex::graphics

extern "C" bool rex_graphics_get_vblank_schedule(uint64_t* last_vblank_ticks,
                                                 uint64_t* interval_ticks) {
  uint64_t interval = g_vblank_interval_ticks.load(std::memory_order_relaxed);
  if (!interval) {
    return false;
  }
  *last_vblank_ticks = g_vblank_last_frame_time.load(std::memory_order_relaxed);
  *interval_ticks = interval;
  return true;
}
