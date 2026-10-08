#pragma once

#include <rex/graphics/graphics_system.h>

namespace rex::graphics::d3d11 {
class D3D11GraphicsSystem final : public GraphicsSystem {
 public:
  std::string name() const override;

 protected:
  void CreateProvider(bool with_presentation) override;
  std::unique_ptr<CommandProcessor> CreateCommandProcessor() override;
};
}  // namespace rex::graphics::d3d11
