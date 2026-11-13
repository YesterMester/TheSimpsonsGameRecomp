#include "resources.h"

#include <algorithm>
#include <exception>
#include <limits>
#include <set>

namespace simpsons::modkit {

void Resources::Bind(std::string name, Binding binding) {
  std::lock_guard lock(mutex_);
  if (name.empty() || name.size() > 80 || !binding.read || !binding.write || !binding.revision ||
      !binding.validate || resources_.size() >= 128 || resources_.contains(name))
    throw Failure("invalid_argument", "Invalid or duplicate host resource binding");
  resources_.emplace(std::move(name), Resource{std::move(binding)});
}

void Resources::Observe(Resource& resource) {
  if (resource.owners.empty())
    return;
  // A manual control has priority over old mod requests, including a manual
  // request that happens to have the same value. Its revision still changes.
  if (resource.binding.revision() != resource.revision ||
      resource.binding.read() != resource.applied) {
    resource.owners.clear();
    resource.baseline = nullptr;
  }
}

void Resources::Apply(Resource& resource) {
  const auto winner = std::max_element(
      resource.owners.begin(), resource.owners.end(),
      [](const auto& a, const auto& b) { return a.second.order < b.second.order; });
  const auto value = winner == resource.owners.end() ? resource.baseline : winner->second.value;
  if (resource.binding.read() != value)
    resource.binding.write(value);
  if (resource.binding.read() != value)
    throw Failure("resource_unavailable", "The host could not apply a mod resource");
  resource.applied = value;
  resource.revision = resource.binding.revision();
}

void Resources::Set(const HostOwner& owner, std::string_view name, const Json& value) {
  std::lock_guard lock(mutex_);
  const auto found = resources_.find(std::string(name));
  if (!owner.instance || owner.mod.empty() || owner.mod.size() > 160 || found == resources_.end())
    throw Failure("invalid_argument", "A host resource needs a registered binding and owner");
  auto& resource = found->second;
  resource.binding.validate(value);
  Observe(resource);
  if (resource.owners.empty()) {
    auto baseline = resource.binding.capture ? resource.binding.capture() : resource.binding.read();
    resource.binding.validate(baseline);
    resource.baseline = std::move(baseline);
  }
  if (!resource.owners.contains(owner) && resource.owners.size() >= 1024)
    throw Failure("resource_limit", "Too many owners of a host resource");
  if (order_ == std::numeric_limits<uint64_t>::max())
    throw Failure("resource_limit", "Host resource order is exhausted");
  const auto previous = resource.owners;
  resource.owners[owner] = {value, ++order_};
  try {
    Apply(resource);
  } catch (...) {
    const auto error = std::current_exception();
    resource.owners = previous;
    try {
      Apply(resource);
    } catch (...) {}
    std::rethrow_exception(error);
  }
}

Json Resources::Save(const HostOwner& owner) {
  std::lock_guard lock(mutex_);
  Json state = Json::object();
  for (auto& [name, resource] : resources_) {
    Observe(resource);
    const auto found = resource.owners.find(owner);
    if (found != resource.owners.end())
      state[name] = {{"value", found->second.value},
                     {"baseline", resource.baseline},
                     {"order", found->second.order}};
  }
  return state;
}

uint64_t Resources::Order(const Json& value) {
  if (!value.is_number_integer() || (!value.is_number_unsigned() && value.get<int64_t>() <= 0))
    throw Failure("save_incompatible", "Host resource order must be a positive integer");
  const auto order = value.get<uint64_t>();
  if (!order || order == std::numeric_limits<uint64_t>::max())
    throw Failure("save_incompatible", "Host resource order is outside its range");
  return order;
}

void Resources::ValidateLocked(const std::vector<Json>& owners) const {
  if (owners.size() > 1024)
    throw Failure("resource_limit", "Too many saved host resource owners");
  std::map<std::string, Json> baselines;
  std::map<std::string, std::set<uint64_t>> orders;
  for (const auto& state : owners) {
    if (!state.is_object() || state.size() > resources_.size() || state.dump().size() > 65536)
      throw Failure("save_incompatible", "Invalid saved host resources");
    for (auto it = state.begin(); it != state.end(); ++it) {
      const auto found = resources_.find(it.key());
      const auto& value = it.value();
      if (found == resources_.end() || !value.is_object() || value.size() != 3 ||
          !value.contains("value") || !value.contains("baseline") || !value.contains("order"))
        throw Failure("save_incompatible", "Unknown or invalid saved host resource");
      found->second.binding.validate(value.at("value"));
      found->second.binding.validate(value.at("baseline"));
      const auto order = Order(value.at("order"));
      if (!orders[it.key()].insert(order).second)
        throw Failure("save_incompatible", "Saved host resource orders must be unique");
      const auto [base, inserted] = baselines.try_emplace(it.key(), value.at("baseline"));
      if (!inserted && base->second != value.at("baseline"))
        throw Failure("save_incompatible", "Saved host resource baselines disagree");
    }
  }
}

void Resources::Validate(const std::vector<Json>& owners) const {
  std::lock_guard lock(mutex_);
  ValidateLocked(owners);
}

void Resources::Restore(const HostOwner& owner, const Json& state, bool checkpoint) {
  std::lock_guard lock(mutex_);
  ValidateLocked({state});
  if (!owner.instance || owner.mod.empty() || owner.mod.size() > 160)
    throw Failure("invalid_argument", "Invalid restored resource owner");
  std::vector<Resource*> restored;
  try {
    for (auto it = state.begin(); it != state.end(); ++it) {
      auto& resource = resources_.at(it.key());
      if (checkpoint && resource.binding.world_scoped)
        continue;
      Observe(resource);
      if (resource.owners.contains(owner))
        throw Failure("save_incompatible", "A restored resource owner is already active");
      if (resource.owners.size() >= 1024)
        throw Failure("resource_limit", "Too many owners of a host resource");
      if (resource.owners.empty())
        resource.baseline = it.value().at("baseline");
      else if (resource.baseline != it.value().at("baseline"))
        throw Failure("save_incompatible", "Restored host resource baseline changed");
      const auto order = Order(it.value().at("order"));
      for (const auto& [other, lease] : resource.owners)
        if (other != owner && lease.order == order)
          throw Failure("save_incompatible", "Restored host resource order is already active");
      order_ = std::max(order_, order);
      resource.owners[owner] = {it.value().at("value"), order};
      restored.push_back(&resource);
      Apply(resource);
    }
  } catch (...) {
    const auto error = std::current_exception();
    for (auto it = restored.rbegin(); it != restored.rend(); ++it) {
      (*it)->owners.erase(owner);
      // One broken binding must not prevent retirement of the remaining
      // resources acquired by this restore attempt.
      try {
        Apply(**it);
      } catch (...) {}
    }
    std::rethrow_exception(error);
  }
}

void Resources::Release(const HostOwner& owner) {
  std::lock_guard lock(mutex_);
  std::exception_ptr error;
  for (auto& [name, resource] : resources_) {
    try {
      Observe(resource);
      if (resource.owners.erase(owner))
        Apply(resource);
    } catch (...) {
      resource.owners.erase(owner);
      if (!error)
        error = std::current_exception();
    }
  }
  if (error)
    std::rethrow_exception(error);
}

void Resources::Release(const HostOwner& owner, std::string_view name) {
  std::lock_guard lock(mutex_);
  const auto found = resources_.find(std::string(name));
  if (found == resources_.end() || !owner.instance || owner.mod.empty() || owner.mod.size() > 160)
    throw Failure("invalid_argument", "A resource release needs a registered binding and owner");
  auto& resource = found->second;
  Observe(resource);
  if (resource.owners.erase(owner))
    Apply(resource);
}

}  // namespace simpsons::modkit
