// simpsons - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <rex/rex_app.h>
#include <rex/runtime.h>

namespace rex::memory {
class Memory;
}

// ink_outlines.cpp: patches the ink outline constants into the loaded image.
void ApplyInkOutlineOptions(rex::memory::Memory* memory);

// eye_shading.cpp: patches the characters' eye shading into the loaded image.
void ApplyEyeShadingOptions(rex::memory::Memory* memory);

class SimpsonsApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<SimpsonsApp>(new SimpsonsApp(ctx, "simpsons",
        PPCImageConfig));
  }

  void OnPostLoadXexImage() override {
    ApplyInkOutlineOptions(runtime()->memory());
    ApplyEyeShadingOptions(runtime()->memory());
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
