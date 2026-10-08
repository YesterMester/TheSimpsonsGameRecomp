#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

#include <rex/ui/d3d11/d3d11_api.h>

namespace rex::ui::d3d11 {

// Serializes the immediate context shared by the game and UI threads. The
// epoch advances whenever a different thread takes the lock, so state cached
// from the context can tell when another thread may have changed it. Code on
// the owning thread that changes cached state invalidates it explicitly.
class ContextMutex {
 public:
  void lock() {
    mutex_.lock();
    Acquired();
  }
  bool try_lock() {
    if (!mutex_.try_lock())
      return false;
    Acquired();
    return true;
  }
  void unlock() {
    --depth_;
    mutex_.unlock();
  }
  // Only meaningful while the calling thread holds the lock.
  uint64_t epoch() const { return epoch_; }

 private:
  void Acquired() {
    if (depth_++)
      return;
    auto thread = std::this_thread::get_id();
    if (thread != owner_) {
      owner_ = thread;
      ++epoch_;
    }
  }
  std::recursive_mutex mutex_;
  uint32_t depth_ = 0;
  std::thread::id owner_;
  uint64_t epoch_ = 0;
};

// Native device shared by the game command processor and window presenter.
class D3D11Device {
 public:
  struct Options {
    // -1 chooses a supported hardware adapter, preferring high performance.
    int adapter_index = -1;
    bool debug = false;
    // Explicit software-device selection for Windows qualification only.
    bool warp = false;
    // Qualification can cap a newer GPU at the older hardware baseline.
    D3D_FEATURE_LEVEL maximum_feature_level = D3D_FEATURE_LEVEL_11_1;
  };

  enum class ShaderStage { kVertex, kPixel, kCompute };

  struct Features {
    D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    uint32_t vendor_id = 0;
    uint32_t device_id = 0;
    uint64_t dedicated_video_memory = 0;
    std::string adapter_name;
    bool software = false;
    bool debug_layer = false;
    bool constant_buffer_ranges = false;
    bool rasterizer_ordered_views = false;
    bool pixel_shader_stencil_reference = false;
    bool typed_uav_load_additional_formats = false;
  };

  static std::unique_ptr<D3D11Device> Create(const Options& options, std::string& error);
  ~D3D11Device();
  D3D11Device(const D3D11Device&) = delete;
  D3D11Device& operator=(const D3D11Device&) = delete;

  ID3D11Device* device() const { return device_.Get(); }
  ID3D11DeviceContext* context() const { return context_.Get(); }
  IDXGIFactory1* factory() const { return factory_.Get(); }
  const Features& features() const { return features_; }
  // The game and UI share one immediate context. Mailbox consumer/producer
  // locks must be acquired before this lock to avoid inverting their order.
  ContextMutex& context_mutex() { return context_mutex_; }

  // The owner serializes immediate-context commands, including these waits.
  // No queue or resource may outlive the owning device.
  HRESULT WaitForCompletion(std::chrono::milliseconds timeout = std::chrono::seconds(5));

  // Native HLSL helpers use fixed Shader Model 5.0 bindings for FL 11_0 GPUs.
  static HRESULT CompileShader(std::string_view source, const char* entry, ShaderStage stage,
                               ID3DBlob** bytecode, std::string& error);

 private:
  D3D11Device() = default;
  bool Initialize(const Options& options, std::string& error);
  HRESULT TryCreate(IDXGIAdapter1* adapter, bool warp, bool debug,
                    D3D_FEATURE_LEVEL maximum_feature_level);
  void ReadFeatures(bool software);

  Microsoft::WRL::ComPtr<IDXGIFactory1> factory_;
  Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter_;
  Microsoft::WRL::ComPtr<ID3D11Device> device_;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
  Features features_;
  ContextMutex context_mutex_;
};

}  // namespace rex::ui::d3d11
