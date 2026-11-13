#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#endif

// Reserve guest virtual addresses without committing gigabytes of RAM. Tests
// commit only their low-memory fixture and the explicitly requested XEX pages.
// Nothing here loads a game, a runtime or game assets.
class GuestMemoryFixture {
 public:
  explicit GuestMemoryFixture(size_t low_size) : low_size_(low_size) {
#if defined(_WIN32)
    base_ = static_cast<uint8_t*>(VirtualAlloc(nullptr, kSize, MEM_RESERVE, PAGE_NOACCESS));
#else
    void* value =
        mmap(nullptr, kSize, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    base_ = value == MAP_FAILED ? nullptr : static_cast<uint8_t*>(value);
#endif
    if (!base_)
      throw std::runtime_error("Cannot reserve the fixture guest address space");
    Commit(0, low_size_);
  }
  ~GuestMemoryFixture() {
    if (!base_)
      return;
#if defined(_WIN32)
    VirtualFree(base_, 0, MEM_RELEASE);
#else
    munmap(base_, kSize);
#endif
  }
  GuestMemoryFixture(const GuestMemoryFixture&) = delete;
  GuestMemoryFixture& operator=(const GuestMemoryFixture&) = delete;
  uint8_t* data() { return base_; }
  const uint8_t* data() const { return base_; }
  size_t low_size() const { return low_size_; }
  void Commit(uint32_t address, size_t size) {
    const uint64_t first = uint64_t(address) & ~uint64_t(4095);
    const uint64_t end = (uint64_t(address) + size + 4095) & ~uint64_t(4095);
    if (end > kSize)
      throw std::runtime_error("Fixture commit crosses its address space");
#if defined(_WIN32)
    if (!VirtualAlloc(base_ + first, size_t(end - first), MEM_COMMIT, PAGE_READWRITE))
#else
    if (mprotect(base_ + first, size_t(end - first), PROT_READ | PROT_WRITE))
#endif
      throw std::runtime_error("Cannot commit a fixture guest memory page");
  }

 private:
  static constexpr size_t kSize = size_t(0x100001000ull);
  uint8_t* base_ = nullptr;
  size_t low_size_;
};
