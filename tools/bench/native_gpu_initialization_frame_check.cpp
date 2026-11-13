// Compare the real initialization prefix at its allocator boundary, then the
// real continuation with supplied heap results. This does not test the heap.
#include "simpsons_init.h"
#include "native_gpu_initialization_frame.h"
#include "guest_memory_fixture.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <vector>

static uint64_t reference_calls = 0, copy_calls = 0, boundary_calls = 0;
struct AllocationBoundary {};
void allocation_boundary(PPCContext&, uint8_t*) {
  ++boundary_calls;
  throw AllocationBoundary{};
}
#include "native_gpu_initialization_frame_reference.inc"

namespace port = simpsons::native_gpu_calls;
namespace state = rex::graphics::native_gpu_state;
namespace program = rex::graphics::native_gpu_program;
namespace ports = rex::graphics::native_gpu_ports;
using simpsons::LoadGuestU32;
using simpsons::StoreGuestU32;

namespace {
constexpr uint32_t kSize = 1024 * 1024, kDevice = 0x1000, kStack = 0xF0000;
constexpr uint32_t kShader = 0x8206A0C8;
std::unique_ptr<GuestMemoryFixture> original, native;
std::array<uint8_t, kSize> pristine;
uint64_t prefixes = 0, continuations = 0, failures = 0, compatibility = 0, rejections = 0;
uint64_t records = 0;
uint32_t current_buffer = 0;

void Require(bool ok, const char* why) {
  if (!ok)
    throw std::runtime_error(why);
}

PPCContext Prepare(uint32_t seed, uint32_t buffer = 0x20000) {
  std::memcpy(original->data(), pristine.data(), kSize);
  for (uint32_t i = 0; i < 33; ++i)
    StoreGuestU32(original->data(), kShader + i * 4, seed * 0x91E10DA5u + i * 0x1020305u);
  for (uint32_t i = 0; i < 8320; i += 4)
    StoreGuestU32(original->data(), buffer - 64 + i, seed * 7 + i * 13);
  std::memcpy(native->data(), original->data(), kSize);
  std::memcpy(native->data() + kShader, original->data() + kShader, 132);
  for (uint32_t i = 0; i < 8320; i += 4)
    StoreGuestU32(native->data(), buffer - 64 + i, LoadGuestU32(original->data(), buffer - 64 + i));
  PPCContext ctx{};
  const std::array regs = {&PPCContext::r0,  &PPCContext::r1,  &PPCContext::r2,  &PPCContext::r3,
                           &PPCContext::r4,  &PPCContext::r5,  &PPCContext::r6,  &PPCContext::r7,
                           &PPCContext::r8,  &PPCContext::r9,  &PPCContext::r10, &PPCContext::r11,
                           &PPCContext::r12, &PPCContext::r13, &PPCContext::r14, &PPCContext::r15,
                           &PPCContext::r16, &PPCContext::r17, &PPCContext::r18, &PPCContext::r19,
                           &PPCContext::r20, &PPCContext::r21, &PPCContext::r22, &PPCContext::r23,
                           &PPCContext::r24, &PPCContext::r25, &PPCContext::r26, &PPCContext::r27,
                           &PPCContext::r28, &PPCContext::r29, &PPCContext::r30, &PPCContext::r31};
  for (uint32_t i = 0; i < regs.size(); ++i)
    (ctx.*regs[i]).u64 = 0xABCDEF0000000000ull | (seed * 0x1020305ull + i);
  ctx.r1.u64 = (seed & 1 ? 0xABCD000000000000ull : 0) | kStack;
  ctx.r3.u64 = (seed & 2 ? 0xFFFF000000000000ull : 0) | kDevice;
  ctx.lr = 0xDEADBEEF82123450ull | seed * 4;
  return ctx;
}

void EqualMemory(bool private_frame, uint32_t buffer) {
  Require(!std::memcmp(original->data(), native->data(), private_frame ? kStack - 328 : kSize),
          "Initialization changed memory outside its output/private frame");
  if (private_frame)
    Require(!std::memcmp(original->data() + kStack, native->data() + kStack, kSize - kStack),
            "Initialization changed memory above the caller stack");
  Require(!std::memcmp(original->data() + kShader, native->data() + kShader, 132),
          "Initialization modified its shader source");
  if (buffer >= kSize)
    for (uint32_t i = 0; i < 8320; i += 4)
      Require(LoadGuestU32(original->data(), buffer - 64 + i) ==
                  LoadGuestU32(native->data(), buffer - 64 + i),
              "Initialization changed an aliased output or its padding");
}

PPCContext CheckPrefix(uint32_t seed, uint32_t buffer) {
  auto expected = Prepare(seed, buffer), actual = expected;
  const auto before = boundary_calls;
  try {
    reference_prefix(expected, original->data());
    Require(false, "Reference missed its allocation boundary");
  } catch (const AllocationBoundary&) {}
  try {
    port::InitializeGraphicsBuffers(actual, native->data(), allocation_boundary,
                                    reference_savegprlr_28, reference_restgprlr_28,
                                    reference_sub_82444700, reference_sub_824449E8);
    Require(false, "Native prefix missed its allocation boundary");
  } catch (const AllocationBoundary&) {}
  Require(boundary_calls == before + 2 && !std::memcmp(&expected, &actual, sizeof(actual)),
          "Allocator call state differs from the real initialization prefix");
  Require(actual.r3.u64 == 8192 && actual.r4.s64 == -1249902592 && actual.lr == 0x82444A9C,
          "Allocator size, flags or return address changed");
  EqualMemory(false, buffer);
  ++prefixes;
  return expected;
}

void Normalize(uint32_t buffer, uint32_t first, uint32_t count, bool record = true) {
  for (uint32_t at = 0; at < count;) {
    const uint32_t address = buffer + (first + at) * 4;
    const uint32_t header = LoadGuestU32(native->data(), address);
    const uint32_t size = ((header >> 16) & 0x3FFF) + 1;
    Require(at + size < count, "Initialization record crosses a sub-buffer");
    const uint32_t opcode = (header >> 8) & 127;
    uint32_t old_header;
    if (state::IsOpcode(opcode)) {
      state::StateRange range;
      Require(state::DecodeHeader(header, size, range), "Invalid state record");
      old_header = ((size - 1) << 16) | range.first_register;
    } else if (opcode == program::kOpcode) {
      Require(program::IsValidHeader(header, size), "Invalid shader record");
      old_header =
          header == program::kInvalidateHeader ? 0xC0003B00 : 0xC0002B00 | ((size - 1) << 16);
    } else if (opcode == ports::kOpcode) {
      uint32_t reg;
      Require(ports::DecodeHeader(header, size, reg), "Invalid register port");
      old_header = ((size - 1) << 16) | reg | (size > 1 ? 0x8000 : 0);
    } else {
      Require(
          opcode == rex::graphics::native_gpu_calls::kBareDrawOpcode && size == 1 && !(header & 1),
          "Invalid warmup draw");
      old_header = 0xC0003600;
    }
    StoreGuestU32(native->data(), address, old_header);
    records += record;
    at += size + 1;
  }
}

void CheckContinuation(uint32_t seed, uint32_t buffer, uint64_t high, bool null_result,
                       bool compat) {
  current_buffer = buffer;
  auto expected = CheckPrefix(seed, buffer), actual = expected;
  // Inject the same result into both suspended calls. The allocator itself is
  // untouched in the runtime, and is outside this fixture's coverage.
  actual.r3.u64 = expected.r3.u64 = null_result ? high : high | buffer;
  actual.r7.u64 = expected.r7.u64 = seed * 0x1020305ull;
  reference_continuation(expected, original->data());
  const auto calls_before = reference_calls, copies_before = copy_calls;
  const auto boundaries_before = boundary_calls;
  port::FinishInitialization(actual, native->data(), reference_restgprlr_28, reference_sub_82444700,
                             reference_sub_824449E8);
  Require(boundaries_before == boundary_calls, "Continuation allocated a second block");
  Require(reference_calls - calls_before == (null_result || !compat ? 0 : 98) &&
              copy_calls - copies_before == (null_result || !compat ? 0 : 2),
          "Native initialization unexpectedly entered a compatibility builder");
  Require(actual.r1.u64 == expected.r1.u64 && actual.r3.u64 == expected.r3.u64 &&
              actual.lr == expected.lr && actual.fpscr.csr == expected.fpscr.csr,
          "Initialization changed its return, stack, link or FP state");
  for (auto reg :
       {&PPCContext::r14, &PPCContext::r15, &PPCContext::r16, &PPCContext::r17, &PPCContext::r18,
        &PPCContext::r19, &PPCContext::r20, &PPCContext::r21, &PPCContext::r22, &PPCContext::r23,
        &PPCContext::r24, &PPCContext::r25, &PPCContext::r26, &PPCContext::r27, &PPCContext::r28,
        &PPCContext::r29, &PPCContext::r30, &PPCContext::r31})
    Require((actual.*reg).u64 == (expected.*reg).u64, "Initialization changed a saved register");
  Require(!std::memcmp(&actual.cr2, &expected.cr2, sizeof(actual.cr2)) &&
              !std::memcmp(&actual.cr3, &expected.cr3, sizeof(actual.cr3)) &&
              !std::memcmp(&actual.cr4, &expected.cr4, sizeof(actual.cr4)),
          "Initialization changed a saved condition field");
  if (!null_result && !compat) {
    Require(actual.r3.u64 == 1 && LoadGuestU32(native->data(), kDevice + 13756) == buffer,
            "Initialization lost its owner or success result");
    Normalize(buffer, 0, 74);
    for (uint32_t i = 0; i < 97; ++i)
      Normalize(buffer, (320 + i * 64) / 4, 13);
    Normalize(buffer, 6528 / 4, 48);
  }
  EqualMemory(true, buffer);
  continuations += !null_result && !compat;
  failures += null_result;
  compatibility += compat;
}

void Rejections() {
  for (uint32_t probe = 0; probe < 8; ++probe) {
    auto ctx = Prepare(3);
    switch (probe) {
      case 0:
        ctx.r3.u32 += 1;
        break;
      case 1:
        ctx.r1.u32 += 4;
        break;
      case 2:
        ctx.r1.u32 = 320;
        break;
      case 3:
        ctx.r3.u32 = 0x7EFFFFFC;
        break;
      case 4:
        ctx.r3.u32 = kStack - 328;
        break;
      case 5:
        ctx.r3.u32 = 0x9206A0C8;
        break;
      case 6:
        ctx.r1.u32 = 0x8206A140;
        break;
      case 7:
        ctx.r1.u32 = 0x80000010;
        break;
    }
    const auto saved = ctx;
    const auto before = boundary_calls;
    Require(!port::InitializeGraphicsBuffers(ctx, probe == 7 ? nullptr : native->data(),
                                             allocation_boundary, reference_savegprlr_28,
                                             reference_restgprlr_28, reference_sub_82444700,
                                             reference_sub_824449E8),
            "Unsafe initialization frame did not fall back before allocation");
    Require(!std::memcmp(&saved, &ctx, sizeof(ctx)) && before == boundary_calls,
            "Rejected frame changed context or called its allocator");
    EqualMemory(false, 0x20000);
    ++rejections;
  }
}
}  // namespace

int main(int argc, char** argv) {
  try {
    original = std::make_unique<GuestMemoryFixture>(kSize);
    native = std::make_unique<GuestMemoryFixture>(kSize);
    for (auto* memory : {original.get(), native.get()}) {
      memory->Commit(kShader, 132);
      for (uint32_t address : {0xA0020000u, 0xC0020000u, 0xE001F000u})
        memory->Commit(address - 64, 16384);
    }
    for (uint32_t i = 0; i < kSize; ++i)
      pristine[i] = uint8_t(i * 37 + i / 7);
    for (uint32_t seed = 0; seed < 4; ++seed)
      for (uint32_t address : {0x20000u, 0xA0020000u, 0xC0020000u, 0xE001F000u})
        for (uint32_t alignment : {0u, 4u, 8u, 12u})
          for (uint64_t high : {0ull, 0xFFFFFFFF00000000ull})
            CheckContinuation(seed, address + alignment, high, false, false);
    for (uint32_t seed = 0; seed < 4; ++seed) {
      CheckContinuation(seed, 0x20000, seed & 1 ? 0xFFFFFFFF00000000ull : 0, true, false);
      CheckContinuation(seed, 0x20001, 0, false, true);
    }
    Rejections();
    Require(continuations == 128 && records == continuations * 331 &&
                prefixes == continuations + failures + compatibility &&
                copy_calls == (continuations + compatibility * 2) * 2,
            "Initialization frame coverage changed unexpectedly");
    char report[768];
    std::snprintf(report, sizeof(report),
                  "{\"passed\":true,\"allocation_boundary_comparisons\":%llu,"
                  "\"native_continuations\":%llu,\"allocation_failures\":%llu,"
                  "\"compatibility_continuations\":%llu,\"unchanged_rejections\":%llu,"
                  "\"native_records\":%llu,"
                  "\"allocator_executed\":false,\"real_memcpy_calls\":%llu}\n",
                  (unsigned long long)prefixes, (unsigned long long)continuations,
                  (unsigned long long)failures, (unsigned long long)compatibility,
                  (unsigned long long)rejections, (unsigned long long)records,
                  (unsigned long long)copy_calls);
    FILE* out = argc == 2 ? std::fopen(argv[1], "wb") : stdout;
    Require(out, "Cannot open initialization frame report");
    std::fputs(report, out);
    if (out != stdout)
      Require(!std::fclose(out), "Cannot close initialization frame report");
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "Initialization frame buffer=%08X: %s\n", current_buffer, e.what());
    return 1;
  }
}
