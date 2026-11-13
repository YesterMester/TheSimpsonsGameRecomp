#pragma once

#include <cstdint>
#include <vector>

namespace rex::memory {
class Memory;
}

// Guest memory the recompilation's own code uses: scratch for calls into the
// game, vertex buffers, textures. It is set aside once at startup, before the
// game runs, so it sits at the same addresses in every run, and exact saves
// leave it as it is (snapshot.cpp): a save only goes back under a game whose
// memory set aside here is where the save's was.
namespace host_memory {

void Init(rex::memory::Memory* memory);
// Zeroed and never freed; 0 when there's no room left.
uint32_t Alloc(uint32_t size, uint32_t alignment = 16);
// A separately sized region for immutable native resources. The runtime owns
// its lifetime; exact saves treat it like the other native resource regions.
uint32_t AllocBlock(uint32_t size);
// Physical memory the GPU can read, in whole pages.
uint32_t AllocPhysical(uint32_t size);

struct Region {
  uint32_t address, size;  // guest virtual
};
// The memory set aside.
std::vector<Region> Regions();

}  // namespace host_memory
