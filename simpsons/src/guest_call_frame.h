#pragma once

#include "guest_memory.h"

#include <initializer_list>
#include <rex/ppc/context.h>

namespace simpsons {

// Native adapters may call routines that change flush/rounding mode or throw.
// Restore the actual host FP state as well as the guest's saved register copy.
class GuestContextScope {
 public:
  explicit GuestContextScope(PPCContext& context)
      : context_(context), saved_(context), host_csr_(context.fpscr.getcsr()) {}
  ~GuestContextScope() {
    context_ = saved_;
    context_.fpscr.setcsr(host_csr_);
  }
  GuestContextScope(const GuestContextScope&) = delete;
  GuestContextScope& operator=(const GuestContextScope&) = delete;

 private:
  PPCContext& context_;
  PPCContext saved_;
  uint32_t host_csr_;
};

template <typename Function>
bool InvokeGuestFrame(PPCContext& ctx, uint8_t* base, std::initializer_list<uint32_t> args,
                      Function function, uint32_t* result = nullptr, double* float_result = nullptr) {
  if (!base || args.size() > 8 || ctx.r1.u32 < 512 || (ctx.r1.u32 & 15)) return false;
  const auto caller_stack = ctx.r1.u32;
  GuestContextScope restore(ctx);
  ctx.r1.u64 -= 512;
  StoreGuestU32(base, ctx.r1.u32, caller_stack);
  PPCRegister* registers[] = {&ctx.r3, &ctx.r4, &ctx.r5, &ctx.r6,
                              &ctx.r7, &ctx.r8, &ctx.r9, &ctx.r10};
  size_t index = 0;
  for (const auto value : args) registers[index++]->u64 = value;
  function(ctx, base);
  if (result) *result = ctx.r3.u32;
  if (float_result) *float_result = ctx.f1.f64;
  return true;
}

}  // namespace simpsons
