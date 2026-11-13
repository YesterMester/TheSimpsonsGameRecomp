#pragma once

#include "runtime.h"

#include <exception>

namespace simpsons::modkit {

struct PreparedSources {
  std::vector<Manifest> original, mods;
  bool reload = false;
  bool checkpoint_prepared = false;
  std::optional<Json> checkpoint_state{};
  void Validate(const std::vector<Manifest>& current, bool for_reload) const;
};

PreparedSources PrepareSources(const std::vector<Manifest>& original, const fs::path& cache,
                               bool reload, std::stop_token stop = {});

// One bounded preparation job. The worker verifies code and optionally captures
// checkpoint JSON; callbacks and DLL entrypoints initialize on the game thread
// after Take(). A missing sidecar explicitly selects fresh mod initialization.
class SourceJob {
 public:
  SourceJob(std::vector<Manifest> original, fs::path cache, bool reload,
            fs::path checkpoint_state = {});
  ~SourceJob();
  void Cancel();
  bool ready() const;
  Json Status() const;
  PreparedSources Take();

 private:
  mutable std::mutex mutex_;
  std::optional<PreparedSources> result_;
  std::exception_ptr error_;
  size_t completed_ = 0, total_ = 0;
  bool done_ = false, cancelled_ = false;
  std::jthread worker_;
};
}  // namespace simpsons::modkit
