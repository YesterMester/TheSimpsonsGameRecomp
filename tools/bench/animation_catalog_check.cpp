#include "spawner.h"
#include "game_thread.h"
#include "guest_memory.h"
#include <rex/ppc/context.h>
#include <rex/logging.h>

#include <algorithm>
#include <cstring>
#include <iostream>
#include <map>
#include <random>
#include <set>
#include <stdexcept>
#include <sys/mman.h>

REX_EXTERN(sub_827451C0);
REX_EXTERN(sub_8278AD00);
REX_EXTERN(sub_8278B478);

// The original table lookup logs an impossible zero-divisor condition.
// Treat that path as a fixture failure without linking the entire runtime.
namespace rex {
LogCategoryId RegisterLogCategory(const char*) { return LogCategoryId{0}; }
spdlog::logger* GetLoggerRaw(LogCategoryId) {
  throw std::runtime_error("Unexpected native lookup diagnostic");
}
}  // namespace rex

namespace {
constexpr uint32_t region = 0x83000000, region_bytes = 0x800000;
constexpr uint32_t entity = region, player = region + 0x1000;
constexpr uint32_t bank = region + 0x2000, table = region + 0x4000;
constexpr uint32_t buckets = region + 0x8000, nodes = region + 0x10000;
constexpr uint32_t clip_data = region + 0x40000, names = region + 0x80000;
constexpr uint32_t scratch = region + region_bytes - 0x1000;
uint8_t* memory = nullptr;
uint32_t denied_begin = 0, denied_end = 0;
unsigned checks = 0, native_queries = 0;

void Require(bool ok, const char* message) {
  ++checks;
  if (!ok) throw std::runtime_error(message);
}
void Put(uint32_t at, uint32_t value) { simpsons::StoreGuestU32(memory, at, value); }
uint32_t Get(uint32_t at) { return simpsons::LoadGuestU32(memory, at); }
uint32_t NativeHash(const std::string& name) {
  Require(name.size() < 512, "Fixture hash input fits scratch storage");
  std::memcpy(memory + scratch, name.c_str(), name.size() + 1);
  PPCContext ctx{};
  ctx.r3.u64 = scratch;
  sub_827451C0(ctx, memory);
  return ctx.r3.u32;
}
uint32_t NativeLookup(uint32_t key, bool through_bank) {
  PPCContext ctx{};
  ctx.r1.u64 = scratch + 0x800;
  ctx.r3.u64 = through_bank ? bank : table;
  Put(scratch + 0x400, key);
  ctx.r4.u64 = through_bank ? key : scratch + 0x400;
  ++native_queries;
  if (through_bank) sub_8278B478(ctx, memory);
  else sub_8278AD00(ctx, memory);
  return ctx.r3.u32;
}
void Reset(uint32_t count) {
  std::memset(memory + region, 0, region_bytes);
  denied_begin = denied_end = 0;
  Put(entity + 236, bank);
  Put(entity + 248, player);
  Put(player + 4, bank);
  Put(bank + 48, table);
  Put(table, buckets);
  Put(table + 4, count);
}
void Add(uint32_t index, uint32_t key, const std::string& label) {
  const auto count = Get(table + 4), bucket = key % count;
  const auto node = nodes + index * 16, clip = clip_data + index * 16;
  const auto name = names + index * 256;
  Require(label.size() < 256, "Fixture label fits its name slot");
  std::memcpy(memory + name, label.c_str(), label.size() + 1);
  Put(node, key); Put(node + 4, name); Put(node + 8, clip);
  Put(node + 12, Get(buckets + bucket * 4));
  Put(buckets + bucket * 4, node);
}
void Reject(const char* message) {
  Require(spawner::ListEntityAnimations(memory, entity).empty(), message);
}
void CheckCatalog(const std::map<uint32_t, std::string>& expected) {
  const auto before = std::vector<uint8_t>(memory + region, memory + scratch);
  const auto clips = spawner::ListEntityAnimations(memory, entity);
  Require(clips.size() == expected.size(), "All native entries are enumerated exactly once");
  Require(!std::memcmp(before.data(), memory + region, before.size()),
          "Catalog query does not mutate native bank or entity storage");
  uint32_t previous = 0;
  for (size_t i = 0; i < clips.size(); ++i) {
    const auto& clip = clips[i];
    Require(i == 0 || previous < clip.key, "Results have deterministic key order");
    previous = clip.key;
    Require(expected.at(clip.key) == clip.label, "Authored name belongs to this hash");
    Require(clip.clip_key == clip.key, "Native key is retained for playback");
    Require(NativeLookup(clip.key, true) == clip.clip_ptr,
            "Clip pointer exactly matches the original bank getter");
    const auto record = NativeLookup(clip.key, false);
    Require(record && Get(record + 4) == clip.clip_ptr,
            "Original bucket lookup returns the same name/clip record");
  }
}
}  // namespace

namespace spawner {
bool Readable(uint32_t address, uint32_t bytes) {
  return bytes && address >= region && uint64_t(address) + bytes <= uint64_t(region) + region_bytes &&
      (!denied_end || uint64_t(address) + bytes <= denied_begin || address >= denied_end);
}
}  // namespace spawner

int main() {
  try {
    memory = static_cast<uint8_t*>(mmap(nullptr, 0x100000000ull, PROT_NONE,
        MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0));
    if (memory == MAP_FAILED || mprotect(memory + region, region_bytes, PROT_READ | PROT_WRITE))
      throw std::runtime_error("Cannot reserve guarded guest memory");
    Reset(7);
    const std::vector<std::string> recovery{
        "damage_knockdown_front_ground_getup", "damage_knockdown_back_ground_getup",
        "damage_heavy_front_getup", "Idle", "RUN", "a"};
    std::map<uint32_t, std::string> expected;
    for (size_t i = 0; i < recovery.size(); ++i) {
      const auto key = NativeHash(recovery[i]);
      Require(scores::Hash(recovery[i]) == key, "Host hash matches original native hash");
      Add(i, key, recovery[i]); expected[key] = recovery[i];
    }
    CheckCatalog(expected);
    Require(NativeHash(recovery[0]) == 0x2B30F028 && NativeHash(recovery[1]) == 0xE1E4E558 &&
            NativeHash(recovery[2]) == 0x1841FD13, "Authored recovery hashes remain stable");
    Put(entity + 248, 0);
    CheckCatalog(expected);  // Native character-bank fallback when no player exists.
    Put(entity + 248, player);
    Require(spawner::ListEntityAnimations(nullptr, entity).empty(), "Null base is rejected");
    Require(spawner::ListEntityAnimations(memory, 0xFFFFFFF0).empty(), "Wrapped entity is rejected");

    std::mt19937 random(0x8278AD00);
    for (unsigned pass = 0; pass < 128; ++pass) {
      const uint32_t counts[] = {1, 3, 7, 31, 64, 257, 4096};
      Reset(counts[pass % 7]); expected.clear();
      const auto n = 1 + random() % 127;
      while (expected.size() < n) {
        auto label = "Action_" + std::to_string(random()) + "_" + std::to_string(pass);
        const auto key = NativeHash(label);
        Require(scores::Hash(label) == key, "Random name hash matches original code");
        if (expected.contains(key)) continue;
        Add(expected.size(), key, label); expected[key] = label;
      }
      CheckCatalog(expected);
      uint32_t missing;
      do { missing = random(); } while (expected.contains(missing));
      Require(!NativeLookup(missing, false) && !NativeLookup(missing, true),
              "Missing keys match original lookup semantics");
    }
    auto One = [] { Reset(7); Add(0, scores::Hash("idle"), "idle"); };
    One(); Put(nodes + 12, nodes); Reject("Cycles return no partial catalog");
    One(); Add(1, scores::Hash("idle"), "idle"); Reject("Duplicate keys reject the whole catalog");
    One(); Put(nodes, Get(nodes) + 1); Reject("Keys in the wrong bucket are rejected");
    One(); Put(nodes + 8, clip_data + 1); Reject("Misaligned clip pointers are rejected");
    One(); Put(nodes + 12, 0x82000000); Reject("Unreadable next nodes return no partial catalog");
    One(); denied_begin = nodes + 8; denied_end = nodes + 16;
    Reject("Partially readable nodes are rejected");
    One(); denied_begin = clip_data; denied_end = clip_data + 16;
    Reject("Unreadable native clips are rejected");
    One(); Put(table + 4, 0); Reject("Zero bucket count never reaches native division");
    One(); Put(table + 4, 4097); Reject("Oversized bucket tables are bounded");
    One(); Put(table, region + region_bytes - 4); Reject("Truncated bucket arrays are rejected");
    One(); Put(bank + 48, table + 1); Reject("Misaligned table pointers are rejected");
    One(); Put(player + 4, 0x82000000); Reject("Unreadable selected bank is not silently substituted");

    for (unsigned bad = 0; bad < 7; ++bad) {
      One();
      if (bad == 0) Put(nodes + 4, 0);
      if (bad == 1) Put(nodes + 4, 0xFFFFFFFE);
      if (bad == 2) std::memcpy(memory + names, "walk", 5);
      if (bad == 3) std::memset(memory + names, 'a', 256);
      if (bad == 4) memory[names] = 0xFF;
      if (bad == 5) memory[names] = 0;
      if (bad == 6) { Put(nodes + 4, region + region_bytes - 1); memory[region + region_bytes - 1] = 'x'; }
      const auto clips = spawner::ListEntityAnimations(memory, entity);
      Require(clips.size() == 1 && clips[0].label == "Clip (46F5C7B4)" &&
              clips[0].clip_ptr == NativeLookup(scores::Hash("idle"), true),
              "Malformed or mismatched names use a safe label without changing the clip");
    }
    Reset(1); expected.clear();
    for (uint32_t i = 0; i < 4096; ++i) {
      Add(i, i, ""); expected[i] = "";
    }
    const auto maximum = spawner::ListEntityAnimations(memory, entity);
    Require(maximum.size() == 4096, "Maximum supported collision chain is complete");
    Add(4096, 4096, ""); Reject("Excess nodes reject the whole catalog within its budget");
    munmap(memory, 0x100000000ull);
    std::cout << "PASS: " << checks << " animation catalog assertions; " << native_queries
              << " original native lookups\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL after " << checks << " assertions: " << error.what() << '\n';
    return 1;
  }
}
