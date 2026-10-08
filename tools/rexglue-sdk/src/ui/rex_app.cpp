/**
 * @file        ui/rex_app.cpp
 * @brief       ReXApp implementation — compiled as part of the consumer executable
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/rex_app.h>

#include <rex/cvar.h>
#include <rex/ui/flags.h>
#include <rex/kernel/crt/heap.h>
#include <rex/filesystem.h>
#include <rex/logging/sink.h>
#include <rex/logging.h>
#include <rex/perf/counter.h>
#include <rex/string/utf8.h>
#include <rex/ui/overlay/console_overlay.h>
#include <rex/ui/overlay/controls_overlay.h>
#include <rex/ui/overlay/debug_overlay.h>
#include <rex/ui/overlay/settings_overlay.h>
#include <rex/graphics/flags.h>
#include <rex/graphics/graphics_system.h>
#if REX_HAS_VULKAN
#include <rex/graphics/vulkan/graphics_system.h>
#endif
#if REX_HAS_D3D12
#include <rex/graphics/d3d12/graphics_system.h>
#endif
#if REX_HAS_D3D11
#include <rex/graphics/d3d11/graphics_system.h>
#endif
#include <rex/audio/audio_system.h>
#include <rex/audio/sdl/sdl_audio_system.h>
#include <rex/input/input_system.h>
#include <rex/kernel/init.h>
#include <rex/system.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xthread.h>
#include <rex/ui/graphics_provider.h>
#include <rex/ui/keybinds.h>
#include <rex/version.h>

#include <fmt/format.h>
#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <thread>
#include <string_view>

REXCVAR_DEFINE_INT32(shutdown_watchdog_seconds, 10, "UI",
                     "Seconds after the window closes before the process exits even if "
                     "shutdown is stuck (0 = wait forever)")
    .range(0, 3600);

namespace rex {

namespace {
// Not a ReXApp member: games derive their app class from ReXApp, so its layout
// stays as the game binary was built against.
std::unique_ptr<ui::ControlsOverlayDialog> controls_overlay_;
}  // namespace

// --- ReXApp ---

ReXApp::~ReXApp() = default;

ReXApp::ReXApp(ui::WindowedAppContext& ctx, std::string_view name, PPCImageInfo ppc_info,
               std::string_view usage)
    : WindowedApp(ctx, name, usage), ppc_info_(ppc_info) {}

bool ReXApp::OnInitialize() {
  if (!SetupEnvironment())
    return false;
  if (!SetupPresentation())
    return false;

#ifdef REXGLUE_ENABLE_PERF_COUNTERS
  // HAND PATCH: SetGuestFrameStats was declared and wired into the F3 debug
  // overlay's constructor, but nothing ever called it -- the "Guest: FPS"
  // line was dead, always skipped by the `stats.frame_count > 0` check in
  // debug_overlay.cpp. Feed it from the same fps/frame_time_us counters the
  // perf_log_csv logger already uses (set every swap, see XE_SWAP handling
  // in command_processor.cpp), so the overlay actually shows live numbers.
  SetGuestFrameStats([]() -> ui::FrameStats {
    static uint64_t overlay_frame_counter = 0;
    ++overlay_frame_counter;
    ui::FrameStats stats;
    stats.frame_time_ms =
        double(rex::perf::GetSnapshotCounter(rex::perf::CounterId::kFrameTimeUs)) / 1000.0;
    stats.fps = double(rex::perf::GetSnapshotCounter(rex::perf::CounterId::kFps));
    stats.frame_count = overlay_frame_counter;
    return stats;
  });
#endif

  auto paths = OnFinalizePaths(resolved_defaults_, MakeResumeCallback());
  if (!paths) {
    // Async: consumer will invoke resume when ready. OnInitialize returns
    // true so the event loop keeps pumping (wizard dialogs render).
    return true;
  }

  if (!ConstructRuntime(*paths))
    return false;
  LaunchModule();
  return true;
}

bool ReXApp::SetupEnvironment() {
  auto exe_dir = rex::filesystem::GetExecutableFolder();

  std::filesystem::path game_dir;
  std::string game_data_cvar = REXCVAR_GET(game_data_root);
  if (!game_data_cvar.empty()) {
    game_dir = game_data_cvar;
  }

  // User data: cvar override, or platform user directory
  std::filesystem::path user_dir;
  std::string user_data_cvar = REXCVAR_GET(user_data_root);
  if (!user_data_cvar.empty()) {
    user_dir = user_data_cvar;
  } else {
    user_dir = rex::filesystem::GetUserFolder() / GetName();
    // Saves used to live under a different folder on Windows (Documents).
    // Move them across once so nobody loses progress to the relocation; the
    // shader cache is left behind deliberately since it is disposable and
    // regenerates, and is the bulkiest part by far.
    auto legacy_root = rex::filesystem::GetLegacyUserFolder();
    if (!legacy_root.empty() && !user_dir.empty()) {
      auto legacy_dir = legacy_root / GetName();
      std::error_code ec;
      if (std::filesystem::is_directory(legacy_dir, ec) &&
          !std::filesystem::exists(user_dir, ec)) {
        std::filesystem::remove_all(legacy_dir / "cache", ec);
        std::filesystem::create_directories(user_dir.parent_path(), ec);
        std::filesystem::rename(legacy_dir, user_dir, ec);
        if (ec) {
          // Cross-volume moves fail; fall back to a copy and keep the
          // original rather than risk losing saves.
          ec.clear();
          std::filesystem::copy(legacy_dir, user_dir,
                                std::filesystem::copy_options::recursive, ec);
        }
        migrated_user_data_from_ = ec ? std::filesystem::path() : legacy_dir;
      }
    }
  }

  // Update data: cvar override, or empty (opt-in)
  std::filesystem::path update_dir;
  std::string update_data_cvar = REXCVAR_GET(update_data_root);
  if (!update_data_cvar.empty()) {
    update_dir = update_data_cvar;
  }

  // Cache: cvar override, or user_dir/cache
  std::filesystem::path cache_dir;
  std::string cache_path_cvar = REXCVAR_GET(cache_path);
  if (!cache_path_cvar.empty()) {
    cache_dir = cache_path_cvar;
  } else {
    cache_dir = user_dir / "cache";
  }

  PathConfig path_config{game_dir, user_dir, update_dir, cache_dir,
                         exe_dir / (std::string(GetName()) + ".toml")};
  OnConfigurePaths(path_config);
  game_data_root_ = path_config.game_data_root;
  user_data_root_ = path_config.user_data_root;
  update_data_root_ = path_config.update_data_root;
  cache_root_ = path_config.cache_root;
  config_path_ = path_config.config_path;
  resolved_defaults_ = std::move(path_config);

  // Load config FIRST so log cvars have final values
  if (std::filesystem::exists(config_path_))
    rex::cvar::LoadConfig(config_path_);

  // Late-phase logging
  std::string log_file_cvar = REXCVAR_GET(log_file);
  std::string log_level_str = REXCVAR_GET(log_level);
  if (REXCVAR_GET(log_verbose) && log_level_str == "info")
    log_level_str = "trace";

  auto category_levels = rex::ParseCategoryLevelsFromConfig(config_path_);
  auto log_config = rex::BuildLogConfig(log_file_cvar.empty() ? nullptr : log_file_cvar.c_str(),
                                        log_level_str, category_levels);
  if (log_file_cvar.empty()) {
    log_config.app_name = std::string(GetName());
    log_config.log_dir = (exe_dir / "logs").string();
  }

  rex::InitLogging(log_config);
  rex::RegisterLogLevelCallback();

  log_sink_ = std::make_shared<rex::LogCaptureSink>();
  rex::AddSink(log_sink_);

  OnPostInitLogging();

  if (std::filesystem::exists(config_path_))
    REXLOG_INFO("Loaded config: {}", config_path_.filename().string());

  REXLOG_INFO("{} starting", GetName());
  if (!game_data_root_.empty()) {
    REXLOG_INFO("  Game directory: {}", game_data_root_.string());
  }
  if (!user_data_root_.empty()) {
    REXLOG_INFO("  User data:      {}", user_data_root_.string());
  }
  if (!migrated_user_data_from_.empty()) {
    REXLOG_INFO("  Moved existing save data here from {}", migrated_user_data_from_.string());
  }
  if (!update_data_root_.empty()) {
    REXLOG_INFO("  Update data:    {}", update_data_root_.string());
  }
  REXLOG_INFO("  Cache root:     {}", cache_root_.string());

  return true;
}

bool ReXApp::ConstructRuntime(const PathConfig& paths) {
  if (paths.game_data_root.empty()) {
    auto msg = std::string("--game_data_root was not provided.");
    REXLOG_ERROR("{}", msg);
    rex::ShowSimpleMessageBox(rex::SimpleMessageBoxType::Error, msg);
    return false;
  }
  if (!std::filesystem::is_directory(paths.game_data_root)) {
    auto msg = fmt::format("--game_data_root does not exist: {}", paths.game_data_root.string());
    REXLOG_ERROR("{}", msg);
    rex::ShowSimpleMessageBox(rex::SimpleMessageBoxType::Error, msg);
    return false;
  }

  runtime_ = std::make_unique<rex::Runtime>(paths.game_data_root, paths.user_data_root,
                                            paths.update_data_root, paths.cache_root);
  runtime_->set_app_context(&app_context());

  // Window and ImGui drawer already exist from SetupPresentation; publish them
  // to the runtime before Setup so hooks and native rendering see them.
  if (window_) {
    runtime_->set_display_window(window_.get());
  }
  if (imgui_drawer_) {
    runtime_->set_imgui_drawer(imgui_drawer_.get());
  }

  auto status = runtime_->Setup(ppc_info_, std::move(config_));
  if (XFAILED(status)) {
    REXLOG_ERROR("Runtime setup failed: {:08X}", status);
    return false;
  }

  if (window_ && runtime_->input_system()) {
    static_cast<rex::input::InputSystem*>(runtime_->input_system())->AttachWindow(window_.get());
  }

  if (ppc_info_.register_modules) {
    ppc_info_.register_modules(runtime_->kernel_state());
  }

  if (imgui_drawer_) {
    auto* input_sys = static_cast<rex::input::InputSystem*>(runtime_->input_system());
    if (input_sys) {
      input_sys->SetActiveCallback([this]() {
        if (!debug_overlay_ && !console_overlay_ && !settings_overlay_)
          return true;
        return !imgui_drawer_->GetIO().WantCaptureMouse;
      });
    }
  }

  std::string xex_image = "game:\\default.xex";
  OnLoadXexImage(xex_image);

  // Mirrors the game:\ / d:\ -> game_data_root mapping in Runtime::SetupVfs.
  {
    constexpr std::string_view kGameDevice = "game:\\";
    constexpr std::string_view kDDevice = "d:\\";
    std::string_view tail = xex_image;
    if (tail.starts_with(kGameDevice)) {
      tail.remove_prefix(kGameDevice.size());
    } else if (tail.starts_with(kDDevice)) {
      tail.remove_prefix(kDDevice.size());
    }
    std::string host_tail{tail};
    std::replace(host_tail.begin(), host_tail.end(), '\\', '/');
    auto xex_host = paths.game_data_root / host_tail;
    if (!std::filesystem::is_regular_file(xex_host)) {
      // The guest filesystem matches names case-insensitively (Entry::GetChild
      // uses utf8_equal_case), so an extraction that produced DEFAULT.XEX
      // loads perfectly well - only this pre-check, which hits the host
      // filesystem directly, was case-sensitive on Linux and rejected it.
      // Retry the lookup the way the VFS would before giving up.
      std::error_code ec;
      bool found_case_insensitive = false;
      for (const auto& dir_entry :
           std::filesystem::directory_iterator(xex_host.parent_path(), ec)) {
        if (!dir_entry.is_regular_file(ec)) {
          continue;
        }
        if (rex::string::utf8_equal_case(dir_entry.path().filename().string(),
                                         xex_host.filename().string())) {
          xex_host = dir_entry.path();
          found_case_insensitive = true;
          break;
        }
      }
      if (!found_case_insensitive) {
        // Say what to actually do about it - "not found" alone sent people
        // hunting for a file the installer was supposed to put there.
        bool have_dir = std::filesystem::is_directory(paths.game_data_root, ec);
        auto msg = fmt::format(
            "Game data is missing its executable ({} not found).\n\n"
            "The game data folder is:\n{}\n\n{}",
            xex_host.filename().string(), paths.game_data_root.string(),
            have_dir ? "That folder exists but has no default.xex in it, so the install is "
                       "incomplete. Open the launcher's Install tab and install from your "
                       "Xbox 360 ISO again."
                     : "That folder does not exist. Open the launcher's Install tab and "
                       "install from your Xbox 360 ISO.");
        REXLOG_ERROR("{}", msg);
        rex::ShowSimpleMessageBox(rex::SimpleMessageBoxType::Error, msg);
        return false;
      }
    }
  }

  status = runtime_->LoadXexImage(xex_image);
  if (XFAILED(status)) {
    auto msg = fmt::format("Failed to load XEX ({}): {:08X}", xex_image, status);
    REXLOG_ERROR("{}", msg);
    rex::ShowSimpleMessageBox(rex::SimpleMessageBoxType::Error, msg);
    return false;
  }

  OnPostLoadXexImage();

  if (ppc_info_.rexcrt_heap) {
    if (!rex::kernel::crt::InitHeap(REXCVAR_GET(rexcrt_heap_size_mb), runtime_->memory())) {
      REXLOG_ERROR("Failed to initialize rexcrt heap");
      return false;
    }
  }

  OnPostSetup();

  return true;
}

bool ReXApp::SetupPresentation() {
  // Vulkan is the primary backend everywhere it is built: its fragment
  // shader interlock render path is the one that plays the VP6 FMVs and
  // matches console colors, while D3D12 only has the host render target
  // path, which still loses the video render target's dumps (black intro
  // videos). D3D12 remains available with gpu = "d3d12" and as the
  // automatic fallback when Vulkan cannot initialize.
#if REX_HAS_D3D12 && REX_HAS_VULKAN
  const bool want_d3d12 = REXCVAR_GET(gpu) == "d3d12";
  if (want_d3d12) {
    config_.graphics = REX_GRAPHICS_BACKEND(rex::graphics::d3d12::D3D12GraphicsSystem);
  } else {
    config_.graphics = REX_GRAPHICS_BACKEND(rex::graphics::vulkan::VulkanGraphicsSystem);
  }
#elif REX_HAS_VULKAN
  config_.graphics = REX_GRAPHICS_BACKEND(rex::graphics::vulkan::VulkanGraphicsSystem);
#elif REX_HAS_D3D12
  config_.graphics = REX_GRAPHICS_BACKEND(rex::graphics::d3d12::D3D12GraphicsSystem);
#endif
#if REX_HAS_D3D11
#if !REX_HAS_VULKAN && !REX_HAS_D3D12
  config_.graphics = REX_GRAPHICS_BACKEND(rex::graphics::d3d11::D3D11GraphicsSystem);
#else
  if (REXCVAR_GET(gpu) == "d3d11") {
    config_.graphics = REX_GRAPHICS_BACKEND(rex::graphics::d3d11::D3D11GraphicsSystem);
  }
#endif
#endif
  config_.audio_factory = REX_AUDIO_BACKEND(rex::audio::sdl::SDLAudioSystem);
  config_.input_factory = REX_INPUT_BACKEND(rex::input::CreateDefaultInputSystem);
  config_.kernel_init = rex::kernel::InitializeKernel;

  OnPreSetup(config_);

  if (config_.graphics) {
    X_STATUS status = config_.graphics->SetupPresentation(&app_context());
#if REX_HAS_D3D12 && REX_HAS_VULKAN
    if (XFAILED(status) && !want_d3d12 && REXCVAR_GET(gpu) != "d3d11") {
      REXLOG_WARN(
          "Vulkan presentation setup failed ({:08X}) - falling back to the "
          "D3D12 backend",
          status);
      config_.graphics = REX_GRAPHICS_BACKEND(rex::graphics::d3d12::D3D12GraphicsSystem);
      status = config_.graphics->SetupPresentation(&app_context());
    }
#endif
    if (XFAILED(status)) {
      REXLOG_ERROR("Graphics presentation setup failed: {:08X}", status);
      return false;
    }
  }

  // Create window
  window_ = rex::ui::Window::Create(app_context(), GetName(), 1280, 720);
  if (!window_) {
    REXLOG_ERROR("Failed to create window");
    return false;
  }

  // Set window title with SDK build stamp
  std::string title = std::string(GetName()) + " " + REXGLUE_BUILD_TITLE;
  window_->SetTitle(title);

  window_->AddListener(this);
  window_->AddInputListener(this, 0);

  if (REXCVAR_GET(fullscreen)) {
    window_->SetFullscreen(true);
  }
  window_->Open();

  auto* graphics_system = static_cast<rex::graphics::GraphicsSystem*>(config_.graphics.get());
  if (graphics_system && graphics_system->presenter()) {
    auto* presenter = graphics_system->presenter();
    auto* provider = graphics_system->provider();
    if (provider) {
      immediate_drawer_ = provider->CreateImmediateDrawer();
      if (immediate_drawer_) {
        immediate_drawer_->SetPresenter(presenter);
        imgui_drawer_ = std::make_unique<rex::ui::ImGuiDrawer>(
            window_.get(), 64, [this](ImFontAtlas* atlas) { OnConfigureFonts(atlas); });
        imgui_drawer_->SetPresenterAndImmediateDrawer(presenter, immediate_drawer_.get());
        rex::ui::RegisterBind("bind_debug_overlay", "F3", "Toggle debug overlay", [this] {
          if (debug_overlay_) {
            debug_overlay_.reset();
          } else {
            debug_overlay_ = std::make_unique<ui::DebugOverlayDialog>(imgui_drawer_.get(),
                                                                      frame_stats_provider_);
          }
        });
        rex::ui::RegisterBind("bind_console", "Backtick", "Toggle console overlay", [this] {
          if (console_overlay_) {
            console_overlay_.reset();
          } else {
            console_overlay_ = std::make_unique<ui::ConsoleDialog>(imgui_drawer_.get(), log_sink_);
          }
        });
        rex::ui::RegisterBind("bind_controls", "F1", "Show the keyboard & mouse controls",
                              [this] {
                                if (controls_overlay_ && !controls_overlay_->hint_only()) {
                                  controls_overlay_.reset();
                                } else {
                                  controls_overlay_ = std::make_unique<ui::ControlsOverlayDialog>(
                                      imgui_drawer_.get());
                                }
                              });
        if (REXCVAR_QUERY(bool, mnk_mode)) {
          // Says once, briefly, that F1 shows the controls.
          controls_overlay_ =
              std::make_unique<ui::ControlsOverlayDialog>(imgui_drawer_.get(), true);
        }
        rex::ui::RegisterBind("bind_settings", "F4", "Toggle settings overlay", [this] {
          if (settings_overlay_) {
            settings_overlay_.reset();
          } else {
            settings_overlay_ =
                std::make_unique<ui::SettingsDialog>(imgui_drawer_.get(), config_path_);
          }
        });
        // The frame-trace machinery (TraceWriter, RequestFrameTrace) has been
        // in the graphics system all along with nothing wired to trigger it.
        // One trace of one frame is the complete ground truth for a draw --
        // every PM4 packet, register write and memory read -- which is what
        // the native renderer work is developed and verified against.
        // Home rather than an F-key: compact keyboards bury the F-row behind
        // an Fn chord, but every 75% layout keeps a real Home key.
        rex::ui::RegisterBind("bind_gpu_frame_trace", "Home", "Capture one GPU frame trace",
                              [this] {
          auto* gfx = static_cast<rex::graphics::GraphicsSystem*>(runtime_->graphics_system());
          if (gfx) {
            gfx->RequestFrameTrace();
            REXLOG_INFO("GPU frame trace requested (writes next to trace_gpu_prefix)");
          }
        });

        OnCreateDialogs(imgui_drawer_.get());
      }
    }
    window_->SetPresenter(presenter);
  }

  return true;
}

void ReXApp::LaunchModule() {
  app_context().CallInUIThreadDeferred([this]() {
    OnPreLaunchModule();

    auto main_thread = runtime_->PrepareModuleLaunch();
    if (!main_thread) {
      REXLOG_ERROR("Failed to launch module");
      app_context().QuitFromUIThread();
      return;
    }

    auto* graphics_system =
        static_cast<rex::graphics::GraphicsSystem*>(runtime_->graphics_system());
    if (graphics_system && !runtime_->cache_root().empty()) {
      uint32_t title_id = runtime_->kernel_state()->title_id();
      if (title_id != 0) {
        REXLOG_INFO("Initializing shader storage for title {:08X}...", title_id);
        graphics_system->InitializeShaderStorage(runtime_->cache_root(), title_id, true);
      }
    }

    OnPostLaunchModule(main_thread.get());
    main_thread->Resume();

    module_thread_ = std::thread([this, main_thread = std::move(main_thread)]() mutable {
      main_thread->Wait(0, 0, 0, nullptr);
      OnGuestThreadExit(main_thread.get());
      REXLOG_INFO("Execution complete");
      if (!shutting_down_.load(std::memory_order_acquire)) {
        app_context().CallInUIThread([this]() { app_context().QuitFromUIThread(); });
      }
    });
  });
}

std::function<void(PathConfig)> ReXApp::MakeResumeCallback() {
  return [this](PathConfig paths) {
    if (shutting_down_.load(std::memory_order_acquire))
      return;
    if (!ConstructRuntime(std::move(paths))) {
      app_context().QuitFromUIThread();
      return;
    }
    LaunchModule();
  };
}

void ReXApp::OnKeyDown(ui::KeyEvent& e) {
  rex::ui::ProcessKeyEvent(e);
}

void ReXApp::OnClosing(ui::UIEvent& e) {
  (void)e;
  REXLOG_INFO("Window closing, shutting down...");
  shutting_down_.store(true, std::memory_order_release);
  // Shutdown runs guest callbacks (graphics interrupts, audio) that can wait
  // forever on guest threads the title termination already stopped, leaving an
  // invisible process behind. Nothing is saved at exit (the game saves while
  // playing, the pipeline cache is written first), so if shutdown has not
  // finished in time, end the process.
  int32_t watchdog_seconds = REXCVAR_GET(shutdown_watchdog_seconds);
  if (watchdog_seconds > 0) {
    std::thread([watchdog_seconds]() {
      std::this_thread::sleep_for(std::chrono::seconds(watchdog_seconds));
      REXLOG_WARN("Shutdown did not finish within {} s, exiting (last step logged above)",
                  watchdog_seconds);
      std::_Exit(0);
    }).detach();
  }
  if (runtime_ && runtime_->kernel_state()) {
    runtime_->kernel_state()->TerminateTitle();
  }
  app_context().QuitFromUIThread();
}

void ReXApp::OnDestroy() {
  // Notify subclass before cleanup
  OnShutdown();

  // Unregister overlay keybinds before destroying dialogs
  rex::ui::UnregisterBind("bind_debug_overlay");
  rex::ui::UnregisterBind("bind_console");
  rex::ui::UnregisterBind("bind_settings");
  rex::ui::UnregisterBind("bind_controls");

  // ImGui cleanup (reverse of setup)
  controls_overlay_.reset();
  settings_overlay_.reset();
  console_overlay_.reset();
  debug_overlay_.reset();
  if (imgui_drawer_) {
    imgui_drawer_->SetPresenterAndImmediateDrawer(nullptr, nullptr);
    imgui_drawer_.reset();
  }
  if (immediate_drawer_) {
    immediate_drawer_->SetPresenter(nullptr);
    immediate_drawer_.reset();
  }
  if (runtime_) {
    runtime_->set_display_window(nullptr);
    runtime_->set_imgui_drawer(nullptr);
  }
  // Window/runtime cleanup
  if (window_) {
    window_->SetPresenter(nullptr);
  }
  if (module_thread_.joinable()) {
    module_thread_.join();
  }
  if (window_) {
    window_->RemoveInputListener(this);
    window_->RemoveListener(this);
  }
  window_.reset();
  runtime_.reset();
}

void ReXApp::SetGuestFrameStats(ui::DebugOverlayDialog::FrameStatsProvider provider) {
  frame_stats_provider_ = provider;
  if (debug_overlay_) {
    debug_overlay_->SetStatsProvider(provider);
  }
}

}  // namespace rex
