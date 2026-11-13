#include "simpsons_init.h"
#include "guest_memory_fixture.h"
#include "native_gpu_inline_shader.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <vector>

static uint64_t body_calls = 0, reserve_calls = 0, copy_calls = 0;
static uint64_t link_calls = 0, declaration_calls = 0, sync_calls = 0;
#include "native_gpu_inline_shader_reference.inc"

namespace port = simpsons::native_gpu_calls;
namespace program = rex::graphics::native_gpu_program;
using simpsons::GuestToHost;
using simpsons::LoadGuestU32;
using simpsons::StoreGuestU32;

namespace {
constexpr uint32_t kSize = 0x200000, kDevice = 0x1000, kResource = 0x20000;
constexpr uint32_t kDeclaration = 0x40000, kPixel = 0x70000, kResult = 0x80000;
constexpr uint32_t kCursor = 0xA0000, kStack = 0x1F0000, kSource = 0xC0100000;
constexpr uint32_t kTables = port::vertex_declaration_detail::kSwizzleTable;
constexpr uint32_t kCodeSpan = 65536 + 128;
constexpr std::array<PPCRegister PPCContext::*, 18> kNonvolatile = {
    &PPCContext::r14, &PPCContext::r15, &PPCContext::r16, &PPCContext::r17, &PPCContext::r18,
    &PPCContext::r19, &PPCContext::r20, &PPCContext::r21, &PPCContext::r22, &PPCContext::r23,
    &PPCContext::r24, &PPCContext::r25, &PPCContext::r26, &PPCContext::r27, &PPCContext::r28,
    &PPCContext::r29, &PPCContext::r30, &PPCContext::r31};
std::unique_ptr<GuestMemoryFixture> original, native;
std::array<uint8_t, kSize> pristine;
uint64_t cases = 0, rejections = 0, linked_cases = 0, empty_cases = 0, high_cases = 0;
uint32_t current_bytes = 0, current_count = 0, current_mode = 0, current_seed = 0;

void Require(bool ok, const char* why) {
  if (!ok)
    throw std::runtime_error(why);
}

void CommitView(GuestMemoryFixture& memory, uint32_t address, uint32_t bytes) {
#if REX_PLATFORM_WIN32
  if (address >= 0xE0000000u)
    bytes += 4096;
#endif
  memory.Commit(address, bytes);
}

uint32_t Next(uint32_t& state) {
  state ^= state << 13;
  state ^= state >> 17;
  state ^= state << 5;
  return state;
}

PPCContext Prepare(uint32_t bytes, uint32_t count, uint32_t variant, uint32_t mode, uint32_t seed,
                   uint32_t export_override = UINT32_MAX,
                   uint32_t attribute_override = UINT32_MAX) {
  auto* base = original->data();
  std::memcpy(base, pristine.data(), kSize);
  std::memset(base + kDevice, 0, 22256);
  uint32_t random = seed + 1;
  const uint32_t cursor = kCursor + (seed & 4), relative = 0x4000 + variant * 0x2000;
  const uint32_t selected = kResource + relative + 872, table = selected + 72;
  StoreGuestU32(base, kDevice + 48, cursor);
  StoreGuestU32(base, kDevice + 52, cursor + 20 + bytes + (seed & 1 ? 0 : 64));
  StoreGuestU32(base, kDevice + 56, cursor);
  *GuestToHost(base, kDevice + 10942) = uint8_t(seed * 37);
  for (uint32_t i = 0; i < 96; ++i)
    *GuestToHost(base, kDevice + 12520 + i) = uint8_t(seed + i * 3);
  StoreGuestU32(base, kResource + 32, kSource + (seed & 4));
  StoreGuestU32(base, kResource + (variant + 112) * 8, relative);
  std::memset(base + selected, 0, 32);
  StoreGuestU32(base, selected, 0);
  StoreGuestU32(base, selected + 4, bytes);
  StoreGuestU32(base, selected + 24, 9);
  StoreGuestU32(base, selected + 28, count);
  constexpr std::array<uint32_t, 8> attributes = {0, 1, 7, 16, 32, 1, 16, 32};
  const uint32_t attribute_count =
      attribute_override == UINT32_MAX ? attributes[seed & 7] : attribute_override;
  StoreGuestU32(base, kDeclaration + 24, attribute_count);
  for (uint32_t i = 0; i < attribute_count; ++i) {
    const uint32_t stream = seed & 1 ? 0 : Next(random) % 96;
    StoreGuestU32(base, kDeclaration + 52 + i * 12, stream << 16 | ((i * 4) & 65535));
    StoreGuestU32(base, kDeclaration + 56 + i * 12, Next(random));
    StoreGuestU32(base, kDeclaration + 60 + i * 12, ((i & 15) << 16) | ((i >> 4) << 8));
  }
  const uint32_t instructions = std::min(std::max(bytes / 12, 1u), 4096u);
  for (uint32_t i = 0; i < count; ++i) {
    for (uint32_t j = 0; j < 3; ++j)
      StoreGuestU32(base, kResource + variant * 416 + 68 + i * 12 + j * 4, Next(random));
    const uint32_t output =
        seed & 4 ? (i & 1 ? instructions - 1 : 0) : (i * 17 + seed) % instructions;
    const uint32_t marker = seed & 2 ? (i == 0           ? 0x100000
                                        : i == count - 1 ? 0x300000
                                                         : 0)
                                     : (Next(random) & 3) << 20;
    StoreGuestU32(base, table + i * 4, marker | (i >> 4) << 16 | (i & 15) << 12 | output);
  }
  const uint32_t exports = export_override == UINT32_MAX ? (mode == 7 ? 0 : 3) : export_override;
  const uint32_t export_table = table + count * 4;
  StoreGuestU32(base, selected + 20, exports << 5 | (mode == 4 ? 0x40000 : 0));
  if (mode == 3)
    *GuestToHost(base, selected + 8) = 7;
  const uint32_t length = 1 + (seed % 3), lists = export_table + exports * 4;
  for (uint32_t i = 0; i < exports; ++i) {
    const uint32_t nibble = (seed + i) & 15;
    StoreGuestU32(base, export_table + i * 4, (i * length) << 16 | nibble << 8 | i * 2);
    for (uint32_t j = 0; j < length; ++j) {
      const uint32_t index =
          seed & 4 ? (j & 1 ? instructions - 1 : 0) : (seed + i * 5 + j) % instructions;
      StoreGuestU32(base, lists + (i * length + j) * 4, index | (j == length - 1 ? 0x1000 : 0));
    }
  }
  const uint32_t secondary_count = mode == 2 || mode == 7 ? 0 : mode == 6 ? exports : 2;
  StoreGuestU32(base, kPixel + 20,
                secondary_count << 5 | ((seed + 1) & 31) | (mode == 5 ? 0x20000 : 0));
  for (uint32_t i = 0; i < secondary_count; ++i) {
    const uint32_t selected_export = mode == 6 ? i : i * 2;
    const uint32_t nibble = (seed + selected_export + (mode == 6 ? 0 : 1)) & 15;
    StoreGuestU32(base, kPixel + 32 + i * 4, nibble << 8 | selected_export * 2);
  }
  const uint32_t result = seed & 1 ? kStack + 80 : kResult;
  StoreGuestU32(base, result, seed * 0x91E10DA5u);
  *GuestToHost(base, kStack - 736) = uint8_t(seed);
  *GuestToHost(base, kStack - 735) = uint8_t(seed);
  for (uint32_t i = 0; i < 16; i += 4)
    StoreGuestU32(base, kTables + i, Next(random));
  for (uint32_t i = 0; i < 64; ++i)
    *GuestToHost(base, kTables + 16 + i) = uint8_t(1 + (Next(random) & 7));
  for (uint32_t i = 0; i < kCodeSpan; ++i)
    *GuestToHost(base, kSource + i) = uint8_t(i * 37 + seed * 7 + (i >> 8));
  std::memcpy(native->data(), base, kSize);
  for (auto [address, size] : {std::pair{kTables, 80u}, {kSource, kCodeSpan}})
    std::memcpy(GuestToHost(native->data(), address), GuestToHost(base, address), size);
  PPCContext ctx{};
  ctx.r1.u64 = 0xABCD000000000000ull | kStack;
  ctx.r3.u64 = 0xA5A5000000000000ull | kDevice;
  ctx.r4.u64 = 0xABBA000000000000ull | (mode ? (seed & 1 ? UINT32_MAX : 1) : 0);
  ctx.r5.u64 = 0xFEED000000000000ull | kResource;
  ctx.r6.u64 = 0xBCDE000000000000ull | result;
  ctx.r7.u64 = 0xBCDE000000000000ull | kDeclaration;
  ctx.r8.u64 = 0xABBA000000000000ull | selected;
  ctx.r9.u64 = 0xBEEF000000000000ull | (secondary_count ? kPixel : 0);
  ctx.r10.u64 = variant;
  ctx.lr = 0xDEADBEEF82345678ull;
  ctx.msr = 0xA5A58020;
  ctx.cr2.set_raw(seed & 15);
  ctx.cr3.set_raw((seed + 1) & 15);
  ctx.cr4.set_raw((seed + 2) & 15);
  for (uint32_t i = 0; i < kNonvolatile.size(); ++i)
    (ctx.*kNonvolatile[i]).u64 = 0x1234ABCD00000000ull | (i * 0x10203u);
  return ctx;
}

void SameRange(uint32_t address, uint32_t size) {
  if (std::memcmp(GuestToHost(original->data(), address), GuestToHost(native->data(), address),
                  size)) {
    for (uint32_t i = 0; i < size; ++i)
      if (*GuestToHost(original->data(), address + i) !=
          *GuestToHost(native->data(), address + i)) {
        std::fprintf(stderr, "Difference at %08X: expected=%02X actual=%02X\n", address + i,
                     *GuestToHost(original->data(), address + i),
                     *GuestToHost(native->data(), address + i));
        break;
      }
    throw std::runtime_error("Inline shader changed code, state, metadata or neighboring bytes");
  }
}

void Compare(PPCContext input, uint32_t bytes) {
  auto expected = input, actual = input;
  const uint32_t cursor = LoadGuestU32(original->data(), input.r3.u32 + 48);
  reference_sub_8245F6B0(expected, original->data());
  Require(port::WriteInlineResourceShader(actual, native->data()),
          "Safe inline shader was rejected");
  Require(expected.r1.u64 == actual.r1.u64 && expected.r3.u64 == actual.r3.u64 &&
              expected.lr == actual.lr && expected.msr == actual.msr &&
              expected.cr2.raw() == actual.cr2.raw() && expected.cr3.raw() == actual.cr3.raw() &&
              expected.cr4.raw() == actual.cr4.raw(),
          "Inline shader changed its stack, return, link, interrupt or condition-register ABI");
  for (auto reg : kNonvolatile)
    Require((expected.*reg).u64 == (actual.*reg).u64, "Inline shader changed a nonvolatile GPR");
  const uint32_t selected =
      input.r5.u32 + LoadGuestU32(original->data(), input.r5.u32 + (input.r10.u32 + 112) * 8) + 872;
  const uint32_t fetches = LoadGuestU32(original->data(), selected + 28);
  const uint32_t header = LoadGuestU32(native->data(), cursor + 12);
  Require(LoadGuestU32(native->data(), cursor + 4) == program::kInvalidateHeader &&
              LoadGuestU32(native->data(), cursor + 8) == 0x100 &&
              program::IsShaderHeader(header, bytes / 4 + 2) &&
              program::ValidateShader(LoadGuestU32(native->data(), cursor + 16),
                                      LoadGuestU32(native->data(), cursor + 20), bytes / 4 + 2),
          "Inline native program changed its invalidation, stage or payload size");
  // Only the two deliberately converted transport headers differ. Validate the
  // native records above, then compare every operand, code and neighboring byte.
  StoreGuestU32(native->data(), cursor + 4, 0xC0003B00);
  StoreGuestU32(native->data(), cursor + 12, 0xC0002B00 | ((bytes / 4 + 1) << 16));
  Require(!std::memcmp(original->data(), native->data(), kStack - 824) &&
              !std::memcmp(original->data() + kStack, native->data() + kStack, kSize - kStack),
          "Inline shader changed low memory outside its private frame");
  SameRange(input.r3.u32, 22256);
  SameRange(input.r5.u32, 0xA000);
  if (fetches)
    SameRange(input.r7.u32, 4096);
  SameRange(cursor - 4, bytes + 32);
  SameRange(kSource, kCodeSpan);
  SameRange(kTables, 80);
  ++cases;
  linked_cases += input.r4.s32 != 0;
  empty_cases += fetches == 0;
}

void RequireRejected(PPCContext& ctx,
                     std::initializer_list<std::pair<uint32_t, uint32_t>> extra = {}) {
  auto* base = native->data();
  std::vector<uint8_t> before(base, base + kSize);
  std::array<uint8_t, sizeof(PPCContext)> context;
  std::memcpy(context.data(), &ctx, context.size());
  std::vector<std::pair<uint32_t, std::vector<uint8_t>>> chunks;
  for (auto [address, bytes] : {std::pair{kSource, kCodeSpan}, {kTables, 80u}})
    chunks.emplace_back(address, std::vector<uint8_t>(GuestToHost(base, address),
                                                      GuestToHost(base, address) + bytes));
  for (auto [address, bytes] : extra)
    chunks.emplace_back(address, std::vector<uint8_t>(GuestToHost(base, address),
                                                      GuestToHost(base, address) + bytes));
  Require(!port::WriteInlineResourceShader(ctx, base), "Unsafe inline shader was accepted");
  Require(!std::memcmp(context.data(), &ctx, context.size()) &&
              !std::memcmp(base, before.data(), before.size()),
          "Inline shader fallback changed state before rejecting the call");
  for (const auto& [address, contents] : chunks)
    Require(!std::memcmp(GuestToHost(base, address), contents.data(), contents.size()),
            "Inline shader rejection changed a high-view owner");
  ++rejections;
}

void Reject(uint32_t problem) {
  auto ctx = Prepare(384, 7, 0, 1, 3);
  auto* base = native->data();
  const uint32_t selected = ctx.r8.u32;
  switch (problem) {
    case 0:
      ctx.r1.u32 |= 1;
      break;
    case 1:
      ctx.r1.u32 = 64;
      break;
    case 2:
      ctx.r3.u32 |= 1;
      break;
    case 3:
      ctx.r5.u32 |= 1;
      break;
    case 4:
      ctx.r10.u64 = UINT64_MAX;
      break;
    case 5:
      ctx.r10.u64 = UINT32_MAX;
      break;
    case 6:
      StoreGuestU32(base, kResource + 896, UINT32_MAX);
      break;
    case 7:
      StoreGuestU32(base, selected + 4, 0);
      break;
    case 8:
      StoreGuestU32(base, selected + 4, 385);
      break;
    case 9:
      StoreGuestU32(base, selected + 4, 65532);
      break;
    case 10:
      StoreGuestU32(base, kDevice + 52, kCursor + 20 + 384 - 4);
      break;
    case 11:
      StoreGuestU32(base, kDevice + 48, kCursor + 1);
      break;
    case 12:
      StoreGuestU32(base, kDevice + 48, 0);
      break;
    case 13:
      StoreGuestU32(base, kResource + 32, UINT32_MAX - 3);
      StoreGuestU32(base, selected, 8);
      break;
    case 14:
      StoreGuestU32(base, kResource + 32, 0xDFFFFFFC);
      break;
    case 15:
      StoreGuestU32(base, selected + 28, 33);
      break;
    case 16:
      StoreGuestU32(base, selected + 24, UINT32_MAX);
      break;
    case 17:
      StoreGuestU32(base, kDeclaration + 24, 257);
      break;
    case 18:
      ctx.r7.u32 |= 1;
      break;
    case 19:
      ctx.r8.u32 |= 1;
      break;
    case 20:
      ctx.r9.u32 |= 1;
      break;
    case 21:
      ctx.r6.u32 |= 1;
      break;
    case 22:
      ctx.r6.u32 = kCursor + 4;
      break;
    case 23:
      ctx.r6.u32 = kDeclaration + 32;
      break;
    case 24:
      ctx.r6.u32 = kDevice + 12520;
      break;
    case 25:
      ctx.r6.u32 = kSource;
      break;
    case 26:
      ctx.r6.u32 = kStack - 128;
      break;
    case 27:
      StoreGuestU32(base, selected + 72, 4095);
      break;
    case 28:
      StoreGuestU32(base, selected + 72 + 7 * 4 + 3 * 4, 0x1FFF);
      break;
    case 29:
      StoreGuestU32(base, kDevice + 48, kStack - 824);
      StoreGuestU32(base, kDevice + 52, kStack);
      break;
    case 30:
      StoreGuestU32(base, kDevice + 48, kResource);
      StoreGuestU32(base, kDevice + 52, kResource + 4096);
      break;
    case 31:
      StoreGuestU32(base, kDevice + 48, kSource);
      StoreGuestU32(base, kDevice + 52, kSource + 4096);
      break;
    case 32:
      ctx.r8.u32 = kStack - 64;
      StoreGuestU32(base, ctx.r8.u32 + 20, 0x40000);
      break;
    case 33:
      ctx.r8.u32 = kDevice + 128;
      StoreGuestU32(base, ctx.r8.u32 + 20, 0x40000);
      break;
    case 34:
      StoreGuestU32(base, kDeclaration + 52, 96u << 16);
      StoreGuestU32(base, selected + 72, 0);
      break;
    case 35:
      StoreGuestU32(base, selected + 72 + 7 * 4 + 3 * 4, 0);
      for (uint32_t i = 1; i < 4096; ++i)
        StoreGuestU32(base, selected + 72 + 7 * 4 + 3 * 4 + i * 4, 0);
      break;
    default:
      throw std::runtime_error("Unknown rejection");
  }
  RequireRejected(ctx);
}

void AliasRejections() {
  for (uint32_t kind = 0; kind < 4; ++kind) {
    auto ctx = Prepare(384, 7, 0, 1, 3);
    auto* base = native->data();
    const uint32_t low = kind == 0 ? kDevice : kind == 1 ? kResource : kDeclaration;
    const uint32_t address = 0xC0000000u + low;
    const uint32_t span = kind == 0 ? 22256 : kind == 1 ? 0xA000 : 4096;
    native->Commit(address, span);
    std::memcpy(GuestToHost(base, address), GuestToHost(base, low), span);
    if (kind == 0)
      ctx.r3.u32 = address;
    if (kind == 1) {
      ctx.r5.u32 = address;
      ctx.r8.u32 += address - low;
    }
    if (kind < 2) {
      StoreGuestU32(base, ctx.r3.u32 + 48, 0xA0000000u + low);
      StoreGuestU32(base, ctx.r3.u32 + 52, 0xA0000000u + low + 4096);
      RequireRejected(ctx, {{address, span}});
    } else if (kind == 2) {
      ctx.r7.u32 = address;
      const uint32_t other = 0xA0000000u + low;
      native->Commit(other, span);
      std::memcpy(GuestToHost(base, other), GuestToHost(base, low), span);
      ctx.r6.u32 = other + 32;
      RequireRejected(ctx, {{address, span}, {other, span}});
    } else {
      StoreGuestU32(base, kDevice + 48, 0xA0100000);
      StoreGuestU32(base, kDevice + 52, 0xA0100000 + 4096);
      RequireRejected(ctx, {{address, span}});
    }
  }
}

void Views() {
  constexpr std::array<uint32_t, 6> views = {0x7F000000, 0x80000000, 0x90000000,
                                             0xA0000000, 0xC0000000, 0xE0000000};
  for (uint32_t kind = 0; kind < 4; ++kind)
    for (uint32_t view : views)
      for (uint32_t alignment : {0u, 4u}) {
        auto ctx = Prepare(1024, 7, 1, 1, 5);
        const uint32_t old = kind == 0   ? kDevice
                             : kind == 1 ? kResource
                             : kind == 2 ? kDeclaration
                                         : kCursor + 4;
        const uint32_t address = view + old + alignment;
        const uint32_t span = kind == 0 ? 22256 : kind == 1 ? 0xA000 : kind == 2 ? 4096 : 2048;
        for (auto* memory : {original.get(), native.get()}) {
          CommitView(*memory, address, span);
          std::memcpy(GuestToHost(memory->data(), address), GuestToHost(memory->data(), old), span);
          if (kind == 3) {
            StoreGuestU32(memory->data(), kDevice + 48, address);
            StoreGuestU32(memory->data(), kDevice + 52, address + 1044);
          }
        }
        if (kind == 0)
          ctx.r3.u32 = address;
        if (kind == 1) {
          ctx.r5.u32 = address;
          ctx.r8.u32 += address - old;
        }
        if (kind == 2)
          ctx.r7.u32 = address;
        Compare(ctx, 1024);
        ++high_cases;
      }
  for (uint32_t raw : {0x00100000u, 0x7F100000u, 0x82100000u, 0x92100000u, 0xA0100000u, 0xC0100000u,
                       0xE0100000u}) {
    auto ctx = Prepare(1024, 7, 0, 1, 2);
    const uint32_t physical = (raw & 0x1FFFFFFF) + (((raw >> 20) + 512) & 0x1000);
    const uint32_t address = 0xC0000000u + physical;
    for (auto* memory : {original.get(), native.get()}) {
      memory->Commit(address, 2048);
      std::memmove(GuestToHost(memory->data(), address), GuestToHost(memory->data(), kSource),
                   2048);
      StoreGuestU32(memory->data(), kResource + 32, raw);
    }
    Compare(ctx, 1024);
    SameRange(address, 2048);
    ++high_cases;
  }
}
}  // namespace

int main(int argc, char** argv) {
  if (argc > 2 || (argc == 2 && !std::freopen(argv[1], "w", stdout)))
    return 2;
  try {
    original = std::make_unique<GuestMemoryFixture>(kSize);
    native = std::make_unique<GuestMemoryFixture>(kSize);
    for (auto* memory : {original.get(), native.get()}) {
      memory->Commit(kTables, 80);
      memory->Commit(kSource, kCodeSpan);
    }
    for (uint32_t i = 0; i < kSize; ++i)
      pristine[i] = uint8_t(i * 37 + (i >> 6));
    for (uint32_t bytes : {4u, 12u, 24u, 128u, 384u, 1024u, 49152u, 65528u})
      for (uint32_t count : {0u, 1u, 7u, 32u})
        for (uint32_t variant : {0u, 1u})
          for (uint32_t mode = 0; mode < 8; ++mode)
            for (uint32_t seed = 0; seed < 8; ++seed) {
              if (bytes < 12 && (count || mode == 1 || mode == 2 || mode == 5 || mode == 6))
                continue;
              current_bytes = bytes;
              current_count = count;
              current_mode = mode;
              current_seed = seed;
              Compare(Prepare(bytes, count, variant, mode, seed), bytes);
            }
    Views();
    for (uint32_t bytes : {49152u, 65528u})
      for (uint32_t mode : {1u, 2u, 6u})
        for (uint32_t variant : {0u, 1u})
          for (uint32_t seed = 0; seed < 8; ++seed) {
            current_bytes = bytes;
            current_count = 32;
            current_mode = mode;
            current_seed = seed;
            Compare(Prepare(bytes, 32, variant, mode, seed, 31, 256), bytes);
          }
    // No linkage means no access to its metadata/result arguments. An empty
    // fetch table likewise does not dereference the declaration pointer.
    for (uint32_t count : {0u, 7u}) {
      auto ctx = Prepare(384, count, 0, 0, 3);
      ctx.r6.u32 = ctx.r8.u32 = ctx.r9.u32 = UINT32_MAX;
      if (!count)
        ctx.r7.u32 = 0xFFFF0000;
      Compare(ctx, 384);
    }
    for (uint32_t problem = 0; problem < 36; ++problem) {
      current_mode = 100 + problem;
      Reject(problem);
    }
    AliasRejections();
    Require(body_calls == cases && reserve_calls == cases && declaration_calls == cases &&
                sync_calls == cases * 2 && link_calls == linked_cases && copy_calls > cases,
            "The inline oracle did not execute the complete real call chain");
    std::printf(
        "{\"passed\":true,\"cases\":%llu,\"unchanged_fallbacks\":%llu,"
        "\"real_reserve_calls\":%llu,\"real_memcpy_calls\":%llu,\"real_linkage_calls\":%llu,"
        "\"real_declaration_calls\":%llu,\"barriers\":%llu,\"empty_fetch_cases\":%llu,"
        "\"high_view_cases\":%llu}\n",
        (unsigned long long)cases, (unsigned long long)rejections,
        (unsigned long long)reserve_calls, (unsigned long long)copy_calls,
        (unsigned long long)link_calls, (unsigned long long)declaration_calls,
        (unsigned long long)sync_calls, (unsigned long long)empty_cases,
        (unsigned long long)high_cases);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s; bytes=%u count=%u mode=%u seed=%u\n", error.what(), current_bytes,
                 current_count, current_mode, current_seed);
    return 1;
  }
}
