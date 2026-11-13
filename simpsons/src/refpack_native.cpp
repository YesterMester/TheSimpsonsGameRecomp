// Asset streaming uses sub_8281E5B0 to unpack RefPack into the same allocation.
// Keep its forward-copy and guest-address overlap rules. The whole-buffer
// path preflights commands; the asynchronous path retains a bounded rollback
// buffer, so rejected input reaches the original decoder unchanged.
#include "refpack_native.h"
#include "refpack_state.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <new>
#include <span>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

REXCVAR_DEFINE_BOOL(native_refpack_decode, true, "Game",
                    "Use the checked native asset decompressor");
REXCVAR_DEFINE_BOOL(native_refpack_log, false, "Game",
                    "Development: log native asset decompression totals");

REX_EXTERN(__imp__sub_8281E5B0);
REX_EXTERN(sub_8281E5B0);
REX_EXTERN(__imp__sub_8281E9B0);
REX_EXTERN(sub_8281E9B0);
REX_EXTERN(__imp__sub_8281E8C0);
REX_EXTERN(sub_8281E8C0);

namespace {
constexpr size_t kMaximum = 512 * 1024 * 1024;
std::atomic<uint64_t> g_decodes{0}, g_bytes{0}, g_microseconds{0}, g_fallbacks{0},
    g_legacy_bytes{0}, g_legacy_microseconds{0};
std::mutex g_stream_mutex;
simpsons::native_refpack::StreamOwners g_streams;

bool OneHostRange(uint32_t address, uint64_t size) {
  const uint64_t end = uint64_t(address) + size;
  return end <= 0x100000000ull && (address >= 0xE0000000u || end <= 0xE0000000ull);
}

// QueryRegionInfo counts whole pages starting at the page containing address,
// while its base_address is the unaligned query. Subtract that first-page
// offset rather than treating region_size as bytes remaining at address.
size_t Available(rex::memory::Memory& memory, uint32_t address, uint32_t access) {
  auto* heap = memory.LookupHeap(address);
  rex::memory::HeapAllocationInfo info{};
  if (!heap || !heap->QueryRegionInfo(address, &info) ||
      !(info.state & rex::memory::kMemoryAllocationCommit) || (info.protect & access) != access)
    return 0;
  const auto offset = address & (heap->page_size() - 1);
  if (offset >= info.region_size)
    return 0;
  const uint64_t allocation_end = uint64_t(info.allocation_base) + info.allocation_size;
  const uint64_t region_end = uint64_t(address) + info.region_size - offset;
  const uint64_t end = std::min(allocation_end, region_end);
  if (end <= address)
    return 0;
  return OneHostRange(address, end - address) ? size_t(end - address) : 0;
}

bool Overlaps(uint32_t a, size_t a_size, uint32_t b, size_t b_size) {
  return uint64_t(a) < uint64_t(b) + b_size && uint64_t(b) < uint64_t(a) + a_size;
}

void Totals(bool native, uint64_t bytes, uint64_t micros) {
  if (!REXCVAR_GET(native_refpack_log))
    return;
  const auto calls = native ? g_decodes.fetch_add(1, std::memory_order_relaxed) + 1
                            : g_fallbacks.fetch_add(1, std::memory_order_relaxed) + 1;
  if (native) {
    g_bytes.fetch_add(bytes, std::memory_order_relaxed);
    g_microseconds.fetch_add(micros, std::memory_order_relaxed);
  } else {
    g_legacy_bytes.fetch_add(bytes, std::memory_order_relaxed);
    g_legacy_microseconds.fetch_add(micros, std::memory_order_relaxed);
  }
  if (calls <= 4 || calls % 128 == 0)
    REXLOG_INFO(
        "[refpack-native] {} decodes, {:.1f} MiB, {:.2f} ms, {} legacy calls ({:.1f} MiB, {:.2f} "
        "ms)",
        g_decodes.load(std::memory_order_relaxed),
        g_bytes.load(std::memory_order_relaxed) / 1048576.0,
        g_microseconds.load(std::memory_order_relaxed) / 1000.0,
        g_fallbacks.load(std::memory_order_relaxed),
        g_legacy_bytes.load(std::memory_order_relaxed) / 1048576.0,
        g_legacy_microseconds.load(std::memory_order_relaxed) / 1000.0);
}

bool Decode(PPCContext& ctx, uint8_t* base, uint32_t& decoded_bytes) {
  auto* kernel = rex::system::kernel_state();
  if (!kernel || !kernel->memory())
    return false;
  auto& memory = *kernel->memory();
  const uint32_t source = ctx.r3.u32, destination = ctx.r4.u32;
  const uint32_t result = ctx.r5.u32, stack = ctx.r1.u32;
  if (!source || !destination || stack < 8 ||
      memory.LookupHeap(source) != memory.LookupHeap(destination))
    return false;
  const size_t capacity = Available(
      memory, destination, rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite);
  if (!capacity)
    return false;
  size_t input_size = 0;
  if (source >= destination && uint64_t(source) - destination < capacity)
    input_size = capacity - size_t(source - destination);
  else
    input_size = Available(memory, source, rex::memory::kMemoryProtectRead);
  if (!input_size)
    return false;
  const std::span<const uint8_t> packed(simpsons::GuestToHost(base, source),
                                        std::min(input_size, kMaximum));
  const auto header = simpsons::refpack::Header(packed);
  if (!header || header.size > kMaximum || header.size > capacity ||
      !OneHostRange(destination, header.size))
    return false;
  if (REXCVAR_GET(native_refpack_log) && g_decodes.load(std::memory_order_relaxed) < 4)
    REXLOG_INFO("[refpack-native] resource {} bytes, input offset {}, mapped capacity {} KiB",
                header.size, int64_t(source) - destination, capacity / 1024);
  const std::span<uint8_t> output(simpsons::GuestToHost(base, destination), header.size);
  const auto checked = simpsons::refpack::Validate(packed, output, source, destination);
  if (!checked)
    return false;
  if (Available(memory, stack - 8, rex::memory::kMemoryProtectWrite) < 8 ||
      Overlaps(stack - 8, 8, source, checked.consumed) ||
      Overlaps(stack - 8, 8, destination, header.size) ||
      (result &&
       (Available(memory, result, rex::memory::kMemoryProtectWrite) < 4 ||
        Overlaps(result, 4, stack - 8, 8) || Overlaps(result, 4, source, checked.consumed) ||
        Overlaps(result, 4, destination, header.size))))
    return false;
  // Match the leaf routine's saved nonvolatile register, including its stack
  // spill. Its remaining changed registers/condition fields are volatile.
  const auto decoded =
      simpsons::native_refpack::Commit(ctx, base, packed.first(checked.consumed), output);
  const bool ok = bool(decoded);
  decoded_bytes = ok ? header.size : 0;
  if (!ok)
    REXLOG_ERROR("[refpack-native] Verified resource changed during decompression");
  return true;
}

bool Step(PPCContext& ctx, uint8_t* base, simpsons::native_refpack::StreamOwner& owner,
          uint32_t& decoded_bytes, uint32_t& previous_output) {
  auto* kernel = rex::system::kernel_state();
  if (!kernel || !kernel->memory() || ctx.r1.u32 < 32)
    return false;
  auto& memory = *kernel->memory();
  constexpr auto rw = rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite;
  const uint32_t input_cursor = ctx.r3.u32, output_cursor = ctx.r4.u32;
  const uint32_t budget = ctx.r5.u32, stack = ctx.r1.u32 - 32;
  // Large chunks amortize heap validation and the reusable rollback buffer.
  // Unusual huge requests keep the original routine without a large host
  // allocation on memory-constrained systems.
  if (!input_cursor || !output_cursor || Available(memory, input_cursor, rw) < 4 ||
      Available(memory, output_cursor, rw) < 4 || Overlaps(input_cursor, 4, output_cursor, 4))
    return false;
  const uint32_t source = simpsons::LoadGuestU32(base, input_cursor);
  const uint32_t destination = simpsons::LoadGuestU32(base, output_cursor);
  previous_output = destination;
  {
    std::lock_guard lock(g_stream_mutex);
    owner = g_streams.Find(input_cursor, source, destination);
  }
  if (!owner.cursor || budget < 4096 || budget > 8 * 1024 * 1024 ||
      Available(memory, stack, rex::memory::kMemoryProtectWrite) < 28)
    return false;
  auto* heap = memory.LookupHeap(destination);
  rex::memory::HeapAllocationInfo allocation{};
  if (!owner.cursor || owner.source != source || owner.produced > owner.size || !source ||
      !destination || !heap || memory.LookupHeap(source) != heap ||
      !heap->QueryRegionInfo(destination, &allocation) || destination < allocation.allocation_base)
    return false;
  const size_t history = std::min<size_t>(131072, owner.produced);
  if (history > destination - allocation.allocation_base)
    return false;
  const uint32_t output_start = destination - uint32_t(history);
  const size_t capacity = Available(memory, output_start, rw);
  if (capacity < history || owner.size - owner.produced > capacity - history)
    return false;
  const size_t input_size = source >= output_start && uint64_t(source) - output_start < capacity
                                ? capacity - size_t(source - output_start)
                                : Available(memory, source, rex::memory::kMemoryProtectRead);
  if (!input_size)
    return false;
  const std::span<const uint8_t> input(simpsons::GuestToHost(base, source),
                                       std::min(input_size, kMaximum));
  // One command may overshoot the budget by at most 1,030 bytes. The mapping
  // check also covers its previous 128 KiB of reference history.
  const size_t changed_size = std::min<size_t>(owner.size - owner.produced, size_t(budget) + 1030);
  const size_t output_size = history + changed_size;
  const std::span<uint8_t> output(simpsons::GuestToHost(base, output_start), output_size);
  for (const auto address : {input_cursor, output_cursor, stack}) {
    const size_t size = address == stack ? 28 : 4;
    if (Overlaps(address, size, source, input.size()) ||
        Overlaps(address, size, output_start, output.size()))
      return false;
  }
  if (Overlaps(stack, 28, input_cursor, 4) || Overlaps(stack, 28, output_cursor, 4))
    return false;
  if (REXCVAR_GET(native_refpack_log) && g_decodes.load(std::memory_order_relaxed) < 4)
    REXLOG_INFO("[refpack-native] stream input {:08X}, output {:08X}, budget {}, history {}",
                source, destination, budget, history);
  thread_local std::vector<uint8_t> backup;
  try {
    if (backup.size() < changed_size)
      backup.resize(changed_size);
  } catch (const std::bad_alloc&) {
    return false;
  }
  simpsons::refpack::StepResult decoded;
  if (!simpsons::native_refpack::TryStep(ctx, base, input, output, history, source, destination,
                                         budget, backup, decoded))
    return false;
  decoded_bytes = uint32_t(decoded.produced);
  return true;
}
}  // namespace

void simpsons::native_refpack::InvalidateStreams() {
  std::lock_guard lock(g_stream_mutex);
  g_streams.Clear();
}

REX_FUNC(sub_8281E5B0) {
  uint32_t size = 0;
  const bool timing = REXCVAR_GET(native_refpack_log);
  const auto start =
      timing ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
  if (REXCVAR_GET(native_refpack_decode) && Decode(ctx, base, size)) {
    const auto micros = timing ? std::chrono::duration_cast<std::chrono::microseconds>(
                                     std::chrono::steady_clock::now() - start)
                                     .count()
                               : 0;
    Totals(true, size, uint64_t(micros));
    return;
  }
  __imp__sub_8281E5B0(ctx, base);
  const auto micros = timing ? std::chrono::duration_cast<std::chrono::microseconds>(
                                   std::chrono::steady_clock::now() - start)
                                   .count()
                             : 0;
  Totals(false, 0, uint64_t(micros));
}

REX_FUNC(sub_8281E9B0) {
  uint32_t size = 0;
  const bool timing = REXCVAR_GET(native_refpack_log);
  const auto start =
      timing ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
  const bool enabled = REXCVAR_GET(native_refpack_decode);
  const uint32_t input_cursor = ctx.r3.u32, cursor = ctx.r4.u32;
  uint32_t previous_output = 0;
  simpsons::native_refpack::StreamOwner owner;
  const bool native = enabled && Step(ctx, base, owner, size, previous_output);
  if (!native) {
    if (timing && !owner.cursor)
      previous_output = simpsons::LoadGuestU32(base, cursor);
    __imp__sub_8281E9B0(ctx, base);
    if (timing || owner.cursor)
      size = simpsons::LoadGuestU32(base, cursor) - previous_output;
  }
  if (owner.cursor) {
    const auto source = simpsons::LoadGuestU32(base, input_cursor);
    const auto destination = simpsons::LoadGuestU32(base, cursor);
    std::lock_guard lock(g_stream_mutex);
    g_streams.Advance(owner, source, destination, size, ctx.r3.u32 != 0);
  }
  if (timing) {
    const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - start)
                            .count();
    Totals(native, size, uint64_t(micros));
  }
}

REX_FUNC(sub_8281E8C0) {
  const uint32_t cursor = ctx.r3.u32;
  __imp__sub_8281E8C0(ctx, base);
  const auto source = simpsons::LoadGuestU32(base, cursor);
  std::lock_guard lock(g_stream_mutex);
  if (ctx.r3.u32 <= kMaximum)
    g_streams.Begin(cursor, source, ctx.r3.u32);
  else
    g_streams.Forget(cursor);
}
