// Execute the original world routines against bounded synthetic objects.
// Dispatch stubs record calls; no guessed class destructor replaces the game.
#include "../../simpsons/src/guest_memory.h"
#include <rex/ppc/context.h>

#include <array>
#include <iostream>
#include <stdexcept>

REX_EXTERN(sub_8268F538);
REX_EXTERN(sub_826A1008);
REX_EXTERN(sub_826A2AE8);
REX_EXTERN(sub_826A1108);
REX_EXTERN(sub_8269B738);
REX_EXTERN(sub_826A2258);
REX_EXTERN(sub_826A3190);
REX_EXTERN(sub_82745390);
REX_EXTERN(__savegprlr_28);
REX_EXTERN(__restgprlr_28);
namespace {
constexpr uint32_t world = 0x2000, entity = 0x3000, table = 0x4000;
constexpr uint32_t type = 0x5000, guid = 0x6000, header = 0x6100, buckets = 0x7000;
int checks = 0, dispatches = 0, unregisters = 0, direct_inserts = 0;
uint32_t target = 0, argument = 0, flags = 0;
uint32_t Read(const uint8_t* base, uint32_t at) {
  return at == 0x82D5729C || at == 0x82D57448 ? 0 : simpsons::LoadGuestU32(base, at);
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
void Require(bool ok, const char* message) {
  ++checks;
  if (!ok) throw std::runtime_error(message);
}
void Indirect(PPCContext& ctx, uint8_t*, uint32_t address) {
  ++dispatches; target = address; argument = ctx.r3.u32; flags = ctx.r4.u32;
}
void ppc_trap(PPCContext&, uint8_t*, uint32_t) {
  throw std::runtime_error("The original routine trapped on invalid fixture input");
}
}  // namespace
#undef REX_LOAD_U32
#define REX_LOAD_U32(address) Read(base, uint32_t(address))
#define REX_LOAD_U8(address) base[uint32_t(address)]
#define REX_STORE_U32(address, value) simpsons::StoreGuestU32(base, uint32_t(address), uint32_t(value))
#define REX_LOAD_U64(address) Read64(base, uint32_t(address))
#define REX_STORE_U64(address, value) Write64(base, uint32_t(address), uint64_t(value))
#undef REX_CALL_INDIRECT_FUNC
#define REX_CALL_INDIRECT_FUNC(address) Indirect(ctx, base, uint32_t(address))
#define REX_FUNC_PROLOGUE() ((void)0)
#include "native_entity.h"

REX_FUNC(sub_8268F538) { GameDestroySubscriber(ctx, base); }
REX_FUNC(sub_826A1008) {
  Require(ctx.r3.u32 == world + 88 && ctx.r4.u32 == entity,
      "Unregister removes the exact entity from the direct GUID index");
  ++unregisters; ctx.r3.u64 = 0;
}
REX_FUNC(__savegprlr_28) {}
REX_FUNC(__restgprlr_28) {}
REX_FUNC(sub_826A2258) {
  Require(ctx.r3.u32 == world + 32 && Read(base, ctx.r4.u32) == 42,
      "Registration selects the source layer's entity list");
  ctx.r3.u64 = type;
}
REX_FUNC(sub_826A3190) { throw std::runtime_error("Existing layer must not be recreated"); }
REX_FUNC(sub_826A1108) { throw std::runtime_error("Direct registration must not borrow a streaming GUID node"); }
REX_FUNC(sub_8269B738) { throw std::runtime_error("Direct registration must not mutate a streaming GUID chain"); }
REX_FUNC(sub_826A2AE8) {
  Require(ctx.r3.u32 == world + 88 && ctx.r4.u32 == entity &&
      Read(base, entity + 28) == 0x534D4F44 && Read(base, entity + 40) == 123,
      "The unique GUID is present before direct world indexing");
  ++direct_inserts;
}
REX_FUNC(sub_82745390) {
  Require(ctx.r4.u32 == 16, "GUID lookups hash all sixteen bytes");
  ctx.r3.u64 = 2;
}

int main() {
  try {
    std::array<uint8_t, 32768> bytes{};
    auto* base = bytes.data();
    const auto put = [&](uint32_t at, uint32_t value) { simpsons::StoreGuestU32(base, at, value); };
    PPCContext ctx{};
    ctx.r1.u64 = 0x1800;
    put(entity, table); put(table, 0x1110);
    put(entity + 56, table + 16); put(table + 16, 0x2220); put(table + 20, 0x3330);
    for (uint32_t value : {0u, 1u, 0x40000000u, 0xF0000000u}) {
      put(entity + 20, value); ctx.r3.u64 = entity; dispatches = 0;
      GameDestroyEntity(ctx, base);
      Require(dispatches == 1 && target == 0x1110 && argument == entity && flags == 1,
          "Normal objects dispatch the complete class deleting destructor");
    }
    put(entity + 20, 0x08000000); ctx.r3.u64 = entity; dispatches = 0;
    GameDestroyEntity(ctx, base);
    Require(dispatches == 2 && target == 0x2220 && argument == entity + 56 && flags == 1,
        "Subscriber-owned objects receive their destruction message and deleting destructor");
    ctx.r3.u64 = 0; dispatches = 0;
    GameDestroyEntity(ctx, base);
    Require(dispatches == 0, "The original removal helper accepts a null object");

    for (uint32_t guid_next : {0u, 0x3200u}) for (uint32_t type_next : {0u, 0x3400u}) {
      bytes.fill(0);
      put(world + 164, 10);
      put(entity + 12, 0x5100); put(entity + 16, guid_next);
      put(0x5100, entity);
      if (guid_next) put(guid_next + 12, entity + 16);
      put(entity + 4, type + 12); put(entity + 8, type_next);
      put(type + 12, entity);
      if (type_next) put(type_next + 4, entity + 8);
      ctx.r1.u64 = 0x1800; ctx.r3.u64 = world; ctx.r4.u64 = entity;
      GameUnregisterEntity(ctx, base);
      Require(Read(base, world + 164) == 9 && Read(base, 0x5100) == guid_next &&
          Read(base, type + 12) == type_next,
          "World counts and both list heads are updated by the original unregister");
      Require(!Read(base, entity + 4) && !Read(base, entity + 8) &&
          !Read(base, entity + 12) && !Read(base, entity + 16),
          "All of the removed entity's intrusive links are cleared");
      Require((!guid_next || Read(base, guid_next + 12) == 0x5100) &&
          (!type_next || Read(base, type_next + 4) == type + 12),
          "Successors point back to the surviving list head");
    }
    Require(unregisters == 4, "Each complete removal unregisters exactly once");

    bytes.fill(0);
    put(guid, 0x534D4F44); put(guid + 4, 2); put(guid + 8, 3); put(guid + 12, 123);
    base[header + 16] = 4;
    // Some constructors set the unregister flag before the loader links them.
    // Registration is still required; this flag alone is not world membership.
    put(entity + 20, 0x28000000);
    put(world + 164, 10); put(type + 12, 0x3400);
    ctx.r1.u64 = 0x1800; ctx.r3.u64 = world; ctx.r4.u64 = guid;
    ctx.r5.u64 = header; ctx.r6.u64 = entity; ctx.r7.u64 = 42;
    GameRegisterEntity(ctx, base);
    Require(direct_inserts == 1 && Read(base, world + 164) == 11 &&
        (Read(base, entity + 20) & 0x20000000) && Read(base, entity + 24) == 42,
        "Direct registration marks a real world entity in the original source layer");
    Require(Read(base, type + 12) == entity && Read(base, entity + 4) == type + 12 &&
        Read(base, entity + 8) == 0x3400 && Read(base, 0x3404) == entity + 8,
        "A spawned object joins the normal layer teardown list");
    Require(!Read(base, entity + 12) && !Read(base, entity + 16),
        "A direct entity does not alias the source template's GUID node");

    put(world + 88, buckets); put(world + 92, 4); put(buckets + 8, entity);
    ctx.r3.u64 = world + 88; ctx.r4.u64 = guid; GameFindDirectEntity(ctx, base);
    Require(ctx.r3.u32 == entity, "The original GUID lookup finds the registered unique entity");
    put(guid + 12, 124);
    ctx.r3.u64 = world + 88; ctx.r4.u64 = guid; GameFindDirectEntity(ctx, base);
    Require(!ctx.r3.u32, "Hash collisions still compare the full GUID");
    std::cout << checks << " original entity routine checks passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n'; return 1;
  }
}
