// Execute the current generated builders and allocator chain independently.
// Accepted native paths may call the real allocator, never the old builders.
#include "simpsons_init.h"
#include "native_gpu_direct_draws.h"
#include "native_gpu_indexed.h"
#include "native_gpu_aux_draws.h"

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
void Require(bool ok, const char* message) {
  if (!ok)
    throw std::runtime_error(message);
}
constexpr uint32_t kDevice = 0x1000, kSync = 0x8000, kCursor = 0x10000;
constexpr uint32_t kStack = 0x1F0000, kOutV = kStack + 96, kOutI = kStack + 100;
constexpr size_t kSize = 2 * 1024 * 1024;
alignas(4096) std::array<uint8_t, kSize> reference_memory, native_memory;
constexpr std::array<PPCRegister PPCContext::*, 18> kNonvolatile = {
    &PPCContext::r14, &PPCContext::r15, &PPCContext::r16, &PPCContext::r17, &PPCContext::r18,
    &PPCContext::r19, &PPCContext::r20, &PPCContext::r21, &PPCContext::r22, &PPCContext::r23,
    &PPCContext::r24, &PPCContext::r25, &PPCContext::r26, &PPCContext::r27, &PPCContext::r28,
    &PPCContext::r29, &PPCContext::r30, &PPCContext::r31};

void Abi(const PPCContext& a, const PPCContext& b) {
  Require(a.r1.u64 == b.r1.u64 && a.lr == b.lr && a.r3.u64 == b.r3.u64,
          "Stack, return value or link register changed");
  for (auto field : kNonvolatile)
    Require((a.*field).u64 == (b.*field).u64, "Nonvolatile GPR changed");
}
PPCContext Context() {
  PPCContext ctx{};
  ctx.r1.u64 = kStack;
  ctx.r3.u64 = 0xA5A5000000000000ull | kDevice;
  ctx.lr = 0xA5A5000082234560ull;
  for (uint32_t i = 0; i < kNonvolatile.size(); ++i)
    (ctx.*kNonvolatile[i]).u64 = 0xDEADBEEF00000000ull + i;
  return ctx;
}
void Prepare(uint32_t stride, uint32_t pattern) {
  reference_memory.fill(0xCD);
  auto* base = reference_memory.data();
  std::memset(base + kDevice, 0, 14916);
  StoreGuestU32(base, kStack + 84, kOutV);
  StoreGuestU32(base, kDevice + 48, kCursor);
  StoreGuestU32(base, kDevice + 52, 0x20000);
  StoreGuestU32(base, kDevice + 56, kCursor + 128);
  StoreGuestU32(base, kDevice + 10896, kSync);
  *simpsons::GuestToHost(base, kDevice + 11824) = uint8_t(stride / 4);
  *simpsons::GuestToHost(base, kDevice + 12520) = 0x53;
  *simpsons::GuestToHost(base, kDevice + 10940) = pattern & 1 ? 0x80 : 0;
  *simpsons::GuestToHost(base, kDevice + 12179) = pattern & 1 ? 255 : 0;
  for (uint32_t i = 0; i < 256; ++i)
    *simpsons::GuestToHost(base, kDevice + 12114 + i) = uint8_t(0xF0 + pattern + i);
  // The lookup table spans the mode byte; set the selector afterwards.
  *simpsons::GuestToHost(base, kDevice + 12179) = pattern & 1 ? 255 : 0;
  if (pattern == 3)
    *simpsons::GuestToHost(base, kDevice + 10941) = 0x20;
  StoreGuestU32(base, kDevice + 14884, 0x180000);
  StoreGuestU32(base, kDevice + 14892, pattern == 2 ? 0 : 0x40001);
  StoreGuestU32(base, kDevice + 14896, pattern * 0x55555555u);
  StoreGuestU32(base, kSync + 4, pattern * 0x55555555u + 4);
  StoreGuestU32(base, kDevice + 14908, pattern == 2 ? 0xFFFFFFF0u : 0);
  StoreGuestU32(base, kDevice + 14912, pattern == 1 ? 1 : 0x7FFFFFFF);
  StoreGuestU32(base, kDevice + 14900, 0x30000);
  StoreGuestU32(base, kDevice + 14880, 0x30000);
  native_memory = reference_memory;
}
template <size_t N>
std::array<uint32_t, N> Words(const uint8_t* base) {
  std::array<uint32_t, N> words;
  for (uint32_t i = 0; i < N; ++i)
    words[i] = LoadGuestU32(base, kCursor + 4 + i * 4);
  return words;
}
auto Values(const codec::VerticesDrawCall& c) {
  return std::tie(c.first_vertex, c.fetch, c.constant_flush, c.viz_query_condition, c.initiator,
                  c.index_base, c.index_size);
}
codec::VerticesDrawCall Original(const std::array<uint32_t, 18>& w) {
  Require(w[0] == 0x2102 && w[2] == 0x548BA && w[9] == 0x25000 && w[13] == 0xC0032201,
          "Original indexed record changed its contract");
  return {w[1], {w[3], w[4], w[5], w[6], w[7], w[8]}, {w[10], w[11], w[12]}, w[14], w[15], w[16],
          w[17]};
}
// Only the private stack frame and transport headers may differ. Compare all
// other guest bytes, including both outputs, allocator state and data guards.
void MemoryEqual(std::initializer_list<uint32_t> headers) {
  auto normalized = std::vector<uint8_t>(native_memory.begin(), native_memory.end());
  for (uint32_t word : headers)
    std::memcpy(normalized.data() + kCursor + 4 + word * 4,
                reference_memory.data() + kCursor + 4 + word * 4, 4);
  Require(!std::memcmp(reference_memory.data(), normalized.data(), kStack - 608) &&
              !std::memcmp(reference_memory.data() + kStack + 88, normalized.data() + kStack + 88,
                           kSize - kStack - 88),
          "Native builder changed guest memory outside its transport headers/private frame");
}
}  // namespace

int main(int argc, char** argv) {
  if (argc > 2 || (argc == 2 && !std::freopen(argv[1], "w", stdout)))
    return 2;
  try {
    uint64_t indexed_cases = 0, range_cases = 0, indexed_ranges = 0;
    uint64_t failures = 0, patches = 0, rejections = 0;
    uint64_t warmup_cases = 0, bare_cases = 0;
    for (uint32_t primitive = 0; primitive < 64; ++primitive)
      for (uint32_t count : {1u, 9u, 257u, 65535u})
        for (uint32_t stride : {4u, 16u, 1020u})
          for (uint32_t format = 0; format < 8; ++format)
            for (uint32_t pattern = 0; pattern < 4; ++pattern) {
              if (count == 65535 && stride > 4)
                continue;
              Prepare(stride, pattern);
              auto original = Context();
              original.r4.u64 = primitive | 0xFFFF0000000000C0ull;
              original.r5.u64 = pattern & 1 ? 0xFFFFFFFFFFFFFFFDull : 7;
              original.r6.u64 = count | 0xABCD000000000000ull;
              original.r7.u64 = ((count + 5) / 2) | 0xA5A5000000000000ull;
              original.r8.u64 = format | 0xBDBD000000000000ull;
              original.r9.u64 = stride;
              original.r10.u64 = kOutI;
              auto native = original;
              reference_sub_8244C970(original, reference_memory.data());
              const auto calls = reference_begin_calls, allocs = allocator_body_calls;
              Require(
                  port::BeginIndexedVertices(native, native_memory.data(), reference_sub_82456D60),
                  "Prepared indexed Begin fell back");
              Require(reference_begin_calls == calls && allocator_body_calls == allocs + 2,
                      "Native indexed Begin called the old builder or bypassed its allocator");
              Abi(original, native);
              Require(LoadGuestU32(native_memory.data(), kDevice + 48) == kCursor,
                      "Indexed Begin published before its caller filled both buffers");
              if (original.r3.s64 < 0) {
                MemoryEqual({});
                ++failures;
              } else {
                auto words = Words<18>(native_memory.data());
                codec::VerticesDrawCall decoded;
                Require(codec::DecodeVertices({words.data() + 1, 17}, decoded) &&
                            Values(decoded) == Values(Original(Words<18>(reference_memory.data()))),
                        "Native indexed arguments differ from the original");
                const uint32_t vertex = LoadGuestU32(native_memory.data(), kOutV);
                const uint32_t index = LoadGuestU32(native_memory.data(), kOutI);
                const uint32_t index_bytes =
                    LoadGuestU32(native_memory.data(), kDevice + 13444) * 4;
                for (uint32_t i = 0; i < count * stride; ++i)
                  reference_memory[vertex + i] = native_memory[vertex + i] =
                      uint8_t(i * 31 + pattern);
                for (uint32_t i = 0; i < index_bytes; ++i)
                  reference_memory[index + i] = native_memory[index + i] = uint8_t(i * 19 + format);
                MemoryEqual({0, 2, 9, 13});
                original.r3.u64 = native.r3.u64 = kDevice;
                reference_sub_8244CEA0(original, reference_memory.data());
                Require(port::EndVertices(native, native_memory.data()), "Indexed End fell back");
                Abi(original, native);
                Require(original.r10.u64 == native.r10.u64 && original.r11.u64 == native.r11.u64 &&
                            LoadGuestU32(native_memory.data(), kDevice + 48) == kCursor + 72,
                        "Indexed End changed its results or publication cursor");
                MemoryEqual({0, 2, 9, 13});
                if (primitive % 8 == 0) {
                  // A copied recording patched at original operand offsets
                  // must stay independent of the builder and allocator.
                  for (uint32_t position : {1u, 3u, 8u, 10u, 14u, 15u, 16u, 17u}) {
                    auto copy = words;
                    copy[position] ^= position == 15 ? 0x80u : 0xA5A50102u;
                    codec::VerticesDrawCall patched;
                    Require(codec::DecodeVertices({copy.data() + 1, 17}, patched),
                            "A legal patched indexed record was rejected");
                    std::array<uint32_t, 18> encoded;
                    Require(codec::EncodeVertices(patched, encoded) && encoded == copy,
                            "Copied indexed record lost its patched arguments");
                    ++patches;
                  }
                }
              }
              ++indexed_cases;
            }
    for (uint32_t primitive = 0; primitive < 64; ++primitive)
      for (uint32_t count : {0u, 1u, 3u, 257u, 65535u})
        for (uint32_t first : {0u, 7u, 0xFFFFFFFDu})
          for (uint32_t pattern = 0; pattern < 4; ++pattern) {
            Prepare(16, pattern);
            auto original = Context();
            original.r4.u64 = primitive | 0xABCD0000000000C0ull;
            original.r5.u64 = first | 0xABCD000000000000ull;
            original.r6.u64 = count | 0xFFFF000000000000ull;
            auto native = original;
            reference_sub_8244CF78(original, reference_memory.data());
            const auto calls = reference_begin_calls, allocs = allocator_body_calls;
            Require(port::DrawRange(native, native_memory.data()) &&
                        reference_begin_calls == calls && allocator_body_calls == allocs,
                    "Prepared range used an old builder/allocator");
            Abi(original, native);
            const auto expected = Words<5>(reference_memory.data());
            const auto words = Words<5>(native_memory.data());
            codec::RangeDrawCall decoded;
            Require(expected[0] == 0x2102 && expected[2] == 0xC0012201 &&
                        codec::DecodeRange({words.data() + 1, 4}, decoded) &&
                        decoded.first_vertex == expected[1] &&
                        decoded.viz_query_condition == expected[3] &&
                        decoded.initiator == expected[4],
                    "Prepared range changed its original draw arguments");
            MemoryEqual({0, 2});
            ++range_cases;
          }
    for (uint32_t primitive = 0; primitive < 64; ++primitive)
      for (uint32_t count : {0u, 1u, 3u, 257u, 65535u})
        for (uint32_t start : {1u, 0xFFFFFFFDu})
          for (uint32_t format = 0; format < 8; ++format)
            for (uint32_t pattern = 0; pattern < 2; ++pattern) {
              Prepare(16, pattern);
              constexpr uint32_t metadata = 0x9000;
              for (auto* base : {reference_memory.data(), native_memory.data()}) {
                StoreGuestU32(base, kDevice + 12428, metadata);
                StoreGuestU32(base, metadata, (format << 29) | 0x00123456u);
                StoreGuestU32(base, metadata + 24, pattern ? 0xE00FF002u : 0x80010002u);
              }
              auto original = Context();
              original.r4.u64 = primitive | 0xABCD0000000000C0ull;
              original.r5.u64 = 0xFFFFFFFFFFFFFFFDull;
              original.r6.u64 = start | 0xABCD000000000000ull;
              original.r7.u64 = count | 0xFFFF000000000000ull;
              auto native = original;
              reference_sub_8244D360(original, reference_memory.data());
              const auto calls = reference_begin_calls, allocs = allocator_body_calls;
              Require(port::DrawRange(native, native_memory.data(), true) &&
                          reference_begin_calls == calls && allocator_body_calls == allocs,
                      "Prepared indexed range used an old builder/allocator");
              Abi(original, native);
              const auto expected = Words<7>(reference_memory.data());
              const auto words = Words<7>(native_memory.data());
              codec::RangeDrawCall decoded;
              Require(expected[0] == 0x2102 && expected[2] == 0xC0032201 &&
                          codec::DecodeRange({words.data() + 1, 6}, decoded) &&
                          decoded.first_vertex == expected[1] &&
                          decoded.viz_query_condition == expected[3] &&
                          decoded.initiator == expected[4] && decoded.index_base == expected[5] &&
                          decoded.index_size == expected[6],
                      "Prepared indexed range changed its original draw arguments");
              MemoryEqual({0, 2});
              ++indexed_ranges;
            }
    for (uint32_t pattern = 0; pattern < 4; ++pattern) {
      Prepare(16, pattern);
      auto original = Context();
      original.r4.u64 = (pattern & 1 ? 0xABCD000000000000ull : 0) | (kCursor + 4);
      auto native = original;
      reference_sub_82444A48(original, reference_memory.data());
      const auto calls = reference_begin_calls;
      Require(simpsons::native_gpu_calls::WriteWarmupDraws(native, native_memory.data()) &&
                  calls == reference_begin_calls,
              "Warmup called its original builder");
      Abi(original, native);
      for (uint32_t i = 0; i < 24; ++i) {
        const uint32_t where = kCursor + 4 + i * 8;
        Require(LoadGuestU32(reference_memory.data(), where) == 0xC0003600u &&
                    LoadGuestU32(native_memory.data(), where) ==
                        (0xC0000000u | (codec::kBareDrawOpcode << 8)) &&
                    LoadGuestU32(native_memory.data(), where + 4) == 0x10081,
                "Warmup changed its 24-draw contract");
        StoreGuestU32(native_memory.data(), where, 0xC0003600u);
      }
      MemoryEqual({});
      ++warmup_cases;
      for (uint32_t count : {0u, 1u, 3u, 65535u, 65536u, UINT32_MAX}) {
        Prepare(16, pattern);
        original = Context();
        original.r4.u64 = 0xABCD000000000000ull | count;
        native = original;
        reference_sub_8246AEF0(original, reference_memory.data());
        const auto calls_before = reference_begin_calls;
        Require(simpsons::native_gpu_calls::WriteBareTriangleDraw(native, native_memory.data()) &&
                    calls_before == reference_begin_calls,
                "Bare triangle called its old builder");
        Abi(original, native);
        Require(LoadGuestU32(reference_memory.data(), kCursor + 4) == 0xC0003601u &&
                    LoadGuestU32(native_memory.data(), kCursor + 4) ==
                        (0xC0000001u | (codec::kBareDrawOpcode << 8)),
                "Bare triangle lost whole-draw predication");
        MemoryEqual({0});
        ++bare_cases;
      }
    }
    for (uint32_t problem = 0; problem < 24; ++problem) {
      Prepare(16, 0);
      auto ctx = Context();
      ctx.r4.u64 = 6;
      ctx.r5.u64 = 3;
      ctx.r6.u64 = 9;
      ctx.r7.u64 = 12;
      ctx.r8.u64 = 4;
      ctx.r9.u64 = 16;
      ctx.r10.u64 = kOutI;
      auto* base = native_memory.data();
      if (problem < 5)
        port::Store64(base, kDevice + problem * 8, 1);
      if (problem == 5)
        *simpsons::GuestToHost(base, kDevice + 10940) = 1;
      if (problem == 6)
        StoreGuestU32(base, kDevice + 13492, 0x8000);
      if (problem == 7)
        StoreGuestU32(base, kDevice + 13216, 1);
      if (problem == 8)
        StoreGuestU32(base, kDevice + 56, kCursor - 4);
      if (problem == 9)
        StoreGuestU32(base, kDevice + 14884, 0x400A4);
      if (problem == 10)
        StoreGuestU32(base, kSync + 4, 1);
      if (problem == 11)
        StoreGuestU32(base, kDevice + 48, kDevice);
      if (problem == 12)
        StoreGuestU32(base, kDevice + 14892, kOutI);
      if (problem == 13) {
        ctx.r9.u64 = 12;
        StoreGuestU32(base, kDevice + 12688, 1);
      }
      if (problem == 14)
        ctx.r10.u64 = kOutV;
      if (problem == 15)
        ctx.r10.u64 = kDevice;
      if (problem == 16)
        StoreGuestU32(base, kStack + 84, kCursor + 12);
      if (problem == 17)
        StoreGuestU32(base, kStack + 84, kStack - 500);
      if (problem == 18) {
        StoreGuestU32(base, kDevice + 14912, 1);
        StoreGuestU32(base, kDevice + 14900, 0x30004);
      }
      if (problem == 19)
        ctx.r7.u64 = 65536;
      if (problem == 20)
        ctx.r9.u64 = 1024;
      if (problem == 21)
        ctx.r6.u64 = 0;
      if (problem == 22)
        StoreGuestU32(base, kDevice + 10896, kOutI);
      if (problem == 23) {
        StoreGuestU32(base, kStack + 84, 0xA0000000u | kOutV);
        ctx.r10.u64 = 0xC0000000u | kOutV;
      }
      const auto before = std::vector<uint8_t>(native_memory.begin(), native_memory.end());
      const auto saved = ctx;
      const auto allocs = allocator_body_calls;
      Require(!port::BeginIndexedVertices(ctx, base, reference_sub_82456D60) &&
                  allocator_body_calls == allocs && !std::memcmp(&ctx, &saved, sizeof(ctx)) &&
                  std::equal(before.begin(), before.end(), native_memory.begin()),
              "Unsupported indexed path changed memory, context or allocation state");
      ++rejections;
    }
    for (uint32_t problem = 0; problem < 14; ++problem) {
      Prepare(16, 0);
      auto ctx = Context();
      ctx.r4.u64 = 6;
      ctx.r5.u64 = 0;
      ctx.r6.u64 = 3;
      auto* base = native_memory.data();
      if (problem < 5)
        port::Store64(base, kDevice + problem * 8, 1);
      if (problem == 5)
        *simpsons::GuestToHost(base, kDevice + 10940) = 1;
      if (problem == 6)
        ctx.r6.u64 = 65536;
      if (problem == 7)
        StoreGuestU32(base, kDevice + 48, kDevice);
      if (problem == 8)
        StoreGuestU32(base, kDevice + 48, kStack - 128);
      const bool indexed = problem >= 9;
      ctx.r7.u64 = problem == 9 ? 65536 : 3;
      if (problem == 10)
        StoreGuestU32(base, kDevice + 12428, kDevice);
      if (problem == 11)
        StoreGuestU32(base, kDevice + 12428, kStack - 128);
      if (problem == 12)
        StoreGuestU32(base, kDevice + 12428, kCursor + 4);
      if (problem == 13)
        StoreGuestU32(base, kDevice + 12428, 0xFFFFFFF0u);
      const auto before = std::vector<uint8_t>(native_memory.begin(), native_memory.end());
      const auto saved = ctx;
      Require(!port::DrawRange(ctx, base, indexed) && !std::memcmp(&ctx, &saved, sizeof(ctx)) &&
                  std::equal(before.begin(), before.end(), native_memory.begin()),
              "Unsupported range changed memory or context");
      ++rejections;
    }
    std::printf(
        "{\"passed\":true,\"indexed_cases\":%llu,\"range_cases\":%llu,"
        "\"warmup_cases\":%llu,\"bare_triangle_cases\":%llu,"
        "\"indexed_range_cases\":%llu,"
        "\"real_allocation_failures\":%llu,\"patched_replays\":%llu,"
        "\"unchanged_fallbacks\":%llu,\"native_original_builder_calls\":0}\n",
        (unsigned long long)indexed_cases, (unsigned long long)range_cases,
        (unsigned long long)warmup_cases, (unsigned long long)bare_cases,
        (unsigned long long)indexed_ranges, (unsigned long long)failures,
        (unsigned long long)patches, (unsigned long long)rejections);
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "FAIL: %s\n", e.what());
    return 1;
  }
}
