/**
 ******************************************************************************
 * ReXGlue - Xbox 360 Static Recompilation Runtime                            *
 ******************************************************************************
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>

#include <rex/graphics/shared_memory.h>

namespace rex::graphics {

// One-shot write watches for an immutable CPU snapshot. Guest writes and
// watched GPU writes make future reads compare the bytes again. Direct host
// writes must invalidate the owner explicitly. Never rearm changing streams:
// their ordinary fresh snapshots avoid repeated guest write faults.
class NativeBufferWatch {
 public:
  explicit NativeBufferWatch(SharedMemory& memory) : memory_(memory) {}
  NativeBufferWatch(const NativeBufferWatch&) = delete;
  NativeBufferWatch& operator=(const NativeBufferWatch&) = delete;

  ~NativeBufferWatch() {
    auto global_lock = critical_region_.Acquire();
    for (auto handle : handles_) {
      if (handle) {
        memory_.UnwatchMemoryRange(handle);
      }
    }
  }

  void AddRange(uint32_t address, uint32_t length, const void* snapshot, const void* source) {
    auto global_lock = critical_region_.Acquire();
    size_t index = handles_.size();
    auto handle = memory_.WatchMemoryRange(
        address, length,
        [](const std::unique_lock<std::recursive_mutex>&, void*, void* data, uint64_t index, bool) {
          auto& watch = *static_cast<NativeBufferWatch*>(data);
          watch.current_.store(false, std::memory_order_release);
          watch.handles_[index] = nullptr;
        },
        nullptr, this, index);
    handles_.push_back(handle);
    // Protection and comparison share the invalidation lock. A write between
    // taking the snapshot and arming its watch must never validate old bytes.
    if (!handle || !memory_.WatchCpuMemoryRange(address, length) ||
        std::memcmp(snapshot, source, length)) {
      current_.store(false, std::memory_order_release);
    }
  }

  bool IsCurrent() const { return current_.load(std::memory_order_acquire); }

 private:
  SharedMemory& memory_;
  rex::thread::global_critical_region critical_region_;
  std::vector<SharedMemory::WatchHandle> handles_;
  std::atomic<bool> current_{true};
};

}  // namespace rex::graphics
