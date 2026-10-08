#include <rex/ui/d3d11/d3d11_swap_chain.h>

#include <cstdio>

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
                                                       std::string& error) {
  error.clear();
  auto result = std::unique_ptr<D3D11SwapChain>(new D3D11SwapChain(device));
  if (!result->Initialize(window, width, height, error)) {
    return nullptr;
  }
  return result;
}

D3D11SwapChain::~D3D11SwapChain() {
  // The caller closes the waitable handle returned by DXGI.
  if (frame_ready_) {
    CloseHandle(frame_ready_);
  }
}

bool D3D11SwapChain::Initialize(HWND window, uint32_t width, uint32_t height, std::string& error) {
  if (!window || !width || !height || width > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
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
  if (SUCCEEDED(factory2.As(&factory5)) &&
      SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow_tearing,
                                              sizeof(allow_tearing)))) {
    tearing_supported_ = allow_tearing != FALSE;
  }
  flags_ = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT |
           (tearing_supported_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
  DXGI_SWAP_CHAIN_DESC1 desc = {};
  desc.Width = width;
  desc.Height = height;
  desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  desc.SampleDesc.Count = 1;
  desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  desc.BufferCount = 2;
  desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  desc.Scaling = DXGI_SCALING_STRETCH;
  desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
  desc.Flags = flags_;
  Microsoft::WRL::ComPtr<IDXGISwapChain1> chain;
  result = factory2->CreateSwapChainForHwnd(device_.device(), window, &desc, nullptr, nullptr,
                                            chain.GetAddressOf());
  if (FAILED(result)) {
    error = SwapError("CreateSwapChainForHwnd", result);
    return false;
  }
  result = device_.factory()->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);
  if (FAILED(result)) {
    error = SwapError("MakeWindowAssociation", result);
    return false;
  }
  result = chain.As(&swap_chain_);
  if (SUCCEEDED(result)) {
    result = swap_chain_->SetMaximumFrameLatency(1);
  }
  if (FAILED(result)) {
    error = SwapError("Swap-chain frame latency", result);
    return false;
  }
  frame_ready_ = swap_chain_->GetFrameLatencyWaitableObject();
  if (!frame_ready_) {
    error = "DXGI did not return its frame-latency waitable handle";
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
