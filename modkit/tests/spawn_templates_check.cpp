#include "../../simpsons/src/guest_memory.h"
#include "../../simpsons/src/spawn_templates.h"
#include <rex/ppc/context.h>

#include <iostream>
#include <random>
#include <stdexcept>

REX_EXTERN(sub_8269B028);
REX_EXTERN(sub_82745390);
REX_EXTERN(sub_82745BF8);

namespace {
std::array<uint8_t, 131072> bytes{};
int checks = 0;
uint32_t denied = 0;
void Require(bool value, const char* description) {
  ++checks;
  if (!value)
    throw std::runtime_error(description);
}
bool Readable(uint32_t at, uint32_t size) {
  return uint64_t(at) + size <= bytes.size() &&
         (!denied || !(at <= denied && uint64_t(at) + size > denied));
}
uint32_t Word(uint32_t at) {
  if (!Readable(at, 4))
    throw std::runtime_error("Unexpected fixture read");
  return simpsons::LoadGuestU32(bytes.data(), at);
}
uint8_t Byte(uint32_t at) {
  if (!Readable(at, 1))
    throw std::runtime_error("Unexpected fixture byte read");
  return bytes[at];
}
void Put(uint32_t at, uint32_t value) {
  simpsons::StoreGuestU32(bytes.data(), at, value);
}
uint64_t Read64(uint32_t at) {
  return uint64_t(Word(at)) << 32 | Word(at + 4);
}
void Write64(uint32_t at, uint64_t value) {
  Put(at, uint32_t(value >> 32));
  Put(at + 4, uint32_t(value));
}
void ppc_trap(PPCContext&, uint8_t*, uint32_t) {
  throw std::runtime_error("Unexpected native lookup trap");
}
}  // namespace
#undef REX_LOAD_U32
#define REX_LOAD_U32(address) Word(uint32_t(address))
#define REX_LOAD_U8(address) Byte(uint32_t(address))
#define REX_LOAD_U64(address) Read64(uint32_t(address))
#define REX_STORE_U32(address, value) Put(uint32_t(address), uint32_t(value))
#define REX_STORE_U64(address, value) Write64(uint32_t(address), uint64_t(value))
#define REX_FUNC_PROLOGUE() ((void)0)
#include "native_spawn.h"
REX_FUNC(sub_8269B028) {
  GameDefinitionGuid(ctx, base);
}
REX_FUNC(sub_82745390) {
  GameGuidHash(ctx, base);
}
REX_FUNC(sub_82745BF8) {
  throw std::runtime_error("Unreviewed null GUID branch");
}

int main() {
  try {
    constexpr uint32_t world = 0x2000, buckets = 0x3000, count = 67;
    const spawn_templates::Memory memory{Readable, Word, Byte};
    uint32_t region_queries = 0;
    const auto cached = spawn_templates::ReadOnlyPages({
        [&](uint32_t at, uint32_t size) { ++region_queries; return Readable(at, size); }, Word, Byte});
    for (uint32_t i = 0; i < 1000; ++i)
      Require(cached.Has(0x3000 + i * 4, 4), "A read-only batch retains committed page guards");
    Require(region_queries == 1, "Repeated fields use one full-page mapping query");
    denied = 0x5008;
    const auto partial = spawn_templates::ReadOnlyPages(memory);
    Require(partial.Has(0x5000, 4) && !partial.Has(0x5008, 4) && !partial.Has(0x5004, 8),
            "A partial page never expands readable bounds around an unmapped field");
    Require(!partial.Has(0xfffffff0, 32), "Cached views still reject address wrap");
    denied = 0;
    Put(world + 72, buckets);
    Put(world + 76, count);
    std::mt19937 random(0x722);
    std::vector<spawn_templates::Definition> expected;
    PPCContext context{};
    for (uint32_t i = 0; i < 128; ++i) {
      const uint32_t at = 0x5000 + i * 48, guid_at = 0x8000 + i * 16;
      spawn_templates::Definition record{
          at,
          uint32_t(random()),
          {uint32_t(random()), uint32_t(random()), uint32_t(random()), uint32_t(random())}};
      // Half the definitions refer backwards, as the installed game does.
      const auto target = i & 1 ? 0x4000 + i * 16 : guid_at;
      for (uint32_t j = 0; j < 4; ++j)
        Put(target + j * 4, record.guid[j]);
      bytes[at + 16] = 1;
      Put(at + 24, target - (at + 24));
      Put(at + 28, record.hash);
      const auto bucket = spawn_templates::Hash(record.guid) % count;
      Put(at + 12, Word(buckets + bucket * 4));
      Put(buckets + bucket * 4, at);
      expected.push_back(record);
    }
    const auto all = spawn_templates::List(memory, world);
    Require(all.size() == expected.size(), "All resident definitions are enumerated once");
    for (const auto& record : expected) {
      const auto actual = spawn_templates::Find(memory, world, record.guid);
      Require(actual && actual->data == record.data && actual->hash == record.hash,
              "The guarded reader preserves the exact definition identity");
      for (uint32_t j = 0; j < 4; ++j)
        Put(0x1800 + j * 4, record.guid[j]);
      context.r1.u64 = 0x1000;
      context.r3.u64 = world + 72;
      context.r4.u64 = 0x1800;
      GameFindDefinition(context, bytes.data());
      Require(context.r3.u32 == actual->data,
              "Definition selection agrees with the original lookup");
      context.r3.u64 = 0x1800;
      context.r4.u64 = 16;
      GameGuidHash(context, bytes.data());
      Require(context.r3.u32 == spawn_templates::Hash(record.guid),
              "All GUID bytes agree with the unchanged native hash");
    }
    Require(!spawn_templates::Find(memory, world, {1, 2, 3, 4}), "Unknown GUIDs have no template");
    const auto first = expected.front();
    Put(first.data + 12, first.data);
    Require(spawn_templates::List(memory, world).empty(), "A cycle rejects the resident snapshot");
    Require(!spawn_templates::Find(memory, world, {1, 2, 3, 4}),
            "A bounded search cannot loop forever");
    Put(first.data + 24, 0x7fffffff);
    Require(!spawn_templates::Read(memory, first.data), "Unmapped relative GUIDs are rejected");
    Put(first.data + 24, 0);
    Require(!spawn_templates::Read(memory, first.data),
            "Null GUID offsets cannot lend an identity");
    Put(world + 76, UINT32_MAX);
    Require(spawn_templates::List(memory, world).empty(), "Corrupt bucket counts cannot overflow");
    Require(!spawn_templates::Read(memory, 0xfffffff0), "Address wrap cannot access host memory");

    bytes.fill(0);
    constexpr uint32_t manager = 0xa000, file = 0xd000;
    Put(manager + 8704, file);
    Put(file + 96, 42);
    Put(file + 100, 4);
    Put(file + 108, 0xabcdef);
    Require(spawn_templates::Owners(memory, manager).at(0xabcdef) == 42,
            "Templates retain the current native stream-load key");
    Put(file + 96, 43);
    Require(spawn_templates::Owners(memory, manager).at(0xabcdef) != 42,
            "Reloading at the same address expires the previous source load");
    for (uint32_t state : {0u, 1u, 2u, 3u, 5u, 6u}) {
      Put(file + 100, state);
      Require(spawn_templates::Owners(memory, manager).empty(),
              "Pending, failed and unloading streams cannot lend templates");
    }
    Put(file + 100, 4);
    Put(file, file);
    Require(spawn_templates::Owners(memory, manager).empty(), "A stream-list cycle fails closed");
    Put(file, 0);
    denied = file + 108;
    Require(spawn_templates::Owners(memory, manager).empty(),
            "Unmapped owner metadata is rejected");
    denied = 0;
    Put(manager + 8708, file);
    Put(file + 108, 0);
    const auto files = spawn_templates::Files(memory, manager);
    Require(files.size() == 1 && files.front().key == 43 && files.front().state == 4,
            "File identities retain their unique load keys even with namespace zero");
    Put(file + 4, file);
    Require(spawn_templates::Files(memory, manager).empty(), "Broken file backlinks fail closed");
    Put(file + 4, 0);
    Put(manager + 8708, 0);
    Require(spawn_templates::Files(memory, manager).empty(), "A broken file tail is not retirement");

    constexpr uint32_t layer_a = 0x4000, layer_b = 0x4100, a = 0x5000, b = 0x5100, c = 0x5200;
    Put(world + 56, layer_a);
    Put(world + 60, layer_b);
    Put(layer_a, layer_b);
    Put(layer_a + 8, 42);
    Put(layer_a + 16, a);
    Put(layer_b + 4, layer_a);
    Put(layer_b + 8, 43);
    Put(layer_b + 16, c);
    Put(a, layer_a + 16);
    Put(a + 4, b);
    Put(b, a + 4);
    Put(c, layer_b + 16);
    const auto owners = spawn_templates::DefinitionOwners(memory, world);
    Require(owners.size() == 3 && owners.at(a) == 42 && owners.at(b) == 42 && owners.at(c) == 43,
            "Exact layer membership distinguishes records without relying on GUID prefixes");
    Put(b, layer_b + 16);
    Require(spawn_templates::DefinitionOwners(memory, world).empty(),
            "A definition linked to the wrong layer is rejected");
    Put(b, a + 4);
    Put(b + 4, a);
    Require(spawn_templates::DefinitionOwners(memory, world).empty(),
            "A definition cycle cannot lend an owner");
    Put(b + 4, 0);
    Put(layer_b + 4, 0);
    Require(spawn_templates::DefinitionOwners(memory, world).empty(),
            "Corrupt layer backlinks reject the ownership snapshot");
    std::cout << "{\"ok\":true,\"checks\":" << checks << "}\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
