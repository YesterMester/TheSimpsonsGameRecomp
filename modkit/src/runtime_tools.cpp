#include "runtime.h"
#include "missions.h"

#include <algorithm>
#include <cmath>

#include "storage.h"

namespace simpsons::modkit {

Json Runtime::Timers(std::string_view owner) const {
  Json result = Json::array();
  for (const auto& [key, timer] : timers_)
    if (key.first == owner)
      result.push_back({{"id", key.second},
                        {"seconds", std::max(0.0, timer.deadline - simulation_time_)},
                        {"interval", timer.interval},
                        {"data", timer.data}});
  return result;
}

void Runtime::AdvanceTimers() {
  // Only the host's simulation delta advances these. Pausing, a menu redraw,
  // or a fast display must not speed up a mod's cooldowns or mission logic.
  std::vector<std::pair<TimerKey, uint64_t>> due;
  for (auto& [key, timer] : timers_) {
    if (timer.deadline <= simulation_time_)
      due.emplace_back(key, timer.generation);
  }
  if (due.empty())
    return;
  const auto first =
      std::upper_bound(due.begin(), due.end(), last_timer_,
                       [](const auto& key, const auto& item) { return key < item.first; });
  std::rotate(due.begin(), first, due.end());
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2);
  size_t delivered = 0;
  for (const auto& [key, generation] : due) {
    if (delivered && (delivered >= 32 || std::chrono::steady_clock::now() >= deadline))
      break;
    const auto found = timers_.find(key);
    // A callback can cancel or replace another timer due on this step. Do
    // not deliver its old event or immediately run its new zero-delay timer.
    if (found == timers_.end() || found->second.generation != generation ||
        found->second.deadline > simulation_time_)
      continue;
    auto timer = found->second;
    const auto& mod = Find(key.first);
    if (mod.failed) {
      timers_.erase(found);
      continue;
    }
    const double late = simulation_time_ - timer.deadline;
    const double missed = timer.interval > 0 ? std::floor(late / timer.interval) : 0;
    if (timer.interval > 0)
      found->second.deadline = simulation_time_ + timer.interval - std::fmod(late, timer.interval);
    else
      timers_.erase(found);
    last_timer_ = key;
    const Json event = {{"id", key.second},
                        {"data", timer.data},
                        {"skipped", std::min(missed, double(UINT32_MAX))},
                        {"frame", frame_}};
    Trace("timer", {{"owner", key.first}, {"data", event}});
    Invoke(Find(key.first), "timer", event);
    ++delivered;
  }
}

void Runtime::EnableInspection(bool enabled) {
  inspection_ = enabled;
  if (!enabled) {
    std::lock_guard lock(trace_mutex_);
    trace_.clear();
  }
}

void Runtime::AdvanceMissions() {
  // Snapshot revisions so a callback cannot recursively deliver its own new
  // changes in the same tick. Dirty missions coalesce and rotate fairly.
  auto pending = missions_->Events();
  const auto first = std::upper_bound(pending.begin(), pending.end(), last_mission_,
      [](const auto& key, const auto& event) { return key < event.first; });
  std::rotate(pending.begin(), first, pending.end());
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2);
  size_t delivered = 0;
  for (const auto& event : pending) {
    if (delivered && (delivered >= 32 || std::chrono::steady_clock::now() >= deadline)) break;
    auto data = missions_->TakeEvent(event);
    if (!data) continue;
    last_mission_ = event.first;
    auto& mod = Find(event.first.first);
    Trace("mission.changed", {{"owner", event.first.first}, {"data", *data}});
    Invoke(mod, "mission.changed", *data);
    ++delivered;
  }
}

void Runtime::Trace(std::string_view event, const Json& data) {
  if (!inspection_)
    return;
  std::lock_guard lock(trace_mutex_);
  if (trace_.size() >= 128)
    trace_.pop_front();
  trace_.push_back({{"sequence", ++trace_sequence_}, {"event", event}, {"data", data}});
}

Json Runtime::Inspect(const Json& request) {
  CheckThread();
  if (!inspection_)
    throw Failure("developer_disabled",
                  "Start with mods_developer enabled to use creator inspection");
  if (!request.is_object())
    throw Failure("invalid_argument", "A creator request must be an object");
  const auto action = request.at("action").get<std::string>();
  if (action == "inspect") {
    Json result = {{"api", 1},
                   {"frame", frame_},
                   {"epoch", epoch_},
                   {"level_ready", level_ready_},
                   {"mods", Status()},
                   {"pages", Json::array()},
                   {"trace", Json::array()}};
    for (const auto& [key, page] : pages_) {
      Json rows = Json::array();
      for (const auto& row : page.rows)
        rows.push_back(
            {{"id", row.id}, {"label", row.label}, {"type", row.type}, {"value", row.value}});
      result["pages"].push_back(
          {{"id", key}, {"owner", page.owner}, {"title", page.title}, {"rows", std::move(rows)}});
    }
    for (auto& mod : result["mods"]) {
      const auto owner = mod.at("id").get<std::string>();
      mod["storage"] = storage_cache_->Status(owner);
      mod["timers"] = Timers(owner);
      mod["resources"] = host_.save ? host_.save(Owner(Find(owner))) : Json::object();
      mod["missions"] = missions_->Call(owner, "missions.list", Json::object());
    }
    const auto after = request.value("after", uint64_t(0));
    {
      std::lock_guard lock(trace_mutex_);
      for (const auto& event : trace_)
        if (event.at("sequence").get<uint64_t>() > after)
          result["trace"].push_back(event);
    }
    return result;
  }
  if (action == "activate") {
    const auto& player = request.at("player");
    if (!player.is_number_integer() || player.get<int64_t>() < 0 || player.get<int64_t>() > 1)
      throw Failure("invalid_argument", "Menu player must be 0 or 1");
    Activate(request.at("page").get<std::string>(), request.at("row").get<std::string>(),
             request.value("value", Json()), player.get<uint32_t>());
    return nullptr;
  }
  if (action == "state") {
    auto& mod = Find(request.at("owner").get<std::string>());
    if (mod.failed)
      throw Failure("disabled_mod", "The requested mod is disabled");
    return Serialize(mod);
  }
  if (action == "call") {
    const auto method = request.at("method").get<std::string>();
    const auto args = request.value("arguments", Json::object());
    const auto owner = request.value("owner", std::string());
    // Mutations and namespaced services use a real active mod's ownership.
    // A creator may inspect the host before loading any mod, but cannot
    // impersonate a missing mod or create unowned menu/storage resources.
    if (!owner.empty())
      return Call(Find(owner), method, args);
    static const std::vector<std::string> readable = {
        "system.describe",    "mods.status",     "world.status",  "players.list",
        "players.controlled", "players.health",  "scores.get",    "camera.status",
        "camera.pose",        "characters.list", "world.catalog", "input.players",
        "input.state",        "settings.get",    "text.lookup",   "assets.search", "assets.templates"};
    if (std::find(readable.begin(), readable.end(), method) == readable.end())
      throw Failure("owner_required", "This operation needs --owner with a loaded mod id");
    // Reuse exactly the normal argument validation and named host adapters.
    // This temporary descriptor never receives callbacks or owns resources.
    struct Reader final : Mod {
      using Mod::Mod;
      void Start() override {}
      void Event(std::string_view, const Json&) override {}
      Json Save() override { return Json::object(); }
      void Load(const Json&) override {}
    };
    Manifest descriptor;
    descriptor.id = "modkit.creator";
    Reader reader(*this, std::move(descriptor));
    return Call(reader, method, args);
  }
  throw Failure("unsupported", "Unknown creator action: " + action);
}
}  // namespace simpsons::modkit
