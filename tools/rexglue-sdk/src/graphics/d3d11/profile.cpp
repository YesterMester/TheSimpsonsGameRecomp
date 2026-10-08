#include <rex/graphics/d3d11/gpu_profiler.h>

#include <chrono>

namespace rex::graphics::d3d11 {

ProfileCounters g_profile;
namespace {
std::atomic<bool> profile_enabled{false};
uint64_t Nanoseconds() {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}
}  // namespace
bool ProfileEnabled() {
  return profile_enabled.load(std::memory_order_relaxed);
}
void SetProfileEnabled(bool enabled) {
  profile_enabled.store(enabled, std::memory_order_relaxed);
}
ProfileClock::ProfileClock() : enabled_(ProfileEnabled()) {
  if (enabled_)
    last_ = Nanoseconds();
}
void ProfileClock::Mark(ProfilePhase phase) {
  if (!enabled_)
    return;
  uint64_t now = Nanoseconds();
  g_profile.ns[size_t(phase)].fetch_add(now - last_, std::memory_order_relaxed);
  last_ = now;
}

GpuProfiler* g_gpu_profiler = nullptr;
void GpuSwitch(GpuCategory category) {
  if (g_gpu_profiler)
    g_gpu_profiler->Switch(category);
}
Microsoft::WRL::ComPtr<ID3D11Query> GpuProfiler::Timestamp() {
  Microsoft::WRL::ComPtr<ID3D11Query> query;
  if (!free_timestamps_.empty()) {
    query = std::move(free_timestamps_.back());
    free_timestamps_.pop_back();
    return query;
  }
  D3D11_QUERY_DESC desc = {D3D11_QUERY_TIMESTAMP, 0};
  device_.device()->CreateQuery(&desc, query.GetAddressOf());
  return query;
}
void GpuProfiler::Switch(GpuCategory category) {
  if (category == category_ || !ProfileEnabled())
    return;
  auto* context = device_.context();
  if (!frame_active_) {
    D3D11_QUERY_DESC desc = {D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
    if (FAILED(device_.device()->CreateQuery(&desc, frame_.disjoint.GetAddressOf())))
      return;
    context->Begin(frame_.disjoint.Get());
    frame_active_ = true;
  }
  auto query = Timestamp();
  if (!query)
    return;
  context->End(query.Get());
  frame_.marks.push_back({category, std::move(query)});
  category_ = category;
}
void GpuProfiler::EndFrame() {
  if (frame_active_) {
    Switch(GpuCategory::kCount);
    device_.context()->End(frame_.disjoint.Get());
    pending_.push_back(std::move(frame_));
    frame_ = {};
    frame_active_ = false;
  }
  category_ = GpuCategory::kCount;
  Collect();
}
void GpuProfiler::Collect() {
  auto* context = device_.context();
  size_t done = 0;
  for (; done < pending_.size(); ++done) {
    auto& frame = pending_[done];
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint;
    if (context->GetData(frame.disjoint.Get(), &disjoint, sizeof(disjoint),
                         D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
      break;
    std::vector<uint64_t> times(frame.marks.size());
    bool complete = true;
    for (size_t i = 0; i < frame.marks.size() && complete; ++i)
      complete = context->GetData(frame.marks[i].query.Get(), &times[i], sizeof(uint64_t),
                                  D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK;
    if (!complete)
      break;
    if (disjoint.Disjoint || !disjoint.Frequency) {
      g_profile.gpu_disjoint_frames.fetch_add(1, std::memory_order_relaxed);
    } else {
      for (size_t i = 0; i + 1 < frame.marks.size(); ++i)
        if (times[i + 1] > times[i])
          g_profile.gpu_ns[size_t(frame.marks[i].category)].fetch_add(
              (times[i + 1] - times[i]) * 1000000000ull / disjoint.Frequency,
              std::memory_order_relaxed);
      g_profile.gpu_frames.fetch_add(1, std::memory_order_relaxed);
    }
    for (auto& mark : frame.marks)
      free_timestamps_.push_back(std::move(mark.query));
  }
  pending_.erase(pending_.begin(), pending_.begin() + done);
  // Results are read without flushing; drop frames that never complete.
  if (pending_.size() > 8)
    pending_.erase(pending_.begin(), pending_.end() - 8);
}

}  // namespace rex::graphics::d3d11
