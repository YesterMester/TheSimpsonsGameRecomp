#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <algorithm>
#include <cstdlib>
#include <map>
#include <memory>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/thread.h>
#include <rex/ui/windowed_app.h>
#include <rex/ui/windowed_app_context_android.h>
#include <unistd.h>

extern "C" int SDL_main(int argc, char** argv) {
  // Guest allocations and GPU write watches still require independently
  // protected 4 KiB pages. Rounding them to a larger host page would change
  // neighboring allocations and the physical E-heap alias.
  const size_t host_page_size = rex::memory::page_size();
  if (host_page_size != 4096) {
    SDL_Log("This Android development build requires 4 KiB memory pages; "
            "the device uses %zu-byte pages. Guest memory protection and "
            "physical aliases are not supported on this page size yet.",
            host_page_size);
    return EXIT_FAILURE;
  }
  if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
    SDL_Log("Failed to initialize Android video: %s", SDL_GetError());
    return EXIT_FAILURE;
  }
  const char* storage = SDL_GetAndroidInternalStoragePath();
  if (!storage || chdir(storage) != 0) {
    SDL_Log("Failed to open the application's data directory");
    SDL_Quit();
    return EXIT_FAILURE;
  }
  setenv("REX_ANDROID_DATA_DIR", storage, 1);
  setenv("XDG_DATA_HOME", storage, 1);
  rex::thread::AndroidInitialize();
  rex::memory::AndroidInitialize();
  auto remaining = rex::cvar::Init(argc, argv);
  rex::cvar::ApplyEnvironment();
  rex::InitLoggingEarly();
  int result = EXIT_FAILURE;
  auto creator = rex::ui::WindowedApp::GetSingleCreator();
  if (!creator) {
    SDL_Log("Android entry point requires exactly one registered application");
  } else {
    rex::ui::AndroidWindowedAppContext context;
    auto app = creator(context);
    std::map<std::string, std::string> parsed;
    const auto& options = app->GetPositionalOptions();
    for (size_t i = 0; i < std::min(remaining.size(), options.size()); ++i) {
      parsed[options[i]] = remaining[i];
    }
    app->SetParsedArguments(std::move(parsed));
    if (app->OnInitialize()) {
      context.RunMainLoop();
      result = EXIT_SUCCESS;
    }
    app->InvokeOnDestroy();
  }
  rex::ShutdownLogging();
  rex::memory::AndroidShutdown();
  rex::thread::AndroidShutdown();
  SDL_Quit();
  return result;
}
