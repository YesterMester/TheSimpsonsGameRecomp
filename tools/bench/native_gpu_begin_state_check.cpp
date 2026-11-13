// Execute the generated Begin/End, flush and allocation bodies as the oracle.
// The native path retains the actual allocator, not a replacement allocator.
#include "simpsons_init.h"
#include "native_gpu_direct_draws.h"
#include "native_gpu_begin.h"
#include "guest_memory_fixture.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <vector>

static uint64_t reference_draw_calls = 0, reference_calls = 0;
static uint64_t reference_begin_calls = 0, allocator_body_calls = 0;
static uint64_t recorded_allocator_calls = 0;
#include "native_gpu_begin_state_reference.inc"

namespace port = simpsons::native_gpu_vertices;
namespace calls = rex::graphics::native_gpu_calls;
namespace state = rex::graphics::native_gpu_state;
namespace constants = rex::graphics::native_gpu_constants;
namespace coherency = rex::graphics::native_gpu_coherency;
using simpsons::LoadGuestU32;
using simpsons::StoreGuestU32;
using Dirty = std::array<uint64_t, 5>;

namespace {
constexpr uint32_t kDevice = 0x1000, kSync = 0x8000, kOutVertex = 0x9000, kOutIndex = 0x9004;
constexpr uint32_t kCursor = 0x10000, kStack = 0x3F0000, kSize = 4 * 1024 * 1024;
constexpr uint32_t kRecording = 0xA000;
std::unique_ptr<GuestMemoryFixture> reference_memory, native_memory;
std::array<uint8_t, kSize> pristine;
constexpr Dirty kAll = {UINT64_MAX, UINT64_MAX, 0x03FFFFFFFFE1FFFFull, 0x007FFFFCFFFFFFFFull,
                        0x017E3FFFFFFFFFFFull};
constexpr std::array<PPCRegister PPCContext::*, 18> kNonvolatile = {
    &PPCContext::r14, &PPCContext::r15, &PPCContext::r16, &PPCContext::r17, &PPCContext::r18,
    &PPCContext::r19, &PPCContext::r20, &PPCContext::r21, &PPCContext::r22, &PPCContext::r23,
    &PPCContext::r24, &PPCContext::r25, &PPCContext::r26, &PPCContext::r27, &PPCContext::r28,
    &PPCContext::r29, &PPCContext::r30, &PPCContext::r31};
struct Counts {
  uint64_t cases = 0, indexed = 0, allocation_failures = 0, real_allocations = 0;
  uint64_t state_ranges = 0, float_ranges = 0, coherency = 0, copied_patched = 0, rejected = 0;
  uint64_t published_flushes = 0;
  uint64_t recorded_cases = 0, recorded_failures = 0, second_allocation_failures = 0;
  uint64_t recorded_rejected = 0;
} counts;
uint32_t current_indexed = 0, current_alignment = 0, current_allocation = 0, current_variant = 0;
uint32_t current_recording = 0;
Dirty current_dirty{};

void Require(bool ok, const char* why) {
  if (!ok)
    throw std::runtime_error(why);
}

PPCContext Prepare(bool indexed, uint32_t alignment, uint32_t allocation, uint32_t variant,
                   const Dirty& dirty, bool large = false) {
  auto* base = reference_memory->data();
  std::memcpy(base, pristine.data(), kSize);
  std::memset(base + kDevice, 0, 14916);
  for (uint32_t offset = 1152; offset < 10900; offset += 4)
    StoreGuestU32(base, kDevice + offset, 0x91E10DA5u ^ (offset * 0x10203u + variant));
  for (uint32_t i = 0; i < dirty.size(); ++i)
    port::Store64(base, kDevice + i * 8, dirty[i]);
  port::Store64(base, kDevice + 40, 0);
  StoreGuestU32(base, kDevice + 48, kCursor + alignment);
  StoreGuestU32(base, kDevice + 52, 0x30000);
  StoreGuestU32(base, kDevice + 56, 0x2F000);
  StoreGuestU32(base, kDevice + 10896, kSync);
  StoreGuestU32(base, kDevice + 14884, 0x3C0000);
  StoreGuestU32(base, kDevice + 14880, 0x30000);
  StoreGuestU32(base, kDevice + 14892, allocation == 2 ? 0 : 0x180003);
  StoreGuestU32(base, kDevice + 14896, variant * 0x55555555u);
  StoreGuestU32(base, kSync + 4, variant * 0x55555555u + 4);
  StoreGuestU32(base, kDevice + 14900, allocation == 3 && !indexed ? 0x30004 : 0x30000);
  StoreGuestU32(base, kDevice + 14908, allocation == 2 ? 0xFFFFFFF0u : 0);
  StoreGuestU32(base, kDevice + 14912, allocation == 3 ? 1 : UINT32_MAX);
  if (allocation == 1)
    *simpsons::GuestToHost(base, kDevice + 10941) = 0x20;
  const uint32_t stride = large ? 28 : std::array{4u, 16u, 28u, 64u}[variant % 4];
  *simpsons::GuestToHost(base, kDevice + 11824) = uint8_t(stride / 4);
  *simpsons::GuestToHost(base, kDevice + 12520) = uint8_t(variant * 37);
  *simpsons::GuestToHost(base, kDevice + 12179) = uint8_t(variant * 31);
  *simpsons::GuestToHost(base, kDevice + 10940) = variant & 1 ? 0x80 : 0;
  StoreGuestU32(base, kStack + 84, kOutVertex);
  std::memcpy(native_memory->data(), base, kSize);
  PPCContext ctx{};
  ctx.r1.u64 = kStack;
  ctx.r3.u64 = 0xA5A5000000000000ull | kDevice;
  ctx.r4.u64 = 0xABCD000000000000ull | (variant & 1 ? 6u : 1u);
  const uint32_t count = large ? 65535 : std::array{1u, 7u, 31u, 64u}[variant % 4];
  ctx.r5.u64 = indexed ? 0x12345678 : count;
  ctx.r6.u64 = indexed ? count : stride;
  ctx.r7.u64 = large ? 65535 : 3 + variant % 61;
  ctx.r8.u64 = variant & 7;
  ctx.r9.u64 = stride;
  ctx.r10.u64 = kOutIndex;
  ctx.lr = 0xABCD000082123450ull;
  ctx.fpscr.InitHost();
  for (uint32_t i = 0; i < kNonvolatile.size(); ++i)
    (ctx.*kNonvolatile[i]).u64 = 0xDEADBEEF00000000ull + i;
  return ctx;
}

void PrepareRecording(const PPCContext& ctx, bool indexed, uint32_t kind) {
  auto* base = reference_memory->data();
  auto dirty = simpsons::native_gpu_flush::DirtyState(base, kDevice);
  if (!indexed)
    dirty[3] &= ~uint64_t(1);
  simpsons::native_gpu_flush::FlushPlan flush;
  Require(simpsons::native_gpu_flush::PlanFlush(ctx, base, indexed ? 240 : 176, dirty, flush, true),
          "Recording fixture did not reach a valid original shadow flush");
  const uint32_t bytes = indexed ? ctx.r6.u32 * ctx.r9.u32 : ctx.r5.u32 * ctx.r6.u32;
  uint32_t tail = 0x3C0000;
  if (kind == 3)
    tail = flush.cursor + 164;  // First allocation runs out of storage without ring growth.
  if (kind == 4)
    tail = ((flush.cursor + 179) & ~15u) + bytes;  // Indexed allocation fails after vertices.
  StoreGuestU32(base, kDevice + 13492, kRecording);
  StoreGuestU32(base, kRecording + 108, 0x1802);
  StoreGuestU32(base, kRecording + 152, kCursor);
  StoreGuestU32(base, kRecording + 156, tail - kCursor);
  StoreGuestU32(base, kDevice + 52, tail - 4);
  StoreGuestU32(base, kDevice + 56, tail - 164);
  StoreGuestU32(base, kDevice + 14892, kind == 2 ? tail - 32 : 0);
  if (kind == 5 || kind == 7) {
    StoreGuestU32(base, kDevice + 14912, 1);
    StoreGuestU32(base, kDevice + 14900, kind == 7 ? 0x30004 : 0x30000);
  }
  if (kind == 6)
    StoreGuestU32(base, kDevice + 14908, 0xFFFFFFF0u);
  if (kind == 8)
    *simpsons::GuestToHost(base, kDevice + 10941) = 0x20;
  std::memcpy(native_memory->data(), base, kSize);
}

void Abi(const PPCContext& a, const PPCContext& b) {
  Require(
      a.r1.u64 == b.r1.u64 && a.r3.u64 == b.r3.u64 && a.lr == b.lr && a.fpscr.csr == b.fpscr.csr,
      "Begin changed return/stack/link/FPSCR ABI");
  for (auto field : kNonvolatile)
    Require((a.*field).u64 == (b.*field).u64, "Begin changed a nonvolatile GPR");
}

void EqualMemory(const std::vector<uint8_t>& normalized, bool indexed) {
  const auto* base = reference_memory->data();
  const uint32_t frame = indexed ? 608 : 544;
  for (const auto& [start, end] : {std::pair{0u, kStack - frame}, std::pair{kStack, kSize}}) {
    if (std::memcmp(base + start, normalized.data() + start, end - start)) {
      for (uint32_t i = start; i < end; ++i)
        if (base[i] != normalized[i]) {
          std::fprintf(stderr, "Memory mismatch at %08X: original=%02X native=%02X\n", i, base[i],
                       normalized[i]);
          break;
        }
      throw std::runtime_error("Begin changed guest data/state/allocation outside private frames");
    }
  }
}

void Normalize(std::vector<uint8_t>& normalized, uint32_t start, uint32_t end, bool indexed) {
  const auto* original = reference_memory->data();
  const auto* native = native_memory->data();
  uint32_t cursor = start + 4;
  while (cursor <= end) {
    const uint32_t header = LoadGuestU32(original, cursor);
    const uint32_t replacement = LoadGuestU32(native, cursor);
    if (header == 0x80000000u) {
      Require(replacement == header, "Begin changed alignment padding");
      cursor += 4;
      continue;
    }
    Require((header >> 30) == 0, "Unexpected original Begin state operation");
    if (header == 0x2007 && cursor + 48 <= end && LoadGuestU32(original, cursor + 8) == 0xA31) {
      std::array<uint32_t, 12> payload;
      for (uint32_t i = 0; i < payload.size(); ++i)
        payload[i] = LoadGuestU32(native, cursor + 4 + i * 4);
      coherency::Call call;
      Require(replacement == coherency::kHeader && coherency::Decode(payload, call),
              "Begin lost its ordered sampler coherency");
      for (uint32_t offset : {0u, 8u, 16u, 28u})
        StoreGuestU32(normalized.data(), cursor + offset, LoadGuestU32(original, cursor + offset));
      cursor += 52;
      ++counts.coherency;
      continue;
    }
    const uint32_t first = header & 0x7FFF, size = ((header >> 16) & 0x3FFF) + 1;
    const uint32_t dwords = indexed ? 18 : 16;
    if (header == 0x2102 && cursor + (dwords - 1) * 4 == end) {
      std::array<uint32_t, 18> record;
      for (uint32_t i = 0; i < dwords; ++i)
        record[i] = LoadGuestU32(native, cursor + i * 4);
      calls::VerticesDrawCall call;
      Require(
          record[0] == (0xC0000000u | ((dwords - 2) << 16) | (calls::kVerticesDrawOpcode << 8)) &&
              calls::DecodeVertices({record.data() + 1, dwords - 1}, call),
          "Begin lost its deferred native draw arguments");
      const auto before = call;
      record[1] ^= 0x91E10DA5u;
      record[7] ^= 0x01234560u;
      record[15] ^= 0x7F000000u;
      if (indexed)
        record[16] ^= 0x01234560u;
      Require(calls::DecodeVertices({record.data() + 1, dwords - 1}, call) &&
                  call.first_vertex == (before.first_vertex ^ 0x91E10DA5u) &&
                  call.fetch[4] == (before.fetch[4] ^ 0x01234560u) &&
                  call.initiator == (before.initiator ^ 0x7F000000u) &&
                  (!indexed || call.index_base == (before.index_base ^ 0x01234560u)),
              "Copied/patched Begin operands changed their original positions");
      for (uint32_t word : {0u, 2u, 9u, 13u})
        StoreGuestU32(normalized.data(), cursor + word * 4,
                      LoadGuestU32(original, cursor + word * 4));
      cursor += dwords * 4;
      ++counts.copied_patched;
      continue;
    }
    if (first >= 0x4000 && first < 0x4800) {
      constants::ConstantRange decoded;
      Require(constants::DecodeHeader(replacement, size, decoded) &&
                  decoded.first_register == first && decoded.register_count == size,
              "Begin changed float-constant destinations");
      ++counts.float_ranges;
    } else {
      state::StateRange decoded;
      Require(state::DecodeHeader(replacement, size, decoded) && decoded.first_register == first &&
                  decoded.register_count == size,
              "Begin changed render/fetch/bool/sampler destinations");
      ++counts.state_ranges;
    }
    StoreGuestU32(normalized.data(), cursor, header);
    cursor += (size + 1) * 4;
  }
  Require(cursor == end + 4, "Begin changed its complete command span");
}

void Case(bool indexed, uint32_t alignment, uint32_t allocation, uint32_t variant,
          const Dirty& dirty, bool large = false, uint32_t recording = 0) {
  current_indexed = indexed;
  current_alignment = alignment;
  current_allocation = allocation;
  current_variant = variant;
  current_dirty = dirty;
  current_recording = recording;
  auto original = Prepare(indexed, alignment, allocation, variant, dirty, large), native = original;
  if (recording)
    PrepareRecording(original, indexed, recording);
  if (indexed)
    reference_sub_8244C970(original, reference_memory->data());
  else
    reference_sub_8244C450(original, reference_memory->data());
  const auto begins = reference_begin_calls, helpers = reference_calls,
             allocations = allocator_body_calls;
  const bool accepted = port::BeginWithState(native, native_memory->data(), reference_sub_82456D60,
                                             indexed, true, true);
  if (!accepted) {
    const auto* base = native_memory->data();
    auto dirty = simpsons::native_gpu_flush::DirtyState(base, kDevice);
    if (!indexed)
      dirty[3] &= ~uint64_t(1);
    simpsons::native_gpu_flush::FlushPlan flush;
    const bool flushed = simpsons::native_gpu_flush::PlanFlush(native, base, indexed ? 240 : 176,
                                                               dirty, flush, true);
    std::fprintf(stderr, "flush=%u cursor=%08X capacity=%08X limit=%08X tail=%08X "
                         "owner=%08X/%08X state=%08X\n", flushed, flush.cursor,
                 LoadGuestU32(base, kDevice + 52), LoadGuestU32(base, kDevice + 56),
                 LoadGuestU32(base, kDevice + 14892), LoadGuestU32(base, kRecording + 152),
                 LoadGuestU32(base, kRecording + 156), LoadGuestU32(base, kRecording + 108));
  }
  Require(accepted, "Prepared dirty Begin unexpectedly fell back");
  Require(begins == reference_begin_calls && helpers == reference_calls &&
              allocator_body_calls == allocations + (indexed ? 2 : 1),
          "Begin used original flush/builders, bypassed or repeated a real allocation");
  counts.real_allocations += indexed ? 2 : 1;
  Abi(original, native);
  const uint32_t before_end = LoadGuestU32(reference_memory->data(), kDevice + 48);
  Require(before_end == LoadGuestU32(native_memory->data(), kDevice + 48),
          "Begin published a draw before the caller filled its buffers");
  if (before_end != kCursor + alignment)
    ++counts.published_flushes;
  const bool success = indexed ? original.r3.u32 == 0 : original.r3.u32 != 0;
  if (success) {
    const uint32_t vertex =
        indexed ? LoadGuestU32(reference_memory->data(), kOutVertex) : original.r3.u32;
    const uint32_t native_vertex =
        indexed ? LoadGuestU32(native_memory->data(), kOutVertex) : native.r3.u32;
    Require(vertex == native_vertex && vertex >= (recording ? kCursor : 0x30000u),
            "Begin changed the returned vertex pointer");
    const uint32_t bytes = LoadGuestU32(reference_memory->data(), kDevice + 13440) * 4;
    for (uint32_t i = 0; i < bytes; ++i)
      reference_memory->data()[vertex + i] = native_memory->data()[vertex + i] =
          uint8_t(i * 31 + variant);
    if (indexed) {
      const uint32_t index = LoadGuestU32(reference_memory->data(), kOutIndex);
      Require(index == LoadGuestU32(native_memory->data(), kOutIndex),
              "Begin changed its index pointer");
      const uint32_t bytes = LoadGuestU32(reference_memory->data(), kDevice + 13444) * 4;
      for (uint32_t i = 0; i < bytes; ++i)
        reference_memory->data()[index + i] = native_memory->data()[index + i] =
            uint8_t(i * 17 + variant);
    }
    // End contexts are independent of volatile Begin outputs.
    PPCContext end_reference{}, end_native{};
    end_reference.r1.u64 = kStack;
    end_reference.r3.u64 = kDevice;
    end_reference.lr = 0x82123450;
    end_native = end_reference;
    if (indexed)
      reference_sub_8244CEA0(end_reference, reference_memory->data());
    else
      reference_sub_8244C8F0(end_reference, reference_memory->data());
    Require(port::EndVertices(end_native, native_memory->data()),
            "Native End unexpectedly fell back");
    Abi(end_reference, end_native);
    Require(
        end_reference.r10.u64 == end_native.r10.u64 && end_reference.r11.u64 == end_native.r11.u64,
        "End changed its cursor/flag outputs");
  } else {
    ++counts.allocation_failures;
    if (recording) {
      ++counts.recorded_failures;
      if (recording == 4 && indexed)
        ++counts.second_allocation_failures;
    }
  }
  const uint32_t end = LoadGuestU32(reference_memory->data(), kDevice + 48);
  Require(end == LoadGuestU32(native_memory->data(), kDevice + 48),
          "Begin/fill/End publication differs");
  std::vector<uint8_t> normalized(native_memory->data(), native_memory->data() + kSize);
  Normalize(normalized, kCursor + alignment, end, indexed);
  EqualMemory(normalized, indexed);
  ++counts.cases;
  counts.indexed += indexed;
  counts.recorded_cases += recording != 0;
}

void RejectRecordings(bool indexed) {
  for (uint32_t problem = 0; problem < 23; ++problem) {
    auto ctx = Prepare(indexed, 0, 0, 7, kAll);
    PrepareRecording(ctx, indexed, 1);
    auto* base = native_memory->data();
    switch (problem) {
      case 0: StoreGuestU32(base, kRecording + 152, 0); break;
      case 1: StoreGuestU32(base, kRecording + 156, 0); break;
      case 2: StoreGuestU32(base, kRecording + 108, 1); break;
      case 3: StoreGuestU32(base, kRecording + 108, 7); break;
      case 4: StoreGuestU32(base, kDevice + 13492, kRecording + 1); break;
      case 5: StoreGuestU32(base, kDevice + 13492, kDevice + 64); break;
      case 6: StoreGuestU32(base, kDevice + 13492, kCursor + 4); break;
      case 7: StoreGuestU32(base, kRecording + 152, kCursor + 8); break;
      case 8: StoreGuestU32(base, kRecording + 156, 0x100); break;
      case 9: StoreGuestU32(base, kRecording + 156, 0x7EFFFFF4); break;
      case 10: StoreGuestU32(base, kRecording + 152, kCursor + 1); break;
      case 11: StoreGuestU32(base, kRecording + 156, 0x1FFFFF); break;
      case 12: StoreGuestU32(base, kDevice + 14892, 0x3C0004); break;
      case 13: StoreGuestU32(base, kDevice + 14892, 0xA0030000); break;
      case 14: StoreGuestU32(base, kDevice + 14892, 0x180001); break;
      case 15: StoreGuestU32(base, kRecording + 152, kRecording); break;
      case 16: StoreGuestU32(base, kRecording + 152, kDevice); break;
      case 17: StoreGuestU32(base, kRecording + 156, kSize - kCursor); break;
      case 18: StoreGuestU32(base, kDevice + 52, 0x3BFFFF); break;
      case 19: StoreGuestU32(base, kDevice + 56, 0x3BFFFF); break;
      case 20: StoreGuestU32(base, kRecording + 152, 0xFFF00000); break;
      case 21:
        if (indexed) {
          StoreGuestU32(base, kDevice + 14912, 1);
          StoreGuestU32(base, kDevice + 14900, 0x30004);
        } else {
          StoreGuestU32(base, kRecording + 152, kOutVertex);
        }
        break;
      case 22: {
        // The conditional-state source extends beyond the allocator fields.
        // A physically owned command span must not own that device tail.
        constexpr uint32_t object = 0x500, storage = kDevice + 14916;
        std::memcpy(base + object, base + kRecording, 184);
        StoreGuestU32(base, kDevice + 13492, object);
        StoreGuestU32(base, object + 152, storage);
        StoreGuestU32(base, object + 156, 0x3C0000 - storage);
        break;
      }
    }
    const auto saved = ctx;
    const auto before = std::vector<uint8_t>(base, base + kSize);
    const auto allocations = allocator_body_calls, recorded = recorded_allocator_calls;
    Require(!port::BeginWithState(ctx, base, reference_sub_82456D60, indexed, true, true) &&
                !std::memcmp(&saved, &ctx, sizeof(ctx)) &&
                std::equal(before.begin(), before.end(), base) &&
                allocator_body_calls == allocations && recorded_allocator_calls == recorded,
            "Unproved recording was flushed, allocated or changed before rejection");
    ++counts.recorded_rejected;
  }
}

void RejectCases(bool indexed) {
  for (uint32_t problem = 0; problem < 20; ++problem) {
    const uint32_t variant = problem >= 7 && problem <= 9 ? 0 : 7;
    auto ctx = Prepare(indexed, 0, 0, variant, kAll);
    auto* base = native_memory->data();
    bool state_enabled = true, samplers = true;
    const auto plan_flush = [&] {
      auto dirty = simpsons::native_gpu_flush::DirtyState(base, kDevice);
      if (!indexed)
        dirty[3] &= ~uint64_t(1);
      simpsons::native_gpu_flush::FlushPlan plan;
      Require(
          simpsons::native_gpu_flush::PlanFlush(ctx, base, indexed ? 240 : 176, dirty, plan, true),
          "Late rejection probe did not reach a valid whole-flush plan");
      return plan;
    };
    switch (problem) {
      case 0:
        port::Store64(base, kDevice + 16, uint64_t(1) << 17);
        break;
      case 1:
        port::Store64(base, kDevice + 40, kAll[2]);
        break;
      case 2:
        samplers = false;
        break;
      case 3:
        state_enabled = false;
        break;
      case 4:
        StoreGuestU32(base, kDevice + 52, kCursor + 32);
        break;
      case 5: {
        const auto plan = plan_flush();
        StoreGuestU32(base, kDevice + 52, plan.cursor + (indexed ? 68 : 60));
        (void)plan_flush();
        break;
      }
      case 6: {
        const auto plan = plan_flush();
        StoreGuestU32(base, kDevice + 56, plan.cursor - 4);
        (void)plan_flush();
        break;
      }
      case 7:
      case 8:
      case 9: {
        const auto plan = plan_flush();
        if (problem == 7)
          StoreGuestU32(base, kDevice + 14892, kCursor + 16);
        if (problem == 8) {
          StoreGuestU32(base, kDevice + 10896, kCursor + 16);
          StoreGuestU32(base, kCursor + 20, LoadGuestU32(base, kDevice + 14896));
        }
        if (problem == 9) {
          if (indexed)
            StoreGuestU32(base, kStack + 84, kCursor + 8);
          else
            StoreGuestU32(base, kDevice + 14892, kCursor + 32);
        }
        // These allocations/outputs do not overlap the deferred draw alone.
        // Only checking the entire preceding flush catches the actual alias.
        const port::BeginSubmission short_span{plan.cursor, plan.cursor};
        port::BeginPlan auto_plan;
        port::IndexedBeginPlan indexed_plan;
        Require(indexed ? port::PlanIndexedBegin(ctx, base, indexed_plan, &short_span)
                        : port::PlanBegin(ctx, base, auto_plan, &short_span),
                "Prefix-alias probe was already rejected without its prefix");
        break;
      }
      case 10:
        if (indexed) {
          StoreGuestU32(base, kDevice + 14912, 1);
          StoreGuestU32(base, kDevice + 14900, 0x30004);
        } else {
          StoreGuestU32(base, kDevice + 13492, 0x4000);
        }
        break;
      case 11:
        StoreGuestU32(base, kSync + 4, LoadGuestU32(base, kDevice + 14896) + 1);
        break;
      case 12:
        StoreGuestU32(base, kDevice + 14884, 0x180004);
        break;
      case 13:
        *simpsons::GuestToHost(base, kDevice + 10940) |= 1;
        break;
      case 14:
        StoreGuestU32(base, kDevice + 13216, 1);
        break;
      case 15:
        *simpsons::GuestToHost(base, kDevice + 11824) ^= 1;
        StoreGuestU32(base, kDevice + 12688, 0x4000);
        break;
      case 16: {
        for (uint32_t i = 0; i < kAll.size(); ++i)
          port::Store64(base, kDevice + i * 8, i ? 0 : 0x8000000000000000ull);
        StoreGuestU32(base, kDevice + 48, kStack - (indexed ? 624 : 568));
        StoreGuestU32(base, kDevice + 52, kStack + 1024);
        StoreGuestU32(base, kDevice + 56, kStack + 1024);
        (void)plan_flush();
        break;
      }
      case 17:
        (indexed ? ctx.r6 : ctx.r5).u64 = 0;
        break;
      case 18:
        (indexed ? ctx.r7 : ctx.r6).u64 = indexed ? 0 : 6;
        break;
      case 19:
        ctx.r1.u64 = 128;
        break;
    }
    const auto saved = ctx;
    const auto before = std::vector<uint8_t>(base, base + kSize);
    const auto allocations = allocator_body_calls, begins = reference_begin_calls,
               helpers = reference_calls;
    Require(!port::BeginWithState(ctx, base, reference_sub_82456D60, indexed, state_enabled,
                                  samplers) &&
                !std::memcmp(&ctx, &saved, sizeof(ctx)) &&
                std::equal(before.begin(), before.end(), base) &&
                allocator_body_calls == allocations && reference_begin_calls == begins &&
                reference_calls == helpers,
            "Rejected dirty Begin allocated or partially changed its commands/state/context");
    ++counts.rejected;
  }
}
}  // namespace

int main(int argc, char** argv) {
  if (argc > 2 || (argc == 2 && !std::freopen(argv[1], "w", stdout)))
    return 2;
  try {
    reference_memory = std::make_unique<GuestMemoryFixture>(kSize);
    native_memory = std::make_unique<GuestMemoryFixture>(kSize);
    for (uint32_t i = 0; i < kSize; ++i)
      pristine[i] = uint8_t(i * 29 + (i >> 7));
    std::vector<Dirty> masks(1);
    for (uint32_t bank = 0; bank < kAll.size(); ++bank)
      for (uint32_t bit = 0; bit < 64; ++bit)
        if (kAll[bank] & (uint64_t(1) << bit)) {
          Dirty dirty{};
          dirty[bank] = uint64_t(1) << bit;
          masks.push_back(dirty);
        }
    masks.push_back(kAll);
    for (uint64_t pattern : {0x5555555555555555ull, 0xAAAAAAAAAAAAAAAAull, 0x00FF00FF00FF00FFull}) {
      auto dirty = kAll;
      for (auto& value : dirty)
        value &= pattern;
      masks.push_back(dirty);
    }
    for (bool indexed : {false, true})
      for (uint32_t alignment : {0u, 4u, 8u, 12u})
        for (uint32_t allocation = 0; allocation < 4; ++allocation)
          for (uint32_t i = 0; i < masks.size(); ++i)
            Case(indexed, alignment, allocation, i + alignment / 4, masks[i]);
    for (bool indexed : {false, true})
      Case(indexed, 0, 0, 7, kAll, true);
    // A failed allocation may leave a multi-register state run beginning at
    // 2102 with exactly the size of a deferred vertices record. Its type-0
    // count must distinguish it from the scalar first-vertex draw prefix.
    for (bool indexed : {false, true}) {
      Dirty dirty{};
      const uint32_t length = indexed ? 17 : 15;
      dirty[2] = ((uint64_t(1) << length) - 1) << (40 - length);
      Case(indexed, 0, 1, 7, dirty);
    }
    for (bool indexed : {false, true})
      RejectCases(indexed);
    const std::array<Dirty, 7> recording_masks = {Dirty{}, kAll, masks[1], masks[65],
                                                masks[129], masks[175], masks.back()};
    for (bool indexed : {false, true})
      for (uint32_t kind = 1; kind <= 8; ++kind) {
        if ((kind == 4 && !indexed) || (kind == 7 && indexed))
          continue;
        for (uint32_t alignment : {0u, 4u, 8u, 12u})
          for (uint32_t variant = 0; variant < 8; ++variant)
            for (const auto& dirty : recording_masks)
              Case(indexed, alignment, 0, variant, dirty, false, kind);
      }
    for (bool indexed : {false, true})
      RejectRecordings(indexed);
    std::printf(
        "{\"passed\":true,\"cases\":%llu,\"indexed_cases\":%llu,\"real_allocation_failures\":%llu,"
        "\"real_allocator_calls\":%llu,\"state_ranges\":%llu,\"float_ranges\":%llu,"
        "\"coherency_calls\":%llu,\"copied_patched_draws\":%llu,\"published_flushes\":%llu,"
        "\"unchanged_fallbacks\":%llu,\"recorded_cases\":%llu,\"recorded_failures\":%llu,"
        "\"second_allocation_failures\":%llu,\"recorded_allocator_calls\":%llu,"
        "\"unchanged_recording_fallbacks\":%llu,"
        "\"native_original_begin_calls\":0,\"native_original_flush_calls\":0}\n",
        (unsigned long long)counts.cases, (unsigned long long)counts.indexed,
        (unsigned long long)counts.allocation_failures, (unsigned long long)counts.real_allocations,
        (unsigned long long)counts.state_ranges, (unsigned long long)counts.float_ranges,
        (unsigned long long)counts.coherency, (unsigned long long)counts.copied_patched,
        (unsigned long long)counts.published_flushes, (unsigned long long)counts.rejected,
        (unsigned long long)counts.recorded_cases, (unsigned long long)counts.recorded_failures,
        (unsigned long long)counts.second_allocation_failures,
        (unsigned long long)recorded_allocator_calls, (unsigned long long)counts.recorded_rejected);
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr,
                 "%s\nindexed=%u alignment=%u allocation=%u variant=%u recording=%u "
                 "masks=%016llX/%016llX/%016llX/%016llX/%016llX\n",
                 e.what(), current_indexed, current_alignment, current_allocation, current_variant,
                 current_recording,
                 (unsigned long long)current_dirty[0], (unsigned long long)current_dirty[1],
                 (unsigned long long)current_dirty[2], (unsigned long long)current_dirty[3],
                 (unsigned long long)current_dirty[4]);
    return 1;
  }
}
