#include <rex/ui/d3d11/d3d11_swap_chain.h>

#include <cstdio>
#include <array>

namespace rex::ui::d3d11 {

namespace {
std::string SwapError(const char* operation, HRESULT result) {
  char message[160];
  std::snprintf(message, sizeof(message), "%s failed (0x%08X)", operation, unsigned(result));
  return message;
}
}  // namespace

std::unique_ptr<D3D11SwapChain> D3D11SwapChain::Create(D3D11Device& device, HWND window,
                                                       uint32_t width, uint32_t height,
                                                       std::string& error, PresentationMode mode) {
  error.clear();
  auto result = std::unique_ptr<D3D11SwapChain>(new D3D11SwapChain(device));
  if (!result->Initialize(window, width, height, error, mode)) {
    return nullptr;
  }
  return result;
}

D3D11SwapChain::~D3D11SwapChain() {
  ReleaseSwapChain();
}

void D3D11SwapChain::ReleaseSwapChain() {
  // DXGI permits one flip chain per HWND. Releasing the COM pointers alone
  // leaves D3D11's deferred destruction pending and can make reconnect fail
  // with DXGI_ERROR_INVALID_CALL. Flush only at destruction, never per frame.
  if (swap_chain_) {
    device_.context()->ClearState();
    render_target_.Reset();
    back_buffer_.Reset();
    swap_chain_.Reset();
    device_.context()->Flush();
  }
  // The caller closes the waitable handle returned by DXGI.
  if (frame_ready_) {
    CloseHandle(frame_ready_);
    frame_ready_ = nullptr;
  }
}

bool D3D11SwapChain::Initialize(HWND window, uint32_t width, uint32_t height, std::string& error,
                                PresentationMode mode) {
  if (mode < PresentationMode::kAutomatic || mode > PresentationMode::kDiscard || !window ||
      !IsWindow(window) || !width || !height || width > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
      height > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION) {
    error = "A valid window and nonzero Direct3D 11 surface size are required";
    return false;
  }
  Microsoft::WRL::ComPtr<IDXGIFactory2> factory2;
  HRESULT result = device_.factory()->QueryInterface(IID_PPV_ARGS(factory2.GetAddressOf()));
  if (FAILED(result)) {
    error = SwapError("IDXGIFactory2", result);
    return false;
  }
  Microsoft::WRL::ComPtr<IDXGIFactory5> factory5;
  BOOL allow_tearing = FALSE;
  if (FAILED(factory2.As(&factory5)) ||
      FAILED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow_tearing,
                                           sizeof(allow_tearing))))
    allow_tearing = FALSE;
  DXGI_SWAP_CHAIN_DESC1 desc = {};
  desc.Width = width;
  desc.Height = height;
  desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  desc.SampleDesc.Count = 1;
  desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  desc.Scaling = DXGI_SCALING_STRETCH;
  desc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
  struct Candidate {
    DXGI_SWAP_EFFECT effect;
    UINT flags;
  };
  constexpr UINT waitable = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
  const std::array candidates = {
      Candidate{DXGI_SWAP_EFFECT_FLIP_DISCARD,
                waitable | (allow_tearing ? UINT(DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) : 0u)},
      Candidate{DXGI_SWAP_EFFECT_FLIP_DISCARD, waitable},
      Candidate{DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL, waitable},
      Candidate{DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL, 0}, Candidate{DXGI_SWAP_EFFECT_DISCARD, 0}};
  for (size_t i = 0; i < candidates.size(); ++i) {
    const auto& candidate = candidates[i];
    if (mode == PresentationMode::kAutomatic) {
      if (i == 1 && !allow_tearing)
        continue;
    } else if ((mode == PresentationMode::kFlipDiscard && i != 0) ||
               (mode == PresentationMode::kFlipSequential && i != 3) ||
               (mode == PresentationMode::kDiscard && i != 4)) {
      continue;
    }
    desc.SwapEffect = candidate.effect;
    desc.Flags = candidate.flags;
    desc.BufferCount = candidate.effect == DXGI_SWAP_EFFECT_DISCARD ? 1 : 2;
    result = factory2->CreateSwapChainForHwnd(device_.device(), window, &desc, nullptr, nullptr,
                                              swap_chain_.GetAddressOf());
    const char* operation = "CreateSwapChainForHwnd";
    if (SUCCEEDED(result)) {
      if (candidate.flags & waitable) {
        Microsoft::WRL::ComPtr<IDXGISwapChain2> chain2;
        operation = "Swap-chain frame latency";
        result = swap_chain_.As(&chain2);
        if (SUCCEEDED(result))
          result = chain2->SetMaximumFrameLatency(1);
        if (SUCCEEDED(result)) {
          frame_ready_ = chain2->GetFrameLatencyWaitableObject();
          if (!frame_ready_)
            result = DXGI_ERROR_UNSUPPORTED;
        }
      } else {
        // Older presentation paths have no latency handle. Keep their queue
        // bounded and let synchronized Present provide back pressure instead.
        Microsoft::WRL::ComPtr<IDXGIDevice1> dxgi_device;
        operation = "Device frame latency";
        result = device_.device()->QueryInterface(IID_PPV_ARGS(dxgi_device.GetAddressOf()));
        if (SUCCEEDED(result))
          result = dxgi_device->SetMaximumFrameLatency(1);
      }
    }
    if (SUCCEEDED(result)) {
      flags_ = candidate.flags;
      swap_effect_ = candidate.effect;
      tearing_supported_ = (flags_ & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) != 0;
      break;
    }
    char configuration[160];
    std::snprintf(configuration, sizeof(configuration),
                  "%s (effect=%u, flags=0x%X, %ux%u): 0x%08X; ", operation,
                  unsigned(candidate.effect), candidate.flags, width, height, unsigned(result));
    creation_diagnostics_ += configuration;
    ReleaseSwapChain();
    // Device loss and memory errors are not unsupported presentation modes.
    if (result != DXGI_ERROR_INVALID_CALL && result != DXGI_ERROR_UNSUPPORTED &&
        result != E_INVALIDARG && result != E_NOINTERFACE)
      break;
  }
  if (!swap_chain_) {
    error = "DX11 could not create a presentation surface: " + creation_diagnostics_;
    return false;
  }
  result = device_.factory()->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);
  if (FAILED(result)) {
    error = SwapError("MakeWindowAssociation", result);
    return false;
  }
  result = AcquireBackBuffer();
  if (FAILED(result)) {
    error = SwapError("Swap-chain back buffer", result);
    return false;
  }
  return true;
}

HRESULT D3D11SwapChain::AcquireBackBuffer() {
  HRESULT result = swap_chain_->GetBuffer(0, IID_PPV_ARGS(back_buffer_.GetAddressOf()));
  if (FAILED(result)) {
    return result;
  }
  return device_.device()->CreateRenderTargetView(back_buffer_.Get(), nullptr,
                                                  render_target_.GetAddressOf());
}

HRESULT D3D11SwapChain::WaitForFrame(uint32_t timeout_ms) {
  if (!frame_ready_)
    return S_OK;
  DWORD result = WaitForSingleObjectEx(frame_ready_, timeout_ms, FALSE);
  if (result == WAIT_OBJECT_0) {
    return S_OK;
  }
  if (result == WAIT_TIMEOUT) {
    return HRESULT_FROM_WIN32(WAIT_TIMEOUT);
  }
  if (result == WAIT_FAILED) {
    DWORD error = GetLastError();
    return error ? HRESULT_FROM_WIN32(error) : E_FAIL;
  }
  return E_FAIL;
}

HRESULT D3D11SwapChain::Present(bool vsync) {
  vsync |= implicit_vsync();
  return swap_chain_->Present(vsync ? 1 : 0,
                              !vsync && tearing_supported_ ? DXGI_PRESENT_ALLOW_TEARING : 0);
}

HRESULT D3D11SwapChain::Resize(uint32_t width, uint32_t height) {
  if (!width || !height || width > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
      height > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION) {
    return E_INVALIDARG;
  }
  HRESULT result = device_.WaitForCompletion();
  if (FAILED(result)) {
    return result;
  }
  device_.context()->ClearState();
  render_target_.Reset();
  back_buffer_.Reset();
  result = swap_chain_->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, flags_);
  if (FAILED(result)) {
    return result;
  }
  return AcquireBackBuffer();
}

}  // namespace rex::ui::d3d11
