#pragma once

#include <array>
#include <rex/ui/d3d11/d3d11_image_renderer.h>
#include <rex/ui/d3d11/d3d11_swap_chain.h>
#include <rex/ui/presenter.h>

namespace rex::ui::d3d11 {
class D3D11UIDrawContext final : public UIDrawContext {
 public:
  D3D11UIDrawContext(Presenter& presenter, uint32_t width, uint32_t height,
                     ID3D11RenderTargetView* target)
      : UIDrawContext(presenter, width, height), target_(target) {}
  ID3D11RenderTargetView* render_target() const { return target_.Get(); }

 private:
  Microsoft::WRL::ComPtr<ID3D11RenderTargetView> target_;
};
class D3D11Presenter final : public Presenter {
 public:
  class RefreshContext final : public GuestOutputRefreshContext {
   public:
    RefreshContext(bool& is_8bpc, ID3D11RenderTargetView* target, ID3D11UnorderedAccessView* write)
        : GuestOutputRefreshContext(is_8bpc), target_(target), write_(write) {}
    ID3D11RenderTargetView* render_target() const { return target_.Get(); }
    ID3D11UnorderedAccessView* unordered_access() const { return write_.Get(); }

   private:
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> target_;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> write_;
  };
  static std::unique_ptr<D3D11Presenter> Create(D3D11Device& device, HostGpuLossCallback callback);
  ~D3D11Presenter() override;
  Surface::TypeFlags GetSupportedSurfaceTypes() const override {
    return Surface::kTypeFlag_Win32Hwnd;
  }
  bool CaptureGuestOutput(RawImage& image) override;

 protected:
  SurfacePaintConnectResult ConnectOrReconnectPaintingToSurfaceFromUIThread(
      Surface& surface, uint32_t width, uint32_t height, bool was_paintable,
      bool& implicit_vsync) override;
  void DisconnectPaintingFromSurfaceFromUIThreadImpl() override;
  bool RefreshGuestOutputImpl(uint32_t mailbox_index, uint32_t width, uint32_t height,
                              std::function<bool(GuestOutputRefreshContext&)> refresher,
                              bool& is_8bpc) override;
  PaintResult PaintAndPresentImpl(bool execute_ui_drawers) override;

 private:
  struct Image {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> target;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> write;
    uint32_t width = 0, height = 0;
  };
  D3D11Presenter(D3D11Device& device, HostGpuLossCallback callback)
      : Presenter(std::move(callback)), device_(device), renderer_(device) {}
  bool PrepareImage(Image& image, uint32_t width, uint32_t height);
  D3D11Device& device_;
  D3D11ImageRenderer renderer_;
  std::array<Image, kGuestOutputMailboxSize> images_;
  std::array<Image, kMaxGuestOutputPaintEffects - 1> intermediates_;
  std::unique_ptr<D3D11SwapChain> swap_chain_;
  HWND window_ = nullptr;
  uint32_t width_ = 0, height_ = 0;
};
}  // namespace rex::ui::d3d11
