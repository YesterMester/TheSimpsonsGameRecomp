#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct PPCContext;
namespace simpsons::modkit { struct AssetSnapshot; }
namespace rex {
class Runtime;
}

// Entities created from level data (spawner.cpp).
namespace spawner {

struct Record {
  std::string name;   // the entity class
  uint32_t hash = 0;  // its class hash
  uint32_t data = 0;  // the record it was built from
  uint32_t entity = 0;
  uint32_t vtable = 0;  // the entity's, to see it is still there
  uint64_t lifetime = 0;  // expires at the base destructor, even if memory is reused
  // Resident definitions survive the source instance, but never their native
  // stream load or world generation. These fields are host-owned identity.
  std::array<uint32_t, 4> source_guid{};
  uint32_t source_layer = 0, source_world = 0;
  uint64_t source_epoch = 0;
};

// Something this level has loaded that can be spawned again, by name.
struct Spawnable {
  std::string name;      // "Compact car", "Pitchfork", "Agnes Skinner"
  std::string category;  // "Vehicles", "Props", "Weapons", "Toys", "People", ...
  Record record;         // the latest one built
  int seen = 0;          // how many the level has
  std::string model_resource;  // native resource GUID, when the layout is supported
};

void Init(rex::Runtime* runtime);
void Shutdown();
// Entire installed game and enabled mods; indexing begins on the first call.
std::shared_ptr<const simpsons::modkit::AssetSnapshot> Library();

// The latest record of each class built in this level.
std::vector<Record> Records();
// Current factory instances, including editor/mod copies. Lifetime validation
// excludes destroyed objects and reused addresses; definitions are not actors.
std::vector<Record> Instances(const uint8_t* base);
uint64_t InstanceRevision();
// Game thread: what can be spawned here, by category and name.
const std::vector<Spawnable>& Catalog(const uint8_t* base);
// Refresh changing native labels outside a timed mod callback. Queries reuse
// that snapshot while still checking current stream and definition identities.
void WarmCatalog(const uint8_t* base);
// Native factory names for supported placeable classes, including ones which
// have no live instance in the current scene.
std::map<uint32_t, std::string> PlaceableClasses(const uint8_t* base);
std::optional<Spawnable> FindTemplate(const uint8_t* base, std::string_view source_guid,
                                     uint32_t hash);
// A game-thread query validates stream loads once for the whole batch. Pointers
// remain valid until the next catalog update; consume them in this frame.
std::vector<const Spawnable*> CurrentCatalog(const uint8_t* base);
bool TemplateValid(const uint8_t* base, const Record& record);
// A source load changed the world's retained definitions.
void InvalidateCatalog();
uint64_t WorldEpoch();
// The level is unloading: its records go with it.
void Reset();

// Game thread: builds another entity from a record, as the level loader does,
// and puts it at a position. `radius` is the room it takes on the ground, for
// Placed. Returns the new entity, or 0.
uint32_t Clone(PPCContext& ctx, uint8_t* base, const Record& record,
               const std::array<float, 3>& position, float radius = 1.0f,
               std::string_view owner = {});

// Identities are host tokens, never guest addresses. Observe is for existing
// entities (such as players); the factory and base destructor track reuse.
uint64_t Observe(uint32_t entity);
uint64_t Lifetime(uint32_t entity);
uint32_t Address(uint64_t lifetime);
bool Readable(uint32_t address, uint32_t bytes);
// Only pass a lifetime acquired for an owned spawn, after callbacks finish.
bool Destroy(PPCContext& ctx, uint8_t* base, uint64_t lifetime);
// Releasing the menu's claim on a shared source removes its own copies while
// preserving mod-owned instances and the resource buffers those mods retain.
void DestroyDebugCopies(PPCContext& ctx, uint8_t* base, const std::vector<uint32_t>& layers);
// Find a mod's reserved GUID in the world's direct-entity index. Used after
// an exact snapshot restores the original world, without creating a copy.
uint32_t FindOwned(PPCContext& ctx, uint8_t* base, std::string_view owner,
                   const std::array<uint32_t, 4>& guid, uint32_t vtable);

// Something spawned earlier that is still there, where it is now.
struct Placed {
  std::array<float, 3> position;
  float radius;
};
// Game thread: the latest spawns still in this level.
std::vector<Placed> Spawned(PPCContext& ctx, uint8_t* base);

// AI control and animation playback options for spawned/existing NPCs & players.
// Controller suspension is unsupported until the class has a reviewed adapter.
// A failed request does not mutate memory or silently change the requested policy.
bool SpawnWithAi();
void SetSpawnWithAi(bool enabled);
bool IsAiDisabled(uint64_t lifetime);
bool SetEntityAiDisabled(PPCContext& ctx, uint8_t* base, uint64_t lifetime, bool disabled);

enum class AnimTargetMode {
  kCurrentPlayer = 0,
  kSpawnSelectedNpc = 1,
  kSpawnOriginalNpc = 2,
};

struct AnimClipInfo {
  uint32_t key = 0;
  uint32_t clip_key = 0;
  uint32_t clip_ptr = 0;
  std::string label;
};

// Resolve an unambiguous authored GUID/class/model relationship; never guess
// from a display name or substitute the first available character.
std::optional<Spawnable> LibraryTemplate(const uint8_t* base, size_t asset_index);

bool AnimLoopEnabled();
void SetAnimLoopEnabled(bool loop);
AnimTargetMode GetAnimTargetMode();
void SetAnimTargetMode(AnimTargetMode mode);
std::vector<AnimClipInfo> ListEntityAnimations(const uint8_t* base, uint32_t entity);
bool PlayEntityAnimation(PPCContext& ctx, uint8_t* base, uint32_t entity, uint32_t clip_key,
                         bool loop, float speed = 1.0f);
bool SetEntityAnimationPaused(PPCContext& ctx, uint8_t* base, uint32_t entity, bool paused);
bool SetEntityAnimationPaused(uint8_t* base, uint32_t entity, bool paused);
bool IsEntityAnimationPaused(const uint8_t* base, uint32_t entity);
bool StopEntityAnimation(PPCContext& ctx, uint8_t* base, uint32_t entity);
bool StopEntityAnimation(uint8_t* base, uint32_t entity);
uint32_t PlayAnimationFromLibrary(PPCContext& ctx, uint8_t* base, size_t asset_index,
                                  size_t clip_index, AnimTargetMode target_mode, bool loop);
inline uint32_t PlayAnimationFromLibrary(PPCContext& ctx, uint8_t* base, size_t asset_index,
                                         AnimTargetMode target_mode, bool loop) {
  return PlayAnimationFromLibrary(ctx, base, asset_index, 0, target_mode, loop);
}

// Spooner mode: freecam world object & entity editor.
struct MapEntityInfo {
  Record record;
  uint64_t lifetime = 0;
  uint32_t entity = 0;
  uint32_t hash = 0;
  std::string name;
  std::string class_name;
  std::string category;
  std::string model_resource;
  std::array<float, 3> position{};
  float yaw_degrees = 0.0f;
  std::array<float, 3> scale{1.0f, 1.0f, 1.0f};
  float radius = 1.0f;
  float distance = 0.0f;
  float distance_to_camera = 0.0f;
  bool is_spawned = false;
  bool is_spawned_clone = false;
  bool is_character = false;
  bool ai_disabled = false;
  bool hidden = false;
  bool has_transform = false;
};

bool SpoonerEnabled();
void SetSpoonerEnabled(bool enabled);
bool SpoonerGrabbing();
void SetSpoonerGrabbing(bool grabbing);
float SpoonerGrabDistance();
void SetSpoonerGrabDistance(float distance);
void AdjustSpoonerGrabDistance(float delta);
uint64_t SpoonerSelectedLifetime();
void SetSpoonerSelectedLifetime(uint64_t lifetime);
uint64_t SpoonerHoveredLifetime();
std::optional<MapEntityInfo> DescribeEntity(PPCContext& ctx, uint8_t* base, uint64_t lifetime);
std::optional<MapEntityInfo> SpoonerSelectedInfo(PPCContext& ctx, uint8_t* base);
std::optional<MapEntityInfo> SpoonerHoveredInfo(PPCContext& ctx, uint8_t* base);
std::vector<MapEntityInfo> ListMapEntities(PPCContext& ctx, uint8_t* base,
                                           bool spawned_only = false);
bool SpoonerSelectHovered(PPCContext& ctx, uint8_t* base);
bool SpoonerSelectHovered();
bool SpoonerToggleGrab(PPCContext& ctx, uint8_t* base);
bool SpoonerMoveSelected(PPCContext& ctx, uint8_t* base, std::array<float, 3> delta);
bool SpoonerRotateSelected(PPCContext& ctx, uint8_t* base, float dyaw_deg,
                           float dpitch_deg = 0.0f, float droll_deg = 0.0f);
bool SpoonerScaleSelected(PPCContext& ctx, uint8_t* base, float factor);
bool SpoonerResetSelectedTransform(PPCContext& ctx, uint8_t* base);
uint32_t SpoonerCloneSelected(PPCContext& ctx, uint8_t* base);
uint32_t SpoonerSwapSelected(PPCContext& ctx, uint8_t* base, const Record& replacement);
bool SpoonerDeleteSelected(PPCContext& ctx, uint8_t* base);
size_t SpoonerRemovedMapEntityCount();
size_t SpoonerRestoreRemovedMapEntities(PPCContext& ctx, uint8_t* base);
void VisibilityReset(uint64_t lifetime);
void TickSpooner(PPCContext& ctx, uint8_t* base);

}  // namespace spawner
