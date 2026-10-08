#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace rex::graphics::d3d11 {

// Command processor thread time spent on native DX11 work, for the
// gpu_wait_stats log. Accumulated only while the command processor enables it
// for that log.
enum class ProfilePhase : uint32_t {
  // Locking and resetting the shared immediate context for a guest draw.
  kDrawContext,
  // Shader analysis and draw classification.
  kDrawSetup,
  // Primitive normalization, including index processing.
  kDrawPrimitives,
  // Render target ownership and transfers.
  kDrawRenderTargets,
  kDrawShaders,
  kDrawTextures,
  // Vertex stream and memexport destination memory requests.
  kDrawStreams,
  // Constant, texture and sampler bindings.
  kDrawBindings,
  // Final shader variants and fixed-function state.
  kDrawPipelines,
  // Immediate-context binding and the draw call itself.
  kDrawSubmit,
  kDrawReadback,
  kResolve,
  kSwap,
  kCount,
};

// GPU work categories, timed from timestamps written at category changes.
enum class GpuCategory : uint32_t {
  kDraws,
  // Render target ownership transfers, clears and snapshots.
  kRenderTargets,
  // Texture decoding and uploads.
  kTextures,
  // Resolves: encoding render target samples, copying them to the
  // destination, the original-resolution copy, and clears.
  kResolves,
  kResolveCopies,
  kResolveMirrors,
  kResolveClears,
  // Guest output, including FXAA and presentation on this thread.
  kSwap,
  kCount,
};

struct ProfileCounters {
  std::atomic<uint64_t> ns[size_t(ProfilePhase::kCount)] = {};
  std::atomic<uint64_t> draws{0};
  std::atomic<uint64_t> resolves{0};
  std::atomic<uint64_t> memory_uploads{0};
  std::atomic<uint64_t> memory_upload_bytes{0};
  std::atomic<uint64_t> context_invalidations{0};
  std::atomic<uint64_t> buffer_hits{0};
  std::atomic<uint64_t> buffer_creations{0};
  std::atomic<uint64_t> buffer_creation_bytes{0};
  std::atomic<uint64_t> output_changes{0};
  std::atomic<uint64_t> transfers{0};
  std::atomic<uint64_t> transfer_draws{0};
  std::atomic<uint64_t> transfer_pixels{0};
  std::atomic<uint64_t> transfer_same_format{0};
  std::atomic<uint64_t> snapshots{0};
  std::atomic<uint64_t> snapshot_bytes{0};
  std::atomic<uint64_t> gpu_ns[size_t(GpuCategory::kCount)] = {};
  std::atomic<uint64_t> gpu_frames{0};
  std::atomic<uint64_t> gpu_disjoint_frames{0};
};
extern ProfileCounters g_profile;

bool ProfileEnabled();
void SetProfileEnabled(bool enabled);

// Records a change of the submitted GPU work's category while the gpu_wait_stats
// log is enabled. Command processor thread only.
void GpuSwitch(GpuCategory category);

// Accumulates the time since the previous mark into each named phase.
class ProfileClock {
 public:
  ProfileClock();
  void Mark(ProfilePhase phase);

 private:
  bool enabled_;
  uint64_t last_ = 0;
};

}  // namespace rex::graphics::d3d11
