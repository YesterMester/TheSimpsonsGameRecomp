/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <link.h>
#include <sys/mman.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/windowed_app.h>
#include <rex/ui/windowed_app_context_gtk.h>

#include <gtk/gtk.h>

REXCVAR_DEFINE_BOOL(code_huge_pages, true, "Memory",
                    "Map the game's code with 2 MB pages instead of 4 KB ones")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace {

// The recompiled game is tens of megabytes of code run with a flat profile, so
// with 4 KB pages its threads miss the instruction TLB constantly. Linux maps
// an executable file with 2 MB pages (CONFIG_READ_ONLY_THP_FOR_FS) where its
// code segment is 2 MB aligned both in memory and in the file, which the host
// and the runtime are linked for (-z max-page-size). khugepaged gets to it
// eventually; MADV_COLLAPSE does it now.
void MapCodeWithHugePages() {
#ifndef MADV_COLLAPSE
  constexpr int MADV_COLLAPSE = 25;
#endif
  constexpr uintptr_t kHugePage = uintptr_t(1) << 21;
  struct Totals {
    size_t eligible = 0;
    size_t mapped = 0;
    int error = 0;
  } totals;
  dl_iterate_phdr(
      [](dl_phdr_info* info, size_t, void* data) {
        // The host executable (unnamed) and the runtime hold the code.
        if (info->dlpi_name[0] != '\0' && !std::strstr(info->dlpi_name, "librexruntime")) {
          return 0;
        }
        auto& totals = *static_cast<Totals*>(data);
        for (ElfW(Half) i = 0; i < info->dlpi_phnum; ++i) {
          const ElfW(Phdr)& phdr = info->dlpi_phdr[i];
          if (phdr.p_type != PT_LOAD || !(phdr.p_flags & PF_X)) {
            continue;
          }
          uintptr_t begin = info->dlpi_addr + phdr.p_vaddr;
          uintptr_t start = (begin + kHugePage - 1) & ~(kHugePage - 1);
          uintptr_t end = (begin + phdr.p_memsz) & ~(kHugePage - 1);
          if (end <= start) {
            continue;
          }
          totals.eligible += end - start;
          void* address = reinterpret_cast<void*>(start);
          madvise(address, end - start, MADV_HUGEPAGE);
          if (madvise(address, end - start, MADV_COLLAPSE) == 0) {
            totals.mapped += end - start;
          } else {
            totals.error = errno;
          }
        }
        return 0;
      },
      &totals);
  if (totals.error) {
    REXLOG_INFO("Code huge pages: {} of {} MB mapped ({})", totals.mapped >> 20,
                totals.eligible >> 20, std::strerror(totals.error));
  } else {
    REXLOG_INFO("Code huge pages: {} MB mapped", totals.mapped >> 20);
  }
}

}  // namespace

extern "C" int main(int argc_pre_gtk, char** argv_pre_gtk) {
  // Before touching anything GTK+, make sure that when running on Wayland,
  // we'll still get an X11 (Xwayland) window
  setenv("GDK_BACKEND", "x11", 1);

  // Initialize GTK+, which will handle and remove its own arguments from argv.
  // Both GTK+ and Xenia use --option=value argument format (see man
  // gtk-options), however, it's meaningless to try to parse the same argument
  // both as a GTK+ one and as a cvar. Make GTK+ options take precedence in case
  // of a name collision, as there's an alternative way of setting Xenia options
  // (the config).
  int argc_post_gtk = argc_pre_gtk;
  char** argv_post_gtk = argv_pre_gtk;
  if (!gtk_init_check(&argc_post_gtk, &argv_post_gtk)) {
    // Logging has not been initialized yet.
    std::fputs("Failed to initialize GTK+\n", stderr);
    return EXIT_FAILURE;
  }

  auto remaining = rex::cvar::Init(argc_post_gtk, argv_post_gtk);
  rex::cvar::ApplyEnvironment();
  rex::InitLoggingEarly();
  if (REXCVAR_GET(code_huge_pages)) {
    std::thread(MapCodeWithHugePages).detach();
  }

  int result;

  {
    rex::ui::GTKWindowedAppContext app_context;

    std::unique_ptr<rex::ui::WindowedApp> app = rex::ui::GetWindowedAppCreator()(app_context);

    // Match remaining positional args to app's expected options
    const auto& option_names = app->GetPositionalOptions();
    std::map<std::string, std::string> parsed;
    size_t count = std::min(remaining.size(), option_names.size());
    for (size_t i = 0; i < count; ++i) {
      parsed[option_names[i]] = remaining[i];
    }
    app->SetParsedArguments(std::move(parsed));

    if (app->OnInitialize()) {
      app_context.RunMainGTKLoop();
      result = EXIT_SUCCESS;
    } else {
      result = EXIT_FAILURE;
    }

    app->InvokeOnDestroy();
  }

  // Logging may still be needed in the destructors.
  rex::ShutdownLogging();

  return result;
}
