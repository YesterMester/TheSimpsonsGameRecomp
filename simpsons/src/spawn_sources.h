#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

struct PPCContext;
namespace rex {
class Runtime;
}
namespace simpsons::modkit {
struct HostOwner;
}

// The debug spawner borrows whole native stream loads, never loose records.
// Calls that change residency run only on the game's main thread.
namespace spawn_sources {
enum class Phase { idle, preparing, loading, ready, releasing, failed };
struct Status {
  Phase phase = Phase::idle;
  std::string path, message;
  size_t files = 0, loaded = 0, definitions = 0;
  size_t owners = 0;
  bool debug_claimed = false;
  std::vector<std::string> streams;
  std::string pending_stream;
};
void Init(rex::Runtime* runtime);
void Shutdown();
Status Get();
Status Get(const simpsons::modkit::HostOwner& owner);
Status DebugStatus();
bool IsReady(std::string_view relative);
std::map<uint32_t, std::string> LoadedStreams(const uint8_t* base);
std::string LevelRoot(const uint8_t* base);
bool Streaming(const uint8_t* base);
bool Request(std::string_view relative);
bool Request(std::string_view relative, const simpsons::modkit::HostOwner& owner);
std::string Claimed(const simpsons::modkit::HostOwner& owner);
// Host-only retirement; actual native unload waits for the game's thread and
// for every other owner of the same source to release its claim.
bool ReleaseOwner(const simpsons::modkit::HostOwner& owner);
bool CancelOwner(const simpsons::modkit::HostOwner& owner);
bool Cancel();
void RetainDebug(uint32_t source_layer);
void RetainOwner(uint32_t source_layer, const simpsons::modkit::HostOwner& owner);
bool CanSpawn(uint32_t source_layer);
void Tick(PPCContext& ctx, uint8_t* base);
void Release(PPCContext& ctx, uint8_t* base, bool force = false);
// Called before ordinary teardown, while native stream ownership still exists.
void BeforeWorldReset(PPCContext& ctx, uint8_t* base);
// Exact restores have already replaced memory: forget host identities only.
void Forget();
// Exact saves retain the load identities along with guest memory. Ordinary
// checkpoints load their own level and do not retain a debug source.
nlohmann::json SaveState(bool exact);
void ValidateState(const nlohmann::json& state);
void RestoreState(PPCContext& ctx, uint8_t* base, const nlohmann::json& state);
}  // namespace spawn_sources
