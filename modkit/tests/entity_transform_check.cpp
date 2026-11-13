// Exercise the production adapter against unchanged game setters/getters.
// Controller/model dispatch, the square-root lookup and RenderWare's matrix backend are fixture seams;
// class cleanup and game rendering are qualified separately in the live game.
#include "../../simpsons/src/entity_transform.h"
#include "../../simpsons/src/game_thread.h"
#include "../../simpsons/src/guest_memory.h"
#include <rex/ppc/context.h>

#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>

REX_EXTERN(sub_823F2460);
REX_EXTERN(sub_823F24D8);
REX_EXTERN(sub_823F3B68);
REX_EXTERN(sub_82A3CD80);
REX_EXTERN(sub_82696D10);
REX_EXTERN(sub_82697690);
REX_EXTERN(sub_823EC410);
REX_EXTERN(sub_823EBF10);
REX_EXTERN(sub_823F2748);
REX_EXTERN(sub_823F41D8);
REX_EXTERN(sub_826A8708);
namespace {
constexpr uint32_t entity = 0x2000, table = 0x3000, frame = 0x4000, parent = 0x4500;
constexpr uint32_t root = 0x4800, controller = 0x5000, model = 0x5500, queue = 0x1000;
constexpr uint32_t base_setter = 0x82696D10, player_setter = 0x8288D2E0, changed = 0x82697160;
constexpr uint32_t base_transform = 0x82697690, player_transform = 0x8288D428;
constexpr uint32_t controller_move = 0x1234, controller_transform = 0x1238;
std::array<uint8_t, 32768> bytes{};
int checks = 0, setters = 0, model_notifications = 0, controller_notifications = 0;
uint32_t denied = 0;
using entity_transform::Transform;
using entity_transform::Vec3;
void Require(bool ok, const char* message) {
  ++checks;
  if (!ok) throw std::runtime_error(message);
}
uint32_t Read(const uint8_t* base, uint32_t at) {
  return at == 0x82D0CA68 ? queue : simpsons::LoadGuestU32(base, at);
}
uint64_t Read64(const uint8_t* base, uint32_t at) {
  uint64_t value;
  std::memcpy(&value, base + at, 8);
  return std::byteswap(value);
}
void Write64(uint8_t* base, uint32_t at, uint64_t value) {
  value = std::byteswap(value);
  std::memcpy(base + at, &value, 8);
}
void Put(uint32_t at, uint32_t value) { simpsons::StoreGuestU32(bytes.data(), at, value); }
void Vector(uint32_t at, const Vec3& value) {
  for (size_t i = 0; i < 3; ++i) simpsons::StoreGuestFloat(bytes.data(), at + uint32_t(i) * 4, value[i]);
}
Vec3 Vector(uint32_t at) {
  Vec3 value;
  for (size_t i = 0; i < 3; ++i) value[i] = simpsons::LoadGuestFloat(bytes.data(), at + uint32_t(i) * 4);
  return value;
}
Transform Identity(Vec3 position = {}) {
  return {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}, position};
}
void Matrix(uint32_t at, const Transform& matrix) {
  Vector(at, matrix.right); Vector(at + 16, matrix.up);
  Vector(at + 32, matrix.forward); Vector(at + 48, matrix.position);
}
Transform Matrix(uint32_t at) {
  return {Vector(at), Vector(at + 16), Vector(at + 32), Vector(at + 48)};
}
Vec3 Apply(const Transform& matrix, const Vec3& v, bool point = true) {
  Vec3 out;
  for (size_t axis = 0; axis < 3; ++axis)
    out[axis] = float(double(v[0]) * matrix.right[axis] + double(v[1]) * matrix.up[axis] +
        double(v[2]) * matrix.forward[axis] + (point ? matrix.position[axis] : 0));
  return out;
}
bool Near(const Vec3& a, const Vec3& b) {
  for (size_t i = 0; i < 3; ++i)
    if (!std::isfinite(a[i]) || std::abs(a[i] - b[i]) > 0.002f) return false;
  return true;
}
void Indirect(PPCContext& ctx, uint8_t* base, uint32_t address);
}  // namespace

#undef REX_LOAD_U32
#define REX_LOAD_U32(address) Read(base, uint32_t(address))
#define REX_LOAD_U8(address) base[uint32_t(address)]
#define REX_LOAD_U16(address) uint16_t(uint16_t(base[uint32_t(address)]) << 8 | base[uint32_t(address) + 1])
#define REX_STORE_U32(address, value) simpsons::StoreGuestU32(base, uint32_t(address), uint32_t(value))
#define REX_STORE_U16(address, value) do { const auto at = uint32_t(address); \
  base[at] = uint8_t(uint16_t(value) >> 8); base[at + 1] = uint8_t(value); } while (0)
#define REX_STORE_U8(address, value) (base[uint32_t(address)] = uint8_t(value))
#define REX_LOAD_U64(address) Read64(base, uint32_t(address))
#define REX_STORE_U64(address, value) Write64(base, uint32_t(address), uint64_t(value))
#undef REX_CALL_INDIRECT_FUNC
#define REX_CALL_INDIRECT_FUNC(address) Indirect(ctx, base, uint32_t(address))
#define REX_FUNC_PROLOGUE() ((void)0)
#include "native_transform.h"

REX_FUNC(sub_823F2460) { GameFrameMarkDirty(ctx, base); }
REX_FUNC(sub_823F24D8) { GameFrameUpdateWorld(ctx, base); }
REX_FUNC(sub_82696D10) { GameSetPosition(ctx, base); }
REX_FUNC(sub_82697690) { GameSetTransform(ctx, base); }
REX_FUNC(sub_823EC410) { GameVectorLength(ctx, base); }
// Production uses the game's TLS square-root lookup. This seam models its
// mathematical result; native cached-scale rounding is checked in the game.
REX_FUNC(sub_823EBF10) { ctx.f1.f64 = double(std::sqrt(float(ctx.f1.f64))); }
REX_FUNC(sub_823F2748) { GameFrameTransform(ctx, base); }
REX_FUNC(sub_823F41D8) {
  Require(ctx.r5.u32 == 0, "The full matrix setter replaces its local transform");
  std::memmove(base + ctx.r3.u32, base + ctx.r4.u32, 64);
}
REX_FUNC(sub_82A3CD80) {
  std::memmove(base + ctx.r3.u32, base + ctx.r4.u32, ctx.r5.u32);
}
REX_FUNC(sub_823F3B68) {
  const auto left = Matrix(ctx.r4.u32), right = Matrix(ctx.r5.u32);
  Matrix(ctx.r3.u32, {Apply(right, left.right, false), Apply(right, left.up, false),
      Apply(right, left.forward, false), Apply(right, left.position)});
}
REX_FUNC(sub_826A8708) {
  Require(ctx.r3.u32 == model && ctx.r4.u32 == 1, "The model receives its original transform notification");
  ++model_notifications;
}
namespace {
void Indirect(PPCContext& ctx, uint8_t* base, uint32_t address) {
  if (address == changed) GameTransformChanged(ctx, base);
  else if (address == controller_move) {
    Require(ctx.r3.u32 == controller && ctx.r5.u32 == 0,
        "The player setter uses its original controller placement operation");
    Vector(controller + 64, Vector(ctx.r4.u32));
    ++controller_notifications;
  } else if (address == controller_transform) {
    Require(ctx.r3.u32 == controller && ctx.r5.u32 == 0,
        "The derived full setter dispatches its original controller matrix operation");
    std::memmove(base + controller + 64, base + ctx.r4.u32, 64);
    ++controller_notifications;
  } else throw std::runtime_error("Unexpected class transform dispatch");
}
void Reset(bool player = false) {
  bytes.fill(0); denied = 0; setters = model_notifications = controller_notifications = 0;
  Put(entity, table); Put(entity + 136, model); Put(entity + 140, frame);
  Put(table + 28, player ? player_setter : base_setter); Put(table + 92, changed);
  Put(table + 40, player ? player_transform : base_transform);
  Put(entity + 784, controller); Put(controller, controller + 0x100);
  Put(controller + 0x100 + 60, controller_move);
  Put(controller + 0x100 + 56, controller_transform);
  Put(frame + 160, frame); Put(queue + 188, queue + 188);
  Matrix(frame + 16, Identity({1, 2, 3}));
  // Poison neighbouring words to expose the previous near-position scan.
  Vector(entity + 208, {1, 2, 3}); Vector(model + 128, {1, 2, 3});
}
PPCContext Context() { PPCContext ctx{}; ctx.r1.u64 = 0x7800; return ctx; }
}  // namespace

namespace spawner {
bool Readable(uint32_t address, uint32_t size) {
  return address >= 0x100 && uint64_t(address) + size <= bytes.size() &&
      (!denied || denied < address || denied >= uint64_t(address) + size);
}
}  // namespace spawner

bool CallGuest(PPCContext& ctx, uint8_t* base, uint32_t address,
               std::initializer_list<uint32_t> args, uint32_t* result, double*) {
  const PPCContext saved = ctx;
  ctx.r1.u64 -= 512;
  PPCRegister* regs[] = {&ctx.r3, &ctx.r4, &ctx.r5, &ctx.r6, &ctx.r7, &ctx.r8, &ctx.r9, &ctx.r10};
  size_t i = 0;
  for (auto arg : args) regs[i++]->u64 = arg;
  if (address == 0x823F2540) GameFrameGetWorld(ctx, base);
  else if (address == base_setter) { ++setters; GameSetPosition(ctx, base); }
  else if (address == player_setter) { ++setters; GamePlayerSetPosition(ctx, base); }
  else if (address == base_transform) { ++setters; GameSetTransform(ctx, base); }
  else if (address == player_transform) { ++setters; GamePlayerSetTransform(ctx, base); }
  else { ctx = saved; return false; }
  if (result) *result = ctx.r3.u32;
  ctx = saved;
  return true;
}

int main() {
  try {
    for (bool player : {false, true}) {
      Reset(player); auto ctx = Context();
      const auto basis = Matrix(frame + 16);
      Require(entity_transform::Move(ctx, bytes.data(), entity, {7, 8, 9}), "A live class setter accepts a new position");
      const auto transform = entity_transform::Get(ctx, bytes.data(), entity);
      Require(transform && transform->position == Vec3{7, 8, 9} && transform->right == basis.right &&
          transform->up == basis.up && transform->forward == basis.forward,
          "The original frame getter reports the new world position with its basis unchanged");
      Require(setters == 1 && model_notifications == 1 && (bytes[entity + 202] & 8),
          "Placement calls the class once and marks its model dirty through the game callback");
      Require(Vector(entity + 208) == Vec3{1, 2, 3} && Vector(model + 128) == Vec3{1, 2, 3},
          "Unrelated fields matching the old position are never touched");
      Require(controller_notifications == int(player) &&
          (!player || Vector(controller + 64) == Vec3{7, 8, 9}),
          "Derived player placement updates its controller rather than bypassing the override");
      Require(ctx.r1.u32 == 0x7800, "Transform calls preserve the caller's stack");

      Reset(player); ctx = Context();
      const Transform desired{{0, 0, -2}, {0, 3, 0}, {4, 0, 0}, {7, 8, 9}};
      Require(entity_transform::Set(ctx, bytes.data(), entity, desired),
          "Full placement uses the class matrix setter for orientation, scale and position");
      const auto full = entity_transform::Get(ctx, bytes.data(), entity);
      Require(full && full->right == desired.right && full->up == desired.up &&
          full->forward == desired.forward && full->position == desired.position,
          "The original frame getter observes the complete requested root transform");
      Require(Vector(entity + 144) == Vec3{2, 3, 4} && setters == 1 && model_notifications == 1 &&
          (bytes[entity + 202] & 8), "The native full setter updates cached scale and its model notification");
      Require(controller_notifications == int(player) && (!player ||
          (Matrix(controller + 64).right == desired.right && Matrix(controller + 64).position == desired.position)),
          "Derived full placement reaches the controller with the new matrix");
      Require(ctx.r1.u32 == 0x7800 && Vector(entity + 208) == Vec3{1, 2, 3},
          "Full placement preserves caller registers and unrelated object fields");
    }

    std::mt19937 random(0x20071030);
    std::uniform_real_distribution<float> number(-10, 10), scale(0.25f, 4.0f);
    for (size_t i = 0; i < 1000; ++i) {
      Reset(); auto ctx = Context();
      const float angle = number(random), s = std::sin(angle), c = std::cos(angle);
      const float x = scale(random) * (i % 2 ? -1 : 1), y = scale(random), z = scale(random);
      // Known invertible triangular shear followed by rotation; reflection
      // and nonuniform scale deliberately disallow an orthonormal shortcut.
      Transform p{{c * x, 0, -s * x}, {c * 0.2f, y, -s * 0.2f},
          {s * z, 0.3f, c * z}, {number(random), number(random), number(random)}};
      Matrix(parent + 16, p); Put(parent + 160, parent);
      Put(frame + 160, parent); Put(frame + 4, parent);
      Vec3 desired{number(random), number(random), number(random)};
      const auto expected = Apply(p, desired);
      const auto basis = Matrix(frame + 16);
      Require(entity_transform::Move(ctx, bytes.data(), entity, expected), "Affine parents accept world-space placement");
      Require(Near(Vector(frame + 64), desired), "Placement recovers local position under rotation, reflection, scale and shear");
      const auto actual = entity_transform::Get(ctx, bytes.data(), entity);
      Require(actual && Near(actual->position, expected), "Original parent traversal and frame composition return the authored world position");
      Require(Matrix(frame + 16).right == basis.right && setters == 1,
          "Parent conversion preserves the entity basis and calls one setter");

      const Transform local{{2, 0.15f, 0}, {0, 0.75f, 0.1f}, {0, 0, i % 2 ? -1.5f : 1.5f}, desired};
      const Transform world{Apply(p, local.right, false), Apply(p, local.up, false),
          Apply(p, local.forward, false), Apply(p, local.position)};
      Require(entity_transform::Set(ctx, bytes.data(), entity, world),
          "Affine parents accept a complete world-space transform");
      const auto authored = entity_transform::Get(ctx, bytes.data(), entity);
      const auto resulting_local = Matrix(frame + 16);
      Require(authored && Near(authored->right, world.right) && Near(authored->up, world.up) &&
          Near(authored->forward, world.forward) && Near(authored->position, world.position),
          "World orientation, reflection, shear, scale and position round-trip through the original frame getter");
      Require(Near(resulting_local.right, local.right) && Near(resulting_local.up, local.up) &&
          Near(resulting_local.forward, local.forward) && Near(resulting_local.position, local.position),
          "All basis vectors use inverse parent directions without subtracting the parent's position");
    }

    // A second parent exercises the original recursive world update, rather
    // than accidentally treating the immediate parent's local pose as world.
    Reset(); auto ctx = Context();
    Put(frame + 160, root); Put(frame + 4, parent);
    Put(parent + 160, root); Put(parent + 4, root); Put(root + 160, root);
    Matrix(root + 16, {{0, 0, -2}, {0, 3, 0}, {4, 0, 0}, {11, 12, 13}});
    Matrix(parent + 16, Identity({4, 5, 6})); bytes[root + 3] = 1;
    Require(entity_transform::Move(ctx, bytes.data(), entity, {42, 43, 44}), "Nested parent chains accept world coordinates");
    Require(Near(entity_transform::Get(ctx, bytes.data(), entity)->position, {42, 43, 44}),
        "Nested parent world matrices are synchronized before conversion");

    for (float invalid : {std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(),
                           std::numeric_limits<float>::quiet_NaN()}) {
      Reset(); auto ctx = Context(); const auto before = bytes;
      Require(!entity_transform::Move(ctx, bytes.data(), entity, {1, invalid, 3}) && bytes == before && !setters,
          "Nonfinite coordinates fail without any game memory mutation");
    }
    for (uint32_t missing : {entity, table + 28, frame + 160}) {
      Reset(); denied = missing; auto ctx = Context();
      Require(!entity_transform::Move(ctx, bytes.data(), entity, {7, 8, 9}) && !setters,
          "Unreadable objects, tables and frames fail before class dispatch");
    }
    Reset(); Put(frame + 160, parent); Put(frame + 4, frame); ctx = Context();
    Require(!entity_transform::Get(ctx, bytes.data(), entity) &&
        !entity_transform::Move(ctx, bytes.data(), entity, {7, 8, 9}) && !setters,
        "Cyclic frame hierarchies are rejected before recursive game calls");
    Reset(); Put(frame + 160, parent); Put(frame + 4, parent); Put(parent + 160, root); ctx = Context();
    Require(!entity_transform::Move(ctx, bytes.data(), entity, {7, 8, 9}) && !setters,
        "Inconsistent frame roots cannot enter the native traversal");
    Reset(); Put(frame + 160, parent); Put(frame + 4, parent); Put(parent + 160, parent);
    Matrix(parent + 16, {{1, 0, 0}, {1, 0, 0}, {0, 0, 1}, {2, 3, 4}}); ctx = Context();
    Require(!entity_transform::Move(ctx, bytes.data(), entity, {7, 8, 9}) && !setters &&
        Vector(frame + 64) == Vec3{1, 2, 3}, "Singular parents reject placement without running the setter");
    Reset(); Put(table + 28, 0); ctx = Context();
    Require(!entity_transform::Move(ctx, bytes.data(), entity, {7, 8, 9}) && !setters,
        "A missing class setter has no raw-write fallback");

    for (float invalid : {0.0f, 1e-30f, 1e30f, std::numeric_limits<float>::infinity(),
                           std::numeric_limits<float>::quiet_NaN()}) {
      Reset(); ctx = Context(); const auto before = bytes;
      auto invalid_transform = Identity(); invalid_transform.right[0] = invalid;
      Require(!entity_transform::Set(ctx, bytes.data(), entity, invalid_transform) && bytes == before && !setters,
          "Zero, nonfinite and native-length overflow/underflow bases reject without changing guest memory");
    }
    Reset(); ctx = Context(); auto invalid_transform = Identity(); invalid_transform.up = invalid_transform.right;
    const auto before = bytes;
    Require(!entity_transform::Set(ctx, bytes.data(), entity, invalid_transform) && bytes == before && !setters,
        "Dependent basis vectors reject before native dispatch");
    for (uint32_t missing : {entity + 144, table + 40, frame + 160, 0x7300u}) {
      Reset(); ctx = Context(); denied = missing;
      Require(!entity_transform::Set(ctx, bytes.data(), entity, Identity()) && !setters,
          "Cached scale, method, hierarchy and scratch-stack mappings are validated before dispatch");
    }
    Reset(); ctx = Context(); ctx.r1.u32 = 0x100;
    Require(!entity_transform::Set(ctx, bytes.data(), entity, Identity()) && !setters,
        "A low stack cannot wrap the private argument frame into guest memory");
    Reset(); Put(table + 40, 0); ctx = Context();
    Require(!entity_transform::Set(ctx, bytes.data(), entity, Identity()) && !setters,
        "An unavailable full setter never falls back to writing the frame directly");

    std::cout << checks << " original transform and adapter checks passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n'; return 1;
  }
}
