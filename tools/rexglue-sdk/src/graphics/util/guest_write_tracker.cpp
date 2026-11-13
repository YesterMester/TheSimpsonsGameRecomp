/**
 ******************************************************************************
 * ReXGlue - Xbox 360 Static Recompilation Runtime                            *
 ******************************************************************************
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <rex/graphics/util/guest_write_tracker.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iterator>

#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/platform.h>
#include <rex/system/xmemory.h>

// Not on Android: its kernels lack the asynchronous mode, and older Android
// versions kill apps for the system call.
#if REX_PLATFORM_GNU_LINUX
#include <fcntl.h>
#include <linux/fs.h>
#include <linux/userfaultfd.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#if defined(PAGEMAP_SCAN) && defined(UFFD_FEATURE_WP_ASYNC) && defined(UFFD_USER_MODE_ONLY)
#define REX_GUEST_WRITE_TRACKING 1
#endif
#endif

namespace rex::graphics {

namespace {
constexpr uint32_t kPhysicalSize = 0x20000000;
constexpr uint32_t kPageShift = 12;
}  // namespace

#if REX_GUEST_WRITE_TRACKING

GuestWriteTracker::~GuestWriteTracker() {
  // Closing the descriptor unregisters the views and lifts the protection.
  if (uffd_ >= 0) {
    close(uffd_);
  }
  if (pagemap_ >= 0) {
    close(pagemap_);
  }
}

bool GuestWriteTracker::Initialize(const memory::Memory& memory) {
  if (memory::page_size() != (size_t(1) << kPageShift)) {
    return false;
  }
  // The guest's physical views start at 0xA0000000 (64 KB pages), 0xC0000000
  // (16 MB pages) and 0xE0000000 (4 KB pages, at physical 0x1000).
  views_ = {
      {memory.TranslateVirtual(0xA0000000), 0, kPhysicalSize},
      {memory.TranslateVirtual(0xC0000000), 0, kPhysicalSize},
      {memory.TranslateVirtual(0xE0000000) - 0x1000, 0x1000, kPhysicalSize},
      {memory.TranslatePhysical(0), 0, kPhysicalSize},
  };
  // Only user mode faults are reported to a handler, which is all an
  // unprivileged process may ask for; the asynchronous mode has no handler.
  uffd_ = int(syscall(SYS_userfaultfd, O_CLOEXEC | O_NONBLOCK | UFFD_USER_MODE_ONLY));
  if (uffd_ < 0) {
    REXGPU_INFO("Guest write tracking unavailable: no userfaultfd ({})", errno);
    return false;
  }
  uffdio_api api = {};
  api.api = UFFD_API;
  api.features =
      UFFD_FEATURE_WP_ASYNC | UFFD_FEATURE_WP_HUGETLBFS_SHMEM | UFFD_FEATURE_WP_UNPOPULATED;
  pagemap_ = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
  if (ioctl(uffd_, UFFDIO_API, &api) || pagemap_ < 0) {
    REXGPU_INFO("Guest write tracking unavailable: the kernel has no asynchronous write protection");
    close(uffd_);
    uffd_ = -1;
    return false;
  }
  page_generations_.assign(kPhysicalSize >> kPageShift, 0);
  for (const View& view : views_) {
    uffdio_register registration = {};
    registration.range.start = uint64_t(view.physical_0 + view.first);
    registration.range.len = view.end - view.first;
    registration.mode = UFFDIO_REGISTER_MODE_WP;
    uffdio_writeprotect protect = {};
    protect.range = registration.range;
    protect.mode = UFFDIO_WRITEPROTECT_MODE_WP;
    if (ioctl(uffd_, UFFDIO_REGISTER, &registration) ||
        ioctl(uffd_, UFFDIO_WRITEPROTECT, &protect)) {
      REXGPU_WARN("Guest write tracking unavailable: physical memory can't be protected ({})",
                  errno);
      close(uffd_);
      uffd_ = -1;
      return false;
    }
  }
  generation_ = 1;
  REXGPU_INFO("Guest write tracking: large unchanged buffers are no longer hashed");
  return true;
}

bool GuestWriteTracker::ScanView(const View& view, uint32_t first, uint32_t end,
                                 uint64_t generation) {
  first = std::max(first, view.first);
  end = std::min(end, view.end);
  if (first >= end) {
    return true;
  }
  page_region regions[32];
  pm_scan_arg scan = {};
  scan.size = sizeof(scan);
  // Report the written pages and protect them again; fail on a view no longer
  // protected asynchronously (replaced by a new mapping).
  scan.flags = PM_SCAN_WP_MATCHING | PM_SCAN_CHECK_WPASYNC;
  scan.start = uint64_t(view.physical_0 + first);
  scan.end = uint64_t(view.physical_0 + end);
  scan.vec = uint64_t(regions);
  scan.vec_len = std::size(regions);
  scan.category_mask = PAGE_IS_WRITTEN;
  scan.return_mask = PAGE_IS_WRITTEN;
  while (scan.start < scan.end) {
    long count = ioctl(pagemap_, PAGEMAP_SCAN, &scan);
    if (count < 0) {
      return false;
    }
    for (long i = 0; i < count; ++i) {
      uint64_t region_first = (regions[i].start - uint64_t(view.physical_0)) >> kPageShift;
      uint64_t region_end = (regions[i].end - uint64_t(view.physical_0)) >> kPageShift;
      std::fill(page_generations_.begin() + region_first, page_generations_.begin() + region_end,
                generation);
    }
    // The walk stops early only when the regions are full.
    if (scan.walk_end <= scan.start) {
      return false;
    }
    scan.start = scan.walk_end;
  }
  return true;
}

void GuestWriteTracker::Rearm(uint32_t first, uint32_t end) {
  for (const View& view : views_) {
    uint32_t view_first = std::max(first, view.first);
    uint32_t view_end = std::min(end, view.end);
    if (view_first >= view_end) {
      continue;
    }
    uffdio_register registration = {};
    registration.range.start = uint64_t(view.physical_0 + view_first);
    registration.range.len = view_end - view_first;
    registration.mode = UFFDIO_REGISTER_MODE_WP;
    uffdio_writeprotect protect = {};
    protect.range = registration.range;
    protect.mode = UFFDIO_WRITEPROTECT_MODE_WP;
    ioctl(uffd_, UFFDIO_REGISTER, &registration);
    ioctl(uffd_, UFFDIO_WRITEPROTECT, &protect);
  }
}

bool GuestWriteTracker::LastWrite(uint32_t address, uint32_t length, uint64_t& generation_out) {
  generation_out = generation_;
  if (uffd_ < 0 || !length) {
    return false;
  }
  address &= kPhysicalSize - 1;
  uint32_t first = address & ~((uint32_t(1) << kPageShift) - 1);
  uint32_t end = uint32_t(std::min(
      (uint64_t(address) + length + (uint64_t(1) << kPageShift) - 1) &
          ~((uint64_t(1) << kPageShift) - 1),
      uint64_t(kPhysicalSize)));
  uint64_t generation = generation_ + 1;
  bool scanned = true;
  auto scan_start = std::chrono::steady_clock::now();
  for (const View& view : views_) {
    if (!ScanView(view, first, end, generation)) {
      scanned = false;
    }
  }
  scans_ += views_.size();
  scan_ns_ += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                           std::chrono::steady_clock::now() - scan_start)
                           .count());
  if (!scanned) {
    // A failed scan may already have protected written pages again: count the
    // whole range as written, and protect it again for the next checks.
    std::fill(page_generations_.begin() + (first >> kPageShift),
              page_generations_.begin() + (end >> kPageShift), generation);
    Rearm(first, end);
  }
  uint64_t newest = 0;
  for (uint32_t page = first >> kPageShift; page < (end >> kPageShift); ++page) {
    newest = std::max(newest, page_generations_[page]);
  }
  if (newest == generation) {
    generation_ = generation;
  }
  generation_out = newest;
  return scanned;
}

bool GuestWriteTracker::WrittenPagesMatch(uint32_t address, uint32_t length, uint64_t since,
                                          const void* source, const void* snapshot) const {
  if (uffd_ < 0) {
    return false;
  }
  address &= kPhysicalSize - 1;
  const uint64_t end = std::min(uint64_t(address) + length, uint64_t(kPhysicalSize));
  const uint64_t page_size = uint64_t(1) << kPageShift;
  for (uint64_t page = address & ~(page_size - 1); page < end; page += page_size) {
    if (page_generations_[page >> kPageShift] <= since) {
      continue;
    }
    uint64_t first = std::max(page, uint64_t(address));
    uint64_t last = std::min(page + page_size, end);
    if (std::memcmp(static_cast<const uint8_t*>(source) + (first - address),
                    static_cast<const uint8_t*>(snapshot) + (first - address), last - first)) {
      return false;
    }
  }
  return true;
}

#else  // REX_GUEST_WRITE_TRACKING

GuestWriteTracker::~GuestWriteTracker() = default;

bool GuestWriteTracker::Initialize(const memory::Memory&) {
  return false;
}

bool GuestWriteTracker::ScanView(const View&, uint32_t, uint32_t, uint64_t) {
  return false;
}

void GuestWriteTracker::Rearm(uint32_t, uint32_t) {}

bool GuestWriteTracker::LastWrite(uint32_t, uint32_t, uint64_t& generation_out) {
  generation_out = generation_;
  return false;
}

bool GuestWriteTracker::WrittenPagesMatch(uint32_t, uint32_t, uint64_t, const void*,
                                          const void*) const {
  return false;
}

#endif  // REX_GUEST_WRITE_TRACKING

}  // namespace rex::graphics
