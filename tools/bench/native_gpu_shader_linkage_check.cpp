#include "simpsons_init.h"
#include "guest_memory_fixture.h"
#include "native_gpu_shader_linkage.h"
#include <rex/ppc/barriers.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>

static uint64_t copy_calls = 0;
#include "native_gpu_shader_linkage_reference.inc"

namespace port = simpsons::native_gpu_calls;
using simpsons::LoadGuestU32;
using simpsons::StoreGuestU32;

namespace {
constexpr uint32_t kSize = 0x90000, kResource = 0x1000, kSecondary = 0x30000;
constexpr uint32_t kCode = 0x10000, kResult = 0x40000, kStack = 0x80000;
constexpr uint32_t kTable = kResource + (5 + 7 + 9) * 4;
std::unique_ptr<GuestMemoryFixture> original, native;
std::array<uint8_t, kSize> pristine;
constexpr std::array<PPCRegister PPCContext::*, 18> kNonvolatile = {
    &PPCContext::r14, &PPCContext::r15, &PPCContext::r16, &PPCContext::r17, &PPCContext::r18,
    &PPCContext::r19, &PPCContext::r20, &PPCContext::r21, &PPCContext::r22, &PPCContext::r23,
    &PPCContext::r24, &PPCContext::r25, &PPCContext::r26, &PPCContext::r27, &PPCContext::r28,
    &PPCContext::r29, &PPCContext::r30, &PPCContext::r31};
uint64_t cases = 0, rejections = 0, neutral_cases = 0, component_cases = 0, merge_cases = 0;
uint32_t current_kind = 0, current_seed = 0, current_count = 0, current_length = 0;

void Require(bool ok, const char* why) {
  if (!ok)
    throw std::runtime_error(why);
}

PPCContext Prepare(uint32_t kind, uint32_t count, uint32_t length, uint32_t secondary_count,
                   uint32_t seed) {
  auto* base = original->data();
  std::memcpy(base, pristine.data(), kSize);
  StoreGuestU32(base, kResource + 8, 0);
  StoreGuestU32(base, kResource + 20, count * 32);
  StoreGuestU32(base, kResource + 24, 5);
  StoreGuestU32(base, kResource + 28, 7);
  StoreGuestU32(base, kSecondary + 20, secondary_count * 32 | (seed & 31));
  const uint32_t lists = std::max(1u, count), list_base = kTable + count * 4;
  for (uint32_t i = 0; i < lists; ++i) {
    const uint32_t type = (seed + i * 7) & 15, semantic = i * 2;
    StoreGuestU32(base, kTable + i * 4, (i * length) << 16 | type << 8 | semantic);
    for (uint32_t j = 0; j < length; ++j) {
      uint32_t instruction = (seed * 37 + i * 19 + j * 5) & 0xFFF;
      if ((seed & 7) == 0)
        instruction = (j & 1) ? 4095 : 0;
      StoreGuestU32(base, list_base + (i * length + j) * 4,
                    instruction | (j == length - 1 ? 0x1000 : 0));
    }
  }
  for (uint32_t i = 0; i < secondary_count; ++i) {
    const uint32_t selected = i * count / secondary_count;
    const uint32_t type = (seed + selected * 7 + (seed & 1)) & 15;
    StoreGuestU32(base, kSecondary + 32 + i * 4, type << 8 | selected * 2);
  }
  StoreGuestU32(base, kResult, seed * 0x91E10DA5u);
  std::memcpy(native->data(), base, kSize);
  PPCContext ctx{};
  ctx.r1.u64 = 0xABCD000000000000ull | kStack;
  ctx.r3.u64 = 0xA5A5000000000000ull | (kind == 2 ? 0x12345678 : kResource);
  ctx.r4.u64 = 0xFEED000000000000ull | (kind == 2 ? kResource : kCode + (seed & 4));
  ctx.r5.u64 = 0xBCDE000000000000ull | (kind == 2 ? kCode + (seed & 4) : kTable);
  ctx.r6.u64 = kResult;
  ctx.r7.u64 = secondary_count || (seed & 2) ? kSecondary : 0;
  ctx.lr = 0xDEADBEEF82345678ull;
  ctx.xer.ca = 1;
  for (uint32_t i = 0; i < kNonvolatile.size(); ++i)
    (ctx.*kNonvolatile[i]).u64 = 0x1234ABCD00000000ull | (i * 0x10203u);
  return ctx;
}

bool Native(uint32_t kind, PPCContext& ctx, uint8_t* base) {
  return kind == 2 ? port::LinkShaderExports(ctx, base)
                   : port::PatchShaderLinkage(ctx, base, kind == 0);
}

void Compare(uint32_t kind, PPCContext input) {
  auto expected = input, actual = input;
  const std::array reference = {reference_sub_8245F3D8, reference_sub_8245F4A8,
                                reference_sub_8245F560};
  reference[kind](expected, original->data());
  Require(Native(kind, actual, native->data()), "Safe shader linkage was rejected");
  Require(expected.r1.u64 == actual.r1.u64 && expected.r3.u64 == actual.r3.u64 &&
              expected.lr == actual.lr,
          "Shader linkage changed stack, return or link ABI");
  for (auto reg : kNonvolatile)
    Require((expected.*reg).u64 == (actual.*reg).u64, "Shader linkage changed a nonvolatile GPR");
  const uint32_t frame = kind == 2 ? 328 : kind == 0 ? 136 : 152;
  Require(!std::memcmp(original->data(), native->data(), kStack - frame) &&
              !std::memcmp(original->data() + kStack, native->data() + kStack, kSize - kStack),
          "Shader linkage changed code, metadata, export count or neighboring memory");
  const uint32_t code = kind == 2 ? input.r5.u32 : input.r4.u32;
  if (code >= kSize)
    Require(!std::memcmp(simpsons::GuestToHost(original->data(), code),
                         simpsons::GuestToHost(native->data(), code), 4096 * 12),
            "Shader linkage changed high-view code, padding or neighboring instructions");
  ++cases;
  (kind == 2 ? merge_cases : kind == 0 ? neutral_cases : component_cases)++;
}

void Reject(uint32_t kind, uint32_t problem) {
  auto ctx = Prepare(kind, 2, 2, 1, 1);
  auto* base = native->data();
  switch (problem) {
    case 0:
      ctx.r1.u32 += 1;
      break;
    case 1:
      ctx.r1.u32 = 64;
      break;
    case 2:
      (kind == 2 ? ctx.r4 : ctx.r3).u32 |= 1;
      break;
    case 3:
      (kind == 2 ? ctx.r5 : ctx.r4).u32 |= 1;
      break;
    case 4:
      StoreGuestU32(base, kResource + 24, UINT32_MAX);
      break;
    case 5:
      StoreGuestU32(base, kResource + 28, 0x40000000);
      break;
    case 6:
      for (uint32_t i = 0; i < 4096; ++i)
        StoreGuestU32(base, kTable + 8 + i * 4, 0);
      break;
    case 7:
      (kind == 2 ? ctx.r5 : ctx.r4).u32 = kResource;
      StoreGuestU32(base, kTable + 8, 0x1000);
      break;
    case 8:
      (kind == 2 ? ctx.r5 : ctx.r4).u32 = kStack - 16;
      StoreGuestU32(base, kTable + 8, 0x1000);
      break;
    case 9:
      (kind == 2 ? ctx.r5 : ctx.r4).u32 = 0xFFFFFFFC;
      StoreGuestU32(base, kTable + 8, 0x1000);
      break;
    case 10:
      (kind == 2 ? ctx.r5 : ctx.r4).u32 = 0x7EFFFFFC;
      StoreGuestU32(base, kTable + 8, 0x1000);
      break;
    case 11:
      if (kind == 2) {
        ctx.r6.u32 = kResource + 20;
      } else {
        ctx.r5.u32 = kStack - 48;
      }
      break;
    case 12:
      if (kind == 2) {
        ctx.r7.u32 = kStack - 64;
      } else {
        // The first instruction is safe; a later instruction aliases metadata.
        (kind == 2 ? ctx.r5 : ctx.r4).u32 = kResource;
        StoreGuestU32(base, kTable + 8, 4095);
        StoreGuestU32(base, kTable + 12, 0x1000);
      }
      break;
    case 13:
      if (kind == 2) {
        StoreGuestU32(base, kSecondary + 32, 0x1FF);
      } else if (kind == 1) {
        ctx.r6.u32 = kCode + 12;
        StoreGuestU32(base, kTable + 8, 0x1001);
      } else {
        ctx.r5.u32 = kCode + 12;
        StoreGuestU32(base, kCode + 12, 0);
        StoreGuestU32(base, kTable + 8, 0x1001);
      }
      break;
    case 14:
      (kind == 2 ? ctx.r5 : ctx.r4).u32 = 0xC0000000;
      StoreGuestU32(base, kTable + 8, 0x1000);
      if (kind == 2)
        ctx.r6.u32 = 0xA0000004;
      else
        ctx.r5.u32 = 0xA0000004;
      // A/C physical aliases are proved even when only the source view is read.
      native->Commit(0xA0000000, 4096);
      StoreGuestU32(base, 0xA0000004, 0);
      break;
  }
  const auto saved = ctx;
  std::memcpy(original->data(), base, kSize);
  Require(!Native(kind, ctx, base), "Unsafe shader linkage was accepted");
  Require(!std::memcmp(&ctx, &saved, sizeof(ctx)) && !std::memcmp(base, original->data(), kSize),
          "Rejected shader linkage changed context or partially patched code");
  if (problem == 14)
    Require(LoadGuestU32(base, 0xA0000004) == 0, "Rejected alias changed physical source");
  ++rejections;
}
}  // namespace

int main(int argc, char** argv) {
  if (argc > 2 || (argc == 2 && !std::freopen(argv[1], "w", stdout)))
    return 2;
  try {
    original = std::make_unique<GuestMemoryFixture>(kSize);
    native = std::make_unique<GuestMemoryFixture>(kSize);
    for (uint32_t i = 0; i < kSize; ++i)
      pristine[i] = uint8_t(i * 37 + (i >> 6));
    for (uint32_t kind : {0u, 1u})
      for (uint32_t length : {1u, 2u, 7u, 31u, 256u, 4096u})
        for (uint32_t seed = 0; seed < 32; ++seed) {
          current_kind = kind;
          current_seed = seed;
          current_count = 1;
          current_length = length;
          Compare(kind, Prepare(kind, 1, length, 0, seed));
        }
    for (uint32_t count : {0u, 1u, 2u, 3u, 5u, 16u, 31u})
      for (uint32_t selected = 0; selected <= count; ++selected)
        for (uint32_t length : {1u, 2u, 7u})
          for (uint32_t seed = 0; seed < 32; ++seed) {
            current_kind = 2;
            current_seed = seed;
            current_count = count;
            current_length = length;
            Compare(2, Prepare(2, count, length, selected, seed));
          }
    for (uint32_t skip = 0; skip < 3; ++skip) {
      auto ctx = Prepare(2, 31, 7, 31, skip);
      for (auto* base : {original->data(), native->data()}) {
        if (skip == 0)
          *simpsons::GuestToHost(base, kResource + 8) = 7;
        else
          StoreGuestU32(base, (skip == 1 ? kResource : kSecondary) + 20,
                        skip == 1 ? 0x40000 : 0x20000);
      }
      Compare(2, ctx);
    }
    for (uint32_t view :
         {0x7F040000u, 0x82040000u, 0x90040000u, 0xA0040000u, 0xC0040000u, 0xE0040000u})
      for (uint32_t kind = 0; kind < 3; ++kind) {
        for (uint32_t alignment : {0u, 4u}) {
          auto ctx = Prepare(kind, 5, 7, 3, 7);
          for (auto* memory : {original.get(), native.get()}) {
            memory->Commit(view, 4096 * 12 + 8192);
            std::memcpy(simpsons::GuestToHost(memory->data(), view), pristine.data(),
                        4096 * 12 + 4);
          }
          (kind == 2 ? ctx.r5 : ctx.r4).u32 = view + alignment;
          Compare(kind, ctx);
        }
        auto ctx = Prepare(kind, 5, 7, 3, 7);
        for (auto* memory : {original.get(), native.get()}) {
          memory->Commit(view, 0x9000);
          std::memcpy(simpsons::GuestToHost(memory->data(), view), memory->data() + kResource,
                      0x8000);
        }
        (kind == 2 ? ctx.r4 : ctx.r3).u32 = view;
        Compare(kind, ctx);
        Require(!std::memcmp(simpsons::GuestToHost(original->data(), view),
                             simpsons::GuestToHost(native->data(), view), 0x8000),
                "Shader linkage changed a high-view resource");
      }
    // Descriptors may share a terminated patch list. Preserve repeated writes
    // and the original merge behavior when semantic bytes are not identical.
    for (uint32_t seed = 0; seed < 32; ++seed) {
      auto ctx = Prepare(2, 5, 7, 3, seed);
      for (auto* base : {original->data(), native->data()}) {
        for (uint32_t i = 1; i < 5; ++i)
          StoreGuestU32(base, kTable + i * 4, LoadGuestU32(base, kTable + i * 4) & 0xFFFF);
        StoreGuestU32(base, kSecondary + 32, (seed & 15) << 8 | 0);
        StoreGuestU32(base, kSecondary + 36, ((seed + 1) & 15) << 8 | 1);
        StoreGuestU32(base, kSecondary + 40, ((seed + 2) & 15) << 8 | 3);
      }
      Compare(2, ctx);
    }
    for (uint32_t kind = 0; kind < 3; ++kind)
      for (uint32_t problem = 0; problem < 15; ++problem) {
        current_kind = kind;
        current_seed = problem;
        Reject(kind, problem);
      }
    std::printf(
        "{\"passed\":true,\"cases\":%llu,\"neutral_cases\":%llu,"
        "\"component_cases\":%llu,\"merge_cases\":%llu,\"real_memcpy_calls\":%llu,"
        "\"unchanged_fallbacks\":%llu}\n",
        (unsigned long long)cases, (unsigned long long)neutral_cases,
        (unsigned long long)component_cases, (unsigned long long)merge_cases,
        (unsigned long long)copy_calls, (unsigned long long)rejections);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s; kind=%u count=%u length=%u seed=%u\n", error.what(), current_kind,
                 current_count, current_length, current_seed);
    return 1;
  }
}
