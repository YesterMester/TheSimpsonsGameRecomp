#pragma once

#include "runtime.h"

namespace simpsons::modkit {

// Authored missions are owned services, not pointers into a campaign script.
// Definitions are registered by code; progress is saved by the host.
class Missions {
 public:
  using Key = std::pair<std::string, std::string>;
  using Pending = std::pair<Key, uint64_t>;
  Json Call(std::string_view owner, std::string_view method, const Json& args);
  Json Save(std::string_view owner) const;
  static Json Empty();
  static void ValidateSaved(const Json& state);
  void Restore(std::string_view owner, const Json& state);
  void Release(std::string_view owner);
  std::vector<Pending> Events() const;
  std::optional<Json> TakeEvent(const Pending& pending);

 private:
  struct Mission {
    Json definition, progress, checkpoint;
    std::string status = "idle", reason;
  };
  static Json Definition(const Json& value);
  static Json Contract(const Json& definition);
  static Mission Initial(Json definition);
  static bool Ready(const Mission& mission, const Json& objective);
  static bool Complete(const Mission& mission);
  static Json View(const Mission& mission);
  static Json Stored(const Mission& mission);
  static std::vector<Mission> ReadSaved(const Json& state);
  void Changed(const Key& key, std::string reason);
  std::map<Key, Mission> missions_;
  struct Change {
    uint64_t revision;
    std::string reason;
  };
  std::map<Key, Change> changes_;
  uint64_t revision_ = 0;
};

}  // namespace simpsons::modkit
