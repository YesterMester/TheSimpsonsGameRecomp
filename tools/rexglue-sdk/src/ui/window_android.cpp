#include <rex/ui/window_android.h>

#include <algorithm>
#include <rex/logging.h>
#include <rex/ui/surface_android.h>
#include <rex/ui/windowed_app_context_android.h>

namespace rex::ui {

std::unique_ptr<Window> Window::Create(WindowedAppContext& app_context,
                                     const std::string_view title, uint32_t width,
                                     uint32_t height) {
  return std::make_unique<AndroidWindow>(app_context, title, width, height);
}

// Android's app actions are in the touch interface rather than a native menu bar.
std::unique_ptr<MenuItem> MenuItem::Create(Type type, const std::string& text,
                                         const std::string& hotkey,
                                         std::function<void()> callback) {
  return std::unique_ptr<MenuItem>(new MenuItem(type, text, hotkey, std::move(callback)));
}

AndroidWindow::~AndroidWindow() {
  EnterDestructor();
  SDL_RemoveEventWatch(LifecycleEventWatch, this);
  static_cast<AndroidWindowedAppContext&>(app_context()).SetWindow(nullptr);
  if (sdl_window_) {
    SDL_DestroyWindow(sdl_window_);
  }
}

bool AndroidWindow::OpenImpl() {
  sdl_window_ = SDL_CreateWindow(GetTitle().c_str(), int(GetDesiredLogicalWidth()),
                                 int(GetDesiredLogicalHeight()),
                                 SDL_WINDOW_VULKAN | SDL_WINDOW_FULLSCREEN |
                                     SDL_WINDOW_HIGH_PIXEL_DENSITY);
  if (!sdl_window_) {
    REXLOG_ERROR("Android: failed to create the game window: {}", SDL_GetError());
    return false;
  }
  static_cast<AndroidWindowedAppContext&>(app_context()).SetWindow(this);
  SDL_AddEventWatch(LifecycleEventWatch, this);
  WindowDestructionReceiver receiver(this);
  UpdateSize(receiver);
  if (receiver.IsWindowDestroyedOrClosed()) {
    return true;
  }
  OnDesiredFullscreenUpdate(true);
  OnFocusUpdate(true, receiver);
  return true;
}

void AndroidWindow::RequestCloseImpl() {
  WindowDestructionReceiver receiver(this);
  OnBeforeClose(receiver);
  if (receiver.IsWindowDestroyed()) {
    return;
  }
  if (sdl_window_) {
    SDL_DestroyWindow(sdl_window_);
    sdl_window_ = nullptr;
  }
  OnAfterClose();
}

void AndroidWindow::ApplyNewTitle() { SDL_SetWindowTitle(sdl_window_, GetTitle().c_str()); }
void AndroidWindow::ApplyNewFullscreen() { OnDesiredFullscreenUpdate(true); }

void AndroidWindow::UpdateSize(WindowDestructionReceiver& receiver) {
  int width = 0, height = 0;
  SDL_GetWindowSizeInPixels(sdl_window_, &width, &height);
  OnActualSizeUpdate(uint32_t(std::max(0, width)), uint32_t(std::max(0, height)), receiver);
}

ANativeWindow* AndroidWindow::CurrentNativeWindow() const {
  if (!sdl_window_) {
    return nullptr;
  }
  return static_cast<ANativeWindow*>(SDL_GetPointerProperty(
      SDL_GetWindowProperties(sdl_window_), SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER, nullptr));
}

std::unique_ptr<Surface> AndroidWindow::CreateSurfaceImpl(Surface::TypeFlags allowed_types) {
  if (!(allowed_types & Surface::kTypeFlag_AndroidNativeWindow) || in_background_) {
    return nullptr;
  }
  ANativeWindow* window = CurrentNativeWindow();
  attached_native_window_ = window;
  return window ? std::make_unique<AndroidNativeWindowSurface>(window) : nullptr;
}

void AndroidWindow::SyncSurface() {
  if (!sdl_window_ || in_background_) {
    return;
  }
  ANativeWindow* current = CurrentNativeWindow();
  if (current == attached_native_window_) {
    return;
  }
  WindowDestructionReceiver receiver(this);
  UpdateSize(receiver);
  if (receiver.IsWindowDestroyedOrClosed()) {
    return;
  }
  attached_native_window_ = nullptr;
  OnSurfaceChanged(current != nullptr);
  RequestPaint();
}

// SDL blocks its event loop as soon as it has queued the background events, so
// they only reach HandleEvent after the app returns - long after Android has
// destroyed the surface under the swapchain. Event watches run while the event
// is queued, on the thread that pumps Android's lifecycle (the UI thread).
// Other threads also push events (the loop's wake-ups), so only lifecycle
// events are handled here.
bool SDLCALL AndroidWindow::LifecycleEventWatch(void* userdata, SDL_Event* event) {
  auto* window = static_cast<AndroidWindow*>(userdata);
  switch (event->type) {
    case SDL_EVENT_WILL_ENTER_BACKGROUND: {
      if (window->in_background_) {
        break;
      }
      WindowDestructionReceiver receiver(window);
      window->OnFocusUpdate(false, receiver);
      if (receiver.IsWindowDestroyedOrClosed()) {
        break;
      }
      // Release the swapchain while its native window is still alive.
      window->in_background_ = true;
      window->attached_native_window_ = nullptr;
      window->OnSurfaceChanged(false);
      break;
    }
    case SDL_EVENT_DID_ENTER_FOREGROUND: {
      if (!window->in_background_) {
        break;
      }
      // SDL resumes only once the new surface exists.
      window->in_background_ = false;
      window->SyncSurface();
      WindowDestructionReceiver receiver(window);
      window->OnFocusUpdate(true, receiver);
      window->RequestPaint();
      break;
    }
    default:
      break;
  }
  return true;
}

void AndroidWindow::RequestPaintImpl() {
  static_cast<AndroidWindowedAppContext&>(app_context()).RequestPaint();
}

void AndroidWindow::HandleEvent(const SDL_Event& event) {
  WindowDestructionReceiver receiver(this);
  switch (event.type) {
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
      RequestClose();
      break;
    case SDL_EVENT_FINGER_DOWN:
    case SDL_EVENT_FINGER_UP:
    case SDL_EVENT_FINGER_MOTION:
    case SDL_EVENT_FINGER_CANCELED: {
      TouchEvent::Action action = TouchEvent::Action::kMove;
      if (event.type == SDL_EVENT_FINGER_DOWN) action = TouchEvent::Action::kDown;
      if (event.type == SDL_EVENT_FINGER_UP) action = TouchEvent::Action::kUp;
      if (event.type == SDL_EVENT_FINGER_CANCELED) action = TouchEvent::Action::kCancel;
      TouchEvent touch(this, uint32_t(event.tfinger.fingerID), action,
                       event.tfinger.x * float(GetActualPhysicalWidth()),
                       event.tfinger.y * float(GetActualPhysicalHeight()));
      OnTouchEvent(touch, receiver);
      break;
    }
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
    case SDL_EVENT_WINDOW_RESIZED:
      UpdateSize(receiver);
      break;
    case SDL_EVENT_WINDOW_EXPOSED:
      OnPaint();
      break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
      OnFocusUpdate(false, receiver);
      break;
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
      OnFocusUpdate(true, receiver);
      break;
    case SDL_EVENT_WINDOW_SHOWN:
      // The lifecycle event watch handles the background and foreground.
      SyncSurface();
      break;
    default:
      break;
  }
}

}  // namespace rex::ui
