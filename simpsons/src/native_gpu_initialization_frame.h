#pragma once

#include "native_gpu_initialization.h"
#include "native_gpu_aux_draws.h"

namespace simpsons::native_gpu_calls {

inline bool InitializationFrameReady(const PPCContext& ctx, const uint8_t* base) {
  using namespace native_gpu_memory;
  MemoryRange device, stack, source;
  return base && !(ctx.r3.u32 & 3) && !(ctx.r1.u32 & 15) && ctx.r1.u32 >= 328 &&
         BackingRange(ctx.r3.u32, 14916, device) && BackingRange(ctx.r1.u32 - 328, 328, stack) &&
         BackingRange(0x8206A0C8, 132, source) && !Overlaps(device, stack) &&
         !Overlaps(device, source) && !Overlaps(stack, source);
}

inline bool InitializationBufferReady(const PPCContext& ctx) {
  using namespace native_gpu_memory;
  MemoryRange device, stack, source, buffer;
  return !(ctx.r3.u32 & 3) && BackingRange(ctx.r3.u32, 8192, buffer) &&
         BackingRange(ctx.r31.u32, 14916, device) && BackingRange(ctx.r1.u32 - 200, 328, stack) &&
         BackingRange(0x8206A0C8, 132, source) && !Overlaps(buffer, device) &&
         !Overlaps(buffer, stack) && !Overlaps(buffer, source);
}

// Allocation remains a call to the original game heap with its original
// context. The continuation consumes that result; it never allocates again
// after a rejected output span or an allocation failure.
template <typename Restore, typename OriginalSetup, typename OriginalGpr>
void FinishInitialization(PPCContext& ctx, uint8_t* base, Restore restore,
                          OriginalSetup original_setup, OriginalGpr original_gpr) {
  const uint64_t buffer = ctx.r3.u64;
  const uint32_t device = ctx.r31.u32;
  bool native = ctx.r3.u32 && InitializationBufferReady(ctx);
  ctx.r30.u64 = buffer;
  ctx.r28.s64 = ctx.r30.s64 + 8192;
  if (!ctx.r30.u32) {
    ctx.r3.u64 = 0;
    ctx.r1.s64 += 128;
    restore(ctx, base);
    return;
  }
  const auto physical = [](uint32_t address) {
    return (address & 0x1FFFFFFFu) + (((address >> 20) + 512) & 0x1000);
  };
  const auto descriptor = [&](uint32_t offset, uint32_t count, uint32_t address) {
    StoreGuestU32(base, device + offset,
                  (LoadGuestU32(base, device + offset) & 0xFF000000) | (count & 0xFFFFFF));
    StoreGuestU32(base, device + offset + 4, physical(address));
  };
  const auto aligned_bytes = [](uint32_t count) { return ((count + 7) << 2) & ~31u; };

  StoreGuestU32(base, device + 13756, ctx.r30.u32);
  ctx.r3.u64 = ctx.r31.u64;
  ctx.r4.u64 = ctx.r30.u64;
  ctx.r5.u64 = ctx.r28.u64;
  ctx.lr = 0x82444AC4;
  if (!native || !WriteInitializationSetup(ctx, base)) {
    native = false;
    original_setup(ctx, base);
  }
  const uint32_t setup_words = ctx.r3.u32;
  descriptor(13760, setup_words, ctx.r30.u32);
  ctx.r29.u64 = ctx.r31.u64 + 13896;
  ctx.r4.u64 = 16;
  ctx.r5.u64 = buffer + aligned_bytes(setup_words);
  for (uint32_t index = 16; index <= 112; ++index) {
    ctx.r3.u64 = ctx.r31.u64;
    ctx.r6.u64 = ctx.r28.u64;
    ctx.lr = 0x82444B0C;
    if (!native || !WriteInitializationGpr(ctx, base)) {
      native = false;
      original_gpr(ctx, base);
    }
    const uint32_t words = ctx.r3.u32;
    descriptor(13896 + (index - 16) * 8, words, ctx.r5.u32);
    ctx.r4.s64 += 1;
    ctx.r29.s64 += 8;
    ctx.r5.u64 += aligned_bytes(words);
  }
  // The complete owned 8 KiB span includes all 24 draws. An unusual heap
  // result keeps its compatibility continuation without allocating again.
  const uint32_t draw_header =
      native ? 0xC0000000u | (rex::graphics::native_gpu_calls::kBareDrawOpcode << 8) : 0xC0003600;
  for (uint32_t i = 0; i < 24; ++i) {
    StoreGuestU32(base, ctx.r5.u32 + i * 8, draw_header);
    StoreGuestU32(base, ctx.r5.u32 + i * 8 + 4, 0x10081);
  }
  descriptor(14792, 48, ctx.r5.u32);
  ctx.r3.u64 = 1;
  ctx.r1.s64 += 128;
  restore(ctx, base);
}

template <typename Allocate, typename Save, typename Restore, typename OriginalSetup,
          typename OriginalGpr>
bool InitializeGraphicsBuffers(PPCContext& ctx, uint8_t* base, Allocate allocate, Save save,
                               Restore restore, OriginalSetup original_setup,
                               OriginalGpr original_gpr) {
  if (!InitializationFrameReady(ctx, base))
    return false;
  ctx.r12.u64 = ctx.lr;
  ctx.lr = 0x82444A88;
  save(ctx, base);
  const uint32_t caller_sp = ctx.r1.u32;
  ctx.r1.u32 -= 128;
  StoreGuestU32(base, ctx.r1.u32, caller_sp);
  ctx.r31.u64 = ctx.r3.u64;
  ctx.r4.s64 = -1249902592;
  ctx.r3.u64 = 8192;
  ctx.lr = 0x82444A9C;
  allocate(ctx, base);
  FinishInitialization(ctx, base, restore, original_setup, original_gpr);
  return true;
}

}  // namespace simpsons::native_gpu_calls
