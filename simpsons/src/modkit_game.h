#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "runtime.h"

struct PPCContext;
namespace rex { class Runtime; }

// Game adapters own all reverse-engineered details. Public mods use the SDK.
namespace modkit_game {
// Internal frame scope; public callbacks still pass through Runtime's thread
// guard. Nested native UI events restore the caller's context on leaving.
class Context {
 public:
  Context(PPCContext& ctx, uint8_t* base);
  ~Context();
  Context(const Context&) = delete;
  Context& operator=(const Context&) = delete;
 private:
  PPCContext* previous_;
  uint8_t* previous_base_;
};
void Prepare(rex::Runtime* runtime);
void InitText(rex::Runtime* runtime);
void Tick(PPCContext& ctx, uint8_t* base);
void InvalidateWorld();
void PhysicsStepped();
// Game-thread source loading uses the current world lifetime, including while
// an original pause menu is open. Does not acquire the modkit/source mutexes.
bool SourceWorldReady();
void Shutdown();
bool Active();
std::vector<std::filesystem::path> AssetRoots();
std::vector<simpsons::modkit::MenuPage> Pages();
void Activate(std::string_view page, std::string_view row,
              const simpsons::modkit::Json& value, uint32_t player);
bool IsGameThread();
bool SupportsExact();
simpsons::modkit::Json SaveState(bool exact = true);
// Checkpoint files and code are verified by a worker before the save UI
// changes the working slot. Preparation/commit callbacks remain on the game thread.
bool BeginCheckpoint(const std::filesystem::path& folder);
bool CheckpointReady();
std::string CheckpointError();
bool PrepareCheckpoint(const std::filesystem::path& folder);
bool Compatible(const std::filesystem::path& folder, bool vanilla_checkpoint = false);
std::shared_ptr<const simpsons::modkit::PreparedSources> VerifyRestoreSources();
bool PrepareRestore(const std::filesystem::path& folder, PPCContext& ctx, uint8_t* base,
                    const simpsons::modkit::PreparedSources* sources = nullptr);
void CommitRestore(PPCContext& ctx, uint8_t* base);
void CancelRestore();
}  // namespace modkit_game
