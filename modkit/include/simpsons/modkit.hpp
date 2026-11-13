#pragma once

#include "modkit.h"

#include <nlohmann/json.hpp>

#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace simpsons::modkit {

using Json = nlohmann::json;

class Error : public std::runtime_error {
 public:
  Error(std::string code, std::string message)
      : std::runtime_error(std::move(message)), code_(std::move(code)) {}
  const std::string& code() const { return code_; }

 private:
  std::string code_;
};

// A handle is an identity issued by the game, never an address. Do not store
// it in persistent state; reacquire it after level changes and save restores.
struct Handle {
  std::string id;
};
struct Vec3 {
  float x = 0, y = 0, z = 0;
};
struct Player {
  Handle handle;
  uint32_t slot = 0;  // Family-list index; use input_player for logical ownership.
  std::string character;
  Vec3 position;
  float health = 0, max_health = 0;
  std::optional<uint32_t> input_player;  // empty for AI, unbound or ambiguous
};
struct Entity {
  Handle handle;
  std::string class_name;
  std::string tag;
};
struct EntityTransform {
  Vec3 position, right, up, forward;
};
struct SourceStatusInfo {
  std::string phase, stream, message, pending_stream;
  bool owned = false, ready = false;
  uint32_t files = 0, loaded = 0, definitions = 0;
  std::vector<std::string> streams;
};
struct EntityRenderState {
  bool hidden_by_mod = false;
  uint32_t hide_count = 0;
  bool has_model = false, scene_attached = false;
};
struct Health {
  float current = 0, maximum = 0;
  bool dead = false, enabled = false;
};
struct HealthChange {
  Health before;
  std::optional<Health> after;
  bool removed = false;
};

class Game {
 public:
  explicit Game(const SimModHost* host) : host_(host) {
    if (!host || host->struct_size < sizeof(SimModHost) ||
        host->abi_version != SIM_MOD_ABI_VERSION || !host->call)
      throw Error("abi_mismatch", "This mod needs a compatible Simpsons Modkit host");
  }

  Json Call(std::string_view method, const Json& arguments = Json::object()) const {
    struct Reply { std::string text; bool failed = false; } reply;
    const std::string name(method), data = arguments.dump();
    host_->call(host_->context, name.c_str(), data.c_str(),
                [](void* user, const char* bytes, size_t size) {
                  auto& reply = *static_cast<Reply*>(user);
                  if ((!bytes && size) || size > 4 * 1024 * 1024 - reply.text.size()) {
                    reply.failed = true;
                    return;
                  }
                  try { if (size) reply.text.append(bytes, size); }
                  catch (...) { reply.failed = true; }
                }, &reply);
    if (reply.failed) throw Error("reply_too_large", "The game reply could not be received");
    const auto result = Json::parse(reply.text);
    if (!result.at("ok").get<bool>())
      throw Error(result.value("code", "error"), result.value("error", "Mod operation failed"));
    return result.at("value");
  }

  Json TryCall(std::string_view method, const Json& arguments = Json::object()) const {
    return Call("system.try_call", {{"method", method}, {"arguments", arguments}});
  }

  std::vector<Player> Players() const {
    std::vector<Player> players;
    for (const auto& p : Call("players.list")) players.push_back(ReadPlayer(p));
    return players;
  }
  // Requery during simulation: joining, dropping out and switching characters
  // can change the actor while its previous handle still identifies a live AI.
  std::optional<Player> ControlledPlayer(uint32_t player) const {
    const auto actor = Call("players.controlled", {{"player", player}});
    return actor.is_null() ? std::nullopt : std::optional<Player>(ReadPlayer(actor));
  }
  // Logical player 0 or 1, after the game's routing and held-input guards.
  // A snapshot is shared by every mod in this frame; reading never consumes it.
  Json Input(uint32_t player) const { return Call("input.state", {{"player", player}}); }
  Json InputPlayers() const { return Call("input.players"); }
  void Teleport(const Handle& player, Vec3 p) const {
    Call("players.teleport", {{"player", player.id}, {"position", {{"x", p.x}, {"y", p.y}, {"z", p.z}}}});
  }
  void RefillHealth(const Handle& player) const {
    Call("players.refill_health", {{"player", player.id}});
  }
  std::optional<Health> PlayerHealth(const Handle& player) const {
    return ReadHealth(Call("players.health", {{"player", player.id}}));
  }
  std::optional<Health> EntityHealth(const Handle& entity) const {
    return ReadHealth(Call("entities.health", {{"entity", entity.id}}));
  }
  HealthChange DamagePlayer(const Handle& player, float amount) const {
    return ReadHealthChange(Call("players.damage", {{"player", player.id}, {"amount", amount}}));
  }
  HealthChange HealPlayer(const Handle& player, float amount) const {
    return ReadHealthChange(Call("players.heal", {{"player", player.id}, {"amount", amount}}));
  }
  HealthChange DamageEntity(const Handle& entity, float amount) const {
    return ReadHealthChange(Call("entities.damage", {{"entity", entity.id}, {"amount", amount}}));
  }
  HealthChange HealEntity(const Handle& entity, float amount) const {
    return ReadHealthChange(Call("entities.heal", {{"entity", entity.id}, {"amount", amount}}));
  }
  Handle Spawn(const Handle& loaded_template, Vec3 position, float radius = 1,
               std::string_view tag = {}) const {
    Json arguments = {{"template", loaded_template.id},
        {"position", {{"x", position.x}, {"y", position.y}, {"z", position.z}}},
        {"radius", radius}};
    if (!tag.empty()) arguments["tag"] = tag;
    return {Call("world.spawn", arguments).get<std::string>()};
  }
  std::optional<Handle> Tagged(std::string_view tag) const {
    const auto value = Call("world.tagged", {{"tag", tag}});
    return value.is_null() ? std::nullopt : std::optional<Handle>{{value.get<std::string>()}};
  }
  std::vector<Entity> OwnedEntities() const {
    std::vector<Entity> result;
    for (const auto& item : Call("world.owned"))
      result.push_back({{item.at("id").get<std::string>()}, item.at("class").get<std::string>(),
                        item.value("tag", std::string())});
    return result;
  }
  bool Destroy(const Handle& entity) const {
    return Call("world.destroy", {{"entity", entity.id}}).get<bool>();
  }
  EntityTransform Transform(const Handle& entity) const {
    const auto transform = Call("entities.transform", {{"entity", entity.id}});
    const auto vector = [&](const char* name) -> Vec3 {
      const auto& v = transform.at(name);
      return {v.at("x").get<float>(), v.at("y").get<float>(), v.at("z").get<float>()};
    };
    return {vector("position"), vector("right"), vector("up"), vector("forward")};
  }
  void Move(const Handle& entity, Vec3 position) const {
    Call("entities.move", {{"entity", entity.id},
        {"position", {{"x", position.x}, {"y", position.y}, {"z", position.z}}}});
  }
  void SetTransform(const Handle& entity, const EntityTransform& transform) const {
    const auto vector = [](Vec3 v) -> Json { return {{"x", v.x}, {"y", v.y}, {"z", v.z}}; };
    Call("entities.set_transform", {{"entity", entity.id}, {"transform", {
        {"position", vector(transform.position)}, {"right", vector(transform.right)},
        {"up", vector(transform.up)}, {"forward", vector(transform.forward)}}}});
  }
  bool LoadSource(std::string_view stream) const {
    return Call("assets.load_source", {{"stream", stream}}).get<bool>();
  }
  Json SceneDocument() const { return Call("scenes.document"); }
  Json ValidateScene(const Json& document) const { return Call("scenes.validate", {{"document", document}}); }
  Json SaveScene(const Json& document) const { return Call("scenes.save", {{"document", document}}); }
  Json ProjectDocument(std::string_view kind) const { return Call("projects.document", {{"kind", kind}}); }
  Json ValidateProjectDocument(std::string_view kind, const Json& document) const {
    return Call("projects.validate", {{"kind", kind}, {"document", document}});
  }
  Json SaveProjectDocument(std::string_view kind, const Json& document) const {
    return Call("projects.save", {{"kind", kind}, {"document", document}});
  }
  Json SaveProjectDocuments(const Json& documents) const {
    return Call("projects.save_all", {{"documents", documents}});
  }
  std::vector<std::string> SourceStreams() const {
    return Call("assets.source_streams").get<std::vector<std::string>>();
  }
  SourceStatusInfo SourceStatus() const {
    const auto state = Call("assets.source_status");
    return {state.at("phase").get<std::string>(), state.at("stream").get<std::string>(),
        state.at("message").get<std::string>(), state.value("pending_stream", std::string()),
        state.at("owned").get<bool>(), state.at("ready").get<bool>(),
        state.at("files").get<uint32_t>(), state.at("loaded").get<uint32_t>(),
        state.at("definitions").get<uint32_t>(), state.value("streams", std::vector<std::string>())};
  }
  bool CancelSource() const { return Call("assets.cancel_source").get<bool>(); }
  bool ReleaseSource() const { return Call("assets.release_source").get<bool>(); }
  std::optional<EntityRenderState> RenderState(const Handle& entity) const {
    return ReadRenderState(Call("entities.render_state", {{"entity", entity.id}}));
  }
  std::optional<EntityRenderState> SetVisible(const Handle& entity, bool visible) const {
    return ReadRenderState(
        Call("entities.set_visible", {{"entity", entity.id}, {"visible", visible}}));
  }
  float Score(std::string_view name) const {
    return Call("scores.get", {{"name", name}}).get<float>();
  }
  void SetScore(std::string_view name, float value) const {
    Call("scores.set", {{"name", name}, {"value", value}});
  }
  void IssueScoreEvent(std::string_view name) const {
    Call("scores.issue", {{"name", name}});
  }
  Json RegisterMission(const Json& definition) const {
    return Call("missions.register", {{"definition", definition}});
  }
  Json Missions() const { return Call("missions.list"); }
  Json Mission(std::string_view id) const { return Call("missions.get", {{"id", id}}); }
  bool RemoveMission(std::string_view id) const { return Call("missions.remove", {{"id", id}}).get<bool>(); }
  Json StartMission(std::string_view id) const { return Call("missions.start", {{"id", id}}); }
  Json AdvanceObjective(std::string_view id, std::string_view objective, double amount = 1) const {
    return Call("missions.advance", {{"id", id}, {"objective", objective}, {"amount", amount}});
  }
  Json AdvanceObjectives(std::string_view id, const Json& updates) const {
    return Call("missions.advance_batch", {{"id", id}, {"updates", updates}});
  }
  Json FailMission(std::string_view id, std::string_view reason) const {
    return Call("missions.fail", {{"id", id}, {"reason", reason}});
  }
  Json CheckpointMission(std::string_view id) const {
    return Call("missions.checkpoint", {{"id", id}});
  }
  Json RestartMission(std::string_view id, bool from_checkpoint = true) const {
    return Call("missions.restart", {{"id", id}, {"from_checkpoint", from_checkpoint}});
  }
  Json RestoreMissions(const Json& state) const {
    return Call("missions.restore", {{"state", state}});
  }
  void FreeCamera(bool enabled) const { Call("camera.free", {{"enabled", enabled}}); }
  void PhotoMode(bool enabled) const { Call("camera.photo", {{"enabled", enabled}}); }
  std::string Text(std::string_view key, const Json& values = Json::object()) const {
    return Call("locale.text", {{"key", key}, {"values", values}}).get<std::string>();
  }
  void ScheduleTimer(std::string_view id, double seconds, double interval = 0,
                     const Json& data = Json::object()) const {
    Call("timers.schedule", {{"id", id}, {"seconds", seconds}, {"interval", interval}, {"data", data}});
  }
  bool CancelTimer(std::string_view id) const {
    return Call("timers.cancel", {{"id", id}}).get<bool>();
  }
  std::string Locale() const { return Call("locale.current").get<std::string>(); }
  void Log(const std::string& message) const { host_->log(host_->context, message.c_str()); }

 private:
  static std::optional<EntityRenderState> ReadRenderState(const Json& value) {
    if (value.is_null())
      return {};
    return EntityRenderState{
        value.at("hidden_by_mod").get<bool>(), value.at("hide_count").get<uint32_t>(),
        value.at("has_model").get<bool>(), value.at("scene_attached").get<bool>()};
  }
  static std::optional<Health> ReadHealth(const Json& value) {
    if (value.is_null()) return {};
    return Health{value.at("current").get<float>(), value.at("maximum").get<float>(),
                  value.at("dead").get<bool>(), value.at("enabled").get<bool>()};
  }
  static HealthChange ReadHealthChange(const Json& value) {
    const auto before = ReadHealth(value.at("before"));
    if (!before) throw Error("invalid_reply", "The native health reply needs its before snapshot");
    return {*before, ReadHealth(value.at("after")),
            value.at("removed").get<bool>()};
  }
  static Player ReadPlayer(const Json& p) {
    const auto& v = p.at("position");
    Player result{{p.at("id").get<std::string>()}, p.at("slot").get<uint32_t>(),
                  p.at("character").get<std::string>(),
                  {v.at("x").get<float>(), v.at("y").get<float>(), v.at("z").get<float>()},
                  p.at("health").get<float>(), p.at("max_health").get<float>(), {}};
    if (p.contains("input_player") && !p.at("input_player").is_null())
      result.input_player = p.at("input_player").get<uint32_t>();
    return result;
  }
  const SimModHost* host_;
};

}  // namespace simpsons::modkit
