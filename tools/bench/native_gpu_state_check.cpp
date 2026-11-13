// Generated reference bodies and their real save/restore helpers are extracted
// by check_gpu_state.py. A slow reference branch fails instead of faking state.
#include "simpsons_init.h"
#include "native_gpu_state.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <set>
#include <stdexcept>
#include <vector>

static uint64_t reference_calls = 0;
#include "native_gpu_state_reference.inc"

namespace port = simpsons::native_gpu_state;
namespace codec = rex::graphics::native_gpu_state;
using simpsons::LoadGuestU32;
using simpsons::StoreGuestU32;

namespace {
constexpr uint32_t kDevice = 0x1000, kShadow = 0x10000, kCursor = 0x20000, kStack = 0x3F000;
constexpr size_t kSize = 256 * 1024;
alignas(4096) std::array<uint8_t, kSize> reference_memory, native_memory;
constexpr std::array<PPCRegister PPCContext::*, 18> kNonvolatile = {
    &PPCContext::r14, &PPCContext::r15, &PPCContext::r16, &PPCContext::r17, &PPCContext::r18,
    &PPCContext::r19, &PPCContext::r20, &PPCContext::r21, &PPCContext::r22, &PPCContext::r23,
    &PPCContext::r24, &PPCContext::r25, &PPCContext::r26, &PPCContext::r27, &PPCContext::r28,
    &PPCContext::r29, &PPCContext::r30, &PPCContext::r31};

void Require(bool ok, const char* why) {
  if (!ok)
    throw std::runtime_error(why);
}
void Prepare(uint32_t alignment) {
  for (uint32_t i = 0; i < kSize; ++i)
    reference_memory[i] = uint8_t(i * 29 + (i >> 5) + 0x73);
  StoreGuestU32(reference_memory.data(), kDevice + 48, kCursor + alignment);
  StoreGuestU32(reference_memory.data(), kDevice + 52, kCursor + 4096);
  native_memory = reference_memory;
}
PPCContext Context(uint64_t dirty, uint32_t first) {
  PPCContext ctx{};
  ctx.r1.u64 = kStack;
  ctx.r3.u64 = 0xA5A5000000000000ull | kDevice;
  ctx.r4.u64 = dirty;
  ctx.r5.u64 = 0xABCD000000000000ull | first;
  ctx.r6.u64 = 0xA5A5000000000000ull | kShadow;
  ctx.lr = 0xABCD000082123450ull;
  for (uint32_t i = 0; i < kNonvolatile.size(); ++i)
    (ctx.*kNonvolatile[i]).u64 = 0xDDDD000000000000ull + i;
  return ctx;
}
void Abi(const PPCContext& a, const PPCContext& b) {
  Require(a.r1.u64 == b.r1.u64 && a.r3.u64 == b.r3.u64 && a.r4.u64 == b.r4.u64 &&
              a.r5.u64 == b.r5.u64 && a.r6.u64 == b.r6.u64 && a.lr == b.lr,
          "Native state changed an output, pointer or stack/link ABI");
  for (auto field : kNonvolatile)
    Require((a.*field).u64 == (b.*field).u64, "Native state changed a nonvolatile register");
}
void EqualMemory(const std::vector<uint8_t>& normalized) {
  Require(!std::memcmp(reference_memory.data(), normalized.data(), kStack - 144) &&
              !std::memcmp(reference_memory.data() + kStack, normalized.data() + kStack,
                           kSize - kStack),
          "Native state changed bytes beyond its headers/private call frame");
}
uint32_t EndOfPage(uint32_t first) {
  if (first >= 0x5000)
    return 0x5003;
  if (first >= 0x4900)
    return 0x4928;
  if (first >= 0x4800)
    return 0x48C0;
  return (first & ~255u) + 256;
}
uint64_t TopBits(uint32_t count) {
  return count == 64 ? UINT64_MAX : UINT64_MAX << (64 - count);
}
}  // namespace

int main(int argc, char** argv) {
  if (argc > 2 || (argc == 2 && !std::freopen(argv[1], "w", stdout)))
    return 2;
  try {
    uint64_t cases = 0, fetch_cases = 0, ranges = 0, patches = 0, rejected = 0, codecs = 0;
    std::set<uint64_t> masks = {UINT64_MAX, 0xAAAAAAAAAAAAAAAAull, 0x5555555555555555ull};
    for (uint32_t i = 1; i <= 64; ++i) {
      masks.insert(uint64_t(1) << (64 - i));
      masks.insert(TopBits(i));
    }
    uint64_t random = 0x123456789ABCDEF0ull;
    for (uint32_t i = 0; i < 64; ++i) {
      random ^= random << 13;
      random ^= random >> 7;
      random ^= random << 17;
      masks.insert(random);
    }
    for (bool fetch : {false, true})
      for (uint32_t first : {0x2000u, 0x207Fu, 0x2080u, 0x20C0u, 0x2100u, 0x2180u, 0x21C0u, 0x2200u,
                             0x2280u, 0x2300u, 0x2380u, 0x23C0u, 0x4800u, 0x4880u, 0x4900u}) {
        if (fetch && first != 0x4800)
          continue;
        const uint32_t valid = fetch ? 32 : std::min(64u, EndOfPage(first) - first);
        for (uint64_t mask : masks) {
          mask &= TopBits(valid);
          if (!mask)
            continue;
          for (uint32_t alignment : {0u, 4u, 8u, 12u}) {
            Prepare(alignment);
            auto original = Context(mask, first), native = original;
            if (fetch)
              reference_sub_8245EB08(original, reference_memory.data());
            else
              reference_sub_8245E8C8(original, reference_memory.data());
            const auto calls = reference_calls;
            Require(port::WriteState(native, native_memory.data(), fetch),
                    "Prepared native state fell back");
            Require(reference_calls == calls, "Native state called the original writer");
            Abi(original, native);
            const uint32_t end = LoadGuestU32(reference_memory.data(), kDevice + 48);
            Require(end == LoadGuestU32(native_memory.data(), kDevice + 48),
                    "Published state cursor changed");
            auto normalized = std::vector<uint8_t>(native_memory.begin(), native_memory.end());
            uint32_t cursor = kCursor + alignment + 4;
            while (cursor <= end) {
              const uint32_t header = LoadGuestU32(reference_memory.data(), cursor);
              if ((header >> 30) == 2) {
                Require(header == LoadGuestU32(native_memory.data(), cursor),
                        "Alignment padding changed");
                cursor += 4;
                continue;
              }
              Require((header >> 30) == 0 && !(header & 0x8000),
                      "Reference emitted an unexpected packet");
              const uint32_t count = ((header >> 16) & 0x3FFF) + 1;
              const uint32_t native_header = LoadGuestU32(native_memory.data(), cursor);
              codec::StateRange decoded;
              Require(codec::DecodeHeader(native_header, count, decoded) &&
                          decoded.first_register == (header & 0x7FFF) &&
                          decoded.register_count == count,
                      "Native state arguments/length differ from the generated writer");
              Require(!std::memcmp(reference_memory.data() + cursor + 4,
                                   native_memory.data() + cursor + 4, count * 4),
                      "Native state changed raw payload bits");
              StoreGuestU32(normalized.data(), cursor, header);
              if ((cases + fetch_cases) % 17 == 0) {
                // Copy a recording and patch its original first/last payload
                // offsets. Header decoding must never require a side queue.
                std::vector<uint8_t> copy(native_memory.begin() + cursor,
                                          native_memory.begin() + cursor + 4 + count * 4);
                for (uint32_t i : {1u, count}) {
                  const uint32_t value = 0x7FC00001u ^ i;
                  StoreGuestU32(copy.data(), i * 4, value);
                  codec::StateRange replay;
                  Require(codec::DecodeHeader(LoadGuestU32(copy.data(), 0), count, replay) &&
                              replay.first_register == decoded.first_register &&
                              LoadGuestU32(copy.data(), i * 4) == value,
                          "Copied/patched native state lost its operands");
                  ++patches;
                }
              }
              cursor += (count + 1) * 4;
              ++ranges;
            }
            Require(cursor == end + 4, "Native state record lengths do not reach the exact end");
            EqualMemory(normalized);
            if (fetch)
              ++fetch_cases;
            else
              ++cases;
          }
        }
      }
    for (uint32_t page : codec::kPages)
      for (uint32_t offset = 0; offset < 256; ++offset)
        for (uint32_t count = 1; count <= 256 - offset; ++count) {
          uint32_t header = 0xDEADBEEF;
          const bool valid = page + offset + count <= EndOfPage(page);
          Require(codec::EncodeHeader(page + offset, count, header) == valid,
                  "Native state codec accepted/rejected the wrong register bounds");
          if (!valid)
            continue;
          codec::StateRange decoded{UINT32_MAX, UINT32_MAX};
          Require(codec::DecodeHeader(header, count, decoded) &&
                      decoded.first_register == page + offset && decoded.register_count == count &&
                      ((header >> 16) & 0x3FFF) + 1 == count,
                  "Native state codec changed its register or ordinary packet length");
          const auto saved = decoded;
          Require(!codec::DecodeHeader(header | 1, count, decoded) &&
                      !codec::DecodeHeader(header, count + 1, decoded) &&
                      !std::memcmp(&saved, &decoded, sizeof(saved)),
                  "Invalid native state metadata changed its decode output");
          ++codecs;
        }
    for (bool fetch : {false, true})
      for (uint32_t problem = 0; problem < 14; ++problem) {
        Prepare(0);
        auto ctx = Context(0x8000000000000000ull, 0x2200);
        auto* base = native_memory.data();
        if (problem == 0)
          ctx.r4.u64 = 0;
        if (problem == 1)
          StoreGuestU32(base, kDevice + 48, kCursor + 1);
        if (problem == 2)
          StoreGuestU32(base, kDevice + 52, kCursor + (fetch ? 44 : 4));
        if (problem == 3)
          ctx.r1.u64 = kDevice + 128;
        if (problem == 4)
          StoreGuestU32(base, kDevice + 48, kStack - 128);
        if (problem == 5)
          StoreGuestU32(base, kDevice + 48, kDevice);
        if (problem == 6)
          StoreGuestU32(base, kDevice + 48, 0x7EFFFFFCu);
        if (problem == 7)
          ctx.r1.u64 = 128;
        if (problem == 8)
          ctx.r3.u64 = kDevice + 1;
        if (!fetch) {
          if (problem == 9)
            ctx.r5.u64 = 0x2400;
          if (problem == 10)
            ctx.r6.u64 = kCursor + 8;
          if (problem == 11)
            ctx.r6.u64 = kStack - 128;
          if (problem == 12)
            ctx.r6.u64 = kShadow + 1;
          if (problem == 13)
            ctx.r6.u64 = kDevice;
        } else {
          if (problem == 9)
            ctx.r4.u64 = 1;
          if (problem == 10)
            StoreGuestU32(base, kDevice + 48, kDevice + 1152);
          if (problem == 11) {
            ctx.r1.u64 = kDevice + 1280;
          }
          if (problem == 12)
            ctx.r4.u64 = UINT64_MAX;
          if (problem == 13)
            StoreGuestU32(base, kDevice + 52, 0);
        }
        const auto before = std::vector<uint8_t>(native_memory.begin(), native_memory.end());
        const auto saved = ctx;
        Require(!port::WriteState(ctx, base, fetch) && !std::memcmp(&ctx, &saved, sizeof(ctx)) &&
                    std::equal(before.begin(), before.end(), native_memory.begin()),
                "Unsupported native state modified context or guest memory");
        ++rejected;
      }
    std::printf(
        "{\"passed\":true,\"state_cases\":%llu,\"fetch_cases\":%llu,"
        "\"native_ranges\":%llu,\"patched_replays\":%llu,\"codec_ranges\":%llu,"
        "\"unchanged_fallbacks\":%llu,\"native_original_writer_calls\":0}\n",
        (unsigned long long)cases, (unsigned long long)fetch_cases, (unsigned long long)ranges,
        (unsigned long long)patches, (unsigned long long)codecs, (unsigned long long)rejected);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
  }
}
