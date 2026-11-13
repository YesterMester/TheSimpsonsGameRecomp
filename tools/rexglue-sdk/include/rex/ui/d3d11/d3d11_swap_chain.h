#pragma once

#include <rex/ui/d3d11/d3d11_device.h>

namespace rex::ui::d3d11 {

// Windowed flip presentation for Windows 10/11. The owner keeps the HWND and
// device alive and serializes commands with the rendering context.
class D3D11SwapChain {
 public:
  enum class PresentationMode { kAutomatic, kFlipDiscard, kFlipSequential, kDiscard };
  static std::unique_ptr<D3D11SwapChain> Create(
      D3D11Device& device, HWND window, uint32_t width, uint32_t height, std::string& error,
      PresentationMode mode = PresentationMode::kAutomatic);
  ~D3D11SwapChain();
  D3D11SwapChain(const D3D11SwapChain&) = delete;
  D3D11SwapChain& operator=(const D3D11SwapChain&) = delete;

  ID3D11Texture2D* back_buffer() const { return back_buffer_.Get(); }
  ID3D11RenderTargetView* render_target() const { return render_target_.Get(); }
  bool tearing_supported() const { return tearing_supported_; }
  bool implicit_vsync() const { return !frame_ready_; }
  DXGI_SWAP_EFFECT swap_effect() const { return swap_effect_; }
  UINT flags() const { return flags_; }
  const std::string& creation_diagnostics() const { return creation_diagnostics_; }

  // Wait before drawing, including the first frame. No CPU spin or old-frame
  // shortcut is needed; the frame-latency handle provides back pressure.
  HRESULT WaitForFrame(uint32_t timeout_ms = 1000);
  HRESULT Present(bool vsync);
  // Clears immediate-context bindings. The renderer must rebind its state.
  // Borrowed back-buffer views must not be retained across a resize.
  HRESULT Resize(uint32_t width, uint32_t height);

 private:
  explicit D3D11SwapChain(D3D11Device& device) : device_(device) {}
  bool Initialize(HWND window, uint32_t width, uint32_t height, std::string& error,
                  PresentationMode mode);
  void ReleaseSwapChain();
  HRESULT AcquireBackBuffer();

  D3D11Device& device_;
  Microsoft::WRL::ComPtr<IDXGISwapChain1> swap_chain_;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> back_buffer_;
  Microsoft::WRL::ComPtr<ID3D11RenderTargetView> render_target_;
  HANDLE frame_ready_ = nullptr;
  UINT flags_ = 0;
  DXGI_SWAP_EFFECT swap_effect_ = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  bool tearing_supported_ = false;
  std::string creation_diagnostics_;
};

}  // namespace rex::ui::d3d11
