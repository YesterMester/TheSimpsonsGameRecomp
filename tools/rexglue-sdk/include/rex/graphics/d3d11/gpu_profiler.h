#pragma once

#include <vector>

#include <rex/graphics/d3d11/profile.h>
#include <rex/ui/d3d11/d3d11_device.h>

namespace rex::graphics::d3d11 {

// Writes GPU timestamps on the command processor's immediate context whenever
// the category of submitted work changes, and accumulates completed frames a
// few frames later. Callers hold the context lock.
class GpuProfiler {
 public:
  explicit GpuProfiler(ui::d3d11::D3D11Device& device) : device_(device) {}
  void Switch(GpuCategory category);
  void EndFrame();

 private:
  struct Mark {
    GpuCategory category;
    Microsoft::WRL::ComPtr<ID3D11Query> query;
  };
  struct Frame {
    Microsoft::WRL::ComPtr<ID3D11Query> disjoint;
    std::vector<Mark> marks;
  };
  Microsoft::WRL::ComPtr<ID3D11Query> Timestamp();
  void Collect();

  ui::d3d11::D3D11Device& device_;
  bool frame_active_ = false;
  Frame frame_;
  GpuCategory category_ = GpuCategory::kCount;
  std::vector<Frame> pending_;
  std::vector<Microsoft::WRL::ComPtr<ID3D11Query>> free_timestamps_;
};
// The command processor's profiler, or null. Command processor thread only.
extern GpuProfiler* g_gpu_profiler;

}  // namespace rex::graphics::d3d11
