// Entities created from level data, for the debug menu's spawner.
//
// The level loader (sub_826A3418 and two siblings) reads an entity record
// from the stream: a header with flags at +16 (bit 0: has a header), the
// entity's 16-byte GUID through the relative offset at +24 and its class hash
// at +28. sub_82703CA8(class hash, record) looks the class up in the registry
// that sub_82704008 fills (hash -> create function, name, size) and builds
// the entity from the record; sub_82703E90(hash) returns the class name.
// The loader separately registers it with the world (sub_826A31F0), which
// copies its GUID to the entity at +28. Once a layer has loaded,
// sub_826A0A28 sends iMsgPostSpawnInitialize (id at 0x82D583C4) to each of its
// entities through the subscriber at +56 (vtable +4, with {id, 0, 1}); a copy
// made later has to be sent it directly.
//
// Names: once built, most visible entities lead to the names of their own
// assets. Their model at +136 has a part list (+180, name at +28) or a mesh
// (+124, name at +244 or nearby) or textures (+148); animated ones have their
// character bank at +236 (name at +148 or +100); weapons point straight at
// their kind at +528 ("_pipe").

#include "spawner.h"
#include "modkit_game.h"
#include "host_memory.h"
#include "assets.h"
#include "asset_selection.h"
#include "asset_names.h"
#include "compiled_objects.h"
#include "resource_id.h"
#include "entity_transform.h"
#include "entity_render.h"
#include "entity_assets.h"
#include "freecam.h"
#include "players.h"
#include "spawn_templates.h"
#include "spawn_sources.h"

#include <atomic>
#include <cmath>

#include "game_thread.h"
#include "guest_memory.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>

REXCVAR_DEFINE_BOOL(debug_menu_log_entities, false, "Game",
                    "Log every entity the level creates and the name the spawner gives it "
                    "(for development)");

REX_EXTERN(__imp__sub_82703CA8);
REX_EXTERN(sub_82703CA8);
REX_EXTERN(__imp__sub_8269BC10);
REX_EXTERN(sub_8269BC10);

namespace spawner {
namespace {

using simpsons::GuestToHost;
using simpsons::LoadGuestU32;

constexpr uint32_t kClassName = 0x82703E90;
constexpr uint32_t kCreate = 0x82703CA8;
constexpr uint32_t kRegister = 0x826A31F0;
constexpr uint32_t kUnregister = 0x826A16F8;
constexpr uint32_t kDelete = 0x8269F990;
constexpr uint32_t kFindDirect = 0x826A11E0;
constexpr uint32_t kWorld = 0x82D08BEC;
constexpr uint32_t kGuid = 28;
constexpr uint32_t kPostSpawnMessage = 0x82D583C4;
constexpr uint32_t kSubscriber = 56;
constexpr uint32_t kFrame = 140;
constexpr size_t kPerClass = 600;

// Friendly categories for reviewed classes, also used to discover resident
// definitions before their first live instance has been observed.
struct Kind {
  const char* class_name;
  const char* category;
  const char* fallback;
};
constexpr Kind kKinds[] = {
    // Both factories construct the native Entity frame directly (82698280);
    // Animated adds its character bank through 82695898. These base classes
    // also have retained definitions before any live instance is observed.
    {"Entity", "Props", "Object"},
    {"Animated", "Props", "Animated object"},
    {"HoGDestructibleObject", "Props", "Giant smashable"},
    {"DestructibleDynamicObject", "Props", "Smashable object"},
    {"Destructible", "Props", "Breakable object"},
    {"DamageableProp", "Props", "Damageable prop"},
    {"MobInteractDamageableProp", "Props", "Mob damageable prop"},
    {"MobInteractDestructible", "Props", "Mob target"},
    {"ScriptableProp", "Props", "Prop"},
    {"HoGPuzzleObject", "Props", "Hand of God object"},
    {"PingPongPlatform", "Props", "Moving platform"},
    {"SpinPlatform", "Props", "Spinning platform"},
    {"DissolvePlatform", "Props", "Disappearing platform"},
    {"Door", "Props", "Door"},
    {"SlidingDoor", "Props", "Sliding door"},
    {"RotatingDoor", "Props", "Revolving door"},
    {"Switch", "Props", "Switch"},
    {"Trampoline", "Toys", "Trampoline"},
    {"Fulcrum", "Toys", "Seesaw"},
    {"GummiObject", "Props", "Gummi object"},
    {"FuncConveyorBelt", "Toys", "Conveyor belt"},
    {"Updraft", "Toys", "Updraft"},
    {"FuncPusher", "Toys", "Pusher"},
    {"Item", "Weapons", "Weapon"},
    {"Gun", "Weapons", "Thrown weapon"},
    {"ExplodingItem", "Weapons", "Exploding item"},
    {"Projectile", "Weapons", "Projectile"},
    {"Turret", "Weapons", "Turret"},
    {"Food", "Pickups", "Food"},
    {"GummiFood", "Pickups", "Gummi food"},
    {"Collectible", "Collectibles", "Collectible"},
    {"VariableCollectible", "Collectibles", "Collectible"},
    {"HoGCollectible", "Collectibles", "Hand of God collectible"},
    {"TuningOverrideCollectible", "Collectibles", "Power collectible"},
    {"NPCMobMember", "People", "Townsperson"},
    {"NPCEatingContestant", "People", "Eating contestant"},
    {"NPCDashEatingContestant", "People", "Eating contestant"},
    {"Maggie", "People", "Maggie"},
    {"NPCMelee", "Enemies", "Brawler"},
    {"NPCBase", "People", "NPC"},
    {"NPCMeleeFollower", "Enemies", "Brawler follower"},
    {"NPCMeleeGuard", "Enemies", "Guard"},
    {"NPCRanged", "Enemies", "Shooter"},
    {"NPCRangedGuard", "Enemies", "Ranged guard"},
    {"NPCMeleeRanged", "Enemies", "Fighter"},
    {"NPCMeleeRangedFollower", "Enemies", "Fighter follower"},
    {"NPCNinja", "Enemies", "Ninja"},
    {"NPCBerserker", "Enemies", "Berserker"},
    {"NPCDash", "Enemies", "Dasher"},
    {"NPCLardLad", "Enemies", "Lard Lad"},
    {"NPCShakespeare", "Enemies", "Shakespeare"},
    {"NPCSelmattyShire", "Enemies", "Selmatty (Shire)"},
    {"NPCSelmattyLair", "Enemies", "Selmatty (lair)"},
    {"NPCMobileRangedFloater", "Enemies", "Floater"},
    {"SpaceInvader", "Enemies", "Space invader"},
    {"NPC", "People", "NPC"},
    {"InteractiveNPC", "People", "Interactive NPC"},
    {"TargetPractice", "People", "Target practice"},
    {"CrowdNPC", "People", "Crowd NPC"},
    {"AmbientNPC", "People", "Townsperson"},
    {"FollowerNPC", "People", "Follower"},
    {"BasicEnemy", "Enemies", "Enemy"},
    {"RangedEnemy", "Enemies", "Ranged enemy"},
    {"Charger", "Enemies", "Charger"},
    {"GummiHomer", "Enemies", "Gummi Homer"},
    {"GummiVenus", "Bosses", "Gummi Venus"},
    {"WhiteChocolateRabbit", "Enemies", "White Chocolate Bunny"},
    {"Boss", "Bosses", "Boss"},
    {"SimpleVehicle", "Vehicles", "Vehicle"},
    {"SimpleProp", "Props", "Prop"},
    {"Breakable", "Props", "Breakable prop"},
    {"Manipulable", "Props", "Interactive prop"},
    {"PhysicsProp", "Props", "Physics prop"},
    {"Toy", "Toys", "Toy"},
    {"ToyVehicle", "Vehicles", "Toy vehicle"},
    {"Weapon", "Weapons", "Weapon"},
    {"Pickup", "Pickups", "Pickup"},
};

// Paths from an entity to the name of one of its assets: offsets to follow,
// then the offset of the name pointer.
struct Path {
  uint32_t steps[3];
  int count;
};
constexpr Path kPaths[] = {
    {{528}, 1},             // weapons: their kind
    {{136, 180, 28}, 3},    // the model's parts
    {{136, 124, 244}, 3},   // the model's mesh
    {{136, 124, 216}, 3},
    {{136, 124, 196}, 3},
    {{136, 124, 188}, 3},
    {{136, 148, 28}, 3},    // the model's textures
    {{236, 148}, 2},        // the character bank of animated things
    {{236, 100}, 2},
    {{236, 28, 4}, 3},
    {{136, 148, 8}, 3},
    {{1180, 8}, 2},
    {{428, 132}, 2},
    {{156, 44, 144}, 3},
};

std::recursive_mutex g_mutex;
rex::Runtime* g_runtime = nullptr;
std::map<uint32_t, uint64_t> g_lifetimes;
std::map<uint64_t, uint32_t> g_addresses;
std::map<uint64_t, Record> g_instances;
uint64_t g_instance_revision = 0;
std::map<uint64_t, uint32_t> g_debug_copies;
uint64_t g_next_lifetime = 0;
thread_local bool g_cloning = false;
std::map<std::string, Record> g_records;  // the latest record of each class
std::map<std::string, std::vector<Record>> g_all;  // every one, per spawnable class
uint32_t g_generation = 0;
std::atomic<uint64_t> g_source_epoch{1};
uint32_t g_scratch = 0;  // guest memory for a message
std::set<uint32_t> g_placeable_classes;
struct TemplateName {
  std::string name, category, model;
};
std::map<std::pair<uint32_t, uint32_t>, TemplateName> g_template_names;
std::map<std::pair<uint32_t, uint32_t>, simpsons::modkit::compiled_objects::Object> g_definition_objects;

uint32_t g_clones = 0;
// The latest spawns, to keep new ones clear of them.
struct Spawn {
  uint32_t entity, vtable;
  float radius;
  uint64_t lifetime;
};
std::vector<Spawn> g_placed;
std::set<uint64_t> g_spawned_lifetimes;

// AI control state
bool g_spawn_with_ai = true;
std::set<uint64_t> g_ai_disabled;

// Animation playback state
bool g_anim_loop = true;
AnimTargetMode g_anim_target_mode = AnimTargetMode::kCurrentPlayer;
struct AnimOverride {
  uint32_t clip_key = 0;
  bool loop = false;
  float speed = 1.0f;
  bool paused = false;
  std::chrono::steady_clock::time_point last_retrigger{};
};
std::map<uint64_t, AnimOverride> g_anim_overrides;
struct PendingAnimSpawn {
  bool active = false;
  size_t asset_index = 0;
  size_t clip_index = 0;
  AnimTargetMode target_mode = AnimTargetMode::kCurrentPlayer;
  bool loop = true;
  std::string stream;
};
PendingAnimSpawn g_pending_anim;

// Spooner mode state
bool g_spooner_enabled = false;
bool g_spooner_grabbing = false;
float g_spooner_grab_dist = 4.5f;
uint64_t g_spooner_selected = 0;
uint64_t g_spooner_hovered = 0;
std::map<uint64_t, entity_transform::Transform> g_initial_transforms;
struct RemovedMapEntity {
  uint64_t lifetime = 0;
  entity_transform::Transform original_transform{};
  bool hid_entity = false;
};
std::map<uint64_t, RemovedMapEntity> g_removed_map_entities;
std::map<uint64_t, std::string> g_instance_label_cache;

bool Plausible(uint32_t address) {
  return address >= 0x82000000u && address < 0xFFF00000u && !(address & 3);
}

const Kind* FindKind(std::string_view class_name) {
  for (const Kind& kind : kKinds)
    if (class_name == kind.class_name)
      return &kind;
  static constexpr Kind kFallbackEnemy{"NPCEnemy", "Enemies", "Enemy"};
  static constexpr Kind kFallbackNpc{"NPC", "People", "NPC"};
  static constexpr Kind kFallbackBoss{"Boss", "Bosses", "Boss"};
  static constexpr Kind kFallbackVehicle{"Vehicle", "Vehicles", "Vehicle"};
  static constexpr Kind kFallbackProp{"Prop", "Props", "Prop"};
  static constexpr Kind kFallbackCollectible{"Collectible", "Collectibles", "Collectible"};
  if (class_name.starts_with("NPC")) {
    if (class_name.find("Melee") != std::string_view::npos ||
        class_name.find("Ranged") != std::string_view::npos ||
        class_name.find("Guard") != std::string_view::npos ||
        class_name.find("Enemy") != std::string_view::npos ||
        class_name.find("Minion") != std::string_view::npos)
      return &kFallbackEnemy;
    return &kFallbackNpc;
  }
  if (class_name.find("Boss") != std::string_view::npos)
    return &kFallbackBoss;
  if (class_name.find("Vehicle") != std::string_view::npos)
    return &kFallbackVehicle;
  if (class_name.find("Collectible") != std::string_view::npos)
    return &kFallbackCollectible;
  if (class_name.find("Prop") != std::string_view::npos ||
      class_name.find("Destructible") != std::string_view::npos ||
      class_name.find("Platform") != std::string_view::npos)
    return &kFallbackProp;
  return nullptr;
}

std::string CategoryOf(std::string_view name) {
  if (const auto* kind = FindKind(name))
    return kind->category;
  if (name.ends_with("Player"))
    return "Player characters";
  if (name.find("Trigger") != name.npos)
    return "Triggers";
  if (name.find("Camera") != name.npos)
    return "Cameras";
  if (name.find("Sequence") != name.npos)
    return "Sequences";
  if (name.find("Audio") != name.npos || name.find("Chatter") != name.npos || name == "Narrator")
    return "Audio and dialogue";
  if (name.find("Cursor") != name.npos)
    return "Ability objects";
  return "Other objects";
}

std::string ClassName(PPCContext& ctx, uint8_t* base, uint32_t hash) {
  uint32_t name = 0;
  if (!CallGuest(ctx, base, kClassName, {hash}, &name) || !name)
    return {};
  return entity_assets::ReadName(base, name);
}

// An asset name: letters, digits and underscores.
bool AssetName(const uint8_t* base, uint32_t address, std::string* out) {
  if (!Plausible(address & ~3u))
    return false;
  const auto name = entity_assets::ReadName(base, address, 64);
  for (const char c : name) {
    if (!(std::isalnum(uint8_t(c)) || c == '_'))
      return false;
  }
  // Real asset names have an underscore or a digit ("spr_trashcan_MMPart",
  // "_pipe"); words like "this" come from other data.
  if (name.size() < 3 || name.find_first_of("_0123456789") == std::string::npos)
    return false;
  static const char* const generic[] = {"simpsons_palette", "moh_palette", "line_anim", "lod",
                                        "opt_part", "eat_", "BROAD_PHASE", "17_failed", "EVENT_"};
  for (const char* g : generic)
    if (name.rfind(g, 0) == 0)
      return false;
  *out = name;
  return true;
}

std::string RawName(const uint8_t* base, uint32_t entity) {
  for (const Path& path : kPaths) {
    uint32_t address = entity;
    bool ok = true;
    for (int i = 0; i + 1 < path.count && ok; ++i) {
      if (!Readable(address, path.steps[i] + 4)) {
        ok = false;
        break;
      }
      address = LoadGuestU32(base, address + path.steps[i]);
      ok = Plausible(address);
    }
    if (!ok || !Readable(address, path.steps[path.count - 1] + 4))
      continue;
    std::string name;
    if (AssetName(base, LoadGuestU32(base, address + path.steps[path.count - 1]), &name))
      return name;
  }
  return {};
}

std::string Pretty(std::string raw) {
  return simpsons::modkit::FriendlyAssetName(std::move(raw));
}

bool InstanceAlive(const uint8_t* base, const Record& record) {
  return record.lifetime && Lifetime(record.entity) == record.lifetime &&
         Readable(record.entity, 72) && Readable(record.data, 32) &&
         LoadGuestU32(base, record.entity) == record.vtable &&
         LoadGuestU32(base, record.data + 28) == record.hash;
}

spawn_templates::Memory Memory(const uint8_t* base) {
  return spawn_templates::ReadOnlyPages(
      {Readable, [base](uint32_t at) { return LoadGuestU32(base, at); },
       [base](uint32_t at) { return simpsons::GuestToHost(base, at)[0]; }});
}
uint32_t FileManager(const uint8_t* base) {
  constexpr uint32_t address = 0x82D096A4;
  return Readable(address, 4) ? LoadGuestU32(base, address) : 0;
}
struct NativeClass {
  std::string name;
  uint32_t bytes = 0;
};
std::map<uint32_t, NativeClass> NativeClasses(const uint8_t* base) {
  // sub_82703BD0 returns this sorted registry. Its 16-byte entries are
  // {class hash, factory, name, component offset}; no hardcoded class ceiling.
  constexpr uint32_t registry = uint32_t(-32041ll * 65536 - 16244);
  std::map<uint32_t, NativeClass> result;
  if (!Readable(registry, 12))
    return result;
  const auto entries = LoadGuestU32(base, registry), count = LoadGuestU32(base, registry + 4);
  if (!count || count > 1024 || !Readable(entries, count * 16))
    return result;
  for (uint32_t i = 0; i < count; ++i) {
    const auto at = entries + i * 16, name = LoadGuestU32(base, at + 8);
    std::string text;
    bool terminated = false;
    if (uint64_t(name) + 128 > 0x100000000ull)
      return {};
    const bool readable_name = Readable(name, 128);
    for (uint32_t j = 0; j < 128 && (readable_name || Readable(name + j, 1)); ++j) {
      const auto c = simpsons::GuestToHost(base, name + j)[0];
      if (!c) {
        terminated = true;
        break;
      }
      if (!std::isalnum(c) && c != '_') {
        text.clear();
        break;
      }
      text += char(c);
    }
    if (!terminated || text.empty() || scores::Hash(text) != LoadGuestU32(base, at))
      return {};
    result.emplace(LoadGuestU32(base, at),
                   NativeClass{std::move(text), LoadGuestU32(base, at + 12)});
  }
  return result;
}

void Forget(uint32_t entity) {
  std::lock_guard lock(g_mutex);
  const auto found = g_lifetimes.find(entity);
  if (found == g_lifetimes.end()) return;
  const auto lt = found->second;
  g_addresses.erase(lt);
  g_instances.erase(lt);
  ++g_instance_revision;
  g_debug_copies.erase(lt);
  g_spawned_lifetimes.erase(lt);
  g_ai_disabled.erase(lt);
  g_anim_overrides.erase(lt);
  g_initial_transforms.erase(lt);
  g_removed_map_entities.erase(lt);
  g_instance_label_cache.erase(lt);
  if (g_spooner_selected == lt) {
    g_spooner_selected = 0;
    g_spooner_grabbing = false;
  }
  if (g_spooner_hovered == lt)
    g_spooner_hovered = 0;
  g_lifetimes.erase(found);
  ++g_generation;
}
std::array<uint32_t, 3> GuidPrefix(std::string_view owner) {
  return {0x534D4F44, scores::Hash(owner), scores::Hash("spawn/" + std::string(owner))};
}
bool WorldLinked(const uint8_t* base, uint32_t entity) {
  if (!Readable(entity, 20)) return false;
  const auto previous = LoadGuestU32(base, entity + 4);
  return Readable(previous, 4) && LoadGuestU32(base, previous) == entity;
}

std::vector<Spawnable> g_catalog;
std::unique_ptr<simpsons::modkit::AssetIndex> g_library;
std::shared_ptr<const simpsons::modkit::AssetSnapshot> g_catalog_library;
uint32_t g_catalog_generation = ~0u;
uint32_t g_catalog_frame = 0;

}  // namespace

void Init(rex::Runtime* runtime) {
  g_runtime = runtime;
  g_scratch = host_memory::Alloc(64);
  auto roots = modkit_game::AssetRoots();
  roots.push_back(runtime->game_data_root());
  g_library = std::make_unique<simpsons::modkit::AssetIndex>(std::move(roots));
  spawn_sources::Init(runtime);
}
void Shutdown() {
  spawn_sources::Shutdown();
  g_library.reset();
  g_runtime = nullptr;
}
std::shared_ptr<const simpsons::modkit::AssetSnapshot> Library() {
  return g_library ? g_library->Get() : std::make_shared<simpsons::modkit::AssetSnapshot>();
}

uint32_t Clone(PPCContext& ctx, uint8_t* base, const Record& record,
               const std::array<float, 3>& position, float radius, std::string_view owner) {
  if (!g_scratch || !record.data)
    return 0;
  if (owner.empty() && !SpawnWithAi() &&
      (record.name.starts_with("NPC") || CategoryOf(record.name) == "People" ||
       CategoryOf(record.name) == "Enemies" || CategoryOf(record.name) == "Bosses")) {
    REXLOG_WARN("[spawner] AI suspension is not supported for {}; no copy created", record.name);
    return 0;
  }
  // Streaming may have unloaded the section the record came from.
  if (!spawn_sources::CanSpawn(record.source_layer) || !TemplateValid(base, record)) {
    REXLOG_WARN("[spawner] {} is no longer loaded here", record.name);
    return 0;
  }
  const uint32_t world = LoadGuestU32(base, kWorld);
  if (!Readable(world, 432)) return 0;
  const auto count_before = LoadGuestU32(base, world + 164);
  uint64_t created_after;
  { std::lock_guard lock(g_mutex); created_after = g_next_lifetime; }
  uint32_t entity = 0;
  // A clone is not a new streaming template. Its source record still belongs
  // to the original layer, so never replace that layer's catalog entry.
  struct CloneScope {
    bool previous = g_cloning;
    CloneScope() { g_cloning = true; }
    ~CloneScope() { g_cloning = previous; }
  } cloning;
  if (!CallGuest(ctx, base, kCreate, {record.hash, record.data}, &entity) || !entity) {
    REXLOG_WARN("[spawner] {} could not be built", record.name);
    return 0;
  }
  const auto lifetime = Observe(entity);
  const auto count_created = LoadGuestU32(base, world + 164);
  // Some class initializers register before returning; other classes leave
  // registration to the loader. Preserve singleton factory results and use
  // the original unregister routine before changing a linked clone's GUID.
  if ((entity == record.entity && lifetime == record.lifetime) || lifetime <= created_after) {
    REXLOG_WARN("[spawner] {} returned an existing object instead of a new clone", record.name);
    return 0;
  }
  // Constructors may set the unregister flag before the loader has linked
  // anything. Test the actual intrusive world link, not that default flag.
  if (!Readable(entity, 72)) {
    Forget(entity);
    return 0;
  }
  if (WorldLinked(base, entity) &&
      !CallGuest(ctx, base, kUnregister, {world, entity})) {
    Destroy(ctx, base, lifetime);
    return 0;
  }
  const auto count_detached = LoadGuestU32(base, world + 164);
  rex::CallFrame scratch(ctx);
  scratch.ctx.r1.u64 -= 0x100;
  const uint32_t guid = scratch.ctx.r1.u32 + 0x80;
  const uint32_t header = scratch.ctx.r1.u32 + 0xA0;
  const auto prefix = GuidPrefix(owner);
  for (size_t i = 0; i < prefix.size(); ++i)
    simpsons::StoreGuestU32(base, guid + uint32_t(i) * 4, prefix[i]);
  uint32_t collision = 0;
  bool unique = false;
  for (size_t attempt = 0; attempt < 128 && g_clones != UINT32_MAX; ++attempt) {
    simpsons::StoreGuestU32(base, guid + 12, ++g_clones);
    if (!CallGuest(scratch.ctx, base, kFindDirect, {world + 88, guid}, &collision)) break;
    if (!collision) { unique = true; break; }
  }
  if (!unique) { Destroy(ctx, base, lifetime); return 0; }
  // Register's bit-2 branch indexes a live entity directly, without inserting
  // a borrowed level-data record into the world's separate streaming lists.
  // This scratch header is read only during registration; it is not retained.
  std::memset(GuestToHost(base, header), 0, 32);
  GuestToHost(base, header + 16)[0] = 4;
  const uint32_t layer =
      record.source_layer ? record.source_layer : LoadGuestU32(base, record.entity + 24);
  if (!CallGuest(scratch.ctx, base, kRegister, {world, guid, header, entity, layer}) ||
      !WorldLinked(base, entity)) {
    Destroy(ctx, base, lifetime);
    return 0;
  }
  if (REXCVAR_GET(debug_menu_log_entities))
    REXLOG_INFO("[spawner] {} world counts: {} before, {} factory, {} detached, {} registered",
        record.name, count_before, count_created, count_detached, LoadGuestU32(base, world + 164));
  const auto placement_start = std::chrono::steady_clock::now();
  // The post-spawn initialization the layer's other entities got.
  const uint32_t subscriber = entity + kSubscriber;
  const uint32_t table = LoadGuestU32(base, subscriber);
  if (Plausible(table) && (LoadGuestU32(base, subscriber + 4) & 2)) {
    const uint32_t message = g_scratch + 16;
    simpsons::StoreGuestU32(base, message, LoadGuestU32(base, kPostSpawnMessage));
    simpsons::StoreGuestU32(base, message + 4, 0);
    simpsons::StoreGuestU32(base, message + 8, 1);
    CallGuest(ctx, base, LoadGuestU32(base, table + 4), {subscriber, message});
  }
  // Initialization and class setters can run other game logic. A rejected
  // instance may destroy itself; never read it again after its lifetime ends.
  if (Lifetime(entity) != lifetime) return 0;
  if (!entity_transform::Move(ctx, base, entity, position)) {
    REXLOG_WARN("[spawner] {} has no supported position setter", record.name);
    Destroy(ctx, base, lifetime);
    return 0;
  }
  if (Lifetime(entity) != lifetime || !Readable(entity, kFrame + 4)) return 0;
  if (REXCVAR_GET(debug_menu_log_entities))
    REXLOG_INFO("[spawner] {} initialization and placement: {} us", record.name,
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - placement_start).count());
  {
    std::lock_guard lock(g_mutex);
    g_spawned_lifetimes.insert(lifetime);
    if (owner.empty())
      g_debug_copies[lifetime] = layer;
    if (g_placed.size() >= 64)
      g_placed.erase(g_placed.begin());
    g_placed.push_back({entity, LoadGuestU32(base, entity), radius, lifetime});
    if (record.source_layer) {
      // A foreign definition has no source actor to lend it an asset name.
      // Learn from this successful native instance, then retain that label for
      // the definition even after the spawned copy has been removed.
      auto& label = g_template_names[{record.source_layer, record.data}];
      const auto* kind = FindKind(record.name);
      if (kind) {
        if (auto name = Pretty(RawName(base, entity)); !name.empty())
          label.name = std::move(name);
      }
      if (label.name.empty())
        label.name = kind ? kind->fallback : record.name;
      if (label.category.empty())
        label.category = CategoryOf(record.name);
      if (const auto model = entity_assets::ModelOf(base, entity))
        label.model = model->Id();
      ++g_generation;
    }
  }
  REXLOG_INFO("[spawner] spawned {} as {:08X} at ({:.1f}, {:.1f}, {:.1f})", record.name, entity,
              position[0], position[1], position[2]);
  return entity;
}

std::vector<Placed> Spawned(PPCContext& ctx, uint8_t* base) {
  std::lock_guard lock(g_mutex);
  std::vector<Placed> placed;
  for (const auto& clone : g_placed) {
    if (Lifetime(clone.entity) != clone.lifetime || !Readable(clone.entity, kFrame + 4) ||
      LoadGuestU32(base, clone.entity) != clone.vtable)
      continue;
    if (const auto transform = entity_transform::Get(ctx, base, clone.entity))
      placed.push_back({transform->position, clone.radius});
  }
  return placed;
}

void Reset() {
  modkit_game::InvalidateWorld();
  entity_assets::Reset();
  players::ResetSessionState();
  std::lock_guard lock(g_mutex);
  g_placed.clear();
  g_spawned_lifetimes.clear();
  g_ai_disabled.clear();
  g_anim_overrides.clear();
  g_pending_anim = {};
  g_spooner_selected = 0;
  g_spooner_hovered = 0;
  g_spooner_grabbing = false;
  g_initial_transforms.clear();
  g_removed_map_entities.clear();
  g_instance_label_cache.clear();
  g_lifetimes.clear();
  g_instances.clear();
  ++g_instance_revision;
  g_addresses.clear();
  g_debug_copies.clear();
  g_records.clear();
  g_all.clear();
  g_template_names.clear();
  g_definition_objects.clear();
  g_placeable_classes.clear();
  ++g_source_epoch;
  g_catalog.clear();
  g_catalog_library.reset();
  ++g_generation;
}

uint64_t Observe(uint32_t entity) {
  if (!entity) return 0;
  std::lock_guard lock(g_mutex);
  auto& lifetime = g_lifetimes[entity];
  if (!lifetime) { lifetime = ++g_next_lifetime; g_addresses[lifetime] = entity; }
  return lifetime;
}
uint64_t Lifetime(uint32_t entity) {
  std::lock_guard lock(g_mutex);
  const auto found = g_lifetimes.find(entity);
  return found == g_lifetimes.end() ? 0 : found->second;
}
uint32_t Address(uint64_t lifetime) {
  std::lock_guard lock(g_mutex);
  const auto found = g_addresses.find(lifetime);
  return found == g_addresses.end() ? 0 : found->second;
}
bool Readable(uint32_t address, uint32_t bytes) {
  if (!g_runtime || !address || !bytes || uint64_t(address) + bytes > 0x100000000ull)
    return false;
  auto* heap = g_runtime->memory()->LookupHeap(address);
  rex::memory::HeapAllocationInfo info{};
  if (!heap || !heap->QueryRegionInfo(address, &info) ||
      !(info.state & rex::memory::kMemoryAllocationCommit) ||
      !(info.protect & rex::memory::kMemoryProtectRead)) return false;
  const uint32_t offset = address & (heap->page_size() - 1);
  return info.region_size > offset && bytes <= info.region_size - offset &&
      uint64_t(address) + bytes <= uint64_t(info.allocation_base) + info.allocation_size;
}
bool Destroy(PPCContext& ctx, uint8_t* base, uint64_t lifetime) {
  const auto entity = Address(lifetime);
  if (!entity) return true;  // The game already destroyed it.
  if (!Readable(entity, 72)) { Forget(entity); return true; }
  const auto table = LoadGuestU32(base, entity);
  if (!Readable(table, 4)) return false;
  // A factory failure can leave a constructed object before registration.
  // Its constructor's default unregister bit must not decrement world counts
  // or remove an unrelated hash entry during rollback.
  if (!WorldLinked(base, entity))
    simpsons::StoreGuestU32(base, entity + 20,
        LoadGuestU32(base, entity + 20) & ~uint32_t(0x20000000));
  // This original helper chooses subscriber destruction for bit 0x08000000
  // and the class's deleting destructor otherwise, exactly as world teardown.
  if (!CallGuest(ctx, base, kDelete, {entity})) return false;
  return !Address(lifetime);
}
void DestroyDebugCopies(PPCContext& ctx, uint8_t* base, const std::vector<uint32_t>& layers) {
  std::vector<uint64_t> copies;
  {
    std::lock_guard lock(g_mutex);
    for (const auto& [lifetime, layer] : g_debug_copies)
      if (std::find(layers.begin(), layers.end(), layer) != layers.end())
        copies.push_back(lifetime);
  }
  for (const auto lifetime : copies)
    Destroy(ctx, base, lifetime);
}
uint32_t FindOwned(PPCContext& ctx, uint8_t* base, std::string_view owner,
                   const std::array<uint32_t, 4>& guid, uint32_t vtable) {
  const auto prefix = GuidPrefix(owner);
  if (!std::equal(prefix.begin(), prefix.end(), guid.begin()) || !guid[3]) return 0;
  const auto world = LoadGuestU32(base, kWorld);
  if (!Readable(world, 432)) return 0;
  rex::CallFrame scratch(ctx);
  scratch.ctx.r1.u64 -= 0x80;
  const auto at = scratch.ctx.r1.u32 + 0x60;
  for (size_t i = 0; i < guid.size(); ++i)
    simpsons::StoreGuestU32(base, at + uint32_t(i) * 4, guid[i]);
  uint32_t entity = 0;
  if (!CallGuest(scratch.ctx, base, kFindDirect, {world + 88, at}, &entity) ||
      !Readable(entity, 72) || LoadGuestU32(base, entity) != vtable ||
      !(LoadGuestU32(base, entity + 20) & 0x20000000)) return 0;
  return entity;
}

std::vector<Record> Records() {
  std::lock_guard lock(g_mutex);
  std::vector<Record> list;
  for (const auto& [name, record] : g_records)
    list.push_back(record);
  return list;
}

std::vector<Record> Instances(const uint8_t* base) {
  std::lock_guard lock(g_mutex);
  std::vector<Record> list;
  for (const auto& [lifetime, record] : g_instances)
    if (InstanceAlive(base, record)) list.push_back(record);
  return list;
}

uint64_t InstanceRevision() {
  std::lock_guard lock(g_mutex);
  return g_instance_revision;
}

bool TemplateValid(const uint8_t* base, const Record& record) {
  std::lock_guard lock(g_mutex);
  if (!record.source_layer)
    return InstanceAlive(base, record);
  if (record.source_epoch != g_source_epoch || !Readable(kWorld, 4) ||
      LoadGuestU32(base, kWorld) != record.source_world)
    return false;
  const auto memory = Memory(base);
  const auto definition = spawn_templates::Find(memory, record.source_world, record.source_guid);
  if (!definition || definition->data != record.data || definition->hash != record.hash)
    return false;
  // Catalog captured exact layer membership. That record cannot change its
  // allocation owner while the same unique file-load key remains alive. Check
  // the retained identity and load here; walking every definition for every
  // handle would make a catalog query quadratic in the level's object count.
  const auto files = spawn_templates::Files(memory, FileManager(base));
  return std::any_of(files.begin(), files.end(), [&](const auto& file) {
    return file.key == record.source_layer && file.state == 4;
  });
}

std::vector<const Spawnable*> CurrentCatalog(const uint8_t* base) {
  std::lock_guard lock(g_mutex);
  const auto& catalog = Catalog(base);
  const auto memory = Memory(base);
  const auto world = Readable(kWorld, 4) ? LoadGuestU32(base, kWorld) : 0;
  std::set<uint32_t> loaded;
  for (const auto& file : spawn_templates::Files(memory, FileManager(base)))
    if (file.state == 4)
      loaded.insert(file.key);
  std::vector<const Spawnable*> result;
  result.reserve(catalog.size());
  for (const auto& item : catalog) {
    const auto& record = item.record;
    if (!record.source_layer) {
      if (InstanceAlive(base, record))
        result.push_back(&item);
      continue;
    }
    if (record.source_epoch != g_source_epoch || record.source_world != world ||
        !loaded.contains(record.source_layer))
      continue;
    const auto definition = spawn_templates::Find(memory, world, record.source_guid);
    if (definition && definition->data == record.data && definition->hash == record.hash)
      result.push_back(&item);
  }
  return result;
}

void InvalidateCatalog() {
  std::lock_guard lock(g_mutex);
  ++g_generation;
}

uint64_t WorldEpoch() {
  return g_source_epoch.load(std::memory_order_acquire);
}

std::map<uint32_t, std::string> PlaceableClasses(const uint8_t* base) {
  std::lock_guard lock(g_mutex);
  std::map<uint32_t, std::string> result;
  for (const auto& [hash, type] : NativeClasses(base))
    if (type.bytes >= kFrame + 4 && (FindKind(type.name) || g_placeable_classes.contains(hash)))
      result.emplace(hash, type.name);
  return result;
}

std::optional<Spawnable> FindTemplate(const uint8_t* base, std::string_view guid, uint32_t hash) {
  for (const auto* item : CurrentCatalog(base))
    if (item->record.hash == hash &&
        simpsons::modkit::ResourceId(item->record.source_guid) == guid)
      return *item;
  return {};
}

void WarmCatalog(const uint8_t* base) {
  std::lock_guard lock(g_mutex);
  if (g_catalog.empty() || ++g_catalog_frame % 240 == 0) {
    if (g_catalog_frame % 240 == 0)
      g_catalog_generation = ~g_generation;
    Catalog(base);
  }
}

const std::vector<Spawnable>& Catalog(const uint8_t* base) {
  std::lock_guard lock(g_mutex);
  // Opening the resource library starts its worker. Plain template queries
  // only inspect an existing result and never trigger a whole-game scan.
  auto library = g_library ? g_library->Peek() : nullptr;
  if (library && !library->ready)
    library.reset();
  // Periodic work belongs to WarmCatalog, never an arbitrary mod query.
  if (g_catalog_generation == g_generation && g_catalog_library == library)
    return g_catalog;
  g_catalog_generation = g_generation;
  g_catalog_library = library;
  const auto memory = Memory(base);
  const auto world = Readable(kWorld, 4) ? LoadGuestU32(base, kWorld) : 0;
  const auto classes = NativeClasses(base);
  auto owners = spawn_templates::DefinitionOwners(memory, world);
  std::set<uint32_t> loaded;
  for (const auto& file : spawn_templates::Files(memory, FileManager(base)))
    if (file.state == 4)
      loaded.insert(file.key);
  std::erase_if(owners, [&](const auto& entry) { return !loaded.contains(entry.second); });
  const auto definitions = spawn_templates::List(memory, world);
  std::map<uint32_t, spawn_templates::Definition> resident;
  for (const auto& definition : definitions)
    resident.emplace(definition.data, definition);
  std::erase_if(g_template_names, [&](const auto& entry) {
    const auto definition = resident.find(entry.first.second);
    if (definition == resident.end())
      return true;
    const auto owner = owners.find(definition->second.data);
    return owner == owners.end() || owner->second != entry.first.first;
  });
  std::erase_if(g_definition_objects, [&](const auto& entry) {
    const auto definition = resident.find(entry.first.second);
    const auto owner = owners.find(entry.first.second);
    return definition == resident.end() || owner == owners.end() || owner->second != entry.first.first ||
           definition->second.guid != entry.second.guid || definition->second.hash != entry.second.hash;
  });
  std::map<std::tuple<std::string, std::string, uint32_t>, Spawnable> found;
  const auto retain_source = [&](Record& record) {
    const auto source = resident.find(record.data);
    if (source == resident.end() || source->second.hash != record.hash)
      return;
    const auto owner = owners.find(source->second.data);
    if (owner == owners.end() || !owner->second)
      return;
    record.source_guid = source->second.guid;
    record.source_layer = owner->second;
    record.source_world = world;
    record.source_epoch = g_source_epoch;
  };
  for (auto& [class_name, records] : g_all) {
    const Kind* kind = FindKind(class_name);
    for (Record& record : records) {
      if (!InstanceAlive(base, record) || !Readable(record.entity, kFrame + 4) ||
          !WorldLinked(base, record.entity))
        continue;
      // Factory registrations also include non-world services and singletons.
      // Only an actual registered entity with a native frame teaches a new
      // placeable class. The original transform adapter validates its hierarchy.
      if (!Readable(LoadGuestU32(base, record.entity + kFrame), 164))
        continue;
      const auto native_class = classes.find(record.hash);
      if (native_class == classes.end() || native_class->second.bytes < kFrame + 4)
        continue;
      g_placeable_classes.insert(record.hash);
      retain_source(record);
      // These asset-offset paths describe the reviewed visible classes.
      // Logic objects use native names rather than interpreting unrelated fields.
      std::string name = kind ? Pretty(RawName(base, record.entity)) : std::string();
      const auto model = entity_assets::ModelOf(base, record.entity);
      const auto model_id = model ? model->Id() : std::string();
      auto model_name = model ? model->name : std::string();
      if (library && !model_id.empty()) {
        if (const auto* asset = library->FindNative(model_id)) {
          name = asset->name;
          model_name = asset->native_name;
        }
      }
      constexpr std::string_view suffix = ".meta.xml";
      if (model_name.ends_with(suffix)) {
        model_name.resize(model_name.size() - suffix.size());
        if (model_name.ends_with("_geo")) model_name.resize(model_name.size() - 4);
        if (auto pretty = Pretty(model_name); !pretty.empty()) name = std::move(pretty);
      }
      if (name.empty())
        name = kind ? kind->fallback : class_name;
      std::string category = CategoryOf(class_name);
      for (const char* vehicle : {"car", "Van", "truck", "Convertible"})
        if (name.find(vehicle) != std::string::npos)
          category = "Vehicles";
      // A shared model is not proof of equal behavior. Keep distinct native
      // definitions selectable, including different tuning and script links.
      auto& spawnable = found[{category, name, record.data}];
      spawnable.name = name;
      spawnable.category = category;
      spawnable.record = record;
      spawnable.model_resource = model_id;
      ++spawnable.seen;
      if (record.source_layer)
        g_template_names[{record.source_layer, record.data}] = {name, category, model_id};
    }
  }
  for (const auto& [data, definition] : resident) {
    const auto native_class = classes.find(definition.hash);
    if (native_class == classes.end() || native_class->second.bytes < kFrame + 4)
      continue;
    const auto* kind = FindKind(native_class->second.name);
    if (!kind && !g_placeable_classes.contains(definition.hash))
      continue;
    Record record{native_class->second.name, definition.hash, data};
    retain_source(record);
    if (!record.source_layer)
      continue;
    TemplateName name{kind ? kind->fallback : record.name, CategoryOf(record.name), {}};
    const auto identity = std::make_pair(record.source_layer, data);
    auto parsed = g_definition_objects.find(identity);
    if (parsed == g_definition_objects.end())
      if (const auto object = simpsons::modkit::compiled_objects::Read(memory, data))
        parsed = g_definition_objects.emplace(identity, *object).first;
    if (parsed != g_definition_objects.end()) {
      const auto* object = &parsed->second;
      auto model_id = simpsons::modkit::ResourceId(object->model);
      if (model_id.empty() && library) {
        const auto fallback = simpsons::modkit::ResourceId(object->fallback_model);
        if (const auto* asset = library->FindNative(fallback); asset && asset->native_class == "MetaModel")
          model_id = fallback;
      }
      if (!model_id.empty()) {
        name.model = std::move(model_id);
        if (library)
          if (const auto* asset = library->FindNative(name.model))
            name.name = asset->name;
      }
    }
    if (const auto known = g_template_names.find({record.source_layer, data});
        known != g_template_names.end())
      name = known->second;
    const auto key = std::make_tuple(name.category, name.name, data);
    if (!found.contains(key))
      found.emplace(key, Spawnable{name.name, name.category, record, 0, name.model});
  }
  g_catalog.clear();
  std::map<std::pair<std::string, std::string>, size_t> labels;
  for (const auto& [key, spawnable] : found)
    ++labels[{spawnable.category, spawnable.name}];
  for (auto& [key, spawnable] : found) {
    if (labels[{spawnable.category, spawnable.name}] > 1) {
      spawnable.name +=
          " [" +
          fmt::format("{:08X}", spawnable.record.source_layer
                                    ? spawnable.record.source_guid[3]
                                    : LoadGuestU32(base, spawnable.record.entity + 40)) +
          "]";
    }
    g_catalog.push_back(std::move(spawnable));
  }
  if (REXCVAR_GET(debug_menu_log_entities))
    REXLOG_INFO(
        "[spawner] catalog: {} native classes, {} resident definitions, {} loaded source files, "
        "{} placeable classes, {} templates",
        classes.size(), definitions.size(), loaded.size(), g_placeable_classes.size(),
        g_catalog.size());
  return g_catalog;
}

bool SpawnWithAi() {
  std::lock_guard lock(g_mutex);
  return g_spawn_with_ai;
}
void SetSpawnWithAi(bool enabled) {
  std::lock_guard lock(g_mutex);
  g_spawn_with_ai = enabled;
}
bool IsAiDisabled(uint64_t lifetime) {
  std::lock_guard lock(g_mutex);
  return g_ai_disabled.contains(lifetime);
}
bool SetEntityAiDisabled(PPCContext&, uint8_t*, uint64_t lifetime, bool disabled) {
  // Input fields +820/+2076 describe family control, not arbitrary NPC brains.
  // The former implementation also called an interior MEGA branch with no
  // caller frame. Until each controller has a reviewed suspension adapter,
  // report unsupported before changing any guest memory or claiming success.
  return !disabled && Address(lifetime) && !IsAiDisabled(lifetime);
}

std::optional<Spawnable> LibraryTemplate(const uint8_t* base, size_t asset_index) {
  const auto snapshot = Library();
  std::optional<Spawnable> result;
  for (const auto* candidate : CurrentCatalog(base)) {
    if (!simpsons::modkit::LibraryTemplateMatches(*snapshot, asset_index,
            simpsons::modkit::ResourceId(candidate->record.source_guid),
            candidate->record.hash, candidate->model_resource)) continue;
    if (result && (result->record.source_guid != candidate->record.source_guid ||
                   result->record.hash != candidate->record.hash))
      return {};  // The asset has several authored actors: require a template selection.
    result = *candidate;
  }
  return result;
}

bool AnimLoopEnabled() {
  std::lock_guard lock(g_mutex);
  return g_anim_loop;
}
void SetAnimLoopEnabled(bool loop) {
  std::lock_guard lock(g_mutex);
  g_anim_loop = loop;
}
AnimTargetMode GetAnimTargetMode() {
  std::lock_guard lock(g_mutex);
  return g_anim_target_mode;
}
void SetAnimTargetMode(AnimTargetMode mode) {
  std::lock_guard lock(g_mutex);
  g_anim_target_mode = mode;
}

std::vector<AnimClipInfo> ListEntityAnimations(const uint8_t* base, uint32_t entity) {
  std::vector<AnimClipInfo> clips;
  if (!base || !entity || !Readable(entity, 320)) return clips;
  const uint32_t anim_player = LoadGuestU32(base, entity + 248);
  uint32_t char_bank = LoadGuestU32(base, entity + 236);
  if (Plausible(anim_player) && Readable(anim_player, 16)) {
    const uint32_t player_bank = LoadGuestU32(base, anim_player + 4);
    if (Plausible(player_bank)) char_bank = player_bank;
  }
  if (!Plausible(char_bank) || !Readable(char_bank, 56)) return clips;
  // 8278AD00 hashes the key modulo +4, then follows each node's +12 chain.
  // This is a bucket table, not a binary tree. 8278B478 returns node+8;
  // the node's +4 is the authored clip name (also used by 8278B6A8).
  const uint32_t table = LoadGuestU32(base, char_bank + 48);
  if (!Plausible(table) || !Readable(table, 24)) return clips;
  const uint32_t buckets = LoadGuestU32(base, table);
  const uint32_t count = LoadGuestU32(base, table + 4);
  constexpr uint32_t limit = 4096;
  if (!count || count > limit || !Plausible(buckets) || !Readable(buckets, count * 4))
    return clips;
  std::set<uint32_t> visited, keys;
  for (uint32_t bucket = 0; bucket < count; ++bucket) {
    uint32_t node = LoadGuestU32(base, buckets + bucket * 4);
    while (node) {
      if (visited.size() >= limit || !Plausible(node) || !Readable(node, 16) ||
          !visited.insert(node).second) return {};
      const uint32_t key = LoadGuestU32(base, node);
      const uint32_t name = LoadGuestU32(base, node + 4);
      const uint32_t clip = LoadGuestU32(base, node + 8);
      if (key % count != bucket || !keys.insert(key).second ||
          !Plausible(clip) || !Readable(clip, 16)) return {};
      std::string label;
      bool terminated = false;
      if (name && uint64_t(name) + 256 <= 0x100000000ull) {
        const bool whole_name = Readable(name, 256);
        for (uint32_t i = 0; i < 256 && (whole_name || Readable(name + i, 1)); ++i) {
          const auto c = simpsons::GuestToHost(base, name)[i];
          if (!c) { terminated = true; break; }
          if (c < 0x20 || c > 0x7E) break;
          label += char(c);
        }
      }
      // Never attach a plausible-looking string to another clip's hash.
      if (!terminated || label.empty() || scores::Hash(label) != key)
        label = fmt::format("Clip ({:08X})", key);
      clips.push_back({key, key, clip, std::move(label)});
      node = LoadGuestU32(base, node + 12);
    }
  }
  if (LoadGuestU32(base, entity + 248) != anim_player ||
      LoadGuestU32(base, char_bank + 48) != table ||
      LoadGuestU32(base, table) != buckets || LoadGuestU32(base, table + 4) != count)
    return {};
  std::sort(clips.begin(), clips.end(),
            [](const AnimClipInfo& a, const AnimClipInfo& b) { return a.key < b.key; });
  return clips;
}

bool PlayEntityAnimation(PPCContext& ctx, uint8_t* base, uint32_t entity, uint32_t clip_key,
                         bool loop, float speed) {
  if (!entity || !Readable(entity, 320) || !clip_key || speed != 1.0f) return false;
  const auto clips = ListEntityAnimations(base, entity);
  if (std::none_of(clips.begin(), clips.end(),
                   [&](const auto& clip) { return clip.key == clip_key; })) return false;
  const uint32_t anim_player = LoadGuestU32(base, entity + 248);
  if (!Plausible(anim_player) || !Readable(anim_player, 160)) return false;

  // Ensure animation playback is unpaused on the entity first.
  uint32_t played = 0;
  if (!CallGuest(ctx, base, 0x827867C0,
                 {anim_player, clip_key, 0u, uint32_t(loop)}, &played) || !(played & 0xFF))
    return false;
  if (!CallGuest(ctx, base, 0x82695260, {entity})) return false;
  const auto lt = Observe(entity);
  if (lt) {
    std::lock_guard lock(g_mutex);
    g_anim_overrides[lt] = {clip_key, loop, speed, false, std::chrono::steady_clock::now()};
  }
  return true;
}

bool SetEntityAnimationPaused(PPCContext& ctx, uint8_t* base, uint32_t entity, bool paused) {
  if (!entity || !Readable(entity, 320)) return false;
  if (ListEntityAnimations(base, entity).empty() ||
      !CallGuest(ctx, base, paused ? 0x82695208u : 0x82695260u, {entity})) return false;
  if (const auto lt = Lifetime(entity)) {
    std::lock_guard lock(g_mutex);
    if (auto it = g_anim_overrides.find(lt); it != g_anim_overrides.end())
      it->second.paused = paused;
  }
  return true;
}

bool SetEntityAnimationPaused(uint8_t* base, uint32_t entity, bool paused) {
  if (!entity || !Readable(entity, 320)) return false;
  if (ListEntityAnimations(base, entity).empty()) return false;
  const uint32_t flags = LoadGuestU32(base, entity + 220);
  simpsons::StoreGuestU32(base, entity + 220, paused ? (flags | 2u) : (flags & ~2u));
  if (const auto lt = Lifetime(entity)) {
    std::lock_guard lock(g_mutex);
    if (auto it = g_anim_overrides.find(lt); it != g_anim_overrides.end())
      it->second.paused = paused;
  }
  return true;
}

bool IsEntityAnimationPaused(const uint8_t* base, uint32_t entity) {
  if (!entity || !Readable(entity, 260)) return false;
  return !ListEntityAnimations(base, entity).empty() &&
         (LoadGuestU32(base, entity + 220) & 2u) != 0;
}

bool StopEntityAnimation(PPCContext& ctx, uint8_t* base, uint32_t entity) {
  if (!entity || !Readable(entity, 320)) return false;
  if (const auto lt = Lifetime(entity)) {
    std::lock_guard lock(g_mutex);
    g_anim_overrides.erase(lt);
  }
  return SetEntityAnimationPaused(ctx, base, entity, false);
}

bool StopEntityAnimation(uint8_t* base, uint32_t entity) {
  if (!entity || !Readable(entity, 320)) return false;
  if (const auto lt = Lifetime(entity)) {
    std::lock_guard lock(g_mutex);
    g_anim_overrides.erase(lt);
  }
  SetEntityAnimationPaused(base, entity, false);
  return true;
}

namespace {
std::array<float, 3> DefaultSpawnSpot(PPCContext& ctx, uint8_t* base) {
  if (const auto cam = ReadCameraPose(); cam && FreecamActive()) {
    const float pitch = cam->pitch * 0.0174532925f;
    const float yaw = cam->yaw * 0.0174532925f;
    const float fx = std::cos(pitch) * std::sin(yaw);
    const float fy = -std::sin(pitch);
    const float fz = std::cos(pitch) * std::cos(yaw);
    return {cam->position[0] + fx * 3.2f, cam->position[1] + fy * 3.2f,
            cam->position[2] + fz * 3.2f};
  }
  const auto list = players::List(base);
  for (const auto& p : list) {
    if (p.input_player == 0) {
      if (const auto tf = entity_transform::Get(ctx, base, p.entity)) {
        return {tf->position[0] + tf->forward[0] * 2.4f,
                tf->position[1] + 0.15f,
                tf->position[2] + tf->forward[2] * 2.4f};
      }
      return {p.position[0] + 2.0f, p.position[1] + 0.15f, p.position[2]};
    }
  }
  if (!list.empty())
    return {list[0].position[0] + 2.0f, list[0].position[1] + 0.15f, list[0].position[2]};
  return {0.0f, 0.0f, 0.0f};
}
}  // namespace

uint32_t PlayAnimationFromLibrary(PPCContext& ctx, uint8_t* base, size_t asset_index,
                                  size_t clip_index, AnimTargetMode target_mode, bool loop) {
  // Only an exact authored object relationship can select a source actor.
  // Unlinked animation resources need a reviewed bank/rig mapping; neither
  // a substring match nor "first NPC" is a valid fallback.
  const auto source = LibraryTemplate(base, asset_index);
  if (!source) return 0;
  const auto clips = ListEntityAnimations(base, source->record.entity);
  if (clip_index >= clips.size()) return 0;
  const uint32_t clip_key = clips[clip_index].key;
  if (target_mode == AnimTargetMode::kCurrentPlayer) {
    for (const auto& player : players::List(base))
      if (player.input_player == 0 && PlayEntityAnimation(ctx, base, player.entity, clip_key, loop))
        return player.entity;
    return 0;
  }
  if (target_mode != AnimTargetMode::kSpawnOriginalNpc) return 0;
  const uint32_t spawned = Clone(ctx, base, source->record, DefaultSpawnSpot(ctx, base), 1.0f);
  if (!spawned) return 0;
  const auto lifetime = Lifetime(spawned);
  if (!PlayEntityAnimation(ctx, base, spawned, clip_key, loop)) {
    Destroy(ctx, base, lifetime);
    return 0;
  }
  spawn_sources::RetainDebug(source->record.source_layer);
  return spawned;
}

bool SpoonerEnabled() {
  std::lock_guard lock(g_mutex);
  return g_spooner_enabled;
}
void SetSpoonerEnabled(bool enabled) {
  {
    std::lock_guard lock(g_mutex);
    g_spooner_enabled = enabled;
    if (!enabled)
      g_spooner_grabbing = false;
  }
  if (enabled && !FreecamActive())
    SetFreecam(true);
}
bool SpoonerGrabbing() {
  std::lock_guard lock(g_mutex);
  return g_spooner_grabbing;
}
void SetSpoonerGrabbing(bool grabbing) {
  std::lock_guard lock(g_mutex);
  g_spooner_grabbing = grabbing && g_spooner_selected != 0;
}
float SpoonerGrabDistance() {
  std::lock_guard lock(g_mutex);
  return g_spooner_grab_dist;
}
void SetSpoonerGrabDistance(float distance) {
  std::lock_guard lock(g_mutex);
  g_spooner_grab_dist = std::clamp(distance, 1.0f, 80.0f);
}
void AdjustSpoonerGrabDistance(float delta) {
  std::lock_guard lock(g_mutex);
  g_spooner_grab_dist = std::clamp(g_spooner_grab_dist + delta, 1.0f, 80.0f);
}
uint64_t SpoonerSelectedLifetime() {
  std::lock_guard lock(g_mutex);
  return g_spooner_selected;
}
void SetSpoonerSelectedLifetime(uint64_t lifetime) {
  std::lock_guard lock(g_mutex);
  g_spooner_selected = lifetime;
  if (!lifetime)
    g_spooner_grabbing = false;
}
uint64_t SpoonerHoveredLifetime() {
  std::lock_guard lock(g_mutex);
  return g_spooner_hovered;
}

std::optional<MapEntityInfo> DescribeEntity(PPCContext& ctx, uint8_t* base, uint64_t lifetime) {
  std::unique_lock lock(g_mutex);
  const auto it = g_instances.find(lifetime);
  if (it == g_instances.end() || !InstanceAlive(base, it->second))
    return std::nullopt;
  const Record record = it->second;
  const bool is_spawned = g_spawned_lifetimes.contains(lifetime);
  const bool ai_off = g_ai_disabled.contains(lifetime);
  std::string cached_name;
  if (const auto found = g_instance_label_cache.find(lifetime); found != g_instance_label_cache.end())
    cached_name = found->second;
  lock.unlock();

  const auto model = entity_assets::ModelOf(base, record.entity);
  const std::string model_id = model ? model->Id() : std::string();
  if (cached_name.empty()) {
    auto library = g_library ? g_library->Peek() : nullptr;
    if (library && library->ready && !model_id.empty()) {
      if (const auto* asset = library->FindNative(model_id))
        cached_name = asset->name;
    }
    if (cached_name.empty() && model && !model->name.empty())
      cached_name = Pretty(model->name);
    if (cached_name.empty())
      cached_name = Pretty(RawName(base, record.entity));
    if (cached_name.empty()) {
      const auto* kind = FindKind(record.name);
      cached_name = kind ? kind->fallback : record.name;
    }
    std::lock_guard relock(g_mutex);
    g_instance_label_cache[lifetime] = cached_name;
  }

  MapEntityInfo info;
  info.record = record;
  info.lifetime = lifetime;
  info.entity = record.entity;
  info.hash = record.hash;
  info.name = std::move(cached_name);
  info.class_name = record.name;
  info.category = CategoryOf(record.name);
  info.model_resource = model_id;
  info.is_spawned = is_spawned;
  info.is_spawned_clone = is_spawned;
  info.is_character = (info.category == "People" || info.category == "Enemies" || info.category == "Bosses");
  info.ai_disabled = ai_off;
  if (const auto rstate = entity_render::Get(base, record.entity))
    info.hidden = rstate->hide_count > 0;
  if (const auto tf = entity_transform::Get(ctx, base, record.entity)) {
    info.has_transform = true;
    info.position = tf->position;
    info.yaw_degrees = std::atan2(tf->forward[0], tf->forward[2]) * 57.2957795f;
    const float sx = std::sqrt(tf->right[0] * tf->right[0] + tf->right[1] * tf->right[1] + tf->right[2] * tf->right[2]);
    const float sy = std::sqrt(tf->up[0] * tf->up[0] + tf->up[1] * tf->up[1] + tf->up[2] * tf->up[2]);
    const float sz = std::sqrt(tf->forward[0] * tf->forward[0] + tf->forward[1] * tf->forward[1] + tf->forward[2] * tf->forward[2]);
    info.scale = {sx > 0.001f ? sx : 1.0f, sy > 0.001f ? sy : 1.0f, sz > 0.001f ? sz : 1.0f};
    std::lock_guard relock(g_mutex);
    if (!g_initial_transforms.contains(lifetime))
      g_initial_transforms[lifetime] = *tf;
    if (const auto cam = ReadCameraPose()) {
      const float dx = tf->position[0] - cam->position[0];
      const float dy = tf->position[1] - cam->position[1];
      const float dz = tf->position[2] - cam->position[2];
      info.distance = std::sqrt(dx * dx + dy * dy + dz * dz);
      info.distance_to_camera = info.distance;
    }
  }
  return info;
}

std::optional<MapEntityInfo> SpoonerSelectedInfo(PPCContext& ctx, uint8_t* base) {
  const uint64_t lt = SpoonerSelectedLifetime();
  return lt ? DescribeEntity(ctx, base, lt) : std::nullopt;
}

std::optional<MapEntityInfo> SpoonerHoveredInfo(PPCContext& ctx, uint8_t* base) {
  const uint64_t lt = SpoonerHoveredLifetime();
  return lt ? DescribeEntity(ctx, base, lt) : std::nullopt;
}

std::vector<MapEntityInfo> ListMapEntities(PPCContext& ctx, uint8_t* base, bool spawned_only) {
  std::vector<uint64_t> lifetimes;
  {
    std::lock_guard lock(g_mutex);
    lifetimes.reserve(g_instances.size());
    for (const auto& [lt, rec] : g_instances) {
      if (g_removed_map_entities.contains(lt)) continue;
      if (spawned_only && !g_spawned_lifetimes.contains(lt)) continue;
      if (InstanceAlive(base, rec))
        lifetimes.push_back(lt);
    }
  }
  std::vector<MapEntityInfo> result;
  result.reserve(lifetimes.size());
  for (const uint64_t lt : lifetimes) {
    auto info = DescribeEntity(ctx, base, lt);
    if (!info || !info->has_transform) continue;
    result.push_back(std::move(*info));
  }
  std::sort(result.begin(), result.end(), [](const MapEntityInfo& a, const MapEntityInfo& b) {
    if (a.is_spawned != b.is_spawned) return a.is_spawned > b.is_spawned;
    return a.distance < b.distance;
  });
  return result;
}

bool SpoonerSelectHovered(PPCContext& ctx, uint8_t* base) {
  const uint64_t hov = SpoonerHoveredLifetime();
  if (!hov) return false;
  const auto info = DescribeEntity(ctx, base, hov);
  if (!info || !info->has_transform) return false;
  std::lock_guard lock(g_mutex);
  g_spooner_selected = hov;
  if (info->distance >= 1.0f && info->distance <= 80.0f)
    g_spooner_grab_dist = info->distance;
  return true;
}

bool SpoonerSelectHovered() {
  std::lock_guard lock(g_mutex);
  if (!g_spooner_hovered) return false;
  g_spooner_selected = g_spooner_hovered;
  return true;
}

bool SpoonerToggleGrab(PPCContext& ctx, uint8_t* base) {
  uint64_t sel = SpoonerSelectedLifetime();
  if (!sel) {
    if (!SpoonerSelectHovered(ctx, base)) return false;
    sel = SpoonerSelectedLifetime();
  }
  const auto info = DescribeEntity(ctx, base, sel);
  if (!info || !info->has_transform) return false;
  std::lock_guard lock(g_mutex);
  g_spooner_grabbing = !g_spooner_grabbing;
  if (g_spooner_grabbing && info->distance >= 1.0f && info->distance <= 80.0f)
    g_spooner_grab_dist = info->distance;
  return g_spooner_grabbing;
}

bool SpoonerMoveSelected(PPCContext& ctx, uint8_t* base, std::array<float, 3> delta) {
  uint64_t sel = SpoonerSelectedLifetime();
  if (!sel) sel = SpoonerHoveredLifetime();
  if (!sel) return false;
  const uint32_t entity = Address(sel);
  if (!entity) return false;
  const auto tf = entity_transform::Get(ctx, base, entity);
  if (!tf) return false;
  SetSpoonerSelectedLifetime(sel);
  return entity_transform::Move(
      ctx, base, entity,
      {tf->position[0] + delta[0], tf->position[1] + delta[1], tf->position[2] + delta[2]});
}

namespace {
entity_transform::Vec3 RotateAxisAngle(const entity_transform::Vec3& v,
                                       const entity_transform::Vec3& axis, float rad) {
  const float c = std::cos(rad);
  const float s = std::sin(rad);
  const float dot = v[0] * axis[0] + v[1] * axis[1] + v[2] * axis[2];
  const entity_transform::Vec3 cross{
      axis[1] * v[2] - axis[2] * v[1],
      axis[2] * v[0] - axis[0] * v[2],
      axis[0] * v[1] - axis[1] * v[0],
  };
  return {
      v[0] * c + cross[0] * s + axis[0] * dot * (1.0f - c),
      v[1] * c + cross[1] * s + axis[1] * dot * (1.0f - c),
      v[2] * c + cross[2] * s + axis[2] * dot * (1.0f - c),
  };
}
}  // namespace

bool SpoonerRotateSelected(PPCContext& ctx, uint8_t* base, float dyaw_deg, float dpitch_deg,
                           float droll_deg) {
  uint64_t sel = SpoonerSelectedLifetime();
  if (!sel) sel = SpoonerHoveredLifetime();
  if (!sel) return false;
  const uint32_t entity = Address(sel);
  if (!entity) return false;
  auto tf = entity_transform::Get(ctx, base, entity);
  if (!tf) return false;
  SetSpoonerSelectedLifetime(sel);
  constexpr float kDegToRad = 0.017453292519943295f;
  if (std::abs(dyaw_deg) > 1e-4f) {
    const entity_transform::Vec3 up{0.0f, 1.0f, 0.0f};
    tf->right = RotateAxisAngle(tf->right, up, dyaw_deg * kDegToRad);
    tf->up = RotateAxisAngle(tf->up, up, dyaw_deg * kDegToRad);
    tf->forward = RotateAxisAngle(tf->forward, up, dyaw_deg * kDegToRad);
  }
  if (std::abs(dpitch_deg) > 1e-4f) {
    const float len = std::sqrt(tf->right[0] * tf->right[0] + tf->right[1] * tf->right[1] +
                                tf->right[2] * tf->right[2]);
    if (len > 1e-4f) {
      const entity_transform::Vec3 axis{tf->right[0] / len, tf->right[1] / len, tf->right[2] / len};
      tf->up = RotateAxisAngle(tf->up, axis, dpitch_deg * kDegToRad);
      tf->forward = RotateAxisAngle(tf->forward, axis, dpitch_deg * kDegToRad);
    }
  }
  if (std::abs(droll_deg) > 1e-4f) {
    const float len = std::sqrt(tf->forward[0] * tf->forward[0] + tf->forward[1] * tf->forward[1] +
                                tf->forward[2] * tf->forward[2]);
    if (len > 1e-4f) {
      const entity_transform::Vec3 axis{tf->forward[0] / len, tf->forward[1] / len,
                                        tf->forward[2] / len};
      tf->right = RotateAxisAngle(tf->right, axis, droll_deg * kDegToRad);
      tf->up = RotateAxisAngle(tf->up, axis, droll_deg * kDegToRad);
    }
  }
  return entity_transform::Valid(*tf) && entity_transform::Set(ctx, base, entity, *tf);
}

bool SpoonerScaleSelected(PPCContext& ctx, uint8_t* base, float factor) {
  if (!std::isfinite(factor) || factor <= 0.05f || factor > 20.0f) return false;
  uint64_t sel = SpoonerSelectedLifetime();
  if (!sel) sel = SpoonerHoveredLifetime();
  if (!sel) return false;
  const uint32_t entity = Address(sel);
  if (!entity) return false;
  auto tf = entity_transform::Get(ctx, base, entity);
  if (!tf) return false;
  SetSpoonerSelectedLifetime(sel);
  for (int i = 0; i < 3; ++i) {
    tf->right[i] *= factor;
    tf->up[i] *= factor;
    tf->forward[i] *= factor;
  }
  return entity_transform::Valid(*tf) && entity_transform::Set(ctx, base, entity, *tf);
}

bool SpoonerResetSelectedTransform(PPCContext& ctx, uint8_t* base) {
  uint64_t sel = SpoonerSelectedLifetime();
  if (!sel) sel = SpoonerHoveredLifetime();
  if (!sel) return false;
  const uint32_t entity = Address(sel);
  if (!entity) return false;
  auto tf = entity_transform::Get(ctx, base, entity);
  if (!tf) return false;
  entity_transform::Transform reset = *tf;
  {
    std::lock_guard lock(g_mutex);
    if (const auto it = g_initial_transforms.find(sel); it != g_initial_transforms.end()) {
      reset.right = it->second.right;
      reset.up = it->second.up;
      reset.forward = it->second.forward;
    } else {
      reset.right = {1.0f, 0.0f, 0.0f};
      reset.up = {0.0f, 1.0f, 0.0f};
      reset.forward = {0.0f, 0.0f, 1.0f};
    }
  }
  return entity_transform::Valid(reset) && entity_transform::Set(ctx, base, entity, reset);
}

uint32_t SpoonerCloneSelected(PPCContext& ctx, uint8_t* base) {
  uint64_t sel = SpoonerSelectedLifetime();
  if (!sel) sel = SpoonerHoveredLifetime();
  if (!sel) return 0;
  const auto info = DescribeEntity(ctx, base, sel);
  if (!info || !info->has_transform) return 0;
  std::array<float, 3> pos = info->position;
  pos[0] += 1.25f;
  if (const auto cam = ReadCameraPose(); cam && FreecamActive()) {
    const float pitch = cam->pitch * 0.0174532925f;
    const float yaw = cam->yaw * 0.0174532925f;
    const float dist = SpoonerGrabDistance();
    pos = {cam->position[0] + std::cos(pitch) * std::sin(yaw) * dist,
           cam->position[1] - std::sin(pitch) * dist,
           cam->position[2] + std::cos(pitch) * std::cos(yaw) * dist};
  }
  const uint32_t cloned = Clone(ctx, base, info->record, pos, 1.0f);
  if (!cloned) return 0;
  spawn_sources::RetainDebug(info->record.source_layer);
  if (const auto orig_tf = entity_transform::Get(ctx, base, info->record.entity)) {
    auto new_tf = *orig_tf;
    new_tf.position = pos;
    if (entity_transform::Valid(new_tf))
      entity_transform::Set(ctx, base, cloned, new_tf);
  }
  SetSpoonerSelectedLifetime(Lifetime(cloned));
  return cloned;
}

uint32_t SpoonerSwapSelected(PPCContext& ctx, uint8_t* base, const Record& replacement) {
  uint64_t selected = SpoonerSelectedLifetime();
  if (!selected) selected = SpoonerHoveredLifetime();
  const auto entity = Address(selected);
  if (!entity) return 0;
  const auto transform = entity_transform::Get(ctx, base, entity);
  if (!transform) return 0;
  const auto spawned = Clone(ctx, base, replacement, transform->position, 1.0f);
  if (!spawned) return 0;
  const auto lifetime = Lifetime(spawned);
  if (!entity_transform::Set(ctx, base, spawned, *transform)) {
    Destroy(ctx, base, lifetime);
    return 0;
  }
  SetSpoonerSelectedLifetime(selected);
  if (!SpoonerDeleteSelected(ctx, base)) {
    Destroy(ctx, base, lifetime);
    SetSpoonerSelectedLifetime(selected);
    return 0;
  }
  spawn_sources::RetainDebug(replacement.source_layer);
  SetSpoonerSelectedLifetime(lifetime);
  return spawned;
}

void VisibilityReset(uint64_t lifetime) {
  std::lock_guard lock(g_mutex);
  if (const auto it = g_removed_map_entities.find(lifetime); it != g_removed_map_entities.end())
    it->second.hid_entity = false;
}

bool SpoonerDeleteSelected(PPCContext& ctx, uint8_t* base) {
  uint64_t selected = SpoonerSelectedLifetime();
  if (!selected) selected = SpoonerHoveredLifetime();
  const auto info = DescribeEntity(ctx, base, selected);
  if (!info) return false;
  for (const auto& player : players::List(base))
    if (player.entity == info->entity) return false;
  if (info->is_spawned) {
    if (!Destroy(ctx, base, selected)) return false;
    SetSpoonerSelectedLifetime(0);
    return true;
  }

  // Existing level actors still belong to their scripts. This reversible
  // editor removal hides and parks them; it cannot suspend arbitrary AI or
  // make authored mission references safe to delete.
  const auto entity = info->entity;
  const auto transform = entity_transform::Get(ctx, base, entity);
  if (!transform || !entity_render::Get(base, entity)) return false;
  {
    std::lock_guard lock(g_mutex);
    if (g_removed_map_entities.contains(selected)) return false;
    g_removed_map_entities.emplace(selected, RemovedMapEntity{selected, *transform, false});
  }
  if (!entity_render::Change(ctx, base, entity, true)) {
    std::lock_guard lock(g_mutex);
    g_removed_map_entities.erase(selected);
    return false;
  }
  {
    std::lock_guard lock(g_mutex);
    g_removed_map_entities.at(selected).hid_entity = true;
  }
  if (!entity_transform::Move(ctx, base, entity,
      {transform->position[0], transform->position[1] - 500.0f, transform->position[2]})) {
    bool hidden = false;
    {
      std::lock_guard lock(g_mutex);
      hidden = g_removed_map_entities.at(selected).hid_entity;
      g_removed_map_entities.erase(selected);
    }
    if (hidden) entity_render::Change(ctx, base, entity, false);
    return false;
  }
  SetSpoonerSelectedLifetime(0);
  {
    std::lock_guard lock(g_mutex);
    if (g_spooner_hovered == selected) g_spooner_hovered = 0;
  }
  return true;
}

size_t SpoonerRemovedMapEntityCount() {
  std::lock_guard lock(g_mutex);
  return g_removed_map_entities.size();
}

size_t SpoonerRestoreRemovedMapEntities(PPCContext& ctx, uint8_t* base) {
  std::map<uint64_t, RemovedMapEntity> pending;
  {
    std::lock_guard lock(g_mutex);
    pending = g_removed_map_entities;
  }
  size_t count = 0;
  for (const auto& [lifetime, item] : pending) {
    const auto entity = Address(lifetime);
    if (!entity) {
      std::lock_guard lock(g_mutex);
      g_removed_map_entities.erase(lifetime);
      continue;
    }
    if (!entity_transform::Set(ctx, base, entity, item.original_transform)) continue;
    bool hidden = false;
    {
      std::lock_guard lock(g_mutex);
      const auto it = g_removed_map_entities.find(lifetime);
      if (it == g_removed_map_entities.end()) continue;
      hidden = it->second.hid_entity;
    }
    if (hidden && !entity_render::Change(ctx, base, entity, false)) continue;
    std::lock_guard lock(g_mutex);
    g_removed_map_entities.erase(lifetime);
    ++count;
  }
  return count;
}

void TickSpooner(PPCContext& ctx, uint8_t* base) {
  // 1. Check pending animation spawn waiting on a stream load
  {
    PendingAnimSpawn pending;
    {
      std::lock_guard lock(g_mutex);
      if (g_pending_anim.active &&
          (g_pending_anim.stream.empty() || spawn_sources::IsReady(g_pending_anim.stream))) {
        pending = g_pending_anim;
        g_pending_anim.active = false;
      } else if (g_pending_anim.active &&
                 spawn_sources::DebugStatus().phase == spawn_sources::Phase::failed) {
        g_pending_anim.active = false;
      }
    }
    if (pending.active) {
      WarmCatalog(base);
      PlayAnimationFromLibrary(ctx, base, pending.asset_index, pending.clip_index,
                               pending.target_mode, pending.loop);
    }
  }

  // The native animation player handles looping. Restarting every 1.4 s
  // truncated long clips and overrode scene/controller animation transitions.

  // 3. Spooner raycast & grab update
  if (!SpoonerEnabled()) return;
  const auto cam = ReadCameraPose();
  if (!cam) return;

  const float pitch = cam->pitch * 0.0174532925f;
  const float yaw = cam->yaw * 0.0174532925f;
  const float fx = std::cos(pitch) * std::sin(yaw);
  const float fy = -std::sin(pitch);
  const float fz = std::cos(pitch) * std::cos(yaw);

  if (SpoonerGrabbing()) {
    const uint64_t sel = SpoonerSelectedLifetime();
    const uint32_t entity = Address(sel);
    if (entity && Readable(entity, kFrame + 4)) {
      const float dist = SpoonerGrabDistance();
      const std::array<float, 3> target_pos{
          cam->position[0] + fx * dist,
          cam->position[1] + fy * dist,
          cam->position[2] + fz * dist,
      };
      entity_transform::Move(ctx, base, entity, target_pos);
    } else {
      SetSpoonerGrabbing(false);
    }
  }

  // Ray-cone pick closest entity to crosshair using direct RenderWare frame reads
  std::vector<Record> records = Instances(base);
  std::set<uint64_t> removed_copy;
  {
    std::lock_guard lock(g_mutex);
    for (const auto& [lt, _] : g_removed_map_entities) removed_copy.insert(lt);
  }
  uint64_t best_lt = 0;
  float best_score = 1e9f;
  for (const auto& rec : records) {
    if (removed_copy.contains(rec.lifetime)) continue;
    if (!Readable(rec.entity, kFrame + 4)) continue;
    const uint32_t frame = LoadGuestU32(base, rec.entity + kFrame);
    if (!frame || !Readable(frame, 76)) continue;
    const float px = simpsons::LoadGuestFloat(base, frame + 64);
    const float py = simpsons::LoadGuestFloat(base, frame + 68);
    const float pz = simpsons::LoadGuestFloat(base, frame + 72);
    if (!std::isfinite(px) || !std::isfinite(py) || !std::isfinite(pz)) continue;
    const float dx = px - cam->position[0];
    const float dy = py - cam->position[1];
    const float dz = pz - cam->position[2];
    const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (dist < 0.4f || dist > 120.0f) continue;
    const float along = dx * fx + dy * fy + dz * fz;
    if (along <= 0.2f) continue;
    const float perp_sq = std::max(0.0f, dist * dist - along * along);
    const float perp = std::sqrt(perp_sq);
    const float cone_radius = std::max(0.85f, along * 0.14f);
    if (perp <= cone_radius) {
      const float score = perp * 3.0f + along * 0.15f;
      if (score < best_score) {
        best_score = score;
        best_lt = rec.lifetime;
      }
    }
  }
  {
    std::lock_guard lock(g_mutex);
    g_spooner_hovered = best_lt;
  }
}

}  // namespace spawner

REX_FUNC(sub_82703CA8) {
  const uint32_t hash = ctx.r3.u32;
  const uint32_t data = ctx.r4.u32;
  const PPCContext saved = ctx;
  __imp__sub_82703CA8(ctx, base);
  const uint32_t entity = ctx.r3.u32;
  if (!entity)
    return;
  const PPCContext result = ctx;
  ctx = saved;
  const std::string name = spawner::ClassName(ctx, base, hash);
  ctx = result;
  // Base destruction retires the previous lifetime before allocation reuse.
  // A factory returning an already live object retains that object's identity.
  const auto lifetime = spawner::Observe(entity);
  const spawner::Record record{name, hash, data, entity, simpsons::LoadGuestU32(base, entity), lifetime};
  {
    std::lock_guard lock(spawner::g_mutex);
    spawner::g_instances[lifetime] = record;
    ++spawner::g_instance_revision;
  }
  if (spawner::g_cloning) return;
  {
    std::lock_guard lock(spawner::g_mutex);
    spawner::g_records[name] = record;
    auto& all = spawner::g_all[name];
    if (all.size() < spawner::kPerClass)
      all.push_back(record);
    ++spawner::g_generation;
  }
  if (REXCVAR_GET(debug_menu_log_entities))
    REXLOG_INFO("[spawner] {} (class {:08X}) record {:08X} -> entity {:08X} vtable {:08X}", name,
                hash, data, entity, simpsons::LoadGuestU32(base, entity));
}

REX_FUNC(sub_8269BC10) {
  spawner::Forget(ctx.r3.u32);
  __imp__sub_8269BC10(ctx, base);
}
