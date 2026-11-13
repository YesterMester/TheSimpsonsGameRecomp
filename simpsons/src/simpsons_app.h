// simpsons - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once


#include <rex/rex_app.h>
#include <rex/runtime.h>
#include "achievements.h"
#include "game_menu.h"
#include "game_prompts.h"
#include "game_canvas.h"
#include "game_thread.h"
#include "host_memory.h"
#include "debug_menu.h"
#include "saves.h"
#include "snapshot.h"
#include "game_ui.h"
#include "overlay.h"
#include "pause_menu.h"
#include "spawner.h"
#include "modkit_game.h"
#include "cutscene_skip.h"

namespace rex::memory {
class Memory;
}

// ink_outlines.cpp: patches the ink outline constants into the loaded image.
void ApplyInkOutlineOptions(rex::memory::Memory* memory);

namespace rex::ui {
class Window;
}

// freecam.cpp: the free camera's key binds and keyboard listener.
void InitFreecam(rex::ui::Window* window);
void ShutdownFreecam(rex::ui::Window* window);

// eye_shading.cpp: patches the characters' eye shading into the loaded image.
void ApplyEyeShadingOptions(rex::memory::Memory* memory);

// physics_step.cpp: sets the Havok step threshold in the loaded image.
void ApplyPhysicsStepOptions(rex::memory::Memory* memory);

class SimpsonsApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<SimpsonsApp>(new SimpsonsApp(ctx, "simpsons",
        PPCImageConfig));
  }

  void OnConfigurePaths(rex::PathConfig& paths) override {
    const std::string quick = rex::cvar::GetFlagByName("quick_launch_level");
    const std::string preview = rex::cvar::GetFlagByName("mods_editor_preview");
    if (!quick.empty() && preview != "true") {
      if (paths.cache_root.empty() && !paths.user_data_root.empty())
        paths.cache_root = paths.user_data_root / "cache";
      paths.user_data_root = paths.user_data_root / "quick-launch";
      std::error_code ec;
      std::filesystem::remove_all(paths.user_data_root, ec);
      std::filesystem::create_directories(paths.user_data_root, ec);
      rex::cvar::SetFlagByName("saves_screen", "false");
    }
  }
  void OnPostLoadXexImage() override {
    ApplyInkOutlineOptions(runtime()->memory());
    ApplyEyeShadingOptions(runtime()->memory());
    ApplyPhysicsStepOptions(runtime()->memory());
  }
  void OnLoadXexImage(std::string& image) override {
    modkit_game::Prepare(runtime());
  }
  void OnPostSetup() override {
    // Before anything takes guest memory for itself.
    host_memory::Init(runtime()->memory());
    modkit_game::InitText(runtime());
    InitFreecam(window());
    InitGamePrompts(runtime());
    InitGameMenu(runtime(), window(), config_path());
    InitGameThread(runtime());
    achievements::Init(runtime());
    InitPauseMenu(runtime());
    spawner::Init(runtime());
    snapshot::Init(runtime());
    saves::Init(runtime());
    canvas::Init(host_memory::Alloc(16), host_memory::Alloc(4096 * 28), 4096,
                 host_memory::AllocPhysical(canvas::kPictureMemory));
    debug_menu::Init(config_path());
    game_ui::Init(runtime(), window());
    InitCutsceneSkip(runtime(), window());
  }
  void OnShutdown() override {
    ShutdownCutsceneSkip();
    spawner::Shutdown();
    modkit_game::Shutdown();
    saves::Shutdown();
    game_ui::Shutdown();
    overlay::Shutdown();
    achievements::Shutdown();
    ShutdownGameMenu();
    ShutdownGamePrompts();
    ShutdownFreecam(window());
  }
  void OnConfigureFonts(ImFontAtlas* atlas) override { overlay::ConfigureFonts(atlas); }
  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    overlay::Init(window(), drawer, immediate_drawer());
  }

  // Override virtual hooks for customization:
  // void OnPostInitLogging() override {}
  // void OnPreSetup(rex::RuntimeConfig& config) override {}
  // void OnLoadXexImage(std::string& xex_image) override {}
  // void OnPostSetup() override {}
  // void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {}
  // void OnShutdown() override {}
  // void OnConfigurePaths(rex::PathConfig& paths) override {}
};
