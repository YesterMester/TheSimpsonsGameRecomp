#include <rex/ui/windowed_app_context_android.h>

#include <rex/ui/window_android.h>

namespace rex::ui {

AndroidWindowedAppContext::AndroidWindowedAppContext()
    : wake_event_(SDL_RegisterEvents(1)) {}

void AndroidWindowedAppContext::NotifyUILoopOfPendingFunctions() {
  SDL_Event event{};
  event.type = wake_event_;
  SDL_PushEvent(&event);
}

void AndroidWindowedAppContext::PlatformQuitFromUIThread() {
  NotifyUILoopOfPendingFunctions();
}

void AndroidWindowedAppContext::RequestPaint() { NotifyUILoopOfPendingFunctions(); }

void AndroidWindowedAppContext::RunMainLoop() {
  while (!HasQuitFromUIThread()) {
    SDL_Event event;
    // Wake up now and then even without events, so a native window Android
    // replaced without telling SDL's event queue is noticed (SyncSurface).
    if (SDL_WaitEventTimeout(&event, 250)) {
      do {
        if (event.type == SDL_EVENT_QUIT) {
          if (window_) {
            // Run the normal title termination before OnDestroy joins the
            // guest thread. Quitting only the event loop would hang on Back.
            window_->RequestClose();
          }
          QuitFromUIThread();
          break;
        }
        if (window_) {
          window_->HandleEvent(event);
        }
      } while (!HasQuitFromUIThread() && SDL_PollEvent(&event));
    }
    ExecutePendingFunctionsFromUIThread();
    if (window_ && !HasQuitFromUIThread()) {
      window_->SyncSurface();
      window_->Paint();
    }
  }
  QuitFromUIThread();
}

}  // namespace rex::ui
