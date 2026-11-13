#include "native_ragdoll_body.h"
#include "guest_memory.h"
#include "game_thread.h"
#include "guest_call_frame.h"

#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <sys/mman.h>

REX_EXTERN(sub_82AFB470);
REX_EXTERN(sub_82AB0978);
REX_EXTERN(sub_827A4D28);
REX_EXTERN(sub_827A4A90);
REX_EXTERN(sub_827A4A48);
REX_EXTERN(sub_827A4D50);

namespace {
constexpr uint32_t shape = 0x2000, config = 0x3000, stack = 0xF000;
uint8_t* memory = nullptr;
int checks = 0, constructor_calls = 0;
uint32_t reject = 0, interrupt = 0;
void Require(bool ok, const char* message) {
  ++checks;
  if (!ok)
    throw std::runtime_error(message);
}
float Float(uint32_t at) {
  return simpsons::LoadGuestFloat(memory, at);
}
uint32_t Word(uint32_t at) {
  return simpsons::LoadGuestU32(memory, at);
}
void Put(uint32_t at, uint32_t value) {
  simpsons::StoreGuestU32(memory, at, value);
}

// Independent disk integration of the capsule volume. Disk polar/transverse
// moments are integrated in Y, then normalized to the requested total mass.
std::array<double, 3> IntegratedInertia(const native_ragdoll::CapsuleBodyInfo& info) {
  constexpr int slices = 12000;
  const double radius = info.radius, half = info.half_length;
  const double step = 2 * (half + radius) / slices;
  double volume = 0, axial = 0, transverse = 0;
  for (int i = 0; i <= slices; ++i) {
    const double y = -half - radius + step * i;
    const double cap = std::max(0.0, std::abs(y) - half);
    const double disk = std::max(0.0, radius * radius - cap * cap);
    const double weight = (i == 0 || i == slices) ? 1 : (i & 1) ? 4 : 2;
    volume += weight * disk;
    axial += weight * disk * disk / 2;
    transverse += weight * disk * (disk / 4 + y * y);
  }
  return {transverse * info.mass / volume, axial * info.mass / volume,
          transverse * info.mass / volume};
}
}  // namespace

namespace spawner {
uint32_t Address(uint64_t) { return 0; }
bool Readable(uint32_t address, uint32_t bytes) {
  return address >= 0x1000 && uint64_t(address) + bytes <= 0x10000;
}
}  // namespace spawner

bool CallGuest(PPCContext& ctx, uint8_t* base, uint32_t address,
               std::initializer_list<uint32_t> args, uint32_t* result, double* float_result) {
  if (address == reject)
    return false;
  return simpsons::InvokeGuestFrame(
      ctx, base, args,
      [&](PPCContext& call, uint8_t* base) {
        ++constructor_calls;
        if (address == interrupt)
          throw std::runtime_error("Interrupted native constructor");
        if (address == 0x82AFB470)
          sub_82AFB470(call, base);
        else if (address == 0x82AB0978)
          sub_82AB0978(call, base);
        else if (address == 0x827A4D28)
          sub_827A4D28(call, base);
        else if (address == 0x827A4A90)
          sub_827A4A90(call, base);
        else if (address == 0x827A4A48)
          sub_827A4A48(call, base);
        else if (address == 0x827A4D50)
          sub_827A4D50(call, base);
        else
          throw std::runtime_error("Unexpected native routine");
      },
      result, float_result);
}

int main() {
  try {
    memory = static_cast<uint8_t*>(mmap(nullptr, 0x100000000ull, PROT_READ | PROT_WRITE,
                                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0));
    if (memory == MAP_FAILED)
      throw std::runtime_error("Cannot reserve sparse guest memory");
    // Original scalar/vector constants loaded by the game's Cinfo constructor.
    for (const auto [at, value] :
         std::array<std::array<uint32_t, 2>, 11>{{{0x821A10F0, 0},
                                                  {0x821A10F4, 0},
                                                  {0x821A10F8, 0},
                                                  {0x821A10FC, 0x3F800000},
                                                  {0x821DD0D8, 0},
                                                  {0x8200611C, 0x3ECCCCCD},
                                                  {0x82000BB0, 0x3F800000},
                                                  {0x821DD3D0, 0x3D4CCCCD},
                                                  {0x82189870, 0x43480000},
                                                  {0x82000FB8, 0x3F000000},
                                                  {0x821DD110, 0xBF800000}}})
      Put(at, value);
    PPCContext ctx{};
    ctx.fpscr.csr = ctx.fpscr.getcsr();
    ctx.r1.u64 = stack;
    ctx.r3.u64 = 12345;
    ctx.f1.f64 = 3.25;
    native_ragdoll::CapsuleBodyInfo info;
    Require(native_ragdoll::InitializeCapsuleBodyInfo(ctx, memory, shape, config, info),
            "The original constructors build a capsule and native rigid-body configuration");
    Require(Word(shape) == 0x8219F0A4 && simpsons::LoadGuestU16(memory, shape + 4) == 0 &&
                simpsons::LoadGuestU16(memory, shape + 6) == 1,
            "The native capsule is embedded with its correct vtable and reference count");
    Require(Float(shape + 12) == info.radius && Float(shape + 20) == -info.half_length &&
                Float(shape + 36) == info.half_length && Float(shape + 28) == info.radius &&
                Float(shape + 44) == info.radius,
            "The native constructor receives aligned endpoints and the floating-point radius ABI");
    Require(Word(config + 4) == shape && memory[config + 176] == 1 && memory[config + 177] == 2 &&
                Float(config + 156) == 0.5f && Float(config + 160) == 0.4f &&
                Float(config + 152) == 0.05f &&
                simpsons::LoadGuestU16(memory, config + 10) == 0xFFFF,
            "Native motion, deactivation, friction, restitution and filter defaults are preserved");
    Require(ctx.r1.u64 == stack && ctx.r3.u64 == 12345 && ctx.f1.f64 == 3.25,
            "Native constructor calls preserve their game caller's context");

    std::mt19937 random(503);
    for (int sample = 0; sample < 180; ++sample) {
      info.radius = 0.01f + float(random() % 1000) * 0.001f;
      info.half_length = sample % 6 ? info.radius * float(random() % 2000) * 0.01f : 0;
      info.mass = 0.01f + float(random() % 10000) * 0.01f;
      info.position = {float(sample), -3, 4};
      info.linear_velocity = {3, -1, 2};
      info.angular_velocity = {-1, 2, 1};
      const float angle = float(random() % 62831) * 0.0001f;
      info.rotation = {0, 0, std::sin(angle / 2), std::cos(angle / 2)};
      const auto expected = IntegratedInertia(info);
      Require(native_ragdoll::InitializeCapsuleBodyInfo(ctx, memory, shape, config, info),
              "Valid sphere/capsule sizes, poses and velocities construct successfully");
      for (uint32_t column = 0; column < 3; ++column) {
        for (uint32_t row = 0; row < 4; ++row) {
          const double got = Float(config + 80 + column * 16 + row * 4);
          Require(
              row == column ? std::abs(got - expected[column]) < expected[column] * 2e-5 : got == 0,
              "Capsule inertia agrees with independent volume integration");
        }
        Require(Float(config + 16 + column * 4) == info.position[column] &&
                    Float(config + 48 + column * 4) == info.linear_velocity[column] &&
                    Float(config + 64 + column * 4) == info.angular_velocity[column] &&
                    Float(config + 128 + column * 4) == 0,
                "World pose/velocities are distinct from the centered local inertia tensor");
      }
      Require(Float(config + 144) == info.mass && Float(config + 44) == info.rotation[3],
              "Mass and quaternion retain their native field layout");
    }

    std::array<uint8_t, native_ragdoll::kCapsuleBytes> old_shape;
    std::array<uint8_t, native_ragdoll::kBodyInfoBytes> old_config;
    std::memcpy(old_shape.data(), memory + shape, old_shape.size());
    std::memcpy(old_config.data(), memory + config, old_config.size());
    const auto unchanged = [&] {
      return std::memcmp(old_shape.data(), memory + shape, old_shape.size()) == 0 &&
             std::memcmp(old_config.data(), memory + config, old_config.size()) == 0;
    };
    const auto valid = info;
    const int calls = constructor_calls;
    for (float value :
         {0.0f, -1.0f, std::numeric_limits<float>::infinity(),
          std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::denorm_min()}) {
      for (int field = 0; field < 3; ++field) {
        info = valid;
        if (field == 0)
          info.radius = value;
        if (field == 1)
          info.mass = value;
        if (field == 2)
          info.rotation[3] = value;
        Require(!native_ragdoll::InitializeCapsuleBodyInfo(ctx, memory, shape, config, info) &&
                    unchanged() && constructor_calls == calls,
                "Invalid size, mass and rotation fail before any constructor/mutation");
      }
    }
    info = valid;
    info.half_length = -0.1f;
    Require(
        !native_ragdoll::InitializeCapsuleBodyInfo(ctx, memory, shape, config, info) && unchanged(),
        "Negative capsule half-lengths cannot construct inverted shapes");
    info = valid;
    info.half_length = 1e20f;
    info.mass = 1e-20f;
    Require(
        !native_ragdoll::InitializeCapsuleBodyInfo(ctx, memory, shape, config, info) && unchanged(),
        "Finite parameters cannot overflow the native float32 AABB diagonal");
    info = valid;
    info.radius = 1e-20f;
    info.half_length = 0;
    info.mass = 1e37f;
    Require(
        !native_ragdoll::InitializeCapsuleBodyInfo(ctx, memory, shape, config, info) && unchanged(),
        "Finite mass cannot disguise collision dimensions that flush to zero");
    info = valid;
    info.position[1] = std::numeric_limits<float>::infinity();
    Require(
        !native_ragdoll::InitializeCapsuleBodyInfo(ctx, memory, shape, config, info) && unchanged(),
        "Non-finite poses are rejected");
    info = valid;
    for (const auto [a, b] : std::array<std::array<uint32_t, 2>, 6>{{{shape + 1, config},
                                                                     {shape, config + 1},
                                                                     {shape, shape},
                                                                     {shape, shape + 16},
                                                                     {0xFFFFFFF0, config},
                                                                     {shape, stack - 0x100}}})
      Require(!native_ragdoll::InitializeCapsuleBodyInfo(ctx, memory, a, b, info) && unchanged(),
              "Misaligned, overlapping, wrapping and active-stack storage are rejected");
    for (const auto address : {0x82AFB470u, 0x82AB0978u}) {
      reject = address;
      Require(!native_ragdoll::InitializeCapsuleBodyInfo(ctx, memory, shape, config, info) &&
                  unchanged(),
              "Unavailable native constructors restore both unpublished regions");
      reject = 0;
      interrupt = address;
      bool caught = false;
      try {
        native_ragdoll::InitializeCapsuleBodyInfo(ctx, memory, shape, config, info);
      } catch (const std::runtime_error&) {
        caught = true;
      }
      Require(caught && unchanged() && ctx.r1.u64 == stack && ctx.r3.u64 == 12345,
              "Interrupted native constructors restore definitions and caller context");
      interrupt = 0;
    }
    // Execute the game's filter predicate, not stock Havok's similarly named
    // group filter. Its high bits tag pointers to native game filter records.
    constexpr uint32_t filter_a = 0x6000, filter_b = 0x6020, result = 0x6100;
    constexpr uint32_t filter_object = 0x7000, exclusions = 0x6200;
    for (uint32_t layer = 0; layer < 32; ++layer)
      Put(filter_object + 28 + layer * 4, UINT32_MAX);
    const auto make_filter = [&](uint32_t at, uint32_t layer, uint32_t group,
                                 uint32_t mask = UINT32_MAX, uint32_t exclude = 0) {
      Require(CallGuest(ctx, memory, 0x827A4D28,
                        {at, layer, group, mask, UINT32_MAX, exclude ? exclusions : 0,
                         exclude ? 1u : 0u}), "Native game filter constructor exists");
      if (exclude) Put(exclusions, exclude);
      uint32_t token = 0;
      Require(CallGuest(ctx, memory, 0x827A4A90, {at}, &token) &&
                  token == (0x40000000u | (at >> 2)),
              "Native game pointer tag matches the body adapter");
      return token;
    };
    const auto collides = [&](uint32_t a, uint32_t b) {
      memory[result] = 0xCC;
      Require(CallGuest(ctx, memory, 0x827A4D50, {result, filter_object, a, b}) &&
                  memory[result] <= 1,
              "Native pair predicate returns hkBool through its hidden pointer");
      return memory[result] != 0;
    };
    for (uint32_t group = 1; group <= 4095; ++group) {
      const auto a = make_filter(filter_a, 0, group);
      const auto same = make_filter(filter_b, 0, group);
      Require(!collides(a, same) && !collides(same, a),
              "All native actor groups suppress self/adjacent limb collisions symmetrically");
      const auto other = make_filter(filter_b, 0, group == 4095 ? 1 : group + 1);
      Require(collides(a, other) && collides(other, a),
              "Different actors retain collisions throughout the native group range");
      uint32_t compact = 0;
      Require(CallGuest(ctx, memory, 0x827A4A48, {0, 1, group}, &compact) &&
                  !collides(a, compact) && !collides(compact, a),
              "Owned filters suppress contacts with the source actor's native compact filter");
    }
    const auto owned = make_filter(filter_a, 0, 123);
    for (uint32_t layer = 0; layer < 32; ++layer) {
      const auto scenery = make_filter(filter_b, layer, 0);
      Require(collides(owned, scenery) && collides(scenery, owned),
              "The owned actor filter preserves ungrouped scenery on every native layer");
      if (layer) {
        const auto limited = make_filter(filter_b, layer, 0, UINT32_MAX & ~1u);
        // The engine always includes layer zero. Test a nonzero opposing layer
        // separately to avoid asserting a stock-Havok layer-zero convention.
        Require(collides(owned, limited), "Native layer zero remains unconditionally allowed");
        const auto nonzero = make_filter(filter_a, layer, 123);
        const auto masked = make_filter(filter_b, 0, 0, UINT32_MAX & ~(1u << layer));
        Require(!collides(nonzero, masked) && !collides(masked, nonzero),
                "Other objects' native layer masks remain effective in both directions");
        make_filter(filter_a, 0, 123);
      }
    }
    const auto excluded = make_filter(filter_b, 0, 456, UINT32_MAX, 123);
    Require(!collides(owned, excluded) && !collides(excluded, owned),
            "Other objects' native actor exclusions are preserved");
    Require(collides(owned, 0) && collides(0, owned),
            "The game's unfiltered collision case remains enabled");

    info = valid;
    info.collision_filter = owned;
    Require(native_ragdoll::InitializeCapsuleBodyInfo(ctx, memory, shape, config, info) &&
                Word(config) == owned,
            "The body receives the actual native tagged collision token");
    std::memcpy(old_shape.data(), memory + shape, old_shape.size());
    std::memcpy(old_config.data(), memory + config, old_config.size());
    for (uint32_t token : {1u, 0x80001800u, 0xC0001800u, 0x40000000u, 0x40003FFFu}) {
      info.collision_filter = token;
      const auto before_calls = constructor_calls;
      Require(!native_ragdoll::InitializeCapsuleBodyInfo(ctx, memory, shape, config, info) &&
                  unchanged() && before_calls == constructor_calls,
              "Foreign, wrapping and unsupported filter tokens fail without mutation");
    }
    std::cout << "native ragdoll capsule/body configuration and game collision filters: "
              << checks << " checks passed; "
              << constructor_calls << " native constructor calls\n"
              << "Scope: collision/body definitions; world simulation and skeleton driving remain "
                 "unqualified.\n";
    munmap(memory, 0x100000000ull);
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
