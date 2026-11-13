// The reference executes the actual generated draw, flush and save/restore
// bodies. Inputs, lookup tables and guest memory are synthetic; no game boots.
#include "simpsons_init.h"
#include "native_gpu_direct_draws.h"
#include "native_gpu_indexed.h"
#include "native_gpu_special.h"
#include "guest_memory_fixture.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <vector>

static uint64_t reference_draw_calls = 0, reference_calls = 0;
#include "native_gpu_special_reference.inc"

namespace port = simpsons::native_gpu_vertices;
namespace calls = rex::graphics::native_gpu_calls;
namespace state = rex::graphics::native_gpu_state;
namespace constants = rex::graphics::native_gpu_constants;
namespace coherency = rex::graphics::native_gpu_coherency;
using simpsons::LoadGuestU32;
using simpsons::StoreGuestU32;
using Dirty = std::array<uint64_t, 5>;

namespace {
#ifdef NATIVE_GPU_SAMPLERS
constexpr bool kSamplers = true;
#else
constexpr bool kSamplers = false;
#endif
constexpr uint64_t kMiscMask = 0x01003FFFFFFFFFFFull | (kSamplers ? uint64_t(63) << 49 : 0);
constexpr uint32_t kDevice = 0x1000, kMetadata = 0x8000, kCursor = 0x10000;
constexpr uint32_t kTable = 0x8206A000, kStack = 0x3F000, kSize = 256 * 1024;
std::unique_ptr<GuestMemoryFixture> reference_memory, native_memory;
std::array<uint8_t, kSize> pristine;
constexpr std::array<PPCRegister PPCContext::*, 18> kNonvolatile = {
    &PPCContext::r14, &PPCContext::r15, &PPCContext::r16, &PPCContext::r17, &PPCContext::r18,
    &PPCContext::r19, &PPCContext::r20, &PPCContext::r21, &PPCContext::r22, &PPCContext::r23,
    &PPCContext::r24, &PPCContext::r25, &PPCContext::r26, &PPCContext::r27, &PPCContext::r28,
    &PPCContext::r29, &PPCContext::r30, &PPCContext::r31};
struct Counts {
  uint64_t cases = 0, state = 0, constants = 0, padding = 0, draws = 0, patched = 0, rejected = 0;
  uint64_t late_draw_fallbacks = 0;
  uint64_t coherency = 0, standalone_samplers = 0;
  uint64_t sampler_fallbacks = 0, coherency_rejections = 0;
  std::array<uint64_t, 4> families{};
} counts;
uint32_t current_family = 0, current_variant = 0, current_alignment = 0;
Dirty current_dirty{};

void Require(bool ok, const char* why) {
  if (!ok)
    throw std::runtime_error(why);
}

PPCContext Prepare(uint32_t family, uint32_t variant, uint32_t alignment, const Dirty& dirty) {
  auto* base = reference_memory->data();
  std::memcpy(base, pristine.data(), kSize);
  for (uint32_t i = 0; i < dirty.size(); ++i)
    port::Store64(base, kDevice + i * 8, dirty[i]);
  port::Store64(base, kDevice + 40, 0);
  StoreGuestU32(base, kDevice + 48, kCursor + alignment);
  StoreGuestU32(base, kDevice + 52, 0x30000);
  StoreGuestU32(base, kDevice + 56, 0x2F000);
  *simpsons::GuestToHost(base, kDevice + 10940) = 0;
  StoreGuestU32(base, kDevice + 12428, kMetadata);
  StoreGuestU32(base, kDevice + 10616, variant == 8 ? 2 : variant % 4 == 2 ? 3 : variant % 4);
  constexpr std::array<uint32_t, 4> addresses = {0xA0100002, 0xE0100000, 0x7F100000, 0x80010000};
  StoreGuestU32(base, kMetadata, ((variant & 1) << 31) | ((variant & 6) << 28));
  StoreGuestU32(base, kMetadata + 24, addresses[variant % addresses.size()]);
  *simpsons::GuestToHost(base, kDevice + 12179) = uint8_t(variant ? variant * 31 : 0);
  std::memcpy(native_memory->data(), base, kSize);
  for (uint32_t offset = 0; offset < 4096; offset += 4)
    for (auto* memory : {reference_memory.get(), native_memory.get()})
      StoreGuestU32(memory->data(), kTable + offset, 0x91E10DA5u ^ (offset * 0x10203u));
  PPCContext ctx{};
  ctx.r1.u64 = kStack;
  ctx.r3.u64 = 0xA5A5000000000000ull | kDevice;
  constexpr std::array<uint32_t, 9> primitives = {0, 1, 2, 4, 6, 8, 16, 21, 17};
  ctx.r4.u64 = 0xABCD000000000000ull | primitives[variant];
  ctx.r5.u64 = 0xFFFF0000FFFFFFF0ull + variant;
  constexpr std::array<uint32_t, 9> count = {0, 1, 7, 65535, 9, 3, 65535, 128, 19};
  ctx.r6.u64 = family & 1 ? 0xFFFF0000FFFFFFF0ull + variant : count[variant];
  ctx.r7.u64 = 0xABCD000000000000ull | count[variant];
  ctx.lr = 0xABCD000082123450ull;
  ctx.fpscr.InitHost();
  for (uint32_t i = 0; i < kNonvolatile.size(); ++i)
    (ctx.*kNonvolatile[i]).u64 = 0xDEADBEEF00000000ull + i;
  if (variant == 8 && family == 3) {
    const uint32_t factor = 1 + alignment / 4;
    for (auto* memory : {reference_memory.get(), native_memory.get()})
      StoreGuestU32(memory->data(), 0x8206A44C + 12 + 8, factor);
  }
  return ctx;
}

bool Native(uint32_t family, PPCContext& ctx, uint8_t* base) {
  if (family < 2)
    return port::DrawRange(ctx, base, family & 1, true, kSamplers);
  return port::DrawVertexIdRange(ctx, base, family & 1, kSamplers);
}

void Reference(uint32_t family, PPCContext& ctx, uint8_t* base) {
  switch (family) {
    case 0:
      reference_sub_8244CF78(ctx, base);
      break;
    case 1:
      reference_sub_8244D360(ctx, base);
      break;
    case 2:
      reference_sub_8244D7B8(ctx, base);
      break;
    case 3:
      reference_sub_8244DC30(ctx, base);
      break;
  }
}

void Abi(const PPCContext& a, const PPCContext& b) {
  Require(
      a.r1.u64 == b.r1.u64 && a.r3.u64 == b.r3.u64 && a.lr == b.lr && a.fpscr.csr == b.fpscr.csr,
      "Draw changed return/stack/link/FPSCR ABI");
  for (auto field : kNonvolatile)
    Require((a.*field).u64 == (b.*field).u64, "Draw changed a nonvolatile GPR");
}

void EqualMemory(const std::vector<uint8_t>& normalized, uint32_t family,
                 uint32_t private_frame = 0) {
  const auto* base = reference_memory->data();
  const uint32_t frame = private_frame ? private_frame : (family & 1 ? 240 : 208) + 176;
  for (const auto& [start, end] : {std::pair{0u, kStack - frame}, std::pair{kStack, kSize}}) {
    if (std::memcmp(base + start, normalized.data() + start, end - start)) {
      for (uint32_t i = start; i < end; ++i)
        if (base[i] != normalized[i]) {
          std::fprintf(stderr, "Memory mismatch at %08X: original=%02X native=%02X\n", i, base[i],
                       normalized[i]);
          break;
        }
      throw std::runtime_error("Draw changed guest memory outside transport/private frames");
    }
  }
  Require(!std::memcmp(simpsons::GuestToHost(base, kTable),
                       simpsons::GuestToHost(native_memory->data(), kTable), 4096),
          "Draw changed its read-only lookup tables");
}

void Replay(const uint8_t* original, const uint8_t* native, uint32_t start, uint32_t end,
            std::vector<uint8_t>& normalized, bool indexed) {
  uint32_t cursor = start + 4;
  while (cursor <= end) {
    const uint32_t header = LoadGuestU32(original, cursor);
    const uint32_t replacement = LoadGuestU32(native, cursor);
    if (header == 0x80000000u) {
      Require(replacement == header, "Draw changed alignment padding");
      ++counts.padding;
      cursor += 4;
      continue;
    }
    Require((header >> 30) == 0, "Unexpected original command before the range draw");
    if (header == 0x2007 && cursor + 48 <= end && LoadGuestU32(original, cursor + 8) == 0xA31) {
      std::array<uint32_t, 13> words;
      for (uint32_t i = 0; i < words.size(); ++i)
        words[i] = LoadGuestU32(native, cursor + i * 4);
      coherency::Call call;
      Require(words[0] == coherency::kHeader &&
                  coherency::Decode({words.data() + 1, words.size() - 1}, call) &&
                  call.destination_base == LoadGuestU32(original, cursor + 4) &&
                  call.status == LoadGuestU32(original, cursor + 12) &&
                  call.size == LoadGuestU32(original, cursor + 20) &&
                  call.base == LoadGuestU32(original, cursor + 24),
              "Native sampler flush changed its coherency/wait contract");
      auto patched = words;
      for (uint32_t i : {1u, 3u, 5u, 6u})
        patched[i] ^= 0x91E10DA5u;
      coherency::Call copied;
      Require(coherency::Decode({patched.data() + 1, patched.size() - 1}, copied) &&
                  copied.destination_base == (call.destination_base ^ 0x91E10DA5u) &&
                  copied.status == (call.status ^ 0x91E10DA5u) &&
                  copied.size == (call.size ^ 0x91E10DA5u) &&
                  copied.base == (call.base ^ 0x91E10DA5u),
              "Copied/patched coherency operands did not survive replay");
      for (uint32_t offset : {0u, 8u, 16u, 28u})
        StoreGuestU32(normalized.data(), cursor + offset, LoadGuestU32(original, cursor + offset));
      cursor += 52;
      ++counts.coherency;
      continue;
    }
    const uint32_t first = header & 0x7FFF, size = ((header >> 16) & 0x3FFF) + 1;
    if (first == 0x2102 && cursor == end - (indexed ? 24 : 16)) {
      Require(size == 1 && (LoadGuestU32(original, cursor + 8) & 0xFFFFu) == 0x2201u,
              "Range draw lost its original header/operand offsets");
      const uint32_t words = ((LoadGuestU32(original, cursor + 8) >> 16) & 0x3FFF) + 4;
      std::array<uint32_t, 7> record;
      for (uint32_t i = 0; i < words; ++i)
        record[i] = LoadGuestU32(native, cursor + i * 4);
      calls::RangeDrawCall call;
      Require(record[0] == (0xC0000000u | ((words - 2) << 16) | (calls::kRangeDrawOpcode << 8)) &&
                  calls::DecodeRange({record.data() + 1, words - 1}, call),
              "Range draw has invalid native arguments");
      // Replay after copying and editing the original first-vertex/count/index
      // operands. These positions are used by recorded command-buffer patches.
      const auto original_call = call;
      record[1] ^= 0xCDEF0123u;
      record[4] ^= 0x7F000000u;
      if (words == 7) {
        record[5] ^= 0x01234560u;
        record[6] ^= 0x80000000u;
      }
      Require(calls::DecodeRange({record.data() + 1, words - 1}, call) &&
                  call.first_vertex == (original_call.first_vertex ^ 0xCDEF0123u) &&
                  call.initiator == (original_call.initiator ^ 0x7F000000u) &&
                  (words != 7 || (call.index_base == (original_call.index_base ^ 0x01234560u) &&
                                  call.index_size == (original_call.index_size ^ 0x80000000u))),
              "Copied/patched range operands did not survive replay");
      StoreGuestU32(normalized.data(), cursor, header);
      StoreGuestU32(normalized.data(), cursor + 8, LoadGuestU32(original, cursor + 8));
      cursor += words * 4;
      ++counts.draws;
      ++counts.patched;
      continue;
    }
    if (first >= 0x4000 && first < 0x4800) {
      constants::ConstantRange decoded;
      Require(constants::DecodeHeader(replacement, size, decoded) &&
                  decoded.first_register == first && decoded.register_count == size,
              "Native flush changed float-constant destinations");
      ++counts.constants;
    } else {
      state::StateRange decoded;
      Require(state::DecodeHeader(replacement, size, decoded) && decoded.first_register == first &&
                  decoded.register_count == size,
              "Native flush changed render/fetch destinations");
      ++counts.state;
    }
    StoreGuestU32(normalized.data(), cursor, header);
    cursor += (size + 1) * 4;
  }
  Require(cursor == end + 4, "Native transport changed the full command span");
}
}  // namespace

int main(int argc, char** argv) {
  if (argc > 2 || (argc == 2 && !std::freopen(argv[1], "w", stdout)))
    return 2;
  try {
    reference_memory = std::make_unique<GuestMemoryFixture>(kSize);
    native_memory = std::make_unique<GuestMemoryFixture>(kSize);
    for (auto* memory : {reference_memory.get(), native_memory.get()})
      memory->Commit(kTable, 4096);
    native_memory->Commit(0xA0001000, 14916);
    for (uint32_t i = 0; i < pristine.size(); ++i)
      pristine[i] = uint8_t(i * 31 + (i >> 7));
    std::vector<Dirty> masks(1);
    for (uint32_t bank = 0; bank < 5; ++bank) {
      const uint64_t allowed = bank < 2    ? UINT64_MAX
                               : bank == 2 ? 0x03FFFFFFFFE1FFFFull
                               : bank == 3 ? 0x007FFFFCFFFFFFFFull
                                           : kMiscMask;
      for (uint32_t bit = 0; bit < 64; ++bit) {
        if (!(allowed & (uint64_t(1) << bit)))
          continue;
        Dirty mask{};
        mask[bank] = uint64_t(1) << bit;
        masks.push_back(mask);
      }
    }
    const Dirty all = {UINT64_MAX, UINT64_MAX, 0x03FFFFFFFFE1FFFFull, 0x007FFFFCFFFFFFFFull,
                       kMiscMask};
    masks.push_back(all);
    for (uint64_t pattern : {0x5555555555555555ull, 0xAAAAAAAAAAAAAAAAull, 0x00FF00FF00FF00FFull}) {
      auto dirty = all;
      for (auto& value : dirty)
        value &= pattern;
      masks.push_back(dirty);
    }
    for (uint32_t family = 0; family < 4; ++family)
      for (uint32_t alignment : {0u, 4u, 8u, 12u})
        for (uint32_t variant = 0; variant < 9; ++variant)
          for (const auto& dirty : masks) {
            current_family = family;
            current_variant = variant;
            current_alignment = alignment;
            current_dirty = dirty;
            auto original = Prepare(family, variant, alignment, dirty), native = original;
            Reference(family, original, reference_memory->data());
            const auto draws_before = reference_draw_calls, helpers_before = reference_calls;
            Require(Native(family, native, native_memory->data()),
                    "Supported draw unexpectedly fell back");
            Require(draws_before == reference_draw_calls && helpers_before == reference_calls,
                    "Native path called an original draw/flush body");
            Abi(original, native);
            const uint32_t end = LoadGuestU32(reference_memory->data(), kDevice + 48);
            Require(end == LoadGuestU32(native_memory->data(), kDevice + 48),
                    "Draw cursor/publication changed");
            std::vector<uint8_t> normalized(native_memory->data(), native_memory->data() + kSize);
            Replay(reference_memory->data(), native_memory->data(), kCursor + alignment, end,
                   normalized, family & 1);
            EqualMemory(normalized, family);
            ++counts.cases;
            ++counts.families[family];
          }
    for (uint32_t family = 0; family < 4; ++family)
      for (uint32_t problem = 0; problem < 16; ++problem) {
        auto ctx = Prepare(family, 8, 0, all);
        auto* base = native_memory->data();
        switch (problem) {
          case 0:
            StoreGuestU32(base, kDevice + 52, kCursor + 32);
            break;
          case 1:
            StoreGuestU32(base, kDevice + 56, kCursor - 4);
            break;
          case 2:
            port::Store64(base, kDevice + 16, uint64_t(1) << 17);
            break;
          case 3:
            port::Store64(base, kDevice + 32, uint64_t(1) << (kSamplers ? 48 : 49));
            break;
          case 4:
            port::Store64(base, kDevice + 24, uint64_t(1) << 32);
            break;
          case 5:
            port::Store64(base, kDevice + 40, all[2]);
            break;
          case 6:
            StoreGuestU32(base, kDevice + 48, kDevice + 4096);
            break;
          case 7:
            StoreGuestU32(base, kDevice + 48, kStack - 192);
            break;
          case 8:
            *simpsons::GuestToHost(base, kDevice + 10940) = 1;
            break;
          case 9:
            ctx.r1.u64 |= 4;
            break;
          case 10:
            (family & 1 ? ctx.r7 : ctx.r6).u64 = 65536;
            break;
          case 11:
            if (family & 1)
              StoreGuestU32(base, kDevice + 12428, kCursor + 8);
            else
              StoreGuestU32(base, kDevice + 52, kCursor + 33);
            break;
          case 12:
            port::Store64(base, kDevice + 32, uint64_t(1) << 63);
            break;
          case 13:
            port::Store64(base, kDevice + 16, uint64_t(1) << 60);
            break;
          case 14:
            ctx.r1.u64 = 128;
            break;
          case 15:
            StoreGuestU32(base, kDevice + 48, kCursor + 2);
            break;
        }
        const auto saved = ctx;
        const auto before = std::vector<uint8_t>(base, base + kSize);
        const auto draws_before = reference_draw_calls, helpers_before = reference_calls;
        Require(!Native(family, ctx, base) && !std::memcmp(&ctx, &saved, sizeof(ctx)) &&
                    std::equal(before.begin(), before.end(), base) &&
                    draws_before == reference_draw_calls && helpers_before == reference_calls,
                "Unsupported draw modified memory/context or ran an original helper");
        ++counts.rejected;
      }
    // A valid, complete flush must still be discarded if only its final draw
    // would cross a capacity/growth boundary. No partial publication is valid.
    for (uint32_t family = 0; family < 4; ++family)
      for (uint32_t boundary = 0; boundary < 2; ++boundary) {
        auto ctx = Prepare(family, 8, 0, all);
        auto* base = native_memory->data();
        simpsons::native_gpu_flush::FlushPlan plan;
        const uint32_t frame = family & 1 ? 240 : 208, bytes = family & 1 ? 28 : 20;
        Require(simpsons::native_gpu_flush::PlanFlush(ctx, base, frame, all, plan, kSamplers),
                "Late-boundary probe did not start with a valid full flush");
        StoreGuestU32(base, kDevice + (boundary ? 56 : 52),
                      plan.cursor + (boundary ? uint32_t(-4) : bytes - 4));
        Require(simpsons::native_gpu_flush::PlanFlush(ctx, base, frame, all, plan, kSamplers),
                "Late-boundary probe rejected a flush instead of its final draw");
        const auto saved = ctx;
        const auto before = std::vector<uint8_t>(base, base + kSize);
        Require(!Native(family, ctx, base) && !std::memcmp(&ctx, &saved, sizeof(ctx)) &&
                    std::equal(before.begin(), before.end(), base),
                "A final draw rejection left a partially published flush");
        ++counts.rejected;
        ++counts.late_draw_fallbacks;
      }
    if (kSamplers) {
      std::vector<uint64_t> masks;
      for (uint32_t first = 0; first < 30; ++first)
        for (uint32_t length = 1; length <= 30 - first; ++length)
          masks.push_back(((uint64_t(1) << length) - 1) << (64 - first - length));
      for (uint64_t pattern : {0xAAAAAAA800000000ull, 0x5555555400000000ull})
        masks.push_back(pattern);
      for (uint32_t alignment : {0u, 4u, 8u, 12u})
        for (uint64_t mask : masks) {
          auto original = Prepare(0, 0, alignment, {});
          original.r4.u64 = mask;
          auto native = original;
          reference_sub_8245E988(original, reference_memory->data());
          const auto helpers = reference_calls;
          Require(simpsons::native_gpu_samplers::WriteSamplers(native, native_memory->data()) &&
                      helpers == reference_calls,
                  "Standalone native sampler called its original or unexpectedly fell back");
          Abi(original, native);
          Require(original.r4.u64 == native.r4.u64, "Sampler changed the returned command cursor");
          const uint32_t end = LoadGuestU32(reference_memory->data(), kDevice + 48);
          Require(end == LoadGuestU32(native_memory->data(), kDevice + 48),
                  "Standalone sampler cursor/publication changed");
          std::vector<uint8_t> normalized(native_memory->data(), native_memory->data() + kSize);
          Replay(reference_memory->data(), native_memory->data(), kCursor + alignment, end,
                 normalized, false);
          EqualMemory(normalized, 0, 160);
          ++counts.standalone_samplers;
        }
      for (uint32_t problem = 0; problem < 12; ++problem) {
        auto ctx = Prepare(0, 0, 0, {});
        ctx.r4.u64 = 0xFC00000000000000ull;
        auto* base = native_memory->data();
        switch (problem) {
          case 0:
            ctx.r4.u64 = 0;
            break;
          case 1:
            ctx.r4.u64 |= 1;
            break;
          case 2:
            ctx.r1.u64 = 128;
            break;
          case 3:
            ctx.r3.u64 |= 1;
            break;
          case 4:
            StoreGuestU32(base, kDevice + 48, kCursor + 2);
            break;
          case 5:
            StoreGuestU32(base, kDevice + 56, kCursor - 4);
            break;
          case 6:
            StoreGuestU32(base, kDevice + 52, kCursor + 52 + 96 + 4);
            break;
          case 7:
            StoreGuestU32(base, kDevice + 48, kDevice + 10272);
            break;
          case 8:
            StoreGuestU32(base, kDevice + 48, kStack - 96);
            StoreGuestU32(base, kDevice + 56, kStack);
            StoreGuestU32(base, kDevice + 52, kStack + 4096);
            break;
          case 9:
          case 10:
            // A/C map the same physical heap; E adds a physical-page offset.
            // Low virtual memory is a separate backing, not an A-view alias.
            std::memcpy(simpsons::GuestToHost(base, 0xA0001000), base + kDevice, 14916);
            ctx.r3.u64 = 0xA0001000;
            StoreGuestU32(base, ctx.r3.u32 + 48, problem == 9 ? 0xC0001000u : 0xE0000000u);
            StoreGuestU32(base, ctx.r3.u32 + 52, UINT32_MAX);
            StoreGuestU32(base, ctx.r3.u32 + 56, UINT32_MAX);
            break;
          case 11:
            ctx.r1.u64 = kDevice + 176;
            break;
        }
        const auto before = std::vector<uint8_t>(base, base + kSize);
        const auto* physical_device = simpsons::GuestToHost(base, 0xA0001000);
        const auto physical_before = std::vector<uint8_t>(physical_device, physical_device + 14916);
        const auto saved = ctx;
        Require(!simpsons::native_gpu_samplers::WriteSamplers(ctx, base) &&
                    !std::memcmp(&saved, &ctx, sizeof(ctx)) &&
                    std::equal(before.begin(), before.end(), base) &&
                    std::equal(physical_before.begin(), physical_before.end(), physical_device),
                "Unsupported standalone sampler partially emitted or modified its context");
        ++counts.sampler_fallbacks;
      }
      const auto original = coherency::Encode({0x12345678, 0x10000, 0x1800, 0x2000});
      for (uint32_t index : {2u, 4u, 7u, 8u, 9u, 10u, 11u, 12u}) {
        auto invalid = original;
        invalid[index] ^= 1;
        coherency::Call output{1, 2, 3, 4}, saved = output;
        Require(!coherency::Decode({invalid.data() + 1, invalid.size() - 1}, output) &&
                    !std::memcmp(&saved, &output, sizeof(output)),
                "Malformed coherency call was accepted or modified its output");
        ++counts.coherency_rejections;
      }
    }
    std::printf(
        "{\"passed\":true,\"cases\":%llu,\"normal_auto\":%llu,\"normal_indexed\":%llu,"
        "\"vertex_id_auto\":%llu,\"vertex_id_indexed\":%llu,\"state_ranges\":%llu,"
        "\"float_ranges\":%llu,\"padding_words\":%llu,\"draws\":%llu,"
        "\"copied_patched_draws\":%llu,\"unchanged_fallbacks\":%llu,"
        "\"late_draw_fallbacks\":%llu,\"coherency_calls\":%llu,\"standalone_samplers\":%llu,"
        "\"sampler_fallbacks\":%llu,\"coherency_rejections\":%llu,"
        "\"native_original_builder_calls\":0}\n",
        (unsigned long long)counts.cases, (unsigned long long)counts.families[0],
        (unsigned long long)counts.families[1], (unsigned long long)counts.families[2],
        (unsigned long long)counts.families[3], (unsigned long long)counts.state,
        (unsigned long long)counts.constants, (unsigned long long)counts.padding,
        (unsigned long long)counts.draws, (unsigned long long)counts.patched,
        (unsigned long long)counts.rejected, (unsigned long long)counts.late_draw_fallbacks,
        (unsigned long long)counts.coherency, (unsigned long long)counts.standalone_samplers,
        (unsigned long long)counts.sampler_fallbacks,
        (unsigned long long)counts.coherency_rejections);
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "%s\n", e.what());
    std::fprintf(
        stderr, "family=%u variant=%u alignment=%u masks=%016llX/%016llX/%016llX/%016llX/%016llX\n",
        current_family, current_variant, current_alignment, (unsigned long long)current_dirty[0],
        (unsigned long long)current_dirty[1], (unsigned long long)current_dirty[2],
        (unsigned long long)current_dirty[3], (unsigned long long)current_dirty[4]);
    return 1;
  }
}
