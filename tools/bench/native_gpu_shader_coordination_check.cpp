#include "simpsons_init.h"
#include "guest_memory_fixture.h"
#include "native_gpu_shader_constants.h"
#include "native_gpu_shader_control.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <vector>

static uint64_t control_calls = 0, constant_calls = 0, constant_records = 0;
#include "native_gpu_shader_coordination_reference.inc"

namespace port = simpsons::native_gpu_calls;
namespace indirect = rex::graphics::native_gpu_indirect;
namespace state = rex::graphics::native_gpu_state;
namespace ports = rex::graphics::native_gpu_ports;
using simpsons::GuestToHost;
using simpsons::LoadGuestU32;
using simpsons::StoreGuestU32;

namespace {
constexpr uint32_t kSize = 0x90000, kDevice = 0x1000, kResource = 0x10000;
constexpr uint32_t kHeader = kResource + 0x200, kTable = kHeader + 20;
constexpr uint32_t kCursor = 0x40000, kStack = 0x80000, kSource = 0xA0100000;
constexpr uint32_t kDataSize = 24576;
constexpr std::array<PPCRegister PPCContext::*, 18> kNonvolatile = {
    &PPCContext::r14, &PPCContext::r15, &PPCContext::r16, &PPCContext::r17, &PPCContext::r18,
    &PPCContext::r19, &PPCContext::r20, &PPCContext::r21, &PPCContext::r22, &PPCContext::r23,
    &PPCContext::r24, &PPCContext::r25, &PPCContext::r26, &PPCContext::r27, &PPCContext::r28,
    &PPCContext::r29, &PPCContext::r30, &PPCContext::r31};
std::unique_ptr<GuestMemoryFixture> original, native;
std::array<uint8_t, kSize> pristine;
uint64_t cases = 0, controls = 0, constants = 0, rejections = 0;
uint64_t events = 0, waits = 0, flag_sets = 0, empty_constants = 0, high_cases = 0;
uint32_t current_kind = 0, current_case = 0, data_source = kSource;

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

PPCContext Reset(uint32_t kind, uint32_t seed) {
  auto* base = original->data();
  std::memcpy(base, pristine.data(), kSize);
  std::memset(base + kDevice, 0, 22256);
  StoreGuestU32(base, kDevice + 48, kCursor + (seed & 4));
  StoreGuestU32(base, kDevice + 52, kCursor + 4096);
  StoreGuestU32(base, kDevice + 56, kCursor + 4096);
  PPCContext ctx{};
  ctx.r1.u64 = 0xABCD000000000000ull | kStack;
  ctx.r3.u64 = 0xA5A5000000000000ull | kDevice;
  ctx.r4.u64 = 0xFEED000000000000ull | (kind ? kResource : seed * 0x91E10DA5u);
  ctx.r5.u64 = 0xBCDE000000000000ull | kSource;
  ctx.lr = 0xDEADBEEF82345678ull;
  ctx.msr = 0xAA558020;
  ctx.cr2.set_raw(seed & 15);
  ctx.cr3.set_raw((seed + 1) & 15);
  ctx.cr4.set_raw((seed + 2) & 15);
  for (uint32_t i = 0; i < kNonvolatile.size(); ++i)
    (ctx.*kNonvolatile[i]).u64 = 0x1234ABCD00000000ull | i * 0x10203u;
  return ctx;
}

void CopyLow() {
  std::memcpy(native->data(), original->data(), kSize);
}

PPCContext PrepareControl(uint32_t surface, uint32_t control, uint32_t prior, uint8_t shader_flags,
                          bool shader_bit, uint8_t flags, bool wait, uint32_t seed,
                          bool have_shader = true) {
  auto ctx = Reset(0, seed);
  auto* base = original->data();
  StoreGuestU32(base, kDevice + 12684, have_shader ? kResource : 0);
  StoreGuestU32(base, kResource + 64, 0x100);
  StoreGuestU32(base, kResource + 0x100 + 48,
                (seed * 0x1234567u & ~0x08000000u) | (shader_bit ? 0x08000000u : 0));
  StoreGuestU32(base, kDevice + 11844, ((seed & 1023) << 22) | ((surface & 3) << 20));
  StoreGuestU32(base, kDevice + 10548, control);
  StoreGuestU32(base, kDevice + 10560, prior);
  *GuestToHost(base, kDevice + 10942) = shader_flags;
  *GuestToHost(base, kDevice + 10941) = flags;
  StoreGuestU32(base, kDevice + 13488, wait ? seed | 1u : 0);
  const uint32_t cursor = LoadGuestU32(base, kDevice + 48);
  StoreGuestU32(base, kDevice + 52, cursor + 8);
  StoreGuestU32(base, kDevice + 56, cursor);
  CopyLow();
  return ctx;
}

PPCContext PrepareConstants(uint32_t count, uint32_t ending, uint32_t source, uint32_t seed) {
  auto ctx = Reset(1, seed);
  auto* base = original->data();
  ctx.r5.u32 = data_source = source;
  CommitView(*original, source, kDataSize);
  CommitView(*native, source, kDataSize);
  for (uint32_t i = 0; i < kDataSize; ++i)
    *GuestToHost(base, source + i) = uint8_t(i * 37 + (i >> 8) + seed);
  StoreGuestU32(base, kResource + 20, ending == 3 ? 0 : 0x200);
  const uint32_t bytes = count * 8 + (ending == 0 || ending == 3 ? 0 : ending == 1 ? 4 : 16);
  StoreGuestU32(base, kHeader + 16, bytes);
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t words = 1 + (seed + i * 7) % 64;
    uint32_t index = (seed * 37 + i * 11) % (513 - (words + 3) / 4);
    if ((seed & 7) == 5) {
      index = 255;
      words = 8;
    }
    if ((seed & 7) == 6) {
      index = 511;
      words = 4;
    }
    if ((seed & 7) == 7) {
      index = 0;
      words = 2048;
    }
    StoreGuestU32(base, kTable + i * 8, index << 16 | words);
    StoreGuestU32(base, kTable + i * 8 + 4, i * 64 + (seed & 4));
  }
  if (ending == 1 || ending == 2) {
    StoreGuestU32(base, kTable + count * 8, 0xFFFF0000);
    StoreGuestU32(base, kTable + count * 8 + 4, UINT32_MAX);
    StoreGuestU32(base, kTable + count * 8 + 8, UINT32_MAX);
  }
  const uint32_t cursor = LoadGuestU32(base, kDevice + 48);
  StoreGuestU32(base, kDevice + 52, cursor + count * 16);
  StoreGuestU32(base, kDevice + 56, count ? cursor + (count - 1) * 16 : 0);
  CopyLow();
  std::memcpy(GuestToHost(native->data(), source), GuestToHost(base, source), kDataSize);
  return ctx;
}

bool Native(uint32_t kind, PPCContext& ctx, uint8_t* base) {
  return kind ? port::LoadShaderConstants(ctx, base) : port::PrepareShaderControl(ctx, base);
}

void SameRange(uint32_t address, uint32_t bytes) {
  if (std::memcmp(GuestToHost(original->data(), address), GuestToHost(native->data(), address),
                  bytes)) {
    for (uint32_t i = 0; i < bytes; ++i)
      if (*GuestToHost(original->data(), address + i) !=
          *GuestToHost(native->data(), address + i)) {
        std::fprintf(stderr, "Difference at %08X: expected=%02X actual=%02X\n", address + i,
                     *GuestToHost(original->data(), address + i),
                     *GuestToHost(native->data(), address + i));
        break;
      }
    throw std::runtime_error("Shader coordination changed a field, operand or neighboring byte");
  }
}

void Compare(uint32_t kind, PPCContext input) {
  auto expected = input, actual = input;
  const uint32_t device = input.r3.u32, initial = LoadGuestU32(original->data(), device + 48);
  const uint8_t before_flags = *GuestToHost(original->data(), device + 10941);
  const uint64_t before_records = constant_records;
  if (kind)
    reference_sub_8245F810(expected, original->data());
  else
    reference_sub_8245E330(expected, original->data());
  Require(Native(kind, actual, native->data()), "Safe shader coordination was rejected");
  Require(expected.r1.u64 == actual.r1.u64 && expected.r3.u64 == actual.r3.u64 &&
              expected.lr == actual.lr && expected.msr == actual.msr &&
              expected.cr2.raw() == actual.cr2.raw() && expected.cr3.raw() == actual.cr3.raw() &&
              expected.cr4.raw() == actual.cr4.raw(),
          "Shader coordination changed stack, return, link, interrupt or condition ABI");
  for (auto reg : kNonvolatile)
    Require((expected.*reg).u64 == (actual.*reg).u64,
            "Shader coordination changed a nonvolatile GPR");
  const uint32_t cursor = LoadGuestU32(original->data(), device + 48);
  if (kind) {
    const uint32_t count = uint32_t(constant_records - before_records);
    Require(!count || cursor == initial + count * 16, "Original constant loop cursor changed");
    for (uint32_t i = 0; i < count; ++i) {
      const uint32_t at = initial + i * 16 + 4;
      const indirect::Call call{LoadGuestU32(native->data(), at + 4),
                                LoadGuestU32(native->data(), at + 8),
                                LoadGuestU32(native->data(), at + 12)};
      Require(LoadGuestU32(native->data(), at) == indirect::kHeader && indirect::Validate(call),
              "Whole descriptor loop emitted invalid native operands");
      StoreGuestU32(native->data(), at, 0xC0022F00);
    }
    empty_constants += count == 0;
    SameRange(data_source, kDataSize);
    ++constants;
  } else {
    if (cursor != initial) {
      Require(cursor == initial + 8, "Original control emitted an unexpected command length");
      const uint32_t old_header = LoadGuestU32(original->data(), initial + 4);
      const uint32_t header = LoadGuestU32(native->data(), initial + 4);
      if (old_header == 0xC0004600) {
        state::StateRange range;
        Require(state::DecodeHeader(header, 1, range) && range.first_register == 0x21F9 &&
                    range.register_count == 1 && LoadGuestU32(native->data(), initial + 8) == 15,
                "Native event flag changed its register or masked initiator");
        ++events;
      } else {
        uint32_t reg;
        Require(old_header == 0x5C8 && ports::DecodeHeader(header, 1, reg) && reg == 0x5C8 &&
                    LoadGuestU32(native->data(), initial + 8) == 0x20000,
                "Native wait port changed its register or operand");
        ++waits;
      }
      StoreGuestU32(native->data(), initial + 4, old_header);
    }
    flag_sets += !(before_flags & 4) && (*GuestToHost(original->data(), device + 10941) & 4);
    ++controls;
  }
  const uint32_t frame = kind ? 144 : 112;
  SameRange(0, kStack - frame);
  SameRange(kStack, kSize - kStack);
  SameRange(device, 22256);
  SameRange(kind ? input.r4.u32 : kResource, 0x4000);
  if (initial >= kSize && cursor != initial)
    SameRange(initial - 4, cursor - initial + 16);
  ++cases;
}

void Reject(uint32_t kind, uint32_t problem) {
  auto ctx =
      kind ? PrepareConstants(2, 0, kSource, 2) : PrepareControl(1, 2, 0, 32, false, 4, false, 2);
  auto* base = native->data();
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
      StoreGuestU32(base, kDevice + 48, kCursor + 1);
      break;
    case 4:
      StoreGuestU32(base, kDevice + 48, 0);
      break;
    case 5:
      StoreGuestU32(base, kDevice + 52, kCursor + (kind ? 28 : 4));
      break;
    case 6:
      StoreGuestU32(base, kDevice + 56, kCursor - 4);
      break;
    case 7:
      StoreGuestU32(base, kDevice + 48, kStack - 112);
      StoreGuestU32(base, kDevice + 52, kStack + 4096);
      StoreGuestU32(base, kDevice + 56, kStack + 4096);
      break;
    case 8:
      StoreGuestU32(base, kDevice + 48, kResource + 0x100);
      StoreGuestU32(base, kDevice + 52, kResource + 0x2000);
      StoreGuestU32(base, kDevice + 56, kResource + 0x2000);
      break;
    case 9:
      StoreGuestU32(base, kDevice + 48, kDevice + 128);
      StoreGuestU32(base, kDevice + 52, kDevice + 4096);
      StoreGuestU32(base, kDevice + 56, kDevice + 4096);
      break;
    case 10:
      if (kind)
        ctx.r4.u32 |= 1;
      else
        StoreGuestU32(base, kDevice + 12684, kResource + 1);
      break;
    case 11:
      StoreGuestU32(base, kResource + (kind ? 20 : 64), UINT32_MAX);
      break;
    case 12:
      StoreGuestU32(base, kResource + (kind ? 20 : 64), 3);
      break;
    case 13:
      if (kind)
        StoreGuestU32(base, kHeader + 16, 65540);
      else
        StoreGuestU32(base, kDevice + 12684, kStack - 64);
      break;
    case 14:
      if (kind)
        StoreGuestU32(base, kTable, 512u << 16 | 1);
      else
        StoreGuestU32(base, kDevice + 12684, kDevice + 128);
      break;
    case 15:
      StoreGuestU32(base, kTable + 8, 511u << 16 | 5);
      break;
    case 16:
      StoreGuestU32(base, kTable + 8, 2049);
      break;
    case 17:
      StoreGuestU32(base, kHeader + 16, 3);
      break;
    case 18:
      StoreGuestU32(base, kHeader + 16, 4);
      break;
    case 19:
      ctx.r5.u32 = 0x82100000;
      break;
    case 20:
      ctx.r5.u32 |= 1;
      break;
    case 21:
      StoreGuestU32(base, kDevice + 56, kCursor + 12);
      break;
    case 22:
      StoreGuestU32(base, kHeader + 16, 129 * 8);
      for (uint32_t i = 0; i < 129; ++i) {
        StoreGuestU32(base, kTable + i * 8, 1);
        StoreGuestU32(base, kTable + i * 8 + 4, 0);
      }
      StoreGuestU32(base, kDevice + 52, kCursor + 4096);
      StoreGuestU32(base, kDevice + 56, kCursor + 4096);
      break;
    default:
      throw std::runtime_error("Unknown shader rejection");
  }
  std::vector<uint8_t> before(base, base + kSize);
  std::array<uint8_t, sizeof(PPCContext)> context;
  std::memcpy(context.data(), &ctx, context.size());
  Require(!Native(kind, ctx, base), "Unsafe shader coordination was accepted");
  Require(!std::memcmp(context.data(), &ctx, context.size()) &&
              !std::memcmp(base, before.data(), before.size()),
          "Shader coordination rejection left partial writes");
  ++rejections;
}

void Views() {
  constexpr std::array<uint32_t, 6> views = {0x7F000000, 0x80000000, 0x90000000,
                                             0xA0000000, 0xC0000000, 0xE0000000};
  for (uint32_t kind : {0u, 1u})
    for (uint32_t part = 0; part < 3; ++part)
      for (uint32_t view : views)
        for (uint32_t align : {0u, 4u}) {
          auto ctx = kind ? PrepareConstants(7, 1, kSource, 5)
                          : PrepareControl(1, 2, 0, 32, false, 4, false, 5);
          const uint32_t old = part == 0 ? kDevice : part == 1 ? kResource : kCursor + 4;
          const uint32_t address = view + old + align;
          const uint32_t span = part == 0 ? 22256 : part == 1 ? 0x4000 : 4096;
          for (auto* memory : {original.get(), native.get()}) {
            CommitView(*memory, address, span);
            std::memcpy(GuestToHost(memory->data(), address), GuestToHost(memory->data(), old),
                        span);
            if (part == 2) {
              StoreGuestU32(memory->data(), kDevice + 48, address);
              StoreGuestU32(memory->data(), kDevice + 52, address + 4096);
              StoreGuestU32(memory->data(), kDevice + 56, address + 4096);
            }
            if (part == 1 && !kind)
              StoreGuestU32(memory->data(), kDevice + 12684, address);
          }
          if (part == 0)
            ctx.r3.u32 = address;
          if (part == 1 && kind)
            ctx.r4.u32 = address;
          Compare(kind, ctx);
          SameRange(address, span);
          ++high_cases;
        }
}

void AliasRejections() {
  for (uint32_t problem = 0; problem < 4; ++problem) {
    const uint32_t kind = problem >= 2;
    auto ctx =
        kind ? PrepareConstants(2, 0, kSource, 2) : PrepareControl(1, 2, 0, 32, false, 4, false, 2);
    auto* base = native->data();
    const uint32_t low = problem == 0 ? kDevice : problem == 2 ? kCursor : kResource;
    const uint32_t address = 0xC0000000u + (problem == 2 ? kResource : low);
    const uint32_t span = problem == 0 ? 22256 : 0x4000;
    CommitView(*native, address, span);
    std::memcpy(GuestToHost(base, address), GuestToHost(base, low), span);
    if (problem == 0)
      ctx.r3.u32 = address;
    if (problem == 1)
      StoreGuestU32(base, kDevice + 12684, address);
    if (problem == 3)
      ctx.r4.u32 = address;
    const uint32_t output = address - 0x20000000u +
                            (problem == 1   ? 0x100
                             : problem == 3 ? 0x200
                                            : 0);
    StoreGuestU32(base, ctx.r3.u32 + 48, output);
    StoreGuestU32(base, ctx.r3.u32 + 52, output + 4096);
    StoreGuestU32(base, ctx.r3.u32 + 56, output + 4096);
    if (problem == 2) {
      // The first source aliases only the second command, through a different
      // physical view. Reject the whole loop before publishing its first row.
      ctx.r5.u32 = output;
      StoreGuestU32(base, kTable, 1);
      StoreGuestU32(base, kTable + 4, 24);
      StoreGuestU32(base, kTable + 12, 4096);
    }
    std::vector<uint8_t> low_before(base, base + kSize);
    std::vector<uint8_t> high_before(GuestToHost(base, address), GuestToHost(base, address) + span);
    std::array<uint8_t, sizeof(PPCContext)> context;
    std::memcpy(context.data(), &ctx, context.size());
    Require(!Native(kind, ctx, base), "Aliased shader coordination was accepted");
    Require(!std::memcmp(context.data(), &ctx, context.size()) &&
                !std::memcmp(base, low_before.data(), low_before.size()) &&
                !std::memcmp(GuestToHost(base, address), high_before.data(), high_before.size()),
            "Aliased shader coordination left partial writes");
    ++rejections;
  }
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
    constexpr std::array<uint32_t, 8> hazards = {0,          0x80,     0x800,      0x20000,
                                                 0x20000000, 0x800000, 0xE38E3800, 0xE38E3880};
    for (uint32_t surface = 0; surface < 4; ++surface)
      for (uint32_t mode = 0; mode < 8; ++mode)
        for (uint32_t low = 0; low < 8; ++low)
          for (uint32_t bits = 0; bits < 32; ++bits) {
            current_kind = 0;
            current_case = uint32_t(cases);
            const uint32_t seed = mode * 71 + low * 17 + bits * 3 + surface;
            const uint32_t prior = (seed * 0x91E10DA5u & ~16u) | (bits & 1 ? 16 : 0);
            const uint8_t shader = uint8_t((seed * 67 & ~32u) | (bits & 2 ? 32 : 0));
            const uint8_t flags = uint8_t((seed * 73 & ~4u) | (bits & 8 ? 4 : 0));
            Compare(0, PrepareControl(surface, mode * 16 | low | hazards[(seed >> 2) & 7], prior,
                                      shader, bits & 4, flags, bits & 16, seed));
          }
    for (uint32_t count : {0u, 1u, 2u, 7u, 31u, 128u})
      for (uint32_t ending = 0; ending < 4; ++ending)
        for (uint32_t source : {0xA0100000u, 0xC0100000u, 0xE0100000u})
          for (uint32_t seed = 0; seed < 8; ++seed) {
            current_kind = 1;
            current_case = uint32_t(cases);
            Compare(1, PrepareConstants(count, ending, source, seed));
          }
    Views();
    // No command branch reads or validates the unused cursor/source operands.
    for (uint32_t kind : {0u, 1u}) {
      auto ctx = kind ? PrepareConstants(0, 3, kSource, 1)
                      : PrepareControl(0, 0, 0, 0, false, 0, false, 1, false);
      for (auto* base : {original->data(), native->data()}) {
        StoreGuestU32(base, kDevice + 48, UINT32_MAX);
        StoreGuestU32(base, kDevice + 52, 0);
        StoreGuestU32(base, kDevice + 56, 0);
      }
      ctx.r5.u32 = UINT32_MAX;
      Compare(kind, ctx);
    }
    for (uint32_t kind : {0u, 1u})
      for (uint32_t problem = 0; problem < (kind ? 23u : 15u); ++problem) {
        current_kind = kind;
        current_case = 100000 + problem;
        Reject(kind, problem);
      }
    AliasRejections();
    Require(control_calls == controls && constant_calls == constants && constant_records &&
                events && waits && flag_sets && empty_constants && high_cases,
            "Shader oracle missed a real routine or state transition");
    std::printf(
        "{\"passed\":true,\"cases\":%llu,\"control_cases\":%llu,\"constant_cases\":%llu,"
        "\"unchanged_fallbacks\":%llu,\"real_constant_records\":%llu,\"event_flags\":%llu,"
        "\"wait_ports\":%llu,\"flag_sets\":%llu,\"empty_constant_cases\":%llu,\"high_view_cases\":%"
        "llu}\n",
        (unsigned long long)cases, (unsigned long long)controls, (unsigned long long)constants,
        (unsigned long long)rejections, (unsigned long long)constant_records,
        (unsigned long long)events, (unsigned long long)waits, (unsigned long long)flag_sets,
        (unsigned long long)empty_constants, (unsigned long long)high_cases);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s; kind=%u case=%u\n", error.what(), current_kind, current_case);
    return 1;
  }
}
