#include "entities.h"
#include "scenes.h"

#include <algorithm>
#include <set>

namespace simpsons::modkit {
namespace {
constexpr size_t kPerOwner = 64, kTotal = 1024;
void Owner(const HostOwner& owner) {
  if (!owner.instance || owner.mod.empty() || owner.mod.size() > 160)
    throw Failure("invalid_argument", "A spawned object needs a valid mod owner");
}
void Description(const Json& value) {
  if (!value.is_object() || value.size() < 3 || value.size() > 6 ||
      value.size() != 3 + value.contains("health_death") + value.contains("render_hidden") + value.contains("tag") ||
      !value.contains("guid") || !value.contains("vtable") || !value.contains("class") ||
      !value.at("guid").is_array() || value.at("guid").size() != 4 ||
      !value.at("class").is_string() || value.at("class").get_ref<const std::string&>().empty() ||
      value.at("class").get_ref<const std::string&>().size() > 160)
    throw Failure("save_incompatible", "Invalid saved spawned object");
  if (value.contains("tag") && (!value.at("tag").is_string() ||
      !ObjectTag(value.at("tag").get_ref<const std::string&>())))
    throw Failure("save_incompatible", "Invalid saved object tag");
  if (value.contains("health_death") &&
      (!value.at("health_death").is_number_integer() ||
       value.at("health_death").get<int64_t>() < 1 || value.at("health_death").get<int64_t>() > 2))
    throw Failure("save_incompatible", "Invalid saved native death notification");
  if (value.contains("render_hidden") &&
      (!value.at("render_hidden").is_boolean() || !value.at("render_hidden").get<bool>()))
    throw Failure("save_incompatible", "Invalid saved visibility request");
  const auto word = [](const Json& item) {
    return item.is_number_integer() &&
        (item.is_number_unsigned() || item.get<int64_t>() >= 0) &&
        item.get<uint64_t>() <= UINT32_MAX;
  };
  for (const auto& part : value.at("guid"))
    if (!word(part)) throw Failure("save_incompatible", "Invalid spawned object GUID");
  if (value.at("guid").at(0) != 0x534D4F44 || value.at("guid").at(3) == 0)
    throw Failure("save_incompatible", "Saved objects need mod-owned GUIDs");
  if (!word(value.at("vtable")) || value.at("vtable") == 0)
    throw Failure("save_incompatible", "Invalid spawned object type");
}
}  // namespace

Json OwnedEntities::Entry::Save() const {
  auto value = description;
  if (health_death) value["health_death"] = health_death;
  if (render_hidden) value["render_hidden"] = true;
  return value;
}

void OwnedEntities::CheckCapacity(const HostOwner& owner) const {
  Owner(owner);
  std::lock_guard lock(mutex_);
  size_t count = 0;
  for (const auto& [key, entry] : entries_) if (entry.owner == owner) ++count;
  if (count >= kPerOwner || entries_.size() >= kTotal)
    throw Failure("resource_limit", "A mod may own up to 64 objects; the profile limit is 1024");
}
void OwnedEntities::CheckTag(const HostOwner& owner, std::string_view tag) const {
  Owner(owner);
  if (!ObjectTag(tag)) throw Failure("invalid_argument", "Object tags use 1 to 64 lowercase letters, digits, periods, underscores or hyphens");
  std::lock_guard lock(mutex_);
  for (const auto& [key, entry] : entries_)
    if (entry.owner == owner && entry.description.value("tag", std::string()) == tag)
      throw Failure("tag_in_use", "This mod already owns an object with that tag; wait for pending removal to finish");
}

void OwnedEntities::Add(const HostOwner& owner, uint64_t lifetime, const Json& description) {
  Owner(owner);
  Description(description);
  if (description.contains("health_death") || description.contains("render_hidden"))
    throw Failure("invalid_argument", "A new object cannot inherit saved object state");
  std::lock_guard lock(mutex_);
  size_t count = 0;
  for (const auto& [key, entry] : entries_) if (entry.owner == owner) ++count;
  if (description.contains("tag"))
    for (const auto& [key, entry] : entries_)
      if (entry.owner == owner && entry.description.value("tag", std::string()) == description.at("tag").get<std::string>())
        throw Failure("tag_in_use", "An owned object already uses this tag");
  if (!lifetime || entries_.contains(lifetime))
    throw Failure("invalid_argument", "The spawned object already has an owner");
  if (count >= kPerOwner || entries_.size() >= kTotal)
    throw Failure("resource_limit", "A mod may own up to 64 objects; the profile limit is 1024");
  entries_.emplace(lifetime, Entry{owner, lifetime, description});
}
OwnedEntities::Entry OwnedEntities::Get(const HostOwner& owner, uint64_t lifetime) const {
  std::lock_guard lock(mutex_);
  const auto found = entries_.find(lifetime);
  if (found == entries_.end() || found->second.owner != owner)
    throw Failure("not_owner", "A mod can only access its own spawned objects");
  if (found->second.pending)
    throw Failure("stale_handle", "This object's removal is already queued");
  return found->second;
}
void OwnedEntities::SetRenderHidden(const HostOwner& owner, uint64_t lifetime, bool hidden) {
  std::lock_guard lock(mutex_);
  const auto found = entries_.find(lifetime);
  if (found == entries_.end() || found->second.owner != owner)
    throw Failure("not_owner", "A mod can only change visibility of its own objects");
  if (found->second.pending)
    throw Failure("stale_handle", "This object's removal is already queued");
  found->second.render_hidden = hidden;
}
void OwnedEntities::ResetRenderHidden(uint64_t lifetime) {
  std::lock_guard lock(mutex_);
  const auto found = entries_.find(lifetime);
  if (found != entries_.end())
    found->second.render_hidden = false;
}
bool OwnedEntities::Destroy(const HostOwner& owner, uint64_t lifetime) {
  std::lock_guard lock(mutex_);
  const auto found = entries_.find(lifetime);
  if (found == entries_.end() || found->second.owner != owner)
    throw Failure("not_owner", "A mod can only remove its own spawned objects");
  const bool changed = !found->second.pending;
  found->second.pending = true;
  return changed;
}
std::vector<OwnedEntities::Entry> OwnedEntities::List(const HostOwner& owner) const {
  std::lock_guard lock(mutex_);
  std::vector<Entry> result;
  for (const auto& [key, entry] : entries_)
    if (entry.owner == owner && !entry.pending) result.push_back(entry);
  return result;
}
std::vector<OwnedEntities::Entry> OwnedEntities::All() const {
  std::lock_guard lock(mutex_);
  std::vector<Entry> result;
  for (const auto& [key, entry] : entries_) result.push_back(entry);
  return result;
}
std::vector<uint64_t> OwnedEntities::Lifetimes() const {
  std::lock_guard lock(mutex_);
  std::vector<uint64_t> result;
  result.reserve(entries_.size());
  for (const auto& [key, entry] : entries_) result.push_back(key);
  return result;
}
void OwnedEntities::Died(uint64_t lifetime) {
  std::lock_guard lock(mutex_);
  const auto found = entries_.find(lifetime);
  if (found != entries_.end() && !found->second.pending && !found->second.health_death)
    found->second.health_death = 1;
}
bool OwnedEntities::HasPendingDeaths() const {
  std::lock_guard lock(mutex_);
  return std::any_of(entries_.begin(), entries_.end(),
      [](const auto& item) { return item.second.health_death == 1; });
}
std::vector<OwnedEntities::Entry> OwnedEntities::TakeDeaths(size_t limit) {
  std::lock_guard lock(mutex_);
  std::vector<Entry> result;
  for (auto& [key, entry] : entries_) {
    if (result.size() >= limit) break;
    if (entry.health_death == 1) {
      entry.health_death = 2;
      result.push_back(entry);
    }
  }
  return result;
}
void OwnedEntities::Release(const HostOwner& owner) {
  std::lock_guard lock(mutex_);
  for (auto& [key, entry] : entries_) if (entry.owner == owner) entry.pending = true;
}
std::vector<OwnedEntities::Entry> OwnedEntities::TakePending(size_t limit) {
  std::lock_guard lock(mutex_);
  std::vector<Entry> result;
  for (auto& [key, entry] : entries_) {
    if (result.size() >= limit) break;
    if (entry.pending && !entry.dispatched) {
      entry.dispatched = true;
      result.push_back(entry);
    }
  }
  return result;
}
std::optional<OwnedEntities::Entry> OwnedEntities::Forget(uint64_t lifetime) {
  std::lock_guard lock(mutex_);
  const auto found = entries_.find(lifetime);
  if (found == entries_.end()) return {};
  auto entry = found->second;
  entries_.erase(found);
  return entry;
}
void OwnedEntities::Clear() {
  std::lock_guard lock(mutex_);
  entries_.clear();
}
Json OwnedEntities::Save(const HostOwner& owner) const {
  Json result = Json::array();
  for (const auto& entry : List(owner)) result.push_back(entry.Save());
  return result;
}
void OwnedEntities::Validate(const std::vector<Json>& states) {
  size_t count = 0;
  std::set<std::array<uint32_t, 4>> guids;
  for (const auto& state : states) {
    std::set<std::string> tags;
    if (!state.is_array() || state.size() > kPerOwner || (count += state.size()) > kTotal)
      throw Failure("save_incompatible", "Too many saved spawned objects");
    for (const auto& value : state) {
      Description(value);
      if (value.contains("tag") && !tags.insert(value.at("tag").get<std::string>()).second)
        throw Failure("save_incompatible", "A saved owner cannot reuse an object tag");
      if (!guids.insert(value.at("guid").get<std::array<uint32_t, 4>>()).second)
        throw Failure("save_incompatible", "A saved spawned object has multiple owners");
    }
  }
}
void OwnedEntities::Restore(const HostOwner& owner, const Json& state, bool checkpoint,
                            const std::function<uint64_t(const Json&)>& resolve) {
  Owner(owner);
  Validate({state});
  if (checkpoint) return;
  // Resolution may call guest code. Never hold the bookkeeping mutex across it.
  std::vector<Entry> resolved;
  std::set<uint64_t> unique;
  for (const auto& value : state) {
    auto description = value;
    const auto death = uint8_t(description.value("health_death", 0));
    const auto hidden = description.value("render_hidden", false);
    description.erase("health_death");
    description.erase("render_hidden");
    const auto lifetime = resolve(description);
    if (!lifetime || !unique.insert(lifetime).second)
      throw Failure("save_incompatible", "A saved spawned object is no longer available");
    resolved.push_back({owner, lifetime, std::move(description), false, false, death, hidden});
  }
  std::lock_guard lock(mutex_);
  auto replacement = entries_;
  for (const auto& entry : resolved) {
    const auto lifetime = entry.lifetime;
    const auto& description = entry.description;
    const auto found = replacement.find(lifetime);
    if (found != replacement.end() &&
        (!found->second.pending || found->second.owner.mod != owner.mod ||
         found->second.description != description))
      throw Failure("save_incompatible", "A restored spawned object belongs to another owner");
    replacement[lifetime] = entry;
  }
  if (replacement.size() > kTotal)
    throw Failure("resource_limit", "Too many spawned objects in this profile");
  size_t count = 0;
  for (const auto& [key, entry] : replacement) if (entry.owner == owner) ++count;
  if (count > kPerOwner)
    throw Failure("resource_limit", "Too many spawned objects for the restored mod");
  entries_.swap(replacement);
}

}  // namespace simpsons::modkit
