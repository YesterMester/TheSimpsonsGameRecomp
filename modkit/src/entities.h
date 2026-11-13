#pragma once

#include "runtime.h"

#include <array>

namespace simpsons::modkit {

// Host bookkeeping only. Guest destruction is drained by the game adapter,
// after callbacks finish; releasing a runtime on a worker never runs the game.
class OwnedEntities {
 public:
  struct Entry {
    HostOwner owner;
    uint64_t lifetime = 0;
    Json description;
    bool pending = false;
    bool dispatched = false;
    // First native death in this allocation: 0 unseen, 1 queued, 2 delivered.
    uint8_t health_death = 0;
    bool render_hidden = false;
    // Use this for filtered host snapshots too; description is identity only.
    Json Save() const;
  };
  void CheckCapacity(const HostOwner& owner) const;
  void CheckTag(const HostOwner& owner, std::string_view tag) const;
  void Add(const HostOwner& owner, uint64_t lifetime, const Json& description);
  // Reject another owner and objects whose removal has already been queued.
  Entry Get(const HostOwner& owner, uint64_t lifetime) const;
  void SetRenderHidden(const HostOwner& owner, uint64_t lifetime, bool hidden);
  // Original force-show supersedes all native hide requests, including ours.
  void ResetRenderHidden(uint64_t lifetime);
  bool Destroy(const HostOwner& owner, uint64_t lifetime);
  std::vector<Entry> List(const HostOwner& owner) const;
  std::vector<Entry> All() const;
  // Sorted host tokens for per-frame liveness checks, without copying JSON.
  std::vector<uint64_t> Lifetimes() const;
  // Native hooks only queue notifications; never reenter Lua from a hit.
  void Died(uint64_t lifetime);
  bool HasPendingDeaths() const;
  std::vector<Entry> TakeDeaths(size_t limit = 16);
  void Release(const HostOwner& owner);
  // Claim each removal once. Keep its identity until Forget confirms the
  // base destructor: a class may finish removal on a later simulation step.
  std::vector<Entry> TakePending(size_t limit = 32);
  std::optional<Entry> Forget(uint64_t lifetime);
  void Clear();
  Json Save(const HostOwner& owner) const;
  static void Validate(const std::vector<Json>& states);
  // Reload transfers a retiring generation's objects without destroying them.
  // A snapshot restore first clears old identities and resolves its GUIDs in
  // the restored world. Checkpoints intentionally recreate objects in the mod.
  void Restore(const HostOwner& owner, const Json& state, bool checkpoint,
               const std::function<uint64_t(const Json&)>& resolve);

 private:
  mutable std::mutex mutex_;
  std::map<uint64_t, Entry> entries_;
};

}  // namespace simpsons::modkit
