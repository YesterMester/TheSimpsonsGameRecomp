#pragma once

#include <rex/graphics/pipeline/shader/dxbc.h>
#include <rex/graphics/register_file.h>
#include <rex/ui/d3d11/d3d11_api.h>

namespace rex::graphics::d3d11 {

// Fetch-constant state plus each shader instruction's filtering overrides.
// LOD bias, signs and component swizzles are handled by the compiled shader.
D3D11_SAMPLER_DESC BuildGuestSamplerDescription(const RegisterFile& registers,
                                                const DxbcShader::SamplerBinding& binding,
                                                int32_t anisotropic_override = -1);

}  // namespace rex::graphics::d3d11
