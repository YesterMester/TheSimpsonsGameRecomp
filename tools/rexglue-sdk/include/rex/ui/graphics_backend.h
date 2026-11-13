#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace rex::ui {

enum class GraphicsBackend { kVulkan, kD3D12, kD3D11 };

constexpr std::string_view GraphicsBackendName(GraphicsBackend backend) {
  switch (backend) {
    case GraphicsBackend::kVulkan:
      return "Vulkan";
    case GraphicsBackend::kD3D12:
      return "Direct3D 12";
    case GraphicsBackend::kD3D11:
      return "Direct3D 11";
  }
  return "Unknown";
}

struct GraphicsBackendOrder {
  std::array<GraphicsBackend, 3> backends{};
  size_t count = 0;
};

struct GraphicsDevicePreference {
  bool software;
  uint32_t feature_score;
  bool discrete;
};

constexpr bool PreferGraphicsDevice(const GraphicsDevicePreference& a,
                                    const GraphicsDevicePreference& b) {
  if (a.software != b.software)
    return !a.software;
  if (a.feature_score != b.feature_score)
    return a.feature_score > b.feature_score;
  return a.discrete > b.discrete;
}

// An explicit choice stays explicit. Automatic tries each compiled backend,
// releasing a failed provider before trying the next one.
constexpr GraphicsBackendOrder GetGraphicsBackendOrder(std::string_view requested, bool vulkan,
                                                       bool d3d12, bool d3d11) {
  GraphicsBackendOrder order;
  const bool automatic = requested.empty() || requested == "any";
  if (vulkan && (automatic || requested == "vulkan"))
    order.backends[order.count++] = GraphicsBackend::kVulkan;
  if (d3d12 && (automatic || requested == "d3d12"))
    order.backends[order.count++] = GraphicsBackend::kD3D12;
  if (d3d11 && (automatic || requested == "d3d11"))
    order.backends[order.count++] = GraphicsBackend::kD3D11;
  return order;
}

// Driver support and process privileges may both limit queue priority. A
// failed normal-priority attempt still fails initialization.
template <typename Attempt>
std::optional<int> TryGraphicsQueuePriority(int requested, int realtime, int high, int normal,
                                            Attempt attempt) {
  const std::array priorities = {realtime, high, normal};
  bool selected = false;
  for (int priority : priorities) {
    selected |= priority == requested;
    if (selected && attempt(priority))
      return priority;
  }
  return std::nullopt;
}

}  // namespace rex::ui
