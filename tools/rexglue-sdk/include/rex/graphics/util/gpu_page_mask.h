#pragma once

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstdint>
#include <cstring>
#include <span>

namespace rex::graphics::draw_util {

// The upload fast path reads validity without the writer lock. Every access
// to a published validity word must be atomic, including locked updates.
// Writers still hold their common lock, so a load/store update needs no RMW.
static_assert(std::atomic_ref<uint64_t>::is_always_lock_free);
static_assert(alignof(uint64_t) >= std::atomic_ref<uint64_t>::required_alignment);
inline uint64_t LoadGpuPageValidity(uint64_t& word,
                                    std::memory_order order = std::memory_order_acquire) {
  return std::atomic_ref<uint64_t>(word).load(order);
}

inline void StoreGpuPageValidity(uint64_t& word, uint64_t value) {
  std::atomic_ref<uint64_t>(word).store(value, std::memory_order_release);
}

// The caller keeps the ownership bitmap locked for the entire snapshot.
// Validate before writing so an invalid range leaves its destination intact.
inline bool CopyGpuWrittenPages(std::span<const uint64_t> words, uint32_t first_page,
                                std::span<uint8_t> pages) {
  if (uint64_t(first_page) + pages.size() > uint64_t(words.size()) * 64)
    return false;
  for (size_t copied = 0; copied < pages.size();) {
    size_t page = size_t(first_page) + copied;
    uint32_t bit = uint32_t(page & 63);
    size_t count = std::min(size_t(64 - bit), pages.size() - copied);
    uint64_t bits = words[page >> 6] >> bit;
    uint64_t mask = UINT64_MAX >> (64 - count);
    bits &= mask;
    if (!bits || bits == mask) {
      std::fill_n(pages.data() + copied, count, uint8_t(bits != 0));
    } else {
      for (size_t i = 0; i < count; ++i)
        pages[copied + i] = uint8_t((bits >> i) & 1);
    }
    copied += count;
  }
  return true;
}

// Per-page byte flags (one byte per page, zero or not), checked eight pages at
// a time: the native resolve bookkeeping asks these of thousands of pages on
// every write that touches its buffers.
inline uint64_t LoadPageFlags8(const uint8_t* pages) {
  uint64_t word;
  std::memcpy(&word, pages, sizeof(word));
  return word;
}

inline bool AnyPageFlagSet(std::span<const uint8_t> pages) {
  const uint8_t* data = pages.data();
  size_t size = pages.size(), i = 0;
  for (; size - i >= 32; i += 32) {
    if (LoadPageFlags8(data + i) | LoadPageFlags8(data + i + 8) | LoadPageFlags8(data + i + 16) |
        LoadPageFlags8(data + i + 24))
      return true;
  }
  for (; size - i >= 8; i += 8) {
    if (LoadPageFlags8(data + i))
      return true;
  }
  for (; i < size; ++i) {
    if (data[i])
      return true;
  }
  return false;
}

inline bool AllPageFlagsSet(std::span<const uint8_t> pages) {
  constexpr uint64_t kLow = 0x0101010101010101ull, kHigh = 0x8080808080808080ull;
  const uint8_t* data = pages.data();
  size_t size = pages.size(), i = 0;
  for (; size - i >= 8; i += 8) {
    // Non-zero where any of the eight bytes is zero.
    uint64_t word = LoadPageFlags8(data + i);
    if ((word - kLow) & ~word & kHigh)
      return false;
  }
  for (; i < size; ++i) {
    if (!data[i])
      return false;
  }
  return true;
}

inline size_t CountPageFlagsSet(std::span<const uint8_t> pages) {
  size_t count = 0;
  for (uint8_t page : pages)
    count += page != 0;
  return count;
}

inline bool AnyGpuWrittenPage(std::span<const uint64_t> words, uint32_t first_page,
                              std::span<const uint8_t> valid_pages) {
  if (uint64_t(first_page) + valid_pages.size() > uint64_t(words.size()) * 64)
    return AnyPageFlagSet(valid_pages);
  for (size_t checked = 0; checked < valid_pages.size();) {
    size_t page = size_t(first_page) + checked;
    uint32_t bit = uint32_t(page & 63);
    size_t count = std::min(size_t(64 - bit), valid_pages.size() - checked);
    uint64_t bits = (words[page >> 6] >> bit) & (UINT64_MAX >> (64 - count));
    while (bits) {
      if (valid_pages[checked + std::countr_zero(bits)])
        return true;
      bits &= bits - 1;
    }
    checked += count;
  }
  return false;
}

}  // namespace rex::graphics::draw_util
