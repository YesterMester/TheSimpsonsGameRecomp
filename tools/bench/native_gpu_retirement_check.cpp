// Execute the original post-wait construction segment, then compare its full
// continuation context and memory with the native writer. No game is launched.
#include "simpsons_init.h"
#include "native_gpu_retirement.h"
#include "guest_memory_fixture.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>

#include "native_gpu_retirement_reference.inc"

namespace port = simpsons::native_gpu_calls;
namespace state = rex::graphics::native_gpu_state;
namespace gpu_sync = rex::graphics::native_gpu_sync;
using simpsons::LoadGuestU32;
using simpsons::StoreGuestU32;

namespace {
constexpr uint32_t kSize = 1024 * 1024, kDescriptor = 0x1000, kOwner = 0x5000;
constexpr uint32_t kRoot = 0x7000, kStack = 0xE0000, kGlobal = 0x82000710;
std::unique_ptr<GuestMemoryFixture> original, native;
std::array<uint8_t, kSize> pristine;
uint64_t cases = 0, rejections = 0, malformed = 0;
uint32_t current_buffer = 0;

void Require(bool ok, const char* why) {
  if (!ok)
    throw std::runtime_error(why);
}

PPCContext Prepare(uint32_t buffer, uint64_t sequence, uint32_t endian, uint32_t extra) {
  std::memcpy(original->data(), pristine.data(), kSize);
  for (uint32_t i = 0; i < 512; i += 4)
    StoreGuestU32(original->data(), buffer - 64 + i, extra * 0x91E10DA5u + i);
  StoreGuestU32(original->data(), kGlobal, kRoot);
  StoreGuestU32(original->data(), kRoot, 0x13579BDF);
  StoreGuestU32(original->data(), kDescriptor + 20, buffer + 384 + extra);
  std::memcpy(native->data(), original->data(), kSize);
  StoreGuestU32(native->data(), kGlobal, kRoot);
  for (uint32_t i = 0; i < 512; i += 4)
    StoreGuestU32(native->data(), buffer - 64 + i, LoadGuestU32(original->data(), buffer - 64 + i));
  PPCContext ctx{};
  ctx.r1.u64 = 0xABCD000000000000ull | kStack;
  ctx.r3.u64 = 0x1020304050607080ull;
  ctx.r13.u64 = 0x9010;
  ctx.r18.u64 = buffer + 768;
  ctx.r19.u64 = 3;
  ctx.r20.u64 = 0xA030;
  ctx.r21.u64 = 0x10000 | endian;
  ctx.r22.u64 = kOwner;
  ctx.r23.u64 = 0xA31;
  ctx.r24.s64 = -2113929216;
  ctx.r25.u64 = 0x41000000;
  ctx.r26.u64 = uint64_t(buffer) + 384;
  ctx.r27.u64 = 0x123456789ABCDEF0;
  ctx.r28.u64 = sequence;
  ctx.r29.u64 = 0xFFFF000000000000ull | kDescriptor;
  ctx.r30.u64 = 0;
  ctx.r31.u64 = buffer;
  ctx.lr = 0x82463F24;
  return ctx;
}

void EqualMemory(uint32_t buffer) {
  Require(!std::memcmp(original->data(), native->data(), kSize) &&
              LoadGuestU32(original->data(), kGlobal) == LoadGuestU32(native->data(), kGlobal),
          "Retirement changed guest memory outside its records");
  if (buffer >= kSize)
    for (uint32_t i = 0; i < 512; i += 4)
      Require(LoadGuestU32(original->data(), buffer - 64 + i) ==
                  LoadGuestU32(native->data(), buffer - 64 + i),
              "Retirement changed an aliased record, payload or padding");
}

void Check(uint32_t buffer, uint64_t sequence, uint32_t endian, uint32_t extra) {
  current_buffer = buffer;
  auto expected = Prepare(buffer, sequence, endian, extra), actual = expected;
  reference_writer(expected, original->data());
  Require(port::WriteRetirementChunk(actual, native->data()), "Valid retirement span fell back");
  Require(!std::memcmp(&actual, &expected, sizeof(actual)),
          "Retirement changed the complete continuation context");
  uint32_t header;
  Require(state::EncodeHeader(0x48BA, 6, header) && LoadGuestU32(native->data(), buffer) == header,
          "Retirement lost its native fetch range");
  StoreGuestU32(native->data(), buffer, 0x548BA);
  Require(
      state::EncodeHeader(0x5000, 3, header) && LoadGuestU32(native->data(), buffer + 72) == header,
      "Retirement lost its flush range");
  StoreGuestU32(native->data(), buffer + 72, 0x25000);
  std::array<uint32_t, 10> payload;
  for (uint32_t i = 0; i < payload.size(); ++i)
    payload[i] = LoadGuestU32(native->data(), buffer + (8 + i) * 4);
  gpu_sync::Coherency call;
  Require(LoadGuestU32(native->data(), buffer + 28) == gpu_sync::kCoherencyHeader &&
              gpu_sync::Decode(payload, call),
          "Retirement lost its native coherency call");
  StoreGuestU32(native->data(), buffer + 28, 0xA31);
  StoreGuestU32(native->data(), buffer + 36, 0x10A2F);
  StoreGuestU32(native->data(), buffer + 48, 0xC0043C00);
  Require(LoadGuestU32(native->data(), buffer + 88) ==
              (0xC0000000u | (rex::graphics::native_gpu_calls::kBareDrawOpcode << 8)),
          "Retirement emitted an original draw header");
  StoreGuestU32(native->data(), buffer + 88, 0xC0003600);
  Require(LoadGuestU32(native->data(), buffer + 96) == gpu_sync::kEventHeader,
          "Retirement emitted an original shader-event header");
  StoreGuestU32(native->data(), buffer + 96, 0xC0025800);
  EqualMemory(buffer);
  ++cases;
}

void Rejections() {
  for (uint32_t probe = 0; probe < 14; ++probe) {
    auto ctx = Prepare(0x20000, 3, 0, 0);
    switch (probe) {
      case 0:
        ctx.r31.u32 += 1;
        break;
      case 1:
        ctx.r31.u32 = 0x7EFFFFFC;
        break;
      case 2:
        ctx.r31.u32 = kDescriptor;
        break;
      case 3:
        ctx.r31.u32 = kOwner;
        break;
      case 4:
        ctx.r31.u32 = kStack;
        break;
      case 5:
        ctx.r31.u32 = 0x92000710;
        break;
      case 6:
        ctx.r31.u32 = kRoot;
        break;
      case 7:
        ctx.r1.u32 += 4;
        break;
      case 8:
        ctx.r22.u32 += 1;
        break;
      case 9:
        ctx.r30.u64 = 1;
        break;
      case 10:
        ctx.r25.u64 += 1;
        break;
      case 11:
        ctx.r23.u64 += 1;
        break;
      case 12:
        ctx.r24.u64 += 1;
        break;
      case 13:
        ctx.r29.u32 += 1;
        break;
    }
    const auto saved = ctx;
    Require(
        !port::WriteRetirementChunk(ctx, native->data()) && !std::memcmp(&saved, &ctx, sizeof(ctx)),
        "Rejected retirement span changed context");
    EqualMemory(0x20000);
    ++rejections;
  }
  for (uint32_t header : {gpu_sync::kCoherencyHeader, gpu_sync::kEventHeader})
    for (uint32_t mask : {1u, 4u, 0x80u, 0x8000u, 0x10000u}) {
      const uint32_t count = ((header >> 16) & 0x3FFF) + 1;
      Require(!gpu_sync::IsValidHeader(header ^ mask, count),
              "Malformed synchronization header accepted");
      ++malformed;
    }
  auto words = gpu_sync::Encode({0x5000000, 384, 0x1000});
  for (uint32_t index : {2u, 5u, 6u, 7u, 8u, 9u, 10u}) {
    auto invalid = words;
    invalid[index] ^= 1;
    gpu_sync::Coherency call{1, 2, 3};
    Require(!gpu_sync::Decode(std::span(invalid).subspan(1), call) && call.status == 1 &&
                call.size == 2 && call.base == 3,
            "Malformed synchronization payload accepted or changed decode output");
    ++malformed;
  }
}
}  // namespace

int main(int argc, char** argv) {
  try {
    original = std::make_unique<GuestMemoryFixture>(kSize);
    native = std::make_unique<GuestMemoryFixture>(kSize);
    for (auto* memory : {original.get(), native.get()}) {
      memory->Commit(kGlobal, 4);
      for (uint32_t address : {0xA0020000u, 0xC0020000u, 0xE001F000u})
        memory->Commit(address - 64, 16384);
    }
    for (uint32_t i = 0; i < kSize; ++i)
      pristine[i] = uint8_t(i * 37 + i / 7);
    for (uint32_t address : {0x20000u, 0xA0020000u, 0xC0020000u, 0xE001F000u})
      for (uint32_t alignment : {0u, 4u, 8u, 12u})
        for (uint64_t sequence : {0ull, 1ull, 2ull, 3ull, 7ull, 0xFFFFFFFFull, 0x100000000ull})
          for (uint32_t endian = 0; endian < 4; ++endian)
            for (uint32_t extra : {0u, 0x1800u})
              Check(address + alignment, sequence, endian, extra);
    Rejections();
    char report[512];
    std::snprintf(report, sizeof(report),
                  "{\"passed\":true,\"cases\":%llu,\"unchanged_rejections\":%llu,"
                  "\"malformed_records\":%llu,\"native_records\":%llu,"
                  "\"original_poll_lock_submit_executed\":false}\n",
                  (unsigned long long)cases, (unsigned long long)rejections,
                  (unsigned long long)malformed, (unsigned long long)(cases * 5));
    FILE* out = argc == 2 ? std::fopen(argv[1], "wb") : stdout;
    Require(out, "Cannot open retirement report");
    std::fputs(report, out);
    if (out != stdout)
      Require(!std::fclose(out), "Cannot close retirement report");
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "Retirement buffer=%08X: %s\n", current_buffer, e.what());
    return 1;
  }
}
