#include "native_gpu_state.h"
#include "native_gpu_samplers.h"
#include "native_gpu_conditional.h"
#include "native_gpu_initialization.h"
#include "native_gpu_fences.h"
#include "native_gpu_shader_linkage.h"
#include "native_gpu_vertex_declaration.h"
#include "native_gpu_shader_resources.h"
#include "native_gpu_resource_binding.h"
#include "native_gpu_resource_initialization.h"
#include "native_gpu_inline_shader.h"
#include "native_gpu_shader_constants.h"
#include "native_gpu_shader_control.h"
#include "native_gpu_shader_coordinator.h"
#include "native_gpu_buffer_coherency.h"

#include <rex/cvar.h>
#include <rex/graphics/native_gpu_calls.h>
#include <rex/ppc.h>

REXCVAR_DECLARE(bool, native_gpu_calls);
REX_EXTERN(__imp__sub_8245E8C8);
REX_EXTERN(__imp__sub_8245EB08);
REX_EXTERN(__imp__sub_8245E988);
REX_EXTERN(__imp__sub_8245E530);
REX_EXTERN(__imp__sub_82444700);
REX_EXTERN(__imp__sub_824449E8);
REX_EXTERN(__imp__sub_824617D8);
REX_EXTERN(__imp__sub_824618F8);
REX_EXTERN(__imp__sub_8245F3D8);
REX_EXTERN(__imp__sub_8245F4A8);
REX_EXTERN(__imp__sub_8245F560);
REX_EXTERN(__imp__sub_8245EDC8);
REX_EXTERN(__imp__sub_8245F8C0);
REX_EXTERN(__imp__sub_82445278);
REX_EXTERN(__imp__sub_82445578);
REX_EXTERN(__imp__sub_824408E0);
REX_EXTERN(__imp__sub_82445798);
REX_EXTERN(__imp__sub_82445240);
REX_EXTERN(__imp__sub_82445480);
REX_EXTERN(__imp__sub_824457F0);
REX_EXTERN(__imp__sub_8245F6B0);
REX_EXTERN(__imp__sub_8245F810);
REX_EXTERN(__imp__sub_8245E330);
REX_EXTERN(__imp__sub_8245FA00);
REX_EXTERN(__imp__sub_824415D0);
REX_EXTERN(__imp__sub_8244E208);

namespace {
bool StateEnabled() {
  static const bool enabled = REXCVAR_GET(native_gpu_calls);
  return enabled && (rex::graphics::native_gpu_calls::Capabilities() &
                     rex::graphics::native_gpu_state::kCapability);
}

bool CounterEventsEnabled() {
  static const bool enabled = REXCVAR_GET(native_gpu_calls);
  return enabled && (rex::graphics::native_gpu_calls::Capabilities() &
                     rex::graphics::native_gpu_sync::kCapability);
}

bool ShaderLinkageEnabled() {
  static const bool enabled = REXCVAR_GET(native_gpu_calls);
  return enabled && (rex::graphics::native_gpu_calls::Capabilities() &
                     rex::graphics::native_gpu_program::kCapability);
}

bool ShaderConstantsEnabled() {
  static const bool enabled = REXCVAR_GET(native_gpu_calls);
  return enabled && (rex::graphics::native_gpu_calls::Capabilities() &
                     rex::graphics::native_gpu_indirect::kCapability);
}
}  // namespace

REX_FUNC(sub_824415D0) {
  if (!CounterEventsEnabled() || !simpsons::native_gpu_calls::WriteBufferCoherency(ctx, base, true))
    __imp__sub_824415D0(ctx, base);
}

REX_FUNC(sub_8244E208) {
  if (!CounterEventsEnabled() || !simpsons::native_gpu_calls::WriteBufferCoherency(ctx, base, false))
    __imp__sub_8244E208(ctx, base);
}

REX_FUNC(sub_8245E8C8) {
  if (!StateEnabled() || !simpsons::native_gpu_state::WriteState(ctx, base, false))
    __imp__sub_8245E8C8(ctx, base);
}

REX_FUNC(sub_8245EB08) {
  if (!StateEnabled() || !simpsons::native_gpu_state::WriteState(ctx, base, true))
    __imp__sub_8245EB08(ctx, base);
}

REX_FUNC(sub_8245E988) {
  if (!StateEnabled() ||
      !(rex::graphics::native_gpu_calls::Capabilities() &
        rex::graphics::native_gpu_coherency::kCapability) ||
      !simpsons::native_gpu_samplers::WriteSamplers(ctx, base))
    __imp__sub_8245E988(ctx, base);
}

REX_FUNC(sub_8245E530) {
  if (!StateEnabled() ||
      !(rex::graphics::native_gpu_calls::Capabilities() &
        rex::graphics::native_gpu_predication::kCapability) ||
      !simpsons::native_gpu_state::WriteConditional(ctx, base))
    __imp__sub_8245E530(ctx, base);
}

REX_FUNC(sub_82444700) {
  if (!StateEnabled() ||
      !(rex::graphics::native_gpu_calls::Capabilities() &
        rex::graphics::native_gpu_program::kCapability) ||
      !simpsons::native_gpu_calls::WriteInitializationSetup(ctx, base))
    __imp__sub_82444700(ctx, base);
}

REX_FUNC(sub_824449E8) {
  if (!StateEnabled() ||
      !(rex::graphics::native_gpu_calls::Capabilities() &
        rex::graphics::native_gpu_ports::kCapability) ||
      !simpsons::native_gpu_calls::WriteInitializationGpr(ctx, base))
    __imp__sub_824449E8(ctx, base);
}

REX_FUNC(sub_824617D8) {
  if (!CounterEventsEnabled() || !simpsons::native_gpu_calls::WriteCounterEvent(ctx, base))
    __imp__sub_824617D8(ctx, base);
}

REX_FUNC(sub_824618F8) {
  if (!CounterEventsEnabled() || !simpsons::native_gpu_calls::WriteCounterSlotEvent(ctx, base))
    __imp__sub_824618F8(ctx, base);
}

REX_FUNC(sub_8245F3D8) {
  if (!ShaderLinkageEnabled() || !simpsons::native_gpu_calls::PatchShaderLinkage(ctx, base, true))
    __imp__sub_8245F3D8(ctx, base);
}

REX_FUNC(sub_8245F4A8) {
  if (!ShaderLinkageEnabled() || !simpsons::native_gpu_calls::PatchShaderLinkage(ctx, base, false))
    __imp__sub_8245F4A8(ctx, base);
}

REX_FUNC(sub_8245F560) {
  if (!ShaderLinkageEnabled() || !simpsons::native_gpu_calls::LinkShaderExports(ctx, base))
    __imp__sub_8245F560(ctx, base);
}

REX_FUNC(sub_8245EDC8) {
  if (!ShaderLinkageEnabled() || !simpsons::native_gpu_calls::PatchVertexDeclaration(ctx, base))
    __imp__sub_8245EDC8(ctx, base);
}

REX_FUNC(sub_8245F8C0) {
  if (!ShaderLinkageEnabled() || !simpsons::native_gpu_calls::PrepareShaderResource(ctx, base))
    __imp__sub_8245F8C0(ctx, base);
}

REX_FUNC(sub_8245F6B0) {
  if (!ShaderLinkageEnabled() || !simpsons::native_gpu_calls::WriteInlineResourceShader(ctx, base))
    __imp__sub_8245F6B0(ctx, base);
}

REX_FUNC(sub_8245F810) {
  if (!ShaderConstantsEnabled() || !simpsons::native_gpu_calls::LoadShaderConstants(ctx, base))
    __imp__sub_8245F810(ctx, base);
}

REX_FUNC(sub_8245E330) {
  if (!StateEnabled() ||
      !(rex::graphics::native_gpu_calls::Capabilities() &
        rex::graphics::native_gpu_ports::kCapability) ||
      !simpsons::native_gpu_calls::PrepareShaderControl(ctx, base))
    __imp__sub_8245E330(ctx, base);
}

REX_FUNC(sub_8245FA00) {
  constexpr uint32_t capabilities = rex::graphics::native_gpu_state::kCapability |
                                    rex::graphics::native_gpu_ports::kCapability |
                                    rex::graphics::native_gpu_program::kCapability |
                                    rex::graphics::native_gpu_indirect::kCapability |
                                    rex::graphics::native_gpu_shader_reference::kCapability |
                                    rex::graphics::native_gpu_predication::kCapability;
  if (!StateEnabled() ||
      (rex::graphics::native_gpu_calls::Capabilities() & capabilities) != capabilities ||
      !simpsons::native_gpu_calls::CoordinateShaders(ctx, base))
    __imp__sub_8245FA00(ctx, base);
}

REX_FUNC(sub_82445278) {
  if (!ShaderLinkageEnabled() || !simpsons::native_gpu_calls::BindShaderResource(ctx, base, false))
    __imp__sub_82445278(ctx, base);
}

REX_FUNC(sub_82445578) {
  if (!ShaderLinkageEnabled() || !simpsons::native_gpu_calls::BindShaderResource(ctx, base, true))
    __imp__sub_82445578(ctx, base);
}

REX_FUNC(sub_824408E0) {
  if (!ShaderLinkageEnabled() || !simpsons::native_gpu_calls::BindTextureResource(ctx, base))
    __imp__sub_824408E0(ctx, base);
}

REX_FUNC(sub_82445798) {
  if (!ShaderLinkageEnabled() || !simpsons::native_gpu_calls::BindVertexDeclaration(ctx, base))
    __imp__sub_82445798(ctx, base);
}

REX_FUNC(sub_82445240) {
  if (!ShaderLinkageEnabled() || !simpsons::native_gpu_calls::InitializePixelResource(ctx, base))
    __imp__sub_82445240(ctx, base);
}

REX_FUNC(sub_82445480) {
  if (!ShaderLinkageEnabled() || !simpsons::native_gpu_calls::InitializeVertexShaderTemplates(ctx, base))
    __imp__sub_82445480(ctx, base);
}

REX_FUNC(sub_824457F0) {
  if (!ShaderLinkageEnabled() || !simpsons::native_gpu_calls::InitializeVertexDeclaration(ctx, base))
    __imp__sub_824457F0(ctx, base);
}
