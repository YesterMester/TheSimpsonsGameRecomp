// Execute the current generated writer as the independent CPU reference.
// The extracted body and its save/restore helpers stay in temporary storage.
#include "simpsons_init.h"
#include "native_gpu_constants.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

static uint64_t reference_writer_calls = 0, reference_slow_calls = 0;
struct ReferenceSlowPath {};

// Observe whether the real generated body takes its allocation path. We do
// not implement that allocator or count its behavior as a native success.
REX_FUNC(sub_8245E278) {
  (void)base;
  ++reference_slow_calls;
  throw ReferenceSlowPath{};
}

#include "native_gpu_constants_reference.inc"

using simpsons::LoadGuestU32;
using simpsons::StoreGuestU32;
namespace codec = rex::graphics::native_gpu_constants;

namespace {

void Require(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}

constexpr uint32_t kDevice = 0x1000, kShadow = 0x4000, kOutput = 0x10000;
constexpr uint32_t kCopy = 0x18000, kStack = 0x30000;
constexpr size_t kMemorySize = 0x40000, kOutputSpan = 0x3000;
using Memory = std::array<uint8_t, kMemorySize>;
alignas(4096) Memory reference_memory, native_memory;

struct Run {
  uint32_t header_address, first_register, register_count, capacity_threshold;
};

struct Parsed {
  std::array<uint32_t, 2048> values;
  std::array<bool, 2048> written{};
  std::vector<Run> runs;
  uint32_t padding = 0;
};

Parsed Parse(const uint8_t* base, uint32_t first_cursor, uint32_t last_cursor, bool native) {
  Parsed result;
  result.values.fill(0x91A5F00D);
  uint32_t run_cursor = first_cursor;
  for (uint32_t cursor = first_cursor + 4; cursor <= last_cursor;) {
    const uint32_t header = LoadGuestU32(base, cursor);
    if ((header >> 30) == 2) {
      Require(header == 0x80000000u, "Type-2 padding changed");
      ++result.padding;
      cursor += 4;
      continue;
    }
    const uint32_t count = ((header >> 16) & 0x3FFF) + 1;
    Require(uint64_t(cursor) + uint64_t(count) * 4 <= last_cursor,
            "Constant payload exceeds its original byte advance");
    uint32_t first;
    if (native) {
      codec::ConstantRange range;
      Require(codec::DecodeHeader(header, count, range), "Invalid native constant header");
      first = range.first_register;
      Require(!(header & 1), "Stage or group accidentally enabled bin predication");
    } else {
      Require((header >> 30) == 0 && !(header & 0x8000), "Unexpected reference GPU operation");
      first = header & 0x7FFF;
      Require(first >= 0x4000 && first + count <= 0x4800,
              "Reference wrote outside shader float constants");
    }
    Require(((cursor + 4) & 15) == 0, "Constant payload lost 16-byte alignment");
    result.runs.push_back({cursor, first, count, run_cursor + (count + 3) * 4});
    for (uint32_t word = 0; word < count; ++word) {
      const uint32_t slot = first - 0x4000 + word;
      result.values[slot] = LoadGuestU32(base, cursor + 4 + word * 4);
      result.written[slot] = true;
    }
    cursor += (count + 1) * 4;
    run_cursor = cursor - 4;
  }
  Require(!result.runs.empty() && run_cursor == last_cursor,
          "Constant stream did not end at the original cursor");
  return result;
}

constexpr std::array<PPCRegister PPCContext::*, 18> kNonvolatile = {
    &PPCContext::r14, &PPCContext::r15, &PPCContext::r16, &PPCContext::r17, &PPCContext::r18,
    &PPCContext::r19, &PPCContext::r20, &PPCContext::r21, &PPCContext::r22, &PPCContext::r23,
    &PPCContext::r24, &PPCContext::r25, &PPCContext::r26, &PPCContext::r27, &PPCContext::r28,
    &PPCContext::r29, &PPCContext::r30, &PPCContext::r31};

PPCContext Context(uint64_t mask, uint32_t stage, uint32_t seed) {
  PPCContext ctx{};
  ctx.r1.u64 = kStack;
  ctx.r3.u64 = 0xA5A5000000000000ull | kDevice;
  ctx.r4.u64 = mask;
  ctx.r5.u64 = (seed & 1 ? 0x5A5A000000000000ull : 0) | stage;
  ctx.r6.u64 = 0xA5A5000000000000ull | kShadow;
  ctx.r7.u64 = 0xF00DFACE11223344ull;
  ctx.lr = 0x82234560;
  for (size_t i = 0; i < kNonvolatile.size(); ++i)
    (ctx.*kNonvolatile[i]).u64 = 0xDEADBEEF00000000ull | (seed * 31 + i);
  return ctx;
}

void CheckAbi(const PPCContext& actual, const PPCContext& expected) {
  Require(actual.r1.u64 == expected.r1.u64 && actual.r3.u64 == expected.r3.u64 &&
              actual.r4.u64 == expected.r4.u64 && actual.r5.u64 == expected.r5.u64 &&
              actual.r6.u64 == expected.r6.u64 && actual.r7.u64 == expected.r7.u64 &&
              actual.lr == expected.lr,
          "Native constants changed a visible input, consumed cursor, stack or return register");
  for (auto field : kNonvolatile)
    Require((actual.*field).u64 == (expected.*field).u64,
            "Native constants changed a nonvolatile register");
}

void Prepare(uint32_t cursor, uint32_t pattern, uint32_t limit = kOutput + 0x2000) {
  constexpr std::array<uint32_t, 16> raw = {
      0,          0x80000000, 0x7F800000, 0xFF800000, 0x7FC00001, 0x7FA12345,
      0xFFC54321, 0x00000001, 0x007FFFFF, 0x00800000, 0x3F800000, 0xBF800000,
      0xFFFFFFFF, 0x12345678, 0x87654321, 0x7FFFFFFF};
  for (uint8_t* base : {reference_memory.data(), native_memory.data()}) {
    std::memset(base + kOutput - 32, 0xCD, kOutputSpan);
    StoreGuestU32(base, kDevice + 48, cursor);
    StoreGuestU32(base, kDevice + 52, limit);
    for (uint32_t i = 0; i < 1024; ++i) {
      uint32_t value = raw[i & 15];
      if (pattern == 1)
        value = i * 0x91E10DA5u + 0xA53B91C7u;
      else if (pattern == 2)
        value = i & 1 ? 0xFFFFFFFFu : 0u;
      else if (pattern == 3)
        value = raw[(i * 7 + 3) & 15] ^ (i << 3);
      StoreGuestU32(base, kShadow + i * 4, value);
    }
  }
}

uint64_t CodecChecks() {
  uint64_t checks = 0;
  for (uint32_t stage : {0x4000u, 0x4400u})
    for (uint32_t group = 0; group < 64; ++group)
      for (uint32_t groups = 1; groups <= 64 - group; ++groups) {
        uint32_t header;
        Require(codec::EncodeHeader(stage + group * 16, groups * 16, header),
                "A valid constant range could not be encoded");
        codec::ConstantRange range;
        Require(codec::DecodeHeader(header, groups * 16, range) &&
                    range.first_register == stage + group * 16 && range.register_count == groups * 16,
                "Constant header did not round-trip");
        for (uint32_t changed : {header | 1u, header | 0x8000u, header ^ 0x100u,
                                 header ^ 0x40000000u})
          Require(!codec::DecodeHeader(changed, groups * 16, range),
                  "Malformed native constants header was accepted");
        Require(!codec::DecodeHeader(header, groups * 16 + 1, range),
                "A different payload size was accepted");
        ++checks;
      }
  for (uint32_t first : {0x3FF0u, 0x4001u, 0x4401u, 0x4800u, UINT32_MAX}) {
    uint32_t header = 0x12345678;
    Require(!codec::EncodeHeader(first, 16, header) && header == 0x12345678,
            "An invalid first register was accepted or changed the output");
    ++checks;
  }
  for (uint32_t count : {0u, 1u, 15u, 17u, 1025u, UINT32_MAX}) {
    uint32_t header;
    Require(!codec::EncodeHeader(0x4000, count, header), "Invalid constant count was accepted");
    ++checks;
  }
  uint32_t header;
  Require(!codec::EncodeHeader(0x43F0, 32, header) && !codec::EncodeHeader(0x47F0, 32, header),
          "A constant range crossed a shader stage boundary");
  return checks + 2;
}

void RejectUnchanged(PPCContext ctx) {
  const Memory before = native_memory;
  std::array<uint8_t, sizeof(PPCContext)> context_before;
  std::memcpy(context_before.data(), &ctx, sizeof(ctx));
  Require(!simpsons::native_gpu_constants::WriteConstants(ctx, native_memory.data()),
          "Unsupported constants unexpectedly used the native writer");
  Require(!std::memcmp(before.data(), native_memory.data(), before.size()) &&
              !std::memcmp(context_before.data(), &ctx, sizeof(ctx)),
          "Failed preflight changed memory or context before original fallback");
}

uint64_t RejectionChecks() {
  uint64_t checks = 0;
  Prepare(kOutput, 0);
  const auto valid = Context(0x8000000000000000ull, 0x4000, 0);
  auto ctx = valid;
  ctx.r4.u64 = 0;
  RejectUnchanged(ctx); ++checks;
  for (uint32_t stage : {0x3FF0u, 0x4001u, 0x4800u, UINT32_MAX}) {
    ctx = valid; ctx.r5.u64 = stage;
    RejectUnchanged(ctx); ++checks;
  }
  for (uint32_t shadow : {kShadow + 1, kShadow + 4, 0xFFFFFFF0u, kOutput + 16}) {
    ctx = valid; ctx.r6.u64 = shadow;
    RejectUnchanged(ctx); ++checks;
  }
  ctx = valid; ctx.r3.u64 = 0xFFFFFFE0u;
  RejectUnchanged(ctx); ++checks;
  StoreGuestU32(native_memory.data(), kDevice + 48, kOutput + 1);
  RejectUnchanged(valid); ++checks;
  StoreGuestU32(native_memory.data(), kDevice + 48, kDevice + 28);
  RejectUnchanged(valid); ++checks;
  StoreGuestU32(native_memory.data(), kDevice + 48, 0xFFFFFFFCu);
  RejectUnchanged(valid); ++checks;

  // Aliased pointers need no sparse mappings here: alias preflight must reject
  // before touching the source or destination payload at either high address.
  constexpr std::array aliases = {
      std::pair{0xA001000Cu, 0xC0010000u}, std::pair{0xC001000Cu, 0xA0010000u},
      std::pair{0xE000F00Cu, 0xA0010000u}, std::pair{0xA001000Cu, 0xE000F000u},
      std::pair{0x7F01000Cu, 0xC0010000u}, std::pair{0x8001000Cu, 0x90010000u}};
  StoreGuestU32(native_memory.data(), kDevice + 52, UINT32_MAX);
  for (const auto& [cursor, shadow] : aliases) {
    StoreGuestU32(native_memory.data(), kDevice + 48, cursor);
    ctx = valid; ctx.r6.u64 = shadow;
    RejectUnchanged(ctx); ++checks;
  }
  return checks;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc > 2 || (argc == 2 && !std::freopen(argv[1], "w", stdout)))
    return 2;
  try {
    const uint64_t codec_checks = CodecChecks();
    std::memset(reference_memory.data(), 0xCD, reference_memory.size());
    std::memset(native_memory.data(), 0xCD, native_memory.size());
    std::vector<uint64_t> masks;
    for (uint32_t first = 0; first < 64; ++first)
      for (uint32_t groups = 1; groups <= 64 - first; ++groups) {
        uint64_t mask = groups == 64 ? UINT64_MAX : (UINT64_MAX << (64 - groups));
        masks.push_back(mask >> first);
      }
    masks.push_back(0xAAAAAAAAAAAAAAAAull);
    masks.push_back(0x5555555555555555ull);
    uint64_t random = 0x9123A5FE81B753DFull;
    for (uint32_t i = 0; i < 256; ++i) {
      random ^= random << 13; random ^= random >> 7; random ^= random << 17;
      masks.push_back(random ? random : 1);
    }
    uint64_t cases = 0, patched = 0, copied = 0, rejected = 0, padding_words = 0;
    uint64_t native_calls_to_original = 0;
    for (uint64_t mask : masks)
      for (uint32_t stage : {0x4000u, 0x4400u})
        for (uint32_t alignment : {0u, 4u, 8u, 12u})
          for (uint32_t pattern = 0; pattern < 4; ++pattern) {
            const uint32_t cursor = kOutput + alignment;
            Prepare(cursor, pattern);
            const auto before = Context(mask, stage, uint32_t(cases));
            auto expected_ctx = before;
            reference_sub_8245EC68(expected_ctx, reference_memory.data());
            const uint32_t end = LoadGuestU32(reference_memory.data(), kDevice + 48);
            const auto expected = Parse(reference_memory.data(), cursor, end, false);
            auto actual_ctx = before;
            const uint64_t calls_before = reference_writer_calls;
            Require(simpsons::native_gpu_constants::WriteConstants(actual_ctx, native_memory.data()),
                    "A bounded valid constants call unexpectedly fell back");
            native_calls_to_original += reference_writer_calls - calls_before;
            CheckAbi(actual_ctx, expected_ctx);
            Require(LoadGuestU32(native_memory.data(), kDevice + 48) == end,
                    "Native constants changed the device cursor");
            const auto actual = Parse(native_memory.data(), cursor, end, true);
            Require(actual.values == expected.values && actual.written == expected.written &&
                        actual.padding == expected.padding && actual.runs.size() == expected.runs.size(),
                    "Native constant registers, writes or padding differ from the generated body");
            padding_words += actual.padding;
            std::vector<bool> headers((end - cursor) / 4, false);
            for (size_t i = 0; i < actual.runs.size(); ++i) {
              const auto& a = actual.runs[i];
              const auto& e = expected.runs[i];
              Require(a.header_address == e.header_address && a.first_register == e.first_register &&
                          a.register_count == e.register_count,
                      "Native constants moved an original header or operand range");
              headers[(a.header_address - cursor - 4) / 4] = true;
            }
            for (uint32_t offset = 0; offset < headers.size(); ++offset)
              if (!headers[offset])
                Require(LoadGuestU32(native_memory.data(), cursor + 4 + offset * 4) ==
                            LoadGuestU32(reference_memory.data(), cursor + 4 + offset * 4),
                        "Native constants changed a non-header command word");
            Require(!std::memcmp(reference_memory.data() + kDevice,
                                 native_memory.data() + kDevice, 64) &&
                        !std::memcmp(reference_memory.data() + kShadow,
                                     native_memory.data() + kShadow, 4096) &&
                        !std::memcmp(reference_memory.data() + cursor - 16,
                                     native_memory.data() + cursor - 16, 20) &&
                        !std::memcmp(reference_memory.data() + end + 4,
                                     native_memory.data() + end + 4, 32),
                    "Native constants changed source, device fields or command-buffer guards");

            if (!(cases & 31)) {
              const uint32_t copy_cursor = kCopy + alignment;
              std::memcpy(native_memory.data() + copy_cursor + 4,
                          native_memory.data() + cursor + 4, end - cursor);
              const auto replay = Parse(native_memory.data(), copy_cursor,
                                        copy_cursor + end - cursor, true);
              Require(replay.values == expected.values && replay.written == expected.written,
                      "Copied native constants depend on their original buffer address");
              ++copied;
              for (const auto& run : expected.runs)
                for (uint32_t word : {0u, run.register_count / 2, run.register_count - 1}) {
                  const uint32_t at = run.header_address + 4 + word * 4;
                  const uint32_t original = LoadGuestU32(reference_memory.data(), at);
                  const uint32_t replacement = 0x7FA00001u ^ (word * 0x91E10DA5u);
                  StoreGuestU32(reference_memory.data(), at, replacement);
                  StoreGuestU32(native_memory.data(), at, replacement);
                  const auto old_patched = Parse(reference_memory.data(), cursor, end, false);
                  const auto new_patched = Parse(native_memory.data(), cursor, end, true);
                  Require(old_patched.values == new_patched.values &&
                              old_patched.written == new_patched.written,
                          "A patched operand changed meaning between the two encodings");
                  StoreGuestU32(reference_memory.data(), at, original);
                  StoreGuestU32(native_memory.data(), at, original);
                  ++patched;
                }

              const uint32_t threshold = expected.runs.back().capacity_threshold;
              for (uint32_t limit : {threshold - 1, threshold}) {
                Prepare(cursor, pattern, limit);
                auto reference_ctx = before;
                bool slow = false;
                try { reference_sub_8245EC68(reference_ctx, reference_memory.data()); }
                catch (ReferenceSlowPath&) { slow = true; }
                Require(slow, "Capacity-boundary reference unexpectedly stayed on its fast path");
                RejectUnchanged(before);
                ++rejected;
              }
              Prepare(cursor, pattern, threshold + 1);
              auto reference_ctx = before;
              reference_sub_8245EC68(reference_ctx, reference_memory.data());
              auto native_ctx = before;
              Require(simpsons::native_gpu_constants::WriteConstants(native_ctx, native_memory.data()),
                      "The exact first sufficient capacity was rejected");
              CheckAbi(native_ctx, reference_ctx);
            }
            ++cases;
          }
    rejected += RejectionChecks();
    Require(native_calls_to_original == 0, "Native constants executed the original writer");
    std::printf("{\"passed\":true,\"cases\":%llu,\"codec_ranges\":%llu,"
                "\"patched_operand_replays\":%llu,\"copied_buffer_replays\":%llu,"
                "\"unchanged_fallback_checks\":%llu,\"padding_words\":%llu,"
                "\"reference_writer_calls\":%llu,\"reference_slow_path_observations\":%llu,"
                "\"native_original_writer_calls\":0,\"scope\":\"generated-body CPU constant "
                "writer, operand replay and ABI; not GPU or full-game qualification\"}\n",
                (unsigned long long)cases, (unsigned long long)codec_checks,
                (unsigned long long)patched, (unsigned long long)copied,
                (unsigned long long)rejected, (unsigned long long)padding_words,
                (unsigned long long)reference_writer_calls, (unsigned long long)reference_slow_calls);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
  } catch (const ReferenceSlowPath&) {
    std::fprintf(stderr, "FAIL: reference unexpectedly required a growing command segment\n");
    return 1;
  }
}
