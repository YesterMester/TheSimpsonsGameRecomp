/**
 ******************************************************************************
 * ReXGlue - Xbox 360 Static Recompilation Runtime                            *
 ******************************************************************************
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#pragma once

#include <cstdint>
#include <vector>

namespace rex::memory {
class Memory;
}  // namespace rex::memory

namespace rex::graphics {

// Finds the guest physical memory pages written since they were last looked
// at, so unchanged snapshots of large buffers don't have to be hashed again.
// The kernel's asynchronous userfaultfd write protection (Linux 6.7+) lifts a
// page's protection on its first write without a signal or a handler thread,
// and PAGEMAP_SCAN reports the written pages and protects them again. Every
// host view of physical memory is watched (the guest's 64 KB, 16 MB and 4 KB
// page views and the runtime's own), as a write through one view leaves the
// others' pages protected. Used by one thread.
class GuestWriteTracker {
 public:
  GuestWriteTracker() = default;
  GuestWriteTracker(const GuestWriteTracker&) = delete;
  GuestWriteTracker& operator=(const GuestWriteTracker&) = delete;
  ~GuestWriteTracker();

  // False where it's unavailable (other systems, older kernels).
  bool Initialize(const memory::Memory& memory);

  // The generation of the newest write collected so far; writes collected
  // later get a higher one.
  uint64_t generation() const { return generation_; }

  // Collects the writes to the physical range since they were last collected,
  // and gives the generation of the newest write to it (0 if it hasn't been
  // written since tracking began). False if the range couldn't be scanned: its
  // pages then count as written, and the caller must compare the contents.
  bool LastWrite(uint32_t address, uint32_t length, uint64_t& generation_out);

  // Whether the pages of the physical range written after the generation
  // `since` (collected by LastWrite) still hold the bytes of `snapshot`, the
  // range's contents when it matched then. `source` is the range in memory.
  bool WrittenPagesMatch(uint32_t address, uint32_t length, uint64_t since, const void* source,
                         const void* snapshot) const;

  // Scans (one per view) and the nanoseconds they took since the last call,
  // for statistics.
  void TakeScanStats(uint64_t& scans_out, uint64_t& scan_ns_out) {
    scans_out = scans_;
    scan_ns_out = scan_ns_;
    scans_ = 0;
    scan_ns_ = 0;
  }

 private:
  struct View {
    // Where physical address 0 would be in this view.
    uint8_t* physical_0;
    // The physical range the view maps.
    uint32_t first, end;
  };
  bool ScanView(const View& view, uint32_t first, uint32_t end, uint64_t generation);
  // Registers and protects the range again, for views replaced since.
  void Rearm(uint32_t first, uint32_t end);

  int uffd_ = -1;
  int pagemap_ = -1;
  std::vector<View> views_;
  // Per physical page, the generation of its newest collected write.
  std::vector<uint64_t> page_generations_;
  uint64_t generation_ = 0;
  uint64_t scans_ = 0;
  uint64_t scan_ns_ = 0;
};

}  // namespace rex::graphics
