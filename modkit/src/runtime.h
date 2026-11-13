#pragma once

#include <simpsons/modkit.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <compare>
#include <atomic>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace simpsons::modkit {

using Json = nlohmann::json;
namespace fs = std::filesystem;

struct Failure : std::runtime_error {
  std::string code;
  Failure(std::string code, std::string text)
      : std::runtime_error(std::move(text)), code(std::move(code)) {}
};

struct NativeImage {
  fs::path folder, entry;
  NativeImage() = default;
  NativeImage(const NativeImage&) = delete;
  NativeImage& operator=(const NativeImage&) = delete;
  ~NativeImage();
};
struct Manifest {
  std::string id, name, version, author, description, kind, entry, save_mode;
  fs::path folder;
  std::map<std::string, std::string> dependencies;
  std::vector<std::string> conflicts;
  std::map<std::string, std::string> binaries;
  std::string fingerprint;
  Json source;
  // Captured with the package fingerprint. The Lua loader verifies and keeps
  // these bytes before entering the game thread, including require modules.
  std::map<std::string, std::string> script_hashes;
  std::shared_ptr<const std::map<std::string, std::string>> scripts;
  std::shared_ptr<const std::map<std::string, std::string>> files;
  std::shared_ptr<const NativeImage> native_image;
  std::shared_ptr<const Json> model_edits;
  std::shared_ptr<const Json> mesh_edits;
  std::shared_ptr<const Json> texture_edits;
  static Manifest Read(const fs::path& folder, bool contents = true, std::stop_token stop = {});
};
void PrepareCode(Manifest& manifest, const fs::path& cache, std::stop_token stop = {});
struct PreparedSources;

// Canonical containment checks are shared by manifest entrypoints, imports,
// mod assets and storage. Symlinks cannot escape the supplied root.
fs::path Inside(const fs::path& root, std::string_view relative);
fs::path RelativePath(std::string_view relative);
fs::path Utf8Path(std::string_view text);
std::string Utf8(const fs::path& path);
std::string ReadText(const fs::path& path, size_t maximum = 4 * 1024 * 1024);
void WriteText(const fs::path& path, std::string_view text);
std::string Fingerprint(const fs::path& folder, Json* inventory = nullptr, std::stop_token stop = {});
std::string Platform();
bool VersionMatches(std::string_view version, std::string_view requirement);
std::vector<Manifest> Resolve(std::vector<Manifest> manifests,
                              const std::vector<std::string>& enabled);

// Shared by C++ and Lua. Game-specific memory, threading and resource access
// stay behind this boundary; unit tests use a small deterministic host.
using GameCall = std::function<Json(std::string_view, const Json&)>;
using Log = std::function<void(std::string_view)>;

// Internal host adapter contract, independent of the public plugin ABI.
// A replacement runtime gets a distinct instance, so discarding preflight
// state cannot retire the resources of the runtime still in play.
struct HostOwner {
  uint64_t instance = 0;
  std::string mod;
  auto operator<=>(const HostOwner&) const = default;
};
struct HostServices {
  std::function<Json(const HostOwner&, std::string_view, const Json&)> call;
  std::function<Json(const HostOwner&)> save;
  std::function<void(const std::vector<Json>&)> validate;
  std::function<void(const HostOwner&, const Json&, bool)> restore;
  // Release may run during shutdown on another thread. It must not call
  // guest code or depend on a running simulation.
  std::function<void(const HostOwner&)> release;
};

class Runtime;
class PersistentStorage;
class Localizations;
class ModelVariants;
class MeshVariants;
class TextureVariants;
struct GameTextLine;
using GameTextTables = std::map<std::string, std::map<uint32_t, GameTextLine>>;
class Mod {
 public:
  Mod(Runtime& runtime, Manifest manifest);
  virtual ~Mod() = default;
  virtual void Start() = 0;
  virtual void Event(std::string_view event, const Json& data) = 0;
  virtual Json Save() = 0;
  virtual void Load(const Json& data) = 0;
  virtual void Stop() {}
  Json Call(std::string_view method, const Json& args);
  void Message(std::string_view message);
  Manifest manifest;
  bool failed = false;
  bool retired = false;
  std::optional<Json> retired_state;
  std::optional<Json> retired_missions;
  uint64_t calls = 0, errors = 0;
  double cpu_ms = 0, max_ms = 0;

 protected:
  Runtime& runtime_;
};
std::unique_ptr<Mod> MakeLuaMod(Runtime& runtime, Manifest manifest);
std::unique_ptr<Mod> MakeNativeMod(Runtime& runtime, Manifest manifest);

struct MenuRow {
  std::string id, label, type;
  Json value;
  double minimum = 0, maximum = 1, step = 1;
  std::vector<Json> choices;
};
struct MenuPage {
  std::string owner, id, title;
  std::vector<MenuRow> rows;
};

class Runtime {
 public:
  Runtime(GameCall game, Log log, std::string locale = "en", HostServices host = {});
  ~Runtime();
  // Load before the game starts. Code entrypoints initialize on the first
  // game-thread Tick, so loading a DLL cannot call into an unstarted game.
  void Load(const fs::path& root, const fs::path& storage,
            const std::vector<std::string>& enabled);
  void Tick(bool level_ready, double simulation_seconds);
  void InvalidateWorld();
  void Stop();
  bool active() const { return !mods_.empty(); }
  bool IsGameThread() const { return started_ && !stopping_ && thread_ == std::this_thread::get_id(); }
  bool SupportsExact() const;
  Json Call(Mod& mod, std::string_view method, const Json& args);
  Json Status() const;
  Json Save(bool exact = true);
  void ValidateSave(const Json& state) const;
  // Construct and validate replacement host state before changing guest memory.
  // The caller swaps this instance in only after its snapshot restore succeeds.
  std::unique_ptr<Runtime> PrepareRestore(const Json& state, bool checkpoint = false);
  std::unique_ptr<Runtime> PrepareRestore(const Json& state, bool checkpoint,
                                          const PreparedSources& sources);
  std::unique_ptr<Runtime> PrepareInitialState();
  std::unique_ptr<Runtime> PrepareInitialState(const PreparedSources& sources);
  void NotifyRestored(bool checkpoint = false);
  // Copy descriptors on the game thread; verification and immutable code
  // capture may then run on a worker without accessing this Runtime.
  std::vector<Manifest> Sources() const;
  std::unique_ptr<Runtime> PrepareReload(const PreparedSources& sources);
  void NotifyReloaded();
  // A host lifecycle notification belongs to one specific runtime generation.
  void EntityRemoved(const HostOwner& owner, const Json& data);
  void EntityDied(const HostOwner& owner, const Json& data);
  std::vector<fs::path> AssetRoots() const;
  std::vector<MenuPage> Pages() const;
  void Activate(std::string_view page, std::string_view row, const Json& value,
                uint32_t player);
  void Message(std::string_view owner, std::string_view message);
  static Json Catalog();
  const GameTextTables& GameTexts() const;
  std::shared_ptr<const ModelVariants> ModelEdits() const { return model_edits_; }
  std::shared_ptr<const MeshVariants> MeshEdits() const { return mesh_edits_; }
  std::shared_ptr<const TextureVariants> TextureEdits() const { return texture_edits_; }
  // Optional creator inspection. Requests are executed by the game-thread
  // adapter, never by a filesystem or network worker.
  void EnableInspection(bool enabled);
  Json Inspect(const Json& request);

 private:
  void CheckThread() const;
  void Dispatch(std::string_view event, const Json& data);
  void Invoke(Mod& mod, std::string_view event, const Json& data);
  void Retire(Mod& mod);
  HostOwner Owner(const Mod& mod) const { return {instance_, mod.manifest.id}; }
  void AdvanceTimers();
  void AdvanceMissions();
  Json Timers(std::string_view owner) const;
  void Trace(std::string_view event, const Json& data);
  Mod& Find(std::string_view id);
  Json SaveInternal(bool exact, bool recover);
  Json Serialize(Mod& mod);
  void ApplyRestoredResources(bool checkpoint);
  std::unique_ptr<Runtime> PrepareReplacement(const Json* state, bool checkpoint,
                                               const PreparedSources* sources = nullptr,
                                               bool reload = false);
  GameCall game_;
  HostServices host_;
  uint64_t instance_ = 0;
  std::unique_ptr<class Missions> missions_;
  std::map<std::string, Json> pending_mission_restore_;
  std::pair<std::string, std::string> last_mission_;
  std::map<std::string, Json> restored_resources_;
  Log log_;
  std::string locale_;
  std::shared_ptr<const Localizations> localizations_;
  std::shared_ptr<const GameTextTables> game_text_;
  std::shared_ptr<const ModelVariants> model_edits_;
  std::shared_ptr<const MeshVariants> mesh_edits_;
  fs::path storage_;
  std::shared_ptr<PersistentStorage> storage_cache_;
  std::vector<std::unique_ptr<Mod>> mods_;
  std::map<std::string, MenuPage> pages_;
  std::thread::id thread_;
  bool started_ = false, level_ready_ = false, dispatching_ = false, preparing_ = false,
       stopping_ = false, serializing_ = false;
  uint64_t epoch_ = 1, frame_ = 0;
  struct Timer {
    double deadline = 0, interval = 0;
    Json data;
    uint64_t generation = 0;
  };
  using TimerKey = std::pair<std::string, std::string>;
  std::map<TimerKey, Timer> timers_;
  TimerKey last_timer_;
  uint64_t timer_generation_ = 0;
  double simulation_time_ = 0;
  std::atomic<bool> inspection_{false};
  mutable std::mutex trace_mutex_;
  std::deque<Json> trace_;
  uint64_t trace_sequence_ = 0;
  std::shared_ptr<const TextureVariants> texture_edits_;
};

}  // namespace simpsons::modkit
