#include <rex/ui/d3d11/d3d11_device.h>

#include <cstdio>
#include <iterator>
#include <thread>

namespace rex::ui::d3d11 {

namespace {
std::string HResultError(const char* operation, HRESULT result) {
  char message[160];
  std::snprintf(message, sizeof(message), "%s failed (0x%08X)", operation, unsigned(result));
  return message;
}

std::string AdapterName(const DXGI_ADAPTER_DESC1& desc) {
  int bytes = WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, nullptr, 0, nullptr, nullptr);
  if (bytes <= 1) {
    return {};
  }
  std::string name(size_t(bytes), '\0');
  if (!WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name.data(), bytes, nullptr,
                           nullptr)) {
    return {};
  }
  name.resize(size_t(bytes - 1));
  return name;
}
}  // namespace

std::unique_ptr<D3D11Device> D3D11Device::Create(const Options& options, std::string& error) {
  error.clear();
  auto result = std::unique_ptr<D3D11Device>(new D3D11Device());
  if (!result->Initialize(options, error)) {
    return nullptr;
  }
  return result;
}

D3D11Device::~D3D11Device() {
  if (context_) {
    context_->ClearState();
    context_->Flush();
  }
}

HRESULT D3D11Device::TryCreate(IDXGIAdapter1* adapter, bool warp, bool debug,
                               D3D_FEATURE_LEVEL maximum_feature_level) {
  // FL 11_0 is the minimum. Requesting 11_1 explicitly avoids silently losing
  // its optional capabilities when the runtime's default level list is used.
  constexpr D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
  bool cap_11_0 = maximum_feature_level == D3D_FEATURE_LEVEL_11_0;
  UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | (debug ? D3D11_CREATE_DEVICE_DEBUG : 0);
  auto create = [&](UINT creation_flags) {
    device_.Reset();
    context_.Reset();
    return D3D11CreateDevice(adapter, warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_UNKNOWN,
                             nullptr, creation_flags, levels + (cap_11_0 ? 1 : 0),
                             cap_11_0 ? 1 : UINT(std::size(levels)), D3D11_SDK_VERSION,
                             device_.GetAddressOf(), &features_.level, context_.GetAddressOf());
  };
  HRESULT result = create(flags);
  if (result == DXGI_ERROR_SDK_COMPONENT_MISSING && debug) {
    flags &= ~D3D11_CREATE_DEVICE_DEBUG;
    result = create(flags);
  }
  if (SUCCEEDED(result)) {
    features_.debug_layer = (flags & D3D11_CREATE_DEVICE_DEBUG) != 0;
  }
  return result;
}

bool D3D11Device::Initialize(const Options& options, std::string& error) {
  if (options.maximum_feature_level != D3D_FEATURE_LEVEL_11_0 &&
      options.maximum_feature_level != D3D_FEATURE_LEVEL_11_1) {
    error = "Direct3D 11 requires feature level 11_0 or 11_1";
    return false;
  }
  if (options.adapter_index < -1 || (options.warp && options.adapter_index != -1)) {
    error = "Choose a hardware adapter or WARP, with an adapter index of -1 or greater";
    return false;
  }
  HRESULT result = CreateDXGIFactory1(IID_PPV_ARGS(factory_.GetAddressOf()));
  if (FAILED(result)) {
    error = HResultError("CreateDXGIFactory1", result);
    return false;
  }
  if (options.warp) {
    result = TryCreate(nullptr, true, options.debug, options.maximum_feature_level);
    if (SUCCEEDED(result)) {
      ReadFeatures(true);
      return true;
    }
    error = HResultError("D3D11CreateDevice(WARP)", result);
    return false;
  }

  Microsoft::WRL::ComPtr<IDXGIFactory6> factory6;
  if (options.adapter_index == -1) {
    factory_.As(&factory6);
  }
  HRESULT last_create = DXGI_ERROR_UNSUPPORTED;
  for (UINT index = 0;; ++index) {
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    UINT selected = options.adapter_index == -1 ? index : UINT(options.adapter_index);
    result = factory6 ? factory6->EnumAdapterByGpuPreference(selected,
                                                             DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                             IID_PPV_ARGS(adapter.GetAddressOf()))
                      : factory_->EnumAdapters1(selected, adapter.GetAddressOf());
    if (result == DXGI_ERROR_NOT_FOUND) {
      break;
    }
    if (FAILED(result)) {
      error = HResultError("DXGI adapter enumeration", result);
      return false;
    }
    DXGI_ADAPTER_DESC1 desc = {};
    result = adapter->GetDesc1(&desc);
    if (FAILED(result)) {
      error = HResultError("DXGI adapter description", result);
      return false;
    }
    if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
      last_create = TryCreate(adapter.Get(), false, options.debug, options.maximum_feature_level);
      if (SUCCEEDED(last_create)) {
        adapter_ = std::move(adapter);
        ReadFeatures(false);
        return true;
      }
    }
    if (options.adapter_index != -1) {
      break;
    }
  }
  error = HResultError("No hardware adapter supporting Direct3D feature level 11_0", last_create);
  return false;
}

void D3D11Device::ReadFeatures(bool software) {
  features_.software = software;
  if (software) {
    features_.adapter_name = "Microsoft WARP";
    features_.vendor_id = 0x1414;
  } else {
    DXGI_ADAPTER_DESC1 desc = {};
    if (SUCCEEDED(adapter_->GetDesc1(&desc))) {
      features_.adapter_name = AdapterName(desc);
      features_.vendor_id = desc.VendorId;
      features_.device_id = desc.DeviceId;
      features_.dedicated_video_memory = desc.DedicatedVideoMemory;
    }
  }
  D3D11_FEATURE_DATA_D3D11_OPTIONS options = {};
  if (SUCCEEDED(
          device_->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &options, sizeof(options)))) {
    features_.constant_buffer_ranges = options.ConstantBufferOffsetting != FALSE;
  }
  D3D11_FEATURE_DATA_D3D11_OPTIONS2 options2 = {};
  if (SUCCEEDED(device_->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS2, &options2,
                                             sizeof(options2)))) {
    features_.rasterizer_ordered_views = options2.ROVsSupported != FALSE;
    features_.pixel_shader_stencil_reference = options2.PSSpecifiedStencilRefSupported != FALSE;
    features_.typed_uav_load_additional_formats = options2.TypedUAVLoadAdditionalFormats != FALSE;
  }
}

HRESULT D3D11Device::WaitForCompletion(std::chrono::milliseconds timeout) {
  Microsoft::WRL::ComPtr<ID3D11Query> event;
  D3D11_QUERY_DESC desc = {D3D11_QUERY_EVENT, 0};
  HRESULT result = device_->CreateQuery(&desc, event.GetAddressOf());
  if (FAILED(result)) {
    return result;
  }
  context_->End(event.Get());
  context_->Flush();
  auto deadline = std::chrono::steady_clock::now() + timeout;
  for (;;) {
    BOOL completed = FALSE;
    result = context_->GetData(event.Get(), &completed, sizeof(completed),
                               D3D11_ASYNC_GETDATA_DONOTFLUSH);
    if (FAILED(result) || (result == S_OK && completed)) {
      return result;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      return HRESULT_FROM_WIN32(WAIT_TIMEOUT);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

HRESULT D3D11Device::CompileShader(std::string_view source, const char* entry, ShaderStage stage,
                                   ID3DBlob** bytecode, std::string& error) {
  if (!bytecode || !entry) {
    error = "Shader bytecode output and entry point are required";
    return E_INVALIDARG;
  }
  *bytecode = nullptr;
  error.clear();
  const char* target;
  switch (stage) {
    case ShaderStage::kVertex:
      target = "vs_5_0";
      break;
    case ShaderStage::kPixel:
      target = "ps_5_0";
      break;
    case ShaderStage::kCompute:
      target = "cs_5_0";
      break;
    default:
      error = "Unsupported Direct3D 11 shader stage";
      return E_INVALIDARG;
  }
  Microsoft::WRL::ComPtr<ID3DBlob> messages;
  HRESULT result =
      D3DCompile(source.data(), source.size(), nullptr, nullptr, nullptr, entry, target,
                 D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_WARNINGS_ARE_ERRORS |
                     D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_IEEE_STRICTNESS,
                 0, bytecode, messages.GetAddressOf());
  if (FAILED(result)) {
    error = messages ? std::string(static_cast<const char*>(messages->GetBufferPointer()),
                                   messages->GetBufferSize())
                     : HResultError("D3DCompile", result);
  }
  return result;
}

}  // namespace rex::ui::d3d11
