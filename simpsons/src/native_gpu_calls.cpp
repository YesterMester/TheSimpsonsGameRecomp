#include "native_gpu_calls.h"
#include "native_gpu_vertices.h"
#include "native_gpu_indexed.h"
#include "native_gpu_aux_draws.h"
#include "native_gpu_clear.h"
#include "native_gpu_special.h"
#include "native_gpu_begin.h"
#include "native_gpu_utility.h"
#include "native_gpu_initialization_frame.h"
#include "native_gpu_retirement.h"
#include "native_gpu_resolve_draws.h"
#include "native_gpu_indirect.h"
#include "native_gpu_shader_setup.h"
#include "native_gpu_shader_reference.h"
#include "native_gpu_shader_state.h"
#include "native_gpu_buffer_coherency.h"

#include <rex/cvar.h>
#include <rex/ppc.h>

REX_EXTERN(sub_82456D60);
REX_EXTERN(sub_8238E880);
REX_EXTERN(__savegprlr_28);
REX_EXTERN(__restgprlr_28);
REX_EXTERN(__imp__sub_82444700);
REX_EXTERN(__imp__sub_824449E8);

REXCVAR_DEFINE_BOOL(native_gpu_calls, true, "GPU",
                    "Native draw builders, bounded state flushes, Begin/EndVertices "
                    "and constants. Unconverted branches keep their original submission path.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace simpsons::native_gpu_calls {

bool TryUnlockCoherency(PPCContext& ctx, uint8_t* base) {
  static const bool enabled = REXCVAR_GET(native_gpu_calls);
  return enabled &&
         (rex::graphics::native_gpu_calls::Capabilities() & rex::graphics::native_gpu_sync::kCapability) &&
         WriteUnlockCoherency(ctx, base);
}

bool TryShaderState(uint32_t kind, PPCContext& ctx, uint8_t* base) {
  static const bool enabled = REXCVAR_GET(native_gpu_calls);
  constexpr uint32_t needed = rex::graphics::native_gpu_shader_reference::kCapability |
                              rex::graphics::native_gpu_state::kCapability |
                              rex::graphics::native_gpu_predication::kCapability |
                              rex::graphics::native_gpu_predication::kRangeCapability;
  if (!enabled || (rex::graphics::native_gpu_calls::Capabilities() & needed) != needed)
    return false;
  return kind == 0 ? WritePixelShaderReference(ctx, base)
                   : kind == 1 && WriteConditionalShaderState(ctx, base);
}

bool TryPrimaryShaderReference(PPCContext& ctx, uint8_t* base) {
  static const bool enabled = REXCVAR_GET(native_gpu_calls);
  return enabled &&
         (rex::graphics::native_gpu_calls::Capabilities() &
          rex::graphics::native_gpu_shader_reference::kCapability) &&
         WritePrimaryShaderReference(ctx, base);
}

bool TryShaderSetup(uint32_t kind, PPCContext& ctx, uint8_t* base) {
  static const bool enabled = REXCVAR_GET(native_gpu_calls);
  if (!enabled || !(rex::graphics::native_gpu_calls::Capabilities() &
                    rex::graphics::native_gpu_program::kCapability))
    return false;
  if (kind == 0)
    return WriteResourceShaderPrefix(ctx, base);
  if (kind == 1)
    return WriteCopyVertexShaderPrefix(ctx, base);
  return kind == 2 && WriteCopyPixelShaderPrefix(ctx, base);
}

bool TryIndirectConstants(PPCContext& ctx, uint8_t* base) {
  static const bool enabled = REXCVAR_GET(native_gpu_calls);
  if (!enabled || !(rex::graphics::native_gpu_calls::Capabilities() &
                    rex::graphics::native_gpu_indirect::kCapability))
    return false;
  return WriteIndirectConstants(ctx, base) ||
         ((rex::graphics::native_gpu_calls::Capabilities() &
           rex::graphics::native_gpu_indirect::kTypedCapability) &&
          WriteIndirectConstants(ctx, base, true));
}

namespace {
bool ResolveDrawsEnabled() {
  static const bool enabled = REXCVAR_GET(native_gpu_calls);
  constexpr uint32_t capabilities = rex::graphics::native_gpu_state::kCapability |
                                    rex::graphics::native_gpu_calls::kVerticesCapability;
  return enabled &&
         (rex::graphics::native_gpu_calls::Capabilities() & capabilities) == capabilities;
}
}  // namespace

bool TryClearResolveDraw(PPCContext& ctx, uint8_t* base) {
  return ResolveDrawsEnabled() && WriteClearResolveDraw(ctx, base);
}

bool TryCopyResolveDraw(PPCContext& ctx, uint8_t* base) {
  return ResolveDrawsEnabled() && WriteCopyResolveDraw(ctx, base);
}

bool TryRetirementChunk(PPCContext& ctx, uint8_t* base) {
  static const bool enabled = REXCVAR_GET(native_gpu_calls);
  constexpr uint32_t capabilities = rex::graphics::native_gpu_state::kCapability |
                                    rex::graphics::native_gpu_sync::kCapability |
                                    rex::graphics::native_gpu_calls::kVerticesCapability;
  return enabled &&
         (rex::graphics::native_gpu_calls::Capabilities() & capabilities) == capabilities &&
         WriteRetirementChunk(ctx, base);
}

bool TrySimpleDraw(uint32_t address, PPCContext& ctx, uint8_t* base) {
  static const bool enabled = REXCVAR_GET(native_gpu_calls);
  direct_draw_headers_enabled = enabled && (rex::graphics::native_gpu_calls::Capabilities() &
                                            rex::graphics::native_gpu_direct::kCapability);
  direct_state_headers_enabled = enabled && (rex::graphics::native_gpu_calls::Capabilities() &
                                             rex::graphics::native_gpu_state::kCapability);
  if (!IsSimpleDraw(address) && address != 0x8244C450 && address != 0x8244C970 &&
      address != 0x8244CF78 && address != 0x8244D360 && address != 0x82444A48 &&
      address != 0x8246AEF0 && address != 0x824529C0 && address != 0x8244D7B8 &&
      address != 0x8244DC30 && address != 0x8246AF58 && address != 0x82444A80)
    return false;
  if (!enabled || !rex::graphics::native_gpu_calls::Enabled())
    return false;
  const bool conditional = rex::graphics::native_gpu_calls::Capabilities() &
                           rex::graphics::native_gpu_predication::kCapability;
  if (address == 0x82444A80) {
    constexpr uint32_t capabilities = rex::graphics::native_gpu_state::kCapability |
                                      rex::graphics::native_gpu_program::kCapability |
                                      rex::graphics::native_gpu_ports::kCapability |
                                      rex::graphics::native_gpu_calls::kVerticesCapability;
    return (rex::graphics::native_gpu_calls::Capabilities() & capabilities) == capabilities &&
           InitializeGraphicsBuffers(ctx, base, sub_8238E880, __savegprlr_28, __restgprlr_28,
                                     __imp__sub_82444700, __imp__sub_824449E8);
  }
  if (address == 0x8246AF58) {
    constexpr uint32_t capabilities = rex::graphics::native_gpu_state::kCapability |
                                      rex::graphics::native_gpu_coherency::kCapability |
                                      rex::graphics::native_gpu_program::kCapability;
    return (rex::graphics::native_gpu_calls::Capabilities() & capabilities) == capabilities &&
           WriteUtilityDraw(ctx, base);
  }
  if (address == 0x8244D7B8 || address == 0x8244DC30)
    return (rex::graphics::native_gpu_calls::Capabilities() &
            rex::graphics::native_gpu_state::kCapability) &&
           (rex::graphics::native_gpu_calls::Capabilities() &
            rex::graphics::native_gpu_calls::kRangeDrawCapability) &&
           native_gpu_vertices::DrawVertexIdRange(
               ctx, base, address == 0x8244DC30,
               rex::graphics::native_gpu_calls::Capabilities() &
                   rex::graphics::native_gpu_coherency::kCapability,
               conditional);
  if (address == 0x824529C0)
    return (rex::graphics::native_gpu_calls::Capabilities() &
            rex::graphics::native_gpu_state::kCapability) &&
           native_gpu_clear::DrawClear(ctx, base, sub_82456D60);
  if (address == 0x82444A48 || address == 0x8246AEF0)
    return (rex::graphics::native_gpu_calls::Capabilities() &
            rex::graphics::native_gpu_calls::kVerticesCapability) &&
           (address == 0x82444A48 ? WriteWarmupDraws(ctx, base) : WriteBareTriangleDraw(ctx, base));
  if (address == 0x8244C450)
    return (rex::graphics::native_gpu_calls::Capabilities() &
            rex::graphics::native_gpu_calls::kVerticesCapability) &&
           native_gpu_vertices::BeginWithState(ctx, base, sub_82456D60, false,
                                               rex::graphics::native_gpu_calls::Capabilities() &
                                                   rex::graphics::native_gpu_state::kCapability,
                                               rex::graphics::native_gpu_calls::Capabilities() &
                                                   rex::graphics::native_gpu_coherency::kCapability,
                                               conditional);
  if (address == 0x8244C970)
    return (rex::graphics::native_gpu_calls::Capabilities() &
            rex::graphics::native_gpu_calls::kIndexedVerticesCapability) &&
           native_gpu_vertices::BeginWithState(ctx, base, sub_82456D60, true,
                                               rex::graphics::native_gpu_calls::Capabilities() &
                                                   rex::graphics::native_gpu_state::kCapability,
                                               rex::graphics::native_gpu_calls::Capabilities() &
                                                   rex::graphics::native_gpu_coherency::kCapability,
                                               conditional);
  if (address == 0x8244CF78 || address == 0x8244D360)
    return (rex::graphics::native_gpu_calls::Capabilities() &
            rex::graphics::native_gpu_calls::kRangeDrawCapability) &&
           native_gpu_vertices::DrawRange(ctx, base, address == 0x8244D360,
                                          rex::graphics::native_gpu_calls::Capabilities() &
                                              rex::graphics::native_gpu_state::kCapability,
                                          rex::graphics::native_gpu_calls::Capabilities() &
                                              rex::graphics::native_gpu_coherency::kCapability,
                                          conditional);
  return WriteSimpleDraw(base, address, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32,
                         ctx.r8.u32);
}

}  // namespace simpsons::native_gpu_calls
