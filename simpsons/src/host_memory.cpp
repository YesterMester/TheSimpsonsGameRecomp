// The guest memory the recompilation's own code uses (host_memory.h): one
// block of virtual memory and one of physical memory, handed out in order.

#include "host_memory.h"

#include <algorithm>
#include <mutex>

#include <rex/logging.h>
#include <rex/system/xmemory.h>

namespace host_memory {
namespace {

constexpr uint32_t kVirtualSize = 1024 * 1024;
constexpr uint32_t kPhysicalSize = 4 * 1024 * 1024;

struct Block {
  uint32_t address = 0, size = 0, used = 0;
};
std::mutex g_mutex;
Block g_virtual, g_physical;
rex::memory::Memory* g_memory = nullptr;
std::vector<Block> g_blocks;

uint32_t Take(Block& block, uint32_t size, uint32_t alignment) {
  std::lock_guard lock(g_mutex);
  if (!block.address || !size)
    return 0;
  alignment = std::max<uint32_t>(alignment, 16);
  const uint32_t at = (block.used + alignment - 1) / alignment * alignment;
  if (at > block.size || size > block.size - at) {
    REXLOG_ERROR("[host-memory] out of room for {} bytes", size);
    return 0;
  }
  block.used = at + size;
  return block.address + at;
}

}  // namespace

void Init(rex::memory::Memory* memory) {
  std::lock_guard lock(g_mutex);
  g_memory = memory;
  g_blocks.clear();
  g_virtual = {memory->SystemHeapAlloc(kVirtualSize, 64 * 1024), kVirtualSize};
  g_physical = {memory->SystemHeapAlloc(kPhysicalSize, 64 * 1024,
                                        rex::memory::kSystemHeapPhysical),
                kPhysicalSize};
  REXLOG_INFO("[host-memory] {:08X} ({} KB) and physical {:08X} ({} KB)", g_virtual.address,
              kVirtualSize / 1024, g_physical.address, kPhysicalSize / 1024);
}

uint32_t Alloc(uint32_t size, uint32_t alignment) {
  return Take(g_virtual, size, alignment);
}
uint32_t AllocBlock(uint32_t size) {
  std::lock_guard lock(g_mutex);
  if (!g_memory || !size) return 0;
  const auto address = g_memory->SystemHeapAlloc(size, 64 * 1024);
  if (address) g_blocks.push_back({address, size, size});
  return address;
}

uint32_t AllocPhysical(uint32_t size) {
  return Take(g_physical, (size + 4095) / 4096 * 4096, 4096);
}

std::vector<Region> Regions() {
  std::lock_guard lock(g_mutex);
  std::vector<Region> regions;
  if (g_virtual.address)
    regions.push_back({g_virtual.address, g_virtual.size});
  if (g_physical.address)
    regions.push_back({g_physical.address, g_physical.size});
  for (const auto& block : g_blocks) regions.push_back({block.address, block.size});
  return regions;
}

}  // namespace host_memory
