#include "spawn_sources.h"

#include "game_thread.h"
#include "guest_memory.h"
#include "host_memory.h"
#include "modkit_game.h"
#include "spawn_templates.h"
#include "spawner.h"
#include "streams.h"
#include "source_state.h"
#include "source_leases.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <future>
#include <map>
#include <mutex>
#include <set>

#include <nlohmann/json.hpp>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/runtime.h>

REX_EXTERN(__imp__sub_826A1818);
REX_EXTERN(__imp__sub_826C76B8);
REX_EXTERN(__imp__sub_826C8358);
REX_EXTERN(__imp__sub_826C97C0);
REX_EXTERN(sub_826A1818);
REX_EXTERN(sub_826C76B8);
REX_EXTERN(sub_826C8358);
REX_EXTERN(sub_826C97C0);

namespace spawn_sources {
namespace {
using simpsons::LoadGuestU32;
using Clock = std::chrono::steady_clock;
namespace fs = std::filesystem;
namespace saved = simpsons::modkit::source_state;

constexpr uint32_t kWorld = 0x82D08BEC, kManager = 0x82D096A4;
std::recursive_mutex g_mutex;
rex::Runtime* g_runtime = nullptr;
uint32_t g_string = 0, g_world = 0, g_manager = 0;
uint8_t* g_base = nullptr;
uint64_t g_epoch = 0;
Status g_status;
struct Prepared { std::string path, bootstrap, error; };
struct Addition {
  std::string path, failure;
  std::future<Prepared> prepare;
  Clock::time_point started;
  bool target_requested = false, rollback = false, canceled = false;
};
std::optional<Addition> g_addition;
struct OwnedFile { uint32_t address; std::string name, source; };
std::map<uint32_t, OwnedFile> g_owned;
std::set<uint32_t> g_before, g_committed, g_retiring_keys;
std::map<std::string, std::string> g_roots;  // normalized -> original public path
std::string g_primary, g_release_failure;
std::map<uint32_t, uint32_t> g_bodies;  // body -> original stream load key
std::atomic<bool> g_active{false};
size_t g_definitions = 0;
uint32_t g_ready_checks = 0;
bool g_full_release = false;
const simpsons::modkit::HostOwner kDebugOwner{0, "debug-menu"};
simpsons::modkit::SourceLeases g_leases;
thread_local bool g_requesting = false, g_retiring = false;

spawn_templates::Memory Memory(const uint8_t* base) {
  return {spawner::Readable, [base](uint32_t at) { return LoadGuestU32(base, at); },
          [base](uint32_t at) { return simpsons::GuestToHost(base, at)[0]; }};
}
uint32_t Global(const uint8_t* base, uint32_t at) {
  return spawner::Readable(at, 4) ? LoadGuestU32(base, at) : 0;
}
std::string FileName(const uint8_t* base, uint32_t node) {
  if (!spawner::Readable(node, 320))
    return {};
  const auto text = LoadGuestU32(base, node + 8);
  std::string result;
  for (uint32_t i = 0; i < 256 && uint64_t(text) + i < UINT32_MAX; ++i) {
    if (!spawner::Readable(text + i, 1))
      break;
    const auto c = simpsons::GuestToHost(base, text + i)[0];
    if (!c)
      return result;
    if (c < 32 || c >= 127)
      break;
    result += char(c);
  }
  return {};
}
bool SameFile(const uint8_t* base, const spawn_templates::File& file) {
  const auto owned = g_owned.find(file.key);
  return owned != g_owned.end() && owned->second.address == file.address &&
         FileName(base, file.address) == owned->second.name;
}
Prepared Prepare(std::vector<fs::path> roots, std::string relative) {
  Prepared result{std::move(relative)};
  try {
    const fs::path path(result.path);
    if (path.empty() || path.is_absolute() || path.has_root_path() || result.path.size() > 200 ||
        result.path.find(':') != std::string::npos || result.path.find('\\') != std::string::npos ||
        path.extension() != ".str")
      throw std::runtime_error("Choose a relative game stream path");
    for (const auto& part : path)
      if (part == ".." || part == ".")
        throw std::runtime_error("Choose a stream inside the game library");
    fs::path source;
    for (const auto& root : roots) {
      if (!fs::is_regular_file(root / path))
        continue;
      const auto canonical = fs::canonical(root / path).lexically_relative(fs::canonical(root));
      if (canonical.empty() || *canonical.begin() == "..")
        throw std::runtime_error("The stream points outside the game library");
      source = root / path;
      break;
    }
    if (source.empty())
      throw std::runtime_error("The source stream is no longer installed");
    // Validate the same metadata the original loader will relocate. In
    // particular, its fixed 32-slot folder stack must never overflow.
    simpsons::modkit::StreamFile stream(source);
    const auto folder = stream.Folder();
    if (folder.name.size() + 4 > 200)
      throw std::runtime_error("The native stream name is too long");
    if (folder.name.empty()) {
      // Most zone/mission archives have no folder metadata of their own.
      // Their episode root supplies the TOC which names their dependencies.
      const auto episode = *path.begin();
      const auto catalog = episode / episode;
      auto catalog_file = catalog;
      catalog_file += ".str";
      bool found = false;
      for (const auto& root : roots) {
        if (!fs::is_regular_file(root / catalog_file))
          continue;
        simpsons::modkit::StreamFile parent(root / catalog_file);
        if (parent.Folder().name.empty())
          throw std::runtime_error("The episode stream has no native folder metadata");
        found = true;
        break;
      }
      if (!found)
        throw std::runtime_error("The source needs its installed episode stream catalog");
      result.bootstrap = simpsons::modkit::Utf8(catalog_file);
    } else {
      result.bootstrap = result.path;
    }
    if (result.bootstrap.size() >= 128)
      throw std::runtime_error("The native folder request path exceeds its 128-byte buffer");
  } catch (const std::exception& error) {
    result.error = error.what();
  }
  return result;
}
struct FlagScope {
  bool& flag;
  bool previous;
  explicit FlagScope(bool& value, bool enabled = true) : flag(value), previous(value) {
    flag = enabled;
  }
  ~FlagScope() { flag = previous; }
};
uint32_t NativeRequest(PPCContext& ctx, uint8_t* base, std::string path, bool header) {
  std::replace(path.begin(), path.end(), '/', '\\');
  std::memcpy(simpsons::GuestToHost(base, g_string), path.c_str(), path.size() + 1);
  rex::CallFrame scratch(ctx);
  scratch.ctx.f1.f64 = 0.0;
  uint32_t key = 0;
  FlagScope scope(g_requesting);
  CallGuest(scratch.ctx, base, 0x826CB888, {g_manager, g_string, 0, 0, header ? 1u : 0u, 0, 0},
            &key);
  REXLOG_INFO("[spawn-sources] requested {}: key {:08X}, {} retained streams", path, key,
              g_owned.size());
  return key;
}

std::vector<std::string> Streams() {
  std::vector<std::string> result;
  for (const auto& [key, path] : g_roots) result.push_back(path);
  return result;
}
bool CurrentWorld(const uint8_t* base) {
  return g_epoch == spawner::WorldEpoch() && Global(base, kWorld) == g_world &&
         Global(base, kManager) == g_manager;
}
Status StatusLocked() {
  auto result = g_status;
  result.files = g_owned.size();
  result.owners = g_leases.Size();
  result.debug_claimed = !g_leases.Claim(kDebugOwner).empty();
  result.streams = Streams();
  result.pending_stream = g_addition ? g_addition->path : std::string();
  return result;
}
std::set<uint32_t> NewKeys() {
  std::set<uint32_t> result;
  for (const auto& [key, file] : g_owned)
    if (!g_committed.contains(key)) result.insert(key);
  return result;
}
void Retire(PPCContext& ctx, uint8_t* base) {
  if (!CurrentWorld(base)) return;
  const auto files = spawn_templates::Files(Memory(base), g_manager);
  std::vector<uint32_t> keys;
  for (const auto& file : files)
    if (g_retiring_keys.contains(file.key) && SameFile(base, file) &&
        file.state != 3 && file.state != 5)
      keys.push_back(file.key);
  // Recursive retirement may visit dependencies. The persistent mask also
  // protects committed files when native IO finishes outside this call stack.
  FlagScope scope(g_retiring);
  for (auto it = keys.rbegin(); it != keys.rend(); ++it)
    CallGuest(ctx, base, 0x823B1A18, {g_manager, *it});
}
void FinishRollback() {
  const auto path = g_addition->path, failure = g_addition->failure;
  const bool canceled = g_addition->canceled;
  for (auto it = g_bodies.begin(); it != g_bodies.end();)
    if (!g_committed.contains(it->second)) it = g_bodies.erase(it);
    else ++it;
  for (auto it = g_owned.begin(); it != g_owned.end();)
    if (!g_committed.contains(it->first)) it = g_owned.erase(it);
    else ++it;
  g_retiring_keys.clear();
  g_leases.Complete(canceled ? std::string() : failure);
  g_addition.reset();
  g_status = {};
  g_status.phase = canceled ? (g_roots.empty() ? Phase::idle : Phase::ready) : Phase::failed;
  g_status.path = canceled ? g_primary : path;
  g_status.message = canceled ? "Source addition canceled; retained objects are unchanged" : failure;
  g_status.loaded = g_owned.size();
  g_status.definitions = g_definitions;
  g_active.store(!g_owned.empty(), std::memory_order_release);
  if (g_owned.empty()) {
    g_world = g_manager = 0;
    g_epoch = 0;
    g_before.clear();
  }
  if (!canceled) REXLOG_WARN("[spawn-sources] {}: {}; {} committed sources retained",
                            path, failure, g_roots.size());
}
void Rollback(PPCContext& ctx, uint8_t* base, std::string failure, bool canceled = false) {
  std::unique_lock lock(g_mutex);
  if (!g_addition) return;
  g_addition->rollback = true;
  g_addition->canceled = canceled;
  g_addition->failure = std::move(failure);
  g_retiring_keys = NewKeys();
  g_status.phase = Phase::releasing;
  g_status.message = canceled ? "Canceling the source addition" : "Removing only the failed source addition";
  if (g_retiring_keys.empty()) {
    FinishRollback();
    return;
  }
  lock.unlock();
  Retire(ctx, base);
}
void BeginRelease(PPCContext& ctx, uint8_t* base, std::string failure = {}) {
  std::unique_lock lock(g_mutex);
  if (g_full_release) return;
  const auto path = g_status.path;
  g_leases.Clear();
  g_addition.reset();
  g_release_failure = std::move(failure);
  if (g_owned.empty()) {
    const auto error = g_release_failure;
    Forget();
    if (!error.empty()) {
      g_status.phase = Phase::failed;
      g_status.path = path;
      g_status.message = error;
    }
    return;
  }
  g_full_release = true;
  g_status.phase = Phase::releasing;
  g_status.message = "Removing session objects and releasing their streams";
  g_retiring_keys.clear();
  for (const auto& [key, file] : g_owned) g_retiring_keys.insert(key);
  lock.unlock();
  Retire(ctx, base);
}
void Register(PPCContext& ctx, uint8_t* base) {
  const auto memory = Memory(base);
  std::map<uint32_t, uint32_t> bodies;
  {
    std::lock_guard lock(g_mutex);
    bodies = g_bodies;
  }
  std::vector<std::pair<spawn_templates::Definition, uint32_t>> definitions;
  std::map<spawn_templates::Guid, uint32_t> unique;
  for (const auto& [body, key] : bodies) {
    if (!memory.Has(body, 48) || memory.word(body + 8) != 0x53696D47 ||
        memory.word(body + 12) != 1 || memory.word(body + 44) != key)
      throw std::runtime_error("A source definition lost its native stream owner");
    const auto table = memory.word(body + 40), count = memory.word(body + 24);
    if (count > 16384 || (count && !memory.Has(table, count * 4)))
      throw std::runtime_error("The source definition table is invalid");
    for (uint32_t i = 0; i < count; ++i) {
      auto definition = spawn_templates::Read(memory, memory.word(table + i * 4));
      if (!definition) throw std::runtime_error("The source contains an unsupported entity record");
      const auto existing = spawn_templates::Find(memory, g_world, definition->guid);
      if (existing) {
        if (existing->data != definition->data)
          throw std::runtime_error("An object GUID conflicts with a retained source or the current level");
        continue;
      }
      const auto [entry, inserted] = unique.emplace(definition->guid, definition->data);
      if (!inserted) {
        if (entry->second != definition->data)
          throw std::runtime_error("The source contains conflicting object GUIDs");
        continue;
      }
      definitions.emplace_back(*definition, key);
    }
  }
  if (definitions.empty()) throw std::runtime_error("This stream has no additional object definitions");
  if (g_definitions + definitions.size() > 16384)
    throw std::runtime_error("The retained source session reached its object definition limit");
  for (const auto& [definition, key] : definitions) {
    if (!CallGuest(ctx, base, 0x826A3350, {g_world, definition.data, key}))
      throw std::runtime_error("The native definition registrar is unavailable");
    const auto retained = spawn_templates::Find(memory, g_world, definition.guid);
    if (!retained || retained->data != definition.data)
      throw std::runtime_error("The native world did not retain an object definition");
  }
  spawner::InvalidateCatalog();
  spawner::Catalog(base);
  std::lock_guard lock(g_mutex);
  const auto path = g_addition->path;
  g_roots.emplace(*saved::Path(path), path);
  if (g_primary.empty()) g_primary = path;
  g_definitions += definitions.size();
  for (const auto& [key, file] : g_owned) g_committed.insert(key);
  g_leases.Complete();
  g_addition.reset();
  g_status.phase = Phase::ready;
  g_status.path = path;
  g_status.message = "Object sources ready in the spawner";
  g_status.loaded = g_owned.size();
  g_status.definitions = g_definitions;
  REXLOG_INFO("[spawn-sources] ready {}: {} sources, {} files, {} definitions; source layers held inactive",
              path, g_roots.size(), g_owned.size(), g_definitions);
}
}  // namespace

void Init(rex::Runtime* runtime) {
  g_runtime = runtime;
  g_string = host_memory::Alloc(256);
}
void Shutdown() {
  Forget();
  g_runtime = nullptr;
}
Status Get() {
  std::lock_guard lock(g_mutex);
  return StatusLocked();
}
Status Get(const simpsons::modkit::HostOwner& owner) {
  std::lock_guard lock(g_mutex);
  auto result = StatusLocked();
  if (const auto failure = g_leases.FailureFor(owner)) {
    result.phase = Phase::failed;
    result.path = failure->path;
    result.message = failure->message;
  } else if (const auto path = saved::Path(g_leases.Claim(owner));
             !g_full_release && path && g_roots.contains(*path)) {
    result.phase = Phase::ready;
    result.path = g_leases.Claim(owner);
    result.message = "Retained object source is ready";
    result.files = result.loaded = g_committed.size();
    result.definitions = g_definitions;
  }
  return result;
}
Status DebugStatus() { return Get(kDebugOwner); }
std::map<uint32_t, std::string> LoadedStreams(const uint8_t* base) {
  std::lock_guard lock(g_mutex);
  std::map<uint32_t, std::string> result;
  for (const auto& file : spawn_templates::Files(Memory(base), Global(base, kManager)))
    if (file.state == 4)
      if (const auto path = saved::Path(FileName(base, file.address), true))
        result.emplace(file.key, *path);
  return result;
}
std::string LevelRoot(const uint8_t* base) {
  std::lock_guard lock(g_mutex);
  if (g_epoch && !CurrentWorld(base)) return {};
  std::string result;
  for (const auto& file : spawn_templates::Files(Memory(base), Global(base, kManager))) {
    if (file.state != 4 || SameFile(base, file)) continue;
    const auto path = saved::Path(FileName(base, file.address), true);
    if (!path) continue;
    const auto slash = path->find('/');
    if (slash == std::string::npos || path->find('/', slash + 1) != std::string::npos ||
        path->substr(0, slash) != path->substr(slash + 1, path->size() - slash - 5) ||
        path->starts_with("frontend/") || path->starts_with("simpsons_chars/")) continue;
    if (!result.empty() && result != *path) return {};
    result = *path;
  }
  return result;
}
bool Streaming(const uint8_t* base) {
  const auto memory = Memory(base);
  const auto manager = Global(base, kManager);
  if (!memory.Has(manager, 8768) || memory.word(manager + 8736)) return true;
  const auto files = spawn_templates::Files(memory, manager);
  return files.empty() || std::any_of(files.begin(), files.end(), [](const auto& file) {
    return file.state != 4;
  });
}
bool IsReady(std::string_view relative) {
  std::lock_guard lock(g_mutex);
  const auto path = saved::Path(relative);
  if (!path || g_full_release) return false;
  return std::any_of(g_owned.begin(), g_owned.end(), [&](const auto& entry) {
    return g_committed.contains(entry.first) && saved::Path(entry.second.name, true) == path;
  });
}
void RetainDebug(uint32_t source_layer) { RetainOwner(source_layer, kDebugOwner); }
void RetainOwner(uint32_t source_layer, const simpsons::modkit::HostOwner& owner) {
  std::lock_guard lock(g_mutex);
  if (!g_full_release && g_committed.contains(source_layer))
    g_leases.Retain(owner, g_owned.at(source_layer).source);
}
bool CanSpawn(uint32_t source_layer) {
  std::lock_guard lock(g_mutex);
  return !g_owned.contains(source_layer) || (!g_full_release && g_committed.contains(source_layer));
}
bool Request(std::string_view relative) { return Request(relative, kDebugOwner); }
bool Request(std::string_view relative, const simpsons::modkit::HostOwner& owner) {
  std::lock_guard lock(g_mutex);
  const auto path = saved::Path(relative);
  if (!path || relative.size() > 200 || !g_runtime || !g_string || g_full_release) return false;
  for (const auto& [key, file] : g_owned) {
    if (g_committed.contains(key) && saved::Path(file.name, true) == path) {
      g_leases.Share(owner, file.source);
      return true;
    }
  }
  if (g_addition) {
    if (saved::Path(g_addition->path) != path || g_addition->rollback) return false;
    g_leases.Join(owner);
    return true;
  }
  if (g_roots.size() >= saved::kMaxSources) return false;
  auto roots = modkit_game::AssetRoots();
  roots.push_back(g_runtime->game_data_root());
  g_addition.emplace();
  g_addition->path = std::string(relative);
  g_addition->started = Clock::now();
  g_leases.Begin(g_addition->path);
  g_leases.Join(owner);
  g_status.phase = Phase::preparing;
  g_status.path = g_addition->path;
  g_status.message = "Checking an additional object source";
  g_addition->prepare = std::async(std::launch::async, Prepare, std::move(roots), g_addition->path);
  return true;
}
std::string Claimed(const simpsons::modkit::HostOwner& owner) {
  std::lock_guard lock(g_mutex);
  return g_leases.Claim(owner);
}
bool ReleaseOwner(const simpsons::modkit::HostOwner& owner) {
  std::lock_guard lock(g_mutex);
  return g_leases.Release(owner);
}
bool CancelOwner(const simpsons::modkit::HostOwner& owner) {
  std::lock_guard lock(g_mutex);
  return g_leases.Withdraw(owner);
}
bool Cancel() { return CancelOwner(kDebugOwner); }
void Release(PPCContext& ctx, uint8_t* base, bool force) {
  std::unique_lock lock(g_mutex);
  if (!force) {
    g_leases.Release(kDebugOwner);
    if (!g_leases.Empty()) {
      std::vector<uint32_t> layers;
      for (const auto& [key, file] : g_owned) layers.push_back(key);
      lock.unlock();
      spawner::DestroyDebugCopies(ctx, base, layers);
      return;
    }
  }
  lock.unlock();
  BeginRelease(ctx, base);
}
void BeforeWorldReset(PPCContext& ctx, uint8_t* base) { Release(ctx, base, true); }
void Forget() {
  std::lock_guard lock(g_mutex);
  g_addition.reset();
  g_leases.Clear();
  g_owned.clear();
  g_before.clear();
  g_committed.clear();
  g_retiring_keys.clear();
  g_roots.clear();
  g_primary.clear();
  g_bodies.clear();
  g_active.store(false, std::memory_order_release);
  g_world = g_manager = 0;
  g_base = nullptr;
  g_epoch = 0;
  g_definitions = 0;
  g_status = {};
  g_full_release = false;
  g_release_failure.clear();
  g_ready_checks = 0;
}
nlohmann::json SaveState(bool exact) {
  std::lock_guard lock(g_mutex);
  if (!exact) return nullptr;
  if (g_addition || g_full_release || (!g_owned.empty() && g_leases.Empty()))
    throw simpsons::modkit::Failure("busy", "Wait for object sources to finish loading or releasing");
  if (g_owned.empty()) return nullptr;
  if (!g_base || !CurrentWorld(g_base))
    throw simpsons::modkit::Failure("busy", "The source session changed while preparing the save");
  const auto files = spawn_templates::Files(Memory(g_base), g_manager);
  const auto loaded = std::count_if(files.begin(), files.end(), [&](const auto& file) {
    return file.state == 4 && SameFile(g_base, file);
  });
  if (size_t(loaded) != g_owned.size())
    throw simpsons::modkit::Failure("busy", "Wait for retained source files to finish streaming");
  nlohmann::json state = {{"api", 2}, {"path", g_primary}, {"streams", Streams()},
      {"world", g_world}, {"manager", g_manager}, {"definitions", uint32_t(g_definitions)},
      {"debug_claimed", !g_leases.Claim(kDebugOwner).empty()}, {"before", g_before},
      {"files", nlohmann::json::array()}, {"bodies", nlohmann::json::array()}};
  if (state.at("debug_claimed").get<bool>()) state["debug_stream"] = g_leases.Claim(kDebugOwner);
  for (const auto& [key, file] : g_owned)
    state["files"].push_back({{"key", key}, {"address", file.address}, {"name", file.name}, {"source", file.source}});
  for (const auto& [body, key] : g_bodies)
    state["bodies"].push_back({{"address", body}, {"key", key}});
  saved::Validate(state);
  return state;
}
void ValidateState(const nlohmann::json& state) { saved::Validate(state); }
void RestoreState(PPCContext& ctx, uint8_t* base, const nlohmann::json& state) {
  std::unique_lock lock(g_mutex);
  Forget();
  if (state.is_null()) return;
  ValidateState(state);
  g_world = state.at("world");
  g_manager = state.at("manager");
  g_base = base;
  g_epoch = spawner::WorldEpoch();
  g_primary = state.at("path");
  g_roots = saved::Paths(state);
  g_before = state.at("before").get<std::set<uint32_t>>();
  const bool session = state.at("api") == 2;
  for (const auto& file : state.at("files")) {
    const uint32_t key = file.at("key");
    const std::string source = session ? file.at("source").get<std::string>() : g_primary;
    g_owned.emplace(key, OwnedFile{file.at("address"), file.at("name"), source});
    g_committed.insert(key);
  }
  for (const auto& body : state.at("bodies"))
    g_bodies.emplace(body.at("address").get<uint32_t>(), body.at("key").get<uint32_t>());
  g_definitions = state.at("definitions");
  g_active.store(!g_owned.empty(), std::memory_order_release);
  if (state.value("debug_claimed", true))
    g_leases.Share(kDebugOwner, session ? state.at("debug_stream").get<std::string>() : g_primary);
  g_status.phase = Phase::ready;
  g_status.path = g_primary;
  g_status.message = "Object source session restored";
  g_status.loaded = g_owned.size();
  g_status.definitions = g_definitions;
  const auto files = spawn_templates::Files(Memory(base), Global(base, kManager));
  const auto loaded = std::count_if(files.begin(), files.end(), [&](const auto& file) {
    return file.state == 4 && SameFile(base, file);
  });
  if (!CurrentWorld(base) || size_t(loaded) != g_owned.size()) {
    lock.unlock();
    BeginRelease(ctx, base, "The restored source session does not match its native stream loads");
  }
}
void Tick(PPCContext& ctx, uint8_t* base) {
  std::unique_lock lock(g_mutex);
  if (g_epoch && !CurrentWorld(base)) {
    Forget();
    return;
  }
  if (!g_full_release && g_leases.Empty() && (g_addition || !g_owned.empty())) {
    lock.unlock();
    BeginRelease(ctx, base);
    return;
  }
  if (g_addition && !g_addition->rollback && !g_leases.Wanted()) {
    lock.unlock();
    Rollback(ctx, base, {}, true);
    return;
  }
  if (g_addition && !g_addition->rollback && g_status.phase == Phase::preparing) {
    if (!g_addition->prepare.valid() ||
        g_addition->prepare.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
    const auto prepared = g_addition->prepare.get();
    if (!prepared.error.empty()) {
      lock.unlock();
      Rollback(ctx, base, prepared.error);
      return;
    }
    const auto world = Global(base, kWorld), manager = Global(base, kManager);
    const auto memory = Memory(base);
    if (!modkit_game::SourceWorldReady() || !memory.Has(world, 432) || !memory.Has(manager, 8768)) {
      lock.unlock();
      Rollback(ctx, base, "Enter a level before loading object sources");
      return;
    }
    const auto files = spawn_templates::Files(memory, manager);
    if (files.empty() || memory.word(manager + 8736) ||
        std::any_of(files.begin(), files.end(), [](const auto& file) { return file.state != 4; })) {
      lock.unlock();
      Rollback(ctx, base, "Wait for the current level to finish streaming");
      return;
    }
    if (!g_epoch) {
      g_world = world;
      g_manager = manager;
      g_base = base;
      g_epoch = spawner::WorldEpoch();
      for (const auto& file : files) g_before.insert(file.key);
    }
    g_status.phase = Phase::loading;
    g_status.message = "Loading an additional source and its dependencies";
    g_addition->started = Clock::now();
    g_addition->target_requested = prepared.bootstrap == prepared.path;
    lock.unlock();
    const auto key = NativeRequest(ctx, base, prepared.bootstrap, true);
    if (!key) Rollback(ctx, base, "The native stream request failed");
    return;
  }
  if (g_owned.empty() && !g_addition) return;
  if (!g_addition && !g_full_release && ++g_ready_checks % 30 != 0) return;
  const auto files = spawn_templates::Files(Memory(base), g_manager);
  if (files.empty()) return;  // A transitional list is not proof of retirement.
  size_t present = 0, loaded = 0, retiring = 0, committed_loaded = 0;
  bool bad = false;
  for (const auto& file : files) {
    if (!SameFile(base, file)) continue;
    ++present;
    loaded += file.state == 4;
    committed_loaded += g_committed.contains(file.key) && file.state == 4;
    retiring += g_retiring_keys.contains(file.key);
    bad |= !g_committed.contains(file.key) && (file.state == 3 || file.state == 5 || file.state == 6);
  }
  g_status.loaded = loaded;
  if (g_full_release) {
    if (!present) {
      const auto path = g_status.path, failure = g_release_failure;
      REXLOG_INFO("[spawn-sources] released session: {} sources", g_roots.size());
      Forget();
      lock.unlock();
      spawner::InvalidateCatalog();
      if (!failure.empty()) {
        lock.lock();
        g_status.phase = Phase::failed;
        g_status.path = path;
        g_status.message = failure;
      }
    }
    return;
  }
  if (committed_loaded != g_committed.size()) {
    lock.unlock();
    BeginRelease(ctx, base, "A retained source changed or started unloading; releasing the session");
    return;
  }
  if (g_addition && g_addition->rollback) {
    if (!retiring) {
      FinishRollback();
      lock.unlock();
      spawner::InvalidateCatalog();
    }
    return;
  }
  if (!g_addition) return;
  if (present != g_owned.size() || bad || g_owned.size() > 1024 ||
      Clock::now() - g_addition->started > std::chrono::seconds(60)) {
    lock.unlock();
    Rollback(ctx, base, "The additional source did not finish loading");
    return;
  }
  if (loaded != g_owned.size()) return;
  if (!g_addition->target_requested) {
    const auto path = g_addition->path;
    std::string name = path.substr(0, path.size() - 4);
    std::replace(name.begin(), name.end(), '/', '\\');
    std::memcpy(simpsons::GuestToHost(base, g_string), name.c_str(), name.size() + 1);
    uint32_t metadata = 0;
    lock.unlock();
    const bool found = CallGuest(ctx, base, 0x826C5E90, {g_manager, g_string}, &metadata);
    if (!found || !metadata) {
      Rollback(ctx, base, "The episode catalog does not describe this object source");
      return;
    }
    lock.lock();
    g_addition->target_requested = true;
    g_status.message = "Loading additional object resources";
    lock.unlock();
    const auto key = NativeRequest(ctx, base, path, false);
    if (!key) Rollback(ctx, base, "The native object stream request failed");
    return;
  }
  const auto path = saved::Path(g_addition->path);
  const bool target_owned = std::any_of(g_owned.begin(), g_owned.end(), [&](const auto& entry) {
    return !g_committed.contains(entry.first) && saved::Path(entry.second.name, true) == path;
  });
  if (!target_owned) {
    lock.unlock();
    Rollback(ctx, base, "This object source is already loaded outside the retained session");
    return;
  }
  lock.unlock();
  try {
    Register(ctx, base);
  } catch (const std::exception& error) {
    Rollback(ctx, base, error.what());
  }
}

void Queued(PPCContext& ctx, uint8_t* base) {
  if (!g_requesting) return;
  std::lock_guard lock(g_mutex);
  if (!g_addition || !g_requesting || !CurrentWorld(base) || ctx.r3.u32 != g_manager ||
      !spawner::Readable(ctx.r4.u32, 320)) return;
  const auto node = ctx.r4.u32, key = LoadGuestU32(base, node + 96);
  if (key && !g_before.contains(key)) {
    g_owned.try_emplace(key, OwnedFile{node, FileName(base, node), g_addition->path});
    g_active.store(true, std::memory_order_release);
  }
}
bool Hold(PPCContext& ctx, uint8_t* base) {
  if (!g_active.load(std::memory_order_acquire)) return false;
  std::lock_guard lock(g_mutex);
  if (g_epoch != spawner::WorldEpoch()) return false;
  const auto body = ctx.r4.u32;
  if (!spawner::Readable(body, 48)) return false;
  const auto key = LoadGuestU32(base, body + 44);
  if (!g_owned.contains(key)) return false;
  if (ctx.r3.u32 == g_world) g_bodies.emplace(body, key);
  return true;
}
bool OwnedNode(const uint8_t* base, uint32_t node) {
  if (!spawner::Readable(node, 320)) return false;
  const auto key = LoadGuestU32(base, node + 96);
  return saved::OwnsFile(g_owned, key, node, FileName(base, node));
}
bool Protect(PPCContext& ctx, uint8_t* base) {
  if (!g_retiring || ctx.r3.u32 != g_manager || !spawner::Readable(ctx.r4.u32, 320)) return false;
  const auto key = LoadGuestU32(base, ctx.r4.u32 + 96);
  return !OwnedNode(base, ctx.r4.u32) || (!g_retiring_keys.empty() && !g_retiring_keys.contains(key));
}
template <typename Function>
void Retiring(PPCContext& ctx, uint8_t* base, Function original) {
  if (!g_active.load(std::memory_order_acquire)) { original(ctx, base); return; }
  std::unique_lock lock(g_mutex);
  const bool same_manager = g_epoch == spawner::WorldEpoch() && ctx.r3.u32 == g_manager;
  const bool owned = same_manager && OwnedNode(base, ctx.r4.u32);
  FlagScope scope(g_retiring, same_manager && (g_retiring || owned));
  const bool protect = Protect(ctx, base);
  lock.unlock();
  if (!protect) original(ctx, base);
}
}  // namespace spawn_sources

REX_FUNC(sub_826C76B8) {
  spawn_sources::Queued(ctx, base);
  __imp__sub_826C76B8(ctx, base);
}
REX_FUNC(sub_826A1818) {
  if (spawn_sources::Hold(ctx, base)) {
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_826A1818(ctx, base);
}
REX_FUNC(sub_826C8358) {
  spawn_sources::Retiring(ctx, base, __imp__sub_826C8358);
}
REX_FUNC(sub_826C97C0) {
  spawn_sources::Retiring(ctx, base, __imp__sub_826C97C0);
}
