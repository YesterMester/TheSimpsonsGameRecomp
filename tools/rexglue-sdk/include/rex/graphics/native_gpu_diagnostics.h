#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>

namespace rex::graphics {

// Cumulative, per-command-processor evidence. Diagnostic readbacks are kept
// separate from rendering dependencies and never count as native work.
class NativeGpuDiagnostics {
 public:
  ~NativeGpuDiagnostics();

  // Units are defined by the instrumented boundary, not by dependency events.
  // Command routes and resource-local operations are separate measurements.
  enum class RouteSystem {
    kDrawDispatch,
    kGeometry,
    kTextures,
    kResolves,
    kShaderCommands,
    kSynchronization,
    kSubmission,
    kPresentation,
    kStateCommands,
    kQueries,
    kOtherCommands,
    kHostPresentation,
    kCount,
  };
  enum class RouteOutcome { kNative, kRetained, kMixed, kFailed, kSkipped, kUnclassified, kCount };
  class RouteScope;

  enum class Dependency {
    kEdram,
    kMemoryMirror,
    kScaledMirrorRead,
    kScaledMirrorWrite,
    kResolveFallback,
    kVertexFallback,
    kTextureFallback,
    kCount,
  };
  enum class Operation {
    kVertexStreams,
    kCpuTextureUpload,
    kGpuTextureUpload,
    kResolve,
    kAliasTransfer,
    kNativeDrawCall,
    kLegacyDrawPacket,
    kCount,
  };
  enum class Allocation {
    kEdram,
    kMemoryMirror,
    kScaledMirror,
    kCount,
  };

  void Initialize(std::string_view backend);
  void BindRegisterFile(const void* register_file);
  static NativeGpuDiagnostics* ForRegisterFile(const void* register_file);
  void RecordDependency(Dependency kind, uint64_t bytes, std::string_view detail);
  void RecordOperation(Operation kind);
  void RecordAllocation(Allocation kind, uint64_t virtual_bytes, uint64_t resident_bytes);
  void Frame();
  void WriteReport(bool complete = false);
  bool enabled() const { return enabled_; }
  bool strict() const { return strict_; }

  // Tracing may deliberately serialize legacy layouts for the reference
  // replayer. Attribute that work explicitly, including its allocations.
  class ReadbackScope {
   public:
    explicit ReadbackScope(NativeGpuDiagnostics& diagnostics);
    ~ReadbackScope();
    ReadbackScope(const ReadbackScope&) = delete;
    ReadbackScope& operator=(const ReadbackScope&) = delete;
   private:
    NativeGpuDiagnostics* previous_;
  };

 private:
  struct Counter {
    std::atomic<uint64_t> calls{0};
    std::atomic<uint64_t> bytes{0};
    std::atomic<uint64_t> resident_bytes{0};
    std::string first_detail;
  };
  bool enabled_ = false;
  bool strict_ = false;
  std::string backend_;
  std::string report_path_;
  std::atomic<uint64_t> frames_{0};
  std::atomic<uint64_t> readback_calls_{0};
  std::atomic<uint64_t> readback_bytes_{0};
  std::atomic<uint64_t> readback_allocations_{0};
  std::atomic<uint64_t> readback_virtual_bytes_{0};
  std::atomic<uint64_t> readback_resident_bytes_{0};
  std::atomic<bool> failed_{false};
  std::array<Counter, size_t(Dependency::kCount)> dependencies_;
  std::array<Counter, size_t(Operation::kCount)> operations_;
  std::array<Counter, size_t(Allocation::kCount)> allocations_;
  std::mutex report_mutex_;
  static thread_local NativeGpuDiagnostics* readback_owner_;
};

// One scope is one attempted operation. Early errors record failure, predicated
// commands record skipped, and compatibility dependencies make a native resource
// operation mixed. Diagnostic readback is excluded. Storage lives outside the
// processor so these diagnostics do not change the renderer object layouts.
class NativeGpuDiagnostics::RouteScope {
 public:
  RouteScope(NativeGpuDiagnostics* diagnostics, RouteSystem system,
             bool observe_dependencies = true)
      : diagnostics_(diagnostics), system_(system), observe_dependencies_(observe_dependencies) {
    if (!diagnostics_ || !diagnostics_->enabled_ || system_ == RouteSystem::kCount) {
      diagnostics_ = nullptr;
      return;
    }
    Start();
  }
  ~RouteScope() {
    if (diagnostics_) {
      Finish(false);
      Stop();
    }
  }
  RouteScope(const RouteScope&) = delete;
  RouteScope& operator=(const RouteScope&) = delete;
  void Native() { native_ = true; }
  void Retained() { retained_ = true; }
  void Finish(bool success) {
    if (diagnostics_ && !finished_)
      FinishSlow(success);
  }
  void Skip() {
    if (diagnostics_ && !finished_)
      SkipSlow();
  }

 private:
  friend class NativeGpuDiagnostics;
  void Start();
  void Stop();
  void FinishSlow(bool success);
  void SkipSlow();
  static void ObserveCompatibility(NativeGpuDiagnostics* diagnostics);
  static thread_local RouteScope* active_;
  NativeGpuDiagnostics* diagnostics_;
  RouteSystem system_;
  RouteScope* previous_ = nullptr;
  bool observe_dependencies_;
  bool native_ = false;
  bool retained_ = false;
  bool finished_ = false;
};

}  // namespace rex::graphics
