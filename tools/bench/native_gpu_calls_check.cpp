// The reference bodies are extracted from the current generated game by
// check_gpu_calls.py. They are never copied into the repository as a second
// implementation, and this check never launches or installs the game.
#include "simpsons_init.h"
#include "native_gpu_calls.h"

#include <rex/graphics/registers.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <vector>

static uint64_t reference_writer_calls = 0;
#include "native_gpu_reference.inc"

using rex::graphics::native_gpu_calls::DrawCall;
using simpsons::LoadGuestU32;
using simpsons::StoreGuestU32;
using namespace rex::graphics;

namespace {
void Require(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}

DrawCall ReadReference(const uint8_t* base, uint32_t start, uint32_t end) {
  std::array<uint32_t, 0x5003> state{};
  DrawCall result{};
  uint32_t draws = 0, coherency_waits = 0;
  for (uint32_t cursor = start; cursor < end;) {
    uint32_t header = LoadGuestU32(base, cursor);
    cursor += 4;
    uint32_t count = ((header >> 16) & 0x3FFF) + 1;
    Require(count <= (end - cursor) / 4, "Reference command exceeds its written buffer");
    if ((header >> 30) == 0) {
      uint32_t first = header & 0x7FFF;
      Require(!(header & 0x8000) && first < state.size() && count <= state.size() - first,
              "Unexpected register writer in the reference draw");
      for (uint32_t i = 0; i < count; ++i)
        state[first + i] = LoadGuestU32(base, cursor + i * 4);
    } else if ((header >> 30) == 3) {
      uint32_t opcode = (header >> 8) & 0x7F;
      if (opcode == 0x3C) {
        Require(count == 5 && LoadGuestU32(base, cursor) == 3 &&
                    LoadGuestU32(base, cursor + 4) == XE_GPU_REG_COHER_STATUS_HOST &&
                    LoadGuestU32(base, cursor + 8) == 0 &&
                    LoadGuestU32(base, cursor + 12) == 0x80000000 &&
                    LoadGuestU32(base, cursor + 16) == 8,
                "The reference changed its coherency wait contract");
        ++coherency_waits;
      } else if (opcode == 0x22 || opcode == 0x36) {
        const uint32_t offset = opcode == 0x22 ? 4 : 0;
        Require(count == (opcode == 0x22 ? 4u : 1u), "Unexpected reference draw size");
        if (offset)
          result.viz_query_condition = LoadGuestU32(base, cursor);
        result.initiator = LoadGuestU32(base, cursor + offset);
        if (offset) {
          result.index_base = LoadGuestU32(base, cursor + offset + 4);
          result.index_size = LoadGuestU32(base, cursor + offset + 8);
        }
        ++draws;
      } else {
        throw std::runtime_error("A reference handler gained an unconverted GPU operation");
      }
    } else {
      throw std::runtime_error("Unexpected packet type in the reference draw");
    }
    cursor += count * 4;
  }
  Require(draws == 1 && coherency_waits == 1, "Reference did not submit one synchronized draw");
  std::copy_n(state.begin() + 0x48BA, result.fetch.size(), result.fetch.begin());
  result.coherent_size = state[XE_GPU_REG_COHER_SIZE_HOST];
  result.coherent_base = state[XE_GPU_REG_COHER_BASE_HOST];
  result.coherent_status = state[XE_GPU_REG_COHER_STATUS_HOST];
  std::copy_n(state.begin() + 0x5000, result.constant_flush.size(), result.constant_flush.begin());
  return result;
}

auto Values(const DrawCall& call) {
  return std::tie(call.fetch, call.coherent_size, call.coherent_base, call.coherent_status,
                  call.constant_flush, call.viz_query_condition, call.initiator, call.index_base,
                  call.index_size);
}

constexpr std::array<PPCRegister PPCContext::*, 18> kNonvolatile = {
    &PPCContext::r14, &PPCContext::r15, &PPCContext::r16, &PPCContext::r17, &PPCContext::r18,
    &PPCContext::r19, &PPCContext::r20, &PPCContext::r21, &PPCContext::r22, &PPCContext::r23,
    &PPCContext::r24, &PPCContext::r25, &PPCContext::r26, &PPCContext::r27, &PPCContext::r28,
    &PPCContext::r29, &PPCContext::r30, &PPCContext::r31};
}  // namespace

int main(int argc, char** argv) {
  if (argc > 2 || (argc == 2 && !std::freopen(argv[1], "w", stdout)))
    return 2;
  try {
    constexpr uint32_t builder = 0x1000, device = 0x2000, output = 0x80000;
    constexpr size_t memory_size = 1024 * 1024;
    alignas(4096) static std::array<uint8_t, memory_size> reference, native;
    std::memset(reference.data(), 0xCD, memory_size);
    std::memset(native.data(), 0xCD, memory_size);
    struct Handler {
      uint32_t address;
      void (*reference)(PPCContext&, uint8_t*);
    };
    constexpr std::array handlers = {
        Handler{0x824626B8, reference_sub_824626B8}, Handler{0x82462810, reference_sub_82462810},
        Handler{0x82464590, reference_sub_82464590}, Handler{0x824646E8, reference_sub_824646E8}};
    constexpr std::array counts = {0u, 1u, 2u, 65535u, 65536u, 0xFFFFFFu, 0x1000000u, UINT32_MAX};
    constexpr std::array addresses = {0u,          1u,          2u,          3u,
                                      0x7F0FFFFFu, 0x7F100000u, 0x86FFFFFFu, 0x87000000u,
                                      0xA0000002u, 0xE0000000u, 0xFFFFF001u, UINT32_MAX};
    uint64_t cases = 0, patched_cases = 0;
    for (const auto& handler : handlers)
      for (uint32_t primitive = 0; primitive < 64; ++primitive)
        for (uint32_t count : counts)
          for (uint32_t address : addresses)
            for (uint32_t format : {0u, 6u, UINT32_MAX}) {
              PPCContext ctx{};
              ctx.r1.u64 = 0x40000;
              ctx.r3.u64 = 0xA5A5000000000000ull | builder;
              ctx.r4.u64 = primitive | 0xFFF00040u;
              ctx.r5.u64 = count;
              ctx.r6.u64 = address;
              ctx.r7.u64 = format;
              ctx.r8.u64 = address ^ 0xA5001000u;
              ctx.lr = 0x82234560;
              for (size_t i = 0; i < kNonvolatile.size(); ++i)
                (ctx.*kNonvolatile[i]).u64 = 0xDEADBEEF00000000ull | (cases * 31 + i);
              auto before = ctx;
              for (uint8_t* base : {reference.data(), native.data()}) {
                std::memset(base + output - 16, 0xCD, 160);
                StoreGuestU32(base, builder, device);
                StoreGuestU32(base, builder + 16, output);
                StoreGuestU32(base, device + 20, uint32_t(cases * 4097) ^ 0xF0000010u);
              }
              handler.reference(ctx, reference.data());
              uint32_t end = LoadGuestU32(reference.data(), builder + 16);
              auto expected = ReadReference(reference.data(), output, end);
              uint64_t original_calls = reference_writer_calls;
              Require(simpsons::native_gpu_calls::WriteSimpleDraw(
                          native.data(), handler.address, builder, uint32_t(before.r4.u64), count,
                          address, format, uint32_t(before.r8.u64)),
                      "A supported native draw unexpectedly fell back");
              Require(reference_writer_calls == original_calls,
                      "Native draw executed the original packet writer");
              Require(LoadGuestU32(native.data(), builder + 16) == end,
                      "Native draw changed the command-builder byte advance");
              std::array<uint32_t, 27> payload{};
              uint32_t header = LoadGuestU32(native.data(), output);
              Require(((header >> 8) & 0x7F) == native_gpu_calls::kOpcode && !(header & 1),
                      "Native draw generated an original or predicated draw packet");
              uint32_t payload_size = ((header >> 16) & 0x3FFF) + 1;
              Require((payload_size + 1) * 4 == end - output,
                      "Native call transport did not own its full reserved span");
              for (uint32_t i = 0; i < payload_size; ++i)
                payload[i] = LoadGuestU32(native.data(), output + 4 + i * 4);
              DrawCall actual{};
              Require(native_gpu_calls::DecodeDraw({payload.data(), payload_size}, actual),
                      "Native draw arguments could not be decoded");
              Require(Values(actual) == Values(expected),
                      "Native arguments differ from the original draw/state/coherency writes");
              if ((cases & 63) == 0) {
                // Recorded command buffers can patch operands before replay.
                // Those offsets must remain meaningful in both encodings.
                for (uint32_t word : {1u, 4u, 5u, 6u, 8u, 10u, 11u, 19u, 20u, 21u, 23u}) {
                  uint32_t original = LoadGuestU32(reference.data(), output + word * 4);
                  uint32_t changed = original ^ (uint32_t(cases + word) * 0x91E10DA5u);
                  if (word == 23 && payload_size == 23)
                    changed = original ^ 0x1234003Fu;  // Keep the draw's source selection.
                  for (uint8_t* base : {reference.data(), native.data()})
                    StoreGuestU32(base, output + word * 4, changed);
                  for (uint32_t i = 0; i < payload_size; ++i)
                    payload[i] = LoadGuestU32(native.data(), output + 4 + i * 4);
                  Require(native_gpu_calls::DecodeDraw({payload.data(), payload_size}, actual),
                          "An operand patch broke native command-buffer replay");
                  auto patched = ReadReference(reference.data(), output, end);
                  Require(Values(actual) == Values(patched),
                          "An operand patch changed meaning between the original and native calls");
                  for (uint8_t* base : {reference.data(), native.data()})
                    StoreGuestU32(base, output + word * 4, original);
                  ++patched_cases;
                }
                if (payload_size == 26) {
                  for (uint32_t word : {24u, 25u, 26u}) {
                    uint32_t original = LoadGuestU32(reference.data(), output + word * 4);
                    uint32_t changed = original ^ (word == 24 ? 0x1234003Fu : 0xA53B91C7u);
                    for (uint8_t* base : {reference.data(), native.data()})
                      StoreGuestU32(base, output + word * 4, changed);
                    for (uint32_t i = 0; i < payload_size; ++i)
                      payload[i] = LoadGuestU32(native.data(), output + 4 + i * 4);
                    Require(native_gpu_calls::DecodeDraw({payload.data(), payload_size}, actual),
                            "An index operand patch broke native replay");
                    Require(Values(actual) == Values(ReadReference(reference.data(), output, end)),
                            "An index operand patch changed its native meaning");
                    for (uint8_t* base : {reference.data(), native.data()})
                      StoreGuestU32(base, output + word * 4, original);
                    ++patched_cases;
                  }
                }
              }
              Require(
                  ctx.r1.u64 == before.r1.u64 && ctx.r3.u64 == before.r3.u64 && ctx.lr == before.lr,
                  "Reference draw changed its stack, return or link contract");
              for (auto field : kNonvolatile)
                Require((ctx.*field).u64 == (before.*field).u64,
                        "Reference draw changed a nonvolatile register");
              Require(!std::memcmp(reference.data() + builder, native.data() + builder, 32) &&
                          !std::memcmp(reference.data() + device, native.data() + device, 32) &&
                          !std::memcmp(native.data() + output - 16, reference.data() + output - 16,
                                       16) &&
                          !std::memcmp(native.data() + end, reference.data() + end, 16),
                      "Native draw changed non-command state or command-buffer guards");
              ++cases;
            }
    std::array<uint32_t, 23> malformed{};
    DrawCall call{};
    Require(!native_gpu_calls::DecodeDraw(malformed, call), "Invalid native version was accepted");
    malformed[6] = native_gpu_calls::kDrawVersion;
    malformed[12] = 3;
    malformed[13] = 0xA31;
    malformed[15] = 0x80000000;
    malformed[16] = 8;
    malformed[22] = 0x80;
    Require(native_gpu_calls::DecodeDraw(malformed, call), "Valid native call was rejected");
    malformed[21] = 1;
    Require(!native_gpu_calls::DecodeDraw(malformed, call), "Invalid reserved data was accepted");
    malformed[21] = 0;
    malformed[22] = 0x40;
    Require(!native_gpu_calls::DecodeDraw(malformed, call), "Immediate indices were accepted");
    Require(!simpsons::native_gpu_calls::WriteSimpleDraw(nullptr, 0, 0, 0, 0, 0, 0, 0),
            "An unconverted call reached the native writer");
    StoreGuestU32(native.data(), builder + 16, builder);
    Require(!simpsons::native_gpu_calls::WriteSimpleDraw(native.data(), handlers[0].address,
                                                         builder, 0, 0, 0, 0, 0),
            "An overlapping builder was accepted");
    uint32_t alias_fallbacks = 0;
    // The device address is deliberately not mapped in this small check. An
    // alias must be rejected before reading it or writing any command bytes.
    // These are different virtual addresses of the same backing bytes, plus
    // ranges crossing a view boundary; a numeric overlap check is insufficient.
    for (auto [device_alias, write_alias] :
         {std::pair{0xA0002000u, 0xC0002000u}, std::pair{0xA0002000u, 0xE0001000u},
          std::pair{0xC0002000u, 0xA0002000u}, std::pair{0x7F002000u, 0xA0002000u},
          std::pair{0x90002000u, 0x80002000u}, std::pair{0x80002000u, 0x90002000u},
          std::pair{0xC0002000u, 0x7F002000u}, std::pair{device, 0x7FFFFFF0u},
          std::pair{0x8FFFFFF0u, output}, std::pair{0xFFFFFFF0u, output}}) {
      StoreGuestU32(native.data(), builder, device_alias);
      StoreGuestU32(native.data(), builder + 16, write_alias);
      const std::vector<uint8_t> unchanged(native.begin(), native.end());
      Require(!simpsons::native_gpu_calls::WriteSimpleDraw(
                  native.data(), handlers[0].address, builder, 0, 0, 0, 0, 0) &&
                  std::equal(native.begin(), native.end(), unchanged.begin()),
              "An aliased or crossing draw modified memory before fallback");
      ++alias_fallbacks;
    }
    Require(!simpsons::native_gpu_calls::WriteSimpleDraw(
                native.data(), handlers[0].address, 0xFFFFFFF0u, 0, 0, 0, 0, 0) &&
                !simpsons::native_gpu_calls::WriteSimpleDraw(
                    nullptr, handlers[0].address, builder, 0, 0, 0, 0, 0),
            "Invalid builder storage reached the native writer");
    std::printf(
        "{\"passed\":true,\"handlers\":4,\"cases\":%llu,\"patched_replay_cases\":%llu,"
        "\"alias_fallback_checks\":%u,\"reference_"
        "writer_calls\":%llu,"
        "\"native_original_writer_calls\":0,\"scope\":\"draw-builder argument oracle; "
        "not backend or full-game qualification\"}\n",
        static_cast<unsigned long long>(cases), static_cast<unsigned long long>(patched_cases),
        alias_fallbacks,
        static_cast<unsigned long long>(reference_writer_calls));
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
  }
}
