#include "native_ragdoll_driver.h"

#include "guest_call_frame.h"
#include "guest_memory.h"
#include "spawner.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <limits>
#include <map>
#include <mutex>
#include <thread>

#include <rex/hook.h>

namespace native_ragdoll {
namespace {
struct Driver {
  uint64_t rig = 0;
  uint32_t animation = 0;
  std::thread::id thread;
  BipedRig definition;
  PoseDriverStats stats;
};
std::recursive_mutex g_mutex;
std::map<uint64_t, Driver> g_drivers;
uint64_t g_next_driver = 0;
std::atomic<bool> g_enabled{false};
thread_local bool t_writing = false;

void EnabledChanged() {
  g_enabled.store(std::any_of(g_drivers.begin(), g_drivers.end(),
      [](const auto& value) { return value.second.stats.enabled; }), std::memory_order_release);
}

bool SameSkeleton(const SkeletonPose& a, const SkeletonPose& b) {
  if (a.actor != b.actor || a.world_epoch != b.world_epoch || a.character != b.character ||
      a.model_guid != b.model_guid || a.model_nodes != b.model_nodes || a.bones.size() != b.bones.size())
    return false;
  for (size_t i = 0; i < a.bones.size(); ++i)
    if (a.bones[i].id != b.bones[i].id || a.bones[i].model_index != b.bones[i].model_index ||
        a.bones[i].parent != b.bones[i].parent) return false;
  return true;
}
}  // namespace

uint64_t AttachFamilyPose(PPCContext& ctx, uint8_t* base, uint64_t rig,
                          const BipedRig& definition) {
  if (!rig || RigActor(rig) != definition.source.actor) return 0;
  const auto bodies = ReadRig(ctx, base, rig);
  const auto current = ReadFamilySkeleton(ctx, base, definition.source.actor);
  if (!bodies || !current || !SameSkeleton(*current, definition.source) ||
      !EvaluateBipedRig(definition, *bodies, current->world_transform)) return 0;
  const auto entity = spawner::Address(current->actor);
  if (!entity || !spawner::Readable(entity, 252)) return 0;
  const auto animation = simpsons::LoadGuestU32(base, entity + 248);
  if (!animation) return 0;
  std::lock_guard lock(g_mutex);
  for (auto it = g_drivers.begin(); it != g_drivers.end();) {
    if (!RigActor(it->second.rig)) it = g_drivers.erase(it);
    else if (it->second.definition.source.actor == current->actor) return 0;
    else ++it;
  }
  EnabledChanged();
  if (g_drivers.size() >= 2 || g_next_driver == std::numeric_limits<uint64_t>::max()) return 0;
  const auto id = ++g_next_driver;
  g_drivers.emplace(id, Driver{rig, animation, std::this_thread::get_id(), definition, {}});
  return id;
}

bool SetFamilyPoseEnabled(uint64_t id, bool enabled) {
  std::lock_guard lock(g_mutex);
  const auto found = g_drivers.find(id);
  if (found == g_drivers.end() || found->second.thread != std::this_thread::get_id() ||
      (enabled && RigActor(found->second.rig) != found->second.definition.source.actor)) return false;
  found->second.stats.enabled = enabled;
  EnabledChanged();
  return true;
}

bool DetachFamilyPose(uint64_t& id) {
  std::lock_guard lock(g_mutex);
  const auto found = g_drivers.find(id);
  if (found != g_drivers.end()) {
    if (found->second.thread != std::this_thread::get_id()) return false;
    g_drivers.erase(found);
    EnabledChanged();
  }
  id = 0;
  return true;
}

std::optional<PoseDriverStats> FamilyPoseStats(uint64_t id) {
  std::lock_guard lock(g_mutex);
  const auto found = g_drivers.find(id);
  return found == g_drivers.end() ? std::nullopt : std::optional<PoseDriverStats>(found->second.stats);
}

void AfterFamilyAnimation(PPCContext& ctx, uint8_t* base, uint32_t animation) {
  if (!animation || t_writing || !g_enabled.load(std::memory_order_acquire)) return;
  std::lock_guard lock(g_mutex);
  for (auto& [id, driver] : g_drivers) {
    auto& stats = driver.stats;
    if (driver.animation != animation || !stats.enabled) continue;
    ++stats.callbacks;
    // Do not invoke native physics or mutate palettes from an animation job
    // worker. Its occurrence remains visible to qualification/diagnostic code.
    if (driver.thread != std::this_thread::get_id()) {
      ++stats.foreign_thread_callbacks;
      stats.enabled = false;
      EnabledChanged();
      continue;
    }
    simpsons::GuestContextScope preserve(ctx);
    const auto started = std::chrono::steady_clock::now();
    bool written = false;
    t_writing = true;
    try {
      const auto bodies = ReadRig(ctx, base, driver.rig);
      // Do not traverse a stale actor at all if its owned rig has retired.
      if (bodies) {
        const auto current = ReadFamilySkeleton(ctx, base, driver.definition.source.actor);
        if (current && SameSkeleton(*current, driver.definition.source)) {
          const auto evaluated = EvaluateBipedRig(driver.definition, *bodies, current->world_transform);
          if (evaluated) {
            auto candidate = *current;
            candidate.bones = *evaluated;
            written = WriteFamilySkeleton(ctx, base, candidate);
          }
        }
      }
    } catch (...) {
      written = false;
    }
    t_writing = false;
    if (written) ++stats.writes;
    else { ++stats.failures; stats.enabled = false; EnabledChanged(); }
    const auto elapsed = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - started).count());
    stats.total_nanoseconds += elapsed;
    stats.maximum_nanoseconds = std::max(stats.maximum_nanoseconds, elapsed);
  }
}
}  // namespace native_ragdoll

REX_EXTERN(__imp__sub_82789028);
REX_HOOK_RAW(sub_82789028) {
  const auto animation = ctx.r3.u32;
  __imp__sub_82789028(ctx, base);
  native_ragdoll::AfterFamilyAnimation(ctx, base, animation);
}
