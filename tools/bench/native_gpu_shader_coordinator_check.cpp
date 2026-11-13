#include "simpsons_init.h"
#include "guest_memory_fixture.h"
#include "native_gpu_shader_coordinator.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <vector>

static uint64_t body_calls = 0, reserve_calls = 0, copy_calls = 0, link_calls = 0;
static uint64_t declaration_calls = 0, sync_calls = 0, coordinator_calls = 0;
static uint64_t control_calls = 0, constant_calls = 0, resource_calls = 0;
#include "native_gpu_shader_coordinator_reference.inc"

namespace port = simpsons::native_gpu_calls;
using simpsons::GuestToHost;
using simpsons::LoadGuestU32;
using simpsons::StoreGuestU32;

namespace {
constexpr uint32_t kSize = 0x200000, kDevice = 0x1000, kVertex = 0x20000;
constexpr uint32_t kDeclaration = 0x40000, kPixel = 0x70000, kSurface = 0x80000;
constexpr uint32_t kCursor = 0xA0000, kFence = 0x90000, kStack = 0x1F0000;
constexpr uint32_t kSource = 0xC0100000, kCodeSpan = 0x30000;
constexpr uint32_t kTables = port::vertex_declaration_detail::kSwizzleTable;
constexpr uint32_t kCounter = port::shader_resource_detail::kVersionCounter;
constexpr uint32_t kDebug = 0x82D5155C;
constexpr std::array<PPCRegister PPCContext::*, 18> kNonvolatile = {
    &PPCContext::r14, &PPCContext::r15, &PPCContext::r16, &PPCContext::r17, &PPCContext::r18,
    &PPCContext::r19, &PPCContext::r20, &PPCContext::r21, &PPCContext::r22, &PPCContext::r23,
    &PPCContext::r24, &PPCContext::r25, &PPCContext::r26, &PPCContext::r27, &PPCContext::r28,
    &PPCContext::r29, &PPCContext::r30, &PPCContext::r31};
std::unique_ptr<GuestMemoryFixture> original, native;
std::array<uint8_t, kSize> pristine;
uint64_t cases = 0, rejects = 0, native_records = 0, null_vertex_cases = 0;
uint64_t conditional_cases = 0, inline_cases = 0, version_cases = 0;
uint64_t pending_version_cases = 0, pending_busy_cases = 0;
uint64_t retained_version_cases = 0, retained_cache_hits = 0, retained_busy_hits = 0;
uint64_t capacity_reference_checks = 0, unused_retained_cases = 0;
uint32_t current_seed = 0, current_flags = 0, current_mode = 0;

void Require(bool ok, const char* why) {
  if (!ok)
    throw std::runtime_error(why);
}

void Same(uint32_t address, uint32_t bytes) {
  auto* a = GuestToHost(original->data(), address);
  auto* b = GuestToHost(native->data(), address);
  if (std::memcmp(a, b, bytes)) {
    for (uint32_t i = 0; i < bytes; ++i)
      if (a[i] != b[i]) {
        std::fprintf(stderr, "Difference at %08X: expected=%02X actual=%02X\n", address + i, a[i],
                     b[i]);
        break;
      }
    throw std::runtime_error("Coordinator changed operands, code, state or neighboring data");
  }
}

void CopyToNative() {
  std::memcpy(native->data(), original->data(), kSize);
  for (auto [address, size] : {std::pair{kSource, kCodeSpan}, {kTables, 80u}, {kDebug, 8u}})
    std::memcpy(GuestToHost(native->data(), address), GuestToHost(original->data(), address), size);
}

// Modes exercise actual cached, rebuild, busy, direct and relink branches.
// All child calls in the oracle execute the extracted generated implementations.
PPCContext Prepare(uint64_t flags, uint32_t mode, uint32_t seed) {
  current_seed = seed;
  current_mode = mode;
  current_flags = uint32_t(flags);
  auto* base = original->data();
  std::memcpy(base, pristine.data(), kSize);
  std::memset(base + kDevice, 0, 22256);
  std::memset(base + kVertex, 0, 0xA000);
  std::memset(base + kPixel, 0, 0x4000);
  std::memset(base + kDeclaration, 0, 4096);
  StoreGuestU32(base, kDevice + 48, kCursor);
  StoreGuestU32(base, kDevice + 52, kCursor + 16384);
  StoreGuestU32(base, kDevice + 56, kCursor + 16384);
  StoreGuestU32(base, kDevice + 12688, mode == 0 ? 0 : kVertex);
  StoreGuestU32(base, kDevice + 12684, mode == 1 || mode == 2 ? 0 : kPixel);
  StoreGuestU32(base, kDevice + 11812, mode == 3 || mode == 0 ? 0 : kDeclaration);
  StoreGuestU32(base, kDevice + 12432, kSurface);
  StoreGuestU32(base, kSurface + 28, 0x91E10DA5);
  StoreGuestU32(base, kDevice + 10580, 0xABCD1200 | (seed & 7));
  StoreGuestU32(base, kDevice + 10372, seed * 0x91E10DA5u);
  StoreGuestU32(base, kDevice + 10528, 0x5A123456);
  StoreGuestU32(base, kDevice + 10532, 0x7C91E10D);
  StoreGuestU32(base, kDevice + 10556, seed & 1 ? 8 : 0);
  StoreGuestU32(base, kDevice + 12692, seed & 2 ? 0xABCD1205 : 0xABCD1204);
  *GuestToHost(base, kDevice + 10940) = uint8_t((seed & 4 ? 0x40 : 0) | (mode == 4 ? 0x80 : 0));
  *GuestToHost(base, kDevice + 10942) = uint8_t((seed & 8 ? 8 : 0) | (mode == 5 ? 0x80 : 0));
  *GuestToHost(base, kDevice + 10943) = seed & 16 ? 4 : 0;
  *GuestToHost(base, kDevice + 10941) = seed & 1 ? 4 : 0;
  StoreGuestU32(base, kDevice + 13488, seed & 32 ? 1 : 0);
  StoreGuestU32(base, kDevice + 11844, (seed & 1 ? 1u : 2u) << 20);
  StoreGuestU32(base, kDevice + 10548, seed & 1 ? 0 : 0x36);
  StoreGuestU32(base, kDevice + 10560, seed * 17);
  StoreGuestU32(base, kDevice + 12700, seed * 0x1020305u);
  StoreGuestU32(base, kDevice + 12704, seed * 0x71E10DA5u);
  StoreGuestU32(base, kDevice + 10908, 100);
  StoreGuestU32(base, kDevice + 10896, kFence);
  StoreGuestU32(base, kFence, 70);
  for (uint32_t i = 0; i < 96; ++i)
    *GuestToHost(base, kDevice + 12520 + i) = uint8_t(seed + i * 3);
  const uint32_t count = std::array<uint32_t, 4>{0, 1, 7, 32}[seed & 3];
  StoreGuestU32(base, kDeclaration + 24, 7);
  StoreGuestU32(base, kDeclaration + 48, mode == 8 ? 0 : 7);
  for (uint32_t i = 0; i < 7; ++i) {
    StoreGuestU32(base, kDeclaration + 52 + i * 12, (i << 16) | i * 4);
    StoreGuestU32(base, kDeclaration + 56 + i * 12, seed * 123 + i);
    StoreGuestU32(base, kDeclaration + 60 + i * 12, i << 16);
  }
  StoreGuestU32(base, kVertex + 32, kSource);
  StoreGuestU32(base, kVertex + 872, mode == 2 || mode == 9 ? 0x20 : mode == 10 ? 0x40 : 0);
  StoreGuestU32(base, kPixel + 24, kSource + 0x20000);
  StoreGuestU32(base, kPixel + 64, 0x1000);
  const uint32_t pixel_metadata = kPixel + 0x1000 + 40;
  StoreGuestU32(base, pixel_metadata, 0);
  StoreGuestU32(base, pixel_metadata + 4, 384);
  StoreGuestU32(base, pixel_metadata + 8, (seed % 5) << 28 | (seed & 31));
  StoreGuestU32(base, pixel_metadata + 12, seed * 17);
  StoreGuestU32(base, pixel_metadata + 20, mode == 6 ? (2 << 5 | 1) : 0);
  StoreGuestU32(base, pixel_metadata + 24, seed * 0x1234);
  StoreGuestU32(base, pixel_metadata + 28, seed & 2 ? 0x10 : 0);
  for (uint32_t i = 0; i < 2; ++i)
    StoreGuestU32(base, pixel_metadata + 32 + i * 4, ((seed + i + 1) & 15) << 8 | i * 2);
  for (uint32_t variant = 0; variant < 2; ++variant) {
    const uint32_t relative = 0x4000 + variant * 0x2000;
    const uint32_t selected = kVertex + relative + 872;
    StoreGuestU32(base, kVertex + 896 + variant * 8, relative);
    StoreGuestU32(base, selected, variant * 0x2000);
    StoreGuestU32(base, selected + 4, 384);
    StoreGuestU32(base, selected + 8, seed * 0x1234567u);
    StoreGuestU32(base, selected + 12, seed * 0x23456789u);
    StoreGuestU32(base, selected + 20, mode == 6 ? 3 << 5 : 0);
    StoreGuestU32(base, selected + 24, 9);
    StoreGuestU32(base, selected + 28, count);
    for (uint32_t i = 0; i < count; ++i) {
      for (uint32_t j = 0; j < 3; ++j)
        StoreGuestU32(base, kVertex + variant * 416 + 68 + i * 12 + j * 4,
                      0x91E10DA5u * (i + 1) + (seed + j * 37));
      StoreGuestU32(base, selected + 72 + i * 4, (i % 7) << 12 | i);
    }
    for (uint32_t i = 0; i < 3; ++i) {
      StoreGuestU32(base, selected + 72 + count * 4 + i * 4,
                    i << 16 | ((seed + i) & 15) << 8 | i * 2);
      StoreGuestU32(base, selected + 72 + (count + 3 + i) * 4, i | 0x1000);
    }
    StoreGuestU32(base, kVertex + variant * 416 + 40, mode == 7 || mode == 8 ? 9 : 7);
    StoreGuestU32(base, kVertex + variant * 416 + 64, mode == 11 ? 90 : 0);
    if (mode == 11)
      StoreGuestU32(base, kVertex + variant * 416 + 40, 9);
  }
  // Real descriptor records, using independent deferred physical sources.
  for (auto [metadata, source] :
       {std::pair{kVertex + 872, kSource}, {kPixel + 40, kSource + 0x20000}}) {
    const uint32_t relative = seed & 64 ? (metadata == kVertex + 872 ? 0x1000 : 0x1800) : 0;
    StoreGuestU32(base, metadata + 20, relative);
    if (relative) {
      StoreGuestU32(base, metadata + relative + 16, 16);
      StoreGuestU32(base, metadata + relative + 20, 3u << 16 | 4);
      StoreGuestU32(base, metadata + relative + 24, 0x4000);
      StoreGuestU32(base, metadata + relative + 28, 7u << 16 | 8);
      StoreGuestU32(base, metadata + relative + 32, 0x4080);
    }
    (void)source;
  }
  for (uint32_t i = 0; i < kCodeSpan; ++i)
    *GuestToHost(base, kSource + i) = uint8_t(seed + i * 37 + i / 31);
  for (uint32_t i = 0; i < 80; ++i)
    *GuestToHost(base, kTables + i) = uint8_t(1 + (seed + i * 17) % 7);
  StoreGuestU32(base, kCounter, seed & 1 ? UINT32_MAX - 1 : 123);
  StoreGuestU32(base, kDebug, 1);
  *GuestToHost(base, kStack - 896) = uint8_t(seed);
  *GuestToHost(base, kStack - 895) = uint8_t(seed);
  *GuestToHost(base, kStack - 992) = uint8_t(seed);
  *GuestToHost(base, kStack - 991) = uint8_t(seed);
  CopyToNative();
  PPCContext ctx{};
  ctx.r1.u64 = 0xABCD000000000000ull | kStack;
  ctx.r3.u64 = 0xCDEF000000000000ull | kDevice;
  ctx.r4.u64 = flags;
  ctx.r13.u64 = 0x82D00000;
  ctx.lr = 0xDEADBEEF82345678ull;
  ctx.msr = 0x5A5A8020;
  ctx.cr2.set_raw(seed & 15);
  ctx.cr3.set_raw((seed + 1) & 15);
  ctx.cr4.set_raw((seed + 2) & 15);
  for (uint32_t i = 0; i < kNonvolatile.size(); ++i)
    (ctx.*kNonvolatile[i]).u64 = 0x1234ABCD00000000ull | (i * 0x10203u);
  return ctx;
}

void Normalize(uint32_t initial, uint32_t final) {
  uint32_t position = initial + 4;
  while (position <= final) {
    auto* base = native->data();
    const uint32_t header = LoadGuestU32(base, position);
    const uint32_t count = ((header >> 16) & 0x3FFF) + 1;
    const uint32_t opcode = (header >> 8) & 0x7F;
    uint32_t old = 0;
    if (opcode == rex::graphics::native_gpu_shader_reference::kOpcode) {
      namespace ref = rex::graphics::native_gpu_shader_reference;
      Require(
          ref::IsValidHeader(header, count) &&
              ref::Validate({LoadGuestU32(base, position + 4), LoadGuestU32(base, position + 8)}),
          "Invalid native shader reference");
      old = 0xC0012700 | (header & 1);
    } else if (opcode == rex::graphics::native_gpu_program::kOpcode) {
      namespace program = rex::graphics::native_gpu_program;
      Require(program::IsValidHeader(header, count), "Invalid native inline program header");
      if (header == program::kInvalidateHeader) {
        old = 0xC0003B00;
      } else {
        Require(program::ValidateShader(LoadGuestU32(base, position + 4),
                                        LoadGuestU32(base, position + 8), count),
                "Invalid native inline program operands");
        old = 0xC0002B00 | ((count - 1) << 16);
      }
    } else if (opcode == rex::graphics::native_gpu_indirect::kOpcode) {
      namespace indirect = rex::graphics::native_gpu_indirect;
      Require(indirect::IsValidHeader(header, count) &&
                  indirect::Validate({LoadGuestU32(base, position + 4),
                                      LoadGuestU32(base, position + 8),
                                      LoadGuestU32(base, position + 12)}),
              "Invalid native deferred constant range");
      old = 0xC0022F00;
    } else if (opcode == rex::graphics::native_gpu_predication::kOpcode) {
      namespace pred = rex::graphics::native_gpu_predication;
      Require(pred::IsValidHeader(header, count), "Invalid native selected state");
      old = header == pred::kMaskLowHeader    ? 0xC0006000
            : header == pred::kMaskHighHeader ? 0xC0006100
                                              : 0xC0002D01 | ((count - 1) << 16);
    } else if (header == rex::graphics::native_gpu_ports::kWaitHeader) {
      Require(count == 1, "Invalid native ordered wait port");
      old = 0x5C8;
    } else {
      rex::graphics::native_gpu_state::StateRange range;
      Require(rex::graphics::native_gpu_state::DecodeHeader(header, count, range),
              "Unknown or legacy command in coordinator output");
      old = range.first_register == 0x21F9 && count == 1
                ? 0xC0004600
                : ((count - 1) << 16) | range.first_register;
    }
    StoreGuestU32(base, position, old);
    position += (count + 1) * 4;
    ++native_records;
  }
  Require(position == final + 4, "Coordinator output lost an operand boundary");
}

void Compare(PPCContext input) {
  auto expected = input, actual = input;
  const auto copies = body_calls;
  const uint32_t version = LoadGuestU32(original->data(), kCounter);
  const uint32_t initial = LoadGuestU32(original->data(), kDevice + 48);
  reference_sub_8245FA00(expected, original->data());
  if (!port::CoordinateShaders(actual, native->data())) {
    throw std::runtime_error("A safe coordinator was rejected");
  }
  Require(expected.r1.u64 == actual.r1.u64 && expected.r3.u64 == actual.r3.u64 &&
              expected.lr == actual.lr && expected.msr == actual.msr &&
              expected.cr2.raw() == actual.cr2.raw() && expected.cr3.raw() == actual.cr3.raw() &&
              expected.cr4.raw() == actual.cr4.raw() && actual.r13.u64 == input.r13.u64,
          "Coordinator changed stack, return, link, interrupt or condition-register ABI");
  for (auto reg : kNonvolatile)
    Require((expected.*reg).u64 == (actual.*reg).u64, "Coordinator changed a nonvolatile GPR");
  Require(ppc_global_lock_count_().load() == 0, "Coordinator leaked an interrupt lock");
  const uint32_t final = LoadGuestU32(native->data(), kDevice + 48);
  Require(final == LoadGuestU32(original->data(), kDevice + 48), "Coordinator changed publication");
  if (initial != final)
    Normalize(initial, final);
  Same(0, kStack - 1080);
  Same(kStack, kSize - kStack);
  Same(kSource, kCodeSpan);
  Same(kTables, 80);
  Same(kDebug, 8);
  ++cases;
  inline_cases += body_calls != copies;
  null_vertex_cases += !LoadGuestU32(native->data(), kDevice + 12688);
  conditional_cases += LoadGuestU32(native->data(), kDevice + 12688) &&
                       (*GuestToHost(native->data(), kDevice + 10940) & 0x40);
  version_cases += version != LoadGuestU32(native->data(), kCounter);
}

void Rejected(PPCContext input) {
  auto* base = native->data();
  std::vector<uint8_t> low(base, base + kSize);
  std::vector<uint8_t> code(GuestToHost(base, kSource), GuestToHost(base, kSource) + kCodeSpan);
  const uint32_t counter = LoadGuestU32(base, kCounter);
  const auto saved = input;
  Require(!port::CoordinateShaders(input, base), "An unsafe coordinator was accepted");
  Require(!std::memcmp(&saved, &input, sizeof(input)) && !std::memcmp(low.data(), base, kSize) &&
              !std::memcmp(code.data(), GuestToHost(base, kSource), kCodeSpan) &&
              counter == LoadGuestU32(base, kCounter) && ppc_global_lock_count_().load() == 0,
          "Coordinator fallback had effects before a late rejection");
  ++rejects;
}

void OriginalFits(PPCContext input) {
  std::memcpy(original->data(), native->data(), kSize);
  for (auto [address, size] : {std::pair{kSource, kCodeSpan}, {kTables, 80u}, {kDebug, 8u}})
    std::memcpy(GuestToHost(original->data(), address), GuestToHost(native->data(), address), size);
  reference_sub_8245FA00(input, original->data());
  Require(
      LoadGuestU32(original->data(), kDevice + 48) <= LoadGuestU32(original->data(), kDevice + 52),
      "The original selected branch did not fit its capacity");
  ++capacity_reference_checks;
}

void Rejections() {
  for (uint32_t problem = 0; problem < 23; ++problem) {
    auto ctx = Prepare(0x1A0000, 7, 69);
    auto* base = native->data();
    const uint32_t selected = kVertex + 0x4000 + 872;
    const uint32_t pixel_selected = kPixel + 0x1000 + 40;
    current_mode = 100 + problem;
    switch (problem) {
      case 0:
        ctx.r3.u32 |= 1;
        break;
      case 1:
        ctx.r1.u32 += 4;
        break;
      case 2:
        ctx.r1.u32 = 1024;
        break;
      case 3:
        StoreGuestU32(base, kDevice + 48, kCursor + 1);
        break;
      case 4:
        StoreGuestU32(base, kDevice + 52, kCursor + 8);
        break;
      case 5:
        StoreGuestU32(base, kDevice + 56, kCursor + 8);
        break;
      case 6:
        StoreGuestU32(base, kVertex + 896, UINT32_MAX);
        break;
      case 7:
        StoreGuestU32(base, kPixel + 64, 3);
        break;
      case 8:
        StoreGuestU32(base, kDebug, 0);
        StoreGuestU32(base, kVertex + 872, 0x10);
        break;
      case 9:
        StoreGuestU32(base, kDebug, 0);
        StoreGuestU32(base, kPixel + 40, 0x10);
        break;
      case 10:
        StoreGuestU32(base, pixel_selected + 4, 382);
        break;
      case 11:
        *GuestToHost(base, kDevice + 10942) = 0x80;
        StoreGuestU32(base, selected + 4, 0);
        break;
      case 12:
        StoreGuestU32(base, kDeclaration + 24, 257);
        break;
      case 13:
        StoreGuestU32(base, kVertex + 32, kCursor);
        break;
      case 14:
      case 15:
      case 16: {
        const uint32_t cursor = problem == 14   ? selected - 4
                                : problem == 15 ? kStack - 1024
                                                : kDevice + 128;
        StoreGuestU32(base, kDevice + 48, cursor);
        StoreGuestU32(base, kDevice + 52, cursor + 4096);
        StoreGuestU32(base, kDevice + 56, cursor + 4096);
        break;
      }
      case 17:
        StoreGuestU32(base, kVertex + 32, kVertex);
        break;
      case 18:
        StoreGuestU32(base, kDeclaration + 48, 0);
        StoreGuestU32(base, kVertex + 872, 0x20);
        StoreGuestU32(base, kDevice + 10556, 0);
        StoreGuestU32(base, pixel_selected + 28, 0);
        // The miss fits, but the possible cache hit needs a larger final batch.
        StoreGuestU32(base, kVertex + 416 + 64, 90);
        StoreGuestU32(base, kDevice + 52, kCursor + 180);
        break;
      case 19:
        StoreGuestU32(base, kDevice + 52, kCursor + 84);
        break;
      case 20:
        StoreGuestU32(base, kVertex + 32, kCounter);
        break;
      case 21:
        StoreGuestU32(base, kVertex + 32, pixel_selected);
        break;
      case 22:
        StoreGuestU32(base, kVertex + 0x6000 + 876, 383);
        StoreGuestU32(base, kVertex + 872, 0x20);
        StoreGuestU32(base, kDevice + 10556, 0);
        StoreGuestU32(base, pixel_selected + 28, 0);
        break;
    }
    if (problem == 18)
      OriginalFits(ctx);
    Rejected(ctx);
  }
}

void UnusedInputs() {
  for (uint32_t seed = 0; seed < 8; ++seed) {
    auto ctx = Prepare(0xABCD000000000000ull, 0, seed);
    auto* base = original->data();
    StoreGuestU32(base, kDevice + 48, UINT32_MAX);
    StoreGuestU32(base, kDevice + 11812, UINT32_MAX);
    StoreGuestU32(base, kDevice + 12684, UINT32_MAX);
    CopyToNative();
    Compare(ctx);
    ctx = Prepare(0, 2, seed);
    base = original->data();
    *GuestToHost(base, kDevice + 10940) = 0;
    StoreGuestU32(base, kVertex + 896, UINT32_MAX);
    StoreGuestU32(base, kDevice + 10580, 5);
    StoreGuestU32(base, kDevice + 48, UINT32_MAX);
    StoreGuestU32(base, kDevice + 52, 0);
    StoreGuestU32(base, kDevice + 56, 0);
    StoreGuestU32(base, kDevice + 11812, UINT32_MAX);
    StoreGuestU32(base, kDevice + 12432, UINT32_MAX);
    StoreGuestU32(base, kVertex + 0x6000 + 872, UINT32_MAX);
    StoreGuestU32(base, kVertex + 0x6000 + 876, 3);
    CopyToNative();
    Compare(ctx);
  }
}

void PendingVersions() {
  for (uint32_t seed = 0; seed < 4; ++seed)
    for (uint32_t counter : {0u, 6u, 123u, UINT32_MAX - 2, UINT32_MAX - 1, UINT32_MAX})
      for (uint32_t saved : {0u, UINT32_MAX})
        for (bool busy : {false, true}) {
          auto ctx = Prepare(0x180000, 9, seed);
          auto* base = original->data();
          *GuestToHost(base, kDevice + 10940) = 0x40;
          StoreGuestU32(base, kDevice + 10556, 0);
          StoreGuestU32(base, kPixel + 0x1000 + 68, 0);
          StoreGuestU32(base, kDeclaration + 48, 0);
          StoreGuestU32(base, kVertex + 416 + 40, saved);
          StoreGuestU32(base, kVertex + 416 + 64, busy ? 90 : 0);
          StoreGuestU32(base, kCounter, counter);
          CopyToNative();
          Compare(ctx);
          ++pending_version_cases;
          pending_busy_cases += busy;
        }
  // These four formerly ambiguous IDs now execute and compare the real hit.
  for (uint32_t saved : {1u, 7u, 124u, UINT32_MAX - 1}) {
    auto ctx = Prepare(0x180000, 9, 4);
    auto* base = original->data();
    StoreGuestU32(base, kDevice + 10556, 0);
    StoreGuestU32(base, kPixel + 0x1000 + 68, 0);
    StoreGuestU32(base, kDeclaration + 48, 0);
    StoreGuestU32(base, kVertex + 416 + 40, saved);
    StoreGuestU32(base, kCounter, saved - 1);
    CopyToNative();
    Compare(ctx);
    ++retained_version_cases;
    ++retained_cache_hits;
  }
}

void RetainedCase(uint32_t seed, uint32_t saved, uint32_t counter, bool busy, bool streams) {
  auto ctx = Prepare(0x180000, 9, seed);
  auto* base = original->data();
  *GuestToHost(base, kDevice + 10940) = 0x40;
  StoreGuestU32(base, kDevice + 10556, 0);
  StoreGuestU32(base, kPixel + 0x1000 + 68, 0);
  StoreGuestU32(base, kDeclaration + 48, 0);
  StoreGuestU32(base, kVertex + 416 + 40, saved);
  StoreGuestU32(base, kVertex + 416 + 64, busy ? 90 : 0);
  StoreGuestU32(base, kCounter, counter);
  if (!streams) {
    StoreGuestU32(base, kDeclaration + 32, UINT32_MAX);
    StoreGuestU32(base, kVertex + 416 + 48, ~LoadGuestU32(base, kDevice + 12520));
  }
  // The original treats a -1 alternate loop mask as an omitted delta.
  if (seed & 2)
    StoreGuestU32(base, kVertex + 0x6000 + 880, UINT32_MAX);
  CopyToNative();
  Compare(ctx);
  const bool hit = streams && LoadGuestU32(base, kDeclaration + 48) == saved;
  ++retained_version_cases;
  retained_cache_hits += hit;
  retained_busy_hits += hit && busy;
}

void RetainedVersions() {
  for (uint32_t seed = 0; seed < 4; ++seed)
    for (uint32_t saved : {1u, 7u, 124u, UINT32_MAX - 1, 0x80000000u})
      for (bool match : {false, true})
        for (bool busy : {false, true})
          for (bool streams : {false, true})
            RetainedCase(seed | (saved & 1 ? 16 : 0) | (saved == 124 ? 64 : 0), saved,
                         match ? saved - 1 : saved, busy, streams);
  for (uint32_t seed = 0; seed < 4; ++seed)
    for (uint32_t counter : {UINT32_MAX - 1, UINT32_MAX})
      for (bool busy : {false, true})
        for (bool streams : {false, true})
          RetainedCase(seed, 1, counter, busy, streams);
  // Both potential final batches must be safe before the first allocation.
  for (uint32_t problem = 0; problem < 4; ++problem) {
    auto ctx = Prepare(0x180000, 9, 4);
    auto* base = native->data();
    StoreGuestU32(base, kDevice + 10556, 0);
    StoreGuestU32(base, kPixel + 0x1000 + 68, 0);
    StoreGuestU32(base, kDeclaration + 48, 0);
    StoreGuestU32(base, kVertex + 416 + 40, 9);
    StoreGuestU32(base, kVertex + 416 + 64, 90);
    StoreGuestU32(base, kCounter, 123);
    if (problem == 0)
      StoreGuestU32(base, kVertex + 0x6000 + 876, 383);
    if (problem == 1) {
      const uint32_t cursor = kSource + 0x2000;
      StoreGuestU32(base, kDevice + 48, cursor);
      StoreGuestU32(base, kDevice + 52, cursor + 512);
      StoreGuestU32(base, kDevice + 56, cursor + 512);
    }
    if (problem == 2)
      StoreGuestU32(base, kVertex + 904, kStack - 64 - kVertex - 880);
    if (problem == 3) {
      // A hit ignores fetch metadata that the possible rebuild cannot accept.
      StoreGuestU32(base, kCounter, 8);
      StoreGuestU32(base, kVertex + 416 + 64, 0);
      StoreGuestU32(base, kVertex + 0x6000 + 900, 33);
      OriginalFits(ctx);
    }
    Rejected(ctx);
  }
  for (uint32_t saved : {0u, UINT32_MAX, 9u}) {
    auto ctx = Prepare(0x180000, 9, 5);
    auto* base = original->data();
    *GuestToHost(base, kDevice + 10940) = 0x40;
    StoreGuestU32(base, kDevice + 10556, 0);
    StoreGuestU32(base, kPixel + 0x1000 + 68, 0);
    StoreGuestU32(base, kDeclaration + 48, 0);
    StoreGuestU32(base, kVertex + 416 + 40, saved);
    StoreGuestU32(base, kVertex + 416 + 64, 90);
    StoreGuestU32(base, kCounter, 8);
    StoreGuestU32(base, kDeclaration + 32, UINT32_MAX);
    StoreGuestU32(base, kVertex + 416 + 48, ~LoadGuestU32(base, kDevice + 12520));
    StoreGuestU32(base, kVertex + 904, UINT32_MAX);
    CopyToNative();
    Compare(ctx);
    ++unused_retained_cases;
  }
}
}  // namespace

int main(int argc, char** argv) {
  try {
    original = std::make_unique<GuestMemoryFixture>(kSize);
    native = std::make_unique<GuestMemoryFixture>(kSize);
    for (auto* memory : {original.get(), native.get()}) {
      memory->Commit(kSource, kCodeSpan);
      memory->Commit(kTables, 80);
      memory->Commit(kDebug, 8);
    }
    for (uint32_t i = 0; i < kSize; ++i)
      pristine[i] = uint8_t(i * 37 + i / 7);
    for (uint32_t seed = 0; seed < 128; ++seed)
      for (uint32_t mode = 0; mode < 12; ++mode)
        for (uint64_t flags :
             {0ull, 8ull, 0x80000ull, 0x100000ull, 0x180000ull, 0x1A0000ull, 0x01000000001A0008ull})
          Compare(Prepare(flags, mode, seed));
    Rejections();
    UnusedInputs();
    PendingVersions();
    RetainedVersions();
    FILE* output = argc > 1 ? std::fopen(argv[1], "wb") : stdout;
    Require(output, "Cannot write the coordinator report");
    std::fprintf(output,
                 "{\"passed\":true,\"cases\":%llu,\"unchanged_fallbacks\":%llu,"
                 "\"null_vertex_cases\":%llu,\"conditional_cases\":%llu,"
                 "\"inline_cases\":%llu,\"version_allocations\":%llu,"
                 "\"native_records\":%llu,\"real_coordinator_calls\":%llu,"
                 "\"real_constant_calls\":%llu,\"real_resource_calls\":%llu,"
                 "\"real_reserve_calls\":%llu,\"real_memcpy_calls\":%llu,"
                 "\"real_linkage_calls\":%llu,\"real_declaration_calls\":%llu,"
                 "\"real_control_calls\":%llu,\"inline_barriers\":%llu,"
                 "\"pending_version_cases\":%llu,\"pending_busy_cases\":%llu,"
                 "\"retained_version_cases\":%llu,\"retained_cache_hits\":%llu,"
                 "\"retained_busy_hits\":%llu,\"capacity_reference_checks\":%llu,"
                 "\"unused_retained_cases\":%llu}\n",
                 (unsigned long long)cases, (unsigned long long)rejects,
                 (unsigned long long)null_vertex_cases, (unsigned long long)conditional_cases,
                 (unsigned long long)inline_cases, (unsigned long long)version_cases,
                 (unsigned long long)native_records, (unsigned long long)coordinator_calls,
                 (unsigned long long)constant_calls, (unsigned long long)resource_calls,
                 (unsigned long long)reserve_calls, (unsigned long long)copy_calls,
                 (unsigned long long)link_calls, (unsigned long long)declaration_calls,
                 (unsigned long long)control_calls, (unsigned long long)sync_calls,
                 (unsigned long long)pending_version_cases, (unsigned long long)pending_busy_cases,
                 (unsigned long long)retained_version_cases,
                 (unsigned long long)retained_cache_hits, (unsigned long long)retained_busy_hits,
                 (unsigned long long)capacity_reference_checks,
                 (unsigned long long)unused_retained_cases);
    if (output != stdout)
      std::fclose(output);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(
        stderr, "Shader coordinator failed at case %llu, seed %u, mode %u, flags %08X: %s\n",
        (unsigned long long)cases, current_seed, current_mode, current_flags, error.what());
    return 1;
  }
}
