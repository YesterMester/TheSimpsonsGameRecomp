#include <rex/graphics/d3d11/graphics_system.h>

#include <rex/graphics/d3d11/command_processor.h>
#include <rex/ui/d3d11/d3d11_provider.h>

namespace rex::graphics::d3d11 {
std::string D3D11GraphicsSystem::name() const {
  auto* provider = static_cast<ui::d3d11::D3D11Provider*>(this->provider());
  return provider ? "Direct3D 11 (" + provider->device().features().adapter_name + ")"
                  : "Direct3D 11";
}
void D3D11GraphicsSystem::CreateProvider(bool with_presentation) {
  provider_ = ui::d3d11::D3D11Provider::Create();
}
std::unique_ptr<CommandProcessor> D3D11GraphicsSystem::CreateCommandProcessor() {
  return std::make_unique<D3D11CommandProcessor>(this, kernel_state_);
}
}  // namespace rex::graphics::d3d11
