// Compare selected-state planning with the actual generated helper. Every
// stream, mask restoration and return bit is checked before backend tests.
#include "simpsons_init.h"
#include "native_gpu_conditional.h"
#include "guest_memory_fixture.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <vector>

static uint64_t reference_calls = 0;
#include "native_gpu_conditional_reference.inc"

namespace port = simpsons::native_gpu_state;
namespace state = rex::graphics::native_gpu_state;
namespace predication = rex::graphics::native_gpu_predication;
using simpsons::LoadGuestU32;
using simpsons::StoreGuestU32;

namespace {
constexpr uint32_t kSize = 256 * 1024, kDevice = 0x1000, kCursor = 0x10000, kStack = 0x3F000;
std::unique_ptr<GuestMemoryFixture> original, native;
std::array<uint8_t, kSize> pristine;
uint64_t cases = 0, mask_calls = 0, selected_states = 0, ordinary_states = 0, rejected = 0;
uint32_t current_variant = 0, current_streams = 0;

void Require(bool ok, const char* why) {
  if (!ok)
    throw std::runtime_error(why);
}

PPCContext Prepare(uint32_t flags, uint32_t mode, uint32_t streams, uint32_t alignment,
                   uint32_t variant) {
  auto* base = original->data();
  std::memcpy(base, pristine.data(), kSize);
  StoreGuestU32(base, kDevice + 48, kCursor + alignment);
  StoreGuestU32(base, kDevice + 52, 0x30000);
  StoreGuestU32(base, kDevice + 56, 0x2F000);
  *simpsons::GuestToHost(base, kDevice + 10940) = uint8_t((flags & 7) << 4);
  *simpsons::GuestToHost(base, kDevice + 10943) = uint8_t((flags >> 3) << 4);
  StoreGuestU32(base, kDevice + 11844, mode << 17);
  StoreGuestU32(base, kDevice + 12740, streams);
  for (uint32_t i = 0; i < 5; ++i) {
    const uint32_t value = i * 0x10203u + variant;
    StoreGuestU32(base, kDevice + 12432 + i * 4, variant % 7 == 6 ? 0 : value);
    StoreGuestU32(base, kDevice + 12720 + i * 4, value + (variant % 7 == i));
  }
  StoreGuestU32(base, kDevice + 12700, 0xA5A50102u ^ variant);
  StoreGuestU32(base, kDevice + 12704, 0x01234567u + variant);
  StoreGuestU32(base, kDevice + 40, 0x02000020);
  StoreGuestU32(base, kDevice + 44, 0x100);
  StoreGuestU32(base, kDevice + 10368, variant & 1 ? UINT32_MAX : variant * 0x9E3779B9u);
  StoreGuestU32(base, kDevice + 10460, variant * 0x10203u);
  for (uint32_t i = 0; i < 453; ++i)
    StoreGuestU32(base, kDevice + 13104 + i * 4,
                  variant & 1 ? UINT32_MAX : variant * 0x9E3779B9u + i);
  StoreGuestU32(base, kStack - 112, variant * 0x1020305u);
  std::memcpy(native->data(), base, kSize);
  PPCContext ctx{};
  ctx.r1.u64 = kStack;
  ctx.r3.u64 = kDevice;
  ctx.r4.u64 = (variant & 1 ? 0x100ull : 0) | (variant & 2 ? uint64_t(1) << 37 : 0) |
               (variant & 4 ? uint64_t(1) << 57 : 0) | 0x8080808080808080ull;
  ctx.r5.u64 = 0xABCD000000000000ull | (variant & 1 ? UINT32_MAX : variant * 0x91E10DA5u);
  ctx.lr = 0xABCD000082123450ull;
  ctx.r20.u64 = 0xDEADBEEF00000020ull;
  ctx.r21.u64 = 0xDEADBEEF00000021ull;
  ctx.r22.u64 = 0xDEADBEEF00000022ull;
  ctx.r23.u64 = 0xDEADBEEF00000023ull;
  ctx.r24.u64 = 0xDEADBEEF00000024ull;
  ctx.r25.u64 = 0xDEADBEEF00000025ull;
  ctx.r26.u64 = 0xDEADBEEF00000026ull;
  ctx.r27.u64 = 0xDEADBEEF00000027ull;
  ctx.r28.u64 = 0xDEADBEEF00000028ull;
  ctx.r29.u64 = 0xDEADBEEF00000029ull;
  ctx.r30.u64 = 0xDEADBEEF00000030ull;
  ctx.r31.u64 = 0xDEADBEEF00000031ull;
  return ctx;
}

void Check(uint32_t flags, uint32_t mode, uint32_t streams, uint32_t alignment, uint32_t variant) {
  current_variant = variant;
  current_streams = streams;
  auto expected = Prepare(flags, mode, streams, alignment, variant), actual = expected;
  reference_sub_8245E530(expected, original->data());
  const uint64_t before = reference_calls;
  Require(port::WriteConditional(actual, native->data()), "Supported selected state rejected");
  Require(reference_calls == before, "Native selected state entered original helper");
  Require(actual.r1.u64 == expected.r1.u64 && actual.r3.u64 == expected.r3.u64 &&
              actual.lr == expected.lr && actual.fpscr.csr == expected.fpscr.csr,
          "Selected state changed its return, stack, link or FP state");
  for (auto reg : {&PPCContext::r20, &PPCContext::r21, &PPCContext::r22, &PPCContext::r23,
                   &PPCContext::r24, &PPCContext::r25, &PPCContext::r26, &PPCContext::r27,
                   &PPCContext::r28, &PPCContext::r29, &PPCContext::r30, &PPCContext::r31})
    Require((actual.*reg).u64 == (expected.*reg).u64, "Selected state changed a saved register");
  const uint32_t end = LoadGuestU32(original->data(), kDevice + 48);
  Require(end == LoadGuestU32(native->data(), kDevice + 48),
          "Selected state changed buffer advance");
  std::vector<uint8_t> normalized(native->data(), native->data() + kSize);
  for (uint32_t at = kCursor + alignment + 4; at <= end;) {
    const uint32_t old = LoadGuestU32(original->data(), at),
                   replacement = LoadGuestU32(native->data(), at);
    const uint32_t count = ((replacement >> 16) & 0x3FFF) + 1;
    Require(uint64_t(at) + count * 4 <= end, "Selected state crosses the published end");
    if ((old >> 30) == 0) {
      state::StateRange range;
      Require(state::DecodeHeader(replacement, count, range) &&
                  range.first_register == (old & 0x7FFF) && count == ((old >> 16) & 0x3FFF) + 1,
              "Selected state changed a register range");
      ++ordinary_states;
    } else {
      Require(predication::IsValidHeader(replacement, count), "Malformed selected-state metadata");
      if (old == 0xC0006000 || old == 0xC0006100) {
        Require(replacement == (old == 0xC0006000 ? predication::kMaskLowHeader
                                                  : predication::kMaskHighHeader),
                "Selected state exchanged mask halves");
        ++mask_calls;
      } else {
        uint32_t first;
        Require(old == 0xC0012D01 && replacement == predication::kStateHeader &&
                    predication::DecodeRegister(LoadGuestU32(native->data(), at + 4), first),
                "Selected state lost a predicated destination");
        ++selected_states;
      }
    }
    StoreGuestU32(normalized.data(), at, old);
    // Copy and patch a value at its original position; metadata stays intact.
    std::array<uint32_t, 3> copied{};
    for (uint32_t i = 0; i <= count; ++i)
      copied[i] = LoadGuestU32(native->data(), at + i * 4);
    copied[count] ^= 0x91E10DA5u;
    Require(copied[count] == (LoadGuestU32(original->data(), at + count * 4) ^ 0x91E10DA5u),
            "Copied selected-state operand changed its patch offset");
    at += (count + 1) * 4;
  }
  Require(!std::memcmp(original->data(), normalized.data(), kStack - 192) &&
              !std::memcmp(original->data() + kStack, normalized.data() + kStack, kSize - kStack),
          "Selected state changed operands or memory outside its private frame");
  ++cases;
}

void Reject(PPCContext ctx) {
  const auto saved = ctx;
  std::vector<uint8_t> memory(native->data(), native->data() + kSize);
  const uint64_t before = reference_calls;
  Require(!port::WriteConditional(ctx, native->data()) && !std::memcmp(&ctx, &saved, sizeof(ctx)) &&
              !std::memcmp(native->data(), memory.data(), kSize) && reference_calls == before,
          "Rejected conditional state changed memory/context or called a helper");
  ++rejected;
}

void NegativeChecks() {
  for (uint32_t probe = 0; probe < 10; ++probe) {
    auto ctx = Prepare(0x11, 2, 5, 0, 7);
    auto* base = native->data();
    switch (probe) {
      case 0:
        ctx.r3.u32 += 1;
        break;
      case 1:
        ctx.r1.u32 += 4;
        break;
      case 2:
        ctx.r1.u32 = 176;
        break;
      case 3:
        StoreGuestU32(base, kDevice + 48, kCursor + 1);
        break;
      case 4:
        StoreGuestU32(base, kDevice + 56, kCursor - 4);
        break;
      case 5:
        StoreGuestU32(base, kDevice + 12740, 454);
        break;
      case 6:
        StoreGuestU32(base, kDevice + 48, UINT32_MAX - 3);
        break;
      case 7:
        StoreGuestU32(base, kDevice + 48, kDevice + 512);
        break;
      case 8:
        StoreGuestU32(base, kDevice + 48, kStack - 196);
        break;
      case 9:
        ctx.r1.u32 = kDevice + 256;
        break;
    }
    if (probe >= 6) {
      StoreGuestU32(base, kDevice + 52, UINT32_MAX);
      StoreGuestU32(base, kDevice + 56, UINT32_MAX);
    }
    Reject(ctx);
  }
  auto ctx = Prepare(0x11, 2, 33, 0, 7);
  port::ConditionalPlan plan;
  Require(port::PlanConditionalAt(ctx, native->data(), kCursor, plan) && plan.publications == 33,
          "Conditional growth probes lost their real loop boundaries");
  for (uint32_t slot : {0u, 15u, 32u}) {
    StoreGuestU32(native->data(), kDevice + 56, kCursor + plan.PublicationWords(slot) * 4 - 4);
    Reject(ctx);
  }
  StoreGuestU32(native->data(), kDevice + 56, 0x2F000);
  StoreGuestU32(native->data(), kDevice + 52, plan.cursor - 4);
  Reject(ctx);
  for (uint32_t operand : {0u, 0x30000u, 0x40400u, 0x40800u, UINT32_MAX}) {
    uint32_t unchanged = 0x91E10DA5;
    Require(!predication::DecodeRegister(operand, unchanged) && unchanged == 0x91E10DA5,
            "Invalid conditional register accepted or changed decode output");
  }
}
}  // namespace

int main(int argc, char** argv) {
  try {
    original = std::make_unique<GuestMemoryFixture>(kSize);
    native = std::make_unique<GuestMemoryFixture>(kSize);
    for (uint32_t i = 0; i < kSize; ++i)
      pristine[i] = uint8_t(i * 37 + i / 7);
    uint32_t variant = 0;
    for (uint32_t flags = 0; flags < 32; ++flags)
      for (uint32_t mode = 0; mode < 8; ++mode)
        for (uint32_t streams : {0u, 1u, 2u, 5u, 16u, 17u, 32u, 33u, 453u, 0x80000001u, UINT32_MAX})
          for (uint32_t alignment : {0u, 4u, 8u, 12u})
            Check(flags, mode, streams, alignment, variant++);
    NegativeChecks();
    Require(reference_calls == cases, "Conditional oracle skipped its real body");
    char report[768];
    std::snprintf(
        report, sizeof(report),
        "{\"passed\":true,\"plan_bytes\":%u,\"cases\":%llu,\"mask_calls\":%llu,\"selected_states\":"
        "%llu,"
        "\"ordinary_states\":%llu,\"unchanged_rejections\":%llu,\"real_reference_calls\":%llu}\n",
        uint32_t(sizeof(port::ConditionalPlan)), (unsigned long long)cases,
        (unsigned long long)mask_calls, (unsigned long long)selected_states,
        (unsigned long long)ordinary_states, (unsigned long long)rejected,
        (unsigned long long)reference_calls);
    FILE* out = argc == 2 ? std::fopen(argv[1], "wb") : stdout;
    Require(out, "Cannot open conditional oracle output");
    std::fputs(report, out);
    if (out != stdout)
      Require(!std::fclose(out), "Cannot close conditional oracle output");
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "Conditional variant=%u streams=%u: %s\n", current_variant,
                 current_streams, e.what());
    return 1;
  }
}
