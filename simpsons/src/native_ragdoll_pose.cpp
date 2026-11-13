#include "native_ragdoll_pose.h"

#include "entity_assets.h"
#include "game_thread.h"
#include "guest_memory.h"
#include "host_memory.h"
#include "players.h"
#include "spawner.h"

#include <algorithm>
#include <cmath>

namespace native_ragdoll {
namespace {
constexpr uint32_t kFindClump = 0x82693AE8, kHierarchy = 0x826B8970;
constexpr uint32_t kDescription = 0x82785EC0;
constexpr uint32_t kMaxNodes = 256;
uint32_t g_clump_scratch = 0;

bool Readable(uint32_t at, uint32_t bytes) {
  return at && !(at & 3) && spawner::Readable(at, bytes);
}

bool ReadPose(const uint8_t* base, uint32_t at, entity_transform::Transform& pose) {
  if ((at & 15) || !Readable(at, 64))
    return false;
  for (auto* column : {&pose.right, &pose.up, &pose.forward, &pose.position}) {
    for (uint32_t axis = 0; axis < 3; ++axis) {
      (*column)[axis] = simpsons::LoadGuestFloat(base, at + axis * 4);
      if (!std::isfinite((*column)[axis]))
        return false;
    }
    at += 16;
  }
  return true;
}
}  // namespace

std::optional<SkeletonPose> ReadFamilySkeleton(PPCContext& ctx, uint8_t* base, uint64_t actor) {
  if (!base || !actor)
    return {};
  const auto entity = spawner::Address(actor);
  if (!Readable(entity, 324))
    return {};
  const auto family = players::List(base);
  const auto player = std::find_if(family.begin(), family.end(),
                                   [&](const auto& value) { return value.entity == entity; });
  if (player == family.end())
    return {};
  const auto model_id = entity_assets::ModelOf(base, entity);
  const auto entity_pose = entity_transform::Get(ctx, base, entity);
  if (!model_id || !entity_pose || spawner::Address(actor) != entity)
    return {};
  const auto world_epoch = spawner::WorldEpoch();
  const auto wrapper = simpsons::LoadGuestU32(base, entity + 136);
  const auto anim = simpsons::LoadGuestU32(base, entity + 248);
  const auto palette = simpsons::LoadGuestU32(base, entity + 312);
  const auto palette_count = simpsons::LoadGuestU16(base, entity + 316);
  if (!Readable(wrapper, 20) || !Readable(anim, 640) || !palette_count ||
      palette_count > kMaxNodes || !Readable(palette, palette_count * 64) ||
      simpsons::LoadGuestU32(base, anim + 92) != palette)
    return {};
  const auto bank = simpsons::LoadGuestU32(base, anim + 4);
  if (!Readable(bank, 64) || bank != simpsons::LoadGuestU32(base, entity + 236))
    return {};
  // The lookup helper uses player+8; the description getter uses player+10.
  // During a transition they need not identify the same skeleton. Never read
  // one description's parent records with the other's track indices.
  const auto description_index = simpsons::LoadGuestU16(base, anim + 10);
  if (description_index != simpsons::LoadGuestU16(base, anim + 8) || int16_t(description_index) < 0)
    return {};
  const auto descriptions = simpsons::LoadGuestU32(base, bank + 28);
  const auto description_count = simpsons::LoadGuestU32(base, bank + 24);
  if (!description_count || description_count > 4096 || description_index >= description_count ||
      !Readable(descriptions, description_count * 24))
    return {};
  const auto payload = simpsons::LoadGuestU32(base, wrapper + 16);
  if (!Readable(payload, 80))
    return {};
  if (!g_clump_scratch)
    g_clump_scratch = host_memory::Alloc(16);
  if (!g_clump_scratch || !Readable(g_clump_scratch, 16))
    return {};
  simpsons::StoreGuestU32(base, g_clump_scratch, 0);
  simpsons::StoreGuestU32(base, g_clump_scratch + 4, 0);
  // The wrapper is a model resource, not a clump. This is the same native
  // resource traversal used when copying an Animated's render palette.
  if (!CallGuest(ctx, base, kFindClump, {payload, g_clump_scratch, g_clump_scratch + 4}))
    return {};
  const auto clump = simpsons::LoadGuestU32(base, g_clump_scratch);
  uint32_t hierarchy = 0, description = 0;
  if (!Readable(clump, 8) || !CallGuest(ctx, base, kHierarchy, {clump}, &hierarchy) ||
      !Readable(hierarchy, 32) || !CallGuest(ctx, base, kDescription, {anim}, &description) ||
      !Readable(description, 24) || description != descriptions + description_index * 24)
    return {};
  const auto node_count = simpsons::LoadGuestU32(base, hierarchy + 4);
  const auto nodes = simpsons::LoadGuestU32(base, hierarchy + 16);
  const auto tracks = simpsons::LoadGuestU32(base, description + 8);
  const auto parents = simpsons::LoadGuestU32(base, description + 12);
  const auto lookup = simpsons::LoadGuestU32(base, description + 20);
  const auto mapping = simpsons::LoadGuestU32(base, anim + 96);
  const auto mapping_bytes = simpsons::LoadGuestU32(base, anim + 88);
  if (!node_count || node_count > kMaxNodes || !tracks || tracks > kMaxNodes ||
      tracks > mapping_bytes || mapping_bytes > kMaxNodes || !Readable(nodes, node_count * 16) ||
      !Readable(parents, tracks * 16) || !Readable(lookup, tracks * 20) ||
      !Readable(mapping, mapping_bytes))
    return {};
  // The native track search is a bounded binary search. Validate the source
  // table before invoking it; indices must be unique and in this description.
  std::vector<bool> seen(tracks, false);
  std::vector<std::pair<uint32_t, uint32_t>> track_lookup;
  track_lookup.reserve(tracks);
  uint32_t previous_key = 0;
  for (uint32_t index = 0; index < tracks; ++index) {
    const auto key = simpsons::LoadGuestU32(base, lookup + index * 20);
    const auto track = simpsons::LoadGuestU32(base, lookup + index * 20 + 16);
    if ((index && key <= previous_key) || track >= tracks || seen[track])
      return {};
    previous_key = key;
    seen[track] = true;
    track_lookup.emplace_back(key, track);
  }
  SkeletonPose result;
  result.actor = actor;
  result.character = player->character;
  result.world_epoch = world_epoch;
  result.model_guid = model_id->guid;
  result.world_transform = *entity_pose;
  result.model_nodes = node_count;
  result.bones.resize(tracks);
  std::fill(seen.begin(), seen.end(), false);
  std::vector<bool> model_indices(palette_count, false);
  for (uint32_t node = 0; node < node_count; ++node) {
    const auto id = simpsons::LoadGuestU32(base, nodes + node * 16);
    const auto index = simpsons::LoadGuestU32(base, nodes + node * 16 + 4);
    if (index >= palette_count || model_indices[index])
      return {};
    model_indices[index] = true;
    // 82786D60 -> 82799058 -> 82798FD8 searches this exact sorted table
    // with unsigned keys and returns record+16. The validated local copy
    // avoids a full guest-call frame for every bone on every pose read/write.
    const auto match = std::lower_bound(track_lookup.begin(), track_lookup.end(), id,
        [](const auto& entry, uint32_t key) { return entry.first < key; });
    if (match == track_lookup.end() || match->first != id)
      continue;  // model-only nodes are not animation tracks
    const auto track = match->second;
    if (track >= tracks || seen[track] || simpsons::GuestToHost(base, mapping)[track] != index)
      return {};
    const auto parent = simpsons::LoadGuestU32(base, parents + track * 16);
    if (track ? parent >= track : parent != UINT32_MAX)
      return {};
    auto& bone = result.bones[track];
    bone.id = id;
    bone.model_index = index;
    bone.parent = track ? int32_t(parent) : -1;
    if (!ReadPose(base, palette + index * 64, bone.model_transform))
      return {};
    seen[track] = true;
  }
  if (std::find(seen.begin(), seen.end(), false) != seen.end() ||
      spawner::Address(actor) != entity || spawner::WorldEpoch() != world_epoch ||
      simpsons::LoadGuestU32(base, entity + 136) != wrapper ||
      simpsons::LoadGuestU32(base, wrapper + 16) != payload ||
      simpsons::LoadGuestU32(base, entity + 248) != anim ||
      simpsons::LoadGuestU32(base, anim + 4) != bank ||
      simpsons::LoadGuestU16(base, anim + 8) != description_index ||
      simpsons::LoadGuestU16(base, anim + 10) != description_index ||
      simpsons::LoadGuestU32(base, entity + 312) != palette ||
      simpsons::LoadGuestU16(base, entity + 316) != palette_count)
    return {};
  const auto current_model = entity_assets::ModelOf(base, entity);
  if (!current_model || current_model->guid != result.model_guid)
    return {};
  return result;
}

bool WriteFamilySkeleton(PPCContext& ctx, uint8_t* base, const SkeletonPose& pose) {
  const auto current = ReadFamilySkeleton(ctx, base, pose.actor);
  if (!current || current->world_epoch != pose.world_epoch ||
      current->character != pose.character || current->model_guid != pose.model_guid ||
      current->model_nodes != pose.model_nodes || current->bones.size() != pose.bones.size())
    return false;
  const auto& a = current->world_transform;
  const auto& b = pose.world_transform;
  if (a.right != b.right || a.up != b.up || a.forward != b.forward || a.position != b.position)
    return false;
  // Validate the whole candidate first. Invalid late tracks must not leave
  // half a character in a new pose. No guest call occurs inside the commit.
  for (size_t i = 0; i < pose.bones.size(); ++i) {
    const auto& bone = pose.bones[i];
    const auto& expected = current->bones[i];
    if (bone.id != expected.id || bone.model_index != expected.model_index ||
        bone.parent != expected.parent || !entity_transform::Finite(bone.model_transform))
      return false;
  }
  const auto entity = spawner::Address(pose.actor);
  if (!Readable(entity, 324) || spawner::WorldEpoch() != pose.world_epoch) return false;
  const auto palette = simpsons::LoadGuestU32(base, entity + 312);
  const auto count = simpsons::LoadGuestU16(base, entity + 316);
  if (!count || count > kMaxNodes || (palette & 15) || !Readable(palette, count * 64)) return false;
  for (const auto& bone : pose.bones)
    if (bone.model_index >= count) return false;
  for (const auto& bone : pose.bones) {
    auto at = palette + bone.model_index * 64;
    for (const auto& column : {bone.model_transform.right, bone.model_transform.up,
                               bone.model_transform.forward, bone.model_transform.position}) {
      for (uint32_t axis = 0; axis < 3; ++axis)
        simpsons::StoreGuestFloat(base, at + axis * 4, column[axis]);
      at += 16;
    }
  }
  return true;
}

}  // namespace native_ragdoll
