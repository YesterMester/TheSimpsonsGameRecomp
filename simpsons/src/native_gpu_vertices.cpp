#include "native_gpu_vertices.h"
#include "native_gpu_recorded_allocator.h"
#include "native_gpu_default_allocator.h"
#include "native_gpu_chunk_growth.h"
#include "native_gpu_chunk_close.h"
#include "native_gpu_chunk_dispatch.h"
#include "native_gpu_submission_state.h"
#include "native_gpu_ring_submission.h"
#include "native_gpu_normal_close.h"
#include "native_gpu_descriptor_list.h"
#include "native_gpu_queued_submission.h"
#include "native_gpu_fence_waits.h"

#include <rex/cvar.h>
#include <rex/ppc.h>

REXCVAR_DECLARE(bool, native_gpu_calls);
REX_EXTERN(sub_8244C8F0);
REX_EXTERN(sub_8244CEA0);
REX_EXTERN(sub_82458E60);
REX_EXTERN(sub_82456D60);
REX_EXTERN(sub_82456B58);
REX_EXTERN(sub_824578C0);
REX_EXTERN(sub_82457210);
REX_EXTERN(sub_82457B40);
REX_EXTERN(sub_82457CC8);
REX_EXTERN(sub_82457F50);
REX_EXTERN(sub_82457050);
REX_EXTERN(sub_824565A8);
REX_EXTERN(sub_82456E68);
REX_EXTERN(sub_824572F8);
REX_EXTERN(sub_82457748);
REX_EXTERN(sub_8245DA50);
REX_EXTERN(sub_82456A90);
REX_EXTERN(sub_824564D0);
REX_EXTERN(sub_824574B8);
REX_EXTERN(sub_82451DE0);
REX_EXTERN(sub_82452018);
REX_EXTERN(sub_82451E10);
REX_EXTERN(__imp__sub_8244C8F0);
REX_EXTERN(__imp__sub_8244CEA0);
REX_EXTERN(__imp__sub_82458E60);
REX_EXTERN(__imp__sub_82456D60);
REX_EXTERN(__imp__sub_82456B58);
REX_EXTERN(__imp__sub_824578C0);
REX_EXTERN(__imp__sub_82457210);
REX_EXTERN(__imp__sub_82457B40);
REX_EXTERN(__imp__sub_82457CC8);
REX_EXTERN(__imp__sub_82457F50);
REX_EXTERN(__imp__sub_82457050);
REX_EXTERN(__imp__sub_824565A8);
REX_EXTERN(__imp__sub_82456E68);
REX_EXTERN(__imp__sub_824572F8);
REX_EXTERN(__imp__sub_82457748);
REX_EXTERN(__imp__sub_8245DA50);
REX_EXTERN(__imp__sub_82456A90);
REX_EXTERN(__imp__sub_824564D0);
REX_EXTERN(__imp__sub_824574B8);

namespace {
struct QueuedSubmissionOps : simpsons::native_gpu_vertices::QueuedKernelLock {
  static void Store(uint8_t* base, uint32_t address, uint32_t value) { REX_MM_STORE_U32(address, value); }
  static void AppendOriginal(PPCContext& ctx, uint8_t* base) { __imp__sub_82456A90(ctx, base); }
  static void PublishOriginal(PPCContext& ctx, uint8_t* base) { __imp__sub_82456E68(ctx, base); }
};
struct FenceWaitOps {
  static void Begin(PPCContext& ctx, uint8_t* base) { sub_82451DE0(ctx, base); }
  static void Poll(PPCContext& ctx, uint8_t* base) { sub_82452018(ctx, base); }
  static void End(PPCContext& ctx, uint8_t* base) { sub_82451E10(ctx, base); }
  static void Dispatch(PPCContext& ctx, uint8_t* base) { sub_82457CC8(ctx, base); }
};
static_assert(offsetof(rex::system::X_KPCR, current_irql) == 0x18);
}  // namespace

REX_FUNC(sub_824564D0) {
  static const bool enabled = REXCVAR_GET(native_gpu_calls);
  if (!enabled || !rex::graphics::native_gpu_calls::Enabled() ||
      !simpsons::native_gpu_vertices::WaitForAllocationFence(ctx, base, FenceWaitOps{}))
    __imp__sub_824564D0(ctx, base);
}

REX_FUNC(sub_824574B8) {
  static const bool enabled = REXCVAR_GET(native_gpu_calls);
  if (!enabled || !rex::graphics::native_gpu_calls::Enabled() ||
      !simpsons::native_gpu_vertices::WaitForGpuMarker(ctx, base, FenceWaitOps{}))
    __imp__sub_824574B8(ctx, base);
}

REX_FUNC(sub_82456A90) {
  static const bool enabled = REXCVAR_GET(native_gpu_calls);
  if (!enabled || !rex::graphics::native_gpu_calls::Enabled() ||
      !simpsons::native_gpu_vertices::AppendLinkedCommandBuffer(
          ctx, base, [base](uint32_t address, uint32_t value) { REX_MM_STORE_U32(address, value); }))
    __imp__sub_82456A90(ctx, base);
}

REX_FUNC(sub_82457748) {
  static const bool enabled = REXCVAR_GET(native_gpu_calls);
  if (!enabled || !rex::graphics::native_gpu_calls::Enabled() ||
      !simpsons::native_gpu_vertices::ExpandDescriptorList(ctx, base))
    __imp__sub_82457748(ctx, base);
}

REX_FUNC(sub_8245DA50) {
  static const bool enabled = REXCVAR_GET(native_gpu_calls);
  if (!enabled || !rex::graphics::native_gpu_calls::Enabled() ||
      !simpsons::native_gpu_vertices::FlushCpuCommandRange(ctx, base))
    __imp__sub_8245DA50(ctx, base);
}

REX_FUNC(sub_824565A8) {
  static const bool enabled = REXCVAR_GET(native_gpu_calls);
  if (!enabled || !rex::graphics::native_gpu_calls::Enabled() ||
      !simpsons::native_gpu_vertices::ReadySubmissionRing(ctx, base))
    __imp__sub_824565A8(ctx, base);
}

REX_FUNC(sub_82456E68) {
  static const bool enabled = REXCVAR_GET(native_gpu_calls);
  if (!enabled || !rex::graphics::native_gpu_calls::Enabled() ||
      !(rex::graphics::native_gpu_calls::Capabilities() &
        rex::graphics::native_gpu_indirect::kBufferCapability) ||
      !simpsons::native_gpu_vertices::PublishSubmissionRing(
          ctx, base, [base](uint32_t address, uint32_t value) { REX_MM_STORE_U32(address, value); }))
    __imp__sub_82456E68(ctx, base);
}

REX_FUNC(sub_824572F8) {
  static const bool enabled = REXCVAR_GET(native_gpu_calls);
  if (!enabled || !rex::graphics::native_gpu_calls::Enabled() ||
      !(rex::graphics::native_gpu_calls::Capabilities() &
        rex::graphics::native_gpu_indirect::kBufferCapability) ||
      (!simpsons::native_gpu_vertices::SubmitQueuedCommandBuffer(ctx, base, QueuedSubmissionOps{}) &&
       !simpsons::native_gpu_vertices::SubmitCommandBuffer(
           ctx, base, [base](uint32_t address, uint32_t value) { REX_MM_STORE_U32(address, value); })))
    __imp__sub_824572F8(ctx, base);
}

REX_FUNC(sub_82457050) {
  static const bool enabled = REXCVAR_GET(native_gpu_calls);
  if (!enabled || !rex::graphics::native_gpu_calls::Enabled() ||
      !(rex::graphics::native_gpu_calls::Capabilities() &
        rex::graphics::native_gpu_sync::kCapability) ||
      !simpsons::native_gpu_vertices::PrepareSubmissionState(ctx, base))
    __imp__sub_82457050(ctx, base);
}

REX_FUNC(sub_82457CC8) {
  static const bool enabled = REXCVAR_GET(native_gpu_calls);
  if (!enabled || !rex::graphics::native_gpu_calls::Enabled() ||
      !simpsons::native_gpu_vertices::DispatchCommandChunk(ctx, base))
    __imp__sub_82457CC8(ctx, base);
}

REX_FUNC(sub_82457F50) {
  static const bool enabled = REXCVAR_GET(native_gpu_calls);
  if (!enabled || !rex::graphics::native_gpu_calls::Enabled() ||
      !simpsons::native_gpu_vertices::ReserveCommandWords(ctx, base))
    __imp__sub_82457F50(ctx, base);
}

REX_FUNC(sub_82457210) {
  static const bool enabled = REXCVAR_GET(native_gpu_calls);
  constexpr uint32_t capabilities = rex::graphics::native_gpu_program::kCapability |
                                    rex::graphics::native_gpu_sync::kCapability;
  if (!enabled || !rex::graphics::native_gpu_calls::Enabled() ||
      (rex::graphics::native_gpu_calls::Capabilities() & capabilities) != capabilities ||
      !simpsons::native_gpu_vertices::WriteChunkCompletion(ctx, base))
    __imp__sub_82457210(ctx, base);
}

REX_FUNC(sub_82457B40) {
  static const bool enabled = REXCVAR_GET(native_gpu_calls);
  if (enabled && rex::graphics::native_gpu_calls::Enabled()) {
    constexpr uint32_t capabilities = rex::graphics::native_gpu_program::kCapability |
                                      rex::graphics::native_gpu_sync::kCapability |
                                      rex::graphics::native_gpu_indirect::kBufferCapability;
    if ((rex::graphics::native_gpu_calls::Capabilities() & capabilities) == capabilities &&
        simpsons::native_gpu_vertices::CloseNormalCommandChunk(
            ctx, base, [base](uint32_t address, uint32_t value) { REX_MM_STORE_U32(address, value); }))
      return;
    if (simpsons::native_gpu_vertices::CloseCommandChunk(ctx, base))
      return;
  }
  __imp__sub_82457B40(ctx, base);
}

REX_FUNC(sub_824578C0) {
  static const bool enabled = REXCVAR_GET(native_gpu_calls);
  if (!enabled || !rex::graphics::native_gpu_calls::Enabled() ||
      !simpsons::native_gpu_vertices::GrowCommandChunk(ctx, base))
    __imp__sub_824578C0(ctx, base);
}

REX_FUNC(sub_82456B58) {
  static const bool enabled = REXCVAR_GET(native_gpu_calls);
  if (!enabled || !rex::graphics::native_gpu_calls::Enabled() ||
      !simpsons::native_gpu_vertices::AllocateDefaultStorage(ctx, base))
    __imp__sub_82456B58(ctx, base);
}

REX_FUNC(sub_82458E60) {
  static const bool enabled = REXCVAR_GET(native_gpu_calls);
  if (!enabled || !rex::graphics::native_gpu_calls::Enabled() ||
      !simpsons::native_gpu_vertices::AllocateRecordedStorage(ctx, base))
    __imp__sub_82458E60(ctx, base);
}

REX_FUNC(sub_82456D60) {
  static const bool enabled = REXCVAR_GET(native_gpu_calls);
  if (!enabled || !rex::graphics::native_gpu_calls::Enabled() ||
      !simpsons::native_gpu_vertices::AllocateTransient(ctx, base))
    __imp__sub_82456D60(ctx, base);
}

REX_FUNC(sub_8244C8F0) {
  static const bool enabled = REXCVAR_GET(native_gpu_calls);
  if (!enabled || !rex::graphics::native_gpu_calls::Enabled() ||
      !simpsons::native_gpu_vertices::EndVertices(ctx, base))
    __imp__sub_8244C8F0(ctx, base);
}

REX_FUNC(sub_8244CEA0) {
  static const bool enabled = REXCVAR_GET(native_gpu_calls);
  if (!enabled || !rex::graphics::native_gpu_calls::Enabled() ||
      !simpsons::native_gpu_vertices::EndVertices(ctx, base))
    __imp__sub_8244CEA0(ctx, base);
}
