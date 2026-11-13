#pragma once

#include "runtime.h"

namespace simpsons::modkit {

// Native source files can depend on one another across archive requests. A
// generation leases the whole retained session; additions commit or roll back
// without retiring the session's previously accepted objects and resources.
// The game adapter serializes access and supplies only committed source paths.
class SourceLeases {
 public:
  struct Error { std::string path, message; };

  void Begin(std::string path) {
    if (!loading_.empty()) throw Failure("busy", "An object source is already being prepared");
    loading_ = std::move(path);
  }
  void Join(const HostOwner& owner) {
    if (loading_.empty()) throw Failure("not_ready", "No source addition is being prepared");
    if (!pending_.contains(owner)) {
      Pending previous;
      if (const auto found = claims_.find(owner); found != claims_.end()) previous.before = found->second;
      pending_.emplace(owner, std::move(previous));
    }
    claims_[owner] = loading_;
    errors_.erase(owner);
  }
  void Share(const HostOwner& owner, std::string path) {
    claims_[owner] = std::move(path);
    pending_.erase(owner);
    errors_.erase(owner);
  }
  void Retain(const HostOwner& owner, const std::string& committed_path) {
    if (const auto pending = pending_.find(owner); pending != pending_.end() &&
        Claim(owner) == loading_)
      pending->second.spawned = committed_path;
    if (!claims_.contains(owner)) claims_[owner] = committed_path;
    errors_.erase(owner);
  }
  bool Release(const HostOwner& owner) {
    pending_.erase(owner);
    errors_.erase(owner);
    return claims_.erase(owner) != 0;
  }
  bool Withdraw(const HostOwner& owner) {
    const auto found = pending_.find(owner);
    if (found == pending_.end()) return false;
    if (found->second.before) claims_[owner] = *found->second.before;
    else if (found->second.spawned) claims_[owner] = *found->second.spawned;
    else claims_.erase(owner);
    pending_.erase(found);
    errors_.erase(owner);
    return true;
  }
  void Complete(std::string error = {}) {
    if (!error.empty()) {
      for (const auto& [owner, previous] : pending_) {
        if (Claim(owner) != loading_) continue;
        if (previous.before) claims_[owner] = *previous.before;
        else if (previous.spawned) claims_[owner] = *previous.spawned;
        else claims_.erase(owner);
        errors_[owner] = {loading_, error};
      }
    }
    pending_.clear();
    loading_.clear();
  }
  bool Wanted() const { return !pending_.empty(); }
  std::string Claim(const HostOwner& owner) const {
    const auto found = claims_.find(owner);
    return found == claims_.end() ? std::string() : found->second;
  }
  std::optional<Error> FailureFor(const HostOwner& owner) const {
    const auto found = errors_.find(owner);
    return found == errors_.end() ? std::nullopt : std::optional<Error>(found->second);
  }
  bool Empty() const { return claims_.empty(); }
  size_t Size() const { return claims_.size(); }
  void Clear() { claims_.clear(); pending_.clear(); errors_.clear(); loading_.clear(); }

 private:
  struct Pending { std::optional<std::string> before, spawned; };
  std::map<HostOwner, std::string> claims_;
  std::map<HostOwner, Pending> pending_;
  std::map<HostOwner, Error> errors_;
  std::string loading_;
};

}  // namespace simpsons::modkit
