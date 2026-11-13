// The driver extracts the current generated Begin/End and allocator bodies.
// Unsupported reference branches fail the check; they never fabricate data.
#include "simpsons_init.h"
#include "native_gpu_direct_draws.h"
#include "native_gpu_vertices.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <tuple>
#include <vector>

static uint64_t reference_begin_calls = 0, allocator_body_calls = 0;
#include "native_gpu_vertices_reference.inc"

namespace port = simpsons::native_gpu_vertices;
namespace codec = rex::graphics::native_gpu_calls;
using simpsons::LoadGuestU32;
using simpsons::StoreGuestU32;

namespace {
void Require(bool pass, const char* message) {
  if (!pass)
    throw std::runtime_error(message);
}
constexpr uint32_t kDevice = 0x1000, kSync = 0x8000, kCursor = 0x10000, kStack = 0x1F0000;
constexpr size_t kSize = 2 * 1024 * 1024;
alignas(4096) std::array<uint8_t, kSize> reference_memory, native_memory;
constexpr std::array<PPCRegister PPCContext::*, 18> kNonvolatile = {
    &PPCContext::r14, &PPCContext::r15, &PPCContext::r16, &PPCContext::r17, &PPCContext::r18,
    &PPCContext::r19, &PPCContext::r20, &PPCContext::r21, &PPCContext::r22, &PPCContext::r23,
    &PPCContext::r24, &PPCContext::r25, &PPCContext::r26, &PPCContext::r27, &PPCContext::r28,
    &PPCContext::r29, &PPCContext::r30, &PPCContext::r31};

auto Values(const codec::VerticesDrawCall& call) {
  return std::tie(call.first_vertex, call.fetch, call.constant_flush,
                  call.viz_query_condition, call.initiator);
}
codec::VerticesDrawCall Original(const std::array<uint32_t, 16>& words) {
  Require(words[0] == 0x2102 && words[2] == 0x548BA && words[9] == 0x25000 &&
              words[13] == 0xC0012201,
          "Reference changed its state or predicated-draw contract");
  return {words[1], {words[3], words[4], words[5], words[6], words[7], words[8]},
          {words[10], words[11], words[12]}, words[14], words[15]};
}
std::array<uint32_t, 16> Words(const uint8_t* base) {
  std::array<uint32_t, 16> words;
  for (uint32_t i = 0; i < words.size(); ++i)
    words[i] = LoadGuestU32(base, kCursor + 4 + i * 4);
  return words;
}
void Abi(const PPCContext& a, const PPCContext& b) {
  Require(a.r1.u64 == b.r1.u64 && a.lr == b.lr && a.r3.u64 == b.r3.u64,
          "Native vertices changed its stack, return value or link ABI");
  // The generated function saves r22-r31; the earlier nonvolatile GPRs also
  // remain untouched. Volatile scratch registers are not output parameters.
  for (auto field : kNonvolatile)
    Require((a.*field).u64 == (b.*field).u64, "Native vertices changed a nonvolatile GPR");
}
void Prepare(uint32_t stride, uint32_t pattern) {
  reference_memory.fill(0xCD);
  auto* base = reference_memory.data();
  std::memset(base + kDevice, 0, 14916);
  StoreGuestU32(base, kDevice + 48, kCursor);
  StoreGuestU32(base, kDevice + 52, 0x20000);
  StoreGuestU32(base, kDevice + 56, kCursor + 128);
  StoreGuestU32(base, kDevice + 10896, kSync);
  *simpsons::GuestToHost(base, kDevice + 11824) = uint8_t(stride / 4);
  *simpsons::GuestToHost(base, kDevice + 12520) = 0x53;
  *simpsons::GuestToHost(base, kDevice + 10940) = pattern & 1 ? 0x80 : 0;
  *simpsons::GuestToHost(base, kDevice + 12179) = pattern & 1 ? 3 : 0;
  for (uint32_t i = 0; i < 4; ++i)
    *simpsons::GuestToHost(base, kDevice + 12114 + i) = uint8_t(0xF0 + pattern + i);
  if (pattern == 3)
    *simpsons::GuestToHost(base, kDevice + 10941) = 0x20;
  StoreGuestU32(base, kDevice + 14884, 0x180000);
  StoreGuestU32(base, kDevice + 14892, pattern == 2 ? 0 : 0x40000 + pattern * 4);
  StoreGuestU32(base, kDevice + 14896, pattern * 0x55555555u);
  StoreGuestU32(base, kSync + 4, pattern * 0x55555555u + 4);
  StoreGuestU32(base, kDevice + 14908, pattern == 2 ? 0xFFFFFFF0u : 0);
  StoreGuestU32(base, kDevice + 14912, pattern == 1 ? 1 : 0x7FFFFFFF);
  StoreGuestU32(base, kDevice + 14900, pattern & 1 ? 0x30004 : 0x30000);
  StoreGuestU32(base, kDevice + 14880, 0x30000);
  port::Store64(base, kDevice + 24, pattern & 1);
  native_memory = reference_memory;
}
}  // namespace

int main(int argc, char** argv) {
  if (argc > 2 || (argc == 2 && !std::freopen(argv[1], "w", stdout)))
    return 2;
  try {
    uint64_t cases = 0, failures = 0, patches = 0, rejections = 0, high_addresses = 0;
    for (uint32_t primitive = 0; primitive < 64; ++primitive)
      for (uint32_t count : {1u, 3u, 9u, 31u, 257u, 65535u})
        for (uint32_t stride : {4u, 12u, 16u, 32u, 64u, 1020u})
          for (uint32_t pattern = 0; pattern < 4; ++pattern) {
            if (count == 65535 && stride > 4)
              continue;
            Prepare(stride, pattern);
            PPCContext original{};
            original.r1.u64 = kStack;
            original.r3.u64 = 0xA5A5000000000000ull | kDevice;
            original.r4.u64 = primitive | 0xFFFF0000000000C0ull;
            original.r5.u64 = count | 0xABCD000000000000ull;
            original.r6.u64 = stride;
            original.lr = 0xA5A5000082234560ull;
            for (uint32_t i = 0; i < kNonvolatile.size(); ++i)
              (original.*kNonvolatile[i]).u64 = 0xDEADBEEF00000000ull + i;
            auto native = original;
            reference_sub_8244C450(original, reference_memory.data());
            const uint64_t begin_count = reference_begin_calls, alloc_count = allocator_body_calls;
            Require(port::BeginVertices(native, native_memory.data(), reference_sub_82456D60),
                    "Prepared BeginVertices unexpectedly fell back");
            Require(reference_begin_calls == begin_count && allocator_body_calls == alloc_count + 1,
                    "Native BeginVertices bypassed the real allocator or executed the original Begin");
            Abi(original, native);
            Require(!std::memcmp(reference_memory.data() + kDevice, native_memory.data() + kDevice,
                                 14916), "Native BeginVertices changed device/allocator state");
            Require(LoadGuestU32(native_memory.data(), kDevice + 48) == kCursor,
                    "BeginVertices published before the caller filled its data");
            if (!original.r3.u32) {
              Require(Words(reference_memory.data()) == Words(native_memory.data()),
                      "Failed allocation emitted a native draw");
              ++failures;
            } else {
              auto expected = Words(reference_memory.data()), encoded = Words(native_memory.data());
              codec::VerticesDrawCall actual;
              Require(((encoded[0] >> 8) & 0x7F) == codec::kVerticesDrawOpcode &&
                          !(encoded[0] & 1) &&
                          codec::DecodeVertices({encoded.data() + 1, 15}, actual) &&
                          Values(actual) == Values(Original(expected)),
                      "Native vertices changed state, predication or draw operands");
              if ((cases & 31) == 0)
                for (uint32_t word : {1u, 3u, 4u, 5u, 6u, 7u, 8u, 10u, 11u, 12u, 14u, 15u}) {
                  auto a = expected, b = encoded;
                  a[word] ^= word == 15 ? 0x1234003Fu : 0x91E10DA5u;
                  b[word] = a[word];
                  Require(codec::DecodeVertices({b.data() + 1, 15}, actual) &&
                              Values(actual) == Values(Original(a)),
                          "Copied/patched vertices operands changed meaning");
                  ++patches;
                }
              const uint32_t vertex = native.r3.u32;
              for (uint32_t i = 0; i < count * stride; ++i)
                reference_memory[vertex + i] = native_memory[vertex + i] = uint8_t(i * 31 + pattern);
              original.r3.u64 = native.r3.u64 = kDevice;
              reference_sub_8244C8F0(original, reference_memory.data());
              Require(port::EndVertices(native, native_memory.data()), "EndVertices fell back");
              Abi(original, native);
              Require(LoadGuestU32(native_memory.data(), kDevice + 48) == kCursor + 64 &&
                          !std::memcmp(reference_memory.data() + kDevice,
                                       native_memory.data() + kDevice, 14916) &&
                          !std::memcmp(reference_memory.data() + vertex,
                                       native_memory.data() + vertex, count * stride),
                      "EndVertices lost caller data or published a different cursor");
            }
            ++cases;
          }
    // Begin writes only the descriptor, not the returned data. High allocator
    // addresses can therefore be compared without mapping or faking their data.
    // The preceding cases separately qualify the actual caller-fill lifecycle.
    for (uint32_t address : {0x7F010001u, 0x80010001u, 0x90010001u, 0xA0010001u,
                             0xC0010001u, 0xE0010001u, 0xFFF00001u}) {
      Prepare(16, 0);
      for (uint8_t* base : {reference_memory.data(), native_memory.data()}) {
        StoreGuestU32(base, kDevice + 14892, address);
        StoreGuestU32(base, kDevice + 14884, address + 4096);
      }
      PPCContext original{}; original.r1.u64 = kStack; original.r3.u64 = kDevice;
      original.r4.u64 = 6; original.r5.u64 = 9; original.r6.u64 = 16; original.lr = 0x82234560;
      auto native = original;
      reference_sub_8244C450(original, reference_memory.data());
      Require(port::BeginVertices(native, native_memory.data(), reference_sub_82456D60),
              "A supported high-address descriptor was rejected");
      Abi(original, native);
      const auto words = Words(native_memory.data());
      codec::VerticesDrawCall call;
      Require(codec::DecodeVertices({words.data() + 1, 15}, call) &&
                  Values(call) == Values(Original(Words(reference_memory.data()))),
              "High-address vertex descriptor changed physical address conversion");
      ++high_addresses;
    }
    for (uint32_t problem = 0; problem < 14; ++problem) {
      Prepare(16, 0);
      PPCContext ctx{}; ctx.r1.u64 = kStack; ctx.r3.u64 = kDevice;
      ctx.r4.u64 = 6; ctx.r5.u64 = 9; ctx.r6.u64 = 16;
      auto* base = native_memory.data();
      if (problem < 5) port::Store64(base, kDevice + problem * 8, 2);
      if (problem == 5) *simpsons::GuestToHost(base, kDevice + 10940) = 1;
      if (problem == 6) StoreGuestU32(base, kDevice + 13492, 0x8000);
      if (problem == 7) StoreGuestU32(base, kDevice + 13216, 1);
      if (problem == 8) StoreGuestU32(base, kDevice + 56, kCursor - 4);
      if (problem == 9) StoreGuestU32(base, kDevice + 14884, 0x40008);
      if (problem == 10) StoreGuestU32(base, kSync + 4, 1);
      if (problem == 11) StoreGuestU32(base, kDevice + 48, kDevice);
      if (problem == 12) StoreGuestU32(base, kDevice + 14892, kDevice);
      if (problem == 13) { ctx.r6.u64 = 12; StoreGuestU32(base, kDevice + 12688, 1); }
      const std::vector<uint8_t> before(native_memory.begin(), native_memory.end());
      const auto saved = ctx; const uint64_t allocs = allocator_body_calls;
      Require(!port::BeginVertices(ctx, base, reference_sub_82456D60) &&
                  allocator_body_calls == allocs && !std::memcmp(&ctx, &saved, sizeof(ctx)) &&
                  std::equal(before.begin(), before.end(), native_memory.begin()),
              "Unsupported BeginVertices changed context, memory or allocation state");
      ++rejections;
    }
    std::printf("{\"passed\":true,\"cases\":%llu,\"real_allocation_failures\":%llu,"
                "\"patched_replays\":%llu,\"unchanged_fallbacks\":%llu,\"high_address_descriptors\":%llu,"
                "\"native_original_begin_calls\":0,\"scope\":\"prepared Begin/fill/End CPU lifecycle\"}\n",
                (unsigned long long)cases, (unsigned long long)failures,
                (unsigned long long)patches, (unsigned long long)rejections,
                (unsigned long long)high_addresses);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
  }
}
