#include "modkit_game.h"

#include "characters.h"
#include "freecam.h"
#include "game_thread.h"
#include "game_ui.h"
#include "guest_memory.h"
#include "modkit_filesystem.h"
#include "modkit_input.h"
#include "players.h"
#include "ragdoll_settings.h"
#include "refpack_state.h"
#include "saves.h"
#include "spawner.h"
#include "spawn_sources.h"
#include "source_state.h"
#include "assets.h"
#include "resource_id.h"
#include "modkit_text.h"
#include "modkit_models.h"
#include "modkit_meshes.h"
#include "developer.h"
#include "resources.h"
#include "entities.h"
#include "scenes.h"
#include "entity_transform.h"
#include "entity_health.h"
#include "entity_render.h"
#include "source.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <charconv>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <map>
#include <mutex>
#include <sstream>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/string.h>

REX_EXTERN(sub_8282AC20);
REX_EXTERN(__imp__sub_829563B8);

REXCVAR_DEFINE_STRING(mods_root, "", "Mods", "Mod packages folder (default: user data/mods)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(mods_profile, "default", "Mods", "Enabled mod profile")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(mods_profile_file, "", "Mods", "Development: absolute path to an isolated mod profile")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(mods_disabled, false, "Mods", "Start without loading mods")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(mods_language, "", "Mods", "Mod text language code (empty follows the game language)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(mods_developer, false, "Mods", "Create a local creator inspection and automation session")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(mods_editor_preview, false, "Mods", "Development: start the private editor-content map preview")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(quick_launch_level, "", "Game",
                      "Development: boot straight into the specified episode or map entry, "
                      "skipping logos, menus, and cutscenes")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DECLARE(uint32_t, user_language);

namespace modkit_game {
namespace {
using namespace simpsons::modkit;
rex::Runtime* g_game = nullptr;
std::unique_ptr<Runtime> g_mods;
std::unique_ptr<Runtime> g_prepared;
nlohmann::json g_prepared_spawn_source;
std::unique_ptr<DeveloperBridge> g_developer;
std::shared_ptr<Resources> g_resources;
std::shared_ptr<OwnedEntities> g_entities;
std::mutex g_entity_hook_mutex;
std::weak_ptr<OwnedEntities> g_hook_entities;
bool g_restoring_entities = false;
std::unique_ptr<SourceJob> g_reload_job;
std::unique_ptr<SourceJob> g_checkpoint_job;
fs::path g_checkpoint_folder;
bool g_checkpoint_cancelling = false;
std::string g_checkpoint_error;
Json g_reload_status = {{"state", "idle"}, {"generation", 0}};
uint64_t g_reload_generation = 0;
bool g_reload_cancelling = false;
std::recursive_mutex g_mutex;
thread_local PPCContext* g_context = nullptr;
thread_local uint8_t* g_base = nullptr;
bool g_checkpoint_pending = false;
uint64_t g_checkpoint_step = 0, g_checkpoint_world = 0;
bool g_checkpoint_was_ready = false;
std::atomic<uint64_t> g_world_changes{1};
std::atomic<uint64_t> g_physics_steps{0}, g_unload_step{0};
uint64_t g_world = 0, g_next_handle = 0;
uint64_t g_previous_step = 0;
bool g_ready = false;
std::string g_view_mode = "all";
std::map<uint64_t, bool> g_view_hidden;
uint64_t g_view_revision = ~uint64_t(0), g_view_isolate = 0;
size_t g_view_supported = 0, g_view_unsupported = 0;
bool g_editor_boot = false;
std::chrono::steady_clock::time_point g_editor_started, g_editor_press, g_view_checked;
std::mutex g_view_reset_mutex;
std::vector<uint64_t> g_view_resets;
std::atomic<bool> g_view_active{false};
struct Identity {
  std::string kind;
  uint32_t address = 0;
  std::array<uint32_t, 4> guid{};
  uint32_t vtable = 0;
  spawner::Record record;
  uint64_t lifetime = 0;
};
std::map<std::string, Identity> g_handles;

std::array<uint32_t, 4> Guid(uint32_t entity) {
  std::array<uint32_t, 4> guid;
  for (size_t i = 0; i < 4; ++i) guid[i] = simpsons::LoadGuestU32(g_base, entity + 28 + i * 4);
  return guid;
}
std::string Handle(std::string kind, uint32_t entity, spawner::Record record = {},
                   bool current_template = false) {
  if (kind == "template" && record.source_layer) {
    if (!current_template && !spawner::TemplateValid(g_base, record))
      throw Failure("stale_handle", "This template's source stream is no longer loaded");
    for (const auto& [id, value] : g_handles)
      if (value.kind == kind && value.record.data == record.data &&
          value.record.source_guid == record.source_guid &&
          value.record.source_layer == record.source_layer &&
          value.record.source_world == record.source_world &&
          value.record.source_epoch == record.source_epoch)
        return id;
    std::ostringstream id;
    id << kind << ':' << std::hex << ++g_next_handle;
    g_handles[id.str()] = {std::move(kind), 0, record.source_guid, 0, std::move(record), 0};
    return id.str();
  }
  if (!spawner::Readable(entity, 72))
    throw Failure("stale_handle", "This object is no longer loaded");
  const auto lifetime = spawner::Observe(entity);
  const auto guid = Guid(entity);
  const auto vtable = simpsons::LoadGuestU32(g_base, entity);
  for (const auto& [id, value] : g_handles)
    if (value.address == entity && value.kind == kind && value.guid == guid && value.vtable == vtable &&
        value.lifetime == lifetime)
      return id;
  std::ostringstream id;
  id << kind << ':' << std::hex << ++g_next_handle;
  g_handles[id.str()] = {std::move(kind), entity, guid, vtable, std::move(record), lifetime};
  return id.str();
}
const Identity& ResolveHandle(const std::string& id, std::string_view kind) {
  const auto found = g_handles.find(id);
  if (g_world == g_world_changes.load() && found != g_handles.end() && kind == "template" &&
      found->second.kind == kind && found->second.record.source_layer &&
      spawner::TemplateValid(g_base, found->second.record))
    return found->second;
  if (g_world != g_world_changes.load() || found == g_handles.end() || found->second.kind != kind ||
      spawner::Lifetime(found->second.address) != found->second.lifetime ||
      !spawner::Readable(found->second.address, 72) ||
      simpsons::LoadGuestU32(g_base, found->second.address) != found->second.vtable ||
      Guid(found->second.address) != found->second.guid)
    throw Failure("stale_handle", "This object is no longer loaded; query the current level again");
  return found->second;
}
std::array<float, 3> Position(const Json& value) {
  const std::array<float, 3> p{value.at("x").get<float>(), value.at("y").get<float>(), value.at("z").get<float>()};
  for (const float axis : p)
    if (!std::isfinite(axis)) throw Failure("invalid_argument", "Positions must be finite");
  return p;
}
players::Player Player(const std::string& id) {
  const auto& identity = ResolveHandle(id, "player");
  for (const auto& player : players::List(g_base))
    if (player.entity == identity.address) return player;
  throw Failure("stale_handle", "That player is no longer loaded");
}
Json PlayerRecord(const players::Player& player, uint32_t slot) {
  return {{"id", Handle("player", player.entity)}, {"slot", slot},
          {"character", players::CharacterName(player.character)},
          {"input_player", player.input_player >= 0 ? Json(player.input_player) : Json(nullptr)},
          {"health", player.hit_points}, {"max_health", player.max_hit_points},
          {"position", {{"x", player.position[0]}, {"y", player.position[1]}, {"z", player.position[2]}}}};
}
uint32_t InputPlayer(const Json& args) {
  const auto& player = args.at("player");
  if (!player.is_number_integer() || player.get<int64_t>() < 0 || player.get<int64_t>() > 1)
    throw Failure("invalid_argument", "Input player must be 0 or 1");
  return player.get<uint32_t>();
}
Json HealthRecord(const entity_health::Health& health) {
  return {{"current", health.current}, {"maximum", health.maximum},
          {"dead", health.dead}, {"enabled", health.enabled}};
}
Json HealthCall(uint32_t entity, std::string_view name, const Json& args) {
  const auto health = entity_health::Get(*g_context, g_base, entity);
  if (name.ends_with(".health")) return health ? HealthRecord(*health) : Json(nullptr);
  if (!health) throw Failure("unsupported", "This object has no supported health component");
  const bool refill = name == "players.refill_health";
  const double amount = refill ? std::max(0.0f, health->maximum - health->current)
                               : args.at("amount").get<double>();
  if (!std::isfinite(amount) || amount < 0 || amount > 1000000)
    throw Failure("invalid_argument", "Health amounts must be between 0 and 1000000");
  const auto change = entity_health::Apply(*g_context, g_base, entity,
      name.ends_with(".damage") ? entity_health::Action::kDamage : entity_health::Action::kHeal,
      float(amount));
  if (!change) throw Failure("unsupported", "The original health operation is unavailable");
  if (refill) return nullptr;
  return {{"before", HealthRecord(change->before)},
          {"after", change->after ? HealthRecord(*change->after) : Json(nullptr)},
          {"removed", change->removed}};
}
Json GameCall(std::string_view name, const Json& args) {
  if (!g_context || !g_base) throw Failure("not_ready", "Game APIs are available during mod callbacks");
  const bool ready = g_ready && g_world == g_world_changes.load();
  if (name == "world.status")
    return {{"ready", ready}, {"location", ready ? saves::Where(*g_context, g_base) : ""},
            {"epoch", g_world}, {"level_root", ready ? spawn_sources::LevelRoot(g_base) : ""},
            {"streaming", !ready || spawn_sources::Streaming(g_base)}};
  if (name == "text.lookup") {
    const auto id = args.at("id").get<std::string>();
    uint32_t key;
    const auto parsed = std::from_chars(id.data(), id.data() + id.size(), key, 16);
    if (id.size() != 8 || parsed.ec != std::errc() || parsed.ptr != id.data() + id.size())
      throw Failure("invalid_argument", "Game text IDs need eight hexadecimal digits; use the translation catalog to find them");
    const auto database = simpsons::LoadGuestU32(g_base, 0x82D08B0C);
    if (!database) return {{"found", false}, {"text", ""}};
    rex::CallFrame frame(*g_context);
    frame.ctx.r1.u64 -= 0x80;
    const auto out = frame.ctx.r1.u32 + 0x60;
    simpsons::StoreGuestU32(g_base, out, 0);
    simpsons::StoreGuestU32(g_base, out + 4, 0);
    frame.ctx.r3.u64 = database; frame.ctx.r4.u64 = key;
    frame.ctx.r5.u64 = out; frame.ctx.r6.u64 = out + 4;
    sub_8282AC20(frame, g_base);
    const auto pointer = simpsons::LoadGuestU32(g_base, out);
    if (!(frame.ctx.r3.u32 & 0xFF) || !pointer) return {{"found", false}, {"text", ""}};
    const bool wide = simpsons::GuestToHost(g_base, out + 4)[0] != 0;
    constexpr uint16_t cp1252[] = {0x20AC,0x81,0x201A,0x192,0x201E,0x2026,0x2020,0x2021,
      0x2C6,0x2030,0x160,0x2039,0x152,0x8D,0x17D,0x8F,0x90,0x2018,0x2019,0x201C,
      0x201D,0x2022,0x2013,0x2014,0x2DC,0x2122,0x161,0x203A,0x153,0x9D,0x17E,0x178};
    std::u16string text;
    const auto* bytes = simpsons::GuestToHost(g_base, pointer);
    for (size_t i = 0; i < 65536; ++i) {
      uint16_t c = wide ? uint16_t(bytes[i * 2]) << 8 | bytes[i * 2 + 1] : bytes[i];
      if (!c) return {{"found", true}, {"text", rex::string::to_utf8(text)}};
      if (!wide && c >= 0x80 && c < 0xA0) c = cp1252[c - 0x80];
      text += char16_t(c);
    }
    throw Failure("resource_limit", "Game text exceeds 65536 characters");
  }
  if (name == "camera.status") return {{"free", FreecamActive()}, {"photo", PhotoModeActive()}};
  if (name == "camera.pose") {
    const auto pose =
        g_ready && g_world == g_world_changes.load() ? ReadCameraPose() : std::nullopt;
    return pose ? SaveCameraPose(*pose) : Json();
  }
  if (name == "characters.list") {
    Json result = Json::array();
    for (const auto& choice : characters::Choices()) result.push_back({{"id", choice.name}, {"name", choice.label}});
    return result;
  }
  if (name == "settings.get") {
    const std::string key = args.at("name");
    const auto* info = rex::cvar::GetFlagInfo(key);
    if (!info) throw Failure("unknown_setting", "There is no setting named " + key);
    return {{"name", key}, {"value", rex::cvar::GetFlagByName(key)}, {"description", info->description},
             {"restart", info->lifecycle == rex::cvar::Lifecycle::kInitOnly ||
                         info->lifecycle == rex::cvar::Lifecycle::kRequiresRestart}};
  }
  if (name == "input.players") {
    auto* input = dynamic_cast<rex::input::InputSystem*>(g_game->input_system());
    if (!input) throw Failure("not_ready", "Input system is unavailable");
    Json result = Json::array();
    for (uint32_t i = 0; i < 2; ++i) {
      const auto device = input->GetActiveDevice(i);
      const auto override = rex::cvar::GetFlagByName("player" + std::to_string(i + 1) + "_prompts");
      const auto style = rex::input::ParsePromptStyle(override, device.prompts);
      const char* families[] = {"xbox", "playstation", "nintendo", "steamdeck", "keyboard"};
      result.push_back({{"player", i}, {"assignment", input->GetAssignment(i)},
                        {"prompts", override}, {"prompt_family", families[size_t(style)]},
                        {"device", device.id}, {"name", device.name},
                        {"kind", device.id.empty() ? "none" : device.kind == rex::input::DeviceKind::kKeyboard
                            ? "keyboard" : device.kind == rex::input::DeviceKind::kTouch ? "touch" : "gamepad"}});
    }
    return result;
  }
  if (name == "input.state") {
    return modkit_input::State(InputPlayer(args));
  }
  if (name == "players.controlled") {
    const auto input = InputPlayer(args);
    if (!ready) return nullptr;
    uint32_t slot = 0;
    for (const auto& player : players::List(g_base)) {
      if (player.input_player == int32_t(input)) return PlayerRecord(player, slot);
      ++slot;
    }
    return nullptr;
  }
  if (name == "players.list") {
    Json result = Json::array();
    if (!ready) return result;
    uint32_t slot = 0;
    for (const auto& player : players::List(g_base))
      result.push_back(PlayerRecord(player, slot++));
    return result;
  }
  if (!ready && name != "assets.search" && name != "assets.templates")
    throw Failure("not_ready", "Load a level before using this operation");
  if (name == "players.teleport") {
    if (!players::MoveTo(*g_context, g_base, Player(args.at("player")), Position(args.at("position"))))
      throw Failure("unsupported", "This player's position setter is unavailable");
    return nullptr;
  }
  if (name == "players.health" || name == "players.damage" || name == "players.heal" ||
      name == "players.refill_health")
    return HealthCall(Player(args.at("player")).entity, name, args);
  if (name == "scores.get" || name == "scores.set" || name == "scores.issue") {
    if (!scores::Ready(g_base)) throw Failure("not_ready", "ScoreKeeper is unavailable");
    const std::string score = args.at("name");
    if (score.empty() || score.size() > 200) throw Failure("invalid_argument", "A score needs a valid name");
    if (name == "scores.get") return scores::Get(*g_context, g_base, score);
    if (name == "scores.set") {
      const float value = args.at("value").get<float>();
      if (!std::isfinite(value)) throw Failure("invalid_argument", "Score value is out of range");
      scores::Set(*g_context, g_base, score, value);
    } else scores::Issue(*g_context, g_base, score);
    return nullptr;
  }
  if (name == "characters.apply") {
    const std::string a = args.at("player1"), b = args.at("player2");
    for (const auto& value : {a, b})
      if (std::none_of(characters::Choices().begin(), characters::Choices().end(), [&](const auto& c) {return c.name == value;}))
        throw Failure("invalid_argument", "Unknown character template: " + value);
    characters::Apply(a, b);
    return nullptr;
  }
  if (name == "world.catalog") {
    Json result = Json::array();
    const auto streams = spawn_sources::LoadedStreams(g_base);
    for (const auto* current : spawner::CurrentCatalog(g_base)) {
      const auto& item = *current;
      const auto source = streams.find(item.record.source_layer);
      result.push_back({{"id", Handle("template", item.record.entity, item.record, true)},
                        {"name", item.name},
                        {"category", item.category},
                        {"seen", item.seen},
                        {"class", item.record.name},
                        {"source_guid", simpsons::modkit::ResourceId(item.record.source_guid)},
                        {"model_resource", item.model_resource},
                        {"source_stream", source == streams.end() ? "" : source->second}});
    }
    return result;
  }
  if (name == "assets.search") {
    auto resource_id = args.value("resource_id", std::string());
    if (!resource_id.empty()) {
      if (resource_id.size() != 32)
        throw Failure("invalid_argument", "A resource ID needs 32 hexadecimal digits");
      for (auto& c : resource_id) {
        if (c >= 'a' && c <= 'f')
          c -= 'a' - 'A';
        if (!(c >= '0' && c <= '9') && !(c >= 'A' && c <= 'F'))
          throw Failure("invalid_argument", "A resource ID needs 32 hexadecimal digits");
      }
    }
    const auto snapshot = spawner::Library();
    Json result = {{"ready", snapshot->ready}, {"files", snapshot->files}, {"total_files", snapshot->total_files},
                   {"categories", snapshot->categories}, {"errors", snapshot->errors}, {"items", Json::array()}, {"total", 0}};
    const auto category = args.value("category", std::string());
    auto search = args.value("text", std::string());
    for (const auto* key : {"offset", "limit"})
      if (args.contains(key) && !args.at(key).is_number_integer())
        throw Failure("invalid_argument", "Asset paging values must be whole numbers");
    const auto offset = args.value("offset", int64_t(0)), limit = args.value("limit", int64_t(80));
    if (offset < 0 || limit < 1 || limit > 200 || search.size() > 160)
      throw Failure("invalid_argument", "Asset queries need an offset, 1 to 200 results, and at most 160 search bytes");
    const auto lower = [](std::string text) {
      for (char& c : text) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
      return text;
    };
    search = lower(search);
    int64_t total = 0;
    for (const auto& asset : snapshot->assets) {
      if ((!category.empty() && asset.category != category) ||
          (!search.empty() &&
           lower(asset.name + " " + asset.native_name + " " + asset.source).find(search) == std::string::npos) ||
          (!resource_id.empty() &&
           std::none_of(asset.locations.begin(), asset.locations.end(),
                        [&](const auto& location) { return location.resource_id == resource_id; })))
        continue;
      if (total >= offset && int64_t(result["items"].size()) < limit) {
        Json locations = Json::array();
        for (const auto& location : asset.locations)
          locations.push_back({{"stream", location.stream},
                               {"entry", location.entry},
                               {"offset", location.offset},
                               {"bytes", location.bytes},
                               {"container", location.container},
                               {"resource_id", location.resource_id}});
        result["items"].push_back({{"id", asset.id}, {"name", asset.name}, {"category", asset.category},
                                  {"native_name", asset.native_name}, {"native_class", asset.native_class},
                                  {"object_count", asset.objects.size()},
                                  {"source", asset.source}, {"locations", std::move(locations)}});
      }
      ++total;
    }
    result["total"] = total;
    return result;
  }
  if (name == "assets.templates") {
    const auto snapshot = spawner::Library();
    Json result = {{"ready", snapshot->ready}, {"total", 0}, {"items", Json::array()}};
    if (!snapshot->ready)
      return result;
    const auto id = args.at("asset").get<std::string>();
    size_t index = 0;
    if (!id.starts_with("asset:"))
      throw Failure("invalid_argument", "Choose an asset id from assets.search");
    const auto parsed = std::from_chars(id.data() + 6, id.data() + id.size(), index);
    if (parsed.ec != std::errc() || parsed.ptr != id.data() + id.size() || index >= snapshot->assets.size())
      throw Failure("invalid_argument", "This asset id is not in the current library");
    for (const auto* key : {"offset", "limit"})
      if (args.contains(key) && !args.at(key).is_number_integer())
        throw Failure("invalid_argument", "Template paging values must be whole numbers");
    const auto offset = args.value("offset", int64_t(0)), limit = args.value("limit", int64_t(40));
    if (offset < 0 || limit < 1 || limit > 100)
      throw Failure("invalid_argument", "Template queries need an offset and 1 to 100 results");
    const auto classes = g_base ? spawner::PlaceableClasses(g_base) : std::map<uint32_t, std::string>();
    std::map<std::pair<std::string, uint32_t>, const spawner::Spawnable*> resident;
    if (ready)
      for (const auto* item : spawner::CurrentCatalog(g_base))
        resident.emplace(std::make_pair(ResourceId(item->record.source_guid), item->record.hash), item);
    const auto& asset = snapshot->assets[index];
    result["total"] = asset.objects.size();
    const auto end = std::min<uint64_t>(asset.objects.size(), uint64_t(offset) + uint64_t(limit));
    for (uint64_t i = uint64_t(offset); i < end; ++i) {
      const auto& object = snapshot->objects.at(asset.objects[size_t(i)]);
      const auto type = classes.find(object.hash);
      const auto found = resident.find({object.guid, object.hash});
      const auto* model = snapshot->FindNative(object.model_resource);
      std::string handle;
      if (found != resident.end() && (object.model_resource.empty() ||
                                     found->second->model_resource == object.model_resource))
        handle = Handle("template", found->second->record.entity, found->second->record, true);
      result["items"].push_back({{"source_guid", object.guid}, {"stream", object.stream},
                                  {"class_hash", object.hash},
                                  {"class", type != classes.end() ? type->second : std::string()},
                                  {"supported", type != classes.end()},
                                  {"name", model ? model->name : asset.name},
                                  {"model_resource", object.model_resource},
                                  {"resident_template", handle}});
    }
    return result;
  }
  throw Failure("unsupported", "Game adapter is not implemented: " + std::string(name));
}

Json EntityCall(const HostOwner& owner, std::string_view name, const Json& args) {
  if (!g_context || !g_base || !g_ready || g_world != g_world_changes.load())
    throw Failure("not_ready", "Load a level before using owned objects");
  if (name == "world.tagged") {
    const auto tag = args.at("tag").get<std::string>();
    if (!simpsons::modkit::ObjectTag(tag)) throw Failure("invalid_argument", "Choose a valid lowercase object tag");
    for (const auto& entry : g_entities->List(owner))
      if (entry.description.value("tag", std::string()) == tag) {
        const auto address = spawner::Address(entry.lifetime);
        if (address && spawner::Readable(address, 72)) return Handle("entity", address);
      }
    return nullptr;
  }
  if (name == "world.owned") {
    Json result = Json::array();
    for (const auto& entry : g_entities->List(owner)) {
      const auto address = spawner::Address(entry.lifetime);
      if (address && spawner::Readable(address, 72))
        result.push_back({{"id", Handle("entity", address)}, {"class", entry.description.at("class")},
                          {"tag", entry.description.value("tag", std::string())}});
    }
    return result;
  }
  if (name == "world.destroy") {
    const auto& entity = ResolveHandle(args.at("entity"), "entity");
    return g_entities->Destroy(owner, entity.lifetime);
  }
  if (name == "entities.health" || name == "entities.damage" || name == "entities.heal") {
    const auto& entity = ResolveHandle(args.at("entity"), "entity");
    g_entities->Get(owner, entity.lifetime);
    return HealthCall(entity.address, name, args);
  }
  if (name == "entities.render_state" || name == "entities.set_visible") {
    const auto entity = ResolveHandle(args.at("entity"), "entity");
    auto entry = g_entities->Get(owner, entity.lifetime);
    auto state = entity_render::Get(g_base, entity.address);
    if (name == "entities.set_visible") {
      if (!state)
        throw Failure("unsupported", "This object's visibility layout is unavailable");
      const bool hidden = !args.at("visible").get<bool>();
      if (hidden != entry.render_hidden) {
        if (hidden && state->hide_count == 255)
          throw Failure("resource_limit", "This object's native hide counter is full");
        if (!entity_render::Change(*g_context, g_base, entity.address, hidden))
          throw Failure("unsupported", "This object's visibility method is unavailable");
        // Scene/model callbacks can remove an object. Do not keep a lease on
        // its successor, or read an allocation after its native destructor.
        if (spawner::Lifetime(entity.address) != entity.lifetime)
          return nullptr;
        state = entity_render::Get(g_base, entity.address);
        g_entities->SetRenderHidden(owner, entity.lifetime, hidden && state && state->hide_count);
        entry = g_entities->Get(owner, entity.lifetime);
      }
    }
    if (!state)
      return nullptr;
    return {{"hidden_by_mod", entry.render_hidden},
            {"hide_count", state->hide_count},
            {"has_model", state->has_model},
            {"scene_attached", state->scene_attached}};
  }
  if (name == "entities.transform" || name == "entities.move" || name == "entities.set_transform") {
    const auto& entity = ResolveHandle(args.at("entity"), "entity");
    g_entities->Get(owner, entity.lifetime);
    if (name == "entities.set_transform") {
      const auto& value = args.at("transform");
      const entity_transform::Transform transform{Position(value.at("right")), Position(value.at("up")),
          Position(value.at("forward")), Position(value.at("position"))};
      if (!entity_transform::Valid(transform))
        throw Failure("invalid_argument", "Choose a nonsingular transform with native float32 vector lengths");
      if (!entity_transform::Set(*g_context, g_base, entity.address, transform))
        throw Failure("unsupported", "This object's full matrix setter or parent transform is unavailable");
      return nullptr;
    }
    if (name == "entities.move") {
      if (!entity_transform::Move(*g_context, g_base, entity.address, Position(args.at("position"))))
        throw Failure("unsupported", "This object's position setter or parent transform is unavailable");
      return nullptr;
    }
    const auto transform = entity_transform::Get(*g_context, g_base, entity.address);
    if (!transform) throw Failure("unsupported", "This object's transform is unavailable");
    const auto vector = [](const entity_transform::Vec3& v) -> Json {
      return {{"x", v[0]}, {"y", v[1]}, {"z", v[2]}};
    };
    return {{"position", vector(transform->position)}, {"right", vector(transform->right)},
            {"up", vector(transform->up)}, {"forward", vector(transform->forward)}};
  }
  if (name == "world.spawn") {
    g_entities->CheckCapacity(owner);
    const auto tag = args.value("tag", std::string());
    if (args.contains("tag")) g_entities->CheckTag(owner, tag);
    const auto record = ResolveHandle(args.at("template"), "template").record;
    const float radius = args.value("radius", 1.0f);
    if (!std::isfinite(radius) || radius <= 0 || radius > 1000) throw Failure("invalid_argument", "Invalid spawn radius");
    const uint32_t entity = spawner::Clone(*g_context, g_base, record, Position(args.at("position")), radius, owner.mod);
    if (!entity) throw Failure("spawn_failed", "The original entity factory rejected this template");
    const auto lifetime = spawner::Lifetime(entity);
    try {
      const auto id = Handle("entity", entity);
      Json description = {{"guid", Guid(entity)},
          {"vtable", simpsons::LoadGuestU32(g_base, entity)}, {"class", record.name}};
      if (!tag.empty()) description["tag"] = tag;
      g_entities->Add(owner, lifetime, description);
      spawn_sources::RetainOwner(record.source_layer, owner);
      return id;
    } catch (...) {
      spawner::Destroy(*g_context, g_base, lifetime);
      throw;
    }
  }
  throw Failure("unsupported", "Unknown owned-object operation");
}

void RetireEntities() {
  if (!g_entities || !g_mods) return;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2);
  size_t retired = 0;
  for (; retired < 16 &&
       (!retired || std::chrono::steady_clock::now() < deadline); ++retired) {
    const auto deaths = g_entities->TakeDeaths(1);
    if (deaths.empty()) break;
    const auto& entry = deaths.front();
    std::string handle;
    for (const auto& [id, value] : g_handles)
      if (value.kind == "entity" && value.lifetime == entry.lifetime) { handle = id; break; }
    if (handle.empty()) {
      const auto address = spawner::Address(entry.lifetime);
      if (address && spawner::Readable(address, 72)) handle = Handle("entity", address);
      else {
        // The death callback may have deleted the entity before its first
        // query in a replacement runtime. Issue an already-stale identity so
        // death and subsequent removal still refer to the same notification.
        std::ostringstream id; id << "entity:" << std::hex << ++g_next_handle;
        handle = id.str();
        Identity identity; identity.kind = "entity"; identity.lifetime = entry.lifetime;
        g_handles.emplace(handle, std::move(identity));
      }
    }
    g_mods->EntityDied(entry.owner, {{"entity", handle}});
  }
  const auto notice = [&](const OwnedEntities::Entry& entry, std::string reason) {
    std::string handle;
    for (const auto& [id, value] : g_handles)
      if (value.kind == "entity" && value.lifetime == entry.lifetime) {
        handle = id;
        break;
      }
    std::erase_if(g_handles, [&](const auto& item) { return item.second.lifetime == entry.lifetime; });
    if (!handle.empty())
      g_mods->EntityRemoved(entry.owner, {{"entity", handle}, {"reason", reason}});
  };
  for (const auto lifetime : g_entities->Lifetimes()) {
    if (retired >= 16 || (retired && std::chrono::steady_clock::now() >= deadline)) break;
    if (!spawner::Address(lifetime)) {
      if (const auto entry = g_entities->Forget(lifetime))
        notice(*entry, entry->pending ? "removed" : "game");
      ++retired;
    }
  }
  // Include notification callbacks in the same budget. Dead entries stay
  // tracked until delivered, providing backpressure instead of dropping events
  // or dispatching thousands of callbacks after a large encounter disappears.
  for (; retired < 16 &&
       (!retired || std::chrono::steady_clock::now() < deadline); ++retired) {
    const auto pending = g_entities->TakePending(1);
    if (pending.empty()) break;
    const auto& entry = pending.front();
    if (spawner::Destroy(*g_context, g_base, entry.lifetime)) {
      g_entities->Forget(entry.lifetime);
      notice(entry, "removed");
    } else {
      // A class may defer its final removal. Keep ownership until the base
      // destructor runs; never send its destruction request a second time.
      REXLOG_WARN("[modkit] object cleanup is awaiting the class destructor for {}", entry.owner.mod);
    }
  }
  const auto deferred = g_entities->Lifetimes();
  std::erase_if(g_handles, [&](const auto& item) {
    return spawner::Lifetime(item.second.address) != item.second.lifetime &&
        !std::binary_search(deferred.begin(), deferred.end(), item.second.lifetime);
  });
}

HostServices ResourceServices() {
  auto resources = std::make_shared<Resources>();
  resources->Bind("damage_protection", {
      [] { return Json(players::GodMode()); },
      [](const Json& value) { players::SetGodMode(value.get<bool>()); },
      players::GodModeRevision,
      [](const Json& value) {
        if (!value.is_boolean()) throw Failure("invalid_argument", "Damage protection must be boolean");
      }});
  resources->Bind("knockdowns", {
      [] { return players::SaveRagdollSettings(players::ReadRagdollSettings()); },
      [](const Json& value) { players::ApplyRagdollSettings(players::ParseRagdollSettings(value)); },
      players::RagdollSettingsRevision,
      [](const Json& value) { players::ParseRagdollSettings(value); }});
  resources->Bind("camera",
                  {[] { return SaveCameraRequest(ReadFreecamRequests()); },
                   [](const Json& value) { ApplyFreecamRequests(ParseCameraRequest(value)); },
                   FreecamRequestsRevision,
                   [](const Json& value) {
                     if (SaveCameraRequest(ParseCameraRequest(value)) != value)
                       throw Failure("invalid_argument",
                                     "Saved camera requests must use canonical angles");
                   },
                   true, [] { return SaveCameraRequest(CaptureFreecamRequests()); }});
  g_resources = resources;
  auto entities = std::make_shared<OwnedEntities>();
  g_entities = entities;
  {
    std::lock_guard lock(g_entity_hook_mutex);
    g_hook_entities = entities;
  }
  HostServices host;
  host.call = [resources](const HostOwner& owner, std::string_view name, const Json& args) -> Json {
    if (name == "physics.knockdowns") {
      const auto settings = players::ParseRagdollSettings(args.at("options"));
      resources->Set(owner, "knockdowns", players::SaveRagdollSettings(settings));
      return nullptr;
    }
    if (name == "physics.release_knockdowns") {
      resources->Release(owner, "knockdowns");
      return nullptr;
    }
    if (name == "assets.release_source")
      return spawn_sources::ReleaseOwner(owner);
    if (name == "assets.cancel_source")
      return spawn_sources::CancelOwner(owner);
    if (name == "assets.source_streams")
      return spawn_sources::Get().streams;
    if (name == "assets.source_status") {
      const auto state = spawn_sources::Get(owner);
      const auto stream = spawn_sources::Claimed(owner);
      constexpr const char* phases[] = {"idle", "preparing", "loading", "ready", "releasing", "failed"};
      return {{"phase", phases[int(state.phase)]}, {"stream", stream},
               {"owned", !stream.empty()},
               {"ready", !stream.empty() && stream == state.path && state.phase == spawn_sources::Phase::ready},
               {"message", state.message}, {"files", state.files}, {"loaded", state.loaded},
               {"definitions", state.definitions}, {"streams", state.streams},
               {"pending_stream", state.pending_stream}};
    }
    if (name == "assets.load_source") {
      if (!g_context || !g_base || !g_ready || g_world != g_world_changes.load())
        throw Failure("not_ready", "Load a level before requesting an object source");
      const auto stream = args.at("stream").get<std::string>();
      const auto path = RelativePath(stream);
      if (stream.size() > 200 || path.extension() != ".str" || stream.find('\\') != std::string::npos)
        throw Failure("invalid_argument", "Choose a relative .str path from assets.templates");
      if (spawn_sources::Get().streams.size() >= simpsons::modkit::source_state::kMaxSources &&
          !spawn_sources::IsReady(stream))
        throw Failure("resource_limit", "The native source session retains at most eight archives");
      if (!spawn_sources::Request(stream, owner))
        throw Failure("busy", "Wait for the current object source operation to finish");
      return true;
    }
    if (name == "world.spawn" || name == "world.owned" || name == "world.destroy" || name == "world.tagged" ||
        name == "entities.transform" || name == "entities.move" || name == "entities.set_transform" ||
        name == "entities.render_state" || name == "entities.set_visible" ||
        name == "entities.health" || name == "entities.damage" || name == "entities.heal")
      return EntityCall(owner, name, args);
    if (name == "camera.release") {
      resources->Release(owner, "camera");
      return nullptr;
    }
    if (name == "players.god_mode" || name == "camera.free" || name == "camera.photo" ||
        name == "camera.set_pose") {
      if (!g_context || !g_base || !g_ready || g_world != g_world_changes.load())
        throw Failure("not_ready", "Load a level before changing a mod-owned host resource");
      if (name == "players.god_mode") {
        resources->Set(owner, "damage_protection", args.at("enabled").get<bool>());
      } else {
        auto request = ReadFreecamRequests();
        if (name == "camera.set_pose") {
          if (!ReadCameraPose())
            throw Failure("not_ready", "No level camera is available in this view");
          request.pose = ParseCameraPose(args.at("pose"));
          request.free = true;
        } else {
          const bool enabled = args.at("enabled").get<bool>();
          if (name == "camera.free") {
            request.free = enabled;
            if (!enabled) {
              request.photo = false;
              request.pose.reset();
            }
          } else {
            request.photo = enabled;
            if (enabled)
              request.free = true;
          }
        }
        resources->Set(owner, "camera", SaveCameraRequest(request));
        if (name == "camera.set_pose")
          return SaveCameraPose(*request.pose);
      }
      return nullptr;
    }
    return GameCall(name, args);
  };
  host.save = [resources, entities](const HostOwner& owner) {
    auto state = resources->Save(owner);
    Json owned = Json::array();
    if (g_world == g_world_changes.load())
      for (const auto& entry : entities->List(owner))
        if (spawner::Address(entry.lifetime)) owned.push_back(entry.Save());
    if (!owned.empty()) state["entities"] = std::move(owned);
    if (const auto source = spawn_sources::Claimed(owner); !source.empty())
      state["source_stream"] = source;
    return state;
  };
  host.validate = [resources](const std::vector<Json>& states) {
    std::vector<Json> bindings, objects;
    for (auto state : states) {
      if (state.contains("source_stream")) {
        if (!state.at("source_stream").is_string())
          throw Failure("save_incompatible", "Saved object source must be a stream path");
        const auto source = state.at("source_stream").get<std::string>();
        if (source.empty() || source.size() > 200 || RelativePath(source).extension() != ".str" ||
            source.find('\\') != std::string::npos)
          throw Failure("save_incompatible", "Saved object source is invalid");
        state.erase("source_stream");
      }
      objects.push_back(state.value("entities", Json::array()));
      state.erase("entities");
      bindings.push_back(std::move(state));
    }
    OwnedEntities::Validate(objects);
    resources->Validate(bindings);
  };
  host.restore = [resources, entities](const HostOwner& owner, Json state, bool checkpoint) {
    const auto source = state.value("source_stream", std::string());
    state.erase("source_stream");
    if (!checkpoint && !source.empty() && !spawn_sources::Request(source, owner))
      throw Failure("save_incompatible", "The saved object source cannot be retained by this mod");
    const auto owned = state.value("entities", Json::array());
    state.erase("entities");
    resources->Restore(owner, state, checkpoint);
    entities->Restore(owner, owned, checkpoint, [&](const Json& value) {
      if (!g_context || !g_base) return uint64_t(0);
      const auto address = spawner::FindOwned(*g_context, g_base, owner.mod,
          value.at("guid").get<std::array<uint32_t, 4>>(), value.at("vtable").get<uint32_t>());
      if (!address) return uint64_t(0);
      if (g_restoring_entities) return spawner::Observe(address);
      // A reload may transfer only this mod's retiring objects. It cannot
      // adopt another mod's entities or an unowned debug-menu spawn.
      const auto lifetime = spawner::Lifetime(address);
      for (const auto& entry : entities->All())
        if (entry.lifetime == lifetime && entry.owner.mod == owner.mod &&
            entry.pending && entry.description == value) return lifetime;
      return uint64_t(0);
    });
  };
  host.release = [resources, entities](const HostOwner& owner) {
    spawn_sources::ReleaseOwner(owner);
    entities->Release(owner);
    resources->Release(owner);
  };
  return host;
}

void ReapCheckpoint() {
  if (g_checkpoint_job && g_checkpoint_cancelling && g_checkpoint_job->ready()) {
    g_checkpoint_job.reset();
    g_checkpoint_folder.clear();
    g_checkpoint_cancelling = false;
  }
}
void PollReload() {
  if (!g_reload_job || !g_reload_job->ready()) return;
  if (g_reload_cancelling) {
    g_reload_job.reset();
    g_reload_cancelling = false;
    g_reload_status = {{"state", "cancelled"}, {"generation", g_reload_generation}};
    return;
  }
  if (g_checkpoint_pending || g_prepared) return;
  if (g_entities && g_entities->HasPendingDeaths()) return;
  try {
    const auto sources = g_reload_job->Take();
    auto replacement = g_mods->PrepareReload(sources);
    // No guest memory or mounted assets change. Retire old callbacks before
    // applying the replacement's owned host requests and reload notification.
    g_mods.swap(replacement);
    modkit_input::Reset();
    g_handles.clear();
    replacement.reset();
    g_mods->NotifyReloaded();
    g_reload_status = {{"state", "applied"}, {"generation", g_reload_generation}};
    REXLOG_INFO("[modkit] code reload {} applied", g_reload_generation);
  } catch (const Failure& error) {
    g_reload_status = {{"state", "failed"}, {"generation", g_reload_generation},
                       {"code", error.code}, {"error", error.what()}};
    REXLOG_ERROR("[modkit] code reload failed: {}", error.what());
  } catch (const std::exception& error) {
    g_reload_status = {{"state", "failed"}, {"generation", g_reload_generation},
                       {"code", "reload_failed"}, {"error", error.what()}};
    REXLOG_ERROR("[modkit] code reload failed: {}", error.what());
  }
  g_reload_job.reset();
}

void UpdateEditorView(bool force = false) {
  if (!g_context || !g_base || !g_ready || g_world != g_world_changes.load()) return;
  {
    std::lock_guard lock(g_view_reset_mutex);
    for (const auto lifetime : g_view_resets) g_view_hidden.erase(lifetime);
    if (!g_view_resets.empty()) force = true;
    g_view_resets.clear();
  }
  const auto revision = spawner::InstanceRevision();
  const auto now = std::chrono::steady_clock::now();
  if (!force && revision == g_view_revision && now - g_view_checked < std::chrono::milliseconds(250)) return;
  g_view_revision = revision;
  g_view_checked = now;
  if (g_view_isolate && !spawner::Address(g_view_isolate)) {
    g_view_mode = "all";
    g_view_isolate = 0;
  }
  for (auto entry = g_view_hidden.begin(); entry != g_view_hidden.end();) {
    const auto address = spawner::Address(entry->first);
    if (!address) entry = g_view_hidden.erase(entry);
    else if (g_view_mode == "all" || entry->first == g_view_isolate) {
      // Release only this editor's lease, preserving scripted hide requests.
      if (entity_render::Change(*g_context, g_base, address, false)) entry = g_view_hidden.erase(entry);
      else ++entry;
    } else ++entry;
  }
  g_view_active = g_view_mode != "all" || !g_view_hidden.empty();
  if (g_view_mode == "all") return;
  g_view_supported = g_view_unsupported = 0;
  for (const auto& record : spawner::Instances(g_base)) {
    const auto state = entity_render::Get(g_base, record.entity);
    if (!state) { ++g_view_unsupported; continue; }
    if (!state->has_model) continue;
    ++g_view_supported;
    if (record.lifetime == g_view_isolate || g_view_hidden.contains(record.lifetime)) continue;
    if (entity_render::Change(*g_context, g_base, record.entity, true) &&
        spawner::Address(record.lifetime) == record.entity)
      g_view_hidden[record.lifetime] = true;
  }
}

Json EditorViewStatus() {
  return {{"mode", g_view_mode}, {"hidden", g_view_hidden.size()},
          {"supported", g_view_supported}, {"unsupported", g_view_unsupported},
          {"booting", g_editor_boot}, {"preview", REXCVAR_GET(mods_editor_preview)}};
}

Json MapEntityToJson(const spawner::MapEntityInfo& info) {
  return {
      {"lifetime", spawner::Lifetime(info.record.entity)},
      {"entity", info.record.entity},
      {"hash", info.record.hash},
      {"name", info.name},
      {"class_name", info.record.name},
      {"category", info.category},
      {"model_resource", info.model_resource},
      {"position", {info.position[0], info.position[1], info.position[2]}},
      {"distance_to_camera", info.distance},
      {"is_spawned_clone", info.is_spawned},
      {"is_character", info.is_character},
      {"has_transform", info.has_transform},
      {"hidden", info.hidden},
      {"ai_disabled", info.ai_disabled},
  };
}

Json SpoonerStatusJson(bool include_entities) {
  Json out = {
      {"enabled", spawner::SpoonerEnabled()},
      {"grabbing", spawner::SpoonerGrabbing()},
      {"grab_distance", spawner::SpoonerGrabDistance()},
      {"removed_map_entities", spawner::SpoonerRemovedMapEntityCount()},
      {"spawn_with_ai", spawner::SpawnWithAi()},
  };
  if (g_context && g_base) {
    if (const auto sel = spawner::SpoonerSelectedInfo(*g_context, g_base))
      out["selected"] = MapEntityToJson(*sel);
    else
      out["selected"] = nullptr;
    if (const auto hov = spawner::SpoonerHoveredInfo(*g_context, g_base))
      out["hovered"] = MapEntityToJson(*hov);
    else
      out["hovered"] = nullptr;
    if (include_entities) {
      Json arr = Json::array();
      for (const auto& item : spawner::ListMapEntities(*g_context, g_base))
        arr.push_back(MapEntityToJson(item));
      out["entities"] = std::move(arr);
    }
  }
  return out;
}

Json DeveloperRequest(const Json& request) {
  const auto action = request.value("action", std::string());
  if (action == "debug_menu") {
    const auto op = request.value("op", std::string("status"));
    if (op == "open") game_ui::Open(game_ui::Screen::kDebugMenu);
    else if (op == "close") {
      if (game_ui::Current() == game_ui::Screen::kDebugMenu) game_ui::Close();
    } else if (op != "status") throw Failure("invalid_argument", "Unknown debug-menu operation");
    return {{"open", game_ui::Current() == game_ui::Screen::kDebugMenu}};
  }
  if (action == "npc_preview") {
    if (!g_context || !g_base || !g_ready || g_world != g_world_changes.load())
      throw Failure("not_ready", "Wait for the editor level to finish loading");
    const auto op = request.value("op", std::string("status"));
    if (op == "start") {
      const auto record = ResolveHandle(request.at("template").get<std::string>(), "template").record;
      std::optional<spawner::Spawnable> selected;
      for (const auto* item : spawner::CurrentCatalog(g_base))
        if (item->record.source_guid == record.source_guid && item->record.hash == record.hash &&
            item->record.source_layer == record.source_layer) { selected = *item; break; }
      if (!selected || !players::PlayAsNpc(*g_context, g_base, *selected))
        throw Failure("unsupported", "This resident template cannot be used as a costume preview");
    } else if (op == "restore") {
      players::RestoreDefaultCharacter(*g_context, g_base);
    } else if (op != "status") throw Failure("invalid_argument", "Unknown NPC preview operation");
    const auto avatar = players::ActiveNpcAvatar();
    return {{"active", avatar.active}, {"name", avatar.name},
            {"lifetime", avatar.active ? avatar.npc_lifetime : 0},
            {"native_npc_possession", false}, {"npc_abilities", false}};
  }
  if (action == "spooner") {
    if (!g_context || !g_base || !g_ready || g_world != g_world_changes.load())
      throw Failure("not_ready", "Wait for the editor level to finish loading");
    const auto op = request.value("op", std::string("status"));
    bool applied = true;
    if (op == "enable") {
      const bool enabled = request.value("enabled", true);
      spawner::SetSpoonerEnabled(enabled);
    } else if (op == "select") {
      if (request.contains("lifetime")) {
        const auto lifetime = request.at("lifetime").get<uint64_t>();
        applied = lifetime && spawner::DescribeEntity(*g_context, g_base, lifetime).has_value();
        if (applied) spawner::SetSpoonerSelectedLifetime(lifetime);
      } else applied = spawner::SpoonerSelectHovered(*g_context, g_base);
    } else if (op == "grab") {
      applied = spawner::SpoonerToggleGrab(*g_context, g_base);
    } else if (op == "move") {
      applied = spawner::SpoonerMoveSelected(*g_context, g_base, Position(request.at("delta")));
    } else if (op == "rotate") {
      const auto yaw = request.at("delta_yaw").get<float>();
      if (!std::isfinite(yaw)) throw Failure("invalid_argument", "Rotation must be finite");
      applied = spawner::SpoonerRotateSelected(*g_context, g_base, yaw);
    } else if (op == "scale") {
      const auto factor = request.at("factor").get<float>();
      if (!std::isfinite(factor) || factor <= 0)
        throw Failure("invalid_argument", "Scale factor must be finite and positive");
      applied = spawner::SpoonerScaleSelected(*g_context, g_base, factor);
    } else if (op == "clone") {
      applied = spawner::SpoonerCloneSelected(*g_context, g_base) != 0;
    } else if (op == "delete") {
      applied = spawner::SpoonerDeleteSelected(*g_context, g_base);
    } else if (op == "restore_removed") {
      const auto expected = spawner::SpoonerRemovedMapEntityCount();
      applied = spawner::SpoonerRestoreRemovedMapEntities(*g_context, g_base) == expected;
    } else if (op != "status") {
      throw Failure("invalid_argument", "Unknown Spooner operation");
    }
    if (!applied) throw Failure("unsupported", "The selected entity cannot complete this editor operation");
    return SpoonerStatusJson(request.value("include_entities", false));
  }
  if (action == "viewport") {
    if (!g_ready || g_world != g_world_changes.load())
      throw Failure("not_ready", "Wait for the editor level to finish loading");
    const auto mode = request.at("mode").get<std::string>();
    if (mode != "all" && mode != "map" && mode != "isolate")
      throw Failure("invalid_argument", "Choose all, map or isolate viewport layers");
    uint64_t isolate = 0;
    if (mode == "isolate")
      isolate = ResolveHandle(request.at("entity").get<std::string>(), "entity").lifetime;
    g_view_mode = mode;
    g_view_isolate = isolate;
    UpdateEditorView(true);
    return EditorViewStatus();
  }
  if (action == "reload") {
    if (g_reload_job || g_checkpoint_job || g_checkpoint_pending || g_prepared)
      throw Failure("busy", "Finish the pending reload or save restore first");
    g_reload_job = std::make_unique<SourceJob>(g_mods->Sources(),
        g_game->user_data_root() / "mod-storage/.native-images", true);
    g_reload_cancelling = false;
    g_reload_status = {{"state", "preparing"}, {"generation", ++g_reload_generation}};
    return g_reload_status;
  }
  if (action == "cancel_reload") {
    if (g_reload_job) {
      // Request cancellation without joining a file reader on the game thread.
      g_reload_job->Cancel();
      g_reload_cancelling = true;
      g_reload_status = {{"state", "cancelling"}, {"generation", g_reload_generation}};
    }
    return g_reload_status;
  }
  auto result = g_mods->Inspect(request);
  if (action == "inspect") {
    result["viewport"] = EditorViewStatus();
    result["spooner"] = SpoonerStatusJson(false);
    result["reload"] = g_reload_job ? g_reload_job->Status() : g_reload_status;
    if (g_reload_cancelling) result["reload"]["state"] = "cancelling";
    result["reload"]["generation"] = g_reload_generation;
  }
  return result;
}
Json SavedProfile(const fs::path& folder) {
  const auto path = folder / "mods.json";
  if (!fs::is_regular_file(path)) return {{"api", 1}, {"mods", Json::array()}};
  return Json::parse(ReadText(path, 16 * 1024 * 1024));
}

std::string LowerAscii(std::string_view text) {
  std::string out(text);
  for (char& c : out)
    if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
  return out;
}

std::string TrimAscii(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r'))
    text.remove_prefix(1);
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r'))
    text.remove_suffix(1);
  return std::string(text);
}

std::vector<std::string> ExtractQuotedStrings(std::string_view line) {
  std::vector<std::string> out;
  for (size_t i = 0; i < line.size(); ++i) {
    if (line[i] != '"') continue;
    const size_t end = line.find('"', i + 1);
    if (end == std::string_view::npos) break;
    out.emplace_back(line.substr(i + 1, end - i - 1));
    i = end;
  }
  return out;
}

std::string BuildQuickLaunchGameflow(std::string_view script, std::string_view target_spec) {
  const std::string nl = script.find("\r\n") != std::string_view::npos ? "\r\n" : "\n";
  std::vector<std::string> lines;
  for (size_t pos = 0; pos < script.size();) {
    const size_t next = script.find('\n', pos);
    std::string_view line =
        next == std::string_view::npos ? script.substr(pos) : script.substr(pos, next - pos);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    lines.emplace_back(line);
    if (next == std::string_view::npos) break;
    pos = next + 1;
  }

  struct MapEntry {
    int index = 1;
    std::string name, folder, stream;
    std::vector<std::string> lines;
  };
  struct EpisodeBlock {
    size_t start_line = 0, end_line = 0;
    std::string id;
    std::vector<std::string> header_lines;
    std::string standard_mode_line;
    std::vector<MapEntry> entries;
  };

  size_t install_line = lines.size();
  std::vector<EpisodeBlock> episodes;
  for (size_t i = 0; i < lines.size(); ++i) {
    const auto trimmed = TrimAscii(lines[i]);
    if (trimmed.starts_with("--")) continue;
    if (trimmed.find("gameFlowManager:SetGame(") != std::string::npos) {
      install_line = i;
      break;
    }
    if (trimmed.find("NewEpisode(") != std::string::npos) {
      const auto quotes = ExtractQuotedStrings(trimmed);
      if (!quotes.empty()) {
        EpisodeBlock ep;
        ep.start_line = i;
        ep.id = quotes[0];
        episodes.push_back(std::move(ep));
      }
    }
  }
  for (size_t e = 0; e < episodes.size(); ++e) {
    episodes[e].end_line = (e + 1 < episodes.size()) ? episodes[e + 1].start_line : install_line;
    auto& ep = episodes[e];
    int mode_state = 0;  // 0 = before first mode, 1 = in MODE_STANDARD, 2 = in other mode
    int entry_idx = 0;
    for (size_t i = ep.start_line; i < ep.end_line; ++i) {
      const auto trimmed = TrimAscii(lines[i]);
      if (!trimmed.starts_with("--") && trimmed.find("NewMode(") != std::string::npos) {
        const auto quotes = ExtractQuotedStrings(trimmed);
        if (!quotes.empty() && quotes[0] == "MODE_STANDARD") {
          mode_state = 1;
          ep.standard_mode_line = lines[i];
        } else {
          mode_state = 2;
        }
        continue;
      }
      if (mode_state == 0) {
        if (trimmed.find("episode:SetDefault()") == std::string::npos)
          ep.header_lines.push_back(lines[i]);
      } else if (mode_state == 1 && !trimmed.starts_with("--") &&
                 trimmed.find("NewMap(") != std::string::npos) {
        const auto quotes = ExtractQuotedStrings(trimmed);
        MapEntry entry;
        entry.index = ++entry_idx;
        if (quotes.size() >= 3) {
          entry.name = quotes[0];
          entry.folder = quotes[1];
          entry.stream = quotes[2];
        }
        entry.lines.push_back(lines[i]);
        if (trimmed.starts_with("map")) {
          while (i + 1 < ep.end_line) {
            ++i;
            entry.lines.push_back(lines[i]);
            if (TrimAscii(lines[i]).find("mode:AddEntry(") != std::string::npos) break;
          }
        }
        ep.entries.push_back(std::move(entry));
      }
    }
  }

  std::string spec = LowerAscii(TrimAscii(target_spec));
  int wanted_index = 1;
  if (const auto colon = spec.rfind(':'); colon != std::string::npos) {
    int parsed = 0;
    const auto tail = std::string_view(spec).substr(colon + 1);
    if (std::from_chars(tail.data(), tail.data() + tail.size(), parsed).ec == std::errc() &&
        parsed >= 1) {
      wanted_index = parsed;
      spec.resize(colon);
      if (spec.ends_with(":mode_standard"))
        spec.resize(spec.size() - std::string_view(":mode_standard").size());
    }
  }
  if (spec.ends_with(".str")) spec.resize(spec.size() - 4);

  const EpisodeBlock* chosen_ep = nullptr;
  const MapEntry* chosen_entry = nullptr;
  for (const auto& ep : episodes) {
    for (const auto& entry : ep.entries) {
      if (entry.index != wanted_index) continue;
      std::string stream_stem = LowerAscii(entry.stream);
      if (stream_stem.ends_with(".str")) stream_stem.resize(stream_stem.size() - 4);
      if (LowerAscii(ep.id) == spec || LowerAscii(entry.folder) == spec || stream_stem == spec ||
          LowerAscii(entry.name) == spec) {
        chosen_ep = &ep;
        chosen_entry = &entry;
        break;
      }
    }
    if (chosen_ep) break;
  }
  if (!chosen_ep || !chosen_entry)
    throw Failure("invalid_argument", "Unknown quick_launch_level target: " + std::string(target_spec));

  std::string out;
  for (size_t i = 0; i < chosen_ep->start_line; ++i) {
    if (TrimAscii(lines[i]).starts_with("episode:SetDefault()"))
      out += "\t-- episode:SetDefault()" + nl;
    else
      out += lines[i] + nl;
  }
  if (!chosen_ep->header_lines.empty()) {
    out += chosen_ep->header_lines[0] + nl;
    out += "\tepisode:SetDefault()" + nl;
    for (size_t i = 1; i < chosen_ep->header_lines.size(); ++i)
      out += chosen_ep->header_lines[i] + nl;
  }
  out += chosen_ep->standard_mode_line + nl;
  for (const auto& line : chosen_entry->lines)
    out += line + nl;
  out += nl;
  for (size_t i = chosen_ep->end_line; i < lines.size(); ++i) {
    if (TrimAscii(lines[i]).starts_with("episode:SetDefault()"))
      out += "\t-- episode:SetDefault()" + nl;
    else
      out += lines[i] + nl;
  }
  return out;
}
}  // namespace

Context::Context(PPCContext& ctx, uint8_t* base)
    : previous_(g_context), previous_base_(g_base) { g_context = &ctx; g_base = base; }
Context::~Context() { g_context = previous_; g_base = previous_base_; }

void Prepare(rex::Runtime* runtime) {
  std::lock_guard lock(g_mutex);
  g_game = runtime;
  const bool developer = REXCVAR_GET(mods_developer);
  const std::string quick_launch = REXCVAR_GET(quick_launch_level);
  if (REXCVAR_GET(mods_disabled) && !developer && quick_launch.empty()) return;
  const auto root = REXCVAR_GET(mods_root).empty() ? runtime->user_data_root() / "mods"
                                                : Utf8Path(REXCVAR_GET(mods_root));
  const auto profile_path = REXCVAR_GET(mods_profile_file).empty()
      ? Inside(root / "profiles", REXCVAR_GET(mods_profile) + ".json")
      : fs::absolute(Utf8Path(REXCVAR_GET(mods_profile_file)));
  std::vector<std::string> enabled;
  if (!REXCVAR_GET(mods_disabled) && fs::is_regular_file(profile_path)) {
    const auto profile = Json::parse(ReadText(profile_path, 256 * 1024));
    enabled = profile.at("enabled").get<std::vector<std::string>>();
  }
  if (enabled.empty() && !developer && quick_launch.empty()) return;
  constexpr std::string_view languages[] = {"en", "en", "ja", "de", "fr", "es", "it", "ko",
                                          "zh-tw", "pt", "zh-cn", "pl", "ru"};
  const auto language = REXCVAR_GET(user_language);
  const auto locale = REXCVAR_GET(mods_language).empty()
      ? std::string(language < std::size(languages) ? languages[language] : "en")
      : REXCVAR_GET(mods_language);
  auto mods = std::make_unique<Runtime>(GameCall, [](std::string_view text) { REXLOG_INFO("{}", text); },
                                       locale, ResourceServices());
  if (!fs::is_directory(root)) {
    std::error_code ec;
    fs::create_directories(root, ec);
  }
  mods->Load(root, runtime->user_data_root() / "mod-storage", enabled);
  mods->EnableInspection(developer);
  auto roots = mods->AssetRoots();
  if (REXCVAR_GET(mods_editor_preview)) {
    if (!developer) throw Failure("developer_disabled", "Map previews require a creator session");
    const auto content = runtime->user_data_root() / "editor-content";
    const auto flow = content / "simpsons_gameflow.lua";
    if (fs::is_symlink(content) || fs::is_symlink(flow) || !fs::is_regular_file(flow))
      throw Failure("invalid_argument", "The private preview gameflow is missing or linked");
    for (const auto& entry : fs::directory_iterator(content))
      if (entry.path() != flow)
        throw Failure("invalid_argument", "Editor content may only contain the selected gameflow");
    // The original archives remain mounted in place. Only this one script
    // wins over the project's overlay, for this private Test process.
    roots.insert(roots.begin(), content);
    g_editor_boot = true;
    g_editor_started = std::chrono::steady_clock::now();
  } else if (!quick_launch.empty()) {
    const auto orig = runtime->game_data_root() / "simpsons_gameflow.lua.original";
    const auto base_flow = fs::is_regular_file(orig)
                               ? orig
                               : (runtime->game_data_root() / "simpsons_gameflow.lua");
    const auto patched =
        BuildQuickLaunchGameflow(ReadText(base_flow, 2 * 1024 * 1024), quick_launch);
    const auto content = runtime->user_data_root() / "quick-launch-content";
    std::error_code ec;
    fs::remove_all(content, ec);
    fs::create_directories(content, ec);
    std::ofstream out(content / "simpsons_gameflow.lua", std::ios::binary | std::ios::trunc);
    out.write(patched.data(), std::streamsize(patched.size()));
    out.close();
    roots.insert(roots.begin(), content);
    g_editor_boot = true;
    g_editor_started = std::chrono::steady_clock::now();
    REXLOG_INFO("[modkit] quick-launch prepared for '{}'", quick_launch);
  }
  MountModFiles(runtime->file_system(), runtime->game_data_root(), roots);
  modkit_models::Init(runtime, mods->ModelEdits());
  modkit_meshes::Init(runtime, mods->MeshEdits(), mods->TextureEdits());
  REXLOG_INFO("[modkit] prepared {} mods for {}", mods->Status().size(), Platform());
  g_mods = std::move(mods);
  modkit_input::Enable(true);
  if (developer) {
    g_developer = std::make_unique<DeveloperBridge>(runtime->user_data_root() / "modkit-dev",
        [](std::string_view text) { REXLOG_ERROR("{}", text); });
    REXLOG_INFO("[modkit] creator session: {}", Utf8(g_developer->SessionFile()));
  }
}
void InitText(rex::Runtime* runtime) {
  std::lock_guard lock(g_mutex);
  if (g_mods) modkit_text::Init(runtime, g_mods->GameTexts());
}
void Tick(PPCContext& ctx, uint8_t* base) {
  std::lock_guard lock(g_mutex);
  ReapCheckpoint();
  if (!g_mods) return;
  Context context(ctx, base);
  const auto changed = g_world_changes.load();
  if (changed != g_world) {
    g_view_hidden.clear();
    g_view_mode = "all";
    g_view_active = false;
    g_view_isolate = 0;
    g_view_revision = ~uint64_t(0);
    g_handles.clear();
    if (g_entities) g_entities->Clear();
    modkit_input::Reset();
    g_mods->InvalidateWorld();
    g_world = changed;
    g_ready = false;
  }
  modkit_input::BeginFrame();
  const auto physics = g_physics_steps.load();
  if (physics > g_unload_step.load()) g_ready = true;
  if (g_editor_boot) {
    const auto now = std::chrono::steady_clock::now();
    if (g_ready || now - g_editor_started > std::chrono::minutes(3)) g_editor_boot = false;
    else if (now - g_editor_press > std::chrono::milliseconds(120) && !game_ui::PressingForGame()) {
      const auto front = simpsons::LoadGuestU32(base, 0x82D08C98);
      // Operate the original menus through their normal input route. The
      // preview has fresh private progress; no slot is deleted or patched.
      if (front < 9) game_ui::PressForGame(front < 6 ? game_ui::kPadStart : game_ui::kPadA);
      g_editor_press = now;
    }
  }
  // Build native names and definition ownership when the creator bridge is active.
  // Direct catalog queries still validate current loads and exact identities on demand.
  if (g_ready && g_developer)
    spawner::WarmCatalog(base);
  if (g_checkpoint_pending && g_ready && physics > g_checkpoint_step &&
      (!g_checkpoint_was_ready || g_world != g_checkpoint_world)) {
    // The checkpoint's guest level has begun. Install the prepared host state
    // before its first level.ready callback, without running old-level handles.
    g_checkpoint_pending = false;
    g_handles.clear();
    g_mods.swap(g_prepared);
    modkit_input::Reset();
    g_prepared.reset();
    g_mods->NotifyRestored(true);
  }
  RetireEntities();
  // This is the game's simulation delta, rather than a wall-clock timer.
  const float seconds = simpsons::LoadGuestFloat(base, 0x82CED748);
  g_mods->Tick(g_ready, physics != g_previous_step ? seconds : 0);
  g_previous_step = physics;
  PollReload();
  if (g_developer)
    g_developer->Pump(DeveloperRequest);
  if (g_developer && (g_view_mode != "all" || !g_view_hidden.empty())) UpdateEditorView();
  RetireEntities();
}
void InvalidateWorld() {
  modkit_text::Invalidate();
  g_unload_step = g_physics_steps.load();
  ++g_world_changes;
}
void PhysicsStepped() { g_physics_steps.fetch_add(1, std::memory_order_relaxed); }
bool SourceWorldReady() {
  return g_mods ? g_ready && g_world == g_world_changes.load() : LevelStepped(1500);
}
void Shutdown() {
  std::lock_guard lock(g_mutex);
  {
    std::lock_guard hook_lock(g_entity_hook_mutex);
    g_hook_entities.reset();
  }
  modkit_input::Enable(false);
  modkit_text::Shutdown();
  modkit_models::Shutdown();
  modkit_meshes::Shutdown();
  g_developer.reset();
  g_checkpoint_job.reset();
  g_checkpoint_folder.clear();
  g_checkpoint_cancelling = false;
  g_reload_job.reset();
  g_checkpoint_pending = false;
  g_prepared.reset(); g_mods.reset(); g_resources.reset(); g_entities.reset();
  g_handles.clear(); g_game = nullptr;
}
bool Active() { std::lock_guard lock(g_mutex); return g_mods && g_mods->active(); }
std::vector<fs::path> AssetRoots() {
  std::lock_guard lock(g_mutex);
  return g_mods ? g_mods->AssetRoots() : std::vector<fs::path>{};
}
std::vector<MenuPage> Pages() {
  std::lock_guard lock(g_mutex);
  return g_mods ? g_mods->Pages() : std::vector<MenuPage>{};
}
void Activate(std::string_view page, std::string_view row, const Json& value, uint32_t player) {
  std::lock_guard lock(g_mutex);
  if (!g_mods) return;
  // A native menu dispatch uses the same main-thread context as Tick.
  RunOnGameThread([page = std::string(page), row = std::string(row), value, player](PPCContext& ctx, uint8_t* base) {
    std::lock_guard lock(g_mutex);
    if (!g_mods) return;
    Context context(ctx, base);
    try { g_mods->Activate(page, row, value, player); }
    catch (const std::exception& error) { REXLOG_ERROR("[modkit] {}", error.what()); }
  });
}
bool IsGameThread() {
  std::lock_guard lock(g_mutex);
  return g_mods && g_mods->IsGameThread();
}
bool SupportsExact() {
  std::lock_guard lock(g_mutex);
  return !g_mods || g_mods->SupportsExact();
}
Json SaveState(bool exact) {
  std::lock_guard lock(g_mutex);
  if (g_entities && g_entities->HasPendingDeaths())
    throw Failure("busy", "Native combat notifications are queued; save again after the next frame");
  auto source = spawn_sources::SaveState(exact);
  auto state = g_mods ? g_mods->Save(exact) : Json{{"api", 1}, {"mods", Json::array()}};
  state["spawn_source"] = std::move(source);
  return state;
}
bool BeginCheckpoint(const fs::path& folder) {
  std::lock_guard lock(g_mutex);
  g_checkpoint_error.clear();
  try {
    ReapCheckpoint();
    // Choosing a different checkpoint can supersede a checkpoint staged by
    // Game Menu. An exact restore already being committed remains exclusive.
    if (g_checkpoint_job || g_reload_job || (g_prepared && !g_checkpoint_pending))
      throw Failure("busy", "Finish the pending mod preparation first");
    const auto sources = g_mods ? g_mods->Sources() : std::vector<Manifest>{};
    const auto cache = g_game ? g_game->user_data_root() / "mod-storage/.native-images" : fs::path{};
    g_checkpoint_job = std::make_unique<SourceJob>(sources, cache, false, folder / "mods.json");
    g_checkpoint_folder = folder;
    g_checkpoint_cancelling = false;
    return true;
  } catch (const std::exception& error) {
    g_checkpoint_error = error.what();
    REXLOG_ERROR("[modkit] checkpoint source preparation failed: {}", error.what());
    return false;
  }
}
bool CheckpointReady() {
  std::lock_guard lock(g_mutex);
  return g_checkpoint_job && !g_checkpoint_cancelling && g_checkpoint_job->ready();
}
std::string CheckpointError() {
  std::lock_guard lock(g_mutex);
  return g_checkpoint_error;
}
bool PrepareCheckpoint(const fs::path& folder) {
  std::lock_guard lock(g_mutex);
  try {
    g_checkpoint_pending = false;
    g_prepared_spawn_source = nullptr;
    g_prepared.reset();
    if (!g_checkpoint_job || g_checkpoint_cancelling || g_checkpoint_folder != folder ||
        !g_checkpoint_job->ready())
      throw Failure("not_ready", "Verify checkpoint sources before changing the working slot");
    auto sources = g_checkpoint_job->Take();
    g_checkpoint_job.reset();
    g_checkpoint_folder.clear();
    if (!sources.checkpoint_prepared)
      throw Failure("not_ready", "The checkpoint state was not prepared");
    if (!g_mods) return !sources.checkpoint_state || sources.checkpoint_state->at("mods").empty();
    if (!g_context) throw Failure("wrong_thread", "Checkpoint preflight requires a game-thread frame");
    g_prepared = sources.checkpoint_state
        ? g_mods->PrepareRestore(*sources.checkpoint_state, true, sources)
        : g_mods->PrepareInitialState(sources);
    g_checkpoint_step = g_physics_steps.load();
    g_checkpoint_world = g_world_changes.load();
    g_checkpoint_was_ready = g_ready;
    g_checkpoint_pending = true;
    return true;
  } catch (const std::exception& error) {
    if (g_checkpoint_job) {
      g_checkpoint_job->Cancel();
      g_checkpoint_cancelling = true;
      ReapCheckpoint();
    }
    g_checkpoint_error = error.what();
    REXLOG_ERROR("[modkit] checkpoint preflight failed: {}", error.what());
    return false;
  }
}
bool Compatible(const fs::path& folder, bool vanilla_checkpoint) {
  std::lock_guard lock(g_mutex);
  try {
    // Opening an older vanilla checkpoint with mods starts mod state from
    // initialization. Its guest-memory snapshot is never accepted this way.
    if (vanilla_checkpoint && !fs::exists(folder / "mods.json")) return true;
    const auto state = SavedProfile(folder);
    if (!vanilla_checkpoint)
      simpsons::modkit::source_state::ValidateClaims(state);
    if (g_mods) g_mods->ValidateSave(state);
    else if (!state.at("mods").empty()) return false;
    return true;
  } catch (const std::exception& error) {
    REXLOG_ERROR("[modkit] incompatible save: {}", error.what());
    return false;
  }
}
std::shared_ptr<const PreparedSources> VerifyRestoreSources() {
  std::vector<Manifest> sources;
  fs::path cache;
  {
    std::lock_guard lock(g_mutex);
    if (g_mods) sources = g_mods->Sources();
    if (g_game) cache = g_game->user_data_root() / "mod-storage/.native-images";
  }
  // File hashing and immutable code capture belong to the save reader's
  // worker, without holding the game adapter's mutex across filesystem I/O.
  return std::make_shared<PreparedSources>(PrepareSources(sources, cache, false));
}
bool PrepareRestore(const fs::path& folder, PPCContext& ctx, uint8_t* base,
                    const PreparedSources* sources) {
  std::lock_guard lock(g_mutex);
  Context context(ctx, base);
  g_checkpoint_pending = false;
  g_prepared.reset();
  g_prepared_spawn_source = nullptr;
  try {
    const auto state = SavedProfile(folder);
    const auto source = state.value("spawn_source", Json(nullptr));
    simpsons::modkit::source_state::ValidateClaims(state);
    if (g_mods)
      g_prepared =
          sources ? g_mods->PrepareRestore(state, false, *sources) : g_mods->PrepareRestore(state);
    else if (!state.at("mods").empty())
      throw Failure("save_incompatible", "The save requires mods");
    g_prepared_spawn_source = source;
    return true;
  } catch (const std::exception& error) {
    g_prepared.reset();
    REXLOG_ERROR("[modkit] save preflight failed: {}", error.what());
    return false;
  }
}
void CommitRestore(PPCContext& ctx, uint8_t* base) {
  simpsons::native_refpack::InvalidateStreams();
  modkit_input::Reset();
  modkit_text::Invalidate();
  std::lock_guard lock(g_mutex);
  // Guest memory has already rewound. Old host tokens must never destroy
  // allocations now belonging to that restored world.
  if (g_entities) g_entities->Clear();
  spawner::Reset();
  spawn_sources::RestoreState(ctx, base, g_prepared_spawn_source);
  g_prepared_spawn_source = nullptr;
  g_handles.clear();
  g_world = ++g_world_changes;
  g_ready = true;
  if (!g_prepared) return;
  g_mods.swap(g_prepared);
  g_prepared.reset();
  Context context(ctx, base);
  struct RestoreScope {
    RestoreScope() { g_restoring_entities = true; }
    ~RestoreScope() { g_restoring_entities = false; }
  } restoring;
  // Exact restore resets host catalog caches. Reconstruct them before timing
  // save.restored handlers that inspect the native object library.
  spawner::WarmCatalog(base);
  g_mods->NotifyRestored();
}
void CancelRestore() {
  std::lock_guard lock(g_mutex); g_checkpoint_pending = false; g_prepared.reset();
  g_prepared_spawn_source = nullptr;
  if (g_checkpoint_job) {
    g_checkpoint_job->Cancel();
    g_checkpoint_cancelling = true;
    ReapCheckpoint();
  }
}
void NativeHealthDeath(PPCContext& ctx, uint8_t* base) {
  std::shared_ptr<OwnedEntities> entities;
  {
    std::lock_guard lock(g_entity_hook_mutex);
    entities = g_hook_entities.lock();
  }
  if (!entities) return;
  const auto entity = entity_health::DyingEntity(base, ctx.r3.u32);
  if (entity) entities->Died(spawner::Lifetime(entity));
}
void NativeVisibilityReset(PPCContext& ctx, uint8_t*) {
  if (g_view_active.load()) {
    std::lock_guard lock(g_view_reset_mutex);
    g_view_resets.push_back(spawner::Lifetime(ctx.r3.u32));
  }
  std::shared_ptr<OwnedEntities> entities;
  {
    std::lock_guard lock(g_entity_hook_mutex);
    entities = g_hook_entities.lock();
  }
  if (entities)
    entities->ResetRenderHidden(spawner::Lifetime(ctx.r3.u32));
}
}  // namespace modkit_game

REX_HOOK_RAW(sub_829563B8) {
  // Queue before class callbacks can remove the target. The original routine
  // still owns death behavior; Lua runs later within the normal frame budget.
  modkit_game::NativeHealthDeath(ctx, base);
  __imp__sub_829563B8(ctx, base);
}

REX_EXTERN(__imp__sub_82697200);
REX_HOOK_RAW(sub_82697200) {
  // The game's explicit force-show cancels every native hide request. Retire
  // our lease too, so a later reveal cannot consume a newer authored request.
  const auto lifetime = spawner::Lifetime(ctx.r3.u32);
  players::VisibilityReset(lifetime);
  spawner::VisibilityReset(lifetime);
  modkit_game::NativeVisibilityReset(ctx, base);
  __imp__sub_82697200(ctx, base);
}
