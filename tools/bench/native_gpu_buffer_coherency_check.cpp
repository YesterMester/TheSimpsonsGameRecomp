// Compare the native range writers with the current generated functions. The
// extracted originals and their stack helpers are kept outside the checkout.
#include "simpsons_init.h"
#include "native_gpu_buffer_coherency.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>

struct GrowthRequired {};
static uint64_t growth_calls = 0;
REX_FUNC(sub_82457CC8) {
  (void)ctx;
  (void)base;
  ++growth_calls;
  throw GrowthRequired{};
}
#include "native_gpu_buffer_coherency_reference.inc"

namespace {
using simpsons::LoadGuestU32;
using simpsons::StoreGuestU32;
namespace codec = rex::graphics::native_gpu_sync;
constexpr uint32_t kDevice = 0x1000, kOutput = 0x10000, kStack = 0x30000;
using Memory = std::array<uint8_t, 0x40000>;
Memory original_memory, native_memory;
constexpr std::array<PPCRegister PPCContext::*, 32> kGpr = {
    &PPCContext::r0, &PPCContext::r1, &PPCContext::r2, &PPCContext::r3,
    &PPCContext::r4, &PPCContext::r5, &PPCContext::r6, &PPCContext::r7,
    &PPCContext::r8, &PPCContext::r9, &PPCContext::r10, &PPCContext::r11,
    &PPCContext::r12, &PPCContext::r13, &PPCContext::r14, &PPCContext::r15,
    &PPCContext::r16, &PPCContext::r17, &PPCContext::r18, &PPCContext::r19,
    &PPCContext::r20, &PPCContext::r21, &PPCContext::r22, &PPCContext::r23,
    &PPCContext::r24, &PPCContext::r25, &PPCContext::r26, &PPCContext::r27,
    &PPCContext::r28, &PPCContext::r29, &PPCContext::r30, &PPCContext::r31};

void Require(bool value, const char* message) {
  if (!value)
    throw std::runtime_error(message);
}

PPCContext Context(uint32_t address, uint32_t length) {
  PPCContext ctx{};
  for (uint32_t i = 0; i < kGpr.size(); ++i)
    (ctx.*kGpr[i]).u64 = 0x1234567800000000ull | (i * 0x321321u);
  ctx.r1.u64 = 0xABCD000000000000ull | kStack;
  ctx.r3.u64 = 0xA5A5000000000000ull | kDevice;
  ctx.r4.u64 = 0xFACE000000000000ull | address;
  ctx.r5.u64 = 0xC0DE000000000000ull | length;
  ctx.lr = 0xCDEF000082345678ull;
  return ctx;
}

void Prepare(uint32_t cursor, uint32_t limit, uint32_t guard) {
  original_memory.fill(0xA7);
  StoreGuestU32(original_memory.data(), kDevice + 48, cursor);
  StoreGuestU32(original_memory.data(), kDevice + 52, limit);
  StoreGuestU32(original_memory.data(), kDevice + 56, guard);
  native_memory = original_memory;
}

void Original(PPCContext& ctx, uint8_t* base, bool writeback) {
  if (writeback)
    reference_sub_824415D0(ctx, base);
  else
    reference_sub_8244E208(ctx, base);
}

void Compare(bool writeback, uint32_t address, uint32_t length, uint32_t cursor,
             uint32_t headroom, bool exact_guard) {
  Prepare(cursor, cursor + headroom, cursor + (exact_guard ? 0 : 128));
  auto native = Context(address, length), original = native;
  Original(original, original_memory.data(), writeback);
  Require(simpsons::native_gpu_calls::WriteBufferCoherency(native, native_memory.data(), writeback),
          "Ready original range did not use the native writer");
  std::array<uint32_t, 10> payload;
  for (uint32_t i = 0; i < payload.size(); ++i)
    payload[i] = LoadGuestU32(native_memory.data(), cursor + 8 + i * 4);
  codec::Coherency call;
  Require(LoadGuestU32(native_memory.data(), cursor + 4) == codec::kCoherencyHeader &&
              codec::Decode(payload, call), "Native coherency span is malformed");
  Require(call.status == LoadGuestU32(original_memory.data(), cursor + 8) &&
              call.size == LoadGuestU32(original_memory.data(), cursor + 16) &&
              call.base == LoadGuestU32(original_memory.data(), cursor + 20),
          "Native range changed physical-page arithmetic or status bits");
  // Only the typed record's three structural words differ. All status, page,
  // mask and polling operands keep their original offsets and values.
  for (uint32_t offset : {4u, 12u, 24u})
    StoreGuestU32(native_memory.data(), cursor + offset,
                  LoadGuestU32(original_memory.data(), cursor + offset));
  Require(native_memory == original_memory,
          "Native writer changed a save slot, cursor, canary or command operand");
  for (auto reg : kGpr)
    Require((native.*reg).u64 == (original.*reg).u64,
            "Native writer changed an original general-purpose register result");
  Require(native.lr == original.lr && native.ctr.u64 == original.ctr.u64 &&
              native.xer.ca == original.xer.ca && native.xer.ov == original.xer.ov,
          "Native writer changed the return link, counter or arithmetic flags");
}

void Reject(PPCContext input, bool writeback) {
  const auto memory = native_memory;
  auto ctx = input;
  Require(!simpsons::native_gpu_calls::WriteBufferCoherency(ctx, native_memory.data(), writeback),
          "Unproved range unexpectedly used the native writer");
  Require(memory == native_memory && !std::memcmp(&ctx, &input, sizeof(ctx)),
          "Failed preflight changed memory or context before fallback");
}

void CompareUnlock(uint32_t first, uint32_t end, uint32_t status, uint32_t cursor) {
  Prepare(cursor, cursor + 44, cursor);
  StoreGuestU32(original_memory.data(), kStack + 260, status);
  native_memory = original_memory;
  auto original = Context(first, end);
  original.r31 = original.r3;
  original.r30.u64 = original.r4.u64;
  original.r29.u64 = original.r5.u64;
  original.r11.u64 = 0xBEAF000000000000ull | cursor;
  auto native = original;
  reference_unlock_flush(original, original_memory.data());
  Require(simpsons::native_gpu_calls::WriteUnlockCoherency(native, native_memory.data()),
          "The original unlock flush did not use its native checkpoint");
  std::array<uint32_t, 10> payload;
  for (uint32_t i = 0; i < payload.size(); ++i)
    payload[i] = LoadGuestU32(native_memory.data(), cursor + 8 + i * 4);
  codec::Coherency call;
  Require(LoadGuestU32(native_memory.data(), cursor + 4) == codec::kCoherencyHeader &&
              codec::Decode(payload, call) && call.status == status &&
              call.size == LoadGuestU32(original_memory.data(), cursor + 16) &&
              call.base == LoadGuestU32(original_memory.data(), cursor + 20),
          "Native unlock changed a flush operand");
  for (uint32_t offset : {4u, 12u, 24u})
    StoreGuestU32(native_memory.data(), cursor + offset,
                  LoadGuestU32(original_memory.data(), cursor + offset));
  Require(native_memory == original_memory && !std::memcmp(&native, &original, sizeof(native)),
          "Native unlock changed memory or a continuation register");
}

uint64_t RejectUnlock() {
  uint64_t count = 0;
  auto input = Context(0, 0);
  input.r31.u64 = kDevice;
  const auto reject = [&](PPCContext ctx) {
    const auto before = ctx;
    const auto memory = native_memory;
    Require(!simpsons::native_gpu_calls::WriteUnlockCoherency(ctx, native_memory.data()),
            "An unproved unlock range used the native checkpoint");
    Require(memory == native_memory && !std::memcmp(&ctx, &before, sizeof(ctx)),
            "The unlock checkpoint changed memory or registers before fallback");
    ++count;
  };
  for (uint32_t cursor : {0u, kOutput + 1, kDevice + 16, kStack + 240,
                          0x7FFFFFFCu, 0x8FFFFFFCu, 0xFFFFFFFCu}) {
    Prepare(cursor, UINT32_MAX, cursor);
    auto ctx = input; ctx.r11.u64 = cursor;
    reject(ctx);
  }
  Prepare(kOutput, kOutput + 40, kOutput);
  input.r11.u64 = kOutput;
  reject(input);
  Prepare(kOutput + 4, kOutput + 128, kOutput);
  reject(input);
  Prepare(kOutput, kOutput + 44, kOutput);
  for (uint32_t stack : {0xFFFFFFF0u, 0x7FFFFFF0u, kDevice, kStack + 1}) {
    auto ctx = input; ctx.r1.u64 = stack;
    reject(ctx);
  }
  return count;
}

uint64_t Rejections() {
  uint64_t count = 0;
  for (bool writeback : {false, true}) {
    const auto input = Context(0xE0030000, 4096);
    Prepare(kOutput, kOutput + 44, kOutput - 4);
    Reject(input, writeback); ++count;
    auto original = input;
    const uint64_t before = growth_calls;
    bool grew = false;
    try { Original(original, original_memory.data(), writeback); }
    catch (const GrowthRequired&) { grew = true; }
    Require(grew && growth_calls == before + 1, "Original growth boundary was not observed");
    for (uint32_t room : {0u, 4u, 40u}) {
      Prepare(kOutput, kOutput + room, kOutput);
      Reject(input, writeback); ++count;
    }
    for (uint32_t cursor : {0u, kOutput + 1, kOutput + 2, kDevice + 16,
                            kStack - 64, 0x7FFFFFFCu, 0x8FFFFFFCu, 0xFFFFFFFCu}) {
      Prepare(cursor, UINT32_MAX - 3, UINT32_MAX - 3);
      Reject(input, writeback); ++count;
    }
    Prepare(kOutput, kOutput + 44, kOutput);
    for (uint32_t device : {kDevice + 1, 0x8FFFF000u, 0xFFFFFFF0u, kStack - 96}) {
      auto ctx = input; ctx.r3.u64 = device;
      Reject(ctx, writeback); ++count;
    }
    for (uint32_t stack : {0u, 16u, kStack + 1, kDevice + 128, 0x80000010u}) {
      auto ctx = input; ctx.r1.u64 = stack;
      Reject(ctx, writeback); ++count;
    }
    auto ctx = input;
    Require(!simpsons::native_gpu_calls::WriteBufferCoherency(ctx, nullptr, writeback),
            "A missing guest base was accepted"); ++count;
  }
  return count;
}
}  // namespace

int main(int argc, char** argv) {
  try {
    uint64_t compared = 0;
    constexpr std::array addresses = {
        0u, 1u, 0xFFFu, 0x1000u, 0x0314C000u, 0x03FF7000u, 0x1FFFFFFFu,
        0x7F001000u, 0x80000000u, 0x82001234u, 0x8FFFFFFFu, 0x90001234u,
        0x9FFFFFFFu, 0xA0000000u, 0xA0010003u, 0xBFFFFFFFu, 0xC0010003u,
        0xDFFFFFFFu, 0xE0010003u, 0xEFFFFFFFu, 0xFFFFF000u, 0xFFFFFFFFu};
    constexpr std::array lengths = {0u, 1u, 4u, 4095u, 4096u, 4097u, 0x40000u,
                                    0x385000u, 0x1FFFFFFFu, 0xFFFFFFFFu};
    for (bool writeback : {false, true})
      for (uint32_t address : addresses)
        for (uint32_t length : lengths)
          for (uint32_t cursor : {kOutput, kOutput + 4})
            for (uint32_t room : {44u, 48u, 256u})
              for (bool exact_guard : {false, true}) {
                Compare(writeback, address, length, cursor, room, exact_guard);
                ++compared;
              }
    uint64_t unlock = 0;
    for (uint32_t first : addresses)
      for (uint32_t length : lengths)
        for (uint32_t status : {0u, 0x03000000u, 0x03000100u, 0x03000200u, UINT32_MAX})
          for (uint32_t cursor : {kOutput, kOutput + 4}) {
            CompareUnlock(first, first + length, status, cursor);
            ++unlock;
          }
    const auto rejected = Rejections(), unlock_rejected = RejectUnlock();
    char report[512];
    std::snprintf(report, sizeof(report),
                  "{\"passed\":true,\"original_native_cases\":%llu,\"unchanged_rejections\":%llu,"
                  "\"unlock_original_native_cases\":%llu,\"unlock_unchanged_rejections\":%llu,"
                  "\"observed_growth_boundaries\":%llu,\"rendered_frames\":0}\n",
                  static_cast<unsigned long long>(compared), static_cast<unsigned long long>(rejected),
                  static_cast<unsigned long long>(unlock), static_cast<unsigned long long>(unlock_rejected),
                  static_cast<unsigned long long>(growth_calls));
    if (argc > 1) {
      FILE* file = std::fopen(argv[1], "wb");
      Require(file != nullptr, "Cannot open the requested report");
      std::fputs(report, file);
      std::fclose(file);
    }
    std::fputs(report, stdout);
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "%s\n", e.what());
    return 1;
  }
}
