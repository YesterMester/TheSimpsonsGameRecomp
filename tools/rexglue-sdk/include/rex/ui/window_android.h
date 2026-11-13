#pragma once

#include <android/native_window.h>
#include <rex/ui/window.h>
#include <SDL3/SDL.h>

namespace rex::ui {

class AndroidWindow final : public Window {
 public:
  AndroidWindow(WindowedAppContext& context, std::string_view title, uint32_t width,
                uint32_t height)
      : Window(context, title, width, height) {}
  ~AndroidWindow() override;
  void HandleEvent(const SDL_Event& event);
  // Reattaches the presenter when Android has replaced the native window
  // without an event the loop has seen yet.
  void SyncSurface();
  void Paint() { OnPaint(); }

 protected:
  bool OpenImpl() override;
  void RequestCloseImpl() override;
  void ApplyNewTitle() override;
  void ApplyNewFullscreen() override;
  std::unique_ptr<Surface> CreateSurfaceImpl(Surface::TypeFlags allowed_types) override;
  void RequestPaintImpl() override;

 private:
  // Runs while SDL queues an app lifecycle event, before SDL blocks the loop
  // for as long as the app is in the background.
  static bool SDLCALL LifecycleEventWatch(void* userdata, SDL_Event* event);
  ANativeWindow* CurrentNativeWindow() const;

  SDL_Window* sdl_window_ = nullptr;
  // The native window the presenter's surface was created from, if any.
  ANativeWindow* attached_native_window_ = nullptr;
  bool in_background_ = false;
  void UpdateSize(WindowDestructionReceiver& receiver);
};

}  // namespace rex::ui
