// Apply fixed-size model variants before the game's normal relocation and
// binding. The original loader keeps ownership, dependency loading and teardown.
#include "modkit_models.h"
#include "guest_memory.h"

#include <atomic>
#include <cstring>
#include <mutex>
#include <set>

#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>

namespace modkit_models {
namespace {
std::atomic<bool> g_active{false};
std::mutex g_mutex;
rex::Runtime* g_runtime = nullptr;
std::shared_ptr<const simpsons::modkit::ModelVariants> g_variants;
std::set<std::string> g_errors;

bool Writable(uint32_t address, uint32_t size) {
  if (!address || !size || uint64_t(address) + size > 0x100000000ull)
    return false;
  // Check each region. A single committed first page does not prove the
  // following model bytes are accessible or writable.
  const uint64_t end = uint64_t(address) + size;
  for (uint64_t cursor = address; cursor < end;) {
    auto* heap = g_runtime->memory()->LookupHeap(uint32_t(cursor));
    rex::memory::HeapAllocationInfo info{};
    if (!heap || !heap->QueryRegionInfo(uint32_t(cursor), &info) ||
        !(info.state & rex::memory::kMemoryAllocationCommit) ||
        !(info.protect & rex::memory::kMemoryProtectRead) ||
        !(info.protect & rex::memory::kMemoryProtectWrite))
      return false;
    const auto offset = uint32_t(cursor) & (heap->page_size() - 1);
    if (info.region_size <= offset)
      return false;
    const uint64_t next = std::min({end, cursor + info.region_size - offset,
                                    uint64_t(info.allocation_base) + info.allocation_size});
    if (next <= cursor || (cursor < 0xE0000000ull && next > 0xE0000000ull))
      return false;
    cursor = next;
  }
  return true;
}
}  // namespace
void Apply(uint8_t* base, uint32_t address, uint32_t size) {
  if (!g_active.load(std::memory_order_acquire))
    return;
  std::lock_guard lock(g_mutex);
  if (!g_active.load(std::memory_order_relaxed) || size < 88 || size > 16 * 1024 * 1024 ||
      !Writable(address, size))
    return;
  try {
    auto* data = simpsons::GuestToHost(base, address);
    const auto edited = g_variants->Apply({data, size});
    if (!edited)
      return;
    // All validation and allocation have finished. Only these same-size bytes
    // are committed; an invalid document leaves the original buffer untouched.
    std::memcpy(data, edited->bytes.data(), edited->bytes.size());
    REXLOG_INFO("[modkit-model] applied {} variant at {:08X} ({} bytes)", edited->owner, address,
                size);
  } catch (const std::exception& error) {
    const std::string message(error.what());
    if (g_errors.size() < 256 && g_errors.insert(message).second)
      REXLOG_WARN("[modkit-model] {}", message);
  }
}

void Init(rex::Runtime* runtime, std::shared_ptr<const simpsons::modkit::ModelVariants> variants) {
  std::lock_guard lock(g_mutex);
  g_active.store(false, std::memory_order_release);
  g_runtime = runtime;
  g_variants = std::move(variants);
  g_errors.clear();
  g_active.store(runtime && g_variants && g_variants->size(), std::memory_order_release);
}
void Shutdown() {
  g_active.store(false, std::memory_order_release);
  std::lock_guard lock(g_mutex);
  g_variants.reset();
  g_runtime = nullptr;
  g_errors.clear();
}
}  // namespace modkit_models
