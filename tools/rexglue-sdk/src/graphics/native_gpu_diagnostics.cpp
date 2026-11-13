#include <rex/graphics/native_gpu_diagnostics.h>

#include <filesystem>
#include <fstream>
#include <unordered_map>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(native_gpu_diagnostics, false, "GPU",
                    "Record cumulative native GPU coverage and legacy dependencies");
REXCVAR_DEFINE_BOOL(native_gpu_strict, false, "GPU",
                    "Testing: stop at the first legacy GPU rendering dependency")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(native_gpu_report_path, "", "GPU",
                      "Write cumulative native GPU coverage as JSON (empty disables the file)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace rex::graphics {
namespace {
constexpr std::array kDependencyNames = {"edram",
                                         "memory_mirror",
                                         "scaled_mirror_read",
                                         "scaled_mirror_write",
                                         "resolve_fallback",
                                         "vertex_fallback",
                                         "texture_fallback"};
constexpr std::array kOperationNames = {
    "vertex_streams", "cpu_texture_upload", "gpu_texture_upload", "resolve",
    "alias_transfer", "native_draw_call",   "legacy_draw_packet"};
constexpr std::array kAllocationNames = {"edram", "memory_mirror", "scaled_mirror"};
static_assert(kOperationNames.size() == size_t(NativeGpuDiagnostics::Operation::kCount));

constexpr std::array kRouteNames = {"draw_dispatch", "geometry",        "textures",
                                    "resolves",      "shader_commands", "synchronization",
                                    "submission",    "presentation",    "state_commands",
                                    "queries",       "other_commands",  "host_presentation"};
constexpr std::array kOutcomeNames = {"native", "retained", "mixed",
                                      "failed", "skipped",  "unclassified"};
static_assert(kRouteNames.size() == size_t(NativeGpuDiagnostics::RouteSystem::kCount));
static_assert(kOutcomeNames.size() == size_t(NativeGpuDiagnostics::RouteOutcome::kCount));

struct RouteCounter {
  uint64_t attempts = 0;
  uint64_t in_flight = 0;
  std::array<uint64_t, kOutcomeNames.size()> outcomes{};
};
struct RouteOwner {
  const void* register_file = nullptr;
  std::array<RouteCounter, kRouteNames.size()> systems{};
};
struct RouteRegistry {
  std::mutex mutex;
  std::unordered_map<const NativeGpuDiagnostics*, RouteOwner> owners;
};

RouteRegistry& Routes() {
  // Keep the registry alive during process-wide runtime destruction. Individual
  // owners are erased by their diagnostics destructor.
  static auto* registry = new RouteRegistry;
  return *registry;
}
std::atomic<bool> route_owners_present{false};

void BeginRoute(const NativeGpuDiagnostics* diagnostics, NativeGpuDiagnostics::RouteSystem system) {
  auto& registry = Routes();
  std::lock_guard lock(registry.mutex);
  auto& counter = registry.owners.at(diagnostics).systems[size_t(system)];
  ++counter.attempts;
  ++counter.in_flight;
}

void EndRoute(const NativeGpuDiagnostics* diagnostics, NativeGpuDiagnostics::RouteSystem system,
              NativeGpuDiagnostics::RouteOutcome outcome) {
  auto& registry = Routes();
  std::lock_guard lock(registry.mutex);
  auto& counter = registry.owners.at(diagnostics).systems[size_t(system)];
  --counter.in_flight;
  ++counter.outcomes[size_t(outcome)];
}

std::string RouteReport(const NativeGpuDiagnostics* diagnostics) {
  auto& registry = Routes();
  std::lock_guard lock(registry.mutex);
  const auto& owner = registry.owners.at(diagnostics);
  std::string result = "{\"version\": 1, \"systems\": {";
  for (size_t i = 0; i < kRouteNames.size(); ++i) {
    const auto& counter = owner.systems[i];
    result += fmt::format("{}\n    \"{}\": {{\"attempts\": {}, \"in_flight\": {}", i ? "," : "",
                          kRouteNames[i], counter.attempts, counter.in_flight);
    for (size_t j = 0; j < kOutcomeNames.size(); ++j)
      result += fmt::format(", \"{}\": {}", kOutcomeNames[j], counter.outcomes[j]);
    result += "}";
  }
  return result + "\n  }}";
}

std::string JsonString(std::string_view text) {
  std::string out = "\"";
  for (unsigned char c : text) {
    if (c == '"' || c == '\\') {
      out += '\\';
      out += char(c);
    } else if (c < 32) {
      out += fmt::format("\\u{:04x}", c);
    } else {
      out += char(c);
    }
  }
  out += '"';
  return out;
}
}  // namespace

thread_local NativeGpuDiagnostics* NativeGpuDiagnostics::readback_owner_ = nullptr;
thread_local NativeGpuDiagnostics::RouteScope* NativeGpuDiagnostics::RouteScope::active_ = nullptr;

NativeGpuDiagnostics::~NativeGpuDiagnostics() {
  if (enabled_) {
    auto& registry = Routes();
    std::lock_guard lock(registry.mutex);
    registry.owners.erase(this);
    route_owners_present.store(!registry.owners.empty(), std::memory_order_release);
  }
}

void NativeGpuDiagnostics::Initialize(std::string_view backend) {
  backend_ = backend;
  strict_ = REXCVAR_GET(native_gpu_strict);
  report_path_ = REXCVAR_GET(native_gpu_report_path);
  enabled_ = strict_ || !report_path_.empty() || REXCVAR_GET(native_gpu_diagnostics);
  auto& registry = Routes();
  std::lock_guard lock(registry.mutex);
  registry.owners.erase(this);
  if (enabled_)
    registry.owners.emplace(this, RouteOwner{});
  route_owners_present.store(!registry.owners.empty(), std::memory_order_release);
}

void NativeGpuDiagnostics::BindRegisterFile(const void* register_file) {
  if (enabled_) {
    auto& registry = Routes();
    std::lock_guard lock(registry.mutex);
    registry.owners.at(this).register_file = register_file;
  }
}

NativeGpuDiagnostics* NativeGpuDiagnostics::ForRegisterFile(const void* register_file) {
  if (!register_file || !route_owners_present.load(std::memory_order_acquire))
    return nullptr;
  auto& registry = Routes();
  std::lock_guard lock(registry.mutex);
  for (const auto& [diagnostics, owner] : registry.owners)
    if (owner.register_file == register_file)
      return const_cast<NativeGpuDiagnostics*>(diagnostics);
  return nullptr;
}

void NativeGpuDiagnostics::RouteScope::Start() {
  // Keep TLS access in the runtime module rather than in a Windows DLL client.
  if (readback_owner_ == diagnostics_) {
    diagnostics_ = nullptr;
    return;
  }
  BeginRoute(diagnostics_, system_);
  previous_ = active_;
  active_ = this;
}

void NativeGpuDiagnostics::RouteScope::Stop() { active_ = previous_; }

void NativeGpuDiagnostics::RouteScope::ObserveCompatibility(NativeGpuDiagnostics* diagnostics) {
  for (auto* scope = active_; scope; scope = scope->previous_)
    if (scope->diagnostics_ == diagnostics && scope->observe_dependencies_ && !scope->finished_)
      scope->retained_ = true;
}

void NativeGpuDiagnostics::RouteScope::FinishSlow(bool success) {
  if (!diagnostics_ || finished_)
    return;
  RouteOutcome outcome = RouteOutcome::kFailed;
  if (success) {
    outcome = native_ ? (retained_ ? RouteOutcome::kMixed : RouteOutcome::kNative)
                      : (retained_ ? RouteOutcome::kRetained : RouteOutcome::kUnclassified);
  }
  EndRoute(diagnostics_, system_, outcome);
  finished_ = true;
}

void NativeGpuDiagnostics::RouteScope::SkipSlow() {
  if (diagnostics_ && !finished_) {
    EndRoute(diagnostics_, system_, RouteOutcome::kSkipped);
    finished_ = true;
  }
}

void NativeGpuDiagnostics::RecordDependency(Dependency kind, uint64_t bytes,
                                           std::string_view detail) {
  if (!enabled_) {
    return;
  }
  if (readback_owner_ == this) {
    readback_calls_.fetch_add(1, std::memory_order_relaxed);
    readback_bytes_.fetch_add(bytes, std::memory_order_relaxed);
    return;
  }
  RouteScope::ObserveCompatibility(this);
  Counter& counter = dependencies_[size_t(kind)];
  uint64_t previous = counter.calls.fetch_add(1, std::memory_order_relaxed);
  counter.bytes.fetch_add(bytes, std::memory_order_relaxed);
  if (!previous) {
    std::lock_guard lock(report_mutex_);
    counter.first_detail = detail;
    REXGPU_WARN("[native-gpu-dependency] {}: {}", kDependencyNames[size_t(kind)], detail);
  }
  if (strict_) {
    failed_.store(true, std::memory_order_relaxed);
    WriteReport();
    REX_FATAL("Native GPU validation failed: {}: {}", kDependencyNames[size_t(kind)], detail);
  }
}

void NativeGpuDiagnostics::RecordOperation(Operation kind) {
  if (enabled_ && readback_owner_ != this) {
    operations_[size_t(kind)].calls.fetch_add(1, std::memory_order_relaxed);
  }
}

void NativeGpuDiagnostics::RecordAllocation(Allocation kind, uint64_t virtual_bytes,
                                           uint64_t resident_bytes) {
  if (!enabled_) {
    return;
  }
  if (readback_owner_ == this) {
    readback_allocations_.fetch_add(1, std::memory_order_relaxed);
    readback_virtual_bytes_.fetch_add(virtual_bytes, std::memory_order_relaxed);
    readback_resident_bytes_.fetch_add(resident_bytes, std::memory_order_relaxed);
    return;
  }
  RouteScope::ObserveCompatibility(this);
  Counter& counter = allocations_[size_t(kind)];
  counter.calls.fetch_add(1, std::memory_order_relaxed);
  counter.bytes.fetch_add(virtual_bytes, std::memory_order_relaxed);
  counter.resident_bytes.fetch_add(resident_bytes, std::memory_order_relaxed);
}

void NativeGpuDiagnostics::Frame() {
  if (enabled_ && frames_.fetch_add(1, std::memory_order_relaxed) % 120 == 119) {
    WriteReport();
  }
}

void NativeGpuDiagnostics::WriteReport(bool complete) {
  if (!enabled_) {
    return;
  }
  std::lock_guard lock(report_mutex_);
  uint64_t dependencies = 0;
  std::string entries;
  for (size_t i = 0; i < dependencies_.size(); ++i) {
    const auto& counter = dependencies_[i];
    uint64_t calls = counter.calls.load(std::memory_order_relaxed);
    dependencies += calls;
    entries += fmt::format("{}    {}: {{\"calls\": {}, \"bytes\": {}, \"first_detail\": {}}}",
                           i ? ",\n" : "", JsonString(kDependencyNames[i]), calls,
                           counter.bytes.load(std::memory_order_relaxed),
                           JsonString(counter.first_detail));
  }
  REXGPU_INFO("[native-gpu] {} frames, {} legacy dependencies{}",
              frames_.load(std::memory_order_relaxed), dependencies,
              complete ? " (final)" : "");
  if (report_path_.empty()) {
    return;
  }
  std::string text = fmt::format(
      "{{\n  \"version\": 1,\n  \"backend\": {},\n  \"strict\": {},\n  \"complete\": {},\n"
      "  \"failed\": {},\n  \"frames\": {},\n  \"dependencies\": {{\n{}\n  }},\n"
      "  \"operations\": {{\n",
      JsonString(backend_), strict_, complete, failed_.load(std::memory_order_relaxed),
      frames_.load(std::memory_order_relaxed), entries);
  for (size_t i = 0; i < operations_.size(); ++i) {
    text += fmt::format("{}    {}: {}", i ? ",\n" : "", JsonString(kOperationNames[i]),
                        operations_[i].calls.load(std::memory_order_relaxed));
  }
  text += "\n  },\n  \"allocations\": {\n";
  for (size_t i = 0; i < allocations_.size(); ++i) {
    const auto& counter = allocations_[i];
    text += fmt::format("{}    {}: {{\"calls\": {}, \"virtual_bytes\": {}, \"resident_bytes\": {}}}",
                        i ? ",\n" : "", JsonString(kAllocationNames[i]),
                        counter.calls.load(std::memory_order_relaxed),
                        counter.bytes.load(std::memory_order_relaxed),
                        counter.resident_bytes.load(std::memory_order_relaxed));
  }
  text += fmt::format(
      "\n  }},\n  \"diagnostic_readback\": {{\"calls\": {}, \"bytes\": {}, "
      "\"allocations\": {}, \"virtual_bytes\": {}, \"resident_bytes\": {}}},\n"
      "  \"route_coverage\": {}\n}}\n",
      readback_calls_.load(std::memory_order_relaxed),
      readback_bytes_.load(std::memory_order_relaxed),
      readback_allocations_.load(std::memory_order_relaxed),
      readback_virtual_bytes_.load(std::memory_order_relaxed),
      readback_resident_bytes_.load(std::memory_order_relaxed), RouteReport(this));
  std::filesystem::path path(report_path_), temporary(report_path_ + ".tmp");
  std::error_code error;
  if (path.has_parent_path()) {
    std::filesystem::create_directories(path.parent_path(), error);
  }
  {
    std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
    stream << text;
    stream.close();
    if (!stream) {
      REXGPU_ERROR("Unable to write native GPU report {}", report_path_);
      return;
    }
  }
  std::filesystem::rename(temporary, path, error);
  if (error) {
    // Windows doesn't replace an existing file with filesystem::rename.
    std::filesystem::remove(path, error);
    error.clear();
    std::filesystem::rename(temporary, path, error);
  }
  if (error) {
    REXGPU_ERROR("Unable to replace native GPU report {}: {}", report_path_, error.message());
  }
}

NativeGpuDiagnostics::ReadbackScope::ReadbackScope(NativeGpuDiagnostics& diagnostics)
    : previous_(readback_owner_) {
  readback_owner_ = &diagnostics;
}

NativeGpuDiagnostics::ReadbackScope::~ReadbackScope() {
  readback_owner_ = previous_;
}

}  // namespace rex::graphics
