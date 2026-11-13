#pragma once

#include <rex/ui/windowed_app_context.h>
#include <SDL3/SDL_events.h>

namespace rex::ui {

class AndroidWindow;

class AndroidWindowedAppContext final : public WindowedAppContext {
 public:
  AndroidWindowedAppContext();
  void RunMainLoop();
  void SetWindow(AndroidWindow* window) { window_ = window; }
  void RequestPaint();

 protected:
  void NotifyUILoopOfPendingFunctions() override;
  void PlatformQuitFromUIThread() override;

 private:
  AndroidWindow* window_ = nullptr;
  uint32_t wake_event_;
};

}  // namespace rex::ui
