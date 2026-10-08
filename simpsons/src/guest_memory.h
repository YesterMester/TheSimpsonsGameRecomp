#pragma once

#include <bit>
#include <cstdint>
#include <cstring>

#include <rex/platform.h>

namespace simpsons {

// Match REX_PHYS_HOST_OFFSET in the generated code. Windows reserves an
// extra page before the physical heaps; ordinary virtual addresses do not.
inline const uint8_t* GuestToHost(const uint8_t* base, uint32_t address) {
#if REX_PLATFORM_WIN32
  return base + address + (address >= 0xE0000000u ? 0x1000u : 0u);
#else
  return base + address;
#endif
}

inline uint8_t* GuestToHost(uint8_t* base, uint32_t address) {
  return const_cast<uint8_t*>(GuestToHost(static_cast<const uint8_t*>(base), address));
}

inline uint32_t LoadGuestU32(const uint8_t* base, uint32_t address) {
  uint32_t value;
  std::memcpy(&value, GuestToHost(base, address), sizeof(value));
  return std::byteswap(value);
}

inline void StoreGuestU32(uint8_t* base, uint32_t address, uint32_t value) {
  value = std::byteswap(value);
  std::memcpy(GuestToHost(base, address), &value, sizeof(value));
}

inline float LoadGuestFloat(const uint8_t* base, uint32_t address) {
  return std::bit_cast<float>(LoadGuestU32(base, address));
}

inline void StoreGuestFloat(uint8_t* base, uint32_t address, float value) {
  StoreGuestU32(base, address, std::bit_cast<uint32_t>(value));
}

}  // namespace simpsons
