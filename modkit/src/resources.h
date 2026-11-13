#pragma once

#include "runtime.h"

namespace simpsons::modkit {

// Reversible host state shared by runtime generations. Bindings use host
// values only: releasing a resource must remain safe after guest threads stop.
class Resources {
 public:
  struct Binding {
    std::function<Json()> read;
    std::function<void(const Json&)> write;
    std::function<uint64_t()> revision;
    std::function<void(const Json&)> validate;
    bool world_scoped = false;
    // Optional richer baseline (for example, the current manual camera pose).
    // It is captured only while acquiring the first owner, never on retirement.
    std::function<Json()> capture;
  };
  void Bind(std::string name, Binding binding);
  void Set(const HostOwner& owner, std::string_view name, const Json& value);
  Json Save(const HostOwner& owner);
  void Validate(const std::vector<Json>& owners) const;
  void Restore(const HostOwner& owner, const Json& state, bool checkpoint);
  void Release(const HostOwner& owner);
  void Release(const HostOwner& owner, std::string_view name);

 private:
  struct Lease {
    Json value;
    uint64_t order = 0;
  };
  struct Resource {
    Binding binding;
    std::map<HostOwner, Lease> owners;
    Json baseline, applied;
    uint64_t revision = 0;
  };
  static void Observe(Resource& resource);
  static void Apply(Resource& resource);
  static uint64_t Order(const Json& value);
  void ValidateLocked(const std::vector<Json>& owners) const;
  mutable std::mutex mutex_;
  std::map<std::string, Resource> resources_;
  uint64_t order_ = 0;
};

}  // namespace simpsons::modkit
