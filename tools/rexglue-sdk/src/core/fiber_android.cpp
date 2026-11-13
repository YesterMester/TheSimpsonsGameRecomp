#include <rex/thread/fiber.h>

#include <cassert>
#include <cstdlib>

extern "C" void rex_android_fiber_switch(void* from, const void* to);

namespace rex::thread {

thread_local Fiber* Fiber::tls_current_ = nullptr;

Fiber* Fiber::ConvertCurrentThread() {
  auto* fiber = new Fiber();
  fiber->is_thread_fiber_ = true;
  tls_current_ = fiber;
  return fiber;
}

Fiber* Fiber::Create(size_t stack_size, void (*entry)(void*), void* arg) {
  static_assert(sizeof(Context) == 192 && offsetof(Context, sp) == 96 &&
                offsetof(Context, fpcr) == 104 && offsetof(Context, fpsr) == 112 &&
                offsetof(Context, d) == 128);
  auto* fiber = new Fiber();
  fiber->stack_.resize(stack_size);
  fiber->entry_ = entry;
  fiber->arg_ = arg;
  fiber->context_.sp =
      reinterpret_cast<uintptr_t>(fiber->stack_.data() + fiber->stack_.size()) & ~uintptr_t(15);
  fiber->context_.x[11] = reinterpret_cast<uintptr_t>(&Fiber::Trampoline);
  asm volatile("mrs %0, fpcr" : "=r"(fiber->context_.fpcr));
  asm volatile("mrs %0, fpsr" : "=r"(fiber->context_.fpsr));
  return fiber;
}

void Fiber::Trampoline() {
  Fiber* fiber = tls_current_;
  fiber->entry_(fiber->arg_);
  // A guest fiber must switch or exit explicitly; it has no caller to return to.
  std::abort();
}

void Fiber::SwitchTo(Fiber* target) {
  Fiber* from = tls_current_;
  assert(from && target);
  tls_current_ = target;
  rex_android_fiber_switch(&from->context_, &target->context_);
}

void Fiber::Destroy() {
  if (is_thread_fiber_) {
    tls_current_ = nullptr;
  } else {
    assert(this != tls_current_);
  }
  delete this;
}

}  // namespace rex::thread
