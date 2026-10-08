#include <rex/ui/d3d11/d3d11_provider.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/d3d11/d3d11_immediate_drawer.h>
#include <rex/ui/d3d11/d3d11_presenter.h>

REXCVAR_DEFINE_BOOL(d3d11_debug, false, "UI/D3D11", "Enable the Direct3D 11 debug layer.");
REXCVAR_DEFINE_INT32(d3d11_adapter, -1, "UI/D3D11",
                     "Direct3D 11 adapter index; -1 selects a hardware GPU.");

namespace rex::ui::d3d11 {
std::unique_ptr<D3D11Provider> D3D11Provider::Create() {
  D3D11Device::Options options;
  options.debug = REXCVAR_GET(d3d11_debug);
  options.adapter_index = REXCVAR_GET(d3d11_adapter);
  std::string error;
  auto device = D3D11Device::Create(options, error);
  if (!device) {
    REXGPU_ERROR("DX11 initialization failed: {}", error);
    return nullptr;
  }
  return std::unique_ptr<D3D11Provider>(new D3D11Provider(std::move(device)));
}
std::unique_ptr<Presenter> D3D11Provider::CreatePresenter(Presenter::HostGpuLossCallback callback) {
  return D3D11Presenter::Create(*device_, std::move(callback));
}
std::unique_ptr<ImmediateDrawer> D3D11Provider::CreateImmediateDrawer() {
  return D3D11ImmediateDrawer::Create(*device_);
}
}  // namespace rex::ui::d3d11
