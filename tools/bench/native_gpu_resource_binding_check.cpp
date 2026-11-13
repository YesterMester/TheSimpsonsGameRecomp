#include "simpsons_init.h"
#include "guest_memory_fixture.h"
#include "native_gpu_resource_binding.h"
#include "native_gpu_resource_initialization.h"
#include <rex/ppc/barriers.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <vector>

static uint64_t copy_calls = 0, clear_calls = 0;
#include "native_gpu_resource_binding_reference.inc"

namespace port = simpsons::native_gpu_calls;
namespace binding = port::resource_binding_detail;
using simpsons::LoadGuestU32;
using simpsons::StoreGuestU32;
namespace {
constexpr uint32_t kSize = 0x100000, kDevice = 0x1000, kOld = 0x10000, kNew = 0x20000;
constexpr uint32_t kCode = 0x50000, kInput = 0x60000, kQueue = 0x70000, kStack = 0xF0000;
constexpr std::array<uint32_t, 7> frames = {136, 136, 160, 0, 0, 168, 144};
constexpr std::array<PPCRegister PPCContext::*, 18> kNonvolatile = {
    &PPCContext::r14, &PPCContext::r15, &PPCContext::r16, &PPCContext::r17, &PPCContext::r18,
    &PPCContext::r19, &PPCContext::r20, &PPCContext::r21, &PPCContext::r22, &PPCContext::r23,
    &PPCContext::r24, &PPCContext::r25, &PPCContext::r26, &PPCContext::r27, &PPCContext::r28,
    &PPCContext::r29, &PPCContext::r30, &PPCContext::r31};
std::unique_ptr<GuestMemoryFixture> original, native;
std::array<uint8_t, kSize> pristine;
uint64_t cases = 0, rejected = 0, queued = 0, stamped = 0;
std::array<uint64_t, 7> families{};
uint32_t current_kind = 0, current_mode = 0, current_seed = 0;

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

void Metadata(uint8_t* base, uint32_t resource, bool vertex, uint32_t mode, uint32_t seed) {
  const uint32_t prefix = vertex ? 872 : 40, header = resource + prefix + 0x400;
  StoreGuestU32(base, resource + prefix + 20, mode == 0 ? 0 : 0x400);
  binding::Store64(base, header, 0xF0E1D2C3B4A59687ull ^ seed);
  binding::Store64(base, header + 8, seed & 1 ? 1ull << 63 : 0);
  std::vector<uint32_t> words;
  const auto pair = [](uint32_t offset, uint32_t count) { return offset << 16 | count; };
  if (mode == 2 || mode == 6) {
    words.push_back(pair(0x1234, 511));
    words.push_back(0xFEE1DEAD);
  }
  if (mode >= 2)
    words.push_back(0);
  if (mode == 3 || mode == 5 || mode == 6) {
    words.push_back(pair(256, 8));
    for (uint32_t i = 0; i < 8; ++i)
      words.push_back(0x10203040u * (i + 1) ^ seed);
    if (mode != 3) {
      words.push_back(pair(260, 3));
      words.insert(words.end(), {0xFEDCBA98, 0x76543210, seed});
    }
  }
  if (mode >= 4) {
    words.push_back(0);
    words.push_back(pair(256, 8));
    for (uint32_t i = 0; i < 4; ++i) {
      words.push_back(0xFF00FF00u >> i);
      words.push_back(seed | (0x10203u << i));
    }
    words.push_back(pair(257, 4));
    words.insert(words.end(), {0xF0F0F0F0, 0x01020304, 0x0F0F0F0F, 0xAABBCCDD});
    words.push_back(0);
    if (mode == 6)
      words.push_back(0xDEADBEEF);  // Ignored after the final marker.
  }
  StoreGuestU32(base, header + 16, uint32_t(words.size() * 4));
  for (uint32_t i = 0; i < words.size(); ++i)
    StoreGuestU32(base, header + 20 + i * 4, words[i]);
}

PPCContext Prepare(uint32_t kind, uint32_t mode, uint32_t seed, uint32_t retirement = 0,
                   uint32_t slot = 0, uint32_t count = 7, uint32_t second_count = 7,
                   uint32_t variants = 2) {
  auto* base = original->data();
  std::memcpy(base, pristine.data(), kSize);
  std::memset(base + kDevice, 0, 22256);
  uint32_t random = seed + 1;
  for (uint32_t i = 0; i < 5; ++i)
    binding::Store64(base, kDevice + i * 8, (uint64_t(Next(random)) << 32) | Next(random));
  for (uint32_t i = 1152; i < 10896; i += 4)
    StoreGuestU32(base, kDevice + i, Next(random));
  const uint32_t cursor = kQueue + 8 + ((seed * 17) % 249) * 8;
  StoreGuestU32(base, kDevice + 13496, kQueue);
  StoreGuestU32(base, kDevice + 13500, cursor);
  StoreGuestU32(base, kDevice + 13504, kQueue + 2008);
  *simpsons::GuestToHost(base, kDevice + 10941) = 0;
  *simpsons::GuestToHost(base, kDevice + 10942) = uint8_t(seed * 37);
  StoreGuestU32(base, kDevice + 10908, retirement == 1 || retirement == 4 ? 0xFFFFFFFE : 0);
  StoreGuestU32(base, kDevice + 10912, retirement == 3 || retirement == 5 ? 1 : 0);
  StoreGuestU32(base, kOld, 1);
  const uint32_t old = retirement == 0 ? 0 : retirement >= 4 ? kNew : kOld;
  const uint32_t pointer = kind == 2 ? 12536 + slot * 4 : kind == 1 ? 12688 : 12684;
  if (kind < 3)
    StoreGuestU32(base, kDevice + pointer, old);
  if (kind < 2)
    Metadata(base, kNew, kind == 1, mode, seed);
  StoreGuestU32(base, kNew, 1);
  if (kind == 2) {
    for (uint32_t i = 0; i < 6; ++i)
      StoreGuestU32(base, kNew + 28 + i * 4, Next(random));
    constexpr std::array<uint8_t, 6> lod = {0, 1, 15, 16, 31, 255};
    *simpsons::GuestToHost(base, kDevice + 11942 + slot) = lod[seed % lod.size()];
    *simpsons::GuestToHost(base, kDevice + 11968 + slot) = lod[(seed + 3) % lod.size()];
  }
  if (kind == 5) {
    StoreGuestU32(base, kNew + 872, variants == 2 ? 32 : 0);
    for (uint32_t variant = 0; variant < variants; ++variant) {
      const uint32_t relative = 0x2000 + variant * 0x400, header = kNew + relative + 872;
      StoreGuestU32(base, kNew + 896 + variant * 8, relative);
      StoreGuestU32(base, header, variant * 128 + (seed & 3));
      StoreGuestU32(base, header + 24, 4);
      const uint32_t entries = variant ? second_count : count;
      StoreGuestU32(base, header + 28, entries);
      for (uint32_t i = 0; i < entries; ++i)
        StoreGuestU32(base, header + 52 + i * 4,
                      ((seed & 1) ? (i & 1 ? 4095 : 0) : Next(random) & 4095) | 0xABC00000);
    }
  }
  if (kind == 6) {
    for (uint32_t i = 0; i < count; ++i) {
      StoreGuestU32(base, kInput + i * 12, ((i + seed) & 15) << 16 | (seed * 7 + i));
      StoreGuestU32(base, kInput + i * 12 + 4, Next(random));
      StoreGuestU32(base, kInput + i * 12 + 8, Next(random));
    }
    StoreGuestU32(base, kInput + count * 12, 0x00FF00AB);
  }
  if (kind < 3) {
    const uint32_t frame = kind == 2 ? 160 : 128;
    StoreGuestU32(base, kStack - frame + 80, 0x40000000u | (seed & 1 ? 0x80000000u : 0) | seed);
  }
  std::memcpy(native->data(), base, kSize);
  PPCContext ctx{};
  ctx.r1.u64 = 0xABCD000000000000ull | kStack;
  ctx.r3.u64 = 0xA5A5000000000000ull | (kind == 4 || kind == 5 ? kNew
                                        : kind == 6            ? kInput
                                                               : kDevice);
  ctx.r4.u64 = 0xFEED000000000000ull | (kind == 2                ? slot
                                        : kind == 4 || kind == 5 ? kCode
                                        : mode == 7 && kind < 2  ? 0
                                                                 : kNew);
  ctx.r5.u64 = 0xBCDE000000000000ull | (mode == 7 ? 0 : kNew);
  ctx.r6.u64 = (uint64_t(Next(random)) << 32) | Next(random);
  ctx.lr = 0xDEADBEEF82345678ull;
  ctx.msr = 0xA5A58020;
  for (uint32_t i = 0; i < kNonvolatile.size(); ++i)
    (ctx.*kNonvolatile[i]).u64 = 0x1234ABCD00000000ull | (i * 0x10203u);
  return ctx;
}

bool Native(uint32_t kind, PPCContext& ctx, uint8_t* base) {
  switch (kind) {
    case 0:
      return port::BindShaderResource(ctx, base, false);
    case 1:
      return port::BindShaderResource(ctx, base, true);
    case 2:
      return port::BindTextureResource(ctx, base);
    case 3:
      return port::BindVertexDeclaration(ctx, base);
    case 4:
      return port::InitializePixelResource(ctx, base);
    case 5:
      return port::InitializeVertexShaderTemplates(ctx, base);
    case 6:
      return port::InitializeVertexDeclaration(ctx, base);
  }
  return false;
}

void Compare(uint32_t kind, PPCContext input) {
  constexpr std::array reference = {reference_sub_82445278, reference_sub_82445578,
                                    reference_sub_824408E0, reference_sub_82445798,
                                    reference_sub_82445240, reference_sub_82445480,
                                    reference_sub_824457F0};
  auto expected = input, actual = input;
  const uint32_t device = kind < 4 ? input.r3.u32 : kDevice;
  const uint32_t queue = LoadGuestU32(original->data(), device + 13500);
  const uint32_t old_slot = kind == 2 ? 12536 + input.r4.u32 * 4 : kind == 1 ? 12688 : 12684;
  const uint32_t old = kind < 3 ? LoadGuestU32(original->data(), device + old_slot) : 0;
  reference[kind](expected, original->data());
  Require(Native(kind, actual, native->data()), "Safe resource binding/initializer was rejected");
  Require(expected.r1.u64 == actual.r1.u64 && expected.r3.u64 == actual.r3.u64 &&
              expected.lr == actual.lr && expected.msr == actual.msr,
          "Resource binding/initializer changed return, stack, link or interrupt ABI");
  for (auto reg : kNonvolatile)
    Require((expected.*reg).u64 == (actual.*reg).u64, "Resource binding changed a nonvolatile GPR");
  const uint32_t frame = frames[kind];
  if (std::memcmp(original->data(), native->data(), kStack - frame) ||
      std::memcmp(original->data() + kStack, native->data() + kStack, kSize - kStack)) {
    for (uint32_t i = 0; i < kSize; ++i)
      if (!(i >= kStack - frame && i < kStack) && original->data()[i] != native->data()[i]) {
        std::fprintf(stderr, "Difference at %08X: expected=%02X actual=%02X\n", i,
                     original->data()[i], native->data()[i]);
        break;
      }
    throw std::runtime_error(
        "Binding/initializer changed resources, descriptors, patches or neighboring bytes");
  }
  if (kind < 3) {
    queued += queue != LoadGuestU32(original->data(), device + 13500);
    stamped += old && LoadGuestU32(original->data(), device + 10908);
  }
  ++cases;
  ++families[kind];
}

void Reject(uint32_t kind, uint32_t problem) {
  auto ctx = Prepare(kind, 6, 3, 3);
  auto* base = native->data();
  switch (problem) {
    case 0:
      ctx.r3.u32 |= 1;
      break;
    case 1:
      ctx.r1.u32 |= 1;
      break;
    case 2:
      ctx.r1.u32 = 64;
      break;
    case 3:
      if (kind < 2)
        ctx.r4.u32 |= 1;
      else
        ctx.r5.u32 |= 1;
      break;
    case 4:
      StoreGuestU32(base, kDevice + 13500, kQueue + 2008);
      break;
    case 5:
      StoreGuestU32(base, kDevice + 13504, kQueue + 2000);
      break;
    case 6:
      *simpsons::GuestToHost(base, kDevice + 10941) = 0x20;
      break;
    case 7:
      StoreGuestU32(base, kDevice + 13496, kDevice);
      break;
    case 8:
      StoreGuestU32(base, kDevice + 13500, kQueue + 9);
      break;
    case 9:
      if (kind == 2)
        ctx.r4.u32 = 32;
      else
        StoreGuestU32(base, kNew + (kind == 1 ? 872 : 40) + 20, UINT32_MAX);
      break;
    case 10:
      if (kind == 2)
        ctx.r5.u32 = kDevice;
      else
        ctx.r4.u32 = kDevice;
      break;
    case 11:
      if (kind == 2)
        ctx.r5.u32 = kStack - 32;
      else
        ctx.r4.u32 = kStack - 32;
      break;
    case 12:
      StoreGuestU32(base, kDevice + (kind == 2 ? 12536 : kind == 1 ? 12688 : 12684), kDevice);
      break;
    case 13:
      StoreGuestU32(base, kDevice + (kind == 2 ? 12536 : kind == 1 ? 12688 : 12684), kStack - 32);
      break;
    case 14:
      if (kind == 2) {
        ctx.r5.u32 = kOld + 4;
      } else {
        const uint32_t header = kNew + (kind == 1 ? 872 : 40) + 0x400;
        StoreGuestU32(base, header + 16, 0x10004);
      }
      break;
    case 15:
      if (kind == 2) {
        StoreGuestU32(base, kDevice + 13496, 0);
      } else {
        const uint32_t header = kNew + (kind == 1 ? 872 : 40) + 0x400;
        StoreGuestU32(base, header + 16, 3);
      }
      break;
    case 16:
    case 17:
    case 18:
      if (kind == 2) {
        StoreGuestU32(base, kDevice + 13500, kQueue);
      } else {
        const uint32_t header = kNew + (kind == 1 ? 872 : 40) + 0x400;
        StoreGuestU32(base, header + 16, problem == 16 ? 24 : 16);
        StoreGuestU32(base, header + 20, 0);
        StoreGuestU32(base, header + 24, problem == 16 ? 0 : problem == 17 ? 0xFFFF0001 : 100);
        StoreGuestU32(base, header + 28, problem == 16 ? 3 : 0);
        StoreGuestU32(base, header + 32, 0);
      }
      break;
  }
  auto saved = ctx;
  std::memcpy(original->data(), base, kSize);
  Require(!Native(kind, ctx, base), "Unsafe resource binding was accepted");
  Require(!std::memcmp(&ctx, &saved, sizeof(ctx)) && !std::memcmp(base, original->data(), kSize),
          "Rejected binding changed context or partially updated resources");
  ++rejected;
}

void RejectInitializer(uint32_t kind, uint32_t problem) {
  auto ctx = Prepare(kind, 0, 3, 0, 0, 7);
  auto* base = native->data();
  if (problem == 0)
    (kind == 6 ? ctx.r4 : ctx.r3).u32 |= 1;
  else if (problem == 1)
    (kind == 6 ? ctx.r4 : ctx.r3).u32 = 0xFFFFFFF0;
  else if (kind == 5) {
    constexpr uint32_t header = kNew + 0x2000 + 872;
    switch (problem) {
      case 2:
        ctx.r1.u32 |= 1;
        break;
      case 3:
        ctx.r1.u32 = 64;
        break;
      case 4:
        StoreGuestU32(base, kNew + 896, UINT32_MAX);
        break;
      case 5:
        StoreGuestU32(base, header + 28, 33);
        break;
      case 6:
        StoreGuestU32(base, header + 24, UINT32_MAX);
        break;
      case 7:
        ctx.r4.u32 = kNew;
        StoreGuestU32(base, header, 0);
        StoreGuestU32(base, header + 52, 0);
        break;
      case 8:
        ctx.r4.u32 = kStack - 16;
        StoreGuestU32(base, header, 0);
        StoreGuestU32(base, header + 52, 0);
        break;
      case 9:
        ctx.r4.u32 = UINT32_MAX - 3;
        StoreGuestU32(base, header, 0);
        StoreGuestU32(base, header + 52, 0);
        break;
    }
  } else if (kind == 6) {
    switch (problem) {
      case 2:
        ctx.r1.u32 |= 1;
        break;
      case 3:
        ctx.r1.u32 = 64;
        break;
      case 4:
        StoreGuestU32(base, kInput, 16u << 16);
        break;
      case 5:
        for (uint32_t i = 0; i <= 256; ++i)
          StoreGuestU32(base, kInput + i * 12, 0);
        break;
      case 6:
        ctx.r4.u32 = kInput;
        break;
      case 7:
        ctx.r3.u32 = kStack - 32;
        break;
      case 8:
        ctx.r4.u32 = kStack - 32;
        break;
      case 9:
        ctx.r3.u32 = UINT32_MAX - 3;
        break;
    }
  }
  const auto saved = ctx;
  std::memcpy(original->data(), base, kSize);
  Require(!Native(kind, ctx, base), "Unsafe resource initializer was accepted");
  Require(!std::memcmp(&ctx, &saved, sizeof(ctx)) && !std::memcmp(base, original->data(), kSize),
          "Rejected initializer cleared output, copied templates or changed context");
  ++rejected;
}

void HighViews() {
  for (uint32_t view :
       {0x7F040000u, 0x82040000u, 0x90040000u, 0xA0040000u, 0xC0040000u, 0xE0040000u})
    for (uint32_t alignment : {0u, 4u})
      for (uint32_t kind = 0; kind < 7; ++kind)
        for (uint32_t role = 0; role < (kind < 3 ? 4u : 1u); ++role) {
          const uint32_t address = view + alignment;
          auto ctx = Prepare(kind, 6, 3, 3, 7, 7);
          const uint32_t source = kind == 6                ? kNew
                                  : kind == 4 || kind == 5 ? kNew
                                  : role == 0              ? kDevice
                                  : role == 1              ? kNew
                                  : role == 2              ? kOld
                                                           : kQueue;
          const uint32_t bytes = source == kDevice ? 22256 : source == kQueue ? 2008 : 0x8000;
          // The current retirement block is eight-byte aligned in the real allocator.
          if (source == kQueue && alignment)
            continue;
          for (auto* memory : {original.get(), native.get()}) {
            memory->Commit(address, bytes + 4096);
            std::memcpy(simpsons::GuestToHost(memory->data(), address), memory->data() + source,
                        bytes);
            if (source == kQueue) {
              const uint32_t cursor = LoadGuestU32(memory->data(), kDevice + 13500) - kQueue;
              StoreGuestU32(memory->data(), kDevice + 13496, address);
              StoreGuestU32(memory->data(), kDevice + 13500, address + cursor);
              StoreGuestU32(memory->data(), kDevice + 13504, address + 2008);
            } else if (source == kOld) {
              StoreGuestU32(memory->data(),
                            kDevice + (kind == 2   ? 12536 + 7 * 4
                                       : kind == 1 ? 12688
                                                   : 12684),
                            address);
            }
          }
          if (source == kDevice || kind == 4 || kind == 5)
            ctx.r3.u32 = address;
          else if (source == kNew)
            (kind == 2 ? ctx.r5 : ctx.r4).u32 = address;
          Compare(kind, ctx);
          Require(!std::memcmp(simpsons::GuestToHost(original->data(), address),
                               simpsons::GuestToHost(native->data(), address), bytes),
                  "Binding/initializer changed high-view data or publication");
        }
}

void TableBoundaries() {
  for (uint32_t kind : {0u, 1u}) {
    for (uint32_t count : {128u, 129u}) {
      auto ctx = Prepare(kind, 6, 3, 3);
      const uint32_t header = kNew + (kind == 1 ? 872 : 40) + 0x400;
      for (auto* base : {original->data(), native->data()}) {
        StoreGuestU32(base, header + 16, 4 + count * 8);
        StoreGuestU32(base, header + 20, 0);
        for (uint32_t i = 0; i < count; ++i) {
          StoreGuestU32(base, header + 24 + i * 8, ((i % 32) * 4) << 16 | 1);
          StoreGuestU32(base, header + 28 + i * 8, i * 0x9E3779B9u);
        }
      }
      if (count == 128)
        Compare(kind, ctx);
      else {
        const auto saved = ctx;
        Require(!Native(kind, ctx, native->data()) && !std::memcmp(&ctx, &saved, sizeof(ctx)) &&
                    !std::memcmp(native->data(), original->data(), kSize),
                "Oversized patch plan partially retired or rebound the shader");
        ++rejected;
      }
    }
    for (uint32_t bytes : {4u, 65536u}) {
      auto ctx = Prepare(kind, 6, 3, 3);
      const uint32_t header = kNew + (kind == 1 ? 872 : 40) + 0x400;
      for (auto* base : {original->data(), native->data()}) {
        StoreGuestU32(base, header + 16, bytes);
        for (uint32_t i = 0; i < bytes; i += 4)
          StoreGuestU32(base, header + 20 + i, 0x12340001);
      }
      Compare(kind, ctx);
    }
  }
}

void AliasRejections() {
  for (uint32_t kind : {0u, 1u, 2u, 5u, 6u}) {
    auto ctx = Prepare(kind, 6, 3, 3, 0, 7);
    auto* base = native->data();
    uint32_t watched = 0, bytes = 0;
    if (kind < 3) {
      native->Commit(0xC0040000, 0x9000);
      std::memcpy(simpsons::GuestToHost(base, 0xC0040000), base + kDevice, 22256);
      ctx.r3.u32 = 0xC0040000;
      (kind == 2 ? ctx.r5 : ctx.r4).u32 = 0xA0040000;
      watched = 0xC0040000;
      bytes = 22256;
    } else if (kind == 5) {
      native->Commit(0xA0040000, 0x9000);
      std::memcpy(simpsons::GuestToHost(base, 0xA0040000), base + kNew, 0x8000);
      ctx.r3.u32 = 0xA0040000;
      ctx.r4.u32 = 0xC0040000;
      StoreGuestU32(base, ctx.r3.u32 + 0x2000 + 872, 0);
      StoreGuestU32(base, ctx.r3.u32 + 0x2000 + 924, 0);
      watched = 0xA0040000;
      bytes = 0x8000;
    } else {
      native->Commit(0xA0040000, 0x9000);
      std::memcpy(simpsons::GuestToHost(base, 0xA0040000), base + kInput, 0x8000);
      ctx.r3.u32 = 0xA0040000;
      ctx.r4.u32 = 0xC0040000;
      watched = 0xA0040000;
      bytes = 0x8000;
    }
    const auto saved = ctx;
    std::memcpy(original->data(), base, kSize);
    std::vector<uint8_t> before(simpsons::GuestToHost(base, watched),
                                simpsons::GuestToHost(base, watched) + bytes);
    Require(!Native(kind, ctx, base) && !std::memcmp(&ctx, &saved, sizeof(ctx)) &&
                !std::memcmp(base, original->data(), kSize) &&
                !std::memcmp(before.data(), simpsons::GuestToHost(base, watched), bytes),
            "Physical alias rejection changed resource/device backing");
    ++rejected;
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
    for (uint32_t kind = 0; kind < 2; ++kind)
      for (uint32_t mode = 0; mode < 8; ++mode)
        for (uint32_t retirement = 0; retirement < 6; ++retirement)
          for (uint32_t seed = 0; seed < 16; ++seed) {
            current_kind = kind;
            current_mode = mode;
            current_seed = seed;
            Compare(kind, Prepare(kind, mode, seed, retirement));
          }
    for (uint32_t slot = 0; slot < 32; ++slot)
      for (uint32_t retirement = 0; retirement < 6; ++retirement)
        for (uint32_t seed = 0; seed < 12; ++seed)
          for (uint32_t mode : {0u, 7u}) {
            current_kind = 2;
            current_mode = slot;
            current_seed = seed;
            Compare(2, Prepare(2, mode, seed, retirement, slot));
          }
    for (uint32_t kind : {3u, 4u})
      for (uint32_t seed = 0; seed < 32; ++seed) {
        current_kind = kind;
        current_seed = seed;
        Compare(kind, Prepare(kind, 0, seed));
      }
    for (uint32_t variants : {1u, 2u})
      for (uint32_t count : {0u, 1u, 7u, 32u})
        for (uint32_t second : {0u, 1u, 7u, 32u})
          for (uint32_t seed = 0; seed < 8; ++seed) {
            current_kind = 5;
            current_mode = count;
            current_seed = seed;
            Compare(5, Prepare(5, 0, seed, 0, 0, count, second, variants));
          }
    for (uint32_t count : {0u, 1u, 2u, 7u, 16u, 31u, 32u, 128u, 256u})
      for (uint32_t seed = 0; seed < 16; ++seed) {
        current_kind = 6;
        current_mode = count;
        current_seed = seed;
        Compare(6, Prepare(6, 0, seed, 0, 0, count));
      }
    TableBoundaries();
    HighViews();
    // The two leaf writers do not use a frame or truncate a caller's link.
    for (uint32_t kind : {3u, 4u}) {
      auto ctx = Prepare(kind, 0, 3);
      ctx.r1.u32 = 1;
      Compare(kind, ctx);
    }
    for (uint32_t kind = 0; kind < 3; ++kind) {
      auto ctx = Prepare(kind, 6, 3, 3);
      for (auto* base : {original->data(), native->data()})
        StoreGuestU32(base, kDevice + 13500, kQueue + 2000);
      Compare(kind, ctx);
    }
    for (uint32_t kind = 0; kind < 3; ++kind)
      for (uint32_t problem = 0; problem < 19; ++problem) {
        current_kind = kind;
        current_mode = problem;
        Reject(kind, problem);
      }
    for (uint32_t kind : {4u, 5u, 6u})
      for (uint32_t problem = 0; problem < (kind == 4 ? 2u : 10u); ++problem) {
        current_kind = kind;
        current_mode = problem;
        RejectInitializer(kind, problem);
      }
    AliasRejections();
    Require(copy_calls && clear_calls && queued && stamped,
            "Binding oracle missed copies, clears or resource retirement");
    std::printf(
        "{\"passed\":true,\"cases\":%llu,\"families\":[%llu,%llu,%llu,%llu,%llu,%llu,%llu],"
        "\"real_memcpy_calls\":%llu,\"real_memset_calls\":%llu,\"queue_appends\":%llu,"
        "\"fence_stamps\":%llu,\"unchanged_fallbacks\":%llu}\n",
        (unsigned long long)cases, (unsigned long long)families[0], (unsigned long long)families[1],
        (unsigned long long)families[2], (unsigned long long)families[3],
        (unsigned long long)families[4], (unsigned long long)families[5],
        (unsigned long long)families[6], (unsigned long long)copy_calls,
        (unsigned long long)clear_calls, (unsigned long long)queued, (unsigned long long)stamped,
        (unsigned long long)rejected);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s; kind=%u mode=%u seed=%u\n", error.what(), current_kind, current_mode,
                 current_seed);
    return 1;
  }
}
