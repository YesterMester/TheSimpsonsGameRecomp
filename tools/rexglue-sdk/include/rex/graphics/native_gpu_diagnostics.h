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
    kCount,
  };
  enum class Allocation {
    kEdram,
    kMemoryMirror,
    kScaledMirror,
    kCount,
  };

  void Initialize(std::string_view backend);
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

}  // namespace rex::graphics
