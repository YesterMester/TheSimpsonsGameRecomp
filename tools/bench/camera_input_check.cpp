// Exercise the actual recompiled camera handlers. The test supplies controller
// axes and records the rotation request; camera filtering and comparisons run
// through the generated game code and the production input hooks.
#include "simpsons_init.h"
#include "guest_memory.h"

#include <bit>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <sys/mman.h>

namespace {
constexpr uint32_t kCamera = 0x20000;
constexpr uint32_t kActor = 0x10000;
float axis_x, axis_y, turn_x, turn_y;
uint32_t port, calls;
bool throw_input;

void Require(bool value, const char* message) {
  if (!value)
    throw std::runtime_error(message);
}
void Port(PPCContext& ctx, uint8_t*) {
  ctx.r3.u64 = port;
}
}  // namespace

namespace rex::runtime {
PPCFunc* ResolveIndirectFunction(uint32_t) {
  throw std::runtime_error("Unexpected indirect call in camera input");
}
}  // namespace rex::runtime

REX_FUNC(sub_826BBCF8) {
  Require(ctx.r4.u32 == port && ctx.r5.u32 == 1, "Wrong controller or stick");
  if (throw_input)
    throw std::runtime_error("Controller read interrupted");
  simpsons::StoreGuestFloat(base, ctx.r6.u32, axis_x);
  simpsons::StoreGuestFloat(base, ctx.r7.u32, axis_y);
}
REX_FUNC(sub_82A1AA38) {
  ctx.r3.u64 = 0;
}
REX_FUNC(sub_82A1AAA8) {
  ctx.r3.u64 = 0;
}
REX_FUNC(sub_82A2A480) {
  turn_y = simpsons::LoadGuestFloat(base, ctx.r4.u32);
  turn_x = simpsons::LoadGuestFloat(base, ctx.r5.u32);
  ++calls;
  ctx.r3.u64 = 0;
}
REX_FUNC(sub_82960328) {
  turn_y = float(ctx.f1.f64);
  turn_x = float(ctx.f2.f64);
  ++calls;
}

namespace {
struct Result {
  uint32_t calls;
  float x, y;
};
Result Run(uint8_t* base, PPCFunc* function, bool alternate, float factor, float x, float y) {
  constexpr uint32_t kCutoff[] = {0x82CF8EDC, 0x82CF8F80};
  std::memset(base + kCamera, 0, 512);
  simpsons::StoreGuestU32(base, kCamera + (alternate ? 112 : 268), kActor);
  simpsons::StoreGuestFloat(base, kCamera + 72, factor);
  simpsons::StoreGuestFloat(base, kCamera + (alternate ? 100 : 120), .06f);
  simpsons::StoreGuestFloat(base, kCamera + (alternate ? 96 : 124), .03f);
  for (auto address : kCutoff)
    simpsons::StoreGuestFloat(base, address, .02f);
  calls = 0;
  turn_x = turn_y = 0;
  axis_x = x;
  axis_y = y;
  PPCContext ctx{};
  ctx.r1.u32 = 0x800000;
  ctx.r3.u32 = kCamera;
  ctx.r29.u64 = 0x1234567890abcdef;
  function(ctx, base);
  Require(ctx.r1.u32 == 0x800000 && ctx.r29.u64 == 0x1234567890abcdef,
          "Camera handler did not restore its registers");
  for (auto address : kCutoff)
    Require(simpsons::LoadGuestU32(base, address) == std::bit_cast<uint32_t>(.02f),
            "Camera cutoff leaked into another input update");
  return {calls, turn_x, turn_y};
}
}  // namespace

int Check() {
  auto* base = static_cast<uint8_t*>(mmap(nullptr, size_t{1} << 32, PROT_READ | PROT_WRITE,
                                          MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0));
  Require(base != MAP_FAILED, "Could not reserve sparse guest memory");
  simpsons::StoreGuestU32(base, kActor + 2032, 0x4000);
  simpsons::StoreGuestU32(base, 0x4008, uint32_t(REX_CODE_BASE));
  REX_LOOKUP_FUNC(base, REX_CODE_BASE) = Port;
  simpsons::StoreGuestFloat(base, 0x821DD110, -1.0f);
  simpsons::StoreGuestFloat(base, 0x821DD0D8, 0.0f);
  unsigned checks = 0;
  for (bool alternate : {false, true}) {
    auto* original = alternate ? __imp__sub_82960870 : __imp__sub_8295C9C0;
    auto* fixed = alternate ? sub_82960870 : sub_8295C9C0;
    for (port = 0; port != 2; ++port) {
      for (float factor : {1.0f, 2.0f, 4.0f}) {
        for (float axis : {-.8f, -.5f, 0.0f, .5f, .8f, 1.0f}) {
          const auto before = Run(base, original, alternate, factor, axis, axis);
          const auto after = Run(base, fixed, alternate, factor, axis, axis);
          Require(before.calls == after.calls && before.x == after.x && before.y == after.y,
                  "Input changed at 60 FPS or below");
          ++checks;
        }
      }
      // At these short frame factors the shipped code rejects a fully held
      // vertical stick. Keep the requested rotation proportional to time.
      for (float fps : {90.0f, 120.0f, 144.0f, 165.0f, 180.0f, 240.0f, 1000.0f}) {
        const float factor = 60.0f / fps;
        for (float axis : {-.8f, .8f, -1.0f, 1.0f}) {
          const auto after = Run(base, fixed, alternate, factor, 0, axis);
          Require(after.calls == 1 && std::abs(after.y) > 0,
                  "Vertical camera input disappeared on a short frame");
          Require(std::abs(std::abs(after.y) - std::abs(axis * .03f * factor)) < 1e-7f,
                  "Camera input changed the requested turn speed");
          ++checks;
        }
      }
      const auto before = Run(base, original, alternate, .25f, 0, 1);
      Require(before.calls == 0, "The original high-FPS failure was not reproduced");
      Require(Run(base, fixed, alternate, .25f, .1f, .1f).calls == 0,
              "Small stick drift should remain filtered");
      checks += 2;
      throw_input = true;
      bool interrupted = false;
      try {
        Run(base, fixed, alternate, .25f, 0, 1);
      } catch (const std::runtime_error& error) {
        if (std::string_view(error.what()) != "Controller read interrupted")
          throw;
        interrupted = true;
      }
      throw_input = false;
      Require(interrupted, "Interrupted controller read did not propagate");
      Require(simpsons::LoadGuestU32(base, alternate ? 0x82CF8F80 : 0x82CF8EDC) ==
                  std::bit_cast<uint32_t>(.02f),
              "Interrupted input did not restore the cutoff");
      ++checks;
    }
  }
  munmap(base, size_t{1} << 32);
  std::cout << "Camera input: " << checks << " checks passed\n";
  return 0;
}

int main() {
  try {
    return Check();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
