#include "native_ragdoll_joint.h"
#include "guest_memory.h"
#include "game_thread.h"
#include "guest_call_frame.h"
#include <rex/ppc/context.h>

#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <sys/mman.h>

REX_EXTERN(sub_82B20270);
REX_EXTERN(sub_82B1FF18);

namespace {
constexpr uint32_t joint = 0x2000, stack = 0xF000;
uint8_t* memory = nullptr;
int checks = 0, construct_calls = 0, validation_calls = 0;
bool reject_constructor = false, throw_validation = false;
void Require(bool ok, const char* message) {
  ++checks;
  if (!ok) throw std::runtime_error(message);
}
void Put(uint32_t at, uint32_t value) { simpsons::StoreGuestU32(memory, at, value); }
void Float(uint32_t at, float value) { simpsons::StoreGuestFloat(memory, at, value); }
entity_transform::Transform Identity() { return {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {0, 0, 0}}; }
}

namespace spawner {
bool Readable(uint32_t address, uint32_t bytes) {
  return address >= 0x1000 && uint64_t(address) + bytes <= 0x10000;
}
}

bool CallGuest(PPCContext& ctx, uint8_t* base, uint32_t address,
               std::initializer_list<uint32_t> args, uint32_t* result, double* float_result) {
  if (address == 0x82B20270 && reject_constructor) return false;
  return simpsons::InvokeGuestFrame(ctx, base, args, [&](PPCContext& call, uint8_t* memory) {
    if (address == 0x82B20270) {
      ++construct_calls;
      sub_82B20270(call, memory);
    } else if (address == 0x82B1FF18) {
      ++validation_calls;
      if (throw_validation) throw std::runtime_error("Interrupted native validator");
      sub_82B1FF18(call, memory);
    } else throw std::runtime_error("Unexpected native routine");
  }, result, float_result);
}

int main() {
  try {
    memory = static_cast<uint8_t*>(mmap(nullptr, 0x100000000ull, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0));
    if (memory == MAP_FAILED) throw std::runtime_error("Cannot reserve sparse guest memory");
    // Original scalar math constants used by the unmodified constructor and
    // validator. No game image or extracted asset is distributed by this test.
    for (const auto [at, value] : std::array<std::array<uint32_t, 2>, 13>{{
        {0x82000BB0, 0x3F800000}, {0x821DD0D8, 0}, {0x821A07F8, 0xBCCCCCCD},
        {0x821A07FC, 0x3F060AA6}, {0x821A0800, 0xBF060AA6}, {0x82181848, 0xC2C80000},
        {0x82152E30, 0x3CCCCCCD}, {0x821DD2A4, 0x3F4CCCCD}, {0x821DD32C, 0x3A83126F},
        {0x82061920, 0x40490FDB}, {0x821DD0E4, 0x40000000},
        {0x821DD1E4, 0x40800000}, {0x821DD23C, 0x41000000}}})
      Put(at, value);
    PPCContext ctx{};
    ctx.fpscr.csr = ctx.fpscr.getcsr();
    ctx.r1.u64 = stack; ctx.r3.u64 = 12345; ctx.f1.f64 = 3.25;
    auto a = Identity(), b = Identity();
    Require(native_ragdoll::InitializeJoint(ctx, memory, joint, a, b),
            "The original constructor creates a valid real ragdoll joint definition");
    Require(simpsons::LoadGuestU32(memory, joint) == 0x821A07D4 &&
            simpsons::LoadGuestU16(memory, joint + 16) == 2 &&
            simpsons::LoadGuestU16(memory, joint + 312) == 5,
            "Native vtable and constraint atoms are initialized by Havok");
    Require(ctx.r1.u64 == stack && ctx.r3.u64 == 12345 && ctx.f1.f64 == 3.25,
            "The adapter preserves the game caller's registers");
    Require(native_ragdoll::JointValid(ctx, memory, joint),
            "Native hidden-output hkBool ABI leaves the joint intact");

    std::array<uint8_t, native_ragdoll::kJointBytes> defaults;
    std::memcpy(defaults.data(), memory + joint, defaults.size());
    std::mt19937 limit_random(511);
    for (int i = 0; i < 1000; ++i) {
      native_ragdoll::JointLimits limits;
      limits.twist_min = -float(limit_random() % 3000) * .001f;
      limits.twist_max = float(limit_random() % 3000) * .001f;
      limits.cone_max = float(limit_random() % 3100) * .001f;
      limits.plane_min = -float(limit_random() % 1500) * .001f;
      limits.plane_max = float(limit_random() % 1500) * .001f;
      limits.tau = .1f + float(limit_random() % 900) * .001f;
      Require(native_ragdoll::InitializeJoint(ctx, memory, joint, a, b, &limits),
              "Custom limits pass the actual native joint validator");
      const std::array<std::pair<uint32_t, float>, 8> expected{{
          {260, limits.twist_min}, {264, limits.twist_max}, {268, limits.tau},
          {284, limits.cone_max}, {288, limits.tau}, {300, limits.plane_min},
          {304, limits.plane_max}, {308, limits.tau}}};
      for (const auto [at, value] : expected)
        Require(simpsons::LoadGuestFloat(memory, joint + at) == value,
                "Native angle/tau atoms retain the requested limit");
      bool rest_unchanged = true;
      for (size_t byte = 0; byte < defaults.size(); ++byte) {
        bool limit_byte = false;
        for (const auto [at, value] : expected)
          limit_byte |= byte >= at && byte < at + 4;
        if (!limit_byte) rest_unchanged &= memory[joint + byte] == defaults[byte];
      }
      Require(rest_unchanged,
              "Changing limits preserves frames, axis selectors, motors and cone sentinel");
    }
    Require(native_ragdoll::InitializeJoint(ctx, memory, joint, a, b),
            "An absent profile restores original constructor defaults");
    Require(std::memcmp(defaults.data(), memory + joint, defaults.size()) == 0,
            "Existing callers retain byte-identical native defaults");
    const auto reject_limits = [&](const native_ragdoll::JointLimits& bad) {
      const auto calls = construct_calls;
      Require(!native_ragdoll::ValidJointLimits(bad) &&
              !native_ragdoll::InitializeJoint(ctx, memory, joint, a, b, &bad),
              "Malformed profiles are rejected");
      Require(construct_calls == calls &&
              std::memcmp(defaults.data(), memory + joint, defaults.size()) == 0,
              "Malformed limits never reach native construction or mutate storage");
    };
    for (size_t field = 0; field < 6; ++field) {
      for (float value : {std::numeric_limits<float>::quiet_NaN(),
                          std::numeric_limits<float>::infinity()}) {
        native_ragdoll::JointLimits bad;
        float* fields[] = {&bad.twist_min, &bad.twist_max, &bad.cone_max,
                          &bad.plane_min, &bad.plane_max, &bad.tau};
        *fields[field] = value;
        reject_limits(bad);
      }
    }
    for (int invalid = 0; invalid < 9; ++invalid) {
      native_ragdoll::JointLimits bad;
      if (invalid == 0) bad.twist_min = 1;
      if (invalid == 1) bad.twist_min = -4;
      if (invalid == 2) bad.twist_max = 4;
      if (invalid == 3) bad.cone_max = -1;
      if (invalid == 4) bad.cone_max = 3.1415927f;
      if (invalid == 5) bad.plane_min = 1;
      if (invalid == 6) bad.plane_max = 2;
      if (invalid == 7) bad.tau = 0;
      if (invalid == 8) bad.tau = 1.01f;
      reject_limits(bad);
    }

    const auto host_csr = ctx.fpscr.getcsr();
    bool invoked = false;
    Require(!simpsons::InvokeGuestFrame(ctx, memory, {1,2,3,4,5,6,7,8,9},
        [&](PPCContext&, uint8_t*) { invoked = true; }) && !invoked,
        "Excess register arguments are rejected instead of silently truncated");
    auto bad_stack = ctx; bad_stack.r1.u64 = 256;
    Require(!simpsons::InvokeGuestFrame(bad_stack, memory, {},
        [&](PPCContext&, uint8_t*) { invoked = true; }) && !invoked,
        "A guest stack cannot underflow when adding the native call frame");
    bool failed = false;
    try {
      simpsons::InvokeGuestFrame(ctx, memory, {5}, [&](PPCContext& call, uint8_t*) {
        call.r3.u64 = 999; call.f1.f64 = -2;
        call.fpscr.setcsr(host_csr ^ call.fpscr.FlushMask);
        throw std::runtime_error("Interrupted guest routine");
      });
    } catch (const std::exception&) { failed = true; }
    Require(failed && ctx.r1.u64 == stack && ctx.r3.u64 == 12345 && ctx.f1.f64 == 3.25,
        "Interrupted guest calls restore all saved argument/stack registers");
    Require(ctx.fpscr.getcsr() == host_csr,
        "Interrupted guest calls restore the actual host floating-point mode");

    std::mt19937 random(170);
    for (int i = 0; i < 3000; ++i) {
      const float angle = float(random() % 62831) * 0.0001f;
      a = {{std::cos(angle), 0, -std::sin(angle)}, {0, 1, 0},
           {std::sin(angle), 0, std::cos(angle)}, {float(i) * 0.01f, -2, 3}};
      b = Identity(); b.position = {-2, float(i) * 0.001f, 7};
      Require(native_ragdoll::InitializeJoint(ctx, memory, joint, a, b),
              "Independent body-local rotations and pivots pass real native validation");
      Require(simpsons::LoadGuestFloat(memory, joint + 80) == a.position[0] &&
              simpsons::LoadGuestFloat(memory, joint + 148) == b.position[1],
              "Both native transform blocks retain their own joint pivots");
    }
    std::array<uint8_t, native_ragdoll::kJointBytes> before;
    std::memcpy(before.data(), memory + joint, before.size());
    const int calls = construct_calls;
    for (const auto value : {0.0f, 2.0f, -1.0f, std::numeric_limits<float>::infinity(),
                            std::numeric_limits<float>::quiet_NaN()}) {
      a = Identity(); a.right[0] = value;
      Require(!native_ragdoll::InitializeJoint(ctx, memory, joint, a, Identity()),
              "Scaled, reflected, degenerate and non-finite frames are rejected");
      Require(std::memcmp(before.data(), memory + joint, before.size()) == 0 && construct_calls == calls,
              "Bad input fails before constructor or guest-memory mutation");
    }
    Require(!native_ragdoll::InitializeJoint(ctx, memory, joint + 1, Identity(), Identity()),
            "Unaligned native data is rejected");
    Require(!native_ragdoll::InitializeJoint(ctx, memory, 0xFFFFFFF0, Identity(), Identity()),
            "Wrapping/unmapped storage is rejected");
    reject_constructor = true;
    Require(!native_ragdoll::InitializeJoint(ctx, memory, joint, Identity(), Identity()) &&
            std::memcmp(before.data(), memory + joint, before.size()) == 0,
            "A missing constructor rolls back unpublished storage");
    reject_constructor = false; throw_validation = true;
    bool interrupted = false;
    try { native_ragdoll::InitializeJoint(ctx, memory, joint, Identity(), Identity()); }
    catch (const std::exception&) { interrupted = true; }
    Require(interrupted && std::memcmp(before.data(), memory + joint, before.size()) == 0,
            "An interrupted native call rolls back unpublished storage");
    throw_validation = false;
    Float(joint + 260, 2); Float(joint + 264, -2);
    Require(!native_ragdoll::JointValid(ctx, memory, joint), "The real validator rejects inverted twist limits");
    std::cout << "native ragdoll joint definitions: " << checks << " checks passed; "
              << construct_calls << " constructor calls, " << validation_calls << " native validations\n"
              << "Scope: joint data only; world collision, skeleton driving and recovery remain unqualified.\n";
    munmap(memory, 0x100000000ull);
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
