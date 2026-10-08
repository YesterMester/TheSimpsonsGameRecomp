#pragma once

#include <rex/ui/d3d11/d3d11_device.h>
#include <rex/ui/graphics_provider.h>

namespace rex::ui::d3d11 {
class D3D11Provider final : public GraphicsProvider {
 public:
  static std::unique_ptr<D3D11Provider> Create();
  D3D11Device& device() const { return *device_; }
  std::unique_ptr<Presenter> CreatePresenter(
      Presenter::HostGpuLossCallback callback = Presenter::FatalErrorHostGpuLossCallback) override;
  std::unique_ptr<ImmediateDrawer> CreateImmediateDrawer() override;

 private:
  explicit D3D11Provider(std::unique_ptr<D3D11Device> device) : device_(std::move(device)) {}
  std::unique_ptr<D3D11Device> device_;
};
}  // namespace rex::ui::d3d11
