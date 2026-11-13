// Run the original hard-keyframe calculation with the production timing hook.
// Motion setters capture its computed velocities; a fixed-step feedback loop
// checks that a collision body stays with its animated target at high FPS.
#include "simpsons_init.h"
#include "guest_memory.h"

#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <sys/mman.h>

namespace {
constexpr uint32_t kBody = 0x10000, kMotionTable = 0x20000;
constexpr uint32_t kPosition = 0x30000, kRotation = 0x30010, kWorld = 0x40000;
constexpr float kStep = std::bit_cast<float>(uint32_t{0x3C88AB86});
float simulation_step = kStep;
unsigned checks = 0;

void Require(bool value, const char* message) {
  if (!value)
    throw std::runtime_error(message);
}
void Vector(uint8_t* base, uint32_t at, std::array<float, 4> value) {
  for (size_t i = 0; i < value.size(); ++i)
    simpsons::StoreGuestFloat(base, at + uint32_t(i * 4), value[i]);
}
std::array<float, 4> Vector(const uint8_t* base, uint32_t at) {
  std::array<float, 4> value;
  for (size_t i = 0; i < value.size(); ++i)
    value[i] = simpsons::LoadGuestFloat(base, at + uint32_t(i * 4));
  return value;
}
void Linear(PPCContext& ctx, uint8_t* base) {
  Require(ctx.r3.u32 == kBody + 160, "Unexpected linear-motion receiver");
  Vector(base, kBody + 384, Vector(base, ctx.r4.u32));
}
void Angular(PPCContext& ctx, uint8_t* base) {
  Require(ctx.r3.u32 == kBody + 160, "Unexpected angular-motion receiver");
  Vector(base, kBody + 400, Vector(base, ctx.r4.u32));
}

struct Result {
  std::array<float, 4> linear, angular;
};
Result Run(uint8_t* base, PPCFunc* function, double inverse, bool detached = false) {
  simpsons::StoreGuestU32(base, kBody + 8, detached ? 0 : kWorld);
  PPCContext ctx{};
  ctx.fpscr.csr = ctx.fpscr.getcsr();
  ctx.r1.u32 = 0x800000;
  ctx.r3.u32 = kPosition;
  ctx.r4.u32 = kRotation;
  ctx.r6.u32 = kBody;
  ctx.f1.f64 = inverse;
  ctx.r29.u64 = 0x1234567890abcdef;
  ctx.r30.u64 = 0x1122334455667788;
  ctx.r31.u64 = 0x8877665544332211;
  ctx.f30.f64 = 123.25;
  ctx.f31.f64 = -17.5;
  function(ctx, base);
  Require(ctx.r1.u32 == 0x800000 && ctx.r29.u64 == 0x1234567890abcdef &&
              ctx.r30.u64 == 0x1122334455667788 && ctx.r31.u64 == 0x8877665544332211 &&
              ctx.f30.f64 == 123.25 && ctx.f31.f64 == -17.5,
          "Keyframe calculation did not restore its registers");
  return {Vector(base, kBody + 384), Vector(base, kBody + 400)};
}
bool Same(const Result& a, const Result& b) {
  return std::memcmp(&a, &b, sizeof(Result)) == 0;
}

double Feedback(uint8_t* base, PPCFunc* function, double fps, float step, bool moving) {
  simulation_step = step;
  std::array<float, 4> position{7.71216f, -4.06682f, -9.38248f, 0};
  const std::array<float, 4> origin{7.71216f, -2.25783f, -9.38248f, 1};
  Vector(base, kBody + 288, {0, 0, 0, 1});
  Vector(base, kBody + 304, {0, 0, 0, 0});
  Vector(base, kRotation, {0, 0, 0, 1});
  double marker = 0, simulated = 0, largest_error = 0;
  for (unsigned frame = 0; frame < 1200; ++frame) {
    marker += 1 / fps;
    auto target = origin;
    if (moving)
      target[1] += float(std::sin(marker * 2) * .4);
    Vector(base, kPosition, target);
    Vector(base, kBody + 224, position);
    Vector(base, kBody + 256, position);
    const auto result = Run(base, function, fps);
    while (simulated + 1e-10 < marker) {
      for (size_t i = 0; i < 3; ++i)
        position[i] += result.linear[i] * step;
      simulated += step;
    }
    const double error = std::abs(double(position[1]) - target[1]);
    Require(std::isfinite(error), "Keyframed collision position became nonfinite");
    if (function == sub_82A93FE0 && marker > 1) {
      Require(error < (moving ? .001 + 1.6 * step : .0001),
              "The collision body did not settle with its animated target");
    }
    largest_error = std::max(largest_error, error);
    if (error > 10000)
      break;
  }
  return largest_error;
}
}  // namespace

float HavokKeyframeStep(uint32_t world) {
  Require(world == 0 || world == kWorld, "Wrong keyframe world");
  return world ? simulation_step : 0;
}

namespace rex::runtime {
PPCFunc* ResolveIndirectFunction(uint32_t) {
  throw std::runtime_error("Unexpected indirect call in hard-keyframe calculation");
}
}  // namespace rex::runtime

REX_FUNC(sub_82AAD470) {
  Require(ctx.r3.u32 == kBody, "Wrong body activation request");
}
REX_FUNC(sub_82A428D8) {
  ctx.f1.f64 = std::acos(ctx.f1.f64);
}

int Check() {
  auto* base = static_cast<uint8_t*>(mmap(nullptr, size_t{1} << 32, PROT_READ | PROT_WRITE,
                                          MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0));
  Require(base != MAP_FAILED, "Could not reserve sparse guest memory");
  simpsons::StoreGuestU32(base, kBody + 160, kMotionTable);
  simpsons::StoreGuestU32(base, kMotionTable + 60, uint32_t(REX_CODE_BASE));
  simpsons::StoreGuestU32(base, kMotionTable + 64, uint32_t(REX_CODE_BASE) + 4);
  REX_LOOKUP_FUNC(base, REX_CODE_BASE) = Linear;
  REX_LOOKUP_FUNC(base, REX_CODE_BASE + 4) = Angular;
  simpsons::StoreGuestFloat(base, 0x82000BB0, 1);
  simpsons::StoreGuestFloat(base, 0x821DD0D8, 0);
  simpsons::StoreGuestFloat(base, 0x821DD0E4, 2);
  simpsons::StoreGuestFloat(base, 0x821DD32C, .001f);
  simpsons::StoreGuestFloat(base, 0x82061920, float(std::acos(-1)));
  Vector(base, 0x821A1120, {-.5f, -.5f, -.5f, -.5f});
  for (uint32_t i = 0; i < 4; ++i)
    simpsons::StoreGuestU32(base, 0x820B8150 + i * 4, 0x80000000);

  Vector(base, kPosition, {1, 2, 3, 1});
  Vector(base, kBody + 224, {-.1f, 1.7f, 3.2f, 0});
  Vector(base, kBody + 256, {-.1f, 1.7f, 3.2f, 0});
  Vector(base, kBody + 288, {0, 0, 0, 1});
  Vector(base, kBody + 304, {.2f, .3f, -.1f, 0});
  for (float step : {kStep, kStep * 2, .025f}) {
    simulation_step = step;
    for (double fps : {29.0, 29.97, 59.94, 90.0, 120.0, 165.0, 240.0, 1000.0}) {
      for (double angle : {0.0, -.5, .5, 1.5}) {
        Vector(base, kRotation, {0, float(std::sin(angle / 2)), 0, float(std::cos(angle / 2))});
        const auto expected = Run(base, __imp__sub_82A93FE0, std::min(fps, 1 / double(step)));
        const auto fixed = Run(base, sub_82A93FE0, fps);
        Require(Same(expected, fixed), "Keyframe correction changed the original pose calculation");
        const auto detached = Run(base, sub_82A93FE0, fps, true);
        Require(Same(detached, Run(base, __imp__sub_82A93FE0, fps, true)),
                "Detached-body timing changed");
        checks += 2;
      }
    }
  }
  Vector(base, kRotation, {0, 0, 0, 1});
  for (float step : {0.0f, -1.0f, std::numeric_limits<float>::quiet_NaN()}) {
    simulation_step = step;
    Require(Same(Run(base, sub_82A93FE0, 165), Run(base, __imp__sub_82A93FE0, 165)),
            "An unavailable simulation step changed native timing");
    ++checks;
  }
  simulation_step = kStep;
  for (double inverse : {0.0, -1.0, std::numeric_limits<double>::infinity(),
                         std::numeric_limits<double>::quiet_NaN()}) {
    Require(Same(Run(base, sub_82A93FE0, inverse), Run(base, __imp__sub_82A93FE0, inverse)),
            "Invalid native inverse duration was silently replaced");
    ++checks;
  }
  for (float step : {kStep, kStep * 2, .025f}) {
    for (double fps : {29.97, 59.94, 90.0, 120.0, 165.0, 240.0, 1000.0}) {
      for (bool moving : {false, true}) {
        const double error = Feedback(base, sub_82A93FE0, fps, step, moving);
        Require(error < 2.0, "The corrected collision body left its model");
        ++checks;
      }
    }
  }
  Require(Feedback(base, __imp__sub_82A93FE0, 165, kStep, false) > 10000,
          "The original collision-body divergence was not reproduced");
  ++checks;
  munmap(base, size_t{1} << 32);
  std::cout << "Physics keyframes: " << checks << " checks passed\n";
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
