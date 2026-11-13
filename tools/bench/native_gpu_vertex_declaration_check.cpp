#include "simpsons_init.h"
#include "guest_memory_fixture.h"
#include "native_gpu_vertex_declaration.h"
#include <rex/ppc/barriers.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>

static uint64_t copy_calls = 0, sort_swaps = 0, merged_fetches = 0;
static uint64_t matched_attributes = 0, missing_attributes = 0;
#include "native_gpu_vertex_declaration_reference.inc"

namespace port = simpsons::native_gpu_calls;
using simpsons::LoadGuestU32;
using simpsons::StoreGuestU32;
namespace {
constexpr uint32_t kSize = 0x90000, kResource = 0x1000, kDeclaration = 0x30000;
constexpr uint32_t kCode = 0x10000, kMap = 0x40000, kStack = 0x80000;
constexpr uint32_t kTables = port::vertex_declaration_detail::kSwizzleTable;
std::unique_ptr<GuestMemoryFixture> original, native;
std::array<uint8_t, kSize> pristine;
constexpr std::array<PPCRegister PPCContext::*, 18> kNonvolatile = {
    &PPCContext::r14, &PPCContext::r15, &PPCContext::r16, &PPCContext::r17, &PPCContext::r18,
    &PPCContext::r19, &PPCContext::r20, &PPCContext::r21, &PPCContext::r22, &PPCContext::r23,
    &PPCContext::r24, &PPCContext::r25, &PPCContext::r26, &PPCContext::r27, &PPCContext::r28,
    &PPCContext::r29, &PPCContext::r30, &PPCContext::r31};
uint64_t cases = 0, rejections = 0;
uint32_t current_seed = 0, current_count = 0, current_attributes = 0, current_group = 0;

void Require(bool ok, const char* why) {
  if (!ok)
    throw std::runtime_error(why);
}

uint32_t Next(uint32_t& state) {
  state ^= state << 13;
  state ^= state >> 17;
  state ^= state << 5;
  return state;
}

PPCContext Prepare(uint32_t count, uint32_t attributes, uint32_t variant, uint32_t group,
                   uint32_t seed) {
  auto* base = original->data();
  std::memcpy(base, pristine.data(), kSize);
  uint32_t random = seed + 1;
  const uint32_t relative = 0x2000 + variant * 0x400, selected = kResource + relative + 872;
  StoreGuestU32(base, kResource + (variant + 112) * 8, relative);
  StoreGuestU32(base, selected + 24, 9);
  StoreGuestU32(base, selected + 28, count);
  StoreGuestU32(base, kDeclaration + 24, attributes);
  for (uint32_t i = 0; i < attributes; ++i) {
    const uint32_t stream = (seed & 1) ? 0 : Next(random) % 96;
    const uint32_t offset = (seed & 1) ? (i * 4) % 16 : Next(random) & 0xFFFF;
    StoreGuestU32(base, kDeclaration + 52 + i * 12, stream << 16 | offset);
    StoreGuestU32(base, kDeclaration + 56 + i * 12, Next(random));
    StoreGuestU32(base, kDeclaration + 60 + i * 12, ((i & 15) << 16) | ((i >> 4) << 8));
  }
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t source = kResource + variant * 416 + 68 + i * 12;
    for (uint32_t j = 0; j < 3; ++j)
      StoreGuestU32(base, source + j * 4, Next(random));
    uint32_t marker = 0;
    if (group == 1 && (i == 0 || i == count - 1))
      marker = 0x100000;
    if (group == 2 && (i == 0 || i == count - 1))
      marker = i == 0 ? 0x100000 : 0x300000;
    if (group == 3)
      marker = ((Next(random) & 3) << 20);
    const uint32_t usage = (seed & 2) ? i & 15 : Next(random) & 15;
    const uint32_t usage_index = (seed & 2) ? i >> 4 : Next(random) & 15;
    const uint32_t output = (seed & 4) ? (i & 1 ? 4095 : 0) : Next(random) & 4095;
    StoreGuestU32(base, selected + 72 + i * 4, marker | usage_index << 16 | usage << 12 | output);
  }
  for (uint32_t i = 0; i < 96; ++i)
    *simpsons::GuestToHost(base, kMap + i) = uint8_t(seed + i * 3);
  *simpsons::GuestToHost(base, kStack - 512) = uint8_t(seed);
  *simpsons::GuestToHost(base, kStack - 511) = uint8_t(seed);
  for (uint32_t i = 0; i < 16; i += 4)
    StoreGuestU32(base, kTables + i, Next(random));
  for (uint32_t i = 0; i < 64; ++i)
    *simpsons::GuestToHost(base, kTables + 16 + i) = uint8_t((seed & 1) ? 1 : Next(random) & 15);
  std::memcpy(native->data(), base, kSize);
  std::memcpy(simpsons::GuestToHost(native->data(), kTables), simpsons::GuestToHost(base, kTables),
              80);
  PPCContext ctx{};
  ctx.r1.u64 = 0xABCD000000000000ull | kStack;
  ctx.r3.u64 = 0xA5A5000000000000ull | kResource;
  ctx.r4.u64 = 0xFEED000000000000ull | kCode + (seed & 4);
  ctx.r5.u64 = 0xBCDE000000000000ull | kDeclaration;
  ctx.r6.u64 = kMap;
  ctx.r7.u64 = variant;
  ctx.lr = 0xDEADBEEF82345678ull;
  ctx.xer.ca = 1;
  for (uint32_t i = 0; i < kNonvolatile.size(); ++i)
    (ctx.*kNonvolatile[i]).u64 = 0x1234ABCD00000000ull | (i * 0x10203u);
  return ctx;
}

void Compare(PPCContext input) {
  auto expected = input, actual = input;
  reference_sub_8245EDC8(expected, original->data());
  Require(port::PatchVertexDeclaration(actual, native->data()),
          "Safe vertex declaration was rejected");
  Require(expected.r1.u64 == actual.r1.u64 && expected.r3.u64 == actual.r3.u64 &&
              expected.lr == actual.lr,
          "Vertex declaration changed stack, return or link ABI");
  for (auto reg : kNonvolatile)
    Require((expected.*reg).u64 == (actual.*reg).u64,
            "Vertex declaration changed a nonvolatile GPR");
  for (uint32_t i = 0; i < kSize; ++i) {
    if (i >= kStack - 600 && i < kStack)
      continue;
    if (original->data()[i] != native->data()[i]) {
      std::fprintf(stderr, "First difference at %08X: expected=%02X actual=%02X\n", i,
                   original->data()[i], native->data()[i]);
      throw std::runtime_error("Vertex declaration changed code, metadata or neighboring memory");
    }
  }
  Require(!std::memcmp(simpsons::GuestToHost(original->data(), kTables),
                       simpsons::GuestToHost(native->data(), kTables), 80),
          "Vertex declaration changed the swizzle/size tables");
  if (input.r4.u32 >= kSize)
    Require(!std::memcmp(simpsons::GuestToHost(original->data(), input.r4.u32),
                         simpsons::GuestToHost(native->data(), input.r4.u32), 4096 * 12),
            "Vertex declaration changed high-view code or neighboring instructions");
  ++cases;
}

void Reject(uint32_t problem) {
  auto ctx = Prepare(3, 2, 0, 1, 3);
  auto* base = native->data();
  constexpr uint32_t selected = kResource + 0x2000 + 872;
  switch (problem) {
    case 0:
      ctx.r1.u32 += 1;
      break;
    case 1:
      ctx.r1.u32 = 64;
      break;
    case 2:
      ctx.r3.u32 |= 1;
      break;
    case 3:
      ctx.r4.u32 |= 1;
      break;
    case 4:
      ctx.r5.u32 |= 1;
      break;
    case 5:
      ctx.r7.u64 = UINT64_MAX;
      break;
    case 6:
      ctx.r7.u64 = UINT32_MAX;
      break;
    case 7:
      StoreGuestU32(base, selected + 28, 33);
      break;
    case 8:
      StoreGuestU32(base, selected + 24, UINT32_MAX);
      break;
    case 9:
      StoreGuestU32(base, kResource + 896, UINT32_MAX);
      break;
    case 10:
      StoreGuestU32(base, kDeclaration + 24, 257);
      break;
    case 11:
      ctx.r6.u32 = kStack - 64;
      break;
    case 12:
      ctx.r5.u32 = kStack - 64;
      break;
    case 13:
      StoreGuestU32(base, kDeclaration + 52, 96u << 16);
      StoreGuestU32(base, selected + 72, 0);
      break;
    case 14:
      ctx.r4.u32 = kResource;
      StoreGuestU32(base, selected + 72, 0);
      break;
    case 15:
      ctx.r4.u32 = kMap;
      StoreGuestU32(base, selected + 72, 0);
      break;
    case 16:
      ctx.r4.u32 = kDeclaration;
      StoreGuestU32(base, selected + 72, 0);
      break;
    case 17:
      ctx.r4.u32 = kStack - 16;
      StoreGuestU32(base, selected + 72, 0);
      break;
    case 18:
      ctx.r4.u32 = UINT32_MAX - 3;
      StoreGuestU32(base, selected + 72, 0);
      break;
    case 19:
      ctx.r4.u32 = kTables;
      StoreGuestU32(base, selected + 72, 0);
      break;
    case 20:
      // A later target aliases metadata, after a valid first instruction.
      ctx.r4.u32 = kResource;
      StoreGuestU32(base, selected + 72, 4095);
      StoreGuestU32(base, selected + 76, 0);
      break;
    case 21:
      ctx.r4.u32 = 0xC0000000;
      ctx.r6.u32 = 0xA0000004;
      native->Commit(0xA0000000, 4096);
      std::memset(simpsons::GuestToHost(base, 0xA0000004), 0, 96);
      StoreGuestU32(base, selected + 72, 0);
      break;
    case 22:
      ctx.r1.u32 = (kTables + 128 + 15) & ~15u;
      break;
  }
  const auto saved = ctx;
  std::memcpy(original->data(), base, kSize);
  Require(!port::PatchVertexDeclaration(ctx, base), "Unsafe vertex declaration was accepted");
  Require(!std::memcmp(&ctx, &saved, sizeof(ctx)) && !std::memcmp(base, original->data(), kSize),
          "Rejected vertex declaration changed context or partially patched code");
  Require(!std::memcmp(simpsons::GuestToHost(original->data(), kTables),
                       simpsons::GuestToHost(native->data(), kTables), 80),
          "Rejected vertex declaration changed the lookup tables");
  ++rejections;
}
}  // namespace

int main(int argc, char** argv) {
  if (argc > 2 || (argc == 2 && !std::freopen(argv[1], "w", stdout)))
    return 2;
  try {
    original = std::make_unique<GuestMemoryFixture>(kSize);
    native = std::make_unique<GuestMemoryFixture>(kSize);
    original->Commit(kTables, 80);
    native->Commit(kTables, 80);
    for (uint32_t i = 0; i < kSize; ++i)
      pristine[i] = uint8_t(i * 37 + (i >> 6));
    for (uint32_t count : {0u, 1u, 2u, 3u, 7u, 16u, 31u, 32u})
      for (uint32_t attributes : {0u, 1u, 2u, 4u, 16u, 32u})
        for (uint32_t variant : {0u, 1u})
          for (uint32_t group = 0; group < 4; ++group)
            for (uint32_t seed = 0; seed < 8; ++seed) {
              current_count = count;
              current_attributes = attributes;
              current_group = group;
              current_seed = seed;
              Compare(Prepare(count, attributes, variant, group, seed));
            }
    for (uint32_t view :
         {0x7F040000u, 0x82040000u, 0x90040000u, 0xA0040000u, 0xC0040000u, 0xE0040000u}) {
      for (uint32_t alignment : {0u, 4u}) {
        auto ctx = Prepare(32, 32, 1, 3, 7);
        for (auto* memory : {original.get(), native.get()}) {
          memory->Commit(view, 4096 * 12 + 8192);
          std::memcpy(simpsons::GuestToHost(memory->data(), view), pristine.data(), 4096 * 12 + 4);
        }
        ctx.r4.u32 = view + alignment;
        Compare(ctx);
      }
      auto ctx = Prepare(32, 32, 1, 3, 7);
      for (auto* memory : {original.get(), native.get()}) {
        memory->Commit(view, 0x7000);
        std::memcpy(simpsons::GuestToHost(memory->data(), view), memory->data() + kResource,
                    0x6000);
      }
      ctx.r3.u32 = view;
      Compare(ctx);
      Require(!std::memcmp(simpsons::GuestToHost(original->data(), view),
                           simpsons::GuestToHost(native->data(), view), 0x6000),
              "Vertex declaration changed high-view metadata");
    }
    Require(copy_calls && sort_swaps && merged_fetches && matched_attributes && missing_attributes,
            "Vertex-declaration oracle missed a copy, sort, merge, match or default branch");
    for (uint32_t problem = 0; problem < 23; ++problem) {
      current_seed = problem;
      Reject(problem);
    }
    std::printf(
        "{\"passed\":true,\"cases\":%llu,\"real_memcpy_calls\":%llu,"
        "\"sort_swaps\":%llu,\"merged_fetches\":%llu,\"matched_attributes\":%llu,"
        "\"missing_attributes\":%llu,\"unchanged_fallbacks\":%llu}\n",
        (unsigned long long)cases, (unsigned long long)copy_calls, (unsigned long long)sort_swaps,
        (unsigned long long)merged_fetches, (unsigned long long)matched_attributes,
        (unsigned long long)missing_attributes, (unsigned long long)rejections);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s; count=%u attributes=%u group=%u seed=%u\n", error.what(),
                 current_count, current_attributes, current_group, current_seed);
    return 1;
  }
}
