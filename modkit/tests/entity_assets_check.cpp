// Native GUID lookup is the original game's code. Sparse pages make an
// unchecked access fail; model/container allocation remains a fixture boundary.
#include "../../simpsons/src/entity_assets.h"
#include "../../simpsons/src/guest_memory.h"
#include <rex/ppc/context.h>

#include <fstream>
#include <cstring>
#include <iostream>
#include <random>
#include <stdexcept>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <sys/mman.h>
#endif

REX_EXTERN(sub_82710EB0);
REX_EXTERN(sub_82745390);
REX_EXTERN(sub_82711B28);
REX_EXTERN(__imp__sub_82711B28);
// Mesh application has its own format/ownership fixtures. This endpoint checks
// that optional edit failures cannot change the native name-reader contract.
namespace modkit_meshes {
void Apply(uint8_t*, uint32_t) {
  throw std::runtime_error("fixture optional mesh edit failure");
}
}  // namespace modkit_meshes
namespace {
constexpr uint32_t kResources = 0x82D6CADC, entity = 0x2000, model = 0x3000, prototype = 0x4000,
                   registry = 0x5000, buckets = 0x6000, entry = 0x7000, collision = 0x7100,
                   name = 0x8001, guid_at = 0x9000;
constexpr size_t kGuestBytes = 0x100000000ull;
uint8_t* memory = nullptr;
uint32_t denied = 0;
int checks = 0;
size_t reads = 0;
int native_reads = 0;
bool throw_native_read = false;
PPCContext native_input{};
void Require(bool value, const char* message) {
  ++checks;
  if (!value)
    throw std::runtime_error(message);
}
void Put(uint32_t at, uint32_t value) {
  simpsons::StoreGuestU32(memory, at, value);
}
uint32_t Word(uint32_t at) {
  return simpsons::LoadGuestU32(memory, at);
}
uint64_t Read64(const uint8_t* base, uint32_t at) {
  uint64_t value;
  std::memcpy(&value, base + at, sizeof(value));
  return std::byteswap(value);
}
void Write64(uint8_t* base, uint32_t at, uint64_t value) {
  value = std::byteswap(value);
  std::memcpy(base + at, &value, sizeof(value));
}
void ppc_trap(PPCContext&, uint8_t*, uint32_t) {
  throw std::runtime_error("Unexpected native resource lookup trap");
}
void Allocate() {
#ifdef _WIN32
  memory = static_cast<uint8_t*>(VirtualAlloc(nullptr, kGuestBytes, MEM_RESERVE, PAGE_NOACCESS));
  if (memory)
    for (const auto at : {0u, kResources & ~0xFFFFu})
      if (!VirtualAlloc(memory + at, 65536, MEM_COMMIT, PAGE_READWRITE))
        throw std::runtime_error("Cannot commit resource fixture pages");
#else
  auto* allocation =
      mmap(nullptr, kGuestBytes, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  memory = allocation == MAP_FAILED ? nullptr : static_cast<uint8_t*>(allocation);
  if (memory)
    for (const auto at : {0u, kResources & ~0xFFFFu})
      if (mprotect(memory + at, 65536, PROT_READ | PROT_WRITE))
        throw std::runtime_error("Cannot commit resource fixture pages");
#endif
  if (!memory)
    throw std::runtime_error("Cannot reserve sparse resource fixture memory");
}
}  // namespace

#undef REX_LOAD_U32
#define REX_LOAD_U32(address) simpsons::LoadGuestU32(base, uint32_t(address))
#define REX_LOAD_U8(address) simpsons::GuestToHost(base, uint32_t(address))[0]
#define REX_STORE_U32(address, value) \
  simpsons::StoreGuestU32(base, uint32_t(address), uint32_t(value))
#define REX_LOAD_U64(address) Read64(base, uint32_t(address))
#define REX_STORE_U64(address, value) Write64(base, uint32_t(address), uint64_t(value))
#define REX_FUNC_PROLOGUE() ((void)0)
#include "native_resource.h"
REX_FUNC(sub_82710EB0) {
  GameResourceLookup(ctx, base);
}
REX_FUNC(sub_82745390) {
  GameGuidHash(ctx, base);
}
// The reader's I/O/class-loader endpoint is an explicit fixture boundary.
// This verifies the production observer calls it once with unchanged inputs,
// retains pre-read strings and does not swallow its exceptions or outputs.
REX_FUNC(__imp__sub_82711B28) {
  ++native_reads;
  Require(!std::memcmp(&ctx, &native_input, sizeof(ctx)),
          "The metadata observer preserves every original native reader input");
  ctx.r3.u64 = 0x55;
  ctx.r4.u64 = 0x77;
  ctx.lr = 0x12345678;
  if (throw_native_read)
    throw std::runtime_error("original reader failure");
  base[0xB000 + 16 + 8] = 0;
}

namespace spawner {
bool Readable(uint32_t at, uint32_t bytes) {
  ++reads;
  const uint64_t end = uint64_t(at) + bytes;
  return at && bytes && at != denied &&
         !(denied == buckets && at >= buckets && at < buckets + 28) &&
         ((at < 65536 && end <= 65536) ||
          (at >= (kResources & ~0xFFFFu) && end <= uint64_t(kResources & ~0xFFFFu) + 65536));
}
}  // namespace spawner

int main(int argc, char** argv) {
  try {
    Allocate();
    auto* base = memory;
    Put(kResources, registry);
    Put(registry, buckets);
    Put(registry + 4, 7);
    Put(entity + 136, model);
    Put(model, 0x820B7030);
    Put(model + 16, prototype);
    Put(prototype, 0x4D4D646C);
    Put(prototype + 4, 10);
    Put(entry, name);
    Put(entry + 8, prototype);
    std::memcpy(base + name, "frink_geo.meta.xml", sizeof("frink_geo.meta.xml"));
    std::mt19937 random(0x534D4F44);
    for (int sample = 0; sample < 512; ++sample) {
      std::array<uint32_t, 4> guid;
      for (size_t i = 0; i < guid.size(); ++i) {
        guid[i] = random();
        Put(prototype + 16 + uint32_t(i) * 4, guid[i]);
        Put(guid_at + uint32_t(i) * 4, guid[i]);
        Put(entry + 28 + uint32_t(i) * 4, guid[i]);
        Put(collision + 28 + uint32_t(i) * 4, guid[i]);
      }
      Put(collision + 40, guid[3] ^ 1);
      Put(collision + 44, entry);
      PPCContext ctx{};
      ctx.r1.u64 = 0x1800;
      ctx.r3.u64 = guid_at;
      ctx.r4.u64 = 16;
      GameGuidHash(ctx, base);
      std::memset(base + buckets, 0, 7 * 4);
      Put(buckets + (ctx.r3.u32 % 7) * 4, collision);
      ctx.r3.u64 = guid_at;
      GameResource(ctx, base);
      Require(ctx.r3.u32 == entry, "Native lookup compares every GUID word after a hash collision");
      const auto result = entity_assets::ModelOf(base, entity);
      Require(result && result->guid == guid && result->name == "frink_geo.meta.xml" &&
                  result->Id().size() == 32,
              "The bounded model adapter agrees with the original resource lookup");
      Put(collision + 40, guid[3]);
      ctx.r3.u64 = guid_at;
      GameResource(ctx, base);
      Require(ctx.r3.u32 == collision, "The first exact native identity wins the bucket chain");
      Require(entity_assets::ModelOf(base, entity)->name.empty(),
              "A stale/replaced allocation cannot label a different model payload");
    }
    Put(collision + 40, Word(entry + 40) ^ 1);
    Require(entity_assets::ReadName(base, name, 64) == "frink_geo.meta.xml",
            "Resource names may be unaligned strings");
    base[65534] = 'x';
    base[65535] = 0;
    Require(entity_assets::ReadName(base, 65534, 64) == "x",
            "A name ending at the last readable byte never crosses the mapping");
    base[65535] = 'x';
    Require(entity_assets::ReadName(base, 65534, 64).empty(),
            "An unterminated name at an unreadable page is rejected");
    std::memset(base + 0xA000, 'a', 64);
    Require(entity_assets::ReadName(base, 0xA000, 64).empty() &&
                entity_assets::ReadName(base, UINT32_MAX, 64).empty() &&
                entity_assets::ReadName(nullptr, name).empty() &&
                entity_assets::ReadName(base, name, 1025).empty(),
            "Unterminated, wrapped and invalid string requests fail without a read");
    base[0xA000] = 1;
    base[0xA001] = 0;
    Require(entity_assets::ReadName(base, 0xA000).empty(), "Binary data is not a resource name");
    for (const auto at : {entity, model, prototype}) {
      denied = at;
      Require(!entity_assets::ModelOf(base, entity),
              "Unreadable object/prototype layouts are rejected");
    }
    denied = 0;
    Put(model, 0x820B7000);
    Require(!entity_assets::ModelOf(base, entity), "An unknown model class is not guessed");
    Put(model, 0x820B7030);
    Put(prototype + 4, 11);
    Require(!entity_assets::ModelOf(base, entity), "Unreviewed resource versions are rejected");
    Put(prototype + 4, 10);
    for (const auto at : {kResources, registry, buckets, collision, entry}) {
      denied = at;
      Require(entity_assets::ModelOf(base, entity)->name.empty(),
              "Missing registry mappings retain the model identity without an invented label");
    }
    denied = 0;
    for (const auto count : {0u, 65537u, UINT32_MAX}) {
      Put(registry + 4, count);
      Require(entity_assets::ModelOf(base, entity)->name.empty(),
              "Invalid bucket counts fail bounded lookup");
    }
    Put(registry + 4, 7);
    Put(collision + 44, collision);
    reads = 0;
    Require(entity_assets::ModelOf(base, entity)->name.empty() && reads < 270,
            "A corrupted cyclic registry chain has a finite lookup budget");
    Put(collision + 44, entry);
    constexpr uint32_t stream = 0xA800, buffer = 0xB000, header = buffer + 16, padded = 20;
    Put(stream + 12, 16);
    Put(stream + 20, buffer);
    Put(header, 64);
    Put(header + 4, padded);
    std::memcpy(base + header + 8, "frink_geo.meta.xml", sizeof("frink_geo.meta.xml"));
    for (int i = 0; i < 4; ++i)
      Put(header + 8 + padded + i * 4, Word(prototype + 16 + i * 4));
    auto pending = entity_assets::ReadStreamName(base, stream);
    Require(pending && pending->name == "frink_geo.meta.xml" &&
                pending->guid == entity_assets::ModelOf(base, entity)->guid,
            "The native stream cursor and padded header retain the matching GUID and name");
    Put(entry, 0);
    entity_assets::Loaded(base, *pending);
    Require(entity_assets::ModelOf(base, entity)->name == pending->name,
            "Models whose native entry strips names use the observed original header");
    base[header + 8] = 'X';
    Require(entity_assets::ModelOf(base, entity)->name == pending->name,
            "Retained names own their bytes instead of borrowing unloaded stream memory");
    entity_assets::Reset();
    Require(entity_assets::ModelOf(base, entity)->name.empty(),
            "Guest-memory reset clears retained names before addresses can be reused");
    Put(entry + 8, 0);
    entity_assets::Loaded(base, *pending);
    Put(entry + 8, prototype);
    Require(entity_assets::ModelOf(base, entity)->name.empty(),
            "A failed native load cannot add metadata for an absent model");
    entity_assets::Loaded(base, *pending);
    auto alias = *pending;
    alias.name = "another_geo.meta.xml";
    entity_assets::Loaded(base, alias);
    Require(entity_assets::ModelOf(base, entity)->name.empty(),
            "Conflicting names for one retained payload remain ambiguous");
    entity_assets::Reset();
    entity_assets::Loaded(base, *pending);
    std::memcpy(base + 0xC000, base + prototype, 84);
    Put(model + 16, 0xC000);
    Put(entry + 8, 0xC000);
    Require(entity_assets::ModelOf(base, entity)->name.empty(),
            "A replaced resource allocation cannot borrow a stale retained name");
    entity_assets::Loaded(base, alias);
    Require(entity_assets::ModelOf(base, entity)->name == alias.name,
            "A new proven native allocation gets its own observed header name");
    entity_assets::Reset();
    Put(model + 16, prototype);
    Put(entry + 8, prototype);
    base[header + 8] = 'f';
    for (auto size : {0u, UINT32_MAX}) {
      Put(header + 4, size);
      Require(!entity_assets::ReadStreamName(base, stream),
              "Invalid padded name bounds are rejected");
    }
    Put(header + 4, padded);
    Put(header, 0);
    Require(!entity_assets::ReadStreamName(base, stream),
            "A truncated native header has no metadata");
    Put(header, 64);
    Put(stream + 12, UINT32_MAX);
    Require(!entity_assets::ReadStreamName(base, stream),
            "Native stream cursor arithmetic cannot wrap");
    Put(stream + 12, 16);
    std::memset(base + header + 8, 'a', padded);
    Require(!entity_assets::ReadStreamName(base, stream),
            "Unterminated streamed names are rejected");
    auto hook_check = [&](bool valid_header, bool throws) {
      entity_assets::Reset();
      Put(header + 4, valid_header ? padded : UINT32_MAX);
      std::memcpy(base + header + 8, "frink_geo.meta.xml", sizeof("frink_geo.meta.xml"));
      native_input = {};
      native_input.r1.u64 = 0x1800;
      native_input.r3.u64 = 0x1200;
      native_input.r4.u64 = stream;
      native_input.r30.u64 = 0xFEDCBA9876543210ull;
      auto ctx = native_input, expected = native_input;
      expected.r3.u64 = 0x55;
      expected.r4.u64 = 0x77;
      expected.lr = 0x12345678;
      native_reads = 0;
      throw_native_read = throws;
      bool caught = false;
      try {
        sub_82711B28(ctx, base);
      } catch (const std::runtime_error& error) {
        caught = std::string(error.what()) == "original reader failure";
      }
      Require(native_reads == 1 && caught == throws && !std::memcmp(&ctx, &expected, sizeof(ctx)),
              "The original reader runs once with unchanged output and exception behavior");
      const auto expected_name = valid_header && !throws ? "frink_geo.meta.xml" : "";
      Require(entity_assets::ModelOf(base, entity)->name == expected_name,
              "Only a completed, valid native header contributes a retained model name");
    };
    hook_check(true, false);
    hook_check(false, false);
    hook_check(true, true);
    entity_assets::Reset();
    for (int i = 0; i < 4; ++i)
      Put(prototype + 16 + i * 4, 0);
    Require(!entity_assets::ModelOf(base, entity), "An all-zero model GUID is not an identity");
    if (argc > 1) {
      std::ofstream report(argv[1]);
      report << "{\"ok\":true,\"checks\":" << checks
             << ",\"original_resource_lookup\":true,\"sparse_memory\":true}\n";
      if (!report)
        throw std::runtime_error("Cannot write resource qualification report");
    }
    std::cout << checks << " native resource identity checks passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
