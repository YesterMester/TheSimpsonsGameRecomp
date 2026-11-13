#include "../src/compiled_objects.h"
#include "../../simpsons/src/guest_memory.h"
#include <rex/ppc/context.h>

#include <iostream>
#include <random>
#include <stdexcept>

namespace {
std::array<uint8_t, 65536> bytes{};
uint32_t limit = bytes.size();
int checks = 0;
void Require(bool value, const char* description) {
  ++checks;
  if (!value) throw std::runtime_error(description);
}
bool Has(uint32_t at, uint32_t size) { return uint64_t(at) + size <= limit; }
uint8_t Byte(uint32_t at) {
  if (!Has(at, 1)) throw std::runtime_error("Unexpected original byte read");
  return bytes[at];
}
uint32_t Word(uint32_t at) {
  if (!Has(at, 4)) throw std::runtime_error("Unexpected original word read");
  return simpsons::LoadGuestU32(bytes.data(), at);
}
void Put(uint32_t at, uint32_t value) { simpsons::StoreGuestU32(bytes.data(), at, value); }
void Half(uint32_t at, uint16_t value) { bytes[at] = value >> 8; bytes[at + 1] = value; }
struct Memory {
  // The live native reader validates aligned regions even when individual
  // fields are bytes. Disk and guest views must accept the same valid group.
  bool Has(uint32_t at, uint32_t size) const { return !(at & 3) && ::Has(at, size); }
  uint8_t byte(uint32_t at) const { return Byte(at); }
  uint32_t word(uint32_t at) const { return Word(at); }
};
#define REX_FUNC_PROLOGUE() ((void)0)
#define REX_LOAD_U8(at) Byte(uint32_t(at))
#define REX_LOAD_U16(at) (uint32_t(Byte(uint32_t(at))) * 256 + Byte(uint32_t(at) + 1))
#define REX_LOAD_U32(at) Word(uint32_t(at))
#define REX_LOAD_U64(at) (uint64_t(Word(uint32_t(at))) << 32 | Word(uint32_t(at) + 4))
#define REX_STORE_U8(address, value) (bytes.at(uint32_t(address)) = uint8_t(value))
#define REX_STORE_U16(at, value) Half(uint32_t(at), uint16_t(value))
#define REX_STORE_U32(at, value) Put(uint32_t(at), uint32_t(value))
#define REX_STORE_U64(at, value) do { Put(uint32_t(at), uint64_t(value) >> 32); Put(uint32_t(at) + 4, uint32_t(value)); } while (0)
#include "native_definitions.h"
}  // namespace

int main() {
  using namespace simpsons::modkit::compiled_objects;
  try {
    constexpr uint32_t record = 0x4000, group = 0x5000, iterator = 0x2000;
    constexpr uint32_t ns = 0x38523FC3;
    const Memory memory;
    std::mt19937 random(0x722);
    for (unsigned trial = 0; trial < 256; ++trial) {
      bytes.fill(0);
      bytes[record + 16] = 1;
      bytes[record + 18] = 3;  // Primary resources precede the namespace table.
      bytes[record + 19] = 1;
      Put(record + 44, group - (record + 44));
      Put(group, ns);
      const uint32_t count = 2 + random() % 96;
      Half(group + 4, count);
      const uint32_t values = (group + 6 + (count + 7) / 8 + 3) & ~3u;
      uint32_t packed = values;
      for (uint32_t i = 0; i < count; ++i) {
        if (random() & 1) bytes[group + 6 + i / 8] |= 1u << (i % 8);
        else { Put(packed, random()); packed += 4; }
      }
      PPCContext ctx{};
      ctx.r1.u64 = 0x1000;
      ctx.r3.u64 = iterator;
      ctx.r4.u64 = record;
      ctx.r5.u64 = ns;
      GamePropertyBegin(ctx, bytes.data());
      for (uint32_t i = 0; i < count; ++i) {
        ctx.r3.u64 = iterator;
        ctx.r4.u64 = i;
        GamePropertySelect(ctx, bytes.data());
        ctx.r3.u64 = iterator;
        GamePropertyValue(ctx, bytes.data());
        const auto host = Property(memory, record, ns, i);
        Require(host && *host == Word(ctx.r3.u32),
                "Packed/default properties must agree with the original iterator");
      }
      Require(!Property(memory, record, ns, count), "Out-of-range properties are rejected");
      Require(!Property(memory, record, ns + 1, 0), "An absent namespace has no field");
    }
    // Native GUID fields may point backwards. Compare the original wrapper
    // reader as well as the bytes so host offsets cannot silently shift identity.
    constexpr uint32_t field = 0x6000, wrapper = 0x2100;
    for (uint32_t target : {0x3000u, 0x7000u}) {
      const Guid expected{0x12345678, 0x87654321, 0xABCDEF01, 0xDEADBEEF};
      for (uint32_t i = 0; i < 4; ++i) Put(target + 4 * i, expected[i]);
      Put(field, target - field);
      Put(wrapper, field);
      Half(wrapper + 6, 9699);
      Put(wrapper + 8, target - field);
      PPCContext ctx{};
      ctx.r3.u64 = wrapper;
      GamePropertyGuid(ctx, bytes.data());
      Require(ctx.r3.u32 == target && GuidAt(memory, field) == expected,
              "Signed relative GUIDs agree with the original reader");
    }
    bytes[record + 19] = 2;
    Put(record + 48, group - (record + 48));
    Require(!Property(memory, record, ns, 1), "Duplicate namespaces reject ambiguous identity");
    bytes[record + 19] = 1;
    Half(group + 4, 65535);
    Require(!Property(memory, record, ns, 1), "Excessive packed field counts fail closed");
    Half(group + 4, 2);
    bytes[group + 6] = 0;
    limit = group + 8;
    Require(!Property(memory, record, ns, 1), "Truncated values cannot supply a GUID");
    limit = bytes.size();
    Put(record + 44, 0x7fffffff);
    Require(!Property(memory, record, ns, 1), "An escaping relative group is rejected");
    Put(field, 0);
    Require(!GuidAt(memory, field), "Null references have no identity");
    Require(!Read(memory, 0xfffffff0), "Address overflow cannot read host memory");
    std::cout << "{\"ok\":true,\"checks\":" << checks << "}\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
