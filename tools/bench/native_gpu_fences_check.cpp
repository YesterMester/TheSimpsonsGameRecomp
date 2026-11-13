#include "simpsons_init.h"
#include "native_gpu_fences.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>

#include "native_gpu_fences_reference.inc"

namespace port = simpsons::native_gpu_calls;
using simpsons::LoadGuestU32;
using simpsons::StoreGuestU32;

namespace {
constexpr uint32_t kSize = 0x40000, kDevice = 0x1000, kCursor = 0x10000, kStack = 0x3F000;
std::array<uint8_t, kSize> original_memory, native_memory, pristine, untouched_memory;
constexpr std::array<PPCRegister PPCContext::*, 18> kNonvolatile = {
    &PPCContext::r14, &PPCContext::r15, &PPCContext::r16, &PPCContext::r17, &PPCContext::r18,
    &PPCContext::r19, &PPCContext::r20, &PPCContext::r21, &PPCContext::r22, &PPCContext::r23,
    &PPCContext::r24, &PPCContext::r25, &PPCContext::r26, &PPCContext::r27, &PPCContext::r28,
    &PPCContext::r29, &PPCContext::r30, &PPCContext::r31};
uint64_t cases = 0, events = 0, no_events = 0, rejected = 0;
uint32_t current_family = 0, current_input = 0, current_slot = 0;
void Require(bool ok, const char* why) {
  if (!ok)
    throw std::runtime_error(why);
}

PPCContext Prepare(uint32_t input, uint32_t state = 2, uint32_t slot = 0, uint32_t tail = 3,
                   uint32_t source = 0xA0040000) {
  original_memory = pristine;
  auto* base = original_memory.data();
  std::memset(base + kDevice, 0, 22256);
  StoreGuestU32(base, kDevice + 48, kCursor);
  StoreGuestU32(base, kDevice + 52, kCursor + 16);
  StoreGuestU32(base, kDevice + 56, kCursor);
  StoreGuestU32(base, kDevice + 21620, source);
  StoreGuestU32(base, kDevice + 21624, state);
  StoreGuestU32(base, kDevice + 21628, tail);
  StoreGuestU32(base, kDevice + 21632, slot);
  native_memory = original_memory;
  PPCContext ctx{};
  ctx.r1.u64 = 0xABCD000000000000ull | kStack;
  ctx.r3.u64 = 0xA5A5000000000000ull | kDevice;
  ctx.r4.u64 = 0xFEEDABCD00000000ull | input;
  ctx.lr = 0xDEADBEEF82345678ull;
  ctx.xer.ca = 1;
  ctx.cr0.compare<int32_t>(-2, 7, ctx.xer);
  ctx.cr6.compare<uint32_t>(2, 3, ctx.xer);
  for (uint32_t i = 0; i < kNonvolatile.size(); ++i)
    (ctx.*kNonvolatile[i]).u64 = 0x1234ABCD00000000ull | (i * 0x10203u);
  return ctx;
}

void Case(bool slots, uint32_t input, uint32_t state = 2, uint32_t slot = 0, uint32_t tail = 3,
          uint32_t source = 0xA0040000) {
  current_family = slots;
  current_input = input;
  current_slot = slot;
  auto original = Prepare(input, state, slot, tail, source), native = original;
  const auto saved = native;
  untouched_memory = native_memory;
  if (slots)
    reference_sub_824618F8(original, original_memory.data());
  else
    reference_sub_824617D8(original, original_memory.data());
  const bool emitted = LoadGuestU32(original_memory.data(), kDevice + 48) != kCursor;
  const uint32_t target = LoadGuestU32(original_memory.data(), kCursor + 12);
  const bool valid = !emitted || uint64_t(target & ~3u) + 4 <= 0x20000000ull;
  const bool accepted = slots ? port::WriteCounterSlotEvent(native, native_memory.data())
                              : port::WriteCounterEvent(native, native_memory.data());
  if (!valid) {
    Require(!accepted && !std::memcmp(&native, &saved, sizeof(native)) &&
                native_memory == untouched_memory,
            "Invalid physical target changed context/memory before fallback");
    ++rejected;
    return;
  }
  Require(accepted, "Ready counter event unexpectedly retained the original writer");
  Require(original.r1.u64 == native.r1.u64 && original.r3.u64 == native.r3.u64 &&
              original.lr == native.lr,
          "Counter event changed stack, return register or link ABI");
  for (auto reg : kNonvolatile)
    Require((original.*reg).u64 == (native.*reg).u64, "Counter event changed a nonvolatile GPR");
  if (emitted) {
    Require(LoadGuestU32(original_memory.data(), kCursor + 4) == 0xC0025800u &&
                LoadGuestU32(native_memory.data(), kCursor + 4) ==
                    rex::graphics::native_gpu_sync::kEventHeader,
            "Counter event lost its native synchronization header");
    StoreGuestU32(native_memory.data(), kCursor + 4, 0xC0025800u);
    ++events;
  } else {
    ++no_events;
  }
  Require(!std::memcmp(original_memory.data(), native_memory.data(), kStack - 112) &&
              !std::memcmp(original_memory.data() + kStack, native_memory.data() + kStack,
                           kSize - kStack),
          "Counter event changed state, target/endian/value, publication or neighboring memory");
  ++cases;
}

void Reject(bool slots, uint32_t problem) {
  auto ctx = Prepare(slots ? 1 : 0xA0040000), saved = ctx;
  auto* base = native_memory.data();
  switch (problem) {
    case 0:
      ctx.r3.u32 |= 1;
      break;
    case 1:
      ctx.r1.u32 |= 1;
      break;
    case 2:
      ctx.r1.u64 = 64;
      break;
    case 3:
      StoreGuestU32(base, kDevice + 48, kCursor + 1);
      break;
    case 4:
      StoreGuestU32(base, kDevice + 52, kCursor + 15);
      break;
    case 5:
      StoreGuestU32(base, kDevice + 56, kCursor - 4);
      break;
    case 6:
      *simpsons::GuestToHost(base, kDevice + 10940) = 1;
      break;
    case 7:
      StoreGuestU32(base, kDevice + 13216, 1);
      break;
    case 8:
      StoreGuestU32(base, kDevice + 13492, 1);
      break;
    case 9:
      StoreGuestU32(base, kDevice + 48, kDevice + 128);
      StoreGuestU32(base, kDevice + 52, kDevice + 160);
      StoreGuestU32(base, kDevice + 56, kDevice + 160);
      break;
    case 10:
      StoreGuestU32(base, kDevice + 48, kStack - 32);
      StoreGuestU32(base, kDevice + 52, kStack);
      StoreGuestU32(base, kDevice + 56, kStack);
      break;
    case 11:
      ctx.r4.u64 = 0xFFFFF000;
      StoreGuestU32(base, kDevice + 21620, 0xFFFFEFF8);
      break;
    case 12:
      ctx.r4.u64 = 0x4004;
      StoreGuestU32(base, kDevice + 21620, 0x3FFC);
      StoreGuestU32(base, kDevice + 48, 0xC0004000);
      StoreGuestU32(base, kDevice + 52, 0xC0004020);
      StoreGuestU32(base, kDevice + 56, 0xC0004020);
      break;
  }
  saved = ctx;
  original_memory = native_memory;
  Require(!(slots ? port::WriteCounterSlotEvent(ctx, base) : port::WriteCounterEvent(ctx, base)) &&
              !std::memcmp(&ctx, &saved, sizeof(ctx)) && original_memory == native_memory,
          "Unsafe counter event changed memory/context or emitted a partial record");
  ++rejected;
}
}  // namespace

int main(int argc, char** argv) {
  if (argc > 2 || (argc == 2 && !std::freopen(argv[1], "w", stdout)))
    return 2;
  try {
    for (uint32_t i = 0; i < kSize; ++i)
      pristine[i] = uint8_t(i * 37 + (i >> 6));
    constexpr std::array<uint32_t, 20> addresses = {
        0,          1,          2,          3,          4,          0x1000,     0x0FFFFFFF,
        0x1FFFF000, 0x20000000, 0x7F012345, 0x80010000, 0x90010002, 0xA0040000, 0xBFFFFFFC,
        0xC0040003, 0xDFFFFFFC, 0xE0040000, 0xFFF00000, 0xFFFFF000, UINT32_MAX};
    for (uint32_t address : addresses)
      Case(false, address);
    for (uint32_t state : {0u, 1u, 2u, 3u, UINT32_MAX})
      for (uint32_t slot : {0u, 1u, 2u, 3u, 4u, 0x7FFFFFFFu, UINT32_MAX})
        for (uint32_t tail : {0u, 1u, 3u, 4u, UINT32_MAX})
          for (uint32_t input : {0u, 1u, UINT32_MAX})
            for (uint32_t source : addresses)
              Case(true, input, state, slot, tail, source);
    for (bool slots : {false, true})
      for (uint32_t problem = 0; problem < 13; ++problem)
        Reject(slots, problem);
    std::printf(
        "{\"passed\":true,\"cases\":%llu,\"native_events\":%llu,\"no_event_cases\":%llu,"
        "\"unchanged_fallbacks\":%llu}\n",
        (unsigned long long)cases, (unsigned long long)events, (unsigned long long)no_events,
        (unsigned long long)rejected);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s; family=%u input=%08X slot=%08X\n", error.what(), current_family,
                 current_input, current_slot);
    return 1;
  }
}
